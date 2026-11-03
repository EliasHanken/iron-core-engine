"""Mantling, built like the door (door.py): the prop is a ledge, modelled here only
so the hands have its top to land on (the game plays the clip against real
geometry and doesn't draw it). The eye climbs onto it: both palms planted on
the edge, the body pushed up over them, a knee's lean forward, and stood on top.

mantle_low.py and mantle_high.py build it for a waist-high ledge (pushed up onto)
and a head-high one (reached for, pulled up over). The game picks by the real
ledge's height and lines the camera up for the difference.

Space: the ledge's own, +Y over it, +Z up, the origin on the top edge in front
of the player; the ledge's top at z 0, its face at y 0.
"""
import math

import bmesh
import bpy
from mathutils import Matrix, Vector

import rig
from clips import Hand, Pose, ease_in, ease_out, mix_hand, smooth, track
from weapons.door import Viewer, mix_viewer
from scope import flat_material

EYE = 1.62
PARTS = []
MAGAZINE = None
EYE_FOV = 60.0


def make_ledge(height, depth=0.7):
    """The ledge as a prop: a block `height` high and `depth` deep, its top edge at
    the origin, bound to one LEDGE bone. Only for the hands to land on in Blender."""
    data = bpy.data.armatures.new("ledge_rig")
    skeleton = bpy.data.objects.new("ledge_rig", data)
    bpy.context.scene.collection.objects.link(skeleton)
    rig.select_only(skeleton)
    bpy.ops.object.mode_set(mode="EDIT")
    bone = data.edit_bones.new("LEDGE")
    bone.head, bone.tail = Vector(), Vector((0.0, 0.1, 0.0))
    bpy.ops.object.mode_set(mode="OBJECT")
    bm = bmesh.new()
    lo, hi = Vector((-0.9, 0.0, -height)), Vector((0.9, depth, 0.0))
    verts = [bm.verts.new(Vector((x, y, z))) for x in (lo.x, hi.x) for y in (lo.y, hi.y) for z in (lo.z, hi.z)]
    for quad in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        bm.faces.new([verts[i] for i in quad])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    mesh = bpy.data.meshes.new("ledge")
    bm.to_mesh(mesh)
    bm.free()
    gun = bpy.data.objects.new("ledge", mesh)
    bpy.context.scene.collection.objects.link(gun)
    gun.vertex_groups.new(name="LEDGE").add(range(len(mesh.vertices)), 1.0, "REPLACE")
    gun.modifiers.new("Armature", "ARMATURE").object = skeleton
    mesh.materials.append(flat_material("ledge", (0.5, 0.5, 0.5), 0.0, 0.8))
    return skeleton, gun


def build(name, height):
    """The module's contents for a ledge `height` metres above the player's feet."""
    def load(pack):
        del pack
        return make_ledge(height)

    def move_parts(pose, weapon, p):
        del pose, weapon, p

    # Palms flat on the top just past the edge, fingers over it, a shoulder's width apart.
    PLANT = {k: (0.25, 0.35, 0.2) for k in ("index", "middle", "ring", "pinky")}

    def planted(side):
        x = 0.21 if side == "r" else -0.21
        return Hand(Vector((x, 0.09, 0.03)), Vector((0.0, 0.0, -1.0)), Vector((-0.15 * x / 0.21, 1.0, 0.0)),
                    PLANT, thumb=(0.25, 0.3, 0.2), contact=1.0, spin=20.0, yaw=25.0)

    def from_eye(viewer, side, low=True):
        x = 0.3 if side == "r" else -0.3
        m = viewer.camera()
        turn = m.to_3x3()
        socket = (x, 0.12, -0.62) if low else (x * 0.9, 0.35, 0.05)
        palm = (-1.0 if side == "r" else 1.0, 0.1, 0.15) if low else (0.0, 0.3, -1.0)
        forward = (0.0, 0.35, -0.95) if low else (0.0, 1.0, 0.25)
        return Hand(m @ Vector(socket), turn @ Vector(palm), turn @ Vector(forward),
                    {k: (0.4, 0.5, 0.3) for k in PLANT}, thumb=(0.3, 0.4, 0.3))

    high = height > 1.3
    start = Viewer((0.0, -0.5, EYE - height), pitch=-12.0 if not high else 18.0)
    end = Viewer((0.0, 0.42, EYE), pitch=-6.0)
    length = 0.8 if not high else 1.15
    plant_at = 0.16 if not high else 0.3

    def viewer_at(t):
        if not high:
            keys = [(0.0, start),
                    (plant_at, Viewer((0.0, -0.46, EYE - height + 0.02), pitch=-26.0), ease_out),
                    (0.42, Viewer((0.0, -0.22, 0.42), pitch=-38.0, roll=2.0), smooth),
                    (0.62, Viewer((0.0, 0.18, EYE - 0.25), pitch=-18.0, roll=-1.0), smooth),
                    (length, end, ease_out)]
        else:
            keys = [(0.0, start),
                    (plant_at, Viewer((0.0, -0.42, EYE - height + 0.05), pitch=22.0), ease_out),
                    (0.55, Viewer((0.0, -0.3, -0.05), pitch=-5.0, roll=-2.0), smooth),   # Hauled up.
                    (0.8, Viewer((0.0, -0.12, 0.45), pitch=-35.0, roll=2.0), smooth),   # Over the hands.
                    (0.98, Viewer((0.0, 0.2, EYE - 0.3), pitch=-16.0), smooth),
                    (length, end, ease_out)]
        return track(keys, t, mix_viewer)

    def mantle(t):
        v = viewer_at(t)
        hands = {}
        for side in ("r", "l"):
            reach = from_eye(v, side, low=not high)
            away = from_eye(v, side)
            on = planted(side)
            lead = 0.0 if side == "r" else 0.04  # The hands land a beat apart.
            land, lift = plant_at + lead, length * 0.72 + lead
            if t < land:
                hands[side] = mix_hand(away if not high else reach, on, ease_out(t / land))
            elif t < lift:
                hands[side] = on
            else:
                hands[side] = mix_hand(on, away, ease_in(min((t - lift) / (length - lift), 1.0)))
        return Pose(v, hands["r"], hands["l"], shoulders={"l": 20.0, "r": 20.0},
                    poles={"r": Vector((0.5, -0.2, -0.3)), "l": Vector((-0.5, -0.2, -0.3))})

    def standing(viewer):
        def still(t):
            del t
            return Pose(viewer, from_eye(viewer, "r"), from_eye(viewer, "l"))
        return still

    return {
        "NAME": name,
        "load": load,
        "move_parts": move_parts,
        "CLIPS": [("before", 0.2, standing(start), True, []),
                  ("mantle", length, mantle, False, [(plant_at, "plant"), (plant_at + 0.04, "plant"),
                                                     (length * 0.7, "step")]),
                  ("after", 0.2, standing(end), True, [])],
        "HIP": start, "AIM": start, "GRIP": planted("r"), "SUPPORT": planted("l"),
        "held": lambda place, **kw: Pose(place, planted("r"), planted("l"), **kw),
        "POLES": {"r": Vector((0.5, -0.2, -0.3)), "l": Vector((-0.5, -0.2, -0.3))},
        "PREVIEW": {"mantle": tuple(round(length * f, 2) for f in (0.2, 0.45, 0.65, 0.85))},
    }
