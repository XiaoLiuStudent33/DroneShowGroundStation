#pragma once

/**
 * @brief 为一个 DroneSession 提供线程安全的 FlightData 快照存取。
 *
 * 分类：Production。
 * 输入：MavlinkParser/DroneSession 更新；输出：ConsoleUI、Command、DSP Snapshot 使用的一致副本。
 * 调用链：DroneSession -> DataManager -> Console/DSP/FlightControl。
 */

#include "FlightData.h"

#include <chrono>
#include <mutex>

//==================================================
// 数据管理器
// 负责保存整个程序唯一的一份飞控数据
//==================================================
class DataManager
{
public:

    //--------------------------------------------------
    // 更新数据
    //--------------------------------------------------
    void Update(const FlightData& data);

    //--------------------------------------------------
    // 获取数据（只读）
    //--------------------------------------------------
    FlightData GetFlightData() const;


   
    
    //--------------------------------------------------
    // 判断飞控是否在线
    //--------------------------------------------------
    bool IsOnline() const;

    //--------------------------------------------------
    // 根据最近一次心跳刷新在线状态
    //--------------------------------------------------
    bool RefreshOnlineStatus(
        std::chrono::steady_clock::time_point now,
        std::chrono::milliseconds timeout);

private:

    //--------------------------------------------------
    // 当前飞控数据
    //--------------------------------------------------
    FlightData flightData;

    mutable std::mutex mutex;
};
