# DroneGCS Project Handoff

> 用途：新 ChatGPT / Codex 对话快速接手当前项目。
>
> 架构最高准则：
> `Architecture_Freeze_CN.md`
>
> 当前真实代码主文档：
> `Code_Architecture_CN.md`
>
> 如果本文与 Architecture_Freeze_CN.md 或当前实际代码冲突，
> 以 Architecture_Freeze_CN.md + 当前代码为准。



## 当前阶段

Cleanup 3A~3H 已结束。
Architecture Freeze 已建立。
后续进入正式功能开发。

## 当前稳定能力

- Console多无人机Endpoint Discovery
- Remote IP + Source Port Identity
- Online / Offline / Recovery
- MAVLink GPS / Mode / Command / ACK
- DSP-GCS Console ↔ Unreal
- 1000 Drone Snapshot / Chunk
- UE Fleet
- Telemetry Link Lost
- Cesium GPS定位
- 单Niagara千机可视化
- UI单选详情 + 多选Command目标

## 当前规模

正式稳定基线：
1000 Drone

探索测试：
10000 Session / 10000 UE Fleet
但只有约3000 Online稳定。

10K性能优化暂缓。

## 当前主要文档

最高准则：
Docs/Architecture_Freeze_CN.md

当前代码：
Docs/Code_Architecture_CN.md

Production职责：
Docs/Production_Responsibility_Audit_CN.md

Tests：
Docs/Test_Architecture_CN.md

历史资料：
Docs/History/

## 当前下一步

停止Cleanup。
开始无人机表演平台正式功能开发。

## 开发规则

不要主动重构冻结边界。
不要把History当当前架构。
不要为了减少文件数量修改Production。
先检查真实调用链，再修改。
小步修改，小步验证。

## 当前开发阶段

Cleanup 已结束。
Architecture Freeze 已建立。

当前进入：
无人机表演平台正式功能开发。

## 最近完成

- Production Architecture Freeze
- Legacy / Test / Mock / Acceptance 收敛
- 1000 Drone 稳定基线
- Console ↔ UE DSP-GCS Production 链完成
- Fleet / Cesium / Niagara 主链完成

## 下一步

待确定当前正式功能开发任务。