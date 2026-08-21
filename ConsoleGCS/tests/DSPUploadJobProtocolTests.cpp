#define WIN32_LEAN_AND_MEAN
#include "ConsoleArtifactConsumerV2.h"
#include "ConsoleUploadJobAuthority.h"
#include "DSPGCSUploadJobReceiver.h"
#include "RouteArtifactContractV2.h"
#include "Wpl110Core.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace artifact = dronegcs::route_artifact;
    namespace artifact_v2 = dronegcs::route_artifact::v2;
    namespace wpl = dronegcs::wpl110;

    void Expect(const bool value, const std::string& message)
    {
        if (!value) throw std::runtime_error(message);
    }

    std::string Id(const char value) { return std::string(32, value); }

    artifact::Digest256 Hash(const std::string& bytes)
    {
        artifact::Digest256 digest;
        Expect(artifact::Sha256::Hash(bytes.data(), bytes.size(), digest),
            "SHA-256 fixture failed");
        return digest;
    }

    dspgcs::UploadJobDigest WireDigest(const artifact::Digest256& digest)
    {
        dspgcs::UploadJobDigest result{};
        std::copy(digest.Bytes.begin(), digest.Bytes.end(), result.begin());
        return result;
    }

    class TempRoot final
    {
    public:
        explicit TempRoot(const std::string& tag)
        {
            static std::atomic<unsigned> counter{ 0 };
            root = fs::temp_directory_path() /
                ("DroneGCS-UploadV2E-" + tag + "-" +
                    std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(++counter));
            fs::create_directories(root / "Current");
        }
        ~TempRoot()
        {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
        fs::path root;
    };

    struct ArtifactFixture
    {
        std::unique_ptr<TempRoot> workspace;
        artifact_v2::Manifest manifest;
        ExpectedRouteArtifactV2Identity expected;
    };

    void Write(const fs::path& path, const std::string& bytes)
    {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Expect(output.good(), "fixture file open failed");
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Expect(output.good(), "fixture file write failed");
    }

    std::string RouteBytes(
        const std::int32_t slot,
        const std::int32_t frameCount,
        const std::int32_t variant)
    {
        std::string bytes = std::string(wpl::Header) + "\r\n";
        for (std::int32_t frame = 0; frame < frameCount; ++frame)
        {
            wpl::Row row;
            row.Sequence = frame;
            row.Red = static_cast<std::uint8_t>((slot + frame + variant) % 256);
            row.Green = static_cast<std::uint8_t>((slot * 3 + frame + variant) % 256);
            row.Blue = static_cast<std::uint8_t>((slot * 7 + frame + variant) % 256);
            row.LatitudeE7 = 319000000 + slot + frame + variant;
            row.LongitudeE7 = 1186000000 + slot + frame + variant;
            row.AltitudeE7 = 500000000LL + slot + frame + variant;
            wpl::FormattedRow formatted;
            Expect(wpl::FormatRow(row, formatted), "WPL formatting failed");
            bytes.append(reinterpret_cast<const char*>(formatted.Bytes.data()),
                formatted.Size);
            if (frame + 1 < frameCount) bytes += "\r\n";
        }
        return bytes;
    }

    void Finalize(ArtifactFixture& fixture)
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
            fixture.manifest.ArtifactSetDigest,
            readyBytes, error), error.Detail);
        Write(fixture.workspace->root / "Current" / "manifest.json", manifestBytes);
        Write(fixture.workspace->root / "Current" / "READY", readyBytes);
        fixture.expected.artifactSetId = fixture.manifest.ArtifactSetId;
        fixture.expected.artifactSetDigest = fixture.manifest.ArtifactSetDigest;
    }

    ArtifactFixture MakeArtifact(
        const std::string& tag,
        const std::int32_t routeCount = 2,
        const std::int32_t frameCount = 2,
        const std::int32_t variant = 0)
    {
        ArtifactFixture fixture;
        fixture.workspace = std::make_unique<TempRoot>(tag);
        auto& manifest = fixture.manifest;
        manifest.CreatedAtUtc = 1776556800123LL + variant;
        manifest.Source.SourceDatasetDigest = Hash("dataset-" + tag);
        manifest.Source.SourceByteSize = 1000 + variant;
        manifest.Source.SourceDisplayFileName = "show.npz";
        manifest.FrameCount = frameCount;
        manifest.RouteSlotCount = routeCount;
        manifest.ShowOriginLatitudeE7 = 319324710;
        manifest.ShowOriginLongitudeE7 = 1186385310;
        manifest.WaypointGeneratorVersion = "generator-v2";
        manifest.GeneratorInputDigest = Hash("generator-input-" + tag);
        for (std::int32_t slot = 0; slot < routeCount; ++slot)
        {
            std::string relative;
            artifact::Error error;
            Expect(artifact::BuildRouteRelativePath(slot, relative, error),
                error.Detail);
            const std::string bytes = RouteBytes(slot, frameCount, variant);
            Write(fixture.workspace->root / "Current" / fs::u8path(relative), bytes);
            artifact::RouteRecord record;
            record.RouteSlotIndex = slot;
            record.RelativePath = relative;
            record.FileByteSize = static_cast<std::int64_t>(bytes.size());
            record.RouteArtifactDigest = Hash(bytes);
            manifest.Routes.push_back(std::move(record));
        }
        Finalize(fixture);
        return fixture;
    }

    void ReplaceCurrent(
        ArtifactFixture& destination,
        const ArtifactFixture& replacement)
    {
        std::error_code error;
        fs::remove_all(destination.workspace->root / "Current", error);
        Expect(!error, "Current removal failed");
        fs::copy(replacement.workspace->root / "Current",
            destination.workspace->root / "Current",
            fs::copy_options::recursive, error);
        Expect(!error, "Current replacement failed");
    }

    FrozenAssignmentAcceptedSnapshot MakeFrozen(
        const ArtifactFixture& artifactFixture,
        const std::uint32_t matchedCount = 0,
        const std::uint32_t vacantCount = 0,
        const std::uint32_t spareCount = 0,
        const std::uint32_t excludedCount = 0,
        const std::uint8_t digestSeed = 0x51,
        const std::string& authoritySession = "authority-a",
        const std::uint64_t revision = 1)
    {
        FrozenAssignmentAcceptedSnapshot frozen;
        frozen.sourceDatasetDigest = WireDigest(
            artifactFixture.manifest.Source.SourceDatasetDigest);
        frozen.normalizedShowOriginLatitudeE7 =
            artifactFixture.manifest.ShowOriginLatitudeE7;
        frozen.normalizedShowOriginLongitudeE7 =
            artifactFixture.manifest.ShowOriginLongitudeE7;
        frozen.identity.fleetSessionId = "fleet-v2e";
        frozen.identity.assignmentAuthoritySessionId = authoritySession;
        frozen.identity.assignmentRevision = revision;
        frozen.fleetRevisionAtCapture = 100;
        frozen.routeDatasetGeneration = 999;
        frozen.routeSlotCount = static_cast<std::uint32_t>(
            artifactFixture.manifest.RouteSlotCount);
        frozen.matchedCount = matchedCount == 0
            ? frozen.routeSlotCount - vacantCount : matchedCount;
        frozen.vacantCount = vacantCount;
        frozen.spareCount = spareCount;
        frozen.excludedCount = excludedCount;
        frozen.completionMode = vacantCount == 0
            ? dspgcs::FrozenAssignmentCompletionMode::Full
            : dspgcs::FrozenAssignmentCompletionMode::Degraded;
        frozen.freezePolicy = vacantCount == 0
            ? dspgcs::FrozenAssignmentFreezePolicy::RequireFull
            : dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded;
        frozen.frozenAtUtcMs = 1776556801000LL;
        frozen.canonicalPayloadDigest.fill(digestSeed);
        for (std::uint32_t slot = 0; slot < frozen.matchedCount; ++slot)
        {
            frozen.matchedMappings.push_back({
                "drone-" + std::to_string(slot),
                static_cast<std::int32_t>(slot) });
        }
        return frozen;
    }

    std::shared_ptr<const FrozenAssignmentAcceptedSnapshot> AcceptFrozen(
        FrozenAssignmentStore& store,
        FrozenAssignmentAcceptedSnapshot frozen)
    {
        store.SynchronizeFleetSession(frozen.identity.fleetSessionId);
        const std::string fleet = frozen.identity.fleetSessionId;
        const auto accepted = store.Accept(std::move(frozen));
        Expect(accepted.accepted, "Frozen fixture was not accepted");
        const auto snapshot = store.GetAcceptedSnapshot(fleet);
        Expect(snapshot != nullptr, "Frozen accepted snapshot is missing");
        return snapshot;
    }

    dspgcs::UploadJobCommandDTO StartCommand(
        const ArtifactFixture& fixture,
        const FrozenAssignmentAcceptedSnapshot& frozen,
        const std::string& jobId = Id('1'),
        const std::string& commandId = Id('2'),
        const std::uint32_t concurrency = 8)
    {
        dspgcs::UploadJobCommandDTO command;
        command.clientCommandId = commandId;
        command.uploadJobId = jobId;
        command.commandType = dspgcs::UploadJobCommandType::Start;
        command.requestedMaxConcurrentUploads = concurrency;
        command.artifactBinding.artifactSetId = fixture.manifest.ArtifactSetId;
        command.artifactBinding.artifactSetDigest =
            WireDigest(fixture.manifest.ArtifactSetDigest);
        command.frozenAssignmentBinding.fleetSessionId =
            frozen.identity.fleetSessionId;
        command.frozenAssignmentBinding.assignmentAuthoritySessionId =
            frozen.identity.assignmentAuthoritySessionId;
        command.frozenAssignmentBinding.assignmentRevision =
            frozen.identity.assignmentRevision;
        command.frozenAssignmentBinding.assignmentDigest =
            frozen.canonicalPayloadDigest;
        return command;
    }

    std::vector<std::uint8_t> EncodeCommand(
        const dspgcs::UploadJobCommandDTO& command)
    {
        std::vector<std::uint8_t> bytes;
        std::string error;
        Expect(dspgcs::EncodeUploadJobCommand(command, bytes, error), error);
        return bytes;
    }

    dspgcs::UploadJobSnapshotDTO WaitForStart(
        ConsoleUploadJobAuthority& authority,
        const std::string& jobId,
        const std::chrono::seconds timeout = std::chrono::seconds(30))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (authority.GetPendingStartCount() != 0 &&
            std::chrono::steady_clock::now() < deadline)
        {
            authority.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        authority.Tick();
        Expect(authority.GetPendingStartCount() == 0,
            "PendingStart exceeded its test deadline");
        dspgcs::UploadJobSnapshotRequestDTO request;
        request.clientRequestId = Id('e');
        request.uploadJobId = jobId;
        return authority.BuildSnapshot(request);
    }

    void WaitForConsumerAccepted(
        ConsoleArtifactConsumerV2& consumer,
        const std::chrono::seconds timeout = std::chrono::seconds(30))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!consumer.GetAcceptedSnapshot() &&
            std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Expect(consumer.GetAcceptedSnapshot() != nullptr,
            "Artifact consumer acceptance exceeded its test deadline");
    }

    void VerifyWireContract()
    {
        auto fixture = MakeArtifact("wire");
        auto frozen = MakeFrozen(fixture);
        auto command = StartCommand(fixture, frozen);
        const auto encoded = EncodeCommand(command);
        std::string error;
        dspgcs::UploadJobCommandDTO decoded;
        Expect(dspgcs::DecodeUploadJobCommand(encoded, decoded, error), error);
        Expect(decoded.payloadSchemaVersion == 2 &&
            dspgcs::UploadArtifactBindingEqual(
                decoded.artifactBinding, command.artifactBinding) &&
            dspgcs::UploadFrozenAssignmentBindingEqual(
                decoded.frozenAssignmentBinding,
                command.frozenAssignmentBinding),
            "schema2 command round trip changed a binding");

        auto invalid = command;
        invalid.artifactBinding.artifactSetDigest.fill(0);
        std::vector<std::uint8_t> ignored;
        Expect(!dspgcs::EncodeUploadJobCommand(invalid, ignored, error),
            "zero Artifact digest was accepted");
        invalid = command;
        invalid.frozenAssignmentBinding.assignmentDigest.fill(0);
        Expect(!dspgcs::EncodeUploadJobCommand(invalid, ignored, error),
            "zero Assignment digest was accepted");

        auto legacyBytes = encoded;
        const std::uint16_t legacyVersion = 1;
        std::memcpy(legacyBytes.data(), &legacyVersion, sizeof(legacyVersion));
        Expect(!dspgcs::DecodeUploadJobCommand(legacyBytes, decoded, error) &&
            error.find("Unsupported") != std::string::npos,
            "schema1 payload was not explicitly rejected");

        dspgcs::MessageEnvelope envelope;
        envelope.messageType = dspgcs::UploadJobMessageTypes::Command;
        envelope.sessionId = "legacy-client";
        envelope.correlationId = command.clientCommandId;
        DSPGCSUploadJobReceiver receiver;
        receiver.EnqueueFrame({ dspgcs::MakeFrame(envelope, legacyBytes) });
        TempRoot root("legacy-receiver");
        ConsoleUploadJobAuthority authority(root.root.u8string(), nullptr, nullptr);
        std::vector<dspgcs::UploadJobCommandAckDTO> acknowledgements;
        receiver.ProcessPending(authority,
            [&acknowledgements](const auto& acknowledgement)
            {
                acknowledgements.push_back(acknowledgement);
                return true;
            }, {}, {});
        Expect(acknowledgements.size() == 1 &&
            acknowledgements.front().resultCode ==
                dspgcs::UploadJobErrorCode::UnsupportedProtocolVersion,
            "schema1 receiver rejection did not return UnsupportedProtocolVersion");
        Expect(static_cast<std::uint16_t>(
            dspgcs::UploadJobMessageTypes::Command) == 9 &&
            static_cast<std::uint16_t>(
                dspgcs::UploadJobMessageTypes::RouteStateDelta) == 14,
            "Upload Job message type values changed");
    }

    dspgcs::UploadJobSnapshotDTO RunStart(
        ArtifactFixture& fixture,
        FrozenAssignmentAcceptedSnapshot frozenValue,
        const std::string& jobId,
        const std::string& commandId,
        const std::function<void(ConsoleArtifactConsumerV2&,
            FrozenAssignmentStore&, ConsoleUploadJobAuthority&)>& beforeTick = {})
    {
        FrozenAssignmentStore store;
        const auto frozen = AcceptFrozen(store, std::move(frozenValue));
        ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
        ConsoleUploadJobAuthority authority(
            fixture.workspace->root.u8string(), &consumer, &store);
        const auto start = StartCommand(fixture, *frozen, jobId, commandId);
        const auto acknowledgement = authority.HandleCommand(
            "client", start, EncodeCommand(start));
        Expect(acknowledgement.status ==
            dspgcs::UploadJobCommandAckStatus::Accepted,
            "Start was not accepted for validation");
        Expect(authority.GetJobCount() == 0 &&
            authority.GetPendingStartCount() == 1,
            "PendingStart was exposed as an executable Job");
        if (beforeTick) beforeTick(consumer, store, authority);
        const auto outcome = WaitForStart(authority, jobId);
        if (outcome.resultCode != dspgcs::UploadJobErrorCode::None)
        {
            Expect(authority.GetJobCount() == 0,
                "failed PendingStart published an executable Job");
            artifact::ArtifactReadLease gate;
            bool recovered = false;
            std::string error;
            Expect(gate.TryAcquire(fixture.workspace->root.wstring(), recovered, error) ==
                artifact::ArtifactReadLeaseAcquireResult::Acquired,
                "failed PendingStart leaked an Artifact read lease");
        }
        return outcome;
    }

    void VerifyCompatibilityGate()
    {
        {
            auto fixture = MakeArtifact("compatible");
            auto frozen = MakeFrozen(fixture);
            frozen.routeDatasetGeneration = 123456789;
            FrozenAssignmentStore store;
            const auto accepted = AcceptFrozen(store, frozen);
            ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
            ConsoleUploadJobAuthority authority(
                fixture.workspace->root.u8string(), &consumer, &store);
            const auto start = StartCommand(fixture, *accepted);
            const auto payload = EncodeCommand(start);
            const auto first = authority.HandleCommand("client", start, payload);
            const auto duplicate = authority.HandleCommand("client", start, payload);
            Expect(first.status == dspgcs::UploadJobCommandAckStatus::Accepted &&
                duplicate.authoritativeJobRevision == first.authoritativeJobRevision &&
                authority.GetPendingStartCount() == 1,
                "PendingStart idempotency created duplicate validation");
            auto conflictingPayload = start;
            ++conflictingPayload.requestedMaxConcurrentUploads;
            Expect(authority.HandleCommand("client", conflictingPayload,
                EncodeCommand(conflictingPayload)).resultCode ==
                    dspgcs::UploadJobErrorCode::IdempotencyConflict,
                "same idempotency key accepted a different Start payload");
            dspgcs::UploadJobCommandDTO pendingCancel;
            pendingCancel.clientCommandId = Id('0');
            pendingCancel.uploadJobId = start.uploadJobId;
            pendingCancel.commandType = dspgcs::UploadJobCommandType::Cancel;
            pendingCancel.expectedJobRevision = first.authoritativeJobRevision;
            Expect(authority.HandleCommand("client", pendingCancel,
                EncodeCommand(pendingCancel)).resultCode ==
                    dspgcs::UploadJobErrorCode::UnknownJob &&
                authority.GetPendingStartCount() == 1 &&
                authority.GetJobCount() == 0,
                "PendingStart cancellation was confused with executable Job cancellation");
            const auto ready = WaitForStart(authority, start.uploadJobId);
            Expect(ready.resultCode == dspgcs::UploadJobErrorCode::None &&
                ready.jobState == dspgcs::UploadJobState::Ready &&
                ready.matchedCount == 2 && authority.GetJobCount() == 1,
                "matching StableSource did not materialize one Ready Job");
            auto conflict = start;
            conflict.clientCommandId = Id('3');
            conflict.artifactBinding.artifactSetDigest[0] ^= 1;
            Expect(authority.HandleCommand("client", conflict,
                EncodeCommand(conflict)).resultCode ==
                    dspgcs::UploadJobErrorCode::JobIdCollision,
                "different binding reused an existing JobID");
        }
        {
            auto fixture = MakeArtifact("fleet-session-stale");
            FrozenAssignmentStore store;
            const auto frozen = MakeFrozen(fixture);
            store.SynchronizeFleetSession("different-current-fleet");
            ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
            ConsoleUploadJobAuthority authority(
                fixture.workspace->root.u8string(), &consumer, &store);
            const auto start = StartCommand(fixture, frozen, Id('1'), Id('2'));
            Expect(authority.HandleCommand("client", start,
                EncodeCommand(start)).resultCode ==
                    dspgcs::UploadJobErrorCode::FleetSessionStale,
                "stale FleetSession was not distinguished at Start");
        }
        {
            auto fixture = MakeArtifact("frozen-missing");
            FrozenAssignmentStore store;
            const auto frozen = MakeFrozen(fixture);
            store.SynchronizeFleetSession(frozen.identity.fleetSessionId);
            ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
            ConsoleUploadJobAuthority authority(
                fixture.workspace->root.u8string(), &consumer, &store);
            const auto start = StartCommand(fixture, frozen, Id('3'), Id('4'));
            Expect(authority.HandleCommand("client", start,
                EncodeCommand(start)).resultCode ==
                    dspgcs::UploadJobErrorCode::FrozenAssignmentMissing,
                "missing Frozen Assignment was not distinguished at Start");
        }
        {
            auto fixture = MakeArtifact("frozen-exact-mismatch");
            FrozenAssignmentStore store;
            const auto frozen = AcceptFrozen(store, MakeFrozen(fixture));
            ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
            ConsoleUploadJobAuthority authority(
                fixture.workspace->root.u8string(), &consumer, &store);
            auto start = StartCommand(fixture, *frozen, Id('5'), Id('6'));
            start.frozenAssignmentBinding.assignmentDigest[0] ^= 1;
            Expect(authority.HandleCommand("client", start,
                EncodeCommand(start)).resultCode ==
                    dspgcs::UploadJobErrorCode::FrozenAssignmentIdentityMismatch,
                "exact Frozen identity mismatch was not distinguished at Start");
        }
        {
            auto fixture = MakeArtifact("artifact-exact-mismatch");
            FrozenAssignmentStore store;
            const auto frozen = AcceptFrozen(store, MakeFrozen(fixture));
            ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
            ConsoleUploadJobAuthority authority(
                fixture.workspace->root.u8string(), &consumer, &store);
            auto start = StartCommand(fixture, *frozen, Id('7'), Id('8'));
            start.artifactBinding.artifactSetDigest[0] ^= 1;
            authority.HandleCommand("client", start, EncodeCommand(start));
            const auto outcome = WaitForStart(authority, start.uploadJobId);
            Expect(outcome.resultCode ==
                    dspgcs::UploadJobErrorCode::ArtifactIdentityMismatch &&
                authority.GetJobCount() == 0,
                "exact Artifact identity mismatch did not reject Start");
        }
        {
            auto fixture = MakeArtifact("dataset-mismatch");
            auto frozen = MakeFrozen(fixture);
            frozen.sourceDatasetDigest[0] ^= 1;
            const auto outcome = RunStart(
                fixture, frozen, Id('4'), Id('5'));
            Expect(outcome.resultCode ==
                dspgcs::UploadJobErrorCode::StableSourceDatasetMismatch,
                "dataset mismatch did not fail distinctly");
        }
        {
            auto fixture = MakeArtifact("lat-mismatch");
            auto frozen = MakeFrozen(fixture);
            ++frozen.normalizedShowOriginLatitudeE7;
            const auto outcome = RunStart(
                fixture, frozen, Id('6'), Id('7'));
            Expect(outcome.resultCode ==
                dspgcs::UploadJobErrorCode::StableSourceOriginMismatch,
                "latitude mismatch did not fail distinctly");
        }
        {
            auto fixture = MakeArtifact("lon-mismatch");
            auto frozen = MakeFrozen(fixture);
            ++frozen.normalizedShowOriginLongitudeE7;
            const auto outcome = RunStart(
                fixture, frozen, Id('8'), Id('9'));
            Expect(outcome.resultCode ==
                dspgcs::UploadJobErrorCode::StableSourceOriginMismatch,
                "longitude mismatch did not fail distinctly");
        }
        {
            auto fixture = MakeArtifact("domain-mismatch");
            auto frozen = MakeFrozen(fixture);
            ++frozen.routeSlotCount;
            ++frozen.vacantCount;
            frozen.completionMode = dspgcs::FrozenAssignmentCompletionMode::Degraded;
            frozen.freezePolicy = dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded;
            const auto outcome = RunStart(
                fixture, frozen, Id('a'), Id('b'));
            Expect(outcome.resultCode ==
                dspgcs::UploadJobErrorCode::RouteDomainMismatch,
                "route domain mismatch did not fail distinctly");
        }
        {
            auto fixture = MakeArtifact("degraded", 5, 1);
            auto frozen = MakeFrozen(fixture, 3, 2, 4, 1);
            FrozenAssignmentStore store;
            const auto accepted = AcceptFrozen(store, frozen);
            ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
            ConsoleUploadJobAuthority authority(
                fixture.workspace->root.u8string(), &consumer, &store);
            const auto start = StartCommand(fixture, *accepted, Id('c'), Id('d'));
            authority.HandleCommand("client", start, EncodeCommand(start));
            const auto ready = WaitForStart(authority, start.uploadJobId);
            Expect(ready.resultCode == dspgcs::UploadJobErrorCode::None &&
                ready.matchedCount == 3 && ready.vacantCount == 2 &&
                ready.spareCount == 4 && ready.excludedCount == 1 &&
                ready.waitingCount == 3,
                "degraded Frozen policy did not create matched-only targets");
        }
    }

    void VerifyMissingRouteRecord()
    {
        auto fixture = MakeArtifact("missing-route", 3, 1);
        FrozenAssignmentStore store;
        const auto frozen = AcceptFrozen(store, MakeFrozen(fixture));
        ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
        ConsoleUploadJobAuthority authority(
            fixture.workspace->root.u8string(), &consumer, &store);
        const auto start = StartCommand(fixture, *frozen, Id('1'), Id('2'));
        authority.HandleCommand("client", start, EncodeCommand(start));
        WaitForConsumerAccepted(consumer);
        auto mutableAccepted = std::const_pointer_cast<AcceptedRouteArtifactV2>(
            consumer.GetAcceptedSnapshot());
        mutableAccepted->routes.erase(mutableAccepted->routes.begin() + 1);
        const auto outcome = WaitForStart(authority, start.uploadJobId);
        Expect(outcome.resultCode ==
            dspgcs::UploadJobErrorCode::RouteRecordMissing &&
            authority.GetJobCount() == 0,
            "missing held RouteRecord created a partial Job");
    }

    void VerifyToctouAndImmutableOwnership()
    {
        {
            auto fixture = MakeArtifact("frozen-swap", 2, 2);
            FrozenAssignmentStore store;
            const auto frozenA = AcceptFrozen(store, MakeFrozen(fixture));
            ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
            ConsoleUploadJobAuthority authority(
                fixture.workspace->root.u8string(), &consumer, &store);
            const auto start = StartCommand(fixture, *frozenA, Id('3'), Id('4'));
            authority.HandleCommand("client", start, EncodeCommand(start));
            auto frozenB = MakeFrozen(
                fixture, 0, 0, 0, 0, 0x62, "authority-b", 1);
            Expect(store.Accept(std::move(frozenB)).accepted,
                "replacement Frozen was not accepted");
            const auto outcome = WaitForStart(authority, start.uploadJobId);
            Expect(outcome.resultCode ==
                dspgcs::UploadJobErrorCode::FrozenAssignmentIdentityMismatch &&
                authority.GetJobCount() == 0,
                "Frozen replacement during validation was not rejected");
        }
        {
            auto fixtureA = MakeArtifact("artifact-swap-a", 2, 2, 1);
            auto fixtureB = MakeArtifact("artifact-swap-b", 2, 2, 9);
            FrozenAssignmentStore store;
            const auto frozen = AcceptFrozen(store, MakeFrozen(fixtureA));
            ConsoleArtifactConsumerV2 consumer(fixtureA.workspace->root.u8string());
            ConsoleUploadJobAuthority authority(
                fixtureA.workspace->root.u8string(), &consumer, &store);
            const auto start = StartCommand(fixtureA, *frozen, Id('5'), Id('6'));
            authority.HandleCommand("client", start, EncodeCommand(start));
            WaitForConsumerAccepted(consumer);
            ReplaceCurrent(fixtureA, fixtureB);
            const auto outcome = WaitForStart(authority, start.uploadJobId);
            Expect((outcome.resultCode ==
                    dspgcs::UploadJobErrorCode::ArtifactCurrentChanged ||
                outcome.resultCode ==
                    dspgcs::UploadJobErrorCode::ArtifactIdentityMismatch) &&
                authority.GetJobCount() == 0,
                "Artifact replacement before lease was not rejected");
        }
        {
            auto fixtureA = MakeArtifact("immutable-a", 2, 2, 2);
            auto fixtureB = MakeArtifact("immutable-b", 2, 2, 17);
            FrozenAssignmentStore store;
            const auto frozenA = AcceptFrozen(store, MakeFrozen(fixtureA));
            ConsoleArtifactConsumerV2 consumer(fixtureA.workspace->root.u8string());
            ConsoleUploadJobAuthority authority(
                fixtureA.workspace->root.u8string(), &consumer, &store);
            const auto start = StartCommand(fixtureA, *frozenA, Id('7'), Id('8'));
            authority.HandleCommand("client", start, EncodeCommand(start));
            const auto ready = WaitForStart(authority, start.uploadJobId);
            Expect(ready.jobState == dspgcs::UploadJobState::Ready,
                "immutable ownership fixture did not reach Ready");
            const auto targetA = authority.FindTargetMetadataForTest(
                start.uploadJobId, 0);
            Expect(targetA.has_value(), "Job A target metadata is missing");
            auto executionViewA = authority.GetExecutionView(start.uploadJobId);
            Expect(executionViewA.has_value() &&
                executionViewA->artifactSnapshot &&
                executionViewA->frozenSnapshot &&
                executionViewA->artifactReadLease &&
                executionViewA->targets.size() == 2,
                "production immutable execution view is incomplete");
            const auto heldArtifactA = executionViewA->artifactSnapshot;
            const auto heldFrozenA = executionViewA->frozenSnapshot;
            const std::string authoritativeDroneA =
                executionViewA->targets.front().droneId;
            executionViewA->targets.front().droneId = "caller-mutated-copy";

            ReplaceCurrent(fixtureA, fixtureB);
            const std::uint64_t generation =
                consumer.RequestValidation(fixtureB.expected, std::chrono::seconds(30));
            ConsoleArtifactV2ValidationResult result;
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(30);
            while (!consumer.PollCompleted(result) &&
                std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Expect(result.generation == generation && result.success,
                "Consumer did not accept replacement Artifact B");

            auto frozenB = MakeFrozen(
                fixtureA, 0, 0, 0, 0, 0x72, "authority-c", 1);
            std::swap(frozenB.matchedMappings[0].droneId,
                frozenB.matchedMappings[1].droneId);
            Expect(store.Accept(std::move(frozenB)).accepted,
                "Frozen B was not accepted after Job A");
            const auto targetAfter = authority.FindTargetMetadataForTest(
                start.uploadJobId, 0);
            const auto executionViewAfter =
                authority.GetExecutionView(start.uploadJobId);
            Expect(targetAfter.has_value() &&
                targetAfter->droneId == targetA->droneId &&
                targetAfter->relativePath == targetA->relativePath &&
                targetAfter->routeArtifactDigest == targetA->routeArtifactDigest,
                "existing Job rebound to latest Artifact or Frozen authority");
            Expect(executionViewAfter.has_value() &&
                executionViewAfter->artifactSnapshot == heldArtifactA &&
                executionViewAfter->frozenSnapshot == heldFrozenA &&
                executionViewAfter->targets.front().droneId == authoritativeDroneA,
                "execution view queried latest authority or exposed mutable Job storage");
        }
    }

    void VerifyMissionPayloadByteAuthority()
    {
        for (const auto item : std::array<std::pair<std::int32_t, std::uint64_t>, 2>{
            std::pair<std::int32_t, std::uint64_t>{1, 38},
            std::pair<std::int32_t, std::uint64_t>{9000, 342000}})
        {
            auto fixture = MakeArtifact(
                "payload-" + std::to_string(item.first), 1, item.first);
            FrozenAssignmentStore store;
            const auto frozen = AcceptFrozen(store, MakeFrozen(fixture));
            ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
            ConsoleUploadJobAuthority authority(
                fixture.workspace->root.u8string(), &consumer, &store);
            const auto start = StartCommand(
                fixture, *frozen, Id(item.first == 1 ? '1' : '3'),
                Id(item.first == 1 ? '2' : '4'));
            authority.HandleCommand("client", start, EncodeCommand(start));
            const auto ready = WaitForStart(
                authority, start.uploadJobId, std::chrono::seconds(120));
            const auto view = authority.GetExecutionView(start.uploadJobId);
            Expect(ready.jobState == dspgcs::UploadJobState::Ready &&
                ready.totalBytes == item.second && view.has_value() &&
                view->targets.size() == 1 &&
                view->targets.front().missionItemCount ==
                    static_cast<std::uint32_t>(item.first) &&
                view->targets.front().missionPayloadByteSize == item.second &&
                view->targets.front().fileByteSize !=
                    static_cast<std::int64_t>(item.second),
                "UploadJob TotalBytes did not use logical Mission item payload bytes");
        }
    }

    bool TryGateOnAnotherThread(const fs::path& root)
    {
        std::promise<bool> promise;
        auto future = promise.get_future();
        std::thread thread([root, promise = std::move(promise)]() mutable
        {
            try
            {
                artifact::ArtifactReadLease gate;
                std::string error;
                bool recovered = false;
                promise.set_value(gate.TryAcquire(root.wstring(), recovered, error) ==
                    artifact::ArtifactReadLeaseAcquireResult::Acquired);
            }
            catch (...)
            {
                promise.set_value(false);
            }
        });
        if (future.wait_for(std::chrono::seconds(5)) !=
            std::future_status::ready)
        {
            thread.detach();
            throw std::runtime_error(
                "Artifact lease probe exceeded its 5 second deadline");
        }
        const bool acquired = future.get();
        thread.join();
        return acquired;
    }

    void VerifyLifecycleAndLease()
    {
        auto fixture = MakeArtifact("lifecycle", 2, 2);
        FrozenAssignmentStore store;
        const auto frozen = AcceptFrozen(store, MakeFrozen(fixture));
        ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
        ConsoleUploadJobAuthority authority(
            fixture.workspace->root.u8string(), &consumer, &store);
        const auto start = StartCommand(fixture, *frozen, Id('9'), Id('a'));
        authority.HandleCommand("client", start, EncodeCommand(start));
        auto snapshot = WaitForStart(authority, start.uploadJobId);
        Expect(!TryGateOnAnotherThread(fixture.workspace->root),
            "Job did not hold the Artifact read lease");
        const auto originalTarget = authority.FindTargetMetadataForTest(
            start.uploadJobId, 0);
        Expect(originalTarget.has_value(), "lifecycle target metadata is missing");

        dspgcs::UploadJobCommandDTO pause;
        pause.clientCommandId = Id('b');
        pause.uploadJobId = start.uploadJobId;
        pause.commandType = dspgcs::UploadJobCommandType::Pause;
        pause.expectedJobRevision = snapshot.jobRevision;
        auto acknowledgement = authority.HandleCommand(
            "client", pause, EncodeCommand(pause));
        Expect(acknowledgement.status ==
            dspgcs::UploadJobCommandAckStatus::Accepted,
            "Pause was rejected");
        dspgcs::UploadJobCommandDTO resume;
        resume.clientCommandId = Id('c');
        resume.uploadJobId = start.uploadJobId;
        resume.commandType = dspgcs::UploadJobCommandType::Resume;
        resume.expectedJobRevision = acknowledgement.authoritativeJobRevision;
        acknowledgement = authority.HandleCommand(
            "client", resume, EncodeCommand(resume));
        Expect(acknowledgement.status ==
            dspgcs::UploadJobCommandAckStatus::Accepted,
            "Resume was rejected");
        dspgcs::UploadJobCommandDTO setConcurrency;
        setConcurrency.clientCommandId = Id('f');
        setConcurrency.uploadJobId = start.uploadJobId;
        setConcurrency.commandType =
            dspgcs::UploadJobCommandType::SetMaxConcurrentUploads;
        setConcurrency.expectedJobRevision =
            acknowledgement.authoritativeJobRevision;
        setConcurrency.requestedMaxConcurrentUploads = 3;
        acknowledgement = authority.HandleCommand(
            "client", setConcurrency, EncodeCommand(setConcurrency));
        const auto targetAfterConcurrency = authority.FindTargetMetadataForTest(
            start.uploadJobId, 0);
        Expect(acknowledgement.status ==
                dspgcs::UploadJobCommandAckStatus::Accepted &&
            acknowledgement.effectiveMaxConcurrentUploads == 3 &&
            targetAfterConcurrency.has_value() &&
            targetAfterConcurrency->routeArtifactDigest ==
                originalTarget->routeArtifactDigest &&
            targetAfterConcurrency->droneId == originalTarget->droneId,
            "SetMaxConcurrentUploads rebound immutable Job authorities");

        auto failed = dspgcs::UploadRouteStateDTO{};
        failed.routeSlotIndex = 0;
        failed.droneId = "drone-0";
        failed.state = dspgcs::UploadRouteState::Failed;
        failed.totalBytes = originalTarget->missionPayloadByteSize;
        failed.errorCode = dspgcs::RouteUploadErrorCode::UploadTimedOut;
        failed.retryable = true;
        std::string error;
        Expect(authority.ApplyExecutorRouteUpdates(
            start.uploadJobId, { failed }, 0, 0, 0, error), error);
        Expect(authority.TransitionJobForExecutor(
            start.uploadJobId, dspgcs::UploadJobState::Failed, error), error);
        Expect(!TryGateOnAnotherThread(fixture.workspace->root),
            "retryable Failed Job did not retain its Artifact read lease");
        const auto* failedSnapshot = authority.FindCurrentSnapshotForTest(
            start.uploadJobId);
        dspgcs::UploadJobCommandDTO retry;
        retry.clientCommandId = Id('d');
        retry.uploadJobId = start.uploadJobId;
        retry.commandType = dspgcs::UploadJobCommandType::RetryFailed;
        retry.expectedJobRevision = failedSnapshot->jobRevision;
        acknowledgement = authority.HandleCommand(
            "client", retry, EncodeCommand(retry));
        Expect(acknowledgement.status ==
            dspgcs::UploadJobCommandAckStatus::Accepted,
            "RetryFailed was rejected");
        const auto targetAfterRetry = authority.FindTargetMetadataForTest(
            start.uploadJobId, 0);
        Expect(targetAfterRetry.has_value() && originalTarget.has_value() &&
            targetAfterRetry->routeArtifactDigest ==
                originalTarget->routeArtifactDigest &&
            targetAfterRetry->droneId == originalTarget->droneId,
            "Retry rebound immutable Job authorities");

        const auto* retrySnapshot = authority.FindCurrentSnapshotForTest(
            start.uploadJobId);
        Expect(authority.TransitionJobForExecutor(
            start.uploadJobId, dspgcs::UploadJobState::Running, error), error);
        auto uploading = dspgcs::UploadRouteStateDTO{};
        uploading.routeSlotIndex = 0;
        uploading.droneId = originalTarget->droneId;
        uploading.state = dspgcs::UploadRouteState::Uploading;
        uploading.totalBytes = originalTarget->missionPayloadByteSize;
        uploading.bytesCompleted = 1;
        Expect(authority.ApplyExecutorRouteUpdates(
            start.uploadJobId, { uploading }, 10, 1000, 1, error), error);
        retrySnapshot = authority.FindCurrentSnapshotForTest(start.uploadJobId);
        dspgcs::UploadJobCommandDTO cancel;
        cancel.clientCommandId = Id('e');
        cancel.uploadJobId = start.uploadJobId;
        cancel.commandType = dspgcs::UploadJobCommandType::Cancel;
        cancel.expectedJobRevision = retrySnapshot->jobRevision;
        acknowledgement = authority.HandleCommand(
            "client", cancel, EncodeCommand(cancel));
        Expect(acknowledgement.status ==
                dspgcs::UploadJobCommandAckStatus::Accepted &&
            authority.IsCancellationRequested(start.uploadJobId),
            "Cancel did not publish a cancellation request");
        Expect(!TryGateOnAnotherThread(fixture.workspace->root),
            "Cancel request prematurely released the Artifact read lease");
        Expect(authority.FinalizeExecutorCancellation(
            start.uploadJobId, error), error);
        Expect(!authority.IsCancellationRequested(start.uploadJobId) &&
            TryGateOnAnotherThread(fixture.workspace->root),
            "quiesced cancellation did not release the Artifact read lease");
        Expect(authority.FinalizeExecutorCancellation(
            start.uploadJobId, error),
            "repeated cancellation finalization was not idempotent");

        {
            auto idleCancelFixture = MakeArtifact("idle-cancel", 1, 1);
            FrozenAssignmentStore idleCancelStore;
            const auto idleCancelFrozen = AcceptFrozen(
                idleCancelStore, MakeFrozen(idleCancelFixture));
            ConsoleArtifactConsumerV2 idleCancelConsumer(
                idleCancelFixture.workspace->root.u8string());
            ConsoleUploadJobAuthority idleCancelAuthority(
                idleCancelFixture.workspace->root.u8string(),
                &idleCancelConsumer, &idleCancelStore);
            const auto idleStart = StartCommand(
                idleCancelFixture, *idleCancelFrozen, Id('5'), Id('6'));
            idleCancelAuthority.HandleCommand(
                "client", idleStart, EncodeCommand(idleStart));
            const auto idleReady = WaitForStart(
                idleCancelAuthority, idleStart.uploadJobId);
            dspgcs::UploadJobCommandDTO idleCancel;
            idleCancel.clientCommandId = Id('7');
            idleCancel.uploadJobId = idleStart.uploadJobId;
            idleCancel.commandType = dspgcs::UploadJobCommandType::Cancel;
            idleCancel.expectedJobRevision = idleReady.jobRevision;
            idleCancelAuthority.HandleCommand(
                "client", idleCancel, EncodeCommand(idleCancel));
            Expect(idleCancelAuthority.IsCancellationRequested(
                idleStart.uploadJobId),
                "Cancel without an active executor was not observable");
            Expect(idleCancelAuthority.FinalizeExecutorCancellation(
                idleStart.uploadJobId, error), error);
            Expect(TryGateOnAnotherThread(idleCancelFixture.workspace->root),
                "idle cancellation finalization retained its lease");
        }
        {
            auto completedFixture = MakeArtifact("completed-lease", 1, 1);
            FrozenAssignmentStore completedStore;
            const auto completedFrozen = AcceptFrozen(
                completedStore, MakeFrozen(completedFixture));
            ConsoleArtifactConsumerV2 completedConsumer(
                completedFixture.workspace->root.u8string());
            ConsoleUploadJobAuthority completedAuthority(
                completedFixture.workspace->root.u8string(),
                &completedConsumer, &completedStore);
            const auto completedStart = StartCommand(
                completedFixture, *completedFrozen, Id('1'), Id('2'));
            completedAuthority.HandleCommand(
                "client", completedStart, EncodeCommand(completedStart));
            WaitForStart(completedAuthority, completedStart.uploadJobId);
            Expect(completedAuthority.TransitionJobForExecutor(
                completedStart.uploadJobId,
                dspgcs::UploadJobState::Running, error), error);
            Expect(completedAuthority.TransitionJobForExecutor(
                completedStart.uploadJobId,
                dspgcs::UploadJobState::Completed, error), error);
            Expect(TryGateOnAnotherThread(completedFixture.workspace->root),
                "Completed Job did not release the Artifact read lease");
        }
        {
            auto destroyFixture = MakeArtifact("destroy-failed-lease", 1, 1);
            FrozenAssignmentStore destroyStore;
            const auto destroyFrozen = AcceptFrozen(
                destroyStore, MakeFrozen(destroyFixture));
            ConsoleArtifactConsumerV2 destroyConsumer(
                destroyFixture.workspace->root.u8string());
            ConsoleUploadJobAuthority destroyAuthority(
                destroyFixture.workspace->root.u8string(),
                &destroyConsumer, &destroyStore);
            const auto destroyStart = StartCommand(
                destroyFixture, *destroyFrozen, Id('3'), Id('4'));
            destroyAuthority.HandleCommand(
                "client", destroyStart, EncodeCommand(destroyStart));
            WaitForStart(destroyAuthority, destroyStart.uploadJobId);
            Expect(destroyAuthority.TransitionJobForExecutor(
                destroyStart.uploadJobId,
                dspgcs::UploadJobState::Running, error), error);
            Expect(destroyAuthority.TransitionJobForExecutor(
                destroyStart.uploadJobId,
                dspgcs::UploadJobState::Failed, error), error);
            Expect(!TryGateOnAnotherThread(destroyFixture.workspace->root),
                "Failed Job did not retain its Artifact read lease");
            Expect(destroyAuthority.DestroyFailedJob(destroyStart.uploadJobId),
                "DestroyFailed rejected a Failed Job");
            Expect(TryGateOnAnotherThread(destroyFixture.workspace->root),
                "DestroyFailed did not release the Artifact read lease");
        }
        {
            auto shutdownFixture = MakeArtifact("shutdown-cancel", 1, 1);
            FrozenAssignmentStore shutdownStore;
            const auto shutdownFrozen = AcceptFrozen(
                shutdownStore, MakeFrozen(shutdownFixture));
            ConsoleArtifactConsumerV2 shutdownConsumer(
                shutdownFixture.workspace->root.u8string());
            {
                ConsoleUploadJobAuthority shutdownAuthority(
                    shutdownFixture.workspace->root.u8string(),
                    &shutdownConsumer, &shutdownStore);
                const auto shutdownStart = StartCommand(
                    shutdownFixture, *shutdownFrozen, Id('7'), Id('8'));
                shutdownAuthority.HandleCommand(
                    "client", shutdownStart, EncodeCommand(shutdownStart));
                const auto shutdownReady = WaitForStart(
                    shutdownAuthority, shutdownStart.uploadJobId);
                dspgcs::UploadJobCommandDTO shutdownCancel;
                shutdownCancel.clientCommandId = Id('9');
                shutdownCancel.uploadJobId = shutdownStart.uploadJobId;
                shutdownCancel.commandType = dspgcs::UploadJobCommandType::Cancel;
                shutdownCancel.expectedJobRevision = shutdownReady.jobRevision;
                shutdownAuthority.HandleCommand(
                    "client", shutdownCancel, EncodeCommand(shutdownCancel));
                Expect(!TryGateOnAnotherThread(shutdownFixture.workspace->root),
                    "Cancelling Job did not retain lease before shutdown");
            }
            Expect(TryGateOnAnotherThread(shutdownFixture.workspace->root),
                "Authority shutdown did not clean up a Cancelling Job lease");
        }
    }

    struct ScaleMetric
    {
        std::size_t targetCount = 0;
        std::size_t approximateBytes = 0;
        std::size_t snapshotBytes = 0;
        std::size_t pageBytes = 0;
        double seconds = 0.0;
    };

    ScaleMetric VerifyMaterializationScale(
        const std::int32_t routeCount,
        const char idCharacter)
    {
        auto fixture = MakeArtifact(
            "scale-" + std::to_string(routeCount), routeCount, 1);
        FrozenAssignmentStore store;
        const auto frozen = AcceptFrozen(store, MakeFrozen(fixture));
        ConsoleArtifactConsumerV2 consumer(fixture.workspace->root.u8string());
        ConsoleUploadJobAuthority authority(
            fixture.workspace->root.u8string(), &consumer, &store);
        const auto start = StartCommand(
            fixture, *frozen, Id(idCharacter), Id(idCharacter + 1), 32);
        const auto started = std::chrono::steady_clock::now();
        authority.HandleCommand("scale-client", start, EncodeCommand(start));
        const auto snapshot = WaitForStart(
            authority, start.uploadJobId, std::chrono::seconds(120));
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        Expect(snapshot.resultCode == dspgcs::UploadJobErrorCode::None &&
            snapshot.matchedCount == static_cast<std::uint32_t>(routeCount) &&
            snapshot.waitingCount == static_cast<std::uint32_t>(routeCount),
            "scale materialization target count mismatch");
        std::vector<std::uint8_t> snapshotBytes;
        std::string error;
        Expect(dspgcs::EncodeUploadJobSnapshot(
            snapshot, snapshotBytes, error), error);
        dspgcs::UploadJobSnapshotRequestDTO pageRequest;
        pageRequest.clientRequestId = Id('f');
        pageRequest.uploadJobId = start.uploadJobId;
        pageRequest.requestKind = dspgcs::UploadJobSnapshotRequestKind::RoutePage;
        pageRequest.snapshotToken = snapshot.snapshotToken;
        pageRequest.maximumRouteEntries = 20000;
        dspgcs::UploadJobRouteStatePageDTO page;
        dspgcs::UploadJobErrorCode pageError;
        Expect(authority.BuildRoutePage(pageRequest, page, pageError),
            "scale route page build failed");
        std::vector<std::uint8_t> pageBytes;
        Expect(dspgcs::EncodeUploadJobRouteStatePage(page, pageBytes, error), error);
        ScaleMetric metric;
        metric.targetCount = routeCount;
        metric.approximateBytes = static_cast<std::size_t>(routeCount) *
            (sizeof(UploadJobTargetMetadata) +
                sizeof(dspgcs::UploadRouteStateDTO) + 64);
        metric.snapshotBytes = snapshotBytes.size();
        metric.pageBytes = pageBytes.size();
        metric.seconds = seconds;
        return metric;
    }

    void VerifyScaleAndSnapshotDelta()
    {
        const auto thousand = VerifyMaterializationScale(1000, '4');
        const auto tenThousand = VerifyMaterializationScale(10000, '6');
        Expect(tenThousand.approximateBytes < thousand.approximateBytes * 11,
            "10K materialization memory estimate is not O(N)");

        TempRoot root("existing-20k-state");
        dspgcs::UploadArtifactBindingDTO artifactBinding;
        artifactBinding.artifactSetId = std::string(64, 'a');
        artifactBinding.artifactSetDigest.fill(1);
        dspgcs::UploadFrozenAssignmentBindingDTO frozenBinding;
        frozenBinding.fleetSessionId = "fleet-scale";
        frozenBinding.assignmentAuthoritySessionId = "authority-scale";
        frozenBinding.assignmentRevision = 1;
        frozenBinding.assignmentDigest.fill(2);
        dspgcs::UploadStableSourceDTO stableSource;
        stableSource.sourceDatasetDigest.fill(3);
        stableSource.normalizedShowOriginLatitudeE7 = 300000000;
        stableSource.normalizedShowOriginLongitudeE7 = 1200000000;
        stableSource.routeSlotCount = 20000;
        std::vector<dspgcs::UploadRouteStateDTO> routes;
        routes.reserve(20000);
        for (std::int32_t slot = 0; slot < 20000; ++slot)
        {
            dspgcs::UploadRouteStateDTO route;
            route.routeSlotIndex = slot;
            route.droneId = "drone-" + std::to_string(slot);
            route.totalBytes = 4096;
            routes.push_back(std::move(route));
        }
        ConsoleUploadJobAuthority authority(root.root.u8string(), nullptr, nullptr);
        std::string error;
        Expect(authority.InstallSyntheticReadyJobForTest(
            Id('8'), artifactBinding, frozenBinding, stableSource,
            routes, 0, 0, 0, 32, error), error);
		dspgcs::UploadJobSnapshotRequestDTO aggregateRequest;
		aggregateRequest.clientRequestId = Id('9');
		aggregateRequest.uploadJobId = Id('8');
		aggregateRequest.requestKind =
			dspgcs::UploadJobSnapshotRequestKind::Aggregate;
		const auto frozenAggregate = authority.BuildSnapshot(aggregateRequest);
		Expect(authority.GetFrozenPageRouteCountForTest(Id('8')) == 20000,
			"aggregate did not freeze its 20K Route page snapshot");
        routes.front().state = dspgcs::UploadRouteState::Uploading;
        routes.front().bytesCompleted = 10;
        Expect(authority.ApplyExecutorRouteUpdates(
            Id('8'), { routes.front() }, 100, 1000, 10, error), error);
		dspgcs::UploadJobSnapshotRequestDTO frozenPageRequest;
		frozenPageRequest.clientRequestId = Id('a');
		frozenPageRequest.uploadJobId = Id('8');
		frozenPageRequest.requestKind =
			dspgcs::UploadJobSnapshotRequestKind::RoutePage;
		frozenPageRequest.snapshotToken = frozenAggregate.snapshotToken;
		frozenPageRequest.maximumRouteEntries = 20000;
		dspgcs::UploadJobRouteStatePageDTO frozenPage;
		dspgcs::UploadJobErrorCode frozenPageError;
		Expect(authority.BuildRoutePage(
			frozenPageRequest, frozenPage, frozenPageError),
			"revision progress invalidated the frozen Aggregate Route page");
		Expect(!frozenPage.routes.empty() &&
			frozenPage.routes.front().state == dspgcs::UploadRouteState::Waiting,
			"frozen Route page drifted after executor revision progress");
        const auto deltas = authority.DrainRouteDeltas();
        Expect(deltas.size() == 1 && deltas.front().routes.size() == 1,
            "existing 20K state Delta regression failed");

		auto failedFive = routes[5];
		failedFive.state = dspgcs::UploadRouteState::Failed;
		failedFive.errorCode = dspgcs::RouteUploadErrorCode::UploadRejected;
		failedFive.retryable = true;
		failedFive.updatedAtUtcMs = 50;
		failedFive.safeFailureSummary = "later failure";
		auto failedTwo = routes[2];
		failedTwo.state = dspgcs::UploadRouteState::Failed;
		failedTwo.errorCode = dspgcs::RouteUploadErrorCode::UploadTimedOut;
		failedTwo.retryable = true;
		failedTwo.updatedAtUtcMs = 20;
		failedTwo.safeFailureSummary = "deterministic first failure";
		Expect(authority.ApplyExecutorRouteUpdates(Id('8'),
			{ failedFive, failedTwo }, 0, 1000, 20, error), error);
		const auto* failedAggregate = authority.FindCurrentSnapshotForTest(Id('8'));
		Expect(failedAggregate && failedAggregate->firstFailure.routeSlotIndex == 2 &&
			failedAggregate->firstFailure.droneId == failedTwo.droneId &&
			failedAggregate->firstFailure.routeErrorCode ==
				dspgcs::RouteUploadErrorCode::UploadTimedOut &&
			failedAggregate->firstFailure.safeSummary == failedTwo.safeFailureSummary,
			"deterministic authoritative firstFailure was not materialized");

        std::cout << "UploadJob v2-E materialization scale PASS\n"
            << "routes=" << thousand.targetCount
            << " seconds=" << thousand.seconds
            << " approxBytes=" << thousand.approximateBytes
            << " snapshotBytes=" << thousand.snapshotBytes
            << " pageBytes=" << thousand.pageBytes << '\n'
            << "routes=" << tenThousand.targetCount
            << " seconds=" << tenThousand.seconds
            << " approxBytes=" << tenThousand.approximateBytes
            << " snapshotBytes=" << tenThousand.snapshotBytes
            << " pageBytes=" << tenThousand.pageBytes << '\n'
            << "routes=20000 existingStateRegression=PASS\n";
    }
}

int RunDSPUploadJobProtocolTests()
{
    try
    {
        VerifyWireContract();
        VerifyCompatibilityGate();
        VerifyMissingRouteRecord();
        VerifyToctouAndImmutableOwnership();
        VerifyMissionPayloadByteAuthority();
        VerifyLifecycleAndLease();
        VerifyScaleAndSnapshotDelta();
        std::cout << "DSP Upload Job schema2 dual-binding tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "DSP Upload Job schema2 tests failed: "
            << error.what() << '\n';
        return 1;
    }
}
