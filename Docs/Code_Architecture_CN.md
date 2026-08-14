# DroneGCS 当前代码架构

Status: CURRENT
Authority: 当前 Production 运行链与日常开发主文档
Last Updated: 2026-08-14
Applies To: 当前仓库源码
Supersedes: 本文件 2026-08-14 之前的实现描述

## 1. 文档职责

本文记录当前代码中已经存在的实现。未来功能只在单独的 `PLANNED` 章节引用，不与当前事实混写。长期约束见 [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)。专项文档：

- [Network_Architecture_CN.md](Network_Architecture_CN.md)
- [Visualization_Architecture_CN.md](Visualization_Architecture_CN.md)
- [UI_Architecture_CN.md](UI_Architecture_CN.md)
- [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)
- [Test_Architecture_CN.md](Test_Architecture_CN.md)

## 2. 系统总览

```text
ArduPilot/APM Drone Endpoints
    <-> MAVLink UDP
ConsoleGCS
    <-> DSP-GCS localhost UDP
UnrealGCS
    -> GCSServerAdapter
    -> FleetManager
       |-> UMG UI
       `-> Cesium -> Niagara
```

ConsoleGCS 是 MAVLink、Remote Endpoint Session 和真实命令执行 Authority。UnrealGCS 是操作、Fleet 消费、Route Preview 和地理可视化前端。

## 3. Unreal 启动与主 UI

Module 启动在 PIE/Game World 初始化后延迟创建主 UI：

```text
FDroneShowPlatform1Module::HandleWorldInitialized
-> CreatePrototypeUI
-> ConfigureGroundStationInput
-> Load /Game/UI/WBP_GCS_Main_V2
-> reuse existing viewport instance OR CreateWidget
-> AddToViewport(100), viewport fill
-> UGCSUIBridgeSubsystem::BindMainWidget
```

`BindMainWidget` 查找并保存：

- `CHILD_TopBar`
- `CHILD_LeftToolbar`
- `CHILD_FleetPanel`
- `CHILD_DroneDetail`
- `CHILD_RightToolbar`
- `CHILD_RightPanel`
- `CHILD_Playback`
- authored `SelectionRectangle`

首次绑定将 FleetPanel、DroneDetail、RightPanel 设为 Collapsed；后续按钮基于当前 Visibility 独立切换。UI Designer 继续拥有内部几何和视觉，C++只处理生命周期、绑定和状态派生。

## 4. Console 启动

```text
main
-> ConsoleGCSApplication::Run
-> AppConfig::Load(gcs-config.ini)
-> RunProductionMavlink
-> MavlinkUdpReceiver::Start
-> DroneManager / CommandManager
-> MavlinkDiscoveryService / MavlinkReadOnlyIngress
-> DSPGCSProductionTelemetryProvider / CommandReceiver
-> single service loop
```

主循环：

1. 有界 drain MAVLink datagrams。
2. `MavlinkReadOnlyIngress` 只解码一次。
3. Discovery/Session tick。
4. DSP command queue processing。
5. 周期发布全量 Fleet snapshot。
6. 更新 Console diagnostics。
7. `Sleep(10)`。

## 5. Fleet Identity 与发现

```text
MavlinkUdpReceiver
-> MavlinkReadOnlyIngress
-> valid non-GCS HEARTBEAT #0
-> MavlinkDiscoveryService
-> DroneManager::RegisterOrRefreshDrone
-> DroneSession
```

Identity：

```text
Remote Endpoint = Remote IP + UDP Source Port
```

- HEARTBEAT 之前的非发现消息不能创建 Session。
- 同 endpoint 恢复原 Session/DroneID。
- Offline 不删除 Session、SessionOrder 或最后快照。
- SYSID/COMPID 仅用于 MAVLink target/ACK validation。
- 当前 DroneID 是 Console 进程内稳定分配值，未提供跨进程重启持久化保证。

## 6. MAVLink Inbound

| ID | Message | 当前职责 |
|---:|---|---|
| 0 | HEARTBEAT | 发现、在线、模式、armed、target metadata |
| 1 | SYS_STATUS | battery remaining |
| 24 | GPS_RAW_INT | 纬度、经度、AMSL、Fix、卫星、GPSValid |
| 30 | ATTITUDE | Roll/Pitch/Yaw，当前不进入 UE Fleet |
| 33 | GLOBAL_POSITION_INT | RelativeAltitude、速度融合 |
| 77 | COMMAND_ACK | 当前 endpoint Session 的短命令 ACK |
| 253 | STATUSTEXT | 当前忽略 |

Datagram 只由 `MavlinkReadOnlyIngress` 解码。HEARTBEAT 交给 Discovery；已知 endpoint 的 COMMAND_ACK 交给 `DroneSession::ApplyDiscoveredCommandMessage`；其余尝试作为 telemetry 应用。

当前未实现 `FILE_TRANSFER_PROTOCOL #110` ACK/NAK 路由。

## 7. MAVLink Outbound 与 Command

| ID | Message | 当前用途 |
|---:|---|---|
| 0 | HEARTBEAT | Console 作为 GCS 周期发送 |
| 11 | SET_MODE | GUIDED/STABILIZE/AUTO/LOITER |
| 66 | REQUEST_DATA_STREAM | Session online 阶段请求数据流 |
| 76 | COMMAND_LONG | ARM/DISARM/TAKEOFF/LAND/RTL |
| 186 | LED_CONTROL | Selected subset realtime RGB，fire-and-forget |

Production command chain：

```text
GCSUIBridgeSubsystem
-> DSPGCSProductionCommandProviderSubsystem
-> DSP-GCS CommandRequest
-> DSPGCSProductionCommandReceiver
-> CommandManager::Submit
-> DroneManager::FindDrone(DroneID)
-> DroneSession::SubmitCommand
-> CommandDispatcher
-> MavlinkCommand
-> endpoint-bound UdpClient adapter
-> shared MavlinkUdpReceiver::SendTo
```

`CommandDispatcher` 当前为短命令队列：最大32项、每Session一个pending、ACK timeout 3000ms、最多重发一次。`SetLedColor` 没有 COMMAND_ACK，发送成功后返回 `Sent`。

LED实际使用：

```text
LED_CONTROL #186
target_system=0, target_component=0
instance=255, pattern=255, custom_len=3
custom[0..2]=RGB, custom[3..23]=0
```

0/0 是 legacy device compatibility metadata，不参与 Fleet lookup。

## 8. DSP-GCS

当前 wire constants：

- ProtocolMajor = 1
- ProtocolMinor = 0
- SchemaVersion = 3
- MaxFrameBytes = 256 KiB
- MaxCollectionItems = 20000

当前 MessageType：

- `TelemetryBatch = 1`
- `CommandRequest = 2`
- `CommandResultBatch = 3`

当前 CommandType：

- Arm
- Disarm
- Takeoff
- Land
- RTL
- SetMode
- HoverTest
- SetLedColor

正式 transport：

```text
Console bind 127.0.0.1:12001
UE bind      127.0.0.1:12002
```

1000架 telemetry snapshot 允许分片。UE Provider 在 worker 完成 decode/reassembly/DTO conversion，只有完整 snapshot 才交给 `GCSServerAdapterSubsystem` 在 GameThread 原子 Apply。

## 9. UE Telemetry 与 Fleet

```text
DSPGCSProductionTelemetryProviderSubsystem
-> GCSServerAdapterSubsystem
-> UFleetManagerSubsystem
-> UI / ADroneFleetVisualizationActor
```

`FleetManagerSubsystem` 拥有：

- `DronesByID`
- stable `DroneOrder`
- `CurrentSelectedDroneID`（详情选择）
- FleetRevision
- session-scoped `FFleetGeoOrigin`

选择语义：

- `CurrentSelectedDroneID`：单机详情。
- `UGCSUIBridgeSubsystem::SelectedCommandDroneIDs`：多机命令目标。

两者独立，不得相互覆盖。

## 10. GPS 与 Cesium

```text
valid WGS84 Latitude/Longitude/AltitudeAMSL
-> ACesiumGeoreference
-> UE Absolute World Position
```

- Live GPS 无效时不生成假位置。
- `AltitudeAMSL=0` 可以合法。
- Route Preview 使用 Local ENU + Show Origin，经 Cesium ESU transform 映射到 UE World。
- Cesium/UE World 是渲染空间，不是 Waypoint Generator 的业务 Authority。

## 11. Visualization

当前唯一结构：

```text
one ADroneFleetVisualizationActor
-> one UNiagaraComponent
-> one NS_DroneFleet
-> N particles
```

代码数组和 User Params：

- `ParticleDroneIDs`
- `DronePositions` -> `User.DronePositions`
- `DroneColors` -> `User.DroneColors`
- `DroneDisplayIDs` -> `User.DroneDisplayIDs`
- `DroneSelectedMasks` -> `User.DroneSelectedMasks`
- `User.ShowDroneLabels`

`DisplayID = stable particle index + 1`。`SelectedMask` 来源仍是 `SelectedCommandDroneIDs`。颜色 Preview 通过 `SetNiagaraArrayColor` 提交完整数组。

`EGCSVisualizationMode` 当前有：

- Live
- RoutePreview

RoutePreview 进入时接管同一批数组；Fleet telemetry继续进入FleetManager。Exit后重新提交最新Live snapshot。Live + Route Overlay尚未实现。

详见 [Visualization_Architecture_CN.md](Visualization_Architecture_CN.md)。

## 12. TopDown、Marquee 与标签

当前已实现：

- `BTN_ZCamera`显式Toggle。
- 唯一可复用 runtime `ACameraActor`。
- Orthographic Z TopDown。
- Fleet XY bounds + viewport aspect自动fit。
- 保存/恢复 previous ViewTarget。
- Wheel修改OrthoWidth的center zoom。
- MMB camera-plane pan，速度随OrthoWidth变化。
- LMB Marquee独立于TopDown输入。
- Marquee使用viewport-local mouse与`ProjectWorldLocationToWidgetPosition(..., false)`同一坐标空间。
- Drone ID仅在TopDown通过`User.ShowDroneLabels`显示。

Marquee结果继续写入唯一 `SelectedCommandDroneIDs`，并同步 Fleet勾选、Selected Count、SelectedMask和Orange ID/rim。

## 13. UI Architecture

Main asset：`/Game/UI/WBP_GCS_Main_V2`。

主要 authored widgets：

- `WBP_GCS_TopBar`
- `WBP_GCS_LeftToolbar`
- `WBP_GCS_FleetPanel`
- `WBP_GCS_DroneDetail`
- `WBP_GCS_RightToolbar`
- `WBP_GCS_RightPanel`
- `WBP_GCS_Playback`
- `WBP_GCS_RouteImport`
- `WBP_GCS_PageMap`
- `WBP_GCS_PageSettings`
- `WBP_GCS_PageLogs`

`BTN_Fleet`和`BTN_Control`分别独立Toggle FleetPanel/DroneDetail。Map/Settings/Logs是独立页面；二次点击当前页关闭并回到主场景。详见 [UI_Architecture_CN.md](UI_Architecture_CN.md)。

## 14. Route Preview

### 14.1 Native NPZ

`FRoutePreviewNpzLoader`：

- Store/Deflate
- NPY v1/v2/v3
- little-endian f2/f4/f8
- `position`和`color`必需
- optional `range`
- rank/shape/count/finite validation
- 无Python Runtime

### 14.2 RouteData

```text
Index = FrameIndex * DroneCount + RouteSlotIndex
PositionsENU: TArray<FVector3f>
Colors:       TArray<FFloat16Color>
```

RouteSlot为0-based preview identity，绝不创建Fleet Drone或DroneSession。

9100 frames × 1000 drones最终存储约173.57 MiB。10000-drone route store仍是未来chunked/sharded问题。

### 14.3 Playback

状态：Stopped、Playing、Paused、Completed。

- SampleRate默认为0/unset。
- Play要求有效SampleRate。
- 单一FTSTicker仅Playing时工作。
- Exact frame sampling：`floor(CurrentTime * SampleRateHz)`。
- 当前Frame不变时不重复批量提交。
- Stop回Frame 0。
- Exit恢复最新Live visualization。

### 14.4 UI与Developer Commands

Playback已绑定Play/Pause/Stop/Previous/Next/Slider/Time/Frame。RouteImport popup使用单实例，Windows runtime `IFileOpenDialog`只选`.npz`；Hint Text不作为真实值。用户提交显式SampleRate和Origin后Commit、进入Preview、显示Frame 0。

Developer Commands继续存在：

- `RoutePreview.Load`
- `RoutePreview.Origin`
- `RoutePreview.Rate`
- `RoutePreview.Frame`
- `RoutePreview.Play`
- `RoutePreview.Pause`
- `RoutePreview.Stop`
- `RoutePreview.Status`
- `RoutePreview.Exit`
- `RoutePreview.Unload`

## 15. Production / Developer / Test

| Category | Current Components | State |
|---|---|---|
| Production | Console MAVLink/Session/Command/DSP；UE Production Providers、Adapter、Fleet、UI、Cesium/Niagara、RoutePreview | 正式链 |
| Developer Mock | `Developer/Mock/MockDroneDataSubsystem` | 默认关闭 |
| Developer Acceptance | `Developer/Acceptance/DSPGCSLoopbackProviderSubsystem` + Console acceptance exe | 默认关闭 |
| Regression | ConsoleGCSTests + UE `Private/Tests` | 不进入Production |

Production初始化失败不允许自动切换Mock。

## 16. 当前配置边界

- `ConsoleGCS/gcs-config.ini`：Console deployment。
- `ConsoleGCS/config/DSPGCSTransport.ini`：Console DSP endpoint。
- `UnrealGCS/Config/DefaultGame.ini`：UE Production/Developer开关。
- Protocol constants：`Shared/DSPGCSProtocol`。
- Safety constants/config：Console command safety层。

Production Drone endpoint只来自实际MAVLink datagram，不存在固定目标配置。

## 17. 当前没有实现

- Live + Route Overlay
- RouteAssignment Authority
- Waypoint Generator/Validation/Manifest
- Route Upload DSP messages
- Console RouteUploadManager
- QGC WPL parser/Mission Binary Encoder
- MAVFTP client与FTP ACK/NAK路由
- Mission Ready verification
- Firmware capability model
- Backup Drone Replacement

这些内容见 [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)，不得从该文档反推为当前实现。

## 18. Developer Entry Map

| 目标 | 首要入口 | 第二入口 |
|---|---|---|
| Discovery/identity | `MavlinkDiscoveryService`、`DroneManager` | `MavlinkReadOnlyIngress` |
| Telemetry decode | `MavlinkParser`、`FlightData` | DSP Telemetry Provider |
| 新短命令 | UE/Shared/Console command mapping | `DroneSession`、`CommandDispatcher`、`MavlinkCommand` |
| UE Fleet | `FleetManagerSubsystem` | `GCSDataTypes` |
| UI行为 | `GCSUIBridgeSubsystem` | `GCSUIWidgetPresenter` |
| Visualization arrays | `DroneFleetVisualizationActor` | `NS_DroneFleet` asset |
| Route data/playback | `RoutePreviewSubsystem` | `RoutePreviewNpzLoader` |
| Network/DSP | `MavlinkUdpReceiver`、DSP transport | `MavlinkReadOnlyIngress` |

## 19. 当前事实标签

- Route Preview Import UI：IMPLEMENTED，人工验证PASS。
- Selected-drone LED_CONTROL：IMPLEMENTED，硬件验证PASS。
- Marquee coordinate alignment：IMPLEMENTED，人工验证PASS。
- BTN_ZCamera explicit toggle、wheel zoom、MMB pan：IMPLEMENTED。
- Live + Route Overlay：PLANNED / NEXT PHASE。
- Route Upload：PLANNED，当前无实现。
- Mission timing与custom firmware behavior：UNKNOWN。
