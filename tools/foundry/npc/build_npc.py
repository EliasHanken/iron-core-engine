"""Builds Foundry's soldier: a Microsoft Rocketbox avatar (MIT) with its motion
captured walks, runs and idles, carrying Stein Games' AK-47 (CC0), exported as
one skinned glTF.

    blender -b --factory-startup --python build_npc.py -- <rocketbox> <avatar> <WeaponsPack> <out.gltf>
        [--preview <prefix>]

<rocketbox> holds <avatar>/<avatar>.fbx with its Textures/ (from Rocketbox's
Assets/Avatars/Professions/<avatar>/Export and Textures), and anim/*.fbx
(Rocketbox's motion: walks, runs, idles). The mocap drives the body;
the rifle hangs off the upper chest on a `weapon` bone and the arms are solved
onto it (two-bone IK to targets on its grip and handguard, the hands turned to
hold it), so it sways and bobs with the walk the way a carried rifle does. Each
clip is baked through that rig, its root motion taken out (the game moves the
soldier; the walk's speed is written beside the glTF for foot-sync), and the
fingers closed round the rifle.

Space (in Blender): metres, the soldier stands on the origin facing +Y, +Z up;
the glTF exporter turns that into the engine's (+Y up, facing -Z).
"""
import json
import math
import os
import sys

import bpy
from mathutils import Matrix, Quaternion, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "viewmodel"))
sys.path.insert(0, os.path.dirname(HERE))
import rig  # noqa: E402

args = sys.argv[sys.argv.index("--") + 1:]
rocketbox, avatar, pack, out_path = args[:4]
preview = args[args.index("--preview") + 1] if "--preview" in args else None
FPS = 30

# The clips: name, Rocketbox source, carry ("ready" low, "aim" shouldered), loop.
CLIPS = [
    ("idle", "m_idle_neutral_01", "ready", True),
    ("look", "m_idle_look_around_01", "ready", True),
    ("walk", "m_walk_neutral_01", "ready", True),
    ("walk_fast", "m_walk_fast_01", "ready", True),
    ("run", "m_run_neutral_01", "ready", True),
    ("aim", "m_idle_neutral_01", "aim", True),
    ("aim_walk", "m_walk_neutral_01", "aim", True),
    ("crouch_aim", "m_crouch_idle", "aim", True),
]


# --- Helpers ---

def fcurves(action):
    """Every F-curve in an action (layered actions keep them in channel bags)."""
    if hasattr(action, "layers") and action.layers:
        for layer in action.layers:
            for strip in layer.strips:
                for bag in strip.channelbags:
                    yield from bag.fcurves
    elif hasattr(action, "fcurves"):
        yield from action.fcurves


def remove_fcurves(action, unwanted):
    """Removes the F-curves `unwanted(fcurve)` picks out."""
    if hasattr(action, "layers") and action.layers:
        for layer in action.layers:
            for strip in layer.strips:
                for bag in strip.channelbags:
                    for fc in [f for f in bag.fcurves if unwanted(f)]:
                        bag.fcurves.remove(fc)
    else:
        for fc in [f for f in action.fcurves if unwanted(f)]:
            action.fcurves.remove(fc)


def import_fbx(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path, use_anim=True)
    return [o for o in bpy.data.objects if o not in before]


# --- The soldier, upright in metres, facing +Y ---
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.scene.render.fps = FPS
avatar_dir = os.path.join(rocketbox, avatar)
new = import_fbx(os.path.join(avatar_dir, avatar + ".fbx"))
arm = next(o for o in new if o.type == "ARMATURE")
body = next(o for o in new if o.type == "MESH")
for o in new:
    if o not in (arm, body):
        bpy.data.objects.remove(o, do_unlink=True)
if arm.animation_data:
    arm.animation_data.action = None
SCALE = arm.scale.x  # The FBX is in centimetres: its location keys scale with the skeleton.
# Upright with the face to +Y: the avatar faces -Y as imported.
arm.rotation_euler = (0.0, 0.0, arm.rotation_euler.z + math.pi)
rig.select_only(arm, body)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
bones = arm.data.bones
REST = {b.name: b.matrix_local.copy() for b in bones}


# --- The clips: each Rocketbox motion moved onto the skeleton, in place ---
SPEEDS = {}


def load_motion(source):
    """The motion's action on our skeleton, in place: Rocketbox keeps a motion's
    travel (and its hips' bob and sway) on the armature object, so the pelvis's
    path is sampled in the world on the motion's own armature, the straight-line
    travel taken out (its speed kept), and the rest written onto our pelvis."""
    new = import_fbx(os.path.join(rocketbox, "anim", source + ".fbx"))
    other = next(o for o in new if o.type == "ARMATURE")
    action = other.animation_data.action
    action.name = source
    start, end = (int(action.frame_range[0]), int(action.frame_range[1]))
    scene = bpy.context.scene
    path = {}
    for f in range(start, end + 1):
        scene.frame_set(f)
        path[f] = other.matrix_world @ other.pose.bones["Bip01 Pelvis"].head
    rest_world = other.matrix_world @ other.data.bones["Bip01 Pelvis"].head_local
    travel = path[end] - path[start]
    travel.z = 0.0
    SPEEDS[source] = travel.length / max((end - start) / FPS, 1e-3)
    for o in new:
        bpy.data.objects.remove(o, do_unlink=True)
    # Its object channels go; the bones' locations to metres.
    remove_fcurves(action, lambda fc: not fc.data_path.startswith("pose.bones"))
    for fc in list(fcurves(action)):
        if fc.data_path.endswith(".location"):
            for k in fc.keyframe_points:
                k.co.y *= SCALE
                k.handle_left.y *= SCALE
                k.handle_right.y *= SCALE
    remove_fcurves(action, lambda fc: fc.data_path == 'pose.bones["Bip01 Pelvis"].location')
    arm.animation_data_create()
    arm.animation_data.action = action
    if hasattr(arm.animation_data, "action_slot") and action.slots:
        arm.animation_data.action_slot = action.slots[0]
    # The pelvis's offset from its rest, in our skeleton's space (turned half round
    # from the motion's world), in the pelvis bone's own frame.
    turn = Matrix.Rotation(math.pi, 3, "Z")
    to_local = REST["Bip01 Pelvis"].to_3x3().inverted()
    pelvis = arm.pose.bones["Bip01 Pelvis"]
    for f in range(start, end + 1):
        s_ = (f - start) / max(end - start, 1)
        offset = turn @ (path[f] - travel * s_ - rest_world)
        pelvis.location = to_local @ offset
        pelvis.keyframe_insert("location", frame=f)
    return action, start, end


# --- The rifle, on a bone riding the upper chest ---
skeleton, rifle = rig.import_rifle(os.path.join(pack, "AK47", "SKM_AK47.fbx"))
# Shouldered: the sights' line (0.185 above the grip's foot) at the right eye,
# the stock in the shoulder's pocket.
eye = (arm.matrix_world @ bones["Bip01 REye"].head_local)
AIM = Matrix.Translation(Vector((eye.x + 0.005, 0.24, eye.z - 0.205)))
rifle_root = next(b.name for b in skeleton.data.bones if b.parent is None)
rig.select_only(arm)
bpy.ops.object.mode_set(mode="EDIT")
eb = arm.data.edit_bones
weapon = eb.new("weapon")
weapon.head = AIM.translation
weapon.tail = AIM.translation + Vector((0.0, 0.1, 0.0))
weapon.roll = 0.0
weapon.parent = eb["Bip01 Spine2"]
weapon.use_deform = True
bpy.ops.object.mode_set(mode="OBJECT")
# Move the rifle's skeleton and mesh to the shouldered place, then join it in.
for o in (skeleton, rifle):
    o.matrix_world = AIM @ o.matrix_world
rig.apply_transform(skeleton, rifle)
rig.select_only(arm, skeleton)
bpy.ops.object.join()
rifle.parent = arm
for m in rifle.modifiers:
    if m.type == "ARMATURE":
        m.object = arm
rig.select_only(arm)
bpy.ops.object.mode_set(mode="EDIT")
arm.data.edit_bones[rifle_root].parent = arm.data.edit_bones["weapon"]
bpy.ops.object.mode_set(mode="OBJECT")


# --- The hands on it: IK targets on the rifle, the hands turned to hold it ---
def hand_rest(side):
    """The hand at rest: wrist, palm centre, fingers' direction, which way the
    palm faces, and the hand bone's rest rotation (armature space)."""
    s = "R" if side == "r" else "L"
    b = arm.data.bones
    wrist = b[f"Bip01 {s} Hand"].head_local.copy()
    knuckles = b[f"Bip01 {s} Finger2"].head_local.copy()
    forward = (knuckles - wrist).normalized()
    across = (b[f"Bip01 {s} Finger1"].head_local - b[f"Bip01 {s} Finger4"].head_local).normalized()
    normal = forward.cross(across).normalized()
    bend = 0.0
    for f in "1234":
        p = [b[f"Bip01 {s} Finger{f}{j}"].head_local for j in ("", "1", "2")]
        bend += ((p[2] - p[1]) - (p[1] - p[0])).dot(normal)
    palm = normal if bend >= 0.0 else -normal
    socket = wrist + (knuckles - wrist) * 0.55 + palm * 0.025
    return {"wrist": wrist, "socket": socket, "forward": forward, "across": across, "palm": palm,
            "rotation": b[f"Bip01 {s} Hand"].matrix_local.to_quaternion()}


REST_HANDS = {s: hand_rest(s) for s in ("r", "l")}
# On the AK as the view model holds it (weapons/ak47.py): the grip, and the lower
# handguard from beneath, in the rifle's space.
HOLDS = {"r": (Vector((0.004, 0.0, 0.05)), Vector((-1.0, 0.4, 0.0)), Vector((-0.3, 0.9, -0.35))),
         "l": (Vector((0.03, 0.30, 0.142)), Vector((0.5, 0.0, 1.0)), Vector((1.0, 0.15, -0.3)))}


def hand_target(side):
    """The hand bone's matrix (armature space) holding the rifle at its rest place."""
    rest = REST_HANDS[side]
    socket, palm, forward = HOLDS[side]
    turn = (rig.basis(AIM.to_3x3() @ forward, AIM.to_3x3() @ palm) @
            rig.basis(rest["forward"], rest["palm"]).inverted()).to_quaternion()
    wrist = AIM @ socket - turn @ (rest["socket"] - rest["wrist"])
    return Matrix.LocRotScale(wrist, turn @ rest["rotation"], None)


rig.select_only(arm)
bpy.ops.object.mode_set(mode="EDIT")
eb = arm.data.edit_bones
for side in ("r", "l"):
    s = "R" if side == "r" else "L"
    target = eb.new(f"ik_{side}")
    m = hand_target(side)
    target.head = m.translation
    target.tail = m.translation + m.to_3x3() @ Vector((0.0, 0.08, 0.0))
    target.align_roll(m.to_3x3() @ Vector((0.0, 0.0, 1.0)))
    target.parent = eb["weapon"]
    target.use_deform = False
    pole = eb.new(f"pole_{side}")
    x = 0.55 if side == "r" else -0.55
    pole.head = Vector((x, -0.15, 1.0))
    pole.tail = pole.head + Vector((0.0, 0.1, 0.0))
    pole.parent = eb["Bip01 Spine2"]
    pole.use_deform = False
bpy.ops.object.mode_set(mode="OBJECT")


def solve_arm(side):
    """Puts the hand where its target on the rifle is, turned as it says: the
    elbow placed on the shoulder-wrist triangle towards its pole, the upper arm
    and forearm turned onto it (a two-bone IK, by the joints themselves)."""
    s_ = "R" if side == "r" else "L"
    bones_ = arm.pose.bones
    upper, fore, hand = (bones_[f"Bip01 {s_} UpperArm"], bones_[f"Bip01 {s_} Forearm"], bones_[f"Bip01 {s_} Hand"])
    goal = bones_[f"ik_{side}"].matrix.copy()
    pole = bones_[f"pole_{side}"].head.copy()
    shoulder, elbow, wrist = upper.head.copy(), fore.head.copy(), hand.head.copy()
    l1, l2 = (elbow - shoulder).length, (wrist - elbow).length
    target = goal.translation
    reach = target - shoulder
    d = min(max(reach.length, 1e-4), l1 + l2 - 1e-4)
    axis = reach.normalized()
    along = (l1 * l1 - l2 * l2 + d * d) / (2.0 * d)
    up = math.sqrt(max(l1 * l1 - along * along, 0.0))
    towards = pole - shoulder
    side_dir = towards - axis * towards.dot(axis)
    side_dir = side_dir.normalized() if side_dir.length > 1e-6 else axis.orthogonal().normalized()
    new_elbow = shoulder + axis * along + side_dir * up
    q = (elbow - shoulder).rotation_difference(new_elbow - shoulder)
    upper.matrix = Matrix.Translation(shoulder) @ q.to_matrix().to_4x4() @ Matrix.Translation(-shoulder) @ upper.matrix
    bpy.context.view_layer.update()
    elbow, wrist = fore.head.copy(), hand.head.copy()
    q = (wrist - elbow).rotation_difference(target - elbow)
    fore.matrix = Matrix.Translation(elbow) @ q.to_matrix().to_4x4() @ Matrix.Translation(-elbow) @ fore.matrix
    bpy.context.view_layer.update()
    hand.matrix = Matrix.LocRotScale(hand.head.copy(), goal.to_quaternion(), None)
    bpy.context.view_layer.update()


# --- Fingers closed round the grip and handguard ---
CURLS = {"r": {"1": (0.25, 0.35, 0.25), "2": (1.2, 1.3, 0.8), "3": (1.3, 1.3, 0.8), "4": (1.3, 1.25, 0.8),
               "0": (0.4, 0.5, 0.4)},
         "l": {"1": (0.8, 1.1, 0.8), "2": (0.85, 1.15, 0.85), "3": (0.9, 1.15, 0.85), "4": (0.95, 1.1, 0.8),
               "0": (0.3, 0.4, 0.3)}}


def finger_rotations(side):
    """Each finger joint's local rotation closing it toward the palm."""
    s = "R" if side == "r" else "L"
    rest = REST_HANDS[side]
    out = {}
    sign = None
    for f, angles in CURLS[side].items():
        axis_world = rest["forward"] if f == "0" else rest["across"]
        for j, angle in zip(("", "1", "2"), angles):
            name = f"Bip01 {s} Finger{f}{j}"
            m = arm.data.bones[name].matrix_local.to_3x3()
            axis = (m.inverted() @ axis_world).normalized()
            if sign is None:
                tip = m @ Vector((0.0, 1.0, 0.0))
                moved = m @ (Quaternion(axis, 0.3) @ Vector((0.0, 1.0, 0.0)))
                sign = 1.0 if (moved - tip).dot(rest["palm"]) > 0.0 else -1.0
            k = sign * (1.0 if f != "0" or side == "r" else -1.0)
            out[name] = Quaternion(axis, k * angle)
    return out


FINGERS = {**finger_rotations("r"), **finger_rotations("l")}

# The low ready: the rifle swung down from the shoulder, muzzle toward the
# ground ahead, as a patrol carries it.
READY = (Matrix.Translation((0.05, 0.02, -0.2)) @ Matrix.Rotation(math.radians(-38.0), 4, "X") @
         Matrix.Rotation(math.radians(12.0), 4, "Z"))


# --- Bake each clip through the rig ---
DEFORM = [b.name for b in arm.data.bones if b.use_deform]


def bake(name, source, carry, loop):
    action, start, end = load_motion(source)
    weapon_bone = arm.pose.bones["weapon"]
    weapon_bone.rotation_mode = "QUATERNION"
    carry_basis = READY if carry == "ready" else Matrix()
    frames = {}
    scene = bpy.context.scene
    for f in range(start, end + 1):
        scene.frame_set(f)
        weapon_bone.matrix_basis = carry_basis
        bpy.context.view_layer.update()
        solve_arm("r")
        solve_arm("l")
        for bone_name, q in FINGERS.items():
            pb = arm.pose.bones[bone_name]
            pb.rotation_mode = "QUATERNION"
            pb.rotation_quaternion = q
        frames[f] = {n: (arm.pose.bones[n].location.copy(), arm.pose.bones[n].rotation_quaternion.copy())
                     for n in DEFORM}
    baked = bpy.data.actions.new(name)
    arm.animation_data.action = baked
    for f, pose in frames.items():
        for n, (loc, rot) in pose.items():
            pb = arm.pose.bones[n]
            pb.rotation_mode = "QUATERNION"
            pb.location = loc
            pb.rotation_quaternion = rot
            pb.keyframe_insert("location", frame=f)
            pb.keyframe_insert("rotation_quaternion", frame=f)
    baked.use_fake_user = True
    track = arm.animation_data.nla_tracks.new()
    track.name = name
    track.strips.new(name, start, baked)
    track.mute = True
    arm.animation_data.action = None
    bpy.data.actions.remove(action)
    return {"loop": loop, "speed": round(SPEEDS[source], 3), "events": []}


EVENTS = {}
for name, source, carry, loop in CLIPS:
    EVENTS[name] = bake(name, source, carry, loop)
    print("clip", name, "from", source, carry, "speed", EVENTS[name]["speed"])
# --- Materials: Rocketbox's colour, normal and specular maps ---
tex = os.path.join(avatar_dir, "Textures")


def surface(slot_name):
    stem = slot_name.replace("sm002_", "")
    files = os.listdir(tex)

    def find(kind):
        cands = [f for f in files if f.startswith(slot_name) and f"_{kind}" in f]
        return os.path.join(tex, sorted(cands)[0]) if cands else None
    mat = bpy.data.materials.new("soldier_" + stem)
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    bsdf = next(n for n in nodes if n.type == "BSDF_PRINCIPLED")
    color = find("color")
    if color:
        t = nodes.new("ShaderNodeTexImage")
        t.image = bpy.data.images.load(color)
        links.new(t.outputs["Color"], bsdf.inputs["Base Color"])
    normal = find("normal")
    if normal:
        t = nodes.new("ShaderNodeTexImage")
        t.image = bpy.data.images.load(normal)
        t.image.colorspace_settings.name = "Non-Color"
        n = nodes.new("ShaderNodeNormalMap")
        links.new(t.outputs["Color"], n.inputs["Color"])
        links.new(n.outputs["Normal"], bsdf.inputs["Normal"])
    bsdf.inputs["Roughness"].default_value = 0.75
    bsdf.inputs["Metallic"].default_value = 0.0
    return mat


for slot in body.material_slots:
    if slot.material:
        slot.material = surface(slot.material.name.split(".")[0])
from weapon_material import make_weapon_material  # noqa: E402
work = os.path.join(os.path.dirname(os.path.abspath(out_path)), "_work")
os.makedirs(work, exist_ok=True)
rifle.material_slots[0].material = make_weapon_material(os.path.join(pack, "AK47"), work, "T_AK47_C.png",
                                                        "T_AK47_RMAO.png", "T_AK47_N.png", "ak47")
body.name = "soldier"
rifle.name = "soldier_ak47"

if preview:
    shots = {}
    for clip in ("idle", "walk", "aim", "aim_walk", "run"):
        shots[clip] = {f"{clip}_front": ((0.6, 3.2, 1.3), (0.0, 0.0, 1.1), 50.0),
                       f"{clip}_side": ((3.2, 0.3, 1.3), (0.0, 0.0, 1.1), 50.0)}
    for clip, views in shots.items():
        arm.animation_data.action = bpy.data.actions[clip]
        bpy.context.scene.frame_set(int(bpy.data.actions[clip].frame_range[0]) + 8)
        rig.render_views(preview, views)
    arm.animation_data.action = None

bpy.ops.object.select_all(action="DESELECT")
for o in (arm, body, rifle):
    o.select_set(True)
bpy.ops.export_scene.gltf(
    filepath=out_path, export_format="GLTF_SEPARATE", use_selection=True, export_yup=True, export_apply=False,
    export_tangents=True, export_skins=True, export_def_bones=True, export_animations=True,
    export_animation_mode="NLA_TRACKS", export_force_sampling=True, export_frame_step=1,
    export_optimize_animation_size=False, export_image_format="JPEG", export_image_quality=88)
with open(os.path.splitext(out_path)[0] + ".events.json", "w", encoding="utf-8") as f:
    json.dump({"fps": FPS, "clips": EVENTS}, f, indent=2)
for name in os.listdir(work):
    os.remove(os.path.join(work, name))
os.rmdir(work)
print("wrote", out_path)
