# DroneGCS 测试架构与证据边界

Status: CURRENT SUPPORTING
Authority: 测试资产、运行入口与证据等级
Last Updated: 2026-08-25

Production 边界见 [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)，当前调用链见 [Code_Architecture_CN.md](Code_Architecture_CN.md)，进度见 [Development_Status_CN.md](Development_Status_CN.md)。

## 1. 资产边界

| 类型 | 入口 | 证据范围 |
|---|---|---|
| Production | `ConsoleGCS/DroneGCS.vcxproj`、UE `DroneShowPlatform1` Module | 实际运行；不得依赖测试源码 |
| Console Regression | `ConsoleGCS/ConsoleGCSTests.vcxproj` | Console 核心链、Artifact、Mission、DSP Job、MAVFTP |
| UE Regression | `UnrealGCS/Source/DroneShowPlatform1/Private/Tests/` | Unreal Automation，统一前缀 `DroneGCS.` |
| Acceptance Tool | `ConsoleGCS/DSPGCSAcceptance.vcxproj` | 独立 DSP-GCS loopback 手工验收 |
| Hardware Harness | `DroneGCS.exe --mavftp-test` 或显式 legacy harness 参数 | 真实飞控写入；不属于普通回归 |

测试中的 loopback Session、MAVFTP fake wire 和 synthetic Artifact 不能证明真实飞控行为。

## 2. Console Regression

构建后从 `ConsoleGCS` 目录运行完整回归：

```powershell
.\x64\Release\ConsoleGCSTests.exe
```

专项入口：

```powershell
.\x64\Release\ConsoleGCSTests.exe --mission-tests
.\x64\Release\ConsoleGCSTests.exe --mission-upload-tests
.\x64\Release\ConsoleGCSTests.exe --upload-job-tests
.\x64\Release\ConsoleGCSTests.exe --mavftp-probe-tests
.\x64\Release\ConsoleGCSTests.exe --drone-id-tests
.\x64\Release\ConsoleGCSTests.exe --console-render-tests
```

- `--mission-tests`：Route Mission codec 与 Mission FTP compiler。
- `--mission-upload-tests`：Mission transaction、transport abstraction、preparation 与 RouteUploadManager integration。
- `--upload-job-tests`：DSP Upload Job schema/binding/materialization/scale。
- `--mavftp-probe-tests`：MAVFTP protocol/client、MissionProfileProbe、Mission writer 与 Hardware Harness safety。
- `--drone-id-tests`：Console/UE 共享 canonical DroneID format/parse/overflow/RouteSlot 合同。
- `--console-render-tests`：单 Writer、Dashboard/Linear Handle 判定、长行与短行覆盖、Resize/compact layout、1024 条有界队列、critical retention、active-producer Shutdown 和生产风格终帧。
- Full suite 还覆盖 Console pipeline、command、Frozen Assignment、Artifact Consumer v2 与 contract。

## 3. UE Regression

UE 自动化测试位于 `UnrealGCS/Source/DroneShowPlatform1/Private/Tests/`，覆盖 Fleet、Route Overlay、Assignment、Artifact、Waypoint、Frozen Assignment、Upload Job 与 Workflow 等领域。

E.0 专项前缀 `DroneGCS.UI.RouteWorkflow.AutoStart` 覆盖 canonical ID、单/多 Target exactly-once、拒绝矩阵、Console failure 与 Selection/Fleet/endpoint/Artifact/Manifest/Route record stale 矩阵。既有 `ViewStatePersistence`、`LiveAggregate`、Route Assignment 与 Frozen Sync 继续作为生命周期、cross-job 和 DSP identity 回归。

运行要求：构建 `DroneShowPlatform1Editor Development Win64` 后执行：

```text
Automation RunTests DroneGCS.
```

## 4. Hardware Harness 隔离

真实 Mission 写入只能在用户明确授权后运行。普通回归、文档审计和只读诊断不得启动：

```text
--mavftp-test
--mavftp-hardware-test
--allow-mission-write-probe
任何真实 Mission upload
```

Quick Test 从 `UnrealGCS/Saved/RouteArtifacts/Current/manifest.json` 引导，再由 `ConsoleArtifactConsumerV2` 执行完整 Artifact 校验。生产与显式 Hardware Harness 共享已冻结的 MAVFTP 五项参数合同；Harness 仍只能由显式安全入口启动。

## 5. 当前自动化证据

2026-08-25 在当前 checkout 完成 Release x64 Solution Build（0 errors、12 warnings、ExitCode 0），随后运行：

```text
--drone-id-tests = PASS
--artifact-v2-consumer-tests = PASS
--mission-tests = PASS
--mission-upload-tests = PASS
--upload-job-tests = PASS
--mavftp-probe-tests = PASS
Full ConsoleGCSTests = PASS
--console-render-tests = PASS (连续 20/20)
```

E.0.1 Console Render Integrity 另完成 Full ConsoleGCSTests 连续 3/3 PASS。Fake Console 只替换物理 target，生产 Renderer/queue/layout 未复制；Pipe/File 断言不含 cursor/clear escape，Dashboard 所有输出行严格受当前 viewport 边界约束。

UE Development/DebugGame Editor Build 均 PASS；AutoStart 6/6 PASS，Route Assignment + Frozen shared contract + DSP Upload Job + Route Workflow 合并回归 164/164 PASS。证据限定：本轮未运行真实飞控上传。

## 6. 用户确认的硬件证据

```text
1000–6000 items: USER-CONFIRMED PASS
7000 items: USER-CONFIRMED NAK 2 / Write Failed
```

仓库没有对应持久化日志，飞控源码也未证明精确根因。因此不能写成固定“7000 item limit”，也不能将该观察升级为自动化回归结论。
