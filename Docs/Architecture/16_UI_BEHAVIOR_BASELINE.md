# Phase 0：UE UI 工作流与依赖

基线：`a95309d8b37ecb00c779d5c4264aacb86294d8ec`，2026-09-09。Phase 0仅记录当前行为；源码、资产与生产配置不变。STATIC_BASELINE与自动化/现场证据分别报告。

## 1. 当前依赖

| 对象 | 当前职责与直接依赖 |
|---|---|
| UIBridge | Widget查找/bind、SelectedCommandDroneIDs、命令门禁/确认/暂停、上传面板、相机、定时器；直接访问Fleet、CommandProvider、TelemetryProvider、Filter、Coordinator |
| Coordinator | 私有UObject；由Bridge在DroneDetail绑定后创建；依赖Route/Artifact/Assignment/FrozenSync/UploadSubsystem、Bridge选择与UI刷新；仍以inl包含于Bridge编译单元 |
| Fleet | GameThread机群数据、详情选择、事件；提供UI与Visualization消费状态 |
| TelemetryProvider | DSP解析/重组、MDHK缓存、连接健康；依赖ServerAdapter与GPS Capture发布 |
| CommandProvider | UE命令参数到DSP、pending/results、GameThread事件；通过TelemetryProvider同一transport发送 |
| Filter | 查询TelemetryProvider/MDHK等状态；只定义可见集合，不拥有命令选择 |
| RowBinding / FleetRowData | ListView虚拟化行绑定、复选框与详情点击隔离；保持行对象和委托生命周期 |
| Visualization Actor | 共享Tick、Niagara位置/RGB、真GPS双样本插值；与Route Preview源分离 |
| RouteGpsCapture | Fleet/Bridge/Provider事件与采样门禁；不是新的飞控Authority |

以上仍是当前耦合结构，没有引入Presenter/Service/ViewModel，没有拆Bridge/Coordinator。

## 2. 必须保持的可观察工作流

| 流程 | 触发 → 当前结果 | 本轮证据与边界 |
|---|---|---|
| Vehicle Selection | ID列起详情区域点击 → Fleet详情对象变化；不自动打开DroneDetail | Selection/FleetList源码自动化；实际命中范围仍人工 |
| Command Selection | 复选框 → SelectedCommandDroneIDs；详情选择不变 | Selection自动化；筛选结果不可替代选中目标 |
| Command Submit | 按钮 → 当前选择快照→门禁→Provider→DSP；result回GameThread更新UI | Command/Selection/源码捕获；硬件动作见Command基线 |
| Toolbar | BTN_Fleet只控制CHILD_FleetPanel；BTN_Control只控制CHILD_DroneDetail；BTN_Menu不控制两者 | IndependentToolbarToggles自动化；两面板可独立同时开关 |
| List refresh | 同数据行复用，刷新不反复重建ListView；recycle移除旧绑定 | FleetListContract源码回归；真实滚动/触控人工 |
| Upload | confirmed目标捕获→Freeze→Accepted→重校验→Start→分页镜像 | Coordinator/UploadJob自动化；隐藏与重绑不能合并为一种事件 |
| Mission | 暂停成功才允许前进/后退；结果更新目标暂停资格 | Command捕获与Selection；不会驱动NPZ |
| Time / Dance | 倒计时0、执行舞步不自动播放NPZ | ScheduledShowPreview源码回归 |
| Playback | 仅底部显式播放NPZ，暂停/帧步进/退出预览沿用现有控件 | 源码与现有Route测试；时序观感人工 |
| Telemetry | Provider→Adapter→GameThread Fleet→UI/Actor | TelemetryLink/Chunk/GPS与Visualization自动化；单个字段覆盖见13 |
| Connection | Console通道LinkLost与每机Offline分别显示/门禁 | TelemetryLink自动化；不偷偷清空/替代命令选择 |
| Advanced confirm | 5秒按命令类型确认；高级加锁/拉烟/待机/重启/关机有各自入口 | 现有源码基线；不是冻结第一次目标的确认方案 |

## 3. 生命周期与未覆盖风险

主模块OnPostWorldInitialization延迟0.15s创建WBP_Main_V2并BindMainWidget；弱World检查存在，但本地timer handle不是统一取消Authority。BindMainWidget找不到DroneDetail时返回；Coordinator创建仍依赖这个路径。隐藏面板、重复Bind、GameInstance shutdown不能当作同一动作。

行对象由UPROPERTY引用，弱Bridge和AddUniqueDynamic/Shutdown共同管理委托。已有source-contract测试能捕获这些调用存在，不能证明所有真实Blueprint rebinding、热重载、输入穿透和关闭重开时序。Provider异步回调的deinitialize epoch、result broadcast重入、final result丢失等未新增大规模测试接入，保留为Phase1前风险，不能宣称被全量回归穷尽证明。

生产API、UFUNCTION/UPROPERTY声明、Content资源路径均不变；[BASELINE](BASELINE.md)的hash与资源/反射清单用于后续对照。原新Fleet现场PASS只是用户历史证据；本轮NullRHI自动化不是重新现场验收。

本轮实际测试状态见 [11](11_TEST_BASELINE.md)。FIELD_BASELINE_REQUIRED/人工：真机GPS/颜色观感，操作者点击命中与滚动，多面板开关，断电板，Shipping/Launcher真实UE联合包和Installer（旧1.0.0不含9月7～9日改动）。
