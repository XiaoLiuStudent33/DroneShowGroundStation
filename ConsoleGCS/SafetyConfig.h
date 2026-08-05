#pragma once

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
    inline constexpr int MAVLINK_LOCAL_PORT = 10005;

    inline constexpr const char* MAVLINK_TARGET_IP = "192.168.1.145";
    inline constexpr int MAVLINK_TARGET_PORT = 10005;

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
    inline constexpr int COMMAND_ACK_TIMEOUT_MS = 5000;
    inline constexpr int MAX_COMMAND_PACKET_BYTES = 512;
}
