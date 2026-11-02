"""Builds Foundry's first-person rig in Blender: the WRAD arms holding the AK-47.

The arms' own .blend carries a control rig: an IK target per wrist (wrist_ik.*,
which also sets the wrist's rotation), a pole per elbow (arm_target.*) and forearm
twist bones. This module merges the rifle's skeleton into it under a new `weapon`
bone and parents both wrist targets to that bone, so posing the rifle carries the
hands with it, the way a first-person rig is animated.

Two things make the arms read as real ones. The forearm turns along its length
(setup_forearm_twist): the twist bones take thirds of the wrist's roll by
swing-twist decomposition. And every frame, each arm is settled (solve_arm): the
hand keeps its place on the rifle but may roll round what it holds and turn
about its palm, and the elbow swings round the shoulder-wrist line, to where the
wrist bends and twists least, within what a wrist can do (--joints reports it).

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


def setup_forearm_twist(arms, shares=(0.34, 0.33)):
    """The forearm turns along its length when the hand rolls: the radius crosses
    the ulna from the elbow out. The rig's two twist bones split the forearm in
    half; each takes its share of the wrist's twist about the forearm (cumulative,
    since the second hangs off the first), and the wrist's own skin takes the rest.

    The twist is found by swing-twist decomposition, so it stays true when the
    wrist also bends, and doesn't flip until the hand has turned half round."""
    for side in ("r", "l"):
        for i, share in enumerate(shares):
            pb = arms.pose.bones[f"forearm.Twist{i}.{side}"]
            for c in list(pb.constraints):
                pb.constraints.remove(c)
            c = pb.constraints.new("TRANSFORM")
            c.name = "Twist from wrist"
            c.target = arms
            c.subtarget = f"wrist.{side}"
            c.target_space = "LOCAL"
            c.owner_space = "LOCAL"
            c.map_from = "ROTATION"
            c.from_rotation_mode = "SWING_TWIST_Y"
            c.from_min_y_rot, c.from_max_y_rot = -math.pi, math.pi
            c.map_to = "ROTATION"
            c.map_to_x_from, c.map_to_y_from, c.map_to_z_from = "X", "Y", "Z"
            c.to_min_y_rot, c.to_max_y_rot = -math.pi * share, math.pi * share
            c.mix_mode_rot = "REPLACE"


def wrist_angles(arms, side):
    """The wrist against the forearm, from rest, in degrees: twist about the
    forearm (pronation, supination), bend towards the palm or back (flexion),
    swing sideways (deviation); and the twist the forearm's far twist bone has."""
    bones = arms.pose.bones
    forearm = bones[f"forearm.{side}"]

    def relative(name):
        bone = bones[name]
        rest_rel = forearm.bone.matrix_local.inverted() @ bone.bone.matrix_local
        return (rest_rel.inverted() @ forearm.matrix.inverted() @ bone.matrix).to_quaternion()

    def twist_of(q):
        if q.w < 0.0:
            q = -q
        return math.degrees(2.0 * math.atan2(q.y, q.w))

    q = relative(f"wrist.{side}")
    twist = twist_of(q)
    half = math.radians(twist) / 2.0
    swing = q @ Quaternion((math.cos(half), 0.0, math.sin(half), 0.0)).inverted()
    axis, angle = swing.to_axis_angle()
    return twist, math.degrees(axis.x * angle), math.degrees(axis.z * angle), twist_of(relative(f"forearm.Twist1.{side}"))


# What a wrist does comfortably, in degrees from the rig's rest. At rest the palms
# face down and in, about 45 degrees pronated, so a neutral, thumb-up forearm is
# 45 degrees of supination away: positive twist on the right, negative on the
# left (measured on the rig). A forearm turns about 80 degrees either way from
# neutral.
NEUTRAL_TWIST = {"r": 45.0, "l": -45.0}
TWIST_RANGE = 80.0
FLEX_RANGE = 55.0
DEVIATION_RANGE = 22.0


def wrist_strain(arms, side):
    twist, flex, deviation, _ = wrist_angles(arms, side)
    off = max(abs(twist - NEUTRAL_TWIST[side]) - TWIST_RANGE, 0.0) / 15.0
    return (flex / FLEX_RANGE) ** 2 + (deviation / DEVIATION_RANGE) ** 2 + off * off


def solve_elbow(arms, side, authored_pole, previous=None, steps=16):
    """Swings the elbow round the line from shoulder to wrist (the hand stays where
    it is on the rifle) to where the wrist is least strained, leaning towards the
    authored pole and, frame to frame, towards where the elbow was. Poses the
    pole there and returns the swing angle chosen."""
    bones = arms.pose.bones
    shoulder = bones[f"bicep.{side}"].head.copy()
    wrist = bones[f"wrist_ik.{side}"].matrix.translation.copy()
    upper, lower = bones[f"bicep.{side}"].length, bones[f"forearm.{side}"].length
    reach = wrist - shoulder
    d = min(max(reach.length, 1e-4), upper + lower - 1e-4)
    axis = reach.normalized()
    along = (upper * upper - lower * lower + d * d) / (2.0 * d)
    centre = shoulder + axis * along
    # A frame round the axis, its zero towards the authored pole.
    towards = authored_pole - centre
    e1 = (towards - axis * towards.dot(axis))
    e1 = e1.normalized() if e1.length > 1e-6 else axis.orthogonal().normalized()
    e2 = axis.cross(e1)
    target = bones[f"arm_target.{side}"]

    def pose_at(phi):
        out = e1 * math.cos(phi) + e2 * math.sin(phi)
        target.matrix = Matrix.Translation(centre + out * 0.5)
        bpy.context.view_layer.update()

    def cost(phi):
        pose_at(phi)
        c = wrist_strain(arms, side) + 0.6 * (phi / math.radians(70.0)) ** 2
        # With the hand raised, the elbow hangs below it: arms are heavy, and a
        # raised elbow (a chicken wing) reads as strained.
        if wrist.z > shoulder.z - 0.1:
            drop = wrist.z - bones[f"forearm.{side}"].head.z
            if drop < ELBOW_DROP[side]:
                c += ((ELBOW_DROP[side] - drop) / 0.04) ** 2
        if previous is not None:
            step = (phi - previous + math.pi) % (2.0 * math.pi) - math.pi
            c += (step / math.radians(12.0)) ** 2
        return c

    if previous is None:
        candidates = [(-math.pi + (i + 0.5) * 2.0 * math.pi / steps) for i in range(steps)]
    else:
        candidates = [previous + math.radians(10.0 * k) for k in range(-4, 5)]
    best = min(candidates, key=cost)
    for width in (math.radians(11.0), math.radians(4.0), math.radians(1.5)):
        best = min((best - width, best, best + width), key=cost)
    return best, cost(best)


# How far below a raised hand its elbow hangs, at least, in metres: the support
# arm well under the handguard, the firing arm a little under the grip.
ELBOW_DROP = {"l": 0.15, "r": 0.07}


def solve_arm(arms, side, rest, hand, weapon_matrix, authored_pole, previous=None):
    """Settles one arm. The hand keeps its place on the rifle but may roll round
    what it holds (the line its fingers wrap, palm x fingers, through the socket:
    the handguard, the grip, the magazine) and turn about its palm, square or
    diagonal; the elbow swings round the shoulder-wrist line. All three go where
    the wrist is least strained. `previous` is last frame's (spin, yaw, swing).
    Leaves the arm posed and returns the new (spin, yaw, swing)."""
    spin_range, yaw_range = max(hand.spin, 1e-3), max(hand.yaw, 1e-3)
    palm = hand.palm.normalized()
    wrap = palm.cross(hand.forward)
    wrap = wrap.normalized() if wrap.length > 1e-6 else Vector((0.0, 1.0, 0.0))
    last = previous if previous is not None else (0.0, 0.0, None)
    tried = {}

    def turned(spin, yaw):
        return Quaternion(wrap, math.radians(spin)) @ Quaternion(palm, math.radians(yaw))

    def attempt(spin, yaw):
        spin = max(-spin_range, min(spin_range, spin))
        yaw = max(-yaw_range, min(yaw_range, yaw))
        key = (round(spin, 2), round(yaw, 2))
        if key not in tried:
            turn = turned(spin, yaw)
            place_hand(arms, side, rest, hand.socket, turn @ hand.palm, turn @ hand.forward, weapon_matrix)
            bpy.context.view_layer.update()
            swing, c = solve_elbow(arms, side, authored_pole, last[2])
            c += 0.4 * (spin / spin_range) ** 2 + 0.3 * (yaw / yaw_range) ** 2
            if previous is not None:
                c += ((spin - last[0]) / 8.0) ** 2 + ((yaw - last[1]) / 8.0) ** 2
            tried[key] = (c, spin, yaw, swing)
        return tried[key]

    def best_of(pairs):
        return min((attempt(s, y) for s, y in pairs), key=lambda x: x[0])

    if previous is None:
        best = best_of([(s, y) for s in (-spin_range, 0.0, spin_range) for y in (-yaw_range, 0.0, yaw_range)])
    else:
        best = attempt(last[0], last[1])
    for width in (15.0, 7.5, 3.0):
        _, s0, y0, _ = best
        best = best_of([(s0, y0), (s0 - width, y0), (s0 + width, y0), (s0, y0 - width), (s0, y0 + width)])
    _, spin, yaw, swing = best
    turn = turned(spin, yaw)
    place_hand(arms, side, rest, hand.socket, turn @ hand.palm, turn @ hand.forward, weapon_matrix)
    bpy.context.view_layer.update()
    solve_elbow_pose(arms, side, authored_pole, swing)
    return spin, yaw, swing


def solve_elbow_pose(arms, side, authored_pole, swing):
    """Poses the pole for an elbow swing found by solve_elbow."""
    bones = arms.pose.bones
    shoulder = bones[f"bicep.{side}"].head.copy()
    wrist = bones[f"wrist_ik.{side}"].matrix.translation.copy()
    upper, lower = bones[f"bicep.{side}"].length, bones[f"forearm.{side}"].length
    reach = wrist - shoulder
    d = min(max(reach.length, 1e-4), upper + lower - 1e-4)
    axis = reach.normalized()
    centre = shoulder + axis * ((upper * upper - lower * lower + d * d) / (2.0 * d))
    towards = authored_pole - centre
    e1 = towards - axis * towards.dot(axis)
    e1 = e1.normalized() if e1.length > 1e-6 else axis.orthogonal().normalized()
    e2 = axis.cross(e1)
    out = e1 * math.cos(swing) + e2 * math.sin(swing)
    bones[f"arm_target.{side}"].matrix = Matrix.Translation(centre + out * 0.5)
    bpy.context.view_layer.update()


SKIN = 0.009  # From a finger bone to the skin over it, in metres.


def grip_contact(arms, side, rest, hand, weapon_matrix):
    """Closes each finger, and the thumb, until it meets what the hand holds (a
    cylinder): the hand's curls set each digit's shape, and how far along it
    each goes is searched, so fingers wrap the part rather than float off it or
    sink in. Digits that can't reach keep their authored curl."""
    bones = arms.pose.bones
    centre = weapon_matrix @ hand.hold.centre
    axis = (weapon_matrix.to_3x3() @ hand.hold.axis).normalized()
    reach = hand.hold.radius + SKIN
    digits = ("index", "middle", "ring", "pinky", "thumb")

    def touching(digit):
        points = [bones[f"finger_{digit}{j}.{side}"].head for j in (2, 3)] + [bones[f"finger_{digit}3.{side}"].tail]
        for p in points:
            d = p - centre
            if (d - axis * d.dot(axis)).length < reach:
                return True
        return False

    def pose(scale):
        curls = {f: tuple(a * scale[f] for a in hand.curl[f]) for f in hand.curl}
        curl_fingers(arms, side, rest, curls, tuple(a * scale["thumb"] for a in hand.thumb))
        bpy.context.view_layer.update()

    lo = {d: 0.1 for d in digits}
    hi = {d: 2.0 for d in digits}
    pose(hi)
    reaches = {d: touching(d) for d in digits}
    for _ in range(7):
        mid = {d: (lo[d] + hi[d]) * 0.5 for d in digits}
        pose(mid)
        for d in digits:
            if touching(d):
                hi[d] = mid[d]
            else:
                lo[d] = mid[d]
    w = max(0.0, min(1.0, hand.contact))
    pose({d: 1.0 + ((hi[d] if reaches[d] else 1.0) - 1.0) * w for d in digits})


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
