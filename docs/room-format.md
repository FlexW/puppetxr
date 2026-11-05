# Room format

`src/world.h`, `src/world.cpp` (`w_parse_room`, `w_load_room`). Format version 1
(`W_FORMAT_VERSION`). A room is a directory `rooms/<name>/` with three files:

| File | Content |
|---|---|
| `room.json` | metadata, semantic entities, buffer views into the binary |
| `room.bin` | little-endian raw arrays, 4-byte aligned views |
| `room_albedo.png` / `.jpg` | baked lighting atlas for the visual mesh, sRGB, optional; the exporter writes PNG |

Written by `tools/blender/puppetxr_room_export.py` (`docs/world.md`, Authoring rooms); the
`.blend` source sits next to the exported files (LFS). The runtime never needs Blender.

## Coordinates

Room space is stage space: metres, Y up, -Z forward, floor at y = 0, origin at the
stage origin. Poses are `position [x, y, z]` and `orientation [x, y, z, w]`
(normalized on load).

## room.json

```json
{
  "format": "puppetxr_room",
  "version": 2,
  "name": "bedroom",
  "binary": "room.bin",
  "albedo": "room_albedo.png",
  "visual":      { "vertices": { "offset": 0, "count": 1200 }, "indices": { "offset": 24000, "count": 3600 } },
  "global_mesh": { "positions": { "offset": 38400, "count": 800 }, "indices": { "offset": 48000, "count": 2400 } },
  "entities": [
    { "uuid": "<32 hex>", "label": "other", "position": [0, 0, 0], "orientation": [-0.7071, 0, 0, 0.7071],
      "room": { "floor": 1, "ceiling": 2, "walls": [3, 4, 5, 6], "children": [1, 2, 3, 4, 5, 6, 7, 8] },
      "room_mesh": { "positions": { "offset": 75144, "count": 28 }, "faces": [
        { "uuid": "<32 hex>", "label": "wall_face", "indices": { "offset": 75624, "count": 30 } },
        { "uuid": "<32 hex>", "parent": "<the wall face's uuid>", "label": "window_frame", "indices": { "offset": 75880, "count": 6 } } ] } },
    { "uuid": "<32 hex>", "label": "floor", "position": [0, 0, 0], "orientation": [-0.7071, 0, 0, 0.7071],
      "plane": { "min": [-2, -1.5], "max": [2, 1.5], "boundary": [[-2, -1.5], [2, -1.5], [2, 1.5], [-2, 1.5]] } },
    { "uuid": "<32 hex>", "label": "table", "position": [0.5, 0.75, 0], "orientation": [-0.7071, 0, 0, 0.7071],
      "plane": { "min": [-0.6, -0.4], "max": [0.6, 0.4] }, "volume": { "min": [-0.6, -0.4, -0.75], "max": [0.6, 0.4, 0] } },
    { "uuid": "<32 hex>", "label": "global_mesh", "position": [0, 0, 0], "orientation": [0, 0, 0, 1], "mesh": true }
  ]
}
```

| Key | Meaning |
|---|---|
| `visual.vertices` | `count` x (f32 x, y, z, u, v), 20 bytes each; unlit, sampled from the albedo atlas |
| `visual.indices`, `global_mesh.indices` | u32 triangle lists |
| `global_mesh.positions` | `count` x (f32 x, y, z) in room space; served relative to the global mesh entity's pose |
| `uuid` | 32 hex digits, unique, non-zero; stable across exports |
| `label` | `w_label_name`: `other`, `floor`, `ceiling`, `wall_face`, `invisible_wall_face`, `inner_wall_face`, `door_frame`, `window_frame`, `opening`, `wall_art`, `table`, `couch`, `bed`, `storage`, `screen`, `lamp`, `plant`, `global_mesh` |
| `plane` | rectangle `min`/`max` in the entity's XY plane, +Z is the normal; optional `boundary` polygon (>= 3 points, counter-clockwise seen from +Z) |
| `volume` | box `min`/`max` in entity space |
| `mesh` | the entity carries the global mesh, in its own space |
| `room` | room container: `floor`, `ceiling` and `walls` are entity indices with a plane, `children` lists every entity of the room once |
| `room_mesh` | on a room entity only, optional: the room's closed mesh (`XR_META_spatial_entity_room_mesh`). `positions` are f32 x, y, z in room space (at least 4); each face has a unique `uuid`, an optional `parent` (another face's uuid, the face it is cut out of), a `label` (`floor`, `ceiling`, `wall_face`, `invisible_wall_face`, `inner_wall_face`, `door_frame`, `window_frame`) and u32 `indices` into the room mesh positions, whole triangles, counter-clockwise seen from inside |

## Invariants checked on load

Every entity belongs to exactly one room. Coordinates are finite and within 1000 m.
Rectangles and boxes have `min <= max`. Indices are in range and form whole triangles.
Buffer views are 4-byte aligned and inside the binary. Image is PNG or JPEG, at most
16384 pixels per side. Room mesh faces are planar (within 1 mm), have a known label and a
parent that is another face of the same mesh; a room mesh on an
entity without `room` is rejected. Closure (the spec's closed volume) is checked by the
exporter, not on load. A violation rejects the whole room with a `LOG_ERROR`.

## Compatibility

Names (labels, keys) never change once released. A layout change bumps
`W_FORMAT_VERSION` and the reader keeps a case for every version ever written.

| version | change |
|---|---|
| 1 | first format |
Recordings store the room name and `w_content_hash` (over all three files).
