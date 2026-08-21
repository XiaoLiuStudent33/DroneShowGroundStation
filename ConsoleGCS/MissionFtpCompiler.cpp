#include "MissionFtpCompiler.h"

#include "ArduPilotMissionWireAdapter.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace dronegcs::mission_ftp
{
    namespace
    {
        ValidationResult Fail(
            const CompileErrorCode code,
            std::string safeDiagnostic)
        {
            ValidationResult result;
            result.code = code;
            result.safeDiagnostic = std::move(safeDiagnostic);
            return result;
        }

        bool IsKnownPolicy(const VerificationPolicy policy)
        {
            switch (policy)
            {
            case VerificationPolicy::DebugFull:
            case VerificationPolicy::StandardSamples:
            case VerificationPolicy::ProductionEndpoints:
                return true;
            }
            return false;
        }
    }

    CompileResult MissionFtpCompiler::Compile(
        const std::vector<route_mission::MissionItemIntV1>& artifactItems,
        const CompileConfig& config)
    {
        CompileResult result;
        if (artifactItems.empty())
        {
            result.validation = Fail(CompileErrorCode::EmptyArtifact,
                "Mission FTP compile requires at least one Artifact item.");
            return result;
        }
        if (artifactItems.size() >
            ardupilot_mission::MaximumArtifactRouteItems)
        {
            result.validation = Fail(CompileErrorCode::ArtifactTooLarge,
                "Artifact item count exceeds the Home-expanded uint16 Mission bound.");
            return result;
        }
        if (config.segmentCapacity == 0 ||
            config.segmentCapacity >
                (std::numeric_limits<std::uint16_t>::max)())
        {
            result.validation = Fail(CompileErrorCode::InvalidSegmentCapacity,
                "Mission FTP segment capacity must be within 1..65535 items.");
            return result;
        }
        if (!IsKnownPolicy(config.verificationPolicy))
        {
            result.validation = Fail(CompileErrorCode::InvalidVerificationPolicy,
                "Mission FTP verification policy is invalid.");
            return result;
        }
        for (std::size_t index = 0; index < artifactItems.size(); ++index)
        {
            if (artifactItems[index].Sequence != index)
            {
                result.validation = Fail(CompileErrorCode::ArtifactSequenceMismatch,
                    "Artifact Mission sequences must be contiguous from zero.");
                return result;
            }
            if (artifactItems[index].MissionType !=
                route_mission::LegacyMissionTypeV1)
            {
                result.validation = Fail(CompileErrorCode::MissionTypeMismatch,
                    "Artifact Mission type differs from the mission.dat header type.");
                return result;
            }
        }

        std::vector<route_mission::MissionItemIntV1> wireMissionItems;
        if (!ardupilot_mission::BuildWireMission(
            artifactItems, wireMissionItems))
        {
            result.validation = Fail(CompileErrorCode::WireMissionBuildFailed,
                "Home-expanded wire Mission could not be built.");
            return result;
        }

        std::vector<SegmentDescriptor> segments;
        const std::uint32_t totalWireItems =
            static_cast<std::uint32_t>(wireMissionItems.size());
        std::uint32_t start = 0;
        std::uint64_t totalEncodedBytes = 0;
        while (start < totalWireItems)
        {
            const std::uint32_t itemCount = (std::min)(
                config.segmentCapacity, totalWireItems - start);
            if (itemCount == 0 || start > 65535U || itemCount > 65535U ||
                itemCount - 1U > 65535U - start)
            {
                result.validation = Fail(CompileErrorCode::SegmentPlanInvalid,
                    "Mission FTP segment range exceeds the uint16 wire contract.");
                return result;
            }

            route_mission::Error codecError;
            std::size_t encodedBytes = 0;
            if (!route_mission::CheckedLegacyMissionFtpUploadPayloadSizeV1(
                itemCount, encodedBytes, codecError))
            {
                result.validation = Fail(CompileErrorCode::SegmentPlanInvalid,
                    codecError.Detail.empty()
                        ? "Mission FTP segment byte size is invalid."
                        : codecError.Detail);
                return result;
            }

            SegmentDescriptor descriptor;
            descriptor.segmentIndex =
                static_cast<std::uint32_t>(segments.size());
            descriptor.options = segments.empty()
                ? route_mission::FirstContainerOptionsV1
                : route_mission::SubsequentContainerOptionsV1;
            descriptor.startIndex = static_cast<std::uint16_t>(start);
            descriptor.itemCount = static_cast<std::uint16_t>(itemCount);
            descriptor.encodedBytes = encodedBytes;
            segments.push_back(descriptor);
            totalEncodedBytes += descriptor.encodedBytes;
            start += itemCount;
        }

        if (segments.empty() || start != totalWireItems)
        {
            result.validation = Fail(CompileErrorCode::SegmentPlanInvalid,
                "Mission FTP segment plan is empty or incomplete.");
            return result;
        }

        ByteLayoutInfo byteLayout;
        byteLayout.headerBytes = static_cast<std::uint32_t>(
            route_mission::LegacyMissionFtpContainerBytesV1);
        byteLayout.itemBytes = static_cast<std::uint32_t>(
            route_mission::MissionItemRecordBytesV1);
        byteLayout.wireItemBytes =
            static_cast<std::uint64_t>(totalWireItems) * byteLayout.itemBytes;
        byteLayout.segmentHeaderBytes =
            static_cast<std::uint64_t>(segments.size()) * byteLayout.headerBytes;
        byteLayout.totalEncodedBytes = totalEncodedBytes;

        result.snapshot = std::shared_ptr<const CompiledMissionSnapshot>(
            new CompiledMissionSnapshot(
                std::move(wireMissionItems),
                std::move(segments),
                config.verificationPolicy,
                byteLayout));
        result.validation = {};
        return result;
    }

    ValidationResult MissionFtpCompiler::StreamSegment(
        const CompiledMissionSnapshot& snapshot,
        const std::size_t segmentIndex,
        const ByteConsumer consumer,
        void* const consumerContext)
    {
        if (!consumer)
        {
            return Fail(CompileErrorCode::ConsumerRejected,
                "Mission FTP byte consumer is null.");
        }
        const std::vector<SegmentDescriptor>& segments =
            snapshot.GetSegmentDescriptors();
        if (segmentIndex >= segments.size())
        {
            return Fail(CompileErrorCode::SegmentIndexOutOfRange,
                "Mission FTP segment index is out of range.");
        }

        const SegmentDescriptor& descriptor = segments[segmentIndex];
        std::array<route_mission::Byte,
            route_mission::LegacyMissionFtpContainerBytesV1> header{};
        route_mission::Error codecError;
        if (!route_mission::EncodeLegacyMissionFtpContainerV1(
            descriptor.startIndex,
            descriptor.itemCount,
            descriptor.options,
            header,
            codecError))
        {
            return Fail(CompileErrorCode::SegmentPlanInvalid,
                codecError.Detail.empty()
                    ? "Mission FTP segment header is invalid."
                    : codecError.Detail);
        }
        if (!consumer(header.data(), header.size(), consumerContext))
        {
            return Fail(CompileErrorCode::ConsumerRejected,
                "Mission FTP byte consumer rejected the segment header.");
        }

        const auto& items = snapshot.GetWireMissionItems();
        const std::size_t begin = descriptor.startIndex;
        const std::size_t end = begin + descriptor.itemCount;
        if (end > items.size())
        {
            return Fail(CompileErrorCode::SegmentPlanInvalid,
                "Mission FTP segment descriptor exceeds the wire snapshot.");
        }
        for (std::size_t index = begin; index < end; ++index)
        {
            std::array<route_mission::Byte,
                route_mission::MissionItemRecordBytesV1> encoded{};
            route_mission::EncodeMissionItemRecordV1(items[index], encoded);
            if (!consumer(encoded.data(), encoded.size(), consumerContext))
            {
                return Fail(CompileErrorCode::ConsumerRejected,
                    "Mission FTP byte consumer rejected a Mission item.");
            }
        }
        return {};
    }
}
