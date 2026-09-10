/**
 * @brief 实现 Console 到 UE 的 Production Snapshot 分片与命令结果发送。
 *
 * 分类：Production。
 * 输入：稳定 SessionOrder 下的 Fleet；输出：带 Session/Snapshot/Revision 的 DSP-GCS Frame。
 * 当前同一 Transport 也接收 UE 命令，因此并非单向遥测专用 Socket。
 */
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
        // Existing wire field now carries the true Console GPS_RAW_INT receive
        // time. Re-publishing the same sample preserves this identity.
        dto.sampleTimestampUtcMs = data.lastGpsReceiveUtcMs;
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
        dto.connection.linkId = config.remoteIp + ":" +
            std::to_string(config.remotePort);

        dto.rssi.valid = false;
        dto.led.state = dspgcs::LEDState::Unknown;
        dto.mission.state = dspgcs::MissionState::None;

        const MDHKStatusData& status = data.mdhkStatus;
        dto.mdhkStatus.received = status.received;
        dto.mdhkStatus.sampleTimestampUtcMs = status.lastReceiveUtcMs;
        dto.mdhkStatus.timeBootMs = status.timeBootMs;
        dto.mdhkStatus.gpsYear = status.gpsYear;
        dto.mdhkStatus.gpsMonth = status.gpsMonth;
        dto.mdhkStatus.gpsDay = status.gpsDay;
        dto.mdhkStatus.gpsHour = status.gpsHour;
        dto.mdhkStatus.gpsMinute = status.gpsMinute;
        dto.mdhkStatus.gpsSecond = status.gpsSecond;
        dto.mdhkStatus.validFlags = status.validFlags;
        dto.mdhkStatus.schemaVersion = status.schemaVersion;
        dto.mdhkStatus.batteryRemainingValid = status.batteryRemainingValid;
        dto.mdhkStatus.batteryRemainingPercent = status.batteryRemainingPercent;
        dto.mdhkStatus.batteryTemperatureValid = status.batteryTemperatureValid;
        dto.mdhkStatus.batteryTemperatureCelsius = status.batteryTemperatureCelsius;
        dto.mdhkStatus.cell1Valid = status.cellVoltageValid[0];
        dto.mdhkStatus.cell1Volts = status.cellVoltageVolts[0];
        dto.mdhkStatus.cell2Valid = status.cellVoltageValid[1];
        dto.mdhkStatus.cell2Volts = status.cellVoltageVolts[1];
        dto.mdhkStatus.cell3Valid = status.cellVoltageValid[2];
        dto.mdhkStatus.cell3Volts = status.cellVoltageVolts[2];
        dto.mdhkStatus.cell4Valid = status.cellVoltageValid[3];
        dto.mdhkStatus.cell4Volts = status.cellVoltageVolts[3];
        dto.mdhkStatus.gpsValid = status.gpsValid;
        dto.mdhkStatus.latitudeDeg = status.latitudeDeg;
        dto.mdhkStatus.longitudeDeg = status.longitudeDeg;
        dto.mdhkStatus.altitudeValid = status.altitudeValid;
        dto.mdhkStatus.gpsAltitudeAmslMeters = status.gpsAltitudeAmslMeters;
        dto.mdhkStatus.relativeAltitudeMeters = status.relativeAltitudeMeters;
        dto.mdhkStatus.freeMemoryBytes = status.freeMemoryBytes;
        dto.mdhkStatus.flightSoftwareVersion = status.flightSoftwareVersion;
        dto.mdhkStatus.waypointDistanceMeters = status.waypointDistanceMeters;
        dto.mdhkStatus.compassVariance = status.compassVariance;
        dto.mdhkStatus.magneticField1Mgauss = status.magneticField1Mgauss;
        dto.mdhkStatus.magneticField2Mgauss = status.magneticField2Mgauss;
        dto.mdhkStatus.batteryCycleCount = status.batteryCycleCount;
        dto.mdhkStatus.cellVoltageDeltaMv = status.cellVoltageDeltaMv;
        dto.mdhkStatus.ekfFlags = status.ekfFlags;
        dto.mdhkStatus.missionSequence = status.missionSequence;
        dto.mdhkStatus.missionTotal = status.missionTotal;
        dto.mdhkStatus.batteryCurrentCentiamps = status.batteryCurrentCentiamps;
        dto.mdhkStatus.gpsFixType = status.gpsFixType;
        dto.mdhkStatus.satellitesVisible = status.satellitesVisible;
        dto.mdhkStatus.red = status.red;
        dto.mdhkStatus.green = status.green;
        dto.mdhkStatus.blue = status.blue;
        // MDHK_STATUS (#187) is the flight controller's final logical LED
        // output. Publish confirmed feedback through the protocol's standard
        // LED channel so UE renders the real aircraft state instead of an NPZ
        // preview color.
        if (status.received)
        {
            dto.led.state = dspgcs::LEDState::Static;
            dto.led.red = status.red;
            dto.led.green = status.green;
            dto.led.blue = status.blue;
            dto.led.brightness = 255;
            dto.led.confirmed = true;
        }
        dto.mdhkStatus.flightCustomVersion.assign(
            status.flightCustomVersion.begin(), status.flightCustomVersion.end());
        dto.mdhkStatus.hardwareId = status.hardwareId;
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
    const std::size_t maxTelemetryDatagramBytes,
    const std::size_t maxTelemetryChunkItems,
    std::string& error)
{
    Stop();
    if (maxTelemetryDatagramBytes == 0 ||
        maxTelemetryDatagramBytes >
            dspgcs::DSPGCSDefaults::MaxUdpPayloadBytes ||
        maxTelemetryChunkItems == 0 ||
        maxTelemetryChunkItems > dspgcs::MaxCollectionItems)
    {
        error = "Invalid DSP-GCS snapshot performance configuration";
        return false;
    }

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
    maxTelemetryDatagramBytes_ = maxTelemetryDatagramBytes;
    maxTelemetryChunkItems_ = maxTelemetryChunkItems;
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
    items.reserve(droneManager.GetDroneCount());

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

    // 1000 架全量 Snapshot 不能假定由单个 UDP Datagram 承载；必须先形成
    // 有共同 SnapshotID/Revision 的分片集合，再逐片交给 DSP-GCS Transport。
    std::vector<std::vector<dspgcs::TelemetryDTO>> chunks;
    chunks.emplace_back();
    std::size_t currentFrameBytes = fixedFrameBytes;
    for (dspgcs::TelemetryDTO& item : items)
    {
        dspgcs::detail::Writer itemSizer;
        dspgcs::detail::Telemetry(itemSizer, item);
        const std::size_t itemBytes = itemSizer.Take().size();

        if (!chunks.back().empty() &&
            (chunks.back().size() >= maxTelemetryChunkItems_ ||
             currentFrameBytes + itemBytes > maxTelemetryDatagramBytes_))
        {
            chunks.emplace_back();
            currentFrameBytes = fixedFrameBytes;
        }

        // A single DTO larger than the safe datagram size cannot be sent by
        // this UDP transport; fail without emitting a partial snapshot.
        if (currentFrameBytes + itemBytes > maxTelemetryDatagramBytes_)
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
        if (frame.size() > maxTelemetryDatagramBytes_ ||
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

bool DSPGCSProductionTelemetryProvider::SendFrozenAssignmentAck(
    const dspgcs::FrozenAssignmentAckDTO& acknowledgement)
{
    if (!IsRunning())
    {
        return false;
    }

    std::vector<std::uint8_t> payload;
    std::string error;
    if (!dspgcs::EncodeFrozenAssignmentAck(
            acknowledgement, payload, error))
    {
        return false;
    }

    dspgcs::MessageEnvelope envelope;
    envelope.messageType = dspgcs::FrozenAssignmentMessageTypes::Ack;
    envelope.sourceId = "ConsoleGCS-FrozenAssignment";
    envelope.sessionId = sessionId_;
    envelope.sequence = ++sequence_;
    envelope.timestampUtcMs = UtcNowMs();
    envelope.correlationId = acknowledgement.transactionId;
    envelope.fleetRevision = fleetRevision_;
    return transport_->Send(
        { dspgcs::MakeFrame(envelope, payload) });
}

bool DSPGCSProductionTelemetryProvider::SendUploadJobPayload(
    const dspgcs::MessageType messageType,
    const std::string& correlationId,
    std::vector<std::uint8_t> payload)
{
    if (!IsRunning()) return false;
    dspgcs::MessageEnvelope envelope;
    envelope.messageType = messageType;
    envelope.sourceId = "ConsoleGCS-UploadJob";
    envelope.sessionId = sessionId_;
    envelope.sequence = ++sequence_;
    envelope.timestampUtcMs = UtcNowMs();
    envelope.correlationId = correlationId;
    envelope.fleetRevision = fleetRevision_;
    std::vector<std::uint8_t> frame = dspgcs::MakeFrame(envelope, payload);
    return frame.size() <= dspgcs::UploadJobDefaultDatagramBudgetBytes &&
        transport_->Send({ std::move(frame) });
}

bool DSPGCSProductionTelemetryProvider::SendUploadJobCommandAck(
    const dspgcs::UploadJobCommandAckDTO& acknowledgement)
{
    std::vector<std::uint8_t> payload; std::string error;
    return dspgcs::EncodeUploadJobCommandAck(acknowledgement, payload, error) &&
        SendUploadJobPayload(dspgcs::UploadJobMessageTypes::CommandAck,
            acknowledgement.clientCommandId, std::move(payload));
}

bool DSPGCSProductionTelemetryProvider::SendUploadJobSnapshot(
    const dspgcs::UploadJobSnapshotDTO& snapshot)
{
    std::vector<std::uint8_t> payload; std::string error;
    return dspgcs::EncodeUploadJobSnapshot(snapshot, payload, error) &&
        SendUploadJobPayload(dspgcs::UploadJobMessageTypes::Snapshot,
            snapshot.clientRequestId, std::move(payload));
}

bool DSPGCSProductionTelemetryProvider::SendUploadJobRouteStatePage(
    const dspgcs::UploadJobRouteStatePageDTO& page)
{
    std::vector<std::uint8_t> payload; std::string error;
    return dspgcs::EncodeUploadJobRouteStatePage(page, payload, error) &&
        SendUploadJobPayload(dspgcs::UploadJobMessageTypes::RouteStatePage,
            page.clientRequestId, std::move(payload));
}

bool DSPGCSProductionTelemetryProvider::SendUploadJobRouteStateDelta(
    const dspgcs::UploadJobRouteStateDeltaDTO& delta)
{
    std::vector<std::uint8_t> payload; std::string error;
    return dspgcs::EncodeUploadJobRouteStateDelta(delta, payload, error) &&
        SendUploadJobPayload(dspgcs::UploadJobMessageTypes::RouteStateDelta,
            delta.uploadJobId, std::move(payload));
}

bool DSPGCSProductionTelemetryProvider::SendMavFtpRuntimeConfigAck(
    const dspgcs::MavFtpRuntimeConfigAckDTO& acknowledgement)
{
    std::vector<std::uint8_t> payload;
    std::string error;
    return dspgcs::EncodeMavFtpRuntimeConfigAck(
            acknowledgement, payload, error) &&
        SendUploadJobPayload(dspgcs::MavFtpRuntimeConfigMessageTypes::Ack,
            acknowledgement.clientRequestId, std::move(payload));
}

bool DSPGCSProductionTelemetryProvider::SendMavFtpRuntimeConfigSnapshot(
    const dspgcs::MavFtpRuntimeConfigSnapshotDTO& snapshot)
{
    std::vector<std::uint8_t> payload;
    std::string error;
    return dspgcs::EncodeMavFtpRuntimeConfigSnapshot(snapshot, payload, error) &&
        SendUploadJobPayload(dspgcs::MavFtpRuntimeConfigMessageTypes::Snapshot,
            snapshot.clientRequestId.empty()
                ? snapshot.authoritySessionId : snapshot.clientRequestId,
            std::move(payload));
}

const std::string&
DSPGCSProductionTelemetryProvider::GetCurrentFleetSessionID() const
{
    return sessionId_;
}

bool DSPGCSProductionTelemetryProvider::IsRunning() const
{
    return transport_ &&
        transport_->GetState() == dspgcs::TransportState::Running;
}
