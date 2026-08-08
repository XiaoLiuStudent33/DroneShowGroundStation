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
-> RunReadOnlyMavlink
```

当前 Production 使用多机 MAVLink 路径。`RunLegacySingleSession` 仍保留为 Compatibility，但不是当前 Production 入口。

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
| 253 | STATUSTEXT | 旧独立 Socket 路径日志 | Legacy；当前多机 Production 未使用 |

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
| Legacy / Compatibility | Console GCS1 `CommandProtocol/UdpCommandServer/DroneControlService` | 当前 Production 入口未使用 |
| Legacy / Compatibility | UE `DroneCommandSubsystem/GCSCommandTypes` | 当前 Production UI 未使用；可能有 Blueprint/反射依赖 |
| Legacy Single-Drone Visualization | `DroneActor` | 不参与 Fleet/Niagara；当前地图/Blueprint 仍引用，Runtime 可能 Tick |
| Test Only | Console test projects、UE Automation Tests | 不进入 Production |

## 16. 当前 IP 与端口

| Value | 当前真实用途 |
|---|---|
| `192.168.1.254:10001` | Console MAVLink UDP Production bind；同一 Socket 向 Drone Endpoint 回发 |
| `127.0.0.1:12001` | Console DSP-GCS bind；接收 UE command，发送端身份 |
| `127.0.0.1:12002` | UE DSP-GCS bind；接收 Console telemetry/result |
| `10005` | `gcs-config.ini.example` 示例端口，不是当前 Production |
| `10006` | 旧 GCS1 文本命令链端口；当前没有 Production listener |

`127.0.0.1` 当前也是 Production DSP 同机通信地址，不能统一标为测试地址。

## 17. 配置入口

- Console：`ConsoleGCS/gcs-config.ini`。
- Console DSP Endpoint：`gcs-config.ini` 的 `dspgcs_transport_config` 指向 `config/DSPGCSTransport.ini`。
- UE：`UnrealGCS/Config/DefaultGame.ini`。

当前配置值不得从本文复制回代码；本文只说明用途，运行时仍以实际配置加载结果为准。

## 18. Future / Planned 整理方向

未来可按以下模块域整理，但本轮没有移动任何文件：

```text
Console: App / Config / Network / Mavlink / Fleet / Command / DSP / UI / Tests
Unreal:  Core / Fleet / DSP / Command / UI / Visualization / Simulation / Acceptance / Legacy / Tests
```

未来 Cleanup 必须分阶段编译提交，且不得破坏 Endpoint 身份、Offline Session 保留、#24/#33 职责、DSP Snapshot 原子提交和 DroneOrder/ParticleIndex 稳定映射。
