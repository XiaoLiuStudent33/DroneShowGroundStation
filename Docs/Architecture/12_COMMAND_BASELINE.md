# Phase 0：Command 行为

基线：`a95309d8b37ecb00c779d5c4264aacb86294d8ec`（master，2026-09-09）。本阶段不改变生产源码、协议、配置或资产。执行结果见 [测试基线](11_TEST_BASELINE.md)；静态结论不等于真机验证。

## 1. 入口、身份与结果（全部命令共用）

`UIBridge::Handle* → SubmitSelectedProductionCommand / SubmitProductionCommandForTargets → ProductionCommandProvider::SubmitCommand → ToProtocolType / EncodeCommand → ProductionTelemetryProvider::SendFrame → localhost DSP → Application → ProductionCommandReceiver::EnqueueFrame → ProcessPending → CommandManager::Submit → DroneManager::FindDrone → DroneSession::SubmitCommand → CommandDispatcher → MavlinkCommand → Session Remote Endpoint`。

普通命令的目标由 `SelectedCommandDroneIDs` 经现有门禁得到，详情选择、筛选结果、RouteSlot 都不参与目标替代。Provider 去除空项和重复 ID；空集合不广播。Console 根据 DroneID 找 Session，再使用 Remote IP + UDP Source Port；SYSID/component 只属于该 Session 的包校验和发送参数。Upload 使用独立的 confirmed mapping 规则，见 [14](14_UPLOAD_BASELINE.md)。

三种请求身份分别保留：UE `UE-PROD-<Unix seconds>-<counter>`、Console batch uint64、Session dispatcher requestID。DSP CommandType 数值见下表；它不是 MAV_CMD，也不是请求身份。

回程：`Endpoint/Session 校验 → Dispatcher::HandleAck / HandleHeartbeat / HandleTelemetryMessage → ConsumeResults → CommandManager::Tick → Receiver::ToResultBatch → DSP → UE Provider::HandleResultPayload → GameThread ApplyResultBatch → OnCommandResult → UIBridge`。owner key 包含 DroneID 和 requestID，不可跨机完成。

Provider 保持 enabled、safeTestMode、link 和参数验证顺序；HoverTest 还要求地面站 Autonomous 和有效高度。Console real-command gate、receive-only gate、目标在线/armed/模式门禁均保留。模拟结果只能标 Simulated。

## 2. 完成机制图例

| 代号 | 当前软件完成依据 | Timeout / Retry |
|---|---|---|
| A | 与当前 pending 对应的 COMMAND_ACK；IN_PROGRESS 继续等待并刷新计时，拒绝/失败返回原状态 | Dispatcher 3000ms；MaxRetries=1（初次 + 最多1次重发）；每 Session pending + queue，MaxQueueSize=32 |
| H | SET_MODE 后收到符合预期 custom_mode 的 HEARTBEAT | 同 Dispatcher 3000ms / 1 retry，不以 COMMAND_ACK 代替 |
| S | 成功发包即 Sent，无标准 COMMAND_ACK 等待 | 不进入 A 的 ACK 完成路径；发包失败仍失败 |
| R | 顺序完成各消息 interval ACK 后，再收到新 #187 | 每步 A；最后新样本等待3000ms；旧缓存不确认恢复 |
| W | 应用级 HoverTest 接受进入工作流，非单个 MAV_CMD | GUIDED 等待5s、armed 等待8s、达到高度等待20s；底层命令各自使用现有 Dispatcher |

安全优先命令可取消当前 pending 并前置队列。不得为统一接口而调整排队/取消顺序。CommandManager 将非 Pending 结果收敛为终态，Sent 与 Accepted 均不代表物理完成。UE 当前没有最终 DSP result 丢失后的独立 pending timeout；这是未修复风险。

## 3. 25 类生产命令合同

以下每一行均为 `STATIC_BASELINE`（含公共入口/目标/结果/timeout规则）；现有 Command/DSP/UE 测试的实际结果与覆盖粒度见 [11](11_TEST_BASELINE.md)，通过的捕获包断言才属于 `TESTED_BASELINE`。所有飞行、继电器、灯光、断电动作均还要求 `FIELD_BASELINE_REQUIRED`。

| DSP ID / 命令 | UI/Application 入口 | 飞控包与参数转换 | 机制 / 特殊行为 |
|---|---|---|---|
| 1 Arm | HandleArmClicked | MAV_CMD 400，p1=1,p2=0，其余0 | A，正常解锁，不强制绕过检查 |
| 2 Disarm | HandleDisarmClicked；HandleAdvancedLockClicked | 400，p1=0,p2=0，其余0 | A；高级加锁入口有二次确认，普通入口不同；无旧GPS着陆门禁 |
| 3 Takeoff | HandleTakeoffClicked | 22，p7=高度m；UI固定1m，经现有高度限制 | A |
| 4 Land | HandleLandClicked | 21 | A，安全优先；成功提交清除目标暂停资格 |
| 5 RTL / RETURN_HOME | HandleRTLClicked | #11 SET_MODE custom_mode=6 | H，安全优先；不是调用旧 RTL command-long helper；清除暂停资格 |
| 6 SetMode | HandleGuidedClicked；Provider::SubmitCommand | #11，CUSTOM_MODE_ENABLED，custom_mode；GUIDED=4，支持0/3/4 | H |
| 7 HoverTest | HandleHoverClicked → Provider::SubmitCommand | start/高度/时长字段进入 FlightControl，依次 GUIDED→ARM→TAKEOFF | W；UI先切Autonomous，被拒时恢复；不自动LAND/RTL；每活动Session的worker线程仍存在 |
| 8 SetLedColor | HandleColor*Clicked → ApplyAndSubmitSelectedDroneLedColor → Provider::SubmitCommand | #186 LED_CONTROL，RGB前三byte，len=3，payload target=0/0，仍只发所选Endpoint | S |
| 9 TimeStart | HandleTimeStartClicked | 自定义#78：control=1、mask、hour/minute/second，本地时间；5byte CRC145 | S；倒计时到0不播放NPZ |
| 10 TimeStop | HandleTimeStopClicked | #78 control=0，mask，时间清零 | S |
| 11 ExecuteDanceStep | HandleExecuteDanceStepClicked | 先#11 AUTO=3，后31010 USER_1 p1=1 | A等待USER_1；不等待AUTO心跳再发第二包，不自动播放NPZ |
| 12 EndDanceStep | HandleEndDanceStepClicked | 先#78取消授时，后31010 USER_1 p1=0 | A等待USER_1，复合发送顺序保持 |
| 13 MissionPause | HandleMissionPauseClicked → SubmitMissionPlaybackCommand | 17 NAV_LOITER_UNLIM p1=1 | A；结果更新每目标暂停状态 |
| 14 MissionForward | HandleMissionForwardClicked → SubmitMissionPlaybackCommand | 20 NAV_RETURN_TO_LAUNCH p1=0（旧设备自定义语义） | A；必须先暂停，提交后消耗暂停资格 |
| 15 MissionBackward | HandleMissionBackwardClicked → SubmitMissionPlaybackCommand | 20 p1=1 | A；同上 |
| 16 ChangeSpeed | HandleAdvancedSpeedClicked | 31011 USER_2 p1=速度m/s；UI clamp 1..5，后端验证 | A |
| 17 TriggerSmoke | HandleTriggerSmokeClicked | 181 DO_SET_RELAY relay0/state1；发送后1秒 relay0/state0 | A；二次确认；OFF按发送时间安排，不以ON ACK代替复位 |
| 18 StopTelemetry | HandleStopTelemetryClicked | 511 SET_MESSAGE_INTERVAL，消息1/24/33/30/187，interval=-1 | A，顺序多步 |
| 19 ResumeTelemetry | HandleResumeTelemetryClicked | 511，同五消息 interval=1000000μs（1Hz） | R，必须新#187 |
| 20 DisableLedOutput | HandleLightsOffClicked | 31012 USER_3，disable=true | A |
| 21 RestoreLedOutput | HandleRestoreLightsClicked | 31012 USER_3，disable=false | A；恢复灯光是USER_3 ACK，不另等#187；新#187门禁属于ResumeTelemetry |
| 22 Wake | HandleWakeClicked | 246 PREFLIGHT_REBOOT_SHUTDOWN，autopilot action=4 | A |
| 23 Standby | HandleStandbyClicked | 246，action=2 | A；armed禁止；二次确认 |
| 24 Reboot | HandleRebootClicked | 246，action=1 | A；armed禁止；二次确认 |
| 25 Shutdown | HandleShutdownClicked | 181 relay1/state1，沿用BGC2 | A；armed禁止；二次确认；不将ACK当断电 |

高级确认窗口5秒，当前绑定命令类型与时间，未冻结第一次点击时目标。Phase 0 只记录，不能在重构中悄悄改变。软件捕获包不会证明真实继电器、电机或断电板执行。Drone001/007关机硬件差异继续延期，不改变批量路径。

## 4. 可复查源码

[UIBridge](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSUIBridgeSubsystem.cpp)、[UE Provider](../../UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSProductionCommandProviderSubsystem.cpp)、[Receiver](../../ConsoleGCS/DSPGCSProductionCommandReceiver.cpp)、[Session](../../ConsoleGCS/DroneSession.cpp)、[Dispatcher](../../ConsoleGCS/CommandDispatcher.cpp)、[Dispatcher常量](../../ConsoleGCS/CommandDispatcher.h)、[encoder](../../ConsoleGCS/MavlinkCommand.cpp)、[FlightControl](../../ConsoleGCS/FlightControl.cpp)、[DSP wire枚举](../../Shared/DSPGCSProtocol/DSPGCSProtocol.h)。
