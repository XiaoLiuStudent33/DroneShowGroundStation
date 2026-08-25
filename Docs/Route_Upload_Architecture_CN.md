# DroneGCS Route Upload Architecture

Status: CURRENT
Authority: 当前 Route domain、Upload Job、Mission Transport 与下一阶段边界
Last Updated: 2026-08-25
Baseline: `ed96318db10908f8e7d75bb3a0087d2d0976fd4a`

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
Selected-Drone Auto Assignment     IMPLEMENTED (E.0)
Dynamic-N MAVFTP Scheduling        SOFTWARE GATE PASS (E.1)
DSP-GCS Upload Job                 IMPLEMENTED
Console Artifact Consumer          IMPLEMENTED
Console RouteUploadManager         IMPLEMENTED
QGC Parser / Mission Encoder       IMPLEMENTED
MAVFTP Compiler / Writer / Harness IMPLEMENTED
MAVFTP Production Upload           CURRENT PRODUCTION
Count/Home/First/Last Verification PROVEN (9100/4000)
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

E.0 的临时自动映射规则是 `RouteSlotIndex = NumericDroneID - 1`。输入只来自 `SelectedCommandDroneIDs`，DroneID 必须通过共享 canonical parser；零选择、offline/unknown、非法或重复 ID、越界 Slot、缺失 Manifest Route Record 均整体失败。正常 UI 不再依赖手工 `RouteAssignment.BindExplicit`，该命令仅保留诊断用途。

禁止使用 SYSID、Detail Selection、Niagara particle index、屏幕排序或“所有 Online Drone”隐式回退。`M_DroneIdLabel` 只是可视化下标材质，不是 DroneID/RouteSlot Authority。

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

E.0 Start 在长生命周期 `UGCSRouteWorkflowCoordinator` 中执行有界事务：捕获已排序 Command Selection、Fleet session/revision/endpoint 与 Artifact/Manifest/Route record identity，一次性建立 explicit Working Assignment，`Freeze(AllowDegraded)`，等待精确匹配 Transaction/Fleet/Authority/Revision/Digest 的 Console Accepted，再重新校验全部快照并 exactly-once 创建一个多 Target Job。每次新的 Start 都重新建立 Frozen，不复用来源未知的旧 Frozen。

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
-> MAVFTPMissionTransport
-> DroneSession endpoint-bound MavFtpClient
-> shared MAVLink sender/socket
-> ArduPilot/APM
```

`RouteUploadManager` 已实现 preparation bound、max active uploads、per-route lifecycle、cancel/retry、failure isolation、progress、metrics 与 terminal summary。

E.1 的操作员并发 Authority 是现有 UE 输入框与 Apply 按钮。Applied 值合法范围 `1..50`，同一 runtime session 内跨 View 重开保留；Load Preview 不重置。AutoStart/Freeze/Sync/非终态 Job 期间禁止 Apply。Start 在事务入口冻结 Applied 值并写入现有 `Upload Start DTO.MaxConcurrentTargets`；Console 不 clamp、不回退，也不使用独立 Deployment INI 覆盖，非法值拒绝整个 Job。生产有效并发为 `min(DTO.MaxConcurrentTargets, MatchedTargetCount)`。

`RouteUploadManager` 使用动态 Target 容器与准入队列；终态 Target 释放准入位后自动启动后续 Target。所有 `DroneSession` 继续共享单一 MAVLink sender/socket，且每 Session 只有一个 endpoint-bound `MavFtpClient`。每 Client `write_window=16` 与 Target 并发上限是两个不同维度；共享公平 permit pool 的全局 cap 合法范围 `1..800`、默认 `16`，有效值为 `min(ConfiguredGlobalCap, EffectiveMaxConcurrentTargets * PerClientWriteWindow)`。

当前 `RouteUploadManager::StartPrepared` 的 production factory 只创建 `MAVFTPMissionTransport`，并借用 Session 已有 Client。构造或运行失败会明确失败，不会切换协议。

9100 Artifact Items / 9101 Wire Mission Items、segment capacity 4000 的三段真实生产上传已通过 Count/Home/First/Last 验证，Job ElapsedMs=8829。

## 10. Mission Transport Abstraction — IMPLEMENTED

`IMissionTransport` 提供 `Prepare / Upload / Tick / Verify / Cancel`、terminal status、progress 与 metrics。

实现：

- `MAVFTPMissionTransport`：CURRENT PRODUCTION，并由 Hardware Harness / Quick Test 复用。

Transport abstraction 不持有 Route、Fleet 或 production protocol selection Authority；保留它是为了生命周期统一与离线 Fake 注入。

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

## 13. MAVFTPMissionTransport — CURRENT PRODUCTION

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

代码与部署默认 profile 为 `segment_items=4000`、`request_timeout_ms=750`、`terminate_timeout_ms=2500`、`request_retry_count=3`、`write_window=16`。

## 14. Phase 2I-C.3.D — IMPLEMENTED

已完成：

1. deployment config 定义 `[mavftp]` 五项参数。
2. `segment_items` 明确定义为包含 Reserved Home 的 Wire Mission Items。
3. Quick Test、client/writer、逐段日志与 metrics 完成参数接线。
4. 真实飞控完成 9100/4000 三段 CLEAR + NO_CLEAR 拼接和最终验证。
5. Production cutover 完成，旧 Transport 选择器与实现已退休删除。

生产 Route Upload 固定 MAVFTP；配置与 CLI 不再暴露 Transport 选择。

## 15. Hardware Evidence Boundary

用户提供的真实硬件观察：

```text
ArtifactItems=9100 / WireMissionItems=9101
SegmentCapacity=4000 / SegmentCount=3 / SegmentsCommitted=3
Count/Home/First/Last=PASS / Target=Succeeded / ElapsedMs=8829
```

该 USER-CONFIRMED 证据只证明当前固件与当前生产 profile 的该用例，不定义通用飞控上限。

## 16. FIRMWARE UNKNOWN / NOT PROVEN

- Mission execution timing。
- custom RGB waypoint execution。
- time-control 与 custom `MAV_CMD_USER` behavior。
- Mission 执行态的远端完整性观测。

最终 Operator 状态必须区分 Transfer Complete、Verification 与 Ready。

## 17. DEFERRED

- 10000-drone chunked/sharded Route Store。
- Replacement Flight、backup drone、conflict predictor 与 safe rejoin trajectory。
