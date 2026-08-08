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

    bool ParsePort(
        const std::unordered_map<std::string, std::string>& values,
        const std::string& key,
        std::uint16_t& output,
        std::string& error)
    {
        const auto item = values.find(key);
        if (item == values.end())
        {
            error = "Missing DSP-GCS configuration key: " + key;
            return false;
        }
        try
        {
            std::size_t parsed = 0;
            const long value = std::stol(item->second, &parsed);
            if (parsed != item->second.size() || value <= 0 || value > 65535)
            {
                error = "Invalid DSP-GCS port " + key + "=" + item->second;
                return false;
            }
            output = static_cast<std::uint16_t>(value);
            return true;
        }
        catch (const std::exception&)
        {
            error = "Invalid DSP-GCS port " + key + "=" + item->second;
            return false;
        }
    }
}

bool LoadDSPGCSTransportConfig(
    const std::string& path,
    dspgcs::DSPGCSTransportConfig& config,
    std::string& error)
{
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
    const auto bindAddress = values.find("BindAddress");
    const auto remoteAddress = values.find("RemoteAddress");
    if (type == values.end() ||
        !dspgcs::TryParseTransportType(type->second, config.type))
    {
        error = "Invalid or missing DSP-GCS TransportType";
        return false;
    }
    if (bindAddress == values.end() || remoteAddress == values.end() ||
        bindAddress->second.empty() || remoteAddress->second.empty())
    {
        error = "Missing DSP-GCS bind or remote address";
        return false;
    }

    config.bindAddress = bindAddress->second;
    config.remoteAddress = remoteAddress->second;
    return ParsePort(values, "BindPort", config.bindPort, error) &&
        ParsePort(values, "RemotePort", config.remotePort, error);
}
