/**
 * @brief ConsoleGCS 与 UnrealGCS 共享的 DSP-GCS Wire Format 和 DTO 定义。
 *
 * 分类：Production。
 * 输入/输出：Telemetry Snapshot/Delta、Command Request/Result 的二进制编码与解码。
 * 关键约束：两端必须使用相同版本；注释整理不得改变字段顺序、类型或编码格式。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dspgcs
{
    inline constexpr std::uint16_t ProtocolMajor = 1;
    inline constexpr std::uint16_t ProtocolMinor = 0;
    // Schema 3 adds realtime selected-drone LED RGB command parameters.
    inline constexpr std::uint16_t SchemaVersion = 3;
    inline constexpr std::uint32_t FrameMagic = 0x44535047; // DSPG
    inline constexpr std::size_t MaxFrameBytes = 256 * 1024;
    inline constexpr std::size_t MaxStringBytes = 16 * 1024;
    inline constexpr std::size_t MaxCollectionItems = 20000;
    // Defensive reassembly ceiling. This protects memory use and is not a
    // deployment tuning value, so it intentionally remains non-configurable.
    inline constexpr std::uint32_t MaxSnapshotChunks = 1024;

    enum class MessageType : std::uint16_t
    {
        TelemetryBatch = 1,
        CommandRequest = 2,
        CommandResultBatch = 3
    };

    enum class BatchMode : std::uint8_t { Snapshot = 1, Delta = 2 };
    enum class CommandType : std::uint16_t
    {
        Arm = 1,
        Disarm = 2,
        Takeoff = 3,
        Land = 4,
        RTL = 5,
        SetMode = 6,
        HoverTest = 7,
        SetLedColor = 8
    };
    enum class AckState : std::uint8_t
    {
        Received = 1,
        Validated = 2,
        Queued = 3,
        Sent = 4,
        InProgress = 5,
        Accepted = 6,
        Rejected = 7,
        Failed = 8,
        TimedOut = 9,
        Cancelled = 10,
        Simulated = 11
    };
    enum class AggregateState : std::uint8_t
    {
        Received = 1,
        Validating = 2,
        Pending = 3,
        Executing = 4,
        Succeeded = 5,
        PartialSuccess = 6,
        Failed = 7,
        TimedOut = 8,
        Cancelled = 9,
        Simulated = 10
    };
    enum class ConnectionState : std::uint8_t
    {
        Offline = 0,
        Connecting = 1,
        Online = 2,
        Stale = 3,
        Lost = 4,
        Recovering = 5
    };
    enum class LEDState : std::uint8_t
    {
        Unknown = 0,
        Off = 1,
        Static = 2,
        Playing = 3,
        Fault = 4
    };
    enum class MissionState : std::uint8_t
    {
        None = 0,
        Ready = 1,
        Running = 2,
        Paused = 3,
        Completed = 4,
        Failed = 5,
        Aborted = 6
    };

    namespace TelemetryFields
    {
        inline constexpr std::uint64_t SysId = 1ull << 0;
        inline constexpr std::uint64_t GPS = 1ull << 1;
        inline constexpr std::uint64_t Altitude = 1ull << 2;
        inline constexpr std::uint64_t Battery = 1ull << 3;
        inline constexpr std::uint64_t Mode = 1ull << 4;
        inline constexpr std::uint64_t Connection = 1ull << 5;
        inline constexpr std::uint64_t RSSI = 1ull << 6;
        inline constexpr std::uint64_t LED = 1ull << 7;
        inline constexpr std::uint64_t Mission = 1ull << 8;
        inline constexpr std::uint64_t All = (1ull << 9) - 1;
    }

    struct MessageEnvelope
    {
        std::uint16_t protocolMajor = ProtocolMajor;
        std::uint16_t protocolMinor = ProtocolMinor;
        MessageType messageType = MessageType::TelemetryBatch;
        std::uint16_t schemaVersion = SchemaVersion;
        std::string sourceId;
        std::string sessionId;
        std::uint64_t sequence = 0;
        std::int64_t timestampUtcMs = 0;
        std::string correlationId;
        std::uint64_t fleetRevision = 0;
        std::uint32_t flags = 0;
    };

    struct GPSDTO
    {
        bool valid = false;
        double latitudeDeg = 0.0;
        double longitudeDeg = 0.0;
        std::uint8_t fixType = 0;
        std::uint8_t satellitesVisible = 0;
    };
    struct AltitudeDTO
    {
        bool valid = false;
        float relativeMeters = 0.0f;
        float amslMeters = 0.0f;
    };
    struct BatteryDTO
    {
        bool valid = false;
        float remainingPercent = 0.0f;
        float voltageVolts = 0.0f;
    };
    struct ModeDTO
    {
        std::int32_t modeCode = 0;
        std::string modeName;
        bool armed = false;
    };
    struct ConnectionDTO
    {
        bool online = false;
        ConnectionState state = ConnectionState::Offline;
        std::int64_t lastHeartbeatUtcMs = 0;
        std::string linkId;
    };
    struct RSSIDTO
    {
        bool valid = false;
        std::int16_t valueDbm = 0;
    };
    struct LEDStateDTO
    {
        LEDState state = LEDState::Unknown;
        std::uint8_t red = 0;
        std::uint8_t green = 0;
        std::uint8_t blue = 0;
        std::uint8_t brightness = 0;
        std::uint64_t showFrame = 0;
        bool confirmed = false;
    };
    struct MissionStateDTO
    {
        std::string missionId;
        MissionState state = MissionState::None;
        std::uint32_t currentItem = 0;
        std::uint16_t progressPermille = 0;
        std::int64_t showTimeMs = 0;
    };
    struct TelemetryDTO
    {
        std::string droneId;
        std::uint32_t sysId = 0;
        std::int64_t sampleTimestampUtcMs = 0;
        std::uint64_t telemetryRevision = 0;
        std::uint64_t changedFields = TelemetryFields::All;
        GPSDTO gps;
        AltitudeDTO altitude;
        BatteryDTO battery;
        ModeDTO mode;
        ConnectionDTO connection;
        RSSIDTO rssi;
        LEDStateDTO led;
        MissionStateDTO mission;
    };
    struct TelemetryBatchDTO
    {
        std::string batchId;
        BatchMode batchMode = BatchMode::Delta;
        std::uint64_t fleetRevision = 0;
        std::string snapshotId;
        std::uint32_t chunkIndex = 0;
        std::uint32_t chunkCount = 1;
        std::vector<TelemetryDTO> items;
    };

    struct CommandParametersDTO
    {
        float takeoffAltitudeMeters = 0.0f;
        float hoverAltitudeMeters = 0.0f;
        float hoverDurationSeconds = 0.0f;
        std::int32_t customMode = -1;
        std::string modeName;
        bool start = true;
        std::uint8_t red = 0;
        std::uint8_t green = 0;
        std::uint8_t blue = 0;
    };
    struct CommandSourceDTO
    {
        std::string sourceType;
        std::string sourceId;
        std::string operatorId;
        std::string stationId;
    };
    struct CommandRequestDTO
    {
        std::string commandId;
        CommandType commandType = CommandType::Arm;
        std::vector<std::string> targetDroneIds;
        CommandParametersDTO parameters;
        CommandSourceDTO source;
        std::int64_t timestampUtcMs = 0;
        std::uint64_t expectedFleetRevision = 0;
        bool safeTestMode = true;
    };

    struct ErrorDTO
    {
        std::string code;
        std::string message;
        bool retryable = false;
    };
    struct TimeoutDTO
    {
        std::string stage;
        std::uint32_t timeoutMs = 0;
        std::uint32_t elapsedMs = 0;
    };
    struct DroneCommandResultDTO
    {
        std::string commandId;
        std::string droneId;
        AckState ackState = AckState::Received;
        std::uint32_t mavlinkCommand = 0;
        std::uint32_t mavlinkAckResult = 0;
        std::uint16_t attempts = 0;
        ErrorDTO error;
        TimeoutDTO timeout;
        std::int64_t updatedTimestampUtcMs = 0;
        bool final = false;
    };
    struct CommandResultBatchDTO
    {
        std::string commandId;
        std::uint32_t resultSequence = 0;
        AggregateState aggregateState = AggregateState::Received;
        bool final = false;
        std::uint32_t requestedCount = 0;
        std::uint32_t acceptedCount = 0;
        std::uint32_t succeededCount = 0;
        std::uint32_t failedCount = 0;
        std::vector<DroneCommandResultDTO> results;
    };

    namespace detail
    {
        class Writer
        {
        public:
            void U8(std::uint8_t v) { data.push_back(v); }
            void Bool(bool v) { U8(v ? 1 : 0); }
            void U16(std::uint16_t v) { Integral(v); }
            void I16(std::int16_t v) { Integral(static_cast<std::uint16_t>(v)); }
            void U32(std::uint32_t v) { Integral(v); }
            void I32(std::int32_t v) { Integral(static_cast<std::uint32_t>(v)); }
            void U64(std::uint64_t v) { Integral(v); }
            void I64(std::int64_t v) { Integral(static_cast<std::uint64_t>(v)); }
            void Float(float v) { std::uint32_t bits; std::memcpy(&bits, &v, sizeof(bits)); U32(bits); }
            void Double(double v) { std::uint64_t bits; std::memcpy(&bits, &v, sizeof(bits)); U64(bits); }
            void String(const std::string& value)
            {
                U32(static_cast<std::uint32_t>(value.size()));
                data.insert(data.end(), value.begin(), value.end());
            }
            void Bytes(const std::vector<std::uint8_t>& value)
            {
                U32(static_cast<std::uint32_t>(value.size()));
                data.insert(data.end(), value.begin(), value.end());
            }
            std::vector<std::uint8_t> Take() { return std::move(data); }
        private:
            template<typename T> void Integral(T value)
            {
                for (int shift = static_cast<int>(sizeof(T) - 1) * 8; shift >= 0; shift -= 8)
                    data.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
            }
            std::vector<std::uint8_t> data;
        };

        class Reader
        {
        public:
            explicit Reader(const std::vector<std::uint8_t>& bytes) : data(bytes) {}
            bool U8(std::uint8_t& v) { if (!Need(1)) return false; v = data[offset++]; return true; }
            bool Bool(bool& v) { std::uint8_t b; if (!U8(b) || b > 1) return false; v = b != 0; return true; }
            bool U16(std::uint16_t& v) { return Integral(v); }
            bool I16(std::int16_t& v) { std::uint16_t x; if (!U16(x)) return false; v = static_cast<std::int16_t>(x); return true; }
            bool U32(std::uint32_t& v) { return Integral(v); }
            bool I32(std::int32_t& v) { std::uint32_t x; if (!U32(x)) return false; v = static_cast<std::int32_t>(x); return true; }
            bool U64(std::uint64_t& v) { return Integral(v); }
            bool I64(std::int64_t& v) { std::uint64_t x; if (!U64(x)) return false; v = static_cast<std::int64_t>(x); return true; }
            bool Float(float& v) { std::uint32_t bits; if (!U32(bits)) return false; std::memcpy(&v, &bits, sizeof(v)); return true; }
            bool Double(double& v) { std::uint64_t bits; if (!U64(bits)) return false; std::memcpy(&v, &bits, sizeof(v)); return true; }
            bool String(std::string& value)
            {
                std::uint32_t length; if (!U32(length) || length > MaxStringBytes || !Need(length)) return false;
                value.assign(reinterpret_cast<const char*>(data.data() + offset), length); offset += length; return true;
            }
            bool Bytes(std::vector<std::uint8_t>& value)
            {
                std::uint32_t length; if (!U32(length) || length > MaxFrameBytes || !Need(length)) return false;
                value.assign(data.begin() + offset, data.begin() + offset + length); offset += length; return true;
            }
            bool Done() const { return offset == data.size(); }
        private:
            bool Need(std::size_t count) const { return count <= data.size() - offset; }
            template<typename T> bool Integral(T& value)
            {
                if (!Need(sizeof(T))) return false; value = 0;
                for (std::size_t i = 0; i < sizeof(T); ++i) value = static_cast<T>((value << 8) | data[offset++]);
                return true;
            }
            const std::vector<std::uint8_t>& data;
            std::size_t offset = 0;
        };

        inline void Envelope(Writer& w, const MessageEnvelope& e, const std::vector<std::uint8_t>& payload)
        {
            w.U32(FrameMagic); w.U16(e.protocolMajor); w.U16(e.protocolMinor); w.U16(static_cast<std::uint16_t>(e.messageType));
            w.U16(e.schemaVersion); w.String(e.sourceId); w.String(e.sessionId); w.U64(e.sequence); w.I64(e.timestampUtcMs);
            w.String(e.correlationId); w.U64(e.fleetRevision); w.U32(e.flags); w.Bytes(payload);
        }
        inline bool Envelope(Reader& r, MessageEnvelope& e, std::vector<std::uint8_t>& payload)
        {
            std::uint32_t magic; std::uint16_t type;
            if (!r.U32(magic) || magic != FrameMagic || !r.U16(e.protocolMajor) || !r.U16(e.protocolMinor) ||
                !r.U16(type) || !r.U16(e.schemaVersion) || !r.String(e.sourceId) || !r.String(e.sessionId) ||
                !r.U64(e.sequence) || !r.I64(e.timestampUtcMs) || !r.String(e.correlationId) ||
                !r.U64(e.fleetRevision) || !r.U32(e.flags) || !r.Bytes(payload) || !r.Done()) return false;
            e.messageType = static_cast<MessageType>(type);
            return e.protocolMajor == ProtocolMajor && e.schemaVersion == SchemaVersion;
        }
        inline void Telemetry(Writer& w, const TelemetryDTO& t)
        {
            w.String(t.droneId); w.U32(t.sysId); w.I64(t.sampleTimestampUtcMs); w.U64(t.telemetryRevision); w.U64(t.changedFields);
            w.Bool(t.gps.valid); w.Double(t.gps.latitudeDeg); w.Double(t.gps.longitudeDeg); w.U8(t.gps.fixType); w.U8(t.gps.satellitesVisible);
            w.Bool(t.altitude.valid); w.Float(t.altitude.relativeMeters); w.Float(t.altitude.amslMeters);
            w.Bool(t.battery.valid); w.Float(t.battery.remainingPercent); w.Float(t.battery.voltageVolts);
            w.I32(t.mode.modeCode); w.String(t.mode.modeName); w.Bool(t.mode.armed);
            w.Bool(t.connection.online); w.U8(static_cast<std::uint8_t>(t.connection.state)); w.I64(t.connection.lastHeartbeatUtcMs); w.String(t.connection.linkId);
            w.Bool(t.rssi.valid); w.I16(t.rssi.valueDbm);
            w.U8(static_cast<std::uint8_t>(t.led.state)); w.U8(t.led.red); w.U8(t.led.green); w.U8(t.led.blue); w.U8(t.led.brightness); w.U64(t.led.showFrame); w.Bool(t.led.confirmed);
            w.String(t.mission.missionId); w.U8(static_cast<std::uint8_t>(t.mission.state)); w.U32(t.mission.currentItem); w.U16(t.mission.progressPermille); w.I64(t.mission.showTimeMs);
        }
        inline bool Telemetry(Reader& r, TelemetryDTO& t)
        {
            std::uint8_t connectionState, ledState, missionState;
            if (!r.String(t.droneId) || t.droneId.empty() || !r.U32(t.sysId) || !r.I64(t.sampleTimestampUtcMs) || !r.U64(t.telemetryRevision) || !r.U64(t.changedFields) ||
                !r.Bool(t.gps.valid) || !r.Double(t.gps.latitudeDeg) || !r.Double(t.gps.longitudeDeg) || !r.U8(t.gps.fixType) || !r.U8(t.gps.satellitesVisible) ||
                !r.Bool(t.altitude.valid) || !r.Float(t.altitude.relativeMeters) || !r.Float(t.altitude.amslMeters) ||
                !r.Bool(t.battery.valid) || !r.Float(t.battery.remainingPercent) || !r.Float(t.battery.voltageVolts) ||
                !r.I32(t.mode.modeCode) || !r.String(t.mode.modeName) || !r.Bool(t.mode.armed) ||
                !r.Bool(t.connection.online) || !r.U8(connectionState) || !r.I64(t.connection.lastHeartbeatUtcMs) || !r.String(t.connection.linkId) ||
                !r.Bool(t.rssi.valid) || !r.I16(t.rssi.valueDbm) || !r.U8(ledState) || !r.U8(t.led.red) || !r.U8(t.led.green) || !r.U8(t.led.blue) || !r.U8(t.led.brightness) || !r.U64(t.led.showFrame) || !r.Bool(t.led.confirmed) ||
                !r.String(t.mission.missionId) || !r.U8(missionState) || !r.U32(t.mission.currentItem) || !r.U16(t.mission.progressPermille) || !r.I64(t.mission.showTimeMs)) return false;
            t.connection.state = static_cast<ConnectionState>(connectionState); t.led.state = static_cast<LEDState>(ledState); t.mission.state = static_cast<MissionState>(missionState); return true;
        }
        inline void Command(Writer& w, const CommandRequestDTO& c)
        {
            w.String(c.commandId); w.U16(static_cast<std::uint16_t>(c.commandType)); w.U32(static_cast<std::uint32_t>(c.targetDroneIds.size()));
            for (const auto& id : c.targetDroneIds) w.String(id);
            w.Float(c.parameters.takeoffAltitudeMeters); w.Float(c.parameters.hoverAltitudeMeters); w.Float(c.parameters.hoverDurationSeconds);
            w.I32(c.parameters.customMode); w.String(c.parameters.modeName); w.Bool(c.parameters.start);
            w.U8(c.parameters.red); w.U8(c.parameters.green); w.U8(c.parameters.blue);
            w.String(c.source.sourceType); w.String(c.source.sourceId); w.String(c.source.operatorId); w.String(c.source.stationId);
            w.I64(c.timestampUtcMs); w.U64(c.expectedFleetRevision); w.Bool(c.safeTestMode);
        }
        inline bool Command(Reader& r, CommandRequestDTO& c)
        {
            std::uint16_t type; std::uint32_t count;
            if (!r.String(c.commandId) || c.commandId.empty() || !r.U16(type) || !r.U32(count) || count == 0 || count > MaxCollectionItems) return false;
            c.commandType = static_cast<CommandType>(type); c.targetDroneIds.resize(count);
            for (auto& id : c.targetDroneIds) if (!r.String(id) || id.empty()) return false;
            return r.Float(c.parameters.takeoffAltitudeMeters) && r.Float(c.parameters.hoverAltitudeMeters) && r.Float(c.parameters.hoverDurationSeconds) &&
                r.I32(c.parameters.customMode) && r.String(c.parameters.modeName) && r.Bool(c.parameters.start) &&
                r.U8(c.parameters.red) && r.U8(c.parameters.green) && r.U8(c.parameters.blue) &&
                r.String(c.source.sourceType) && r.String(c.source.sourceId) && r.String(c.source.operatorId) && r.String(c.source.stationId) &&
                r.I64(c.timestampUtcMs) && r.U64(c.expectedFleetRevision) && r.Bool(c.safeTestMode) && r.Done();
        }
        inline void Result(Writer& w, const DroneCommandResultDTO& d)
        {
            w.String(d.commandId); w.String(d.droneId); w.U8(static_cast<std::uint8_t>(d.ackState)); w.U32(d.mavlinkCommand); w.U32(d.mavlinkAckResult); w.U16(d.attempts);
            w.String(d.error.code); w.String(d.error.message); w.Bool(d.error.retryable); w.String(d.timeout.stage); w.U32(d.timeout.timeoutMs); w.U32(d.timeout.elapsedMs); w.I64(d.updatedTimestampUtcMs); w.Bool(d.final);
        }
        inline bool Result(Reader& r, DroneCommandResultDTO& d)
        {
            std::uint8_t state;
            if (!r.String(d.commandId) || !r.String(d.droneId) || !r.U8(state) || !r.U32(d.mavlinkCommand) || !r.U32(d.mavlinkAckResult) || !r.U16(d.attempts) ||
                !r.String(d.error.code) || !r.String(d.error.message) || !r.Bool(d.error.retryable) || !r.String(d.timeout.stage) || !r.U32(d.timeout.timeoutMs) || !r.U32(d.timeout.elapsedMs) || !r.I64(d.updatedTimestampUtcMs) || !r.Bool(d.final)) return false;
            d.ackState = static_cast<AckState>(state); return true;
        }
    }

    inline bool DecodeEnvelope(const std::vector<std::uint8_t>& frame, MessageEnvelope& envelope, std::vector<std::uint8_t>& payload)
    {
        if (frame.empty() || frame.size() > MaxFrameBytes) return false;
        detail::Reader reader(frame); return detail::Envelope(reader, envelope, payload);
    }
    inline std::vector<std::uint8_t> MakeFrame(const MessageEnvelope& envelope, const std::vector<std::uint8_t>& payload)
    {
        detail::Writer writer; detail::Envelope(writer, envelope, payload); return writer.Take();
    }
    inline std::vector<std::uint8_t> EncodeTelemetry(const MessageEnvelope& envelope, const TelemetryBatchDTO& batch)
    {
        detail::Writer p; p.String(batch.batchId); p.U8(static_cast<std::uint8_t>(batch.batchMode)); p.U64(batch.fleetRevision); p.String(batch.snapshotId); p.U32(batch.chunkIndex); p.U32(batch.chunkCount); p.U32(static_cast<std::uint32_t>(batch.items.size()));
        for (const auto& item : batch.items) detail::Telemetry(p, item);
        return MakeFrame(envelope, p.Take());
    }
    inline bool DecodeTelemetry(const std::vector<std::uint8_t>& payload, TelemetryBatchDTO& batch)
    {
        detail::Reader r(payload); std::uint8_t mode; std::uint32_t count;
        if (!r.String(batch.batchId) || !r.U8(mode) || !r.U64(batch.fleetRevision) || !r.String(batch.snapshotId) || !r.U32(batch.chunkIndex) || !r.U32(batch.chunkCount) || !r.U32(count) || count > MaxCollectionItems) return false;
        batch.batchMode = static_cast<BatchMode>(mode); batch.items.resize(count);
        for (auto& item : batch.items) if (!detail::Telemetry(r, item)) return false;
        return r.Done();
    }
    inline std::vector<std::uint8_t> EncodeCommand(const MessageEnvelope& envelope, const CommandRequestDTO& command)
    {
        detail::Writer p; detail::Command(p, command); return MakeFrame(envelope, p.Take());
    }
    inline bool DecodeCommand(const std::vector<std::uint8_t>& payload, CommandRequestDTO& command)
    {
        detail::Reader r(payload); return detail::Command(r, command);
    }
    inline std::vector<std::uint8_t> EncodeCommandResult(const MessageEnvelope& envelope, const CommandResultBatchDTO& batch)
    {
        detail::Writer p; p.String(batch.commandId); p.U32(batch.resultSequence); p.U8(static_cast<std::uint8_t>(batch.aggregateState)); p.Bool(batch.final); p.U32(batch.requestedCount); p.U32(batch.acceptedCount); p.U32(batch.succeededCount); p.U32(batch.failedCount); p.U32(static_cast<std::uint32_t>(batch.results.size()));
        for (const auto& item : batch.results) detail::Result(p, item);
        return MakeFrame(envelope, p.Take());
    }
    inline bool DecodeCommandResult(const std::vector<std::uint8_t>& payload, CommandResultBatchDTO& batch)
    {
        detail::Reader r(payload); std::uint8_t aggregate; std::uint32_t count;
        if (!r.String(batch.commandId) || !r.U32(batch.resultSequence) || !r.U8(aggregate) || !r.Bool(batch.final) || !r.U32(batch.requestedCount) || !r.U32(batch.acceptedCount) || !r.U32(batch.succeededCount) || !r.U32(batch.failedCount) || !r.U32(count) || count > MaxCollectionItems) return false;
        batch.aggregateState = static_cast<AggregateState>(aggregate); batch.results.resize(count);
        for (auto& item : batch.results) if (!detail::Result(r, item)) return false;
        return r.Done();
    }
}
