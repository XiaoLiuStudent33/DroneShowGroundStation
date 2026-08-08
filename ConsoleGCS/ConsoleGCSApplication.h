/**
 * @brief ConsoleGCS 的进程级启动与运行模式编排入口。
 *
 * 分类：Production / Compatibility。
 * 输入：命令行配置路径与 AppConfig；输出：多机 MAVLink 生产循环或旧单会话兼容循环。
 * 调用链：main -> ConsoleGCSApplication -> 网络、Fleet、Command、DSP-GCS、ConsoleUI。
 * 关键约束：这里只组合现有模块，不改变 Endpoint 身份、Session 生命周期或协议语义。
 */
#pragma once

struct AppConfig;

class ConsoleGCSApplication
{
public:
    int Run(int argc, char* argv[]);

private:
    int RunReadOnlyMavlink(const AppConfig& appConfig);
    int RunLegacySingleSession(const AppConfig& appConfig);
};
