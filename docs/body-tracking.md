# Body tracking (planned)

Status: phases 1 and 2 are built; face and eye tracking (phase 3) are not.

Body tracking reports a skeleton of the whole user. puppetxr's skeleton is posed, not animated: it
follows the simulated body's position and facing, its arms reach for the controllers and its
feet stay on the floor by two-bone IK, and everything else holds the pose it was given. Its
shape (bone lengths) and pose are set from the UI, `puppetxrctl` and the C API, so tests can check
that an engine's retargeting copes with unusual proportions, poses and tracking loss. Replays
repeat it bit-identically.

## Who needs it

| App | Uses | Role in verification |
|---|---|---|
| `apps/body_dump` (new) | `XR_FB_body_tracking`, `XR_META_body_tracking_full_body` | prints the T-pose and one located frame; run on puppetxr and on a device that tracks the body, the diff checks joint axes, parents and units |
| Godot OpenXR vendors `body-tracking-sample` (`vendors_body_tracking_gl`) | the same, OpenXR only | sample test: the avatar follows head and controllers; today it stands in T-pose |
| Meta `XrBodyFaceEyeSocial` (native) | `XR_FB_body_tracking`, `XR_FB_face_tracking2`, `XR_FB_eye_tracking_social` | draws the skeleton without retargeting |
| Meta `Unity-Movement` | body through OVRPlugin | retargeting under shape and pose changes; known to be fragile, a difference counts against puppetxr only when `body_dump` or `XrBodyFaceEyeSocial` show it too |
| Meta `Unreal-Movement` | the same through the Meta XR plugin | the same; sample test `unreal_movement_body` |

## Extensions

| Phase | Extension | Size | What |
|---|---|---|---|
| 1 | `XR_FB_body_tracking` | L | `XrBodyTrackerFB`, `xrLocateBodyJointsFB` (70 joints: root, hips, 3 spine, chest, neck, head, per side shoulder, scapula, upper and lower arm, wrist twist, and 26 hand joints), `xrGetBodySkeletonFB` (the T-pose of the current shape), skeleton change count |
| 1 | `XR_META_body_tracking_full_body` | M | joint set `XR_BODY_JOINT_SET_FULL_BODY_META`, 84 joints: the 70 and per side upper leg, lower leg, ankle twist, ankle, subtalar, transverse, ball |
| 2 | `XR_META_body_tracking_fidelity` | S | `xrRequestBodyTrackingFidelityMETA` per tracker: high (default) moves the fingers with the simulated hands, low keeps the hands open (`sim_body_joints`'s `fingers`); reported in `XrBodyTrackingFidelityStatusMETA` |
| 2 | `XR_META_body_tracking_calibration` | S | `xrSuggestBodyTrackingCalibrationOverrideMETA` sets the shape's height, `xrResetBodyTrackingCalibrationMETA` goes back to the configured one; the calibration state is reported |
| 3 | `XR_FB_face_tracking2`, `XR_FB_eye_tracking_social` | M each | separate plan |
| later | `XR_BD_body_tracking`, `XR_HTC_body_tracking` | M each | the same skeleton under their joint sets, once an app needs them |

## Model (sim layer)

`SIM_BodyState` in `SIM_Sim` (`src/sim.h`), three parts:

- **Shape**: a height (config `body.height`, default 1.75 m) scales the reference skeleton,
  Meta's default skeleton (`sim_body_torso_rest`, `sim_body_leg_rest` in `src/sim.cpp`):
  retargeting expects its proportions, and one only roughly like it (a few centimetres off per
  bone) made the Movement SDK pull its avatar's elbows apart. A scale per joint (default 1,
  0.25 to 4) lengthens the bones from that joint to its children, so `left_arm_upper` is the
  left upper arm and `hips` the hip width and lower spine. Left and right scale separately. Only
  joints with such a bone scale (`sim_body_joint_scalable`): not the root, the head, the twists,
  the ball of the foot or the hand; the hand keeps the simulated hand's size. Scaled bones give
  proportions no headset reports, for checking how a retargeting copes.
  `xrGetBodySkeletonFB` returns the T-pose of this shape, the lower foot on the floor; every
  shape change, including a calibration override, raises the skeleton change count.
- **Pose**: a rotation per joint relative to its T-pose parent frame. Presets: `t_pose`,
  `a_pose`, `stand` (default: arms down, legs straight), `arms_forward`, `sit`. A preset
  replaces the whole pose; joint edits change one joint.
- **Tracking**: per joint tracked and a confidence (default 1), so a test can drop the left arm
  or lower the hips' confidence. A dropped joint reports valid but not tracked: while the body
  is active the spec wants every joint valid.

Per frame, from that state and the simulated devices:

1. **Torso**: the head joint sits a level offset behind and below the view (`SIM_BODY_EYES_IN_HEAD`)
   and takes the view's orientation; the posed skeleton hangs from it, facing the head's yaw
   (the simulator turns head and body together). The root is on the floor under the hips.
2. **Arms**: each arm is in mode `follow` (default) or `posed`. In `follow`, two-bone IK from
   the shoulder to the wrist target: the controller's wrist, or the tracked hand's wrist. The
   elbow bends toward a pole below and outside the shoulder, fixed in the torso's frame so it
   cannot flip with the target. Out of reach the arm points at the target at full length and
   the wrist stops short of it, as a tracker does; the reach is clamped from below so a target
   at the shoulder keeps the elbow stable. An inactive or untracked hand lets its arm take the
   pose. Editing an arm joint switches that arm to `posed`; `follow` switches back.
3. **Hands**: the 26 hand joints are the simulated hand's (`sim_hand_joints`), its fingers
   following the hand's inputs; an inactive or untracked hand is the open rest hand on the
   posed wrist.
4. **Legs** (full body): mode `follow` (default) or `posed`, like the arms. In `follow` the hips
   hang below the neck along the posed spine and the feet stay flat on the floor under them, the
   ankles' rotation keeping the soles level; two-bone IK bends the knees toward a pole ahead of
   the hips in the body's frame. So the head going down crouches the body, down to the shortest
   the legs fold (knee angle limit); further down, as when the simulator moves the head into the
   floor, the whole skeleton goes down with it and the feet sink below the floor. The head going
   up straightens the legs; above their full length the feet leave the floor, as when flying.
   No steps: the feet slide with the root. `sit` and editing a leg joint switch that leg to
   `posed`; `follow` switches back.

Everything is `zmath` on the recorded per-frame device state plus `SIM_BodyState`, which only changes
through the control API, the UI and the calibration calls.

## Joint axes

Meta's runtime reports each joint with its own axes, which retargeting relies on. In the T-pose
(the body facing -Z, +X its right), from the default skeleton built into Meta's Unity-Movement
package and the per-joint corrections of Godot's OpenXR vendors plugin:

| Joints | +X | +Y | +Z |
|---|---|---|---|
| root | right | up | back |
| hips, spine, chest, neck, head | down | forward | right |
| left shoulder, scapula, arm upper, arm lower, wrist twist | right | forward | up |
| right shoulder, scapula, arm upper, arm lower, wrist twist | right | back | down |
| left hand (palm down) | toward the elbow | palm side | forward |
| right hand | toward the fingertips | back of the hand | back |
| left upper leg, lower leg, ankle twist | up | back | right |
| right upper leg, lower leg, ankle twist | down | forward | right |
| left ankle, subtalar, transverse, ball | forward | up | right |
| right ankle, subtalar, transverse, ball | back | down | right |

So the bone runs along X: toward the child along -X on the left side and the spine, +X on the
right. The table rounds: each joint's axes are those of Meta's default skeleton exactly, a few
degrees off these on the arms, the spine and the feet (`SIM_BodyRest.basis`). The hand joints are not XR_EXT_hand_tracking's: puppetxr turns the simulated hand's joints
into these axes (`body_hand_basis` in `src/sim.cpp`). The palm and the hand's wrist hang from the
wrist twist, the ankle from the ankle twist. `xrGetBodySkeletonFB` gives model-space poses, the
root at the origin; the feet are on the floor.

Internally a joint's frame is the body's axes in the T-pose and `SIM_BodyState.rotations` are
in it; `body_basis` turns a frame into Meta's when the joints are reported.

## Recording

Shape, pose, arm modes and tracking flags change what apps see, so a recording carries them: a
body block when any of them changes, written in the frame it takes effect. This bumps
`REC_FORMAT_VERSION` and adds a reader case (`docs/recording-format.md`). Calibration calls are
the app's and repeat on replay. Edits during a replay are `PUPPETXR_ERROR_REPLAYING`, as other
device edits are.

## Runtime (oxr layer)

- `XrBodyTrackerFB` handles in the session, the joint set fixed at creation.
- `xrLocateBodyJointsFB` locates the frame's skeleton at the asked time, as the hand joints do
  (`accept_time_zero` rules), into the base space; velocities only if asked.
- `systemProperties` report `supportsBodyTracking` and, with the full-body extension,
  `supportsFullBodyTracking`.

## UI, control API, CLI

- The body is edited as JSON, the same in the config's `body`, the C API and `puppetxrctl`:
  `{"height": 1.75, "pose": "t_pose", "bones": {"left_arm_upper": 1.2}, "joints":
  {"left_arm_lower": {"rotation": [x, y, z, w], "tracked": false, "confidence": 0.5}},
  "limbs": {"left_arm": "posed"}}`, every member optional, applied in that order and all or
  nothing (`sim_body_apply_json`). A rotated arm or leg joint poses its limb; `limbs` then wins.
  `sim_body_to_json` writes what differs from standing; its floats (`json_format_f32`) and the
  rotations (kept as given when of unit length) load back bit-exactly.
- C API: `get_body_joints`, `get_body`, `set_body` (merge), `reset_body`
  (`docs/control-api.md`).
- `puppetxrctl body` prints the joints; `body get|set|reset|save|load`, `body height`, `body pose`,
  `body bone`, `body joint ... rot|tracked|confidence`, `body arm|leg left|right follow|posed`
  (`docs/control-cli.md`).
- Devices panel: a Body tab with the height, a slider per bone scale, the pose preset, arm and
  leg modes, a joint picker with rotation, tracked and confidence, reset, and a toggle to draw
  the skeleton as lines over the mirror window (not in screenshots).

## Tests

- `tests_sim.cpp`: bone lengths equal the shape's in every frame and pose; a bone scale changes
  only that bone (and only that side); the wrist meets the controller when in reach and points
  at it out of reach; the elbow does not flip across a sweep of targets around the shoulder;
  editing an arm joint switches it to posed; a preset gives the same joints twice; dropped
  joints report untracked; lowering the head bends the knees with the soles level on the floor,
  below the legs' fold limit the feet sink with it, above their length the feet leave the floor,
  and the knees never bend backwards across a sweep of head heights.
- `tests_oxr.cpp`: tracker creation with and without the extensions, both joint sets, every
  function's invalid handles and structs, locate before the first frame, parents and T-pose
  match the extension's hierarchy, the skeleton change count after a shape change and a
  calibration override, fidelity round trip.
- `tests_rec.cpp`: shape, pose and tracking edits replay bit-identically; the previous format
  still reads.
- `apps/body_dump` prints the skeleton and one frame of joints of both joint sets as JSON, on
  any runtime (Vulkan session, no layers). ctest `body_dump` runs it fixed-step on puppetxr against
  `tests/body_dump_puppetxr.json`; a change to the body model updates that file from
  `build/<preset>/body_dump.json` after looking at the difference. Its output on a device that tracks
  the body is compared by hand, with the differences fixed or listed here.
- Samples: `vendors_body_tracking_gl` gets a reference where the avatar follows head and
  controllers, and one with a longer left arm; `XrBodyFaceEyeSocial` becomes a sample test on the
  platforms it runs on. `Unity-Movement` (`unity_movement_body`) and `Unreal-Movement`
  (`unreal_movement_body`, Windows) are sample tests (`docs/samples.md`).

## Decisions

- Default height 1.75 m for every device profile.
- `apps/body_dump.cpp` is approved as a new file.
- The Body tab's toggle draws the skeleton as lines over the mirror window, never in
  screenshots.
