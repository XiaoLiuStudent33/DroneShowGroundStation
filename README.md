# DroneGCS

无人机表演地面站生产项目。

Status: CURRENT
Last Updated: 2026-09-11

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

## 当前状态入口

当前源码基线、实现状态、已有验证、人工/真机验收、待办和发布就绪判断仅维护于 [Development_Status_CN.md](Docs/Development_Status_CN.md)。旧安装包、架构阶段和测试结果不得作为当前 HEAD 的完整验收结论。

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
