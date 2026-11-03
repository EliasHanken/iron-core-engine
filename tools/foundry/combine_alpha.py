"""Puts a separate alpha mask into a colour texture's alpha channel, as one PNG:
for Poly Haven plants whose cutout comes as its own map (grass_medium_01's Alpha,
used with its dry_diff colour for a winter yard).

    blender -b --python combine_alpha.py -- <colour image> <mask image> <out.png>
"""
import sys

import bpy
import numpy as np

colour_path, mask_path, out = sys.argv[sys.argv.index("--") + 1:][:3]
colour = bpy.data.images.load(colour_path)
mask = bpy.data.images.load(mask_path)
mask.colorspace_settings.name = "Non-Color"
if tuple(mask.size) != tuple(colour.size):
    mask.scale(*colour.size)
w, h = colour.size
rgba = np.array(colour.pixels[:], np.float32).reshape(h, w, 4)
rgba[..., 3] = np.array(mask.pixels[:], np.float32).reshape(h, w, 4)[..., 0]
image = bpy.data.images.new("combined", w, h, alpha=True)
image.colorspace_settings.name = colour.colorspace_settings.name
image.pixels[:] = rgba.ravel()
image.filepath_raw = out
image.file_format = "PNG"
image.save()
print(f"wrote {out} ({w}x{h})")
