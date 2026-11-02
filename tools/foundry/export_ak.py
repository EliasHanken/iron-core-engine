"""Exports the AK-47 from Stein Games' "Free Classic Weapons Pack" (CC0,
stein-indie.itch.io/classic-weapons-pack) as Foundry's first-person rifle.

    blender -b --disable-autoexec --factory-startup --python export_ak.py -- <AK47 dir> <out.gltf> [preview.png]

<AK47 dir> holds SKM_AK47.fbx and its three textures. The rifle is a skinned mesh
(the magazine, bolt and trigger have their own bones); it's exported at rest as a
plain mesh. It's turned to look down -Z with +Y up, scaled to the real rifle's
0.88 m, and its origin moved to the bottom of the pistol grip. The textures are
repacked for glTF and saved as JPEGs: the pack's RMAO (roughness, metal, AO) becomes ORM (AO,
roughness, metal), and its DirectX normal map gets its green channel flipped.
"""
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector

LENGTH = 0.88  # An AK-47 with its fixed stock, muzzle to butt.

args = sys.argv[sys.argv.index("--") + 1:]
src, out_path = args[0], args[1]
preview = args[2] if len(args) > 2 else None
work = os.path.join(os.path.dirname(os.path.abspath(out_path)), "_work")
os.makedirs(work, exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=os.path.join(src, "SKM_AK47.fbx"))

# The rifle is the big mesh; the small one is a scope rail the classic AK doesn't have.
rifle = max((o for o in bpy.data.objects if o.type == "MESH"), key=lambda o: len(o.data.polygons))
for o in list(bpy.data.objects):
    if o is not rifle:
        bpy.data.objects.remove(o, do_unlink=True)
for m in list(rifle.modifiers):
    rifle.modifiers.remove(m)
rifle.vertex_groups.clear()
bpy.context.view_layer.objects.active = rifle
rifle.select_set(True)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
mesh = rifle.data

# The rounds in the magazine have a material of their own and no textures; they're
# hidden inside it, so they go.
keep = next(i for i, s in enumerate(rifle.material_slots) if "BULLET" not in s.material.name.upper())
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="DESELECT")
bpy.ops.object.mode_set(mode="OBJECT")
for p in mesh.polygons:
    p.select = p.material_index != keep
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.delete(type="FACE")
bpy.ops.object.mode_set(mode="OBJECT")
while len(rifle.material_slots) > 1:
    rifle.active_material_index = 1 if keep == 0 else 0
    bpy.ops.object.material_slot_remove()
    keep = 0

# The muzzle points down Blender's -Y. The glTF exporter turns Blender's -Y into +Z,
# and the game wants the muzzle down -Z, so it turns half a circle.
verts = [v.co.copy() for v in mesh.vertices]
length = max(v.y for v in verts) - min(v.y for v in verts)
mesh.transform(Matrix.Scale(LENGTH / length, 4) @ Matrix.Rotation(math.pi, 4, "Z"))
mesh.update()
verts = [v.co.copy() for v in mesh.vertices]
y_lo, y_hi = min(v.y for v in verts), max(v.y for v in verts)


def band(a, b):
    return [v for v in verts if y_lo + (y_hi - y_lo) * a <= v.y <= y_lo + (y_hi - y_lo) * b]


# The pistol grip hangs behind the trigger guard and the magazine in front of it, at
# about a third of the way from the butt: the grip's foot is the lowest point there.
grip = min(band(0.26, 0.36), key=lambda v: v.z)
origin = Vector(((min(v.x for v in verts) + max(v.x for v in verts)) * 0.5, grip.y, grip.z))
mesh.transform(Matrix.Translation(-origin))
mesh.update()
verts = [v.co.copy() for v in mesh.vertices]
y_lo -= grip.y
y_hi -= grip.y
muzzle = [v for v in verts if v.y > y_hi - 0.01]
muzzle_c = sum((v for v in muzzle), Vector()) / len(muzzle)
rear = max(band(0.40, 0.55), key=lambda v: v.z)
front = max(band(0.88, 0.97), key=lambda v: v.z)
print(f"butt {y_lo:.3f}, muzzle {y_hi:.3f} ahead of the grip; muzzle centre height {muzzle_c.z:.3f}")
print(f"rear sight top y {rear.y:.3f} z {rear.z:.3f}; front sight top y {front.y:.3f} z {front.z:.3f}")
for name, lo, hi in [("handguard", 0.55, 0.75), ("magazine", 0.36, 0.48)]:
    vs = band(lo, hi)
    print(f"{name}: z {min(v.z for v in vs):.3f}..{max(v.z for v in vs):.3f}, "
          f"x {min(v.x for v in vs):.3f}..{max(v.x for v in vs):.3f}")


# Textures, repacked for glTF.
def load(name):
    img = bpy.data.images.load(os.path.join(src, name))
    img.colorspace_settings.name = "Non-Color"
    w, h = img.size
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    return img, px.reshape(h, w, 4)


def save(name, px, srgb=False):
    h, w = px.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=False)
    img.colorspace_settings.name = "sRGB" if srgb else "Non-Color"
    img.pixels.foreach_set(px.ravel())
    img.filepath_raw = os.path.join(work, name + ".png")
    img.file_format = "PNG"
    img.save()
    return img


_, rmao = load("T_AK47_RMAO.png")
orm = np.stack([rmao[..., 2], rmao[..., 0], rmao[..., 1], np.ones_like(rmao[..., 0])], axis=-1)
orm_img = save("ak47_orm", orm)
_, nrm = load("T_AK47_N.png")
nrm[..., 1] = 1.0 - nrm[..., 1]
nrm_img = save("ak47_normal", nrm)
color_img = bpy.data.images.load(os.path.join(src, "T_AK47_C.png"))
color_img.colorspace_settings.name = "sRGB"

mat = bpy.data.materials.new("ak47")
if hasattr(mat, "use_nodes"):
    mat.use_nodes = True
nodes, links = mat.node_tree.nodes, mat.node_tree.links
bsdf = next(n for n in nodes if n.type == "BSDF_PRINCIPLED")


def tex(img):
    n = nodes.new("ShaderNodeTexImage")
    n.image = img
    return n


links.new(tex(color_img).outputs["Color"], bsdf.inputs["Base Color"])
sep = nodes.new("ShaderNodeSeparateColor")
links.new(tex(orm_img).outputs["Color"], sep.inputs["Color"])
links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
nmap = nodes.new("ShaderNodeNormalMap")
links.new(tex(nrm_img).outputs["Color"], nmap.inputs["Color"])
links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
# The exporter picks up AO from a glTF Material Settings group's Occlusion input.
group = bpy.data.node_groups.new("glTF Material Output", "ShaderNodeTree")
group.interface.new_socket("Occlusion", in_out="INPUT", socket_type="NodeSocketFloat")
gnode = nodes.new("ShaderNodeGroup")
gnode.node_tree = group
links.new(sep.outputs["Red"], gnode.inputs["Occlusion"])
rifle.material_slots[0].material = mat

bpy.ops.export_scene.gltf(filepath=out_path, export_format="GLTF_SEPARATE", use_selection=True,
                          export_apply=True, export_tangents=True, export_animations=False,
                          export_skins=False, export_yup=True, export_image_format="JPEG", export_image_quality=92)
for f in os.listdir(work):
    os.remove(os.path.join(work, f))
os.rmdir(work)
print("wrote", out_path)

if preview:
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x, scene.render.resolution_y = 1400, 600
    world = bpy.data.worlds.new("w")
    world.color = (0.5, 0.5, 0.5)
    scene.world = world
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.rotation_euler = (math.radians(40), 0, math.radians(60))
    scene.collection.objects.link(sun)
    cam_data = bpy.data.cameras.new("cam")
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    cam.location = (1.2, (y_lo + y_hi) * 0.5, 0.08)
    cam.rotation_euler = (math.radians(90), 0, math.radians(90))
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = 1.0
    scene.camera = cam
    scene.render.filepath = preview
    bpy.ops.render.render(write_still=True)
