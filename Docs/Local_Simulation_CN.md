# 本机无人机模拟使用说明

Status: CURRENT SUPPORTING
Last Updated: 2026-09-11

本文件维护操作方法与协议适用范围。**当前实现、历史测试、人工验收、真机状态及下一阶段仅见 [唯一状态入口](Development_Status_CN.md)**。以下为使用说明，本轮 P0 未执行其中任何启动或操作。

## 使用方法

1. 使用仿真环境前先结束占用同一 DSP/MAVLink 端口的后端。运行 [StartLocalSimulation.cmd](../ConsoleGCS/StartLocalSimulation.cmd)，它从自身目录调用既有 Release 后端与 [gcs-config-simulation.ini](../ConsoleGCS/gcs-config-simulation.ini)，不自动构建。
2. 在 UE 运行场景中展开集群仿真。默认目标端口 10001、数量 100、行列中心间距 **0.5m**；端口/数量必须为整数，范围分别为 1–65535 / 1–20000；间距允许 0.1–100m。20000 仅为输入上限，验收规模见状态入口。
3. 点击开始。端口、数量和间距在运行中锁定；调整数量或间距先停止，再修改重启。UI 重绑保留运行参数，不因面板重建重新创建仿真。
4. 点击停止释放工作线程及来源端点；正常结束 PIE/GameInstance 也调用清理。后端仍须完成超时判定及快照发布，列表/场景不保证立即清空；异常退出场景的验证范围见状态入口。

只向本机 127.0.0.1 发送；改目标端口时同步匹配后端配置。状态文字“已发送”为本地 UDP 计数，不是后端在线、ACK 或命令执行确认。配置只在后端启动时读取，修改配置后需重新启动后端。

## 参数、遥测与命令合同

源码入口：[GCSSimulationSubsystem.h](../UnrealGCS/Source/DroneShowPlatform1/Public/GCSSimulationSubsystem.h) 与 [GCSSimulationSubsystem.cpp](../UnrealGCS/Source/DroneShowPlatform1/Private/GCSSimulationSubsystem.cpp) 的 BindUI、StartSimulation、StopSimulation、Deinitialize、FGCSSimulationWorker::Run。

- 控件绑定包括 TXT_SimStatus、SPN_SimTelemetryPort、SPN_SimDroneCount、SPN_SimSpacingMeters、BTN_StartSimulation、BTN_StopSimulation；布局由保存的 UI 资源提供。
- 单后台线程，每架机独立 UDP 来源端口；SYSID/COMPID 都为 1，Console 按 IP+端口区分。来源端口从 20000 寻找至 65535，跳过目标端口；不能满足全部数量时整体失败并释放端点。
- 约每机 1Hz 发送 HEARTBEAT、GPS_RAW_INT、GLOBAL_POSITION_INT、SYS_STATUS、MDHK_STATUS #187。初始 XY 为按间距排列的方阵；使用场景 CesiumGeoreference，缺失则使用源码默认场地坐标。
- 非受控演示状态下 XY 固定，各机各轮从 −1000～6000mm 采样高度；一个共享 mt19937 连续随机流，不是每架独立随机流。标准与 #187 共用当轮高度，AMSL = 30m + 相对高度。电量每 30 秒降 1%、最低 20%，温度/电芯随时间波动，均为模拟数据；valid_flags 保留 0，不伪造硬件/固件标识。
- 只处理来自配置后端 IP+端口的命令。LED_CONTROL #186 支持广播或 1/1 目标、instance/pattern=255、三字节 RGB，颜色持续通过 #187 回报，停止重建恢复默认青色。
- SET_MODE 仅处理 custom-mode 标志及 0/4/5/6/9：进入受控状态，终止爬升、负高度归零；0/4/5 保持，6/9 下降。SET_MODE 不应表述为所有模式均支持。
- COMMAND_LONG 普通解锁/上锁要求 param2=0；从未解锁到解锁时高度清零，普通上锁要求高度不大于零。GUIDED 且已解锁才接受有限 0 < 起飞高度 <= 100m，约 1m/s 接近目标后保持；LOITER/悬停停止升降；RTL/LAND 约 1m/s 下降，落地后显式上锁。
- RTL 的 XY 已在各自 HOME，没有水平返航。未解锁起飞、无效高度、空中普通上锁返回拒绝；其他 COMMAND_LONG 返回 UNSUPPORTED。没有自动航线执行、MAVFTP 上传应答、完整飞行动力学或故障注入。

## 后端清理与生产配置差别

[ConsoleGCSApplication.cpp](../ConsoleGCS/ConsoleGCSApplication.cpp) 在 clear_offline_simulation 开启时调用 [DroneManager::RemoveOfflineSimulationDrones](../ConsoleGCS/DroneManager.cpp)。删除须同时满足：

- remoteIp 恰为 127.0.0.1；
- mavFtpServiceEnabled 与 missionUploadServiceEnabled 均关闭；
- Session 状态为 HeartbeatTimeout。

仿真 profile 开启此清理、3000ms 心跳阈值、本机命令和 DSP，关闭上传。普通配置不启用此清理（[AppConfig.cpp](../ConsoleGCS/AppConfig.cpp) 默认 false），不能把仿真清理等同普通生产 Offline 行为。没有独立模拟器认证标记，清理依赖上述配置/端点条件。

停止、缩量后不再发送的旧端点及 UE 退出后的端点经过超时删除，再由完整快照传播到 UE；“约 3 秒”是心跳阈值，不是界面完成清空的硬时限。删除模拟 Session 不等于终止 Console 后端。正常/异常退出、重启后 DroneID 与界面一致性验收见状态入口。

仿真报文供现有发现/遥测链消费；地图贴地仅影响渲染，不改写 NPZ、上传数据或真实飞机原始遥测。UE 内直接使用 MAVLink dialect/模拟 socket 与长期生产边界的适用关系仍列于状态入口待确认。

## 历史记录（原始结论保留）

以下原文仅适用于当时基线；“当前”“本轮”、旧参数、PASS 和“未 commit/push”等不是本轮工作区或验收结论。当前状态统一见 [Development_Status_CN.md](Development_Status_CN.md) 的状态矩阵。历史命令不构成本轮执行授权。

<details>
<summary>展开 P0 开工前原文（完整保留，不作为当前状态入口）</summary>

# 本机无人机模拟

2026-09-11：第一版提供协议遥测仿真，复用现有 ConsoleGCS 发现和 UE 遥测显示链路。

## 使用

1. 关闭已运行的 ConsoleGCS 后端，避免重复占用 DSP-GCS 端口。
2. 双击 `ConsoleGCS/StartLocalSimulation.cmd`，使用 Release 后端和 `gcs-config-simulation.ini`。该配置仅监听 `127.0.0.1:10001`，通过现有 DSP-GCS 回传 UE；打开本机 MAVLink/DSP 指令传输，航线上传仍关闭；模拟器当前只处理 RGB。原有部署配置不变。
3. 打开 UE 项目并运行，展开“集群仿真”。目标端口填 `10001`，数量先填 `100`，排列间距默认 `2.0` 米（可填 0.1–100 米），点击“开始仿真”。间距同时用于行、列相邻中心距离；运行中禁用修改，停止后可调整再启动。
4. “停止仿真”立即停止发送并释放端点；后端在约 3 秒心跳超时后标记离线，不自动删除历史机体。再次开始复用可用来源端口。

只支持本机目标，界面无需 IP 或原点输入。改目标端口时必须同步接收端配置。“已发送”是本机 UDP 发送计数，不是后端在线确认。

## 已实现范围

- 绑定用户制作的 `TXT_SimStatus`、`SPN_SimTelemetryPort`、`SPN_SimDroneCount`、`BTN_StartSimulation`、`BTN_StopSimulation`；未生成或改动 UI 布局。
- 默认端口 10001、数量 100；输入限制为整数，端口 1–65535，数量 1–20000。20000 是输入上限，并非已验证的性能承诺；端点不足时整体启动失败并清理。
- 每架模拟机独立 UDP 来源端口，后台单线程按约 1 Hz 均匀发送 HEARTBEAT、GPS_RAW_INT、GLOBAL_POSITION_INT、SYS_STATUS、MDHK_STATUS #187。MAVLink SYSID/COMPID 均为 1，后端按来源 IP+端口区分，遵循 bgc2 的端点模型。
- 187 遥测补充缓慢下降的电量、24–26°C 温度波动、3.92–3.98V 电芯波动、GPS UTC、RTK 定位、相对高度及 EKF/磁场测试值；有效位语义未提供，valid_flags 保留 0，未伪造固件和硬件标识。复用 ConsoleGCS 的 MDHK dialect 定义，避免双方 CRC/字段顺序漂移。遥测表仅接收 187 行，标准遥测不自动形成该表格行。
- 初始位置为指定间距的方阵，`SPN_SimSpacingMeters` 默认 2m，支持小数。间距在启动时固定，直接参与模拟 GPS 和 187 定位坐标生成。使用场景 CesiumGeoreference 经纬度；无该 Actor 时使用默认场地坐标。水平坐标固定，保留排列间距。参考已安装 bgc2_dspgcs_patch 的集群仿真逻辑，每架机每轮独立随机采样 −1000～6000mm 相对高度；AMSL = 30m + 相对高度。GPS_RAW_INT、GLOBAL_POSITION_INT 与 #187 共用当轮采样值。使用各机独立随机流，便于重现；当前约 1Hz 更新，未声称与 Blender 频率完全一致。此高度跳变是遥测仿真，不是起飞动力学。电量每 30 秒下降 1%，最低 20%；定位为模拟 RTK fixed。以上均为测试数据，非地形测量。
- 运行中禁用端口、数量、开始按钮；支持停止、重新启动、UI 重绑及 GameInstance 退出时释放线程和全部套接字。
- 不修改已有真机遥测、NPZ、显示坐标转换、地图或航线数据。支持来自配置后端端点的 LED_CONTROL #186：广播或 1/1 目标、instance/pattern=255、三字节 RGB；颜色保存在各机状态，持续由 #187 回报，停止重建后恢复默认青色。不执行起飞、航线、上传应答或故障注入，不伪造这些操作的 ACK。

## 代码和验证

排列间距增量：`BuildLogs/SimulationSpacing/index.json` 定向测试 1/1 通过。实际保存的 Spacing SpinBox 绑定成功，输入 0.5m 后从 187 经纬度反算行、列间距并校验 GPS 量化误差；同时覆盖零值、超限、NaN、运行中禁用、重绑保留和停止后恢复。

入口 `GCSUIBridgeSubsystem` 绑定 `UGCSSimulationSubsystem`。工作线程私有持有套接字和 MAVLink 编码状态，仅通过原子计数向 UI 报告进度。

Development Editor 编译通过。UE 全套 252 项通过（230 success、22 带警告、0 failure），报告 `BuildLogs/Simulation/index.json`；补充生命周期定向测试 1/1 无警告通过，报告 `BuildLogs/SimulationLifecycle/index.json`。

`DroneGCS.UI.Simulation.LoopbackAndLifecycle` 实例化实际保存的 RightPanel，检查控件类型、参数校验、三来源端点及四种有效 MAVLink 报文、禁用状态、重绑、停止、重启与退出清理。NullRHI 测试不验证最终画面。

使用测试中捕获的实际 UE 报文在独立本机端口送入 Release ConsoleGCS，确认注册 Drone000/001/002、在线数 3，停止发送后在线数降为 0。证据 `BuildLogs/simulation-backend-test.log`。该验证隔离关闭 DSP 回传；完整交互画面仍需打开 UE 验收。

187 增量编译通过，`BuildLogs/Simulation187/index.json` 定向测试 1/1 无警告通过，校验五类报文 CRC 和 187 电量、温度、电芯、GPS/时间字段。新增报文的后端发现/离线回归记录于 `BuildLogs/simulation187-backend-test.log`。尚未进行像素级表格验收。

间距对照：UE 默认 2m 方阵，现可用间距控件调整。bgc2 的 `simulator.py` 使用 benchmark 文件坐标；`simulator.blend` 中 CreateArray 初始化点并未提供可等同于 UE 的固定方阵间距，BDS_MDFI 的 Ico Sphere 半径为 0.2m（只读背景进程检查，未保存资源）。不能仅凭不同缩放截图认定 Blender 间距值。需要完全一致的排列时应使用相同基准坐标数据。

2026-09-11 动态遥测 / RGB：Development Editor 编译通过，`BuildLogs/SimulationDynamic/index.json` 定向测试通过。真实 UDP 检查经纬度和温度随时间变化、三轮 RGB 持久化、未选机颜色不变、初始间距及生命周期。更换本机配置后必须重启 ConsoleGCS（`StartLocalSimulation.cmd`）；旧进程不会重新加载配置。完整 UI 到后端再到模拟机的点击验收仍需在 UE 中进行。

Blender 类似效果增量：取消同步水平漂移，独立随机高度。平面地图显示仍保留低于地图基准贴地规则，原始遥测负高度不被改写。测试覆盖固定 XY/间距、跨周期高度变化、各机高度差异、标准与 187 高度一致，以及 RGB 持久化。

指令增量：独立递增种子改为 mt19937 连续采样，消除编号间的规律性高度分布。SET_MODE 支持 0/4/5/6/9，持久写入心跳；模式 0/4/5 保持高度，模式 6/9 每秒下降约 1m 至各机初始位置。COMMAND_LONG 支持 RTL/LAND，返回对应 ACK；其他 COMMAND_LONG 返回 UNSUPPORTED。所有模拟机 XY 当前就在各自 HOME，所以 RTL 无水平移动；未实现自动航线执行。Development Editor 编译通过；SimulationCommands 定向测试验证模式持久化、RTL ACK 与下降至零，以及原 RGB/遥测回归。详情卡与列表不一致尚未完成资产绑定核查，不作为此次已修复项。

飞行流程增量：COMPONENT_ARM_DISARM 支持普通解锁/加锁，心跳 armed 位随状态回报。首次解锁从随机演示切换为地面受控状态（高度清零）；GUIDED 且已解锁时接受 NAV_TAKEOFF 的 0～100m 有限正数目标，约 1m/s 接近目标后保持。LOITER/悬停停止升降并保持当前高度。LAND/RTL 约 1m/s 下降，落地后由显式加锁指令锁定；空中普通加锁、未解锁起飞及无效高度被拒绝。各命令返回相应 ACK，标准和187同步高度；这不是完整飞行动力学。使用现有 UI，无新增控件。


## 2026-09-11 仿真退出清理

默认排列间距为 0.5 米。使用 StartLocalSimulation.cmd 启动更新后的后端，仿真配置 clear_offline_simulation=true。点击停止仿真、结束 PIE、关闭 UE（包括异常退出）后，模拟端停止心跳，后端在约 3 秒心跳超时后删除失联的本机模拟机；下一批全量快照同步清空 UE 列表、遥测表与场景。缩小仿真数量时旧端点同样被清理。普通配置默认关闭清理，非 127.0.0.1 的真实无人机不会被此功能删除。更新后需重启后端使配置生效。

</details>
