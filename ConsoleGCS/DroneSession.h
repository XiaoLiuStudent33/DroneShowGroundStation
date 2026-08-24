/**
 * @brief 表示一个稳定 Remote Endpoint 的遥测、在线状态和命令生命周期。
 *
 * 分类：Production；每个 Session 使用共享 MAVLink Endpoint 向自己的 Remote Endpoint 发送。
 * 输入：Discovery/Ingress 消息和领域命令；输出：FlightData、MAVLink 命令、ACK/任务结果。
 * 关键约束：Heartbeat timeout 只转为 Offline，不销毁 Session、DroneID 或历史遥测。
 */
#pragma once

#include "CommandDispatcher.h"
#include "DataManager.h"
#include "DroneDiscovery.h"
#include "DroneCommandRequest.h"
#include "FlightControl.h"
#include "FlightData.h"
#include "HoverTest.h"
#include "MavlinkParser.h"
#include "MavFtpClient.h"
#include "MissionUploadTransaction.h"
#include "UdpClient.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
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
    std::string remoteIp;
    int remotePort = 0;
    int heartbeatTimeoutMs = 3000;
    bool receiveOnly = false;
    bool realCommandEnabled = false;
    bool mavFtpServiceEnabled = false;
    int mavFtpRequestTimeoutMs = 750;
    std::uint32_t mavFtpRequestRetryCount = 3;
    bool missionUploadServiceEnabled = false;
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
class DroneSession : public dronegcs::mavftp::IRequestTransport,
                     public IMissionProtocolTransport
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
    bool ApplyDiscoveredFtpMessage(
        const mavlink_message_t& message,
        const DiscoveredDroneIdentity& identity);
    bool ApplyDiscoveredMissionMessage(
        const mavlink_message_t& message,
        const DiscoveredDroneIdentity& identity);
    bool SendMavFtpPayload(
        const std::array<std::uint8_t, dronegcs::mavftp::PayloadBytes>& payload) override;
    bool SendMissionRequestList();
    bool SendMissionCount(std::uint16_t count, std::size_t& wireBytes) override;
    bool SendMissionItemInt(
        const dronegcs::route_mission::MissionItemIntV1& item,
        std::size_t& wireBytes) override;
    bool SendMissionRequestList(std::size_t& wireBytes) override;
    bool SendMissionRequestInt(
        std::uint16_t sequence,
        std::size_t& wireBytes) override;
    void SetMissionProtocolMessageSink(IMissionProtocolMessageSink* sink);
    MissionProtocolPeerIdentity GetMissionProtocolPeerIdentity() const;
    dronegcs::mavftp::Client* GetMavFtpClient();
    const dronegcs::mavftp::Client* GetMavFtpClient() const;
    std::optional<std::uint16_t> ConsumeMissionCount();

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
    void ProcessDispatchResults(std::chrono::steady_clock::time_point now);

private:
    DroneSessionConfig config;
    DroneSessionState state = DroneSessionState::Stopped;

    UdpClient udp;
    DataManager dataManager;
    FlightData workingData;
    MavlinkParser parser;
    HoverTest hoverTest;
    CommandDispatcher dispatcher;
    FlightControl flightControl;

    bool started = false;
    bool requestSent = false;
    std::chrono::steady_clock::time_point lastGcsHeartbeat{};
    std::deque<CommandDispatchResult> completedCommandResults;
    std::unique_ptr<dronegcs::mavftp::Client> mavFtpClient;
    std::optional<std::uint16_t> receivedMissionCount;
    IMissionProtocolMessageSink* missionProtocolSink = nullptr;
};
