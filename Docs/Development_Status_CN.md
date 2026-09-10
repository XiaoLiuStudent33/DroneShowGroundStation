# DroneGCS Development Status

Status: CURRENT
Authority: 当前实现状态与开发顺序
Last Updated: 2026-09-09
Baseline: `master` / `f115587` + 2026-09-09 本状态文档更新

## 架构治理审计状态（2026-09-09）

- 审计基线：`master` / `a95309d8b37ecb00c779d5c4264aacb86294d8ec`。
- 已完成：一方代码/工程/配置/测试文本扫描、主要调用链与线程/生命周期审计、[十份架构文档](Architecture/00_REPO_OVERVIEW.md)、逐阶段重构方案。
- 未开始：任何源码重构、Phase 0 构建与行为基线、新测试执行、资源改动、真机动作。等待用户确认方案后才进入 Phase 0。
- 重要现状差异：默认 RouteSlot 为 NumericDroneID（零基），已确认 NPZ Mapping 可拥有上传目标集合；已有 runtime MAVFTP config binding；Hover 工作流有每活动 Session 线程。下文旧减一映射/仅选择集合描述不是当前源码最终依据，详见 [当前运行链](Architecture/01_CURRENT_ARCHITECTURE.md)。
- 当前测试结论仍是此前记录；本轮静态审计不增加新的 PASS 或硬件证据。疑似结果丢失、回调重入、确认目标与无界缓存问题仅在 [风险文档](Architecture/08_BEHAVIOR_RISKS.md) 记录。

## 1. DONE / IMPLEMENTED

基础 Production 能力：

- UE 5.5.4 frontend / visualization / operator workflow 与 ConsoleGCS backend 边界。
- ArduPilot/APM MAVLink，多 Remote Endpoint discovery、Session、telemetry、command 与 ACK。
- `Remote IP + UDP Source Port` Fleet identity；同 endpoint 恢复原 Session/DroneID。
- DSP-GCS localhost UDP、1000-drone snapshot/chunk/reassembly。
- Fleet UI、Cesium、one Actor / one Niagara System、TopDown、Marquee、LED preview/command。
- Native NPZ Route Preview、Playback、Scrub、Route Import。

Route Upload 阶段：

| 任务域 | 状态 |
|---|---|
| Phase 2A Live + Route Overlay | IMPLEMENTED |
| Phase 2B RouteAssignment / Freeze / Revision | IMPLEMENTED |
| Phase 2C Artifact Workspace / Manifest / READY | IMPLEMENTED |
| Phase 2D Waypoint Generator | IMPLEMENTED |
| Phase 2E Waypoint Validation | IMPLEMENTED |
| Phase 2F Console Parser / Mission Encoder | IMPLEMENTED |
| Phase 2G DSP Upload Job Protocol | IMPLEMENTED |
| RouteUploadManager / 并发、取消、重试、状态 | IMPLEMENTED |
| UI-3C 上传操作按钮与状态展示 | IMPLEMENTED |
| Phase 2I-C.3.E.0 Selected Drone 自动分配与 Start 事务 | IMPLEMENTED |
| Phase 2I-C.3.E.0.1 Console Render Integrity | IMPLEMENTED |
| Phase 2I-C.3.E.0.2 Console Status Colors / Fleet Select-All | IMPLEMENTED |
| Phase 2I-C.3.E.1 Dynamic-N MAVFTP Concurrency / Isolation | SOFTWARE GATE PASS；E.2 HARDWARE PENDING |
| MAVFTP 9100/4000 三段生产上传与最终验证 | DONE |
| MissionTransport abstraction | DONE |
| MissionFtpCompiler | DONE |
| MAVFTP Writer Core | DONE |
| MAVFTP Hardware Harness / Quick Test | DONE |
| UE + Console 联合 payload 构建与 Shipping 暂存 | DONE |
| 可见完整控制 Console 的联合启动和安全退出 | DONE |
| NSIS V1.0.0 安装程序与 SHA-256 产物 | DONE |
| 无源码/无 VS/无 UE 编辑器电脑安装与启动 | USER-CONFIRMED PASS |

E.0.2 冻结结果：Interactive Dashboard 在单 Renderer frame 中为 Total/Online/Offline 数字及 ONLINE/OFFLINE token 写 Win32 attributes；Linear 输出保持纯文本。Fleet Select-All 复用 `SelectedCommandDroneIDs`，作用于全 Fleet Online DroneID 精确集合，支持跨页、上下线重算、一次批量广播和 active AutoStart/Upload mutation gate；未改动 Route Assignment、MAVFTP、DSP DTO 或 UE Asset。

当前生产上传链：

```text
UE Route Workflow
-> SelectedCommandDroneIDs
-> canonical DroneID -> RouteSlot (`NumericDroneID - 1`，临时 E.0 规则)
-> explicit Working Assignment -> Freeze(AllowDegraded) -> Console Accepted
-> DSP-GCS Upload Job
-> ConsoleUploadJobAuthority
-> RouteUploadManager
-> MissionPreparationWorker
-> MAVFTPMissionTransport
-> DroneSession endpoint-bound MavFtpClient
-> shared MAVLink sender/socket
-> ArduPilot/APM
```

生产 Route Upload 已冻结为 MAVFTP-only；旧 Transport 的源码、factory 分支、CLI、INI 和工程项已删除，失败不会触发协议回退。

## 2. CURRENT / NEXT

### 新版机群 UI、实时遥测与操作命令（2026-09-07～09-09）

- 新 `WBP_GCS_FleetPanel_V2` + `WBP_GCS_FleetRow` + `BP_FleetRowData` 已成为唯一机群列表，旧 `WBP_GCS_FleetPanel` 与旧 Row C++ 已删除。用户已确认新列表现场测试通过。
- 行复选框只负责 Command Selection；从 ID 列开始的独立详情按钮只更新 Detail Selection。选择无人机不再自动显示 `WBP_GCS_DroneDetail`。
- `BTN_Menu` 已解除 Fleet/Detail 可见性职责；`BTN_Fleet` 与 `BTN_Control` 分别独立控制 `CHILD_FleetPanel` 与 `CHILD_DroneDetail`。
- #187 MDHK RGB 已进入标准灯光状态；Visualization 以每机两个真实 GPS 样本做插值，场景颜色刷新使用真实遥测状态。NPZ 预览和真实数字孪生仍是明确分离的来源。
- 授时倒计时到零、执行舞步均不会自动播放 NPZ。底部 Playback 是 NPZ 播放的唯一显式入口。
- HoverTest 已调整为自主工作流：先请求 GUIDED，再继续解锁/起飞；不再由 MANUAL 门禁直接拒绝。
- 已接入返航、原地降落、暂停、前进、倒退、速度、加锁、拉烟、停止/恢复遥测、空中关灯/恢复灯光、唤醒、待机、重启、关机。前进/倒退只有暂停后可用；恢复遥测为消息 `1/24/33/30/187` 的 1 Hz，并等待新鲜 #187 确认。
- 拉烟是 relay 0 的一秒 ON/OFF 脉冲；关机继续兼容旧 BGC2 的 relay 1 latch。待机/重启/关机禁止 armed 状态执行；关机保留二次确认。
- 遥测筛选支持条件和预设，但筛选集合不自动写入 `SelectedCommandDroneIDs`。

当前下一步：按新的明确需求继续开发或进行受监督真机复验。不要重新实现已经通过的新机群列表，也不要把 `Drone001`/`Drone007` 的物理关机问题扩大为 UE 批量命令故障。

### 2026-09-09 真机硬件边界

- 同一批量关机操作中，8 架里的 6 架正常离线；`Drone001`、`Drone007` 仍在线。
- 两机均对 `MAV_CMD_DO_SET_RELAY` 返回 `MAV_RESULT_ACCEPTED`，证明当前 UE -> DSP-GCS -> Console -> MAVLink target/ACK 链已完成；ACK 不证明继电器输出或断电板物理动作。
- 用户已确认按硬件/继电器配置差异暂时跳过。本阶段不修改关机协议或软件实现。
- 备用机/补点飞行仍是 DEFERRED；后续设计时再定义 Link Lost、替补选择、航线重分配与状态接管合同。

### 室内普通上锁误拦截修复（2026-09-04 工作树）

- 现场截图：ARM 收到 `MAV_RESULT_ACCEPTED`，随后 DISARM 被 Console 以 `landing not confirmed` 拒绝，未发送给飞控。旧判断把来自 GPS 3D Fix 的 AMSL `altitudeValid` 与相对高度 `relativeAltitude` 混用，不能代表是否着陆。
- `DroneSession` 不再用 GPS 定位或相对起飞点高度推断着陆以拦截普通上锁；普通 DISARM 始终使用 MAV_CMD 400、param1=0、param2=0，由目标飞控执行自身安全判断。保留 receive-only、真实命令开关、在线检查、Endpoint 路由及 ACK/拒绝/超时处理；不新增强制上锁，不改飞控参数，不改变 GPS 遥测语义。
- 普通上锁的实际允许条件取决于飞控固件、模式和配置，不能宣称所有空中上锁都会被固件拒绝；这不是新的紧急停机功能。ACK 接受也不等于已验证停桨，仍须核对 HEARTBEAT armed 状态及现场电机状态。
- 新增 `ConsoleGCSTests.exe --disarm-tests` 内存捕获专项，覆盖无 GPS、GPS 地面、相对高度非零、ACK 接受/拒绝/缺失、错误 Endpoint/命令 ACK、普通重试与禁用边界。真机地面复测尚待操作员完成，旧 1.0.0 安装包未更新。
- 验证：Console Debug/Release x64 构建及两配置上锁专项 PASS。此前 MissionUpload fake production chain 仍使用已淘汰的 `Drone001–Drone005` 测试编号，和当前 canonical `Drone000–Drone004` 不一致；2026-09-09 已修正夹具编号，完整 Console 回归恢复 PASS。该修复只更新测试夹具，不改变 Production Route Upload 合同。

### Route Upload 列表稳定性修复（2026-09-03 工作树）

- 修复同一 Upload Job 的 `snapshotToken` 轮换导致 ListView 周期性清空/重建：ListItem 改为稳定 `UploadJobID + RouteSlotIndex` 身份，保留现有成员并原位刷新进度。
- 分页恢复期间保留同 Job 的行，明确显示同步中；不显示半份分页为完整结果，不将失效进度标为当前值。
- Editor Development 构建成功；Route Workflow + DSP Upload Job 合并回归 24/24 PASS，含真实 Row 的离屏生命周期与进度更新测试，详细证据见 `Test_Architecture_CN.md`。本项为 UE C++ 展示层修复，未改 Console 上传、DSP 分页安全校验、UE 资源或配置。既有 V3 / 1.0.0 安装包未重新生成；实际上传画面与后续新 Shipping 包仍需操作员验收。

2026-09-03 列表修复后的现场补查发现：手动无参数启动 Console 的默认 `gcs-config.ini` 未配置 `artifact_workspace_root`，不同于现有 `gcs-config-upload.ini`；截图返回的是 Artifact 校验拒绝，不是虚拟机 MAVFTP 上传阶段失败。UE 又把已派发的 Start 预览显示为 `Upload Job Started`，未投影后续拒绝。本次补修仅调整 UE 展示，改为等待/启动失败/结果未知、无 Job 统计 `--` 和完整错误 tooltip；不修改或切换 Console 配置，不启用实际上传权限。验收必须区分“启动校验拒绝”与“任务执行后的逐机失败”；现场上传和安装包仍需单独验收。

补充测试还修复了“已绑定 Start、第一份任务快照丢失后未进入恢复查询”的 UE 状态镜像分支，仅重新查询权威状态，不重发 Start；明确校验拒绝仍保留为错误。最终 Editor Development 构建成功，Route Workflow + DSP Upload Job **25/25 PASS**，证据见 `Test_Architecture_CN.md` 的 Start 拒绝补充回归；尚未由用户复验本次编辑器现场画面。

当前完成点：

```text
Release 1 — UE + Console 联合软件包与 NSIS V1.0.0 安装程序
```

本轮已完成：

- Game Target PreBuildSteps 编译 Release x64 Console 与联合启动器，失败阻断 UE 打包。
- RuntimeDependencies / NonUFS 只暂存严格 allow-list payload，不包含源码、测试程序、PDB 或开发机个人路径。
- 联合启动器预检 payload、配置、进程、端口和本机 `192.168.1.254`，先启动可见 Console，等待 Ready 后再启动 Shipping UE。
- 完整控制模板启用 `real_command_enabled`、`dspgcs_command_enabled` 与 `route_upload_enabled`；既有目标、5 米高度、ACK 和任务状态保护不变。
- 安装版配置、日志、UE Saved 和 Artifact Workspace 迁入 `%LOCALAPPDATA%/DroneGCS`；安装目录不承载可变业务数据。
- NSIS 3.12 无警告生成 `DroneGCS-Setup-1.0.0-x64.exe` 与 SHA-256 文件；另一台电脑安装与启动由用户确认通过。

下一阶段：

- 明确记录异机非飞行控制、飞行、航线上传、升级覆盖和卸载各自的验收范围，避免把“安装通过”扩大为全部业务通过。
- 正式对外发布前配置代码签名、发布版本规则和发布归档。
- 继续保留 E.2 多机硬件规模验证；随后再决定 E.3 稳定身份方案。

Route/MAVFTP 已冻结：

- `segment_items` 表示 Wire Mission Items，包括 Reserved Home `seq 0`。
- 复用现有 `MissionFtpCompiler` 的唯一 deterministic segment plan，不创建第二套 planner。
- MAVFTP 参数、Quick Test、retry/timeout、逐段日志与 metrics 的生产接线。
- 真实飞控 9100/4000 三段 `CLEAR + NO_CLEAR` 与 Count/Home/First/Last 验证。
- 正常 UI Start 已不依赖手工 `RouteAssignment.BindExplicit`；该 Cmd 只保留诊断用途。
- E.1 已取消独立 Deployment Target cap：UE 已 Apply 的 `1..50` 值在 Start 时冻结并进入现有 DTO；Console 严格拒绝非法值，实际准入取 DTO 与 matched targets 的最小值。
- E.0.1 已冻结 Console 单一 Renderer Authority；真实 Console 为 Dashboard，Pipe/File/Tee/CI 为 Linear，业务线程不再直接参与物理绘制。
- 自动事务在 Console Accepted 后重新校验 Selection/Fleet/endpoint/Artifact/Manifest/Route record，并且每个事务最多发送一次 Start。

## 3. NOT STARTED

- E.2 多机硬件规模验证与调度冻结。
- 新版 UI/命令改动对应的下一版 Shipping payload 与安装程序重建、异机复验。

## 4. DEFERRED

- 10000-drone chunked/sharded Route Store。
- Backup Drone Replacement、故障补点、冲突预测、120-degree rejoin rule。
- E.3 Console 重启后的 DroneID 发现顺序/稳定身份风险。
- `Drone001`、`Drone007` relay-1 已 ACK 但未物理断电的硬件/参数排查。

## 5. FIRMWARE UNKNOWN

- 真实 Mission execution timing。
- custom firmware RGB waypoint execution。
- time-control message behavior。
- custom `MAV_CMD_USER` behavior。
- Mission 执行态的远端完整性观测。

`All Write ACKed != Mission Ready`。

## 6. USER-CONFIRMED HARDWARE EVIDENCE

用户确认的真实生产证据：

```text
ArtifactItems=9100 / WireMissionItems=9101
SegmentCapacity=4000 / SegmentCount=3
Count/Home/First/Last=PASS
Target=Succeeded / Job ElapsedMs=8829
```

该证据证明当前 MAVFTP 生产链的 9100/4000 用例，不代表任意飞控或任意 Mission 大小的普遍上限。

## 7. 当前验证快照

2026-08-25 已使用当前 checkout 完成 Release x64 Solution Build（0 errors、12 warnings、ExitCode 0），随后运行：

```text
--drone-id-tests = PASS
--artifact-v2-consumer-tests = PASS
--mission-tests = PASS
--mission-upload-tests = PASS
--upload-job-tests = PASS
--mavftp-probe-tests = PASS
Full ConsoleGCSTests = PASS
--console-render-tests = PASS (连续 20/20)
```

E.1 Console Release Build PASS；动态 N 覆盖 `1/2/6/16/25/50`，公平 permit 专项连续 20/20 PASS，Debug/assert-enabled 专项连续 20/20 PASS，完整 Console 回归连续 3/3 PASS。Endpoint ingress 隔离 Fixture 使用相同 SYSID 和相同 ACK identity、不同 Remote endpoint，验证 ACK 不串包。该软件证据不替代 E.2 真实多机硬件验收。

UE Development/DebugGame Editor Build 均为 PASS；`DroneGCS.Data.DSPGCSUploadJob` 3/3、`DroneGCS.UI.RouteWorkflow` 17/17 PASS，覆盖 Applied 并发、AutoStart 快照、View persistence 与 stale gate。本轮没有运行真实飞控上传。

2026-09-03 联合交付证据：Console Release x64、Launcher `/W4 /WX`、Console Render/Runtime Config、隔离回环 Smoke 32 项、真实 V3 Shipping 输入与 payload manifest 校验、Launcher `--check`、NSIS 3.12 无警告编译全部 PASS。安装程序为 297422854 字节，SHA-256 为 `b236c8a32708c686999009d36909b81ea5034b0b852b6610ac8f1bde1874b99f`，代码签名状态为未签名。

用户于 2026-09-03 确认安装程序已在另一台电脑完成安装并通过启动测试。该项标记为 `USER-CONFIRMED PASS`，仓库没有对应异机自动化日志；升级、卸载以及全部真机控制仍需按各自验收项记录。

当前 Artifact 样本位于 `UnrealGCS/Saved/RouteArtifacts/Current/manifest.json`：Schema 2、9100 frames、1000 RouteSlots、1000 route records；`Saved` 样本不是应提交的 Production 源码。

## 8. 阅读规则

1. [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)
2. [Code_Architecture_CN.md](Code_Architecture_CN.md)
3. 本文件
4. [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)
5. Audit 与 `History/` 仅作证据

## 2026-09-09 架构治理Phase 0

Phase 0已完成并记录局限，见 [Architecture/BASELINE.md](Architecture/BASELINE.md)。Console Release/Debug完整重建、UE Editor Development/DebugGame、Launcher Release成功；最终本地自动化通过，首轮配置失败与现存警告保留。只新增文档/证据/只读比较helper，未改生产行为；未进入Phase1。

## 2026-09-09 架构治理Phase 1

Phase1限定范围已完成；Phase2未开始。核心重复组装与两项低风险依赖收窄通过编译和Phase0完整可用自动回归。没有新增类/接口/状态或生产行为变化；未提交。逐文件理由、保留债务与验证见 [Architecture/17_PHASE1_REPORT.md](Architecture/17_PHASE1_REPORT.md)。

## 2026-09-10 阶段 A：Command 稳定合同

已完成现有结果结构的稳定状态补充、UI字符串判断替换、Private DSP转换边界及契约测试。原用户可见文本不变；阶段结束构建/回归通过，UE244/244、Launcher32/32。未重做全仓审计/架构方案，未进入B，未commit/push。构建日志、逐测试结果及等价检查位于BuildLogs/ArchitectureStageA-20260910。


## 2026-09-10 压缩重构阶段 B 完成

基线 `4820a11`。既有 Coordinator 改为由 UIBridge Subsystem 初始化并稳定持有；Widget 重绑定仅 DetachView/绑定，不清除 Confirmed Mapping、AutoStart 事务与目标模型。命令选择/详情选择继续分别由 UIBridge/Fleet 持有；Assignment/Upload Authority 和协议不变。Telemetry 复用 FDroneData/Fleet 查询事件，私有纯转换与 Provider 通信代码分离，无新状态所有者。先复现重绑丢失再修复；新增无视图单次提交/状态投影测试，强化 Widget 重建和 Delegate、活动 Job 门禁夹具。Command result loss/re-entry 未改，继续技术债。

Console Release/Debug、UE Editor Development 均通过；Console 六次回归通过；Launcher 32/32；UE Full 245 + Synthetic 1 = 246/246（阶段 A 244 + 新增 2），0 failed / 0 not-run。首轮 UploadDashboard 部分 UI 夹具绕过原门禁导致的失败已记录，并补充活动 binding 锁定/idle 草稿两个场景，未放宽生产门禁。证据目录 `BuildLogs/ArchitectureStageB-20260910`、`UnrealGCS/Saved/Automation/ArchitectureStageB-20260910-*`；详见 [阶段 B 简报](Architecture/STAGE_B_REPORT.md)。未运行真机、未改 Content/协议/配置，未 commit/push，未进入阶段 C。


## 2026-09-10 压缩重构阶段 C 完成

基线 e063c78。复用 UIBridge/Fleet/RoutePreview 公共入口；显式 Route/Upload/Mission 意图和只读状态不要求旧 Widget，沿用原 Coordinator/Assignment/Upload Authority。MDHK 现有反射合同独立，数据查询/控件赋值分离；Command readiness 在原 Provider，结果订阅为 Subsystem 生命周期；旧 UMG 调用原共享执行体与投影。新增生产类 0，无第二份业务状态。生产 +743/-312；UIBridge.cpp 7149→7197，Coordinator.inl 4083→4239，包含新增 API/投影，不宣称机械减行。

Console Release/Debug、UE Editor Development、Console 六次回归成功；Launcher 32/32；UE Full247 + Synthetic1 = 248/248，0失败/未运行（B246 + 新增2）。核心八类能力的 UI Replacement Readiness 自检通过；具体稳定入口/类型、文件清单与限制见 [阶段 C 简报](Architecture/STAGE_C_REPORT.md)。未改资产/协议/配置，未运行真机，未进入 UI 重做，未 commit/push。Command result loss/callback re-entry 仍为技术债，没有冒充已修复。


## 2026-09-10 Final：UI Replacement Readiness 验收

以已确认且未提交的阶段 C 工作树验收（HEAD e063c78）。八类核心能力均可使用 UIBridge/Fleet/RoutePreview 和稳定类型，无底层实现类型 BLOCKER。局部 UI 生命周期/Enabled/Visibility/文本调用点检查未发现业务必须依赖旧 Widget 的阻塞路径；保留的控件读取用于展示与显式输入适配。增强现有 NoWidgetAndRebind 测试的 Vehicle/Session/Connection/Command/Telemetry 查询及重绑保持断言，不新增用例、不修改生产代码。三项生命周期测试通过，确认/冻结目标保持、AutoStart/Upload 单次、旧 Delegate 解绑；这些测试记录中 Command submit 日志为 0。

Console Release/Debug、UE Editor Development、Console 六组回归成功；Launcher32/32；UE Full247 + Synthetic1 = 248/248，0失败/未运行。证据：BuildLogs/UIReplacementFinal-20260910/final-results.json 及对应 UE Automation 报告。结论：READY WITH NON-BLOCKING TECH DEBT。Command result loss/callback re-entry 无新增可复现证据，继续非阻塞技术债；软件证据不替代真机或人工视觉验收。停止架构重构，不开始新 UI，未 commit/push。
