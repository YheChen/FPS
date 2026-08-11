#!/usr/bin/env python3
"""Procedural tiling textures for the greybox arena, written as PNGs.

Pure stdlib (zlib + struct): no Pillow, no downloads, reproducible anywhere.
Each texture is seamless (all noise wraps modulo the size) so it tiles across
large surfaces without visible seams.

Every surface is emitted twice: an albedo map and a matching tangent-space
normal map. Both come out of the SAME height field, so they cannot drift --
the seam that darkens the albedo is by construction the seam that dents the
surface. The normal is a central difference of that height field, taken with
wrapping indices; a non-wrapping difference would leave a one-texel seam line
down two edges of an otherwise seamless texture.

Usage: python3 tools/gen_textures.py [outdir]   (default assets/textures/)
"""

import math
import random
import struct
import sys
import zlib
from pathlib import Path

SIZE = 512

# Normal maps ship at half the albedo's resolution. Every byte under assets/
# is downloaded by every browser player, and these textures are paid for
# THREE times over: once loose and once inside each of the two maps that
# embed them. At full resolution the normal maps alone would add ~1.7 MB to
# that download. What they would buy is derivative noise finer than a metre's
# viewing distance resolves, which mips away almost immediately; the relief
# that actually reads -- panel seams, rivets, grout, brush streaks, the
# floor's undulation -- is low-frequency and survives the halving intact.
NORMAL_SIZE = SIZE // 2

# Feature sizes below are written in units of this rather than in raw texels.
# They were tuned at 256; scaling them with the resolution is what makes a
# bigger texture mean more detail rather than the same detail at half the
# physical size.
TEXEL = SIZE // 256

# Height fields are in [0,1] over a UV square, so the slope a feature produces
# depends on the texture's world size, not on its resolution. These numbers are
# the vertical exaggeration each material gets: bigger is a deeper relief.
CONCRETE_RELIEF = 0.030
WALL_RELIEF = 0.008
METAL_RELIEF = 0.005
PLATFORM_RELIEF = 0.008


def paeth(a, b, c):
    """PNG's Paeth predictor: whichever of left/up/up-left it sits nearest."""
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_row(row, prev, bpp=3):
    """Returns (filter_type, filtered_bytes) for one scanline.

    PNG lets each scanline pick its own predictor, and zlib then only has to
    compress the residual.
    """
    candidates = [(0, row)]

    sub = bytearray(len(row))
    up = bytearray(len(row))
    average = bytearray(len(row))
    paeth_row = bytearray(len(row))
    for i, value in enumerate(row):
        left = row[i - bpp] if i >= bpp else 0
        above = prev[i]
        upper_left = prev[i - bpp] if i >= bpp else 0
        sub[i] = (value - left) & 0xFF
        up[i] = (value - above) & 0xFF
        average[i] = (value - ((left + above) >> 1)) & 0xFF
        paeth_row[i] = (value - paeth(left, above, upper_left)) & 0xFF
    candidates += [(1, sub), (2, up), (3, average), (4, paeth_row)]

    # The spec's own heuristic: treat the residuals as signed and take the
    # scanline with the smallest total magnitude.
    def cost(entry):
        return sum(v if v < 128 else 256 - v for v in entry[1])

    return min(candidates, key=cost)


def write_png(path: Path, pixels, size=SIZE):
    """pixels: flat list of (r,g,b) rows, size*size entries."""
    # Two candidate streams: every scanline unfiltered (what this script used
    # to write), and every scanline filtered by the PNG spec's own heuristic.
    # Filtering is worth ~40% on the normal maps, whose channels are smooth
    # ramps a predictor nails, and costs ~5% on the albedo maps, whose speckle
    # is white noise that a predictor can only smear. The heuristic cannot
    # tell those apart because it minimises residual magnitude rather than
    # compressed size -- so compress both and keep whichever actually won.
    plain = bytearray()
    filtered = bytearray()
    previous = bytes(size * 3)
    for y in range(size):
        row = bytearray()
        for x in range(size):
            r, g, b = pixels[y * size + x]
            row += bytes((r, g, b))
        plain.append(0)
        plain += row
        filter_type, data = filter_row(row, previous)
        filtered.append(filter_type)
        filtered += data
        previous = row

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    idat = min(zlib.compress(bytes(plain), 9), zlib.compress(bytes(filtered), 9), key=len)
    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)  # 8-bit RGB
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", idat)
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


def box_downsample(height, factor):
    """Averages a SIZE x SIZE height field down to SIZE//factor per side.

    Averaging before differencing rather than after: a point sample would
    alias the height field's finest octave straight into the slopes, and the
    average is also the mip level the GPU would have picked anyway.
    """
    size = SIZE // factor
    out = [0.0] * (size * size)
    scale = 1.0 / float(factor * factor)
    for y in range(size):
        for x in range(size):
            total = 0.0
            for dy in range(factor):
                row = (y * factor + dy) * SIZE + x * factor
                for dx in range(factor):
                    total += height[row + dx]
            out[y * size + x] = total * scale
    return out


def normal_map(height, relief, size=NORMAL_SIZE):
    """Tangent-space normal map from a height field, as (r,g,b) tuples.

    Central differences in UV space (hence the * size), so a material reads
    the same however finely it is sampled. Neighbour lookups wrap, which is
    what keeps the normal map as seamless as the albedo it came from -- a
    clamped difference would leave a one-texel ridge down two edges.

    +X is +U and +Y is +V, matching the tangent frame the engine derives from
    the mesh's UVs; the shader rebuilds the surface normal with that same
    convention, so the two only have to agree with each other.
    """
    if size != SIZE:
        height = box_downsample(height, SIZE // size)
    out = []
    for y in range(size):
        row_up = ((y - 1) % size) * size
        row_down = ((y + 1) % size) * size
        row = y * size
        for x in range(size):
            left = height[row + (x - 1) % size]
            right = height[row + (x + 1) % size]
            du = (right - left) * 0.5 * size * relief
            dv = (height[row_down + x] - height[row_up + x]) * 0.5 * size * relief
            inv = 1.0 / math.sqrt(du * du + dv * dv + 1.0)
            out.append((
                clamp8((-du * inv * 0.5 + 0.5) * 255.0 + 0.5),
                clamp8((-dv * inv * 0.5 + 0.5) * 255.0 + 0.5),
                clamp8((inv * 0.5 + 0.5) * 255.0 + 0.5),
            ))
    return out


def concrete(seed=1):
    """Floor: mottled grey concrete with fine speckle.

    Returns (albedo, height). The aggregate flecks stay out of the height
    field on purpose: they are one texel wide, so they sit at Nyquist, mip
    away to nothing past a couple of metres, and shimmer on the way there.
    They also happen to be pure white noise, which is the one thing PNG
    cannot compress -- putting them in the normal map tripled that file.
    The broad fbm undulation is the relief a floor actually reads as.
    """
    rng = random.Random(seed)
    noise = fbm(rng, octaves=5, base_cells=4)
    speck = random.Random(seed + 99)
    pixels, height = [], []
    for i in range(SIZE * SIZE):
        base = 118 + noise[i] * 46
        if speck.random() < 0.04:  # dark aggregate flecks
            base -= 26
        pixels.append((clamp8(base), clamp8(base * 1.01), clamp8(base * 1.04)))
        height.append(noise[i])
    return pixels, height


def wall_panels(seed=2):
    """Walls: horizontal metal panels with recessed seams and rivets."""
    rng = random.Random(seed)
    noise = fbm(rng, octaves=4, base_cells=8)
    pixels, height = [], []
    panel_h = SIZE // 4
    for y in range(SIZE):
        row_in_panel = y % panel_h
        seam = row_in_panel < 2 * TEXEL or row_in_panel >= panel_h - 2 * TEXEL
        rivet_row = row_in_panel // TEXEL in (4, panel_h // TEXEL - 5)
        for x in range(SIZE):
            base = 150 + noise[y * SIZE + x] * 34
            h = noise[y * SIZE + x] * 0.5
            # Dark seam lines between panels.
            if seam:
                base *= 0.62
                h -= 0.5
            # Rivets along the seams.
            if rivet_row and (x % (32 * TEXEL)) < 3 * TEXEL:
                base *= 1.25
                h += 0.35
            pixels.append((clamp8(base * 1.02), clamp8(base * 0.96), clamp8(base * 0.86)))
            height.append(h)
    return pixels, height


def metal(seed=3):
    """Pillars: brushed blue-grey metal."""
    rng = random.Random(seed)
    noise = fbm(rng, octaves=3, base_cells=16)
    streak = random.Random(seed + 7)
    columns = [streak.random() for _ in range(SIZE)]
    # Brush marks are one column wide at any resolution, so at 512 they are
    # the Nyquist frequency: sharp in the albedo (which mipmaps to a flat
    # tint) but a mess of alternating slopes in the normal map. A wrapping
    # 3-tap blur keeps them as streaks instead of per-texel noise.
    columns = [
        (columns[(x - 1) % SIZE] + 2.0 * columns[x] + columns[(x + 1) % SIZE]) * 0.25
        for x in range(SIZE)
    ]
    pixels, height = [], []
    for y in range(SIZE):
        for x in range(SIZE):
            base = 96 + noise[y * SIZE + x] * 30 + columns[x] * 18
            pixels.append((clamp8(base * 0.82), clamp8(base * 0.94), clamp8(base * 1.18)))
            height.append(noise[y * SIZE + x] * 0.4 + columns[x] * 0.6)
    return pixels, height


def platform_tiles(seed=4):
    """Center platform: chunky tiles with grout, tinted green."""
    rng = random.Random(seed)
    noise = fbm(rng, octaves=4, base_cells=8)
    pixels, height = [], []
    tile = SIZE // 4
    for y in range(SIZE):
        for x in range(SIZE):
            base = 120 + noise[y * SIZE + x] * 34
            h = noise[y * SIZE + x] * 0.3 + 0.6
            if (x % tile) < 3 * TEXEL or (y % tile) < 3 * TEXEL:  # grout
                base *= 0.6
                h -= 0.6
            pixels.append((clamp8(base * 0.78), clamp8(base * 1.12), clamp8(base * 0.86)))
            height.append(h)
    return pixels, height


# name -> (generator, vertical exaggeration for the normal map)
TEXTURES = [
    ("concrete", concrete, CONCRETE_RELIEF),
    ("wall", wall_panels, WALL_RELIEF),
    ("metal", metal, METAL_RELIEF),
    ("platform", platform_tiles, PLATFORM_RELIEF),
]


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        Path(__file__).resolve().parent.parent / "assets" / "textures"
    )
    for name, generate, relief in TEXTURES:
        albedo, height = generate()
        write_png(out / f"{name}.png", albedo)
        write_png(out / f"{name}_normal.png", normal_map(height, relief), NORMAL_SIZE)


if __name__ == "__main__":
    main()
