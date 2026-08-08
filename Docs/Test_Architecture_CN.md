# 测试架构与运行方式

## 1. 资产边界

| 类型 | 入口 | 用途 |
|---|---|---|
| Production | `ConsoleGCS/DroneGCS.vcxproj`、Unreal `DroneShowPlatform1` Module | 实际运行；不得依赖测试源码 |
| Console Regression | `ConsoleGCS/ConsoleGCSTests.vcxproj` | 一次运行覆盖 Console 核心回归 |
| UE Regression | `UnrealGCS/Source/DroneShowPlatform1/Private/Tests/` | Unreal Automation Test，统一前缀 `DroneGCS.` |
| Acceptance Tool | `ConsoleGCS/DSPGCSAcceptance.vcxproj`、`ConsoleGCS/acceptance/` | 跨进程 DSP-GCS Loopback 手工验收 |

`HoverTest.*` 是现有 Production 应用级飞行工作流，虽然名称包含 Test，但不属于测试资产，本阶段未修改。

Cleanup Phase 3E 的旧资产分类：

| 旧资产 | 分类 | 收敛结果 |
|---|---|---|
| `MavlinkDiscoveryTests` | T0/T1，跨配置、UDP、Session、GPS、ACK、DSP | 重命名并收口为 `ConsolePipelineTests.cpp` |
| `CommandDispatcherTests` | T0，MAVLink Command packet 与 LAND/SET_MODE | 合并到 `CommandTests.cpp` |
| `CommandManagerTests` | T1/T2，SAFE_TEST 与单目标键盘入口 | 去除独立工程和重复 harness，断言并入 `CommandTests.cpp` |
| `DSPGCSProductionCommandTests` | T0，DSP Command/Result 与多目标安全策略 | 保留为 `DSPCommandTests.cpp`，并入统一测试工程 |
| `DSPGCSLoopbackHost` | Acceptance Tool | 重命名为 `DSPGCSAcceptance` 并继续独立构建 |
| UE `Private/Tests/*.cpp` | T0/T1，五个独立领域 | 全部保留；没有 T3 Legacy 或 T4 Temporary 文件 |

## 2. Console Regression

在 `ConsoleGCS` 目录构建并运行：

```powershell
msbuild DroneGCS.sln /t:ConsoleGCSTests /p:Configuration=Debug /p:Platform=x64
.\x64\Debug\ConsoleGCSTests.exe
```

领域分工：

- `ConsolePipelineTests.cpp`：配置、UDP/MAVLink、Endpoint/Session、GPS、ACK、DSP Snapshot/Chunk。
- `CommandTests.cpp`：Command packet、LAND、SET_MODE、SAFE_TEST CommandManager。
- `DSPCommandTests.cpp`：DSP Command/Result、多目标命令、安全策略。

## 3. UE Regression

构建 `DroneShowPlatform1Editor Development Win64` 后，在 Automation Framework 中运行：

```text
Automation RunTests DroneGCS.
```

当前领域为 FleetGeoOrigin、DSP Snapshot Chunk、Telemetry Link、UI Selection、Fleet Niagara Bridge。

## 4. DSP-GCS Acceptance

`DSPGCSAcceptance.exe` 独立发送三架模拟遥测并验证单目标 ARM、多目标 Hover 及逐机结果。它只接受 SAFE_TEST 模式，可用 `--transport-config=<path>` 指定验收 Transport 配置；Production 不会加载或启动该工具。
