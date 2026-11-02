"""Authoring for first-person clips: poses described the way an animator thinks of
them (where the rifle is, where each hand holds it, how far the bolt is back), keyed
on the control rig frame by frame. The glTF exporter samples the rig with its IK, so
what ships is the deforming skeleton's motion.
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
    """A hand on the rifle, in the rifle's space."""
    socket: Vector
    palm: Vector
    forward: Vector
    curl: dict
    thumb: tuple = (0.3, 0.4, 0.3)


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
    return Hand(a.socket.lerp(b.socket, s), q.col[1].copy(), q.col[0].copy(), curl, thumb)


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
    bolt: float = 0.0        # How far the bolt carrier is back, in metres.
    trigger: float = 0.0     # 0 forward, 1 pulled.
    magazine: str = "gun"    # "gun", or "hand" to ride in the left hand.
    poles: dict = field(default_factory=dict)
    # How far each shoulder rolls forward, in degrees: holding a rifle out, the
    # support shoulder comes round to reach the handguard.
    shoulders: dict = field(default_factory=lambda: {"l": 28.0, "r": 8.0})


class Rig:
    """Applies Poses to the arms' control rig and keys them."""

    def __init__(self, arms, rest, poles):
        self.arms = arms
        self.rest = rest
        self.poles = poles
        self.mag_offset = None
        pose = arms.pose.bones
        self.keyed = ["weapon", "shoulder.r", "shoulder.l", "wrist_ik.r", "wrist_ik.l", "arm_target.r",
                      "arm_target.l", "SLIDE", "TRIGGER",
                      "MAGAZINE"]
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
        for side, pole in {**self.poles, **p.poles}.items():
            pose[f"arm_target.{side}"].matrix = Matrix.Translation(pole)
        bpy.context.view_layer.update()
        for side, hand in (("r", p.right), ("l", p.left)):
            rig.place_hand(arms, side, self.rest[side], hand.socket, hand.palm, hand.forward, weapon)
            rig.curl_fingers(arms, side, self.rest[side], hand.curl, hand.thumb)
        # The rifle's moving parts, each moved in the rifle's own space.
        slide = pose["SLIDE"]
        slide.matrix = weapon @ Matrix.Translation((0.0, -p.bolt, 0.0)) @ slide.bone.matrix_local
        trig = pose["TRIGGER"]
        pivot = trig.bone.head_local
        trig.matrix = (weapon @ Matrix.Translation(pivot) @ Matrix.Rotation(math.radians(-14.0 * p.trigger), 4, "X") @
                       Matrix.Translation(-pivot) @ trig.bone.matrix_local)
        bpy.context.view_layer.update()
        mag = pose["MAGAZINE"]
        socket = pose["socket.l"].matrix
        if p.magazine == "hand":
            if self.mag_offset is None:
                self.mag_offset = socket.inverted() @ mag.matrix
            mag.matrix = socket @ self.mag_offset
        else:
            self.mag_offset = None
            mag.matrix = weapon @ mag.bone.matrix_local
        bpy.context.view_layer.update()

    def key(self, frame):
        pose = self.arms.pose.bones
        for b in self.keyed:
            pose[b].keyframe_insert("location", frame=frame)
            pose[b].keyframe_insert("rotation_quaternion", frame=frame)


def bake_clip(r, name, length, pose_at, loop=False):
    """Keys `pose_at(t)` every frame into an action called `name`, stashed on the
    rig so the exporter writes it as its own animation."""
    arms = r.arms
    if arms.animation_data is None:
        arms.animation_data_create()
    action = bpy.data.actions.new(name)
    arms.animation_data.action = action
    r.mag_offset = None
    frames = max(1, round(length * FPS))
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
