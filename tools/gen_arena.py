#!/usr/bin/env python3
"""Generates the deathmatch maps in assets/maps/.

Pure-python GLB (glTF 2.0 binary) writer: no external dependencies, so the
maps can be regenerated anywhere. Each map is built from boxes, each emitted
as its own mesh so UVs can be scaled to WORLD size - that keeps texel density
constant, instead of stretching one 0..1 UV square across a 40 m floor.

Textures (from tools/gen_textures.py) are embedded in the .glb as PNG buffer
views, so each map is a single self-contained file.

Conventions: meters, +Y up, -Z forward (glTF standard). Floor top at y = 0.
Every layout must be fully enclosed by walls: nothing stops a player who
walks off an edge, because there is no kill volume.

A layout is just boxes and spawn points, so a new map is data here rather
than an art pipeline. The engine finds spawns by node name (`spawn_N`) and
collision from every mesh, so nothing downstream needs to know a map exists.

Usage:
    python3 tools/gen_arena.py                    # regenerate every map
    python3 tools/gen_arena.py --map arena02      # just one
    python3 tools/gen_arena.py --map arena01 --out /tmp/a.glb
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

# Texels per meter: how densely textures tile across surfaces.
TEXTURE_SCALE = 0.5

# name -> (texture file, base color factor)
MATERIALS = [
    ("mat_floor", "concrete.png", [0.85, 0.85, 0.88, 1.0]),
    ("mat_wall", "wall.png", [0.9, 0.88, 0.85, 1.0]),
    ("mat_pillar", "metal.png", [0.85, 0.9, 1.0, 1.0]),
    ("mat_platform", "platform.png", [0.9, 1.0, 0.9, 1.0]),
]
MATERIAL_INDEX = {name: i for i, (name, _, _) in enumerate(MATERIALS)}

# arena01 - a symmetric 40x40 box around a raised centre platform. Fights
# converge on the middle, which is the highest ground and the most exposed.
#
# (name, material, translation, scale, rotation_z_degrees|None)
ARENA01_BOXES = [
    ("floor", "mat_floor", (0, -0.5, 0), (40, 1, 40), None),
    ("wall_north", "mat_wall", (0, 2, -20.5), (42, 4, 1), None),
    ("wall_south", "mat_wall", (0, 2, 20.5), (42, 4, 1), None),
    ("wall_east", "mat_wall", (20.5, 2, 0), (1, 4, 42), None),
    ("wall_west", "mat_wall", (-20.5, 2, 0), (1, 4, 42), None),
    ("pillar_ne", "mat_pillar", (8, 1.5, -8), (2, 3, 2), None),
    ("pillar_nw", "mat_pillar", (-8, 1.5, -8), (2, 3, 2), None),
    ("pillar_se", "mat_pillar", (8, 1.5, 8), (2, 3, 2), None),
    ("pillar_sw", "mat_pillar", (-8, 1.5, 8), (2, 3, 2), None),
    ("platform_center", "mat_platform", (0, 0.75, 0), (6, 1.5, 6), None),
    ("ramp_east", "mat_platform", (4.8, 0.65, 0), (4.2, 0.3, 3), -16),
    ("ramp_west", "mat_platform", (-4.8, 0.65, 0), (4.2, 0.3, 3), 16),
    ("cover_n", "mat_pillar", (0, 0.6, -13), (5, 1.2, 1.2), None),
    ("cover_s", "mat_pillar", (0, 0.6, 13), (5, 1.2, 1.2), None),
]

ARENA01_SPAWNS = [
    (15, 0.1, 15), (-15, 0.1, 15), (15, 0.1, -15), (-15, 0.1, -15),
    (0, 0.1, 16), (0, 0.1, -16), (16, 0.1, 0), (-16, 0.1, 0),
]

# arena02 - a 48x32 hall with the high ground pushed to opposite ENDS instead
# of the middle. That inverts arena01's shape of fight: holding height means
# giving up the centre rather than owning it, and the long axis gives the
# sniper sightlines arena01 never has.
#
# Each platform is reached by two ramps, so a defender cannot cover the only
# way up; the staggered cover down the middle is what makes crossing the open
# span survivable.
ARENA02_BOXES = [
    ("floor", "mat_floor", (0, -0.5, 0), (48, 1, 32), None),
    ("wall_north", "mat_wall", (0, 2, -16.5), (50, 4, 1), None),
    ("wall_south", "mat_wall", (0, 2, 16.5), (50, 4, 1), None),
    ("wall_east", "mat_wall", (24.5, 2, 0), (1, 4, 34), None),
    ("wall_west", "mat_wall", (-24.5, 2, 0), (1, 4, 34), None),
    # The two galleries. They stop short of the north/south walls, leaving a
    # ground-level lane behind each one to flank along.
    ("platform_east", "mat_platform", (19, 0.75, 0), (10, 1.5, 20), None),
    ("platform_west", "mat_platform", (-19, 0.75, 0), (10, 1.5, 20), None),
    # Same geometry as arena01's ramps, which are known to be climbable: the
    # top surface lands ~0.13 m under the platform lip, well inside step
    # height. Sign of the rotation is what decides which end is the high one.
    ("ramp_east_n", "mat_platform", (12, 0.65, -6), (4.2, 0.3, 3), 16),
    ("ramp_east_s", "mat_platform", (12, 0.65, 6), (4.2, 0.3, 3), 16),
    ("ramp_west_n", "mat_platform", (-12, 0.65, -6), (4.2, 0.3, 3), -16),
    ("ramp_west_s", "mat_platform", (-12, 0.65, 6), (4.2, 0.3, 3), -16),
    ("pillar_n", "mat_pillar", (0, 1.5, -12), (2, 3, 2), None),
    ("pillar_s", "mat_pillar", (0, 1.5, 12), (2, 3, 2), None),
    ("cover_centre", "mat_pillar", (0, 0.6, 0), (3, 1.2, 3), None),
    ("cover_nw", "mat_pillar", (-6, 0.6, -8), (4, 1.2, 1.2), None),
    ("cover_ne", "mat_pillar", (6, 0.6, -8), (4, 1.2, 1.2), None),
    ("cover_sw", "mat_pillar", (-6, 0.6, 8), (4, 1.2, 1.2), None),
    ("cover_se", "mat_pillar", (6, 0.6, 8), (4, 1.2, 1.2), None),
]

# Four on the galleries (y clears the 1.5 m platform top), four on the floor.
# None sits inside a box: the pillars are at z = +-12 and the spawns behind
# them at +-14, and the x = +-10 pair is clear of the ramps at z = +-6.
ARENA02_SPAWNS = [
    (20, 1.6, -7), (20, 1.6, 7), (-20, 1.6, -7), (-20, 1.6, 7),
    (0, 0.1, -14), (0, 0.1, 14), (10, 0.1, 0), (-10, 0.1, 0),
]

LAYOUTS = {
    "arena01": (ARENA01_BOXES, ARENA01_SPAWNS),
    "arena02": (ARENA02_BOXES, ARENA02_SPAWNS),
}


def zrot_quat(degrees):
    a = math.radians(degrees) * 0.5
    return [0.0, 0.0, math.sin(a), math.cos(a)]  # glTF order: x, y, z, w


def box_geometry(scale):
    """Unit cube scaled to `scale`, with UVs in world units.

    Vertices stay in unit-cube space (the node still applies `scale`), but
    each face's UVs span its real-world size, so a 40 m floor tiles the
    texture ~20 times instead of stretching it once.
    """
    sx, sy, sz = scale
    h = 0.5
    # normal, 4 CCW corners (from outside), and the two world dimensions the
    # face's U and V axes correspond to.
    faces = [
        ((1, 0, 0), [(h, -h, h), (h, -h, -h), (h, h, -h), (h, h, h)], (sz, sy)),
        ((-1, 0, 0), [(-h, -h, -h), (-h, -h, h), (-h, h, h), (-h, h, -h)], (sz, sy)),
        ((0, 1, 0), [(-h, h, h), (h, h, h), (h, h, -h), (-h, h, -h)], (sx, sz)),
        ((0, -1, 0), [(-h, -h, -h), (h, -h, -h), (h, -h, h), (-h, -h, h)], (sx, sz)),
        ((0, 0, 1), [(-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h)], (sx, sy)),
        ((0, 0, -1), [(h, -h, -h), (-h, -h, -h), (-h, h, -h), (h, h, -h)], (sx, sy)),
    ]
    corner_uv = [(0, 0), (1, 0), (1, 1), (0, 1)]

    positions, normals, uvs, indices = [], [], [], []
    for normal, corners, (du, dv) in faces:
        base = len(positions)
        for i, c in enumerate(corners):
            positions.append(c)
            normals.append(normal)
            u, v = corner_uv[i]
            uvs.append((u * du * TEXTURE_SCALE, v * dv * TEXTURE_SCALE))
        for off in (0, 1, 2, 0, 2, 3):
            indices.append(base + off)
    return positions, normals, uvs, indices


class GlbBuilder:
    def __init__(self):
        self.binary = bytearray()
        self.views = []
        self.accessors = []

    def add_view(self, data, target=None):
        while len(self.binary) % 4 != 0:
            self.binary.append(0)
        offset = len(self.binary)
        self.binary.extend(data)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.views.append(view)
        return len(self.views) - 1

    def add_accessor(self, view, component_type, count, type_name, minmax=None):
        accessor = {
            "bufferView": view,
            "componentType": component_type,
            "count": count,
            "type": type_name,
        }
        if minmax:
            accessor["min"], accessor["max"] = minmax
        self.accessors.append(accessor)
        return len(self.accessors) - 1


def build_glb(texture_dir: Path, map_name: str):
    # Not `name`: the box loop below binds that per box, and shadowing this
    # silently named the scene after the last box.
    boxes, spawns = LAYOUTS[map_name]
    b = GlbBuilder()

    # --- textures (embedded PNG bytes) ---
    images, textures = [], []
    for i, (_, texture_file, _) in enumerate(MATERIALS):
        png = (texture_dir / texture_file).read_bytes()
        view = b.add_view(png)
        images.append({"name": texture_file, "mimeType": "image/png", "bufferView": view})
        textures.append({"source": i, "sampler": 0})

    materials = [
        {
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorFactor": color,
                "baseColorTexture": {"index": i},
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
            },
        }
        for i, (name, _, color) in enumerate(MATERIALS)
    ]

    # --- one mesh per box, with world-scaled UVs ---
    meshes, nodes = [], []
    for name, material, translation, scale, rotation in boxes:
        positions, normals, uvs, indices = box_geometry(scale)

        pos_view = b.add_view(b"".join(struct.pack("<3f", *p) for p in positions), 34962)
        nrm_view = b.add_view(b"".join(struct.pack("<3f", *n) for n in normals), 34962)
        uv_view = b.add_view(b"".join(struct.pack("<2f", *t) for t in uvs), 34962)
        idx_view = b.add_view(b"".join(struct.pack("<H", i) for i in indices), 34963)

        mins = [min(p[i] for p in positions) for i in range(3)]
        maxs = [max(p[i] for p in positions) for i in range(3)]
        pos_acc = b.add_accessor(pos_view, 5126, len(positions), "VEC3", (mins, maxs))
        nrm_acc = b.add_accessor(nrm_view, 5126, len(normals), "VEC3")
        uv_acc = b.add_accessor(uv_view, 5126, len(uvs), "VEC2")
        idx_acc = b.add_accessor(idx_view, 5123, len(indices), "SCALAR")

        meshes.append({
            "name": f"mesh_{name}",
            "primitives": [{
                "attributes": {"POSITION": pos_acc, "NORMAL": nrm_acc, "TEXCOORD_0": uv_acc},
                "indices": idx_acc,
                "material": MATERIAL_INDEX[material],
            }],
        })
        node = {
            "name": name,
            "mesh": len(meshes) - 1,
            "translation": list(translation),
            "scale": list(scale),
        }
        if rotation is not None:
            node["rotation"] = zrot_quat(rotation)
        nodes.append(node)

    for i, position in enumerate(spawns):
        nodes.append({"name": f"spawn_{i}", "translation": list(position)})

    gltf = {
        "asset": {"version": "2.0", "generator": "fps-engine gen_arena.py"},
        "scene": 0,
        "scenes": [{"name": map_name, "nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "images": images,
        "textures": textures,
        # Repeat wrapping is what makes the world-scaled UVs tile.
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "accessors": b.accessors,
        "bufferViews": b.views,
        "buffers": [{"byteLength": len(b.binary)}],
    }

    json_bytes = bytearray(json.dumps(gltf, separators=(",", ":")).encode())
    while len(json_bytes) % 4 != 0:
        json_bytes += b" "
    bin_bytes = bytearray(b.binary)
    while len(bin_bytes) % 4 != 0:
        bin_bytes += b"\x00"

    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    out = bytearray()
    out += struct.pack("<4sII", b"glTF", 2, total)
    out += struct.pack("<I4s", len(json_bytes), b"JSON") + json_bytes
    out += struct.pack("<I4s", len(bin_bytes), b"BIN\x00") + bin_bytes
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--map", choices=sorted(LAYOUTS), help="one layout (default: all)")
    parser.add_argument("--out", type=Path, help="output path; requires --map")
    args = parser.parse_args()
    if args.out and not args.map:
        parser.error("--out names a single file, so it needs --map")

    root = Path(__file__).resolve().parent.parent
    texture_dir = root / "assets" / "textures"
    if not (texture_dir / "concrete.png").exists():
        print("textures missing; run tools/gen_textures.py first", file=sys.stderr)
        return 1

    for name in ([args.map] if args.map else sorted(LAYOUTS)):
        out_path = args.out or root / "assets" / "maps" / f"{name}.glb"
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(build_glb(texture_dir, name))
        print(f"wrote {out_path} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
