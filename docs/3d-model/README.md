# ESP32-S3-Touch-LCD-2.8C — 3D Drawing

## Source

Official Waveshare 3D drawing package (STEP format):

**Download URL:**
```
https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C/ESP32-S3-Touch-LCD-2.8C-20241226.zip
```

**Wiki page:** https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C

Archive name: `ESP32-S3-Touch-LCD-2.8C-20241226.zip` (released 2024-12-26)

## Installation

1. Download the zip from the URL above.
2. Extract the archive — it contains a `.step` file (ISO 10303-21).
3. Place the extracted file(s) into this directory (`docs/3d-model/`).

## Usage

The STEP file can be opened with any mechanical CAD tool:

| Tool | Platform | Notes |
|------|----------|-------|
| FreeCAD | Linux / Windows / macOS | Free, open-source |
| KiCad StepUp / KiCad PCB | Linux / Windows / macOS | Useful for PCB integration |
| Fusion 360 | Windows / macOS | Free for hobbyists |
| SOLIDWORKS | Windows | Import via File → Open |
| Onshape | Browser | Import via Documents → Import |

## Purpose

The 3D model is useful for:

- Designing enclosures or mounting brackets for the display module
- Checking mechanical fit in a vehicle dashboard cutout (e.g. VDO Cockpit gauge slot)
- Verifying connector clearances before printing or ordering a case
- Integration into a PCB assembly drawing in KiCad or Altium

## Notes on large binary files

STEP files can exceed GitHub's 50 MB soft limit. If the file is large,
consider tracking it with **Git LFS**:

```bash
git lfs track "*.step" "*.stp" "*.STEP" "*.STP"
git add .gitattributes
git add docs/3d-model/ESP32-S3-Touch-LCD-2.8C.step
git commit -m "Add Waveshare ESP32-S3-Touch-LCD-2.8C STEP model"
```
