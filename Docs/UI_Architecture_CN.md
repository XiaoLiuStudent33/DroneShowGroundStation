# DroneGCS UI Architecture

Status: CURRENT
Authority: 当前 UMG 结构、C++ binding 和交互状态边界
Last Updated: 2026-08-25
Applies To: `WBP_GCS_*`、`UGCSUIBridgeSubsystem`、`FGCSUIWidgetPresenter`

## 1. Purpose 与 Authority

- UMG Designer拥有布局、Anchor、Size、字体、颜色和静态视觉。
- `UGCSUIBridgeSubsystem`拥有UI lifecycle、交互绑定、页面/面板状态协调、命令选择和命令提交。
- `FGCSUIWidgetPresenter`只负责Widget查找、文本、颜色和Visibility派生。
- `FleetManagerSubsystem`拥有真实Fleet和详情选择。
- `URoutePreviewSubsystem`拥有Route数据和Playback；Widget不累计第二个时钟。
- `UGCSRouteWorkflowCoordinator` 协调 Route Convert / Upload 页面与当前 Route、Assignment、Artifact、Upload Job authorities；Widget 不拥有第二份 Job state。

C++不得无理由覆盖Designer geometry。只有修复根Canvas/viewport坐标边界时可以设置必要Anchor/Alignment。

### Fleet Select-All 与 Command Selection

- `SelectedCommandDroneIDs` 是 Command Selection 的唯一 Authority；详情选择仍由 `FleetManagerSubsystem::CurrentSelectedDroneID` 独立维护。
- `BTN_FleetSelectALL` 不遍历五行分页 Widget。`UGCSUIBridgeSubsystem` 一次扫描完整 Fleet，构造排序后的非空 Online DroneID 集合，并与当前 Command Selection 做精确集合比较。
- 非精确相等显示 `X`，点击后原子替换为全体 Online；精确相等显示 `✓`，点击后原子清空。零 Online 时显示 `X` 且 Disabled。
- Fleet 上下线后按钮从 Authority 重新派生；新上线目标不会被静默加入。活动 AutoStart/Upload Job 期间复用 Route Workflow mutation gate，禁止改变 Frozen targets。
- 批量替换最多产生一次 `OnCommandSelectionChanged`、一次 Visualization 同步和一次 Fleet 刷新；重复 Bind 使用 Remove/Add，关闭/重开不创建第二套选择状态。

## 2. Startup 与 Main Widget

```text
Game/PIE World initialized
-> CreatePrototypeUI
-> /Game/UI/WBP_GCS_Main_V2
-> AddToViewport(100), viewport fill
-> GCSUIBridgeSubsystem::BindMainWidget
```

Main children：

- `CHILD_TopBar`
- `CHILD_LeftToolbar`
- `CHILD_FleetPanel`
- `CHILD_DroneDetail`
- `CHILD_RightToolbar`
- `CHILD_RightPanel`
- `CHILD_Playback`
- `SelectionRectangle`

## 3. Authored Widgets

| Asset | Current responsibility |
|---|---|
| `WBP_GCS_Main_V2` | viewport root和child composition |
| `WBP_GCS_TopBar` | 顶部状态 |
| `WBP_GCS_LeftToolbar` | Menu/Fleet/Control/Map/Settings/Logs入口 |
| `WBP_GCS_FleetPanel` | Fleet分页、命令勾选、Selected Count |
| `WBP_GCS_DroneDetail` | 单机详情和飞行控制 |
| `WBP_GCS_RightToolbar` | View/TopDown等视图入口 |
| `WBP_GCS_RightPanel` | LED颜色控制等右侧功能 |
| `WBP_GCS_Playback` | Route Import入口和Playback控件 |
| `WBP_GCS_RouteImport` | NPZ浏览、Origin、SampleRate、metadata、加载Preview |
| `WBP_GCS_RouteWorkflow` | Route Convert / Route Upload workflow shell |
| `WBP_GCS_RouteConvert` | Route load、assignment、waypoint generation 与 Artifact workflow |
| `WBP_GCS_RouteUpload` | Upload Job command、aggregate progress 与状态展示 |
| `WBP_GCS_RouteUploadRow` | 单 Route/Drone upload state row |
| `WBP_GCS_PageMap` | Map页面 |
| `WBP_GCS_PageSettings` | Settings页面 |
| `WBP_GCS_PageLogs` | Logs页面 |

## 4. Initial Visibility 与 Toolbar

Main Widget每次新Bind时一次性设置：

- FleetPanel = Collapsed
- DroneDetail = Collapsed
- RightPanel = Collapsed

`BTN_Fleet`只Toggle FleetPanel，`BTN_Control`只Toggle DroneDetail。允许两者同时开或同时关，不是互斥Tab。

Map/Settings/Logs是独立viewport page。点击当前已显示页面会关闭它并回到主场景；切换到另一个page时只显示目标page。

## 5. Selection Semantics

- 点击Fleet row：更新`FleetManager::CurrentSelectedDroneID`，只影响详情。
- 点击row selector或Marquee：更新`SelectedCommandDroneIDs`，只影响命令目标。
- Route Upload 自动分配只消费 `SelectedCommandDroneIDs`；零选择不会回退为全部 Online drones。
- 两套选择禁止合并。
- 隐藏Panel不会清空任何选择、分页、Selected Count或Drone detail state。

## 6. Commands 与 LED

UI command只调用`DSPGCSProductionCommandProviderSubsystem`。

LED palette：

- `BTN_ColorOff`
- `BTN_ColorWhite`
- `BTN_ColorRed`
- `BTN_ColorGreen`
- `BTN_ColorBlue`
- `BTN_ColorYellow`
- `BTN_ColorMagenta`

点击流程：

```text
SelectedCommandDroneIDs
|-> optimistic UE DroneColors preview
`-> DSP-GCS SetLedColor -> Console -> real drone
```

空选择时不修改全机群，也不发送命令。

## 7. Playback — IMPLEMENTED

绑定控件：

- `BTN_RouteImport`
- `BTN_PreviousFrame`
- `BTN_Pause`
- `BTN_Play`
- `BTN_Stop`
- `BTN_NextFrame`
- `SLD_RouteTimeline`
- `TXT_RouteTime`
- `TXT_RouteFrame`

UI只调用`URoutePreviewSubsystem`。10Hz轻量UI refresh只读取状态；Route frame仍由RoutePreview的单一FTSTicker驱动。

Slider使用0..1归一化；用户scrub时暂停，结束后不自动恢复。程序更新Slider时使用feedback guard。

## 8. Route Import — IMPLEMENTED

绑定控件：

- `BTN_RouteImportClose`
- `BTN_RouteBrowse`
- `BTN_RouteCancel`
- `BTN_RouteLoadPreview`
- `EDT_SampleRate`
- `EDT_OriginLatitude`
- `EDT_OriginLongitude`
- `EDT_OriginAltitude`
- `TXT_RouteFilePath`
- `TXT_RouteDroneValue`
- `TXT_RouteFrameCountValue`
- `TXT_RouteDurationValue`

行为：

- Popup单实例，关闭使用Collapsed，重新打开复用。
- Root Canvas填充viewport，`BORDER_RouteImport`使用center anchor/alignment，不使用固定分辨率offset。
- Windows Runtime `IFileOpenDialog`只允许`*.npz`，可用于packaged Windows runtime，不依赖Editor-only DesktopPlatform。
- 完整绝对路径保存在C++；UI只显示clean filename。
- Browse调用`PrepareRouteImport`，不会立即替换active route。
- Hint Text不是输入Authority。
- SampleRate必须finite且>0；Latitude [-90,90]；Longitude [-180,180]；Altitude finite meters。
- Load Preview成功后Commit、设置Origin/Rate、Enter Preview、Seek Frame 0并关闭Popup。
- Cancel/Close不Unload现有Route，也不Exit Preview。

## 9. TopDown 与 Marquee

- 正式定位按钮：`BTN_ZCamera`。
- TopDown是显式Toggle，不由RMB/WASD自动退出。
- Wheel zoom、MMB pan只在TopDown生效。
- LMB Marquee与MMB pan互不复用状态。
- Marquee期间camera look lock在release后恢复。

## 10. Route Operations UI — IMPLEMENTED

`UGCSRouteWorkflowCoordinator` 加载并绑定 `WBP_GCS_RouteWorkflow`、`WBP_GCS_RouteConvert` 与 `WBP_GCS_RouteUpload`。当前已实现：

- Route browse、Preview load、Assignment/Artifact/Waypoint workflow 协调。
- Upload aggregate/route state 读取与 `FDSPGCSUploadJobStateMirror` 展示。
- Start、Pause、Resume、Cancel、Retry Failed、Apply Max Concurrency。
- Start 自动执行 canonical DroneID 映射、Working Assignment、Freeze、Console Sync 与 exactly-once Job Start；正常 UI 不要求先运行 `BindExplicit`。
- 映射预览来自 Manifest Route Record，显示 `DroneID -> Slot -> route basename`；不从 Niagara、`M_DroneIdLabel`、SYSID 或屏幕排序取 Authority。
- `EDT_MaxConcurrency`（`UEditableTextBox`）、`BTN_ApplyMaxConcurrency`（`UButton`）与 `TXT_CurrentMaxConcurrencyValue`（`UTextBlock`）承载 E.1 操作员并发 Authority。Apply 严格接受 `1..50` 整数；失败保留上次 Applied 值并不创建 Frozen/Job。Start 快照 Applied 值，运行中修改只影响下一 Job；AutoStart/Freeze/Sync/非终态 Job 期间禁止 Apply。
- Waiting、Preparing、Uploading、RetryWaiting、Succeeded、Failed、Cancelled 统计与总进度。
- command gate、safe diagnostic 与 stale binding rejection 展示。
- Workflow 关闭只隐藏 View；AutoStart 在隐藏期间继续接收/推进，重新打开不会重复 Freeze/Sync/Start。

UI command 通过 `UDSPGCSUploadJobSubsystem` 发送 DSP-GCS Upload Job protocol，不直接调用 MAVLink 或 `RouteUploadManager`。

ConsoleGCS 继续 headless，不创建第二套正式 Operator UI。E.1 软件 Gate 已完成，E.2 多机硬件验证尚未完成；Console 重启后的 DroneID 发现顺序风险留给 E.3。
