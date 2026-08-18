#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "RouteMissionCodec.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cfenv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    namespace mission = dronegcs::route_mission;
    namespace wpl = dronegcs::wpl110;

    bool CollectRow(const wpl::Row& row, void* context);

    bool SameRow(const wpl::Row& left, const wpl::Row& right)
    {
        return left.Sequence == right.Sequence
            && left.Red == right.Red
            && left.Green == right.Green
            && left.Blue == right.Blue
            && left.LatitudeE7 == right.LatitudeE7
            && left.LongitudeE7 == right.LongitudeE7
            && left.AltitudeE7 == right.AltitudeE7;
    }

    bool SameRows(const std::vector<wpl::Row>& left, const std::vector<wpl::Row>& right)
    {
        return left.size() == right.size()
            && std::equal(left.begin(), left.end(), right.begin(), SameRow);
    }

    void Expect(const bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    template <std::size_t Size>
    void ExpectBytes(
        const std::array<std::uint8_t, Size>& actual,
        const std::array<std::uint8_t, Size>& expected,
        const std::string& label)
    {
        Expect(actual == expected, label + " bytes differ from the manual golden fixture");
    }

    wpl::Row GoldenRow()
    {
        wpl::Row row;
        row.Sequence = 0x1234;
        row.Red = 1;
        row.Green = 127;
        row.Blue = 255;
        row.LatitudeE7 = 0x12345678;
        row.LongitudeE7 = -2;
        row.AltitudeE7 = 15000000;
        return row;
    }

    void VerifyRecordGolden()
    {
        const std::array<std::uint8_t, 38> expected = {
            0x00,0x00,0x80,0x3f, 0x00,0x00,0xfe,0x42,
            0x00,0x00,0x7f,0x43, 0x00,0x00,0x00,0x00,
            0x78,0x56,0x34,0x12, 0xfe,0xff,0xff,0xff,
            0x00,0x00,0xc0,0x3f, 0x34,0x12,0x10,0x00,
            0x00,0x00,0x03,0x00,0x01,0x00
        };
        std::array<std::uint8_t, mission::MissionItemRecordBytesV1> actual{};
        mission::Error error;
        Expect(mission::EncodeMissionItemRecordV1(GoldenRow(), actual, error),
            "single Mission record did not encode");
        ExpectBytes(actual, expected, "single 38-byte record");
        static_assert(mission::MissionItemRecordBytesV1 == 38);

        wpl::Row minimum;
        minimum.Sequence = 0;
        minimum.LatitudeE7 = std::numeric_limits<std::int32_t>::min();
        minimum.LongitudeE7 = std::numeric_limits<std::int32_t>::max();
        minimum.AltitudeE7 = -15000000;
        const std::array<std::uint8_t, 38> minimumExpected = {
            0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x80, 0xff,0xff,0xff,0x7f,
            0x00,0x00,0xc0,0xbf, 0x00,0x00,0x10,0x00,
            0x00,0x00,0x03,0x00,0x01,0x00
        };
        Expect(mission::EncodeMissionItemRecordV1(minimum, actual, error),
            "signed boundary Mission record did not encode");
        ExpectBytes(actual, minimumExpected, "signed coordinate boundary record");

        wpl::Row maximum = minimum;
        maximum.Sequence = 65535;
        maximum.Red = 1;
        maximum.Green = 128;
        maximum.Blue = 254;
        maximum.LatitudeE7 = 0;
        maximum.LongitudeE7 = 0;
        maximum.AltitudeE7 = 0;
        const std::array<std::uint8_t, 38> maximumExpected = {
            0x00,0x00,0x80,0x3f, 0x00,0x00,0x00,0x43,
            0x00,0x00,0x7e,0x43, 0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00, 0xff,0xff,0x10,0x00,
            0x00,0x00,0x03,0x00,0x01,0x00
        };
        Expect(mission::EncodeMissionItemRecordV1(maximum, actual, error),
            "maximum sequence Mission record did not encode");
        ExpectBytes(actual, maximumExpected, "maximum sequence/RGB record");

        maximum.Sequence = 65536;
        Expect(!mission::EncodeMissionItemRecordV1(maximum, actual, error)
            && error.Code == mission::ErrorCode::SequenceOutOfRange,
            "sequence above uint16 was not rejected");
    }

    void VerifyAltitudeGolden()
    {
        struct Fixture { std::int64_t e7; std::uint32_t bits; };
        const Fixture fixtures[] = {
            { 0, 0x00000000U },
            { 1, 0x33d6bf95U },
            { -1, 0xb3d6bf95U },
            { 15000000, 0x3fc00000U },
            { -15000000, 0xbfc00000U },
            { 123456789, 0x414587e7U },
            { -123456789, 0xc14587e7U },
            // Exact midpoint: lower significand is even, so it rounds down.
            { 1310720078125LL, 0x48000000U },
            // Exact midpoint: lower significand is odd, so it rounds up.
            { 1310720234375LL, 0x48000002U },
            { std::numeric_limits<std::int64_t>::max(), 0x5356bf95U },
            { std::numeric_limits<std::int64_t>::min(), 0xd356bf95U }
        };
        mission::Error error;
        for (const Fixture& fixture : fixtures)
        {
            std::uint32_t bits = 0xffffffffU;
            Expect(mission::AltitudeE7ToBinary32Bits(fixture.e7, bits, error),
                "AltitudeE7 golden conversion failed");
            Expect(bits == fixture.bits, "AltitudeE7 golden binary32 bits differ");
        }
        std::uint32_t zeroBits = 1;
        Expect(mission::AltitudeE7ToBinary32Bits(0, zeroBits, error)
            && zeroBits == 0, "zero AltitudeE7 did not produce positive zero");

        const int originalRoundingMode = std::fegetround();
        if (originalRoundingMode != -1 && std::fesetround(FE_DOWNWARD) == 0)
        {
            std::uint32_t roundingIndependentBits = 0;
            Expect(mission::AltitudeE7ToBinary32Bits(
                1310720234375LL, roundingIndependentBits, error)
                && roundingIndependentBits == 0x48000002U,
                "AltitudeE7 conversion depended on the process FPU rounding mode");
            std::fesetround(originalRoundingMode);
        }

        // int64/E7 is always far below binary32 max. Text outside int64 is
        // rejected by the shared WPL typed boundary before reaching this API.
        const std::string overflowWpl =
            "QGC WPL 110\r\n0\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t922337203685.4775808\t1";
        wpl::StreamingParser parser(1);
        Expect(!parser.Consume(reinterpret_cast<const std::uint8_t*>(overflowWpl.data()),
            overflowWpl.size()) || !parser.Finish(),
            "Altitude text outside int64 was not rejected at the typed boundary");

        // The typed E7 authority has one integer zero (no signed-zero state).
        // Its binary32 encoding was asserted above to be positive zero.
    }

    void VerifyRgbGolden()
    {
        const std::pair<std::uint8_t, std::uint32_t> fixtures[] = {
            {0,0x00000000U}, {1,0x3f800000U}, {127,0x42fe0000U},
            {128,0x43000000U}, {254,0x437e0000U}, {255,0x437f0000U}
        };
        for (const auto& fixture : fixtures)
            Expect(mission::Rgb8ToBinary32Bits(fixture.first) == fixture.second,
                "RGB8 exact integer binary32 fixture differs");
    }

    void VerifyContainerGolden()
    {
        mission::Error error;
        std::array<std::uint8_t, mission::LegacyMissionFtpContainerBytesV1> bytes{};
        const std::array<std::uint8_t, 10> first = {
            0x3d,0x76, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x03,0x00
        };
        Expect(mission::EncodeLegacyMissionFtpContainerV1(
            0, 3, mission::FirstContainerOptionsV1, bytes, error),
            "first legacy container did not encode");
        ExpectBytes(bytes, first, "first legacy container");

        const std::array<std::uint8_t, 10> subsequent = {
            0x3d,0x76, 0x00,0x00, 0x01,0x00, 0x03,0x00, 0x02,0x00
        };
        Expect(mission::EncodeLegacyMissionFtpContainerV1(
            3, 2, mission::SubsequentContainerOptionsV1, bytes, error),
            "subsequent legacy container did not encode");
        ExpectBytes(bytes, subsequent, "subsequent legacy container");
        Expect(!mission::EncodeLegacyMissionFtpContainerV1(0, 0, 0, bytes, error)
            && error.Code == mission::ErrorCode::EmptyContainerRejected,
            "production container accepted a zero-item legacy shot");
        Expect(!mission::EncodeLegacyMissionFtpContainerV1(65535, 2, 0, bytes, error)
            && error.Code == mission::ErrorCode::ContainerRangeOverflow,
            "container accepted a sequence range overflow");
    }

    void VerifyNumericBounds()
    {
        mission::Error error;
        std::size_t bytes = 0;
        Expect(mission::CheckedMissionPayloadSize(65536, bytes, error)
            && bytes == 65536ULL * 38,
            "maximum logical route row count was not accepted");
        Expect(!mission::CheckedMissionPayloadSize(0, bytes, error)
            && error.Code == mission::ErrorCode::RouteRowCountOutOfRange,
            "zero logical route rows were accepted");
        Expect(!mission::CheckedMissionPayloadSize(65537, bytes, error)
            && error.Code == mission::ErrorCode::RouteRowCountOutOfRange,
            "logical route above 65536 rows was accepted");
        std::array<std::uint8_t, 10> container{};
        Expect(!mission::EncodeLegacyMissionFtpContainerV1(
            65536, 1, 0, container, error)
            && error.Code == mission::ErrorCode::ContainerFieldOutOfRange,
            "container start above uint16 was accepted");
        Expect(!mission::EncodeLegacyMissionFtpContainerV1(
            0, 65536, 0, container, error)
            && error.Code == mission::ErrorCode::ContainerFieldOutOfRange,
            "container item count above uint16 was accepted");
    }

    struct VectorConsumer
    {
        std::vector<std::uint8_t> bytes;
        std::uint64_t calls = 0;
    };

    bool AppendBytes(const std::uint8_t* bytes, const std::size_t size, void* context)
    {
        auto& target = *static_cast<VectorConsumer*>(context);
        target.bytes.insert(target.bytes.end(), bytes, bytes + size);
        ++target.calls;
        return true;
    }

    bool DiscardBytes(const std::uint8_t* bytes, const std::size_t size, void* context)
    {
        auto& checksum = *static_cast<std::uint64_t*>(context);
        if (size > 0) checksum = checksum * 1315423911U + bytes[0] + bytes[size - 1] + size;
        return true;
    }

    std::vector<wpl::Row> MakeRows(const std::int32_t route, const std::int32_t count)
    {
        std::vector<wpl::Row> rows;
        rows.reserve(count);
        for (std::int32_t sequence = 0; sequence < count; ++sequence)
        {
            wpl::Row row;
            row.Sequence = sequence;
            row.Red = static_cast<std::uint8_t>((route + sequence) % 256);
            row.Green = static_cast<std::uint8_t>((route * 3 + sequence) % 256);
            row.Blue = static_cast<std::uint8_t>((route * 7 + sequence) % 256);
            row.LatitudeE7 = 319324710 + route + sequence;
            row.LongitudeE7 = 1186385310 + route + sequence;
            row.AltitudeE7 = -222241935LL + sequence;
            rows.push_back(row);
        }
        return rows;
    }

    std::string MakeCanonicalWpl(const std::vector<wpl::Row>& rows)
    {
        std::string bytes = std::string(wpl::Header) + "\r\n";
        for (std::size_t index = 0; index < rows.size(); ++index)
        {
            wpl::FormattedRow formatted;
            Expect(wpl::FormatRow(rows[index], formatted), "benchmark WPL row formatting failed");
            bytes.append(reinterpret_cast<const char*>(formatted.Bytes.data()), formatted.Size);
            if (index + 1 < rows.size()) bytes += "\r\n";
        }
        return bytes;
    }

    bool CollectRow(const wpl::Row& row, void* context)
    {
        static_cast<std::vector<wpl::Row>*>(context)->push_back(row);
        return true;
    }

    std::vector<wpl::Row> ParseCanonicalWpl(
        const std::string& bytes,
        const std::int32_t rows,
        double& elapsed)
    {
        std::vector<wpl::Row> parsed;
        parsed.reserve(rows);
        wpl::StreamingParser parser(rows, &CollectRow, &parsed);
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t offset = 0; offset < bytes.size();)
        {
            const std::size_t count = std::min<std::size_t>(65536, bytes.size() - offset);
            Expect(parser.Consume(reinterpret_cast<const std::uint8_t*>(bytes.data() + offset), count),
                "benchmark canonical WPL parse failed");
            offset += count;
        }
        Expect(parser.Finish(), "benchmark canonical WPL finalization failed");
        elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        return parsed;
    }

    std::string ReadFileBytes(const std::string& path)
    {
        std::ifstream input(path, std::ios::binary);
        Expect(input.is_open(), "representative Artifact route could not be opened: " + path);
        input.seekg(0, std::ios::end);
        const std::streamoff size = input.tellg();
        Expect(size > 0, "representative Artifact route is empty: " + path);
        input.seekg(0, std::ios::beg);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        input.read(bytes.data(), size);
        Expect(input.good(), "representative Artifact route could not be read: " + path);
        return bytes;
    }

    std::vector<std::uint8_t> EncodeWhole(const std::vector<wpl::Row>& rows)
    {
        mission::WholeRouteEncoderV1 encoder;
        mission::Error error;
        Expect(encoder.Begin(static_cast<std::uint32_t>(rows.size()), error),
            "whole-route begin failed");
        for (const auto& row : rows) Expect(encoder.Consume(row, error), "whole-route row failed");
        Expect(encoder.Finish(error), "whole-route finish failed");
        return encoder.GetBytes();
    }

    template <std::size_t BufferBytes>
    std::vector<std::uint8_t> EncodeBuffered(const std::vector<wpl::Row>& rows, std::uint64_t& calls)
    {
        std::array<std::uint8_t, BufferBytes> buffer{};
        VectorConsumer output;
        output.bytes.reserve(rows.size() * mission::MissionItemRecordBytesV1);
        mission::BufferedRouteEncoderV1 encoder;
        mission::Error error;
        Expect(encoder.Begin(static_cast<std::uint32_t>(rows.size()), buffer.data(),
            buffer.size(), &AppendBytes, &output, error), "buffered begin failed");
        for (const auto& row : rows) Expect(encoder.Consume(row, error), "buffered row failed");
        Expect(encoder.Finish(error), "buffered finish failed");
        calls = output.calls;
        return output.bytes;
    }

    void VerifyMultiRecordAndStrategies()
    {
        const auto rows = MakeRows(7, 3);
        const auto whole = EncodeWhole(rows);
        std::uint64_t chunkCalls = 0;
        std::uint64_t streamingCalls = 0;
        const auto chunked = EncodeBuffered<38 * 2>(rows, chunkCalls);
        const auto streaming = EncodeBuffered<38>(rows, streamingCalls);
        Expect(whole.size() == 3 * 38, "multi-record layout size differs");
        Expect(whole == chunked && whole == streaming,
            "whole/chunked/streaming Mission bytes differ");
        Expect(chunkCalls == 2 && streamingCalls == 3,
            "buffer strategies did not flush at complete-record boundaries");
        for (std::size_t index = 0; index < rows.size(); ++index)
        {
            std::array<std::uint8_t, 38> manualRecord{};
            mission::Error error;
            Expect(mission::EncodeMissionItemRecordV1(rows[index], manualRecord, error),
                "multi-record independent record failed");
            Expect(std::equal(manualRecord.begin(), manualRecord.end(), whole.begin() + index * 38),
                "multi-record layout is not contiguous 38-byte records");
        }
    }

    struct StrategyMetrics
    {
        double seconds = 0;
        double cpuSeconds = 0;
        std::uint64_t rows = 0;
        std::uint64_t bytes = 0;
        std::uint64_t checksum = 0;
        std::size_t activeBufferBytes = 0;
        std::uint64_t allocationProxy = 0;
    };

    double CpuSeconds();

    StrategyMetrics BenchmarkWhole(
        const std::vector<std::vector<wpl::Row>>& routes,
        const int iterations)
    {
        StrategyMetrics metrics;
        mission::WholeRouteEncoderV1 encoder;
        mission::Error error;
        const double cpuStart = CpuSeconds();
        const auto start = std::chrono::steady_clock::now();
        for (int iteration = 0; iteration < iterations; ++iteration)
        {
            for (const auto& rows : routes)
            {
                Expect(encoder.Begin(static_cast<std::uint32_t>(rows.size()), error), "whole benchmark begin");
                for (const auto& row : rows) Expect(encoder.Consume(row, error), "whole benchmark row");
                Expect(encoder.Finish(error), "whole benchmark finish");
                const auto& bytes = encoder.GetBytes();
                metrics.checksum += bytes.front() + bytes.back() + bytes.size();
                metrics.rows += rows.size();
                metrics.bytes += bytes.size();
            }
        }
        metrics.seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        metrics.cpuSeconds = CpuSeconds() - cpuStart;
        metrics.activeBufferBytes = routes.front().size() * 38;
        metrics.allocationProxy = 1;
        return metrics;
    }

    template <std::size_t BufferBytes>
    StrategyMetrics BenchmarkBuffered(
        const std::vector<std::vector<wpl::Row>>& routes,
        const int iterations,
        const std::uint64_t allocationProxy)
    {
        StrategyMetrics metrics;
        std::array<std::uint8_t, BufferBytes> buffer{};
        mission::Error error;
        const double cpuStart = CpuSeconds();
        const auto start = std::chrono::steady_clock::now();
        for (int iteration = 0; iteration < iterations; ++iteration)
        {
            for (const auto& rows : routes)
            {
                mission::BufferedRouteEncoderV1 encoder;
                Expect(encoder.Begin(static_cast<std::uint32_t>(rows.size()), buffer.data(),
                    buffer.size(), &DiscardBytes, &metrics.checksum, error), "buffer benchmark begin");
                for (const auto& row : rows) Expect(encoder.Consume(row, error), "buffer benchmark row");
                Expect(encoder.Finish(error), "buffer benchmark finish");
                metrics.rows += rows.size();
                metrics.bytes += rows.size() * 38;
            }
        }
        metrics.seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        metrics.cpuSeconds = CpuSeconds() - cpuStart;
        metrics.activeBufferBytes = BufferBytes;
        metrics.allocationProxy = allocationProxy;
        return metrics;
    }

    void PrintMetrics(const char* label, const StrategyMetrics& metrics)
    {
        const double mib = metrics.bytes / (1024.0 * 1024.0);
        std::cout << label
            << " elapsed_s=" << metrics.seconds
            << " cpu_s=" << metrics.cpuSeconds
            << " rows=" << metrics.rows
            << " encoded_mib=" << mib
            << " rows_per_s=" << (metrics.seconds > 0 ? metrics.rows / metrics.seconds : 0)
            << " mib_per_s=" << (metrics.seconds > 0 ? mib / metrics.seconds : 0)
            << " active_buffer_bytes=" << metrics.activeBufferBytes
            << " allocation_proxy=" << metrics.allocationProxy
            << " checksum=" << metrics.checksum << '\n';
    }

    double CpuSeconds()
    {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        return static_cast<double>(k.QuadPart + u.QuadPart) / 10000000.0;
    }

    int RunBenchmarks(const std::string& artifactRouteDirectory)
    {
        std::vector<std::vector<wpl::Row>> routes;
        double totalParseSeconds = 0;
        std::uint64_t wplBytes = 0;
        const bool realArtifact = !artifactRouteDirectory.empty();
        for (const int routeSlot : {0, 500, 999})
        {
            const auto sourceRows = realArtifact ? std::vector<wpl::Row>{}
                : MakeRows(routeSlot, 9100);
            const std::string canonical = realArtifact
                ? ReadFileBytes(artifactRouteDirectory + "/" + std::to_string(routeSlot)
                    + ".waypoints")
                : MakeCanonicalWpl(sourceRows);
            double parseSeconds = 0;
            auto parsed = ParseCanonicalWpl(canonical, 9100, parseSeconds);
            if (!realArtifact)
                Expect(SameRows(parsed, sourceRows),
                    "representative Artifact fixture typed rows changed during WPL parse");
            std::cout << (realArtifact ? "MISSION_REAL_ARTIFACT_ROUTE" : "MISSION_ARTIFACT_FIXTURE_ROUTE")
                << " slot=" << routeSlot
                << " wpl_bytes=" << canonical.size()
                << " parse_s=" << parseSeconds << " rows=9100\n";
            totalParseSeconds += parseSeconds;
            wplBytes += canonical.size();
            routes.push_back(std::move(parsed));
        }

        constexpr int Iterations = 100;
        const StrategyMetrics whole = BenchmarkWhole(routes, Iterations);
        const StrategyMetrics chunked = BenchmarkBuffered<38 * 1024>(routes, Iterations, 0);
        const StrategyMetrics streaming = BenchmarkBuffered<38 * 8>(routes, Iterations, 0);
        PrintMetrics("MISSION_WHOLE", whole);
        PrintMetrics("MISSION_CHUNKED", chunked);
        PrintMetrics("MISSION_STREAMING", streaming);
        std::cout << "MISSION_PARSE representative_routes=3 rows=27300 wpl_bytes="
            << wplBytes << " elapsed_s=" << totalParseSeconds << '\n';

        const auto deterministic = EncodeWhole(routes[0]);
        std::uint64_t ignored = 0;
        Expect(deterministic == EncodeBuffered<38 * 1024>(routes[0], ignored),
            "chunked benchmark bytes differ");
        Expect(deterministic == EncodeBuffered<38 * 8>(routes[0], ignored),
            "streaming benchmark bytes differ");

        mission::WholeRouteEncoderV1 encoder;
        mission::Error error;
        const double scaleCpuBefore = CpuSeconds();
        const auto scaleStart = std::chrono::steady_clock::now();
        std::uint64_t scaleChecksum = 0;
        for (int route = 0; route < 1000; ++route)
        {
            const auto& rows = routes[static_cast<std::size_t>(route) % routes.size()];
            Expect(encoder.Begin(9100, error), "1000-route begin failed");
            for (const auto& row : rows) Expect(encoder.Consume(row, error), "1000-route row failed");
            Expect(encoder.Finish(error), "1000-route finish failed");
            const auto& bytes = encoder.GetBytes();
            scaleChecksum += bytes.front() + bytes.back() + bytes.size();
        }
        const double scaleSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - scaleStart).count();
        const double scaleCpuAfter = CpuSeconds();
        PROCESS_MEMORY_COUNTERS memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
        const std::uint64_t scaleRows = 9100ULL * 1000;
        const std::uint64_t scaleBytes = scaleRows * 38;
        std::cout << "MISSION_1000_ROUTES routes=1000 rows=" << scaleRows
            << " bytes=" << scaleBytes
            << " elapsed_s=" << scaleSeconds
            << " rows_per_s=" << scaleRows / scaleSeconds
            << " mib_per_s=" << (scaleBytes / (1024.0 * 1024.0)) / scaleSeconds
            << " peak_working_set_mib=" << memory.PeakWorkingSetSize / (1024.0 * 1024.0)
            << " active_encoded_routes=1 active_buffer_bytes=345800 allocation_proxy=1 checksum="
            << scaleChecksum << " cpu_s=" << (scaleCpuAfter - scaleCpuBefore)
            << " source=" << (realArtifact ? "real_artifact" : "isolated_artifact_fixture")
            << '\n';
        return 0;
    }
}

int RunRouteMissionCodecTests()
{
    try
    {
        VerifyRecordGolden();
        VerifyAltitudeGolden();
        VerifyRgbGolden();
        VerifyContainerGolden();
        VerifyNumericBounds();
        VerifyMultiRecordAndStrategies();
        std::cout << "Shared Route Mission codec tests passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Shared Route Mission codec tests failed: " << exception.what() << '\n';
        return 1;
    }
}

int RunRouteMissionBenchmarks(const int argc, char** argv)
{
    try
    {
        return RunBenchmarks(argc >= 3 ? argv[2] : "");
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Shared Route Mission benchmark failed: " << exception.what() << '\n';
        return 1;
    }
}
