"""Authoring for first-person clips: poses described the way an animator thinks of
them (where the gun is, where each hand holds it, how far the bolt is back), keyed
on the control rig frame by frame. The glTF exporter samples the rig with its IK, so
what ships is the deforming skeleton's motion. Each gun (weapons/) says how its own
parts move; the rig here is the same for all of them.
"""
import math
from dataclasses import dataclass, field, replace

import bpy
from mathutils import Matrix, Quaternion, Vector

import rig

FPS = 30


def smooth(s):
    s = min(max(s, 0.0), 1.0)
    return s * s * (3.0 - 2.0 * s)


def ease_out(s):
    s = min(max(s, 0.0), 1.0)
    return 1.0 - (1.0 - s) ** 3


@dataclass
class Hand:
    """A hand on the gun, in the gun's space. `curl` and `thumb` shape the digits;
    with `contact`, the palm seats onto the gun's surface (unless `seat` is off)
    and each digit closes until it meets it, all but the `free` ones (a trigger
    finger laid along the frame). `spin` and `yaw` are how far (degrees) the arm
    solver may roll the hand round what it holds and turn it about the palm."""
    socket: Vector
    palm: Vector
    forward: Vector
    curl: dict
    thumb: tuple = (0.3, 0.4, 0.3)
    contact: float = 0.0  # 0 free, 1 holding the gun: eases in and out between hands.
    spin: float = 30.0
    yaw: float = 35.0
    free: tuple = ()
    seat: bool = True  # Fit onto the gun; off, the hand keeps its last fit (carrying a magazine).
    together: float = 0.7    # How far the fingers close their rest splay (0 splayed, 1 parallel).
    trigger: Vector = None   # Where the index finger's pad rests, on the trigger.
    thumb_aim: Vector = None  # Which way the thumb points along the gun, if it matters.
    weapon_matrix: Matrix = None  # Set by the rig while posing: the gun's place this frame.


@dataclass
class Place:
    """Where the rifle is, in the eye's space: position, then pitch (muzzle up),
    yaw (muzzle left) and roll (top to the right), in degrees."""
    at: Vector
    pitch: float = 0.0
    yaw: float = 0.0
    roll: float = 0.0

    def matrix(self):
        return (Matrix.Translation(self.at) @ Matrix.Rotation(math.radians(self.yaw), 4, "Z") @
                Matrix.Rotation(math.radians(self.pitch), 4, "X") @
                Matrix.Rotation(math.radians(-self.roll), 4, "Y"))

    def moved(self, by=Vector(), pitch=0.0, yaw=0.0, roll=0.0):
        return Place(self.at + by, self.pitch + pitch, self.yaw + yaw, self.roll + roll)


def mix_place(a, b, s):
    return Place(a.at.lerp(b.at, s), a.pitch + (b.pitch - a.pitch) * s, a.yaw + (b.yaw - a.yaw) * s,
                 a.roll + (b.roll - a.roll) * s)


def mix_hand(a, b, s):
    qa = rig.basis(a.forward, a.palm).to_quaternion()
    qb = rig.basis(b.forward, b.palm).to_quaternion()
    q = qa.slerp(qb, s).to_matrix()
    curl = {k: tuple(x + (y - x) * s for x, y in zip(a.curl[k], b.curl[k])) for k in a.curl}
    thumb = tuple(x + (y - x) * s for x, y in zip(a.thumb, b.thumb))
    near = a if s < 0.5 else b
    if a.trigger is not None and b.trigger is not None:
        trigger = a.trigger.lerp(b.trigger, s)
    else:
        trigger = near.trigger
    return Hand(a.socket.lerp(b.socket, s), q.col[1].copy(), q.col[0].copy(), curl, thumb,
                a.contact + (b.contact - a.contact) * s, a.spin + (b.spin - a.spin) * s,
                a.yaw + (b.yaw - a.yaw) * s, near.free, a.seat and b.seat,
                a.together + (b.together - a.together) * s, trigger, near.thumb_aim)


def ease_in(s):
    s = min(max(s, 0.0), 1.0)
    return s * s * s


def overshoot(s, amount=1.6):
    """Eases past the target and settles back: a back-out curve."""
    s = min(max(s, 0.0), 1.0) - 1.0
    return 1.0 + s * s * ((amount + 1.0) * s + amount)


def track(keys, t, mix, ease=smooth):
    """Value at `t` along keys [(time, value)] or [(time, value, ease)], eased
    between neighbours; a key's own ease shapes the way into it."""
    if t <= keys[0][0]:
        return keys[0][1]
    for k0, k1 in zip(keys, keys[1:]):
        t0, a = k0[0], k0[1]
        t1, b = k1[0], k1[1]
        if t <= t1:
            curve = k1[2] if len(k1) > 2 else ease
            return mix(a, b, curve((t - t0) / (t1 - t0)) if t1 > t0 else 1.0)
    return keys[-1][1]


def mix_float(a, b, s):
    return a + (b - a) * s


@dataclass
class Pose:
    place: Place
    right: Hand
    left: Hand
    bolt: float = 0.0        # How far the bolt (carrier, slide) is back, in metres.
    trigger: float = 0.0     # 0 forward, 1 pulled.
    magazine: str = "gun"    # "gun", or "hand" to ride in the left hand.
    parts: dict = field(default_factory=dict)  # The gun's own moving parts (see its move_parts).
    poles: dict = field(default_factory=dict)
    # How far each shoulder rolls forward, in degrees: holding a rifle out, the
    # support shoulder comes round to reach the handguard.
    shoulders: dict = field(default_factory=lambda: {"l": 38.0, "r": 8.0})


class Rig:
    """Applies Poses to the arms' control rig and keys them."""

    def __init__(self, arms, rest, poles, gun, weapon):
        self.arms = arms
        self.rest = rest
        self.poles = poles
        self.weapon = weapon  # The gun's module: its PARTS, MAGAZINE, move_parts.
        # Each hand's skin, so the hands rest on the gun by their skin.
        skin = next(o for o in bpy.data.objects if o.type == "MESH" and o is not gun and
                    any(m.type == "ARMATURE" and m.object is arms for m in o.modifiers))
        self.surface = rig.GunSurface(gun, skin)
        # A pistol's support hand wraps the firing hand: it holds onto that too.
        self.support_on_hand = getattr(weapon, "SUPPORT_ON_HAND", False)
        for side in ("r", "l"):
            rest[side]["skin"] = rig.hand_skin(arms, skin, side)
        # How far the trigger's face moves back when pulled, in metres.
        self.trigger_travel = getattr(weapon, "TRIGGER_TRAVEL", 0.006)
        self.mag_offset = None
        self.mag_fresh = False
        self.elbows = {}  # Each side's (hand spin, elbow swing) last frame, so neither jumps.
        self.grips = {"r": {}, "l": {}}  # Each hand's grasp searches last frame (thumb, trigger finger).
        pose = arms.pose.bones
        self.keyed = ["weapon", "shoulder.r", "shoulder.l", "wrist_ik.r", "wrist_ik.l", "arm_target.r",
                      "arm_target.l"] + list(weapon.PARTS)
        for side in ("r", "l"):
            for f in ("index", "middle", "ring", "pinky", "thumb"):
                self.keyed += [f"finger_{f}{j}.{side}" for j in (1, 2, 3)]
        self.keyed = [b for b in self.keyed if b in pose]
        for b in self.keyed:
            pose[b].rotation_mode = "QUATERNION"

    def apply(self, p):
        arms, pose = self.arms, self.arms.pose.bones
        weapon = p.place.matrix()
        pose["weapon"].matrix = weapon
        for side, degrees in p.shoulders.items():
            # About the vertical through the collarbone's root; forward is +Y.
            bone = pose[f"shoulder.{side}"]
            head = bone.bone.head_local
            angle = math.radians(degrees) * (1.0 if side == "r" else -1.0)
            bone.matrix = (Matrix.Translation(head) @ Matrix.Rotation(angle, 4, "Z") @ Matrix.Translation(-head) @
                           bone.bone.matrix_local)
        bpy.context.view_layer.update()
        poles = {**self.poles, **p.poles}
        for side, pole in poles.items():
            pose[f"arm_target.{side}"].matrix = Matrix.Translation(pole)
        for side, hand in (("r", p.right), ("l", p.left)):
            rig.place_hand(arms, side, self.rest[side], hand.socket, hand.palm, hand.forward, weapon)
            rig.curl_fingers(arms, side, self.rest[side], hand.curl, hand.thumb, hand.together)
        bpy.context.view_layer.update()
        # The elbows go where the wrists are comfortable: the authored poles say
        # roughly where, the solver settles it.
        for side, hand in (("r", p.right), ("l", p.left)):
            self.elbows[side] = rig.solve_arm(arms, side, self.rest[side], hand, weapon, poles[side],
                                              self.elbows.get(side))
        self.move_parts(p, weapon)
        # The hands take hold of the gun as it is now: palms fitted onto its
        # surface (or, carrying something, kept as they were fitted), then fingers
        # closed round it.
        holding = [(s, replace(h, weapon_matrix=weapon)) for s, h in (("r", p.right), ("l", p.left))
                   if h.contact > 0.0]
        # The firing hand first: a pistol's support hand then takes hold of it too.
        for side, hand in holding:
            other = "r" if side == "l" and self.support_on_hand else None
            self.surface.update(other)
            if hand.seat:
                rig.seat_palm(arms, side, self.rest[side], hand, self.surface, self.grips[side])
            else:
                rig.carry_fit(arms, side, self.rest[side], hand, self.grips[side])
            rig.solve_elbow_pose(arms, side, poles[side], self.elbows[side][2])
            # A magazine just taken rides where the fitted hand took it.
            if self.mag_fresh:
                self.mag_offset = None
            self.move_parts(p, weapon)
            self.surface.update(other)
            if hand.trigger is not None:
                # The trigger finger rides the trigger back as it's pulled.
                hand.trigger = hand.trigger - Vector((0.0, self.trigger_travel * p.trigger, 0.0))
            rig.grasp(arms, side, self.rest[side], hand, self.surface, self.grips[side])

    def move_parts(self, p, weapon):
        pose = self.arms.pose.bones
        # The gun's moving parts, each moved in the gun's own space.
        self.weapon.move_parts(pose, weapon, p)
        bpy.context.view_layer.update()
        name = getattr(self.weapon, "MAGAZINE", None)
        if not name:
            return
        # A magazine out of the gun rides in the support hand, as it was taken.
        mag = pose[name]
        socket = pose["socket.l"].matrix
        self.mag_fresh = False
        if p.magazine == "hand":
            if self.mag_offset is None:
                self.mag_offset = socket.inverted() @ mag.matrix
                self.mag_fresh = True
            mag.matrix = socket @ self.mag_offset
        else:
            self.mag_offset = None
            drop = p.parts.get("mag_drop", 0.0)  # Fallen free of its catch, in metres.
            mag.matrix = weapon @ Matrix.Translation((0.0, 0.0, -drop)) @ mag.bone.matrix_local
        bpy.context.view_layer.update()


    def key(self, frame):
        pose = self.arms.pose.bones
        for b in self.keyed:
            pose[b].keyframe_insert("location", frame=frame)
            pose[b].keyframe_insert("rotation_quaternion", frame=frame)



def slide_bone(pose, name, weapon, offset):
    """Moves a gun's bone by `offset` (gun space) from where it rests."""
    bone = pose[name]
    bone.matrix = weapon @ Matrix.Translation(offset) @ bone.bone.matrix_local


def turn_bone(pose, name, weapon, axis, degrees, pivot=None, offset=Vector()):
    """Turns a gun's bone about `axis` (gun space) through `pivot` (its head by
    default), after sliding it by `offset`."""
    bone = pose[name]
    at = bone.bone.head_local if pivot is None else pivot
    bone.matrix = (weapon @ Matrix.Translation(offset) @ Matrix.Translation(at) @
                   Matrix.Rotation(math.radians(degrees), 4, axis) @ Matrix.Translation(-at) @
                   bone.bone.matrix_local)


def bake_clip(r, name, length, pose_at, loop=False):
    """Keys `pose_at(t)` every frame into an action called `name`, stashed on the
    rig so the exporter writes it as its own animation."""
    arms = r.arms
    if arms.animation_data is None:
        arms.animation_data_create()
    action = bpy.data.actions.new(name)
    arms.animation_data.action = action
    r.mag_offset = None
    r.elbows = {}
    r.grips = {"r": {}, "l": {}}
    frames = max(1, round(length * FPS))
    if loop:
        # Solved from the end of its own cycle, so the first frame matches the
        # last and the loop has no seam.
        for f in range(max(frames - 12, 0), frames):
            r.apply(pose_at(f / FPS))
        r.mag_offset = None
    for f in range(frames + 1):
        t = length if (loop and f == frames) else f / FPS
        r.apply(pose_at(0.0 if (loop and f == frames) else t))
        r.key(f)
    action.use_fake_user = True
    track_ = arms.animation_data.nla_tracks.new()
    track_.name = name
    track_.strips.new(name, 0, action)
    track_.mute = True
    arms.animation_data.action = None
    return action
