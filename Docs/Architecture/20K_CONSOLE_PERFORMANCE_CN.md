# Console 20K 性能专项（2026-09-10）

基线 c5d3dfe。备用机仍暂停。仅修改 Console 接收调度、临时解码输出、Console 展示快照及可选诊断；不修改 Command ID、参数、ACK/Retry、3000ms 心跳超时、Endpoint/DroneID、Mapping 或 Upload wire。

## 发现与处理

- 用户现场在 UE 停止后仍大面积离线；原参数 4096 包/轮、4MiB 接收缓存。
- 隔离 DSP 端口的同机 Blender 20000 Endpoint 双向遥测样本复现掉线。只读接收样本无离线，不能将两者作为相同负载。
- 达到 drain 上限后仍 Sleep(10)，同时每轮重复执行全机群心跳检查。改为积压轮 Sleep(0)，每轮仍执行全部业务服务；TickAll 内保留原心跳检查。
- 逐字节清空 mavlink_message_t 改为每个 datagram 初始化一次，成功解码由 MAVLink memcpy 填充完整输出，Endpoint 分帧状态仍独立。
- Console renderer 每次日志唤醒都在日志共用锁内复制完整 Fleet。离线/重连日志突发时该路径放大生产者等待。改成每次 UpdateFleet 在锁外复制一次不可变共享快照，renderer 只取得引用；单物理写线程、日志文字、队列容量和关键日志策略保持。
- 调整 drain/缓存单独不足以解决问题；保留失败日志，不把短测暂时全在线当作完成。

## 诊断

启动进程前设置 DRONEGCS_PERF=1，默认关闭。每秒经原异步日志通道输出 [PERF]；window_ms 是实际窗口长度，packets/bytes/heartbeats 为窗口计数，drain_limits 不是丢包计数。rx_ms、tick_ms、work_ms、snapshot_ms 为分段累计耗时；max_loop_ms 不含末尾 Sleep；max_rx_gap_ms 含两轮接收之间的业务及调度间隔。online 为最近一次 Console Fleet 快照，约每秒刷新，不代表逐瞬间采样。

诊断记录不证明 UDP 内核丢包；定位发送端停顿仍需发送端计时或抓包。

## 验证

原始配置和日志位于 BuildLogs/Scale20K。测试使用本机已运行 Blender 模拟器，MAVLink 接收 192.168.1.254:10001，DSP 使用隔离 19992/19993；无 UE 指令发送端，不执行飞行或上传命令。双向组保留 GCS heartbeat / REQUEST_DATA_STREAM 自动行为。

尚不能宣称任意主机负载下永不掉线、真实硬件规模通过，或 UE 重新打开后的端到端验收通过。


180 秒双向模拟遥测采样：172 个统计窗口；首次 20000/20000 后的 171 个窗口最小在线数 20000；OFFLINE 日志 0；最大单轮耗时 227.518ms。接收参数 16MiB / 16384，超时仍为 3000ms。启动发现期间日志有 184 条普通记录被原有有界队列丢弃（critical drops=0），稳定阶段未新增 drop 警告。因此不把日志计数声称为无损逐事件审计。该采样禁用 Upload，DSP 使用隔离端口，无 UE 消费者；并非真机、上传并发或 UI 操作验收。此前失败样本均保留。

生产 gcs-config.ini 与 gcs-config-upload.ini 同步采用验证参数；代码 fallback 不变。默认不输出性能日志。Release/Debug 构建以及两种配置的完整 Console 回归均通过，包含新增分帧/CRC 和 Fleet 快照测试。最终可执行文件补充 max_rx_gap_ms 字段后另作短测；该字段不改变调度。

正式 ConsoleGCS/x64/Release/DroneGCS.exe 最终 60 秒复测：57 个窗口，预热后最小在线 20000，OFFLINE 日志 0；最终结果见 BuildLogs/Scale20K/results.json。所有本轮临时后端进程结束并释放端口，未 commit/push。


### 现场复测反馈：稳定性验收未通过

用户使用优化后正式 Release 与 16MiB/16384 配置，仍偶发数百至上千架 Offline，数秒后恢复。之前 180/60 秒样本通过不等于现场持续稳定；当前不能声明 20K 无掉线已完成。本次确认运行路径及配置已生效，尚无该次间歇故障的持久化 PERF 记录。新增 Tools/Diagnostics/Start-Console20KDiagnostics.cmd（调用同目录 PowerShell 脚本），仅为现有配置开启 DRONEGCS_PERF 和后台日志落盘，要求保留 UE/模拟器负载，采集故障对应的分段耗时和接收窗口。未修改 3000ms 超时、协议或业务参数。


### 2026-09-10 第二轮修复：协作收包与命令分批

现场诊断 `BuildLogs/Scale20K/UserRetest-20260910-130648-243/backend.log`：13:08:34 online=14094，loop=1830.83ms、Tick=1223.62ms；13:08:38 loop=2093.42ms。此前短测未覆盖这一负载。UE 一次选中 18742 目标时，原命令单包编码超过 UDP 容量，日志记录 command send failed；这是发送边界问题，不是飞控 ACK 拒绝。

- DroneManager::TickAll 每 128 Session 穿插一次最多 2048 包的 ingress（仍受原配置上限约束），同一线程、同一共享 Socket 和 Session owner。每轮固定扫描开始时的 Session 数；Ingress 新发现对象下一轮进入扫描，避免容器扩容使迭代器失效。新接收数据仍完整进入原 Parser/Session，不合并或丢弃 ACK/心跳。没有新增线程、队列 Authority 或修改 3000ms 超时。
- PERF 的 tick_ms 扣除穿插收包耗时；新增 session_send_ms/status_ms/ftp_ms/dispatch_ms/stream_ms。Receive 仅复制 recvfrom 实际写入区间，移除每包对 65507 字节临时数组的无用清零。
- Shared/DSPGCSProtocol/DSPGCSCommandBatching.h 使用已有 CommandRequestDTO，按实际编码字节数（60000 上限）和每批最多 256 目标切分；原 DSP/MAVLink wire 与命令类型 ID 不变。小请求保持原 ID，大请求使用唯一 child correlation ID（parent/part-N），复用后端既有去重规则。
- UE Provider 在非 Widget 生命周期中冻结各批目标，每个 ticker 回调最多发送一批（20ms 最小调度间隔，实际受帧率影响）。每批仅尝试本地发送一次，局部失败产生 LOCAL_SEND_FAILED 最终结果，不重发已经发出的批次。统一向 UI 报告 parent operation ID。大批量提交返回表示已入队，不代表已经全部发送或物理完成；不是同时到达全部飞机的保证。
- 新测试证明旧 ApplyResultBatch 收到非 final 进度就移除目标会丢失后续 final 所有权；现在仅 final 消耗目标，广播前完成状态变更，不在广播后继续解引用可能重入失效的 Map 指针。没有新增 ACK 重试或“收到 ACK 即物理完成”的判断。UDP 完全丢失结果仍未增加可靠重传机制；仍属限制。
- Console 测试覆盖 20K 目标编码/解码、顺序及唯一性、超长目标整单拒绝、单目标 ID 保持，以及 Session 扫描期间新增 Endpoint。UE 新增 DroneGCS.Command.BatchLifecycle，覆盖 progress/final、parent ID 和无 Channel 的局部失败/不重试。

构建/测试：Console Release、Debug 和两种完整回归通过；UE Editor Development 通过；UE 常规 248/248（226 无 warning + 22 带既有 warning），Synthetic 1/1，合计 249；Launcher Smoke 32/32。首次新增测试 Outer 错误已修正并全量重跑，失败日志保留。日志为 BuildLogs/Scale20K/*cooperative* 与 *batching*。

两万架现场持续复测尚待完成：首次尝试被 Blender Python PID 29496 占用 192.168.1.254:10001 阻挡，未停止用户进程。不据上述自动化测试声称现场无掉线。无真机命令、Mission 上传、Content 修改或 commit/push。


端口由用户释放后，完成 180 秒同机 Blender 20K 采样（cooperative-live.log / cooperative-results.json）：162 统计窗口，首次 20K 全在线后的161窗口最小在线20000；无 OFFLINE 记录。最长收包间隔382.791ms。仍观察到一次完整主循环2978.94ms，但该轮穿插接收140000包/60000心跳，所以不能再以整轮耗时等同不收包时间。那一轮 Session heartbeat send 为1021.36ms，是主要 Session 开销。保留普通日志启动突发丢弃15216、critical drops=0的证据，不把日志当作无损审计。启用了原配置 Upload service（无 Job），隐藏原生 Console dashboard，DSP隔离端口，无UE命令或危险操作；尚不能据此确认UE可见遥测刷新延迟/所有操作场景通过。

采样之后仅补充命令 receiver 在子批提交/结果发布之间调用同一 ingress，防止批量提交重新阻塞收包，并添加20K未知目标的完整backend结果计数测试（不发送任何MAVLink命令）。最终Console Release/Debug重新构建及回归。UE/Launcher未因该Console局部补充重复运行，之前验证覆盖的UE代码未再修改。现场请用诊断脚本启动新Release，再启动UE复测选择/批量操作；仍需实测确认模拟器支持对应命令及ACK，软件入队/发包成功不等同物理完成。


### 第三轮：真实遥测发布调度与日志开销（2026-09-10）

现场 AUTO 复测中，13:46:45~53 Console online=20000，但整轮最长3044.24ms，UE 2000ms telemetry timeout 触发 Disconnected。必须维护真实快照发布机会，不能提高超时或发送假保活。

- publishDue 在 Session 每128架、命令子批间和主循环末尾执行；仍在唯一业务线程发布原完整 Snapshot。完成后再等待配置间隔（250ms），不追赶积压发布任务。PERF 将穿插发布从 Tick/Work 中扣除，记录成功 publishes、publish_failures 与成功发布间隔 max_publish_gap_ms。
- Snapshot sizing 直接使用原 Telemetry item writer，去掉临时单-item batch、DTO 拷贝和重复 envelope 编码；原 wire、分片边界容量与 schema 不变。
- Console renderer 使用真正 deadline 等待，日志到来不能绕过刷新限频；默认200ms（最多5Hz，慢渲染不会追赶）。写盘和flush集中在该轮批处理中，仍在单独renderer线程。时间戳格式化移到入队锁外。
- 单条文本最多4096字节（UTF-8边界截断并显式标记），category最多64字节；1024队列+256历史的条数上限保留。满队列的新Info直接计数丢弃，避免普通日志做线性搜索；Error/Terminal仍优先替换Info，全部关键日志拥塞时仍有critical-drop计数，不声称无损审计。
- 文件按32MiB轮转，保留当前文件及3份归档（同一路径的.log.1~3）；仅处理明确配置的日志文件。原有不同诊断运行目录不删除。文件轮转错误停止该文件输出，不改业务状态。
- Session Accepted/Sent结果仍进入 completedCommandResults，仅省略重复的逐机成功日志，成功沿用DSP批次汇总；其余终态日志保留DroneID与原因并提升Error优先级。日志不是Command结果Authority。
- 新测试验证高频日志不绕过200ms期限、超长记录截断、32MiB轮转与Terminal保留。Release/Debug Console回归及Launcher Smoke用于确认；UE源码与Content本轮未修改，未延长2s/3s超时或执行危险命令。


本轮验证记录：Console Release/Debug构建及完整回归通过（build/tests-publish-log-*），Launcher32/32。三分钟同机20K采样 publish-log-live.log：预热后166窗口最小在线20000，publish_failures=0，成功发布最大间隔499.492ms，收包最大间隔229.952ms。启用原生Console dashboard和Upload service但无Job，DSP隔离且不发送命令。首版Python观察器错误按小端解码，而共享Writer为大端，其59161个decode error属于观察器错误，不是可用wire验收证据；原失败结果保留，修正后另行采样publish-log-live-v2.log/publish-log-wire-results-v2.json。采样不等同真实UE全选AUTO/物理动作验收。

修正观察器后的最终90秒wire采样：28318个datagram、0解码错误、289组分片齐全快照（其中285组含20000架），20K完整快照最大到达间隔610.493ms，超过2秒间隔0次。对应publish-log-wire-results-v2.json。该采样使用最终Release（含成功日志抑制），测试进程已退出、端口释放；仍未执行真实UE全选AUTO或危险物理命令。
