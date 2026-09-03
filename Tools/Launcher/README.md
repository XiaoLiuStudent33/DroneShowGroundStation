# DroneGCS 联合软件包与正式安装支持

Status: CURRENT SUPPORTING
Last Updated: 2026-09-03

一个软件包，UE 前端 + 可见 Console 后端。运行编译后的 EXE，不读取开发电脑源码，不需要 PowerShell、Visual Studio 或 UE 编辑器。

本文件说明联合 payload 和便携 `Windows` 目录。正式 NSIS 安装程序已经实现，仓库维护者可查看 `Tools/Installer/README.md` 的构建、分发和验收说明。便携目录“不是安装器”不代表项目没有安装版。

## 联合启动入口

打包后双击 Windows/DroneShowPlatform1/DroneGCSLauncher.exe，可为它创建桌面快捷方式。UE 根目录原有 DroneShowPlatform1.exe 仍是纯前端入口。

交付整个 Windows 文件夹，不是只交付启动器。目录布局：

- Windows/Engine：UE 运行依赖。
- Windows/DroneShowPlatform1/DroneGCSLauncher.exe：联合启动入口。
- Windows/DroneShowPlatform1/Content/Paks：UE 内容。
- Windows/DroneShowPlatform1/Binaries/Win64：UE Game EXE。
- Windows/DroneShowPlatform1/Binaries/Win64/Backend：Console EXE、生产配置、DSP 配置、三个 VC143 CRT DLL、payload-manifest.json。

不带 Console 源码、测试程序、VS 工程、个人配置。该 `Windows` 目录本身是可分发便携包；正式用户可以改用 NSIS 单文件安装程序。

## 启动与退出

1. 启动器检查文件、配置、已有进程和端口。
2. 打开可见 Console 窗口，保留机群状态和日志面板。
3. 收到真实后端就绪信号后打开 UE，不用固定延时猜测。
4. 控制模式下关闭/崩溃 UE 或启动器，后端继续运行，不自动杀进程或重启。
5. 确认无人机和上传任务均已停止、无需继续控制后，在后端按 Ctrl+C 正常释放资源退出。不要在飞行/上传中关闭后端窗口。

已有 UE/Console 时再次联合启动会拒绝，不接管未知实例。关闭前端后如果后端仍在，不要重复启动后端。待安全停机退出后，再重新联合启动。

便携目录（没有 `installed-layout.marker`）继续读取包内配置。正式安装后，第一次启动会把配置模板复制到 `%LOCALAPPDATA%/DroneGCS/Config`；之后升级不覆盖该配置。

正式安装后的运行数据位置：

- 联合启动日志、后端日志、航线上传日志：`%LOCALAPPDATA%/DroneGCS/Logs/<时间-PID>`。
- UE Saved/日志/缓存：`%LOCALAPPDATA%/DroneGCS/UE`。
- UE 与 Console 共享航线工作区：`%LOCALAPPDATA%/DroneGCS/ArtifactWorkspace`。

如果当前用户目录不可写，启动器只对日志回退到启动器旁 `Logs`；安装在 Program Files 时通常也不可写，因此会给出明确错误。后端日志不依靠重定向 stdout，仍保留可见面板。

## 网络与控制

- 飞控网络保持 192.168.1.254:10001。连接无人机网络的电脑网卡必须具备此 IP；启动器会预检并给出明确错误，但不会自动修改网卡。正式安装器创建限定到后端 EXE、该本地 IP 和 UDP 端口的入站防火墙规则。
- 同机 UE↔Console 固定 127.0.0.1:12001 ↔ 127.0.0.1:12002，与飞控地址不是一回事。
- 交付模板启用 real_command_enabled、dspgcs_command_enabled、route_upload_enabled。
- safe_test_mode=true 按当前生产代码保留；真实发送由上述生产开关决定，不是旧版只读锁。目标选择、5 米限制、ACK、上传校验等业务保护不变。
- 航线文件默认共用当前用户 %LOCALAPPDATA%/DroneGCS/ArtifactWorkspace。联合启动器把同一路径传给两端，不再使用开发者 Documents 路径。
- 如配置明确指定 artifact_workspace_root，联合启动器把该路径传给 UE。UE 显式工作目录设置仍优先，不能另选不一致目录。
- 开启开关不代表真机功能验收通过。本步骤不执行任何真机命令。

## 开发机：UE 打包流程接入

在 UE 界面正常进行 Windows Game 构建 + 打包，不要跳过构建。

- Game Target 的 PreBuildSteps 调用 PreparePayload.ps1，编译 Release x64 Console 和启动器。
- 后端编译失败使构建失败，不静默继续使用旧后端。
- Module Build.cs 通过 RuntimeDependencies / NonUFS 暂存 EXE、CRT 和配置，不放入 PAK/IoStore。
- 不需要在“要烘焙的额外资产目录”添加 Console 源码。
- Editor/PIE 不自动构建或启动后端。
- 首次修改构建规则后重新打开项目；打包设置“编译”选“始终”，界面由操作员完成。
- 每次打包选新空目录，不覆盖旧测试包或已经通过验收的暂存包。
- 后端所需 VC143 CRT 随包；UE 先决条件和系统/显卡要求仍需满足。正式安装器脚本位于 `Tools/Installer`，网卡 IP 仍由操作员设置。

仅编译 payload 的开发校验入口：Tools/Launcher/PreparePayload.ps1。不会运行后端、Cook 或打 UE 包。

## 校验

启动器 --check 只预检，退出码 0 表示通过，不启动子进程、不发包。
--no-dialogs 是自动化测试模式，会实际启动程序，仅错误提示不弹框。
tests/Smoke.ps1 使用独立回环网络和假前端，不代表真机/完整 UE 验收。

旧 Deploy.ps1 是历史目录部署工具，不用于新版 UE 随包流程，也不应对保留测试包运行。

正式发布只接受 Shipping 暂存包。安装程序生成、干净电脑验收和卸载流程见 `Tools/Installer/README.md`。
