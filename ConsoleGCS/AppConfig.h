/**
 * @brief 定义 Console 启动配置及其安全默认值。
 *
 * 分类：Production。
 * 当前多机链使用 MAVLink bind、DSP、timeout 和安全限制；无人机 Remote Endpoint 来自实际收包来源。
 */
#pragma once

#include <string>

namespace AppConfigDefaults
{
    inline constexpr const char* MavlinkBindIP = "0.0.0.0";
    inline constexpr int HeartbeatTimeoutMs = 3000;
    inline constexpr int DSPGCSTelemetryIntervalMs = 250;
    inline constexpr int MavlinkReceiveBufferBytes = 4 * 1024 * 1024;
    inline constexpr int MaxPacketsPerDrain = 4096;
    inline constexpr int SnapshotDatagramBytes = 60 * 1024;
    inline constexpr int SnapshotItemsPerChunk = 512;

}

struct AppConfig
{
    bool safeTestMode = true;
    bool mavlinkNetworkEnabled = false;
    bool realCommandEnabled = false;
    std::string mavlinkBindIP = AppConfigDefaults::MavlinkBindIP;
    int mavlinkBindPort = 0;
    bool dspgcsTelemetryEnabled = false;
    bool dspgcsCommandEnabled = false;
    std::string dspgcsTransportConfigPath = "config/DSPGCSTransport.ini";
    int dspgcsTelemetryIntervalMs =
        AppConfigDefaults::DSPGCSTelemetryIntervalMs;
    int mavlinkReceiveBufferBytes =
        AppConfigDefaults::MavlinkReceiveBufferBytes;
    int maxPacketsPerDrain = AppConfigDefaults::MaxPacketsPerDrain;
    int snapshotDatagramBytes = AppConfigDefaults::SnapshotDatagramBytes;
    int snapshotItemsPerChunk = AppConfigDefaults::SnapshotItemsPerChunk;

    int heartbeatTimeoutMs = AppConfigDefaults::HeartbeatTimeoutMs;
    float takeoffAltitudeLimitMeters = 5.0f;
    float hoverAltitudeLimitMeters = 5.0f;

    static bool Load(
        const std::string& path,
        AppConfig& config,
        std::string& error);
};
