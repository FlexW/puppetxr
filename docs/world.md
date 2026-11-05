# World

`src/world.h`, `src/world.cpp`, prefix `W_`/`w_`. Layer between sim and render: CPU
only, testable without Vulkan. Tests: `tests/tests_world.cpp`, fixture
`tests/test_room.h` (a 4 x 3 x 2.5 m box room with a table).

The world is the simulated physical environment: one source of truth that every
scene-understanding extension maps onto. File format: `room-format.md`.

## Three products, independent like on a real headset

| Product | Used by | Accessor |
|---|---|---|
| visual mesh + albedo atlas | passthrough, environment depth | `w_visual_mesh`, `w_albedo` |
| semantic entities | FB scene, META discovery, later EXT spatial entities | `w_entity`, `w_room`, `w_room_indices`, `w_boundary_points` |
| global mesh | META spatial entity mesh, environment raycast | `w_global_mesh`, `w_raycast` |

## Entity model

An entity is a UUID, a label, a pose in room space and a component mask
(`W_ComponentFlags`): `Plane` (rectangle + optional polygon in local XY, +Z normal),
`Volume` (box), `Mesh` (the global mesh), `Room` (layout and children). Extensions
translate labels and components; the world knows no OpenXR vocabulary.

## Raycast

`w_raycast` hits the global mesh, both faces, nearest hit in `(0, max_distance]`,
normal turned towards the origin. A BVH (median split on the longest centroid axis,
leaves of 4 triangles) is built once on load; a ray parallel to an axis that lies in a box face
enters it (`ray_slab`), so rays straight down through the vertices of a scan-sized mesh hit. Pure
`zmath`, so results are deterministic; ties go to the lower triangle index.

## Rooms on disk

`w_list_rooms(rooms_dir)` returns the subdirectories holding a `room.json`, sorted.
Room names are `[a-z0-9_-]`, 1 to 63 characters (`w_room_name_valid`), so they are
safe as path components and as recording Names.

## Starter rooms (`rooms/`)

| Room | Size | Contents |
|---|---|---|
| `bedroom` | 4.0 x 3.6 x 2.5 m | bed, nightstand, lamp, wardrobe, window, door, wall art |
| `office` | 3.4 x 3.0 x 2.6 m | desk (table), monitor (screen), keyboard and chair (other), bookshelf, plant, window, door, whiteboard (wall art) |
| `living_room` | 5.6 x 4.6 x 2.7 m | couch, armchair (couch), coffee table, TV (screen) on a stand (storage), floor lamp, plant, two windows, door, wall art |
| `l_shaped` | 6 x 6 m L, 2.6 m | non-convex floor, an invisible wall opening onto a hallway that exists only in the visual mesh, dining table and chairs, kitchen counter (storage), couch, side table, lamp, two windows, door, wall art |

`living_room` is the default (`docs/config.md`).

Every room of `rooms/` is compiled into the library, so puppetxr ships as one file:
`cmake/embed_rooms.cmake` writes `room.json`, `room.bin` and `room_albedo.png` of each into
`<build>/rooms/rooms_embedded.cpp` as byte arrays (`w_embedded_rooms`), built as the static
library `puppetxr_rooms`, which clang-tidy skips. It reruns only when a room file changes
(about 15 s, plus 10 s to compile). `World.compiled_in_rooms_are_the_source_rooms` checks the
copies against the files.

- More rooms: a rooms dir (`rooms_dir`, `PUPPETXR_ROOMS_DIR`, else `share/puppetxr/rooms` next to the
  library, else the source tree in development builds) adds its rooms, and one of the same name
  replaces the compiled-in room. `w_open_room` and `w_list_all_rooms` (`src/world.cpp`) do both.

The stage origin is free floor in every room (`World.starter_rooms_load_and_make_sense`);
`OxrStarterRoomsTest` renders each from there through passthrough and writes
`<build>/room_<name>_<0..3>.png` for inspection.

## Authoring rooms

Each room is `rooms/<name>/<name>.blend` (Git LFS) next to its export. The exporter is
`tools/blender/puppetxr_room_export.py`, an add-on (sidebar tab "puppetxr": tag shell faces,
export) and a command line tool that never modifies the `.blend`:

```
blender -b rooms/<name>/<name>.blend --python tools/blender/puppetxr_room_export.py -- rooms/<name> [--no-bake] [--room-mesh-only] [--global-mesh-only]
```

Blender as a Python module works too (`pip install bpy` of the Blender version that saved the
`.blend`, Python 3.13 for 5.2): open the file with `bpy.ops.wm.open_mainfile`, set `sys.argv`
to `["blender", "--", ...]` and run the script with `runpy.run_path(..., run_name="__main__")`.

Scene conventions (Blender metres, Z up; exported Y up):

| Collection | Holds | Becomes |
|---|---|---|
| `puppetxr_shell` | one watertight mesh, normals into the room, every face tagged with the int face attribute `puppetxr_label` (1 floor, 2 ceiling, 3 wall, 4 invisible wall, 5 inner wall) | room, floor, ceiling and wall entities; visual mesh except invisible faces |
| `puppetxr_furniture` | empties with a custom property `puppetxr_label`, turned only about Z, with child meshes | a volume (origin at the top face, +Z up) measured in the empty's frame; `table`, `couch`, `bed`, `storage` also get the top as a plane; children join the visual mesh |
| `puppetxr_wall_items` | quads with `puppetxr_label` (`door_frame`, `window_frame`, `wall_art`, `opening`), local +Z into the room, hidden from render | planes; door and window frames are also room mesh faces and must lie on a wall (within 5 cm) |
| `puppetxr_visual` | meshes only seen: frames, baseboards, rugs, whatever lies beyond openings | visual mesh |

Shell rules, checked with a message naming the face: one flat floor region and one flat
ceiling region without holes, vertical walls that are rectangles spanning floor to
ceiling (Scene v1), coplanar connected faces of one label merge into one entity. Walls
are listed in the order the floor boundary meets them, counter-clockwise from above.

The visual mesh (all visible meshes, joined, triangulated, smart-projected UVs) is baked
with Cycles into a 2048 x 2048 PNG atlas:

| Step | Detail |
|---|---|
| lighting | diffuse direct, indirect and colour, 2048 samples, float buffer so lit surfaces never clip |
| albedo | diffuse colour only, 16 samples, the denoiser's guide |
| denoise | OIDN (compositor Denoise node, HDR, high quality) in a scratch scene without render layers |
| save | the scene's view transform, look, exposure and gamma (as in Blender's rendered view), 8-bit sRGB PNG |

Tune lights in the rendered viewport: the atlas looks like it. PNG, not JPEG: the 8 x 8
blocks and chroma subsampling of JPEG show as a pattern in dark gradients such as
contact shadows. A GPU bake takes about two minutes per room. The global
mesh is the same surfaces, welded, decimated to at most 8000 triangles (`GLOBAL_MESH_TRIANGLES`) and cut until no edge
is longer than 0.2 m (`GLOBAL_MESH_EDGE`), as a headset's scan has no wall-sized triangles; its entity is turned like
the floor, local +Z up, as on Meta's headsets, whose scans keep the mesh's vertical along that axis (MRUK reserves the
bottom of the mesh for DestructibleMesh along it). `--global-mesh-only` replaces the global mesh of an exported room
and turns its entity so, keeping the other bytes of `room.bin`; the room mesh after it moves. UUIDs are
md5 hashes of room and object names, so they stay stable across exports.

The room mesh (`XR_META_spatial_entity_room_mesh`) is generated from the shell, no
authoring needed: one face per shell region (floor, ceiling, each wall) and per door or
window frame. Each frame is projected onto the wall it sits on and cut out of it with four
`bisect_plane` cuts limited to that wall's faces; the cuts also split edges shared with
neighbouring regions, so the mesh stays watertight (checked on export). The frame face's
parent is its wall's face. Triangles keep the shell's winding, counter-clockwise seen from
inside. `--room-mesh-only` adds the room mesh to an exported room without touching the
existing bytes of `room.bin` or the atlas, which keeps a room's visual mesh identical
without re-baking.

## Anchor storage

The simulated headset's anchor storage lives in the world layer too, because a stored
anchor is fixed in the room, not in tracking space (`docs/anchors.md` for the runtime
side). Files are JSON, `W_STORE_VERSION` 1, poses with `%.9g` so every `f32` round-trips.

| Function | File | Content |
|---|---|---|
| `w_store_load`, `w_store_save` | `<anchors_dir>/<app key>/user_<id>.json` | `next_serial`, `runs`, anchors: uuid, room name and content hash, pose in room space |
| `w_cloud_save`, `w_cloud_list` | `<cloud_dir>/<app key>/anchors/<uuid>.json` | one shared anchor: owner, users, groups |
| `w_advert_start`, `w_advert_list` | `<cloud_dir>/<app key>/adverts/<uuid>.json` | colocation advertisement held by its process (`os_lock_file_create`); stale files are deleted when listed |

- Saves write a temporary file and rename it (`os_write_file_atomic`), so a crash never
  leaves half a store.
- Listings are sorted by uuid, never in directory order, so they are deterministic.
- `w_anchor_uuid(app key, user, serial)` makes RFC 9562 version 8 uuids; the serial is
  kept in the store so a new run never repeats a stored uuid.
- `w_origin_offset_for_run(seed, run)`: the stage origin in room space for a run, any yaw,
  within `W_ORIGIN_OFFSET_MAX_DISTANCE` (0.5 m) on the floor.
