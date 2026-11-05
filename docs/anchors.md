# Anchors: storage, sharing, tracking origin, drift

Code: `src/oxr.cpp` (sections *Anchor stores*, *Anchor storage*), the file formats in
`src/world.cpp` (`docs/world.md`, *Anchor storage*), tracking drift in `src/sim.cpp`
(`sim_set_tracking_drift`). Tests: `OxrAnchorTest` in `tests/tests_oxr.cpp`, `World.store_*`,
`World.cloud_*` in `tests/tests_world.cpp`, `Sim.*drift*` in `tests/tests_sim.cpp`.

## The room is the physical world

A real headset fixes anchors to the room it sees, while STAGE and LOCAL are measured from a
tracking origin that moves between runs (recenter, a new boundary, a reboot) and drifts during
one. puppetxr models both: the loaded room is the world, and the simulator's frame (the stage as it
lay at instance creation) lies in it at `room_from_stage`, fixed for the instance. Devices,
props, the body, anchors and the room are physical and kept in that frame; tracking drift
moves the app's STAGE away from it (below).

- Room entities, passthrough, environment depth and raycasts are in room space and are
  converted at the boundary (`locate_world`, `place_passthrough`, the depth and raycast
  entry points).
- Anchors live in stage space while the app runs and are stored in room space, so a saved
  anchor comes back on the same table even when the stage moved.
- An app that keeps stage coordinates in its own save files instead of anchors sees its
  content land elsewhere once the origin moves. That is the bug class the offset exists for.

`origin_offset` in the config (`docs/config.md`):

| Value | Stage origin in the room |
|---|---|
| `"none"` (default) | at the room origin, facing -Z |
| `{ "yaw_degrees": 35, "position": [0.4, -0.2] }` | fixed, x and z in metres |
| `"per_run"` | a new place every instance: `w_origin_offset_for_run(origin_seed, run)`, any yaw, within 0.5 m on the floor; `run` counts instances in the store (`runs`) |

`PUPPETXR_ORIGIN_OFFSET=none|per_run|<yaw degrees>,<x>,<z>` and `PUPPETXR_ORIGIN_SEED` override it.
The offset is logged at instance creation. A recenter (UI, C API `recenter`, `puppetxrctl
recenter`) moves LOCAL and sends `XrEventDataReferenceSpaceChangePending` as before.

## Tracking drift

Tracking drifts while an app runs: its estimate of where the headset is slips, so what it
reports in STAGE moves although nothing in the room did. An engine's world locking (an anchor
placed at start, the camera offset by how far the anchor moved since) is tested with it.

- The drift `D` is where the tracked STAGE origin lies in the simulator's frame; the STAGE origin
  lies in the room at `room_from_stage * D`. `D` is a yaw about +Y and a translation (y allowed);
  gravity stays down.
- STAGE, LOCAL, LOCAL_FLOOR and STATIONARY move with `D` (`sim_sample_reference_space_pose`).
  VIEW stays on the head. A thing fixed to the room at `p` in the simulator's frame (the head,
  the hands, props, the body, room entities and the scene mesh, anchors made this session or
  loaded or shared) is reported in STAGE at `inverse(D) * p`.
- Raycasts, environment depth, passthrough and the composited room are of the physical room around
  the physical head, so drift leaves their images alone; their poses in app spaces move like
  any other. Layers in STAGE or LOCAL move with the drift against passthrough, as on a headset.
- Anchors are saved in room space as before: a save under drift stores the same room pose.
- A recenter under drift puts LOCAL at the head; the LOCAL and STAGE origins the change events
  and `XrEventDataReferenceSpaceChangePending` compare are measured from STAGE, so drift alone
  sends no event, as on a headset.
- Velocities do not include the drift rate.

| Form | Effect |
|---|---|
| a step: `sim_set_tracking_drift(D)` | `D` from the next locate on; identity removes it |
| a rate: `sim_set_tracking_drift_rate(velocity, yaw)` | at every live `xrWaitFrame`, with that frame's clock step `dt` in seconds: `D.position += velocity * dt`, `D.orientation = yaw(rate * dt) * D.orientation`, so the origin turns about its own vertical axis. Under the fixed-step clock every run drifts bit-identically; zero stops it and keeps the drift reached |

- Refused (false, nothing changes): a non-finite pose, a quaternion of length outside
  [0.5, 2], a quaternion whose x and z exceed 1e-4 of its length (a tilt above about 0.01
  degrees), a non-finite rate. Accepted quaternions are normalized with x and z zeroed.
- The drift is part of the recorded frame state (`docs/recording-format.md`, v12) and of the
  state hash when not identity. A replay applies the recorded drift and never a rate of its own;
  drift may change while recording.
- Config `drift_rate` and `PUPPETXR_DRIFT_RATE` set the rate of a new instance (`docs/config.md`).

## Stores

| What | Where | Key |
|---|---|---|
| local store | `<anchors_dir>/<app key>/user_<user id>.json` | one simulated headset of one user |
| cloud | `<cloud_dir>/<app key>/anchors/<uuid>.json` | shared by every user and process pointing at the directory |

- `anchors_dir` defaults to `os_state_dir()/anchors`, `cloud_dir` to `<anchors_dir>/cloud`;
  pointing `cloud_dir` at a shared or network directory connects processes and machines.
- The app key is `anchors.app_id` (`PUPPETXR_APP_ID`) if set, else the application name, with
  anything outside `[A-Za-z0-9._-]` turned into `_`. Engines often report one name for every
  app; set the id then.
- `anchors.user_id` (`PUPPETXR_USER_ID`, default 1) is the simulated user: two processes with
  different ids act as two people with their own headsets.
- The store is read at `xrCreateInstance` and written after every save and erase. A corrupt
  store is logged and left untouched; the run works with an empty store and saves nothing.
- One process per store: two processes of the same app and user overwrite each other's saves.

## Saving and loading

| Request | Effect |
|---|---|
| `xrSaveSpaceFB`, `xrSaveSpaceListFB`, `xrSaveSpacesMETA` | put the anchors in the local store (or the cloud for location CLOUD) with the room name, its content hash and the pose in room space |
| `xrEraseSpaceFB`, `xrEraseSpacesMETA` | remove them; erasing a missing anchor succeeds. In the cloud the owner deletes, anyone else only stops seeing it |
| `xrQuerySpacesFB` | the room's entities and the stored anchors; with `XrSpaceStorageLocationFilterInfoFB` CLOUD only cloud anchors |
| `xrDiscoverSpacesMETA` | the room's entities and the stored anchors |

- Anchors support LOCATABLE, and STORABLE with a storage extension enabled. A new anchor
  has only LOCATABLE enabled; apps enable STORABLE before saving, or get
  `XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB`. Loaded anchors have everything enabled.
- A stored anchor loads only in the room it was saved in, the same version of it (content
  hash); anchors of another room or an edited room are skipped with a warning. Anchors saved
  without a room load only without a room.
- Anchor uuids are `w_anchor_uuid(app key, user id, serial)`, the serial counted in the store,
  so a new run never repeats a stored uuid.

## Sharing

Sharing goes through the cloud directory: a share writes or extends
`<cloud_dir>/<app key>/anchors/<uuid>.json` with the owner, the user ids and the group uuids
it was shared with (`docs/world.md`).

| Request | Effect |
|---|---|
| `xrCreateSpaceUserFB` | a handle holding any user id; destroyed with its session |
| `xrShareSpacesFB` | the anchors become visible to those user ids |
| `xrShareSpacesMETA` with `XrShareSpacesRecipientGroupsMETA` | the anchors join those groups; at most 32 per request |
| `xrSaveSpaceFB` with location CLOUD | uploads without recipients, visible to the owner |
| `xrQuerySpacesFB` with location CLOUD | cloud anchors the user owns or was shared |
| `xrQuerySpacesFB` with `XrSpaceGroupUuidFilterInfoMETA`, chained to the query info (the registry) or as its `filter` (the extension's text, OVRPlugin) | every cloud anchor of the group, whoever shared it; none completes with `XR_ERROR_SPACE_GROUP_NOT_FOUND_META` |

- Sharing needs the SHARABLE component enabled (`XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB`);
  anchors support it with a sharing extension enabled.
- Two users on one machine: run the app twice with `PUPPETXR_USER_ID=1` and `=2`. Two
  machines: point `PUPPETXR_CLOUD_DIR` at a directory both mount.
- While recording, each cloud read is stored in the recording (`EXTN` chunks,
  `docs/recording-format.md`), so a replay sees the cloud as it was even after the owner
  erased the anchor.

## Colocation discovery

Apps find each other with `XR_META_colocation_discovery`, usually to exchange the group uuid
they then share anchors with.

- `xrStartColocationAdvertisementMETA` writes `<cloud_dir>/<app key>/adverts/<uuid>.json` (owner,
  buffer as hex) and holds it as a lock file (`os_lock_file_create`) until the advertisement
  stops or its session ends; a crashed process leaves a stale file that the next listing
  deletes. Starting twice completes with `XR_COLOCATION_DISCOVERY_ALREADY_ADVERTISING_META`.
- A running discovery lists the directory at the next frame and then every
  `OXR_COLOCATION_POLL_FRAMES` (18) frames, and reports each advertisement once, never the
  session's own. Starting twice completes with `XR_COLOCATION_DISCOVERY_ALREADY_DISCOVERING_META`.
- Stopping sends the stop completion and then the advertisement or discovery completion.
- Every listing while recording goes into the recording (`EXTN` kind 2); polls are frame
  based, so a replay takes them at the same frames.

## Simulated trouble

| Knob | Config | C API, `puppetxrctl`, UI |
|---|---|---|
| completion latency in frames | `anchors.latency_frames` | `set_anchor_timing`, `anchors timing` |
| frames a loaded anchor stays unlocalized (locations without valid bits) | `anchors.localize_frames` | `set_anchor_timing`, `anchors timing` |
| the next n saves, erases, loads or shares fail with a given error | | `set_anchor_fault`, `anchors fault` |
| wipe the app's local store, optionally its cloud anchors | | `wipe_anchors`, `anchors wipe [cloud]`, UI buttons |
| list stored anchors | | `get_anchors`, `anchors`; the UI shows the count |
| hide an extension, e.g. storage, from the app | `disabled_extensions` | `set_disabled_extensions` |

Completions with latency arrive at the `xrWaitFrame` that many frames later, in request order.
