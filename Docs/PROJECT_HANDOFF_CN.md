# DroneGCS 项目交接速览

Status: CURRENT SUPPORTING
Last Updated: 2026-09-09
Baseline: `master` / `f115587` + 2026-09-09 本交接文档更新

本文件是快速接手入口。长期边界以 [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md) 为准，当前调用链以 [Code_Architecture_CN.md](Code_Architecture_CN.md) 和 Production 源码为准，进度以 [Development_Status_CN.md](Development_Status_CN.md) 为准。

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
