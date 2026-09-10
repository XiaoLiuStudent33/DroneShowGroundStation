# 阶段 C 简报与 UI Replacement Readiness（2026-09-10）

基线 `e063c78`。本轮仅稳定 UI/Application 边界，不改 UMG 资产、不进入 UI 重做、不 commit/push。

## 变更

- 复用 UIBridge Subsystem 作为应用入口；没有新增 Facade/Service/Manager/Repository 或状态所有者。Fleet、Command Provider、RoutePreview、Coordinator、Assignment/Upload 原 Authority 保持。
- Command readiness 判断移至既有 Command Provider，旧按钮与新查询共用；Command 结果处理订阅在 Subsystem 初始化绑定、Deinitialize 解绑，不再要求先创建 Main Widget。`GetCommandResultEvent` 暴露原事件引用，不创建转发队列或第二份结果状态。
- 现有 `FMDHKTelemetryTableRow`、连接状态枚举和 Delegate 原样移入 `GCSTelemetryTypes.h`，反射名称/字段/序列化身份不变。过滤模型不再 include TelemetryProvider 实现。排序数据查询与旧 ListView 更新分开；32 列反射赋值和 Command 结果文字移入已有 `FGCSUIWidgetPresenter`，原数字/文本/占位语义保留。
- Coordinator 增加显式路径、工作区、原点文本、并发和工作流意图入口；旧按钮仍使用相同业务执行体，文件/目录对话框及 TextBox 读取成为适配步骤。查询门禁可接受新 UI 的原点草稿，不要求旧控件存在。
- 既有确认映射/操作门禁类型移出 private header。新增 `FGCSWorkflowView`、`FGCSUploadRouteView`、`FGCSMissionView` 是按次生成的只读投影，不缓存事务/ACK/重试状态，不替代 Upload mirror。上传结果文字复用旧展示函数；生成进度、Job 统计、完整 Route 页和预提交目标通过投影查询。
- `bHasJobSnapshot` 与 `bRoutesComplete` 分开；不完整页不会冒充完整成员列表。新 UI 应在页恢复时保留展示成员，不能把空页当作目标移除。`bAuthoritative=false` 的预提交目标不代表真机上传结果。AutoStart Completed 仍只表示本地 Start 已派发。

## 新 UI 的直接依赖自检

| 能力 | 稳定对象 / 数据 | 入口 |
|---|---|---|
| Vehicle List | UFleetManagerSubsystem / FDroneData | GetDronePage、GetDroneByID、OnDataBatchApplied |
| Vehicle Selection | Fleet + UGCSUIBridgeSubsystem | SelectDrone；Set/ReplaceCommandDroneSelection；OnCommandSelectionChanged |
| Connection Status | UIBridge / GCSTelemetryTypes | GetConnectionState；CanSendOperatorCommands |
| Commands | UIBridge / 既有 Command Contract | ExecuteOperatorCommand、CanExecuteOperatorCommand、Set/GetOperatorControlMode |
| Command Result | UIBridge / FGCSProductionCommandResult | GetCommandResult、GetCommandResultEvent；业务读 State，展示读原文本 |
| Telemetry / MDHK Table | Fleet/FDroneData；UIBridge/FMDHKTelemetryTableRow | GetDroneByID；GetTelemetryTableRows（原排序） |
| Route Upload | UIBridge / GCSWorkflowTypes | SelectRouteFile、SelectRouteWorkspace、CommitRoutePreview、GetWorkflowState[ForOrigin]、ApplyUploadConcurrency、ExecuteWorkflowAction |
| Mission / Playback | UIBridge / FGCSMissionView；URoutePreviewSubsystem | StartScheduledMission、ExecuteOperatorCommand、Execute/EndDanceStep、GetMissionState；既有 Play/Pause/Stop/Seek |

新 UI 不必 include DSP/MAVLink/Socket/CommandReceiver/ACK packet、Coordinator 私有类、Upload 实现或 TelemetryProvider。历史 EDSP 命名的连接枚举与 CommandTypes 文件名只作为既有稳定合同保留，不进行 Blueprint API 大规模重命名。危险操作确认由显式操作员意图传入，不能从控件标签反推。旧 UMG 不需要改资产。

Readiness：上述八类核心能力的 C++ 接入边界就绪，可替换布局；不是宣称旧 UI 已重做或已完成人工视觉验收。UIBridge 仍有旧 Widget 绑定、Mission/Selection 意图编排和展示代码；Coordinator 仍有具体控件投影，均隐藏在入口后。本轮不以迁出所有实现或减少文件行数为目标。

## 规模

新增生产类 0，删除生产类 0。复用已有 Presenter；两份新公共头用于移出的合同和只读投影，不新增业务执行层。

| 文件 | 前 | 后 |
|---|---:|---:|
| UIBridge.cpp | 7149 | 7197 |
| UIBridge.h | 709 | 737 |
| Coordinator.inl | 4083 | 4239 |
| Coordinator.h | 630 | 574 |

生产 +743/-312（净 +431）；测试 +106/-0。行数包含本轮完整公共入口和投影；不是机械拆文件后计算减少量。

## 验证

- 新增 `DroneGCS.Application.NoWidgetAndRebind`：无 Widget 查询/并发意图、独立详情与命令选择、暂停门禁、显式确认、无 Widget 结果订阅、查询不创建事务、两次真实 Main Widget 绑定保持状态、修改 Button Enabled/Visibility 不影响 readiness。
- 新增 `DroneGCS.Application.TelemetryAndCommandProjection`：只读 MDHK 数据投影到既有 BP Row；无效值占位、RGB、ID 与原 Command 文本。新增测试编译不 include Provider/Upload/Coordinator 实现头。
- 强化阶段 B 真实重绑 fake 上传事务：稳定查询保留确认 revision 和三目标，区分预提交/权威 Job，反复查询不重复同步。
- Console Release/Debug、UE Editor Development：成功。Console full（16 顶层 suite）及 disarm/runtime-config/concurrency/artifact10K/mission-benchmark 六次 exit 0；Launcher 32/32。
- UE Full 247/247（225 无 warning + 22 带既有/场景 warning），Synthetic 1/1，合计 248/248，比 B 新增 2，零 failed/not-run。
- 原始日志 `BuildLogs/ArchitectureStageC-20260910`；UE 报告 `UnrealGCS/Saved/Automation/ArchitectureStageC-20260910-{Targeted,Full,Synthetic}`。既有 Cesium/非首选 MSVC 等诊断未顺手修复。
- 已核对移动的 MDHK 反射声明与旧文本函数原样一致。Console/Shared/Content/部署配置无 diff；无真机操作。Command ID/参数/ACK/Timeout/Retry/Identity/Mapping/Freeze/AutoStart/wire format 未改。
- Command result loss / callback re-entry 未修复；无独立证据支持扩大修改，继续技术债。原事件引用继续原回调语义，未承诺新的最终结果保留保证。

## 修改文件

相对 `UnrealGCS/Source/DroneShowPlatform1/`：

- `Public/GCSUIBridgeSubsystem.h`、`Private/GCSUIBridgeSubsystem.cpp`
- `Private/GCSUIWidgetPresenter.h`、`.cpp`
- `Private/GCSRouteWorkflowCoordinator.h`、`.inl`
- `Public/DSPGCSProductionCommandProviderSubsystem.h`、`Private/DSPGCSProductionCommandProviderSubsystem.cpp`
- `Public/DSPGCSProductionTelemetryProviderSubsystem.h`
- `Private/GCSTelemetryFilterModel.h`
- 新增 `Public/GCSTelemetryTypes.h`、`Public/GCSWorkflowTypes.h`
- 新增 `Private/Tests/GCSApplicationBoundaryTests.cpp`
- `Private/Tests/GCSRouteWorkflowCoordinatorTests.inl`

文档：本简报及 PROJECT_HANDOFF_CN / Development_Status_CN / Test_Architecture_CN 的阶段 C 跟踪条目。
