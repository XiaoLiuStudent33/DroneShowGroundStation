#pragma once

/**
 * @brief 保存一个 DroneSession 当前已解析的 MAVLink 飞行状态。
 *
 * 分类：Production。
 * 输入：MavlinkParser；输出：Session 生命周期、Command、ConsoleUI 和 DSP Telemetry DTO。
 * 关键约束：绝对 GPS/AMSL 来自 #24，相对高度与融合运动状态来自 #33。
 */

#include <stdint.h>
#include <chrono>
#include <string>


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



    //--------------------------------------------------
    // 最近一次命令确认
    //--------------------------------------------------

    bool commandAckValid = false;

    uint16_t lastAckCommand = 0;

    uint8_t lastAckResult = 0;

    std::chrono::steady_clock::time_point lastAckTime{};


};
