/**
 * @brief 定义 Console 启动配置及其安全默认值。
 *
 * 分类：Production。
 * 当前多机链使用 MAVLink bind、DSP、timeout 和安全限制；无人机 Remote Endpoint 来自实际收包来源。
 */
#pragma once

#include <cstdint>
#include <optional>
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
    inline constexpr std::uint32_t MavftpSegmentItems = 4000U;
    inline constexpr int MavftpRequestTimeoutMs = 750;
    inline constexpr int MavftpTerminateTimeoutMs = 2500;
    inline constexpr std::uint32_t MavftpRequestRetryCount = 3U;
    inline constexpr std::uint32_t MavftpWriteWindow = 16U;
    inline constexpr std::uint32_t MavftpGlobalWriteOutstandingCap = 800U;

}

struct AppConfig
{
    bool safeTestMode = true;
    bool mavlinkNetworkEnabled = false;
    bool realCommandEnabled = false;
    /** Independent destructive-data gate for production Route upload. */
    bool routeUploadEnabled = false;
    std::string mavlinkBindIP = AppConfigDefaults::MavlinkBindIP;
    int mavlinkBindPort = 0;
    bool dspgcsTelemetryEnabled = false;
    bool dspgcsCommandEnabled = false;
    std::string dspgcsTransportConfigPath = "config/DSPGCSTransport.ini";
    /** UTF-8 absolute local path; empty disables Artifact validation requests. */
    std::string artifactWorkspaceRoot;
    int dspgcsTelemetryIntervalMs =
        AppConfigDefaults::DSPGCSTelemetryIntervalMs;
    int mavlinkReceiveBufferBytes =
        AppConfigDefaults::MavlinkReceiveBufferBytes;
    int maxPacketsPerDrain = AppConfigDefaults::MaxPacketsPerDrain;
    int snapshotDatagramBytes = AppConfigDefaults::SnapshotDatagramBytes;
    int snapshotItemsPerChunk = AppConfigDefaults::SnapshotItemsPerChunk;
    /** Maximum Wire Mission Items per MAVFTP segment, including Reserved Home. */
    std::uint32_t mavftpSegmentItems =
        AppConfigDefaults::MavftpSegmentItems;
    int mavftpRequestTimeoutMs =
        AppConfigDefaults::MavftpRequestTimeoutMs;
    int mavftpTerminateTimeoutMs =
        AppConfigDefaults::MavftpTerminateTimeoutMs;
    std::uint32_t mavftpRequestRetryCount =
        AppConfigDefaults::MavftpRequestRetryCount;
    std::uint32_t mavftpWriteWindow =
        AppConfigDefaults::MavftpWriteWindow;
    std::uint32_t mavftpGlobalWriteOutstandingCap =
        AppConfigDefaults::MavftpGlobalWriteOutstandingCap;

    int heartbeatTimeoutMs = AppConfigDefaults::HeartbeatTimeoutMs;
    float takeoffAltitudeLimitMeters = 5.0f;
    float hoverAltitudeLimitMeters = 5.0f;

    static bool Load(
        const std::string& path,
        AppConfig& config,
        std::string& error);
};

/** Applies explicit production CLI values after INI load (CLI > INI > default). */
bool ApplyMavFtpGlobalWriteOutstandingCapCliOverride(
    AppConfig& config,
    std::optional<int> mavftpGlobalWriteOutstandingCap,
    std::string& error);
