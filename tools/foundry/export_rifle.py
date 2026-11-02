"""Exports the L85A1 from Tabasco's "Various Small Arms" (CC0, opengameart.org/node/3609)
as Foundry's first-person rifle.

    blender -b --disable-autoexec --factory-startup l85.blend --python export_rifle.py -- <out.gltf> [preview.png]

The source has no textures, only two flat materials, so they become PBR ones: a
parkerized steel and a black polymer. It's turned to look down -Z with +Y up, scaled
to the real rifle's 0.785 m, smoothed by angle, and its origin moved to the pistol
grip. Blender's own scripts in the file never run (--disable-autoexec).
"""
import math
import sys

import bpy
from mathutils import Matrix, Vector

LENGTH = 0.785  # An L85A1, muzzle to butt.

args = sys.argv[sys.argv.index("--") + 1:]
out_path = args[0]
preview = args[1] if len(args) > 1 else None

rifle = next(o for o in bpy.data.objects if o.type == "MESH")
for o in list(bpy.data.objects):
    if o is not rifle:
        bpy.data.objects.remove(o, do_unlink=True)
rifle.parent = None
bpy.context.view_layer.objects.active = rifle
rifle.select_set(True)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

mesh = rifle.data
verts = [v.co.copy() for v in mesh.vertices]
lo = Vector((min(v.x for v in verts), min(v.y for v in verts), min(v.z for v in verts)))
hi = Vector((max(v.x for v in verts), max(v.y for v in verts), max(v.z for v in verts)))
size = hi - lo
axis = max(range(3), key=lambda i: size[i])  # The barrel runs along the longest side.


def height_near(end):
    """How tall the rifle is within 6% of one end: the muzzle is the thin end."""
    band = size[axis] * 0.06
    zs = [v.z for v in verts if abs(v[axis] - end) < band]
    return (max(zs) - min(zs)) if zs else 0.0


muzzle_at_max = height_near(hi[axis]) < height_near(lo[axis])
print(f"long axis {'xyz'[axis]}, muzzle at {'max' if muzzle_at_max else 'min'}, size {tuple(round(s, 3) for s in size)}")

# Blender is Z-up and glTF Y-up; the exporter turns Blender's -Y into glTF's +Z. So
# the muzzle points along Blender +Y here, which becomes -Z in the game.
turn = Matrix.Identity(4)
if axis == 0:
    turn = Matrix.Rotation(math.radians(90 if muzzle_at_max else -90), 4, "Z")
elif axis == 1 and not muzzle_at_max:
    turn = Matrix.Rotation(math.radians(180), 4, "Z")
scale = LENGTH / size[axis]
mesh.transform(Matrix.Scale(scale, 4) @ turn)
mesh.update()

# The pistol grip. A bullpup's magazine sits behind it and hangs lower, so look only
# where the grip is: 36-52 % of the way from the butt.
verts = [v.co.copy() for v in mesh.vertices]
y_lo = min(v.y for v in verts)
y_hi = max(v.y for v in verts)


def band(a, b):
    return [v for v in verts if y_lo + (y_hi - y_lo) * a <= v.y <= y_lo + (y_hi - y_lo) * b]


grip = min(band(0.36, 0.52), key=lambda v: v.z)
origin = Vector(((min(v.x for v in verts) + max(v.x for v in verts)) * 0.5, grip.y, grip.z))  # Centred side to side.
mesh.transform(Matrix.Translation(-origin))
verts = [v.co.copy() for v in mesh.vertices]
y_lo -= grip.y
y_hi -= grip.y
# The sights, for lining the eye up: the rear aperture on the carry handle, and the post.
rear = max(band(0.22, 0.45), key=lambda v: v.z)
front = max(band(0.70, 0.84), key=lambda v: v.z)
print(f"butt {y_lo:.3f}, muzzle {y_hi:.3f} ahead of the grip")
print(f"rear sight top y {rear.y:.3f} z {rear.z:.3f}; front sight top y {front.y:.3f} z {front.z:.3f}")

# Materials: the darker one is the steel, the other the polymer furniture. The file's
# own materials predate Blender's node system, so each slot gets a new one.
slots = sorted(rifle.material_slots, key=lambda s: sum(s.material.diffuse_color[:3]))
looks = [("steel", (0.035, 0.036, 0.038), 0.8, 0.42), ("polymer", (0.045, 0.047, 0.042), 0.0, 0.62),
         ("trim", (0.02, 0.02, 0.02), 0.6, 0.5)]
for slot, (name, color, metal, rough) in zip(slots, looks):
    m = bpy.data.materials.new(name)
    if hasattr(m, "use_nodes"):
        m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metal
    bsdf.inputs["Roughness"].default_value = rough
    slot.material = m

# Hard edges stay hard and curved surfaces read as curved.
bpy.ops.object.shade_auto_smooth(angle=math.radians(35))

bpy.ops.export_scene.gltf(filepath=out_path, export_format="GLTF_SEPARATE", use_selection=True,
                          export_apply=True, export_tangents=True, export_animations=False,
                          export_skins=False, export_yup=True)
print("wrote", out_path)

if preview:
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "MATERIAL"
    scene.render.resolution_x, scene.render.resolution_y = 1200, 500
    cam_data = bpy.data.cameras.new("cam")
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    cam.location = (1.1, 0.12, 0.1)
    cam.rotation_euler = (math.radians(90), 0, math.radians(90))
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = 0.95
    scene.camera = cam
    scene.render.filepath = preview
    bpy.ops.render.render(write_still=True)
