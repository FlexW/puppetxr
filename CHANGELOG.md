# Changelog

Changes users notice, newest release first. Every such change adds a line under Unreleased;
`tools/release.sh` turns that section into the release, and the release notes are taken from
it (docs/building.md, Releasing). Breaking changes of the C API, the recording format or the
config start with **Breaking:**.

## Unreleased

- Mesh props: any glTF 2.0 model (`.glb` or `.gltf`) as a prop that stands still, moves back and forth or circles, to test how an app copes with things that move through the room, such as a chair pushed past. No extension tracks them; they show in passthrough, environment depth and environment raycasts like the room. Three are built in (`chair`, `pencil`, `case`); others load at run time (`load_model` in the C API, `puppetxrctl model load`, config `models`). Docs: docs/props.md.
- Keyboard and QR code props show in environment depth and stop environment raycasts too.
- **Breaking:** recordings are format 4: props may be meshes, and a recording keeps the models they show, so a replay needs none of their files. Formats 1 to 3 still read.
- The C API is version 2: `load_model`, and `PUPPETXR_Prop` gains `scale` and `model` for `PUPPETXR_PROP_MESH`.
- **Breaking:** environment depth (`XR_META_environment_depth`) behaves like a Quest 3's sensor by default: a new image every 40 ms that shows 45 ms after it was taken, from views where the head was 26 ms after the capture, 64.85 mm apart whatever the IPD, with noise and the band at the outer edges no camera sees; four images; the capture time through `XrEnvironmentDepthImageTimestampMETA`. Every parameter is configurable (`environment_depth`, docs/config.md); `"environment_depth": "eyes"` or `PUPPETXR_ENVIRONMENT_DEPTH=eyes` gives the depth from the eyes at every frame as before.
- **Breaking:** recordings are format 3: the header holds the environment depth sensor. Formats 1 and 2 still read and replay depth from the eyes, as they were recorded.
- Environment depth is traced into the room on the CPU, so it is the same on every GPU, and rooms load faster.
- **Breaking:** the mirror window's window system is picked with `PUPPETXR_VIDEO_DRIVER` (`wayland`, `x11`, `windows`, `cocoa` or `offscreen`); SDL's `SDL_VIDEODRIVER` no longer applies.
- SDL3 is gone: windows and input use each platform's own window system (Win32, Wayland and X11, Cocoa). On Linux the mirror window uses Wayland where it is available, decorated by the compositor or, on GNOME, by a title bar of its own, else X11. Building on Linux needs only the X11, XInput, XRandR, Xcursor and xkbcommon headers.
- The UI takes the clipboard, the input method and cursor shapes, and its text fields take Delete, Home, End, Page Up and Down and the keypad; those keys and the mouse's side buttons can be bound in the config too.
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
