#!/usr/bin/env python3
"""
convert_picons.py - slice the neon line-art sprite sheet into ANTI-ALIASED
8-bit alpha icons for the POSEIDON menu.

WHY THIS EXISTS (vs the older convert_icons.py):
    convert_icons.py produced 24x24 *1-bit* bitmaps by thresholding a JPG.
    picons.cpp then integer-upscaled those 2x/3x for the carousel card, which
    is exactly why the menu looked like "pixel garbage": a hard-thresholded
    24px glyph blown up to 72px has 3x3 blocky stair-stepped edges and no
    intermediate tones.

    This script keeps the source alpha channel and downsamples with Lanczos to
    the FINAL on-screen size, storing 8-bit coverage per pixel. The renderer
    then alpha-blends the theme colour over the background, so edges are smooth
    at native resolution with zero upscaling. Clean lines, like Bruce.

SOURCE:
    A 4x4 grid RGBA sprite sheet (Gemini-generated neon line art, transparent
    background). Cell size = sheet_size / 4. Alpha carries the stroke.

OUTPUT:
    src/ui/picons_data.h - one `const uint8_t PICON_<NAME>[W*H]` per icon
    (8-bit alpha, row-major) plus a name->pointer table.

USAGE:
    python scripts/convert_picons.py [--size 48] [--src PATH]
"""
import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required:  pip install Pillow")

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SRC = Path.home() / "Downloads" / "Gemini_Generated_Image_7o2xgd7o2xgd7o2x.png"
OUT = ROOT / "src" / "ui" / "picons_data.h"

# Row-major cell index -> icon name. Matches the 4x4 neon sheet layout.
LAYOUT = [
    "WIFI",       # 0  wifi arcs + trident
    "BLUETOOTH",  # 1  bluetooth rune
    "TOWER",      # 2  RF tower radiating
    "SCREEN",     # 3  display with signal
    "REMOTE",     # 4  IR remote control
    "BROADCAST",  # 5  broadcast antenna, arrows out
    "WAVEFORM",   # 6  signal waveform / tridents
    "PIN",        # 7  map / GPS pin
    "FOLDER",     # 8  folder
    "CODE",       # 9  </> code brackets
    "GEAR",       # 10 hex gear / settings
    "CLOCK",      # 11 clock face
    "CHAIN",      # 12 linked chain / mesh nodes
    "SLIDERS",    # 13 sliders / tools
    "RJ45",       # 14 ethernet jack
    "RADIO",      # 15 radio tower + waves
]

GRID = 4


def slice_icons(src_path, size):
    sheet = Image.open(src_path).convert("RGBA")
    w, h = sheet.size
    if w % GRID or h % GRID:
        print(f"  note: sheet {w}x{h} not evenly divisible by {GRID}; cropping remainder")
    cw, ch = w // GRID, h // GRID

    icons = []
    for idx, name in enumerate(LAYOUT):
        r, c = divmod(idx, GRID)
        cell = sheet.crop((c * cw, r * ch, (c + 1) * cw, (r + 1) * ch))

        # Build a coverage map. Two source shapes are supported:
        #
        #  RGBA (PNG): alpha carries the art. Fold in luminance so the bright
        #  neon core reads stronger than the soft outer halo.
        #
        #  RGB (JPEG): there is no alpha, and a "transparent" export usually
        #  bakes the CHECKERBOARD in as real mid-gray pixels. Treating luminance
        #  as coverage would turn that checkerboard into a grey wash behind every
        #  icon -- the same visible box that plagued the first sheet. The neon
        #  strokes are strongly saturated while the checkerboard is pure grey, so
        #  gate on chroma: keep a pixel only if it is colourful OR bright enough
        #  to be a white-hot stroke core.
        px = list(cell.convert("RGB").getdata())
        has_alpha = cell.mode in ("RGBA", "LA")

        if has_alpha:
            alpha = cell.getchannel("A")
            lum = cell.convert("L")
            data = [(a * l) // 255 for a, l in zip(alpha.getdata(), lum.getdata())]
        else:
            data = []
            for (r, g, b) in px:
                hi, lo = max(r, g, b), min(r, g, b)
                chroma = hi - lo
                # Colourful stroke, or a near-white core; anything else (the grey
                # checkerboard, JPEG ringing around it) collapses to zero.
                if chroma >= 30 or hi >= 150:
                    data.append(hi)
                else:
                    data.append(0)

        cov = Image.new("L", cell.size)
        cov.putdata(data)

        # Trim to the icon's real bounding box so every glyph fills its tile
        # consistently (the source cells have uneven padding).
        bbox = cov.getbbox()
        if bbox:
            cov = cov.crop(bbox)

        # Fit into a square canvas preserving aspect, then Lanczos to final size.
        side = max(cov.size)
        square = Image.new("L", (side, side), 0)
        square.paste(cov, ((side - cov.width) // 2, (side - cov.height) // 2))
        small = square.resize((size, size), Image.LANCZOS)

        # Normalise so the brightest pixel is full coverage; keeps every icon
        # equally punchy regardless of how the generator lit it.
        peak = max(small.getdata())
        if peak and peak < 255:
            scale = 255.0 / peak
            small = small.point(lambda v, s=scale: min(255, int(v * s)))

        # Levels curve. The source art is a thin bright stroke wrapped in a
        # WIDE soft neon glow. Downsampled, that glow covers most of the tile
        # at low coverage, which (a) washes the icon out to a faint tint and
        # (b) paints a visible haze box over whatever is behind it.
        #
        # Clamping below LO to fully transparent removes the halo entirely
        # (so the icon composites cleanly against any background), and
        # saturating above HI makes the actual strokes solid. The result is
        # crisp line art rather than a glow blob.
        LO, HI = 0.16, 0.62
        span = HI - LO
        small = small.point(
            lambda v, lo=LO, sp=span: 0 if v < lo * 255 else
            min(255, int(((v / 255.0 - lo) / sp) * 255))
        )

        icons.append((name, list(small.getdata())))
        print(f"  [{idx:2d}] {name:<10s} peak={peak}")
    return icons


def emit(icons, size, src_path):
    lines = []
    lines.append("/*")
    lines.append(" * picons_data.h - AUTO-GENERATED. Do not edit by hand.")
    lines.append(f" * Regenerate: python scripts/convert_picons.py --size {size}")
    lines.append(f" * Source: {src_path.name}")
    lines.append(" *")
    lines.append(f" * {len(icons)} icons, {size}x{size}, 8-bit alpha coverage (row-major).")
    lines.append(" * Rendered by picons.cpp via per-pixel alpha blend against the")
    lines.append(" * background, so edges stay smooth at native size (NO upscaling).")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(f"#define PICON_W {size}")
    lines.append(f"#define PICON_H {size}")
    lines.append("")

    for name, data in icons:
        lines.append(f"static const uint8_t PICON_{name}[{size * size}] = {{")
        for row in range(size):
            chunk = data[row * size:(row + 1) * size]
            lines.append("    " + ",".join(f"{v:3d}" for v in chunk) + ",")
        lines.append("};")
        lines.append("")

    lines.append("struct picon_entry_t { const char *name; const uint8_t *data; };")
    lines.append("")
    lines.append(f"static const picon_entry_t PICON_TABLE[{len(icons)}] = {{")
    for name, _ in icons:
        lines.append(f'    {{ "{name.lower()}", PICON_{name} }},')
    lines.append("};")
    lines.append(f"#define PICON_COUNT {len(icons)}")
    lines.append("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines), encoding="ascii")
    kb = (size * size * len(icons)) / 1024.0
    print(f"\nwrote {OUT}")
    print(f"  {len(icons)} icons @ {size}x{size} = {kb:.1f} KB flash")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=48,
                    help="final icon edge in px (default 48)")
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--prefix", default="", help="symbol prefix, e.g. WIFI")
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()

    if not args.src.exists():
        sys.exit(f"source sheet not found: {args.src}")

    print(f"slicing {args.src.name} -> {args.size}x{args.size} alpha icons")
    icons = slice_icons(args.src, args.size)
    emit(icons, args.size, args.src)


if __name__ == "__main__":
    main()
