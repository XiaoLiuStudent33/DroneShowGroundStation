# DroneGCS Network & DSP Architecture

Status: CURRENT
Authority: 当前网络身份、MAVLink ingress/outbound 与 DSP-GCS Production 链
Last Updated: 2026-08-24

## 1. Identity

Fleet network identity：

```text
Remote IP + UDP Source Port
```

MAVLink SYSID/COMPID 不是 Fleet 唯一身份。相同 endpoint 恢复同一 DroneSession；不同 endpoint 即使 SYSID 相同也属于不同 Drone。DroneID 当前仅保证 Console 进程生命周期内稳定。

## 2. Shared MAVLink Socket

`MavlinkUdpReceiver` 同时负责 non-blocking receive 与 `IMavlinkDatagramSender::SendTo`。`UdpClient` 是绑定某个 DroneSession Remote Endpoint 的 adapter，不创建 per-session socket。

禁止为 Route Upload 建立第二套 socket、network stack 或 Fleet registry。

## 3. Ingress

```text
MavlinkUdpReceiver
-> MavlinkReadOnlyIngress
   |-> HEARTBEAT: MavlinkDiscoveryService
   |-> known endpoint telemetry / COMMAND_ACK: DroneSession
   `-> known endpoint FILE_TRANSFER_PROTOCOL: endpoint-bound MavFtpClient service
```

Datagram 只解码一次。非 HEARTBEAT 消息不能创建 Session；FTP reply 只能路由到已知 endpoint，不得通过 SYSID 创建或查找 Fleet identity。

## 4. Session 与 Online

```text
DroneManager
-> endpoint index
-> DroneSession
   |-> DataManager snapshot
   |-> CommandDispatcher
   |-> Mission verification service
   |-> optional MavFtpClient service
   `-> endpoint-bound sender adapter
```

Heartbeat timeout 只使 Session Offline。Offline Session、DroneID、SessionOrder 和最后遥测保留。

## 5. Short Command Network

```text
UE intent
-> DSP-GCS CommandRequest
-> Console receiver
-> CommandManager fan-out by DroneID
-> DroneSession / CommandDispatcher
-> shared sender
```

`CommandDispatcher` 适合短命令和 COMMAND_ACK；Route Upload 使用独立 Job/Transport lifecycle。

## 6. DSP-GCS Production

当前正式同机 UDP：

| Side | Bind | Remote |
|---|---|---|
| Console | `127.0.0.1:12001` | `127.0.0.1:12002` |
| UE | `127.0.0.1:12002` | `127.0.0.1:12001` |

`DSPGCSLoopbackTransport` 是 Production localhost transport，不是 Mock fallback。

当前已实现的协议域：

- base DSP-GCS Schema 3：TelemetryBatch、CommandRequest、CommandResultBatch。
- Frozen Assignment protocol Schema 2：Begin/Chunk/Commit/Abort/ACK transaction。
- Upload Job protocol Schema 2：Command/ACK、Snapshot request/response、Route page/delta。

Waypoint/Mission bytes 不通过 DSP 传输；DSP 传受控 identity、operation 与 state。

## 7. Frozen Assignment Network

```text
UE Frozen Assignment Sync
-> bounded DSP chunks
-> Console Receiver / TransactionAssembler
-> FrozenAssignmentStore
-> ACK / rejection
```

Transaction 校验 authority session、Fleet session、revision、digest、mapping uniqueness 与 freeze policy。

## 8. Upload Job Network

```text
UE Upload Job Subsystem
-> DSP Upload Command
-> Console Upload Job Receiver / Authority
-> RouteUploadManager executor
-> Snapshot / Route Page / Delta
-> UE StateMirror / Operator UI
```

Upload Job 绑定 ArtifactSet 与 Frozen Assignment identity。Console 只在 allowlisted workspace 读取 canonical Artifact，不接受任意绝对路径或 path traversal。

## 9. Route Upload Outbound

当前 Production：

```text
RouteUploadManager
-> MAVFTPMissionTransport
-> DroneSession endpoint-bound MavFtpClient
-> shared sender
```

显式 MAVFTP Hardware Harness 复用相同网络边界：

```text
MAVFTPMissionTransport
-> endpoint-bound MavFtpClient
-> shared sender
```

两条路径复用相同 Remote Endpoint 与 socket authority。生产 `RouteUploadManager` 已冻结为 MAVFTP-only；失败时明确失败，不切换其他协议。

## 10. Scale 与 Scheduling

`RouteUploadManager` 已提供 bounded preparation、max active uploads、per-target state、cancel/retry 与 failure isolation。当前 shared sender 仍是同步 `sendto`，UDP 不保证网络交付顺序。

未来优化必须保留现有 socket/identity authority，可扩展集中 packet budget、backpressure 或明确 enqueue 边界；禁止 per-drone thread/socket。

## 11. Configuration Authority

- Console MAVLink bind/timeout/safety：deployment config。
- Console DSP endpoints：`config/DSPGCSTransport.ini`。
- UE DSP/developer switches：`DefaultGame.ini`。
- Wire contracts：`Shared/DSPGCSProtocol`。
- 每机 Remote Endpoint：真实入站 datagram。

当前尚无 `[mavftp] segment_items / segment_retry_count / segment_timeout_ms` production 配置接线。
