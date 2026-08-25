# DroneGCS Development Status

Status: CURRENT
Authority: 当前实现状态与开发顺序
Last Updated: 2026-08-25
Baseline: `master` / `17b679265c77d1c2306e1b500574b121b29e525c`

## 1. DONE / IMPLEMENTED

基础 Production 能力：

- UE 5.5.4 frontend / visualization / operator workflow 与 ConsoleGCS backend 边界。
- ArduPilot/APM MAVLink，多 Remote Endpoint discovery、Session、telemetry、command 与 ACK。
- `Remote IP + UDP Source Port` Fleet identity；同 endpoint 恢复原 Session/DroneID。
- DSP-GCS localhost UDP、1000-drone snapshot/chunk/reassembly。
- Fleet UI、Cesium、one Actor / one Niagara System、TopDown、Marquee、LED preview/command。
- Native NPZ Route Preview、Playback、Scrub、Route Import。

Route Upload 阶段：

| 任务域 | 状态 |
|---|---|
| Phase 2A Live + Route Overlay | IMPLEMENTED |
| Phase 2B RouteAssignment / Freeze / Revision | IMPLEMENTED |
| Phase 2C Artifact Workspace / Manifest / READY | IMPLEMENTED |
| Phase 2D Waypoint Generator | IMPLEMENTED |
| Phase 2E Waypoint Validation | IMPLEMENTED |
| Phase 2F Console Parser / Mission Encoder | IMPLEMENTED |
| Phase 2G DSP Upload Job Protocol | IMPLEMENTED |
| RouteUploadManager / 并发、取消、重试、状态 | IMPLEMENTED |
| UI-3C 上传操作按钮与状态展示 | IMPLEMENTED |
| Phase 2I-C.3.E.0 Selected Drone 自动分配与 Start 事务 | IMPLEMENTED |
| Phase 2I-C.3.E.0.1 Console Render Integrity | IMPLEMENTED |
| Phase 2I-C.3.E.0.2 Console Status Colors / Fleet Select-All | IMPLEMENTED |
| Phase 2I-C.3.E.1 Dynamic-N MAVFTP Concurrency / Isolation | SOFTWARE GATE PASS；E.2 HARDWARE PENDING |
| MAVFTP 9100/4000 三段生产上传与最终验证 | DONE |
| MissionTransport abstraction | DONE |

E.0.2 冻结结果：Interactive Dashboard 在单 Renderer frame 中为 Total/Online/Offline 数字及 ONLINE/OFFLINE token 写 Win32 attributes；Linear 输出保持纯文本。Fleet Select-All 复用 `SelectedCommandDroneIDs`，作用于全 Fleet Online DroneID 精确集合，支持跨页、上下线重算、一次批量广播和 active AutoStart/Upload mutation gate；未改动 Route Assignment、MAVFTP、DSP DTO 或 UE Asset。
| MissionFtpCompiler | DONE |
| MAVFTP Writer Core | DONE |
| MAVFTP Hardware Harness / Quick Test | DONE |

当前生产上传链：

```text
UE Route Workflow
-> SelectedCommandDroneIDs
-> canonical DroneID -> RouteSlot (`NumericDroneID - 1`，临时 E.0 规则)
-> explicit Working Assignment -> Freeze(AllowDegraded) -> Console Accepted
-> DSP-GCS Upload Job
-> ConsoleUploadJobAuthority
-> RouteUploadManager
-> MissionPreparationWorker
-> MAVFTPMissionTransport
-> DroneSession endpoint-bound MavFtpClient
-> shared MAVLink sender/socket
-> ArduPilot/APM
```

生产 Route Upload 已冻结为 MAVFTP-only；旧 Transport 的源码、factory 分支、CLI、INI 和工程项已删除，失败不会触发协议回退。

## 2. CURRENT / NEXT

当前完成点：

```text
Phase 2I-C.3.E.0 — Automatic Selected-Drone Assignment and Start Transaction
```

已冻结：

- `segment_items` 表示 Wire Mission Items，包括 Reserved Home `seq 0`。
- 复用现有 `MissionFtpCompiler` 的唯一 deterministic segment plan，不创建第二套 planner。
- MAVFTP 参数、Quick Test、retry/timeout、逐段日志与 metrics 的生产接线。
- 真实飞控 9100/4000 三段 `CLEAR + NO_CLEAR` 与 Count/Home/First/Last 验证。
- 正常 UI Start 已不依赖手工 `RouteAssignment.BindExplicit`；该 Cmd 只保留诊断用途。
- E.1 已取消独立 Deployment Target cap：UE 已 Apply 的 `1..50` 值在 Start 时冻结并进入现有 DTO；Console 严格拒绝非法值，实际准入取 DTO 与 matched targets 的最小值。
- E.0.1 已冻结 Console 单一 Renderer Authority；真实 Console 为 Dashboard，Pipe/File/Tee/CI 为 Linear，业务线程不再直接参与物理绘制。
- 自动事务在 Console Accepted 后重新校验 Selection/Fleet/endpoint/Artifact/Manifest/Route record，并且每个事务最多发送一次 Start。

## 3. NOT STARTED

- E.2 多机硬件规模验证与调度冻结。

## 4. DEFERRED

- 10000-drone chunked/sharded Route Store。
- Backup Drone Replacement、故障补点、冲突预测、120-degree rejoin rule。
- E.3 Console 重启后的 DroneID 发现顺序/稳定身份风险。

## 5. FIRMWARE UNKNOWN

- 真实 Mission execution timing。
- custom firmware RGB waypoint execution。
- time-control message behavior。
- custom `MAV_CMD_USER` behavior。
- Mission 执行态的远端完整性观测。

`All Write ACKed != Mission Ready`。

## 6. USER-CONFIRMED HARDWARE EVIDENCE

用户确认的真实生产证据：

```text
ArtifactItems=9100 / WireMissionItems=9101
SegmentCapacity=4000 / SegmentCount=3
Count/Home/First/Last=PASS
Target=Succeeded / Job ElapsedMs=8829
```

该证据证明当前 MAVFTP 生产链的 9100/4000 用例，不代表任意飞控或任意 Mission 大小的普遍上限。

## 7. 当前验证快照

2026-08-25 已使用当前 checkout 完成 Release x64 Solution Build（0 errors、12 warnings、ExitCode 0），随后运行：

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

E.1 Console Release Build PASS；动态 N 覆盖 `1/2/6/16/25/50`，公平 permit 专项连续 20/20 PASS，Debug/assert-enabled 专项连续 20/20 PASS，完整 Console 回归连续 3/3 PASS。Endpoint ingress 隔离 Fixture 使用相同 SYSID 和相同 ACK identity、不同 Remote endpoint，验证 ACK 不串包。该软件证据不替代 E.2 真实多机硬件验收。

UE Development/DebugGame Editor Build 均为 PASS；`DroneGCS.Data.DSPGCSUploadJob` 3/3、`DroneGCS.UI.RouteWorkflow` 17/17 PASS，覆盖 Applied 并发、AutoStart 快照、View persistence 与 stale gate。本轮没有运行真实飞控上传。

当前 Artifact 样本位于 `UnrealGCS/Saved/RouteArtifacts/Current/manifest.json`：Schema 2、9100 frames、1000 RouteSlots、1000 route records；`Saved` 样本不是应提交的 Production 源码。

## 8. 阅读规则

1. [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)
2. [Code_Architecture_CN.md](Code_Architecture_CN.md)
3. 本文件
4. [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)
5. Audit 与 `History/` 仅作证据
