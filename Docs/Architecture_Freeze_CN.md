# DroneGCS Architecture Freeze

本文件是 Cleanup Phase 3A~3H 完成后的维护基线。除明确 Bug、明确新功能需求或有测量证据的性能瓶颈外，不改变下列边界；“更优雅”“少几个文件”或工具建议重构均不是修改理由。

## 1. 冻结的核心边界

1. Fleet Identity：`Remote IP + UDP Source Port`；SYSID 不是 Fleet Identity。
2. Session Owner：`DroneManager -> DroneSession`；Offline Session 保留，同 Endpoint 恢复原 Session。
3. Telemetry Storage：每个 `DroneSession` 拥有自己的 `DataManager` 线程安全快照。
4. MAVLink Decode：Datagram 只由 `MavlinkReadOnlyIngress` 解码；`MavlinkDiscoveryService` 只消费 decoded HEARTBEAT。
5. Command：`CommandManager -> DroneSession -> CommandDispatcher -> MavlinkCommand`；ACK 归属当前 Endpoint Session。
6. DSP：Shared 只拥有 Protocol、Wire Types、Transport Interface/Config/Constants；Console/UE 业务状态不进入 Shared。
7. UE Thread Boundary：`GCSServerAdapterSubsystem` 是 Telemetry 到 Fleet 唯一 GameThread handoff 和原子提交边界。
8. UE Fleet：`FleetManagerSubsystem` 是 Fleet、DroneOrder、Selection、GeoOrigin 的唯一 Owner。
9. UI：`GCSUIBridgeSubsystem` 协调生命周期、Selection、Command 和 Fleet 事件；`FGCSUIWidgetPresenter` 只做 Widget 查找、文本、颜色和可见性派生。
10. Visualization：`FleetManager -> DroneFleetVisualizationActor -> Cesium -> one Niagara System -> N particles`；不使用 per-drone Actor/Component/Tick。
11. Config：Console Deployment、Console DSP、UE `DefaultGame.ini`、代码 fallback、Protocol/Safety constants 各自职责固定，不建立第二套 Production Endpoint 真值。
12. Runtime 分类：Production 不依赖 Mock、Acceptance、Regression Test；Developer Provider 只依赖 Production interface，且默认关闭。

## 2. Production 调用树

```text
Console Telemetry
MavlinkUdpReceiver
-> MavlinkReadOnlyIngress
-> MavlinkDiscoveryService
-> DroneManager
-> DroneSession
-> DataManager
-> DSPGCSProductionTelemetryProvider
-> ConsoleDSPGCSLoopbackTransport

Console Command
DSPGCSProductionCommandReceiver
-> CommandManager
-> DroneSession
-> CommandDispatcher
-> MavlinkCommand
-> UdpClient / shared MavlinkDatagramSender
-> Drone Remote Endpoint

UE Telemetry
DSPGCSLoopbackTransport
-> DSPGCSProductionTelemetryProviderSubsystem
-> GCSServerAdapterSubsystem
-> FleetManagerSubsystem
-> UI / DroneFleetVisualizationActor
-> Cesium / Niagara

UE Command
GCSUIBridgeSubsystem
-> DSPGCSProductionCommandProviderSubsystem
-> DSPGCSLoopbackTransport
-> Console Production Command Receiver
```

`DSPGCSLoopbackTransport` 是当前 `127.0.0.1:12001 <-> 127.0.0.1:12002` 正式同机 Production 通道，不是 Test Transport。

## 3. Developer Entry Map

| 我要修改 | 主要入口 | 必要时第二入口 |
|---|---|---|
| Drone 发现 | `MavlinkDiscoveryService` | `MavlinkReadOnlyIngress` |
| Endpoint Identity | `DroneDiscovery.h::DiscoveredDroneIdentity` | `DroneManager` |
| GPS / Inbound Mode 解析 | `MavlinkParser::Parse/GetModeName` | 字段扩展时 `FlightData.h` |
| Online / Offline | `DataManager::RefreshOnlineStatus` | `DroneSession::RefreshHeartbeatStatus` |
| 新增 MAVLink 命令 | `DroneCommandRequest`、`DroneSession::SubmitCommand` | `CommandDispatcher`、`MavlinkCommand`；跨 UE 时还需 DSP 两端 mapping |
| ACK 生命周期 | `CommandDispatcher::HandleAck` | `MavlinkReadOnlyIngress` 的 Endpoint 路由 |
| DSP Wire / Transport | `Shared/DSPGCSProtocol`、`Shared/DSPGCSTransport` | Console/UE Production Provider |
| UE Fleet | `FleetManagerSubsystem` | `GCSDataTypes.h` |
| Server Link | `DSPGCSProductionTelemetryProviderSubsystem` | UI 派生显示时 `GCSUIBridgeSubsystem` |
| UI 按钮行为 | `GCSUIBridgeSubsystem` | Command Provider |
| UI 文本 / 颜色 | `GCSUIWidgetPresenter` | 无 |
| Niagara 数据桥 | `DroneFleetVisualizationActor` | 视觉资产变化时 `NS_DroneFleet` |
| Console 配置 | `AppConfig`、`gcs-config.ini` | `DSPGCSTransportConfigLoader`、`DSPGCSTransport.ini` |
| UE 配置 | `DefaultGame.ini` | 对应 Production Provider |
| Console 测试 | `ConsoleGCSTests`、`ConsoleGCS/tests/` | 无 |
| UE 测试 | `Private/Tests/` 下 `DroneGCS.*` | 无 |

## 4. MAVLink 维护入口

当前清单已集中记录在 `Code_Architecture_CN.md` 的 MAVLink Inbound、Outbound、MAV_CMD 三节；实现 Owner 是 `MavlinkParser`、`DroneCommandRequest`、`CommandDispatcher` 和 `MavlinkCommand`。文档已覆盖当前 #0/#1/#24/#30/#33/#77、#0/#11/#66/#76、ARM/DISARM、TAKEOFF、LAND、RTL 及 Inbound mode 映射，因此不创建第二套代码常量 Catalog。

## 5. Runtime 分类与配置

- Production：Console `DroneGCS`；UE Production Provider、Adapter、Fleet、UI、Visualization。
- Regression：Console `ConsoleGCSTests`；UE `Private/Tests`。
- Acceptance：Console `DSPGCSAcceptance`；UE `Developer/Acceptance`。
- Developer Mock：UE `Developer/Mock`。
- Developer 开关必须显式启用且默认 `False`。Production 初始化失败只能进入 `Disconnected/Error/Link Lost`，不得 fallback 到 Mock。

## 6. Future Only

- DSP-GCS 跨机器/LAN 部署及 Deployment Profile。
- 有真实增长需求后再评估 Developer Module。
- 有协议演进需求后再升级 DSP schema/wire format。
- 10K/20K Scaling：先测量 UDP、parse、heartbeat、session、DSP、UE apply 和 Niagara，再决定优化点。
- 如跨 LAN 需求出现，可评估把 `DSPGCSLoopbackTransport` 重命名为更通用的 UDP Transport；当前不改。

Cleanup 阶段到此结束，后续进入无人机表演平台功能开发。
