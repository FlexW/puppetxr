# Exports a Blender room to the puppetxr room format (docs/room-format.md).
#
# As an add-on it adds an "puppetxr" sidebar tab to tag shell faces and export. From the
# command line it exports without touching the .blend:
#
#   blender -b rooms/bedroom/bedroom.blend --python tools/blender/puppetxr_room_export.py -- rooms/bedroom [--no-bake] [--room-mesh-only] [--global-mesh-only]
#
# Scene conventions (docs/world.md):
#   collection puppetxr_shell    one closed mesh, every face tagged with the int face attribute
#                                puppetxr_label (SHELL_LABELS), normals pointing into the room
#   collection puppetxr_furniture empties with a custom property puppetxr_label; their child meshes
#                                give the volume, measured in the empty's frame
#   collection puppetxr_wall_items quads with puppetxr_label (door_frame, window_frame, wall_art, ...);
#                                local XY is the plane, +Z points into the room; semantics only.
#                                Door and window frames are also cut into the room mesh, so they
#                                must lie on a wall (within ROOM_MESH_FRAME_DISTANCE)
#
# The room mesh (XR_META_spatial_entity_room_mesh) is generated from the shell: one face per shell
# region and per door or window frame, the frames cut out of their wall's face.
#
# --room-mesh-only adds the room mesh to an exported room without touching anything else, so the
# visual mesh and the baked atlas stay byte for byte as they were. --global-mesh-only replaces the
# global mesh the same way.
#   collection puppetxr_visual   extra meshes that are only seen (frames, clutter, the outdoors)

bl_info = {
    "name": "puppetxr room export",
    "blender": (4, 2, 0),
    "category": "Import-Export",
    "description": "Tag a room shell and export it for the puppetxr OpenXR simulator",
}

import bpy
import bmesh
import hashlib
import json
import math
import os
import struct
import sys
from mathutils import Matrix, Vector

FORMAT_VERSION = 1

SHELL_LABELS = {1: "floor", 2: "ceiling", 3: "wall_face", 4: "invisible_wall_face", 5: "inner_wall_face"}
TOP_PLANE_LABELS = {"table", "couch", "bed", "storage"}
ENTITY_LABELS = {"other", "floor", "ceiling", "wall_face", "invisible_wall_face", "inner_wall_face", "door_frame", "window_frame", "opening",
                 "wall_art", "table", "couch", "bed", "storage", "screen", "lamp", "plant", "global_mesh"}

ATLAS_SIZE = 2048
ATLAS_MARGIN = 16
BAKE_SAMPLES = 2048
ALBEDO_SAMPLES = 16
GLOBAL_MESH_TRIANGLES = 8000
GLOBAL_MESH_EDGE = 0.2
EPSILON_EDGE = 1e-3 # slivers of decimated furniture keep an edge just past GLOBAL_MESH_EDGE
EPSILON = 1e-4
ROOM_MESH_LABELS = {"floor", "ceiling", "wall_face", "invisible_wall_face", "inner_wall_face", "door_frame", "window_frame"}
ROOM_MESH_FRAME_LABELS = {"door_frame", "window_frame"}
ROOM_MESH_FRAME_DISTANCE = 0.05


class ExportError(Exception):
    pass


#
# Coordinates: Blender is Z up, Y forward; room space is OpenXR stage space, Y up, -Z forward
#

def xr (v):
    return Vector((v.x, v.z, -v.y))


def quat_from_axes (x, y, z):
    q = Matrix((x, y, z)).transposed().to_quaternion()
    q.normalize()
    return [q.x, q.y, q.z, q.w]


def round_list (values, digits = 6):
    return [round(float(v), digits) for v in values]


def uuid_for (room, key):
    return hashlib.md5(("puppetxr/" + room + "/" + key).encode()).hexdigest()


#
# Shell
#

def shell_object ():
    collection = bpy.data.collections.get("puppetxr_shell")
    meshes = [o for o in (collection.all_objects if collection else []) if o.type == 'MESH']
    if len(meshes) != 1:
        raise ExportError("collection puppetxr_shell must hold exactly one mesh, found %d" % len(meshes))
    return meshes[0]


def face_groups (bm, labels):
    # Connected faces of one label on one plane form one entity
    seen  = set()
    groups = []
    for face in bm.faces:
        if face.index in seen:
            continue
        stack = [face]
        group = []
        seen.add(face.index)
        while stack:
            f = stack.pop()
            group.append(f)
            for edge in f.edges:
                for other in edge.link_faces:
                    if other.index in seen or labels[other.index] != labels[face.index]:
                        continue
                    if other.normal.dot(face.normal) < 0.999 or abs((other.verts[0].co - face.verts[0].co).dot(face.normal)) > 1e-3:
                        continue
                    seen.add(other.index)
                    stack.append(other)
        groups.append(group)
    return groups


def boundary_loop (group):
    # Edges with exactly one face in the group, chained into one loop following the faces' winding
    members = set(f.index for f in group)
    directed = {}
    for f in group:
        for loop in f.loops:
            edge = loop.edge
            if sum(1 for other in edge.link_faces if other.index in members) == 1:
                directed[loop.vert.index] = (loop.link_loop_next.vert.index, edge)
    if not directed:
        raise ExportError("a shell region has no boundary")
    start = next(iter(directed))
    order = [start]
    edges = []
    current = start
    while True:
        nxt, edge = directed[current]
        edges.append(edge)
        if nxt == start:
            break
        if nxt in order:
            raise ExportError("a shell region boundary is not a single loop")
        order.append(nxt)
        current = nxt
    if len(order) != len(directed):
        raise ExportError("floor and ceiling must be single polygons without holes")
    return order, edges


def drop_collinear (points):
    out = []
    n = len(points)
    for i in range(n):
        a, b, c = points[i - 1], points[i], points[(i + 1) % n]
        if abs((b - a).cross(c - b).length) > 1e-6 or (b - a).dot(c - b) < 0:
            out.append(b)
    return out


def analyse_shell (shell, room, entities):
    bm = bmesh.new()
    bm.from_mesh(shell.data)
    bm.transform(shell.matrix_world)
    bm.normal_update()
    bm.faces.ensure_lookup_table()
    bm.verts.ensure_lookup_table()

    layer = bm.faces.layers.int.get("puppetxr_label")
    if layer is None:
        raise ExportError("the shell has no puppetxr_label face attribute")
    labels = [f[layer] for f in bm.faces]
    for f in bm.faces:
        if labels[f.index] not in SHELL_LABELS:
            raise ExportError("shell face %d is not tagged" % f.index)
    for e in bm.edges:
        if len(e.link_faces) != 2:
            raise ExportError("the shell is not watertight at edge %d (%d faces)" % (e.index, len(e.link_faces)))

    groups = face_groups(bm, labels)
    floors   = [g for g in groups if SHELL_LABELS[labels[g[0].index]] == "floor"]
    ceilings = [g for g in groups if SHELL_LABELS[labels[g[0].index]] == "ceiling"]
    walls    = [g for g in groups if SHELL_LABELS[labels[g[0].index]] not in ("floor", "ceiling")]
    if len(floors) != 1 or len(ceilings) != 1:
        raise ExportError("the shell needs one flat floor and one flat ceiling region, found %d and %d" % (len(floors), len(ceilings)))
    floor, ceiling = floors[0], ceilings[0]
    if floor[0].normal.z < 0.999:
        raise ExportError("floor face %d is not horizontal or points down; normals must point into the room" % floor[0].index)
    if ceiling[0].normal.z > -0.999:
        raise ExportError("ceiling face %d is not horizontal or points up; normals must point into the room" % ceiling[0].index)
    floor_z   = floor[0].verts[0].co.z
    ceiling_z = ceiling[0].verts[0].co.z

    # Floor: Z up, X along room X, Y along Blender +Y; the boundary follows the inward faces, so it
    # runs counter-clockwise seen from above
    order, floor_edges = boundary_loop(floor)
    floor_points = drop_collinear([bm.verts[i].co.copy() for i in order])
    lo = Vector((min(p.x for p in floor_points), min(p.y for p in floor_points)))
    hi = Vector((max(p.x for p in floor_points), max(p.y for p in floor_points)))
    center = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, floor_z))
    polygon = [[p.x - center.x, p.y - center.y] for p in floor_points]
    floor_pose = (xr(center), quat_from_axes(Vector((1, 0, 0)), Vector((0, 0, -1)), Vector((0, 1, 0))))
    floor_index = add_entity(entities, room, "shell/floor", "floor", floor_pose,
                             plane = (lo - center.xy, hi - center.xy, polygon))
    regions = [("shell/floor", "floor", [f.index for f in floor]), ("shell/ceiling", "ceiling", [f.index for f in ceiling])]

    ceiling_center = Vector((center.x, center.y, ceiling_z))
    order, _ = boundary_loop(ceiling)
    ceiling_points = drop_collinear([bm.verts[i].co.copy() for i in order])
    ceiling_polygon = [[p.x - center.x, -(p.y - center.y)] for p in ceiling_points]
    ceiling_pose = (xr(ceiling_center), quat_from_axes(Vector((1, 0, 0)), Vector((0, 0, 1)), Vector((0, -1, 0))))
    ceiling_index = add_entity(entities, room, "shell/ceiling", "ceiling", ceiling_pose,
                               plane = (Vector((lo.x - center.x, -(hi.y - center.y))), Vector((hi.x - center.x, -(lo.y - center.y))), ceiling_polygon))

    # Walls in the order the floor boundary meets them
    wall_of_face = {}
    for i, g in enumerate(walls):
        for f in g:
            wall_of_face[f.index] = i
    wall_order = []
    for edge in floor_edges:
        for f in edge.link_faces:
            w = wall_of_face.get(f.index)
            if w is not None and w not in wall_order:
                wall_order.append(w)
    if len(wall_order) != len(walls):
        raise ExportError("%d wall regions do not stand on the floor boundary" % (len(walls) - len(wall_order)))

    wall_indices = []
    for n, w in enumerate(wall_order):
        g = walls[w]
        normal = g[0].normal.copy()
        if abs(normal.z) > 1e-3:
            raise ExportError("wall face %d is not vertical" % g[0].index)
        up    = Vector((0, 0, 1))
        right = up.cross(normal).normalized()
        verts = {v.index: v.co for f in g for v in f.verts}
        origin = g[0].verts[0].co
        xs = [(c - origin).dot(right) for c in verts.values()]
        zs = [c.z for c in verts.values()]
        width, height = max(xs) - min(xs), max(zs) - min(zs)
        area = sum(f.calc_area() for f in g)
        if abs(width * height - area) > 1e-3 * max(1.0, area):
            raise ExportError("wall region of face %d is not a rectangle (Scene v1 walls are rectangles)" % g[0].index)
        if abs(min(zs) - floor_z) > 1e-3 or abs(max(zs) - ceiling_z) > 1e-3:
            raise ExportError("wall region of face %d does not span floor to ceiling" % g[0].index)
        mid = origin + right * ((max(xs) + min(xs)) / 2)
        wall_center = Vector((mid.x, mid.y, (floor_z + ceiling_z) / 2))
        pose = (xr(wall_center), quat_from_axes(xr(right), xr(up), xr(normal)))
        label = SHELL_LABELS[labels[g[0].index]]
        wall_indices.append(add_entity(entities, room, "shell/wall%d" % n, label, pose,
                                       plane = (Vector((-width / 2, -height / 2)), Vector((width / 2, height / 2)), None)))
        regions.append(("shell/wall%d" % n, label, [f.index for f in g]))

    invisible = set(f.index for f in bm.faces if SHELL_LABELS[labels[f.index]] == "invisible_wall_face")
    bm.free()
    return floor_index, ceiling_index, wall_indices, floor_pose, invisible, regions


#
# Room mesh
#

def frame_quads ():
    # Door and window frames as (key, label, center, x axis, y axis, z axis, width, height) in world space
    collection = bpy.data.collections.get("puppetxr_wall_items")
    quads = []
    for obj in sorted(collection.all_objects if collection else [], key = lambda o: o.name):
        label = obj.get("puppetxr_label")
        if label not in ROOM_MESH_FRAME_LABELS or obj.type != 'MESH':
            continue
        corners = [Vector(c) for c in obj.bound_box]
        scale = obj.matrix_world.to_scale()
        lo = Vector((min(c.x for c in corners) * scale.x, min(c.y for c in corners) * scale.y))
        hi = Vector((max(c.x for c in corners) * scale.x, max(c.y for c in corners) * scale.y))
        rotation = obj.matrix_world.to_quaternion()
        center = obj.matrix_world @ Vector(((lo.x + hi.x) / 2 / scale.x, (lo.y + hi.y) / 2 / scale.y, 0))
        x_axis, y_axis, z_axis = (rotation @ Vector(a) for a in ((1, 0, 0), (0, 1, 0), (0, 0, 1)))
        quads.append(("wall_items/" + obj.name, label, center, x_axis, y_axis, z_axis, hi.x - lo.x, hi.y - lo.y))
    return quads


def room_mesh (shell, room, regions):
    # The shell, its regions as faces, with every door and window frame cut out of the wall it lies
    # on. Cuts split edges shared with neighbouring regions too, so the mesh stays watertight.
    bm = bmesh.new()
    bm.from_mesh(shell.data)
    bm.transform(shell.matrix_world)
    bm.normal_update()
    bm.faces.ensure_lookup_table()
    layer = bm.faces.layers.int.new("puppetxr_region")
    for r, (_, _, faces) in enumerate(regions):
        for i in faces:
            bm.faces[i][layer] = r

    faces = [(uuid_for(room, "room_mesh/" + key), None, label) for key, label, _ in regions]
    for key, label, center, x_axis, y_axis, z_axis, width, height in frame_quads():
        wall = None
        for r, (_, region_label, _) in enumerate(regions):
            if region_label in ("floor", "ceiling"):
                continue
            region_faces = [f for f in bm.faces if f[layer] == r]
            normal = region_faces[0].normal
            distance = (center - region_faces[0].verts[0].co).dot(normal)
            if normal.dot(z_axis) > 0.999 and abs(distance) <= ROOM_MESH_FRAME_DISTANCE:
                inside = [abs((v.co - center).dot(x_axis)) for f in region_faces for v in f.verts]
                if max(inside) >= width / 2 - EPSILON:
                    wall = r
                    break
        if wall is None:
            raise ExportError("%s %s lies on no wall (closer than %g m, facing into the room)" % (label, key, ROOM_MESH_FRAME_DISTANCE))
        normal = [f for f in bm.faces if f[layer] == wall][0].normal
        center = center - normal * (center - [f for f in bm.faces if f[layer] == wall][0].verts[0].co).dot(normal)
        for axis, half in ((x_axis, width / 2), (y_axis, height / 2)):
            for side in (-1, 1):
                region_faces = [f for f in bm.faces if f[layer] == wall]
                geom = list(set(region_faces) | set(e for f in region_faces for e in f.edges) | set(v for f in region_faces for v in f.verts))
                bmesh.ops.bisect_plane(bm, geom = geom, dist = EPSILON, plane_co = center + axis * (side * half), plane_no = axis)
        bm.faces.ensure_lookup_table()
        frame = len(faces)
        cut = 0
        for f in bm.faces:
            c = f.calc_center_median()
            if f[layer] == wall and abs((c - center).dot(x_axis)) < width / 2 and abs((c - center).dot(y_axis)) < height / 2:
                f[layer] = frame
                cut += 1
        if not cut:
            raise ExportError("%s %s does not overlap its wall" % (label, key))
        faces.append((uuid_for(room, "room_mesh/" + key), faces[wall][0], label))

    bmesh.ops.triangulate(bm, faces = bm.faces[:])
    for e in bm.edges:
        if len(e.link_faces) != 2:
            raise ExportError("the room mesh is not watertight after cutting the frames")
    bm.verts.index_update()
    positions = [xr(v.co) for v in bm.verts]
    triangles = [[] for _ in faces]
    for f in bm.faces:
        triangles[f[layer]] += [v.index for v in f.verts]
    bm.free()
    return positions, [(uuid, parent, label, tris) for (uuid, parent, label), tris in zip(faces, triangles)]


def room_mesh_json (binary, positions, faces):
    # Appends the room mesh to the binary and returns its room.json description
    def view (data, fmt, count):
        offset = len(binary)
        binary.extend(struct.pack("<" + fmt * len(data), *data))
        while len(binary) % 4:
            binary.append(0)
        return {"offset": offset, "count": count}
    out = {"positions": view([c for p in positions for c in p], "f", len(positions)), "faces": []}
    for uuid, parent, label, indices in faces:
        face = {"uuid": uuid, "label": label}
        if parent:
            face["parent"] = parent
        face["indices"] = view(indices, "I", len(indices))
        out["faces"].append(face)
    return out


#
# Entities
#

def add_entity (entities, room, key, label, pose, plane = None, volume = None, mesh = False):
    if label not in ENTITY_LABELS:
        raise ExportError("unknown label '%s' on %s" % (label, key))
    entity = {
        "uuid": uuid_for(room, key),
        "label": label,
        "position": round_list(pose[0]),
        "orientation": round_list(pose[1]),
    }
    if plane:
        lo, hi, polygon = plane
        entity["plane"] = {"min": round_list(lo), "max": round_list(hi)}
        if polygon:
            entity["plane"]["boundary"] = [round_list(p) for p in polygon]
    if volume:
        entity["volume"] = {"min": round_list(volume[0]), "max": round_list(volume[1])}
    if mesh:
        entity["mesh"] = True
    entities.append(entity)
    return len(entities) - 1


def mesh_children (obj):
    out = []
    for child in obj.children_recursive:
        if child.type == 'MESH':
            out.append(child)
    return out


def furniture (room, entities):
    collection = bpy.data.collections.get("puppetxr_furniture")
    indices = []
    depsgraph = bpy.context.evaluated_depsgraph_get()
    for obj in sorted(collection.all_objects if collection else [], key = lambda o: o.name):
        label = obj.get("puppetxr_label")
        if label is None:
            continue
        frame = obj.matrix_world.copy()
        rotation = frame.to_quaternion()
        if abs(rotation.to_euler().x) > 1e-3 or abs(rotation.to_euler().y) > 1e-3:
            raise ExportError("furniture %s may only turn about the vertical axis" % obj.name)
        inverse = Matrix.Translation(frame.translation) @ rotation.to_matrix().to_4x4()
        inverse = inverse.inverted()
        points = []
        for child in mesh_children(obj) + ([obj] if obj.type == 'MESH' else []):
            evaluated = child.evaluated_get(depsgraph)
            for v in evaluated.data.vertices:
                points.append(inverse @ (child.matrix_world @ v.co))
        if not points:
            raise ExportError("furniture %s has no meshes" % obj.name)
        lo = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
        hi = Vector((max(p.x for p in points), max(p.y for p in points), max(p.z for p in points)))
        local_top = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, hi.z))
        top = Matrix.Translation(frame.translation) @ rotation.to_matrix().to_4x4() @ local_top
        x_axis = rotation @ Vector((1, 0, 0))
        y_axis = rotation @ Vector((0, 1, 0))
        pose = (xr(top), quat_from_axes(xr(x_axis), xr(y_axis), xr(Vector((0, 0, 1)))))
        w, d, h = hi.x - lo.x, hi.y - lo.y, hi.z - lo.z
        plane = (Vector((-w / 2, -d / 2)), Vector((w / 2, d / 2)), None) if label in TOP_PLANE_LABELS else None
        indices.append(add_entity(entities, room, "furniture/" + obj.name, label, pose, plane = plane,
                                  volume = (Vector((-w / 2, -d / 2, -h)), Vector((w / 2, d / 2, 0)))))
    return indices


def wall_items (room, entities):
    collection = bpy.data.collections.get("puppetxr_wall_items")
    indices = []
    for obj in sorted(collection.all_objects if collection else [], key = lambda o: o.name):
        label = obj.get("puppetxr_label")
        if label is None or obj.type != 'MESH':
            continue
        corners = [Vector(c) for c in obj.bound_box]
        scale = obj.matrix_world.to_scale()
        lo = Vector((min(c.x for c in corners) * scale.x, min(c.y for c in corners) * scale.y))
        hi = Vector((max(c.x for c in corners) * scale.x, max(c.y for c in corners) * scale.y))
        rotation = obj.matrix_world.to_quaternion()
        local_center = Vector(((lo.x + hi.x) / 2 / scale.x, (lo.y + hi.y) / 2 / scale.y, 0))
        center = obj.matrix_world @ local_center
        x_axis, y_axis, z_axis = (rotation @ Vector(a) for a in ((1, 0, 0), (0, 1, 0), (0, 0, 1)))
        pose = (xr(center), quat_from_axes(xr(x_axis), xr(y_axis), xr(z_axis)))
        w, h = hi.x - lo.x, hi.y - lo.y
        indices.append(add_entity(entities, room, "wall_items/" + obj.name, label, pose, plane = (Vector((-w / 2, -h / 2)), Vector((w / 2, h / 2)), None)))
    return indices


#
# Meshes and baking
#

def visual_objects (shell):
    objects = [shell]
    for name in ("puppetxr_furniture", "puppetxr_visual"):
        collection = bpy.data.collections.get(name)
        if collection:
            objects += [o for o in collection.all_objects if o.type == 'MESH' and not o.hide_render]
    return objects


def joined_copy (objects, invisible_shell_faces, shell):
    # One evaluated, triangulated copy of everything visible, with single-user materials
    depsgraph = bpy.context.evaluated_depsgraph_get()
    bm = bmesh.new()
    materials = []
    for obj in objects:
        evaluated = obj.evaluated_get(depsgraph)
        mesh = bpy.data.meshes.new_from_object(evaluated)
        part = bmesh.new()
        part.from_mesh(mesh)
        if obj == shell and invisible_shell_faces:
            part.faces.ensure_lookup_table()
            bmesh.ops.delete(part, geom = [part.faces[i] for i in sorted(invisible_shell_faces)], context = 'FACES')
        part.transform(obj.matrix_world)
        offset = len(materials)
        materials += [slot.material for slot in obj.material_slots] or [None]
        for f in part.faces:
            f.material_index += offset
        part.to_mesh(mesh)
        part.free()
        bm.from_mesh(mesh)
        bpy.data.meshes.remove(mesh)
    bmesh.ops.triangulate(bm, faces = bm.faces[:])
    mesh = bpy.data.meshes.new("puppetxr_export")
    bm.to_mesh(mesh)
    bm.free()
    for material in materials:
        mesh.materials.append(material.copy() if material else None)
    obj = bpy.data.objects.new("puppetxr_export", mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def bake_pass (image, nodes, samples, lighting):
    scene = bpy.context.scene
    for node in nodes:
        node.image = image
    scene.cycles.samples = samples
    scene.render.bake.use_pass_direct = lighting
    scene.render.bake.use_pass_indirect = lighting
    scene.render.bake.use_pass_color = True
    bpy.ops.object.bake(type = 'DIFFUSE', uv_layer = "puppetxr_bake", margin = ATLAS_MARGIN)


def denoise_and_save (lit, albedo, path, size):
    # OIDN through the compositor of a scratch scene without render layers, so nothing is
    # rendered. The source scene's view transform maps the unclipped bake to display values,
    # so the atlas looks like Blender's rendered view.
    source = bpy.context.scene
    scene = bpy.data.scenes.new("puppetxr_denoise")
    tree = bpy.data.node_groups.new("puppetxr_denoise", "CompositorNodeTree")
    try:
        scene.render.resolution_x = size
        scene.render.resolution_y = size
        scene.render.resolution_percentage = 100
        scene.display_settings.display_device = source.display_settings.display_device
        for key in ("view_transform", "look", "exposure", "gamma"):
            setattr(scene.view_settings, key, getattr(source.view_settings, key))
        settings = scene.render.image_settings
        settings.file_format = 'PNG'
        settings.color_mode = 'RGB'
        settings.color_depth = '8'
        settings.compression = 100
        # No render metadata in the atlas: it would carry the .blend path, host name and date
        for key in ("date", "time", "render_time", "frame", "frame_range", "memory", "hostname", "camera", "lens", "scene", "marker", "filename", "sequencer_strip", "note"):
            if hasattr(scene.render, "use_stamp_" + key):
                setattr(scene.render, "use_stamp_" + key, False)
        tree.interface.new_socket("Image", in_out = 'OUTPUT', socket_type = 'NodeSocketColor')
        scene.compositing_node_group = tree
        lit_node = tree.nodes.new("CompositorNodeImage")
        lit_node.image = lit
        albedo_node = tree.nodes.new("CompositorNodeImage")
        albedo_node.image = albedo
        denoise = tree.nodes.new("CompositorNodeDenoise")
        denoise.inputs["HDR"].default_value = True
        denoise.inputs["Quality"].default_value = 'High'
        output = tree.nodes.new("NodeGroupOutput")
        tree.links.new(lit_node.outputs["Image"], denoise.inputs["Image"])
        tree.links.new(albedo_node.outputs["Image"], denoise.inputs["Albedo"])
        tree.links.new(denoise.outputs["Image"], output.inputs[0])
        bpy.ops.render.render(scene = scene.name)
        bpy.data.images["Render Result"].save_render(path, scene = scene)
    finally:
        bpy.data.scenes.remove(scene)
        bpy.data.node_groups.remove(tree)


def bake_atlas (obj, path, size):
    scene = bpy.context.scene
    bpy.ops.object.select_all(action = 'DESELECT')
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    uv = obj.data.uv_layers.new(name = "puppetxr_bake")
    obj.data.uv_layers.active = uv
    bpy.ops.object.mode_set(mode = 'EDIT')
    bpy.ops.mesh.select_all(action = 'SELECT')
    bpy.ops.uv.smart_project(angle_limit = math.radians(66), island_margin = 0.006, area_weight = 1.0)
    bpy.ops.uv.pack_islands(margin = 0.006, rotate = True)
    bpy.ops.object.mode_set(mode = 'OBJECT')

    # Float buffers: a byte bake clips lit surfaces before the view transform sees them
    lit = bpy.data.images.new("puppetxr_atlas", size, size, alpha = False, float_buffer = True)
    albedo = bpy.data.images.new("puppetxr_albedo", size, size, alpha = False, float_buffer = True)
    nodes = []
    for material in obj.data.materials:
        if material is None:
            continue
        material.use_nodes = True
        node = material.node_tree.nodes.new("ShaderNodeTexImage")
        material.node_tree.nodes.active = node
        nodes.append(node)

    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'GPU' if bpy.context.preferences.addons['cycles'].preferences.has_active_device() else 'CPU'
    try:
        bake_pass(lit, nodes, BAKE_SAMPLES, True)
        bake_pass(albedo, nodes, ALBEDO_SAMPLES, False)
        denoise_and_save(lit, albedo, path, size)
    finally:
        bpy.data.images.remove(lit)
        bpy.data.images.remove(albedo)
    return uv


def global_mesh_of (work):
    # A scan of the same surfaces: welded, decimated, then cut until no edge is longer than
    # GLOBAL_MESH_EDGE, as a headset's scan has no wall-sized triangles (MRUK's destructible mesh
    # segments it per triangle and keeps the ones near the floor)
    mesh = work.data
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.remove_doubles(bm, verts = bm.verts[:], dist = 0.005)
    bm.to_mesh(mesh)
    bm.free()
    triangles = len(mesh.polygons)
    if triangles > GLOBAL_MESH_TRIANGLES:
        decimate = work.modifiers.new("decimate", 'DECIMATE')
        decimate.ratio = GLOBAL_MESH_TRIANGLES / triangles
        decimate.use_collapse_triangulate = True
        depsgraph = bpy.context.evaluated_depsgraph_get()
        scan = bpy.data.meshes.new_from_object(work.evaluated_get(depsgraph))
    else:
        scan = mesh.copy()
    bm = bmesh.new()
    bm.from_mesh(scan)
    bmesh.ops.triangulate(bm, faces = bm.faces[:])
    # Edges and vertices of no face are no surface, and subdividing never shortens them
    bmesh.ops.delete(bm, geom = [e for e in bm.edges if not e.link_faces], context = 'EDGES')
    bmesh.ops.delete(bm, geom = [v for v in bm.verts if not v.link_faces], context = 'VERTS')
    longest = math.inf
    while True:
        long_edges = [e for e in bm.edges if e.calc_length() > GLOBAL_MESH_EDGE + EPSILON_EDGE]
        if not long_edges:
            break
        length = max(e.calc_length() for e in long_edges)
        if length >= longest:
            raise ExportError("the global mesh keeps an edge of %g m: subdividing does not shorten it" % length)
        longest = length
        bmesh.ops.subdivide_edges(bm, edges = long_edges, cuts = 1, use_grid_fill = True)
        bmesh.ops.triangulate(bm, faces = bm.faces[:])
    positions = [xr(v.co) for v in bm.verts]
    bm.verts.index_update()
    indices = [v.index for f in bm.faces for v in f.verts]
    bm.free()
    bpy.data.meshes.remove(scan)
    return positions, indices


def write_room (out_dir, objects, shell, invisible, bake):
    os.makedirs(out_dir, exist_ok = True)
    work = joined_copy(objects, invisible, shell)
    # The copy lies on the originals, which would shadow every surface it bakes
    hidden = [o for o in bpy.context.scene.objects if o.type == 'MESH' and o != work and not o.hide_render]
    for o in hidden:
        o.hide_render = True
    try:
        albedo = os.path.join(out_dir, "room_albedo.png")
        if bake:
            bake_atlas(work, albedo, ATLAS_SIZE)
        mesh = work.data
        uv = mesh.uv_layers.get("puppetxr_bake")

        # Visual vertices are split per triangle corner so every corner keeps its own uv
        vertices = []
        indices = []
        lookup = {}
        for tri in mesh.polygons:
            for loop_index in tri.loop_indices:
                v = mesh.vertices[mesh.loops[loop_index].vertex_index].co
                t = uv.data[loop_index].uv if uv else Vector((0.5, 0.5))
                key = (round(v.x, 5), round(v.y, 5), round(v.z, 5), round(t.x, 6), round(t.y, 6))
                index = lookup.get(key)
                if index is None:
                    index = len(vertices)
                    lookup[key] = index
                    p = xr(v)
                    vertices.append((p.x, p.y, p.z, t.x, 1.0 - t.y))
                indices.append(index)

        positions, mesh_indices = global_mesh_of(work)

        binary = bytearray()
        def view (data, fmt, count):
            offset = len(binary)
            binary.extend(struct.pack("<" + fmt * count, *data) if count else b"")
            while len(binary) % 4:
                binary.append(0)
            return {"offset": offset, "count": count}
        visual = {
            "vertices": view([c for v in vertices for c in v], "f", len(vertices) * 5),
            "indices": view(indices, "I", len(indices)),
        }
        visual["vertices"]["count"] = len(vertices)
        global_mesh = {
            "positions": view([c for p in positions for c in p], "f", len(positions) * 3),
            "indices": view(mesh_indices, "I", len(mesh_indices)),
        }
        global_mesh["positions"]["count"] = len(positions)
        with open(os.path.join(out_dir, "room.bin"), "wb") as f:
            f.write(binary)
        return visual, global_mesh, albedo if bake else None
    finally:
        for o in hidden:
            o.hide_render = False
        materials = list(work.data.materials)
        mesh = work.data
        bpy.data.objects.remove(work)
        bpy.data.meshes.remove(mesh)
        for material in materials:
            if material:
                bpy.data.materials.remove(material)


def export_room (out_dir, bake = True):
    room = os.path.basename(os.path.normpath(out_dir))
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode = 'OBJECT')

    shell = shell_object()
    entities = []
    room_index = add_entity(entities, room, "room", "other", ((0, 0, 0), [0, 0, 0, 1]))
    floor, ceiling, walls, floor_pose, invisible, regions = analyse_shell(shell, room, entities)
    entities[room_index]["position"] = round_list(floor_pose[0])
    entities[room_index]["orientation"] = round_list(floor_pose[1])
    furniture(room, entities)
    wall_items(room, entities)
    # Turned like the floor, local +Z up, as on Meta's headsets: MRUK reserves the bottom of the
    # global mesh along that axis
    add_entity(entities, room, "global_mesh", "global_mesh", floor_pose, mesh = True)
    entities[room_index]["room"] = {"floor": floor, "ceiling": ceiling, "walls": walls, "children": list(range(1, len(entities)))}

    positions, faces = room_mesh(shell, room, regions)
    visual, global_mesh, albedo = write_room(out_dir, visual_objects(shell), shell, invisible, bake)
    binary = bytearray(open(os.path.join(out_dir, "room.bin"), "rb").read())
    entities[room_index]["room_mesh"] = room_mesh_json(binary, positions, faces)
    with open(os.path.join(out_dir, "room.bin"), "wb") as f:
        f.write(binary)
    document = {
        "format": "puppetxr_room",
        "version": FORMAT_VERSION,
        "name": room,
        "binary": "room.bin",
    }
    if albedo:
        document["albedo"] = os.path.basename(albedo)
    document["visual"] = visual
    document["global_mesh"] = global_mesh
    document["entities"] = entities
    with open(os.path.join(out_dir, "room.json"), "w", newline = "
") as f:
        json.dump(document, f, indent = 1)
        f.write("\n")
    return len(entities)


def add_room_mesh (out_dir):
    # Replaces the room mesh of an exported room, keeping every other byte of room.bin in place
    room = os.path.basename(os.path.normpath(out_dir))
    shell = shell_object()
    _, _, _, _, _, regions = analyse_shell(shell, room, [])
    positions, faces = room_mesh(shell, room, regions)
    with open(os.path.join(out_dir, "room.json")) as f:
        document = json.load(f)
    binary = bytearray(open(os.path.join(out_dir, "room.bin"), "rb").read())
    rooms = [e for e in document["entities"] if "room" in e]
    if len(rooms) != 1:
        raise ExportError("room.json has %d rooms, the shell describes one" % len(rooms))
    if "room_mesh" in rooms[0]:
        raise ExportError("room.json already has a room mesh; export the room again instead")
    rooms[0]["room_mesh"] = room_mesh_json(binary, positions, faces)
    document["version"] = FORMAT_VERSION
    with open(os.path.join(out_dir, "room.bin"), "wb") as f:
        f.write(binary)
    with open(os.path.join(out_dir, "room.json"), "w", newline = "
") as f:
        json.dump(document, f, indent = 1)
        f.write("\n")
    return len(faces)


def replace_global_mesh (out_dir):
    # Replaces the global mesh of an exported room and turns its entity like the floor; the visual
    # mesh, the atlas and the room mesh keep their bytes, the room mesh after the global mesh moves
    # with it
    room = os.path.basename(os.path.normpath(out_dir))
    shell = shell_object()
    _, _, _, _, invisible, _ = analyse_shell(shell, room, [])
    work = joined_copy(visual_objects(shell), invisible, shell)
    try:
        positions, indices = global_mesh_of(work)
    finally:
        materials = list(work.data.materials)
        mesh = work.data
        bpy.data.objects.remove(work)
        bpy.data.meshes.remove(mesh)
        for material in materials:
            if material:
                bpy.data.materials.remove(material)

    with open(os.path.join(out_dir, "room.json")) as f:
        document = json.load(f)
    old = open(os.path.join(out_dir, "room.bin"), "rb").read()
    old_global = document["global_mesh"]
    start = old_global["positions"]["offset"]
    end = old_global["indices"]["offset"] + old_global["indices"]["count"] * 4
    while end % 4:
        end += 1

    binary = bytearray(old[:start])
    def view (data, fmt, count):
        offset = len(binary)
        binary.extend(struct.pack("<" + fmt * len(data), *data))
        while len(binary) % 4:
            binary.append(0)
        return {"offset": offset, "count": count}
    document["global_mesh"] = {
        "positions": view([c for p in positions for c in p], "f", len(positions)),
        "indices": view(indices, "I", len(indices)),
    }
    shift = len(binary) - end
    binary.extend(old[end:])

    rooms = [e for e in document["entities"] if "room" in e]
    floor = document["entities"][rooms[0]["room"]["floor"]]
    for entity in document["entities"]:
        if entity["label"] == "global_mesh":
            entity["position"] = floor["position"]
            entity["orientation"] = floor["orientation"]

    views = [document["visual"]["vertices"], document["visual"]["indices"]]
    for entity in document["entities"]:
        if "room_mesh" in entity:
            views.append(entity["room_mesh"]["positions"])
            views.extend(face["indices"] for face in entity["room_mesh"]["faces"])
    for v in views:
        if start <= v["offset"] < end:
            raise ExportError("a view at %d lies inside the global mesh" % v["offset"])
        if v["offset"] >= end:
            v["offset"] += shift

    with open(os.path.join(out_dir, "room.bin"), "wb") as f:
        f.write(binary)
    with open(os.path.join(out_dir, "room.json"), "w", newline = "
") as f:
        json.dump(document, f, indent = 1)
        f.write("\n")
    return len(indices) // 3


#
# Add-on: face tagging and export from the sidebar
#

class PUPPETXR_OT_tag_faces (bpy.types.Operator):
    bl_idname = "puppetxr.tag_faces"
    bl_label = "Tag selected faces"
    bl_options = {'REGISTER', 'UNDO'}
    label: bpy.props.IntProperty()

    def execute (self, context):
        obj = context.edit_object
        if obj is None or obj.type != 'MESH':
            return {'CANCELLED'}
        bm = bmesh.from_edit_mesh(obj.data)
        layer = bm.faces.layers.int.get("puppetxr_label") or bm.faces.layers.int.new("puppetxr_label")
        for f in bm.faces:
            if f.select:
                f[layer] = self.label
        bmesh.update_edit_mesh(obj.data)
        return {'FINISHED'}


class PUPPETXR_OT_export (bpy.types.Operator):
    bl_idname = "puppetxr.export_room"
    bl_label = "Export puppetxr room"
    directory: bpy.props.StringProperty(subtype = 'DIR_PATH')

    def invoke (self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def execute (self, context):
        try:
            count = export_room(self.directory)
        except ExportError as e:
            self.report({'ERROR'}, str(e))
            return {'CANCELLED'}
        self.report({'INFO'}, "Exported %d entities to %s" % (count, self.directory))
        return {'FINISHED'}


class PUPPETXR_PT_panel (bpy.types.Panel):
    bl_label = "puppetxr room"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "puppetxr"

    def draw (self, context):
        layout = self.layout
        if context.mode == 'EDIT_MESH':
            for value, name in SHELL_LABELS.items():
                layout.operator("puppetxr.tag_faces", text = name).label = value
        obj = context.object
        if obj and context.mode == 'OBJECT':
            layout.prop(obj, '["puppetxr_label"]', text = "label") if "puppetxr_label" in obj else layout.label(text = "no puppetxr_label property")
        layout.operator("puppetxr.export_room")


CLASSES = (PUPPETXR_OT_tag_faces, PUPPETXR_OT_export, PUPPETXR_PT_panel)


def register ():
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister ():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not args:
        register()
    else:
        try:
            if "--room-mesh-only" in args:
                count = add_room_mesh(args[0])
                print("puppetxr: added a room mesh of %d faces to %s" % (count, args[0]))
            elif "--global-mesh-only" in args:
                count = replace_global_mesh(args[0])
                print("puppetxr: replaced the global mesh of %s, %d triangles" % (args[0], count))
            else:
                count = export_room(args[0], bake = "--no-bake" not in args)
                print("puppetxr: exported %d entities to %s" % (count, args[0]))
        except ExportError as e:
            print("puppetxr export failed: %s" % e)
            sys.exit(1)
