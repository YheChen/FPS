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
     |   +- arm_r_upper -> arm_r_lower -> hand_r
     |   +- arm_l_upper -> arm_l_lower
     +- leg_r_upper -> leg_r_lower
     +- leg_l_upper -> leg_l_lower

Most body parts are bound rigidly to a single joint (weight 1.0), which is
what gives the blocky look. The torso is the exception: its vertices blend
between `hips` and `chest` by height, so the skinning path is exercised with
real multi-joint weights rather than only the rigid case.

Conventions: meters, +Y up, -Z forward. With -Z forward and +Y up the
character's right is +X, so the _r joints are the ones at positive X. (They
used to be the _l ones: the suffixes were mirrored, which nobody could see on
a symmetric figure but which makes "put the weapon in the right hand"
ambiguous. Only the labels moved -- the joint ORDER is untouched, because
vertex JOINTS_0 indices refer to it and Skeleton::from_gltf rejects a skin
whose parents do not precede their children rather than reordering it.)

Feet at y = 0, total height 1.8 m.

Clips: idle, run, strafe_right, strafe_left, crouch_idle, crouch_move, jump,
land, death. Everything the client can blend is here; the client's state
machine picks between them and never invents a pose of its own.

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
    ("arm_r_upper", "chest", (0.26, 0.16, 0.0)),
    ("arm_r_lower", "arm_r_upper", (0.0, -0.28, 0.0)),
    ("arm_l_upper", "chest", (-0.26, 0.16, 0.0)),
    ("arm_l_lower", "arm_l_upper", (0.0, -0.28, 0.0)),
    ("leg_r_upper", "hips", (0.11, -0.10, 0.0)),
    ("leg_r_lower", "leg_r_upper", (0.0, -0.42, 0.0)),
    ("leg_l_upper", "hips", (-0.11, -0.10, 0.0)),
    ("leg_l_lower", "leg_l_upper", (0.0, -0.42, 0.0)),
    # WEAPON ATTACHMENT, for whoever puts a gun in this hand next. It is a
    # real joint (it has an inverse bind matrix and rides the animation) but
    # no vertex is weighted to it, so it costs one joint matrix and nothing
    # else. GRIP CONVENTION: this node sits at the centre of the closed fist,
    # and a weapon's own origin goes exactly here with its axes equal to this
    # joint's -- barrel down local -Z, magazine down local -Y, which is the
    # same forward/up the rest of the model uses. There is deliberately no
    # extra grip offset: a weapon that needs one should bake it into its own
    # mesh so this transform stays the single stable thing to hang off.
    ("hand_r", "arm_r_lower", (0.0, -0.26, 0.0)),
]
JOINT_INDEX = {name: i for i, (name, _, _) in enumerate(JOINTS)}

# Body parts: (joint, center offset from that joint, half extents,
# blends_between_joints). The torso is the only blended part, and it is
# subdivided vertically so the weight ramp has vertices to land on.
TORSO_SEGMENTS = 4
PARTS = [
    ("chest", (0.0, 0.02, 0.0), (0.20, 0.30, 0.11), True),   # torso
    ("head", (0.0, 0.11, 0.0), (0.115, 0.115, 0.115), False),
    ("arm_r_upper", (0.0, -0.14, 0.0), (0.058, 0.15, 0.058), False),
    ("arm_r_lower", (0.0, -0.14, 0.0), (0.05, 0.145, 0.05), False),
    ("arm_l_upper", (0.0, -0.14, 0.0), (0.058, 0.15, 0.058), False),
    ("arm_l_lower", (0.0, -0.14, 0.0), (0.05, 0.145, 0.05), False),
    ("leg_r_upper", (0.0, -0.21, 0.0), (0.075, 0.22, 0.075), False),
    ("leg_r_lower", (0.0, -0.21, 0.0), (0.066, 0.22, 0.085), False),
    ("leg_l_upper", (0.0, -0.21, 0.0), (0.075, 0.22, 0.075), False),
    ("leg_l_lower", (0.0, -0.21, 0.0), (0.066, 0.22, 0.085), False),
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
#
# Every clip returns (name, times, rotations, translations). Both channel
# dicts are keyed by joint name and must have one value per time. A joint a
# clip never names keeps its REST transform, which is why any clip that
# lowers the body has to animate the hips translation explicitly -- leaving
# it out does not "keep the previous height", it snaps back to standing.
#
# Positive X rotation swings a downward-pointing bone toward -Z (forward) and
# tips an upward-pointing one toward +Z (backward). Positive Z rotation
# swings a downward bone toward +X (the character's right). Knees and elbows
# use negative X: they only bend one way.


def quat_x(degrees):
    a = math.radians(degrees) * 0.5
    return (math.sin(a), 0.0, 0.0, math.cos(a))


def quat_z(degrees):
    a = math.radians(degrees) * 0.5
    return (0.0, 0.0, math.sin(a), math.cos(a))


def quat_mul(a, b):
    """Hamilton product in glTF's xyzw order; `a` is applied after `b`."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def quat_zx(z_degrees, x_degrees):
    """Abduct about Z first, then swing about X.

    Order matters for the sprawled limbs in the death clip: swinging first
    and abducting second twists the limb about its own length instead of
    laying it out flat.
    """
    return quat_mul(quat_x(x_degrees), quat_z(z_degrees))


def quat_matrix(q):
    """Row-major 3x3 rotation matrix for an xyzw quaternion."""
    x, y, z, w = q
    return (
        (1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w)),
        (2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)),
        (2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y)),
    )


def joint_world_transforms(rotations, translations):
    """(rotation, offset) per joint at one instant, parents folded in.

    The same forward pass pose_to_joint_matrices does on the engine side.
    Joints the caller does not name keep their rest transform, which is also
    what sample_clip does -- so what this computes is what the client will
    actually draw.
    """
    out = {}
    for name, parent, rest_translation in JOINTS:
        rotation = quat_matrix(rotations.get(name, (0.0, 0.0, 0.0, 1.0)))
        translation = translations.get(name, rest_translation)
        if parent is None:
            out[name] = (rotation, tuple(translation))
            continue
        parent_rotation, parent_offset = out[parent]
        offset = tuple(
            parent_offset[i] + sum(parent_rotation[i][k] * translation[k] for k in range(3))
            for i in range(3)
        )
        combined = tuple(
            tuple(sum(parent_rotation[i][k] * rotation[k][j] for k in range(3)) for j in range(3))
            for i in range(3)
        )
        out[name] = (combined, offset)
    return out


def lowest_corner_y(rotations, translations):
    """Height of the lowest mesh corner at one instant.

    The torso is treated as rigid on `chest` even though it skins between
    chest and hips; the two joints are 28 cm apart on the same axis and the
    torso is never what touches the floor, so the approximation costs
    nothing and keeps this from having to re-run the skinning.
    """
    world = joint_world_transforms(rotations, translations)
    lowest = None
    for joint_name, center, half, _ in PARTS:
        rotation, offset = world[joint_name]
        for sx in (-1.0, 1.0):
            for sy in (-1.0, 1.0):
                for sz in (-1.0, 1.0):
                    corner = (
                        center[0] + sx * half[0],
                        center[1] + sy * half[1],
                        center[2] + sz * half[2],
                    )
                    y = offset[1] + sum(rotation[1][k] * corner[k] for k in range(3))
                    lowest = y if lowest is None else min(lowest, y)
    return lowest


def quat_slerp(a, b, t):
    """Short-arc slerp, matching what the engine's sample_clip does.

    The floor solve is only meaningful if it interpolates poses the same way
    the runtime will; a nearest-neighbour or component-wise mix here would
    solve a curve nobody ever plays.
    """
    dot = sum(a[i] * b[i] for i in range(4))
    if dot < 0.0:
        b = tuple(-c for c in b)
        dot = -dot
    if dot > 0.9995:  # too close to slerp stably; the linear mix is exact enough
        blended = [a[i] + (b[i] - a[i]) * t for i in range(4)]
    else:
        theta = math.acos(min(1.0, dot))
        sin_theta = math.sin(theta)
        blended = [
            (a[i] * math.sin((1.0 - t) * theta) + b[i] * math.sin(t * theta)) / sin_theta
            for i in range(4)
        ]
    scale = 1.0 / math.sqrt(sum(c * c for c in blended))
    return tuple(c * scale for c in blended)


# How far a sole may sink between two solved keys before that interval earns a
# key of its own, and the ceiling on what one clip may spend doing it. 4 mm is
# well under the 1 cm the engine-side test allows, which leaves room for the
# difference between this rigid-box estimate and the real skinned result.
FLOOR_TOLERANCE = 0.004
MAX_FLOOR_KEYS = 48


def plant_on_floor(times, rotations, translations):
    """Solves the hips height so the lowest corner rests on y=0 THROUGHOUT.

    Authoring hip heights by hand is guesswork, because the sole is not the
    end of the shin bone -- it is the far corner of a box that swings lower
    as the shin tilts, which is how the first draft of the crouch ended up
    7 cm underground. Solving it here lets a pose be authored purely as joint
    angles and still stand on the floor.

    Solving only AT the authored keys is not enough. Between two keys the
    runtime slerps the joints but interpolates the hips height linearly, and
    a fold sharp enough makes the true floor curve bow below that line: the
    landing crouch sank 1.8 cm and the death collapse 8 cm at the midpoints
    of intervals whose endpoints were both exactly on the floor. So the
    interval midpoints are probed the way the runtime would interpolate them,
    and the worst offender is bisected until nothing dips further than
    FLOOR_TOLERANCE. Clips that barely move keep their original keys; only
    the fast ones pay for extra ones.

    Returns (times, rotations, translations) because the key list grows.
    The hips translation a clip provides is a DELTA from the solution: 0 means
    resting on the floor, positive lifts the figure off it.
    """
    keys = [
        {
            "time": time,
            "rotations": {joint: values[key] for joint, values in rotations.items()},
            "translations": {joint: values[key]
                             for joint, values in translations.items() if joint != "hips"},
            "hips": translations.get("hips", [(0.0, 0.0, 0.0)] * len(times))[key],
        }
        for key, time in enumerate(times)
    ]

    def solved_hips(key):
        """The hips translation that rests this key on the floor, plus its delta."""
        probe = dict(key["translations"])
        # Measured with the hips at y=0 and the delta added afterwards, so a
        # clip asking to float 2 cm up gets 2 cm rather than having its
        # request cancelled by the solve.
        probe["hips"] = (key["hips"][0], 0.0, key["hips"][2])
        return (key["hips"][0],
                key["hips"][1] - lowest_corner_y(key["rotations"], probe),
                key["hips"][2])

    def between(lo, hi):
        """The pose the runtime would show halfway between two solved keys."""
        probe_rotations = {joint: quat_slerp(value, hi["rotations"][joint], 0.5)
                           for joint, value in lo["rotations"].items()}
        probe_translations = {
            joint: tuple(0.5 * (value[axis] + hi["translations"][joint][axis]) for axis in range(3))
            for joint, value in lo["translations"].items()
        }
        low_hips, high_hips = solved_hips(lo), solved_hips(hi)
        probe_translations["hips"] = tuple(
            0.5 * (low_hips[axis] + high_hips[axis]) for axis in range(3))
        return probe_rotations, probe_translations

    while len(keys) < MAX_FLOOR_KEYS:
        # Always split the deepest dip first: one bisection there usually
        # fixes several shallower ones on the same fast stretch.
        worst = None
        for index in range(len(keys) - 1):
            probe_rotations, probe_translations = between(keys[index], keys[index + 1])
            dip = lowest_corner_y(probe_rotations, probe_translations)
            if dip < -FLOOR_TOLERANCE and (worst is None or dip < worst[0]):
                worst = (dip, index, probe_rotations, probe_translations)
        if worst is None:
            break
        _, index, probe_rotations, probe_translations = worst
        lo, hi = keys[index], keys[index + 1]
        probe_translations.pop("hips")  # re-solved for the new key, not inherited
        keys.insert(index + 1, {
            "time": 0.5 * (lo["time"] + hi["time"]),
            "rotations": probe_rotations,
            "translations": probe_translations,
            "hips": tuple(0.5 * (lo["hips"][axis] + hi["hips"][axis]) for axis in range(3)),
        })

    out_times = [key["time"] for key in keys]
    out_rotations = {joint: [key["rotations"][joint] for key in keys] for joint in rotations}
    out_translations = {joint: [key["translations"][joint] for key in keys]
                        for joint in translations if joint != "hips"}
    out_translations["hips"] = [solved_hips(key) for key in keys]
    return out_times, out_rotations, out_translations


def run_clip():
    """A 0.8 s run cycle: legs and arms counter-swinging about X.

    First and last keyframes match so the clip loops without a visible pop.
    """
    times = [0.0, 0.2, 0.4, 0.6, 0.8]
    swing = 38.0
    arm = 30.0
    rotations = {
        "leg_r_upper": [quat_x(s) for s in (swing, 0.0, -swing, 0.0, swing)],
        "leg_l_upper": [quat_x(s) for s in (-swing, 0.0, swing, 0.0, -swing)],
        # Knees only bend one way, so the lower leg tucks on the back swing.
        "leg_r_lower": [quat_x(s) for s in (-10.0, -46.0, -6.0, -30.0, -10.0)],
        "leg_l_lower": [quat_x(s) for s in (-6.0, -30.0, -10.0, -46.0, -6.0)],
        "arm_r_upper": [quat_x(s) for s in (-arm, 0.0, arm, 0.0, -arm)],
        "arm_l_upper": [quat_x(s) for s in (arm, 0.0, -arm, 0.0, arm)],
        "arm_r_lower": [quat_x(-32.0)] * 5,
        "arm_l_lower": [quat_x(-32.0)] * 5,
        # A little counter-rotation in the chest sells the stride.
        "chest": [quat_z(s) for s in (2.5, 0.0, -2.5, 0.0, 2.5)],
    }
    return "run", times, rotations, {}


def idle_clip():
    """A 2.4 s breathing sway. Deliberately subtle."""
    times = [0.0, 0.8, 1.6, 2.4]
    rotations = {
        "chest": [quat_x(s) for s in (0.0, -1.8, 0.0, 0.0)],
        "head": [quat_x(s) for s in (0.0, 1.2, 0.0, 0.0)],
        "arm_r_upper": [quat_x(s) for s in (3.0, 5.0, 3.0, 3.0)],
        "arm_l_upper": [quat_x(s) for s in (3.0, 5.0, 3.0, 3.0)],
        "arm_r_lower": [quat_x(-14.0)] * 4,
        "arm_l_lower": [quat_x(-14.0)] * 4,
    }
    return "idle", times, rotations, {}


def strafe_clip(name, sign):
    """A 0.7 s side-step. `sign` = +1 steps toward +X (the character's right).

    This clip is the reason the whole set exists. A sidestepping player used
    to play the forward run cycle, so their legs told everyone watching that
    they were closing when they were actually cutting across -- an animation
    that is wrong about direction is worse than no animation, because people
    aim at where the legs say the body is going.

    So the step is purely lateral: both legs abduct toward the direction of
    travel (the near one leading, the far one dragging across underneath),
    the chest leans into it, and the hips shift their weight from side to
    side. Nothing swings about X, because nothing is going forward.
    """
    times = [0.0, 0.175, 0.35, 0.525, 0.7]
    # The leg on the side being stepped toward leads; the other trails and
    # crosses under the body to catch up.
    lead, trail = ("leg_r", "leg_l") if sign > 0 else ("leg_l", "leg_r")
    lead_arm, trail_arm = ("arm_r", "arm_l") if sign > 0 else ("arm_l", "arm_r")
    rotations = {
        lead + "_upper": [quat_z(sign * s) for s in (10.0, 26.0, 14.0, 4.0, 10.0)],
        trail + "_upper": [quat_z(sign * s) for s in (6.0, 8.0, 20.0, 10.0, 6.0)],
        # The knees stay bent about X regardless of which way the body is
        # going; only the hips and shoulders know about the direction.
        lead + "_lower": [quat_x(s) for s in (-8.0, -16.0, -30.0, -18.0, -8.0)],
        trail + "_lower": [quat_x(s) for s in (-26.0, -14.0, -8.0, -20.0, -26.0)],
        lead_arm + "_upper": [quat_z(sign * s) for s in (12.0, 18.0, 10.0, 8.0, 12.0)],
        trail_arm + "_upper": [quat_z(sign * s) for s in (6.0, 10.0, 4.0, 4.0, 6.0)],
        "arm_r_lower": [quat_x(-22.0)] * 5,
        "arm_l_lower": [quat_x(-22.0)] * 5,
        # Negative Z tips an upward bone toward +X, so the lean goes INTO the
        # step; the head counter-rotates to stay roughly level.
        "chest": [quat_z(-sign * 6.0)] * 5,
        "head": [quat_z(sign * 3.0)] * 5,
    }
    # Weight transfers side to side; the height is solved, because an abducted
    # leg swings its foot box outward and downward at the same time.
    translations = {
        "hips": [
            (0.0, 0.0, 0.0),
            (sign * 0.035, 0.0, 0.0),
            (0.0, 0.0, 0.0),
            (-sign * 0.02, 0.0, 0.0),
            (0.0, 0.0, 0.0),
        ]
    }
    return (name, *plant_on_floor(times, rotations, translations))


# A crouch is a real squat: the game drops the eye from 1.62 m to 1.00 m and
# the capsule from 1.8 m to 1.15 m, so the pose has to lose most of that in
# the knees and the rest in a forward fold of the chest. The shin angle
# mirrors the thigh, which folds the leg while leaving the foot under the
# knee instead of out in front of it.
CROUCH_THIGH = 70.0
CROUCH_SHIN = -68.0
CROUCH_KNEE = CROUCH_SHIN - CROUCH_THIGH
# The lifted leg of the crouch shuffle, folded further than the base stance.
CROUCH_LIFT_THIGH = 78.0
CROUCH_LIFT_SHIN = -76.0
CROUCH_LIFT_KNEE = CROUCH_LIFT_SHIN - CROUCH_LIFT_THIGH


def crouch_pose_rotations(thigh_r, knee_r, thigh_l, knee_l):
    """The shared crouch upper body plus whatever the legs are doing."""
    return {
        "leg_r_upper": [quat_x(s) for s in thigh_r],
        "leg_l_upper": [quat_x(s) for s in thigh_l],
        "leg_r_lower": [quat_x(s) for s in knee_r],
        "leg_l_lower": [quat_x(s) for s in knee_l],
    }


def crouch_idle_clip():
    """A 2.4 s squat, breathing like the standing idle does."""
    times = [0.0, 1.2, 2.4]
    rotations = crouch_pose_rotations(
        [CROUCH_THIGH] * 3, [CROUCH_KNEE] * 3, [CROUCH_THIGH] * 3, [CROUCH_KNEE] * 3
    )
    rotations.update(
        {
            # Folding the chest forward is what buys the last of the eye
            # drop; the head pitches back up so the figure still looks where
            # it is aiming.
            "chest": [quat_x(s) for s in (-38.0, -36.0, -38.0)],
            "head": [quat_x(s) for s in (28.0, 27.0, 28.0)],
            "arm_r_upper": [quat_x(28.0)] * 3,
            "arm_l_upper": [quat_x(28.0)] * 3,
            "arm_r_lower": [quat_x(-40.0)] * 3,
            "arm_l_lower": [quat_x(-40.0)] * 3,
        }
    )
    # The breath rises off the floor solution and settles back onto it, rather
    # than sinking below it: a squat is already resting on its soles, so the
    # only direction a hip bob has left is up.
    translations = {"hips": [(0.0, 0.012, 0.0), (0.0, 0.0, 0.0), (0.0, 0.012, 0.0)]}
    return ("crouch_idle", *plant_on_floor(times, rotations, translations))


def crouch_move_clip():
    """A 0.7 s duck-walk shuffle.

    Deliberately non-committal about direction. Crouched movement is 45% of
    walking speed, and the readable fact at that speed is the stance, not the
    heading -- so the legs shuffle in place under the same squat rather than
    asserting a direction the way the run and strafe cycles do.
    """
    times = [0.0, 0.175, 0.35, 0.525, 0.7]
    base, lift = CROUCH_THIGH, CROUCH_LIFT_THIGH
    knee_base, knee_lift = CROUCH_KNEE, CROUCH_LIFT_KNEE
    # One leg is always at the base fold, so the solved hip height stays put
    # and the figure waddles rather than pumping up and down.
    rotations = crouch_pose_rotations(
        [base, base, lift, base, base],
        [knee_base, knee_base, knee_lift, knee_base, knee_base],
        [lift, base, base, base, lift],
        [knee_lift, knee_base, knee_base, knee_base, knee_lift],
    )
    rotations.update(
        {
            "chest": [quat_x(s) for s in (-38.0, -39.0, -38.0, -39.0, -38.0)],
            "head": [quat_x(28.0)] * 5,
            "arm_r_upper": [quat_x(28.0)] * 5,
            "arm_l_upper": [quat_x(28.0)] * 5,
            "arm_r_lower": [quat_x(-40.0)] * 5,
            "arm_l_lower": [quat_x(-40.0)] * 5,
        }
    )
    translations = {"hips": [(0.0, 0.0, 0.0)] * 5}
    return ("crouch_move", *plant_on_floor(times, rotations, translations))


def jump_clip():
    """A 0.6 s tuck-and-extend. Not looped; the state machine holds the end."""
    times = [0.0, 0.25, 0.6]
    rotations = {
        "leg_r_upper": [quat_x(s) for s in (0.0, 42.0, 12.0)],
        "leg_l_upper": [quat_x(s) for s in (0.0, 42.0, 12.0)],
        "leg_r_lower": [quat_x(s) for s in (0.0, -62.0, -18.0)],
        "leg_l_lower": [quat_x(s) for s in (0.0, -62.0, -18.0)],
        "arm_r_upper": [quat_x(s) for s in (0.0, -70.0, -40.0)],
        "arm_l_upper": [quat_x(s) for s in (0.0, -70.0, -40.0)],
    }
    return "jump", times, rotations, {}


def land_clip():
    """A 0.34 s absorb on touchdown. One-shot, layered over the ground pose.

    The first keyframe deliberately repeats the jump clip's last frame: the
    client cross-fades out of the held jump pose into this one, and a landing
    that starts from a standing figure snaps the legs straight for a frame
    before bending them again.
    """
    times = [0.0, 0.09, 0.34]
    thighs = (12.0, 34.0, 0.0)
    knees = (-18.0, -66.0, 0.0)
    rotations = {
        "leg_r_upper": [quat_x(s) for s in thighs],
        "leg_l_upper": [quat_x(s) for s in thighs],
        "leg_r_lower": [quat_x(s) for s in knees],
        "leg_l_lower": [quat_x(s) for s in knees],
        "chest": [quat_x(s) for s in (0.0, -14.0, 0.0)],
        "arm_r_upper": [quat_x(s) for s in (-40.0, -26.0, 0.0)],
        "arm_l_upper": [quat_x(s) for s in (-40.0, -26.0, 0.0)],
        "arm_r_lower": [quat_x(s) for s in (-8.0, -24.0, 0.0)],
        "arm_l_lower": [quat_x(s) for s in (-8.0, -24.0, 0.0)],
    }
    translations = {"hips": [(0.0, 0.0, 0.0)] * len(times)}
    return ("land", *plant_on_floor(times, rotations, translations))


def death_clip():
    """A 1.25 s collapse onto the back. One-shot; the client holds the end.

    Bodies used to vanish the frame the alive flag cleared, which reads as a
    dropped packet rather than as a kill and gives the killcam nothing to
    look at. The hips do the work: they sink, drift backward, and rotate 88
    degrees about X, which lays the spine along +Z and the legs along -Z.
    The keys are packed toward the front and the last half-second covers
    almost no movement, so the body arrives rather than stops. Height is
    solved per key like the crouch is, which is what stops the folded legs
    scything through the floor on the way down.
    """
    times = [0.0, 0.16, 0.45, 0.80, 1.25]
    # Thigh and knee per key. The last two are read against a hips joint that
    # is already lying down, so near-zero here means "flat on the ground",
    # slightly negative means the heels rest a little below the hips.
    thighs = (0.0, 55.0, 44.0, 10.0, -2.0)
    knees = (0.0, -105.0, -86.0, -28.0, -8.0)
    rotations = {
        "hips": [quat_x(s) for s in (0.0, 12.0, 50.0, 82.0, 88.0)],
        "chest": [quat_x(s) for s in (0.0, -12.0, -22.0, -8.0, -2.0)],
        # Chin to the chest as the knees give, then the head lolls back once
        # the shoulders hit the floor.
        "head": [quat_x(s) for s in (0.0, -18.0, -28.0, -6.0, 8.0)],
        "leg_r_upper": [quat_zx(s, t) for s, t in zip((0.0, 2.0, 6.0, 10.0, 12.0), thighs)],
        "leg_l_upper": [quat_zx(-s, t) for s, t in zip((0.0, 2.0, 6.0, 10.0, 12.0), thighs)],
        "leg_r_lower": [quat_x(s) for s in knees],
        "leg_l_lower": [quat_x(s) for s in knees],
        # Arms sprawl outward about Z so they end up flat on the floor beside
        # the body rather than buried under it.
        "arm_r_upper": [
            quat_zx(s, t)
            for s, t in zip((0.0, 14.0, 34.0, 50.0, 55.0), (0.0, -18.0, -34.0, -20.0, -8.0))
        ],
        "arm_l_upper": [
            quat_zx(-s, t)
            for s, t in zip((0.0, 14.0, 34.0, 50.0, 55.0), (0.0, -18.0, -34.0, -20.0, -8.0))
        ],
        "arm_r_lower": [quat_x(s) for s in (-14.0, -34.0, -46.0, -24.0, -12.0)],
        "arm_l_lower": [quat_x(s) for s in (-14.0, -34.0, -46.0, -24.0, -12.0)],
    }
    # Only the backward drift is authored; how low the hips sit at each key
    # falls out of the pose once it is planted on the floor.
    translations = {"hips": [(0.0, 0.0, z) for z in (0.0, 0.02, 0.10, 0.22, 0.26)]}
    return ("death", *plant_on_floor(times, rotations, translations))


CLIPS = [
    idle_clip,
    run_clip,
    lambda: strafe_clip("strafe_right", 1.0),
    lambda: strafe_clip("strafe_left", -1.0),
    crouch_idle_clip,
    crouch_move_clip,
    jump_clip,
    land_clip,
    death_clip,
]


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
        name, times, rotations, translations = clip()
        time_view = b.add_view(b"".join(struct.pack("<f", t) for t in times))
        time_acc = b.add_accessor(time_view, 5126, len(times), "SCALAR",
                                  ([times[0]], [times[-1]]))
        samplers, animation_channels = [], []
        # (path, glTF accessor type, struct format) per channel kind.
        for path, type_name, fmt, channels in (
                ("rotation", "VEC4", "<4f", rotations),
                ("translation", "VEC3", "<3f", translations)):
            for joint_name, values in channels.items():
                assert len(values) == len(times), f"{name}/{joint_name} keyframe count"
                view = b.add_view(b"".join(struct.pack(fmt, *v) for v in values))
                acc = b.add_accessor(view, 5126, len(values), type_name)
                samplers.append({"input": time_acc, "output": acc,
                                 "interpolation": "LINEAR"})
                animation_channels.append({
                    "sampler": len(samplers) - 1,
                    "target": {"node": JOINT_INDEX[joint_name], "path": path},
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
