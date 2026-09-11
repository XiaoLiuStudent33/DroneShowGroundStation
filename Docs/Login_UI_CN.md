# 本机登录与场景入口

本次新增独立登录关卡 `/Game/Maps/L_GCS_Login`，作为 GameDefaultMap；EditorStartupMap 仍保留现有地面站地图。直接在主地图 PIE 时，也会先显示登录入口。

## 使用

1. 打开项目，打开 `Content/Maps/L_GCS_Login` 并运行，或使用项目的独立游戏启动方式。
2. 首次使用创建本机账号：账号 3–32 位，仅英文字母、数字、下划线、短横线和点；密码 12–128 位，确认输入一致。
3. 后续使用该账号登录。校验在工作线程执行，成功后通过 OpenLevel 进入 `/Game/Maps/DroneShowPlatform1`，恢复既有主界面及其绑定。
4. Tab 切换输入位置，Enter 提交。提交期间禁止重复操作。退出应用后，下次启动重新登录。

Development/Debug 开发版提供特殊测试账号 **123 / 123**，在首次设置页或登录页均可直接提交，确认密码可留空。此入口不创建或覆盖本机账号文件，Shipping/Test 配置不编译该入口及提示。正式使用仍需自行设置账号；正式账号的密码规则不变。自动化测试使用 Saved/Automation 下的独立临时文件，不设置或覆盖真实账号。

## 实现边界

- `GCSLoginWidget` 是 C++ UUserWidget，以 Slate 构建深色双栏表单。当前布局在源码中调整，不是 UMG 设计器内可拖拽编辑的 WidgetTree。
- `GCSLoginSubsystem` 以 GameInstanceSubsystem 保存本次登录状态，跨地图切换有效。
- 本机每个 Windows 用户保存一个工作站账号；文件位置由 `FPlatformProcess::UserSettingsDir()/DroneGCS/Auth/account-v1.dat` 决定。使用随机 16 字节盐、PBKDF2-HMAC-SHA256（600000 次）和 32 字节派生值，不保存明文密码。
- 账号文件损坏时拒绝登录，不自动重建或覆盖。失败后有短暂重试冷却；本次不包含找回密码、多账号管理、云端认证或角色权限。
- 这是本机应用入口，并非 ConsoleGCS 命令授权边界。可修改本机文件或运行其他客户端的用户仍可能直接访问后端。商业权限体系需另行接入服务端身份、会话与命令授权。
- 登录地图采用 GameModeBase，主地图继续使用现有 GCSGameMode。地图已加入 MapsToCook；此轮不代表 Shipping 安装包已重建。
- 登录请求异步，OpenLevel 本身可能阻塞；页面会先提示正在加载，但不承诺加载期间动画或精确进度。本次未引入关卡流送，也未改变现有运行中控制或后台服务生命周期。

## 开发验证

- `DroneGCS.Login.Credentials`：创建、长度与确认校验、错误账号/密码、无明文持久化、损坏文件拒绝。
- 开发版 `-game /Game/Maps/L_GCS_Login -ExecCmds="GCS.Login.Smoke"`：使用隔离临时账号，通过真实 Widget Submit、密码校验线程和 OpenLevel，检查同一 GameInstance 登录状态保留、主界面出现、登录界面移除，然后退出。结果为 `Saved/Automation/Login-smoke-result.txt`。该命令及测试路径覆盖仅在 WITH_DEV_AUTOMATION_TESTS 编译时存在。
- 新增地图生成脚本为 `UnrealGCS/Scripts/create_login_map.py`，已存在地图时不会覆盖。

排版参考：[IBM Carbon 登录模式](https://carbondesignsystem.com/patterns/login-pattern/)。未下载第三方图片或字体。

## 本轮结果（2026-09-11）

- UE Editor Development 编译通过，日志 `BuildLogs/Login-build.log`。
- 账号测试及原有仿真 Commands、FlightLifecycle、LoopbackAndLifecycle 共 4 项通过，0 失败；报告 `BuildLogs/LoginTests/index.json`。
- 真实渲染模式的跨关卡测试通过；`Saved/Automation/Login-smoke-result.txt` 记录结果，`Saved/Automation/Login-station.png` 记录主场景。场景初始化后复位环绕视角，解决刚进入时贴近地面的视角问题。
- 登录页已检查真实运行画面，修正输入框与按钮对比度、初始焦点。未执行真机操作，未重建 Shipping/安装包。
