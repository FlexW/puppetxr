# Builds the built-in mesh prop models (docs/props.md, Models) and exports each as
# models/<name>/<name>.glb next to its models/<name>/<name>.blend:
#
#   blender -b --factory-startup --python tools/blender/puppetxr_models.py -- models [name ...]
#
# Conventions: metres, the origin on the floor under the bounding box centre, the front towards
# Blender's -Y (glTF's +Z), materials of a base colour only, no textures, so the files stay small.

import math
import os
import sys

import bmesh
import bpy
from mathutils import Matrix


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def material(name, rgb):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (rgb[0], rgb[1], rgb[2], 1.0)
    bsdf.inputs["Roughness"].default_value = 0.6
    return m


def link(obj, mat):
    obj.data.materials.append(mat)
    bpy.context.collection.objects.link(obj)
    return obj


def box(name, size, center, mat, bevel=0.0, segments=2):
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co.x = v.co.x * size[0] + center[0]
        v.co.y = v.co.y * size[1] + center[1]
        v.co.z = v.co.z * size[2] + center[2]
    if bevel > 0.0:
        bmesh.ops.bevel(bm, geom=bm.edges[:] + bm.verts[:], offset=bevel, segments=segments, affect="EDGES", profile=0.5)
    bm.to_mesh(mesh)
    bm.free()
    return link(bpy.data.objects.new(name, mesh), mat)


def cylinder(name, radius, depth, center, mat, segments=12, axis="Z", radius_top=None, spin=0.0):
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    top = radius if radius_top is None else radius_top
    bmesh.ops.create_cone(bm, cap_ends=True, segments=segments, radius1=radius, radius2=top, depth=depth)
    bmesh.ops.rotate(bm, verts=bm.verts, matrix=Matrix.Rotation(spin, 3, "Z"))
    if axis == "X":
        bmesh.ops.rotate(bm, verts=bm.verts, matrix=Matrix.Rotation(math.pi / 2, 3, "Y"))
    elif axis == "Y":
        bmesh.ops.rotate(bm, verts=bm.verts, matrix=Matrix.Rotation(math.pi / 2, 3, "X"))
    bmesh.ops.translate(bm, verts=bm.verts, vec=center)
    bm.to_mesh(mesh)
    bm.free()
    return link(bpy.data.objects.new(name, mesh), mat)


def chair():
    wood = material("wood", (0.42, 0.26, 0.13))
    seat = material("seat", (0.10, 0.22, 0.30))
    h = 0.45
    box("seat", (0.44, 0.42, 0.05), (0.0, 0.0, h - 0.025), seat, bevel=0.012, segments=3)
    for x in (-0.19, 0.19):
        for y in (-0.17, 0.17):
            cylinder("leg", 0.018, h - 0.05, (x, y, (h - 0.05) / 2), wood, segments=12, radius_top=0.022)
    # Back posts continue the rear legs, the back rest spans them
    for x in (-0.19, 0.19):
        cylinder("post", 0.016, 0.42, (x, 0.19, h + 0.21), wood, segments=12)
    box("back", (0.42, 0.03, 0.16), (0.0, 0.195, h + 0.33), seat, bevel=0.01, segments=3)
    box("rail", (0.38, 0.02, 0.03), (0.0, 0.19, h + 0.12), wood, bevel=0.005, segments=2)
    for y in (-0.17, 0.17):
        box("stretcher", (0.36, 0.015, 0.02), (0.0, y, 0.15), wood)


def pencil():
    # Lying along X on the floor, its hexagon flat on the ground
    paint = material("paint", (0.85, 0.60, 0.05))
    wood = material("wood", (0.80, 0.62, 0.42))
    lead = material("lead", (0.08, 0.08, 0.08))
    metal = material("ferrule", (0.70, 0.70, 0.66))
    eraser = material("eraser", (0.85, 0.45, 0.50))
    r = 0.0035  # across the corners: 7 mm
    z = r * math.cos(math.pi / 6)
    body = 0.150
    cylinder("body", r, body, (0.0, 0.0, z), paint, segments=6, axis="X", spin=math.pi / 6)
    tip = 0.018
    cylinder("tip", r, tip, (body / 2 + tip / 2, 0.0, z), wood, segments=6, axis="X", radius_top=0.0008, spin=math.pi / 6)
    cylinder("lead", 0.0008, 0.003, (body / 2 + tip + 0.0015, 0.0, z), lead, segments=6, axis="X", radius_top=0.0001)
    cylinder("ferrule", r * 0.85, 0.009, (-body / 2 - 0.0045, 0.0, z), metal, segments=12, axis="X")
    cylinder("eraser", r * 0.8, 0.006, (-body / 2 - 0.012, 0.0, z), eraser, segments=12, axis="X")


def case():
    # A small hard case, its handle at the front
    shell = material("shell", (0.12, 0.12, 0.13))
    seam = material("seam", (0.55, 0.55, 0.55))
    w, d, h = 0.22, 0.15, 0.06
    box("bottom", (w, d, h * 0.5 - 0.002), (0.0, 0.0, h * 0.25 - 0.001), shell, bevel=0.012, segments=3)
    box("lid", (w, d, h * 0.5 - 0.002), (0.0, 0.0, h * 0.75 + 0.001), shell, bevel=0.012, segments=3)
    box("seam", (w - 0.006, d - 0.006, 0.004), (0.0, 0.0, h * 0.5), seam)
    box("handle", (0.08, 0.012, 0.012), (0.0, -d / 2 - 0.012, h * 0.5), seam, bevel=0.004, segments=2)
    for x in (-0.03, 0.03):
        box("lug", (0.01, 0.012, 0.016), (x, -d / 2 - 0.004, h * 0.5), seam)


BUILDERS = {"case": case, "chair": chair, "pencil": pencil}


# The origin goes on the floor under the centre of the bounding box
def recenter():
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    points = [v.co for o in meshes for v in o.data.vertices]
    lo = [min(p[i] for p in points) for i in range(3)]
    hi = [max(p[i] for p in points) for i in range(3)]
    shift = (-(lo[0] + hi[0]) / 2, -(lo[1] + hi[1]) / 2, -lo[2])
    for o in meshes:
        for v in o.data.vertices:
            v.co.x += shift[0]
            v.co.y += shift[1]
            v.co.z += shift[2]


def export(name, out_dir):
    reset()
    BUILDERS[name]()
    recenter()
    directory = os.path.join(out_dir, name)
    os.makedirs(directory, exist_ok=True)
    blend = os.path.join(directory, name + ".blend")
    glb = os.path.join(directory, name + ".glb")
    # Only the scene's data, without the window manager, which remembers local directories
    scene = bpy.context.scene
    scene.render.filepath = "//"
    bpy.data.libraries.write(blend, {scene}, path_remap="RELATIVE_ALL", fake_user=True, compress=True)
    bpy.ops.export_scene.gltf(
        filepath=glb,
        export_format="GLB",
        export_yup=True,
        export_apply=True,
        export_texcoords=False,
        export_normals=False,
        export_animations=False,
        export_cameras=False,
        export_lights=False,
        export_extras=False,
        export_materials="EXPORT",
        export_copyright="",
    )
    triangles = sum(len(p.vertices) - 2 for o in bpy.data.objects if o.type == "MESH" for p in o.data.polygons)
    print("puppetxr_models: %s, %d triangles" % (glb, triangles))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv:
        print("usage: blender -b --factory-startup --python tools/blender/puppetxr_models.py -- <models dir> [name ...]")
        sys.exit(1)
    names = argv[1:] or sorted(BUILDERS)
    for name in names:
        export(name, argv[0])


main()
