"""
Slice a 7-cell horizontal sprite sheet into 7 icons, auto-crop each to
content, resize to 24x24, and emit a single C header of RGB565 arrays.

Usage:
    python tools/sprite_sheet_to_icons.py <input.png> <output.h>
"""
import sys
from PIL import Image

N_CELLS = 7
ICON_SIZE = 16
NAMES = [
    "sj_spr_flag",
    "sj_spr_skull",
    "sj_spr_swords",
    "sj_spr_wheel",
    "sj_spr_horn",
    "sj_spr_web",
    "sj_spr_key",
]

def rgb_to_565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def content_bbox(img, dark_threshold=24):
    """Find bounding box of non-near-black content."""
    w, h = img.size
    px = img.load()
    min_x, min_y, max_x, max_y = w, h, 0, 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y][:3]
            if r > dark_threshold or g > dark_threshold or b > dark_threshold:
                if x < min_x: min_x = x
                if y < min_y: min_y = y
                if x > max_x: max_x = x
                if y > max_y: max_y = y
    if min_x > max_x:
        return (0, 0, w, h)
    return (min_x, min_y, max_x + 1, max_y + 1)

def extract_icon(sheet, cell_idx):
    sw, sh = sheet.size
    cell_w = sw / N_CELLS
    x0 = int(round(cell_idx * cell_w))
    x1 = int(round((cell_idx + 1) * cell_w))
    cell = sheet.crop((x0, 0, x1, sh))
    bbox = content_bbox(cell)
    cropped = cell.crop(bbox)

    # Pad to square so icons don't get stretched weirdly
    cw, ch = cropped.size
    side = max(cw, ch)
    sq = Image.new("RGB", (side, side), (0, 0, 0))
    sq.paste(cropped, ((side - cw) // 2, (side - ch) // 2))

    return sq.resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS)

def emit_icon(f, name, img):
    f.write(f"/* {name} — {ICON_SIZE}x{ICON_SIZE} RGB565 */\n")
    f.write(f"static const uint16_t {name}[{ICON_SIZE * ICON_SIZE}] = {{\n")
    px = img.load()
    vals = []
    for y in range(ICON_SIZE):
        for x in range(ICON_SIZE):
            r, g, b = px[x, y][:3]
            vals.append(rgb_to_565(r, g, b))
    for i in range(0, len(vals), 12):
        row = vals[i:i + 12]
        f.write("    " + ", ".join(f"0x{v:04X}" for v in row) + ",\n")
    f.write("};\n\n")

def main(in_png, out_h):
    sheet = Image.open(in_png).convert("RGB")
    print(f"sheet: {sheet.size[0]}x{sheet.size[1]}, {N_CELLS} cells")

    with open(out_h, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated sprite sheet — see tools/sprite_sheet_to_icons.py */\n")
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define SJ_SPRITE_W {ICON_SIZE}\n")
        f.write(f"#define SJ_SPRITE_H {ICON_SIZE}\n\n")
        for i in range(N_CELLS):
            icon = extract_icon(sheet, i)
            emit_icon(f, NAMES[i], icon)
    total = N_CELLS * ICON_SIZE * ICON_SIZE * 2
    print(f"wrote {out_h}: {total} bytes flash ({ICON_SIZE}x{ICON_SIZE} × {N_CELLS})")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
