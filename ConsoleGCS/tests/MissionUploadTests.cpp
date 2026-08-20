#include "MissionPreparationWorker.h"
#include "MissionUploadTransaction.h"
#include "MavlinkDatagramSender.h"
#include "MavlinkDiscoveryService.h"
#include "MavlinkReadOnlyIngress.h"
#include "RouteUploadManager.h"
#include "RouteArtifactContract.h"
#include "Wpl110Core.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace mission = dronegcs::route_mission;
    namespace wpl = dronegcs::wpl110;
    namespace artifact = dronegcs::route_artifact;

    void Expect(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

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
            items.push_back(item.Sequence); bytes = 50; return allowSend;
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

    void CompleteUpload(MissionUploadTransaction& tx,
        const std::vector<mission::MissionItemIntV1>& items,
        const std::chrono::steady_clock::time_point now)
    {
        for (std::uint16_t sequence = 0; sequence < items.size(); ++sequence)
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
            static_cast<std::uint16_t>(items.size()));
        Expect(tx.HandleMissionMessage(count, Peer(), now),
            "verification count was not consumed");
        const mavlink_message_t first = Item(items.front());
        Expect(tx.HandleMissionMessage(first, Peer(), now),
            "first verification item was not consumed");
        const mavlink_message_t last = Item(items.back());
        Expect(tx.HandleMissionMessage(last, Peer(), now),
            "last verification item was not consumed");
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
        FakeTransport transport;
        auto tx = MakeTransaction(transport);
        Expect(tx.Start(now), "transaction did not start");
        Expect(transport.counts == std::vector<std::uint16_t>{ 3 },
            "MISSION_COUNT was not authoritative");

        const mavlink_message_t legacy = Request(0, true);
        Expect(tx.HandleMissionMessage(legacy, Peer(), now),
            "legacy MISSION_REQUEST was not accepted");
        const mavlink_message_t duplicate = Request(0);
        Expect(tx.HandleMissionMessage(duplicate, Peer(), now),
            "duplicate request was not idempotent");
        Expect(tx.GetMetrics().logicalBytesCompleted ==
            mission::MissionItemRecordBytesV1,
            "duplicate request advanced logical progress");
        Expect(tx.GetMetrics().duplicateRequests == 1,
            "duplicate request metric was not recorded");
        for (std::uint16_t sequence = 1; sequence < items.size(); ++sequence)
        {
            const mavlink_message_t request = Request(sequence);
            Expect(tx.HandleMissionMessage(request, Peer(), now),
                "MISSION_REQUEST_INT was not accepted");
        }
        const mavlink_message_t accepted = Ack(MAV_MISSION_ACCEPTED);
        Expect(tx.HandleMissionMessage(accepted, Peer(), now),
            "MISSION_ACK accepted was not consumed");
        Expect(tx.TransportAccepted() && !tx.Succeeded(),
            "transport acceptance bypassed verification");
        const mavlink_message_t count = Count(3);
        tx.HandleMissionMessage(count, Peer(), now);
        const mavlink_message_t first = Item(items.front());
        tx.HandleMissionMessage(first, Peer(), now);
        const mavlink_message_t last = Item(items.back());
        tx.HandleMissionMessage(last, Peer(), now);
        Expect(tx.Succeeded(), "full accepted/count/first/last gate failed");

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
        for (std::uint16_t sequence = 0; sequence < 3; ++sequence)
        {
            const mavlink_message_t request = Request(sequence);
            rejected.HandleMissionMessage(request, Peer(), now);
        }
        const mavlink_message_t noSpace = Ack(MAV_MISSION_NO_SPACE);
        rejected.HandleMissionMessage(noSpace, Peer(), now);
        Expect(rejected.GetPhase() == MissionUploadPhase::Failed,
            "NO_SPACE did not fail target");

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
        for (std::uint16_t sequence = 0; sequence < 3; ++sequence)
        {
            const mavlink_message_t request = Request(sequence);
            mismatch.HandleMissionMessage(request, Peer(), now);
        }
        mismatch.HandleMissionMessage(accepted, Peer(), now);
        const mavlink_message_t badCount = Count(4);
        mismatch.HandleMissionMessage(badCount, Peer(), now);
        Expect(mismatch.GetPhase() == MissionUploadPhase::Failed,
            "verification count mismatch did not fail");

        FakeTransport firstMismatchTransport;
        auto firstMismatch = MakeTransaction(firstMismatchTransport);
        firstMismatch.Start(now);
        for (std::uint16_t sequence = 0; sequence < 3; ++sequence)
        {
            const mavlink_message_t request = Request(sequence);
            firstMismatch.HandleMissionMessage(request, Peer(), now);
        }
        firstMismatch.HandleMissionMessage(accepted, Peer(), now);
        firstMismatch.HandleMissionMessage(count, Peer(), now);
        auto badFirstItem = items.front();
        ++badFirstItem.X;
        const mavlink_message_t badFirst = Item(badFirstItem);
        firstMismatch.HandleMissionMessage(badFirst, Peer(), now);
        Expect(firstMismatch.GetPhase() == MissionUploadPhase::Failed,
            "first item mismatch did not fail");

        FakeTransport lastMismatchTransport;
        auto lastMismatch = MakeTransaction(lastMismatchTransport);
        lastMismatch.Start(now);
        for (std::uint16_t sequence = 0; sequence < 3; ++sequence)
        {
            const mavlink_message_t request = Request(sequence);
            lastMismatch.HandleMissionMessage(request, Peer(), now);
        }
        lastMismatch.HandleMissionMessage(accepted, Peer(), now);
        lastMismatch.HandleMissionMessage(count, Peer(), now);
        lastMismatch.HandleMissionMessage(first, Peer(), now);
        auto badLastItem = items.back();
        ++badLastItem.Command;
        const mavlink_message_t badLast = Item(badLastItem);
        lastMismatch.HandleMissionMessage(badLast, Peer(), now);
        Expect(lastMismatch.GetPhase() == MissionUploadPhase::Failed,
            "last item mismatch did not fail");

        MissionUploadTransactionConfig timeoutConfig;
        timeoutConfig.responseTimeout = std::chrono::milliseconds(1);
        timeoutConfig.maxRetries = 2;
        FakeTransport timeoutTransport;
        auto timeout = MakeTransaction(timeoutTransport, timeoutConfig);
        timeout.Start(now);
        timeout.Tick(now + std::chrono::milliseconds(2));
        timeout.Tick(now + std::chrono::milliseconds(4));
        timeout.Tick(now + std::chrono::milliseconds(6));
        Expect(timeout.GetPhase() == MissionUploadPhase::Failed &&
            timeout.GetMetrics().retryCount == 2,
            "timeout retry budget was not bounded");

        FakeTransport cancelTransport;
        auto cancelled = MakeTransaction(cancelTransport);
        cancelled.Start(now);
        cancelled.Cancel();
        Expect(cancelled.GetPhase() == MissionUploadPhase::Cancelled &&
            !cancelled.HandleMissionMessage(accepted, Peer(), now) &&
            !cancelled.HandleMissionMessage(request0, Peer(), now) &&
            cancelTransport.items.empty(),
            "late ACK or stale request advanced cancelled transaction");
    }

    void RunPreparationTests()
    {
        TemporaryRoute fixture;
        const std::string valid = BuildWpl(3);
        MissionPreparationRequest request = MakePreparation(fixture.root, valid, 3);
        MissionPreparationResult result =
            MissionPreparationWorker::PrepareForTest(request);
        Expect(result.Succeeded() && result.mission.size() == 3,
            "valid preparation failed");

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

        const std::string large = BuildWpl(9000);
        MissionPreparationRequest largeRequest =
            MakePreparation(fixture.root, large, 9000);
        const MissionPreparationResult largeResult =
            MissionPreparationWorker::PrepareForTest(largeRequest);
        Expect(largeResult.Succeeded() && largeResult.mission.size() == 9000,
            "valid 9000-item preparation failed");

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
        worker.Shutdown();
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
                    Expect(count.count == vehicle->second.items.size(),
                        "production MISSION_COUNT differed from prepared route");
                    mavlink_msg_mission_request_int_pack(vehicle->second.systemId,
                        MAV_COMP_ID_AUTOPILOT1, &response, 255,
                        MAV_COMP_ID_MISSIONPLANNER, 0, MAV_MISSION_TYPE_MISSION);
                    FeedMessage(ingress, response, outbound.port);
                }
                else if (outbound.message.msgid == MAVLINK_MSG_ID_MISSION_ITEM_INT)
                {
                    mavlink_mission_item_int_t item{};
                    mavlink_msg_mission_item_int_decode(&outbound.message, &item);
                    if (item.seq + 1 < vehicle->second.items.size())
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
                        static_cast<std::uint16_t>(vehicle->second.items.size()),
                        MAV_MISSION_TYPE_MISSION, 0);
                    FeedMessage(ingress, response, outbound.port);
                }
                else if (outbound.message.msgid == MAVLINK_MSG_ID_MISSION_REQUEST_INT)
                {
                    mavlink_mission_request_int_t request{};
                    mavlink_msg_mission_request_int_decode(&outbound.message, &request);
                    Expect(request.seq < vehicle->second.items.size(),
                        "verification requested an invalid sequence");
                    const auto& item = vehicle->second.items[request.seq];
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
        const bool shutdownWhileUploading = false)
    {
        TemporaryRoute fixture;
        constexpr std::size_t TargetCount = 5;
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
            vehicles.emplace(port, FakeVehicle{ systemId, port, BuildWplMission(3) });
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
        Expect(manager.GetJobMetricsForTest().peakActiveUploads <= maxConcurrency,
            "measured active upload peak exceeded max concurrency");
        Expect(!authority.DrainRouteDeltas().empty(),
            "production chain did not publish route deltas for UI mirror");
    }

    void RunManagerIntegrationTests()
    {
        RunManagerScenario(1);
        RunManagerScenario(2);
        RunManagerScenario(3);
        RunManagerScenario(1, true);
        RunManagerScenario(2, false, true);
        RunManagerScenario(1, false, false, true);
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
        RunTransactionTests();
        RunPreparationTests();
        RunManagerIntegrationTests();
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
