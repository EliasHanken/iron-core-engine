"""An interaction: a bunker hatch in the ground, its lid heaved open, and the
ladder below climbed down hand over hand; and back up and out. Built like the
door (door.py): the hatch is modelled here (a steel frame round the hole, a lid
on a hinge at the far edge with a handle near this one, a ladder on the far wall
with grab rails standing above the ground), and the hands take hold of its real
surface: the lid's handle, the rails as they go down.

Clips: hatch_down (kneel, open the lid, climb down), ladder_down (the lid
already open), ladder_up (from the bunker floor, up and out over the far edge),
and still poses: closed, opened.

Space: the hatch's own, +Y away from the player who comes to it, +Z up, the
origin on the ground in the middle of the hole. The bunker's floor is DEPTH
below; its room runs back under where the player came from (-Y).
"""
import math

import bmesh
import bpy
from mathutils import Matrix, Vector

import rig
from clips import Hand, Pose, ease_in, ease_out, mix_hand, smooth, track
from scope import flat_material
from weapons.door import Viewer, mix_viewer

NAME = "hatch"
PARTS = ["LID"]
MAGAZINE = None
EYE_FOV = 60.0
EYE = 1.62

HOLE = 0.45          # Half the opening.
DEPTH = 3.2          # From the ground to the bunker's floor.
RAIL_X = 0.25        # The ladder's rails, either side.
RAIL_Y = HOLE - 0.08  # Against the far wall.
RAIL_TOP = 0.9       # How far the rails stand above the ground.
HINGE = Vector((0.0, HOLE + 0.04, 0.11))
LID_OPEN = 112.0


def _box(bm, lo, hi, material, group, groups):
    verts = [bm.verts.new(Vector((x, y, z))) for x in (lo.x, hi.x) for y in (lo.y, hi.y) for z in (lo.z, hi.z)]
    for quad in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        bm.faces.new([verts[i] for i in quad]).material_index = material
    groups.extend((v, group) for v in verts)


def load(pack):
    del pack
    data = bpy.data.armatures.new("hatch_rig")
    skeleton = bpy.data.objects.new("hatch_rig", data)
    bpy.context.scene.collection.objects.link(skeleton)
    rig.select_only(skeleton)
    bpy.ops.object.mode_set(mode="EDIT")
    frame = data.edit_bones.new("FRAME")
    frame.head, frame.tail = Vector(), Vector((0.0, 0.1, 0.0))
    lid = data.edit_bones.new("LID")
    lid.head, lid.tail = HINGE, HINGE + Vector((0.2, 0.0, 0.0))
    lid.parent = frame
    bpy.ops.object.mode_set(mode="OBJECT")

    bm = bmesh.new()
    groups = []
    h, w = HOLE, 0.08
    # 0 the steel frame and ladder, 1 the lid's paint, 2 the handle's bright metal.
    # The frame: a lip round the hole, and its sides down through the slab.
    for lo, hi in ((Vector((-h - w, -h - w, 0.0)), Vector((h + w, -h, 0.06))),
                   (Vector((-h - w, h, 0.0)), Vector((h + w, h + w, 0.06))),
                   (Vector((-h - w, -h, 0.0)), Vector((-h, h, 0.06))),
                   (Vector((h, -h, 0.0)), Vector((h + w, h, 0.06)))):
        _box(bm, lo, hi, 0, "FRAME", groups)
    for lo, hi in ((Vector((-h, -h, -0.6)), Vector((h, -h + 0.01, 0.0))),
                   (Vector((-h, h - 0.01, -0.6)), Vector((h, h, 0.0))),
                   (Vector((-h, -h, -0.6)), Vector((-h + 0.01, h, 0.0))),
                   (Vector((h - 0.01, -h, -0.6)), Vector((h, h, 0.0)))):
        _box(bm, lo, hi, 0, "FRAME", groups)
    # The ladder: two rails from the floor to above the ground, rungs between.
    for x in (-RAIL_X, RAIL_X):
        _box(bm, Vector((x - 0.02, RAIL_Y - 0.02, -DEPTH)), Vector((x + 0.02, RAIL_Y + 0.02, RAIL_TOP)), 0, "FRAME",
             groups)
    z = -0.25
    while z > -DEPTH + 0.15:
        _box(bm, Vector((-RAIL_X, RAIL_Y - 0.013, z - 0.013)), Vector((RAIL_X, RAIL_Y + 0.013, z + 0.013)), 0, "FRAME",
             groups)
        z -= 0.28
    # The lid: a plate over the hole, stiffened underneath, a handle near this edge.
    _box(bm, Vector((-h - 0.03, -h - 0.03, 0.06)), Vector((h + 0.03, h + 0.04, 0.11)), 1, "LID", groups)
    _box(bm, Vector((-0.05, -h + 0.05, 0.02)), Vector((0.05, h - 0.05, 0.06)), 1, "LID", groups)
    for x in (-0.11, 0.11):
        _box(bm, Vector((x - 0.012, -h + 0.06, 0.11)), Vector((x + 0.012, -h + 0.085, 0.16)), 2, "LID", groups)
    _box(bm, Vector((-0.12, -h + 0.06, 0.15)), Vector((0.12, -h + 0.085, 0.172)), 2, "LID", groups)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    mesh = bpy.data.meshes.new("hatch")
    bm.to_mesh(mesh)
    index = {v: i for i, v in enumerate(bm.verts)}
    bm.free()
    gun = bpy.data.objects.new("hatch", mesh)
    bpy.context.scene.collection.objects.link(gun)
    for name in ("FRAME", "LID"):
        gun.vertex_groups.new(name=name)
    for v, name in groups:
        gun.vertex_groups[name].add([index[v]], 1.0, "REPLACE")
    gun.modifiers.new("Armature", "ARMATURE").object = skeleton
    mesh.materials.append(flat_material("hatch_steel", (0.16, 0.16, 0.17), 0.8, 0.5))
    mesh.materials.append(flat_material("hatch_paint", (0.2, 0.24, 0.16), 0.3, 0.6))
    mesh.materials.append(flat_material("hatch_handle", (0.5, 0.5, 0.52), 1.0, 0.3))
    return skeleton, gun


def lid(opened):
    """The lid's move from shut, swung up and over about its hinge by `opened` degrees."""
    return (Matrix.Translation(HINGE) @ Matrix.Rotation(math.radians(-opened), 4, "X") @
            Matrix.Translation(-HINGE))


def move_parts(pose, weapon, p):
    bone = pose["LID"]
    bone.matrix = weapon @ lid(p.parts.get("lid", 0.0)) @ bone.bone.matrix_local


# --- Hands ---
GRASP = {"index": (0.9, 1.1, 0.8), "middle": (1.0, 1.15, 0.8), "ring": (1.05, 1.15, 0.8), "pinky": (1.1, 1.1, 0.8)}
FLAT = {k: (0.25, 0.35, 0.2) for k in GRASP}
LOOSE = {k: (0.4, 0.5, 0.3) for k in GRASP}


def on_handle(opened):
    """The right hand round the lid's handle, palm down at first, turning with the lid."""
    m = lid(opened)
    turn = m.to_3x3()
    return Hand(m @ Vector((0.03, -HOLE + 0.0725, 0.161)), turn @ Vector((0.0, 0.3, -1.0)),
                turn @ Vector((0.0, 1.0, 0.2)), GRASP, thumb=(0.5, 0.5, 0.35), contact=1.0, spin=25.0, yaw=25.0)


def on_rail(side, z):
    """A hand round one of the ladder's rails at height `z`, the way rails are held:
    palms facing each other across the ladder, thumbs up, fingers round toward the
    wall."""
    x = RAIL_X if side == "r" else -RAIL_X
    inward = -1.0 if side == "r" else 1.0
    return Hand(Vector((x, RAIL_Y, z)), Vector((inward, 0.15, 0.0)), Vector((0.0, 1.0, 0.1)),
                GRASP, thumb=(0.4, 0.5, 0.3), contact=1.0, spin=25.0, yaw=25.0)


def braced():
    """The left palm flat on the ground beside the hatch, kneeling."""
    return Hand(Vector((-0.42, -HOLE - 0.32, 0.02)), Vector((0.0, 0.0, -1.0)), Vector((0.3, 1.0, 0.0)), FLAT,
                thumb=(0.2, 0.3, 0.2), contact=1.0, spin=20.0, yaw=25.0)


def from_eye(viewer, side):
    x = 0.3 if side == "r" else -0.3
    m = viewer.camera()
    turn = m.to_3x3()
    return Hand(m @ Vector((x, 0.12, -0.62)), turn @ Vector((-1.0 if side == "r" else 1.0, 0.1, 0.15)),
                turn @ Vector((0.0, 0.35, -0.95)), LOOSE, thumb=(0.3, 0.4, 0.3))


# --- The climb ---
STEP = 0.56       # Each hand moves down (or up) the rail once per step, the other half a step later.
CLIMB = 1.1       # Metres a second.
TOP = 1.42        # The eye over the hole, feet on the top rung.
BOTTOM = EYE - DEPTH


def climbing(t, going_down, start):
    """The eye on the ladder, `t` into a climb begun at `start`: height, and each
    hand's grip on its rail, held a step then moved on."""
    length = (TOP - BOTTOM) / CLIMB
    s = min(max((t - start) / length, 0.0), 1.0)
    eye_z = TOP + (BOTTOM - TOP) * smooth(s) if going_down else BOTTOM + (TOP - BOTTOM) * smooth(s)
    hands = {}
    for side, offset in (("r", 0.0), ("l", STEP * 0.5)):
        k = math.floor((t - start - offset) / STEP)
        into = (t - start - offset) - k * STEP

        def grip(n):
            at = start + offset + n * STEP
            ss = min(max((at - start) / length, 0.0), 1.0)
            z = TOP + (BOTTOM - TOP) * smooth(ss) if going_down else BOTTOM + (TOP - BOTTOM) * smooth(ss)
            return min(z + (0.08 if going_down else 0.18), RAIL_TOP - 0.08)
        here, there = on_rail(side, grip(k)), on_rail(side, grip(k + 1))
        move = 0.2
        hands[side] = mix_hand(here, there, ease_out(max(into - (STEP - move), 0.0) / move)) if into > STEP - move \
            else here
    return eye_z, hands, length


def descend(t, start, v_from):
    """From standing at the hole's near edge (`v_from`), hands to the rails, a step
    in over the hole, down the ladder, off it, and turned to the room."""
    climb_from = start + 0.6
    _, hands, length = climbing(t, True, climb_from)
    bottom_at = climb_from + length
    keys = [(0.0, v_from), (start, v_from),
            (start + 0.3, Viewer((0.0, -0.45, EYE - 0.12), pitch=-35.0), smooth),
            (climb_from, Viewer((0.0, -0.12, TOP), pitch=-22.0), smooth)]
    if t <= climb_from:
        v = track(keys, t, mix_viewer)
    elif t <= bottom_at:
        eye_z, _, _ = climbing(t, True, climb_from)
        v = Viewer((0.0, -0.12, eye_z), pitch=-10.0 - 6.0 * math.sin((t - climb_from) / STEP * math.pi) ** 2)
    else:
        v = track([(bottom_at, Viewer((0.0, -0.12, BOTTOM), pitch=-14.0)),
                   (bottom_at + 0.35, Viewer((0.0, -0.42, BOTTOM - 0.03), pitch=-6.0), smooth),
                   (bottom_at + 1.0, Viewer((0.0, -0.6, BOTTOM), yaw=180.0, pitch=-4.0), smooth)], t, mix_viewer)
    # Hands: to the rails' tops as the step in starts, on them down the ladder,
    # off them at the bottom.
    grab = {"r": on_rail("r", RAIL_TOP - 0.15), "l": on_rail("l", RAIL_TOP - 0.15)}
    out = {}
    for side in ("r", "l"):
        away = from_eye(v, side)
        if t < start:
            out[side] = away
        elif t < start + 0.3:
            out[side] = mix_hand(away, grab[side], ease_out((t - start) / 0.3))
        elif t < climb_from:
            out[side] = grab[side]
        elif t < bottom_at:
            out[side] = hands[side]
        else:
            out[side] = mix_hand(hands[side], away, ease_in(min((t - bottom_at) / 0.35, 1.0)))
    return v, out, bottom_at + 1.0


STAND = Viewer((0.0, -1.05, EYE), pitch=-42.0)
KNEEL = Viewer((0.0, -0.88, 0.98), pitch=-50.0)
ROOM = Viewer((0.0, -0.6, BOTTOM), yaw=180.0, pitch=-4.0)
OUT = Viewer((0.0, 0.95, EYE), pitch=-8.0)


def hatch_down(t):
    """Kneel, heave the lid open by its handle (the other hand braced on the
    ground), stand, and climb down."""
    opened = track([(0.0, 0.0), (0.95, 0.0), (1.55, 80.0, ease_in), (1.85, LID_OPEN, ease_out)],
                   t, lambda a, b, s: a + (b - a) * s)
    start = 2.35
    v, hands, _ = descend(t, start, Viewer((0.0, -0.82, EYE - 0.05), pitch=-40.0))
    if t < 2.1:
        v = track([(0.0, STAND), (0.5, KNEEL, smooth), (1.5, Viewer((0.02, -0.9, 1.05), pitch=-30.0), smooth),
                   (2.1, Viewer((0.0, -0.82, EYE - 0.05), pitch=-40.0), smooth)], t, mix_viewer)
        away_r, away_l = from_eye(v, "r"), from_eye(v, "l")
        if t < 0.55:
            right = away_r
        elif t < 0.9:
            right = mix_hand(away_r, on_handle(opened), ease_out((t - 0.55) / 0.35))
        elif t < 1.5:
            right = on_handle(opened)
        else:
            right = mix_hand(on_handle(min(opened, 80.0)), away_r, ease_in(min((t - 1.5) / 0.4, 1.0)))
        if t < 0.4:
            left = away_l
        elif t < 0.65:
            left = mix_hand(away_l, braced(), ease_out((t - 0.4) / 0.25))
        elif t < 1.55:
            left = braced()
        else:
            left = mix_hand(braced(), away_l, ease_in(min((t - 1.55) / 0.4, 1.0)))
        hands = {"r": right, "l": left}
    return Pose(v, hands["r"], hands["l"], parts={"lid": opened}, shoulders={"l": 25.0, "r": 20.0},
                poles={"r": Vector((0.6, -0.05, -0.6)), "l": Vector((-0.6, -0.05, -0.6))})


def ladder_down(t):
    v, hands, _ = descend(t, 0.0, Viewer((0.0, -0.82, EYE - 0.05), pitch=-40.0))
    return Pose(v, hands["r"], hands["l"], parts={"lid": LID_OPEN}, shoulders={"l": 25.0, "r": 20.0},
                poles={"r": Vector((0.6, -0.05, -0.6)), "l": Vector((-0.6, -0.05, -0.6))})


def ladder_up(t):
    """From the bunker floor facing the ladder: hands to the rails, up hand over
    hand, out over the far edge and stood on the ground."""
    climb_from = 0.7
    eye_z, hands, length = climbing(t, False, climb_from)
    top_at = climb_from + length
    at_ladder = Viewer((0.0, -0.12, BOTTOM), pitch=10.0)
    if t <= climb_from:
        v = track([(0.0, Viewer((0.0, -0.6, BOTTOM), pitch=4.0)), (climb_from, at_ladder, smooth)], t, mix_viewer)
    elif t <= top_at:
        v = Viewer((0.0, -0.12, eye_z), pitch=8.0 - 10.0 * min((t - climb_from) / length, 1.0))
    else:
        v = track([(top_at, Viewer((0.0, -0.12, TOP), pitch=-2.0)),
                   (top_at + 0.45, Viewer((0.0, 0.45, EYE + 0.05), pitch=-25.0), smooth),   # Over the edge.
                   (top_at + 0.9, OUT, smooth)], t, mix_viewer)
    out = {}
    for side in ("r", "l"):
        away = from_eye(v, side)
        first = on_rail(side, BOTTOM + 0.15)
        if t < 0.35:
            out[side] = away
        elif t < climb_from:
            out[side] = mix_hand(away, first, ease_out((t - 0.35) / (climb_from - 0.35)))
        elif t < top_at:
            out[side] = hands[side]
        elif t < top_at + 0.45:
            out[side] = on_rail(side, RAIL_TOP - 0.12)
        else:
            out[side] = mix_hand(on_rail(side, RAIL_TOP - 0.12), away, ease_in(min((t - top_at - 0.45) / 0.4, 1.0)))
    return Pose(v, out["r"], out["l"], parts={"lid": LID_OPEN}, shoulders={"l": 25.0, "r": 20.0},
                poles={"r": Vector((0.6, -0.05, -0.6)), "l": Vector((-0.6, -0.05, -0.6))})


def still(viewer, opened):
    def pose(t):
        del t
        return Pose(viewer, from_eye(viewer, "r"), from_eye(viewer, "l"), parts={"lid": opened})
    return pose


_DOWN_LENGTH = descend(0.0, 2.35, STAND)[2]
_LADDER_DOWN_LENGTH = descend(0.0, 0.0, STAND)[2]
_UP_LENGTH = 0.7 + (TOP - BOTTOM) / CLIMB + 0.9

CLIPS = [
    ("closed", 0.2, still(STAND, 0.0), True, []),
    ("opened", 0.2, still(ROOM, LID_OPEN), True, []),
    ("hatch_down", _DOWN_LENGTH, hatch_down, False,
     [(0.55, "kneel"), (0.95, "lid"), (1.85, "lid_down"), (2.6, "grab")] +
     [(2.95 + i * STEP, "rung") for i in range(int((TOP - BOTTOM) / CLIMB / STEP))]),
    ("ladder_down", _LADDER_DOWN_LENGTH, ladder_down, False,
     [(0.25, "grab")] + [(0.6 + i * STEP, "rung") for i in range(int((TOP - BOTTOM) / CLIMB / STEP))]),
    ("ladder_up", _UP_LENGTH, ladder_up, False,
     [(0.6, "grab")] + [(0.7 + i * STEP, "rung") for i in range(int((TOP - BOTTOM) / CLIMB / STEP))] +
     [(_UP_LENGTH - 0.5, "step")]),
]

HIP = STAND
AIM = STAND
GRIP = on_handle(0.0)
SUPPORT = braced()
POLES = {"r": Vector((0.5, -0.1, -0.4)), "l": Vector((-0.5, -0.1, -0.4))}


def held(place, **kw):
    return Pose(place, GRIP, SUPPORT, **kw)


PREVIEW = {"hatch_down": (0.8, 1.3, 1.9, 2.6, 3.3, 4.5), "ladder_up": (1.5, 3.2, 3.9, 4.3)}
