static void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA start");
        if (wifi_connect_requested) {
            ESP_LOGI(TAG, "Connecting to SSID '%s'", active_wifi_ssid.c_str());
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", event ? event->reason : -1);
        if (wifi_manual_switching) {
            return;
        }
        if (wifi_retry_count++ < kWifiRetryLimit) {
            esp_wifi_connect();
            ESP_LOGI(TAG, "Retry WiFi connection (%d/%d)", wifi_retry_count, kWifiRetryLimit);
        } else {
            xEventGroupSetBits(wifi_event_group, kWifiFailedBit);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        wifi_connect_requested = false;
        xEventGroupSetBits(wifi_event_group, kWifiConnectedBit);
    }
}

static std::string get_saved_wifi_password(const std::string& ssid);
static bool http_health_ok(const std::string& base_url);
static bool wifi_is_connected();

static bool ensure_wifi_stack_started()
{
    if (wifi_event_group == nullptr) {
        wifi_event_group = xEventGroupCreate();
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!wifi_sta_netif_created) {
        esp_netif_create_default_wifi_sta();
        wifi_sta_netif_created = true;
    }
    if (!wifi_ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        wifi_ap_netif_created = true;
    }

    if (!wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr,
                                                            nullptr));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr,
                                                            nullptr));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    }

    return true;
}

static bool save_wifi_credentials(const std::string& ssid, const std::string& password)
{
    if (ssid.empty()) {
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open wifi failed: %s", esp_err_to_name(err));
        return false;
    }

    nvs_set_str(nvs_handle, kWifiNvsSsidKey, ssid.c_str());
    nvs_set_str(nvs_handle, kWifiNvsPasswordKey, password.c_str());
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return err == ESP_OK;
}

static bool load_saved_wifi_credentials(std::string& ssid, std::string& password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    char ssid_buf[33] = {};
    char password_buf[65] = {};
    size_t ssid_len = sizeof(ssid_buf);
    size_t password_len = sizeof(password_buf);
    err = nvs_get_str(nvs_handle, kWifiNvsSsidKey, ssid_buf, &ssid_len);
    if (err == ESP_OK) {
        esp_err_t pass_err = nvs_get_str(nvs_handle, kWifiNvsPasswordKey, password_buf, &password_len);
        if (pass_err != ESP_OK) {
            password_buf[0] = '\0';
        }
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK || ssid_buf[0] == '\0') {
        return false;
    }

    ssid = ssid_buf;
    password = password_buf;
    return true;
}

static std::string get_saved_wifi_password(const std::string& ssid)
{
    std::string saved_ssid;
    std::string saved_password;
    if (load_saved_wifi_credentials(saved_ssid, saved_password) && saved_ssid == ssid) {
        return saved_password;
    }

    for (const auto& candidate : kWifiCandidates) {
        if (candidate.ssid != nullptr && ssid == candidate.ssid) {
            return candidate.password != nullptr ? std::string(candidate.password) : std::string();
        }
    }
    return std::string();
}

static std::string normalize_server_base(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }).base(),
                value.end());
    if (value.empty()) {
        return value;
    }
    if (value.rfind("http://", 0) != 0 && value.rfind("https://", 0) != 0) {
        value = "http://" + value;
    }
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

static bool save_server_base(const std::string& base)
{
    if (base.empty()) {
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open server failed: %s", esp_err_to_name(err));
        return false;
    }

    nvs_set_str(nvs_handle, kServerNvsBaseKey, base.c_str());
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return err == ESP_OK;
}

static bool load_saved_server_base(std::string& base)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    char base_buf[96] = {};
    size_t base_len = sizeof(base_buf);
    err = nvs_get_str(nvs_handle, kServerNvsBaseKey, base_buf, &base_len);
    nvs_close(nvs_handle);
    if (err != ESP_OK || base_buf[0] == '\0') {
        return false;
    }

    base = normalize_server_base(base_buf);
    return !base.empty();
}

static void stop_provisioning_portal()
{
    httpd_handle_t httpd = provisioning_httpd;
    provisioning_httpd = nullptr;
    provisioning_started = false;
    provisioning_stop_pending = false;

    if (httpd != nullptr) {
        esp_err_t err = httpd_stop(httpd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Provisioning HTTP stop failed: %s", esp_err_to_name(err));
        }
    }

    if (wifi_started && wifi_is_connected()) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Provisioning AP stopped; WiFi mode is STA");
        } else {
            ESP_LOGW(TAG, "Failed to switch WiFi mode to STA: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "Provisioning portal stopped without STA connection");
    }
}

static void schedule_stop_provisioning_portal()
{
    if (!provisioning_started || provisioning_stop_pending) {
        return;
    }
    provisioning_stop_pending = true;
    BaseType_t created = xTaskCreatePinnedToCore([](void*) {
        vTaskDelay(pdMS_TO_TICKS(kProvisioningStopDelayMs));
        stop_provisioning_portal();
        vTaskDelete(nullptr);
    }, "prov_stop", kProvisioningStopTaskStackBytes, nullptr, 2, nullptr, 0);
    if (created != pdPASS) {
        provisioning_stop_pending = false;
        ESP_LOGW(TAG, "Failed to schedule provisioning portal stop");
    }
}

static bool select_server_base(const std::string& requested_base, bool persist, bool show_success_expression = true)
{
    std::string base = normalize_server_base(requested_base);
    if (!base.empty()) {
        ESP_LOGI(TAG, "Testing server base: %s", base.c_str());
        if (http_health_ok(base)) {
            active_server_base = base;
            active_server_selected = true;
            if (persist) {
                save_server_base(base);
            }
            ESP_LOGI(TAG, "Server selected: %s", active_server_base.c_str());
            if (show_success_expression) {
                current_app = AppId::VoiceDemo;
                voice_status_screen_suppressed = true;
                show_expression("calm");
            }
            schedule_stop_provisioning_portal();
            return true;
        }

        active_server_selected = false;
        ESP_LOGW(TAG, "Server health failed: %s", base.c_str());
        return false;
    }

    return false;
}

static bool is_known_wifi_ssid(const std::string& ssid)
{
    std::string saved_ssid;
    std::string saved_password;
    if (load_saved_wifi_credentials(saved_ssid, saved_password) && saved_ssid == ssid) {
        return true;
    }
    for (const auto& candidate : kWifiCandidates) {
        if (candidate.ssid != nullptr && ssid == candidate.ssid) {
            return true;
        }
    }
    return false;
}

static bool configure_wifi_credentials(const std::string& ssid, const std::string& password)
{
    if (ssid.empty()) {
        return false;
    }
    active_wifi_ssid = ssid;

    wifi_config_t wifi_config = {};
    snprintf(reinterpret_cast<char*>(wifi_config.sta.ssid), sizeof(wifi_config.sta.ssid), "%s", ssid.c_str());
    snprintf(reinterpret_cast<char*>(wifi_config.sta.password), sizeof(wifi_config.sta.password), "%s",
             password.c_str());
    wifi_config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed for %s: %s", ssid.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool connect_wifi_credentials(const std::string& ssid, const std::string& password, bool persist)
{
    if (!ensure_wifi_stack_started()) {
        return false;
    }

    ESP_LOGI(TAG, "Connecting WiFi credentials: %s", ssid.c_str());
    xEventGroupClearBits(wifi_event_group, kWifiConnectedBit | kWifiFailedBit);
    wifi_retry_count = 0;
    wifi_connect_requested = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(provisioning_started ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    if (!configure_wifi_credentials(ssid, password)) {
        wifi_connect_requested = false;
        return false;
    }

    if (!wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    } else {
        wifi_manual_switching = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        wifi_manual_switching = false;
        esp_wifi_connect();
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, kWifiConnectedBit | kWifiFailedBit, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(18000));
    if (bits & kWifiConnectedBit) {
        active_wifi_ssid = ssid;
        active_server_selected = false;
        if (persist) {
            save_wifi_credentials(ssid, password);
        }
        ESP_LOGI(TAG, "WiFi connected: %s", active_wifi_ssid.c_str());
        return true;
    }

    wifi_connect_requested = false;
    ESP_LOGW(TAG, "WiFi connect failed: %s", ssid.c_str());
    return false;
}

static std::string json_escape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}

static std::string make_provisioning_ap_ssid()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[24];
    snprintf(ssid, sizeof(ssid), "Xiaopai-%02X%02X", mac[4], mac[5]);
    return std::string(ssid);
}

static std::string make_server_options_json()
{
    std::string saved_base;
    bool has_saved = load_saved_server_base(saved_base);
    std::string json = "\"savedServer\":\"";
    json += json_escape(has_saved ? saved_base : active_server_base);
    json += "\",\"servers\":[";
    bool first = true;
    if (has_saved) {
        json += "\"";
        json += json_escape(saved_base);
        json += "\"";
        first = false;
    }
    for (const char* base : kServerBaseCandidates) {
        if (base == nullptr || strlen(base) == 0) {
            continue;
        }
        std::string normalized = normalize_server_base(base);
        if (has_saved && normalized == saved_base) {
            continue;
        }
        if (!first) {
            json += ',';
        }
        json += "\"";
        json += json_escape(normalized);
        json += "\"";
        first = false;
    }
    json += "]";
    return json;
}

static const char* provisioning_page_html()
{
    return R"rawliteral(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>小派同学 WiFi 连接界面</title>
<style>
:root{color-scheme:dark}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#111820;color:#eef3f7}
main{max-width:560px;margin:0 auto;padding:24px 16px 36px}
main.done{min-height:100vh;display:grid;place-items:center;padding:0 20px;text-align:center;font-size:24px;font-weight:750;color:#36c98f}
h1{font-size:25px;margin:6px 0 4px}
.sub{color:#a8b6c2;font-size:14px;margin:0 0 18px}
.panel{border:1px solid #2f3f4b;border-radius:8px;background:#17212b;padding:14px;margin:12px 0}
.status{min-height:22px;color:#8bd3ff;margin:10px 0 0;line-height:1.45}
.net,.server{display:flex;align-items:center;justify-content:space-between;gap:12px;border:1px solid #32424f;border-radius:8px;padding:12px;margin:8px 0;background:#101821}
.selected{border-color:#36c98f;background:#14241f}
.ssid,.server-name{font-size:16px;font-weight:650;overflow:hidden;text-overflow:ellipsis}
.meta{color:#a5b1bc;font-size:13px;margin-top:3px}
button{border:0;border-radius:8px;padding:10px 14px;background:#36c98f;color:#06110c;font-weight:750;font-size:15px}
button.secondary{background:#2b3540;color:#eef3f7}
button.full{width:100%;margin-top:12px}
label{display:block;color:#b9c5ce;font-size:13px;margin:10px 0 5px}
input{box-sizing:border-box;width:100%;border:1px solid #3d4b57;border-radius:8px;background:#0b1117;color:#eef3f7;padding:12px;font-size:16px}
dialog{border:1px solid #42515d;border-radius:8px;background:#151c24;color:#eef3f7;width:min(92vw,420px)}
dialog::backdrop{background:rgba(0,0,0,.55)}
.row{display:flex;gap:10px;justify-content:flex-end;margin-top:14px}
.hint{font-size:13px;color:#96a6b3;line-height:1.45}
</style>
</head>
<body>
<main>
<h1>小派同学 WiFi 连接界面</h1>
<p class="sub">先选择 WiFi，再选择本地服务地址。连接成功后小派同学会自动继续启动。</p>
<section class="panel">
  <strong>1. 选择 WiFi</strong>
  <div class="status" id="status">正在扫描附近 WiFi...</div>
  <button class="secondary" onclick="scan()">重新扫描</button>
  <div id="list"></div>
</section>
<section class="panel">
  <strong>2. 选择服务器</strong>
  <p class="hint">请选择电脑上的小派同学服务地址，或输入新的 IP + 端口，例如 192.168.1.23:8091。</p>
  <div id="servers"></div>
  <label for="customServer">自定义服务器</label>
  <input id="customServer" placeholder="192.168.1.23:8091">
</section>
<button class="full" onclick="connectSelected()">连接 WiFi 并检测服务器</button>
</main>
<dialog id="dlg">
<form method="dialog" onsubmit="event.preventDefault(); saveWifiDialog();">
<h2 id="dlgTitle">连接 WiFi</h2>
<input id="ssid" autocomplete="off" placeholder="SSID">
<input id="password" autocomplete="current-password" type="password" placeholder="密码">
<div class="row">
<button class="secondary" type="button" onclick="dlg.close()">取消</button>
<button type="submit">连接</button>
</div>
</form>
</dialog>
<script>
const list=document.getElementById('list'),serversEl=document.getElementById('servers'),statusEl=document.getElementById('status'),dlg=document.getElementById('dlg'),customServer=document.getElementById('customServer');
let selectedWifi=null,selectedServer='',savedServers=[];
function esc(s){return String(s||'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function normalizeServer(s){s=String(s||'').trim();if(!s)return'';if(!/^https?:\/\//i.test(s))s='http://'+s;return s.replace(/\/+$/,'');}
function showReady(){document.body.innerHTML='<main class="done">可以与小派同学互动啦~</main>';}
async function scan(){
  statusEl.textContent='正在扫描附近 WiFi...';
  const r=await fetch('/scan');
  const data=await r.json();
  if(data.connected&&data.serverOk){showReady();return;}
  savedServers=data.servers||[];
  selectedServer=data.savedServer||savedServers[0]||'';
  customServer.value=selectedServer;
  list.innerHTML='';
  for(const ap of data.aps){
    const row=document.createElement('div');
    row.className='net';
    row.innerHTML='<div><div class="ssid">'+esc(ap.ssid)+'</div><div class="meta">'+ap.rssi+' dBm '+(ap.known?'已保存，可直接连接':'需要输入密码')+'</div></div>';
    const b=document.createElement('button');
    b.textContent='选择';
    b.onclick=()=>selectWifi(ap.ssid,'',ap.known,row);
    row.appendChild(b);
    list.appendChild(row);
  }
  if(!data.aps.length) list.innerHTML='<p>没有扫描到 WiFi。</p>';
  const manual=document.createElement('div');
  manual.className='net';
  manual.innerHTML='<div><div class="ssid">手动输入隐藏网络</div><div class="meta">输入 SSID 和密码</div></div>';
  const mb=document.createElement('button');
  mb.textContent='输入';
  mb.onclick=()=>openDialog('');
  manual.appendChild(mb);
  list.appendChild(manual);
  renderServers();
  statusEl.textContent=data.connected?'当前已连接 '+data.connected+'，也可以重新选择网络':'请选择要连接的 WiFi';
}
function renderServers(){
  serversEl.innerHTML='';
  for(const base of savedServers){
    const row=document.createElement('div');
    row.className='server'+(normalizeServer(base)===normalizeServer(selectedServer)?' selected':'');
    row.innerHTML='<div><div class="server-name">'+esc(base)+'</div><div class="meta">点击使用这个服务地址</div></div>';
    const b=document.createElement('button');
    b.textContent='选择';
    b.onclick=()=>{selectedServer=normalizeServer(base);customServer.value=selectedServer;renderServers();};
    row.appendChild(b);
    serversEl.appendChild(row);
  }
}
function selectWifi(s,p,known,row){
  selectedWifi={ssid:s,password:p,known};
  document.querySelectorAll('.net').forEach(x=>x.classList.remove('selected'));
  if(row)row.classList.add('selected');
  statusEl.textContent='已选择 WiFi：'+s;
  if(!known)openDialog(s);
}
function openDialog(s){ssid.value=s||'';password.value='';dlg.showModal();setTimeout(()=>s?password.focus():ssid.focus(),50);}
function saveWifiDialog(){selectedWifi={ssid:ssid.value,password:password.value,known:false};dlg.close();statusEl.textContent='已选择 WiFi：'+ssid.value;}
async function connectSelected(){
  if(!selectedWifi){alert('请先选择 WiFi。');return;}
  const serverBase=normalizeServer(customServer.value||selectedServer);
  if(!serverBase){alert('请输入服务器 IP 和端口，例如 192.168.1.23:8091。');customServer.focus();return;}
  statusEl.textContent='正在连接 WiFi 并检测服务器...';
  const r=await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({...selectedWifi,serverBase})});
  const data=await r.json();
  if(!data.ok){alert('WiFi 连接失败，请重新输入 WiFi 密码。');openDialog(selectedWifi.ssid);statusEl.textContent='WiFi 连接失败';return;}
  if(!data.serverOk){alert('服务器连接失败，请重新输入 IP + 端口。');customServer.focus();statusEl.textContent='WiFi 已连接，但服务器不可用';return;}
  showReady();
}
scan();
</script>
</body>
</html>
)rawliteral";
}

static esp_err_t provisioning_index_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, provisioning_page_html());
}

static esp_err_t provisioning_scan_handler(httpd_req_t* req)
{
    if (!ensure_wifi_stack_started()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi init failed");
        return ESP_FAIL;
    }

    wifi_scan_config_t scan_config = {};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(err));
    }

    uint16_t ap_count = kProvisioningMaxScanResults;
    wifi_ap_record_t records[kProvisioningMaxScanResults] = {};
    esp_wifi_scan_get_ap_records(&ap_count, records);

    std::string json = "{\"connected\":\"";
    json += wifi_is_connected() ? json_escape(active_wifi_ssid) : "";
    json += "\",\"serverOk\":";
    json += (wifi_is_connected() && active_server_selected) ? "true" : "false";
    json += ",\"aps\":[";
    for (uint16_t i = 0; i < ap_count; ++i) {
        std::string ssid(reinterpret_cast<char*>(records[i].ssid));
        if (ssid.empty()) {
            continue;
        }
        if (json.back() != '[') {
            json += ',';
        }
        json += "{\"ssid\":\"";
        json += json_escape(ssid);
        json += "\",\"rssi\":";
        json += std::to_string(records[i].rssi);
        json += ",\"known\":";
        json += is_known_wifi_ssid(ssid) ? "true" : "false";
        json += "}";
    }
    json += "],";
    json += make_server_options_json();
    json += "}";

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.size());
}

static bool read_http_body(httpd_req_t* req, std::string& body)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        return false;
    }

    body.resize(req->content_len);
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, &body[received], req->content_len - received);
        if (ret <= 0) {
            return false;
        }
        received += ret;
    }
    return true;
}

static esp_err_t provisioning_connect_handler(httpd_req_t* req)
{
    std::string body;
    if (!read_http_body(req, body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_FAIL;
    }

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    cJSON* ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON* password_item = cJSON_GetObjectItem(root, "password");
    cJSON* known_item = cJSON_GetObjectItem(root, "known");
    cJSON* server_base_item = cJSON_GetObjectItem(root, "serverBase");
    std::string ssid = cJSON_IsString(ssid_item) ? ssid_item->valuestring : "";
    std::string password = cJSON_IsString(password_item) ? password_item->valuestring : "";
    std::string server_base = cJSON_IsString(server_base_item) ? server_base_item->valuestring : "";
    bool known = cJSON_IsTrue(known_item);
    cJSON_Delete(root);

    if (ssid.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_FAIL;
    }
    if (known && password.empty()) {
        password = get_saved_wifi_password(ssid);
    }

    bool ok = connect_wifi_credentials(ssid, password, true);
    bool server_ok = false;
    std::string normalized_server = normalize_server_base(server_base);
    if (ok) {
        server_ok = select_server_base(normalized_server, true);
    }
    std::string response = "{\"ok\":";
    response += ok ? "true" : "false";
    response += ",\"serverOk\":";
    response += server_ok ? "true" : "false";
    response += ",\"ssid\":\"";
    response += json_escape(ssid);
    response += "\",\"serverBase\":\"";
    response += json_escape(server_ok ? active_server_base : normalized_server);
    response += "\"}";

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response.c_str(), response.size());
}

static bool start_provisioning_portal()
{
    if (provisioning_started) {
        return true;
    }
    if (!ensure_wifi_stack_started()) {
        return false;
    }

    std::string ap_ssid = make_provisioning_ap_ssid();
    wifi_config_t ap_config = {};
    snprintf(reinterpret_cast<char*>(ap_config.ap.ssid), sizeof(ap_config.ap.ssid), "%s", ap_ssid.c_str());
    snprintf(reinterpret_cast<char*>(ap_config.ap.password), sizeof(ap_config.ap.password), "%s",
             kProvisioningApPassword);
    ap_config.ap.ssid_len = ap_ssid.size();
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = false;
    if (strlen(kProvisioningApPassword) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    if (!wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    }

    if (provisioning_httpd == nullptr) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.lru_purge_enable = true;
        config.stack_size = 6144;
        ESP_ERROR_CHECK(httpd_start(&provisioning_httpd, &config));

        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = provisioning_index_handler,
            .user_ctx = nullptr,
        };
        httpd_uri_t scan_uri = {
            .uri = "/scan",
            .method = HTTP_GET,
            .handler = provisioning_scan_handler,
            .user_ctx = nullptr,
        };
        httpd_uri_t connect_uri = {
            .uri = "/connect",
            .method = HTTP_POST,
            .handler = provisioning_connect_handler,
            .user_ctx = nullptr,
        };
        httpd_register_uri_handler(provisioning_httpd, &index_uri);
        httpd_register_uri_handler(provisioning_httpd, &scan_uri);
        httpd_register_uri_handler(provisioning_httpd, &connect_uri);
    }

    provisioning_started = true;
    {
        M5Lock lock;
        draw_wifi_setup_steps(ap_ssid.c_str());
    }
    return true;
}

static bool configure_wifi_candidate(const WifiCandidate& candidate)
{
    if (candidate.ssid == nullptr || strlen(candidate.ssid) == 0) {
        return false;
    }
    return configure_wifi_credentials(candidate.ssid, candidate.password ? candidate.password : "");
}

static bool wifi_is_connected()
{
    if (!wifi_started) {
        return false;
    }

    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return wifi_event_group != nullptr && (xEventGroupGetBits(wifi_event_group) & kWifiConnectedBit) != 0;
    }

    if (wifi_event_group != nullptr) {
        EventBits_t bits = xEventGroupGetBits(wifi_event_group);
        return (bits & kWifiConnectedBit) != 0;
    }
    return false;
}

static bool ensure_wifi_connected(bool allow_connect, bool force_candidate_scan = false, int start_candidate_index = 0)
{
    ensure_wifi_stack_started();

    if (!force_candidate_scan && wifi_is_connected()) {
        ESP_LOGI(TAG, "WiFi already connected: %s", active_wifi_ssid.c_str());
        return true;
    }

    if (!allow_connect) {
        ESP_LOGW(TAG, "WiFi is not connected; network app will not reconnect automatically");
        return false;
    }

    std::string saved_ssid;
    std::string saved_password;
    if (!force_candidate_scan && load_saved_wifi_credentials(saved_ssid, saved_password)) {
        return connect_wifi_credentials(saved_ssid, saved_password, false);
    }

    for (int offset = 0; offset < kWifiCandidateCount; ++offset) {
        const int candidate_index = (start_candidate_index + offset + kWifiCandidateCount) % kWifiCandidateCount;
        const WifiCandidate& candidate = kWifiCandidates[candidate_index];
        if (candidate.ssid == nullptr || strlen(candidate.ssid) == 0) {
            continue;
        }
        ESP_LOGI(TAG, "Trying WiFi candidate: %s", candidate.ssid);
        xEventGroupClearBits(wifi_event_group, kWifiConnectedBit | kWifiFailedBit);
        wifi_retry_count = 0;
        if (wifi_started) {
            wifi_manual_switching = true;
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(300));
            wifi_manual_switching = false;
        }
        if (!configure_wifi_candidate(candidate)) {
            continue;
        }

        wifi_connect_requested = true;
        ESP_ERROR_CHECK(esp_wifi_set_mode(provisioning_started ? WIFI_MODE_APSTA : WIFI_MODE_STA));
        if (!wifi_started) {
            ESP_ERROR_CHECK(esp_wifi_start());
            wifi_started = true;
        } else {
            esp_wifi_connect();
        }

        EventBits_t bits = xEventGroupWaitBits(wifi_event_group, kWifiConnectedBit | kWifiFailedBit, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(16000));
        if (bits & kWifiConnectedBit) {
            active_wifi_ssid = candidate.ssid;
            active_wifi_candidate_index = candidate_index;
            active_server_selected = false;
            ESP_LOGI(TAG, "WiFi candidate connected: %s", active_wifi_ssid.c_str());
            return true;
        }
        wifi_connect_requested = false;
        ESP_LOGW(TAG, "WiFi candidate failed: %s", candidate.ssid);
    }

    ESP_LOGE(TAG, "WiFi connection failed or timed out");
    active_wifi_candidate_index = -1;
    return false;
}

static bool ensure_wifi_connected()
{
    return ensure_wifi_connected(false);
}

void run_wifi_connect_app()
{
    ensure_client_id();
    if (start_provisioning_portal()) {
        if (wifi_is_connected()) {
            ESP_LOGI(TAG, "Provisioning portal remains open, WiFi already connected: %s", active_wifi_ssid.c_str());
        }
    } else {
        set_wifi_status("Setup", "Could not start portal", "", "", false, false);
    }
}

static std::string make_server_url(const char* path)
{
    std::string url = active_server_base;
    if (!url.empty() && url.back() == '/' && path != nullptr && path[0] == '/') {
        url.pop_back();
    }
    url += path ? path : "";
    return url;
}

static bool http_health_ok(const std::string& base_url)
{
    std::string url = base_url + "/health";
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 3500;
    config.buffer_size = 512;
    config.buffer_size_tx = 512;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "Server health %s -> err=%s status=%d", url.c_str(), esp_err_to_name(err), status);
    return err == ESP_OK && status >= 200 && status < 300;
}

static bool ensure_server_selected()
{
    if (active_server_selected && http_health_ok(active_server_base)) {
        return true;
    }

    active_server_selected = false;
    ESP_LOGW(TAG, "Server is not selected or health check failed");
    return false;
}

static bool try_saved_network_ready()
{
    std::string saved_ssid;
    std::string saved_password;
    if (!wifi_is_connected() && load_saved_wifi_credentials(saved_ssid, saved_password)) {
        if (!connect_wifi_credentials(saved_ssid, saved_password, false)) {
            ESP_LOGW(TAG, "Saved WiFi auto-connect failed: %s", saved_ssid.c_str());
            return false;
        }
    }

    if (!wifi_is_connected()) {
        return false;
    }

    std::string saved_server;
    if (load_saved_server_base(saved_server)) {
        if (select_server_base(saved_server, false, false)) {
            return true;
        }
        ESP_LOGW(TAG, "Saved server auto-select failed: %s", saved_server.c_str());
    }

    return active_server_selected && http_health_ok(active_server_base);
}

static bool ensure_network_ready()
{
    if (!provisioning_started && !start_provisioning_portal()) {
        set_current_network_status("Setup", "Could not start portal", "", "", false, false);
        return false;
    }

    if (try_saved_network_ready()) {
        return true;
    }

    while (!wifi_is_connected() || !active_server_selected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    return true;
}
