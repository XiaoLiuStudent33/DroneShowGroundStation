# 阶段 B 简报（2026-09-10）

基线 `4820a11`，仅 Coordinator / Selection / Mapping / Upload Workflow / Telemetry State 边界；不进入阶段 C，不提交。

## 状态边界

- 纯展示：Widget、控件绑定、可见性、行诊断及文本；`DetachView` 只释放这些视图引用和 Delegate。
- 用户意图：命令选择继续由 GameInstance 的 UIBridge `SelectedCommandDroneIDs` 持有；详情选择继续由 Fleet 持有，两者独立。
- Application / Workflow：既有 Coordinator 保存选中航线、Confirmed NPZ Mapping、确认 revision、AutoStart 快照、冻结并发和预提交目标模型。UIBridge 初始化时创建同一个 Coordinator；Main Widget 重绑定不再调用业务 `Shutdown`。Ticker 从视图生命周期独立，`AdvanceWorkflowState` 不要求 Widget 存在。
- Backend Authority：现有 Assignment / Frozen Sync / Upload Job / Console Authority 不变，构建→冻结→精确 Accepted→复验→单次 Start 逻辑未改。最终 Subsystem Deinitialize 才 Shutdown；没有新状态所有者、DTO、Service、Manager。

修改前：BindMainWidget → Coordinator.Initialize → Shutdown，可能丢失确认与正在等待的事务。修改后：Subsystem.Initialize → InitializeWorkflow；BindMainWidget → DetachView/绑定新视图；原事务继续推进，绑定事件不创建事务。原业务条件导致的确认失效仍按既有规则执行。

## 遥测边界

原 ProductionTelemetryProvider 内的 FDroneData 转换提为 Private `GCSVehicleStateProjection::FromTransport`。与基线逐字段核对一致，保留 validity、无效 Battery/RSSI 默认值、GPS 时间、身份与 RGB 语义。Raw DSP DTO 只参与私有转换；公开读取继续复用 `FDroneData`、Fleet.GetDroneByID/GetDronePage 和 OnDataBatchApplied 等事件。ServerAdapter 原有 GameThread 原子提交、Fleet 的派生状态和 Niagara 消费均未改变，没有重写接收链。

## 证据与限制

先增加重绑定回归再改生产代码：`ArchitectureStageB-20260910-Before2` 复现确认 revision/事务/目标丢失、Accepted 后 Start 为 0。最初 Before 的抽象 Widget 夹具错误不作为业务证据。修复后加强为真实 Main Widget 两次绑定，检查同一 owner、命令/详情选择、目标模型、冻结与同步各一次、无视图 Accepted 后单次 Start、完成后重绑不重放。既有 Ownership 测试增加真实 Workflow 重建、旧 Delegate 无效、新 Delegate 单次触发和已应用并发保持。

新增 `DroneGCS.Telemetry.VehicleState.ProjectionWithoutWidget` 验证有效/无效投影及无 Widget 时 Adapter→Fleet 查询。开发首轮漏设夹具 SourceID，Adapter 正确拒绝；补齐后通过。Main Widget 使用与既有 Selection 测试相同的缺失 Map/Logs 页面预期日志，并精确限制每页 2 次；不忽略其他错误。

全量首轮 244/245，UploadDashboardToggle 的两个断言失败：旧部分 UI 夹具未创建 Coordinator，带活动 Job binding 却期待草稿可编辑，绕过了完整 Main UI 的原有门禁。补充活动 binding 只读/JOB LOCKED 断言；空 binding 下保留原可编辑/STALE 断言；随后恢复 binding，保留原反复开关不改事务断言。生产门禁没有修改。

Command final-result 丢失 / callback re-entry 未修改：本轮没有证明它们影响上述所有权的独立复现。这里修复的是已复现的 Initialize 清理事务问题，不声称解决 Command 回调风险。

行为变化仅为修复 UI 重绑定导致的业务状态丢失，并允许既有工作流脱离视图继续运行、原有门禁不再依赖 Main Widget 是否先绑定；用户文本、协议、参数、ACK/Timeout/Retry、Identity、RouteSlot、Mapping/Assignment/Upload Authority 均未改。软件 fake/回环证据不证明真机执行或 UI 人工观感。

## 阶段 C 前保留项

Coordinator 仍包含 Widget 查找、Enabled/Visibility、文本格式化和部分 Actor 展示投影；本轮仅分离其生命周期及 Tick 的工作流推进入口，未机械拆类。UIBridge 仍持有命令意图并协调具体 Widget；Provider 仍有 MDHK 表格/连接状态格式化，完整协议无关表格模型及 Presenter/Application API 留待阶段 C。既有 Fleet 状态入口已可供新 UI 复用。

代码（不含文档）：生产 +102/-47，测试 +219/-0，总 +321/-47。

## 修改文件

相对 `UnrealGCS/Source/DroneShowPlatform1/`：

- `Private/GCSRouteWorkflowCoordinator.h`、`.inl`
- `Public/GCSUIBridgeSubsystem.h`、`Private/GCSUIBridgeSubsystem.cpp`
- `Private/DSPGCSProductionTelemetryProviderSubsystem.cpp`
- 新增 `Private/GCSVehicleStateProjection.h`
- `Private/Tests/GCSRouteWorkflowCoordinatorTests.inl`
- `Private/Tests/GCSUploadDashboardToggleTests.cpp`
- 新增 `Private/Tests/GCSVehicleStateProjectionTests.cpp`

文档：本简报、PROJECT_HANDOFF_CN.md、Development_Status_CN.md、Test_Architecture_CN.md。

## 最终 Build / Test

- Console Release/Debug solution：均 exit 0，0 errors（各 27 个既有依赖数值转换警告）。UE Editor Development：最终 build exit 0，MSVC 非首选版本及启动期 Cesium 诊断仍在。
- Console full（16 个顶层 suite）/disarm/runtime-config/concurrency/artifact10K/mission-benchmark 六次均 exit 0。
- Launcher Smoke：32/32，exit 0，无遗留测试子进程。
- UE Full2：245/245（224 success + 21 success-with-warnings），0 failed / 0 not-run；Synthetic：1/1。合计 246/246，较阶段 A 新增 2 项。
- 原始报告 `UnrealGCS/Saved/Automation/ArchitectureStageB-20260910-{Before2,Targeted,Targeted2,Full,Full2,Synthetic}`；构建/测试日志与机器摘要 `BuildLogs/ArchitectureStageB-20260910/`。早期失败保留，不以进程 exit 0 冒充全部通过。
- Console/Shared/Content/部署配置无 diff；未使用真机测试入口，未修改飞控参数。未 commit/push，未进入 C。
