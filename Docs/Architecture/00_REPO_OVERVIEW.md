# 仓库总览与审计清单

Status: AUDIT；2026-09-09；HEAD `a95309d8b37ecb00c779d5c4264aacb86294d8ec`（9/9优化2）。
开始时 master 工作树干净；最近提交 a95309d、f115587、8f438d6、660419d、006d8b0、d455f45、1ab08b5、e6e43d4。文档变更尚未提交。

## 1. 范围与方法

完整枚举Git跟踪的一方文件，全文扫描include/声明/函数/状态/委托/异步/线程/常量及引用；沿Command、Telemetry、Upload主要运行链阅读实现、配置和对应测试。统计不包括第三方与生成MAVLink、UE自身、.git/.vs/Binaries/Intermediate/Saved/DerivedDataCache。外部Tools/unreal-mcp视为外部开发工具，只检查插件接入与Editor-only边界；未改或深审第三方实现。没有运行网络、编译、测试、Cook或编辑资源。

筛选后的跟踪条目385个，其中一方cpp/h/inl文本251个。统计基于审计开始时Git索引，不包含本轮新增文档。下面逐文件清单包含测试与Developer实现，避免将测试代码当生产代码。物理行数含空白和注释；没有使用它作为质量评分。

资产只核对跟踪路径、C++字符串/反射契约与authored测试，不读取完整Blueprint图、不声称已证明资产无隐藏引用。机器文本扫描不是每一函数的形式化验证；发现的动态风险明确标为待验证。

## 2. 模块与工程

| 模块 | 实际内容/入口 | 边界 |
|---|---|---|
| ConsoleGCS | main→ConsoleGCSApplication；DroneGCS.vcxproj，ConsoleGCSTests、DSPGCSAcceptance独立工程 | C++17/v143；真实Endpoint/协议/Session/命令/上传/诊断 |
| Shared | DSP、Transport接口/config、Identity、Artifact、WPL、MissionCodec | header-only合同与算法，也有Windows read lease/hash；不持有Fleet |
| UnrealGCS | 一个DroneShowPlatform1 Runtime模块；Game/Editor Target | UI、应用状态、Route领域、DSP客户端与渲染共模块 |
| Tools/Launcher | Launcher.cpp；独立vcxproj直接编译AppConfig.cpp和TransportConfigLoader.cpp | 构建payload、进程/用户目录/Ready，不拥有Session |
| Tools/Installer | BuildInstaller.ps1、DroneGCS.nsi | Shipping验证、安装规则、保留用户数据 |
| Tools/unreal-mcp / UnrealMCP插件 | 外部编辑开发工具；uproject TargetAllowList Editor、uplugin Editor模块 | 不作为Production通信或UI业务Authority |
| Docs / README | 冻结约束、历史验证、工具使用 | 现状与源码冲突时以当前源码为准，差异见01 |

无一方CMakeLists.txt或.cmake；当前构建是MSBuild + UBT/UAT + PowerShell/NSIS，不凭空新增CMake。

## 3. 已有功能和边界

Fleet V2、独立Toolbar、手动NPZ、真实RGB/GPS插值、Hover工作流、高级命令、MAVFTP-only上传、稳定GPS采集与确认NPZ编号、runtime Write cap均在源码中。旧交接漏写零基映射、confirmed目标集合、runtime config与Hover线程，见[01](01_CURRENT_ARCHITECTURE.md)。

硬件关机差异、备用机/补点/重分配/冲突/重新入场、E.2规模、重启稳定身份不在本治理范围。既有1.0.0发布证据不代表HEAD功能已打包。本轮无新PASS证据。

## 4. 文档导航

- [01 当前运行链与状态归属](01_CURRENT_ARCHITECTURE.md)
- [02 审计问题](02_ARCHITECTURE_AUDIT.md)
- [03 模块与类依赖](03_DEPENDENCY_MAP.md)
- [04 目标架构与最小API](04_TARGET_ARCHITECTURE.md)
- [05 后端方案](05_BACKEND_REFACTOR_PLAN.md)
- [06 UI解耦方案](06_UNREAL_UI_DECOUPLING_PLAN.md)
- [07 分阶段路线图](07_REFACTOR_ROADMAP.md)
- [08 行为风险](08_BEHAVIOR_RISKS.md)
- [09 技术债与删除分类](09_TECH_DEBT.md)

## 5. 全量一方C++文本清单

用途：按模块/主类家族定位，不将文件名作为职责的唯一证据。主要类真实职责见01/05/06，编译引用见03。

| 文件 | 物理行数 | 资产类型 |
|---|---:|---|
| [ConsoleGCS/AppConfig.cpp](../../ConsoleGCS/AppConfig.cpp) | 551 | 生产实现/接口 |
| [ConsoleGCS/AppConfig.h](../../ConsoleGCS/AppConfig.h) | 80 | 生产实现/接口 |
| [ConsoleGCS/ArduPilotMissionWireAdapter.h](../../ConsoleGCS/ArduPilotMissionWireAdapter.h) | 61 | 生产实现/接口 |
| [ConsoleGCS/CommandDispatcher.cpp](../../ConsoleGCS/CommandDispatcher.cpp) | 850 | 生产实现/接口 |
| [ConsoleGCS/CommandDispatcher.h](../../ConsoleGCS/CommandDispatcher.h) | 280 | 生产实现/接口 |
| [ConsoleGCS/CommandManager.cpp](../../ConsoleGCS/CommandManager.cpp) | 356 | 生产实现/接口 |
| [ConsoleGCS/CommandManager.h](../../ConsoleGCS/CommandManager.h) | 93 | 生产实现/接口 |
| [ConsoleGCS/ConsoleArtifactConsumerV2.cpp](../../ConsoleGCS/ConsoleArtifactConsumerV2.cpp) | 1122 | 生产实现/接口 |
| [ConsoleGCS/ConsoleArtifactConsumerV2.h](../../ConsoleGCS/ConsoleArtifactConsumerV2.h) | 268 | 生产实现/接口 |
| [ConsoleGCS/ConsoleDSPGCSLoopbackTransport.cpp](../../ConsoleGCS/ConsoleDSPGCSLoopbackTransport.cpp) | 223 | 生产实现/接口 |
| [ConsoleGCS/ConsoleDSPGCSLoopbackTransport.h](../../ConsoleGCS/ConsoleDSPGCSLoopbackTransport.h) | 46 | 生产实现/接口 |
| [ConsoleGCS/ConsoleGCSApplication.cpp](../../ConsoleGCS/ConsoleGCSApplication.cpp) | 981 | 生产实现/接口 |
| [ConsoleGCS/ConsoleGCSApplication.h](../../ConsoleGCS/ConsoleGCSApplication.h) | 27 | 生产实现/接口 |
| [ConsoleGCS/ConsoleUI.cpp](../../ConsoleGCS/ConsoleUI.cpp) | 827 | 生产实现/接口 |
| [ConsoleGCS/ConsoleUI.h](../../ConsoleGCS/ConsoleUI.h) | 142 | 生产实现/接口 |
| [ConsoleGCS/ConsoleUploadJobAuthority.cpp](../../ConsoleGCS/ConsoleUploadJobAuthority.cpp) | 1640 | 生产实现/接口 |
| [ConsoleGCS/ConsoleUploadJobAuthority.h](../../ConsoleGCS/ConsoleUploadJobAuthority.h) | 277 | 生产实现/接口 |
| [ConsoleGCS/DSPGCSFrozenAssignmentReceiver.cpp](../../ConsoleGCS/DSPGCSFrozenAssignmentReceiver.cpp) | 264 | 生产实现/接口 |
| [ConsoleGCS/DSPGCSFrozenAssignmentReceiver.h](../../ConsoleGCS/DSPGCSFrozenAssignmentReceiver.h) | 62 | 生产实现/接口 |
| [ConsoleGCS/DSPGCSProductionCommandReceiver.cpp](../../ConsoleGCS/DSPGCSProductionCommandReceiver.cpp) | 567 | 生产实现/接口 |
| [ConsoleGCS/DSPGCSProductionCommandReceiver.h](../../ConsoleGCS/DSPGCSProductionCommandReceiver.h) | 73 | 生产实现/接口 |
| [ConsoleGCS/DSPGCSProductionTelemetryProvider.cpp](../../ConsoleGCS/DSPGCSProductionTelemetryProvider.cpp) | 448 | 生产实现/接口 |
| [ConsoleGCS/DSPGCSProductionTelemetryProvider.h](../../ConsoleGCS/DSPGCSProductionTelemetryProvider.h) | 67 | 生产实现/接口 |
| [ConsoleGCS/DSPGCSTransportConfigLoader.cpp](../../ConsoleGCS/DSPGCSTransportConfigLoader.cpp) | 196 | 生产实现/接口 |
| [ConsoleGCS/DSPGCSTransportConfigLoader.h](../../ConsoleGCS/DSPGCSTransportConfigLoader.h) | 16 | 生产实现/接口 |
| [ConsoleGCS/DSPGCSTransportFactory.cpp](../../ConsoleGCS/DSPGCSTransportFactory.cpp) | 51 | 生产实现/接口 |
| [ConsoleGCS/DSPGCSUploadJobReceiver.cpp](../../ConsoleGCS/DSPGCSUploadJobReceiver.cpp) | 145 | 生产实现/接口 |
| [ConsoleGCS/DSPGCSUploadJobReceiver.h](../../ConsoleGCS/DSPGCSUploadJobReceiver.h) | 53 | 生产实现/接口 |
| [ConsoleGCS/DataManager.cpp](../../ConsoleGCS/DataManager.cpp) | 57 | 生产实现/接口 |
| [ConsoleGCS/DataManager.h](../../ConsoleGCS/DataManager.h) | 57 | 生产实现/接口 |
| [ConsoleGCS/DroneCommandRequest.cpp](../../ConsoleGCS/DroneCommandRequest.cpp) | 65 | 生产实现/接口 |
| [ConsoleGCS/DroneCommandRequest.h](../../ConsoleGCS/DroneCommandRequest.h) | 99 | 生产实现/接口 |
| [ConsoleGCS/DroneDiscovery.h](../../ConsoleGCS/DroneDiscovery.h) | 46 | 生产实现/接口 |
| [ConsoleGCS/DroneManager.cpp](../../ConsoleGCS/DroneManager.cpp) | 301 | 生产实现/接口 |
| [ConsoleGCS/DroneManager.h](../../ConsoleGCS/DroneManager.h) | 60 | 生产实现/接口 |
| [ConsoleGCS/DroneSession.cpp](../../ConsoleGCS/DroneSession.cpp) | 1141 | 生产实现/接口 |
| [ConsoleGCS/DroneSession.h](../../ConsoleGCS/DroneSession.h) | 167 | 生产实现/接口 |
| [ConsoleGCS/FlightControl.cpp](../../ConsoleGCS/FlightControl.cpp) | 364 | 生产实现/接口 |
| [ConsoleGCS/FlightControl.h](../../ConsoleGCS/FlightControl.h) | 131 | 生产实现/接口 |
| [ConsoleGCS/FlightData.cpp](../../ConsoleGCS/FlightData.cpp) | 7 | 生产实现/接口 |
| [ConsoleGCS/FlightData.h](../../ConsoleGCS/FlightData.h) | 212 | 生产实现/接口 |
| [ConsoleGCS/FrozenAssignmentStore.cpp](../../ConsoleGCS/FrozenAssignmentStore.cpp) | 207 | 生产实现/接口 |
| [ConsoleGCS/FrozenAssignmentStore.h](../../ConsoleGCS/FrozenAssignmentStore.h) | 88 | 生产实现/接口 |
| [ConsoleGCS/FrozenAssignmentTransactionAssembler.cpp](../../ConsoleGCS/FrozenAssignmentTransactionAssembler.cpp) | 537 | 生产实现/接口 |
| [ConsoleGCS/FrozenAssignmentTransactionAssembler.h](../../ConsoleGCS/FrozenAssignmentTransactionAssembler.h) | 116 | 生产实现/接口 |
| [ConsoleGCS/HoverTest.cpp](../../ConsoleGCS/HoverTest.cpp) | 218 | 生产实现/接口 |
| [ConsoleGCS/HoverTest.h](../../ConsoleGCS/HoverTest.h) | 112 | 生产实现/接口 |
| [ConsoleGCS/MAVFTPMissionHardwareTest.cpp](../../ConsoleGCS/MAVFTPMissionHardwareTest.cpp) | 776 | 生产实现/接口 |
| [ConsoleGCS/MAVFTPMissionHardwareTest.h](../../ConsoleGCS/MAVFTPMissionHardwareTest.h) | 158 | 生产实现/接口 |
| [ConsoleGCS/MAVFTPMissionTransport.cpp](../../ConsoleGCS/MAVFTPMissionTransport.cpp) | 626 | 生产实现/接口 |
| [ConsoleGCS/MAVFTPMissionTransport.h](../../ConsoleGCS/MAVFTPMissionTransport.h) | 169 | 生产实现/接口 |
| [ConsoleGCS/ManagedBackendSession.h](../../ConsoleGCS/ManagedBackendSession.h) | 109 | 生产实现/接口 |
| [ConsoleGCS/MavFtpClient.cpp](../../ConsoleGCS/MavFtpClient.cpp) | 1039 | 生产实现/接口 |
| [ConsoleGCS/MavFtpClient.h](../../ConsoleGCS/MavFtpClient.h) | 299 | 生产实现/接口 |
| [ConsoleGCS/MavFtpProtocol.cpp](../../ConsoleGCS/MavFtpProtocol.cpp) | 235 | 生产实现/接口 |
| [ConsoleGCS/MavFtpProtocol.h](../../ConsoleGCS/MavFtpProtocol.h) | 83 | 生产实现/接口 |
| [ConsoleGCS/MavFtpWritePermitPool.h](../../ConsoleGCS/MavFtpWritePermitPool.h) | 148 | 生产实现/接口 |
| [ConsoleGCS/MavlinkCommand.cpp](../../ConsoleGCS/MavlinkCommand.cpp) | 600 | 生产实现/接口 |
| [ConsoleGCS/MavlinkCommand.h](../../ConsoleGCS/MavlinkCommand.h) | 201 | 生产实现/接口 |
| [ConsoleGCS/MavlinkDatagramSender.h](../../ConsoleGCS/MavlinkDatagramSender.h) | 25 | 生产实现/接口 |
| [ConsoleGCS/MavlinkDiscoveryService.cpp](../../ConsoleGCS/MavlinkDiscoveryService.cpp) | 78 | 生产实现/接口 |
| [ConsoleGCS/MavlinkDiscoveryService.h](../../ConsoleGCS/MavlinkDiscoveryService.h) | 54 | 生产实现/接口 |
| [ConsoleGCS/MavlinkParser.cpp](../../ConsoleGCS/MavlinkParser.cpp) | 394 | 生产实现/接口 |
| [ConsoleGCS/MavlinkParser.h](../../ConsoleGCS/MavlinkParser.h) | 41 | 生产实现/接口 |
| [ConsoleGCS/MavlinkReadOnlyIngress.cpp](../../ConsoleGCS/MavlinkReadOnlyIngress.cpp) | 116 | 生产实现/接口 |
| [ConsoleGCS/MavlinkReadOnlyIngress.h](../../ConsoleGCS/MavlinkReadOnlyIngress.h) | 54 | 生产实现/接口 |
| [ConsoleGCS/MavlinkUdpReceiver.cpp](../../ConsoleGCS/MavlinkUdpReceiver.cpp) | 219 | 生产实现/接口 |
| [ConsoleGCS/MavlinkUdpReceiver.h](../../ConsoleGCS/MavlinkUdpReceiver.h) | 69 | 生产实现/接口 |
| [ConsoleGCS/MissionFtpCompiler.cpp](../../ConsoleGCS/MissionFtpCompiler.cpp) | 228 | 生产实现/接口 |
| [ConsoleGCS/MissionFtpCompiler.h](../../ConsoleGCS/MissionFtpCompiler.h) | 139 | 生产实现/接口 |
| [ConsoleGCS/MissionPreparationWorker.cpp](../../ConsoleGCS/MissionPreparationWorker.cpp) | 304 | 生产实现/接口 |
| [ConsoleGCS/MissionPreparationWorker.h](../../ConsoleGCS/MissionPreparationWorker.h) | 90 | 生产实现/接口 |
| [ConsoleGCS/MissionProfileProbe.cpp](../../ConsoleGCS/MissionProfileProbe.cpp) | 618 | 生产实现/接口 |
| [ConsoleGCS/MissionProfileProbe.h](../../ConsoleGCS/MissionProfileProbe.h) | 138 | 生产实现/接口 |
| [ConsoleGCS/MissionTransport.h](../../ConsoleGCS/MissionTransport.h) | 77 | 生产实现/接口 |
| [ConsoleGCS/MissionUploadTransaction.cpp](../../ConsoleGCS/MissionUploadTransaction.cpp) | 533 | 生产实现/接口 |
| [ConsoleGCS/MissionUploadTransaction.h](../../ConsoleGCS/MissionUploadTransaction.h) | 174 | 生产实现/接口 |
| [ConsoleGCS/RouteUploadManager.cpp](../../ConsoleGCS/RouteUploadManager.cpp) | 1082 | 生产实现/接口 |
| [ConsoleGCS/RouteUploadManager.h](../../ConsoleGCS/RouteUploadManager.h) | 229 | 生产实现/接口 |
| [ConsoleGCS/SafetyConfig.h](../../ConsoleGCS/SafetyConfig.h) | 60 | 生产实现/接口 |
| [ConsoleGCS/UdpClient.cpp](../../ConsoleGCS/UdpClient.cpp) | 76 | 生产实现/接口 |
| [ConsoleGCS/UdpClient.h](../../ConsoleGCS/UdpClient.h) | 51 | 生产实现/接口 |
| [ConsoleGCS/acceptance/DSPGCSAcceptance.cpp](../../ConsoleGCS/acceptance/DSPGCSAcceptance.cpp) | 455 | Developer/Acceptance |
| [ConsoleGCS/acceptance/DSPGCSTransportFactory.cpp](../../ConsoleGCS/acceptance/DSPGCSTransportFactory.cpp) | 33 | Developer/Acceptance |
| [ConsoleGCS/acceptance/LoopbackDSPGCSTransport.cpp](../../ConsoleGCS/acceptance/LoopbackDSPGCSTransport.cpp) | 180 | Developer/Acceptance |
| [ConsoleGCS/acceptance/LoopbackDSPGCSTransport.h](../../ConsoleGCS/acceptance/LoopbackDSPGCSTransport.h) | 39 | Developer/Acceptance |
| [ConsoleGCS/main.cpp](../../ConsoleGCS/main.cpp) | 13 | 生产实现/接口 |
| [ConsoleGCS/tests/CommandTests.cpp](../../ConsoleGCS/tests/CommandTests.cpp) | 862 | 测试 |
| [ConsoleGCS/tests/ConsoleArtifactConsumerV2Tests.cpp](../../ConsoleGCS/tests/ConsoleArtifactConsumerV2Tests.cpp) | 750 | 测试 |
| [ConsoleGCS/tests/ConsolePipelineTests.cpp](../../ConsoleGCS/tests/ConsolePipelineTests.cpp) | 1910 | 测试 |
| [ConsoleGCS/tests/ConsoleRenderTests.cpp](../../ConsoleGCS/tests/ConsoleRenderTests.cpp) | 512 | 测试 |
| [ConsoleGCS/tests/DSPCommandTests.cpp](../../ConsoleGCS/tests/DSPCommandTests.cpp) | 623 | 测试 |
| [ConsoleGCS/tests/DSPFrozenAssignmentE2EServer.cpp](../../ConsoleGCS/tests/DSPFrozenAssignmentE2EServer.cpp) | 292 | 测试 |
| [ConsoleGCS/tests/DSPFrozenAssignmentProtocolTests.cpp](../../ConsoleGCS/tests/DSPFrozenAssignmentProtocolTests.cpp) | 513 | 测试 |
| [ConsoleGCS/tests/DSPFrozenAssignmentReceiverTests.cpp](../../ConsoleGCS/tests/DSPFrozenAssignmentReceiverTests.cpp) | 1106 | 测试 |
| [ConsoleGCS/tests/DSPUploadJobProtocolTests.cpp](../../ConsoleGCS/tests/DSPUploadJobProtocolTests.cpp) | 1570 | 测试 |
| [ConsoleGCS/tests/DroneLogicalIdTests.cpp](../../ConsoleGCS/tests/DroneLogicalIdTests.cpp) | 103 | 测试 |
| [ConsoleGCS/tests/MAVFTPMissionHardwareTestTests.cpp](../../ConsoleGCS/tests/MAVFTPMissionHardwareTestTests.cpp) | 346 | 测试 |
| [ConsoleGCS/tests/MAVFTPMissionWriterTests.cpp](../../ConsoleGCS/tests/MAVFTPMissionWriterTests.cpp) | 1229 | 测试 |
| [ConsoleGCS/tests/MavFtpProbeTests.cpp](../../ConsoleGCS/tests/MavFtpProbeTests.cpp) | 1425 | 测试 |
| [ConsoleGCS/tests/MissionFtpCompilerTests.cpp](../../ConsoleGCS/tests/MissionFtpCompilerTests.cpp) | 419 | 测试 |
| [ConsoleGCS/tests/MissionUploadTests.cpp](../../ConsoleGCS/tests/MissionUploadTests.cpp) | 1643 | 测试 |
| [ConsoleGCS/tests/RouteArtifactV2ContractTests.cpp](../../ConsoleGCS/tests/RouteArtifactV2ContractTests.cpp) | 266 | 测试 |
| [ConsoleGCS/tests/RouteMissionCodecTests.cpp](../../ConsoleGCS/tests/RouteMissionCodecTests.cpp) | 612 | 测试 |
| [Shared/DSPGCSProtocol/DSPGCSFrozenAssignmentProtocol.h](../../Shared/DSPGCSProtocol/DSPGCSFrozenAssignmentProtocol.h) | 1089 | Shared合同/算法 |
| [Shared/DSPGCSProtocol/DSPGCSMavFtpRuntimeConfigProtocol.h](../../Shared/DSPGCSProtocol/DSPGCSMavFtpRuntimeConfigProtocol.h) | 437 | Shared合同/算法 |
| [Shared/DSPGCSProtocol/DSPGCSProtocol.h](../../Shared/DSPGCSProtocol/DSPGCSProtocol.h) | 601 | Shared合同/算法 |
| [Shared/DSPGCSProtocol/DSPGCSUploadJobProtocol.h](../../Shared/DSPGCSProtocol/DSPGCSUploadJobProtocol.h) | 976 | Shared合同/算法 |
| [Shared/DSPGCSTransport/DSPGCSTransportConfig.h](../../Shared/DSPGCSTransport/DSPGCSTransportConfig.h) | 89 | Shared合同/算法 |
| [Shared/DSPGCSTransport/DSPGCSTransportFactory.h](../../Shared/DSPGCSTransport/DSPGCSTransportFactory.h) | 29 | Shared合同/算法 |
| [Shared/DSPGCSTransport/IDSPGCSTransport.h](../../Shared/DSPGCSTransport/IDSPGCSTransport.h) | 49 | Shared合同/算法 |
| [Shared/DSPGCSTransport/UDPDSPGCSTransport.h](../../Shared/DSPGCSTransport/UDPDSPGCSTransport.h) | 36 | Shared合同/算法 |
| [Shared/DroneIdentity/DroneLogicalId.h](../../Shared/DroneIdentity/DroneLogicalId.h) | 75 | Shared合同/算法 |
| [Shared/RouteArtifact/RouteArtifactContract.h](../../Shared/RouteArtifact/RouteArtifactContract.h) | 1080 | Shared合同/算法 |
| [Shared/RouteArtifact/RouteArtifactContractV2.h](../../Shared/RouteArtifact/RouteArtifactContractV2.h) | 426 | Shared合同/算法 |
| [Shared/RouteArtifact/RouteArtifactReadLease.h](../../Shared/RouteArtifact/RouteArtifactReadLease.h) | 135 | Shared合同/算法 |
| [Shared/RouteMission/RouteMissionCodec.h](../../Shared/RouteMission/RouteMissionCodec.h) | 490 | Shared合同/算法 |
| [Shared/RouteWaypoint/Wpl110Core.h](../../Shared/RouteWaypoint/Wpl110Core.h) | 605 | Shared合同/算法 |
| [Tools/Launcher/Launcher.cpp](../../Tools/Launcher/Launcher.cpp) | 491 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/DroneShowPlatform1.cpp](../../UnrealGCS/Source/DroneShowPlatform1/DroneShowPlatform1.cpp) | 146 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/DroneShowPlatform1.h](../../UnrealGCS/Source/DroneShowPlatform1/DroneShowPlatform1.h) | 12 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSFrozenAssignmentSyncSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSFrozenAssignmentSyncSubsystem.cpp) | 970 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSLoopbackTransport.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSLoopbackTransport.cpp) | 155 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSLoopbackTransport.h](../../UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSLoopbackTransport.h) | 44 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSProductionCommandProviderSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSProductionCommandProviderSubsystem.cpp) | 361 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSProductionTelemetryProviderSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSProductionTelemetryProviderSubsystem.cpp) | 731 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSTransportFactory.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSTransportFactory.cpp) | 48 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSUploadJobStateMirror.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSUploadJobStateMirror.cpp) | 237 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSUploadJobSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/DSPGCSUploadJobSubsystem.cpp) | 1137 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Developer/Acceptance/DSPGCSLoopbackProviderSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Developer/Acceptance/DSPGCSLoopbackProviderSubsystem.cpp) | 388 | Developer/Acceptance |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Developer/Mock/MockDroneDataSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Developer/Mock/MockDroneDataSubsystem.cpp) | 114 | Developer/Acceptance |
| [UnrealGCS/Source/DroneShowPlatform1/Private/DroneFleetVisualizationActor.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/DroneFleetVisualizationActor.cpp) | 1822 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/FleetManagerSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/FleetManagerSubsystem.cpp) | 345 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSBenchmarkExport.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSBenchmarkExport.cpp) | 97 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSFleetRowCheckBinding.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSFleetRowCheckBinding.cpp) | 208 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSFleetRowCheckBinding.h](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSFleetRowCheckBinding.h) | 54 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSGameMode.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSGameMode.cpp) | 10 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSInvisiblePawn.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSInvisiblePawn.cpp) | 79 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSRouteWorkflowCoordinator.h](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSRouteWorkflowCoordinator.h) | 626 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSRouteWorkflowCoordinator.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSRouteWorkflowCoordinator.inl) | 4054 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSServerAdapterSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSServerAdapterSubsystem.cpp) | 213 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSTelemetryFilterController.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSTelemetryFilterController.cpp) | 842 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSTelemetryFilterController.h](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSTelemetryFilterController.h) | 128 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSTelemetryFilterModel.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSTelemetryFilterModel.cpp) | 206 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSTelemetryFilterModel.h](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSTelemetryFilterModel.h) | 33 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSTelemetryFilterPresetSaveGame.h](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSTelemetryFilterPresetSaveGame.h) | 37 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSUIBridgeSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSUIBridgeSubsystem.cpp) | 7147 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSUIWidgetPresenter.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSUIWidgetPresenter.cpp) | 138 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/GCSUIWidgetPresenter.h](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSUIWidgetPresenter.h) | 35 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/HoverLayerSelectionModel.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/HoverLayerSelectionModel.cpp) | 173 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactFilesystemSafety.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactFilesystemSafety.cpp) | 225 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactFilesystemSafety.h](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactFilesystemSafety.h) | 24 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactManifest.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactManifest.cpp) | 249 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactManifestV2.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactManifestV2.cpp) | 240 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactSharedAdapter.h](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactSharedAdapter.h) | 73 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactSharedAdapterV2.h](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactSharedAdapterV2.h) | 29 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactTypes.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactTypes.cpp) | 512 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactTypesV2.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactTypesV2.cpp) | 54 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactWorkspaceManager.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactWorkspaceManager.cpp) | 2309 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactWorkspacePaths.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactWorkspacePaths.cpp) | 169 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactWorkspaceSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteArtifactWorkspaceSubsystem.cpp) | 331 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentConsoleCommands.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentConsoleCommands.cpp) | 116 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentGeo.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentGeo.cpp) | 303 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentMatcher.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentMatcher.cpp) | 1205 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentMaterializer.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentMaterializer.cpp) | 485 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentPipeline.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentPipeline.cpp) | 74 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteAssignmentSubsystem.cpp) | 1283 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteGpsCaptureSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteGpsCaptureSubsystem.cpp) | 805 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RoutePreviewConsoleCommands.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RoutePreviewConsoleCommands.cpp) | 277 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RoutePreviewNpzLoader.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RoutePreviewNpzLoader.cpp) | 660 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RoutePreviewNpzLoader.h](../../UnrealGCS/Source/DroneShowPlatform1/Private/RoutePreviewNpzLoader.h) | 12 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RoutePreviewSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RoutePreviewSubsystem.cpp) | 827 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteWaypointBatchGenerator.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteWaypointBatchGenerator.cpp) | 509 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteWaypointGenerationSubsystem.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteWaypointGenerationSubsystem.cpp) | 356 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteWaypointGenerator.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteWaypointGenerator.cpp) | 517 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/RouteWaypointValidator.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/RouteWaypointValidator.cpp) | 261 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSFrozenAssignmentProtocolTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSFrozenAssignmentProtocolTests.cpp) | 58 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSFrozenAssignmentSyncTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSFrozenAssignmentSyncTests.cpp) | 2158 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSTelemetryChunkTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSTelemetryChunkTests.cpp) | 134 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSTelemetryLinkTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSTelemetryLinkTests.cpp) | 137 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSUploadJobTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DSPGCSUploadJobTests.cpp) | 816 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DroneFleetVisualizationTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/DroneFleetVisualizationTests.cpp) | 509 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/FleetGeoOriginTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/FleetGeoOriginTests.cpp) | 164 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSBenchmarkExportTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSBenchmarkExportTests.cpp) | 55 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSFleetListContractTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSFleetListContractTests.cpp) | 107 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSGameModeTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSGameModeTests.cpp) | 129 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSIndependentToolbarTogglesTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSIndependentToolbarTogglesTests.cpp) | 169 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl) | 2153 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSScheduledShowPreviewTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSScheduledShowPreviewTests.cpp) | 95 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSSelectionStateTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSSelectionStateTests.cpp) | 439 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSTelemetryFilterTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSTelemetryFilterTests.cpp) | 609 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSUploadDashboardToggleTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSUploadDashboardToggleTests.cpp) | 463 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/HoverLayerSelectionModelTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/HoverLayerSelectionModelTests.cpp) | 96 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactDomainTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactDomainTests.cpp) | 340 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactManifestTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactManifestTests.cpp) | 616 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactRecoveryTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactRecoveryTests.cpp) | 633 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactSourceIdentityTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactSourceIdentityTests.cpp) | 161 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactV2WorkspaceTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactV2WorkspaceTests.cpp) | 713 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactWorkspaceTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteArtifactWorkspaceTests.cpp) | 611 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentAuthoritySessionTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentAuthoritySessionTests.cpp) | 169 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentGeoTests.cpp) | 340 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMatcherTests.cpp) | 2008 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMaterializerTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentMaterializerTests.cpp) | 801 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentPipelineTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteAssignmentPipelineTests.cpp) | 1021 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteGpsCaptureTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteGpsCaptureTests.cpp) | 599 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteMissionCodecTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteMissionCodecTests.cpp) | 97 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointBatchGeneratorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointBatchGeneratorTests.cpp) | 671 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointC2BBenchmarkTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointC2BBenchmarkTests.cpp) | 1307 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointGeneratorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointGeneratorTests.cpp) | 627 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointValidatorTests.cpp](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/RouteWaypointValidatorTests.cpp) | 373 | 测试 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSFrozenAssignmentSyncSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSFrozenAssignmentSyncSubsystem.h) | 197 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSProductionCommandProviderSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSProductionCommandProviderSubsystem.h) | 74 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSProductionCommandTypes.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSProductionCommandTypes.h) | 138 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSProductionTelemetryProviderSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSProductionTelemetryProviderSubsystem.h) | 194 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSUploadJobStateMirror.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSUploadJobStateMirror.h) | 76 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSUploadJobSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/DSPGCSUploadJobSubsystem.h) | 357 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/Developer/Acceptance/DSPGCSLoopbackProviderSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/Developer/Acceptance/DSPGCSLoopbackProviderSubsystem.h) | 77 | Developer/Acceptance |
| [UnrealGCS/Source/DroneShowPlatform1/Public/Developer/Mock/MockDroneDataSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/Developer/Mock/MockDroneDataSubsystem.h) | 40 | Developer/Acceptance |
| [UnrealGCS/Source/DroneShowPlatform1/Public/DroneData.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/DroneData.h) | 96 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/DroneFleetVisualizationActor.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/DroneFleetVisualizationActor.h) | 340 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/FleetManagerSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/FleetManagerSubsystem.h) | 167 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/GCSBenchmarkExport.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/GCSBenchmarkExport.h) | 28 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/GCSDataProvider.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/GCSDataProvider.h) | 28 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/GCSDataTypes.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/GCSDataTypes.h) | 151 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/GCSGameMode.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/GCSGameMode.h) | 17 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/GCSInvisiblePawn.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/GCSInvisiblePawn.h) | 30 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/GCSServerAdapterSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/GCSServerAdapterSubsystem.h) | 56 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/GCSUIBridgeSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/GCSUIBridgeSubsystem.h) | 707 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/HoverLayerSelectionModel.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/HoverLayerSelectionModel.h) | 38 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactManifest.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactManifest.h) | 103 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactManifestV2.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactManifestV2.h) | 61 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactTypes.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactTypes.h) | 149 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactTypesV2.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactTypesV2.h) | 42 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactWorkspaceManager.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactWorkspaceManager.h) | 257 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactWorkspacePaths.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactWorkspacePaths.h) | 50 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactWorkspaceSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteArtifactWorkspaceSubsystem.h) | 79 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentContract.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentContract.h) | 7 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentGeo.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentGeo.h) | 85 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentMatcher.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentMatcher.h) | 199 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentMaterializer.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentMaterializer.h) | 20 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentPipeline.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentPipeline.h) | 16 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentSubsystem.h) | 78 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentTypes.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteAssignmentTypes.h) | 415 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteGpsCaptureSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteGpsCaptureSubsystem.h) | 289 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RoutePreviewData.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RoutePreviewData.h) | 88 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RoutePreviewSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RoutePreviewSubsystem.h) | 169 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteWaypointBatchGenerator.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteWaypointBatchGenerator.h) | 144 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteWaypointGenerationSubsystem.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteWaypointGenerationSubsystem.h) | 61 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteWaypointGenerator.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteWaypointGenerator.h) | 44 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteWaypointGeneratorTypes.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteWaypointGeneratorTypes.h) | 138 | 生产实现/接口 |
| [UnrealGCS/Source/DroneShowPlatform1/Public/RouteWaypointValidator.h](../../UnrealGCS/Source/DroneShowPlatform1/Public/RouteWaypointValidator.h) | 74 | 生产实现/接口 |

## 6. 构建/配置/工具文本入口

- [ConsoleGCS/ConsoleGCSTests.vcxproj](../../ConsoleGCS/ConsoleGCSTests.vcxproj)
- [ConsoleGCS/DSPGCSAcceptance.vcxproj](../../ConsoleGCS/DSPGCSAcceptance.vcxproj)
- [ConsoleGCS/DroneGCS.sln](../../ConsoleGCS/DroneGCS.sln)
- [ConsoleGCS/DroneGCS.vcxproj](../../ConsoleGCS/DroneGCS.vcxproj)
- [ConsoleGCS/acceptance/config/ReservedUDPTransport.ini](../../ConsoleGCS/acceptance/config/ReservedUDPTransport.ini)
- [ConsoleGCS/config/DSPGCSTransport.ini](../../ConsoleGCS/config/DSPGCSTransport.ini)
- [ConsoleGCS/gcs-config-probe.ini](../../ConsoleGCS/gcs-config-probe.ini)
- [ConsoleGCS/gcs-config-upload.ini](../../ConsoleGCS/gcs-config-upload.ini)
- [Tools/Installer/BuildInstaller.ps1](../../Tools/Installer/BuildInstaller.ps1)
- [Tools/Installer/DroneGCS.nsi](../../Tools/Installer/DroneGCS.nsi)
- [Tools/Launcher/Deploy.ps1](../../Tools/Launcher/Deploy.ps1)
- [Tools/Launcher/DroneGCSLauncher.vcxproj](../../Tools/Launcher/DroneGCSLauncher.vcxproj)
- [Tools/Launcher/PreparePayload.ps1](../../Tools/Launcher/PreparePayload.ps1)
- [Tools/Launcher/config/DSPGCSTransport.ini](../../Tools/Launcher/config/DSPGCSTransport.ini)
- [Tools/Launcher/config/production.ini](../../Tools/Launcher/config/production.ini)
- [Tools/Launcher/tests/ConsoleStop.cs](../../Tools/Launcher/tests/ConsoleStop.cs)
- [Tools/Launcher/tests/DSPGCSTransport.ini](../../Tools/Launcher/tests/DSPGCSTransport.ini)
- [Tools/Launcher/tests/FakeFrontend.cs](../../Tools/Launcher/tests/FakeFrontend.cs)
- [Tools/Launcher/tests/FakeOwner.cs](../../Tools/Launcher/tests/FakeOwner.cs)
- [Tools/Launcher/tests/Smoke.ps1](../../Tools/Launcher/tests/Smoke.ps1)
- [UnrealGCS/Config/DefaultEditor.ini](../../UnrealGCS/Config/DefaultEditor.ini)
- [UnrealGCS/Config/DefaultEngine.ini](../../UnrealGCS/Config/DefaultEngine.ini)
- [UnrealGCS/Config/DefaultGame.ini](../../UnrealGCS/Config/DefaultGame.ini)
- [UnrealGCS/Config/DefaultInput.ini](../../UnrealGCS/Config/DefaultInput.ini)
- [UnrealGCS/DroneShowPlatform1.sln](../../UnrealGCS/DroneShowPlatform1.sln)
- [UnrealGCS/DroneShowPlatform1.uproject](../../UnrealGCS/DroneShowPlatform1.uproject)
- [UnrealGCS/Source/DroneShowPlatform1.Target.cs](../../UnrealGCS/Source/DroneShowPlatform1.Target.cs)
- [UnrealGCS/Source/DroneShowPlatform1/DroneShowPlatform1.Build.cs](../../UnrealGCS/Source/DroneShowPlatform1/DroneShowPlatform1.Build.cs)
- [UnrealGCS/Source/DroneShowPlatform1Editor.Target.cs](../../UnrealGCS/Source/DroneShowPlatform1Editor.Target.cs)

## 7. 本轮交付检查

十份文档齐全；逐阶段12项施工字段完整；相对Markdown文件链接存在；git diff --check通过。变更限定本目录十份新文档及交接、开发状态、测试架构、文档索引四份已有文档。未修改源码/构建配置/协议/资源，未提交Git，未执行Phase0。此处通过仅指文档检查，不是软件回归PASS。
