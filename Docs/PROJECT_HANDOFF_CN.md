# DroneGCS 项目交接速览

Status: CURRENT SUPPORTING
Last Updated: 2026-08-24
Baseline: `master` / `a494bd14dae00a5b4c24055a426ca1cee98e37f0` / `Phase4.2`

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

Phase 2A～2G、RouteUploadManager、UI-3C、MissionProtocol 硬件垂直链、MissionTransport abstraction、MissionFtpCompiler、MAVFTP Writer、Hardware Harness 和 Quick Test 已实现。

当前生产链仍是：

```text
UE Route Workflow
-> DSP-GCS Upload Job
-> ConsoleUploadJobAuthority
-> RouteUploadManager
-> MissionPreparationWorker
-> MissionProtocolTransport
-> DroneSession endpoint sender
-> ArduPilot/APM
```

MAVFTP 已存在实验/硬件验证链，但没有接管生产 `RouteUploadManager`。

## 4. 当前暂停点

```text
Phase 2I-C.3.C.3 = DONE
Phase 2I-C.3.D = NOT STARTED
```

下一阶段需要把现有分段能力接入配置和 Quick Test，并补齐 retry/timeout、逐段日志、metrics 与真实多段验证。`segmentCapacity` 当前以 Wire Mission Items 计数，包括 Reserved Home。

## 5. 保护规则

- 不创建第二套 Fleet、RouteAssignment、segment planner 或 network authority。
- 不把 MAVFTP 写成当前生产默认。
- 不把 7000 items 的用户观察写成固定飞控上限。
- 不在未授权时运行 `--mavftp-test`、write probe 或真实 Mission upload。
- 不将 Fleet 重复 ONLINE 日志诊断混入 MAVFTP segmentation。
- 不把 `All Write ACKed` 等同于 `Mission Ready`。

## 6. 当前证据

Console 四组专项测试和 Full ConsoleGCSTests 已于 2026-08-24 使用已有 Release 测试程序通过；未重新 Build、未运行 UE Automation、未运行真实飞控上传。硬件 1000～6000 PASS 与 7000 NAK 2 均为用户确认且没有仓库日志。

详细测试边界见 [Test_Architecture_CN.md](Test_Architecture_CN.md)。
