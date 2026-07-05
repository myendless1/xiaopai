# 固件 OTA 升级

## 职责
服务器发布最新 ESP-IDF app 固件，设备检查版本、下载到 inactive OTA 分区并重启切换。

## 逻辑
1. 服务器 `/realtime/config` 或 `/xiaozhi/ota` 返回 `firmware` 字段。
2. 设备解析版本、URL、大小、sha256 和 force 标记。
3. 比较当前 app version 与广告版本，或 `force` 为真时继续。
4. 下载固件，写入 `esp_ota_get_next_update_partition`，校验结束后设为 boot partition。
5. 显示进度并自动重启。

## 关键实现
- `stack-chan/main/main_firmware_ota.inc`: `check_and_apply_firmware_ota_once`、版本比较、下载写入和重启。
- `stack-chan/stack-chan-server/src/server.py`: `find_latest_ota_firmware`、`ota_firmware_manifest`、`/firmware/latest.json`、`/firmware/<bin>`。
- `stack-chan/build_and_publish_ota.sh`: 构建并发布固件到 `stack-chan-server/static/firmware/`。

## 注意点
- 需要 OTA-capable 分区表；旧 factory-only 设备必须先 USB 刷机一次。
- 版本号要求数字点分格式，便于服务端和设备端稳定比较。
- 设备当前代码未校验 sha256，只依赖 ESP-IDF OTA image validation 和大小检查。
