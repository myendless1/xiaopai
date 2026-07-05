# 相机上传与视觉跟踪

## 职责
采集 CoreS3 相机帧，上传到本地服务器做人脸检测，并可根据检测结果调整头部朝向。

## 逻辑
1. 初始化 GC0308 相机为 QVGA RGB565。
2. 拍摄前丢弃旧帧，检查帧大小和明显绿色坏帧。
3. 上传帧到 `/upload-image`，携带格式、宽高、设备 ID 和是否允许服务器自动跟踪。
4. 解析服务器 `face_detection.best_face`，得到人脸中心坐标。
5. 根据像素偏差和相机内参估算 yaw/pitch 修正。

## 关键实现
- `stack-chan/main/main_camera_motion.inc`: `init_camera_once`、`capture_frame_copy`、`upload_camera_frame_only`、`upload_tracking_frame`、`parse_face_target`。
- `run_camera_upload_app`: 单次拍照上传。
- `run_tracking_user_demo`: 多姿态扫描并选择最佳候选。

## 注意点
- 相机占用内部 I2C 时，灯带和触摸刷新会避让。
- `upload_tracking_frame` 对寻人路径设置 `X-Visual-Tracking: false`，防止服务器和固件同时控制头部。
- RGB565 按大端字节发送，服务器转换和 YuNet 检测也按该约定解析。
