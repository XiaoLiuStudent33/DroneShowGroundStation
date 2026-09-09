# 架构审计结论

Status: AUDIT / NO SOURCE CHANGES；基线 `a95309d8`；2026-09-09。
确认项来自源码，风险项不等于已复现 Bug。没有用静态扫描宣称线程安全或 Blueprint 无依赖。

## 1. 最重要的十个结构问题

| ID/优先级 | 已确认结构与证据 | 影响 | 建议 |
|---|---|---|---|
| A01 高 | UIBridge.cpp 7147行，h 707行；选择、命令策略、结果、授时、地图、控件绑定共存 | 换 UI 容易丢 gate/工作流 | 提取无 Widget 的应用能力，旧 UFUNCTION 委托调用 |
| A02 高 | Coordinator.inl 4054行、h 626行；被 UIBridge.cpp 末尾 include | 上传事务和控件编译/生命周期绑在一起 | 先独立实现单元，再分应用状态与显示；保留单一事务 |
| A03 高 | TelemetryProvider 同时持有 Transport、三类回调、link health、分片、表格格式化和 GPS capture 分发 | Session/Command/Mission 都依赖名为 Telemetry 的入口 | 对外隐藏通道，内部先拆纯投影；不创建第二 socket |
| A04 高 | Public command/telemetry/upload/frozen headers 暴露 DSP 类型；FilterModel include Provider | 新 UI 仍必须认识 wire 与 provider | 高层只读 DTO/应用入口，不暴露 encode/send/mirror 原始类型 |
| A05 高 | UIBridge `HandleProductionCommandResult` 比较 SENT/ACCEPTED/SIMULATED 字符串并维护暂停集合 | 展示字符串变化影响操作 gate | 保留原字段，新增内部语义投影；不改变三种结果差异 |
| A06 高 | Confirmed NPZ Mapping、默认零基编号、runtime cap 与旧文档不一致 | 按旧文档重构会改目标或参数 | 先冻结真实行为用例；文档注明优先级 |
| A07 中 | CommandReceiver 两分支逐字段复制参数；类型/名称/允许列表多点 switch | 加命令容易漏字段 | 局部纯映射与覆盖测试，保持 UE/domain/wire enum 分离 |
| A08 中 | Shared 除 DTO 还有 WPL/codec/hash/canonical/Windows read lease | “只含 wire types”描述失真，公共头扩大依赖 | 明确 pure contract / portable algorithm / platform utility；不全搬后端 |
| A09 中 | Session.cpp 1141行、SubmitCommand约404行；Application 组装与CLI/harness混合；SafetyConfig 可变全局 | 测试隔离和生命周期审计成本 | 保留 Session Owner，局部提取命令策略/启动解析；冻结配置生命周期 |
| A10 中 | 单 UE runtime module 对外传播 UMG/Slate/Sockets/Networking/Niagara 和 MAVLink include path | 目录分层无编译边界保障 | 先 API 负依赖检查，后评估 Public→Private/去未用 include，避免先拆模块 |

## 2. UI 是否直接操纵底层

生产 UIBridge/UMG 接线没有发现直接打包 MAVLink 或调用 Socket send；这部分已有边界。问题是 UI 侧协调代码消费 DSP 的枚举、digest、job mirror，拥有请求 ID、确认窗口、业务 gate 和多种控制状态。`FGCSUIWidgetPresenter` 目前只是无状态查控件/设文本工具，不是完整应用隔离层。

因此不能回答“UI 已完全解耦”，也不能误报“Widget 已直接发送 MAVLink”。.uasset 本轮不修改、不加载编辑器遍历完整 Blueprint 图，C++ 反射与 authored tests 能证明的范围有限。

## 3. Console 与 Shared 越界判定

- Console Dashboard 是必要的后端可观测性，不是第二套业务 GUI；其单 Renderer 正确，不删除。领域类 include ConsoleUI 只为日志会增加编译耦合，可后移小型日志入口，不能改变输出顺序和丢弃策略。
- `CommandManager::HandleConsoleKey` 是输入适配职责混入领域，当前生产主循环未发现调用、测试有调用。列为需要确认，不擅自删除。
- `DroneSession` 同时拥有 parser、DataManager、Dispatcher、FlightControl、MavFtpClient 与 verification sink，作为生命周期聚合根合理；拆分内部实现不等于拆开身份和网络 Owner。
- Shared 没发现保存全局 Fleet/UE Widget/Console Session 的运行状态。共享 canonical/hash/WPL 保证两端一致，属于合理复用；Windows/Bcrypt/ReadLease 的平台耦合应在文档和 include 面分隔，不能复制两份实现。

## 4. 状态、线程与生命周期结果

详见 [01](01_CURRENT_ARCHITECTURE.md) 和 [08](08_BEHAVIOR_RISKS.md)。发现状态副本不直接判定重复：workingData/DataManager 是工作区与锁保护快照，DSP/UE 是跨进程镜像，Niagara 是渲染缓存。真正需要迁移的是 UI 所拥有的应用操作状态。

已核对：Fleet game-thread apply；DSP receiver 加锁队列；CommandDispatcher mutex；DataManager mutex；FlightControl atomic + join；Preparation worker shutdown/join；UE waypoint finalize future 等待；行绑定 UPROPERTY 保活与回收解绑；Ticker remove；Marquee input processor 注销；Visualization EndPlay 解绑。尚未运行乱序/析构压力用例，不能证明所有交错无竞态。

## 5. 超长函数与头文件

静态物理行数包含注释/空白，只用于定位，非复杂度分数。主要已核对函数：Application::RunProductionMavlink 约427行；Run约368行；Session::SubmitCommand约404行；CommandReceiver::ProcessPending约221行；UE HandleTransportFrame约225行；UIBridge::HandleProductionCommandResult约188行；Coordinator::CaptureAutoStartSnapshot约173行。

Shared Frozen protocol 1089行、Artifact contract 1080行、Upload protocol 976行是 header-only 编码/验证集合；不因行数大就认定 God Class。RouteAssignmentMatcher 的匹配算法、RouteUploadManager 的执行状态、ArtifactManager 的发布/恢复，也不能按任意行数均分。

## 6. 循环依赖判断

范围内251个一方 C++文本的 include 图，按唯一 basename/相对路径解析后未检出强连通环；不是预处理器、UHT、Blueprint 的完整证明。UE 当前只有一个业务 Runtime module，不存在已证实的多个业务 UBT module 环。

**存在逻辑双向协作**：UIBridge 持有 Coordinator，Coordinator 又 GetSubsystem<UIBridge> 读取选择；UIBridge 持有 FilterController，FilterController 回调 Bridge 选择；RoutePreview 寻找 Visualization，Visualization 读取 Preview/Fleet。应降低应用层对 UI owner 的反向依赖，不能将 include 无环描述为完全解耦。

## 7. Magic number 和配置归属

| 内容 | 当前位置 | 治理归属/限制 |
|---|---|---|
| MAV_CMD / 自定义参数、relay/channel | MavlinkCommand、Dispatcher | Console protocol profile/constexpr；保持原 wire，不能改标准命令替代定制合同 |
| DSP message ID/schema | Shared DSP headers | 冻结 wire enum/codec；不移入 UI |
| Dispatcher 3000ms/1 retry/32队列/1000ms pulse | CommandDispatcher.h | 命令策略常量；不合并 MAVFTP 750/2500/3 |
| 高度默认1m/限5m | SafetyConfig、AppConfig | 启动配置与安全 fallback，不能“去重”掉边界校验 |
| 速度1～5m/s | Receiver、Session、UI | 语义常量候选，保留所有 trust boundary gate |
| localhost12001/12002、飞控10001/IP | Shared defaults、两端配置、Launcher | 保留各部署层职责；不可自动修改网卡 |
| 4000/window16/global800/concurrency50 | 编译器配置/启动配置/runtime binding/UI applied | 不同维度，不合并为一个并发值 |
| GPS capture阈值 | DefaultGame.ini/RouteGpsCapture | commissioning配置，HardwareValidated=False；不升级硬件证据 |
| UI刷新0.1/0.2/1秒、确认5秒 | UIBridge/Coordinator | presentation频率与业务确认分别归属；本轮不调参 |

## 8. 重复与删除结论

最值得先处理：Receiver 参数转换重复、UI控件查找/反射字符串/数字文本格式化、digest hex/UTF8 局部 helper、常量允许列表的覆盖验证。跨信任边界重复校验、端点复核和不同协议 retry 不属于可直接删除的重复。完整 A/B/C 分类见 [09](09_TECH_DEBT.md)。
