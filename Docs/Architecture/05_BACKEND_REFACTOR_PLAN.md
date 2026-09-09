# 后端治理方案

Status: PROPOSED / NOT STARTED；基线 `a95309d8`。本轮不修 Bug、不改 protocol，不运行后端。

## 1. 保留的骨架

`Application composition → DroneManager → endpoint-bound DroneSession` 保持；短命令 Dispatcher 与长 Upload Job 仍独立。ConsoleUI 的可见 Dashboard、单 Renderer、1024日志队列、Linear输出均保留。Tools/Launcher 的完整控制后端退出规则不变。

## 2. 逐主要类处理

| 类/文件 | 当前职责 | 施工建议 | 不应做 |
|---|---|---|---|
| ConsoleGCSApplication | CLI/config/safety、组装、主循环、显式harness | 私有函数拆CLI解析/组装，保持对象声明与析构顺序 | 新增后台服务或改变10ms循环/每轮drain |
| AppConfig/SafetyConfig | 配置解析及启动安全全局 | 先记录加载后只读合同；未来可传const配置视图 | 本轮移除gate或热更新非atomic值 |
| DroneManager | Session unique_ptr所有权、Endpoint索引、稳定顺序 | 保持聚合根；必要时h前置声明降低include | 重新按SYSID索引、offline删除 |
| DroneSession | 生命周期、状态、命令路由、FTP/verification服务 | 提取纯命令参数检查/映射候选，owner留原处 | 第二Client、第二socket、共享所有飞机pending |
| DroneCommandRequest | Console协议无关领域请求 | 保持独立领域enum/参数 | 直接typedef为DSP DTO |
| DSPGCSProductionCommandReceiver | decode/queue、trust boundary验证、映射、结果DTO | 首先提取两分支重复参数转换；测试枚举覆盖 | 合并安全模式分支的门禁或去掉重复ID策略 |
| CommandManager | 多target聚合、内部/dispatch关联 | 最多拆输入键盘适配；结果映射小函数 | 因Sent名字改变terminal语义 |
| CommandDispatcher | 单机串行pending、优先级、ACK/模式/#187/烟雾复合步骤 | 保留状态机，先独立测量与packet trace，减少参数构造重复 | 统一所有动作成单ACK规则、修改队列取消策略 |
| MavlinkCommand | MAVLink编码及少量复合动作 | 可将重复编码参数局部集中；日志依赖晚做 | 改MAV_CMD、target component、顺序、confirmation字段 |
| MavlinkParser / FlightData / DataManager | 解析/状态/线程安全发布 | 保留解析和快照分工，写清字段有效性 | 把DataManager当全局Singleton或删工作副本 |
| FlightControl / HoverTest | 实际起飞工作流 / 悬停测量 | 只补状态与线程合同文档；线程迁移另案 | 将受理结果等同稳定悬停、自动降落 |
| MavlinkReadOnlyIngress / Discovery | 每endpoint解析与发现、路由 | 保持入口，不因名字ReadOnly删除出站相关路由 | 非heartbeat建Session、按sysid查找 |
| MavlinkUdpReceiver / UdpClient | 共享socket / endpoint适配 | 保持一份生产sender | per-session socket |
| ConsoleUploadJobAuthority | job/幂等/config binding/页快照 | 可提纯展示/DTO builder，保留状态写入 | 把Job放CommandManager |
| RouteUploadManager | 准入/准备/transport生命周期/permit | 保持唯一执行Owner | 第二调度器、固定数组替代动态N |
| MissionPreparationWorker / ArtifactConsumerV2 | 不可变文件准备、trust boundary重验证 | 保留read lease/hash/cancel | 与UE校验“去重”而删后端校验 |
| MissionFtpCompiler / MAVFTPMissionTransport | 唯一分段 / 段上传与验证 | 不为UI解耦改其算法 | 换协议、fallback、改变Home计数 |
| MavFtpClient / WritePermitPool | ACK关联/窗口/公平permit | 保持精确release与retry持有 | 共用短命令tracker |
| MissionUploadTransaction | 上传后的Count/Home/First/Last验证 | 名字有历史性，保留并注明真实职责 | 当作旧Mission上传死代码删除 |
| FrozenStore / Assembler / Receiver | Accepted authority / chunk事务 / queue边界 | 保持transaction identity | 与工作Assignment合并为可变共享对象 |

## 3. Command/ACK统一到什么程度

当前已有 CommandOutcome、DroneCommandTargetState、DSP AckState 和 UE FGCSProductionCommandResult，不缺“Tracker”名字。不同层分别负责物理命令、batch聚合、wire、呈现，保留映射是合理的。

可统一：层内纯映射、字段转换、语义命名和枚举覆盖测试。不可统一：短命令3000ms/1 retry、MAVFTP750/2500ms/3 retry、Frozen chunk重传、Upload command幂等、只读快照恢复、Hover遥测等待。改变这些时钟或终态条件即改变通信行为。

UE pending结果丢失无显式过期清理、Console两个known-ID集合无运行期回收、UI只跟踪当前一组播放请求，都是需特征化的风险；添加自动重发/淘汰/新结果本身不是机械重构，必须另提行为变更。

## 4. Shared治理

保留 `DroneIdentity`、DSP wire、Artifact canonical/digest、WPL110、Mission byte codec 的唯一实现。`RouteArtifactContract.h` 被V2复用，不是全体废弃V1。Windows read lease、Bcrypt在公共头里会传播平台依赖，可未来抽到同一共享平台实现，不复制到UE/Console各一份。

`UDPDSPGCSTransport`只是预留声明；当前Loopback是正式localhost UDP，不是Mock。若删除预留声明，先确认所有工程/文档/外部调用，保留TransportType::UDP值与拒绝语义，不能顺手实现远程UDP。

## 5. 验证/回滚

每个提取单独commit，先packet fixture基线再机械移动；Console Debug与Release编译，按触及范围运行Command/DSP/Pipeline、MissionUpload/UploadJob/MAVFTP专项。不得运行DroneGCS.exe的硬件harness。回滚使用该单commit的反向提交；不reset、不覆盖用户工作树。具体命令和完成标准见 [07](07_REFACTOR_ROADMAP.md)。
