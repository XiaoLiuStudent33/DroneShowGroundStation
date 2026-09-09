# 技术债、重复与死代码候选

Status: AUDIT；基线 `a95309d8`；不授权删除任何源码。

## 1. A/B/C 分类规则

A=在当前扫描边界能确定是冗余文本/结构；B=静态引用不足，需编译/反射/运行或使用者确认；C=现阶段必须保留。A也只列候选，当前不删除。C++无引用不能证明Blueprint反射/API无用户。

| 分类 | 候选/证据 | 处理条件 |
|---|---|---|
| A | DroneShowPlatform1.Build.cs末尾Uncomment Slate/Online模板注释，与实际依赖无关 | 后续纯文本清理commit；不改有效依赖 |
| A | CommandReceiver真实/模拟两分支逐字段复制同组DroneCommandParameters | 提取同一纯转换函数，保留两分支门禁、日志与发送行为 |
| A | 局部控件查找/文本格式化重复 | 只合并参数与空值/大小写行为完全一致的helper，保留反射名 |
| B | Shared UDPDSPGCSTransport预留声明，无生产factory构造 | 确认外部引用后可删声明；保留enum值及拒绝，不实现新网络 |
| B | CommandManager::HandleConsoleKey，生产主循环无调用，CommandTests有调用 | 决定是否保留开发输入API；不是完全unused |
| B | ConsoleUI::Update旧单机overload，当前生产用UpdateFleet | 查测试/外部调用后处理；Renderer保留 |
| B | MavlinkCommand::RTL/Guided/Loiter与Dispatcher现行路径的潜在未用helper | 不凭名称替换当前RTL；需要逐符号引用与packet对照 |
| B | UE MAVLink PublicIncludePaths | 一方生产文本未找到include使用；UHT/clean build/插件与资产核验后决定 |
| B | Tools/Launcher/Deploy.ps1历史部署入口 | 用户工作流与保留包确认后归档；不运行 |
| B | Artifact V1包装/恢复兼容层 | V2复用基础hash/canonical，先建立调用矩阵，不整组删V1 |
| B | 旧阶段名字/注释与编码混杂（部分GBK注释） | 逐文件编码核对；禁止全仓UTF8批量重写 |
| C | MissionUploadTransaction | 当前post-MAVFTP验证仍用，不是旧上传Transport |
| C | MissionTransport抽象 | 唯一生产MAVFTP + 测试seam合理，不是协议选择器 |
| C | Session/DataManager/Fleet/Mirror快照 | 合法跨线程/跨进程/渲染副本，不作为重复状态删除 |
| C | Shared WPL、MissionCodec、Artifact digest/read lease | 两端一致与TOCTOU保护，不复制/去验证 |
| C | Developer Mock/Acceptance及hardware harness | 显式隔离工具，不是生产fallback；保留安全gate |
| C | 所有反射符号、Content资产、UI字符串契约 | 本轮禁止改，未来也需兼容证明 |

## 2. 债务优先级与退出条件

| 债务 | 时机 | 退出条件 |
|---|---|---|
| A01/A02 UI God Class、反向协调 | UI大改前 | 无旧WBP应用测试可运行；旧UI测试保持 |
| A03/A04通道和wire泄漏 | UI大改前 | 新Presenter无DSP/MAVLink/socket依赖 |
| A05结果字符串驱动gate | UI大改前 | typed投影与旧结果逐例一致 |
| A06文档/源码mapping差异 | Phase0前必须识别，Phase0形成基线 | 零基与confirmed目标语义测试明确 |
| A07参数转换重复 | 第一批低风险代码阶段 | 25种领域动作/参数映射完整覆盖 |
| A08 Shared平台头、A10构建传播 | API稳定后 | 两端clean build和codec fixtures不变 |
| A09 Session/Application体积 | 可延后UI替换 | 只提内部实现，无Owner变化 |
| 无界ID/结果丢失/确认目标/回调重入 | 单独问题单 | 先复现，再审批行为修复，不与机械提取混合 |
| Hover每机线程 | UI替换后独立性能/时序工作 | 有规模测量与顺序/等待等价证据 |
| 10000规模、备用机、重分配、补点 | 业务延期 | 独立需求，不借治理开发 |

## 3. TODO/FIXME审计解释

过滤生成MAVLink、第三方、Saved/Intermediate后，对一方251个cpp/h/inl文本扫描未发现显式TODO/FIXME/deprecated标记。并不表示没有债务：存在Reserved、历史阶段说明和上述具体耦合。外部UnrealMCP自身的deprecated注释不算业务债务；只核对其Editor依赖边界。

## 4. 不应拆与可拆清单

应拆职责：UIBridge的操作策略/Presenter、Coordinator的事务/展示、TelemetryProvider的格式化/协议入口、Receiver重复映射。可做局部拆分：Session::SubmitCommand、Application组装、ArtifactManager schema恢复helper。

不应拆Authority：DroneManager/Session身份、RouteAssignment、UploadJobAuthority、RouteUploadManager、MissionFtpCompiler、单Niagara Actor。不强建CommandTracker、VehicleManager或第二份TelemetryState；已有Owner可提供稳定API。

## 2026-09-09 Phase 1增量结论

A07 Receiver重复13字段组装已消除；enum名称/允许列表未统一，不改变边界校验。A04 CommandProvider公共头已移除完整DSP include，以两个既有struct前置声明替代；Telemetry/Upload/Frozen按值合同仍保留。A10 Sockets/Networking已Public→Private，其余模块和Shared路径未大扫除。控件查找/格式化的根对象/顺序/空值/日志语义尚未逐例证明；InputCore/EnhancedInput及MAVLink路径保留为后续独立验证候选。无不确定死代码删除。详见 [17_PHASE1_REPORT.md](17_PHASE1_REPORT.md)。
