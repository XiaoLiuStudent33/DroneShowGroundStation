/** @file
 *    @brief MAVLink comm protocol testsuite generated from mdhk.xml
 *    @see https://mavlink.io/en/
 */
#pragma once
#ifndef MDHK_TESTSUITE_H
#define MDHK_TESTSUITE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MAVLINK_TEST_ALL
#define MAVLINK_TEST_ALL
static void mavlink_test_ardupilotmega(uint8_t, uint8_t, mavlink_message_t *last_msg);
static void mavlink_test_mdhk(uint8_t, uint8_t, mavlink_message_t *last_msg);

static void mavlink_test_all(uint8_t system_id, uint8_t component_id, mavlink_message_t *last_msg)
{
    mavlink_test_ardupilotmega(system_id, component_id, last_msg);
    mavlink_test_mdhk(system_id, component_id, last_msg);
}
#endif

#include "../ardupilotmega/testsuite.h"


static void mavlink_test_mdhk_status(uint8_t system_id, uint8_t component_id, mavlink_message_t *last_msg)
{
#ifdef MAVLINK_STATUS_FLAG_OUT_MAVLINK1
    mavlink_status_t *status = mavlink_get_channel_status(MAVLINK_COMM_0);
        if ((status->flags & MAVLINK_STATUS_FLAG_OUT_MAVLINK1) && MAVLINK_MSG_ID_MDHK_STATUS >= 256) {
            return;
        }
#endif
    mavlink_message_t msg;
        uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
        uint16_t i;
    mavlink_mdhk_status_t packet_in = {
        963497464,963497672,963497880,963498088,963498296,963498504,963498712,963498920,241.0,269.0,297.0,325.0,19731,{ 19835, 19836, 19837, 19838 },20251,20355,20459,20563,20667,20771,20875,221,32,99,166,233,44,111,178,245,56,123,190,{ 1, 2, 3, 4, 5, 6, 7, 8 },"OPQRSTUVWXYZABCDEFG"
    };
    mavlink_mdhk_status_t packet1, packet2;
        memset(&packet1, 0, sizeof(packet1));
        packet1.time_boot_ms = packet_in.time_boot_ms;
        packet1.valid_flags = packet_in.valid_flags;
        packet1.latitude = packet_in.latitude;
        packet1.longitude = packet_in.longitude;
        packet1.gps_alt = packet_in.gps_alt;
        packet1.relative_alt = packet_in.relative_alt;
        packet1.freemem32 = packet_in.freemem32;
        packet1.flight_sw_version = packet_in.flight_sw_version;
        packet1.wp_distance = packet_in.wp_distance;
        packet1.compass_variance = packet_in.compass_variance;
        packet1.mag_field1 = packet_in.mag_field1;
        packet1.mag_field2 = packet_in.mag_field2;
        packet1.gps_year = packet_in.gps_year;
        packet1.battery_cycle_count = packet_in.battery_cycle_count;
        packet1.cell_voltage_delta = packet_in.cell_voltage_delta;
        packet1.ekf_flags = packet_in.ekf_flags;
        packet1.mission_seq = packet_in.mission_seq;
        packet1.mission_total = packet_in.mission_total;
        packet1.battery_current = packet_in.battery_current;
        packet1.battery_temperature = packet_in.battery_temperature;
        packet1.gps_month = packet_in.gps_month;
        packet1.gps_day = packet_in.gps_day;
        packet1.gps_hour = packet_in.gps_hour;
        packet1.gps_minute = packet_in.gps_minute;
        packet1.gps_second = packet_in.gps_second;
        packet1.battery_remaining = packet_in.battery_remaining;
        packet1.gps_fix_type = packet_in.gps_fix_type;
        packet1.satellites_visible = packet_in.satellites_visible;
        packet1.schema_version = packet_in.schema_version;
        packet1.rgb_red = packet_in.rgb_red;
        packet1.rgb_green = packet_in.rgb_green;
        packet1.rgb_blue = packet_in.rgb_blue;
        
        mav_array_memcpy(packet1.cell_voltages, packet_in.cell_voltages, sizeof(uint16_t)*4);
        mav_array_memcpy(packet1.flight_custom_version, packet_in.flight_custom_version, sizeof(uint8_t)*8);
        mav_array_memcpy(packet1.hardware_id, packet_in.hardware_id, sizeof(char)*20);
        
#ifdef MAVLINK_STATUS_FLAG_OUT_MAVLINK1
        if (status->flags & MAVLINK_STATUS_FLAG_OUT_MAVLINK1) {
           // cope with extensions
           memset(MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN + (char *)&packet1, 0, sizeof(packet1)-MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN);
        }
#endif
        memset(&packet2, 0, sizeof(packet2));
    mavlink_msg_mdhk_status_encode(system_id, component_id, &msg, &packet1);
    mavlink_msg_mdhk_status_decode(&msg, &packet2);
        MAVLINK_ASSERT(memcmp(&packet1, &packet2, sizeof(packet1)) == 0);

        memset(&packet2, 0, sizeof(packet2));
    mavlink_msg_mdhk_status_pack(system_id, component_id, &msg , packet1.time_boot_ms , packet1.gps_year , packet1.gps_month , packet1.gps_day , packet1.gps_hour , packet1.gps_minute , packet1.gps_second , packet1.valid_flags , packet1.latitude , packet1.longitude , packet1.gps_alt , packet1.relative_alt , packet1.freemem32 , packet1.flight_sw_version , packet1.wp_distance , packet1.compass_variance , packet1.mag_field1 , packet1.mag_field2 , packet1.cell_voltages , packet1.battery_cycle_count , packet1.cell_voltage_delta , packet1.ekf_flags , packet1.mission_seq , packet1.mission_total , packet1.battery_current , packet1.battery_temperature , packet1.battery_remaining , packet1.gps_fix_type , packet1.satellites_visible , packet1.schema_version , packet1.rgb_red , packet1.rgb_green , packet1.rgb_blue , packet1.flight_custom_version , packet1.hardware_id );
    mavlink_msg_mdhk_status_decode(&msg, &packet2);
        MAVLINK_ASSERT(memcmp(&packet1, &packet2, sizeof(packet1)) == 0);

        memset(&packet2, 0, sizeof(packet2));
    mavlink_msg_mdhk_status_pack_chan(system_id, component_id, MAVLINK_COMM_0, &msg , packet1.time_boot_ms , packet1.gps_year , packet1.gps_month , packet1.gps_day , packet1.gps_hour , packet1.gps_minute , packet1.gps_second , packet1.valid_flags , packet1.latitude , packet1.longitude , packet1.gps_alt , packet1.relative_alt , packet1.freemem32 , packet1.flight_sw_version , packet1.wp_distance , packet1.compass_variance , packet1.mag_field1 , packet1.mag_field2 , packet1.cell_voltages , packet1.battery_cycle_count , packet1.cell_voltage_delta , packet1.ekf_flags , packet1.mission_seq , packet1.mission_total , packet1.battery_current , packet1.battery_temperature , packet1.battery_remaining , packet1.gps_fix_type , packet1.satellites_visible , packet1.schema_version , packet1.rgb_red , packet1.rgb_green , packet1.rgb_blue , packet1.flight_custom_version , packet1.hardware_id );
    mavlink_msg_mdhk_status_decode(&msg, &packet2);
        MAVLINK_ASSERT(memcmp(&packet1, &packet2, sizeof(packet1)) == 0);

        memset(&packet2, 0, sizeof(packet2));
        mavlink_msg_to_send_buffer(buffer, &msg);
        for (i=0; i<mavlink_msg_get_send_buffer_length(&msg); i++) {
            comm_send_ch(MAVLINK_COMM_0, buffer[i]);
        }
    mavlink_msg_mdhk_status_decode(last_msg, &packet2);
        MAVLINK_ASSERT(memcmp(&packet1, &packet2, sizeof(packet1)) == 0);
        
        memset(&packet2, 0, sizeof(packet2));
    mavlink_msg_mdhk_status_send(MAVLINK_COMM_1 , packet1.time_boot_ms , packet1.gps_year , packet1.gps_month , packet1.gps_day , packet1.gps_hour , packet1.gps_minute , packet1.gps_second , packet1.valid_flags , packet1.latitude , packet1.longitude , packet1.gps_alt , packet1.relative_alt , packet1.freemem32 , packet1.flight_sw_version , packet1.wp_distance , packet1.compass_variance , packet1.mag_field1 , packet1.mag_field2 , packet1.cell_voltages , packet1.battery_cycle_count , packet1.cell_voltage_delta , packet1.ekf_flags , packet1.mission_seq , packet1.mission_total , packet1.battery_current , packet1.battery_temperature , packet1.battery_remaining , packet1.gps_fix_type , packet1.satellites_visible , packet1.schema_version , packet1.rgb_red , packet1.rgb_green , packet1.rgb_blue , packet1.flight_custom_version , packet1.hardware_id );
    mavlink_msg_mdhk_status_decode(last_msg, &packet2);
        MAVLINK_ASSERT(memcmp(&packet1, &packet2, sizeof(packet1)) == 0);

#ifdef MAVLINK_HAVE_GET_MESSAGE_INFO
    MAVLINK_ASSERT(mavlink_get_message_info_by_name("MDHK_STATUS") != NULL);
    MAVLINK_ASSERT(mavlink_get_message_info_by_id(MAVLINK_MSG_ID_MDHK_STATUS) != NULL);
#endif
}

static void mavlink_test_mdhk(uint8_t system_id, uint8_t component_id, mavlink_message_t *last_msg)
{
    mavlink_test_mdhk_status(system_id, component_id, last_msg);
}

#ifdef __cplusplus
}
#endif // __cplusplus
#endif // MDHK_TESTSUITE_H
