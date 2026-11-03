"""Writes a view model to the game: the arms' skeleton (sampled from the control
rig every frame, IK and all), its skin and the gun, as a glTF with one animation
per clip, and the <out>.events.json sidecar the game hangs sounds on.

Shared by build_viewmodel.py (straight after a build) and export_blend.py (from a
.blend saved with --save and edited by hand).
"""
import json
import os

import bpy

FPS = 30
SCENE_KEY = "foundry_viewmodel"  # On the scene: {"gun": name, "clips": events}, for re-export.


def remember(gun_name, events):
    """Stores what export needs on the scene, so a saved .blend can be exported again."""
    bpy.context.scene[SCENE_KEY] = json.dumps({"gun": gun_name, "clips": events})


def recall():
    """What remember() stored: (gun name, clips' events)."""
    data = json.loads(bpy.context.scene[SCENE_KEY])
    return data["gun"], data["clips"]


def export(arms, skin, gun, out_path, events):
    bpy.context.scene.render.fps = FPS
    if arms.animation_data is not None:
        arms.animation_data.action = None  # Each clip comes from its own NLA track.
    bpy.ops.object.select_all(action="DESELECT")
    for o in (arms, skin, gun):
        o.select_set(True)
    bpy.ops.export_scene.gltf(
        filepath=out_path, export_format="GLTF_SEPARATE", use_selection=True, export_yup=True,
        export_apply=False, export_tangents=True, export_skins=True, export_def_bones=True,
        export_animations=True, export_animation_mode="NLA_TRACKS", export_force_sampling=True,
        export_frame_step=1, export_optimize_animation_size=False, export_image_format="JPEG",
        export_image_quality=92)
    with open(os.path.splitext(out_path)[0] + ".events.json", "w", encoding="utf-8") as f:
        json.dump({"fps": FPS, "clips": events}, f, indent=2)
    print("wrote", out_path)
