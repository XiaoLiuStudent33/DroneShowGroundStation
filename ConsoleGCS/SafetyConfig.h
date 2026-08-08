#pragma once

#include <string>

/**
 * @brief 保存进程级飞行安全开关、限制和少量旧链兼容常量。
 *
 * 分类：Production / Compatibility。
 * 当前 Production 使用运行时安全模式与高度限制；MAVLINK_TARGET_* 仅服务旧单会话兼容设计。
 */
namespace SafetyConfig
{
    // Runtime value is loaded once from gcs-config.ini during startup.
    // The in-code default remains safe if configuration loading fails or the
    // key is omitted.
    inline bool SAFE_TEST_MODE = true;
    inline bool REAL_COMMAND_ENABLED = false;

    inline void SetSafeTestMode(const bool enabled)
    {
        SAFE_TEST_MODE = enabled;
    }

    inline void SetRealCommandEnabled(const bool enabled)
    {
        REAL_COMMAND_ENABLED = enabled;
    }

    inline bool CanEnableRealCommands(
        const bool mavlinkNetworkEnabled,
        const bool dspgcsCommandEnabled,
        const std::string& mavlinkBindAddress,
        const int mavlinkBindPort)
    {
        // Safety validates explicit operator gates and a concrete local bind.
        // The endpoint value itself belongs exclusively to AppConfig.
        return REAL_COMMAND_ENABLED &&
            mavlinkNetworkEnabled &&
            dspgcsCommandEnabled &&
            !mavlinkBindAddress.empty() &&
            mavlinkBindAddress != "0.0.0.0" &&
            mavlinkBindPort > 0 &&
            mavlinkBindPort <= 65535;
    }

    // Legacy / Compatibility：当前多机 Production 按发现的 Remote Endpoint 发送，
    // 不读取固定 MAVLINK_TARGET_*；这些值仍为旧单会话路径保留。
    inline constexpr const char* MAVLINK_TARGET_IP = "192.168.1.145";
    inline constexpr int MAVLINK_TARGET_PORT = 10001;

    inline constexpr float DEFAULT_TAKEOFF_ALTITUDE_METERS = 1.0f;
    inline constexpr float DEFAULT_FLIGHT_ALTITUDE_LIMIT_METERS = 5.0f;
    inline float TAKEOFF_ALTITUDE_LIMIT_METERS =
        DEFAULT_FLIGHT_ALTITUDE_LIMIT_METERS;
    inline float HOVER_ALTITUDE_LIMIT_METERS =
        DEFAULT_FLIGHT_ALTITUDE_LIMIT_METERS;

    inline void SetFlightAltitudeLimits(
        const float takeoffLimitMeters,
        const float hoverLimitMeters)
    {
        TAKEOFF_ALTITUDE_LIMIT_METERS = takeoffLimitMeters;
        HOVER_ALTITUDE_LIMIT_METERS = hoverLimitMeters;
    }
}
