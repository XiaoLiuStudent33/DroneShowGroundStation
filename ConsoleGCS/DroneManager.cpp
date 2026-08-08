/**
 * @brief 实现 Endpoint 索引下的 Session 创建、恢复、查询和稳定遍历。
 *
 * 分类：Production。
 * 输入：DiscoveredDroneIdentity；输出：已有 Session 刷新或新 Session 创建。
 * 不得用 SYSID 合并无人机，也不得因 Heartbeat timeout 从集合中移除 Session。
 */
#include "DroneManager.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace
{
    std::string MakeSequentialDroneId(const std::size_t number)
    {
        std::ostringstream stream;
        stream << "Drone" << std::setw(3) << std::setfill('0')
               << number;
        return stream.str();
    }
}

DroneManager::DroneManager(DroneIdResolver droneIdResolverValue)
    : droneIdResolver(std::move(droneIdResolverValue))
{
}

bool DroneManager::AddDrone(const DroneSessionConfig& config)
{
    if (config.droneId.empty() ||
        sessions.find(config.droneId) != sessions.end())
    {
        return false;
    }

    auto session = std::make_unique<DroneSession>(config);
    sessionOrder.push_back(config.droneId);
    sessions.emplace(config.droneId, std::move(session));
    return true;
}

DroneRegistrationResult DroneManager::RegisterOrRefreshDrone(
    const DiscoveredDroneIdentity& identity,
    const DroneSessionConfig& sessionDefaults)
{
    DroneRegistrationResult result;
    if (identity.systemId == 0 ||
        identity.componentId == 0 ||
        identity.sourceIp.empty() ||
        identity.sourcePort == 0)
    {
        result.error = "invalid discovered drone identity";
        return result;
    }

    // EndpointKey 才是 Fleet 唯一键：不同 IP，或同 IP 不同 Source Port，
    // 都代表不同无人机；相同 Endpoint 才允许恢复原 DroneID/SessionOrder。
    const std::string endpointKey = identity.EndpointKey();
    const auto knownEndpoint = discoveredEndpointIndex.find(endpointKey);
    if (knownEndpoint != discoveredEndpointIndex.end())
    {
        result.droneId = knownEndpoint->second;
    }
    else if (droneIdResolver)
    {
        result.droneId = droneIdResolver(identity);
    }
    else
    {
        do
        {
            result.droneId =
                MakeSequentialDroneId(nextDiscoveredDroneNumber++);
        }
        while (FindDrone(result.droneId) != nullptr);
    }
    if (result.droneId.empty())
    {
        result.error = "DroneID resolver returned an empty identity";
        return result;
    }

    if (DroneSession* existing = FindDrone(result.droneId))
    {
        const DroneSessionConfig& existingConfig = existing->GetConfig();
        if (existingConfig.remoteIp != identity.sourceIp ||
            existingConfig.remotePort != identity.sourcePort)
        {
            result.status =
                DroneRegistrationStatus::DroneIdentityConflict;
            result.error =
                "DroneIdentityConflict: DroneID=" + result.droneId +
                " existingEndpoint=" + existingConfig.remoteIp + ":" +
                std::to_string(existingConfig.remotePort) +
                " incomingEndpoint=" + identity.sourceIp + ":" +
                std::to_string(identity.sourcePort);
            return result;
        }

        // Heartbeat 恢复只刷新原 Session。即使它此前已 Offline，也不创建
        // 第二个 DroneID，更不改变已建立的 SessionOrder。
        if (!existing->RefreshDiscoveredPresence(identity))
        {
            result.error = "failed to refresh discovered DroneSession";
            return result;
        }

        result.status = DroneRegistrationStatus::Refreshed;
        result.session = existing;
        return result;
    }

    DroneSessionConfig config = sessionDefaults;
    config.droneId = result.droneId;
    config.remoteIp = identity.sourceIp;
    config.remotePort = identity.sourcePort;
    config.discoveredSystemId = identity.systemId;
    config.discoveredComponentId = identity.componentId;

    if (!AddDrone(config))
    {
        result.error = "failed to create discovered DroneSession";
        return result;
    }

    DroneSession* session = FindDrone(result.droneId);
    if (session == nullptr || !session->Start())
    {
        RemoveDrone(result.droneId);
        result.error = "failed to start discovered DroneSession";
        return result;
    }

    if (!session->RefreshDiscoveredPresence(identity))
    {
        RemoveDrone(result.droneId);
        result.error = "failed to initialize discovered DroneSession identity";
        return result;
    }

    result.status = DroneRegistrationStatus::Created;
    result.session = session;
    discoveredEndpointIndex[endpointKey] = result.droneId;
    return result;
}

bool DroneManager::RemoveDrone(const std::string& droneId)
{
    const auto item = sessions.find(droneId);

    if (item == sessions.end())
    {
        return false;
    }

    const DroneSessionConfig removedConfig = item->second->GetConfig();
    item->second->Stop();
    sessions.erase(item);
    if (!removedConfig.remoteIp.empty() && removedConfig.remotePort != 0)
    {
        DiscoveredDroneIdentity removedIdentity;
        removedIdentity.sourceIp = removedConfig.remoteIp;
        removedIdentity.sourcePort =
            static_cast<std::uint16_t>(removedConfig.remotePort);
        const auto indexItem =
            discoveredEndpointIndex.find(
                removedIdentity.EndpointKey());
        if (indexItem != discoveredEndpointIndex.end() &&
            indexItem->second == droneId)
        {
            discoveredEndpointIndex.erase(indexItem);
        }
    }
    sessionOrder.erase(
        std::remove(sessionOrder.begin(), sessionOrder.end(), droneId),
        sessionOrder.end());
    return true;
}

DroneSession* DroneManager::FindDrone(const std::string& droneId)
{
    const auto item = sessions.find(droneId);
    return item == sessions.end() ? nullptr : item->second.get();
}

const DroneSession* DroneManager::FindDrone(
    const std::string& droneId) const
{
    const auto item = sessions.find(droneId);
    return item == sessions.end() ? nullptr : item->second.get();
}

DroneSession* DroneManager::FindDroneByIdentity(
    const DiscoveredDroneIdentity& identity)
{
    return const_cast<DroneSession*>(
        static_cast<const DroneManager&>(*this).FindDroneByIdentity(identity));
}

const DroneSession* DroneManager::FindDroneByIdentity(
    const DiscoveredDroneIdentity& identity) const
{
    const auto indexItem =
        discoveredEndpointIndex.find(identity.EndpointKey());
    if (indexItem == discoveredEndpointIndex.end())
    {
        return nullptr;
    }

    const DroneSession* session = FindDrone(indexItem->second);
    if (session == nullptr)
    {
        return nullptr;
    }

    const DroneSessionConfig& config = session->GetConfig();
    return config.remoteIp == identity.sourceIp &&
        config.remotePort == identity.sourcePort
        ? session
        : nullptr;
}

std::vector<DroneSession*> DroneManager::GetAllDrones()
{
    std::vector<DroneSession*> result;
    result.reserve(sessionOrder.size());

    for (const std::string& droneId : sessionOrder)
    {
        if (DroneSession* session = FindDrone(droneId))
        {
            result.push_back(session);
        }
    }

    return result;
}

std::vector<const DroneSession*> DroneManager::GetAllDrones() const
{
    std::vector<const DroneSession*> result;
    result.reserve(sessionOrder.size());

    for (const std::string& droneId : sessionOrder)
    {
        if (const DroneSession* session = FindDrone(droneId))
        {
            result.push_back(session);
        }
    }

    return result;
}

bool DroneManager::StartDrone(const std::string& droneId)
{
    DroneSession* session = FindDrone(droneId);
    return session != nullptr && session->Start();
}

void DroneManager::RefreshHeartbeatStates(
    const std::chrono::steady_clock::time_point now)
{
    for (const std::string& droneId : sessionOrder)
    {
        DroneSession* session = FindDrone(droneId);
        if (session != nullptr)
        {
            session->RefreshHeartbeatStatus(now);
        }
    }
}

void DroneManager::TickAll(
    std::chrono::steady_clock::time_point now)
{
    for (const std::string& droneId : sessionOrder)
    {
        DroneSession* session = FindDrone(droneId);

        if (session != nullptr)
        {
            session->Tick(now);
        }
    }
}

void DroneManager::StopAll()
{
    for (const std::string& droneId : sessionOrder)
    {
        DroneSession* session = FindDrone(droneId);

        if (session != nullptr)
        {
            session->Stop();
        }
    }
}

std::vector<std::string> DroneManager::GetDroneIds() const
{
    return sessionOrder;
}

std::size_t DroneManager::GetDroneCount() const
{
    return sessions.size();
}
