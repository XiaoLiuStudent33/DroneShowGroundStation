# DroneGCS 联合包与安装程序验收

Status: CURRENT EVIDENCE
Last Updated: 2026-09-03
Baseline: `master` / `03825841f81e516731d12df632867be67f72ad57` + 2026-09-03 联合打包工作树

本文件只保留当前 V3 Shipping / V1.0.0 Installer 证据。V1 只读版和 V2 完整控制便携版的历史记录已移至 [Joint_Package_V1_V2_Validation_CN.md](../../Docs/History/Joint_Package_V1_V2_Validation_CN.md)。

## 1. 当前实现

- Game Target PreBuildSteps 编译 Release x64 Console 与联合启动器，编译失败会阻断 UE 打包。
- RuntimeDependencies / NonUFS 仅暂存严格 allow-list payload，不含源码、测试程序、PDB 或个人配置。
- 联合启动器检查文件、manifest、配置、已有进程、端口和本机 `192.168.1.254`，打开可见完整控制 Console，等待 Ready 后再启动 UE。
- 安装标记启用每用户数据目录；模板配置只在首次运行复制，升级不覆盖。
- Console 使用联合启动器提供的绝对配置路径，不依赖当前目录或开发机路径。
- 联合日志、后端日志、航线上传日志、UE Saved 和 Artifact Workspace 位于 `%LOCALAPPDATA%/DroneGCS`。
- 完整控制模式下 UE/Launcher 退出不自动杀死 Console；确认任务安全停止后由操作员按 Ctrl+C 正常退出后端。
- NSIS 安装器只接受完整 Shipping 输入，创建联合入口快捷方式、运行 UE 先决条件并管理精确 MAVLink UDP 入站规则。

## 2. 自动化与本机构建证据

- Console Release x64 与联合启动器重新编译成功；payload manifest 重新生成。
- Console Render 与 Runtime Config 专项测试通过。
- 隔离回环 Smoke 共 32 项通过，覆盖中文/空格路径、安装标记、首次配置复制、每用户目录、Shipping 布局、可见完整控制后端、Ctrl+C 正常退出及无残留进程。
- 真实 `D:/DroneGCS_JointPackage_V3` Shipping 输入通过文件、单 Game EXE、源码排除、payload allow-list 和哈希校验。
- UAT 仅将两个 INI 的 LF 规范化为 CRLF；规范化内容哈希一致，所有二进制仍逐字节校验。
- V3 `DroneGCSLauncher.exe --check` 返回 0，未启动子进程、未发送数据包。
- NSIS 3.12 无警告生成安装程序和 SHA-256 文件。

```text
Installer: DroneGCS-Setup-1.0.0-x64.exe
Size: 297422854 bytes
Version resource: 1.0.0.0
SHA-256: b236c8a32708c686999009d36909b81ea5034b0b852b6610ac8f1bde1874b99f
Code signing: NotSigned
```

## 3. 用户确认的异机证据

2026-09-03，用户确认安装程序已转移到另一台没有项目源码的电脑，安装与启动测试通过：

```text
Second-PC install and launch: USER-CONFIRMED PASS
Repository-side second-PC logs: NONE
```

该证据证明第一版安装交付链在另一台电脑可用，但不自动证明覆盖升级、卸载、全部非飞行控制、受监督飞行、航线上传或任意固件行为均已通过。

## 4. 尚待分项验收

- 异机非飞行控制闭环：UE 提交、Console 发送、飞控响应、UE 结果。
- 现场安全条件满足后的受监督飞行和航线上传。
- 覆盖升级保留 `%LOCALAPPDATA%/DroneGCS/Config`。
- 卸载移除程序、快捷方式和防火墙规则，同时保留用户业务数据。
- 正式发布代码签名、时间戳和版本归档。
