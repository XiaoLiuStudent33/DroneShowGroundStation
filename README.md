# DroneGCS

无人机表演地面站生产项目。

Status: CURRENT
Last Updated: 2026-08-24

## 系统边界

- 前端：Unreal Engine 5.5.4，负责可视化与操作工作流。
- 后端：ConsoleGCS C++，负责 MAVLink、Session、Flight Controller 网络与真实命令。
- 正式飞控：ArduPilot / APM，不是 PX4。
- 同机生产边界：DSP-GCS localhost UDP。
- Fleet identity：`Remote IP + UDP Source Port`；MAVLink SYSID 不是 Fleet 唯一身份。

```text
UnrealGCS
    <-> DSP-GCS localhost UDP
ConsoleGCS
    <-> MAVLink / MAVFTP
ArduPilot / APM
```

## 当前状态

Route Upload 框架已实现，包括 Live + Route Overlay、RouteAssignment、Artifact Workspace、Waypoint 生成与验证、Frozen Assignment、DSP Upload Job、Console `RouteUploadManager` 和 Mission Transport 抽象。

当前生产上传链仍使用：

```text
RouteUploadManager
-> MissionProtocolTransport
-> ArduPilot
```

MAVFTP 已具备 `MavFtpClient`、FTP ACK/NAK ingress、`MissionFtpCompiler`、分段 writer、Hardware Harness 和 Quick Test，但尚未接管生产 `RouteUploadManager`。下一阶段是 `Phase 2I-C.3.D` 配置化多段 MAVFTP 上传。

`Production Mission Ready` 尚未证明；真实 Mission timing、RGB waypoint、`@MISSION/mission.dat` apply 和最终 ready verification 仍属于固件未知项。

## 文档阅读顺序

1. [文档索引](Docs/README_CN.md)
2. [Architecture Freeze](Docs/Architecture_Freeze_CN.md)
3. [当前代码架构](Docs/Code_Architecture_CN.md)
4. [开发状态](Docs/Development_Status_CN.md)
5. [Route Upload Architecture](Docs/Route_Upload_Architecture_CN.md)
6. 当前任务相关专项文档；Audit 和 `Docs/History/` 只作证据或历史参考。

## 目录

- `ConsoleGCS/`：C++ 后端、MAVLink、MAVFTP、Session、Command、Route Upload。
- `UnrealGCS/`：UE 5.5.4 前端、Fleet UI、Route Workflow、Cesium/Niagara。
- `Shared/`：DSP-GCS Protocol、Route Artifact、Waypoint 和 Mission wire contract。
- `Docs/`：当前 Authority、状态、专项架构、Audit 与 History。
- `Tools/`：独立辅助工具，不属于 DroneGCS Production Authority。
