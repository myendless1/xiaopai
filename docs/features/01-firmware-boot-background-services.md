# 固件启动与后台服务

## 职责
负责设备上电后的统一初始化，并启动常驻后台服务，让小派进入可联网、可听、可被控制的状态。

## 逻辑
1. `app_main` 初始化 NVS、M5Unified、音频服务、语音状态机和表情控制器。
2. 启动灯带探测和 `start_background_services`。
3. 主循环只做触摸刷新、空闲自动暗屏和轻量轮询。

## 关键实现
- `stack-chan/main/main.cpp`: 固件入口，按顺序注册状态钩子、启动音频、初始化显示和后台任务。
- `stack-chan/main/main_command_services.inc`: `start_background_services` 在独立 boot 任务中等待网络、检查 OTA、预初始化相机、启动语音、命令、触摸、播报和久坐服务。
- `stack-chan/main/main_app_state.inc`: 保存全局任务句柄、状态标志、硬件参数和 UI 状态。

## 注意点
- 后台服务依赖 `ensure_network_ready`，网络和服务器未就绪时会循环等待。
- 相机和灯带共用 CoreS3 内部 I2C，主循环刷新触摸时会避开 `camera_owns_internal_i2c`。
- 语音、播放和命令执行之间通过全局状态与互斥锁协调，避免录音和播报抢音频设备。
