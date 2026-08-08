/**
 * @brief 定义 Console/UE 共享的 DSP-GCS Transport 配置值对象。
 *
 * 分类：Production。
 * 当前 Production 配置使用 localhost Loopback Endpoint；默认值仅用于缺省初始化。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dspgcs
{
    namespace DSPGCSDefaults
    {
        inline constexpr const char* LoopbackAddress = "127.0.0.1";
        inline constexpr std::uint16_t ConsoleBindPort = 12001;
        inline constexpr std::uint16_t UnrealBindPort = 12002;
        inline constexpr int SocketBufferBytes = 4 * 1024 * 1024;
        inline constexpr int ConsoleReceiverPollMs = 2;
        inline constexpr int UnrealReceiverPollMs = 20;
        inline constexpr int MinimumSocketBufferBytes = 64 * 1024;
        inline constexpr int MaximumSocketBufferBytes = 64 * 1024 * 1024;
        inline constexpr int MinimumReceiverPollMs = 1;
        inline constexpr int MaximumReceiverPollMs = 1000;
        inline constexpr std::size_t MaxUdpPayloadBytes = 65507;
    }

    enum class TransportType : std::uint8_t
    {
        Loopback = 1,
        UDP = 2
    };

    struct DSPGCSTransportConfig
    {
        TransportType type = TransportType::Loopback;
        std::string bindAddress = DSPGCSDefaults::LoopbackAddress;
        std::uint16_t bindPort = 0;
        std::string remoteAddress = DSPGCSDefaults::LoopbackAddress;
        std::uint16_t remotePort = 0;
        int receiveBufferBytes = DSPGCSDefaults::SocketBufferBytes;
        int receiverPollMs = DSPGCSDefaults::ConsoleReceiverPollMs;
    };

    inline DSPGCSTransportConfig MakeConsoleDSPGCSDefaults()
    {
        DSPGCSTransportConfig config;
        config.bindPort = DSPGCSDefaults::ConsoleBindPort;
        config.remotePort = DSPGCSDefaults::UnrealBindPort;
        config.receiverPollMs = DSPGCSDefaults::ConsoleReceiverPollMs;
        return config;
    }

    inline DSPGCSTransportConfig MakeUnrealDSPGCSDefaults()
    {
        DSPGCSTransportConfig config;
        config.bindPort = DSPGCSDefaults::UnrealBindPort;
        config.remotePort = DSPGCSDefaults::ConsoleBindPort;
        config.receiverPollMs = DSPGCSDefaults::UnrealReceiverPollMs;
        return config;
    }

    inline const char* ToString(const TransportType type)
    {
        switch (type)
        {
        case TransportType::Loopback: return "Loopback";
        case TransportType::UDP: return "UDP";
        }
        return "Unknown";
    }

    inline bool TryParseTransportType(const std::string& value, TransportType& output)
    {
        if (value == "Loopback" || value == "LOOPBACK" || value == "loopback")
        {
            output = TransportType::Loopback;
            return true;
        }
        if (value == "UDP" || value == "udp")
        {
            output = TransportType::UDP;
            return true;
        }
        return false;
    }
}
