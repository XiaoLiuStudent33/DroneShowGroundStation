# 行为、生命周期与验证风险

Status: AUDIT / PROPOSED TESTS；基线 `a95309d8`。本轮无动态测试，以下“待验证”不是已复现故障。

## 1. 不可改变的合同

| 风险 | 必须保持 | 所需证据 |
|---|---|---|
| 目标错配 | Endpoint唯一身份、Drone000零基、confirmed mapping优先分支、选择与详情分离 | 相同SYSID不同endpoint；mapping/selection改变；离线重连 |
| 命令语义漂移 | MAV_CMD/参数/发送顺序/ACK/timeout/retry/priority原样 | 内存捕获完整datagram与顺序、时钟驱动测试 |
| UI替换丢gate | 暂停后方向、关机确认、空选择、AutoStart mutation gate | authored UI + 无Widget应用调用对照 |
| 伪造执行完成 | Sent、Accepted、遥测确认、工作流受理分开 | LED/授时无ACK、Hover受理、relay ACK未断电案例 |
| 上传损坏 | Home计数、segment compiler、Terminate commit gate、最终verification | 编译字节/hash、fake FTP/NAK/retry/cancel矩阵 |
| 重复上传 | exact Frozen Accepted、identity重校验、exactly-once Start、runtime binding | 重复ACK/重开View/stale/config revision/首快照丢失 |
| 状态显示误导 | Live/NPZ分开、LinkLost/Offline分开、未知值保留 | GPS无效/重复时间戳/暂停预览/断链恢复 |
| 部署生命周期 | Launcher等待Ready；完整控制模式前端退出不杀Console | 隔离Smoke、Shipping manifest/Installer验证 |

## 2. 线程/对象交界审计矩阵

| 交界 | 已见保护 | 待验证范围 |
|---|---|---|
| Console MAVLink → Session | 主循环Receive/Ingress/Tick串行 | 未来不能新增接收线程直接改DroneManager |
| Console DSP接收 → Command/Frozen/Upload | mutex队列、主线程ProcessPending | 无界pending/known ID长期增长；当前无新限流 |
| Hover线程 → Dispatcher/DataManager | mutex、atomic状态、析构join | 停止/重启交错、退出join等待；每活动Session线程规模 |
| Preparation → Upload | 请求/结果队列、不可变Artifact、cancel/join | Session/transport退出和permit释放顺序 |
| Console业务 → Renderer | 有界日志队列、快照、单writer | 日志入口提取不能改变丢弃/终帧 |
| UE UDP → Provider | handler mutex、receiver Stop/reset | 接收回调复制后Stop、弱UObject跨线程取值的生命周期假设 |
| Provider → ServerAdapter → Fleet | pending mutex、GameThread AsyncTask、Fleet apply线程检查 | Deinitialize后但GC前任务仍可到达；需shutdown epoch特征化 |
| Provider → GPS Capture | 捕获值拷贝、GameThread AsyncTask | 与Fleet flush先后及重连/退出旧观测 |
| Command结果 → UI | GameThread ApplyResultBatch、弱指针 | Broadcast可能重入：当前Pending指针在循环中继续使用；需用提交/解绑监听器复现评估 |
| Upload/Frozen收包 → Tick | enqueue/process、Ticker移除、handler解绑 | teardown与已入队数据、首次snapshot恢复 |
| Waypoint finalize → Workspace | cancel + batch.Wait + future.Wait | GameThread退出等待长任务；不可假定全异步无阻塞 |
| Row UObject → Widget | UPROPERTY强引用、唯一绑定、Shutdown解绑 | 多次Bind不同主Widget、旧控件残留callback |
| Visualization → Fleet | BeginPlay bind、EndPlay unbind | World切换/Preview退出恢复和raw actor缓存 |

未观察到生产网络线程直接修改Niagara或Widget的路径；Provider/Adapter UObject本身有被worker调用的入口，不能将“最终Fleet在GameThread”扩大为“所有UObject只在GameThread访问”。

## 3. 需单独记录、不在重构中顺修的候选问题

1. UE Command `PendingCommands` 没有自身deadline/断链清理策略。Console结果发送失败后仍移除active；丢失最终UDP结果可能使UE pending保留。记录/特征化，不添加自动命令重发。
2. UE `ApplyResultBatch` 在Broadcast前后持有map元素指针；允许监听器重入提交命令时需验证容器重分配。不是本轮已复现崩溃。
3. CommandReceiver/CommandManager known-ID集合无淘汰，Receiver pending无显式cap。改变去重窗口会改变行为，不能作为“删缓存”小改。
4. 高级二次确认当前绑定命令类型和5秒窗口，未冻结确认时目标集合；更改确认合同需用户单独授权，不把它悄悄加入UI解耦。
5. UIBridge单组 PendingMissionPlaybackCommandID 与 per-target 暂停集合，多次快速提交场景需保留/验证现状，不默认实现并行tracker。
6. `ToProductionFleetDrone`没有armed投影，速度/姿态也未完整传到UI。补齐意味着能力/数据合同变化，延期专项处理。
7. DefaultGame.ini GPS capture commissioning gates标记 HardwareValidated=False；不能把有软件测试解释为室外真机验证。
8. 模块World初始化通过0.15秒一次性Timer创建UI，回调持有WeakWorld；TimerHandle是局部变量，没有模块级显式取消句柄。正常World销毁与弱引用提供部分保护，但模块热卸载/重载仍需单独验证，不能泛称所有Timer都有对称ClearTimer。
9. BindMainWidget会重新Initialize Coordinator，而Coordinator Initialize先Shutdown；普通隐藏重开与重新绑定主Widget是不同生命周期。后续迁移必须分别冻结行为，不能依据“隐藏不取消任务”推断任意重Bind也无副作用。

## 4. 证据等级

- 本轮：代码/工程/配置/测试文本审计与文档一致性检查。
- 既有测试源码：表明有相应case，不等于本轮在HEAD重跑PASS。
- 文档记录的Console/UE历史PASS：Phase0需重新形成基线。
- 用户现场确认：Fleet V2、9100/9101三段上传、6/8关机情况；不升级为自动化或任意硬件保证。
- 真机与发布：本轮全部未运行。旧1.0.0不包含新UI；硬件E.2、Drone001/007、稳定重启身份继续延期。

## 5. 禁止入口

没有明确真机授权，不运行 `--mavftp-test`、`--mavftp-hardware-test`、`--allow-mission-write-probe`、任何真实上传/飞行动作/继电器动作/飞控参数写入。软件测试仅捕获内存或隔离loopback。本轮也不开始Phase0的构建/测试。
