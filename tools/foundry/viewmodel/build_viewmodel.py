"""Builds one of Foundry's first-person view models: the WRAD arms (CC0) holding a
gun from Stein Games' "Free Classic Weapons Pack" (CC0), posed and animated in
Blender, exported as one skinned glTF.

    blender -b --disable-autoexec --factory-startup <wrad>/arms.blend \
        --python build_viewmodel.py -- <gun> <WeaponsPack dir> <out.gltf> \
        [--preview <prefix>] [--only idle,aim] [--report [clip@time]] [--joints]
        [--save <gun>.blend]

<gun> names a module in weapons/ (ak47, m1911, m700): how the gun is imported, how
it's held, how its parts move and its clips. Poses are set on the arms' control
rig (wrist IK targets, elbow poles), which follows the gun; the hands then fit
themselves onto the gun's surface, fingers closed round it (rig.grasp). The
exporter samples the rig, IK and all, into the deforming bones. Each clip is its
own glTF animation, and a sidecar <out>.events.json lists the moments sounds hang
on (magazine out, bolt back...).

--save writes the finished scene as a .blend (textures packed in) to look at and
edit by hand; export_blend.py exports it back to the game.
"""
import importlib
import math
import os
import sys

import bpy
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import rig  # noqa: E402
import export  # noqa: E402
from clips import FPS, Rig, bake_clip  # noqa: E402
from weapon_material import make_weapon_material  # noqa: E402

args = sys.argv[sys.argv.index("--") + 1:]
gun_name, pack, out_path = args[0], args[1], args[2]
gun_def = importlib.import_module(f"weapons.{gun_name}")
preview = args[args.index("--preview") + 1] if "--preview" in args else None
out_dir = os.path.dirname(os.path.abspath(out_path))
os.makedirs(out_dir, exist_ok=True)

arms = bpy.data.objects["arms"]
rig.to_metres_at_eye(arms, bpy.data.objects["Camera"])
skeleton, gun = gun_def.load(pack)
rig.merge_rifle(arms, skeleton, gun)
rig.setup_forearm_twist(arms)
rest = {s: rig.hand_rest(arms, s) for s in ("r", "l")}
for o in list(bpy.data.objects):
    if o.type == "CAMERA":
        bpy.data.objects.remove(o, do_unlink=True)

r = Rig(arms, rest, gun_def.POLES, gun, gun_def)
CLIPS = gun_def.CLIPS
EVENTS = {}

# --only idle,aim: bake just those clips (for quick looks at a hold).
only = args[args.index("--only") + 1].split(",") if "--only" in args else None
for name, length, fn, loop, events in CLIPS:
    if only and name not in only:
        continue
    bake_clip(r, name, length, fn, loop=loop)
    EVENTS[name] = {"loop": loop, "events": [{"time": t, "name": n} for t, n in events]}

# --- Materials: the gun's own maps on its first slot (a module may add more) ---
work = os.path.join(out_dir, "_work")
os.makedirs(work, exist_ok=True)
gun.material_slots[0].material = make_weapon_material(
    os.path.join(pack, os.path.dirname(gun_def.FBX)), work, name=gun_def.NAME, **gun_def.TEXTURES)
skin = next(o for o in bpy.data.objects if o.type == "MESH" and o is not gun)
skin.name = "arms"
gun.name = gun_def.NAME


def report(pose):
    """Where each hand's parts and the magazine end up, in the gun's space, to
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
        shoulder = bones[f"bicep.{side}"].head
        reach = (bones[f"wrist.{side}"].head - shoulder).length
        arm = bones[f"bicep.{side}"].length + bones[f"forearm.{side}"].length
        print(f"HAND {side} reach {reach:.3f} of {arm:.3f} ({100 * reach / arm:.0f}%), shoulder at "
              f"{tuple(round(c, 3) for c in shoulder)} in the eye's space")
    if getattr(gun_def, "MAGAZINE", None):
        print(f"MAG head {tuple(round(c, 3) for c in inv @ bones[gun_def.MAGAZINE].head)}")


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
                r.grips = {"r": {}, "l": {}}
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
        grab = getattr(gun_def, "GRAB_TIME", None)
        if clip.startswith("reload") and grab is not None:
            r.apply(fn(grab))  # The magazine is taken at the grab; pose that first.
        report(fn(float(t)))
    else:
        report(gun_def.held(gun_def.HIP))

if preview:
    # The game draws the view model 42 degrees tall; at 16:9 on Blender's 36 mm
    # sensor that's this lens.
    EYE_LENS = 18.0 / math.tan(math.atan(math.tan(math.radians(21.0)) * 16.0 / 9.0))
    HIP, AIM = gun_def.HIP, gun_def.AIM
    # Each clip at a moment, from the eye and from around the hands.
    hand_l = HIP.matrix() @ gun_def.SUPPORT.socket
    hand_r = HIP.matrix() @ gun_def.GRIP.socket
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
        "aim": (0.0, {
            "aim_eye": ((0.0, 0.0, 0.0), (0.0, 1.0, 0.0), EYE_LENS),
            "aim_left": ((-0.8, 0.3, -0.15), (0.0, 0.3, -0.2), 35.0),
            "aim_below": ((0.0, 0.35, -0.9), (0.0, 0.35, -0.2), 35.0),
            "aim_hand": (tuple(AIM.matrix() @ (gun_def.SUPPORT.socket + Vector((-0.25, -0.05, -0.05)))),
                         tuple(AIM.matrix() @ gun_def.SUPPORT.socket), 35.0),
            "aim_grip": (tuple(AIM.matrix() @ (gun_def.GRIP.socket + Vector((0.25, 0.0, 0.0)))),
                         tuple(AIM.matrix() @ gun_def.GRIP.socket), 35.0),
        }),
    }
    # Close round the firing grip: from either side, below and in front.
    grip_at = HIP.matrix() @ gun_def.GRIP.socket
    for name, off in (("grip_left", (-0.3, 0.02, 0.03)), ("grip_right", (0.3, 0.02, 0.03)),
                      ("grip_below", (0.03, 0.05, -0.3)), ("grip_front", (0.05, 0.3, 0.05))):
        shots["idle"][1][name] = (tuple(grip_at + HIP.matrix().to_3x3() @ Vector(off)), tuple(grip_at), 40.0)
    # The moving clips, from the eye and from a little way off to the right.
    side = ((0.75, 0.0, 0.1), (0.05, 0.3, -0.2), 30.0)
    for clip, times in gun_def.PREVIEW.items():
        for t in times:
            shots[f"{clip}@{t}"] = (t, {f"{clip}@{t}_eye": ((0.0, 0.0, 0.0), (0.0, 1.0, 0.0), EYE_LENS),
                                        f"{clip}@{t}_side": side})
    shots.update(getattr(gun_def, "EXTRA_SHOTS", {}))
    for key, (t, views) in shots.items():
        if key.split("@")[0] not in bpy.data.actions:
            continue
        arms.animation_data.action = bpy.data.actions[key.split("@")[0]]
        bpy.context.scene.frame_set(round(t * FPS))
        rig.render_views(preview, views)
    arms.animation_data.action = None

# --- Export: the deforming skeleton, sampled from the rig every frame ---
export.remember(gun_def.NAME, EVENTS)
if "--save" in args:
    # The scene as built, to open and edit: clips on their NLA tracks, the first
    # one active so it plays; textures packed so the file stands alone.
    blend = os.path.abspath(args[args.index("--save") + 1])
    bpy.context.scene.render.fps = FPS
    bpy.ops.file.pack_all()
    first = next(iter(EVENTS), None)
    if first:
        arms.animation_data.action = bpy.data.actions[first]
        bpy.context.scene.frame_end = int(bpy.data.actions[first].frame_range[1])
    bpy.ops.wm.save_as_mainfile(filepath=blend, compress=True)
    arms.animation_data.action = None
    print("saved", blend)
export.export(arms, skin, gun, out_path, EVENTS)
for name in os.listdir(work):
    os.remove(os.path.join(work, name))
os.rmdir(work)
