# DroneGCS Development Status

Status: CURRENT
Authority: 当前实现状态与开发顺序
Last Updated: 2026-08-14
Applies To: 新开发者、任务交接、阶段规划

## 1. DONE / IMPLEMENTED

- Architecture Freeze和Production/Developer边界。
- Console多Remote Endpoint discovery。
- `Remote IP + UDP Source Port` Fleet identity。
- Offline Session保留和同endpoint恢复。
- MAVLink telemetry、mode、GPS、battery、online/offline。
- UE/Console DSP-GCS localhost UDP Production链。
- 1000-drone snapshot/chunk/reassembly。
- UE FleetManager、stable DroneOrder、详情选择和命令多选。
- Cesium GPS/Route坐标转换。
- one Actor / one Niagara System fleet visualization。
- Stable particle mapping、DisplayID、SelectedMask、TopDown-only labels。
- Marquee、camera lock、viewport coordinate alignment。
- Orthographic BTN_ZCamera explicit toggle、fit、wheel center zoom、MMB pan。
- 7色selected-drone UE preview。
- Real selected-drone LED_CONTROL #186 command；硬件测试PASS。
- Native NPZ loader、1000×9100 RouteData、offline preview。
- RoutePreview frame seek、Play/Pause/Stop、Previous/Next、Scrub、time/frame UI。
- Runtime Windows NPZ file picker和RouteImport popup。
- Fleet/Detail independent toggle和初始Collapsed。

## 2. CURRENT / NEXT

开发顺序：

1. **Phase 2A — Live + Route Overlay**
   保持one Niagara System，同时显示Real Drone和Route Target。

2. **Phase 2B — RouteAssignment**
   独立UE Authority、一对一、距离/质量、revision/freeze。

3. **Phase 2C — Artifact Workspace**
   ShowId、ArtifactSetId、source/waypoints/manifest、READY。

4. **Phase 2D — Waypoint Generator**
   Local ENU + Origin，per-RouteSlot流式QGC WPL 110。

5. **Phase 2E — Waypoint Validation**
   文件/字段/assignment/hash/manifest验证。

6. **Phase 2F — Console QGC Parser + Mission Encoder**

7. **Phase 2G — DSP Route Upload Job Protocol**

8. **Phase 2H — Console RouteUploadManager**

9. **Phase 2I — MavFtpClient + FTP Ingress**

10. **Phase 2J — Hardware/Firmware Capability + Verification**

11. **Phase 2K — Production Route Operations UI**

## 3. PLANNED, NOT IMPLEMENTED

- Live + Route Overlay。
- `URouteAssignmentSubsystem`。
- Waypoint Generator/Validator/Manifest。
- Route Upload DSP messages。
- RouteUploadManager。
- QGC parser/Mission binary encoder。
- MAVFTP state machine和ACK/NAK ingress。
- Upload progress/cancel/retry/failure isolation。
- Firmware capability model和Mission Ready verification。

## 4. DEFERRED

- 10000-drone chunked/sharded Route Store。
- Replacement Flight。
- Conflict predictor。
- 120-degree rejoin rule。
- Backup drone planning和safe rejoin trajectory。

## 5. CURRENT UNKNOWNS / GATES

- 真实Mission timing。
- custom firmware RGB waypoint execution。
- `@MISSION/mission.dat` apply semantics。
- time-control message behavior。
- custom MAV_CMD_USER behavior。
- remote integrity/Mission Ready verification。
- DroneID跨Console重启持久性。

这些Unknown不阻塞Route Upload框架开发，但阻塞Production Ready和真实表演验收。

## 6. Scale

- 1000 drones：当前稳定baseline。
- Route sample 9100×1000 final storage约173.57 MiB。
- 10000 fleet/route：future exploratory；需要chunked storage、bounded scheduling和重新性能测量。

## 7. Next Task Rule

进入新任务前依次阅读：

1. [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)
2. [Code_Architecture_CN.md](Code_Architecture_CN.md)
3. 本文件
4. 对应专项Architecture
5. Audit/History仅作证据

不要从History推断当前能力，不要把PLANNED模块写成已存在。
