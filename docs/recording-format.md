# Recording format

`src/rec.h`, `src/rec.cpp`. Format version 2; version 1 still reads. File extension `.xrrec` by convention.

A recording stores the simulator state an app observes, once per `xrWaitFrame`, not
the keyboard and mouse events that produced it. Replays therefore do not depend on
the key mapping, and poses set through the UI or the control API replay too.

## Layout

All integers are little-endian, `f32` is IEEE-754 binary32 stored by its bits, a
`Pose` is 7 `f32`: position x, y, z then orientation x, y, z, w.

```
file    = magic header chunk*
magic   = "PUPPETXR" (8 bytes)
header  = u32 version, u32 size, payload[size], u32 crc32(payload)
chunk   = tag[4], u32 size, payload[size], u32 crc32(payload)
```

Header payload:

| Field | Type |
|---|---|
| writer version | u32, `PUPPETXR_MAKE_VERSION` of the build that recorded |
| controller | Name (`oculus_touch`, `khr_simple`, `meta_touch_pro`, `meta_touch_plus`) |
| eye width, eye height | u32, u32 |
| refresh rate Hz | f32 |
| display period ns | i64 |
| fov degrees, ipd | f32, f32 |
| stage bounds x, y | f32, f32 |
| aim offset, palm offset | Pose, Pose |
| room      | u8 present; if 1: Name (`docs/world.md`), u64 `w_content_hash` of the room files |
| device table | u8 count, count x Name (`head`, `left`, `right`) |
| input table | u8 count, count x Name (`x/click`, ..., `select/click`) |
| start time ns, start frame index | i64, u64 |
| previous pose per table device | Pose each |
| initial state | State |
| stage in the room      | Pose, `room_from_stage` (`docs/anchors.md`) |
| anchor latency, localize frames      | u32, u32 |
| anchor faults      | u8 count, count x (i32 XrResult, u32 remaining), by `OXR_AnchorOp` |
| hand tracking cone | f32 full angle in degrees, `hands.tracking_fov_degrees` |
| frame faults       | u8 count, count x (i32 XrResult, u32 remaining), by `OXR_FrameOp` (`docs/openxr.md`, Frame faults); a remaining fault must be negative |
| skeleton change period | from version 2: u64 ns, `body_tracking.skeleton_change_period_ms`, 0 never. Version 1 has none and reads as 0 |

A Name is a u8 length (1..63) followed by that many bytes of `[a-z0-9_/.-]`.

Chunks:

| Tag | Payload |
|---|---|
| `STOR` | once before the frames: the anchor store as recording started: u64 next serial, u64 runs, u32 count, count x Anchor |
| `FRAM` | u64 frame index, u64 state hash after the frame, State |
| `EXTN` | between frames: u64 sim frame index, u8 kind, u32 count, then count x Anchor (kind 1, cloud anchors a query read) or count x Advert (kind 2, colocation adverts a discovery read) |
| `END_` | u64 number of `FRAM` chunks |

Anchor: 16 bytes uuid, room as u8 length (0..63) and `[a-z0-9_-]` bytes, u64 room content
hash, Pose in room space. Advert: 16 bytes uuid, u64 owner, u32 size (0..1024), bytes.

State, in the order of the device table:

```
i64  dt                     clock advance of this frame in ns
Pose local origin           the LOCAL reference space in stage space
per table device:
  u8   active
  u8   source               SIM_HandSource: 0 controller, 1 tracked hand; 0 for the head
  Pose detached             only when source is 1: where its controller was put down
  Pose pose                 grip pose for hands, view pose for the head
  u8   mask bytes (0..32)
  mask                      bit i set when table input i is non-zero
  per set bit: f32 x, f32 y
u16  typed length           0..256
     typed bytes            text typed into the app's virtual keyboard this frame: printable
                            ASCII, UTF-8, 0x08 backspace, 0x0A enter
u8   props present          1 in the header's initial state and when the props changed since
                            the last frame written, else 0: the props stay as they were
if present:
  u8   count                0..16
  per prop: u32 id, Name kind (keyboard, qr), Name motion (still, linear, circle), Pose pose,
            f32 target x, y, z, f32 radius, f32 speed, f32 size, i64 start time ns,
            u8 text length (0..213), text bytes (no zero)
u8   body present           1 in the header's initial state and when the body tracking state
                            changed since the last frame written, else 0: the body stays as it was
if present (docs/body-tracking.md):
  f32  height               0.5..3 m
  u8   joint count          up to 84, XR_META_body_tracking_full_body order; later joints default
  per joint: f32 bone scale (0.25..4, 1 where the joint has no bone), f32 rotation x, y, z, w
             (unit length within 1e-3), f32 confidence (0..1), u8 tracked (0 or 1)
  u8   limb count           up to 4: left arm, right arm, left leg, right leg
  per limb: u8 mode         0 follow, 1 posed
u8   face present           1 in the header's initial state and when the face changed since
                            the last frame written, else 0: the face stays as it was
if present (docs/face-tracking.md):
  u8   expression count     up to 70, XR_FB_face_tracking2 order; later ones are skipped
  per expression: f32 weight (0..1)
  u8   viseme count         up to 15, XR_META_face_tracking_visemes order
  per viseme: f32 weight (0..1)
  f32  confidences          lower face, upper face (0..1)
  u8   blink                0 or 1
  f32  gaze yaw, pitch      degrees from the head (up to 35 and 30)
  u8   look at              0 or 1
  f32  target x, y, z       stage space, where the eyes look when look at is 1
u8   drift present          1 in the header's initial state and when the tracking drift changed
                            since the last frame written, else 0: the drift stays as it was
if present (docs/anchors.md, Tracking drift):
  Pose tracking drift       STAGE in the simulator's frame: a yaw about +Y (x and z of the
                            quaternion at most 1e-4 of its length) and a translation
```

A typical frame is about 170 bytes, about 55 MB per hour at 90 Hz.

The start time, frame index and previous poses let a recording that began
mid-session (from the UI or the control API) replay with identical state hashes: the
first replayed frame derives its velocities from the same previous poses. Every frame
is flushed to disk as it is written, so a recording of a crashing app keeps every
frame up to the crash.

## Evolving the format

Inputs, devices and controllers are named in the header, so most growth needs no new
format version:

- **A new input or device** is appended to the `SIM_Input` or `SIM_Device` tables and
  shows up in the header tables of new recordings. When reading, names map to this
  build's indices: a name this build does not know is parsed and dropped
  (`rec_unknown_inputs`, a warning in the log), and an input or device an old file
  lacks reads as zero, or inactive with an identity pose.
- **A new controller type** is a new name. Old builds refuse recordings made with a
  controller they cannot simulate; new builds read old files unchanged.
- **Anything else** (a new header field, a different state encoding) bumps
  `REC_FORMAT_VERSION`. `read_header` and `read_state` in `rec.cpp` switch on the
  version; the new version gets new `get_header_vN` / `get_state_vN` functions and the
  writer moves to it. The old cases stay.

Version policy: until puppetxr 1.0 an old version may be dropped. From 1.0 on, every
version ever written stays readable. A file of a version the build does not read is
rejected with a message naming the versions it reads.

## Validation

Loading never trusts the file. The magic, the version, the header CRC, every header
value range, the names (character set, length, no duplicates among known names) and
the controller must check out or the load fails. Chunks are validated one by one (size
bound, CRC, every value finite, quaternions of plausible length, `dt` in 0..10 s,
`active` 0 or 1, no unknown input bits, typed text without other control characters). The first invalid or truncated chunk ends the
recording: the frames before it replay, and `rec_is_complete` is false. A missing or
mismatching `END_` chunk also marks it incomplete. Tests flip every byte and cut the
file at every length to make sure none of this can crash.

## Determinism across builds

A change to deterministic math (the golden hashes in `tests_zmath.cpp`) or to how
velocities are derived makes old recordings diverge on replay even though they still
load. Replays report this as divergences; the replayed poses and inputs are still the
recorded ones.

## Anchors and the world outside the process

Saved anchors, other users' cloud anchors and colocation adverts come from files the app
does not control, so a replay never reads them:

- `STOR` holds the local anchor store as recording started. A replay runs against a copy
  of it and writes nothing to disk; when the replay stops or ends, the store is read from
  disk again.
- Every read of the simulated cloud or of colocation adverts while recording appends an
  `EXTN` chunk with what was read. A replay takes them in order instead of reading.
- The stage's place in the room, anchor latency, localization delay and pending faults
  are in the header; the C API cannot change them while recording or replaying.
- Pending frame faults (`docs/openxr.md`, Frame faults) are in the header too, so the
  replay fails the same frame calls; a failed `xrWaitFrame` advances no frame, so the
  frames line up. They cannot change while recording or replaying.

## Rooms

A recording replays only in the room it was made in. Replay loads the room named in
the header and refuses to start when it is missing or its content hash differs, since
scene queries, raycasts and depth would no longer match. The room cannot change while
recording or replaying.
