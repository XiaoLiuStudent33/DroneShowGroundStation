# DroneGCS Route Upload Architecture

Status: PLANNED
Authority: Route Upload冻结设计方向；不代表模块已实现
Last Updated: 2026-08-14
Applies To: RouteAssignment、Waypoint Artifact、DSP Upload Job、Console MAVFTP
Evidence: [Audit_Legacy_BGC2_RouteUpload_CN.md](Audit_Legacy_BGC2_RouteUpload_CN.md)、[Audit_DroneGCS_RouteUpload_Gap_CN.md](Audit_DroneGCS_RouteUpload_Gap_CN.md)

## 1. Status Legend

- **IMPLEMENTED**：当前Production源码存在。
- **PLANNED**：冻结方向，尚未实现。
- **DEFERRED**：未来扩展，不进入当前阶段。
- **FIRMWARE UNKNOWN**：必须通过真实定制固件审计/硬件测试确认。

## 2. End-to-End Target

```text
NPZ Import                         IMPLEMENTED
-> Route Preview                  IMPLEMENTED
-> Live + Route Overlay           PLANNED
-> Route Assignment               PLANNED
-> Assignment Validation/Freeze   PLANNED
-> Waypoint Generation            PLANNED
-> Waypoint Validation            PLANNED
-> Artifact Freeze / READY        PLANNED
-> DSP-GCS Upload Job             PLANNED
-> Console RouteUploadManager     PLANNED
-> QGC Parser / Mission Encoder   PLANNED
-> MAVFTP                         PLANNED
-> @MISSION/mission.dat           LEGACY TARGET / FIRMWARE UNKNOWN
-> Verification                   PLANNED / FIRMWARE UNKNOWN
-> Mission Ready                  NOT CURRENTLY PROVABLE
```

UE不得直接发送MAVLink。ConsoleGCS始终是Flight Controller network authority。

## 3. Current Foundations — IMPLEMENTED

- Native NPZ loader，position/color/range validation。
- `FRoutePreviewData` frame-major compact storage。
- Local ENU X East、Y North、Z Relative Up。
- Show Origin和显式SampleRate。
- Offline 1000-drone preview、Playback、Scrub、Import UI。
- Fleet Remote Endpoint/DroneSession链。
- DSP-GCS localhost UDP。
- shared MAVLink sender。
- generated MAVLink headers包含MISSION_ITEM_INT和FILE_TRANSFER_PROTOCOL。

这些基础不等于已经具备Route Upload。

## 4. RouteAssignment — PLANNED

推荐Authority：UE `URouteAssignmentSubsystem`。

核心关系：

```text
DroneID <-> RouteSlotIndex
```

必须一对一并表达：

- DistanceMeters
- PositionQuality
- AssignmentState
- OwnerState
- Revision
- Timestamp
- PreviousOwner/TransitionReason扩展点

匹配空间：真实Drone WGS84转换到同一Show Origin下的double precision Local ENU，再与Route Frame 0比较。禁止使用UE World、particle index、SYSID或`DroneID == RouteSlotIndex`假设。

算法接口必须返回assignment、conflict、unassigned、vacant和quality；当前不冻结Nearest Neighbor/KD Tree/Hungarian/Greedy具体算法。

## 5. Waypoint Generation — PLANNED

Workflow Authority在UE，推荐独立纯C++ Route Core service。

输入：

- 原始Route Local ENU。
- Show Origin。
- Frozen Assignment Revision。
- 显式Timing/Firmware Profile。

输出：每个RouteSlot一个QGC WPL 110文件。

禁止绑定UMG、Niagara、Cesium World或Playback ticker。禁止从UE World反推GPS。

1000-drone NOW：基于已加载RouteData，按RouteSlot迭代Frame，使用reusable buffer和bounded workers流式写盘。

10000-drone FUTURE：chunked/sharded/memory-mapped route store，不能继续依赖全量NPZ常驻和全量Unicode matrix。

## 6. Legacy Compatibility Waypoint Target

当前仅记录已审计的Legacy兼容目标：

- Header：`QGC WPL 110`
- Columns：12
- Command：`MAV_CMD_NAV_WAYPOINT`
- Frame：`MAV_FRAME_GLOBAL_RELATIVE_ALT`
- Latitude/Longitude：Local ENU + Show Origin
- Altitude：Local Z，不加ShowOriginAltitude
- RGB：legacy/custom profile写入param1/param2/param3

RGB不是标准NAV_WAYPOINT语义，不能写成标准ArduPilot行为。

Preview SampleRate也不能证明实际Mission timing。Generator必须要求明确的Firmware/Timing Profile；未知时只能生成实验性工件，不能标记Production Ready。

## 7. Artifact Workspace — PLANNED

```text
WorkspaceRoot/
  Show_<ShowId>/
    source/
    waypoints/
    manifest/
      route-artifacts.json
      READY
```

Manifest至少包含：

- Source hash。
- Generator version。
- Route metadata和Origin。
- SampleRate/TimingProfile。
- AssignmentRevision。
- 每个文件relative path、size、hash、waypoint count。
- Firmware profile。
- Validation result。

READY后ArtifactSet immutable。Upload Job必须绑定ArtifactSetId、Manifest hash和AssignmentRevision。

## 8. Validation — PLANNED

UE `WaypointValidationService`至少检查：

- 文件数/RouteSlot数。
- Frame/Waypoint count。
- Sequence、Frame、Command。
- Latitude/Longitude range。
- Altitude finite。
- RGB range。
- Duplicate/Missing。
- Assignment completeness。
- File hash。

Console在信任边界再次执行防御性parser/hash检查，但不成为第二个Waypoint生成Authority。

## 9. DSP-GCS Upload Contract — PLANNED

应新增独立Job消息，而不是普通短命令：

- Create/SubmitUploadJob
- StartUpload
- CancelUpload
- RetryDrone
- UploadProgress
- PerDroneUploadResult
- UploadSummary/JobState
- CapabilityReport

DSP只传受控Artifact descriptor和operation metadata，不传整份Waypoint bytes，也不接受任意绝对文件路径。

## 10. Console Route Upload Backend — PLANNED

### RouteUploadManager

全局Job/Scheduler Authority：

- Queue
- Max active drone jobs
- Global packet budget
- Per-drone upload state
- Write window
- Retry/backoff
- Timeout
- Progress
- Cancel
- Failure isolation
- Overall summary

### MissionArtifactReader / QGC Parser

从allowlisted WorkspaceRoot读取READY/hash匹配的Artifact，并解析QGC WPL。

### MissionBinaryEncoder

Legacy target：10-byte mission header + N×38-byte MISSION_ITEM_INT records。这个格式在新实现前必须用真实固件和legacy fixture再次验证。

### MavFtpClient

每个active per-drone upload job一个实例；同一DroneSession最多一个active route transfer。负责CreateFile、WriteFile、TerminateSession、sequence、session、offset、window、ACK/NAK、timeout和retry。

`DroneSession`只提供endpoint-bound send、capability和reply routing，不持有全局job。

现有短命令`CommandDispatcher`保持不变，不承载整个upload。

## 11. MAVFTP Target — Legacy Confirmed, New Hardware Test Required

- Message：`FILE_TRANSFER_PROTOCOL #110`
- Opcodes：CreateFile、WriteFile、TerminateSession
- Remote path：`@MISSION/mission.dat`
- Legacy payload metadata：target_system=0、target_component=0

新DroneGCS必须复用shared UDP sender和Remote Endpoint authority。0/0只能存在于兼容profile，不能用于Fleet lookup。

## 12. Reliability States — PLANNED

```text
Pending
Preparing
Uploading
TransferComplete
Verifying
Ready
Failed
Cancelled
```

`All Write ACKed != Mission Ready`。

未来必须有：

- ACK/NAK correlation。
- timeout/retry/backoff。
- per-drone failure isolation。
- global concurrency和packet budget。
- cancel。
- local hash。
- firmware-supported remote verification。

## 13. Legacy Parameters

旧现场经验：

- Write payload = 228 bytes
- Write window = 16
- Mission batch = 5001 items
- Concurrent uploads = 50

只保留概念，不冻结数值。所有参数必须在当前shared sender、固件、WiFi网络和1000-drone压力测试下重新benchmark。

## 14. Firmware Capability — PLANNED / UNKNOWN

未来Console capability model需表达：

- firmware version/hash。
- MAVLink version。
- MAVFTP支持。
- MISSION_ITEM_INT支持。
- mission-file apply语义。
- waypoint RGB profile。
- mission timing profile。
- custom protocol version。

当前未知：

- 真实Mission timing。
- custom RGB执行。
- `@MISSION/mission.dat`如何apply。
- time-control message behavior。
- custom MAV_CMD_USER behavior。
- Mission Ready verification。

Legacy UI的`MISSION_CURRENT.seq / 10`只是一项旧显示假设，不是新系统10Hz Production事实。

## 15. Planned Operator Flow

```text
Import
-> Preview
-> Overlay
-> Assign
-> Validate Assignment
-> Freeze Assignment
-> Generate
-> Validate Files
-> Freeze Artifact
-> Create Upload Job
-> Upload
-> Verify
-> Ready / Partial / Failed
```

正式Operator UI继续在UE。Console只提供headless backend和diagnostics。

## 16. Replacement Flight — DEFERRED

Assignment未来必须支持：

```text
Drone A owns Slot
-> Fault
-> Slot Vacant
-> Backup B Reserved
-> Safe Rejoin
-> Owner transfer
```

仅冻结Owner/State/Revision/Timestamp/PreviousOwner/TransitionReason扩展点。本阶段不定义120-degree算法、补点轨迹、故障状态机或冲突预测。

## 17. Development Gates

不受Mission timing unknown阻塞：Overlay、Assignment、Artifact框架、Generator框架、Validation、Parser/Encoder单测、DSP job schema、MAVFTP状态机。

受阻：最终Waypoint timing语义、硬件表演验收、Production Mission Ready声明。
