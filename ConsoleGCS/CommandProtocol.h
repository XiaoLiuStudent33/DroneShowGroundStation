#pragma once

#include <string>
#include <vector>

struct CommandRequest
{
    std::string requestId;
    std::string command;
    std::vector<std::string> arguments;
};

class CommandProtocol
{
public:
    static bool Parse(
        const char* data,
        int length,
        CommandRequest& request,
        std::string& error);

    static bool IsSupportedCommand(const std::string& command);

    static std::string Pong(
        const std::string& requestId,
        bool safeTestMode);

    static std::string Ack(
        const std::string& requestId,
        const std::string& command,
        const std::string& detail);

    static std::string Error(
        const std::string& requestId,
        const std::string& command,
        const std::string& detail);
};
