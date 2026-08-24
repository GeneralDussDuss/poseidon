#!/usr/bin/env python3
"""
build_picons.py - build src/ui/picons_data.h from ALL menu icon sheets.

Supersedes convert_picons.py (single sheet). Each source is a 4x4 grid of neon
line art; every cell becomes an anti-aliased 8-bit COVERAGE map that picons.cpp
alpha-blends with the theme colour. Nothing is ever upscaled on device -- the
art is authored at the final draw size, which is what keeps the edges clean.

TWO SOURCE SHAPES ARE HANDLED:
  RGBA PNG  - alpha carries the art; fold in luminance so the bright stroke core
              outweighs the soft outer halo.
  RGB  JPEG - no alpha, and a "transparent" export bakes the CHECKERBOARD in as
              real mid-grey pixels. Using luminance directly would paint that
              checkerboard behind every icon as a grey box. The neon strokes are
              strongly saturated and the checkerboard is pure grey, so gate on
              CHROMA: keep a pixel only if it is colourful, or bright enough to
              be a white-hot stroke core.

A levels curve then clamps the residual halo to fully transparent and saturates
the strokes, so each icon composites cleanly over the animated menu background.

USAGE: python scripts/build_picons.py [--size 96]
"""
import argparse, sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required:  pip install Pillow")

ROOT = Path(__file__).resolve().parent.parent
DL   = Path.home() / "Downloads"
OUT  = ROOT / "src" / "ui" / "picons_data.h"
GRID = 4

# prefix -> source sheet. ROOT is the original 16-icon sheet used by the top
# level menu; the rest are per-submenu sheets.
SHEETS = [
    ("ROOT",  DL / "Gemini_Generated_Image_7o2xgd7o2xgd7o2x.png"),
    ("WIFI",  DL / "Wifi.jpg"),
    ("BLE",   DL / "Ble.jpg"),
    ("RADIO", DL / "Radio.jpg"),
    ("TOOL",  DL / "tool.nfc.jpg"),
]

LO, HI = 0.16, 0.62          # levels curve applied after normalisation


def coverage(cell):
    """RGBA alpha*luma, or chroma-gated luma for RGB sheets."""
    if cell.mode in ("RGBA", "LA"):
        a = cell.getchannel("A").getdata()
        l = cell.convert("L").getdata()
        return [(x * y) // 255 for x, y in zip(a, l)]
    out = []
    for (r, g, b) in cell.convert("RGB").getdata():
        hi, lo = max(r, g, b), min(r, g, b)
        out.append(hi if (hi - lo) >= 30 or hi >= 150 else 0)
    return out


def slice_sheet(path, size):
    sheet = Image.open(path)
    W, H = sheet.size
    cw, ch = W // GRID, H // GRID
    icons = []
    for idx in range(GRID * GRID):
        r, c = divmod(idx, GRID)
        cell = sheet.crop((c * cw, r * ch, (c + 1) * cw, (r + 1) * ch))

        cov = Image.new("L", cell.size)
        cov.putdata(coverage(cell))

        bbox = cov.getbbox()                       # trim so glyphs fill evenly
        if bbox:
            cov = cov.crop(bbox)
        side = max(cov.size) or 1
        sq = Image.new("L", (side, side), 0)
        sq.paste(cov, ((side - cov.width) // 2, (side - cov.height) // 2))
        sm = sq.resize((size, size), Image.LANCZOS)

        peak = max(sm.getdata())
        if peak and peak < 255:
            sm = sm.point(lambda v, s=255.0 / peak: min(255, int(v * s)))
        sm = sm.point(lambda v: 0 if v < LO * 255
                      else min(255, int(((v / 255.0 - LO) / (HI - LO)) * 255)))
        icons.append(list(sm.getdata()))
    return icons


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=96)
    args = ap.parse_args()
    size = args.size

    L = ["/*", " * picons_data.h - AUTO-GENERATED. Do not edit by hand.",
         f" * Regenerate: python scripts/build_picons.py --size {size}", " *",
         f" * {len(SHEETS)} sheets x 16 icons, {size}x{size}, 8-bit coverage (row-major).",
         " * Drawn 1:1 by picons.cpp via per-pixel alpha blend -- never upscaled.", " */",
         "#pragma once", "", "#include <stdint.h>", "",
         f"#define PICON_W {size}", f"#define PICON_H {size}", ""]

    total = 0
    for prefix, path in SHEETS:
        if not path.exists():
            print(f"  !! missing {path}"); continue
        icons = slice_sheet(path, size)
        print(f"  {prefix:<6s} {path.name:<45s} 16 icons")
        for i, data in enumerate(icons):
            L.append(f"static const uint8_t PICON_{prefix}_{i}[{size*size}] = {{")
            for row in range(size):
                L.append("    " + ",".join(str(v) for v in data[row*size:(row+1)*size]) + ",")
            L.append("};")
        L.append(f"static const uint8_t *const PICON_SHEET_{prefix}[16] = {{")
        L.append("    " + ", ".join(f"PICON_{prefix}_{i}" for i in range(16)))
        L.append("};")
        L.append("")
        total += 16 * size * size

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(L), encoding="ascii")
    print(f"\nwrote {OUT}\n  {total/1024:.0f} KB flash")


if __name__ == "__main__":
    main()
