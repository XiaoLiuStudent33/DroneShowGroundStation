#include "MavFtpProtocol.h"

#include <algorithm>
#include <limits>

namespace dronegcs::mavftp
{
    namespace
    {
        bool Fail(Error& error, const ErrorCode code, const char* detail)
        {
            error.code = code;
            error.detail = detail ? detail : "";
            return false;
        }
    }

    bool IsSupportedOpcode(const Opcode opcode)
    {
        switch (opcode)
        {
        case Opcode::None:
        case Opcode::TerminateSession:
        case Opcode::ResetSessions:
        case Opcode::ListDirectory:
        case Opcode::OpenFileRO:
        case Opcode::ReadFile:
        case Opcode::CreateFile:
        case Opcode::WriteFile:
        case Opcode::OpenFileWO:
        case Opcode::TruncateFile:
        case Opcode::CalcFileCRC32:
        case Opcode::Ack:
        case Opcode::Nak:
            return true;
        }
        return false;
    }

    const char* OpcodeName(const Opcode opcode)
    {
        switch (opcode)
        {
        case Opcode::None: return "None";
        case Opcode::TerminateSession: return "TerminateSession";
        case Opcode::ResetSessions: return "ResetSessions";
        case Opcode::ListDirectory: return "ListDirectory";
        case Opcode::OpenFileRO: return "OpenFileRO";
        case Opcode::ReadFile: return "ReadFile";
        case Opcode::CreateFile: return "CreateFile";
        case Opcode::WriteFile: return "WriteFile";
        case Opcode::OpenFileWO: return "OpenFileWO";
        case Opcode::TruncateFile: return "TruncateFile";
        case Opcode::CalcFileCRC32: return "CalcFileCRC32";
        case Opcode::Ack: return "ACK";
        case Opcode::Nak: return "NAK";
        }
        return "Unsupported";
    }

    std::uint16_t NextSequence(const std::uint16_t sequence)
    {
        return static_cast<std::uint16_t>(sequence + 1U);
    }

    std::uint32_t ReadU32LittleEndian(const std::uint8_t* bytes)
    {
        return static_cast<std::uint32_t>(bytes[0])
            | (static_cast<std::uint32_t>(bytes[1]) << 8)
            | (static_cast<std::uint32_t>(bytes[2]) << 16)
            | (static_cast<std::uint32_t>(bytes[3]) << 24);
    }

    void WriteU32LittleEndian(std::uint8_t* bytes, const std::uint32_t value)
    {
        bytes[0] = static_cast<std::uint8_t>(value);
        bytes[1] = static_cast<std::uint8_t>(value >> 8);
        bytes[2] = static_cast<std::uint8_t>(value >> 16);
        bytes[3] = static_cast<std::uint8_t>(value >> 24);
    }

    bool EncodePayload(
        const Payload& payload,
        std::array<std::uint8_t, PayloadBytes>& output,
        Error& error)
    {
        output.fill(0);
        if (!IsSupportedOpcode(payload.opcode) ||
            !IsSupportedOpcode(payload.requestOpcode))
        {
            return Fail(error, ErrorCode::UnsupportedOpcode,
                "MAVFTP opcode is not supported by this client.");
        }
        if (payload.data.size() > MaximumDataBytes)
        {
            return Fail(error, ErrorCode::InvalidDataSize,
                "MAVFTP payload data exceeds 239 bytes.");
        }
        if (payload.data.size() >
            static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()
                - payload.offset))
        {
            return Fail(error, ErrorCode::OffsetOverflow,
                "MAVFTP offset plus data size overflows uint32.");
        }

        output[0] = static_cast<std::uint8_t>(payload.sequence);
        output[1] = static_cast<std::uint8_t>(payload.sequence >> 8);
        output[2] = payload.session;
        output[3] = static_cast<std::uint8_t>(payload.opcode);
        output[4] = static_cast<std::uint8_t>(payload.data.size());
        output[5] = static_cast<std::uint8_t>(payload.requestOpcode);
        output[6] = payload.burstComplete ? 1 : 0;
        output[7] = 0;
        WriteU32LittleEndian(output.data() + 8, payload.offset);
        std::copy(payload.data.begin(), payload.data.end(),
            output.begin() + HeaderBytes);
        error = {};
        return true;
    }

    bool DecodePayload(
        const std::uint8_t* bytes,
        const std::size_t size,
        Payload& output,
        Error& error)
    {
        output = {};
        if (bytes == nullptr || size != PayloadBytes)
        {
            return Fail(error, ErrorCode::InvalidDataSize,
                "MAVFTP payload must contain exactly 251 bytes.");
        }
        const Opcode opcode = static_cast<Opcode>(bytes[3]);
        const Opcode requestOpcode = static_cast<Opcode>(bytes[5]);
        if (!IsSupportedOpcode(opcode) || !IsSupportedOpcode(requestOpcode))
        {
            return Fail(error, ErrorCode::UnsupportedOpcode,
                "MAVFTP payload contains an unsupported opcode.");
        }
        if (bytes[4] > MaximumDataBytes)
        {
            return Fail(error, ErrorCode::InvalidDataSize,
                "MAVFTP payload size field exceeds 239 bytes.");
        }
        if (bytes[6] > 1)
        {
            return Fail(error, ErrorCode::InvalidBurstFlag,
                "MAVFTP burst_complete must be zero or one.");
        }
        if (bytes[7] != 0)
        {
            return Fail(error, ErrorCode::NonZeroPadding,
                "MAVFTP padding byte must be zero.");
        }
        const std::uint32_t offset = ReadU32LittleEndian(bytes + 8);
        if (bytes[4] >
            static_cast<std::uint32_t>((std::numeric_limits<std::uint32_t>::max)()
                - offset))
        {
            return Fail(error, ErrorCode::OffsetOverflow,
                "MAVFTP offset plus data size overflows uint32.");
        }
        if (opcode == Opcode::Nak && bytes[4] == 0)
        {
            return Fail(error, ErrorCode::InvalidNak,
                "MAVFTP NAK must include an error byte.");
        }

        output.sequence = static_cast<std::uint16_t>(bytes[0])
            | static_cast<std::uint16_t>(bytes[1] << 8);
        output.session = bytes[2];
        output.opcode = opcode;
        output.requestOpcode = requestOpcode;
        output.burstComplete = bytes[6] != 0;
        output.offset = offset;
        output.data.assign(bytes + HeaderBytes, bytes + HeaderBytes + bytes[4]);
        error = {};
        return true;
    }

    bool MakePathData(
        const std::string& path,
        std::vector<std::uint8_t>& output,
        Error& error)
    {
        output.clear();
        if (path.empty() || path.size() + 1 > MaximumDataBytes ||
            path.find('\0') != std::string::npos)
        {
            return Fail(error, ErrorCode::InvalidPath,
                "MAVFTP path must be non-empty, NUL-free, and fit one request.");
        }
        // ArduPilot-compatible name requests carry strlen(path) raw bytes in
        // header.size. The fixed MAVLink payload remains zero-padded, so the
        // server can safely terminate data[size] without counting that NUL.
        output.assign(path.begin(), path.end());
        error = {};
        return true;
    }

    const char* NakErrorName(const std::uint8_t errorCode)
    {
        switch (errorCode)
        {
        case 0: return "None";
        case 1: return "Fail";
        case 2: return "FailErrno";
        case 3: return "InvalidDataSize";
        case 4: return "InvalidSession";
        case 5: return "NoSessionsAvailable";
        case 6: return "EndOfFile";
        case 7: return "UnknownCommand";
        case 8: return "FileExists";
        case 9: return "FileProtected";
        case 10: return "FileNotFound";
        default: return "UnknownError";
        }
    }

    std::uint32_t CalculateCrc32(
        const std::uint8_t* bytes,
        const std::size_t size)
    {
        std::uint32_t crc = 0xffffffffU;
        for (std::size_t index = 0; index < size; ++index)
        {
            crc ^= bytes[index];
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xedb88320U &
                    (0U - (crc & 1U)));
        }
        return ~crc;
    }
}
