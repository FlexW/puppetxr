# Builds the built-in mesh prop models (docs/props.md, Models) and exports each as
# data/models/<name>/<name>.glb next to its data/models/<name>/<name>.blend:
#
#   blender -b --factory-startup --python tools/blender/puppetxr_models.py -- data/models [name ...]
#
# Conventions: metres, the origin on the floor under the bounding box centre, the front towards
# Blender's -Y (glTF's +Z), one material with a small texture. Passthrough draws a prop unlit, like
# the rooms' baked surfaces, so its light is baked too: a soft sky over a pale floor.

import math
import os
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def material(name, rgb, vary=0.0, scale=40.0, stretch=(1.0, 1.0, 1.0), bump=0.0):
    # A colour that varies by stretched noise: grain, weave, moulded plastic
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nodes, links = m.node_tree.nodes, m.node_tree.links
    bsdf = nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (rgb[0], rgb[1], rgb[2], 1.0)
    bsdf.inputs["Roughness"].default_value = 0.6
    if vary > 0.0 or bump > 0.0:
        geometry = nodes.new("ShaderNodeNewGeometry")
        mapping = nodes.new("ShaderNodeMapping")
        mapping.inputs["Scale"].default_value = stretch
        links.new(geometry.outputs["Position"], mapping.inputs["Vector"])
        noise = nodes.new("ShaderNodeTexNoise")
        noise.inputs["Scale"].default_value = scale
        noise.inputs["Detail"].default_value = 5.0
        links.new(mapping.outputs[0], noise.inputs["Vector"])
        ramp = nodes.new("ShaderNodeMapRange")
        ramp.inputs["To Min"].default_value = 1.0 - vary
        ramp.inputs["To Max"].default_value = 1.0 + vary
        links.new(noise.outputs[0], ramp.inputs[0])
        mix = nodes.new("ShaderNodeVectorMath")
        mix.operation = "SCALE"
        mix.inputs[0].default_value = rgb
        links.new(ramp.outputs[0], mix.inputs["Scale"])
        links.new(mix.outputs[0], bsdf.inputs["Base Color"])
        if bump > 0.0:
            node = nodes.new("ShaderNodeBump")
            node.inputs["Strength"].default_value = bump
            node.inputs["Distance"].default_value = 0.002
            links.new(noise.outputs[0], node.inputs["Height"])
            links.new(node.outputs[0], bsdf.inputs["Normal"])
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


def cushion(name, lo, hi, mat, radius, puff, bend=0.0, cuts=5):
    # A rounded, puffed box in its bounds; bend curves it back (y) towards its ends in x
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    size = [hi[k] - lo[k] for k in range(3)]
    for v in bm.verts:
        v.co = [lo[k] + (v.co[k] + 0.5) * size[k] for k in range(3)]
    bmesh.ops.bevel(bm, geom=bm.edges[:], offset=radius, segments=3, affect="EDGES", profile=0.5, clamp_overlap=True)
    broad = sorted(bm.faces, key=lambda f: -f.calc_area())[:2]
    bmesh.ops.subdivide_edges(bm, edges=list({e for f in broad for e in f.edges}), cuts=cuts, use_grid_fill=True)
    middle = [(lo[k] + hi[k]) / 2 for k in range(3)]
    if bend != 0.0:
        # Cut the rim too, or it would stay straight under the bent faces
        for k in range(1, 8):
            bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], plane_co=(lo[0] + size[0] * k / 8, 0, 0), plane_no=(1, 0, 0))
    thin = size.index(min(size))
    for v in bm.verts:
        q = [(v.co[k] - middle[k]) / (size[k] / 2) for k in range(3)]
        swell = 1.0
        for k in range(3):
            if k != thin:
                swell *= max(0.0, 1.0 - abs(q[k]) ** 2.5)
        v.co[thin] += math.copysign(puff * swell, q[thin])
        v.co.y -= bend * q[0] * q[0]
    bm.to_mesh(mesh)
    bm.free()
    for p in mesh.polygons:
        p.use_smooth = True
    return link(bpy.data.objects.new(name, mesh), mat)


def rod(name, start, end, r0, r1, mat, segments=14):
    # A tapered, slightly rounded rod between two points
    a, b = Vector(start), Vector(end)
    bm = bmesh.new()
    turn = (b - a).to_track_quat("Z", "Y").to_matrix()
    rings = []
    for along, radius in ((0.0, r0 * 0.75), (0.012, r0), (0.5, (r0 + r1) / 2), (0.988, r1), (1.0, r1 * 0.75)):
        rings.append([bm.verts.new(a + (b - a) * along + turn @ Vector((radius * math.cos(2 * math.pi * i / segments), radius * math.sin(2 * math.pi * i / segments), 0))) for i in range(segments)])
    for low, high in zip(rings, rings[1:]):
        for i in range(segments):
            bm.faces.new((low[i], low[(i + 1) % segments], high[(i + 1) % segments], high[i]))
    bm.faces.new(list(reversed(rings[0])))
    bm.faces.new(rings[-1])
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    for p in mesh.polygons:
        p.use_smooth = len(p.vertices) == 4
    return link(bpy.data.objects.new(name, mesh), mat)


def chair():
    oak = material("oak", (0.50, 0.33, 0.18), vary=0.22, scale=60.0, stretch=(1.0, 1.0, 0.06))
    rail = material("oak_rail", (0.50, 0.33, 0.18), vary=0.22, scale=60.0, stretch=(0.06, 1.0, 1.0))
    wool = material("wool", (0.13, 0.22, 0.27), vary=0.30, scale=420.0, bump=0.6)
    h = 0.45
    cushion("seat", (-0.22, -0.21, h - 0.055), (0.22, 0.20, h - 0.004), wool, 0.022, 0.010)
    box("frame", (0.39, 0.36, 0.028), (0.0, -0.005, h - 0.068), rail, bevel=0.006)
    for x in (-1, 1):
        rod("front_leg", (x * 0.195, -0.188, 0.0), (x * 0.172, -0.160, h - 0.06), 0.011, 0.018, oak)
        rod("back_leg", (x * 0.195, 0.198, 0.0), (x * 0.172, 0.160, h - 0.05), 0.011, 0.018, oak)
        rod("post", (x * 0.172, 0.158, h - 0.07), (x * 0.185, 0.197, 0.84), 0.018, 0.011, oak)
        rod("stretcher", (x * 0.186, -0.175, 0.17), (x * 0.186, 0.183, 0.17), 0.009, 0.009, oak)
    cushion("back", (-0.215, 0.160, 0.64), (0.215, 0.208, 0.87), wool, 0.020, 0.008, bend=0.028)
    rod("cross", (-0.186, 0.0, 0.17), (0.186, 0.0, 0.17), 0.009, 0.009, rail)


def pencil():
    # Lying along X on the floor, its hexagon flat on the ground
    paint = material("paint", (0.80, 0.50, 0.03), vary=0.05, scale=300.0)
    band = material("band", (0.03, 0.10, 0.07))
    wood = material("wood", (0.72, 0.50, 0.30), vary=0.25, scale=900.0, stretch=(0.1, 1.0, 1.0))
    lead = material("lead", (0.06, 0.06, 0.065))
    metal = material("ferrule", (0.62, 0.60, 0.52), vary=0.12, scale=900.0, stretch=(1.0, 0.02, 0.02))
    eraser = material("eraser", (0.76, 0.36, 0.36), vary=0.12, scale=500.0)
    r = 0.0035  # across the corners: 7 mm
    z = r * math.cos(math.pi / 6)
    body = 0.150
    cylinder("body", r, body - 0.012, (0.006, 0.0, z), paint, segments=6, axis="X", spin=math.pi / 6)
    cylinder("band", r, 0.012, (-body / 2 + 0.006, 0.0, z), band, segments=6, axis="X", spin=math.pi / 6)
    tip = 0.018
    # The scallops where the paint meets the sharpened wood
    cylinder("collar", r, 0.003, (body / 2 + 0.0015, 0.0, z), paint, segments=6, axis="X", radius_top=r * 0.94, spin=math.pi / 6)
    cylinder("tip", r * 0.94, tip - 0.003, (body / 2 + 0.003 + (tip - 0.003) / 2, 0.0, z), wood, segments=18, axis="X", radius_top=0.0009)
    cylinder("lead", 0.0009, 0.003, (body / 2 + tip + 0.0015, 0.0, z), lead, segments=10, axis="X", radius_top=0.0002)
    for k in range(3):
        cylinder("ferrule", r * (0.90 if k % 2 else 0.84), 0.003, (-body / 2 - 0.0015 - 0.003 * k, 0.0, z), metal, segments=20, axis="X")
    cylinder("eraser", r * 0.8, 0.0045, (-body / 2 - 0.01125, 0.0, z), eraser, segments=20, axis="X")
    cylinder("eraser_end", r * 0.62, 0.0015, (-body / 2 - 0.01425, 0.0, z), eraser, segments=20, axis="X", radius_top=r * 0.8)


def case():
    # A small hard case: ribbed shells, a rubber seal, latches and a folding handle at the front
    shell = material("shell", (0.055, 0.058, 0.062), vary=0.20, scale=900.0, bump=0.35)
    seal = material("seal", (0.02, 0.02, 0.021))
    metal = material("latch", (0.40, 0.40, 0.39), vary=0.08, scale=300.0)
    grip = material("grip", (0.04, 0.04, 0.042), vary=0.1, scale=500.0)
    w, d, h = 0.22, 0.15, 0.06
    box("bottom", (w, d, h * 0.5 - 0.0015), (0.0, 0.0, h * 0.25 - 0.00075), shell, bevel=0.011, segments=3)
    box("lid", (w, d, h * 0.5 - 0.0035), (0.0, 0.0, h * 0.75 - 0.0010), shell, bevel=0.011, segments=3)
    box("seal", (w - 0.008, d - 0.008, 0.005), (0.0, 0.0, h * 0.5), seal)
    for k in range(5):
        box("rib", (0.020, d - 0.034, 0.003), ((k - 2) * 0.036, 0.0, h - 0.0018), shell, bevel=0.0013, segments=2)
    for x in (-0.070, 0.070):
        box("latch", (0.026, 0.005, 0.026), (x, -d / 2 - 0.001, h * 0.5 + 0.001), metal, bevel=0.0018, segments=2)
        box("latch_lip", (0.022, 0.003, 0.004), (x, -d / 2 - 0.0045, h * 0.5 - 0.009), metal, bevel=0.001, segments=1)
        box("hinge", (0.030, 0.004, 0.012), (x, d / 2 + 0.0005, h * 0.5), metal, bevel=0.0015, segments=1)
    for x in (-0.036, 0.036):
        box("lug", (0.010, 0.012, 0.014), (x, -d / 2 - 0.005, h * 0.5), shell, bevel=0.003, segments=2)
    box("handle", (0.082, 0.012, 0.012), (0.0, -d / 2 - 0.012, h * 0.5), grip, bevel=0.0048, segments=3)


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


TEXTURE_SIZE = {"chair": 512, "case": 384, "pencil": 256}
BAKE_SAMPLES = 256


def bake(name, out_dir):
    # One mesh, one texture: the surfaces under a soft sky, standing on a floor that bounces and
    # darkens where the prop touches it
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    for o in bpy.context.selected_objects:
        o.select_set(False)
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.join()
    prop = bpy.context.view_layer.objects.active
    prop.name = prop.data.name = name
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(60), island_margin=0.006, area_weight=0.2)
    bpy.ops.object.mode_set(mode="OBJECT")
    size = TEXTURE_SIZE[name]
    image = bpy.data.images.new(name, size, size, alpha=False)
    for slot in prop.material_slots:
        node = slot.material.node_tree.nodes.new("ShaderNodeTexImage")
        node.image = image
        slot.material.node_tree.nodes.active = node
    scene = bpy.context.scene
    world = bpy.data.worlds.new("sky")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (1.0, 0.99, 0.97, 1.0)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.95
    scene.world = world
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 0.9
    sun.data.angle = 1.2
    sun.rotation_euler = (0.5, 0.35, 0.0)
    scene.collection.objects.link(sun)
    floor = bpy.data.meshes.new("floor")
    floor.from_pydata([(-2, -2, -0.0005), (2, -2, -0.0005), (2, 2, -0.0005), (-2, 2, -0.0005)], [], [(0, 1, 2, 3)])
    floor = bpy.data.objects.new("floor", floor)
    floor.data.materials.append(material("floor", (0.55, 0.52, 0.48)))
    scene.collection.objects.link(floor)
    scene.render.engine = "CYCLES"
    scene.cycles.samples = BAKE_SAMPLES
    scene.cycles.seed = 1
    scene.view_settings.view_transform = "Standard"
    bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT", "COLOR"}, margin=6)
    path = os.path.join(out_dir, name + ".jpg")
    image.file_format = "JPEG"
    image.save(filepath=path, quality=88)
    for o in (sun, floor):
        bpy.data.objects.remove(o)
    baked = bpy.data.materials.new(name + "_baked")
    baked.use_nodes = True
    texture = baked.node_tree.nodes.new("ShaderNodeTexImage")
    texture.image = bpy.data.images.load(path)
    texture.image.pack()
    texture.image.name = name
    baked.node_tree.links.new(texture.outputs["Color"], baked.node_tree.nodes["Principled BSDF"].inputs["Base Color"])
    baked.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 0.8
    for p in prop.data.polygons:
        p.material_index = 0
    prop.data.materials.clear()
    prop.data.materials.append(baked)
    os.remove(path)


def blend_first(directory, name):
    # The parts with their materials, before they are joined and baked. Only the scene's data,
    # without the window manager, which remembers local directories
    scene = bpy.context.scene
    scene.render.filepath = "//"
    bpy.data.libraries.write(os.path.join(directory, name + ".blend"), {scene}, path_remap="RELATIVE_ALL", fake_user=True, compress=True)


def export(name, out_dir):
    reset()
    BUILDERS[name]()
    recenter()
    directory = os.path.join(out_dir, name)
    os.makedirs(directory, exist_ok=True)
    blend_first(directory, name)
    bake(name, directory)
    directory = os.path.join(out_dir, name)
    os.makedirs(directory, exist_ok=True)
    glb = os.path.join(directory, name + ".glb")
    bpy.ops.export_scene.gltf(
        filepath=glb,
        export_format="GLB",
        export_yup=True,
        export_apply=True,
        export_texcoords=True,
        export_normals=False,
        export_image_format="JPEG",
        export_jpeg_quality=88,
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
