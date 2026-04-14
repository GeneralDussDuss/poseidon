#!/usr/bin/env python3
"""
img_to_rgb565.py — convert any PNG/JPG/GIF into RGB565 C headers for POSEIDON.

Usage:
    # single still image → splash_sprite.h (one frame)
    python tools/img_to_rgb565.py input.png

    # animated GIF → splash_anim.h (N frames + duration table)
    python tools/img_to_rgb565.py input.gif --anim

    # override output / symbol / size
    python tools/img_to_rgb565.py input.png -o src/sprites/mytest.h \\
        --name mytest --max 120x96

Output symbols (single image):
    splash_data[], splash_w, splash_h, splash_alpha

Output symbols (--anim):
    splash_anim_frames[N][w*h]
    splash_anim_w, splash_anim_h
    splash_anim_count    (N)
    splash_anim_delay_ms[N]
    splash_anim_alpha    (transparent color key)

Transparency: pixels with alpha < 128 become 0xF81F (magenta). The
splash blitter skips those.

Requires: pip install Pillow
"""

import sys, os
from pathlib import Path

try:
    from PIL import Image, ImageSequence
except ImportError:
    print("error: pip install Pillow"); sys.exit(1)

ALPHA_KEY = 0xF81F

def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def flatten(img, maxw, maxh):
    img = img.convert("RGBA")
    img.thumbnail((maxw, maxh), Image.LANCZOS)
    w, h = img.size
    px = img.load()
    data = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 128:
                data.append(ALPHA_KEY)
            else:
                v = rgb565(r, g, b)
                if v == ALPHA_KEY: v ^= 0x0020
                data.append(v)
    return w, h, data

def write_still(inp, outp, name, maxw, maxh):
    img = Image.open(inp)
    w, h, data = flatten(img, maxw, maxh)
    lines = [
        f"/* Auto-generated from {inp} ({w}x{h}). */",
        "#pragma once", "#include <stdint.h>", "",
        f"static const int {name}_w = {w};",
        f"static const int {name}_h = {h};",
        f"static const uint16_t {name}_alpha = 0x{ALPHA_KEY:04X};", "",
        f"static const uint16_t {name}_data[{w * h}] = {{",
    ]
    for i in range(0, len(data), 12):
        row = ", ".join(f"0x{v:04X}" for v in data[i:i+12])
        lines.append(f"    {row},")
    lines.append("};"); lines.append("")
    Path(outp).write_text("\n".join(lines))
    print(f"wrote {outp}: {w}x{h}, {len(data)*2} bytes")

def write_anim(inp, outp, name, maxw, maxh, max_frames):
    img = Image.open(inp)
    frames = []
    delays = []
    w = h = None
    for i, frame in enumerate(ImageSequence.Iterator(img)):
        if i >= max_frames: break
        fr = frame.copy()
        fw, fh, data = flatten(fr, maxw, maxh)
        if w is None: w, h = fw, fh
        if (fw, fh) != (w, h):
            # Pad to uniform size
            padded = [ALPHA_KEY] * (w * h)
            for y in range(min(fh, h)):
                for x in range(min(fw, w)):
                    padded[y * w + x] = data[y * fw + x]
            data = padded
        frames.append(data)
        delays.append(frame.info.get("duration", 100))
    n = len(frames)
    total_bytes = n * w * h * 2
    print(f"wrote {outp}: {n} frames, {w}x{h}, {total_bytes} bytes total")

    lines = [
        f"/* Auto-generated from {inp}: {n} frames, {w}x{h}. */",
        "#pragma once", "#include <stdint.h>", "",
        f"static const int {name}_w = {w};",
        f"static const int {name}_h = {h};",
        f"static const int {name}_count = {n};",
        f"static const uint16_t {name}_alpha = 0x{ALPHA_KEY:04X};", "",
        f"static const uint16_t {name}_delay_ms[{n}] = {{",
        "    " + ", ".join(str(dd) for dd in delays),
        "};", "",
        f"static const uint16_t {name}_frames[{n}][{w * h}] = {{",
    ]
    for fi, fdata in enumerate(frames):
        lines.append(f"  /* frame {fi} */ {{")
        for i in range(0, len(fdata), 12):
            row = ", ".join(f"0x{v:04X}" for v in fdata[i:i+12])
            lines.append(f"    {row},")
        lines.append("  },")
    lines.append("};"); lines.append("")
    Path(outp).write_text("\n".join(lines))

def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__); sys.exit(0)
    inp = args[0]

    is_anim = "--anim" in args
    outp = None
    name = "splash_anim" if is_anim else "splash"
    maxw, maxh = (120, 96) if is_anim else (96, 96)
    max_frames = 32

    i = 1
    while i < len(args):
        a = args[i]
        if a == "--name" and i + 1 < len(args):       name = args[i+1]; i += 2
        elif a in ("-o", "--out") and i + 1 < len(args): outp = args[i+1]; i += 2
        elif a == "--max" and i + 1 < len(args):
            w, h = args[i+1].split("x")
            maxw, maxh = int(w), int(h); i += 2
        elif a == "--frames" and i + 1 < len(args):
            max_frames = int(args[i+1]); i += 2
        elif a == "--anim":                             i += 1
        elif a.startswith("-"):                         i += 1
        else:                                           outp = a; i += 1

    if outp is None:
        outp = "src/sprites/splash_anim.h" if is_anim else "src/sprites/splash_sprite.h"
    os.makedirs(os.path.dirname(outp) or ".", exist_ok=True)

    if is_anim:
        write_anim(inp, outp, name, maxw, maxh, max_frames)
    else:
        write_still(inp, outp, name, maxw, maxh)

if __name__ == "__main__":
    main()
