# Phase 0：Telemetry 行为

基线：`a95309d8b37ecb00c779d5c4264aacb86294d8ec`（master，2026-09-09）。本阶段不改变生产源码、协议、配置或资产。执行结果见 [测试基线](11_TEST_BASELINE.md)；静态结论不等于真机验证。

## 1. 生产链与线程

`MavlinkUdpReceiver → MavlinkEndpointIngress（每Endpoint parser state）→ DroneSession/MavlinkParser → DroneDataManager → DSPGCSProductionTelemetryProvider::ToTelemetryDTO → DSP → UE ProductionTelemetryProvider → ServerAdapter pending Snapshot/Delta → AsyncTask GameThread → Fleet → UI / Visualization Actor / Niagara`。

Console会话内的真实接收样本是生产遥测依据；DataManager互斥发布快照。UE Fleet 是 GameThread 上的消费状态 Authority，不是独立模拟遥测源。只有HEARTBEAT发现新Session；Endpoint=Remote IP+Source Port，SYSID不可替代。Console主循环每次最多drain4096包，默认快照250ms，单包60KiB、512项chunk；大快照必须完整重组，不能以部分覆盖Fleet。Hover worker只使用现有受锁快照/队列边界。

## 2. 字段覆盖与语义（STATIC_BASELINE）

| 字段 | Raw Telemetry / Console转换 | Derived State | UE投影与UI Formatting |
|---|---|---|---|
| Position | #24 GPS_RAW_INT lat/lon ×1e-7deg、alt mm→m；真实接收UTC样本时间 | 有效性与GPS sample identity | Fleet/插值使用真GPS；不是#33位置，不是NPZ坐标 |
| Altitude | #24 AMSL；#33 relative_alt mm→m | 两种高度基准独立 | relative与AMSL不得互换，UI单位字符串无权反写 |
| Velocity | #33 vx/vy cm/s→m/s | ground speed由水平分量计算 | 当前标准Fleet未传播完整速度向量，不能误称已完整数字孪生 |
| Attitude | #30 roll/pitch/yaw为rad | Console attitude valid | 当前标准DSP/Fleet无完整姿态投影；不得在重构中补字段 |
| Battery | #1剩余电量；#187详细电池值及有效性 | 无效值处理保持现有规则 | 标准投影无效值可落0；MDHK详细行单独保留valid，文本仅展示 |
| GPS | #24 fix/sat与绝对位置；#187有扩展字段 | fix/位置有效性 | MDHK格式化如GPS UTC；不能替代#24标准位置来源 |
| Satellite | #24 satellites_visible，#187扩展状态 | 原始计数/valid | 标准与MDHK详情/筛选投影各有覆盖，不假设全字段在Fleet |
| FlightMode | HEARTBEAT custom_mode | 模式枚举/名称映射 | 标准Mode展示 |
| ArmState | HEARTBEAT base_mode armed bit | Console命令门禁使用armed | DSP DTO携带armed，但当前UE标准Fleet转换未传入armed；不能据UI推定后端无门禁 |
| Heartbeat | 接收时间 | 默认3000ms判Session在线 | 和DSP链路健康分开 |
| ConnectionState | Endpoint last-seen / channel last receive | Drone Offline与UE LinkLost独立；UE链路超时2000ms | UI连接文字/颜色只投影，不改变命令选择 |
| RGB | #187 MDHK RGB | 标准LED状态映射 | 场景使用真机颜色；详情字符串与颜色材质刷新不是新遥测 |
| RSSI | 未获得真实来源时unknown | 保持当前fallback | 旧标准投影0不代表实测0dBm |

## 3. 动画、预览与生命周期

共享 Visualization Actor Tick 每机保留前/后真实GPS样本，默认300ms延迟，alpha clamp[0,1]插值。重复快照不能伪造新GPS样本时间。每机不新增Actor Tick；位置与颜色刷新保持独立。

NPZ的ENU、Origin、RouteSlot、Playback有独立来源；只有底部手动Playback启动，授时到0/执行舞步均不自动播放。不能将NPZ当真机轨迹。RouteGpsCapture通过Provider/GameThread接收GPS，采样/commissioning gate仍保留。

ServerAdapter积累状态后统一GameThread Flush；UI不直接收包。Provider断开、完整快照重组、MDHK缓存、异步弱引用与GameThread发布的现状由现有测试覆盖；deinitialize后的epoch取消与所有重入场景未穷尽证明。

## 4. 验证边界

Console pipeline/parser、DSP DTO round-trip、UE TelemetryLink/Chunk、TelemetryFilter、Visualization的本次结果见 [11](11_TEST_BASELINE.md)。仅通过对应断言者为TESTED_BASELINE。真实位置精度、延迟观感、卫星数一致性、灯具颜色、丢包规模属于FIELD_BASELINE_REQUIRED/人工。

源码：[Parser](../../ConsoleGCS/MavlinkParser.cpp)、[Console Provider](../../ConsoleGCS/DSPGCSProductionTelemetryProvider.cpp)、[UE Provider](../../UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSProductionTelemetryProviderSubsystem.cpp)、[Adapter](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSServerAdapterSubsystem.cpp)、[Fleet](../../UnrealGCS/Source/DroneShowPlatform1/Private/FleetManagerSubsystem.cpp)。
