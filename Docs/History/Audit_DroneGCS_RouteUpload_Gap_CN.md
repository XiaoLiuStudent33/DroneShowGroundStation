# DroneGCS Route Upload Current Architecture & Gap Audit

Status: ARCHIVED / HISTORICAL
Current Authority: [../Route_Upload_Architecture_CN.md](../Route_Upload_Architecture_CN.md)
Do Not Use As Current Implementation State

Authority: 2026-08-14 源码差距审计快照，不是当前架构 Authority
Last Updated: 2026-08-14
Applies To: Route Upload规划证据
Architecture Authority: [../Route_Upload_Architecture_CN.md](../Route_Upload_Architecture_CN.md)

## 1. Executive Summary

在 2026-08-14 审计快照中，DroneGCS仅具备Route Upload前半段基础：Native NPZ、Local ENU RouteData、Show Origin、Playback、Route Import、Fleet/Session/Remote Endpoint、DSP-GCS和shared UDP sender。

该历史快照当时尚缺少：RouteAssignment、Waypoint artifacts/validation、QGC parser、Mission encoder、MAVFTP client、FTP ingress、UploadManager、job/progress DSP messages、Firmware Capability和 Mission Ready verification；其中多项已在后续版本实现，当前状态必须查看上方 Current Authority。

## 2. Current Foundations

### KEEP

- `URoutePreviewSubsystem`和`FRoutePreviewData`。
- `UFleetManagerSubsystem`真实Fleet Authority。
- `DroneManager -> DroneSession` endpoint Session。
- DSP localhost UDP Transport。
- shared MAVLink socket/sender。
- one Niagara Fleet architecture。

### EXTEND

- Fleet telemetry quality/freshness字段。
- Visualization增加Live + Route Overlay channel。
- DSP schema增加Route Upload job/progress。
- Ingress增加FILE_TRANSFER_PROTOCOL reply routing。
- DroneSession增加active upload bridge和capability。
- shared sender增加调度/backpressure边界。

### NEW MODULE

- UE RouteAssignmentSubsystem。
- WaypointGenerationService。
- WaypointValidationService。
- RouteArtifactWorkspace/Manifest。
- Console RouteUploadManager。
- QGC Parser/MissionBinaryEncoder。
- MavFtpClient。
- FirmwareCapability。

## 3. Route Data Audit

- Frame-major：`FrameIndex * DroneCount + RouteSlotIndex`。
- Position：`FVector3f` Local ENU。
- Color：`FFloat16Color`。
- RouteSlot 0-based且不是DroneID。
- 可O(1)读取单sample；单RouteSlot轨迹是strided，可O(FrameCount)直接遍历，无需复制。
- 9100×1000 final route storage约173.57 MiB。
- 10000需chunked/sharded route store。

## 4. Fleet / Assignment Gap

FleetManager已有DroneID、GPS、fix、altitude、online和stable order，但RouteAssignment需要补充位置时间戳、freshness、RTK/accuracy/readiness等质量字段。

Assignment推荐在UE Route domain，使用同Show Origin下的double precision Local ENU比较真实Drone与Route Frame0。禁止使用UE World、SYSID或particle index。

## 5. Visualization Gap

当前`EGCSVisualizationMode`为Live/RoutePreview互斥。Live + Route Overlay需要同一`NS_DroneFleet`内独立Route arrays和target emitter，不需要第二Niagara System。

## 6. Artifact Gap

当前仓库没有Show Workspace、ArtifactSet、Manifest、READY或Waypoint validation。未来必须建立allowlisted workspace和immutable hash contract，防止Console读取任意路径。

## 7. MAVLink Capability

Generated headers已包含：

- FILE_TRANSFER_PROTOCOL #110
- MISSION_ITEM_INT #73
- MISSION_CURRENT #42
- MISSION_COUNT #44
- MISSION_ACK #47
- MAV_MISSION_TYPE_MISSION
- MAVFTP opcode enums

这些只证明协议类型可编译，不证明Production client存在。

## 8. Console Gap

现有CommandDispatcher是32项短命令队列、单pending、COMMAND_ACK关联、3000ms timeout、一次retry。它不适合长生命周期、多packet、windowed upload。

推荐：RouteUploadManager全局调度，MavFtpClient每active per-drone job，DroneSession只提供endpoint-bound send和reply routing。

## 9. DSP Gap

SchemaVersion=3，只有TelemetryBatch、CommandRequest、CommandResultBatch。Route Upload需要独立Job消息，DSP只发送受控artifact descriptor，不发送完整mission bytes。

## 10. Verification Gap

| Verification | State |
|---|---|
| Local artifact hash | NEW MODULE可实现 |
| FTP ACK/NAK | header可用，Production未实现 |
| Terminate ACK | requires hardware test |
| Remote CRC | opcode存在，requires firmware test |
| Remote file size/readback | UNKNOWN |
| Mission Count/readback | requires implementation/test |
| Mission apply/ready | UNKNOWN/firmware dependent |
| Mission timing | UNKNOWN |

## 11. Gap Matrix

| Capability | Current State | Decision | Owner |
|---|---|---|---|
| NPZ Loader | complete | KEEP | RoutePreviewSubsystem |
| Route Data | complete | KEEP | RoutePreviewSubsystem |
| Route Preview | complete | KEEP | RoutePreviewSubsystem |
| Show Origin | complete | KEEP | RoutePreviewSubsystem |
| SampleRate | explicit preview metadata | KEEP + gate | RoutePreviewSubsystem |
| Live Visualization | complete | KEEP | Fleet/Visualization |
| Route Overlay | absent | EXTEND | same Visualization/Niagara |
| RouteAssignment | absent | NEW MODULE | UE Route domain |
| Assignment Validation | absent | NEW MODULE | UE Route domain |
| Waypoint Generator | absent | NEW MODULE | UE Route service |
| Waypoint File Writer | absent | NEW MODULE | UE Route service |
| Waypoint Validation | absent | NEW MODULE | UE validator |
| Manifest/READY | absent | NEW MODULE | UE workspace |
| QGC Parser | absent | NEW MODULE | Console |
| Mission Encoder | absent | NEW MODULE | Console |
| MAVFTP Client | absent | NEW MODULE | Console |
| FTP ACK/NAK | ingress ignores | EXTEND | Console ingress/client |
| Retry/Timeout | short command only | NEW POLICY | UploadManager/client |
| Verification | absent | NEW MODULE/POLICY | Console |
| Upload Queue/Scheduler | absent | NEW MODULE | RouteUploadManager |
| Upload Progress | absent | NEW MODULE | Console + DSP publish |
| DSP Route messages | absent | EXTEND | Shared protocol |
| Shared UDP sender | exists | KEEP + EXTEND | Console network |
| Firmware capability | absent | NEW MODULE | Console |
| Route Operations UI | absent | EXTEND | UE |
| Replacement hook | absent | DEFER | Assignment domain |

## 12. Recommended Flow

```text
Import -> Preview -> Overlay -> Assignment -> Validate/Freeze Assignment
-> Generate -> Validate/Freeze Artifacts -> Create Job
-> Upload -> Verify -> Ready/Partial/Failed
```

每个Upload Job绑定ArtifactSetId、Manifest hash、Assignment revision和target DroneIDs。

## 13. Critical Blockers

### Development Now

- Artifact/path security contract。
- Assignment revision model。
- Mission binary fixture/format确认。
- DSP job schema边界。
- RouteUploadManager/MavFtpClient ownership。

### Real Drone Test

- `@MISSION/mission.dat` apply。
- target metadata 0/0。
- FTP sequence/window/Terminate/CRC。
- RGB custom semantics。
- Mission timing。

### Production Release

- Firmware capability negotiation。
- Mission Ready verification。
- End-to-end integrity。
- 1000-drone load/retry-storm测试。
- 作业恢复与DroneID跨重启语义。

## 14. Recommended Development Order

Phase 2A Overlay -> 2B Assignment -> 2C Artifact Workspace -> 2D Generator -> 2E Validation -> 2F Parser/Encoder -> 2G DSP Job Protocol -> 2H UploadManager -> 2I MavFtpClient/Ingress -> 2J Firmware/Verification -> 2K Production UI。

## 15. Direct Answers

- RouteAssignment：UE独立Subsystem。
- Waypoint Generator：UE独立纯C++ Route service。
- `.waypoints`：UE生成，Console读取并上传。
- Mission Encoder/MAVFTP/UploadManager：Console。
- shared UDP sender：复用，增加调度与backpressure。
- DroneSession：扩展endpoint route/capability，不拥有全局Job。
- CommandDispatcher：不承载Route Upload。
- RoutePreview：核心KEEP，Overlay最小EXTEND。
- Niagara Overlay：同System第二Route target channel/emitter。
- 1000 Route RAM：当前可接受baseline。
- 10000：需要chunked/sharded storage。
- Mission timing unknown：不阻塞框架，阻塞Production Ready。
- Legacy 228/16/5001/50：保留概念，数值全部重测。
- Replacement：动态Owner/State/Revision/Timestamp扩展点，功能DEFER。

本Audit是当前差距证据，不是Route Upload Architecture Authority。
