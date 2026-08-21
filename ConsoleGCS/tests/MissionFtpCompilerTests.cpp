#include "MissionFtpCompiler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    namespace compiler = dronegcs::mission_ftp;
    namespace mission = dronegcs::route_mission;

    void Expect(const bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    std::uint16_t ReadU16(const std::uint8_t* const bytes)
    {
        return static_cast<std::uint16_t>(bytes[0]) |
            static_cast<std::uint16_t>(bytes[1]) << 8U;
    }

    std::uint32_t ReadU32(const std::uint8_t* const bytes)
    {
        return static_cast<std::uint32_t>(bytes[0]) |
            static_cast<std::uint32_t>(bytes[1]) << 8U |
            static_cast<std::uint32_t>(bytes[2]) << 16U |
            static_cast<std::uint32_t>(bytes[3]) << 24U;
    }

    std::vector<mission::MissionItemIntV1> BuildArtifact(
        const std::size_t count)
    {
        std::vector<mission::MissionItemIntV1> items;
        items.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            mission::MissionItemIntV1 item;
            item.Param1 = static_cast<float>(index % 251U);
            item.Param2 = static_cast<float>((index + 7U) % 253U);
            item.Param3 = static_cast<float>((index + 13U) % 255U);
            item.Param4 = -0.0f;
            item.X = 319324710 + static_cast<std::int32_t>(index);
            item.Y = 1186385310 - static_cast<std::int32_t>(index);
            item.Z = -22.2241936f + static_cast<float>(index % 101U) * 0.01f;
            item.Sequence = static_cast<std::uint16_t>(index);
            item.Command = 16;
            item.Frame = 3;
            item.Current = 0;
            item.AutoContinue = 1;
            item.MissionType = 0;
            items.push_back(item);
        }
        return items;
    }

    bool SamePayload(
        const mission::MissionItemIntV1& left,
        const mission::MissionItemIntV1& right)
    {
        return mission::Binary32ToBits(left.Param1) ==
                mission::Binary32ToBits(right.Param1) &&
            mission::Binary32ToBits(left.Param2) ==
                mission::Binary32ToBits(right.Param2) &&
            mission::Binary32ToBits(left.Param3) ==
                mission::Binary32ToBits(right.Param3) &&
            mission::Binary32ToBits(left.Param4) ==
                mission::Binary32ToBits(right.Param4) &&
            left.X == right.X && left.Y == right.Y &&
            mission::Binary32ToBits(left.Z) ==
                mission::Binary32ToBits(right.Z) &&
            left.Command == right.Command && left.Frame == right.Frame &&
            left.Current == right.Current &&
            left.AutoContinue == right.AutoContinue &&
            left.MissionType == right.MissionType;
    }

    struct StreamCapture
    {
        std::vector<std::uint8_t> bytes;
        std::vector<std::size_t> chunkSizes;
        std::size_t rejectAtChunk = (std::numeric_limits<std::size_t>::max)();
    };

    bool CaptureBytes(
        const std::uint8_t* const bytes,
        const std::size_t size,
        void* const context)
    {
        auto& capture = *static_cast<StreamCapture*>(context);
        const std::size_t chunkIndex = capture.chunkSizes.size();
        capture.chunkSizes.push_back(size);
        if (chunkIndex == capture.rejectAtChunk) return false;
        capture.bytes.insert(capture.bytes.end(), bytes, bytes + size);
        return true;
    }

    compiler::CompileResult Compile(
        const std::vector<mission::MissionItemIntV1>& artifact,
        const std::uint32_t segmentCapacity,
        const compiler::VerificationPolicy policy =
            compiler::VerificationPolicy::ProductionEndpoints)
    {
        compiler::CompileConfig config;
        config.segmentCapacity = segmentCapacity;
        config.verificationPolicy = policy;
        return compiler::MissionFtpCompiler::Compile(artifact, config);
    }

    StreamCapture StreamAll(const compiler::CompiledMissionSnapshot& snapshot)
    {
        StreamCapture capture;
        for (std::size_t index = 0;
            index < snapshot.GetSegmentDescriptors().size(); ++index)
        {
            const compiler::ValidationResult streamed =
                compiler::MissionFtpCompiler::StreamSegment(
                    snapshot, index, &CaptureBytes, &capture);
            Expect(streamed.IsValid(),
                "valid Mission FTP segment did not stream");
        }
        return capture;
    }

    void VerifySingleArtifactAndGoldenBytes()
    {
        std::vector<mission::MissionItemIntV1> artifact = BuildArtifact(1);
        artifact[0].Param1 = mission::Binary32FromBits(0x3f800001U);
        artifact[0].Param4 = mission::Binary32FromBits(0x80000000U);
        artifact[0].Command = 300;
        artifact[0].Frame = 6;
        artifact[0].Current = 1;
        artifact[0].AutoContinue = 0;

        const compiler::CompileResult result = Compile(
            artifact, 16, compiler::VerificationPolicy::StandardSamples);
        Expect(result.IsValid(), "single Artifact compile failed");
        const auto& snapshot = *result.snapshot;
        Expect(snapshot.GetTotalWireMissionItems() == 2,
            "single Artifact did not create Home plus one route item");
        Expect(snapshot.GetVerificationPolicy() ==
            compiler::VerificationPolicy::StandardSamples,
            "compile did not freeze the verification policy");

        const auto& wire = snapshot.GetWireMissionItems();
        Expect(wire[0].Sequence == 0 && wire[1].Sequence == 1,
            "single Artifact Home/route sequences differ");
        Expect(SamePayload(wire[0], artifact[0]) &&
            SamePayload(wire[1], artifact[0]),
            "Home expansion changed payload fields other than sequence");

        const StreamCapture capture = StreamAll(snapshot);
        Expect(capture.chunkSizes.size() == 3 &&
            capture.chunkSizes[0] == 10 &&
            capture.chunkSizes[1] == 38 &&
            capture.chunkSizes[2] == 38,
            "streaming encoder did not emit Header then individual items");
        Expect(capture.bytes.size() == 86,
            "single Artifact mission.dat segment size differs");
        const std::uint8_t expectedHeader[10] = {
            0x3d, 0x76, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x02, 0x00
        };
        Expect(std::equal(
            std::begin(expectedHeader), std::end(expectedHeader),
            capture.bytes.begin()),
            "single Artifact mission.dat header differs from golden bytes");

        const std::uint8_t* const home = capture.bytes.data() + 10;
        const std::uint8_t* const route = home + 38;
        Expect(ReadU32(home + 0) == 0x3f800001U &&
            ReadU32(home + 12) == 0x80000000U,
            "typed encoder changed binary32 payload bits");
        Expect(ReadU16(home + 28) == 0 && ReadU16(route + 28) == 1,
            "typed encoder wrote incorrect Home/route sequence bytes");
        Expect(ReadU16(route + 30) == 300 && route[32] == 0 &&
            route[33] == 0 && route[34] == 6 && route[35] == 1 &&
            route[36] == 0 && route[37] == 0,
            "typed encoder field layout differs from MissionDatContractV1");

        const compiler::ByteLayoutInfo& layout = snapshot.GetByteLayoutInfo();
        Expect(layout.headerBytes == 10 && layout.itemBytes == 38 &&
            layout.wireItemBytes == 76 && layout.segmentHeaderBytes == 10 &&
            layout.totalEncodedBytes == 86,
            "single Artifact byte layout metadata differs");
    }

    void VerifyContiguousWireSequence()
    {
        const auto artifact = BuildArtifact(10);
        const compiler::CompileResult result = Compile(artifact, 32);
        Expect(result.IsValid(), "ten-item Artifact compile failed");
        const auto& wire = result.snapshot->GetWireMissionItems();
        Expect(wire.size() == 11, "ten Artifact items did not produce eleven wire items");
        for (std::size_t index = 0; index < wire.size(); ++index)
            Expect(wire[index].Sequence == index,
                "Home-expanded wire sequence is not contiguous");
        for (std::size_t index = 0; index < artifact.size(); ++index)
            Expect(SamePayload(wire[index + 1], artifact[index]),
                "Artifact payload changed during Home expansion");
    }

    void VerifyCapacityOnePlan()
    {
        const compiler::CompileResult result = Compile(BuildArtifact(2), 1);
        Expect(result.IsValid(), "SegmentCapacity=1 compile failed");
        const auto& segments = result.snapshot->GetSegmentDescriptors();
        Expect(segments.size() == 3,
            "SegmentCapacity=1 did not produce one segment per wire item");
        for (std::size_t index = 0; index < segments.size(); ++index)
        {
            Expect(segments[index].segmentIndex == index &&
                segments[index].startIndex == index &&
                segments[index].itemCount == 1 &&
                segments[index].encodedBytes == 48,
                "SegmentCapacity=1 descriptor differs");
            Expect(segments[index].options == (index == 0
                ? mission::FirstContainerOptionsV1
                : mission::SubsequentContainerOptionsV1),
                "SegmentCapacity=1 clear/NO_CLEAR option differs");
        }
    }

    void VerifyCrossSegmentPlan()
    {
        const compiler::CompileResult result = Compile(BuildArtifact(10), 4);
        Expect(result.IsValid(), "cross-segment compile failed");
        const auto& segments = result.snapshot->GetSegmentDescriptors();
        Expect(segments.size() == 3,
            "eleven wire items at capacity four did not produce three segments");
        const std::uint16_t starts[] = { 0, 4, 8 };
        const std::uint16_t counts[] = { 4, 4, 3 };
        std::uint32_t next = 0;
        for (std::size_t index = 0; index < segments.size(); ++index)
        {
            Expect(segments[index].startIndex == starts[index] &&
                segments[index].itemCount == counts[index],
                "cross-segment descriptor start/count differs");
            Expect(segments[index].startIndex == next,
                "segment plan contains a gap or overlap");
            next += segments[index].itemCount;

            StreamCapture encoded;
            const compiler::ValidationResult streamed =
                compiler::MissionFtpCompiler::StreamSegment(
                    *result.snapshot, index, &CaptureBytes, &encoded);
            Expect(streamed.IsValid() &&
                encoded.bytes.size() == segments[index].encodedBytes,
                "cross-segment encoded byte count differs");
            Expect(ReadU16(encoded.bytes.data() + 0) ==
                    mission::LegacyMissionFtpMagicV1 &&
                ReadU16(encoded.bytes.data() + 2) ==
                    mission::LegacyMissionTypeV1 &&
                ReadU16(encoded.bytes.data() + 4) ==
                    segments[index].options &&
                ReadU16(encoded.bytes.data() + 6) ==
                    segments[index].startIndex &&
                ReadU16(encoded.bytes.data() + 8) ==
                    segments[index].itemCount,
                "cross-segment streamed header differs from its descriptor");
        }
        Expect(next == result.snapshot->GetTotalWireMissionItems(),
            "segment plan does not cover the complete wire Mission");
    }

    void VerifyMaximumBoundary()
    {
        const compiler::CompileResult maximum = Compile(BuildArtifact(65534), 65535);
        Expect(maximum.IsValid(), "maximum legal Artifact count was rejected");
        Expect(maximum.snapshot->GetTotalWireMissionItems() == 65535,
            "maximum legal Artifact count produced incorrect wire count");
        const auto& segment = maximum.snapshot->GetSegmentDescriptors().front();
        Expect(maximum.snapshot->GetSegmentDescriptors().size() == 1 &&
            segment.startIndex == 0 && segment.itemCount == 65535,
            "maximum legal wire Mission did not fit the legal uint16 container");
        Expect(maximum.snapshot->GetWireMissionItems().back().Sequence == 65534,
            "maximum legal Artifact produced an invalid last wire sequence");

        const compiler::CompileResult overflow = Compile(BuildArtifact(65535), 65535);
        Expect(!overflow.IsValid() &&
            overflow.validation.code == compiler::CompileErrorCode::ArtifactTooLarge,
            "65535 Artifact items were not rejected before Home expansion");
    }

    void VerifyValidationFailures()
    {
        Expect(!Compile({}, 1).IsValid(), "empty Artifact was accepted");

        compiler::CompileConfig zeroCapacity;
        zeroCapacity.segmentCapacity = 0;
        const compiler::CompileResult zero =
            compiler::MissionFtpCompiler::Compile(BuildArtifact(1), zeroCapacity);
        Expect(!zero.IsValid() && zero.validation.code ==
            compiler::CompileErrorCode::InvalidSegmentCapacity,
            "zero SegmentCapacity was accepted");

        compiler::CompileConfig oversizedCapacity;
        oversizedCapacity.segmentCapacity = 65536;
        const compiler::CompileResult oversized =
            compiler::MissionFtpCompiler::Compile(BuildArtifact(1), oversizedCapacity);
        Expect(!oversized.IsValid() && oversized.validation.code ==
            compiler::CompileErrorCode::InvalidSegmentCapacity,
            "SegmentCapacity above uint16 was accepted");

        auto badSequence = BuildArtifact(2);
        badSequence[1].Sequence = 0;
        const compiler::CompileResult sequence = Compile(badSequence, 2);
        Expect(!sequence.IsValid() && sequence.validation.code ==
            compiler::CompileErrorCode::ArtifactSequenceMismatch,
            "noncontiguous Artifact sequence was normalized instead of rejected");

        auto badType = BuildArtifact(1);
        badType[0].MissionType = 1;
        const compiler::CompileResult type = Compile(badType, 1);
        Expect(!type.IsValid() && type.validation.code ==
            compiler::CompileErrorCode::MissionTypeMismatch,
            "Mission type inconsistent with container header was accepted");

        compiler::CompileConfig badPolicy;
        badPolicy.segmentCapacity = 1;
        badPolicy.verificationPolicy =
            static_cast<compiler::VerificationPolicy>(255);
        const compiler::CompileResult policy =
            compiler::MissionFtpCompiler::Compile(BuildArtifact(1), badPolicy);
        Expect(!policy.IsValid() && policy.validation.code ==
            compiler::CompileErrorCode::InvalidVerificationPolicy,
            "unknown verification policy was accepted");
    }

    void VerifyDeterministicCompileAndStreamingFailure()
    {
        const auto artifact = BuildArtifact(10);
        const compiler::CompileResult first = Compile(
            artifact, 4, compiler::VerificationPolicy::DebugFull);
        const compiler::CompileResult second = Compile(
            artifact, 4, compiler::VerificationPolicy::DebugFull);
        Expect(first.IsValid() && second.IsValid(),
            "determinism fixtures did not compile");
        const StreamCapture firstBytes = StreamAll(*first.snapshot);
        const StreamCapture secondBytes = StreamAll(*second.snapshot);
        Expect(firstBytes.bytes == secondBytes.bytes &&
            firstBytes.chunkSizes == secondBytes.chunkSizes,
            "identical compile inputs did not produce byte-identical streams");

        StreamCapture rejecting;
        rejecting.rejectAtChunk = 1;
        const compiler::ValidationResult rejected =
            compiler::MissionFtpCompiler::StreamSegment(
                *first.snapshot, 0, &CaptureBytes, &rejecting);
        Expect(!rejected.IsValid() && rejected.code ==
            compiler::CompileErrorCode::ConsumerRejected,
            "streaming consumer rejection was not propagated");

        StreamCapture invalidIndex;
        const compiler::ValidationResult invalid =
            compiler::MissionFtpCompiler::StreamSegment(
                *first.snapshot,
                first.snapshot->GetSegmentDescriptors().size(),
                &CaptureBytes,
                &invalidIndex);
        Expect(!invalid.IsValid() && invalid.code ==
            compiler::CompileErrorCode::SegmentIndexOutOfRange,
            "out-of-range segment stream was accepted");
    }
}

int RunMissionFtpCompilerTests()
{
    try
    {
        VerifySingleArtifactAndGoldenBytes();
        VerifyContiguousWireSequence();
        VerifyCapacityOnePlan();
        VerifyCrossSegmentPlan();
        VerifyMaximumBoundary();
        VerifyValidationFailures();
        VerifyDeterministicCompileAndStreamingFailure();
        std::cout << "Mission FTP compiler tests passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Mission FTP compiler tests failed: "
            << exception.what() << '\n';
        return 1;
    }
}
