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
- Boot zeichnet Uhr sofort.
- Backlight bleibt hart HIGH.
- Automatische Display-Retries nach ca. 4s und 10s:
  - `coldBootDisplayRetry()`
  - expander init
  - ST7701 init
  - RGB panel init
  - aktuelle Seite neu zeichnen
- WLAN/Web und BLE sind Runtime-Schalter, aber Stabilitaet geht vor.
- BLE default aus.

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
  2. Erst danach Motorraum-Hub-Daten anzeigen.
  3. Touch/Encoder nur fuer Seitenwechsel.
- WebGUI/WLAN nicht mehr mitten im laufenden Display neu starten.
- Motorraum-Hub-Werte bevorzugt per BLE vom Hub empfangen; Hub-Funktionen einzeln zuschalten.

## Git
- Branch: `codex/waveshare-hub-pages`
- Repo: `niedi74/-ESP32-S3-2-8-Zoll-IPS-Touchscreen-VDO-Copit-T2B`
