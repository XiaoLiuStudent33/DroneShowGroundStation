# DroneGCS 当前代码架构

Status: CURRENT
Authority: 当前 Production 运行链与 Developer Entry Map
Last Updated: 2026-09-11
源码核对基线：统一见 [Development_Status_CN.md](Development_Status_CN.md) 的基线记录，不在本文重复维护。

本文依据当前源码维护实际架构，是当前运行链与开发入口的说明。当前状态、版本基线和验证证据统一见 [Development_Status_CN.md](Development_Status_CN.md)。[Architecture_Freeze_CN.md](Architecture_Freeze_CN.md) 是早期制定的冻结约束，保留作设计背景；与当前源码或本文冲突时，不直接用旧条款判定当前实现，差异列入状态入口待核对。Route Upload 专项说明见 [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)，其中过期表述同样以当前源码核对结果为准。

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

[共享 DroneLogicalId](../Shared/DroneIdentity/DroneLogicalId.h) 的 TryDroneLogicalIdToRouteSlot 采用非负 canonical 编号，默认 RouteSlotIndex = NumericDroneID（Drone000→0）；保留 checked integer 和 format round-trip。DroneID 与 RouteSlot 仍是不同身份。

[GCSRouteWorkflowCoordinator.inl](../UnrealGCS/Source/DroneShowPlatform1/Private/GCSRouteWorkflowCoordinator.inl) 的 CaptureAutoStartSnapshot：无确认 Mapping 时从命令选择建立目标，按默认编号匹配；存在确认 NPZ Mapping 时由其 Entries 决定目标集合与 RouteSlotIndex，不取后续 UI 勾选作为上传目标。校验 Mapping 新鲜度、数据集/域、在线、endpoint、重复/范围与 Manifest，陈旧 Mapping 拒绝。冻结条款适用冲突见 [当前状态](Development_Status_CN.md)。

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
-> CaptureAutoStartSnapshot：有效确认 Mapping 使用 Entries；无确认 Mapping 使用 SelectedCommandDroneIDs
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

存在确认 Mapping 但校验失败或已陈旧时拒绝启动，不回退到命令选择集合。目标 DroneID 与 RouteSlot 来自上述分支确定的快照。

Start 由 `UGCSRouteWorkflowCoordinator` 的 E.0 AutoStart 状态机拥有：`ValidatingMapping -> Freezing -> SyncingConsole -> StartingJob -> Completed/Failed/CancelledAsStale`。快照包含排序后的 Command Selection、Fleet session/revision、每台 endpoint、ArtifactSet/digests 以及 Manifest Route Record path/size/digest；Accepted 后重新捕获并逐项比较。`bJobStartAttempted` 在调用前设置，重复 Tick/ACK/重开 Widget 不会创建第二个 Job。

展示边界补充（2026-09-03）：AutoStart `Completed` 仅完成本地 Start dispatch，记录发出的 `UploadJobID`，日志为 `State=StartDispatched`。`GetAutoStartPresentation` 按相同 Job 的 CommandStatus/Mirror 投影等待、校验、拒绝或结果未知；不修改协议/上传状态机。启动被拒绝时保留预览身份并更新结果，正式 Job 行仍由权威 Route 页拥有。显式新数据集替换是现有 Job 解绑入口，UI 失败提示不自动释放绑定或产生第二次 Start。

镜像恢复补充：已绑定但尚未取得首份快照的 Job 同样需要 `MarkTransportReconnected` 标记只读恢复，后续 TickClient 重新查询同 Job；无绑定或已有权威错误不启用该恢复。此分支不清除错误、不解除绑定、不重发上传命令。

已实现 Start、Pause、Resume、Cancel、RetryFailed 与 max concurrency 操作，以及 aggregate snapshot、route page 和 route delta。E.1 的 max concurrency 由 UE 已应用值拥有，合法范围 `1..50`；Start 快照后复用现有 DTO，Console 再次严格校验，`EffectiveMaxConcurrentTargets = min(DTO.MaxConcurrentTargets, MatchedTargetCount)`。运行中 Apply 被拒绝，当前 Job 不受后续 UI 修改影响。Upload Job 同时绑定 Artifact identity 与 Frozen Assignment identity；不能只凭任意路径或 UI 内存状态启动。

`RouteUploadManager` 对 Target 使用动态容器和准入队列，不存在固定 2/16/50 长度。共享 `WritePermitPool` 对所有活动 Client 执行 round-robin 公平背压：每 Client `write_window=16`，全局 cap 独立配置 `1..800`，有效值为 `min(ConfiguredGlobalCap, EffectiveMaxConcurrentTargets * PerClientWriteWindow)`。同一 pending Write 的 retry 保留原 permit；成功 ACK、NAK、失败、Cancel、Shutdown 和 teardown 精确释放。控制请求不计入 Write cap。

Route Upload ListView 的身份投影由 `UGCSRouteWorkflowCoordinator` 维护：稳定键为 `UploadJobID + RouteSlotIndex`，行数据仍只读取 `FDSPGCSUploadJobStateMirror`。Console 聚合查询每次生成的 `snapshotToken` 仅标识分页事务，不能触发 UI ListItem 重建。相同成员集合只原位刷新可见行；重新分页期间保留成员并标记同步中，完整分页到齐后才比较并替换变化的成员集合。网络 Token/revision/delta 校验及 Console 上传执行链不受此展示层修复影响。

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

Quick Test 当前从 `Current/manifest.json` 引导，并与生产链共享 `[mavftp]` 的 segment capacity、request/terminate timeout、retry 和 write window 合同。显式 Hardware Harness 仍只能通过受控测试入口启用。

## 12. 当前配置边界

- Console 开发/手工运行：`ConsoleGCS/gcs-config.ini` 或显式指定配置。
- Console DSP：`ConsoleGCS/config/DSPGCSTransport.ini`。
- UE：`UnrealGCS/Config/DefaultGame.ini`。
- Route/Mission 测试部署：`ConsoleGCS/gcs-config-upload.ini`，包含真实写入 gate；不得自动运行。
- 联合交付模板：`Tools/Launcher/config/production.ini` 与 `Tools/Launcher/config/DSPGCSTransport.ini`；`PreparePayload.ps1` 通过严格 allow-list 生成 payload 和哈希 manifest。
- 正式安装首次运行：联合启动器把模板复制到 `%LOCALAPPDATA%/DroneGCS/Config`，以后升级不覆盖，并把绝对配置路径传给 Console。
- 联合航线工作区：默认 `%LOCALAPPDATA%/DroneGCS/ArtifactWorkspace`，由启动器通过同一环境设置提供给 UE 与 Console。
- Protocol contracts：`Shared/DSPGCSProtocol`、`Shared/RouteArtifact`、`Shared/RouteWaypoint`、`Shared/RouteMission`。

生产模板已接入 `[mavftp]`：`segment_items=4000`、request/terminate timeout `750/2500 ms`、retry `3`、per-client write window `16`、global outstanding cap `800`。

## 13. 状态与证据入口

实现完成度、未证明范围、人工/硬件验收及后续顺序仅见 [Development_Status_CN.md](Development_Status_CN.md)。本文件描述调用合同，不维护独立发布就绪判断。

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
| 联合 payload | `Tools/Launcher/PreparePayload.ps1` | Game Target PreBuildSteps、RuntimeDependencies / NonUFS |
| 联合启动 | `Tools/Launcher/Launcher.cpp` | `ManagedBackendSession`、安装标记与每用户目录 |
| 正式安装 | `Tools/Installer/BuildInstaller.ps1` | `DroneGCS.nsi`、Shipping/manifest 校验 |
| Test entry | `ConsolePipelineTests.cpp` | UE `Private/Tests/` |

## 15. 架构阶段记录

阶段报告保留各自基线；当前进度及 UI Replacement Readiness 与 UI 实际验收的区分见 [Development_Status_CN.md](Development_Status_CN.md)。

## 16. Console Presentation Authority

`ConsoleUI` 是 Console 物理输出的唯一 Authority。业务线程只提交完整日志记录或替换 Fleet 快照；容量为 1024 的 MPSC 队列和不可变快照由单一 Renderer 线程消费。Dashboard 的 Fleet、Drone List 与 Runtime Log 在内存中完成布局后按完整帧提交，不允许业务线程操作 cursor、颜色或 stdout。

`auto` 模式使用 Windows stdout Handle 的 `GetFileType`、`GetConsoleMode` 与实时 Console geometry 判定：真实 Windows Console 使用 Dashboard；Pipe、Disk/File、无效 Handle、PowerShell `Tee-Object` 和 CI 使用 Linear 行日志。Dashboard 每次绘制重新读取可见窗口宽高，长行只在 Presentation 层做 UTF-8 边界安全截断，完整原文仍保留在线性输出。

队列满时优先丢弃非关键记录并公开累计 `DroppedLogCount`；Error/Job terminal 记录可抢占非关键记录。若关键记录本身耗尽整个上限，critical drop 也必须出现在同一可观测计数中，不能静默发生。Renderer 停止前排空有界队列并恢复 Console cursor。

## 17. 联合启动与安装运行链

```text
NSIS installed shortcut
-> DroneGCSLauncher.exe
-> preflight: payload / config / process / ports / local IP
-> visible DroneGCS.exe --managed-backend --config <absolute user config>
-> backend Ready event
-> Shipping UE Game EXE
```

`DroneShowPlatform1.Target.cs` 的 Game Target PreBuildSteps 调用 `PreparePayload.ps1`。该脚本编译 Release x64 Console 与 Launcher，只复制 allow-list 中的 EXE、两个配置、README 和三个 VC143 CRT DLL，并生成 SHA-256 manifest；`DroneShowPlatform1.Build.cs` 以 NonUFS loose files 暂存，源码、测试程序、PDB 和开发配置不会进入交付 payload。

安装布局由 `installed-layout.marker` 区分。安装版把可变数据放入 `%LOCALAPPDATA%/DroneGCS`；便携包没有该标记，继续使用包内配置语义。完整控制模式下，Launcher/UE 退出后 Console 保持运行，避免控制后端被意外终止；操作员确认安全后在 Console 按 Ctrl+C 正常释放端口和资源。

## 18. UI 应用入口与本机模拟边界

[UIBridge](../UnrealGCS/Source/DroneShowPlatform1/Private/GCSUIBridgeSubsystem.cpp) 在 Subsystem Initialize 中持有既有 Coordinator，Widget 重绑走视图绑定；业务查询/意图经现有公共 API，Coordinator 的 Mapping/AutoStart/Upload 状态不应依赖短期 Widget 存活。公共接口见 [GCSUIBridgeSubsystem.h](../UnrealGCS/Source/DroneShowPlatform1/Public/GCSUIBridgeSubsystem.h)，保留既有 Fleet/Assignment/Upload Authority。

[仿真 Subsystem](../UnrealGCS/Source/DroneShowPlatform1/Private/GCSSimulationSubsystem.cpp) 使用单工作线程与每机模拟 UDP 端点；它直接包含 Console 的 MDHK dialect。该事实不等于生产 UE 可直接控制真机，其与长期冻结边界的适用关系见状态入口待确认。参数、命令范围、停止/超时清理方法见 [Local_Simulation_CN.md](Local_Simulation_CN.md)。
