# DroneGCS 正式安装包

Status: CURRENT SUPPORTING
Last Updated: 2026-09-03

本目录使用 NSIS，把已经完成联合打包的 UE `Windows` 目录制作成一个 Windows x64 安装程序。安装内容只有编译后的 UE、Console 后端、运行库和配置模板，不包含 Console/UE 源码，也不依赖 Visual Studio、UE 编辑器或 PowerShell。

## 1. 当前发布结果

第一版安装程序已经生成：

```text
DroneGCS-Setup-1.0.0-x64.exe
Size: 297422854 bytes
SHA-256: b236c8a32708c686999009d36909b81ea5034b0b852b6610ac8f1bde1874b99f
Code signing: NotSigned
```

2026-09-03，用户确认该安装程序已转移到另一台没有项目源码的电脑，安装与启动测试通过。证据等级为 `USER-CONFIRMED PASS`，仓库没有对应异机自动化日志；不得据此扩大声称所有飞行、航线上传、升级和卸载行为均已验收。

## 2. 分发文件

- 安装时只需要 `DroneGCS-Setup-1.0.0-x64.exe`。
- 同名 `.sha256` 文件不参与安装，只用于校验复制或下载后的安装程序是否完整。
- 正式交付建议同时提供 EXE 与 `.sha256`；普通内部安装可以只提供 EXE。
- 安装完成后可以删除下载的安装程序和 `.sha256`，不影响已安装软件运行。
- 当前安装程序没有数字签名，Windows 可能显示“未知发布者”或 SmartScreen 提示；正式对外发布前应配置代码签名。

## 3. 生成 Shipping 暂存包

操作员在 UE 中选择 **Shipping**，打包到新的空目录。真正的便携联合入口为：

```text
<PackageRoot>\Windows\DroneShowPlatform1\DroneGCSLauncher.exe
```

不要覆盖已保留的历史测试包，也不要把 UE 根目录纯前端 Game EXE 当作联合入口。

## 4. 校验并生成安装程序

安装 NSIS 3 后，在 PowerShell 中从任意目录执行：

```powershell
$RepoRoot = "C:\path\to\DroneGCS"
$PackageRoot = "D:\path\to\DroneGCS_Shipping"
& "$RepoRoot\Tools\Installer\BuildInstaller.ps1" `
  -PackageRoot $PackageRoot `
  -Version 1.0.0
```

默认输出目录是 `Tools/Installer/Output`。同时生成安装程序和 `.sha256` 校验文件。

构建脚本会拒绝：

- 缺少 Engine、Paks、联合启动器、Console、配置或运行库。
- 同时混入 Shipping 与 Development Game EXE。
- 正式构建不是 Shipping。
- payload manifest 不完整或二进制哈希不一致。
- 暂存目录包含源码或开发工程文件。

`-ValidateOnly` 只验证输入，不创建安装程序；`-AllowDevelopment` 只能与 `-ValidateOnly` 一起用于历史 Development 包结构检查，不得生成正式安装包。

## 5. 安装后的边界

- 默认安装到 `C:\Program Files\DroneGCS`，桌面和开始菜单快捷方式只指向联合启动器。
- 后端窗口保持可见；联合启动器先等待 Console Ready，再启动 UE 前端。
- 初次运行把配置模板复制到 `%LOCALAPPDATA%\DroneGCS\Config`，以后升级不覆盖用户配置。
- 日志写入 `%LOCALAPPDATA%\DroneGCS\Logs`，UE Saved 数据写入 `%LOCALAPPDATA%\DroneGCS\UE`。
- 航线共享工作区默认位于 `%LOCALAPPDATA%\DroneGCS\ArtifactWorkspace`。
- 安装器运行 UE 先决条件，并为后端创建仅限其 EXE、`192.168.1.254:10001/UDP` 的专用入站防火墙规则。
- 启动器只检查本机是否已有 `192.168.1.254`，不会修改网卡；网卡地址仍由操作员配置。
- 卸载移除程序、快捷方式和专用防火墙规则，保留用户配置、日志和航线数据。

## 6. 尚待分项验收

1. 异机遥测、选择、灯光等非飞行闭环：UE 提交、Console 发送、飞控响应、UE 结果。
2. 现场安全条件满足后的受监督飞行与航线上传。
3. 覆盖升级是否保留 `%LOCALAPPDATA%\DroneGCS\Config`。
4. 卸载是否移除程序、快捷方式和防火墙规则，同时保留用户业务数据。
5. 正式发布证书、时间戳签名、版本归档和安装包哈希发布流程。

确认无人机和上传任务均已停止后，应先在可见 Console 后端按 Ctrl+C 正常退出，再关闭 UE；不得在飞行或上传过程中直接结束后端。
