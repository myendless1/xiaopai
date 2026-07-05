# WiFi 配网与服务器选择

## 职责
让设备连接可用 WiFi，并选择本地 `stack-chan-server` 地址。支持已保存配置、编译期候选、驱动中已有配置和 AP 配网页。

## 逻辑
1. 启动 WiFi STA/AP 网络栈。
2. 优先尝试 NVS 保存的 WiFi 和服务器地址，再尝试编译期候选。
3. 如果无法完成连接，启动 `Xiaopai-XXXX` AP 和本地 HTTP 配网页。
4. 页面提交 WiFi 与服务器地址后，设备保存到 NVS 并关闭配网页。

## 关键实现
- `stack-chan/main/main_wifi_provisioning.inc`: WiFi 事件处理、凭据保存、服务器地址规范化、配网 HTTP 页面和 `/scan`、`/connect` 逻辑。
- `active_wifi_ssid`、`active_server_base`、`active_server_selected`: 当前网络和服务器状态。
- `http_health_ok(base)`: 通过服务端 `/health` 判断服务器是否可用。

## 注意点
- 服务器地址会自动补 `http://` 并移除末尾 `/`。
- 配网页使用 APSTA 模式，连接成功后切回 STA。
- `kProvisioningApPassword` 固定为 `12345678`，适合本地临时配置，不应暴露到不可信网络。
