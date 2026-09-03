# DroneGCS Documentation Index

Status: CURRENT SUPPORTING
Authority: 文档导航、分类与 Source-of-Truth 索引
Last Updated: 2026-09-03

## 1. START HERE

推荐阅读顺序：

1. [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md) — 不能违反的长期边界。
2. [Code_Architecture_CN.md](Code_Architecture_CN.md) — 当前生产代码如何运行。
3. [Development_Status_CN.md](Development_Status_CN.md) — DONE、NEXT、NOT STARTED、DEFERRED、UNKNOWN。
4. 当前任务相关专项 Architecture。
5. [PROJECT_HANDOFF_CN.md](PROJECT_HANDOFF_CN.md) — 快速接手摘要。
6. `History/` 只作历史证据，不用于判断当前实现状态。

Source of Truth 优先级：当前生产源码、已存在并通过的测试、明确标级的人工/硬件证据、Architecture Freeze、专项 Architecture、History。

## 2. CURRENT Authority

| 文档 | 作用 |
|---|---|
| [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md) | 项目最高长期 Authority；冻结 UE/Console/DSP、Fleet identity、网络、性能和 Route domain 边界 |
| [Code_Architecture_CN.md](Code_Architecture_CN.md) | 当前 Production 调用链、模块职责与 Developer Entry Map |
| [Development_Status_CN.md](Development_Status_CN.md) | 当前阶段完成度、下一阶段、延后项、固件未知项与证据等级 |
| [Network_Architecture_CN.md](Network_Architecture_CN.md) | Remote Endpoint identity、shared socket、MAVLink ingress/outbound 与 DSP-GCS 链 |
| [Visualization_Architecture_CN.md](Visualization_Architecture_CN.md) | Fleet、Route Preview、Overlay、Cesium/Niagara、TopDown 与 Marquee |
| [UI_Architecture_CN.md](UI_Architecture_CN.md) | UMG/C++ binding、selection、Route Workflow 与 Upload Job UI |
| [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md) | RouteAssignment、Artifact、Waypoint、DSP Job、MissionTransport、MAVFTP 与下一阶段语义 |

## 3. CURRENT Supporting Documentation

Route Upload ListView 的稳定行身份与分页恢复显示合同见 `UI_Architecture_CN.md`；对应 `RouteListSnapshotLifecycle` 回归与验收边界见 `Test_Architecture_CN.md`。

启动预览与实际 Job 的区分、Artifact 校验拒绝和超时结果未知的显示合同同见 `UI_Architecture_CN.md`；不得把 `Start Sent` / `Awaiting Console` 当作已执行上传。

| 文档 | 作用 |
|---|---|
| [../README.md](../README.md) | 仓库入口、系统边界、当前生产 Transport 与阅读顺序 |
| 本文件 | 全部 Markdown 的分类与导航 |
| [PROJECT_HANDOFF_CN.md](PROJECT_HANDOFF_CN.md) | 新会话快速接手与安全边界 |
| [Test_Architecture_CN.md](Test_Architecture_CN.md) | Console/UE/Hardware 测试入口和证据边界 |
| [../Tools/Launcher/README.md](../Tools/Launcher/README.md) | 联合软件包、启动/退出、运行数据与开发机打包接入 |
| [../Tools/Launcher/VALIDATION.md](../Tools/Launcher/VALIDATION.md) | 当前联合包与安装程序验证结果 |
| [../Tools/Installer/README.md](../Tools/Installer/README.md) | Shipping 输入校验、NSIS 安装构建、分发和验收流程 |

## 4. History / Archived

以下文件不再承担当前 Authority，但保留独特历史、设计或审计证据：

| 文档 | 历史作用 |
|---|---|
| [History/00-current-state-review.md](History/00-current-state-review.md) | 2026-07-31 工程导入与早期现状审查 |
| [History/Blender_GCS_Analysis.md](History/Blender_GCS_Analysis.md) | Blender/BGC2 旧系统业务参考分析 |
| [History/DroneShowPlatform_Architecture_V5.0.md](History/DroneShowPlatform_Architecture_V5.0.md) | Phase 1 后的早期架构建议 |
| [History/DroneShowPlatform_Communication_Protocol_V1.0.md](History/DroneShowPlatform_Communication_Protocol_V1.0.md) | DSP-GCS 早期协议设计 |
| [History/Production_Responsibility_Audit_CN.md](History/Production_Responsibility_Audit_CN.md) | Cleanup/Architecture Freeze 阶段职责审计快照 |
| [History/Audit_Legacy_BGC2_RouteUpload_CN.md](History/Audit_Legacy_BGC2_RouteUpload_CN.md) | Legacy BGC2 Route Upload 源码证据与兼容性风险 |
| [History/Audit_DroneGCS_RouteUpload_Gap_CN.md](History/Audit_DroneGCS_RouteUpload_Gap_CN.md) | 2026-08-14 Route Upload 尚未实现时的 gap 快照 |
| [History/Joint_Package_V1_V2_Validation_CN.md](History/Joint_Package_V1_V2_Validation_CN.md) | 联合启动器 V1 只读版与 V2 完整控制便携版的历史验证记录 |

这些文件顶部必须保持 `ARCHIVED / HISTORICAL` 或等价历史横幅，并链接回当前 Authority。

## 5. External Tool Documentation

以下文件属于 `Tools/unreal-mcp` 外部工具边界，不是 DroneGCS Production Authority，本次保持原位：

| 文档 | 作用 |
|---|---|
| `Tools/unreal-mcp/README.md` | Unreal MCP 工具入口与使用说明 |
| `Tools/unreal-mcp/Python/README.md` | Python server/runtime 使用说明 |
| `Tools/unreal-mcp/Docs/README.md` | Unreal MCP 文档索引 |
| `Tools/unreal-mcp/Docs/Tools/README.md` | MCP tool API 索引 |
| `Tools/unreal-mcp/Docs/Tools/actor_tools.md` | Actor tool API |
| `Tools/unreal-mcp/Docs/Tools/blueprint_tools.md` | Blueprint tool API |
| `Tools/unreal-mcp/Docs/Tools/editor_tools.md` | Editor tool API |
| `Tools/unreal-mcp/Docs/Tools/node_tools.md` | Node tool API |
| `Tools/unreal-mcp/MCPGameProject/Docs/REFACTOR_COMMANDS.md` | Unreal MCP 历史重构计划 |

## 6. Source-of-Truth Matrix

| Topic | Primary Document |
|---|---|
| Long-term architecture | `Architecture_Freeze_CN.md` |
| Current code chain | `Code_Architecture_CN.md` |
| Current progress / next phase | `Development_Status_CN.md` |
| Fleet identity / network / DSP | `Network_Architecture_CN.md` |
| Visualization / Overlay | `Visualization_Architecture_CN.md` |
| UI / Route Workflow | `UI_Architecture_CN.md` |
| Route Assignment / Artifact / Upload / MAVFTP | `Route_Upload_Architecture_CN.md` |
| Test and evidence boundary | `Test_Architecture_CN.md` |
| Historical rationale | `History/`；无当前 Authority |

## 7. Inventory Classification

2026-09-03 审计得到 31 个现存 Markdown；拆分联合包历史验证后为 32 个：

- CURRENT Authority / Supporting：11。
- Launcher / Installer 当前工具文档：3。
- History / Archived：8。
- External Tool Documentation：9，位于独立 `Tools/unreal-mcp` 仓库。
- Generated Duplicate：1，即 `UnrealGCS/Launcher-README.md`；由 payload 构建生成，不手工维护。
- 本轮未删除 Markdown。历史文档统一保留在 `Docs/History/`，等待人工决定是否删除。

## 8. Maintenance Rules

- Architecture 不写逐日开发日志；Status 不复制完整调用链。
- 当前源码事实优先于旧状态文档。
- 已实现功能不得继续写成 `PLANNED`；未来功能不得写成已完成。
- 用户确认的硬件观察必须标为 `USER-CONFIRMED`，无日志时不得升级为固定协议结论。
- History 文件不得作为当前实现 Authority。
- 仓库 Markdown 使用相对链接，不写本机绝对路径和易漂移行号。
- 功能完成后同步更新 Code Architecture、对应专项 Architecture、Development Status 与本索引。
