# 登录灯光轮播

当前运行资源为 `UnrealGCS/Content/Movies/Login/{Saturn,Wing,Tide}.mp4`，分别对应星环、光翼、光潮。它们是原创数学灯阵离线渲染，不是实际飞行轨迹或任务文件。

## 重新烘焙

`render_loops.py` 需要 Python、numpy、Pillow、scipy、imageio-ffmpeg。当前开发机额外依赖放在被忽略的 `BuildLogs/LoginRenderDeps`，脚本会自动加入搜索路径。不要将该依赖目录打包进程序。

```powershell
& 'C:/Users/13092/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' Tools/LoginMedia/render_loops.py --theme all
```

每段 1280×800、24 fps、8 秒、H.264/yuv420p、无音频；每个循环排除重复末帧，并检查首尾周期几何一致。星环是缓慢往复转动和灯光流动；光翼是波动的抽象编队，光潮是流动光带。并不保证视觉与概念图像素一致。

封面与 manifest 输出到 `Tools/Art/Login/Loops`，视频直接输出到项目 Content/Movies/Login。修改封面后，运行 UE Python 脚本 `UnrealGCS/Scripts/import_login_media.py`，更新 `/Game/GCS/Login/Media` 的三张 UI 纹理及加法材质。

## 播放与回退

- `UGCSLoginMedia` 持有一个播放器、一个视频纹理、三个封面材质。
- 横移时下一页先显示封面，横移结束后打开下一段视频；准备好后 0.3 秒淡入。没有三个同时解码的视频。
- 播放失败或准备超时保留封面；移除登录 Widget 时关闭播放器、解除视频纹理关联。
- `-LoginStaticShow` 启动参数强制静态。也可在 GameUserSettings.ini 设置 `[MD.Login.Presentation] StaticShow=True`。静态模式仍支持轮播与手动切换。
- 视频通过 RuntimeDependencies 以 NonUFS 文件随包发布；`/Game/GCS/Login` 已配置 AlwaysCook。发布前仍需在打包程序中验证平台解码器。

## 开发验证

在 `L_GCS_Login` 的独立游戏窗口运行 `GCS.Login.MediaCheck`，约 28 秒后自行退出。检查三段视频打开、暂停、换页、静态回退、恢复和 Widget 移除后关闭解码器，并保存实际截图及帧间隔统计至 `Saved/Automation/LoginMedia`。此命令不登录、不发送飞行命令。帧间隔是该机器该次运行的观测，不是跨设备性能保证。
