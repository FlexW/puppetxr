# Control server and puppetxrctl

A running simulator can be driven from outside its process: inputs, poses, controller type,
recenter, screenshots, recordings, frame waits. `puppetxrctl` is the command line client; agents
and test scripts can use it, or speak the protocol directly.

Compatibility: client and simulator must be the exact same puppetxr version
(`major.minor.patch`). The handshake refuses anything else. The protocol may change in any
release.

## Enabling it

Off by default. Turn it on per run with `PUPPETXR_CONTROL=1`, or with `control` in the config
(`docs/config.md`):

```json
"control": { "enabled": true, "port": 0, "address": "127.0.0.1", "paused": false }
```

- `port` 0 lets the system pick a free port, so several simulators can run side by side.
- `address` 127.0.0.1 only accepts local clients. Any other address (`0.0.0.0` for multi-machine
  multiplayer tests) lets everyone who can reach that port drive the simulator; the log warns.
- `paused` starts the instance paused (see Pause and step).
- Env overrides: `PUPPETXR_CONTROL`, `PUPPETXR_CONTROL_PORT`, `PUPPETXR_CONTROL_ADDRESS`, `PUPPETXR_CONTROL_PAUSED`.

The server lives from `xrCreateInstance` to `xrDestroyInstance`.

## Discovery

Each listening simulator writes `<runtime dir>/puppetxr/<pid>.json`:

```json
{"pid":4711,"port":41825,"address":"127.0.0.1","app":"hello_xr","version":"0.1.0"}
```

`<runtime dir>` is `$XDG_RUNTIME_DIR` (per user, emptied at logout), else `/tmp`; on Windows
the user's temporary directory (`GetTempPathW`).

The file holds an exclusive `flock` while the simulator runs (`os_lock_file_create`); on
Windows an exclusive `LockFileEx` byte range past the end of the file, held through a read-only
handle so readers still open it. It is written under a temporary name and renamed once locked,
so a visible file always had a holder. A reader holding the last file of the path (a
deleted one, or one opened without delete sharing, as `fopen` does) makes the rename fail with
error 5 until it closes it, so the rename is retried for up to about 200 ms: Unity's next
instance otherwise ran without a discovery file. Sockets are Winsock there, bound with
`SO_EXCLUSIVEADDRUSE` (with `SO_REUSEADDR` another process could take the port over).

- A clean `xrDestroyInstance` deletes it.
- A crash, `kill -9` or Ctrl+C leaves it behind without a holder: the kernel drops the lock
  however the process ends. `puppetxrctl list` and every simulator start treat such a file as
  stale and delete it (`os_lock_file_is_stale`). Pids can be reused, so the lock decides, not
  the pid.

## puppetxrctl

```
puppetxrctl list
puppetxrctl [--target <pid|app|host:port>] [--json] [--timeout <ms>] <command> [args]
```

| command | does |
|---|---|
| `list` | running simulators: pid, address, version, app |
| `status` | frame, time, state hash, recording and replay state, controller, room, controls, control mode, profile, refresh rate, color space and local dimming the app chose, `boundary_suppressed`, `mouse_captured` (a click into the mirror window took the mouse) |
| `pose <device> [x y z [qx qy qz qw]]` | get, or set in stage space; devices `head`, `left`, `right` |
| `point left\|right <u> <v> [left\|right] [grip <pitch>] [ahead <m>]` | move and turn the hand so a ray starts at the eye (default left) and passes through (u, v) of that eye's screenshot, 0 0 top left, 1 1 bottom right; so a UI element picked from a screenshot is hit at any distance. The ray is the aim pose, or with `grip` the grip turned `pitch` degrees up. The aim pose is the grip turned 60 degrees down, where Meta's OVRPlugin casts its UI ray and its controller pose points (`grip -60` is the same ray without the aim's 5 cm). `ahead` (0 to 10) starts the ray that far from the eye along it, the hand in front of the face rather than in it, for apps that hide a ray starting at the eye |
| `active <device> on\|off` | tracked or not |
| `haptic left\|right [main\|trigger\|thumb]` | what the app plays on that output (`main` is `/output/haptic`, the others Touch Pro's `haptic_trigger_fb` and `haptic_thumb_fb`): `active`, `amplitude`, `frequency`, `remaining_s` |
| `input <device> <input> [x [y]\|on\|off]` | get or set an input, names as in `docs/config.md` |
| `controls on\|off` | keyboard and mouse control |
| `fixed-step on\|off` | clock mode |
| `recenter` | LOCAL to the head |
| `mirror left\|right\|both` | mirror and screenshot view |
| `screenshot <path>` | PNG of the last composited frame, written by the simulator; `not_available` before the first frame, as the simulator reads frames back only in `xrEndFrame`: step a frame first |
| `wait <frames>` | returns after that many more frames (timeout at least 60 s) |
| `pause`, `resume` | hold the app in `xrWaitFrame`, or let it run |
| `step <frames>` | run exactly that many frames, then stay paused; replies once they ran |
| `frame fault wait\|begin\|locate\|end <XrResult> [count]` | the next frame calls (default 1) of `xrWaitFrame`, `xrBeginFrame`, `xrLocateViews` or `xrEndFrame` fail with that `XrResult`, an error the call may return, e.g. -2 runtime failure, -17 session lost; count 0 clears (`set_frame_fault`, `docs/openxr.md`, Frame faults). The server answers in `xrWaitFrame` and `xrPollEvent`, so a wait fault may fail the wait that answered it |
| `record start <path>`, `record stop`, `replay start <path>`, `replay stop` | as in the C API |
| `controller [meta_touch_plus\|meta_touch_pro\|oculus_touch\|khr_simple]` | get or set |
| `hand left\|right [controller\|hand]` | get or set what tracks the hand (`get_hand_source`, `set_hand_source`); a tracked hand pinches with `input <hand> trigger/value`, grabs with `squeeze/value` |
| `body` | the 84 body tracking joints: name, position and orientation in stage space, tracked, confidence (`get_body_joints`, `docs/body-tracking.md`) |
| `body get`, `body set <json>`, `body reset` | the body as JSON, JSON merged onto it, back to the configured body (`get_body`, `set_body`, `reset_body`) |
| `body save <file>`, `body load <file>` | the body as JSON into a file of this machine; the configured body with the file's on top, in one request |
| `body height <m>`, `body pose stand\|t_pose\|a_pose\|arms_forward\|sit`, `body bone <joint> <scale>` | the shape and the pose; a bone scale lengthens the bones from the joint to its children |
| `body joint <joint> rot <x y z w>`, `body joint <joint> tracked on\|off`, `body joint <joint> confidence <c>` | a joint's rotation relative to the T-pose in the body's axes (an arm or leg joint poses its limb), whether it is tracked, its confidence |
| `body arm\|leg left\|right follow\|posed` | an arm follows its controller and a leg the floor, or the limb holds its pose |
| `face` | the 70 face weights and where the eyes look (`get_face_weights`, `get_eye_gazes`, `docs/face-tracking.md`) |
| `face get`, `face set <json>`, `face reset`, `face save <file>`, `face load <file>` | as for `body` |
| `face preset neutral\|smile\|frown\|surprise\|eyes_closed\|jaw_open`, `face weight <expression> <w>`, `face viseme <viseme> <w>` | the expression |
| `face blink on\|off`, `face gaze <yaw> <pitch>`, `face look <x y z>`, `face look ahead` | blinks every 4 s, the gaze in degrees from the head, the eyes on a point in stage space, or back to the gaze |
| `props` | the props: id, kind, motion, pose, target, radius, speed, size, text, and where each is now (`get_props`) |
| `prop add qr <text> <x y z> [qx qy qz qw]`, `prop add keyboard <x y z> [qx qy qz qw]` | a QR code (facing its +Z, 10 cm) or a keyboard in stage space (`add_prop`); prints the id |
| `prop move <id> <x y z> [qx qy qz qw]`, `prop motion <id> still\|linear <x y z> <speed>\|circle <radius> <speed>`, `prop size <id> <m>`, `prop text <id> <text>`, `prop remove <id>` | change or remove a prop (`update_prop` with only the given fields, `remove_prop`) |
| `vkb` | the app's virtual keyboard: exists, visible, shifted, pose, scale, text context (`get_virtual_keyboard`) |
| `vkb key <name>` | where a key's top centre is in stage space, oriented as the keyboard (`get_virtual_keyboard_key`); names `a`..`z`, `0`..`9`, `,`, `.`, `backspace`, `enter`, `shift`, `space` |
| `vkb type <text>` | types into the shown keyboard at the next frame (`type_text`); words join with one space, `\n` is enter, `\b` backspace, `\\` a backslash |
| `profile [name]` | get or set the control profile: what keyboard and mouse drive (`docs/config.md`, Profiles) |
| `profiler start <path>`, `profiler stop` | a zone profile capture of the running app into a Chrome trace (`docs/profiling.md`); the path is the app's |
| `mode [body\|head\|left\|right]` | the profiles that were the control modes |
| `reset [hands]` | head and hands to their start poses without velocity, or only the hands in front of the head |
| `key <name> down\|up` | press or release a key exactly as in the mirror window; keys act each frame, so hold one across `wait`/`step` to move, ramp or freeze |
| `mouse <dx> <dy>` | relative mouse motion in pixels; turns as the captured mouse does |
| `lens mask on\|off` | the lens mask (`views.lens_mask`) while running; apps get the visibility mask changed event |
| `ui show\|hide` | the mirror window's panels, as `controls.ui` (Tab) toggles them |
| `ui find <label>` | `x` and `y` of the panel item with this label in the last frame, in window pixels, whether the mouse was on it (`hovered`) and where the UI had the mouse (`mouse`); `not_available` without one |
| `ui click <label>` | left click on the panel item with this label in the last frame (the words after `click` are one label, as shown: `motion`, `add QR code`, `circle` in an open drop-down), sent through the window's events as a person clicks; `not_available` without a window or such an item. Needs `PUPPETXR_CONTROL`, which records the items' places (`ui_set_probe`) |
| `room [name\|none]` | get or set |
| `anchors` | the app's stored anchors: `count` and up to 64 with uuid, room, cloud, owner and pose in room space |
| `anchors wipe [cloud]` | empty the local store; with `cloud` also the user's cloud anchors |
| `anchors fault save\|erase\|load\|share <XrResult> [count]` | the next requests (default 1) fail with that negative `XrResult`, e.g. -1000169002 network timeout, -1000169001 localization failed; count 0 clears |
| `anchors timing <latency> <localize>` | frames until anchor requests complete, frames a loaded anchor stays unlocalized |
| `origin` | where the stage origin lay in the room at instance creation |
| `drift` | the tracking drift (`get_tracking_drift`, `docs/anchors.md`): `position`, `orientation`, `yaw_degrees` of the tracked STAGE origin in stage space as it was at creation, and its rate `velocity` (m/s) and `yaw_rate` (degrees/s) |
| `drift <x y z> [yaw]` | move the tracking origin now to that place, metres and degrees about +Y (`set_tracking_drift`); the head, the room and anchors stay, so the app sees them move by the inverse |
| `drift rate <x y z> [yaw]` | drift at that rate every frame, m/s and degrees/s (`set_tracking_drift_rate`); `drift rate 0 0 0` stops and keeps the drift reached |
| `drift reset` | no drift and no rate |
| `exit` | the session goes to STOPPING |
| `send <json>` | one raw request |
| `shell` | commands or raw JSON lines from stdin, JSON replies on stdout; one connection, made once the target listens and moved to its next instance's server when the first goes away, each within `--timeout` (Unreal makes two instances, with no simulator between them); it gives up at once when a process id target has exited, whose discovery file may outlive it |

- Without `--target` (or `PUPPETXR_TARGET`), the only running simulator is used. With several, the
  CLI lists them and fails. A number picks a pid, `host:port` connects directly (remote
  simulators), anything else matches the app name.
- Relative paths are made absolute against the CLI's directory; the simulator reads and writes
  them.
- Output: `key: value` lines, or the reply line with `--json`.
- Exit codes: 0 done, 1 the simulator refused the command, 2 usage, 3 no simulator, ambiguous
  target, version mismatch, lost connection or timeout.

## Protocol

TCP, one JSON object per line in each direction (`\n` terminated, at most 4095 bytes per
request).

- Request: `{"id": <int>, "cmd": "<name>", ...}`. `id` is echoed, so a client may pipeline.
- Reply: `{"id": <int>, "ok": true, ...}`, or
  `{"id": <int>, "ok": false, "error": "<code>", "message": "..."}`.
- Error codes: `bad_request`, `hello_first`, `version`, `unknown_command`, and the C API
  results `no_instance`, `invalid_argument`, `replaying`, `io`, `no_graphics`, `busy`,
  `not_available`.
- The first request must be `{"cmd": "hello", "version": "<major.minor.patch>"}`. A mismatch is
  answered with `version` and the connection is closed. The reply carries `version`, `pid`,
  `app` and `frame`.

Commands mirror `PUPPETXR_Api` (`docs/control-api.md`), plus `get_haptic` (`device`, `output` default `main`): `status`, `get_pose`/`set_pose`
(`device`, `position`, `orientation`), `set_active` (`device`, `active`), `get_input`/
`set_input` (`device`, `input`, `value`: number, bool or `[x, y]`), `set_controls`/
`set_fixed_step` (`enabled`), `recenter`, `set_mirror_view` (`view`), `screenshot` (`path`),
`record_start`/`replay_start` (`path`), `record_stop`, `replay_stop`, `request_exit`,
`get_controller`/`set_controller` (`controller`), `get_room`/`set_room` (`room`),
`get_control_mode`/`set_control_mode` (`mode`: `body`, `head`, `left`, `right`), `get_profile`/
`set_profile` (`profile`), `reset_poses`,
`reset_hands`, `key` (`key`, `down`), `mouse` (`delta`: `[dx, dy]`), `show_ui` (`shown`), `set_lens_mask` (`on`), `ui_click` (`label`), `ui_find` (`label`; replies `x`, `y`, `hovered`, `mouse`), and
`get_anchors`, `wipe_anchors` (`cloud`), `set_anchor_fault` (`op`: `save`, `erase`, `load`,
`share`; `result`; `count`), `set_frame_fault` (`op`: `wait`, `begin`, `locate`, `end`; `result`;
`count`, default 1), `set_anchor_timing` (`latency_frames`, `localize_frames`),
`get_origin_offset`, `get_tracking_drift` (replies `position`, `orientation`, `yaw_degrees`, `velocity`,
`yaw_rate`), `set_tracking_drift` (`position`, then `orientation` or `yaw_degrees`; each optional, none
removes the drift), `set_tracking_drift_rate` (`velocity`, `yaw_rate`; each optional, default 0),
`reset_tracking_drift` (drift and rate at once), `get_hand_source`/`set_hand_source` (`device`, `source`: `controller`,
`hand`), `get_virtual_keyboard`, `get_virtual_keyboard_key` (`key`), `type_text` (`text`),
`get_props`, `add_prop` (a prop object, replies `prop`), `update_prop` (`prop` and the fields
to change), `remove_prop` (`prop`), and
`wait_frames` (`frames`; replies with the frame reached), `pause`, `resume`, `step` (`frames`).
`status` reports `paused`, `control_mode` (empty in a profile of another name) and `profile`.

## Pause and step

- `pause` holds the app at its next `xrWaitFrame`. The server keeps serving clients from in
  there, so commands, screenshots and `step` work while paused; the runtime is unlocked in
  between, so other threads of the app go on with the frames in flight.
- `step n` adds n frames to run and replies when the last of them committed and every waited
  frame ended (`oxr_frames_in_flight`): an engine that ends frames on another thread than the one
  waiting (Unreal's render thread) still has the last one in flight when the app waits for the
  next, and a screenshot then showed the frame before in some runs. An app that never ends a
  waited frame gets the reply 1 s later. `wait_frames` replies the same way.
- `wait_frames` while paused waits until something resumes or steps.
- `control.paused` (`PUPPETXR_CONTROL_PAUSED=1`) starts every instance paused. A driver can then
  connect and take every frame from the first one, which makes time-dependent scenes
  reproducible.
- When the last client disconnects while paused, the simulator resumes, so a crashed driver
  never leaves the app hanging. A paused driver therefore keeps one connection open
  (`puppetxrctl shell`) instead of one per command.

## Server (`src/puppetxr_api.cpp`)

- Registered with `oxr_set_control_hooks` when the library loads; the oxr layer calls it at
  instance creation and destruction, and first thing in `xrWaitFrame` and `xrPollEvent`.
- Single-threaded. Sockets are non-blocking and polled only from those calls, with the
  runtime lock held. Commands therefore land between frames, before the frame's sim step, and
  recordings capture their effect like any other state change.
- An app that does not run frames or poll events does not answer; the CLI times out with a
  message saying so.
- Up to `API_MAX_CLIENTS` connections. A pending `wait_frames` holds back that client's later
  requests, so replies stay in order. Output the socket does not take is buffered and flushed
  on the next poll.
- Sockets and the discovery file are close-on-exec, so child processes of the app neither keep
  the port nor the lock.
