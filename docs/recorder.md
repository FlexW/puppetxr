# Motion recorder

Status: the desktop build with its tests and the Quest APK are built; the APK has not run on a
headset yet (`TODO.md`, Pending verification). `docs/hand-motion.md` (planned) is the system that
plays what it records.

`puppetxr-recorder` is a minimal OpenXR app that captures how a real person's tracked hands
(later their body) move, as a device reports them, into JSON **takes**. Takes are the ground truth
the simulated hand model and the built-in gestures are fitted to, and users turn their own takes
into gestures for agents to play (`docs/hand-motion.md`, Clip steps). It ships with puppetxr as an
APK for Meta Quest and builds on the desktop, where it runs on puppetxr itself for its tests.

## Goals

- Raw OpenXR and Vulkan, no engine: anyone can read it in an hour and port it.
- What an app sees, nothing smoothed: joints located at the predicted display time in the same
  calls an app makes, plus what the runtime derives from them (`XR_FB_hand_tracking_aim`).
- Labelled data: a protocol prompts each take, so a take's name says what it holds.
- One capture path for every tracked part: hands now, body (`XR_FB_body_tracking`,
  `XR_META_body_tracking_full_body`) and later face and eyes as further tracks of the same take.

## Files

| File | What |
|---|---|
| `tools/recorder/recorder.h`, `recorder.cpp` | `rc_create`, `rc_frame`, `rc_destroy`: OpenXR session, Vulkan, capture, take writer, protocol, quad UI; no OS conditionals. Functions come from the `xrGetInstanceProcAddr` it is given, so the tests run it on the runtime in their process |
| `tools/recorder/recorder_desktop.cpp` | `main` of `puppetxr_recorder` through the loader: `--out DIR`, `--protocol FILE`, `--space local_floor\|stage`, `--countdown S`, `--auto`, `--no-passthrough` |
| `tools/recorder/CMakeLists.txt` | `puppetxr_recorder`, built with `PUPPETXR_BUILD_APPS`; configured on its own with the NDK, the shared library of the APK with the loader, volk, jsmn and the NDK's native app glue |
| `tools/recorder/recorder_android.cpp` | `android_main` (NativeActivity, no Java): `xrInitializeLoaderKHR` and `XR_KHR_android_create_instance` with the activity, logs to logcat (tag `puppetxr-recorder`), takes into a directory per session named by its local start time |
| `tools/recorder/AndroidManifest.xml` | `org.puppetxr.recorder`, NativeActivity, `android:hasCode="false"`, permissions and features below |
| `tools/recorder/build_apk.sh` | `build/android-recorder/puppetxr-recorder.apk`: CMake with the NDK (arm64-v8a, API 29, 16 KB pages), `aapt2 link`, `zip`, `zipalign`, `apksigner` with a debug key it makes in `build/android-recorder/`; no Gradle. `--install`, `--run` |
| `tools/recorder/setup_android.sh`, `android_env.sh` | pinned JDK, command-line tools, platform, build-tools, platform-tools, NDK into the git-ignored `tools/recorder/android_sdk/`, checksums checked; `build_apk.sh` runs it when something is missing. `ANDROID_SDK_ROOT` set beforehand uses an SDK that has these versions |
| `tools/recorder/pull.sh [DIR]` | the takes of the attached headset into `DIR` (default `./takes`) |
| `tools/recorder/push_protocol.sh <file>`, `--remove` | a protocol of one's own onto the headset, or back to the built-in one |

Only libc, OpenXR, Vulkan (volk) and jsmn for the protocol; it does not link `platform` or `base`,
which have no Android port.

## Loader

The Khronos loader from `third_party/openxr`, built for Android with `android_utilities.cpp`,
`src/external/jnipp` (MIT) and `src/external/android-jni-wrappers` (BSL-1.0) from the same pinned
release (`third_party/README.md`), packaged as `libopenxr_loader.so`. Nothing is downloaded at
build time; the Maven `openxr_loader_for_android` package is not used.

## Building the APK

```
tools/recorder/build_apk.sh --run       # builds, installs on the attached headset and starts it
tools/recorder/pull.sh takes            # after recording
```

The first build downloads the SDK (a few GB) into `tools/recorder/android_sdk/`. Needs CMake 3.25,
curl, unzip, zip and bash; macOS or Linux. Takes are written under
`/sdcard/Android/data/org.puppetxr.recorder/files/takes/<yyyymmdd_hhmmss>/`; a protocol from
`push_protocol.sh` lives in the app's own storage (`files/protocol.json`), as the app cannot read a
file the shell pushed elsewhere, and is read at the next start.

## Manifest (Quest)

NativeActivity with `android.app.lib_name` = `puppetxr_recorder`, launched into the headset
(`org.khronos.openxr.intent.category.IMMERSIVE_HMD` with `LAUNCHER`, and
`com.oculus.intent.category.VR`), Horizon OS SDK 69; the loader's permissions and `<queries>` for
the runtime broker and services; `com.oculus.permission.HAND_TRACKING` with feature
`oculus.software.handtracking` (required), `com.oculus.feature.PASSTHROUGH` (optional); min SDK 29,
target 32. `com.oculus.permission.BODY_TRACKING` once the body track lands.

## Rendering

Layers, back to front: passthrough (`XR_FB_passthrough`, where the runtime has it; otherwise
nothing, black), the panel, the hand skeleton, the rays.

- The panel: a quad layer of 0.48 x 0.3 m (640 x 400 pixels), placed as UI below says. Its image
  is drawn on the CPU (rectangles and a 5 x 7 bitmap font in `recorder.cpp`) and copied to the
  swapchain image through a staging buffer only when it changed.
- The hand skeleton: a stereo projection layer at the recommended eye size with source alpha,
  over the panel, since a quad layer covers the passthrough hand behind it. A lit box per joint
  (6 mm, white) and per bone (4 mm, left blue, right orange), built in the vertex shader from an
  instance buffer of start, end, radius and colour, with no vertex or index buffer; depth tested,
  a render pass per eye, the view-projection matrix as a push constant. Only where a hand is
  tracked; frames without one submit no projection layer.
- Shaders: `skeleton.vert`, `skeleton.frag`; `compile_shaders.sh` writes their SPIR-V into the
  checked-in `recorder_shaders.h`, so no build needs a shader compiler. The header names the
  `glslangValidator` version that made it; `RecorderShaders.the_checked_in_spirv_matches_the_glsl`
  fails when it is stale and skips under another version.
- The rays: a quad layer each (UI).

## UI

Each tracked hand points with the system's aim ray (`XR_FB_hand_tracking_aim`, the pointer of the
headset's own menus), drawn as a 4 mm quad layer of 70 % white from the hand to the panel (1.5 m
when it misses), turned toward the eyes; where it hits, the panel shows a cursor and the button
under it lights up. The start of an index pinch clicks the button under the ray; a pinch begun
elsewhere clicks nothing. A pinch is the aim state's `XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB` or
the thumb and index tips closer than 1.5 cm (released beyond 3 cm), both tips located: on a Quest 3
the recorder's aim state reported pinch strength 0 and no pinching bit while the tips were 2 to 7 mm
apart. The hand skeleton is drawn in 3D over the panel (Rendering). Rays and cursors hide while a step records, so they stay out of the way of the motion; a free take keeps them, as it ends with a click on `STOP`.

Buttons can also be poked with either index tip (`XR_HAND_JOINT_INDEX_TIP_EXT` between 1 cm in front of
the quad plane and 3 cm behind it, inside the rectangle; the tip must leave to 2 cm in front, or
the panel, before it presses again). The panel is placed 45 cm ahead of the head and 30 cm below
it, facing it upright, at the first located frame and after `RECENTER`.

- Title: protocol step `n/N` and its name; the instruction below, word wrapped.
- Status: `POKE RECORD TO START`, the countdown, `RECORDING <s> S` on a red border, then `SAVED
  <file>, <n> FRAMES` or why writing failed.
- Buttons: `RECORD` (countdown, then records the step's duration and moves to the next step),
  `REDO` (the previous step again, after its countdown), `PREV`, `NEXT`, `FREE TAKE` (no
  protocol, until it is poked again as `STOP`), `RECENTER`.
- A step's duration ends a take, so the hands are free for the motion and no stop gesture lands
  in it. A free take ends with the poke on `STOP`, which it holds.
- Not yet: controllers' aim rays, track toggles (`Body` comes with the body track).

## Protocol

A built-in table in `recorder.cpp` (name, instruction, hands, duration): the calibration takes
and one or more takes per built-in gesture (`docs/hand-motion.md`, Library). `--protocol <file>`
on the desktop and `push_protocol.sh` on the headset replace it, so a user can script their own
session:

```json
{ "steps": [ { "name": "my_wave", "instruction": "Wave twice", "seconds": 5, "hands": "right" } ] }
```

`name` is `[a-z0-9_]`, `seconds` in (0, 600], `hands` `left`, `right` or `both` (default); at most
64 steps. A bad file names the step (`steps[1]: ...`) and the recorder does not start.

Calibration steps (first): `still_open` (both hands open, still, 10 s; jitter), `still_relaxed`,
`curl_each` (each finger alone, slowly, closed and open; joint ranges and coupling), `thumb_sweep`
(the thumb's whole range), `pinch_each` (thumb to each fingertip), `spread` (fingers apart and
together), `fast_swipe` (fast motion; dropouts and lag), `cross` (one hand over the other;
occlusion), `out_of_view` (hands leave the cameras' view and return).

## On a Quest 3

Measured in the full protocol (39 takes, 72 Hz):

- A hand the cameras lose stays `active` 1: its data source turns to `controller`, every joint is
  zero with flags 0, and the aim state still says computed and valid with a pose. Around a loss
  the index tip goes tracked (flags 15) to about 3 frames of position without tracking (flags 3),
  to lost, back to flags 3, to tracked. No controller was awake; `"source": "controller"` frames in
  a take are lost hands.
- The aim state's pinch strengths were 0 and its pinching bits never set, in every frame of every
  take with the thumb and index tips down to 3 mm apart; the same with an instance enabling only
  `XR_EXT_hand_tracking`, `XR_FB_hand_tracking_aim` and `XR_FB_passthrough`, and a lost hand stays
  active with zero joints there too.
- `XR_EXT_hand_interaction` reports what the aim state does not (10 pinch takes, 16148 hand
  frames): `pinch_ext/value` is 1 with the thumb and index tips under 12 mm apart, about 0.5 at 12
  to 20 mm, 0.2 to 0.3 at 20 to 28 mm, under 0.1 at 32 to 40 mm and 0 beyond 44 mm (medians; the
  spread is widest at 12 to 20 mm). A thumb to middle pinch leaves it at 0. `aim_activate_ext/value`
  equals it in every frame. `grasp_ext/value` follows the curl of the middle, ring and little
  fingers, not the pinch: 1 with their tips within 70 mm of the palm joint, about 0.75 at 80 to
  90 mm, about 0.45 with the hand open, so it is high while a person pinches with the others curled.
- The thumb and index tip joints come to 5 to 9 mm of each other in a pinch, not to 0.

## Take format

`<out>/<nnn>_<step>.json`, `nnn` counting the session's takes from 001, written and flushed frame
by frame and closed when the take ends or the recorder is destroyed; a take cut short by a crash
lacks only its closing `]}`. Numbers: positions in metres with 5 decimals, quaternions with 6,
pinch strengths with 4, times in ns.

```json
{
  "format": "puppetxr-take",
  "version": 1,
  "name": "pinch_right",
  "instruction": "Pinch with the right hand five times ...",
  "hands": "right",
  "space": "local_floor",
  "runtime": "Oculus",
  "system": "Meta Quest 3",
  "tracks": ["head", "hands", "aim"],
  "joint_set": "XR_EXT_hand_tracking",
  "display_period_ns": 11111111,
  "frames": [
    {
      "t": 0,
      "head": [x, y, z, qx, qy, qz, qw],
      "left":  { "active": 1, "flags": [...26 u8...], "joints": [[x, y, z, qx, qy, qz, qw, radius], ...26],
                 "aim": { "status": 31, "pose": [...7], "pinch": [i, m, r, l] },
                 "source": "unobstructed" },
      "right": { ... }
    }
  ]
}
```

- `t` is the predicted display time relative to the first frame; frames are the app's frames
  (72 to 120 Hz), the header has the display period. `head` is `null` while the view is not
  located. `space` is the one used: `local_floor` falls back to `stage`, then `local`.
- `hands` is what the step asks for; both hands are always recorded.
- `interaction`, with `XR_EXT_hand_interaction`: the `pinch_ext/value`, `aim_activate_ext/value`
  and `grasp_ext/value` actions of that hand on `/interaction_profiles/ext/hand_interaction_ext`,
  only in frames where they are active (from the second frame of a session on, once the runtime
  chose the profile), also for a hand without joints.
- `flags` are the `XrSpaceLocationFlags` of each joint; an untracked hand has `active` 0 and no
  joints.
- `aim` is `XrHandTrackingAimStateFB` when the runtime has it: what the runtime reports as pinch
  strengths and aim pose for these joints, the reference the simulated pinch strength is fitted
  to. `source` from `XR_EXT_hand_tracking_data_source`.
- The runtime name and system name come from `xrGetInstanceProperties` and `xrGetSystemProperties`;
  no device serials, user names, OS versions or paths.
- Body (later): a `body` track with the joint set name, `[x, y, z, qx, qy, qz, qw]` per joint
  and its flags, and the skeleton (`xrGetBodySkeletonFB`) in the header whenever its change
  count rises.

Readers accept any later minor fields and reject a `version` they do not know.

## Requests the recorder makes

`XR_EXT_hand_tracking`, `XR_FB_hand_tracking_aim`, `XR_EXT_hand_tracking_data_source`,
`XR_EXT_hand_joints_motion_range` (unobstructed), `XR_META_hand_tracking_frequency_hint` (high),
`XR_EXT_hand_interaction` when offered; `XR_FB_passthrough`. Missing ones are logged and their fields left out.

## Tests

`tests/tests_recorder.cpp` builds `recorder.cpp` into `puppetxr_tests` and runs it headless on
the runtime in the process (`RecorderTest`, about half a second each, mostly loading the room):
a two step protocol in automatic mode while the test changes the pinch and drops the right hand
every third frame of the second take; every frame's joints, radii and pinch strength match the
simulated hand within the rounding, lost frames have `active` 0, the controller-held left hand is
not reported. Poking `RECORD` starts a take that ends after its seconds; a poke beside the buttons
starts nothing. The skeleton of a hand in front of the panel shows in the composited frame in its
colour, and nothing of it once the hand is lost. Every `RecorderTest` runs the recorder's Vulkan
under `VK_LAYER_KHRONOS_validation` where it is installed (`vulkan_validation`) and fails on an
error. A pinch with the aim ray on `RECORD` starts a take; aiming alone, or a pinch begun
beside the button and held while the ray moves onto it, clicks nothing. A runtime offering more
than 64 swapchain formats, as a Quest 3 does, still gives the panel its format. `RecorderProtocol` checks the built-in steps and a bad protocol file's error.

On a Quest: by hand, once per release of the recorder (Pending verification in `TODO.md`).
