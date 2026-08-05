#include "HoverTest.h"

#include <cmath>

constexpr double EARTH_RADIUS = 6378137.0;

//==================================================
// GPS距离计算（米）
//==================================================

static double GPSDistance(
    double lat1,
    double lon1,
    double lat2,
    double lon2)
{
    constexpr double RAD =
        3.14159265358979323846 / 180.0;

    double dLat = (lat2 - lat1) * RAD;
    double dLon = (lon2 - lon1) * RAD;

    lat1 *= RAD;
    lat2 *= RAD;

    double a =
        sin(dLat / 2.0) * sin(dLat / 2.0) +
        cos(lat1) * cos(lat2) *
        sin(dLon / 2.0) * sin(dLon / 2.0);

    double c =
        2.0 * atan2(
            sqrt(a),
            sqrt(1.0 - a));

    return EARTH_RADIUS * c;
}

//==================================================
// 开始测试
//==================================================

void HoverTest::Start(
    const FlightData& data)
{
    running = true;

    startTime =
        std::chrono::steady_clock::now();

    //--------------------------
    // 起始GPS
    //--------------------------

    startLat = data.latitude;
    startLon = data.longitude;
    startAlt = data.altitude;

    //--------------------------
    // 高度
    //--------------------------

    maxAlt = data.altitude;
    minAlt = data.altitude;

    //--------------------------
    // 漂移
    //--------------------------

    currentDrift = 0.0;
    maxDrift = 0.0;

    //--------------------------
    // 姿态
    //--------------------------

    maxRoll = 0.0;
    maxPitch = 0.0;

    //--------------------------
    // 电池
    //--------------------------

    startBattery =
        data.batteryRemaining;

    endBattery =
        startBattery;
}

//==================================================
// 更新测试
//==================================================

void HoverTest::Update(
    const FlightData& data)
{
    if (!running)
    {
        return;
    }

    //--------------------------
    // 高度
    //--------------------------

    if (data.altitude > maxAlt)
    {
        maxAlt = data.altitude;
    }

    if (data.altitude < minAlt)
    {
        minAlt = data.altitude;
    }

    //--------------------------
    // 漂移
    //--------------------------

    currentDrift =
        GPSDistance(
            startLat,
            startLon,
            data.latitude,
            data.longitude);

    if (currentDrift > maxDrift)
    {
        maxDrift = currentDrift;
    }

    //--------------------------
    // 姿态
    //--------------------------

    if (fabs(data.roll) > maxRoll)
    {
        maxRoll = fabs(data.roll);
    }

    if (fabs(data.pitch) > maxPitch)
    {
        maxPitch = fabs(data.pitch);
    }

    //--------------------------
    // 电池
    //--------------------------

    endBattery =
        data.batteryRemaining;
}

//==================================================
// 停止测试
//==================================================

void HoverTest::Stop(
    const FlightData& data)
{
    Update(data);

    running = false;
}

//==================================================
// 是否运行
//==================================================

bool HoverTest::Running() const
{
    return running;
}

//==================================================
// 测试时间（秒）
//==================================================

double HoverTest::GetDuration() const
{
    if (!running)
    {
        return 0.0;
    }

    auto now =
        std::chrono::steady_clock::now();

    return
        std::chrono::duration_cast<
        std::chrono::milliseconds>(
            now - startTime).count() / 1000.0;
}

//==================================================
// 当前漂移
//==================================================

double HoverTest::GetDrift() const
{
    return currentDrift;
}

//==================================================
// 最大漂移
//==================================================

double HoverTest::GetMaxDrift() const
{
    return maxDrift;
}