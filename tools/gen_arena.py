#!/usr/bin/env python3
"""Generates assets/maps/arena01.glb - the greybox deathmatch arena.

Pure-python GLB (glTF 2.0 binary) writer: no external dependencies, so the
map can be regenerated anywhere. The arena is built from scaled/rotated unit
cubes. Empty nodes named "spawn_N" mark player spawn points; the game reads
them by name.

Conventions: meters, +Y up, -Z forward (glTF standard). Floor top at y = 0.

Usage: python3 tools/gen_arena.py [output.glb]
"""

import json
import math
import struct
import sys
from pathlib import Path

# --- unit cube geometry (matches eng::MeshData::unit_cube) -----------------

def cube_geometry():
    h = 0.5
    faces = [
        # normal, four CCW corners (from outside)
        ((1, 0, 0), [(h, -h, h), (h, -h, -h), (h, h, -h), (h, h, h)]),
        ((-1, 0, 0), [(-h, -h, -h), (-h, -h, h), (-h, h, h), (-h, h, -h)]),
        ((0, 1, 0), [(-h, h, h), (h, h, h), (h, h, -h), (-h, h, -h)]),
        ((0, -1, 0), [(-h, -h, -h), (h, -h, -h), (h, -h, h), (-h, -h, h)]),
        ((0, 0, 1), [(-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h)]),
        ((0, 0, -1), [(h, -h, -h), (-h, -h, -h), (-h, h, -h), (h, h, -h)]),
    ]
    positions, normals, indices = [], [], []
    for normal, corners in faces:
        base = len(positions)
        for c in corners:
            positions.append(c)
            normals.append(normal)
        for off in (0, 1, 2, 0, 2, 3):
            indices.append(base + off)
    return positions, normals, indices


def zrot_quat(degrees):
    a = math.radians(degrees) * 0.5
    return [0.0, 0.0, math.sin(a), math.cos(a)]  # glTF order: x, y, z, w


# --- arena description ------------------------------------------------------

MATERIALS = [
    ("mat_floor", [0.55, 0.55, 0.60, 1.0]),
    ("mat_wall", [0.65, 0.55, 0.45, 1.0]),
    ("mat_pillar", [0.35, 0.45, 0.70, 1.0]),
    ("mat_platform", [0.40, 0.60, 0.45, 1.0]),
]
MESH_FOR_MATERIAL = {name: i for i, (name, _) in enumerate(MATERIALS)}

# (name, material, translation, scale, rotation_quat|None)
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
    ("ramp_east", "mat_platform", (4.8, 0.65, 0), (4.2, 0.3, 3), zrot_quat(-16)),
    ("ramp_west", "mat_platform", (-4.8, 0.65, 0), (4.2, 0.3, 3), zrot_quat(16)),
    ("cover_n", "mat_pillar", (0, 0.6, -13), (5, 1.2, 1.2), None),
    ("cover_s", "mat_pillar", (0, 0.6, 13), (5, 1.2, 1.2), None),
]

SPAWNS = [
    (15, 0.1, 15), (-15, 0.1, 15), (15, 0.1, -15), (-15, 0.1, -15),
    (0, 0.1, 16), (0, 0.1, -16), (16, 0.1, 0), (-16, 0.1, 0),
]


def align(data, alignment, pad):
    while len(data) % alignment != 0:
        data += pad
    return data


def build_glb():
    positions, normals, indices = cube_geometry()

    pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
    norm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
    idx_bytes = b"".join(struct.pack("<H", i) for i in indices)

    binary = bytearray()
    views = []

    def add_view(data, target):
        offset = len(binary)
        binary.extend(data)
        align_to = 4
        while len(binary) % align_to != 0:
            binary.append(0)
        views.append({
            "buffer": 0,
            "byteOffset": offset,
            "byteLength": len(data),
            "target": target,
        })
        return len(views) - 1

    pos_view = add_view(pos_bytes, 34962)   # ARRAY_BUFFER
    norm_view = add_view(norm_bytes, 34962)
    idx_view = add_view(idx_bytes, 34963)   # ELEMENT_ARRAY_BUFFER

    mins = [min(p[i] for p in positions) for i in range(3)]
    maxs = [max(p[i] for p in positions) for i in range(3)]
    accessors = [
        {"bufferView": pos_view, "componentType": 5126, "count": len(positions),
         "type": "VEC3", "min": mins, "max": maxs},
        {"bufferView": norm_view, "componentType": 5126, "count": len(normals),
         "type": "VEC3"},
        {"bufferView": idx_view, "componentType": 5123, "count": len(indices),
         "type": "SCALAR"},
    ]

    materials = [
        {"name": name,
         "pbrMetallicRoughness": {"baseColorFactor": color,
                                  "metallicFactor": 0.0,
                                  "roughnessFactor": 1.0}}
        for name, color in MATERIALS
    ]

    # One mesh per material, all sharing the same cube accessors.
    meshes = [
        {"name": f"cube_{name}",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                         "indices": 2, "material": i}]}
        for i, (name, _) in enumerate(MATERIALS)
    ]

    nodes = []
    for name, material, translation, scale, rotation in BOXES:
        node = {
            "name": name,
            "mesh": MESH_FOR_MATERIAL[material],
            "translation": list(translation),
            "scale": list(scale),
        }
        if rotation is not None:
            node["rotation"] = rotation
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
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(binary)}],
    }

    json_bytes = align(bytearray(json.dumps(gltf, separators=(",", ":")).encode()), 4, b" ")
    bin_bytes = align(bytearray(binary), 4, b"\x00")

    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    out = bytearray()
    out += struct.pack("<4sII", b"glTF", 2, total)
    out += struct.pack("<I4s", len(json_bytes), b"JSON") + json_bytes
    out += struct.pack("<I4s", len(bin_bytes), b"BIN\x00") + bin_bytes
    return bytes(out)


def main():
    default = Path(__file__).resolve().parent.parent / "assets" / "maps" / "arena01.glb"
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else default
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(build_glb())
    print(f"wrote {out_path} ({out_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
