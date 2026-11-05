# Face and eye tracking

puppetxr simulates a face and two eyes for Meta's face and eye tracking. The face holds the
expression it was given (a preset or per-expression weights, visemes, confidences); the eyes look
where the gaze points, and blink on a fixed schedule. Everything comes from the simulator's
state and clock, so replays repeat it exactly.

## Extensions

| Extension | What |
|---|---|
| `XR_FB_face_tracking2` | `XrFaceTracker2FB`, 70 expression weights (`SIM_FaceExpression` in its order), lower and upper face confidence, the visual data source when the app asks for it, else audio |
| `XR_FB_face_tracking` | the first 63 of them, the same order |
| `XR_META_face_tracking_visemes` | 15 visemes (`sil` to `ou`) chained to the weights of face tracking 2 |
| `XR_FB_eye_tracking_social` | `XrEyeTrackerFB`, a gaze pose per eye (-Z along the gaze, +Y the head's up) and its confidence |
| `XR_EXT_eye_gaze_interaction` | the `ext/eye_gaze_interaction` profile on `/user/eyes_ext`: its `/input/gaze_ext/pose` is between the eyes, looking along their gaze; the pose action is active while the head is tracked. Actions without subaction paths bind on it too. An `XrEyeGazeSampleTimeEXT` chained to the location of that action space gets the time the gaze was located for, 0 while the gaze is not valid |

## Model (sim layer)

`SIM_FaceState` in `SIM_Sim` (`src/sim.h`):

- **Weights**: 70 expression weights, 0 to 1. Presets (`neutral`, `smile`, `frown`,
  `surprise`, `eyes_closed`, `jaw_open`) replace them all.
- **Visemes**, **confidences** (lower face, upper face; default 1).
- **Blink**: on by default; every 4 s the lids close in 60 ms, stay shut 30 ms and open in 110 ms
  (`face_blink` in `src/sim.cpp`), from the sample's time.
- **Gaze**: yaw and pitch from the head in degrees (up to 35 and 30), or a point in stage space
  both eyes look at (`look_at`), so they converge. The `eyes` control profile (key 4) turns the
  gaze with the mouse (`turn_eyes`, `docs/config.md`).

Per frame (`sim_face_frame`), from that state and the sampled head:

1. The eyes sit half the IPD left and right of the view's centre and look along the gaze.
2. The eye blendshapes (`eyes_look_left`, `_right`, `_up`, `_down`, per eye) are the gaze's
   angles from the head over the eyes' range, replacing the state's: the face and the gaze agree,
   which is what `isEyeFollowingBlendshapesValid` reports.
3. `eyes_closed` is the state's or the blink's, whichever is more.

## Controls

- The face is edited as JSON, the same in the config's `face`, the C API and `puppetxrctl`:
  `{"preset": "smile", "weights": {"jaw_drop": 0.5}, "visemes": {"aa": 1}, "confidences": [1, 1],
  "blink": true, "gaze": [10, -5], "look_at": [0, 1.6, -1]}`, every member optional, applied in
  that order and all or nothing (`sim_face_apply_json`); `"look_at": null` goes back to the gaze
  angles. `sim_face_to_json` writes what differs from neutral; it loads back bit-exactly.
- Names are the extension's in lower case without prefix and suffix (`jaw_drop`,
  `lip_corner_puller_l`) and part of the C API and the recording format.
- C API: `get_face_weights`, `get_eye_gazes`, `get_face`, `set_face`, `reset_face`
  (`docs/control-api.md`); `puppetxrctl face` (`docs/control-cli.md`).

## Recording

The face state is recorded like the body's: a block when it changed
(`docs/recording-format.md`). The state hash covers it once it differs from neutral, so recordings
of a neutral face keep their hashes.

## Tests

- `tests_sim.cpp` (`SimFace.*`): names, a neutral face looking ahead with the eyes at the IPD,
  eye blendshapes following the gaze and converging on a point, the blink schedule, presets and
  clamping, JSON round trips, the config and replays, the eyes profile.
- `tests_oxr.cpp` (`OxrFaceTest.*`, `OxrEyeGazeTest.*`): system properties, trackers and their
  validation, weights of both versions and the visemes equal to the simulated face, gazes looking
  at a point, the gaze profile current on `/user/eyes_ext` and its pose following the eyes.
- `tests_rec.cpp`, `tests_api.cpp`, `tests_ui.cpp`: the face block, the C API and `puppetxrctl face`,
  the Face tab.
- `apps/eye_dump` prints one frame of eye tracking as JSON on any runtime (Vulkan session, no
  layers). ctest `eye_dump` runs it fixed-step on puppetxr against `tests/eye_dump_puppetxr.json`;
  a change to the eyes or the face updates that file from `build/<preset>/eye_dump.json` after
  looking at the difference.
- Samples: `unity_movement_face`, Meta's Movement SDK showing the face on its characters;
  `unity_movement_gaze`, its scene menu selected by gaze and pinch; `unreal_movement_face`, the
  Unreal sample's Aura through the Meta XR plugin (Windows) (`docs/samples.md`).

## Comparing with a headset

`apps/eye_dump` runs on any runtime, so the same binary dumps puppetxr and a headset that tracks
eyes. It enables only what the runtime has, and leaves the rest of the output out. The headset's
own name is not in the output: a dump is pasted into reports, and only the runtime's name belongs
there.

On puppetxr:

```
XR_RUNTIME_JSON=$PWD/build/gcc-debug/puppetxr-dev.json PUPPETXR_HEADLESS=1 PUPPETXR_FIXED_STEP=1 \
  build/gcc-debug/bin/eye_dump --frames 3 --out puppetxr.json
```

On a headset, with its own runtime active and the dump taken while looking straight ahead:

```
build/gcc-debug/bin/eye_dump --frames 90 --out headset.json
```

What the two outputs settle, with the `view` poses the ones that matter (the `stage` poses carry
wherever the head happens to be):

| Question | Where |
|---|---|
| the gaze axes: -Z along the gaze, +Y the head's up | `ext_eye_gaze_interaction.view.orientation`, identity while looking ahead |
| where the eyes sit | `fb_eye_tracking_social[].view.position`, half the IPD left and right at the view's height |
| the gaze sample time | `ext_eye_gaze_interaction.view.sample_time_delta`, 0 on puppetxr because the gaze is sampled at the located time |
| the eye angles per view | `views.in_view_space[].fov`, symmetric on puppetxr and asymmetric on hardware (`TODO.md`) |
| the weights' ranges | `fb_face_tracking2.weights`, 70 values in the extension's enum order |

A gaze other than straight ahead is set on puppetxr with `puppetxrctl face gaze 20 -10`
and read back from the same dump, to check that a yaw turns the gaze the same way on both.

Differences are fixed, or listed here.

### Measured on a device that tracks eyes

Two runs, about 105 s of continuous wear, `XR_EXT_eye_gaze_interaction` only (the device reports
no face tracking, so the weights are still unmeasured). The second run looked at targets of known
position, 31 samples landing on one:

| Question | Hardware | puppetxr | Verdict |
|---|---|---|---|
| gaze origin in view space | mean (-0.3, -1.5, 0.0) mm, excursions to 7 mm while the eyes move | exactly 0 | agrees within 1.5 mm; the simulated origin stands |
| gaze roll about its own axis | mean -0.03 degrees, within +-1.5 while the eyes move | exactly 0 | agrees; the gaze keeps the head's up, it does not stay world up |
| which way yaw turns | a target left of the view reads **positive** yaw (+7.1 to +9.3), one to the right **negative** (-23.7 to -5.5); against the angle the geometry gives, the error averages **-0.04 degrees** | left positive, the same | agrees, with no bias worth naming |
| which way pitch turns | the same sign as the geometry, averaging **1.6 degrees low**, inside the target's own half-angle of 3.4 degrees | up positive, the same | agrees; whether the 1.6 is a calibration lean or the eyes resting low on the target is not separable here |
| gaze sample time | 28 to 51 ms **before** the frame's display time (means of 33 and 39 over the two runs), drifting a steady -88 ppm against it within a run | the located time, so 0 | **differs**: the gaze is a sample from the past, and its clock is not locked to the display's |
| the pose staying usable | never once unusable, and never frozen either: no two consecutive samples repeated their angles, so the gaze is always live rather than a held last value | active while the head is tracked | agrees as far as this reaches |

The sample time is the only difference found. Part of it is that the display time is two frames
ahead, so the sensor lag beyond the pipeline is the smaller remainder; what matters for an app is
that the gaze pose it draws with was sampled a frame or two ago, which puppetxr's 0 hides. The
-88 ppm drift says the eye tracker timestamps on a clock of its own.

The usable flag never went false, through natural blinks and a deliberate closure. The log cannot
prove a sample landed inside a sustained closure, so read this as "no unusable gaze was ever
observed" rather than as a proof that closing the eyes keeps it valid.

Still open: everything that needs a device tracking the face.

## Decisions

- `apps/eye_dump.cpp` is approved as a new file.
- The dump prints the runtime's name but not the system's, so a dump pasted into a report or
  committed as a reference does not name the headset it came from.
