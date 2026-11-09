#!/usr/bin/env python3
#
# Runs third-party OpenXR samples on puppetxr and compares what they show against approved
# references. The samples are fetched, patched and built by cmake/samples.cmake.
#
#   python3 tools/sample_tests.py --samples <build dir/samples> --runtime <puppetxr-dev.json>
#       --puppetxrctl <puppetxrctl> --references tests/samples --out <dir> [--update] [--tests A,B]
#
# Each sample project builds into <samples>/<app>; the references of a test are
# <references>/<app>/references/<test>/<screenshot>.png, left eye only.
#
# Every sample runs headless, paused and at a fixed step, so a script sees exactly the frames
# it asked for. GLES samples draw on llvmpipe through EGL's surfaceless platform with one
# rasterizer thread (LP_NUM_THREADS=0, see docs/testing.md). Godot demos run the official Godot
# binary in an Xvfb display the driver starts, on Windows on Mesa's llvmpipe and lavapipe of
# mesa-dist-win, at a fixed frame rate (--fixed-fps) so that their
# scripts and animations do not follow the wall clock, at 640x672 per eye. A test passes when its script
# found nothing wrong, the sample exited by itself with code 0 after `exit`, and every
# screenshot matches its reference. --update writes the screenshots as the new references,
# for review before committing. Exit code 0 when every test passed.

import argparse
import concurrent.futures
import contextlib
import ctypes
import json
import math
import os
import re
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cts_composition  # noqa: E402  image comparison, discovery files
import cts_scenarios    # noqa: E402  puppetxrctl shell with JSON replies

WINDOWS = sys.platform == "win32"
MACOS = sys.platform == "darwin"
SETTLE_FRAMES = 60
# Godot's and Unity's tests run with the lens mask, as apps see it on a headset; the others' references
# are from before it
GODOT_VIEWS = {"width": 640, "height": 672, "lens_mask": True}
GODOT_RENDERERS = {
    "gl": ["--rendering-method", "gl_compatibility", "--rendering-driver", "opengl3"],
    "vk": ["--rendering-method", "mobile", "--rendering-driver", "vulkan"],
}


def run_frames(shell, count, chunk=50):
    # puppetxrctl gives up on a step after a minute; software rendering may need longer for many frames
    while count > 0:
        if not shell.frames(min(count, chunk)):
            return False
        count -= chunk
    return True


def press(shell, device, input_name, frames=5):
    ok = shell.run(f"input {device} {input_name} on") is not None and shell.frames(frames)
    return ok and shell.run(f"input {device} {input_name} off") is not None and shell.frames(frames)


#
# Scripts: each takes a Shell connected to the paused sample and a function that saves a
# screenshot under a name, plays its scenario and returns what it found wrong
#

def script_look_around(shell, shot, out):
    # Exploration: the four directions at standing height and the floor ahead
    for yaw in (0, 90, 180, 270):
        shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((0.0, 1.0, 0.0), math.radians(yaw))))
        run_frames(shell, 100, chunk=50)
        shot(f"yaw{yaw}")
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(-50.0))))
    run_frames(shell, 100, chunk=50)
    shot("down")
    return []


def script_settled_look_around(shell, shot, out):
    # Unreal's Interaction SDK levels light their fog over the first seconds, as far as the load
    # took in wall-clock time; settled, every run shows the same
    run_frames(shell, 600, chunk=100)
    return script_look_around(shell, shot, out)


def script_still(shell, shot):
    shell.frames(SETTLE_FRAMES)
    shot("start")
    return []


def script_xrinput(shell, shot):
    # The action panels turn currentState true while the bound input is held
    shell.frames(SETTLE_FRAMES)
    shot("start")
    shell.run("input right trigger/value 1")
    shell.frames(10)
    shot("trigger")
    shell.run("input right trigger/value 0")
    shell.frames(10)
    return []


def script_spatial_anchor(shell, shot):
    # A places an anchor at the right controller, the sample draws an orange cuboid on it
    shell.run("anchors wipe cloud")
    shell.frames(SETTLE_FRAMES)
    shell.run(cts_scenarios.pose("right", (0.15, 1.45, -0.5)))
    shell.frames(5)
    press(shell, "right", "a/click")
    shell.frames(30)
    shot("placed")
    return []


def script_passthrough_occlusion(shell, shot):
    # The cubes in front of the TV stand are cut where the room is closer than they are
    shell.frames(SETTLE_FRAMES)
    shot("start")
    shell.run(cts_scenarios.pose("head", (0.6, 1.6, 0.0), cts_scenarios.quat_axis_angle((0, 1, 0), 0.4)))
    shell.frames(10)
    shot("side")
    return []


def script_scene_model(shell, shot):
    # Left thumbstick click cycles all, planes and volumes, room mesh, global mesh
    shell.frames(SETTLE_FRAMES)
    shot("start")
    press(shell, "left", "thumbstick/click")
    press(shell, "left", "thumbstick/click")
    shell.frames(10)
    shot("room_mesh")
    return []


PASSTHROUGH_FRAMES_PER_MODE = 40


def script_passthrough_modes(shell, shot):
    # The sample cycles its modes every PASSTHROUGH_FRAMES_PER_MODE frames (400 upstream): basic,
    # dynamic ramp, green ramp with yellow edges, masked, a projected quad on the left grip,
    # stopped; each is shot in the middle
    shell.run(cts_scenarios.pose("left", (-0.1, 1.5, -0.45)))
    names = ["basic", "ramp", "edges", "masked", "projected", "stopped"]
    for mode, name in enumerate(names):
        run_frames(shell, PASSTHROUGH_FRAMES_PER_MODE // 2 if mode == 0 else PASSTHROUGH_FRAMES_PER_MODE)
        shot(name)
    return []


def script_controllers(shell, shot):
    # The right ray clicks a vibration, a PCM and a parametric button; the simulator must report
    # each effect on the right main output. The grip is pitched up so that the aim pose is level;
    # the sample's local y 0 is stage y 1.57 and its buttons are 0.2 apart.
    shell.frames(SETTLE_FRAMES)
    shot("start")
    problems = []
    level = cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(60.0))
    cases = (
        ("vibration",  (1.6, 2.07, -1.0), lambda h: abs(h["frequency"] - 157.0) < 0.01 and 0.9 < h["remaining_s"] <= 1.0),
        ("pcm",        (1.2, 1.87, -1.0), lambda h: 0.9 < h["remaining_s"] <= 1.0),
        ("parametric", (2.0, 2.27, -1.0), lambda h: abs(h["frequency"] - 157.0) >= 0.01),
    )
    for name, target, expected in cases:
        shell.run(cts_scenarios.pose("right", target, level))
        shell.frames(5)
        shell.run("input right trigger/value 1")
        shell.frames(2)
        shot(name)
        state = shell.run("haptic right")
        if not state or not state.get("active") or not expected(state):
            problems.append(f"{name}: wrong haptic on the right controller ({state})")
        shell.run("input right trigger/value 0")
        shell.run("pose right 0 1.2 -0.3 0 0 0 1")
        run_frames(shell, 200)
    return problems


def script_hands(shell, shot):
    # Both hands switch to hand tracking; the right hand pinches (trigger), then makes a fist
    # (trigger and squeeze), the left stays open
    hands_forward(shell)
    # XrHandDataSource renders about a frame a second on one llvmpipe thread, so steps stay small
    run_frames(shell, SETTLE_FRAMES, chunk=10)
    shot("open")
    shell.run("input right trigger/value 1")
    run_frames(shell, 10, chunk=10)
    shot("pinch")
    shell.run("input right squeeze/value 1")
    run_frames(shell, 10, chunk=10)
    shot("fist")
    shell.run("input right trigger/value 0")
    shell.run("input right squeeze/value 0")
    run_frames(shell, 10, chunk=10)
    return []


def hands_forward(shell):
    # Spec grip: the fingers of a hand wrap the controller handle along grip -Z, so pitching the
    # grip up 80 degrees points an open hand's fingers forward
    shell.run("hand left hand")
    shell.run("hand right hand")
    forward = cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(80.0))
    shell.run(cts_scenarios.pose("left", (-0.12, 1.45, -0.3), forward))
    shell.run(cts_scenarios.pose("right", (0.12, 1.45, -0.3), forward))
    return forward


def script_render_models(shell, shot):
    # Controllers in hand: the right ray clicks "Render Controllers" (the sample's local origin is
    # the head; the button is 0.5 m left, 0.15 m down, 2 m ahead) and the sample draws the models
    # it loads through XR_FB_render_model; a grip pitched 60 degrees aims level
    run_frames(shell, SETTLE_FRAMES, chunk=10)
    target = (-0.5, 1.45, -2.0)
    hand = (0.0, 1.5, -0.3)
    d = (target[0] - hand[0], target[1] - hand[1], target[2] - hand[2])
    yaw = math.atan2(-d[0], -d[2])
    pitch = math.atan2(d[1], math.hypot(d[0], d[2]))
    aim = cts_scenarios.quat_mul(cts_scenarios.quat_axis_angle((0.0, 1.0, 0.0), yaw), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(60.0) + pitch))
    shell.run(cts_scenarios.pose("right", hand, aim))
    run_frames(shell, 5, chunk=5)
    shell.run("input right trigger/value 1")
    run_frames(shell, 3, chunk=3)
    shell.run("input right trigger/value 0")
    forward = cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(60.0))
    shell.run(cts_scenarios.pose("left", (-0.12, 1.4, -0.3), forward))
    shell.run(cts_scenarios.pose("right", (0.12, 1.4, -0.3), forward))
    run_frames(shell, 10, chunk=10)
    shot("controllers")
    return []


def quat_rotate(q, v):
    x, y, z, _ = cts_scenarios.quat_mul(cts_scenarios.quat_mul(q, (v[0], v[1], v[2], 0.0)), (-q[0], -q[1], -q[2], q[3]))
    return (x, y, z)


def offset(position, q, local):
    d = quat_rotate(q, local)
    return (position[0] + d[0], position[1] + d[1], position[2] + d[2])


def ray_press(shell, key, frames=3, release=True):
    # Aims the right controller's ray straight into a key of the virtual keyboard from 55 cm, behind
    # the head that looks at it from 45 cm, and pulls the trigger; the aim pose is the grip turned
    # down 60 degrees and 5 cm ahead (aim_offset)
    reply = shell.run(f"vkb key {key}")
    if not reply:
        return False
    q = tuple(reply["orientation"])
    aim = offset(reply["position"], q, (0.0, 0.0, 0.55))
    grip = cts_scenarios.quat_mul(q, cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(60.0)))
    shell.run(cts_scenarios.pose("right", offset(aim, grip, (0.0, 0.0, 0.05)), grip))
    shell.frames(2)
    shell.run("input right trigger/value 1")
    shell.frames(frames)
    if release:
        shell.run("input right trigger/value 0")
        shell.frames(frames)
    return True


def script_virtual_keyboard(shell, shot):
    # The sample shows the keyboard within reach (DIRECT) once its model loaded. The head looks
    # straight at the keys from 45 cm; the ray types shift, h, i (held down to show the press
    # animation) and space; the simulator types the rest and the head looks back at the text box.
    shell.frames(SETTLE_FRAMES)
    state = shell.run("vkb")
    if not state or not state.get("visible"):
        return [f"the keyboard is not shown ({state})"]
    q = tuple(state["orientation"])
    shell.run(cts_scenarios.pose("head", offset(state["position"], q, (0.0, 0.0, 0.45)), q))
    shell.run("active left off")
    shell.frames(5)
    shot("keyboard")
    ray_press(shell, "shift")
    shot("shifted")
    ray_press(shell, "h")
    ray_press(shell, "i", release=False)
    shot("pressed")
    shell.run("input right trigger/value 0")
    shell.frames(3)
    ray_press(shell, "space")
    problems = []
    for text in ("vkb type there\\b\\b\\b\\b\\bpuppetxr\\n", "vkb type 1.0"):
        if shell.run(text) is None:
            problems.append(f"{text} refused")
        shell.frames(2)
    shell.run("reset")
    shell.frames(10)
    shot("typed")
    return problems


def script_dynamic_objects(shell, shot):
    # A keyboard prop below eye level in front of the head, tilted 50 degrees towards it (config
    # PUPPETXR_PROPS); the sample tracks it and cuts a passthrough window around it; then the
    # keyboard moves to the right and the window follows
    shell.run("active left off")
    shell.run("active right off")
    run_frames(shell, SETTLE_FRAMES)
    shot("tracked")
    shell.run("prop move 1 0.25 1.3 -0.6 0.4226183 0 0 0.9063078")
    run_frames(shell, 30)
    shot("moved")
    return []


def script_microgestures(shell, shot):
    # Each microgesture of the right hand is a one frame pulse; the sample shows the last ones
    hands_forward(shell)
    run_frames(shell, SETTLE_FRAMES, chunk=10)
    shot("start")
    for name in ("swipe_left", "swipe_right", "swipe_forward", "swipe_backward", "tap_thumb"):
        shell.run(f"input right {name}_meta/click 1")
        shell.frames(3)
    shell.frames(5)
    shot("gestures")
    return []


def script_wide_motion(shell, shot):
    # The test narrows hand tracking to a 40 degree cone: the left hand ahead stays in it, the
    # right hand 30 degrees aside leaves it, so only the wide motion hands keep showing it
    forward = hands_forward(shell)
    shell.run(cts_scenarios.pose("left", (-0.05, 1.5, -0.4), forward))
    run_frames(shell, SETTLE_FRAMES, chunk=10)
    shot("in_view")
    aside = (0.4 * math.sin(math.radians(30.0)), 1.5, -0.4 * math.cos(math.radians(30.0)))
    shell.run(cts_scenarios.pose("right", aside, forward))
    run_frames(shell, 10, chunk=10)
    shot("right_aside")
    # Both sets coincide, so the green wide motion hands hide the red ones: the left hand pinches
    # "Stop Rendering Wide Motion Mode Hands" (13 degrees left of the view centre at eye height;
    # a grip pitched 60 degrees aims level) and only the red hands the cameras see remain
    aim = cts_scenarios.quat_mul(cts_scenarios.quat_axis_angle((0.0, 1.0, 0.0), math.radians(13.4)), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(60.0)))
    shell.run(cts_scenarios.pose("left", (-0.05, 1.6, -0.2), aim))
    run_frames(shell, 5, chunk=5)
    shell.run("input left trigger/value 1")
    run_frames(shell, 3, chunk=3)
    shell.run("input left trigger/value 0")
    shell.run(cts_scenarios.pose("left", (-0.05, 1.5, -0.4), forward))
    run_frames(shell, 5, chunk=5)
    shot("base_only")
    return []


# app is the sample project, exe relative to its build directory; room is what PUPPETXR_ROOM gets
TESTS = {
    "meta_xrinput":                {"app": "meta_openxr_sdk", "exe": "XrSamples/XrInput/xrsamples_xrinput",                          "room": "none",        "script": script_xrinput},
    "meta_xrcontrollers":          {"app": "meta_openxr_sdk", "exe": "XrSamples/XrControllers/xrcontrollers",                        "room": "none",        "script": script_controllers},
    "meta_xrhandsfb":              {"app": "meta_openxr_sdk", "exe": "XrSamples/XrHandsFB/xrhandsfb",                                "room": "none",        "script": script_hands},
    "meta_xrhanddatasource":       {"app": "meta_openxr_sdk", "exe": "XrSamples/XrHandDataSource/xrhanddatasource",                  "room": "none",        "script": script_hands},
    "meta_xrhanddatasource_models": {"app": "meta_openxr_sdk", "exe": "XrSamples/XrHandDataSource/xrhanddatasource",                 "room": "none",        "script": script_render_models},
    "meta_xrhandsandcontrollers":  {"app": "meta_openxr_sdk", "exe": "XrSamples/XrHandsAndControllers/xrsamples_xrhands_and_controllers", "room": "none", "script": script_hands},
    "meta_xrmicrogestures":        {"app": "meta_openxr_sdk", "exe": "XrSamples/XrMicrogestures/xrmicrogestures",                    "room": "none",        "script": script_microgestures},
    "meta_xrhandtrackingwidemotionmode": {"app": "meta_openxr_sdk", "exe": "XrSamples/XrHandTrackingWideMotionMode/xrhandtrackingwidemotionmode", "room": "none", "script": script_wide_motion, "env": {"PUPPETXR_HAND_TRACKING_FOV": "40"}},
    "meta_xrcolorspacefb":         {"app": "meta_openxr_sdk", "exe": "XrSamples/XrColorSpaceFB/xrsamples_xrcolorspacefb",            "room": "none",        "script": script_still},
    "meta_xrdynamicobjects":       {"app": "meta_openxr_sdk", "exe": "XrSamples/XrDynamicObjects/xrdynamicobjects",                  "room": "living_room", "script": script_dynamic_objects,
                                    "env": {"PUPPETXR_PROPS": '[{"kind": "keyboard", "position": [0, 1.3, -0.6], "orientation": [0.4226183, 0, 0, 0.9063078]}]'}},
    "meta_xrcolocationdiscovery":  {"app": "meta_openxr_sdk", "exe": "XrSamples/XrColocationDiscovery/xrcolocationdiscovery",        "room": "none",        "script": script_still},
    "meta_xroverlaykeyboard":      {"app": "meta_openxr_sdk", "exe": "XrSamples/XrOverlayKeyboard/xroverlaykeyboard",                "room": "none",        "script": script_still},
    "meta_xrvirtualkeyboard":      {"app": "meta_openxr_sdk", "exe": "XrSamples/XrVirtualKeyboard/xrvirtualkeyboard",                "room": "none",        "script": script_virtual_keyboard},
    "meta_xrpassthrough":          {"app": "meta_openxr_sdk", "exe": "XrSamples/XrPassthrough/xrpassthrough",                      "room": "living_room", "script": script_passthrough_modes, "env": {"XRPASSTHROUGH_FRAMES_PER_MODE": str(PASSTHROUGH_FRAMES_PER_MODE)}},
    "meta_xrpassthroughocclusion": {"app": "meta_openxr_sdk", "exe": "XrSamples/XrPassthroughOcclusion/xrpassthroughocclusion",      "room": "living_room", "script": script_passthrough_occlusion},
    "meta_xrscenesharing":         {"app": "meta_openxr_sdk", "exe": "XrSamples/XrSceneSharing/scenesharing",                        "room": "living_room", "script": script_still},
    "meta_xrscenemodel":           {"app": "meta_openxr_sdk", "exe": "XrSamples/XrSceneModel/scenemodel",                          "room": "living_room", "script": script_scene_model},
    "meta_xrspatialanchor":        {"app": "meta_openxr_sdk", "exe": "XrSamples/XrSpatialAnchor/spatialanchor",                      "room": "living_room", "script": script_spatial_anchor},
}


def script_godot_still(shell, shot):
    # The driver already ran the demo until it held still
    shot("start")
    return []


def script_godot_binding_modifiers(shell, shot):
    # The panels on the controllers show the trigger thresholds the demo saw the click change at
    # (right 0.7 and 0.3, left 0.55 and 0.45, XR_VALVE_analog_threshold) and the dpad directions
    # of the thumbsticks (XR_EXT_dpad_binding); the head leans over the panels, the controllers
    # are lowered out of the way
    shell.run(cts_scenarios.pose("head", (0.0, 1.35, -0.35), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(-35.0))))
    shell.run(cts_scenarios.pose("left", (-0.3, 0.8, -0.2)))
    shell.run(cts_scenarios.pose("right", (0.3, 0.8, -0.2)))
    shell.frames(5)
    shot("start")
    ramp = [i / 20.0 for i in range(21)]
    for value in ramp + ramp[::-1]:
        shell.run(f"input left trigger/value {value}")
        shell.run(f"input right trigger/value {value}")
        shell.frames(2)
    shell.run("input right thumbstick 0 1")
    shell.run("input left thumbstick 1 0")
    shell.frames(5)
    shot("thresholds_dpad")
    return []


def script_godot_render_models(shell, shot):
    # The demo draws the controllers from XR_EXT_render_model: the right one is held up in front
    # of the eyes, then the trigger and A press its trigger and buttons nodes in
    shot("start")
    shell.run(cts_scenarios.pose("right", (0.03, 1.5, -0.2), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(60.0))))
    shell.frames(5)
    shot("model")
    shell.run("input right trigger/value 1")
    shell.run("input right a/click on")
    shell.frames(5)
    shot("pressed")
    return []


def script_godot_button(shell, shot):
    # The right pointer rests just below "Press this button" of the composition layer; tilted up
    # 4 degrees it is on the button, and the trigger (select on the simple profile) presses it
    shot("start")
    right = shell.run("pose right")
    if not right:
        return ["no right controller pose"]
    up = cts_scenarios.quat_mul(cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(4.0)), right["orientation"])
    shell.run(cts_scenarios.pose("right", right["position"], up))
    shell.frames(5)
    shell.run("input right trigger/value 1")
    shell.frames(10)
    shell.run("input right trigger/value 0")
    shell.frames(10)
    shot("pressed")
    return []


def script_godot_walk(shell, shot, out):
    # The move action on the right thumbstick walks the player forward. The shot waits until the
    # player stopped: on macOS Godot now and then draws a frame with the uniforms of the frame
    # before (the world origin it hands the renderer is current), which a moving view shows
    shot("start")
    shell.run("input right thumbstick 0 1")
    run_frames(shell, 45, chunk=10)
    shell.run("input right thumbstick 0 0")
    if not settle_still(shell, out):
        return ["the player never stopped"]
    shot("walked")
    return []


def script_godot_hands(shell, shot):
    # With hand tracking the demo's hand meshes follow the simulated joints
    shot("controllers")
    hands_forward(shell)
    run_frames(shell, 20, chunk=10)
    shot("hands")
    shell.run("input right trigger/value 1")
    run_frames(shell, 10, chunk=10)
    shot("pinch")
    return []


def script_godot_passthrough(shell, shot):
    # A switches between the virtual scene and passthrough of the room
    shot("start")
    press(shell, "right", "a/click")
    shell.frames(10)
    shot("toggled")
    return []


def settle_still(shell, out, limit=30, pixels=0):
    # Godot compiles pipelines in the background and draws with fallbacks until they are ready,
    # so when a frame looks final depends on the machine; a still demo is final once two frames
    # 10 apart are equal, or differ in at most `pixels`
    previous = None
    for i in range(limit):
        path = os.path.join(out, f"settle_{i % 2}.png")
        if shell.run(f"screenshot {path}") is None:
            return False
        current = cts_composition.read_png(path)
        if current == previous or (pixels and previous and differing_pixels(previous, current) <= pixels):
            return True
        previous = current
        run_frames(shell, 10, chunk=10)
    return False


def differing_pixels(a, b):
    if a[:2] != b[:2]:
        return a[0] * a[1]
    pa, pb = a[2], b[2]
    return sum(1 for i in range(0, len(pa), 4) if pa[i:i + 3] != pb[i:i + 3])


def godot_tests():
    # Each Godot demo on both renderers: godot_<demo>_gl and godot_<demo>_vk
    demos = {
        "binding_modifier_demo":       {"room": "none", "script": script_godot_binding_modifiers},
        "character_centric_movement":  {"room": "none", "script": script_godot_walk, "needs_out": True},
        "composition_layers":          {"room": "none", "script": script_godot_button},
        "hand_tracking_demo":          {"room": "none", "script": script_godot_hands},
        "origin_centric_movement":     {"room": "none", "script": script_godot_walk, "needs_out": True},
        "passthrough":                 {"room": "living_room", "script": script_godot_passthrough},
        "render_models":               {"room": "none", "script": script_godot_render_models},
        "spectator_view":              {"room": "none", "script": script_godot_still},
    }
    # Godot sets the pixel format of its window twice, which Mesa's WGL refuses, so on Windows
    # the compatibility renderer cannot draw on llvmpipe
    # macOS has no OpenGL for puppetxr to share
    renderers = [r for r in GODOT_RENDERERS if not ((WINDOWS or MACOS) and r == "gl")]
    tests = {}
    for demo, spec in demos.items():
        for renderer in renderers:
            tests[f"godot_{demo}_{renderer}"] = dict(spec, app="godot_demos", project=f"xr/openxr_{demo}", renderer=renderer)
    return tests


TESTS.update(godot_tests())


def aim_ray_down(hand, target):
    # The vendors samples cast their pointer rays along the grip's -Y: pitching the grip up 90
    # degrees points the ray forward, then it turns towards the target
    d = (target[0] - hand[0], target[1] - hand[1], target[2] - hand[2])
    yaw = math.atan2(-d[0], -d[2])
    pitch = math.atan2(d[1], math.hypot(d[0], d[2]))
    return cts_scenarios.quat_mul(cts_scenarios.quat_axis_angle((0.0, 1.0, 0.0), yaw), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(90.0) + pitch))


def script_vendors_scene(shell, shot):
    # The scene manager places a labelled box or plane on every entity of the room. The sample's
    # occlusion by environment depth works on Android only, so B turns it off first; then Y
    # switches from passthrough to the virtual background.
    press(shell, "right", "b/click")
    shell.frames(5)
    shot("scene")
    press(shell, "left", "y/click")
    shell.frames(5)
    shot("virtual")
    return []


def script_vendors_passthrough(shell, shot):
    # The right ray clicks "Mono Map" (2 m right, 1 m up, 3 m ahead of the stage origin), which
    # turns passthrough grey and shows the filter's ramp
    shot("start")
    hand = (0.2, 1.3, -0.3)
    shell.run(cts_scenarios.pose("right", hand, aim_ray_down(hand, (2.0, 1.0, -3.0))))
    shell.frames(5)
    shell.run("input right trigger/value 1")
    shell.frames(5)
    shell.run("input right trigger/value 0")
    shell.frames(5)
    shot("mono_map")
    return []


def script_vendors_hands(shell, shot):
    # The panels light the pinch strengths and discrete signals of each hand
    hands_forward(shell)
    run_frames(shell, 10, chunk=10)
    shot("hands")
    shell.run("input right trigger/value 1")
    run_frames(shell, 10, chunk=10)
    shot("pinch")
    return []


def script_vendors_space_warp(shell, shot):
    # A switches XR_FB_space_warp off, which the panel shows
    shot("start")
    press(shell, "right", "a/click")
    shell.frames(5)
    shot("disabled")
    return []


def script_vendors_metrics(shell, shot):
    # The HUD reads the XR_META_performance_metrics counters once a second and drifts in front
    shell.frames(SETTLE_FRAMES)
    run_frames(shell, 60, chunk=10)
    shot("metrics")
    return []


def script_vendors_demo(shell, shot):
    # Controllers from XR_FB_render_model, then hands from XR_FB_hand_tracking_mesh
    shot("controllers")
    hands_forward(shell)
    run_frames(shell, 10, chunk=10)
    shot("hands")
    return []


def vendors_tests():
    # Samples of the Godot OpenXR vendors plugin, each on the renderer its project picks:
    # vendors_<sample>_gl and vendors_<sample>_vk
    samples = {
        "body_tracking":           {"project": "samples/body-tracking-sample",          "renderer": "gl", "room": "none",        "script": script_godot_still},
        "demo":                    {"project": "demo",                                  "renderer": "gl", "room": "none",        "script": script_vendors_demo},
        "meta_composition_layers": {"project": "samples/meta-composition-layers-sample", "renderer": "gl", "room": "none",       "script": script_godot_still},
        "meta_hand_tracking":      {"project": "samples/meta-hand-tracking-sample",     "renderer": "gl", "room": "none",        "script": script_vendors_hands},
        "meta_passthrough":        {"project": "samples/meta-passthrough-sample",       "renderer": "gl", "room": "living_room", "script": script_vendors_passthrough},
        "meta_scene":              {"project": "samples/meta-scene-sample",             "renderer": "gl", "room": "living_room", "script": script_vendors_scene},
        "meta_space_warp":         {"project": "samples/meta-space-warp-sample",        "renderer": "gl", "room": "none",        "script": script_vendors_space_warp},
        "performance_metrics":     {"project": "samples/performance-metrics-sample",    "renderer": "vk", "room": "living_room", "script": script_vendors_metrics},
    }
    # The samples skip their animations and quit when the session stops given this argument. The
    # name is upstream's, for Meta's XR Simulator, and has nothing to do with this project's names
    return {f"vendors_{name}_{spec['renderer']}": dict(spec, app="godot_vendors", args=["--", "--xrsim-automated-tests"]) for name, spec in samples.items()}


TESTS.update(vendors_tests())


def hello_xr_tests():
    # hello_xr of the OpenXR SDK per graphics API: cubes at its reference spaces and the hands.
    # Its OpenGL window needs XFree86-VidMode, which Xvfb lacks.
    tests = {}
    for api in ("Vulkan2", "Vulkan"):
        tests[f"hello_xr_{api.lower()}"] = {"app": "openxr_sdk", "exe": "src/tests/hello_xr/hello_xr", "args": ["-g", api], "room": "none",
                                           "script": script_still, "stdin_open": True, "display": True}
    return tests


TESTS.update(hello_xr_tests())


def script_simple_example(shell, shot):
    # A block follows each grip; the right trigger past 0.75 makes the right controller vibrate
    shell.frames(SETTLE_FRAMES)
    shot("start")
    shell.run(cts_scenarios.pose("right", (0.15, 1.5, -0.4), cts_scenarios.quat_axis_angle((0.0, 1.0, 0.0), math.radians(30.0))))
    shell.run("input right trigger/value 1")
    shell.frames(2)
    shot("moved")
    state = shell.run("haptic right")
    shell.run("input right trigger/value 0")
    shell.frames(2)
    if not state or not state.get("active"):
        return [f"no haptic on the right controller ({state})"]
    return []


# Monado's xrgears: Vulkan gears in a projection layer over an equirect sky, and quad layers.
# With -1 (XR_KHR_vulkan_enable) it asks for external semaphore and memory fd device
# extensions itself, which lavapipe lacks.
TESTS["xrgears_vk"] = {"app": "xrgears", "exe": "src/xrgears", "room": "none", "script": script_still}

# Monado's openxr-simple-example: OpenGL over GLX through SDL2, in the Xvfb display
TESTS["simple_example_gl"] = {"app": "openxr_simple_example", "exe": "openxr-example", "room": "none", "script": script_simple_example, "display": True}


# Meta's MRUK samples for Unity: a start menu (the buttons top to bottom) loads each sample scene
# into the simulated living room. OVRPlugin refuses runtimes not named like Meta's, casts its UI
# ray from the grip turned 60 degrees down and clicks when the right trigger is released over a
# button. They draw with D3D11 on the GPU, so the references are of one machine. The player's
# -puppetxr-fixed-step (tests/samples/unity_mruk/PuppetxrFixedStep.cs) makes their animations and
# random numbers follow the stepped frames.
MRUK_SCENES = ["MRUKBase", "KeyboardTracker", "BouncingBall", "DestructibleMesh", "EnvironmentPanelPlacement", "FindFloorZone",
               "FindMultiSpawn", "NavMesh", "QRCodeDetection", "PassthroughRelighting", "SpaceMap", "VirtualHome"]
MRUK_BUTTON_U = 0.511
# The UI cursor lands a third of a button above where the ray aims, so the aims sit lower than the
# buttons' centres; at the centres the cursor was on the top edges and VirtualHome's click missed
MRUK_BUTTON_V = [(703.0 + 32.27 * i) / 1760.0 for i in range(len(MRUK_SCENES))]
MRUK_BUTTON_BLUE = (22, 98, 216)


def pixel_at(path, u, v):
    width, height, pixels = cts_composition.read_png(path)
    i = (int(v * height) * width + int(u * width)) * 4
    return tuple(pixels[i:i + 3])


def wait_until(shell, out, condition, frames=30, limit=60.0, abort=None):
    # Loading and Unity's splash screen take wall-clock time, so frames run until the
    # screenshot shows the state, for at most `limit` seconds. Without a frame there is no
    # screenshot: Unreal loads, compiling shaders the first time, before it creates a session,
    # and the paused simulator ends no frame until it is stepped; the step returns once one ran
    path = os.path.join(out, "wait.png")
    deadline = time.time() + limit
    while time.time() < deadline and shell.alive and not (abort and abort()):
        if shell.run(f"screenshot {path}") is None:
            shell.frames(1)
            continue
        if condition(path):
            return True
        run_frames(shell, frames, chunk=frames)
    return False


def mruk_menu_shown(path):
    color = pixel_at(path, 0.446, MRUK_BUTTON_V[0])
    return all(abs(a - b) <= 8 for a, b in zip(color, MRUK_BUTTON_BLUE))


def mruk_click(shell, v):
    shell.run(f"point right {MRUK_BUTTON_U} {v} left grip -60")
    shell.frames(10)
    shell.run("input right trigger/value 1")
    shell.frames(10)
    shell.run("input right trigger/value 0")
    shell.frames(10)
    shell.run("reset hands")


def mruk_open_scene(shell, out, index):
    # A scene loads in wall-clock time after its button is clicked; VirtualHome took longer than
    # the frames after the click, and its shot was the menu still up
    mruk_click(shell, MRUK_BUTTON_V[index])
    return wait_until(shell, out, lambda path: not mruk_menu_shown(path), frames=10)


def script_mruk_menu(shell, shot, out, pixels=0):
    # The menu follows the head lazily until it is in front
    if not settle_still(shell, out, pixels=pixels):
        return ["the menu never held still"]
    shot("menu")
    return []


def mruk_scene_script(index):
    # The controllers are lit by some scenes' moving lights, so they leave before the screenshot,
    # their rays first turned to the floor: a ray on a panel may stay drawn after they left
    def script(shell, shot, out):
        if not mruk_open_scene(shell, out, index):
            return [f"{MRUK_SCENES[index]} never replaced the menu"]
        shell.run("point left 0.5 1 left grip -60")
        shell.run("point right 0.5 1 left grip -60")
        run_frames(shell, 10, chunk=10)
        shell.run("active left off")
        shell.run("active right off")
        run_frames(shell, 300, chunk=50)
        shot("scene")
        return []
    return script


def script_mruk_destructible(shell, shot, out):
    # The trigger destroys the segment along the right controller's forward axis (its grip, not
    # the aim ray), turned 30 degrees down onto the TV here; the segment's wireframe goes with it
    mruk_scene_script(MRUK_SCENES.index("DestructibleMesh"))(shell, shot, out)
    shell.run("active right on")
    shell.run("point right 0.5 0.3 left grip -30")
    run_frames(shell, 20, chunk=20)
    # OVRPlugin on the Oculus loader reads the trigger on its own time, so it stays down a while
    shell.run("input right trigger/value 1")
    run_frames(shell, 30, chunk=30)
    shell.run("input right trigger/value 0")
    run_frames(shell, 30, chunk=30)
    shell.run("point right 0.5 1 left grip -60")
    run_frames(shell, 10, chunk=10)
    shell.run("active right off")
    run_frames(shell, 60, chunk=60)
    shot("destroyed")
    return []


def head_down(degrees):
    return cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(-degrees)))


def mruk_walk(shell, x, y, frames):
    shell.run(f"input left thumbstick {x} {y}")
    run_frames(shell, frames, chunk=frames)
    shell.run("input left thumbstick 0 0")
    run_frames(shell, 60, chunk=60)


def script_mruk_relighting(shell, shot, out, walk=True):
    # Oppy stands at the user's feet and lights the passthrough around him; the left stick walks
    # him, relative to the view. The stick needs the controllers tracked, so they go behind the
    # head instead of leaving.
    # Oppy spawns in front of the head once the scene is loaded, which on the Oculus loader
    # happens on its own time, so the head stays put until he is there.
    if not mruk_open_scene(shell, out, MRUK_SCENES.index("PassthroughRelighting")):
        return ["PassthroughRelighting never replaced the menu"]
    run_frames(shell, 300, chunk=50)
    shell.run(head_down(35))
    shell.run(cts_scenarios.pose("left", (-0.3, 1.0, 0.5)))
    shell.run(cts_scenarios.pose("right", (0.3, 1.0, 0.5)))
    run_frames(shell, 60, chunk=60)
    shot("oppy")
    # The eye low behind the coffee table looks at Oppy through it: Meta's Depth API (Oculus
    # loader only) hides him behind the table, then the eye rises above it
    shell.run(cts_scenarios.pose("head", (0.0, 0.55, 1.4), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(-10.4))))
    run_frames(shell, 60, chunk=60)
    shot("behind_table")
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 1.4), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(-35.0))))
    run_frames(shell, 60, chunk=60)
    shot("over_table")
    if not walk:
        return []
    shell.run(head_down(35))
    run_frames(shell, 60, chunk=60)
    mruk_walk(shell, 0, 1, 90)
    shot("walked")
    mruk_walk(shell, 1, 0, 60)
    shot("walked_right")
    return []


def unity_mruk_tests(app, instance, scenes):
    # unity_mruk on Unity's OpenXR loader, unity_mruk_oculus on its Oculus XR plugin, where
    # OVRPlugin drives the runtime itself and the app's instance is named "Unity"
    exe = "player/MRUK.app/Contents/MacOS/MRUKSample" if MACOS else "player/MRUK.exe"
    common = {"app": app, "exe": exe, "room": "living_room", "config": {"views": GODOT_VIEWS},
              "args": ["-screen-fullscreen", "0", "-screen-width", "320", "-screen-height", "240", "-logFile", "{out}/player.log",
                       "-puppetxr-fixed-step"],
              # puppetxr recognises OVRPlugin and runs as Oculus with time 0 accepted, without settings
              "env": {}, "instance": instance, "ready": mruk_menu_shown, "needs_out": True, "exit_crash_ok": MACOS,
              "start_alone": True, "retry_start_crash": True,
              # OVRPlugin paces Unity's frames on the Oculus loader, not locked to the steps, so
              # Oppy's idle animation and FindMultiSpawn's moving spawns land a frame apart now and then
              "max_pixels": 10000 if app == "unity_mruk_oculus" else 0}
    # The trackers' scenes get what they track: a keyboard tilted towards the head below the
    # menu, a QR code facing the head at its height
    props = {"KeyboardTracker": '[{"kind": "keyboard", "position": [0, 1.3, -0.6], "orientation": [0.4226183, 0, 0, 0.9063078]}]',
             "QRCodeDetection": '[{"kind": "qr", "text": "https://github.com/oculus-samples", "size": 0.15, "position": [0.25, 1.45, -0.7]}]'}
    # On the Oculus loader OVRPlugin reads the stick on its own time, so how far Oppy walks varies
    walk = app != "unity_mruk_oculus"
    scripts = {"PassthroughRelighting": lambda shell, shot, out: script_mruk_relighting(shell, shot, out, walk), "DestructibleMesh": script_mruk_destructible}
    tests = {f"{app}_menu": dict(common, script=script_mruk_menu)}
    for i, scene in enumerate(MRUK_SCENES):
        if scene not in scenes:
            continue
        env = dict(common["env"], **({"PUPPETXR_PROPS": props[scene]} if scene in props else {}))
        # VirtualHome's lit furniture moves a few pixels by one level from run to run
        tests[f"{app}_{scene.lower()}"] = dict(common, script=scripts.get(scene, mruk_scene_script(i)), env=env,
                                               max_pixels=max(common["max_pixels"], 100 if scene == "VirtualHome" else 0))
    # On macOS MRUK logs its asynchronous events onto the tracker's panel in varying order
    tests[f"{app}_keyboardtracker"]["ignore_macos"] = [(0.31, 0.43, 0.70, 0.625)]
    return tests


TESTS.update(unity_mruk_tests("unity_mruk", "MRUKSample", MRUK_SCENES))
TESTS.update(unity_mruk_tests("unity_mruk_oculus", "Unity", MRUK_SCENES))


# Meta's Shared Spatial Anchors sample. Its panel hangs off the left controller, which holds still
# in front of the head; buttons are clicked like MRUK's, the cursor sitting 0.033 above the ray.
SSA_PANEL_POSE = cts_scenarios.pose("left", (-0.05, 1.45, -0.3), (0.5, 0.0, 0.0, 0.866))
SSA_COLOCATION = (0.540, 0.576)
SSA_START_ADVERTISING = (0.354, 0.345)
SSA_START_DISCOVERING = (0.351, 0.368)
SSA_FIRST_GROUP = (0.36, 0.425)
SSA_PLACE_ANCHORS = (0.339, 0.506)
SSA_SHARE_ALL = (0.327, 0.536)
SSA_RELOAD_FROM_GROUP = (0.315, 0.569)
SSA_ANCHOR_AT = (0.3, 1.3, -0.8)


def ssa_click(shell, at, frames=100):
    shell.run(SSA_PANEL_POSE)
    shell.run(f"point right {at[0]} {at[1]} left grip -60")
    shell.frames(10)
    shell.run("input right trigger/value 1")
    shell.frames(10)
    shell.run("input right trigger/value 0")
    run_frames(shell, frames)


def ssa_panel_shown(path):
    # The colocation button, dark on the light wall
    return sum(pixel_at(path, SSA_COLOCATION[0], SSA_COLOCATION[1] - 0.033)) < 300


def ssa_open_colocation(shell, out):
    shell.run(SSA_PANEL_POSE)
    if not wait_until(shell, out, ssa_panel_shown):
        return False
    ssa_click(shell, SSA_COLOCATION, frames=300)
    return True


def ssa_look_at_anchor(shell):
    # The panel prints wall-clock times, so it goes behind the head
    shell.run(cts_scenarios.pose("left", (-0.2, 1.0, 0.5)))
    shell.run(cts_scenarios.pose("right", (0.2, 1.0, 0.5)))
    # The anchor is a gizmo of 10 cm with a label panel, so the head comes close, looking down
    x, y, z = SSA_ANCHOR_AT
    q = cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(-25.0))
    shell.run(cts_scenarios.pose("head", (x, y + 0.2, z + 0.4), q))
    run_frames(shell, 30)


def script_ssa_colocation(shell, shot, out, peer):
    # The host advertises a group, places an anchor and shares it with the group; the peer, a
    # second user on the same simulated cloud, discovers the group, joins it and loads its anchors
    if not ssa_open_colocation(shell, out) or not ssa_open_colocation(peer.shell, out):
        return ["the colocation panel never showed"]
    ssa_click(shell, SSA_START_ADVERTISING)
    ssa_click(shell, SSA_PLACE_ANCHORS)
    shell.run(cts_scenarios.pose("right", SSA_ANCHOR_AT))
    shell.frames(10)
    shell.run("input right trigger/value 1")
    shell.frames(10)
    shell.run("input right trigger/value 0")
    run_frames(shell, 100)
    ssa_click(shell, SSA_SHARE_ALL, frames=200)
    ssa_click(peer.shell, SSA_START_DISCOVERING, frames=200)
    ssa_click(peer.shell, SSA_FIRST_GROUP)
    ssa_click(peer.shell, SSA_RELOAD_FROM_GROUP, frames=300)
    ssa_look_at_anchor(shell)
    ssa_look_at_anchor(peer.shell)
    shot("host_anchor")
    peer.shot("anchor")
    # The anchor's gizmo has a red X axis; without the shared anchor neither shot has one
    problems = []
    for name in ("host_anchor", "peer_anchor"):
        width, height, pixels = cts_composition.read_png(os.path.join(out, f"{name}.png"))
        red = sum(1 for i in range(0, len(pixels), 4) if pixels[i] > 180 and pixels[i + 1] < 70 and pixels[i + 2] < 70)
        if red < 40:
            problems.append(f"{name}: no anchor gizmo ({red} red pixels)")
    return problems


# On Unity's OpenXR loader and on its Oculus XR plugin, whose instance is named "Unity"
for ssa_app, ssa_instance in (("unity_ssa", "SharedSpatialAnchorsSample"), ("unity_ssa_oculus", "Unity")):
    TESTS[f"{ssa_app}_colocation"] = {
        "app": ssa_app, "exe": "player/SSA.exe", "room": "living_room", "config": {"views": GODOT_VIEWS},
        "args": ["-screen-fullscreen", "0", "-screen-width", "320", "-screen-height", "240", "-logFile", "{out}/{prefix}player.log",
                 "-puppetxr-fixed-step"],
        "instance": ssa_instance,
        "needs_out": True, "start_alone": True, "retry_start_crash": True, "peer": True, "script": script_ssa_colocation,
        # OVRPlugin paces the Oculus loader's frames, so the peer's anchor lands a few pixels apart
        "max_pixels": 4000 if ssa_app == "unity_ssa_oculus" else 0,
    }


# Meta's Unity Starter Samples and Passthrough Camera API samples: each scene opened directly
# (-puppetxr-scene, tests/samples/unity/PuppetxrFixedStep.cs)
def unity_scene_tests(app, exe, instance, scenes):
    tests = {}
    for scene in scenes:
        tests[f"{app}_{scene.lower()}"] = {
            "app": app, "exe": exe, "room": "living_room", "config": {"views": GODOT_VIEWS},
            "args": ["-screen-fullscreen", "0", "-screen-width", "320", "-screen-height", "240", "-logFile", "{out}/player.log",
                     "-puppetxr-fixed-step", "-puppetxr-scene", scene],
            "instance": instance, "ready": lambda path: True, "needs_out": True, "start_alone": True, "retry_start_crash": True,
            "script": script_look_around, "max_pixels": 0,
        }
    return tests


def unity_click(shell, at):
    # The Meta XR SDK's UI ray is the grip turned 60 degrees down; a click is the trigger's release
    shell.run(f"point right {at[0]} {at[1]} left grip -60")
    shell.frames(10)
    shell.run("input right trigger/value 1")
    shell.frames(10)
    shell.run("input right trigger/value 0")
    shell.frames(10)


def script_unity_starter_overlay_modes(shell, shot, out):
    # The comparison's modes: the panels as compositor quads (OVROverlay), drawn by the app
    # (Application), not at all (None); then a simulated level load shows a loading overlay
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
    run_frames(shell, 100, chunk=50)
    shot("ovroverlay")
    for name, at in (("application", (0.425, 0.689)), ("none", (0.425, 0.720)), ("level_load", (0.503, 0.553))):
        unity_click(shell, at)
        shell.run("reset hands")
        run_frames(shell, 30, chunk=30)
        shot(name)
    return []


def script_unity_starter_toggle_passthrough(shell, shot, out):
    # A toggles between the virtual sky and passthrough. The scene starts as if it had just switched
    # to passthrough, so its first press asks for the sky it shows; the second turns the passthrough
    # layer on and, once its resumed event arrives (XR_META_passthrough_layer_resumed_event), fades
    # the sky away; the third fades it back
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
    run_frames(shell, 60, chunk=60)
    shot("start")
    for name in ("first_press", "on", "off"):
        press(shell, "right", "a/click")
        run_frames(shell, 200, chunk=100)
        shot(name)
    return []


def unity_trigger(shell, hand="right"):
    shell.run(f"input {hand} trigger/value 1")
    run_frames(shell, 10, chunk=10)
    shell.run(f"input {hand} trigger/value 0")
    run_frames(shell, 10, chunk=10)


def script_unity_starter_anchor(shell, shot, out):
    # The menu on the right controller: the trigger picks Enter Create Mode, then A places an anchor
    # at the controller (XR_FB_spatial_entity), which stays when the hand leaves
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(-25.0))))
    shell.run(cts_scenarios.pose("right", (0.15, 1.3, -0.4)))
    run_frames(shell, 60, chunk=60)
    shot("menu")
    unity_trigger(shell)
    run_frames(shell, 30, chunk=30)
    shot("create_mode")
    press(shell, "right", "a/click")
    run_frames(shell, 30, chunk=30)
    shell.run(cts_scenarios.pose("right", (0.45, 1.0, 0.2)))
    run_frames(shell, 60, chunk=60)
    shot("anchor")
    return []


def unity_stick_down(shell, times=1):
    for _ in range(times):
        shell.run("input right thumbstick 0 -1")
        run_frames(shell, 5, chunk=5)
        shell.run("input right thumbstick 0 0")
        run_frames(shell, 5, chunk=5)


def stored_anchors(shell):
    reply = shell.run("anchors")
    return reply.get("count", -1) if reply else -1


def script_unity_starter_anchor_menu(shell, shot, out):
    # An anchor placed, the trigger switches back to select mode; the ray on the anchor and A open
    # its menu (Save, Hide, Erase), the stick moves through it and the trigger presses. Saved and
    # hidden, the main menu's Load Anchors brings it back from the store, then Erase removes it
    problems = []
    shell.run("anchors wipe")
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(-25.0))))
    shell.run(cts_scenarios.pose("right", (0.15, 1.3, -0.4)))
    run_frames(shell, 60, chunk=60)
    unity_trigger(shell)
    press(shell, "right", "a/click")
    run_frames(shell, 30, chunk=30)
    unity_trigger(shell)
    shell.run("point right 0.633 0.51 left grip -60 ahead 0.3")
    run_frames(shell, 10, chunk=10)
    shot("hover")
    press(shell, "right", "a/click")
    run_frames(shell, 10, chunk=10)
    shot("selected")
    unity_trigger(shell)
    run_frames(shell, 30, chunk=30)
    shot("saved")
    if stored_anchors(shell) != 1:
        problems.append(f"saved: {stored_anchors(shell)} stored anchors, expected 1")
    unity_stick_down(shell)
    unity_trigger(shell)
    run_frames(shell, 10, chunk=10)
    shot("hidden")
    unity_stick_down(shell)
    unity_trigger(shell)
    run_frames(shell, 60, chunk=60)
    shot("loaded")
    press(shell, "right", "a/click")
    run_frames(shell, 10, chunk=10)
    unity_stick_down(shell, 2)
    unity_trigger(shell)
    run_frames(shell, 30, chunk=30)
    shot("erased")
    if stored_anchors(shell) != 0:
        problems.append(f"erased: {stored_anchors(shell)} stored anchors, expected 0")
    return problems


def script_unity_starter_teleport(shell, shot, out):
    # The right stick pushed forward aims the teleport arc; released, the player lands at its end
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
    run_frames(shell, 60, chunk=60)
    shot("start")
    shell.run("input right thumbstick 0 1")
    run_frames(shell, 40, chunk=40)
    shot("aiming")
    shell.run("input right thumbstick 0 0")
    run_frames(shell, 60, chunk=60)
    shot("teleported")
    return []


UNITY_STARTER_SCENES = ["StartScene", "Passthrough", "EnableDisablePassthrough", "OVROverlay", "OVROverlayCanvas", "OVROverlayCanvas_Text",
                        "OVROverlayCanvas_RenderOnChange", "SpatialAnchor", "CustomHands", "CustomControllers", "Locomotion",
                        "TouchProSample", "HandsInteractionTrainScene"]
TESTS.update(unity_scene_tests("unity_starter", "player/StarterSamples.exe", "Starter Samples", UNITY_STARTER_SCENES))
# The Meta XR Core SDK's own samples, built into the same player (cmake/samples.cmake, PACKAGE_SAMPLES)
UNITY_CORE_SCENES = ["HandMicrogestures", "HandTest", "HandTrackingWideMotionMode", "SimultaneousHandsAndControllers",
                     "ControllerDrivenHandPoses", "ControllerModels", "HapticsSample"]
TESTS.update(unity_scene_tests("unity_starter", "player/StarterSamples.exe", "Starter Samples", UNITY_CORE_SCENES))


def unity_core_look(shell):
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
    run_frames(shell, 100, chunk=50)


def script_unity_core_microgestures(shell, shot, out):
    # XR_META_hand_tracking_microgestures through OVRMicrogestureEventSource: each thumb gesture
    # lights its arrow or the select icon of that hand and names it below
    unity_core_look(shell)
    hands_forward(shell)
    run_frames(shell, 60, chunk=60)
    shot("hands")
    for hand, gestures in (("right", ("swipe_left", "swipe_forward")), ("left", ("swipe_right", "tap_thumb"))):
        for name in gestures:
            shell.run(f"input {hand} {name}_meta/click 1")
            shell.frames(3)
        shell.frames(5)
        shot(hand)
    return []


def script_unity_core_hands(shell, shot, out):
    # Tracked hands with Meta's hand mesh and skeleton, open, then the right one pinching
    unity_core_look(shell)
    hands_forward(shell)
    run_frames(shell, 60, chunk=60)
    shot("open")
    shell.run("input right trigger/value 1")
    run_frames(shell, 30, chunk=30)
    shot("pinch")
    return []


def script_unity_core_simultaneous(shell, shot, out):
    # Enable makes OVRInput report hands and controllers at once: the left hand tracked, the
    # right controller held, the panel names the active controllers
    unity_core_look(shell)
    shot("controllers")
    unity_click(shell, (0.505, 0.478))
    shell.run("reset hands")
    shell.run("hand left hand")
    shell.run(cts_scenarios.pose("left", (-0.15, 1.3, -0.35), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(80.0))))
    run_frames(shell, 60, chunk=60)
    shot("hand_and_controller")
    return []


def script_unity_core_controller_hands(shell, shot, out):
    # Hands drawn from the controllers (XR_EXT_hand_tracking_data_source): conforming to the
    # controller, natural, off
    unity_core_look(shell)
    controllers_ahead = lambda: (shell.run(cts_scenarios.pose("left", (-0.12, 1.35, -0.35))), shell.run(cts_scenarios.pose("right", (0.12, 1.35, -0.35))))
    controllers_ahead()
    run_frames(shell, 30, chunk=30)
    shot("conforming")
    for name, at in (("natural", (0.505, 0.613)), ("off", (0.505, 0.536))):
        unity_click(shell, at)
        controllers_ahead()
        run_frames(shell, 30, chunk=30)
        shot(name)
    return []


def script_unity_core_haptics(shell, shot, out):
    # Each button plays its kind of haptics on both controllers; Get Parametric Properties shows
    # what the controllers report
    unity_core_look(shell)
    problems = []
    for name, at in (("simple", (0.406, 0.253)), ("pcm", (0.406, 0.342)), ("parametric", (0.406, 0.432))):
        unity_click(shell, at)
        for hand in ("left", "right"):
            state = shell.run(f"haptic {hand}") or {}
            if not state.get("active"):
                problems.append(f"{name}: no haptic on the {hand} controller ({state})")
        shell.run("reset hands")
        run_frames(shell, 300, chunk=100)
    unity_click(shell, (0.608, 0.432))
    shell.run("reset hands")
    run_frames(shell, 30, chunk=30)
    shot("properties")
    return problems


TESTS["unity_starter_microgestures"] = dict(TESTS["unity_starter_handmicrogestures"], script=script_unity_core_microgestures)
TESTS["unity_starter_hands"] = dict(TESTS["unity_starter_handtest"], script=script_unity_core_hands)
TESTS["unity_starter_simultaneous"] = dict(TESTS["unity_starter_simultaneoushandsandcontrollers"], script=script_unity_core_simultaneous)
TESTS["unity_starter_controller_hands"] = dict(TESTS["unity_starter_controllerdrivenhandposes"], script=script_unity_core_controller_hands)
TESTS["unity_starter_haptics"] = dict(TESTS["unity_starter_hapticssample"], script=script_unity_core_haptics)
for scene in ("handmicrogestures", "handtest", "simultaneoushandsandcontrollers", "controllerdrivenhandposes", "hapticssample"):
    del TESTS["unity_starter_" + scene]
TESTS["unity_starter_passthrough_toggle"] = dict(TESTS["unity_starter_enabledisablepassthrough"], script=script_unity_starter_toggle_passthrough)
TESTS["unity_starter_anchor"] = dict(TESTS["unity_starter_spatialanchor"], script=script_unity_starter_anchor)
TESTS["unity_starter_anchor_menu"] = dict(TESTS["unity_starter_spatialanchor"], script=script_unity_starter_anchor_menu)
TESTS["unity_starter_teleport"] = dict(TESTS["unity_starter_locomotion"], script=script_unity_starter_teleport)
TESTS["unity_starter_ovroverlay_modes"] = dict(TESTS["unity_starter_ovroverlay"], script=script_unity_starter_overlay_modes)
# Meta's Depth API sample, on the Oculus XR plugin, whose instance is named "Unity"
UNITY_DEPTHAPI_SCENES = ["OcclusionToggler", "SceneAPIPlacement", "HandsRemoval", "DepthMask"]
TESTS.update(unity_scene_tests("unity_depthapi", "player/DepthAPI.exe", "Unity", UNITY_DEPTHAPI_SCENES))


def script_movement_body(shell, shot, out):
    # The Movement SDK's mirror avatar retargets puppetxr's body (docs/body-tracking.md): standing
    # with the arms on the controllers; a T-posed left arm with the right controller raised, every
    # bone connected; then a longer right upper arm, proportions no headset reports, to show how
    # the retargeting copes
    run_frames(shell, 100, chunk=50)
    shot("standing")
    shell.run('body set {"pose":"t_pose","limbs":{"left_arm":"posed"}}')
    shell.run("pose right 0.35 1.9 -0.3")
    run_frames(shell, 60, chunk=30)
    shot("posed")
    shell.run('body set {"bones":{"right_arm_upper":1.4}}')
    run_frames(shell, 60, chunk=30)
    shot("longer_arm")
    return []


def script_movement_face(shell, shot, out):
    # The Movement SDK's face scene shows puppetxr's face (docs/face-tracking.md) on its characters:
    # neutral without blinks, a smile, the eyes turned left, then shut with the jaw open
    shell.run('face set {"blink":false}')
    run_frames(shell, 100, chunk=50)
    shot("neutral")
    shell.run("face preset smile")
    run_frames(shell, 30, chunk=30)
    shot("smile")
    shell.run("face preset neutral")
    shell.run("face gaze 30 0")
    run_frames(shell, 30, chunk=30)
    shot("look_left")
    shell.run('face set {"preset":"eyes_closed","weights":{"jaw_drop":0.8},"gaze":[0,0]}')
    run_frames(shell, 30, chunk=30)
    shot("closed_open")
    return []


def script_movement_gaze(shell, shot, out):
    # XR_EXT_eye_gaze_interaction: the samples' scene menu points where the eyes look and a pinch
    # selects. Turned left and 25 degrees down, the menu is in view; the gaze 4.06 degrees right
    # and 9 down is on its Face button (in the 90 degree views a pixel is a gaze angle, docs/
    # face-tracking.md); a pinch of the tracked right hand loads the face scene, whose menu then
    # marks Face
    run_frames(shell, 100, chunk=50)
    shell.run("face blink off")
    shell.run("pose head 0 1.6 0 -0.153046 0.690346 0.153046 0.690346")
    shell.run("face gaze -4.06 -9")
    shell.run("hand right hand")
    run_frames(shell, 30, chunk=30)
    shot("aim")
    shell.run("input right trigger/value 1")
    shell.frames(10)
    shell.run("input right trigger/value 0")
    run_frames(shell, 120, chunk=40)
    shot("face_scene")
    return []


# Meta's Movement SDK in the Starter samples' project (cmake/samples.cmake): its body tracking
# scene first, its face tracking scene opened with -puppetxr-scene
TESTS["unity_movement_body"] = {
    "app": "unity_movement", "exe": "player/Movement.app/Contents/MacOS/Starter Samples" if MACOS else "player/Movement.exe",
    "room": "none", "config": {"views": GODOT_VIEWS},
    "args": ["-screen-fullscreen", "0", "-screen-width", "320", "-screen-height", "240", "-logFile", "{out}/player.log", "-puppetxr-fixed-step"],
    "instance": "Starter Samples", "ready": lambda path: True, "needs_out": True, "start_alone": True, "retry_start_crash": True,
    "script": script_movement_body, "max_pixels": 0,
}
TESTS["unity_movement_face"] = dict(TESTS["unity_movement_body"], script=script_movement_face,
    args=TESTS["unity_movement_body"]["args"] + ["-puppetxr-scene", "MovementFace"])
TESTS["unity_movement_gaze"] = dict(TESTS["unity_movement_body"], script=script_movement_gaze)


def script_unity_depthapi_modes(shell, shot, out):
    # A cycles soft, no and hard occlusion. The room is moved so that its TV wall stands between the
    # head and the objects (PUPPETXR_ORIGIN_OFFSET): occlusion hides them, none shows them. OVRPlugin
    # reads the button on its own time, so it is held
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
    run_frames(shell, 100, chunk=50)
    shot("mode0")
    for mode in (1, 2):
        press(shell, "right", "a/click", frames=30)
        run_frames(shell, 30, chunk=30)
        shot(f"mode{mode}")
    return []


def script_unity_depthapi_hands(shell, shot, out):
    # Tracked hands held before the eyes, over the room's depth; the scene removes them from it,
    # drawn as its virtual hand mask
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(-20.0))))
    shell.run("hand left hand")
    shell.run("hand right hand")
    shell.run("reset hands")
    run_frames(shell, 100, chunk=50)
    shot("hands")
    # Thumb taps (XR_META_hand_tracking_microgestures): the right hand's switches the occlusion mode
    # (soft to none), the left hand's the hands' style (the mask to none)
    for name, hand in (("occlusion", "right"), ("style", "left")):
        shell.run(f"input {hand} tap_thumb_meta/click 1")
        run_frames(shell, 60, chunk=60)
        shot(name)
    return []


def script_unity_depthapi_posters(shell, shot, out):
    # The right controller's ray on the wall shows a poster's preview, the trigger places a poster
    # there, B takes every poster down
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
    shell.run("point right 0.5 0.4 left ahead 0.4")
    run_frames(shell, 100, chunk=50)
    shot("preview")
    unity_trigger(shell)
    shell.run("point right 0.25 0.4 left ahead 0.4")
    run_frames(shell, 30, chunk=30)
    shot("placed")
    press(shell, "right", "b/click", frames=10)
    run_frames(shell, 30, chunk=30)
    shot("cleared")
    return []


def script_unity_depthapi_mask(shell, shot, out):
    # B masks the room's mesh out of the environment depth, the stick held right raises the mask's
    # bias, B again removes the mask
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
    run_frames(shell, 100, chunk=50)
    shot("unmasked")
    press(shell, "right", "b/click", frames=10)
    run_frames(shell, 30, chunk=30)
    shot("masked")
    shell.run("input right thumbstick 1 0")
    run_frames(shell, 60, chunk=60)
    shell.run("input right thumbstick 0 0")
    run_frames(shell, 10, chunk=10)
    shot("bias")
    press(shell, "right", "b/click", frames=10)
    run_frames(shell, 30, chunk=30)
    shot("off")
    return []

# OVRPlugin paces the Oculus loader's frames, not locked to the steps: the torch's fire flickers
TESTS["unity_depthapi_occlusiontoggler"]["max_pixels"] = 30000
TESTS["unity_depthapi_hands"] = dict(TESTS["unity_depthapi_handsremoval"], script=script_unity_depthapi_hands)
TESTS["unity_depthapi_posters"] = dict(TESTS["unity_depthapi_sceneapiplacement"], script=script_unity_depthapi_posters)
TESTS["unity_depthapi_mask"] = dict(TESTS["unity_depthapi_depthmask"], script=script_unity_depthapi_mask)
TESTS["unity_depthapi_occlusionmodes"] = dict(TESTS["unity_depthapi_occlusiontoggler"], script=script_unity_depthapi_modes, env={"PUPPETXR_ORIGIN_OFFSET": "0,0,-2.0"})
# The train set's smoke lands a little apart from run to run
TESTS["unity_starter_handsinteractiontrainscene"]["max_pixels"] = 2000


# Meta's MRUK sample for Unreal, on Meta's fork of the engine (its OpenXR plugin and MRUK's shared
# library, no OVRPlugin), run uncooked by the editor named by --unreal with a fixed time step. Its
# menu shows the room it discovered, dressed in its own walls, floor and a dark ceiling.
def unreal_mruk_dressed(path):
    return sum(pixel_at(path, 0.5, 0.15)) < 60


def unreal_mruk_menu_up(path):
    # The blue button of the menu in front of the head turned 10 degrees left, lighter on the
    # desktop renderer than on the mobile one
    width, height, pixels = cts_composition.read_png(path)
    count = 0
    for y in range(int(0.64 * height), int(0.70 * height)):
        for x in range(int(0.44 * width), int(0.59 * width)):
            i = (y * width + x) * 4
            count += pixels[i + 2] > 180 and pixels[i] < 100
    return count > 500


def script_unreal_mruk_menu(shell, shot, out):
    # Furniture of a random colour stands at the right edge of the view, so the head turns left of
    # it; a few pixels of the lit room change by one level from frame to frame. The menu comes up
    # in wall-clock time: where the room is dressed in a few seconds, the room alone held still.
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((0.0, 1.0, 0.0), math.radians(10.0))))
    if not wait_until(shell, out, unreal_mruk_menu_up):
        return ["the menu never came up"]
    return script_mruk_menu(shell, shot, out, pixels=50)


def script_unreal_mruk_scene(shell, shot, out):
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((0.0, 1.0, 0.0), math.radians(10.0))))
    run_frames(shell, 300, chunk=50)
    shot("scene")
    return []


def script_unreal_mruk_random_scene(shell, shot, out):
    # Scenes drawn differently every run (random colours and places, a pattern on the wall
    # clock) have no reference; drawn over passthrough they are mostly lit and show the menu
    script_unreal_mruk_scene(shell, shot, out)
    path = os.path.join(out, "scene.png")
    width, height, pixels = cts_composition.read_png(path)
    lit = sum(1 for i in range(0, len(pixels), 4) if pixels[i] + pixels[i + 1] + pixels[i + 2] > 90)
    panel = sum(pixel_at(path, 0.44, 0.47))
    problems = []
    if lit < width * height // 2:
        problems.append(f"only {lit} of {width * height} pixels lit: no passthrough under the scene?")
    if panel < 600:
        problems.append(f"no menu panel at the view's centre (sum {panel})")
    return problems


def script_unreal_mruk_destructible(shell, shot, out):
    # The trigger removes the global mesh segment along the right controller's forward axis, here
    # on the wall right of the menu panel. The scene moves on its own (the wall clock), so the trigger must
    # change clearly more than as many frames without it do. Needs the sample's patch: without it
    # the pawn's input events are never bound (docs/samples.md)
    problems = script_unreal_mruk_random_scene(shell, shot, out)
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
    shell.run("point right 0.85 0.35 left ahead 0.3")
    run_frames(shell, 30, chunk=30)
    shot("aimed")
    run_frames(shell, 40, chunk=40)
    shot("still")
    shell.run("input right trigger/value 1")
    run_frames(shell, 10, chunk=10)
    shell.run("input right trigger/value 0")
    run_frames(shell, 30, chunk=30)
    shot("destroyed")
    aimed     = cts_composition.read_png(os.path.join(out, "aimed.png"))
    by_itself = differing_pixels(aimed, cts_composition.read_png(os.path.join(out, "still.png")))
    triggered = differing_pixels(cts_composition.read_png(os.path.join(out, "still.png")), cts_composition.read_png(os.path.join(out, "destroyed.png")))
    if triggered < by_itself + 500:
        problems.append(f"the trigger changed {triggered} pixels, the scene by itself {by_itself}: no segment removed?")
    return problems


UNREAL_MRUK_DEMO_TAB = (0.466, 0.764)
UNREAL_MRUK_MAPS_TAB = (0.566, 0.764)
UNREAL_MRUK_DEMO_LIST = (0.52, 0.454)
UNREAL_MRUK_MAP_LIST = (0.558, 0.568)
UNREAL_MRUK_LOAD_MAP = (0.516, 0.594)
# The entries of the two drop-down lists, top to bottom, as the menu names them
UNREAL_MRUK_CORE_DEMOS = ["RoomDetails", "Raycast", "RaycastAll", "PositionInRoom", "BestPoseFromRaycast", "ClosestSurfacePosition",
                          "ClosestSeatPose", "KeyWall", "LargestSurface"]
UNREAL_MRUK_MENU_MAPS = ["Demo", "Navmesh", "Guardian", "PTRL", "DistanceMap", "RandomPlacement", "SceneDecorator", "DestructibleMesh",
                         "EnvironmentRaycast", "PassthroughCameraAccess", "KeyboardTracking", "QRCodeTracking"]


def unreal_mruk_click(shell, at):
    # The Interaction SDK casts its controller ray along the aim pose, but hides it while the hand
    # is at the eye, so the hand starts half a metre along the ray. It hovers a widget only some
    # frames after the hand arrives and selects only while hovering; the hand leaves the panel
    # after each click, or the next hover starts later still. The tabs take clicks on their icons.
    shell.run(f"point right {at[0]} {at[1]} left ahead 0.5")
    run_frames(shell, 30, chunk=30)
    shell.run("input right trigger/value 1")
    run_frames(shell, 10, chunk=10)
    shell.run("input right trigger/value 0")
    run_frames(shell, 10, chunk=10)
    shell.run(cts_scenarios.pose("right", (0.3, 1.0, 0.3)))
    run_frames(shell, 20, chunk=20)


def unreal_mruk_list_entry(top, index):
    return (top[0] - 0.07, top[1] + 0.0195 + 0.016 * index)


def script_unreal_mruk_core_demos(shell, shot, out):
    # Every core demo picked from the menu's list; each describes itself on the panel and draws
    # its result into the room
    problems = script_unreal_mruk_menu(shell, shot, out)
    for index, demo in enumerate(UNREAL_MRUK_CORE_DEMOS[1:], 1):
        unreal_mruk_click(shell, UNREAL_MRUK_DEMO_LIST)
        unreal_mruk_click(shell, unreal_mruk_list_entry(UNREAL_MRUK_DEMO_LIST, index))
        shot(demo)
    return problems


def script_unreal_mruk_maps_tab(shell, shot, out):
    # The Maps tab loads the Guardian map, whose own panel replaces the core demos
    problems = script_unreal_mruk_menu(shell, shot, out)
    unreal_mruk_click(shell, UNREAL_MRUK_MAPS_TAB)
    shot("maps")
    unreal_mruk_click(shell, UNREAL_MRUK_MAP_LIST)
    unreal_mruk_click(shell, unreal_mruk_list_entry(UNREAL_MRUK_MAP_LIST, UNREAL_MRUK_MENU_MAPS.index("Guardian")))
    unreal_mruk_click(shell, UNREAL_MRUK_LOAD_MAP)
    run_frames(shell, 300, chunk=50)
    shot("guardian")
    return problems


TESTS["unreal_mruk_menu"] = {
    "app": "unreal_mruk", "unreal": "project/MRUtilityKitSample.uproject", "room": "living_room", "config": {"views": GODOT_VIEWS},
    "instance": "MRUtilityKitSample", "ready": unreal_mruk_dressed, "needs_out": True, "start_alone": True,
    # The first run after a build compiles the project's shaders for minutes before the session
    "ready_seconds": 900.0, "timeout": 1200.0,
    "script": script_unreal_mruk_menu, "max_pixels": 50,
}
TESTS["unreal_mruk_core_demos"] = dict(TESTS["unreal_mruk_menu"], script=script_unreal_mruk_core_demos)

# A startup script of the editor: starts and stops a VR Preview and quits when the test
# leaves a file of that name in PUPPETXR_EDITOR_COMMANDS; leaves "ready" once it runs
UNREAL_EDITOR_SCRIPT = r"""
import os
import unreal
commands = os.environ["PUPPETXR_EDITOR_COMMANDS"]
def tick(delta):
    for name in ("start", "stop", "quit"):
        path = os.path.join(commands, name)
        if os.path.exists(path):
            os.remove(path)
            if name == "start":
                unreal.SystemLibrary.execute_console_command(None, "puppetxr.VRPreview")
            elif name == "stop":
                unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_request_end_play()
            else:
                unreal.SystemLibrary.quit_editor()
unreal.register_slate_post_tick_callback(tick)
open(os.path.join(commands, "ready"), "w").close()
"""


def unreal_mruk_room_shown(path):
    # The dark ceiling of the dressed room over mostly lit walls and floor; a black frame has the one
    width, height, pixels = cts_composition.read_png(path)
    lit = sum(1 for i in range(0, len(pixels), 4) if pixels[i] + pixels[i + 1] + pixels[i + 2] > 90)
    return unreal_mruk_dressed(path) and lit > width * height // 3


def top_windows(pid, title):
    # The visible top-level windows of a process whose title contains `title`
    user32 = ctypes.windll.user32
    found = []
    def each(hwnd, _):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        text = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, text, 256)
        if owner.value == pid and title in text.value and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    user32.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)(each), 0)
    return found


def bring_to_front(hwnd):
    # As the click on the editor's play button does: Windows lets the process that has the last
    # input take the foreground, which a key press gives the caller
    user32 = ctypes.windll.user32
    user32.keybd_event(0x12, 0, 0, 0)  # VK_MENU down
    user32.keybd_event(0x12, 0, 2, 0)  # and up
    user32.SetForegroundWindow(ctypes.c_void_p(hwnd))
    return user32.GetForegroundWindow() == hwnd


def wait_for(condition, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if condition():
            return True
        time.sleep(0.1)
    return condition()


def script_unreal_vr_preview(shell, shot, out):
    # The editor's VR Preview: puppetxr's window comes with the session in front of the editor and its
    # play window, leaves with it when the preview stops or the window is closed, and the editor
    # quits cleanly. Real time, as a user plays it; the window and its focus are Windows'.
    pid = shell.pid
    user32 = ctypes.windll.user32
    puppetxr = lambda: top_windows(pid, "puppetxr")
    probe = os.path.join(out, "probe.png")
    problems = []
    if not wait_for(lambda: os.path.exists(os.path.join(out, "ready")), 900):
        return ["the editor never ran its command script"]
    # Stopped in the editor, closed through puppetxr's window, and once more: a closed window used to
    # end the next preview at its first frame
    for round_ in range(3):
        editor = top_windows(pid, "Unreal Editor")
        if not editor or not wait_for(lambda: bring_to_front(editor[0]), 10):
            problems.append(f"preview {round_}: the editor did not come to the front")
        open(os.path.join(out, "start"), "w").close()
        if not wait_for(lambda: puppetxr(), 120):
            problems.append(f"preview {round_}: no puppetxr window")
            return problems
        if not wait_for(lambda: user32.GetForegroundWindow() in puppetxr(), 10):
            problems.append(f"preview {round_}: the puppetxr window is not in front")
        # The preview runs once the start level draws its dressed room into the window
        if not wait_for(lambda: shell.run(f"screenshot {probe}") is not None and unreal_mruk_room_shown(probe), 300):
            problems.append(f"preview {round_}: the room never showed")
        shot(f"preview_{round_}")
        if round_ != 1:
            open(os.path.join(out, "stop"), "w").close()
            how = "the preview stopped"
        else:
            # As a user closing the window: puppetxr requests the exit and Unreal ends the preview
            user32.PostMessageW(ctypes.c_void_p(puppetxr()[0]), 0x0010, 0, 0) # WM_CLOSE
            how = "the window was closed"
        if not wait_for(lambda: not puppetxr(), 60):
            problems.append(f"preview {round_}: the puppetxr window stayed after {how}")
    open(os.path.join(out, "quit"), "w").close()
    log = os.path.join(out, "editor.log")
    read = lambda: open(log, errors="replace").read() if os.path.exists(log) else ""
    if not wait_for(lambda: "Log file closed" in read(), 120):
        problems.append("the editor did not quit")
    # Unreal's XR_ENSURE logs every failed OpenXR call it checks
    problems += [f"failed in the editor: {line.strip()}" for line in read().splitlines() if "XRES_Result" in line and "Ensure condition failed" in line]
    return problems

TESTS["unreal_mruk_maps_tab"] = dict(TESTS["unreal_mruk_menu"], script=script_unreal_mruk_maps_tab)
# The editor with puppetxr as a user runs it: a window, real time, not paused; no frame before the
# first preview, so no `ready` screenshot
TESTS["unreal_mruk_vr_preview"] = dict({k: v for k, v in TESTS["unreal_mruk_menu"].items() if k != "ready"}, editor=True, quits_itself=True, compare=False, script=script_unreal_vr_preview,
                                       env={"PUPPETXR_HEADLESS": "0", "PUPPETXR_FIXED_STEP": "0", "PUPPETXR_CONTROL_PAUSED": "0"})

# Each demo map of the sample, opened from the start level as its menu does: that level starts the
# persistent passthrough that DistanceMap, Navmesh, RandomPlacement and SceneDecoration keep using
# (opened on its own such a map draws over no passthrough, so black)
UNREAL_MRUK_MAPS = ["SampleDestructibleMesh/DestructibleMesh", "SampleDistanceMap/DistanceMap", "SampleEnvironmentRaycast/EnvironmentRaycast",
                    "SampleGuardian/Guardian", "SampleKeyboardTracker/KeyboardTracker", "SampleNavmesh/Navmesh",
                    "SamplePassthroughCameraAccess/PassthroughCameraAccess", "SamplePTRL/PTRL", "SampleQRCodeTracking/QRCodeTracker",
                    "SampleRandomPlacement/RandomPlacement", "SampleSceneDecoration/SceneDecoration"]
UNREAL_MRUK_RANDOM = ["DestructibleMesh", "Navmesh", "RandomPlacement"]
# The trackers' maps get what they track, as the Unity scenes do
UNREAL_MRUK_PROPS = {"KeyboardTracker": '[{"kind": "keyboard", "position": [0, 1.3, -0.6], "orientation": [0.4226183, 0, 0, 0.9063078]}]',
                     "QRCodeTracker": '[{"kind": "qr", "text": "https://github.com/oculus-samples", "size": 0.15, "position": [0.25, 1.45, -0.7]}]'}
for unreal_map in UNREAL_MRUK_MAPS:
    map_name     = unreal_map.split("/")[1]
    random_scene = map_name in UNREAL_MRUK_RANDOM
    scene_script = script_unreal_mruk_destructible if map_name == "DestructibleMesh" else script_unreal_mruk_random_scene if random_scene else script_unreal_mruk_scene
    TESTS["unreal_mruk_" + map_name.lower()] = dict(TESTS["unreal_mruk_menu"], args=[f"-ExecCmds=open /Game/{unreal_map}"],
                                                    script=scene_script,
                                                    compare=not random_scene,
                                                    env={"PUPPETXR_PROPS": UNREAL_MRUK_PROPS[map_name]} if map_name in UNREAL_MRUK_PROPS else {})


# Meta's Passthrough sample for Unreal: a level per feature of XR_FB_passthrough, each opened directly
def unreal_lit(path):
    width, height, pixels = cts_composition.read_png(path)
    return sum(1 for i in range(0, len(pixels), 4) if pixels[i] + pixels[i + 1] + pixels[i + 2] > 90) > width * height // 4


def script_unreal_passthrough_level(shell, shot, out):
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((0.0, 1.0, 0.0), math.radians(10.0))))
    run_frames(shell, 300, chunk=50)
    shot("level")
    return []


def script_unreal_occlusion_modes(shell, shot, out):
    # A cycles the Depth API's modes: none, hard, soft
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((0.0, 1.0, 0.0), math.radians(10.0))))
    run_frames(shell, 200, chunk=50)
    shot("mode0")
    for mode in (1, 2, 3):
        press(shell, "right", "a/click")
        run_frames(shell, 60, chunk=60)
        shot(f"mode{mode}")
    return []


def script_unreal_occlusion_hands(shell, shot, out):
    # Tracked hands in front of the head; a pinch toggles their removal from the environment depth
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((0.0, 1.0, 0.0), math.radians(10.0))))
    shell.run("hand left hand")
    shell.run("hand right hand")
    shell.run("reset hands")
    run_frames(shell, 200, chunk=50)
    shot("hands")
    shell.run("input right trigger/value 1")
    run_frames(shell, 10, chunk=10)
    shell.run("input right trigger/value 0")
    run_frames(shell, 60, chunk=60)
    shot("hands_toggled")
    return []


TESTS["unreal_occlusion_modes"] = {
    "app": "unreal_occlusion", "unreal": "project/OcclusionSample.uproject", "room": "living_room", "config": {"views": GODOT_VIEWS},
    # OVRPlugin names its session instance after the project or after the engine, either way round
    "instance": ("OcclusionSample", "Unreal Engine"), "ready": unreal_lit, "needs_out": True, "start_alone": True,
    "ready_seconds": 900.0, "timeout": 1200.0, "args": ["-FeatureLevelES31"], "script": script_unreal_occlusion_modes, "max_pixels": 50,
}
TESTS["unreal_occlusion_hands"] = dict(TESTS["unreal_occlusion_modes"], script=script_unreal_occlusion_hands)
# Meta's plugin darkens its virtual objects to near black (docs/samples.md), so the modes are run
# and not compared yet
TESTS["unreal_occlusion_modes"]["compare"] = False
TESTS["unreal_occlusion_hands"]["compare"] = False
TESTS["unreal_occlusion_explore"] = dict(TESTS["unreal_occlusion_modes"], script=script_look_around)

# Meta's Shared Anchors sample for Unreal: a lobby with a menu on the right controller
TESTS["unreal_shared_anchors_lobby"] = {
    "app": "unreal_shared_anchors", "unreal": "project/SharedAnchorsSample.uproject", "room": "living_room", "config": {"views": GODOT_VIEWS},
    "instance": "SharedAnchorsSample", "ready": unreal_lit, "needs_out": True, "start_alone": True,
    # The controller's ray and an on-screen message land a few pixels apart from run to run
    "ready_seconds": 900.0, "timeout": 1200.0, "args": ["-FeatureLevelES31"], "script": script_look_around, "max_pixels": 4000,
}

def unreal_anchors_menu_in_view(shell):
    # The menu hangs off the right controller; the hand held up before the eyes, turned to them
    shell.run(cts_scenarios.pose("right", (0.05, 1.45, -0.35), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(40.0))))
    run_frames(shell, 30, chunk=30)


def unreal_anchors_pick(shell):
    shell.run("input right trigger/value 1")
    run_frames(shell, 10, chunk=10)
    shell.run("input right trigger/value 0")
    run_frames(shell, 10, chunk=10)


def script_unreal_anchors_session(shell, shot, out):
    # The thumbstick moves the selection, the trigger picks: Create Session opens the shared scene,
    # whose menu starts at Create Anchor
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
    run_frames(shell, 100, chunk=50)
    unreal_anchors_menu_in_view(shell)
    shot("lobby")
    unreal_anchors_pick(shell)
    run_frames(shell, 300, chunk=50)
    unreal_anchors_menu_in_view(shell)
    shot("session")
    unreal_anchors_pick(shell)
    run_frames(shell, 100, chunk=50)
    shot("anchor")
    # The anchor stays where it was made when the hand leaves
    shell.run(cts_scenarios.pose("right", (0.35, 1.2, -0.3)))
    run_frames(shell, 60, chunk=60)
    shot("placed")
    return []


# Meta's Interaction SDK sample for Unreal: a level per kind of interaction, each opened directly
# TransformerExamples is left out: the plugin's hand grab pose asks for the editor's world in -game and crashes
TESTS["unreal_shared_anchors_session"] = dict(TESTS["unreal_shared_anchors_lobby"], script=script_unreal_anchors_session)


# Sharing with users between two players of one Unreal session. Unreal's sessions and replication
# time out in wall-clock time, so both run freely: the script waits on frame counts (wait_frames)
# and on the logs, and the API trace decides
def unreal_free_frames(shell, count):
    shell.run(f"wait_frames {count}")


def unreal_free_input(shell, line, off, frames=10):
    shell.run(line)
    unreal_free_frames(shell, frames)
    shell.run(off)
    unreal_free_frames(shell, frames)


def unreal_free_menu_in_view(shell):
    shell.run(cts_scenarios.pose("right", (0.05, 1.45, -0.35), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(40.0))))
    unreal_free_frames(shell, 30)


def wait_log(path, pattern, limit):
    deadline = time.time() + limit
    while time.time() < deadline:
        if os.path.exists(path) and re.search(pattern, open(path, errors="replace").read()):
            return True
        time.sleep(0.5)
    return False


def script_unreal_anchors_sharing(shell, shot, out, peer):
    # The host (user 1) creates a session, the peer (user 2) finds and joins it; the host places,
    # selects, saves and shares an anchor with the session's players (xrShareSpacesFB with their
    # space users), the peer queries it from the shared cloud and shows it. The sample takes its
    # players' ids from the Meta platform, which tests/samples/unreal_shared_anchors/sample_tests.patch
    # replaces with -puppetxr-user-id
    trigger = lambda s: unreal_free_input(s, "input right trigger/value 1", "input right trigger/value 0")
    stick = lambda s: unreal_free_input(s, "input right thumbstick 0 -1", "input right thumbstick 0 0")
    button_a = lambda s: unreal_free_input(s, "input right a/click on", "input right a/click off")
    game_log, host_log, peer_log = (os.path.join(out, name) for name in ("game.log", "puppetxr.log", "peer_puppetxr.log"))
    for s in (shell, peer.shell):
        s.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
        s.run("resume")
    for s in (shell, peer.shell):
        unreal_free_frames(s, 100)
        unreal_free_menu_in_view(s)
    trigger(shell)
    if not wait_log(game_log, r"listening on port 7777", 60):
        return ["the host opened no session"]
    unreal_free_frames(shell, 200)
    stick(peer.shell)
    unreal_free_menu_in_view(peer.shell)
    trigger(peer.shell)
    if not wait_log(game_log, r"Join succeeded", 120):
        return ["the peer never joined the host's session"]
    # The peer's player state sends its user id to the host 2 s after it begins play
    unreal_free_frames(peer.shell, 600)
    # A joining player makes the host share its anchors with it, which keeps the menu busy for a
    # while too: a trigger it ignored is pressed again
    unreal_free_menu_in_view(shell)
    for _ in range(3):
        time.sleep(4)
        trigger(shell)
        if wait_log(host_log, r"xrCreateSpatialAnchorFB\(", 5):
            break
    else:
        return ["the host created no anchor"]
    # The menu takes no input while it shows the result of its last action, about 3 s of wall
    # time, which free-running frames may pass much faster
    time.sleep(4)
    # Select Anchor: the right controller's forward axis on the anchor, the trigger selects it and
    # opens its menu; A saves it, then Share Anchors(s) is the second entry
    stick(shell)
    unreal_free_menu_in_view(shell)
    shell.run("point right 0.625 0.506 left grip 0 ahead 0.3")
    unreal_free_frames(shell, 30)
    trigger(shell)
    unreal_free_frames(shell, 30)
    shot("selected")
    button_a(shell)
    if not wait_log(host_log, r"xrSaveSpacesMETA\(.*-> XR_SUCCESS", 30):
        return ["the host did not save the anchor"]
    time.sleep(4)
    stick(shell)
    button_a(shell)
    if not wait_log(host_log, r"xrShareSpacesFB\(", 30):
        return ["the host did not share the anchor"]
    problems = []
    trace = open(host_log, errors="replace").read()
    if not re.search(r"xrCreateSpaceUserFB\(.*userId=2\}\) -> XR_SUCCESS", trace):
        problems.append("the host shared with no space user of the peer (user 2)")
    if not re.search(r"xrShareSpacesFB\(.*-> XR_SUCCESS", trace):
        problems.append("the share failed")
    saved = re.search(r"uuid=([0-9a-f-]{36})", trace)
    if not saved or not wait_log(peer_log, rf"xrRetrieveSpaceQueryResultsFB\(.*uuid={saved.group(1)}", 60):
        problems.append("the peer did not load the host's anchor")
    unreal_free_frames(peer.shell, 120)
    peer.shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
    peer.shell.run(cts_scenarios.pose("right", (0.3, 1.0, 0.3)))
    unreal_free_frames(peer.shell, 30)
    shot("shared")
    peer.shot("loaded")
    return problems


TESTS["unreal_shared_anchors_sharing"] = dict(TESTS["unreal_shared_anchors_lobby"], script=script_unreal_anchors_sharing, peer=True, compare=False,
                                              env={"PUPPETXR_TRACE": "xrCreateSpatialAnchorFB,xrSaveSpacesMETA,xrCreateSpaceUserFB,xrShareSpacesFB,xrQuerySpacesFB,xrRetrieveSpaceQueryResultsFB"})
UNREAL_ISDK_LEVELS = ["StartLevel", "PokeExamples", "RayExamples", "DistanceGrabExamples", "ThrowExamples", "GazeExamples", "HandPoseStudio"]
for level in UNREAL_ISDK_LEVELS:
    TESTS["unreal_isdk_" + level.lower()] = {
        "app": "unreal_isdk", "unreal": "project/MetaIsdkSample.uproject", "room": "living_room", "config": {"views": GODOT_VIEWS},
        "instance": "MetaIsdkSample", "ready": lambda path: True, "needs_out": True, "start_alone": True,
        "ready_seconds": 900.0, "timeout": 1200.0, "args": [f"-ExecCmds=open /OculusInteractionSamples/Levels/{level}", "-FeatureLevelES31", "-dpcvars=r.Tonemapper.GrainQuantization=0,r.Mobile.TonemapperFilm=0"],
        # The desktop renderer's lighting changes from run to run; the mobile one fails an assert in a
        # post-process material pass at exit, after the shots
        "exit_codes": (3,),
        # Settled, runs differ by up to 3 levels in the fog, and a few small animated widgets (a
        # torch, a card's icon) follow the wall clock
        "script": script_settled_look_around, "tolerance": 3, "max_pixels": 2000,
    }

def red_pixels(path, x0, y0, x1, y1):
    width, height, pixels = cts_composition.read_png(path)
    count = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * width + x) * 4
            count += pixels[i] > 150 and pixels[i + 1] < 70 and pixels[i + 2] < 70
    return count


def script_unreal_isdk_poke(shell, shot, out):
    # The controller pushed along the eye ray onto the big red button presses it (the Interaction
    # SDK's controller poke) pushes its cap down into the base, from where it springs back. The
    # press leaves nothing else: the label under it reads "Large Button" throughout, which the
    # mobile renderer draws at this size as a few dots.
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((1.0, 0.0, 0.0), math.radians(-50.0))))
    run_frames(shell, 200, chunk=50)
    shot("before")
    for i in range(41):
        shell.run(f"point right 0.35 0.53 left ahead {0.3 + 0.02 * i:.2f}")
        run_frames(shell, 2, chunk=2)
    shot("pressed")
    for i in range(40, -1, -2):
        shell.run(f"point right 0.35 0.53 left ahead {0.3 + 0.02 * i:.2f}")
        run_frames(shell, 2, chunk=2)
    shell.run(cts_scenarios.pose("right", (0.3, 1.0, 0.3)))
    run_frames(shell, 30, chunk=30)
    shot("released")
    cap = [red_pixels(os.path.join(out, f"{label}.png"), 180, 300, 270, 380) for label in ("before", "pressed", "released")]
    problems = [] if cap[1] * 4 < cap[0] else [f"pressed: the button's cap is not pushed in (red pixels {cap[0]} before, {cap[1]} pressed)"]
    return problems + ([] if cap[2] * 5 > cap[0] * 4 else [f"released: the button's cap did not come back (red pixels {cap[0]} before, {cap[2]} after)"])


# The cap's red pixels decide; the shots come before the fog settled
TESTS["unreal_isdk_poke"] = dict(TESTS["unreal_isdk_pokeexamples"], script=script_unreal_isdk_poke, compare=False)
# Meta's Unreal Movement sample (cmake/samples.cmake): the mirrored character with puppetxr's upper
# body, and Aura's face with puppetxr's face, through OculusXR's body and face tracking. On the desktop
# forward renderer its settings ask for; the engine prints its shader progress into the frame
# unless screen messages are off
TESTS["unreal_movement_body"] = {
    "app": "unreal_movement", "unreal": "project/MovementSample.uproject", "room": "none", "config": {"views": GODOT_VIEWS},
    "instance": "Unreal Engine", "ready": lambda path: True, "needs_out": True, "start_alone": True,
    "ready_seconds": 900.0, "timeout": 1200.0, "args": ["-ExecCmds=DisableAllScreenMessages, open /Game/Maps/MAP_HighFidelity"],
    "script": script_movement_body, "max_pixels": 2000,
}
TESTS["unreal_movement_face"] = dict(TESTS["unreal_movement_body"], script=script_movement_face, args=["-ExecCmds=DisableAllScreenMessages, open /Game/Maps/MAP_Aura"])


def unreal_passthrough_look(shell):
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), cts_scenarios.quat_axis_angle((0.0, 1.0, 0.0), math.radians(10.0))))
    run_frames(shell, 300, chunk=50)


def script_unreal_passthrough_startup(shell, shot, out):
    # The left menu button opens the navigation menu: passthrough support, the level, the levels
    unreal_passthrough_look(shell)
    shot("level")
    press(shell, "left", "menu/click")
    run_frames(shell, 60, chunk=60)
    shot("menu")
    return []


def script_unreal_passthrough_resumed(shell, shot, out):
    # Show creates a passthrough layer and fades it in, Hide fades it out and destroys it
    unreal_passthrough_look(shell)
    shot("level")
    unreal_mruk_click(shell, (0.31, 0.50))
    run_frames(shell, 200, chunk=50)
    shot("shown")
    unreal_mruk_click(shell, (0.62, 0.50))
    run_frames(shell, 200, chunk=50)
    shot("hidden")
    return []


def script_unreal_passthrough_styles(shell, shot, out):
    # The widget's colour map curves (a colour curve, grayscale, none) by the buttons under them,
    # then the edge colour's alpha slider at its end, each clicked with the controller ray
    unreal_passthrough_look(shell)
    shot("level")
    for name, at in (("curve_colour", (0.503, 0.343)), ("curve_gray", (0.588, 0.343)), ("curve_none", (0.677, 0.339))):
        unreal_mruk_click(shell, at)
        run_frames(shell, 30, chunk=30)
        shot(name)
    # Sliders are dragged: the trigger held on the edge alpha's thumb, the ray moved to its end
    shell.run("point right 0.558 0.524 left ahead 0.5")
    run_frames(shell, 30, chunk=30)
    shell.run("input right trigger/value 1")
    for i in range(1, 15):
        shell.run(f"point right {0.558 + 0.0125 * i:.4f} 0.524 left ahead 0.5")
        run_frames(shell, 3, chunk=3)
    shell.run("input right trigger/value 0")
    shell.run(cts_scenarios.pose("right", (0.3, 1.0, 0.3)))
    run_frames(shell, 30, chunk=30)
    shot("edges")
    return []


def script_unreal_passthrough_brush(shell, shot, out):
    # The right trigger held while the controller sweeps paints a stroke that shows passthrough;
    # the stroke must change the image
    shell.run(cts_scenarios.pose("head", (0.0, 1.6, 0.0), (0.0, 0.0, 0.0, 1.0)))
    run_frames(shell, 300, chunk=50)
    shot("level")
    shell.run("input right trigger/value 1")
    for i in range(30):
        shell.run(cts_scenarios.pose("right", (0.1 + 0.01 * i, 1.4 + 0.005 * i, -0.4), (0.0, 0.0, 0.0, 1.0)))
        run_frames(shell, 3, chunk=3)
    shell.run("input right trigger/value 0")
    run_frames(shell, 30, chunk=30)
    shot("stroke")
    # Where the stroke lands, below the moving sky
    width, height, before = cts_composition.read_png(os.path.join(out, "level.png"))
    _, _, after = cts_composition.read_png(os.path.join(out, "stroke.png"))
    changed = sum(1 for y in range(int(height * 0.45), int(height * 0.8)) for x in range(int(width * 0.6), width)
                  if before[(y * width + x) * 4:(y * width + x) * 4 + 3] != after[(y * width + x) * 4:(y * width + x) * 4 + 3])
    return [] if changed > 2000 else [f"the stroke changed only {changed} pixels where it is painted"]


def script_unreal_passthrough_masked(shell, shot, out):
    # A circle on the right controller shows passthrough through the virtual room; A moves it to the left
    unreal_passthrough_look(shell)
    shell.run(cts_scenarios.pose("right", (0.15, 1.45, -0.45)))
    shell.run(cts_scenarios.pose("left", (-0.4, 1.0, 0.3)))
    run_frames(shell, 30, chunk=30)
    shot("right")
    press(shell, "right", "a/click")
    shell.run(cts_scenarios.pose("right", (0.4, 1.0, 0.3)))
    shell.run(cts_scenarios.pose("left", (-0.15, 1.45, -0.45)))
    run_frames(shell, 30, chunk=30)
    shot("left")
    return []


UNREAL_PASSTHROUGH_MAPS = ["PTStartup", "PTBackground", "PTStyles", "PTColorMap", "PTColorLut", "PTColorScaleAndOffset", "PTMasked", "PTMaskedBrush", "PTLayerResumedEvent"]
for level in UNREAL_PASSTHROUGH_MAPS:
    TESTS["unreal_passthrough_" + level[2:].lower()] = {
        "app": "unreal_passthrough", "unreal": "project/PTSamples.uproject", "room": "living_room", "config": {"views": GODOT_VIEWS},
        "instance": "PTSamples", "ready": unreal_lit, "needs_out": True, "start_alone": True,
        "ready_seconds": 900.0, "timeout": 1200.0, "args": [f"-ExecCmds=open /Game/Passthrough/Maps/{level}", "-FeatureLevelES31"],
        "script": script_unreal_passthrough_level, "max_pixels": 50,
    }
# Its passthrough waits for the Show button, so the level starts black
TESTS["unreal_passthrough_layerresumedevent"]["ready"] = lambda path: True
TESTS["unreal_passthrough_layerresumedevent"]["script"] = script_unreal_passthrough_resumed
TESTS["unreal_passthrough_startup"]["script"] = script_unreal_passthrough_startup
TESTS["unreal_passthrough_styles"]["script"] = script_unreal_passthrough_styles
TESTS["unreal_passthrough_masked"]["script"] = script_unreal_passthrough_masked
TESTS["unreal_passthrough_maskedbrush"]["script"] = script_unreal_passthrough_brush
# Meta's plugin destroys the color LUTs at exit after the HMD went, through OVRPlugin's wrapper,
# which a native OpenXR project never loaded: a null call and exit code 3 after the test
TESTS["unreal_passthrough_colorlut"]["exit_codes"] = (3,)

# Each Unreal sample on every RHI its plugin presents with (OculusXR: D3D11, D3D12, Vulkan; no
# OpenGL) and on both renderers: mobile (-FeatureLevelES31, what Quest runs) and desktop. The
# tests above run the project's default, D3D11, on the renderer it ships for.
UNREAL_RHIS = ("d3d11", "d3d12", "vulkan")
# Each base test with the RHI its project starts on: DefaultGraphicsRHI, else Unreal's D3D12
UNREAL_RHI_TESTS = {"unreal_mruk_menu": "d3d11", "unreal_occlusion_explore": "d3d11", "unreal_shared_anchors_session": "d3d12",
                    "unreal_isdk_poke": "d3d12", "unreal_passthrough_startup": "d3d12", "unreal_passthrough_styles": "d3d12"}
for base, base_rhi in UNREAL_RHI_TESTS.items():
    base_args = TESTS[base].get("args") or []
    base_renderer = "mobile" if "-FeatureLevelES31" in base_args else "desktop"
    for rhi in UNREAL_RHIS:
        for renderer in ("mobile", "desktop"):
            if rhi == base_rhi and renderer == base_renderer:
                continue
            args = [a for a in base_args if a != "-FeatureLevelES31"] + [f"-{rhi}"] + (["-FeatureLevelES31"] if renderer == "mobile" else [])
            # A cold derived data cache compiles every shader of the level for this RHI and renderer first
            TESTS[f"{base}_{rhi}_{renderer}"] = dict(TESTS[base], args=args, ready_seconds=3600.0, timeout=4200.0)

def start_xvfb():
    # The display number comes back on a pipe once the server listens
    read_end, write_end = os.pipe()
    proc = subprocess.Popen(["Xvfb", "-displayfd", str(write_end), "-screen", "0", "1280x720x24", "-nolisten", "tcp"],
                            pass_fds=(write_end,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(write_end)
    with os.fdopen(read_end) as pipe:
        number = pipe.readline().strip()
    if not number:
        proc.kill()
        return None, None
    return proc, f":{number}"


def app_command(test, opts, out, env, prefix=""):
    if "unreal" in test and test.get("editor"):
        project = os.path.join(opts.samples, test["app"], test["unreal"])
        config = os.path.join(out, "puppetxr.json")
        with open(config, "w") as f:
            json.dump(test["config"], f)
        script = os.path.join(out, "editor_commands.py")
        with open(script, "w") as f:
            f.write(UNREAL_EDITOR_SCRIPT)
        env.update({"PUPPETXR_CONFIG": config, "PUPPETXR_EDITOR_COMMANDS": out})
        # A startup script stays with the editor; -ExecutePythonScript quits it after the script
        return [opts.unreal, project] + ([test["map"]] if "map" in test else []) + ["-EnablePlugins=PythonScriptPlugin", f"-ini:Engine:[/Script/PythonScriptPlugin.PythonScriptPluginSettings]:+StartupScripts={script}",
                "-nosound", "-unattended", "-nosplash",
                f"-abslog={out}/{prefix}editor.log"], os.path.dirname(project)
    if "unreal" in test:
        project = os.path.join(opts.samples, test["app"], test["unreal"])
        config = os.path.join(out, "puppetxr.json")
        with open(config, "w") as f:
            json.dump(test["config"], f)
        env["PUPPETXR_CONFIG"] = config
        # Temporal anti-aliasing jitter and the tonemapper's dither to 8 bits change every frame.
        # Unreal reads the first -dpcvars only, so a test's own cvars join these
        cvars = ["r.AntiAliasingMethod=0", "r.MSAACount=1", "r.BackbufferQuantizationDitheringOverride=24", "r.FilmGrain=0"]
        # tests/samples/unreal_shared_anchors/sample_tests.patch: the platform's user id is the simulated user
        args  = [f"-puppetxr-user-id={env.get('PUPPETXR_USER_ID', '1')}"] if test.get("peer") else []
        for arg in test.get("args", []):
            if arg.startswith("-dpcvars="):
                cvars += arg[len("-dpcvars="):].split(",")
            else:
                args.append(arg)
        return [opts.unreal, project] + ([test["map"]] if "map" in test else []) + args + ["-game", "-vr", "-windowed", "-ResX=320", "-ResY=240", "-nosound", "-unattended", "-nosplash",
                "-deterministic", "-FPS=90", f"-abslog={out}/{prefix}game.log",
                # Compiling asynchronously, Unreal draws a default material until a material's shaders
                # are done: the first run of a cold cache showed menus as grey grids
                "-ini:Engine:[DevOptions.Shaders]:bAllowAsynchronousShaderCompiling=False",
                "-dpcvars=" + ",".join(cvars)], os.path.dirname(project)
    if "project" not in test:
        exe = os.path.join(opts.samples, test["app"], test["exe"])
        if test.get("display") and opts.display:
            env["DISPLAY"] = opts.display
        if "config" in test:
            config = os.path.join(out, "puppetxr.json")
            with open(config, "w") as f:
                json.dump(test["config"], f)
            env["PUPPETXR_CONFIG"] = config
        return [exe] + [arg.replace("{out}", out).replace("{prefix}", prefix) for arg in test.get("args", [])], os.path.dirname(exe)
    config = os.path.join(out, "puppetxr.json")
    with open(config, "w") as f:
        json.dump({"views": GODOT_VIEWS}, f)
    # Godot's shader cache lives in the build tree, not the user's home
    userdata = os.path.join(opts.samples, "godot", "userdata")
    env["PUPPETXR_CONFIG"] = config
    env.pop("EGL_PLATFORM", None)
    if WINDOWS:
        # Mesa's opengl32.dll next to godot.exe is llvmpipe; lavapipe is an ICD of its own
        env.update({"APPDATA": userdata, "GALLIUM_DRIVER": "llvmpipe", "VK_DRIVER_FILES": os.path.join(opts.samples, "mesa", "lvp_icd.x86_64.json")})
        godot = os.path.join(opts.samples, "godot", "godot.exe")
    elif MACOS:
        # Godot on macOS keeps its data in ~/Library/Application Support/Godot, whatever XDG says
        godot = os.path.join(opts.samples, "godot", "Godot.app", "Contents", "MacOS", "Godot")
    else:
        env.update({"DISPLAY": opts.display, "XDG_DATA_HOME": userdata})
        godot = os.path.join(opts.samples, "godot", "godot")
    project = os.path.join(opts.samples, test["app"], test["project"])
    return [godot, "--path", project, "--fixed-fps", "90", "--audio-driver", "Dummy"] + GODOT_RENDERERS[test["renderer"]] + test.get("args", []), project


class JobLimits(ctypes.Structure):
    # JOBOBJECT_EXTENDED_LIMIT_INFORMATION, of which only LimitFlags is set
    _fields_ = [("per_process_user_time", ctypes.c_int64), ("per_job_user_time", ctypes.c_int64), ("limit_flags", ctypes.c_uint32),
                ("minimum_working_set", ctypes.c_size_t), ("maximum_working_set", ctypes.c_size_t), ("active_process_limit", ctypes.c_uint32),
                ("affinity", ctypes.c_size_t), ("priority_class", ctypes.c_uint32), ("scheduling_class", ctypes.c_uint32),
                ("io_counters", ctypes.c_uint64 * 6), ("process_memory_limit", ctypes.c_size_t), ("job_memory_limit", ctypes.c_size_t),
                ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t)]


def kill_children_at_end(proc):
    # Meta's Unity plugin starts an adb server, which inherits the player's handles and would keep
    # its logs open after the test; on Windows the app runs in a job that ends with the test
    if not WINDOWS:
        return None
    kernel32 = ctypes.windll.kernel32
    kernel32.CreateJobObjectW.restype = ctypes.c_void_p
    job = kernel32.CreateJobObjectW(None, None)
    limits = JobLimits(limit_flags=0x2000)  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    kernel32.SetInformationJobObject(ctypes.c_void_p(job), 9, ctypes.byref(limits), ctypes.sizeof(limits))  # JobObjectExtendedLimitInformation
    kernel32.AssignProcessToJobObject(ctypes.c_void_p(job), ctypes.c_void_p(int(proc._handle)))
    return ctypes.c_void_p(job)


# OVRPlugin of players starting together crashed in its JSON reader (an uncaught exception, exit
# code 0xC0000409) while its telemetry started, so tests marked `start_alone` start one at a time,
# each once the previous one's own instance is up
START_ALONE = threading.Lock()


EXIT_SECONDS  = 60
START_SECONDS = 90
# A ready app answers every command within this; past it the app is hung and gets killed
HANG_SECONDS  = 120
# Lines of an app or engine log after which it never draws again: the test ends at once
FATAL_MARKERS = ("VK_ERROR_DEVICE_LOST", "DXGI_ERROR_DEVICE_REMOVED", "DXGI_ERROR_DEVICE_HUNG", "DXGI_ERROR_DEVICE_RESET", "Fatal error!")


def fatal_lines(out, prefix):
    # The ends of the logs only: a broken app may write hundreds of megabytes
    lines = []
    for name in (f"{prefix}app.log", "game.log" if not prefix else None):
        if not name:
            continue
        try:
            with open(os.path.join(out, name), "rb") as f:
                f.seek(0, os.SEEK_END)
                f.seek(max(0, f.tell() - 262144))
                tail = f.read().decode("utf-8", "replace")
        except OSError:
            continue
        # Unreal writes the failing call two lines before the error
        previous = ["", ""]
        for line in tail.splitlines():
            line = line.strip()
            if any(marker in line for marker in FATAL_MARKERS):
                found = " ".join(previous + [line]).strip()[-300:]
                if found not in lines:
                    lines.append(found)
            elif line:
                previous = [previous[1], line]
    return lines[:3]


def save_stacks(pid, path):
    # A Unity player once hung after its shutdown log; the stacks of every thread show where
    if not WINDOWS:
        return
    kits = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Windows Kits", "10", "Debuggers", "x64", "cdb.exe")
    cdb = shutil.which("cdb") or (kits if os.path.exists(kits) else None)
    if cdb:
        with open(path, "w") as f:
            subprocess.run([cdb, "-pv", "-p", str(pid), "-c", "~*kc 30; qd"], stdout=f, stderr=subprocess.STDOUT, timeout=120, check=False)


def ovrplugin_crash_after_openxr(pid, started, player_log):
    # OVRPlugin's analytics thread sometimes locks a destroyed mutex while a Unity player quits on
    # macOS. That exit is accepted only when OpenXR had shut down, the crashed thread is in
    # OVRPlugin and no frame of the crash report is puppetxr's
    if not MACOS:
        return False
    try:
        with open(player_log, errors="replace") as f:
            if "Report Reason: System Shutdown" not in f.read():
                return False
    except OSError:
        return False
    reports = os.path.expanduser("~/Library/Logs/DiagnosticReports")
    for _ in range(50):
        for name in sorted(os.listdir(reports)) if os.path.isdir(reports) else []:
            path = os.path.join(reports, name)
            if not name.endswith(".ips") or os.path.getmtime(path) < started:
                continue
            with open(path, errors="replace") as f:
                header, _, body = f.read().partition("\n")
            try:
                report = json.loads(body)
            except ValueError:
                continue
            if report.get("pid") != pid:
                continue
            images = [image.get("name", "") for image in report.get("usedImages", [])]
            threads = report.get("threads", [])
            crashed = next((t for t in threads if t.get("triggered")), None)
            frames = [images[f["imageIndex"]] for t in threads for f in t.get("frames", []) if f.get("imageIndex", -1) < len(images)]
            in_ovrplugin = crashed and any(images[f["imageIndex"]] == "OVRPlugin.dylib" for f in crashed.get("frames", []) if f.get("imageIndex", -1) < len(images))
            return bool(in_ovrplugin) and "libpuppetxr.dylib" not in frames
        time.sleep(0.2)
    return False


def discovered(pid, app_name):
    # Engines may create and destroy probe instances before their own; `instance` names the one
    # the test drives, or a tuple of the names it may have
    names = app_name if isinstance(app_name, tuple) else (app_name,)
    try:
        with open(cts_composition.discovery_file(pid)) as f:
            return not app_name or json.load(f).get("app") in names
    except (OSError, ValueError):
        return False


class App:
    """One process of a test: started paused, driven through its own puppetxrctl shell."""

    def __init__(self, test, opts, out, env, prefix):
        self.test, self.opts, self.out, self.prefix = test, opts, out, prefix
        self.shots, self.problems, self.shell = [], [], None
        command, cwd = app_command(test, opts, out, env, prefix)
        self.log = open(os.path.join(out, f"{prefix}app.log"), "w")
        # hello_xr quits when stdin reaches its end, so its stdin stays open until the test ends
        stdin = subprocess.PIPE if test.get("stdin_open") else subprocess.DEVNULL
        # A with block: an app that cannot start must not keep the others waiting for the lock
        with START_ALONE if test.get("start_alone") else contextlib.nullcontext():
            self.started = time.time()
            self.proc = subprocess.Popen(command, cwd=cwd, env=env, stdin=stdin, stdout=self.log, stderr=subprocess.STDOUT)
            self.job = kill_children_at_end(self.proc)
            self.deadline = time.time() + test.get("timeout", opts.timeout)
            started = min(self.deadline, time.time() + START_SECONDS)
            while not discovered(self.proc.pid, test.get("instance")):
                if self.proc.poll() is not None or time.time() > started:
                    break
                time.sleep(0.05)
            if self.proc.poll() is None and not discovered(self.proc.pid, test.get("instance")):
                save_stacks(self.proc.pid, os.path.join(out, f"{prefix}start_stacks.txt"))
                self.problems.append(f"{prefix}no simulator within {START_SECONDS} s (thread stacks in start_stacks.txt when cdb is installed)")
                self.proc.kill()
                self.proc.wait()
        if self.proc.poll() is None and time.time() <= self.deadline:
            # Godot on lavapipe beside other tests can take minutes for a step of 10 frames; Unreal
            # loads for a while without a call to its first instance, whose server answers in them;
            # a cold shader compile keeps it silent for as long as the test may take to get ready
            # and a shell that times out is gone for the rest of the test
            slow = "project" in test or "unreal" in test
            self.shell = cts_scenarios.Shell(opts.puppetxrctl, self.proc.pid, int(max(300.0, test.get("ready_seconds", 0.0)) * 1000) if slow else None)
            self.shell.run("mirror left")

    def shot(self, label):
        path = os.path.join(self.out, f"{self.prefix}{label}.png")
        if self.shell.run(f"screenshot {path}") is None:
            self.problems.append(f"screenshot {self.prefix}{label} failed")
        else:
            self.shots.append(path)

    def finish(self):
        fatal = fatal_lines(self.out, self.prefix)
        self.problems += [f"{self.prefix}log: {line}" for line in fatal]
        if self.shell and (self.shell.hung or fatal) and self.proc.poll() is None:
            # Where every thread waits tells whose the hang is
            save_stacks(self.proc.pid, os.path.join(self.out, f"{self.prefix}hang_stacks.txt"))
            self.problems.append(f"{self.prefix}hung: {self.shell.hung or 'the log says the app cannot go on'} (thread stacks in hang_stacks.txt when cdb is installed)")
            self.shell.hung = self.shell.hung or "killed"
            self.proc.kill()
        elif self.shell:
            self.problems += [f"{self.prefix}refused: {line}" for line in self.shell.rejected]
            if not self.test.get("quits_itself") and self.shell.run("exit") is None:
                self.problems.append(f"{self.prefix}exit refused")
        if self.shell:
            self.shell.close()
        if self.proc.stdin:
            self.proc.stdin.close()
        try:
            self.proc.wait(timeout=max(1.0, min(self.deadline - time.time(), EXIT_SECONDS)))
        except subprocess.TimeoutExpired:
            save_stacks(self.proc.pid, os.path.join(self.out, f"{self.prefix}hang_stacks.txt"))
            self.proc.kill()
            self.proc.wait()
            self.problems.append(f"{self.prefix}no exit within {EXIT_SECONDS} s of `exit` (thread stacks in hang_stacks.txt when cdb is installed)" if time.time() < self.deadline else "timeout")
        if self.job:
            ctypes.windll.kernel32.CloseHandle(self.job)
        self.log.close()
        if self.proc.returncode not in (0, *self.test.get("exit_codes", ())) and "timeout" not in self.problems and not (
                self.test.get("exit_crash_ok") and ovrplugin_crash_after_openxr(self.proc.pid, self.started, os.path.join(self.out, f"{self.prefix}player.log"))):
            self.problems.append(f"{self.prefix}exit code {self.proc.returncode}")


def run_test(name, test, opts):
    out = os.path.join(opts.out, name)
    # A crashed app's crash handler may still hold its files for a moment after the job ended it
    for _ in range(50):
        shutil.rmtree(out, ignore_errors=True)
        if not os.path.exists(out):
            break
        time.sleep(0.1)
    os.makedirs(out)
    # MRUK keeps the world-locking anchor of the last run there on macOS, which changes its log
    if MACOS and test["app"].startswith("unity_mruk"):
        try:
            os.remove(os.path.expanduser("~/Library/Application Support/Meta/MRUKSample/mruk_config.dat"))
        except OSError:
            pass
    env = dict(os.environ)
    env.update({
        "XR_RUNTIME_JSON": opts.runtime, "PUPPETXR_HEADLESS": "1", "PUPPETXR_FIXED_STEP": "1", "PUPPETXR_CONTROL": "1",
        "PUPPETXR_CONTROL_PAUSED": "1", "PUPPETXR_ROOM": test["room"], "PUPPETXR_LOG_FILE": os.path.join(out, "puppetxr.log"),
        "PUPPETXR_ANCHORS_DIR": os.path.join(out, "anchors"), "PUPPETXR_CLOUD_DIR": os.path.join(out, "cloud"),
        "EGL_PLATFORM": "surfaceless", "LP_NUM_THREADS": "0",
        # Surfaceless EGL picks the GPU when there is one; references are from llvmpipe
        "LIBGL_ALWAYS_SOFTWARE": "1",
        # llvmpipe of Mesa 26.2.4 (LLVM 23.1.1) crashes in shaders loaded from its disk cache
        "MESA_SHADER_CACHE_DISABLE": "true",
    })
    env.update(test.get("env", {}))
    apps = [App(test, opts, out, env, "")]
    if test.get("peer"):
        # A second user of the same app on the same simulated cloud: anchors of its own, shared
        # ones through the cloud directory, colocation adverts beside them
        peer_env = dict(env, PUPPETXR_USER_ID="2", PUPPETXR_LOG_FILE=os.path.join(out, "peer_puppetxr.log"), PUPPETXR_ANCHORS_DIR=os.path.join(out, "peer_anchors"))
        apps.append(App(test, opts, out, peer_env, "peer_"))
    main = apps[0]
    if all(app.shell for app in apps):
        if "project" in test:
            # Godot on lavapipe may draw a frame a second while other tests run, so steps stay
            # well inside puppetxrctl's minute per step
            run_frames(main.shell, SETTLE_FRAMES, chunk=10)
            if not settle_still(main.shell, out):
                main.problems.append("the demo never held still")
        for app in apps:
            if "ready" in test and not wait_until(app.shell, out, test["ready"], limit=test.get("ready_seconds", 60.0), abort=lambda app=app: fatal_lines(out, app.prefix)):
                app.problems.append(f"{app.prefix}the app never became ready")
            app.shell.reply_seconds = HANG_SECONDS
        if test.get("peer"):
            main.problems += test["script"](main.shell, main.shot, out, apps[1])
        elif test.get("needs_out"):
            main.problems += test["script"](main.shell, main.shot, out)
        else:
            main.problems += test["script"](main.shell, main.shot)
    for app in apps:
        app.finish()
    return [path for app in apps for path in app.shots], [problem for app in apps for problem in app.problems]


# OVRPlugin's JSON reader crashes at start now and then, reading the log of another player as it
# is written (exit code 0xC0000409; its logs are found through the known folder, so no
# environment can separate them). Tests marked `retry_start_crash` run up to twice more after that.
START_CRASH = f"exit code {0xC0000409}"


def reference_path(opts, name, shot):
    # The Apple GPU draws some scenes differently enough for references of their own, which
    # --update on macOS writes; elsewhere macOS compares against the Linux references
    common = os.path.join(opts.references, TESTS[name]["app"], "references", name, shot)
    if not MACOS:
        return common
    macos = os.path.join(opts.references, TESTS[name]["app"], "references_macos", name, shot)
    return macos if opts.update or os.path.exists(macos) else common


def timed_test(name, opts):
    start = time.time()
    retried = 0
    while True:
        try:
            shots, problems = run_test(name, TESTS[name], opts)
        except Exception as error:  # noqa: BLE001  one broken script fails its test, not the run
            shots, problems = [], [f"driver error: {error!r}"]
        if retried == 2 or shots or not any(p.endswith(START_CRASH) for p in problems) or not TESTS[name].get("retry_start_crash"):
            break
        retried += 1
    if retried:
        print(f"{name}: crashed at start ({START_CRASH}), ran {retried} more times")
    return shots, problems, time.time() - start


# llvmpipe compiles its shaders for the host CPU: the references come from AMD CPUs, and Intel ones
# draw a few pixels of them 2 or 3 steps apart
def cpu_vendor(path):
    try:
        with open(path) as f:
            for line in f:
                key, _, value = line.partition(":")
                if key.strip() == "vendor_id":
                    return value.strip()
    except OSError:
        pass
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--samples", required=True)
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--puppetxrctl", required=True)
    parser.add_argument("--references", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--tests", default="")
    parser.add_argument("--apps", default="", help="only tests of these sample projects, comma separated")
    parser.add_argument("--tolerance", type=int, default=0)
    parser.add_argument("--intel-tolerance", type=int, help="the tolerance on an Intel CPU, if higher")
    parser.add_argument("--cpuinfo", default="/proc/cpuinfo", help="where to read the CPU vendor")
    parser.add_argument("--pixel-budget", type=float, default=0.0, help="percent of a screenshot's pixels that may differ by more than the tolerance")
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--unreal", default="", help="UnrealEditor.exe that runs the Unreal samples")
    opts = parser.parse_args()
    for key in ("samples", "runtime", "puppetxrctl", "references", "out"):
        setattr(opts, key, os.path.abspath(getattr(opts, key)))
    vendor = cpu_vendor(opts.cpuinfo)
    if vendor == "GenuineIntel" and opts.intel_tolerance is not None:
        opts.tolerance = max(opts.tolerance, opts.intel_tolerance)
    print(f"tolerance {opts.tolerance} on {vendor or 'an unknown CPU'}")

    apps = [a for a in opts.apps.split(",") if a]
    names = [n for n in opts.tests.split(",") if n] or [n for n in TESTS if not apps or TESTS[n]["app"] in apps]
    unknown = [n for n in names if n not in TESTS]
    if unknown:
        print(f"unknown tests: {', '.join(unknown)}")
        return 2
    if not opts.unreal:
        names = [n for n in names if "unreal" not in TESTS[n]]
    # Named tests and apps must run; asked for nothing in particular, the build decides what exists
    if not opts.tests and not apps:
        built = [n for n in names if os.path.isdir(os.path.join(opts.samples, TESTS[n]["app"]))]
        missing = sorted({TESTS[n]["app"] for n in names if n not in built})
        if missing:
            print(f"not built, so not run: {', '.join(missing)}")
        if not built:
            print(f"no sample app is built in {opts.samples}")
            return 2
        names = built

    xvfb = None
    opts.display = None
    if not WINDOWS and not MACOS and any("project" in TESTS[n] or TESTS[n].get("display") for n in names):
        xvfb, opts.display = start_xvfb()
        if not xvfb:
            print("Xvfb did not start")
            return 2

    # Each test is a process of its own with a single-threaded llvmpipe, so they run side by side.
    # The longest start first (durations of the last run, unknown ones before all), so the run
    # takes about as long as its longest test rather than ending on a long test started last.
    timings_path = os.path.join(opts.out, "timings.json")
    try:
        with open(timings_path) as f:
            timings = json.load(f)
    except (OSError, ValueError):
        timings = {}
    order = sorted(names, key=lambda name: -timings.get(name, float("inf")))
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, opts.jobs)) as pool:
        futures = {name: pool.submit(timed_test, name, opts) for name in order}
        results = [futures[name].result() for name in names]
    timings.update({name: round(seconds, 1) for name, (_, _, seconds) in zip(names, results)})
    os.makedirs(opts.out, exist_ok=True)
    with open(timings_path, "w") as f:
        json.dump(timings, f, indent=1, sort_keys=True)

    failed = 0
    for name, (shots, problems, seconds) in zip(names, results):
        for shot in shots if TESTS[name].get("compare", True) else []:
            reference = reference_path(opts, name, os.path.basename(shot))
            if opts.update:
                os.makedirs(os.path.dirname(reference), exist_ok=True)
                shutil.copyfile(shot, reference)
                continue
            if not os.path.exists(reference):
                problems.append(f"{os.path.basename(shot)}: no reference")
                continue
            width, height, _ = cts_composition.read_png(shot)
            budget = int(width * height * opts.pixel_budget / 100.0)
            ignore = TESTS[name].get("ignore_macos", []) if MACOS else []
            bad, text = cts_composition.compare(reference, shot, max(opts.tolerance, TESTS[name].get("tolerance", 0)), shot[:-4] + "_diff.png", ignore)
            if bad is None or bad > max(budget, TESTS[name].get("max_pixels", 0)):
                problems.append(f"{os.path.basename(shot)}: {text}")
        print(f"{name}: {'ok' if not problems else 'FAILED'} ({len(shots)} screenshots, {seconds:.0f} s)")
        for problem in problems:
            print(f"    {problem}")
        failed += 1 if problems else 0
    if xvfb:
        xvfb.terminate()
        xvfb.wait()
    print(f"{len(names) - failed} of {len(names)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
