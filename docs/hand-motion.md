# Hand motion (planned)

Status: not built. Recording real hands comes first (`docs/recorder.md`); every number this
plan leaves open is fitted from those takes.

Simulated tracked hands move and shape like a person's, so an agent can test hand interactions
from `puppetxrctl` without a headset. A **gesture** is a JSON file: timed steps in which a hand
changes its shape and follows a path. Shipped gestures and a user's own go through the same
loader, checker and player; the shipped ones are embedded in the library. The same system
plays recorded clips, and later body motion.

## Why

Today a tracked hand has one shape family, `hand_shape(pinch, grab)` in `src/sim.cpp`: four
equal finger curls and a thumb blended between three fitted poses, driven by `trigger/value` and
`squeeze/value`. Its bones are hand-made constants (`sim_fingers`, `sim_thumb_*`), it never
shakes, misses or drops out, and it reaches a target in one frame unless keys move it. Apps
tuned on real hands (pinch thresholds with hysteresis, velocity estimates for throws, poke
debouncing) are not exercised.

## Layers

| Part | Where | Why there |
|---|---|---|
| Hand model: skeleton, joint limits, shape to joints, pinch and grab strength from joints | sim (`src/sim.cpp`) | the joints are sim state, located by oxr and recorded |
| Gesture loader, checker, expressions, paths, noise, player | sim (`src/sim.cpp`, section `Gestures`, `sim_gesture_*`) | runs in `sim_begin_frame` after the controls, in the fixed device order; never during replay |
| Built-in gestures, fitted hand model | `motion/` at the top (like `rooms/`), embedded by `cmake/embed_motion.cmake` | JSON in the repository, one file per gesture, no code change to add one |
| Users' gestures | files on disk, loaded at startup and at runtime (Loading) | the same loader and checker as the built-ins |
| Play, stop, list, check, events | control API, `puppetxrctl gesture`, C API, UI panel | how agents and tests drive it |

## Hand model (sim)

Fitted by `tools/hand_fit.py` (Python standard library only) from the calibration takes into
`motion/hand_model.json`, which replaces the constants in `src/sim.cpp`:

- **Skeleton**: the joint offsets of the open hand in the wrist frame and every bone length,
  the median over `still_open`. The hand scale override (`TODO.md` 8b) scales them.
- **Joint limits and coupling**: per finger joint flexion range; the distal joint follows the
  middle one (ratio fitted from `curl_each`); ring and little finger pulled along by the middle
  one (coupling fitted, so a fist and a point both look right).
- **Shape**: `SIM_HandShape` grows from `curls[4]`, `thumb[5]` to per finger curl, spread and a
  thumb pose (`open`, `pinch:<finger>`, `fist`, `side`, `up`, or five angles); joints are
  `hand_local` of the shape. Solved shapes (Motion, Shapes) produce the same struct.
- **Strengths from joints**: pinch strength per finger as a function of tip distance, fitted
  against the recorded `XrHandTrackingAimStateFB` pinch strengths; grab from the mean curl.
  `trigger/value`, `squeeze/value` and the aim state's strengths read from the joints, so a
  gesture that does not fully close a pinch reads as a weak one. The keyboard keeps working:
  a key sets a target shape (pinch 1 closes thumb and index until strength 1).
- **Relaxed pose**: `idle` from `still_relaxed`: slightly curled, more toward the little finger.

Tests (`tests_sim.cpp`): bone lengths and the open hand within 2 mm of `motion/takes/still_open`;
each built-in shape's pinch strength within 0.05 of the take that shows it; the keyboard pinch
still reads 1.

## Gesture format

```json
{
  "format": "puppetxr-gesture",
  "version": 1,
  "name": "pinch_tap",
  "description": "Reach to a point, pinch, hold, release, return",
  "hands": ["right"],
  "params": {
    "target": { "type": "point", "default": "head + forward * 0.4 + down * 0.15" },
    "hold":   { "type": "seconds", "default": 0.2, "min": 0, "max": 10 }
  },
  "steps": [
    { "hand": "right", "anchor": "pinch", "to": "target", "bow": 0.03, "duration": 0.6,
      "palm": "down", "shape": "idle" },
    { "hand": "right", "shape": { "pinch": "index" }, "duration": 0.12, "event": "pinch" },
    { "wait": "hold" },
    { "hand": "right", "shape": "idle", "duration": 0.15, "event": "release" },
    { "hand": "right", "path": "home", "duration": 0.7 }
  ],
  "noise": { "jitter": 1, "tremor": 0, "sloppiness": 0 }
}
```

- **Steps** run in order; `"with": "previous"` runs a step alongside the one before (two hands,
  or a shape change during a path). A step has a `duration` in seconds or a `speed` in m/s.
- **Expressions** for points, directions, numbers: `+ - * /`, parentheses, `vec(x, y, z)`,
  params and built-ins: `head`, `forward`, `up`, `down`, `outward`, `inward` (per hand), `home`
  (the hand's rest place before the gesture), `start` (where the step starts), `left.<anchor>`,
  `right.<anchor>`. All are evaluated once when the gesture starts, so the whole motion is known
  up front and a replayed or reseeded run moves the same way apart from noise.
- **Anchor**: the point of the hand that follows the path: `grip`, `palm`, `pinch`, `index_tip`,
  `wrist`.
- **Paths**: `to` a point with an optional `bow` (metres, sideways and up), `circle` (centre,
  axis, turns), `twist` (degrees about the anchor's axis), `shake` and `swing` (pivot, amplitude,
  frequency), `curve` (points walked at an even pace, Catmull-Rom), `throw` (below), `home`.
- **Orientation**: `palm` `down`, `up`, `in`, `out`, `forward`, `toward:<point>`, or a quaternion;
  slerped over the step. Unset, the hand keeps its orientation.
- **Easing**: minimum jerk for reaches (default), `linear`, `ease_in`, `ease_out`.
- **Events**: a named event at a step's start or end; the player reports it with its frame.

### Shapes

`"idle"`, `"open"`, `"fist"`, `"point"`, `{ "pinch": "index" }`, `{ "curls": [i, m, r, l],
"thumb": "pinch:index" }`, and solved shapes: `{ "around": "handle", "radius": 0.02 }` (cylinder
along the palm's width), `{ "around": "ball", "radius": 0.05 }`, `{ "around": "dial", "radius":
0.03 }`: each finger curls until its tip touches the surface, then a small search places the
thumb against it. A solved shape has a `contact` value 0..1 (how much of each tip touches), so a
solved grasp reads as a firm grab.

### Throw

A straight, slightly upward stroke from behind to in front of the shoulder toward `target`: it
starts slow and accelerates to peak speed exactly at the `release` event, then stops quickly. The
hand stays behind the object with the palm toward the target and the wrist held still; the fingers
spring open a fitted lead time before the release (about 35 ms is the hypothesis to check against
`throw_*` takes), so grip strength drops below an app's threshold at top speed. Peak speed is a
param (default fitted).

### Noise

Seeded per play (`seed`, default from the recording's seed so a replay matches):

- `jitter`: smooth position and rotation noise of the wrist and tips, a few Hz plus about 11 Hz,
  amplitude and spectrum fitted from `still_open`; `0` off, `1` as measured.
- `tremor`: optional 9 Hz tremor, amplitude a param.
- `sloppiness` 0..1: missed targets (offset drawn per reach), overshoot with correction, pinches
  that do not fully close, per-run speed and path variation.
- `dropouts`: tracking loss as measured in `fast_swipe`, `cross`, `out_of_view`: per frame chance
  at speed, during overlap, and the existing FOV cone; a lost hand reports untracked and returns
  where the motion has gone on.

### Clip steps

A step can play recorded motion: `{ "hand": "right", "clip": "wave_right", "from": 1.2, "to": 3.0,
"relative": "head" }`. `clip` names a take (built-in, in `motion/takes/`) or a file path; the
loader converts it to wrist poses relative to the head at the take's first frame and joint
rotations relative to their parents (the take's bone lengths are dropped, the sim skeleton
kept), resampled to the sim's frames. So a user records their own gesture with the recorder,
pulls the take and plays it as is, or inside a gesture between procedural steps. Body clips
later use the same step with `"body"` instead of `"hand"`.

## Loading

Built-ins and users' gestures are the same JSON, loaded by the same `sim_gesture_load`; a
user's file of a built-in's name replaces it for that run.

- At startup: every `*.json` of the directories in `gestures.paths` (config, `docs/config.md`) and
  `PUPPETXR_GESTURE_PATH` (separated by `:`, `;` on Windows), in that order, after the built-ins. A
  file that fails the checker is logged with its errors and skipped; the others load.
- At runtime: `puppetxrctl gesture load <file|dir>` (`load_gesture` in the C API), `gesture play
  <file>` loads and plays a file once without keeping it. Relative paths are made absolute by the
  CLI, as for `model load`.
- Takes (`docs/recorder.md`) load the same way and are named by their file name, so a clip step
  finds them; `gesture play <take.json>` plays a take on its recorded hands.
- Names are `[a-z0-9_-]`, at most 63 bytes, from the file name without `.json`.
- A recording stores the joints a gesture made, not the gesture, so it replays without the files.

## Controllers

A gesture drives a hand held by its controller as well: paths and orientation move the grip
(anchors `grip`, `aim`, `tip`: the controller's front end, for poking), and the shape's pinch and
grab strengths set `trigger/value` and `squeeze/value` (with their touch and click inputs as the
Touch controllers report them). Other inputs are set by a step's `inputs`: `{ "a/click": 1,
"thumbstick": [0, 1] }`, held until a later step changes them, released when the gesture ends.
Noise applies to the pose as for a tracked hand. Controller-only built-ins: `controller_poke`,
`controller_point_and_click`, `controller_grab`, `controller_throw`; the hand built-ins that only
need pinch, grab and a path play on a controller too. A hand step that needs fingers (a solved
grasp, `point`, a microgesture) on a controller hand is a checker error unless the gesture says
`"source": "hand"`, which switches the hand to tracked.

## Library (built-ins)

Each built-in is `motion/gestures/<name>.json`, has params with defaults that work in front of
the head, and is checked against at least one take of the same motion (Tests).

| Group | Gestures |
|---|---|
| select | `pinch_tap`, `pinch_hold`, `double_pinch`, `middle_pinch`, `poke` (press a point with the index tip), `double_poke`, `point_and_pinch` (aim then select, as Meta's system pointer) |
| drag | `pinch_drag` (pinch, move to a point, release), `pinch_scroll` (vertical), `swipe_left`, `_right`, `_up`, `_down` (open hand, quick) |
| grab | `grab` (solved around a ball or handle at a point), `grab_move`, `release`, `throw_overhand`, `throw_underhand`, `pull_lever`, `turn_wheel`, `turn_dial` |
| two hands | `two_hand_scale`, `two_hand_rotate`, `clap` |
| poses | `fist`, `open_palm`, `point`, `thumbs_up`, `ok`, `victory`, `palm_up` (wrist menus), `look_at_palm` |
| social | `wave`, `beckon` |
| system | `system_gesture` (palm toward the face, pinch: Meta's menu gesture), microgestures (`thumb_tap`, `thumb_swipe_*` on the index) |
| must trigger nothing | `idle`, `talking_hands`, `scratch_head`, `hands_down`: an app test asserts no pinch, grab or poke event fires |

## Recording (rec)

Recordings store the resulting joints, so a replay needs neither the gesture nor the motion
layer, and a later change to the player cannot change an old replay. Format version 6: per hand
device, after `source`, when `source` is 1: `u8 shape present` (1 when the joints changed since
the last frame written) and then 26 joint rotations relative to their parents (`f32` x, y, z, w)
plus `f32` pinch and grab strengths. The wrist is the existing `pose` field. Versions 1 to 5 read
as today, the joints from the inputs. About 840 bytes a frame while both hands move.

## Control API and CLI

| `puppetxrctl` | C API (`src/puppetxr.h`, appended) | does |
|---|---|---|
| `gesture list` | `list_gestures` | built-ins and loaded files, their params |
| `gesture show <name>` | `get_gesture` | the JSON |
| `gesture load <file\|dir>` | `load_gesture` | adds or replaces users' gestures (Loading) |
| `gesture check <file>` | `check_gesture` | errors with JSON paths: `steps[2].to: unknown name 'targte'` |
| `gesture play <name\|file> [hand] [k=v ...] [seed <n>]` | `play_gesture` | starts at the next frame; prints the play id |
| `gesture wait <id>` | (poll `get_gesture_state`) | returns when the play ends, with its events |
| `gesture stop [hand]` | `stop_gesture` | the hand keeps its place, returns to the keyboard |
| `gesture events <id>` | `get_gesture_events` | authored events and observed edges (pinch strength crossing 0.5 up and down, grab, tracking lost and found), each with its frame and time, to match against an app's log |

While a gesture plays a hand, keys and `pose` for that hand are ignored and reported as busy.
A gesture plays on whatever tracks the hand (Controllers), unless it names a `source`.
The UI gets a Gestures panel: the list, params as fields, Play and Stop.

Sweeping seeds (`gesture play ... seed <n>` in a loop with `reset hands`) shows which thresholds
are fragile; a `tools/` script for it comes once a sample test needs it.

## Tests

- `tests_sim.cpp`: parser and checker (every error path), expressions, each path
  kind's positions at given times, minimum jerk's peak at half time, throw peak speed at the
  release frame and grip below 0.5 before it, determinism (same seed, bit-identical joints;
  other seed differs), noise spectrum peaks where fitted, each built-in loads and checks.
- Against takes: per built-in, the simulated motion's duration, peak speed and final shape lie
  within the spread of the recorded reps (`motion/takes/`, trimmed and resampled).
- `tests_rec.cpp`: a gesture recorded and replayed gives identical state hashes; version 5 files
  still read.
- `tests_api.cpp`: CLI and C API, events' frames match the frames the app saw the input change.
- An app test: `apps/` or the CTS hand scenarios see a pinch from `pinch_tap` and nothing from
  `talking_hands`.

## Decisions

- The player is part of sim (`src/sim.cpp`), no layer of its own.
- Only trimmed takes (60 Hz, 4 decimals, a few hundred kB each) go into `motion/takes/`; raw
  sessions stay out.
- Gestures drive controllers too (Controllers).
- Users' gestures load from disk at startup and at runtime (Loading).
