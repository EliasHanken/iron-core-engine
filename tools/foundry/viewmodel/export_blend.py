"""Exports a view model from a .blend saved by `build_viewmodel.py --save` and
edited by hand, to the game.

    blender -b <gun>.blend --python export_blend.py -- <out.gltf>

The .blend holds the arms with their control rig, the gun and one action per
clip (each on its own NLA track, keyed every frame on the rig: the wrist IK
targets, elbow poles, fingers, the gun's place and its moving parts). Edit the
keys, or add a layer on a track, and export; the clips' sound events come from
the build and are written beside the glTF as before. A later build overwrites
the hand edits: carry anything worth keeping back into weapons/<gun>.py.
"""
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export  # noqa: E402

out_path = sys.argv[sys.argv.index("--") + 1]
gun_name, events = export.recall()
arms = next(o for o in bpy.data.objects if o.type == "ARMATURE")
gun = bpy.data.objects[gun_name]
skin = next(o for o in bpy.data.objects if o.type == "MESH" and o is not gun)
export.export(arms, skin, gun, out_path, events)
