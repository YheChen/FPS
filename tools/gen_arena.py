#!/usr/bin/env python3
"""Generates assets/maps/arena01.glb - the textured deathmatch arena.

Pure-python GLB (glTF 2.0 binary) writer: no external dependencies, so the
map can be regenerated anywhere. The arena is built from boxes, each emitted
as its own mesh so UVs can be scaled to WORLD size - that keeps texel density
constant, instead of stretching one 0..1 UV square across a 40 m floor.

Textures (from tools/gen_textures.py) are embedded in the .glb as PNG buffer
views, so the map is a single self-contained file.

Conventions: meters, +Y up, -Z forward (glTF standard). Floor top at y = 0.

Usage: python3 tools/gen_arena.py [output.glb]
"""

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

# (name, material, translation, scale, rotation_z_degrees|None)
BOXES = [
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

SPAWNS = [
    (15, 0.1, 15), (-15, 0.1, 15), (15, 0.1, -15), (-15, 0.1, -15),
    (0, 0.1, 16), (0, 0.1, -16), (16, 0.1, 0), (-16, 0.1, 0),
]


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


def build_glb(texture_dir: Path):
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
    for name, material, translation, scale, rotation in BOXES:
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

    for i, position in enumerate(SPAWNS):
        nodes.append({"name": f"spawn_{i}", "translation": list(position)})

    gltf = {
        "asset": {"version": "2.0", "generator": "fps-engine gen_arena.py"},
        "scene": 0,
        "scenes": [{"name": "arena01", "nodes": list(range(len(nodes)))}],
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
    root = Path(__file__).resolve().parent.parent
    texture_dir = root / "assets" / "textures"
    if not (texture_dir / "concrete.png").exists():
        print("textures missing; run tools/gen_textures.py first", file=sys.stderr)
        return 1
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "assets" / "maps" / "arena01.glb"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(build_glb(texture_dir))
    print(f"wrote {out_path} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
