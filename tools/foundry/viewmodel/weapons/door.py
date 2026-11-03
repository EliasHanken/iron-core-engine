"""An interaction, not a gun: a steel door the player opens and walks through.

Built by the same pipeline as the guns. The door is modelled here (a steel frame,
a painted leaf on a hinge bone, a lever handle on its own bone), takes the gun's
place on the rig, and the hands take hold of its real surface the same way: the
right hand closes round the lever, the left palm lies flat on the leaf to push.

The clip moves the camera through the doorway. On the rig the eye stays put, so
the door is animated moving round the eye instead (Viewer below); the game turns
that back into the camera's path from where the door stands in the world:
camera = door's world place x inverse(the door's pose in the clip).

Space: the door's own, +Y into the room, +Z up, x to the right facing in; the
origin on the floor in the middle of the doorway.
"""
import math

import bmesh
import bpy
from mathutils import Matrix, Vector

import rig
from clips import Hand, Pose, ease_in, ease_out, mix_hand, mix_float, smooth, track
from scope import flat_material

NAME = "door"
PARTS = ["DOOR", "LEVER"]
MAGAZINE = None

WIDTH = 0.9
HEIGHT = 2.05
THICK = 0.05
HINGE = Vector((-WIDTH * 0.5, -THICK * 0.5, 0.0))
LEVER_PIVOT = Vector((0.36, -THICK - 0.012, 1.0))
OPEN_DEGREES = 96.0
EYE = 1.62  # The game's eye height.


# --- The model ---

def _box(bm, lo, hi, material, group, groups):
    verts = [bm.verts.new(Vector((x, y, z))) for x in (lo.x, hi.x) for y in (lo.y, hi.y) for z in (lo.z, hi.z)]
    for quad in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        bm.faces.new([verts[i] for i in quad]).material_index = material
    groups.extend((v, group) for v in verts)


def load(pack):
    """The door's armature (FRAME, the leaf's DOOR on its hinge, the LEVER on the
    leaf) and its mesh, bound to it. `pack` is unused: nothing is imported."""
    del pack
    data = bpy.data.armatures.new("door_rig")
    skeleton = bpy.data.objects.new("door_rig", data)
    bpy.context.scene.collection.objects.link(skeleton)
    rig.select_only(skeleton)
    bpy.ops.object.mode_set(mode="EDIT")
    frame = data.edit_bones.new("FRAME")
    frame.head, frame.tail = Vector(), Vector((0.0, 0.1, 0.0))
    door = data.edit_bones.new("DOOR")
    door.head, door.tail = HINGE, HINGE + Vector((0.0, 0.0, 0.3))
    door.parent = frame
    lever = data.edit_bones.new("LEVER")
    lever.head, lever.tail = LEVER_PIVOT, LEVER_PIVOT + Vector((0.0, -0.05, 0.0))
    lever.parent = door
    bpy.ops.object.mode_set(mode="OBJECT")

    bm = bmesh.new()
    groups = []
    # 0 the leaf's paint, 1 the frame's steel, 2 the lever's bright metal.
    w, h, t = WIDTH, HEIGHT, THICK
    for x0, x1 in ((-w * 0.5 - 0.1, -w * 0.5), (w * 0.5, w * 0.5 + 0.1)):
        _box(bm, Vector((x0, -0.09, 0.0)), Vector((x1, 0.04, h + 0.1)), 1, "FRAME", groups)
    _box(bm, Vector((-w * 0.5, -0.09, h)), Vector((w * 0.5, 0.04, h + 0.1)), 1, "FRAME", groups)
    # The leaf, with a raised kick plate and a panel line across it.
    _box(bm, Vector((-w * 0.5 + 0.005, -t, 0.01)), Vector((w * 0.5 - 0.005, 0.0, h - 0.005)), 0, "DOOR", groups)
    _box(bm, Vector((-w * 0.5 + 0.04, -t - 0.004, 0.04)), Vector((w * 0.5 - 0.04, -t, 0.28)), 1, "DOOR", groups)
    _box(bm, Vector((-w * 0.5 + 0.04, -t - 0.003, 1.48)), Vector((w * 0.5 - 0.04, -t, 1.5)), 1, "DOOR", groups)
    # The lever, both sides: an escutcheon plate, a spindle, the handle pointing to
    # the hinge.
    for side in (-1.0, 1.0):
        face = -t if side < 0 else 0.0
        y0 = face + side * 0.008
        _box(bm, Vector((LEVER_PIVOT.x - 0.022, min(face, y0), LEVER_PIVOT.z - 0.09)),
             Vector((LEVER_PIVOT.x + 0.022, max(face, y0), LEVER_PIVOT.z + 0.07)), 2, "DOOR", groups)
        y1 = face + side * 0.032
        _box(bm, Vector((LEVER_PIVOT.x - 0.009, min(y0, y1), LEVER_PIVOT.z - 0.009)),
             Vector((LEVER_PIVOT.x + 0.009, max(y0, y1), LEVER_PIVOT.z + 0.009)), 2, "LEVER", groups)
        y2 = face + side * 0.048
        _box(bm, Vector((LEVER_PIVOT.x - 0.13, min(y1, y2), LEVER_PIVOT.z - 0.011)),
             Vector((LEVER_PIVOT.x + 0.012, max(y1, y2), LEVER_PIVOT.z + 0.011)), 2, "LEVER", groups)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    mesh = bpy.data.meshes.new("door")
    bm.to_mesh(mesh)
    index = {v: i for i, v in enumerate(bm.verts)}
    bm.free()
    gun = bpy.data.objects.new("door", mesh)
    bpy.context.scene.collection.objects.link(gun)
    for name in ("FRAME", "DOOR", "LEVER"):
        gun.vertex_groups.new(name=name)
    for v, name in groups:
        gun.vertex_groups[name].add([index[v]], 1.0, "REPLACE")
    gun.modifiers.new("Armature", "ARMATURE").object = skeleton
    mesh.materials.append(flat_material("door_paint", (0.13, 0.2, 0.17), 0.25, 0.55))
    mesh.materials.append(flat_material("door_steel", (0.17, 0.17, 0.18), 0.8, 0.45))
    mesh.materials.append(flat_material("door_lever", (0.55, 0.55, 0.57), 1.0, 0.28))
    return skeleton, gun


def leaf(opened):
    """The leaf's move from closed, opened by `opened` degrees about its hinge."""
    return (Matrix.Translation(HINGE) @ Matrix.Rotation(math.radians(opened), 4, "Z") @
            Matrix.Translation(-HINGE))


def lever(pressed):
    """The lever's move on the leaf, pressed down by `pressed` (0..1)."""
    return (Matrix.Translation(LEVER_PIVOT) @ Matrix.Rotation(math.radians(-42.0 * pressed), 4, "Y") @
            Matrix.Translation(-LEVER_PIVOT))


def move_parts(pose, weapon, p):
    opened, pressed = p.parts.get("open", 0.0), p.parts.get("lever", 0.0)
    door = pose["DOOR"]
    door.matrix = weapon @ leaf(opened) @ door.bone.matrix_local
    handle = pose["LEVER"]
    handle.matrix = weapon @ leaf(opened) @ lever(pressed) @ handle.bone.matrix_local


# --- The camera, as the door moving round a fixed eye ---

class Viewer:
    """Where the eye is in the door's space (position, then yaw left about +Z,
    pitch up, roll right, in degrees); as a place for the rig, the door in the
    eye's space, which is its inverse."""

    def __init__(self, at, yaw=0.0, pitch=0.0, roll=0.0):
        self.at, self.yaw, self.pitch, self.roll = Vector(at), yaw, pitch, roll

    def camera(self):
        return (Matrix.Translation(self.at) @ Matrix.Rotation(math.radians(self.yaw), 4, "Z") @
                Matrix.Rotation(math.radians(self.pitch), 4, "X") @ Matrix.Rotation(math.radians(self.roll), 4, "Y"))

    def matrix(self):
        return self.camera().inverted()


def mix_viewer(a, b, s):
    return Viewer(a.at.lerp(b.at, s), mix_float(a.yaw, b.yaw, s), mix_float(a.pitch, b.pitch, s),
                  mix_float(a.roll, b.roll, s))


def lin(a, b, s):
    return a + (b - a) * s


# --- Hands ---
HOLD = {"index": (0.9, 1.1, 0.8), "middle": (1.0, 1.15, 0.8), "ring": (1.05, 1.15, 0.8), "pinky": (1.1, 1.1, 0.8)}
FLAT = {k: (0.12, 0.18, 0.1) for k in ("index", "middle", "ring", "pinky")}
LOOSE = {k: (0.4, 0.5, 0.3) for k in ("index", "middle", "ring", "pinky")}


def on_lever(opened, pressed):
    """The right hand over the lever's handle, palm down, fingers round it."""
    m = leaf(opened) @ lever(pressed)
    turn = m.to_3x3()
    return Hand(m @ Vector((0.28, -THICK - 0.04, 1.03)), turn @ Vector((0.0, 0.25, -1.0)),
                turn @ Vector((0.15, 1.0, -0.1)), HOLD, thumb=(0.5, 0.5, 0.35), contact=1.0, spin=20.0,
                yaw=25.0)


def on_leaf(opened, height=1.3):
    """The left palm flat on the leaf's face, fingers up, pushing."""
    m = leaf(opened)
    turn = m.to_3x3()
    return Hand(m @ Vector((0.0, -THICK - 0.03, 1.15)), turn @ Vector((0.0, 1.0, 0.0)),
                turn @ Vector((-0.35, 0.0, 0.94)), FLAT, thumb=(0.15, 0.2, 0.1), contact=1.0, spin=15.0, yaw=20.0)


def from_eye(viewer, socket, palm, forward, curl=LOOSE):
    """A hand placed relative to the eye (down out of view, say), in the door's space."""
    m = viewer.camera()
    turn = m.to_3x3()
    return Hand(m @ Vector(socket), turn @ Vector(palm), turn @ Vector(forward), curl, thumb=(0.3, 0.4, 0.3))


START = Viewer((0.18, -0.86, EYE), yaw=8.0, pitch=-24.0)
END = Viewer((0.0, 1.15, EYE), yaw=-6.0, pitch=-2.0)
POLES = {"r": Vector((0.45, 0.1, -0.55)), "l": Vector((-0.35, 0.25, -0.6))}


def viewer_at(t):
    """The eye's path: a step up to the door, a lean into the push, through it."""
    keys = [(0.0, START),
            (0.6, Viewer((0.18, -0.8, EYE - 0.01), yaw=10.0, pitch=-30.0), smooth),
            (1.2, Viewer((0.12, -0.66, EYE - 0.02), yaw=5.0, pitch=-14.0, roll=-1.5), smooth),
            (1.9, Viewer((0.03, -0.25, EYE - 0.015), yaw=-2.0, pitch=-6.0, roll=1.0), smooth),
            (2.5, Viewer((0.0, 0.35, EYE - 0.025), yaw=-8.0, pitch=-3.0, roll=-1.0), smooth),
            (3.3, END, smooth)]
    v = track(keys, t, mix_viewer)
    # Each step dips the head a little.
    if 1.2 < t < 3.3:
        v.at.z -= 0.012 * abs(math.sin((t - 1.2) / 0.45 * math.pi))
    return v


def open_door(t):
    """Reach for the lever, press it, push the door wide and walk through."""
    v = viewer_at(t)
    pressed = track([(0.0, 0.0), (0.5, 0.0), (0.68, 1.0, ease_out), (1.25, 1.0), (1.4, 0.0, ease_in)], t, lin)
    opened = track([(0.0, 0.0), (0.72, 0.0), (1.2, 22.0, ease_in), (2.2, OPEN_DEGREES, ease_out)], t, lin)
    away_r = from_eye(v, (0.3, 0.12, -0.62), (-1.0, 0.1, 0.15), (0.0, 0.35, -0.95))
    away_l = from_eye(v, (-0.32, 0.1, -0.62), (-1.0, 0.1, 0.15), (0.0, 0.35, -0.95))
    if t < 0.15:
        right = away_r
    elif t < 0.48:
        right = mix_hand(away_r, on_lever(opened, pressed), ease_out((t - 0.15) / 0.33))
    elif t < 1.3:
        right = on_lever(opened, pressed)
    elif t < 1.75:
        right = mix_hand(on_lever(opened, 0.0), away_r, ease_in((t - 1.3) / 0.45))
    else:
        right = away_r
    if t < 0.75:
        left = away_l
    elif t < 1.05:
        left = mix_hand(away_l, on_leaf(opened), ease_out((t - 0.75) / 0.3))
    elif t < 1.6:
        left = on_leaf(opened)
    elif t < 2.0:
        left = mix_hand(on_leaf(opened), away_l, ease_in((t - 1.6) / 0.4))
    else:
        left = away_l
    return Pose(v, right, left, parts={"open": opened, "lever": pressed},
                poles={"r": Vector((0.45, 0.0, -0.4)), "l": Vector((-0.45, 0.1, -0.45))})


def closed(t):
    return Pose(START, from_eye(START, (0.3, 0.12, -0.62), (-1.0, 0.1, 0.15), (0.0, 0.35, -0.95)),
                from_eye(START, (-0.32, 0.1, -0.62), (-1.0, 0.1, 0.15), (0.0, 0.35, -0.95)))


def opened_still(t):
    return Pose(END, from_eye(END, (0.3, 0.12, -0.62), (-1.0, 0.1, 0.15), (0.0, 0.35, -0.95)),
                from_eye(END, (-0.32, 0.1, -0.62), (-1.0, 0.1, 0.15), (0.0, 0.35, -0.95)),
                parts={"open": OPEN_DEGREES})


CLIPS = [
    ("closed", 0.2, closed, True, []),
    ("open", 3.3, open_door, False, [(0.3, "reach"), (0.55, "lever"), (0.8, "push"), (2.0, "swing"),
                                     (1.6, "step"), (2.05, "step"), (2.5, "step"), (2.95, "step")]),
    ("opened", 0.2, opened_still, True, []),
]

# For the driver's previews and reports.
HIP = START
AIM = START
GRIP = on_lever(0.0, 0.0)
SUPPORT = on_leaf(0.0)


def held(place, **kw):
    return Pose(place, GRIP, SUPPORT, **kw)


PREVIEW = {"open": (0.5, 0.7, 0.85, 1.2, 1.55, 1.85, 2.2, 2.8)}
EYE_FOV = 60.0  # Drawn in the world, at the world's field of view.
