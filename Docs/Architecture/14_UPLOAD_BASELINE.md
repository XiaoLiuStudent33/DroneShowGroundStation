# Phase 0：Upload 行为

基线：`a95309d8b37ecb00c779d5c4264aacb86294d8ec`，2026-09-09。Phase 0仅记录当前行为；源码、资产与生产配置不变。STATIC_BASELINE与自动化/现场证据分别报告。

## 1. 调用链与Authority

`UI/Selection → Coordinator::CaptureAutoStartSnapshot → Confirmed Mapping/默认映射 → Working Assignment → Freeze(AllowDegraded) → FrozenSync → Console Receiver/Assembler/Store Accepted → Coordinator重校验 → UploadSubsystem::Start → DSPUploadJobReceiver → ConsoleUploadJobAuthority → RouteUploadManager → MissionPreparationWorker/ConsoleArtifactConsumerV2 → MAVFTPMissionTransport/MissionFtpCompiler → Session MavFtpClient → Endpoint`。

这是同一条生产链，不创建第二RouteAssignment、segment planner、network Authority。Shared提供合同，不拥有运行时会话。`MissionUploadTransaction`当前是MAVFTP完成后的**只读Mission verification**，不是被删除的旧Mission Upload transport。

## 2. 目标、冻结与AutoStart（STATIC_BASELINE）

| 检查点 | 当前行为 |
|---|---|
| 未确认Mapping | 从SelectedCommandDroneIDs捕获目标；默认RouteSlot=NumericDroneID，零基、范围校验 |
| Confirmed NPZ Mapping存在 | 校验dataset/domain/revision；使用Entries的DroneID和slot，并替换目标快照；UI后来改勾选不会覆盖confirmed目标 |
| Freeze | 捕获当前dataset/artifact、Fleet/Endpoint、assignment/config revision等身份，形成显式Working Assignment，AllowDegraded冻结 |
| ConsoleAccepted | Frozen Assignment带authority session/revision/digest，同一绑定被Console原子接受后才可继续 |
| AutoStart | Accepted后再次验证捕获身份与当前Authority一致；设置bJobStartAttempted，再调用Start，保持一次启动尝试边界 |
| 幂等 | 同一DSP命令重发保留请求身份，Console去重；不把网络retry变成新job |
| Start结果/首个snapshot丢失 | 查询/recovery已有job；不能通过重复Start恢复画面 |
| 页面/生命周期 | route list稳定key为UploadJobID+RouteSlot；snapshotToken只是分页轮次，不当行身份；部分页不发布；重绑、隐藏和Shutdown区别见UI基线 |

“一次启动”是现有捕获/identity/gate/去重作用域内保证，不是对任意进程崩溃和身份重建的全局exactly-once承诺。不能在拆Coordinator时重新从UI动态推导正在上传的目标。

## 3. Preparation / wire /并发

Artifact v2：`.waypoints + manifest + READY`、Current指针、schema/hash、允许根目录、read lease、TOCTOU二次验证保持；失败不换另一来源继续。生成使用Local ENU+Origin，mission相对高度来自Local Z，不能加AMSL。

唯一segment planner：`segment_items=4000`包含Reserved Home。9100 Artifact rows→9101 wire mission items→4000+4000+1101；第一段CLEAR(options0)，后段NO_CLEAR(options1)，前段terminate ACK后才进入下一段。固定wire格式和Home语义不改变。

并发1..50（默认50），global Write outstanding cap1..800（默认800），per-client window默认16，有效上限受 `min(global, concurrency*16)`约束。runtime query/apply有revision/digest绑定，不能以旧配置快照授权新job。fair permit由现有Manager共享；同一Write重试持有原permit，精确ACK/NAK/取消/Shutdown释放，不能重试重复占额。

## 4. Timeout / Retry分层

| 层 | 当前常量/规则 |
|---|---|
| FrozenSync | MaxAttempts=3；ACK0.75s；InitialRetryDelay0.25s；仅可重试错误进入现有策略 |
| UE Upload command | MaxCommandAttempts=3（含初次）；retry0.5s；原frame/identity重发 |
| UE Snapshot | MaxSnapshotRequestAttempts=3；retry0.5s；live poll0.25s，tick0.05s；page最大20000 |
| MAVFTP client默认 | request750ms；terminate2500ms；requestRetryCount=3；配置可改变有效值，必须保留配置校验 |
| post-MAVFTP verification | response750ms；maxRetries3；绑定原Endpoint与mission identity |

不得将层间的attempt和retry混为同一预算。保持超时触发状态、最终失败诊断及重试权限，不在Phase0调参。

## 5. 完成/失败边界

All Write ACKed只是字节传输检查点。所有段结束后还要Mission count、Home、首/末项验证；只有现有verification成功才能进入对应Succeeded/Mission Ready状态。NAK、超时、源文件/绑定变化、verify mismatch分别保留诊断；失败**绝不**回退标准Mission Upload。RetryFailed/Cancel调用现有job接口，不重新规划目标。

TESTED_BASELINE：本轮Console full里的consumer/compiler/writer/mission-upload，独立runtime-config/concurrency，以及UE FrozenSync/UploadJob/Coordinator/Artifact测试，逐项结果见 [11](11_TEST_BASELINE.md)。源hash与wire合同见 [BASELINE](BASELINE.md)。FIELD_BASELINE_REQUIRED：9100三段真机结果只有用户此前证据，本轮未复跑；真实Mission Ready、飞行轨迹、E.2规模不由本地fake证明。

源码：[Coordinator](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSRouteWorkflowCoordinator.inl)、[Upload client](../../UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSUploadJobSubsystem.cpp)、[Authority](../../ConsoleGCS/ConsoleUploadJobAuthority.cpp)、[Manager](../../ConsoleGCS/RouteUploadManager.cpp)、[Compiler](../../ConsoleGCS/MissionFtpCompiler.cpp)、[verification](../../ConsoleGCS/MissionUploadTransaction.cpp)、[defaults](../../ConsoleGCS/AppConfig.h)。
