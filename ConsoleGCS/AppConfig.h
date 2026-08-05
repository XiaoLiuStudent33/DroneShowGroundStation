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
