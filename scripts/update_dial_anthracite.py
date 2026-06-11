#!/usr/bin/env python3
"""Retarget VDO dial face background to VW T2 tacho anthracite (scale-crop strategy).

Loads the original 480x480 dial bitmap (git 81f2149), remaps only neutral
face-background pixels to the flat anthracite tone sampled from the cockpit
tacho, and leaves the outer chrome ring, bezel, numerals, and tick marks
untouched. Firmware zooms above 100% so the chrome ring is cropped off-screen.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "vdo_dial_480_rgb565.h"
SOURCE_COMMIT = "81f2149:src/vdo_dial_480_rgb565.h"
RECOMMENDED_SCALE_PCT = 115
# Cockpit photo with tacho (left) + VDO clock (right) for color reference.
_COCKPIT_NAME = (
    "c__Users_niedi01_AppData_Roaming_Cursor_User_workspaceStorage_empty-window_images_"
    "image-86fe423c-34c6-4c4b-8937-82bd7f259f11.png"
)
COCKPIT_CANDIDATES = [
    Path.home()
    / ".cursor/projects/d-claude-waveshare-vdo-clock/assets"
    / _COCKPIT_NAME,
    ROOT / "assets" / _COCKPIT_NAME,
]
REF_PHOTOS = [
    *[p for p in COCKPIT_CANDIDATES if p.exists()],
    ROOT / "docs/photos/20260524_173440.jpg",
    ROOT / "docs/photos/20260524_173456.jpg",
    ROOT / "docs/photos/20260524_173546.jpg",
    ROOT / "docs/photos/20260524_173803.jpg",
    ROOT / "docs/photos/20260524_173800.jpg",
]

CX = CY = 240
# VW T2 tacho flat face (sampled from cockpit photo, left instrument).
ANTHRACITE = np.array([55, 55, 57], dtype=np.float32)


def rgb565_to_rgb(color: int) -> np.ndarray:
    r = ((color >> 11) & 0x1F) << 3
    g = ((color >> 5) & 0x3F) << 2
    b = (color & 0x1F) << 3
    return np.array([r, g, b], dtype=np.int32)


def rgb_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def lum(rgb: np.ndarray) -> float:
    r, g, b = rgb
    return 0.299 * r + 0.587 * g + 0.114 * b


def sat(rgb: np.ndarray) -> int:
    return int(max(rgb) - min(rgb))


def sample_tacho_face(arr: np.ndarray) -> list[tuple[int, int, int]]:
    """Sample flat face from left tacho in wide cockpit photo."""
    h, w = arr.shape[:2]
    if w < h * 1.2:
        return []
    tcx, tcy = int(w * 0.24), int(h * 0.42)
    max_r = min(tcx, tcy, w - tcx, h - tcy)
    samples: list[tuple[int, int, int]] = []
    for y in range(h):
        for x in range(w):
            dx, dy = x - tcx, y - tcy
            r = (dx * dx + dy * dy) ** 0.5
            if not (0.12 * max_r < r < 0.32 * max_r):
                continue
            rgb = arr[y, x]
            l = lum(rgb)
            if 20 < l < 70 and sat(rgb) < 30:
                samples.append(tuple(int(v) for v in rgb))
    return samples


def sample_dial_face(arr: np.ndarray) -> list[tuple[int, int, int]]:
    """Sample flat face from centred round dial photo."""
    h, w = arr.shape[:2]
    cx, cy = w // 2, h // 2
    max_r = min(cx, cy)
    samples: list[tuple[int, int, int]] = []
    for y in range(h):
        for x in range(w):
            dx, dy = x - cx, y - cy
            r = (dx * dx + dy * dy) ** 0.5
            if not (0.15 * max_r < r < 0.38 * max_r):
                continue
            rgb = arr[y, x]
            l = lum(rgb)
            if 35 < l < 75 and sat(rgb) < 25:
                samples.append(tuple(int(v) for v in rgb))
    return samples


def sample_anthracite() -> np.ndarray:
    """Median anthracite from tacho face in cockpit / docs reference photos."""
    samples: list[tuple[int, int, int]] = []
    for path in REF_PHOTOS:
        if not path.exists():
            continue
        arr = np.array(Image.open(path).convert("RGB"))
        tacho = sample_tacho_face(arr)
        if tacho:
            samples.extend(tacho)
            continue
        samples.extend(sample_dial_face(arr))
    if samples:
        return np.median(samples, axis=0).astype(np.float32)
    return ANTHRACITE.copy()


def load_dial_from_git(commit_path: str) -> np.ndarray:
    text = subprocess.check_output(["git", "show", commit_path], text=True, cwd=ROOT)
    return parse_header_text(text)


def parse_header_text(content: str) -> np.ndarray:
    start = content.index("{") + 1
    end = content.rindex("}")
    vals = np.array(
        [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", content[start:end])],
        dtype=np.uint16,
    )
    if vals.size != 480 * 480:
        raise RuntimeError(f"Expected 230400 pixels, got {vals.size}")
    return vals.reshape(480, 480).copy()


def load_dial() -> np.ndarray:
    return load_dial_from_git(SOURCE_COMMIT)


def is_face_gray(rgb: np.ndarray, radius: float) -> bool:
    l = lum(rgb)
    if radius > 212 or l < 22 or l > 82 or sat(rgb) > 18:
        return False
    if l < 28 and radius > 200:
        return False
    return True


def is_outside_circle(radius: float) -> bool:
    return radius > 239.5


def process_dial(dial: np.ndarray, target: np.ndarray) -> np.ndarray:
    old_mid = 48.0
    out = dial.copy()
    target_px = np.clip(target, 0, 255).astype(int)
    for y in range(480):
        for x in range(480):
            rgb = rgb565_to_rgb(int(out[y, x]))
            radius = ((x - CX) ** 2 + (y - CY) ** 2) ** 0.5
            if is_outside_circle(radius):
                out[y, x] = rgb_to_rgb565(*target_px)
            elif is_face_gray(rgb, radius):
                ratio = lum(rgb) / old_mid
                new_rgb = np.clip(target * ratio, 0, 255).astype(int)
                out[y, x] = rgb_to_rgb565(*new_rgb)
    return out


def write_header(dial: np.ndarray, target: np.ndarray) -> None:
    t = tuple(np.clip(target, 0, 255).astype(int))
    lines = [
        "#pragma once\n",
        "#include <Arduino.h>\n",
        "\n",
        f"// Tacho-matched anthracite face RGB{t}; chrome ring kept for scale-crop.\n",
        f"// Render at {RECOMMENDED_SCALE_PCT}% in firmware to clip the ring off-screen.\n",
        "// 480x480 RGB565 VDO dial face without hands; firmware draws live hands on top.\n",
        "static const uint16_t VDO_DIAL_480_RGB565[480 * 480] PROGMEM = {\n",
    ]
    flat = dial.flatten()
    for i in range(0, len(flat), 12):
        chunk = flat[i : i + 12]
        line = "  " + ", ".join(f"0x{v:04X}" for v in chunk)
        if i + 12 < len(flat):
            line += ","
        lines.append(line + "\n")
    lines.append("};\n")
    HEADER.write_text("".join(lines), encoding="utf-8")


def main() -> None:
    target = sample_anthracite()
    print(f"Target tacho anthracite RGB: {tuple(target.astype(int))}")
    print(f"Recommended firmware scale: {RECOMMENDED_SCALE_PCT}%")
    dial = process_dial(load_dial(), target)
    write_header(dial, target)
    print(f"Updated {HEADER}")


if __name__ == "__main__":
    main()
