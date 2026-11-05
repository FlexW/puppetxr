# Control API

`src/puppetxr.h` (installed to `<prefix>/include/puppetxr.h`), implemented in
`src/puppetxr_api.cpp`. C89: no `stdint.h`, no `inline`, no trailing enum commas, no
declarations after statements. Fixed-width types are `PUPPETXR_U32`, `PUPPETXR_I32`,
`PUPPETXR_U64`, `PUPPETXR_I64`, taken from `stdint.h` where available and from the compiler's
64-bit type otherwise. `tests/puppetxr_header.c` compiles the header with
`-std=c89 -pedantic` to keep it that way. The only other dependency is the platform's
dynamic loader header for `puppetxr_load_api`.

From outside the process, the same calls are available over the control server and
`puppetxrctl` (`docs/control-cli.md`).

## Getting the API

The OpenXR loader loads `libpuppetxr` into the app's process. Once an `XrInstance`
exists, any code in that process can get the API:

```c
#include <puppetxr.h>

PUPPETXR_Api puppetxr;
if (puppetxr_load_api(&puppetxr) == PUPPETXR_SUCCESS)
{
    puppetxr.set_input(PUPPETXR_DEVICE_RIGHT_HAND, "trigger/value", 1.0f, 0.0f);
}
```

`puppetxr_load_api` asks for the header's `PUPPETXR_API_VERSION` and finds the already loaded runtime with `dlopen(RTLD_NOLOAD)` (Linux,
macOS) or `GetModuleHandle` (Windows) and never loads it itself, so it returns
`PUPPETXR_ERROR_NOT_LOADED` when the app runs on another runtime. The library exports one
symbol for this, `puppetxr_get_api(version, &api)`, which fills a table of function
pointers. Define `PUPPETXR_NO_LOADER` to leave out the helper and its system headers.

## Versions

```c
PUPPETXR_U32 runtime_version, api_version;
puppetxr_get_version(&runtime_version, &api_version); /* no XrInstance needed */
if (PUPPETXR_VERSION_GET_MINOR(runtime_version) >= 2) { /* behave differently */ }
```

- `puppetxr_get_version` returns the runtime version (`PUPPETXR_MAKE_VERSION`, the same
  number the OpenXR runtime properties report) and the highest API version it serves.
  `PUPPETXR_Api.runtime_version` carries the same runtime version.
- `puppetxr_get_api(v, &api)` fills the lower of `v` and the runtime's API version and
  stores it in `api.version`. It copies only the part of the table that version
  defines, so a client built against an older header is never written past, and one
  built against a newer header gets a smaller table instead of an error. Check
  `api.version` before calling a function added after version 1.
- `PUPPETXR_Status` starts with `struct_size`; set it to `sizeof(PUPPETXR_Status)`. The runtime
  writes only that many bytes.

## Calls

| Function | Effect |
|---|---|
| `get_status` | frame index, simulated time, state hash, record and replay progress, divergences |
| `get_device_pose`, `set_device_pose` | head view pose or hand grip pose in stage space |
| `set_device_active` | tracking on or off; inactive hands make their actions inactive |
| `get_input`, `set_input` | inputs by component name (`trigger/value`, `a/click`, `thumbstick`, ...), clamped to their range |
| `set_fixed_step` | fixed-step or realtime clock |
| `set_controls_enabled` | keyboard and mouse control on or off |
| `recenter` | moves the LOCAL space to the head, yaw only; sessions get `XrEventDataReferenceSpaceChangePending` |
| `set_mirror_view` | left eye, right eye or both in the mirror window |
| `screenshot` | PNG of the last composited frame at native eye resolution |
| `record_start`, `record_stop` | record from the current state (`docs/recording-format.md`) |
| `replay_start`, `replay_stop` | replay a recording made at the same eye resolution and refresh rate |
| `request_exit` | the session goes to STOPPING at the next frame |
| `get_controller`, `set_controller` | simulated controller type: Meta Touch Plus (default), Touch Pro, Oculus Touch or KHR simple; `PUPPETXR_ERROR_BUSY` while recording or replaying |
| `get_room`, `set_room` | the simulated room by name (`docs/world.md`), null or `""` for none; unknown or invalid rooms give `PUPPETXR_ERROR_INVALID_ARGUMENT` and keep the current one; `PUPPETXR_ERROR_BUSY` while recording or replaying. `replay_start` loads the recording's room and fails with `PUPPETXR_ERROR_IO` when it is missing or its files changed |
| `get_control_mode`, `set_control_mode` | the profiles `body`, `head`, `left` and `right`, which were the control modes; `get_control_mode` gives `PUPPETXR_ERROR_NOT_AVAILABLE` in another profile and `PUPPETXR_Status.control_mode` is -1 there |
| `profiler_start`, `profiler_stop` | a zone profile capture into a Chrome trace (`docs/profiling.md`), with or without an instance; `PUPPETXR_ERROR_BUSY` while one runs, `PUPPETXR_ERROR_IO` when the file cannot be made, stop without a capture `PUPPETXR_ERROR_NOT_AVAILABLE` |
| `get_body_joints`, `get_body`, `set_body`, `reset_body` | body tracking (`docs/body-tracking.md`): the 84 joints of `XR_META_body_tracking_full_body` at the predicted display time in stage space and Meta's joint axes, each with tracked and confidence (`PUPPETXR_BodyJoint`); the body as JSON (height, pose, bones, joints, limbs; what differs from standing; a buffer too small is `PUPPETXR_ERROR_INVALID_ARGUMENT` with `out_length` the size it needs); JSON merged onto it, all or nothing, invalid JSON or unknown names `PUPPETXR_ERROR_INVALID_ARGUMENT`; back to the configured body. Floats read back bit-exactly. Recorded per frame; `PUPPETXR_ERROR_REPLAYING` during a replay |
| `get_face_weights`, `get_eye_gazes`, `get_face`, `set_face`, `reset_face` | face and eye tracking (`docs/face-tracking.md`): the 70 weights at the predicted display time with the eyes' and the blinks', where both eyes look (stage space, -Z along the gaze), the face as JSON, JSON merged onto it, back to the configured face; as the body's. Recorded per frame; `PUPPETXR_ERROR_REPLAYING` during a replay |
| `get_profile`, `set_profile` | the control profile by name (`docs/config.md`, Profiles): what keys and mouse drive; an unknown name or a buffer too small gives `PUPPETXR_ERROR_INVALID_ARGUMENT`; `PUPPETXR_Status.profile` names it |
| `reset_poses`, `reset_hands` | head and hands back to their configured start poses, or only the hands in front of the level head; both without a velocity spike |
| `control_key`, `control_mouse` | a key (name as in `docs/config.md`) down or up, relative mouse motion in pixels: the same path as the mirror window's events, so profiles, modifiers, ramps and freeze behave as for a person; the mouse turns as the captured mouse does. Keys act once per frame; a key pressed and released between two frames only triggers the press actions (profiles, resets, freeze) |
| `set_disabled_extensions` | comma separated extension names never offered, on top of `disabled_extensions` in the config; works without an instance, applies to enumeration and the next `xrCreateInstance` |
| `get_anchors` | the app's stored anchors for the simulated user, local store first, then cloud anchors the user may load, as `PUPPETXR_Anchor` (uuid, room, pose in room space, cloud flag, owner); `out_count` is the total, at most `capacity` are written |
| `wipe_anchors` | empties the local store; non-zero `include_cloud` also deletes the user's cloud anchors; `PUPPETXR_ERROR_BUSY` while recording or replaying |
| `set_anchor_fault` | the next `count` requests of an `PUPPETXR_AnchorOp` complete with `xr_result`, a negative `XrResult`; count 0 clears; `PUPPETXR_ERROR_BUSY` while recording or replaying |
| `set_anchor_timing` | frames until anchor requests complete, frames a loaded anchor stays unlocalized; `PUPPETXR_ERROR_BUSY` while recording or replaying |
| `get_origin_offset` | where the stage origin lay in the room at instance creation (`docs/anchors.md`); under tracking drift it lies at this times the drift |
| `set_tracking_drift`, `set_tracking_drift_rate`, `get_tracking_drift` | tracking drift (`docs/anchors.md`, Tracking drift): the tracked STAGE origin moves in the room while the head, hands, props, body, room and anchors stay, so the app sees them shift by the drift's inverse in STAGE, LOCAL, LOCAL_FLOOR and STATIONARY, with no reference space event. The drift is a pose in this API's stage space, a yaw about +Y and a translation in metres; null resets it. The rate adds `velocity` (m/s, null for none) and `yaw_degrees_per_second` at every frame with its clock step, so under the fixed step it is deterministic; zero stops it and keeps the drift reached. A non-finite pose, a quaternion of length outside [0.5, 2] or one tilted more than 1e-4 of its length in x and z, and non-finite rates are `PUPPETXR_ERROR_INVALID_ARGUMENT`. `get_tracking_drift` fills any of its non-null outputs (`out_velocity` 3 floats). Recorded per frame, so allowed while recording; `PUPPETXR_ERROR_REPLAYING` during a replay |
| `set_frame_fault` | the next `count` calls of an `PUPPETXR_FrameOp` (`PUPPETXR_FRAME_OP_WAIT` 0, `_BEGIN` 1, `_LOCATE_VIEWS` 2, `_END` 3) that pass their own checks return `xr_result` without doing their work (`docs/openxr.md`, Frame faults): a failed wait steps no frame, a failed begin begins none (`xrEndFrame` is then `XR_ERROR_CALL_ORDER_INVALID`, and the next wait does not wait for that frame), a failed locate returns no views, a failed end leaves the frame begun. The session state never changes. `xr_result` must be an error the OpenXR registry lists for that function, else `PUPPETXR_ERROR_INVALID_ARGUMENT`, as is an unknown op; count 0 clears. Works without an instance, for the next one; cleared when the instance is destroyed; `PUPPETXR_ERROR_BUSY` while recording or replaying (the faults pending when a recording starts are in it, and its replay fails the same calls) |
| `get_hand_source`, `set_hand_source` | `PUPPETXR_HandSource` of a hand: its controller or hand tracking. A tracked hand has only `trigger/value` (pinch), `squeeze/value` (grab), `menu/click` and the microgestures `swipe_left_meta/click`, `swipe_right_meta/click`, `swipe_forward_meta/click`, `swipe_backward_meta/click`, `tap_thumb_meta/click` (set to 1, each is a pulse the app sees for the next frame); switching zeroes the rest. Recorded per frame, so allowed while recording; `PUPPETXR_ERROR_REPLAYING` during a replay |
| `get_virtual_keyboard`, `get_virtual_keyboard_key`, `type_text` | the app's `XR_META_virtual_keyboard` (`PUPPETXR_VirtualKeyboard`: exists, shown, shift, stage pose, scale, the end of the app's text before the cursor), where a key's top is (`a`, `1`, `,`, `backspace`, `enter`, `shift`, `space`; `PUPPETXR_ERROR_NOT_AVAILABLE` without a keyboard), and text typed into the shown keyboard at the next frame (UTF-8, `\b` backspace, `\n` enter, at most 256 bytes a frame; `PUPPETXR_ERROR_NOT_AVAILABLE` unless shown). Typed text is recorded per frame; `PUPPETXR_ERROR_REPLAYING` during a replay |
| `add_prop`, `update_prop`, `remove_prop`, `get_props` | props, `docs/props.md`: keyboards and QR codes the headset tracks, as `PUPPETXR_Prop` (kind, motion, pose, target, radius, speed, size, text; `get_props` adds the id and where it is now). An invalid prop, a full list (16) or an unknown id is `PUPPETXR_ERROR_INVALID_ARGUMENT`; a change restarts the motion. Recorded per frame; `PUPPETXR_ERROR_REPLAYING` during a replay. `get_props` writes each element `out_props[0].struct_size` apart |
| `point_hand` | moves and turns a hand so a ray starts at the eye of view 0 (left) or 1 and passes through (u, v) of that view's image, 0 0 top left; u and v outside [0, 1] or a ray quaternion whose length is outside [0.5, 2] are `PUPPETXR_ERROR_INVALID_ARGUMENT` (others are normalized). The ray is a pose in grip space looking down -Z, null the aim pose. `puppetxrctl point` (`docs/control-cli.md`); `PUPPETXR_ERROR_REPLAYING` during a replay |
| `set_log_callback` | every runtime log message that passes the log level, as `PUPPETXR_LogLevel` and text, on the logging thread; null removes it. Works without an instance. A non-zero `replay_history` first passes the kept lines (`OS_LOG_HISTORY`), oldest first, so messages of `xrCreateInstance` are not lost. The app's own captured stdout/stderr is not passed. Return quickly and do not call into OpenXR or puppetxr; lines the callback logs through puppetxr are kept but not passed again. A good place for a breakpoint on runtime errors |

Poses of this API are in stage space as it lay at instance creation, the simulator's frame:
STAGE as the app sees it until tracking drift moves STAGE away from it.

Every call takes the runtime lock, so it is safe from any thread; changes take effect
at the next `xrWaitFrame`. While a replay drives the simulator, calls that would
change simulator state return `PUPPETXR_ERROR_REPLAYING`.

## Errors

| Result | Meaning |
|---|---|
| `PUPPETXR_ERROR_NOT_LOADED` | the runtime is not loaded in this process |
| `PUPPETXR_ERROR_VERSION` | the runtime is older than the header |
| `PUPPETXR_ERROR_NO_INSTANCE` | no `XrInstance` is alive |
| `PUPPETXR_ERROR_INVALID_ARGUMENT` | null pointer, unknown device or input, non-finite value, degenerate quaternion, or a record or replay already in the wrong state |
| `PUPPETXR_ERROR_REPLAYING` | a replay owns the simulator state |
| `PUPPETXR_ERROR_IO` | a file could not be read or written, or a recording does not fit this instance |
| `PUPPETXR_ERROR_NO_GRAPHICS` | screenshots need a session with a graphics binding |
| `PUPPETXR_ERROR_BUSY` | the call is not possible while recording or replaying |
| `PUPPETXR_ERROR_NOT_AVAILABLE` | the app has nothing of the kind, such as a shown virtual keyboard |

## Compatibility

Binding from puppetxr 1.0 on (see rule 11 in `AGENTS.md`):

- Functions are appended to `PUPPETXR_Api` and fields to `PUPPETXR_Status`; nothing is
  removed, reordered or redefined. Each addition bumps `PUPPETXR_API_VERSION` and adds a
  case to `api_table_size` in `puppetxr_api.cpp`.
- Enum values are appended and never renumbered. Each enum ends in a `*_MAX_ENUM =
  0x7fffffff` sentinel that keeps it 32 bit; it is never a valid argument.
- Input, device and controller names never change.
- A behaviour change that clients might depend on goes behind a new API version or a
  new function, never into an existing one.

Before 1.0 the API may still change; the version is 1 until the first release.
