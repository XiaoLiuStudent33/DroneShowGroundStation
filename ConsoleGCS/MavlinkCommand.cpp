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
    (void)component;

    return SetMode(
        udp,
        system,
        5);
}
