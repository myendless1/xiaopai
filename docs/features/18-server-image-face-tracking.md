# 图像转换、人脸检测与跟踪命令

## 职责
接收设备相机帧，完成 RGB565/YUV422/JPEG 解码、人脸检测，并可将人脸偏差转换为头部运动命令。

## 逻辑
1. `/upload-image` 读取 `X-Image-Format`、宽高、设备 ID 和 `X-Visual-Tracking`。
2. 根据格式校验尺寸并转换为检测输入。
3. 优先使用 OpenCV YuNet 检测；legacy 模式回退 `face_recognition`。
4. 选取最大人脸为 `best_face`。
5. 若允许自动跟踪，计算人脸中心相对画面中心的像素误差，超过 deadzone 则入队 `motion` 或 `sequence` 命令。

## 关键实现
- `stack-chan/stack-chan-server/src/server.py`: `_handle_upload_image`、`_maybe_enqueue_visual_tracking`、RGB565/YUV422/PNG/BMP 转换。
- `stack-chan/stack-chan-server/src/yunet_service.py`: YuNet 模型加载、RGB565 大端转 BGR、YUV422 转 BGR、检测结果标准化。

## 注意点
- 默认 `capture_save_mode=none`，仅 debug/raw 模式保存图片。
- 自动跟踪有 pending 数量、最小间隔和 deadzone 限制，防止命令堆积。
- 固件寻人路径会禁用服务器自动跟踪，由固件自行根据检测坐标移动。
