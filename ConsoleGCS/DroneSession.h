#pragma once

#include "CommandDispatcher.h"
#include "DataManager.h"
#include "DroneDiscovery.h"
#include "DroneCommandRequest.h"
#include "FlightControl.h"
#include "FlightData.h"
#include "HoverTest.h"
#include "MavlinkParser.h"
#include "UdpClient.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class IMavlinkDatagramSender;

enum class DroneSessionState
{
    Stopped,
    Initializing,
    WaitingForHeartbeat,
    Online,
    HeartbeatTimeout,
    Error
};

const char* DroneSessionStateName(DroneSessionState state);

struct DroneSessionConfig
{
    std::string droneId;
    std::string targetIp;
    int targetPort = 0;
    std::string localIp;
    int localPort = 0;
    int heartbeatTimeoutMs = 3000;
    bool receiveOnly = false;
    bool realCommandEnabled = false;
    IMavlinkDatagramSender* sharedMavlinkEndpoint = nullptr;
    std::uint8_t discoveredSystemId = 0;
    std::uint8_t discoveredComponentId = 0;
};

struct DroneAckState
{
    bool valid = false;
    std::uint16_t command = 0;
    std::uint8_t result = 0;
    std::chrono::steady_clock::time_point receivedAt{};
};

struct DroneSessionCommandSubmission
{
    bool accepted = false;
    bool completedImmediately = false;
    bool simulated = false;
    std::vector<std::uint64_t> dispatchRequestIds;
    std::string error;
    std::string detail;
};

// Owns the complete runtime state and existing MAVLink control stack for one
// aircraft. This is a ConsoleGCS domain object, not a UE display DTO.
class DroneSession
{
public:
    explicit DroneSession(DroneSessionConfig config);
    ~DroneSession() noexcept;

    DroneSession(const DroneSession&) = delete;
    DroneSession& operator=(const DroneSession&) = delete;

    bool Start();
    void Stop();
    void Tick(std::chrono::steady_clock::time_point now);
    void RefreshHeartbeatStatus(
        std::chrono::steady_clock::time_point now);
    DroneSessionCommandSubmission SubmitCommand(
        const DroneCommandRequest& request);
    std::vector<CommandDispatchResult> ConsumeCommandResults();
    bool RefreshDiscoveredPresence(
        const DiscoveredDroneIdentity& identity);
    bool ApplyDiscoveredHeartbeat(
        const mavlink_message_t& message,
        const DiscoveredDroneIdentity& identity);
    bool ApplyDiscoveredTelemetry(
        const mavlink_message_t& message,
        const DiscoveredDroneIdentity& identity);
    bool ApplyDiscoveredCommandMessage(
        const mavlink_message_t& message,
        const DiscoveredDroneIdentity& identity);

    const std::string& GetDroneId() const;
    std::uint8_t GetSysId() const;
    DroneSessionState GetState() const;
    const DroneSessionConfig& GetConfig() const;
    FlightData GetFlightData() const;
    DroneAckState GetAckState() const;
    const HoverTest& GetHoverTest() const;
    HoverMissionState GetHoverMissionState() const;
    float GetHoverTargetAltitudeMeters() const;

private:
    void SetState(DroneSessionState newState);
    void SendHeartbeat();
    void RequestDataStream(std::uint8_t system, std::uint8_t component);
    void ProcessIncomingData(std::chrono::steady_clock::time_point now);
    void ProcessDispatchResults(std::chrono::steady_clock::time_point now);

private:
    DroneSessionConfig config;
    DroneSessionState state = DroneSessionState::Stopped;

    UdpClient udp;
    DataManager dataManager;
    FlightData workingData;
    MavlinkParser parser;
    mavlink_message_t parseMessageBuffer{};
    mavlink_status_t parseStatus{};
    HoverTest hoverTest;
    CommandDispatcher dispatcher;
    FlightControl flightControl;

    bool started = false;
    bool requestSent = false;
    std::chrono::steady_clock::time_point lastGcsHeartbeat{};
    std::deque<CommandDispatchResult> completedCommandResults;
};
