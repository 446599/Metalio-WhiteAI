# Metalio AI Ink Dashboard

Firmware for the **Metalio E-Ink 4** (ESP32-S3, GDEM0397T81 / SSD1677). The
device boots directly into a quiet AI dashboard for an e-paper screen. The
rendering path is a raw 1-bit framebuffer; LVGL is not compiled or linked into
the application target.

| Item | Value |
|------|-------|
| Project | `metalio-hw-test` |
| Target | ESP32-S3 |
| Board | Metalio E-Ink 4 |
| Panel | 800×480, SSD1677 (480×800 logical portrait UI) |
| Touch | CST816S |
| ESP-IDF | 6.0.1 verified (5.5.2+ intended) |
| Application offset | `0x80000` |

Browser flasher (GitHub Pages, no toolchain needed):
**https://446599.github.io/Metalio-WhiteAI/** — pair it with the firmware
binaries from [Releases](https://github.com/446599/Metalio-WhiteAI/releases).

**The Chinese README is the maintained one**: [README_zh.md](README_zh.md)
covers every product feature, build/flash steps and known limits.
For adding features, see the [secondary development guide](docs/DEVELOPMENT.md)
(currently in Chinese). The sections below are a short English summary.

## Interface

The home screen has a static clock, four tiles (alarm, calendar, notes, Xiaozhi)
and separate app/device directories. The app directory has six entries:
alarm, calendar, recorder, Xiaozhi, AI notes and voice capsules.
Lucide icons share native monochrome strokes.

The Xiaozhi MCP channel exposes **27 tools over 9 pages** for reminders,
schedules, notes, chat history, volume, app navigation, recorder control, weather
and real device status. Queued operations have IDs and explicit completion
states. See [AI system tools](docs/AI_SYSTEM_MCP.md).

Content taps and cover keys use the existing routes. HOME returns home, PREV
goes back, NEXT advances selections/pages, and holding the AI key records speech;
releasing it sends the utterance. A short POWER press locks and wakes the screen.
Local audio recording remains a separate 30-second recorder with SD save and
playback. Third-party calendar sync and WAV transcription are not implemented.

Run `python3 tools/check_ui_contract.py` for geometry checks. There are 24
`tools/check_*.py` host checks in total. With a C++17 compiler and Pillow,
`python3 tools/render_ui_preview.py` executes actual firmware drawing and bitmap
fonts across normal, offline, empty, long-content and stale scenarios.
These are software previews, not panel photographs.

## Bitmap fonts

`main/display/font/ai_ui_assets.c/.h` contains checked-in converted bitmap
fonts generated with the same packed format used by the sibling EegoRead
project: sorted codepoints, MSB-first rows and 2-bit coverage. The raw renderer
thresholds coverage while writing the SSD1677 1-bit buffer, so no runtime font
rasterizer or UI framework is needed. `generate_assets.py` records the
Pillow-based conversion recipe; the generated C file is what ships on-device.

Legacy LVGL adapters and test pages remain in the repository for board
diagnostics, but they are excluded from `main/CMakeLists.txt` for this target.

## Providers and Xiaozhi

`dashboard::DashboardService` runs on its own FreeRTOS task. It reads optional
configuration from the `dashboard` NVS namespace, fetches Open-Meteo JSON by
default, accepts a configurable quota endpoint, bounds every response to 16 KiB,
and stores the latest valid weather/quota snapshot for offline display. Custom
cards, schedule rows and AI summary lines can also be provisioned through NVS.

`xiaozhi::Client` implements the Xiaozhi v1 WebSocket hello/session/control
layer. It sends the reference `listen` and `abort` envelopes, handles hello,
STT, LLM, TTS, activation and tool events, and maps those events to the AI
card. Credentials are read from the `xiaozhi` NVS namespace and are never
printed.

`xiaozhi::AudioSession` owns the audio direction and, like the reference client,
keeps the two directions on different clocks:

- uplink uses the board microphone clock (16 kHz / 60 ms); Opus frames use
  raw version 1 framing by default; version 3 adds the
  `{type, reserved, payload_size_be16}` header when explicitly configured;
- downlink opens the decoder at the rate announced in the server hello (24 kHz
  on this deployment) and linearly resamples it onto the fixed 16 kHz speaker
  clock;
- capture stops automatically on an `stt` event or when TTS starts, so the
  speaker is never fed back into the server.

Tapping the AI card opens the conversation page. Hold BOOT to record and release
to send. Canceling an incomplete turn retires its connection; press again after
reconnection to begin another turn. Codec
handles are opened lazily and released when a direction goes idle, and a
disconnect drops queued audio instead of resuming with stale speech. Capture
and playback use 40 KiB / 24 KiB stacks (libopus needs about 25 KiB; 8 KiB
overflowed) and an eight packet queue (~0.5 s) that drops the oldest packet when
full.

`XIAOZHI_STATS?` reports counters, error counts, input peak, all three sample
rates, transport/session flags, stack headroom and free heap. `AI_TEXT?` returns
the AI card status and its three lines so a conversation can be checked without
photographing the panel. `XIAOZHI_AUDIO_TEST?` runs a microphone round trip plus
a 1 kHz speaker tone, and `XIAOZHI_DOWNLINK_TEST?` pushes a 24 kHz tone through
the real downlink path (header strip, decode, resample, play) without a server.

Measured on the board: 33/33 microphone frames, 180 bytes per Opus frame,
10.6 ms encode and 1.7 ms decode per frame, 16/16 tone and 16/16 downlink frames,
about 19 KB / 15.8 KB of stack headroom, and free internal heap returning to
about 100 KB once the codecs are released. The server round trip
(speech -> STT -> LLM -> TTS) still needs a spoken test on the physical device.

Useful NVS keys:

```text
dashboard/weather_url              # optional URL; defaults to Open-Meteo Beijing
dashboard/weather_loc              # NVS key names stay within the 15-byte limit
dashboard/quota_url                # optional JSON endpoint
dashboard/quota_token              # optional Bearer token
dashboard/refresh_minutes          # 1..120, default 10
dashboard/custom0_* / custom1_*    # title, value, detail, enabled
dashboard/schedule0_time/title, dashboard/sched0_detail .. sched2_detail
dashboard/summary0 .. summary2
xiaozhi/enabled                    # default true
xiaozhi/url                        # default wss://api.tenclass.net/xiaozhi/v1/
xiaozhi/token                      # provision a deployment token
```

The service writes the compact `w_*` and `q_*` keys after a successful fetch.
These keys are implementation details and can be cleared to return to the
offline defaults.

## Build

```bash
export IDF_PATH=/Users/henry/.espressif/v6.0.1/esp-idf
export IDF_PYTHON_ENV_PATH=/Users/henry/.espressif/python_env/idf6.0_py3.14_env
idf.py set-target esp32s3
idf.py build
```

The verified application image is `build/metalio-hw-test.bin`. The smallest
OTA slot is 5 MiB; the raw dashboard build remains below that limit.

## Flash and monitor

For an application-only update, keep the existing bootloader, partition table
and NVS data, and write the image at `0x80000`:

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem21101 --baud 921600 \
  write-flash 0x80000 build/metalio-hw-test.bin
```

Confirm the target MAC with `esptool.py chip-id` before flashing a board. The
board's display/driver and touch task report their startup state on the serial
console. A successful esptool hash proves flash readback only; optical pixels
and physical touch still require observing the actual panel.

## Hardware diagnostics

The board HAL still contains the original peripheral test sources (battery,
buttons, vibration, audio, Bluetooth, Wi-Fi/4G and SD). They are kept for
bring-up and can be selected in a diagnostic build; the product startup path
uses the raw AI home page above.

## License

Unless a file or third-party component states otherwise, follow the license
terms that apply to this repository and its dependencies.
