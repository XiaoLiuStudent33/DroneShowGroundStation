# Phase 0：测试盘点与实际执行

HEAD `a95309d8`；2026-09-09。以下来自本轮执行，历史文档PASS不计数。原始日志根 `BuildLogs/ArchitecturePhase0-20260909`，UE报告根 `UnrealGCS/Saved/Automation/ArchitecturePhase0-20260909*`；完整逐项结果/诊断已纳入 [PHASE0_EVIDENCE.json](PHASE0_EVIDENCE.json)。

## 1. 结果与计数口径

- Console：6次进程运行全部通过（full、disarm、runtime-config、concurrency、artifact10K、mission benchmark）；full内含16个顶层suite，不把重复disarm另算新的覆盖。
- Launcher Smoke：1次通过，32条PASS检查。
- UE最终全量：242项，242成功（其中20项带warning）、0失败、0未运行。
- UE单独Synthetic10Kx16：1项成功。因此最终UE已执行不同测试243项，243通过。
- UE首轮：242项中239成功、3失败，exit仍为0；保留首轮记录，不与复跑合并为新增测试数。
- 真实1000x9100 NPZ benchmark缺少指定fixture/env，未执行；跨进程手工Acceptance、真机/人工项目不计通过。

首次按旧roadmap将ProductionTelemetry/Command全局Enabled=False，导致AtomicThousandDroneSnapshot、ProductionTelemetryPreservation、DetailAndCommandStateSeparation失败（样本/队列为0、provider disabled）。复跑保留SafeTestMode=True，启用Provider、无运行生产Console，三个失败全部消失；推断为测试运行配置不兼容，而非修复了生产代码。未改任何源码。运行时要检查report.failed/notRun和日志，不能只看exit=0。

复跑命令尝试覆盖19261/19262端口，但实际部分测试日志恢复到127.0.0.1:12001/12002；故不宣称全程专用端口生效。Frozen E2E自建回环随机端口与ConsoleGCSTests子进程。运行前确认无DroneGCS/Launcher进程，结束无遗留；生产MAVLink程序未被普通配置启动。下次复跑也必须核查实际有效端口及占用。Launcher单独用fixture绑定127.0.0.1:10091，不发飞控数据报/命令；并非运行真实部署配置。

## 2. 实际命令与运行时间

Console在ConsoleGCS目录运行新编译的 `./x64/Release/ConsoleGCSTests.exe`，full无参数，其余同下表flag。完整suite涵盖配置、Endpoint/Parser/telemetry、command packet/manager/session、DSP command、frozen协议/receiver、artifact contract/consumer、mission codec/compiler、upload job、MAVFTP probe/writer/harness安全、mission upload/preparation、ID与render。没有虚构--command-tests。runtime/concurrency/scale/benchmark使用main中真实独立入口。

| 进程运行 | Result | 墙钟秒 | 日志/失败原因 |
|---|---|---:|---|
| full | PASS（exit0） | 23.269 | test-full.log；无失败 |
| disarm-tests | PASS（exit0） | 0.028 | test-disarm-tests.log；无失败 |
| runtime-config-tests | PASS（exit0） | 0.026 | test-runtime-config-tests.log；无失败 |
| mavftp-concurrency-tests | PASS（exit0） | 0.260 | test-mavftp-concurrency-tests.log；无失败 |
| artifact-v2-scale-10k | PASS（exit0） | 14.652 | test-artifact-v2-scale-10k.log；无失败 |
| mission-benchmark | PASS（exit0） | 5.236 | test-mission-benchmark.log；无失败 |
| Launcher-Smoke | PASS（exit0） | 11.507 | launcher-smoke.log；无失败 |
| UE-DroneGCS | 239/242，3配置相关失败 | 158.307 | 对应UE JSON与日志 |
| UE-DroneGCS-Isolated | PASS（以报告为准） | 53.921 | 对应UE JSON与日志 |
| UE-Synthetic10Kx16 | PASS（以报告为准） | 24.310 | 对应UE JSON与日志 |

Console旧runner不输出内部每个函数独立耗时；上表是实测进程级耗时，不能杜撰每断言耗时。UE每个case耗时见第5节。过程压力测试的CPU/时延受同时构建影响，只作基线观察，未用于性能提升结论。

Launcher实际命令（独立PowerShell进程隔离LOCALAPPDATA）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Launcher/tests/Smoke.ps1
```

UE首轮使用旧roadmap的两个Enabled=False参数及默认地图；复跑的实际完整参数如下（端口有效性限制见上文）：

```powershell
& 'D:/EpicDownLoad/EPICDefault/UE_5.5/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/13092/Documents/DroneGCS/UnrealGCS/DroneShowPlatform1.uproject' /Engine/Maps/Entry -unattended -NullRHI -nosound '-ini:Game:[DSPGCS.ProductionTelemetry]:Enabled=True,ConsoleAddress=127.0.0.1,ConsolePort=19261,UnrealAddress=127.0.0.1,UnrealPort=19262' '-ini:Game:[DSPGCS.ProductionCommand]:Enabled=True,SafeTestMode=True' '-ExecCmds=Automation RunTests DroneGCS.' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=C:/Users/13092/Documents/DroneGCS/UnrealGCS/Saved/Automation/ArchitecturePhase0-20260909-Isolated' '-abslog=C:/Users/13092/Documents/DroneGCS/BuildLogs/ArchitecturePhase0-20260909/ue-tests-isolated.log'
```

Synthetic以`DroneGCSBenchmark.RouteWaypointBatch.2DC2B.Synthetic10Kx16`代替测试前缀，两个Provider Enabled=False，报告新目录`ArchitecturePhase0-20260909-Synthetic`。完整命令行保存在对应UE日志的LogInit记录。默认地图资源警告与Cesium反射错误保留；最终243/243并不表示整个引擎/地图无错误。

## 3. 分类与未执行项目

类别可重叠：A自动；B需UE Editor；C需Console二进制；D需飞控/真机；E需人工/资源。

| 项目 | 类别 | 本轮状态/原因 |
|---|---|---|
| ConsoleGCSTests默认及全部安全独立suite | A+C | 已执行；subset flags被full覆盖的无需重复跑 |
| UE DroneGCS. | A+B；Frozen RealUDP另外C | 242项已执行；fake/回环，不连飞机 |
| Synthetic10Kx16 | A+B | 已执行 |
| Real1000x9100 | B+E（fixture依赖） | 未执行；DRONEGCS_C2B_NPZ未设置，未指定符合1000机×9100帧的数据；不猜文件 |
| DSPGCSAcceptance.exe手工联合验收 | C+E | 已编译；需另一端手工触发singleArm/multiHover，不是独立无参自测；未冒充PASS |
| --frozen-e2e-server | A+B+C | 由UE E2E场景自动启动，作为测试fixture，不单独常驻 |
| --mavftp-test / --mavftp-hardware-test / mission-write probe | D | 禁止执行；MAVFTPMissionHardwareTestTests安全单测不等于这些入口 |
| 真机飞行/继电器/断电/灯光、E.2规模 | D+E | FIELD_BASELINE_REQUIRED；本轮没有硬件证据 |
| 虚拟化滚动、点击命中、面板重开、数字孪生观感 | B+E | 自动源码契约已跑；视觉/输入人工未跑 |
| Shipping/Installer/真实UE联合Launcher | B+C+E | 本轮只Editor与fake frontend Smoke；需新包/staging、人工安装验收 |
| Tools/unreal-mcp/Python/scripts下test_*示例 | B+E | 第三方工具示例会创建Actor/Blueprint/Input资产，违反本轮不改uasset范围，未执行 |

这些不是被跳过的纯内存单测；分别缺外部fixture/人工流程或触及明确禁止项。未新增重复实现生产逻辑的测试；新增verify_phase0.py只比较源码/配置hash和资源Git内容。

## 4. 完整源码入口清单

扫描Console测试目录得到45个Run*测试/benchmark函数（包括被其他runner调用的内部suite），UE得到185个注册项，其中复杂测试展开为242个DroneGCS.用例，另2个Benchmark。以下列表是盘点，不额外算执行次数；执行以结果报告为准。

| Console函数（A+C） | 源码 |
|---|---|
| RunCommandPacketTests | [CommandTests.cpp](../../ConsoleGCS/tests/CommandTests.cpp) L89 |
| RunCommandManagerSafeModeTests | [CommandTests.cpp](../../ConsoleGCS/tests/CommandTests.cpp) L615 |
| RunSessionDisarmTests | [CommandTests.cpp](../../ConsoleGCS/tests/CommandTests.cpp) L688 |
| RunCommandTests | [CommandTests.cpp](../../ConsoleGCS/tests/CommandTests.cpp) L856 |
| RunConsoleArtifactConsumerV2Tests | [ConsoleArtifactConsumerV2Tests.cpp](../../ConsoleGCS/tests/ConsoleArtifactConsumerV2Tests.cpp) L714 |
| RunConsoleArtifactConsumerV2Scale10K | [ConsoleArtifactConsumerV2Tests.cpp](../../ConsoleGCS/tests/ConsoleArtifactConsumerV2Tests.cpp) L738 |
| RunCommandTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L32 |
| RunSessionDisarmTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L33 |
| RunDSPCommandTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L34 |
| RunDSPFrozenAssignmentProtocolTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L35 |
| RunDSPFrozenAssignmentReceiverTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L36 |
| RunConsoleArtifactConsumerV2Tests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L38 |
| RunConsoleArtifactConsumerV2Scale10K | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L39 |
| RunRouteArtifactV2ContractTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L40 |
| RunRouteMissionCodecTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L41 |
| RunMissionFtpCompilerTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L42 |
| RunRouteMissionBenchmarks | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L43 |
| RunDSPUploadJobProtocolTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L44 |
| RunMavFtpRuntimeConfigTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L45 |
| RunMavFtpProbeTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L46 |
| RunMAVFTPMissionWriterTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L47 |
| RunMAVFTPWritePermitConcurrencyTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L48 |
| RunRouteUploadConcurrencySchedulerTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L49 |
| RunMAVFTPMissionHardwareTestTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L50 |
| RunMissionUploadTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L51 |
| RunDroneLogicalIdTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L52 |
| RunConsoleRenderTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L53 |
| RunConsolePipelineTests | [ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) L1021 |
| RunConsoleRenderTests | [ConsoleRenderTests.cpp](../../ConsoleGCS/tests/ConsoleRenderTests.cpp) L486 |
| RunDSPCommandTests | [DSPCommandTests.cpp](../../ConsoleGCS/tests/DSPCommandTests.cpp) L85 |
| RunDSPFrozenAssignmentProtocolTests | [DSPFrozenAssignmentProtocolTests.cpp](../../ConsoleGCS/tests/DSPFrozenAssignmentProtocolTests.cpp) L484 |
| RunDSPFrozenAssignmentReceiverTests | [DSPFrozenAssignmentReceiverTests.cpp](../../ConsoleGCS/tests/DSPFrozenAssignmentReceiverTests.cpp) L1071 |
| RunDSPUploadJobProtocolTests | [DSPUploadJobProtocolTests.cpp](../../ConsoleGCS/tests/DSPUploadJobProtocolTests.cpp) L1530 |
| RunMavFtpRuntimeConfigTests | [DSPUploadJobProtocolTests.cpp](../../ConsoleGCS/tests/DSPUploadJobProtocolTests.cpp) L1556 |
| RunDroneLogicalIdTests | [DroneLogicalIdTests.cpp](../../ConsoleGCS/tests/DroneLogicalIdTests.cpp) L22 |
| RunMAVFTPMissionHardwareTestTests | [MAVFTPMissionHardwareTestTests.cpp](../../ConsoleGCS/tests/MAVFTPMissionHardwareTestTests.cpp) L326 |
| RunMAVFTPMissionWriterTests | [MAVFTPMissionWriterTests.cpp](../../ConsoleGCS/tests/MAVFTPMissionWriterTests.cpp) L1183 |
| RunMAVFTPWritePermitConcurrencyTests | [MAVFTPMissionWriterTests.cpp](../../ConsoleGCS/tests/MAVFTPMissionWriterTests.cpp) L1214 |
| RunMavFtpProbeTests | [MavFtpProbeTests.cpp](../../ConsoleGCS/tests/MavFtpProbeTests.cpp) L1404 |
| RunMissionFtpCompilerTests | [MissionFtpCompilerTests.cpp](../../ConsoleGCS/tests/MissionFtpCompilerTests.cpp) L399 |
| RunMissionUploadTests | [MissionUploadTests.cpp](../../ConsoleGCS/tests/MissionUploadTests.cpp) L1601 |
| RunRouteUploadConcurrencySchedulerTests | [MissionUploadTests.cpp](../../ConsoleGCS/tests/MissionUploadTests.cpp) L1629 |
| RunRouteArtifactV2ContractTests | [RouteArtifactV2ContractTests.cpp](../../ConsoleGCS/tests/RouteArtifactV2ContractTests.cpp) L143 |
| RunRouteMissionCodecTests | [RouteMissionCodecTests.cpp](../../ConsoleGCS/tests/RouteMissionCodecTests.cpp) L581 |
| RunRouteMissionBenchmarks | [RouteMissionCodecTests.cpp](../../ConsoleGCS/tests/RouteMissionCodecTests.cpp) L601 |

| UE注册名（A+B，需fixture者见第3节） | 源码 |
|---|---|
| DroneGCS.Data.DSPGCSFrozenAssignment.SharedContract | [DSPGCSFrozenAssignmentProtocolTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSFrozenAssignmentProtocolTests.cpp) L7 |
| DroneGCS.RouteAssignment.FrozenSync.WireRetryStateValidation | [DSPGCSFrozenAssignmentSyncTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSFrozenAssignmentSyncTests.cpp) L801 |
| DroneGCS.Data.DSPGCSTelemetryChunkAssembly.AtomicThousandDroneSnapshot | [DSPGCSTelemetryChunkTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSTelemetryChunkTests.cpp) L13 |
| DroneGCS.Data.DSPGCSTelemetryLink.HealthTimeoutFleetRetentionAndRecovery | [DSPGCSTelemetryLinkTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSTelemetryLinkTests.cpp) L12 |
| DroneGCS.Data.DSPGCSUploadJob.LiveAggregate | [DSPGCSUploadJobTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSUploadJobTests.cpp) L122 |
| DroneGCS.Data.DSPGCSUploadJob.MirrorReliability | [DSPGCSUploadJobTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSUploadJobTests.cpp) L211 |
| DroneGCS.Data.DSPGCSUploadJob.ClientHardening | [DSPGCSUploadJobTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSUploadJobTests.cpp) L381 |
| DroneGCS.Data.RouteArtifact.UploadLeasePromotionGate | [DSPGCSUploadJobTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSUploadJobTests.cpp) L733 |
| DroneGCS.Visualization.FleetNiagaraBridge.CountOrderAndOfflineStability | [DroneFleetVisualizationTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DroneFleetVisualizationTests.cpp) L13 |
| DroneGCS.Data.FleetGeoOrigin.LockingAndGPSValidity | [FleetGeoOriginTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/FleetGeoOriginTests.cpp) L11 |
| DroneGCS.Export.Benchmark.LegacyFormat | [GCSBenchmarkExportTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSBenchmarkExportTests.cpp) L7 |
| DroneGCS.UI.FleetList.AuthoredContract | [GCSFleetListContractTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSFleetListContractTests.cpp) L15 |
| DroneGCS.UI.VisualCleanup.GameModePawnContract | [GCSGameModeTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSGameModeTests.cpp) L16 |
| DroneGCS.UI.VisualCleanup.PossessionAndCameraViewTarget | [GCSGameModeTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSGameModeTests.cpp) L61 |
| DroneGCS.UI.Navigation.IndependentMapPlaybackLogsToggles | [GCSIndependentToolbarTogglesTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSIndependentToolbarTogglesTests.cpp) L14 |
| DroneGCS.UI.RouteWorkflow.OwnershipAndTeardown | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L26 |
| DroneGCS.UI.RouteWorkflow.ShowOrigin.AnchorCaptureAuthority | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L160 |
| DroneGCS.UI.RouteWorkflow.ViewStatePersistence | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L475 |
| DroneGCS.UI.RouteWorkflow.RequiredWidgetLookupFailure | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L671 |
| DroneGCS.UI.RouteWorkflow.ConvertWithoutAssignmentWidgets | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L692 |
| DroneGCS.UI.RouteWorkflow.ProgressAndFormatting | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L807 |
| DroneGCS.UI.RouteWorkflow.UploadCommandGate | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L883 |
| DroneGCS.UI.RouteWorkflow.ArtifactIdentitySafety | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L958 |
| DroneGCS.UI.RouteWorkflow.RouteListScale | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1006 |
| DroneGCS.UI.RouteWorkflow.PreJobPresentation | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1080 |
| DroneGCS.UI.RouteWorkflow.RouteListSnapshotLifecycle | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1263 |
| DroneGCS.UI.RouteWorkflow.RouteRowContract | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1477 |
| DroneGCS.UI.RouteWorkflow.CommandOriginAndGate | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1526 |
| DroneGCS.UI.RouteWorkflow.CommandDispatch | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1576 |
| DroneGCS.UI.RouteWorkflow.AutoStart.CanonicalDroneId | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1727 |
| DroneGCS.UI.RouteWorkflow.AutoStart.ExactlyOnce | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1767 |
| DroneGCS.UI.RouteWorkflow.AutoStart.SingleTarget | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1824 |
| DroneGCS.UI.RouteWorkflow.AutoStart.FailClosed | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1874 |
| DroneGCS.UI.RouteWorkflow.AutoStart.PreflightRejectMatrix | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1912 |
| DroneGCS.UI.RouteWorkflow.AutoStart.StaleMatrix | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L1951 |
| DroneGCS.UI.RouteWorkflow.NpzNumbering.CaptureBoundSource | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L2004 |
| DroneGCS.UI.RouteWorkflow.NpzNumbering.ConfirmedMappingPriority | [GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) L2068 |
| DroneGCS.UI.TimeControl.ScheduledPreview10Hz | [GCSScheduledShowPreviewTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSScheduledShowPreviewTests.cpp) L16 |
| DroneGCS.UI.Selection.DetailAndCommandStateSeparation | [GCSSelectionStateTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSSelectionStateTests.cpp) L19 |
| DroneGCS.UI.Selection.SnapshotDoesNotAutoSelectDetail | [GCSSelectionStateTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSSelectionStateTests.cpp) L24 |
| DroneGCS.UI.FleetSelectAll.Scale1000 | [GCSSelectionStateTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSSelectionStateTests.cpp) L415 |
| DroneGCS.UI.FleetSelectAll.Scale20000 | [GCSSelectionStateTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSSelectionStateTests.cpp) L426 |
| DroneGCS.UI.TelemetryFilter.Model | [GCSTelemetryFilterTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSTelemetryFilterTests.cpp) L22 |
| DroneGCS.UI.TelemetryFilter.AuthoredWidgets | [GCSTelemetryFilterTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSTelemetryFilterTests.cpp) L91 |
| DroneGCS.UI.TelemetryFilter.PresetsPersistLocally | [GCSTelemetryFilterTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSTelemetryFilterTests.cpp) L235 |
| DroneGCS.UI.TelemetryFilter.CurrentAndCombinedModes | [GCSTelemetryFilterTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSTelemetryFilterTests.cpp) L327 |
| DroneGCS.UI.TelemetryFilter.CountsAndSelection | [GCSTelemetryFilterTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSTelemetryFilterTests.cpp) L409 |
| DroneGCS.UI.TelemetryFilter.IndependentToolbarToggle | [GCSTelemetryFilterTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSTelemetryFilterTests.cpp) L546 |
| DroneGCS.UI.UploadDashboardToggle | [GCSUploadDashboardToggleTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSUploadDashboardToggleTests.cpp) L78 |
| DroneGCS.HoverLayers.01SeparatesNearbyDrones | [HoverLayerSelectionModelTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/HoverLayerSelectionModelTests.cpp) L22 |
| DroneGCS.HoverLayers.02NoLegacyLayerCap | [HoverLayerSelectionModelTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/HoverLayerSelectionModelTests.cpp) L50 |
| DroneGCS.HoverLayers.03RevalidatesCurrentPositions | [HoverLayerSelectionModelTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/HoverLayerSelectionModelTests.cpp) L72 |
| DroneGCS.RouteArtifact.01Digest | [RouteArtifactDomainTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactDomainTests.cpp) L49 |
| DroneGCS.RouteArtifact.02BuildIdentity | [RouteArtifactDomainTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactDomainTests.cpp) L101 |
| DroneGCS.RouteArtifact.03PathSafety | [RouteArtifactDomainTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactDomainTests.cpp) L180 |
| DroneGCS.RouteArtifact.04TenThousandPaths | [RouteArtifactDomainTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactDomainTests.cpp) L296 |
| DroneGCS.RouteArtifact2C2.01CanonicalManifest | [RouteArtifactManifestTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactManifestTests.cpp) L139 |
| DroneGCS.RouteArtifact2C2.02ArtifactSetDigest | [RouteArtifactManifestTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactManifestTests.cpp) L204 |
| DroneGCS.RouteArtifact2C2.03MappingRoutesBinding | [RouteArtifactManifestTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactManifestTests.cpp) L272 |
| DroneGCS.RouteArtifact2C2.04MalformedJson | [RouteArtifactManifestTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactManifestTests.cpp) L358 |
| DroneGCS.RouteArtifact2C2.05TenThousandManifest | [RouteArtifactManifestTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactManifestTests.cpp) L411 |
| DroneGCS.RouteArtifact2C2.06StreamingFileHash | [RouteArtifactManifestTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactManifestTests.cpp) L445 |
| DroneGCS.RouteArtifact2C2.07FileIntegrity | [RouteArtifactManifestTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactManifestTests.cpp) L527 |
| DroneGCS.RouteArtifact.2C3B.CrashRecovery | [RouteArtifactRecoveryTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactRecoveryTests.cpp) L244 |
| DroneGCS.RouteArtifact.2C3B.CleanupAndContention | [RouteArtifactRecoveryTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactRecoveryTests.cpp) L406 |
| DroneGCS.RouteArtifact.2C3B.CurrentAllowlist | [RouteArtifactRecoveryTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactRecoveryTests.cpp) L463 |
| DroneGCS.RouteArtifact.2C3B.ReparseSafety | [RouteArtifactRecoveryTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactRecoveryTests.cpp) L501 |
| DroneGCS.RouteArtifact.2C3B.TenThousandAllowlist | [RouteArtifactRecoveryTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactRecoveryTests.cpp) L599 |
| DroneGCS.RouteArtifact.05SourceIdentityLifecycle | [RouteArtifactSourceIdentityTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactSourceIdentityTests.cpp) L33 |
| DroneGCS.RouteArtifactV2B.WorkspaceSourceSafety | [RouteArtifactV2WorkspaceTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactV2WorkspaceTests.cpp) L369 |
| DroneGCS.RouteArtifactV2B.LegacyReplacement | [RouteArtifactV2WorkspaceTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactV2WorkspaceTests.cpp) L464 |
| DroneGCS.RouteArtifactV2B.RuntimeReplacement | [RouteArtifactV2WorkspaceTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactV2WorkspaceTests.cpp) L518 |
| DroneGCS.RouteArtifactV2B.DeletedCurrentRecovery | [RouteArtifactV2WorkspaceTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactV2WorkspaceTests.cpp) L560 |
| DroneGCS.RouteArtifactV2B.OfflineProductionGeneration | [RouteArtifactV2WorkspaceTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactV2WorkspaceTests.cpp) L632 |
| DroneGCS.RouteArtifact.2C3A.WorkspaceLifecycle | [RouteArtifactWorkspaceTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactWorkspaceTests.cpp) L246 |
| DroneGCS.RouteArtifact.2C3A.FailureAndFinalizeGate | [RouteArtifactWorkspaceTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactWorkspaceTests.cpp) L368 |
| DroneGCS.RouteArtifact.2C3A.ReadyAndCurrentValidation | [RouteArtifactWorkspaceTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactWorkspaceTests.cpp) L490 |
| DroneGCS.RouteArtifact.2C3A.TenThousandDomainLifecycle | [RouteArtifactWorkspaceTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactWorkspaceTests.cpp) L582 |
| DroneGCS.RouteAssignment.AuthoritySession.LifetimeContract | [RouteAssignmentAuthoritySessionTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentAuthoritySessionTests.cpp) L45 |
| DroneGCS.RouteAssignment.Geo.01SameOrigin | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L49 |
| DroneGCS.RouteAssignment.Geo.02LongitudeIncreasesEast | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L72 |
| DroneGCS.RouteAssignment.Geo.03LatitudeIncreasesNorth | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L95 |
| DroneGCS.RouteAssignment.Geo.04EcefEquatorPrimeMeridian | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L118 |
| DroneGCS.RouteAssignment.Geo.05EcefNinetyLongitude | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L134 |
| DroneGCS.RouteAssignment.Geo.06ProductionOriginSamePosition | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L150 |
| DroneGCS.RouteAssignment.Geo.07OfflineRetainedIneligible | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L173 |
| DroneGCS.RouteAssignment.Geo.08InsufficientFixRetainedIneligible | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L200 |
| DroneGCS.RouteAssignment.Geo.09InvalidCoordinatesRetainedIneligible | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L224 |
| DroneGCS.RouteAssignment.Geo.10CountAndOrderPreserved | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L258 |
| DroneGCS.RouteAssignment.Geo.11InvalidOriginRejectsBuild | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L289 |
| DroneGCS.RouteAssignment.Geo.12AltitudeIsAuditOnly | [RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) L308 |
| DroneGCS.RouteAssignment.CandidateEdges.01SamePoint | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L335 |
| DroneGCS.RouteAssignment.CandidateEdges.02ExactBoundaryIncluded | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L359 |
| DroneGCS.RouteAssignment.CandidateEdges.03JustBelowBoundaryIncluded | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L385 |
| DroneGCS.RouteAssignment.CandidateEdges.04JustAboveBoundaryExcluded | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L408 |
| DroneGCS.RouteAssignment.CandidateEdges.05MultipleCandidatesRetained | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L431 |
| DroneGCS.RouteAssignment.CandidateEdges.06CompetingDronesRetained | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L457 |
| DroneGCS.RouteAssignment.CandidateEdges.07EligibleZeroCandidatesVisible | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L483 |
| DroneGCS.RouteAssignment.CandidateEdges.08IneligibleProducesNoEdges | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L505 |
| DroneGCS.RouteAssignment.CandidateEdges.09InputCountMismatchFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L527 |
| DroneGCS.RouteAssignment.CandidateEdges.10InputIdentityMismatchFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L548 |
| DroneGCS.RouteAssignment.CandidateEdges.11CanonicalOrdering | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L568 |
| DroneGCS.RouteAssignment.CandidateEdges.12RepeatEquivalent | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L604 |
| DroneGCS.RouteAssignment.CandidateEdges.13AltitudeAndUpIgnored | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L629 |
| DroneGCS.RouteAssignment.CandidateEdges.14EmptyRoute | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L651 |
| DroneGCS.RouteAssignment.CandidateEdges.15MalformedRouteFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L673 |
| DroneGCS.RouteAssignment.CandidateEdges.16ConfigValidation | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L693 |
| DroneGCS.RouteAssignment.CandidateEdges.17FleetCompatibleIdentity | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L716 |
| DroneGCS.RouteAssignment.MaximumMatching.01OneDroneOneRoute | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L743 |
| DroneGCS.RouteAssignment.MaximumMatching.02OneDroneTwoRoutes | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L762 |
| DroneGCS.RouteAssignment.MaximumMatching.03TwoDronesOneRoute | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L785 |
| DroneGCS.RouteAssignment.MaximumMatching.04GreedyTrap | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L808 |
| DroneGCS.RouteAssignment.MaximumMatching.05ComplexAugmentingPath | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L826 |
| DroneGCS.RouteAssignment.MaximumMatching.06ZeroCandidateDrone | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L847 |
| DroneGCS.RouteAssignment.MaximumMatching.07EmptyDroneSide | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L866 |
| DroneGCS.RouteAssignment.MaximumMatching.08EmptyRouteSide | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L884 |
| DroneGCS.RouteAssignment.MaximumMatching.09DisconnectedGraph | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L903 |
| DroneGCS.RouteAssignment.MaximumMatching.10AllToAll | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L923 |
| DroneGCS.RouteAssignment.MaximumMatching.11EligibleGreaterThanRoutes | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L943 |
| DroneGCS.RouteAssignment.MaximumMatching.12EligibleLessThanRoutes | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L963 |
| DroneGCS.RouteAssignment.MaximumMatching.13DeficientGraph | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L981 |
| DroneGCS.RouteAssignment.MaximumMatching.14WitnessDroneUnique | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1001 |
| DroneGCS.RouteAssignment.MaximumMatching.15WitnessRouteUnique | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1027 |
| DroneGCS.RouteAssignment.MaximumMatching.16WitnessUsesInputEdges | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1053 |
| DroneGCS.RouteAssignment.MaximumMatching.17RepeatDeterministic | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1075 |
| DroneGCS.RouteAssignment.MaximumMatching.18SourceOrderIndependent | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1099 |
| DroneGCS.RouteAssignment.MaximumMatching.19MultipleMaximumDeterministic | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1139 |
| DroneGCS.RouteAssignment.MaximumMatching.20MalformedOffsetsFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1162 |
| DroneGCS.RouteAssignment.MaximumMatching.21DuplicateEdgeFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1181 |
| DroneGCS.RouteAssignment.MaximumMatching.22MalformedRouteFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1199 |
| DroneGCS.RouteAssignment.MaximumMatching.23DistanceIndependent | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1217 |
| DroneGCS.RouteAssignment.MaximumMatching.24MalformedDistanceFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1245 |
| DroneGCS.RouteAssignment.MaximumMatching.25MalformedEdgeIdentityFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1264 |
| DroneGCS.RouteAssignment.MaximumMatching.26MalformedCountFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1283 |
| DroneGCS.RouteAssignment.MinimumCost.01OneDroneOneRoute | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1302 |
| DroneGCS.RouteAssignment.MinimumCost.02OneDroneChoosesLowerCost | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1322 |
| DroneGCS.RouteAssignment.MinimumCost.03TwoDronesChoosesLowerCost | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1341 |
| DroneGCS.RouteAssignment.MinimumCost.04GreedyCostTrap | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1362 |
| DroneGCS.RouteAssignment.MinimumCost.05PreservesMaximumCardinality | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1385 |
| DroneGCS.RouteAssignment.MinimumCost.06ResidualReassignment | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1405 |
| DroneGCS.RouteAssignment.MinimumCost.07DisconnectedComponents | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1428 |
| DroneGCS.RouteAssignment.MinimumCost.08EqualCostDeterministic | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1454 |
| DroneGCS.RouteAssignment.MinimumCost.09CandidateInputOrderIndependent | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1475 |
| DroneGCS.RouteAssignment.MinimumCost.10PairUniqueness | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1499 |
| DroneGCS.RouteAssignment.MinimumCost.11WitnessUsesCandidateEdges | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1528 |
| DroneGCS.RouteAssignment.MinimumCost.12CanonicalTotalRecomputation | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1550 |
| DroneGCS.RouteAssignment.MinimumCost.13ExactBoundaryEdge | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1578 |
| DroneGCS.RouteAssignment.MinimumCost.14ZeroMaximumCardinality | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1597 |
| DroneGCS.RouteAssignment.MinimumCost.15EligibleGreaterThanRoutes | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1616 |
| DroneGCS.RouteAssignment.MinimumCost.16EligibleLessThanRoutes | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1640 |
| DroneGCS.RouteAssignment.MinimumCost.17MalformedMaximumWitnessFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1661 |
| DroneGCS.RouteAssignment.MinimumCost.18CardinalityMismatchFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1685 |
| DroneGCS.RouteAssignment.MinimumCost.19MalformedCandidateGraphFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1713 |
| DroneGCS.RouteAssignment.MinimumCost.20NonFiniteDistanceFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1737 |
| DroneGCS.RouteAssignment.MinimumCost.21RepeatDeterministic | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1766 |
| DroneGCS.RouteAssignment.MinimumCost.22DistanceChangesOptimalWitness | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1791 |
| DroneGCS.RouteAssignment.MinimumCost.23BruteForceOracleFixed | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1820 |
| DroneGCS.RouteAssignment.MinimumCost.24BruteForceOracleSynthetic | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1847 |
| DroneGCS.RouteAssignment.MinimumCost.25CloseDoubleCostsRemainDistinct | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1901 |
| DroneGCS.RouteAssignment.MinimumCost.26RoundoffScaleReducedCostClamps | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1923 |
| DroneGCS.RouteAssignment.MinimumCost.27MaterialNegativeReducedCostFails | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1941 |
| DroneGCS.RouteAssignment.MinimumCost.28Sparse1000Baseline | [RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) L1959 |
| DroneGCS.Data.RouteGpsCapture.F1.ConfiguredPolicy | [RouteGpsCaptureTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteGpsCaptureTests.cpp) L102 |
| DroneGCS.Data.RouteGpsCapture.F1.ProductionTelemetryPreservation | [RouteGpsCaptureTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteGpsCaptureTests.cpp) L126 |
| DroneGCS.Data.RouteGpsCapture.F1.SelectionStableWindow | [RouteGpsCaptureTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteGpsCaptureTests.cpp) L215 |
| DroneGCS.Data.RouteGpsCapture.F1.Invalidation | [RouteGpsCaptureTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteGpsCaptureTests.cpp) L312 |
| DroneGCS.Data.RouteGpsCapture.F1.TransactionIsolation | [RouteGpsCaptureTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteGpsCaptureTests.cpp) L401 |
| DroneGCS.Data.RouteGpsCapture.F1.Robustness | [RouteGpsCaptureTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteGpsCaptureTests.cpp) L462 |
| DroneGCS.Data.RouteGpsCapture.F1.ExplicitParticipantAuthority | [RouteGpsCaptureTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteGpsCaptureTests.cpp) L540 |
| DroneGCS.RouteMission.01CrossBuildGolden | [RouteMissionCodecTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteMissionCodecTests.cpp) L37 |
| DroneGCS.RouteWaypointBatch.2DC2A.DeterminismAndDegraded | [RouteWaypointBatchGeneratorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointBatchGeneratorTests.cpp) L312 |
| DroneGCS.RouteWaypointBatch.2DC2A.FailureCancelAndStaleGate | [RouteWaypointBatchGeneratorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointBatchGeneratorTests.cpp) L362 |
| DroneGCS.RouteWaypointBatch.2DC2A.SnapshotDiskAnd10K | [RouteWaypointBatchGeneratorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointBatchGeneratorTests.cpp) L571 |
| DroneGCS.RouteWaypointBatch.2DC2B.StrictBoundsAndShutdown | [RouteWaypointC2BBenchmarkTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointC2BBenchmarkTests.cpp) L1012 |
| DroneGCS.RouteWaypointBatch.2DC2B.BoundedCancelShutdown | [RouteWaypointC2BBenchmarkTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointC2BBenchmarkTests.cpp) L1050 |
| DroneGCSBenchmark.RouteWaypointBatch.2DC2B.Synthetic10Kx16 | [RouteWaypointC2BBenchmarkTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointC2BBenchmarkTests.cpp) L1101 |
| DroneGCSBenchmark.RouteWaypointBatch.2DC2B.Real1000x9100 | [RouteWaypointC2BBenchmarkTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointC2BBenchmarkTests.cpp) L1173 |
| DroneGCS.RouteWaypointGenerator.2DC1.GeoAndContract | [RouteWaypointGeneratorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointGeneratorTests.cpp) L176 |
| DroneGCS.RouteWaypointGenerator.2DC1.RgbSaturationCompatibility | [RouteWaypointGeneratorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointGeneratorTests.cpp) L300 |
| DroneGCS.RouteWaypointGenerator.2DC1.GoldenFiles | [RouteWaypointGeneratorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointGeneratorTests.cpp) L408 |
| DroneGCS.RouteWaypointGenerator.2DC1.ValidationFailureCancellation | [RouteWaypointGeneratorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointGeneratorTests.cpp) L458 |
| DroneGCS.RouteWaypointGenerator.2DC1.WorkspaceStagingAdapter | [RouteWaypointGeneratorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointGeneratorTests.cpp) L567 |
| DroneGCS.RouteWaypointValidation.2EB.StreamingSemanticAndSourceBound | [RouteWaypointValidatorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointValidatorTests.cpp) L148 |
| DroneGCS.RouteWaypointValidation.2EB.MalformedCorpus | [RouteWaypointValidatorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointValidatorTests.cpp) L239 |
| DroneGCS.RouteWaypointValidation.2EB.SourceCorruption | [RouteWaypointValidatorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointValidatorTests.cpp) L323 |

## 5. 最终逐项结果与耗时

Success含warning的条目仍注明数量；诊断全文见PHASE0_EVIDENCE.json。同一测试首轮失败信息另列第6节。

| 测试名 | 结果 | 秒 | Warning/Error |
|---|---|---:|---|
| DroneGCS.Data.DSPGCSFrozenAssignment.SharedContract | Success | 0.001636 | 0/0 |
| DroneGCS.Data.DSPGCSTelemetryChunkAssembly.AtomicThousandDroneSnapshot | Success | 0.048859 | 0/0 |
| DroneGCS.Data.DSPGCSTelemetryLink.HealthTimeoutFleetRetentionAndRecovery | Success | 0.008298 | 0/0 |
| DroneGCS.Data.DSPGCSUploadJob.ClientHardening | Success | 0.007797 | 0/0 |
| DroneGCS.Data.DSPGCSUploadJob.LiveAggregate | Success | 0.007639 | 0/0 |
| DroneGCS.Data.DSPGCSUploadJob.MirrorReliability | Success | 0.008462 | 0/0 |
| DroneGCS.Data.FleetGeoOrigin.LockingAndGPSValidity | Success | 0.046117 | 0/0 |
| DroneGCS.Data.RouteArtifact.UploadLeasePromotionGate | Success | 0.008195 | 0/0 |
| DroneGCS.Data.RouteGpsCapture.F1.ConfiguredPolicy | Success | 0.008399 | 0/0 |
| DroneGCS.Data.RouteGpsCapture.F1.ExplicitParticipantAuthority | Success | 0.026949 | 0/0 |
| DroneGCS.Data.RouteGpsCapture.F1.Invalidation | Success | 0.045759 | 0/0 |
| DroneGCS.Data.RouteGpsCapture.F1.ProductionTelemetryPreservation | Success | 0.046070 | 0/0 |
| DroneGCS.Data.RouteGpsCapture.F1.Robustness | Success | 0.045878 | 0/0 |
| DroneGCS.Data.RouteGpsCapture.F1.SelectionStableWindow | Success | 0.046579 | 0/0 |
| DroneGCS.Data.RouteGpsCapture.F1.TransactionIsolation | Success | 0.046393 | 0/0 |
| DroneGCS.Export.Benchmark.LegacyFormat | Success | 0.007891 | 0/0 |
| DroneGCS.HoverLayers.01SeparatesNearbyDrones | Success | 0.008333 | 0/0 |
| DroneGCS.HoverLayers.02NoLegacyLayerCap | Success | 0.008067 | 0/0 |
| DroneGCS.HoverLayers.03RevalidatesCurrentPositions | Success | 0.008135 | 0/0 |
| DroneGCS.RouteArtifact.2C3A.FailureAndFinalizeGate | Success | 0.133413 | 10/0 |
| DroneGCS.RouteArtifact.2C3A.ReadyAndCurrentValidation | Success | 0.075029 | 0/0 |
| DroneGCS.RouteArtifact.2C3A.TenThousandDomainLifecycle | Success | 0.008533 | 0/0 |
| DroneGCS.RouteArtifact.2C3A.WorkspaceLifecycle | Success | 0.104359 | 1/0 |
| DroneGCS.RouteArtifact.2C3B.CleanupAndContention | Success | 0.031433 | 0/0 |
| DroneGCS.RouteArtifact.2C3B.CrashRecovery | Success | 0.187035 | 1/0 |
| DroneGCS.RouteArtifact.2C3B.CurrentAllowlist | Success | 0.184302 | 6/0 |
| DroneGCS.RouteArtifact.2C3B.ReparseSafety | Success | 0.063148 | 2/0 |
| DroneGCS.RouteArtifact.2C3B.TenThousandAllowlist | Success | 0.013459 | 0/0 |
| DroneGCS.RouteArtifact.01Digest | Success | 0.008344 | 0/0 |
| DroneGCS.RouteArtifact.02BuildIdentity | Success | 0.008232 | 0/0 |
| DroneGCS.RouteArtifact.03PathSafety | Success | 0.008276 | 0/0 |
| DroneGCS.RouteArtifact.04TenThousandPaths | Success | 0.009962 | 0/0 |
| DroneGCS.RouteArtifact.05SourceIdentityLifecycle | Success | 0.047795 | 1/0 |
| DroneGCS.RouteArtifact2C2.01CanonicalManifest | Success | 0.008182 | 0/0 |
| DroneGCS.RouteArtifact2C2.02ArtifactSetDigest | Success | 0.008235 | 0/0 |
| DroneGCS.RouteArtifact2C2.03MappingRoutesBinding | Success | 0.008274 | 0/0 |
| DroneGCS.RouteArtifact2C2.04MalformedJson | Success | 0.008229 | 0/0 |
| DroneGCS.RouteArtifact2C2.05TenThousandManifest | Success | 0.100600 | 0/0 |
| DroneGCS.RouteArtifact2C2.06StreamingFileHash | Success | 0.063617 | 0/0 |
| DroneGCS.RouteArtifact2C2.07FileIntegrity | Success | 0.072093 | 0/0 |
| DroneGCS.RouteArtifactV2B.DeletedCurrentRecovery | Success | 0.042227 | 1/0 |
| DroneGCS.RouteArtifactV2B.LegacyReplacement | Success | 0.055426 | 1/0 |
| DroneGCS.RouteArtifactV2B.OfflineProductionGeneration | Success | 0.213347 | 0/0 |
| DroneGCS.RouteArtifactV2B.RuntimeReplacement | Success | 0.053134 | 0/0 |
| DroneGCS.RouteArtifactV2B.WorkspaceSourceSafety | Success | 0.024917 | 2/0 |
| DroneGCS.RouteAssignment.AuthorityDefense.01DistanceValidityRequired | Success | 0.007693 | 1/0 |
| DroneGCS.RouteAssignment.AuthorityDefense.02NaNRejected | Success | 0.008350 | 1/0 |
| DroneGCS.RouteAssignment.AuthorityDefense.03NegativeRejected | Success | 0.008194 | 1/0 |
| DroneGCS.RouteAssignment.AuthorityDefense.04ExactBoundaryAccepted | Success | 0.007009 | 0/0 |
| DroneGCS.RouteAssignment.AuthorityDefense.05AboveBoundaryRejected | Success | 0.007913 | 1/0 |
| DroneGCS.RouteAssignment.AuthorityDefense.06Point35Rejected | Success | 0.007961 | 1/0 |
| DroneGCS.RouteAssignment.AuthoritySession.LifetimeContract | Success | 0.072445 | 6/0 |
| DroneGCS.RouteAssignment.CandidateEdges.01SamePoint | Success | 0.008534 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.02ExactBoundaryIncluded | Success | 0.007981 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.03JustBelowBoundaryIncluded | Success | 0.008911 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.04JustAboveBoundaryExcluded | Success | 0.008529 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.05MultipleCandidatesRetained | Success | 0.008076 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.06CompetingDronesRetained | Success | 0.008175 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.07EligibleZeroCandidatesVisible | Success | 0.009989 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.08IneligibleProducesNoEdges | Success | 0.008144 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.09InputCountMismatchFails | Success | 0.007655 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.10InputIdentityMismatchFails | Success | 0.008056 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.11CanonicalOrdering | Success | 0.008369 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.12RepeatEquivalent | Success | 0.008486 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.13AltitudeAndUpIgnored | Success | 0.008708 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.14EmptyRoute | Success | 0.008367 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.15MalformedRouteFails | Success | 0.007649 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.16ConfigValidation | Success | 0.008331 | 0/0 |
| DroneGCS.RouteAssignment.CandidateEdges.17FleetCompatibleIdentity | Success | 0.008153 | 0/0 |
| DroneGCS.RouteAssignment.Explicit.01ValidThreeBindings | Success | 0.008247 | 0/0 |
| DroneGCS.RouteAssignment.Explicit.02PartialDegradedDomain | Success | 0.008217 | 0/0 |
| DroneGCS.RouteAssignment.Explicit.03InvalidInputs | Success | 0.008367 | 0/0 |
| DroneGCS.RouteAssignment.Explicit.04DistanceContract | Success | 0.008430 | 2/0 |
| DroneGCS.RouteAssignment.FrozenSync.WireRetryStateValidation | Success | 6.734070 | 16/0 |
| DroneGCS.RouteAssignment.Geo.01SameOrigin | Success | 0.008275 | 0/0 |
| DroneGCS.RouteAssignment.Geo.02LongitudeIncreasesEast | Success | 0.008003 | 0/0 |
| DroneGCS.RouteAssignment.Geo.03LatitudeIncreasesNorth | Success | 0.008019 | 0/0 |
| DroneGCS.RouteAssignment.Geo.04EcefEquatorPrimeMeridian | Success | 0.008134 | 0/0 |
| DroneGCS.RouteAssignment.Geo.05EcefNinetyLongitude | Success | 0.007994 | 0/0 |
| DroneGCS.RouteAssignment.Geo.06ProductionOriginSamePosition | Success | 0.008123 | 0/0 |
| DroneGCS.RouteAssignment.Geo.07OfflineRetainedIneligible | Success | 0.008195 | 0/0 |
| DroneGCS.RouteAssignment.Geo.08InsufficientFixRetainedIneligible | Success | 0.008239 | 0/0 |
| DroneGCS.RouteAssignment.Geo.09InvalidCoordinatesRetainedIneligible | Success | 0.008392 | 0/0 |
| DroneGCS.RouteAssignment.Geo.10CountAndOrderPreserved | Success | 0.008295 | 0/0 |
| DroneGCS.RouteAssignment.Geo.11InvalidOriginRejectsBuild | Success | 0.008360 | 0/0 |
| DroneGCS.RouteAssignment.Geo.12AltitudeIsAuditOnly | Success | 0.008139 | 0/0 |
| DroneGCS.RouteAssignment.Integration.01LiveSourceEndToEnd | Success | 0.046474 | 0/0 |
| DroneGCS.RouteAssignment.Integration.02CaptureFailurePreservesWorking | Success | 0.046188 | 0/0 |
| DroneGCS.RouteAssignment.Integration.03FrozenRejectsBeforeCapture | Success | 0.046052 | 0/0 |
| DroneGCS.RouteAssignment.Integration.04ExplicitAuthorityAndStaleSources | Success | 0.046892 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.01EqualCountsMatchedOnly | Success | 0.008143 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.02MoreDronesProducesSpare | Success | 0.008291 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.03FewerDronesProducesVacant | Success | 0.008219 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.04DeficientGraphProducesUnresolved | Success | 0.008247 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.05DegreeZeroIsNoCandidate | Success | 0.008276 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.06DegreePositiveIsConflict | Success | 0.008120 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.07MultipleCandidatesNotAmbiguous | Success | 0.008184 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.08OfflineExcluded | Success | 0.008295 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.09InvalidGPSExcluded | Success | 0.008161 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.10InsufficientFixExcluded | Success | 0.008629 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.11InvalidLatLonExcluded | Success | 0.008235 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.12DerivedGeoInvalidExcluded | Success | 0.008225 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.13DronePartitionComplete | Success | 0.008395 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.14RoutePartitionComplete | Success | 0.008346 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.15DroneBucketsDisjoint | Success | 0.008278 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.16VacantForbidsSpare | Success | 0.008328 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.17ZeroRoutesMakesEligibleSpare | Success | 0.008351 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.18ZeroEligibleAllRoutesVacant | Success | 0.008283 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.19PairOverProductionThresholdFails | Success | 0.008229 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.20PairDistanceMismatchFails | Success | 0.008244 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.21PairAbsentFromGraphFails | Success | 0.008325 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.22DuplicatePairDroneFails | Success | 0.008125 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.23DuplicatePairRouteFails | Success | 0.008198 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.24LocalCountMismatchFails | Success | 0.008244 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.25LocalSourceIdentityMismatchFails | Success | 0.008218 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.26GraphMissingEligibleDroneFails | Success | 0.008177 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.27GraphExtraEligibleDroneFails | Success | 0.008253 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.28RouteCountMismatchFails | Success | 0.008331 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.29InvalidSourceIdentityFails | Success | 0.008265 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.30CanonicalOutputOrdering | Success | 0.008593 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.31RepeatInputEquivalent | Success | 0.008219 | 0/0 |
| DroneGCS.RouteAssignment.Materializer.32EquivalentEdgeOrderEquivalentCandidate | Success | 0.008280 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.01OneDroneOneRoute | Success | 0.008222 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.02OneDroneTwoRoutes | Success | 0.008290 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.03TwoDronesOneRoute | Success | 0.008301 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.04GreedyTrap | Success | 0.008219 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.05ComplexAugmentingPath | Success | 0.008349 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.06ZeroCandidateDrone | Success | 0.008583 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.07EmptyDroneSide | Success | 0.008273 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.08EmptyRouteSide | Success | 0.008277 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.09DisconnectedGraph | Success | 0.008243 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.10AllToAll | Success | 0.008414 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.11EligibleGreaterThanRoutes | Success | 0.008274 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.12EligibleLessThanRoutes | Success | 0.007855 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.13DeficientGraph | Success | 0.008192 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.14WitnessDroneUnique | Success | 0.006603 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.15WitnessRouteUnique | Success | 0.007852 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.16WitnessUsesInputEdges | Success | 0.008404 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.17RepeatDeterministic | Success | 0.008361 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.18SourceOrderIndependent | Success | 0.008272 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.19MultipleMaximumDeterministic | Success | 0.008237 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.20MalformedOffsetsFails | Success | 0.008283 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.21DuplicateEdgeFails | Success | 0.008247 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.22MalformedRouteFails | Success | 0.008286 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.23DistanceIndependent | Success | 0.008215 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.24MalformedDistanceFails | Success | 0.007917 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.25MalformedEdgeIdentityFails | Success | 0.008143 | 0/0 |
| DroneGCS.RouteAssignment.MaximumMatching.26MalformedCountFails | Success | 0.006560 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.01OneDroneOneRoute | Success | 0.008278 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.02OneDroneChoosesLowerCost | Success | 0.008306 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.03TwoDronesChoosesLowerCost | Success | 0.007728 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.04GreedyCostTrap | Success | 0.008272 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.05PreservesMaximumCardinality | Success | 0.008202 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.06ResidualReassignment | Success | 0.008465 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.07DisconnectedComponents | Success | 0.008387 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.08EqualCostDeterministic | Success | 0.008322 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.09CandidateInputOrderIndependent | Success | 0.008198 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.10PairUniqueness | Success | 0.007871 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.11WitnessUsesCandidateEdges | Success | 0.008349 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.12CanonicalTotalRecomputation | Success | 0.008345 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.13ExactBoundaryEdge | Success | 0.008375 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.14ZeroMaximumCardinality | Success | 0.008214 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.15EligibleGreaterThanRoutes | Success | 0.008282 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.16EligibleLessThanRoutes | Success | 0.008309 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.17MalformedMaximumWitnessFails | Success | 0.008098 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.18CardinalityMismatchFails | Success | 0.008282 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.19MalformedCandidateGraphFails | Success | 0.008490 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.20NonFiniteDistanceFails | Success | 0.007302 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.21RepeatDeterministic | Success | 0.008379 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.22DistanceChangesOptimalWitness | Success | 0.008337 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.23BruteForceOracleFixed | Success | 0.007813 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.24BruteForceOracleSynthetic | Success | 0.008347 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.25CloseDoubleCostsRemainDistinct | Success | 0.008257 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.26RoundoffScaleReducedCostClamps | Success | 0.007802 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.27MaterialNegativeReducedCostFails | Success | 0.008094 | 0/0 |
| DroneGCS.RouteAssignment.MinimumCost.28Sparse1000Baseline | Success | 0.179956 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.01FullOneToOne | Success | 0.008485 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.02FullWithSpare | Success | 0.008211 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.03DegradedWithVacant | Success | 0.008262 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.04ExcludedStillFull | Success | 0.008179 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.05NoCandidateDraft | Success | 0.008208 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.06DeficientConflictDraft | Success | 0.008195 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.07ZeroEligibleDraft | Success | 0.008280 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.08ExactProductionBoundary | Success | 0.008366 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.08FixedFrameAnchorMatrix | Success | 0.008110 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.09InvalidSourceFailsWithStage | Success | 0.008221 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.10RepeatDeterministic | Success | 0.008325 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.11CanonicalEquivalentCandidate | Success | 0.008352 | 0/0 |
| DroneGCS.RouteAssignment.Pipeline.12ProductionThresholdActuallyUsed | Success | 0.008372 | 0/0 |
| DroneGCS.RouteMission.01CrossBuildGolden | Success | 0.007720 | 0/0 |
| DroneGCS.RouteWaypointBatch.2DC2A.DeterminismAndDegraded | Success | 0.317520 | 0/0 |
| DroneGCS.RouteWaypointBatch.2DC2A.FailureCancelAndStaleGate | Success | 0.152850 | 0/0 |
| DroneGCS.RouteWaypointBatch.2DC2A.SnapshotDiskAnd10K | Success | 12.620590 | 0/0 |
| DroneGCS.RouteWaypointBatch.2DC2B.BoundedCancelShutdown | Success | 0.025620 | 0/0 |
| DroneGCS.RouteWaypointBatch.2DC2B.StrictBoundsAndShutdown | Success | 0.008220 | 0/0 |
| DroneGCS.RouteWaypointGenerator.2DC1.GeoAndContract | Success | 0.008260 | 0/0 |
| DroneGCS.RouteWaypointGenerator.2DC1.GoldenFiles | Success | 0.008196 | 0/0 |
| DroneGCS.RouteWaypointGenerator.2DC1.RgbSaturationCompatibility | Success | 0.008277 | 0/0 |
| DroneGCS.RouteWaypointGenerator.2DC1.ValidationFailureCancellation | Success | 0.008312 | 0/0 |
| DroneGCS.RouteWaypointGenerator.2DC1.WorkspaceStagingAdapter | Success | 0.017509 | 0/0 |
| DroneGCS.RouteWaypointValidation.2EB.MalformedCorpus | Success | 0.259078 | 0/0 |
| DroneGCS.RouteWaypointValidation.2EB.SourceCorruption | Success | 0.036441 | 0/0 |
| DroneGCS.RouteWaypointValidation.2EB.StreamingSemanticAndSourceBound | Success | 0.022343 | 0/0 |
| DroneGCS.UI.FleetList.AuthoredContract | Success | 0.047848 | 0/0 |
| DroneGCS.UI.FleetSelectAll.Scale1000 | Success | 0.047223 | 0/0 |
| DroneGCS.UI.FleetSelectAll.Scale20000 | Success | 0.067515 | 0/0 |
| DroneGCS.UI.Navigation.IndependentMapPlaybackLogsToggles | Success | 0.089975 | 0/0 |
| DroneGCS.UI.RouteWorkflow.AutoStart.CanonicalDroneId | Success | 0.008989 | 0/0 |
| DroneGCS.UI.RouteWorkflow.AutoStart.ExactlyOnce | Success | 0.007893 | 0/0 |
| DroneGCS.UI.RouteWorkflow.AutoStart.FailClosed | Success | 0.008057 | 0/0 |
| DroneGCS.UI.RouteWorkflow.AutoStart.PreflightRejectMatrix | Success | 0.008305 | 0/0 |
| DroneGCS.UI.RouteWorkflow.AutoStart.SingleTarget | Success | 0.008128 | 2/0 |
| DroneGCS.UI.RouteWorkflow.AutoStart.StaleMatrix | Success | 0.008188 | 0/0 |
| DroneGCS.UI.RouteWorkflow.NpzNumbering.CaptureBoundSource | Success | 0.008154 | 0/0 |
| DroneGCS.UI.RouteWorkflow.NpzNumbering.ConfirmedMappingPriority | Success | 0.008150 | 0/0 |
| DroneGCS.UI.RouteWorkflow.ShowOrigin.AnchorCaptureAuthority | Success | 0.066989 | 0/0 |
| DroneGCS.UI.RouteWorkflow.ArtifactIdentitySafety | Success | 0.008067 | 0/0 |
| DroneGCS.UI.RouteWorkflow.CommandDispatch | Success | 0.046211 | 0/0 |
| DroneGCS.UI.RouteWorkflow.CommandOriginAndGate | Success | 0.008259 | 0/0 |
| DroneGCS.UI.RouteWorkflow.ConvertWithoutAssignmentWidgets | Success | 0.046279 | 0/0 |
| DroneGCS.UI.RouteWorkflow.OwnershipAndTeardown | Success | 0.047655 | 2/0 |
| DroneGCS.UI.RouteWorkflow.PreJobPresentation | Success | 0.046602 | 0/0 |
| DroneGCS.UI.RouteWorkflow.ProgressAndFormatting | Success | 0.007654 | 0/0 |
| DroneGCS.UI.RouteWorkflow.RequiredWidgetLookupFailure | Success | 0.068838 | 0/0 |
| DroneGCS.UI.RouteWorkflow.RouteListScale | Success | 0.066461 | 0/0 |
| DroneGCS.UI.RouteWorkflow.RouteListSnapshotLifecycle | Success | 0.050957 | 0/0 |
| DroneGCS.UI.RouteWorkflow.RouteRowContract | Success | 0.045795 | 0/0 |
| DroneGCS.UI.RouteWorkflow.UploadCommandGate | Success | 0.007941 | 0/0 |
| DroneGCS.UI.RouteWorkflow.ViewStatePersistence | Success | 0.046511 | 0/0 |
| DroneGCS.UI.Selection.DetailAndCommandStateSeparation | Success | 0.053551 | 3/0 |
| DroneGCS.UI.Selection.SnapshotDoesNotAutoSelectDetail | Success | 0.048802 | 0/0 |
| DroneGCS.UI.TelemetryFilter.AuthoredWidgets | Success | 0.069412 | 0/0 |
| DroneGCS.UI.TelemetryFilter.CountsAndSelection | Success | 0.046186 | 0/0 |
| DroneGCS.UI.TelemetryFilter.CurrentAndCombinedModes | Success | 0.045343 | 0/0 |
| DroneGCS.UI.TelemetryFilter.IndependentToolbarToggle | Success | 0.048827 | 0/0 |
| DroneGCS.UI.TelemetryFilter.Model | Success | 0.008073 | 0/0 |
| DroneGCS.UI.TelemetryFilter.PresetsPersistLocally | Success | 0.088277 | 0/0 |
| DroneGCS.UI.TimeControl.ScheduledPreview10Hz | Success | 0.046463 | 0/0 |
| DroneGCS.UI.VisualCleanup.GameModePawnContract | Success | 0.008065 | 0/0 |
| DroneGCS.UI.VisualCleanup.PossessionAndCameraViewTarget | Success | 0.048825 | 0/0 |
| DroneGCS.UI.UploadDashboardToggle | Success | 0.093082 | 0/0 |
| DroneGCS.Visualization.FleetNiagaraBridge.CountOrderAndOfflineStability | Success | 0.049382 | 0/0 |
| DroneGCSBenchmark.RouteWaypointBatch.2DC2B.Synthetic10Kx16 | Success | 8.730362 | 0/0 |

## 6. 首轮失败保留

### DroneGCS.Data.DSPGCSTelemetryChunkAssembly.AtomicThousandDroneSnapshot

0.140121秒；首轮Fail；复跑Success。主要诊断：

- Expected 'All drones reach FleetManager atomically' to be 1000, but it was 0.
- Expected 'Complete snapshot applies exactly once' to be 1, but it was 0.
- Expected 'Provider records the complete snapshot count' to be 1000, but it was 0.
- Expected 'Complete valid snapshot keeps telemetry link healthy' to be true.
### DroneGCS.Data.RouteGpsCapture.F1.ProductionTelemetryPreservation

0.028904秒；首轮Fail；复跑Success。主要诊断：

- Expected 'UE preserves Console GPS receive timestamp' to be 1788945744919, but it was 0.
- Expected 'First production DTO creates one unique GPS sample' to be 1, but it was 0.
- Expected 'Repeated production DTO keeps one unique GPS sample' to be 1, but it was 0.
- Expected 'New production GPS timestamp creates second sample' to be 2, but it was 0.
### DroneGCS.UI.Selection.DetailAndCommandStateSeparation

0.039955秒；首轮Fail；复跑Success。主要诊断：

- LogTemp: DSP-GCS command rejected locally: provider/targets invalid.
- Expected 'Production command provider enabled' to be true.
- LogTemp: DSP-GCS command rejected locally: provider/targets invalid.
- Operator HOVER enters autonomous workflow mode before submission: The two values are not equal.

其余28条错误保留在JSON。

## 7. TESTED_BASELINE与尚缺证据

Console CommandTests捕获Arm/Disarm/模式/起降/任务/高级命令包，DSPCommandTests验证多目标、转换、结果关联；当前通过不等于所有timeout/取消/重入排列全覆盖。零基ID、confirmed选择变化、上传冻结/幂等/恢复、源/端点不一致等由对应现有suite和UE Coordinator场景提供自动证据。协议本轮版本：DSP8、Frozen2、Upload3、RuntimeConfig1；hash与现有packet fixture源码纳入固定清单。

尚缺：真实RGB/GPS精度和物理执行，丢失最终command result与ApplyResultBatch事件重入穷尽验证、热重载/全部重复Bind交错、重启后身份稳定、实际安装包与地图依赖。只记录风险，不为Phase0拆业务或引入第二Authority。
