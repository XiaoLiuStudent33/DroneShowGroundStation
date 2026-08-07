#include "MavlinkCommand.h"

#include <iostream>

extern "C"
{
#include "ardupilotmega/mavlink.h"
}


bool MavlinkCommand::SendCommandLong(

    UdpClient& udp,

    uint8_t targetSystem,

    uint8_t targetComponent,

    uint16_t command,

    float param1,

    float param2,

    float param3,

    float param4,

    float param5,

    float param6,

    float param7

)
{
    // MAVLink #76 COMMAND_LONG：承载当前 ARM/DISARM/TAKEOFF/LAND/RTL 的 MAV_CMD。

    mavlink_message_t msg;


    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];



    mavlink_msg_command_long_pack(

        255,

        MAV_COMP_ID_MISSIONPLANNER,

        &msg,


        targetSystem,

        targetComponent,


        command,


        0,


        param1,

        param2,

        param3,

        param4,

        param5,

        param6,

        param7

    );




    uint16_t len =

        mavlink_msg_to_send_buffer(

            buffer,

            &msg

        );



    return udp.Send(

        buffer,

        len

    );



  


}


bool MavlinkCommand::Arm(
    UdpClient& udp,
    uint8_t targetSystem,
    uint8_t targetComponent
)
{
    // MAV_CMD #400 COMPONENT_ARM_DISARM：param1=1，param2=0，保留飞控安全检查。

    return SendCommandLong(

        udp,

        targetSystem,

        targetComponent,

        MAV_CMD_COMPONENT_ARM_DISARM,

        1,       // ARM

        0,       // Normal ARM: keep all flight-controller safety checks enabled

        0,
        0,
        0,
        0,
        0

    );

}
//==================================================
// DISARM 上锁
//==================================================

bool MavlinkCommand::Disarm(

    UdpClient& udp,

    uint8_t targetSystem,

    uint8_t targetComponent

)
{
    // MAV_CMD #400 COMPONENT_ARM_DISARM：param1=0 上锁。

    return SendCommandLong(

        udp,

        targetSystem,

        targetComponent,

        MAV_CMD_COMPONENT_ARM_DISARM,

        0,      // param1 = 0 上锁

        0,
        0,
        0,
        0,
        0,
        0

    );

}

bool MavlinkCommand::SetMode(

    UdpClient& udp,

    uint8_t targetSystem,

    uint8_t mode

)
{
    // MAVLink #11 SET_MODE：ArduCopter custom_mode 由调用方传入。

    mavlink_message_t msg;


    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];



    mavlink_msg_set_mode_pack(

        255,

        MAV_COMP_ID_MISSIONPLANNER,

        &msg,

        targetSystem,

        MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,

        mode

    );



    uint16_t len =

        mavlink_msg_to_send_buffer(

            buffer,

            &msg

        );



    return udp.Send(

        buffer,

        len

    );



}

bool MavlinkCommand::Guided(

    UdpClient& udp,

    uint8_t targetSystem,

    uint8_t targetComponent

)
{

    (void)targetComponent;


    return SetMode(
        udp,
        targetSystem,
        4
    );

}


bool MavlinkCommand::Takeoff(

    UdpClient& udp,

    uint8_t system,

    uint8_t component,

    float altitude

)
{
    // MAV_CMD #22 NAV_TAKEOFF：param7 为目标高度。

    return SendCommandLong(

        udp,

        system,

        component,

        MAV_CMD_NAV_TAKEOFF,

        0,      // pitch

        0,

        0,

        0,

        0,      // latitude

        0,      // longitude

        altitude

    );

}



bool MavlinkCommand::Land(

    UdpClient& udp,

    uint8_t system,

    uint8_t component

)
{
    // MAV_CMD #21 NAV_LAND：当前参数均使用默认值 0。

    return SendCommandLong(

        udp,

        system,

        component,

        MAV_CMD_NAV_LAND

    );

}



bool MavlinkCommand::RTL(

    UdpClient& udp,

    uint8_t system,

    uint8_t component

)
{
    // MAV_CMD #20 NAV_RETURN_TO_LAUNCH：当前参数均使用默认值 0。

    return SendCommandLong(

        udp,

        system,

        component,

        MAV_CMD_NAV_RETURN_TO_LAUNCH

    );

}

bool MavlinkCommand::Loiter(
    UdpClient& udp,
    uint8_t system,
    uint8_t component)
{
    // MAVLink #11 SET_MODE：ArduCopter LOITER custom_mode=5。
    (void)component;

    return SetMode(
        udp,
        system,
        5);
}
