# DroneGCS 测试架构与证据边界

Status: CURRENT SUPPORTING
Authority: 测试资产、运行入口与证据等级
Last Updated: 2026-09-03

Production 边界见 [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)，当前调用链见 [Code_Architecture_CN.md](Code_Architecture_CN.md)，进度见 [Development_Status_CN.md](Development_Status_CN.md)。

## 1. 资产边界

| 类型 | 入口 | 证据范围 |
|---|---|---|
| Production | `ConsoleGCS/DroneGCS.vcxproj`、UE `DroneShowPlatform1` Module | 实际运行；不得依赖测试源码 |
| Console Regression | `ConsoleGCS/ConsoleGCSTests.vcxproj` | Console 核心链、Artifact、Mission、DSP Job、MAVFTP |
| UE Regression | `UnrealGCS/Source/DroneShowPlatform1/Private/Tests/` | Unreal Automation，统一前缀 `DroneGCS.` |
| Acceptance Tool | `ConsoleGCS/DSPGCSAcceptance.vcxproj` | 独立 DSP-GCS loopback 手工验收 |
| Hardware Harness | `DroneGCS.exe --mavftp-test` 或显式 legacy harness 参数 | 真实飞控写入；不属于普通回归 |
| Joint Package Smoke | `Tools/Launcher/tests/Smoke.ps1` | 联合进程、配置、目录、端口与隔离回环；不发真机命令 |
| Installer Validation | `Tools/Installer/BuildInstaller.ps1` | Shipping 布局、payload 哈希、源码排除与 NSIS 产物 |

测试中的 loopback Session、MAVFTP fake wire 和 synthetic Artifact 不能证明真实飞控行为。

### E.0.2 Console/Fleet 证据

- `--console-render-tests` 同时断言字符帧与 attribute 帧，包括数量色、ONLINE/OFFLINE token、状态反转、尾部默认色、Resize/Compact 边界、Linear 无控制序列及单 Renderer writer。
- `DroneGCS.UI.Selection.DetailAndCommandStateSeparation` 使用真实 `WBP_GCS_Main_V2` / `WBP_GCS_FleetPanel` 控件，覆盖全选/清空、跨页、Online/Offline、新上线、stale ID、零 Online、重复绑定、Detail 隔离和 active Upload gate。
- `DroneGCS.UI.FleetSelectAll.Scale1000` 与 `Scale20000` 使用单次 Fleet Snapshot 和 Authority toggle，断言完整数量、一次 SelectionChanged、零 per-drone Widget 创建。
- 2026-08-25 Gate：Console Render 连续 20/20 PASS，Console 全回归 PASS；`DroneGCS.UI` 22/22 PASS，`DroneGCS.Data.DSPGCSUploadJob` 3/3 PASS。规模热路径观测为 1000 台约 1.225 ms、20000 台约 11.454 ms（同机单次观测，不作为跨机器 SLA）。

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
.\x64\Release\ConsoleGCSTests.exe --mavftp-concurrency-tests
.\x64\Release\ConsoleGCSTests.exe --drone-id-tests
.\x64\Release\ConsoleGCSTests.exe --console-render-tests
```

- `--mission-tests`：Route Mission codec 与 Mission FTP compiler。
- `--mission-upload-tests`：Mission transaction、transport abstraction、preparation 与 RouteUploadManager integration。
- `--upload-job-tests`：DSP Upload Job schema/binding/materialization/scale。
- `--mavftp-probe-tests`：MAVFTP protocol/client、MissionProfileProbe、Mission writer 与 Hardware Harness safety。
- `--mavftp-concurrency-tests`：E.1 动态 N Target scheduler、全局 Write permit、公平性、精确 release/retry 与 endpoint/ACK 隔离。
- `--drone-id-tests`：Console/UE 共享 canonical DroneID format/parse/overflow/RouteSlot 合同。
- `--console-render-tests`：单 Writer、Dashboard/Linear Handle 判定、长行与短行覆盖、Resize/compact layout、1024 条有界队列、critical retention、active-producer Shutdown 和生产风格终帧。
- Full suite 还覆盖 Console pipeline、command、Frozen Assignment、Artifact Consumer v2 与 contract。

## 3. UE Regression

UE 自动化测试位于 `UnrealGCS/Source/DroneShowPlatform1/Private/Tests/`，覆盖 Fleet、Route Overlay、Assignment、Artifact、Waypoint、Frozen Assignment、Upload Job 与 Workflow 等领域。

E.0 专项前缀 `DroneGCS.UI.RouteWorkflow.AutoStart` 覆盖 canonical ID、单/多 Target exactly-once、拒绝矩阵、Console failure 与 Selection/Fleet/endpoint/Artifact/Manifest/Route record stale 矩阵。既有 `ViewStatePersistence`、`LiveAggregate`、Route Assignment 与 Frozen Sync 继续作为生命周期、cross-job 和 DSP identity 回归。

E.1 Console 调度矩阵覆盖 TargetCount `1/2/6/16/25/50`，另用 100/1000 Target 只做 bookkeeping scale，不创建真实 socket。Permit cap=1 的 50-owner round-robin 覆盖无饥饿；ACK/NAK/retry exhausted/Cancel/Shutdown 覆盖精确释放。两个生产 `MavFtpClient` 经真实 endpoint ingress 路径使用相同 SYSID、sequence/session/offset/opcode，验证不同 Remote IP/port 不串包。并发专项 Release 与 Debug/assert-enabled 均连续 20/20 PASS，完整 Console 回归连续 3/3 PASS。

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

## 7. 联合软件包与安装程序证据

2026-09-03 自动化/本机证据：

- Release x64 Console 与联合启动器构建 PASS。
- `Tools/Launcher/tests/Smoke.ps1` 共 32 项 PASS，覆盖中文/空格路径、安装标记、首次配置复制、每用户日志与工作区、Shipping 布局、可见完整控制后端、Ctrl+C 正常退出及无残留进程。
- 真实 V3 Shipping 暂存目录通过 manifest、二进制 SHA-256、源码排除和单 Game EXE 严格校验。
- `DroneGCSLauncher.exe --check` PASS；该模式不启动子进程、不发包。
- NSIS 3.12 无警告生成 V1.0.0 安装程序和 SHA-256 校验文件。

用户确认的异机证据：

```text
Second-PC install and launch: USER-CONFIRMED PASS
Repository-side second-PC logs: NONE
```

因此可以确认第一版安装交付链可用；不能仅凭该结论声称升级、卸载、全部灯光/飞行/航线上传或任意固件行为均已验收。
