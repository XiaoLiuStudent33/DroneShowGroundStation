# DroneGCS 项目交接速览

Status: CURRENT SUPPORTING
Last Updated: 2026-08-24
Baseline: `master` / `d2f933c6e4a180d3996452fee58827801dfd3cb1`

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
- UE 不直接向飞控发送 MAVLink。

## 3. 当前 Route Upload 状态

Phase 2A～2G、RouteUploadManager、UI-3C、MissionTransport abstraction、MissionFtpCompiler、MAVFTP Writer、Hardware Harness、Quick Test 与 MAVFTP production cutover 已实现。

当前唯一生产链是：

```text
UE Route Workflow
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
```

生产参数为 `segment_items=4000`、request/terminate timeout `750/2500 ms`、retry `3`、write window `16`。`segmentCapacity` 以包含 Reserved Home 的 Wire Mission Items 计数。

## 5. 保护规则

- 不创建第二套 Fleet、RouteAssignment、segment planner 或 network authority。
- 不恢复 Transport 选择器或协议 fallback。
- 不把 7000 items 的用户观察写成固定飞控上限。
- 不在未授权时运行 `--mavftp-test`、write probe 或真实 Mission upload。
- 不将 Fleet 重复 ONLINE 日志诊断混入 MAVFTP segmentation。
- 不把 `All Write ACKed` 等同于 `Mission Ready`。

## 6. 当前证据

Console Release x64 Solution Rebuild 已于 2026-08-24 通过（0 errors）484 warnings，ExitCode 0）；四组专项测试和 Full ConsoleGCSTests 全部通过。本轮未运行 UE Automation、未运行真实飞控上传。硬件 1000～6000 PASS 与 7000 NAK 2 均为用户确认且没有仓库日志。

详细测试边界见 [Test_Architecture_CN.md](Test_Architecture_CN.md)。
