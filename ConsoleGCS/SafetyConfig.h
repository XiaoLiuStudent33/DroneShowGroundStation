#pragma once

/**
 * @brief 保存进程级飞行安全开关、限制和少量旧链兼容常量。
 *
 * 分类：Production / Compatibility。
 * 当前 Production 使用运行时安全模式与高度限制；MAVLINK_TARGET/COMMAND_* 仅服务旧兼容设计。
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

    inline constexpr const char* MAVLINK_LOCAL_IP = "192.168.1.254";
    inline constexpr int MAVLINK_LOCAL_PORT = 10001;

    // Legacy / Compatibility：当前多机 Production 按发现的 Remote Endpoint 发送，
    // 不读取固定 MAVLINK_TARGET_*；这些值仍为旧单会话路径保留。
    inline constexpr const char* MAVLINK_TARGET_IP = "192.168.1.145";
    inline constexpr int MAVLINK_TARGET_PORT = 10001;

    // Legacy / Compatibility：旧 GCS1 UdpCommandServer 当前未由 Production 入口创建。
    inline constexpr const char* COMMAND_BIND_IP = "192.168.1.254";
    inline constexpr int COMMAND_BIND_PORT = 10006;
    inline constexpr const char* COMMAND_ALLOWED_CLIENT_IP = "192.168.1.254";

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
    // Legacy / Compatibility：以下限制仅由旧 DroneControlService 使用。
    inline constexpr int COMMAND_ACK_TIMEOUT_MS = 5000;
    inline constexpr int MAX_COMMAND_PACKET_BYTES = 512;
}
