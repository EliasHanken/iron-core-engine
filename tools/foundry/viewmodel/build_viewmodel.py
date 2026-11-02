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
from clips import FPS, Hand, Place, Pose, Rig, bake_clip, ease_out, mix_hand, mix_place, smooth, track  # noqa: E402

args = sys.argv[sys.argv.index("--") + 1:]
ak_dir, out_path = args[0], args[1]
preview = args[args.index("--preview") + 1] if "--preview" in args else None
out_dir = os.path.dirname(os.path.abspath(out_path))
os.makedirs(out_dir, exist_ok=True)

arms = bpy.data.objects["arms"]
rig.to_metres_at_eye(arms, bpy.data.objects["Camera"])
skeleton, rifle = rig.import_rifle(os.path.join(ak_dir, "SKM_AK47.fbx"))
rig.merge_rifle(arms, skeleton, rifle)
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


CLIPS = [
    ("idle", 3.2, idle, True, []),
    ("aim", 3.2, aim, True, []),
    ("fire", 0.2, fire, False, []),
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

def report_hand(side, place):
    """Where the hand's parts end up, in the rifle's space, to check a grip."""
    arms.animation_data.action = None
    r.apply(held(place))
    inv = place.matrix().inverted()
    pose = arms.pose.bones
    for name in ("wrist", "socket", "finger_index1", "finger_middle1", "finger_pinky1", "finger_thumb1"):
        print(f"HAND {side} {name:15s} head {tuple(round(c, 3) for c in inv @ pose[f'{name}.{side}'].head)}")
    for f in ("index", "middle", "ring", "pinky", "thumb"):
        print(f"HAND {side} {f:6s} tip {tuple(round(c, 3) for c in inv @ pose[f'finger_{f}3.{side}'].tail)}")


if "--report" in args:
    report_hand("l", HIP)
    report_hand("r", HIP)

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
    for clip, (t, views) in shots.items():
        arms.animation_data.action = bpy.data.actions[clip]
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
