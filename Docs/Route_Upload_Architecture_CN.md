# DroneGCS Route Upload Architecture

Status: CURRENT
Authority: 当前 Route domain、Upload Job、Mission Transport 与下一阶段边界
Last Updated: 2026-08-24
Baseline: `a494bd14dae00a5b4c24055a426ca1cee98e37f0`

历史证据见 [Legacy BGC2 Route Upload Audit](History/Audit_Legacy_BGC2_RouteUpload_CN.md) 与 [2026-08-14 DroneGCS Gap Audit](History/Audit_DroneGCS_RouteUpload_Gap_CN.md)。历史 Audit 不代表当前实现状态。

## 1. Status Legend

- **IMPLEMENTED**：当前 Production 源码或显式测试/硬件工具已存在。
- **CURRENT PRODUCTION**：当前默认生产调用链。
- **EXPERIMENTAL / HARDWARE HARNESS**：仅由显式入口启用，不是生产默认。
- **NEXT**：下一阶段开发内容。
- **DEFERRED**：当前阶段不接入。
- **FIRMWARE UNKNOWN / NOT PROVEN**：需要真实固件证据或硬件验证。

## 2. End-to-End Status

```text
NPZ Import                         IMPLEMENTED
Route Preview                      IMPLEMENTED
Live + Route Overlay               IMPLEMENTED
Route Assignment                   IMPLEMENTED
Assignment Validation / Freeze     IMPLEMENTED
Waypoint Generation                IMPLEMENTED
Waypoint Validation                IMPLEMENTED
Artifact Freeze / READY            IMPLEMENTED
Frozen Assignment DSP              IMPLEMENTED
DSP-GCS Upload Job                 IMPLEMENTED
Console Artifact Consumer          IMPLEMENTED
Console RouteUploadManager         IMPLEMENTED
QGC Parser / Mission Encoder       IMPLEMENTED
MissionProtocol Production Upload  IMPLEMENTED
MAVFTP Compiler / Writer / Harness IMPLEMENTED
MAVFTP Production Cutover          NOT IMPLEMENTED
Mission Ready                      FIRMWARE UNKNOWN / NOT PROVEN
```

UE 不直接发送 MAVLink。ConsoleGCS 始终是 Flight Controller network 与 upload execution Authority。

## 3. Route Overlay — IMPLEMENTED

`RoutePreviewSubsystem` 在 Live mode 中生成 Route target positions，并由同一个 `ADroneFleetVisualizationActor` / Niagara System 显示。Overlay 与 Live drone arrays、particle identity 和 Assignment Authority 隔离。

Overlay 只回答“真实位置与 Route target 如何同屏显示”，不决定 DroneID 与 RouteSlot 的绑定。

## 4. RouteAssignment — IMPLEMENTED

Authority：`URouteAssignmentSubsystem`。

核心关系：

```text
DroneID <-> RouteSlotIndex
```

已实现：

- Local ENU / Show Origin 一致空间中的位置匹配。
- one-to-one DroneID/RouteSlot validation。
- explicit binding 与 position matching。
- full/degraded freeze policy。
- working/frozen snapshot、monotonic Revision、source identity 与 digest。
- conflict、unassigned、vacant 与质量信息。

禁止使用 SYSID、particle index 或 `DroneID == RouteSlotIndex` 假设。

## 5. Waypoint Generation / Validation — IMPLEMENTED

```text
Route Local ENU + Show Origin + Frozen Assignment identity
-> RouteWaypointGenerationSubsystem
-> QGC WPL 110 per RouteSlot
-> RouteWaypointValidator
```

生成使用 Local ENU + Show Origin，不从 UE World 反推 GPS。`MAV_FRAME_GLOBAL_RELATIVE_ALT` altitude 使用 Local Z，不叠加 ShowOriginAltitude。

当前 legacy/custom profile 将 RGB 写入 `MAV_CMD_NAV_WAYPOINT` param1/param2/param3。这不是标准 ArduPilot NAV_WAYPOINT 语义，仍依赖定制固件。

Preview SampleRate 也不能证明 Mission execution timing。

## 6. Artifact Workspace — IMPLEMENTED

当前 canonical schema v2 Artifact 包含：

- source dataset identity、FrameCount、RouteSlotCount、range。
- Show Origin 与 generator identity。
- per-route relative path、file size、digest。
- ArtifactSetId 与 ArtifactSetDigest。
- `manifest.json` 与 `READY`。
- immutable `Current` publication 与 read lease / TOCTOU identity。

Console `ConsoleArtifactConsumerV2` 在 trust boundary 内重新验证 workspace allowlist、canonical path、READY/Manifest identity、route allowlist、size/digest 与 waypoint semantics。

`UnrealGCS/Saved/RouteArtifacts/Current` 是运行期样本，不是 Production 源码提交要求。

## 7. Frozen Assignment DSP — IMPLEMENTED

```text
UE Frozen Assignment
-> DSPGCSFrozenAssignmentProtocol schema 2
-> Console Receiver / TransactionAssembler
-> FrozenAssignmentStore
-> ACK / rejection
```

Upload Job 必须双绑定 Artifact 与 accepted Frozen Assignment，不得仅凭 RouteSlot 数字或任意路径启动。

## 8. DSP-GCS Upload Job — IMPLEMENTED

`DSPGCSUploadJobProtocol` schema 2 已实现：

- Start、Pause、Resume、Cancel、RetryFailed、SetMaxConcurrentUploads。
- Command ACK。
- aggregate snapshot、route state page、route state delta。
- Artifact/Frozen Assignment dual binding、job revision 与 idempotency。

UE `UDSPGCSUploadJobSubsystem` 和 `FDSPGCSUploadJobStateMirror` 负责 operator-side command/state；Console `DSPGCSUploadJobReceiver` 与 `ConsoleUploadJobAuthority` 负责 accepted job state。

## 9. Console Route Upload — CURRENT PRODUCTION

```text
ConsoleUploadJobAuthority
-> RouteUploadManager
-> MissionPreparationWorker
-> IMissionTransport
-> MissionProtocolTransport
-> DroneSession endpoint-bound sender
-> ArduPilot/APM
```

`RouteUploadManager` 已实现 preparation bound、max active uploads、per-route lifecycle、cancel/retry、failure isolation、progress、metrics 与 terminal summary。

当前 `RouteUploadManager::StartPrepared` 固定创建 `MissionProtocolTransport`。MAVFTP 不是生产默认 Transport。

MissionProtocol 真实硬件垂直链已完成，但最终 Production Mission Ready 仍未证明。

## 10. Mission Transport Abstraction — IMPLEMENTED

`IMissionTransport` 提供 `Prepare / Upload / Tick / Verify / Cancel`、terminal status、progress 与 metrics。

实现：

- `MissionProtocolTransport`：CURRENT PRODUCTION。
- `MAVFTPMissionTransport`：EXPERIMENTAL / HARDWARE HARNESS。

Transport abstraction 不持有 Route、Fleet 或 protocol selection Authority。

## 11. MAVFTP Client / Ingress — IMPLEMENTED

`MavFtpClient` 已实现：

- `FILE_TRANSFER_PROTOCOL #110` ACK/NAK correlation。
- known Remote Endpoint reply routing。
- Create/Open/Truncate、streaming WriteFile、TerminateSession。
- sequence/session/offset validation。
- request timeout、retry 与 bounded file size。

它复用 `DroneSession` endpoint 与 shared UDP sender，不创建 per-drone socket 或第二套 Fleet registry。

## 12. MissionFtpCompiler 与分段 — IMPLEMENTED

```text
Artifact Mission Items
-> Reserved Home expansion
-> Wire Mission Items
-> SegmentDescriptor[]
-> mission.dat segment byte streams
```

`MissionFtpCompiler` 是当前唯一 deterministic segment planner。

`CompileConfig.segmentCapacity` 的计算域是 **Wire Mission Items**，包括 Reserved Home `seq 0`。例如：

```text
Artifact = 9100
Wire = 9101
Capacity = 4000
Segments = 4000 + 4000 + 1101
```

分段合同：

- 第一段 `options = 0`，CLEAR。
- 后续段 `options = 1`，NO_CLEAR。
- StartIndex 连续，无 gap、无 overlap。
- descriptor 按确定顺序 streaming encode。

不得新增第二套互相竞争的 `MissionSegmentPlanner`，除非明确重构并移除原 planner authority。

## 13. MAVFTPMissionTransport — IMPLEMENTED / EXPLICIT ONLY

每段执行：

```text
Create/Open/Truncate @MISSION/mission.dat
-> streaming WriteFile
-> TerminateSession
-> wait TerminateSession ACK
-> commit segment
-> start next SegmentDescriptor
```

上一段 `TerminateSession ACK` 是启动下一段的 commit gate。`All Write ACKed != Mission Ready`。

Hardware Harness 与 Quick Test 已实现；Quick Test 从 `Current/manifest.json` 读取 Artifact identity，再由 `ConsoleArtifactConsumerV2` 完成完整校验。

当前 Quick Test 会使用默认 segment capacity `65535`，因此常规 1000～7000 items 验证均是单 segment。Legacy explicit harness 可传 segment 参数，但真实多段拼接尚未确认。

## 14. NEXT — Phase 2I-C.3.D

尚未开始：

1. 在 deployment config 中定义 `[mavftp]` segment/retry/timeout keys。
2. 将 `segment_items` 明确定义为 Wire Mission Items。
3. Quick Test 读取并传递配置化 segment capacity。
4. 将 segment retry/timeout 接入现有 client/writer。
5. 增加明确逐段日志、公开状态与 metrics。
6. 经用户显式授权，在真实飞控验证多段 CLEAR + NO_CLEAR 拼接。

本阶段不自动授权 MAVFTP production cutover。

## 15. Hardware Evidence Boundary

用户提供的真实硬件观察：

```text
1000–6000 items: USER-CONFIRMED PASS
7000 items: USER-CONFIRMED NAK 2 / Write Failed
```

仓库没有对应日志，飞控源码也未证明精确内部限制。不得写成固定“7000 item limit”。

## 16. FIRMWARE UNKNOWN / NOT PROVEN

- Mission execution timing。
- custom RGB waypoint execution。
- `@MISSION/mission.dat` apply semantics。
- time-control 与 custom `MAV_CMD_USER` behavior。
- remote integrity 和 Mission Ready verification。

最终 Operator 状态必须区分 Transfer Complete、Verification 与 Ready。

## 17. DEFERRED

- MAVFTP production `RouteUploadManager` cutover，等待配置化多段、硬件证据和明确批准。
- 10000-drone chunked/sharded Route Store。
- Replacement Flight、backup drone、conflict predictor 与 safe rejoin trajectory。
