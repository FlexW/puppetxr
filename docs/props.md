# Props

Keyboards and QR codes in the room that the simulated headset tracks: `SIM_Prop` in
`src/sim.h`, at most `SIM_MAX_PROPS` (16). Apps learn about them through tracking extensions
(keyboards: `XR_META_dynamic_object_keyboard`, docs/openxr.md "Dynamic objects", and
`XR_FB_keyboard_tracking`, docs/openxr.md "Tracked keyboards"; QR codes:
`XR_EXT_spatial_marker_tracking`, docs/openxr.md "Spatial entity contexts"); people see them in
passthrough.

## Model

| Field | Meaning |
|---|---|
| `id` | from 1, never reused while the sim lives; set by `sim_add_prop` |
| `kind` | `keyboard`: the `sim_keyboard_model`, +Y up, front at +Z, origin at its bounding box centre, 30 x 1.8 x 11.2 cm. `qr`: a code centred in its XY plane, facing +Z |
| `pose` | stage space: where it is (still), where it starts (linear), the centre it circles (circle) |
| `motion` | `still`, `linear` (back and forth between `pose` and `target` at `speed`), `circle` (horizontal circle of `radius` around `pose`, counter-clockwise seen from above starting at +X, turning with the path, at `speed`) |
| `target`, `radius`, `speed` | meters, meters, meters per second (0 to 100) |
| `size` | QR: the code's edge length without the quiet zone, 0.01 to 10 m |
| `text` | QR: what it encodes, up to 213 bytes (byte mode, level M, versions 1 to 10) |
| `start` | sim time its motion started: `sim_add_prop` and every `sim_update_prop` set it to now |

- `sim_prop_pose(prop, time)` is a pure function of the prop and the time, so props move
  without per-frame updates and replays reproduce them. Path distances are computed in f64.
- Invalid props are refused whole: unknown kind or motion, non-finite or degenerate pose, a
  circle without radius, a QR size out of range, unterminated text.

## Where props come from

- Config `props` (`docs/config.md`) and the environment `PUPPETXR_PROPS` (a JSON array, replacing
  the config's), spawned when the sim is created. Invalid entries are skipped with a warning.
- UI: the Props panel adds a QR code at eye height or a keyboard below it, 1 m in front of the
  head, and edits pose, text, size and motion; every edit restarts the motion.
- C API (`add_prop`, `update_prop`, `remove_prop`, `get_props`), `docs/control-api.md`.
- puppetxrctl `props` and `prop ...`, `docs/control-cli.md`.

A prop object (config, `PUPPETXR_PROPS`, control protocol):

```json
{ "kind": "qr", "text": "https://example.com", "size": 0.15,
  "position": [0, 1.5, -1], "orientation": [0, 0, 0, 1],
  "motion": "linear", "target": [1, 1.5, -1], "speed": 0.2 }
```

`yaw_degrees` may replace `orientation`. `update_prop` applies only the fields given.

## Recording

Props are frame state (`SIM_FrameState::props`), in the recording header's initial state and in
a frame only when they changed since the last frame written
(`docs/recording-format.md`). A frame without props keeps them.

## Passthrough

`comp_set_props` (`src/comp.cpp`) runs every `xrEndFrame` with the frame's display time:

- Geometry in room space, rebuilt per frame into frame-slotted host-visible buffers, drawn with
  the passthrough pipeline right after the room so both share depth.
- Keyboards copy the `sim_keyboard_model` (dark body, light keys). QR codes are a plate: the
  code with its 4-module quiet zone on the front, gray on the back 2 mm behind.
- The atlas: a 72 x 72 cell per prop (one texel per module, `w_qr_encode`, `src/world.cpp`)
  above a row of solid colours, sampled with nearest filtering, uploaded only when a prop's kind
  or text changes. Codes decode with OpenCV's detector from a screenshot.
- Not yet in environment depth (`XR_META_environment_depth`) or the global mesh raycast.
