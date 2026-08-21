#pragma once

#include "MissionTransport.h"
#include "MissionUploadTransaction.h"

#include <memory>

/** Debug/test fallback that preserves the proven standard Mission Protocol. */
class MissionProtocolTransport final : public IMissionTransport,
    public IMissionProtocolMessageSink
{
public:
    MissionProtocolTransport(
        IMissionProtocolTransport& wireTransport,
        MissionProtocolPeerIdentity peer,
        MissionUploadTransactionConfig config = {});

    bool Prepare(
        MissionTransportIdentity identity,
        std::vector<dronegcs::route_mission::MissionItemIntV1> mission,
        std::string& outDiagnostic) override;
    bool Upload(std::chrono::steady_clock::time_point now) override;
    bool Verify() const override;
    void Tick(std::chrono::steady_clock::time_point now) override;
    void Cancel(std::chrono::steady_clock::time_point now) override;

    MissionTransportStatus GetStatus() const override;
    bool IsTerminal() const override;
    bool ConsumeProgressChanged() override;
    MissionTransportMetrics GetMetrics() const override;
    const std::string& GetSafeDiagnostic() const override;
    const std::string& GetVerificationMismatchDiagnostic() const override;
    const char* GetTerminalPhaseName() const override;
    IMissionProtocolMessageSink* GetMissionProtocolMessageSink() override
        { return this; }

    bool HandleMissionMessage(
        const mavlink_message_t& message,
        const MissionProtocolPeerIdentity& peer,
        std::chrono::steady_clock::time_point now) override;

private:
    IMissionProtocolTransport& wireTransport_;
    MissionProtocolPeerIdentity peer_;
    MissionUploadTransactionConfig config_;
    std::unique_ptr<MissionUploadTransaction> transaction_;
    bool prepared_ = false;
    static const std::string EmptyDiagnostic;
};

std::unique_ptr<IMissionTransport> CreateMissionProtocolTransport(
    IMissionProtocolTransport& wireTransport,
    MissionProtocolPeerIdentity peer,
    MissionUploadTransactionConfig config = {});
