# DroneGCS 联合软件包 V1 / V2 历史验证

Status: ARCHIVED / HISTORICAL
Current Authority: [../../Tools/Launcher/VALIDATION.md](../../Tools/Launcher/VALIDATION.md)
Do Not Use As Current Implementation State
Last Updated: 2026-09-03 (archived)

本文件保存第一版只读联合启动器和第二版完整控制便携包的历史验证证据。当前 Shipping 与 NSIS 安装程序状态必须查看上方 Current Authority。

## 1. V2 完整控制便携包

V2 的目标是把 UE 前端和可见 Console 后端放入同一便携 `Windows` 目录，不包含源码，也不要求 PowerShell、Visual Studio 或 UE 编辑器。

已实现：

- Game Target 后端构建 hook 与显式 NonUFS runtime dependencies。
- 可见 Console Dashboard 和文件日志。
- 完整控制交付模板以及共享的当前用户 Artifact Workspace。
- 控制模式下 UE/Launcher 退出后 Console 继续运行。
- Console Ctrl+C 正常释放资源和端口。

自动化证据：

- Console Release x64 与联合启动器构建通过；Launcher 使用 `/W4 /WX` 和静态 CRT。
- UE 5.5 Win64 Development Game compile 通过，UBT receipt 中 9 个联合 NonUFS dependencies 全部存在。
- payload allow-list 包含 Console/Launcher、两个配置、README 和三个 VC143 CRT DLL；不包含源码通配符。
- 隔离回环 Smoke 最终 30 项通过；飞控侧 fixture 仅使用 `127.0.0.1:10091`，未向真机发送命令。
- 覆盖 full-control flags、telemetry、共享工作区、重复启动拒绝、UE/Launcher 退出后端存活、Ctrl+C 返回 0 和无残留 fixture 进程。
- Console `--console-render-tests` 与 `--runtime-config-tests` 通过。

V2 属于 Development 便携测试包，保留用于回归，不是正式安装程序。其配置在打包后曾按测试要求调整，后续正式 Shipping 校验不允许同类 payload 哈希漂移。

## 2. V1 只读联合启动器

V1 的范围是为既有 Development Windows 包增加原生 GUI Launcher 和受管只读 Console 生命周期，不修改 UE 资源，不开放命令和航线上传。

自动化证据：

- Console Release x64 构建成功；Launcher `/W4 /WX` 构建成功。
- `tests/Smoke.ps1` 19 项通过，使用真实 Console、接收型假前端和 `127.0.0.1:10091` 回环 MAVLink fixture。
- 覆盖中文/空格路径、端口冲突拒绝、重复 Launcher 拒绝、DSP snapshot、前端退出后后端清理、无效 token 与 owner 退出。
- Managed backend 会在打开事件和网络前拒绝 command-enabled 配置。

历史人工启动观察：

- Launcher 启动只读后端并等待 Ready 后启动实际 UE Development Game。
- 后端占用 `192.168.1.254:10001` 与 `127.0.0.1:12001`，UE 占用 `127.0.0.1:12002`。
- UE 日志观察到 DSP-GCS telemetry link Connected。

V1 的只读、前端退出自动清理和无可见 Console 行为已被 V2/V3 完整控制生命周期取代，不得作为当前发布操作说明。

## 3. 历史边界

- V1/V2 证据不代表当前 V1.0.0 安装程序的安装、升级或卸载结果。
- 回环 Smoke 不证明真实飞控命令、飞行或 Mission Upload。
- 当前证据和待验收项见 [当前联合包验证](../../Tools/Launcher/VALIDATION.md)。
