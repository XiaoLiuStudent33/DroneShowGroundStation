# Legacy BGC2 Route Upload Audit

Status: AUDIT / REFERENCE
Authority: Legacy BGC2源码证据，不是DroneGCS当前架构Authority
Last Updated: 2026-08-14
Applies To: BGC2 Route conversion/upload compatibility research
Superseded By For New Design: [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)

## 1. Purpose 与证据等级

本报告固化已经完成的BGC2只读源码审计结果，供新DroneGCS协议兼容和风险判断。它不授权照搬Python、asyncio、Blender对象或Legacy默认参数。

证据等级：

- **Source Fact**：旧Production源码调用链直接证明。
- **Inference**：由调用关系推断，但飞控端没有完整源码证明。
- **Unknown**：现有BGC2源码无法确认。

## 2. Executive Summary

Legacy链：

```text
NPZ
-> worker.transwp
-> one n.waypoints per RouteSlot
-> QGC WPL parser / MissionItemProtocol
-> mission binary
-> per-drone concurrent ftp_upload
-> MAVLink FILE_TRANSFER_PROTOCOL
-> @MISSION/mission.dat
-> flight controller
```

关键事实：

- Conversion和Upload分离。
- `.waypoints`落盘并在上传时重新读取。
- 上传的不是WPL文本原样bytes，而是Mission binary。
- 网络目标由Remote IP + UDP Source Port区分，不依赖SYSID唯一性。
- Legacy缺少正式RouteAssignment Authority和可靠的端到端Mission Ready证明。

## 3. NPZ — Source Facts

- Reader：Python NumPy。
- Required arrays：`position`、`color`；可有`range`。
- `position`：`[Frame][Drone*3]`。
- `color`：`[Frame][Drone*4]`。
- RouteSlot n / Frame f：

```text
X = position[f][n*3+0]
Y = position[f][n*3+1]
Z = position[f][n*3+2]
R = color[f][n*4+0]
G = color[f][n*4+1]
B = color[f][n*4+2]
A = color[f][n*4+3]
```

- X=East meters、Y=North meters、Z=Relative Up meters。
- RGB输入为float 0..1，Alpha不进入Waypoint业务灯光。
- RouteSlot为0-based，slot n生成`n.waypoints`。

## 4. Stake / Alignment — Source Facts and Gaps

旧系统存在桩基、编号、位置阈值、忽略编号和忽略定桩等工作流，但没有正式、独立、revisioned的RouteAssignment数据模型。

缺失：

- RouteSlot unique ownership。
- duplicate RouteSlot rejection的统一Authority。
- `DroneID <-> RouteSlot`事务化映射。
- replacement owner transition。

因此新系统只复用“真实无人机与Route起点需要对齐”的业务语义，不照搬旧状态分散方式。

## 5. NPZ -> Waypoint — Source Facts

真实入口：Blender转换/上传Operator调用worker转换逻辑，`worker.transwp`按RouteSlot生成文件。

坐标转换：

```text
Latitude  = OriginLat + Y / 6378137 * 180 / PI
Longitude = OriginLon + X / 6378137 * 180 / PI / cos(OriginLatRadians)
Altitude  = Local Z
```

Mission Frame为`MAV_FRAME_GLOBAL_RELATIVE_ALT`，因此Altitude不加OriginAltitude。

## 6. QGC WPL 110 — Source Facts

每个RouteSlot一个文本文件：

```text
QGC WPL 110
```

12列对应：

1. sequence
2. current
3. frame
4. command
5. param1
6. param2
7. param3
8. param4
9. latitude
10. longitude
11. relative altitude
12. autocontinue

Legacy position item使用`MAV_CMD_NAV_WAYPOINT`与`MAV_FRAME_GLOBAL_RELATIVE_ALT`。

RGB写入param1/param2/param3，转换到uint8语义；Alpha丢弃。这不是标准NAV_WAYPOINT灯光语义，属于custom firmware compatibility behavior。

## 7. File Lifecycle — Source Facts

- 一个RouteSlot一个`n.waypoints`。
- 文件名代表Route/NPZ slot，不天然代表新系统DroneID。
- 文件落盘后可与转换阶段分离上传。
- 上传阶段重新打开`.waypoints`并解析。
- 实际上传前存在二次Mission编码。
- 旧转换不是适合万机的streaming架构，存在全量NPZ/大中间对象风险。

## 8. Mission Binary — Source Facts

上传bytes是：

```text
10-byte mission header
+ N * 38-byte MISSION_ITEM_INT records
```

不是QGC WPL文本原样上传。

`MissionItemProtocol`参与WPL解析和Mission Item编码；它不是独立的MAVLink Mission Protocol上传链。

## 9. MAVFTP — Source Facts

- MAVLink message：`FILE_TRANSFER_PROTOCOL #110`。
- Opcodes：CreateFile、WriteFile、TerminateSession。
- Remote path：`@MISSION/mission.dat`。
- target_system=0、target_component=0。
- 最终网络路由仍由每机Remote IP + UDP Source Port决定。

Legacy参数：

| UI概念 | Source-confirmed role |
|---|---|
| block/write size 228 | 每个Write request数据payload大小 |
| block count/window 16 | 每机允许pipeline中的Write窗口 |
| packet/mission batch 5001 | Mission item分区规模 |
| max upload 50 | 同时active drone upload并发限制 |

这些值是旧现场经验，不是DroneGCS Production默认值。

## 10. Concurrency — Source Facts

Legacy使用per-drone upload tasks和semaphore限制并发。`upload_with_semaphore`限制同时上传的Drone任务，不是Fleet identity或MAVLink SYSID限制。

风险：

- 高thread/task数。
- 大量文件IO。
- 50×16潜在突发包。
- 缺少全局packet budget和backpressure。
- retry/timeout不完整时可能形成retry storm。

## 11. ACK / Retry / Verification

Source-confirmed缺口：

- 没有可靠完整retry策略。
- 没有Route CRC闭环。
- 没有remote readback verification。
- 没有可靠Terminate ACK completion语义。
- 没有Mission apply verification。
- “文件发送完成”不能证明“Mission Ready”。

## 12. Mission Timing — Critical Unknown

Legacy UI使用`MISSION_CURRENT.seq / 10`显示秒数，但源码没有证明Flight Controller实际按10Hz执行Mission。

结论：

- 不能把10Hz写成新系统SampleRate。
- NPZ本身不提供可靠SampleRate metadata。
- 真实Waypoint timing、speed/delay/time-control由定制固件语义决定。

## 13. Firmware Dependency

判断：**LIKELY / CONFIRMED CUSTOM SEMANTICS**。

依据：

- RGB占用NAV_WAYPOINT param1/2/3。
- `@MISSION/mission.dat`消费链未由标准行为证明。
- Mission timing未由标准ArduPilot语义证明。
- 旧系统存在custom MAV_CMD_USER/time-control相关业务。

BGC2 sender源码不能证明飞控如何消费RGB和Mission file，真实固件审计仍是必要Gate。

## 14. Scale Risks

| Dimension | Legacy risk |
|---|---|
| Memory | NPZ与大量中间对象/字符串 |
| Disk IO | per-slot大量文本文件 |
| Thread/task | per-drone tasks + semaphore |
| Bandwidth | active uploads × write window burst |
| Retry | 无全局budget/backoff |
| Progress | 缺少正式job authority |
| Verification | transfer不等于ready |
| Identity | mapping缺少独立authority |

## 15. Reuse Conceptually

- NPZ -> per-RouteSlot auditable artifact。
- Conversion和Upload分离。
- Remote endpoint路由。
- Mission binary编码边界。
- MAVFTP Create/Write/Terminate流程。
- Global active limit和per-drone window概念。

## 16. Do Not Copy Directly

- Blender/Python/asyncio结构。
- Client.id身份假设。
- 分散assignment状态。
- 全量Unicode waypoint矩阵。
- 无验证的completion语义。
- 228/16/5001/50硬编码默认值。
- 10Hz timing假设。

## 17. Summary Table

| Item | Result |
|---|---|
| NPZ Loader | FOUND |
| Stake Alignment workflow | FOUND, no formal authority |
| Drone ↔ Route formal mapping | NOT FOUND |
| Waypoint Generator | FOUND |
| QGC WPL 110 | YES |
| Waypoint files written to disk | YES |
| File per RouteSlot | YES |
| Streaming conversion | NO |
| Upload reads disk waypoint file | YES |
| MAVLink FILE_TRANSFER_PROTOCOL | YES |
| FTP local file | `n.waypoints`, parsed/encoded before transfer |
| FTP remote path | `@MISSION/mission.dat` |
| FTP opcodes | CreateFile, WriteFile, TerminateSession |
| MissionItemProtocol relation | parser/encoder, not separate upload transport |
| ACK handling | PARTIAL/WEAK |
| NAK handling | PARTIAL |
| Reliable retry | NO |
| Timeout | YES, legacy policy |
| CRC/integrity | NO reliable end-to-end verification |
| Upload resume | NO |
| Mission timing | UNKNOWN |
| Standard ArduPilot compatible | PARTIAL/UNKNOWN |
| Custom firmware dependency | LIKELY |

## 18. Unknowns

- Exact flight-controller mission-file apply trigger。
- Exact Mission Ready response。
- Actual show timing implementation。
- RGB consumption in firmware。
- custom time-control and MAV_CMD_USER behavior。
- remote CRC/readback support on deployed firmware。

本Audit是证据文档，不是当前架构Authority。
