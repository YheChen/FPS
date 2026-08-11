#!/usr/bin/env python3
"""Generates the first-person weapon models in assets/weapons/*.glb.

Pure-python GLB writer, same approach as gen_arena.py and gen_character.py:
no dependencies, so an asset is reviewable as a diff of the script that made
it rather than as an opaque binary.

FIVE weapons, ONE armoury. The four guns were specced independently, which
produced four internally coherent designs that did not agree with each other
about the things a SET has to agree on -- bore height above the hand, the
material palette, and where the gun sits relative to the eye. Those are
reconciled here (see FAMILY RULES); the parts that make each weapon
identifiable in silhouette are kept exactly as designed.

FAMILY RULES (what makes five separate models read as one armoury):

  1. The GRIP IS THE ORIGIN. (0,0,0) sits inside the hand's grasp on every
     weapon. Both consumers -- the first-person viewmodel offsetting from the
     eye, and (later) a third-person model parented to the character's hand
     joint -- then add the SAME offset for every weapon. Put the origin
     anywhere else and each consumer carries a per-weapon correction, and
     those corrections drift apart the moment one weapon is a different
     length.

  2. The BORE AXIS IS 0.105 m ABOVE THE ORIGIN on all four guns. The specs
     came in at 0.100 / 0.105 / 0.125 / 0.055. That is not a stylistic
     difference: the viewmodel holds the grip at one fixed offset from the
     eye, so a bore that moves per weapon makes the barrel sit at a different
     screen height every time you switch, which reads as the camera moving
     rather than the gun. One number, and swapping weapons swaps only the
     silhouette. The knife deliberately breaks this -- see below.

  3. ONE MATERIAL PALETTE, shared by all five (MATERIALS below). Each weapon
     draws 3-4 of the six. The specs each proposed their own `mat_gun_body`
     with a different colour; three subtly different dark greys across four
     guns is how a set stops looking like a set.

  4. EVERYTHING IS DARK. The character is baseColorFactor 0.82-0.90 and the
     arena walls are 0.85-1.0, so every surface a weapon is seen against is
     pale. Dark bodies (0.13-0.20) silhouette against all of it. The two
     non-grey materials -- olive magazine, warm brown furniture -- exist
     because a fully monochrome gun loses its identifying feature at range.

  5. METALLIC 0.0 EVERYWHERE, matching gen_arena.py and gen_character.py.
     These must not be the one asset that exercises an untested BRDF path.

  6. LENGTH IS THE COARSEST READ, and the four guns take four bands:
     smg 0.54 m, shotgun 0.88 m, rifle 0.93 m, sniper 1.30 m, against a
     1.80 m character. The knife is 0.32 m, a quarter of the sniper.

THE KNIFE IS NOT A GUN. Slot 5 is melee (assets/weapons/knife.cfg,
`melee=true`): no magazine, no reload, and arm's-reach range. It gets a
blade, and it breaks family rule 2 on purpose -- its working edge runs
through the hand's own line rather than 0.105 m above it, which is the
clearest possible statement that it is not held like the other four. It has
NO `muzzle` node, because it has no muzzle; the client keys the muzzle flash
off the presence of that node, so the absence is what stops a knife swing
from spitting fire.

STRUCTURE. Every box is the same unit cube: one shared set of accessors, one
mesh per material, and one NODE per box carrying its own translation,
rotation and scale -- gen_arena.py's proven shape. That keeps each part
individually addressable by name (a later pump-action or bolt-cycle
animation wants `pump` and `bolt_handle` as nodes) while costing 24 vertices
of buffer for the whole file. Non-uniform node scale is safe for normals
because the renderer builds a per-draw inverse-transpose normal matrix, the
same way every arena box already relies on.

Plus one mesh-less marker node named `muzzle` on each gun, read by name --
the mechanism gen_arena.py already uses for `spawn_N`. That makes the flash
position a property of the ASSET, so four weapons of four different lengths
need zero `if (weapon == ...)` in the client.

Conventions: meters, +Y up, -Z forward (glTF standard), origin at the grip.

Usage:
    python3 tools/gen_weapons.py                     # regenerate every weapon
    python3 tools/gen_weapons.py --weapon sniper     # just one
    python3 tools/gen_weapons.py --weapon knife --out /tmp/k.glb
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

# Bore axis height above the grip origin, shared by all four guns. See family
# rule 2: this is the number that makes swapping weapons swap the silhouette
# and nothing else.
BORE = 0.105

# name -> (baseColorFactor, roughness). metallic is 0.0 for all of them.
MATERIALS = [
    # Receivers, handguards, stocks: the main structural mass.
    ("mat_gun_body", [0.20, 0.21, 0.24, 1.0], 0.60),
    # Barrels, rails, sights, optics: darker and smoother than the body, so
    # the two separate under a specular highlight instead of merging.
    ("mat_gun_steel", [0.13, 0.14, 0.16, 1.0], 0.40),
    # Grips and other rubber: same value family, but rough enough to kill the
    # highlight, which is what makes it read as grip material.
    ("mat_gun_grip", [0.17, 0.175, 0.19, 1.0], 0.85),
    # Desaturated olive. The identity colour of a magazine-fed gun: it is the
    # one feature that must not merge into the dark body at range.
    ("mat_gun_mag", [0.33, 0.38, 0.34, 1.0], 0.75),
    # Warm brown furniture. The only warm hue in a scene of pale-grey greybox,
    # and the read that survives after the geometry has blurred out entirely.
    ("mat_gun_wood", [0.30, 0.22, 0.16, 1.0], 0.85),
    # Bright steel, the lightest thing on any weapon. Reserved for the parts
    # that say which end is dangerous: muzzle devices, the bead, the blade.
    ("mat_gun_bright", [0.30, 0.31, 0.33, 1.0], 0.45),
]
MATERIAL_INDEX = {name: i for i, (name, _, _) in enumerate(MATERIALS)}

# --- the arsenal -----------------------------------------------------------
# Each part is (name, material, center, size, rot_x_degrees).
#   center  = box centre offset from the GRIP ORIGIN, meters
#   size    = FULL x,y,z extent (not half extents)
#   rot_x   = rotation about the box's own centre; positive swings the box's
#             -Y end forward (-Z), which is the classic forward magazine cant.
#
# Every table is authored UPRIGHT. A weapon that is held canted says so with
# its own ROLL entry (see WEAPONS), which is applied to the whole table at
# build time -- that keeps "what shape is it" and "how is it held" separate,
# and it is the only reason a part list stays readable.
#
# Adjacent boxes deliberately overlap by ~10 mm rather than sharing a face.
# Coincident faces z-fight, and a viewmodel fills a quarter of the screen --
# do not "clean this up".

# rifle -- the straight line of the set. One unbroken top edge from butt-plate
# to muzzle (stock, receiver, rail and handguard tops all within 2 cm), and
# exactly one thing hanging below it forward of the hand: a long forward-canted
# 30-round box magazine. Two prongs under a ruler. The magazine is the feature
# and nothing else is allowed to compete with it -- the sights are 3 cm nubs
# and the muzzle device is a 5 cm step.
RIFLE = [
    ("grip", "mat_gun_grip", (0.000, -0.010, 0.012), (0.038, 0.135, 0.052), -12.0),
    ("receiver", "mat_gun_body", (0.000, 0.100, -0.030), (0.052, 0.075, 0.320), 0.0),
    ("top_rail", "mat_gun_steel", (0.000, 0.145, -0.090), (0.026, 0.018, 0.420), 0.0),
    ("handguard", "mat_gun_body", (0.000, 0.103, -0.310), (0.046, 0.052, 0.260), 0.0),
    ("barrel", "mat_gun_steel", (0.000, 0.105, -0.515), (0.026, 0.026, 0.170), 0.0),
    ("muzzle_brake", "mat_gun_bright", (0.000, 0.105, -0.622), (0.040, 0.040, 0.056), 0.0),
    ("stock", "mat_gun_body", (0.000, 0.093, 0.200), (0.046, 0.086, 0.160), 0.0),
    ("magazine", "mat_gun_mag", (0.000, -0.027, -0.115), (0.030, 0.190, 0.070), 15.0),
    ("front_sight", "mat_gun_steel", (0.000, 0.155, -0.425), (0.018, 0.055, 0.022), 0.0),
    ("rear_sight", "mat_gun_steel", (0.000, 0.168, -0.105), (0.024, 0.032, 0.020), 0.0),
]

# smg -- the short one. Claims the 0.50-0.60 m band, a deep forward-raked
# stick magazine hanging 26% of the gun's own length below the hand, and a
# SKELETAL open stock: a thin strut with air above it ending in a small
# paddle, so the back half reads as a line with a plate rather than a solid
# block. Almost no barrel protrudes. Stubby and bottom-heavy, which is what
# 900 rpm at 14 damage should look like before the HUD says so.
SMG = [
    ("receiver", "mat_gun_body", (0.000, 0.105, -0.050), (0.052, 0.075, 0.300), 0.0),
    ("handguard", "mat_gun_body", (0.000, 0.100, -0.243), (0.044, 0.052, 0.085), 0.0),
    ("barrel", "mat_gun_steel", (0.000, 0.105, -0.310), (0.022, 0.022, 0.050), 0.0),
    ("magazine", "mat_gun_mag", (0.000, -0.032, -0.102), (0.042, 0.210, 0.058), 15.0),
    ("grip", "mat_gun_grip", (0.000, 0.012, -0.002), (0.040, 0.115, 0.050), -10.0),
    ("stock_strut", "mat_gun_body", (0.000, 0.088, 0.145), (0.030, 0.024, 0.090), 0.0),
    ("stock_plate", "mat_gun_steel", (0.000, 0.078, 0.197), (0.048, 0.088, 0.014), 0.0),
    # On the LEFT face on purpose: in a right-handed hold that is the face
    # turned toward the first-person camera, so it is the one detail that
    # earns its vertices at viewmodel range.
    ("charging_handle", "mat_gun_steel", (-0.036, 0.128, -0.140), (0.020, 0.016, 0.060), 0.0),
    ("sight_front", "mat_gun_steel", (0.000, 0.152, -0.190), (0.014, 0.020, 0.016), 0.0),
    ("sight_rear", "mat_gun_steel", (0.000, 0.152, 0.060), (0.024, 0.020, 0.018), 0.0),
]

# shotgun -- the fat one. Shorter than the rifle but with the largest
# cross-section in the set, so it reads as a heavy tool rather than a rifle.
# Its signature is the STEPPED TWIN-TUBE front: a magazine tube directly under
# the barrel, stopping 45 mm short of the muzzle, which doubles the apparent
# thickness of the front half and gives it a blunt "=" muzzle no other weapon
# here has. Nothing hangs below the receiver -- no box magazine breaks the
# bottom line -- and the two warm-brown masses (pump at 1/3, butt plate at the
# tail) are the read that survives when the geometry blurs out.
SHOTGUN = [
    ("receiver", "mat_gun_body", (0.000, 0.085, -0.135), (0.072, 0.115, 0.300), 0.0),
    ("barrel", "mat_gun_steel", (0.000, 0.105, -0.470), (0.044, 0.044, 0.380), 0.0),
    ("magazine_tube", "mat_gun_steel", (0.000, 0.063, -0.447), (0.040, 0.040, 0.345), 0.0),
    ("pump", "mat_gun_wood", (0.000, 0.068, -0.430), (0.078, 0.098, 0.200), 0.0),
    ("ejection_port", "mat_gun_steel", (0.038, 0.095, -0.160), (0.010, 0.040, 0.100), 0.0),
    ("trigger_guard", "mat_gun_body", (0.000, 0.001, -0.068), (0.020, 0.026, 0.085), 0.0),
    ("grip", "mat_gun_grip", (0.000, -0.068, 0.012), (0.052, 0.150, 0.070), 0.0),
    ("stock_wrist", "mat_gun_body", (0.000, -0.010, 0.080), (0.050, 0.070, 0.130), 0.0),
    ("stock_butt", "mat_gun_body", (0.000, -0.032, 0.173), (0.056, 0.125, 0.066), 0.0),
    ("butt_plate", "mat_gun_wood", (0.000, -0.032, 0.213), (0.060, 0.129, 0.014), 0.0),
    ("front_bead", "mat_gun_bright", (0.000, 0.136, -0.646), (0.012, 0.018, 0.016), 0.0),
]

# sniper -- the long thin one, and the only weapon in the set with SKY PUNCHED
# THROUGH IT. A stepped scope floats 64 mm above the receiver on two thin
# posts with a 200 mm unobstructed gap between them: negative space is the one
# thing that survives at distance on a blocky model, and every other weapon
# here is a solid mass. Do not close that gap with a rail. The barrel then
# runs bare for 540 mm ahead of the optic -- length and thinness are the read
# past 50 m. The magazine is a deliberate 100 mm stub: a 5-round gun must not
# be mistaken for the smg.
SNIPER = [
    ("receiver", "mat_gun_body", (0.000, 0.105, -0.090), (0.058, 0.100, 0.380), 0.0),
    ("barrel", "mat_gun_body", (0.000, 0.105, -0.570), (0.036, 0.036, 0.620), 0.0),
    ("muzzle_brake", "mat_gun_bright", (0.000, 0.105, -0.900), (0.056, 0.056, 0.080), 0.0),
    ("magazine", "mat_gun_body", (0.000, 0.010, -0.120), (0.042, 0.120, 0.100), 0.0),
    ("grip", "mat_gun_grip", (0.000, -0.005, 0.022), (0.050, 0.180, 0.068), 0.0),
    ("stock", "mat_gun_wood", (0.000, 0.095, 0.220), (0.050, 0.110, 0.280), 0.0),
    ("comb", "mat_gun_wood", (0.000, 0.170, 0.210), (0.052, 0.050, 0.220), 0.0),
    ("scope_tube", "mat_gun_steel", (0.000, 0.250, -0.130), (0.062, 0.062, 0.380), 0.0),
    ("scope_objective", "mat_gun_steel", (0.000, 0.250, -0.350), (0.086, 0.086, 0.100), 0.0),
    ("scope_mount_front", "mat_gun_body", (0.000, 0.186, -0.240), (0.030, 0.080, 0.040), 0.0),
    ("scope_mount_rear", "mat_gun_body", (0.000, 0.186, 0.000), (0.030, 0.080, 0.040), 0.0),
    ("bolt_handle", "mat_gun_bright", (0.058, 0.128, -0.070), (0.058, 0.026, 0.060), 0.0),
]

# knife -- slot 5, and the only thing here that is not a gun.
#
# Everything about it is an anti-read of the other four. It is 0.32 m, a
# quarter of the sniper. Its working edge runs through the hand's own line
# (y ~ 0) instead of 0.105 m above it, so the moment it comes up the whole
# screen silhouette drops to hand height. It is mostly BRIGHT -- the blade is
# the lightest surface in the armoury, the exact inverse of four dark guns --
# because a knife has no muzzle flash to say where its business end is and
# the blade has to do that job on its own. The crossguard is the one wide
# element, and it is wide precisely so the shape reads as knife rather than
# as a stick at a glance.
#
# It is also the one weapon with a ROLL (see WEAPONS). A 10 mm blade held
# upright points its flat at the first-person camera edge-on, and a knife
# seen edge-on is a wire. Canting it presents the flat instead. That is
# `how it is held`, so it lives on the weapon rather than smeared across
# five part rows -- and emphatically not in the client, which must not know
# which slot is the knife.
#
# No `muzzle` node: it has none, and the client keys the flash off that node.
KNIFE = [
    ("handle", "mat_gun_grip", (0.000, 0.000, 0.045), (0.030, 0.036, 0.110), 0.0),
    ("pommel", "mat_gun_steel", (0.000, 0.000, 0.106), (0.036, 0.042, 0.016), 0.0),
    ("guard", "mat_gun_steel", (0.000, 0.000, -0.017), (0.070, 0.020, 0.016), 0.0),
    ("blade", "mat_gun_bright", (0.000, 0.004, -0.098), (0.010, 0.038, 0.150), 0.0),
    # Narrower, and dropped 5 mm, so the point tapers toward the edge instead
    # of ending in a square stub. Cheapest possible "this end goes in first".
    ("tip", "mat_gun_bright", (0.000, -0.005, -0.190), (0.010, 0.020, 0.044), 0.0),
]

# name -> (parts, muzzle offset or None, HOLD).
#
# The muzzle offset sits ~10-30 mm in FRONT of the physical muzzle face:
# emit_muzzle_flash's particles are additive and depth-tested, so spawning
# them flush with the brake gets the near ones rejected by the brake's own
# faces, which reads as the flash leaking out through the barrel wall.
#
# HOLD is (pitch, roll) in degrees about the grip origin: how the hand
# PRESENTS this weapon, as opposed to what shape it is. It is a rotation and
# never a translation, so the origin stays inside the grip and family rule 1
# survives. The four guns are held level, because that is what a shouldered
# gun is. The knife is carried point-up and canted, which is both how a
# knife is actually held and the only way a 0.32 m blade shares a hold with
# a 1.30 m rifle without disappearing into the bottom corner of the frame.
#
# Putting this in the asset rather than in the client is the point: the
# viewmodel code has one hold offset and no idea which slot is a knife.
WEAPONS = {
    "rifle": (RIFLE, (0.000, BORE, -0.680), (0.0, 0.0)),
    "smg": (SMG, (0.000, BORE, -0.345), (0.0, 0.0)),
    "shotgun": (SHOTGUN, (0.000, BORE, -0.680), (0.0, 0.0)),
    "sniper": (SNIPER, (0.000, BORE, -0.950), (0.0, 0.0)),
    "knife": (KNIFE, None, (35.0, 62.0)),
}


def xrot_quat(degrees):
    a = math.radians(degrees) * 0.5
    return [math.sin(a), 0.0, 0.0, math.cos(a)]  # glTF order: x, y, z, w


def zrot_quat(degrees):
    a = math.radians(degrees) * 0.5
    return [0.0, 0.0, math.sin(a), math.cos(a)]


def quat_mul(a, b):
    """a * b, glTF order (x, y, z, w). Applies b first, then a."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ]


def quat_rotate(q, v):
    """Rotates a vector by a glTF-order quaternion."""
    x, y, z, w = q
    # t = 2 * (q.xyz x v); v' = v + w*t + q.xyz x t
    tx = 2.0 * (y * v[2] - z * v[1])
    ty = 2.0 * (z * v[0] - x * v[2])
    tz = 2.0 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + y * tz - z * ty,
            v[1] + w * ty + z * tx - x * tz,
            v[2] + w * tz + x * ty - y * tx)


IDENTITY_QUAT = [0.0, 0.0, 0.0, 1.0]


def hold_quat(name):
    """The weapon's hold as one rotation about the grip origin."""
    pitch, roll = WEAPONS[name][2]
    return quat_mul(xrot_quat(pitch), zrot_quat(roll))


def placed_parts(name, hold=None):
    """The weapon's part list with its hold baked in.

    Pass IDENTITY_QUAT for `hold` to get the DESIGN frame -- upright, as the
    tables are authored. That is the frame the family rules are stated in,
    so it is the frame they have to be checked in.

    Rows come back as (name, material, center, size, quaternion), with the
    rotation already composed -- the raw table's rot_x degrees never escape
    this function. The hold has to move each box's CENTRE about the grip
    origin as well as turn the box itself, or the parts fan out instead of
    rotating together. Everything downstream -- bounds, the GLB nodes, the
    muzzle marker, the family checks -- reads this rather than the raw table,
    so the hold cannot be applied in one place and forgotten in another.
    """
    hold = hold_quat(name) if hold is None else hold
    placed = []
    for part_name, material, center, size, rot_x in WEAPONS[name][0]:
        placed.append((part_name, material, quat_rotate(hold, center), size,
                       quat_mul(hold, xrot_quat(rot_x))))
    return placed


def placed_muzzle(name):
    """The muzzle marker in the same held frame the boxes are in."""
    muzzle = WEAPONS[name][1]
    return None if muzzle is None else quat_rotate(hold_quat(name), muzzle)


def unit_cube():
    """A unit cube with per-face normals and plain 0..1 UVs.

    Shared by every box in every weapon: the node's scale does the sizing, so
    one 24-vertex mesh covers all 48 boxes in the armoury. UVs are per-face
    0..1 rather than gen_arena.py's world-scaled tiling because nothing here
    samples a texture -- the attribute exists only because eng::Vertex is one
    global format.
    """
    h = 0.5
    # normal, then 4 corners wound CCW seen from outside the cube.
    faces = [
        ((1, 0, 0), [(h, -h, h), (h, -h, -h), (h, h, -h), (h, h, h)]),
        ((-1, 0, 0), [(-h, -h, -h), (-h, -h, h), (-h, h, h), (-h, h, -h)]),
        ((0, 1, 0), [(-h, h, h), (h, h, h), (h, h, -h), (-h, h, -h)]),
        ((0, -1, 0), [(-h, -h, -h), (h, -h, -h), (h, -h, h), (-h, -h, h)]),
        ((0, 0, 1), [(-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h)]),
        ((0, 0, -1), [(h, -h, -h), (-h, -h, -h), (-h, h, -h), (h, h, -h)]),
    ]
    corner_uv = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]

    positions, normals, uvs, indices = [], [], [], []
    for normal, corners in faces:
        base = len(positions)
        for i, corner in enumerate(corners):
            positions.append(corner)
            normals.append(normal)
            uvs.append(corner_uv[i])
        for offset in (0, 1, 2, 0, 2, 3):
            indices.append(base + offset)
    return positions, normals, uvs, indices


def bounds(parts):
    """Axis-aligned bounds of a placed_parts() list, in grip-local meters.

    A rotated box is bounded by its rotated corners, not by its size, so the
    rotation is applied here rather than approximated -- the magazine cant is
    what decides how far below the hand the rifle and smg actually reach.
    """
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    for _, _, center, size, rotation in parts:
        for sx in (-0.5, 0.5):
            for sy in (-0.5, 0.5):
                for sz in (-0.5, 0.5):
                    corner = quat_rotate(rotation, (sx * size[0], sy * size[1], sz * size[2]))
                    for i in range(3):
                        lo[i] = min(lo[i], center[i] + corner[i])
                        hi[i] = max(hi[i], center[i] + corner[i])
    return lo, hi


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


def build_glb(weapon_name):
    parts = placed_parts(weapon_name)
    muzzle = placed_muzzle(weapon_name)
    b = GlbBuilder()

    # --- one shared cube, referenced by every box -------------------------
    positions, normals, uvs, indices = unit_cube()
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

    # One mesh per material this weapon actually uses. Boxes then differ only
    # by node transform, so the whole model is a few hundred bytes of geometry
    # and a page of node JSON.
    #
    # No JOINTS_0 / WEIGHTS_0: weapons are unskinned and ride a node transform.
    # eng::Vertex carries skinning attributes globally and the loader
    # zero-fills them; the shader's "all weights 0 -> use the model matrix
    # unchanged" path is what has to handle that, and it already does for
    # every arena box.
    used_materials = []
    for _, material, _, _, _ in parts:
        if material not in used_materials:
            used_materials.append(material)
    mesh_for_material = {}
    meshes = []
    for material in used_materials:
        mesh_for_material[material] = len(meshes)
        meshes.append({
            "name": f"mesh_{material}",
            "primitives": [{
                "attributes": {"POSITION": pos_acc, "NORMAL": nrm_acc, "TEXCOORD_0": uv_acc},
                "indices": idx_acc,
                "material": MATERIAL_INDEX[material],
            }],
        })

    nodes = []
    for name, material, center, size, rotation in parts:
        node = {
            "name": name,
            "mesh": mesh_for_material[material],
            "translation": list(center),
            "scale": list(size),
        }
        if rotation[3] < 1.0 - 1e-9:  # not the identity quaternion
            node["rotation"] = list(rotation)
        nodes.append(node)

    if muzzle is not None:
        nodes.append({"name": "muzzle", "translation": list(muzzle)})

    # The full palette ships in every file even when a weapon uses four of the
    # six. Six unused JSON objects cost ~600 bytes and keep material indices
    # identical across the set, which is what makes the table above readable
    # as one palette rather than five.
    materials = [
        {
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorFactor": color,
                "metallicFactor": 0.0,
                "roughnessFactor": roughness,
            },
        }
        for name, color, roughness in MATERIALS
    ]

    gltf = {
        "asset": {"version": "2.0", "generator": "fps-engine gen_weapons.py"},
        "scene": 0,
        "scenes": [{"name": weapon_name, "nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
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


def check_family(name):
    """Asserts the family rules this file's docstring claims to hold.

    These are the invariants a reviewer would otherwise have to re-derive from
    the tables by hand, and the ones a careless edit breaks silently: a weapon
    whose origin drifts out of the grip, or whose bore stops agreeing with the
    rest of the set, still generates a perfectly valid GLB.
    """
    parts = placed_parts(name, IDENTITY_QUAT)
    muzzle = WEAPONS[name][1]
    lo, hi = bounds(parts)
    length = hi[2] - lo[2]

    # Family rule 1: the origin is inside the grip, so both consumers attach
    # with no per-weapon correction. Checked against the grip's own rotated
    # extent, which is what actually contains the hand once a roll is applied.
    grip = next(part for part in parts if part[0] in ("grip", "handle"))
    grip_lo, grip_hi = bounds([grip])
    for axis in range(3):
        assert grip_lo[axis] <= 1e-6 and grip_hi[axis] >= -1e-6, \
            f"{name}: origin outside the grip on axis {axis}"

    if muzzle is not None:
        # Family rule 2: one bore height for the whole set.
        assert abs(muzzle[1] - BORE) < 1e-6, f"{name}: muzzle is off the shared bore axis"
        # The flash must spawn AHEAD of the geometry, or it is born inside the
        # barrel and half of it is depth-rejected on the first frame.
        assert muzzle[2] < lo[2], f"{name}: muzzle marker is inside the mesh"
        assert muzzle[2] > lo[2] - 0.05, f"{name}: muzzle marker floats too far off the barrel"
    return lo, hi, length


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weapon", choices=sorted(WEAPONS), help="one weapon (default: all)")
    parser.add_argument("--out", type=Path, help="output path; requires --weapon")
    args = parser.parse_args()
    if args.out and not args.weapon:
        parser.error("--out names a single file, so it needs --weapon")

    root = Path(__file__).resolve().parent.parent
    for name in ([args.weapon] if args.weapon else sorted(WEAPONS)):
        lo, hi, length = check_family(name)
        out_path = args.out or root / "assets" / "weapons" / f"{name}.glb"
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(build_glb(name))
        print(f"wrote {out_path} ({out_path.stat().st_size} bytes), "
              f"{len(WEAPONS[name][0])} boxes, {length:.3f} m long, "
              f"x {lo[0]:+.3f}..{hi[0]:+.3f}, y {lo[1]:+.3f}..{hi[1]:+.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
