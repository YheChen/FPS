#!/usr/bin/env python3
"""Generates assets/models/character.glb - a rigged, animated player figure.

Pure-python GLB writer, same approach as gen_arena.py: no dependencies, so
the asset can be regenerated anywhere and reviewed as a diff of this script
rather than as an opaque binary.

The figure is blocky on purpose. M16's engineering risk is GPU skinning and
joint-matrix maths, and a 12-joint rig makes those failures readable; a
60-joint mocap character would mean every wrong-looking frame has two
possible causes. Swapping in a real glTF character later only changes this
asset, not the engine code that consumes it.

Rig (12 joints, hips at the root):

    hips
     +- chest
     |   +- head
     |   +- arm_l_upper -> arm_l_lower
     |   +- arm_r_upper -> arm_r_lower
     +- leg_l_upper -> leg_l_lower
     +- leg_r_upper -> leg_r_lower

Most body parts are bound rigidly to a single joint (weight 1.0), which is
what gives the blocky look. The torso is the exception: its vertices blend
between `hips` and `chest` by height, so the skinning path is exercised with
real multi-joint weights rather than only the rigid case.

Conventions: meters, +Y up, -Z forward. Feet at y = 0, total height 1.8 m.

Usage: python3 tools/gen_character.py [output.glb]
"""

import json
import math
import struct
import sys
from pathlib import Path

# --- rig -------------------------------------------------------------------
# name -> (parent name or None, translation relative to the parent)
JOINTS = [
    ("hips", None, (0.0, 0.95, 0.0)),
    ("chest", "hips", (0.0, 0.28, 0.0)),
    ("head", "chest", (0.0, 0.30, 0.0)),
    ("arm_l_upper", "chest", (0.26, 0.16, 0.0)),
    ("arm_l_lower", "arm_l_upper", (0.0, -0.28, 0.0)),
    ("arm_r_upper", "chest", (-0.26, 0.16, 0.0)),
    ("arm_r_lower", "arm_r_upper", (0.0, -0.28, 0.0)),
    ("leg_l_upper", "hips", (0.11, -0.10, 0.0)),
    ("leg_l_lower", "leg_l_upper", (0.0, -0.42, 0.0)),
    ("leg_r_upper", "hips", (-0.11, -0.10, 0.0)),
    ("leg_r_lower", "leg_r_upper", (0.0, -0.42, 0.0)),
    ("hand_marker", "arm_r_lower", (0.0, -0.26, 0.0)),  # where a weapon goes
]
JOINT_INDEX = {name: i for i, (name, _, _) in enumerate(JOINTS)}

# Body parts: (joint, center offset from that joint, half extents,
# blends_between_joints). The torso is the only blended part, and it is
# subdivided vertically so the weight ramp has vertices to land on.
TORSO_SEGMENTS = 4
PARTS = [
    ("chest", (0.0, 0.02, 0.0), (0.20, 0.30, 0.11), True),   # torso
    ("head", (0.0, 0.11, 0.0), (0.115, 0.115, 0.115), False),
    ("arm_l_upper", (0.0, -0.14, 0.0), (0.058, 0.15, 0.058), False),
    ("arm_l_lower", (0.0, -0.14, 0.0), (0.05, 0.145, 0.05), False),
    ("arm_r_upper", (0.0, -0.14, 0.0), (0.058, 0.15, 0.058), False),
    ("arm_r_lower", (0.0, -0.14, 0.0), (0.05, 0.145, 0.05), False),
    ("leg_l_upper", (0.0, -0.21, 0.0), (0.075, 0.22, 0.075), False),
    ("leg_l_lower", (0.0, -0.21, 0.0), (0.066, 0.22, 0.085), False),
    ("leg_r_upper", (0.0, -0.21, 0.0), (0.075, 0.22, 0.075), False),
    ("leg_r_lower", (0.0, -0.21, 0.0), (0.066, 0.22, 0.085), False),
]


def joint_world_translation(name):
    """Accumulates translations up to the root; the rest pose has no rotation."""
    total = [0.0, 0.0, 0.0]
    while name is not None:
        index = JOINT_INDEX[name]
        _, parent, translation = JOINTS[index]
        total = [total[i] + translation[i] for i in range(3)]
        name = parent
    return total


def box_geometry(center, half, y_segments=1):
    """A box with per-face normals and UVs, in the joint's local space.

    `y_segments` splits the four side faces vertically. That matters for any
    part with height-varying skin weights: a plain box only has vertices at
    its top and bottom, so a weight ramp evaluated at those extremes always
    lands on 0 or 1 and no vertex ever ends up genuinely blended between two
    joints. Extra rings give the ramp somewhere to land.
    """
    cx, cy, cz = center
    hx, hy, hz = half
    positions, normals, uvs, indices = [], [], [], []

    def add_quad(corners, normal, v_lo, v_hi):
        base = len(positions)
        for point, uv in zip(corners, ((0.0, v_lo), (1.0, v_lo), (1.0, v_hi), (0.0, v_hi))):
            positions.append(point)
            normals.append(normal)
            uvs.append(uv)
        for offset in (0, 1, 2, 0, 2, 3):
            indices.append(base + offset)

    # Each side face is defined by its outward normal and the two bottom
    # corners, ordered so the resulting quad winds CCW seen from outside.
    sides = [
        ((1, 0, 0), (1, -1, 1), (1, -1, -1)),
        ((-1, 0, 0), (-1, -1, -1), (-1, -1, 1)),
        ((0, 0, 1), (-1, -1, 1), (1, -1, 1)),
        ((0, 0, -1), (1, -1, -1), (-1, -1, -1)),
    ]
    for normal, left, right in sides:
        for segment in range(y_segments):
            f_lo = segment / y_segments
            f_hi = (segment + 1) / y_segments
            y_lo = -1.0 + 2.0 * f_lo
            y_hi = -1.0 + 2.0 * f_hi

            def point(corner, y_sign):
                return (cx + corner[0] * hx, cy + y_sign * hy, cz + corner[2] * hz)

            add_quad([point(left, y_lo), point(right, y_lo),
                      point(right, y_hi), point(left, y_hi)], normal, f_lo, f_hi)

    caps = [
        ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),
        ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
    ]
    for normal, corners in caps:
        add_quad([(cx + s[0] * hx, cy + s[1] * hy, cz + s[2] * hz) for s in corners],
                 normal, 0.0, 1.0)

    return positions, normals, uvs, indices


def torso_weights(local_y):
    """Blends the torso between hips and chest by height.

    The chest joint sits at the torso's middle, so vertices below it lean on
    hips and those above lean on chest. This is the only part with weights
    that are not 1.0, and it is what makes the shader's 4-bone blend do real
    work instead of always picking a single matrix.
    """
    # local_y is relative to the chest joint: -0.28 at the hips, +0.32 at the
    # shoulders. Map that to 0..1 and smooth it.
    t = (local_y + 0.28) / 0.60
    t = max(0.0, min(1.0, t))
    t = t * t * (3.0 - 2.0 * t)
    return [(JOINT_INDEX["chest"], t), (JOINT_INDEX["hips"], 1.0 - t)]


def build_mesh():
    """One mesh, one primitive: the whole figure skins in a single draw."""
    positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []

    for joint_name, center, half, blend in PARTS:
        joint_translation = joint_world_translation(joint_name)
        segments = TORSO_SEGMENTS if blend else 1
        part_positions, part_normals, part_uvs, part_indices = box_geometry(center, half, segments)
        base = len(positions)

        for i, local in enumerate(part_positions):
            # Vertices are authored in MODEL space (the rest pose), which is
            # what inverse bind matrices expect. The joint's local offset is
            # folded in here.
            positions.append(tuple(local[k] + joint_translation[k] for k in range(3)))
            normals.append(part_normals[i])
            uvs.append(part_uvs[i])

            if blend:
                influences = torso_weights(local[1])
            else:
                influences = [(JOINT_INDEX[joint_name], 1.0)]
            # glTF requires exactly 4 joints/weights per vertex; pad with
            # joint 0 at weight 0, which contributes nothing.
            padded = influences + [(0, 0.0)] * (4 - len(influences))
            joints.append(tuple(j for j, _ in padded))
            weights.append(tuple(w for _, w in padded))

        indices.extend(base + index for index in part_indices)

    return positions, normals, uvs, joints, weights, indices


# --- animation -------------------------------------------------------------


def quat_x(degrees):
    a = math.radians(degrees) * 0.5
    return (math.sin(a), 0.0, 0.0, math.cos(a))


def quat_z(degrees):
    a = math.radians(degrees) * 0.5
    return (0.0, 0.0, math.sin(a), math.cos(a))


def run_clip():
    """A 0.8 s run cycle: legs and arms counter-swinging about X.

    First and last keyframes match so the clip loops without a visible pop.
    """
    times = [0.0, 0.2, 0.4, 0.6, 0.8]
    swing = 38.0
    arm = 30.0
    channels = {
        "leg_l_upper": [quat_x(s) for s in (swing, 0.0, -swing, 0.0, swing)],
        "leg_r_upper": [quat_x(s) for s in (-swing, 0.0, swing, 0.0, -swing)],
        # Knees only bend one way, so the lower leg tucks on the back swing.
        "leg_l_lower": [quat_x(s) for s in (-10.0, -46.0, -6.0, -30.0, -10.0)],
        "leg_r_lower": [quat_x(s) for s in (-6.0, -30.0, -10.0, -46.0, -6.0)],
        "arm_l_upper": [quat_x(s) for s in (-arm, 0.0, arm, 0.0, -arm)],
        "arm_r_upper": [quat_x(s) for s in (arm, 0.0, -arm, 0.0, arm)],
        "arm_l_lower": [quat_x(-32.0)] * 5,
        "arm_r_lower": [quat_x(-32.0)] * 5,
        # A little counter-rotation in the chest sells the stride.
        "chest": [quat_z(s) for s in (2.5, 0.0, -2.5, 0.0, 2.5)],
    }
    return "run", times, channels


def idle_clip():
    """A 2.4 s breathing sway. Deliberately subtle."""
    times = [0.0, 0.8, 1.6, 2.4]
    channels = {
        "chest": [quat_x(s) for s in (0.0, -1.8, 0.0, 0.0)],
        "head": [quat_x(s) for s in (0.0, 1.2, 0.0, 0.0)],
        "arm_l_upper": [quat_x(s) for s in (3.0, 5.0, 3.0, 3.0)],
        "arm_r_upper": [quat_x(s) for s in (3.0, 5.0, 3.0, 3.0)],
        "arm_l_lower": [quat_x(-14.0)] * 4,
        "arm_r_lower": [quat_x(-14.0)] * 4,
    }
    return "idle", times, channels


def jump_clip():
    """A 0.6 s tuck-and-extend. Not looped; the state machine holds the end."""
    times = [0.0, 0.25, 0.6]
    channels = {
        "leg_l_upper": [quat_x(s) for s in (0.0, 42.0, 12.0)],
        "leg_r_upper": [quat_x(s) for s in (0.0, 42.0, 12.0)],
        "leg_l_lower": [quat_x(s) for s in (0.0, -62.0, -18.0)],
        "leg_r_lower": [quat_x(s) for s in (0.0, -62.0, -18.0)],
        "arm_l_upper": [quat_x(s) for s in (0.0, -70.0, -40.0)],
        "arm_r_upper": [quat_x(s) for s in (0.0, -70.0, -40.0)],
    }
    return "jump", times, channels


CLIPS = [idle_clip, run_clip, jump_clip]


# --- GLB writing -----------------------------------------------------------


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

    def add_accessor(self, view, component_type, count, type_name, minmax=None,
                     normalized=False):
        accessor = {
            "bufferView": view,
            "componentType": component_type,
            "count": count,
            "type": type_name,
        }
        if normalized:
            accessor["normalized"] = True
        if minmax:
            accessor["min"], accessor["max"] = minmax
        self.accessors.append(accessor)
        return len(self.accessors) - 1


def build_glb():
    b = GlbBuilder()
    positions, normals, uvs, joints, weights, indices = build_mesh()

    pos_view = b.add_view(b"".join(struct.pack("<3f", *p) for p in positions), 34962)
    nrm_view = b.add_view(b"".join(struct.pack("<3f", *n) for n in normals), 34962)
    uv_view = b.add_view(b"".join(struct.pack("<2f", *t) for t in uvs), 34962)
    joint_view = b.add_view(b"".join(struct.pack("<4H", *j) for j in joints), 34962)
    weight_view = b.add_view(b"".join(struct.pack("<4f", *w) for w in weights), 34962)
    index_view = b.add_view(b"".join(struct.pack("<H", i) for i in indices), 34963)

    mins = [min(p[i] for p in positions) for i in range(3)]
    maxs = [max(p[i] for p in positions) for i in range(3)]
    pos_acc = b.add_accessor(pos_view, 5126, len(positions), "VEC3", (mins, maxs))
    nrm_acc = b.add_accessor(nrm_view, 5126, len(normals), "VEC3")
    uv_acc = b.add_accessor(uv_view, 5126, len(uvs), "VEC2")
    joint_acc = b.add_accessor(joint_view, 5123, len(joints), "VEC4")  # u16
    weight_acc = b.add_accessor(weight_view, 5126, len(weights), "VEC4")
    index_acc = b.add_accessor(index_view, 5123, len(indices), "SCALAR")

    # Inverse bind matrices: each joint's rest-pose world transform inverted.
    # The rest pose is translation-only, so the inverse is just a negated
    # translation -- but write full matrices, because that is what the format
    # says and what a real exporter would produce.
    ibm_bytes = bytearray()
    for name, _, _ in JOINTS:
        tx, ty, tz = joint_world_translation(name)
        matrix = [1.0, 0.0, 0.0, 0.0,
                  0.0, 1.0, 0.0, 0.0,
                  0.0, 0.0, 1.0, 0.0,
                  -tx, -ty, -tz, 1.0]  # column-major, translation last
        ibm_bytes += struct.pack("<16f", *matrix)
    ibm_view = b.add_view(bytes(ibm_bytes))
    ibm_acc = b.add_accessor(ibm_view, 5126, len(JOINTS), "MAT4")

    # --- nodes: joints first, so joint indices double as node indices ------
    nodes = []
    for name, parent, translation in JOINTS:
        node = {"name": name, "translation": list(translation)}
        children = [JOINT_INDEX[child_name]
                    for child_name, child_parent, _ in JOINTS
                    if child_parent == name]
        if children:
            node["children"] = children
        nodes.append(node)

    mesh_node_index = len(nodes)
    nodes.append({"name": "character", "mesh": 0, "skin": 0})

    # --- animations --------------------------------------------------------
    animations = []
    for clip in CLIPS:
        name, times, channels = clip()
        time_view = b.add_view(b"".join(struct.pack("<f", t) for t in times))
        time_acc = b.add_accessor(time_view, 5126, len(times), "SCALAR",
                                  ([times[0]], [times[-1]]))
        samplers, animation_channels = [], []
        for joint_name, rotations in channels.items():
            assert len(rotations) == len(times), f"{name}/{joint_name} keyframe count"
            rot_view = b.add_view(b"".join(struct.pack("<4f", *q) for q in rotations))
            rot_acc = b.add_accessor(rot_view, 5126, len(rotations), "VEC4")
            samplers.append({"input": time_acc, "output": rot_acc,
                             "interpolation": "LINEAR"})
            animation_channels.append({
                "sampler": len(samplers) - 1,
                "target": {"node": JOINT_INDEX[joint_name], "path": "rotation"},
            })
        animations.append({"name": name, "samplers": samplers,
                           "channels": animation_channels})

    gltf = {
        "asset": {"version": "2.0", "generator": "fps-engine gen_character.py"},
        "scene": 0,
        "scenes": [{"name": "character", "nodes": [JOINT_INDEX["hips"], mesh_node_index]}],
        "nodes": nodes,
        "meshes": [{
            "name": "character_mesh",
            "primitives": [{
                "attributes": {
                    "POSITION": pos_acc,
                    "NORMAL": nrm_acc,
                    "TEXCOORD_0": uv_acc,
                    "JOINTS_0": joint_acc,
                    "WEIGHTS_0": weight_acc,
                },
                "indices": index_acc,
                "material": 0,
            }],
        }],
        "materials": [{
            "name": "mat_character",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.82, 0.84, 0.90, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.75,
            },
        }],
        "skins": [{
            "name": "character_skin",
            "skeleton": JOINT_INDEX["hips"],
            "joints": list(range(len(JOINTS))),
            "inverseBindMatrices": ibm_acc,
        }],
        "animations": animations,
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
    out_path = (Path(sys.argv[1]) if len(sys.argv) > 1
                else root / "assets" / "models" / "character.glb")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(build_glb())
    print(f"wrote {out_path} ({out_path.stat().st_size} bytes), "
          f"{len(JOINTS)} joints, {len(CLIPS)} clips")
    return 0


if __name__ == "__main__":
    sys.exit(main())
