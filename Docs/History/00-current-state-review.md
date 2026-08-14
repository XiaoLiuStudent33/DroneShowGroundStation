# 当前工程现状审查

> **Status: HISTORICAL**
>
> **Authority: None for current architecture**
>
> **Last Updated: 2026-08-14 (classification only)**
>
> **DO NOT USE AS CURRENT ARCHITECTURE**
>
> Superseded by [Architecture_Freeze_CN.md](../Architecture_Freeze_CN.md), [Code_Architecture_CN.md](../Code_Architecture_CN.md), and [Development_Status_CN.md](../Development_Status_CN.md).

审查日期：2026-07-31

## 导入结果

- `ConsoleGCS/`：VS2022 C++ 控制台项目，已排除 `.vs`、`x64` 等生成目录。
- `UnrealGCS/`：Unreal Engine 5.5 项目，已排除 `Binaries`、`Intermediate`、`DerivedDataCache` 和 `Saved`。
- 控制台工作副本已使用 VS2022 MSBuild 成功完成 `Debug|x64` 构建。
- 构建警告主要来自 MAVLink 生成头文件中的浮点精度转换，以及 `MavlinkParser.cpp` 的两处 `double` 到 `float` 转换。

## 当前数据流

控制台项目当前的数据流为：

```text
UDP Receive
  -> mavlink_parse_char
  -> MavlinkParser
  -> FlightData
  -> DataManager
  -> ConsoleUI / FlightControl
```

命令流为：

```text
键盘输入
  -> MavlinkCommand
  -> UDP Send
  -> 飞控
```

UE 项目目前是独立的本地动画模拟：

```text
UMG/蓝图按钮
  -> ADroneActor
  -> 修改 EDroneState
  -> Tick 中模拟升降和旋转
```

UE 与控制台通信核心目前尚未连接。

## 优先处理的风险

### P0：真机安全

1. `MavlinkCommand::Arm` 的 `param2` 固定使用 `21196`。在 ArduPilot 中这属于强制解锁用法，会绕过正常的部分安全检查。生产路径不应默认使用它。
2. 控制台允许在没有确认在线状态、目标身份和当前飞行状态的情况下直接执行解锁、上锁、降落和返航。
3. `COMMAND_ACK` 目前只写日志，自动悬停任务依靠固定延时继续执行，没有根据 ACK、飞行模式和 armed 状态决定是否进入下一步。
4. `FlightControl` 中自动任务固定向 System ID `1`、Component ID `1` 发送，与实际发现的飞控身份没有关联。

### P0：并发稳定性

1. 主线程写入 `DataManager::flightData`，悬停任务线程同时读取，没有互斥锁或快照机制，存在数据竞争。
2. 主线程和悬停线程都可能调用 `ConsoleUI::AddLog`，控制台状态同样没有同步。
3. 已结束但仍为 `joinable` 的 `hoverThread` 如果被新线程直接覆盖，程序可能触发 `std::terminate`。
4. 停止悬停任务时主线程和任务线程都可能发送降落指令。

### P1：连接与遥测

1. 收到一次心跳后 `online` 永远保持为 `true`，缺少心跳超时和重新连接逻辑。
2. 没有按 `sysid/compid` 过滤消息，多个 MAVLink 设备进入同一端口后会覆盖同一份 `FlightData`。
3. `gpsValid` 和 `altitudeValid` 已定义但没有更新。
4. 缺少遥测时间戳、数据新鲜度、GPS fix、卫星数、电压、链路质量和 EKF 健康信息。
5. IP `192.168.1.145`、目标端口和本地端口写死在 `main.cpp`。

### P1：UE 集成

1. `ADroneActor` 只模拟视觉状态，没有调用真实通信接口。
2. UE 的状态是“按钮触发的期望状态”，不是飞控遥测确认的实际状态。
3. `DISARM` 和 `RTL` 可以无条件切换状态，缺少合法状态转换和拒绝原因。
4. 每架无人机永久启用 `Actor Tick`，未来不能直接扩展到千机或万机。
5. `Build.cs` 目前没有引入通信、UMG 等后续核心模块。
6. 默认启动地图仍指向引擎模板 OpenWorld，而不是项目的 `Content/Maps/DroneShowPlatform1`。
7. `DefaultEngine.ini` 中包含 Android File Server 安全令牌；如果该功能不用，应关闭并移除令牌，若继续使用则应轮换令牌。

## 推荐的第一轮实现

第一轮目标不是改变 UI，而是建立一个可验证的单机控制核心：

1. 增加外部配置文件，移除硬编码 IP、端口和目标 ID。
2. 将普通解锁与“强制解锁”彻底分开，默认只允许普通解锁。
3. 建立 `VehicleId`、`TelemetrySnapshot` 和心跳超时机制。
4. 建立命令请求、ACK、超时和结果模型。
5. 将自动任务改成由状态和确认驱动，不再依赖固定 `Sleep`。
6. 修复 `DataManager`、任务线程和日志的并发问题。
7. 编写 MAVLink 数据包回放测试，不连接真机也能验证解析和状态机。
8. 完成后再把该核心以 UE 模块或本地服务接口接入 UMG。

## 安全约束

在完成普通解锁、身份确认、状态校验和 ACK 流程前，不应使用当前工作副本连接装有桨叶的真实无人机进行自动悬停测试。优先使用 SITL、拆桨测试架或受控测试环境。

## 第一轮加固完成情况

2026-07-31 已完成：

- 默认解锁的 `param2` 已由 `21196` 改为 `0`，保留飞控安全检查。
- 网络地址、端口和心跳超时已迁移到外部配置文件。
- 未提供配置时程序会安全退出，示例配置默认指向 `127.0.0.1`。
- 自动悬停任务改为使用心跳发现的实际 System ID 和 Component ID。
- 加入三秒可配置心跳超时，超时后禁止普通控制指令。
- 加入 GPS fix、卫星数、遥测有效性及最近 ACK 数据字段。
- `DataManager` 改为互斥保护的值快照，消除主线程与任务线程直接共享引用。
- 控制台日志和界面刷新增加互斥保护。
- 修复任务线程结束后再次启动可能触发 `std::terminate` 的问题。
- 自动任务使用飞行模式、armed 状态和高度反馈推进，不再只依赖固定延时。
- 上锁操作在无法确认已落地时会被阻止。
- 中止自动任务时，根据是否离地选择普通上锁或降落。
- UDP 初始化增加目标 IP 校验，并修复 Winsock 重复清理及来源状态初始化。
- VS2022 `Debug|x64` 完整重建通过。

下一轮仍需完成：

- 增加 MAVLink 报文回放测试和 SITL 集成测试。
- 将控制核心从控制台入口中抽离为可供 UE 调用的独立库。

## 第二轮加固完成情况

2026-07-31 已完成：

- 新增线程安全的 `CommandDispatcher`，所有解锁、上锁、模式切换、起飞、降落和返航均通过单一队列发送。
- 每条请求包含递增请求 ID、目标身份、期望 ACK、发送次数和最终结果。
- COMMAND_LONG 指令支持三秒 ACK 超时和一次重试。
- ACK 会按飞控 System ID 和命令编号匹配当前请求。
- `MAV_RESULT_IN_PROGRESS` 会延长等待时间，拒绝和超时会形成明确结果日志。
- `LAND`、`RTL` 和已通过安全检查的 `DISARM` 具有安全优先级，会取消尚未完成的普通指令，避免降落后继续执行陈旧的起飞命令。
- 新增本机 UDP 集成测试，验证普通解锁参数、ACK 匹配和 LAND 抢占 TAKEOFF；测试已通过。

### ConsoleUI 问题和修复

原界面强制设置为 `120 × 60`，菜单位于第 36 行，运行日志位于第
42～58 行。Windows 控制台或 VS 调试窗口无法提供 60 行时，
`SetConsoleScreenBufferSize` / `SetConsoleWindowInfo` 可能失败，导致下半部分必须滚动或完全不可见。

现已调整为：

- 默认 `110 × 36` 的双栏紧凑布局。
- 菜单位于第 23 行，日志从第 28 行开始。
- 启动时根据 `GetLargestConsoleWindowSize` 调整可见窗口。
- 缩小尺寸时先缩小窗口，再调整缓冲区，避免 Windows API 拒绝操作。
- 飞控身份、状态和悬停数据位于左栏。
- 姿态、GPS、GPS Fix、卫星数和电池位于右栏。
- 尚未收到对应遥测时显示 `N/A`，不再用 `0` 冒充有效数据。
- 日志末行根据实际缓冲区高度计算。
- 界面刷新和任务线程日志继续由同一互斥锁保护。

后续仍需完成：

- 使用录制的 MAVLink 报文测试心跳掉线、错误 ACK 和多飞控消息过滤。
- 使用 ArduPilot SITL 验证完整的 GUIDED → ARM → TAKEOFF → LAND 状态闭环。
- 将协议、调度、遥测和状态机抽成不依赖控制台的静态库，再接入 UE。
