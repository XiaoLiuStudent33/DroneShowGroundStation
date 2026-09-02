#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "DroneManager.h"
#include "MavFtpClient.h"
#include "MavFtpProtocol.h"
#include "MavFtpWritePermitPool.h"
#include "MavlinkDatagramSender.h"
#include "MavlinkReadOnlyIngress.h"
#include "MissionProfileProbe.h"
#include "RouteMissionCodec.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include "mdhk/mavlink.h"
}

namespace
{
    namespace ftp = dronegcs::mavftp;
    namespace mission = dronegcs::route_mission;

    void Expect(const bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    ftp::PeerIdentity TestPeer()
    {
        return { "Drone-7", "127.0.0.1", 30007, 7, MAV_COMP_ID_AUTOPILOT1 };
    }

    class FakeTransport final : public ftp::IRequestTransport
    {
    public:
        bool SendMavFtpPayload(
            const std::array<std::uint8_t, ftp::PayloadBytes>& payload) override
        {
            if (!sendSucceeds) return false;
            sent.push_back(payload);
            return true;
        }

        ftp::Payload Last() const
        {
            Expect(!sent.empty(), "fake transport has no MAVFTP request");
            ftp::Payload decoded;
            ftp::Error error;
            Expect(ftp::DecodePayload(sent.back().data(), sent.back().size(),
                decoded, error), "captured MAVFTP request did not decode");
            return decoded;
        }

        std::array<std::uint8_t, ftp::PayloadBytes> ReplyWire(
            const ftp::Opcode opcode,
            std::vector<std::uint8_t> data = {},
            const int sessionOverride = -1,
            const int requestOpcodeOverride = -1,
            const int sequenceDelta = 1,
            const int offsetOverride = -1) const
        {
            const ftp::Payload request = Last();
            ftp::Payload response;
            response.sequence = static_cast<std::uint16_t>(
                request.sequence + sequenceDelta);
            response.session = sessionOverride < 0
                ? request.session : static_cast<std::uint8_t>(sessionOverride);
            response.opcode = opcode;
            response.requestOpcode = requestOpcodeOverride < 0
                ? request.opcode
                : static_cast<ftp::Opcode>(requestOpcodeOverride);
            response.offset = offsetOverride < 0
                ? request.offset : static_cast<std::uint32_t>(offsetOverride);
            response.data = std::move(data);
            std::array<std::uint8_t, ftp::PayloadBytes> wire{};
            ftp::Error error;
            Expect(ftp::EncodePayload(response, wire, error),
                "fake MAVFTP response did not encode");
            return wire;
        }

        bool sendSucceeds = true;
        std::vector<std::array<std::uint8_t, ftp::PayloadBytes>> sent;
    };

    struct FakeClock
    {
        std::chrono::steady_clock::time_point now{
            std::chrono::milliseconds(100000) };

        static std::chrono::steady_clock::time_point Read(void* context)
        {
            return static_cast<FakeClock*>(context)->now;
        }

        void Advance(const std::chrono::milliseconds amount)
        {
            now += amount;
        }
    };

    std::vector<std::uint8_t> U32(const std::uint32_t value)
    {
        std::vector<std::uint8_t> bytes(4);
        ftp::WriteU32LittleEndian(bytes.data(), value);
        return bytes;
    }

    std::array<std::uint8_t, ftp::PayloadBytes> PathRequestWire(
        const ftp::Opcode opcode,
        const std::string& path,
        const bool includeTerminalNull)
    {
        ftp::Payload request;
        request.opcode = opcode;
        request.data.assign(path.begin(), path.end());
        if (includeTerminalNull) request.data.push_back(0);
        std::array<std::uint8_t, ftp::PayloadBytes> wire{};
        ftp::Error error;
        Expect(ftp::EncodePayload(request, wire, error),
            "path request fixture did not encode");
        return wire;
    }

    std::uint8_t ArduPilotLikeCheckNameLen(
        const std::array<std::uint8_t, ftp::PayloadBytes>& wire)
    {
        constexpr std::uint8_t NakInvalidDataSize = 3;
        const std::size_t requestSize = wire[4];
        if (requestSize == 0 || requestSize > ftp::MaximumDataBytes)
            return NakInvalidDataSize;
        const std::uint8_t* data = wire.data() + ftp::HeaderBytes;
        std::size_t fileNameLength = 0;
        while (fileNameLength < ftp::MaximumDataBytes &&
            data[fileNameLength] != 0)
        {
            ++fileNameLength;
        }
        if (fileNameLength == requestSize) return 0;
        if (requestSize > fileNameLength &&
            requestSize - fileNameLength == 1 &&
            data[ftp::MaximumDataBytes - 1] == 0)
        {
            return 0;
        }
        return NakInvalidDataSize;
    }

    void TestArduPilotPathCompatibility()
    {
        constexpr std::uint8_t NakInvalidDataSize = 3;
        const std::string path = "@MISSION/mission.dat";
        Expect(path.size() == 20, "mission VFS path length changed");

        std::vector<std::uint8_t> encodedPath;
        ftp::Error error;
        Expect(ftp::MakePathData(path, encodedPath, error),
            "mission VFS path did not encode");
        Expect(encodedPath == std::vector<std::uint8_t>(path.begin(), path.end()),
            "production path helper did not emit raw path bytes");

        const auto raw = PathRequestWire(ftp::Opcode::OpenFileRO, path, false);
        Expect(raw[4] == path.size(),
            "OpenFileRO header.size is not strlen(path)");
        Expect(std::equal(path.begin(), path.end(),
            raw.begin() + ftp::HeaderBytes),
            "OpenFileRO path bytes changed on the wire");
        Expect(raw[ftp::HeaderBytes + path.size()] == 0,
            "fixed MAVFTP padding does not terminate the raw path safely");
        Expect(ArduPilotLikeCheckNameLen(raw) == 0,
            "ArduPilot-like server rejected raw path form");

        const auto terminated = PathRequestWire(
            ftp::Opcode::OpenFileRO, path, true);
        Expect(terminated[4] == path.size() + 1 &&
            ArduPilotLikeCheckNameLen(terminated) == 0,
            "ArduPilot-like server rejected valid NUL-included form");

        auto tooLarge = raw;
        tooLarge[4] = static_cast<std::uint8_t>(path.size() + 2);
        Expect(ArduPilotLikeCheckNameLen(tooLarge) == NakInvalidDataSize,
            "oversized path length did not produce NAK3");
        auto tooSmall = raw;
        tooSmall[4] = static_cast<std::uint8_t>(path.size() - 1);
        Expect(ArduPilotLikeCheckNameLen(tooSmall) == NakInvalidDataSize,
            "undersized path length did not produce NAK3");
        auto badNullAccounting = raw;
        badNullAccounting[ftp::HeaderBytes + 5] = 0;
        Expect(ArduPilotLikeCheckNameLen(badNullAccounting) == NakInvalidDataSize,
            "bad NUL accounting did not produce NAK3");

        const std::array<ftp::Opcode, 6> pathOpcodes = {
            ftp::Opcode::OpenFileRO, ftp::Opcode::CalcFileCRC32,
            ftp::Opcode::CreateFile, ftp::Opcode::OpenFileWO,
            ftp::Opcode::TruncateFile, ftp::Opcode::ListDirectory };
        for (const ftp::Opcode opcode : pathOpcodes)
        {
            const auto wire = PathRequestWire(opcode, path, false);
            Expect(wire[4] == path.size() &&
                ArduPilotLikeCheckNameLen(wire) == 0,
                std::string(ftp::OpcodeName(opcode)) +
                    " did not use an ArduPilot-compatible path size");
        }
        Expect(std::string(ftp::NakErrorName(3)) == "InvalidDataSize",
            "NAK3 symbolic name is wrong");

        FakeTransport transport;
        ftp::Client client(transport, TestPeer());
        std::string clientError;
        Expect(client.StartReadFile(path, clientError),
            "NAK3 diagnostic OpenFileRO did not start");
        const auto nakWire = transport.ReplyWire(
            ftp::Opcode::Nak, { NakInvalidDataSize });
        Expect(client.HandleResponse(
            TestPeer(), nakWire.data(), nakWire.size()),
            "correlated NAK3 response was rejected");
        const auto rejected = client.ConsumeResult();
        Expect(rejected && !rejected->success &&
            rejected->safeDiagnostic.find("NAK 3 (InvalidDataSize)") !=
                std::string::npos,
            "NAK3 diagnostic omitted InvalidDataSize");
    }

    void Accept(ftp::Client& client, const FakeTransport& transport,
        const ftp::Opcode opcode, std::vector<std::uint8_t> data = {},
        const int session = -1, const int requestOpcode = -1,
        const int sequenceDelta = 1, const int offset = -1)
    {
        const auto wire = transport.ReplyWire(opcode, std::move(data), session,
            requestOpcode, sequenceDelta, offset);
        Expect(client.HandleResponse(TestPeer(), wire.data(), wire.size()),
            "expected fake MAVFTP response was rejected");
    }

    std::size_t CountOpcode(
        const FakeTransport& transport,
        const ftp::Opcode opcode)
    {
        std::size_t count = 0;
        for (const auto& wire : transport.sent)
        {
            ftp::Payload request;
            ftp::Error error;
            Expect(ftp::DecodePayload(wire.data(), wire.size(), request, error),
                "captured timeout fixture request did not decode");
            if (request.opcode == opcode) ++count;
        }
        return count;
    }

    void TestPerOpcodeTimeoutAndTerminateSafety()
    {
        FakeClock clock;
        ftp::ClientConfig config;
        config.requestTimeout = std::chrono::milliseconds(750);
        config.terminateTimeout = std::chrono::milliseconds(2500);
        config.maximumRetries = 1;
        config.clockNow = &FakeClock::Read;
        config.clockContext = &clock;

        FakeTransport ordinaryTransport;
        ftp::Client ordinaryClient(ordinaryTransport, TestPeer(), config);
        std::string error;
        Expect(ordinaryClient.StartResetSessions(error),
            "ordinary timeout fixture did not start");
        const auto resetWire = ordinaryTransport.sent.back();
        clock.Advance(std::chrono::milliseconds(749));
        ordinaryClient.Tick(clock.now);
        Expect(ordinaryTransport.sent.size() == 1,
            "ordinary request retried before 750 ms");
        clock.Advance(std::chrono::milliseconds(1));
        ordinaryClient.Tick(clock.now);
        Expect(ordinaryTransport.sent.size() == 2 &&
            ordinaryTransport.sent.back() == resetWire,
            "ordinary request did not retry at the 750 ms boundary");
        Accept(ordinaryClient, ordinaryTransport, ftp::Opcode::Ack);
        const auto ordinaryResult = ordinaryClient.ConsumeResult();
        Expect(ordinaryResult && ordinaryResult->success &&
            ordinaryResult->retryCount == 1,
            "ordinary request timeout selection did not complete correctly");

        FakeTransport writeTransport;
        ftp::Client writeClient(writeTransport, TestPeer(), config);
        Expect(writeClient.StartWriteFile(
            "@MISSION/mission.dat", { 0x5a }, error),
            "per-opcode timeout Write fixture did not start");
        const auto createWire = writeTransport.sent.back();
        clock.Advance(std::chrono::milliseconds(749));
        writeClient.Tick(clock.now);
        Expect(writeTransport.sent.size() == 1,
            "Create retried before its 750 ms request timeout");
        clock.Advance(std::chrono::milliseconds(1));
        writeClient.Tick(clock.now);
        Expect(writeTransport.sent.size() == 2 &&
            writeTransport.sent.back() == createWire,
            "Create did not retry at the 750 ms boundary");
        Accept(writeClient, writeTransport, ftp::Opcode::Ack, {}, 41);
        const auto writeWire = writeTransport.sent.back();
        clock.Advance(std::chrono::milliseconds(749));
        writeClient.Tick(clock.now);
        Expect(writeTransport.sent.size() == 3,
            "Write block retried before its 750 ms request timeout");
        clock.Advance(std::chrono::milliseconds(1));
        writeClient.Tick(clock.now);
        Expect(writeTransport.sent.size() == 4 &&
            writeTransport.sent.back() == writeWire,
            "Write block did not retry at the 750 ms boundary");
        Accept(writeClient, writeTransport, ftp::Opcode::Ack, U32(1), 41);
        const auto terminateWire = writeTransport.sent.back();
        Expect(writeTransport.Last().opcode == ftp::Opcode::TerminateSession,
            "timeout fixture did not reach TerminateSession");
        const std::size_t writesBeforeTerminateRetry =
            CountOpcode(writeTransport, ftp::Opcode::WriteFile);
        clock.Advance(std::chrono::milliseconds(2499));
        writeClient.Tick(clock.now);
        Expect(writeTransport.sent.back() == terminateWire &&
            writeTransport.sent.size() == 5,
            "Terminate retried before 2500 ms");
        clock.Advance(std::chrono::milliseconds(1));
        writeClient.Tick(clock.now);
        Expect(writeTransport.sent.size() == 6 &&
            writeTransport.sent.back() == terminateWire &&
            CountOpcode(writeTransport, ftp::Opcode::WriteFile) ==
                writesBeforeTerminateRetry,
            "Terminate boundary retry changed identity or resent Write");
        const auto duplicateTerminateAck = writeTransport.ReplyWire(
            ftp::Opcode::Ack, {}, 41);
        Accept(writeClient, writeTransport, ftp::Opcode::Ack, {}, 41);
        Expect(!writeClient.HandleResponse(TestPeer(),
            duplicateTerminateAck.data(), duplicateTerminateAck.size()),
            "duplicate Terminate ACK completed the operation twice");
        const auto writeResult = writeClient.ConsumeResult();
        Expect(writeResult && writeResult->success &&
            writeResult->createRetries == 1 &&
            writeResult->writeRetries == 1 &&
            writeResult->terminateRetries == 1 &&
            writeResult->writeBlocksAcked == 1 &&
            !writeClient.ConsumeResult(),
            "per-opcode retries or result-once behavior are incorrect");

        FakeTransport earlyTransport;
        ftp::Client earlyClient(earlyTransport, TestPeer(), config);
        Expect(earlyClient.StartWriteFile(
            "@MISSION/mission.dat", { 0x6b }, error),
            "early Terminate ACK fixture did not start");
        Accept(earlyClient, earlyTransport, ftp::Opcode::Ack, {}, 42);
        Accept(earlyClient, earlyTransport, ftp::Opcode::Ack, U32(1), 42);
        clock.Advance(std::chrono::milliseconds(2499));
        Accept(earlyClient, earlyTransport, ftp::Opcode::Ack, {}, 42);
        const auto earlyResult = earlyClient.ConsumeResult();
        Expect(earlyResult && earlyResult->success &&
            earlyResult->terminateRetries == 0,
            "Terminate ACK before its first deadline caused a retry");

        FakeTransport exhaustedTransport;
        ftp::Client exhaustedClient(exhaustedTransport, TestPeer(), config);
        Expect(exhaustedClient.StartWriteFile(
            "@MISSION/mission.dat", { 0x7c }, error),
            "Terminate exhaustion fixture did not start");
        Accept(exhaustedClient, exhaustedTransport, ftp::Opcode::Ack, {}, 43);
        Accept(exhaustedClient, exhaustedTransport, ftp::Opcode::Ack, U32(1), 43);
        const auto exhaustedTerminate = exhaustedTransport.Last();
        const auto staleSequence = exhaustedTransport.ReplyWire(
            ftp::Opcode::Ack, {}, 43, -1, 0);
        Expect(!exhaustedClient.HandleResponse(TestPeer(),
            staleSequence.data(), staleSequence.size()) &&
            exhaustedClient.IsBusy(),
            "wrong-identity Terminate ACK advanced the operation");
        const auto wrongSession = exhaustedTransport.ReplyWire(
            ftp::Opcode::Ack, {}, 44);
        Expect(!exhaustedClient.HandleResponse(TestPeer(),
            wrongSession.data(), wrongSession.size()) &&
            exhaustedClient.IsBusy(),
            "wrong-session Terminate ACK advanced the operation");
        const std::size_t writesBeforeExhaustion =
            CountOpcode(exhaustedTransport, ftp::Opcode::WriteFile);
        clock.Advance(std::chrono::milliseconds(2500));
        exhaustedClient.Tick(clock.now);
        Expect(exhaustedTransport.Last().sequence ==
                exhaustedTerminate.sequence &&
            exhaustedTransport.Last().session == exhaustedTerminate.session &&
            exhaustedTransport.Last().opcode ==
                ftp::Opcode::TerminateSession &&
            exhaustedTransport.Last().data == exhaustedTerminate.data &&
            CountOpcode(exhaustedTransport, ftp::Opcode::WriteFile) ==
                writesBeforeExhaustion,
            "Terminate retry did not preserve its immutable request");
        const auto lateAck = exhaustedTransport.ReplyWire(
            ftp::Opcode::Ack, {}, 43);
        const std::size_t sendsBeforeExhaustion = exhaustedTransport.sent.size();
        clock.Advance(std::chrono::milliseconds(2500));
        exhaustedClient.Tick(clock.now);
        const auto exhaustedResult = exhaustedClient.ConsumeResult();
        Expect(exhaustedResult && !exhaustedResult->success &&
            exhaustedResult->terminalRequestOpcode ==
                ftp::Opcode::TerminateSession &&
            exhaustedResult->terminateRetries == 1 &&
            exhaustedResult->writeBlocksAcked == 1 &&
            exhaustedTransport.sent.size() == sendsBeforeExhaustion &&
            !exhaustedClient.IsBusy(),
            "Terminate retry exhaustion resent cleanup or committed the segment");
        Expect(!exhaustedClient.HandleResponse(TestPeer(), lateAck.data(),
            lateAck.size()) && !exhaustedClient.ConsumeResult(),
            "late Terminate ACK recovered an exhausted operation");
    }

    void TestCodec()
    {
        const std::array<ftp::Opcode, 13> opcodes = {
            ftp::Opcode::None, ftp::Opcode::TerminateSession,
            ftp::Opcode::ResetSessions, ftp::Opcode::ListDirectory,
            ftp::Opcode::OpenFileRO, ftp::Opcode::ReadFile,
            ftp::Opcode::CreateFile, ftp::Opcode::WriteFile,
            ftp::Opcode::OpenFileWO, ftp::Opcode::TruncateFile,
            ftp::Opcode::CalcFileCRC32, ftp::Opcode::Ack, ftp::Opcode::Nak };
        for (const ftp::Opcode opcode : opcodes)
        {
            ftp::Payload source;
            source.sequence = 65535;
            source.session = 9;
            source.opcode = opcode;
            source.requestOpcode = ftp::Opcode::ReadFile;
            source.offset = 123;
            source.data = opcode == ftp::Opcode::Nak
                ? std::vector<std::uint8_t>{ 4 }
                : std::vector<std::uint8_t>{ 1, 2, 3 };
            std::array<std::uint8_t, ftp::PayloadBytes> wire{};
            ftp::Error error;
            Expect(ftp::EncodePayload(source, wire, error),
                "supported MAVFTP opcode failed encode");
            ftp::Payload decoded;
            Expect(ftp::DecodePayload(wire.data(), wire.size(), decoded, error),
                "supported MAVFTP opcode failed decode");
            Expect(decoded.sequence == source.sequence &&
                decoded.opcode == source.opcode && decoded.data == source.data,
                "MAVFTP codec round-trip changed fields");
        }

        ftp::Error error;
        ftp::Payload decoded;
        std::array<std::uint8_t, ftp::PayloadBytes> corrupt{};
        corrupt[3] = 99;
        Expect(!ftp::DecodePayload(corrupt.data(), corrupt.size(), decoded, error)
            && error.code == ftp::ErrorCode::UnsupportedOpcode,
            "bad opcode was not rejected");
        Expect(!ftp::DecodePayload(corrupt.data(), corrupt.size() - 1,
            decoded, error), "short MAVFTP payload was not rejected");
        corrupt.fill(0);
        corrupt[3] = static_cast<std::uint8_t>(ftp::Opcode::Ack);
        corrupt[6] = 2;
        Expect(!ftp::DecodePayload(corrupt.data(), corrupt.size(), decoded, error),
            "bad burst flag was not rejected");
        corrupt[6] = 0;
        corrupt[7] = 1;
        Expect(!ftp::DecodePayload(corrupt.data(), corrupt.size(), decoded, error),
            "non-zero padding was not rejected");
        ftp::Payload oversized;
        oversized.data.resize(ftp::MaximumDataBytes + 1);
        Expect(!ftp::EncodePayload(oversized, corrupt, error),
            "oversized MAVFTP data was not rejected");
        ftp::Payload overflow;
        overflow.offset = (std::numeric_limits<std::uint32_t>::max)();
        overflow.data = { 1 };
        Expect(!ftp::EncodePayload(overflow, corrupt, error)
            && error.code == ftp::ErrorCode::OffsetOverflow,
            "offset overflow was not rejected");
        Expect(ftp::NextSequence(65535) == 0, "MAVFTP sequence did not wrap");
        const std::uint8_t crcFixture[] = { '1','2','3','4','5','6','7','8','9' };
        Expect(ftp::CalculateCrc32(crcFixture, sizeof(crcFixture)) == 0xcbf43926U,
            "MAVFTP CRC32 did not match the standard vector");
    }

    void TestClientReadWriteAndCorrelation()
    {
        FakeTransport transport;
        ftp::Client client(transport, TestPeer());
        std::string error;
        Expect(client.StartReadFile("@MISSION/mission.dat", error),
            "OpenFileRO did not start");
        Expect(transport.Last().data == std::vector<std::uint8_t>{
            '@','M','I','S','S','I','O','N','/','m','i','s','s','i','o','n',
            '.','d','a','t' },
            "OpenFileRO did not send the exact raw mission VFS path");

        auto wrongPeer = TestPeer();
        wrongPeer.droneId = "wrong";
        auto wire = transport.ReplyWire(ftp::Opcode::Ack, U32(300), 9);
        Expect(!client.HandleResponse(wrongPeer, wire.data(), wire.size()),
            "wrong Drone response advanced client");
        wire = transport.ReplyWire(ftp::Opcode::Ack, U32(300), 9,
            static_cast<int>(ftp::Opcode::WriteFile));
        Expect(!client.HandleResponse(TestPeer(), wire.data(), wire.size()),
            "wrong req_opcode advanced client");
        wire = transport.ReplyWire(ftp::Opcode::Ack, U32(300), 9, -1, 0);
        Expect(!client.HandleResponse(TestPeer(), wire.data(), wire.size()),
            "stale response advanced client");
        Accept(client, transport, ftp::Opcode::Ack, U32(300), 9);

        Expect(transport.Last().opcode == ftp::Opcode::ReadFile &&
            transport.Last().data.size() == ftp::MaximumDataBytes,
            "first read request was not a bounded 239-byte chunk");
        std::vector<std::uint8_t> first(ftp::MaximumDataBytes, 0x31);
        wire = transport.ReplyWire(ftp::Opcode::Ack, first, 8);
        Expect(!client.HandleResponse(TestPeer(), wire.data(), wire.size()),
            "wrong FTP session advanced read");
        Accept(client, transport, ftp::Opcode::Ack, first, 9);
        Expect(transport.Last().offset == ftp::MaximumDataBytes &&
            transport.Last().data.size() == 300 - ftp::MaximumDataBytes,
            "second read chunk range is wrong");
        std::vector<std::uint8_t> second(300 - ftp::MaximumDataBytes, 0x32);
        Accept(client, transport, ftp::Opcode::Ack, second, 9);
        Expect(transport.Last().opcode == ftp::Opcode::TerminateSession,
            "read did not terminate its FTP session");
        Accept(client, transport, ftp::Opcode::Ack, {}, 9);
        const auto read = client.ConsumeResult();
        Expect(read && read->success && read->bytes.size() == 300 &&
            read->bytes.front() == 0x31 && read->bytes.back() == 0x32,
            "multi-chunk read result is wrong");
        Expect(client.GetStats().wrongPeerResponses == 1 &&
            client.GetStats().wrongRequestOpcodeResponses == 1 &&
            client.GetStats().wrongSessionResponses == 1 &&
            client.GetStats().duplicateOrStaleResponses == 1,
            "response-correlation counters are wrong");

        Expect(client.StartCalculateCrc32("@MISSION/mission.dat", error),
            "CRC request did not start");
        Accept(client, transport, ftp::Opcode::Ack, U32(0x11223344));
        const auto crc = client.ConsumeResult();
        Expect(crc && crc->success && crc->remoteCrc32 == 0x11223344,
            "remote CRC result is wrong");

        std::vector<std::uint8_t> writeBytes(300, 0x5a);
        Expect(client.StartWriteFile("@MISSION/mission.dat", writeBytes, error),
            "CreateFile did not start");
        Accept(client, transport, ftp::Opcode::Nak, { 8 });
        Expect(transport.Last().opcode == ftp::Opcode::TruncateFile,
            "FileExists did not use the truncate fallback");
        Accept(client, transport, ftp::Opcode::Ack);
        Expect(transport.Last().opcode == ftp::Opcode::OpenFileWO,
            "truncate fallback did not reopen write-only");
        Accept(client, transport, ftp::Opcode::Ack, {}, 12);
        Expect(transport.Last().opcode == ftp::Opcode::WriteFile &&
            transport.Last().data.size() == ftp::MaximumDataBytes,
            "first write chunk is wrong");
        Accept(client, transport, ftp::Opcode::Ack,
            U32(static_cast<std::uint32_t>(ftp::MaximumDataBytes)), 12);
        Expect(transport.Last().offset == ftp::MaximumDataBytes &&
            transport.Last().data.size() == 300 - ftp::MaximumDataBytes,
            "second write chunk is wrong");
        Accept(client, transport, ftp::Opcode::Ack,
            U32(static_cast<std::uint32_t>(300 - ftp::MaximumDataBytes)), 12);
        Expect(transport.Last().opcode == ftp::Opcode::TerminateSession,
            "write did not wait for close/terminate ACK");
        Accept(client, transport, ftp::Opcode::Ack, {}, 12);
        const auto write = client.ConsumeResult();
        Expect(write && write->success, "multi-chunk write did not complete");

        Expect(client.StartResetSessions(error), "reset did not start");
        Accept(client, transport, ftp::Opcode::Nak, { 4 });
        const auto nak = client.ConsumeResult();
        Expect(nak && !nak->success && nak->nakError == 4,
            "NAK did not fail the active operation");
        Expect(nak->safeDiagnostic.find("NAK 4 (InvalidSession)") !=
            std::string::npos, "NAK diagnostic omitted its symbolic name");
    }

    void TestClientTimeoutCancelShutdown()
    {
        FakeTransport transport;
        ftp::ClientConfig config;
        config.requestTimeout = std::chrono::milliseconds(1);
        config.maximumRetries = 1;
        ftp::Client client(transport, TestPeer(), config);
        std::string error;
        Expect(client.StartResetSessions(error), "timeout reset did not start");
        const auto original = transport.sent.back();
        client.Tick(std::chrono::steady_clock::now() + std::chrono::seconds(1));
        Expect(transport.sent.size() == 2 && transport.sent.back() == original,
            "retry did not reuse the exact request/sequence");
        client.Tick(std::chrono::steady_clock::now() + std::chrono::seconds(2));
        const auto exhausted = client.ConsumeResult();
        Expect(exhausted && !exhausted->success && !client.IsBusy(),
            "bounded retry exhaustion did not complete");

        Expect(client.StartReadFile("@MISSION/mission.dat", error),
            "cancel read did not start");
        Accept(client, transport, ftp::Opcode::Ack, U32(1), 3);
        client.Cancel();
        const auto cancelled = client.ConsumeResult();
        Expect(cancelled && cancelled->cancelled && !client.IsBusy(),
            "cancel did not end the active transaction");
        Expect(client.StartResetSessions(error), "shutdown reset did not start");
        client.Shutdown();
        Expect(!client.IsBusy(), "shutdown left an operation active");
    }

    class CapturingSender final : public IMavlinkDatagramSender
    {
    public:
        struct Datagram
        {
            std::uint16_t port = 0;
            std::vector<std::uint8_t> bytes;
        };

        bool SendTo(const std::string& ip, const std::uint16_t port,
            const std::uint8_t* data, const int length) override
        {
            if (ip != "127.0.0.1" ||
                (port != 30007 && port != 30008) || !data || length <= 0)
                return false;
            datagrams.push_back({ port,
                std::vector<std::uint8_t>(data, data + length) });
            return true;
        }

        std::pair<std::uint16_t, mavlink_message_t> PopWithEndpoint()
        {
            Expect(cursor < datagrams.size(), "DroneSession emitted no request");
            mavlink_message_t message{};
            mavlink_status_t status{};
            const Datagram& datagram = datagrams[cursor++];
            for (const std::uint8_t byte : datagram.bytes)
                (void)mavlink_parse_char(MAVLINK_COMM_3, byte, &message, &status);
            return { datagram.port, message };
        }

        mavlink_message_t Pop()
        {
            return PopWithEndpoint().second;
        }

        std::size_t cursor = 0;
        std::vector<Datagram> datagrams;
    };

    DiscoveredDroneIdentity Identity()
    {
        DiscoveredDroneIdentity identity;
        identity.systemId = 7;
        identity.componentId = MAV_COMP_ID_AUTOPILOT1;
        identity.sourceIp = "127.0.0.1";
        identity.sourcePort = 30007;
        identity.observedAt = std::chrono::steady_clock::now();
        return identity;
    }

    std::vector<std::uint8_t> MissionBytes(const std::uint16_t count)
    {
        std::array<std::uint8_t, mission::LegacyMissionFtpContainerBytesV1> header{};
        mission::Error error;
        Expect(mission::EncodeLegacyMissionFtpContainerV1(
            0, count, mission::FirstContainerOptionsV1, header, error),
            "mission fixture header failed");
        std::vector<std::uint8_t> bytes(header.begin(), header.end());
        for (std::uint16_t index = 0; index < count; ++index)
        {
            dronegcs::wpl110::Row row;
            row.Sequence = index;
            std::array<std::uint8_t, mission::MissionItemRecordBytesV1> record{};
            Expect(mission::EncodeMissionItemRecordV1(row, record, error),
                "mission fixture record failed");
            bytes.insert(bytes.end(), record.begin(), record.end());
        }
        return bytes;
    }

    class ProbeHarness
    {
    public:
        ProbeHarness()
            : session([this]() {
                DroneSessionConfig value;
                value.droneId = "Drone-7";
                value.remoteIp = "127.0.0.1";
                value.remotePort = 30007;
                value.receiveOnly = true;
                value.mavFtpServiceEnabled = true;
                value.sharedMavlinkEndpoint = &sender;
                value.discoveredSystemId = 7;
                value.discoveredComponentId = MAV_COMP_ID_AUTOPILOT1;
                return value;
            }())
        {
            Expect(session.Start(), "probe DroneSession did not start");
        }

        void Online(const bool armed = false)
        {
            Expect(session.RefreshDiscoveredPresence(Identity()),
                "probe session did not become online");
            mavlink_message_t heartbeat{};
            mavlink_msg_heartbeat_pack(7, MAV_COMP_ID_AUTOPILOT1, &heartbeat,
                MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
                armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0, 0, MAV_STATE_STANDBY);
            Expect(session.ApplyDiscoveredHeartbeat(heartbeat, Identity()),
                "probe heartbeat was not applied");
        }

        ftp::Payload PopFtpRequest()
        {
            const mavlink_message_t message = sender.Pop();
            Expect(message.msgid == MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL,
                "expected FILE_TRANSFER_PROTOCOL request");
            mavlink_file_transfer_protocol_t envelope{};
            mavlink_msg_file_transfer_protocol_decode(&message, &envelope);
            ftp::Payload request;
            ftp::Error error;
            Expect(ftp::DecodePayload(envelope.payload,
                sizeof(envelope.payload), request, error),
                "DroneSession FTP request did not decode");
            opcodes.push_back(request.opcode);
            return request;
        }

        void Reply(const ftp::Payload& request, const ftp::Opcode opcode,
            std::vector<std::uint8_t> data = {}, const std::uint8_t sessionId = 6)
        {
            ftp::Payload response;
            response.sequence = ftp::NextSequence(request.sequence);
            response.session = request.opcode == ftp::Opcode::OpenFileRO
                ? sessionId : request.session;
            response.opcode = opcode;
            response.requestOpcode = request.opcode;
            response.offset = request.offset;
            response.data = std::move(data);
            std::array<std::uint8_t, ftp::PayloadBytes> wire{};
            ftp::Error error;
            Expect(ftp::EncodePayload(response, wire, error),
                "probe response encoding failed");
            mavlink_message_t message{};
            mavlink_msg_file_transfer_protocol_pack(7, MAV_COMP_ID_AUTOPILOT1,
                &message, 0, 255, MAV_COMP_ID_MISSIONPLANNER, wire.data());
            Expect(session.ApplyDiscoveredFtpMessage(message, Identity()),
                "probe response did not route to session client");
        }

        MissionProfileProbeReport RunReadOnly(
            std::vector<std::uint8_t> remote,
            const bool missing = false,
            const bool crcUnsupported = false,
            const bool crcMismatch = false)
        {
            MissionProfileProbeOptions options;
            options.enabled = true;
            options.droneId = "Drone-7";
            MissionProfileProbe probe(session, options);
            std::string error;
            Expect(probe.Start(error), "read-only probe did not start: " + error);
            for (int step = 0; step < 100 && !probe.IsComplete(); ++step)
            {
                const ftp::Payload request = PopFtpRequest();
                switch (request.opcode)
                {
                case ftp::Opcode::ResetSessions:
                    Reply(request, ftp::Opcode::Ack);
                    break;
                case ftp::Opcode::OpenFileRO:
                    if (missing) Reply(request, ftp::Opcode::Nak, { 10 });
                    else Reply(request, ftp::Opcode::Ack,
                        U32(static_cast<std::uint32_t>(remote.size())));
                    break;
                case ftp::Opcode::ReadFile:
                {
                    const std::size_t count = (std::min)(request.data.size(),
                        remote.size() - request.offset);
                    Reply(request, ftp::Opcode::Ack,
                        std::vector<std::uint8_t>(remote.begin() + request.offset,
                            remote.begin() + request.offset + count));
                    break;
                }
                case ftp::Opcode::TerminateSession:
                    Reply(request, ftp::Opcode::Ack);
                    break;
                case ftp::Opcode::CalcFileCRC32:
                    if (crcUnsupported) Reply(request, ftp::Opcode::Nak, { 7 });
                    else Reply(request, ftp::Opcode::Ack, U32(
                        ftp::CalculateCrc32(remote.data(), remote.size()) +
                        (crcMismatch ? 1U : 0U)));
                    break;
                default:
                    throw std::runtime_error("read-only probe attempted a write opcode");
                }
                probe.Tick(std::chrono::steady_clock::now());
            }
            Expect(probe.IsComplete(), "read-only probe did not finish within bound");
            return probe.GetReport();
        }

        MissionProfileProbeReport RunWrite(
            std::vector<std::uint8_t> remote,
            const bool rejectTinyClose = false,
            const bool mismatchTinyCount = false,
            const bool rejectRestoreClose = false)
        {
            MissionProfileProbeOptions options;
            options.enabled = true;
            options.allowWriteProbe = true;
            options.droneId = "Drone-7";
            MissionProfileProbe probe(session, options);
            std::string error;
            Expect(probe.Start(error), "write probe did not start: " + error);
            std::uint8_t activeSession = 6;
            int writeOrdinal = 0;
            bool writeTransaction = false;
            int missionCountOrdinal = 0;
            bool discardBestEffortTerminate = false;
            for (int step = 0; step < 300 && !probe.IsComplete(); ++step)
            {
                const mavlink_message_t message = sender.Pop();
                if (message.msgid == MAVLINK_MSG_ID_MISSION_REQUEST_LIST)
                {
                    std::uint16_t count = remote.size() >= 10
                        ? static_cast<std::uint16_t>(remote[8] | (remote[9] << 8))
                        : 0;
                    ++missionCountOrdinal;
                    if (missionCountOrdinal == 2 && mismatchTinyCount) ++count;
                    mavlink_message_t response{};
                    mavlink_msg_mission_count_pack(7, MAV_COMP_ID_AUTOPILOT1,
                        &response, 255, MAV_COMP_ID_MISSIONPLANNER, count,
                        MAV_MISSION_TYPE_MISSION, 0);
                    Expect(session.ApplyDiscoveredMissionMessage(
                        response, Identity()), "MISSION_COUNT did not route");
                    probe.Tick(std::chrono::steady_clock::now());
                    continue;
                }
                Expect(message.msgid == MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL,
                    "write probe emitted an unexpected MAVLink message");
                mavlink_file_transfer_protocol_t envelope{};
                mavlink_msg_file_transfer_protocol_decode(&message, &envelope);
                ftp::Payload request;
                ftp::Error protocolError;
                Expect(ftp::DecodePayload(envelope.payload,
                    sizeof(envelope.payload), request, protocolError),
                    "write-probe request did not decode");
                opcodes.push_back(request.opcode);
                switch (request.opcode)
                {
                case ftp::Opcode::ResetSessions:
                    Reply(request, ftp::Opcode::Ack);
                    break;
                case ftp::Opcode::OpenFileRO:
                    activeSession = 6;
                    writeTransaction = false;
                    Reply(request, ftp::Opcode::Ack,
                        U32(static_cast<std::uint32_t>(remote.size())), activeSession);
                    break;
                case ftp::Opcode::ReadFile:
                {
                    const std::size_t count = (std::min)(request.data.size(),
                        remote.size() - request.offset);
                    Reply(request, ftp::Opcode::Ack,
                        std::vector<std::uint8_t>(remote.begin() + request.offset,
                            remote.begin() + request.offset + count), activeSession);
                    break;
                }
                case ftp::Opcode::CalcFileCRC32:
                    Reply(request, ftp::Opcode::Ack,
                        U32(ftp::CalculateCrc32(remote.data(), remote.size())));
                    break;
                case ftp::Opcode::CreateFile:
                    ++writeOrdinal;
                    writeTransaction = true;
                    if (remote.empty())
                    {
                        activeSession = 8;
                        Reply(request, ftp::Opcode::Ack, {}, activeSession);
                    }
                    else Reply(request, ftp::Opcode::Nak, { 8 });
                    break;
                case ftp::Opcode::TruncateFile:
                    remote.clear();
                    Reply(request, ftp::Opcode::Ack);
                    break;
                case ftp::Opcode::OpenFileWO:
                    activeSession = 8;
                    Reply(request, ftp::Opcode::Ack, {}, activeSession);
                    break;
                case ftp::Opcode::WriteFile:
                    if (remote.size() < request.offset + request.data.size())
                        remote.resize(request.offset + request.data.size());
                    std::copy(request.data.begin(), request.data.end(),
                        remote.begin() + request.offset);
                    Reply(request, ftp::Opcode::Ack,
                        U32(static_cast<std::uint32_t>(request.data.size())),
                        activeSession);
                    break;
                case ftp::Opcode::TerminateSession:
                    if (discardBestEffortTerminate)
                    {
                        discardBestEffortTerminate = false;
                        continue;
                    }
                    if (writeTransaction &&
                        ((writeOrdinal == 1 && rejectTinyClose) ||
                         (writeOrdinal == 2 && rejectRestoreClose)))
                    {
                        Reply(request, ftp::Opcode::Nak, { 1 }, activeSession);
                        discardBestEffortTerminate = true;
                    }
                    else Reply(request, ftp::Opcode::Ack, {}, activeSession);
                    writeTransaction = false;
                    break;
                default:
                    throw std::runtime_error("write probe emitted unsupported opcode");
                }
                probe.Tick(std::chrono::steady_clock::now());
            }
            Expect(probe.IsComplete(), "write probe did not finish within bound");
            return probe.GetReport();
        }

        CapturingSender sender;
        DroneSession session;
        std::vector<ftp::Opcode> opcodes;
    };

    void TestProbeReadOnlyAndGuards()
    {
        DroneManager emptyManager;
        MissionProfileProbeOptions missingOptions;
        missingOptions.enabled = true;
        missingOptions.droneId = "missing";
        std::string error;
        Expect(!CreateMissionProfileProbe(emptyManager, missingOptions, error),
            "missing explicit DroneID was not rejected");

        {
            ProbeHarness offline;
            MissionProfileProbeOptions options{ true, false, "Drone-7" };
            MissionProfileProbe probe(offline.session, options);
            Expect(!probe.Start(error), "offline probe was not rejected");
        }
        {
            ProbeHarness armed;
            armed.Online(true);
            MissionProfileProbeOptions options{ true, false, "Drone-7" };
            MissionProfileProbe probe(armed.session, options);
            Expect(!probe.Start(error), "armed probe was not rejected");
        }
        {
            ProbeHarness exact;
            exact.Online();
            const auto report = exact.RunReadOnly(MissionBytes(2));
            Expect(report.succeeded && report.missionPathOpen &&
                report.currentCodecComparison == MissionProfileComparison::ExactMatch,
                "exact current-codec mission was not recognized");
            Expect(report.missionPath == "@MISSION/mission.dat" &&
                report.missionPathBytes == 20 &&
                report.missionPathEncodedSize == 20 &&
                !report.missionPathNullTerminated &&
                report.OpenFileRequestSafeSummary().find(
                    "PathBytes=20 EncodedSize=20 NullTerminated=no") !=
                        std::string::npos,
                "read-only probe path wire summary is wrong");
            for (const ftp::Opcode opcode : exact.opcodes)
                Expect(opcode != ftp::Opcode::CreateFile &&
                    opcode != ftp::Opcode::WriteFile &&
                    opcode != ftp::Opcode::TruncateFile &&
                    opcode != ftp::Opcode::OpenFileWO,
                    "default read-only probe emitted a write opcode");
        }
        {
            ProbeHarness headerOnly;
            headerOnly.Online();
            auto bytes = MissionBytes(1);
            bytes.resize(mission::LegacyMissionFtpContainerBytesV1);
            const auto report = headerOnly.RunReadOnly(bytes, false, true);
            Expect(report.succeeded && !report.remoteCrcSupported &&
                report.headerSizeObserved == mission::LegacyMissionFtpContainerBytesV1,
                "header-only/CRC-unsupported probe result is wrong");
        }
        {
            ProbeHarness mismatch;
            mismatch.Online();
            auto bytes = MissionBytes(1);
            bytes.push_back(0xff);
            const auto report = mismatch.RunReadOnly(bytes);
            Expect(report.succeeded && report.currentCodecComparison ==
                MissionProfileComparison::Incompatible,
                "profile size mismatch was not reported incompatible");
        }
        {
            ProbeHarness missing;
            missing.Online();
            const auto report = missing.RunReadOnly({}, true);
            Expect(!report.succeeded && !report.missionPathOpen,
                "missing mission path did not fail closed");
        }
        {
            ProbeHarness crcMismatch;
            crcMismatch.Online();
            const auto report = crcMismatch.RunReadOnly(MissionBytes(1),
                false, false, true);
            Expect(!report.succeeded && report.safeDiagnostic.find("mismatch") !=
                std::string::npos, "remote/local CRC mismatch was not rejected");
        }
        {
            ProbeHarness backupRequired;
            backupRequired.Online();
            const auto report = backupRequired.RunWrite({});
            Expect(!report.succeeded && !report.writeAccepted,
                "empty/incomplete backup did not block write");
            for (const ftp::Opcode opcode : backupRequired.opcodes)
                Expect(opcode != ftp::Opcode::CreateFile,
                    "backup safety failure still started CreateFile");
        }
        {
            ProbeHarness writeSuccess;
            writeSuccess.Online();
            const auto report = writeSuccess.RunWrite(MissionBytes(2));
            Expect(report.succeeded && report.writeProbeAttempted &&
                report.writeAccepted && report.missionCountBeforeWrite == 2 &&
                report.missionCountAfterWrite == 1 &&
                report.restoreAttempted && report.restoreVerified,
                "tiny mission success/restore path failed");
        }
        {
            ProbeHarness closeRejected;
            closeRejected.Online();
            const auto report = closeRejected.RunWrite(
                MissionBytes(2), true, false, false);
            Expect(report.succeeded && !report.writeAccepted &&
                report.restoreVerified,
                "tiny close rejection did not restore the backup");
        }
        {
            ProbeHarness countMismatch;
            countMismatch.Online();
            const auto report = countMismatch.RunWrite(
                MissionBytes(2), false, true, false);
            Expect(report.succeeded && report.missionCountAfterWrite == 2 &&
                report.restoreVerified &&
                report.safeDiagnostic.find("mismatch") != std::string::npos,
                "MISSION_COUNT mismatch was not reported before restore");
        }
        {
            ProbeHarness restoreFailed;
            restoreFailed.Online();
            const auto report = restoreFailed.RunWrite(
                MissionBytes(2), false, false, true);
            Expect(!report.succeeded && report.restoreAttempted &&
                !report.restoreVerified &&
                report.safeDiagnostic.find("HIGH-VISIBILITY") != std::string::npos,
                "restore failure did not fail closed visibly");
        }
    }

    std::vector<std::uint8_t> SerializeMavlink(const mavlink_message_t& message)
    {
        std::vector<std::uint8_t> bytes(MAVLINK_MAX_PACKET_LEN);
        bytes.resize(mavlink_msg_to_send_buffer(bytes.data(), &message));
        return bytes;
    }

    void TestIngressDispatcher()
    {
        CapturingSender sender;
        DroneManager manager;
        DroneSessionConfig defaults;
        defaults.receiveOnly = true;
        defaults.mavFtpServiceEnabled = true;
        defaults.sharedMavlinkEndpoint = &sender;
        MavlinkDiscoveryService discovery(manager, defaults);
        MavlinkReadOnlyIngress ingress(manager, discovery);

        mavlink_message_t heartbeat{};
        mavlink_msg_heartbeat_pack(7, MAV_COMP_ID_AUTOPILOT1, &heartbeat,
            MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0,
            MAV_STATE_STANDBY);
        const auto heartbeatBytes = SerializeMavlink(heartbeat);
        const auto heartbeatStats = ingress.SubmitDatagram(
            heartbeatBytes.data(), heartbeatBytes.size(), "127.0.0.1", 30007);
        Expect(heartbeatStats.heartbeatsApplied == 1 && manager.GetDroneCount() == 1,
            "ingress heartbeat did not create the endpoint-owned session");
        DroneSession* session = manager.FindDrone(manager.GetDroneIds().front());
        Expect(session && session->GetMavFtpClient(),
            "discovered session has no reusable MAVFTP client");

        std::string error;
        Expect(session->GetMavFtpClient()->StartResetSessions(error),
            "ingress integration reset did not start");
        const mavlink_message_t sent = sender.Pop();
        mavlink_file_transfer_protocol_t sentEnvelope{};
        mavlink_msg_file_transfer_protocol_decode(&sent, &sentEnvelope);
        ftp::Payload request;
        ftp::Error protocolError;
        Expect(ftp::DecodePayload(sentEnvelope.payload, sizeof(sentEnvelope.payload),
            request, protocolError), "ingress integration request did not decode");
        ftp::Payload response;
        response.sequence = ftp::NextSequence(request.sequence);
        response.session = request.session;
        response.opcode = ftp::Opcode::Ack;
        response.requestOpcode = request.opcode;
        std::array<std::uint8_t, ftp::PayloadBytes> responseWire{};
        Expect(ftp::EncodePayload(response, responseWire, protocolError),
            "ingress integration response did not encode");
        mavlink_message_t responseMessage{};
        mavlink_msg_file_transfer_protocol_pack(7, MAV_COMP_ID_AUTOPILOT1,
            &responseMessage, 0, 255, MAV_COMP_ID_MISSIONPLANNER,
            responseWire.data());
        const auto responseBytes = SerializeMavlink(responseMessage);
        const auto wrongStats = ingress.SubmitDatagram(responseBytes.data(),
            responseBytes.size(), "127.0.0.2", 30007);
        Expect(wrongStats.ftpMessagesApplied == 0 &&
            session->GetMavFtpClient()->IsBusy(),
            "wrong endpoint advanced the ingress transaction");
        const auto ftpStats = ingress.SubmitDatagram(responseBytes.data(),
            responseBytes.size(), "127.0.0.1", 30007);
        const auto result = session->GetMavFtpClient()->ConsumeResult();
        Expect(ftpStats.ftpMessagesApplied == 1 && result && result->success,
            "FILE_TRANSFER_PROTOCOL ingress did not reach the client");

        Expect(session->SendMissionRequestList(),
            "MISSION_REQUEST_LIST bridge send failed");
        (void)sender.Pop();
        mavlink_message_t count{};
        mavlink_msg_mission_count_pack(7, MAV_COMP_ID_AUTOPILOT1, &count,
            255, MAV_COMP_ID_MISSIONPLANNER, 3, MAV_MISSION_TYPE_MISSION, 0);
        const auto countBytes = SerializeMavlink(count);
        const auto countStats = ingress.SubmitDatagram(countBytes.data(),
            countBytes.size(), "127.0.0.1", 30007);
        Expect(countStats.missionMessagesApplied == 1 &&
            session->ConsumeMissionCount() == 3,
            "MISSION_COUNT ingress did not preserve same-session identity");
    }

    void TestIngressTwoEndpointAckIsolation()
    {
        CapturingSender sender;
        DroneManager manager;
        DroneSessionConfig defaults;
        defaults.receiveOnly = true;
        defaults.mavFtpServiceEnabled = true;
        defaults.sharedMavlinkEndpoint = &sender;
        MavlinkDiscoveryService discovery(manager, defaults);
        MavlinkReadOnlyIngress ingress(manager, discovery);

        mavlink_message_t heartbeat{};
        mavlink_msg_heartbeat_pack(7, MAV_COMP_ID_AUTOPILOT1, &heartbeat,
            MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0,
            MAV_STATE_STANDBY);
        const auto heartbeatBytes = SerializeMavlink(heartbeat);
        Expect(ingress.SubmitDatagram(heartbeatBytes.data(),
                heartbeatBytes.size(), "127.0.0.1", 30007).heartbeatsApplied == 1 &&
            ingress.SubmitDatagram(heartbeatBytes.data(),
                heartbeatBytes.size(), "127.0.0.1", 30008).heartbeatsApplied == 1 &&
            manager.GetDroneCount() == 2,
            "same-SYSID A/B endpoints did not create distinct Sessions");
        const auto ids = manager.GetDroneIds();
        DroneSession* sessionA = manager.FindDrone(ids[0]);
        DroneSession* sessionB = manager.FindDrone(ids[1]);
        Expect(sessionA && sessionB && sessionA->GetMavFtpClient() &&
            sessionB->GetMavFtpClient(),
            "A/B endpoint Sessions omitted their unique Clients");
        std::string error;
        Expect(sessionA->GetMavFtpClient()->StartResetSessions(error) &&
            sessionB->GetMavFtpClient()->StartResetSessions(error),
            "A/B identical request fixture did not start");

        const auto decodeRequest = [&sender]()
        {
            const mavlink_message_t message = sender.Pop();
            mavlink_file_transfer_protocol_t envelope{};
            mavlink_msg_file_transfer_protocol_decode(&message, &envelope);
            ftp::Payload request;
            ftp::Error decodeError;
            Expect(ftp::DecodePayload(envelope.payload,
                sizeof(envelope.payload), request, decodeError),
                "A/B ResetSessions request did not decode");
            return request;
        };
        const ftp::Payload requestA = decodeRequest();
        const ftp::Payload requestB = decodeRequest();
        Expect(requestA.sequence == requestB.sequence &&
            requestA.session == requestB.session &&
            requestA.offset == requestB.offset &&
            requestA.opcode == requestB.opcode,
            "A/B isolation fixture did not share identical ACK identity fields");

        ftp::Payload response;
        response.sequence = ftp::NextSequence(requestA.sequence);
        response.session = requestA.session;
        response.opcode = ftp::Opcode::Ack;
        response.requestOpcode = requestA.opcode;
        response.offset = requestA.offset;
        std::array<std::uint8_t, ftp::PayloadBytes> responseWire{};
        ftp::Error protocolError;
        Expect(ftp::EncodePayload(response, responseWire, protocolError),
            "A/B identical ACK did not encode");
        mavlink_message_t responseMessage{};
        mavlink_msg_file_transfer_protocol_pack(7, MAV_COMP_ID_AUTOPILOT1,
            &responseMessage, 0, 255, MAV_COMP_ID_MISSIONPLANNER,
            responseWire.data());
        const auto responseBytes = SerializeMavlink(responseMessage);

        const auto statsA = ingress.SubmitDatagram(responseBytes.data(),
            responseBytes.size(), "127.0.0.1", 30007);
        const auto resultA = sessionA->GetMavFtpClient()->ConsumeResult();
        Expect(statsA.ftpMessagesApplied == 1 && resultA && resultA->success &&
            sessionB->GetMavFtpClient()->IsBusy() &&
            !sessionB->GetMavFtpClient()->ConsumeResult(),
            "endpoint A ACK advanced endpoint B with identical identity fields");
        const auto statsB = ingress.SubmitDatagram(responseBytes.data(),
            responseBytes.size(), "127.0.0.1", 30008);
        const auto resultB = sessionB->GetMavFtpClient()->ConsumeResult();
        Expect(statsB.ftpMessagesApplied == 1 && resultB && resultB->success,
            "endpoint B did not independently consume its identical ACK");
    }

    void TestIngressTwoEndpointWritePermitIsolation()
    {
        CapturingSender sender;
        ftp::WritePermitPool permitPool(2);
        DroneManager manager;
        DroneSessionConfig defaults;
        defaults.receiveOnly = true;
        defaults.mavFtpServiceEnabled = true;
        defaults.sharedMavlinkEndpoint = &sender;
        defaults.mavFtpWritePermitPool = &permitPool;
        defaults.mavFtpWriteWindow = 1;
        defaults.mavFtpRequestRetryCount = 1;
        defaults.mavFtpRequestTimeoutMs = 750;
        MavlinkDiscoveryService discovery(manager, defaults);
        MavlinkReadOnlyIngress ingress(manager, discovery);

        mavlink_message_t heartbeat{};
        mavlink_msg_heartbeat_pack(7, MAV_COMP_ID_AUTOPILOT1, &heartbeat,
            MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0,
            MAV_STATE_STANDBY);
        const auto heartbeatBytes = SerializeMavlink(heartbeat);
        Expect(ingress.SubmitDatagram(heartbeatBytes.data(),
                heartbeatBytes.size(), "127.0.0.1", 30007).heartbeatsApplied == 1 &&
            ingress.SubmitDatagram(heartbeatBytes.data(),
                heartbeatBytes.size(), "127.0.0.1", 30008).heartbeatsApplied == 1,
            "Write isolation endpoints did not create two Sessions");

        DroneSession* sessionA = nullptr;
        DroneSession* sessionB = nullptr;
        for (const std::string& id : manager.GetDroneIds())
        {
            DroneSession* session = manager.FindDrone(id);
            const ftp::Client* client = session ? session->GetMavFtpClient() : nullptr;
            if (client && client->GetPeer().remotePort == 30007) sessionA = session;
            else if (client && client->GetPeer().remotePort == 30008) sessionB = session;
        }
        Expect(sessionA && sessionB && sessionA->GetMavFtpClient() &&
            sessionB->GetMavFtpClient(),
            "Write isolation could not resolve endpoint-owned Clients");
        ftp::Client* const clientA = sessionA->GetMavFtpClient();
        ftp::Client* const clientB = sessionB->GetMavFtpClient();
        std::string error;
        const std::vector<std::uint8_t> bytesA(1, 0xA1);
        const std::vector<std::uint8_t> bytesB(300, 0xB2);
        Expect(clientA->StartWriteFile("@MISSION/mission.dat", bytesA, error) &&
            clientB->StartWriteFile("@MISSION/mission.dat", bytesB, error),
            "A/B endpoint Write operations did not start");

        const auto DecodeRequest = [](const mavlink_message_t& message)
        {
            mavlink_file_transfer_protocol_t envelope{};
            mavlink_msg_file_transfer_protocol_decode(&message, &envelope);
            ftp::Payload request;
            ftp::Error decodeError;
            Expect(ftp::DecodePayload(envelope.payload,
                sizeof(envelope.payload), request, decodeError),
                "A/B endpoint request did not decode");
            return request;
        };
        const auto SubmitResponse = [&ingress](
            const std::uint16_t port, const ftp::Payload& request,
            const ftp::Opcode responseOpcode, const std::uint8_t session,
            std::vector<std::uint8_t> data = {})
        {
            ftp::Payload response;
            response.sequence = ftp::NextSequence(request.sequence);
            response.session = session;
            response.opcode = responseOpcode;
            response.requestOpcode = request.opcode;
            response.offset = request.offset;
            response.data = std::move(data);
            std::array<std::uint8_t, ftp::PayloadBytes> wire{};
            ftp::Error encodeError;
            Expect(ftp::EncodePayload(response, wire, encodeError),
                "A/B endpoint response did not encode");
            mavlink_message_t message{};
            mavlink_msg_file_transfer_protocol_pack(7,
                MAV_COMP_ID_AUTOPILOT1, &message, 0, 255,
                MAV_COMP_ID_MISSIONPLANNER, wire.data());
            const auto datagram = SerializeMavlink(message);
            return ingress.SubmitDatagram(datagram.data(), datagram.size(),
                "127.0.0.1", port);
        };

        const auto createDatagramA = sender.PopWithEndpoint();
        const auto createDatagramB = sender.PopWithEndpoint();
        Expect(createDatagramA.first == 30007 && createDatagramB.first == 30008,
            "A/B CreateFile requests crossed endpoints");
        const ftp::Payload createA = DecodeRequest(createDatagramA.second);
        const ftp::Payload createB = DecodeRequest(createDatagramB.second);
        Expect(createA.opcode == ftp::Opcode::CreateFile &&
            createB.opcode == ftp::Opcode::CreateFile &&
            createA.sequence == createB.sequence,
            "A/B CreateFile identity fixture diverged");
        Expect(SubmitResponse(30007, createA, ftp::Opcode::Ack, 41)
                .ftpMessagesApplied == 1 &&
            SubmitResponse(30008, createB, ftp::Opcode::Ack, 41)
                .ftpMessagesApplied == 1,
            "A/B CreateFile ACKs were not routed by endpoint");

        const auto writeDatagramA = sender.PopWithEndpoint();
        const auto writeDatagramB = sender.PopWithEndpoint();
        const ftp::Payload writeA = DecodeRequest(writeDatagramA.second);
        const ftp::Payload writeB = DecodeRequest(writeDatagramB.second);
        Expect(writeDatagramA.first == 30007 && writeDatagramB.first == 30008 &&
            writeA.opcode == ftp::Opcode::WriteFile &&
            writeB.opcode == ftp::Opcode::WriteFile &&
            writeA.sequence == writeB.sequence &&
            writeA.session == writeB.session &&
            writeA.offset == writeB.offset &&
            permitPool.GetMetrics().outstanding == 2,
            "A/B identical Write identity did not hold two isolated permits");

        const auto unknownEndpoint = SubmitResponse(
            30009, writeA, ftp::Opcode::Ack, 41);
        Expect(unknownEndpoint.ftpMessagesApplied == 0 &&
            permitPool.GetMetrics().outstanding == 2,
            "unknown endpoint ACK released a Write permit");

        clientA->Tick(std::chrono::steady_clock::now() +
            std::chrono::milliseconds(751));
        const auto retryDatagramA = sender.PopWithEndpoint();
        const ftp::Payload retryA = DecodeRequest(retryDatagramA.second);
        Expect(retryDatagramA.first == 30007 && retryA.sequence == writeA.sequence &&
            retryA.session == writeA.session && retryA.offset == writeA.offset &&
            retryA.data == writeA.data &&
            permitPool.GetMetrics().outstanding == 2 &&
            permitPool.GetMetrics().permitsGranted == 2,
            "A retry changed identity or reacquired its existing permit");

        Expect(SubmitResponse(30008, writeB, ftp::Opcode::Ack, 41)
                .ftpMessagesApplied == 1 &&
            clientA->IsBusy() && clientB->IsBusy(),
            "A timeout/retry blocked B Write progress");
        const auto secondWriteDatagramB = sender.PopWithEndpoint();
        const ftp::Payload secondWriteB = DecodeRequest(secondWriteDatagramB.second);
        Expect(secondWriteDatagramB.first == 30008 &&
            secondWriteB.opcode == ftp::Opcode::WriteFile &&
            secondWriteB.offset > writeB.offset &&
            permitPool.GetMetrics().outstanding == 2,
            "B did not reuse its ACK-released permit while A was retrying");

        Expect(SubmitResponse(30007, writeA, ftp::Opcode::Ack, 41)
                .ftpMessagesApplied == 1,
            "A Write ACK did not route to A");
        const auto terminateDatagramA = sender.PopWithEndpoint();
        const ftp::Payload terminateA = DecodeRequest(terminateDatagramA.second);
        Expect(terminateDatagramA.first == 30007 &&
            terminateA.opcode == ftp::Opcode::TerminateSession &&
            clientA->IsAwaitingWriteCommit() && clientB->IsBusy() &&
            permitPool.GetMetrics().outstanding == 1,
            "A Terminate wait prevented B from retaining its Write permit");

        Expect(SubmitResponse(30007, writeA, ftp::Opcode::Ack, 41)
                .ftpMessagesApplied == 0 &&
            permitPool.GetMetrics().outstanding == 1,
            "duplicate A Write ACK released B's permit");
        Expect(SubmitResponse(30008, secondWriteB, ftp::Opcode::Nak, 41, { 1 })
                .ftpMessagesApplied == 1 &&
            permitPool.GetMetrics().outstanding == 0 && clientA->IsBusy(),
            "B Write NAK leaked a permit or terminated A");
        const auto resultB = clientB->ConsumeResult();
        Expect(resultB && !resultB->success && !clientB->ConsumeResult(),
            "B Write NAK did not complete B exactly once");

        Expect(SubmitResponse(30007, terminateA, ftp::Opcode::Ack, 41)
                .ftpMessagesApplied == 1,
            "A Terminate ACK did not route to A");
        const auto resultA = clientA->ConsumeResult();
        Expect(resultA && resultA->success && !clientA->ConsumeResult() &&
            !clientA->IsBusy() && !clientB->IsBusy() &&
            permitPool.GetMetrics().permitsGranted ==
                permitPool.GetMetrics().permitsReleased &&
            permitPool.GetMetrics().invalidReleaseCount == 0,
            "A/B terminal isolation leaked, double-released, or double-completed");
        Expect(SubmitResponse(30008, secondWriteB, ftp::Opcode::Ack, 41)
                .ftpMessagesApplied == 0 &&
            permitPool.GetMetrics().outstanding == 0,
            "stale B ACK advanced a terminal Client");
    }
}

int RunMavFtpProbeTests()
{
    try
    {
        TestCodec();
        TestArduPilotPathCompatibility();
        TestClientReadWriteAndCorrelation();
        TestClientTimeoutCancelShutdown();
        TestPerOpcodeTimeoutAndTerminateSafety();
        TestProbeReadOnlyAndGuards();
        TestIngressDispatcher();
        TestIngressTwoEndpointAckIsolation();
        TestIngressTwoEndpointWritePermitIsolation();
        std::cout << "MAVFTP protocol/client and MissionProfileProbe tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "MAVFTP/probe tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
