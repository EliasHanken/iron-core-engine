"""Builds Foundry's first-person view model: the WRAD arms (CC0) holding Stein Games'
AK-47 (CC0), posed and animated in Blender, exported as one skinned glTF.

    blender -b --disable-autoexec --factory-startup <wrad>/arms.blend \
        --python build_viewmodel.py -- <AK47 dir> <out.gltf> [--preview <prefix>]

<AK47 dir> holds SKM_AK47.fbx and its textures, from the "Free Classic Weapons
Pack". Poses are set on the arms' control rig (wrist IK targets, elbow poles),
which follows the rifle; the exporter samples the rig, IK and all, into the
deforming bones. Each clip is its own glTF animation, and a sidecar
<out>.events.json lists the moments sounds hang on (magazine out, bolt back...).
"""
import json
import math
import os
import sys

import bpy
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import rig  # noqa: E402
from ak_material import make_ak_material  # noqa: E402
from clips import (FPS, Hand, Place, Pose, Rig, bake_clip, ease_in, ease_out, mix_hand, mix_place,  # noqa: E402
                   smooth, track)

args = sys.argv[sys.argv.index("--") + 1:]
ak_dir, out_path = args[0], args[1]
preview = args[args.index("--preview") + 1] if "--preview" in args else None
out_dir = os.path.dirname(os.path.abspath(out_path))
os.makedirs(out_dir, exist_ok=True)

arms = bpy.data.objects["arms"]
rig.to_metres_at_eye(arms, bpy.data.objects["Camera"])
skeleton, rifle = rig.import_rifle(os.path.join(ak_dir, "SKM_AK47.fbx"))
rig.merge_rifle(arms, skeleton, rifle)
rig.setup_forearm_twist(arms)
rest = {s: rig.hand_rest(arms, s) for s in ("r", "l")}
for o in list(bpy.data.objects):
    if o.type == "CAMERA":
        bpy.data.objects.remove(o, do_unlink=True)

# --- The hold, in the rifle's space (+Y out of the muzzle, +Z up, origin at the
# foot of the pistol grip). Each hand's socket marks the middle of what it holds. ---
# Measured off the model: the lower handguard runs y 0.26..0.38, 4.8 cm wide and
# 0.119..0.157 high, so its middle is (0, 0.32, 0.138).
FIST = {"index": (0.2, 0.3, 0.2), "middle": (1.1, 1.2, 0.7), "ring": (1.2, 1.2, 0.7), "pinky": (1.2, 1.2, 0.7)}
WRAP = {"index": (0.65, 1.15, 0.85), "middle": (0.7, 1.2, 0.85), "ring": (0.75, 1.2, 0.85), "pinky": (0.8, 1.15, 0.85)}
GRIP = Hand(Vector((0.004, 0.0, 0.05)), Vector((-1.0, 0.4, 0.0)), Vector((-0.3, 0.9, -0.35)), FIST)
# The palm under the handguard's left edge, facing up and in; the fingers run
# across beneath it and close up its right side; the thumb lies along the left.
HANDGUARD = Hand(Vector((0.03, 0.32, 0.142)), Vector((0.5, 0.0, 1.0)), Vector((1.0, 0.15, -0.3)), WRAP,
                 thumb=(0.1, 0.2, 0.1))
POLES = {"r": Vector((0.45, 0.05, -0.55)), "l": Vector((-0.35, 0.25, -0.6))}

# Where the rifle sits, in the eye's space (x right, y ahead, z up). At the hip
# the muzzle turns a little in, towards where the shots land.
HIP = Place(Vector((0.13, 0.2, -0.27)), pitch=3.0, yaw=7.0, roll=-5.0)
# Aimed: the line over the rear notch and the front post (0.185 above the grip)
# runs through the eye. As in most shooters the rifle sits further out than a real
# cheek weld would put it, so the stock doesn't fill the bottom of the view.
AIM = Place(Vector((0.0, 0.28, -0.185)))

r = Rig(arms, rest, POLES)
EVENTS = {}


def held(place, **kw):
    return Pose(place, GRIP, HANDGUARD, **kw)


# --- Clips ---

def idle(t):
    """Breathing: the rifle rises and settles every 3.2 seconds."""
    b = math.sin(t / 3.2 * 2.0 * math.pi)
    return held(HIP.moved(Vector((0.0, 0.0, 0.003 * b)), pitch=0.6 * b, roll=0.4 * b))


def aim(t):
    """Aimed, breathing less."""
    b = math.sin(t / 3.2 * 2.0 * math.pi)
    return held(AIM.moved(Vector((0.0, 0.0, 0.0012 * b)), pitch=0.15 * b))


def fire(t):
    """One shot's moving parts: the trigger breaks, the bolt carrier slams back and
    returns. Played as an additive layer over whatever else is playing; the kick
    itself is the engine's, a spring on the whole view, so the hands never leave
    the rifle."""
    lin = lambda a, b, s: a + (b - a) * s  # noqa: E731
    bolt = track([(0.0, 0.0), (0.012, 0.0), (0.04, 0.085), (0.075, 0.0)], t, lin, ease=lambda s: s)
    trigger = track([(0.0, 0.0), (0.01, 1.0), (0.06, 1.0), (0.1, 0.0)], t, lin)
    return held(HIP, bolt=bolt, trigger=trigger)


# --- Hands for the reload, in the rifle's space ---
# The magazine hangs from y 0.16, curving forward to y 0.21 at its foot (z -0.05).
# The support hand takes it from the left, palm on its side, fingers round its
# front, thumb on the release behind it.
# Measured: the magazine is 2.6 cm wide (x -0.013..0.013), its front edge near y 0.215
# at the middle of its height. The palm lies on its left side, the knuckles at the
# front-left edge, and the fingers curl round the front onto the right side; the
# thumb reaches back for the release.
CUP = {"index": (1.25, 1.3, 0.6), "middle": (1.3, 1.3, 0.6), "ring": (1.3, 1.3, 0.6), "pinky": (1.3, 1.25, 0.6)}
MAG_GRAB = Hand(Vector((0.019, 0.215, 0.0)), Vector((1.0, 0.0, 0.0)), Vector((0.0, 1.0, -0.25)), CUP,
                thumb=(0.1, 0.2, 0.1))
# Rocked forward off its catch: the hand rolls the magazine's foot ahead.
MAG_ROCKED = Hand(Vector((0.019, 0.24, -0.035)), Vector((1.0, 0.0, 0.0)), Vector((0.0, 0.88, -0.48)), CUP,
                  thumb=(0.1, 0.2, 0.1))
# Down out of the view, and the "new" one coming back up.
MAG_AWAY = Hand(Vector((-0.04, 0.14, -0.32)), Vector((1.0, 0.0, 0.0)), Vector((0.0, 0.5, -0.85)), CUP,
                thumb=(0.1, 0.2, 0.1))
MAG_RISING = Hand(Vector((0.01, 0.25, -0.12)), Vector((1.0, 0.0, 0.0)), Vector((0.0, 0.8, -0.6)), CUP,
                  thumb=(0.1, 0.2, 0.1))
# On the way from the handguard to the magazine, below the receiver.
UNDER = Hand(Vector((-0.03, 0.26, 0.03)), Vector((0.8, 0.0, 0.6)), Vector((0.3, 0.6, -0.7)),
             {k: (0.3, 0.4, 0.3) for k in CUP}, thumb=(0.2, 0.3, 0.2))
# The right hand off the grip, up over the receiver, hooking the charging handle on
# the right of the bolt carrier.
HOOK = {"index": (0.9, 1.2, 0.8), "middle": (1.0, 1.2, 0.8), "ring": (1.1, 1.2, 0.8), "pinky": (1.1, 1.1, 0.8)}
OVER = Hand(Vector((0.08, 0.06, 0.15)), Vector((-0.7, 0.0, -0.7)), Vector((-0.2, 0.7, 0.6)),
            {k: (0.4, 0.5, 0.3) for k in HOOK})
CHARGE = Hand(Vector((0.055, 0.2, 0.15)), Vector((-1.0, -0.2, 0.0)), Vector((0.0, 0.3, 1.0)), HOOK)
BOLT_TRAVEL = 0.085
# Lifted and canted so the magazine well turns towards the eye, in view.
RELOAD_TILT = Place(Vector((0.13, 0.25, -0.15)), pitch=8.0, yaw=6.0, roll=65.0)


def mix_vector(a, b, s):
    return a.lerp(b, s)


def hands(t, keys_r, keys_l):
    return track(keys_r, t, mix_hand), track(keys_l, t, mix_hand)


def draw(t):
    """Brought up from below and to the right, rolled over, swinging past the hip
    and settling."""
    low = Place(Vector((0.26, 0.06, -0.62)), pitch=-55.0, yaw=35.0, roll=70.0)
    place = track([(0.0, low), (0.42, HIP.moved(Vector((-0.01, 0.01, 0.012)), pitch=5.0, roll=-6.0), ease_out),
                   (0.75, HIP, smooth)], t, mix_place)
    return held(place)


def reload_pose(t, empty):
    """A magazine change: the rifle cants towards the support hand, which strips
    the magazine forward off its catch, drops it out of view, rocks the new one
    in and returns. From empty, the right hand then racks the charging handle."""
    tilt = RELOAD_TILT
    end = 2.3 if not empty else 3.0
    place_keys = [
        (0.0, HIP),
        (0.08, HIP.moved(Vector((0.0, 0.0, -0.008)), pitch=-2.0), smooth),   # Anticipation.
        (0.35, tilt, ease_out),
        (0.62, tilt.moved(Vector((0.004, 0.0, -0.006)), roll=-3.0), smooth),  # Tugged by the strip.
        (0.72, tilt, smooth),
        (1.62, tilt.moved(Vector((0.0, 0.0, 0.004)), roll=2.0), smooth),
        (1.70, tilt.moved(Vector((0.0, 0.0, -0.010)), pitch=-2.5), ease_out),  # The new one seats.
        (1.85, tilt, smooth),
    ]
    if empty:
        rack = tilt.moved(Vector((0.02, 0.0, 0.0)), roll=-18.0, yaw=-4.0)
        place_keys += [(2.05, rack, smooth), (2.3, rack.moved(Vector((0.0, -0.01, 0.0)), pitch=2.0), ease_out),
                       (2.42, rack.moved(Vector((0.0, 0.006, 0.0)), pitch=-1.5), ease_out)]
    place_keys += [(end - 0.35, HIP.moved(Vector((0.0, 0.0, 0.006)), pitch=2.0, roll=-3.0), smooth),
                   (end, HIP, smooth)]
    place = track(place_keys, t, mix_place)

    left_keys = [(0.0, HANDGUARD), (0.2, HANDGUARD), (0.38, UNDER, smooth), (0.5, MAG_GRAB, ease_out),
                 (0.58, MAG_GRAB), (0.72, MAG_ROCKED, ease_out), (1.05, MAG_AWAY, ease_in),
                 (1.15, MAG_AWAY), (1.45, MAG_RISING, ease_out), (1.58, MAG_ROCKED, smooth),
                 (1.72, MAG_GRAB, ease_out), (1.8, MAG_GRAB)]
    left_keys += [(1.98, UNDER, smooth), (2.15 if not empty else 2.6, HANDGUARD, ease_out)]
    right_keys = [(0.0, GRIP)]
    bolt = 0.0
    if empty:
        right_keys += [(1.8, GRIP), (1.95, OVER, smooth), (2.08, CHARGE, ease_out), (2.12, CHARGE),
                       (2.28, Hand(CHARGE.socket - Vector((0.0, BOLT_TRAVEL, 0.0)), CHARGE.palm, CHARGE.forward,
                                   CHARGE.curl), ease_out),
                       (2.36, Hand(CHARGE.socket - Vector((0.0, BOLT_TRAVEL + 0.01, -0.03)), CHARGE.palm,
                                   CHARGE.forward, {k: (0.3, 0.4, 0.3) for k in HOOK}), smooth),
                       (2.62, GRIP, smooth)]
        lin = lambda a, b, s: a + (b - a) * s  # noqa: E731
        bolt = track([(0.0, 0.0), (2.12, 0.0), (2.28, BOLT_TRAVEL, ease_out), (2.31, BOLT_TRAVEL),
                      (2.34, 0.0, ease_in)], t, lin)
    right, left = hands(t, right_keys, left_keys)
    magazine = "hand" if 0.55 <= t < 1.75 else "gun"
    # The left elbow swings out and drops as the hand goes down for the magazine,
    # and comes back in to seat the new one; racking, the right elbow lifts over.
    lp = POLES["l"]
    left_pole = track([(0.0, lp), (0.45, Vector((-0.5, 0.05, -0.5))), (1.05, Vector((-0.45, -0.05, -0.75))),
                       (1.5, Vector((-0.5, 0.05, -0.5))), (1.9, lp), (end, lp)], t, mix_vector)
    rp = POLES["r"]
    right_keys_pole = [(0.0, rp)]
    if empty:
        right_keys_pole += [(1.8, rp), (2.0, Vector((0.55, -0.05, -0.1))), (2.35, Vector((0.55, -0.1, -0.15))),
                            (2.62, rp)]
    right_pole = track(right_keys_pole, t, mix_vector)
    return Pose(place, right, left, bolt=bolt, magazine=magazine, poles={"l": left_pole, "r": right_pole})


def reload(t):
    return reload_pose(t, empty=False)


def reload_empty(t):
    return reload_pose(t, empty=True)


def sprint(t):
    """Running: the rifle swung low and across the chest, bouncing in a figure
    eight with the stride (two steps a cycle)."""
    phase = t / 0.62 * 2.0 * math.pi
    base = Place(Vector((0.11, 0.2, -0.25)), pitch=-12.0, yaw=28.0, roll=-22.0)
    # Elbows tucked in against the body, as when running with a rifle.
    return held(base.moved(Vector((0.018 * math.sin(phase), 0.0, 0.016 * abs(math.sin(phase)) - 0.008)),
                           pitch=2.5 * math.sin(2.0 * phase), roll=4.0 * math.sin(phase)),
                poles={"l": Vector((-0.2, 0.1, -0.7)), "r": Vector((0.3, -0.05, -0.7))})


def inspect(t):
    """Turned over in the hands to look at: the left side, then the right."""
    left_side = Place(Vector((0.04, 0.24, -0.19)), pitch=12.0, yaw=-12.0, roll=68.0)
    right_side = Place(Vector((0.09, 0.23, -0.2)), pitch=8.0, yaw=34.0, roll=-55.0)
    place = track([(0.0, HIP), (0.7, left_side, smooth), (1.9, left_side.moved(Vector((0.0, 0.01, 0.006)), roll=6.0),
                                                          smooth),
                   (2.6, right_side, smooth), (3.6, right_side.moved(Vector((0.0, 0.0, 0.006)), pitch=3.0), smooth),
                   (4.3, HIP, smooth)], t, mix_place)
    # The left elbow rises as the rifle rolls onto its side, then drops for the right.
    lp = POLES["l"]
    left_pole = track([(0.0, lp), (0.7, Vector((-0.45, 0.2, -0.25))), (1.9, Vector((-0.45, 0.2, -0.25))),
                       (2.6, Vector((-0.3, 0.2, -0.75))), (3.6, Vector((-0.3, 0.2, -0.75))), (4.3, lp)], t, mix_vector)
    return held(place, poles={"l": left_pole})


CLIPS = [
    ("idle", 3.2, idle, True, []),
    ("aim", 3.2, aim, True, []),
    ("fire", 0.2, fire, False, []),
    ("draw", 0.75, draw, False, [(0.0, "draw")]),
    ("reload", 2.3, reload, False, [(0.4, "grab"), (0.66, "mag_out"), (1.7, "mag_in")]),
    ("reload_empty", 3.0, reload_empty, False,
     [(0.4, "grab"), (0.66, "mag_out"), (1.7, "mag_in"), (2.24, "bolt_back"), (2.33, "bolt_forward")]),
    ("sprint", 0.62, sprint, True, []),
    ("inspect", 4.3, inspect, False, [(0.2, "grab"), (2.0, "grab")]),
]

for name, length, fn, loop, events in CLIPS:
    bake_clip(r, name, length, fn, loop=loop)
    EVENTS[name] = {"loop": loop, "events": [{"time": t, "name": n} for t, n in events]}

# --- Materials ---
work = os.path.join(out_dir, "_work")
os.makedirs(work, exist_ok=True)
rifle.material_slots[0].material = make_ak_material(ak_dir, work)
skin = next(o for o in bpy.data.objects if o.type == "MESH" and o is not rifle)
skin.name = "arms"
rifle.name = "ak47"

def report(pose):
    """Where each hand's parts and the magazine end up, in the rifle's space, to
    check a grip against the measured model."""
    arms.animation_data.action = None
    r.mag_offset = None
    r.apply(pose)
    inv = pose.place.matrix().inverted()
    bones = arms.pose.bones
    for side in ("l", "r"):
        for name in ("wrist", "socket", "finger_index1", "finger_middle1", "finger_pinky1", "finger_thumb1"):
            print(f"HAND {side} {name:15s} head {tuple(round(c, 3) for c in inv @ bones[f'{name}.{side}'].head)}")
        for f in ("index", "middle", "ring", "pinky", "thumb"):
            print(f"HAND {side} {f:6s} tip {tuple(round(c, 3) for c in inv @ bones[f'finger_{f}3.{side}'].tail)}")
        print(f"HAND {side} elbow          {tuple(round(c, 3) for c in inv @ bones[f'forearm.{side}'].head)}")
    print(f"MAG head {tuple(round(c, 3) for c in inv @ bones['MAGAZINE'].head)}")


if "--joints" in args:
    # --joints: each clip's worst wrist, against what a wrist can do (twist about
    # +-80 from neutral, bend about +-70, sideways -30..+20).
    for name, length, fn, _, _ in CLIPS:
        worst = {}
        for i in range(int(length / 0.05) + 1):
            t = min(i * 0.05, length)
            r.mag_offset = None
            if i == 0:
                r.elbows = {}
            r.apply(fn(t))
            for side in ("r", "l"):
                tw, fl, dv, carried = rig.wrist_angles(arms, side)
                for key, v in (("twist", tw), ("flex", fl), ("dev", dv)):
                    k = f"{side} {key}"
                    if k not in worst or abs(v) > abs(worst[k][0]):
                        worst[k] = (v, t, carried if key == "twist" else None)
        print("JOINTS", name, "  ".join(f"{k} {v:6.1f}@{t:.2f}" + (f"(arm {c:5.1f})" if c is not None else "")
                                        for k, (v, t, c) in sorted(worst.items())))

if "--report" in args:
    # --report [clip@time]: the hold at the hip, or a clip's pose at a moment.
    at = args[args.index("--report") + 1] if args.index("--report") + 1 < len(args) else ""
    if "@" in at:
        clip, t = at.split("@")
        fn = {name: f for name, _, f, _, _ in CLIPS}[clip]
        if clip.startswith("reload"):
            fn(0.56)  # The magazine is taken at the grab; pose that first.
            r.apply(fn(0.56))
        report(fn(float(t)))
    else:
        report(held(HIP))

if preview:
    # The game draws the view model 42 degrees tall; at 16:9 on Blender's 36 mm
    # sensor that's this lens.
    EYE_LENS = 18.0 / math.tan(math.atan(math.tan(math.radians(21.0)) * 16.0 / 9.0))
    # Each clip at a moment, from the eye and from around the hands.
    hand_l = HIP.matrix() @ HANDGUARD.socket
    hand_r = HIP.matrix() @ GRIP.socket
    shots = {
        "idle": (0.0, {
            "eye": ((0.0, 0.0, 0.0), (0.0, 1.0, 0.0), EYE_LENS),
            "right": ((0.9, 0.25, -0.2), (0.1, 0.25, -0.2), 35.0),
            "left": ((-0.9, 0.25, -0.2), (0.1, 0.25, -0.2), 35.0),
            "below": ((0.15, 0.3, -1.0), (0.1, 0.3, -0.2), 35.0),
            "lefthand": (tuple(hand_l + Vector((-0.28, -0.05, -0.08))), tuple(hand_l), 35.0),
            "lefthand_under": (tuple(hand_l + Vector((0.0, 0.0, -0.3))), tuple(hand_l), 35.0),
            "lefthand_right": (tuple(hand_l + Vector((0.28, -0.05, 0.02))), tuple(hand_l), 35.0),
            "righthand": (tuple(hand_r + Vector((0.28, -0.05, 0.0))), tuple(hand_r), 35.0),
            "righthand_left": (tuple(hand_r + Vector((-0.28, 0.05, 0.05))), tuple(hand_r), 35.0),
        }),
        "aim": (0.0, {"aim_eye": ((0.0, 0.0, 0.0), (0.0, 1.0, 0.0), EYE_LENS)}),
    }
    # The moving clips, from the eye and from a little way off to the right.
    side = ((0.75, 0.0, 0.1), (0.05, 0.3, -0.2), 30.0)
    for clip, times in {"draw": (0.15, 0.4), "reload": (0.3, 0.55, 0.7, 1.1, 1.5, 1.72, 2.0),
                        "reload_empty": (2.1, 2.27, 2.45), "sprint": (0.15, 0.45), "inspect": (1.2, 3.0)}.items():
        for t in times:
            shots[f"{clip}@{t}"] = (t, {f"{clip}@{t}_eye": ((0.0, 0.0, 0.0), (0.0, 1.0, 0.0), EYE_LENS),
                                        f"{clip}@{t}_side": side})
    # Close on the magazine as it's taken, from beside and from the gun's far side.
    mag_at = RELOAD_TILT.matrix() @ Vector((0.0, 0.2, 0.0))
    shots["reload@0.6"] = (0.6, {"magclose_near": (tuple(RELOAD_TILT.matrix() @ Vector((-0.25, 0.25, 0.05))),
                                                   tuple(mag_at), 30.0),
                                 "magclose_far": (tuple(RELOAD_TILT.matrix() @ Vector((0.25, 0.2, -0.05))),
                                                  tuple(mag_at), 30.0)})
    for key, (t, views) in shots.items():
        arms.animation_data.action = bpy.data.actions[key.split("@")[0]]
        bpy.context.scene.frame_set(round(t * FPS))
        rig.render_views(preview, views)
    arms.animation_data.action = None

# --- Export: the deforming skeleton, sampled from the rig every frame ---
bpy.context.scene.render.fps = FPS
bpy.ops.object.select_all(action="DESELECT")
for o in (arms, skin, rifle):
    o.select_set(True)
bpy.ops.export_scene.gltf(
    filepath=out_path, export_format="GLTF_SEPARATE", use_selection=True, export_yup=True,
    export_apply=False, export_tangents=True, export_skins=True, export_def_bones=True,
    export_animations=True, export_animation_mode="NLA_TRACKS", export_force_sampling=True,
    export_frame_step=1, export_optimize_animation_size=False, export_image_format="JPEG",
    export_image_quality=92)
with open(os.path.splitext(out_path)[0] + ".events.json", "w", encoding="utf-8") as f:
    json.dump({"fps": FPS, "clips": EVENTS}, f, indent=2)
for name in os.listdir(work):
    os.remove(os.path.join(work, name))
os.rmdir(work)
print("wrote", out_path)
