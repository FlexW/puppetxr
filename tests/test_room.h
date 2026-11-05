#pragma once

// A hand-written 4 x 3 x 2.5 m box room with a table and a window in the -z wall, shared by the
// world and OpenXR tests

#include "world.h"

#include <stdio.h>
#include <string.h>

#define TEST_ROOM_JSON_SIZE 16384
#define TEST_ROOM_BIN_SIZE  4096

#define TEST_ROOM_HALF_X    2.0f
#define TEST_ROOM_HALF_Z    1.5f
#define TEST_ROOM_HEIGHT    2.5f
#define TEST_ROOM_TABLE_Y   0.75f

#define TEST_ROOM_ENTITY_ROOM    0
#define TEST_ROOM_ENTITY_FLOOR   1
#define TEST_ROOM_ENTITY_CEILING 2
#define TEST_ROOM_ENTITY_WALL0   3
#define TEST_ROOM_ENTITY_TABLE   7
#define TEST_ROOM_ENTITY_MESH    8
#define TEST_ROOM_ENTITIES       9

// Room mesh: the shell's eight corners and the window's four, seven faces
#define TEST_ROOM_MESH_POSITIONS 12
#define TEST_ROOM_MESH_FACES     7
#define TEST_ROOM_MESH_WALL0     2 // the -z wall, around the window
#define TEST_ROOM_MESH_WINDOW    6

struct TestRoom
{
    char json[TEST_ROOM_JSON_SIZE];
    u8   bin[TEST_ROOM_BIN_SIZE];
    u32  bin_size;
};

// 2x1 RGBA PNG: red, blue
static const u8 test_room_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0xf4, 0x22, 0x7f, 0x8a, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0x00,
    0x42, 0xff, 0x01, 0x0f, 0xf9, 0x03, 0xfd, 0x85, 0x11, 0x99, 0x76, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline u32 test_room_append (TestRoom& room, const void* data, u32 size)
{
    const u32 offset = room.bin_size;
    memcpy(room.bin + offset, data, size);
    room.bin_size += size;
    return offset;
}

inline void test_room_build (TestRoom& room)
{
    memset(&room, 0, sizeof(room));

    const f32 x = TEST_ROOM_HALF_X;
    const f32 z = TEST_ROOM_HALF_Z;
    const f32 h = TEST_ROOM_HEIGHT;
    const f32 t = TEST_ROOM_TABLE_Y;

    // The shell's eight corners, then the table top
    const f32 positions[12][3] = {
        { -x, 0, -z }, { x, 0, -z }, { x, 0, z }, { -x, 0, z },
        { -x, h, -z }, { x, h, -z }, { x, h, z }, { -x, h, z },
        { -0.1f, t, -0.4f }, { 1.1f, t, -0.4f }, { 1.1f, t, 0.4f }, { -0.1f, t, 0.4f },
    };
    const u32 indices[14][3] = {
        { 0, 1, 2 }, { 0, 2, 3 },             // floor
        { 4, 6, 5 }, { 4, 7, 6 },             // ceiling
        { 0, 4, 5 }, { 0, 5, 1 },             // -z wall
        { 3, 2, 6 }, { 3, 6, 7 },             // +z wall
        { 0, 3, 7 }, { 0, 7, 4 },             // -x wall
        { 1, 5, 6 }, { 1, 6, 2 },             // +x wall
        { 8, 10, 9 }, { 8, 11, 10 },          // table top
    };

    f32 visual[12][5] = {};
    for (u32 i = 0; i < 12; i++)
    {
        visual[i][0] = positions[i][0];
        visual[i][1] = positions[i][1];
        visual[i][2] = positions[i][2];
        visual[i][3] = i < 8 ? 0.25f : 0.75f; // the shell samples the red texel, the table top the blue one
        visual[i][4] = 0.5f;
    }

    const u32 visual_offset   = test_room_append(room, visual, sizeof(visual));
    const u32 visual_indices  = test_room_append(room, indices, sizeof(indices));
    const u32 mesh_positions  = test_room_append(room, positions, sizeof(positions));
    const u32 mesh_indices    = test_room_append(room, indices, sizeof(indices));

    // Counter-clockwise seen from inside the room; the window spans x -0.5..0.5, y 1..2
    const f32 room_positions[TEST_ROOM_MESH_POSITIONS][3] = {
        { -x, 0, -z }, { x, 0, -z }, { x, 0, z }, { -x, 0, z },
        { -x, h, -z }, { x, h, -z }, { x, h, z }, { -x, h, z },
        { -0.5f, 1.0f, -z }, { 0.5f, 1.0f, -z }, { 0.5f, 2.0f, -z }, { -0.5f, 2.0f, -z },
    };
    const u32 floor_tris[6]   = { 0, 3, 2, 0, 2, 1 };
    const u32 ceiling_tris[6] = { 4, 5, 6, 4, 6, 7 };
    const u32 wall0_tris[24]  = { 0, 1, 9, 0, 9, 8, 1, 5, 10, 1, 10, 9, 5, 4, 11, 5, 11, 10, 4, 0, 8, 4, 8, 11 };
    const u32 wall1_tris[6]   = { 1, 2, 6, 1, 6, 5 };
    const u32 wall2_tris[6]   = { 2, 3, 7, 2, 7, 6 };
    const u32 wall3_tris[6]   = { 3, 0, 4, 3, 4, 7 };
    const u32 window_tris[6]  = { 8, 9, 10, 8, 10, 11 };
    const u32 rm_positions = test_room_append(room, room_positions, sizeof(room_positions));
    const u32 rm_floor     = test_room_append(room, floor_tris, sizeof(floor_tris));
    const u32 rm_ceiling   = test_room_append(room, ceiling_tris, sizeof(ceiling_tris));
    const u32 rm_wall0     = test_room_append(room, wall0_tris, sizeof(wall0_tris));
    const u32 rm_wall1     = test_room_append(room, wall1_tris, sizeof(wall1_tris));
    const u32 rm_wall2     = test_room_append(room, wall2_tris, sizeof(wall2_tris));
    const u32 rm_wall3     = test_room_append(room, wall3_tris, sizeof(wall3_tris));
    const u32 rm_window    = test_room_append(room, window_tris, sizeof(window_tris));

    (void)snprintf(room.json, sizeof(room.json),
        "{\n"
        "  \"format\": \"puppetxr_room\",\n"
        "  \"version\": 1,\n"
        "  \"name\": \"test_box\",\n"
        "  \"binary\": \"room.bin\",\n"
        "  \"albedo\": \"room_albedo.png\",\n"
        "  \"visual\": { \"vertices\": { \"offset\": %u, \"count\": 12 }, \"indices\": { \"offset\": %u, \"count\": 42 } },\n"
        "  \"global_mesh\": { \"positions\": { \"offset\": %u, \"count\": 12 }, \"indices\": { \"offset\": %u, \"count\": 42 } },\n"
        "  \"entities\": [\n"
        "    { \"uuid\": \"a0000000000000000000000000000000\", \"label\": \"other\", \"position\": [0, 0, 0], \"orientation\": [-0.70710678, 0, 0, 0.70710678],\n"
        "      \"room\": { \"floor\": 1, \"ceiling\": 2, \"walls\": [3, 4, 5, 6], \"children\": [1, 2, 3, 4, 5, 6, 7, 8] },\n"
        "      \"room_mesh\": { \"positions\": { \"offset\": %u, \"count\": 12 }, \"faces\": [\n"
        "        { \"uuid\": \"b0000000000000000000000000000001\", \"label\": \"floor\", \"indices\": { \"offset\": %u, \"count\": 6 } },\n"
        "        { \"uuid\": \"b0000000000000000000000000000002\", \"label\": \"ceiling\", \"indices\": { \"offset\": %u, \"count\": 6 } },\n"
        "        { \"uuid\": \"b0000000000000000000000000000003\", \"label\": \"wall_face\", \"indices\": { \"offset\": %u, \"count\": 24 } },\n"
        "        { \"uuid\": \"b0000000000000000000000000000004\", \"label\": \"wall_face\", \"indices\": { \"offset\": %u, \"count\": 6 } },\n"
        "        { \"uuid\": \"b0000000000000000000000000000005\", \"label\": \"wall_face\", \"indices\": { \"offset\": %u, \"count\": 6 } },\n"
        "        { \"uuid\": \"b0000000000000000000000000000006\", \"label\": \"invisible_wall_face\", \"indices\": { \"offset\": %u, \"count\": 6 } },\n"
        "        { \"uuid\": \"b0000000000000000000000000000007\", \"parent\": \"b0000000000000000000000000000003\", \"label\": \"window_frame\", \"indices\": { \"offset\": %u, \"count\": 6 } } ] } },\n"
        "    { \"uuid\": \"a0000000000000000000000000000001\", \"label\": \"floor\", \"position\": [0, 0, 0], \"orientation\": [-0.70710678, 0, 0, 0.70710678],\n"
        "      \"plane\": { \"min\": [-2, -1.5], \"max\": [2, 1.5], \"boundary\": [[-2, -1.5], [2, -1.5], [2, 1.5], [-2, 1.5]] } },\n"
        "    { \"uuid\": \"a0000000000000000000000000000002\", \"label\": \"ceiling\", \"position\": [0, 2.5, 0], \"orientation\": [0.70710678, 0, 0, 0.70710678],\n"
        "      \"plane\": { \"min\": [-2, -1.5], \"max\": [2, 1.5], \"boundary\": [[-2, -1.5], [2, -1.5], [2, 1.5], [-2, 1.5]] } },\n"
        "    { \"uuid\": \"a0000000000000000000000000000003\", \"label\": \"wall_face\", \"position\": [0, 1.25, -1.5], \"orientation\": [0, 0, 0, 1],\n"
        "      \"plane\": { \"min\": [-2, -1.25], \"max\": [2, 1.25] } },\n"
        "    { \"uuid\": \"a0000000000000000000000000000004\", \"label\": \"wall_face\", \"position\": [2, 1.25, 0], \"orientation\": [0, -0.70710678, 0, 0.70710678],\n"
        "      \"plane\": { \"min\": [-1.5, -1.25], \"max\": [1.5, 1.25] } },\n"
        "    { \"uuid\": \"a0000000000000000000000000000005\", \"label\": \"wall_face\", \"position\": [0, 1.25, 1.5], \"orientation\": [0, 1, 0, 0],\n"
        "      \"plane\": { \"min\": [-2, -1.25], \"max\": [2, 1.25] } },\n"
        "    { \"uuid\": \"a0000000000000000000000000000006\", \"label\": \"invisible_wall_face\", \"position\": [-2, 1.25, 0], \"orientation\": [0, 0.70710678, 0, 0.70710678],\n"
        "      \"plane\": { \"min\": [-1.5, -1.25], \"max\": [1.5, 1.25] } },\n"
        "    { \"uuid\": \"a0000000000000000000000000000007\", \"label\": \"table\", \"position\": [0.5, 0.75, 0], \"orientation\": [-0.70710678, 0, 0, 0.70710678],\n"
        "      \"plane\": { \"min\": [-0.6, -0.4], \"max\": [0.6, 0.4] }, \"volume\": { \"min\": [-0.6, -0.4, -0.75], \"max\": [0.6, 0.4, 0] } },\n"
        "    { \"uuid\": \"a0000000000000000000000000000008\", \"label\": \"global_mesh\", \"position\": [0, 0, 0], \"orientation\": [0, 0, 0, 1], \"mesh\": true }\n"
        "  ]\n"
        "}\n",
        visual_offset, visual_indices, mesh_positions, mesh_indices, rm_positions, rm_floor, rm_ceiling, rm_wall0, rm_wall1, rm_wall2, rm_wall3, rm_window);
}

inline W_RoomSource test_room_source (const TestRoom& room)
{
    return {
        .json        = room.json,
        .binary      = room.bin,
        .binary_size = room.bin_size,
        .image       = test_room_png,
        .image_size  = sizeof(test_room_png),
    };
}

// Replaces the first occurrence of `from`; false when it is missing or the result does not fit
inline bool test_room_replace (TestRoom& room, const char* from, const char* to)
{
    char* at = strstr(room.json, from);
    if (!at)
    {
        return false;
    }
    const size_t from_size = strlen(from);
    const size_t to_size   = strlen(to);
    const size_t tail      = strlen(at + from_size);
    if (strlen(room.json) - from_size + to_size >= sizeof(room.json))
    {
        return false;
    }
    memmove(at + to_size, at + from_size, tail + 1);
    memcpy(at, to, to_size); // NOLINT(bugprone-not-null-terminated-result): the terminator was moved with the tail
    return true;
}

inline bool test_room_write_file (const char* path, const void* data, size_t size)
{
    return os_write_file(path, data, size);
}

// Writes <dir>/<name>/room.json, room.bin and room_albedo.png
inline bool test_room_write (const TestRoom& room, const char* dir, const char* name)
{
    char path[1024];
    os_create_directory(dir);
    (void)cstr_format(path, sizeof(path), "%s/%s", dir, name);
    os_create_directory(path);
    (void)cstr_format(path, sizeof(path), "%s/%s/room.json", dir, name);
    if (!test_room_write_file(path, room.json, strlen(room.json)))
    {
        return false;
    }
    (void)cstr_format(path, sizeof(path), "%s/%s/room.bin", dir, name);
    if (!test_room_write_file(path, room.bin, room.bin_size))
    {
        return false;
    }
    (void)cstr_format(path, sizeof(path), "%s/%s/room_albedo.png", dir, name);
    return test_room_write_file(path, test_room_png, sizeof(test_room_png));
}
