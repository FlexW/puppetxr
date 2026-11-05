#pragma once

#include "base.h"
#include "zmath.h"

#define W_FORMAT_VERSION 1
#define W_UUID_SIZE      16
#define W_MAX_NAME       64
#define W_RAYCAST_MAX_DISTANCE 2000.0f // spans any room, whose coordinates stay within 1000 m
#define W_STORE_VERSION  1
#define W_ADVERT_MAX_SIZE 1024 // XR_MAX_COLOCATION_DISCOVERY_BUFFER_SIZE_META
#define W_ORIGIN_OFFSET_MAX_DISTANCE 0.5f

struct W_World;

// Semantic labels of the canonical world model. Names (w_label_name) are what room files use
// and never change once released; each OpenXR extension maps them to its own vocabulary.
enum class W_Label : u8
{
    Other = 0,
    Floor,
    Ceiling,
    WallFace,
    InvisibleWallFace,
    InnerWallFace,
    DoorFrame,
    WindowFrame,
    Opening,
    WallArt,
    Table,
    Couch,
    Bed,
    Storage,
    Screen,
    Lamp,
    Plant,
    GlobalMesh,
    Count,
};

enum class W_ComponentFlags : u32
{
    Plane    = 1 << 0, // rectangle and optional polygon in the entity's XY plane, +Z is the normal
    Volume   = 1 << 1, // box in entity space
    Mesh     = 1 << 2, // the global mesh, in entity space
    Room     = 1 << 3, // layout (floor, ceiling, walls) and the entities it contains
    RoomMesh = 1 << 4, // the room's closed mesh of labelled faces, on the room entity
};

struct W_Uuid
{
    u8 bytes[W_UUID_SIZE];
};

struct W_Entity
{
    W_Uuid  uuid;
    u32     components; // W_ComponentFlags
    W_Label label;
    Pose    pose;       // in room space, which is stage space
    Vec2    plane_min;
    Vec2    plane_max;
    u32     boundary_first; // into w_boundary_points, 0 points when the plane has no polygon
    u32     boundary_count;
    Vec3    volume_min;
    Vec3    volume_max;
    u32     room; // index of the room this entity belongs to
};

struct W_Room
{
    u32 entity;
    u32 floor;   // entity indices
    u32 ceiling;
    u32 walls_first; // into w_room_indices
    u32 walls_count;
    u32 children_first; // into w_room_indices, every entity of the room except the room itself
    u32 children_count;
    u32 mesh_positions_first; // into w_room_mesh_positions, room space; none without a room mesh
    u32 mesh_positions_count;
    u32 mesh_faces_first;     // into w_room_mesh_faces
    u32 mesh_faces_count;
};

// One planar face of a room mesh. Indices count from the room's first position and form
// triangles, counter-clockwise seen from inside the room.
struct W_RoomMeshFace
{
    W_Uuid  uuid;
    W_Uuid  parent; // the face this one is cut out of (a frame in a wall), zero for none
    W_Label label;  // floor, ceiling, a wall face, door_frame or window_frame
    u32     indices_first; // into w_room_mesh_indices
    u32     indices_count;
};

struct W_VisualVertex
{
    Vec3 position;
    Vec2 uv;
};

struct W_RaycastHit
{
    f32  distance;
    Vec3 point;
    Vec3 normal; // faces the ray origin
};

// A room as given to w_parse_room: the JSON text plus the binary and image files it names
struct W_RoomSource
{
    const char* json;
    const u8*   binary;
    usize       binary_size;
    const u8*   image; // PNG or JPEG bytes of the albedo atlas, null for an untextured room
    usize       image_size;
};

// A room compiled into the library from rooms/ (cmake/embed_rooms.cmake)
struct W_EmbeddedRoom
{
    const char*  name;
    W_RoomSource source;
};

// An anchor as the simulated headset stores it: fixed in the room, not in the tracking space
struct W_StoredAnchor
{
    W_Uuid uuid;
    char   room[W_MAX_NAME]; // the room it was saved in, empty for none
    u64    room_hash;        // w_content_hash of that room, 0 for none
    Pose   pose;             // in room space
};

// What one simulated headset keeps for one app (docs/anchors.md)
struct W_Store
{
    Array<W_StoredAnchor> anchors;
    u64                   next_serial; // seeds the uuids of new anchors
    u64                   runs;        // instances created with a per-run origin offset
};

// An anchor in the simulated cloud, with who may load it
struct W_CloudAnchor
{
    W_StoredAnchor anchor;
    u64            owner;  // user id that uploaded it
    Array<u64>     users;  // shared with, besides the owner
    Array<W_Uuid>  groups;
};

// A colocation advertisement, alive while its process holds the file
struct W_Advert
{
    W_Uuid uuid;
    u64    owner;
    u32    size;
    u8     buffer[W_ADVERT_MAX_SIZE];
};

//
// Loading
//

W_World*              w_parse_room          (const W_RoomSource& source);
W_World*              w_load_room           (const char* json_path); // the binary and image are resolved next to it
void                  w_destroy             (W_World* world);
b8                    w_find_room_file      (const char* rooms_dir, const char* name, char* out_path, usize out_size); // <rooms_dir>/<name>/room.json
u32                   w_list_rooms          (const char* rooms_dir, char (*out_names)[W_MAX_NAME], u32 capacity);   // sorted by name
b8                    w_room_name_valid     (const char* name); // [a-z0-9_-], 1 to 63 characters
W_World*              w_open_room           (const char* rooms_dir, const char* name); // <rooms_dir>/<name>, else the compiled-in room; null for neither
u32                   w_list_all_rooms      (const char* rooms_dir, char (*out_names)[W_MAX_NAME], u32 capacity);   // the rooms dir's and the compiled-in, sorted, each once

extern const W_EmbeddedRoom w_embedded_rooms[]; // sorted by name, generated into the build
extern const u32            w_embedded_rooms_count;

//
// Queries
//

const char*           w_name                (const W_World* world);
u64                   w_content_hash        (const W_World* world); // over every file of the room, for recordings

u32                   w_entity_count        (const W_World* world);
const W_Entity&       w_entity              (const W_World* world, u32 index);
b8                    w_find_entity         (const W_World* world, const W_Uuid& uuid, u32& out_index);
const Vec2*           w_boundary_points     (const W_World* world);

u32                   w_room_count          (const W_World* world);
const W_Room&         w_room                (const W_World* world, u32 index);
const u32*            w_room_indices        (const W_World* world);
const Vec3*           w_room_mesh_positions (const W_World* world);
const W_RoomMeshFace* w_room_mesh_faces     (const W_World* world);
const u32*            w_room_mesh_indices   (const W_World* world);

void                  w_global_mesh         (const W_World* world, const Vec3*& out_positions, u32& out_positions_count, const u32*& out_indices, u32& out_indices_count);
void                  w_visual_mesh         (const W_World* world, const W_VisualVertex*& out_vertices, u32& out_vertices_count, const u32*& out_indices, u32& out_indices_count);
b8                    w_albedo              (const W_World* world, const u8*& out_rgba, u32& out_width, u32& out_height); // sRGB, false for an untextured room

// Nearest hit with the global mesh, in room space, within (0, max_distance]
b8                    w_raycast             (const W_World* world, Vec3 origin, Vec3 direction, f32 max_distance, W_RaycastHit& out_hit);

const char*           w_label_name          (W_Label label);
b8                    w_label_from_name     (const char* name, W_Label& out_label);
b8                    w_uuid_from_string    (const char* str, W_Uuid& out_uuid); // 32 hex digits
b8                    w_uuid_equal          (const W_Uuid& a, const W_Uuid& b);
b8                    w_uuid_is_zero        (const W_Uuid& uuid);
void                  w_uuid_to_string      (const W_Uuid& uuid, char (&out)[W_UUID_SIZE * 2 + 1]); // lowercase hex

//
// QR codes: the patterns of simulated QR markers, for passthrough
//

#define W_QR_MAX_VERSION    10
#define W_QR_MAX_SIZE       (17 + 4 * W_QR_MAX_VERSION)
#define W_QR_MAX_BYTES      213 // byte mode at error correction level M in version 10
#define W_QR_MAX_CODEWORDS  346 // version 10
#define W_QR_MAX_BLOCKS     5
#define W_QR_MAX_BLOCK_DATA 44
#define W_QR_MAX_ECC        26

// Byte mode at error correction level M (ISO/IEC 18004), the smallest version that holds the
// text and the mask with the lowest penalty; without the quiet zone
struct W_QrCode
{
    u32 version;
    u32 size; // modules per side
    u8  modules[W_QR_MAX_SIZE][W_QR_MAX_SIZE]; // [row][column], 1 dark
};

b8                    w_qr_encode           (const char* text, W_QrCode& out_code); // false for null or more than W_QR_MAX_BYTES bytes
u32                   w_qr_size             (const char* text); // modules per side of its code, 0 when it has none

//
// Anchor storage: JSON files, docs/anchors.md
//

void                  w_store_release       (W_Store& store);
b8                    w_store_parse         (const char* json, W_Store& out_store);
void                  w_store_write         (const W_Store& store, Array<char>& out_json); // not null terminated
b8                    w_store_load          (const char* path, W_Store& out_store); // a missing file is an empty store
b8                    w_store_save          (const char* path, const W_Store& store); // atomically, creating its directory
b8                    w_store_find          (const W_Store& store, const W_Uuid& uuid, u32& out_index);
void                  w_store_put           (W_Store& store, const W_StoredAnchor& anchor); // replaces the anchor of the same uuid
b8                    w_store_erase         (W_Store& store, const W_Uuid& uuid);
b8                    w_store_key           (const char* app_name, char* out_key, usize size); // [A-Za-z0-9._-], anything else becomes _
W_Uuid                w_anchor_uuid         (const char* app_key, u64 user, u64 serial);

void                  w_cloud_release       (W_CloudAnchor& anchor);
void                  w_cloud_release_all   (Array<W_CloudAnchor>& anchors);
b8                    w_cloud_visible       (const W_CloudAnchor& anchor, u64 user);
b8                    w_cloud_in_group      (const W_CloudAnchor& anchor, const W_Uuid& group);
b8                    w_cloud_save          (const char* dir, const W_CloudAnchor& anchor); // <dir>/<uuid>.json
b8                    w_cloud_load          (const char* dir, const W_Uuid& uuid, W_CloudAnchor& out_anchor);
b8                    w_cloud_erase         (const char* dir, const W_Uuid& uuid);
u32                   w_cloud_list          (const char* dir, Array<W_CloudAnchor>& out_anchors); // every valid entry, sorted by uuid; replaces the contents

OS_LockFile*          w_advert_start        (const char* dir, const W_Advert& advert); // <dir>/<uuid>.json, null on failure
void                  w_advert_stop         (OS_LockFile* file);
u32                   w_advert_list         (const char* dir, Array<W_Advert>& out_adverts); // live adverts sorted by uuid; stale files are deleted

// Stage origin in room space for one run: any yaw, on the floor within W_ORIGIN_OFFSET_MAX_DISTANCE
Pose                  w_origin_offset_for_run (u64 seed, u64 run);
