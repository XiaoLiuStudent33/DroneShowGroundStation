# DroneGCS Architecture Freeze

Status: CURRENT
Authority: 项目最高长期架构约束
Last Updated: 2026-08-24
Applies To: `ConsoleGCS`、`UnrealGCS`、`Shared`、Route domain
Supersedes: 早期阶段性架构建议中的冲突内容

## 1. 文档职责

本文件只定义长期稳定的 Authority、身份、数据流、性能和 Production 边界。当前类和调用细节见 [Code_Architecture_CN.md](Code_Architecture_CN.md)，Route Upload 方向见 [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)，当前进度见 [Development_Status_CN.md](Development_Status_CN.md)。

Source of Truth 优先级：

1. 当前 Production 源码。
2. 已存在并通过的自动化测试代码。
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
- Offline 只改变在线状态，不删除 Session、DroneID、最后快照或稳定顺序。
- 相同 endpoint 恢复后继续复用原 Session/DroneID。
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

1000 drones 是当前稳定基线。10000 drones 是探索性/未来扩展路径，必须先测量 memory、UDP、parse、session、DSP、UE apply 和 Niagara，再决定优化。

## 6. Production / Developer 边界

- Production 不依赖 Mock、Acceptance 或 Regression Test。
- Developer Provider 只能依赖 Production interface，默认关闭。
- Production Provider 失败只能进入 `Disconnected/Error/Link Lost`，不得自动 fallback 到 Mock。
- `DSPGCSLoopbackTransport` 名称包含 Loopback，但当前代表 `127.0.0.1:12001 <-> 127.0.0.1:12002` 正式同机 Production UDP。

## 7. Route Domain 冻结方向

### 7.1 IMPLEMENTED

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

### 7.2 FROZEN DIRECTION / NEXT

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
- 9100/4000 三段生产上传的 Count/Home/First/Last verification 已通过；真实 Mission execution timing、RGB 执行、time-control 与 custom MAV_CMD_USER 仍为 Firmware Unknown。

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
