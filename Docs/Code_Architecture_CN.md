# 无人机地面站当前代码架构

本文记录当前代码真实运行链和必须保持的架构约束。它描述已经存在的实现；标为 Future / Planned 的内容尚未实现。

## 1. 系统总览

```text
BGC2 / Real APM
    -> MAVLink UDP
ConsoleGCS
    -> DSP-GCS
UnrealGCS
    -> GCSServerAdapter
    -> FleetManager
       |-> UMG UI
       `-> Cesium -> Niagara
```

ConsoleGCS 是 MAVLink、Endpoint Session 和命令执行的权威端。UnrealGCS 消费 DSP-GCS 数据，在 GameThread 上维护 Fleet，并负责 UI 与地理可视化。

## 2. Console 启动入口

```text
main
-> ConsoleGCSApplication::Run
-> AppConfig::Load(gcs-config.ini)
-> mavlink_network_enabled=true
-> RunProductionMavlink
```

Cleanup Phase 3C 后，Console 只保留多机 MAVLink Production 路径。`mavlink_network_enabled=false` 会明确阻止启动，不再回退到固定 Target 的单会话模式。

## 3. 无人机发现链

```text
MavlinkUdpReceiver
-> MavlinkReadOnlyIngress
-> MAVLink #0 HEARTBEAT
-> MavlinkDiscoveryService
-> DroneManager::RegisterOrRefreshDrone
-> DroneSession
```

只有有效的非 GCS HEARTBEAT 可以创建或刷新 Session。其他 MAVLink 消息只能进入已经由该 Endpoint 建立的 Session。

## 4. Endpoint 身份模型

Fleet 唯一身份是：

```text
Remote Endpoint = Remote IP + UDP Source Port
```

- 不同 IP：不同无人机。
- 相同 IP、不同 Source Port：不同无人机。
- IP 和 Source Port 都相同：同一无人机，恢复原 DroneSession/DroneID。
- SYSID/COMPID 只用于 MAVLink `target_system/target_component` 和 ACK 路由。
- SYSID 不是 Fleet 唯一身份；允许多架模拟机同时使用 SYSID=1。

## 5. Online / Offline 生命周期

```text
Discovered -> Online -> Heartbeat Timeout -> Offline -> Heartbeat Recovery -> Online
```

Offline 只改变在线状态。必须继续保留：

- DroneSession
- DroneID
- DroneManager SessionOrder
- 最后已知遥测
- UE Fleet 成员、DroneOrder 和粒子索引

相同 Endpoint 再次收到 HEARTBEAT 时恢复原 Session，不创建新的 DroneID，Fleet 数量不会翻倍。

## 6. MAVLink Inbound

| Message ID | Name | 当前职责 | 最终去向 |
|---:|---|---|---|
| 0 | HEARTBEAT | 发现、Online、模式、armed、路由元数据 | Discovery、DroneSession、FlightData |
| 1 | SYS_STATUS | `battery_remaining` | FlightData、DSP battery、UE Battery |
| 24 | GPS_RAW_INT | 纬度、经度、AMSL、Fix、卫星数、GPSValid | FlightData、DSP、UE/Cesium |
| 30 | ATTITUDE | Roll/Pitch/Yaw | FlightData；当前未送入 UE Fleet |
| 33 | GLOBAL_POSITION_INT | RelativeAltitude、vx/vy 融合 GroundSpeed | FlightData；UE 当前使用相对高度 |
| 77 | COMMAND_ACK | MAV_CMD 执行结果 | CommandDispatcher、DSP Command Result、UE |
| 253 | STATUSTEXT | 当前未消费 | Production Ingress 忽略 |

关键边界：

- `GPS_RAW_INT #24` 是绝对经纬度和 AMSL 的权威来源。
- `GLOBAL_POSITION_INT #33` 只更新相对高度和融合运动状态，不得覆盖绝对经纬高。
- 海拔 `0m` 可以是合法海平面高度，不能单独判定 GPS 无效。

## 7. MAVLink Outbound

| Message ID | Name | 当前用途 |
|---:|---|---|
| 0 | HEARTBEAT | Console 作为 GCS 周期发送，SYSID=255 |
| 11 | SET_MODE | GUIDED、STABILIZE、AUTO、LOITER |
| 66 | REQUEST_DATA_STREAM | Session 每次 Online 阶段请求 10Hz 数据流 |
| 76 | COMMAND_LONG | 承载 ARM/DISARM/TAKEOFF/LAND/RTL 的 MAV_CMD |

UE 不直接发送 MAVLink。UE 发送 DSP-GCS 领域命令，由 Console 转换并发往发现到的 Drone Endpoint。

## 8. MAV_CMD

| Command ID | Name | 地面站动作 | 承载 Message |
|---:|---|---|---|
| 400 | MAV_CMD_COMPONENT_ARM_DISARM | ARM / DISARM | #76 COMMAND_LONG |
| 22 | MAV_CMD_NAV_TAKEOFF | TAKEOFF | #76 COMMAND_LONG |
| 21 | MAV_CMD_NAV_LAND | LAND；当前 STOP/EStop 也映射为受控 LAND | #76 COMMAND_LONG |
| 20 | MAV_CMD_NAV_RETURN_TO_LAUNCH | RTL | #76 COMMAND_LONG |

GUIDED、STABILIZE、AUTO、LOITER 属于 `#11 SET_MODE`，不是 MAV_CMD。HoverTest 是项目应用级工作流，也不是单独 MAV_CMD。

## 9. Command + ACK 链

```text
UE UI
-> DSPGCSProductionCommandProviderSubsystem
-> DSP Command Request
-> Console DSPGCSProductionCommandReceiver
-> CommandManager
-> DroneSession
-> CommandDispatcher
-> MavlinkCommand
-> Remote Endpoint
```

ACK 返回链：

```text
MAVLink #77 COMMAND_ACK
-> MavlinkReadOnlyIngress
-> 原 Endpoint DroneSession
-> CommandDispatcher
-> DSP Command Result
-> UE ProductionCommandProvider
```

ACK 必须匹配当前 Session、目标 MAVLink sysid 和期望 MAV_CMD。这里 SYSID 只参与协议应答关联，不承担 Fleet 身份职责。

## 10. Console -> UE 遥测

```text
DroneManager 稳定 SessionOrder
-> 全量 Fleet Snapshot
-> 按安全 UDP 大小分片
-> Console DSP Transport
-> UE ProductionTelemetryProvider
-> SessionID + SnapshotID + FleetRevision 重组
-> 完整 Snapshot
-> GCSServerAdapter GameThread Flush
-> FleetManager 原子应用
```

1000 架 Snapshot 不能假定由单个 UDP Datagram 承载。UE 在全部分片收齐前不得提交部分 Fleet；不同 Session/Snapshot/Revision 的分片不能混合。

## 11. Unreal Fleet

当前 Production 数据链：

```text
DSPGCSProductionTelemetryProviderSubsystem
-> GCSServerAdapterSubsystem
-> UFleetManagerSubsystem
-> UI / DroneFleetVisualizationActor
```

`FleetManager` 是 UE Fleet 状态权威源：

- `DronesByID` 保存按稳定 DroneID 索引的数据。
- `DroneOrder` 保存稳定顺序。
- `CurrentSelectedDroneID` 保存详情选择。
- 网络线程不直接修改 Fleet/UI；Adapter 批量切换到 GameThread。

## 12. UE 选择模型

- `CurrentSelectedDroneID`：只负责详情查看和当前行高亮，由 FleetManager 保存。
- `SelectedCommandDroneIDs`：只负责多机命令目标，由 GCSUIBridge 保存。

两者相互独立：勾选命令目标不能覆盖详情选择，点击详情行也不能清空命令目标集合。

## 13. GPS -> Cesium

```text
有效 WGS84 Latitude/Longitude/AltitudeAMSL
-> ACesiumGeoreference
-> UE Absolute World Position
```

- 禁止用手写经纬度平面公式替代 Cesium。
- GPS 无效时使用 `FVector::ZeroVector`。
- 不使用随机位置、矩阵排列或最后已知位置替代无效 GPS。
- `AltitudeAMSL=0` 本身不是 GPS 无效条件。

## 14. Fleet -> Niagara

```text
Fleet DroneOrder
-> DroneFleetVisualizationActor
-> User.DronePositions
-> NS_DroneFleet
```

当前架构固定为：

- 一个 `ADroneFleetVisualizationActor`
- 一个 `UNiagaraComponent`
- 一个 `NS_DroneFleet`
- N 架 Drone 对应 N 个稳定粒子索引

`DroneOrder[Index] -> ParticleIndex -> DroneID` 必须稳定。Offline 保留粒子和索引。禁止 per-drone Actor、per-drone NiagaraComponent 和 per-drone Tick。

Future / Planned：淡紫色 Offline 表现和粒子 DisplayID 数字显示尚未确认正式实现，不能当作当前能力。

## 15. Production / Mock / Loopback / Legacy

| 分类 | 当前模块 | 状态 |
|---|---|---|
| Production | Console MAVLink/Discovery/Session/Command/DSP；UE Production Provider、Adapter、Fleet、UI、Cesium/Niagara | 当前真实运行链 |
| Production / Loopback | Console/UE `DSPGCSLoopbackTransport` | 当前 Production 同机 DSP 通道正在使用 |
| Mock | `MockDroneDataSubsystem` | `UseMockData=False`，当前关闭 |
| Acceptance / Loopback Test | `DSPGCSLoopbackProviderSubsystem` | `[DSPGCS.Transport] Enabled=False`，当前关闭 |
| Removed in Cleanup Phase 3A | Console GCS1 `CommandProtocol/UdpCommandServer/DroneControlService` | Console 端旧 10006 文本服务已移除；当前唯一 Production 命令入口为 `DSPGCSProductionCommandReceiver` |
| Removed in Cleanup Phase 3B | UE `DroneCommandSubsystem/GCSCommandTypes` | 旧 10006/GCS1 命令客户端与 `[DroneGCS.Network]` 配置已移除；当前唯一 UE Production 命令入口为 `DSPGCSProductionCommandProviderSubsystem` |
| Removed in Cleanup Phase 3C | Console Legacy Single Session / fixed Target / per-Session Socket | `target_ip/target_port/local_port`、`RunLegacySingleSession` 与独立 `UdpClient` Socket 已移除；Session 只能由 `DroneManager` 按实际 Remote Endpoint 建立 |
| Legacy Single-Drone Visualization | `DroneActor` | 不参与 Fleet/Niagara；当前地图/Blueprint 仍引用，Runtime 可能 Tick |
| Test Only | Console test projects、UE Automation Tests | 不进入 Production |

## 16. 当前 IP 与端口

| Value | 当前真实用途 |
|---|---|
| `192.168.1.254:10001` | Console MAVLink UDP Production bind；同一 Socket 向 Drone Endpoint 回发 |
| `127.0.0.1:12001` | Console DSP-GCS bind；接收 UE command，发送端身份 |
| `127.0.0.1:12002` | UE DSP-GCS bind；接收 Console telemetry/result |
| `10005` | `gcs-config.ini.example` 示例端口，不是当前 Production |
| `10006` | 旧 GCS1 文本命令链端口；Console 服务与 UE 客户端已分别在 Cleanup Phase 3A/3B 移除，当前 Runtime 不再使用该端口 |

`127.0.0.1` 当前也是 Production DSP 同机通信地址，不能统一标为测试地址。

## 17. 配置来源与优先级

Production 保留 Console 与 Unreal 两份部署配置，因为两个进程有不同的启动系统和部署边界；当前不强制 UE 直接读取 Console 文件。两端使用相同的 Endpoint 角色名称，并在启动时打印最终生效值，避免通过 Bind/Remote 反向关系猜测端口。

### 17.1 Console Deployment

`ConsoleGCS/gcs-config.ini` 是 Console Production 真值来源：

| 配置 | 当前字段 | 代码 Fallback / 缺失行为 |
|---|---|---|
| MAVLink 本地监听 | `mavlink_bind_ip`、`mavlink_bind_port` | Production 启用时必须提供；当前为 `192.168.1.254:10001` |
| Session 超时 | `heartbeat_timeout_ms` | 必须提供；当前为 `3000ms` |
| 真实命令总开关 | `real_command_enabled` | 缺失时安全默认 `false` |
| DSP 开关与配置文件 | `dspgcs_*_enabled`、`dspgcs_transport_config` | DSP 配置路径默认 `config/DSPGCSTransport.ini` |
| Snapshot 周期 | `dspgcs_telemetry_interval_ms` | 默认 `250ms` |
| MAVLink 接收缓冲 | `[performance].mavlink_receive_buffer_bytes` | 默认 `4MiB` |
| 单轮最大 Drain | `[performance].max_packets_per_drain` | 默认 `4096` |
| Snapshot Datagram 上限 | `[performance].snapshot_datagram_bytes` | 默认 `61440` 字节 |
| 单 Chunk Drone 上限 | `[performance].snapshot_items_per_chunk` | 默认 `512` |

Cleanup Phase 3C 已删除 `target_ip`、`target_port`、`local_ip`、`local_port` 及其解析、验证和示例值。Production 不接受启动时配置的单架无人机目标；每架无人机的 Remote IP 与 UDP Source Port 只来自实际收到的 MAVLink Datagram。

Production Session 与命令路由固定为：

```text
MAVLink Datagram Remote Endpoint
-> DroneManager endpoint index
-> DroneSession(remoteIp, remotePort)
-> HEARTBEAT 中的 per-Session target_system/target_component
-> CommandDispatcher / MavlinkCommand
-> shared MavlinkUdpReceiver Socket
-> 同一 Drone Remote Endpoint
```

`SYSID/COMPID` 是每个 Session 的 MAVLink 路由元数据，不是 Fleet Identity；相同 IP、不同 Source Port、相同 SYSID 仍创建不同 Session。

### 17.2 DSP-GCS Endpoint 角色

`gcs-config.ini` 的 `dspgcs_transport_config` 指向 `ConsoleGCS/config/DSPGCSTransport.ini`。该文件使用固定角色名：

- `ConsoleAddress` / `ConsolePort`：Console 本地 Bind Endpoint，当前 `127.0.0.1:12001`。
- `UnrealAddress` / `UnrealPort`：UE 本地 Bind Endpoint，当前 `127.0.0.1:12002`。
- `TransportType`：当前 `Loopback`。
- `ReceiveBufferBytes` / `ReceiverPollMs`：Console DSP Socket 性能参数，当前 `4MiB / 2ms`。

旧 `BindAddress/BindPort/RemoteAddress/RemotePort` 仍可由 Console Loader 读取，仅用于配置兼容；新部署应使用角色字段。

### 17.3 Unreal Deployment

`UnrealGCS/Config/DefaultGame.ini` 的 `[DSPGCS.ProductionTelemetry]` 使用与 Console 相同的 `ConsoleAddress/ConsolePort/UnrealAddress/UnrealPort` 角色字段。UE 运行时将 Unreal Endpoint 作为本地 Bind，将 Console Endpoint 作为 Remote Target。

UE Production 配置优先级：

1. `DefaultGame.ini` 的角色字段。
2. 旧 `BindAddress/BindPort/RemoteAddress/RemotePort` 兼容字段。
3. `Shared/DSPGCSTransport/DSPGCSTransportConfig.h` 中的代码 Fallback。

`ReceiveBufferBytes` 和 `ReceiverPollMs` 当前为 `4MiB / 20ms`。`TelemetryTimeoutMs=2000` 只判断 UE 是否持续收到完整、有效并已提交的 Production Snapshot，不重新实现 MAVLink Heartbeat timeout。Console 与 UE 启动日志都会打印 Transport、ConsoleEndpoint、UnrealEndpoint、缓冲参数和遥测超时，便于人工核对两端是否漂移。

UE 的 DSP-GCS 遥测链路状态位于 `UDSPGCSProductionTelemetryProviderSubsystem`：首个有效 Snapshot 提交后为 `Connected`，超过 `TelemetryTimeoutMs` 未收到下一份有效 Snapshot 后为 `Disconnected`。断链只改变 UI 派生显示和命令可用性，不清空 Fleet、不修改原始 `Drone.bOnline`、不改变 `DroneOrder` 或 ParticleIndex。UI 显示规则为：Server Disconnected → `Link Lost`；否则使用 Console Snapshot 中的 `Drone.bOnline` 显示 `Online / Offline`。

### 17.4 Safety 与部署配置边界

Safety Policy 继续要求真实命令开关、Production MAVLink、DSP Command 三个门同时开启，并拒绝 `0.0.0.0` 这种通配监听作为真实命令 Endpoint。SafetyConfig 不再保存另一套 `192.168.1.254:10001` Production 真值；实际 Endpoint 只来自 `AppConfig`。

高度限制仍由 `gcs-config.ini [max_flight]` 加载到 Safety Policy。固定单目标 `MAVLINK_TARGET_IP/MAVLINK_TARGET_PORT` 已在 Cleanup Phase 3C 删除；Safety Policy 不拥有无人机 Endpoint 真值。

### 17.5 不配置化的常量

以下属于协议或防御性 Safety Limit，不是部署调优项：

- DSP-GCS `FrameMagic`、Schema、字段布局和编码规则。
- `MaxFrameBytes`、`MaxStringBytes`、`MaxCollectionItems`。
- `MaxSnapshotChunks=1024`，用于限制异常分片导致的内存占用。
- UDP 单 Datagram 物理上限 `65507` 字节；配置的 Snapshot Datagram 必须低于该上限。
- UE `MaxPendingDroneDeltas=20000`，用于限制网络线程积压。

### 17.6 特殊端口

- `10005` 只在 `gcs-config.ini.example` 中作为示例值；不会进入当前 Production 启动日志。
- `10006` 曾属于 `[DroneGCS.Network]` Legacy GCS1 文本命令客户端；Console 服务与 UE 客户端已分别在 Cleanup Phase 3A/3B 移除，当前 Runtime 源码和配置均不再使用该 UDP 端口。
- `127.0.0.1` 是当前 Console 与 UE 同机运行的 Production DSP 地址，不是 Test Address。

Future / Planned：如果 DSP-GCS 需要跨机器部署，可新增共享 Deployment Profile 生成两端配置；在此之前不让 UE 直接依赖 Console 私有配置文件。

## 18. Future / Planned 整理方向

未来可按以下模块域整理，但本轮没有移动任何文件：

```text
Console: App / Config / Network / Mavlink / Fleet / Command / DSP / UI / Tests
Unreal:  Core / Fleet / DSP / Command / UI / Visualization / Simulation / Acceptance / Legacy / Tests
```

未来 Cleanup 必须分阶段编译提交，且不得破坏 Endpoint 身份、Offline Session 保留、#24/#33 职责、DSP Snapshot 原子提交和 DroneOrder/ParticleIndex 稳定映射。

【已知规模基线：10000 Drone】

测试结果：

- Console Total Session：10000
- Console稳定Online：约2900~3200
- 其余Session因Heartbeat timeout进入Offline
- Unreal Fleet：10000
- Niagara：可以实际生成万级粒子

结论：

10K身份发现、Session保留、DSP Fleet同步和UE Fleet容量已经初步跑通。

当前10K主要未解决问题：
Console在万机持续MAVLink流量下无法维持全部Session Online。

注意：
当前1000架已经稳定通过，因此不要为了10K实验修改当前1000架稳定参数。

后续单独建立：
10K Scaling / Performance Phase

届时通过测量：
- UDP Rx packets/s
- Kernel drop
- MAVLink parse/s
- Heartbeat Rx/s
- Session refresh/s
- Timeout count/s
- DSP snapshot build time
- UE apply time
- Niagara update time

再决定优化位置。
