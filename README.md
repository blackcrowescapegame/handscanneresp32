# Native ESP32-P4 Handscanner

Native (no HTML/webview) PlatformIO firmware for the Waveshare
**ESP32-P4-WIFI6-Touch-LCD-10.1**, SKU **33150**. It recreates the game from
`htmlhandscanner/` on the board's display, touch panel, speaker, and hosted
Wi-Fi interface.

## Included behavior

- 800x1280 portrait handscanner UI using the original artwork
- animated scan line and five capacitive-touch targets
- the original solution, `3, 2, 4, 5, 1`
- finger, access-granted, access-denied, and three-play reset-riser sounds
- Home Assistant event POST with the original `state_changed` payload
- reset endpoint polling every 3000 ms by default
- remote command `1`: reset and play the riser three times
- remote command `2`: full-screen blackout
- remote command `3`: full-screen Belial hint
- skull fade on a successful solution and two-second reset after denial
- authenticated Arduino OTA firmware updates over Wi-Fi
- read-only JSON health/version API at `/api/health`

## Configure

Copy the example secrets file and edit the copy:

```powershell
Copy-Item include/secrets.example.h include/secrets.h
```

Set the Wi-Fi SSID, password, and Home Assistant long-lived access token in
`include/secrets.h`. The two API URLs already default to the values used by
the HTML implementation and can also be overridden there. `secrets.h` is
ignored by Git.

Set `HANDSCANNER_OTA_PASSWORD` in the same file to enable OTA. OTA stays
disabled when that value is empty.

The ESP32-C6 coprocessor must contain Waveshare's compatible hosted Wi-Fi
firmware. A board still running its factory firmware normally already has it.

## Build and flash

Use the board's **USB TO UART** connector:

```powershell
pio run
pio run --target upload --upload-port COM5
pio device monitor --baud 115200
```

Replace `COM5` with the device's port. If automatic download mode does not
start, hold **BOOT**, tap **RESET**, start the upload, then release **BOOT**.

The default environment targets current ESP32-P4 revision 3.x silicon, as
recommended by Waveshare for current boards. ESP32-P4 revisions before 3.0
need a separate build target and must not be flashed with this binary.

### Install an update over Wi-Fi

The dual-slot OTA partition table must first be installed once through USB.
After that first USB upload, confirm the serial log contains `OTA: ready` and
run the deployment script. It reads the password from `include/secrets.h`
without printing it and defaults to this unit's current IP, `192.168.40.57`:

```bat
deployOTA.cmd
```

Pass another IP or mDNS hostname as the first argument if DHCP changes it:

```bat
deployOTA.cmd 192.168.40.57
```

Each OTA update is written to the inactive slot, verified, selected for the
next boot, and then activated by an automatic restart. A USB upload remains
available as the recovery path.

### Check the running firmware

Increment `custom_firmware_version` in `platformio.ini` when preparing a new
release. The running device reports that version and its active OTA slot:

```powershell
curl.exe http://192.168.40.57/api/health
```

Example response:

```json
{"status":"ok","state":"ready","version":"1.1.0","uptime_ms":12345,"ip":"192.168.40.57","rssi_dbm":-52,"free_heap_bytes":180000,"ota_enabled":true,"ota_state":"ready","ota_error":0,"ota_partition":"ota_0"}
```

## Assets

The RGB565 UI screens and 16 kHz mono PCM clips under `data/` are embedded in
the firmware. Regenerate them after editing the original PNG/MP3 files with:

```powershell
.\tools\generate_assets.ps1
```

This requires `ffmpeg` on `PATH`. The 32 MB flash is divided into two 15 MB
application slots so the approximately 7.7 MB firmware can be updated safely.

## Source layout

- `src/main.cpp`: game state, rendering, touch, and remote-command handling
- `src/network_client.cpp`: Wi-Fi, API POST, time sync, and command polling
- `src/audio_engine.cpp`: ES8311/I2S PCM playback task
- `tools/platformio_pre.py`: Windows toolchain path and pinned esptool setup
- `tools/platformio_post.py`: Windows-safe esptool progress configuration
- `deployOTA.cmd`: authenticated OTA build and upload using the private header
- `lib/WaveshareDisplays/`: first-party Waveshare display/touch support
- `lib/ArduinoGFXMinimal/`: Waveshare-tested ESP32-P4 DSI subset of Arduino_GFX
- `lib/ES8311/`: first-party ES8311 codec driver

The native game remains usable offline; API features stay disabled until
Wi-Fi credentials are configured.
