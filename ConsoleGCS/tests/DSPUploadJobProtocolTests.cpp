#define WIN32_LEAN_AND_MEAN
#include "ConsoleUploadJobAuthority.h"
#include "DSPGCSUploadJobReceiver.h"
#include "RouteArtifactContract.h"
#include "RouteArtifactReadLease.h"
#include "Wpl110Core.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace artifact = dronegcs::route_artifact;
    namespace wpl = dronegcs::wpl110;

    void Expect(const bool value, const std::string& message)
    {
        if (!value) throw std::runtime_error(message);
    }

    std::string Id(const char value) { return std::string(32, value); }

    std::string NumericId(const std::uint64_t value)
    {
        std::ostringstream stream;
        stream << std::hex << std::setfill('0') << std::setw(32) << value;
        return stream.str();
    }

    artifact::Digest256 Hash(const std::string& text)
    {
        artifact::Digest256 value;
        Expect(artifact::Sha256::Hash(text.data(), text.size(), value), "hash failed");
        return value;
    }

    dspgcs::UploadJobDigest WireDigest(const artifact::Digest256& source)
    {
        dspgcs::UploadJobDigest result{};
        std::copy(source.Bytes.begin(), source.Bytes.end(), result.begin());
        return result;
    }

    class TempRoot final
    {
    public:
        explicit TempRoot(const char* tag)
        {
            static std::atomic<unsigned> counter{ 0 };
            root = fs::temp_directory_path() /
                (std::string("DroneGCS-2GB-") + tag + "-" +
                 std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(++counter));
            fs::create_directories(root / "Current");
        }
        ~TempRoot() { std::error_code error; fs::remove_all(root, error); }
        fs::path root;
    };

    void Write(const fs::path& path, const std::string& bytes)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Expect(stream.good(), "fixture open failed");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Expect(stream.good(), "fixture write failed");
    }

    std::string RouteBytes()
    {
        wpl::Row row;
        row.Sequence = 0; row.Red = 1; row.Green = 2; row.Blue = 3;
        row.LatitudeE7 = 399000000; row.LongitudeE7 = 1163000000;
        row.AltitudeE7 = 500000000;
        wpl::FormattedRow formatted;
        Expect(wpl::FormatRow(row, formatted), "row format failed");
        return std::string(wpl::Header) + "\r\n" +
            std::string(reinterpret_cast<const char*>(formatted.Bytes.data()), formatted.Size);
    }

    struct ArtifactFixture
    {
        std::unique_ptr<TempRoot> root;
        artifact::Manifest manifest;
        std::shared_ptr<FrozenAssignmentAcceptedSnapshot> frozen;
        dspgcs::UploadArtifactIdentityDTO wireIdentity;
    };

    ArtifactFixture MakeArtifact(const char* tag)
    {
        ArtifactFixture fixture;
        fixture.root = std::make_unique<TempRoot>(tag);
        auto& manifest = fixture.manifest;
        manifest.CreatedAtUtc = 1700000000000LL;
        manifest.Assignment.FleetSessionId = "fleet-2gb";
        manifest.Assignment.AssignmentAuthoritySessionId = "authority-2gb";
        manifest.Assignment.AssignmentRevision = 7;
        manifest.Assignment.AssignmentDigest = Hash("assignment-2gb");
        manifest.FleetRevisionAtCapture = 9;
        manifest.RouteDatasetGeneration = 11;
        manifest.MatchedCount = 1; manifest.RouteSlotCount = 1; manifest.FrameCount = 1;
        manifest.Source.SourceDatasetDigest = Hash("source-2gb");
        manifest.Source.SourceByteSize = 100;
        manifest.Source.SourceDisplayFileName = "source.npz";
        manifest.WaypointGeneratorVersion = "DroneGCS-WPL110-v1";
        manifest.GeneratorInputDigest = Hash("generator-2gb");
        manifest.Mapping.push_back({ "drone-0", 0 });
        std::string relative; artifact::Error error;
        Expect(artifact::BuildRouteRelativePath(0, relative, error), "route path failed");
        const std::string route = RouteBytes();
        Write(fixture.root->root / "Current" / fs::u8path(relative), route);
        manifest.Routes.push_back({ 0, relative,
            static_cast<std::int64_t>(route.size()), Hash(route) });
        Expect(artifact::ComputeArtifactSetId(
            artifact::BuildIdentityFromManifest(manifest), manifest.ArtifactSetId, error),
            "set id failed");
        Expect(artifact::ComputeArtifactSetDigest(manifest, manifest.ArtifactSetDigest, error),
            "set digest failed");
        std::string manifestBytes, readyBytes;
        Expect(artifact::SerializeCanonicalManifest(manifest, manifestBytes, error),
            "manifest failed");
        Expect(artifact::SerializeReadyMarker(manifest.ArtifactSetId,
            manifest.ArtifactSetDigest, readyBytes, error), "ready failed");
        Write(fixture.root->root / "Current" / "manifest.json", manifestBytes);
        Write(fixture.root->root / "Current" / "READY", readyBytes);

        fixture.frozen = std::make_shared<FrozenAssignmentAcceptedSnapshot>();
        fixture.frozen->identity.fleetSessionId = manifest.Assignment.FleetSessionId;
        fixture.frozen->identity.assignmentAuthoritySessionId =
            manifest.Assignment.AssignmentAuthoritySessionId;
        fixture.frozen->identity.assignmentRevision = 7;
        fixture.frozen->fleetRevisionAtCapture = 9;
        fixture.frozen->routeDatasetGeneration = 11;
        fixture.frozen->routeSlotCount = 1; fixture.frozen->matchedCount = 1;
        fixture.frozen->matchedMappings.push_back({ "drone-0", 0 });
        std::copy(manifest.Assignment.AssignmentDigest.Bytes.begin(),
            manifest.Assignment.AssignmentDigest.Bytes.end(),
            fixture.frozen->canonicalPayloadDigest.begin());

        fixture.wireIdentity.artifactSetId = manifest.ArtifactSetId;
        fixture.wireIdentity.artifactSetDigest = WireDigest(manifest.ArtifactSetDigest);
        fixture.wireIdentity.fleetSessionId = manifest.Assignment.FleetSessionId;
        fixture.wireIdentity.assignmentAuthoritySessionId =
            manifest.Assignment.AssignmentAuthoritySessionId;
        fixture.wireIdentity.assignmentRevision = 7;
        fixture.wireIdentity.assignmentDigest = WireDigest(manifest.Assignment.AssignmentDigest);
        return fixture;
    }

    void ReplaceCurrentWithDifferentIdentity(ArtifactFixture& fixture)
    {
        fixture.manifest.CreatedAtUtc += 1;
        artifact::Error error;
        Expect(artifact::ComputeArtifactSetId(
            artifact::BuildIdentityFromManifest(fixture.manifest),
            fixture.manifest.ArtifactSetId, error), "replacement set id failed");
        Expect(artifact::ComputeArtifactSetDigest(
            fixture.manifest, fixture.manifest.ArtifactSetDigest, error),
            "replacement set digest failed");
        std::string manifestBytes, readyBytes;
        Expect(artifact::SerializeCanonicalManifest(
            fixture.manifest, manifestBytes, error), "replacement manifest failed");
        Expect(artifact::SerializeReadyMarker(
            fixture.manifest.ArtifactSetId,
            fixture.manifest.ArtifactSetDigest, readyBytes, error),
            "replacement READY failed");
        Write(fixture.root->root / "Current" / "manifest.json", manifestBytes);
        Write(fixture.root->root / "Current" / "READY", readyBytes);
    }

    dspgcs::UploadJobCommandDTO StartCommand(
        const ArtifactFixture& fixture,
        const std::string& jobId = Id('1'),
        const std::string& commandId = Id('2'))
    {
        dspgcs::UploadJobCommandDTO command;
        command.clientCommandId = commandId;
        command.uploadJobId = jobId;
        command.commandType = dspgcs::UploadJobCommandType::Start;
        command.requestedMaxConcurrentUploads = 8;
        command.artifactIdentity = fixture.wireIdentity;
        return command;
    }

    std::vector<std::uint8_t> EncodeCommand(const dspgcs::UploadJobCommandDTO& command)
    {
        std::vector<std::uint8_t> bytes; std::string error;
        Expect(dspgcs::EncodeUploadJobCommand(command, bytes, error), error);
        return bytes;
    }

    void AcceptFrozen(FrozenAssignmentStore& store, const ArtifactFixture& fixture)
    {
        store.SynchronizeFleetSession(fixture.frozen->identity.fleetSessionId);
        Expect(store.Accept(*fixture.frozen).accepted, "frozen assignment not accepted");
    }

    void WaitForValidation(ConsoleUploadJobAuthority& authority, const std::string& jobId)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            authority.Tick();
            const auto* snapshot = authority.FindCurrentSnapshotForTest(jobId);
            if (snapshot && snapshot->jobState != dspgcs::UploadJobState::Validating) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        throw std::runtime_error("Artifact validation timed out");
    }

    void VerifyWireContract()
    {
        auto fixture = MakeArtifact("wire");
        const auto command = StartCommand(fixture);
        std::vector<std::uint8_t> encoded; std::string error;
        Expect(dspgcs::EncodeUploadJobCommand(command, encoded, error), error);
        dspgcs::UploadJobCommandDTO decoded;
        Expect(dspgcs::DecodeUploadJobCommand(encoded, decoded, error), error);
        Expect(decoded.clientCommandId == command.clientCommandId &&
            dspgcs::UploadArtifactIdentityEqual(decoded.artifactIdentity,
                command.artifactIdentity), "command round trip changed identity");
        auto malformed = command;
        malformed.clientCommandId = "short";
        Expect(!dspgcs::EncodeUploadJobCommand(malformed, encoded, error),
            "noncanonical command id accepted");
        malformed = command; malformed.clientCommandId[0] = 'A';
        Expect(!dspgcs::EncodeUploadJobCommand(malformed, encoded, error),
            "uppercase command id accepted");
        malformed = command; malformed.payloadSchemaVersion = 2;
        Expect(!dspgcs::EncodeUploadJobCommand(malformed, encoded, error),
            "version mismatch accepted");
        std::vector<std::uint8_t> oversized(dspgcs::MaxFrameBytes + 1, 0);
        dspgcs::MessageEnvelope envelope; std::vector<std::uint8_t> payload;
        Expect(!dspgcs::DecodeEnvelope(oversized, envelope, payload),
            "oversize frame accepted");
        Expect(static_cast<std::uint16_t>(dspgcs::UploadJobMessageTypes::Command) == 9 &&
            static_cast<std::uint16_t>(dspgcs::UploadJobMessageTypes::RouteStateDelta) == 14,
            "message type values changed");
    }

    void VerifyAuthorityAndArtifactGate()
    {
        auto fixture = MakeArtifact("authority");
        FrozenAssignmentStore store; AcceptFrozen(store, fixture);
        ConsoleArtifactConsumer consumer(fixture.root->root.u8string());
        ConsoleUploadJobAuthority authority(
            fixture.root->root.u8string(), &consumer, &store);
        auto start = StartCommand(fixture);
        const auto payload = EncodeCommand(start);
        const auto first = authority.HandleCommand("client-session", start, payload);
        Expect(first.status == dspgcs::UploadJobCommandAckStatus::Accepted,
            "valid Start was rejected");
        const auto duplicate = authority.HandleCommand("client-session", start, payload);
        Expect(duplicate.authoritativeJobRevision == first.authoritativeJobRevision &&
            authority.GetJobCount() == 1, "duplicate Start was not idempotent");
        auto conflict = start; conflict.requestedMaxConcurrentUploads = 9;
        const auto conflictAck = authority.HandleCommand(
            "client-session", conflict, EncodeCommand(conflict));
        Expect(conflictAck.resultCode == dspgcs::UploadJobErrorCode::IdempotencyConflict,
            "ClientCommandID conflict not rejected");
        WaitForValidation(authority, start.uploadJobId);
        const auto* ready = authority.FindCurrentSnapshotForTest(start.uploadJobId);
        Expect(ready && ready->jobState == dspgcs::UploadJobState::Ready &&
            ready->artifactGateState == dspgcs::UploadArtifactGateState::Validated,
            "valid Artifact did not reach Ready");

        dspgcs::UploadJobCommandDTO pause;
        pause.clientCommandId = Id('9');
        pause.uploadJobId = start.uploadJobId;
        pause.commandType = dspgcs::UploadJobCommandType::Pause;
        pause.expectedJobRevision = ready->jobRevision;
        const auto pausePayload = EncodeCommand(pause);
        dspgcs::MessageEnvelope pauseEnvelope;
        pauseEnvelope.messageType = dspgcs::UploadJobMessageTypes::Command;
        pauseEnvelope.sourceId = "UE-test";
        pauseEnvelope.sessionId = "loss-retry-client";
        pauseEnvelope.correlationId = pause.clientCommandId;
        dspgcs::TransportFrame pauseFrame{ dspgcs::MakeFrame(pauseEnvelope, pausePayload) };
        DSPGCSUploadJobReceiver receiver;
        // The first send is deliberately lost. The same encoded command is retried.
        receiver.EnqueueFrame(pauseFrame);
        std::vector<dspgcs::UploadJobCommandAckDTO> receivedAcks;
        receiver.ProcessPending(authority,
            [&receivedAcks](const auto& ack) { receivedAcks.push_back(ack); return true; },
            {}, {});
        receiver.EnqueueFrame(pauseFrame);
        receiver.ProcessPending(authority,
            [&receivedAcks](const auto& ack) { receivedAcks.push_back(ack); return true; },
            {}, {});
        Expect(receivedAcks.size() == 2 &&
            receivedAcks[0].authoritativeJobRevision ==
                receivedAcks[1].authoritativeJobRevision,
            "lost/retried or duplicate command changed authority twice");

        auto collision = StartCommand(fixture, start.uploadJobId, Id('3'));
        collision.artifactIdentity.artifactSetDigest[0] ^= 1;
        const auto collisionAck = authority.HandleCommand(
            "client-session", collision, EncodeCommand(collision));
        Expect(collisionAck.resultCode == dspgcs::UploadJobErrorCode::JobIdCollision,
            "JobID collision not rejected");

        auto stale = start; stale.clientCommandId = Id('4');
        stale.commandType = dspgcs::UploadJobCommandType::Pause;
        stale.expectedJobRevision = ready->jobRevision - 1;
        stale.requestedMaxConcurrentUploads = 0;
        stale.artifactIdentity = {};
        const auto staleAck = authority.HandleCommand(
            "client-session", stale, EncodeCommand(stale));
        Expect(staleAck.resultCode == dspgcs::UploadJobErrorCode::StaleJobRevision,
            "stale revision accepted");

        const auto* paused = authority.FindCurrentSnapshotForTest(start.uploadJobId);
        dspgcs::UploadJobCommandDTO later;
        later.clientCommandId = Id('c'); later.uploadJobId = start.uploadJobId;
        later.commandType = dspgcs::UploadJobCommandType::SetMaxConcurrentUploads;
        later.expectedJobRevision = paused->jobRevision;
        later.requestedMaxConcurrentUploads = 4;
        dspgcs::UploadJobCommandDTO earlier;
        earlier.clientCommandId = Id('d'); earlier.uploadJobId = start.uploadJobId;
        earlier.commandType = dspgcs::UploadJobCommandType::Resume;
        earlier.expectedJobRevision = paused->jobRevision;
        Expect(authority.HandleCommand("reordered-client", later,
            EncodeCommand(later)).status ==
            dspgcs::UploadJobCommandAckStatus::Accepted,
            "later reordered command was not accepted");
        Expect(authority.HandleCommand("reordered-client", earlier,
            EncodeCommand(earlier)).resultCode ==
            dspgcs::UploadJobErrorCode::StaleJobRevision,
            "reordered earlier command did not become stale");

        ConsoleUploadJobAuthority missing(fixture.root->root.u8string(), nullptr, &store);
        auto missingStart = StartCommand(fixture, Id('5'), Id('6'));
        Expect(missing.HandleCommand("client", missingStart, EncodeCommand(missingStart)).resultCode ==
            dspgcs::UploadJobErrorCode::ArtifactNotValidated,
            "missing Artifact authority accepted");
        FrozenAssignmentStore emptyStore;
        emptyStore.SynchronizeFleetSession(fixture.wireIdentity.fleetSessionId);
        ConsoleUploadJobAuthority frozenMismatch(
            fixture.root->root.u8string(), &consumer, &emptyStore);
        auto frozenStart = StartCommand(fixture, Id('7'), Id('8'));
        Expect(frozenMismatch.HandleCommand("client", frozenStart,
            EncodeCommand(frozenStart)).resultCode ==
            dspgcs::UploadJobErrorCode::FrozenAssignmentMismatch,
            "Frozen Assignment mismatch accepted");

        {
            auto identityFixture = MakeArtifact("identity-mismatch");
            FrozenAssignmentStore identityStore; AcceptFrozen(identityStore, identityFixture);
            ConsoleArtifactConsumer identityConsumer(identityFixture.root->root.u8string());
            ConsoleUploadJobAuthority identityAuthority(identityFixture.root->root.u8string(),
                &identityConsumer, &identityStore);
            auto identityStart = StartCommand(identityFixture, Id('0'), Id('1'));
            identityStart.artifactIdentity.artifactSetDigest[0] ^= 1;
            Expect(identityAuthority.HandleCommand("identity-client", identityStart,
                EncodeCommand(identityStart)).status ==
                dspgcs::UploadJobCommandAckStatus::Accepted,
                "identity mismatch did not enter asynchronous validation");
            WaitForValidation(identityAuthority, identityStart.uploadJobId);
            const auto* rejected = identityAuthority.FindCurrentSnapshotForTest(
                identityStart.uploadJobId);
            Expect(rejected && rejected->jobState == dspgcs::UploadJobState::Failed &&
                rejected->firstFailure.jobErrorCode ==
                    dspgcs::UploadJobErrorCode::ArtifactIdentityMismatch,
                "Artifact identity mismatch was not rejected authoritatively");
        }

        {
            auto replacedFixture = MakeArtifact("current-replaced");
            FrozenAssignmentStore replacedStore; AcceptFrozen(replacedStore, replacedFixture);
            ConsoleArtifactConsumer replacedConsumer(replacedFixture.root->root.u8string());
            ConsoleUploadJobAuthority replacedAuthority(replacedFixture.root->root.u8string(),
                &replacedConsumer, &replacedStore);
            const auto replacedStart = StartCommand(replacedFixture, Id('2'), Id('3'));
            ReplaceCurrentWithDifferentIdentity(replacedFixture);
            Expect(replacedAuthority.HandleCommand("replaced-client", replacedStart,
                EncodeCommand(replacedStart)).status ==
                dspgcs::UploadJobCommandAckStatus::Accepted,
                "replaced Current did not enter asynchronous validation");
            WaitForValidation(replacedAuthority, replacedStart.uploadJobId);
            const auto* rejected = replacedAuthority.FindCurrentSnapshotForTest(
                replacedStart.uploadJobId);
            Expect(rejected && rejected->jobState == dspgcs::UploadJobState::Failed &&
                rejected->firstFailure.jobErrorCode ==
                    dspgcs::UploadJobErrorCode::ArtifactIdentityMismatch,
                "Current replaced before Start was not rejected");
        }

        ConsoleUploadJobAuthority bounded(fixture.root->root.u8string(), nullptr, nullptr);
        for (std::uint64_t index = 1; index <= 1100; ++index)
        {
            dspgcs::UploadJobCommandDTO command;
            command.clientCommandId = NumericId(index);
            command.uploadJobId = Id('f');
            command.commandType = dspgcs::UploadJobCommandType::Pause;
            command.expectedJobRevision = 1;
            bounded.HandleCommand("bounded-client", command, EncodeCommand(command));
        }
        Expect(bounded.GetIdempotencyEntryCount() <=
            ConsoleUploadJobAuthority::MaxIdempotencyEntries,
            "idempotency cache grew without bound");
    }

    bool TryGateOnAnotherThread(const fs::path& root, bool& recovered)
    {
        bool acquired = false;
        std::thread thread([&]
        {
            dronegcs::route_artifact::ArtifactReadLease gate;
            std::string error; bool localRecovered = false;
            acquired = gate.TryAcquire(root.wstring(), localRecovered, error) ==
                artifact::ArtifactReadLeaseAcquireResult::Acquired;
            recovered = localRecovered;
        });
        thread.join();
        return acquired;
    }

    void VerifyLeaseLifecycle()
    {
        TempRoot root("lease");
        dspgcs::UploadArtifactIdentityDTO identity;
        identity.artifactSetId = std::string(64, 'a');
        identity.fleetSessionId = "fleet";
        identity.assignmentAuthoritySessionId = "authority";
        identity.assignmentRevision = 1;
        std::vector<dspgcs::UploadRouteStateDTO> routes(1);
        routes[0].routeSlotIndex = 0; routes[0].droneId = "drone-0";
        routes[0].totalBytes = 100;
        ConsoleUploadJobAuthority authority(root.root.u8string(), nullptr, nullptr);
        std::string error;
        Expect(authority.InstallSyntheticReadyJobForTest(
            Id('a'), identity, routes, 0, 0, 0, 1, error), error);
        bool recovered = false;
        Expect(!TryGateOnAnotherThread(root.root, recovered),
            "promotion gate acquired while Upload Job lease was active");
        const auto* snapshot = authority.FindCurrentSnapshotForTest(Id('a'));
        dspgcs::UploadJobCommandDTO cancel;
        cancel.clientCommandId = Id('b'); cancel.uploadJobId = Id('a');
        cancel.commandType = dspgcs::UploadJobCommandType::Cancel;
        cancel.expectedJobRevision = snapshot->jobRevision;
        authority.HandleCommand("client", cancel, EncodeCommand(cancel));
        Expect(TryGateOnAnotherThread(root.root, recovered),
            "promotion gate remained locked after Cancelled");

        const std::wstring name = artifact::ArtifactReadLease::BuildMutexName(root.root.wstring());
        HANDLE abandonedHandle = nullptr;
        std::thread abandoned([&]
        {
            abandonedHandle = CreateMutexW(nullptr, FALSE, name.c_str());
            Expect(abandonedHandle != nullptr &&
                WaitForSingleObject(abandonedHandle, 0) == WAIT_OBJECT_0,
                "abandoned lease setup failed");
            // Thread exit without ReleaseMutex deliberately models owner death.
        });
        abandoned.join();
        artifact::ArtifactReadLease recoveredLease;
        Expect(recoveredLease.TryAcquire(root.root.wstring(), recovered, error) ==
            artifact::ArtifactReadLeaseAcquireResult::Acquired && recovered,
            "abandoned lease was not recoverable");
        CloseHandle(abandonedHandle);
        std::size_t entryCount = 0;
        for (const auto& ignored : fs::directory_iterator(root.root))
        {
            (void)ignored; ++entryCount;
        }
        Expect(entryCount == 1 && fs::exists(root.root / "Current"),
            "lease created a historical Artifact archive");
    }

    struct ScaleMetric
    {
        std::size_t routeCount = 0;
        std::size_t snapshotBytes = 0;
        std::size_t deltaBytes = 0;
        std::size_t maximumPackedDeltaBytes = 0;
        std::size_t pageBytes = 0;
        std::size_t pageRoutes = 0;
        std::size_t estimatedStateBytes = 0;
        double codecMegabytesPerSecond = 0.0;
    };

    ScaleMetric VerifyScale(const std::size_t routeCount, const char idCharacter)
    {
        TempRoot root("scale");
        dspgcs::UploadArtifactIdentityDTO identity;
        identity.artifactSetId = std::string(64, 'c');
        identity.fleetSessionId = "fleet-scale";
        identity.assignmentAuthoritySessionId = "authority-scale";
        identity.assignmentRevision = 1;
        std::vector<dspgcs::UploadRouteStateDTO> routes;
        routes.reserve(routeCount);
        for (std::size_t index = 0; index < routeCount; ++index)
        {
            dspgcs::UploadRouteStateDTO route;
            route.routeSlotIndex = static_cast<std::int32_t>(index);
            route.droneId = "drone-" + std::to_string(index);
            route.totalBytes = 4096; route.updatedAtUtcMs = 1;
            routes.push_back(std::move(route));
        }
        ConsoleUploadJobAuthority authority(root.root.u8string(), nullptr, nullptr);
        std::string error;
        const std::string jobId = Id(idCharacter);
        Expect(authority.InstallSyntheticReadyJobForTest(
            jobId, identity, routes, 0, 0, 0, 32, error), error);
        dspgcs::UploadJobSnapshotRequestDTO request;
        request.clientRequestId = Id('d'); request.uploadJobId = jobId;
        const auto snapshot = authority.BuildSnapshot(request);
        Expect(authority.GetFrozenPageRouteCountForTest(jobId) == 0,
            "aggregate snapshot copied route detail instead of remaining O(1)");
        std::vector<std::uint8_t> snapshotBytes;
        Expect(dspgcs::EncodeUploadJobSnapshot(snapshot, snapshotBytes, error), error);

        request.clientRequestId = Id('e');
        request.requestKind = dspgcs::UploadJobSnapshotRequestKind::RoutePage;
        request.snapshotToken = snapshot.snapshotToken;
        request.maximumRouteEntries = static_cast<std::uint32_t>(routeCount);
        dspgcs::UploadJobRouteStatePageDTO page; dspgcs::UploadJobErrorCode pageError;
        Expect(authority.BuildRoutePage(request, page, pageError), "page build failed");
        std::vector<std::uint8_t> pageBytes;
        Expect(dspgcs::EncodeUploadJobRouteStatePage(page, pageBytes, error), error);

        routes[0].state = dspgcs::UploadRouteState::Uploading;
        routes[0].bytesCompleted = 512;
        Expect(authority.ApplyExecutorRouteUpdates(
            jobId, { routes[0] }, 0, 0, 0, error), error);
        auto deltas = authority.DrainRouteDeltas();
        Expect(deltas.size() == 1, "one route update did not produce one delta");
        std::vector<std::uint8_t> deltaBytes;
        Expect(dspgcs::EncodeUploadJobRouteStateDelta(deltas[0], deltaBytes, error), error);

        request.clientRequestId = Id('f');
        request.routePageStart = page.nextPageStart;
        dspgcs::UploadJobRouteStatePageDTO nextPage;
        Expect(authority.BuildRoutePage(request, nextPage, pageError) &&
            nextPage.jobRevision == snapshot.jobRevision,
            "paged snapshot changed after live route state mutation");

        std::vector<dspgcs::UploadRouteStateDTO> bulkUpdates;
        bulkUpdates.reserve(routeCount > 0 ? routeCount - 1 : 0);
        for (std::size_t index = 1; index < routeCount; ++index)
        {
            routes[index].state = dspgcs::UploadRouteState::Uploading;
            routes[index].bytesCompleted = 512;
            bulkUpdates.push_back(routes[index]);
        }
        Expect(authority.ApplyExecutorRouteUpdates(
            jobId, bulkUpdates, 0, 0, 0, error), error);
        auto packedDeltas = authority.DrainRouteDeltas();
        std::size_t packedRouteCount = 0;
        std::size_t maximumPackedDeltaBytes = 0;
        for (const auto& packedDelta : packedDeltas)
        {
            std::vector<std::uint8_t> encoded;
            Expect(dspgcs::EncodeUploadJobRouteStateDelta(
                packedDelta, encoded, error), error);
            maximumPackedDeltaBytes = std::max(
                maximumPackedDeltaBytes, encoded.size());
            packedRouteCount += packedDelta.routes.size();
            Expect(encoded.size() < 900, "packed route Delta exceeded byte budget");
        }
        Expect(packedRouteCount == bulkUpdates.size(),
            "bulk route Delta split lost or duplicated routes");

        const auto started = std::chrono::steady_clock::now();
        std::size_t codecBytes = 0;
        for (int iteration = 0; iteration < 200; ++iteration)
        {
            std::vector<std::uint8_t> encoded;
            dspgcs::UploadJobRouteStatePageDTO decoded;
            Expect(dspgcs::EncodeUploadJobRouteStatePage(page, encoded, error), error);
            Expect(dspgcs::DecodeUploadJobRouteStatePage(encoded, decoded, error), error);
            codecBytes += encoded.size() * 2;
        }
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        ScaleMetric metric;
        metric.routeCount = routeCount;
        metric.snapshotBytes = snapshotBytes.size();
        metric.deltaBytes = deltaBytes.size();
        metric.maximumPackedDeltaBytes = maximumPackedDeltaBytes;
        metric.pageBytes = pageBytes.size();
        metric.pageRoutes = page.routes.size();
        metric.estimatedStateBytes = routeCount * sizeof(dspgcs::UploadRouteStateDTO);
        for (const auto& route : routes) metric.estimatedStateBytes += route.droneId.capacity();
        metric.codecMegabytesPerSecond = codecBytes / seconds / (1024.0 * 1024.0);
        Expect(metric.snapshotBytes < 1000 && metric.deltaBytes < 900 &&
            metric.pageBytes < 900, "wire payload budget exceeded");
        return metric;
    }

    void VerifySnapshotRecoveryAndScale()
    {
        const auto thousand = VerifyScale(1000, '4');
        const auto tenThousand = VerifyScale(10000, '5');
        const auto twentyThousand = VerifyScale(20000, '6');
        Expect(thousand.snapshotBytes == tenThousand.snapshotBytes &&
            tenThousand.snapshotBytes == twentyThousand.snapshotBytes,
            "aggregate snapshot is not O(1)");
        Expect(twentyThousand.estimatedStateBytes <
            tenThousand.estimatedStateBytes * 3, "state memory is not O(N)");
        std::cout << "UploadJob scale PASS\n";
        for (const auto& metric : { thousand, tenThousand, twentyThousand })
            std::cout << "routes=" << metric.routeCount
                << " snapshotBytes=" << metric.snapshotBytes
                << " deltaBytes=" << metric.deltaBytes
                << " maxPackedDeltaBytes=" << metric.maximumPackedDeltaBytes
                << " pageBytes=" << metric.pageBytes
                << " pageRoutes=" << metric.pageRoutes
                << " stateBytes=" << metric.estimatedStateBytes
                << " codecMBps=" << metric.codecMegabytesPerSecond << '\n';

        TempRoot restartRoot("restart");
        ConsoleUploadJobAuthority restarted(restartRoot.root.u8string(), nullptr, nullptr);
        dspgcs::UploadJobSnapshotRequestDTO request;
        request.clientRequestId = Id('7'); request.uploadJobId = Id('8');
        const auto missing = restarted.BuildSnapshot(request);
        Expect(missing.resultCode == dspgcs::UploadJobErrorCode::UnknownJob,
            "Console restart/no job did not return UnknownJob");
    }
}

int RunDSPUploadJobProtocolTests()
{
    try
    {
        VerifyWireContract();
        VerifyAuthorityAndArtifactGate();
        VerifyLeaseLifecycle();
        VerifySnapshotRecoveryAndScale();
        std::cout << "DSP Upload Job protocol/reliability tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "DSP Upload Job protocol/reliability tests failed: "
            << error.what() << '\n';
        return 1;
    }
}
