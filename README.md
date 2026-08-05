# 无人机表演地面站

本工作区包含两个现有工程的开发副本：

- `ConsoleGCS/`：VS2022 C++ 控制台地面站及 MAVLink/UDP 通信实现。
- `UnrealGCS/`：Unreal Engine 5.5.4 前端工程。
- `Docs/`：架构、协议、测试和开发计划文档。
- `Tools/`：后续仿真、数据转换和压力测试工具。

原始工程：

- `D:\VSProject\DroneGCS1\DroneGCS`
- `D:\EpicProject\DroneShowPlatform1`

`Binaries`、`Intermediate`、`DerivedDataCache`、`Saved`、`.vs` 和本地构建输出未导入，可由 Visual Studio 或 Unreal Engine 重新生成。

## 控制台网络配置

控制台程序启动时必须读取网络配置，默认文件名为
`ConsoleGCS/gcs-config.ini`。先复制示例文件：

```powershell
Copy-Item .\ConsoleGCS\gcs-config.ini.example .\ConsoleGCS\gcs-config.ini
```

确认目标 IP、目标端口和本地端口后，在 `ConsoleGCS` 目录中运行程序。
也可以把其他配置文件路径作为第一个命令行参数传入。

`gcs-config.ini` 属于本机配置，默认不会提交到 Git。示例文件使用
`127.0.0.1`，避免未检查配置时连接真实无人机。

当前工作区已经创建了同样指向 `127.0.0.1` 的本地
`ConsoleGCS/gcs-config.ini`，可以直接调试控制台界面。连接 SITL 或测试设备前，
需要手动核对并修改该文件。

## 控制台测试

解决方案中包含 `CommandDispatcherTests` 测试项目，验证：

- 普通解锁报文的 `param2` 为 `0`。
- 匹配的 `COMMAND_ACK` 能正确完成请求。
- `LAND` 等安全指令会取消尚未执行的 `TAKEOFF`。

运行：

```powershell
.\ConsoleGCS\x64\Debug\CommandDispatcherTests.exe
```
