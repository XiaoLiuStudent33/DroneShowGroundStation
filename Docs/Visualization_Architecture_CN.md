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
