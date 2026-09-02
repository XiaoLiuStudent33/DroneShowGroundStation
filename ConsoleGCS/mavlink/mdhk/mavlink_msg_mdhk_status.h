#pragma once
// MESSAGE MDHK_STATUS PACKING

#define MAVLINK_MSG_ID_MDHK_STATUS 187


typedef struct __mavlink_mdhk_status_t {
 uint32_t time_boot_ms; /*< [ms] Timestamp since system boot.*/
 uint32_t valid_flags; /*<  Validity bitmap for the grouped values in this packet. The vendor enum is preserved as a raw value until its bit definitions are supplied.*/
 int32_t latitude; /*< [degE7] Raw GPS latitude.*/
 int32_t longitude; /*< [degE7] Raw GPS longitude.*/
 int32_t gps_alt; /*< [mm] Raw GPS altitude above mean sea level.*/
 int32_t relative_alt; /*< [mm] Estimated altitude relative to HOME, positive upward.*/
 uint32_t freemem32; /*< [bytes] Available system memory.*/
 uint32_t flight_sw_version; /*<  Firmware version encoded as in AUTOPILOT_VERSION.flight_sw_version.*/
 float wp_distance; /*< [m] Horizontal distance from the aircraft to the current navigation waypoint.*/
 float compass_variance; /*<  Maximum EKF compass variance.*/
 float mag_field1; /*< [mgauss] Magnitude of compass instance 1 field vector.*/
 float mag_field2; /*< [mgauss] Magnitude of compass instance 2 field vector.*/
 uint16_t gps_year; /*<  GPS UTC Gregorian year. Zero means invalid.*/
 uint16_t cell_voltages[4]; /*< [mV] Primary battery cell voltages 1 through 4. UINT16_MAX means invalid.*/
 uint16_t battery_cycle_count; /*<  Primary smart-battery cycle count. UINT16_MAX means invalid.*/
 uint16_t cell_voltage_delta; /*< [mV] Difference between the highest and lowest valid primary-battery cell voltage. UINT16_MAX means invalid.*/
 uint16_t ekf_flags; /*<  Flags encoded as in EKF_STATUS_REPORT.flags.*/
 uint16_t mission_seq; /*<  Current mission sequence number.*/
 uint16_t mission_total; /*<  Total mission item count excluding HOME.*/
 int16_t battery_current; /*< [cA] Primary battery current. -1 means invalid.*/
 int16_t battery_temperature; /*< [cdegC] Primary battery temperature. INT16_MAX means invalid.*/
 uint8_t gps_month; /*<  GPS UTC month, 1 through 12. Zero means invalid.*/
 uint8_t gps_day; /*<  GPS UTC day, 1 through 31. Zero means invalid.*/
 uint8_t gps_hour; /*<  GPS UTC hour in 24-hour format, 0 through 23.*/
 uint8_t gps_minute; /*<  GPS UTC minute, 0 through 59.*/
 uint8_t gps_second; /*<  GPS UTC second, 0 through 60.*/
 int8_t battery_remaining; /*< [%] Primary battery remaining percentage. -1 means invalid.*/
 uint8_t gps_fix_type; /*<  Raw GPS fix type.*/
 uint8_t satellites_visible; /*<  Number of satellites visible. 255 means invalid.*/
 uint8_t schema_version; /*<  MDHK_STATUS payload schema version.*/
 uint8_t rgb_red; /*<  Final logical red LED output, 0 through 255.*/
 uint8_t rgb_green; /*<  Final logical green LED output, 0 through 255.*/
 uint8_t rgb_blue; /*<  Final logical blue LED output, 0 through 255.*/
 uint8_t flight_custom_version[8]; /*<  Firmware Git/custom version bytes.*/
 char hardware_id[20]; /*<  Twenty-character Base32 STM32 hardware identifier.*/
} mavlink_mdhk_status_t;

#define MAVLINK_MSG_ID_MDHK_STATUS_LEN 112
#define MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN 112
#define MAVLINK_MSG_ID_187_LEN 112
#define MAVLINK_MSG_ID_187_MIN_LEN 112

#define MAVLINK_MSG_ID_MDHK_STATUS_CRC 83
#define MAVLINK_MSG_ID_187_CRC 83

#define MAVLINK_MSG_MDHK_STATUS_FIELD_CELL_VOLTAGES_LEN 4
#define MAVLINK_MSG_MDHK_STATUS_FIELD_FLIGHT_CUSTOM_VERSION_LEN 8
#define MAVLINK_MSG_MDHK_STATUS_FIELD_HARDWARE_ID_LEN 20

#if MAVLINK_COMMAND_24BIT
#define MAVLINK_MESSAGE_INFO_MDHK_STATUS { \
    187, \
    "MDHK_STATUS", \
    35, \
    {  { "time_boot_ms", NULL, MAVLINK_TYPE_UINT32_T, 0, 0, offsetof(mavlink_mdhk_status_t, time_boot_ms) }, \
         { "gps_year", NULL, MAVLINK_TYPE_UINT16_T, 0, 48, offsetof(mavlink_mdhk_status_t, gps_year) }, \
         { "gps_month", NULL, MAVLINK_TYPE_UINT8_T, 0, 72, offsetof(mavlink_mdhk_status_t, gps_month) }, \
         { "gps_day", NULL, MAVLINK_TYPE_UINT8_T, 0, 73, offsetof(mavlink_mdhk_status_t, gps_day) }, \
         { "gps_hour", NULL, MAVLINK_TYPE_UINT8_T, 0, 74, offsetof(mavlink_mdhk_status_t, gps_hour) }, \
         { "gps_minute", NULL, MAVLINK_TYPE_UINT8_T, 0, 75, offsetof(mavlink_mdhk_status_t, gps_minute) }, \
         { "gps_second", NULL, MAVLINK_TYPE_UINT8_T, 0, 76, offsetof(mavlink_mdhk_status_t, gps_second) }, \
         { "valid_flags", NULL, MAVLINK_TYPE_UINT32_T, 0, 4, offsetof(mavlink_mdhk_status_t, valid_flags) }, \
         { "latitude", NULL, MAVLINK_TYPE_INT32_T, 0, 8, offsetof(mavlink_mdhk_status_t, latitude) }, \
         { "longitude", NULL, MAVLINK_TYPE_INT32_T, 0, 12, offsetof(mavlink_mdhk_status_t, longitude) }, \
         { "gps_alt", NULL, MAVLINK_TYPE_INT32_T, 0, 16, offsetof(mavlink_mdhk_status_t, gps_alt) }, \
         { "relative_alt", NULL, MAVLINK_TYPE_INT32_T, 0, 20, offsetof(mavlink_mdhk_status_t, relative_alt) }, \
         { "freemem32", NULL, MAVLINK_TYPE_UINT32_T, 0, 24, offsetof(mavlink_mdhk_status_t, freemem32) }, \
         { "flight_sw_version", NULL, MAVLINK_TYPE_UINT32_T, 0, 28, offsetof(mavlink_mdhk_status_t, flight_sw_version) }, \
         { "wp_distance", NULL, MAVLINK_TYPE_FLOAT, 0, 32, offsetof(mavlink_mdhk_status_t, wp_distance) }, \
         { "compass_variance", NULL, MAVLINK_TYPE_FLOAT, 0, 36, offsetof(mavlink_mdhk_status_t, compass_variance) }, \
         { "mag_field1", NULL, MAVLINK_TYPE_FLOAT, 0, 40, offsetof(mavlink_mdhk_status_t, mag_field1) }, \
         { "mag_field2", NULL, MAVLINK_TYPE_FLOAT, 0, 44, offsetof(mavlink_mdhk_status_t, mag_field2) }, \
         { "cell_voltages", NULL, MAVLINK_TYPE_UINT16_T, 4, 50, offsetof(mavlink_mdhk_status_t, cell_voltages) }, \
         { "battery_cycle_count", NULL, MAVLINK_TYPE_UINT16_T, 0, 58, offsetof(mavlink_mdhk_status_t, battery_cycle_count) }, \
         { "cell_voltage_delta", NULL, MAVLINK_TYPE_UINT16_T, 0, 60, offsetof(mavlink_mdhk_status_t, cell_voltage_delta) }, \
         { "ekf_flags", NULL, MAVLINK_TYPE_UINT16_T, 0, 62, offsetof(mavlink_mdhk_status_t, ekf_flags) }, \
         { "mission_seq", NULL, MAVLINK_TYPE_UINT16_T, 0, 64, offsetof(mavlink_mdhk_status_t, mission_seq) }, \
         { "mission_total", NULL, MAVLINK_TYPE_UINT16_T, 0, 66, offsetof(mavlink_mdhk_status_t, mission_total) }, \
         { "battery_current", NULL, MAVLINK_TYPE_INT16_T, 0, 68, offsetof(mavlink_mdhk_status_t, battery_current) }, \
         { "battery_temperature", NULL, MAVLINK_TYPE_INT16_T, 0, 70, offsetof(mavlink_mdhk_status_t, battery_temperature) }, \
         { "battery_remaining", NULL, MAVLINK_TYPE_INT8_T, 0, 77, offsetof(mavlink_mdhk_status_t, battery_remaining) }, \
         { "gps_fix_type", NULL, MAVLINK_TYPE_UINT8_T, 0, 78, offsetof(mavlink_mdhk_status_t, gps_fix_type) }, \
         { "satellites_visible", NULL, MAVLINK_TYPE_UINT8_T, 0, 79, offsetof(mavlink_mdhk_status_t, satellites_visible) }, \
         { "schema_version", NULL, MAVLINK_TYPE_UINT8_T, 0, 80, offsetof(mavlink_mdhk_status_t, schema_version) }, \
         { "rgb_red", NULL, MAVLINK_TYPE_UINT8_T, 0, 81, offsetof(mavlink_mdhk_status_t, rgb_red) }, \
         { "rgb_green", NULL, MAVLINK_TYPE_UINT8_T, 0, 82, offsetof(mavlink_mdhk_status_t, rgb_green) }, \
         { "rgb_blue", NULL, MAVLINK_TYPE_UINT8_T, 0, 83, offsetof(mavlink_mdhk_status_t, rgb_blue) }, \
         { "flight_custom_version", NULL, MAVLINK_TYPE_UINT8_T, 8, 84, offsetof(mavlink_mdhk_status_t, flight_custom_version) }, \
         { "hardware_id", NULL, MAVLINK_TYPE_CHAR, 20, 92, offsetof(mavlink_mdhk_status_t, hardware_id) }, \
         } \
}
#else
#define MAVLINK_MESSAGE_INFO_MDHK_STATUS { \
    "MDHK_STATUS", \
    35, \
    {  { "time_boot_ms", NULL, MAVLINK_TYPE_UINT32_T, 0, 0, offsetof(mavlink_mdhk_status_t, time_boot_ms) }, \
         { "gps_year", NULL, MAVLINK_TYPE_UINT16_T, 0, 48, offsetof(mavlink_mdhk_status_t, gps_year) }, \
         { "gps_month", NULL, MAVLINK_TYPE_UINT8_T, 0, 72, offsetof(mavlink_mdhk_status_t, gps_month) }, \
         { "gps_day", NULL, MAVLINK_TYPE_UINT8_T, 0, 73, offsetof(mavlink_mdhk_status_t, gps_day) }, \
         { "gps_hour", NULL, MAVLINK_TYPE_UINT8_T, 0, 74, offsetof(mavlink_mdhk_status_t, gps_hour) }, \
         { "gps_minute", NULL, MAVLINK_TYPE_UINT8_T, 0, 75, offsetof(mavlink_mdhk_status_t, gps_minute) }, \
         { "gps_second", NULL, MAVLINK_TYPE_UINT8_T, 0, 76, offsetof(mavlink_mdhk_status_t, gps_second) }, \
         { "valid_flags", NULL, MAVLINK_TYPE_UINT32_T, 0, 4, offsetof(mavlink_mdhk_status_t, valid_flags) }, \
         { "latitude", NULL, MAVLINK_TYPE_INT32_T, 0, 8, offsetof(mavlink_mdhk_status_t, latitude) }, \
         { "longitude", NULL, MAVLINK_TYPE_INT32_T, 0, 12, offsetof(mavlink_mdhk_status_t, longitude) }, \
         { "gps_alt", NULL, MAVLINK_TYPE_INT32_T, 0, 16, offsetof(mavlink_mdhk_status_t, gps_alt) }, \
         { "relative_alt", NULL, MAVLINK_TYPE_INT32_T, 0, 20, offsetof(mavlink_mdhk_status_t, relative_alt) }, \
         { "freemem32", NULL, MAVLINK_TYPE_UINT32_T, 0, 24, offsetof(mavlink_mdhk_status_t, freemem32) }, \
         { "flight_sw_version", NULL, MAVLINK_TYPE_UINT32_T, 0, 28, offsetof(mavlink_mdhk_status_t, flight_sw_version) }, \
         { "wp_distance", NULL, MAVLINK_TYPE_FLOAT, 0, 32, offsetof(mavlink_mdhk_status_t, wp_distance) }, \
         { "compass_variance", NULL, MAVLINK_TYPE_FLOAT, 0, 36, offsetof(mavlink_mdhk_status_t, compass_variance) }, \
         { "mag_field1", NULL, MAVLINK_TYPE_FLOAT, 0, 40, offsetof(mavlink_mdhk_status_t, mag_field1) }, \
         { "mag_field2", NULL, MAVLINK_TYPE_FLOAT, 0, 44, offsetof(mavlink_mdhk_status_t, mag_field2) }, \
         { "cell_voltages", NULL, MAVLINK_TYPE_UINT16_T, 4, 50, offsetof(mavlink_mdhk_status_t, cell_voltages) }, \
         { "battery_cycle_count", NULL, MAVLINK_TYPE_UINT16_T, 0, 58, offsetof(mavlink_mdhk_status_t, battery_cycle_count) }, \
         { "cell_voltage_delta", NULL, MAVLINK_TYPE_UINT16_T, 0, 60, offsetof(mavlink_mdhk_status_t, cell_voltage_delta) }, \
         { "ekf_flags", NULL, MAVLINK_TYPE_UINT16_T, 0, 62, offsetof(mavlink_mdhk_status_t, ekf_flags) }, \
         { "mission_seq", NULL, MAVLINK_TYPE_UINT16_T, 0, 64, offsetof(mavlink_mdhk_status_t, mission_seq) }, \
         { "mission_total", NULL, MAVLINK_TYPE_UINT16_T, 0, 66, offsetof(mavlink_mdhk_status_t, mission_total) }, \
         { "battery_current", NULL, MAVLINK_TYPE_INT16_T, 0, 68, offsetof(mavlink_mdhk_status_t, battery_current) }, \
         { "battery_temperature", NULL, MAVLINK_TYPE_INT16_T, 0, 70, offsetof(mavlink_mdhk_status_t, battery_temperature) }, \
         { "battery_remaining", NULL, MAVLINK_TYPE_INT8_T, 0, 77, offsetof(mavlink_mdhk_status_t, battery_remaining) }, \
         { "gps_fix_type", NULL, MAVLINK_TYPE_UINT8_T, 0, 78, offsetof(mavlink_mdhk_status_t, gps_fix_type) }, \
         { "satellites_visible", NULL, MAVLINK_TYPE_UINT8_T, 0, 79, offsetof(mavlink_mdhk_status_t, satellites_visible) }, \
         { "schema_version", NULL, MAVLINK_TYPE_UINT8_T, 0, 80, offsetof(mavlink_mdhk_status_t, schema_version) }, \
         { "rgb_red", NULL, MAVLINK_TYPE_UINT8_T, 0, 81, offsetof(mavlink_mdhk_status_t, rgb_red) }, \
         { "rgb_green", NULL, MAVLINK_TYPE_UINT8_T, 0, 82, offsetof(mavlink_mdhk_status_t, rgb_green) }, \
         { "rgb_blue", NULL, MAVLINK_TYPE_UINT8_T, 0, 83, offsetof(mavlink_mdhk_status_t, rgb_blue) }, \
         { "flight_custom_version", NULL, MAVLINK_TYPE_UINT8_T, 8, 84, offsetof(mavlink_mdhk_status_t, flight_custom_version) }, \
         { "hardware_id", NULL, MAVLINK_TYPE_CHAR, 20, 92, offsetof(mavlink_mdhk_status_t, hardware_id) }, \
         } \
}
#endif

/**
 * @brief Pack a mdhk_status message
 * @param system_id ID of this system
 * @param component_id ID of this component (e.g. 200 for IMU)
 * @param msg The MAVLink message to compress the data into
 *
 * @param time_boot_ms [ms] Timestamp since system boot.
 * @param gps_year  GPS UTC Gregorian year. Zero means invalid.
 * @param gps_month  GPS UTC month, 1 through 12. Zero means invalid.
 * @param gps_day  GPS UTC day, 1 through 31. Zero means invalid.
 * @param gps_hour  GPS UTC hour in 24-hour format, 0 through 23.
 * @param gps_minute  GPS UTC minute, 0 through 59.
 * @param gps_second  GPS UTC second, 0 through 60.
 * @param valid_flags  Validity bitmap for the grouped values in this packet. The vendor enum is preserved as a raw value until its bit definitions are supplied.
 * @param latitude [degE7] Raw GPS latitude.
 * @param longitude [degE7] Raw GPS longitude.
 * @param gps_alt [mm] Raw GPS altitude above mean sea level.
 * @param relative_alt [mm] Estimated altitude relative to HOME, positive upward.
 * @param freemem32 [bytes] Available system memory.
 * @param flight_sw_version  Firmware version encoded as in AUTOPILOT_VERSION.flight_sw_version.
 * @param wp_distance [m] Horizontal distance from the aircraft to the current navigation waypoint.
 * @param compass_variance  Maximum EKF compass variance.
 * @param mag_field1 [mgauss] Magnitude of compass instance 1 field vector.
 * @param mag_field2 [mgauss] Magnitude of compass instance 2 field vector.
 * @param cell_voltages [mV] Primary battery cell voltages 1 through 4. UINT16_MAX means invalid.
 * @param battery_cycle_count  Primary smart-battery cycle count. UINT16_MAX means invalid.
 * @param cell_voltage_delta [mV] Difference between the highest and lowest valid primary-battery cell voltage. UINT16_MAX means invalid.
 * @param ekf_flags  Flags encoded as in EKF_STATUS_REPORT.flags.
 * @param mission_seq  Current mission sequence number.
 * @param mission_total  Total mission item count excluding HOME.
 * @param battery_current [cA] Primary battery current. -1 means invalid.
 * @param battery_temperature [cdegC] Primary battery temperature. INT16_MAX means invalid.
 * @param battery_remaining [%] Primary battery remaining percentage. -1 means invalid.
 * @param gps_fix_type  Raw GPS fix type.
 * @param satellites_visible  Number of satellites visible. 255 means invalid.
 * @param schema_version  MDHK_STATUS payload schema version.
 * @param rgb_red  Final logical red LED output, 0 through 255.
 * @param rgb_green  Final logical green LED output, 0 through 255.
 * @param rgb_blue  Final logical blue LED output, 0 through 255.
 * @param flight_custom_version  Firmware Git/custom version bytes.
 * @param hardware_id  Twenty-character Base32 STM32 hardware identifier.
 * @return length of the message in bytes (excluding serial stream start sign)
 */
static inline uint16_t mavlink_msg_mdhk_status_pack(uint8_t system_id, uint8_t component_id, mavlink_message_t* msg,
                               uint32_t time_boot_ms, uint16_t gps_year, uint8_t gps_month, uint8_t gps_day, uint8_t gps_hour, uint8_t gps_minute, uint8_t gps_second, uint32_t valid_flags, int32_t latitude, int32_t longitude, int32_t gps_alt, int32_t relative_alt, uint32_t freemem32, uint32_t flight_sw_version, float wp_distance, float compass_variance, float mag_field1, float mag_field2, const uint16_t *cell_voltages, uint16_t battery_cycle_count, uint16_t cell_voltage_delta, uint16_t ekf_flags, uint16_t mission_seq, uint16_t mission_total, int16_t battery_current, int16_t battery_temperature, int8_t battery_remaining, uint8_t gps_fix_type, uint8_t satellites_visible, uint8_t schema_version, uint8_t rgb_red, uint8_t rgb_green, uint8_t rgb_blue, const uint8_t *flight_custom_version, const char *hardware_id)
{
#if MAVLINK_NEED_BYTE_SWAP || !MAVLINK_ALIGNED_FIELDS
    char buf[MAVLINK_MSG_ID_MDHK_STATUS_LEN];
    _mav_put_uint32_t(buf, 0, time_boot_ms);
    _mav_put_uint32_t(buf, 4, valid_flags);
    _mav_put_int32_t(buf, 8, latitude);
    _mav_put_int32_t(buf, 12, longitude);
    _mav_put_int32_t(buf, 16, gps_alt);
    _mav_put_int32_t(buf, 20, relative_alt);
    _mav_put_uint32_t(buf, 24, freemem32);
    _mav_put_uint32_t(buf, 28, flight_sw_version);
    _mav_put_float(buf, 32, wp_distance);
    _mav_put_float(buf, 36, compass_variance);
    _mav_put_float(buf, 40, mag_field1);
    _mav_put_float(buf, 44, mag_field2);
    _mav_put_uint16_t(buf, 48, gps_year);
    _mav_put_uint16_t(buf, 58, battery_cycle_count);
    _mav_put_uint16_t(buf, 60, cell_voltage_delta);
    _mav_put_uint16_t(buf, 62, ekf_flags);
    _mav_put_uint16_t(buf, 64, mission_seq);
    _mav_put_uint16_t(buf, 66, mission_total);
    _mav_put_int16_t(buf, 68, battery_current);
    _mav_put_int16_t(buf, 70, battery_temperature);
    _mav_put_uint8_t(buf, 72, gps_month);
    _mav_put_uint8_t(buf, 73, gps_day);
    _mav_put_uint8_t(buf, 74, gps_hour);
    _mav_put_uint8_t(buf, 75, gps_minute);
    _mav_put_uint8_t(buf, 76, gps_second);
    _mav_put_int8_t(buf, 77, battery_remaining);
    _mav_put_uint8_t(buf, 78, gps_fix_type);
    _mav_put_uint8_t(buf, 79, satellites_visible);
    _mav_put_uint8_t(buf, 80, schema_version);
    _mav_put_uint8_t(buf, 81, rgb_red);
    _mav_put_uint8_t(buf, 82, rgb_green);
    _mav_put_uint8_t(buf, 83, rgb_blue);
    _mav_put_uint16_t_array(buf, 50, cell_voltages, 4);
    _mav_put_uint8_t_array(buf, 84, flight_custom_version, 8);
    _mav_put_char_array(buf, 92, hardware_id, 20);
        memcpy(_MAV_PAYLOAD_NON_CONST(msg), buf, MAVLINK_MSG_ID_MDHK_STATUS_LEN);
#else
    mavlink_mdhk_status_t packet;
    packet.time_boot_ms = time_boot_ms;
    packet.valid_flags = valid_flags;
    packet.latitude = latitude;
    packet.longitude = longitude;
    packet.gps_alt = gps_alt;
    packet.relative_alt = relative_alt;
    packet.freemem32 = freemem32;
    packet.flight_sw_version = flight_sw_version;
    packet.wp_distance = wp_distance;
    packet.compass_variance = compass_variance;
    packet.mag_field1 = mag_field1;
    packet.mag_field2 = mag_field2;
    packet.gps_year = gps_year;
    packet.battery_cycle_count = battery_cycle_count;
    packet.cell_voltage_delta = cell_voltage_delta;
    packet.ekf_flags = ekf_flags;
    packet.mission_seq = mission_seq;
    packet.mission_total = mission_total;
    packet.battery_current = battery_current;
    packet.battery_temperature = battery_temperature;
    packet.gps_month = gps_month;
    packet.gps_day = gps_day;
    packet.gps_hour = gps_hour;
    packet.gps_minute = gps_minute;
    packet.gps_second = gps_second;
    packet.battery_remaining = battery_remaining;
    packet.gps_fix_type = gps_fix_type;
    packet.satellites_visible = satellites_visible;
    packet.schema_version = schema_version;
    packet.rgb_red = rgb_red;
    packet.rgb_green = rgb_green;
    packet.rgb_blue = rgb_blue;
    mav_array_memcpy(packet.cell_voltages, cell_voltages, sizeof(uint16_t) * 4);
    mav_array_memcpy(packet.flight_custom_version, flight_custom_version, sizeof(uint8_t) * 8);
    mav_array_memcpy(packet.hardware_id, hardware_id, sizeof(char) * 20);
        memcpy(_MAV_PAYLOAD_NON_CONST(msg), &packet, MAVLINK_MSG_ID_MDHK_STATUS_LEN);
#endif

    msg->msgid = MAVLINK_MSG_ID_MDHK_STATUS;
    return mavlink_finalize_message(msg, system_id, component_id, MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN, MAVLINK_MSG_ID_MDHK_STATUS_LEN, MAVLINK_MSG_ID_MDHK_STATUS_CRC);
}

/**
 * @brief Pack a mdhk_status message
 * @param system_id ID of this system
 * @param component_id ID of this component (e.g. 200 for IMU)
 * @param status MAVLink status structure
 * @param msg The MAVLink message to compress the data into
 *
 * @param time_boot_ms [ms] Timestamp since system boot.
 * @param gps_year  GPS UTC Gregorian year. Zero means invalid.
 * @param gps_month  GPS UTC month, 1 through 12. Zero means invalid.
 * @param gps_day  GPS UTC day, 1 through 31. Zero means invalid.
 * @param gps_hour  GPS UTC hour in 24-hour format, 0 through 23.
 * @param gps_minute  GPS UTC minute, 0 through 59.
 * @param gps_second  GPS UTC second, 0 through 60.
 * @param valid_flags  Validity bitmap for the grouped values in this packet. The vendor enum is preserved as a raw value until its bit definitions are supplied.
 * @param latitude [degE7] Raw GPS latitude.
 * @param longitude [degE7] Raw GPS longitude.
 * @param gps_alt [mm] Raw GPS altitude above mean sea level.
 * @param relative_alt [mm] Estimated altitude relative to HOME, positive upward.
 * @param freemem32 [bytes] Available system memory.
 * @param flight_sw_version  Firmware version encoded as in AUTOPILOT_VERSION.flight_sw_version.
 * @param wp_distance [m] Horizontal distance from the aircraft to the current navigation waypoint.
 * @param compass_variance  Maximum EKF compass variance.
 * @param mag_field1 [mgauss] Magnitude of compass instance 1 field vector.
 * @param mag_field2 [mgauss] Magnitude of compass instance 2 field vector.
 * @param cell_voltages [mV] Primary battery cell voltages 1 through 4. UINT16_MAX means invalid.
 * @param battery_cycle_count  Primary smart-battery cycle count. UINT16_MAX means invalid.
 * @param cell_voltage_delta [mV] Difference between the highest and lowest valid primary-battery cell voltage. UINT16_MAX means invalid.
 * @param ekf_flags  Flags encoded as in EKF_STATUS_REPORT.flags.
 * @param mission_seq  Current mission sequence number.
 * @param mission_total  Total mission item count excluding HOME.
 * @param battery_current [cA] Primary battery current. -1 means invalid.
 * @param battery_temperature [cdegC] Primary battery temperature. INT16_MAX means invalid.
 * @param battery_remaining [%] Primary battery remaining percentage. -1 means invalid.
 * @param gps_fix_type  Raw GPS fix type.
 * @param satellites_visible  Number of satellites visible. 255 means invalid.
 * @param schema_version  MDHK_STATUS payload schema version.
 * @param rgb_red  Final logical red LED output, 0 through 255.
 * @param rgb_green  Final logical green LED output, 0 through 255.
 * @param rgb_blue  Final logical blue LED output, 0 through 255.
 * @param flight_custom_version  Firmware Git/custom version bytes.
 * @param hardware_id  Twenty-character Base32 STM32 hardware identifier.
 * @return length of the message in bytes (excluding serial stream start sign)
 */
static inline uint16_t mavlink_msg_mdhk_status_pack_status(uint8_t system_id, uint8_t component_id, mavlink_status_t *_status, mavlink_message_t* msg,
                               uint32_t time_boot_ms, uint16_t gps_year, uint8_t gps_month, uint8_t gps_day, uint8_t gps_hour, uint8_t gps_minute, uint8_t gps_second, uint32_t valid_flags, int32_t latitude, int32_t longitude, int32_t gps_alt, int32_t relative_alt, uint32_t freemem32, uint32_t flight_sw_version, float wp_distance, float compass_variance, float mag_field1, float mag_field2, const uint16_t *cell_voltages, uint16_t battery_cycle_count, uint16_t cell_voltage_delta, uint16_t ekf_flags, uint16_t mission_seq, uint16_t mission_total, int16_t battery_current, int16_t battery_temperature, int8_t battery_remaining, uint8_t gps_fix_type, uint8_t satellites_visible, uint8_t schema_version, uint8_t rgb_red, uint8_t rgb_green, uint8_t rgb_blue, const uint8_t *flight_custom_version, const char *hardware_id)
{
#if MAVLINK_NEED_BYTE_SWAP || !MAVLINK_ALIGNED_FIELDS
    char buf[MAVLINK_MSG_ID_MDHK_STATUS_LEN];
    _mav_put_uint32_t(buf, 0, time_boot_ms);
    _mav_put_uint32_t(buf, 4, valid_flags);
    _mav_put_int32_t(buf, 8, latitude);
    _mav_put_int32_t(buf, 12, longitude);
    _mav_put_int32_t(buf, 16, gps_alt);
    _mav_put_int32_t(buf, 20, relative_alt);
    _mav_put_uint32_t(buf, 24, freemem32);
    _mav_put_uint32_t(buf, 28, flight_sw_version);
    _mav_put_float(buf, 32, wp_distance);
    _mav_put_float(buf, 36, compass_variance);
    _mav_put_float(buf, 40, mag_field1);
    _mav_put_float(buf, 44, mag_field2);
    _mav_put_uint16_t(buf, 48, gps_year);
    _mav_put_uint16_t(buf, 58, battery_cycle_count);
    _mav_put_uint16_t(buf, 60, cell_voltage_delta);
    _mav_put_uint16_t(buf, 62, ekf_flags);
    _mav_put_uint16_t(buf, 64, mission_seq);
    _mav_put_uint16_t(buf, 66, mission_total);
    _mav_put_int16_t(buf, 68, battery_current);
    _mav_put_int16_t(buf, 70, battery_temperature);
    _mav_put_uint8_t(buf, 72, gps_month);
    _mav_put_uint8_t(buf, 73, gps_day);
    _mav_put_uint8_t(buf, 74, gps_hour);
    _mav_put_uint8_t(buf, 75, gps_minute);
    _mav_put_uint8_t(buf, 76, gps_second);
    _mav_put_int8_t(buf, 77, battery_remaining);
    _mav_put_uint8_t(buf, 78, gps_fix_type);
    _mav_put_uint8_t(buf, 79, satellites_visible);
    _mav_put_uint8_t(buf, 80, schema_version);
    _mav_put_uint8_t(buf, 81, rgb_red);
    _mav_put_uint8_t(buf, 82, rgb_green);
    _mav_put_uint8_t(buf, 83, rgb_blue);
    _mav_put_uint16_t_array(buf, 50, cell_voltages, 4);
    _mav_put_uint8_t_array(buf, 84, flight_custom_version, 8);
    _mav_put_char_array(buf, 92, hardware_id, 20);
        memcpy(_MAV_PAYLOAD_NON_CONST(msg), buf, MAVLINK_MSG_ID_MDHK_STATUS_LEN);
#else
    mavlink_mdhk_status_t packet;
    packet.time_boot_ms = time_boot_ms;
    packet.valid_flags = valid_flags;
    packet.latitude = latitude;
    packet.longitude = longitude;
    packet.gps_alt = gps_alt;
    packet.relative_alt = relative_alt;
    packet.freemem32 = freemem32;
    packet.flight_sw_version = flight_sw_version;
    packet.wp_distance = wp_distance;
    packet.compass_variance = compass_variance;
    packet.mag_field1 = mag_field1;
    packet.mag_field2 = mag_field2;
    packet.gps_year = gps_year;
    packet.battery_cycle_count = battery_cycle_count;
    packet.cell_voltage_delta = cell_voltage_delta;
    packet.ekf_flags = ekf_flags;
    packet.mission_seq = mission_seq;
    packet.mission_total = mission_total;
    packet.battery_current = battery_current;
    packet.battery_temperature = battery_temperature;
    packet.gps_month = gps_month;
    packet.gps_day = gps_day;
    packet.gps_hour = gps_hour;
    packet.gps_minute = gps_minute;
    packet.gps_second = gps_second;
    packet.battery_remaining = battery_remaining;
    packet.gps_fix_type = gps_fix_type;
    packet.satellites_visible = satellites_visible;
    packet.schema_version = schema_version;
    packet.rgb_red = rgb_red;
    packet.rgb_green = rgb_green;
    packet.rgb_blue = rgb_blue;
    mav_array_memcpy(packet.cell_voltages, cell_voltages, sizeof(uint16_t)*4);
    mav_array_memcpy(packet.flight_custom_version, flight_custom_version, sizeof(uint8_t)*8);
    mav_array_memcpy(packet.hardware_id, hardware_id, sizeof(char)*20);
        memcpy(_MAV_PAYLOAD_NON_CONST(msg), &packet, MAVLINK_MSG_ID_MDHK_STATUS_LEN);
#endif

    msg->msgid = MAVLINK_MSG_ID_MDHK_STATUS;
#if MAVLINK_CRC_EXTRA
    return mavlink_finalize_message_buffer(msg, system_id, component_id, _status, MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN, MAVLINK_MSG_ID_MDHK_STATUS_LEN, MAVLINK_MSG_ID_MDHK_STATUS_CRC);
#else
    return mavlink_finalize_message_buffer(msg, system_id, component_id, _status, MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN, MAVLINK_MSG_ID_MDHK_STATUS_LEN);
#endif
}

/**
 * @brief Pack a mdhk_status message on a channel
 * @param system_id ID of this system
 * @param component_id ID of this component (e.g. 200 for IMU)
 * @param chan The MAVLink channel this message will be sent over
 * @param msg The MAVLink message to compress the data into
 * @param time_boot_ms [ms] Timestamp since system boot.
 * @param gps_year  GPS UTC Gregorian year. Zero means invalid.
 * @param gps_month  GPS UTC month, 1 through 12. Zero means invalid.
 * @param gps_day  GPS UTC day, 1 through 31. Zero means invalid.
 * @param gps_hour  GPS UTC hour in 24-hour format, 0 through 23.
 * @param gps_minute  GPS UTC minute, 0 through 59.
 * @param gps_second  GPS UTC second, 0 through 60.
 * @param valid_flags  Validity bitmap for the grouped values in this packet. The vendor enum is preserved as a raw value until its bit definitions are supplied.
 * @param latitude [degE7] Raw GPS latitude.
 * @param longitude [degE7] Raw GPS longitude.
 * @param gps_alt [mm] Raw GPS altitude above mean sea level.
 * @param relative_alt [mm] Estimated altitude relative to HOME, positive upward.
 * @param freemem32 [bytes] Available system memory.
 * @param flight_sw_version  Firmware version encoded as in AUTOPILOT_VERSION.flight_sw_version.
 * @param wp_distance [m] Horizontal distance from the aircraft to the current navigation waypoint.
 * @param compass_variance  Maximum EKF compass variance.
 * @param mag_field1 [mgauss] Magnitude of compass instance 1 field vector.
 * @param mag_field2 [mgauss] Magnitude of compass instance 2 field vector.
 * @param cell_voltages [mV] Primary battery cell voltages 1 through 4. UINT16_MAX means invalid.
 * @param battery_cycle_count  Primary smart-battery cycle count. UINT16_MAX means invalid.
 * @param cell_voltage_delta [mV] Difference between the highest and lowest valid primary-battery cell voltage. UINT16_MAX means invalid.
 * @param ekf_flags  Flags encoded as in EKF_STATUS_REPORT.flags.
 * @param mission_seq  Current mission sequence number.
 * @param mission_total  Total mission item count excluding HOME.
 * @param battery_current [cA] Primary battery current. -1 means invalid.
 * @param battery_temperature [cdegC] Primary battery temperature. INT16_MAX means invalid.
 * @param battery_remaining [%] Primary battery remaining percentage. -1 means invalid.
 * @param gps_fix_type  Raw GPS fix type.
 * @param satellites_visible  Number of satellites visible. 255 means invalid.
 * @param schema_version  MDHK_STATUS payload schema version.
 * @param rgb_red  Final logical red LED output, 0 through 255.
 * @param rgb_green  Final logical green LED output, 0 through 255.
 * @param rgb_blue  Final logical blue LED output, 0 through 255.
 * @param flight_custom_version  Firmware Git/custom version bytes.
 * @param hardware_id  Twenty-character Base32 STM32 hardware identifier.
 * @return length of the message in bytes (excluding serial stream start sign)
 */
static inline uint16_t mavlink_msg_mdhk_status_pack_chan(uint8_t system_id, uint8_t component_id, uint8_t chan,
                               mavlink_message_t* msg,
                                   uint32_t time_boot_ms,uint16_t gps_year,uint8_t gps_month,uint8_t gps_day,uint8_t gps_hour,uint8_t gps_minute,uint8_t gps_second,uint32_t valid_flags,int32_t latitude,int32_t longitude,int32_t gps_alt,int32_t relative_alt,uint32_t freemem32,uint32_t flight_sw_version,float wp_distance,float compass_variance,float mag_field1,float mag_field2,const uint16_t *cell_voltages,uint16_t battery_cycle_count,uint16_t cell_voltage_delta,uint16_t ekf_flags,uint16_t mission_seq,uint16_t mission_total,int16_t battery_current,int16_t battery_temperature,int8_t battery_remaining,uint8_t gps_fix_type,uint8_t satellites_visible,uint8_t schema_version,uint8_t rgb_red,uint8_t rgb_green,uint8_t rgb_blue,const uint8_t *flight_custom_version,const char *hardware_id)
{
#if MAVLINK_NEED_BYTE_SWAP || !MAVLINK_ALIGNED_FIELDS
    char buf[MAVLINK_MSG_ID_MDHK_STATUS_LEN];
    _mav_put_uint32_t(buf, 0, time_boot_ms);
    _mav_put_uint32_t(buf, 4, valid_flags);
    _mav_put_int32_t(buf, 8, latitude);
    _mav_put_int32_t(buf, 12, longitude);
    _mav_put_int32_t(buf, 16, gps_alt);
    _mav_put_int32_t(buf, 20, relative_alt);
    _mav_put_uint32_t(buf, 24, freemem32);
    _mav_put_uint32_t(buf, 28, flight_sw_version);
    _mav_put_float(buf, 32, wp_distance);
    _mav_put_float(buf, 36, compass_variance);
    _mav_put_float(buf, 40, mag_field1);
    _mav_put_float(buf, 44, mag_field2);
    _mav_put_uint16_t(buf, 48, gps_year);
    _mav_put_uint16_t(buf, 58, battery_cycle_count);
    _mav_put_uint16_t(buf, 60, cell_voltage_delta);
    _mav_put_uint16_t(buf, 62, ekf_flags);
    _mav_put_uint16_t(buf, 64, mission_seq);
    _mav_put_uint16_t(buf, 66, mission_total);
    _mav_put_int16_t(buf, 68, battery_current);
    _mav_put_int16_t(buf, 70, battery_temperature);
    _mav_put_uint8_t(buf, 72, gps_month);
    _mav_put_uint8_t(buf, 73, gps_day);
    _mav_put_uint8_t(buf, 74, gps_hour);
    _mav_put_uint8_t(buf, 75, gps_minute);
    _mav_put_uint8_t(buf, 76, gps_second);
    _mav_put_int8_t(buf, 77, battery_remaining);
    _mav_put_uint8_t(buf, 78, gps_fix_type);
    _mav_put_uint8_t(buf, 79, satellites_visible);
    _mav_put_uint8_t(buf, 80, schema_version);
    _mav_put_uint8_t(buf, 81, rgb_red);
    _mav_put_uint8_t(buf, 82, rgb_green);
    _mav_put_uint8_t(buf, 83, rgb_blue);
    _mav_put_uint16_t_array(buf, 50, cell_voltages, 4);
    _mav_put_uint8_t_array(buf, 84, flight_custom_version, 8);
    _mav_put_char_array(buf, 92, hardware_id, 20);
        memcpy(_MAV_PAYLOAD_NON_CONST(msg), buf, MAVLINK_MSG_ID_MDHK_STATUS_LEN);
#else
    mavlink_mdhk_status_t packet;
    packet.time_boot_ms = time_boot_ms;
    packet.valid_flags = valid_flags;
    packet.latitude = latitude;
    packet.longitude = longitude;
    packet.gps_alt = gps_alt;
    packet.relative_alt = relative_alt;
    packet.freemem32 = freemem32;
    packet.flight_sw_version = flight_sw_version;
    packet.wp_distance = wp_distance;
    packet.compass_variance = compass_variance;
    packet.mag_field1 = mag_field1;
    packet.mag_field2 = mag_field2;
    packet.gps_year = gps_year;
    packet.battery_cycle_count = battery_cycle_count;
    packet.cell_voltage_delta = cell_voltage_delta;
    packet.ekf_flags = ekf_flags;
    packet.mission_seq = mission_seq;
    packet.mission_total = mission_total;
    packet.battery_current = battery_current;
    packet.battery_temperature = battery_temperature;
    packet.gps_month = gps_month;
    packet.gps_day = gps_day;
    packet.gps_hour = gps_hour;
    packet.gps_minute = gps_minute;
    packet.gps_second = gps_second;
    packet.battery_remaining = battery_remaining;
    packet.gps_fix_type = gps_fix_type;
    packet.satellites_visible = satellites_visible;
    packet.schema_version = schema_version;
    packet.rgb_red = rgb_red;
    packet.rgb_green = rgb_green;
    packet.rgb_blue = rgb_blue;
    mav_array_memcpy(packet.cell_voltages, cell_voltages, sizeof(uint16_t) * 4);
    mav_array_memcpy(packet.flight_custom_version, flight_custom_version, sizeof(uint8_t) * 8);
    mav_array_memcpy(packet.hardware_id, hardware_id, sizeof(char) * 20);
        memcpy(_MAV_PAYLOAD_NON_CONST(msg), &packet, MAVLINK_MSG_ID_MDHK_STATUS_LEN);
#endif

    msg->msgid = MAVLINK_MSG_ID_MDHK_STATUS;
    return mavlink_finalize_message_chan(msg, system_id, component_id, chan, MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN, MAVLINK_MSG_ID_MDHK_STATUS_LEN, MAVLINK_MSG_ID_MDHK_STATUS_CRC);
}

/**
 * @brief Encode a mdhk_status struct
 *
 * @param system_id ID of this system
 * @param component_id ID of this component (e.g. 200 for IMU)
 * @param msg The MAVLink message to compress the data into
 * @param mdhk_status C-struct to read the message contents from
 */
static inline uint16_t mavlink_msg_mdhk_status_encode(uint8_t system_id, uint8_t component_id, mavlink_message_t* msg, const mavlink_mdhk_status_t* mdhk_status)
{
    return mavlink_msg_mdhk_status_pack(system_id, component_id, msg, mdhk_status->time_boot_ms, mdhk_status->gps_year, mdhk_status->gps_month, mdhk_status->gps_day, mdhk_status->gps_hour, mdhk_status->gps_minute, mdhk_status->gps_second, mdhk_status->valid_flags, mdhk_status->latitude, mdhk_status->longitude, mdhk_status->gps_alt, mdhk_status->relative_alt, mdhk_status->freemem32, mdhk_status->flight_sw_version, mdhk_status->wp_distance, mdhk_status->compass_variance, mdhk_status->mag_field1, mdhk_status->mag_field2, mdhk_status->cell_voltages, mdhk_status->battery_cycle_count, mdhk_status->cell_voltage_delta, mdhk_status->ekf_flags, mdhk_status->mission_seq, mdhk_status->mission_total, mdhk_status->battery_current, mdhk_status->battery_temperature, mdhk_status->battery_remaining, mdhk_status->gps_fix_type, mdhk_status->satellites_visible, mdhk_status->schema_version, mdhk_status->rgb_red, mdhk_status->rgb_green, mdhk_status->rgb_blue, mdhk_status->flight_custom_version, mdhk_status->hardware_id);
}

/**
 * @brief Encode a mdhk_status struct on a channel
 *
 * @param system_id ID of this system
 * @param component_id ID of this component (e.g. 200 for IMU)
 * @param chan The MAVLink channel this message will be sent over
 * @param msg The MAVLink message to compress the data into
 * @param mdhk_status C-struct to read the message contents from
 */
static inline uint16_t mavlink_msg_mdhk_status_encode_chan(uint8_t system_id, uint8_t component_id, uint8_t chan, mavlink_message_t* msg, const mavlink_mdhk_status_t* mdhk_status)
{
    return mavlink_msg_mdhk_status_pack_chan(system_id, component_id, chan, msg, mdhk_status->time_boot_ms, mdhk_status->gps_year, mdhk_status->gps_month, mdhk_status->gps_day, mdhk_status->gps_hour, mdhk_status->gps_minute, mdhk_status->gps_second, mdhk_status->valid_flags, mdhk_status->latitude, mdhk_status->longitude, mdhk_status->gps_alt, mdhk_status->relative_alt, mdhk_status->freemem32, mdhk_status->flight_sw_version, mdhk_status->wp_distance, mdhk_status->compass_variance, mdhk_status->mag_field1, mdhk_status->mag_field2, mdhk_status->cell_voltages, mdhk_status->battery_cycle_count, mdhk_status->cell_voltage_delta, mdhk_status->ekf_flags, mdhk_status->mission_seq, mdhk_status->mission_total, mdhk_status->battery_current, mdhk_status->battery_temperature, mdhk_status->battery_remaining, mdhk_status->gps_fix_type, mdhk_status->satellites_visible, mdhk_status->schema_version, mdhk_status->rgb_red, mdhk_status->rgb_green, mdhk_status->rgb_blue, mdhk_status->flight_custom_version, mdhk_status->hardware_id);
}

/**
 * @brief Encode a mdhk_status struct with provided status structure
 *
 * @param system_id ID of this system
 * @param component_id ID of this component (e.g. 200 for IMU)
 * @param status MAVLink status structure
 * @param msg The MAVLink message to compress the data into
 * @param mdhk_status C-struct to read the message contents from
 */
static inline uint16_t mavlink_msg_mdhk_status_encode_status(uint8_t system_id, uint8_t component_id, mavlink_status_t* _status, mavlink_message_t* msg, const mavlink_mdhk_status_t* mdhk_status)
{
    return mavlink_msg_mdhk_status_pack_status(system_id, component_id, _status, msg,  mdhk_status->time_boot_ms, mdhk_status->gps_year, mdhk_status->gps_month, mdhk_status->gps_day, mdhk_status->gps_hour, mdhk_status->gps_minute, mdhk_status->gps_second, mdhk_status->valid_flags, mdhk_status->latitude, mdhk_status->longitude, mdhk_status->gps_alt, mdhk_status->relative_alt, mdhk_status->freemem32, mdhk_status->flight_sw_version, mdhk_status->wp_distance, mdhk_status->compass_variance, mdhk_status->mag_field1, mdhk_status->mag_field2, mdhk_status->cell_voltages, mdhk_status->battery_cycle_count, mdhk_status->cell_voltage_delta, mdhk_status->ekf_flags, mdhk_status->mission_seq, mdhk_status->mission_total, mdhk_status->battery_current, mdhk_status->battery_temperature, mdhk_status->battery_remaining, mdhk_status->gps_fix_type, mdhk_status->satellites_visible, mdhk_status->schema_version, mdhk_status->rgb_red, mdhk_status->rgb_green, mdhk_status->rgb_blue, mdhk_status->flight_custom_version, mdhk_status->hardware_id);
}

/**
 * @brief Send a mdhk_status message
 * @param chan MAVLink channel to send the message
 *
 * @param time_boot_ms [ms] Timestamp since system boot.
 * @param gps_year  GPS UTC Gregorian year. Zero means invalid.
 * @param gps_month  GPS UTC month, 1 through 12. Zero means invalid.
 * @param gps_day  GPS UTC day, 1 through 31. Zero means invalid.
 * @param gps_hour  GPS UTC hour in 24-hour format, 0 through 23.
 * @param gps_minute  GPS UTC minute, 0 through 59.
 * @param gps_second  GPS UTC second, 0 through 60.
 * @param valid_flags  Validity bitmap for the grouped values in this packet. The vendor enum is preserved as a raw value until its bit definitions are supplied.
 * @param latitude [degE7] Raw GPS latitude.
 * @param longitude [degE7] Raw GPS longitude.
 * @param gps_alt [mm] Raw GPS altitude above mean sea level.
 * @param relative_alt [mm] Estimated altitude relative to HOME, positive upward.
 * @param freemem32 [bytes] Available system memory.
 * @param flight_sw_version  Firmware version encoded as in AUTOPILOT_VERSION.flight_sw_version.
 * @param wp_distance [m] Horizontal distance from the aircraft to the current navigation waypoint.
 * @param compass_variance  Maximum EKF compass variance.
 * @param mag_field1 [mgauss] Magnitude of compass instance 1 field vector.
 * @param mag_field2 [mgauss] Magnitude of compass instance 2 field vector.
 * @param cell_voltages [mV] Primary battery cell voltages 1 through 4. UINT16_MAX means invalid.
 * @param battery_cycle_count  Primary smart-battery cycle count. UINT16_MAX means invalid.
 * @param cell_voltage_delta [mV] Difference between the highest and lowest valid primary-battery cell voltage. UINT16_MAX means invalid.
 * @param ekf_flags  Flags encoded as in EKF_STATUS_REPORT.flags.
 * @param mission_seq  Current mission sequence number.
 * @param mission_total  Total mission item count excluding HOME.
 * @param battery_current [cA] Primary battery current. -1 means invalid.
 * @param battery_temperature [cdegC] Primary battery temperature. INT16_MAX means invalid.
 * @param battery_remaining [%] Primary battery remaining percentage. -1 means invalid.
 * @param gps_fix_type  Raw GPS fix type.
 * @param satellites_visible  Number of satellites visible. 255 means invalid.
 * @param schema_version  MDHK_STATUS payload schema version.
 * @param rgb_red  Final logical red LED output, 0 through 255.
 * @param rgb_green  Final logical green LED output, 0 through 255.
 * @param rgb_blue  Final logical blue LED output, 0 through 255.
 * @param flight_custom_version  Firmware Git/custom version bytes.
 * @param hardware_id  Twenty-character Base32 STM32 hardware identifier.
 */
#ifdef MAVLINK_USE_CONVENIENCE_FUNCTIONS

static inline void mavlink_msg_mdhk_status_send(mavlink_channel_t chan, uint32_t time_boot_ms, uint16_t gps_year, uint8_t gps_month, uint8_t gps_day, uint8_t gps_hour, uint8_t gps_minute, uint8_t gps_second, uint32_t valid_flags, int32_t latitude, int32_t longitude, int32_t gps_alt, int32_t relative_alt, uint32_t freemem32, uint32_t flight_sw_version, float wp_distance, float compass_variance, float mag_field1, float mag_field2, const uint16_t *cell_voltages, uint16_t battery_cycle_count, uint16_t cell_voltage_delta, uint16_t ekf_flags, uint16_t mission_seq, uint16_t mission_total, int16_t battery_current, int16_t battery_temperature, int8_t battery_remaining, uint8_t gps_fix_type, uint8_t satellites_visible, uint8_t schema_version, uint8_t rgb_red, uint8_t rgb_green, uint8_t rgb_blue, const uint8_t *flight_custom_version, const char *hardware_id)
{
#if MAVLINK_NEED_BYTE_SWAP || !MAVLINK_ALIGNED_FIELDS
    char buf[MAVLINK_MSG_ID_MDHK_STATUS_LEN];
    _mav_put_uint32_t(buf, 0, time_boot_ms);
    _mav_put_uint32_t(buf, 4, valid_flags);
    _mav_put_int32_t(buf, 8, latitude);
    _mav_put_int32_t(buf, 12, longitude);
    _mav_put_int32_t(buf, 16, gps_alt);
    _mav_put_int32_t(buf, 20, relative_alt);
    _mav_put_uint32_t(buf, 24, freemem32);
    _mav_put_uint32_t(buf, 28, flight_sw_version);
    _mav_put_float(buf, 32, wp_distance);
    _mav_put_float(buf, 36, compass_variance);
    _mav_put_float(buf, 40, mag_field1);
    _mav_put_float(buf, 44, mag_field2);
    _mav_put_uint16_t(buf, 48, gps_year);
    _mav_put_uint16_t(buf, 58, battery_cycle_count);
    _mav_put_uint16_t(buf, 60, cell_voltage_delta);
    _mav_put_uint16_t(buf, 62, ekf_flags);
    _mav_put_uint16_t(buf, 64, mission_seq);
    _mav_put_uint16_t(buf, 66, mission_total);
    _mav_put_int16_t(buf, 68, battery_current);
    _mav_put_int16_t(buf, 70, battery_temperature);
    _mav_put_uint8_t(buf, 72, gps_month);
    _mav_put_uint8_t(buf, 73, gps_day);
    _mav_put_uint8_t(buf, 74, gps_hour);
    _mav_put_uint8_t(buf, 75, gps_minute);
    _mav_put_uint8_t(buf, 76, gps_second);
    _mav_put_int8_t(buf, 77, battery_remaining);
    _mav_put_uint8_t(buf, 78, gps_fix_type);
    _mav_put_uint8_t(buf, 79, satellites_visible);
    _mav_put_uint8_t(buf, 80, schema_version);
    _mav_put_uint8_t(buf, 81, rgb_red);
    _mav_put_uint8_t(buf, 82, rgb_green);
    _mav_put_uint8_t(buf, 83, rgb_blue);
    _mav_put_uint16_t_array(buf, 50, cell_voltages, 4);
    _mav_put_uint8_t_array(buf, 84, flight_custom_version, 8);
    _mav_put_char_array(buf, 92, hardware_id, 20);
    _mav_finalize_message_chan_send(chan, MAVLINK_MSG_ID_MDHK_STATUS, buf, MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN, MAVLINK_MSG_ID_MDHK_STATUS_LEN, MAVLINK_MSG_ID_MDHK_STATUS_CRC);
#else
    mavlink_mdhk_status_t packet;
    packet.time_boot_ms = time_boot_ms;
    packet.valid_flags = valid_flags;
    packet.latitude = latitude;
    packet.longitude = longitude;
    packet.gps_alt = gps_alt;
    packet.relative_alt = relative_alt;
    packet.freemem32 = freemem32;
    packet.flight_sw_version = flight_sw_version;
    packet.wp_distance = wp_distance;
    packet.compass_variance = compass_variance;
    packet.mag_field1 = mag_field1;
    packet.mag_field2 = mag_field2;
    packet.gps_year = gps_year;
    packet.battery_cycle_count = battery_cycle_count;
    packet.cell_voltage_delta = cell_voltage_delta;
    packet.ekf_flags = ekf_flags;
    packet.mission_seq = mission_seq;
    packet.mission_total = mission_total;
    packet.battery_current = battery_current;
    packet.battery_temperature = battery_temperature;
    packet.gps_month = gps_month;
    packet.gps_day = gps_day;
    packet.gps_hour = gps_hour;
    packet.gps_minute = gps_minute;
    packet.gps_second = gps_second;
    packet.battery_remaining = battery_remaining;
    packet.gps_fix_type = gps_fix_type;
    packet.satellites_visible = satellites_visible;
    packet.schema_version = schema_version;
    packet.rgb_red = rgb_red;
    packet.rgb_green = rgb_green;
    packet.rgb_blue = rgb_blue;
    mav_array_memcpy(packet.cell_voltages, cell_voltages, sizeof(uint16_t) * 4);
    mav_array_memcpy(packet.flight_custom_version, flight_custom_version, sizeof(uint8_t) * 8);
    mav_array_memcpy(packet.hardware_id, hardware_id, sizeof(char) * 20);
    _mav_finalize_message_chan_send(chan, MAVLINK_MSG_ID_MDHK_STATUS, (const char *)&packet, MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN, MAVLINK_MSG_ID_MDHK_STATUS_LEN, MAVLINK_MSG_ID_MDHK_STATUS_CRC);
#endif
}

/**
 * @brief Send a mdhk_status message
 * @param chan MAVLink channel to send the message
 * @param struct The MAVLink struct to serialize
 */
static inline void mavlink_msg_mdhk_status_send_struct(mavlink_channel_t chan, const mavlink_mdhk_status_t* mdhk_status)
{
#if MAVLINK_NEED_BYTE_SWAP || !MAVLINK_ALIGNED_FIELDS
    mavlink_msg_mdhk_status_send(chan, mdhk_status->time_boot_ms, mdhk_status->gps_year, mdhk_status->gps_month, mdhk_status->gps_day, mdhk_status->gps_hour, mdhk_status->gps_minute, mdhk_status->gps_second, mdhk_status->valid_flags, mdhk_status->latitude, mdhk_status->longitude, mdhk_status->gps_alt, mdhk_status->relative_alt, mdhk_status->freemem32, mdhk_status->flight_sw_version, mdhk_status->wp_distance, mdhk_status->compass_variance, mdhk_status->mag_field1, mdhk_status->mag_field2, mdhk_status->cell_voltages, mdhk_status->battery_cycle_count, mdhk_status->cell_voltage_delta, mdhk_status->ekf_flags, mdhk_status->mission_seq, mdhk_status->mission_total, mdhk_status->battery_current, mdhk_status->battery_temperature, mdhk_status->battery_remaining, mdhk_status->gps_fix_type, mdhk_status->satellites_visible, mdhk_status->schema_version, mdhk_status->rgb_red, mdhk_status->rgb_green, mdhk_status->rgb_blue, mdhk_status->flight_custom_version, mdhk_status->hardware_id);
#else
    _mav_finalize_message_chan_send(chan, MAVLINK_MSG_ID_MDHK_STATUS, (const char *)mdhk_status, MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN, MAVLINK_MSG_ID_MDHK_STATUS_LEN, MAVLINK_MSG_ID_MDHK_STATUS_CRC);
#endif
}

#if MAVLINK_MSG_ID_MDHK_STATUS_LEN <= MAVLINK_MAX_PAYLOAD_LEN
/*
  This variant of _send() can be used to save stack space by reusing
  memory from the receive buffer.  The caller provides a
  mavlink_message_t which is the size of a full mavlink message. This
  is usually the receive buffer for the channel, and allows a reply to an
  incoming message with minimum stack space usage.
 */
static inline void mavlink_msg_mdhk_status_send_buf(mavlink_message_t *msgbuf, mavlink_channel_t chan,  uint32_t time_boot_ms, uint16_t gps_year, uint8_t gps_month, uint8_t gps_day, uint8_t gps_hour, uint8_t gps_minute, uint8_t gps_second, uint32_t valid_flags, int32_t latitude, int32_t longitude, int32_t gps_alt, int32_t relative_alt, uint32_t freemem32, uint32_t flight_sw_version, float wp_distance, float compass_variance, float mag_field1, float mag_field2, const uint16_t *cell_voltages, uint16_t battery_cycle_count, uint16_t cell_voltage_delta, uint16_t ekf_flags, uint16_t mission_seq, uint16_t mission_total, int16_t battery_current, int16_t battery_temperature, int8_t battery_remaining, uint8_t gps_fix_type, uint8_t satellites_visible, uint8_t schema_version, uint8_t rgb_red, uint8_t rgb_green, uint8_t rgb_blue, const uint8_t *flight_custom_version, const char *hardware_id)
{
#if MAVLINK_NEED_BYTE_SWAP || !MAVLINK_ALIGNED_FIELDS
    char *buf = (char *)msgbuf;
    _mav_put_uint32_t(buf, 0, time_boot_ms);
    _mav_put_uint32_t(buf, 4, valid_flags);
    _mav_put_int32_t(buf, 8, latitude);
    _mav_put_int32_t(buf, 12, longitude);
    _mav_put_int32_t(buf, 16, gps_alt);
    _mav_put_int32_t(buf, 20, relative_alt);
    _mav_put_uint32_t(buf, 24, freemem32);
    _mav_put_uint32_t(buf, 28, flight_sw_version);
    _mav_put_float(buf, 32, wp_distance);
    _mav_put_float(buf, 36, compass_variance);
    _mav_put_float(buf, 40, mag_field1);
    _mav_put_float(buf, 44, mag_field2);
    _mav_put_uint16_t(buf, 48, gps_year);
    _mav_put_uint16_t(buf, 58, battery_cycle_count);
    _mav_put_uint16_t(buf, 60, cell_voltage_delta);
    _mav_put_uint16_t(buf, 62, ekf_flags);
    _mav_put_uint16_t(buf, 64, mission_seq);
    _mav_put_uint16_t(buf, 66, mission_total);
    _mav_put_int16_t(buf, 68, battery_current);
    _mav_put_int16_t(buf, 70, battery_temperature);
    _mav_put_uint8_t(buf, 72, gps_month);
    _mav_put_uint8_t(buf, 73, gps_day);
    _mav_put_uint8_t(buf, 74, gps_hour);
    _mav_put_uint8_t(buf, 75, gps_minute);
    _mav_put_uint8_t(buf, 76, gps_second);
    _mav_put_int8_t(buf, 77, battery_remaining);
    _mav_put_uint8_t(buf, 78, gps_fix_type);
    _mav_put_uint8_t(buf, 79, satellites_visible);
    _mav_put_uint8_t(buf, 80, schema_version);
    _mav_put_uint8_t(buf, 81, rgb_red);
    _mav_put_uint8_t(buf, 82, rgb_green);
    _mav_put_uint8_t(buf, 83, rgb_blue);
    _mav_put_uint16_t_array(buf, 50, cell_voltages, 4);
    _mav_put_uint8_t_array(buf, 84, flight_custom_version, 8);
    _mav_put_char_array(buf, 92, hardware_id, 20);
    _mav_finalize_message_chan_send(chan, MAVLINK_MSG_ID_MDHK_STATUS, buf, MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN, MAVLINK_MSG_ID_MDHK_STATUS_LEN, MAVLINK_MSG_ID_MDHK_STATUS_CRC);
#else
    mavlink_mdhk_status_t *packet = (mavlink_mdhk_status_t *)msgbuf;
    packet->time_boot_ms = time_boot_ms;
    packet->valid_flags = valid_flags;
    packet->latitude = latitude;
    packet->longitude = longitude;
    packet->gps_alt = gps_alt;
    packet->relative_alt = relative_alt;
    packet->freemem32 = freemem32;
    packet->flight_sw_version = flight_sw_version;
    packet->wp_distance = wp_distance;
    packet->compass_variance = compass_variance;
    packet->mag_field1 = mag_field1;
    packet->mag_field2 = mag_field2;
    packet->gps_year = gps_year;
    packet->battery_cycle_count = battery_cycle_count;
    packet->cell_voltage_delta = cell_voltage_delta;
    packet->ekf_flags = ekf_flags;
    packet->mission_seq = mission_seq;
    packet->mission_total = mission_total;
    packet->battery_current = battery_current;
    packet->battery_temperature = battery_temperature;
    packet->gps_month = gps_month;
    packet->gps_day = gps_day;
    packet->gps_hour = gps_hour;
    packet->gps_minute = gps_minute;
    packet->gps_second = gps_second;
    packet->battery_remaining = battery_remaining;
    packet->gps_fix_type = gps_fix_type;
    packet->satellites_visible = satellites_visible;
    packet->schema_version = schema_version;
    packet->rgb_red = rgb_red;
    packet->rgb_green = rgb_green;
    packet->rgb_blue = rgb_blue;
    mav_array_memcpy(packet->cell_voltages, cell_voltages, sizeof(uint16_t) * 4);
    mav_array_memcpy(packet->flight_custom_version, flight_custom_version, sizeof(uint8_t) * 8);
    mav_array_memcpy(packet->hardware_id, hardware_id, sizeof(char) * 20);
    _mav_finalize_message_chan_send(chan, MAVLINK_MSG_ID_MDHK_STATUS, (const char *)packet, MAVLINK_MSG_ID_MDHK_STATUS_MIN_LEN, MAVLINK_MSG_ID_MDHK_STATUS_LEN, MAVLINK_MSG_ID_MDHK_STATUS_CRC);
#endif
}
#endif

#endif

// MESSAGE MDHK_STATUS UNPACKING


/**
 * @brief Get field time_boot_ms from mdhk_status message
 *
 * @return [ms] Timestamp since system boot.
 */
static inline uint32_t mavlink_msg_mdhk_status_get_time_boot_ms(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint32_t(msg,  0);
}

/**
 * @brief Get field gps_year from mdhk_status message
 *
 * @return  GPS UTC Gregorian year. Zero means invalid.
 */
static inline uint16_t mavlink_msg_mdhk_status_get_gps_year(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint16_t(msg,  48);
}

/**
 * @brief Get field gps_month from mdhk_status message
 *
 * @return  GPS UTC month, 1 through 12. Zero means invalid.
 */
static inline uint8_t mavlink_msg_mdhk_status_get_gps_month(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint8_t(msg,  72);
}

/**
 * @brief Get field gps_day from mdhk_status message
 *
 * @return  GPS UTC day, 1 through 31. Zero means invalid.
 */
static inline uint8_t mavlink_msg_mdhk_status_get_gps_day(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint8_t(msg,  73);
}

/**
 * @brief Get field gps_hour from mdhk_status message
 *
 * @return  GPS UTC hour in 24-hour format, 0 through 23.
 */
static inline uint8_t mavlink_msg_mdhk_status_get_gps_hour(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint8_t(msg,  74);
}

/**
 * @brief Get field gps_minute from mdhk_status message
 *
 * @return  GPS UTC minute, 0 through 59.
 */
static inline uint8_t mavlink_msg_mdhk_status_get_gps_minute(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint8_t(msg,  75);
}

/**
 * @brief Get field gps_second from mdhk_status message
 *
 * @return  GPS UTC second, 0 through 60.
 */
static inline uint8_t mavlink_msg_mdhk_status_get_gps_second(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint8_t(msg,  76);
}

/**
 * @brief Get field valid_flags from mdhk_status message
 *
 * @return  Validity bitmap for the grouped values in this packet. The vendor enum is preserved as a raw value until its bit definitions are supplied.
 */
static inline uint32_t mavlink_msg_mdhk_status_get_valid_flags(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint32_t(msg,  4);
}

/**
 * @brief Get field latitude from mdhk_status message
 *
 * @return [degE7] Raw GPS latitude.
 */
static inline int32_t mavlink_msg_mdhk_status_get_latitude(const mavlink_message_t* msg)
{
    return _MAV_RETURN_int32_t(msg,  8);
}

/**
 * @brief Get field longitude from mdhk_status message
 *
 * @return [degE7] Raw GPS longitude.
 */
static inline int32_t mavlink_msg_mdhk_status_get_longitude(const mavlink_message_t* msg)
{
    return _MAV_RETURN_int32_t(msg,  12);
}

/**
 * @brief Get field gps_alt from mdhk_status message
 *
 * @return [mm] Raw GPS altitude above mean sea level.
 */
static inline int32_t mavlink_msg_mdhk_status_get_gps_alt(const mavlink_message_t* msg)
{
    return _MAV_RETURN_int32_t(msg,  16);
}

/**
 * @brief Get field relative_alt from mdhk_status message
 *
 * @return [mm] Estimated altitude relative to HOME, positive upward.
 */
static inline int32_t mavlink_msg_mdhk_status_get_relative_alt(const mavlink_message_t* msg)
{
    return _MAV_RETURN_int32_t(msg,  20);
}

/**
 * @brief Get field freemem32 from mdhk_status message
 *
 * @return [bytes] Available system memory.
 */
static inline uint32_t mavlink_msg_mdhk_status_get_freemem32(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint32_t(msg,  24);
}

/**
 * @brief Get field flight_sw_version from mdhk_status message
 *
 * @return  Firmware version encoded as in AUTOPILOT_VERSION.flight_sw_version.
 */
static inline uint32_t mavlink_msg_mdhk_status_get_flight_sw_version(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint32_t(msg,  28);
}

/**
 * @brief Get field wp_distance from mdhk_status message
 *
 * @return [m] Horizontal distance from the aircraft to the current navigation waypoint.
 */
static inline float mavlink_msg_mdhk_status_get_wp_distance(const mavlink_message_t* msg)
{
    return _MAV_RETURN_float(msg,  32);
}

/**
 * @brief Get field compass_variance from mdhk_status message
 *
 * @return  Maximum EKF compass variance.
 */
static inline float mavlink_msg_mdhk_status_get_compass_variance(const mavlink_message_t* msg)
{
    return _MAV_RETURN_float(msg,  36);
}

/**
 * @brief Get field mag_field1 from mdhk_status message
 *
 * @return [mgauss] Magnitude of compass instance 1 field vector.
 */
static inline float mavlink_msg_mdhk_status_get_mag_field1(const mavlink_message_t* msg)
{
    return _MAV_RETURN_float(msg,  40);
}

/**
 * @brief Get field mag_field2 from mdhk_status message
 *
 * @return [mgauss] Magnitude of compass instance 2 field vector.
 */
static inline float mavlink_msg_mdhk_status_get_mag_field2(const mavlink_message_t* msg)
{
    return _MAV_RETURN_float(msg,  44);
}

/**
 * @brief Get field cell_voltages from mdhk_status message
 *
 * @return [mV] Primary battery cell voltages 1 through 4. UINT16_MAX means invalid.
 */
static inline uint16_t mavlink_msg_mdhk_status_get_cell_voltages(const mavlink_message_t* msg, uint16_t *cell_voltages)
{
    return _MAV_RETURN_uint16_t_array(msg, cell_voltages, 4,  50);
}

/**
 * @brief Get field battery_cycle_count from mdhk_status message
 *
 * @return  Primary smart-battery cycle count. UINT16_MAX means invalid.
 */
static inline uint16_t mavlink_msg_mdhk_status_get_battery_cycle_count(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint16_t(msg,  58);
}

/**
 * @brief Get field cell_voltage_delta from mdhk_status message
 *
 * @return [mV] Difference between the highest and lowest valid primary-battery cell voltage. UINT16_MAX means invalid.
 */
static inline uint16_t mavlink_msg_mdhk_status_get_cell_voltage_delta(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint16_t(msg,  60);
}

/**
 * @brief Get field ekf_flags from mdhk_status message
 *
 * @return  Flags encoded as in EKF_STATUS_REPORT.flags.
 */
static inline uint16_t mavlink_msg_mdhk_status_get_ekf_flags(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint16_t(msg,  62);
}

/**
 * @brief Get field mission_seq from mdhk_status message
 *
 * @return  Current mission sequence number.
 */
static inline uint16_t mavlink_msg_mdhk_status_get_mission_seq(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint16_t(msg,  64);
}

/**
 * @brief Get field mission_total from mdhk_status message
 *
 * @return  Total mission item count excluding HOME.
 */
static inline uint16_t mavlink_msg_mdhk_status_get_mission_total(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint16_t(msg,  66);
}

/**
 * @brief Get field battery_current from mdhk_status message
 *
 * @return [cA] Primary battery current. -1 means invalid.
 */
static inline int16_t mavlink_msg_mdhk_status_get_battery_current(const mavlink_message_t* msg)
{
    return _MAV_RETURN_int16_t(msg,  68);
}

/**
 * @brief Get field battery_temperature from mdhk_status message
 *
 * @return [cdegC] Primary battery temperature. INT16_MAX means invalid.
 */
static inline int16_t mavlink_msg_mdhk_status_get_battery_temperature(const mavlink_message_t* msg)
{
    return _MAV_RETURN_int16_t(msg,  70);
}

/**
 * @brief Get field battery_remaining from mdhk_status message
 *
 * @return [%] Primary battery remaining percentage. -1 means invalid.
 */
static inline int8_t mavlink_msg_mdhk_status_get_battery_remaining(const mavlink_message_t* msg)
{
    return _MAV_RETURN_int8_t(msg,  77);
}

/**
 * @brief Get field gps_fix_type from mdhk_status message
 *
 * @return  Raw GPS fix type.
 */
static inline uint8_t mavlink_msg_mdhk_status_get_gps_fix_type(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint8_t(msg,  78);
}

/**
 * @brief Get field satellites_visible from mdhk_status message
 *
 * @return  Number of satellites visible. 255 means invalid.
 */
static inline uint8_t mavlink_msg_mdhk_status_get_satellites_visible(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint8_t(msg,  79);
}

/**
 * @brief Get field schema_version from mdhk_status message
 *
 * @return  MDHK_STATUS payload schema version.
 */
static inline uint8_t mavlink_msg_mdhk_status_get_schema_version(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint8_t(msg,  80);
}

/**
 * @brief Get field rgb_red from mdhk_status message
 *
 * @return  Final logical red LED output, 0 through 255.
 */
static inline uint8_t mavlink_msg_mdhk_status_get_rgb_red(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint8_t(msg,  81);
}

/**
 * @brief Get field rgb_green from mdhk_status message
 *
 * @return  Final logical green LED output, 0 through 255.
 */
static inline uint8_t mavlink_msg_mdhk_status_get_rgb_green(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint8_t(msg,  82);
}

/**
 * @brief Get field rgb_blue from mdhk_status message
 *
 * @return  Final logical blue LED output, 0 through 255.
 */
static inline uint8_t mavlink_msg_mdhk_status_get_rgb_blue(const mavlink_message_t* msg)
{
    return _MAV_RETURN_uint8_t(msg,  83);
}

/**
 * @brief Get field flight_custom_version from mdhk_status message
 *
 * @return  Firmware Git/custom version bytes.
 */
static inline uint16_t mavlink_msg_mdhk_status_get_flight_custom_version(const mavlink_message_t* msg, uint8_t *flight_custom_version)
{
    return _MAV_RETURN_uint8_t_array(msg, flight_custom_version, 8,  84);
}

/**
 * @brief Get field hardware_id from mdhk_status message
 *
 * @return  Twenty-character Base32 STM32 hardware identifier.
 */
static inline uint16_t mavlink_msg_mdhk_status_get_hardware_id(const mavlink_message_t* msg, char *hardware_id)
{
    return _MAV_RETURN_char_array(msg, hardware_id, 20,  92);
}

/**
 * @brief Decode a mdhk_status message into a struct
 *
 * @param msg The message to decode
 * @param mdhk_status C-struct to decode the message contents into
 */
static inline void mavlink_msg_mdhk_status_decode(const mavlink_message_t* msg, mavlink_mdhk_status_t* mdhk_status)
{
#if MAVLINK_NEED_BYTE_SWAP || !MAVLINK_ALIGNED_FIELDS
    mdhk_status->time_boot_ms = mavlink_msg_mdhk_status_get_time_boot_ms(msg);
    mdhk_status->valid_flags = mavlink_msg_mdhk_status_get_valid_flags(msg);
    mdhk_status->latitude = mavlink_msg_mdhk_status_get_latitude(msg);
    mdhk_status->longitude = mavlink_msg_mdhk_status_get_longitude(msg);
    mdhk_status->gps_alt = mavlink_msg_mdhk_status_get_gps_alt(msg);
    mdhk_status->relative_alt = mavlink_msg_mdhk_status_get_relative_alt(msg);
    mdhk_status->freemem32 = mavlink_msg_mdhk_status_get_freemem32(msg);
    mdhk_status->flight_sw_version = mavlink_msg_mdhk_status_get_flight_sw_version(msg);
    mdhk_status->wp_distance = mavlink_msg_mdhk_status_get_wp_distance(msg);
    mdhk_status->compass_variance = mavlink_msg_mdhk_status_get_compass_variance(msg);
    mdhk_status->mag_field1 = mavlink_msg_mdhk_status_get_mag_field1(msg);
    mdhk_status->mag_field2 = mavlink_msg_mdhk_status_get_mag_field2(msg);
    mdhk_status->gps_year = mavlink_msg_mdhk_status_get_gps_year(msg);
    mavlink_msg_mdhk_status_get_cell_voltages(msg, mdhk_status->cell_voltages);
    mdhk_status->battery_cycle_count = mavlink_msg_mdhk_status_get_battery_cycle_count(msg);
    mdhk_status->cell_voltage_delta = mavlink_msg_mdhk_status_get_cell_voltage_delta(msg);
    mdhk_status->ekf_flags = mavlink_msg_mdhk_status_get_ekf_flags(msg);
    mdhk_status->mission_seq = mavlink_msg_mdhk_status_get_mission_seq(msg);
    mdhk_status->mission_total = mavlink_msg_mdhk_status_get_mission_total(msg);
    mdhk_status->battery_current = mavlink_msg_mdhk_status_get_battery_current(msg);
    mdhk_status->battery_temperature = mavlink_msg_mdhk_status_get_battery_temperature(msg);
    mdhk_status->gps_month = mavlink_msg_mdhk_status_get_gps_month(msg);
    mdhk_status->gps_day = mavlink_msg_mdhk_status_get_gps_day(msg);
    mdhk_status->gps_hour = mavlink_msg_mdhk_status_get_gps_hour(msg);
    mdhk_status->gps_minute = mavlink_msg_mdhk_status_get_gps_minute(msg);
    mdhk_status->gps_second = mavlink_msg_mdhk_status_get_gps_second(msg);
    mdhk_status->battery_remaining = mavlink_msg_mdhk_status_get_battery_remaining(msg);
    mdhk_status->gps_fix_type = mavlink_msg_mdhk_status_get_gps_fix_type(msg);
    mdhk_status->satellites_visible = mavlink_msg_mdhk_status_get_satellites_visible(msg);
    mdhk_status->schema_version = mavlink_msg_mdhk_status_get_schema_version(msg);
    mdhk_status->rgb_red = mavlink_msg_mdhk_status_get_rgb_red(msg);
    mdhk_status->rgb_green = mavlink_msg_mdhk_status_get_rgb_green(msg);
    mdhk_status->rgb_blue = mavlink_msg_mdhk_status_get_rgb_blue(msg);
    mavlink_msg_mdhk_status_get_flight_custom_version(msg, mdhk_status->flight_custom_version);
    mavlink_msg_mdhk_status_get_hardware_id(msg, mdhk_status->hardware_id);
#else
        uint8_t len = msg->len < MAVLINK_MSG_ID_MDHK_STATUS_LEN? msg->len : MAVLINK_MSG_ID_MDHK_STATUS_LEN;
        memset(mdhk_status, 0, MAVLINK_MSG_ID_MDHK_STATUS_LEN);
    memcpy(mdhk_status, _MAV_PAYLOAD(msg), len);
#endif
}
