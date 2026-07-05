#include "dji_mic_receiver_input.h"

#include <M5Unified.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "usb/usb_host.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifndef CONFIG_STACKCHAN_DJI_MIC_USB_INPUT
#define CONFIG_STACKCHAN_DJI_MIC_USB_INPUT 0
#endif

namespace {

static constexpr const char* TAG = "DjiMicEnum";
static constexpr uint16_t kDjiVid = 0x2ca3;
static constexpr uint16_t kDjiPid = 0x4011;
static constexpr uint8_t kAudioControlSubclass = 0x01;
static constexpr uint8_t kAudioStreamingSubclass = 0x02;
static constexpr uint8_t kCsInterfaceDescriptor = 0x24;
static constexpr uint8_t kAsFormatTypeSubtype = 0x02;
static constexpr int kUsbScanIntervalMs = 1000;
static constexpr int kNoUsbDeviceScanLogIntervalMs = 5000;

static const char* speed_name(usb_speed_t speed)
{
    switch (speed) {
        case USB_SPEED_LOW:
            return "Low-Speed";
        case USB_SPEED_FULL:
            return "Full-Speed";
        case USB_SPEED_HIGH:
            return "High-Speed";
        default:
            return "Unknown";
    }
}

static const char* audio_subclass_name(uint8_t subclass)
{
    switch (subclass) {
        case kAudioControlSubclass:
            return "AudioControl";
        case kAudioStreamingSubclass:
            return "AudioStreaming";
        default:
            return "AudioOther";
    }
}

static const char* transfer_type_name(uint8_t attributes)
{
    switch (attributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) {
        case USB_BM_ATTRIBUTES_XFER_CONTROL:
            return "control";
        case USB_BM_ATTRIBUTES_XFER_ISOC:
            return "isochronous";
        case USB_BM_ATTRIBUTES_XFER_BULK:
            return "bulk";
        case USB_BM_ATTRIBUTES_XFER_INT:
            return "interrupt";
        default:
            return "unknown";
    }
}

static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

static bool ascii_contains_i(const char* haystack, const char* needle)
{
    if (haystack == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }
    for (size_t i = 0; haystack[i] != '\0'; ++i) {
        size_t j = 0;
        while (needle[j] != '\0' && haystack[i + j] != '\0' &&
               ascii_lower(haystack[i + j]) == ascii_lower(needle[j])) {
            ++j;
        }
        if (needle[j] == '\0') {
            return true;
        }
    }
    return false;
}

static void usb_string_to_ascii(const usb_str_desc_t* desc, char* out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (desc == nullptr || desc->bLength <= 2) {
        return;
    }
    size_t chars = (desc->bLength - 2) / sizeof(uint16_t);
    size_t pos = 0;
    for (size_t i = 0; i < chars && pos + 1 < out_size; ++i) {
        uint16_t wc = desc->wData[i];
        out[pos++] = (wc >= 0x20 && wc <= 0x7e) ? static_cast<char>(wc) : '?';
    }
    out[pos] = '\0';
}

static int read_le24_sample_rate(const uint8_t* data)
{
    return static_cast<int>(data[0]) | (static_cast<int>(data[1]) << 8) | (static_cast<int>(data[2]) << 16);
}

static void append_hex(char* out, size_t out_size, const uint8_t* bytes, size_t len)
{
    if (out == nullptr || out_size == 0) {
        return;
    }
    out[0] = '\0';
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 4 < out_size; ++i) {
        int written = snprintf(out + pos, out_size - pos, "%s%02x", i == 0 ? "" : " ", bytes[i]);
        if (written <= 0) {
            break;
        }
        pos += static_cast<size_t>(written);
    }
}

struct EnumSummary {
    bool target_vid_pid = false;
    bool full_speed = false;
    bool audio_control = false;
    bool audio_streaming = false;
    int sample_rate = 0;
    int channels = 0;
};

class DjiMicReceiverEnumerator {
public:
    bool start()
    {
#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT
        if (started_.exchange(true)) {
            return true;
        }
        BaseType_t ok = xTaskCreatePinnedToCore(
            [](void* arg) {
                static_cast<DjiMicReceiverEnumerator*>(arg)->task_main();
                vTaskDelete(nullptr);
            },
            "dji_usb_enum", 8192, this, 3, &task_handle_, 0);
        if (ok != pdPASS) {
            set_detail("USB枚举任务创建失败");
            started_ = false;
            return false;
        }
        return true;
#else
        set_detail("disabled by Kconfig");
        return false;
#endif
    }

    DjiMicReceiverStatus status() const
    {
        DjiMicReceiverStatus status;
        status.detected = detected_.load();
        status.target_vid_pid = target_vid_pid_.load();
        status.full_speed = full_speed_.load();
        status.audio_control = audio_control_.load();
        status.audio_streaming = audio_streaming_.load();
        status.capture_ready = capture_ready_.load();
        status.identity_confirmed = identity_confirmed_.load();
        status.vendor_id = vendor_id_.load();
        status.product_id = product_id_.load();
        status.sample_rate = sample_rate_.load();
        status.channels = channels_.load();
        status.speed = speed_;
        status.manufacturer = manufacturer_;
        status.product = product_;
        status.detail = detail_;
        return status;
    }

    size_t read_16k(int16_t*, size_t, TickType_t)
    {
        return 0;
    }

private:
    static void client_event_cb(const usb_host_client_event_msg_t* event_msg, void* arg)
    {
        auto* self = static_cast<DjiMicReceiverEnumerator*>(arg);
        if (self == nullptr || event_msg == nullptr) {
            return;
        }
        if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
            self->pending_dev_addr_ = event_msg->new_dev.address;
            self->ignored_dev_addr_ = 0;
            ESP_LOGI(TAG, "USB设备已连接: addr=%u", static_cast<unsigned>(event_msg->new_dev.address));
        } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
            if (event_msg->dev_gone.dev_hdl == self->dev_hdl_) {
                self->device_gone_ = true;
            }
        }
    }

    void task_main()
    {
        ESP_LOGI(TAG, "DJI Mic枚举模式启动: 只读descriptor，不claim接口，不提交音频传输");
        ESP_LOGI(TAG, "CoreS3 USB-C将作为Host使用；请用Wi-Fi、屏幕或额外UART看日志");
        ESP_LOGI(TAG, "正在打开CoreS3 USB VBUS输出");
        M5.Power.setUsbOutput(true);
        vTaskDelay(pdMS_TO_TICKS(200));

        usb_host_config_t host_config = {};
        host_config.skip_phy_setup = false;
        host_config.root_port_unpowered = false;
        host_config.intr_flags = 0;
        esp_err_t err = usb_host_install(&host_config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            set_detail("USB Host安装失败: %s", esp_err_to_name(err));
            ESP_LOGE(TAG, "%s", detail_);
            return;
        }
        ESP_LOGI(TAG, "USB Host已初始化 reused=%d", err == ESP_ERR_INVALID_STATE ? 1 : 0);

        usb_host_client_config_t client_config = {};
        client_config.is_synchronous = false;
        client_config.max_num_event_msg = 8;
        client_config.async.client_event_callback = client_event_cb;
        client_config.async.callback_arg = this;
        err = usb_host_client_register(&client_config, &client_hdl_);
        if (err != ESP_OK) {
            set_detail("USB Host client注册失败: %s", esp_err_to_name(err));
            ESP_LOGE(TAG, "%s", detail_);
            return;
        }

        set_detail("VBUS已打开，等待DJI Mic USB设备");
        TickType_t last_scan_ticks = 0;
        while (true) {
            uint32_t event_flags = 0;
            usb_host_lib_handle_events(pdMS_TO_TICKS(10), &event_flags);
            usb_host_client_handle_events(client_hdl_, pdMS_TO_TICKS(10));

            uint8_t addr = pending_dev_addr_.exchange(0);
            if (addr != 0 && dev_hdl_ == nullptr) {
                inspect_device(addr, "connect");
            }
            if (device_gone_.exchange(false)) {
                close_device();
            }

            TickType_t now = xTaskGetTickCount();
            if (dev_hdl_ == nullptr &&
                (last_scan_ticks == 0 || now - last_scan_ticks >= pdMS_TO_TICKS(kUsbScanIntervalMs))) {
                scan_connected_devices(last_scan_ticks == 0 ? "boot" : "poll");
                last_scan_ticks = now;
            }
        }
    }

    void scan_connected_devices(const char* reason)
    {
        if (client_hdl_ == nullptr || dev_hdl_ != nullptr) {
            return;
        }
        uint8_t dev_addr_list[8] = {};
        int num_devices = 0;
        esp_err_t err = usb_host_device_addr_list_fill(
            static_cast<int>(sizeof(dev_addr_list)), dev_addr_list, &num_devices);
        if (err != ESP_OK) {
            set_detail("USB设备扫描失败: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "%s", detail_);
            return;
        }
        if (num_devices <= 0) {
            ignored_dev_addr_ = 0;
            set_detail("未检测到USB设备；检查VBUS 5V、数据线、USB角色和日志口");
            TickType_t now = xTaskGetTickCount();
            if (last_no_device_scan_log_ticks_ == 0 ||
                now - last_no_device_scan_log_ticks_ >= pdMS_TO_TICKS(kNoUsbDeviceScanLogIntervalMs)) {
                last_no_device_scan_log_ticks_ = now;
                ESP_LOGI(TAG, "USB扫描: reason=%s 未检测到设备", reason != nullptr ? reason : "-");
            }
            return;
        }

        bool saw_ignored_device = false;
        for (int i = 0; i < num_devices && i < static_cast<int>(sizeof(dev_addr_list)); ++i) {
            uint8_t addr = dev_addr_list[i];
            if (addr == 0) {
                continue;
            }
            if (addr == ignored_dev_addr_) {
                saw_ignored_device = true;
                continue;
            }
            ESP_LOGI(TAG, "USB扫描: reason=%s addr=%u count=%d",
                     reason != nullptr ? reason : "-", static_cast<unsigned>(addr), num_devices);
            inspect_device(addr, reason);
            if (dev_hdl_ != nullptr) {
                return;
            }
        }
        if (!saw_ignored_device) {
            ignored_dev_addr_ = 0;
        }
    }

    void inspect_device(uint8_t addr, const char* reason)
    {
        usb_device_handle_t dev = nullptr;
        esp_err_t err = usb_host_device_open(client_hdl_, addr, &dev);
        if (err != ESP_OK) {
            set_detail("USB设备打开失败: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "USB设备打开失败: addr=%u reason=%s err=%s",
                     static_cast<unsigned>(addr), reason != nullptr ? reason : "-", esp_err_to_name(err));
            return;
        }

        usb_device_info_t dev_info = {};
        const usb_device_desc_t* device_desc = nullptr;
        const usb_config_desc_t* config_desc = nullptr;
        err = usb_host_device_info(dev, &dev_info);
        esp_err_t dev_desc_err = usb_host_get_device_descriptor(dev, &device_desc);
        esp_err_t cfg_desc_err = usb_host_get_active_config_descriptor(dev, &config_desc);
        if (err != ESP_OK || dev_desc_err != ESP_OK || cfg_desc_err != ESP_OK ||
            device_desc == nullptr || config_desc == nullptr) {
            set_detail("USB描述符读取失败");
            ESP_LOGW(TAG, "USB描述符读取失败: addr=%u info=%s dev=%s cfg=%s",
                     static_cast<unsigned>(addr), esp_err_to_name(err), esp_err_to_name(dev_desc_err),
                     esp_err_to_name(cfg_desc_err));
            usb_host_device_close(client_hdl_, dev);
            return;
        }

        usb_string_to_ascii(dev_info.str_desc_manufacturer, manufacturer_, sizeof(manufacturer_));
        usb_string_to_ascii(dev_info.str_desc_product, product_, sizeof(product_));
        snprintf(speed_, sizeof(speed_), "%s", speed_name(dev_info.speed));

        EnumSummary summary;
        summary.target_vid_pid = device_desc->idVendor == kDjiVid && device_desc->idProduct == kDjiPid;
        summary.full_speed = dev_info.speed == USB_SPEED_FULL;

        log_device_descriptor(addr, dev_info, device_desc);
        log_config_descriptor(config_desc, summary);
        apply_summary(*device_desc, summary);

        ESP_LOGI(TAG, "DJI Mic匹配结果: vid_pid=%d full_speed=%d audio_control=%d audio_streaming=%d dev=%04x:%04x speed=%s manufacturer=%s product=%s",
                 summary.target_vid_pid ? 1 : 0,
                 summary.full_speed ? 1 : 0,
                 summary.audio_control ? 1 : 0,
                 summary.audio_streaming ? 1 : 0,
                 static_cast<unsigned>(device_desc->idVendor),
                 static_cast<unsigned>(device_desc->idProduct),
                 speed_, manufacturer_[0] != '\0' ? manufacturer_ : "-",
                 product_[0] != '\0' ? product_ : "-");

        if (summary.target_vid_pid) {
            dev_hdl_ = dev;
            ignored_dev_addr_ = 0;
            if (summary.full_speed && summary.audio_control && summary.audio_streaming) {
                set_detail("DJI Mic枚举成功：2CA3:4011 Full-Speed AC/AS");
                ESP_LOGI(TAG, "DJI Mic枚举成功: VID/PID=2CA3:4011 speed=Full-Speed AudioControl=1 AudioStreaming=1");
            } else {
                set_detail("DJI Mic已连接，但descriptor未完全匹配");
                ESP_LOGW(TAG, "DJI Mic枚举未完全匹配: full_speed=%d ac=%d as=%d",
                         summary.full_speed ? 1 : 0,
                         summary.audio_control ? 1 : 0,
                         summary.audio_streaming ? 1 : 0);
            }
        } else {
            ignored_dev_addr_ = addr;
            usb_host_device_close(client_hdl_, dev);
            set_detail("检测到非DJI目标USB设备；等待2CA3:4011");
        }
    }

    void log_device_descriptor(uint8_t addr, const usb_device_info_t& info, const usb_device_desc_t* desc)
    {
        if (desc == nullptr) {
            return;
        }
        ESP_LOGI(TAG, "Device descriptor: addr=%u speed=%s config=%u mps0=%u manufacturer=%s product=%s",
                 static_cast<unsigned>(addr), speed_name(info.speed),
                 static_cast<unsigned>(info.bConfigurationValue),
                 static_cast<unsigned>(info.bMaxPacketSize0),
                 manufacturer_[0] != '\0' ? manufacturer_ : "-",
                 product_[0] != '\0' ? product_ : "-");
        ESP_LOGI(TAG, "Device descriptor: bLength=%u bDescriptorType=0x%02x bcdUSB=0x%04x class=0x%02x subclass=0x%02x protocol=0x%02x bMaxPacketSize0=%u",
                 static_cast<unsigned>(desc->bLength),
                 static_cast<unsigned>(desc->bDescriptorType),
                 static_cast<unsigned>(desc->bcdUSB),
                 static_cast<unsigned>(desc->bDeviceClass),
                 static_cast<unsigned>(desc->bDeviceSubClass),
                 static_cast<unsigned>(desc->bDeviceProtocol),
                 static_cast<unsigned>(desc->bMaxPacketSize0));
        ESP_LOGI(TAG, "Device descriptor: idVendor=0x%04x idProduct=0x%04x bcdDevice=0x%04x iManufacturer=%u iProduct=%u iSerialNumber=%u bNumConfigurations=%u",
                 static_cast<unsigned>(desc->idVendor),
                 static_cast<unsigned>(desc->idProduct),
                 static_cast<unsigned>(desc->bcdDevice),
                 static_cast<unsigned>(desc->iManufacturer),
                 static_cast<unsigned>(desc->iProduct),
                 static_cast<unsigned>(desc->iSerialNumber),
                 static_cast<unsigned>(desc->bNumConfigurations));
    }

    void log_config_descriptor(const usb_config_desc_t* config_desc, EnumSummary& summary)
    {
        if (config_desc == nullptr) {
            return;
        }
        ESP_LOGI(TAG, "Configuration descriptor: bLength=%u bDescriptorType=0x%02x wTotalLength=%u bNumInterfaces=%u bConfigurationValue=%u iConfiguration=%u bmAttributes=0x%02x bMaxPower=%umA",
                 static_cast<unsigned>(config_desc->bLength),
                 static_cast<unsigned>(config_desc->bDescriptorType),
                 static_cast<unsigned>(config_desc->wTotalLength),
                 static_cast<unsigned>(config_desc->bNumInterfaces),
                 static_cast<unsigned>(config_desc->bConfigurationValue),
                 static_cast<unsigned>(config_desc->iConfiguration),
                 static_cast<unsigned>(config_desc->bmAttributes),
                 static_cast<unsigned>(config_desc->bMaxPower * 2));

        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(config_desc);
        size_t offset = 0;
        const size_t total = config_desc->wTotalLength;
        bool in_audio_streaming = false;
        while (offset + 2 <= total) {
            uint8_t len = bytes[offset];
            uint8_t type = bytes[offset + 1];
            if (len < 2 || offset + len > total) {
                ESP_LOGW(TAG, "Configuration descriptor parse stopped: offset=%u len=%u total=%u",
                         static_cast<unsigned>(offset), static_cast<unsigned>(len),
                         static_cast<unsigned>(total));
                break;
            }

            const uint8_t* desc = bytes + offset;
            char raw[128];
            append_hex(raw, sizeof(raw), desc, len);
            if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= sizeof(usb_intf_desc_t)) {
                const auto* intf = reinterpret_cast<const usb_intf_desc_t*>(desc);
                bool is_audio = intf->bInterfaceClass == USB_CLASS_AUDIO;
                bool is_ac = is_audio && intf->bInterfaceSubClass == kAudioControlSubclass;
                bool is_as = is_audio && intf->bInterfaceSubClass == kAudioStreamingSubclass;
                summary.audio_control = summary.audio_control || is_ac;
                summary.audio_streaming = summary.audio_streaming || is_as;
                in_audio_streaming = is_as;
                ESP_LOGI(TAG, "Config[%03u] Interface: num=%u alt=%u eps=%u class=0x%02x%s subclass=0x%02x%s protocol=0x%02x iInterface=%u raw=%s",
                         static_cast<unsigned>(offset),
                         static_cast<unsigned>(intf->bInterfaceNumber),
                         static_cast<unsigned>(intf->bAlternateSetting),
                         static_cast<unsigned>(intf->bNumEndpoints),
                         static_cast<unsigned>(intf->bInterfaceClass),
                         is_audio ? "(Audio)" : "",
                         static_cast<unsigned>(intf->bInterfaceSubClass),
                         is_audio ? audio_subclass_name(intf->bInterfaceSubClass) : "",
                         static_cast<unsigned>(intf->bInterfaceProtocol),
                         static_cast<unsigned>(intf->iInterface),
                         raw);
            } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && len >= sizeof(usb_ep_desc_t)) {
                const auto* ep = reinterpret_cast<const usb_ep_desc_t*>(desc);
                bool in = (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;
                ESP_LOGI(TAG, "Config[%03u] Endpoint: address=0x%02x dir=%s attr=0x%02x type=%s mps=%u interval=%u raw=%s",
                         static_cast<unsigned>(offset),
                         static_cast<unsigned>(ep->bEndpointAddress),
                         in ? "IN" : "OUT",
                         static_cast<unsigned>(ep->bmAttributes),
                         transfer_type_name(ep->bmAttributes),
                         static_cast<unsigned>(USB_EP_DESC_GET_MPS(ep)),
                         static_cast<unsigned>(ep->bInterval),
                         raw);
            } else if (type == kCsInterfaceDescriptor) {
                parse_class_specific_interface(desc, len, in_audio_streaming, summary);
                ESP_LOGI(TAG, "Config[%03u] CS_INTERFACE: subtype=0x%02x raw=%s",
                         static_cast<unsigned>(offset),
                         len >= 3 ? static_cast<unsigned>(desc[2]) : 0,
                         raw);
            } else {
                ESP_LOGI(TAG, "Config[%03u] Descriptor: len=%u type=0x%02x raw=%s",
                         static_cast<unsigned>(offset),
                         static_cast<unsigned>(len),
                         static_cast<unsigned>(type),
                         raw);
            }
            offset += len;
        }
    }

    void parse_class_specific_interface(const uint8_t* desc, uint8_t len, bool in_audio_streaming,
                                        EnumSummary& summary)
    {
        if (desc == nullptr || len < 4 || !in_audio_streaming) {
            return;
        }
        if (desc[2] != kAsFormatTypeSubtype || desc[3] != 0x01) {
            return;
        }
        if (len >= 8) {
            summary.channels = desc[4];
        }
        if (len >= 11 && desc[7] > 0) {
            int rate = read_le24_sample_rate(desc + 8);
            if (rate >= 8000 && rate <= 192000) {
                summary.sample_rate = rate;
            }
        }
    }

    void apply_summary(const usb_device_desc_t& desc, const EnumSummary& summary)
    {
        vendor_id_ = desc.idVendor;
        product_id_ = desc.idProduct;
        target_vid_pid_ = summary.target_vid_pid;
        full_speed_ = summary.full_speed;
        audio_control_ = summary.audio_control;
        audio_streaming_ = summary.audio_streaming;
        capture_ready_ = false;
        identity_confirmed_ = summary.target_vid_pid ||
                              ascii_contains_i(manufacturer_, "DJI") ||
                              ascii_contains_i(product_, "DJI");
        detected_ = summary.target_vid_pid;
        sample_rate_ = summary.sample_rate;
        channels_ = summary.channels;
    }

    void close_device()
    {
        if (dev_hdl_ != nullptr && client_hdl_ != nullptr) {
            usb_host_device_close(client_hdl_, dev_hdl_);
            dev_hdl_ = nullptr;
        }
        detected_ = false;
        target_vid_pid_ = false;
        full_speed_ = false;
        audio_control_ = false;
        audio_streaming_ = false;
        capture_ready_ = false;
        identity_confirmed_ = false;
        vendor_id_ = 0;
        product_id_ = 0;
        sample_rate_ = 0;
        channels_ = 0;
        speed_[0] = '\0';
        manufacturer_[0] = '\0';
        product_[0] = '\0';
        set_detail("USB设备已断开，等待DJI Mic 2CA3:4011");
        ESP_LOGI(TAG, "%s", detail_);
    }

    void set_detail(const char* format, ...)
    {
        if (format == nullptr) {
            detail_[0] = '\0';
            return;
        }
        va_list args;
        va_start(args, format);
        vsnprintf(detail_, sizeof(detail_), format, args);
        va_end(args);
    }

    std::atomic<bool> started_{false};
    std::atomic<bool> detected_{false};
    std::atomic<bool> target_vid_pid_{false};
    std::atomic<bool> full_speed_{false};
    std::atomic<bool> audio_control_{false};
    std::atomic<bool> audio_streaming_{false};
    std::atomic<bool> capture_ready_{false};
    std::atomic<bool> identity_confirmed_{false};
    std::atomic<uint16_t> vendor_id_{0};
    std::atomic<uint16_t> product_id_{0};
    std::atomic<int> sample_rate_{0};
    std::atomic<int> channels_{0};
    std::atomic<uint8_t> pending_dev_addr_{0};
    std::atomic<bool> device_gone_{false};
    char speed_[16] = {};
    char manufacturer_[48] = {};
    char product_[64] = {};
    char detail_[128] = "未启动";
    TaskHandle_t task_handle_ = nullptr;
    usb_host_client_handle_t client_hdl_ = nullptr;
    usb_device_handle_t dev_hdl_ = nullptr;
    uint8_t ignored_dev_addr_ = 0;
    TickType_t last_no_device_scan_log_ticks_ = 0;
};

DjiMicReceiverEnumerator g_dji_mic_receiver;

} // namespace

bool dji_mic_receiver_input_start()
{
    return g_dji_mic_receiver.start();
}

DjiMicReceiverStatus dji_mic_receiver_input_status()
{
    return g_dji_mic_receiver.status();
}

size_t dji_mic_receiver_input_read_16k(int16_t* out, size_t samples, TickType_t timeout)
{
    (void)out;
    (void)samples;
    (void)timeout;
    return 0;
}
