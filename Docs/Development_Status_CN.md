# DroneGCS Development Status

Status: CURRENT
Authority: 当前实现状态与开发顺序
Last Updated: 2026-08-24
Baseline: `master` / `a494bd14dae00a5b4c24055a426ca1cee98e37f0` / `Phase4.2`

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
| MissionProtocol 真实硬件垂直链 | DONE |
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
-> MissionProtocolTransport
-> DroneSession endpoint-bound sender
-> ArduPilot/APM
```

`MAVFTPMissionTransport` 已实现，但不是生产默认 Transport。

## 2. CURRENT / NEXT

下一阶段：

```text
Phase 2I-C.3.D — 配置化多段 MAVFTP 上传
```

开发前必须冻结：

- `segment_items` 表示 Wire Mission Items，包括 Reserved Home `seq 0`。
- 复用现有 `MissionFtpCompiler` 的唯一 deterministic segment plan，不创建第二套 planner。
- 配置、Quick Test、retry/timeout、逐段日志与 metrics 的明确接线范围。
- 真实飞控多段 `CLEAR + NO_CLEAR` 验证必须单独显式授权。

## 3. NOT STARTED

- `Phase 2I-C.3.D` 配置化多段 MAVFTP 上传。
- Quick Test 从配置读取较小 segment capacity。
- `[mavftp] segment_items / segment_retry_count / segment_timeout_ms` 配置接线。
- MAVFTP 接管生产 `RouteUploadManager`；当前为 intentionally deferred。
- 真实飞控多段 `CLEAR + NO_CLEAR` 拼接验证。
- 可证明的 Production Mission Ready verification。

## 4. DEFERRED

- 10000-drone chunked/sharded Route Store。
- Backup Drone Replacement、故障补点、冲突预测、120-degree rejoin rule。
- MAVFTP production cutover，直到配置化多段与硬件验证完成且另行批准。

## 5. FIRMWARE UNKNOWN

- 真实 Mission execution timing。
- custom firmware RGB waypoint execution。
- `@MISSION/mission.dat` apply semantics。
- time-control message behavior。
- custom `MAV_CMD_USER` behavior。
- remote integrity 与 Mission Ready verification。

`All Write ACKed != Mission Ready`。

## 6. USER-CONFIRMED HARDWARE EVIDENCE

以下是用户提供的真实硬件观察，仓库中没有对应持久化日志：

```text
1000–6000 items: USER-CONFIRMED PASS
7000 items: USER-CONFIRMED NAK 2 / Write Failed
```

不得将该观察升级为固定“7000 item limit”；精确根因尚无保存日志或飞控源码证据。

## 7. 当前验证快照

2026-08-24 已使用当前 checkout 中已有的 Release 测试程序运行：

```text
--mission-tests = PASS
--mission-upload-tests = PASS
--upload-job-tests = PASS
--mavftp-probe-tests = PASS
Full ConsoleGCSTests = PASS
```

本轮没有重新 Build、没有运行 UE Automation、没有运行真实飞控上传。

当前 Artifact 样本位于 `UnrealGCS/Saved/RouteArtifacts/Current/manifest.json`：Schema 2、9100 frames、1000 RouteSlots、1000 route records；`Saved` 样本不是应提交的 Production 源码。

## 8. 阅读规则

1. [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)
2. [Code_Architecture_CN.md](Code_Architecture_CN.md)
3. 本文件
4. [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)
5. Audit 与 `History/` 仅作证据
