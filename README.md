# DroneShowGroundStation

无人机表演地面站项目。

当前正式工作目录：

C:\Users\13092\Documents\DroneGCS

## 工程结构

- `ConsoleGCS/`
  - C++ 后端地面站
  - MAVLink / UDP
  - Drone Session / Command
  - DSP-GCS Console 端

- `UnrealGCS/`
  - Unreal Engine 5.5.4 前端
  - Fleet UI
  - DSP-GCS UE 端
  - Cesium / Niagara 可视化

- `Shared/`
  - Console 与 Unreal 共用的 DSP-GCS Protocol / Transport

- `Docs/`
  - 当前架构、测试和维护文档

- `Tools/`
  - 辅助工具

## 当前架构文档

### 最高架构准则

`Docs/Architecture_Freeze_CN.md`

当前 Production 架构已经冻结。
除明确 Bug、新功能需求或有测量证据的性能瓶颈外，
不要随意重构核心边界。

### 当前代码架构

`Docs/Code_Architecture_CN.md`

用于了解：

- Console / UE Production 调用链
- MAVLink 数据流
- DSP-GCS
- Fleet
- Command
- Cesium / Niagara

### Production 类职责

`Docs/Production_Responsibility_Audit_CN.md`

用于查询某个 Production 类：

- 为什么存在
- 负责什么
- 为什么需要保持独立

### 测试体系

`Docs/Test_Architecture_CN.md`

Console：

- `ConsoleGCSTests` = Regression Tests
- `DSPGCSAcceptance` = Acceptance Tool

Unreal：

- `Private/Tests/` = Automation Regression Tests

## 核心架构原则

Drone Identity：

`Remote IP + UDP Source Port`

MAVLink SYSID 不是 Fleet 唯一身份。

Console Session：

`DroneManager → DroneSession → DataManager`

Production Command：

`DSP Receiver → CommandManager → DroneSession → CommandDispatcher → MavlinkCommand`

UE Telemetry：

`DSP Transport → Production Provider → GCSServerAdapter → FleetManager`

Visualization：

`FleetManager → DroneFleetVisualizationActor → Cesium → Niagara`

## 配置

Console 本机运行配置：

`ConsoleGCS/gcs-config.ini`

该文件属于本机 Deployment 配置，不提交到 Git。

示例：

`ConsoleGCS/gcs-config.ini.example`

DSP-GCS Console 配置：

`ConsoleGCS/config/DSPGCSTransport.ini`

Unreal 配置：

`UnrealGCS/Config/DefaultGame.ini`

具体配置职责以：

`Docs/Architecture_Freeze_CN.md`
和
`Docs/Code_Architecture_CN.md`

为准。

## 历史资料

历史设计和旧平台分析统一放在：

`Docs/History/`

历史文档不得作为当前 Production 架构依据。

---

当前实现以：

1. `Docs/Architecture_Freeze_CN.md`
2. 当前实际代码

为最终准则。