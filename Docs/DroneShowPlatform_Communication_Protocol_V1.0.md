# DroneShowPlatform UE ↔ ConsoleGCS 通信协议设计 V1.0

> 阶段：4.1 协议设计  
> 状态：设计基线，不包含网络实现  
> 适用组件：UnrealGCS、ConsoleGCS、未来 GCS Server / Simulator / Replay Provider  
> 协议标识：`DSP-GCS`  
> 协议版本：`1.0`  
> 日期：2026-08-03

---

## 1. 目标与非目标

本协议定义 UnrealGCS 与 ConsoleGCS 之间的标准化数据边界：

- ConsoleGCS → UnrealGCS：遥测数据；
- UnrealGCS → ConsoleGCS：命令请求；
- ConsoleGCS → UnrealGCS：命令处理和逐机 ACK 结果。

本阶段只定义逻辑消息、字段语义、版本和批量规则。

本阶段明确不实现：

- Socket、UDP、TCP、WebSocket 或 QUIC；
- 序列化库；
- UE Provider；
- Console 网络服务；
- MAVLink 新功能；
- 真实 ARM、TAKEOFF 或真机自动测试。

---

## 2. 模型边界

协议 DTO 是独立的传输模型，不等同于任一程序的内部模型。

```text
ConsoleGCS
  FlightData / DroneSession / ManagedDroneCommand
          │
          │ Console Adapter（未来）
          ▼
  TelemetryDTO / CommandDTO / ResultDTO
          │
          │ Transport（本阶段未选择、未实现）
          ▼
  UE Provider / GCSServerAdapter（未来）
          │
          ▼
UE
  FGCSFleetSnapshot / FGCSTelemetryBatch / FDroneDataSnapshot
```

约束：

1. ConsoleGCS 不包含 UE 类型、`UObject`、`FString` 或 `FDroneData`。
2. UnrealGCS 不包含 MAVLink 消息、APM Command ID 或 Console `FlightData`。
3. `DroneID` 是平台稳定身份；`SYSID` 只是链路/会话内的 MAVLink 身份。
4. DTO 不保存 UI Selection。选择状态属于 UE 客户端交互模型。
5. LED 表演状态、飞行任务状态、健康状态不得混用同一个字段表达。

---

## 3. 通用消息信封

所有消息使用统一 `MessageEnvelope`。字段名称是规范名称，具体 C++/UE 命名可遵循各自代码风格。

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `protocol` | string | 是 | 固定为 `DSP-GCS` |
| `protocolMajor` | uint16 | 是 | 不兼容变更版本，当前为 `1` |
| `protocolMinor` | uint16 | 是 | 向后兼容的增量版本，当前为 `0` |
| `messageType` | enum | 是 | `TELEMETRY_BATCH`、`COMMAND_REQUEST`、`COMMAND_RESULT_BATCH` |
| `schemaVersion` | uint16 | 是 | 当前消息 Payload 的结构版本，V1 均为 `1` |
| `sourceId` | string | 是 | 发送组件实例，例如 `ConsoleGCS-Station01` |
| `sessionId` | string | 是 | 本次进程或连接会话的 UUID |
| `sequence` | uint64 | 是 | 在 `sourceId + sessionId + messageType` 范围内单调递增 |
| `timestampUtcMs` | int64 | 是 | 发送端 UTC Unix Epoch 毫秒 |
| `correlationId` | string | 否 | 请求/响应关联；命令结果使用 CommandID |
| `fleetRevision` | uint64 | 否 | 发送端权威机群结构/状态版本 |
| `flags` | uint32 bitmask | 否 | Snapshot、Delta、Compressed、Final 等标记 |
| `payload` | object/bytes | 是 | 对应消息类型的 DTO |

### 3.1 时间语义

- 传输时间统一使用 UTC Unix Epoch 毫秒；
- 遥测项可以携带飞控采样时间 `sampleTimestampUtcMs`；
- `timestampUtcMs` 表示消息产生时间，不等同于飞控采样时间；
- 单调计时器只用于进程内部超时，不直接跨进程传输。

### 3.2 序列语义

- `sequence` 只在同一 `sourceId + sessionId + messageType` 中比较；
- 接收方发现序列跳跃时标记数据可能缺失；
- 遥测缺口可请求/等待下一次全量 Snapshot 恢复；
- 命令结果不得因序列跳跃静默丢弃，应记录并触发状态核对。

---

## 4. Telemetry DTO

方向：`ConsoleGCS → UnrealGCS`

### 4.1 TelemetryBatchDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `batchId` | string | 是 | 批次 UUID |
| `batchMode` | enum | 是 | `SNAPSHOT` 或 `DELTA` |
| `fleetRevision` | uint64 | 是 | 本批次应用后的机群版本 |
| `snapshotId` | string | 条件 | Snapshot 分片所属快照 UUID |
| `chunkIndex` | uint32 | 条件 | 从 0 开始的快照分片序号 |
| `chunkCount` | uint32 | 条件 | Snapshot 总分片数 |
| `items` | TelemetryDTO[] | 是 | 一批无人机数据 |

### 4.2 TelemetryDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `droneId` | string | 是 | 平台稳定 DroneID，不使用 SYSID 代替 |
| `sysId` | uint32 | 是 | 当前 MAVLink SYSID；未知时为 0 |
| `sampleTimestampUtcMs` | int64 | 是 | 当前遥测样本时间 |
| `telemetryRevision` | uint64 | 是 | 该 DroneID 的单调状态版本 |
| `changedFields` | uint64 bitmask | DELTA 必填 | 标记本条包含的有效字段；Snapshot 使用 `ALL` |
| `gps` | GPSDTO | 条件 | GPS字段有效时出现 |
| `altitude` | AltitudeDTO | 条件 | 高度字段有效时出现 |
| `battery` | BatteryDTO | 条件 | 电池字段有效时出现 |
| `mode` | ModeDTO | 条件 | 模式字段有效时出现 |
| `connection` | ConnectionDTO | 条件 | 在线/链路字段有效时出现 |
| `rssi` | RSSIDTO | 条件 | RSSI字段有效时出现 |
| `led` | LEDStateDTO | 条件 | LED字段有效时出现 |
| `mission` | MissionStateDTO | 条件 | 任务字段有效时出现 |

### 4.3 GPSDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `valid` | bool | 是 | GPS位置是否有效 |
| `latitudeDeg` | float64 | 条件 | WGS84 纬度，范围 `[-90, 90]` |
| `longitudeDeg` | float64 | 条件 | WGS84 经度，范围 `[-180, 180]` |
| `fixType` | uint8 | 否 | MAVLink FixType 的规范化数值 |
| `satellitesVisible` | uint8 | 否 | 可见卫星数 |

### 4.4 AltitudeDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `valid` | bool | 是 | 高度是否有效 |
| `relativeMeters` | float32 | 条件 | 相对起飞点高度 |
| `amslMeters` | float32 | 否 | 海拔高度 |

UE 当前 `FDroneData.Altitude` 应由 UE Adapter 明确选择 `relativeMeters`，不得依赖字段名称猜测。

### 4.5 BatteryDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `valid` | bool | 是 | 电池状态是否有效 |
| `remainingPercent` | float32 | 条件 | 范围 `[0, 100]` |
| `voltageVolts` | float32 | 否 | 为后续健康检查预留 |

### 4.6 ModeDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `modeCode` | int32 | 是 | 飞控原始/规范化模式编号 |
| `modeName` | string | 是 | `GUIDED`、`LOITER`、`RTL`、`LAND` 等 |
| `armed` | bool | 是 | 当前解锁状态 |

### 4.7 ConnectionDTO 与 RSSIDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `online` | bool | 是 | 是否在心跳有效期内 |
| `state` | enum | 是 | `OFFLINE`、`CONNECTING`、`ONLINE`、`STALE`、`LOST`、`RECOVERING` |
| `lastHeartbeatUtcMs` | int64 | 否 | 最后心跳时间 |
| `linkId` | string | 否 | 多链路身份，不能用 SYSID 代替 |
| `rssi.valid` | bool | 是 | RSSI是否有效 |
| `rssi.valueDbm` | int16 | 条件 | dBm；未知时不得伪造 0 |

### 4.8 LEDStateDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `state` | enum | 是 | `UNKNOWN`、`OFF`、`STATIC`、`PLAYING`、`FAULT` |
| `red` / `green` / `blue` | uint8 | 条件 | 颜色通道 `[0,255]` |
| `brightness` | uint8 | 否 | `[0,255]` |
| `showFrame` | uint64 | 否 | 已确认的表演帧 |
| `confirmed` | bool | 是 | 飞行器是否确认当前LED状态 |

`FAULT` 表示 LED 子系统故障，不代表整机健康状态；健康状态未来使用独立 DTO。

### 4.9 MissionStateDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `missionId` | string | 否 | 当前任务 UUID/业务ID |
| `state` | enum | 是 | `NONE`、`READY`、`RUNNING`、`PAUSED`、`COMPLETED`、`FAILED`、`ABORTED` |
| `currentItem` | uint32 | 否 | 当前任务项 |
| `progressPermille` | uint16 | 否 | `[0,1000]`，避免跨语言浮点比较 |
| `showTimeMs` | int64 | 否 | 当前表演时间码 |

---

## 5. Command DTO

方向：`UnrealGCS → ConsoleGCS`

### 5.1 CommandRequestDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `commandId` | string | 是 | 全局唯一 UUIDv7；也是幂等键 |
| `commandType` | enum | 是 | 见稳定编号表 |
| `targetDroneIds` | string[] | 是 | V1 显式目标集合，不允许空数组 |
| `parameters` | CommandParametersDTO | 是 | 命令对应的强类型参数 |
| `source` | CommandSourceDTO | 是 | 命令来源与审计信息 |
| `timestampUtcMs` | int64 | 是 | 客户端创建命令的 UTC 时间 |
| `expectedFleetRevision` | uint64 | 否 | 防止目标集合基于过期机群状态 |
| `safeTestMode` | bool | 是 | UE侧安全意图；Console仍必须执行自己的安全检查 |

### 5.2 CommandType 稳定编号

枚举编号一旦发布不得修改或复用。

| 编号 | 名称 | 参数 |
|---:|---|---|
| 1 | `ARM` | 无 |
| 2 | `DISARM` | 无 |
| 3 | `TAKEOFF` | `takeoffAltitudeMeters` |
| 4 | `LAND` | 无 |
| 5 | `RTL` | 无 |
| 6 | `SET_MODE` | `customMode`、可选 `modeName` |
| 7 | `HOVER_TEST` | `start`，未来可增加测试配置引用 |

未知枚举必须返回 `UNSUPPORTED_COMMAND_TYPE`，不得回退成其他飞行动作。

### 5.3 CommandParametersDTO

V1 使用一个带字段存在性的强类型对象：

| 字段 | 类型 | 适用命令 | 规则 |
|---|---:|---|---|
| `takeoffAltitudeMeters` | float32 | TAKEOFF | 必须 `> 0`，服务端再次校验上限 |
| `customMode` | int32 | SET_MODE | 必须属于 Console 当前明确支持的模式 |
| `modeName` | string | SET_MODE | 可选，仅供可读性和一致性校验 |
| `start` | bool | HOVER_TEST | `true` 开始，`false` 停止 |

不适用于当前命令的参数必须被拒绝或明确忽略并记录，不能改变命令语义。

### 5.4 CommandSourceDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `sourceType` | enum | 是 | `UE_UI`、`CONSOLE_KEYBOARD`、`AUTOMATION`、`SERVER` |
| `sourceId` | string | 是 | 客户端/模块实例ID |
| `operatorId` | string | 否 | 登录用户或操作员ID |
| `stationId` | string | 否 | 地面站编号 |

### 5.5 命令安全和幂等

- `commandId` 在全系统唯一，同一 ID 重发必须返回原命令状态，不得重复执行；
- ARM、TAKEOFF 等非幂等动作不得由客户端无条件自动重试；
- UE 的 `safeTestMode` 不能关闭 Console 的安全检查；
- Console 默认 `SAFE_TEST_MODE=true`；
- 真机命令仍需在线、目标、参数、权限、PreArm/APM等检查；
- 不允许通过协议参数绕过 APM 安全检查；
- V1 不定义强制解锁或强制空中 DISARM。

---

## 6. Result DTO

方向：`ConsoleGCS → UnrealGCS`

命令结果是增量状态流，不只有最终 bool。一个 CommandID 可以收到多次结果批次。

### 6.1 CommandResultBatchDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `commandId` | string | 是 | 对应请求 CommandID |
| `resultSequence` | uint32 | 是 | 该命令结果序列，从 1 递增 |
| `aggregateState` | enum | 是 | 聚合状态 |
| `final` | bool | 是 | 是否为最终结果 |
| `requestedCount` | uint32 | 是 | 请求目标数 |
| `acceptedCount` | uint32 | 是 | 已接受目标数 |
| `succeededCount` | uint32 | 是 | 成功目标数 |
| `failedCount` | uint32 | 是 | 失败或超时目标数 |
| `results` | DroneCommandResultDTO[] | 是 | 本批次发生变化的逐机结果 |

聚合状态：

```text
RECEIVED
VALIDATING
PENDING
EXECUTING
SUCCEEDED
PARTIAL_SUCCESS
FAILED
TIMED_OUT
CANCELLED
SIMULATED
```

### 6.2 DroneCommandResultDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `commandId` | string | 是 | 对应命令 |
| `droneId` | string | 是 | 逐机结果身份 |
| `ackState` | enum | 是 | 见下表 |
| `mavlinkCommand` | uint32 | 否 | 诊断字段，不作为 UE 业务逻辑依据 |
| `mavlinkAckResult` | uint32 | 否 | 原始ACK诊断值 |
| `attempts` | uint16 | 否 | 实际发送次数 |
| `error` | ErrorDTO | 否 | 拒绝/失败原因 |
| `timeout` | TimeoutDTO | 否 | 超时详情 |
| `updatedTimestampUtcMs` | int64 | 是 | 状态更新时间 |
| `final` | bool | 是 | 该 DroneID 是否已终结 |

ACK状态：

```text
RECEIVED
VALIDATED
QUEUED
SENT
IN_PROGRESS
ACCEPTED
REJECTED
FAILED
TIMED_OUT
CANCELLED
SIMULATED
```

### 6.3 ErrorDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `code` | string | 是 | 稳定错误代码 |
| `message` | string | 是 | 面向日志/操作员的说明 |
| `retryable` | bool | 是 | 仅表示技术上可重试，不授权自动重试飞行动作 |
| `details` | map<string,string> | 否 | 结构化诊断信息，禁止包含密钥 |

V1基础错误代码：

```text
UNKNOWN_DRONE_ID
INVALID_TARGET_SET
INVALID_PARAMETERS
UNSUPPORTED_COMMAND_TYPE
UNSUPPORTED_MODE
VEHICLE_OFFLINE
SAFE_TEST_MODE_BLOCKED
PREARM_CHECK_FAILED
COMMAND_REJECTED
COMMAND_SEND_FAILED
COMMAND_SUPERSEDED
ACK_TIMEOUT
INTERNAL_ERROR
```

### 6.4 TimeoutDTO

| 字段 | 类型 | 必填 | 说明 |
|---|---:|:---:|---|
| `stage` | enum | 是 | `QUEUE`、`SEND`、`ACK`、`MISSION` |
| `timeoutMs` | uint32 | 是 | 配置的超时 |
| `elapsedMs` | uint32 | 是 | 实际等待时长 |

---

## 7. 版本号与兼容性设计

### 7.1 三层版本

| 层级 | 示例 | 用途 |
|---|---|---|
| 协议主版本 | `protocolMajor=1` | 不兼容的语义、信封或基础编码变化 |
| 协议次版本 | `protocolMinor=0` | 向后兼容的可选能力增加 |
| Payload结构版本 | `schemaVersion=1` | 单个消息DTO的字段演进 |

### 7.2 兼容规则

1. 主版本不同：拒绝业务消息，报告版本不兼容。
2. 同主版本、发送方次版本更高：接收方忽略未知可选字段。
3. 新增字段必须是可选字段或有明确默认语义。
4. 删除必填字段、改变单位、改变枚举含义必须升级主版本。
5. 枚举数字、字段数字和错误代码发布后不得复用。
6. 未知必需能力必须明确拒绝，不能静默降级成其他飞行动作。
7. 单位固定写入字段名或协议规范，例如米、度、dBm、UTC毫秒。

### 7.3 未来能力协商

网络阶段可增加 `HELLO/CAPABILITIES`，协商：

- 支持的协议主/次版本；
- 支持的消息 SchemaVersion；
- 支持的 CommandType；
- 最大批次数和字节数；
- Snapshot/Delta、压缩和 DroneKey 字典能力；
- SafeTestMode 与服务器只读状态。

阶段4.1不实现该握手。

---

## 8. 千机/万机批量传输方案

### 8.1 遥测批次

推荐默认值，最终由能力协商和性能测试确定：

```text
TelemetryFlushIntervalMs = 50
MaxTelemetryBatchItems   = 512
MaxTelemetryBatchBytes   = 256 KiB（序列化前估算）
```

- 1000台全量遥测通常拆成约2个应用层批次；
- 10000台全量快照通常拆成约20个以上分片；
- 应用层批次不等于一个网络包，未来传输层负责分帧；
- 不得假设一个批次能放入单个 UDP Datagram。

### 8.2 Snapshot + Delta

1. 新会话、重连或序列缺口后发送全量 Snapshot；
2. 正常运行只发送带 `changedFields` 的 Delta；
3. 同一刷新窗口内同一 DroneID 多次更新合并为最新字段值；
4. `fleetRevision` 和逐机 `telemetryRevision` 防止旧数据覆盖新数据；
5. Snapshot 分片使用同一 `snapshotId`，全部到齐后原子应用；
6. Snapshot 超时或校验失败时丢弃不完整快照，继续使用上一稳定版本。

### 8.3 有界队列与背压

消息优先级：

```text
P0 命令最终结果 / 安全告警
P1 命令中间状态 / 连接状态
P2 任务与关键遥测
P3 高频位置、姿态和LED播放遥测
```

- P0/P1不得采用“最新值覆盖”静默丢弃；
- 高频遥测积压时按 DroneID 合并，只保留最新 Delta；
- 队列必须有上限并记录丢弃/合并统计；
- UE只按受控频率应用批次，Widget不逐包刷新；
- 命令结果与遥测使用独立逻辑队列，防止遥测洪峰阻塞ACK。

### 8.4 DroneID压缩映射

V1所有业务DTO必须支持完整 `droneId`。

未来可在同一 Session 内协商：

```text
DroneKeyDictionary
  uint32 DroneKey -> string DroneID
```

Snapshot携带完整 DroneID 和 DroneKey；后续高频 Delta 可只携带 DroneKey。字典版本不一致时必须回退到完整 DroneID 或请求新 Snapshot，不能猜测映射。

### 8.5 序列化和压缩建议

本阶段不选定库，但协议逻辑模型应支持：

- 开发/诊断：JSON，便于抓包和人工阅读；
- 生产批量：带稳定字段编号的二进制 Schema（优先评估 Protobuf）；
- 大型 Snapshot：可选 Zstandard 压缩；
- 小型实时 Delta 默认不压缩，避免额外延迟；
- CRC、认证和加密属于未来传输帧/安全层，不嵌入业务DTO。

正式实现前必须通过 UE5.5.4、VS2022、Windows 打包部署和1000/10000台数据压测后确定编码库。

---

## 9. 示例

以下仅用于说明逻辑字段，不代表已经选择JSON作为生产编码。

### 9.1 单机命令请求

```json
{
  "protocol": "DSP-GCS",
  "protocolMajor": 1,
  "protocolMinor": 0,
  "messageType": "COMMAND_REQUEST",
  "schemaVersion": 1,
  "sourceId": "UnrealGCS-Station01",
  "sessionId": "0198f000-0000-7000-8000-000000000001",
  "sequence": 42,
  "timestampUtcMs": 1785744000000,
  "correlationId": "0198f001-0000-7000-8000-000000000001",
  "payload": {
    "commandId": "0198f001-0000-7000-8000-000000000001",
    "commandType": "ARM",
    "targetDroneIds": ["Drone001"],
    "parameters": {},
    "source": {
      "sourceType": "UE_UI",
      "sourceId": "WBP_GCS_DroneDetail",
      "operatorId": "Operator01",
      "stationId": "Station01"
    },
    "timestampUtcMs": 1785744000000,
    "safeTestMode": true
  }
}
```

### 9.2 多机结果

```json
{
  "commandId": "0198f001-0000-7000-8000-000000000001",
  "resultSequence": 3,
  "aggregateState": "PARTIAL_SUCCESS",
  "final": true,
  "requestedCount": 3,
  "acceptedCount": 2,
  "succeededCount": 2,
  "failedCount": 1,
  "results": [
    { "commandId": "0198f001-0000-7000-8000-000000000001", "droneId": "Drone001", "ackState": "ACCEPTED", "final": true, "updatedTimestampUtcMs": 1785744000500 },
    { "commandId": "0198f001-0000-7000-8000-000000000001", "droneId": "Drone002", "ackState": "ACCEPTED", "final": true, "updatedTimestampUtcMs": 1785744000520 },
    { "commandId": "0198f001-0000-7000-8000-000000000001", "droneId": "Drone003", "ackState": "REJECTED", "error": { "code": "VEHICLE_OFFLINE", "message": "Drone heartbeat timed out", "retryable": false }, "final": true, "updatedTimestampUtcMs": 1785744000100 }
  ]
}
```

---

## 10. 两端映射责任

### 10.1 Console Adapter（未来）

- `DroneSession/FlightData` → `TelemetryDTO`；
- Console内部 `uint64 CommandID` 与协议 UUID CommandID 建立关联；
- `CommandRequestDTO` → Console `DroneCommandRequest`；
- `ManagedDroneCommand/CommandDispatchResult` → `CommandResultBatchDTO`；
- MAVLink原始枚举只进入可选诊断字段，不泄漏到 UE 业务逻辑。

### 10.2 UE Provider（未来）

- `TelemetryBatchDTO` → `FGCSFleetSnapshot` 或 `FGCSTelemetryBatch`；
- 单位、字段有效性和 ChangedFields 显式映射；
- `CommandResultBatchDTO` → UE 命令状态模型；
- Provider不直接操作Widget；
- `UGCSServerAdapterSubsystem` 不感知具体传输协议。

---

## 11. 阶段4.1验收结论

- Telemetry、Command、Result 三类DTO已定义；
- 数据模型与 UE/Console 内部模型分离；
- CommandID、逐机ACK、错误和超时语义已定义；
- 版本、序列、时间、幂等和兼容规则已定义；
- Snapshot、Delta、分片、合并、背压和千机批量方案已定义；
- 未实现或选择Socket；
- 未修改 UnrealGCS；
- 未修改 ConsoleGCS 控制逻辑；
- 下一阶段必须先确认本协议，再选择编码和传输实现。
