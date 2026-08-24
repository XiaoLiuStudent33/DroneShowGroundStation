# DroneGCS Development Status

Status: CURRENT
Authority: 当前实现状态与开发顺序
Last Updated: 2026-08-24
Baseline: `master` / `d2f933c6e4a180d3996452fee58827801dfd3cb1`

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
| MAVFTP 9100/4000 三段生产上传与最终验证 | DONE |
| MissionTransport abstraction | DONE |
| MissionFtpCompiler | DONE |
| MAVFTP Writer Core | DONE |
| MAVFTP Hardware Harness / Quick Test | DONE |

当前生产上传链：

```text
UE Route Workflow
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

当前阶段：

```text
Phase 2I-C.3.D.4 — MAVFTP Single-Transport Freeze
```

已冻结：

- `segment_items` 表示 Wire Mission Items，包括 Reserved Home `seq 0`。
- 复用现有 `MissionFtpCompiler` 的唯一 deterministic segment plan，不创建第二套 planner。
- MAVFTP 参数、Quick Test、retry/timeout、逐段日志与 metrics 的生产接线。
- 真实飞控 9100/4000 三段 `CLEAR + NO_CLEAR` 与 Count/Home/First/Last 验证。

## 3. NOT STARTED

- 多机并发上传与 20000 台目标架构扩展。

## 4. DEFERRED

- 10000-drone chunked/sharded Route Store。
- Backup Drone Replacement、故障补点、冲突预测、120-degree rejoin rule。

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

2026-08-24 已使用当前 checkout 完成 Release x64 Solution Rebuild（0 errors）484 warnings，ExitCode 0），随后运行：

```text
--mission-tests = PASS
--mission-upload-tests = PASS
--upload-job-tests = PASS
--mavftp-probe-tests = PASS
Full ConsoleGCSTests = PASS
```

本轮没有运行 UE Automation，没有运行真实飞控上传。

当前 Artifact 样本位于 `UnrealGCS/Saved/RouteArtifacts/Current/manifest.json`：Schema 2、9100 frames、1000 RouteSlots、1000 route records；`Saved` 样本不是应提交的 Production 源码。

## 8. 阅读规则

1. [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)
2. [Code_Architecture_CN.md](Code_Architecture_CN.md)
3. 本文件
4. [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)
5. Audit 与 `History/` 仅作证据
