/**
 * @brief 解析并校验 gcs-config.ini，生成 Console 单一启动配置快照。
 *
 * 分类：Production；只读取配置，不创建 Socket 或修改协议行为。
 */
#include "AppConfig.h"

#include "DSPGCSProtocol.h"
#include "DSPGCSTransportConfig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace
{
    std::string Trim(const std::string& value)
    {
        const auto first = std::find_if_not(
            value.begin(),
            value.end(),
            [](unsigned char ch) { return std::isspace(ch) != 0; });

        if (first == value.end())
        {
            return {};
        }

        const auto last = std::find_if_not(
            value.rbegin(),
            value.rend(),
            [](unsigned char ch) { return std::isspace(ch) != 0; }).base();

        return std::string(first, last);
    }

    bool ParseInteger(
        const std::unordered_map<std::string, std::string>& values,
        const std::string& key,
        int minimum,
        int maximum,
        int& result,
        std::string& error)
    {
        const auto item = values.find(key);

        if (item == values.end())
        {
            error = "Missing configuration key: " + key;
            return false;
        }

        try
        {
            std::size_t parsed = 0;
            const long value = std::stol(item->second, &parsed);

            if (parsed != item->second.size() ||
                value < minimum ||
                value > maximum)
            {
                error = "Invalid value for " + key + ": " + item->second;
                return false;
            }

            result = static_cast<int>(value);
            return true;
        }
        catch (const std::exception&)
        {
            error = "Invalid value for " + key + ": " + item->second;
            return false;
        }
    }

    bool ParseBoolean(
        const std::unordered_map<std::string, std::string>& values,
        const std::string& key,
        const bool defaultValue,
        bool& result,
        std::string& error)
    {
        const auto item = values.find(key);
        if (item == values.end())
        {
            result = defaultValue;
            return true;
        }

        std::string value = item->second;
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](const unsigned char ch)
            {
                return static_cast<char>(std::tolower(ch));
            });

        if (value == "true" || value == "1" || value == "yes")
        {
            result = true;
            return true;
        }
        if (value == "false" || value == "0" || value == "no")
        {
            result = false;
            return true;
        }

        error = "Invalid value for " + key + ": " + item->second;
        return false;
    }

    bool ParseOptionalInteger(
        const std::unordered_map<std::string, std::string>& values,
        const std::string& key,
        const int minimum,
        const int maximum,
        int& result,
        std::string& error)
    {
        if (values.find(key) == values.end())
        {
            return true;
        }
        return ParseInteger(
            values,
            key,
            minimum,
            maximum,
            result,
            error);
    }

    bool ParseOptionalFloat(
        const std::unordered_map<std::string, std::string>& values,
        const std::string& key,
        const float defaultValue,
        const float minimumExclusive,
        const float maximumInclusive,
        float& result,
        std::string& error)
    {
        const auto item = values.find(key);
        if (item == values.end())
        {
            result = defaultValue;
            return true;
        }

        try
        {
            std::size_t parsed = 0;
            const float value = std::stof(item->second, &parsed);
            if (parsed != item->second.size() ||
                !std::isfinite(value) ||
                value <= minimumExclusive ||
                value > maximumInclusive)
            {
                error = "Invalid value for " + key + ": " + item->second;
                return false;
            }

            result = value;
            return true;
        }
        catch (const std::exception&)
        {
            error = "Invalid value for " + key + ": " + item->second;
            return false;
        }
    }
}

bool AppConfig::Load(
    const std::string& path,
    AppConfig& config,
    std::string& error)
{
    std::ifstream input(path);

    if (!input)
    {
        error = "Unable to open configuration file: " + path;
        return false;
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    std::string section;
    int lineNumber = 0;

    while (std::getline(input, line))
    {
        ++lineNumber;

        const std::string trimmed = Trim(line);

        if (trimmed.empty() ||
            trimmed.front() == '#' ||
            trimmed.front() == ';')
        {
            continue;
        }

        if (trimmed.front() == '[')
        {
            if (trimmed.size() < 3 || trimmed.back() != ']')
            {
                error =
                    "Invalid configuration line " +
                    std::to_string(lineNumber) +
                    ": malformed section";
                return false;
            }

            section = Trim(trimmed.substr(1, trimmed.size() - 2));
            if (section.empty())
            {
                error =
                    "Invalid configuration line " +
                    std::to_string(lineNumber) +
                    ": empty section";
                return false;
            }
            continue;
        }

        const auto separator = trimmed.find('=');

        if (separator == std::string::npos)
        {
            error =
                "Invalid configuration line " +
                std::to_string(lineNumber) +
                ": expected key=value";
            return false;
        }

        const std::string key = Trim(trimmed.substr(0, separator));
        const std::string value = Trim(trimmed.substr(separator + 1));

        if (key.empty() || value.empty())
        {
            error =
                "Invalid configuration line " +
                std::to_string(lineNumber) +
                ": empty key or value";
            return false;
        }

        values[section.empty() ? key : section + "." + key] = value;
    }

    if (!ParseBoolean(
        values,
        "safe_test_mode",
        true,
        config.safeTestMode,
        error))
    {
        return false;
    }

    if (!ParseBoolean(
        values,
        "mavlink_network_enabled",
        false,
        config.mavlinkNetworkEnabled,
        error))
    {
        return false;
    }

    if (!ParseBoolean(
        values,
        "real_command_enabled",
        false,
        config.realCommandEnabled,
        error))
    {
        return false;
    }

    if (!ParseBoolean(
        values,
        "route_upload_enabled",
        false,
        config.routeUploadEnabled,
        error))
    {
        return false;
    }

    const auto bindIP = values.find("mavlink_bind_ip");
    if (bindIP != values.end())
    {
        config.mavlinkBindIP = bindIP->second;
    }

    if (config.mavlinkNetworkEnabled)
    {
        if (bindIP == values.end())
        {
            error = "Missing configuration key: mavlink_bind_ip";
            return false;
        }
        if (!ParseInteger(
            values,
            "mavlink_bind_port",
            1,
            65535,
            config.mavlinkBindPort,
            error))
        {
            return false;
        }
    }

    if (!ParseBoolean(
        values,
        "dspgcs_telemetry_enabled",
        false,
        config.dspgcsTelemetryEnabled,
        error))
    {
        return false;
    }

    if (!ParseBoolean(
        values,
        "dspgcs_command_enabled",
        false,
        config.dspgcsCommandEnabled,
        error))
    {
        return false;
    }

    if (config.realCommandEnabled && !config.mavlinkNetworkEnabled)
    {
        error =
            "real_command_enabled=true requires mavlink_network_enabled=true";
        return false;
    }

    if (config.routeUploadEnabled && !config.mavlinkNetworkEnabled)
    {
        error =
            "route_upload_enabled=true requires mavlink_network_enabled=true";
        return false;
    }

    if (config.realCommandEnabled && !config.dspgcsCommandEnabled)
    {
        error =
            "real_command_enabled=true requires dspgcs_command_enabled=true";
        return false;
    }

    const auto transportConfigPath =
        values.find("dspgcs_transport_config");
    if (transportConfigPath != values.end())
    {
        config.dspgcsTransportConfigPath = transportConfigPath->second;
    }

    const auto artifactWorkspaceRoot = values.find("artifact_workspace_root");
    if (artifactWorkspaceRoot != values.end())
    {
        const std::filesystem::path root =
            std::filesystem::u8path(artifactWorkspaceRoot->second).lexically_normal();
        if (!root.is_absolute() || root == root.root_path())
        {
            error = "artifact_workspace_root must be an absolute non-volume path";
            return false;
        }
        config.artifactWorkspaceRoot = root.u8string();
    }

    const auto telemetryInterval =
        values.find("dspgcs_telemetry_interval_ms");
    if (telemetryInterval != values.end() &&
        !ParseInteger(
            values,
            "dspgcs_telemetry_interval_ms",
            50,
            5000,
            config.dspgcsTelemetryIntervalMs,
            error))
    {
        return false;
    }

    if (!ParseInteger(
            values,
            "heartbeat_timeout_ms",
            1000,
            30000,
            config.heartbeatTimeoutMs,
            error))
    {
        return false;
    }

    if (!ParseOptionalInteger(
            values,
            "performance.mavlink_receive_buffer_bytes",
            dspgcs::DSPGCSDefaults::MinimumSocketBufferBytes,
            dspgcs::DSPGCSDefaults::MaximumSocketBufferBytes,
            config.mavlinkReceiveBufferBytes,
            error) ||
        !ParseOptionalInteger(
            values,
            "performance.max_packets_per_drain",
            1,
            100000,
            config.maxPacketsPerDrain,
            error) ||
        !ParseOptionalInteger(
            values,
            "performance.snapshot_datagram_bytes",
            1024,
            static_cast<int>(dspgcs::DSPGCSDefaults::MaxUdpPayloadBytes),
            config.snapshotDatagramBytes,
            error) ||
        !ParseOptionalInteger(
            values,
            "performance.snapshot_items_per_chunk",
            1,
            static_cast<int>(dspgcs::MaxCollectionItems),
            config.snapshotItemsPerChunk,
            error))
    {
        return false;
    }

    int mavftpSegmentItems = static_cast<int>(config.mavftpSegmentItems);
    int mavftpRequestRetryCount =
        static_cast<int>(config.mavftpRequestRetryCount);
    int mavftpWriteWindow = static_cast<int>(config.mavftpWriteWindow);
    if (!ParseOptionalInteger(
            values,
            "mavftp.segment_items",
            1,
            65535,
            mavftpSegmentItems,
            error) ||
        !ParseOptionalInteger(
            values,
            "mavftp.request_timeout_ms",
            100,
            60000,
            config.mavftpRequestTimeoutMs,
            error) ||
        !ParseOptionalInteger(
            values,
            "mavftp.request_retry_count",
            0,
            10,
            mavftpRequestRetryCount,
            error) ||
        !ParseOptionalInteger(
            values,
            "mavftp.write_window",
            1,
            16,
            mavftpWriteWindow,
            error))
    {
        return false;
    }
    config.mavftpSegmentItems =
        static_cast<std::uint32_t>(mavftpSegmentItems);
    config.mavftpRequestRetryCount =
        static_cast<std::uint32_t>(mavftpRequestRetryCount);
    config.mavftpWriteWindow =
        static_cast<std::uint32_t>(mavftpWriteWindow);

    constexpr float defaultLimitMeters = 5.0f;
    constexpr float maximumConfigurableLimitMeters = 1000.0f;
    return
        ParseOptionalFloat(
            values,
            "max_flight.takeoff_altitude_limit",
            defaultLimitMeters,
            0.0f,
            maximumConfigurableLimitMeters,
            config.takeoffAltitudeLimitMeters,
            error) &&
        ParseOptionalFloat(
            values,
            "max_flight.hover_altitude_limit",
            defaultLimitMeters,
            0.0f,
            maximumConfigurableLimitMeters,
            config.hoverAltitudeLimitMeters,
            error);
}
