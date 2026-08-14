# DroneGCS Network & DSP Architecture

Status: CURRENT
Authority: 当前网络身份、MAVLink ingress/outbound、DSP-GCS Production链
Last Updated: 2026-08-14
Applies To: `ConsoleGCS`、`Shared/DSPGCSProtocol`、UE Production Providers

## 1. Identity

Fleet network identity：

```text
Remote IP + UDP Source Port
```

MAVLink SYSID/COMPID不承担Fleet唯一身份。相同endpoint恢复同一DroneSession；不同endpoint即使SYSID相同也属于不同Drone。

当前DroneID由Console按发现顺序分配，在Console进程生命周期内稳定，尚未持久化到跨重启身份数据库。

## 2. MAVLink Socket

`MavlinkUdpReceiver`同时实现：

- 一个Production bound socket的non-blocking receive。
- `IMavlinkDatagramSender::SendTo`。
- 向每个真实Remote Endpoint发送MAVLink bytes。

`UdpClient`在多机Production中只是绑定某个DroneSession remote IP/port的adapter，不创建per-session socket。

## 3. Ingress

```text
MavlinkUdpReceiver
-> MavlinkReadOnlyIngress
-> HEARTBEAT: MavlinkDiscoveryService
-> known endpoint COMMAND_ACK: DroneSession
-> known endpoint telemetry: DroneSession
```

Datagram只解码一次。非HEARTBEAT消息不能创建Session。

当前`FILE_TRANSFER_PROTOCOL #110`未被路由，未来必须在known endpoint之后交给RouteUpload backend，而不是作为telemetry或COMMAND_ACK处理。

## 4. Session 与 Online

```text
DroneManager
-> endpoint index
-> DroneSession
   |-> DataManager snapshot
   |-> CommandDispatcher
   `-> endpoint-bound UdpClient
```

Heartbeat timeout只使Session Offline。Offline Session、DroneID、SessionOrder和最后遥测保留。

## 5. Current Command Network

```text
UE intent
-> DSP-GCS CommandRequest
-> Console receiver
-> CommandManager fan-out by DroneID
-> DroneSession endpoint
-> CommandDispatcher
-> MavlinkCommand
-> shared sender
```

CommandDispatcher适合短命令和COMMAND_ACK，不适合多包长生命周期Route Upload。

## 6. DSP-GCS Production

当前正式同机UDP：

| Side | Bind | Remote |
|---|---|---|
| Console | `127.0.0.1:12001` | `127.0.0.1:12002` |
| UE | `127.0.0.1:12002` | `127.0.0.1:12001` |

`DSPGCSLoopbackTransport`是Production transport，不是test fallback。

当前SchemaVersion=3，消息只有TelemetryBatch、CommandRequest、CommandResultBatch。Route Upload job/progress尚未定义。

## 7. Telemetry Snapshot

```text
DroneManager stable SessionOrder
-> Fleet Snapshot
-> bounded UDP chunks
-> UE worker decode/reassembly
-> complete Snapshot DTO
-> GCSServerAdapter GameThread handoff
-> FleetManager atomic apply
```

不同Session/Snapshot/FleetRevision的chunks不得混合；未收齐时不能提交partial Fleet。

## 8. Shared Sender Scale Boundary

当前sender：

- 同一socket。
- 同步`sendto`。
- endpoint精确路由。
- 没有专用outbound queue。
- 没有backpressure或bandwidth limiter。
- 没有packet batching。
- 单主循环当前提供调用顺序，但UDP不保证网络交付顺序。

Route Upload必须KEEP socket与endpoint authority，并EXTEND：

- centralized upload scheduler。
- global packet budget。
- per-drone write window。
- retry backoff。
- 明确发送串行化或主循环enqueue边界。

禁止新socket、新network thread per drone或第二套Fleet registry。

## 9. Future Route Upload DSP Boundary — PLANNED

UE与Console同机，Waypoint artifacts落盘，因此DSP应发送受控作业描述，不传完整Mission bytes：

```text
ShowId
ArtifactSetId
Manifest hash
Assignment revision
target DroneIDs
operation/correlation id
```

Console在allowlisted WorkspaceRoot下canonicalize并读取artifact。拒绝绝对任意路径、path traversal和READY/hash不匹配。

## 10. Configuration Authority

- Console MAVLink bind/timeout/safety：`gcs-config.ini`。
- Console DSP endpoints：`config/DSPGCSTransport.ini`。
- UE DSP/developer switches：`DefaultGame.ini`。
- Wire constants：`Shared/DSPGCSProtocol`。
- 每机Remote Endpoint：真实入站datagram。

不得在文档或代码中把SYSID写成唯一无人机身份，也不得建立Production Mock fallback。
