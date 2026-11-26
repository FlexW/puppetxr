# Changelog

Changes users notice, newest release first. Every such change adds a line under Unreleased;
`tools/release.sh` turns that section into the release, and the release notes are taken from
it (docs/building.md, Releasing). Breaking changes of the C API, the recording format or the
config start with **Breaking:**.

## Unreleased

## 0.2.0 - 2026-11-26

- Fixed: in Unity's editor with Meta's OpenXR feature the app's image was cut off at the eye's edge while the head turned fast. OVRPlugin ends each frame there with the display time of the frame before, and puppetxr showed it with that older head pose; a frame is now never shown before the time it was waited for.
- `XR_FB_hand_tracking_mesh` gives a sculpted hand fitted to a Quest 3's skeleton: one closed surface ending at the wrist, blended across the joints, where it was a prism per bone that bent with a hard edge.
- Render models: modelled, textured controllers (`XR_FB_render_model`, `XR_EXT_render_model`), keyboard and virtual keyboard. `XR_EXT_render_model` moves the trigger, squeeze, thumbstick and each button with its input, where one node moved for all buttons.
- Gestures in the simulator: JSON files of timed steps that move a hand's anchor to points given as expressions, turn its palm and shape its fingers, with params, events and errors that name the JSON path (`docs/hand-motion.md`); 14 built-in gestures (`data/motion/gestures/`) that shape, hold and place the hand as recorded on a Quest 3, and users' gestures from `gestures.paths` and `PUPPETXR_GESTURE_PATH`; `puppetxrctl gesture list|show|check|load|play|wait|events|stop` and the C API's gesture functions (API version 3).
- The Simulator panel's Gestures tab plays gestures, poses a hand with a slider per finger and saves the pose as a gesture (`gestures.dir`).
- Fixed: on Vulkan, swapchains with `XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT` are storage images; compute shaders writing to them were undefined behaviour and crashed some drivers.
- Fixed: on Vulkan with the offscreen window and a driver without `VK_EXT_headless_surface`, the device asked for `VK_KHR_swapchain` without `VK_KHR_surface` on the instance, a validation error.
- `XR_EXT_uuid`.
- Fixed: recording with a model over 64 KB loaded, or a large anchor store, wrote past the recorder's buffer.
- The four rooms are furnished with scanned models and surfaces and baked at 3072 x 3072; their entities stand where they did, seats and couches now down to the floor.
- The chair, case and pencil props have more shape and baked light.
- Recording format 6: the shape a gesture gives a tracked hand, per frame; format 1 to 5 recordings still read.
- As Meta's runtime a tracked hand outside the hand tracking cone stays active with the controller data source and zero joints, as a Quest 3 reports a lost hand; under puppetxr's own name it is inactive, as the spec says.
- A tracked hand reports the pinch and grasp strengths its joints give on a Quest 3: a pinch input closes the hand until it reads that strength (a half pinch leaves the tips 12 to 20 mm apart), an idle hand reports a grasp of about 0.45 and a pinching one a high grasp, as on the headset.
- Tracked hands have the skeleton and poses of real hands, fitted to recordings on a Quest 3 (`data/motion/hand_model.json`, `tools/hand_fit.py`): the palm and knuckles 2 cm farther from the wrist than before, and a hand at rest is relaxed instead of spread open; a pinch closes the tips to 7.5 mm, as on the headset.
- `puppetxr_recorder`: a minimal OpenXR app that records tracked hands, head and the runtime's pinch strengths into JSON takes, step by step through a protocol of calibration motions and gestures, with a panel poked by the index tip (`docs/recorder.md`); `tools/recorder/build_apk.sh` builds it as an APK for Meta Quest without Gradle.
- macOS: a new instance the main thread creates after the last one ended on another thread keeps its mirror window; the stop the old one left queued runs before the new video starts.
- macOS: an app that destroys its last instance on another thread while the main thread is busy no longer leaves the mirror window open; it closes once the main thread comes by.
- The Blender room exporter (`tools/blender/puppetxr_room_export.py`) loads again: three string literals were broken across lines, a syntax error.
- While the runtime captures an app's output, its own lines on the terminal start with `puppetxr: ` again, not `puppetx`.
- Text the runtime and `puppetxrctl` format comes out the same on every platform, through stb_sprintf instead of the C runtime: not-a-number and infinity print as `NaN` and `Inf` everywhere.
- Mesh props: any glTF 2.0 model (`.glb` or `.gltf`) as a prop that stands still, moves back and forth or circles, to test how an app copes with things that move through the room, such as a chair pushed past. No extension tracks them; they show in passthrough, environment depth and environment raycasts like the room. Three are built in (`chair`, `pencil`, `case`); others load at run time (`load_model` in the C API, `puppetxrctl model load`, config `models`). Docs: docs/props.md.
- Keyboard and QR code props show in environment depth and stop environment raycasts too.
- **Breaking:** recordings are format 4: props may be meshes, and a recording keeps the models they show, so a replay needs none of their files. Formats 1 to 3 still read.
- The C API is version 2: `load_model`, and `PUPPETXR_Prop` gains `scale` and `model` for `PUPPETXR_PROP_MESH`.
- Passthrough dimmed through its style's opacity shows only the nearest surface; before, the room's surfaces behind showed through, its triangles plain to see.
- With the control server on, Vulkan apps no longer drop to a few frames a second: reading each frame back took 60 ms on GPUs whose first host visible memory is their own; it now reads through cached host memory, about 1.5 ms.
- **Breaking:** environment depth (`XR_META_environment_depth`) behaves like a Quest 3's sensor by default: a new image every 40 ms that shows 45 ms after it was taken, from views where the head was 26 ms after the capture, 64.85 mm apart whatever the IPD, with noise and the band at the outer edges no camera sees; four images; the capture time through `XrEnvironmentDepthImageTimestampMETA`. Every parameter is configurable (`environment_depth`, docs/config.md); `"environment_depth": "eyes"` or `PUPPETXR_ENVIRONMENT_DEPTH=eyes` gives the depth from the eyes at every frame as before.
- **Breaking:** environment raycasts (`XR_META_environment_raycast`) behave like a Quest 3's: they march through the depth sensor's captures, so hits change every 40 ms, lag the head (a capture is read from 130 ms after it), carry the sensor's noise in position and normal, reach below the eyes' field of view (a view 55 degrees down), and report every status: `RAY_OCCLUDED` for a ray starting inside a surface, `HIT_POINT_OCCLUDED` behind an edge, `HIT_INVALID_ORIENTATION` at edges, `HIT_POINT_OUTSIDE_OF_FOV` when the distance filter ends the ray first. The time asked for is ignored, as on a Quest 3. `"raycast": "mesh"` in `environment_depth` or `PUPPETXR_ENVIRONMENT_RAYCAST=mesh` gives the exact hits on the global mesh as before.
- **Breaking:** recordings are format 3 for the environment depth sensor and format 5 for the raycast settings, both in the header. Formats 1 and 2 still read and replay depth from the eyes, formats 1 to 4 raycasts against the global mesh, as they were recorded.
- Environment depth is traced into the room on the CPU, so it is the same on every GPU, and rooms load faster.
- **Breaking:** the mirror window's window system is picked with `PUPPETXR_VIDEO_DRIVER` (`wayland`, `x11`, `windows`, `cocoa` or `offscreen`); SDL's `SDL_VIDEODRIVER` no longer applies.
- SDL3 is gone: windows and input use each platform's own window system (Win32, Wayland and X11, Cocoa). On Linux the mirror window uses Wayland where it is available, decorated by the compositor or, on GNOME, by a title bar of its own, else X11. Building on Linux needs only the X11, XInput, XRandR, Xcursor and xkbcommon headers.
- The UI takes the clipboard, the input method and cursor shapes, and its text fields take Delete, Home, End, Page Up and Down and the keypad; those keys and the mouse's side buttons can be bound in the config too.
- Building on macOS no longer needs `spirv-cross`: the Metal backend compiles the shaders' single source as MSL, like the GL backend does as GLSL.
- The dependencies are part of the repository (`third_party/`, versions and licenses in `third_party/README.md`): building no longer downloads anything, and the source release is one tarball that builds offline; the separate `-src-full.tar.gz` is gone.
- Body tracking: the skeleton's hierarchy is a Quest 3's: the hand's wrist hangs from the lower arm and the palm from the wrist (both hung from the wrist twist), and the root's parent is -1 in the full body joint set too (was `XR_FULL_BODY_JOINT_NONE_META`). A hand wrist rotation is now relative to the lower arm.
- Body tracking: an untracked hand bends its fingers by their joints' rotations (`body joint right_hand_index_proximal rot ...`); before, hand joint rotations were stored but had no effect.

## 0.1.0 - 2026-11-07

- Body tracking: `body_tracking.skeleton_change_period_ms` (or `PUPPETXR_SKELETON_CHANGE_PERIOD_MS`) makes the skeleton change count rise once per period with the same shape, as some runtimes do; off by default.
- Recordings are format 2: the header holds the skeleton change period. Format 1 still reads.

- `xrQuerySpacesFB` refuses a component filter without `XrSpaceStorageLocationFilterInfoFB`, as a Quest 3 does.
- The sources build with MSVC again.
- Linux: closing the mirror window no longer can end the app with X's BadWindow error.
- First public release.
