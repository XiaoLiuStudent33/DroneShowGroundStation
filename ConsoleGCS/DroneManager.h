#pragma once

#include "DroneDiscovery.h"
#include "DroneSession.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class DroneManager
{
public:
    using DroneIdResolver =
        std::function<std::string(const DiscoveredDroneIdentity&)>;

    explicit DroneManager(DroneIdResolver droneIdResolver = {});

    bool AddDrone(const DroneSessionConfig& config);
    DroneRegistrationResult RegisterOrRefreshDrone(
        const DiscoveredDroneIdentity& identity,
        const DroneSessionConfig& sessionDefaults);
    bool RemoveDrone(const std::string& droneId);

    DroneSession* FindDrone(const std::string& droneId);
    const DroneSession* FindDrone(const std::string& droneId) const;
    DroneSession* FindDroneByIdentity(
        const DiscoveredDroneIdentity& identity);
    const DroneSession* FindDroneByIdentity(
        const DiscoveredDroneIdentity& identity) const;

    DroneSession* GetDrone(const std::string& droneId);
    const DroneSession* GetDrone(const std::string& droneId) const;
    std::vector<DroneSession*> GetAllDrones();
    std::vector<const DroneSession*> GetAllDrones() const;

    bool StartDrone(const std::string& droneId);
    void RefreshHeartbeatStates(
        std::chrono::steady_clock::time_point now);
    void TickAll(std::chrono::steady_clock::time_point now);
    void StopAll();

    std::vector<std::string> GetDroneIds() const;
    std::size_t GetDroneCount() const;

private:
    DroneIdResolver droneIdResolver;
    std::unordered_map<std::string, std::unique_ptr<DroneSession>> sessions;
    std::unordered_map<std::string, std::string> discoveredEndpointIndex;
    std::vector<std::string> sessionOrder;
    std::size_t nextDiscoveredDroneNumber = 1;
};
