# Production 职责审计（Architecture Freeze 基线）

> Status: ARCHIVED / HISTORICAL
>
> Current Authority: [Architecture_Freeze_CN.md](../Architecture_Freeze_CN.md) and [Code_Architecture_CN.md](../Code_Architecture_CN.md)
>
> Do Not Use As Current Implementation State
>
> Last Updated: 2026-08-14 (classification only)
>
> Authority: 保留 Cleanup/Freeze 时的职责审计证据；当前调用链以 [Code_Architecture_CN.md](../Code_Architecture_CN.md) 与 Production 源码为准，冻结边界以 [Architecture_Freeze_CN.md](../Architecture_Freeze_CN.md) 为准。
>
> Current status: [Development_Status_CN.md](../Development_Status_CN.md)

## 现行文档职责与审计问题

本文回答：这些类为什么存在、能否删除、`DataManager` 为什么不能随意合并，以及 `ServerAdapter` 的边界是什么。





最终审计基线：`master`，Cleanup Phase 3A~3H 已完成。本文描述 Architecture Freeze 时的代码快照；冻结规则与维护入口见 [`Architecture_Freeze_CN.md`](../Architecture_Freeze_CN.md)。

## 1. 统计口径与 Production 主链

- ConsoleGCS 自有 Production class/interface：20 个；Shared `DSPGCSTransportFactory` 另计 1 个。
- UnrealGCS 自有核心 Production class/interface：9 个；Shared Factory 另计 1 个。
- UE 的 `UMockDroneDataSubsystem` 与 `UDSPGCSLoopbackProviderSubsystem` 是开发/验收 Provider，不计入 Production 数量。
- `AppConfig`、`SafetyConfig`、DTO、Loader 等虽不是 class，也纳入职责矩阵。

```text
Console Telemetry
MavlinkUdpReceiver -> MavlinkReadOnlyIngress -> MavlinkDiscoveryService
-> DroneManager -> DroneSession -> DataManager -> DSPGCSProductionTelemetryProvider

Console Command
DSPGCSProductionCommandReceiver -> CommandManager -> DroneSession
-> CommandDispatcher -> MavlinkCommand -> UdpClient -> Remote Endpoint

UE Telemetry
FDSPGCSLoopbackTransport -> UDSPGCSProductionTelemetryProviderSubsystem
-> UGCSServerAdapterSubsystem -> UFleetManagerSubsystem -> UI / Visualization

UE Command
UMG -> UGCSUIBridgeSubsystem -> UDSPGCSProductionCommandProviderSubsystem
-> Production Telemetry Provider 已拥有的双向 DSP Transport -> Console
```

层存在的理由：Ingress 隔离 Endpoint parser state；Discovery 只接受有效 HEARTBEAT 并注册 Session；Manager 拥有 Session 集合；Session 绑定单架 Endpoint 和路由元数据；Dispatcher 管理 ACK/重试；MavlinkCommand 是编码边界。UE Provider 负责 Wire Decode/分片重组，Adapter 负责跨线程批量提交，FleetManager 负责 UE 权威 Fleet 状态。

## 2. Console 职责矩阵

风险：低表示局部纯逻辑；中表示公共 API、线程或生命周期；高表示身份、网络、ACK 或安全路径。

| Class/File | Current Responsibility | State Owned | Input | Output | Direct Callers | Direct Dependencies | Thread Context | Unique Responsibility? | Duplicate With | Merge Candidate? | Split Candidate? | Keep As-Is? | Risk | Recommended Action |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `ConsoleGCSApplication` | Composition root、启动校验、主循环、调度周期 | 局部组件和调度时间 | argv/config/UDP | 启动后的完整服务 | `main` | Config、MAVLink、Fleet、DSP、UI | 主线程 | 是 | 无规则重复 | 否 | 增长后可拆运行循环 | 当前保留 | 中 | R0；不要在 3F 大拆 |
| `AppConfig` | 读取、默认值、范围验证 | 启动配置 DTO | ini | 最终 Console 配置 | Application/tests | 文件解析 | 启动主线程 | 是 | Safety 只消费、不拥有部署值 | 否 | 否 | 是 | 中 | R0 |
| `SafetyConfig` | 真实命令总门和高度限制 | 进程级安全开关 | AppConfig/命令参数 | allow/reject | Application/Session/DSP Receiver | 无网络依赖 | 主线程启动后只读 | 是，Safety boundary | UE 本地校验不是权威安全门 | 否 | 否 | 是 | 高 | R0，保持独立 |
| `MavlinkUdpReceiver` | 单共享 UDP bind、收包、同 Socket 回发 | Socket/bind/buffer | UDP | 带 Remote Endpoint 的 Datagram / SendTo | Application、`IMavlinkDatagramSender` 使用方 | Winsock | 主循环接收；发送由 Session 调度 | 是，network boundary | 无 per-drone socket | 否 | 否 | 是 | 高 | R0 |
| `IMavlinkDatagramSender` | 隔离共享 Socket 发送能力 | 无 | endpoint+bytes | send result | UdpClient/tests | 无 | 调用者线程 | 是，test/network seam | 无 | 否 | 否 | 是 | 中 | R0 |
| `MavlinkReadOnlyIngress` | 每 Endpoint 解码、消息分类、Session 路由 | 每 Endpoint parser state | Datagram | Discovery/Telemetry/ACK 调用 | Application | Discovery、Manager、MAVLink C API | 主线程 | 是 | 无第二个 raw decode Owner | 否 | 否 | 是 | 高 | R2 Resolved：Datagram decode 只在 Ingress |
| `MavlinkDiscoveryService` | HEARTBEAT 过滤、Endpoint identity 生成、注册/刷新 | Session defaults | 已解码 HEARTBEAT | registration result | Ingress/Application/tests | DroneManager | 主线程 | 是（消息到注册边界） | 无 | 否 | 否 | 是 | 高 | R2 Resolved：删除 `IngestDatagram` 与 parser state |
| `MavlinkParser` | Inbound MAVLink 字段到 FlightData | 无 | message | FlightData fields | DroneSession | generated MAVLink | 主线程 | 是，protocol decode boundary | 无第二套 mode/GPS authority | 否 | 否 | 是 | 高 | R0 |
| `DroneManager` | Session 集合、稳定顺序、Endpoint index、Fleet 查询 | sessions/order/index/ID sequence | discovered identity/DroneID | Session、稳定列表 | Discovery、Application、Command、DSP | DroneSession | 主线程 | 是，Fleet owner | 无 | 否 | 否 | 是 | 高 | R2 Resolved：nullable 查询统一为 `FindDrone` |
| `DroneSession` | 单 Endpoint 聚合：遥测、Online、路由元数据、命令入口 | config/state/working data/dispatcher/hover | routed MAVLink、domain command | snapshot、dispatch/result | Manager、Ingress、CommandManager | Parser、DataManager、FlightControl、Dispatcher | 主线程；Hover worker 通过受控对象协作 | 是，per-drone aggregate | 与 FlightControl 有少量命令入口重叠 | 否 | 暂不拆 | 是 | 高 | R0；避免破坏聚合一致性 |
| `DataManager` | 单 Session 线程安全 FlightData 快照和 timeout 更新 | FlightData+mutex | parser/session update | consistent copy/online transition | Session、FlightControl | FlightData | 主线程+Hover worker | 是，thread-safety boundary | 无 | 否 | 否 | 是 | 高 | R0；不合入 Session |
| `FlightData` | Console 单机遥测领域数据 | 遥测字段 | Parser | Session/DSP/Control | Parser、DataManager、DSP | 标准类型 | 受 DataManager 保护 | 是，domain DTO | UE DTO 是进程边界后的模型 | 否 | 否 | 是 | 中 | R0 |
| `CommandManager` | 按 DroneID 的多目标业务命令和结果汇总 | pending/owners/completed/IDs | DSP/domain command | per-target result | DSP Receiver、Console input | DroneManager/Session | 主线程 | 是，fleet command owner | 无 | 否 | 否 | 是 | 高 | R0 |
| `CommandDispatcher` | 单 Session 队列、发送、重试、ACK 生命周期 | queue/pending/results/mutex | session actions/#77 | Mavlink send/result | DroneSession、FlightControl | MavlinkCommand/UdpClient | 主线程 Tick/ACK；Hover worker 可入队 | 是，ACK boundary | 无 | 否 | 否 | 是 | 高 | R0 |
| `MavlinkCommand` | Outbound #11/#76 编码发送 | 无 | SYSID/COMPID/params | datagram | Dispatcher/Session heartbeat helpers旁路之外 | generated MAVLink、UdpClient | 调用者线程 | 是，protocol encode boundary | 无 | 否 | 否 | 是 | 高 | R0；LOC 大但职责单一 |
| `UdpClient` | 将 per-Session Remote Endpoint 绑定到共享 sender | sender pointer/IP/port | bytes | `SendTo(endpoint)` | DroneSession/MavlinkCommand | IMavlinkDatagramSender | 调用者线程 | 是，endpoint-bound sender seam | 无第二套 Socket | 否 | 否 | 是 | 高 | R0；不要合入 Session |
| `FlightControl` | HoverTest 工作线程和动作确认流程 | worker/atomic mission state | Session command/FlightData | Dispatcher queue | DroneSession | Dispatcher/DataManager | 自有 Hover worker + caller | Hover workflow 独有 | 无正式命令旁路 | 否 | 可拆纯状态机但暂不做 | 是 | 高 | R2 Resolved：删除无调用 `Land/RTL/Hover`，`Takeoff` 收为 private |
| `HoverTest` | Hover 稳定性指标 | running/time/drift | GPS/altitude | acceptance metrics | DroneSession/FlightControl/UI | math/time | Hover worker/读取方 | 是 | 无 | 否 | 否 | 是 | 中 | R0 |
| `DSPGCSProductionTelemetryProvider` | 构建 Fleet DTO、Snapshot 分片、结果发送 | transport/sequence/revision/session/chunk limits | DroneManager/result | DSP frames | Application | Protocol/Transport factory | 主线程发布；transport 内部接收线程 | 是，Console DSP egress | 无 | 否 | 否 | 是 | 高 | R0 |
| `DSPGCSProductionCommandReceiver` | DSP decode、接收线程队列、主线程验证/提交/结果关联 | pending/dedup/active/gate | DSP frame | CommandManager/result DTO | Telemetry transport callback/Application | Protocol/CommandManager/Safety | transport thread enqueue；主线程 process | 是，trust/thread boundary | Session 安全校验是纵深防御 | 否 | 增长后可提取纯 mapping | 是 | 高 | R0 |
| `ConsoleDSPGCSLoopbackTransport` | Console 同机 Production UDP Transport | socket/thread/handler/state/config | DSP frames | send/receive callback | Factory/Provider | Winsock/Transport interface | 自有 receive worker | 是，transport boundary | UE 有平台不同实现 | 否 | 否 | 是 | 高 | R0；Loopback 不等于 Test |
| `DSPGCSTransportFactory`（Shared） | 按配置创建平台 Transport | 无 | config | interface instance | Console/UE Provider | platform implementation | 启动线程 | 是，construction boundary | 当前仅 Loopback 实现 | 否 | 否 | 是 | 中 | R0，为未来 LAN 保留 |
| `DSPGCSTransportConfigLoader` | Console DSP ini 解析/验证 | 无 | ini | shared config | Application/tests | AppConfig defaults | 启动主线程 | 是 | UE 使用自身 Config 系统是部署边界 | 否 | 否 | 是 | 中 | R0 |
| `ConsoleUI` | Console 摘要和日志渲染 | 静态控制台位置/日志行 | Fleet/log | terminal | Application/Session/Command | Win32 console | 主线程为主 | 是，presentation | 业务层直接日志耦合较多 | 否 | R2：未来事件化日志 | 当前保留 | 中 | 不在 3F 改调用链 |

## 3. Unreal 职责矩阵

| Class/File | Current Responsibility | State Owned | Input | Output | Direct Callers | Direct Dependencies | Thread Context | Unique Responsibility? | Duplicate With | Merge Candidate? | Split Candidate? | Keep As-Is? | Risk | Recommended Action |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `FDSPGCSLoopbackTransport` | UE 同机 Production UDP Transport | socket/receiver/handler/config/state | DSP UDP | frame callback/send | Factory/Providers | UE UDP socket API | `FUdpSocketReceiver` worker | 是，platform transport boundary | 无 | 否 | 否 | 是 | 高 | R0；名称不表示 Test |
| `DSPGCSTransportFactory`（Shared） | 构造 Transport interface | 无 | config | transport | UE Providers | FDSPGCSLoopbackTransport | 启动 GameThread | 是 | 无 | 否 | 否 | 是 | 中 | R0 |
| `UDSPGCSProductionTelemetryProviderSubsystem` | DSP envelope decode、Snapshot 分片重组、DTO 转换、Link Health、共享双向 Channel | transport/assembly/link state/handler | transport frame/config | internal snapshot/command result | UE subsystem lifecycle、CommandProvider | Protocol/Factory/Adapter | receive worker decode/DTO；link ticker GameThread | 是，wire/assembly owner | 无 | 否 | 可提取纯 assembler，但非必要 | 是 | 高 | R2 Resolved：不再自行切 GameThread |
| `UDSPGCSProductionCommandProviderSubsystem` | UE command DTO、发送、pending/result UI event | pending/sequence/mode/result | UI command/result payload | DSP request/UI result event | UIBridge | Telemetry Provider/Protocol | GameThread；result decode 后切 GameThread | 是，UE command boundary | Console 校验属于信任边界，不重复 | 否 | 否 | 是 | 高 | R0 |
| `IGCSDataProvider` | 协议无关 Provider 生命周期接口 | 无 | Adapter | Provider start/stop | Adapter/Providers | 无 | lifecycle thread | 是，provider seam | 无 | 否 | 否 | 是 | 中 | R0 |
| `UGCSServerAdapterSubsystem` | Provider 注册、Header 校验、跨线程有界合并、单次 GameThread flush | providers/pending snapshot/deltas/mutex | internal snapshot/delta | Fleet atomic apply | Providers | FleetManager/GCSDataTypes | 任意生产线程入；唯一 GameThread flush | 是，thread/lifecycle/application port | 无 | 否 | 否 | 是 | 高 | R2 Resolved：唯一 Network/Worker 到 GameThread 边界 |
| `UFleetManagerSubsystem` | UE Fleet 权威状态、稳定 order、详情选择、GeoOrigin、revision | map/order/selection/origin/session | snapshot/delta | queries/events | Adapter/UI/Viz | DroneData/GCSDataTypes | GameThread only mutation | 是，Fleet owner | 无 | 否 | 否 | 是 | 高 | R0 |
| `UGCSUIBridgeSubsystem` | UI 生命周期、页面导航、Fleet/Selection 事件和命令提交 | widget refs/page/selection/paging/UI cache | Fleet/events/buttons | UMG updates/commands | Main Widget/UE lifecycle | Fleet/CommandProvider/TelemetryProvider/RowView/Presenter | GameThread | 是，UI coordination owner | 无 | 否 | 否 | 是 | 中 | R2 Resolved：纯查找/文本/颜色/可见性移至单一无状态 Presenter |
| `FGCSUIWidgetPresenter` | Widget 查找与纯展示派生 | 无 | Widget/status/value | UMG presentation | UIBridge | UMG | GameThread | 是，presentation helper | 无 | 否 | 否 | 是 | 低 | 普通 C++ helper；无网络/Fleet/Command/生命周期/Tick |
| `UGCSDroneRowView` | 单行 DroneID 到选中 Overlay 的映射 | row ID/overlay/selected | row bind/detail selection | row visual | UIBridge | UMG | GameThread | 是，Blueprint row view seam | 无 | 否 | 否 | 是 | 低 | R0；薄但有解耦价值 |
| `ADroneFleetVisualizationActor` | Stable DroneOrder 到 Cesium World/Niagara arrays | actor/component/cache/arrays/origin world | Fleet events | one Niagara component arrays | level/FleetManager | Cesium/Niagara | GameThread/event-driven | 是，visualization boundary | 无 per-drone path | 否 | 否 | 是 | 高 | R0 |
| `FDroneData` / `GCSDataTypes` | UE 领域 DTO、Snapshot/Delta contract、consumer GPS guard | 值类型 | Provider DTO | Adapter/Fleet/UI/Viz | 全 UE 数据链 | Core types | 跨线程复制，GameThread消费 | 是 | Console FlightData 是跨进程前模型 | 否 | 否 | 是 | 高 | R0 |
| `UMockDroneDataSubsystem`（`Developer/Mock`） | 本地 UI 开发 Mock Snapshot | adapter ref | fixed seeds | Adapter snapshot | subsystem lifecycle | Adapter | GameThread | 是，developer mock | 不与 Production 同时启用 | 否 | 否 | 是（开发） | 低 | Phase 3H Resolved；显式配置且缺失时均默认关闭 |
| `UDSPGCSLoopbackProviderSubsystem`（`Developer/Acceptance`） | Stage 4 跨进程遥测/命令验收 | transport/acceptance counters | DSP frame | Adapter/acceptance commands | subsystem lifecycle | Protocol/Factory/Adapter | receive worker + GameThread | 是，acceptance tool | `ToFleetDrone` 与 Production 类似，但不应反向塑造 Production | 否 | 否 | 是（验收） | 低 | Phase 3H Resolved；与 Console `DSPGCSAcceptance` 配对，显式配置且默认关闭 |

## 4. 重复规则审计

| 规则 | 当前权威 Owner | 审计结论 | 等级 |
|---|---|---|---|
| Endpoint identity compare/hash/key | `DiscoveredDroneIdentity::EndpointKey` + `DroneManager` index | 发现 Ingress、Discovery、RemoveDrone 各自拼 `IP:Port`；3F 已改为统一调用 `EndpointKey()` | R1 已完成 |
| GPS 原始有效性和 #24/#33 职责 | `MavlinkParser::Parse` | Console 唯一决定 `gpsValid`；UE `DroneGPS::IsValid` 是消费侧输入防御，不覆盖字段 | R0 |
| Inbound mode name | `MavlinkParser::GetModeName` | UE 只显示字符串，没有第二套 inbound mode mapping | R0 |
| Outbound mode whitelist | `ArduCopterCustomMode` | 这是可发送命令的安全集合，不应与 inbound 显示表合并 | R0 |
| Drone Online/Offline | Console `DataManager` + `DroneSession` | UE 不实现 Heartbeat timeout | R0 |
| Server Connected/Disconnected | UE Production Telemetry Provider | 与 Drone Online 是不同语义 | R0 |
| Display Online/Offline/LinkLost | `GCSUIBridge::ResolveDroneDisplayStatus` | 纯派生显示，不修改 `FDroneData::bOnline` | R0 |
| Command mapping | UE DTO -> DSP、Console DSP -> domain、Dispatcher -> MAVLink | 三次映射位于三个协议/信任边界，不是重复 Owner | R0 |
| Command validation | UE 快速拒绝、Console Safety、Session 状态检查 | 纵深安全；Console 仍为权威 | R0/R3 不合并 |
| ACK matching | `CommandDispatcher`，Session 先按 Endpoint 路由 | 唯一 ACK 生命周期 Owner | R0 |
| Snapshot building/decode | Console Telemetry Provider / UE Telemetry Provider | 分属 wire 两端，无重复 | R0 |
| Fleet conversion/state | Provider 转 DTO；FleetManager 拥有状态 | Adapter 不拥有 Fleet | R0 |
| Selection | Fleet 详情选择 / UI 命令选择 | 两种产品语义，禁止合并 | R0/R3 |
| Niagara arrays | `DroneFleetVisualizationActor` | 唯一 Owner | R0 |
| GameThread handoff | `GCSServerAdapterSubsystem` | Provider 在 worker 完成 decode/reassembly/DTO；Adapter 唯一切 GameThread 并原子 Apply | R2 Resolved |
| MAVLink raw datagram decode | `MavlinkReadOnlyIngress` | Discovery 只接收已解码 HEARTBEAT | R2 Resolved |

## 5. Merge / Split Candidates 与保留边界

### Phase 3G 已完成的收敛

1. 删除 `MavlinkDiscoveryService::IngestDatagram` 和其 parser state；Discovery 测试直接提交已解码 HEARTBEAT，Datagram/Ingress 集成继续由现有 pipeline 回归覆盖。
2. 删除 `DroneManager::GetDrone` 别名，nullable 查询统一使用 `FindDrone`。
3. 删除 `FlightControl::Land/RTL/Hover` 无调用入口，Hover workflow 所需 `Takeoff` 改为 private；正式命令入口仍唯一落在 `DroneSession::SubmitCommand`。
4. Provider 移除上游 GameThread 调度，Adapter 成为唯一 Fleet 提交线程边界；1000 架分片回归验证只 Apply 一次。
5. 提取一个 `FGCSUIWidgetPresenter`，UIBridge 继续拥有生命周期、Selection、Command 和 Fleet 事件协调。

### Split Candidates

1. R2：如 Snapshot 逻辑继续增长，可把 UE 分片重组提取为无 UObject 的纯 assembler；当前不是必需。
2. R2：`ConsoleGCSApplication` 只有在新增更多服务生命周期后再提取 main-loop runner；当前作为 composition root 可接受。

### 明确不要合并

- `DataManager`：线程安全边界。
- `UdpClient` + `IMavlinkDatagramSender`：per-Session Endpoint 发送和测试/网络边界。
- `MavlinkCommand`：MAVLink protocol encoding boundary。
- `CommandManager` / `DroneSession` / `CommandDispatcher`：Fleet 业务、Session identity、ACK 生命周期三个不同 Owner。
- `SafetyConfig`：真实命令安全边界。
- `GCSServerAdapterSubsystem`：Provider lifecycle、bounded queue、GameThread boundary。
- `IGCSDataProvider` / DSP Transport interface / Factory：Provider 与平台 Transport seam；当前只有 Loopback 实现也应保留。
- `GCSDroneRowView`：Blueprint authored row 与 C++ selection 的 View seam。
- `MavlinkParser` inbound mode 表与 outbound mode whitelist：观测能力和允许控制集合不同。
- `CurrentSelectedDroneID` 与 `SelectedCommandDroneIDs`：详情选择和命令目标不同。

## 6. 主要文件复杂度

LOC 为当前 header+cpp 物理行数；方法/成员按头文件可见声明计数，目的是排序而非复杂度评分。

| Rank | Type | LOC | Public methods（约） | State members（约） | Responsibility count | 结论 |
|---:|---|---:|---:|---:|---:|---|
| 1 | `UGCSUIBridgeSubsystem` | 1451 | 6 | 17 | 5 | Phase 3G 已提取 173 LOC Presenter；仍是 UI coordination owner |
| 2 | `DroneSession` | 770 | 19 | 13 | 5 | 大但属于聚合根，R0 |
| 3 | `UDSPGCSProductionTelemetryProviderSubsystem` | 626 | 12 | 16 | 5 | R2 只消除双线程切换 |
| 4 | `FlightControl` | 495 | 4 | 6 | 1 | Phase 3G 已收窄为 Hover workflow |
| 5 | `CommandDispatcher` | 538 | 11 | 6 | 2 | ACK/queue 紧密相关，R0 |
| 6 | `DSPGCSProductionCommandReceiver` | 528 | 5 | 6 | 4 | trust/thread boundary，R0 |
| 7 | `MavlinkCommand` | 505 | 9 | 0 | 1 | LOC 受旧格式影响，职责清晰，R0 |
| 8 | `ADroneFleetVisualizationActor` | 503 | 7 | 17 | 4 | 单 Actor 数据桥，R0 |
| 9 | `AppConfig` | 481 | 1 | 15 | 2 | 配置解析集中，R0 |
| 10 | `UFleetManagerSubsystem` | 462 | 14 | 5 | 4 | Fleet aggregate，R0 |

## 7. Top Maintenance Risks

1. Developer Provider 仍编译在 Runtime Module；Phase 3H 已用独立目录、独立配置段和“缺失配置仍关闭”消除误启用风险。若未来 Developer 类显著增长，再评估独立 Module。
2. `GCSUIBridgeSubsystem` 虽已移出纯展示 helper，仍同时协调导航、分页、详情、选择和命令；后续只按真实变化再评估，不拆多个 Subsystem。
3. `ConsoleGCSApplication` 作为 composition root 同时组装网络、DSP、线程和主循环；新增服务前应控制继续增长。
4. UE 分片重组仍位于 Telemetry Provider 内；只有协议复杂度继续增长时才提取纯 assembler。
5. Console 业务层仍有较多直接 `ConsoleUI` 日志调用，未来若引入第二种前端再考虑事件化。

## 8. 新开发者入口

| 修改目标 | 首要入口 | 必要时第二入口 |
|---|---|---|
| A. Drone Online | `DataManager::RefreshOnlineStatus` | `DroneSession::RefreshHeartbeatStatus`（状态/日志/Recovery side effect） |
| B. MAVLink GPS 解析 | `MavlinkParser::Parse` 的 #24/#33 case | 新增字段时 `FlightData.h`；UE 仅消费 guard 看 `DroneData.h` |
| C. 新 MAV_CMD | `DroneCommandRequest` + `DroneSession::SubmitCommand`（业务） | `CommandDispatcher` + `MavlinkCommand`（ACK/编码）；若 UE 可触发，还需两端 DSP mapping |
| D. UE Fleet 状态 | `FleetManagerSubsystem` | 内部 Snapshot/Delta contract 变化时 `GCSDataTypes.h` |
| E. UI 按钮行为 | `GCSUIBridgeSubsystem` | 仅文本/颜色/查找时进入 `GCSUIWidgetPresenter`；payload/结果语义变化时进入 CommandProvider |
| F. Niagara 数据桥 | `DroneFleetVisualizationActor` | 仅视觉资产行为再进入 `NS_DroneFleet` |

新增端到端命令天然跨 UE/DSP/Console/MAVLink 边界，会超过两个组件；这不是通过合并安全/协议层来解决的问题。继续使用现有边界和文档化调用链，不创建 `MavlinkProtocolCatalog`。

## 9. R0 / R1 / R2 / R3 计划

### R0 当前合理

保留 DataManager、UdpClient、MavlinkCommand、Command 四层职责、Adapter、Transport interface/factory、RowView、FleetManager、Visualization 和三种连接/显示状态分层。

### R1 本轮已完成

- Endpoint key 构造统一到 `DiscoveredDroneIdentity::EndpointKey()`；移除 Ingress/Discovery 两个重复 private helper，并让 `DroneManager::RemoveDrone` 使用同一权威实现。
- 修正 `DataManager.h` 中“整个程序唯一一份数据”的旧注释，明确它是 per-Session 快照。
- 未改变 public Production API。

### R2 Phase 3G 结果

本阶段指定的五项均已 Resolved。Phase 3H 随后完成 Mock/Acceptance 轻量边界隔离；当前规模不创建 Developer Module。

### R3 暂时不要动

- Endpoint identity、Session lifecycle、ACK 路由、Command 安全门。
- CommandManager/Session/Dispatcher/MavlinkCommand 合并。
- Adapter/FleetManager 合并或删除 Transport interface。
- DSP Wire Format、MAVLink message、Cesium/Niagara、Selection、FleetGeoOrigin 和性能参数。

## 10. Target Architecture

```text
Console
App        ConsoleGCSApplication
Config     AppConfig / SafetyConfig / DSP config loader
Network    MavlinkUdpReceiver / IMavlinkDatagramSender / UdpClient
Mavlink    ReadOnlyIngress / Discovery / Parser / MavlinkCommand
Fleet      DroneManager / DroneSession / DataManager / FlightData
Command    CommandManager / CommandDispatcher / FlightControl / HoverTest
DSP        ProductionTelemetryProvider / ProductionCommandReceiver / Transport
UI         ConsoleUI

Unreal Production
DSP        Production Providers / Transport / Factory
Fleet      GCSServerAdapter / FleetManager / DroneData contracts
Command    ProductionCommandProvider
UI         GCSUIBridge / GCSDroneRowView
Visualization DroneFleetVisualizationActor / Cesium / Niagara

Unreal Developer
Mock       Public|Private/Developer/Mock
Acceptance Public|Private/Developer/Acceptance

Unreal Tests
Regression Private/Tests
```

该结构保留边界并减少重复 Owner；Cleanup Phase 3H 只移动 Developer Provider，不移动 Production 目录、不重命名公共类、不改变 Production 业务行为。

## 11. Cleanup Phase 3H：Developer Runtime 边界

| Class / File | Category | Current Purpose | Enabled By | Default Enabled | Production / Asset / Test Reference | Runtime State | Shipping Needed | Action / Risk |
|---|---|---|---|---|---|---|---|---|
| `FDSPGCSLoopbackTransport` | Production | localhost UDP 双向 DSP Transport | Production Provider | 是 | Production Provider / 无资产引用 | socket receiver | 是 | 保留；`Loopback` 描述同机地址，不表示 Test |
| `UDSPGCSProductionTelemetryProviderSubsystem` | Production | decode、chunk reassembly、link health | Production config | 是 | Production | transport/assembly/link | 是 | 保留 |
| `UDSPGCSProductionCommandProviderSubsystem` | Production | Command request/result | UI Bridge | 是 | Production | pending command | 是 | 保留 |
| `Developer/Mock/UMockDroneDataSubsystem` | Developer Mock | 无 Console 时提供 5 架固定 Fleet 数据供 UI 开发 | `[DroneGCS.Developer.Mock] Enabled=True` | 否；配置缺失也否 | 无 Production、Blueprint、Map、Automation 引用 | 仅显式启用时注册 Provider | 否（当前开发用途） | 保留；低风险 |
| `Developer/Acceptance/UDSPGCSLoopbackProviderSubsystem` | Developer Acceptance | 配合 Console 工具做 3 架遥测、命令和结果跨进程验收 | `[DroneGCS.Developer.Acceptance] Enabled=True` | 否 | 无 Production、Blueprint、Map、Automation 引用 | 仅显式启用时创建验收 Transport | 否（当前验收用途） | 保留；低风险 |
| `ConsoleGCS/DSPGCSAcceptance.vcxproj` | Acceptance Tool | Console/UE 跨进程协议验收 host | 独立 exe 手工启动 | 否 | 不由 `DroneGCS.exe` 或 Regression Test 链接 | 独立进程 | 否 | 保留独立 Project |

依赖方向固定为：`Production` 不依赖 `Mock/Acceptance/Test`；`Developer Mock/Acceptance` 依赖 Production interface；`Regression Test` 依赖 Production。Production Provider 初始化失败只产生 `Disconnected/Error/Link Lost`，不存在自动切换 Mock 的 fallback。

当前只有两个 Developer UCLASS。为它们新增 Module 会引入额外 `Build.cs`、`.uproject` 模块声明、加载时机和 Standalone/Commandlet 测试边界，维护成本高于收益，因此本阶段不创建 Developer Module，也不加未经验证的 `WITH_EDITOR` 编译条件。

`DSPGCSLoopbackTransport` 将来可评估重命名为 `DSPGCSUdpTransport`，以减少人工误判；这是涉及 Production 公共名称的后续 R2 候选，本阶段不实施。
