#include "MavlinkParser.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <string>

namespace
{
    std::int64_t UtcNowMilliseconds()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

/**
 * @brief 实现 Console 当前 Inbound MAVLink 能力到 FlightData 的字段映射。
 *
 * 分类：Production。
 * 输入：HEARTBEAT、SYS_STATUS、GPS_RAW_INT、ATTITUDE、GLOBAL_POSITION_INT、COMMAND_ACK。
 * 输出：仅更新对应职责字段，不在解析层创建或删除 DroneSession。
 */

//==================================================
// ArduPilot Copter 飞行模式转换
//==================================================
std::string MavlinkParser::GetModeName(int mode)
{
    switch (mode)
    {
    case 0:  return "STABILIZE";
    case 1:  return "ACRO";
    case 2:  return "ALT_HOLD";
    case 3:  return "AUTO";
    case 4:  return "GUIDED";
    case 5:  return "LOITER";
    case 6:  return "RTL";
    case 7:  return "CIRCLE";
    case 9:  return "LAND";
    case 13: return "SPORT";
    case 16: return "POSHOLD";
    case 17: return "BRAKE";

    default:
        return "UNKNOWN";
    }
}

//==================================================
// 飞控类型
//==================================================
std::string MavlinkParser::GetAutopilotName(int type)
{
    switch (type)
    {
    case MAV_AUTOPILOT_ARDUPILOTMEGA:
        return "ArduPilot";

    case MAV_AUTOPILOT_PX4:
        return "PX4";

    default:
        return "Unknown";
    }
}

//==================================================
// 飞机类型
//==================================================
std::string MavlinkParser::GetVehicleName(int type)
{
    switch (type)
    {
    case MAV_TYPE_QUADROTOR:
        return "Quadrotor";

    case MAV_TYPE_HEXAROTOR:
        return "Hexarotor";

    case MAV_TYPE_OCTOROTOR:
        return "Octorotor";

    case MAV_TYPE_FIXED_WING:
        return "Plane";

    default:
        return "Unknown";
    }
}

//==================================================
// 飞控状态
//==================================================
std::string MavlinkParser::GetSystemStatusName(int status)
{
    switch (status)
    {
    case MAV_STATE_UNINIT:
        return "UNINIT";

    case MAV_STATE_BOOT:
        return "BOOT";

    case MAV_STATE_CALIBRATING:
        return "CALIBRATING";

    case MAV_STATE_STANDBY:
        return "STANDBY";

    case MAV_STATE_ACTIVE:
        return "ACTIVE";

    case MAV_STATE_CRITICAL:
        return "CRITICAL";

    case MAV_STATE_EMERGENCY:
        return "EMERGENCY";

    case MAV_STATE_POWEROFF:
        return "POWEROFF";

    default:
        return "UNKNOWN";
    }
}

//==================================================
// MAVLink 数据解析
//==================================================
void MavlinkParser::Parse(
    const mavlink_message_t& msg,
    FlightData& data)
{
    ParseAt(
        msg,
        data,
        std::chrono::steady_clock::now(),
        UtcNowMilliseconds());
}

void MavlinkParser::ParseAt(
    const mavlink_message_t& msg,
    FlightData& data,
    const std::chrono::steady_clock::time_point receiveMonotonic,
    const std::int64_t receiveUtcMs)
{
    switch (msg.msgid)
    {
        //--------------------------------------------------
        // MAVLink #0 HEARTBEAT：飞控身份、在线/解锁状态与飞行模式。
        //--------------------------------------------------
    case MAVLINK_MSG_ID_HEARTBEAT:
    {
        mavlink_heartbeat_t hb;

        mavlink_msg_heartbeat_decode(&msg, &hb);

        data.systemID = msg.sysid;
        data.componentID = msg.compid;

        data.armed =
            (hb.base_mode & MAV_MODE_FLAG_SAFETY_ARMED)
            != 0;

        data.mode = hb.custom_mode;
        data.modeName = GetModeName(hb.custom_mode);

        data.online = true;
        data.lastHeartbeat =
            std::chrono::steady_clock::now();

        data.autopilot = hb.autopilot;
        data.autopilotName = GetAutopilotName(hb.autopilot);

        data.vehicleType = hb.type;
        data.vehicleName = GetVehicleName(hb.type);

        data.systemStatus = hb.system_status;
        data.systemStatusName = GetSystemStatusName(hb.system_status);

        break;
    }

    //--------------------------------------------------
    // MAVLink #30 ATTITUDE：姿态角 Roll/Pitch/Yaw。
    //--------------------------------------------------
    case MAVLINK_MSG_ID_ATTITUDE:
    {
        mavlink_attitude_t att;

        mavlink_msg_attitude_decode(&msg, &att);

        data.roll = att.roll;
        data.pitch = att.pitch;
        data.yaw = att.yaw;
        data.attitudeValid = true;

        break;
    }

    //--------------------------------------------------
    // MAVLink #33 GLOBAL_POSITION_INT：飞行器融合运动状态。
    // 只维护相对高度和速度；绝对经纬高统一由#24 GPS_RAW_INT维护。
    //--------------------------------------------------
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
    {
        mavlink_global_position_int_t gps;


        mavlink_msg_global_position_int_decode(
            &msg,
            &gps
        );


        // 相对起飞点高度（飞控控制使用）
        data.relativeAltitude =
            gps.relative_alt / 1000.0;

        const float velocityNorth =
            gps.vx / 100.0f;
        const float velocityEast =
            gps.vy / 100.0f;
        data.groundSpeed = std::sqrt(
            velocityNorth * velocityNorth +
            velocityEast * velocityEast);

        break;
    }

    //--------------------------------------------------
    // MAVLink #24 GPS_RAW_INT：绝对GPS位置权威源。
    // 统一维护纬度、经度、AMSL海拔、Fix、卫星数和GPS有效性。
    // lat/lon单位为1e-7度，alt为MSL毫米。
    //--------------------------------------------------
    case MAVLINK_MSG_ID_GPS_RAW_INT:
    {
        mavlink_gps_raw_int_t gps;

        mavlink_msg_gps_raw_int_decode(&msg, &gps);

        data.latitude =
            static_cast<double>(gps.lat) / 10000000.0;
        data.longitude =
            static_cast<double>(gps.lon) / 10000000.0;
        data.altitude =
            static_cast<float>(static_cast<double>(gps.alt) / 1000.0);
        data.gpsFixType = gps.fix_type;
        data.satellitesVisible = gps.satellites_visible;
        data.lastGpsReceiveUtcMs = receiveUtcMs;
        data.lastGpsReceiveMonotonic = receiveMonotonic;
        ++data.gpsSampleSequence;
        data.gpsValid =
            gps.fix_type >= 3 &&
            (gps.lat != 0 || gps.lon != 0);
        // 海拔有效性与经纬度非零规则解耦；海平面0m是合法AMSL值。
        data.altitudeValid = gps.fix_type >= 3;

        break;
    }

    //--------------------------------------------------
    // MAVLink #1 SYS_STATUS：剩余电量百分比。
    //--------------------------------------------------
    case MAVLINK_MSG_ID_SYS_STATUS:
    {
        mavlink_sys_status_t status;

        mavlink_msg_sys_status_decode(&msg, &status);

        data.batteryRemaining = status.battery_remaining;

        break;
    }

    //--------------------------------------------------
    // Vendor MAVLink #187 MDHK_STATUS：1 Hz consolidated detail telemetry.
    // This is a separate UI/status source and never overwrites the standard
    // operational GPS/SYS_STATUS fields in FlightData.
    //--------------------------------------------------
    case MAVLINK_MSG_ID_MDHK_STATUS:
    {
        mavlink_mdhk_status_t status{};
        mavlink_msg_mdhk_status_decode(&msg, &status);

        MDHKStatusData& target = data.mdhkStatus;
        target.received = true;
        target.lastReceiveUtcMs = receiveUtcMs;
        target.lastReceiveMonotonic = receiveMonotonic;
        ++target.sampleSequence;
        target.timeBootMs = status.time_boot_ms;
        target.gpsYear = status.gps_year;
        target.gpsMonth = status.gps_month;
        target.gpsDay = status.gps_day;
        target.gpsHour = status.gps_hour;
        target.gpsMinute = status.gps_minute;
        target.gpsSecond = status.gps_second;
        target.validFlags = status.valid_flags;
        target.schemaVersion = status.schema_version;

        target.gpsFixType = status.gps_fix_type;
        target.satellitesVisible = status.satellites_visible;
        target.gpsValid = status.gps_fix_type >= 3;
        target.latitudeDeg = static_cast<double>(status.latitude) / 10000000.0;
        target.longitudeDeg = static_cast<double>(status.longitude) / 10000000.0;
        target.altitudeValid = status.gps_fix_type >= 3;
        target.gpsAltitudeAmslMeters =
            static_cast<float>(static_cast<double>(status.gps_alt) / 1000.0);
        target.relativeAltitudeMeters =
            static_cast<float>(static_cast<double>(status.relative_alt) / 1000.0);

        target.batteryRemainingValid = status.battery_remaining >= 0;
        target.batteryRemainingPercent = status.battery_remaining;
        target.batteryTemperatureValid =
            status.battery_temperature != std::numeric_limits<std::int16_t>::max();
        target.batteryTemperatureCelsius = target.batteryTemperatureValid
            ? static_cast<float>(status.battery_temperature) / 100.0f
            : 0.0f;
        for (std::size_t index = 0; index < target.cellVoltageVolts.size(); ++index)
        {
            target.cellVoltageValid[index] =
                status.cell_voltages[index] != std::numeric_limits<std::uint16_t>::max();
            target.cellVoltageVolts[index] = target.cellVoltageValid[index]
                ? static_cast<float>(status.cell_voltages[index]) / 1000.0f
                : 0.0f;
        }

        target.freeMemoryBytes = status.freemem32;
        target.flightSoftwareVersion = status.flight_sw_version;
        target.waypointDistanceMeters = status.wp_distance;
        target.compassVariance = status.compass_variance;
        target.magneticField1Mgauss = status.mag_field1;
        target.magneticField2Mgauss = status.mag_field2;
        target.batteryCycleCount = status.battery_cycle_count;
        target.cellVoltageDeltaMv = status.cell_voltage_delta;
        target.ekfFlags = status.ekf_flags;
        target.missionSequence = status.mission_seq;
        target.missionTotal = status.mission_total;
        target.batteryCurrentCentiamps = status.battery_current;
        target.red = status.rgb_red;
        target.green = status.rgb_green;
        target.blue = status.rgb_blue;
        for (std::size_t index = 0; index < target.flightCustomVersion.size(); ++index)
        {
            target.flightCustomVersion[index] = status.flight_custom_version[index];
        }
        target.hardwareId.assign(status.hardware_id, sizeof(status.hardware_id));
        const std::size_t terminator = target.hardwareId.find('\0');
        if (terminator != std::string::npos)
        {
            target.hardwareId.resize(terminator);
        }
        break;
    }

    case MAVLINK_MSG_ID_AUTOPILOT_VERSION:
    {
        mavlink_autopilot_version_t version{};
        mavlink_msg_autopilot_version_decode(&msg, &version);
        data.autopilotVersionValid = true;
        data.autopilotCapabilities = version.capabilities;
        data.flightSoftwareVersion = version.flight_sw_version;
        data.boardVersion = version.board_version;
        data.vendorId = version.vendor_id;
        data.productId = version.product_id;
        break;
    }

    //--------------------------------------------------
    // MAVLink #77 COMMAND_ACK：控制命令结果确认。
    //--------------------------------------------------
    case MAVLINK_MSG_ID_COMMAND_ACK:
    {
        mavlink_command_ack_t ack;

        mavlink_msg_command_ack_decode(&msg, &ack);

        data.commandAckValid = true;
        data.lastAckCommand = ack.command;
        data.lastAckResult = ack.result;
        data.lastAckTime =
            std::chrono::steady_clock::now();

        break;
    }

    //--------------------------------------------------
    // 默认
    //--------------------------------------------------
    default:
        break;
    }
}
