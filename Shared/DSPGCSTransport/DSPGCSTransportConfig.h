/**
 * @brief 定义 Console/UE 共享的 DSP-GCS Transport 配置值对象。
 *
 * 分类：Production。
 * 当前 Production 配置使用 localhost Loopback Endpoint；默认值仅用于缺省初始化。
 */
#pragma once

#include <cstdint>
#include <string>

namespace dspgcs
{
    enum class TransportType : std::uint8_t
    {
        Loopback = 1,
        UDP = 2
    };

    struct DSPGCSTransportConfig
    {
        TransportType type = TransportType::Loopback;
        std::string bindAddress = "127.0.0.1";
        std::uint16_t bindPort = 0;
        std::string remoteAddress = "127.0.0.1";
        std::uint16_t remotePort = 0;
    };

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
