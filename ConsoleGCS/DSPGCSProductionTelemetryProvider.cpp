#include "DSPGCSProductionTelemetryProvider.h"

#include "DSPGCSProtocol.h"
#include "DSPGCSTransportFactory.h"
#include "DroneManager.h"
#include "DroneSession.h"

#include <chrono>

namespace
{
    // Leave headroom below the UDP payload ceiling (65,507 bytes) for the
    // encoded DSP-GCS envelope and for platform-specific transport handling.
    constexpr std::size_t MaxTelemetryDatagramBytes = 60 * 1024;
    constexpr std::size_t MaxTelemetryChunkItems = 512;

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

    const std::uint64_t fleetRevision = ++fleetRevision_;
    const std::string snapshotId =
        "TELEMETRY-" + std::to_string(fleetRevision);
    std::vector<dspgcs::TelemetryDTO> items;

    for (const DroneSession* session : droneManager.GetAllDrones())
    {
        dspgcs::TelemetryDTO dto = ToTelemetryDTO(*session);
        dto.telemetryRevision = fleetRevision;
        items.push_back(std::move(dto));
    }

    auto MakeBatch = [&snapshotId, fleetRevision]()
    {
        dspgcs::TelemetryBatchDTO batch;
        batch.batchId = snapshotId;
        batch.batchMode = dspgcs::BatchMode::Snapshot;
        batch.fleetRevision = fleetRevision;
        batch.snapshotId = snapshotId;
        return batch;
    };
    auto MakeEnvelope = [this, fleetRevision]()
    {
        dspgcs::MessageEnvelope envelope;
        envelope.messageType = dspgcs::MessageType::TelemetryBatch;
        envelope.sourceId = "ConsoleGCS-ProductionTelemetry";
        envelope.sessionId = sessionId_;
        envelope.timestampUtcMs = UtcNowMs();
        envelope.fleetRevision = fleetRevision;
        return envelope;
    };

    // Encoding is additive per TelemetryDTO. Measure each item once and use
    // the empty-frame size as exact fixed overhead, avoiding repeated trial
    // serialization while still guaranteeing a UDP-safe frame size.
    dspgcs::TelemetryBatchDTO emptyBatch = MakeBatch();
    dspgcs::MessageEnvelope sizingEnvelope = MakeEnvelope();
    const std::size_t fixedFrameBytes =
        dspgcs::EncodeTelemetry(sizingEnvelope, emptyBatch).size();

    std::vector<std::vector<dspgcs::TelemetryDTO>> chunks;
    chunks.emplace_back();
    std::size_t currentFrameBytes = fixedFrameBytes;
    for (dspgcs::TelemetryDTO& item : items)
    {
        dspgcs::TelemetryBatchDTO oneItemBatch = MakeBatch();
        oneItemBatch.items.push_back(item);
        const std::size_t itemBytes =
            dspgcs::EncodeTelemetry(sizingEnvelope, oneItemBatch).size() -
            fixedFrameBytes;

        if (!chunks.back().empty() &&
            (chunks.back().size() >= MaxTelemetryChunkItems ||
             currentFrameBytes + itemBytes > MaxTelemetryDatagramBytes))
        {
            chunks.emplace_back();
            currentFrameBytes = fixedFrameBytes;
        }

        // A single DTO larger than the safe datagram size cannot be sent by
        // this UDP transport; fail without emitting a partial snapshot.
        if (currentFrameBytes + itemBytes > MaxTelemetryDatagramBytes)
        {
            return false;
        }
        currentFrameBytes += itemBytes;
        chunks.back().push_back(std::move(item));
    }

    const std::uint32_t chunkCount =
        static_cast<std::uint32_t>(chunks.size());
    for (std::uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
    {
        dspgcs::TelemetryBatchDTO batch = MakeBatch();
        batch.chunkIndex = chunkIndex;
        batch.chunkCount = chunkCount;
        batch.items = std::move(chunks[chunkIndex]);

        dspgcs::MessageEnvelope envelope = MakeEnvelope();
        envelope.sequence = ++sequence_;
        std::vector<std::uint8_t> frame =
            dspgcs::EncodeTelemetry(envelope, batch);
        if (frame.size() > MaxTelemetryDatagramBytes ||
            !transport_->Send({ std::move(frame) }))
        {
            return false;
        }
    }

    return true;
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
