# 分阶段施工路线图

Status: Phase 0 COMPLETE WITH DOCUMENTED LIMITATIONS / Phase 1–10 NOT STARTED；基线 `a95309d8`；2026-09-09。
**用户已授权且仅执行Phase 0；结果见 [BASELINE.md](BASELINE.md)。Phase 1未开始。** 以下为原施工配方；Phase 0实际执行命令、测试配置差异和结果以基线10/11为准。每阶段以先决阶段已合并且工作树保护检查完成为起点；“独立”指单一小改、可单独编译/测试/commit/revert，不表示可乱序同时改Owner。

## 1. 共用编译与测试配方

在仓库根目录、已安装VS2022/v143与UE5.5.4的开发环境执行。使用VS Developer PowerShell解析MSBuild；UE路径通过本机已安装Engine确认，下面$EngineRoot是待填环境值，不能猜安装盘符。失败即停，不自动放宽开关。

```powershell
# C：Console Release；D：Debug（分别执行并检查退出码）
MSBuild.exe .\ConsoleGCS\DroneGCS.sln /m /p:Configuration=Release /p:Platform=x64
MSBuild.exe .\ConsoleGCS\DroneGCS.sln /m /p:Configuration=Debug /p:Platform=x64
# U：Editor Development；UD：Editor DebugGame
& "$EngineRoot\Engine\Build\BatchFiles\Build.bat" DroneShowPlatform1Editor Win64 Development "-Project=$PWD\UnrealGCS\DroneShowPlatform1.uproject" -WaitMutex
& "$EngineRoot\Engine\Build\BatchFiles\Build.bat" DroneShowPlatform1Editor Win64 DebugGame "-Project=$PWD\UnrealGCS\DroneShowPlatform1.uproject" -WaitMutex
# L：Launcher，仅构建
MSBuild.exe .\Tools\Launcher\DroneGCSLauncher.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

Console测试从ConsoleGCS目录运行编译后的ConsoleGCSTests.exe；完整suite无参数，专项以ConsolePipelineTests.cpp的main为准：`--disarm-tests`、`--drone-id-tests`、`--mission-tests`、`--mission-upload-tests`、`--upload-job-tests`、`--mavftp-probe-tests`、`--mavftp-concurrency-tests`、`--console-render-tests`、`--runtime-config-tests`。Command/DSP专项没有在本方案臆造新flag，使用full suite包含RunCommandTests/RunDSPCommandTests。

UE配方 UT（匹配阶段替换测试前缀和报告目录，必须使用新目录）：

```powershell
& "$EngineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\UnrealGCS\DroneShowPlatform1.uproject" -unattended -NullRHI -nosound '-ini:Game:[DSPGCS.ProductionTelemetry]:Enabled=False' '-ini:Game:[DSPGCS.ProductionCommand]:Enabled=False' '-ExecCmds=Automation RunTests DroneGCS.UI' '-TestExit=Automation Test Queue Empty' "-ReportExportPath=$PWD\UnrealGCS\Saved\Automation\Architecture-PhaseX"
```

Phase 0发现上面的全局Enabled=False配方不适用于依赖启用Provider的全量测试；详细运行方法和实际端口证据见[11_TEST_BASELINE.md](11_TEST_BASELINE.md)，不得仅凭exit=0判通过。DefaultGame.ini的Developer Mock/Acceptance应保持False；不启动部署配置的Console控制程序。未来测试只用内存捕获或明确隔离回环。验收同时检查退出码、报告failed/notRun、日志错误与预期拒绝警告；不能只看“构建成功”。测试产物Saved是未来Phase0输出，不纳入本轮审计范围或源码提交。

发布配方 P：使用UE Game Shipping/UAT正常构建到新空staging目录（不能skip build），触发既有PreparePayload；按Tools/Launcher/README和Tools/Installer/README验证manifest，再用BuildInstaller.ps1 -ValidateOnly。Launcher --check是只读预检；不要用--no-dialogs冒充预检，它会运行程序。制作/安装新包与Smoke执行安排在用户确认的阶段，不能当本轮工作。

每阶段更新PROJECT_HANDOFF_CN.md、Development_Status_CN.md；改变测试资产/入口时更新Test_Architecture_CN.md。单阶段独立commit，只stage本阶段文件；回滚用`git revert <该阶段commit>`，冲突时人工保留用户更改，禁止reset --hard/checkout丢弃文件。若回滚早期阶段有后续依赖，按逆序回滚后续提交；不承诺任意乱序revert无冲突。

## 2. 阶段表

## Phase 0 — 行为与构建基线

| 项目 | 说明 |
|---|---|
| 目的 | 建立可复验行为/构建基线 |
| 当前问题 | 已完成HEAD构建/测试基线；零基/confirmed/runtime cap/线程事实已记录。首轮3项配置相关失败与复跑PASS均保留，未修业务代码。 |
| 修改范围 | 只形成基线与必要特征化测试，不改生产行为。先记录反射符号、资源路径、命令packet fixture、配置/协议hash及已有测试结果。 |
| 涉及文件 | ConsoleGCS/tests/CommandTests.cpp；DSPCommandTests.cpp；DroneLogicalIdTests.cpp；MissionUploadTests.cpp；UE Private/Tests/GCSSelectionStateTests.cpp、GCSRouteWorkflowCoordinatorTests.inl、DSPGCSTelemetryLinkTests.cpp；Docs测试/状态文档。 |
| 本阶段产物 | BASELINE.md索引、10–16专题、PHASE0_EVIDENCE.json、verify_phase0.py；未修改现有生产或测试源码。 |
| 预期删除文件 | 无。 |
| 行为是否变化 | 无；发现失败先记录，修Bug另案。 |
| 风险 | 中：测试误启生产链、误把历史fixture当真机。 |
| 编译方法 | C+D+U+UD+L；先核对测试程序依赖/安全隔离。 |
| 测试方法 | Console完整suite及独立 --runtime-config-tests；UE DroneGCS.；新case覆盖零基/confirmed选择变化、重复Bind/关闭重开、结果丢失/重入与特殊命令完成语义，未复現也如实记录。 |
| 完成标准 | 可追溯HEAD/工具链/输出/通过失败；所有后续触及行为已有明确基线，任何未通过项单独列出，不默认为可修。 |
| 回滚方式 | 独立提交Phase 0（页/类子步骤各自commit）；按本文件共用规则revert该提交，恢复原调用入口及对应测试。运行期Artifact/用户配置不进入源码回滚。 |

## Phase 1 — 纯转换重复清理

| 项目 | 说明 |
|---|---|
| 目的 | 减少低风险重复 |
| 当前问题 | CommandReceiver真实/模拟分支重复复制参数，局部helper增加漏字段风险。 |
| 修改范围 | 只提取相同参数转换，不合并门禁/known-ID/日志/发送分支；一次commit仅处理一个转换族。 |
| 涉及文件 | ConsoleGCS/DSPGCSProductionCommandReceiver.cpp；ConsoleGCS/tests/DSPCommandTests.cpp。 |
| 预期新增文件 | 默认无；优先匿名namespace纯函数。 |
| 预期删除文件 | 无。 |
| 行为是否变化 | 无，DTO字段和拒绝条件逐例一致。 |
| 风险 | 低；漏字段/默认值。 |
| 编译方法 | C+D。 |
| 测试方法 | Console完整suite，DSP round-trip与高级命令捕获对照。 |
| 完成标准 | 转换后所有25类动作/参数与基线一致，代码重复减少。 |
| 回滚方式 | 独立提交Phase 1（页/类子步骤各自commit）；按本文件共用规则revert该提交，恢复原调用入口及对应测试。运行期Artifact/用户配置不进入源码回滚。 |

## Phase 2 — UE高层类型与查询边界

| 项目 | 说明 |
|---|---|
| 目的 | 提供不暴露wire的读取类型 |
| 当前问题 | Public Provider头暴露DSP，FilterModel依赖整Provider，UI查询和wire类型耦合。 |
| 修改范围 | 增加最小协议无关查询投影并隐藏新API的wire依赖；旧反射结构与Provider函数保留兼容，不增第二状态源。 |
| 涉及文件 | UE Public/DSPGCSProductionTelemetryProviderSubsystem.h、DSPGCSProductionCommandTypes.h；Private/GCSTelemetryFilterModel.h/.cpp；Public/FleetManagerSubsystem.h。 |
| 预期新增文件 | 候选Public/GCSApplicationTypes.h（只新增缺少的高层非重复类型）、Private/GCSStateProjection.h/.cpp；最终以提取收益决定。 |
| 预期删除文件 | 无；不删旧反射类型。 |
| 行为是否变化 | 无，不补armed/姿态/速度功能，不改invalid显示。 |
| 风险 | 中；有效性/source时间戳、反射兼容。 |
| 编译方法 | U+UD。 |
| 测试方法 | UT DroneGCS.UI.TelemetryFilter、DroneGCS.UI.Selection及TelemetryLink/Chunk；类型转换fixture。 |
| 完成标准 | 新查询消费者不include DSP/MAVLink/socket；旧类型和界面行为保留。 |
| 回滚方式 | 独立提交Phase 2（页/类子步骤各自commit）；按本文件共用规则revert该提交，恢复原调用入口及对应测试。运行期Artifact/用户配置不进入源码回滚。 |

## Phase 3 — 命令应用入口与选择代理

| 项目 | 说明 |
|---|---|
| 目的 | 使操作策略不依赖旧Widget |
| 当前问题 | Bridge持有暂停、确认、结果聚合、Hover工作流模式；新UI会重复这些规则。 |
| 修改范围 | 提取无Widget操作策略与结果语义投影；稳定API先代理Bridge唯一选择集合，旧Handler转发。不新增Tracker，不改请求次数。 |
| 涉及文件 | UE Private/GCSUIBridgeSubsystem.cpp；Public/GCSUIBridgeSubsystem.h；Private/DSPGCSProductionCommandProviderSubsystem.cpp；Private/Tests/GCSSelectionStateTests.cpp。 |
| 预期新增文件 | 候选Public/GCSOperatorActions.h、Private/GCSOperatorActions.cpp；以一个应用对象承载原操作状态，不按命令创建Service。 |
| 预期删除文件 | 无。 |
| 行为是否变化 | 无；确认5秒和当前目标语义也保持，任何加固另案。 |
| 风险 | 高；安全gate次序、Sent/Accepted、目标快照、回调重入。 |
| 编译方法 | U+UD；触及wire映射则C。 |
| 测试方法 | UT Selection/TimeControl；Console完整命令suite；无WBP内存捕获调用者对照；关机/暂停/空选择/重复提交。 |
| 完成标准 | 旧UI和无Widget调用输出相同，唯一选择/操作Owner可辨识，零真机发包。 |
| 回滚方式 | 独立提交Phase 3（页/类子步骤各自commit）；按本文件共用规则revert该提交，恢复原调用入口及对应测试。运行期Artifact/用户配置不进入源码回滚。 |

## Phase 4 — Route协调器独立实现单元

| 项目 | 说明 |
|---|---|
| 目的 | 降低翻译单元耦合 |
| 当前问题 | 4054行inl依赖Bridge翻译单元，业务与Widget耦合。 |
| 修改范围 | 先只将Coordinator实现编译独立，收敛来自Bridge匿名helper的依赖；不改变类名/成员/状态机；此阶段不顺便拆业务。 |
| 涉及文件 | UE Private/GCSUIBridgeSubsystem.cpp、GCSRouteWorkflowCoordinator.h/.inl、Private/Tests/GCSRouteWorkflowCoordinatorTests.inl。 |
| 预期新增文件 | 候选Private/GCSRouteWorkflowCoordinator.cpp；必要最小私有helper头。 |
| 预期删除文件 | 仅在引用迁移且独立编译通过后删除旧.inl；不删测试。 |
| 行为是否变化 | 无。 |
| 风险 | 中；ODR、匿名helper、测试依赖、UHT。 |
| 编译方法 | U+UD；需要干净编译排除unity掩盖。 |
| 测试方法 | UT DroneGCS.UI.RouteWorkflow、DroneGCS.Data.DSPGCSUploadJob；同一请求字节和数量。 |
| 完成标准 | 独立编译单元通过，旧WBP和反射名零变化，上传状态机diff仅机械提取。 |
| 回滚方式 | 独立提交Phase 4（页/类子步骤各自commit）；按本文件共用规则revert该提交，恢复原调用入口及对应测试。运行期Artifact/用户配置不进入源码回滚。 |

## Phase 5 — Route应用状态与展示分离

| 项目 | 说明 |
|---|---|
| 目的 | 替换View不重建任务 |
| 当前问题 | Coordinator同时拥有confirmed映射/AutoStart和控件生命周期。 |
| 修改范围 | 保留单一AutoStart/Mapping Owner；把Widget投影和反射查找提取为Presenter，应用初始化不以特定Widget存在为前提。 |
| 涉及文件 | UE Private/GCSRouteWorkflowCoordinator.cpp/.h、Public/DSPGCSUploadJobSubsystem.h、Private/DSPGCSUploadJobSubsystem.cpp；RouteWorkflow测试。 |
| 预期新增文件 | 候选Private/GCSRouteWorkflowPresenter.h/.cpp；最少只读应用状态类型。 |
| 预期删除文件 | 无业务Owner删除；旧Handler保留。 |
| 行为是否变化 | 无：confirmed目标、runtime config binding、Freeze/ACK/Start、分页token不变。 |
| 风险 | 高；exactly-once、关闭View、stale/拒绝/首快照恢复。 |
| 编译方法 | U+UD；若仅Presenter仍不需改Console。 |
| 测试方法 | UT RouteWorkflow + UploadJob + Frozen + Assignment；Console --runtime-config-tests；无WBP事务和真实ListView生命周期对照，packet capture零新增Start。 |
| 完成标准 | View可替换且事务连续；旧选择和confirmed分支均一致；行对象稳定。 |
| 回滚方式 | 独立提交Phase 5（页/类子步骤各自commit）；按本文件共用规则revert该提交，恢复原调用入口及对应测试。运行期Artifact/用户配置不进入源码回滚。 |

## Phase 6 — Telemetry投影与通道职责收窄

| 项目 | 说明 |
|---|---|
| 目的 | 让Telemetry读取不控制网络 |
| 当前问题 | Provider做双向通道、表格文本格式化和RouteGpsCapture分发。 |
| 修改范围 | 只提纯遥测/表格格式化，并经稳定查询接口提供；保留原Transport Owner和线程分发。通道独立类只在确有生命周期收益时再单独提案。 |
| 涉及文件 | UE Private/DSPGCSProductionTelemetryProviderSubsystem.cpp；Public同名头；Private/GCSTelemetryFilterModel.*、GCSUIBridgeSubsystem.cpp、GCSServerAdapterSubsystem.cpp。 |
| 预期新增文件 | 复用Phase2 GCSStateProjection；必要Private/GCSTelemetryPresentation.h/.cpp。 |
| 预期删除文件 | 无。 |
| 行为是否变化 | 无；不得改变采样时钟、#24/#187来源、分片和发布顺序。 |
| 风险 | 中高；异步退出、样本重放、MDHK与Fleet快照不同步。 |
| 编译方法 | U+UD。 |
| 测试方法 | UT TelemetryLink/Chunk、RouteGpsCapture、TelemetryFilter、Visualization；销毁/重连特征化。 |
| 完成标准 | 新UI不知provider/raw DTO；原数据有效性/时间戳/呈现逐例相同。 |
| 回滚方式 | 独立提交Phase 6（页/类子步骤各自commit）；按本文件共用规则revert该提交，恢复原调用入口及对应测试。运行期Artifact/用户配置不进入源码回滚。 |

## Phase 7 — 旧UI逐页薄化

| 项目 | 说明 |
|---|---|
| 目的 | 把旧UI变成高层API消费者 |
| 当前问题 | Bridge仍有重复查控件、相机输入、行格式化和面板持久状态。 |
| 修改范围 | Fleet、Detail/Command、Playback、Filter、Route、地图按页小commit转发高层API；先不替换资源，不建立每机Timer/ViewModel Actor。 |
| 涉及文件 | UE Private/GCSUIBridgeSubsystem.cpp、GCSUIWidgetPresenter.*、GCSFleetRowCheckBinding.*、GCSTelemetryFilterController.*；对应UI测试。 |
| 预期新增文件 | 仅必要的每页Presenter.h/.cpp，默认复用现有；每新增文件必须从Bridge移走相应实现。 |
| 预期删除文件 | 只删除已迁移的重复私有helper，不删除反射接口/.uasset。 |
| 行为是否变化 | 无，隐藏保留状态、输入区域、滚动和显示一致。 |
| 风险 | 中；重复绑定、旧Widget残留、GC、World actor失效。 |
| 编译方法 | 每页U；全部完成UD。 |
| 测试方法 | UT FleetList/Selection/Toolbar/TimeControl/TelemetryFilter/RouteWorkflow/Visualization，重新Bind/GC/回收案例。 |
| 完成标准 | 每页可独立commit，Bridge不再决定协议和操作状态；已有现场行为保持。 |
| 回滚方式 | 独立提交Phase 7（页/类子步骤各自commit）；按本文件共用规则revert该提交，恢复原调用入口及对应测试。运行期Artifact/用户配置不进入源码回滚。 |

## Phase 8 — 后端局部职责整理（可延后）

| 项目 | 说明 |
|---|---|
| 目的 | 降低后端维护成本 |
| 当前问题 | Session/App组装过大、日志头/键盘适配扩展编译耦合。 |
| 修改范围 | 逐类提私有策略或输入适配，保留Session ownership/主循环顺序。SafetyConfig和Hover线程调度变化不纳入机械提取。 |
| 涉及文件 | ConsoleGCS/DroneSession.cpp/.h、CommandManager.cpp/.h、ConsoleGCSApplication.cpp、ConsoleUI.h；只按实际子提交选一组。 |
| 预期新增文件 | 候选ConsoleGCS/DroneCommandPolicy.h/.cpp或ConsoleInputAdapter.*，不要同时制造两套策略；必要vcxproj工程项。 |
| 预期删除文件 | 仅已迁移私有代码；HandleConsoleKey需确认后才删除/代理。 |
| 行为是否变化 | 无；输出、gate、timeout、relay时序保持。 |
| 风险 | 中高；构造析构顺序、并发与日志行为。 |
| 编译方法 | C+D+L（Launcher编译共享配置源码）。 |
| 测试方法 | Console完整suite、Render、MAVFTP并发/取消、Session同SYSID隔离；配置与Launcher隔离Smoke在明确安全环境。 |
| 完成标准 | Owner和发送trace不变，变更只涉及单一职责提取。 |
| 回滚方式 | 独立提交Phase 8（页/类子步骤各自commit）；按本文件共用规则revert该提交，恢复原调用入口及对应测试。运行期Artifact/用户配置不进入源码回滚。 |

## Phase 9 — Shared与编译依赖清理

| 项目 | 说明 |
|---|---|
| 目的 | 用编译边界约束架构 |
| 当前问题 | Shared平台实现与协议头混合；UE Public dependencies过宽；存在预留/旧helper候选。 |
| 修改范围 | 先做include/forward声明与负依赖检查；按B类证据决定去未用路径。Shared平台helper如提取保持一份实现和两端合同；不先库化全Shared。 |
| 涉及文件 | UE DroneShowPlatform1.Build.cs及Provider public/private headers；Shared/DSPGCSProtocol与RouteArtifact局部；ConsoleGCS .vcxproj；Tools/Launcher工程（若关联）。 |
| 预期新增文件 | 必要Shared/Platform具体helper及一方边界检查脚本；没有收益则不新增。 |
| 预期删除文件 | 经Phase0反射/clean build确认的未用include/预留声明；wire enum和codec不删。 |
| 行为是否变化 | 无；canonical/digest/wire/lease名字与语义保持。 |
| 风险 | 中；UBT transitive include、ODR、Windows编码。 |
| 编译方法 | C+D+U+UD+L，最后P验证Shipping依赖。 |
| 测试方法 | 完整Console+UE、WPL/ArtifactV2/Frozen/Upload golden fixtures、架构负依赖检查。 |
| 完成标准 | 新应用/Presenter无底层include，生产不依赖测试，Shared无领域状态，字节与hash一致。 |
| 回滚方式 | 独立提交Phase 9（页/类子步骤各自commit）；按本文件共用规则revert该提交，恢复原调用入口及对应测试。运行期Artifact/用户配置不进入源码回滚。 |

## Phase 10 — 架构验收与Legacy收尾

| 项目 | 说明 |
|---|---|
| 目的 | 证明治理目标达到 |
| 当前问题 | 代码提取完成不等于UI可替换和交付可用。 |
| 修改范围 | 验证新UI最小消费者、旧UI兼容、资产/反射/协议清单，按A/B/C最终确认清理；更新全套架构与交接。 |
| 涉及文件 | Docs/Architecture、Docs/PROJECT_HANDOFF_CN.md、Development_Status_CN.md、Test_Architecture_CN.md；相关架构验证测试；不改Content。 |
| 预期新增文件 | Docs/Architecture/VERIFICATION.md（未来证据）；缺少时才加无WBP应用测试。 |
| 预期删除文件 | 默认无；B类没有批准继续保留。 |
| 行为是否变化 | 无。 |
| 风险 | 中；误将软件验证升级真机或旧安装包证据。 |
| 编译方法 | C+D+U+UD+L+P。 |
| 测试方法 | 完整Console/UE；Launcher隔离Smoke32项基线对照；Shipping manifest与Installer ValidateOnly；用户正常窗口复验另记。 |
| 完成标准 | 15项执行摘要可回溯源码/测试；新UI无底层依赖且既有用户行为保留；硬件未验收项明确不勾完成。 |
| 回滚方式 | 独立提交Phase 10（页/类子步骤各自commit）；按本文件共用规则revert该提交，恢复原调用入口及对应测试。运行期Artifact/用户配置不进入源码回滚。 |

## 3. 停止条件与不包含事项

发生基线失败、wire/hash/发包次数变化、反射引用变化、任务目标变化，停止该阶段并记录；不能通过更新expected值吞掉行为变化。发现明确Bug只进入独立问题单，未经批准不修。

UI大改前至少完成0～7的相关入口与无Widget证明。8可推迟；9的负依赖检查应提前建立，重模块调整可推迟。整个计划不包括真机验证、备用机功能、稳定身份方案、Hover线程改写、自动Command重发或资源整体替换。
