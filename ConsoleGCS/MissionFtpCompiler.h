#pragma once

#include "RouteMissionCodec.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dronegcs::mission_ftp
{
    enum class VerificationPolicy : std::uint8_t
    {
        DebugFull,
        StandardSamples,
        ProductionEndpoints
    };

    enum class CompileErrorCode : std::uint8_t
    {
        None,
        EmptyArtifact,
        ArtifactTooLarge,
        ArtifactSequenceMismatch,
        MissionTypeMismatch,
        InvalidSegmentCapacity,
        InvalidVerificationPolicy,
        WireMissionBuildFailed,
        SegmentPlanInvalid,
        SegmentIndexOutOfRange,
        ConsumerRejected
    };

    struct ValidationResult
    {
        CompileErrorCode code = CompileErrorCode::None;
        std::string safeDiagnostic;

        bool IsValid() const { return code == CompileErrorCode::None; }
    };

    struct CompileConfig
    {
        std::uint32_t segmentCapacity = 0;
        VerificationPolicy verificationPolicy =
            VerificationPolicy::ProductionEndpoints;
    };

    struct SegmentDescriptor
    {
        std::uint32_t segmentIndex = 0;
        std::uint16_t options = 0;
        std::uint16_t startIndex = 0;
        std::uint16_t itemCount = 0;
        std::uint64_t encodedBytes = 0;
    };

    struct ByteLayoutInfo
    {
        std::uint32_t headerBytes = 0;
        std::uint32_t itemBytes = 0;
        std::uint64_t wireItemBytes = 0;
        std::uint64_t segmentHeaderBytes = 0;
        std::uint64_t totalEncodedBytes = 0;
    };

    class CompiledMissionSnapshot final
    {
    public:
        const std::vector<route_mission::MissionItemIntV1>&
            GetWireMissionItems() const { return wireMissionItems_; }
        std::uint32_t GetTotalWireMissionItems() const
        {
            return static_cast<std::uint32_t>(wireMissionItems_.size());
        }
        const std::vector<SegmentDescriptor>& GetSegmentDescriptors() const
        {
            return segments_;
        }
        VerificationPolicy GetVerificationPolicy() const
        {
            return verificationPolicy_;
        }
        const ByteLayoutInfo& GetByteLayoutInfo() const { return byteLayout_; }

    private:
        friend class MissionFtpCompiler;

        CompiledMissionSnapshot(
            std::vector<route_mission::MissionItemIntV1> wireMissionItems,
            std::vector<SegmentDescriptor> segments,
            const VerificationPolicy verificationPolicy,
            const ByteLayoutInfo byteLayout)
            : wireMissionItems_(std::move(wireMissionItems))
            , segments_(std::move(segments))
            , verificationPolicy_(verificationPolicy)
            , byteLayout_(byteLayout)
        {
        }

        std::vector<route_mission::MissionItemIntV1> wireMissionItems_;
        std::vector<SegmentDescriptor> segments_;
        VerificationPolicy verificationPolicy_ =
            VerificationPolicy::ProductionEndpoints;
        ByteLayoutInfo byteLayout_;
    };

    struct CompileResult
    {
        std::shared_ptr<const CompiledMissionSnapshot> snapshot;
        ValidationResult validation;

        bool IsValid() const
        {
            return validation.IsValid() && static_cast<bool>(snapshot);
        }
    };

    using ByteConsumer = bool (*)(
        const std::uint8_t* bytes,
        std::size_t size,
        void* context);

    class MissionFtpCompiler final
    {
    public:
        static CompileResult Compile(
            const std::vector<route_mission::MissionItemIntV1>& artifactItems,
            const CompileConfig& config);

        static ValidationResult StreamSegment(
            const CompiledMissionSnapshot& snapshot,
            std::size_t segmentIndex,
            ByteConsumer consumer,
            void* consumerContext);
    };
}
