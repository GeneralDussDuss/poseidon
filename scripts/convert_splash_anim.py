#!/usr/bin/env python3
"""
convert_splash_anim.py - turn the POSEIDON type-motion GIF into a compact
animated boot splash for the T-Embed's 320x170 panel.

WHY RLE + PALETTE:
    Raw RGB565 at 320x170 is 108 KB per frame. A 16-frame splash would be
    1.7 MB of flash. The art is neon strokes on black, so per-frame palette
    quantisation (32 colours) collapses the gradient noise into flat regions,
    and run-length encoding then compresses those hard: ~35 KB/frame, about
    a 3x saving, with no visible loss at this screen size.

FORMAT (per frame):
    palette : PAL_N x uint16 RGB565
    stream  : pairs of (index, runlength) as uint8, runlength 1..255,
              scanning row-major across the WHOLE frame (runs may cross rows,
              which is what makes the black background compress so well).
    Decoder streams into a single row buffer, so playback needs ~640 bytes
    of RAM regardless of frame size (no PSRAM dependency).

USAGE:
    python scripts/convert_splash_anim.py [--frames 16] [--colors 32]
"""
import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required:  pip install Pillow")

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SRC = Path.home() / "Downloads" / "typemotion-1786581490907.gif"
OUT = ROOT / "src" / "sprites" / "splash_anim.h"

W, H = 320, 170


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def encode_frame(img, ncolors):
    """-> (palette list[uint16], stream bytes[(idx,run)...])"""
    q = img.quantize(colors=ncolors, method=Image.MEDIANCUT, dither=Image.NONE)
    # A frame that uses fewer distinct colours than requested (the all-black
    # opening frame, for one) returns a SHORT palette. Pad to a fixed width so
    # every frame's table is the same size and the decoder can index blindly.
    pal_raw = list(q.getpalette() or [])[: ncolors * 3]
    pal_raw += [0] * (ncolors * 3 - len(pal_raw))
    palette = [
        rgb565(pal_raw[i * 3], pal_raw[i * 3 + 1], pal_raw[i * 3 + 2])
        for i in range(ncolors)
    ]

    px = list(q.getdata())
    stream = []
    i, n = 0, len(px)
    while i < n:
        v = px[i]
        run = 1
        while i + run < n and px[i + run] == v and run < 255:
            run += 1
        stream.append((v, run))
        i += run
    return palette, stream


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--colors", type=int, default=32)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    args = ap.parse_args()

    if not args.src.exists():
        sys.exit(f"source not found: {args.src}")
    if not (2 <= args.colors <= 256):
        sys.exit("--colors must be 2..256")

    im = Image.open(args.src)
    total = getattr(im, "n_frames", 1)

    # Sample evenly across the animation, always including the final frame so
    # the splash ends on the fully-revealed wordmark.
    if args.frames >= total:
        picks = list(range(total))
    else:
        step = (total - 1) / float(args.frames - 1)
        picks = sorted({int(round(k * step)) for k in range(args.frames)})

    print(f"source {args.src.name}: {im.size}, {total} frames")
    print(f"encoding {len(picks)} frames -> {W}x{H}, {args.colors} colours\n")

    frames = []
    for fi in picks:
        im.seek(fi)
        rgb = im.convert("RGB").resize((W, H), Image.LANCZOS)
        pal, stream = encode_frame(rgb, args.colors)
        frames.append((fi, pal, stream))
        print(f"  frame {fi:3d}: {len(stream):6d} runs  ({len(stream)*2/1024:6.1f} KB)")

    lines = []
    lines.append("/*")
    lines.append(" * splash_anim.h - AUTO-GENERATED. Do not edit by hand.")
    lines.append(f" * Regenerate: python scripts/convert_splash_anim.py"
                 f" --frames {args.frames} --colors {args.colors}")
    lines.append(f" * Source: {args.src.name}")
    lines.append(" *")
    lines.append(f" * {len(frames)} frames, {W}x{H}, {args.colors}-colour palette + RLE.")
    lines.append(" * Each frame: SPLASH_PAL_n[] (RGB565) + SPLASH_RLE_n[] as")
    lines.append(" * (index, runlength) byte pairs scanning row-major across the frame.")
    lines.append(" * Decode with splash_anim_play() in src/splash_anim.cpp.")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(f"#define SPLASH_ANIM_W      {W}")
    lines.append(f"#define SPLASH_ANIM_H      {H}")
    lines.append(f"#define SPLASH_ANIM_FRAMES {len(frames)}")
    lines.append(f"#define SPLASH_ANIM_COLORS {args.colors}")
    lines.append("")

    total_bytes = 0
    for n, (fi, pal, stream) in enumerate(frames):
        lines.append(f"/* frame {n} (source index {fi}) */")
        lines.append(f"static const uint16_t SPLASH_PAL_{n}[{len(pal)}] = {{")
        for r in range(0, len(pal), 8):
            lines.append("    " + ",".join(f"0x{c:04X}" for c in pal[r:r + 8]) + ",")
        lines.append("};")

        flat = []
        for idx, run in stream:
            flat.append(idx)
            flat.append(run)
        total_bytes += len(flat) + len(pal) * 2

        lines.append(f"static const uint8_t SPLASH_RLE_{n}[{len(flat)}] = {{")
        for r in range(0, len(flat), 24):
            lines.append("    " + ",".join(str(v) for v in flat[r:r + 24]) + ",")
        lines.append("};")
        lines.append("")

    lines.append("struct splash_frame_t {")
    lines.append("    const uint16_t *pal;")
    lines.append("    const uint8_t  *rle;")
    lines.append("    uint32_t        rle_len;   /* bytes, = 2 * runs */")
    lines.append("};")
    lines.append("")
    lines.append(f"static const splash_frame_t SPLASH_ANIM[{len(frames)}] = {{")
    for n, (_, pal, stream) in enumerate(frames):
        lines.append(f"    {{ SPLASH_PAL_{n}, SPLASH_RLE_{n}, {len(stream) * 2} }},")
    lines.append("};")
    lines.append("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines), encoding="ascii")
    print(f"\nwrote {OUT}")
    print(f"  {len(frames)} frames, {total_bytes/1024:.1f} KB flash "
          f"(raw RGB565 would be {len(frames)*W*H*2/1024:.1f} KB)")


if __name__ == "__main__":
    main()
