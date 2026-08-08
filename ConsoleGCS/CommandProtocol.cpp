/**
 * @brief 实现旧 GCS1 文本命令协议的解析、ACK/PONG/ERR 文本编码。
 *
 * 分类：Legacy / Compatibility；当前 Production 命令由 DSPGCSProductionCommandReceiver 接收。
 */
#include "CommandProtocol.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

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

    std::string Upper(const std::string& value)
    {
        std::string result = value;

        std::transform(
            result.begin(),
            result.end(),
            result.begin(),
            [](unsigned char ch)
            {
                return static_cast<char>(std::toupper(ch));
            });

        return result;
    }

    bool IsSafeToken(const std::string& value)
    {
        if (value.empty() || value.size() > 64)
        {
            return false;
        }

        return std::all_of(
            value.begin(),
            value.end(),
            [](unsigned char ch)
            {
                return std::isalnum(ch) != 0 ||
                    ch == '-' ||
                    ch == '_' ||
                    ch == '.';
            });
    }
}

bool CommandProtocol::Parse(
    const char* data,
    int length,
    CommandRequest& request,
    std::string& error)
{
    request = {};
    error.clear();

    if (data == nullptr || length <= 0)
    {
        error = "EMPTY_PACKET";
        return false;
    }

    std::string payload(data, static_cast<size_t>(length));

    while (!payload.empty() &&
        (payload.back() == '\0' ||
            payload.back() == '\r' ||
            payload.back() == '\n'))
    {
        payload.pop_back();
    }

    std::vector<std::string> fields;
    std::stringstream stream(payload);
    std::string field;

    while (std::getline(stream, field, '|'))
    {
        fields.push_back(Trim(field));
    }

    if (fields.size() < 3 || fields[0] != "GCS1")
    {
        error = "BAD_FORMAT";
        return false;
    }

    if (!IsSafeToken(fields[1]))
    {
        error = "BAD_REQUEST_ID";
        return false;
    }

    request.requestId = fields[1];
    request.command = Upper(fields[2]);

    if (!IsSupportedCommand(request.command))
    {
        error = "UNSUPPORTED_COMMAND";
        return false;
    }

    for (size_t index = 3; index < fields.size(); ++index)
    {
        if (fields[index].size() > 64)
        {
            error = "ARGUMENT_TOO_LONG";
            return false;
        }

        request.arguments.push_back(fields[index]);
    }

    return true;
}

bool CommandProtocol::IsSupportedCommand(const std::string& command)
{
    static const std::unordered_set<std::string> supported
    {
        "PING",
        "ARM",
        "DISARM",
        "GUIDED",
        "TAKEOFF",
        "LAND",
        "RTL",
        "LOITER",
        "HOVER",
        "STOP"
    };

    return supported.find(command) != supported.end();
}

std::string CommandProtocol::Pong(
    const std::string& requestId,
    bool safeTestMode)
{
    return "GCS1|" + requestId + "|PONG|SAFE_TEST_MODE=" +
        (safeTestMode ? "1" : "0");
}

std::string CommandProtocol::Ack(
    const std::string& requestId,
    const std::string& command,
    const std::string& detail)
{
    return "GCS1|" + requestId + "|ACK|" + command + "|" + detail;
}

std::string CommandProtocol::Error(
    const std::string& requestId,
    const std::string& command,
    const std::string& detail)
{
    return "GCS1|" + requestId + "|ERR|" + command + "|" + detail;
}
