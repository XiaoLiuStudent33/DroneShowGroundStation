#include "MAVFTPMissionHardwareTest.h"

#include "AppConfig.h"
#include "MavlinkDatagramSender.h"

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

    void TestDisabledHarnessRemainsValid()
    {
        MAVFTPMissionHardwareTestOptions options;
        std::string error;
        Expect(ValidateMAVFTPMissionHardwareTestOptions(options, error),
            "disabled hardware harness options were rejected");
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

    void TestTransportConfigurationPrecedence()
    {
        AppConfig config;
        config.mavftpSegmentItems = 4000U;
        config.mavftpRequestTimeoutMs = 750;
        config.mavftpTerminateTimeoutMs = 2500;
        config.mavftpRequestRetryCount = 3U;
        config.mavftpWriteWindow = 8U;
        MAVFTPMissionHardwareTestOptions options;
        std::string error;

        Expect(ResolveMAVFTPMissionTransportOptions(
            config, false, false, false, options, error) &&
            options.segmentItemCapacity == 4000U &&
            options.requestTimeoutMs == 750 &&
            options.terminateTimeoutMs == 2500 &&
            options.requestRetryCount == 3U &&
            options.writeWindow == 8U,
            "missing CLI did not use the INI-derived MAVFTP values");

        options.segmentItemCapacity = 4U;
        options.terminateTimeoutMs = 5000;
        options.writeWindow = 4U;
        Expect(ResolveMAVFTPMissionTransportOptions(
            config, true, true, true, options, error) &&
            options.segmentItemCapacity == 4U &&
            options.terminateTimeoutMs == 5000 && options.writeWindow == 4U,
            "explicit CLI transport values did not override INI");

        options.segmentItemCapacity = 0U;
        Expect(!ResolveMAVFTPMissionTransportOptions(
            config, true, true, true, options, error) &&
            options.segmentItemCapacity == 0U,
            "explicit CLI zero fell back to INI instead of failing");
        options.segmentItemCapacity = 4U;
        options.writeWindow = 0U;
        Expect(!ResolveMAVFTPMissionTransportOptions(
            config, true, true, true, options, error) && options.writeWindow == 0U,
            "explicit CLI write-window zero fell back instead of failing");
        options.writeWindow = 4U;
        options.terminateTimeoutMs = 0;
        Expect(!ResolveMAVFTPMissionTransportOptions(
            config, true, true, true, options, error) &&
            options.terminateTimeoutMs == 0,
            "explicit CLI terminate-timeout zero fell back instead of failing");
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
        options.droneId = "Drone001";
        options.confirmedDroneId = options.droneId;
        options.routeSlotIndex = 0;
        options.segmentItemCapacity = 4;
        options.requestTimeoutMs = 1234;
        options.terminateTimeoutMs = 4321;
        options.requestRetryCount = 2;
        options.writeWindow = 8;
        std::string error;
        const bool resolved = ResolveMAVFTPQuickTestOptionsFromCurrent(
            root.u8string(), options, error);
        std::error_code cleanupError;
        fs::remove_all(root, cleanupError);
        Expect(resolved, error);
        Expect(options.artifactSetId == manifest.ArtifactSetId &&
            options.artifactSetDigest == manifest.ArtifactSetDigest &&
            options.artifactItemLimit == 10 &&
            options.segmentItemCapacity == 4 &&
            options.requestTimeoutMs == 1234 &&
            options.terminateTimeoutMs == 4321 &&
            options.requestRetryCount == 2 &&
            options.writeWindow == 8 &&
            ValidateMAVFTPMissionHardwareTestOptions(options, error),
            "quick resolver changed transport configuration or failed Current authority");
    }

    class CountingSender final : public IMavlinkDatagramSender
    {
    public:
        bool SendTo(
            const std::string& targetIp,
            const std::uint16_t targetPort,
            const std::uint8_t* data,
            const int length) override
        {
            if (targetIp != "127.0.0.1" || targetPort != 30007 ||
                data == nullptr || length <= 0)
                return false;
            ++sendCount;
            return true;
        }

        std::size_t sendCount = 0;
    };

    void TestSessionAppliesEndpointClientRetryConfiguration()
    {
        AppConfig appConfig;
        appConfig.mavftpRequestTimeoutMs = 100;
        appConfig.mavftpTerminateTimeoutMs = 2500;
        appConfig.mavftpRequestRetryCount = 1;
        appConfig.mavftpWriteWindow = 4;
        CountingSender sender;
        DroneSessionConfig sessionConfig;
        sessionConfig.droneId = "Drone001";
        sessionConfig.remoteIp = "127.0.0.1";
        sessionConfig.remotePort = 30007;
        sessionConfig.receiveOnly = true;
        sessionConfig.mavFtpServiceEnabled = true;
        sessionConfig.mavFtpRequestTimeoutMs =
            appConfig.mavftpRequestTimeoutMs;
        sessionConfig.mavFtpTerminateTimeoutMs =
            appConfig.mavftpTerminateTimeoutMs;
        sessionConfig.mavFtpRequestRetryCount =
            appConfig.mavftpRequestRetryCount;
        sessionConfig.mavFtpWriteWindow = appConfig.mavftpWriteWindow;
        sessionConfig.sharedMavlinkEndpoint = &sender;
        sessionConfig.discoveredSystemId = 7;
        sessionConfig.discoveredComponentId = 1;
        DroneSession session(sessionConfig);
        Expect(session.Start(), "configured DroneSession did not start");
        DiscoveredDroneIdentity identity;
        identity.systemId = sessionConfig.discoveredSystemId;
        identity.componentId = sessionConfig.discoveredComponentId;
        identity.sourceIp = sessionConfig.remoteIp;
        identity.sourcePort =
            static_cast<std::uint16_t>(sessionConfig.remotePort);
        identity.observedAt = std::chrono::steady_clock::now();
        Expect(session.RefreshDiscoveredPresence(identity),
            "configured DroneSession did not become online");
        auto* client = session.GetMavFtpClient();
        std::string error;
        Expect(client && client->GetConfiguredWriteWindow() == 4 &&
            client->GetConfiguredRequestTimeout() ==
                std::chrono::milliseconds(100) &&
            client->GetConfiguredTerminateTimeout() ==
                std::chrono::milliseconds(2500) &&
            client->StartResetSessions(error),
            "endpoint-bound client did not start configured request");
        const auto startedAt = std::chrono::steady_clock::now();
        const std::size_t initialSends = sender.sendCount;
        client->Tick(startedAt + std::chrono::milliseconds(50));
        Expect(sender.sendCount == initialSends,
            "configured request retried before its 100 ms timeout");
        client->Tick(startedAt + std::chrono::milliseconds(150));
        Expect(sender.sendCount == initialSends + 1 &&
            client->GetStats().retriesSent == 1,
            "configured endpoint-bound client did not perform one retry");
        client->Tick(startedAt + std::chrono::milliseconds(300));
        const auto result = client->ConsumeResult();
        Expect(result && !result->success && result->retryCount == 1 &&
            !client->IsBusy(),
            "configured retry limit did not fail the endpoint-bound request");
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
        report.segmentCapacity = 4;
        report.segmentCount = 3;
        report.mavftpMetrics.segmentsPlanned = 3;
        report.mavftpMetrics.segmentsCommitted = 3;
        report.mavftpMetrics.writeBytes = 428;
        report.mavftpMetrics.writeBlocks = 2;
        report.mavftpMetrics.retryCount = 1;
        report.requestTimeoutMs = 750;
        report.terminateTimeoutMs = 2500;
        report.requestRetryCount = 3;
        report.configuredWriteWindow = 8;
        report.mavftpMetrics.configuredWriteWindow = 8;
        report.mavftpMetrics.maxObservedOutstanding = 8;
        report.mavftpMetrics.writeBlocksSent = 2;
        report.mavftpMetrics.writeBlocksAcked = 2;
        report.mavftpMetrics.writeDataDurationMs = 50;
        report.hardwareValidationDurationMs = 100;
        const std::string summary = report.ToHumanSummary();
        Expect(summary.find("ArtifactRouteItems=10") != std::string::npos &&
            summary.find("WireMissionItems=11") != std::string::npos &&
            summary.find("SegmentCapacityDomain=WireMissionItems") != std::string::npos &&
            summary.find("SegmentCapacity=4") != std::string::npos &&
            summary.find("SegmentCount=3") != std::string::npos &&
            summary.find("SegmentsCommitted=3") != std::string::npos &&
            summary.find("WriteBytes=428") != std::string::npos &&
            summary.find("WriteBlocks=2") != std::string::npos &&
            summary.find("RequestTimeoutMs=750") != std::string::npos &&
            summary.find("TerminateTimeoutMs=2500") != std::string::npos &&
            summary.find("RequestRetryCount=3") != std::string::npos &&
            summary.find("ConfiguredWriteWindow=8") != std::string::npos &&
            summary.find("WriteDataDurationMs=50") != std::string::npos &&
            summary.find("HardwareValidationDurationMs=100") != std::string::npos &&
            summary.find("RetryCount=1") != std::string::npos &&
            summary.find("RetriesTotal=1") != std::string::npos &&
            summary.find("TotalMavFtpDurationMs=") != std::string::npos,
            "hardware report omitted required compile or MAVFTP metrics");
    }
}

int RunMAVFTPMissionHardwareTestTests()
{
    try
    {
        TestDisabledHarnessRemainsValid();
        TestExplicitSafetyContract();
        TestRuntimeItemCountsAreNotFrozen();
        TestTransportConfigurationPrecedence();
        TestQuickModeResolvesCanonicalCurrentIdentity();
        TestSessionAppliesEndpointClientRetryConfiguration();
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
