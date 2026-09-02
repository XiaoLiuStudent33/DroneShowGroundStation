#pragma once

/**
 * @brief 封装 Console 当前支持的 Outbound MAVLink 消息和飞控动作。
 *
 * 分类：Production。
 * 输入：目标 SYSID/COMPID、模式或高度；输出：#11 SET_MODE、#76 COMMAND_LONG 等报文。
 * 关键约束：MAV_CMD 与 Message ID 分离，目标 SYSID 只参与 MAVLink 协议路由。
 */

#include "UdpClient.h"
#include <cstdint>


class MavlinkCommand
{

public:


    // 基础COMMAND_LONG
    static bool SendCommandLong(

        UdpClient& udp,

        uint8_t targetSystem,

        uint8_t targetComponent,

        uint16_t command,

        float param1 = 0,

        float param2 = 0,

        float param3 = 0,

        float param4 = 0,

        float param5 = 0,

        float param6 = 0,

        float param7 = 0

    );

    static bool Arm(
        UdpClient& udp,
        uint8_t targetSystem,
        uint8_t targetComponent
    );

    static bool Disarm(
        UdpClient& udp,
        uint8_t targetSystem,
        uint8_t targetComponent
    );

    static bool Guided(
        UdpClient& udp,
        uint8_t targetSystem,
        uint8_t targetComponent
    );

    // 起飞
    static bool Takeoff(

        UdpClient& udp,

        uint8_t system,

        uint8_t component,

        float altitude

    );



    // 降落
    static bool Land(

        UdpClient& udp,

        uint8_t system,

        uint8_t component

    );



    // 返航
    static bool RTL(

        UdpClient& udp,

        uint8_t system,

        uint8_t component

    );

    static bool Loiter(
        UdpClient& udp,
        uint8_t system,
        uint8_t component
    );

    static bool SetLedColor(
        UdpClient& udp,
        uint8_t red,
        uint8_t green,
        uint8_t blue);

    /** Sends custom MP_TIME_CONTROL message #78 (LEN=5, CRC_EXTRA=145). */
    static bool TimeControl(
        UdpClient& udp,
        uint8_t control,
        uint8_t controlMask,
        uint8_t hour,
        uint8_t minute,
        uint8_t second);

    /** Legacy dance start: SET_MODE AUTO, then MAV_CMD_USER_1 param1=1. */
    static bool ExecuteDanceStep(
        UdpClient& udp,
        uint8_t targetSystem,
        uint8_t targetComponent);

    /** Legacy dance end: cancel MP_TIME_CONTROL, then MAV_CMD_USER_1 param1=0. */
    static bool EndDanceStep(
        UdpClient& udp,
        uint8_t targetSystem,
        uint8_t targetComponent,
        uint8_t controlMask);

    static bool SetMode(

        UdpClient& udp,

        uint8_t targetSystem,

        uint8_t mode

    );
};
