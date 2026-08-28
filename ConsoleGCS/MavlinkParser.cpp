#include "MavlinkParser.h"

#include <chrono>
#include <cmath>
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
