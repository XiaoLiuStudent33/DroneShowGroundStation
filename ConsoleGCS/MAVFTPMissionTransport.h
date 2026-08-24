#pragma once

#include "MavFtpClient.h"
#include "MissionFtpCompiler.h"
#include "MissionTransport.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dronegcs::mission_ftp
{
    class ISegmentRangeSource
    {
    public:
        virtual ~ISegmentRangeSource() = default;

        virtual bool ReadRange(
            const CompiledMissionSnapshot& snapshot,
            const SegmentDescriptor& segment,
            std::size_t offset,
            std::uint8_t* destination,
            std::size_t size,
            std::string& error) const = 0;
    };

    struct MAVFTPMissionTransportConfig
    {
        CompileConfig compileConfig{
            65535U,
            VerificationPolicy::ProductionEndpoints };
        std::size_t writeBlockBytes = mavftp::MaximumDataBytes;
    };

    struct MAVFTPMissionTransportMetrics
    {
        std::uint64_t createDurationMs = 0;
        std::uint64_t writeBytes = 0;
        std::uint64_t writeBlocks = 0;
        std::uint64_t retryCount = 0;
        std::uint64_t terminateDurationMs = 0;
        std::uint64_t commitDurationMs = 0;
        std::uint64_t totalMavftpDurationMs = 0;
        std::uint64_t segmentsPlanned = 0;
        std::uint64_t segmentsCommitted = 0;
        std::uint64_t activeSegmentIndex = 0;
        std::uint32_t segmentCapacity = 0;
    };

    /**
     * Per-Drone production Mission transport writer core. It owns only an
     * immutable compiled snapshot and borrows the endpoint-bound MAVFTP client.
     */
    class MAVFTPMissionTransport final : public IMissionTransport
    {
    public:
        static constexpr const char* MissionPath = "@MISSION/mission.dat";

        MAVFTPMissionTransport(
            mavftp::Client& client,
            MAVFTPMissionTransportConfig config = {},
            const ISegmentRangeSource* segmentSource = nullptr);

        bool Prepare(
            MissionTransportIdentity identity,
            std::vector<route_mission::MissionItemIntV1> mission,
            std::string& outDiagnostic) override;
        bool PrepareCompiled(
            MissionTransportIdentity identity,
            std::shared_ptr<const CompiledMissionSnapshot> snapshot,
            std::string& outDiagnostic);
        bool Upload(std::chrono::steady_clock::time_point now) override;
        bool Verify() const override;
        void Tick(std::chrono::steady_clock::time_point now) override;
        void Cancel(std::chrono::steady_clock::time_point now) override;

        MissionTransportStatus GetStatus() const override { return status_; }
        bool IsTerminal() const override;
        bool ConsumeProgressChanged() override;
        MissionTransportMetrics GetMetrics() const override;
        const MAVFTPMissionTransportMetrics& GetMavFtpMetrics() const
            { return mavftpMetrics_; }
        const std::string& GetSafeDiagnostic() const override
            { return safeDiagnostic_; }
        const std::string& GetVerificationMismatchDiagnostic() const override
            { return verificationMismatchDiagnostic_; }
        const char* GetTerminalPhaseName() const override
            { return terminalPhase_.c_str(); }
        IMissionProtocolMessageSink* GetMissionProtocolMessageSink() override
            { return nullptr; }

    private:
        static bool ReadChunk(
            std::size_t offset,
            std::uint8_t* destination,
            std::size_t size,
            void* context,
            std::string& error);
        bool StartCurrentSegment();
        void ApplyOperationMetrics(const mavftp::OperationResult& result);
        void Fail(std::string phase, std::string diagnostic,
            std::chrono::steady_clock::time_point now);
        void UpdateElapsed(std::chrono::steady_clock::time_point now);

        mavftp::Client& client_;
        MAVFTPMissionTransportConfig config_;
        const ISegmentRangeSource* segmentSource_ = nullptr;
        MissionTransportIdentity identity_;
        std::shared_ptr<const CompiledMissionSnapshot> snapshot_;
        std::size_t currentSegmentIndex_ = 0;
        MissionTransportStatus status_ = MissionTransportStatus::Idle;
        MissionTransportMetrics metrics_;
        MAVFTPMissionTransportMetrics mavftpMetrics_;
        std::chrono::steady_clock::time_point uploadStartedAt_{};
        bool progressChanged_ = false;
        std::string safeDiagnostic_;
        std::string verificationMismatchDiagnostic_;
        std::string terminalPhase_ = "Idle";
    };
}
