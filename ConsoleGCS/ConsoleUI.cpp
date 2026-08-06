#include "ConsoleUI.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

using namespace std;

namespace
{
    std::mutex consoleMutex;

    constexpr short CONSOLE_WIDTH = 110;
    constexpr short CONSOLE_HEIGHT = 36;

    constexpr short VALUE_X = 15;
    constexpr short Y_TOTAL = 5;
    constexpr short Y_ONLINE = 6;
    constexpr short Y_OFFLINE = 7;

    constexpr short DRONE_LIST_START = 11;
    constexpr short DRONE_LIST_END = 21;

    constexpr short LOG_START = 26;
    constexpr short LOG_END = 35;

    short runtimeConsoleWidth = CONSOLE_WIDTH;
    short runtimeConsoleHeight = CONSOLE_HEIGHT;
    short runtimeWindowWidth = CONSOLE_WIDTH;
    short runtimeWindowHeight = CONSOLE_HEIGHT;
    short runtimeLogEnd = LOG_END;
}

int ConsoleUI::logLine = LOG_START;

void ConsoleUI::Init()
{
    std::lock_guard<std::mutex> lock(consoleMutex);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    const COORD largestWindow = GetLargestConsoleWindowSize(hOut);

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
        (std::min)(LOG_END, static_cast<short>(runtimeConsoleHeight - 1));

    SMALL_RECT temporaryRect = { 0, 0, 1, 1 };
    SetConsoleWindowInfo(hOut, TRUE, &temporaryRect);

    COORD size = { runtimeConsoleWidth, runtimeConsoleHeight };
    SetConsoleScreenBufferSize(hOut, size);

    SMALL_RECT rect;
    rect.Left = 0;
    rect.Top = 0;
    rect.Right = runtimeWindowWidth - 1;
    rect.Bottom = runtimeWindowHeight - 1;
    SetConsoleWindowInfo(hOut, TRUE, &rect);

    DWORD written;
    COORD home = { 0, 0 };

    FillConsoleOutputCharacter(
        hOut,
        ' ',
        runtimeConsoleWidth * runtimeConsoleHeight,
        home,
        &written);

    FillConsoleOutputAttribute(
        hOut,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
        runtimeConsoleWidth * runtimeConsoleHeight,
        home,
        &written);

    SetConsoleCursorPosition(hOut, home);

    CONSOLE_CURSOR_INFO cursor;
    cursor.dwSize = 20;
    cursor.bVisible = FALSE;
    SetConsoleCursorInfo(hOut, &cursor);

    DrawFrame();
    logLine = LOG_START;
    MoveCursor(0, 0);
}

void ConsoleUI::SetColor(WORD color)
{
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}

void ConsoleUI::MoveCursor(short x, short y)
{
    COORD pos = { x, y };
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), pos);
}

void ConsoleUI::Print(
    short x,
    short y,
    const string& text,
    WORD color)
{
    MoveCursor(x, y);
    SetColor(color);
    cout << left << setw(24) << text;
    SetColor(7);
}

void ConsoleUI::ClearLine(short y)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD pos = { 0, y };
    DWORD written;

    FillConsoleOutputCharacter(
        hOut,
        ' ',
        runtimeConsoleWidth,
        pos,
        &written);

    FillConsoleOutputAttribute(
        hOut,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
        runtimeConsoleWidth,
        pos,
        &written);
}

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
    cout << "Drone GCS - Backend Server";
    MoveCursor(0, 1);
    cout << majorLine;
    SetColor(7);

    MoveCursor(0, 3);
    cout << "Fleet Status:";
    MoveCursor(0, Y_TOTAL);
    cout << "Total Drone :";
    MoveCursor(0, Y_ONLINE);
    cout << "Online      :";
    MoveCursor(0, Y_OFFLINE);
    cout << "Offline     :";

    MoveCursor(0, 9);
    cout << "Drone List:";
    MoveCursor(0, 10);
    cout << minorLine;

    for (short y = DRONE_LIST_START; y <= DRONE_LIST_END; ++y)
    {
        ClearLine(y);
    }

    MoveCursor(0, 23);
    cout << majorLine;
    MoveCursor(0, 24);
    cout << "Runtime Log:";
    MoveCursor(0, 25);
    cout << minorLine;

    for (short y = LOG_START; y <= runtimeLogEnd; ++y)
    {
        ClearLine(y);
    }

    MoveCursor(0, 0);
}

void ConsoleUI::UpdateFleet(
    const std::vector<ConsoleDroneStatus>& drones)
{
    std::lock_guard<std::mutex> lock(consoleMutex);

    const std::size_t onlineCount =
        static_cast<std::size_t>(std::count_if(
            drones.begin(),
            drones.end(),
            [](const ConsoleDroneStatus& drone)
            {
                return drone.online;
            }));

    Print(VALUE_X, Y_TOTAL, std::to_string(drones.size()), 11);
    Print(VALUE_X, Y_ONLINE, std::to_string(onlineCount), 10);
    Print(
        VALUE_X,
        Y_OFFLINE,
        std::to_string(drones.size() - onlineCount),
        (drones.size() == onlineCount) ? 8 : 12);

    for (short y = DRONE_LIST_START; y <= DRONE_LIST_END; ++y)
    {
        ClearLine(y);
    }

    const std::size_t rowCapacity =
        static_cast<std::size_t>(DRONE_LIST_END - DRONE_LIST_START + 1);

    const bool hasOverflow = drones.size() > rowCapacity;
    const std::size_t visibleCount =
        hasOverflow ? rowCapacity - 1 : drones.size();

    for (std::size_t index = 0; index < visibleCount; ++index)
    {
        const ConsoleDroneStatus& drone = drones[index];
        std::ostringstream row;
        row << std::left << std::setw(16) << drone.droneId
            << (drone.online ? "ONLINE" : "OFFLINE");

        Print(
            0,
            static_cast<short>(DRONE_LIST_START + index),
            row.str(),
            drone.online ? 10 : 12);
    }

    if (hasOverflow)
    {
        const std::size_t hiddenCount = drones.size() - visibleCount;
        Print(
            0,
            DRONE_LIST_END,
            "... " + std::to_string(hiddenCount) + " more drones",
            8);
    }

    MoveCursor(0, 0);
}

void ConsoleUI::Update(
    const FlightData& data,
    const HoverTest& test)
{
    Update("Drone", data, test);
}

void ConsoleUI::Update(
    const std::string& droneId,
    const FlightData& data,
    const HoverTest& /*test*/)
{
    UpdateFleet({ ConsoleDroneStatus{ droneId.empty() ? "Drone" : droneId, data.online } });
}

void ConsoleUI::AddLog(const string& text)
{
    std::lock_guard<std::mutex> lock(consoleMutex);

    const auto now =
        chrono::system_clock::to_time_t(chrono::system_clock::now());

    tm t;
    localtime_s(&t, &now);

    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &t);

    ClearLine(static_cast<short>(logLine));
    Print(0, static_cast<short>(logLine), "[" + string(timeBuf) + "] " + text);

    ++logLine;
    if (logLine > runtimeLogEnd)
    {
        logLine = LOG_START;
    }

    MoveCursor(0, 0);
}
