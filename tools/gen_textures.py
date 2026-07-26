#!/usr/bin/env python3
"""Procedural tiling textures for the greybox arena, written as PNGs.

Pure stdlib (zlib + struct): no Pillow, no downloads, reproducible anywhere.
Each texture is seamless (all noise wraps modulo the size) so it tiles across
large surfaces without visible seams.

Usage: python3 tools/gen_textures.py [outdir]   (default assets/textures/)
"""

import math
import random
import struct
import sys
import zlib
from pathlib import Path

SIZE = 256


def write_png(path: Path, pixels, size=SIZE):
    """pixels: flat list of (r,g,b) rows, size*size entries."""
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter type 0 (None) per scanline
        for x in range(size):
            r, g, b = pixels[y * size + x]
            raw += bytes((r, g, b))

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)  # 8-bit RGB
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)
    print(f"wrote {path} ({len(png)} bytes)")


def clamp8(v):
    return max(0, min(255, int(v)))


def value_noise(rng, cells, size=SIZE):
    """Seamless value noise: a lattice that wraps, smoothly interpolated."""
    lattice = [[rng.random() for _ in range(cells)] for _ in range(cells)]
    step = size / cells
    out = [0.0] * (size * size)
    for y in range(size):
        fy = y / step
        y0 = int(fy) % cells
        y1 = (y0 + 1) % cells
        ty = fy - int(fy)
        ty = ty * ty * (3 - 2 * ty)  # smoothstep
        for x in range(size):
            fx = x / step
            x0 = int(fx) % cells
            x1 = (x0 + 1) % cells
            tx = fx - int(fx)
            tx = tx * tx * (3 - 2 * tx)
            top = lattice[y0][x0] * (1 - tx) + lattice[y0][x1] * tx
            bot = lattice[y1][x0] * (1 - tx) + lattice[y1][x1] * tx
            out[y * size + x] = top * (1 - ty) + bot * ty
    return out


def fbm(rng, octaves=4, base_cells=4):
    """Fractal sum of seamless noise octaves, normalized to [0,1]."""
    total = [0.0] * (SIZE * SIZE)
    amplitude = 1.0
    norm = 0.0
    cells = base_cells
    for _ in range(octaves):
        layer = value_noise(rng, cells)
        for i in range(SIZE * SIZE):
            total[i] += layer[i] * amplitude
        norm += amplitude
        amplitude *= 0.5
        cells *= 2
    return [v / norm for v in total]


def concrete(seed=1):
    """Floor: mottled grey concrete with fine speckle."""
    rng = random.Random(seed)
    noise = fbm(rng, octaves=5, base_cells=4)
    speck = random.Random(seed + 99)
    pixels = []
    for i in range(SIZE * SIZE):
        base = 118 + noise[i] * 46
        if speck.random() < 0.04:  # dark aggregate flecks
            base -= 26
        pixels.append((clamp8(base), clamp8(base * 1.01), clamp8(base * 1.04)))
    return pixels


def wall_panels(seed=2):
    """Walls: horizontal metal panels with recessed seams and rivets."""
    rng = random.Random(seed)
    noise = fbm(rng, octaves=4, base_cells=8)
    pixels = []
    panel_h = SIZE // 4
    for y in range(SIZE):
        row_in_panel = y % panel_h
        for x in range(SIZE):
            base = 150 + noise[y * SIZE + x] * 34
            # Dark seam lines between panels.
            if row_in_panel < 2 or row_in_panel > panel_h - 3:
                base *= 0.62
            # Rivets along the seams.
            if row_in_panel in (4, panel_h - 5) and (x % 32) < 3:
                base *= 1.25
            pixels.append((clamp8(base * 1.02), clamp8(base * 0.96), clamp8(base * 0.86)))
    return pixels


def metal(seed=3):
    """Pillars: brushed blue-grey metal."""
    rng = random.Random(seed)
    noise = fbm(rng, octaves=3, base_cells=16)
    streak = random.Random(seed + 7)
    columns = [streak.random() for _ in range(SIZE)]
    pixels = []
    for y in range(SIZE):
        for x in range(SIZE):
            base = 96 + noise[y * SIZE + x] * 30 + columns[x] * 18
            pixels.append((clamp8(base * 0.82), clamp8(base * 0.94), clamp8(base * 1.18)))
    return pixels


def platform_tiles(seed=4):
    """Center platform: chunky tiles with grout, tinted green."""
    rng = random.Random(seed)
    noise = fbm(rng, octaves=4, base_cells=8)
    pixels = []
    tile = SIZE // 4
    for y in range(SIZE):
        for x in range(SIZE):
            base = 120 + noise[y * SIZE + x] * 34
            if (x % tile) < 3 or (y % tile) < 3:  # grout
                base *= 0.6
            pixels.append((clamp8(base * 0.78), clamp8(base * 1.12), clamp8(base * 0.86)))
    return pixels


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        Path(__file__).resolve().parent.parent / "assets" / "textures"
    )
    write_png(out / "concrete.png", concrete())
    write_png(out / "wall.png", wall_panels())
    write_png(out / "metal.png", metal())
    write_png(out / "platform.png", platform_tiles())


if __name__ == "__main__":
    main()
