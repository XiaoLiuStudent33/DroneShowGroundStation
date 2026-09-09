# Unreal UI 解耦施工设计

Status: PROPOSED，未修改反射名/资源/代码。建议轻量Presenter，理由见 [04](04_TARGET_ARCHITECTURE.md)。

## 1. 不可直接删除旧 UI 的原因

`UGCSUIBridgeSubsystem`不只是Widget适配：命令选择、暂停资格、二次确认、结果聚合、授时输入与工作流模式还在这里。`UGCSRouteWorkflowCoordinator`由Bridge创建并持有，同时拥有确认NPZ映射和AutoStart。若直接删除Bridge或把其生命周期改成Widget生命周期，会改变现有业务。

仅删除WBP也会使按控件名绑定的入口失效。新UI最少应依赖协议无关Fleet/选择、Session健康、Command操作、Telemetry读取、RouteWorkflow与Preview API；这些能力可由现有Subsystem提供，不要求新建同名五个类。

具体证据：Bridge::BindMainWidget在缺少DroneDetail时直接返回，随后才创建/Initialize Coordinator；RouteGpsCapture::Initialize还显式依赖UIBridge并订阅选择事件。因此稳定API须让这些应用对象无需特定Widget完成初始化，选择迁移时同步调整Capture订阅，但不能短期双写两个集合。

## 2. 逐类与UE类型边界

| 类型 | 当前入口 | 目标/处理 |
|---|---|---|
| UUserWidget | WBP_Main_V2/FleetPanel_V2/Row/Detail/Playback/Route等 | 保留现有asset名及反射契约；旧Presenter先转发新应用入口 |
| UObject | Coordinator、FilterController、RowBinding、UploadListItem | 事务/模型寿命与Widget分离；行对象只为虚拟化可见数据 |
| AActor | DroneFleetVisualizationActor | 维持单Actor；Live与Preview模式/overlay/选择mask仍集中提交 |
| AGameMode/APawn | GCSGameMode、GCSInvisiblePawn | 保留World启动与输入，不搬入网络业务 |
| GameInstanceSubsystem | Fleet/UIBridge/Providers/Route等 | 长生命周期Owner；不要复制为每页状态 |
| ActorComponent | 当前无需要引入的每机业务组件 | 禁止为了ViewModel增每机Component |
| BlueprintFunctionLibrary | 一方业务清单未发现需要替代Owner的此类 | 无状态helper不持有Command/Selection状态 |
| Module | FDroneShowPlatform1Module | World/UI启动接线属于bootstrap；新UI接线可改这里的适配，不能改变网络启动条件 |

## 3. 拆分顺序

1. 特征化旧界面行为：勾选/详情、关闭重开、命令暂停、多次确认、授时/Playback、AutoStart/分页恢复、confirmed映射覆盖选择。
2. 将Coordinator.inl改为独立.cpp的候选先做编译验证；它当前依赖Bridge翻译单元中的helper，不能只改扩展名。盘点helper归属并保持测试include可编译，不改UCLASS。
3. 从Bridge抽取协议无关的操作状态与策略，旧Handler只获取输入后转发；先不移动 `SelectedCommandDroneIDs` 存储，稳定API委托它，避免短期双Authority。
4. 把Route事务与Widget读写分开，已确认映射/AutoStart保留唯一Owner；允许无Widget启动应用状态，关闭View只解绑显示。
5. 对Provider增加只读高层投影，Filter不再include完整TelemetryProvider；所有unknown/stale/source语义保持，不趁机补armed/姿态/速度。
6. Presenter提取按页进行：Fleet→Command/Detail→Playback/授时→Route→TelemetryFilter→地图与工具栏。每页分别测试和提交，保留原Widget名。
7. 新UI未来只需实现Presenter输入/输出合同。替换asset是另一轮明确授权工作，本治理不删旧Content。

## 4. 生命周期合同

- Bind可重复；绑定前解绑旧对象，行回收时移除委托并清除引用。当前 `UGCSFleetRowCheckBinding` 的 UPROPERTY TObjectPtr 和 Shutdown 模式应保留。
- View隐藏保留选择、滚动、草稿、Applied并发、Job和confirmed映射；销毁View不应取消物理操作。停止应用才释放长生命周期状态。
- InitializeDependency与Deinitialize顺序需建立矩阵；先停止接收、等待worker、再释放Owner，GameThread排队回调另需epoch/取消状态特征化。
- `TWeakObjectPtr`防止GC后解引用，但不能证明Deinitialize后不会执行排队回调。未来增加生命周期防护若改变迟到结果处置，应单独列行为风险。
- Presenter无socket/thread，不持久保存 raw `PendingCommands.Find` 指针跨委托广播；重入专项需覆盖事件回调期间提交新命令/解绑。
- World actor查找集中到现有渲染适配入口，允许有生命周期失效的弱缓存；不得持久保存跨World裸Actor。

## 5. 头文件/模块策略

先在一个Runtime模块内建立可验证的include边界；不为目录整齐立即拆UE模块。新应用Public类型不得include DSPGCSProtocol/IDSPGCSTransport/MAVLink，Presenter不得调用SendFrame。底层Provider public头的wire细节逐步移到private实现，保留所有UFUNCTION/UPROPERTY名称和类型兼容；反射类型移动跨模块需要独立迁移评估，本计划默认不做。

Build.cs将Sockets/Networking等从Public改Private之前，先找Public头实际类型需求；不能仅按名字删UMG/Niagara。UE MAVLink include path目前未发现一方生产使用，应归“需确认”，Phase0 clean build/UHT/asset依赖核验后才提删除。

## 6. 最小替换验收

离线测试创建应用Owner但不加载旧WBP，执行选择、读取状态、通过捕获transport提交原逻辑命令、注入原编码结果；测试新Presenter没有DSP依赖。旧authored UI回归继续覆盖真实ListView可见行、无反复重建、Checkbox/详情分离、独立Toolbar、NPZ只手动播放、Route Start exactly-once/首快照恢复。

UI规模不通过每机ViewModel Actor/Timer实现；保持稳定ListItem和虚拟化ListView。完整UI替换前，这些验收是前置条件；Console文件夹改名、Shared库化、全面MVVM是可延后项。
