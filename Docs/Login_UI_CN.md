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

## 2026-09-14 登录页改版（当前）

`L_GCS_Login` 继续复用原生 `UGCSLoginWidget` 入口，因此编辑器里空关卡是正常的；运行时创建登录页面，无需在关卡中手工摆放控件。

- 左上角 MD / 摩登地面站；左侧 64% 演示区，右侧 36% 登录区。表单按可用尺寸缩小，保持输入与按钮可见。
- 星环为首屏，随后凤凰、编队，约每 14 秒横向切换，过渡 0.75 秒。自动播放，不提供左右箭头、暂停按钮；可在演示区左右拖动或点击分页圆点切换。
- 夜景使用本次生成并导入的 `T_CityNight`；光点由 `SGCSLoginShow` 实时绘制、旋转/变形，并非把整张效果图放到登录页，也不是视频。灯阵仅为装饰，不连接真实机群、遥测或飞行命令。凤凰是程序化示意造型，与概念图细节有差异。
- 四语言菜单：简体中文、繁體中文、English、日本語。登录页标题、输入提示、按钮、演示标题与已知认证错误均翻译；语言切换不清空输入。语言仅作用于登录页，不声称地面站所有业务页已完成国际化。
- 记住账号仅保存账号，不保存密码。偏好位于 GameUserSettings.ini 的 `MD.Login.Presentation`。密码显示/隐藏可切换。
- 保留首次创建账号、异步验证、12 位正式初始密码规则以及 Development/Debug 的 123/123 测试入口；界面不再公开展示测试凭据。
- 认证成功后的场景选择沿用既有行为：从 L_GCS_Login 进入 DroneShowPlatform1；从 V3 内登录仍进入 V3。本轮不改认证权限边界。
- 包含登录背景的 AlwaysCook 配置；尚未重建 Shipping 安装包。
- `GCS.Login.VisualCheck` 为开发专用实际渲染检查，不提交登录、不连接 Console，生成四语言及三个演示阶段截图。`DroneGCS.Login.Presentation` 验证首屏、自动切换、切换互斥、反向及循环；原 Credentials 与 Smoke 继续验证账号与跨关卡链路。

美术源图：`Tools/Art/Login/CityNight.png`，AI 生成的演示场景，不代表真实项目/真实飞行记录。导入脚本 `UnrealGCS/Scripts/import_login_art.py`。原关卡和旧 UI 资源不需要复制或覆盖。

验证结果：Editor Development 构建通过（BuildLogs/login-redesign-build.log）；DroneGCS.Login.Credentials 与 Presentation 两项成功、0 失败（BuildLogs/LoginRedesignTests/index.json）。1600×900 实际渲染截图检查了三主题与四语言，结果见 BuildLogs/LoginRedesign；日文控件换行和光点半径已修正。真实 Widget Submit → 异步账号验证 → OpenLevel → 原地面站主界面闭环通过（本轮 2026-09-14 的 Login-smoke-result.txt，已复制到 BuildLogs/LoginRedesign）。未测试 Shipping 安装包或现场飞行。

另外修正了已有工作台两个通用颜色常量名与 Unity 合并编译中的局部变量冲突，仅重命名，不改变工作台行为。


### 2026-09-14 登录视觉第二轮修订（覆盖上文轮播按钮说明）

- 用户最新要求恢复左右箭头和暂停/继续。保留自动轮播、拖动与分页圆点；暂停停止灯阵时间及自动轮播，手动切换仍可用，正在进行的转场完成后停留。
- 新增代码绘制 MD 标志、语言地球、账号/锁/密码可见图标；扩大表单，采用浅蓝主按钮和右侧渐变底色。
- 三张独立 AI 背景：T_CityV2、T_PhoenixV2、T_BayV2；原始图片位于 Tools/Art/Login。按比例填充并裁切，不再拉伸旧竖图。
- 星环采用均匀球面采样、前半球显示及前后环带分层；凤凰重画羽翼、冠羽和尾羽。均为装饰性 Slate 动画，不是飞行任务数据或真实演出记录。
- Editor Development 构建通过；BuildLogs/LoginV2Tests/index.json 记录 Credentials/Presentation 两项成功、零失败。新增暂停冻结时间、停止自动切换、暂停时手动切换及恢复断言。
- BuildLogs/LoginV2 保存 1600x900 实际运行截图及语言切换检查结果。截图可能捕捉到自动横向转场中间帧。本轮未执行 Shipping 打包或人工鼠标全流程验收。

### 2026-09-14 三张静态画面定稿（取代上述逐点动画方案）

- 主题为星环、星海光幕、山海明月；删除凤凰及星球逐点绘制，静止期间不再持续重绘灯阵。切换采用 0.75 秒水平过渡，每 14 秒自动换页。
- 保留左右箭头、分页点、暂停/继续、横向拖动；控件固定在左侧文案下方同一基线。暂停自动轮播后仍可手动换页。
- 三张独立源图位于 Tools/Art/Login/*Static.png，导入为 /Game/GCS/Login/T_*Static。采用源图 1402×1122 比例，避免初始化时纹理尺寸造成错误裁切。
- 保留 MD 标志、四语言、账号记忆、密码可见及原登录链路。图片是 AI 生成的展示素材，不是飞行任务数据。
- Editor Development 构建成功；BuildLogs/LoginStaticTests/index.json 中 Credentials、Presentation 两项成功、零失败。Saved/Automation/LoginDesign 为本轮实际运行截图，覆盖三图和四语言。
- 尚未重建 Shipping 安装包，也未量化帧率；本轮性能改进是移除逐点动画绘制，不能据此承诺所有机器固定帧率。
- 本轮真实 Widget Submit → 异步验证 → OpenLevel → 主界面显示验证通过（Saved/Automation/Login-smoke-result.txt，2026-09-14 11:12）。

### 2026-09-14 无城市背景的动态星环登录页（最新确认方案）

- 统一深蓝底色，左侧悬浮灯点星环与居中文案/轮播控制，右侧独立描边登录卡片；MD 标志及四语言入口位于页头。
- 取消登录时加载三张静态城市图片。演示使用 LoginPlanetMesh.h 合并绘制球面与环带灯点；球面采样预生成，缓慢旋转与亮度呼吸。单个场景通过一次 MakeCustomVerts 提交，横移转场时两个场景各提交一次。没有逐灯 Actor、Widget 或定时器。
- 三个轮播页复用同一星环几何，以冰蓝金、青色金、蓝紫色变体呈现；原凤凰与城市画面不再使用。暂停冻结演示时间和自动换页，手动切换继续有效。
- 动画主动请求重绘频率为 30 Hz，转场期间连续请求重绘；实际绘制频率还受 Slate 上层失效影响，不承诺固定 FPS。
- 原密码验证、记住账号、密码可见、语言与登录进入地面站链路保留。尚未重建 Shipping 安装包。
- 本轮 Editor Development 编译成功，BuildLogs/LoginOrbitTests/index.json 两项测试成功、零失败；真实截图保存在 Saved/Automation/LoginDesign。

- 本轮真实登录闭环通过：Widget Submit → 异步校验 → OpenLevel → 地面站主界面显示且登录 Widget 移除，结果时间 2026-09-14 11:32（Saved/Automation/Login-smoke-result.txt）。实际截图用于视觉核对，尚未量化帧率。

### 2026-09-14 登录排版、背景与本机账号管理（最新规则）

- 背景改为较亮的深蓝渐变与柔和蓝色光晕，星环增加轮廓光和少量外围灯点；左右区域共享背景。左下角说明固定在全窗口页脚，与右下角“退出”对齐。
- 登录表单在“记住账号”右侧提供“忘记密码？”，主按钮下提供“注册账号”。首次打开默认显示登录页，注册通过明确入口进入。
- 注册及重设密码统一要求 **6–18 位**，两次输入一致；旧账号登录保留原密码兼容性。
- Development/Debug 保留 **123 / 123** 测试账号，可直接登录，不写入本机账号文件；Shipping/Test 不包含此测试入口。
- 注册为独立本机账号，不依赖邮箱或短信。注册成功显示一次恢复码，并提供复制按钮；保存后返回登录。忘记密码使用恢复码重设，成功后生成新恢复码，旧码失效。旧账号可用原密码设置恢复码。
- 密码和恢复码均用随机盐及 PBKDF2-SHA256 验证，文件不存明文。密码计算在线程池执行；新账号分文件存放，继续兼容原 account-v1.dat。
- 开发测试命令 `GCS.Login.AccountSmoke` 使用隔离账号执行真实 Widget 注册 → 恢复码重设 → 登录 → 地面站，并删除测试账号；结果在 Saved/Automation/Login-account-flow.txt。
- Editor Development 构建及 Credentials、Recovery、Presentation 三项测试通过，0 失败。实际渲染检查包含登录、注册、重设密码、旧账号恢复码设置及四语言。证据目录 BuildLogs/LoginAccount、BuildLogs/LoginAccountTests。尚未重建 Shipping 安装包，未量化帧率。

### 2026-09-14 星环细化与退出图标

- 星球改为 5000 个预生成球面采样，显示前半球；增加不均匀轮廓辉光、细密边缘灯点和少量高亮星点。灯点中心与外围光晕分层绘制，并稍微缩小球体，为文案留出空间。
- 七条金色环带采用细光轨与大小、亮度不同的灯点，继续计算前后遮挡；三主题均保留金色环带，球面分别使用蓝白、青白、淡紫白。
- 在“退出”文字左侧补上门框与向右箭头图标，共用按钮点击区域。
- Slate 网格在接近 60000 顶点时分批提交，避免 16 位索引溢出；本节取代旧文档“每场景严格一次提交”的描述。仍无逐灯 Actor 或 Widget，保留旋转、呼吸、横向切换和暂停。
- Editor Development 编译通过；1600×900 实际运行截图、球体区域两帧变化检查位于 BuildLogs/LoginPlanetRefine。本轮未修改账号流程，也未重新执行账号测试或量化帧率。图片是实际程序截图，程序化效果与概念图仍有细节差异。

### 2026-09-14 离线光效短片轮播（当前实现，取代实时星球网格）

- 三主题改为 **星环、光翼、光潮**，每段 8 秒、24 fps、1280×800；使用原创数学灯阵离线烘焙辉光与运动，运行时不再构建星球灯点网格。没有凤凰、城市背景或真实飞行数据。
- 星环缓慢往复转动并流动发光，光翼以抽象编队展开，光潮以三层蓝白/青色/金色光带起伏。用 UI 加法材质融入现有深蓝渐变；右侧账号功能、四语言与退出图标保留。
- 仅一个 MediaPlayer。横向换页期间下一页显示对应封面，转场结束再打开该视频，准备好后 0.3 秒淡入。保留自动轮播、左右箭头、分页点、暂停及拖动。
- 播放失败或准备超时自动保留封面；支持 `-LoginStaticShow` 或 GameUserSettings.ini 的 `[MD.Login.Presentation] StaticShow=True` 强制静态。移除登录 Widget 时关闭解码器并解除视频纹理关联，避免登录演示继续在工作台后台播放。
- 美术生成及复现步骤见 `Tools/LoginMedia/README.md`。三段 MP4 总计约 11.9 MiB；封面与材质位于 `/Game/GCS/Login/Media`，视频位于 Content/Movies/Login，并配置 NonUFS 发布依赖。旧 `LoginPlanetMesh.h` 已不在登录绘制路径使用。
- Editor Development 构建通过；Credentials、Recovery、Presentation 三项测试通过。真实 1600×900 游戏窗口检查覆盖三段播放、暂停、手动换页、静态回退、恢复及 Widget 移除后的解码器关闭，全部通过。证据：`BuildLogs/LoginMedia`、`BuildLogs/LoginMediaTests`。
- 本机这次检查的横移阶段帧间隔均值 3.89 ms、P95 4.39 ms、最大 5.21 ms；其他采样段均值 3.98 ms，最大 70.61 ms。统计包含测试中的加载与切换流程，不能当作稳定 FPS 保证，也未将峰值归因到特定子系统。尚未验证 Shipping 安装包或不同配置电脑，视觉仍与概念图有细节差异。
