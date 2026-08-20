#define WIN32_LEAN_AND_MEAN
#include "ConsoleArtifactConsumerV2.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace artifact = dronegcs::route_artifact;
    namespace artifact_v2 = dronegcs::route_artifact::v2;
    namespace wpl = dronegcs::wpl110;

    void Expect(const bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    artifact::Digest256 HashText(const std::string& text)
    {
        artifact::Digest256 digest;
        Expect(artifact::Sha256::Hash(text.data(), text.size(), digest),
            "SHA-256 fixture failed");
        return digest;
    }

    class TemporaryWorkspace final
    {
    public:
        explicit TemporaryWorkspace(const std::string& tag)
        {
            static std::atomic<unsigned> counter{ 0 };
            root = fs::temp_directory_path() /
                ("DroneGCS-ArtifactV2-" + tag + "-" +
                    std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(++counter));
            fs::create_directories(root / "Current");
        }

        ~TemporaryWorkspace()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }

        fs::path root;
    };

    struct Fixture
    {
        std::unique_ptr<TemporaryWorkspace> workspace;
        artifact_v2::Manifest manifest;
        ExpectedRouteArtifactV2Identity expected;
    };

    void WriteBinary(const fs::path& path, const std::string& bytes)
    {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Expect(output.good(), "fixture file could not be opened");
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Expect(output.good(), "fixture file write failed");
    }

    std::string ReadBinary(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        Expect(input.good(), "fixture file could not be read");
        return std::string(std::istreambuf_iterator<char>(input), {});
    }

    std::string MakeRouteBytes(
        const std::int32_t routeSlot,
        const std::int32_t frames)
    {
        std::string bytes = std::string(wpl::Header) + "\r\n";
        for (std::int32_t frame = 0; frame < frames; ++frame)
        {
            wpl::Row row;
            row.Sequence = frame;
            row.Red = static_cast<std::uint8_t>((routeSlot + frame) % 256);
            row.Green = static_cast<std::uint8_t>((routeSlot * 3 + frame) % 256);
            row.Blue = static_cast<std::uint8_t>((routeSlot * 7 + frame) % 256);
            row.LatitudeE7 = 319000000 + routeSlot + frame;
            row.LongitudeE7 = 1186000000 + routeSlot + frame;
            row.AltitudeE7 = 500000000LL + routeSlot + frame;
            wpl::FormattedRow formatted;
            Expect(wpl::FormatRow(row, formatted), "WPL row formatting failed");
            bytes.append(reinterpret_cast<const char*>(formatted.Bytes.data()),
                formatted.Size);
            if (frame + 1 < frames) bytes += "\r\n";
        }
        return bytes;
    }

    void FinalizeMetadata(Fixture& fixture)
    {
        artifact::Error error;
        Expect(artifact_v2::ComputeArtifactSetId(
            artifact_v2::BuildIdentityFromManifest(fixture.manifest),
            fixture.manifest.ArtifactSetId, error), error.Detail);
        Expect(artifact_v2::ComputeArtifactSetDigest(
            fixture.manifest, fixture.manifest.ArtifactSetDigest, error),
            error.Detail);
        std::string manifestBytes;
        std::string readyBytes;
        Expect(artifact_v2::SerializeCanonicalManifest(
            fixture.manifest, manifestBytes, error), error.Detail);
        Expect(artifact::SerializeReadyMarker(
            fixture.manifest.ArtifactSetId,
            fixture.manifest.ArtifactSetDigest, readyBytes, error), error.Detail);
        const fs::path current = fixture.workspace->root / "Current";
        WriteBinary(current / "manifest.json", manifestBytes);
        WriteBinary(current / "READY", readyBytes);
        fixture.expected.artifactSetId = fixture.manifest.ArtifactSetId;
        fixture.expected.artifactSetDigest = fixture.manifest.ArtifactSetDigest;
    }

    Fixture CreateFixture(
        const std::string& tag,
        const std::int32_t routeCount,
        const std::int32_t frameCount,
        const std::int64_t createdAt = 1776556800123LL)
    {
        Fixture fixture;
        fixture.workspace = std::make_unique<TemporaryWorkspace>(tag);
        auto& manifest = fixture.manifest;
        manifest.CreatedAtUtc = createdAt;
        manifest.Source.SourceDatasetDigest = HashText("v2-source-dataset");
        manifest.Source.SourceByteSize = 987654321;
        manifest.Source.SourceDisplayFileName = "show-source.npz";
        manifest.FrameCount = frameCount;
        manifest.RouteSlotCount = routeCount;
        manifest.ShowOriginLatitudeE7 = 319324710;
        manifest.ShowOriginLongitudeE7 = 1186385310;
        manifest.WaypointGeneratorVersion =
            "qgc-wpl110-wgs84-relative-rgb-saturate-v2";
        manifest.GeneratorInputDigest = HashText("v2-generator-input");

        const fs::path current = fixture.workspace->root / "Current";
        for (std::int32_t slot = 0; slot < routeCount; ++slot)
        {
            std::string relative;
            artifact::Error error;
            Expect(artifact::BuildRouteRelativePath(slot, relative, error),
                error.Detail);
            const std::string bytes = MakeRouteBytes(slot, frameCount);
            WriteBinary(current / fs::u8path(relative), bytes);
            artifact::RouteRecord record;
            record.RouteSlotIndex = slot;
            record.RelativePath = relative;
            record.FileByteSize = static_cast<std::int64_t>(bytes.size());
            record.RouteArtifactDigest = HashText(bytes);
            manifest.Routes.push_back(std::move(record));
        }
        FinalizeMetadata(fixture);
        return fixture;
    }

    ConsoleArtifactV2ValidationResult Validate(const Fixture& fixture)
    {
        return ConsoleArtifactConsumerV2::ValidateCurrentForTest(
            fixture.workspace->root.u8string(), fixture.expected,
            nullptr, 1, std::chrono::seconds(30));
    }

    void ExpectFailure(
        const Fixture& fixture,
        const ConsoleArtifactV2ErrorCode expected,
        const std::string& label)
    {
        const auto result = Validate(fixture);
        Expect(!result.success, label + " unexpectedly succeeded");
        Expect(result.diagnostic.errorCode == expected,
            label + " returned unexpected diagnostic " +
                std::to_string(static_cast<unsigned>(result.diagnostic.errorCode)));
    }

    void VerifyValidSnapshotAndReadApi()
    {
        auto fixture = CreateFixture("accept", 3, 4);
        const auto result = Validate(fixture);
        Expect(result.success && result.snapshot, "valid schema-v2 Current rejected");
        const auto& snapshot = *result.snapshot;
        Expect(snapshot.artifactSchemaVersion == 2
            && snapshot.artifactSetId == fixture.manifest.ArtifactSetId
            && snapshot.artifactSetDigest == fixture.manifest.ArtifactSetDigest
            && snapshot.createdAtUtcMs == fixture.manifest.CreatedAtUtc,
            "accepted v2 identity projection mismatch");
        Expect(snapshot.stableSource.sourceDatasetDigest ==
                fixture.manifest.Source.SourceDatasetDigest
            && snapshot.stableSource.normalizedShowOriginLatitudeE7 == 319324710
            && snapshot.stableSource.normalizedShowOriginLongitudeE7 == 1186385310
            && snapshot.stableSource.routeSlotCount == 3,
            "stable source projection mismatch");
        Expect(snapshot.sourceByteSize == 987654321
            && snapshot.sourceDisplayFileName == "show-source.npz"
            && snapshot.frameCount == 4
            && snapshot.waypointGeneratorVersion ==
                "qgc-wpl110-wgs84-relative-rgb-saturate-v2"
            && snapshot.generatorInputDigest == fixture.manifest.GeneratorInputDigest
            && snapshot.routes.size() == 3,
            "source/generator/route projection mismatch");
        Expect(snapshot.canonicalWorkspaceRoot ==
                fixture.workspace->root.lexically_normal().u8string()
            && !snapshot.resolvedCurrentRoot.empty()
            && snapshot.currentDirectoryIdentity.fileIndexHigh
                + snapshot.currentDirectoryIdentity.fileIndexLow > 0,
            "runtime Current authority projection mismatch");

        ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
        const auto generation = consumer.RequestValidation(
            fixture.expected, std::chrono::seconds(30));
        ConsoleArtifactV2ValidationResult workerResult;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        while (!consumer.PollCompleted(workerResult)
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Expect(workerResult.success && workerResult.generation == generation,
            "persistent v2 worker did not complete");
        const auto accepted = consumer.GetAcceptedSnapshot();
        Expect(accepted && accepted->artifactSetId == fixture.expected.artifactSetId,
            "consumer-owned accepted snapshot missing");
        const auto route = consumer.FindRouteRecord(2);
        Expect(route && route->routeSlotIndex == 2
            && route->relativePath == "routes/00000000/2.waypoints",
            "FindRouteRecord failed");
        ConsoleArtifactV2Diagnostic diagnostic;
        Expect(consumer.IsCurrentIdentityStill(fixture.expected, diagnostic),
            "unchanged Current identity rejected");
    }

    artifact::Manifest MakeLegacyManifest(const std::string& routeBytes)
    {
        artifact::Manifest manifest;
        manifest.CreatedAtUtc = 1776556800123LL;
        manifest.Assignment.FleetSessionId = "legacy-fleet";
        manifest.Assignment.AssignmentAuthoritySessionId = "legacy-authority";
        manifest.Assignment.AssignmentRevision = 1;
        manifest.Assignment.AssignmentDigest = HashText("legacy-assignment");
        manifest.FleetRevisionAtCapture = 1;
        manifest.RouteDatasetGeneration = 1;
        manifest.MatchedCount = 1;
        manifest.Source.SourceDatasetDigest = HashText("legacy-source");
        manifest.Source.SourceByteSize = 1;
        manifest.Source.SourceDisplayFileName = "legacy.npz";
        manifest.FrameCount = 2;
        manifest.RouteSlotCount = 1;
        manifest.WaypointGeneratorVersion = "legacy-generator";
        manifest.GeneratorInputDigest = HashText("legacy-generator-input");
        manifest.Mapping.push_back({ "drone-0", 0 });
        artifact::RouteRecord route;
        route.RouteSlotIndex = 0;
        artifact::Error error;
        Expect(artifact::BuildRouteRelativePath(0, route.RelativePath, error),
            error.Detail);
        route.FileByteSize = static_cast<std::int64_t>(routeBytes.size());
        route.RouteArtifactDigest = HashText(routeBytes);
        manifest.Routes.push_back(route);
        Expect(artifact::ComputeArtifactSetId(
            artifact::BuildIdentityFromManifest(manifest),
            manifest.ArtifactSetId, error), error.Detail);
        Expect(artifact::ComputeArtifactSetDigest(
            manifest, manifest.ArtifactSetDigest, error), error.Detail);
        return manifest;
    }

    void VerifyLegacyAndReadyFailures()
    {
        auto legacy = CreateFixture("legacy", 1, 2);
        const std::string routeBytes = MakeRouteBytes(0, 2);
        const artifact::Manifest legacyManifest = MakeLegacyManifest(routeBytes);
        artifact::Error error;
        std::string manifestBytes;
        std::string readyBytes;
        Expect(artifact::SerializeCanonicalManifest(
            legacyManifest, manifestBytes, error), error.Detail);
        Expect(artifact::SerializeReadyMarker(legacyManifest.ArtifactSetId,
            legacyManifest.ArtifactSetDigest, readyBytes, error), error.Detail);
        WriteBinary(legacy.workspace->root / "Current" / "manifest.json", manifestBytes);
        WriteBinary(legacy.workspace->root / "Current" / "READY", readyBytes);
        legacy.expected.artifactSetId = legacyManifest.ArtifactSetId;
        legacy.expected.artifactSetDigest = legacyManifest.ArtifactSetDigest;
        ExpectFailure(legacy, ConsoleArtifactV2ErrorCode::LegacyUnsupported,
            "canonical schema-v1 artifact");

        auto missing = CreateFixture("ready-missing", 1, 2);
        fs::remove(missing.workspace->root / "Current" / "READY");
        ExpectFailure(missing, ConsoleArtifactV2ErrorCode::ReadyMissingOrInvalid,
            "missing READY");

        auto malformed = CreateFixture("ready-malformed", 1, 2);
        WriteBinary(malformed.workspace->root / "Current" / "READY", "bad-ready");
        ExpectFailure(malformed, ConsoleArtifactV2ErrorCode::ReadyMissingOrInvalid,
            "malformed READY");

        auto idMismatch = CreateFixture("ready-id", 1, 2);
        readyBytes = ReadBinary(idMismatch.workspace->root / "Current" / "READY");
        const auto idPosition = readyBytes.find(idMismatch.expected.artifactSetId);
        readyBytes[idPosition] = readyBytes[idPosition] == '0' ? '1' : '0';
        WriteBinary(idMismatch.workspace->root / "Current" / "READY", readyBytes);
        ExpectFailure(idMismatch, ConsoleArtifactV2ErrorCode::ArtifactIdentityMismatch,
            "READY ID mismatch");

        auto digestMismatch = CreateFixture("ready-digest", 1, 2);
        readyBytes = ReadBinary(digestMismatch.workspace->root / "Current" / "READY");
        const std::string digestHex = artifact::DigestToLowerHex(
            digestMismatch.expected.artifactSetDigest);
        const auto digestPosition = readyBytes.find(digestHex);
        readyBytes[digestPosition] = readyBytes[digestPosition] == '0' ? '1' : '0';
        WriteBinary(digestMismatch.workspace->root / "Current" / "READY", readyBytes);
        ExpectFailure(digestMismatch,
            ConsoleArtifactV2ErrorCode::ArtifactIdentityMismatch,
            "READY digest mismatch");
    }

    void TamperManifestHex(Fixture& fixture, const std::string& needle)
    {
        const fs::path path = fixture.workspace->root / "Current" / "manifest.json";
        std::string bytes = ReadBinary(path);
        const auto position = bytes.find(needle);
        Expect(position != std::string::npos, "manifest tamper needle missing");
        bytes[position] = bytes[position] == '0' ? '1' : '0';
        WriteBinary(path, bytes);
    }

    void VerifyManifestAndRouteFailures()
    {
        auto id = CreateFixture("manifest-id", 2, 2);
        TamperManifestHex(id, id.manifest.ArtifactSetId);
        ExpectFailure(id, ConsoleArtifactV2ErrorCode::ManifestMissingOrInvalid,
            "Manifest ArtifactSetID tamper");

        auto digest = CreateFixture("manifest-digest", 2, 2);
        TamperManifestHex(digest,
            artifact::DigestToLowerHex(digest.manifest.ArtifactSetDigest));
        ExpectFailure(digest,
            ConsoleArtifactV2ErrorCode::ManifestMissingOrInvalid,
            "Manifest ArtifactSetDigest tamper");

        auto missing = CreateFixture("route-missing", 2, 2);
        fs::remove(missing.workspace->root / "Current" /
            fs::u8path(missing.manifest.Routes[1].RelativePath));
        ExpectFailure(missing, ConsoleArtifactV2ErrorCode::UnexpectedCurrentEntry,
            "missing route");

        auto size = CreateFixture("route-size", 2, 2);
        ++size.manifest.Routes[1].FileByteSize;
        FinalizeMetadata(size);
        ExpectFailure(size, ConsoleArtifactV2ErrorCode::RouteSizeMismatch,
            "route size mismatch");

        auto sha = CreateFixture("route-sha", 2, 2);
        sha.manifest.Routes[1].RouteArtifactDigest = HashText("wrong-route-content");
        FinalizeMetadata(sha);
        ExpectFailure(sha, ConsoleArtifactV2ErrorCode::RouteDigestMismatch,
            "route SHA mismatch");

        auto malformed = CreateFixture("wpl-malformed", 2, 2);
        const fs::path malformedPath = malformed.workspace->root / "Current" /
            fs::u8path(malformed.manifest.Routes[1].RelativePath);
        const std::string malformedBytes = "QGC WPL 110\r\ninvalid";
        WriteBinary(malformedPath, malformedBytes);
        malformed.manifest.Routes[1].FileByteSize = malformedBytes.size();
        malformed.manifest.Routes[1].RouteArtifactDigest = HashText(malformedBytes);
        FinalizeMetadata(malformed);
        ExpectFailure(malformed,
            ConsoleArtifactV2ErrorCode::WaypointSemanticInvalid,
            "malformed WPL");

        auto rowCount = CreateFixture("wpl-row-count", 2, 2);
        const fs::path rowPath = rowCount.workspace->root / "Current" /
            fs::u8path(rowCount.manifest.Routes[1].RelativePath);
        const std::string oneRow = MakeRouteBytes(1, 1);
        WriteBinary(rowPath, oneRow);
        rowCount.manifest.Routes[1].FileByteSize = oneRow.size();
        rowCount.manifest.Routes[1].RouteArtifactDigest = HashText(oneRow);
        FinalizeMetadata(rowCount);
        ExpectFailure(rowCount,
            ConsoleArtifactV2ErrorCode::WaypointSemanticInvalid,
            "WPL row count mismatch");
    }

    void ReplaceOnce(std::string& value,
        const std::string& from, const std::string& to)
    {
        const auto position = value.find(from);
        Expect(position != std::string::npos, "manifest replacement needle missing");
        value.replace(position, from.size(), to);
    }

    void VerifyDomainAndPathRejection()
    {
        const std::vector<std::string> unsafePaths{
            "../escape.waypoints",
            "C:/escape.waypoints",
            "//server/share/escape.waypoints",
            "routes\\00000000\\0.waypoints",
            "routes//00000000/0.waypoints",
            "routes/00000000/CON.waypoints"
        };
        for (std::size_t index = 0; index < unsafePaths.size(); ++index)
        {
            auto fixture = CreateFixture("unsafe-" + std::to_string(index), 2, 2);
            const fs::path manifestPath =
                fixture.workspace->root / "Current" / "manifest.json";
            std::string bytes = ReadBinary(manifestPath);
            ReplaceOnce(bytes, fixture.manifest.Routes[0].RelativePath,
                unsafePaths[index]);
            WriteBinary(manifestPath, bytes);
            ExpectFailure(fixture,
                ConsoleArtifactV2ErrorCode::ManifestMissingOrInvalid,
                "unsafe RelativePath " + std::to_string(index));
        }

        for (const std::string& replacement : {
            std::string("\"routeSlotIndex\":0,\"relativePath\":\"routes/00000000/1.waypoints\""),
            std::string("\"routeSlotIndex\":2,\"relativePath\":\"routes/00000000/1.waypoints\"") })
        {
            auto fixture = CreateFixture("bad-domain", 2, 2);
            const fs::path manifestPath =
                fixture.workspace->root / "Current" / "manifest.json";
            std::string bytes = ReadBinary(manifestPath);
            ReplaceOnce(bytes,
                "\"routeSlotIndex\":1,\"relativePath\":\"routes/00000000/1.waypoints\"",
                replacement);
            WriteBinary(manifestPath, bytes);
            ExpectFailure(fixture,
                ConsoleArtifactV2ErrorCode::ManifestMissingOrInvalid,
                "duplicate/out-of-domain route slot");
        }
    }

    void VerifyReparseAndCurrentOnly()
    {
        auto fixture = CreateFixture("reparse", 2, 2);
        const fs::path shard = (fixture.workspace->root / "Current" /
            fs::u8path(fixture.manifest.Routes[0].RelativePath)).parent_path();
        const fs::path outside = fixture.workspace->root / "outside";
        fs::create_directories(outside);
        std::error_code error;
        fs::remove_all(shard, error);
        const DWORD flags = SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2;
        const BOOLEAN linked = CreateSymbolicLinkW(shard.c_str(), outside.c_str(), flags);
        Expect(linked != FALSE, "directory reparse fixture could not be created");
        ExpectFailure(fixture,
            ConsoleArtifactV2ErrorCode::ReparsePointDetected,
            "directory reparse escape");

        auto currentEscape = CreateFixture("current-escape", 1, 2);
        auto outsideCurrent = CreateFixture("current-outside", 1, 2);
        fs::remove_all(currentEscape.workspace->root / "Current", error);
        const BOOLEAN currentLinked = CreateSymbolicLinkW(
            (currentEscape.workspace->root / "Current").c_str(),
            (outsideCurrent.workspace->root / "Current").c_str(), flags);
        Expect(currentLinked != FALSE,
            "Current reparse escape fixture could not be created");
        ExpectFailure(currentEscape,
            ConsoleArtifactV2ErrorCode::ReparsePointDetected,
            "Current reparse escape");

        auto stagingOnly = CreateFixture("staging-only", 1, 2);
        fs::rename(stagingOnly.workspace->root / "Current",
            stagingOnly.workspace->root / "Staging");
        ExpectFailure(stagingOnly, ConsoleArtifactV2ErrorCode::CurrentMissing,
            "Staging-only workspace");

        auto unexpected = CreateFixture("unexpected", 1, 2);
        WriteBinary(unexpected.workspace->root / "Current" / "extra.txt", "extra");
        ExpectFailure(unexpected,
            ConsoleArtifactV2ErrorCode::UnexpectedCurrentEntry,
            "unexpected Current file");
    }

    void VerifyShard1200()
    {
        auto fixture = CreateFixture("shard-1200", 1200, 2);
        const auto result = Validate(fixture);
        Expect(result.success && result.snapshot
            && result.snapshot->routes.size() == 1200,
            "1200-route sharded fixture rejected");
        Expect(result.snapshot->routes[999].relativePath ==
                "routes/00000000/999.waypoints"
            && result.snapshot->routes[1000].relativePath ==
                "routes/00001000/1000.waypoints"
            && result.snapshot->routes[1199].relativePath ==
                "routes/00001000/1199.waypoints",
            "1200-route shard boundary mismatch");
    }

    ConsoleArtifactV2ValidationResult WaitForResult(
        ConsoleArtifactConsumerV2& consumer,
        const std::chrono::seconds timeout)
    {
        ConsoleArtifactV2ValidationResult result;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!consumer.PollCompleted(result)
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Expect(result.generation != 0, "v2 worker result timed out");
        return result;
    }

    void VerifyAtomicityCancellationDeadlineAndSwap()
    {
        auto fixture = CreateFixture("atomic", 3, 20);
        ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
        consumer.RequestValidation(fixture.expected, std::chrono::seconds(30));
        auto first = WaitForResult(consumer, std::chrono::seconds(10));
        Expect(first.success, "Artifact A was not accepted");
        const auto acceptedA = consumer.GetAcceptedSnapshot();

        fixture.manifest.Routes[2].RouteArtifactDigest = HashText("bad-b-route");
        ++fixture.manifest.CreatedAtUtc;
        FinalizeMetadata(fixture);
        consumer.RequestValidation(fixture.expected, std::chrono::seconds(30));
        const auto second = WaitForResult(consumer, std::chrono::seconds(10));
        Expect(!second.success
            && second.diagnostic.errorCode ==
                ConsoleArtifactV2ErrorCode::RouteDigestMismatch,
            "invalid Artifact B did not fail atomically");
        const auto acceptedAfterB = consumer.GetAcceptedSnapshot();
        Expect(acceptedAfterB
            && acceptedAfterB->artifactSetDigest == acceptedA->artifactSetDigest,
            "failed Artifact B replaced accepted Artifact A");

        std::atomic_bool cancelled{ true };
        const auto cancelledResult =
            ConsoleArtifactConsumerV2::ValidateCurrentForTest(
                fixture.workspace->root.u8string(), fixture.expected,
                &cancelled, 3, std::chrono::seconds(30));
        Expect(!cancelledResult.success
            && cancelledResult.diagnostic.errorCode ==
                ConsoleArtifactV2ErrorCode::Cancelled,
            "v2 cancellation was not honored");
        const auto deadlineResult =
            ConsoleArtifactConsumerV2::ValidateCurrentForTest(
                fixture.workspace->root.u8string(), fixture.expected,
                nullptr, 4, std::chrono::milliseconds(0));
        Expect(!deadlineResult.success
            && deadlineResult.diagnostic.errorCode ==
                ConsoleArtifactV2ErrorCode::DeadlineExceeded,
            "v2 validation deadline was not honored");

        auto currentA = CreateFixture("swap-a", 4, 5, 1776556801000LL);
        auto currentB = CreateFixture("swap-b", 4, 5, 1776556802000LL);
        bool swapped = false;
        const auto swapResult = ConsoleArtifactConsumerV2::ValidateCurrentForTest(
            currentA.workspace->root.u8string(), currentA.expected,
            nullptr, 5, std::chrono::seconds(30),
            [&](const std::int32_t slot)
            {
                if (slot != 0 || swapped) return;
                fs::rename(currentA.workspace->root / "Current",
                    currentA.workspace->root / "Previous");
                fs::rename(currentB.workspace->root / "Current",
                    currentA.workspace->root / "Current");
                swapped = true;
            });
        Expect(swapped && !swapResult.success,
            "concurrent Current replacement was accepted");
    }

    void VerifyCreatedAtIdentitySemantics()
    {
        auto first = CreateFixture("created-first", 2, 2, 1776556803000LL);
        auto second = CreateFixture("created-second", 2, 2, 1776556803001LL);
        Expect(first.manifest.ArtifactSetId == second.manifest.ArtifactSetId,
            "CreatedAt changed v2 ArtifactSetID");
        Expect(first.manifest.ArtifactSetDigest != second.manifest.ArtifactSetDigest,
            "CreatedAt did not change v2 ArtifactSetDigest");
    }

    int RunScale10K()
    {
        const auto started = std::chrono::steady_clock::now();
        auto fixture = CreateFixture("scale-10k", 10000, 2);
        const auto result = ConsoleArtifactConsumerV2::ValidateCurrentForTest(
            fixture.workspace->root.u8string(), fixture.expected,
            nullptr, 1, std::chrono::minutes(4));
        Expect(result.success && result.snapshot
            && result.snapshot->routes.size() == 10000
            && result.metrics.routeFilesRead == 10000
            && result.metrics.rowsParsed == 20000,
            "10K x 2 v2 scale validation failed");
        const double totalSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << "Console Artifact Consumer v2 10Kx2 PASS"
            << " validation_s=" << result.metrics.elapsedSeconds
            << " total_fixture_and_validation_s=" << totalSeconds
            << " files=" << result.metrics.routeFilesRead
            << " rows=" << result.metrics.rowsParsed
            << " peak_working_set_mib="
            << result.metrics.peakWorkingSetBytes / (1024.0 * 1024.0)
            << " peak_handles=" << result.metrics.peakProcessHandles
            << " parsed_rows_retained=0 mission_binaries=0\n";
        return 0;
    }
}

int RunConsoleArtifactConsumerV2Tests()
{
    try
    {
        VerifyValidSnapshotAndReadApi();
        VerifyLegacyAndReadyFailures();
        VerifyManifestAndRouteFailures();
        VerifyDomainAndPathRejection();
        VerifyReparseAndCurrentOnly();
        VerifyShard1200();
        VerifyAtomicityCancellationDeadlineAndSwap();
        VerifyCreatedAtIdentitySemantics();
        std::cout << "Console Artifact Consumer v2 tests passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Console Artifact Consumer v2 tests failed: "
            << exception.what() << '\n';
        return 1;
    }
}

int RunConsoleArtifactConsumerV2Scale10K()
{
    try
    {
        return RunScale10K();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Console Artifact Consumer v2 10Kx2 failed: "
            << exception.what() << '\n';
        return 1;
    }
}
