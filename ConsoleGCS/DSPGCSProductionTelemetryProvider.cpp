#include "DSPGCSProductionTelemetryProvider.h"

#include "DSPGCSProtocol.h"
#include "DSPGCSTransportFactory.h"
#include "DroneManager.h"
#include "DroneSession.h"

#include <chrono>

namespace
{
    std::int64_t UtcNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::int64_t ToUtcMilliseconds(
        const std::chrono::steady_clock::time_point steadyTimestamp)
    {
        if (steadyTimestamp == std::chrono::steady_clock::time_point{})
        {
            return 0;
        }
        const auto elapsed = std::chrono::steady_clock::now() - steadyTimestamp;
        return UtcNowMs() -
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    }

    dspgcs::TelemetryDTO ToTelemetryDTO(const DroneSession& session)
    {
        const FlightData data = session.GetFlightData();
        const DroneSessionConfig& config = session.GetConfig();

        dspgcs::TelemetryDTO dto;
        dto.droneId = session.GetDroneId();
        dto.sysId = data.systemID;
        dto.sampleTimestampUtcMs = UtcNowMs();
        dto.changedFields = dspgcs::TelemetryFields::All;

        dto.gps.valid = data.gpsValid;
        dto.gps.latitudeDeg = data.latitude;
        dto.gps.longitudeDeg = data.longitude;
        dto.gps.fixType = data.gpsFixType;
        dto.gps.satellitesVisible = data.satellitesVisible;

        dto.altitude.valid = data.altitudeValid;
        dto.altitude.relativeMeters = data.relativeAltitude;
        dto.altitude.amslMeters = data.altitude;

        dto.battery.valid = data.batteryRemaining >= 0;
        dto.battery.remainingPercent = data.batteryRemaining >= 0
            ? static_cast<float>(data.batteryRemaining)
            : 0.0f;

        dto.mode.modeCode = data.mode;
        dto.mode.modeName = data.modeName;
        dto.mode.armed = data.armed;

        dto.connection.online = data.online;
        dto.connection.state = data.online
            ? dspgcs::ConnectionState::Online
            : dspgcs::ConnectionState::Offline;
        dto.connection.lastHeartbeatUtcMs =
            ToUtcMilliseconds(data.lastHeartbeat);
        dto.connection.linkId = config.targetIp + ":" +
            std::to_string(config.targetPort);

        dto.rssi.valid = false;
        dto.led.state = dspgcs::LEDState::Unknown;
        dto.mission.state = dspgcs::MissionState::None;
        return dto;
    }
}

void DSPGCSProductionTelemetryProvider::SetReceiveHandler(
    dspgcs::TransportReceiveHandler handler)
{
    receiveHandler_ = std::move(handler);
}

bool DSPGCSProductionTelemetryProvider::Start(
    const dspgcs::DSPGCSTransportConfig& transportConfig,
    std::string& error)
{
    Stop();
    transport_ =
        dspgcs::DSPGCSTransportFactory::CreateTransport(transportConfig, error);
    if (!transport_)
    {
        return false;
    }

    if (receiveHandler_ && !transport_->Receive(receiveHandler_))
    {
        error = "DSP-GCS production receive handler registration failed";
        transport_.reset();
        return false;
    }

    if (!transport_->Start())
    {
        error = "DSP-GCS production telemetry transport failed to start";
        transport_.reset();
        return false;
    }

    sequence_ = 0;
    fleetRevision_ = 0;
    sessionId_ = "PRODUCTION-" + std::to_string(UtcNowMs());
    return true;
}

void DSPGCSProductionTelemetryProvider::Stop()
{
    if (transport_)
    {
        transport_->Stop();
        transport_.reset();
    }
}

bool DSPGCSProductionTelemetryProvider::PublishSnapshot(
    const DroneManager& droneManager)
{
    if (!IsRunning())
    {
        return false;
    }

    dspgcs::TelemetryBatchDTO batch;
    batch.batchId = "TELEMETRY-" + std::to_string(++sequence_);
    batch.batchMode = dspgcs::BatchMode::Snapshot;
    batch.fleetRevision = ++fleetRevision_;
    batch.snapshotId = batch.batchId;

    for (const DroneSession* session : droneManager.GetAllDrones())
    {
        dspgcs::TelemetryDTO dto = ToTelemetryDTO(*session);
        dto.telemetryRevision = fleetRevision_;
        batch.items.push_back(std::move(dto));
    }

    dspgcs::MessageEnvelope envelope;
    envelope.messageType = dspgcs::MessageType::TelemetryBatch;
    envelope.sourceId = "ConsoleGCS-ProductionTelemetry";
    envelope.sessionId = sessionId_;
    envelope.sequence = sequence_;
    envelope.timestampUtcMs = UtcNowMs();
    envelope.fleetRevision = fleetRevision_;

    return transport_->Send({ dspgcs::EncodeTelemetry(envelope, batch) });
}

bool DSPGCSProductionTelemetryProvider::SendCommandResult(
    const dspgcs::CommandResultBatchDTO& resultBatch)
{
    if (!IsRunning())
    {
        return false;
    }

    dspgcs::MessageEnvelope envelope;
    envelope.messageType = dspgcs::MessageType::CommandResultBatch;
    envelope.sourceId = "ConsoleGCS-ProductionCommand";
    envelope.sessionId = sessionId_;
    envelope.sequence = ++sequence_;
    envelope.timestampUtcMs = UtcNowMs();
    envelope.correlationId = resultBatch.commandId;
    envelope.fleetRevision = fleetRevision_;
    return transport_->Send(
        { dspgcs::EncodeCommandResult(envelope, resultBatch) });
}

bool DSPGCSProductionTelemetryProvider::IsRunning() const
{
    return transport_ &&
        transport_->GetState() == dspgcs::TransportState::Running;
}
