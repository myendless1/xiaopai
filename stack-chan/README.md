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

The device checks the server after WiFi/server selection. If `/realtime/config`
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
