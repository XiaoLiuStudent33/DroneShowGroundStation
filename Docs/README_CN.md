# DroneGCS Documentation Index

Status: CURRENT
Authority: 文档导航、职责和Source-of-Truth索引
Last Updated: 2026-08-14
Applies To: 仓库全部Markdown

## START HERE

推荐阅读顺序：

1. [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md) — 不能违反的长期边界。
2. [Code_Architecture_CN.md](Code_Architecture_CN.md) — 当前代码如何运行。
3. [Development_Status_CN.md](Development_Status_CN.md) — DONE/NEXT/DEFERRED。
4. 当前任务相关专项Architecture。
5. Audit作为证据。
6. `History/`仅作历史参考。

## Architecture Authorities

- [Architecture_Freeze_CN.md](Architecture_Freeze_CN.md)
- [Code_Architecture_CN.md](Code_Architecture_CN.md)
- [Network_Architecture_CN.md](Network_Architecture_CN.md)
- [Visualization_Architecture_CN.md](Visualization_Architecture_CN.md)
- [UI_Architecture_CN.md](UI_Architecture_CN.md)
- [Route_Upload_Architecture_CN.md](Route_Upload_Architecture_CN.md)
- [Test_Architecture_CN.md](Test_Architecture_CN.md)

## Current Status

- [Development_Status_CN.md](Development_Status_CN.md)
- [PROJECT_HANDOFF_CN.md](PROJECT_HANDOFF_CN.md)

## Audits

- [Production_Responsibility_Audit_CN.md](Production_Responsibility_Audit_CN.md)
- [Audit_Legacy_BGC2_RouteUpload_CN.md](Audit_Legacy_BGC2_RouteUpload_CN.md)
- [Audit_DroneGCS_RouteUpload_Gap_CN.md](Audit_DroneGCS_RouteUpload_Gap_CN.md)

Audit是证据/快照，不是Architecture Authority。

## History

`History/`文件全部为`STATUS: HISTORICAL`。不得用于判断当前Production状态。

## Source-of-Truth Matrix

| Topic | Primary Document |
|---|---|
| Overall Architecture | `Architecture_Freeze_CN.md` |
| Current Code Chain | `Code_Architecture_CN.md` |
| Fleet Identity | `Network_Architecture_CN.md` |
| DSP Protocol | `Network_Architecture_CN.md` |
| UI | `UI_Architecture_CN.md` |
| Visualization | `Visualization_Architecture_CN.md` |
| Route Data | `Code_Architecture_CN.md`，Route Preview章节 |
| Route Assignment | `Route_Upload_Architecture_CN.md` |
| Waypoint Artifact | `Route_Upload_Architecture_CN.md` |
| Route Upload | `Route_Upload_Architecture_CN.md` |
| Firmware Compatibility | `Route_Upload_Architecture_CN.md`，Firmware章节 |
| Current Development Status | `Development_Status_CN.md` |
| Test Architecture | `Test_Architecture_CN.md` |
| Production Responsibility Rationale | `Production_Responsibility_Audit_CN.md` |
| History classification/index | `README_CN.md`，History章节；单个历史文件无当前Authority |

## Complete Markdown Inventory

Recommended action枚举：KEEP、UPDATE、MERGE、MOVE-TO-HISTORY、NEW-REPLACEMENT、DELETE-DUPLICATE。本轮没有删除或移动文件。

| Path | Title | Purpose | Last relevant content | Old status | Action |
|---|---|---|---|---|---|
| `README.md` | DroneShowGroundStation | 仓库入口 | 工程与Docs导航 | Current but stale | UPDATE |
| `Docs/Architecture_Freeze_CN.md` | DroneGCS Architecture Freeze | 最高架构约束 | Route domain长期边界 | Current but incomplete | UPDATE |
| `Docs/Code_Architecture_CN.md` | DroneGCS当前代码架构 | 当前运行链 | Route/LED/UI/Visualization事实 | Stale in later sections | UPDATE |
| `Docs/Production_Responsibility_Audit_CN.md` | Production职责审计 | Cleanup阶段职责证据 | Production/Developer边界 | Audit snapshot | UPDATE metadata |
| `Docs/PROJECT_HANDOFF_CN.md` | Project Handoff | 快速交接 | 当前阶段和入口 | Stale | UPDATE |
| `Docs/Test_Architecture_CN.md` | 测试架构 | 测试入口 | Console/UE/Acceptance | Current reference | UPDATE metadata |
| `Docs/README_CN.md` | Documentation Index | 文档导航与矩阵 | 本表与Source-of-Truth | Missing | NEW-REPLACEMENT |
| `Docs/Development_Status_CN.md` | Development Status | DONE/NEXT/DEFERRED | Phase 2 roadmap | Missing | NEW-REPLACEMENT |
| `Docs/UI_Architecture_CN.md` | UI Architecture | UMG/C++ binding Authority | RouteImport/Playback/UI semantics | Scattered | NEW-REPLACEMENT |
| `Docs/Visualization_Architecture_CN.md` | Visualization Architecture | Cesium/Niagara/selection | Live/Preview与Overlay边界 | Scattered | NEW-REPLACEMENT |
| `Docs/Network_Architecture_CN.md` | Network & DSP Architecture | Identity/network/DSP | shared sender与future FTP reply | Scattered | NEW-REPLACEMENT |
| `Docs/Route_Upload_Architecture_CN.md` | Route Upload Architecture | 正式规划Authority | Assignment->Artifact->Upload | Missing | NEW-REPLACEMENT |
| `Docs/Audit_Legacy_BGC2_RouteUpload_CN.md` | Legacy BGC2 Route Upload Audit | Legacy证据 | Source facts/inference/unknown | Not landed | NEW-REPLACEMENT |
| `Docs/Audit_DroneGCS_RouteUpload_Gap_CN.md` | DroneGCS Route Upload Gap Audit | 当前差距证据 | Gap matrix与blockers | Not landed | NEW-REPLACEMENT |
| `Docs/History/00-current-state-review.md` | 当前工程现状审查 | 早期导入/加固历史 | 早期Console状态 | History but unlabeled | UPDATE banner |
| `Docs/History/Blender_GCS_Analysis.md` | Blender旧地面站参考分析 | 旧业务参考 | BGC2早期分析 | History but unlabeled | UPDATE banner |
| `Docs/History/DroneShowPlatform_Architecture_V5.0.md` | 架构审查V5.0 | 早期设计建议 | Phase 1建议 | History but unlabeled | UPDATE banner |
| `Docs/History/DroneShowPlatform_Communication_Protocol_V1.0.md` | DSP协议设计V1.0 | 早期协议设计 | 设计阶段结论 | History but unlabeled | UPDATE banner |
| `Tools/unreal-mcp/.github/copilot-instructions.md` | UnrealMCP Python guidelines | vendored tool instructions | tool coding guidelines | External tool | KEEP |
| `Tools/unreal-mcp/Docs/README.md` | Unreal Engine MCP Documentation | tool docs index | MCP docs links | External tool | KEEP |
| `Tools/unreal-mcp/Docs/Tools/README.md` | Unreal MCP Tools | tool index | actor/blueprint/editor/node links | External tool | KEEP |
| `Tools/unreal-mcp/Docs/Tools/actor_tools.md` | Unreal MCP Editor Tools | actor tool API | actor operations | External tool | KEEP |
| `Tools/unreal-mcp/Docs/Tools/blueprint_tools.md` | Unreal MCP Blueprint Tools | blueprint tool API | blueprint operations | External tool | KEEP |
| `Tools/unreal-mcp/Docs/Tools/editor_tools.md` | Unreal MCP Editor Tools | editor tool API | editor operations | External tool | KEEP |
| `Tools/unreal-mcp/Docs/Tools/node_tools.md` | Unreal MCP Node Tools | node tool API | node operations | External tool | KEEP |
| `Tools/unreal-mcp/MCPGameProject/Docs/REFACTOR_COMMANDS.md` | UnrealMCP Refactoring Plan | vendored tool history | tool refactor plan | External tool history | KEEP |
| `Tools/unreal-mcp/Python/README.md` | Unreal MCP | vendored runtime instructions | Python server setup | External tool | KEEP |
| `Tools/unreal-mcp/README.md` | Model Context Protocol for Unreal Engine | vendored tool root readme | UnrealMCP usage | External tool | KEEP |
| `Tools/unreal-mcp/Python/.venv/Lib/site-packages/httpcore-1.0.7.dist-info/licenses/LICENSE.md` | httpcore license | dependency license | license text | Third-party license | KEEP |
| `Tools/unreal-mcp/Python/.venv/Lib/site-packages/httpx-0.28.1.dist-info/licenses/LICENSE.md` | httpx license | dependency license | license text | Third-party license | KEEP |
| `Tools/unreal-mcp/Python/.venv/Lib/site-packages/idna-3.10.dist-info/LICENSE.md` | idna license | dependency license | license text | Third-party license | KEEP |
| `Tools/unreal-mcp/Python/.venv/Lib/site-packages/starlette-0.46.1.dist-info/licenses/LICENSE.md` | starlette license | dependency license | license text | Third-party license | KEEP |
| `Tools/unreal-mcp/Python/.venv/Lib/site-packages/uvicorn-0.34.0.dist-info/licenses/LICENSE.md` | uvicorn license | dependency license | license text | Third-party license | KEEP |

说明：任务开始扫描得到25个Markdown；新增8个项目文档后表中共33项。`Tools/unreal-mcp`和`.venv`属于vendor/tool/license边界，本轮不改。

## Documentation Matrix

| File | New Responsibility | Authority Level | Implemented/Planned coverage |
|---|---|---|---|
| Architecture Freeze | 长期边界 | Highest | Implemented constraints + frozen directions |
| Code Architecture | 当前源码运行链 | Current code authority | Implemented only |
| Network Architecture | Identity/network/DSP | Subsystem authority | Implemented + explicit future FTP hook |
| Visualization Architecture | Cesium/Niagara/input | Subsystem authority | Implemented + planned Overlay |
| UI Architecture | Widget/C++ binding | Subsystem authority | Implemented + planned Route Operations UI |
| Route Upload Architecture | Route upload design | Planned architecture authority | Planned/Deferred/Unknown |
| Development Status | 当前状态 | Status authority | DONE/NEXT/DEFERRED |
| Audits | 证据和差距 | Reference only | Facts/inference/unknown |
| History | 历史上下文 | None for current | Historical only |

## Maintenance Rules

- Architecture文件不写逐日开发日志。
- Status文件不重复完整调用链。
- Audit不得宣称自己是Authority。
- History必须保留Historical banner。
- 新功能完成后同时更新Code Architecture、对应专项Architecture和Development Status。
- 如果文档与Production源码冲突，先按源码修正文档；不要为保留旧文档而扭曲事实。
