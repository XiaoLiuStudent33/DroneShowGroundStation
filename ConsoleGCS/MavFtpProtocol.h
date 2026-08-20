#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dronegcs::mavftp
{
    inline constexpr std::size_t PayloadBytes = 251;
    inline constexpr std::size_t HeaderBytes = 12;
    inline constexpr std::size_t MaximumDataBytes = PayloadBytes - HeaderBytes;

    enum class Opcode : std::uint8_t
    {
        None = 0,
        TerminateSession = 1,
        ResetSessions = 2,
        ListDirectory = 3,
        OpenFileRO = 4,
        ReadFile = 5,
        CreateFile = 6,
        WriteFile = 7,
        OpenFileWO = 11,
        TruncateFile = 12,
        CalcFileCRC32 = 14,
        Ack = 128,
        Nak = 129
    };

    enum class ErrorCode : std::uint8_t
    {
        None,
        UnsupportedOpcode,
        InvalidDataSize,
        InvalidBurstFlag,
        NonZeroPadding,
        OffsetOverflow,
        InvalidPath,
        InvalidNak
    };

    struct Error
    {
        ErrorCode code = ErrorCode::None;
        std::string detail;
    };

    struct Payload
    {
        std::uint16_t sequence = 0;
        std::uint8_t session = 0;
        Opcode opcode = Opcode::ResetSessions;
        Opcode requestOpcode = Opcode::None;
        bool burstComplete = false;
        std::uint32_t offset = 0;
        std::vector<std::uint8_t> data;
    };

    bool IsSupportedOpcode(Opcode opcode);
    const char* OpcodeName(Opcode opcode);
    std::uint16_t NextSequence(std::uint16_t sequence);

    bool EncodePayload(
        const Payload& payload,
        std::array<std::uint8_t, PayloadBytes>& output,
        Error& error);
    bool DecodePayload(
        const std::uint8_t* bytes,
        std::size_t size,
        Payload& output,
        Error& error);
    bool MakePathData(
        const std::string& path,
        std::vector<std::uint8_t>& output,
        Error& error);
    const char* NakErrorName(std::uint8_t errorCode);

    std::uint32_t ReadU32LittleEndian(const std::uint8_t* bytes);
    void WriteU32LittleEndian(std::uint8_t* bytes, std::uint32_t value);
    std::uint32_t CalculateCrc32(const std::uint8_t* bytes, std::size_t size);
}
