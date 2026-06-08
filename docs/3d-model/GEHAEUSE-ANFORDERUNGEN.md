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

**Betrachtungsachse:** Das T2b-Armaturenbrett ist leicht nach hinten geneigt
(ca. 10–15° zur Vertikalen). Das Display sollte zur Fahrerblickachse senkrecht
ausgerichtet sein, d. h. die Frontfläche des Gehäuses um ~10° nach oben kippen
(ggf. Keilform im Flansch einarbeiten oder im CAD berücksichtigen).
Vor der Konstruktion Neigungswinkel vor Ort mit einem Winkelmesser prüfen.

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

### 2.3 Anschlüsse und Bedienelemente

| Element | Position am PCB | Maße / Öffnung im Gehäuse |
|---------|----------------|---------------------------|
| USB-C (5 V, Programmierung) | Unterseite, eine Schmalseite | **10 × 5 mm** Schlitz; mit Kabelzugentlastung (siehe Abschn. 4.4) |
| Reset-Taster (EN) | Oberseite PCB, Randnähe | **Ø 4 mm** Stiftöffnung oder verlängerter Taster-Stößel (siehe Abschn. 4.3) |
| TF-Karten-Slot (microSD) | Unterseite PCB, Schmalseite | **15 × 3 mm** Schlitz; Karte muss tauschbar bleiben (siehe Abschn. 4.3) |
| 40-pol. FPC-Anschluss | Oberseite PCB | intern, kein Zugang nötig |
| GPIO-Pins | eine Längsseite | 2,54 mm Raster; bei Bedarf seitlicher Schlitz |

> **Position von Reset und TF-Slot:** Exakte Lage am PCB-Rand vor der
> Konstruktion im STEP messen. Taster-Stößel und Kartenöffnung müssen von
> außen zugänglich sein, ohne das Gehäuse zu demontieren.

---

## 3. Maße VDO-Rundschacht (DIN-Norm, T2b)

| Maß | Wert |
|-----|------|
| Frontausschnitt-Ø | **52 mm** (DIN-Norm-Instrument) |
| Einbautiefe verfügbar | **40–50 mm** (T2b-spezifisch, **vor Ort messen!**) |
| Klemmring-Ø (außen) | **~58 mm** |
| Wandstärke Armaturenbrett | **3–4 mm** |
| Haltemethode Original | Klemmfeder von hinten, alternativ 4× M4 von hinten |

---

## 4. Konstruktionsanforderungen

### 4.1 Geometrie / Passform

```
Vorderseite (Armaturenbrett-Seite):
┌────────────────────┐
│    ┌────────┐       │  ← Runde Blende Ø 52 mm, presst in VDO-Schacht
│    │  Ø 50  │       │  ← Fenster Ø 50 mm (Display-Sichtbereich)
│    └────────┘       │
│  ←── ~58 mm ──→     │  ← Flansch liegt auf Armaturenbrett auf
└────────────────────┘

Rückseite:
- PCB-Aufnahme ~65 × 65 mm, Rastnasen oder M2-Schrauben
- FPC-Kabelkanal (intern, kein Außenzugang nötig)
- Öffnungen für USB-C, Reset, TF-Slot (seitlich/rückseitig)
```

| Maß | Empfehlung |
|-----|------------|
| Außen-Ø Einbau | **52,0 mm** (Presssitz) oder 51,8 mm + O-Ring |
| Frontflansch-Ø | **58 mm** |
| Display-Fenster-Ø | **50,0 mm** |
| Tiefe gesamt | **17–20 mm** (Platine + Luft + Zugentlastung) |
| Wandstärke Zylinder | min. **1,5 mm** (PETG/ASA), **1,2 mm** (Resin) |
| Flansch-Stärke | min. **2,0 mm** |

### 4.2 Vibrationsfestigkeit (T2b Diesel)

Der T2b-Dieselmotor erzeugt starke Vibrationen (insb. im Leerlauf).
Presssitz allein reicht **nicht** für Dauerbetrieb.

**Pflichtanforderungen:**

- **Klemmring** von hinten (wie Original-VDO-Instrumente) **oder**
  4× **M4-Einschmelzmuttern** (Heatset Inserts) im Flansch + M4-Schrauben
  von hinten durch das Armaturenbrett
- PCB auf **Gummi-Abstandshaltern** (Silikon, Shore 30–40) lagern,
  kein Hartplastik-direktkontakt
- Schrauben zum PCB: **M2 mit Federring** oder Nyloc-Muttern
- Alle Kabelverbindungen mit Zugentlastung (siehe 4.4)

### 4.3 Zugänglichkeit Reset-Taster und TF-Karten-Slot

**Reset-Taster (EN):**

Zwei Optionen:
1. **Stiftöffnung** Ø 4 mm im Gehäuse, fluchtend mit dem Taster → Reset mit
   Kugelschreiber-Stift möglich (ausreichend für Wartung)
2. **Verlängerter Stößel**: 3D-gedruckter Kunststoffstab presst auf den
   Board-Taster; Betätigung durch kleine Taste im Gehäuse von außen

> Option 2 ist komfortabler, aber mechanisch aufwändiger. Option 1 reicht
> für den Kfz-Einsatz.

**TF-Karten-Slot (microSD):**

- Schlitz im Gehäuse **15 mm breit × 3 mm hoch**, bündig mit dem Slot am PCB
- Karte muss einschiebbar und entnehmbar sein ohne Werkzeug und ohne
  Gehäusedemontage
- Schlitz mit **IP52-konformer Staublippe** (Gummidichtung) empfohlen
  (Armaturenbrett ist staubanfällig)

### 4.4 Kabelzugentlastung USB-C

Im Fahrbetrieb wirkt mechanische Last auf das USB-C-Kabel (Vibrationen,
gelegentliches Ziehen). Ohne Zugentlastung droht Bruch des USB-C-Ports am PCB.

**Anforderungen:**

- **Kabelklemme** im Gehäuse, max. 20 mm hinter dem USB-C-Port:
  Kabelbinder-Öse oder Klemmbügel (M2-Schraube) für Kabeldurchmesser 4–6 mm
- Gehäusewandöffnung für USB-C: **10 × 5 mm**, mit 0,5 mm Spiel zum
  Stecker-Gehäuse (kein Presssitz am Stecker)
- Kabelausführung vorzugsweise **nach hinten oder nach unten** (schützt vor
  Zugkräften beim Ein-/Aussteigen)

### 4.5 Thermisches Management (Pflicht)

Der ESP32-S3 mit aktivem WiFi + BLE erreicht bis zu **85 °C** Chiptemperatur.
Im geschlossenen Armaturenbrett bei sommerlicher Innenraumtemperatur (80 °C)
entsteht ohne Belüftung **Hitzestau** → Thermal Throttling oder Absturz.

**Belüftungsschlitze sind keine Option, sondern Pflicht:**

| Position | Empfehlung |
|----------|------------|
| Rückwand / Deckel | 4–6× Schlitze **3 mm × 15 mm**, gesamt ≥ 150 mm² freie Fläche |
| Unterseite Flansch | 2–3× Schlitze, Konvektionsöffnung nach unten |
| Schlitzrichtung | Horizontal (schützt vor Staub von oben) |
| Abstand zum PCB | min. 3 mm Luftspalt zwischen PCB-Unterseite und Gehäusewand |

> Keine Schlitze auf der Sichtseite (Frontblende) — nur Rück-/Unterseite.
> Kein Lüfter nötig, Konvektion reicht bei ≤ 20 mW WiFi-Dauerlast.

### 4.6 Blendschutz / Anti-Glare

Die gewölbte Glasoberfläche des runden Displays reflektiert direktes Sonnenlicht.

**Optionen (im Gehäuse vorsehen):**

1. **Anti-Glare-Folie** (Matt-Folie, z. B. 3M AG): Aufkleben auf Displayglas,
   Ø 49 mm ausschneiden. Einfachste Lösung, ggf. Berührungsempfindlichkeit
   leicht reduziert.
2. **Vertieftes Display-Fenster**: Fenster 2–3 mm tiefer setzen als Frontflansch
   → erzeugt Schattenwurf ähnlich einer Sonnenblende (wie Original-VDO-Uhren).
   Empfohlen, da ohne Zusatzmaterial.
3. **Polarisationsfilter** (optionaler Zubehörartikel): Maximaler Glare-Schutz,
   aber Farben wirken bei schräger Betrachtung gedämpft.

> Option 2 lässt sich direkt im CAD einarbeiten (keine Zusatzkosten).

---

## 5. Materialempfehlung

| Material | Empfehlung | Grund |
|----------|-----------|-------|
| **PETG** | ✅ Empfohlen (Struktur) | Hitzebeständig bis ~85 °C, zäh, vibrationsfest |
| **ASA** | ✅ Empfohlen (Außen) | UV-beständig, kein Ausbleichen im Kfz |
| **Resin SLA** | ✅ für Frontblende | Sehr glatte Oberfläche, enger Passsitz Ø 52 mm |
| ABS | bedingt | Verzug beim Druck, schlechtere Schichthaftung |
| PLA | ❌ | Erweicht ab ~60 °C — im Sommer im Auto sicher zu warm |

**Kombination empfohlen:** Frontblende in Resin (Optik), Gehäusekorpus in PETG (Festigkeit).

Wandstärke min. **1,5 mm** (FDM) / **1,2 mm** (Resin SLA)
Layer-Höhe max. **0,15 mm** für sichtbare Flächen

---

## 6. Checkliste für den Konstrukteur

- [ ] STEP in FreeCAD öffnen, PCB-Körper `"Board"` isoliert messen (Breite × Tiefe)
- [ ] Display-Glas-Ø exakt messen (STEP: Z = +1,5 mm-Schicht)
- [ ] USB-C-Position und -Ausrichtung am PCB messen (Abstand Boardkante)
- [ ] Reset-Taster (EN) Position am PCB messen → Stiftöffnung oder Stößel festlegen
- [ ] TF-Karten-Slot Position und Öffnungsrichtung messen
- [ ] Neigungswinkel Armaturenbrett T2b vor Ort messen → Keilflansch oder gerade?
- [ ] Einbautiefe im T2b-Schacht vor Ort messen (Schublehre von vorne)
- [ ] Klemmring-Ø des VDO-Schachts messen (Original-Instrument zur Hand nehmen)
- [ ] Belüftungsschlitze und Luftspalt mind. 3 mm im CAD einplanen
- [ ] Kabelzugentlastung und Klemmring/M4-Muttern im Modell vorsehen
- [ ] Passform-Dummy (nur Adapterring, 5 % Füllung) zuerst drucken und anprobieren
- [ ] Finale Version in PETG/ASA (Korpus) + Resin (Frontblende) drucken

---

## 7. Druckservice-Anfrage (Mustertext)

> Ich benötige 3D-Druck-Teile in **PETG/ASA** (Gehäusekorpus) und **Resin**
> (Frontblende) für eine Kfz-Anwendung (VW T2b Armaturenbrett, Innenraum):
>
> **Bauteil:** Einbaugehäuse für ein rundes 2,8″-Touch-Display
> (Waveshare ESP32-S3-Touch-LCD-2.8C) in einen DIN-52-mm-VDO-Rundschacht.
>
> **Maße (Referenz, vor Konstruktion im STEP-CAD zu verifizieren):**
> - Außen-Ø Einbau: 52 mm (Presssitz)
> - Frontalflansch-Ø: 58 mm
> - Display-Fenster-Ø: 50 mm (vertieft 2–3 mm für Blendschutz)
> - Einbautiefe gesamt: ~18–20 mm
> - Wandstärke: 1,5 mm (PETG), 1,2 mm (Resin-Frontblende)
> - USB-C-Öffnung: 10 × 5 mm seitlich mit Kabelzugentlastungs-Öse
> - Reset-Öffnung: Ø 4 mm (Stiftöffnung)
> - TF-Slot-Öffnung: 15 × 3 mm
> - Belüftungsschlitze: 5× (3 × 15 mm) auf der Rückseite
> - Vibrationsfestigkeit: 4× M4-Heatset-Inserts im Flansch
>
> **3D-Quelldatei:** `ESP32-S3-Touch-LCD-2.8C-20241226.step` (mitgeliefert)
>
> Bitte zuerst einen **Passform-Dummy** (nur Adapterring, 5 % Füllung) drucken,
> dann nach Freigabe das finale Bauteil.

---

## 8. Referenzdateien

| Datei | Inhalt |
|-------|--------|
| `docs/3d-model/ESP32-S3-Touch-LCD-2.8C-20241226.step` | Offizielles Waveshare-3D-Modell (17,8 MB) |
| `docs/design/vdo_clock_preview.png` | Display-Design (Referenz Optik) |
| `docs/photos/` | Fotos des verbauten Displays |
| `docs/PINOUT.md` | GPIO-Belegung (für Kabeldurchführung relevant) |
