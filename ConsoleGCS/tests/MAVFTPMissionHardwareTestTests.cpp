#include "MAVFTPMissionHardwareTest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    void Expect(const bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    MAVFTPMissionHardwareTestOptions ValidOptions(
        const std::uint32_t itemCount)
    {
        MAVFTPMissionHardwareTestOptions options;
        options.enabled = true;
        options.transportMode = MissionTransportMode::MAVFTP;
        options.droneId = "Drone001";
        options.confirmedDroneId = "Drone001";
        options.artifactSetId = "Artifact-Validation-Fixture";
        options.artifactSetDigest.IsSet = true;
        options.artifactSetDigest.Bytes[0] = 1;
        options.routeSlotIndex = 0;
        options.artifactItemLimit = itemCount;
        options.segmentItemCapacity = 127;
        return options;
    }

    void TestDefaultSelectionRemainsMissionProtocol()
    {
        MAVFTPMissionHardwareTestOptions options;
        std::string error;
        Expect(options.transportMode == MissionTransportMode::MissionProtocol &&
            ValidateMAVFTPMissionHardwareTestOptions(options, error),
            "default MissionProtocol selection is not valid");
        options.transportMode = MissionTransportMode::MAVFTP;
        Expect(!ValidateMAVFTPMissionHardwareTestOptions(options, error),
            "MAVFTP selection was accepted without the explicit harness");
    }

    void TestExplicitSafetyContract()
    {
        std::string error;
        auto options = ValidOptions(10);
        Expect(ValidateMAVFTPMissionHardwareTestOptions(options, error),
            "valid explicit hardware options were rejected");
        options.confirmedDroneId = "Drone002";
        Expect(!ValidateMAVFTPMissionHardwareTestOptions(options, error) &&
            error.find("match") != std::string::npos,
            "wrong confirmation DroneID did not fail closed");
        options = ValidOptions(10);
        options.artifactSetDigest = {};
        Expect(!ValidateMAVFTPMissionHardwareTestOptions(options, error),
            "unset Artifact digest did not fail closed");
    }

    void TestRuntimeItemCountsAreNotFrozen()
    {
        std::string error;
        for (const std::uint32_t itemCount : { 10U, 500U, 9100U })
        {
            const auto options = ValidOptions(itemCount);
            Expect(ValidateMAVFTPMissionHardwareTestOptions(options, error) &&
                options.artifactItemLimit == itemCount,
                "runtime hardware-test item count was rejected or rewritten");
        }
    }

    void TestQuickModeResolvesCanonicalCurrentIdentity()
    {
        namespace artifact = dronegcs::route_artifact;
        namespace artifactV2 = dronegcs::route_artifact::v2;

        artifactV2::Manifest manifest;
        manifest.CreatedAtUtc = 1;
        manifest.Source.SourceDatasetDigest.IsSet = true;
        manifest.Source.SourceDatasetDigest.Bytes[0] = 1;
        manifest.Source.SourceByteSize = 1;
        manifest.Source.SourceDisplayFileName = "fixture.npz";
        manifest.FrameCount = 10;
        manifest.RouteSlotCount = 1;
        manifest.ShowOriginLatitudeE7 = 319324710;
        manifest.ShowOriginLongitudeE7 = 1186385310;
        manifest.WaypointGeneratorVersion = "MAVFTP-Quick-Test-Fixture";
        manifest.GeneratorInputDigest.IsSet = true;
        manifest.GeneratorInputDigest.Bytes[0] = 2;
        artifact::RouteRecord route;
        route.RouteSlotIndex = 0;
        artifact::Error contractError;
        Expect(artifact::BuildRouteRelativePath(
            route.RouteSlotIndex, route.RelativePath, contractError),
            contractError.Detail);
        route.FileByteSize = 1;
        route.RouteArtifactDigest.IsSet = true;
        route.RouteArtifactDigest.Bytes[0] = 3;
        manifest.Routes.push_back(route);
        Expect(artifactV2::ComputeArtifactSetId(
            artifactV2::BuildIdentityFromManifest(manifest),
            manifest.ArtifactSetId, contractError), contractError.Detail);
        Expect(artifactV2::ComputeArtifactSetDigest(
            manifest, manifest.ArtifactSetDigest, contractError),
            contractError.Detail);
        std::string manifestBytes;
        Expect(artifactV2::SerializeCanonicalManifest(
            manifest, manifestBytes, contractError), contractError.Detail);

        const fs::path root = fs::temp_directory_path() /
            ("DroneGCS-MAVFTP-Quick-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        const fs::path current = root / "Current";
        fs::create_directories(current);
        {
            std::ofstream output(current / "manifest.json", std::ios::binary);
            output.write(manifestBytes.data(),
                static_cast<std::streamsize>(manifestBytes.size()));
        }

        MAVFTPMissionHardwareTestOptions options;
        options.enabled = true;
        options.quickTest = true;
        options.transportMode = MissionTransportMode::MAVFTP;
        options.droneId = "Drone001";
        options.confirmedDroneId = options.droneId;
        options.routeSlotIndex = 0;
        std::string error;
        const bool resolved = ResolveMAVFTPQuickTestOptionsFromCurrent(
            root.u8string(), options, error);
        std::error_code cleanupError;
        fs::remove_all(root, cleanupError);
        Expect(resolved, error);
        Expect(options.artifactSetId == manifest.ArtifactSetId &&
            options.artifactSetDigest == manifest.ArtifactSetDigest &&
            options.artifactItemLimit == 10 &&
            options.segmentItemCapacity ==
                dronegcs::mission_ftp::MAVFTPMissionTransportConfig{}
                    .compileConfig.segmentCapacity &&
            ValidateMAVFTPMissionHardwareTestOptions(options, error),
            "quick mode did not resolve canonical Current authority");
    }

    void TestReportContainsRequiredMetrics()
    {
        MAVFTPMissionHardwareTestReport report;
        report.succeeded = true;
        report.droneId = "Drone001";
        report.targetIp = "127.0.0.1";
        report.targetPort = 14550;
        report.artifactRouteItems = 10;
        report.wireMissionItems = 11;
        report.segmentCount = 1;
        report.mavftpMetrics.writeBytes = 428;
        report.mavftpMetrics.writeBlocks = 2;
        report.mavftpMetrics.retryCount = 1;
        const std::string summary = report.ToHumanSummary();
        Expect(summary.find("ArtifactRouteItems=10") != std::string::npos &&
            summary.find("WireMissionItems=11") != std::string::npos &&
            summary.find("WriteBytes=428") != std::string::npos &&
            summary.find("WriteBlocks=2") != std::string::npos &&
            summary.find("RetryCount=1") != std::string::npos,
            "hardware report omitted required compile or MAVFTP metrics");
    }
}

int RunMAVFTPMissionHardwareTestTests()
{
    try
    {
        TestDefaultSelectionRemainsMissionProtocol();
        TestExplicitSafetyContract();
        TestRuntimeItemCountsAreNotFrozen();
        TestQuickModeResolvesCanonicalCurrentIdentity();
        TestReportContainsRequiredMetrics();
        std::cout << "MAVFTP Mission hardware harness safety tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "MAVFTP Mission hardware harness tests FAILED: "
            << error.what() << '\n';
        return 1;
    }
}
