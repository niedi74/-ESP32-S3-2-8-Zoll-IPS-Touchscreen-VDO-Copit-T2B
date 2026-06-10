# Zukünftige Features / Backlog

## Original-Tacho-Drehregler → Display-Helligkeit (ADC)

**Später:** Analogen Eingang am ESP32-S3 an die Beleuchtungsschaltung des
Original-Tachometers anschließen.

Der Helligkeits-Drehregler (Drehregler) aus der originalen Cockpit-Instrumententafel
steuert vermutlich die Lampenhelligkeit über **quasi-Dimmung per Spannungsabsenkung**
(12 V → niedrigere Spannung = dunklere Glühbirnen-Beleuchtung).

**Ziel:** ESP liest diese Spannung per **ADC** ein und mappt sie auf die
Display-Hintergrundbeleuchtung (`hal_backlight` in `hal_waveshare_28c.h`, aktuell
binär auf GPIO 6 — ggf. auf **PWM/LEDC** umstellen für echtes Dimmen).

### Hardware-Hinweise

- **Spannungsteiler:** 12-V-Signal auf ESP32-ADC-tauglichen Bereich absenken (max. 3,3 V)
- Messpunkt an der originalen Tacho-Beleuchtungsschaltung, nicht am 12-V-Bordnetz direkt
- Entkopplung/Filtern prüfen (Glühbirnen-Last, PWM-artige Schwankungen?)

### Offene Fragen

- **GPIO-Pin:** Welcher freie ADC-fähige GPIO? (GPIO 6 ist bereits LCD-Backlight)
- **Spannungsteiler:** Widerstandswerte für erwarteten Spannungsbereich des Dimmers
- **Kalibrierung:** Mapping ADC-Rohwert → Helligkeitsprozent (min/max am Drehregler)
- **Verhalten Original-Dimmer:** Rein analoges Widerstandsnetzwerk oder andere Topologie?
