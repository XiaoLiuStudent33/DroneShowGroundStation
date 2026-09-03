# DroneGCS

无人机表演地面站生产项目。

Status: CURRENT
Last Updated: 2026-09-03

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

当前生产上传链已冻结为：

```text
RouteUploadManager
-> MAVFTPMissionTransport
-> DroneSession endpoint-bound MavFtpClient
-> shared MAVLink sender/socket
-> ArduPilot
```

旧的标准 MAVLink Mission 上传 Transport 已退休并从生产源码、配置、CLI 与工程项删除。MAVFTP 失败会明确失败，不存在协议回退。

9100 Artifact Items / 9101 Wire Mission Items 的三段真实生产上传已通过 Count/Home/First/Last 验证，Job ElapsedMs=8829。真实 Mission execution timing 与 RGB waypoint 执行仍属于固件验证边界。

第一版可安装交付链已经完成：UE 5.5 Shipping 前端、可见 Console 后端、生产配置和运行库由联合启动器统一启动，并通过 NSIS 生成 `DroneGCS-Setup-1.0.0-x64.exe`。安装后的程序不读取开发机源码，也不要求 Visual Studio、UE 编辑器或 PowerShell。2026-09-03 的另一台电脑安装与启动结果为 `USER-CONFIRMED PASS`；该证据不自动等同于全部飞行、航线上传和固件行为验收。

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
- `Tools/Launcher/`：联合 payload 构建、可见后端进程管理和隔离 Smoke。
- `Tools/Installer/`：Shipping 暂存包校验与 NSIS 安装程序构建。
