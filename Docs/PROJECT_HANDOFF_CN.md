# DroneGCS 项目交接速览

Status: CURRENT SUPPORTING
Last Updated: 2026-09-09
Baseline: `master` / `f115587` + 2026-09-09 本交接文档更新

本文件是快速接手入口。长期边界以 [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md) 为准，当前调用链以 [Code_Architecture_CN.md](Code_Architecture_CN.md) 和 Production 源码为准，进度以 [Development_Status_CN.md](Development_Status_CN.md) 为准。

## 2026-09-09 架构治理审计补充（源码未改）

以 `a95309d8b37ecb00c779d5c4264aacb86294d8ec` 完成一方源码、工程、配置与测试文本审计，交付 [Architecture/00_REPO_OVERVIEW.md](Architecture/00_REPO_OVERVIEW.md) 至 09 的十份文档。本轮仅编写文档，没有编译、重跑回归或启动真机链路；重构 Phase 0 尚未开始，须用户确认方案。

当前源码与下文旧阶段描述存在差异，接手时优先阅读 [当前架构审计](Architecture/01_CURRENT_ARCHITECTURE.md)：默认编号映射为 `RouteSlot = NumericDroneID`（Drone000→0），已确认 NPZ Mapping 存在时使用其 Entries 目标/Slot，而非始终使用当前 UI 勾选；已有 DSP MAVFTP runtime cap 查询/应用及 binding；HoverTest 的 FlightControl 使用每活动 Session 工作线程。这些是已存在行为，本轮只记录，不恢复旧映射或改线程/通信。旧文档中的减一映射不可作为重构依据。

主要治理方向是分离 UIBridge/RouteCoordinator 的应用状态与 Widget 展示、提供协议无关高层 API、保留现有 Session/Assignment/Upload/Socket Authority；细分步骤见 [路线图](Architecture/07_REFACTOR_ROADMAP.md)。所有发现的潜在 Bug 仅记录，未修复。

## 1. 阅读顺序

1. [README_CN.md](README_CN.md)
2. [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)
3. [Code_Architecture_CN.md](Code_Architecture_CN.md)
4. [Development_Status_CN.md](Development_Status_CN.md)
5. [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)
6. 当前任务相关专项文档

Audit 与 [History/](History/) 只作证据或历史参考。

## 2. 系统边界

```text
Unreal Engine 5.5.4
        <-> DSP-GCS localhost UDP
ConsoleGCS C++
        <-> MAVLink / MAVFTP
ArduPilot / APM
```

- UE：Frontend、Visualization、Operator Workflow。
- ConsoleGCS：MAVLink、Network、Session、真实命令与 Route Upload backend。
- Fleet identity：`Remote IP + UDP Source Port`，不是 SYSID。
- RouteSlot 不是 DroneID、SYSID 或 particle index。
- E.0 临时自动规则是 canonical `NumericDroneID - 1`；只消费 `SelectedCommandDroneIDs`，`M_DroneIdLabel` 不是 Authority。
- UE 不直接向飞控发送 MAVLink。
- 正式用户入口是安装后的 `DroneGCSLauncher.exe`，不是 UE 根目录的纯前端 Game EXE。
- 安装版程序位于 Program Files，可变配置、日志、UE Saved 和 Artifact Workspace 位于 `%LOCALAPPDATA%/DroneGCS`。

开发期对照资料（只作协议审计，不属于 Production Authority）：

- 旧版 BGC2 源码：`D:\Download\重要资源存储\bgc2\bgc2`
- 更新后的 BGC2 打包项目：`D:\Blender\bin`

## E.0.2 操作合同

- Console Interactive Dashboard 的颜色属于 styled frame，由 Renderer thread 一次提交；Total=`0x09`、Online/ONLINE=`0x0A`、Offline/OFFLINE=`0x0C`。Linear/Pipe/File/Tee 不输出 ANSI。
- Fleet 全选按钮只选择当前完整 Fleet 中的非空 Online DroneID；`X` 表示当前集合不是精确全选，`✓` 表示精确相等，再点清空。零 Online 时 Disabled。
- 唯一选择 Authority 是 `UGCSUIBridgeSubsystem::SelectedCommandDroneIDs`；不使用当前页、Niagara ID 或 SYSID。新上线 Drone 不会自动加入，下一次显式点击才以当前 Online 集合替换。
- AutoStart 或非终态 Upload Job 活动时选择 mutation 被禁用，Frozen Assignment 不受按钮影响。

## 3. 当前 Route Upload 状态

Phase 2A～2G、RouteUploadManager、UI-3C、MissionTransport abstraction、MissionFtpCompiler、MAVFTP Writer、Hardware Harness、Quick Test 与 MAVFTP production cutover 已实现。

当前唯一生产链是：

```text
UE Route Workflow
-> SelectedCommandDroneIDs -> automatic explicit Assignment
-> Freeze(AllowDegraded) -> exact Console Accepted
-> DSP-GCS Upload Job
-> ConsoleUploadJobAuthority
-> RouteUploadManager
-> MissionPreparationWorker
-> MAVFTPMissionTransport
-> DroneSession endpoint-bound MavFtpClient
-> shared MAVLink sender/socket
-> ArduPilot/APM
```

旧的标准 MAVLink Mission 上传 Transport 已退休并删除；生产失败明确失败，不做协议回退。

## 4. 当前完成点与暂停点

```text
Route/MAVFTP Production Chain = DONE
Joint Shipping Package = DONE
Visible Full-Control Backend Lifecycle = DONE
NSIS V1.0.0 Installer = DONE
Second-PC Install/Launch = USER-CONFIRMED PASS
FleetPanel V2 / Row Virtualization = USER-CONFIRMED PASS
Operator Command Expansion = IMPLEMENTED
Real Telemetry Digital Twin = IMPLEMENTED
```

### 4.1 2026-09-07～09-09 当前 UI 与控制状态

- `WBP_GCS_FleetPanel_V2`、`WBP_GCS_FleetRow`、`BP_FleetRowData` 是当前机群列表；旧 `WBP_GCS_FleetPanel` 及旧 Row C++ 已删除。ListView 行使用稳定数据对象原位刷新，复选框与从 ID 列开始的详情点击区互不覆盖。
- 详情选择与命令选择仍是两套 Authority。点击一行只更新当前详情对象，不自动打开 `CHILD_DroneDetail`；复选框只改变 `SelectedCommandDroneIDs`。
- `BTN_Menu` 不再改变 Fleet/Detail；`BTN_Fleet` 独立切换 `CHILD_FleetPanel`，`BTN_Control` 独立切换 `CHILD_DroneDetail`，两面板可同时开关。
- 授时倒计时到零和“执行舞步”只发送真实飞控命令，不再自动启动底部 NPZ 播放；NPZ 仅由 Playback 控件显式播放。
- HoverTest 起飞工作流会先请求 GUIDED，再执行解锁/起飞，不再被 MANUAL 门禁错误拒绝。
- 已接入返航、原地降落、暂停、前进、倒退和高级控制；前进/倒退仅在任务暂停后可用。恢复数据将消息 `1/24/33/30/187` 恢复为 1 Hz，并以新鲜 #187 样本确认恢复。
- 拉烟沿用旧协议的 relay 0，但改为 1 秒脉冲后自动复位；关机沿用旧协议的 relay 1 置 1。待机/重启/关机有 armed gate，关机还保留二次确认。
- #187 RGB 已映射到标准灯光状态；真实 GPS 为每机保留前后样本并在共享 Visualization Actor tick 中插值。没有有效真实 GPS 时仍遵守既有预览/初始位置边界，不把 NPZ 播放伪装成真机遥测。
- 遥测筛选页面已支持条件和预设；筛选结果不会隐式改变命令选择，操作员必须明确勾选目标。

### 4.2 当前已知边界

- 2026-09-09 真机观察：8 架中的 6 架可由旧 relay-1 合同正常关机；`Drone001`、`Drone007` 对 `MAV_CMD_DO_SET_RELAY` 返回 `ACCEPTED` 但硬件未断电。批量目标与 ACK 链正常，当前按两机继电器配置/断电硬件差异处理，暂不修改 UE/Console。
- “命令 ACCEPTED”只证明飞控接受相应 MAVLink 命令，不证明执行器、继电器或整机物理状态已经达到目标。
- 备用机/补点飞行仍为 DEFERRED；实时遥测中断策略不得提前承担备用机调度语义。
- 旧 1.0.0 安装包未包含 9 月 7～9 日的上述工作树更新，若要外部部署需重新走联合 Shipping/Installer 验证。

生产参数为 `segment_items=4000`、request/terminate timeout `750/2500 ms`、retry `3`、write window `16`。`segmentCapacity` 以包含 Reserved Home 的 Wire Mission Items 计数。

正常 UI Start 每次重新建立当前选择的 Working/Frozen Assignment，等待精确 Console Accepted，重新校验全部 Authority 后 exactly once 创建一个 Job。E.1 后，`EDT_MaxConcurrency` + `BTN_ApplyMaxConcurrency` 是本次及后续 Job 的操作员 Authority：合法 Applied 值为 `1..50`，Start 在事务开始时冻结该值并复用现有 DTO `MaxConcurrentTargets`；Console 严格拒绝越界值，实际并发为 `min(DTO.MaxConcurrentTargets, MatchedTargetCount)`。不存在 Console `max_concurrent_targets` INI 覆盖。E.2 多机硬件验证尚未完成；Console 重启后的 DroneID 发现顺序风险为 DEFERRED/E.3。

Console 可观测性由 `ConsoleUI` 的单一 Renderer 线程负责。Producer 只入队 1024 条上限的结构化记录或发布 Fleet 快照；真实 Windows Console 使用动态尺寸 Dashboard，重定向、Pipe、文件、Tee 和 CI 使用无 cursor 控制的 Linear 输出。溢出通过 `DroppedLogCount` 可观测，terminal/error 优先于普通日志。E.1 使用动态 N Target 准入和公平 Write permit pool；`write_window=16` 仍为每 Client 上限，独立 `global_write_outstanding_cap` 合法范围 `1..800`、启动默认 `800`。

当前软件基线为 `f115587`。下一步应从具体的新需求或受监督真机复验继续，不重做已经通过的新机群列表和按钮接线；仍需分别验收非飞行闭环、受监督飞行与航线上传、升级覆盖、卸载清理和代码签名。异机“安装/启动通过”不能替代这些分项证据。

## 5. 保护规则

- 不创建第二套 Fleet、RouteAssignment、segment planner 或 network authority。
- 不恢复 Transport 选择器或协议 fallback。
- 不把 7000 items 的用户观察写成固定飞控上限。
- 不在未授权时运行 `--mavftp-test`、write probe 或真实 Mission upload。
- 不将 Fleet 重复 ONLINE 日志诊断混入 MAVFTP segmentation。
- 不把 `All Write ACKed` 等同于 `Mission Ready`。

## 6. 当前证据

Console Release x64 Solution Build 已于 2026-08-25 通过（0 errors、12 warnings、ExitCode 0）；六组专项测试和 Full ConsoleGCSTests 全部通过。UE Development/DebugGame Build 均通过，E.0 AutoStart 6/6 与合并相关回归 164/164 PASS。硬件 1000～6000 PASS 与 7000 NAK 2 均为用户确认且没有仓库日志。

2026-09-03：联合启动隔离 Smoke 32 项、V3 Shipping/manifest 严格校验、Launcher `--check` 与 NSIS 3.12 无警告构建 PASS；生成 V1.0.0 安装程序。用户确认安装程序转移到另一台电脑后安装与启动通过，证据等级为 `USER-CONFIRMED`，没有异机日志进入仓库。

2026-09-09：新版机群列表由用户完成现场测试；Console 高级命令、DSP round-trip、HoverTest、#187 RGB、真实 GPS 插值和独立 Toolbar 均有对应源码回归。原 MissionUpload fake production chain 使用 `Drone001–Drone005` 的旧夹具编号，已改为当前 canonical `Drone000–Drone004`，完整 Console 回归恢复通过。真机 relay 输出和物理断电行为不属于软件自动化证明范围。

详细测试边界见 [Test_Architecture_CN.md](Test_Architecture_CN.md)。

## 2026-09-09 Phase 0完成（架构重构未开始）

用户仅授权Phase 0；构建/行为基线见 [Architecture/BASELINE.md](Architecture/BASELINE.md)。5项构建通过；Console 6次运行与Launcher Smoke 32检查通过；UE最终242+Synthetic1=243项通过。首轮3项Provider禁用配置相关失败保留；测试不能只看exit=0。762源码/配置与69资源内容对比不变，无生产源码/资产/配置修改、无真机操作。真机、人工UI、实际安装包和真实NPZ fixture性能基准仍未验证。Phase1未开始，须另行授权。

## 2026-09-09 Phase 1完成，Phase 2未开始

正式基线b3da56f；本轮仅3个生产文件局部整理：Receiver纯参数组装去重、CommandProvider公共头前置声明、Sockets/Networking降Private。未改业务行为/Blueprint/协议/Authority。Console Release/Debug、UE Development构建通过；UE243/243、Console6次运行、Launcher32检查通过。生产代码+38/-39行。未自行stage/commit；详见 [Architecture/17_PHASE1_REPORT.md](Architecture/17_PHASE1_REPORT.md)。

## 2026-09-10 压缩重构阶段 A 完成

后续按用户确认合并为A Command+Stable Contract、B Coordinator+Telemetry+Workflow State、C UIBridge Slimming+Presenter/Application API、Final UI Migration/Readiness；本轮仅A，B未开始。基线b5f9a6d。复用现有CommandType/Parameters/Result，新增EGCSCommandResultState及IsSuccessfulDispatch；AckState原文本保留显示，UI业务判断改用状态。DSP结果转换仅在Private，CommandProvider公共头不再出现DSP wire类型；请求签名、CommandID、参数、ACK/timeout/retry、映射/上传/遥测来源不变。无新DTO、Service或Authority；不改资产、不提交。Console Release/Debug、UE Development通过；Console六次回归、Launcher32/32、UE244/244（原243+新契约1）通过。原final-result丢失/事件重入、UI工作流状态归属及Telemetry/Upload公共合同仍待后续限定阶段处理。MavlinkCommand.cpp仅发现行尾变化，Git内容hash与HEAD一致，保留未处理。


## 2026-09-10 压缩重构阶段 B 完成

基线 `4820a11`。既有 Coordinator 改为由 UIBridge Subsystem 初始化并稳定持有；Widget 重绑定仅 DetachView/绑定，不清除 Confirmed Mapping、AutoStart 事务与目标模型。命令选择/详情选择继续分别由 UIBridge/Fleet 持有；Assignment/Upload Authority 和协议不变。Telemetry 复用 FDroneData/Fleet 查询事件，私有纯转换与 Provider 通信代码分离，无新状态所有者。先复现重绑丢失再修复；新增无视图单次提交/状态投影测试，强化 Widget 重建和 Delegate、活动 Job 门禁夹具。Command result loss/re-entry 未改，继续技术债。

Console Release/Debug、UE Editor Development 均通过；Console 六次回归通过；Launcher 32/32；UE Full 245 + Synthetic 1 = 246/246（阶段 A 244 + 新增 2），0 failed / 0 not-run。首轮 UploadDashboard 部分 UI 夹具绕过原门禁导致的失败已记录，并补充活动 binding 锁定/idle 草稿两个场景，未放宽生产门禁。证据目录 `BuildLogs/ArchitectureStageB-20260910`、`UnrealGCS/Saved/Automation/ArchitectureStageB-20260910-*`；详见 [阶段 B 简报](Architecture/STAGE_B_REPORT.md)。未运行真机、未改 Content/协议/配置，未 commit/push，未进入阶段 C。


## 2026-09-10 压缩重构阶段 C 完成

基线 e063c78。复用 UIBridge/Fleet/RoutePreview 公共入口；显式 Route/Upload/Mission 意图和只读状态不要求旧 Widget，沿用原 Coordinator/Assignment/Upload Authority。MDHK 现有反射合同独立，数据查询/控件赋值分离；Command readiness 在原 Provider，结果订阅为 Subsystem 生命周期；旧 UMG 调用原共享执行体与投影。新增生产类 0，无第二份业务状态。生产 +743/-312；UIBridge.cpp 7149→7197，Coordinator.inl 4083→4239，包含新增 API/投影，不宣称机械减行。

Console Release/Debug、UE Editor Development、Console 六次回归成功；Launcher 32/32；UE Full247 + Synthetic1 = 248/248，0失败/未运行（B246 + 新增2）。核心八类能力的 UI Replacement Readiness 自检通过；具体稳定入口/类型、文件清单与限制见 [阶段 C 简报](Architecture/STAGE_C_REPORT.md)。未改资产/协议/配置，未运行真机，未进入 UI 重做，未 commit/push。Command result loss/callback re-entry 仍为技术债，没有冒充已修复。


## 2026-09-10 Final：UI Replacement Readiness 验收

以已确认且未提交的阶段 C 工作树验收（HEAD e063c78）。八类核心能力均可使用 UIBridge/Fleet/RoutePreview 和稳定类型，无底层实现类型 BLOCKER。局部 UI 生命周期/Enabled/Visibility/文本调用点检查未发现业务必须依赖旧 Widget 的阻塞路径；保留的控件读取用于展示与显式输入适配。增强现有 NoWidgetAndRebind 测试的 Vehicle/Session/Connection/Command/Telemetry 查询及重绑保持断言，不新增用例、不修改生产代码。三项生命周期测试通过，确认/冻结目标保持、AutoStart/Upload 单次、旧 Delegate 解绑；这些测试记录中 Command submit 日志为 0。

Console Release/Debug、UE Editor Development、Console 六组回归成功；Launcher32/32；UE Full247 + Synthetic1 = 248/248，0失败/未运行。证据：BuildLogs/UIReplacementFinal-20260910/final-results.json 及对应 UE Automation 报告。结论：READY WITH NON-BLOCKING TECH DEBT。Command result loss/callback re-entry 无新增可复现证据，继续非阻塞技术债；软件证据不替代真机或人工视觉验收。停止架构重构，不开始新 UI，未 commit/push。
