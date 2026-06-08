# Handoff 2026-06-08 - Waveshare 2.8C Cockpit Display

## Ziel
- Minimal stabil: Uhr beim USB-C/Zündung-Power-On anzeigen.
- Danach als Remote-Display fuer Motorraum-Hub-Werte nutzen.
- Touch/Encoder spaeter nur als einfache Seitenumschaltung.

## Hardware
- Waveshare ESP32-S3 2.8C Round LCD auf COM13.
- Touch ist aktuell nicht sinnvoll nutzbar/angeschlossen: `FEATURE_TOUCH 0`.
- SD bleibt aus; GPIO1/2 teilen LCD-Init/SD-Themen.

## Aktueller Befund
- Reines Display/Uhr funktioniert.
- WLAN-Reconnect nach Displaystart verursacht `Guru Meditation: Cache disabled but cached memory region accessed`.
- Deshalb: kein WLAN-Reconnect nach Panelstart.
- WLAN wird vor Displaystart kurz versucht. Wenn kein Connect: WLAN aus, Uhr bleibt Prioritaet.
- Nach echtem USB-C Power-Cycle kann das Panel schwarz bleiben, obwohl ESP laeuft.
- Manuelles serielles `clock` brachte die Uhr wieder. Daher ist ein automatischer Cold-Boot-Retry eingebaut.

## Aktuelle Firmware-Strategie
- Stand nach weiterem Debugging: `COCKPIT_DISPLAY_ONLY 1`.
- Nur-Uhr-Firmware wurde wieder geflasht, weil diese beim User bisher stabil war.
- WLAN/Web/BLE/Touch/SD duerfen nicht mehr gleichzeitig mit dem Displayproblem getestet werden.
- `platformio.ini` hat wieder die S3/RGB/PSRAM-Stabilitaetsflags:
  - `BOARD_HAS_PSRAM`
  - `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=1`
  - `CONFIG_SPIRAM_SPEED_120M=1`
  - `CONFIG_LCD_RGB_RESTART_IN_VSYNC=1`
  - `board_build.psram_type = opi`

## Serielle Befehle COM13 / 115200
- `status`
- `safe`
- `wifi:zoo`
- `wifi:s24`
- `wifi:on`
- `wifi:off`
- `ble:on`
- `ble:off`
- `clock`
- `reinit`

## Wichtig fuer naechsten Schritt
- Fokus eng halten:
  1. USB-C Power-Cycle: Uhr muss allein wiederkommen.
  2. Dann Display-HAL aus Marine-Displays sauber portieren.
  3. Erst danach Motorraum-Hub-Daten anzeigen.
  4. Touch/Encoder nur fuer Seitenwechsel.
- Aktueller Code enthaelt keinen M5GFX/M5Stack-Init, der RGB-Pins belegt.
- Trotzdem ist die Marine-Displays-HAL der richtige naechste Umbau, damit Display-Init und App-Code getrennt sind.
- WebGUI/WLAN nicht mehr mitten im laufenden Display neu starten.
- Motorraum-Hub-Werte bevorzugt per BLE vom Hub empfangen; Hub-Funktionen einzeln zuschalten.

## Git
- Branch: `codex/waveshare-hub-pages`
- Repo: `niedi74/-ESP32-S3-2-8-Zoll-IPS-Touchscreen-VDO-Copit-T2B`
