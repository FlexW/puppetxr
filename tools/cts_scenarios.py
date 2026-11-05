#!/usr/bin/env python3
#
# Runs CTS scenario tests ([scenario][interactive][no_auto]) unattended: each test gets a script
# that moves the simulated devices and presses buttons frame by frame through puppetxrctl, the way
# a person would. The CTS judges the result itself.
#
#   python3 tools/cts_scenarios.py --cts <conformance_cli dir> --runtime <puppetxr-dev.json>
#       --puppetxrctl <puppetxrctl> --icd <lvp_icd.json> --out <dir> [--tests A,B]
#
# The simulator starts paused and every frame is stepped explicitly, so a script sees exactly
# the frames it asked for, and a scenario plays out the same way on every run. Tests that ask a
# person to judge what they see capture screenshots, compared like cts_composition.py does
# against approved references (--references, --update). Exit code 0 when every test passed.

import argparse
import json
import math
import os
import queue
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cts_composition  # noqa: E402  reference image comparison

FRAME_RATE = 90.0 # the default refresh rate; scripts count frames, velocities follow from it


def quat_axis_angle(axis, angle):
    s = math.sin(angle / 2.0)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(angle / 2.0))


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def pose(device, position, orientation=(0.0, 0.0, 0.0, 1.0)):
    return "pose %s %.6f %.6f %.6f %.6f %.6f %.6f %.6f" % ((device,) + tuple(position) + tuple(orientation))


class Shell:
    """One puppetxrctl shell, the only client of a paused simulator."""

    def __init__(self, puppetxrctl, pid, timeout_ms=None):
        timeout = ["--timeout", str(timeout_ms)] if timeout_ms else []
        self.proc = subprocess.Popen([puppetxrctl, "--target", str(pid)] + timeout + ["shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.alive = True
        self.pid = pid
        # Device commands refused by a live simulator (a misspelt input, a missing device)
        self.rejected = []
        # Once set, a command without a reply in this many seconds marks the app hung: puppetxrctl's
        # own timeout has to cover an app that loads for minutes without a call to puppetxr
        self.reply_seconds = None
        self.hung = None
        self.replies = queue.Queue()
        threading.Thread(target=self.read_replies, daemon=True).start()

    def read_replies(self):
        for reply in self.proc.stdout:
            self.replies.put(reply)
        self.replies.put("")

    def run(self, line):
        if not self.alive:
            return None
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
            reply = self.replies.get(timeout=self.reply_seconds)
        except (BrokenPipeError, OSError):
            reply = ""
        except queue.Empty:
            self.hung = f"no reply to `{line}` within {self.reply_seconds:.0f} s"
            reply = ""
        if not reply:
            self.alive = False
            return None
        result = json.loads(reply)
        if not result.get("ok") and line.split(" ", 1)[0] in ("input", "pose", "point"):
            self.rejected.append(f"{line}: {result.get('message') or result.get('error', 'refused')}")
        return result if result.get("ok") else None

    def frames(self, count):
        return self.run(f"step {count}") is not None

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        if self.hung:
            self.proc.kill()
        self.proc.wait()


#
# Scripts: each takes a Shell connected to a fresh, paused instance, plays the scenario and
# returns what it found wrong beyond what the CTS checks itself
#

def press(shell, device, input_name, frames=3):
    ok = shell.run(f"input {device} {input_name} on") is not None and shell.frames(frames)
    return ok and shell.run(f"input {device} {input_name} off") is not None and shell.frames(frames)


def haptic(shell, device):
    return shell.run(f"haptic {device}") or {}


def script_haptic_interrupt(shell, capture):
    # Spec 11.7 xrApplyHapticFeedback: a new event on the same action replaces the running one
    problems = []
    shell.frames(30)
    press(shell, "right", "menu/click")
    first = haptic(shell, "right")
    if not first.get("active") or abs(first.get("amplitude", 0) - 0.25) > 1e-3 or not 1.8 < first.get("remaining_s", 0) <= 2.0:
        problems.append(f"first press: expected a 2 s haptic at 0.25, got {first}")
    shell.frames(45)
    press(shell, "right", "menu/click")
    second = haptic(shell, "right")
    if not second.get("active") or abs(second.get("amplitude", 0) - 0.75) > 1e-3 or not 1.8 < second.get("remaining_s", 0) <= 2.0:
        problems.append(f"second press: expected the haptic restarted for 2 s at 0.75, got {second}")
    shell.frames(int(2.1 * FRAME_RATE))
    if haptic(shell, "right").get("active"):
        problems.append("the haptic still runs after 2 s")
    press(shell, "right", "trigger/value")
    return problems

def script_space_offsets(shell, capture):
    # Spec 7.4 velocities: the CTS checks every offset space against its own lever-arm math
    # while the grip moves along and turns about each axis fast enough to tick its criteria
    base = (0.2, 1.3, -0.4)
    shell.frames(30)
    for axis in ((1, 0, 0), (0, 1, 0), (0, 0, 1)):
        for frame in range(40):
            t = frame / FRAME_RATE
            offset = 0.15 * math.sin(2.0 * math.pi * 1.5 * t) # peaks at 1.4 m/s
            position = tuple(base[i] + axis[i] * offset for i in range(3))
            if shell.run(pose("right", position)) is None or not shell.frames(1):
                return []
    for axis in ((1, 0, 0), (0, 1, 0), (0, 0, 1)):
        for frame in range(40):
            angle = 1.2 * math.sin(2.0 * math.pi * 1.5 * frame / FRAME_RATE) # peaks at 11 rad/s
            if shell.run(pose("right", base, quat_axis_angle(axis, angle))) is None or not shell.frames(1):
                return []
    shell.frames(30)
    return []


def script_interactive_throw(shell, capture):
    # The CTS launches cubes from the pose and velocity located at the release's lastChangeTime,
    # which takes the pose history (spec 2.16) and lastChangeTime (spec 11.6) to be right.
    # Targets are in LOCAL, whose origin is the default head position in stage space.
    local_origin = (0.0, 1.6, 0.0)
    targets      = ((-1.0, -1.0, -3.0), (1.0, -1.0, -4.0), (0.0, 1.0, -5.0))
    release      = (0.2, 1.3, -0.3)
    flight       = 0.5
    gravity      = -9.8
    shell.frames(30)
    for target in targets:
        goal     = tuple(target[i] + local_origin[i] for i in range(3))
        velocity = tuple((goal[i] - release[i] - (0.5 * gravity * flight * flight if i == 1 else 0.0)) / flight for i in range(3))
        # A straight line through the release point at the launch velocity, select released on it
        for frame in range(-10, 6):
            position = tuple(release[i] + velocity[i] * frame / FRAME_RATE for i in range(3))
            if shell.run(pose("right", position)) is None:
                return []
            if frame == -10 and shell.run("input right trigger/value 1") is None:
                return []
            if frame == 0 and shell.run("input right trigger/value 0") is None:
                return []
            if not shell.frames(1):
                return []
        shell.run(pose("right", release))
        if not shell.frames(int(0.8 * FRAME_RATE)):
            return []
    return []


# Held thumbs-up and pointing into the screen, the handle (grip -Z) rises 60 degrees, which
# levels the aim pose (aim_offset in sim.cpp)
THUMBS_UP = quat_axis_angle((1.0, 0.0, 0.0), 60.0 * math.pi / 180.0)


def script_hand_poses(shell, capture):
    # A person checks that what is drawn at the grip, aim or grip surface pose sits in the hand like
    # the spec diagrams; here the hands hold the thumbs-up pose the CTS describes, turned a little
    # so all three axes show, and the result is compared with a reviewed reference
    shell.run(pose("left", (-0.15, 1.35, -0.45), quat_mul(quat_axis_angle((0.0, 1.0, 0.0), 0.4), THUMBS_UP)))
    shell.run(pose("right", (0.15, 1.35, -0.45), quat_mul(quat_axis_angle((0.0, 1.0, 0.0), -0.4), THUMBS_UP)))
    shell.frames(60)
    capture("hands")
    press(shell, "right", "menu/click")
    shell.frames(30)
    capture("swapped")
    press(shell, "right", "trigger/value")
    return []


def script_local_floor(shell, capture):
    # The floor drawn at LOCAL_FLOOR must lie on the simulated floor, stage y = 0, below the cube drawn
    # at LOCAL; step back from the LOCAL origin and look down so both show
    shell.run(pose("head", (0.0, 1.6, 1.2), quat_axis_angle((1.0, 0.0, 0.0), -0.55)))
    shell.frames(60)
    capture("floor")
    press(shell, "right", "trigger/value")
    return []


# Index tips of both tracked hands in the grip frame of an unrotated hand (sim_hand_joints): the
# fingers curl down and forward from the grip
INDEX_TIP = { "left": (-0.025, -0.107836, -0.031667), "right": (0.025, -0.107836, -0.031667) }


def script_index_tips(shell, capture):
    # The person moves both tracked hands, then brings the index finger tips within 1 cm of each
    # other; the CTS ends the test when it locates them that close
    shell.run("hand left hand")
    shell.run("hand right hand")
    meet  = (0.0, 1.3, -0.4)
    apart = 0.15
    for frame in range(60):
        swing = 0.05 * math.sin(2.0 * math.pi * frame / 60.0)
        for side, sign in (("left", -1.0), ("right", 1.0)):
            tip = (meet[0] + sign * apart, meet[1] + swing, meet[2])
            shell.run(pose(side, tuple(tip[i] - INDEX_TIP[side][i] for i in range(3))))
        if not shell.frames(1):
            return []
    for frame in range(1, 31):
        gap = apart * (1.0 - frame / 30.0)
        for side, sign in (("left", -1.0), ("right", 1.0)):
            tip = (meet[0] + sign * gap, meet[1], meet[2])
            shell.run(pose(side, tuple(tip[i] - INDEX_TIP[side][i] for i in range(3))))
        if not shell.frames(1):
            return []
    shell.frames(10)
    return []


SCRIPTS = {
    "SpaceOffsets":    script_space_offsets,
    "HapticInterrupt": script_haptic_interrupt,
    "InteractiveThrow": script_interactive_throw,
    "GripAndAimPose":   script_hand_poses,
    "GripSurface":      script_hand_poses,
    "local_floor-local": script_local_floor,
    "local_floor-stage": script_local_floor,
    "XR_EXT_local_floor-local": script_local_floor,
    "XR_EXT_local_floor-stage": script_local_floor,
    "XR_FB_hand_tracking_mesh-interactive": script_index_tips,
    "XR_EXT_hand_tracking-interactive": script_index_tips,
}


def discovery_file(pid):
    # os_runtime_dir: GetTempPath on Windows, which reads TMP before TEMP
    if sys.platform == "win32":
        runtime = os.environ.get("TMP") or os.environ.get("TEMP") or os.environ.get("USERPROFILE", "")
        return os.path.join(runtime, "puppetxr", f"{pid}.json")
    runtime = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    if not os.path.isdir(runtime):
        runtime = "/tmp"
    return os.path.join(runtime, "puppetxr", f"{pid}.json")


def check_shot(shot, opts):
    reference = os.path.join(opts.references, os.path.basename(shot))
    if opts.update:
        open(reference, "wb").write(open(shot, "rb").read())
        return []
    if not os.path.exists(reference):
        return [f"{os.path.basename(shot)} has no reference"]
    bad, verdict = cts_composition.compare(reference, shot, opts.tolerance, shot.replace(".png", "_diff.png"))
    return [] if bad == 0 else [f"{os.path.basename(shot)}: {verdict}"]


def run_test(name, opts, env):
    log_path = os.path.join(opts.out, f"{name}.log")
    log      = open(log_path, "w")
    proc     = subprocess.Popen([os.path.join(opts.cts, "conformance_cli"), name, "-G", "Vulkan2", "-r", "junit", "-o", os.path.join(opts.out, f"{name}.xml")], cwd=opts.cts, env=env, stdout=log, stderr=subprocess.STDOUT)
    deadline = time.time() + opts.timeout
    played   = False
    problems = []
    while proc.poll() is None and time.time() < deadline:
        if not os.path.exists(discovery_file(proc.pid)):
            time.sleep(0.05)
            continue
        shell = Shell(opts.puppetxrctl, proc.pid)
        shell.run("controls off")
        def capture(label):
            shot = os.path.join(opts.out, f"{name}_{label}.png")
            if shell.run("mirror both") is None or not shell.frames(1) or shell.run(f"screenshot {shot}") is None:
                problems.append(f"no screenshot for {label}")
                return
            problems.extend(check_shot(shot, opts))

        if not played:
            problems += SCRIPTS[name](shell, capture)
            played = True
        # A scenario that did not end by itself has failed or got stuck; Menu ends every one of them
        for _ in range(4):
            if shell.alive:
                shell.frames(30)
        if shell.alive:
            problems.append("the scenario did not end after its script")
            press(shell, "right", "menu/click")
        while shell.alive and proc.poll() is None and time.time() < deadline:
            shell.frames(30)
        shell.close()
    if proc.poll() is None:
        proc.kill()
        proc.wait()
        return False, "timeout"
    log.close()
    text = open(log_path).read()
    if "Test Success Count: 0" in text and "Test Failure Count: 0" in text:
        return not problems, "skipped"
    if proc.returncode != 0:
        problems.append(f"the CTS failed (exit {proc.returncode}, see {log_path})")
    return not problems, "passed" if not problems else "; ".join(problems)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cts", required=True)
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--puppetxrctl", required=True)
    parser.add_argument("--icd", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--tests", default=",".join(SCRIPTS))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--references", required=True)
    parser.add_argument("--tolerance", type=int, default=0)
    parser.add_argument("--update", action="store_true")
    opts = parser.parse_args()
    opts.out = os.path.abspath(opts.out)
    os.makedirs(opts.out, exist_ok=True)
    os.makedirs(opts.references, exist_ok=True)

    config = os.path.join(opts.out, "puppetxr.json")
    open(config, "w").write('{ "views": { "width": 640, "height": 640 } }\n')
    env = dict(os.environ, PUPPETXR_CONFIG=config, PUPPETXR_CONTROL="1", PUPPETXR_CONTROL_PAUSED="1", PUPPETXR_HEADLESS="1", PUPPETXR_ROOM="none", PUPPETXR_FIXED_STEP="1",
               VK_DRIVER_FILES=os.path.abspath(opts.icd), XR_API_LAYER_PATH=opts.cts, XR_RUNTIME_JSON=os.path.abspath(opts.runtime))

    failures = 0
    for name in opts.tests.split(","):
        start  = time.time()
        ok, verdict = run_test(name, opts, env)
        failures += 0 if ok else 1
        print(f"{name}: {verdict} ({time.time() - start:.0f} s)", flush=True)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
