# Anforderungsliste: 3D-Adapterplatte / Gehäuse
## Waveshare ESP32-S3-Touch-LCD-2.8C → VDO Cockpit T2b

Dieses Dokument fasst alle Maße und Anforderungen zusammen, die ein
CAD-Konstrukteur oder Druckservice benötigt, um eine Adapterplatte oder ein
vollständiges Gehäuse zu konstruieren und zu drucken.

---

## 1. Einbausituation

```
┌─────────────────── Armaturenbrett T2b ───────────────────┐
│                                                           │
│   ┌──────────────┐   ◯ 52mm     ◯ 52mm                   │
│   │  Tacho/Drehz │  VDO-Uhr   VDO-Öl   ← freie Slots    │
│   └──────────────┘                                        │
└───────────────────────────────────────────────────────────┘
```

**Ziel:** Das Waveshare-Board in einen freien VDO-Rundschacht (Ø 52 mm)
einbauen, sodass das runde Display bündig zur Armaturenbrettoberfläche sitzt
und die Optik der originalen VDO-Instrumente nachbildet.

---

## 2. Maße Waveshare ESP32-S3-Touch-LCD-2.8C

Aus STEP-Datei `ESP32-S3-Touch-LCD-2.8C-20241226.step` (Autodesk/FreeCAD, 2024-12-26) ermittelt.
**Vor dem Druck in FreeCAD oder Fusion 360 verifizieren!**

### 2.1 Display

| Maß | Wert | Herkunft |
|-----|------|---------|
| Display-Typ | Rund, IPS, 480×480 px | Datenblatt |
| Aktive Fläche (sichtbar) | Ø **50 mm** | STEP-Messung (Z-Schicht +1,5..+3 mm) |
| Glas-Ø (gesamt inkl. Bezel) | ca. **55–58 mm** | Schätzung ± 2 mm |
| Displayglas-Dicke über PCB | **+1,5 .. +2,5 mm** | STEP-Messung |
| Berührungs-IC | GT911 (kapazitiv) | Datenblatt |

### 2.2 Platine (PCB)

| Maß | Wert | Hinweis |
|-----|------|---------|
| PCB-Breite | ca. **64–66 mm** | ¹ In FreeCAD prüfen |
| PCB-Tiefe | ca. **64–66 mm** | ¹ Fast quadratisch |
| PCB-Dicke (FR4) | **1,6 mm** | Standard |
| Bauhöhe Unterseite | bis **-11,5 mm** | STEP-Messung (ESP32, Konnektor) |
| Gesamthöhe Baugruppe | ca. **14–15 mm** | Z=-12 mm (Unterseite) bis Z=+3 mm (Glas) |

> ¹ Das STEP-File enthält ein 40-poliges FPC-Flachbandkabel (0,5 mm Raster),
> das die Bounding-Box auf 98,9 × 71,7 mm aufbläst. Das eigentliche PCB ist
> deutlich kleiner (~65 × 65 mm). In FreeCAD: PCB-Körper `"Board"` einzeln
> selektieren → `Part → Check Geometry` für exakte Maße.

### 2.3 Anschlüsse

| Anschluss | Position | Maße (ca.) |
|-----------|----------|------------|
| USB-C (Stromversorgung + Programmierung) | Unterseite PCB, eine Schmalseite | 9 × 3,5 mm |
| 40-pol. FPC-Anschluss (Display-Kabel) | Oberseite PCB | intern, kein Zugang nötig |
| Erweiterungspins (GPIO) | eine Längsseite | 2,54 mm Raster, nach Pinout |

---

## 3. Maße VDO-Rundschacht (DIN-Norm, T2b)

| Maß | Wert |
|-----|------|
| Frontausschnitt-Ø | **52 mm** (DIN-Norm-Instrument) |
| Einbautiefe verfügbar | **40–50 mm** (T2b-spezifisch, messen!) |
| Klemmring-Ø (außen) | **~58 mm** |
| Wandstärke Armaturenbrett | **3–4 mm** |
| Haltemethode Original | Klemmfeder von hinten, ggf. 4× M4-Schrauben |

---

## 4. Konstruktionsaufgabe

### Option A — Adapterplatte (einfacher)

```
Vorderseite (Armaturenbrett-Seite):
┌────────────────────┐
│    ┌────────┐       │  ← Runde Blende Ø 52 mm, passt in VDO-Schacht
│    │  Ø 50  │       │  ← Ausschnitt Ø 50 mm (Display-Fenster)
│    └────────┘       │
│  ←── ~58 mm ──→     │
└────────────────────┘

Rückseite:
- Aufnahme für PCB (~65 × 65 mm)
- Rastnasen oder M2-Schrauben zum Fixieren der Platine
- Kanal für FPC-Kabel (ist bereits am Display angelötet, bleibt intern)
- Öffnung für USB-C an Schmalseite
```

| Maß Adapterplatte | Empfehlung |
|-------------------|------------|
| Außen-Ø | **52,0 mm** (presst in Schacht) oder 51,8 mm + O-Ring |
| Frontblende-Ø (Außen) | **58 mm** (Auflageflansch) |
| Display-Fenster-Ø | **50,0 mm** (bündig mit aktivem Bereich) |
| Tiefe gesamt | **15–18 mm** (Platine + Luft) |
| Wandstärke | min. **1,5 mm** (PETG/ASA), **1,2 mm** (Resin) |
| USB-Öffnung | 10 × 5 mm, seitlich oder rückseitig |

### Option B — Vollgehäuse (aufwändiger)

Wie Option A, aber mit:
- Deckel/Rückwand (kann abgenommen werden für USB-Zugang)
- Stecknase / Federclip für T2b-Schacht (kein Kleber)
- Ggf. Belüftungsschlitze (ESP32 kann warm werden)

---

## 5. Materialempfehlung

| Material | Empfehlung | Grund |
|----------|-----------|-------|
| **PETG** | ✅ Empfohlen | Hitzebeständig (Kfz-Innenraum ~80 °C), zäh |
| **ASA** | ✅ Empfohlen | UV-beständig, Kfz-geeignet |
| ABS | bedingt | Verzug beim Druck, schlechtere Schichthaftung |
| PLA | ❌ | Erweicht ab ~60 °C, ungeeignet für Kfz |
| Resin (SLA) | ✅ für Frontteil | Sehr glatte Oberfläche, enger Passitz |

Wandstärke min. **1,5 mm** (FDM) / **1,2 mm** (Resin SLA)
Layer-Höhe max. **0,15 mm** für sichtbare Flächen

---

## 6. Checkliste für den Konstrukteur / Druckservice

- [ ] STEP-Datei in FreeCAD öffnen, PCB-Körper isoliert messen (Breite × Tiefe × Höhe)
- [ ] Display-Glas-Ø exakt messen (STEP: `Z = +1,5 mm`-Schicht)
- [ ] USB-C-Position am PCB ausmessen (Abstand von Boardkante)
- [ ] Einbautiefe im T2b-Schacht vor Ort messen (Stab/Schublehre von vorne)
- [ ] Klemmring-Ø des VDO-Schachts messen (Original-Instrument zur Hand nehmen)
- [ ] Passung testen: zuerst Kleindruck (20 %) als Passform-Dummy drucken
- [ ] Finale Version in PETG oder ASA drucken

---

## 7. Druckservice-Anfrage (Mustertext)

> Ich benötige einen 3D-Druck in **PETG oder ASA** für eine Kfz-Anwendung
> (VW T2b Armaturenbrett, Innenraum):
>
> **Bauteil:** Adapterplatte / Einbaugehäuse für ein rundes 2,8″-Display
> (Waveshare ESP32-S3-Touch-LCD-2.8C) in einen DIN 52-mm-Rundinstrumenten-Schacht.
>
> **Maße (Referenz, vor Konstruktion in CAD zu verifizieren):**
> - Außen-Ø Einbau: 52 mm (presst in Schacht)
> - Frontalflansch-Ø: 58 mm
> - Display-Fenster-Ø: 50 mm
> - Einbautiefe gesamt: ~17 mm
> - Wandstärke: 1,5 mm
> - USB-C-Öffnung: 10 × 5 mm seitlich
>
> **3D-Quelldatei:** `ESP32-S3-Touch-LCD-2.8C-20241226.step` (mitgeliefert)
>
> Bitte zuerst einen **Passform-Dummy** (nur Adapterring, ~5 % Füllung) drucken,
> dann nach Freigabe das finale Bauteil.

---

## 8. Referenzdateien

| Datei | Inhalt |
|-------|--------|
| `docs/3d-model/ESP32-S3-Touch-LCD-2.8C-20241226.step` | Offizielles Waveshare-3D-Modell (17,8 MB) |
| `docs/design/vdo_clock_preview.png` | Display-Design (Referenz Optik) |
| `docs/photos/` | Fotos des verbauten Displays |
| `docs/PINOUT.md` | GPIO-Belegung (für Kabeldurchführung relevant) |
