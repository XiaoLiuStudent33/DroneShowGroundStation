/**
 * @brief 定义无人机发现阶段的 Endpoint 身份与注册结果。
 *
 * 分类：Production。
 * 输入：UDP Remote IP、UDP Source Port 和 MAVLink 路由元数据；输出：DroneManager 可消费的身份对象。
 * 关键约束：Remote IP + Source Port 是唯一身份；同 IP 不同 Port 仍是不同无人机。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

class DroneSession;

struct DiscoveredDroneIdentity
{
    // Fleet 唯一身份是 Remote IP + UDP Source Port。SYSID/COMPID 仅用于
    // MAVLink target_system/target_component 路由，允许多架模拟机使用相同 SYSID。
    std::uint8_t systemId = 0;
    std::uint8_t componentId = 0;
    std::string sourceIp;
    std::uint16_t sourcePort = 0;
    std::chrono::steady_clock::time_point observedAt{};

    std::string EndpointKey() const
    {
        return sourceIp + ":" + std::to_string(sourcePort);
    }
};

enum class DroneRegistrationStatus
{
    Created,
    Refreshed,
    DroneIdentityConflict,
    Rejected
};

struct DroneRegistrationResult
{
    DroneRegistrationStatus status = DroneRegistrationStatus::Rejected;
    std::string droneId;
    DroneSession* session = nullptr;
    std::string error;
};
