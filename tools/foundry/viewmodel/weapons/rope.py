"""An interaction: a heavy rope hanging from the hall's roof truss beside the
catwalk, climbed hand over hand up onto it. Built like the door (door.py): the
rope is modelled here, and the hands close round it, one above the other, then
plant on the catwalk's deck through a gap in its rail to step up onto it.

Space: the rope's own, +Y towards the catwalk (the way the player faces it),
+Z up, the origin where the rope meets the floor. The catwalk's deck is DECK up,
its edge EDGE beyond the rope.
"""
import math

import bmesh
import bpy
from mathutils import Vector

import rig
from clips import Hand, Pose, ease_in, ease_out, mix_hand, smooth, track
from scope import flat_material
from weapons.door import Viewer, mix_viewer

NAME = "rope"
PARTS = []
MAGAZINE = None
EYE_FOV = 60.0
EYE = 1.62

TOP = 13.0     # Where it's tied off, on the truss.
RADIUS = 0.022
DECK = 5.2     # The catwalk's deck, above the floor.
EDGE = 0.4     # From the rope to the deck's edge.
CLIMB = 1.0    # Metres a second.
STEP = 0.62    # Each hand reaches up once a step, the other half a step later.


def load(pack):
    del pack
    data = bpy.data.armatures.new("rope_rig")
    skeleton = bpy.data.objects.new("rope_rig", data)
    bpy.context.scene.collection.objects.link(skeleton)
    rig.select_only(skeleton)
    bpy.ops.object.mode_set(mode="EDIT")
    bone = data.edit_bones.new("ROPE")
    bone.head, bone.tail = Vector(), Vector((0.0, 0.0, 0.3))
    bpy.ops.object.mode_set(mode="OBJECT")

    # A rope: a twisted tube from the floor up to the truss.
    bm = bmesh.new()
    sides, rings = 10, 260
    verts = []
    for r in range(rings + 1):
        z = 0.08 + (TOP - 0.08) * r / rings
        ring = []
        for k in range(sides):
            a = 2.0 * math.pi * k / sides + z * 9.0  # The lay's twist.
            bulge = RADIUS * (1.0 + 0.12 * math.cos(3.0 * a))
            ring.append(bm.verts.new(Vector((bulge * math.cos(a), bulge * math.sin(a), z))))
        verts.append(ring)
    for r0, r1 in zip(verts, verts[1:]):
        for k in range(sides):
            k1 = (k + 1) % sides
            bm.faces.new((r0[k], r0[k1], r1[k1], r1[k]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    mesh = bpy.data.meshes.new("rope")
    bm.to_mesh(mesh)
    bm.free()
    gun = bpy.data.objects.new("rope", mesh)
    bpy.context.scene.collection.objects.link(gun)
    gun.vertex_groups.new(name="ROPE").add(range(len(mesh.vertices)), 1.0, "REPLACE")
    gun.modifiers.new("Armature", "ARMATURE").object = skeleton
    mesh.materials.append(flat_material("rope", (0.32, 0.25, 0.16), 0.0, 0.92))
    for p in mesh.polygons:
        p.use_smooth = True
    return skeleton, gun


def move_parts(pose, weapon, p):
    del pose, weapon, p


GRASP = {"index": (0.9, 1.1, 0.8), "middle": (1.0, 1.15, 0.8), "ring": (1.05, 1.15, 0.8), "pinky": (1.1, 1.1, 0.8)}
FLAT = {k: (0.25, 0.35, 0.2) for k in GRASP}
LOOSE = {k: (0.4, 0.5, 0.3) for k in GRASP}


def on_rope(side, z):
    """A hand round the rope at `z`, thumb up, the right from the right."""
    inward = -1.0 if side == "r" else 1.0
    return Hand(Vector((0.0, 0.0, z)), Vector((inward, 0.2, 0.0)), Vector((0.0, 1.0, 0.1)), GRASP,
                thumb=(0.5, 0.5, 0.35), contact=1.0, spin=25.0, yaw=25.0)


def on_deck(side):
    """A palm flat on the catwalk's deck (the level's own; it isn't modelled here)."""
    x = 0.28 + (0.24 if side == "r" else -0.2)  # To the rope's right, where the body goes over.
    return Hand(Vector((x, EDGE + 0.12, DECK + 0.025)), Vector((0.0, 0.0, -1.0)),
                Vector((-0.2 if side == "r" else 0.2, 1.0, 0.0)),
                FLAT, thumb=(0.25, 0.3, 0.2), spin=20.0, yaw=25.0)


def from_eye(viewer, side):
    x = 0.3 if side == "r" else -0.3
    m = viewer.camera()
    turn = m.to_3x3()
    return Hand(m @ Vector((x, 0.12, -0.62)), turn @ Vector((-1.0 if side == "r" else 1.0, 0.1, 0.15)),
                turn @ Vector((0.0, 0.35, -0.95)), LOOSE, thumb=(0.3, 0.4, 0.3))


START_Z = EYE
TOP_Z = DECK + EYE + 0.35   # High enough to get the feet over the deck.
CLIMB_FROM = 0.55
CLIMB_LENGTH = (TOP_Z - START_Z) / CLIMB
OVER_AT = CLIMB_FROM + CLIMB_LENGTH
LENGTH = OVER_AT + 1.3


def eye_at(t):
    s = min(max((t - CLIMB_FROM) / CLIMB_LENGTH, 0.0), 1.0)
    # Pulled up in surges, a pull a step.
    surge = 0.04 * math.sin((t - CLIMB_FROM) / STEP * 2.0 * math.pi) if 0.0 < s < 1.0 else 0.0
    return START_Z + (TOP_Z - START_Z) * smooth(s) + surge


def grip(side, n):
    """Where hand `side` takes the rope on its `n`th reach: above the head."""
    offset = 0.0 if side == "r" else STEP * 0.5
    at = CLIMB_FROM + offset + n * STEP
    return min(eye_at(at) + 0.28, TOP_Z + 0.25)


def rope_up(t):
    """Hands to the rope, up it hand over hand (it sways a little), then palms on
    the catwalk's deck and up onto it."""
    start = Viewer((0.0, -0.4, START_Z), pitch=12.0)
    if t <= OVER_AT:
        sway = math.sin(t * 1.7) * min(t / 2.0, 1.0)
        v = Viewer((0.02 * sway, -0.36, eye_at(t)), yaw=2.0 * sway, pitch=14.0 - 6.0 * min(t / 2.0, 1.0),
                   roll=1.5 * sway)
        if t < CLIMB_FROM:
            v = mix_viewer(start, v, smooth(t / CLIMB_FROM))
    else:
        top = Viewer((0.0, -0.36, TOP_Z), pitch=8.0)
        # Round the rope to its right, reaching over onto the deck.
        v = track([(OVER_AT, top), (OVER_AT + 0.45, Viewer((0.26, -0.18, TOP_Z + 0.1), yaw=-6.0, pitch=-35.0), smooth),
                   (OVER_AT + 0.9, Viewer((0.32, 0.45, DECK + EYE - 0.2), yaw=-4.0, pitch=-20.0), smooth),
                   (LENGTH, Viewer((0.3, EDGE + 0.6, DECK + EYE), pitch=-4.0), ease_out)], t, mix_viewer)
    hands = {}
    for side in ("r", "l"):
        offset = 0.0 if side == "r" else STEP * 0.5
        away = from_eye(v, side)
        if t < 0.15 + offset * 0.3:
            hands[side] = away
        elif t < CLIMB_FROM:
            hands[side] = mix_hand(away, on_rope(side, grip(side, 0)),
                                   ease_out((t - 0.15 - offset * 0.3) / (CLIMB_FROM - 0.15 - offset * 0.3)))
        elif t < OVER_AT:
            k = math.floor((t - CLIMB_FROM - offset) / STEP)
            into = (t - CLIMB_FROM - offset) - k * STEP
            here, there = on_rope(side, grip(side, max(k, 0))), on_rope(side, grip(side, max(k + 1, 0)))
            reach = 0.24
            hands[side] = (mix_hand(here, there, ease_out((into - (STEP - reach)) / reach)) if into > STEP - reach
                           else here)
        elif t < OVER_AT + 0.35:
            last = on_rope(side, grip(side, math.floor((OVER_AT - CLIMB_FROM - offset) / STEP)))
            hands[side] = mix_hand(last, on_deck(side), ease_out((t - OVER_AT) / 0.35))
        elif t < OVER_AT + 0.9:
            hands[side] = on_deck(side)
        else:
            hands[side] = mix_hand(on_deck(side), away, ease_in(min((t - OVER_AT - 0.9) / 0.35, 1.0)))
    return Pose(v, hands["r"], hands["l"], shoulders={"l": 25.0, "r": 25.0},
                poles={"r": Vector((0.6, -0.1, -0.5)), "l": Vector((-0.6, -0.1, -0.5))})


def standing(viewer):
    def still(t):
        del t
        return Pose(viewer, from_eye(viewer, "r"), from_eye(viewer, "l"))
    return still


START = Viewer((0.0, -0.4, START_Z), pitch=12.0)
CLIPS = [
    ("hanging", 0.2, standing(START), True, []),
    ("rope_up", LENGTH, rope_up, False,
     [(0.4, "grab")] + [(CLIMB_FROM + i * STEP * 0.5, "pull") for i in range(int(CLIMB_LENGTH / STEP * 2))] +
     [(OVER_AT + 0.35, "plant"), (LENGTH - 0.3, "step")]),
]

HIP = START
AIM = START
GRIP = on_rope("r", 2.0)
SUPPORT = on_rope("l", 1.7)
POLES = {"r": Vector((0.6, -0.1, -0.5)), "l": Vector((-0.6, -0.1, -0.5))}


def held(place, **kw):
    return Pose(place, GRIP, SUPPORT, **kw)


PREVIEW = {"rope_up": (0.5, 2.0, 4.0, round(OVER_AT + 0.4, 2), round(OVER_AT + 1.0, 2))}
