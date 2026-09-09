# Phase 0：多机身份与映射

基线：`a95309d8b37ecb00c779d5c4264aacb86294d8ec`（master，2026-09-09）。本阶段不改变生产源码、协议、配置或资产。执行结果见 [测试基线](11_TEST_BASELINE.md)；静态结论不等于真机验证。

## 1. 当前规则（STATIC_BASELINE）

唯一网络身份=Remote IP + UDP Source Port。HEARTBEAT首次发现时DroneManager按进程内顺序生成Drone000起的ID；离线保留Session，Console重启后稳定ID方案未实现。相同SYSID的两个Endpoint仍是两架机；同IP不同端口也不能合并。

`Shared/DroneIdentity/DroneLogicalId.h::TryDroneLogicalIdToRouteSlot` 当前直接赋值 `routeSlotIndex = numericId`，并要求 `< routeSlotCount`。规范格式由formatter/parser roundtrip控制，零合法，有范围/溢出检查。

| DroneID | NumericDroneID | 默认RouteSlot | 前提 |
|---|---:|---:|---|
| Drone000 | 0 | 0 | routeCount≥1 |
| Drone001 | 1 | 1 | routeCount≥2 |
| Drone007 | 7 | 7 | routeCount≥8 |

旧交接中的 `NumericDroneID - 1` 不适用于当前HEAD。SYSID、RouteSlot、Niagara particle/display ID都不是DroneID。

## 2. 两种上传目标来源

未有confirmed mapping：CaptureAutoStartSnapshot读取命令选择，校验每DroneID并取默认零基slot。

已有confirmed NPZ mapping：先验证当前dataset/domain和mapping revision，目标集合替换为confirmed Entries中的DroneID（排序），slot取Entries，不再随当前UI勾选变更而替换。示例：confirmed Entry为Drone007→slot2，后将UI改选Drone001，上传仍使用Drone007→2（其余冻结门禁仍须通过）；普通命令仍使用Drone001。这是两条已存在Authority的用途区别，不能通过统一selection悄悄改写。

详情选择由Fleet当前对象维护；命令选择由UIBridge `SelectedCommandDroneIDs`维护；筛选不等于勾选。Upload重试/快照查询不能重新用当前UI选项生成新目标。

## 3. 后续变更边界

本轮什么语义都不变。未来批准的结构重构可以改变类/函数内部组织、纯转换helper的位置；对外ID规范、分配顺序、Endpoint隔离、零基规则、confirmed mapping revision/优先级、三类选择状态、冻结后目标不可变都必须保持。

稳定跨重启ID、重分配、备用机、补点均需独立需求。不能添加第二套RouteAssignment或网络Authority。自动化通过仅证明fixture中的映射与路由，E.2多机硬件规模仍延期。

证据：[DroneLogicalId](../../Shared/DroneIdentity/DroneLogicalId.h)、[DroneManager](../../ConsoleGCS/DroneManager.cpp)、[Coordinator](../../UnrealGCS/Source/DroneShowPlatform1/Private/GCSRouteWorkflowCoordinator.inl)、[DroneLogicalIdTests](../../ConsoleGCS/tests/DroneLogicalIdTests.cpp)、[Coordinator tests](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSRouteWorkflowCoordinatorTests.inl)、[Selection tests](../../UnrealGCS/Source/DroneShowPlatform1/Private/Tests/GCSSelectionStateTests.cpp)。实际TESTED_BASELINE范围以 [11](11_TEST_BASELINE.md) 为准。
