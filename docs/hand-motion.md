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
| Built-in gestures, fitted hand model | `data/motion/` (next to `data/rooms/`), embedded by `cmake/embed_motion.cmake` | JSON in the repository, one file per gesture, no code change to add one |
| Users' gestures | files on disk, loaded at startup and at runtime (Loading) | the same loader and checker as the built-ins |
| Play, stop, list, check, events | control API, `puppetxrctl gesture`, C API, UI panel | how agents and tests drive it |

## Hand model (sim)

Built: skeleton, key poses, curves, strengths from the joints, finger coupling and limits.

`data/motion/hand_model.json` is fitted by `tools/hand_fit.py` (Python standard library only) to
recorder takes measured on a Quest 3, embedded by `cmake/embed_motion.cmake` and loaded by
`load_hand_model` in `src/sim.cpp` at `sim_create`; a missing or invalid file fails there.

```
python3 tools/hand_fit.py <takes dir>... -o data/motion/hand_model.json
blender -b data/motion/hand.blend --factory-startup --disable-autoexec --python tools/blender/puppetxr_hand_mesh.py -- data/motion/hand_model.json data/motion/hand_mesh.json
```

`data/motion/hand_mesh.json`, the hand's skinned mesh (`XR_FB_hand_tracking_mesh`, docs/openxr.md),
is the sculpt of `data/motion/hand.blend` fitted to the model's open hand and goes with it: after a
refit run the second command too (`Sim.the_hand_mesh_fits_the_hand_model` fails until then). The
sculpt is "Hand - Realistic" by Dan Ulrich of Blender Studio's Human Base Meshes 1.4.1 (CC0), with
an empty per joint. Each bone maps the skin around it onto the same bone of the open hand: turned,
stretched along it, scaled across it. Blender's heat weights blend the bones and skin the result,
smoothed (`SMOOTHING`) so a bend spreads over several rings, the two largest kept per vertex. The
forearm is cut `FOREARM` behind the wrist joint, as a Quest's hand ends there.

- **Skeleton**: each joint's offset in its parent's frame, averaged over `still_open`, and its
  radius, the median. A Quest 3's skeleton is fixed (a bone varies by under 0.3 mm), and its
  left hand mirrors the right within a millimetre, so both hands go into one right hand that
  `hand_local` mirrors for the left. Its joints point up to 5 degrees off their bones.
- **Key poses**: every joint's rotation relative to its parent in nine poses, each the medoid
  of its frames (the recorded frame closest to all others; an average of the rotations opens a
  pinch that closes in every frame): `open` (`still_open`), `relaxed` (`still_relaxed`), `pinch`
  (frames of `pinch_tap` and `pinch_hold` with the thumb and index tips under 8 mm apart),
  `fist` (the tenth of `fist` with the fingertips closest to the palm), and the recorded
  gestures `thumbs_up`, `point`, `poke`, `grasp` and `wave` (Recorded gestures). The pinch's thumb comes
  from another session than the bones, so the fit turns its four thumb joints until the tips are
  as far apart as the median of the recorded pinches (7.5 mm).
- **Shape**: `SIM_HandShape` holds per finger, thumb to little, how far it goes from the base
  pose toward `open`, then `pinch`, then `fist` (nlerp per joint), the whole hand toward each
  recorded gesture's pose by its weight (`recorded`, in the order above), and thumb turns on top
  (microgestures). A thumb curl of 0 is the relaxed or open thumb, which lies ahead along the
  index; only the recorded thumbs-up raises it to the side. The base is `relaxed` for a tracked
  hand at rest and `open` for the hand mesh's bind pose and the body skeleton's T-pose.
  `hand_shape(pinch, grab)`: all fingers toward
  the pinch by `pinch * (1 - grab)` and toward the fist by `grab`, except the index, which goes
  to the fist only with the pinch and straightens to the open pose with grab alone (pointing).
- **Coupling and limits**: `coupling`, index to little, how far each finger curls at least
  while another is fully curled, as a blend weight. Fitted from `curl_each`, `poses`, `fist` and
  `spread`: the 1st percentile of a finger's curl (tip to palm, open pose 0, fist pose 1) while
  another is at least 0.8 curled, converted to the weight that reaches it. Only the fingers with
  joined tendons are coupled, middle and ring and ring and little (middle to ring 0.62, little to
  ring 0.72, ring to middle 0.50); the index moves alone, as it has its own extensor and the takes
  hold no tight point to measure it by. Every weight is clamped to [0, 1], so no finger curls past
  the recorded fist (no frame of the takes curls more than 5 to 12 % past it). `couple_shape` in
  `hand_local` applies both to every shape. `sim_curled_hand_joints` (`SIM_HandCurls`: a curl per
  finger, a pinch, how far open from relaxed, a weight per recorded pose) gives the joints of such a shape for gestures.
- **Facings**: `facings`, how people hold the hand, as the palm joint's orientation in the head's
  level frame (-Z its level forward, +Y up) of a right hand, the mean over a recorded gesture's
  frames (Recorded gestures). Only the hands a take is for go in, the left mirrored; the sim
  mirrors it back. Gestures take them as palm facings of the same names (Gesture format).
- **Recorded gestures**: the frames of each, from the takes of `docs/recorder.md`; the pose is
  their medoid, the facing their mean (the back of the hand rolled outward from up, the fingers
  up from level):

  | Name | Frames | Pose | Facing |
  |---|---|---|---|
  | `pinch` | `pinch_tap`, `pinch_hold`, the thumb and index tips under 8 mm apart | key pose `pinch` | 38, 28 degrees |
  | `poke` | `poke_right`, the tenth with the index tip farthest ahead | yes | 12, 18 |
  | `thumbs_up` | `poses`, the fingertips under 4 cm from the palm, the thumb tip over 6 cm from the index tip | yes | 98, 13: the palm in, the thumb 45 degrees from up |
  | `point` | `poses`, the fingertips 4.5 to 6.5 cm from the palm, the thumb tip over 9 cm from the index tip | yes | 22, 10 |
  | `grasp` | `grab_ball_right`, `grasp_ext/value` at least 0.75 | yes | 37, 23 |
  | `palm_up` | `palm_up`, the back of the hand pointing down by more than half | | 139, -15 |
  | `wave` | `wave`, the palm less than 25 cm below the head | yes, also the open hand of the swipes | 54, 71 |
  | `swipe_in`, `swipe_out` | `swipe_right`, the palm moving sideways over 0.8 m/s and twice as fast as up or down, toward the body's middle or away | | 52, 13; -23, 13 |
  | `two_hand_pinch` | `two_hand_scale`, both hands, the thumb and index tips under 12 mm apart (the right closed to 8.5 mm at best there) | | 23, 33: more upright than one hand's pinch |

  The point holds the other fingers loosely (5.5 cm from the palm) and the thumb out, the poke
  curls them less (7 cm) with the thumb in; the grasp closes around a ball (7 cm), not to a fist
  (2.5 cm); in `palm_up` the palm turns up and toward the body's middle. `swipe_right` asks for
  four directions; only the sideways strokes are used.
- **Curves**: `pinch_curve` (thumb to index tip in m to `pinch_ext/value`) and `grasp_curve`
  (mean of the middle, ring and little tips to the palm to `grasp_ext/value`), medians of 2 and
  5 mm bins, non-increasing.
- **Strengths**: what apps read of a tracked hand's `trigger/value` and `squeeze/value` (and so
  `pinch_ext`, `aim_activate_ext`, `grasp_ext`, `select` and the aim state's pinch strength) comes
  from its joints through the curves (`sim_reported_input`); controllers report their inputs as
  they are. A tracked hand's inputs are the strengths to reach: `solve_hand_shape` bisects the
  grab weight on the grasp curve, then the pinch weight on the pinch curve, so an input reads
  back (within 0.001), a half pinch shows the 12 to 20 mm a Quest 3 reports it at, and 1 takes
  the whole way to the recorded pinch. No hand reads less grasp than its relaxed fingers give
  (about 0.45, as an open hand on a Quest 3), and a pinch curls the others enough to read a high
  grasp, as on the headset.

What the model gives (`tests_sim.cpp`): the open hand 195 mm from wrist to middle tip, the palm
joint 67 mm from the wrist; at rest the thumb and index tips 47 mm apart, beyond any pinch; a
pinch closes them to 4 to 10 mm with the other fingers part way curled; a fist brings every tip
within 65 mm of the palm; pinch and grasp inputs read back as reported strengths.

## Gesture format

Built: the format, its checker and the player (`src/sim.cpp`, section Gestures; `sim_gesture_*`),
tests `SimGesture.*`. Not yet: the paths `circle`, `twist`, `shake`, `swing`, `curve`, `throw`,
solved shapes, noise, clip steps, controllers (a controller-held hand moves, its shape waits for
Controllers), loading from disk and the CLI.

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
    { "anchor": "pinch", "to": "target", "bow": 0.03, "duration": 0.6, "palm": "down" },
    { "shape": "pinch", "duration": 0.12, "event": "pinch" },
    { "wait": "hold" },
    { "shape": "idle", "duration": 0.15, "event": "release" },
    { "path": "home", "speed": 0.5 }
  ]
}
```

- Top level: `format` (`puppetxr-gesture`), `version` (1), `steps` (1 to 64); optional `name`,
  `description`, `hands` (informational), `params`. Any other key is an error.
- **Params** (up to 16): `type` `point`, `number` or `seconds`, `default`, for numbers `min` and
  `max`. A play may give values (a JSON object: numbers, `[x, y, z]` or expressions); a value of
  no param, or outside min and max, is an error.
- **Steps** run one after the other; `"with": "previous"` starts a step with the one before
  (another hand, or a shape during a move). Keys: `hand` (`primary`, default, the hand the play
  names; `other`; `left`; `right`), `anchor` (`grip`, default, `aim`, `palm`, `pinch`,
  `index_tip`: the point that follows the path), `to` (a point) or `path` (`home`: where the
  anchor and the hand's orientation were when the gesture started), `bow` (metres the middle of
  a move rises, as a sine of the eased progress, perpendicular to the path), `palm` (`down`,
  `up`, `in`, `out` toward the body's middle, `forward`, or as recorded: `pinch`, `poke`,
  `thumbs_up`, `point`, `grasp`, `palm_up`, `wave`, `swipe_in`, `swipe_out`, `two_hand_pinch` (Hand model,
  Recorded gestures); the palm joint's facing relative to the head's level forward, slerped),
  `shape` (`idle`, `open`, `fist`, `pinch`, a recorded pose: `thumbs_up`, `point`, `poke`,
  `grasp`, `wave`, or `{ "curls": [5 numbers 0..1], "pinch": 0..1, "base": "relaxed"|"open",
  "<recorded pose>": 0..1 }`, every number interpolated, the base too),
  `duration` (seconds) or `speed` (m/s, for moves), `ease` (`min_jerk`, default, `linear`,
  `ease_in`, `ease_out`), `event` (a name reported when the step ends), `hold` (true keeps the
  last shape after the gesture). `{"wait": seconds}` waits and has no other key but `hand` and
  `with`. Durations are 0 to 600 s.
- **Expressions** for points and numbers: numbers, `+ - * /`, unary minus, parentheses,
  `vec(x, y, z)`, params and `head`, `forward` (the head's, level), `up`, `down`, `outward`,
  `inward` (of the step's hand), `home` (the step's anchor when the gesture started),
  `left.<anchor>`, `right.<anchor>`. A number times a point scales it; adding a number to a point
  is an error. Everything is evaluated once when the gesture starts, so the whole motion is known
  up front, and a speed's duration comes from the distance planned so far.
- **Errors** name the JSON path: `steps[1].to: unknown name targte`, `params given.nope: the
  gesture has no such param`.

### Player

- One gesture plays at a time; playing another stops it. It starts in the frame after the play,
  in `sim_begin_frame` after the controls (keys cannot move a hand it moves), never in a replay:
  recordings hold the poses and shapes it made (Recording).
- Slow frames skip no step: when a frame's step of time would run a step that also ends in the
  frame the one before it ended in (on the realtime clock, a screenshot can stall a frame by
  hundreds of ms), it waits a frame, so the end of the one before shows, and the rest of the
  timeline moves on with it. Under the fixed step, steps at least a frame long run as planned.
- A step takes its start (anchor position, palm orientation, shape) in its first frame, then
  each frame eases from there: the shape by `sim_set_hand_shape`, the palm by turning the grip
  (the palm joint's offset from the grip is fixed), the anchor by moving the grip under it.
  Velocities come from the frame-to-frame motion, as for keys.
- Events: a step's when it ends, `end` when the gesture ends, `stopped` for `sim_gesture_stop`;
  each with the frame (`sim_frame_index` during it) and its display time, the last 64 kept
  (`sim_gesture_events`).
- At the end, and at a stop, the hands keep their poses and go back to their inputs, unless the
  last shaping step of a hand holds.

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
"relative": "head" }`. `clip` names a take (built-in, in `data/motion/takes/`) or a file path; the
loader converts it to wrist poses relative to the head at the take's first frame and joint
rotations relative to their parents (the take's bone lengths are dropped, the sim skeleton
kept), resampled to the sim's frames. So a user records their own gesture with the recorder,
pulls the take and plays it as is, or inside a gesture between procedural steps. Body clips
later use the same step with `"body"` instead of `"hand"`.

## Loading

Built. Built-ins and users' gestures are the same JSON, loaded by `sim_gesture_load`; a user's file
of a built-in's name replaces it for that run.

- Built-ins: every `data/motion/gestures/*.json`, embedded by `cmake/embed_motion.cmake` with the hand
  model and loaded at `sim_create`; an invalid one fails there (`SimGesture.every_built_in_gesture_plays_to_its_end`).
- At startup, after the built-ins: the files and directories of `gestures.paths` (config,
  `docs/config.md`), then those of `PUPPETXR_GESTURE_PATH` (`:` apart, `;` on Windows,
  `os_path_list_separator`), up to 8 in all. A directory loads its `.json` files sorted by name. A
  file that fails the checker is logged with its path and error and skipped; the others load.
- At runtime: `sim_gesture_load_path` (a file or a directory; the CLI and C API come next).
- Names are the file name without `.json`: 1 to 63 of `[a-z0-9_-]`. Files are at most 256 KB,
  up to 128 gestures in all.
- A recording stores the poses and shapes a gesture made, not the gesture, so it replays without
  the files.

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

Built (`data/motion/gestures/`), each with defaults that work in front of the head. Where a gesture
reaches, its default target puts the palm where the recorded takes put it (within 5 cm), and the
recorded gestures hold the hand as in the takes; `SimGesture.built_in_gestures_look_like_their_recordings`
holds both to the measured values:

| Gesture | What | Events |
|---|---|---|
| `pinch_tap` | pinch anchor to `target`, pinch, `hold`, release, home | `pinch`, `release` |
| `pinch_hold` | pinch where the hand is for `hold` (1.5 s) | `pinch`, `release` |
| `poke` | the recorded poke, the index tip `depth` (2 cm) past `target` along forward and back, home | `press`, `release` |
| `grab` | palm to `target`, open, the recorded grasp, `hold`, open, home | `grab`, `release` |
| `point`, `thumbs_up` | the recorded pose and facing, kept (`hold`) where the hand is | the name |
| `fist`, `open_palm` | the shape, kept (`hold`); `open_palm` turns the palm forward, fingers up | the name |
| `palm_up` | the left palm up in front of the body for `hold`, home | `up`, `down` |
| `wave` | the recorded wave beside the head, three swings of `width` | |
| `swipe_left`, `swipe_right` | the open hand across `distance` (50 cm) on the hand's side of the body | `swipe` |
| `two_hand_scale` | both hands pinch in front, pull `apart` (80 cm), release | `pinch`, `scaled`, `release` |
| `idle` | the hands rest for `seconds`; nothing should trigger | |

Planned, with the paths and solved shapes they need: `double_pinch`, `middle_pinch`,
`point_and_pinch`, `pinch_drag`, `pinch_scroll`, `grab_move`, `throw_overhand`,
`throw_underhand`, `pull_lever`, `turn_wheel`, `turn_dial`, `two_hand_rotate`, `clap`, `ok`,
`victory`, `look_at_palm`, `beckon`, `system_gesture`, microgestures, `talking_hands`,
`scratch_head`, `hands_down`; and checks against the takes of each.

## Recording (rec)

Built (format 6). A gesture sets a tracked hand's shape (`sim_set_hand_shape`, `SIM_HandCurls`: a
curl per finger, a pinch, how far open, a weight per recorded pose); while it is set, the joints and the reported
strengths come from it instead of the pinch and grab inputs. Recordings store the shape per frame
after the hand's detached controller pose, as they store the inputs: both replay through the hand
model, so a replay needs neither the gesture nor the player, and 30 bytes a frame against 840 for
the joints. Versions 1 to 5 read with every hand unshaped.

## Control API and CLI

Built (API version 3, docs/control-api.md, docs/control-cli.md):

| `puppetxrctl` | C API | does |
|---|---|---|
| `gesture list`, `gesture show <name>` | `list_gestures`, `get_gesture` | the loaded names, a gesture's JSON |
| `gesture check <file>` | `check_gesture` | errors with JSON paths |
| `gesture load <file\|dir>` | `load_gesture` | adds or replaces users' gestures (Loading) |
| `gesture play <name\|file.json> [left\|right] [k=v ...]` | `play_gesture` | starts at the next frame; prints the play id |
| `gesture wait <id>`, `gesture events <id>` | `get_gesture_state` | the play's events (authored, `end`, `stopped`) with frames, once it ended or now |
| `gesture stop` | `stop_gesture` | the hands stay and go back to their inputs |

The server's requests are `load_gesture`, `check_gesture`, `list_gestures`, `get_gesture`,
`play_gesture` (`name` or an inline `gesture` object, `hand`, `params` object), `stop_gesture`,
`get_gesture_state` and `wait_gesture` (`play`: the play id; `id` numbers requests), which replies once the play ended; replies give the play id as `play`. A failing request's
`message` carries the checker's error. While a gesture plays a hand, the keys cannot move it. Not
yet: observed edges in the events (a pinch strength crossing 0.5 up and down, tracking lost and
found), a `tools/` script sweeping seeds once noise exists.

### Gestures tab

The Simulator panel's Gestures tab (`gestures_tab`, `src/ui.cpp`,
`UiTest.gestures_tab_plays_poses_and_saves`): the hand, a gesture with its description and params
(a JSON object), play and stop, the last events or the checker's error. Playing switches the hands
the gesture uses to hand tracking (`sim_gesture_uses`). "pose the hand" holds the chosen hand in a
shape (`sim_set_hand_shape`) made from a preset or a recorded pose, a slider per finger, the pinch,
the open base and the recorded poses' weights. "save as gesture" turns it into a gesture of one step
that takes the shape and keeps it (`sim_gesture_from_shape`), loads it and writes it to
`gestures.dir` (`sim_gesture_save`), which loads at every start after the built-ins and before
`gestures.paths`. A built-in's name is refused.

## Tests

- `tests_sim.cpp`: parser and checker (every error path), expressions, each path
  kind's positions at given times, minimum jerk's peak at half time, throw peak speed at the
  release frame and grip below 0.5 before it, determinism (same seed, bit-identical joints;
  other seed differs), noise spectrum peaks where fitted, each built-in loads and checks.
- Against takes: per built-in, the simulated motion's duration, peak speed and final shape lie
  within the spread of the recorded reps (`data/motion/takes/`, trimmed and resampled).
- `tests_rec.cpp`: a gesture recorded and replayed gives identical state hashes; version 5 files
  still read.
- `tests_api.cpp`: CLI and C API, events' frames match the frames the app saw the input change.
- An app test: `apps/` or the CTS hand scenarios see a pinch from `pinch_tap` and nothing from
  `talking_hands`.

## Decisions

- The player is part of sim (`src/sim.cpp`), no layer of its own.
- Only trimmed takes (60 Hz, 4 decimals, a few hundred kB each) go into `data/motion/takes/`; raw
  sessions stay out.
- Gestures drive controllers too (Controllers).
- Users' gestures load from disk at startup and at runtime (Loading).
