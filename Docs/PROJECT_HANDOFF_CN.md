# DroneGCS 项目交接速览

Status: CURRENT SUPPORTING
Last Updated: 2026-08-25
Baseline: `master` / `17b679265c77d1c2306e1b500574b121b29e525c`

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

## 4. 当前暂停点

```text
Phase 2I-C.3.D.1-D.3 = DONE
Phase 2I-C.3.D.4 = MAVFTP SINGLE-TRANSPORT FREEZE
Phase 2I-C.3.E.0 = AUTOMATIC SELECTED-DRONE ASSIGNMENT / SERIAL MULTI-TARGET DONE
Phase 2I-C.3.E.0.1 = CONSOLE RENDER INTEGRITY DONE
```

生产参数为 `segment_items=4000`、request/terminate timeout `750/2500 ms`、retry `3`、write window `16`。`segmentCapacity` 以包含 Reserved Home 的 Wire Mission Items 计数。

正常 UI Start 每次重新建立当前选择的 Working/Frozen Assignment，等待精确 Console Accepted，重新校验全部 Authority 后 exactly once 创建一个 Job。一个 Job 可含多个 Targets，但 E.0 强制 `MaxConcurrentTargets=1`。E.1 全局 outstanding cap、E.2 多机硬件验证尚未完成；Console 重启后的 DroneID 发现顺序风险为 DEFERRED/E.3。

Console 可观测性由 `ConsoleUI` 的单一 Renderer 线程负责。Producer 只入队 1024 条上限的结构化记录或发布 Fleet 快照；真实 Windows Console 使用动态尺寸 Dashboard，重定向、Pipe、文件、Tee 和 CI 使用无 cursor 控制的 Linear 输出。溢出通过 `DroppedLogCount` 可观测，terminal/error 优先于普通日志。E.1 尚未开始。

## 5. 保护规则

- 不创建第二套 Fleet、RouteAssignment、segment planner 或 network authority。
- 不恢复 Transport 选择器或协议 fallback。
- 不把 7000 items 的用户观察写成固定飞控上限。
- 不在未授权时运行 `--mavftp-test`、write probe 或真实 Mission upload。
- 不将 Fleet 重复 ONLINE 日志诊断混入 MAVFTP segmentation。
- 不把 `All Write ACKed` 等同于 `Mission Ready`。

## 6. 当前证据

Console Release x64 Solution Build 已于 2026-08-25 通过（0 errors、12 warnings、ExitCode 0）；六组专项测试和 Full ConsoleGCSTests 全部通过。UE Development/DebugGame Build 均通过，E.0 AutoStart 6/6 与合并相关回归 164/164 PASS。本轮未运行真实飞控上传。硬件 1000～6000 PASS 与 7000 NAK 2 均为用户确认且没有仓库日志。

详细测试边界见 [Test_Architecture_CN.md](Test_Architecture_CN.md)。
