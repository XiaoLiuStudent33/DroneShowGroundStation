#include "MAVFTPMissionTransport.h"
#include "ArduPilotMissionWireAdapter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    namespace ftp = dronegcs::mavftp;
    namespace compiler = dronegcs::mission_ftp;
    namespace mission = dronegcs::route_mission;

    void Expect(const bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    ftp::PeerIdentity TestPeer()
    {
        return { "Drone-Writer", "127.0.0.1", 31001, 7, 1 };
    }

    MissionTransportIdentity TestIdentity()
    {
        MissionTransportIdentity identity;
        identity.uploadJobId = "Writer-Test-Job";
        identity.droneId = TestPeer().droneId;
        identity.routeSlotIndex = 0;
        identity.generation = 1;
        return identity;
    }

    std::vector<mission::MissionItemIntV1> BuildArtifact(
        const std::size_t count)
    {
        std::vector<mission::MissionItemIntV1> items(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            auto& item = items[index];
            item.Sequence = static_cast<std::uint16_t>(index);
            item.Command = 16;
            item.Frame = 3;
            item.X = 319324710 + static_cast<std::int32_t>(index);
            item.Y = 1186385310 + static_cast<std::int32_t>(index);
            item.Z = 30.0f + static_cast<float>(index);
            item.AutoContinue = 1;
            item.MissionType = mission::LegacyMissionTypeV1;
        }
        return items;
    }

    class FakeWire final : public ftp::IRequestTransport
    {
    public:
        bool SendMavFtpPayload(
            const std::array<std::uint8_t, ftp::PayloadBytes>& payload) override
        {
            sent.push_back(payload);
            return sendSucceeds;
        }

        ftp::Payload Decode(const std::size_t index) const
        {
            Expect(index < sent.size(), "fake MAVFTP request index is invalid");
            ftp::Payload payload;
            ftp::Error error;
            Expect(ftp::DecodePayload(sent[index].data(), sent[index].size(),
                payload, error), "captured MAVFTP request did not decode");
            return payload;
        }

        ftp::Payload Last() const
        {
            Expect(!sent.empty(), "fake MAVFTP transport is empty");
            return Decode(sent.size() - 1);
        }

        std::array<std::uint8_t, ftp::PayloadBytes> Reply(
            const ftp::Payload& request,
            const ftp::Opcode opcode,
            const std::uint8_t session,
            std::vector<std::uint8_t> data = {},
            const std::uint32_t offset = UINT32_MAX) const
        {
            ftp::Payload response;
            response.sequence = ftp::NextSequence(request.sequence);
            response.session = session;
            response.opcode = opcode;
            response.requestOpcode = request.opcode;
            response.offset = offset == UINT32_MAX ? request.offset : offset;
            response.data = std::move(data);
            std::array<std::uint8_t, ftp::PayloadBytes> wire{};
            ftp::Error error;
            Expect(ftp::EncodePayload(response, wire, error),
                "fake MAVFTP response did not encode");
            return wire;
        }

        void Ack(
            ftp::Client& client,
            const ftp::Payload& request,
            const std::uint8_t session = 23,
            std::vector<std::uint8_t> data = {},
            const std::uint32_t offset = UINT32_MAX)
        {
            const auto wire = Reply(request, ftp::Opcode::Ack, session,
                std::move(data), offset);
            if (!client.HandleResponse(TestPeer(), wire.data(), wire.size()))
            {
                throw std::runtime_error(
                    std::string("fake MAVFTP ACK was not accepted for ") +
                    ftp::OpcodeName(request.opcode) +
                    " seq=" + std::to_string(request.sequence) +
                    " session=" + std::to_string(session));
            }
        }

        bool sendSucceeds = true;
        std::vector<std::array<std::uint8_t, ftp::PayloadBytes>> sent;
    };

    MissionVerificationPeerIdentity VerificationPeer()
    {
        return { TestPeer().droneId, TestPeer().remoteIp,
            TestPeer().remotePort, TestPeer().systemId, TestPeer().componentId };
    }

    class FakeMissionWire final : public IMissionVerificationTransport
    {
    public:
        bool SendMissionRequestList(std::size_t& bytes) override
            { ++requestLists; bytes = 16; return true; }
        bool SendMissionRequestInt(std::uint16_t sequence,
            std::size_t& bytes) override
            { requested.push_back(sequence); bytes = 18; return true; }

        std::size_t requestLists = 0;
        std::vector<std::uint16_t> requested;
    };

    mavlink_message_t VerificationCount(const std::uint16_t count)
    {
        mavlink_message_t message{};
        mavlink_msg_mission_count_pack(TestPeer().systemId,
            TestPeer().componentId, &message, 255,
            MAV_COMP_ID_MISSIONPLANNER, count, MAV_MISSION_TYPE_MISSION, 0);
        return message;
    }

    mavlink_message_t VerificationItem(
        const mission::MissionItemIntV1& item)
    {
        mavlink_message_t message{};
        mavlink_msg_mission_item_int_pack(TestPeer().systemId,
            TestPeer().componentId, &message, 255,
            MAV_COMP_ID_MISSIONPLANNER, item.Sequence, item.Frame,
            item.Command, item.Current, item.AutoContinue, item.Param1,
            item.Param2, item.Param3, item.Param4, item.X, item.Y, item.Z,
            MAV_MISSION_TYPE_MISSION);
        return message;
    }

    struct CompiledFixture
    {
        compiler::CompileResult compiled;
        compiler::MAVFTPMissionTransportConfig writerConfig;
    };

    CompiledFixture CompileFixture(
        const std::size_t artifactItems,
        const std::uint32_t segmentCapacity,
        const std::size_t blockBytes)
    {
        CompiledFixture fixture;
        fixture.writerConfig.compileConfig.segmentCapacity = segmentCapacity;
        fixture.writerConfig.writeBlockBytes = blockBytes;
        fixture.compiled = compiler::MissionFtpCompiler::Compile(
            BuildArtifact(artifactItems), fixture.writerConfig.compileConfig);
        Expect(fixture.compiled.IsValid(), "writer fixture did not compile");
        return fixture;
    }

    bool AppendBytes(
        const std::uint8_t* bytes,
        const std::size_t size,
        void* const context)
    {
        auto& output = *static_cast<std::vector<std::uint8_t>*>(context);
        output.insert(output.end(), bytes, bytes + size);
        return true;
    }

    std::vector<std::uint8_t> ExpectedSegment(
        const compiler::CompiledMissionSnapshot& snapshot,
        const std::size_t index)
    {
        std::vector<std::uint8_t> result;
        const auto validation = compiler::MissionFtpCompiler::StreamSegment(
            snapshot, index, &AppendBytes, &result);
        Expect(validation.IsValid(), "expected segment stream failed");
        return result;
    }

    struct DriveResult
    {
        std::vector<std::vector<std::uint8_t>> segmentBytes;
        std::vector<ftp::Opcode> requests;
    };

    DriveResult DriveSuccess(
        compiler::MAVFTPMissionTransport& writer,
        ftp::Client& client,
        FakeWire& wire,
        const std::chrono::steady_clock::time_point now)
    {
        DriveResult result;
        std::vector<std::uint8_t>* activeSegment = nullptr;
        std::size_t cursor = 0;
        std::uint8_t session = 23;
        for (std::size_t guard = 0; guard < 10000 && !writer.IsTerminal(); ++guard)
        {
            Expect(cursor < wire.sent.size(),
                "writer stopped before reaching a terminal state");
            const ftp::Payload request = wire.Decode(cursor++);
            result.requests.push_back(request.opcode);
            switch (request.opcode)
            {
            case ftp::Opcode::CreateFile:
                Expect(std::string(request.data.begin(), request.data.end()) ==
                    compiler::MAVFTPMissionTransport::MissionPath,
                    "writer CreateFile path is not @MISSION/mission.dat");
                result.segmentBytes.emplace_back();
                activeSegment = &result.segmentBytes.back();
                wire.Ack(client, request, session);
                break;
            case ftp::Opcode::WriteFile:
                Expect(activeSegment != nullptr,
                    "WriteFile arrived before CreateFile");
                Expect(request.offset == activeSegment->size(),
                    "WriteFile offsets are not contiguous");
                activeSegment->insert(activeSegment->end(),
                    request.data.begin(), request.data.end());
                // Canonical ArduPilot WriteFile ACK: opcode/session/offset,
                // with no returned byte-count payload.
                wire.Ack(client, request, session);
                break;
            case ftp::Opcode::TerminateSession:
                wire.Ack(client, request, session);
                writer.Tick(now);
                ++session;
                break;
            default:
                throw std::runtime_error("writer sent an unexpected MAVFTP opcode");
            }
            writer.Tick(now);
        }
        Expect(writer.IsTerminal(), "writer fake server guard was exhausted");
        return result;
    }

    void AckRemainingRequests(
        compiler::MAVFTPMissionTransport& writer,
        ftp::Client& client,
        FakeWire& wire,
        std::size_t cursor,
        const std::chrono::steady_clock::time_point now)
    {
        std::uint8_t session = 23;
        for (std::size_t guard = 0; guard < 10000 && !writer.IsTerminal(); ++guard)
        {
            Expect(cursor < wire.sent.size(),
                "writer stopped while ACKing remaining requests");
            const ftp::Payload request = wire.Decode(cursor++);
            Expect(request.opcode == ftp::Opcode::CreateFile ||
                request.opcode == ftp::Opcode::WriteFile ||
                request.opcode == ftp::Opcode::TerminateSession,
                "writer emitted an unexpected remaining request");
            wire.Ack(client, request, session);
            writer.Tick(now);
            if (request.opcode == ftp::Opcode::TerminateSession) ++session;
        }
        Expect(writer.IsTerminal(), "remaining ACK loop exhausted its guard");
    }

    void TestCreateWriteTerminateSuccess()
    {
        FakeWire wire;
        ftp::Client client(wire, TestPeer());
        auto fixture = CompileFixture(5, 32, 31);
        compiler::MAVFTPMissionTransport writer(client, fixture.writerConfig);
        std::string diagnostic;
        Expect(writer.PrepareCompiled(TestIdentity(), fixture.compiled.snapshot,
            diagnostic), "compiled writer prepare failed: " + diagnostic);
        const auto now = std::chrono::steady_clock::now();
        Expect(writer.Upload(now), "compiled writer upload did not start");
        const DriveResult driven = DriveSuccess(writer, client, wire, now);
        Expect(writer.GetStatus() == MissionTransportStatus::Succeeded &&
            writer.Verify() &&
            std::string(writer.GetTerminalPhaseName()) == "CommitConfirmed",
            "Terminate ACK did not commit the writer");
        Expect(driven.segmentBytes.size() == 1 &&
            driven.segmentBytes.front() ==
                ExpectedSegment(*fixture.compiled.snapshot, 0),
            "writer bytes differ from MissionFtpCompiler::StreamSegment");
        const auto metrics = writer.GetMavFtpMetrics();
        Expect(metrics.segmentsPlanned == 1 &&
            metrics.segmentsCommitted == 1 &&
            metrics.activeSegmentIndex == 0 &&
            metrics.segmentCapacity == 32 &&
            metrics.writeBytes == driven.segmentBytes.front().size() &&
            metrics.writeBlocks > 1,
            "writer success metrics are incomplete");
    }

    void TestAcknowledgedLogicalProgressBeforeTerminate()
    {
        FakeWire wire;
        ftp::Client client(wire, TestPeer());
        auto fixture = CompileFixture(5, 32, ftp::MaximumDataBytes);
        compiler::MAVFTPMissionTransport writer(client, fixture.writerConfig);
        std::string diagnostic;
        Expect(writer.PrepareCompiled(TestIdentity(), fixture.compiled.snapshot,
            diagnostic), "live progress writer prepare failed: " + diagnostic);
        const auto now = std::chrono::steady_clock::now();
        Expect(writer.Upload(now), "live progress writer upload did not start");

        const ftp::Payload create = wire.Decode(0);
        Expect(create.opcode == ftp::Opcode::CreateFile,
            "live progress did not start with CreateFile");
        wire.Ack(client, create, 23);
        writer.Tick(now);
        const ftp::Payload write = wire.Decode(1);
        Expect(write.opcode == ftp::Opcode::WriteFile,
            "live progress did not issue WriteFile");
        wire.Ack(client, write, 23);
        writer.Tick(now);

        const std::uint64_t expectedLogicalBytes =
            5 * mission::MissionItemRecordBytesV1;
        Expect(client.GetAcknowledgedWriteBytes() == write.data.size() &&
            client.GetAcknowledgedWriteBlocks() == 1 &&
            writer.GetMetrics().logicalBytesCompleted == expectedLogicalBytes &&
            writer.GetMavFtpMetrics().segmentsCommitted == 0 &&
            writer.GetStatus() == MissionTransportStatus::WaitingAck,
            "Write ACK did not expose logical progress before Terminate ACK");
        Expect(writer.ConsumeProgressChanged(),
            "Write ACK did not raise the bounded progress signal");

        const ftp::Payload terminate = wire.Decode(2);
        Expect(terminate.opcode == ftp::Opcode::TerminateSession,
            "live progress did not preserve Terminate gate");
        wire.Ack(client, terminate, 23);
        writer.Tick(now);
        Expect(writer.Verify() &&
            writer.GetMetrics().logicalBytesCompleted == expectedLogicalBytes,
            "Terminate ACK changed acknowledged logical progress");
    }

    void TestCancelFreezesLatestAcknowledgedLogicalProgress()
    {
        FakeWire wire;
        ftp::Client client(wire, TestPeer());
        auto fixture = CompileFixture(50, 64, ftp::MaximumDataBytes);
        compiler::MAVFTPMissionTransport writer(client, fixture.writerConfig);
        std::string diagnostic;
        Expect(writer.PrepareCompiled(TestIdentity(), fixture.compiled.snapshot,
            diagnostic), "cancel progress writer prepare failed: " + diagnostic);
        const auto now = std::chrono::steady_clock::now();
        Expect(writer.Upload(now), "cancel progress writer upload did not start");
        const ftp::Payload create = wire.Decode(0);
        wire.Ack(client, create, 29);
        writer.Tick(now);
        const ftp::Payload write = wire.Decode(1);
        wire.Ack(client, write, 29);

        writer.Cancel(now);
        const std::uint64_t frozen = writer.GetMetrics().logicalBytesCompleted;
        Expect(writer.GetStatus() == MissionTransportStatus::Cancelled &&
            frozen > 0 && frozen <
                50 * mission::MissionItemRecordBytesV1,
            "Cancel discarded or fabricated the latest ACK progress");
        writer.Tick(now + std::chrono::seconds(1));
        Expect(writer.GetMetrics().logicalBytesCompleted == frozen,
            "Cancelled progress continued after the terminal state");
    }

    void TestProductionCutoverLifecycleAndTickOwnership()
    {
        FakeWire ftpWire;
        ftp::ClientConfig clientConfig;
        clientConfig.writeWindow = 16;
        clientConfig.requestTimeout = std::chrono::milliseconds(750);
        clientConfig.terminateTimeout = std::chrono::milliseconds(2500);
        clientConfig.maximumRetries = 3;
        ftp::Client client(ftpWire, TestPeer(), clientConfig);
        FakeMissionWire verificationWire;
        compiler::MAVFTPMissionTransportConfig config;
        config.compileConfig.segmentCapacity = 4;
        config.driveClientTick = false;
        config.verificationConfig.responseTimeout =
            std::chrono::milliseconds(750);
        config.verificationConfig.maxRetries = 3;
        const auto artifact = BuildArtifact(10);
        compiler::MAVFTPMissionTransport transport(
            client, verificationWire, VerificationPeer(), config);
        std::string diagnostic;
        Expect(transport.Prepare(TestIdentity(), artifact, diagnostic),
            "production MAVFTP Transport Prepare failed: " + diagnostic);
        const auto now = std::chrono::steady_clock::now();
        Expect(transport.Upload(now) && !transport.DrivesClientTickForTest(),
            "production MAVFTP Transport owns Client Tick or failed to start");

        std::size_t cursor = 0;
        std::size_t terminateCount = 0;
        std::uint8_t session = 40;
        for (std::size_t guard = 0;
             guard < 10000 &&
             transport.GetStatus() != MissionTransportStatus::Verifying;
             ++guard)
        {
            Expect(cursor < ftpWire.sent.size(),
                "production writer stopped before Verification");
            const ftp::Payload request = ftpWire.Decode(cursor++);
            ftpWire.Ack(client, request, session);
            if (request.opcode == ftp::Opcode::TerminateSession)
            {
                ++terminateCount;
                ++session;
            }
            transport.Tick(now);
        }
        Expect(transport.GetStatus() == MissionTransportStatus::Verifying &&
            terminateCount == 3 && verificationWire.requestLists == 1,
            "4+4+3 upload did not wait for three Terminate ACKs before Verification");

        std::vector<mission::MissionItemIntV1> wireMission;
        Expect(dronegcs::ardupilot_mission::BuildWireMission(
                artifact, wireMission),
            "production verification wire fixture failed");
        IMissionVerificationMessageSink* const sink =
            transport.GetMissionVerificationMessageSink();
        Expect(sink != nullptr && sink->HandleMissionMessage(
                VerificationCount(static_cast<std::uint16_t>(wireMission.size())),
                VerificationPeer(), now) &&
            verificationWire.requested == std::vector<std::uint16_t>{ 0 },
            "production verification did not start with Count then Home");
        Expect(sink->HandleMissionMessage(
                VerificationItem(wireMission[0]), VerificationPeer(), now) &&
            sink->HandleMissionMessage(
                VerificationItem(wireMission[1]), VerificationPeer(), now) &&
            sink->HandleMissionMessage(
                VerificationItem(wireMission.back()), VerificationPeer(), now),
            "production Count/Home/First/Last responses were rejected");
        transport.Tick(now);
        Expect(transport.Verify() &&
            transport.GetStatus() == MissionTransportStatus::Succeeded &&
            std::string(transport.GetTerminalPhaseName()) == "Verified" &&
            client.GetConfiguredWriteWindow() == 16 &&
            client.GetConfiguredRequestTimeout() == std::chrono::milliseconds(750) &&
            client.GetConfiguredTerminateTimeout() == std::chrono::milliseconds(2500) &&
            client.GetPeer().droneId == TestPeer().droneId,
            "production MAVFTP verification or frozen Client configuration failed");
    }

    void TestWriteTimeoutRetriesSameBlock()
    {
        FakeWire wire;
        ftp::ClientConfig clientConfig;
        clientConfig.requestTimeout = std::chrono::milliseconds(1);
        clientConfig.maximumRetries = 1;
        ftp::Client client(wire, TestPeer(), clientConfig);
        auto fixture = CompileFixture(2, 32, 17);
        compiler::MAVFTPMissionTransport writer(client, fixture.writerConfig);
        std::string diagnostic;
        Expect(writer.PrepareCompiled(TestIdentity(), fixture.compiled.snapshot,
            diagnostic), "retry writer prepare failed");
        const auto now = std::chrono::steady_clock::now();
        Expect(writer.Upload(now), "retry writer upload did not start");
        const auto firstCreate = wire.sent.back();
        writer.Tick(now + std::chrono::seconds(2));
        Expect(wire.sent.size() == 2 && wire.sent.back() == firstCreate,
            "Create retry changed sequence/path/segment");
        wire.Ack(client, wire.Last(), 23);
        const auto firstWrite = wire.sent.back();
        writer.Tick(now + std::chrono::seconds(4));
        Expect(wire.sent.size() == 4 && wire.sent.back() == firstWrite,
            "Write retry changed sequence/session/offset/data");
        wire.Ack(client, wire.Last(), 23);
        AckRemainingRequests(writer, client, wire, 4,
            std::chrono::steady_clock::now());
        Expect(writer.GetStatus() == MissionTransportStatus::Succeeded &&
            writer.GetMavFtpMetrics().retryCount == 2,
            "Create/Write retries did not complete or report both retries");
    }

    void TestOffsetMismatchFails()
    {
        FakeWire wire;
        ftp::Client client(wire, TestPeer());
        auto fixture = CompileFixture(2, 32, 19);
        compiler::MAVFTPMissionTransport writer(client, fixture.writerConfig);
        std::string diagnostic;
        Expect(writer.PrepareCompiled(TestIdentity(), fixture.compiled.snapshot,
            diagnostic), "offset writer prepare failed");
        const auto now = std::chrono::steady_clock::now();
        Expect(writer.Upload(now), "offset writer upload did not start");
        wire.Ack(client, wire.Last(), 23);
        const ftp::Payload write = wire.Last();
        Expect(write.opcode == ftp::Opcode::WriteFile,
            "offset fixture did not reach WriteFile");
        wire.Ack(client, write, 23, {}, write.offset + 1);
        writer.Tick(now);
        Expect(writer.GetStatus() == MissionTransportStatus::Failed &&
            std::string(writer.GetTerminalPhaseName()) == "OffsetMismatch" &&
            writer.GetSafeDiagnostic().find("OffsetMismatch") != std::string::npos,
            "offset mismatch did not fail closed with a bounded diagnostic");
    }

    void TestTerminateAckTimeoutFails()
    {
        FakeWire wire;
        ftp::ClientConfig clientConfig;
        clientConfig.requestTimeout = std::chrono::milliseconds(1);
        clientConfig.terminateTimeout = std::chrono::milliseconds(1);
        clientConfig.maximumRetries = 1;
        ftp::Client client(wire, TestPeer(), clientConfig);
        auto fixture = CompileFixture(5, 3, ftp::MaximumDataBytes);
        compiler::MAVFTPMissionTransport writer(client, fixture.writerConfig);
        std::string diagnostic;
        Expect(writer.PrepareCompiled(TestIdentity(), fixture.compiled.snapshot,
            diagnostic), "terminate writer prepare failed");
        const auto now = std::chrono::steady_clock::now();
        Expect(writer.Upload(now), "terminate writer upload did not start");
        wire.Ack(client, wire.Last(), 23);
        while (wire.Last().opcode == ftp::Opcode::WriteFile)
            wire.Ack(client, wire.Last(), 23);
        Expect(wire.Last().opcode == ftp::Opcode::TerminateSession,
            "terminate fixture never reached commit gate");
        writer.Tick(now + std::chrono::seconds(2));
        writer.Tick(now + std::chrono::seconds(4));
        std::size_t createRequests = 0;
        for (std::size_t index = 0; index < wire.sent.size(); ++index)
        {
            if (wire.Decode(index).opcode == ftp::Opcode::CreateFile)
                ++createRequests;
        }
        const auto metrics = writer.GetMavFtpMetrics();
        Expect(writer.GetStatus() == MissionTransportStatus::Failed &&
            std::string(writer.GetTerminalPhaseName()) == "TerminateFailed" &&
            !writer.Verify() && createRequests == 1 &&
            metrics.segmentsPlanned == 2 &&
            metrics.segmentsCommitted == 0 &&
            metrics.activeSegmentIndex == 0,
            "missing Terminate ACK committed or advanced beyond the failed segment");
    }

    void TestNextSegmentStartsOnlyAfterTerminateAck()
    {
        FakeWire wire;
        ftp::Client client(wire, TestPeer());
        auto fixture = CompileFixture(5, 3, ftp::MaximumDataBytes);
        compiler::MAVFTPMissionTransport writer(client, fixture.writerConfig);
        std::string diagnostic;
        Expect(writer.PrepareCompiled(TestIdentity(), fixture.compiled.snapshot,
            diagnostic), "commit-gate writer prepare failed");
        const auto now = std::chrono::steady_clock::now();
        Expect(writer.Upload(now), "commit-gate writer upload did not start");
        wire.Ack(client, wire.Last(), 23);
        while (wire.Last().opcode == ftp::Opcode::WriteFile)
            wire.Ack(client, wire.Last(), 23);
        Expect(wire.Last().opcode == ftp::Opcode::TerminateSession,
            "commit-gate fixture never reached TerminateSession");
        const std::size_t beforeTerminateAck = wire.sent.size();
        writer.Tick(now);
        Expect(wire.sent.size() == beforeTerminateAck &&
            writer.GetMavFtpMetrics().segmentsCommitted == 0 &&
            writer.GetMavFtpMetrics().activeSegmentIndex == 0,
            "second segment started before the first Terminate ACK");

        wire.Ack(client, wire.Last(), 23);
        Expect(writer.GetMavFtpMetrics().segmentsCommitted == 0,
            "client ACK bypassed the writer commit gate");
        writer.Tick(now);
        Expect(wire.sent.size() == beforeTerminateAck + 1 &&
            wire.Last().opcode == ftp::Opcode::CreateFile &&
            writer.GetMavFtpMetrics().segmentsCommitted == 1 &&
            writer.GetMavFtpMetrics().activeSegmentIndex == 1,
            "second segment did not start immediately after Terminate ACK commit");
        writer.Cancel(now);
    }

    void TestMultipleSegmentsRemainOrdered()
    {
        FakeWire wire;
        ftp::Client client(wire, TestPeer());
        auto fixture = CompileFixture(7, 3, 23);
        compiler::MAVFTPMissionTransport writer(client, fixture.writerConfig);
        std::string diagnostic;
        Expect(writer.PrepareCompiled(TestIdentity(), fixture.compiled.snapshot,
            diagnostic), "multi-segment writer prepare failed");
        const auto now = std::chrono::steady_clock::now();
        Expect(writer.Upload(now), "multi-segment writer upload did not start");
        const DriveResult driven = DriveSuccess(writer, client, wire, now);
        const auto& descriptors =
            fixture.compiled.snapshot->GetSegmentDescriptors();
        Expect(driven.segmentBytes.size() == descriptors.size() &&
            writer.GetMavFtpMetrics().segmentsCommitted == descriptors.size(),
            "multi-segment writer commit count is wrong");
        for (std::size_t index = 0; index < descriptors.size(); ++index)
        {
            Expect(driven.segmentBytes[index] ==
                ExpectedSegment(*fixture.compiled.snapshot, index),
                "multi-segment writer changed stream order or bytes");
            Expect(descriptors[index].segmentIndex == index &&
                (index == 0
                    ? descriptors[index].options == mission::FirstContainerOptionsV1
                    : descriptors[index].options == mission::SubsequentContainerOptionsV1),
                "multi-segment descriptor options changed");
        }
    }

    class RejectingSource final : public compiler::ISegmentRangeSource
    {
    public:
        bool ReadRange(
            const compiler::CompiledMissionSnapshot&,
            const compiler::SegmentDescriptor&,
            std::size_t,
            std::uint8_t*,
            std::size_t,
            std::string& error) const override
        {
            error = "test consumer rejection";
            return false;
        }
    };

    void TestStreamingRejectionStopsWrites()
    {
        FakeWire wire;
        ftp::Client client(wire, TestPeer());
        auto fixture = CompileFixture(2, 32, 17);
        RejectingSource rejecting;
        compiler::MAVFTPMissionTransport writer(
            client, fixture.writerConfig, &rejecting);
        std::string diagnostic;
        Expect(writer.PrepareCompiled(TestIdentity(), fixture.compiled.snapshot,
            diagnostic), "rejecting writer prepare failed");
        const auto now = std::chrono::steady_clock::now();
        Expect(writer.Upload(now), "rejecting writer upload did not start");
        const std::size_t beforeAck = wire.sent.size();
        wire.Ack(client, wire.Last(), 23);
        writer.Tick(now);
        const bool anyWrite = std::any_of(
            wire.sent.begin() + beforeAck, wire.sent.end(),
            [](const auto& packet)
            {
                ftp::Payload decoded;
                ftp::Error error;
                return ftp::DecodePayload(packet.data(), packet.size(), decoded, error) &&
                    decoded.opcode == ftp::Opcode::WriteFile;
            });
        Expect(!anyWrite && writer.GetStatus() == MissionTransportStatus::Failed &&
            std::string(writer.GetTerminalPhaseName()) == "StreamRejected",
            "stream rejection emitted a WriteFile or failed without diagnosis");
    }

    std::vector<std::uint8_t> Bytes(const std::size_t count)
    {
        std::vector<std::uint8_t> bytes(count);
        for (std::size_t index = 0; index < count; ++index)
            bytes[index] = static_cast<std::uint8_t>(index);
        return bytes;
    }

    void TestBoundedWindowFillOutOfOrderAndMetrics()
    {
        for (const std::uint32_t window : { 1U, 2U, 4U, 8U })
        {
            FakeWire wire;
            ftp::ClientConfig config;
            config.writeWindow = window;
            ftp::Client client(wire, TestPeer(), config);
            std::string error;
            const auto source = Bytes(9 * ftp::MaximumDataBytes + 7);
            Expect(client.StartWriteFile("@MISSION/mission.dat", source, error),
                "window client did not start");
            const ftp::Payload create = wire.Decode(0);
            wire.Ack(client, create, 41);
            Expect(wire.sent.size() == 1 + window,
                "Write window did not fill immediately after Create ACK");

            std::vector<std::uint32_t> acknowledged;
            if (window > 1)
            {
                const ftp::Payload outOfOrder = wire.Decode(window);
                wire.Ack(client, outOfOrder, 41);
                acknowledged.push_back(outOfOrder.offset);
                Expect(wire.sent.size() == 2 + window,
                    "out-of-order ACK did not refill the Write window");
                const auto duplicate = wire.Reply(outOfOrder, ftp::Opcode::Ack, 41);
                Expect(!client.HandleResponse(TestPeer(), duplicate.data(), duplicate.size()),
                    "duplicate Write ACK was accepted twice");
            }

            for (std::size_t guard = 0; guard < 1000; ++guard)
            {
                bool madeProgress = false;
                for (std::size_t index = 1; index < wire.sent.size(); ++index)
                {
                    const ftp::Payload request = wire.Decode(index);
                    if (request.opcode != ftp::Opcode::WriteFile ||
                        std::find(acknowledged.begin(), acknowledged.end(),
                            request.offset) != acknowledged.end())
                        continue;
                    wire.Ack(client, request, 41);
                    acknowledged.push_back(request.offset);
                    madeProgress = true;
                    break;
                }
                if (wire.Last().opcode == ftp::Opcode::TerminateSession) break;
                Expect(madeProgress, "Write window stopped before TerminateSession");
            }
            const ftp::Payload terminate = wire.Last();
            Expect(terminate.opcode == ftp::Opcode::TerminateSession,
                "Write window sent Terminate before all blocks were ACKed or stalled");
            const std::size_t beforeTerminateAck = wire.sent.size();
            Expect(!client.ConsumeResult(),
                "Write operation completed before Terminate ACK");
            wire.Ack(client, terminate, 41);
            const auto result = client.ConsumeResult();
            Expect(result && result->success &&
                result->configuredWriteWindow == window &&
                result->maxObservedOutstanding == window &&
                result->writeBlocksSent == 10 &&
                result->writeBlocksAcked == 10 &&
                result->writeBytes == source.size() &&
                wire.sent.size() == beforeTerminateAck,
                "Write window result or bounded metrics are incorrect");
            if (window > 1)
                Expect(result->duplicateAcks == 1,
                    "duplicate ACK metric was not reported");
        }
    }

    void TestWindowCorrelationAndSequenceWrap()
    {
        FakeWire wire;
        ftp::ClientConfig config;
        config.writeWindow = 4;
        config.initialRequestSequence = 65534;
        ftp::Client client(wire, TestPeer(), config);
        std::string error;
        Expect(client.StartWriteFile("@MISSION/mission.dat",
            Bytes(6 * ftp::MaximumDataBytes), error),
            "sequence-wrap client did not start");
        const ftp::Payload create = wire.Decode(0);
        Expect(create.sequence == 65534,
            "sequence-wrap fixture did not start at uint16 boundary");
        wire.Ack(client, create, 51);
        Expect(wire.Decode(1).sequence == 0 && wire.Decode(2).sequence == 2 &&
            wire.Decode(3).sequence == 4 && wire.Decode(4).sequence == 6,
            "Write request sequence did not wrap without pending reuse");

        const ftp::Payload target = wire.Decode(1);
        auto wrongPeerWire = wire.Reply(target, ftp::Opcode::Ack, 51);
        auto wrongPeer = TestPeer();
        wrongPeer.remotePort++;
        Expect(!client.HandleResponse(wrongPeer,
            wrongPeerWire.data(), wrongPeerWire.size()),
            "wrong-peer ACK was accepted");

        auto wrongSessionWire = wire.Reply(target, ftp::Opcode::Ack, 52);
        Expect(!client.HandleResponse(TestPeer(),
            wrongSessionWire.data(), wrongSessionWire.size()),
            "wrong-session ACK was accepted");

        ftp::Payload wrongSequenceResponse;
        ftp::Error protocolError;
        Expect(ftp::DecodePayload(wrongPeerWire.data(), wrongPeerWire.size(),
            wrongSequenceResponse, protocolError), "wrap response decode failed");
        wrongSequenceResponse.sequence = ftp::NextSequence(
            ftp::NextSequence(wrongSequenceResponse.sequence));
        std::array<std::uint8_t, ftp::PayloadBytes> wrongSequenceWire{};
        Expect(ftp::EncodePayload(wrongSequenceResponse, wrongSequenceWire,
            protocolError), "wrong sequence response encode failed");
        Expect(!client.HandleResponse(TestPeer(), wrongSequenceWire.data(),
            wrongSequenceWire.size()), "wrong-sequence ACK was accepted");

        ftp::Payload wrongOpcodeResponse = wrongSequenceResponse;
        wrongOpcodeResponse.sequence = ftp::NextSequence(target.sequence);
        wrongOpcodeResponse.requestOpcode = ftp::Opcode::ReadFile;
        std::array<std::uint8_t, ftp::PayloadBytes> wrongOpcodeWire{};
        Expect(ftp::EncodePayload(wrongOpcodeResponse, wrongOpcodeWire,
            protocolError), "wrong opcode response encode failed");
        Expect(!client.HandleResponse(TestPeer(), wrongOpcodeWire.data(),
            wrongOpcodeWire.size()), "wrong-opcode ACK was accepted");

        wire.Ack(client, target, 51);
        Expect(client.IsBusy() && client.GetStats().wrongPeerResponses == 1 &&
            client.GetStats().wrongSessionResponses == 1 &&
            client.GetStats().wrongRequestOpcodeResponses == 1 &&
            client.GetStats().staleOrMismatchedWriteAcks >= 3,
            "correlation rejection metrics are incomplete");
        client.Cancel();
        const auto cancelled = client.ConsumeResult();
        Expect(cancelled && cancelled->cancelled && !client.IsBusy(),
            "window cancel did not clear pending blocks");
        const std::size_t sendsAfterCancel = wire.sent.size();
        Expect(!client.HandleResponse(TestPeer(), wrongPeerWire.data(),
            wrongPeerWire.size()) && wire.sent.size() == sendsAfterCancel,
            "late ACK after cancel emitted another request");
    }

    void TestSingleBlockRetryAndWindowNakFailure()
    {
        FakeWire wire;
        ftp::ClientConfig config;
        config.writeWindow = 4;
        config.requestTimeout = std::chrono::milliseconds(1);
        config.maximumRetries = 1;
        ftp::Client client(wire, TestPeer(), config);
        std::string error;
        Expect(client.StartWriteFile("@MISSION/mission.dat",
            Bytes(3 * ftp::MaximumDataBytes), error),
            "retry window did not start");
        wire.Ack(client, wire.Decode(0), 61);
        const ftp::Payload first = wire.Decode(1);
        const ftp::Payload second = wire.Decode(2);
        const ftp::Payload third = wire.Decode(3);
        wire.Ack(client, first, 61);
        wire.Ack(client, second, 61);
        const std::size_t beforeRetry = wire.sent.size();
        client.Tick(std::chrono::steady_clock::now() + std::chrono::seconds(1));
        Expect(wire.sent.size() == beforeRetry + 1 &&
            wire.Last().sequence == third.sequence &&
            wire.Last().session == third.session &&
            wire.Last().offset == third.offset &&
            wire.Last().data == third.data,
            "single timed-out block retry changed immutable identity or payload");
        wire.Ack(client, third, 61);
        Expect(wire.Last().opcode == ftp::Opcode::TerminateSession,
            "retry completion did not reach Terminate gate");
        wire.Ack(client, wire.Last(), 61);
        const auto success = client.ConsumeResult();
        Expect(success && success->success && success->writeRetries == 1 &&
            success->createRetries == 0 && success->terminateRetries == 0,
            "Write retry was not classified independently");

        FakeWire nakWire;
        ftp::Client nakClient(nakWire, TestPeer(), config);
        Expect(nakClient.StartWriteFile("@MISSION/mission.dat",
            Bytes(6 * ftp::MaximumDataBytes), error), "NAK window did not start");
        nakWire.Ack(nakClient, nakWire.Decode(0), 62);
        const ftp::Payload rejected = nakWire.Decode(2);
        const auto nak = nakWire.Reply(rejected, ftp::Opcode::Nak, 62, { 1 });
        const std::size_t beforeNak = nakWire.sent.size();
        Expect(nakClient.HandleResponse(TestPeer(), nak.data(), nak.size()),
            "exact Write NAK was not consumed");
        const auto failed = nakClient.ConsumeResult();
        Expect(failed && !failed->success && !failed->cancelled &&
            !nakClient.IsBusy() && nakWire.sent.size() == beforeNak + 1,
            "Write NAK did not fail once with one best-effort cleanup request");
        const ftp::Payload lateRequest = nakWire.Decode(1);
        const auto late = nakWire.Reply(lateRequest, ftp::Opcode::Ack, 62);
        const std::size_t afterFailure = nakWire.sent.size();
        Expect(!nakClient.HandleResponse(TestPeer(), late.data(), late.size()) &&
            !nakClient.ConsumeResult() && nakWire.sent.size() == afterFailure,
            "late ACK recovered or completed a failed window twice");
    }

    void TestWindowBoundariesExhaustionShutdownAndMetricBound()
    {
        ftp::ClientConfig config;
        config.writeWindow = 8;
        std::string error;

        FakeWire boundaryWire;
        ftp::Client boundaryClient(boundaryWire, TestPeer(), config);
        Expect(!boundaryClient.StartWriteFile(
            "@MISSION/mission.dat", {}, error) && boundaryWire.sent.empty(),
            "empty Write source did not preserve the existing reject contract");
        error.clear();
        Expect(boundaryClient.StartWriteFile("@MISSION/mission.dat",
            Bytes(ftp::MaximumDataBytes + 1), error),
            "small-window boundary source did not start");
        boundaryWire.Ack(boundaryClient, boundaryWire.Decode(0), 71);
        Expect(boundaryWire.sent.size() == 3,
            "file smaller than Window emitted more than its two Write blocks");
        boundaryWire.Ack(boundaryClient, boundaryWire.Decode(1), 71);
        Expect(boundaryWire.sent.size() == 3,
            "small file emitted Terminate before every Write ACK");
        boundaryWire.Ack(boundaryClient, boundaryWire.Decode(2), 71);
        Expect(boundaryWire.sent.size() == 4 &&
            boundaryWire.Last().opcode == ftp::Opcode::TerminateSession,
            "small file did not emit exactly one Terminate after its last ACK");
        boundaryWire.Ack(boundaryClient, boundaryWire.Last(), 71);
        const auto boundaryResult = boundaryClient.ConsumeResult();
        Expect(boundaryResult && boundaryResult->success &&
            boundaryResult->writeBlocksSent == 2 &&
            boundaryResult->writeBlocksAcked == 2 &&
            boundaryResult->maxObservedOutstanding == 2,
            "small-file Window metrics are incorrect");

        FakeWire exhaustionWire;
        config.writeWindow = 4;
        config.requestTimeout = std::chrono::milliseconds(1);
        config.maximumRetries = 1;
        ftp::Client exhaustionClient(exhaustionWire, TestPeer(), config);
        Expect(exhaustionClient.StartWriteFile("@MISSION/mission.dat",
            Bytes(4 * ftp::MaximumDataBytes), error),
            "retry-exhaustion Window did not start");
        exhaustionWire.Ack(exhaustionClient, exhaustionWire.Decode(0), 72);
        const ftp::Payload timedOut = exhaustionWire.Decode(1);
        exhaustionWire.Ack(exhaustionClient, exhaustionWire.Decode(2), 72);
        exhaustionWire.Ack(exhaustionClient, exhaustionWire.Decode(3), 72);
        exhaustionWire.Ack(exhaustionClient, exhaustionWire.Decode(4), 72);
        exhaustionClient.Tick(
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        Expect(exhaustionWire.Last().sequence == timedOut.sequence &&
            exhaustionWire.Last().session == timedOut.session &&
            exhaustionWire.Last().offset == timedOut.offset &&
            exhaustionWire.Last().data == timedOut.data,
            "retry-exhaustion fixture did not resend the immutable Block");
        exhaustionClient.Tick(
            std::chrono::steady_clock::now() + std::chrono::seconds(2));
        const auto exhausted = exhaustionClient.ConsumeResult();
        Expect(exhausted && !exhausted->success &&
            exhausted->writeRetries == 1 && !exhaustionClient.IsBusy(),
            "one exhausted in-flight Block did not fail the whole Window once");
        const auto late = exhaustionWire.Reply(
            timedOut, ftp::Opcode::Ack, 72);
        Expect(!exhaustionClient.HandleResponse(TestPeer(),
            late.data(), late.size()) && !exhaustionClient.ConsumeResult(),
            "late ACK recovered a retry-exhausted Window");

        FakeWire shutdownWire;
        ftp::Client shutdownClient(shutdownWire, TestPeer(), config);
        Expect(shutdownClient.StartWriteFile("@MISSION/mission.dat",
            Bytes(6 * ftp::MaximumDataBytes), error),
            "shutdown Window did not start");
        shutdownWire.Ack(shutdownClient, shutdownWire.Decode(0), 73);
        const ftp::Payload shutdownPending = shutdownWire.Decode(1);
        shutdownClient.Shutdown();
        const std::size_t sendsAfterShutdown = shutdownWire.sent.size();
        const auto shutdownLate = shutdownWire.Reply(
            shutdownPending, ftp::Opcode::Ack, 73);
        Expect(!shutdownClient.IsBusy() &&
            !shutdownClient.HandleResponse(TestPeer(),
                shutdownLate.data(), shutdownLate.size()) &&
            shutdownWire.sent.size() == sendsAfterShutdown,
            "Shutdown retained pending Blocks or emitted traffic for a late ACK");

        FakeWire metricWire;
        config.writeWindow = 8;
        config.maximumRetries = 1;
        ftp::Client metricClient(metricWire, TestPeer(), config);
        constexpr std::size_t BlockCount = 2050;
        Expect(metricClient.StartWriteFile("@MISSION/mission.dat",
            Bytes(BlockCount * ftp::MaximumDataBytes), error),
            "bounded-metric Window did not start");
        metricWire.Ack(metricClient, metricWire.Decode(0), 74);
        std::size_t cursor = 1;
        while (metricWire.Last().opcode != ftp::Opcode::TerminateSession)
        {
            Expect(cursor < metricWire.sent.size(),
                "bounded-metric Window stopped before Terminate");
            const ftp::Payload request = metricWire.Decode(cursor++);
            if (request.opcode == ftp::Opcode::WriteFile)
                metricWire.Ack(metricClient, request, 74);
        }
        metricWire.Ack(metricClient, metricWire.Last(), 74);
        const auto metricResult = metricClient.ConsumeResult();
        Expect(metricResult && metricResult->success &&
            metricResult->writeBlocksAcked == BlockCount &&
            metricResult->boundedAckRttSamples.size() == 2048 &&
            metricResult->ackRttMinMs <= metricResult->ackRttP50Ms &&
            metricResult->ackRttP50Ms <= metricResult->ackRttP95Ms &&
            metricResult->ackRttP95Ms <= metricResult->ackRttP99Ms &&
            metricResult->ackRttP99Ms <= metricResult->maxAckRttMs &&
            metricResult->writeDataDurationMs <= metricResult->totalDurationMs,
            "RTT/duration aggregation exceeded its bound or ordering contract");
    }
}

int RunMAVFTPMissionWriterTests()
{
    try
    {
        TestCreateWriteTerminateSuccess();
        TestAcknowledgedLogicalProgressBeforeTerminate();
        TestCancelFreezesLatestAcknowledgedLogicalProgress();
        TestProductionCutoverLifecycleAndTickOwnership();
        TestWriteTimeoutRetriesSameBlock();
        TestOffsetMismatchFails();
        TestTerminateAckTimeoutFails();
        TestNextSegmentStartsOnlyAfterTerminateAck();
        TestMultipleSegmentsRemainOrdered();
        TestStreamingRejectionStopsWrites();
        TestBoundedWindowFillOutOfOrderAndMetrics();
        TestWindowCorrelationAndSequenceWrap();
        TestSingleBlockRetryAndWindowNakFailure();
        TestWindowBoundariesExhaustionShutdownAndMetricBound();
        std::cout << "MAVFTP Mission writer core tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "MAVFTP Mission writer core tests FAILED: "
            << error.what() << '\n';
        return 1;
    }
}
