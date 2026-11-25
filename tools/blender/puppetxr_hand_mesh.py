# Fits the sculpted hand of data/motion/hand.blend to the skeleton of data/motion/hand_model.json and
# writes data/motion/hand_mesh.json, the skinned mesh of the simulated tracked hand
# (XR_FB_hand_tracking_mesh, docs/openxr.md):
#
#   blender -b data/motion/hand.blend --factory-startup --disable-autoexec --python tools/blender/puppetxr_hand_mesh.py -- data/motion/hand_model.json data/motion/hand_mesh.json
#
# The sculpt is a right hand with its forearm, fingers down, its back to -Y; empties named after the
# joints mark where its bones are. Each bone maps the sculpt around it onto the same bone of the open
# hand of the model: turned, stretched along the bone to its length, scaled across it, blended by
# Blender's heat weights. Those weights, the two largest per vertex, skin the result. Rerun after
# refitting the hand model; Sim.the_hand_mesh_fits_the_hand_model checks that.

import base64
import json
import math
import os
import struct
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import hand_fit as h  # noqa: E402

FOREARM = 0.015  # m behind the wrist joint where the sculpt is cut: a Quest's hand ends where it goes over into the arm
SMOOTHING = 4    # passes over the weights
WRIST = h.WRIST
NAMES = ["thumb", "index", "middle", "ring", "little"]
# (name, joint the skin follows, joints the bone runs between). Meta's hand skeleton has no palm nor
# index, middle or ring metacarpal: they move with the wrist, and OVRPlugin cannot map a vertex skinned
# to one
BONES = [("forearm", WRIST, "elbow", "wrist")]
BONES += [("thumb_metacarpal", 2, 2, 3), ("thumb_proximal", 3, 3, 4), ("thumb_distal", 4, 4, 5)]
for finger in range(4):
    base = 6 + 5 * finger
    metacarpal = base if finger == 3 else WRIST
    BONES += [(NAMES[finger + 1] + "_metacarpal", metacarpal, base, base + 1)]
    BONES += [(NAMES[finger + 1] + "_" + part, base + 1 + i, base + 1 + i, base + 2 + i) for i, part in enumerate(("proximal", "intermediate", "distal"))]


def to_vector(p):
    return Vector((p[0], p[1], p[2]))


def rotation_between(a, b):
    return a.normalized().rotation_difference(b.normalized()).to_matrix()


def sculpt():
    hand = next(o for o in bpy.data.objects if o.type == "MESH")
    mesh = hand.data.copy()
    mesh.transform(hand.matrix_world)
    marks = {o.name: o.matrix_world.translation.copy() for o in bpy.data.objects if o.type == "EMPTY"}
    cut_forearm(mesh, marks["joint%02d" % WRIST].z + FOREARM)
    return mesh, marks


def cut_forearm(mesh, height):
    # The arm goes up: cut across it and close the end with a rim rounded over two rings
    bm = bmesh.new()
    bm.from_mesh(mesh)
    # dist: an edge loop almost on the plane is moved onto it rather than cut into slivers
    bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], dist=0.002, plane_co=(0, 0, height), plane_no=(0, 0, 1), clear_outer=True)
    loop = [next(e for e in bm.edges if e.is_boundary).verts[0]]
    while True:
        around = [e.other_vert(loop[-1]) for e in loop[-1].link_edges if e.is_boundary]
        following = [v for v in around if v not in loop[-2:]] or around
        if following[0] == loop[0]:
            break
        loop.append(following[0])
    # The cut leaves the rim uneven, which a glossy skin shows: even it out, and the rings below it less
    # and less, then close it with a dome
    rings = [loop]
    for _ in range(4):
        known = {v for ring in rings for v in ring}
        rings.append([next((e.other_vert(v) for e in v.link_edges if e.other_vert(v) not in known and e.other_vert(v).co.z < v.co.z), None) for v in rings[-1]])
        # Not a loop of its own: where faces meet unevenly, two vertices share their neighbour
        if any(v is None for v in rings[-1]) or len(set(rings[-1])) < len(rings[-1]):
            rings.pop()
            break
    for depth, ring in enumerate(rings):
        for _ in range(12 >> depth):
            even = [(ring[k - 1].co + ring[k].co * 2.0 + ring[(k + 1) % len(ring)].co) / 4.0 for k in range(len(ring))]
            for v, p in zip(ring, even):
                v.co = p
    for v in loop:
        v.co.z = height
    center = sum((v.co for v in loop), Vector()) / len(loop)
    ring = loop
    for step in range(1, 6):
        angle = math.radians(90.0 * step / 6.0)
        inner = [bm.verts.new(center + (v.co - center) * math.cos(angle) ** 0.6 + Vector((0, 0, 0.006 * math.sin(angle)))) for v in loop]
        for k in range(len(loop)):
            n = (k + 1) % len(loop)
            bm.faces.new((ring[k], ring[n], inner[n], inner[k]))
        ring = inner
    top = bm.verts.new(center + Vector((0, 0, 0.006)))
    for k in range(len(ring)):
        bm.faces.new((ring[k], ring[(k + 1) % len(ring)], top))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()


def heat_weights(mesh, segments):
    # Blender's automatic weights: bones named as in BONES, per vertex name -> weight
    hand = bpy.data.objects.new("fit", mesh)
    armature = bpy.data.objects.new("bones", bpy.data.armatures.new("bones"))
    for o in (hand, armature):
        bpy.context.collection.objects.link(o)
    bpy.context.view_layer.objects.active = armature
    bpy.ops.object.mode_set(mode="EDIT")
    for name, (head, tail) in segments.items():
        bone = armature.data.edit_bones.new(name)
        bone.head, bone.tail = head, tail
    bpy.ops.object.mode_set(mode="OBJECT")
    for o in bpy.context.selected_objects:
        o.select_set(False)
    hand.select_set(True)
    armature.select_set(True)
    bpy.context.view_layer.objects.active = armature
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")
    names = {g.index: g.name for g in hand.vertex_groups}
    weights = [{names[g.group]: g.weight for g in v.groups if g.weight > 1e-4} for v in mesh.vertices]
    if any(not w for w in weights):
        sys.exit("puppetxr_hand_mesh.py: the heat weights leave vertices without a bone")
    # Heat weights change within one ring of faces where a finger is thin, which bends with a hard edge:
    # average each vertex with its neighbours, so a bend spreads over the rings around the joint
    weights = [{n: x / sum(w.values()) for n, x in w.items()} for w in weights]
    neighbours = [[] for _ in weights]
    for a, b in mesh.edge_keys:
        neighbours[a].append(b)
        neighbours[b].append(a)
    for _ in range(SMOOTHING):
        smooth = []
        for v, w in enumerate(weights):
            mixed = {n: 0.5 * x for n, x in w.items()}
            for other in neighbours[v]:
                for n, x in weights[other].items():
                    mixed[n] = mixed.get(n, 0.0) + 0.5 * x / len(neighbours[v])
            smooth.append(mixed)
        weights = smooth
    return weights


def main():
    model_path, out_path = sys.argv[sys.argv.index("--") + 1:][:2]
    with open(model_path) as f:
        model = json.load(f)
    bind = h.forward([tuple(j["offset"]) for j in model["joints"]], [tuple(q) for q in model["poses"]["open"]])
    target = [to_vector(b[0]) for b in bind]
    mesh, marks = sculpt()
    source = lambda j: marks["joint%02d" % j]

    # The whole palm first: the similarity that takes the sculpt's wrist, knuckles and thumb base to
    # the model's, so each bone only turns what is left
    anchors = [WRIST, 2, 7, 12, 17, 22]
    cs = sum((source(j) for j in anchors), Vector()) / len(anchors)
    ct = sum((target[j] for j in anchors), Vector()) / len(anchors)
    scale = math.sqrt(sum((target[j] - ct).length_squared for j in anchors) / sum((source(j) - cs).length_squared for j in anchors))
    import numpy as np
    covariance = sum(np.outer(np.array(target[j] - ct), np.array(source(j) - cs)) for j in anchors)
    u, _, vt = np.linalg.svd(covariance)
    d = np.diag([1.0, 1.0, np.sign(np.linalg.det(u @ vt))])
    palm = Matrix((u @ d @ vt).tolist())

    segments = {}
    maps = {}
    for name, _, a, b in BONES:
        if a == "elbow":
            head, tail = source(WRIST) + Vector((0, 0, FOREARM)), source(WRIST)
            direction = palm @ (tail - head)
            goal_tail, goal_head = target[WRIST], target[WRIST] - direction.normalized() * FOREARM
        else:
            head, tail, goal_head, goal_tail = source(a), source(b), target[a], target[b]
        # the index, middle and ring metacarpals of a Quest start beside the wrist
        segments[name] = (head, tail)
        along = (tail - head).normalized()
        turn = rotation_between(palm @ along, goal_tail - goal_head) @ palm
        stretch = (goal_tail - goal_head).length / (tail - head).length
        # scaled across the bone, stretched along it
        shape = Matrix.Identity(3) * scale
        for r in range(3):
            for c in range(3):
                shape[r][c] += (stretch - scale) * along[r] * along[c]
        maps[name] = (turn @ shape, head, goal_head)

    joint_of = {name: joint for name, joint, _, _ in BONES}
    weights = heat_weights(mesh, segments)
    positions = []
    for v, w in zip(mesh.vertices, weights):
        p = Vector()
        for name, x in w.items():
            m, head, goal = maps[name]
            p += (goal + m @ (v.co - head)) * x
        positions.append(p)

    joints, pairs = [], []
    for w in weights:
        per_joint = {}
        for name, x in w.items():
            per_joint[joint_of[name]] = per_joint.get(joint_of[name], 0.0) + x
        top = sorted(per_joint.items(), key=lambda x: (-x[1], x[0]))[:2]
        first = round(top[0][1] / sum(x for _, x in top), 3)
        joints.append((top[0][0], top[1][0] if len(top) > 1 and first < 1.0 else top[0][0]))
        pairs.append((first, round(1.0 - first, 3)))

    triangles = []
    for polygon in mesh.polygons:
        corners = list(polygon.vertices)
        if len(corners) == 4 and (positions[corners[0]] - positions[corners[2]]).length > (positions[corners[1]] - positions[corners[3]]).length:
            corners = corners[1:] + corners[:1]
        for i in range(1, len(corners) - 1):
            triangles += [corners[0], corners[i], corners[i + 1]]
    normals = [Vector() for _ in positions]
    for t in range(0, len(triangles), 3):
        a, b, c = triangles[t:t + 3]
        n = (positions[b] - positions[a]).cross(positions[c] - positions[a])
        for x in (a, b, c):
            normals[x] += n
    normals = [n.normalized() for n in normals]

    # Texture coordinates: along the hand and around it
    along = (target[12] - target[WRIST]).normalized()
    back = target[WRIST] - along * FOREARM
    uvs = []
    for p in positions:
        uvs += [round(math.atan2(p[1], p[0]) / (2 * math.pi) + 0.5, 4), round(min(max((p - back).dot(along) / 0.3, 0.0), 1.0), 4)]

    def pack(fmt, values):
        return base64.b64encode(struct.pack("<" + fmt * len(values), *values)).decode("ascii")

    document = {
        "format": "puppetxr-hand-mesh",
        "version": 1,
        "source": "fitted by tools/blender/puppetxr_hand_mesh.py to the open hand of data/motion/hand_model.json",
        "bind": [[round(c, 5) for c in b[0]] for b in bind],
        "vertices": len(positions),
        "triangles": len(triangles) // 3,
        "positions": pack("f", [c for p in positions for c in p]),
        "normals": pack("f", [c for n in normals for c in n]),
        "uvs": pack("f", uvs),
        "joints": pack("B", [j for pair in joints for j in pair]),
        "weights": pack("f", [w for pair in pairs for w in pair]),
        "indices": pack("H", triangles),
    }
    with open(out_path, "w") as f:
        f.write(json.dumps(document, indent=1) + "\n")
    print("HAND", len(positions), "vertices", len(triangles) // 3, "triangles, scale", round(scale, 4))


main()
