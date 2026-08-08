/**
 * @brief 管理全部 DroneSession、稳定 DroneID、Endpoint 索引和 SessionOrder。
 *
 * 分类：Production。
 * 输入：发现身份、Session 默认配置和命令查询；输出：稳定有序的 Fleet Session 集合。
 * 调用链：Discovery/Command -> DroneManager -> DroneSession。
 * 关键约束：Offline 不删除 Session，SessionOrder 是下游稳定 Fleet 顺序的来源。
 */
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
