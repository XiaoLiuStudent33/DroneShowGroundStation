#pragma once

/**
 * @brief 保存一个 DroneSession 当前已解析的 MAVLink 飞行状态。
 *
 * 分类：Production。
 * 输入：MavlinkParser；输出：Session 生命周期、Command、ConsoleUI 和 DSP Telemetry DTO。
 * 关键约束：绝对 GPS/AMSL 来自 #24，相对高度与融合运动状态来自 #33。
 */

#include <stdint.h>
#include <array>
#include <chrono>
#include <string>

struct MDHKStatusData
{
    bool received = false;
    std::int64_t lastReceiveUtcMs = 0;
    std::chrono::steady_clock::time_point lastReceiveMonotonic{};
    std::uint64_t sampleSequence = 0;

    std::uint32_t timeBootMs = 0;
    std::uint16_t gpsYear = 0;
    std::uint8_t gpsMonth = 0;
    std::uint8_t gpsDay = 0;
    std::uint8_t gpsHour = 0;
    std::uint8_t gpsMinute = 0;
    std::uint8_t gpsSecond = 0;
    std::uint32_t validFlags = 0;
    std::uint8_t schemaVersion = 0;

    bool gpsValid = false;
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    bool altitudeValid = false;
    float gpsAltitudeAmslMeters = 0.0f;
    float relativeAltitudeMeters = 0.0f;
    std::uint8_t gpsFixType = 0;
    std::uint8_t satellitesVisible = 0;

    bool batteryRemainingValid = false;
    std::int8_t batteryRemainingPercent = -1;
    bool batteryTemperatureValid = false;
    float batteryTemperatureCelsius = 0.0f;
    std::array<bool, 4> cellVoltageValid{};
    std::array<float, 4> cellVoltageVolts{};

    std::uint32_t freeMemoryBytes = 0;
    std::uint32_t flightSoftwareVersion = 0;
    float waypointDistanceMeters = 0.0f;
    float compassVariance = 0.0f;
    float magneticField1Mgauss = 0.0f;
    float magneticField2Mgauss = 0.0f;
    std::uint16_t batteryCycleCount = UINT16_MAX;
    std::uint16_t cellVoltageDeltaMv = UINT16_MAX;
    std::uint16_t ekfFlags = 0;
    std::uint16_t missionSequence = 0;
    std::uint16_t missionTotal = 0;
    std::int16_t batteryCurrentCentiamps = -1;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::array<std::uint8_t, 8> flightCustomVersion{};
    std::string hardwareId;
};


class FlightData
{

public:


    //--------------------------------------------------
    // MAVLink身份
    //--------------------------------------------------

    uint8_t systemID = 0;

    uint8_t componentID = 0;



    //--------------------------------------------------
    // Heartbeat
    //--------------------------------------------------

    bool online = false;

    bool armed = false;

    std::chrono::steady_clock::time_point lastHeartbeat{};



    int mode = 0;

    std::string modeName = "Unknown";



    uint8_t autopilot = 0;

    std::string autopilotName = "Unknown";



    uint8_t vehicleType = 0;

    std::string vehicleName = "Unknown";



    uint8_t systemStatus = 0;

    std::string systemStatusName = "Unknown";

    bool autopilotVersionValid = false;
    std::uint64_t autopilotCapabilities = 0;
    std::uint32_t flightSoftwareVersion = 0;
    std::uint32_t boardVersion = 0;
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;



    //--------------------------------------------------
    // 姿态
    //--------------------------------------------------

    bool attitudeValid = false;

    float roll = 0;

    float pitch = 0;

    float yaw = 0;



    //--------------------------------------------------
    // GPS
    //--------------------------------------------------

    bool gpsValid = false;

    bool altitudeValid = false;

    uint8_t gpsFixType = 0;

    uint8_t satellitesVisible = 0;

    // Wall-clock audit timestamp for the most recently received GPS_RAW_INT.
    // This is captured by Console ingress, not when a DSP telemetry DTO is built.
    std::int64_t lastGpsReceiveUtcMs = 0;

    // Process-local monotonic authority for GPS age/timeout decisions. This value
    // is intentionally not serialized across DSP-GCS.
    std::chrono::steady_clock::time_point lastGpsReceiveMonotonic{};

    // In-process identity for unique GPS_RAW_INT samples. DSP-GCS reuses the UTC
    // timestamp because the wire schema is frozen and already carries it.
    std::uint64_t gpsSampleSequence = 0;



    double latitude = 0;

    double longitude = 0;



    // 海拔高度
    float altitude = 0;


    // 相对起飞点高度
    float relativeAltitude = 0;

    // 水平地速（m/s）
    float groundSpeed = 0;



    //--------------------------------------------------
    // 电池
    //--------------------------------------------------

    int batteryRemaining = -1;

    // Vendor MAVLink #187 status is intentionally isolated from the standard
    // operational fields above. UI/detail telemetry must not replace the GPS,
    // altitude or battery authority used by flight-control workflows.
    MDHKStatusData mdhkStatus;



    //--------------------------------------------------
    // 最近一次命令确认
    //--------------------------------------------------

    bool commandAckValid = false;

    uint16_t lastAckCommand = 0;

    uint8_t lastAckResult = 0;

    std::chrono::steady_clock::time_point lastAckTime{};


};
