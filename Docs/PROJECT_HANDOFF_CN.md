# DroneGCS 项目交接速览

> Status: CURRENT HANDOFF
>
> Last Updated: 2026-08-14
>
> Authority: 快速接手入口；边界以 [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md) 为准，代码事实以 [Code_Architecture_CN.md](Code_Architecture_CN.md) 与当前 Production 源码为准。

## 1. 新会话阅读顺序

1. [README_CN.md](README_CN.md)：文档索引与 Source-of-Truth Matrix。
2. [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)：不可绕过的长期边界。
3. [Code_Architecture_CN.md](Code_Architecture_CN.md)：当前真实调用链。
4. [Development_Status_CN.md](Development_Status_CN.md)：已完成、当前下一步、延后项。
5. 按任务域阅读 UI、可视化、网络或 Route Upload 专题文档。

不要把 [History/](History/) 中的历史设计当作当前实现。

## 2. 当前稳定 Production 主链

```text
MAVLink UDP ingress
  -> MavlinkUdpReceiver (one shared socket)
  -> MavlinkReadOnlyIngress
  -> MavlinkDiscoveryService / DroneManager / DroneSession / DataManager
  -> DSP-GCS SchemaVersion 3
  -> UE Production telemetry provider / server adapter
  -> FleetManagerSubsystem
  -> one DroneFleetVisualizationActor
  -> one NS_DroneFleet
```

命令链：

```text
UE intent
  -> GCSUIBridgeSubsystem
  -> DSP-GCS Production CommandRequest
  -> Console DSPGCSProductionCommandReceiver
  -> CommandManager
  -> DroneSession
  -> CommandDispatcher
  -> MavlinkCommand
  -> shared UDP sender
  -> session Remote IP + UDP source port
```

Fleet 唯一身份是 `Remote IP + UDP Source Port`；MAVLink SYSID 只属于协议元数据，不得升级为 Fleet identity。

## 3. 当前已实现能力

- Console 多 Endpoint discovery、online/offline/recovery、telemetry 与命令分发。
- DSP-GCS SchemaVersion 3 telemetry/command/result 双向链。
- UE FleetManager、Cesium 地理转换、单 Niagara 多粒子 Fleet 可视化。
- Fleet 单选详情、多选命令目标、Marquee、Selected Count、Orange selection rim。
- 7 色 Preview 与真实 selected-subset `LED_CONTROL #186` 下发。
- BTN_ZCamera 显式切换的正交俯视、中心缩放、中键平移。
- Route Preview 原生 NPZ 加载、离线预览、静态帧、播放时钟、步进、Scrub、导入弹窗。
- Live / RoutePreview 可视化来源隔离，退出 Preview 后恢复最新 Live snapshot。

## 4. Route Preview 当前边界

已实现：

```text
NPZ position/color
  -> RoutePreviewSubsystem
  -> RouteSlotIndex (0-based, not DroneID)
  -> Local ENU + ShowOrigin
  -> Cesium -> UE World
  -> existing DronePositions / DroneColors arrays
  -> one Niagara fleet
```

未实现：RouteSlot 到真实 DroneID assignment、Waypoint 生成、Route Upload job、DSP route schema、Console MAVFTP backend、固件侧 mission consumption verification。

Route Preview 数据不得制造真实 Fleet session，也不得成为真实 command selection authority。

## 5. 当前下一阶段

Route Upload 仍处于设计/审计阶段。开始实现前先读：

- [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)
- [Audit_Legacy_BGC2_RouteUpload_CN.md](Audit_Legacy_BGC2_RouteUpload_CN.md)
- [Audit_DroneGCS_RouteUpload_Gap_CN.md](Audit_DroneGCS_RouteUpload_Gap_CN.md)

下一阶段必须先明确 RouteAssignment、artifact authority、DSP job lifecycle、Console upload backend 与 firmware contract，不能直接复制旧 BGC2 参数或绕过 ConsoleGCS。

## 6. 开发保护规则

- 先读真实调用链，再做最小改动。
- 不主动重构冻结边界。
- 不新增第二套 Fleet identity、selection authority、network stack 或 visualization system。
- 不使用 per-drone Actor/Component/Tick/Widget 承载大规模 Fleet。
- UE 不直接向无人机发送 MAVLink；真实设备命令必须通过 DSP-GCS 与 ConsoleGCS。
- Route Preview 与 Live telemetry 必须保持明确 mode/source authority。
- 未经固件证据确认的 Route Upload/mission consumption 行为必须标记为 `FIRMWARE UNKNOWN`。

## 7. 当前状态入口

详见 [Development_Status_CN.md](Development_Status_CN.md)。测试命令与测试资产边界详见 [Test_Architecture_CN.md](Test_Architecture_CN.md)。
