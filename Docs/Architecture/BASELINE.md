# Phase 0 基线索引

Status: COMPLETE WITH DOCUMENTED LIMITATIONS；2026-09-09；master HEAD `a95309d8b37ecb00c779d5c4264aacb86294d8ec`。Phase 1 NOT STARTED。本文件沿用路线图规定的BASELINE.md名称，只作索引，不重复七份专题内容。

| 基线 | 内容 |
|---|---|
| [10_BUILD_BASELINE](10_BUILD_BASELINE.md) | Git、真实构建命令/工具链、5项构建结果与warning |
| [11_TEST_BASELINE](11_TEST_BASELINE.md) | 全测试入口盘点、A–E分类、每次运行/每个UE用例耗时、首轮失败/复跑记录 |
| [12_COMMAND_BASELINE](12_COMMAND_BASELINE.md) | 25命令入口、DSP/MAV ID、参数、Endpoint目标、ACK/timeout/retry/结果 |
| [13_TELEMETRY_BASELINE](13_TELEMETRY_BASELINE.md) | 字段来源/投影/格式化、线程与样本Authority |
| [14_UPLOAD_BASELINE](14_UPLOAD_BASELINE.md) | confirmed目标、Freeze/Accepted/AutoStart、幂等、cap、MAVFTP/verification |
| [15_MULTI_VEHICLE_BASELINE](15_MULTI_VEHICLE_BASELINE.md) | Endpoint身份、零基映射、selection与confirmed差别 |
| [16_UI_BEHAVIOR_BASELINE](16_UI_BEHAVIOR_BASELINE.md) | 当前耦合与关键工作流、生命周期、人工验收缺口 |

## 证据标签

STATIC_BASELINE：当前HEAD源码确认。TESTED_BASELINE：本轮对应断言已执行通过，仅适用于测试覆盖范围。FIELD_BASELINE_REQUIRED：仍需飞控/真机/现场确认，不能用ACK或fake替代。人工UI验证另外明确标记。

5项构建成功；Console 6次运行全部通过，Launcher Smoke 1次/32检查通过；最终UE全量242项加Synthetic1项，共243项全部成功。UE首轮239/242、3项配置相关失败保留记录；最终复跑242/242并没有修改生产代码。真实NPZ fixture性能测试、手工联合Acceptance、真机/安装包等缺口见11。Cesium反射错误、默认地图依赖warning、MSVC非preferred/C4819/C4244警告未修复。

## 可比较清单

[PHASE0_EVIDENCE.json](PHASE0_EVIDENCE.json)保存HEAD、762项源码/配置SHA256、69个跟踪资源Git blob、640个反射声明位置及邻近源码、185个UE注册入口、Console函数盘点、构建运行时间、完整UE测试报告（含首轮诊断）。反射清单是文本基线，不声称等同UHT生成API全量解析。生成/第三方代码的hash只用于内容对照，不表示完成其业务审计。既有CommandTests/Shared wire round-trip的fixture源码包含在hash内，未创建第二套packet encoder。

仓库根运行：

```powershell
python Docs/Architecture/verify_phase0.py
```

本轮输出：762 source/config + 69 resources，PASS。helper只读、不运行程序/网络；逐字节差异只用于定位待review变更，未来正常重构可以产生DIFF，不能将hash相同/不同等同完整行为等价。它不枚举后续新增文件；后续仍须git status/diff配合检查。资源hash使用Git clean过滤，不写object。

原始大日志/临时artifact在ignored `BuildLogs/ArchitecturePhase0-20260909`、`UnrealGCS/Saved/Automation/ArchitecturePhase0-20260909*`和Launcher的隔离Smoke目录；摘要与逐项结果在JSON/Markdown中可随文档保存。没有commit/stage，没有改源码/生产配置/.uasset，没有启动真机上传或调整飞控参数。

## Phase 1前风险

测试配方需启用依赖Provider的case并验证实际回环端口及生产进程不存在；首轮禁用配方不可再当全量PASS基准。最终result丢失/事件重入/全部Bind交错并未穷尽测试。armed虽到DSP但未完整投影UE Fleet；Confirmed mapping优先于当前勾选；零基slot、现有特殊命令复合packet顺序、恢复遥测新#187和MAVFTP后Mission verification均是后续不可漂移边界。稳定跨重启ID、硬件规模、Drone001/007硬件关机排查继续延期。开始Phase1须用户另行授权。
