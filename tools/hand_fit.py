#!/usr/bin/env python3
#
# Fits motion/hand_model.json, the simulated tracked hand (docs/hand-motion.md, Hand model), to
# takes of the motion recorder (docs/recorder.md):
#
#   python3 tools/hand_fit.py <takes dir>... -o motion/hand_model.json
#
# A take is used by its step name: still_open, still_relaxed, fist, pinch_tap_* and pinch_hold_*
# for the poses, the takes with XR_EXT_hand_interaction values for the pinch and grasp curves.
# Both hands go into one right hand, the left mirrored across its wrist's X; the simulator mirrors
# it back. Standard library only.

import argparse
import json
import math
import os
import sys

JOINTS = 26
PALM, WRIST = 0, 1
THUMB_TIP, INDEX_TIP = 5, 10
FINGER_TIPS = (10, 15, 20, 25)
NAMES = [
    "palm", "wrist",
    "thumb_metacarpal", "thumb_proximal", "thumb_distal", "thumb_tip",
    "index_metacarpal", "index_proximal", "index_intermediate", "index_distal", "index_tip",
    "middle_metacarpal", "middle_proximal", "middle_intermediate", "middle_distal", "middle_tip",
    "ring_metacarpal", "ring_proximal", "ring_intermediate", "ring_distal", "ring_tip",
    "little_metacarpal", "little_proximal", "little_intermediate", "little_distal", "little_tip",
]
# XrHandJointEXT parents; the palm hangs off the wrist, the wrist is the root
PARENTS = [WRIST, None, 1, 2, 3, 4, 1, 6, 7, 8, 9, 1, 11, 12, 13, 14, 1, 16, 17, 18, 19, 1, 21, 22, 23, 24]
POSITION_VALID = 2
PINCH_GAP = 0.008     # thumb and index tips closer than this make the pinch pose
FIST_SHARE = 0.1      # the most curled tenth of the fist take's frames make the fist pose


def q_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def q_conj(q):
    return (-q[0], -q[1], -q[2], q[3])


def q_rotate(q, v):
    p = q_mul(q_mul(q, (v[0], v[1], v[2], 0.0)), q_conj(q))
    return (p[0], p[1], p[2])


def mirror(joint):
    # The left hand's joints mirrored across X: positions negate x, rotations keep x and w
    x, y, z, qx, qy, qz, qw = joint[:7]
    return (-x, y, z, qx, -qy, -qz, qw)


def frames_of(path, use_left=True):
    with open(path) as f:
        take = json.load(f)
    for frame in take["frames"]:
        for side in ("right", "left") if use_left else ("right",):
            hand = frame[side]
            if not hand.get("active") or not all(flag & POSITION_VALID for flag in hand.get("flags", [0])):
                continue
            joints = [tuple(j) for j in hand["joints"]]
            if side == "left":
                joints = [mirror(j) + (j[7],) for j in joints]
            yield joints, hand.get("interaction")


def wrist_local(joints):
    w = joints[WRIST]
    inv = q_conj(w[3:7])
    out = []
    for j in joints:
        p = q_rotate(inv, (j[0] - w[0], j[1] - w[1], j[2] - w[2]))
        out.append(p + q_mul(inv, j[3:7]))
    return out


def local_rotations(joints):
    out = []
    for j in range(JOINTS):
        p = PARENTS[j]
        out.append((0.0, 0.0, 0.0, 1.0) if p is None else q_mul(q_conj(joints[p][3:7]), joints[j][3:7]))
    return out


def gap(joints, a, b):
    return math.dist(joints[a][:3], joints[b][:3])


def curl(joints):
    return sum(gap(joints, t, PALM) for t in FINGER_TIPS[1:]) / 3.0


TIPS = (THUMB_TIP,) + FINGER_TIPS
MEDOID_CANDIDATES = 400


def tips_apart(a, b):
    return sum(math.dist(a[t][:3], b[t][:3]) for t in TIPS)


def medoid(samples):
    # The recorded frame closest to all others: an average of the rotations of thumb and index
    # separately opens a pinch that closes in every frame
    step = max(1, len(samples) // MEDOID_CANDIDATES)
    candidates = samples[::step]
    return min(candidates, key=lambda c: sum(tips_apart(c, o) for o in candidates))


def forward(offsets, rotations):
    out = [None] * JOINTS
    order = [WRIST] + [j for j in range(JOINTS) if j != WRIST]
    for j in order:
        p = PARENTS[j]
        if p is None:
            out[j] = ((0.0, 0.0, 0.0), rotations[j])
        else:
            pp, pq = out[p]
            o = q_rotate(pq, offsets[j])
            out[j] = ((pp[0] + o[0], pp[1] + o[1], pp[2] + o[2]), q_mul(pq, rotations[j]))
    return out


def q_axis_angle(axis, radians):
    h = radians * 0.5
    return (axis[0] * math.sin(h), axis[1] * math.sin(h), axis[2] * math.sin(h), math.cos(h))


THUMB_TURNS = ((2, (1.0, 0.0, 0.0)), (2, (0.0, 1.0, 0.0)), (3, (1.0, 0.0, 0.0)), (4, (1.0, 0.0, 0.0)))


def close_pinch(offsets, rotations, target):
    # The pose's thumb comes from a frame of another session than the bones: turn its joints
    # (metacarpal about X and Y, proximal and distal about X) until the tips are as far apart as
    # in the recorded pinches
    rotations = list(rotations)

    def error(r):
        fk = forward(offsets, r)
        return (math.dist(fk[THUMB_TIP][0], fk[INDEX_TIP][0]) - target) ** 2

    step = math.radians(4.0)
    while step > math.radians(0.01):
        improved = False
        for joint, axis in THUMB_TURNS:
            for sign in (1.0, -1.0):
                trial = list(rotations)
                trial[joint] = q_mul(rotations[joint], q_axis_angle(axis, sign * step))
                if error(trial) < error(rotations):
                    rotations, improved = trial, True
        if not improved:
            step *= 0.5
    return rotations


COUPLING_TAKES = ["curl_each", "poses", "fist", "spread"]
COUPLING_CURLED = 0.8     # a finger at least this far from the open hand toward the fist is curled
COUPLING_PERCENTILE = 1   # how open a neighbour stays at the least, so a few noisy frames do not count
# Fingers whose tendons are joined: the middle and ring and the ring and little share muscles. The
# index has its own extensor, and the takes hold no tight point to measure it by, so it moves alone
COUPLED = {(2, 3), (3, 2), (3, 4), (4, 3), (2, 4), (4, 2)}


def nlerp(a, b, t):
    if sum(x * y for x, y in zip(a, b)) < 0.0:
        b = tuple(-c for c in b)
    q = tuple(x + (y - x) * t for x, y in zip(a, b))
    n = math.sqrt(sum(c * c for c in q))
    return tuple(c / n for c in q)


def finger_joints(finger):
    first = 2 if finger == 0 else 6 + (finger - 1) * 5
    return range(first, first + (4 if finger == 0 else 5))


def weight_for_curl(offsets, poses, finger, curl):
    # The sim blends a finger's joint rotations from the open pose toward the fist (nlerp); the
    # weight whose tip lands at this curl, as tips do not move linearly with the weight
    tip = (THUMB_TIP,) + FINGER_TIPS

    def tip_curl(w):
        rotations = list(poses["open"])
        for j in finger_joints(finger):
            rotations[j] = nlerp(poses["open"][j], poses["fist"][j], w)
        fk = forward(offsets, rotations)
        o = math.dist(forward(offsets, poses["open"])[tip[finger]][0], forward(offsets, poses["open"])[PALM][0])
        f = math.dist(forward(offsets, poses["fist"])[tip[finger]][0], forward(offsets, poses["fist"])[PALM][0])
        return (o - math.dist(fk[tip[finger]][0], fk[PALM][0])) / (o - f)

    low, high = 0.0, 1.0
    for _ in range(30):
        mid = (low + high) / 2
        low, high = (mid, high) if tip_curl(mid) < curl else (low, mid)
    return high


def coupling(offsets, poses, samples):
    # How far each finger, index to little, stays curled at the least while another is curled,
    # on the scale from the open pose's tip (0) to the fist pose's (1), as the sim's blend weight
    fk_open = forward(offsets, poses["open"])
    fk_fist = forward(offsets, poses["fist"])
    tips = (THUMB_TIP,) + FINGER_TIPS

    def curls(s):
        out = []
        for t in tips:
            o = math.dist(fk_open[t][0], fk_open[PALM][0])
            f = math.dist(fk_fist[t][0], fk_fist[PALM][0])
            out.append((o - math.dist(s[t][:3], s[PALM][:3])) / (o - f))
        return out

    table = [[1.0 if f == g else 0.0 for g in range(1, 5)] for f in range(1, 5)]
    measured = [curls(s) for s in samples]
    for f in range(1, 5):
        curled = [c for c in measured if c[f] >= COUPLING_CURLED]
        for g in range(1, 5):
            if (f, g) in COUPLED and curled:
                values = sorted(c[g] for c in curled)
                curl = max(0.0, min(1.0, values[len(values) * COUPLING_PERCENTILE // 100]))
                table[f - 1][g - 1] = round(weight_for_curl(offsets, poses, g, curl), 3)
    return table


def curve(pairs, width, top):
    # Medians in bins of width from 0 to top, made monotonically non-increasing from the closest bin
    points = []
    for i in range(int(round(top / width))):
        lo, hi = i * width, (i + 1) * width
        values = sorted(v for g, v in pairs if lo <= g < hi)
        if len(values) >= 20:
            points.append([round(lo + width / 2, 4), values[len(values) // 2]])
    for i in range(1, len(points)):
        points[i][1] = min(points[i][1], points[i - 1][1])
    return [[g, round(v, 3)] for g, v in points]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("takes", nargs="+")
    parser.add_argument("-o", "--output", required=True)
    args = parser.parse_args()

    takes = {}
    for d in args.takes:
        for name in sorted(os.listdir(d)):
            if name.endswith(".json"):
                step = name[:-5].split("_", 1)[1]
                takes.setdefault(step, []).append(os.path.join(d, name))

    def collect(prefixes):
        out = []
        for step, paths in takes.items():
            if any(step.startswith(p) for p in prefixes):
                for path in paths:
                    out.extend(frames_of(path))
        return out

    opened = [wrist_local(j) for j, _ in collect(["still_open"])]
    relaxed = [wrist_local(j) for j, _ in collect(["still_relaxed"])]
    pinching = [wrist_local(j) for j, _ in collect(["pinch_tap", "pinch_hold"]) if gap(j, THUMB_TIP, INDEX_TIP) < PINCH_GAP]
    fisted = sorted((wrist_local(j) for j, _ in collect(["fist"])), key=curl)
    fisted = fisted[: max(1, int(len(fisted) * FIST_SHARE))]
    if not (opened and relaxed and pinching and fisted):
        sys.exit("hand_fit.py: needs still_open, still_relaxed, pinch_tap or pinch_hold, and fist takes")

    # Bone offsets in the parent's frame, fixed on a Quest 3 (bones vary by under 0.3 mm)
    offsets = []
    for j in range(JOINTS):
        p = PARENTS[j]
        if p is None:
            offsets.append((0.0, 0.0, 0.0))
            continue
        samples = [q_rotate(q_conj(s[p][3:7]), (s[j][0] - s[p][0], s[j][1] - s[p][1], s[j][2] - s[p][2])) for s in opened]
        offsets.append(tuple(sum(v[i] for v in samples) / len(samples) for i in range(3)))
    radii = []
    for j in range(JOINTS):
        values = sorted(f[0][j][7] for f in collect(["still_open"]))
        radii.append(values[len(values) // 2])

    poses = {name: local_rotations(medoid(samples)) for name, samples in (("open", opened), ("relaxed", relaxed), ("pinch", pinching), ("fist", fisted))}
    pinch_gaps = sorted(gap(s, THUMB_TIP, INDEX_TIP) for s in pinching)
    poses["pinch"] = close_pinch(offsets, poses["pinch"], pinch_gaps[len(pinch_gaps) // 2])

    # How well each averaged pose reproduces its frames, the tips in the wrist frame
    report = {}
    for name, samples in (("open", opened), ("relaxed", relaxed), ("pinch", pinching), ("fist", fisted)):
        fk = forward(offsets, poses[name])
        errors = sorted(max(math.dist(fk[t][0], s[t][:3]) for t in TIPS) for s in samples)
        tip_gap = math.dist(fk[THUMB_TIP][0], fk[INDEX_TIP][0])
        report[name] = (len(samples), errors[len(errors) // 2], errors[len(errors) * 9 // 10], tip_gap)

    pinch_pairs, grasp_pairs = [], []
    for joints, interaction in collect([""]):
        if interaction:
            pinch_pairs.append((gap(joints, THUMB_TIP, INDEX_TIP), interaction["pinch"]))
            grasp_pairs.append((curl(joints), interaction["grasp"]))
    if not pinch_pairs:
        sys.exit("hand_fit.py: no take has XR_EXT_hand_interaction values")

    def rounded(values, digits):
        return [round(v, digits) for v in values]

    model = {
        "format": "puppetxr-hand-model",
        "version": 1,
        "source": "fitted by tools/hand_fit.py to motion recorder takes measured on a Quest 3",
        "joints": [
            {"name": NAMES[j], "parent": -1 if PARENTS[j] is None else PARENTS[j], "offset": rounded(offsets[j], 5), "radius": round(radii[j], 4)}
            for j in range(JOINTS)
        ],
        "poses": {name: [rounded(q, 5) for q in rotations] for name, rotations in poses.items()},
        "coupling": coupling(offsets, poses, [wrist_local(j) for j, _ in collect(COUPLING_TAKES)]),
        "pinch_curve": curve(pinch_pairs, 0.002, 0.06),
        "grasp_curve": curve(grasp_pairs, 0.005, 0.12),
    }
    with open(args.output, "w") as f:
        f.write(json.dumps(model, indent=1) + "\n")

    for name, (count, median, p90, tip_gap) in report.items():
        print(f"{name:8} {count:6d} frames, the others' tips off by median {median * 1000:5.1f} mm, 90% {p90 * 1000:5.1f} mm; thumb to index {tip_gap * 1000:5.1f} mm")
    print(f"pinch curve from {len(pinch_pairs)} hand frames, grasp curve from {len(grasp_pairs)}")
    print("coupling (row curled, column following, index to little):", model["coupling"])


if __name__ == "__main__":
    main()
