# Xiaopai Firmware

ESP-IDF firmware for Xiaopai / M5Stack CoreS3.

The firmware side contains only the device application: WiFi connection, voice upload, Aliyun PCM streaming TTS playback, and camera upload.

The local HTTP server has been extracted into:

```text
stack-chan-server/
```

See [stack-chan-server/README.md](stack-chan-server/README.md) for Aliyun ASR/TTS, image upload, RGB565 conversion, and face detection visualization.

## Build And Flash

Activate ESP-IDF, then build and flash:

```bash
. ./esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

If your serial port is different, replace `/dev/ttyACM0`.

Convenience scripts:

```bash
./build_and_flash.sh
./flash.sh
```

## Build And Publish OTA Firmware

The server advertises the newest app firmware from `stack-chan-server/static/firmware/`.
Build and publish a new OTA image with a numeric dotted version:

```bash
./build_and_publish_ota.sh 0.1.1
```

The device checks the server after WiFi/server selection. If `/ota`
advertises a newer `firmware.version`, it downloads the app image, writes it to
the inactive OTA slot, sets that slot as bootable, and restarts.

Important: devices already flashed with the old single `factory` partition table
must be flashed once over USB with this OTA-capable partition table before OTA
updates can work.

## Firmware Configuration

Set these URLs to your computer's LAN IP when the local server is running:

```text
CONFIG_STACKCHAN_RECORD_UPLOAD_URL = http://<lan-ip>:8091/upload
CONFIG_STACKCHAN_STREAM_TTS_URL    = http://<lan-ip>:8091/stream-speak
CONFIG_STACKCHAN_IMAGE_UPLOAD_URL  = http://<lan-ip>:8091/upload-image
```

The Xiaopai server listens on port `8091` by default.

Hold the Xiaopai screen for three seconds to cancel the current reply, clear the
current Morrow conversation history, and start a fresh logical session. After a
successful reset Xiaopai says: `你好，我是小派，今天有什么需要帮忙的？`

### DJI Mic battery mode

The default firmware enables the DJI Mic USB UAC input and CoreS3 USB Host VBUS.
This build requires a suitable external supply whenever DJI Mic is used. Keep the
DJI receiver on the USB-C OTG host connection and provide robot power through the
separate supported power input; do not attach a PC to the active OTG port.

`CONFIG_STACKCHAN_DJI_MIC_LOW_BATTERY_PROTECTION` is disabled, so transient
battery-voltage readings are logged but do not stop DJI capture or turn off USB
VBUS. USB overcurrent safe mode remains enabled. Do not use DJI Mic from the
internal battery with this configuration.

DJI USB Host does not start automatically. This keeps Wi-Fi diagnostics available
while the USB-C port is connected to a PC. Disconnect the PC, attach DJI Mic, then
start it through the network command channel:

```bash
curl 'http://127.0.0.1:8091/command/dji_mic_start'
curl 'http://127.0.0.1:8091/command/dji_mic_stop'
```

USB Serial/JTAG is disabled in this configuration because it shares the ESP32-S3
USB PHY with USB OTG Host. Use UART0 for bench logs and Wi-Fi heartbeat/device
logs during normal operation.

Before hardware acceptance, run:

```bash
python3 tools/validate_dji_audio_pipeline.py
```

See [DJI Mic battery validation](../docs/dji_mic_battery_validation.md) for the
power, hot-plug, combined-load, low-battery, and soak-test procedure.

### Permanent network debugging

The default firmware keeps network debugging enabled so DJI Mic can occupy the
USB OTG PHY without removing diagnostics. Xiaopai receives normal and debug
commands through the server command queue, mirrors ESP-IDF log lines to
`POST /device/logs`, and advertises `network_debug`, `remote_commands`, and
`wifi_log_upload` in its control hello capabilities.

From the server host:

```bash
# Request a power/audio/heap diagnostic snapshot.
curl 'http://127.0.0.1:8091/command/debug_status'

# Read recent uploaded logs or follow the per-device readable file.
curl 'http://127.0.0.1:8091/device/logs?limit=100'
tail -f stack-chan-server/captures/device-logs/<device-id>.log

# Temporarily change an ESP-IDF tag's runtime log level.
curl -G 'http://127.0.0.1:8091/command/debug_log_level' \
  --data-urlencode 'tag=USB_STREAM' \
  --data-urlencode 'level=debug'
```

This path is asynchronous: command completion is visible in `/devices`, while
the command result and requested snapshot appear in `/device/logs`. Log upload
uses a bounded, non-blocking ring buffer so network stalls do not block robot
tasks. Configure it with the `STACKCHAN_NETWORK_DEBUG*` Kconfig options.

## Project Layout

```text
main/                    Firmware source
stack-chan-server/       Local server subproject
third_parties/           Firmware third-party source
CMakeLists.txt           ESP-IDF project entry
sdkconfig.defaults       Default firmware config
partitions.csv           Flash partition layout
```

Generated build output is intentionally not part of the cleaned project tree.
