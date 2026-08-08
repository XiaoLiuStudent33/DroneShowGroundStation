/**
 * @brief 定义 Console 启动配置及其安全默认值。
 *
 * 分类：Production / Compatibility。
 * 当前多机链使用 MAVLink bind、DSP、timeout 和安全限制；target/local 字段服务旧单会话兼容路径。
 */
#pragma once

#include <string>

struct AppConfig
{
    bool safeTestMode = true;
    bool mavlinkNetworkEnabled = false;
    bool realCommandEnabled = false;
    std::string mavlinkBindIP = "0.0.0.0";
    int mavlinkBindPort = 0;
    bool dspgcsTelemetryEnabled = false;
    bool dspgcsCommandEnabled = false;
    std::string dspgcsTransportConfigPath = "config/DSPGCSTransport.ini";
    int dspgcsTelemetryIntervalMs = 250;
    std::string targetIP;
    int targetPort = 0;
    int localPort = 0;
    int heartbeatTimeoutMs = 3000;
    float takeoffAltitudeLimitMeters = 5.0f;
    float hoverAltitudeLimitMeters = 5.0f;

    static bool Load(
        const std::string& path,
        AppConfig& config,
        std::string& error);
};
