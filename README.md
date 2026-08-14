# DroneShowGroundStation

无人机表演地面站项目。

Status: CURRENT
Last Updated: 2026-08-14

## Start Here

完整文档入口：[Docs/README_CN.md](Docs/README_CN.md)

阅读顺序：

1. [Architecture Freeze](Docs/Architecture_Freeze_CN.md)
2. [Current Code Architecture](Docs/Code_Architecture_CN.md)
3. [Development Status](Docs/Development_Status_CN.md)
4. 任务相关专项Architecture
5. Audits与History

## Project Structure

- `ConsoleGCS/`：C++ backend、MAVLink/UDP、DroneSession、Command、DSP Console端。
- `UnrealGCS/`：UE 5.5.4 frontend、Fleet UI、Route Preview、Cesium/Niagara。
- `Shared/`：DSP-GCS Protocol、Wire Types和Transport boundary。
- `Docs/`：当前Authority、专项架构、状态、Audit和History。
- `Tools/`：独立辅助工具；不属于DroneGCS Production Authority。

## Frozen Principles

- UE = Frontend / Visualization / Operator Workflow。
- ConsoleGCS = Backend / MAVLink / Drone Network。
- 正式同机Transport = localhost UDP DSP-GCS。
- Fleet identity = Remote IP + UDP Source Port。
- SYSID不是Fleet unique identity。
- Offline Session保留，同endpoint恢复原Session/DroneID；跨Console重启持久性尚未保证。
- `GCSServerAdapterSubsystem`是唯一Network/Worker -> GameThread Fleet submit边界。
- `FleetManagerSubsystem`是UE real Fleet Authority。
- Visualization保持one Actor、one Niagara System、N particles。
- Production不得fallback到Mock。

## Current Capabilities

- Multi-drone telemetry/session/command/ACK。
- DSP-GCS Schema 3，含selected-drone LED command。
- 1000-drone Fleet/Cesium/Niagara。
- Fleet detail selection和command multi-selection。
- Marquee、Orthographic TopDown、wheel zoom、MMB pan。
- Drone ID、selection rim和7色LED preview/real command。
- Native NPZ Route Preview、Playback、Scrub和RouteImport UI。

## Current Next Phase

Route Upload前置开发从Live + Route Overlay和RouteAssignment开始。完整顺序见[Development_Status_CN.md](Docs/Development_Status_CN.md)和[Route_Upload_Architecture_CN.md](Docs/Route_Upload_Architecture_CN.md)。

Route Upload当前尚未实现；Mission timing和定制固件Mission语义仍未确认。

## Configuration

- Console deployment：`ConsoleGCS/gcs-config.ini`
- Console DSP：`ConsoleGCS/config/DSPGCSTransport.ini`
- UE：`UnrealGCS/Config/DefaultGame.ini`

具体Authority以Architecture文档和Production源码为准。

## History

`Docs/History/`只用于历史参考，不能作为当前Production架构依据。
