# V3 原生工作台：机群与视图接入

2026-09-11。当前 V3 用原生 Slate 重新绘制，替代此前的旧控件组合。当前已接入机群遥测、搜索、详情、命令目标勾选和本地视图操作；飞行命令等其余区域仍为占位。

## 打开

打开 `/Game/Maps/L_GCS_Workbench_V3`，点击运行，正常登录后进入新版。Development 测试账号仍为 `123 / 123`。从默认登录关卡启动仍进入原工作台，本轮没有切换产品默认入口。

## 布局

- 左抽屉顶部：机群、日志、筛选。
- 右抽屉顶部：视图、控制、参数。
- 中央视图区左上：定位、播放。
- 左右内侧把手分别控制抽屉，约 0.2 秒滑动，可中途反向。
- 播放按钮或底部把手控制播放区域；Esc 收起播放区域。
- 控件尺寸以 1672×940 为设计基准等比缩放，但布局占满实际视口：顶栏贴顶、底栏贴底、右栏贴右；额外高度分配给面板和列表，额外宽度分配给中央视区。窗口调整时绘制和命中坐标同步更新，不再将整个界面按固定 16:9 居中留边。

这不是整张截图贴图：文字、圆角、线条、色块和图标均由 Slate 绘制。地图仍是原工程中的 UE 场景，效果图的生成园区不属于本次地图资源。V3 运行时隐藏原粗坐标轴，原关卡资源不变。

## 演示范围

可交互：两侧标签切换、抽屉收起/展开、播放区域开关、仿真分组折叠，以及按钮悬停反馈。

已接入：机群在线/勾选统计、编号搜索、虚拟化列表、当前搜索结果中的在线飞机全选、详情、场景点选和多机高亮；定位、透视/正交、地面坐标轴 XY、10 米地面网格、角落方向指示器。

占位：日志与高级筛选、参数输入、飞行按钮、灯光色块、仿真启动及播放执行。占位操作仍不发送指令、不写配置。顶栏与底栏连接状态来自原 Production Telemetry Provider，不以列表有数据冒充已连接。

## 代码边界

- `GCSMockWorkbench.h/.cpp`：UUserWidget 包装原生 Slate 展示层；通过 FGCSWorkbenchModel 读取 Fleet 和既有多机选择接口，不提交飞行命令。
- 模块启动代码在 V3 登录成功后移除旧视图并创建新展示层。
- `GCSUIBridgeSubsystem::BindMainWidget` 在 V3 原型地图中不绑定旧控件，避免旧视图参与控制。
- 原地图 `/Game/Maps/DroneShowPlatform1` 的正常业务入口保持原逻辑。
- 旧 `UI_V3` 资源和生成器暂留；`build_v3_workbench.py` 属于上一版控件组合，不能用于生成当前原生原型。

机群搜索框已是真实输入控件。右侧参数/飞行控制区域的输入框仍是绘制占位，后续接业务时需补充校验、禁用状态和服务绑定。

## 历史布局阶段验证

Editor Development 编译日志：`BuildLogs/mock-workbench-build4.log`。

1672×940 实际运行截图与本地交互结果：`BuildLogs/MockWorkbench1672/`。检查了机群默认页、筛选/视图页、左右收起和播放展开。测试使用与鼠标点击相同的本地事件分派，但不代替完整人工交互验收。

开发构建可使用 `-GCSMockCapture` 在 V3 直接运行隔离假 UI 截图检查并自动退出；只在 `WITH_DEV_AUTOMATION_TESTS` 中启用，不创建登录会话。正常使用仍需登录。

登录烟雾命令 `GCS.Login.Smoke` 已更新为识别 V3 原生主视图；使用隔离测试账号文件验证实际登录提交、异步验证、关卡切换和新主视图显示。

本轮正常登录烟雾检查通过，1280×720 截图为 `BuildLogs/MockWorkbench1672/04-login-1280.png`，结果为同目录 `login-result.txt`。启动日志仍有 Cesium 的 `FCesiumMetadataPropertyStatisticValue::Semantic` 初始化报错；该项不属于本轮 UI 改动，不能把本次结果表述为启动日志零错误。

本次没有进行真实飞行、后端规模、Shipping 打包验收。原 UI/原地图 38 个受保护资源哈希与原有基线一致。

## 视口比例修正

原实现将整套 UI 当作固定 16:9 画布等比居中，导致编辑器内较高视口上下留空。现改为独立计算控件缩放与可用布局空间，统一供绘制、抽屉位置、播放区域和点击坐标使用。布局计算位于 `Private/GCSMockWorkbenchLayout.h`。地图相机取景不属于这次修正。

编译日志：`BuildLogs/mock-responsive-build3.log`。开发截图模式额外传 `-GCSMockResize` 可在 1400×1040 启动后自动切换到 1600×720，再验证抽屉和底部播放把手。

最终实机验证：1400×1040 启动，运行中调整到 1600×720，顶栏/底栏贴边；两种比例的左右抽屉、标签切换、底部播放把手事件分派检查通过。截图和结果保存在 `BuildLogs/MockResponsive/`；最终运行日志为 `Mock-responsive-verified.log`。定时自动截图模式隔离外部点击，普通运行输入不受影响。

## 紧凑比例

根据视觉反馈，控件比例调整为此前的 85%（`FGCSMockWorkbenchLayout::ControlScale`）。字体、按钮、边栏宽度与顶底栏高度一起缩小，新增空间由中央视区和伸展面板吸收，继续保持四边贴合；场景相机缩放不变。

## 视区方向指示器

中央视区右下角增加屏幕空间 X/Y/Z 方向指示器，分别为红、绿、蓝色。读取所属 PlayerController 的实际相机视角，投影 UE 世界三轴；相机旋转时实时更新，缩放地图不会改变指示器尺寸。右侧抽屉收放时跟随视区边缘，底部播放面板展开时上移避让。沿视线的轴以圆点及标签显示。它是世界方向参考，不代表经纬度或北向，也不控制飞行。视图页现在提供三个独立显示开关。网格/世界轴是中央视区裁剪的投影参考线，不改地图资源和飞行坐标。

开发截图参数 `-GCSMockCapture -GCSMockAxes` 在首张截图后旋转环绕相机，再记录方向变化及抽屉/播放展开画面。

## 机群与视图接入边界

- `GCSWorkbenchModel` 每 0.1 秒检查 FleetRevision，保留按 DroneID 的行对象，仅成员/搜索结果变化刷新列表结构。
- `SGCSFleetPanel` 使用真实 SEditableTextBox、SCheckBox 和 SListView；只生成可见行。搜索不清除勾选，详情不改变命令目标。离线/移除目标从可用勾选中清理，会话切换清理旧选择；已有事务锁仍由旧桥接接口执行。
- 场景点击查看详情，蓝色方框为详情焦点，原 Niagara 选择遮罩表示多机勾选。定位优先有效的在线勾选机群；无勾选时定位全部有效在线机群，无有效坐标则恢复地图取景。
- `GCSInvisiblePawn` 新增投影切换，沿用相同环绕、缩放和平移；正交裁剪根据实际相机距离设置。V3 面板背景与控件区域阻止场景拖动/缩放。
- 原 `/Game/UI`、地图资源与 Console 协议未改动。旧桥接仍负责共享选择，但不会绑定旧 Widget 树。

## 本轮验证

开发模式参数 `-GCSMockCapture -GCSWorkbenchAcceptance` 使用 Adapter 注入 1000 架测试遥测，覆盖搜索、在线全选、详情/勾选分离、增量和离线清理、场景点选、正交切换、空快照清空。必须同时传 `-ini:Game:[DSPGCS.ProductionTelemetry]:Enabled=False -ini:Game:[DSPGCS.ProductionCommand]:Enabled=False`，测试检测到 Production Provider 启用会立即退出，不能向正常业务会话注入。截图测试隔离外部鼠标输入。此验证是进程内 Adapter/UE 联调，不等同于 Console UDP 或真机飞行验收。

## 底图比例与 Z 轴

本机 V3 MapSettings 原 Width=1 米，旧版 Width=100 米。已备份并将 V3 显示宽度恢复到 100 米，新工作台初始化调用原 MapSubsystem 恢复配置再取景。此宽度用于底图显示，不等同于场地地理标定；无人机坐标、间距和模型尺寸未修改。场景只画 X/Y 参考线，右下角 XYZ 方向指示器继续保留。

最终验证：`BuildLogs/workbench-live-build-final.log` 编译通过；`BuildLogs/WorkbenchLive/live-result.txt` 中 14 项 Adapter/UE 联调检查通过，实际截图位于同目录。`BuildLogs/WorkbenchOrbitRegression/index.json` 旧相机回归 1 项成功、0 失败。38 项原 UI/关卡保护资源哈希未变化。实际 Console UDP 和现场真机未在本轮验收。
