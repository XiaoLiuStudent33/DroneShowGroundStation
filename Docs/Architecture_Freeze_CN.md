# DroneGCS Architecture Freeze

Status: CURRENT
Authority: 项目最高长期架构约束
Last Updated: 2026-09-11（事实适用范围核对；未重新冻结边界）
Applies To: `ConsoleGCS`、`UnrealGCS`、`Shared`、Route domain、联合启动与安装交付
Supersedes: 早期阶段性架构建议中的冲突内容

## 1. 文档职责

本文件只定义长期稳定的 Authority、身份、数据流、性能和 Production 边界。当前类和调用细节见 [Code_Architecture_CN.md](Code_Architecture_CN.md)，Route Upload 方向见 [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)，当前进度见 [Development_Status_CN.md](Development_Status_CN.md)。

Source of Truth 优先级：

1. 当前 Production 源码。
2. 已有自动化运行日志及当时基线；测试代码只说明断言范围，不独立证明运行通过。
3. 用户提供的本地审计和人工验证结果；必须标明证据等级。
4. 本文件仍成立的冻结原则。
5. 当前专项 Architecture 文档。
6. Audit 与历史记录。

本文不是开发日志。未来设计必须明确标记为 `PLANNED` 或 `DEFERRED`，不得写成已实现事实。

## 2. 系统 Authority

1. **UE**：Frontend、Visualization、Operator Workflow。
2. **ConsoleGCS**：Backend、MAVLink、Drone Network、Remote Endpoint 路由。
3. **DSP-GCS**：UE 与 ConsoleGCS 之间的版本化同机 Production 边界；当前正式 Transport 是 localhost UDP。
4. **Shared**：只拥有 DSP Protocol、Wire Types、Transport interface/config/constants；Console/UE 领域状态不得进入 Shared。
5. **FleetManagerSubsystem**：UE 真实 Fleet、稳定 `DroneOrder`、详情选择和 Fleet Geo Origin 的 Authority。
6. **GCSUIBridgeSubsystem**：UI lifecycle、页面/面板、命令选择、命令提交与 Route UI 协调者；不拥有 Route 样本或 Fleet 网络身份。
7. **URoutePreviewSubsystem**：当前 Route Preview 数据、Show Origin、SampleRate、Frame 和 Playback 的 Authority。
8. **URouteAssignmentSubsystem**：UE RouteAssignment、Freeze 和 Revision 的 Authority。
9. **Route Artifact / Waypoint services**：生成、验证、Manifest、READY 与 Artifact identity 的 Route-domain Authority；不拥有 Flight Controller 网络。
10. **ConsoleUploadJobAuthority / RouteUploadManager**：Console Upload Job 状态与执行 Authority；不得迁入 UE 或短命令队列。

## 3. Fleet Identity 与 Session

- Fleet network identity 固定为 `Remote IP + UDP Source Port`。
- MAVLink SYSID/COMPID 是协议元数据和 ACK/target 字段，不是 Fleet unique identity。
- `DroneManager -> DroneSession` 是 Console Session ownership。
- 普通生产配置下 Offline 只改变在线状态，不删除 Session、DroneID、最后快照或稳定顺序。既有仿真清理例外：AppConfig 默认关闭 clear_offline_simulation；开启后仅删除 IP 为 127.0.0.1、MAVFTP/MissionUpload service 均关闭且 HeartbeatTimeout 的 Session。源码为 [DroneManager::RemoveOfflineSimulationDrones](../ConsoleGCS/DroneManager.cpp) 与 [应用循环](../ConsoleGCS/ConsoleGCSApplication.cpp)，不是普通生产机删除规则。
- 未删除的相同 endpoint 恢复后继续复用原 Session/DroneID；不能对已被专用仿真清理删除的对象承诺同一身份。
- 当前 DroneID 在 Console 进程生命周期内稳定；Console 重启后的持久化身份尚未保证。
- RouteSlot 是航线槽位身份，不是 Drone、DroneID、SYSID 或 particle index。

## 4. Production 数据流

```text
Console Telemetry
MavlinkUdpReceiver
-> MavlinkReadOnlyIngress
-> MavlinkDiscoveryService
-> DroneManager
-> DroneSession
-> DataManager
-> DSPGCSProductionTelemetryProvider
-> DSP-GCS localhost UDP

UE Telemetry
DSPGCSProductionTelemetryProviderSubsystem
-> GCSServerAdapterSubsystem
-> FleetManagerSubsystem
-> UI / DroneFleetVisualizationActor

UE Command
GCSUIBridgeSubsystem
-> DSPGCSProductionCommandProviderSubsystem
-> DSP-GCS localhost UDP
-> DSPGCSProductionCommandReceiver
-> CommandManager
-> DroneSession
-> CommandDispatcher
-> MavlinkCommand
-> endpoint-bound sender
-> shared UDP socket
-> Remote Endpoint
```

`GCSServerAdapterSubsystem` 是 Network/Worker 到 GameThread Fleet submit 的唯一正式边界。网络线程不得直接修改 Fleet、UI 或 Niagara。

## 5. Visualization 与性能

冻结链：

```text
FleetManager / RoutePreview
-> one ADroneFleetVisualizationActor
-> CesiumGeoreference
-> one UNiagaraComponent
-> one NS_DroneFleet
-> N stable particles
```

禁止：

- per-drone Actor
- per-drone Component
- per-drone Niagara System
- per-drone Widget/TextRender
- per-drone Tick/Timer/Thread

规模边界仍须以 memory、UDP、parse、session、DSP、UE apply 和 Niagara 分项测量决定优化。既有 1000/10000 规模表述不作为当前验收结论，Console 20K 采样也不扩大 UE 或硬件承诺；当前证据与范围见 [Development_Status_CN.md](Development_Status_CN.md)。

## 6. Production / Developer 边界

- Production 不依赖 Mock、Acceptance 或 Regression Test。
- Developer Provider 只能依赖 Production interface，默认关闭。
- Production Provider 失败只能进入 `Disconnected/Error/Link Lost`，不得自动 fallback 到 Mock。
- `DSPGCSLoopbackTransport` 名称包含 Loopback，但当前代表 `127.0.0.1:12001 <-> 127.0.0.1:12002` 正式同机 Production UDP。
- 正式交付必须包含编译后的 UE、Console、配置模板和所需运行库，禁止依赖开发机源码、Visual Studio、UE 编辑器或开发机绝对路径。
- 用户入口固定为联合启动器；它先启动可见 Console 后端，等待真实 Ready 后再启动 UE。UE 根目录 Game EXE 只是前端入口，不作为交付快捷方式。
- 正式安装目录中的文件视为只读程序资产。配置、日志、UE Saved 与航线工作区必须写入当前用户 `%LOCALAPPDATA%/DroneGCS`，升级不得静默覆盖用户配置。
- 完整控制模式下，UE 或启动器退出不得自动杀死仍承担控制职责的 Console；确认飞行和上传停止后，由操作员在 Console 中正常退出。
- 启动器可以校验本机是否具有 `192.168.1.254`，但不得自动修改网卡；安装器只管理限定到后端 EXE 和 `192.168.1.254:10001/UDP` 的专用入站防火墙规则。

## 7. Route Domain 冻结方向

### 7.1 既有 Route 调用合同

- `URoutePreviewSubsystem` 是 Route Preview Authority。
- `FRoutePreviewData` 保留原始 Local ENU，RouteSlot 为 0-based preview identity。
- Route Preview 与 Live telemetry 使用明确的 Visualization Mode，不能同时写同一组视觉数组。
- Live 模式可以通过同一个 `ADroneFleetVisualizationActor` / Niagara System 显示独立 Route Target overlay；Overlay 不拥有 Assignment。
- SampleRate 必须显式配置；unset 为合法状态，禁止猜 24/25/30/50/60 Hz。
- 2026-08-28 操作员明确确认本项目表演按 **10 帧/秒** 执行：航线管理导入及授时预览显式使用 `ScheduledShowSampleRateHz=10`；普通独立 Preview 导入仍支持显式自定帧率。该值不是从 NPZ 猜测出的元数据。
- RouteAssignment、Freeze/Revision、Artifact Workspace、Manifest/READY、Waypoint Generator/Validator 已实现。
- Frozen Assignment 与 Upload Job 已通过 DSP-GCS 版本化协议连接 UE 和 Console。
- Console Artifact Consumer、Upload Job Authority、`RouteUploadManager`、Mission Preparation 与 `IMissionTransport` 已实现。
- 当前 Production Route Upload Transport 唯一固定为 `MAVFTPMissionTransport`。
- `MavFtpClient`、FTP ingress、`MissionFtpCompiler`、`MAVFTPMissionTransport`、Hardware Harness 与 Quick Test 已实现并完成生产接线。

### 7.2 冻结方向

- Waypoint 生成必须使用 `Route Local ENU + Show Origin`，禁止从 UE World Position 反推 GPS。
- RouteAssignment 必须保持独立 UE Route-domain Authority，不能藏在 FleetManager。
- `DroneID != RouteSlotIndex`，不得自动按相同数字匹配。
- `.waypoints` 必须作为可人工检查、可追溯、可哈希的落盘 Artifact。
- UE 不直接发送 MAVLink Route Upload。
- 正式 Route Upload 链必须是：

```text
UE Route Workflow
-> DSP-GCS
-> Console Route Upload Backend
-> DroneSession endpoint authority
-> shared UDP sender
-> Flight Controller
```

- 长生命周期 Route Upload 不能塞入现有短命令 `CommandDispatcher`。
- Route Upload 必须复用 shared UDP sender 和现有 Remote Endpoint authority。
- 当前生产 `RouteUploadManager` 只创建 `MAVFTPMissionTransport`；构造或运行失败必须明确失败，不允许协议回退。
- MAVFTP 必须复用现有 `MissionFtpCompiler` segment plan，且 `segmentCapacity` 以包含 Reserved Home 的 Wire Mission Items 计数。
- 生产链复用 `DroneSession` 已有 endpoint-bound Client、Tick owner 与 shared UDP sender，不创建第二套 Client 或 Socket。

### 7.3 DEFERRED

- 10000-drone chunked/sharded Route Store。
- Backup Drone Replacement、故障补点、冲突预测、120-degree rejoin rule。
- 这些未来能力只允许通过动态 Assignment 的 Owner/State/Revision/Timestamp 扩展，不得提前写成当前功能。

## 8. Route 与 Mission 高风险语义

- Preview geographic altitude 使用 `ShowOriginAltitude + LocalZ`。
- `MAV_FRAME_GLOBAL_RELATIVE_ALT` Waypoint altitude 必须使用 Local Z；不能把 Preview geographic altitude写入 relative-alt mission。
- Preview SampleRate 不等于飞控 Mission execution rate。
- Legacy RGB `param1/param2/param3` 不是标准 `MAV_CMD_NAV_WAYPOINT` 语义，属于定制固件兼容行为。
- `All Write ACKed != Mission Ready`。
- 9100/4000 历史上传、Mission timing、RGB、time-control 与 custom MAV_CMD_USER 的证据和未知项统一见 [Development_Status_CN.md](Development_Status_CN.md)。历史上传结果不构成当前源码或任意固件的完整验收。

## 9. 配置与网络

- Console deployment、Console DSP transport、UE config、代码安全 fallback 和 protocol constants 各自保持单一职责。
- 不建立第二套 endpoint、port、fleet identity 或 command selection Authority。
- Session Remote Endpoint 始终来自真实入站 MAVLink datagram，不从 SYSID 或 UE particle 推导。
- Route Upload 必须复用现有 shared UDP sender；禁止新增每机 socket 或第二套网络栈。

## 10. 变更纪律

允许改变冻结边界的理由仅限：

- 明确新业务需求无法由现有边界安全承载。
- 明确 Bug。
- 有测量证据的性能瓶颈。
- 经审计确认的协议/固件事实变化。

“更优雅”“文件更少”或工具建议不是修改冻结边界的理由。

## 11. 阅读顺序

1. 本文件。
2. [Code_Architecture_CN.md](Code_Architecture_CN.md)。
3. [Development_Status_CN.md](Development_Status_CN.md)。
4. 当前任务相关专项 Architecture。
5. Audit。
6. `History/`。

## 12. 既有实现与冻结条款的待确认关系

本次仅纠正源码事实及适用范围，不变更上述冻结合同，不宣布整个架构合规。

- 第 7.2 节“不得自动按相同数字匹配”原条款保留；[DroneLogicalId.h](../Shared/DroneIdentity/DroneLogicalId.h) 的 TryDroneLogicalIdToRouteSlot 当前默认数字匹配，且 [Coordinator](../UnrealGCS/Source/DroneShowPlatform1/Private/GCSRouteWorkflowCoordinator.inl) 的 CaptureAutoStartSnapshot 优先采用确认 NPZ Mapping 的 Entries。不能把实际默认规则描述为 DroneID 与 RouteSlot 是同一身份；条款与实现的关系待验证/总控确认。
- 第 5 节为 Visualization 热路径合同。[FlightControl::StartHoverMission](../ConsoleGCS/FlightControl.cpp) 另有每活动 Session 工作线程；本文未据此扩大或豁免既有线程约束，后端适用关系待确认。
- [GCSSimulationSubsystem.cpp](../UnrealGCS/Source/DroneShowPlatform1/Private/GCSSimulationSubsystem.cpp) 在 UE 内使用 MAVLink dialect 与本机模拟端点；用途为本机模拟，不据此改写生产通信 Authority。其与第 2/6/9 节边界的适用关系待确认。
- 当前核对基线、验证结果、证据缺口及下一阶段仅维护在 [Development_Status_CN.md](Development_Status_CN.md)。
