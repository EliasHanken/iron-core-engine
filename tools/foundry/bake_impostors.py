"""Bakes a real plant model into an impostor atlas: the way AAA games draw far
trees and dense grass. The model is rendered from `views` angles round it, its
albedo only (the game lights it), on a clear background, side by side in one
image. The game draws a card facing the camera and picks the view nearest the
angle it's seen from.

    blender -b --python bake_impostors.py -- <model.gltf> <out.png> <views> <frame px wide> <frame px high> [crop] [shaded] [only=<node>] [alpha=<material>:<mask image>]

Used on Poly Haven's fir_tree_01 and grass_medium_01 (CC0), far too heavy to draw
as they are (the fir is 7.8 million triangles).
"""
import math
import sys

import bpy
import numpy as np
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
source, out = argv[0], argv[1]
views, fw, fh = int(argv[2]), int(argv[3]), int(argv[4])
# "crop": frame a slice the model's height tall (a long, low patch of grass becomes a clump).
crop = "crop" in argv[5:]
# "shaded": lit by an even white sky in Cycles, so the colour carries its own
# occlusion (inner branches darker), not albedo alone.
shaded = "shaded" in argv[5:]
# "only=<node>": one model out of a file holding several (fir_tree_01 has three trees).
only = next((a.split("=", 1)[1] for a in argv[5:] if a.startswith("only=")), None)
# "alpha=<material>:<image>": a cutout mask the glTF doesn't carry (fir_tree_01's twigs).
masks = dict(a.split("=", 1)[1].split(":", 1) for a in argv[5:] if a.startswith("alpha="))

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=source)
if only:
    for o in list(bpy.context.scene.objects):
        if o.type == "MESH" and o.name != only:
            bpy.data.objects.remove(o, do_unlink=True)
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]

# All of it under one empty, to turn it round.
pivot = bpy.data.objects.new("pivot", None)
bpy.context.scene.collection.objects.link(pivot)
lo = Vector((1e9, 1e9, 1e9))
hi = Vector((-1e9, -1e9, -1e9))
for o in meshes:
    for c in o.bound_box:
        w = o.matrix_world @ Vector(c)
        lo = Vector(map(min, lo, w))
        hi = Vector(map(max, hi, w))
centre = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z))
pivot.location = centre
bpy.context.view_layer.update()  # So the pivot's matrix is where it now stands.
for o in meshes:
    if o.parent is None:
        o.parent = pivot
        o.matrix_parent_inverse = pivot.matrix_world.inverted()
height = hi.z - lo.z
width = max(hi.x - lo.x, hi.y - lo.y)
print(f"model {height:.2f} m tall, {width:.2f} m across, {sum(len(o.data.polygons) for o in meshes)} faces")

# Albedo only: each material's base colour, as light, so the render is its colour.
for mat in bpy.data.materials:
    if not mat.use_nodes:
        continue
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    bsdf = next((n for n in nodes if n.type == "BSDF_PRINCIPLED"), None)
    output = next((n for n in nodes if n.type == "OUTPUT_MATERIAL"), None)
    if not bsdf or not output:
        continue
    if shaded:
        if mat.name in masks:
            mask = nodes.new("ShaderNodeTexImage")
            mask.image = bpy.data.images.load(masks[mat.name])
            mask.image.colorspace_settings.name = "Non-Color"
            base = bsdf.inputs["Base Color"]
            source = base.links[0].from_node if base.is_linked else None
            if source is not None and "Vector" in source.inputs and source.inputs["Vector"].is_linked:
                links.new(source.inputs["Vector"].links[0].from_socket, mask.inputs["Vector"])
            links.new(mask.outputs["Color"], bsdf.inputs["Alpha"])
        continue
    emit = nodes.new("ShaderNodeEmission")
    base = bsdf.inputs["Base Color"]
    if base.is_linked:
        links.new(base.links[0].from_socket, emit.inputs["Color"])
    else:
        emit.inputs["Color"].default_value = base.default_value
    alpha = bsdf.inputs["Alpha"]
    if mat.name in masks:
        mask = nodes.new("ShaderNodeTexImage")
        mask.image = bpy.data.images.load(masks[mat.name])
        mask.image.colorspace_settings.name = "Non-Color"
        source = base.links[0].from_node if base.is_linked else None
        if source is not None and "Vector" in source.inputs and source.inputs["Vector"].is_linked:
            links.new(source.inputs["Vector"].links[0].from_socket, mask.inputs["Vector"])
        links.new(mask.outputs["Color"], alpha)
    if alpha.is_linked:
        mix = nodes.new("ShaderNodeMixShader")
        clear = nodes.new("ShaderNodeBsdfTransparent")
        links.new(alpha.links[0].from_socket, mix.inputs["Fac"])
        links.new(clear.outputs[0], mix.inputs[1])
        links.new(emit.outputs[0], mix.inputs[2])
        links.new(mix.outputs[0], output.inputs["Surface"])
    else:
        links.new(emit.outputs[0], output.inputs["Surface"])

scene = bpy.context.scene
for engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
    try:
        scene.render.engine = engine
        break
    except TypeError:
        continue
scene.render.film_transparent = True
scene.render.resolution_x, scene.render.resolution_y = fw, fh
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGBA"
scene.view_settings.view_transform = "Standard"
world = bpy.data.worlds.new("none")
world.color = (1.0, 1.0, 1.0) if shaded else (0, 0, 0)
scene.world = world
if shaded:
    world.use_nodes = True
    background = world.node_tree.nodes.get("Background")
    background.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    background.inputs["Strength"].default_value = 1.0
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 48
    scene.cycles.use_denoising = True
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        prefs.compute_device_type = "OPTIX"
        prefs.get_devices()
        for d in prefs.devices:
            d.use = True
        scene.cycles.device = "GPU"
    except Exception as e:  # No GPU: the CPU, slowly.
        print("cycles on the CPU:", e)

# Side on, orthographic, the frame fitting the model's height (and width at the frame's shape).
cam_data = bpy.data.cameras.new("cam")
cam_data.type = "ORTHO"
aspect = fw / fh
# Blender's orthographic scale is the frame's longer side, in metres.
if crop:
    frame_h = height * 1.04
    cam_data.ortho_scale = frame_h * max(aspect, 1.0)
elif aspect <= 1.0:
    cam_data.ortho_scale = max(height * 1.02, width * 1.04 / aspect)
    frame_h = cam_data.ortho_scale
else:
    cam_data.ortho_scale = max(width * 1.04, height * 1.02 * aspect)
    frame_h = cam_data.ortho_scale / aspect
frame_w = frame_h * aspect
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
cam.location = Vector((centre.x, centre.y - 100.0, centre.z + frame_h * 0.5))
cam.rotation_euler = (math.radians(90), 0, 0)
cam_data.clip_end = 500.0

atlas = np.zeros((fh, fw * views, 4), np.float32)
for i in range(views):
    pivot.rotation_euler = (0, 0, 2 * math.pi * i / views)
    scene.render.filepath = out + f".view{i}.png"
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(scene.render.filepath)
    px = np.array(img.pixels[:], np.float32).reshape(fh, fw, 4)
    atlas[:, i * fw:(i + 1) * fw] = px
    bpy.data.images.remove(img)

# Colour bleeds out into the clear pixels round each leaf, so filtering at a
# distance doesn't pull in a dark fringe.
rgb, a = atlas[..., :3], atlas[..., 3:]
filled = rgb * (a > 0.5)
weight = (a > 0.5).astype(np.float32)
for _ in range(8):
    nb = sum(np.roll(np.roll(filled, dy, 0), dx, 1) for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)))
    nw = sum(np.roll(np.roll(weight, dy, 0), dx, 1) for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)))
    grow = (weight == 0) & (nw > 0)
    filled = np.where(grow, nb / np.maximum(nw, 1e-6), filled)
    weight = np.maximum(weight, grow.astype(np.float32))
atlas[..., :3] = filled
image = bpy.data.images.new("atlas", fw * views, fh, alpha=True)
image.pixels[:] = atlas.ravel()
image.filepath_raw = out
image.file_format = "PNG"
image.save()
# What the game needs to size its card: each view's frame in metres, the model's
# base on the frame's bottom edge, centred across it.
import json

with open(out.rsplit(".", 1)[0] + ".json", "w", encoding="utf-8") as f:
    json.dump({"views": views, "frameWidth": round(frame_w, 3), "frameHeight": round(frame_h, 3),
               "modelHeight": round(height, 3)}, f)
print(f"wrote {out}: {views} views of {fw}x{fh} ({frame_w:.2f} x {frame_h:.2f} m), the model {height:.2f} m tall")
