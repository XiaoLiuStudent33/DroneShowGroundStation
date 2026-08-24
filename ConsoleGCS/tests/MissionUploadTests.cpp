#include "MissionPreparationWorker.h"
#include "MissionProtocolTransport.h"
#include "MissionUploadTransaction.h"
#include "ArduPilotMissionWireAdapter.h"
#include "MavlinkDatagramSender.h"
#include "MavlinkDiscoveryService.h"
#include "MavlinkReadOnlyIngress.h"
#include "MAVFTPMissionTransport.h"
#include "RouteUploadManager.h"
#include "RouteArtifactContract.h"
#include "Wpl110Core.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace mission = dronegcs::route_mission;
    namespace ardupilotMission = dronegcs::ardupilot_mission;
    namespace wpl = dronegcs::wpl110;
    namespace artifact = dronegcs::route_artifact;

    void Expect(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    class ScopedCoutCapture final
    {
    public:
        explicit ScopedCoutCapture(const bool enabled)
        {
            if (enabled) original_ = std::cout.rdbuf(output_.rdbuf());
        }

        ~ScopedCoutCapture() { Restore(); }

        std::string Stop()
        {
            Restore();
            return output_.str();
        }

    private:
        void Restore()
        {
            if (!original_) return;
            std::cout.rdbuf(original_);
            original_ = nullptr;
        }

        std::ostringstream output_;
        std::streambuf* original_ = nullptr;
    };

    MissionProtocolPeerIdentity Peer()
    {
        return { "Drone001", "127.0.0.1", 14550, 1,
            MAV_COMP_ID_AUTOPILOT1 };
    }

    std::vector<mission::MissionItemIntV1> BuildMission(
        const std::size_t count = 3)
    {
        std::vector<mission::MissionItemIntV1> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            wpl::Row row;
            row.Sequence = static_cast<std::int32_t>(index);
            row.Red = static_cast<std::uint8_t>(index + 1);
            row.Green = static_cast<std::uint8_t>(index + 2);
            row.Blue = static_cast<std::uint8_t>(index + 3);
            row.LatitudeE7 = 319327550 + static_cast<std::int32_t>(index);
            row.LongitudeE7 = 1186382469 + static_cast<std::int32_t>(index);
            row.AltitudeE7 = 10000000 + static_cast<std::int64_t>(index);
            mission::MissionItemIntV1 item;
            mission::Error error;
            Expect(mission::BuildMissionItemIntV1(row, item, error),
                "mission item conversion failed");
            result.push_back(item);
        }
        return result;
    }

    std::vector<mission::MissionItemIntV1> BuildWireMissionForTest(
        const std::vector<mission::MissionItemIntV1>& artifactItems)
    {
        std::vector<mission::MissionItemIntV1> result;
        Expect(ardupilotMission::BuildWireMission(artifactItems, result),
            "ArduPilot wire Mission fixture failed");
        return result;
    }

    bool SameMissionPayload(
        const mission::MissionItemIntV1& left,
        const mission::MissionItemIntV1& right)
    {
        return left.Frame == right.Frame && left.Command == right.Command &&
            left.Current == right.Current &&
            left.AutoContinue == right.AutoContinue &&
            mission::Binary32ToBits(left.Param1) ==
                mission::Binary32ToBits(right.Param1) &&
            mission::Binary32ToBits(left.Param2) ==
                mission::Binary32ToBits(right.Param2) &&
            mission::Binary32ToBits(left.Param3) ==
                mission::Binary32ToBits(right.Param3) &&
            mission::Binary32ToBits(left.Param4) ==
                mission::Binary32ToBits(right.Param4) &&
            left.X == right.X && left.Y == right.Y &&
            mission::Binary32ToBits(left.Z) ==
                mission::Binary32ToBits(right.Z) &&
            left.MissionType == right.MissionType;
    }

    class FakeTransport final : public IMissionProtocolTransport
    {
    public:
        bool SendMissionCount(std::uint16_t count, std::size_t& bytes) override
        {
            counts.push_back(count); bytes = 20; return allowSend;
        }
        bool SendMissionItemInt(const mission::MissionItemIntV1& item,
            std::size_t& bytes) override
        {
            items.push_back(item.Sequence);
            sentMissionItems.push_back(item);
            bytes = 50;
            return allowSend;
        }
        bool SendMissionRequestList(std::size_t& bytes) override
        {
            ++requestLists; bytes = 16; return allowSend;
        }
        bool SendMissionRequestInt(std::uint16_t sequence,
            std::size_t& bytes) override
        {
            requestedItems.push_back(sequence); bytes = 18; return allowSend;
        }

        bool allowSend = true;
        std::vector<std::uint16_t> counts;
        std::vector<std::uint16_t> items;
        std::vector<mission::MissionItemIntV1> sentMissionItems;
        std::vector<std::uint16_t> requestedItems;
        std::size_t requestLists = 0;
    };

    MissionUploadTransaction MakeTransaction(
        FakeTransport& transport,
        const MissionUploadTransactionConfig config = {})
    {
        MissionUploadTransactionIdentity identity;
        identity.uploadJobId = std::string(32, 'a');
        identity.droneId = "Drone001";
        identity.routeSlotIndex = 0;
        identity.peer = Peer();
        identity.transactionGeneration = 1;
        return MissionUploadTransaction(
            std::move(identity), BuildMission(), transport, config);
    }

    MissionUploadTransaction MakeTransactionWithMission(
        FakeTransport& transport,
        std::vector<mission::MissionItemIntV1> missionItems,
        const MissionUploadTransactionConfig config = {})
    {
        MissionUploadTransactionIdentity identity;
        identity.uploadJobId = std::string(32, 'a');
        identity.droneId = "Drone001";
        identity.routeSlotIndex = 0;
        identity.peer = Peer();
        identity.transactionGeneration = 1;
        return MissionUploadTransaction(
            std::move(identity), std::move(missionItems), transport, config);
    }

    mavlink_message_t Request(const std::uint16_t sequence,
        const bool legacy = false, const std::uint8_t missionType = 0)
    {
        mavlink_message_t message{};
        if (legacy)
            mavlink_msg_mission_request_pack(1, MAV_COMP_ID_AUTOPILOT1,
                &message, 255, MAV_COMP_ID_MISSIONPLANNER, sequence,
                missionType);
        else
            mavlink_msg_mission_request_int_pack(1, MAV_COMP_ID_AUTOPILOT1,
                &message, 255, MAV_COMP_ID_MISSIONPLANNER, sequence,
                missionType);
        return message;
    }

    mavlink_message_t Ack(const std::uint8_t result)
    {
        mavlink_message_t message{};
        mavlink_msg_mission_ack_pack(1, MAV_COMP_ID_AUTOPILOT1, &message,
            255, MAV_COMP_ID_MISSIONPLANNER, result,
            MAV_MISSION_TYPE_MISSION, 0);
        return message;
    }

    mavlink_message_t Count(const std::uint16_t count)
    {
        mavlink_message_t message{};
        mavlink_msg_mission_count_pack(1, MAV_COMP_ID_AUTOPILOT1, &message,
            255, MAV_COMP_ID_MISSIONPLANNER, count,
            MAV_MISSION_TYPE_MISSION, 0);
        return message;
    }

    mavlink_message_t Item(const mission::MissionItemIntV1& item)
    {
        mavlink_message_t message{};
        mavlink_msg_mission_item_int_pack(1, MAV_COMP_ID_AUTOPILOT1,
            &message, 255, MAV_COMP_ID_MISSIONPLANNER, item.Sequence,
            item.Frame, item.Command, item.Current, item.AutoContinue,
            item.Param1, item.Param2, item.Param3, item.Param4,
            item.X, item.Y, item.Z, MAV_MISSION_TYPE_MISSION);
        return message;
    }

    mission::MissionItemIntV1 ArduPilotReadbackItem(
        mission::MissionItemIntV1 item)
    {
        const std::int32_t storedCentimeters =
            static_cast<std::int32_t>(item.Z * 100.0f);
        item.Z = static_cast<float>(storedCentimeters) * 0.01f;
        return item;
    }

    void CompleteUpload(MissionUploadTransaction& tx,
        const std::vector<mission::MissionItemIntV1>& items,
        const std::chrono::steady_clock::time_point now)
    {
        const auto wireMission = BuildWireMissionForTest(items);
        for (std::uint16_t sequence = 0; sequence < wireMission.size(); ++sequence)
        {
            const mavlink_message_t request = Request(sequence);
            Expect(tx.HandleMissionMessage(request, Peer(), now),
                "request was not accepted");
        }
        const mavlink_message_t ack = Ack(MAV_MISSION_ACCEPTED);
        Expect(tx.HandleMissionMessage(ack, Peer(), now),
            "accepted ack was not consumed");
        Expect(!tx.Succeeded(), "ACK alone incorrectly completed upload");
        const mavlink_message_t count = Count(
            static_cast<std::uint16_t>(wireMission.size()));
        Expect(tx.HandleMissionMessage(count, Peer(), now),
            "verification count was not consumed");
        const mavlink_message_t first = Item(
            ArduPilotReadbackItem(wireMission[1]));
        Expect(tx.HandleMissionMessage(first, Peer(), now),
            "first verification item was not consumed");
        const mavlink_message_t last = Item(
            ArduPilotReadbackItem(wireMission.back()));
        Expect(tx.HandleMissionMessage(last, Peer(), now),
            "last verification item was not consumed");
    }

    void RunMissionTransportAbstractionTests()
    {
        FakeTransport wireTransport;
        MissionProtocolTransport transport(wireTransport, Peer());
        MissionTransportIdentity identity;
        identity.uploadJobId = std::string(32, 't');
        identity.droneId = "Drone001";
        identity.routeSlotIndex = 0;
        identity.generation = 7;
        const auto artifactMission = BuildMission();
        std::string diagnostic;
        Expect(transport.GetStatus() == MissionTransportStatus::Idle,
            "new Mission Transport did not start Idle");
        Expect(transport.Prepare(identity, artifactMission, diagnostic) &&
            diagnostic.empty() &&
            transport.GetStatus() == MissionTransportStatus::Prepared,
            "MissionProtocol Transport Prepare failed");

        const auto now = std::chrono::steady_clock::time_point(
            std::chrono::milliseconds(1000));
        Expect(transport.Upload(now) &&
            transport.GetStatus() == MissionTransportStatus::Uploading,
            "MissionProtocol Transport Upload failed");
        IMissionProtocolMessageSink* const sink =
            transport.GetMissionProtocolMessageSink();
        Expect(sink != nullptr, "MissionProtocol Transport omitted its ingress sink");
        const auto wireMission = BuildWireMissionForTest(artifactMission);
        for (std::uint16_t sequence = 0; sequence < wireMission.size(); ++sequence)
        {
            Expect(sink->HandleMissionMessage(Request(sequence), Peer(), now),
                "MissionProtocol Transport rejected a Mission request");
        }
        Expect(transport.GetStatus() == MissionTransportStatus::WaitingAck,
            "MissionProtocol Transport did not expose WaitingAck");
        Expect(sink->HandleMissionMessage(
                Ack(MAV_MISSION_ACCEPTED), Peer(), now) &&
            transport.GetStatus() == MissionTransportStatus::Verifying,
            "MissionProtocol Transport did not enter Verification");
        Expect(sink->HandleMissionMessage(
                Count(static_cast<std::uint16_t>(wireMission.size())), Peer(), now) &&
            sink->HandleMissionMessage(
                Item(ArduPilotReadbackItem(wireMission[1])), Peer(), now) &&
            sink->HandleMissionMessage(
                Item(ArduPilotReadbackItem(wireMission.back())), Peer(), now),
            "MissionProtocol Transport verification flow failed");
        Expect(transport.IsTerminal() && transport.Verify() &&
            transport.GetStatus() == MissionTransportStatus::Succeeded &&
            transport.GetMetrics().artifactRouteItems == artifactMission.size() &&
            transport.GetMetrics().wireMissionItems == wireMission.size(),
            "MissionProtocol Transport changed transaction result or metrics");

        FakeTransport secondWireTransport;
        MissionProtocolPeerIdentity secondPeer = Peer();
        secondPeer.droneId = "Drone002";
        MissionProtocolTransport second(secondWireTransport, secondPeer);
        MissionTransportIdentity secondIdentity = identity;
        secondIdentity.droneId = "Drone002";
        secondIdentity.routeSlotIndex = 1;
        secondIdentity.generation = 8;
        Expect(second.Prepare(secondIdentity, BuildMission(), diagnostic) &&
            second.Upload(now), "second per-Drone Mission Transport failed to start");
        second.Cancel(now);
        Expect(second.GetStatus() == MissionTransportStatus::Cancelled &&
            transport.GetStatus() == MissionTransportStatus::Succeeded,
            "per-Drone Mission Transport instances shared mutable state");

        // MAVFTP production reuses the same verifier after its final
        // TerminateSession ACK, without replaying MissionProtocol upload.
        FakeTransport verifierWire;
        auto verifier = MakeTransaction(verifierWire);
        Expect(verifier.BeginVerification(now, true) &&
            verifierWire.counts.empty() && verifierWire.requestLists == 1,
            "verification-only entry sent upload data or omitted Mission Count");
        const auto verifierMission = BuildWireMissionForTest(BuildMission());
        Expect(verifier.HandleMissionMessage(
                Count(static_cast<std::uint16_t>(verifierMission.size())),
                Peer(), now) &&
            verifier.GetPhase() == MissionUploadPhase::VerifyingHome &&
            verifierWire.requestedItems == std::vector<std::uint16_t>{ 0 },
            "production verifier did not request Reserved Home after Count");
        Expect(verifier.HandleMissionMessage(
                Item(verifierMission[0]), Peer(), now) &&
            verifier.GetPhase() == MissionUploadPhase::VerifyingFirst &&
            verifierWire.requestedItems.back() == 1,
            "production verifier did not observe Home before First Item");
        MissionProtocolPeerIdentity wrongPeer = Peer();
        wrongPeer.remotePort++;
        Expect(!verifier.HandleMissionMessage(
                Item(verifierMission[1]), wrongPeer, now) &&
            verifier.GetPhase() == MissionUploadPhase::VerifyingFirst,
            "production verifier accepted a wrong-endpoint response");
        Expect(verifier.HandleMissionMessage(
                Item(ArduPilotReadbackItem(verifierMission[1])), Peer(), now) &&
            verifier.HandleMissionMessage(
                Item(ArduPilotReadbackItem(verifierMission.back())), Peer(), now) &&
            verifier.Succeeded(),
            "production verifier did not require Count/Home/First/Last success");

        FakeTransport mismatchWire;
        auto mismatchVerifier = MakeTransaction(mismatchWire);
        Expect(mismatchVerifier.BeginVerification(now, true) &&
            mismatchVerifier.HandleMissionMessage(
                Count(static_cast<std::uint16_t>(verifierMission.size() - 1)),
                Peer(), now) && !mismatchVerifier.Succeeded() &&
            mismatchVerifier.GetPhase() == MissionUploadPhase::Failed,
            "Mission Count mismatch incorrectly passed production verification");
    }

    void AdvanceToVerifyingFirst(
        MissionUploadTransaction& tx,
        const std::vector<mission::MissionItemIntV1>& items,
        const std::chrono::steady_clock::time_point startedAt,
        const std::chrono::steady_clock::time_point acceptedAt,
        const std::chrono::steady_clock::time_point countAt)
    {
        const auto wireMission = BuildWireMissionForTest(items);
        Expect(tx.Start(startedAt), "diagnostic transaction did not start");
        for (std::uint16_t sequence = 0; sequence < wireMission.size(); ++sequence)
        {
            Expect(tx.HandleMissionMessage(Request(sequence), Peer(), startedAt),
                "diagnostic upload request was not consumed");
        }
        Expect(tx.HandleMissionMessage(Ack(MAV_MISSION_ACCEPTED), Peer(), acceptedAt),
            "diagnostic accepted ACK was not consumed");
        Expect(tx.HandleMissionMessage(
            Count(static_cast<std::uint16_t>(wireMission.size())), Peer(), countAt),
            "diagnostic Mission count was not consumed");
        Expect(tx.GetPhase() == MissionUploadPhase::VerifyingFirst,
            "diagnostic transaction did not reach VerifyingFirst");
    }

    struct MismatchResult
    {
        MissionUploadPhase phase = MissionUploadPhase::Idle;
        MissionUploadTransactionMetrics metrics;
        std::string safeDiagnostic;
        std::string mismatchDiagnostic;
        bool rejectedLateMessage = false;
    };

    MismatchResult RunFirstMismatch(
        const std::vector<mission::MissionItemIntV1>& items,
        const mission::MissionItemIntV1& actual)
    {
        FakeTransport transport;
        auto tx = MakeTransactionWithMission(transport, items);
        const auto startedAt = std::chrono::steady_clock::time_point(
            std::chrono::milliseconds(1000));
        const auto acceptedAt = startedAt + std::chrono::milliseconds(100);
        const auto countAt = acceptedAt + std::chrono::milliseconds(50);
        const auto mismatchAt = acceptedAt + std::chrono::milliseconds(250);
        AdvanceToVerifyingFirst(tx, items, startedAt, acceptedAt, countAt);
        mission::MissionItemIntV1 wireActual = actual;
        ++wireActual.Sequence;
        const mavlink_message_t mismatch = Item(wireActual);
        Expect(tx.HandleMissionMessage(mismatch, Peer(), mismatchAt),
            "first mismatch was not consumed");
        const std::string diagnostic = tx.GetVerificationMismatchDiagnostic();
        return {
            tx.GetPhase(),
            tx.GetMetrics(),
            tx.GetSafeDiagnostic(),
            diagnostic,
            !tx.HandleMissionMessage(mismatch, Peer(),
                mismatchAt + std::chrono::milliseconds(1)) &&
                tx.GetVerificationMismatchDiagnostic() == diagnostic
        };
    }

    bool HasExactFieldMask(
        const std::string& diagnostic,
        const std::string& expected)
    {
        return diagnostic.find("FieldMask=" + expected + " ") !=
            std::string::npos;
    }

    std::string BuildWpl(const std::size_t rows)
    {
        std::string result = "QGC WPL 110\r\n";
        for (std::size_t index = 0; index < rows; ++index)
        {
            wpl::Row row;
            row.Sequence = static_cast<std::int32_t>(index);
            row.Red = static_cast<std::uint8_t>(index % 256);
            row.Green = static_cast<std::uint8_t>((index + 1) % 256);
            row.Blue = static_cast<std::uint8_t>((index + 2) % 256);
            row.LatitudeE7 = 319327550;
            row.LongitudeE7 = 1186382469;
            row.AltitudeE7 = 10000000;
            wpl::FormattedRow formatted;
            Expect(wpl::FormatRow(row, formatted), "WPL row formatting failed");
            result.append(reinterpret_cast<const char*>(formatted.Bytes.data()),
                formatted.Size);
            if (index + 1 < rows) result += "\r\n";
        }
        return result;
    }

    std::vector<mission::MissionItemIntV1> BuildWplMission(
        const std::size_t rows)
    {
        std::vector<mission::MissionItemIntV1> result;
        result.reserve(rows);
        for (std::size_t index = 0; index < rows; ++index)
        {
            wpl::Row row;
            row.Sequence = static_cast<std::int32_t>(index);
            row.Red = static_cast<std::uint8_t>(index % 256);
            row.Green = static_cast<std::uint8_t>((index + 1) % 256);
            row.Blue = static_cast<std::uint8_t>((index + 2) % 256);
            row.LatitudeE7 = 319327550;
            row.LongitudeE7 = 1186382469;
            row.AltitudeE7 = 10000000;
            mission::MissionItemIntV1 item;
            mission::Error error;
            Expect(mission::BuildMissionItemIntV1(row, item, error),
                "WPL fixture Mission conversion failed");
            result.push_back(item);
        }
        return result;
    }

    struct TemporaryRoute
    {
        TemporaryRoute()
        {
            static std::atomic_uint64_t next{ 0 };
            root = fs::temp_directory_path() /
                ("dronegcs-mission-prep-" +
                 std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(++next));
            std::error_code error;
            fs::remove_all(root, error);
            fs::create_directories(root / "routes");
        }
        ~TemporaryRoute()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }
        fs::path root;
    };

    MissionPreparationRequest MakePreparation(
        const fs::path& root, const std::string& bytes,
        const std::uint32_t rows)
    {
        const fs::path route = root / "routes" / "0.waypoints";
        std::ofstream stream(route, std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        artifact::Digest256 digest;
        Expect(artifact::Sha256::Hash(bytes.data(), bytes.size(), digest),
            "fixture digest failed");
        auto snapshot = std::make_shared<AcceptedRouteArtifactV2>();
        snapshot->resolvedCurrentRoot = root.u8string();
        snapshot->frameCount = static_cast<std::int32_t>(rows);
        MissionPreparationRequest request;
        request.uploadJobId = std::string(32, 'b');
        request.generation = 7;
        request.artifactSnapshot = snapshot;
        request.target.droneId = "Drone001";
        request.target.routeSlotIndex = 0;
        request.target.relativePath = "routes/0.waypoints";
        request.target.fileByteSize = static_cast<std::int64_t>(bytes.size());
        request.target.missionItemCount = rows;
        request.target.missionPayloadByteSize =
            static_cast<std::uint64_t>(rows) * mission::MissionItemRecordBytesV1;
        request.target.routeArtifactDigest = digest;
        return request;
    }

    void RunTransactionTests()
    {
        const auto now = std::chrono::steady_clock::now();
        const auto items = BuildMission();
        const auto originalItems = items;
        const auto wireItems = BuildWireMissionForTest(items);
        FakeTransport transport;
        auto tx = MakeTransaction(transport);
        Expect(tx.Start(now), "transaction did not start");
        Expect(transport.counts == std::vector<std::uint16_t>{ 4 } &&
            tx.GetMissionItemCount() == 3 && tx.GetWireMissionItemCount() == 4,
            "MISSION_COUNT was not authoritative");

        const mavlink_message_t legacy = Request(0, true);
        Expect(tx.HandleMissionMessage(legacy, Peer(), now),
            "legacy MISSION_REQUEST was not accepted");
        const mavlink_message_t duplicate = Request(0);
        Expect(tx.HandleMissionMessage(duplicate, Peer(), now),
            "duplicate request was not idempotent");
        Expect(tx.GetMetrics().logicalBytesCompleted == 0,
            "reserved Home request advanced route logical progress");
        Expect(tx.GetMetrics().duplicateRequests == 1,
            "duplicate request metric was not recorded");
        for (std::uint16_t sequence = 1; sequence < wireItems.size(); ++sequence)
        {
            const mavlink_message_t request = Request(sequence);
            Expect(tx.HandleMissionMessage(request, Peer(), now),
                "MISSION_REQUEST_INT was not accepted");
        }
        Expect(transport.sentMissionItems.size() == 5 &&
            transport.sentMissionItems[0].Sequence == 0 &&
            SameMissionPayload(transport.sentMissionItems[0], items[0]) &&
            transport.sentMissionItems[2].Sequence == 1 &&
            SameMissionPayload(transport.sentMissionItems[2], items[0]) &&
            transport.sentMissionItems[3].Sequence == 2 &&
            SameMissionPayload(transport.sentMissionItems[3], items[1]) &&
            transport.sentMissionItems[4].Sequence == 3 &&
            SameMissionPayload(transport.sentMissionItems[4], items[2]) &&
            items[0].Sequence == originalItems[0].Sequence &&
            items[2].Sequence == originalItems[2].Sequence,
            "Home slot mapping changed or lost Artifact route payload");
        Expect(tx.GetMetrics().logicalBytesCompleted ==
                items.size() * mission::MissionItemRecordBytesV1 &&
            tx.GetMetrics().artifactRouteItems == 3 &&
            tx.GetMetrics().wireMissionItems == 4 &&
            tx.GetMetrics().uniqueItemsServed == 4,
            "Artifact/wire progress metrics were not separated");
        {
            std::vector<mission::MissionItemIntV1> maximumArtifact(
                ardupilotMission::MaximumArtifactRouteItems, items.front());
            std::vector<mission::MissionItemIntV1> boundaryWire;
            Expect(ardupilotMission::BuildWireMission(
                    maximumArtifact, boundaryWire) &&
                boundaryWire.size() ==
                    ardupilotMission::MaximumWireMissionItems &&
                boundaryWire.front().Sequence == 0 &&
                boundaryWire.back().Sequence == 65534,
                "maximum Artifact route boundary did not map to uint16 wire count");
            maximumArtifact.push_back(items.front());
            Expect(!ardupilotMission::BuildWireMission(
                    maximumArtifact, boundaryWire) && boundaryWire.empty(),
                "overflowing Artifact route count was not rejected");
        }
        const mavlink_message_t accepted = Ack(MAV_MISSION_ACCEPTED);
        Expect(tx.HandleMissionMessage(accepted, Peer(), now),
            "MISSION_ACK accepted was not consumed");
        Expect(tx.TransportAccepted() && !tx.Succeeded(),
            "transport acceptance bypassed verification");
        const mavlink_message_t count = Count(4);
        tx.HandleMissionMessage(count, Peer(), now);
        auto arbitraryHome = wireItems.front();
        arbitraryHome.Frame = 0;
        arbitraryHome.Param1 = 0.0f;
        arbitraryHome.Param2 = 0.0f;
        arbitraryHome.Param3 = 0.0f;
        arbitraryHome.X = 0;
        arbitraryHome.Y = 0;
        arbitraryHome.Z = 0.0f;
        Expect(tx.HandleMissionMessage(Item(arbitraryHome), Peer(), now) &&
            tx.GetPhase() == MissionUploadPhase::VerifyingFirst,
            "authoritative FC Home readback failed route verification");
        const mavlink_message_t first = Item(
            ArduPilotReadbackItem(wireItems[1]));
        tx.HandleMissionMessage(first, Peer(), now);
        const mavlink_message_t last = Item(
            ArduPilotReadbackItem(wireItems.back()));
        tx.HandleMissionMessage(last, Peer(), now);
        Expect(tx.Succeeded() && tx.GetVerificationMismatchDiagnostic().empty(),
            "full accepted/count/first/last gate emitted mismatch diagnostics");

        FakeTransport wrongTransport;
        auto wrong = MakeTransaction(wrongTransport);
        wrong.Start(now);
        MissionProtocolPeerIdentity wrongPeer = Peer();
        wrongPeer.droneId = "Drone002";
        const mavlink_message_t request0 = Request(0);
        Expect(!wrong.HandleMissionMessage(request0, wrongPeer, now),
            "wrong Drone advanced transaction");
        wrongPeer = Peer();
        wrongPeer.remotePort = 14551;
        Expect(!wrong.HandleMissionMessage(request0, wrongPeer, now),
            "wrong endpoint advanced transaction");
        mavlink_message_t wrongSystem = request0;
        wrongSystem.sysid = 2;
        Expect(!wrong.HandleMissionMessage(wrongSystem, Peer(), now),
            "wrong SYSID advanced transaction");
        mavlink_message_t wrongComponent = request0;
        wrongComponent.compid = MAV_COMP_ID_CAMERA;
        Expect(!wrong.HandleMissionMessage(wrongComponent, Peer(), now),
            "wrong component advanced transaction");
        const mavlink_message_t wrongType = Request(0, false,
            MAV_MISSION_TYPE_FENCE);
        Expect(!wrong.HandleMissionMessage(wrongType, Peer(), now),
            "wrong Mission type advanced transaction");
        const mavlink_message_t outOfRange = Request(99);
        Expect(wrong.HandleMissionMessage(outOfRange, Peer(), now) &&
            wrong.GetPhase() == MissionUploadPhase::Failed,
            "out-of-range request did not fail closed");

        FakeTransport rejectTransport;
        auto rejected = MakeTransaction(rejectTransport);
        rejected.Start(now);
        for (std::uint16_t sequence = 0; sequence < 4; ++sequence)
        {
            const mavlink_message_t request = Request(sequence);
            rejected.HandleMissionMessage(request, Peer(), now);
        }
        const mavlink_message_t noSpace = Ack(MAV_MISSION_NO_SPACE);
        rejected.HandleMissionMessage(noSpace, Peer(), now);
        Expect(rejected.GetPhase() == MissionUploadPhase::Failed,
            "NO_SPACE did not fail target");
		Expect(rejected.GetTerminalSourcePhase() == MissionUploadPhase::AwaitingAck &&
			rejected.GetMetrics().uniqueItemsServed == 4 &&
			rejected.GetSafeDiagnostic().find("storage") != std::string::npos,
			"AwaitingAck terminal phase/diagnostic was not retained");

        FakeTransport disconnectTransport;
        auto disconnected = MakeTransaction(disconnectTransport);
        disconnected.Start(now);
        disconnectTransport.allowSend = false;
        Expect(disconnected.HandleMissionMessage(request0, Peer(), now) &&
            disconnected.GetPhase() == MissionUploadPhase::Failed,
            "transport disconnect did not fail the isolated target");

        FakeTransport mismatchTransport;
        auto mismatch = MakeTransaction(mismatchTransport);
        mismatch.Start(now);
        for (std::uint16_t sequence = 0; sequence < 4; ++sequence)
        {
            const mavlink_message_t request = Request(sequence);
            mismatch.HandleMissionMessage(request, Peer(), now);
        }
        mismatch.HandleMissionMessage(accepted, Peer(), now);
        const mavlink_message_t badCount = Count(3);
        mismatch.HandleMissionMessage(badCount, Peer(), now);
        Expect(mismatch.GetPhase() == MissionUploadPhase::Failed,
            "verification count mismatch did not fail");
		Expect(mismatch.GetTerminalSourcePhase() == MissionUploadPhase::VerifyingCount &&
			mismatch.GetSafeDiagnostic().find("count") != std::string::npos,
			"VerifyingCount terminal diagnostic was not retained");

        FakeTransport firstMismatchTransport;
        auto firstMismatch = MakeTransaction(firstMismatchTransport);
        firstMismatch.Start(now);
        for (std::uint16_t sequence = 0; sequence < 4; ++sequence)
        {
            const mavlink_message_t request = Request(sequence);
            firstMismatch.HandleMissionMessage(request, Peer(), now);
        }
        firstMismatch.HandleMissionMessage(accepted, Peer(), now);
        firstMismatch.HandleMissionMessage(count, Peer(), now);
        auto badFirstItem = items.front();
        badFirstItem.Sequence = 1;
        ++badFirstItem.X;
        const mavlink_message_t badFirst = Item(badFirstItem);
        firstMismatch.HandleMissionMessage(badFirst, Peer(), now);
        Expect(firstMismatch.GetPhase() == MissionUploadPhase::Failed,
            "first item mismatch did not fail");
		Expect(firstMismatch.GetTerminalSourcePhase() == MissionUploadPhase::VerifyingFirst &&
			firstMismatch.GetSafeDiagnostic().find("First") != std::string::npos,
			"VerifyingFirst terminal diagnostic was not retained");

        FakeTransport lastMismatchTransport;
        auto lastMismatch = MakeTransaction(lastMismatchTransport);
        lastMismatch.Start(now);
        for (std::uint16_t sequence = 0; sequence < 4; ++sequence)
        {
            const mavlink_message_t request = Request(sequence);
            lastMismatch.HandleMissionMessage(request, Peer(), now);
        }
        lastMismatch.HandleMissionMessage(accepted, Peer(), now);
        lastMismatch.HandleMissionMessage(count, Peer(), now);
        lastMismatch.HandleMissionMessage(
            Item(ArduPilotReadbackItem(wireItems[1])), Peer(), now);
        auto badLastItem = ArduPilotReadbackItem(items.back());
        badLastItem.Sequence = 3;
        ++badLastItem.Command;
        const mavlink_message_t badLast = Item(badLastItem);
        lastMismatch.HandleMissionMessage(badLast, Peer(), now);
        Expect(lastMismatch.GetPhase() == MissionUploadPhase::Failed,
            "last item mismatch did not fail");
		Expect(lastMismatch.GetTerminalSourcePhase() == MissionUploadPhase::VerifyingLast &&
			lastMismatch.GetSafeDiagnostic().find("Last") != std::string::npos,
			"VerifyingLast terminal diagnostic was not retained");

        MissionUploadTransactionConfig timeoutConfig;
        timeoutConfig.responseTimeout = std::chrono::milliseconds(1);
        timeoutConfig.maxRetries = 2;
        FakeTransport timeoutTransport;
        auto timeout = MakeTransaction(timeoutTransport, timeoutConfig);
        timeout.Start(now);
		for (std::uint16_t sequence = 0; sequence < 4; ++sequence)
			timeout.HandleMissionMessage(Request(sequence), Peer(), now);
        timeout.Tick(now + std::chrono::milliseconds(2));
        timeout.Tick(now + std::chrono::milliseconds(4));
        timeout.Tick(now + std::chrono::milliseconds(6));
        Expect(timeout.GetPhase() == MissionUploadPhase::Failed &&
			timeout.GetMetrics().retryCount == 2 &&
			timeout.GetTerminalSourcePhase() == MissionUploadPhase::AwaitingAck &&
			timeout.GetSafeDiagnostic().find("timed out") != std::string::npos,
            "timeout retry budget was not bounded");

        FakeTransport cancelTransport;
        auto cancelled = MakeTransaction(cancelTransport);
        cancelled.Start(now);
        cancelled.Cancel(now);
        Expect(cancelled.GetPhase() == MissionUploadPhase::Cancelled &&
            !cancelled.HandleMissionMessage(accepted, Peer(), now) &&
            !cancelled.HandleMissionMessage(request0, Peer(), now) &&
            cancelTransport.items.empty(),
            "late ACK or stale request advanced cancelled transaction");
    }

    void RunVerificationInstrumentationTests()
    {
        const auto items = BuildMission();
        const auto wireItems = BuildWireMissionForTest(items);

        const auto expectEquivalentFramePass = [&items, &wireItems](
            const std::uint8_t canonicalFrame)
        {
            FakeTransport compatibleTransport;
            auto compatible = MakeTransactionWithMission(
                compatibleTransport, items);
            const auto startedAt = std::chrono::steady_clock::time_point(
                std::chrono::milliseconds(500));
            const auto acceptedAt = startedAt + std::chrono::milliseconds(100);
            AdvanceToVerifyingFirst(compatible, items, startedAt, acceptedAt,
                acceptedAt + std::chrono::milliseconds(10));
            auto firstCanonical = wireItems[1];
            firstCanonical.Frame = canonicalFrame;
            firstCanonical = ArduPilotReadbackItem(firstCanonical);
            Expect(compatible.HandleMissionMessage(
                    Item(firstCanonical), Peer(),
                    acceptedAt + std::chrono::milliseconds(20)) &&
                compatible.GetPhase() == MissionUploadPhase::VerifyingLast &&
                compatible.GetVerificationMismatchDiagnostic().empty(),
                "ArduPilot first-route canonical Frame was not accepted");
            auto lastCanonical = wireItems.back();
            lastCanonical.Frame = canonicalFrame;
            lastCanonical = ArduPilotReadbackItem(lastCanonical);
            Expect(compatible.HandleMissionMessage(
                    Item(lastCanonical), Peer(),
                    acceptedAt + std::chrono::milliseconds(30)) &&
                compatible.Succeeded() &&
                compatible.GetVerificationMismatchDiagnostic().empty(),
                "ArduPilot last-route canonical Frame was not accepted");
        };
        expectEquivalentFramePass(4);
        expectEquivalentFramePass(static_cast<std::uint8_t>(
            MAV_FRAME_GLOBAL_RELATIVE_ALT_INT));

        const auto expectCanonicalAltitudePass = [](const std::uint32_t expectedBits,
            const std::uint32_t readbackBits, const char* const caseName)
        {
            auto altitudeItems = BuildMission();
            const float expected = mission::Binary32FromBits(expectedBits);
            for (auto& item : altitudeItems) item.Z = expected;
            const auto altitudeWireItems = BuildWireMissionForTest(altitudeItems);

            FakeTransport transport;
            auto transaction = MakeTransactionWithMission(
                transport, altitudeItems);
            const auto startedAt = std::chrono::steady_clock::time_point(
                std::chrono::milliseconds(750));
            const auto acceptedAt = startedAt + std::chrono::milliseconds(100);
            AdvanceToVerifyingFirst(transaction, altitudeItems, startedAt,
                acceptedAt, acceptedAt + std::chrono::milliseconds(10));

            auto firstReadback = altitudeWireItems[1];
            firstReadback.Z = mission::Binary32FromBits(readbackBits);
            if (!transaction.HandleMissionMessage(Item(firstReadback), Peer(),
                    acceptedAt + std::chrono::milliseconds(20)) ||
                transaction.GetPhase() != MissionUploadPhase::VerifyingLast ||
                !transaction.GetVerificationMismatchDiagnostic().empty())
            {
                throw std::runtime_error(std::string(
                    "ArduPilot canonical first-altitude verification failed: ") +
                    caseName);
            }

            auto lastReadback = altitudeWireItems.back();
            lastReadback.Z = mission::Binary32FromBits(readbackBits);
            if (!transaction.HandleMissionMessage(Item(lastReadback), Peer(),
                    acceptedAt + std::chrono::milliseconds(30)) ||
                !transaction.Succeeded() ||
                !transaction.GetVerificationMismatchDiagnostic().empty())
            {
                throw std::runtime_error(std::string(
                    "ArduPilot canonical last-altitude verification failed: ") +
                    caseName);
            }
        };

        expectCanonicalAltitudePass(0x41F00000U, 0x41F00000U,
            "positive integer meters"); // 30.0f -> 3000 cm -> 30.0f
        expectCanonicalAltitudePass(0x41F0FCD3U, 0x41F0F5C2U,
            "positive sub-centimeter"); // 30.12345f -> 3012 cm -> 30.12f
        expectCanonicalAltitudePass(0xC1B1CB26U, 0xC1B1C28FU,
            "negative hardware value"); // -22.2241936f -> -2222 cm -> -22.22f
        expectCanonicalAltitudePass(0x00000000U, 0x00000000U,
            "zero altitude");
        expectCanonicalAltitudePass(0x414570A4U, 0x414570A4U,
            "exact centimeter boundary"); // 12.34f

        auto strictFieldItems = BuildMission();
        strictFieldItems.front().Z = mission::Binary32FromBits(0x41F0FCD3U);
        auto strictFieldActual = strictFieldItems.front();
        strictFieldActual.Z = mission::Binary32FromBits(0x41F0F5C2U);
        ++strictFieldActual.Command;
        Expect(HasExactFieldMask(
                RunFirstMismatch(strictFieldItems, strictFieldActual)
                    .mismatchDiagnostic,
                "Command"),
            "canonical Z weakened or polluted strict non-Z verification");

        auto frame = items.front();
        frame.Frame = static_cast<std::uint8_t>(MAV_FRAME_GLOBAL);
        const MismatchResult frameResult = RunFirstMismatch(items, frame);
        Expect(frameResult.phase == MissionUploadPhase::Failed &&
            frameResult.safeDiagnostic ==
                "First Mission item verification failed." &&
            frameResult.mismatchDiagnostic.find(
                "FirstItemMismatch ReplyType=MISSION_ITEM_INT "
                "ArtifactRouteIndex=0 WireMissionSeq=1 ") == 0 &&
            HasExactFieldMask(frameResult.mismatchDiagnostic, "Frame"),
            "Frame mismatch diagnostic was not exact");

        auto command = items.front();
        ++command.Command;
        Expect(HasExactFieldMask(
            RunFirstMismatch(items, command).mismatchDiagnostic, "Command"),
            "Command mismatch diagnostic was not exact");

        auto param1 = items.front();
        param1.Param1 = mission::Binary32FromBits(
            mission::Binary32ToBits(param1.Param1) + 1);
        const MismatchResult param1Result = RunFirstMismatch(items, param1);
        Expect(HasExactFieldMask(param1Result.mismatchDiagnostic, "Param1") &&
            param1Result.mismatchDiagnostic.find("param1Bits=0x") !=
                std::string::npos,
            "Param1 numeric/bit mismatch diagnostic was incomplete");

        auto x = items.front();
        ++x.X;
        Expect(HasExactFieldMask(
            RunFirstMismatch(items, x).mismatchDiagnostic, "X"),
            "X mismatch diagnostic was not exact");

        auto y = items.front();
        ++y.Y;
        Expect(HasExactFieldMask(
            RunFirstMismatch(items, y).mismatchDiagnostic, "Y"),
            "Y mismatch diagnostic was not exact");

        auto z = items.front();
        z.Z = mission::Binary32FromBits(mission::Binary32ToBits(z.Z) + 1);
        const MismatchResult zResult = RunFirstMismatch(items, z);
        const auto readZDiagnostic = [&zResult](const char* const key)
        {
            const std::size_t keyBegin = zResult.mismatchDiagnostic.find(key);
            if (keyBegin == std::string::npos)
                throw std::runtime_error("Z comparison diagnostic key was absent");
            const std::size_t valueBegin = keyBegin + std::char_traits<char>::length(key);
            const std::size_t valueEnd =
                zResult.mismatchDiagnostic.find(' ', valueBegin);
            const std::string text = zResult.mismatchDiagnostic.substr(
                valueBegin, valueEnd - valueBegin);
            std::istringstream input(text);
            double value = 0.0;
            input >> value;
            if (!input || !input.eof())
                throw std::runtime_error("Z comparison diagnostic was not numeric");
            return value;
        };
        Expect(HasExactFieldMask(zResult.mismatchDiagnostic, "Z") &&
            zResult.mismatchDiagnostic.find(" ExpectedZ=") !=
                std::string::npos &&
            zResult.mismatchDiagnostic.find(" ActualZ=") !=
                std::string::npos &&
            zResult.mismatchDiagnostic.find(" DeltaZ=") !=
                std::string::npos &&
            static_cast<float>(readZDiagnostic("ExpectedZ=")) ==
                items.front().Z &&
            static_cast<float>(readZDiagnostic("ActualZ=")) == z.Z &&
            readZDiagnostic("DeltaZ=") ==
                static_cast<double>(z.Z) -
                static_cast<double>(items.front().Z) &&
            zResult.mismatchDiagnostic.find(" z=") != std::string::npos &&
            zResult.mismatchDiagnostic.find(" zBits=0x") != std::string::npos,
            "Z numeric/bit mismatch diagnostic was incomplete");

        auto autoContinue = items.front();
        autoContinue.AutoContinue = 0;
        Expect(HasExactFieldMask(
            RunFirstMismatch(items, autoContinue).mismatchDiagnostic,
            "AutoContinue"),
            "AutoContinue mismatch diagnostic was not exact");

        auto multiple = items.front();
        ++multiple.Sequence;
        multiple.Frame = static_cast<std::uint8_t>(MAV_FRAME_GLOBAL);
        ++multiple.Command;
        multiple.AutoContinue = 0;
        multiple.Param1 = mission::Binary32FromBits(
            mission::Binary32ToBits(multiple.Param1) + 1);
        multiple.Param2 = mission::Binary32FromBits(
            mission::Binary32ToBits(multiple.Param2) + 1);
        multiple.Param3 = mission::Binary32FromBits(
            mission::Binary32ToBits(multiple.Param3) + 1);
        multiple.Param4 = mission::Binary32FromBits(1);
        ++multiple.X;
        ++multiple.Y;
        multiple.Z = mission::Binary32FromBits(
            mission::Binary32ToBits(multiple.Z) + 1);
        const MismatchResult multipleResult = RunFirstMismatch(items, multiple);
        Expect(HasExactFieldMask(multipleResult.mismatchDiagnostic,
                "Seq,Frame,Command,AutoContinue,Param1,Param2,Param3,Param4,X,Y,Z") &&
            multipleResult.mismatchDiagnostic.size() <=
                MissionVerificationMismatchDiagnosticMaxBytes &&
            multipleResult.rejectedLateMessage,
            "multi-field mismatch ordering or bounded one-shot behavior drifted");

        FakeTransport currentTransport;
        auto currentOnly = MakeTransactionWithMission(currentTransport, items);
        const auto startedAt = std::chrono::steady_clock::time_point(
            std::chrono::milliseconds(2000));
        const auto acceptedAt = startedAt + std::chrono::milliseconds(100);
        AdvanceToVerifyingFirst(currentOnly, items, startedAt, acceptedAt,
            acceptedAt + std::chrono::milliseconds(10));
        auto currentChanged = items.front();
        currentChanged.Sequence = 1;
        currentChanged.Current = 1;
        Expect(currentOnly.HandleMissionMessage(Item(currentChanged), Peer(),
                acceptedAt + std::chrono::milliseconds(100)) &&
            currentOnly.GetPhase() == MissionUploadPhase::VerifyingLast &&
            currentOnly.GetVerificationMismatchDiagnostic().empty(),
            "ignored Current field changed the verification contract");
        Expect(currentOnly.HandleMissionMessage(
                Item(ArduPilotReadbackItem(wireItems.back())), Peer(),
                acceptedAt + std::chrono::milliseconds(200)) &&
            currentOnly.Succeeded() &&
            currentOnly.GetVerificationMismatchDiagnostic().empty(),
            "Current-only difference prevented successful verification");

        FakeTransport lastTransport;
        auto lastTx = MakeTransactionWithMission(lastTransport, items);
        const auto lastStart = std::chrono::steady_clock::time_point(
            std::chrono::milliseconds(3000));
        const auto lastAccepted = lastStart + std::chrono::milliseconds(100);
        AdvanceToVerifyingFirst(lastTx, items, lastStart, lastAccepted,
            lastAccepted + std::chrono::milliseconds(10));
        Expect(lastTx.HandleMissionMessage(
                Item(ArduPilotReadbackItem(wireItems[1])), Peer(),
                lastAccepted + std::chrono::milliseconds(100)),
            "last mismatch fixture did not pass first verification");
        auto badLast = ArduPilotReadbackItem(items.back());
        badLast.Sequence = 3;
        ++badLast.Command;
        Expect(lastTx.HandleMissionMessage(Item(badLast), Peer(),
                lastAccepted + std::chrono::milliseconds(250)) &&
            lastTx.GetPhase() == MissionUploadPhase::Failed &&
            lastTx.GetVerificationMismatchDiagnostic().find(
                "LastItemMismatch ReplyType=MISSION_ITEM_INT "
                "ArtifactRouteIndex=2 WireMissionSeq=3 ") == 0 &&
            HasExactFieldMask(
                lastTx.GetVerificationMismatchDiagnostic(), "Command"),
            "LastItemMismatch diagnostic was not exact");

        std::vector<mission::MissionItemIntV1> golden = items;
        golden.front() = {};
        golden.front().Sequence = 0;
        golden.front().Frame = 3;
        golden.front().Command = 16;
        golden.front().Current = 0;
        golden.front().AutoContinue = 1;
        golden.front().Param1 = mission::Binary32FromBits(0x43140000);
        golden.front().Param2 = mission::Binary32FromBits(0x41D00000);
        golden.front().Param3 = mission::Binary32FromBits(0x43580000);
        golden.front().Param4 = mission::Binary32FromBits(0x00000000);
        golden.front().X = 319324710;
        golden.front().Y = 1186385310;
        golden.front().Z = mission::Binary32FromBits(0xC1B1CB26);
        golden.front().MissionType = 0;
        auto goldenActual = golden.front();
        goldenActual.Frame = static_cast<std::uint8_t>(MAV_FRAME_GLOBAL);
        const std::string goldenDiagnostic =
            RunFirstMismatch(golden, goldenActual).mismatchDiagnostic;
        Expect(goldenDiagnostic.find(
                "ArtifactRouteIndex=0 WireMissionSeq=1") != std::string::npos &&
            goldenDiagnostic.find(
                "Expected{seq=1 frame=3 command=16 current=0 autocontinue=1") !=
                std::string::npos &&
            goldenDiagnostic.find("param1=148 param1Bits=0x43140000") !=
                std::string::npos &&
            goldenDiagnostic.find("param2=26 param2Bits=0x41D00000") !=
                std::string::npos &&
            goldenDiagnostic.find("param3=216 param3Bits=0x43580000") !=
                std::string::npos &&
            goldenDiagnostic.find("x=319324710 y=1186385310") !=
                std::string::npos &&
            goldenDiagnostic.find("zBits=0xC1B1CB26 mission_type=0}") !=
                std::string::npos,
            "RouteSlot0 golden Expected diagnostic drifted");

        Expect(frameResult.metrics.uploadDurationMs == 100 &&
            frameResult.metrics.verificationDurationMs == 250,
            "ACK-authoritative deterministic verification timing drifted");
    }

    void RunPreparationTests()
    {
		std::cout << "  preparation: basic fixtures" << std::endl;
        TemporaryRoute fixture;
        const std::string valid = BuildWpl(3);
        MissionPreparationRequest request = MakePreparation(fixture.root, valid, 3);
        MissionPreparationResult result =
            MissionPreparationWorker::PrepareForTest(request);
        Expect(result.Succeeded() && result.mission.size() == 3,
            "valid preparation failed");

        MissionPreparationRequest overWireLimit = request;
        overWireLimit.target.missionItemCount = static_cast<std::uint32_t>(
            ardupilotMission::MaximumArtifactRouteItems + 1);
        Expect(MissionPreparationWorker::PrepareForTest(overWireLimit).error ==
                MissionPreparationError::MissionConversionFailed,
            "preparation did not reject Artifact count that overflows wire count");

        MissionPreparationRequest missing = request;
        missing.target.relativePath = "routes/missing.waypoints";
        Expect(MissionPreparationWorker::PrepareForTest(missing).error ==
            MissionPreparationError::RouteMissing,
            "missing route was not rejected");
        MissionPreparationRequest size = request;
        ++size.target.fileByteSize;
        Expect(MissionPreparationWorker::PrepareForTest(size).error ==
            MissionPreparationError::RouteSizeMismatch,
            "size mismatch was not rejected");
        MissionPreparationRequest digest = request;
        digest.target.routeArtifactDigest.Bytes[0] ^= 1;
        Expect(MissionPreparationWorker::PrepareForTest(digest).error ==
            MissionPreparationError::RouteDigestMismatch,
            "digest mismatch was not rejected");
        MissionPreparationRequest rows = request;
        rows.target.missionItemCount = 4;
        Expect(MissionPreparationWorker::PrepareForTest(rows).error ==
            MissionPreparationError::WaypointValidationFailed,
            "ExpectedRows mismatch was not rejected");

        std::atomic_bool cancelled{ true };
        Expect(MissionPreparationWorker::PrepareForTest(request, &cancelled).error ==
            MissionPreparationError::Cancelled,
            "cooperative cancellation was not honored");

		std::cout << "  preparation: 9000 items" << std::endl;
        const std::string large = BuildWpl(9000);
        MissionPreparationRequest largeRequest =
            MakePreparation(fixture.root, large, 9000);
        const MissionPreparationResult largeResult =
            MissionPreparationWorker::PrepareForTest(largeRequest);
        Expect(largeResult.Succeeded() && largeResult.mission.size() == 9000,
            "valid 9000-item preparation failed");

		std::cout << "  preparation: worker cancellation" << std::endl;
        MissionPreparationWorker worker;
        Expect(worker.Submit(largeRequest), "bounded worker rejected first request");
        Expect(!worker.Submit(largeRequest),
            "bounded worker accepted a second pending request");
        worker.CancelActive();
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        MissionPreparationResult completion;
        while (!worker.Poll(completion) &&
            std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        Expect(completion.error == MissionPreparationError::Cancelled ||
            completion.Succeeded(), "bounded worker did not complete or cancel");
		std::cout << "  preparation: worker shutdown" << std::endl;
        worker.Shutdown();
		std::cout << "  preparation: complete" << std::endl;
    }

    class FakeMissionEndpoint final : public IMavlinkDatagramSender
    {
    public:
        struct Sent
        {
            std::uint16_t port = 0;
            mavlink_message_t message{};
        };

        bool SendTo(const std::string& targetIp, const std::uint16_t targetPort,
            const std::uint8_t* data, const int length) override
        {
            if (targetIp != "127.0.0.1" || !data || length <= 0) return false;
            mavlink_message_t buffer{};
            mavlink_status_t status{};
            mavlink_message_t decoded{};
            mavlink_status_t decodedStatus{};
            bool complete = false;
            for (int index = 0; index < length; ++index)
            {
                if (mavlink_frame_char_buffer(&buffer, &status, data[index],
                    &decoded, &decodedStatus) == MAVLINK_FRAMING_OK)
                {
                    complete = true;
                    break;
                }
            }
            if (!complete) return false;
            sent.push_back({ targetPort, decoded });
            return true;
        }

        std::vector<Sent> Take()
        {
            std::vector<Sent> result;
            result.swap(sent);
            return result;
        }

        std::size_t CountMessage(const std::uint32_t messageId) const
        {
            return static_cast<std::size_t>(std::count_if(sent.begin(), sent.end(),
                [messageId](const Sent& value)
                { return value.message.msgid == messageId; }));
        }

        std::vector<Sent> sent;
    };

    void VerifyProductionTransportFactoryAndOwnership()
    {
        FakeMissionEndpoint endpoint;
        DroneSessionConfig sessionConfig;
        sessionConfig.droneId = "FactoryDrone";
        sessionConfig.remoteIp = "127.0.0.1";
        sessionConfig.remotePort = 15100;
        sessionConfig.sharedMavlinkEndpoint = &endpoint;
        sessionConfig.receiveOnly = true;
        sessionConfig.mavFtpServiceEnabled = true;
        sessionConfig.missionUploadServiceEnabled = true;
        sessionConfig.discoveredSystemId = 12;
        sessionConfig.discoveredComponentId = MAV_COMP_ID_AUTOPILOT1;
        sessionConfig.mavFtpWriteWindow = 16;
        sessionConfig.mavFtpRequestTimeoutMs = 750;
        sessionConfig.mavFtpTerminateTimeoutMs = 2500;
        DroneSession session(sessionConfig);
        Expect(session.Start(), "factory DroneSession failed to start");
        auto* const ownedClient = session.GetMavFtpClient();
        Expect(ownedClient != nullptr, "factory Session omitted MAVFTP Client");

        RouteUploadManagerConfig mavftpConfig;
        mavftpConfig.routeUploadEnabled = true;
        mavftpConfig.transportMode = MissionTransportMode::MAVFTP;
        mavftpConfig.mavftpSegmentItems = 4000;
        mavftpConfig.mavftpWriteWindow = 16;
        std::string diagnostic;
        {
            auto transport = CreateConfiguredMissionTransport(
                session, mavftpConfig, diagnostic);
            auto* const mavftp = dynamic_cast<
                dronegcs::mission_ftp::MAVFTPMissionTransport*>(transport.get());
            Expect(mavftp != nullptr && diagnostic.empty() &&
                !mavftp->DrivesClientTickForTest() &&
                session.GetMavFtpClient() == ownedClient &&
                ownedClient->GetConfiguredWriteWindow() == 16,
                "factory did not borrow the Session Client with one Tick owner");
        }
        Expect(session.GetMavFtpClient() == ownedClient,
            "destroying a Job Transport destroyed the Session Client");

        RouteUploadManagerConfig rollbackConfig;
        rollbackConfig.routeUploadEnabled = true;
        rollbackConfig.transportMode = MissionTransportMode::MissionProtocol;
        auto rollback = CreateConfiguredMissionTransport(
            session, rollbackConfig, diagnostic);
        Expect(dynamic_cast<MissionProtocolTransport*>(rollback.get()) != nullptr,
            "explicit MissionProtocol rollback did not create the old Transport");

        DroneSessionConfig missingClientConfig = sessionConfig;
        missingClientConfig.droneId = "NoFtpDrone";
        missingClientConfig.remotePort = 15101;
        missingClientConfig.mavFtpServiceEnabled = false;
        DroneSession missingClient(missingClientConfig);
        Expect(missingClient.Start(), "missing-Client Session failed to start");
        auto noFallback = CreateConfiguredMissionTransport(
            missingClient, mavftpConfig, diagnostic);
        Expect(!noFallback && diagnostic.find("unavailable") != std::string::npos,
            "explicit MAVFTP construction failure silently fell back");
    }

    void FeedMessage(MavlinkReadOnlyIngress& ingress,
        const mavlink_message_t& message, const std::uint16_t port)
    {
        std::uint8_t bytes[MAVLINK_MAX_PACKET_LEN]{};
        const std::uint16_t length = mavlink_msg_to_send_buffer(bytes, &message);
        const MavlinkIngressStats stats = ingress.SubmitDatagram(
            bytes, length, "127.0.0.1", port);
        Expect(stats.missionMessagesApplied == 1,
            "fake vehicle response did not traverse production ingress");
    }

    struct FakeVehicle
    {
        std::uint8_t systemId = 0;
        std::uint16_t port = 0;
        std::vector<mission::MissionItemIntV1> items;
        bool corruptFirstVerification = false;
        bool corruptLastVerification = false;
    };

    void PumpVehicles(FakeMissionEndpoint& endpoint,
        MavlinkReadOnlyIngress& ingress,
        const std::map<std::uint16_t, FakeVehicle>& vehicles)
    {
        for (;;)
        {
            std::vector<FakeMissionEndpoint::Sent> sent = endpoint.Take();
            if (sent.empty()) return;
            for (const auto& outbound : sent)
            {
                const auto vehicle = vehicles.find(outbound.port);
                Expect(vehicle != vehicles.end(), "message targeted unknown fake Drone");
                mavlink_message_t response{};
                if (outbound.message.msgid == MAVLINK_MSG_ID_MISSION_COUNT)
                {
                    mavlink_mission_count_t count{};
                    mavlink_msg_mission_count_decode(&outbound.message, &count);
                    Expect(count.count == vehicle->second.items.size() + 1,
                        "production MISSION_COUNT omitted reserved Home slot");
                    mavlink_msg_mission_request_int_pack(vehicle->second.systemId,
                        MAV_COMP_ID_AUTOPILOT1, &response, 255,
                        MAV_COMP_ID_MISSIONPLANNER, 0, MAV_MISSION_TYPE_MISSION);
                    FeedMessage(ingress, response, outbound.port);
                }
                else if (outbound.message.msgid == MAVLINK_MSG_ID_MISSION_ITEM_INT)
                {
                    mavlink_mission_item_int_t item{};
                    mavlink_msg_mission_item_int_decode(&outbound.message, &item);
                    if (item.seq + 1 < vehicle->second.items.size() + 1)
                    {
                        mavlink_msg_mission_request_int_pack(vehicle->second.systemId,
                            MAV_COMP_ID_AUTOPILOT1, &response, 255,
                            MAV_COMP_ID_MISSIONPLANNER,
                            static_cast<std::uint16_t>(item.seq + 1),
                            MAV_MISSION_TYPE_MISSION);
                    }
                    else
                    {
                        mavlink_msg_mission_ack_pack(vehicle->second.systemId,
                            MAV_COMP_ID_AUTOPILOT1, &response, 255,
                            MAV_COMP_ID_MISSIONPLANNER, MAV_MISSION_ACCEPTED,
                            MAV_MISSION_TYPE_MISSION, 0);
                    }
                    FeedMessage(ingress, response, outbound.port);
                }
                else if (outbound.message.msgid == MAVLINK_MSG_ID_MISSION_REQUEST_LIST)
                {
                    mavlink_msg_mission_count_pack(vehicle->second.systemId,
                        MAV_COMP_ID_AUTOPILOT1, &response, 255,
                        MAV_COMP_ID_MISSIONPLANNER,
                        static_cast<std::uint16_t>(vehicle->second.items.size() + 1),
                        MAV_MISSION_TYPE_MISSION, 0);
                    FeedMessage(ingress, response, outbound.port);
                }
                else if (outbound.message.msgid == MAVLINK_MSG_ID_MISSION_REQUEST_INT)
                {
                    mavlink_mission_request_int_t request{};
                    mavlink_msg_mission_request_int_decode(&outbound.message, &request);
                    Expect(request.seq >= 1 &&
                        request.seq <= vehicle->second.items.size(),
                        "verification requested an invalid sequence");
                    auto item = vehicle->second.items[request.seq - 1];
                    item.Sequence = request.seq;
                    item = ArduPilotReadbackItem(item);
                    if (vehicle->second.corruptFirstVerification &&
                        request.seq == 1)
                    {
                        item.Frame = static_cast<std::uint8_t>(MAV_FRAME_GLOBAL);
                    }
                    if (vehicle->second.corruptLastVerification &&
                        request.seq == vehicle->second.items.size())
                    {
                        ++item.Command;
                    }
                    mavlink_msg_mission_item_int_pack(vehicle->second.systemId,
                        MAV_COMP_ID_AUTOPILOT1, &response, 255,
                        MAV_COMP_ID_MISSIONPLANNER, item.Sequence, item.Frame,
                        item.Command, item.Current, item.AutoContinue,
                        item.Param1, item.Param2, item.Param3, item.Param4,
                        item.X, item.Y, item.Z, MAV_MISSION_TYPE_MISSION);
                    FeedMessage(ingress, response, outbound.port);
                }
                else
                {
                    throw std::runtime_error("production uploader sent a forbidden MAVLink message");
                }
            }
        }
    }

    dspgcs::UploadJobCommandAckDTO SendManagerCommand(
        ConsoleUploadJobAuthority& authority, const std::string& jobId,
        const dspgcs::UploadJobCommandType commandType,
        const std::uint32_t requestedConcurrency = 0)
    {
        static std::uint32_t commandSequence = 0;
        const auto* snapshot = authority.FindCurrentSnapshotForTest(jobId);
        Expect(snapshot != nullptr, "manager command snapshot is missing");
        dspgcs::UploadJobCommandDTO command;
        const char idDigit = static_cast<char>('0' + (++commandSequence % 10));
        command.clientCommandId = std::string(32, idDigit);
        command.uploadJobId = jobId;
        command.commandType = commandType;
        command.expectedJobRevision = snapshot->jobRevision;
        command.requestedMaxConcurrentUploads = requestedConcurrency;
        return authority.HandleCommand("mission-manager-test", command, {});
    }

    void RunManagerScenario(const std::uint32_t maxConcurrency,
        const bool cancelWhileUploading = false,
        const bool firstTargetInitiallyOffline = false,
        const bool shutdownWhileUploading = false,
        const bool firstVerificationMismatch = false,
        const bool lastVerificationMismatch = false)
    {
        ScopedCoutCapture runtimeLog(
            firstVerificationMismatch || lastVerificationMismatch);
        TemporaryRoute fixture;
        const std::size_t TargetCount =
            firstVerificationMismatch || lastVerificationMismatch ? 1 : 5;
        const std::string routeBytes = BuildWpl(3);
        artifact::Digest256 digest;
        Expect(artifact::Sha256::Hash(routeBytes.data(), routeBytes.size(), digest),
            "manager fixture digest failed");
        std::vector<dspgcs::UploadRouteStateDTO> routes;
        std::vector<UploadJobTargetMetadata> metadata;
        auto artifactSnapshot = std::make_shared<AcceptedRouteArtifactV2>();
        artifactSnapshot->resolvedCurrentRoot = fixture.root.u8string();
        artifactSnapshot->frameCount = 3;
        for (std::size_t index = 0; index < TargetCount; ++index)
        {
            const std::string relative = "routes/" + std::to_string(index) +
                ".waypoints";
            std::ofstream stream(fixture.root / fs::u8path(relative),
                std::ios::binary | std::ios::trunc);
            stream.write(routeBytes.data(),
                static_cast<std::streamsize>(routeBytes.size()));
            dspgcs::UploadRouteStateDTO route;
            route.routeSlotIndex = static_cast<std::int32_t>(index);
            route.droneId = "Drone00" + std::to_string(index + 1);
            route.totalBytes = 3 * mission::MissionItemRecordBytesV1;
            routes.push_back(route);
            UploadJobTargetMetadata target;
            target.droneId = route.droneId;
            target.routeSlotIndex = route.routeSlotIndex;
            target.relativePath = relative;
            target.fileByteSize = static_cast<std::int64_t>(routeBytes.size());
            target.missionItemCount = 3;
            target.missionPayloadByteSize = route.totalBytes;
            target.routeArtifactDigest = digest;
            metadata.push_back(target);
        }

        ConsoleUploadJobAuthority authority(fixture.root.u8string(), nullptr, nullptr);
        const std::string jobId(32, static_cast<char>('c' + maxConcurrency));
        std::string error;
        Expect(authority.InstallSyntheticReadyJobForTest(jobId, {}, {}, {},
            routes, 0, 0, 0, maxConcurrency, error),
            "synthetic manager job failed");
        Expect(authority.InstallSyntheticExecutionBindingForTest(jobId,
            artifactSnapshot, metadata, error),
            "synthetic execution binding failed");

        FakeMissionEndpoint endpoint;
        DroneManager drones;
        DroneSessionConfig defaults;
        defaults.receiveOnly = true;
        defaults.missionUploadServiceEnabled = true;
        defaults.sharedMavlinkEndpoint = &endpoint;
        defaults.heartbeatTimeoutMs = 30000;
        MavlinkDiscoveryService discovery(drones, defaults);
        std::map<std::uint16_t, FakeVehicle> vehicles;
        for (std::size_t index = 0; index < TargetCount; ++index)
        {
            const std::uint8_t systemId = static_cast<std::uint8_t>(index + 1);
            const std::uint16_t port = static_cast<std::uint16_t>(15000 + index);
            mavlink_message_t heartbeat{};
            mavlink_msg_heartbeat_pack(systemId, MAV_COMP_ID_AUTOPILOT1,
                &heartbeat, MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
                0, 0, MAV_STATE_ACTIVE);
            if (!firstTargetInitiallyOffline || index != 0)
                Expect(discovery.IngestMessage(heartbeat, "127.0.0.1", port) ==
                    MavlinkDiscoveryResult::Registered,
                    "fake Drone discovery failed");
            vehicles.emplace(port, FakeVehicle{
                systemId, port, BuildWplMission(3),
                firstVerificationMismatch && index == 0,
                lastVerificationMismatch && index == 0 });
        }
        MavlinkReadOnlyIngress ingress(drones, discovery);
        RouteUploadManager manager(authority, drones, true);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(10);
        while (endpoint.CountMessage(MAVLINK_MSG_ID_MISSION_COUNT) <
                maxConcurrency && std::chrono::steady_clock::now() < deadline)
        {
            manager.Tick(std::chrono::steady_clock::now());
            std::this_thread::yield();
        }
        Expect(manager.GetActiveSlotCountForTest() == maxConcurrency,
            "scheduler exceeded or failed to fill configured concurrency");
        Expect(manager.GetPreparedMissionCountForTest() <= maxConcurrency &&
            manager.GetWorkerThreadCountForTest() == 1 &&
            manager.GetSocketCountForTest() == 0 &&
            manager.GetTimerCountForTest() == 0,
            "scheduler resource bounds were violated");

        if (shutdownWhileUploading)
        {
            manager.Shutdown();
            const auto* stopped = authority.FindCurrentSnapshotForTest(jobId);
            Expect(stopped && stopped->jobState ==
                    dspgcs::UploadJobState::Cancelled &&
                !authority.GetExecutionView(jobId).has_value() &&
                manager.GetActiveSlotCountForTest() == 0,
                "manager shutdown did not quiesce and release authority lease");
            return;
        }

        if (cancelWhileUploading)
        {
            const auto acknowledgement = SendManagerCommand(authority, jobId,
                dspgcs::UploadJobCommandType::Cancel);
            Expect(acknowledgement.status ==
                dspgcs::UploadJobCommandAckStatus::Accepted,
                "active manager Cancel was rejected");
            while (std::chrono::steady_clock::now() < deadline)
            {
                manager.Tick(std::chrono::steady_clock::now());
                const auto* cancelled = authority.FindCurrentSnapshotForTest(jobId);
                if (cancelled && cancelled->jobState ==
                    dspgcs::UploadJobState::Cancelled)
                    break;
                std::this_thread::yield();
            }
            const auto* cancelled = authority.FindCurrentSnapshotForTest(jobId);
            Expect(cancelled && cancelled->jobState ==
                    dspgcs::UploadJobState::Cancelled &&
                cancelled->cancelledCount == TargetCount &&
                manager.GetActiveSlotCountForTest() == 0,
                "manager Cancel did not quiesce active upload and finalize authority");
            return;
        }

        if (maxConcurrency == 2)
        {
            const auto pause = SendManagerCommand(authority, jobId,
                dspgcs::UploadJobCommandType::Pause);
            Expect(pause.status == dspgcs::UploadJobCommandAckStatus::Accepted,
                "manager Pause was rejected");
            PumpVehicles(endpoint, ingress, vehicles);
            manager.Tick(std::chrono::steady_clock::now());
            const auto* paused = authority.FindCurrentSnapshotForTest(jobId);
            Expect(paused && paused->jobState == dspgcs::UploadJobState::Paused &&
                manager.GetActiveSlotCountForTest() == 0 &&
                paused->waitingCount == TargetCount - maxConcurrency,
                "Pause did not drain active targets while blocking new admission");
            const auto resume = SendManagerCommand(authority, jobId,
                dspgcs::UploadJobCommandType::Resume);
            Expect(resume.status == dspgcs::UploadJobCommandAckStatus::Accepted,
                "manager Resume was rejected");
        }
        else if (maxConcurrency == 3)
        {
            const auto lower = SendManagerCommand(authority, jobId,
                dspgcs::UploadJobCommandType::SetMaxConcurrentUploads, 1);
            Expect(lower.status == dspgcs::UploadJobCommandAckStatus::Accepted &&
                manager.GetActiveSlotCountForTest() == 3,
                "3-to-1 SetMax interrupted active transactions");
            PumpVehicles(endpoint, ingress, vehicles);
            manager.Tick(std::chrono::steady_clock::now());
            while (manager.GetActiveSlotCountForTest() < 1 &&
                std::chrono::steady_clock::now() < deadline)
            {
                manager.Tick(std::chrono::steady_clock::now());
                std::this_thread::yield();
            }
            Expect(manager.GetActiveSlotCountForTest() == 1,
                "lowered concurrency did not stop admission at the new bound");
            const auto raise = SendManagerCommand(authority, jobId,
                dspgcs::UploadJobCommandType::SetMaxConcurrentUploads, 3);
            Expect(raise.status == dspgcs::UploadJobCommandAckStatus::Accepted,
                "1-to-3 SetMax was rejected");
            while (manager.GetActiveSlotCountForTest() < 2 &&
                std::chrono::steady_clock::now() < deadline)
            {
                manager.Tick(std::chrono::steady_clock::now());
                std::this_thread::yield();
            }
            Expect(manager.GetActiveSlotCountForTest() >= 2 &&
                manager.GetActiveSlotCountForTest() <= 3,
                "raised concurrency did not replenish bounded admission");
        }

        while (std::chrono::steady_clock::now() < deadline)
        {
            PumpVehicles(endpoint, ingress, vehicles);
            manager.Tick(std::chrono::steady_clock::now());
            const auto* snapshot = authority.FindCurrentSnapshotForTest(jobId);
            if (snapshot && (snapshot->jobState ==
                    dspgcs::UploadJobState::Completed ||
                snapshot->jobState == dspgcs::UploadJobState::Failed))
                break;
            std::this_thread::yield();
        }
        const auto* snapshot = authority.FindCurrentSnapshotForTest(jobId);
        if (firstVerificationMismatch || lastVerificationMismatch)
        {
            const std::string runtimeOutput = runtimeLog.Stop();
            const auto& terminalMetrics = manager.GetTargetMetricsForTest();
            const std::string expectedPrefix = firstVerificationMismatch
                ? "FirstItemMismatch ReplyType=MISSION_ITEM_INT "
                  "ArtifactRouteIndex=0 WireMissionSeq=1 "
                : "LastItemMismatch ReplyType=MISSION_ITEM_INT "
                  "ArtifactRouteIndex=2 WireMissionSeq=3 ";
            Expect(snapshot && snapshot->jobState == dspgcs::UploadJobState::Failed &&
                snapshot->failedCount == 1 && terminalMetrics.size() == 1 &&
                terminalMetrics.front().verificationMismatchDiagnostic.find(
                    expectedPrefix) == 0 &&
                manager.GetLastTerminalDiagnosticForTest().find(
                    expectedPrefix) != std::string::npos &&
                manager.GetLastTerminalDiagnosticForTest().find("Expected{") !=
                    std::string::npos &&
                manager.GetLastTerminalDiagnosticForTest().find("Actual{") !=
                    std::string::npos &&
                runtimeOutput.find(expectedPrefix) != std::string::npos &&
                runtimeOutput.find("FieldMask=") != std::string::npos &&
                runtimeOutput.find("ExpectedZ=") != std::string::npos &&
                runtimeOutput.find("ActualZ=") != std::string::npos &&
                runtimeOutput.find("DeltaZ=") != std::string::npos &&
                runtimeOutput.find("Expected{") != std::string::npos &&
                runtimeOutput.find("Actual{") != std::string::npos,
                "manager did not expose the bounded verification mismatch");
            return;
        }
        if (firstTargetInitiallyOffline)
        {
            Expect(snapshot && snapshot->jobState == dspgcs::UploadJobState::Failed &&
                snapshot->failedCount == 1 &&
                snapshot->succeededCount == TargetCount - 1,
                "one offline target blocked or corrupted independent targets");
            const auto original = authority.FindTargetMetadataForTest(jobId, 0);
            mavlink_message_t heartbeat{};
            mavlink_msg_heartbeat_pack(1, MAV_COMP_ID_AUTOPILOT1,
                &heartbeat, MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
                0, 0, MAV_STATE_ACTIVE);
            Expect(discovery.IngestMessage(heartbeat, "127.0.0.1", 15000) ==
                MavlinkDiscoveryResult::Registered,
                "retry target did not return online");
            const auto retry = SendManagerCommand(authority, jobId,
                dspgcs::UploadJobCommandType::RetryFailed);
            Expect(retry.status == dspgcs::UploadJobCommandAckStatus::Accepted,
                "manager RetryFailed was rejected");
            while (std::chrono::steady_clock::now() < deadline)
            {
                manager.Tick(std::chrono::steady_clock::now());
                PumpVehicles(endpoint, ingress, vehicles);
                manager.Tick(std::chrono::steady_clock::now());
                snapshot = authority.FindCurrentSnapshotForTest(jobId);
                if (snapshot && snapshot->jobState ==
                    dspgcs::UploadJobState::Completed)
                    break;
                std::this_thread::yield();
            }
            const auto afterRetry = authority.FindTargetMetadataForTest(jobId, 0);
            snapshot = authority.FindCurrentSnapshotForTest(jobId);
            Expect(snapshot && snapshot->jobState ==
                    dspgcs::UploadJobState::Completed &&
                snapshot->succeededCount == TargetCount &&
                original && afterRetry &&
                original->routeArtifactDigest == afterRetry->routeArtifactDigest &&
                original->relativePath == afterRetry->relativePath,
                "RetryFailed did not create a fresh successful transaction on the immutable binding");
            return;
        }
        Expect(snapshot && snapshot->jobState == dspgcs::UploadJobState::Completed &&
            snapshot->succeededCount == TargetCount &&
            snapshot->bytesCompleted == snapshot->totalBytes,
            "fake production chain did not complete authoritative Job state");
		const auto& terminalMetrics = manager.GetTargetMetricsForTest();
		Expect(terminalMetrics.size() == TargetCount &&
			!terminalMetrics.front().terminalPhase.empty() &&
			terminalMetrics.front().uniqueItemsServed ==
				terminalMetrics.front().missionItemCount + 1 &&
			terminalMetrics.front().artifactRouteItems ==
				terminalMetrics.front().missionItemCount &&
			terminalMetrics.front().wireMissionItems ==
				terminalMetrics.front().missionItemCount + 1 &&
			terminalMetrics.front().missionRequestsReceived >=
				terminalMetrics.front().missionItemCount + 1 &&
			manager.GetLastTerminalDiagnosticForTest().find("TerminalPhase=") !=
				std::string::npos &&
			manager.GetLastTerminalDiagnosticForTest().find(
				"ArtifactRouteItems=3 WireMissionItems=4") != std::string::npos &&
			manager.GetLastTerminalDiagnosticForTest().find("WireBytesSent=") !=
				std::string::npos &&
			manager.GetLastJobSummaryForTest().find("PeakPreparedMissions=") !=
				std::string::npos,
			"terminal target/job diagnostics omitted required metrics");
        Expect(manager.GetJobMetricsForTest().peakActiveUploads <= maxConcurrency,
            "measured active upload peak exceeded max concurrency");
        Expect(!authority.DrainRouteDeltas().empty(),
            "production chain did not publish route deltas for UI mirror");
    }

    void RunManagerIntegrationTests()
    {
        VerifyProductionTransportFactoryAndOwnership();
        RunManagerScenario(1);
        RunManagerScenario(2);
        RunManagerScenario(3);
        RunManagerScenario(1, true);
        RunManagerScenario(2, false, true);
        RunManagerScenario(1, false, false, true);
        RunManagerScenario(1, false, false, false, true);
        RunManagerScenario(1, false, false, false, false, true);
    }

    void RunSchedulerScale(const std::size_t targetCount)
    {
        TemporaryRoute fixture;
        std::vector<dspgcs::UploadRouteStateDTO> routes;
        std::vector<UploadJobTargetMetadata> metadata;
        routes.reserve(targetCount);
        metadata.reserve(targetCount);
        artifact::Digest256 digest;
        digest.IsSet = true;
        for (std::size_t index = 0; index < targetCount; ++index)
        {
            dspgcs::UploadRouteStateDTO route;
            route.routeSlotIndex = static_cast<std::int32_t>(index);
            route.droneId = "ScaleDrone" + std::to_string(index);
            route.totalBytes = mission::MissionItemRecordBytesV1;
            routes.push_back(route);
            UploadJobTargetMetadata target;
            target.droneId = route.droneId;
            target.routeSlotIndex = route.routeSlotIndex;
            target.relativePath = "routes/unused.waypoints";
            target.fileByteSize = 1;
            target.missionItemCount = 1;
            target.missionPayloadByteSize = route.totalBytes;
            target.routeArtifactDigest = digest;
            metadata.push_back(std::move(target));
        }
        auto snapshot = std::make_shared<AcceptedRouteArtifactV2>();
        snapshot->resolvedCurrentRoot = fixture.root.u8string();
        snapshot->frameCount = 1;
        ConsoleUploadJobAuthority authority(fixture.root.u8string(), nullptr, nullptr);
        const std::string jobId(32, targetCount == 1000 ? '1' : '2');
        std::string error;
        Expect(authority.InstallSyntheticReadyJobForTest(jobId, {}, {}, {},
            std::move(routes), 0, 0, 0, 3, error),
            "scale synthetic Job failed");
        Expect(authority.InstallSyntheticExecutionBindingForTest(jobId,
            std::move(snapshot), std::move(metadata), error),
            "scale execution binding failed");
        DroneManager drones;
        RouteUploadManager manager(authority, drones, false);
        manager.Tick(std::chrono::steady_clock::now());
        const auto* final = authority.FindCurrentSnapshotForTest(jobId);
        Expect(final && final->jobState == dspgcs::UploadJobState::Failed &&
            final->failedCount == targetCount,
            "disabled scale Job did not fail closed deterministically");
        Expect(manager.GetJobMetricsForTest().targetCount == targetCount &&
            manager.GetJobMetricsForTest().peakActiveUploads == 0 &&
            manager.GetJobMetricsForTest().peakPreparedMissions == 0 &&
            manager.GetWorkerThreadCountForTest() == 1 &&
            manager.GetSocketCountForTest() == 0 &&
            manager.GetTimerCountForTest() == 0,
            "scale scheduler resource cardinality grew with Fleet size");
    }

    void RunScaleTests()
    {
        RunSchedulerScale(1000);
        RunSchedulerScale(10000);
    }
}

int RunMissionUploadTests()
{
    try
    {
        std::cout << "Mission upload tests: transactions" << std::endl;
        RunTransactionTests();
		std::cout << "Mission upload tests: transport abstraction" << std::endl;
		RunMissionTransportAbstractionTests();
		std::cout << "Mission upload tests: verification instrumentation" << std::endl;
		RunVerificationInstrumentationTests();
        std::cout << "Mission upload tests: preparation" << std::endl;
        RunPreparationTests();
		std::cout << "Mission upload tests: manager integration" << std::endl;
        RunManagerIntegrationTests();
		std::cout << "Mission upload tests: scale" << std::endl;
        RunScaleTests();
        std::cout << "Mission Protocol upload/preparation tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Mission Protocol upload tests failed: " << error.what() << '\n';
        return 1;
    }
}
