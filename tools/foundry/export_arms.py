"""Exports the WRAD ARMS by wriks (CC0, github.com/wwwriks/wrad-arms) for Foundry.

    blender -b --disable-autoexec --factory-startup --python export_arms.py -- <arms.glb> <out.gltf>

Drops the two helper meshes that ship beside the arms, scales the rig to a real arm
(shoulder to wrist 0.56 m), applies that scale, and writes a glTF with its texture
as a separate file, which is what the engine's loader reads.
"""
import sys

import bpy

SHOULDER_TO_WRIST = 0.56

args = sys.argv[sys.argv.index("--") + 1:]
source, out_path = args[0], args[1]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=source)

armature = next(o for o in bpy.data.objects if o.type == "ARMATURE")
skinned = [o for o in bpy.data.objects if o.type == "MESH" and o.parent is armature]
for o in list(bpy.data.objects):
    if o is not armature and o not in skinned:
        bpy.data.objects.remove(o, do_unlink=True)

bones = armature.data.bones
reach = (bones["wrist.r"].head_local - bones["shoulder.r"].head_local).length
scale = SHOULDER_TO_WRIST / reach
print(f"shoulder to wrist {reach:.3f} units, scaling by {scale:.4f}")

bpy.ops.object.select_all(action="DESELECT")
for o in [armature, *skinned]:
    o.select_set(True)
bpy.context.view_layer.objects.active = armature
armature.scale = (armature.scale.x * scale, armature.scale.y * scale, armature.scale.z * scale)
bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

for name in ("shoulder.r", "bicep.r", "forearm.r", "wrist.r", "socket.r", "wrist.l", "socket.l"):
    b = armature.data.bones[name]
    print(f"{name:10} head {tuple(round(v, 3) for v in b.head_local)}")

bpy.ops.object.select_all(action="DESELECT")
for o in [armature, *skinned]:
    o.select_set(True)
bpy.ops.export_scene.gltf(filepath=out_path, export_format="GLTF_SEPARATE", use_selection=True,
                          export_skins=True, export_animations=False, export_tangents=True,
                          export_image_format="AUTO", export_yup=True)
print("wrote", out_path)
