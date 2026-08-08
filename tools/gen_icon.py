#!/usr/bin/env python3
"""Generates web/favicon.ico -- a crosshair, drawn fresh at every size.

Same approach as the other asset generators here: the icon is committed, but
so is the thing that made it, so a colour or weight change is an edit rather
than a round trip through an image editor.

Icons are NOT scaled down from one master. A 48x48 crosshair resampled to
16x16 turns into four grey smudges, because the arms land between pixels. Each
size is instead drawn on its own integer grid, so every edge is exact.

Output is a classic multi-size .ico with uncompressed 32-bit BGRA images.
PNG-in-ICO is smaller and modern browsers accept it, but BMP is what every
tool in the chain understands, and the whole file is still ~10 KB.

Usage: python3 tools/gen_icon.py [--preview]
"""

import argparse
import struct
from pathlib import Path

# Matches web/shell.html's page background, so the icon does not look like a
# sticker sitting on top of the app.
BACKGROUND = (0x0E, 0x12, 0x16)
# The HUD's "active weapon" amber (game/client/main.cpp), which is as close to
# a brand colour as this project has.
CROSSHAIR = (0xFF, 0xD9, 0x59)

SIZES = (16, 32, 48)


def draw(size):
    """Returns size*size RGB tuples, top-down."""
    # Proportions chosen so the 16px case lands on whole pixels; the larger
    # sizes then follow the same shape rather than a different one.
    thickness = max(2, round(size * 0.115))
    margin = max(1, round(size * 0.09))
    gap = max(2, round(size * 0.20))

    pixels = [BACKGROUND] * (size * size)

    def fill(x0, y0, x1, y1):
        for y in range(max(0, y0), min(size, y1)):
            for x in range(max(0, x0), min(size, x1)):
                pixels[y * size + x] = CROSSHAIR

    centre = size // 2
    # Centre the bar on the pixel grid: for an even thickness this straddles
    # the middle two columns, which is what keeps it looking symmetrical.
    lo = centre - thickness // 2
    hi = lo + thickness

    # Four arms, leaving a gap around the middle.
    fill(lo, margin, hi, centre - gap)               # up
    fill(lo, centre + gap, hi, size - margin)        # down
    fill(margin, lo, centre - gap, hi)               # left
    fill(centre + gap, lo, size - margin, hi)        # right

    # Centre dot, so the icon still reads as a crosshair at 16px where the
    # arms are only a couple of pixels long.
    dot = max(2, thickness)
    d_lo = centre - dot // 2
    fill(d_lo, d_lo, d_lo + dot, d_lo + dot)

    return pixels


def bmp_image(size, pixels):
    """One ICO entry payload: BITMAPINFOHEADER + BGRA + AND mask."""
    # Height is doubled in the header: the format expects the colour image and
    # a 1-bit transparency mask stacked, even when the colour image already
    # carries alpha.
    header = struct.pack(
        "<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0
    )

    # BMP rows run bottom-up.
    body = bytearray()
    for y in reversed(range(size)):
        for x in range(size):
            r, g, b = pixels[y * size + x]
            body += bytes((b, g, r, 0xFF))

    # Fully opaque, so the mask is all zeros -- but the rows still have to be
    # padded to 4 bytes or every renderer disagrees about where they start.
    mask_row = ((size + 31) // 32) * 4
    mask = bytes(mask_row * size)

    return header + bytes(body) + mask


def build_ico(sizes):
    images = [bmp_image(s, draw(s)) for s in sizes]

    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, image in zip(sizes, images):
        out += struct.pack(
            "<BBBBHHII", size, size, 0, 0, 1, 32, len(image), offset
        )
        offset += len(image)
    for image in images:
        out += image
    return bytes(out)


def preview(size):
    pixels = draw(size)
    print(f"\n{size}x{size}:")
    for y in range(size):
        row = "".join(
            "##" if pixels[y * size + x] == CROSSHAIR else ".."
            for x in range(size)
        )
        print(f"  {row}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--preview", action="store_true", help="print ASCII art of each size"
    )
    parser.add_argument(
        "--out", default="web/favicon.ico", help="output path"
    )
    args = parser.parse_args()

    if args.preview:
        for size in SIZES:
            preview(size)

    data = build_ico(SIZES)
    path = Path(args.out)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    print(
        f"wrote {path} ({len(data)} bytes, sizes {', '.join(map(str, SIZES))})"
    )


if __name__ == "__main__":
    main()
