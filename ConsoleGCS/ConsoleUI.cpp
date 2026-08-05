#include "ConsoleUI.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <ctime>
#include <mutex>
#include <algorithm>

using namespace std;

namespace
{
    std::mutex consoleMutex;
}

//==================================================
// Console Layout
//==================================================

constexpr short CONSOLE_WIDTH = 110;
constexpr short CONSOLE_HEIGHT = 36;

constexpr short VALUE_X = 16;
constexpr short RIGHT_LABEL_X = 44;
constexpr short RIGHT_VALUE_X = 62;

short runtimeConsoleWidth = CONSOLE_WIDTH;
short runtimeConsoleHeight = CONSOLE_HEIGHT;
short runtimeWindowWidth = CONSOLE_WIDTH;
short runtimeWindowHeight = CONSOLE_HEIGHT;

//--------------------------------------------------
// Connection
//--------------------------------------------------

constexpr short Y_CONNECTION = 5;
constexpr short Y_DRONE_ID = 6;

//--------------------------------------------------
// Flight Controller
//--------------------------------------------------

constexpr short Y_SYSID = 8;
constexpr short Y_COMPID = 9;
constexpr short Y_MODE = 10;
constexpr short Y_ARMED = 11;
constexpr short Y_AUTOPILOT = 12;
constexpr short Y_VEHICLE = 13;
constexpr short Y_STATE = 14;

//--------------------------------------------------
// Attitude
//--------------------------------------------------

constexpr short Y_ROLL = 5;
constexpr short Y_PITCH = 6;
constexpr short Y_YAW = 7;

//--------------------------------------------------
// GPS
//--------------------------------------------------

constexpr short Y_LAT = 10;
constexpr short Y_LON = 11;
constexpr short Y_ALT = 12;
constexpr short Y_GPS_FIX = 13;
constexpr short Y_SATELLITES = 14;

//--------------------------------------------------
// Battery
//--------------------------------------------------

constexpr short Y_BATTERY = 17;

//--------------------------------------------------
// Hover Test
//--------------------------------------------------

constexpr short Y_HOVER_STATUS = 17;
constexpr short Y_HOVER_TIME = 18;
constexpr short Y_HOVER_DRIFT = 19;
constexpr short Y_HOVER_MAX = 20;

//--------------------------------------------------
// Runtime Log
//--------------------------------------------------

constexpr short LOG_START = 28;
constexpr short LOG_END = 35;

short runtimeLogEnd = LOG_END;

//==================================================

int ConsoleUI::logLine = LOG_START;

//==================================================
// 初始化控制台
//==================================================

void ConsoleUI::Init()
{
    std::lock_guard<std::mutex> lock(consoleMutex);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    const COORD largestWindow =
        GetLargestConsoleWindowSize(hOut);

    runtimeWindowWidth =
        (largestWindow.X > 0)
        ? (std::min)(CONSOLE_WIDTH, largestWindow.X)
        : CONSOLE_WIDTH;

    runtimeWindowHeight =
        (largestWindow.Y > 0)
        ? (std::min)(CONSOLE_HEIGHT, largestWindow.Y)
        : CONSOLE_HEIGHT;

    runtimeConsoleWidth =
        (std::max)(runtimeWindowWidth, static_cast<short>(90));

    runtimeConsoleHeight =
        (std::max)(runtimeWindowHeight, static_cast<short>(29));

    runtimeLogEnd =
        (std::min)(
            LOG_END,
            static_cast<short>(runtimeConsoleHeight - 1));

    // Shrink the visible window first so reducing the screen buffer
    // does not fail when Visual Studio starts with a larger console.
    SMALL_RECT temporaryRect = { 0, 0, 1, 1 };
    SetConsoleWindowInfo(
        hOut,
        TRUE,
        &temporaryRect);

    COORD size =
    {
        runtimeConsoleWidth,
        runtimeConsoleHeight
    };

    SetConsoleScreenBufferSize(
        hOut,
        size);

    SMALL_RECT rect;

    rect.Left = 0;
    rect.Top = 0;
    rect.Right = runtimeWindowWidth - 1;
    rect.Bottom = runtimeWindowHeight - 1;

    SetConsoleWindowInfo(
        hOut,
        TRUE,
        &rect);

    DWORD written;

    COORD home = { 0,0 };

    FillConsoleOutputCharacter(
        hOut,
        ' ',
        runtimeConsoleWidth * runtimeConsoleHeight,
        home,
        &written);

    FillConsoleOutputAttribute(
        hOut,
        FOREGROUND_RED |
        FOREGROUND_GREEN |
        FOREGROUND_BLUE,
        runtimeConsoleWidth * runtimeConsoleHeight,
        home,
        &written);

    SetConsoleCursorPosition(
        hOut,
        home);

    CONSOLE_CURSOR_INFO cursor;

    cursor.dwSize = 20;
    cursor.bVisible = FALSE;

    SetConsoleCursorInfo(
        hOut,
        &cursor);

    DrawFrame();

    ClearValues();

    logLine = LOG_START;

    MoveCursor(0, 0);
}

//==================================================
// 设置颜色
//==================================================

void ConsoleUI::SetColor(
    WORD color)
{
    SetConsoleTextAttribute(
        GetStdHandle(STD_OUTPUT_HANDLE),
        color);
}

//==================================================
// 移动光标
//==================================================

void ConsoleUI::MoveCursor(
    short x,
    short y)
{
    COORD pos =
    {
        x,
        y
    };

    SetConsoleCursorPosition(
        GetStdHandle(STD_OUTPUT_HANDLE),
        pos);
}

//==================================================
// 输出字符串
//==================================================

void ConsoleUI::Print(
    short x,
    short y,
    const string& text,
    WORD color)
{
    MoveCursor(
        x,
        y);

    SetColor(color);

    cout
        << left
        << setw(24)
        << text;

    SetColor(7);
}

//==================================================
// 输出整数
//==================================================

void ConsoleUI::PrintInt(
    short x,
    short y,
    int value,
    WORD color)
{
    ostringstream oss;

    oss << value;

    Print(
        x,
        y,
        oss.str(),
        color);
}

//==================================================
// 输出浮点数
//==================================================

void ConsoleUI::PrintDouble(
    short x,
    short y,
    double value,
    int precision,
    const string& suffix,
    WORD color)
{
    ostringstream oss;

    oss
        << fixed
        << setprecision(precision)
        << value
        << suffix;

    Print(
        x,
        y,
        oss.str(),
        color);
}

//==================================================
// 清空一行
//==================================================

void ConsoleUI::ClearLine(
    short y)
{
    HANDLE hOut =
        GetStdHandle(STD_OUTPUT_HANDLE);

    COORD pos =
    {
        0,
        y
    };

    DWORD written;

    FillConsoleOutputCharacter(
        hOut,
        ' ',
        runtimeConsoleWidth,
        pos,
        &written);

    FillConsoleOutputAttribute(
        hOut,
        FOREGROUND_RED |
        FOREGROUND_GREEN |
        FOREGROUND_BLUE,
        runtimeConsoleWidth,
        pos,
        &written);
}

//==================================================
// 绘制固定界面
//==================================================

void ConsoleUI::DrawFrame()
{
    const std::string majorLine(
        static_cast<std::size_t>(runtimeConsoleWidth),
        '=');
    const std::string minorLine(
        static_cast<std::size_t>(runtimeConsoleWidth),
        '-');

    SetColor(11);
    MoveCursor(0, 0);
    cout << majorLine;
    MoveCursor(39, 1);
    cout << "Drone GCS - Vehicle Telemetry";
    MoveCursor(0, 2);
    cout << majorLine;
    SetColor(7);

    MoveCursor(0, 4);
    cout << "[Connection]";
    MoveCursor(0, 5);
    cout << "Status      :";
    MoveCursor(0, Y_DRONE_ID);
    cout << "Drone ID    :";

    MoveCursor(0, 7);
    cout << "[Flight Controller]";
    MoveCursor(0, 8);
    cout << "System ID   :";
    MoveCursor(0, 9);
    cout << "Component   :";
    MoveCursor(0, 10);
    cout << "Mode        :";
    MoveCursor(0, 11);
    cout << "Armed       :";
    MoveCursor(0, 12);
    cout << "Autopilot   :";
    MoveCursor(0, 13);
    cout << "Vehicle     :";
    MoveCursor(0, 14);
    cout << "State       :";

    MoveCursor(RIGHT_LABEL_X, 4);
    cout << "[Attitude]";
    MoveCursor(RIGHT_LABEL_X, Y_ROLL);
    cout << "Roll        :";
    MoveCursor(RIGHT_LABEL_X, Y_PITCH);
    cout << "Pitch       :";
    MoveCursor(RIGHT_LABEL_X, Y_YAW);
    cout << "Yaw         :";

    MoveCursor(RIGHT_LABEL_X, 9);
    cout << "[GPS]";
    MoveCursor(RIGHT_LABEL_X, Y_LAT);
    cout << "Latitude    :";
    MoveCursor(RIGHT_LABEL_X, Y_LON);
    cout << "Longitude   :";
    MoveCursor(RIGHT_LABEL_X, Y_ALT);
    cout << "Altitude    :";
    MoveCursor(RIGHT_LABEL_X, Y_GPS_FIX);
    cout << "Fix Type    :";
    MoveCursor(RIGHT_LABEL_X, Y_SATELLITES);
    cout << "Satellites  :";

    MoveCursor(0, 16);
    cout << "[Hover Test]";
    MoveCursor(0, Y_HOVER_STATUS);
    cout << "Status      :";
    MoveCursor(0, Y_HOVER_TIME);
    cout << "Time        :";
    MoveCursor(0, Y_HOVER_DRIFT);
    cout << "Drift       :";
    MoveCursor(0, Y_HOVER_MAX);
    cout << "Max Drift   :";

    MoveCursor(RIGHT_LABEL_X, 16);
    cout << "[Battery]";
    MoveCursor(RIGHT_LABEL_X, Y_BATTERY);
    cout << "Remaining   :";

    SetColor(14);
    MoveCursor(0, 22);
    cout << majorLine;
    MoveCursor(0, 23);
    cout << "[A] ARM [D] DISARM [G] GUIDED [T] TAKEOFF [R] RTL [L] LAND [H] HOVER [X] STOP";
    MoveCursor(0, 24);
    cout << majorLine;
    SetColor(7);

    MoveCursor(0, 26);
    cout << "Runtime Log";
    MoveCursor(0, 27);
    cout << minorLine;

    for (short y = LOG_START; y <= runtimeLogEnd; y++)
    {
        ClearLine(y);
    }

    MoveCursor(0, 0);
}

//==================================================
// 更新整个界面
//==================================================

void ConsoleUI::Update(
    const FlightData& data,
    const HoverTest& test)
{
    Update("", data, test);
}

void ConsoleUI::Update(
    const std::string& droneId,
    const FlightData& data,
    const HoverTest& test)
{
    std::lock_guard<std::mutex> lock(consoleMutex);

    Print(VALUE_X, Y_DRONE_ID, droneId.empty() ? "N/A" : droneId, 11);

    UpdateConnection(data);

    UpdateFlightInfo(data);

    UpdateAttitude(data);

    UpdateGPS(data);

    UpdateBattery(data);

    UpdateHoverTest(test);

    MoveCursor(0, 0);
}

//==================================================
// 更新连接状态
//==================================================

void ConsoleUI::UpdateConnection(
    const FlightData& data)
{
    Print(
        VALUE_X,
        Y_CONNECTION,
        data.online ? "Online" : "Offline",
        data.online ? 10 : 12);
}
//==================================================
// 更新飞控信息
//==================================================

void ConsoleUI::UpdateFlightInfo(
    const FlightData& data)
{
    PrintInt(
        VALUE_X,
        Y_SYSID,
        data.systemID);

    PrintInt(
        VALUE_X,
        Y_COMPID,
        data.componentID);

    Print(
        VALUE_X,
        Y_MODE,
        data.modeName,
        11);

    Print(
        VALUE_X,
        Y_ARMED,
        data.armed ? "YES" : "NO",
        data.armed ? 10 : 12);

    Print(
        VALUE_X,
        Y_AUTOPILOT,
        data.autopilotName);

    Print(
        VALUE_X,
        Y_VEHICLE,
        data.vehicleName);

    Print(
        VALUE_X,
        Y_STATE,
        data.systemStatusName);
}

//==================================================
// 更新姿态
//==================================================

void ConsoleUI::UpdateAttitude(
    const FlightData& data)
{
    if (!data.attitudeValid)
    {
        Print(RIGHT_VALUE_X, Y_ROLL, "N/A", 8);
        Print(RIGHT_VALUE_X, Y_PITCH, "N/A", 8);
        Print(RIGHT_VALUE_X, Y_YAW, "N/A", 8);
        return;
    }

    constexpr double RAD2DEG =
        180.0 / 3.14159265358979323846;

    PrintDouble(
        RIGHT_VALUE_X,
        Y_ROLL,
        data.roll * RAD2DEG,
        2,
        " deg");

    PrintDouble(
        RIGHT_VALUE_X,
        Y_PITCH,
        data.pitch * RAD2DEG,
        2,
        " deg");

    PrintDouble(
        RIGHT_VALUE_X,
        Y_YAW,
        data.yaw * RAD2DEG,
        2,
        " deg");
}
//==================================================
// 更新GPS
//==================================================

void ConsoleUI::UpdateGPS(
    const FlightData& data)
{
    if (data.gpsValid)
    {
        PrintDouble(
            RIGHT_VALUE_X,
            Y_LAT,
            data.latitude,
            7);

        PrintDouble(
            RIGHT_VALUE_X,
            Y_LON,
            data.longitude,
            7);
    }
    else
    {
        Print(RIGHT_VALUE_X, Y_LAT, "N/A", 8);
        Print(RIGHT_VALUE_X, Y_LON, "N/A", 8);
    }

    if (data.altitudeValid)
    {
        PrintDouble(
            RIGHT_VALUE_X,
            Y_ALT,
            data.altitude,
            2,
            " m");
    }
    else
    {
        Print(RIGHT_VALUE_X, Y_ALT, "N/A", 8);
    }

    PrintInt(
        RIGHT_VALUE_X,
        Y_GPS_FIX,
        data.gpsFixType,
        data.gpsValid ? 10 : 12);

    PrintInt(
        RIGHT_VALUE_X,
        Y_SATELLITES,
        data.satellitesVisible,
        data.gpsValid ? 10 : 14);
}

//==================================================
// 更新电池
//==================================================

void ConsoleUI::UpdateBattery(
    const FlightData& data)
{
    if (data.batteryRemaining < 0)
    {
        Print(
            RIGHT_VALUE_X,
            Y_BATTERY,
            "N/A",
            8);
        return;
    }

    WORD color = 10;

    if (data.batteryRemaining < 30)
    {
        color = 12;
    }
    else if (data.batteryRemaining < 60)
    {
        color = 14;
    }

    Print(
        RIGHT_VALUE_X,
        Y_BATTERY,
        to_string(data.batteryRemaining) + " %",
        color);
}

//==================================================
// 更新悬停测试
//==================================================

void ConsoleUI::UpdateHoverTest(
    const  HoverTest& test)
{
    Print(
        VALUE_X,
        Y_HOVER_STATUS,
        test.Running() ? "RUNNING" : "STOPPED",
        test.Running() ? 10 : 8);

    PrintDouble(
        VALUE_X,
        Y_HOVER_TIME,
        test.GetDuration(),
        1,
        " s");

    PrintDouble(
        VALUE_X,
        Y_HOVER_DRIFT,
        test.GetDrift(),
        2,
        " m");

    PrintDouble(
        VALUE_X,
        Y_HOVER_MAX,
        test.GetMaxDrift(),
        2,
        " m");
}

//==================================================
// 清空数据显示区域
//==================================================

void ConsoleUI::ClearValues()
{
    const short leftRows[] =
    {
        Y_CONNECTION,
        Y_DRONE_ID,
        Y_SYSID,
        Y_COMPID,
        Y_MODE,
        Y_ARMED,
        Y_AUTOPILOT,
        Y_VEHICLE,
        Y_STATE,
        Y_HOVER_STATUS,
        Y_HOVER_TIME,
        Y_HOVER_DRIFT,
        Y_HOVER_MAX
    };

    for (const short y : leftRows)
    {
        Print(VALUE_X, y, "");
    }

    const short rightRows[] =
    {
        Y_ROLL,
        Y_PITCH,
        Y_YAW,
        Y_LAT,
        Y_LON,
        Y_ALT,
        Y_GPS_FIX,
        Y_SATELLITES,
        Y_BATTERY
    };

    for (const short y : rightRows)
    {
        Print(RIGHT_VALUE_X, y, "");
    }
}

void ConsoleUI::AddLog(
    const string& text)
{
    std::lock_guard<std::mutex> lock(consoleMutex);

    auto now =
        chrono::system_clock::to_time_t(
            chrono::system_clock::now());

    tm t;

    localtime_s(
        &t,
        &now);

    char timeBuf[32];

    strftime(
        timeBuf,
        sizeof(timeBuf),
        "%H:%M:%S",
        &t);

    ClearLine(logLine);

    Print(
        0,
        logLine,
        "[" + string(timeBuf) + "] " + text);

    logLine++;

    if (logLine > runtimeLogEnd)
    {
        logLine = LOG_START;
    }

    MoveCursor(0, 0);
}
