#pragma once

#include "RouteMissionCodec.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class IMissionVerificationMessageSink;

enum class MissionTransportStatus : std::uint8_t
{
    Idle,
    Prepared,
    Uploading,
    WaitingAck,
    Verifying,
    Succeeded,
    Failed,
    Cancelled
};

struct MissionTransportIdentity
{
    std::string uploadJobId;
    std::string droneId;
    std::int32_t routeSlotIndex = -1;
    std::uint64_t generation = 0;
};

struct MissionTransportMetrics
{
    std::uint64_t logicalBytesCompleted = 0;
    std::uint64_t artifactRouteItems = 0;
    std::uint64_t wireMissionItems = 0;
    std::uint64_t uniqueItemsServed = 0;
    std::uint64_t wireBytesSent = 0;
    std::uint64_t missionMessagesSent = 0;
    std::uint64_t missionRequestsReceived = 0;
    std::uint64_t duplicateRequests = 0;
    std::uint64_t retryCount = 0;
    std::uint64_t uploadDurationMs = 0;
    std::uint64_t verificationDurationMs = 0;
};

/**
 * Per-target Mission transfer lifecycle. The interface owns no Route, UE, or
 * Fleet authority and deliberately does not select a production protocol.
 */
class IMissionTransport
{
public:
    virtual ~IMissionTransport() = default;

    virtual bool Prepare(
        MissionTransportIdentity identity,
        std::vector<dronegcs::route_mission::MissionItemIntV1> mission,
        std::string& outDiagnostic) = 0;
    virtual bool Upload(std::chrono::steady_clock::time_point now) = 0;

    // Verification is protocol-driven; this reports its authoritative result.
    virtual bool Verify() const = 0;
    virtual void Tick(std::chrono::steady_clock::time_point now) = 0;
    virtual void Cancel(std::chrono::steady_clock::time_point now) = 0;

    virtual MissionTransportStatus GetStatus() const = 0;
    virtual bool IsTerminal() const = 0;
    virtual bool ConsumeProgressChanged() = 0;
    virtual MissionTransportMetrics GetMetrics() const = 0;
    virtual const std::string& GetSafeDiagnostic() const = 0;
    virtual const std::string& GetVerificationMismatchDiagnostic() const = 0;
    virtual const char* GetTerminalPhaseName() const = 0;

    // Optional ingress hook for post-transfer Mission count/item verification.
    virtual IMissionVerificationMessageSink* GetMissionVerificationMessageSink() = 0;
};
