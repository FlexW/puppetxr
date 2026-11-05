# Configuration

The runtime reads one JSON file, then environment variables on top. Lookup order:
`PUPPETXR_CONFIG=<path>`, else `puppetxr.json` in the working directory, else defaults.
Parsing lives in `sim_parse_config` (`src/sim.cpp`); every key is optional and
unknown keys are ignored with a warning only for bad values.

```json
{
    "device": "quest3",
    "runtime_name": "puppetxr",
    "headless": false,
    "show_ui": false,
    "show_welcome": true,
    "window": { "width": 1280, "height": 720, "maximized": true },
    "clock": { "mode": "realtime", "refresh_rate_hz": 90, "refresh_rates": [ 72, 80, 90, 120 ] },
    "views": { "width": 1680, "height": 1760, "fov_degrees": 90, "ipd": 0.064, "mask_radius": 1.25, "mirror": "left" },
    "stage": { "width": 2, "depth": 2 },
    "hands": { "tracking_fov_degrees": 120 },
    "body": { "height": 1.75 },
    "controller": "meta_touch_plus",
    "devices": {
        "head":  { "position": [0, 1.6, 0],     "orientation": [0, 0, 0, 1], "active": true },
        "left":  { "position": [-0.2, 1.3, -0.3], "orientation": [0.5, 0, 0, 0.86603] },
        "right": { "position": [0.2, 1.3, -0.3],  "orientation": [0.5, 0, 0, 0.86603] }
    },
    "controls": {
        "move_speed": 1.0,
        "look_sensitivity": 0.003,
        "turn_speed": 1.5708,
        "primary_hand": "right",
        "auto_touch": true,
        "analog_ramp": 0.5,
        "analog_release": 0.1,
        "hand_speed": 0.3, "next_profile": "right_bracket", "previous_profile": "left_bracket",
        "profiles": [
            { "name": "panel", "extends": "hands", "key": "5", "wheel": "push_left" }
        ],
        "move_forward": "w", "move_back": "s", "move_left": "a", "move_right": "d",
        "move_up": "e", "move_down": "q",
        "turn_left": "j", "turn_right": "l", "turn_up": "i", "turn_down": "k",
        "reset_hands": "0", "reset": "r", "freeze": "space", "hand_source": "t", "ui": "tab",
        "bindings": [
            { "key": "u", "slot": "trigger" },
            { "key": "u", "slot": "trigger", "modifier": "shift", "hand": "left" },
            { "key": "u", "slot": "trigger", "modifier": "ctrl",  "hand": "right" },
            { "key": "i", "slot": "stick",   "modifier": "shift", "hand": "left", "value": [0, 1] },
            { "key": "f", "input": "thumbrest/touch" }
        ]
    },
    "screenshot_at_frame": 0,
    "exit_at_frame": 0,
    "record": "",
    "profile": "",
    "replay": "",
    "screenshot_dir": "",
    "room": "living_room",
    "rooms_dir": "",
    "disabled_extensions": [],
    "graphics": ["vulkan", "opengl", "opengles", "d3d11", "d3d12", "metal"],
    "origin_offset": "none",
    "origin_seed": 0,
    "drift_rate": { "yaw_degrees": 0, "velocity": [0, 0, 0] },
    "anchors": { "dir": "", "cloud_dir": "", "app_id": "", "user_id": 1, "latency_frames": 0, "localize_frames": 0 }
}
```

| Key | Meaning |
|---|---|
| `device` | the headset preset (see Device presets), default `quest3`; applied before the file's other keys, so those win |
| `system_name` | what `xrGetSystemProperties` reports, default the preset's |
| `runtime_name` | what `xrGetInstanceProperties` reports, default `puppetxr` (`Oculus` with OVRPlugin loaded, see Known modules); empty keeps the default. Meta's OVRPlugin (Unity's Meta XR packages) refuses runtimes it does not know as Meta's: `Oculus` lets it run |
| `accept_time_zero` | locates (`xrLocateViews`, `xrLocateSpace`, `xrLocateSpaces`), anchor creation (`xrCreateSpatialAnchorFB`), hand joints, environment depth and raycasts, geometry transforms and render model states at time 0 use the predicted display time of the frame being drawn (the latest `xrWaitFrame` gave; the simulator's time before the first) instead of failing with `XR_ERROR_TIME_INVALID` as the spec requires (default false, true with OVRPlugin loaded; the CTS checks it). Meta's OVRPlugin locates at time 0 and without it never settles its tracking origin; a time a frame behind made its moving controllers jitter against Unity's |
| `accept_older_frame_time` | `xrGetRecommendedLayerResolutionMETA` answers for the display time of any frame not yet ended instead of failing with `XR_ERROR_TIME_INVALID` for a time before the last `xrWaitFrame`'s, as the extension requires (default false, true with Meta's Unreal plugin loaded, see Known modules). Meta's Unreal plugin asks on its RHI thread with the time of the frame it submits, after the game thread waited for the next, and logs an ensure for each refusal |
| `discover_before_running` | `xrDiscoverSpacesMETA` answers on a session that does not run yet instead of failing with `XR_ERROR_SESSION_NOT_RUNNING`, which the spec allows; each such call logs an error (default false, true with OVRPlugin or MRUK's shared library loaded, see Known modules). MRUK discovers the room in the first scene's `Awake`, before Unity begins the session, and never retries |
| `trace` | the OpenXR API trace: `true` or `"*"` logs every xr* call with its arguments, result and outputs, a string of comma-separated names with `*` wildcards (`"xrDiscover*,*Space*"`) only those (default off; docs/openxr.md, API trace). `PUPPETXR_TRACE=1` or `PUPPETXR_TRACE=<names>`, `0` off |
| `show_ui` | whether the ImGui panels start visible (default hidden); `controls.ui` (the key left of 1) toggles them at any time |
| `show_welcome` | whether the welcome overlay (controls and where to get help) shows at start until dismissed with Enter (default true); `false` for development. F1 opens it at any time |
| `window` | mirror window: `maximized` (default true) starts it maximized where the video driver can (not offscreen); `width` and `height` are its size otherwise and when restored. The window shows the eye image downscaled to fit, never upscaled |
| `clock.mode` | `realtime` samples the OS clock once per `xrWaitFrame`; `fixed_step` advances by one display period per frame (replay, CI) |
| `clock.refresh_rate_hz` | simulated display refresh rate; the display period reported to apps is its inverse. Default 90 like Quest 3 |
| `clock.refresh_rates` | the other rates an app may request with `XR_FB_display_refresh_rate`, up to 8 in 1..1000 Hz; `refresh_rate_hz` is always one of them. Default `[72, 80, 90, 120]` like Quest 3 |
| `views.width`, `views.height` | per-eye render resolution recommended to the app. Apps cannot change resolution at runtime, so it is fixed for the run. Default 1680x1760 like Quest 3 |
| `views.mirror` | `left`, `right` or `both`: which eye the mirror window shows; views are downscaled to fit the window but never upscaled. Default `left` |
| `controller` | simulated controllers: `meta_touch_plus` (default, Quest 3), `meta_touch_pro` (Quest Pro), `oculus_touch` or `khr_simple`; also selectable in the UI, fixed while recording or replaying |
| `views.fov_degrees` | full angle per eye on both axes; poses are offset by half `ipd` from the head |
| `views.mask_radius` | size of the lens mask (`views.lens_mask`) for `XR_KHR_visibility_mask`: an ellipse of this many half-extents of the view around its center cuts the corners; default 1.25, 0 (or 1.41 and more) means no mask. The composite (mirror window and screenshots) is black outside it, as the wearer sees it (docs/compositor.md) |
| `views.lens_mask` | the lens mask on (default off, so the whole view shows): black outside `mask_radius` in the mirror window and screenshots, and the hidden area apps get from `XR_KHR_visibility_mask`. The Display section switches it while running, and apps get `XrEventDataVisibilityMaskChangedKHR` for both views; `PUPPETXR_LENS_MASK=0\|1`, `puppetxrctl lens mask on\|off` |
| `devices.*.orientation` | quaternion `[x, y, z, w]`, normalized on load. The hands default to 60 degrees about X, the handle raised, which makes the aim pose (60 degrees below the grip, where OVRPlugin's controller pose points and Meta's samples aim) level and forward |
| `controls.turn_speed` | rad/s of the turn keys, default 90 degrees per second |
| `controls.ui` | the key that shows and hides the panels, default `tab` |
| `controls.primary_hand` | `left` or `right` (default): the hand of bare keys in profiles without a `hand` |
| `controls.hand_speed` | m/s of a hand moved by keys, default 0.3 |
| `controls.next_profile` | the key that goes to the next profile with a key; default `right_bracket` |
| `controls.previous_profile` | the key that goes to the one before; default `left_bracket` |
| `controls.profiles` | adds or changes profiles (see Profiles) |
| `controls.auto_touch` | default true: a key pressing a click, the trigger or the stick also touches it |
| `controls.analog_ramp`, `controls.analog_release` | seconds for a key-held trigger, grip or stick to go from 0 to full (default 0.5) and back once released (default 0.1); 0 jumps. Buttons never ramp |
| `controls.bindings` | replaces the default table below. While `key` is held with exactly `modifier` (`none` default, `shift`, `ctrl`; either side), `value` (default `[1, 0]`) is written to `slot` (mapped per controller and hand) or to the raw `input` of `hand` (`active` default, `left`, `right`); several bindings on one input are summed and clamped, sticks to the unit circle. Slots the controller or hand lacks and inputs the controller lacks do nothing |
| `controls.mode_*`, `controls.look` | gone; profiles say what the keys drive, a click into the view takes the mouse. A warning is logged |
| `record` | record the session to this file from the first frame (`docs/recording-format.md`) |
| `profile` | a zone profile capture into this Chrome trace from `xrCreateInstance` to the runtime's shutdown (`docs/profiling.md`); `PUPPETXR_PROFILE` too |
| `replay` | replay this recording: its resolution, refresh rate, optics and start state override the config, keyboard and mouse controls are off until it ends |
| `replay_exit` | request session exit when the replay ends, default true; false continues live |
| `screenshot_at_frame` | write `<screenshot_dir>/puppetxr_frame_<n>.png` at native eye resolution when that frame is composited, 0 never |
| `screenshot_dir` | directory of screenshots and recordings started from the window, created when missing; empty is the working directory |
| `exit_at_frame` | request session exit at that frame, for CI runs of apps, 0 never |
| `room` | the simulated room (`docs/world.md`): a directory name in the rooms dir (default `living_room`, so apps that need a room such as Meta's MRUK samples find one), or `none`. A room that is not found logs an error and runs without one. Also selectable in the UI and the control API, fixed while recording or replaying; a replay uses the recording's room |
| `control` | `{ "enabled": false, "port": 0, "address": "127.0.0.1", "paused": false }`: the control server for `puppetxrctl` (`docs/control-cli.md`); port 0 picks a free one, any address but 127.0.0.1 exposes the simulator to that network |
| `disabled_extensions` | extension names never offered by `xrEnumerateInstanceExtensionProperties` and refused by `xrCreateInstance` (`XR_ERROR_EXTENSION_NOT_PRESENT`), to test how an app copes without them; unknown names are logged and ignored. The C API's `set_disabled_extensions` adds to the list |
| `graphics` | the graphics APIs apps may use, one name or a list of `vulkan`, `opengl`, `opengles`, `d3d11`, `d3d12`, `metal` (default: all). Only their binding extensions are offered (`XR_KHR_vulkan_enable(2)` and the Vulkan-only `XR_FB_swapchain_update_state_vulkan`, `XR_META_vulkan_swapchain_create_info`; `XR_KHR_opengl_enable`; `XR_KHR_opengl_es_enable`; `XR_MNDX_egl_enable` for either GL; `XR_KHR_D3D11_enable`; `XR_KHR_D3D12_enable`), so an engine that picks its API from the offered extensions runs the forced one. Unknown names are logged and ignored; a list of only unknown names restricts nothing. The forced set is logged at `xrCreateInstance` |
| `origin_offset`, `origin_seed` | where the stage origin lies in the room: `"none"` (default), `"per_run"` or `{ "yaw_degrees", "position": [x, z] }` (`docs/anchors.md`) |
| `drift_rate` | tracking drift from the first frame on (`docs/anchors.md`, Tracking drift): `yaw_degrees` per second about +Y and `velocity` `[x, y, z]` in m/s; default none. A malformed velocity is logged and ignored |
| `passthrough.default_active` | the simulated user prefers to start in passthrough: `XR_META_passthrough_preferences` reports `DEFAULT_TO_ACTIVE`. Default false |
| `keyboard_tracking.enabled` | the simulated user's keyboard tracking setting (Settings > Devices > Keyboard on Quest); false: `XR_META_dynamic_object_tracker`'s create result is `XR_ERROR_INITIALIZATION_FAILED`, as Meta documents for a tracker the user turned off. Default true |
| `anchors` | anchor stores (`docs/anchors.md`): `dir` (default `os_state_dir()/anchors`), `cloud_dir` (default `<dir>/cloud`), `app_id` (default the application name), `user_id` (default 1), `latency_frames` and `localize_frames` (default 0) |
| `rooms_dir` | where rooms are looked up; empty means `rooms/` of the source tree for development builds, else `<datadir>/puppetxr/rooms` of the install |
| key names | `a`..`z`, `0`..`9`, `f1`..`f12`, `space`, `escape`, `enter`, `tab`, `backspace`, `arrow_*`, `left_shift`, `left_ctrl`, `left_alt`, `right_*`, `mouse_left`, `mouse_right`, `mouse_middle`, `grave` (the key left of 1), `left_bracket`, `right_bracket` (right of P; Ü and + on German layouts) |
| slot names | `trigger`, `grip`, `primary` (X left, A right), `secondary` (Y, B), `menu` (left Touch, both simple; on the right Touch, which has none, the left one's), `system` (right Touch), `stick`, `stick_click`; the same on every Touch generation; on `khr_simple` only `trigger` (select) and `menu` |
| input names | `x/click`, `x/touch`, `y/click`, `y/touch`, `a/click`, `a/touch`, `b/click`, `b/touch`, `menu/click`, `system/click`, `squeeze/value`, `trigger/value`, `trigger/touch`, `thumbstick`, `thumbstick/click`, `thumbstick/touch`, `thumbrest/touch`, `trigger/proximity`, `thumb_resting_surfaces/proximity` (every Touch; `x`, `y` and `menu/click` on the left hand only, `a`, `b` and `system/click` on the right only, as the profiles bind them); `trigger/curl`, `trigger/slide` (Touch Pro and Plus); `thumbrest/force`, `stylus/force` (Touch Pro); `trigger/force` (Touch Plus); `select/click`, `menu/click` (KHR simple). With `auto_touch` a pressed button also touches, and a touched trigger or thumb surface is also in proximity. Bindings to inputs the current controller lacks are ignored |

Environment overrides: `PUPPETXR_TRACE=1|<names>`, `PUPPETXR_DEVICE=<preset>` (wins over the file's `device`, the file's other keys still win over the preset), `PUPPETXR_RUNTIME_NAME=<name>`, `PUPPETXR_ACCEPT_TIME_ZERO=0|1`, `PUPPETXR_ACCEPT_OLDER_FRAME_TIME=0|1`, `PUPPETXR_DISCOVER_BEFORE_RUNNING=0|1`, `PUPPETXR_HEADLESS=1`, `PUPPETXR_SHOW_WELCOME=0|1`, `PUPPETXR_FIXED_STEP=1`, `PUPPETXR_RECORD=<path>`, `PUPPETXR_PROFILE=<path>`,
`PUPPETXR_REPLAY=<path>`, `PUPPETXR_REPLAY_EXIT=0`, `PUPPETXR_SCREENSHOT_DIR=<dir>`, `PUPPETXR_SCREENSHOT_AT=<frame>`,
`PUPPETXR_EXIT_AT=<frame>`, `PUPPETXR_ROOM=<name|none>`, `PUPPETXR_ROOMS_DIR=<dir>`, `PUPPETXR_CONTROL=1`,
`PUPPETXR_CONTROL_PORT=<port>`, `PUPPETXR_CONTROL_ADDRESS=<ip>`, `PUPPETXR_CONTROL_PAUSED=1`,
`PUPPETXR_DISABLE_EXTENSIONS=<name>,<name>` (replaces `disabled_extensions`), `PUPPETXR_GRAPHICS=<api>,<api>` (replaces `graphics`),
`PUPPETXR_ORIGIN_OFFSET=none|per_run|<yaw>,<x>,<z>`, `PUPPETXR_ORIGIN_SEED=<n>`, `PUPPETXR_DRIFT_RATE=<yaw degrees/s>,<x>,<y>,<z m/s>`, `PUPPETXR_HAND_TRACKING_FOV=<degrees>`, `PUPPETXR_PROPS=<JSON array of props>`, `PUPPETXR_KEYBOARD_TRACKING=0|1`, `PUPPETXR_ANCHORS_DIR=<dir>`,
`PUPPETXR_CLOUD_DIR=<dir>`, `PUPPETXR_APP_ID=<id>`, `PUPPETXR_USER_ID=<n>`, `PUPPETXR_CONFIG=<path>`, `PUPPETXR_LOG_FILE=<path>` (append every runtime
log line to a file, for hosts that swallow stdout and stderr such as test frameworks
and engines; others may read it, and more processes append to it, while it is open), `PUPPETXR_STATE_DIR=<dir>` (the per-user state directory itself, see below).

## Device presets

A preset is a headset as Meta's PC runtime (Link) presents it: the system name and vendor id of
`xrGetSystemProperties`, the `XR_META_headset_id` UUID, the controller, the eye buffer size the
runtime recommends (`xrEnumerateViewConfigurationViews`) and the refresh rates of
`XR_FB_display_refresh_rate`. Meta's OVRPlugin takes its headset type from the UUID (OVRPlugin 207
treats an unknown one as a Quest 2, and falls back to the system name without the extension), so
MRUK and the Meta XR SDKs see the model the preset names and pick its controller models. Table `sim_device_presets` in `src/sim.cpp`,
`sim_apply_device`; keys in the file or the environment change any value afterwards.

| `device` | System name | Headset id | OVRPlugin type | Controller | Eye buffer | Refresh rates (Hz) |
|---|---|---|---|---|---|---|
| `quest3` (default) | Meta Quest 3 | `6409e3b3-5089-4ed6-868e-aaed82da3676` | `Meta_Link_Quest_3` | `meta_touch_plus` | 1680x1760 | 72, 80, 90, 120 |
| `quest3s` | Meta Quest 3S | `8c2ec447-ac73-419f-9096-36a772337862` | `Meta_Link_Quest_3S` | `meta_touch_plus` | 1440x1584 | 72, 80, 90, 120 |
| `quest2` | Oculus Quest2 | `525078a5-5aab-4dc9-b802-1ebd6c7cfff4` | `Oculus_Link_Quest_2` | `oculus_touch` | 1440x1584 | 72, 80, 90, 120 |
| `questpro` | Meta Quest Pro | `990986c7-9bad-475f-8983-c1c7bd49ad51` | `Meta_Link_Quest_Pro` | `meta_touch_pro` | 1800x1920 | 72, 90 |

- The vendor id is Meta's, `0x2833`, for every preset.
- The refresh rate stays `clock.refresh_rate_hz` (90 by default), which every preset offers.
- The field of view stays `views.fov_degrees` for every preset: one symmetric angle, as recordings
  store it. The models' own asymmetric eye angles need a recording format version (TODO.md).

## Known modules

Some engine plugins need the runtime to bend the spec. puppetxr recognises them by their library in
the process (`os_module_loaded`) and changes its defaults, so their apps run without a config file.
Order: defaults, known modules, the config file, the environment; a key set in the file or the
environment always wins. The table is `sim_known_modules` in `src/sim.cpp`
(`sim_apply_known_modules`); the instance's log line names the module found. A module loaded
after `xrCreateInstance` read the config still answers an early `xrDiscoverSpacesMETA`
(`sim_late_discovery_module`) unless the file or the environment set `discover_before_running`;
the error each such call logs names the module.

| Library | `runtime_name` | `accept_time_zero` | `accept_older_frame_time` | `discover_before_running` | Why |
|---|---|---|---|---|---|
| `OVRPlugin.dll`, `libOVRPlugin.so` | `Oculus` | true | unchanged | true | Meta's OVRPlugin (Meta XR SDKs for Unity and Unreal) refuses runtimes it does not know as Meta's and locates at time 0; MRUK on it discovers the room before the session runs |
| `UnrealEditor-OculusXRHMD.dll` | unchanged | unchanged | true | unchanged | Meta's Unreal plugin (`OculusXRLayerExtensionPlugin.cpp`) asks for layer resolutions with an older frame's time, on Unreal's OpenXR plugin too. A packaged game links the plugin into its executable, where no library names it: set `PUPPETXR_ACCEPT_OLDER_FRAME_TIME=1` |
| `mrutilitykitshared.dll`, `libmrutilitykitshared.so` | unchanged | unchanged | unchanged | true | MRUK's shared library discovers the room before the session runs and never retries, on OVRPlugin and on Unreal's OpenXR plugin, which loads it after creating the instance and has no OVRPlugin |

Another SDK that needs the same is one row and a sample test. If a plugin stops shipping the
library, its apps run with the spec defaults and the sample tests that relied on the row fail.

## Remembered UI choices

A room picked in the UI is written to `os_state_dir()/state.json`
(`$PUPPETXR_STATE_DIR` if set, else `$XDG_STATE_HOME/puppetxr`, else `~/.local/state/puppetxr`;
`%LOCALAPPDATA%\puppetxr` on Windows) as `{ "room": "<name>" }` (`"none"` for no room) and becomes the default of the
next start. Code: `sim_load_state`, `sim_save_state`, `sim_apply_state` (`src/sim.cpp`),
`choose_room_locked` (`src/oxr.cpp`).

- Applies only when neither `room` in the config file nor `PUPPETXR_ROOM` is set.
- Headless runs ignore it, so tests, CI and agents never depend on someone's last click.
- Only the UI writes it; `set_room` in the C API and `puppetxrctl room` leave it alone.
- A replay always uses the recording's room. A remembered room that no longer exists
  loads none, with an error in the log.
- ctest sets `PUPPETXR_STATE_DIR` to `build/<preset>/test_state` (`tests/CMakeLists.txt`).

The same file remembers `"welcome_dismissed": true` once the welcome overlay was closed
with Enter or "Show welcome at start" was unticked in the Help menu
(`remember_welcome_locked` in `src/oxr.cpp`). Both fields are written together, so
saving one keeps the other. Escape and F1 close the overlay without writing anything.

## Controls model

Code: `apply_controls` in `src/sim.cpp`, once per frame in fixed order: hand reset, the
profile's drives (mouse, turn keys, move keys, wheel), bindings. Keys are physical positions
(scancodes) named after US QWERTY; the UI prints the label of the active layout
(`os_key_label`), so `y` shows as Z on QWERTZ. The arrows are the turn keys (I/J/K/L) everywhere,
with modifiers too. The mirror window sends mouse moves, buttons and the wheel only once a click
into the view took the mouse (Escape gives it back).

### Profiles

A profile (`SIM_Profile`) says what each source drives. The number keys pick the profiles with a
key, ] goes to the next of them and [ back; the UI's Session section, `puppetxrctl profile`
and the C API pick any. A toast names the new profile and its keys; the welcome overlay (F1)
lists them.

| Profile | Key | WASD, Q/E | I/J/K/L, arrows | mouse | Shift + mouse | wheel | Shift + wheel | bare keys' hand |
|---|---|---|---|---|---|---|---|---|
| `body` (start) | 1 | walk | turn the body | turn the body | | | | primary |
| `stick` | 2 | left stick | right stick | turn the body | | | | primary |
| `hands` | 3 | move the left hand | move the right hand | turn the right hand | turn the left hand | push the right hand | roll the left hand | right |
| `eyes` | 4 | walk | turn the body | turn the eyes | | | | primary |
| `head` | | move the head | turn the head | turn the head | | | | primary |
| `left`, `right` | | move that hand | turn that hand | turn that hand | | | | that hand |

`hands` is for a panel on the left hand and a pointer on the right: the head stays, both hands
move in the view's plane. `head`, `left` and `right` are the control modes before profiles; they
have no key.

Drives (`SIM_Drive`, names as in config files):

| Drive | Keys | Mouse | Wheel |
|---|---|---|---|
| `walk` | head and both hands as one rigid body in the plane of the head's yaw, Q/E along world Y | | |
| `move_head` | the head alone, as `walk` | | |
| `move_left`, `move_right` | the hand in the view's plane: forward and back keys up and down, the others sideways, E away, Q toward; `controls.hand_speed` | | |
| `turn_body` | yaw (A/J left) turns the hands around the head, pitch (W/I up) tilts the head and the hands with it | the same, `look_sensitivity` rad a pixel | |
| `turn_head`, `turn_left`, `turn_right` | that device about the world's up and its own side axis | the same | |
| `turn_eyes` | the gaze from the head, up to 35 degrees sideways and 30 up and down; lets go of a point the eyes looked at (`docs/face-tracking.md`) | the same | |
| `left_stick`, `right_stick` | that thumbstick, ramped like the stick bindings; on a tracked hand microgestures | | |
| `push_left`, `push_right` | | | the hand 2 cm a notch along the view's forward |
| `roll_left`, `roll_right` | | | the hand 15 degrees a notch about its grip's forward axis |
| `none` | nothing | | |

A drive a source cannot do does nothing. The move keys drive with any modifier; the turn keys only
without one, as Shift and Ctrl give them to the thumbsticks' bindings.

`controls.profiles` entries: `name` (required, at most 31 characters); `extends` starts from that
profile without its key; `key`; `wasd`, `ijkl`, `mouse`, `shift_mouse`, `wheel`, `shift_wheel`
(drive names; `none` for the Shift ones is the same as without Shift); `hand` (`primary`, `left`,
`right`). An entry of an existing name changes only the fields it names; others are added, at most
16 profiles in all. Pick keys nothing else uses: a profile's key wins over other uses.

### Resets

R puts head and hands back to their start poses; 0 only the hands, at their configured
offset from the level head frame. Both are teleports: the app sees no velocity spike.

Analog inputs (triggers, grips, sticks) move toward the held keys' target at
`1 / analog_ramp` per second and back to zero at `1 / analog_release`; several stick keys
give their normalized sum, so diagonals ramp along the diagonal. Space toggles a freeze:
every analog input held by keys stops where it is until its keys are released (slow
walking at a partial stick); pressed again while something is frozen, all ramps continue.
Profile keys, resets and Space act on the press and ignore modifiers.

T switches a hand between its controller and hand tracking (`SIM_HandSource`): bare the
profile's hand; Shift+T the left, Ctrl+T the right. A tracked hand keeps only the trigger, grip and menu slots: trigger pinches, grip
grabs (both make a fist, grip alone points), menu is the palm-up menu gesture. The stick
slot (Shift or Ctrl + I/J/K/L, or the `stick` profile) swipes the thumb along the index (`XR_META_hand_tracking_microgestures`:
up forward, down backward, left, right) and the primary slot (H) taps it; each press is one
pulse that the app sees for one frame, holding does not repeat.

`props` is an array of prop objects (`docs/props.md`): keyboards and QR codes spawned when the
simulator starts, still or moving; `PUPPETXR_PROPS` replaces it.

`hands.tracking_fov_degrees` (default 120) is the full angle of the cone around the head's
forward axis in which hand tracking sees a tracked hand; outside it the hand is lost unless the
app asked for wide motion mode. Controllers are tracked everywhere.

`body` is the simulated body of body tracking at start and after a reset, in the JSON that
`puppetxrctl body save` writes (`docs/body-tracking.md`): `height` (default 1.75, 0.5 to 3, the
top of the head in the T-pose, the skeleton scales from it), `pose`, `bones`, `joints`, `limbs`.
A body with an unknown name or a value out of range is ignored with a warning.

`face` is the simulated face of face and eye tracking at start and after a reset, in the JSON
that `puppetxrctl face save` writes (`docs/face-tracking.md`): `preset`, `weights`, `visemes`,
`confidences`, `blink` (default on), `gaze`, `look_at`.

Default bindings, every slot on three variants (bare: the profile's hand; Shift: left; Ctrl:
right):

| Key | Slot |
|---|---|
| U, mouse left | trigger |
| O, mouse right | grip |
| H, mouse middle | primary (X/A) |
| Y | secondary (Y/B) |
| N | stick click |
| M | menu (left), system (right) |
| Shift/Ctrl + I/K/J/L, arrows | stick up/down/left/right (bare these are the profile's) |

With both Shift and Ctrl held no default binding fires. A held key follows profile and
modifier changes on the next frame; inputs set through the control API are left alone
unless a key drives them. Controls are disabled during replay; recordings store poses and
inputs, never keys.
