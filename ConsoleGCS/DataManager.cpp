#include "DataManager.h"

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
