# DroneGCS Visualization Architecture

Status: CURRENT
Authority: 当前 UE Fleet/Route 可视化架构
Last Updated: 2026-08-24
Applies To: `ADroneFleetVisualizationActor`、Cesium、Niagara、Marquee、TopDown

## 1. Purpose

本文定义当前批量可视化数据链和已实现的 Live + Route Overlay 边界。业务 Fleet 与 Route Authority 分别见 [Code_Architecture_CN.md](Code_Architecture_CN.md) 和 [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)。

## 2. Frozen Runtime Shape

```text
one ADroneFleetVisualizationActor
-> one UNiagaraComponent
-> one NS_DroneFleet
-> N stable particles
```

禁止 per-drone Actor、Component、Niagara、Widget、TextRender、Tick、Timer。

## 3. Live Mode — IMPLEMENTED

```text
FleetManager stable DroneOrder
-> ParticleDroneIDs
-> GPS/AltitudeAMSL
-> CesiumGeoreference
-> DronePositions
-> Niagara arrays
```

- `ParticleDroneIDs[Index]` 是粒子到 Live DroneID 的稳定映射。
- Offline Drone继续保留Fleet成员和稳定顺序。
- GPS无效不制造随机位置。
- `FleetManagerSubsystem`仍是Live状态Authority。

## 4. RoutePreview Mode — IMPLEMENTED

```text
FRoutePreviewData frame
-> Local ENU + Show Origin
-> Cesium ESU/UE World
-> DronePositions / DroneColors
-> same Niagara component
```

RoutePreview 进入后接管完整 Preview 视觉数组；Live telemetry 仍继续进入 FleetManager。Exit Preview 后重新提交最新 Live snapshot。

完整 RoutePreview mode 与 Live drone arrays 仍保持单一写入 Authority；Live mode 下的 Route Target Overlay 使用独立 target arrays/visibility，不覆盖 Live particle mapping。

## 5. Niagara Data Contract — IMPLEMENTED

| C++ data | Niagara user parameter | Meaning |
|---|---|---|
| `DronePositions` | `User.DronePositions` | UE world positions |
| `DroneColors` | `User.DroneColors` | sphere/LED preview colors |
| `DroneDisplayIDs` | `User.DroneDisplayIDs` | stable particle index + 1 |
| `DroneSelectedMasks` | `User.DroneSelectedMasks` | command selection mask |
| particle count | `User.DroneCount` | active visualization count |
| label visibility | `User.ShowDroneLabels` | ID sprite renderer enable |

`Particles.Color`只承载球体颜色；`DisplayID`和`SelectedMask`使用独立粒子属性和Dynamic Material Parameter。Selection不得再次污染Sphere RGB。

## 6. Selection 与 LED — IMPLEMENTED

- Selection Authority：`UGCSUIBridgeSubsystem::SelectedCommandDroneIDs`。
- `DroneSelectedMasks`是派生render cache。
- Fleet勾选、Selected Count、Orange ID和Orange rim来自同一Authority。
- Color palette只修改选中StableParticleIndex对应的`DroneColors`。
- 取消选择不恢复颜色；`DroneColors`是UE preview，不是真实设备LED state Authority。
- 真实LED命令仍经DSP-GCS和Console发送。

## 7. Drone ID Labels — IMPLEMENTED

- DisplayID：1..N，来源`stable particle index + 1`。
- Material data：Dynamic Parameter R=DisplayID、G=SelectedMask。
- 未选中浅灰白，选中Orange `#FF8A00`。
- Sprite基于Particles.Position并使用camera offset避免mesh遮挡。
- Label只在TopDown时通过`User.ShowDroneLabels=true`显示。
- Perspective默认隐藏。

## 8. TopDown Orthographic — IMPLEMENTED

- 正式按钮：`BTN_ZCamera`；旧`BTN_GPS`只保留兼容fallback。
- 第一次点击进入Orthographic Z TopDown，第二次恢复PreviousViewTarget。
- Camera XY来自Fleet bounds center。
- OrthoWidth根据Fleet XY bounds、viewport aspect和margin自动fit。
- 进入时重新fit，不保留上次pan/zoom。
- Wheel中心缩放：只修改OrthoWidth。
- MMB drag：使用camera right/up投影在XY平面pan。
- Camera Z和Rotation在pan/zoom中保持不变。
- 只有BTN_ZCamera第二次点击正常退出；RMB/WASD/mouse move不退出。

## 9. Marquee — IMPLEMENTED

```text
LMB down/drag
-> viewport-local mouse coordinates
-> authored SelectionRectangle
-> ProjectWorldLocationToWidgetPosition(..., false)
-> point-in-rectangle
-> ReplaceCommandDroneSelection
```

- 正反方向drag都用min/max归一化。
- 选择使用真实Drone world position，不使用LabelCameraOffset。
- Drag超过阈值才`SetIgnoreLookInput(true)`；release恢复。
- Marquee lock和TopDown state独立。
- MMB pan不触发Marquee，LMB不触发pan。

## 10. Live + Route Overlay — IMPLEMENTED

当前显示语义：

```text
filled sphere = Real Drone
hollow target = Route Frame 0 / selected route frame
```

实现保持一个 Niagara System，并使用：

- `RouteTargetPositions` / `User.RouteTargetPositions`。
- Route target count 与 `User.ShowRouteTargets`。
- 同一 `NS_DroneFleet` 内独立 Route Target render data。
- `RoutePreviewSubsystem::SetRouteOverlayEnabled` 与 refresh/clear lifecycle。

禁止：

- 第二个Niagara System。
- 用RouteSlot覆盖Live ParticleDroneIDs。
- per-drone target Actor。

Overlay 只负责显示，不成为 RouteAssignment Authority。全量 RoutePreview 激活时 Overlay 隐藏；退出 Preview 回到 Live 后按 preference 恢复。

## 11. Scale

- 1000 drones：当前稳定目标。
- 10000：未来测量目标；先测CPU array build、Cesium conversion、Niagara upload、GPU overdraw和label policy。
- Route Overlay 增加第二批 render data 后仍需重新测量，而不是默认等同当前 1000 架成本。

## 2026-09-10 — Locked-origin orbit camera

- `AGCSInvisiblePawn` now owns a CameraComponent and local orbit state. DefaultPawn movement bindings are disabled; inherited movement is deactivated at BeginPlay. No Content assets changed by this implementation.
- BeginPlay / `ResetOrbitView` (Home) reads the existing Visualization geographic tangent basis when available, otherwise uses tagged `GCS.SceneMap` / `BP_GCS_2DMapGround` bounds and axes. The adopted center remains fixed until explicit reset/basis update; later telemetry, selection and preview changes do not silently move it.
- Perspective: MMB orbit, wheel distance, Home reset/re-fit; locked origin blocks pan. `SetOrbitOriginLocked(false)` enables Shift+MMB camera-plane pan; relocking restores the saved center. View-state APIs are Blueprint callable on the player pawn. They do not write Fleet/Cesium/map/route state.
- Auto-fit uses the map bounding sphere relative to the adopted center and viewport aspect ratio. A missing map has a finite default viewing distance. No per-drone Tick or per-frame fleet bounds scan.
- UI blocking / LMB selection / ignored look input suppress orbit gestures. Drag must start in scene space. The old BTN_ZCamera orthographic mode keeps its existing MMB-pan/wheel behavior; while another ViewTarget is active, the pawn does not process orbit input. Exiting topdown returns to the retained orbit camera.
- UE Editor Development build PASS (`BuildLogs/orbit-build.log`). `DroneGCS.UI.` automation: 39/39 (35 success, 4 success-with-warnings, 0 failed), `BuildLogs/OrbitUI/index.json`. Strengthened pawn tests cover tilted tangent basis, look-at center, zoom, locked pan, unlocked pan/relock, invalid basis and preserving an external ViewTarget.
- Manual acceptance remaining: actual map fit, mouse feel, UMG hit regions and topdown enter/exit in the user's level. NullRHI tests do not prove visual appearance. No physical commands or backend changes in this camera step.

### 2026-09-10 Orbit control panel binding

- RightPanel `BTN_ResetOrbitView` and `CHK_OrbitOriginLocked` are connected through UIBridge to the existing pawn view API. Weak control references are explicitly unbound before rebind and at UI teardown; UI lifecycle never resets camera state.
- Existing UI input polling projects current pawn lock state (including Home and pawn replacement) without widget-tree/fleet scans per frame. Projection has a re-entry guard. No pawn disables controls; orthographic mode disables the perspective lock checkbox. Reset exits orthographic mode before resetting orbit.
- No Blueprint click/check event wiring is needed. User-authored Widget assets are loaded read-only by the new `DroneGCS.UI.Orbit.ControlsLifecycle` test, covering actual names/types, repeated bind, reset, lock, Home projection, unbind, reopen, topdown and missing pawn.
- Final binding verification: Editor Development PASS; UI Automation 40/40 PASS (4 warnings), `BuildLogs/OrbitControlsUI-v3/index.json`. Earlier fixture failures were due to the standalone editor world's missing controller registration; explicit registration resolved them without production workaround.

### 2026-09-10 更正锁定交互：锁定也允许平移

按用户确认，MMB 旋转、滚轮缩放、Shift+MMB 平移在锁定与解锁两种状态都可用，取代上文“锁定禁止平移”的旧行为。锁定时平移记录相机平面取景偏移，保存的旋转基准不变；解锁时将偏移吸收到自由观察中心，镜头不跳变；重新锁定恢复保存中心并清除偏移。Home/复位清除取景偏移。地图、Cesium 与业务坐标不变。

## 2026-09-10 — Full orbit and flat-map ground alignment

- Orbit elevation now wraps through 360 degrees. Analytic camera-up vector remains orthogonal to view direction at both poles, instead of MakeFromXZ with a fixed Up that flips at the pole. Horizontal rotation remains unrestricted. Active MMB gesture warps cursor back to its viewport start point after each delta; panel hit-test applies at gesture start, focus loss/LMB/viewtarget change ends gesture. Actual desktop-edge and mouse-feel acceptance remains manual.
- Scene-map discovery is cached once alongside Cesium lookup. Tagged `GCS.SceneMap` / named BP_GCS_2DMapGround uses its MapPlane component (root fallback). For this flat-map presentation, the existing visualization anchor is projected onto the map plane; a 15 UU marker radius offset places the 30 UU sphere bottom on the surface. Map transform/scale is never modified.
- Live uses the existing frozen Fleet origin's altitude as display reference; horizontal geographic tangent displacement is projected into map East/North, altitude difference is preserved explicitly (no Earth-curvature sag into a flat plane). This does not establish a surveyed terrain altitude; the fixed origin must represent the intended ground reference. Invalid GPS handling is unchanged.
- NPZ overlay and full playback now both map ENU into that same flat display frame. Pure ConvertRouteENUPositionsToWorld remains absolute Cesium conversion, and source ENU/GPS/Waypoint/upload semantics are untouched. No-map scenes retain prior behavior.
- Negative live altitude differences and negative NPZ Z remain negative. Once-per-actor diagnostic warnings explain below-reference data; no automatic clamping, always-on x-ray, forced landing or data rewriting.
- Read-only audit of D:/Download/MDHK_100P_5000F_260817_1345.npz: first frame 99/100 negative Z, minimum -25.863325m; last frame all Z=0. SHA256 and metrics: BuildLogs/map-orbit-npz-audit.json. Therefore source-negative first-frame points may still be occluded. Disable map visibility for inspection; source correction requires independently verifying intended trajectory, not a display offset.
- Regression adds tilted/translated map, live 10m altitude difference, negative altitude preservation, overlay/full equality, NPZ landing at map surface, map/Cesium transforms and source ENU unchanged, preview exit, full vertical/horizontal revolution, pole continuity and below-plane viewing. No physical flight operation is exercised.

### 2026-09-10 负高度仅在地图渲染中贴地（后续用户确认）

此项取代上一节“负高度继续显示在地图下方”的显示策略。用户明确要求不修改小球数据，仅调整渲染：实时无人机相对显示基准的高度及 NPZ 预览 ENU Z，在平面地图显示转换中使用 max(0, height)。正高度不增加整体偏移，负高度显示在地图表面；球心保留现有 15 UU 半径偏移使球底贴地。地图倾斜或移动时沿地图法线计算，不强制改变地图世界 Z。

原始遥测、NPZ 数组、绝对 Cesium 坐标转换、界面高度读数和航线上传不修改。负高度诊断仍记录原始数值，文字说明仅显示贴地。覆盖实时显示、叠加预览和完整播放。用户无需修改控件或下移地图。

验证：Development Editor 编译通过；DroneGCS.Visualization 自动化通过，报告 BuildLogs/DisplayGround/index.json。GroundAndPreviewAlignment 覆盖负高度贴地、正高度保持 10 m、原始遥测及 ENU 不变、绝对转换保留负高度、地图与 Georeference 变换不变。NullRHI 测试验证坐标逻辑；实际画面需重新启动 UE 验收。

### 2026-09-10 桩基应用后坐标轴被地图遮挡

确认旧 HandleFixedPileApplyClicked 使用 OriginWorld - UpWorld * 300，平面地图原点为 Z=15 时将轴放到 -285。删除旧 FixedPileAxisDepthOffsetUU 常量及下移操作，传入显示原点本身，坐标轴与小球基准对齐；不更改蓝图、地图、遥测或航线上传。此项仅修正轴的位置，不修改蓝图根据 East/North 生成的旋转。

Development Editor 编译通过（BuildLogs/axes-ground-build.log）。实际画面验收：重新打开 UE 并应用桩基，水平地图位于 Z=0 时轴位置应为 Z=15，不再为 -285。

### 2026-09-10 坐标轴子网格偏移翻转

前一次仅取消 Actor 下移还不完整。通过只读 Python 实例化实际 BP_GCS_CoordinateAxes，确认 InitializeCoordinateAxes 将 Actor 旋转 180 度，SM_XAxis/SM_YAxis/箭头局部 Z=20 因此变为世界 Z=-5（Actor Z=15），并非 Actor 隐藏。证据 BuildLogs/inspect-axes.log。

C++ 在蓝图初始化后，沿 North x East 得到的地图上方向，将低于显示原点的静态网格偏移镜像到上方。保留轴水平坐标、东西南北朝向及 Actor 原点，材质、蓝图资源和地图不变。当前资产应为 Actor Z=15、轴网格 Z=35。添加实际资产初始化及重复应用回归检查，避免只验证 Actor 坐标遗漏组件遮挡。

验证完成：Development Editor 编译成功；AxesMeshV4/index.json 定向回归 1/1 通过、无警告。实际蓝图重复初始化后，全部静态轴网格 Z=35、Actor Z=15，East/North 方向保留。测试使用 FEditorScriptExecutionGuard 允许编辑器测试世界执行蓝图；早期测试未开启此保护导致蓝图不执行，已修正。NullRHI 不验证像素，重启 UE 应用桩基后进行最终画面验收。

### 2026-09-10 可调整的本地地图贴图

UGCSMapSubsystem 接入 RightPanel 地图控件。首次捕获 MapPlane 的场景基准变换，底图宽度、东/北偏移和旋转仅修改贴图网格；实时无人机、NPZ 与坐标轴使用冻结基准。此机制取代此前直接读取当前底图变换的显示路径，避免调整底图同时拖动机群。地图物理宽度使用米，与图片像素尺寸分开；初始宽度取网格实际尺寸，不代表经过地理标定。

专用 M_GCS_RuntimeMap 使用 Unlit、TwoSided、Translucent，参数 MapTexture/MapOpacity。原有地图材质及用户 UI 布局保留。PNG/JPG 异步解码，单次导入、最大 32MB 和 4096x4096 像素；失败保留当前贴图。设置写入 Saved/MapSettings/<关卡>.ini，保存原图片路径，移动或删除图片会影响下次恢复。重置地图调整恢复宽度和零偏移/旋转，保留贴图与透明度。查看整张地图改变相机取景，不修改飞行数据。在线地图源切换尚未接入。
