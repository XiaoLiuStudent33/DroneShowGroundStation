#define WIN32_LEAN_AND_MEAN
#include "ConsoleArtifactConsumer.h"

#include <windows.h>
#include <psapi.h>

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
    namespace wpl = dronegcs::wpl110;

    void Expect(const bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    artifact::Digest256 HashText(const std::string& text)
    {
        artifact::Digest256 digest;
        Expect(artifact::Sha256::Hash(text.data(), text.size(), digest), "SHA-256 failed");
        return digest;
    }

    class TemporaryWorkspace final
    {
    public:
        explicit TemporaryWorkspace(const std::string& tag)
        {
            static std::atomic<unsigned> counter{ 0 };
            root = fs::temp_directory_path() /
                ("DroneGCS-Artifact-" + tag + "-" +
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
        artifact::Manifest manifest;
        ExpectedArtifactIdentity expected;
        std::shared_ptr<FrozenAssignmentAcceptedSnapshot> frozen;
    };

    std::string MakeRouteBytes(const std::int32_t routeSlot, const std::int32_t frames)
    {
        std::string bytes = std::string(wpl::Header) + "\r\n";
        for (std::int32_t frame = 0; frame < frames; ++frame)
        {
            wpl::Row row;
            row.Sequence = frame;
            row.Red = static_cast<std::uint8_t>((routeSlot + frame) % 256);
            row.Green = static_cast<std::uint8_t>((routeSlot * 3 + frame) % 256);
            row.Blue = static_cast<std::uint8_t>((routeSlot * 7 + frame) % 256);
            row.LatitudeE7 = 399000000 + routeSlot * 10 + frame;
            row.LongitudeE7 = 1163000000 + routeSlot * 10 + frame;
            row.AltitudeE7 = 500000000LL + routeSlot * 100 + frame;
            wpl::FormattedRow formatted;
            Expect(wpl::FormatRow(row, formatted), "WPL row formatting failed");
            bytes.append(reinterpret_cast<const char*>(formatted.Bytes.data()), formatted.Size);
            if (frame + 1 < frames) bytes += "\r\n";
        }
        return bytes;
    }

    void WriteBinary(const fs::path& path, const std::string& bytes)
    {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Expect(output.good(), "fixture file could not be opened");
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Expect(output.good(), "fixture file write failed");
    }

    void FinalizeMetadata(Fixture& fixture)
    {
        artifact::Error error;
        Expect(artifact::ComputeArtifactSetId(
            artifact::BuildIdentityFromManifest(fixture.manifest),
            fixture.manifest.ArtifactSetId, error), "ArtifactSetID failed");
        Expect(artifact::ComputeArtifactSetDigest(
            fixture.manifest, fixture.manifest.ArtifactSetDigest, error),
            "ArtifactSetDigest failed");
        std::string manifestBytes;
        std::string readyBytes;
        Expect(artifact::SerializeCanonicalManifest(fixture.manifest, manifestBytes, error),
            "canonical Manifest serialization failed");
        Expect(artifact::SerializeReadyMarker(
            fixture.manifest.ArtifactSetId, fixture.manifest.ArtifactSetDigest,
            readyBytes, error), "canonical READY serialization failed");
        const fs::path current = fixture.workspace->root / "Current";
        WriteBinary(current / "manifest.json", manifestBytes);
        WriteBinary(current / "READY", readyBytes);

        fixture.expected.artifactSetId = fixture.manifest.ArtifactSetId;
        fixture.expected.artifactSetDigest = fixture.manifest.ArtifactSetDigest;
        fixture.expected.assignmentBinding = fixture.manifest.Assignment;

        fixture.frozen = std::make_shared<FrozenAssignmentAcceptedSnapshot>();
        fixture.frozen->identity.fleetSessionId = fixture.manifest.Assignment.FleetSessionId;
        fixture.frozen->identity.assignmentAuthoritySessionId =
            fixture.manifest.Assignment.AssignmentAuthoritySessionId;
        fixture.frozen->identity.assignmentRevision =
            static_cast<std::uint64_t>(fixture.manifest.Assignment.AssignmentRevision);
        fixture.frozen->fleetRevisionAtCapture =
            static_cast<std::uint64_t>(fixture.manifest.FleetRevisionAtCapture);
        fixture.frozen->routeDatasetGeneration =
            static_cast<std::uint64_t>(fixture.manifest.RouteDatasetGeneration);
        fixture.frozen->completionMode = fixture.manifest.Completion == artifact::CompletionMode::Full
            ? dspgcs::FrozenAssignmentCompletionMode::Full
            : dspgcs::FrozenAssignmentCompletionMode::Degraded;
        fixture.frozen->freezePolicy = fixture.manifest.Freeze == artifact::FreezePolicy::RequireFull
            ? dspgcs::FrozenAssignmentFreezePolicy::RequireFull
            : dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded;
        fixture.frozen->routeSlotCount = static_cast<std::uint32_t>(fixture.manifest.RouteSlotCount);
        fixture.frozen->matchedCount = static_cast<std::uint32_t>(fixture.manifest.MatchedCount);
        fixture.frozen->spareCount = static_cast<std::uint32_t>(fixture.manifest.SpareCount);
        fixture.frozen->excludedCount = static_cast<std::uint32_t>(fixture.manifest.ExcludedCount);
        fixture.frozen->vacantCount = static_cast<std::uint32_t>(fixture.manifest.VacantCount);
        fixture.frozen->matchedMappings.reserve(fixture.manifest.Mapping.size());
        for (const auto& mapping : fixture.manifest.Mapping)
            fixture.frozen->matchedMappings.push_back({ mapping.DroneId, mapping.RouteSlotIndex });
        std::copy(fixture.manifest.Assignment.AssignmentDigest.Bytes.begin(),
            fixture.manifest.Assignment.AssignmentDigest.Bytes.end(),
            fixture.frozen->canonicalPayloadDigest.begin());
    }

    Fixture CreateFixture(
        const std::string& tag,
        const std::int32_t routeCount,
        const std::int32_t frameCount,
        const std::int32_t matchedCount)
    {
        Fixture fixture;
        fixture.workspace = std::make_unique<TemporaryWorkspace>(tag);
        auto& manifest = fixture.manifest;
        manifest.CreatedAtUtc = 1700000000000LL;
        manifest.Assignment.FleetSessionId = "fleet-session-fixture";
        manifest.Assignment.AssignmentAuthoritySessionId = "assignment-authority-fixture";
        manifest.Assignment.AssignmentRevision = 9;
        manifest.Assignment.AssignmentDigest = HashText("assignment-fixture");
        manifest.FleetRevisionAtCapture = 21;
        manifest.RouteDatasetGeneration = 34;
        manifest.Completion = matchedCount == routeCount
            ? artifact::CompletionMode::Full : artifact::CompletionMode::Degraded;
        manifest.Freeze = matchedCount == routeCount
            ? artifact::FreezePolicy::RequireFull : artifact::FreezePolicy::AllowDegraded;
        manifest.MatchedCount = matchedCount;
        manifest.SpareCount = 2;
        manifest.ExcludedCount = 1;
        manifest.VacantCount = routeCount - matchedCount;
        manifest.Source.SourceDatasetDigest = HashText("source-dataset-fixture");
        manifest.Source.SourceByteSize = 123456;
        manifest.Source.SourceDisplayFileName = "fixture.npz";
        manifest.FrameCount = frameCount;
        manifest.RouteSlotCount = routeCount;
        manifest.WaypointGeneratorVersion = "DroneGCS-WPL110-v1";
        manifest.GeneratorInputDigest = HashText("generator-input-fixture");
        for (std::int32_t slot = 0; slot < matchedCount; ++slot)
            manifest.Mapping.push_back({ "drone-" + std::to_string(slot), slot });

        const fs::path current = fixture.workspace->root / "Current";
        for (std::int32_t slot = 0; slot < routeCount; ++slot)
        {
            std::string relative;
            artifact::Error error;
            Expect(artifact::BuildRouteRelativePath(slot, relative, error),
                "route relative path failed");
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

    ConsoleArtifactValidationResult Validate(const Fixture& fixture)
    {
        return ConsoleArtifactConsumer::ValidateCurrentForTest(
            fixture.workspace->root.u8string(), fixture.expected, fixture.frozen);
    }

    void ExpectFailure(
        const Fixture& fixture,
        const ConsoleArtifactErrorCode expectedCode,
        const std::string& label)
    {
        const auto result = Validate(fixture);
        Expect(!result.success, label + " unexpectedly passed");
        Expect(result.diagnostic.errorCode == expectedCode,
            label + " returned the wrong finite error code");
    }

    void VerifyValidFullAndDegraded()
    {
        auto full = CreateFixture("valid-full", 3, 5, 3);
        auto fullResult = Validate(full);
        Expect(fullResult.success && fullResult.snapshot, "valid Full Artifact failed");
        Expect(fullResult.metrics.physicalRouteOpenCount == 3
            && fullResult.metrics.routeFilesRead == 3
            && fullResult.metrics.rowsParsed == 15,
            "Full streaming metrics are incorrect");
        Expect(fullResult.snapshot->routes.size() == 3
            && fullResult.snapshot->matchedMappings.size() == 3,
            "Full snapshot is not lightweight metadata");

        auto degraded = CreateFixture("valid-degraded", 4, 3, 2);
        auto degradedResult = Validate(degraded);
        Expect(degradedResult.success && degradedResult.snapshot,
            "valid Degraded Artifact failed");
        Expect(degradedResult.snapshot->routes.size() == 4
            && degradedResult.snapshot->matchedMappings.size() == 2
            && degradedResult.snapshot->vacantCount == 2,
            "Vacant routes were not validated separately from matched projection");
        Expect(degradedResult.metrics.routeFilesRead == 4
            && degradedResult.metrics.rowsParsed == 12,
            "Degraded route domain was not fully validated");
    }

    void VerifyIdentityAndFrozenFailures()
    {
        auto fixture = CreateFixture("identity", 2, 2, 2);
        fixture.expected.artifactSetId[0] = fixture.expected.artifactSetId[0] == 'a' ? 'b' : 'a';
        ExpectFailure(fixture, ConsoleArtifactErrorCode::ArtifactIdentityMismatch,
            "ArtifactSetID mismatch");

        fixture = CreateFixture("digest", 2, 2, 2);
        fixture.expected.artifactSetDigest.Bytes[0] ^= 1;
        ExpectFailure(fixture, ConsoleArtifactErrorCode::ArtifactIdentityMismatch,
            "ArtifactSetDigest mismatch");

        fixture = CreateFixture("frozen-binding", 2, 2, 2);
        fixture.frozen->identity.assignmentAuthoritySessionId += "-different";
        ExpectFailure(fixture, ConsoleArtifactErrorCode::FrozenAssignmentMismatch,
            "Frozen binding mismatch");

        fixture = CreateFixture("frozen-mapping", 2, 2, 2);
        fixture.frozen->matchedMappings[0].droneId += "-different";
        ExpectFailure(fixture, ConsoleArtifactErrorCode::FrozenAssignmentMismatch,
            "Frozen mapping mismatch");
    }

    void VerifyFilesystemAndCorruptionFailures()
    {
        auto missing = CreateFixture("missing-route", 2, 2, 2);
        fs::remove(missing.workspace->root / "Current" /
            fs::u8path(missing.manifest.Routes[1].RelativePath));
        ExpectFailure(missing, ConsoleArtifactErrorCode::UnexpectedCurrentEntry,
            "missing route");

        auto extra = CreateFixture("extra-route", 2, 2, 2);
        WriteBinary(extra.workspace->root / "Current" / "extra.txt", "extra");
        ExpectFailure(extra, ConsoleArtifactErrorCode::UnexpectedCurrentEntry,
            "extra route");

        auto corrupt = CreateFixture("corrupt-sha", 2, 2, 2);
        const fs::path corruptPath = corrupt.workspace->root / "Current" /
            fs::u8path(corrupt.manifest.Routes[0].RelativePath);
        std::string bytes = MakeRouteBytes(0, 2);
        const std::size_t firstRgb = bytes.find("\t0\t0\t0\t0\t", 13);
        Expect(firstRgb != std::string::npos, "corrupt SHA fixture RGB was not found");
        bytes[firstRgb + 1] = '1';
        WriteBinary(corruptPath, bytes);
        ExpectFailure(corrupt, ConsoleArtifactErrorCode::RouteDigestMismatch,
            "corrupt route SHA");

        auto malformed = CreateFixture("malformed-wpl", 2, 2, 2);
        const fs::path malformedPath = malformed.workspace->root / "Current" /
            fs::u8path(malformed.manifest.Routes[0].RelativePath);
        bytes = MakeRouteBytes(0, 2);
        bytes[0] = 'X';
        WriteBinary(malformedPath, bytes);
        ExpectFailure(malformed, ConsoleArtifactErrorCode::WaypointSemanticInvalid,
            "malformed WPL");

        auto wrongSequence = CreateFixture("wrong-sequence", 2, 2, 2);
        const fs::path sequencePath = wrongSequence.workspace->root / "Current" /
            fs::u8path(wrongSequence.manifest.Routes[0].RelativePath);
        bytes = MakeRouteBytes(0, 2);
        const std::size_t secondRow = bytes.find("\r\n", 13) + 2;
        bytes[secondRow] = '9';
        WriteBinary(sequencePath, bytes);
        ExpectFailure(wrongSequence, ConsoleArtifactErrorCode::WaypointSemanticInvalid,
            "wrong sequence");

        auto wrongFrameCount = CreateFixture("wrong-frame-count", 2, 2, 2);
        const fs::path framePath = wrongFrameCount.workspace->root / "Current" /
            fs::u8path(wrongFrameCount.manifest.Routes[0].RelativePath);
        bytes = MakeRouteBytes(0, 1);
        WriteBinary(framePath, bytes);
        wrongFrameCount.manifest.Routes[0].FileByteSize =
            static_cast<std::int64_t>(bytes.size());
        wrongFrameCount.manifest.Routes[0].RouteArtifactDigest = HashText(bytes);
        FinalizeMetadata(wrongFrameCount);
        ExpectFailure(wrongFrameCount, ConsoleArtifactErrorCode::WaypointSemanticInvalid,
            "wrong FrameCount");

        auto ready = CreateFixture("bad-ready", 2, 2, 2);
        WriteBinary(ready.workspace->root / "Current" / "READY", "bad-ready");
        ExpectFailure(ready, ConsoleArtifactErrorCode::ReadyMissingOrInvalid,
            "READY corruption");

        auto manifest = CreateFixture("bad-manifest", 2, 2, 2);
        WriteBinary(manifest.workspace->root / "Current" / "manifest.json", "{}");
        ExpectFailure(manifest, ConsoleArtifactErrorCode::ManifestMissingOrInvalid,
            "Manifest corruption");

        auto traversal = CreateFixture("path-traversal", 2, 2, 2);
        std::ifstream input(traversal.workspace->root / "Current" / "manifest.json", std::ios::binary);
        std::string manifestBytes((std::istreambuf_iterator<char>(input)), {});
        const std::string canonical = traversal.manifest.Routes[0].RelativePath;
        manifestBytes.replace(manifestBytes.find(canonical), canonical.size(), "../escape.waypoints");
        WriteBinary(traversal.workspace->root / "Current" / "manifest.json", manifestBytes);
        ExpectFailure(traversal, ConsoleArtifactErrorCode::ManifestMissingOrInvalid,
            "path traversal");
    }

    void VerifyReparseAndIdentityRecheck()
    {
        auto fixture = CreateFixture("reparse", 2, 2, 2);
        const fs::path shard = (fixture.workspace->root / "Current" /
            fs::u8path(fixture.manifest.Routes[0].RelativePath)).parent_path();
        const fs::path outside = fixture.workspace->root / "outside";
        fs::create_directories(outside);
        std::error_code error;
        fs::remove_all(shard, error);
        const DWORD flags = SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2;
        const BOOLEAN linked = CreateSymbolicLinkW(shard.c_str(), outside.c_str(), flags);
        Expect(linked != FALSE, "real directory reparse fixture could not be created");
        ExpectFailure(fixture, ConsoleArtifactErrorCode::ReparsePointDetected,
            "real directory reparse escape");

        auto identity = CreateFixture("identity-recheck", 2, 2, 2);
        ConsoleArtifactConsumer consumer(identity.workspace->root.u8string());
        ConsoleArtifactDiagnostic diagnostic;
        Expect(consumer.IsCurrentIdentityStill(identity.expected, diagnostic),
            "identity recheck rejected unchanged Current");
        WriteBinary(identity.workspace->root / "Current" / "READY", "replaced");
        diagnostic = {};
        Expect(!consumer.IsCurrentIdentityStill(identity.expected, diagnostic),
            "identity recheck accepted replaced Current");
    }

    void VerifyCancellationAndBackgroundWorker()
    {
        auto fixture = CreateFixture("cancel", 4, 50, 4);
        std::atomic_bool cancelled{ true };
        const auto cancelledResult = ConsoleArtifactConsumer::ValidateCurrentForTest(
            fixture.workspace->root.u8string(), fixture.expected, fixture.frozen, &cancelled);
        Expect(!cancelledResult.success
            && cancelledResult.diagnostic.errorCode == ConsoleArtifactErrorCode::Cancelled,
            "synchronous cancellation was not honored");

        ConsoleArtifactConsumer consumer(fixture.workspace->root.u8string());
        const std::uint64_t generation = consumer.RequestValidation(fixture.expected, fixture.frozen);
        ConsoleArtifactValidationResult result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!consumer.PollCompleted(result) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Expect(result.success && result.generation == generation,
            "persistent background validation worker failed");
    }

    void WriteLargeRoute(
        const fs::path& path,
        const std::int32_t routeSlot,
        const std::int32_t frames,
        artifact::RouteRecord& record)
    {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Expect(output.good(), "large route could not be opened");
        artifact::Sha256 sha;
        const std::string header = std::string(wpl::Header) + "\r\n";
        output.write(header.data(), static_cast<std::streamsize>(header.size()));
        Expect(sha.Update(header.data(), header.size()), "large route header hash failed");
        std::int64_t size = static_cast<std::int64_t>(header.size());
        for (std::int32_t frame = 0; frame < frames; ++frame)
        {
            wpl::Row row;
            row.Sequence = frame;
            row.Red = static_cast<std::uint8_t>((routeSlot + frame) % 256);
            row.Green = static_cast<std::uint8_t>((routeSlot + frame * 3) % 256);
            row.Blue = static_cast<std::uint8_t>((routeSlot + frame * 7) % 256);
            row.LatitudeE7 = 399000000 + routeSlot + frame;
            row.LongitudeE7 = 1163000000 + routeSlot + frame;
            row.AltitudeE7 = 500000000LL + frame;
            wpl::FormattedRow formatted;
            Expect(wpl::FormatRow(row, formatted), "large route row formatting failed");
            output.write(reinterpret_cast<const char*>(formatted.Bytes.data()),
                static_cast<std::streamsize>(formatted.Size));
            Expect(sha.Update(formatted.Bytes.data(), formatted.Size), "large route row hash failed");
            size += static_cast<std::int64_t>(formatted.Size);
            if (frame + 1 < frames)
            {
                output.write("\r\n", 2);
                const std::uint8_t newline[] = { '\r', '\n' };
                Expect(sha.Update(newline, 2), "large route newline hash failed");
                size += 2;
            }
        }
        Expect(output.good(), "large route write failed");
        Expect(sha.Finalize(record.RouteArtifactDigest), "large route digest failed");
        record.FileByteSize = size;
    }

    Fixture CreateScaleFixture(
        const std::string& tag,
        const std::int32_t routes,
        const std::int32_t frames)
    {
        Fixture fixture;
        fixture.workspace = std::make_unique<TemporaryWorkspace>(tag);
        auto& manifest = fixture.manifest;
        manifest.CreatedAtUtc = 1700000000000LL;
        manifest.Assignment.FleetSessionId = "fleet-scale";
        manifest.Assignment.AssignmentAuthoritySessionId = "authority-scale";
        manifest.Assignment.AssignmentRevision = 1;
        manifest.Assignment.AssignmentDigest = HashText("assignment-scale");
        manifest.FleetRevisionAtCapture = 1;
        manifest.RouteDatasetGeneration = 1;
        manifest.Completion = artifact::CompletionMode::Full;
        manifest.Freeze = artifact::FreezePolicy::RequireFull;
        manifest.MatchedCount = routes;
        manifest.Source.SourceDatasetDigest = HashText("source-scale");
        manifest.Source.SourceByteSize = 1;
        manifest.Source.SourceDisplayFileName = "scale.npz";
        manifest.FrameCount = frames;
        manifest.RouteSlotCount = routes;
        manifest.WaypointGeneratorVersion = "DroneGCS-WPL110-v1";
        manifest.GeneratorInputDigest = HashText("generator-scale");
        manifest.Mapping.reserve(routes);
        manifest.Routes.reserve(routes);
        for (std::int32_t slot = 0; slot < routes; ++slot)
        {
            manifest.Mapping.push_back({ "drone-" + std::to_string(slot), slot });
            artifact::RouteRecord record;
            record.RouteSlotIndex = slot;
            artifact::Error error;
            Expect(artifact::BuildRouteRelativePath(slot, record.RelativePath, error),
                "scale route path failed");
            WriteLargeRoute(fixture.workspace->root / "Current" /
                fs::u8path(record.RelativePath), slot, frames, record);
            manifest.Routes.push_back(std::move(record));
        }
        FinalizeMetadata(fixture);
        return fixture;
    }

    double FileTimeSeconds(const FILETIME& value)
    {
        ULARGE_INTEGER ticks{};
        ticks.LowPart = value.dwLowDateTime;
        ticks.HighPart = value.dwHighDateTime;
        return static_cast<double>(ticks.QuadPart) / 10000000.0;
    }

    int RunScale(const std::int32_t routes, const std::int32_t frames, const char* label)
    {
        std::cout << label << " fixture preparation: routes=" << routes
            << " frames=" << frames << '\n';
        auto fixture = CreateScaleFixture(label, routes, frames);
        FILETIME created{}, exited{}, kernelBefore{}, userBefore{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernelBefore, &userBefore);
        const auto result = Validate(fixture);
        FILETIME kernelAfter{}, userAfter{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernelAfter, &userAfter);
        PROCESS_MEMORY_COUNTERS memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
        Expect(result.success, std::string(label) + " validation failed");
        Expect(result.metrics.routeFilesRead == static_cast<std::uint64_t>(routes)
            && result.metrics.physicalRouteOpenCount == static_cast<std::uint64_t>(routes)
            && result.metrics.rowsParsed == static_cast<std::uint64_t>(routes) * frames,
            std::string(label) + " metrics mismatch");
        Expect(result.snapshot->routes.size() == static_cast<std::size_t>(routes)
            && result.snapshot->matchedMappings.size() == static_cast<std::size_t>(routes),
            std::string(label) + " snapshot projection mismatch");
        const double mib = static_cast<double>(result.metrics.bytesRead) / (1024.0 * 1024.0);
        const double elapsed = result.metrics.elapsedSeconds;
        const double cpu = FileTimeSeconds(kernelAfter) + FileTimeSeconds(userAfter)
            - FileTimeSeconds(kernelBefore) - FileTimeSeconds(userBefore);
        std::cout << label
            << " PASS elapsed_s=" << elapsed
            << " mib=" << mib
            << " mib_per_s=" << (elapsed > 0.0 ? mib / elapsed : 0.0)
            << " rows_per_s=" << (elapsed > 0.0 ? result.metrics.rowsParsed / elapsed : 0.0)
            << " cpu_s=" << cpu
            << " peak_working_set_mib=" << memory.PeakWorkingSetSize / (1024.0 * 1024.0)
            << " peak_handles=" << result.metrics.peakProcessHandles
            << " physical_reads=" << result.metrics.physicalRouteOpenCount
            << " parsed_rows_retained=0\n";
        return 0;
    }
}

int RunConsoleArtifactConsumerTests()
{
    try
    {
        VerifyValidFullAndDegraded();
        VerifyIdentityAndFrozenFailures();
        VerifyFilesystemAndCorruptionFailures();
        VerifyReparseAndIdentityRecheck();
        VerifyCancellationAndBackgroundWorker();
        std::cout << "Console Artifact Consumer tests passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Console Artifact Consumer tests failed: " << exception.what() << '\n';
        return 1;
    }
}

int RunConsoleArtifactBenchmark(const int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string(argv[1]) == "--artifact-benchmark")
            return RunScale(1000, 9100, "artifact-1000x9100");
        if (argc > 1 && std::string(argv[1]) == "--artifact-scale-10k")
            return RunScale(10000, 2, "artifact-10000x2");
        return -1;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Console Artifact scale test failed: " << exception.what() << '\n';
        return 1;
    }
}
