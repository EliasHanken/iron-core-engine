"""A rifle scope, modelled in Blender from lathed profiles: eyepiece bell, a
one-inch tube with its turrets, the objective bell, two rings on the rail, and
glass at both ends. Joined into a gun's mesh, bound to its frame, so it moves with
the gun.

Space: the gun's own, +Y out of the muzzle, +Z up, metres.
"""
import math

import bmesh
import bpy
from mathutils import Matrix, Vector

import rig

# The scope's outline along its length from the eyepiece (0) forward: (u, radius).
PROFILE = [(0.0, 0.0175), (0.003, 0.0205), (0.058, 0.0205), (0.066, 0.019), (0.09, 0.0127), (0.215, 0.0127),
           (0.245, 0.0205), (0.318, 0.0215), (0.326, 0.0215), (0.33, 0.0195)]
LENGTH = PROFILE[-1][0]
SEGMENTS = 40


def flat_material(name, color, metallic, roughness):
    mat = bpy.data.materials.new(name)
    if hasattr(mat, "use_nodes"):
        mat.use_nodes = True
    bsdf = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    return mat


def _lathe(bm, profile, centre, axis_y=True, segments=SEGMENTS, cap_start=None, cap_end=None):
    """Rings of vertices round the scope's axis (along +Y through `centre`), or
    round +Z for a turret when `axis_y` is off, joined into a tube; capped flat at
    either end if asked (the cap's faces get material `cap_*`)."""
    rings = []
    for u, r in profile:
        ring = []
        for k in range(segments):
            a = 2.0 * math.pi * k / segments
            if axis_y:
                p = centre + Vector((r * math.cos(a), u, r * math.sin(a)))
            else:
                p = centre + Vector((r * math.cos(a), r * math.sin(a), u))
            ring.append(bm.verts.new(p))
        rings.append(ring)
    faces = []
    for r0, r1 in zip(rings, rings[1:]):
        for k in range(segments):
            k1 = (k + 1) % segments
            quad = (r0[k], r0[k1], r1[k1], r1[k]) if axis_y else (r0[k], r1[k], r1[k1], r0[k1])
            faces.append(bm.faces.new(quad))
    caps = []
    if cap_start is not None:
        f = bm.faces.new(list(reversed(rings[0])) if axis_y else rings[0])
        f.material_index = cap_start
        caps.append(f)
    if cap_end is not None:
        f = bm.faces.new(rings[-1] if axis_y else list(reversed(rings[-1])))
        f.material_index = cap_end
        caps.append(f)
    return faces


def _box(bm, lo, hi):
    verts = [bm.verts.new(Vector((x, y, z))) for x in (lo.x, hi.x) for y in (lo.y, hi.y) for z in (lo.z, hi.z)]
    for quad in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        bm.faces.new([verts[i] for i in quad])


def attach_scope(gun, mount, centre_y, height):
    """Builds the scope with its middle at `centre_y`, its axis `height` above the
    rail top at `mount` (gun space), and joins it into `gun`, bound to the FRAME
    bone. Returns the axis's height (z) in gun space, for aiming through it."""
    axis_z = mount.z + height
    rear = Vector((0.0, centre_y - LENGTH * 0.5, axis_z))
    bm = bmesh.new()
    # 0 the scope's body, 1 its glass.
    _lathe(bm, PROFILE, rear, cap_start=1, cap_end=1)
    # Turrets: elevation on top, windage on the right, with knurled caps.
    mid = rear + Vector((0.0, 0.152, 0.0))
    _lathe(bm, [(0.008, 0.0105), (0.012, 0.0105), (0.013, 0.0095), (0.026, 0.0095), (0.027, 0.008)],
           mid, axis_y=False, segments=24, cap_end=0)
    side = rear + Vector((0.0, 0.152, 0.0))
    turret = bm.verts[:]
    _lathe(bm, [(0.008, 0.0105), (0.012, 0.0105), (0.013, 0.0095), (0.024, 0.0095), (0.025, 0.008)],
           Vector(), axis_y=False, segments=24, cap_end=0)
    windage = [v for v in bm.verts if v not in turret]
    bmesh.ops.rotate(bm, verts=windage, cent=Vector(), matrix=Matrix.Rotation(math.pi / 2.0, 3, "Y"))
    bmesh.ops.translate(bm, verts=windage, vec=side)
    # Two rings round the tube, on bases down to the rail.
    for u in (0.105, 0.2):
        ring_at = rear + Vector((0.0, u, 0.0))
        _lathe(bm, [(-0.008, 0.0152), (0.008, 0.0152)], ring_at, cap_start=None)
        _box(bm, Vector((-0.011, ring_at.y - 0.008, mount.z)), Vector((0.011, ring_at.y + 0.008, axis_z - 0.011)))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    mesh = bpy.data.meshes.new("scope")
    bm.to_mesh(mesh)
    bm.free()
    scope = bpy.data.objects.new("scope", mesh)
    bpy.context.scene.collection.objects.link(scope)
    mesh.materials.append(flat_material("scope", (0.025, 0.026, 0.028), 0.55, 0.42))
    mesh.materials.append(flat_material("lens", (0.02, 0.035, 0.06), 1.0, 0.04))
    for p in mesh.polygons:
        p.use_smooth = True

    # Bound to the frame like the rest of the gun.
    group = scope.vertex_groups.new(name="FRAME")
    group.add(range(len(mesh.vertices)), 1.0, "REPLACE")
    armature = next(m.object for m in gun.modifiers if m.type == "ARMATURE")
    scope.modifiers.new("Armature", "ARMATURE").object = armature
    rig.select_only(gun, scope)
    bpy.ops.object.join()
    return axis_z
