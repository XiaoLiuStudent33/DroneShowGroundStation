#include "DataManager.h"

/**
 * @brief 实现每个 Session 的 FlightData 加锁更新与快照读取。
 *
 * 分类：Production。
 * 该类不是全局单例；每个 DroneSession 拥有自己的 DataManager，避免多机状态互相覆盖。
 */

//==================================================
// 更新飞控数据
//==================================================
void DataManager::Update(const FlightData& data)
{
    std::lock_guard<std::mutex> lock(mutex);
    flightData = data;
}

//==================================================
// 获取飞控数据
//==================================================
FlightData DataManager::GetFlightData() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return flightData;
}

//==================================================
// 判断飞控是否在线
//==================================================
bool DataManager::IsOnline() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return flightData.online;
}

bool DataManager::RefreshOnlineStatus(
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds timeout)
{
    std::lock_guard<std::mutex> lock(mutex);

    const bool wasOnline = flightData.online;

    if (flightData.lastHeartbeat ==
        std::chrono::steady_clock::time_point{})
    {
        flightData.online = false;
    }
    else
    {
        flightData.online =
            now - flightData.lastHeartbeat <= timeout;
    }

    return wasOnline != flightData.online;
}
