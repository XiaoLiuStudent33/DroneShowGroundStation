# Phase 1 完成报告：纯转换与局部依赖收窄

基线：`b3da56f`（docs: establish architecture refactor baseline）；源码行为与Phase0的`a95309d8`一致。2026-09-09。本轮用户授权仅Phase1，禁止自行提交；开始工作树干净。本报告不把Phase2高层DTO/API迁移当成已完成。

## A. 实际范围与路线图关系

路线图Phase1核心是CommandReceiver参数转换去重。附件另授权低风险依赖收窄；本轮只增加两个可编译证明的局部调整：CommandProvider公共头使用前置声明，Sockets/Networking降为Private。未拆UIBridge/Coordinator/TelemetryProvider，未改变任何Authority/事务；不开展后续应用类型/Presenter/Service重构。

## B–C. 修改文件与原因

| 文件 | 修改原因与边界 |
|---|---|
| ConsoleGCS/DSPGCSProductionCommandReceiver.cpp | 真实/模拟分支完全相同的13字段组装提取到匿名namespace纯函数；原分支、校验、日志、Submit位置全部保留 |
| UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSProductionCommandProviderSubsystem.h | private函数声明只需要两个DSP struct声明；移除完整DSPGCSProtocol.h传递包含，显式包含cstdint/vector；generated.h仍为最后include |
| UnrealGCS/Source/DroneShowPlatform1/DroneShowPlatform1.Build.cs | Sockets和Networking仅为Private transport和Private E2E测试使用，不对外传播；不删除模块依赖，不改变实现 |
| Docs/Architecture/17_PHASE1_REPORT.md、PHASE1_EVIDENCE.json | 本报告及构建、测试、字段等价和资源检查证据 |
| Docs/Architecture/07_REFACTOR_ROADMAP.md、09_TECH_DEBT.md | 更新阶段状态，记录本轮未处理的依赖/helper候选 |
| Docs/PROJECT_HANDOFF_CN.md、Development_Status_CN.md、Test_Architecture_CN.md | 同步交接、完成状态与本轮回归结果；不改写Phase0原证据 |

## D–E. 去重与新增内容

唯一新增helper：`ToInternalParameters(const dspgcs::CommandParametersDTO&) -> DroneCommandParameters`，文件内匿名namespace、两个调用点。13字段：takeoffAltitudeMeters、hoverAltitudeMeters、hoverDurationSeconds、customMode、start→startHoverTest、red/green/blue、timeControlMask、scheduledHour/Minute/Second、advancedSpeedMetersPerSecond。保持默认构造、类型与赋值顺序；不增加clamp/解析/转换规则。

没有新增类型定义、class、Manager、Interface、Factory或测试专用生产接口。两个DSP struct只是既有类型的forward declaration。未合并UI/protocol/Console信任边界校验，也未合并enum允许列表。

结构等价检查：从已提交基线取出两份赋值，确认13对字段完全一致；把新helper在两个调用点展开回原形式后，整份Receiver去空白文本与基线完全一致。这独立于测试结果证明没有顺带更改gate/日志/循环/提交次数。现有Command/DSP packet回归继续验证执行链，未新增镜像实现的重复测试。

## F. Public Header前后

之前：CommandProvider公共头直接include完整DSPGCSProtocol.h（wire类型和inline codec）。

之后：仅声明`dspgcs::MessageEnvelope`、`dspgcs::CommandResultBatchDTO`，补齐直接使用的标准库头；cpp原本已有显式DSPGCSProtocol.h，继续在那里使用完整定义。整个UCLASS起的类声明逐字节不变，Blueprint公开API、UPROPERTY、布局/成员、回调类型和函数签名都不改。

仍保留TelemetryProvider的完整DSP定义：有`vector<vector<TelemetryDTO>>`等按值成员；Upload/Frozen/StateMirror同样持有协议值、inline逻辑或默认enum。Shared DSP/Artifact头本身是两端必要合同/算法，不属于可随意隐藏的implementation。强行删除会要求析构/存储边界调整或新的应用合同，不在本阶段做。Shared PublicIncludePaths未降级。

## G. Build.cs依赖

Public从11项减为9项，Private从2项增为4项。

| 模块 | 本轮处理与依据 |
|---|---|
| Sockets | Public→Private；FSocket/SocketSubsystem/Sockets.h只在Private LoopbackTransport与FrozenSync测试使用；Public头与Shared transport接口无UE socket类型 |
| Networking | Public→Private；UdpSocketBuilder/Receiver、IPv4Address/Endpoint同上，无公开头引用 |
| Core/CoreUObject/Engine | 保持Public；Core类型、UObject/Subsystem/Actor公开继承/反射合同 |
| UMG | 保持Public；Widget公开类型/反射契约仍存在，未迁移UI API |
| Slate/SlateCore | 保持Public；UI头保留FReply、ESelectInfo/ETextCommit相关声明，本轮不扩展UI头依赖整理 |
| Niagara | 保持Public；Visualization公开getter/反射成员暴露Niagara对象 |
| InputCore/EnhancedInput | 保持Public；本轮仅验证并调整网络两个模块，输入传播/插件配置依赖未完成独立移除证明，不宣称一定必须Public；作为候选债务保留 |
| CesiumRuntime/EngineSettings | 原有Private不变；zlib及系统库不变 |

源码扫描+Editor编译支持本轮两项移动；未声称已经为所有外部消费者构建独立consumer module，未作全依赖大扫除。

## H. 代码量

生产文件git diff：Receiver +28/-36；CommandProvider.h +8/-1；Build.cs +2/-2；合计 **+38/-39，净减少1行**。数量包含空行与声明。报告/证据文件单独统计，不将生成的测试结果行数当生产复杂度。

## I–J. 行为保护

**未修改生产行为。** 修改只改变纯组装代码组织和编译依赖。Phase0 hash对照仅3个预期生产文件有差异；其余759个源码/配置文件保持原hash，69个资源保持Git blob。不是把预期hash差异误判成回归。

| 对比项 | 证据/结论 |
|---|---|
| RouteSlot | 默认NumericDroneID零基；身份helper未改，ID/Coordinator回归通过 |
| DroneID / Endpoint | Remote IP+Source Port Authority与进程内编号未改；Shared/Session/Manager未改 |
| confirmed mapping / targets | Coordinator/Freeze/AutoStart源码未改；保持confirmed Entries优先于后来UI勾选 |
| Command | 25枚举/ID不变；13字段完全等价；Submit仍位于原两分支，各只调用一次 |
| ACK | Dispatcher/Session/结果转换token未改；ACCEPTED不当物理执行 |
| Timeout / Retry | 所有原常量和重试代码hash未变；不合并跨层预算 |
| Upload | manager/preparation/MAVFTP/wire/verification未改；不改变Freeze/Accepted/Start去重 |
| Telemetry | Provider/Parser/Adapter/Visualization源码未改；GPS/RGB与NPZ来源分离 |
| Blueprint | UCLASS起完整声明相同、69资产不变；本轮未进行UI美化/资源变更 |

## K–N. 构建与测试

构建命令沿用10_BUILD_BASELINE的MSBuild Release/Debug与Editor Development（本轮Build而非Rebuild）。工具链仍为VS2022 v143/MSVC14.44、UE5.5.4；完整命令/墙钟/结果见PHASE1_EVIDENCE.json和`BuildLogs/ArchitecturePhase1-20260909`。

Console两配置成功，分别18 warnings/0 errors（本次增量编译触发的既有C4819/C4244；不能与Phase0完整重建500条直接比较）。UE Development成功，11 actions，仍有非preferred MSVC警告。没有为变绿修改业务代码。

本轮重跑Phase0可用安全自动集：Console full + disarm/runtime-config/concurrency/artifact10K/mission-benchmark；UE DroneGCS. + Synthetic10Kx16；Launcher Smoke。全量UE启用依赖的Provider并保留SafeTestMode，使用Entry地图；启动前确认无生产进程且12001/12002未占用；每个报告使用新目录；Synthetic关闭Provider。Console与UE/Launcher按回环端口隔离顺序执行，没有真机操作。UE进程exit=0之外另检查报告failed/notRun。

实测结果见文末表；逐项UE名称/耗时/诊断保存在 [PHASE1_EVIDENCE.json](PHASE1_EVIDENCE.json)。

## O. Git摘要与交付

开始master/b3da56f工作树干净。结束只有上述3个生产文件及文档/证据修改；`git diff --check`通过，无staged/commit。原文件编码保持不变；没有全项目格式化、Content移动、资产编辑或未确认旧代码删除。

## P. 发现但未处理

1. Telemetry/Upload/Frozen公共DSP值类型仍是当前必要存储/调用合同，后续API治理另做。
2. InputCore/EnhancedInput及MAVLink PublicIncludePath为后续独立编译证明候选，本次未一并移除。
3. 控件查找/格式化跨Bridge/Coordinator/Filter的候选尚未逐一证明Widget根、查找顺序、空值/日志行为完全相同；本轮不机械合并，不新增万能Utils。
4. 没有满足“静态无引用+非反射/配置+编译+测试”四项证据的局部死代码删除；CommandManager键盘入口等仍保留。
5. Relay值、速度/高度边界、timeout/retry、4000/window/cap不是非业务Magic Number；保持各trust boundary。
6. Phase0的Cesium反射错误/资源警告、final result丢失/重入、真实UI与硬件验证缺口继续保留。本轮自动测试不证明物理动作。

## Q. 完成标准

完成本轮限定Phase1：核心重复消除、公共依赖收窄、无新架构/状态、行为等价、要求构建与完整可用自动回归通过、文档更新。较宽候选没有被误报“全部债务清零”。Phase2未开始；不自行提交，等待用户下一阶段指令。

## 实测结果

| 运行 | Exit/结果 | 墙钟秒 |
|---|---|---:|
| Console-Release | 0 / PASS | 3.351 |
| Console-Debug | 0 / PASS | 1.794 |
| UE-Development | 0 / PASS | 45.509 |
| full | 0 / PASS | 60.459 |
| disarm-tests | 0 / PASS | 0.017 |
| runtime-config-tests | 0 / PASS | 0.018 |
| mavftp-concurrency-tests | 0 / PASS | 0.160 |
| artifact-v2-scale-10k | 0 / PASS | 9.809 |
| mission-benchmark | 0 / PASS | 1.457 |
| UE-Full | 0 / PASS | 60.924 |
| UE-Synthetic | 0 / PASS | 26.267 |
| Launcher-Smoke | 0 / PASS | 10.996 |

UE Full：242/242成功（222无warning、20带warning），0失败/未运行；Synthetic：1/1成功。合计243/243，与Phase0相同测试数。Console6次运行0失败；Launcher32检查通过；没有新增自动化测试项。记录仍包含既有Cesium启动错误及预期拒绝诊断，不宣称整个引擎/真机无风险。
