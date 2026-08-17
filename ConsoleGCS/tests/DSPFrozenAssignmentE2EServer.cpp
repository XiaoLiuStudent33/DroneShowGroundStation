#include "DSPGCSFrozenAssignmentReceiver.h"
#include "DSPGCSProductionTelemetryProvider.h"
#include "DroneManager.h"
#include "FrozenAssignmentStore.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    struct ServerOptions
    {
        std::uint16_t consolePort = 0;
        std::uint16_t remotePort = 0;
        std::size_t droneCount = 0;
        std::filesystem::path resultPath;
        std::filesystem::path stopPath;
        int timeoutSeconds = 90;
    };

    std::string DroneId(const std::size_t index)
    {
        std::ostringstream stream;
        stream << "Drone" << std::setw(6) << std::setfill('0') << index;
        return stream.str();
    }

    std::string DigestHex(const dspgcs::FrozenAssignmentDigest& digest)
    {
        static constexpr char Hex[] = "0123456789abcdef";
        std::string output;
        output.reserve(digest.size() * 2);
        for (const std::uint8_t value : digest)
        {
            output.push_back(Hex[(value >> 4) & 0x0f]);
            output.push_back(Hex[value & 0x0f]);
        }
        return output;
    }

    bool ParseUnsigned(const char* text, std::uint64_t& output)
    {
        try
        {
            const std::string value(text ? text : "");
            std::size_t consumed = 0;
            output = std::stoull(value, &consumed, 10);
            return consumed == value.size();
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseOptions(const int argc, char** argv, ServerOptions& output)
    {
        for (int index = 2; index + 1 < argc; index += 2)
        {
            const std::string key = argv[index];
            const char* value = argv[index + 1];
            std::uint64_t number = 0;
            if (key == "--console-port" && ParseUnsigned(value, number) && number <= 65535)
            {
                output.consolePort = static_cast<std::uint16_t>(number);
            }
            else if (key == "--remote-port" && ParseUnsigned(value, number) && number <= 65535)
            {
                output.remotePort = static_cast<std::uint16_t>(number);
            }
            else if (key == "--drones" && ParseUnsigned(value, number))
            {
                output.droneCount = static_cast<std::size_t>(number);
            }
            else if (key == "--result")
            {
                output.resultPath = value;
            }
            else if (key == "--stop")
            {
                output.stopPath = value;
            }
            else if (key == "--timeout" && ParseUnsigned(value, number) && number <= 600)
            {
                output.timeoutSeconds = static_cast<int>(number);
            }
            else
            {
                return false;
            }
        }
        return argc >= 12 && output.consolePort != 0 && output.remotePort != 0 &&
            output.droneCount != 0 && !output.resultPath.empty() && !output.stopPath.empty();
    }

    void AddDrones(DroneManager& manager, const std::size_t count)
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            DroneSessionConfig config;
            config.droneId = DroneId(index);
            if (!manager.AddDrone(config))
            {
                throw std::runtime_error("failed to add E2E DroneSession");
            }
        }
    }

    struct ServerMetrics
    {
        std::size_t framesProcessed = 0;
        std::size_t transactionsAccepted = 0;
        std::size_t transactionsRejected = 0;
        std::size_t acknowledgementsSent = 0;
        std::size_t acceptedAcks = 0;
        std::size_t rejectedAcks = 0;
        std::size_t storeMutationCount = 0;
        std::size_t maximumInboundDatagramBytes = 0;
        std::uint16_t lastErrorCode = 0;
        long long processPendingMicroseconds = 0;
    };

    void WriteResult(
        const ServerOptions& options,
        const std::string& sessionId,
        const FrozenAssignmentStore& store,
        const ServerMetrics& metrics)
    {
        const auto snapshot = store.GetAcceptedSnapshot(sessionId);
        std::size_t mappingMismatches = 0;
        if (snapshot)
        {
            for (std::size_t index = 0; index < snapshot->matchedMappings.size(); ++index)
            {
                std::int32_t routeSlot = -1;
                if (!store.TryGetRouteSlotForDroneID(sessionId, DroneId(index), routeSlot) ||
                    routeSlot != static_cast<std::int32_t>(index))
                {
                    ++mappingMismatches;
                }
            }
        }

        const std::filesystem::path temporary = options.resultPath.string() + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "session=" << sessionId << '\n'
            << "framesProcessed=" << metrics.framesProcessed << '\n'
            << "transactionsAccepted=" << metrics.transactionsAccepted << '\n'
            << "transactionsRejected=" << metrics.transactionsRejected << '\n'
            << "acknowledgementsSent=" << metrics.acknowledgementsSent << '\n'
            << "acceptedAcks=" << metrics.acceptedAcks << '\n'
            << "rejectedAcks=" << metrics.rejectedAcks << '\n'
            << "lastErrorCode=" << metrics.lastErrorCode << '\n'
            << "storeMutationCount=" << metrics.storeMutationCount << '\n'
            << "maximumInboundDatagramBytes=" << metrics.maximumInboundDatagramBytes << '\n'
            << "processPendingMicroseconds=" << metrics.processPendingMicroseconds << '\n'
            << "storePresent=" << (snapshot ? 1 : 0) << '\n'
            << "storeRevision=" << (snapshot ? snapshot->identity.assignmentRevision : 0) << '\n'
            << "storeAuthority=" << (snapshot ? snapshot->identity.assignmentAuthoritySessionId : "") << '\n'
            << "storeDigest=" << (snapshot ? DigestHex(snapshot->canonicalPayloadDigest) : "") << '\n'
            << "storeMatched=" << (snapshot ? snapshot->matchedCount : 0) << '\n'
            << "storeVacant=" << (snapshot ? snapshot->vacantCount : 0) << '\n'
            << "storeCompletion=" << (snapshot ? static_cast<int>(snapshot->completionMode) : 0) << '\n'
            << "mappingMismatches=" << mappingMismatches << '\n';
        output.close();
        std::error_code error;
        std::filesystem::remove(options.resultPath, error);
        error.clear();
        std::filesystem::rename(temporary, options.resultPath, error);
    }
}

int RunDSPFrozenAssignmentE2EServer(const int argc, char** argv)
{
    try
    {
        ServerOptions options;
        if (!ParseOptions(argc, argv, options))
        {
            return 64;
        }

        std::error_code fileError;
        std::filesystem::remove(options.resultPath, fileError);
        fileError.clear();
        std::filesystem::remove(options.stopPath, fileError);

        DroneManager drones;
        AddDrones(drones, options.droneCount);
        FrozenAssignmentStore store;
        DSPGCSFrozenAssignmentReceiver receiver;
        ServerMetrics metrics;
        std::atomic<std::size_t> maximumInboundDatagramBytes{0};

        DSPGCSProductionTelemetryProvider provider;
        provider.SetReceiveHandler([&receiver, &maximumInboundDatagramBytes](
            dspgcs::TransportFrame frame)
        {
            std::size_t observed = maximumInboundDatagramBytes.load();
            while (observed < frame.bytes.size() &&
                !maximumInboundDatagramBytes.compare_exchange_weak(observed, frame.bytes.size()))
            {
            }
            receiver.EnqueueFrame(std::move(frame));
        });

        dspgcs::DSPGCSTransportConfig config = dspgcs::MakeConsoleDSPGCSDefaults();
        config.bindAddress = "127.0.0.1";
        config.remoteAddress = "127.0.0.1";
        config.bindPort = options.consolePort;
        config.remotePort = options.remotePort;
        config.receiverPollMs = 1;
        std::string error;
        if (!provider.Start(config, 60000, 256, error))
        {
            return 65;
        }

        const std::string sessionId = provider.GetCurrentFleetSessionID();
        store.SynchronizeFleetSession(sessionId);
        if (!provider.PublishSnapshot(drones))
        {
            provider.Stop();
            return 66;
        }

        std::uint64_t observedRevision = 0;
        dspgcs::FrozenAssignmentDigest observedDigest{};
        bool hasObservedStore = false;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(options.timeoutSeconds);
        while (std::chrono::steady_clock::now() < deadline &&
            !std::filesystem::exists(options.stopPath))
        {
            const auto started = std::chrono::steady_clock::now();
            const DSPGCSFrozenAssignmentProcessStats stats = receiver.ProcessPending(
                sessionId,
                drones,
                store,
                [&provider, &metrics](const dspgcs::FrozenAssignmentAckDTO& ack)
                {
                    if (ack.result == dspgcs::FrozenAssignmentAckResult::Accepted)
                    {
                        ++metrics.acceptedAcks;
                    }
                    else
                    {
                        ++metrics.rejectedAcks;
                    }
                    metrics.lastErrorCode = static_cast<std::uint16_t>(ack.errorCode);
                    return provider.SendFrozenAssignmentAck(ack);
                });
            metrics.processPendingMicroseconds +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count();
            metrics.framesProcessed += stats.framesProcessed;
            metrics.transactionsAccepted += stats.transactionsAccepted;
            metrics.transactionsRejected += stats.transactionsRejected;
            metrics.acknowledgementsSent += stats.acknowledgementsSent;

            const auto accepted = store.GetAcceptedSnapshot(sessionId);
            if (accepted && (!hasObservedStore ||
                accepted->identity.assignmentRevision != observedRevision ||
                accepted->canonicalPayloadDigest != observedDigest))
            {
                hasObservedStore = true;
                observedRevision = accepted->identity.assignmentRevision;
                observedDigest = accepted->canonicalPayloadDigest;
                ++metrics.storeMutationCount;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        metrics.maximumInboundDatagramBytes = maximumInboundDatagramBytes.load();
        WriteResult(options, sessionId, store, metrics);
        provider.Stop();
        return 0;
    }
    catch (...)
    {
        return 70;
    }
}
