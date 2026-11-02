"""The AK-47's material for glTF, from Stein Games' textures (CC0).

The pack's RMAO (roughness, metal, AO) becomes glTF's ORM (AO, roughness, metal),
and its DirectX normal map gets its green channel flipped for glTF's OpenGL
convention. The repacked images are written to `work` for the exporter to pick up.
"""
import os

import bpy
import numpy as np


def make_ak_material(src, work):
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
    return mat
