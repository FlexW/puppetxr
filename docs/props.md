# Props

Things in the room besides the room itself: `SIM_Prop` in `src/sim.h`, at most `SIM_MAX_PROPS`
(16). Keyboards and QR codes are tracked: apps learn about them through tracking extensions
(keyboards: `XR_META_dynamic_object_keyboard`, docs/openxr.md "Dynamic objects", and
`XR_FB_keyboard_tracking`, docs/openxr.md "Tracked keyboards"; QR codes:
`XR_EXT_spatial_marker_tracking`, docs/openxr.md "Spatial entity contexts"). Meshes are any
model (a chair pushed through the room, clutter on a table) that no extension reports, as a Quest
tracks no chair: apps meet them only where a Quest's cameras do. Every prop shows in passthrough,
environment depth (`XR_META_environment_depth`) and the depth raycast
(`XR_META_environment_raycast`); none is in the global mesh or the scene, which are the scan of
the still room. Props collide with nothing, neither each other nor the room.

## Model

| Field | Meaning |
|---|---|
| `id` | from 1, never reused while the sim lives; set by `sim_add_prop` |
| `kind` | `keyboard`: the `sim_keyboard_model`, +Y up, front at +Z, origin at its bounding box centre, 30 x 1.8 x 11.2 cm. `qr`: a code centred in its XY plane, facing +Z. `mesh`: a model (Models), in its own frame |
| `pose` | stage space: where it is (still), where it starts (linear), the centre it circles (circle) |
| `motion` | `still`, `linear` (back and forth between `pose` and `target` at `speed`), `circle` (horizontal circle of `radius` around `pose`, counter-clockwise seen from above starting at +X, turning with the path, at `speed`) |
| `target`, `radius`, `speed` | meters, meters, meters per second (0 to 100) |
| `size` | QR: the code's edge length without the quiet zone, 0.01 to 10 m |
| `text` | QR: what it encodes, up to 213 bytes (byte mode, level M, versions 1 to 10) |
| `model` | mesh: the name of a model, `[a-z0-9_-]`, 1 to 63 characters |
| `scale` | mesh: along every axis of the model, 0.01 to 100; 0 is 1 |
| `start` | sim time its motion started: `sim_add_prop` and every `sim_update_prop` set it to now |

- `sim_prop_pose(prop, time)` is a pure function of the prop and the time, so props move
  without per-frame updates and replays reproduce them. Path distances are computed in f64.
- Invalid props are refused whole: unknown kind or motion, non-finite or degenerate pose, a
  circle without radius, a QR size out of range, unterminated text, a mesh whose model is not
  loaded and cannot be (Models) or whose scale is out of range.

## Models

What a mesh shows: a glTF 2.0 model, loaded into the sim under a name (`sim_add_model`,
`SIM_MAX_MODELS` 16). The sim keeps its GLB as `w_pack_model` (`src/world.cpp`) wrote it; the
runtime parses each once (`refresh_prop_models`, `src/oxr.cpp`) for passthrough, depth and
raycasts. A model of a name already loaded replaces it, and every mesh naming it shows the new one.

Built in (`data/models/<name>/<name>.glb`, compiled in by `cmake/embed_models.cmake`): each stands on
its origin, centred, its front at +Z (glTF), with base colours only.

| Name | Size | What |
|---|---|---|
| `chair` | 44 x 87 x 42 cm, 772 triangles | wooden legs and back posts, blue seat and back rest |
| `pencil` | 18.6 x 0.7 x 0.6 cm, 148 triangles | lying along X, hexagonal, sharpened at +X |
| `case` | 22 x 6 x 16.8 cm, 520 triangles | a small dark hard case, its handle at the front |

`tools/blender/puppetxr_models.py` builds them in Blender and writes each `.glb` next to its
`.blend` (Git LFS, scene data only, so neither holds a local path):
`blender -b --factory-startup --python tools/blender/puppetxr_models.py -- data/models [name ...]`.

Loading a model:

- Each loader parses the model to check it and hands the parse to the runtime
  (`keep_parsed_model`, `src/oxr.cpp`), so the frame that first shows it does not parse it again
  under the runtime's lock.
- A prop naming a model not loaded yet loads it (`SIM_Config::load_model`): one of the config's
  `models` by its file, else a built-in one. This is how config props and `add_prop` show a chair.
- From a file at run time: C API `load_model`, control command `load_model`, `puppetxrctl model
  load <path> [name]`. The file is read and checked before the runtime's lock is taken.
- From a recording: its models replace the loaded ones of their names before the frames that
  show them (Recording).

What `w_pack_model` reads, all else refused with `LOG_ERROR` naming it:

- `.glb`, and `.gltf` whose buffers and images are data URIs (base64) or files relative to it
  (percent-decoded; no scheme, no absolute path).
- The default scene (`scene`, else scene 0, else the nodes no node names as a child), every node's
  `matrix` or TRS applied, a mesh under two nodes placed twice. Nesting deeper than 32 or a cycle
  is refused.
- Triangles, strips and fans (as triangles); points and lines are skipped with a warning.
- `POSITION` (float), `TEXCOORD_0` (float, normalized unsigned byte or short), indices (unsigned
  byte, short or int), byte strides.
- Per material `baseColorFactor` and `baseColorTexture` (PNG or JPEG, texture coordinate set 0),
  the texture times the factor in linear light; untextured parts show the factor. Texture
  coordinates outside [0, 1] clamp to the edge. Normals, other textures and alpha are ignored.
- Animations are ignored with a warning: the model shows its rest pose.
- Refused: skins, morph targets, sparse accessors, any `extensionsRequired` (Draco, mesh
  quantization), a position not finite or beyond 1000 m, an index past its primitive's vertices.
- Limits (`src/world.h`): 64 MB per file and per packed model, 2^20 vertices, 2^19 triangles,
  256 primitives, 4096 texels per side of a texture, and 2^26 texels (256 MB of RGBA) decoded over
  all materials, as each material decodes its texture on its own.

The packed GLB is canonical: one node, one mesh, a primitive per input primitive with its own
vertices, materials and images as read. The same model always packs to the same bytes, so a
`.gltf` and the `.glb` of the same scene pack alike, and packing a packed model changes nothing
(`ModelTest.*`, `tests/tests_world.cpp`).

## Where props come from

- Config `props` (`docs/config.md`) and the environment `PUPPETXR_PROPS` (a JSON array, replacing
  the config's), spawned when the sim is created. Invalid entries are skipped with a warning.
- UI: the Props panel adds a QR code at eye height, a keyboard below it or a chair on the floor,
  1 m in front of the head, and edits pose, text, size, model (loaded and built-in ones), scale
  and motion; every edit restarts the motion.
- C API (`add_prop`, `update_prop`, `remove_prop`, `get_props`, `load_model`), `docs/control-api.md`.
- puppetxrctl `props`, `prop ...`, `models` and `model load`, `docs/control-cli.md`.

A prop object (config, `PUPPETXR_PROPS`, control protocol):

```json
{ "kind": "mesh", "model": "chair", "position": [0.5, 0, -1.2], "yaw_degrees": 30,
  "motion": "linear", "target": [-0.5, 0, -1.2], "speed": 0.3 }
```


```json
{ "kind": "qr", "text": "https://example.com", "size": 0.15,
  "position": [0, 1.5, -1], "orientation": [0, 0, 0, 1],
  "motion": "linear", "target": [1, 1.5, -1], "speed": 0.2 }
```

`yaw_degrees` may replace `orientation`. `update_prop` applies only the fields given.

## Recording

Props are frame state (`SIM_FrameState::props`), in the recording header's initial state and in
a frame only when they changed since the last frame written
(`docs/recording-format.md`). A frame without props keeps them. Models are not frame state: each
goes into a `MODL` chunk with its GLB before the first frame that may show it (the models loaded
when the recording starts before the first frame), again only when its bytes change, so a replay
needs none of the files they came from (`OxrRecordTest.mesh_props_replay_without_their_model_files`).

## Passthrough

`comp_set_props` (`src/comp.cpp`) runs every `xrEndFrame` with the frame's display time:

- Geometry in room space, rebuilt per frame into frame-slotted host-visible buffers, drawn with
  the passthrough pipeline right after the room so both share depth.
- Keyboards copy the `sim_keyboard_model` (aluminium body, dark caps). QR codes are a plate: the
  code with its 4-module quiet zone on the front, gray on the back 2 mm behind.
- The atlas: a 72 x 72 cell per prop (one texel per module, `w_qr_encode`, `src/world.cpp`)
  above a row of solid colours, sampled with nearest filtering, uploaded only when a prop's kind
  or text changes. Codes decode with OpenCV's detector from a screenshot.
- Meshes draw their model, uploaded once per parse (`COMP_PropModel::serial`), each part with its
  base colour image, through a per-draw transform; models no prop shows are released.

## Environment depth and raycasts

`prop_instances` (`src/oxr.cpp`) poses every prop at the capture time (depth) or the ray's time
(raycast) as a `W_Instance` in room space: a keyboard as its model, a QR code as its plate (the
code with its quiet zone, `COMP_PROP_PLATE` thick), a mesh as its model at its scale.
`w_trace_depth` and `w_raycast_instances` (`src/world.cpp`) take each ray into every instance's
model space against its BVH; the nearest of the room and the props wins. The sensor's errors
(`sim_depth_sensor`) then apply to props as to the room, so a moving chair arrives in the depth a
capture late, as on a Quest. The raycast is a depth query on a Quest, so props stop rays too; the
global mesh, the scene and the room mesh never see them.
