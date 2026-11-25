# Models the simulated devices and writes data/devices/ (docs/openxr.md, Render models):
#
#   blender -b --factory-startup --disable-autoexec --python tools/blender/puppetxr_devices.py -- data/devices
#
#   controller.json  the controllers: every part of every controller type, in the right hand's grip
#                    space, and how its nodes move with the inputs
#   keyboard.json    the physical keyboard, around the centre of its bounding box
#                    Both carry a palette texture: a row per material, occlusion along it, so a node
#                    is one primitive whatever it is made of and an unlit app shows its form
#   glyphs.json      the virtual keyboard's labels as coverage, its keys are modelled by src/sim.cpp
#
# The labels are set in Inter (SIL Open Font License), which Blender ships. The same Blender writes
# the same bytes.

import base64
import json
import math
import os
import struct
import sys
import zlib

import bmesh
import bpy
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

TOUCH = ["oculus_touch", "meta_touch_pro", "meta_touch_plus"]
SMOOTH = math.cos(math.radians(42))  # edges sharper than this split their vertices


# Linear colours
SHELL = (0.62, 0.61, 0.58)
FACE = (0.028, 0.030, 0.034)
RUBBER = (0.045, 0.046, 0.050)
BUTTON = (0.085, 0.088, 0.095)
LABEL = (0.72, 0.72, 0.70)
ACCENT = (0.10, 0.42, 0.46)
LENS = (0.008, 0.008, 0.010)
ALUMINIUM = (0.42, 0.43, 0.44)
WELL = (0.020, 0.020, 0.022)
CAP = (0.040, 0.041, 0.044)
MODIFIER = (0.075, 0.076, 0.080)
LEGEND = (0.75, 0.76, 0.76)
MATERIALS = [SHELL, FACE, RUBBER, BUTTON, LABEL, ACCENT, LENS, ALUMINIUM, WELL, CAP, MODIFIER, LEGEND]

PALETTE_ROWS = 16  # SIM_MODEL_PALETTE_ROWS
PALETTE_CELL = 16  # texels a row is high: mipmaps must not mix materials
PALETTE_WIDTH = 64
OCCLUSION_RAYS = 64
OCCLUSION_DISTANCE = 0.012  # about the size of the gaps
OCCLUSION_FLOOR = 0.3       # of the colour where nothing is open


#
# Meshes: lists of points and faces, counter-clockwise seen from outside
#


def grip(x, y, z):
    # Grip space in Blender's axes, where the shapes are built: the handle along Y, its head at +Y
    return Vector((x, -z, y))


def from_grip_axes(p):
    return (p.x, p.z, -p.y)


def from_bmesh(bm):
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.verts.index_update()
    points = [v.co.copy() for v in bm.verts]
    faces = [tuple(v.index for v in f.verts) for f in bm.faces]
    bm.free()
    return points, faces


def ring_points(center, ex, ey, rx, ry, n, power):
    points = []
    for i in range(n):
        a = 2 * math.pi * i / n
        c, s = math.cos(a), math.sin(a)
        points.append(center + ex * (rx * math.copysign(abs(c) ** (2 / power), c)) + ey * (ry * math.copysign(abs(s) ** (2 / power), s)))
    return points


def skin(rings, caps):
    bm = bmesh.new()
    rows = [[bm.verts.new(p) for p in ring] for ring in rings]
    for a, b in zip(rows, rows[1:]):
        n = max(len(a), len(b))
        for i in range(n):
            j = (i + 1) % n
            if len(b) == 1:
                bm.faces.new((a[i], a[j], b[0]))
            elif len(a) == 1:
                bm.faces.new((a[0], b[j], b[i]))
            else:
                bm.faces.new((a[i], a[j], b[j], b[i]))
    if caps[0] and len(rows[0]) > 1:
        bm.faces.new(list(reversed(rows[0])))
    if caps[1] and len(rows[-1]) > 1:
        bm.faces.new(rows[-1])
    return from_bmesh(bm)


def loft(sections, n, sub=1, caps=(True, True)):
    # Superellipse sections (centre, x axis, y axis, rx, ry, power), Catmull-Rom between them
    full = []
    last = len(sections) - 1
    for i in range(last):
        p = [sections[max(i - 1, 0)], sections[i], sections[i + 1], sections[min(i + 2, last)]]
        for j in range(sub):
            u = j / sub

            def spline(k):
                a, b, c, d = (s[k] for s in p)
                return 0.5 * (2 * b + (c - a) * u + (2 * a - 5 * b + 4 * c - d) * u * u + (3 * b - a - 3 * c + d) * u ** 3)

            ex = (p[1][1] * (1 - u) + p[2][1] * u).normalized()
            ey = (p[1][2] * (1 - u) + p[2][2] * u).normalized()
            full.append((spline(0), ex, ey, max(spline(3), 1e-4), max(spline(4), 1e-4), p[1][5]))
    full.append(sections[-1])
    return skin([ring_points(*s[:5], n, s[5]) for s in full], caps)


def lathe(origin, normal, profile, n, squash=(1.0, 1.0)):
    # Profile (radius, height), from the rim to the axis where the radius is 0
    z = normal.normalized()
    up = grip(0, 0, -1) if abs(z.dot(grip(0, 0, -1))) < 0.95 else grip(0, 1, 0)
    x = up.cross(z).normalized()
    y = z.cross(x).normalized()
    rings = []
    for radius, height in profile:
        turn = range(n) if radius > 0 else ()
        ring = [origin + z * height + x * (radius * squash[0] * math.cos(2 * math.pi * i / n)) + y * (radius * squash[1] * math.sin(2 * math.pi * i / n)) for i in turn]
        rings.append(ring or [origin + z * height])
    return skin(rings, (False, False))


def torus(center, ex, ey, ez, major, minor, segments, sides):
    points, faces = [], []
    for i in range(segments):
        a = 2 * math.pi * i / segments
        out = ex * math.cos(a) + ey * math.sin(a)
        for j in range(sides):
            b = 2 * math.pi * j / sides
            points.append(center + out * (major + minor * math.cos(b)) + ez * (minor * math.sin(b)))
    for i in range(segments):
        for j in range(sides):
            a, b = i * sides + j, ((i + 1) % segments) * sides + j
            c, d = ((i + 1) % segments) * sides + (j + 1) % sides, i * sides + (j + 1) % sides
            faces.append((a, b, c, d))
    return points, faces


def rounded_box(cx, cy, hx, hy, radius, z0, z1, bevel, taper=0.0, seg=3, steps=(45, 90), bottom=True):
    # A slab with rounded corners, its top edge bevelled; z up
    def ring(inset, z):
        r = max(radius - inset, 0.0003)
        points = []
        for qx, qy, start in ((1, 1, 0), (-1, 1, 90), (-1, -1, 180), (1, -1, 270)):
            for i in range(seg + 1):
                a = math.radians(start + 90 * i / seg)
                points.append(Vector((cx + qx * (hx - inset - r) + r * math.cos(a), cy + qy * (hy - inset - r) + r * math.sin(a), z)))
        return points

    profile = [(0.0, z0), (taper, z1 - bevel)]
    profile += [(taper + bevel * (1 - math.cos(math.radians(t))), z1 - bevel + bevel * math.sin(math.radians(t))) for t in steps]
    return skin([ring(i, z) for i, z in profile], (bottom, True))


def join(*meshes):
    points, faces = [], []
    for p, f in meshes:
        faces += [tuple(i + len(points) for i in face) for face in f]
        points = points + list(p)
    return points, faces


def transformed(mesh, matrix):
    points = [matrix @ p for p in mesh[0]]
    faces = mesh[1] if matrix.determinant() > 0 else [tuple(reversed(f)) for f in mesh[1]]
    return points, faces


FONT = None


def text_mesh(text, size, resolution=2, align="CENTER", bolder=0.0):
    # The letters in the XY plane, centred on their ink
    global FONT
    if FONT is None:
        FONT = bpy.data.fonts.load(os.path.join(bpy.utils.system_resource("DATAFILES"), "fonts", "Inter.woff2"))
    curve = bpy.data.curves.new("text", "FONT")
    curve.font = FONT
    curve.body = text
    curve.size = size
    curve.resolution_u = resolution
    curve.offset = bolder
    o = bpy.data.objects.new("text", curve)
    bpy.context.collection.objects.link(o)
    bpy.context.view_layer.update()
    mesh = bpy.data.meshes.new_from_object(o)
    bpy.data.objects.remove(o)
    mesh.calc_loop_triangles()
    points = [v.co.copy() for v in mesh.vertices]
    faces = []
    for t in mesh.loop_triangles:
        a, b, c = t.vertices
        up = (points[b] - points[a]).cross(points[c] - points[a]).z
        if abs(up) > 1e-14:
            faces.append((a, b, c) if up > 0 else (a, c, b))
    if align == "CENTER":
        lo = [min(p[k] for p in points) for k in (0, 1)]
        hi = [max(p[k] for p in points) for k in (0, 1)]
        points = [Vector((p.x - (lo[0] + hi[0]) / 2, p.y - (lo[1] + hi[1]) / 2, 0.0)) for p in points]
    return points, faces


#
# Output
#


def pack(fmt, values):
    return base64.b64encode(struct.pack("<" + fmt * len(values), *values)).decode("ascii")


def flatten(mesh, axes):
    # Triangles, with a vertex for each normal its smooth faces share
    points = [Vector(axes(p)) for p in mesh[0]]
    triangles = []
    for face in mesh[1]:
        for i in range(1, len(face) - 1):
            triangles.append((face[0], face[i], face[i + 1]))
    normals = []
    at = {}
    kept = []
    for t in triangles:
        n = (points[t[1]] - points[t[0]]).cross(points[t[2]] - points[t[0]])
        if n.length < 1e-13:
            continue
        for v in t:
            at.setdefault(v, []).append(len(kept))
        kept.append(t)
        normals.append(n)
    units = [n.normalized() for n in normals]
    vertices = {}
    positions, directions, indices = [], [], []
    for k, t in enumerate(kept):
        for v in t:
            total = Vector()
            for other in at[v]:
                if units[other].dot(units[k]) > SMOOTH:
                    total += normals[other]
            total = total.normalized()
            key = (v, round(total.x, 2), round(total.y, 2), round(total.z, 2))
            if key not in vertices:
                vertices[key] = len(positions)
                positions.append(tuple(round(c, 6) for c in points[v]))
                directions.append(tuple(round(c, 4) for c in total))
            indices.append(vertices[key])
    return positions, directions, indices


def write(path, document):
    with open(path, "w", newline="\n") as f:
        f.write(json.dumps(document, indent=1) + "\n")


def part(node, material, mesh, axes, controllers=None, hand=None, occludes=True):
    positions, normals, indices = flatten(mesh, axes)
    return {"node": node, "occludes": occludes, "material": material, "controllers": controllers, "hand": hand, "positions": positions, "normals": normals, "indices": indices}


def occlusion(part, occluders):
    points, faces = [], []
    for p in occluders:
        base = len(points)
        points += p["positions"]
        faces += [tuple(base + i for i in p["indices"][t:t + 3]) for t in range(0, len(p["indices"]), 3)]
    tree = BVHTree.FromPolygons(points, faces)
    rays = []
    for k in range(OCCLUSION_RAYS):
        # Cosine weighted over the hemisphere, a Fibonacci spiral
        z = math.sqrt(1 - (k + 0.5) / OCCLUSION_RAYS)
        a = k * math.pi * (3 - math.sqrt(5))
        r = math.sqrt(1 - z * z)
        rays.append((r * math.cos(a), r * math.sin(a), z))
    out = []
    for position, normal in zip(part["positions"], part["normals"]):
        n = Vector(normal).normalized()
        x = n.orthogonal().normalized()
        y = n.cross(x)
        start = Vector(position) + n * 0.0003
        hits = sum(1 for ray in rays if tree.ray_cast(start, x * ray[0] + y * ray[1] + n * ray[2], OCCLUSION_DISTANCE)[0] is not None)
        out.append(1.0 - hits / OCCLUSION_RAYS)
    return out


def srgb(linear):
    return linear * 12.92 if linear <= 0.0031308 else 1.055 * linear ** (1 / 2.4) - 0.055


def palette():
    rows = bytearray()
    for y in range(PALETTE_ROWS * PALETTE_CELL):
        color = MATERIALS[min(y // PALETTE_CELL, len(MATERIALS) - 1)]
        rows.append(0)
        for x in range(PALETTE_WIDTH):
            light = OCCLUSION_FLOOR + (1 - OCCLUSION_FLOOR) * x / (PALETTE_WIDTH - 1)
            rows += bytes(int(srgb(c * light) * 255 + 0.5) for c in color)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", PALETTE_WIDTH, PALETTE_ROWS * PALETTE_CELL, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")


def device(parts, nodes):
    # Each part is occluded by what a right Touch Plus has, and by itself
    common = [p for p in parts if (not p["controllers"] or "meta_touch_plus" in p["controllers"]) and p["hand"] != "left" and p["occludes"]]
    out = []
    for p in parts:
        row = (MATERIALS.index(p["material"]) + 0.5) / PALETTE_ROWS
        uvs = [c for value in occlusion(p, common if p in common else common + [p]) for c in ((0.5 + value * (PALETTE_WIDTH - 1)) / PALETTE_WIDTH, row)]
        entry = {"node": p["node"]}
        if p["controllers"]:
            entry["controllers"] = p["controllers"]
        if p["hand"]:
            entry["hand"] = p["hand"]
        entry.update({
            "vertices": len(p["positions"]),
            "triangles": len(p["indices"]) // 3,
            "positions": pack("f", [c for v in p["positions"] for c in v]),
            "normals": pack("f", [c for v in p["normals"] for c in v]),
            "uvs": pack("f", uvs),
            "indices": pack("H", p["indices"]),
        })
        out.append(entry)
    return {"format": "puppetxr-device-model", "version": 1, "source": "made by tools/blender/puppetxr_devices.py", "palette": [list(m) for m in MATERIALS],
            "texture": base64.b64encode(palette()).decode("ascii"), "nodes": nodes, "parts": out}


#
# Controller, the right hand's. The thumb side is -X, the trigger faces -Y, the head is at -Z
#

X, Y, Z = grip(1, 0, 0), grip(0, 1, 0), grip(0, 0, -1)
FACE_TILT = math.radians(30)  # the face leans to the player
NORMAL = (Z * math.cos(FACE_TILT) + Y * math.sin(FACE_TILT)).normalized()
FORWARD = (Z * math.sin(FACE_TILT) - Y * math.cos(FACE_TILT)).normalized()
CENTER = grip(0.0, -0.010, -0.0575)


def on_face(u, v, height=0.0):
    return CENTER + X * u + FORWARD * v + NORMAL * height


def handle_section(z, y, rx, ry, tilt=0.0):
    return (grip(0.0, y, z), X, Y * math.cos(tilt) - Z * math.sin(tilt), rx, ry, 2.3)


def head_section(height, rx, ry):
    return (on_face(0, 0, height), X, FORWARD, rx, ry, 2.25)


def blade_section(z, y, width, thickness, tilt):
    return (grip(0, y, z), X, Y * math.cos(tilt) + Z * math.sin(tilt), width, thickness, 2.8)


def pad_section(z, width, thickness):
    return (grip(-0.0030, -0.0178 + 0.10 * max(0.0, z), z), X, Y, width, thickness, 2.4)


def shell():
    handle = loft([handle_section(0.0605, 0.0065, 0.003, 0.004), handle_section(0.0600, 0.0065, 0.0085, 0.0105), handle_section(0.0570, 0.0060, 0.0130, 0.0160), handle_section(0.0480, 0.0048, 0.0160, 0.0200),
                   handle_section(0.0340, 0.0020, 0.0176, 0.0222), handle_section(0.0180, -0.0008, 0.0172, 0.0216), handle_section(0.0020, -0.0020, 0.0156, 0.0190), handle_section(-0.014, -0.0035, 0.0152, 0.0178, 0.1),
                   handle_section(-0.028, -0.0060, 0.0178, 0.0196, 0.25), handle_section(-0.040, -0.0090, 0.0215, 0.0222, 0.4), handle_section(-0.049, -0.011, 0.0215, 0.0215, 0.5)], n=40, sub=4)
    head = loft([head_section(-0.0255, 0.0155, 0.0155), head_section(-0.021, 0.0235, 0.0235), head_section(-0.0145, 0.0295, 0.0305), head_section(-0.008, 0.0322, 0.0337),
                 head_section(-0.0035, 0.0328, 0.0345), head_section(-0.0008, 0.0320, 0.0337), head_section(0.0, 0.0306, 0.0322)], n=56)
    points, faces = join(handle, head)
    mesh = bpy.data.meshes.new("shell")
    mesh.from_pydata(points, [], faces)
    o = bpy.data.objects.new("shell", mesh)
    bpy.context.collection.objects.link(o)
    remesh = o.modifiers.new("remesh", "REMESH")
    remesh.mode = "VOXEL"
    remesh.voxel_size = 0.0007
    smooth = o.modifiers.new("smooth", "SMOOTH")
    smooth.iterations = 14
    smooth.factor = 0.5
    mesh = bpy.data.meshes.new_from_object(o.evaluated_get(bpy.context.evaluated_depsgraph_get()))
    o.modifiers.clear()
    o.data = mesh
    decimate = o.modifiers.new("decimate", "DECIMATE")
    decimate.ratio = min(1.0, 2600 / (2 * len(mesh.polygons)))
    mesh = bpy.data.meshes.new_from_object(o.evaluated_get(bpy.context.evaluated_depsgraph_get()))
    bpy.data.objects.remove(o)
    return [v.co.copy() for v in mesh.vertices], [tuple(p.vertices) for p in mesh.polygons]


def face_text(text, u, v, size, height):
    basis = Matrix((X, FORWARD, NORMAL)).transposed().to_4x4()
    basis.translation = on_face(u, v, height)
    return transformed(text_mesh(text, size), basis)


def mirrored(mesh, u):
    # Mirrored in place: the left controller mirrors the right one, which turns these back
    return transformed(mesh, Matrix.Translation((u, 0, 0)) @ Matrix.Scale(-1, 4, (1, 0, 0)) @ Matrix.Translation((-u, 0, 0)))


def button(u, v, radius):
    at = on_face(u, v, 0.0006)
    well = lathe(at, NORMAL, [(radius + 0.0010, -0.0004), (radius + 0.0010, 0.0005), (radius + 0.0004, 0.0006), (radius + 0.0002, 0.0002), (0, 0.0002)], 28)
    cap = lathe(at, NORMAL, [(radius, 0.0), (radius, 0.0017), (radius - 0.0005, 0.0023), (radius * 0.6, 0.0027), (0, 0.0029)], 28)
    return well, cap


def controller():
    axes = from_grip_axes
    parts = []
    add = lambda *a, **k: parts.append(part(*a, **k))
    primary, secondary, menu, stick = (0.0105, -0.0020, 0.0058), (0.0150, 0.0122, 0.0058), (-0.0090, -0.0140, 0.0032), (-0.0118, 0.0062)
    wells, caps = zip(*[button(*b) for b in (primary, secondary, menu)])

    house = loft([blade_section(-0.0615, -0.0240, 0.0060, 0.0040, 0.5), blade_section(-0.0585, -0.0275, 0.0115, 0.0085, 0.45), blade_section(-0.0500, -0.0290, 0.0130, 0.0095, 0.3),
                  blade_section(-0.0400, -0.0270, 0.0120, 0.0080, 0.0), blade_section(-0.0340, -0.0235, 0.0080, 0.0050, -0.2)], n=28, sub=2)
    plate = loft([head_section(-0.0020, 0.0282, 0.0298), head_section(0.0002, 0.0282, 0.0298), head_section(0.0008, 0.0276, 0.0292), head_section(0.0010, 0.0262, 0.0278), head_section(0.0008, 0.020, 0.0215)], n=48, caps=(False, True))
    band = loft([handle_section(0.0540, 0.0056, 0.0146, 0.0182), handle_section(0.0536, 0.0056, 0.0150, 0.0187), handle_section(0.0524, 0.0055, 0.0152, 0.0190), handle_section(0.0520, 0.0055, 0.0150, 0.0187)], n=40, caps=(False, False))
    rest = lathe(on_face(0.0130, -0.0178, 0.0007), NORMAL, [(0.0066, 0), (0.0064, 0.0006), (0.0056, 0.0008), (0.004, 0.0004), (0, 0.0001)], 24, squash=(1.1, 0.8))
    at = on_face(*stick)
    collar = lathe(at + NORMAL * 0.0006, NORMAL, [(0.0112, 0), (0.0110, 0.0010), (0.0100, 0.0014), (0.0092, 0.0008), (0.0062, 0.0003), (0, 0.0003)], 32)
    add("body", SHELL, shell(), axes)
    add("body", FACE, join(plate, house), axes)
    # The shell has few vertices around the band, which would spread its thin shadow over the handle
    add("body", ACCENT, band, axes, occludes=False)
    add("body", RUBBER, wells[2], axes)
    add("body", RUBBER, join(rest, wells[0], wells[1]), axes, controllers=TOUCH)
    add("body", ACCENT, collar, axes, controllers=TOUCH)
    # Touch Pro finds itself with cameras
    lenses = [lathe(p, n, [(0.0042, -0.0010), (0.0042, 0.0004), (0.0034, 0.0007), (0.0030, 0.0003), (0, 0.0006)], 20)
              for p, n in ((on_face(0.0, 0.0325, -0.0075), FORWARD), (on_face(0.0312, 0.008, -0.0075), X), (on_face(0.016, -0.0300, -0.0075), X * 0.45 - FORWARD))]
    add("body", LENS, join(*lenses), axes, controllers=["meta_touch_pro"])

    trigger = loft([blade_section(-0.0640, -0.0345, 0.0030, 0.0018, 0.75), blade_section(-0.0625, -0.0360, 0.0082, 0.0052, 0.7), blade_section(-0.0570, -0.0395, 0.0098, 0.0074, 0.5),
                    blade_section(-0.0490, -0.0405, 0.0100, 0.0078, 0.2), blade_section(-0.0405, -0.0385, 0.0098, 0.0070, -0.1), blade_section(-0.0320, -0.0372, 0.0094, 0.0054, -0.1),
                    blade_section(-0.0240, -0.0392, 0.0086, 0.0042, 0.15), blade_section(-0.0205, -0.0412, 0.0060, 0.0026, 0.3), blade_section(-0.0198, -0.0418, 0.0015, 0.0008, 0.3)], n=24, sub=2)
    add("trigger", BUTTON, trigger, axes)
    squeeze = loft([pad_section(0.0285, 0.0012, 0.0006), pad_section(0.0280, 0.0050, 0.0030), pad_section(0.0250, 0.0082, 0.0052), pad_section(0.0170, 0.0096, 0.0064),
                    pad_section(0.0080, 0.0096, 0.0064), pad_section(0.0000, 0.0084, 0.0052), pad_section(-0.0032, 0.0052, 0.0030), pad_section(-0.0038, 0.0012, 0.0006)], n=24, sub=2)
    add("squeeze", BUTTON, squeeze, axes, controllers=TOUCH)

    cap = lathe(at, NORMAL, [(0.0046, 0.0002), (0.0040, 0.0036), (0.0052, 0.0058), (0.0078, 0.0064), (0.0084, 0.0072), (0.0084, 0.0082), (0.0078, 0.0090), (0.0066, 0.0091), (0.0050, 0.0083), (0.0025, 0.0078), (0, 0.0076)], 32)
    ribs = []
    for i in range(16):
        a = 2 * math.pi * i / 16
        out = X * math.cos(a) + FORWARD * math.sin(a)
        ribs.append(lathe(at + out * 0.0084 + NORMAL * 0.0077, out, [(0.00075, -0.0002), (0.0006, 0.00035), (0, 0.0005)], 5))
    add("thumbstick", RUBBER, join(cap, *ribs), axes, controllers=TOUCH)

    for node, b, mesh, letters in (("primary", primary, caps[0], "AX"), ("secondary", secondary, caps[1], "BY")):
        add(node, BUTTON, mesh, axes, controllers=TOUCH)
        right, left = (face_text(c, b[0], b[1], 0.0072, 0.00365) for c in letters)
        add(node, LABEL, right, axes, controllers=TOUCH, hand="right")
        add(node, LABEL, mirrored(left, b[0]), axes, controllers=TOUCH, hand="left")
    add("menu", BUTTON, caps[2], axes)
    # The first Touch generations track through a ring around the hand
    add("ring", FACE, torus(grip(0.0, -0.030, -0.036), Y, Z, X, 0.052, 0.0055, 48, 10), axes, controllers=["oculus_touch"])

    vec = lambda v: [round(c, 6) for c in axes(v)]
    sink = lambda depth: vec(NORMAL * -depth)
    none = [0.0, 0.0, 0.0]
    # turn: rotation vectors (axis times radians) at 1 of the node's two values; travel at 1 of its press
    nodes = [
        {"name": "trigger", "pivot": vec(grip(0, -0.0330, -0.0600)), "turn": [vec(X * -0.24), none], "travel": none},
        {"name": "squeeze", "pivot": none, "turn": [none, none], "travel": vec(Y * 0.0022)},
        {"name": "thumbstick", "pivot": vec(at), "turn": [vec(NORMAL.cross(X) * 0.35), vec(NORMAL.cross(FORWARD) * 0.35)], "travel": sink(0.0012)},
        {"name": "primary", "pivot": none, "turn": [none, none], "travel": sink(0.0015)},
        {"name": "secondary", "pivot": none, "turn": [none, none], "travel": sink(0.0015)},
        {"name": "menu", "pivot": none, "turn": [none, none], "travel": sink(0.0015)},
    ]
    return device(parts, nodes)


#
# Physical keyboard, 30 x 11.2 x 1.8 cm: Z is up here
#


def keyboard():
    axes = lambda p: (p.x, p.z - 0.003, -p.y)  # centred: from -6 to 12 mm
    pitch, top, legend = 0.019, 0.0116, 0.012
    caps, modifiers, legends = [], [], []

    def key(x, y, half, text):
        mesh = rounded_box(x, y, half, 0.0085, 0.0020, 0.0060, top, 0.0007, taper=0.0009, seg=2, steps=(90,), bottom=False)
        (modifiers if len(text) > 1 else caps).append(mesh)
        if text:
            # strokes about a millimetre wide: thinner ones fall between the pixels of passthrough
            letters = text_mesh(text, 0.0080 if len(text) == 1 else 0.0046, resolution=1, bolder=0.00016 if len(text) == 1 else 0.00006)
            legends.append(transformed(letters, Matrix.Translation((x, y, legend))))

    rows = [["esc"] + list("1234567890-=\\") + ["del"], ["tab"] + list("QWERTYUIOP[]") + ["ret"], ["caps"] + list("ASDFGHJKL;'") + ["ret"], ["shift"] + list("ZXCVBNM,./") + ["shift"]]
    for r, row in enumerate(rows):
        n = len(row)
        edge = 0.1425 - n * pitch / 2
        for k, text in enumerate(row):
            x, half = (k - 0.5 * (n - 1)) * pitch, 0.0085
            if n < 15 and k in (0, n - 1):
                x += math.copysign(edge / 2, k - 0.5)
                half += edge / 2
            key(x, 0.038 - r * pitch, half, text)
    y = 0.038 - 4 * pitch
    key(0.0, y, 3 * pitch - 0.0005, "")
    for k, (left, right) in enumerate((("cmd", "cmd"), ("alt", "alt"), ("fn", "ctrl"), ("ctrl", "fn"))):
        key(-(3.5 + k) * pitch, y, 0.0085, left)
        key((3.5 + k) * pitch, y, 0.0085, right)
    parts = [
        part("body", ALUMINIUM, rounded_box(0, 0, 0.150, 0.056, 0.009, -0.006, 0.006, 0.0016, seg=6, steps=(30, 60, 90)), axes),
        part("body", WELL, rounded_box(0, 0, 0.1455, 0.0500, 0.005, 0.0055, 0.0063, 0.0003, seg=4, bottom=False), axes),
        part("keys", CAP, join(*caps), axes),
        part("keys", MODIFIER, join(*modifiers), axes),
        part("keys", LEGEND, join(*legends), axes),
    ]
    return device(parts, [])


#
# Virtual keyboard labels
#

GLYPH_HEIGHT = 44  # texels, all glyphs
BASELINE = 31      # texels from the top of a glyph
SAMPLES = 8        # per texel and axis
CHARACTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789,.?!"


def stroke(points, width, closed=False):
    # A polyline as a union of quads with square ends, texels, y up
    faces, out = [], []
    pairs = list(zip(points, points[1:])) + ([(points[-1], points[0])] if closed else [])
    for a, b in pairs:
        a, b = Vector((a[0], a[1], 0)), Vector((b[0], b[1], 0))
        d = (b - a).normalized() * width / 2
        n = Vector((-d.y, d.x, 0))
        base = len(out)
        out += [a - d + n, a - d - n, b + d - n, b + d + n]
        faces += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    return out, faces


def coverage(mesh, width):
    # Supersampled, the union of the triangles: y up, the baseline at 0
    points, faces = mesh
    grid = bytearray(width * SAMPLES * GLYPH_HEIGHT * SAMPLES)
    columns, rows = width * SAMPLES, GLYPH_HEIGHT * SAMPLES
    for face in faces:
        a, b, c = (((points[i].x) * SAMPLES, (BASELINE - points[i].y) * SAMPLES) for i in face)
        for row in range(max(0, math.floor(min(a[1], b[1], c[1]))), min(rows, math.ceil(max(a[1], b[1], c[1])) + 1)):
            y = row + 0.5
            cuts = []
            for p, q in ((a, b), (b, c), (c, a)):
                if (p[1] <= y) != (q[1] <= y):
                    cuts.append(p[0] + (y - p[1]) * (q[0] - p[0]) / (q[1] - p[1]))
            if len(cuts) == 2:
                for x in range(max(0, math.ceil(min(cuts) - 0.5)), min(columns, math.floor(max(cuts) - 0.5) + 1)):
                    grid[row * columns + x] = 1
    out = []
    for y in range(GLYPH_HEIGHT):
        for x in range(width):
            total = sum(sum(grid[(y * SAMPLES + s) * columns + x * SAMPLES:(y * SAMPLES + s) * columns + (x + 1) * SAMPLES]) for s in range(SAMPLES))
            out.append((total * 255 + SAMPLES * SAMPLES // 2) // (SAMPLES * SAMPLES))
    return out


def glyphs():
    entries, data = [], []

    def add(code, mesh):
        left = min(p.x for p in mesh[0])
        right = max(p.x for p in mesh[0])
        width = math.ceil(right - left) + 2
        shift = (width - (right - left)) / 2 - left
        mesh = ([Vector((p.x + shift, p.y, 0)) for p in mesh[0]], mesh[1])
        entries.append([code, width])
        data.extend(coverage(mesh, width))

    for c in CHARACTERS:
        add(ord(c), text_mesh(c, 52.0 if c in ",." else 38.0, resolution=6, align="LEFT"))
    add(ord(" "), text_mesh("space", 26.0, resolution=6, align="LEFT"))
    up = 12.0  # the middle of a capital
    scaled = lambda pts, k: [(x * k, y * k + up) for x, y in pts]
    backspace = join(stroke(scaled([(8, 4.5), (-2.5, 4.5), (-7.5, 0), (-2.5, -4.5), (8, -4.5)], 2.1), 2.6, closed=True), stroke(scaled([(0.5, 2), (4.5, -2)], 2.1), 2.6), stroke(scaled([(4.5, 2), (0.5, -2)], 2.1), 2.6))
    add(8, backspace)
    add(10, join(stroke(scaled([(7, 4.5), (7, -1.5), (-6, -1.5)], 2.1), 2.8), stroke(scaled([(-2.5, 2), (-6, -1.5), (-2.5, -5)], 2.1), 2.8)))
    add(14, stroke(scaled([(-5, -0.5), (0, 5.5), (5, -0.5), (2.4, -0.5), (2.4, -5), (-2.4, -5), (-2.4, -0.5)], 2.3), 2.6, closed=True))
    return {"format": "puppetxr-glyphs", "version": 1, "source": "made by tools/blender/puppetxr_devices.py, letters in Inter (SIL Open Font License)",
            "height": GLYPH_HEIGHT, "glyphs": entries, "coverage": pack("B", data)}


def main():
    out = sys.argv[sys.argv.index("--") + 1]
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    os.makedirs(out, exist_ok=True)
    for name, make in (("controller", controller), ("keyboard", keyboard), ("glyphs", glyphs)):
        document = make()
        write(os.path.join(out, name + ".json"), document)
        for p in document.get("parts", []):
            print("PART", name, p["node"], p["vertices"], p["triangles"], p.get("controllers", ""), p.get("hand", ""))
        if "parts" in document:
            print("TOTAL", name, sum(p["vertices"] for p in document["parts"]), sum(p["triangles"] for p in document["parts"]))


main()
