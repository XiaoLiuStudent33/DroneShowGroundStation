# 遥测筛选：条件数量与无人机选择

## 控件契约

主面板 `WBP_GCS_TelemetryFilter` 的 `CHK_SelectAllResults` 放在
`TXT_ResultCount` 与 `LV_FilterResults` 之间。

- 条件行 `WBP_GCS_TelemetryConditionRow`：`TXT_ConditionMatchCount` 显示独立匹配数量；保留 `CHK_ConditionEnabled`。
- 结果行 `WBP_GCS_TelemetryFilterResultRow`：`CHK_ResultSelected` 控制对应 DroneID 的选择；保留 `TXT_ResultDroneID` 和 `TXT_ResultValue`。
- 条件列表使用 Single，结果列表保持 None。无人机多选由复选框和权威选择集合管理，不使用 ListView 的另一套选择状态。

## 行为

初次创建或重新绑定面板时，默认“当前条件”，编辑字段和比较符显示 `--`，数值留空并提示 `--`；未选择真实字段前，比较符和输入框禁用。普通隐藏/重新打开保留现有模式、编辑状态和条件，不需要修改 UMG 控件或事件。

| 模式（下拉顺序） | 结果范围 |
| --- | --- |
| 当前条件（默认） | 单独满足当前选中已保存条件的无人机，选中行改变立即刷新；不受该行启用复选框影响 |
| 任一满足 | 满足至少一条已启用有效条件 |
| 全部满足 | 同时满足所有已启用有效条件 |

- “新增”始终创建并选中一条 `0 -- -- --` 空白条件，不复制编辑区内容。
- “应用条件”有选中行时更新该行；未选中行时创建并选中新行。全空白允许保存，也能把已有条件恢复为空白；不完整或格式错误的非空条件会报错并保留原条件。
- 选中空白行会重置编辑区，并在当前条件模式下清空结果。空白行独立计数为 0，组合模式忽略它；没有有效参与条件时三种模式均显示空结果。
- 编辑草稿在点击应用前不改变已保存条件、结果集合或显示字段；结果值跟随选中已保存条件的字段。
- 选中条件行使用 sRGB `#2E7D5B` 深绿色背景（RGB 46、125、91）和白色文字，不透明度为 1；未选中行为 `#263A41` 背景和白字。运行时使用中性笔刷，避免与设计器深色着色相乘。

1. 每条条件的数量独立计算，不受其他条件以及 AND/OR 模式影响。停用条件仍显示独立数量，但不参与组合筛选。沿用原有数据有效性与新鲜度规则；缺失、过期的数值不匹配。在线字段可以匹配已知离线状态，断链时在线状态为未知。
2. 下方“匹配 / 总计”表示当前模式筛选结果 / 当前遥测表数据行数，不代表所有已发现但尚未上报遥测表数据的飞机。
3. 逐架勾选与主界面机群多选共用 `SelectedCommandDroneIDs`；地图选择高亮沿用现有桥接逻辑。不改变用于详情查看的 `CurrentSelectedDroneID`。
4. 全选一次性操作当前全部匹配 ID，包含离屏行；取消全选只移除当前匹配 ID，保留结果外已有选择。部分选中显示 Undetermined，再点击会补选当前结果。
5. 筛选或遥测变化不自动勾选新结果，也不自动清除离开结果的已有选择。零结果时全选框禁用并显示未勾选。
6. 复选框只改变选择集合，不提交飞行/灯光/上传命令。随后用户主动执行命令时，现有命令流程使用该选择集合。保留既有上传/工作流期间禁止修改选择的限制。
7. 行生成/回收时绑定/解绑独立条目回调，拒绝已离开结果的过期回调；刷新保留条目身份和滚动位置。关闭筛选面板不清空机群选择。

## 验证

运行 `DroneGCS.UI.TelemetryFilter` 专项，使用真实已保存的 UMG 主面板和行模板、离屏 Slate 布局与合成遥测数据。

- `AuthoredWidgets`：控件名称、空白新增/应用/再编辑、无选中行应用、无效草稿保留、选择切换立即刷新、真实行绑定、深绿色选中背景与白字、独立计数、数据过期与重绑定。
- `CurrentAndCombinedModes`：当前/任一/全部三模式、四架无人机的冲突条件与交集/并集、空白忽略、启用状态、无条件/无选中结果为空。
- `CountsAndSelection`：80 架无人机、AND/OR 独立计数、逐行选择、外部选择同步、离屏全选、单次批量更新、保留结果外选择、部分选中点击、新匹配不自动选择、过期条目回调隔离与关闭解绑。
- `IndependentToolbarToggle`：筛选按钮与日志页面独立，隐藏/恢复保留条件。
- `Model`：字段比较、输入校验、缺失值和在线状态语义。

测试进程使用 `-NullRHI`、`-ini:Game:[DSPGCS.ProductionTelemetry]:Enabled=False` 和
`-ini:Game:[DSPGCS.ProductionCommand]:Enabled=False`，不启动 Console 或连接真实无人机。
测试不能代替用户在正常窗口下对实际字号、颜色、鼠标操作和地图高亮的视觉复验。

2026-09-04：Development Editor 构建成功，最终专项 4/4 PASS，0 failed / 0 not-run。
报告：`UnrealGCS/Saved/Automation/TelemetryFilterSelectionFinal/index.json`；
日志：`UnrealGCS/Saved/Logs/TelemetryFilterSelectionFinal.log`。
2026-09-04：增加 `--` 初始编辑状态后，Development Editor 再次构建成功，专项 4/4 PASS。
本次报告：`UnrealGCS/Saved/Automation/TelemetryFilterNeutralDraft/index.json`；
日志：`UnrealGCS/Saved/Logs/TelemetryFilterNeutralDraft.log`。
2026-09-04：三模式与空白条件编辑接入后，Development Editor 构建成功，最终专项 5/5 PASS，0 failed / 0 not-run。
测试同时发现并修复清空列表后 Slate 仍保留旧选中对象的延迟问题，避免应用条件写入已移除的行。
最新报告：`UnrealGCS/Saved/Automation/TelemetryFilterThreeModesFinal/index.json`；
日志：`UnrealGCS/Saved/Logs/TelemetryFilterThreeModesFinal.log`。
2026-09-04：选中背景改为深绿色、文字改为白色；Development Editor 构建成功，专项 5/5 PASS。
报告：`UnrealGCS/Saved/Automation/TelemetryFilterGreenHighlight/index.json`。
启动阶段日志仍有 Cesium 元数据枚举初始化诊断，不属于本次筛选测试失败；本次未修改 Cesium。
