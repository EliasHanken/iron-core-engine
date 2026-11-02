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
from mathutils.bvhtree import BVHTree

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


def import_gun(fbx_path, length=None, keep_unbound=False, keep_rounds=False):
    """Imports any gun from the pack: its meshes joined into one (the rounds and
    glow sights dropped), muzzle down +Y, at its own metric size (or scaled to
    `length`), the origin at the trigger with the sights' line on x = 0. Meshes
    bound to no bone are the pack's optional accessories (rails, lights): dropped,
    or with `keep_unbound` fixed to the frame. With `keep_rounds`, the cartridges
    stay, on a brass material of their own (the mesh's second slot). Returns
    (armature, mesh)."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=fbx_path)
    new = [o for o in bpy.data.objects if o not in before]
    skeleton = next(o for o in new if o.type == "ARMATURE")
    meshes = []
    for o in new:
        if o.type != "MESH" or len(o.data.vertices) <= 64:
            continue
        bound = any(v.groups for v in o.data.vertices)
        if not bound:
            if not keep_unbound:
                continue
            group = o.vertex_groups.get("FRAME") or o.vertex_groups.new(name="FRAME")
            group.add(range(len(o.data.vertices)), 1.0, "REPLACE")
            if not any(m.type == "ARMATURE" for m in o.modifiers):
                o.modifiers.new("Armature", "ARMATURE").object = skeleton
        meshes.append(o)
    for o in new:
        if o is not skeleton and o not in meshes:
            bpy.data.objects.remove(o, do_unlink=True)
    select_only(*meshes)
    if len(meshes) > 1:
        bpy.ops.object.join()
    gun = bpy.context.view_layer.objects.active

    # The glow inserts go, and the rounds unless kept: they have materials of
    # their own, untextured.
    def kind(material):
        name = material.name.upper() if material else ""
        return "round" if "BULLET" in name else "glow" if "GLOW" in name else "gun"
    select_only(gun)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="DESELECT")
    bpy.ops.object.mode_set(mode="OBJECT")
    kinds = [kind(gun.material_slots[p.material_index].material) for p in gun.data.polygons]
    for p, k in zip(gun.data.polygons, kinds):
        p.select = k == "glow" or (k == "round" and not keep_rounds)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.delete(type="FACE")
    bpy.ops.object.mode_set(mode="OBJECT")
    # The gun's textures cover every part; the rounds, if kept, are plain brass.
    rounds = [kind(gun.material_slots[p.material_index].material) == "round" for p in gun.data.polygons]
    gun.data.materials.clear()
    gun.data.materials.append(bpy.data.materials.new("gun"))
    if any(rounds):
        brass = bpy.data.materials.new("rounds")
        if hasattr(brass, "use_nodes"):
            brass.use_nodes = True
        bsdf = next(n for n in brass.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
        bsdf.inputs["Base Color"].default_value = (0.78, 0.56, 0.3, 1.0)
        bsdf.inputs["Metallic"].default_value = 1.0
        bsdf.inputs["Roughness"].default_value = 0.32
        gun.data.materials.append(brass)
    for p, is_round in zip(gun.data.polygons, rounds):
        p.material_index = 1 if is_round else 0
    apply_transform(skeleton, gun)

    ys = [(gun.matrix_world @ v.co).y for v in gun.data.vertices]
    scale = 1.0 if length is None else length / (max(ys) - min(ys))
    turn = Matrix.Rotation(math.pi, 4, "Z") @ Matrix.Scale(scale, 4)
    skeleton.matrix_world = turn @ skeleton.matrix_world
    gun.matrix_world = turn @ gun.matrix_world
    apply_transform(skeleton, gun)

    trigger = skeleton.data.bones["TRIGGER"].head_local
    x_mid = skeleton.data.bones["IRONSIGHT"].head_local.x
    shift = Matrix.Translation(-Vector((x_mid, trigger.y, trigger.z)))
    skeleton.matrix_world = shift @ skeleton.matrix_world
    gun.matrix_world = shift @ gun.matrix_world
    apply_transform(skeleton, gun)
    return skeleton, gun


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


SKIN = 0.009        # From a finger bone to the skin over it, in metres (without skin vertices).
PALM_SKIN = 0.012   # From the hand's bones to the skin of the palm (likewise).
TOUCH = 0.0012      # How close skin comes to the gun where it rests on it: just touching.


def hand_skin(arms, skin, side, per_bone=48):
    """The skin each part of a hand carries: for every finger bone and the wrist
    (the palm and the back of the hand), the vertices it moves most, in that
    bone's rest space. Posed with the bone, they're where the skin is, so contact
    can be measured on the skin itself rather than guessed from the bones."""
    names = {g.index: g.name for g in skin.vertex_groups}
    to_arms = arms.matrix_world.inverted() @ skin.matrix_world
    verts = skin.data.vertices

    def owner(indices):
        """The bone that moves these vertices most, and by how much."""
        totals = {}
        for i in indices:
            for g in verts[i].groups:
                totals[g.group] = totals.get(g.group, 0.0) + g.weight / len(indices)
        if not totals:
            return None, 0.0
        group, weight = max(totals.items(), key=lambda kv: kv[1])
        return names[group], weight

    # Every vertex, and the middle of every face: the arms are low-poly, and a
    # finger's faces are wide enough for an edge of the gun to slip between corners.
    samples = [((v.index,), v.co) for v in verts]
    samples += [(tuple(p.vertices), p.center) for p in skin.data.polygons]
    parts = {}
    for indices, co in samples:
        name, weight = owner(indices)
        if name is None or weight < 0.5 or not name.endswith("." + side):
            continue
        if not (name.startswith("finger_") or name == f"wrist.{side}"):
            continue
        bone = arms.data.bones[name]
        local = bone.matrix_local.inverted() @ (to_arms @ co)
        if name.startswith("wrist") and local.y < 0.15 * bone.length:
            continue  # The wrist's own skin, not the hand's.
        if name[-3] == "1" and local.y < 0.4 * bone.length:
            continue  # A finger's root is the palm's skin: it rests on the gun with the palm.
        parts.setdefault(name, []).append(local)
    for name, verts in parts.items():
        limit = per_bone * (2 if name.startswith("wrist") else 1)
        if len(verts) > limit:
            stride = len(verts) / limit
            parts[name] = [verts[int(i * stride)] for i in range(limit)]
    return parts
# How far each finger joint closes at most, in radians (knuckle, middle, tip);
# the thumb's own.
JOINT_LIMITS = {"finger": (1.65, 1.9, 1.45), "thumb": (1.1, 1.3, 1.4)}


class GunSurface:
    """The gun's surface as it is this frame, moving parts and all (a magazine in
    the hand, the slide back), for palms to rest on and fingers to close round;
    and, for a pistol's support hand, the firing hand's skin as well, so it wraps
    the hand that holds the grip rather than sinking through it."""

    def __init__(self, gun, skin=None):
        self.gun = gun
        self.skin = skin
        self.tree = None
        self.hand_tree = None
        self.to_local = Matrix()
        self.hand_polys = {}

    def update(self, with_hand=None):
        """Rebuilds from the gun as posed now; `with_hand` ("r", "l") adds that
        hand's skin as posed now."""
        depsgraph = bpy.context.evaluated_depsgraph_get()
        self.tree = BVHTree.FromObject(self.gun, depsgraph)
        self.to_local = self.gun.matrix_world.inverted() @ self.gun.parent.matrix_world
        self.hand_tree = None
        if with_hand and self.skin is not None:
            polys = self._hand_polys(with_hand)
            mesh = self.skin.evaluated_get(depsgraph).to_mesh()
            to_arms = self.gun.parent.matrix_world.inverted() @ self.skin.matrix_world
            to_local = self.to_local @ to_arms
            verts = [to_local @ v.co for v in mesh.vertices]
            self.hand_tree = BVHTree.FromPolygons(verts, polys)
            self.skin.evaluated_get(depsgraph).to_mesh_clear()

    def _hand_polys(self, side):
        """The skin's faces a hand's bones move (its fingers and palm)."""
        if side not in self.hand_polys:
            names = {g.index: g.name for g in self.skin.vertex_groups}
            verts = self.skin.data.vertices

            def on_hand(i):
                if not verts[i].groups:
                    return False
                g = max(verts[i].groups, key=lambda g: g.weight)
                name = names[g.group]
                return name.endswith("." + side) and (name.startswith("finger_") or name.startswith("wrist"))
            self.hand_polys[side] = [tuple(p.vertices) for p in self.skin.data.polygons
                                     if all(on_hand(i) for i in p.vertices)]
        return self.hand_polys[side]

    def signed(self, p, far=0.06):
        """How far a point (armature space) is outside the gun; negative inside."""
        q = self.to_local @ p
        d = self._signed(self.tree, q, far)
        if self.hand_tree is not None:
            d = min(d, self._signed(self.hand_tree, q, far))
        return d

    @staticmethod
    def _signed(tree, q, far):
        loc, normal, _, dist = tree.find_nearest(q, far)
        if loc is None:
            return far
        # Inside, by the nearest face's facing; but not trusted far from it: a
        # stray inward face (a hollow part's inner wall) would read a point well
        # clear of the gun as deep in it, and nothing gets far in unnoticed.
        return dist if (q - loc).dot(normal) >= 0.0 or dist > 0.012 else -dist


def _curl_sign(arms, side, rest):
    """Which way round a finger joint turns to close towards the palm."""
    key = ("curl_sign", side)
    if key not in rest:
        pb = arms.pose.bones[f"finger_index1.{side}"]
        rest_m = pb.bone.matrix_local.to_3x3()
        local_axis = (rest_m.inverted() @ rest["across"]).normalized()
        tip = rest_m @ Vector((0.0, 1.0, 0.0))
        moved = rest_m @ (Quaternion(local_axis, 0.3) @ Vector((0.0, 1.0, 0.0)))
        rest[key] = 1.0 if (moved - tip).dot(rest["palm"]) > 0.0 else -1.0
    return rest[key]


def _fan(arms, side, rest, digit):
    """How far a finger fans out from the middle one at rest, in radians about the
    palm's normal: the rig's resting hand is splayed, a gripping one isn't."""
    key = ("fan", side, digit)
    if key not in rest:
        b = arms.data.bones
        normal = rest["palm"]

        def flat(name):
            d = b[name].tail_local - b[name].head_local
            return (d - normal * d.dot(normal)).normalized()
        middle, own = flat(f"finger_middle1.{side}"), flat(f"finger_{digit}1.{side}")
        rest[key] = math.atan2(middle.cross(own).dot(normal), middle.dot(own))
    return rest[key]


def digit_rotations(arms, side, rest, digit, angles, together=0.0, spread=0.0):
    """Each joint's local rotation closing a digit by `angles` (radians): fingers
    fold about the knuckles' line, the thumb about the hand's length. At the
    knuckle a finger also swings about the palm's normal: `together` (0..1) closes
    the rest pose's splay, `spread` (radians) swings it further on its own. The
    thumb's `spread` is a pair: its base swung about the palm's normal, and towards
    or away from the palm."""
    sign = _curl_sign(arms, side, rest)
    world_axis = rest["forward"] if digit == "thumb" else rest["across"]
    if digit == "thumb":
        sign *= 1.0 if side == "r" else -1.0
    out = []
    for j, angle in zip((1, 2, 3), angles):
        rest_m = arms.pose.bones[f"finger_{digit}{j}.{side}"].bone.matrix_local.to_3x3()
        q = Quaternion((rest_m.inverted() @ world_axis).normalized(), sign * angle)
        if j == 1 and digit != "thumb":
            swing = -_fan(arms, side, rest, digit) * together + spread
            if swing:
                q = Quaternion((rest_m.inverted() @ rest["palm"]).normalized(), swing) @ q
        elif j == 1 and spread:
            about_palm, towards_palm = spread
            q = (Quaternion((rest_m.inverted() @ rest["palm"]).normalized(), about_palm) @
                 Quaternion((rest_m.inverted() @ rest["across"]).normalized(), towards_palm) @ q)
        out.append(q)
    return out


def digit_points(arms, side, rest, digit, angles, wrist_matrix, start=1, together=0.0, spread=0.0, at=None):
    """Points along a digit closed by `angles`, from joint `start` out to its tip,
    in armature space: forward kinematics from the posed wrist, so a grasp can try
    many closings without updating the scene."""
    bones = arms.pose.bones
    parent_rest = bones[f"wrist.{side}"].bone.matrix_local
    m = wrist_matrix
    points = []
    for j, q in zip((1, 2, 3), digit_rotations(arms, side, rest, digit, angles, together, spread)):
        b = bones[f"finger_{digit}{j}.{side}"].bone
        m = m @ (parent_rest.inverted() @ b.matrix_local) @ q.to_matrix().to_4x4()
        parent_rest = b.matrix_local
        if j >= start:
            skin = rest.get("skin", {}).get(b.name)
            if skin and at is None:
                points += [m @ v for v in skin]
            else:
                points += [m @ Vector((0.0, b.length * t, 0.0)) for t in (at or (0.3, 0.65, 1.0))]
    return points


def _clearance(rest):
    """How close a digit's sample points may come to the gun: the skin itself just
    touching, or the guessed skin over the bone's line."""
    return TOUCH if rest.get("skin") else SKIN


def _first(test, lo, hi, step):
    """The least value in [lo, hi] where `test` turns true, found by stepping then
    halving (so thin parts aren't stepped over); None if it never does."""
    if test(lo):
        return lo
    prev, v = lo, lo
    while v < hi:
        v = min(v + step, hi)
        if test(v):
            for _ in range(6):
                mid = (prev + v) * 0.5
                if test(mid):
                    v = mid
                else:
                    prev = mid
            return v
        prev = v
    return None


def seat_palm(arms, side, rest, hand, surface, memory=None):
    """Fits the hand onto the gun: slides it (along its palm's facing, along its
    fingers and across its knuckles, a couple of centimetres at most) to where no
    skin is buried in the gun, the open fingers and the thumb's root included, and
    the palm rests on the surface. A hand authored roughly in place ends up
    holding, not hovering, with its fingers free to wrap from there.

    The fit is remembered in the hand's own frame (`memory["fit"]`): a hand that
    then carries what it took (a magazine out of the gun) keeps it (carry_fit)."""
    bones = arms.pose.bones
    w = max(0.0, min(1.0, hand.contact))
    skin = rest.get("skin", {})
    wrist = bones[f"wrist.{side}"]
    if f"wrist.{side}" not in skin:
        return Vector()
    m = wrist.matrix.copy()
    turn = m.to_3x3() @ wrist.bone.matrix_local.to_3x3().inverted()
    axes = [(turn @ rest[k]).normalized() for k in ("palm", "forward", "across")]
    palm_points = [m @ v for v in skin[f"wrist.{side}"]]
    held = []
    for f in hand.curl:
        if f not in hand.free and not (f == "index" and hand.trigger is not None):
            held += digit_points(arms, side, rest, f, (0.0, 0.0, 0.0), m, 1, hand.together)
    thumb_root = len(skin.get(f"finger_thumb1.{side}", []))
    held += digit_points(arms, side, rest, "thumb", hand.thumb, m, 1)[:thumb_root]

    def cost(t):
        buried = 0.0
        nearest = 1.0
        for p in palm_points:
            d = surface.signed(p + t)
            nearest = min(nearest, d)
            buried += max(0.0, TOUCH - d)
        for p in held:
            buried += max(0.0, -surface.signed(p + t))
        return buried + 0.3 * max(0.0, nearest - TOUCH) + 5.0 * t.length_squared

    t = Vector()
    best = cost(t)
    for step in (0.008, 0.004, 0.002, 0.001):
        improved = True
        while improved:
            improved = False
            for axis in axes:
                for sign in (1.0, -1.0):
                    trial = t + axis * (step * sign)
                    if max(abs(trial.dot(a)) for a in axes) > 0.025:
                        continue
                    c = cost(trial)
                    if c < best - 1e-7:
                        best, t, improved = c, trial, True
    if os.environ.get("VM_DEBUG"):  # VM_DEBUG=1: how each hand was fitted, in the gun's space.
        inv = hand.weapon_matrix.inverted().to_3x3()
        print("FIT", side, "moved", tuple(round(c, 4) for c in inv @ t), "cost", round(best, 5))
    target = bones[f"wrist_ik.{side}"]
    if memory is not None:
        memory["fit"] = turn.inverted() @ t
    target.matrix = Matrix.Translation(t * w) @ target.matrix
    bpy.context.view_layer.update()
    return t * w


def carry_fit(arms, side, rest, hand, memory):
    """Keeps the last fit (seat_palm) on a hand that carries what it took: the
    offset rides in the hand's frame, so hand and load move as one, no pop."""
    fit = memory.get("fit")
    if fit is None:
        return
    wrist = arms.pose.bones[f"wrist.{side}"]
    turn = wrist.matrix.to_3x3() @ wrist.bone.matrix_local.to_3x3().inverted()
    w = max(0.0, min(1.0, hand.contact))
    target = arms.pose.bones[f"wrist_ik.{side}"]
    target.matrix = Matrix.Translation((turn @ fit) * w) @ target.matrix
    bpy.context.view_layer.update()


def _thumb_swing(arms, side, rest, hand, shape, wrist_matrix, surface, previous=None):
    """Where the thumb's base swings so the thumb clears the gun: the rig's thumb
    only rolls about the hand's length, which can leave it through a grip whatever
    its curl. Searches the base's swing for the pose that rests its outer joints on
    the gun without burying them (its root is the hand fit's to clear), pointing
    along `hand.thumb_aim`, nearest the authored one. With last frame's swing
    (`previous`, degrees), it only searches near it, so the thumb doesn't jump."""
    def cost(about, towards):
        spread = (math.radians(about), math.radians(towards))
        points = digit_points(arms, side, rest, "thumb", shape, wrist_matrix, 2, 0.0, spread)
        gaps = [surface.signed(p) for p in points]
        buried = sum(max(0.0, TOUCH - d) for d in gaps)
        # A thumb lies along the gun, not off in the air, pointing where the hold
        # wants it (forward along a handguard, round a grip).
        hover = max(0.0, min(gaps) - TOUCH)
        c = buried + 0.15 * hover + 0.0004 * (about * about + towards * towards) / 900.0
        if aim is not None:
            line = digit_points(arms, side, rest, "thumb", shape, wrist_matrix, 2, 0.0, spread, at=(0.0, 1.0))
            pointing = (line[-1] - line[0]).normalized()
            c += 0.006 * (1.0 - pointing.dot(aim))
        return c

    aim = (hand.weapon_matrix.to_3x3() @ hand.thumb_aim).normalized() if hand.thumb_aim is not None else None

    if previous is None:
        best = min(((a, t) for a in range(-50, 51, 10) for t in range(-50, 51, 10)), key=lambda at: cost(*at))
    else:
        best = previous
    for step in (5.0, 2.5, 1.0):
        a0, t0 = best
        best = min(((a0 + da, t0 + dt) for da in (-step, 0.0, step) for dt in (-step, 0.0, step)),
                   key=lambda at: cost(*at))
    return best


def grasp(arms, side, rest, hand, surface, memory=None):
    """Closes each finger and the thumb onto the gun, the way a hand takes hold:
    the whole digit folds in its authored shape until its skin meets the surface,
    then the joints past the contact keep closing, middle then tip, until they
    meet it too, wrapping what's held. Skin that already rests on the gun with the
    hand open (a finger's length lying along a grip) only counts once it presses
    in. Digits with nothing in reach keep their authored curl, as do the hand's
    `free` ones; `hand.contact` blends between authored and gripped. `memory`
    carries the searches' answers from frame to frame."""
    memory = {} if memory is None else memory
    w = max(0.0, min(1.0, hand.contact))
    clear = _clearance(rest)
    wrist_m = arms.pose.bones[f"wrist.{side}"].matrix.copy()
    shapes = dict(hand.curl)
    shapes["thumb"] = hand.thumb
    solved = {}
    spreads = {}
    if hand.trigger is not None and w > 0.0:
        angles, spread, memory["trigger"] = trigger_finger(arms, side, rest, hand, wrist_m, surface,
                                                           memory.get("trigger"))
        solved["index"] = tuple(a + (b - a) * w for a, b in zip(hand.curl["index"], angles))
        spreads["index"] = spread * w
        shapes.pop("index")
    for digit, shape in shapes.items():
        if digit in hand.free:
            solved[digit] = tuple(shape)
            continue
        limits = JOINT_LIMITS["thumb" if digit == "thumb" else "finger"]
        spread = 0.0
        if digit == "thumb":
            memory["thumb"] = _thumb_swing(arms, side, rest, hand, shape, wrist_m, surface, memory.get("thumb"))
            spread = (math.radians(memory["thumb"][0]), math.radians(memory["thumb"][1]))
            spreads["thumb"] = (spread[0] * w, spread[1] * w)

        def points(angles, start, digit=digit, spread=spread):
            return digit_points(arms, side, rest, digit, angles, wrist_m, start, hand.together, spread)
        opened = {start: [surface.signed(p) for p in points((0.0, 0.0, 0.0), start)] for start in (1, 2, 3)}

        def touches(angles, start=1, opened=opened, points=points):
            for p, before in zip(points(angles, start), opened[start]):
                d = surface.signed(p)
                if (d < clear) if before > clear + 0.002 else (d < before - 0.002):
                    return True
            return False

        angles = list(shape)
        # Small steps: the gun's thin shells (a handguard's wood) mustn't be stepped through.
        if digit == "thumb":
            # The thumb starts from its authored curl, where the hand was fitted
            # clear of the gun (opened right out, its root swings into the gun):
            # it closes on from there, or opens just until its skin is clear.
            def buried(s):
                return any(surface.signed(p) < clear for p in points([a * s for a in shape], 1))
            if buried(1.0):
                back = _first(lambda x: not buried(1.0 - x), 0.0, 1.0, 0.04)
                scale = 1.0 - (back if back is not None else 0.0)
            else:
                scale = _first(lambda s: buried(s), 1.0, 2.0, 0.04)
        else:
            scale = _first(lambda s: touches([a * s for a in shape]), 0.0, 2.0, 0.04)
        if scale is not None:
            angles = [min(a * scale, lim) for a, lim in zip(shape, limits)]
            for j in (1, 2):
                if touches(angles, j + 1):
                    continue
                trial = lambda e: touches(angles[:j] + [angles[j] + e] + angles[j + 1:], j + 1)  # noqa: E731
                extra = _first(trial, 0.0, max(limits[j] - angles[j], 0.0), 0.04)
                if extra is not None:
                    angles[j] += extra
        solved[digit] = tuple(a + (b - a) * w for a, b in zip(shape, angles))
        if os.environ.get("VM_DEBUG"):  # VM_DEBUG=1: each digit's closing and its skin's nearest gap.
            gap = min(surface.signed(p) for p in points(angles, 1))
            print("GRASP", side, digit, "angles", [round(a, 2) for a in angles], "gap", round(gap, 4))
    thumb = solved.pop("thumb")
    curl_fingers(arms, side, rest, solved, thumb, hand.together, spreads)
    bpy.context.view_layer.update()


def trigger_finger(arms, side, rest, hand, wrist_matrix, surface, previous=None):
    """Lays the index finger on the trigger: the pad of its last joint (the first
    crease) on `hand.trigger`, by searching how far it closes, how its joints share
    the bend, and how far it swings sideways, keeping it out of the guard and the
    frame. Near last frame's (bias, spread) if given, so it doesn't jump. Returns
    (angles, spread, (bias, spread in degrees))."""
    target = hand.weapon_matrix @ hand.trigger
    best = None
    if previous is None:
        tries = [(b, d) for b in (0.6, 0.85, 1.1, 1.35) for d in range(-24, 25, 4)]
    else:
        tries = [(previous[0] + db, previous[1] + dd) for db in (-0.06, 0.0, 0.06) for dd in (-2, 0, 2)]
    for bias, spread_deg in tries:
        spread = math.radians(spread_deg)

        def miss(s):
            angles = (0.55 * s, 0.9 * s * bias, 0.5 * s * bias)
            pad = digit_points(arms, side, rest, "index", angles, wrist_matrix, 3, hand.together, spread,
                               at=(0.45,))[0]
            return (pad - target).length, angles
        lo, hi = 0.0, 2.0
        for _ in range(16):
            a, b = lo + (hi - lo) * 0.382, lo + (hi - lo) * 0.618
            if miss(a)[0] < miss(b)[0]:
                hi = b
            else:
                lo = a
        d, angles = miss((lo + hi) * 0.5)
        inside = sum(max(0.0, -surface.signed(p))
                     for p in digit_points(arms, side, rest, "index", angles, wrist_matrix, 2, hand.together, spread))
        cost = d + 4.0 * inside + 0.002 * abs(spread_deg) / 24.0
        if best is None or cost < best[0]:
            best = (cost, angles, spread, (bias, spread_deg))
    return best[1], best[2], best[3]


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


def curl_fingers(arms, side, rest, curls, thumb, together=0.0, spreads=None):
    """Closes each finger joint about the knuckles' line, towards the palm, and the
    thumb about the hand's length. `curls` maps finger name to three joint angles
    in radians; `together` and `spreads` swing the fingers at the knuckles (see
    digit_rotations)."""
    spreads = spreads or {}
    for digit, angles in list(curls.items()) + [("thumb", thumb)]:
        for j, q in zip((1, 2, 3), digit_rotations(arms, side, rest, digit, angles, together,
                                                   spreads.get(digit, 0.0))):
            pb = arms.pose.bones[f"finger_{digit}{j}.{side}"]
            pb.rotation_mode = "QUATERNION"
            pb.rotation_quaternion = q


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
