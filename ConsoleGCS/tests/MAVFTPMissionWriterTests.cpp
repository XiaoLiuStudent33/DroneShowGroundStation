#include "MAVFTPMissionTransport.h"

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
        Expect(metrics.segmentsCommitted == 1 &&
            metrics.writeBytes == driven.segmentBytes.front().size() &&
            metrics.writeBlocks > 1,
            "writer success metrics are incomplete");
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
        clientConfig.maximumRetries = 1;
        ftp::Client client(wire, TestPeer(), clientConfig);
        auto fixture = CompileFixture(1, 32, ftp::MaximumDataBytes);
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
        Expect(writer.GetStatus() == MissionTransportStatus::Failed &&
            std::string(writer.GetTerminalPhaseName()) == "TerminateFailed" &&
            !writer.Verify(),
            "missing Terminate ACK did not fail the commit gate");
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
}

int RunMAVFTPMissionWriterTests()
{
    try
    {
        TestCreateWriteTerminateSuccess();
        TestWriteTimeoutRetriesSameBlock();
        TestOffsetMismatchFails();
        TestTerminateAckTimeoutFails();
        TestMultipleSegmentsRemainOrdered();
        TestStreamingRejectionStopsWrites();
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
