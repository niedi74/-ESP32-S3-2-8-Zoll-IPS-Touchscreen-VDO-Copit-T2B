# AGENTS.md

## Cursor Cloud specific instructions

### Product

Single embedded firmware target: **VDO Quartz-Zeit** for the Waveshare ESP32-S3-Touch-LCD-2.8C (480×480 round display). There are no host-side web servers, Docker services, or npm/pip application runtimes — development is **PlatformIO cross-compile + optional USB flash/monitor** on real hardware.

### PATH

PlatformIO installs to `~/.local/bin`. Ensure it is on `PATH` before running `pio`:

```bash
export PATH="$HOME/.local/bin:$PATH"
```

### Build (primary validation)

```bash
pio run -e waveshare_s3_28c
```

Successful output ends with `[SUCCESS]` and produces `.pio/build/waveshare_s3_28c/firmware.bin`. The pre-build script `scripts/inject_time.py` prints `[inject_time] RTC build time = ...` during compile.

### Flash and monitor (requires physical board)

```bash
pio run -e waveshare_s3_28c -t upload
pio device monitor -e waveshare_s3_28c
```

`platformio.ini` sets `monitor_rts = 0` and `monitor_dtr = 0` so opening the serial monitor does not reset the ESP32-S3 via USB-CDC.

Cloud VMs typically have **no ESP32 attached** (`pio device list` is empty). Upload/monitor and on-device Web GUI/BLE/WiFi testing need real hardware.

### WiFi credentials (optional)

Copy `src/wifi_secret.example.h` → `src/wifi_secret.h` (gitignored). The firmware builds without it (`__has_include` in `main.cpp`); WiFi/NTP/Web GUI stay disabled until credentials are provided.

### Lint / tests

- **No unit tests** — there is no `test/` directory; `pio test` is not applicable.
- **No CI lint config** — no pre-commit hooks or Makefile.
- `pio check` (cppcheck) is not configured for this embedded toolchain and may fail; use `pio run` as the reliable compile check.

### Platform / dependencies

- Platform: custom pioarduino zip from `platformio.ini` (Arduino-ESP32 3.x on ESP-IDF 5.3).
- Library: `h2zero/NimBLE-Arduino` via `lib_deps`.
- First build downloads the ESP32 toolchain and platform (~several hundred MB); later builds are incremental.
