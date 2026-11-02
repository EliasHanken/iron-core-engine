"""Builds Foundry's first-person rig in Blender: the WRAD arms holding the AK-47.

The arms' own .blend carries a control rig: an IK target per wrist (wrist_ik.*,
which also sets the wrist's rotation), a pole per elbow (arm_target.*) and forearm
twist bones that follow the wrist. This module merges the rifle's skeleton into it
under a new `weapon` bone and parents both wrist targets to that bone, so posing the
rifle carries the hands with it, the way a first-person rig is animated.

Space: metres, the eye at the origin looking down +Y with +Z up. The glTF exporter
turns that into the engine's view space (looking down -Z, +Y up), so the exported
model sits directly on the camera.
"""
import math
import os

import bpy
from mathutils import Matrix, Quaternion, Vector

SHOULDER_TO_WRIST = 0.62  # A tall man's arm, in metres: first-person rigs run long.
RIFLE_LENGTH = 0.88       # An AK-47 with its fixed stock.


def select_only(*objects):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objects:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]


def apply_transform(*objects):
    select_only(*objects)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


def to_metres_at_eye(arms, eye_object):
    """Scales the arms to a real arm and moves them so the eye sits at the origin."""
    bones = arms.data.bones
    reach = (bones["wrist.r"].head_local - bones["shoulder.r"].head_local).length
    k = SHOULDER_TO_WRIST / reach
    eye = eye_object.matrix_world.translation.copy()
    children = [o for o in bpy.data.objects if o.parent is arms]
    for o in [arms]:
        o.matrix_world = Matrix.Translation(-eye * k) @ Matrix.Scale(k, 4) @ o.matrix_world
    apply_transform(arms, *children)
    return k


def import_rifle(fbx_path):
    """Imports the AK-47 at its real size, muzzle down +Y, origin at the foot of the
    pistol grip. Returns (armature, mesh)."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=fbx_path)
    new = [o for o in bpy.data.objects if o not in before]
    skeleton = next(o for o in new if o.type == "ARMATURE")
    meshes = [o for o in new if o.type == "MESH"]
    rifle = max(meshes, key=lambda o: len(o.data.polygons))
    for o in new:
        if o is not rifle and o is not skeleton:
            bpy.data.objects.remove(o, do_unlink=True)

    # The rounds inside the magazine have a material of their own and no textures.
    keep = next(i for i, s in enumerate(rifle.material_slots) if "BULLET" not in s.material.name.upper())
    select_only(rifle)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="DESELECT")
    bpy.ops.object.mode_set(mode="OBJECT")
    for p in rifle.data.polygons:
        p.select = p.material_index != keep
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.delete(type="FACE")
    bpy.ops.object.mode_set(mode="OBJECT")
    while len(rifle.material_slots) > 1:
        rifle.active_material_index = 1 if keep == 0 else 0
        bpy.ops.object.material_slot_remove()
        keep = 0
    apply_transform(skeleton, rifle)

    # Real size, muzzle down +Y (it comes pointing down -Y).
    ys = [(rifle.matrix_world @ v.co).y for v in rifle.data.vertices]
    turn = Matrix.Rotation(math.pi, 4, "Z") @ Matrix.Scale(RIFLE_LENGTH / (max(ys) - min(ys)), 4)
    skeleton.matrix_world = turn @ skeleton.matrix_world
    rifle.matrix_world = turn @ rifle.matrix_world
    apply_transform(skeleton, rifle)

    # The pistol grip's foot: the lowest point a third of the way from the butt.
    verts = [rifle.matrix_world @ v.co for v in rifle.data.vertices]
    y_lo, y_hi = min(v.y for v in verts), max(v.y for v in verts)
    band = [v for v in verts if y_lo + 0.26 * (y_hi - y_lo) <= v.y <= y_lo + 0.36 * (y_hi - y_lo)]
    grip = min(band, key=lambda v: v.z)
    # Side to side, the sights' line: the charging handle sticks out to the right.
    x_mid = skeleton.data.bones["IRONSIGHT"].head_local.x
    shift = Matrix.Translation(-Vector((x_mid, grip.y, grip.z)))
    skeleton.matrix_world = shift @ skeleton.matrix_world
    rifle.matrix_world = shift @ rifle.matrix_world
    apply_transform(skeleton, rifle)
    return skeleton, rifle


def merge_rifle(arms, skeleton, rifle):
    """Joins the rifle's skeleton into the arms' under a `weapon` bone at the grip,
    and hangs both wrist targets from it."""
    select_only(arms)
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arms.data.edit_bones
    weapon = eb.new("weapon")
    weapon.head = Vector((0.0, 0.0, 0.0))
    weapon.tail = Vector((0.0, 0.1, 0.0))
    weapon.roll = 0.0
    weapon.parent = eb["root"]
    weapon.use_deform = True  # So the exporter keeps it.
    bpy.ops.object.mode_set(mode="OBJECT")

    rifle_root = next(b.name for b in skeleton.data.bones if b.parent is None)
    select_only(arms, skeleton)
    bpy.ops.object.join()
    rifle.parent = arms
    for m in rifle.modifiers:
        if m.type == "ARMATURE":
            m.object = arms

    select_only(arms)
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arms.data.edit_bones
    eb[rifle_root].parent = eb["weapon"]
    for side in ("r", "l"):
        eb[f"wrist_ik.{side}"].parent = eb["weapon"]
    bpy.ops.object.mode_set(mode="OBJECT")


def hand_rest(arms, side):
    """The hand at rest, in armature space: wrist, socket, fingers' direction,
    which way the palm faces, and the wrist's rest rotation."""
    b = arms.data.bones
    wrist = b[f"wrist.{side}"].head_local.copy()
    socket = b[f"socket.{side}"].head_local.copy()
    forward = (b[f"finger_middle1.{side}"].head_local - wrist).normalized()
    across = (b[f"finger_index1.{side}"].head_local - b[f"finger_pinky1.{side}"].head_local).normalized()
    normal = forward.cross(across).normalized()
    # Resting fingers bend a little towards the palm, which says which side it is.
    bend = 0.0
    for f in ("index", "middle", "ring", "pinky"):
        p = [b[f"finger_{f}{j}.{side}"].head_local for j in (1, 2, 3)]
        bend += ((p[2] - p[1]) - (p[1] - p[0])).dot(normal)
    palm = normal if bend >= 0.0 else -normal
    return {"wrist": wrist, "socket": socket, "forward": forward, "across": across, "palm": palm,
            "rotation": b[f"wrist.{side}"].matrix_local.to_quaternion()}


def basis(forward, palm):
    f = forward.normalized()
    p = (palm - f * palm.dot(f)).normalized()
    return Matrix((f, p, f.cross(p))).transposed()


def place_hand(arms, side, rest, socket, palm, forward, weapon_matrix):
    """Poses `side`'s wrist target so the hand's socket sits at `socket` with its
    palm facing `palm` and fingers along `forward`, all in the weapon's space."""
    socket_w = weapon_matrix @ socket
    palm_w = weapon_matrix.to_3x3() @ palm
    forward_w = weapon_matrix.to_3x3() @ forward
    turn = (basis(forward_w, palm_w) @ basis(rest["forward"], rest["palm"]).inverted()).to_quaternion()
    wrist = socket_w - turn @ (rest["socket"] - rest["wrist"])
    target = Matrix.LocRotScale(wrist, turn @ rest["rotation"], None)
    arms.pose.bones[f"wrist_ik.{side}"].matrix = target


def curl_fingers(arms, side, rest, curls, thumb):
    """Closes each finger joint about the knuckles' line, towards the palm.
    `curls` maps finger name to three joint angles in radians."""
    axis_sign = None
    for f, angles in curls.items():
        for j, angle in zip((1, 2, 3), angles):
            pb = arms.pose.bones[f"finger_{f}{j}.{side}"]
            rest_m = pb.bone.matrix_local.to_3x3()
            local_axis = (rest_m.inverted() @ rest["across"]).normalized()
            if axis_sign is None:
                # Which way round curls towards the palm: try it on the rest pose.
                tip = rest_m @ Vector((0.0, 1.0, 0.0))
                moved = rest_m @ (Quaternion(local_axis, 0.3) @ Vector((0.0, 1.0, 0.0)))
                axis_sign = 1.0 if (moved - tip).dot(rest["palm"]) > 0.0 else -1.0
            pb.rotation_mode = "QUATERNION"
            pb.rotation_quaternion = Quaternion(local_axis, axis_sign * angle)
    for j, angle in zip((1, 2, 3), thumb):
        pb = arms.pose.bones[f"finger_thumb{j}.{side}"]
        rest_m = pb.bone.matrix_local.to_3x3()
        local_axis = (rest_m.inverted() @ rest["forward"]).normalized()
        pb.rotation_mode = "QUATERNION"
        pb.rotation_quaternion = Quaternion(local_axis, axis_sign * angle * (1.0 if side == "r" else -1.0))


def render_views(path_prefix, views):
    """Workbench renders from named (location, look_at, lens) cameras."""
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "TEXTURE"
    scene.render.resolution_x, scene.render.resolution_y = 960, 540
    cam_data = bpy.data.cameras.get("preview") or bpy.data.cameras.new("preview")
    cam = bpy.data.objects.get("preview") or bpy.data.objects.new("preview", cam_data)
    if cam.name not in scene.collection.objects:
        scene.collection.objects.link(cam)
    cam_data.clip_start = 0.01
    scene.camera = cam
    for name, (loc, at, lens) in views.items():
        cam.location = Vector(loc)
        direction = Vector(at) - Vector(loc)
        cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
        cam_data.lens = lens
        scene.render.filepath = f"{path_prefix}{name}.png"
        bpy.ops.render.render(write_still=True)
