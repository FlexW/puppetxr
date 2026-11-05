#!/usr/bin/env python3
#
# Runs the interactive CTS composition tests unattended on puppetxr and compares what puppetxr
# composited against approved reference images.
#
#   python3 tools/cts_composition.py --cts <conformance_cli dir> --runtime <puppetxr-dev.json>
#       --puppetxrctl <puppetxrctl> --icd <lvp_icd.json> --references tests/cts/composition
#       --out <dir> [--update] [--tests A,B] [--graphics OpenGL]
#
# Each test runs in its own conformance_cli process on lavapipe with a fixed-step clock, or with
# --graphics OpenGL on llvmpipe (LIBGL_ALWAYS_SOFTWARE), which needs an X display. For
# every scene the test shows, puppetxrctl waits for it to settle, saves both eyes and presses
# Select, which the CTS takes as "pass"; the verdict comes from the image comparison instead.
# --update writes the screenshots as the new references, for review before committing.
# Exit code 0 when every screenshot matches its reference.

import argparse
import base64
import html
import os
import struct
import subprocess
import sys
import time
import zlib

CORE_TESTS = [
    "GradientFormatsLinearVsNonLinear", "QuadOcclusion", "QuadProjectionQuad", "ProjectionQuadProjection",
    "QuadPoses", "MultipleMutableProjections", "SourceAlphaBlending", "SourceAlphaBlendingWithEnvironment",
    "EyeVisibility", "Subimage", "ProjectionArraySwapchain", "ProjectionWideSwapchain",
    "ProjectionSeparateSwapchains", "MinLayers", "QuadHands", "ProjectionMutableFieldOfView", "StaleSwapchain",
]

# Composition tests of extensions puppetxr implements
EXTENSION_TESTS = [
    "XR_KHR_visibility_mask-interactive", "XR_EXT_composition_layer_inverted_alpha", "XR_FB_composition_layer_image_layout",
    "XR_KHR_composition_layer_equirect-interactive", "XR_KHR_composition_layer_equirect2-interactive", "ProjectionDepth",
]

# Commands run before the first scene of a test, to frame what it shows
SETUP = {
    "QuadHands": ["pose left -0.12 1.5 -0.45", "pose right 0.12 1.5 -0.45"],
}

# Environment of a test's process: the visibility mask test needs the lens mask, off by default;
# without it puppetxr's mask cuts nothing (the whole view visible)
ENV = {
    "XR_KHR_visibility_mask-interactive": {"PUPPETXR_LENS_MASK": "1"},
}

SETTLE_FRAMES = 60
MAX_STEPS     = 64


def png_stream(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, width, height = 8, b"", 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", chunk[:10])
            assert depth == 8 and color == 6, f"{path}: only 8-bit RGBA"
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + length
    return width, height, zlib.decompress(idat)


def read_png(path):
    width, height, raw = png_stream(path)
    stride, out = width * 4, bytearray()
    prev = bytearray(stride)
    for y in range(height):
        kind, row = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if kind == 1:
            for i in range(4, stride):
                row[i] = (row[i] + row[i - 4]) & 255
        elif kind == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 255
        elif kind == 3:
            for i in range(stride):
                row[i] = (row[i] + ((row[i - 4] if i >= 4 else 0) + prev[i]) // 2) & 255
        elif kind == 4:
            for i in range(stride):
                a = row[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += row
        prev = row
    return width, height, bytes(out)


def write_png(path, width, height, rgba):
    raw = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


# Pixels whose largest channel difference exceeds `tolerance`; the diff image shows them red over a
# dimmed copy of the reference
def compare(reference, current, tolerance, diff_path, ignore=()):
    # `ignore`: rectangles (u0, v0, u1, v1) as fractions of the image that are skipped, blue in the diff
    # Equal filtered streams are equal pixels; unfiltering in Python takes seconds per image
    if png_stream(reference) == png_stream(current):
        return 0, "identical"
    rw, rh, rp = read_png(reference)
    cw, ch, cp = read_png(current)
    if (rw, rh) != (cw, ch):
        return None, f"size {cw}x{ch}, reference {rw}x{rh}"
    if rp == cp:
        return 0, "identical"
    diff, bad, worst = bytearray(len(rp)), 0, 0
    skipped = set()
    for u0, v0, u1, v1 in ignore:
        for y in range(int(v0 * rh), min(rh, int(v1 * rh) + 1)):
            skipped.update(range((y * rw + int(u0 * rw)) * 4, (y * rw + min(rw - 1, int(u1 * rw)) + 1) * 4, 4))
    for i in range(0, len(rp), 4):
        if i in skipped:
            diff[i:i + 4] = b"\x00\x00\xff\xff"
            continue
        delta = max(abs(rp[i] - cp[i]), abs(rp[i + 1] - cp[i + 1]), abs(rp[i + 2] - cp[i + 2]))
        worst = max(worst, delta)
        if delta > tolerance:
            bad += 1
            diff[i:i + 4] = b"\xff\x00\x00\xff"
        else:
            diff[i:i + 4] = bytes((rp[i] // 3, rp[i + 1] // 3, rp[i + 2] // 3, 255))
    write_png(diff_path, rw, rh, bytes(diff))
    return bad, f"{bad} pixels differ by more than {tolerance}, largest difference {worst}"


def discovery_file(pid):
    # os_runtime_dir: GetTempPath on Windows, which reads TMP before TEMP
    if sys.platform == "win32":
        runtime = os.environ.get("TMP") or os.environ.get("TEMP") or os.environ.get("USERPROFILE", "")
        return os.path.join(runtime, "puppetxr", f"{pid}.json")
    runtime = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    if not os.path.isdir(runtime):
        runtime = "/tmp"
    return os.path.join(runtime, "puppetxr", f"{pid}.json")


class Shell:
    """One puppetxrctl shell, the only client of a paused simulator. Closing it would resume it."""

    def __init__(self, puppetxrctl, pid):
        self.proc = subprocess.Popen([puppetxrctl, "--target", str(pid), "shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    def run(self, line):
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            return False
        reply = self.proc.stdout.readline()
        return '"ok":true' in reply

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.wait()


# The simulator starts paused, so every scene is captured after exactly SETTLE_FRAMES frames
# of its instance, whatever the wall clock did
def run_test(name, opts, env):
    shots, out = [], opts.out
    log = open(os.path.join(out, f"{name}.log"), "w")
    proc = subprocess.Popen([os.path.join(opts.cts, "conformance_cli"), name, "-G", opts.graphics, "-r", "junit", "-o", os.path.join(out, f"{name}.xml")], cwd=opts.cts, env=dict(env, **ENV.get(name, {})), stdout=log, stderr=subprocess.STDOUT)
    deadline = time.time() + opts.timeout
    while proc.poll() is None and time.time() < deadline and len(shots) < MAX_STEPS:
        if not os.path.exists(discovery_file(proc.pid)):
            time.sleep(0.05)
            continue
        shell = Shell(opts.puppetxrctl, proc.pid)
        for line in ["mirror both"] + SETUP.get(name, []):
            shell.run(line)
        while len(shots) < MAX_STEPS and shell.run(f"step {SETTLE_FRAMES}"):
            shot = os.path.join(out, f"{name}_{len(shots)}.png")
            if not shell.run(f"screenshot {shot}"):
                break
            shots.append(shot)
            # Select passes the scene; the next scene follows in this instance or in a new one
            if not (shell.run("input right trigger/value 1") and shell.run("step 5") and shell.run("input right trigger/value 0") and shell.run("step 5")):
                break
        shell.close()
    if proc.poll() is None:
        proc.kill()
        proc.wait()
        return shots, "timeout"
    log.close()
    text = open(os.path.join(out, f"{name}.log")).read()
    if "Test Success Count: 0" in text and "Test Failure Count: 0" in text:
        return shots, "skipped"
    return shots, "cts passed" if proc.returncode == 0 else f"cts exit {proc.returncode}"


def image_tag(path):
    if not path or not os.path.exists(path):
        return "<em>none</em>"
    return f'<img src="data:image/png;base64,{base64.b64encode(open(path, "rb").read()).decode()}">'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cts", required=True)
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--puppetxrctl", required=True)
    parser.add_argument("--icd", required=True)
    parser.add_argument("--references", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--tests", default=",".join(CORE_TESTS + EXTENSION_TESTS))
    parser.add_argument("--tolerance", type=int, default=0)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--graphics", default="Vulkan2", help="graphics plugin of the CTS: Vulkan2 or OpenGL")
    parser.add_argument("--skip", default="", help="tests left out, comma separated")
    opts = parser.parse_args()
    if opts.graphics == "OpenGL" and not os.environ.get("DISPLAY"):
        print("skipped: the CTS OpenGL plugin needs an X display")
        return 77
    opts.out = os.path.abspath(opts.out)
    os.makedirs(opts.out, exist_ok=True)
    os.makedirs(opts.references, exist_ok=True)

    config = os.path.join(opts.out, "puppetxr.json")
    open(config, "w").write('{ "views": { "width": 640, "height": 640 } }\n')
    env = dict(os.environ, PUPPETXR_CONFIG=config, PUPPETXR_CONTROL="1", PUPPETXR_CONTROL_PAUSED="1", PUPPETXR_HEADLESS="1", PUPPETXR_ROOM="none", PUPPETXR_FIXED_STEP="1",
               VK_DRIVER_FILES=os.path.abspath(opts.icd), XR_API_LAYER_PATH=opts.cts, XR_RUNTIME_JSON=os.path.abspath(opts.runtime))
    if opts.graphics == "OpenGL":
        env["LIBGL_ALWAYS_SOFTWARE"] = "1"

    rows, failures = [], 0
    for name in [test for test in opts.tests.split(",") if test not in opts.skip.split(",")]:
        start = time.time()
        shots, cts = run_test(name, opts, env)
        print(f"{name}: {len(shots)} scenes, {cts} ({time.time() - start:.0f} s)", flush=True)
        if cts == "timeout" or (cts != "skipped" and not shots):
            failures += 1
        for shot in shots:
            reference = os.path.join(opts.references, os.path.basename(shot))
            diff = shot.replace(".png", "_diff.png")
            if opts.update:
                open(reference, "wb").write(open(shot, "rb").read())
                verdict, ok = "reference updated", True
            elif not os.path.exists(reference):
                verdict, ok = "no reference", False
            else:
                bad, verdict = compare(reference, shot, opts.tolerance, diff)
                ok = bad == 0
            failures += 0 if ok else 1
            print(f"  {os.path.basename(shot)}: {verdict}", flush=True)
            rows.append((name, cts, os.path.basename(shot), verdict, ok, reference, shot, diff if os.path.exists(diff) else None))
        if not shots:
            rows.append((name, cts, "", "no scene captured" if cts != "skipped" else "needs an extension puppetxr lacks", cts == "skipped", None, None, None))

    report = os.path.join(opts.out, "report.html")
    with open(report, "w") as f:
        f.write("<!doctype html><meta charset=utf-8><title>CTS composition</title><style>body{font:14px sans-serif;background:#111;color:#ddd}"
                "img{width:420px;border:1px solid #444}td{vertical-align:top;padding:6px}.bad{color:#f66}.ok{color:#6c6}</style>")
        f.write(f"<h1>CTS composition tests on {'llvmpipe GL' if opts.graphics == 'OpenGL' else 'lavapipe'}</h1><p>{len(rows)} scenes, {failures} failing, tolerance {opts.tolerance}</p><table>")
        f.write("<tr><th>scene</th><th>reference</th><th>puppetxr</th><th>difference</th></tr>")
        for name, cts, shot, verdict, ok, reference, current, diff in rows:
            css = "ok" if ok else "bad"
            f.write(f"<tr><td><b>{html.escape(name)}</b><br>{html.escape(shot)}<br>{html.escape(cts)}<br><span class={css}>{html.escape(verdict)}</span></td>"
                    f"<td>{image_tag(reference)}</td><td>{image_tag(current)}</td><td>{image_tag(diff)}</td></tr>")
        f.write("</table>")
    print(f"report: {report}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
