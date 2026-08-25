# DroneGCS 当前代码架构

Status: CURRENT
Authority: 当前 Production 运行链与 Developer Entry Map
Last Updated: 2026-08-25
Baseline: `17b679265c77d1c2306e1b500574b121b29e525c`

本文只记录当前源码中已经存在的实现。长期约束见 [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)，进度见 [Development_Status_CN.md](Development_Status_CN.md)，Route Upload 语义见 [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)。

## 1. 系统总览

```text
ArduPilot / APM
    <-> MAVLink UDP / explicit MAVFTP harness
ConsoleGCS C++ Backend
    <-> DSP-GCS localhost UDP
Unreal Engine 5.5.4 Frontend
    -> Operator Workflow / Fleet / Route / Cesium / Niagara
```

ConsoleGCS 是 MAVLink、Remote Endpoint Session、真实命令和 Route Upload execution Authority。UE 是 operator workflow、Route domain 与 visualization Authority；UE 不直接发送 MAVLink。

### Console Dashboard styled frame

Interactive Dashboard 由单一 Renderer thread 提交 `ConsoleDashboardFrame`（每行字符加逐 Cell attribute）。Win32 target 使用 `WriteConsoleOutputCharacterW` 后由同一线程写入 `WriteConsoleOutputAttribute`：Total 数字为 `0x09`，Online/`ONLINE` 为 `0x0A`，Offline/`OFFLINE` 为 `0x0C`，其余 Cell 恢复启动时默认属性。Linear/Pipe/File/Tee 仍只输出纯文本，不含 ANSI 或颜色控制序列。

### Fleet bulk command selection

`UGCSUIBridgeSubsystem::BuildEligibleOnlineCommandDroneIDs` 以完整 Fleet Snapshot 为输入，只收集非空 Online DroneID；`ReplaceCommandDroneSelection` 在一个 Authority 操作内替换 `SelectedCommandDroneIDs`，去重且最多广播一次。`BTN_FleetSelectALL` 的 `X/✓` 和 Enabled 状态均实时派生，不保存独立 latch，也不模拟分页 Row click。`UGCSRouteWorkflowCoordinator::IsCommandSelectionMutationBlocked` 将 AutoStart 和非终态 Upload Job 纳入同一写保护门。

## 2. Fleet、Telemetry 与短命令

```text
MavlinkUdpReceiver
-> MavlinkReadOnlyIngress
-> MavlinkDiscoveryService
-> DroneManager
-> DroneSession
-> DataManager
-> DSPGCSProductionTelemetryProvider
-> DSP-GCS localhost UDP
-> DSPGCSProductionTelemetryProviderSubsystem
-> GCSServerAdapterSubsystem
-> FleetManagerSubsystem
```

Fleet identity 是 `Remote IP + UDP Source Port`。SYSID/COMPID 只用于 MAVLink metadata、target 与 ACK validation。相同 endpoint 恢复原 Session/DroneID；Offline 不删除稳定 Session order。

短命令链：

```text
GCSUIBridgeSubsystem
-> DSPGCSProductionCommandProviderSubsystem
-> DSPGCSProductionCommandReceiver
-> CommandManager
-> DroneSession
-> CommandDispatcher
-> MavlinkCommand
-> endpoint-bound adapter
-> shared MavlinkUdpReceiver::SendTo
```

长生命周期 Route Upload 不进入 `CommandDispatcher`。

## 3. Visualization、Route Preview 与 Overlay

唯一批量渲染形态：

```text
FleetManager / RoutePreviewSubsystem
-> one ADroneFleetVisualizationActor
-> one UNiagaraComponent
-> one NS_DroneFleet
-> N stable particles + Route Target data
```

`RoutePreviewSubsystem` 拥有 Native NPZ、Local ENU Route data、Show Origin、SampleRate、Playback 与 Preview mode。Live telemetry 继续由 FleetManager 保存；退出 Preview 后恢复最新 Live snapshot。

Live + Route Overlay 已实现：`RoutePreviewSubsystem` 构建 Frame-0 Route target world positions，`ADroneFleetVisualizationActor` 通过独立 Route Target arrays/visibility 向同一个 Niagara System 提交。Overlay 只负责显示，不修改 Fleet identity、Live particle mapping 或 Assignment。

## 4. RouteAssignment Authority

```text
Fleet snapshot + Route frame 0 + Show Origin
-> RouteAssignmentGeo
-> RouteAssignmentMatcher
-> RouteAssignmentMaterializer
-> RouteAssignmentPipeline
-> URouteAssignmentSubsystem
-> Working / Frozen snapshot + monotonic Revision
```

`URouteAssignmentSubsystem` 是 working/frozen Assignment Authority，支持显式 binding、位置匹配、full/degraded freeze policy、one-to-one DroneID/RouteSlot validation、revision 与 source identity。`RouteSlotIndex` 始终是 0-based route identity，不等于 DroneID、SYSID 或 particle index。

`Shared/DroneIdentity/DroneLogicalId.h` 是 Console/UE 共用的 canonical DroneID formatter/parser。E.0 临时规则由 canonical 正整数计算 `RouteSlotIndex = NumericDroneID - 1`；最少三位、超过 999 自然扩展，解析使用 checked integer 与 format round-trip。Console 默认发现编号也复用同一 formatter。

## 5. Artifact Workspace 与 Waypoint

```text
Frozen Assignment + Route Preview source
-> RouteWaypointGenerationSubsystem
-> RouteWaypointBatchGenerator / RouteWaypointGenerator
-> QGC WPL 110 route files
-> RouteWaypointValidator
-> RouteArtifactWorkspaceManager / Subsystem
-> canonical schema v2 manifest.json + READY
-> Current publication
```

Waypoint 生成使用 Route Local ENU + Show Origin；Mission relative altitude 使用 Local Z。Generator 与 Validator 不依赖 Niagara 或 UE World 反算 GPS。

Artifact identity 包含 source、range、origin、generator、route records、file size/digest 和 ArtifactSet digest。READY 后的 Current Artifact 通过 immutable/canonical contract 供 Console 消费。

## 6. Frozen Assignment DSP 链

```text
URouteAssignmentSubsystem frozen snapshot
-> UDSPGCSFrozenAssignmentSyncSubsystem
-> DSPGCSFrozenAssignmentProtocol schema 2
-> DSPGCSFrozenAssignmentReceiver
-> FrozenAssignmentTransactionAssembler
-> FrozenAssignmentStore
-> ACK / rejection back to UE
```

该链校验 authority session、Fleet session、revision、digest、duplicate DroneID/RouteSlot、full/degraded policy 和 chunk transaction。Console store 是 accepted frozen snapshot 的原子 Authority。

## 7. Upload Job DSP 与 Operator UI

```text
GCSRouteWorkflowCoordinator / WBP_GCS_RouteUpload
-> SelectedCommandDroneIDs snapshot
-> BuildAndReplaceExplicitWorkingAssignment
-> Freeze(AllowDegraded)
-> UDSPGCSFrozenAssignmentSyncSubsystem exact accepted identity
-> UDSPGCSUploadJobSubsystem
-> DSPGCSUploadJobProtocol schema 2
-> DSPGCSUploadJobReceiver
-> ConsoleUploadJobAuthority
-> snapshots / route pages / deltas / command ACK
-> FDSPGCSUploadJobStateMirror
-> UI state and progress
```

Start 由 `UGCSRouteWorkflowCoordinator` 的 E.0 AutoStart 状态机拥有：`ValidatingMapping -> Freezing -> SyncingConsole -> StartingJob -> Completed/Failed/CancelledAsStale`。快照包含排序后的 Command Selection、Fleet session/revision、每台 endpoint、ArtifactSet/digests 以及 Manifest Route Record path/size/digest；Accepted 后重新捕获并逐项比较。`bJobStartAttempted` 在调用前设置，重复 Tick/ACK/重开 Widget 不会创建第二个 Job。

已实现 Start、Pause、Resume、Cancel、RetryFailed 与 max concurrency 操作，以及 aggregate snapshot、route page 和 route delta。E.1 的 max concurrency 由 UE 已应用值拥有，合法范围 `1..50`；Start 快照后复用现有 DTO，Console 再次严格校验，`EffectiveMaxConcurrentTargets = min(DTO.MaxConcurrentTargets, MatchedTargetCount)`。运行中 Apply 被拒绝，当前 Job 不受后续 UI 修改影响。Upload Job 同时绑定 Artifact identity 与 Frozen Assignment identity；不能只凭任意路径或 UI 内存状态启动。

`RouteUploadManager` 对 Target 使用动态容器和准入队列，不存在固定 2/16/50 长度。共享 `WritePermitPool` 对所有活动 Client 执行 round-robin 公平背压：每 Client `write_window=16`，全局 cap 独立配置 `1..800`，有效值为 `min(ConfiguredGlobalCap, EffectiveMaxConcurrentTargets * PerClientWriteWindow)`。同一 pending Write 的 retry 保留原 permit；成功 ACK、NAK、失败、Cancel、Shutdown 和 teardown 精确释放。控制请求不计入 Write cap。

## 8. Console Artifact Consumer 与 Upload Authority

`ConsoleArtifactConsumerV2` 在 Console trust boundary 内重新验证：

- allowlisted workspace 与 canonical path。
- `Current`、`READY`、schema v2 manifest。
- ArtifactSetId / ArtifactSetDigest 双绑定。
- route allowlist、file size/digest、waypoint semantics。
- read lease 与 TOCTOU identity。

`ConsoleUploadJobAuthority` 拥有 Job state、command idempotency、Artifact/Frozen Assignment binding、aggregate/route snapshots、cancel/retry/concurrency 与 executor-facing immutable view。

## 9. 当前 Production Route Upload

```text
ConsoleUploadJobAuthority
-> RouteUploadManager
-> MissionPreparationWorker
-> Console Artifact mission rows
-> IMissionTransport
-> MAVFTPMissionTransport
-> DroneSession endpoint-bound MavFtpClient
-> shared endpoint-bound sender
-> ArduPilot/APM
```

`RouteUploadManager` 负责 bounded preparation、active upload concurrency、per-target state、cancel、retry、failure isolation、progress、metrics 和 terminal summary。

当前 `RouteUploadManager::StartPrepared` 通过单一 production factory 创建 `MAVFTPMissionTransport`。Client 缺失或 MAVFTP 运行失败会进入明确失败，不存在其他协议回退。

`MAVFTPMissionTransport` 封装分段 compile/write/commit，并在最后一个 Terminate ACK 后复用通用 Mission Count/Home/First/Last 验证组件。

## 10. Mission Transport abstraction

`IMissionTransport` 定义 per-target lifecycle：

```text
Prepare -> Upload -> Tick -> Verify / Cancel -> terminal status + metrics
```

当前实现：

- `MAVFTPMissionTransport`：唯一生产实现，同时供显式 Hardware Harness / Quick Test 复用。

抽象本身不提供生产协议选择；`RouteUploadManager` 的 production factory 固定构造 MAVFTP，抽象仅保留测试注入 seam。

## 11. MAVFTP 实验与硬件验证链

```text
known endpoint FILE_TRANSFER_PROTOCOL ingress
-> DroneSession endpoint-bound MavFtpClient
-> MissionFtpCompiler
   Artifact items
   -> Reserved Home expansion
   -> Wire Mission Items
   -> SegmentDescriptor[]
-> MAVFTPMissionTransport
-> Create/Open/Truncate @MISSION/mission.dat
-> streaming WriteFile
-> TerminateSession ACK commit gate
-> next segment
```

`MavFtpClient` 已实现 ACK/NAK correlation、timeout/retry、streaming WriteFile、offset validation 与 TerminateSession。FTP reply 只路由到已知 endpoint Session；不创建第二套 Fleet registry 或 socket。

`MissionFtpCompiler` 是唯一 deterministic segment planner。`segmentCapacity` 计算 Wire Mission Items，包含 Reserved Home `seq 0`。第一段 options=0/CLEAR，后续段 options=1/NO_CLEAR；StartIndex 连续、无 gap、无 overlap。

`MAVFTPMissionTransport` 必须等待上一段 `TerminateSession ACK` 后才开始下一段。Hardware Harness 在上传后检查 FC Mission Count、Home seq 0、first artifact seq 1 和 last seq。

显式入口：

- Quick Test：`DroneGCS.exe --mavftp-test <DroneID> --route-slot <N> [--items <N>]`。
- Legacy explicit Hardware Harness 参数仍保留。

Quick Test 当前从 `Current/manifest.json` 引导，但会使用默认 segment capacity 65535；尚未接入 `[mavftp]` 配置。MAVFTP 只通过显式测试入口启用。

## 12. 当前配置边界

- Console deployment：`ConsoleGCS/gcs-config.ini` 或显式指定配置。
- Console DSP：`ConsoleGCS/config/DSPGCSTransport.ini`。
- UE：`UnrealGCS/Config/DefaultGame.ini`。
- Route/Mission 测试部署：`ConsoleGCS/gcs-config-upload.ini`，包含真实写入 gate；不得自动运行。
- Protocol contracts：`Shared/DSPGCSProtocol`、`Shared/RouteArtifact`、`Shared/RouteWaypoint`、`Shared/RouteMission`。

当前没有 `[mavftp] segment_items / segment_retry_count / segment_timeout_ms` 配置接线。

## 13. 尚未实现或尚未证明

- Phase 2I-C.3.D 配置化多段 MAVFTP 上传。
- Quick Test 配置化 segment capacity、segment retry/timeout 和完整逐段日志/metrics。
- MAVFTP production `RouteUploadManager` cutover。
- 真实飞控多段 CLEAR + NO_CLEAR 拼接验证。
- Production Mission Ready verification 与完整 firmware capability model。
- 10000-drone chunked/sharded Route Store。
- Backup Drone Replacement 与 conflict/rejoin workflow。

## 14. Developer Entry Map

| 目标 | 首要入口 | 关联入口 |
|---|---|---|
| Discovery / Fleet identity | `MavlinkDiscoveryService`、`DroneManager` | `MavlinkReadOnlyIngress`、`DroneSession` |
| Telemetry / short command | `DroneSession`、`CommandManager` | DSP telemetry/command providers |
| Visualization / Overlay | `RoutePreviewSubsystem`、`DroneFleetVisualizationActor` | `NS_DroneFleet` asset |
| RouteAssignment | `RouteAssignmentSubsystem`、`RouteAssignmentPipeline` | Geo/Matcher/Materializer |
| Artifact / Manifest | `RouteArtifactWorkspaceSubsystem`、`RouteArtifactWorkspaceManager` | Shared RouteArtifact contract |
| Waypoint | `RouteWaypointGenerationSubsystem` | BatchGenerator、Generator、Validator |
| Frozen Assignment DSP | `DSPGCSFrozenAssignmentSyncSubsystem` | Console Receiver、Assembler、Store |
| Upload Job DSP/UI | `DSPGCSUploadJobSubsystem`、`GCSRouteWorkflowCoordinator` | Console Receiver、Authority、StateMirror |
| Production upload | `RouteUploadManager` | `MissionPreparationWorker`、`MAVFTPMissionTransport`、`MavFtpClient` |
| MAVFTP test chain | `MAVFTPMissionHardwareTest` | `MavFtpClient`、`MissionFtpCompiler`、`MAVFTPMissionTransport` |
| Test entry | `ConsolePipelineTests.cpp` | UE `Private/Tests/` |

## 15. 当前事实标签

- Phase 2A～2G：IMPLEMENTED。
- RouteUploadManager / UI-3C / MAVFTP production vertical slice：IMPLEMENTED。
- MissionTransport / MissionFtpCompiler / MAVFTP Writer / Hardware Harness / Quick Test：IMPLEMENTED。
- Production Transport：MAVFTP only。
- Phase 2I-C.3.D.1-D.3：IMPLEMENTED；D.4 冻结单 Transport 并删除旧选择路径。
- Mission Ready、timing、RGB 与 mission.dat apply：FIRMWARE UNKNOWN / NOT PROVEN。

## 16. Console Presentation Authority

`ConsoleUI` 是 Console 物理输出的唯一 Authority。业务线程只提交完整日志记录或替换 Fleet 快照；容量为 1024 的 MPSC 队列和不可变快照由单一 Renderer 线程消费。Dashboard 的 Fleet、Drone List 与 Runtime Log 在内存中完成布局后按完整帧提交，不允许业务线程操作 cursor、颜色或 stdout。

`auto` 模式使用 Windows stdout Handle 的 `GetFileType`、`GetConsoleMode` 与实时 Console geometry 判定：真实 Windows Console 使用 Dashboard；Pipe、Disk/File、无效 Handle、PowerShell `Tee-Object` 和 CI 使用 Linear 行日志。Dashboard 每次绘制重新读取可见窗口宽高，长行只在 Presentation 层做 UTF-8 边界安全截断，完整原文仍保留在线性输出。

队列满时优先丢弃非关键记录并公开累计 `DroppedLogCount`；Error/Job terminal 记录可抢占非关键记录。若关键记录本身耗尽整个上限，critical drop 也必须出现在同一可观测计数中，不能静默发生。Renderer 停止前排空有界队列并恢复 Console cursor。
