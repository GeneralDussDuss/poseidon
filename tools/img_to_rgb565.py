#!/usr/bin/env python3
"""
img_to_rgb565.py — convert any PNG/JPG into an RGB565 C header for POSEIDON.

Usage:
    python tools/img_to_rgb565.py input.png [output.h] [--name SYMBOL] [--max WxH]

Output: a C header with:
    static const uint16_t <name>_data[] = { ... };
    static const int      <name>_w = 96;
    static const int      <name>_h = 96;
    static const uint16_t <name>_alpha = 0xXXXX;  // color treated as transparent

If the input has alpha, pixels below 128 alpha are set to a magic color
(0xF81F magenta by default) and the splash code skips drawing those.

Resizes to fit within --max (default 96x96) preserving aspect ratio.
Requires Pillow (pip install Pillow).
"""

import sys, os
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("error: pip install Pillow")
    sys.exit(1)

def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def convert(inp, outp, name, maxw, maxh):
    img = Image.open(inp).convert("RGBA")
    img.thumbnail((maxw, maxh), Image.LANCZOS)
    w, h = img.size
    pixels = img.load()

    alpha_key = 0xF81F  # magenta — pick a rare color for transparency

    out = [
        f"/* Auto-generated from {inp} ({w}x{h}). */",
        f"#pragma once",
        f"#include <stdint.h>",
        f"",
        f"static const int {name}_w = {w};",
        f"static const int {name}_h = {h};",
        f"static const uint16_t {name}_alpha = 0x{alpha_key:04X};",
        f"",
        f"static const uint16_t {name}_data[{w * h}] = {{",
    ]

    data = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            if a < 128:
                data.append(alpha_key)
            else:
                v = rgb565(r, g, b)
                if v == alpha_key:  # collision: nudge
                    v ^= 0x0020
                data.append(v)

    for i in range(0, len(data), 12):
        row = ", ".join(f"0x{v:04X}" for v in data[i:i+12])
        out.append(f"    {row},")
    out.append("};")
    out.append("")

    Path(outp).write_text("\n".join(out))
    print(f"wrote {outp}: {w}x{h}, {len(data)*2} bytes")

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(0)
    inp = args[0]
    outp = "src/sprites/splash_sprite.h"
    name = "splash"
    maxw, maxh = 96, 96
    i = 1
    while i < len(args):
        if args[i] == "--name" and i + 1 < len(args):
            name = args[i + 1]; i += 2
        elif args[i] == "--max" and i + 1 < len(args):
            w, h = args[i + 1].split("x")
            maxw, maxh = int(w), int(h); i += 2
        elif args[i].startswith("-"):
            i += 1
        else:
            outp = args[i]; i += 1
    os.makedirs(os.path.dirname(outp) or ".", exist_ok=True)
    convert(inp, outp, name, maxw, maxh)

if __name__ == "__main__":
    main()
