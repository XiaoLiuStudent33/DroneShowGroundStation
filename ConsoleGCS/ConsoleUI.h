#pragma once

//--------------------------------------------------
// Windows
//--------------------------------------------------

#include <windows.h>

//--------------------------------------------------
// C++
//--------------------------------------------------

#include <string>
#include <vector>

//--------------------------------------------------
// 工程
//--------------------------------------------------

#include "FlightData.h"
#include "HoverTest.h"

//==================================================
// Console backend fleet status row.
//==================================================

struct ConsoleDroneStatus
{
    std::string droneId;
    bool online = false;
};

class ConsoleUI
{
public:

    //--------------------------------------------------
    // 初始化控制台
    //--------------------------------------------------

    static void Init();

    //--------------------------------------------------
    // Update backend fleet overview and drone list.
    //--------------------------------------------------

    static void UpdateFleet(
        const std::vector<ConsoleDroneStatus>& drones);

    //--------------------------------------------------
    // Compatibility adapters for legacy callers. They no longer display
    // single-drone telemetry and should not be used by the backend service.
    //--------------------------------------------------

    static void Update(
        const FlightData& data,
        const HoverTest& test);

    static void Update(
        const std::string& droneId,
        const FlightData& data,
        const HoverTest& test);

    //--------------------------------------------------
    // 添加运行日志
    //--------------------------------------------------

    static void AddLog(
        const std::string& text);

private:

    //--------------------------------------------------
    // 绘制固定界面
    //--------------------------------------------------

    static void DrawFrame();

    //--------------------------------------------------
    // 指定位置输出文本
    //--------------------------------------------------

    static void Print(
        short x,
        short y,
        const std::string& text,
        WORD color = 7);

    //--------------------------------------------------
    // 设置文字颜色
    //--------------------------------------------------

    static void SetColor(
        WORD color);

    //--------------------------------------------------
    // 移动光标
    //--------------------------------------------------

    static void MoveCursor(
        short x,
        short y);

    //--------------------------------------------------
    // 清空指定行
    //--------------------------------------------------

    static void ClearLine(
        short y);

private:

    //--------------------------------------------------
    // 当前日志输出位置
    //--------------------------------------------------

    static int logLine;
};
