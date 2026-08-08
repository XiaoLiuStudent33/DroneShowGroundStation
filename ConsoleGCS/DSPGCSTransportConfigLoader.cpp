/**
 * @brief 实现 DSP-GCS Transport 配置文件解析和范围校验。
 *
 * 分类：Production。
 * 当前配置选择同机 Loopback Transport，但 127.0.0.1 在这里属于实际 Production 通道。
 */
#include "DSPGCSTransportConfigLoader.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>

namespace
{
    std::string Trim(const std::string& value)
    {
        const auto first = std::find_if_not(
            value.begin(), value.end(),
            [](const unsigned char ch) { return std::isspace(ch) != 0; });
        if (first == value.end())
        {
            return {};
        }
        const auto last = std::find_if_not(
            value.rbegin(), value.rend(),
            [](const unsigned char ch) { return std::isspace(ch) != 0; }).base();
        return std::string(first, last);
    }

    const std::string* FindValue(
        const std::unordered_map<std::string, std::string>& values,
        const std::string& primaryKey,
        const std::string& compatibilityKey)
    {
        const auto primary = values.find(primaryKey);
        if (primary != values.end())
        {
            return &primary->second;
        }
        const auto compatibility = values.find(compatibilityKey);
        return compatibility != values.end() ? &compatibility->second : nullptr;
    }

    bool ParseIntegerValue(
        const std::string& valueText,
        const std::string& key,
        const long minimum,
        const long maximum,
        long& output,
        std::string& error)
    {
        try
        {
            std::size_t parsed = 0;
            const long value = std::stol(valueText, &parsed);
            if (parsed != valueText.size() ||
                value < minimum || value > maximum)
            {
                error = "Invalid DSP-GCS value " + key + "=" + valueText;
                return false;
            }
            output = value;
            return true;
        }
        catch (const std::exception&)
        {
            error = "Invalid DSP-GCS value " + key + "=" + valueText;
            return false;
        }
    }

    bool ParseOptionalInteger(
        const std::unordered_map<std::string, std::string>& values,
        const std::string& key,
        const long minimum,
        const long maximum,
        int& output,
        std::string& error)
    {
        const auto item = values.find(key);
        if (item == values.end())
        {
            return true;
        }
        long parsedValue = 0;
        if (!ParseIntegerValue(
                item->second,
                key,
                minimum,
                maximum,
                parsedValue,
                error))
        {
            return false;
        }
        output = static_cast<int>(parsedValue);
        return true;
    }
}

bool LoadDSPGCSTransportConfig(
    const std::string& path,
    dspgcs::DSPGCSTransportConfig& config,
    std::string& error)
{
    config = dspgcs::MakeConsoleDSPGCSDefaults();
    std::ifstream input(path);
    if (!input)
    {
        error = "Unable to open DSP-GCS transport config: " + path;
        return false;
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line))
    {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';')
        {
            continue;
        }
        const std::size_t separator = trimmed.find('=');
        if (separator == std::string::npos)
        {
            error = "Invalid DSP-GCS configuration line: " + trimmed;
            return false;
        }
        values[Trim(trimmed.substr(0, separator))] =
            Trim(trimmed.substr(separator + 1));
    }

    const auto type = values.find("TransportType");
    const std::string* consoleAddress =
        FindValue(values, "ConsoleAddress", "BindAddress");
    const std::string* unrealAddress =
        FindValue(values, "UnrealAddress", "RemoteAddress");
    const std::string* consolePort =
        FindValue(values, "ConsolePort", "BindPort");
    const std::string* unrealPort =
        FindValue(values, "UnrealPort", "RemotePort");
    if (type == values.end() ||
        !dspgcs::TryParseTransportType(type->second, config.type))
    {
        error = "Invalid or missing DSP-GCS TransportType";
        return false;
    }
    if (consoleAddress == nullptr || unrealAddress == nullptr ||
        consolePort == nullptr || unrealPort == nullptr ||
        consoleAddress->empty() || unrealAddress->empty())
    {
        error = "Missing DSP-GCS ConsoleEndpoint or UnrealEndpoint";
        return false;
    }

    long parsedConsolePort = 0;
    long parsedUnrealPort = 0;
    if (!ParseIntegerValue(
            *consolePort,
            "ConsolePort",
            1,
            65535,
            parsedConsolePort,
            error) ||
        !ParseIntegerValue(
            *unrealPort,
            "UnrealPort",
            1,
            65535,
            parsedUnrealPort,
            error) ||
        !ParseOptionalInteger(
            values,
            "ReceiveBufferBytes",
            dspgcs::DSPGCSDefaults::MinimumSocketBufferBytes,
            dspgcs::DSPGCSDefaults::MaximumSocketBufferBytes,
            config.receiveBufferBytes,
            error) ||
        !ParseOptionalInteger(
            values,
            "ReceiverPollMs",
            dspgcs::DSPGCSDefaults::MinimumReceiverPollMs,
            dspgcs::DSPGCSDefaults::MaximumReceiverPollMs,
            config.receiverPollMs,
            error))
    {
        return false;
    }

    config.bindAddress = *consoleAddress;
    config.bindPort = static_cast<std::uint16_t>(parsedConsolePort);
    config.remoteAddress = *unrealAddress;
    config.remotePort = static_cast<std::uint16_t>(parsedUnrealPort);
    return true;
}
