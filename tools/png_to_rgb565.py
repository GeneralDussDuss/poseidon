"""
Convert a PNG to a 240x135 RGB565 C header for the SaltyJack splash.

Usage:
    python tools/png_to_rgb565.py <input.png> <output.h> <symbol_name>

Produces a header exporting `const uint16_t <symbol_name>[240*135]` suitable
for M5GFX's `pushImage(x, y, w, h, data)`.
"""
import sys
from PIL import Image

SCR_W = 240
SCR_H = 135

def rgb_to_565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def main(in_png, out_h, sym):
    img = Image.open(in_png).convert("RGB")
    img = img.resize((SCR_W, SCR_H), Image.LANCZOS)

    pixels = []
    for y in range(SCR_H):
        for x in range(SCR_W):
            r, g, b = img.getpixel((x, y))
            pixels.append(rgb_to_565(r, g, b))

    with open(out_h, "w", encoding="utf-8") as f:
        f.write(f"/* Auto-generated from {in_png} — do not hand-edit. */\n")
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define {sym.upper()}_W {SCR_W}\n")
        f.write(f"#define {sym.upper()}_H {SCR_H}\n\n")
        f.write(f"static const uint16_t {sym}[{SCR_W * SCR_H}] = {{\n")
        for i in range(0, len(pixels), 12):
            row = pixels[i:i + 12]
            f.write("    " + ", ".join(f"0x{v:04X}" for v in row) + ",\n")
        f.write("};\n")
    print(f"wrote {out_h}: {SCR_W}x{SCR_H}, {len(pixels)*2} bytes")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
