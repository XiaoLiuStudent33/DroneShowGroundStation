#pragma once

#include "CommandDispatcher.h"
#include "CommandProtocol.h"
#include "DataManager.h"
#include "UdpCommandServer.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class DroneControlService
{
public:
    DroneControlService(
        DataManager& dataManager,
        CommandDispatcher& dispatcher,
        UdpCommandServer& commandServer);

    void Poll();
    void Tick();

    void HandleDispatchResults(
        const std::vector<CommandDispatchResult>& results);

private:
    struct PendingRequest
    {
        bool active = false;
        std::uint64_t dispatchId = 0;
        std::string requestId;
        std::string command;
        int expectedMode = -1;
        sockaddr_in recipient{};
        std::chrono::steady_clock::time_point startedAt{};
    };

    void HandleRequest(
        const CommandRequest& request,
        const sockaddr_in& sender);

    bool QueueRealCommand(
        const CommandRequest& request,
        const sockaddr_in& sender,
        std::string& error);

    void CompletePending(
        bool success,
        const std::string& detail);

    void ReplyAndCache(
        const std::string& requestId,
        const std::string& response,
        const sockaddr_in& recipient);

    void PublishFlightState();
    std::string BuildFlightStateMessage() const;

    static bool ParseTakeoffAltitude(
        const CommandRequest& request,
        float& altitude);

    DataManager& dataManager;
    CommandDispatcher& dispatcher;
    UdpCommandServer& commandServer;
    PendingRequest pending;
    std::unordered_map<std::string, std::string> responseCache;
    bool hasStatusSubscriber = false;
    sockaddr_in statusSubscriber{};
    std::chrono::steady_clock::time_point lastStatusPublish{};
};
