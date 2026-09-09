# Phase 0：Git 与构建基线

基线日期2026-09-09；master HEAD `a95309d8b37ecb00c779d5c4264aacb86294d8ec`（9/9优化2）。Phase 0已经执行，不进入Phase1。

## 1. Git与范围

开始执行了 `git status`、`git diff`、`git log --oneline -20`；只有上一阶段4份Docs更新和未跟踪Docs/Architecture，没有预期外源码修改。未提交、未stage、未覆盖/回滚用户内容。完整输出保存在 `BuildLogs/ArchitecturePhase0-20260909/git-*-before.txt`及`git-log.txt`。最终变化仍全部位于Docs。

未修改生产源码、测试源码、.uasset、.umap、生产ini、协议、超时、重试或命令语义。新增只读hash比较helper以及文档/证据。构建/测试产物位于既有ignored的BuildLogs、Saved、Binaries、Intermediate、x64目录。没有Shipping、真实Mission upload或硬件动作。

## 2. 工具链与真实构建来源

依据ConsoleGCS/DroneGCS.sln、三个vcxproj、UE Build.cs/Editor.Target.cs、Launcher vcxproj及现有文档确定命令。Console solution包含DroneGCS、ConsoleGCSTests、DSPGCSAcceptance。Shared为两端include编译的header-only合同/算法，无独立CMake构建；Launcher复用既有Console配置解析源码。

VS2022 Community；MSBuild位于 `C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe`。v143 C++17；MSVC安装目录14.44.35207，UBT报告compiler14.44.35228，Windows SDK10.0.22621.0。UE安装根 `D:/EpicDownLoad/EPICDefault/UE_5.5`，Build.version确认5.5.4、CL40574608，bundled .NET8.0.300。测试机器Windows11、i7-14700HX、16GB。

## 3. 实际命令

仓库根为 `C:/Users/13092/Documents/DroneGCS`。以下五项逐配置运行并检查退出码；Console使用完整Rebuild，UE使用实际Editor Build（增量，未声称Clean Rebuild）。

```powershell
$ms = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
& $ms ConsoleGCS/DroneGCS.sln /t:Rebuild /m /p:Configuration=Release /p:Platform=x64
& $ms ConsoleGCS/DroneGCS.sln /t:Rebuild /m /p:Configuration=Debug /p:Platform=x64
& $ms Tools/Launcher/DroneGCSLauncher.vcxproj /t:Rebuild /m /p:Configuration=Release /p:Platform=x64
$engine = 'D:\EpicDownLoad\EPICDefault\UE_5.5'
& "$engine/Engine/Build/BatchFiles/Build.bat" DroneShowPlatform1Editor Win64 Development '-Project=C:\Users\13092\Documents\DroneGCS\UnrealGCS\DroneShowPlatform1.uproject' -WaitMutex
& "$engine/Engine/Build/BatchFiles/Build.bat" DroneShowPlatform1Editor Win64 DebugGame '-Project=C:\Users\13092\Documents\DroneGCS\UnrealGCS\DroneShowPlatform1.uproject' -WaitMutex
```

## 4. 构建结果

| Target/配置 | 结果/exit | 实测墙钟秒 | Warning / Error |
|---|---|---:|---|
| Console-Release | PASS / 0 | 241.456 | 500 warnings，0 errors；C4819/C4244 |
| Console-Debug | PASS / 0 | 66.193 | 500 warnings，0 errors；C4819/C4244 |
| Launcher-Release | PASS / 0 | 4.490 | 0 warnings，0 errors |
| UE-Development | PASS / 0 | 43.866 | 非preferred MSVC警告；0编译错误 |
| UE-DebugGame | PASS / 0 | 18.050 | 非preferred MSVC警告；0编译错误 |

Console两配置的MSBuild汇总分别500 warnings（不是500种不同问题），主要既有CP936/C4819编码警告、generated MAVLink和Parser的C4244窄化。未通过改业务代码消警。Launcher /W4 /WX通过。UE Development执行5 actions，DebugGame10 actions；MSVC14.44不在UE推荐14.38范围，记录为工具链兼容风险，未换工具链。

原始日志：`BuildLogs/ArchitecturePhase0-20260909/console-Release.log`、`console-Debug.log`、`launcher.log`、`ue-Development.log`、`ue-DebugGame.log`。墙钟含进程/资源等待，不能用于硬实时或公平性能比较。阶段证据汇总保存在 [PHASE0_EVIDENCE.json](PHASE0_EVIDENCE.json)。

UE启动日志另有Cesium `FCesiumMetadataPropertyStatisticValue::Semantic`未初始化错误；首轮默认地图还报一个ExternalActor依赖不存在。这些不是C++构建失败，不在本轮修复，不随测试通过而抹去。真实打包/资源加载仍需后续发布验证。
