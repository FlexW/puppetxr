#include "world.h"

#include <gtest/gtest.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "test_room.h"

//
// Helpers
//

static W_World* parse (const TestRoom& room)
{
    return w_parse_room(test_room_source(room));
}

static void expect_rejected (TestRoom& room, const char* from, const char* to)
{
    ASSERT_TRUE(test_room_replace(room, from, to)) << from;
    W_World* world = parse(room);
    EXPECT_EQ(world, nullptr) << "accepted after replacing " << from << " with " << to;
    w_destroy(world);
}

static W_Uuid uuid_of (const char* str)
{
    W_Uuid uuid = {};
    EXPECT_TRUE(w_uuid_from_string(str, uuid));
    return uuid;
}

namespace
{

struct WorldTest : testing::Test
{
    TestRoom room = {};

    void SetUp () override
    {
        os_set_assert_mode(OS_AssertMode::Log);
        test_room_build(room);
    }
};

} // namespace

//
// Parsing
//

TEST_F(WorldTest, parses_the_box_room)
{
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);

    EXPECT_STREQ(w_name(world), "test_box");
    ASSERT_EQ(w_entity_count(world), (u32)TEST_ROOM_ENTITIES);
    ASSERT_EQ(w_room_count(world), 1u);

    const W_Room& r = w_room(world, 0);
    EXPECT_EQ(r.entity, (u32)TEST_ROOM_ENTITY_ROOM);
    EXPECT_EQ(r.floor, (u32)TEST_ROOM_ENTITY_FLOOR);
    EXPECT_EQ(r.ceiling, (u32)TEST_ROOM_ENTITY_CEILING);
    ASSERT_EQ(r.walls_count, 4u);
    ASSERT_EQ(r.children_count, 8u);
    const u32* indices = w_room_indices(world);
    for (u32 i = 0; i < 4; i++)
    {
        EXPECT_EQ(indices[r.walls_first + i], TEST_ROOM_ENTITY_WALL0 + i);
    }
    for (u32 i = 0; i < 8; i++)
    {
        EXPECT_EQ(indices[r.children_first + i], 1 + i);
        EXPECT_EQ(w_entity(world, 1 + i).room, 0u);
    }

    const W_Entity& room_entity = w_entity(world, TEST_ROOM_ENTITY_ROOM);
    EXPECT_EQ(room_entity.components, (u32)W_ComponentFlags::Room | (u32)W_ComponentFlags::RoomMesh);

    const W_Entity& floor = w_entity(world, TEST_ROOM_ENTITY_FLOOR);
    EXPECT_EQ(floor.label, W_Label::Floor);
    EXPECT_EQ(floor.components, (u32)W_ComponentFlags::Plane);
    EXPECT_TRUE(zm::near(floor.plane_min, Vec2{ -2.0f, -1.5f }));
    EXPECT_TRUE(zm::near(floor.plane_max, Vec2{ 2.0f, 1.5f }));
    ASSERT_EQ(floor.boundary_count, 4u);
    EXPECT_TRUE(zm::near(w_boundary_points(world)[floor.boundary_first + 2], Vec2{ 2.0f, 1.5f }));
    EXPECT_TRUE(zm::near(zm::rotate(floor.pose.orientation, Vec3{ 0, 0, 1 }), Vec3{ 0, 1, 0 }, 1e-5f));

    const W_Entity& wall = w_entity(world, TEST_ROOM_ENTITY_WALL0);
    EXPECT_EQ(wall.label, W_Label::WallFace);
    EXPECT_EQ(wall.boundary_count, 0u);
    EXPECT_EQ(w_entity(world, TEST_ROOM_ENTITY_WALL0 + 3).label, W_Label::InvisibleWallFace);

    const W_Entity& table = w_entity(world, TEST_ROOM_ENTITY_TABLE);
    EXPECT_EQ(table.label, W_Label::Table);
    EXPECT_EQ(table.components, (u32)W_ComponentFlags::Plane | (u32)W_ComponentFlags::Volume);
    EXPECT_TRUE(zm::near(table.volume_min, Vec3{ -0.6f, -0.4f, -0.75f }));
    EXPECT_TRUE(zm::near(table.volume_max, Vec3{ 0.6f, 0.4f, 0.0f }));

    const W_Entity& mesh = w_entity(world, TEST_ROOM_ENTITY_MESH);
    EXPECT_EQ(mesh.label, W_Label::GlobalMesh);
    EXPECT_EQ(mesh.components, (u32)W_ComponentFlags::Mesh);

    const Vec3* positions       = nullptr;
    const u32*  mesh_indices    = nullptr;
    u32         positions_count = 0;
    u32         indices_count   = 0;
    w_global_mesh(world, positions, positions_count, mesh_indices, indices_count);
    EXPECT_EQ(positions_count, 12u);
    EXPECT_EQ(indices_count, 42u);
    EXPECT_TRUE(zm::near(positions[6], Vec3{ 2.0f, 2.5f, 1.5f }));

    const W_VisualVertex* vertices       = nullptr;
    u32                   vertices_count = 0;
    w_visual_mesh(world, vertices, vertices_count, mesh_indices, indices_count);
    EXPECT_EQ(vertices_count, 12u);
    EXPECT_EQ(indices_count, 42u);
    EXPECT_TRUE(zm::near(vertices[1].uv, Vec2{ 0.25f, 0.5f }));
    EXPECT_TRUE(zm::near(vertices[9].uv, Vec2{ 0.75f, 0.5f }));

    const u8* rgba   = nullptr;
    u32       width  = 0;
    u32       height = 0;
    ASSERT_TRUE(w_albedo(world, rgba, width, height));
    EXPECT_EQ(width, 2u);
    EXPECT_EQ(height, 1u);
    EXPECT_EQ(rgba[0], 255);
    EXPECT_EQ(rgba[2], 0);
    EXPECT_EQ(rgba[4], 0);
    EXPECT_EQ(rgba[6], 255);

    w_destroy(world);
}

TEST_F(WorldTest, finds_entities_by_uuid)
{
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);

    u32 index = 0;
    EXPECT_TRUE(w_find_entity(world, uuid_of("a0000000000000000000000000000007"), index));
    EXPECT_EQ(index, (u32)TEST_ROOM_ENTITY_TABLE);
    EXPECT_FALSE(w_find_entity(world, uuid_of("b0000000000000000000000000000007"), index));
    w_destroy(world);
}

TEST_F(WorldTest, room_without_image_is_untextured)
{
    W_RoomSource source = test_room_source(room);
    source.image        = nullptr;
    source.image_size   = 0;
    W_World* world      = w_parse_room(source);
    ASSERT_NE(world, nullptr);

    const u8* rgba   = nullptr;
    u32       width  = 0;
    u32       height = 0;
    EXPECT_FALSE(w_albedo(world, rgba, width, height));
    w_destroy(world);
}

TEST_F(WorldTest, content_hash_changes_with_any_file)
{
    W_World* a = parse(room);
    W_World* b = parse(room);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(w_content_hash(a), w_content_hash(b));

    room.bin[0] ^= 1;
    W_World* c = parse(room);
    ASSERT_NE(c, nullptr);
    EXPECT_NE(w_content_hash(a), w_content_hash(c));

    W_RoomSource source = test_room_source(room);
    source.image        = nullptr;
    source.image_size   = 0;
    W_World* d          = w_parse_room(source);
    ASSERT_NE(d, nullptr);
    EXPECT_NE(w_content_hash(c), w_content_hash(d));

    w_destroy(a);
    w_destroy(b);
    w_destroy(c);
    w_destroy(d);
}

TEST_F(WorldTest, normalizes_orientations)
{
    ASSERT_TRUE(test_room_replace(room, "\"orientation\": [0, 0, 0, 1],\n      \"plane\": { \"min\": [-2, -1.25]", "\"orientation\": [0, 0, 0, 2],\n      \"plane\": { \"min\": [-2, -1.25]"));
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);
    EXPECT_TRUE(zm::near(w_entity(world, TEST_ROOM_ENTITY_WALL0).pose.orientation, zm::QUAT_IDENTITY));
    w_destroy(world);
}

//
// Room mesh
//

// The room mesh text of the room entity, to drop it from a test room
static void strip_room_mesh (TestRoom& room)
{
    char* start = strstr(room.json, ",\n      \"room_mesh\"");
    ASSERT_NE(start, nullptr);
    char* end = strstr(start, "] } },\n");
    ASSERT_NE(end, nullptr);
    memmove(start, end + 4, strlen(end + 4) + 1);
}

// Offset in the binary of the first position equal to p
static u32 find_position (const TestRoom& room, f32 x, f32 y, f32 z)
{
    const f32 p[3] = { x, y, z };
    for (u32 offset = 0; offset + sizeof(p) <= room.bin_size; offset += 4)
    {
        if (memcmp(room.bin + offset, p, sizeof(p)) == 0)
        {
            return offset;
        }
    }
    return UINT32_MAX;
}

TEST_F(WorldTest, parses_the_room_mesh)
{
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);

    EXPECT_TRUE(w_entity(world, TEST_ROOM_ENTITY_ROOM).components & (u32)W_ComponentFlags::RoomMesh);
    EXPECT_FALSE(w_entity(world, TEST_ROOM_ENTITY_FLOOR).components & (u32)W_ComponentFlags::RoomMesh);

    const W_Room& r = w_room(world, 0);
    ASSERT_EQ(r.mesh_positions_count, (u32)TEST_ROOM_MESH_POSITIONS);
    ASSERT_EQ(r.mesh_faces_count, (u32)TEST_ROOM_MESH_FACES);
    const Vec3*          positions = w_room_mesh_positions(world) + r.mesh_positions_first;
    const W_RoomMeshFace* faces    = w_room_mesh_faces(world) + r.mesh_faces_first;
    const u32*           indices   = w_room_mesh_indices(world);
    EXPECT_TRUE(zm::near(positions[10], Vec3 { 0.5f, 2.0f, -TEST_ROOM_HALF_Z }));

    EXPECT_EQ(faces[0].label, W_Label::Floor);
    EXPECT_EQ(faces[5].label, W_Label::InvisibleWallFace);
    EXPECT_TRUE(w_uuid_is_zero(faces[TEST_ROOM_MESH_WALL0].parent));
    const W_RoomMeshFace& window = faces[TEST_ROOM_MESH_WINDOW];
    EXPECT_EQ(window.label, W_Label::WindowFrame);
    EXPECT_TRUE(w_uuid_equal(window.uuid, uuid_of("b0000000000000000000000000000007")));
    EXPECT_TRUE(w_uuid_equal(window.parent, faces[TEST_ROOM_MESH_WALL0].uuid));
    ASSERT_EQ(window.indices_count, 6u);
    EXPECT_EQ(indices[window.indices_first + 2], 10u); // indices count from the room's first position
    EXPECT_EQ(faces[TEST_ROOM_MESH_WALL0].indices_count, 24u);
    w_destroy(world);
}

TEST_F(WorldTest, rooms_without_a_room_mesh_load)
{
    strip_room_mesh(room);
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);
    EXPECT_FALSE(w_entity(world, TEST_ROOM_ENTITY_ROOM).components & (u32)W_ComponentFlags::RoomMesh);
    EXPECT_EQ(w_room(world, 0).mesh_faces_count, 0u);
    w_destroy(world);
}

TEST_F(WorldTest, rejects_corrupt_room_meshes)
{
    TestRoom label = room, frame_label = room, parent = room, self = room, duplicate = room, range = room, partial = room, few = room, empty = room, planar = room;
    expect_rejected(label, "\"label\": \"ceiling\", \"indices\"", "\"label\": \"table\", \"indices\"");
    expect_rejected(frame_label, "\"label\": \"window_frame\"", "\"label\": \"opening\"");
    expect_rejected(parent, "\"parent\": \"b0000000000000000000000000000003\"", "\"parent\": \"b0000000000000000000000000000009\"");
    expect_rejected(self, "\"parent\": \"b0000000000000000000000000000003\"", "\"parent\": \"b0000000000000000000000000000007\"");
    expect_rejected(duplicate, "\"uuid\": \"b0000000000000000000000000000002\"", "\"uuid\": \"b0000000000000000000000000000001\"");
    expect_rejected(range, "\"count\": 12 }, \"faces\"", "\"count\": 11 }, \"faces\"");
    expect_rejected(partial, "\"label\": \"floor\", \"indices\": { \"offset\": ", "\"label\": \"floor\", \"indices\": { \"count\": 5, \"offset\": ");
    expect_rejected(few, "\"count\": 12 }, \"faces\"", "\"count\": 3 }, \"faces\"");
    expect_rejected(empty, "\"faces\": [\n", "\"faces\": [], \"x\": [\n");

    // Spec XR_META_spatial_entity_room_mesh: every face is planar
    const u32 corner = find_position(planar, 0.5f, 2.0f, -TEST_ROOM_HALF_Z);
    ASSERT_NE(corner, UINT32_MAX);
    const f32 off_wall = -TEST_ROOM_HALF_Z + 0.1f;
    memcpy(planar.bin + corner + 8, &off_wall, sizeof(off_wall));
    W_World* world = parse(planar);
    EXPECT_EQ(world, nullptr);
    w_destroy(world);
}

//
// Rejection of corrupt rooms
//

TEST_F(WorldTest, rejects_malformed_json)
{
    expect_rejected(room, "\"entities\": [", "\"entities\": [[");
}

TEST_F(WorldTest, rejects_wrong_format_or_version)
{
    TestRoom copy = room;
    expect_rejected(room, "\"puppetxr_room\"", "\"something_else\"");
    expect_rejected(copy, "\"version\": 1", "\"version\": 99");
}

TEST_F(WorldTest, rejects_unknown_labels)
{
    expect_rejected(room, "\"label\": \"table\"", "\"label\": \"tabel\"");
}

TEST_F(WorldTest, rejects_bad_uuids)
{
    TestRoom duplicate = room;
    TestRoom zero      = room;
    TestRoom short_    = room;
    expect_rejected(duplicate, "a0000000000000000000000000000002", "a0000000000000000000000000000001");
    expect_rejected(zero, "a0000000000000000000000000000002", "00000000000000000000000000000000");
    expect_rejected(short_, "a0000000000000000000000000000002", "a000000000000000000000000000002");
}

TEST_F(WorldTest, rejects_buffers_past_the_binary)
{
    TestRoom indices = room;
    expect_rejected(room, "\"positions\": { \"offset\": ", "\"positions\": { \"offset\": 9");
    expect_rejected(indices, "\"indices\": { \"offset\": ", "\"indices\": { \"offset\": 3");
}

TEST_F(WorldTest, rejects_indices_out_of_range)
{
    u32* index = (u32*)(room.bin + room.bin_size - sizeof(u32));
    *index     = 12;
    W_World* world = parse(room);
    EXPECT_EQ(world, nullptr);
    w_destroy(world);
}

TEST_F(WorldTest, rejects_non_finite_values)
{
    room.bin[0] = 0xff;
    room.bin[1] = 0xff;
    room.bin[2] = 0xff;
    room.bin[3] = 0x7f; // NaN in the first visual vertex
    W_World* world = parse(room);
    EXPECT_EQ(world, nullptr);
    w_destroy(world);

    TestRoom huge = {};
    test_room_build(huge);
    expect_rejected(huge, "\"position\": [0.5, 0.75, 0]", "\"position\": [0.5, 1e39, 0]");
}

TEST_F(WorldTest, rejects_inconsistent_rooms)
{
    TestRoom not_plane      = room;
    TestRoom out_of_range   = room;
    TestRoom twice          = room;
    TestRoom orphan         = room;
    TestRoom zero_rotation  = room;
    TestRoom bad_boundary   = room;
    TestRoom inverted       = room;
    expect_rejected(not_plane, "\"floor\": 1", "\"floor\": 8");
    expect_rejected(out_of_range, "\"walls\": [3, 4, 5, 6]", "\"walls\": [3, 4, 5, 60]");
    expect_rejected(twice, "\"children\": [1, 2, 3, 4, 5, 6, 7, 8]", "\"children\": [1, 2, 3, 4, 5, 6, 7, 8, 8]");
    expect_rejected(orphan, "\"children\": [1, 2, 3, 4, 5, 6, 7, 8]", "\"children\": [1, 2, 3, 4, 5, 6, 7]");
    expect_rejected(zero_rotation, "\"orientation\": [0, 1, 0, 0]", "\"orientation\": [0, 0, 0, 0]");
    expect_rejected(bad_boundary, "\"boundary\": [[-2, -1.5], [2, -1.5], [2, 1.5], [-2, 1.5]]", "\"boundary\": [[-2, -1.5], [2, -1.5]]");
    expect_rejected(inverted, "\"min\": [-0.6, -0.4], \"max\": [0.6, 0.4]", "\"min\": [0.6, -0.4], \"max\": [-0.6, 0.4]");
}

TEST_F(WorldTest, rejects_a_corrupt_image)
{
    u8 png[sizeof(test_room_png)];
    memcpy(png, test_room_png, sizeof(png));
    png[40] ^= 0xff;
    W_RoomSource source = test_room_source(room);
    source.image        = png;
    W_World* world      = w_parse_room(source);
    EXPECT_EQ(world, nullptr);
    w_destroy(world);
}

TEST_F(WorldTest, survives_every_truncation_of_the_json)
{
    const size_t size = strlen(room.json);
    for (size_t cut = 0; cut < size; cut += 7)
    {
        TestRoom copy = room;
        copy.json[cut] = 0;
        W_World* world = parse(copy);
        w_destroy(world);
    }
}

//
// Raycasts
//

TEST_F(WorldTest, raycast_hits_the_nearest_surface)
{
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);

    W_RaycastHit hit = {};
    ASSERT_TRUE(w_raycast(world, { 0.0f, 1.5f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f, hit));
    EXPECT_FLOAT_EQ(hit.distance, 0.75f); // the table top, not the floor
    EXPECT_TRUE(zm::near(hit.point, Vec3{ 0.0f, 0.75f, 0.0f }));
    EXPECT_TRUE(zm::near(hit.normal, Vec3{ 0.0f, 1.0f, 0.0f }));

    ASSERT_TRUE(w_raycast(world, { -1.0f, 1.5f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f, hit));
    EXPECT_FLOAT_EQ(hit.distance, 1.5f);

    ASSERT_TRUE(w_raycast(world, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, 10.0f, hit));
    EXPECT_FLOAT_EQ(hit.distance, 1.5f);
    EXPECT_TRUE(zm::near(hit.normal, Vec3{ 0.0f, 0.0f, 1.0f }));

    w_destroy(world);
}

TEST_F(WorldTest, raycast_respects_max_distance_and_rejects_bad_rays)
{
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);

    W_RaycastHit hit = {};
    EXPECT_FALSE(w_raycast(world, { -1.0f, 1.5f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 1.0f, hit));
    EXPECT_FALSE(w_raycast(world, { -1.0f, 1.5f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 10.0f, hit));
    EXPECT_FALSE(w_raycast(world, { -1.0f, 1.5f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 0.0f, hit));
    EXPECT_FALSE(w_raycast(world, { -1.0f, 1.5f, 0.0f }, { 0.0f, INFINITY, 0.0f }, 10.0f, hit));
    EXPECT_FALSE(w_raycast(world, { 10.0f, 1.5f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 100.0f, hit));
    w_destroy(world);
}

// The box room with `positions` and `indices` as its global mesh, appended to its binary; the
// visual mesh stays the box
static W_World* parse_with_global_mesh (TestRoom& room, const Array<f32>& positions, const Array<u32>& indices)
{
    const u32 verts = (u32)positions.length / 3;
    const u32 tris  = (u32)indices.length / 3;
    static u8 big[TEST_ROOM_BIN_SIZE * 16];
    if (room.bin_size + verts * 12 + tris * 12 > (u32)sizeof(big))
    {
        return nullptr;
    }
    memcpy(big, room.bin, room.bin_size);
    u32       big_size         = room.bin_size;
    const u32 positions_offset = big_size;
    memcpy(big + big_size, positions.data, (usize)verts * 12);
    big_size += verts * 12;
    const u32 indices_offset = big_size;
    memcpy(big + big_size, indices.data, (usize)tris * 12);
    big_size += tris * 12;

    char from[128];
    char to[128];
    const char* positions_key = strstr(room.json, "\"global_mesh\"");
    if (!positions_key)
    {
        return nullptr;
    }
    const char* positions_offset_key = strstr(positions_key, "\"offset\": ");
    if (!positions_offset_key)
    {
        return nullptr;
    }
    const char* indices_offset_key = strstr(positions_offset_key + 1, "\"offset\": ");
    if (!indices_offset_key)
    {
        return nullptr;
    }
    const u32 old_positions = (u32)strtoul(positions_offset_key + 10, nullptr, 10);
    const u32 old_indices   = (u32)strtoul(indices_offset_key + 10, nullptr, 10);
    (void)snprintf(from, sizeof(from), "\"positions\": { \"offset\": %u, \"count\": 12 }, \"indices\": { \"offset\": %u, \"count\": 42 }", old_positions, old_indices);
    (void)snprintf(to, sizeof(to), "\"positions\": { \"offset\": %u, \"count\": %u }, \"indices\": { \"offset\": %u, \"count\": %u }", positions_offset, verts, indices_offset, tris * 3);
    if (!test_room_replace(room, from, to))
    {
        return nullptr;
    }

    W_RoomSource source = test_room_source(room);
    source.binary       = big;
    source.binary_size  = big_size;
    return w_parse_room(source);
}

TEST_F(WorldTest, raycast_matches_brute_force_on_a_dense_mesh)
{
    // A 32x32 grid of bumps as the global mesh so the BVH has depth
    const u32 n          = 32;
    const u32 tris       = n * n * 2;
    Array<f32> positions = {};
    Array<u32> indices   = {};
    defer {
        positions.release();
        indices.release();
    };
    u32 seed = 12345;
    for (u32 j = 0; j <= n; j++)
    {
        for (u32 i = 0; i <= n; i++)
        {
            seed = seed * 1664525u + 1013904223u;
            positions.add(-2.0f + 4.0f * (f32)i / (f32)n);
            positions.add(0.2f * (f32)(seed >> 8) / (f32)(1u << 24));
            positions.add(-1.5f + 3.0f * (f32)j / (f32)n);
        }
    }
    for (u32 j = 0; j < n; j++)
    {
        for (u32 i = 0; i < n; i++)
        {
            const u32 a = j * (n + 1) + i;
            indices.add(a);
            indices.add(a + 1);
            indices.add(a + n + 2);
            indices.add(a);
            indices.add(a + n + 2);
            indices.add(a + n + 1);
        }
    }

    W_World* world = parse_with_global_mesh(room, positions, indices);
    ASSERT_NE(world, nullptr);

    const Vec3* p     = (const Vec3*)positions.data;
    u32         rays  = 0;
    u32         hits  = 0;
    for (u32 k = 0; k < 2000; k++)
    {
        seed = seed * 1664525u + 1013904223u;
        const f32 ox = -2.5f + 5.0f * (f32)(seed >> 8) / (f32)(1u << 24);
        seed = seed * 1664525u + 1013904223u;
        const f32 oz = -2.0f + 4.0f * (f32)(seed >> 8) / (f32)(1u << 24);
        seed = seed * 1664525u + 1013904223u;
        const f32 dx = -0.5f + (f32)(seed >> 8) / (f32)(1u << 24);
        const Vec3 origin    = { ox, 1.0f, oz };
        const Vec3 direction = zm::normalize(Vec3{ dx, -1.0f, dx * 0.5f });

        f32 best = zm::F32_MAX;
        for (u32 t = 0; t < tris; t++)
        {
            const Vec3 a  = p[indices[t * 3 + 0]];
            const Vec3 b  = p[indices[t * 3 + 1]];
            const Vec3 c  = p[indices[t * 3 + 2]];
            const Vec3 e1 = b - a;
            const Vec3 e2 = c - a;
            const Vec3 pv = zm::cross(direction, e2);
            const f32  det = zm::dot(e1, pv);
            if (zm::abs(det) < 1e-12f)
            {
                continue;
            }
            const f32  inv = 1.0f / det;
            const Vec3 tv  = origin - a;
            const f32  u   = zm::dot(tv, pv) * inv;
            const Vec3 qv  = zm::cross(tv, e1);
            const f32  v   = zm::dot(direction, qv) * inv;
            const f32  d   = zm::dot(e2, qv) * inv;
            if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f && d > 0.0f && d < best)
            {
                best = d;
            }
        }

        W_RaycastHit hit = {};
        const b8     got = w_raycast(world, origin, direction, 100.0f, hit);
        rays++;
        if (best < zm::F32_MAX)
        {
            hits++;
            ASSERT_TRUE(got) << "ray " << k;
            EXPECT_NEAR(hit.distance, best, 1e-5f) << "ray " << k;
        }
        else
        {
            EXPECT_FALSE(got) << "ray " << k;
        }
    }
    EXPECT_GT(hits, rays / 2);
    w_destroy(world);
}

// A ray along an axis through a vertex or an edge of a flat grid lies in the planes of BVH node
// faces, where the slab test multiplied 0 by an infinite inverse direction; the stage origin of
// a room scanned in small triangles is such a vertex
TEST_F(WorldTest, raycast_along_an_axis_hits_vertices_and_edges)
{
    const u32  n         = 8;
    Array<f32> positions = {};
    Array<u32> indices   = {};
    defer {
        positions.release();
        indices.release();
    };
    for (u32 j = 0; j <= n; j++)
    {
        for (u32 i = 0; i <= n; i++)
        {
            positions.add(-1.0f + 2.0f * (f32)i / (f32)n);
            positions.add(0.0f);
            positions.add(-1.0f + 2.0f * (f32)j / (f32)n);
        }
    }
    for (u32 j = 0; j < n; j++)
    {
        for (u32 i = 0; i < n; i++)
        {
            const u32 a = j * (n + 1) + i;
            indices.add(a);
            indices.add(a + 1);
            indices.add(a + n + 2);
            indices.add(a);
            indices.add(a + n + 2);
            indices.add(a + n + 1);
        }
    }
    W_World* world = parse_with_global_mesh(room, positions, indices);
    ASSERT_NE(world, nullptr);

    for (u32 j = 0; j <= 2 * n; j++)
    {
        for (u32 i = 0; i <= 2 * n; i++)
        {
            const f32    x   = -1.0f + (f32)i / (f32)n;
            const f32    z   = -1.0f + (f32)j / (f32)n;
            W_RaycastHit hit = {};
            ASSERT_TRUE(w_raycast(world, { x, 1.0f, z }, { 0.0f, -1.0f, 0.0f }, 10.0f, hit)) << x << " " << z;
            EXPECT_FLOAT_EQ(hit.distance, 1.0f) << x << " " << z;
        }
    }
    W_RaycastHit hit = {};
    EXPECT_TRUE(w_raycast(world, { 0.0f, 0.5f, -2.0f }, { 0.0f, -0.5f, 1.0f }, 10.0f, hit)) << "a slanted ray in the plane x = 0";
    w_destroy(world);
}

//
// Environment depth
//

#define DEPTH_NEAR 0.1f

// The distance along the view's -Z to the box room's visual mesh (shell and table top) through
// pixel (x, y), row 0 at the bottom; the ray starts inside the room
static f32 box_room_depth (const Pose& view, const Fov& fov, u32 width, u32 height, u32 x, u32 y, b8& out_table)
{
    const f32  tl  = zm::tan(fov.angle_left);
    const f32  tr  = zm::tan(fov.angle_right);
    const f32  tu  = zm::tan(fov.angle_up);
    const f32  td  = zm::tan(fov.angle_down);
    const Vec3 dir = zm::rotate(view.orientation, { tl + (tr - tl) * ((f32)x + 0.5f) / (f32)width, td + (tu - td) * ((f32)y + 0.5f) / (f32)height, -1.0f });
    const Vec3 o   = view.position;
    f32        best = INFINITY;
    out_table       = false;
    const f32  lo[3] = { -TEST_ROOM_HALF_X, 0.0f, -TEST_ROOM_HALF_Z };
    const f32  hi[3] = { TEST_ROOM_HALF_X, TEST_ROOM_HEIGHT, TEST_ROOM_HALF_Z };
    const f32  oa[3] = { o.x, o.y, o.z };
    const f32  da[3] = { dir.x, dir.y, dir.z };
    for (u32 axis = 0; axis < 3; axis++)
    {
        if (da[axis] != 0.0f)
        {
            const f32 s = ((da[axis] > 0.0f ? hi[axis] : lo[axis]) - oa[axis]) / da[axis];
            best        = s > 0.0f && s < best ? s : best;
        }
    }
    if (dir.y != 0.0f)
    {
        const f32 s = (TEST_ROOM_TABLE_Y - o.y) / dir.y;
        const f32 px = o.x + dir.x * s;
        const f32 pz = o.z + dir.z * s;
        if (s > 0.0f && s < best && px >= -0.1f && px <= 1.1f && pz >= -0.4f && pz <= 0.4f)
        {
            best      = s;
            out_table = true;
        }
    }
    return best;
}

static u16 depth_value (f32 z)
{
    return (u16)zm::round((1.0f - DEPTH_NEAR / z) * 65535.0f);
}

TEST_F(WorldTest, trace_depth_matches_the_box_room_analytically)
{
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);
    const Fov fov = { -0.9424778f, 0.6981317f, 0.7679449f, -0.9599311f }; // Quest 3's left eye
    const Pose views[] = {
        { .position = { 0.0f, 1.6f, 0.0f }, .orientation = zm::QUAT_IDENTITY },
        { .position = { 0.5f, 1.6f, 1.2f }, .orientation = zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -0.7f) },
        { .position = { 1.5f, 1.8f, -1.0f }, .orientation = zm::normalize(zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 2.2f) * zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -0.9f)) },
    };
    const u32 width  = 40;
    const u32 height = 30;
    u16       image[width * height];
    for (u32 v = 0; v < sizeof(views) / sizeof(views[0]); v++)
    {
        w_trace_depth(world, nullptr, 0, views[v], fov, DEPTH_NEAR, width, height, 0, height, image);
        u32 table = 0;
        for (u32 y = 0; y < height; y++)
        {
            for (u32 x = 0; x < width; x++)
            {
                b8        on_table = false;
                const f32 z        = box_room_depth(views[v], fov, width, height, x, y, on_table);
                EXPECT_NEAR(image[y * width + x], depth_value(z), 1) << "view " << v << " pixel " << x << ", " << y;
                table += on_table ? 1u : 0u;
            }
        }
        EXPECT_TRUE(v != 1 || table > 0) << "the second view looks down at the table";
    }

    // Row 0 looks down: level at 1.6 m, the floor 1.6 m below is farther than the ceiling 0.9 m above
    w_trace_depth(world, nullptr, 0, views[0], fov, DEPTH_NEAR, width, height, 0, height, image);
    EXPECT_GT(image[width / 2], image[(height - 1) * width + width / 2]);
    w_destroy(world);
}

TEST_F(WorldTest, trace_depth_sees_the_visual_mesh_not_the_global_mesh)
{
    // A global mesh floor at 1 m that the visual mesh does not have
    Array<f32> positions = {};
    Array<u32> indices   = {};
    defer {
        positions.release();
        indices.release();
    };
    const f32 corners[4][3] = { { -2.0f, 1.0f, -1.5f }, { 2.0f, 1.0f, -1.5f }, { 2.0f, 1.0f, 1.5f }, { -2.0f, 1.0f, 1.5f } };
    for (const auto& c : corners)
    {
        positions.add(c[0]);
        positions.add(c[1]);
        positions.add(c[2]);
    }
    for (u32 i : { 0u, 1u, 2u, 0u, 2u, 3u })
    {
        indices.add(i);
    }
    W_World* world = parse_with_global_mesh(room, positions, indices);
    ASSERT_NE(world, nullptr);

    // Straight down from 1.6 m over free floor: 1.6 m to the visual floor, not 0.6 m
    const Pose view  = { .position = { -1.0f, 1.6f, 1.0f }, .orientation = zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -zm::PI / 2.0f) };
    const Fov  fov   = { -0.1f, 0.1f, 0.1f, -0.1f };
    u16        image = 0;
    w_trace_depth(world, nullptr, 0, view, fov, DEPTH_NEAR, 1, 1, 0, 1, &image);
    EXPECT_NEAR(image, depth_value(1.6f), 1);
    W_RaycastHit hit = {};
    ASSERT_TRUE(w_raycast(world, view.position, { 0.0f, -1.0f, 0.0f }, 10.0f, hit));
    EXPECT_NEAR(hit.distance, 0.6f, 1e-5f);
    w_destroy(world);
}

TEST_F(WorldTest, trace_depth_rows_split_the_image_and_near_and_empty_have_their_values)
{
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);
    const Fov  fov  = { -0.8f, 0.8f, 0.8f, -0.8f };
    const Pose view = { .position = { 0.3f, 1.2f, 0.2f }, .orientation = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.7f) };
    const u32  size = 16;
    u16        whole[size * size];
    u16        split[size * size];
    memset(split, 0xab, sizeof(split));
    w_trace_depth(world, nullptr, 0, view, fov, DEPTH_NEAR, size, size, 0, size, whole);
    w_trace_depth(world, nullptr, 0, view, fov, DEPTH_NEAR, size, size, 0, 5, split);
    w_trace_depth(world, nullptr, 0, view, fov, DEPTH_NEAR, size, size, 5, 11, split);
    EXPECT_EQ(memcmp(whole, split, sizeof(whole)), 0);

    // Rows past the image are not written
    u16 guard[size * size + 1];
    guard[size * size] = 0x1234;
    w_trace_depth(world, nullptr, 0, view, fov, DEPTH_NEAR, size, size, 10, 100, guard);
    EXPECT_EQ(guard[size * size], 0x1234);

    // 5 cm from the -z wall, nearer than the near plane: 0
    const Pose close = { .position = { 0.0f, 1.2f, -TEST_ROOM_HALF_Z + 0.05f }, .orientation = zm::QUAT_IDENTITY };
    w_trace_depth(world, nullptr, 0, close, fov, DEPTH_NEAR, size, size, 0, size, whole);
    for (u16 value : whole)
    {
        ASSERT_EQ(value, 0);
    }

    // Outside the room looking away from it: nothing is hit, 65535
    const Pose outside = { .position = { 0.0f, 1.2f, TEST_ROOM_HALF_Z + 1.0f }, .orientation = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, zm::PI) };
    w_trace_depth(world, nullptr, 0, outside, fov, DEPTH_NEAR, size, size, 0, size, whole);
    for (u16 value : whole)
    {
        ASSERT_EQ(value, 65535);
    }

    // A pose that is not finite sees nothing rather than crashing
    const Pose broken = { .position = { NAN, 1.2f, 0.0f }, .orientation = zm::QUAT_IDENTITY };
    w_trace_depth(world, nullptr, 0, broken, fov, DEPTH_NEAR, size, size, 0, size, whole);
    for (u16 value : whole)
    {
        ASSERT_EQ(value, 65535);
    }
    w_destroy(world);
}

// Environment raycasts trace the texels they read one at a time
TEST_F(WorldTest, a_traced_texel_is_the_one_in_the_traced_image)
{
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);
    const Fov  fov    = { -0.9f, 0.7f, 0.8f, -0.95f };
    const Pose view   = { .position = { 0.3f, 1.2f, 0.2f }, .orientation = zm::quat_from_axis_angle(zm::normalize(Vec3{ 0.3f, 1.0f, 0.1f }), 0.7f) };
    const u32  width  = 24;
    const u32  height = 17;
    u16        image[width * height];
    w_trace_depth(world, nullptr, 0, view, fov, DEPTH_NEAR, width, height, 0, height, image);
    for (u32 y = 0; y < height; y++)
    {
        for (u32 x = 0; x < width; x++)
        {
            ASSERT_EQ(w_trace_depth_texel(world, nullptr, 0, view, fov, DEPTH_NEAR, width, height, x, y), image[y * width + x]) << x << " " << y;
        }
    }
    const Pose broken = { .position = { NAN, 1.2f, 0.0f }, .orientation = zm::QUAT_IDENTITY };
    EXPECT_EQ(w_trace_depth_texel(world, nullptr, 0, broken, fov, DEPTH_NEAR, width, height, 3, 4), 65535);
    w_destroy(world);
}

//
// Names
//

TEST(World, label_names_round_trip)
{
    for (u32 i = 0; i < (u32)W_Label::Count; i++)
    {
        W_Label label = W_Label::Count;
        ASSERT_TRUE(w_label_from_name(w_label_name((W_Label)i), label)) << i;
        EXPECT_EQ((u32)label, i);
    }
    W_Label label = W_Label::Count;
    EXPECT_FALSE(w_label_from_name("FLOOR", label));
    EXPECT_FALSE(w_label_from_name(nullptr, label));
    EXPECT_STREQ(w_label_name(W_Label::WallFace), "wall_face");
    EXPECT_STREQ(w_label_name(W_Label::Count), "");
}

TEST(World, uuid_strings)
{
    W_Uuid uuid = {};
    ASSERT_TRUE(w_uuid_from_string("00112233445566778899AABBccddeeff", uuid));
    EXPECT_EQ(uuid.bytes[0], 0x00);
    EXPECT_EQ(uuid.bytes[10], 0xaa);
    EXPECT_EQ(uuid.bytes[15], 0xff);
    EXPECT_FALSE(w_uuid_from_string("00112233445566778899aabbccddeef", uuid));
    EXPECT_FALSE(w_uuid_from_string("00112233445566778899aabbccddeeffa", uuid));
    EXPECT_FALSE(w_uuid_from_string("0011223344556677889gaabbccddeeff", uuid));
    EXPECT_FALSE(w_uuid_from_string(nullptr, uuid));
    EXPECT_TRUE(w_uuid_is_zero(W_Uuid{}));
}

//
// Files
//

TEST_F(WorldTest, loads_and_lists_rooms_on_disk)
{
    char dir[OS_MAX_PATH] = {};
    ASSERT_TRUE(os_create_temp_directory("puppetxr_rooms", dir, sizeof(dir)));
    defer { os_delete_tree(dir); };
    ASSERT_TRUE(test_room_write(room, dir, "zeta"));
    ASSERT_TRUE(test_room_write(room, dir, "alpha"));

    char names[4][W_MAX_NAME] = {};
    ASSERT_EQ(w_list_rooms(dir, names, 4), 2u);
    EXPECT_STREQ(names[0], "alpha");
    EXPECT_STREQ(names[1], "zeta");
    EXPECT_EQ(w_list_rooms(dir, names, 1), 1u);
    EXPECT_EQ(w_list_rooms("/nonexistent/puppetxr", names, 4), 0u);

    char path[OS_MAX_PATH];
    ASSERT_TRUE(w_find_room_file(dir, "alpha", path, sizeof(path)));
    EXPECT_FALSE(w_find_room_file(dir, "missing", path, sizeof(path)));
    EXPECT_FALSE(w_find_room_file(dir, "../alpha", path, sizeof(path)));
    EXPECT_FALSE(w_find_room_file(dir, "Alpha", path, sizeof(path)));
    EXPECT_TRUE(w_room_name_valid("living_room-2"));
    EXPECT_FALSE(w_room_name_valid(""));
    EXPECT_FALSE(w_room_name_valid(nullptr));
    EXPECT_FALSE(w_room_name_valid("a/b"));

    ASSERT_TRUE(w_find_room_file(dir, "zeta", path, sizeof(path)));
    W_World* world = w_load_room(path);
    ASSERT_NE(world, nullptr);
    EXPECT_EQ(w_entity_count(world), (u32)TEST_ROOM_ENTITIES);

    W_World* parsed = parse(room);
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(w_content_hash(world), w_content_hash(parsed));
    w_destroy(parsed);
    w_destroy(world);

    EXPECT_EQ(w_load_room("/nonexistent/puppetxr/room.json"), nullptr);
}

// A rooms dir adds rooms to the compiled-in ones and replaces those of the same name
TEST_F(WorldTest, rooms_dir_adds_to_and_overrides_compiled_in_rooms)
{
    char dir[OS_MAX_PATH] = {};
    ASSERT_TRUE(os_create_temp_directory("puppetxr_rooms_merged", dir, sizeof(dir)));
    defer { os_delete_tree(dir); };
    ASSERT_TRUE(test_room_write(room, dir, "zeta"));
    ASSERT_TRUE(test_room_write(room, dir, "living_room"));

    char names[16][W_MAX_NAME] = {};
    const u32 count = w_list_all_rooms(dir, names, 16);
    ASSERT_EQ(count, w_embedded_rooms_count + 1);
    for (u32 i = 1; i < count; i++)
    {
        EXPECT_LT(strcmp(names[i - 1], names[i]), 0) << "sorted, each once";
    }
    EXPECT_STREQ(names[count - 1], "zeta");
    EXPECT_EQ(w_list_all_rooms(dir, names, 2), 2u);

    W_World* parsed = parse(room);
    ASSERT_NE(parsed, nullptr);
    defer { w_destroy(parsed); };
    W_World* overridden = w_open_room(dir, "living_room");
    ASSERT_NE(overridden, nullptr);
    EXPECT_EQ(w_content_hash(overridden), w_content_hash(parsed));
    w_destroy(overridden);

    W_World* compiled_in = w_open_room(dir, "office");
    ASSERT_NE(compiled_in, nullptr);
    EXPECT_STREQ(w_name(compiled_in), "office");
    w_destroy(compiled_in);
    EXPECT_EQ(w_open_room(dir, "missing"), nullptr);
    EXPECT_EQ(w_open_room(dir, "../zeta"), nullptr);
    EXPECT_EQ(w_open_room(nullptr, "zeta"), nullptr);
}

//
// The rooms that ship with puppetxr
//

// The library carries every room of rooms/, byte for byte, so it ships as one file
TEST(World, compiled_in_rooms_are_the_source_rooms)
{
    char names[16][W_MAX_NAME] = {};
    const u32 count = w_list_rooms(PUPPETXR_SOURCE_ROOMS_DIR, names, 16);
    ASSERT_EQ(w_embedded_rooms_count, count);
    char all[16][W_MAX_NAME] = {};
    ASSERT_EQ(w_list_all_rooms("/nonexistent/puppetxr", all, 16), count);
    for (u32 i = 0; i < count; i++)
    {
        EXPECT_STREQ(all[i], names[i]);
        char path[OS_MAX_PATH];
        ASSERT_TRUE(w_find_room_file(PUPPETXR_SOURCE_ROOMS_DIR, names[i], path, sizeof(path)));
        W_World* file = w_load_room(path);
        W_World* compiled_in = w_open_room("/nonexistent/puppetxr", names[i]);
        ASSERT_NE(file, nullptr) << names[i];
        ASSERT_NE(compiled_in, nullptr) << names[i];
        EXPECT_EQ(w_content_hash(compiled_in), w_content_hash(file)) << names[i];
        w_destroy(compiled_in);
        w_destroy(file);
    }
}

TEST(World, starter_rooms_load_and_make_sense)
{
    char names[16][W_MAX_NAME] = {};
    const u32 count = w_list_rooms(PUPPETXR_SOURCE_ROOMS_DIR, names, 16);
    ASSERT_GE(count, 4u);
    for (u32 i = 0; i < count; i++)
    {
        char path[OS_MAX_PATH];
        ASSERT_TRUE(w_find_room_file(PUPPETXR_SOURCE_ROOMS_DIR, names[i], path, sizeof(path)));
        W_World* world = w_load_room(path);
        ASSERT_NE(world, nullptr) << names[i];
        EXPECT_STREQ(w_name(world), names[i]);
        ASSERT_EQ(w_room_count(world), 1u) << names[i];
        const W_Room& room = w_room(world, 0);
        EXPECT_GE(room.walls_count, 4u) << names[i];
        EXPECT_EQ(w_entity(world, room.floor).label, W_Label::Floor) << names[i];
        EXPECT_EQ(w_entity(world, room.ceiling).label, W_Label::Ceiling) << names[i];

        // The room mesh: floor, ceiling, every wall, and frames cut out of walls
        EXPECT_TRUE(w_entity(world, room.entity).components & (u32)W_ComponentFlags::RoomMesh) << names[i];
        EXPECT_GE(room.mesh_faces_count, 2 + room.walls_count) << names[i];
        u32 frames = 0;
        for (u32 f = 0; f < room.mesh_faces_count; f++)
        {
            const W_RoomMeshFace& face = w_room_mesh_faces(world)[room.mesh_faces_first + f];
            frames += (face.label == W_Label::DoorFrame || face.label == W_Label::WindowFrame) && !w_uuid_is_zero(face.parent) ? 1u : 0u;
        }
        EXPECT_GE(frames, 1u) << names[i];

        const u8* rgba = nullptr;
        u32       width = 0;
        u32       height = 0;
        EXPECT_TRUE(w_albedo(world, rgba, width, height)) << names[i];

        // The stage origin is free floor: a ray down from head height hits the floor
        W_RaycastHit hit = {};
        ASSERT_TRUE(w_raycast(world, { 0.0f, 1.6f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f, hit)) << names[i];
        EXPECT_NEAR(hit.distance, 1.6f, 0.05f) << names[i];
        w_destroy(world);
    }
}

// A headset's scan has no wall-sized triangles; MRUK's DestructibleMesh segments the global mesh
// per triangle and keeps the ones near the floor, so a wall of two triangles never breaks
TEST(World, starter_rooms_global_mesh_has_scan_sized_triangles)
{
    char names[16][W_MAX_NAME] = {};
    const u32 count = w_list_rooms(PUPPETXR_SOURCE_ROOMS_DIR, names, 16);
    ASSERT_GE(count, 4u);
    for (u32 i = 0; i < count; i++)
    {
        char path[OS_MAX_PATH];
        ASSERT_TRUE(w_find_room_file(PUPPETXR_SOURCE_ROOMS_DIR, names[i], path, sizeof(path)));
        W_World* world = w_load_room(path);
        ASSERT_NE(world, nullptr) << names[i];
        const Vec3* positions       = nullptr;
        const u32*  indices         = nullptr;
        u32         positions_count = 0;
        u32         indices_count   = 0;
        w_global_mesh(world, positions, positions_count, indices, indices_count);
        f32 longest = 0.0f;
        for (u32 t = 0; t + 2 < indices_count; t += 3)
        {
            for (u32 e = 0; e < 3; e++)
            {
                longest = zm::max(longest, zm::length(positions[indices[t + e]] - positions[indices[t + (e + 1) % 3]]));
            }
        }
        EXPECT_LE(longest, 0.2f + 1e-3f) << names[i];

        // As on Meta's headsets, the global mesh entity is turned like the floor, local +Z up:
        // MRUK reserves the bottom of the mesh along that axis
        for (u32 e = 0; e < w_entity_count(world); e++)
        {
            const W_Entity& entity = w_entity(world, e);
            if (entity.label == W_Label::GlobalMesh)
            {
                EXPECT_TRUE(zm::near(zm::rotate(entity.pose.orientation, Vec3{ 0.0f, 0.0f, 1.0f }), Vec3{ 0.0f, 1.0f, 0.0f })) << names[i];
            }
        }
        w_destroy(world);
    }
}

//
// Anchor storage
//

static W_StoredAnchor stored_anchor (const char* uuid, const char* room, u64 room_hash, Pose pose)
{
    W_StoredAnchor anchor = { .uuid = uuid_of(uuid), .room_hash = room_hash, .pose = pose };
    cstr_copy(anchor.room, sizeof(anchor.room), room);
    return anchor;
}

static void expect_same_anchor (const W_StoredAnchor& a, const W_StoredAnchor& b)
{
    EXPECT_TRUE(w_uuid_equal(a.uuid, b.uuid));
    EXPECT_STREQ(a.room, b.room);
    EXPECT_EQ(a.room_hash, b.room_hash);
    EXPECT_TRUE(memory_equal(&a.pose, &b.pose, sizeof(Pose))) << "poses round trip bit exactly";
}

static void temp_dir (char (&dir)[OS_MAX_PATH])
{
    ASSERT_TRUE(os_create_temp_directory("puppetxr_store", dir, sizeof(dir)));
}

TEST(World, store_round_trips_through_json)
{
    W_Store store = {};
    store.next_serial = 12;
    store.runs        = 3;
    const Pose odd    = { { 0.1f, 1.0f / 3.0f, -2.75e-5f }, zm::normalize(Quat{ 0.1f, 0.7f, -0.2f, 0.6f }) };
    w_store_put(store, stored_anchor("00000000000000000000000000000001", "bedroom", 0x0123456789abcdefull, odd));
    w_store_put(store, stored_anchor("00000000000000000000000000000002", "", 0, zm::POSE_IDENTITY));

    Array<char> json = {};
    w_store_write(store, json);
    json.add(0);
    W_Store parsed = {};
    ASSERT_TRUE(w_store_parse(json.data, parsed)) << json.data;
    EXPECT_EQ(parsed.next_serial, 12u);
    EXPECT_EQ(parsed.runs, 3u);
    ASSERT_EQ(parsed.anchors.length, 2u);
    expect_same_anchor(parsed.anchors[0], store.anchors[0]);
    expect_same_anchor(parsed.anchors[1], store.anchors[1]);
    json.release();
    w_store_release(parsed);
    w_store_release(store);
}

TEST(World, store_put_replaces_and_erase_removes)
{
    W_Store store = {};
    w_store_put(store, stored_anchor("00000000000000000000000000000001", "office", 1, zm::POSE_IDENTITY));
    w_store_put(store, stored_anchor("00000000000000000000000000000001", "office", 1, { { 1, 2, 3 }, zm::QUAT_IDENTITY }));
    ASSERT_EQ(store.anchors.length, 1u);
    EXPECT_EQ(store.anchors[0].pose.position.y, 2.0f);
    u32 index = 99;
    EXPECT_TRUE(w_store_find(store, uuid_of("00000000000000000000000000000001"), index));
    EXPECT_EQ(index, 0u);
    EXPECT_FALSE(w_store_erase(store, uuid_of("00000000000000000000000000000002")));
    EXPECT_TRUE(w_store_erase(store, uuid_of("00000000000000000000000000000001")));
    EXPECT_EQ(store.anchors.length, 0u);
    EXPECT_FALSE(w_store_find(store, uuid_of("00000000000000000000000000000001"), index));
    w_store_release(store);
}

TEST(World, store_rejects_bad_files)
{
    W_Store store = {};
    EXPECT_FALSE(w_store_parse("{ not json", store));
    EXPECT_FALSE(w_store_parse(R"({ "version": 99, "anchors": [] })", store));
    EXPECT_FALSE(w_store_parse(R"({ "version": 1, "anchors": [ { "uuid": "xyz", "room": "", "room_hash": "0", "pose": [0,0,0,0,0,0,1] } ] })", store));
    EXPECT_FALSE(w_store_parse(R"({ "version": 1, "anchors": [ { "uuid": "00000000000000000000000000000001", "room": "", "room_hash": "0", "pose": [0,0,0,0,0,0] } ] })", store));
    EXPECT_FALSE(w_store_parse(R"({ "version": 1, "anchors": [ { "uuid": "00000000000000000000000000000001", "room": "../x", "room_hash": "0", "pose": [0,0,0,0,0,0,1] } ] })", store));
    EXPECT_FALSE(w_store_parse(R"({ "version": 1, "anchors": [ { "uuid": "00000000000000000000000000000000", "room": "", "room_hash": "0", "pose": [0,0,0,0,0,0,1] } ] })", store));
    ASSERT_TRUE(w_store_parse(R"({ "version": 1, "next_serial": 5, "anchors": [ { "uuid": "00000000000000000000000000000001", "room": "", "room_hash": "0", "pose": [0,0,0,0,0,0,2] } ] })", store));
    EXPECT_EQ(store.next_serial, 5u);
    EXPECT_FLOAT_EQ(store.anchors[0].pose.orientation.w, 1.0f) << "orientations are normalized";
    w_store_release(store);
}

TEST(World, store_files_are_created_and_a_missing_file_is_empty)
{
    char dir[OS_MAX_PATH] = {};
    temp_dir(dir);
    defer { os_delete_tree(dir); };
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/app/user_1.json", dir);

    W_Store store = {};
    ASSERT_TRUE(w_store_load(path, store));
    EXPECT_EQ(store.anchors.length, 0u);
    w_store_put(store, stored_anchor("00000000000000000000000000000003", "office", 7, zm::POSE_IDENTITY));
    store.next_serial = 4;
    ASSERT_TRUE(w_store_save(path, store));

    W_Store loaded = {};
    ASSERT_TRUE(w_store_load(path, loaded));
    ASSERT_EQ(loaded.anchors.length, 1u);
    EXPECT_EQ(loaded.next_serial, 4u);
    w_store_release(loaded);

    ASSERT_TRUE(os_write_file(path, "garbage", 7));
    EXPECT_FALSE(w_store_load(path, loaded));
    EXPECT_EQ(loaded.anchors.length, 0u);
    w_store_release(loaded);
    w_store_release(store);
}

TEST(World, store_keys_are_safe_file_names)
{
    char key[W_MAX_NAME];
    ASSERT_TRUE(w_store_key("com.example.Game", key, sizeof(key)));
    EXPECT_STREQ(key, "com.example.Game");
    ASSERT_TRUE(w_store_key("My Game: Episode/2", key, sizeof(key)));
    EXPECT_STREQ(key, "My_Game__Episode_2");
    ASSERT_TRUE(w_store_key("..", key, sizeof(key)));
    EXPECT_STREQ(key, "__");
    EXPECT_FALSE(w_store_key("", key, sizeof(key)));
    EXPECT_FALSE(w_store_key(nullptr, key, sizeof(key)));
}

TEST(World, anchor_uuids_differ_by_app_user_and_serial)
{
    const W_Uuid a = w_anchor_uuid("game", 1, 1);
    EXPECT_FALSE(w_uuid_is_zero(a));
    EXPECT_TRUE(w_uuid_equal(a, w_anchor_uuid("game", 1, 1)));
    EXPECT_FALSE(w_uuid_equal(a, w_anchor_uuid("game", 1, 2)));
    EXPECT_FALSE(w_uuid_equal(a, w_anchor_uuid("game", 2, 1)));
    EXPECT_FALSE(w_uuid_equal(a, w_anchor_uuid("other", 1, 1)));

    char text[W_UUID_SIZE * 2 + 1];
    w_uuid_to_string(a, text);
    W_Uuid back = {};
    ASSERT_TRUE(w_uuid_from_string(text, back));
    EXPECT_TRUE(w_uuid_equal(a, back));
}

TEST(World, cloud_anchors_carry_their_recipients)
{
    char dir[OS_MAX_PATH] = {};
    temp_dir(dir);
    defer { os_delete_tree(dir); };
    W_CloudAnchor shared = { .anchor = stored_anchor("000000000000000000000000000000c1", "bedroom", 9, { { 1, 0, 2 }, zm::QUAT_IDENTITY }), .owner = 1 };
    shared.users.add(2);
    shared.groups.add(uuid_of("0000000000000000000000000000aaaa"));
    ASSERT_TRUE(w_cloud_save(dir, shared));
    W_CloudAnchor other = { .anchor = stored_anchor("000000000000000000000000000000b2", "", 0, zm::POSE_IDENTITY), .owner = 5 };
    ASSERT_TRUE(w_cloud_save(dir, other));

    EXPECT_TRUE(w_cloud_visible(shared, 1)) << "the owner";
    EXPECT_TRUE(w_cloud_visible(shared, 2));
    EXPECT_FALSE(w_cloud_visible(shared, 3));
    EXPECT_TRUE(w_cloud_in_group(shared, uuid_of("0000000000000000000000000000aaaa")));
    EXPECT_FALSE(w_cloud_in_group(shared, uuid_of("0000000000000000000000000000bbbb")));

    Array<W_CloudAnchor> listed = {};
    ASSERT_EQ(w_cloud_list(dir, listed), 2u);
    EXPECT_TRUE(w_uuid_equal(listed[0].anchor.uuid, other.anchor.uuid)) << "sorted by uuid";
    expect_same_anchor(listed[1].anchor, shared.anchor);
    EXPECT_EQ(listed[1].owner, 1u);
    ASSERT_EQ(listed[1].users.length, 1u);
    EXPECT_EQ(listed[1].users[0], 2u);
    ASSERT_EQ(listed[1].groups.length, 1u);
    w_cloud_release_all(listed);

    W_CloudAnchor loaded = {};
    ASSERT_TRUE(w_cloud_load(dir, shared.anchor.uuid, loaded));
    EXPECT_EQ(loaded.owner, 1u);
    w_cloud_release(loaded);
    EXPECT_TRUE(w_cloud_erase(dir, shared.anchor.uuid));
    EXPECT_FALSE(w_cloud_load(dir, shared.anchor.uuid, loaded));
    EXPECT_EQ(w_cloud_list(dir, listed), 1u);
    w_cloud_release_all(listed);
    EXPECT_EQ(w_cloud_list("/nonexistent/puppetxr", listed), 0u);
    w_cloud_release(shared);
    w_cloud_release(other);
}

TEST(World, adverts_are_listed_while_their_owner_advertises)
{
    char dir[OS_MAX_PATH] = {};
    temp_dir(dir);
    defer { os_delete_tree(dir); };
    const W_Advert advert = { .uuid = uuid_of("00000000000000000000000000000ad1"), .owner = 3, .size = 3, .buffer = { 'a', 'b', 'c' } };
    OS_LockFile* lock = w_advert_start(dir, advert);
    ASSERT_NE(lock, nullptr);

    Array<W_Advert> adverts = {};
    ASSERT_EQ(w_advert_list(dir, adverts), 1u);
    EXPECT_TRUE(w_uuid_equal(adverts[0].uuid, advert.uuid));
    EXPECT_EQ(adverts[0].owner, 3u);
    ASSERT_EQ(adverts[0].size, 3u);
    EXPECT_TRUE(memory_equal(adverts[0].buffer, "abc", 3));

    w_advert_stop(lock);
    EXPECT_EQ(w_advert_list(dir, adverts), 0u);
    adverts.release();
}

TEST(World, per_run_origin_offsets_are_seeded_and_stay_on_the_floor)
{
    const Pose first = w_origin_offset_for_run(7, 1);
    const Pose again = w_origin_offset_for_run(7, 1);
    EXPECT_TRUE(memory_equal(&first, &again, sizeof(Pose)));
    b8 differs = false;
    for (u64 run = 1; run < 20; run++)
    {
        const Pose offset = w_origin_offset_for_run(7, run);
        EXPECT_EQ(offset.position.y, 0.0f);
        EXPECT_LE(zm::length(offset.position), W_ORIGIN_OFFSET_MAX_DISTANCE + 1e-6f);
        EXPECT_EQ(offset.orientation.x, 0.0f);
        EXPECT_EQ(offset.orientation.z, 0.0f);
        EXPECT_NEAR(zm::length(offset.orientation), 1.0f, 1e-6f);
        const Pose other = w_origin_offset_for_run(8, run);
        differs = differs || !memory_equal(&offset, &other, sizeof(Pose));
    }
    EXPECT_TRUE(differs);
}

//
// QR codes
//

static u32 qr_checksum (const W_QrCode& code)
{
    u32 sum = 0;
    for (u32 y = 0; y < code.size; y++)
    {
        for (u32 x = 0; x < code.size; x++)
        {
            sum = code.modules[y][x] ? sum * 31u + y * code.size + x + 1 : sum;
        }
    }
    return sum;
}

// The golden codes were read back by OpenCV 4.10's QRCodeDetector
TEST(World, qr_codes_match_a_decoded_reference)
{
    static const char* const hello[21] = {
        "#######..##...#######", "#.....#.##....#.....#", "#.###.#..#.##.#.###.#", "#.###.#...##..#.###.#",
        "#.###.#.##..#.#.###.#", "#.....#.....#.#.....#", "#######.#.#.#.#######", "..........###........",
        "#.#.#.#..#.#....#..#.", "..#.##....#...#....##", ".#.#..#.###.#...#####", "##..#.........#....#.",
        ".##.#.##..#.#.#.#....", "........####.#.#..###", "#######...##.###..###", "#.....#...####.##....",
        "#.###.#.#.##.###...##", "#.###.#..#....##..##.", "#.###.#.###.#...#.#.#", "#.....#..#....#.#..#.",
        "#######.###.#.##...##",
    };
    static W_QrCode code;
    ASSERT_TRUE(w_qr_encode("hello", code));
    ASSERT_EQ(code.size, 21u);
    EXPECT_EQ(code.version, 1u);
    for (u32 y = 0; y < 21; y++)
    {
        for (u32 x = 0; x < 21; x++)
        {
            EXPECT_EQ(code.modules[y][x], hello[y][x] == '#' ? 1 : 0) << y << ", " << x;
        }
    }

    // Two blocks (version 7 and up also carry version information), and the largest
    char text[W_QR_MAX_BYTES + 2] = {};
    ASSERT_TRUE(w_qr_encode("https://example.com/puppetxr", code));
    EXPECT_EQ(code.size, 29u);
    EXPECT_EQ(qr_checksum(code), 3145696850u);
    for (u32 i = 0; i < 120; i++)
    {
        text[i] = 'a';
    }
    ASSERT_TRUE(w_qr_encode(text, code));
    EXPECT_EQ(code.size, 45u);
    EXPECT_EQ(qr_checksum(code), 3734951354u);
    for (u32 i = 0; i < W_QR_MAX_BYTES; i++)
    {
        text[i] = 'b';
    }
    ASSERT_TRUE(w_qr_encode(text, code));
    EXPECT_EQ(code.size, 57u);
    EXPECT_EQ(qr_checksum(code), 2459218568u);
}

TEST(World, qr_codes_grow_with_their_text)
{
    static W_QrCode code;
    char text[W_QR_MAX_BYTES + 2] = {};
    // Spec: version 1 at level M holds 14 bytes, version 2 holds 26
    const u32 lengths[]  = { 0, 14, 15, 26, 27 };
    const u32 versions[] = { 1, 1, 2, 2, 3 };
    for (u32 i = 0; i < ARRAY_SIZE(lengths); i++)
    {
        for (u32 c = 0; c < lengths[i]; c++)
        {
            text[c] = 'q';
        }
        text[lengths[i]] = 0;
        ASSERT_TRUE(w_qr_encode(text, code)) << lengths[i];
        EXPECT_EQ(code.version, versions[i]) << lengths[i];
        EXPECT_EQ(code.size, 17 + 4 * versions[i]);
        // Finder patterns in three corners and the dark module next to the bottom left one
        EXPECT_EQ(code.modules[0][0], 1);
        EXPECT_EQ(code.modules[3][code.size - 4], 1);
        EXPECT_EQ(code.modules[code.size - 2][1], 0);
        EXPECT_EQ(code.modules[code.size - 8][8], 1);
    }
    for (u32 c = 0; c <= W_QR_MAX_BYTES; c++)
    {
        text[c] = 'q';
    }
    text[W_QR_MAX_BYTES + 1] = 0;
    EXPECT_FALSE(w_qr_encode(text, code));
    EXPECT_FALSE(w_qr_encode(nullptr, code));
    EXPECT_EQ(w_qr_size(text), 0u);
    EXPECT_EQ(w_qr_size(nullptr), 0u);
    EXPECT_EQ(w_qr_size("hello"), 21u);
}

//
// Models
//

// 2 x 2 RGBA: red, green on the top row, blue, white below
static const u8 MODEL_PNG[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08,
    0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x0c,
    0x81, 0x34, 0x18, 0x00, 0x00, 0x49, 0xc8, 0x09, 0xf7, 0x03, 0xd9, 0x64, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

#define CUBE_POSITIONS 0   // 8 x Vec3
#define CUBE_UVS       96  // 8 x Vec2
#define CUBE_INDICES   160 // 36 x u16
#define CUBE_IMAGE     232 // MODEL_PNG
#define CUBE_BIN_SIZE  308

// A unit cube around the origin; the first primitive's material is red, the texture is MODEL_PNG
static const char* const CUBE_JSON =
    "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
    "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"indices\":2,\"material\":0}]}],"
    "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0,0,1]}},{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
    "\"textures\":[{\"source\":0}],\"images\":[{\"bufferView\":3,\"mimeType\":\"image/png\"}],"
    "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":8,\"type\":\"VEC3\",\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
    "{\"bufferView\":1,\"componentType\":5126,\"count\":8,\"type\":\"VEC2\"},{\"bufferView\":2,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}],"
    "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":96},{\"buffer\":0,\"byteOffset\":96,\"byteLength\":64},"
    "{\"buffer\":0,\"byteOffset\":160,\"byteLength\":72},{\"buffer\":0,\"byteOffset\":232,\"byteLength\":75}],"
    "\"buffers\":[{\"byteLength\":308}]}";

static void cube_bin (u8 (&out)[CUBE_BIN_SIZE])
{
    memset(out, 0, sizeof(out));
    f32 positions[8][3];
    f32 uvs[8][2];
    for (u32 v = 0; v < 8; v++)
    {
        positions[v][0] = (v & 1) ? 0.5f : -0.5f;
        positions[v][1] = (v & 2) ? 0.5f : -0.5f;
        positions[v][2] = (v & 4) ? 0.5f : -0.5f;
        uvs[v][0]       = (v & 1) ? 1.0f : 0.0f;
        uvs[v][1]       = (v & 2) ? 1.0f : 0.0f;
    }
    const u16 indices[36] = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
    memcpy(out + CUBE_POSITIONS, positions, sizeof(positions));
    memcpy(out + CUBE_UVS, uvs, sizeof(uvs));
    memcpy(out + CUBE_INDICES, indices, sizeof(indices));
    memcpy(out + CUBE_IMAGE, MODEL_PNG, sizeof(MODEL_PNG));
}

static void put_u32 (Array<u8>& out, u32 value)
{
    for (u32 i = 0; i < 4; i++)
    {
        out.add((u8)(value >> (8 * i)));
    }
}

// glTF 2.0 binary: header, JSON chunk padded with spaces, BIN chunk padded with zeros
static void make_glb (const char* json, const u8* bin, usize bin_size, Array<u8>& out)
{
    const usize json_size   = strlen(json);
    const usize json_padded = (json_size + 3) & ~(usize)3;
    const usize bin_padded  = (bin_size + 3) & ~(usize)3;
    out.clear();
    put_u32(out, 0x46546C67);
    put_u32(out, 2);
    put_u32(out, (u32)(12 + 8 + json_padded + (bin ? 8 + bin_padded : 0)));
    put_u32(out, (u32)json_padded);
    put_u32(out, 0x4E4F534A);
    for (usize i = 0; i < json_padded; i++)
    {
        out.add(i < json_size ? (u8)json[i] : (u8)' ');
    }
    if (bin)
    {
        put_u32(out, (u32)bin_padded);
        put_u32(out, 0x004E4942);
        for (usize i = 0; i < bin_padded; i++)
        {
            out.add(i < bin_size ? bin[i] : (u8)0);
        }
    }
}

// `json` with the first `from` replaced by `to`
static const char* replaced (const char* json, const char* from, const char* to, char (&out)[4096])
{
    const char* at = strstr(json, from);
    EXPECT_NE(at, nullptr) << from;
    if (!at)
    {
        snprintf(out, sizeof(out), "%s", json);
        return out;
    }
    snprintf(out, sizeof(out), "%.*s%s%s", (int)(at - json), json, to, at + strlen(from));
    return out;
}

static void cube_glb (const char* json, Array<u8>& out)
{
    u8 bin[CUBE_BIN_SIZE];
    cube_bin(bin);
    make_glb(json, bin, sizeof(bin), out);
}

static W_Model* load_model (const Array<u8>& file)
{
    Array<u8> glb = {};
    defer { glb.release(); };
    if (!w_pack_model(file.data, file.length, nullptr, glb))
    {
        return nullptr;
    }
    return w_create_model(glb.data, glb.length);
}

static void expect_model_refused (const char* json, const char* why)
{
    Array<u8> file = {};
    Array<u8> glb  = {};
    defer {
        file.release();
        glb.release();
    };
    cube_glb(json, file);
    EXPECT_FALSE(w_pack_model(file.data, file.length, nullptr, glb)) << why;
    W_Model* model = w_create_model(file.data, file.length);
    EXPECT_EQ(model, nullptr) << why;
    w_destroy_model(model);
}

static void base64 (const u8* data, usize size, Array<char>& out)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (usize i = 0; i < size; i += 3)
    {
        const u32 n = (u32)data[i] << 16 | (i + 1 < size ? (u32)data[i + 1] << 8 : 0) | (i + 2 < size ? (u32)data[i + 2] : 0);
        out.add(digits[n >> 18 & 63]);
        out.add(digits[n >> 12 & 63]);
        out.add(i + 1 < size ? digits[n >> 6 & 63] : '=');
        out.add(i + 2 < size ? digits[n & 63] : '=');
    }
}

namespace
{

struct ModelTest : testing::Test
{
    void SetUp () override
    {
        os_set_assert_mode(OS_AssertMode::Log);
    }
};

} // namespace

TEST_F(ModelTest, packs_and_loads_a_glb)
{
    Array<u8> file = {};
    defer { file.release(); };
    cube_glb(CUBE_JSON, file);
    W_Model* model = load_model(file);
    ASSERT_NE(model, nullptr);
    defer { w_destroy_model(model); };

    const W_VisualVertex* vertices       = nullptr;
    const u32*            indices        = nullptr;
    u32                   vertices_count = 0;
    u32                   indices_count  = 0;
    w_model_mesh(model, vertices, vertices_count, indices, indices_count);
    EXPECT_EQ(vertices_count, 8u);
    ASSERT_EQ(indices_count, 36u);
    EXPECT_EQ(vertices[7].position.x, 0.5f);
    EXPECT_EQ(vertices[7].uv.y, 1.0f);
    Vec3 lo = {};
    Vec3 hi = {};
    w_model_bounds(model, lo, hi);
    EXPECT_EQ(lo.x, -0.5f);
    EXPECT_EQ(hi.z, 0.5f);

    const W_ModelPart* parts = nullptr;
    ASSERT_EQ(w_model_parts(model, parts), 1u);
    EXPECT_EQ(parts[0].indices_first, 0u);
    EXPECT_EQ(parts[0].indices_count, 36u);
    ASSERT_LT(parts[0].image, w_model_image_count(model));
    const u8* rgba   = nullptr;
    u32       width  = 0;
    u32       height = 0;
    w_model_image(model, parts[0].image, rgba, width, height);
    ASSERT_EQ(width, (u32)W_MODEL_SOLID_SIZE);
    ASSERT_EQ(height, (u32)W_MODEL_SOLID_SIZE);
    EXPECT_EQ(rgba[0], 255);
    EXPECT_EQ(rgba[1], 0);
    EXPECT_EQ(rgba[2], 0);
    EXPECT_EQ(rgba[3], 255);
}

TEST_F(ModelTest, textures_carry_the_base_colour_times_the_factor)
{
    char      json[4096];
    Array<u8> file = {};
    defer { file.release(); };
    replaced(CUBE_JSON, "\"material\":0", "\"material\":1", json);
    cube_glb(json, file);
    W_Model* model = load_model(file);
    ASSERT_NE(model, nullptr);
    const W_ModelPart* parts = nullptr;
    ASSERT_EQ(w_model_parts(model, parts), 1u);
    const u8* rgba   = nullptr;
    u32       width  = 0;
    u32       height = 0;
    w_model_image(model, parts[0].image, rgba, width, height);
    ASSERT_EQ(width, 2u);
    ASSERT_EQ(height, 2u);
    const u8 expected[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
    EXPECT_EQ(memcmp(rgba, expected, sizeof(expected)), 0);
    w_destroy_model(model);

    // A factor of 0.5 in linear light is 188 in sRGB
    char half[4096];
    replaced(json, "{\"baseColorTexture\":{\"index\":0}}", "{\"baseColorTexture\":{\"index\":0},\"baseColorFactor\":[0.5,1,1,1]}", half);
    cube_glb(half, file);
    model = load_model(file);
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(w_model_parts(model, parts), 1u);
    w_model_image(model, parts[0].image, rgba, width, height);
    EXPECT_NEAR(rgba[0], 188, 1);
    EXPECT_EQ(rgba[4], 0);
    EXPECT_EQ(rgba[13], 255) << "green is untouched";
    w_destroy_model(model);
}

TEST_F(ModelTest, packing_is_deterministic_and_idempotent)
{
    Array<u8> file   = {};
    Array<u8> first  = {};
    Array<u8> second = {};
    Array<u8> again  = {};
    defer {
        file.release();
        first.release();
        second.release();
        again.release();
    };
    cube_glb(CUBE_JSON, file);
    ASSERT_TRUE(w_pack_model(file.data, file.length, nullptr, first));
    ASSERT_TRUE(w_pack_model(file.data, file.length, nullptr, second));
    ASSERT_TRUE(w_pack_model(first.data, first.length, nullptr, again));
    ASSERT_EQ(first.length, second.length);
    EXPECT_EQ(memcmp(first.data, second.data, first.length), 0);
    ASSERT_EQ(first.length, again.length);
    EXPECT_EQ(memcmp(first.data, again.data, first.length), 0) << "a packed model packs to itself";
    EXPECT_EQ(memcmp(first.data, "glTF", 4), 0);
}

TEST_F(ModelTest, a_gltf_with_files_or_data_uris_packs_like_its_glb)
{
    Array<u8>   file   = {};
    Array<u8>   packed = {};
    Array<u8>   other  = {};
    Array<char> uri    = {};
    defer {
        file.release();
        packed.release();
        other.release();
        uri.release();
    };
    u8 bin[CUBE_BIN_SIZE];
    cube_bin(bin);
    cube_glb(CUBE_JSON, file);
    ASSERT_TRUE(w_pack_model(file.data, file.length, nullptr, packed));

    char dir[OS_MAX_PATH];
    ASSERT_TRUE(os_create_temp_directory("puppetxr_model", dir, sizeof(dir)));
    defer { os_delete_tree(dir); };
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/cube data.bin", dir);
    ASSERT_TRUE(os_write_file(path, bin, sizeof(bin)));
    char json[4096];
    replaced(CUBE_JSON, "{\"byteLength\":308}", "{\"byteLength\":308,\"uri\":\"cube%20data.bin\"}", json);
    cstr_format(path, sizeof(path), "%s/cube.gltf", dir);
    ASSERT_TRUE(os_write_file(path, json, strlen(json)));
    ASSERT_TRUE(w_pack_model_file(path, other));
    ASSERT_EQ(other.length, packed.length);
    EXPECT_EQ(memcmp(other.data, packed.data, packed.length), 0);

    // Data URIs need no directory; a file needs one
    const char prefix[] = "{\"byteLength\":308,\"uri\":\"data:application/octet-stream;base64,";
    for (const char* c = prefix; *c; c++)
    {
        uri.add(*c);
    }
    base64(bin, sizeof(bin), uri);
    uri.add('"');
    uri.add('}');
    uri.add(0);
    static char big[8192];
    const char* at = strstr(CUBE_JSON, "{\"byteLength\":308}");
    ASSERT_NE(at, nullptr);
    snprintf(big, sizeof(big), "%.*s%s%s", (int)(at - CUBE_JSON), CUBE_JSON, uri.data, at + strlen("{\"byteLength\":308}"));
    ASSERT_TRUE(w_pack_model((const u8*)big, strlen(big), nullptr, other));
    ASSERT_EQ(other.length, packed.length);
    EXPECT_EQ(memcmp(other.data, packed.data, packed.length), 0);
    EXPECT_FALSE(w_pack_model((const u8*)json, strlen(json), nullptr, other)) << "a file without a directory";
    EXPECT_FALSE(w_pack_model_file("/nonexistent/puppetxr/model.glb", other));
}

TEST_F(ModelTest, nodes_place_their_meshes)
{
    char      json[4096];
    Array<u8> file = {};
    defer { file.release(); };
    // The root turns a quarter about +Y and lifts by 1 m; its child scales by 2 and holds the mesh
    replaced(CUBE_JSON, "\"nodes\":[{\"mesh\":0}]", "\"nodes\":[{\"children\":[1],\"translation\":[0,1,0],\"rotation\":[0,0.70710678,0,0.70710678]},{\"mesh\":0,\"scale\":[2,1,1]}]", json);
    cube_glb(json, file);
    W_Model* model = load_model(file);
    ASSERT_NE(model, nullptr);
    Vec3 lo = {};
    Vec3 hi = {};
    w_model_bounds(model, lo, hi);
    EXPECT_NEAR(lo.x, -0.5f, 1e-5f);
    EXPECT_NEAR(hi.x, 0.5f, 1e-5f);
    EXPECT_NEAR(lo.y, 0.5f, 1e-5f);
    EXPECT_NEAR(hi.y, 1.5f, 1e-5f);
    EXPECT_NEAR(lo.z, -1.0f, 1e-5f);
    EXPECT_NEAR(hi.z, 1.0f, 1e-5f);
    w_destroy_model(model);

    // A node used twice places the mesh twice; a matrix works like TRS
    replaced(CUBE_JSON, "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}]", "\"scenes\":[{\"nodes\":[0,1]}],\"nodes\":[{\"mesh\":0},{\"children\":[0],\"matrix\":[1,0,0,0,0,1,0,0,0,0,1,0,3,0,0,1]}]", json);
    cube_glb(json, file);
    model = load_model(file);
    ASSERT_NE(model, nullptr);
    const W_VisualVertex* vertices       = nullptr;
    const u32*            indices        = nullptr;
    u32                   vertices_count = 0;
    u32                   indices_count  = 0;
    w_model_mesh(model, vertices, vertices_count, indices, indices_count);
    EXPECT_EQ(vertices_count, 16u);
    EXPECT_EQ(indices_count, 72u);
    w_model_bounds(model, lo, hi);
    EXPECT_NEAR(hi.x, 3.5f, 1e-5f);
    w_destroy_model(model);
}

TEST_F(ModelTest, strips_and_fans_become_triangles_and_points_are_skipped)
{
    char      json[4096];
    Array<u8> file = {};
    defer { file.release(); };
    // Six indices as a strip are four triangles, as a fan four too
    replaced(CUBE_JSON, "\"count\":36,\"type\":\"SCALAR\"}", "\"count\":6,\"type\":\"SCALAR\"}", json);
    char strip[4096];
    replaced(json, "\"material\":0}", "\"material\":0,\"mode\":5},{\"attributes\":{\"POSITION\":0},\"indices\":2,\"mode\":6},{\"attributes\":{\"POSITION\":0},\"mode\":0}", strip);
    cube_glb(strip, file);
    W_Model* model = load_model(file);
    ASSERT_NE(model, nullptr);
    const W_ModelPart* parts = nullptr;
    ASSERT_EQ(w_model_parts(model, parts), 2u);
    EXPECT_EQ(parts[0].indices_count, 12u);
    EXPECT_EQ(parts[1].indices_count, 12u);
    EXPECT_NE(parts[0].image, parts[1].image) << "the default material is white";
    w_destroy_model(model);
}

TEST_F(ModelTest, animations_are_ignored)
{
    char      json[4096];
    Array<u8> file = {};
    defer { file.release(); };
    replaced(CUBE_JSON, "\"buffers\"", "\"animations\":[{\"channels\":[],\"samplers\":[]}],\"buffers\"", json);
    cube_glb(json, file);
    W_Model* model = load_model(file);
    EXPECT_NE(model, nullptr);
    w_destroy_model(model);
}

TEST_F(ModelTest, refuses_what_it_cannot_show)
{
    char json[4096];
    expect_model_refused(replaced(CUBE_JSON, "\"nodes\":[{\"mesh\":0}]", "\"nodes\":[{\"mesh\":0,\"skin\":0}],\"skins\":[{\"joints\":[0]}]", json), "a skin");
    expect_model_refused(replaced(CUBE_JSON, "\"indices\":2,", "\"indices\":2,\"targets\":[{\"POSITION\":0}],", json), "morph targets");
    expect_model_refused(replaced(CUBE_JSON, "\"type\":\"VEC2\"}", "\"type\":\"VEC2\",\"sparse\":{\"count\":1}}", json), "a sparse accessor");
    expect_model_refused(replaced(CUBE_JSON, "\"asset\":{\"version\":\"2.0\"}", "\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"KHR_draco_mesh_compression\"]", json), "a required extension");
    expect_model_refused(replaced(CUBE_JSON, "\"version\":\"2.0\"", "\"version\":\"1.0\"", json), "glTF 1.0");
    expect_model_refused(replaced(CUBE_JSON, "\"meshes\"", "\"x\"", json), "no meshes");
    expect_model_refused(replaced(CUBE_JSON, "\"scenes\":[{\"nodes\":[0]}]", "\"scenes\":[{\"nodes\":[]}]", json), "an empty scene");
}

TEST_F(ModelTest, refuses_malformed_files)
{
    char json[4096];
    expect_model_refused(replaced(CUBE_JSON, "\"byteLength\":96}", "\"byteLength\":400}", json), "a buffer view past its buffer");
    expect_model_refused(replaced(CUBE_JSON, "\"count\":8,\"type\":\"VEC3\"", "\"count\":9,\"type\":\"VEC3\"", json), "an accessor past its view");
    expect_model_refused(replaced(CUBE_JSON, "\"count\":36", "\"count\":37", json), "indices past their view");
    expect_model_refused(replaced(CUBE_JSON, "\"count\":8,\"type\":\"VEC3\"", "\"count\":6,\"type\":\"VEC3\"", json), "indices past the vertices");
    expect_model_refused(replaced(CUBE_JSON, "\"componentType\":5126,\"count\":8,\"type\":\"VEC3\"", "\"componentType\":5121,\"count\":8,\"type\":\"VEC3\"", json), "byte positions");
    expect_model_refused(replaced(CUBE_JSON, "\"type\":\"VEC3\"", "\"type\":\"VEC4\"", json), "the wrong type");
    expect_model_refused(replaced(CUBE_JSON, "\"POSITION\":0", "\"POSITION\":7", json), "an accessor that does not exist");
    expect_model_refused(replaced(CUBE_JSON, "\"nodes\":[{\"mesh\":0}]", "\"nodes\":[{\"mesh\":0,\"children\":[0]}]", json), "a node that is its own child");
    expect_model_refused(replaced(CUBE_JSON, "\"nodes\":[{\"mesh\":0}]", "\"nodes\":[{\"mesh\":0,\"scale\":[1e30,1,1]}]", json), "a mesh far beyond any room");
    char textured[4096];
    replaced(CUBE_JSON, "\"material\":0", "\"material\":1", textured);
    expect_model_refused(replaced(textured, "\"byteLength\":75}", "\"byteLength\":20}", json), "an image that is not one");
    expect_model_refused(replaced(CUBE_JSON, "\"material\":0", "\"material\":5", json), "a material that does not exist");
    expect_model_refused(replaced(CUBE_JSON, "\"byteLength\":308}", "\"byteLength\":308,\"uri\":\"../../etc/passwd\"}", json), "a file without a directory");

    Array<u8> glb = {};
    defer { glb.release(); };
    EXPECT_FALSE(w_pack_model(nullptr, 0, nullptr, glb));
    EXPECT_FALSE(w_pack_model((const u8*)"glTF", 4, nullptr, glb));
    EXPECT_FALSE(w_pack_model((const u8*)"not a model", 11, nullptr, glb));
    EXPECT_EQ(w_create_model(nullptr, 0), nullptr);
}

TEST_F(ModelTest, survives_every_truncation_and_corruption)
{
    Array<u8> file = {};
    Array<u8> bad  = {};
    Array<u8> glb  = {};
    defer {
        file.release();
        bad.release();
        glb.release();
    };
    cube_glb(CUBE_JSON, file);
    for (usize size = 0; size < file.length; size++)
    {
        EXPECT_FALSE(w_pack_model(file.data, size, nullptr, glb)) << size;
        W_Model* model = w_create_model(file.data, size);
        EXPECT_EQ(model, nullptr) << size;
        w_destroy_model(model);
    }
    // Any byte may be anything: no crash, whatever the answer
    for (usize i = 0; i < file.length; i++)
    {
        bad.clear();
        for (usize j = 0; j < file.length; j++)
        {
            bad.add(file.data[j]);
        }
        bad.data[i] ^= 0xa5;
        if (w_pack_model(bad.data, bad.length, nullptr, glb))
        {
            w_destroy_model(w_create_model(glb.data, glb.length));
        }
    }
}

TEST_F(ModelTest, mesh_models_are_white)
{
    const Vec3 positions[3] = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    const u32  indices[3]   = { 0, 1, 2 };
    W_Model*   model        = w_create_mesh_model(positions, 3, indices, 3);
    ASSERT_NE(model, nullptr);
    const W_ModelPart* parts = nullptr;
    ASSERT_EQ(w_model_parts(model, parts), 1u);
    const u8* rgba   = nullptr;
    u32       width  = 0;
    u32       height = 0;
    w_model_image(model, parts[0].image, rgba, width, height);
    EXPECT_EQ(rgba[0], 255);
    w_destroy_model(model);
    const u32 bad[3] = { 0, 1, 3 };
    EXPECT_EQ(w_create_mesh_model(positions, 3, bad, 3), nullptr);
    EXPECT_EQ(w_create_mesh_model(positions, 3, indices, 2), nullptr);
}

TEST_F(ModelTest, instances_hide_the_room_in_depth_and_raycasts)
{
    TestRoom room = {};
    test_room_build(room);
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);
    Array<u8> file = {};
    defer {
        file.release();
        w_destroy(world);
    };
    cube_glb(CUBE_JSON, file);
    W_Model* model = load_model(file);
    ASSERT_NE(model, nullptr);
    defer { w_destroy_model(model); };

    // A box 0.4 m deep, its front 1 m ahead of the view, turned about its own axis
    const Pose       view     = { .position = { 0.0f, 1.2f, 0.0f }, .orientation = zm::QUAT_IDENTITY };
    const W_Instance instance = { .model = model, .pose = { { 0.0f, 1.2f, -1.2f }, zm::quat_from_axis_angle({ 0.0f, 0.0f, 1.0f }, 0.3f) }, .scale = { 0.5f, 0.5f, 0.4f } };
    const Fov        fov      = { -0.05f, 0.05f, 0.05f, -0.05f };
    u16              behind   = 0;
    u16              front    = 0;
    w_trace_depth(world, nullptr, 0, view, fov, DEPTH_NEAR, 1, 1, 0, 1, &behind);
    w_trace_depth(world, &instance, 1, view, fov, DEPTH_NEAR, 1, 1, 0, 1, &front);
    EXPECT_NEAR(front, depth_value(1.0f), 2);
    EXPECT_NEAR(behind, depth_value(TEST_ROOM_HALF_Z), 2);

    // Beyond the wall it hides nothing
    W_Instance far = instance;
    far.pose.position.z = -TEST_ROOM_HALF_Z - 1.0f;
    w_trace_depth(world, &far, 1, view, fov, DEPTH_NEAR, 1, 1, 0, 1, &front);
    EXPECT_EQ(front, behind);

    W_RaycastHit hit   = {};
    u32          index = 99;
    ASSERT_TRUE(w_raycast_instances(&instance, 1, view.position, { 0.0f, 0.0f, -2.0f }, 10.0f, hit, index));
    EXPECT_EQ(index, 0u);
    EXPECT_NEAR(hit.distance, 1.0f, 1e-5f);
    EXPECT_NEAR(hit.normal.z, 1.0f, 1e-5f);
    EXPECT_FALSE(w_raycast_instances(&instance, 1, view.position, { 0.0f, 0.0f, -1.0f }, 0.9f, hit, index));
    EXPECT_FALSE(w_raycast_instances(&instance, 1, view.position, { 0.0f, 0.0f, 1.0f }, 10.0f, hit, index));
    EXPECT_FALSE(w_raycast_instances(nullptr, 0, view.position, { 0.0f, 0.0f, -1.0f }, 10.0f, hit, index));
    W_Instance flat = instance;
    flat.scale      = { 0.5f, 0.0f, 0.4f };
    EXPECT_FALSE(w_raycast_instances(&flat, 1, view.position, { 0.0f, 0.0f, -1.0f }, 10.0f, hit, index)) << "a degenerate scale hides nothing";
}

// Environment raycasts trace the texels they read one at a time, props included
TEST_F(ModelTest, a_traced_texel_with_instances_is_the_one_in_the_traced_image)
{
    TestRoom room = {};
    test_room_build(room);
    W_World* world = parse(room);
    ASSERT_NE(world, nullptr);
    Array<u8> file = {};
    defer {
        file.release();
        w_destroy(world);
    };
    cube_glb(CUBE_JSON, file);
    W_Model* model = load_model(file);
    ASSERT_NE(model, nullptr);
    defer { w_destroy_model(model); };
    const W_Instance instances[2] = {
        { model, { { 0.0f, 1.2f, -1.0f }, zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.7f) }, { 0.3f, 0.5f, 0.2f } },
        { model, { { -0.4f, 1.5f, -0.8f }, zm::QUAT_IDENTITY }, { 0.1f, 0.1f, 0.1f } },
    };
    const Pose view = { { 0.05f, 1.3f, 0.2f }, zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -0.15f) };
    const Fov  fov  = { -0.6f, 0.5f, 0.45f, -0.55f };
    const u32  size = 32;
    static u16 image[size * size];
    w_trace_depth(world, instances, 2, view, fov, DEPTH_NEAR, size, size, 0, size, image);
    u32 nearer = 0;
    for (u32 y = 0; y < size; y++)
    {
        for (u32 x = 0; x < size; x++)
        {
            ASSERT_EQ(w_trace_depth_texel(world, instances, 2, view, fov, DEPTH_NEAR, size, size, x, y), image[y * size + x]) << x << " " << y;
            nearer += image[y * size + x] < w_trace_depth_texel(world, nullptr, 0, view, fov, DEPTH_NEAR, size, size, x, y) ? 1u : 0u;
        }
    }
    EXPECT_GT(nearer, 20u) << "the boxes hide the room in part of the image";
}

// The bounding spheres that let most rays skip an instance never drop a hit: every pixel of the
// image is what a raycast of its ray finds
TEST_F(ModelTest, depth_of_instances_matches_their_raycasts_pixel_for_pixel)
{
    Array<u8> file = {};
    defer { file.release(); };
    cube_glb(CUBE_JSON, file);
    W_Model* model = load_model(file);
    ASSERT_NE(model, nullptr);
    defer { w_destroy_model(model); };
    const W_Instance instances[3] = {
        { model, { { 0.0f, 1.2f, -1.0f }, zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.7f) }, { 0.3f, 0.5f, 0.2f } },
        { model, { { 0.35f, 1.0f, -1.4f }, zm::normalize(zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, 0.4f) * zm::quat_from_axis_angle({ 0.0f, 0.0f, 1.0f }, 0.9f)) }, { 0.6f, 0.05f, 0.3f } },
        { model, { { -0.4f, 1.5f, -0.8f }, zm::QUAT_IDENTITY }, { 0.1f, 0.1f, 0.1f } },
    };
    const Pose view = { { 0.05f, 1.3f, 0.2f }, zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -0.15f) };
    const Fov  fov  = { -0.6f, 0.5f, 0.45f, -0.55f };
    const u32  size = 48;
    static u16 image[size * size];
    w_trace_depth(nullptr, instances, 3, view, fov, DEPTH_NEAR, size, size, 0, size, image);
    const Vec3 forward = zm::rotate(view.orientation, { 0.0f, 0.0f, -1.0f });
    u32        hits    = 0;
    for (u32 y = 0; y < size; y++)
    {
        for (u32 x = 0; x < size; x++)
        {
            const f32    l         = zm::tan(fov.angle_left);
            const f32    r         = zm::tan(fov.angle_right);
            const f32    u         = zm::tan(fov.angle_up);
            const f32    d         = zm::tan(fov.angle_down);
            const Vec3   direction = zm::rotate(view.orientation, { l + (r - l) * ((f32)x + 0.5f) / (f32)size, d + (u - d) * ((f32)y + 0.5f) / (f32)size, -1.0f });
            W_RaycastHit hit       = {};
            u32          index     = 0;
            if (!w_raycast_instances(instances, 3, view.position, direction, 100.0f, hit, index))
            {
                EXPECT_EQ(image[y * size + x], 0xffff) << x << ", " << y;
                continue;
            }
            hits++;
            const f32 z = zm::dot(hit.point - view.position, forward);
            EXPECT_NEAR(image[y * size + x], depth_value(z), 2) << x << ", " << y;
        }
    }
    EXPECT_GT(hits, size * size / 10) << "the instances fill part of the view";
}

// A white RGBA PNG of width x height, its deflate stream uncompressed (stored blocks)
static void make_png (u32 width, u32 height, Array<u8>& out)
{
    Array<u8> raw = {};
    Array<u8> z   = {};
    defer {
        raw.release();
        z.release();
    };
    for (u32 y = 0; y < height; y++)
    {
        raw.add(0);
        for (u32 x = 0; x < width * 4; x++)
        {
            raw.add(255);
        }
    }
    z.add(0x78);
    z.add(0x01);
    u32 a = 1;
    u32 b = 0;
    for (usize at = 0; at < raw.length; at += 65535)
    {
        const u32 n = (u32)(raw.length - at < 65535 ? raw.length - at : 65535);
        z.add(at + n == raw.length ? 1 : 0);
        z.add((u8)n);
        z.add((u8)(n >> 8));
        z.add((u8)~n);
        z.add((u8)(~n >> 8));
        z.add(raw.data + at, n);
        for (u32 i = 0; i < n; i++)
        {
            a = (a + raw.data[at + i]) % 65521;
            b = (b + a) % 65521;
        }
    }
    const u32 adler = b << 16 | a;
    for (u32 i = 0; i < 4; i++)
    {
        z.add((u8)(adler >> (24 - 8 * i)));
    }
    const auto chunk = [&out] (const char* type, const u8* data, usize size) {
        for (u32 i = 0; i < 4; i++)
        {
            out.add((u8)(size >> (24 - 8 * i)));
        }
        const usize start = out.length;
        out.add((const u8*)type, 4);
        out.add(data, size);
        const u32 crc = crc32(out.data + start, size + 4);
        for (u32 i = 0; i < 4; i++)
        {
            out.add((u8)(crc >> (24 - 8 * i)));
        }
    };
    const u8 signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    const u8 header[13]   = { (u8)(width >> 24), (u8)(width >> 16), (u8)(width >> 8), (u8)width, (u8)(height >> 24), (u8)(height >> 16), (u8)(height >> 8), (u8)height, 8, 6, 0, 0, 0 };
    out.clear();
    out.add(signature, 8);
    chunk("IHDR", header, sizeof(header));
    chunk("IDAT", z.data, z.length);
    chunk("IEND", nullptr, 0);
}

// One triangle per material, every material showing the same texture
static void textured_triangles (u32 materials, const Array<u8>& png, Array<u8>& out)
{
    Array<u8>   bin  = {};
    Array<char> json = {};
    defer {
        bin.release();
        json.release();
    };
    const f32 positions[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    bin.add((const u8*)positions, sizeof(positions));
    bin.add(png.data, png.length);
    char text[256];
    const auto append = [&json] (const char* t) { json.add(t, strlen(t)); };
    append("{\"asset\":{\"version\":\"2.0\"},\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[");
    for (u32 m = 0; m < materials; m++)
    {
        snprintf(text, sizeof(text), "%s{\"attributes\":{\"POSITION\":0},\"material\":%u}", m ? "," : "", m);
        append(text);
    }
    append("]}],\"materials\":[");
    for (u32 m = 0; m < materials; m++)
    {
        append(m ? ",{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}" : "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}");
    }
    snprintf(text, sizeof(text), "],\"textures\":[{\"source\":0}],\"images\":[{\"bufferView\":1,\"mimeType\":\"image/png\"}],"
             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],");
    append(text);
    snprintf(text, sizeof(text), "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":%u}],\"buffers\":[{\"byteLength\":%u}]}", (u32)png.length, (u32)bin.length);
    append(text);
    json.add(0);
    make_glb(json.data, bin.data, bin.length, out);
}

// Regression: a texture of 1024 x 1024 used by 65 materials decoded to 65 RGBA copies, and one of
// 4096 x 4096 used by 256 to 16 GiB; what all materials decode to is capped
TEST_F(ModelTest, refuses_textures_that_decode_to_too_much)
{
    Array<u8> png  = {};
    Array<u8> file = {};
    Array<u8> glb  = {};
    defer {
        png.release();
        file.release();
        glb.release();
    };
    make_png(1024, 1024, png);
    textured_triangles(2, png, file);
    ASSERT_TRUE(w_pack_model(file.data, file.length, nullptr, glb)) << "two copies are fine";
    W_Model* model = w_create_model(glb.data, glb.length);
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(w_model_image_count(model), 2u);
    w_destroy_model(model);
    textured_triangles(W_MODEL_MAX_TEXELS / (1024 * 1024) + 1, png, file);
    EXPECT_FALSE(w_pack_model(file.data, file.length, nullptr, glb));
    model = w_create_model(file.data, file.length);
    EXPECT_EQ(model, nullptr);
    w_destroy_model(model);
}

TEST(World, compiled_in_models_are_the_source_models_and_load)
{
    ASSERT_GE(w_embedded_models_count, 3u);
    for (u32 i = 0; i < w_embedded_models_count; i++)
    {
        const W_EmbeddedModel& embedded = w_embedded_models[i];
        EXPECT_TRUE(w_room_name_valid(embedded.name)) << embedded.name;
        EXPECT_TRUE(i == 0 || strcmp(w_embedded_models[i - 1].name, embedded.name) < 0) << "sorted";
        char path[OS_MAX_PATH];
        cstr_format(path, sizeof(path), "%s/%s/%s.glb", PUPPETXR_SOURCE_MODELS_DIR, embedded.name, embedded.name);
        u8*   data = nullptr;
        usize size = 0;
        ASSERT_TRUE(os_read_file(path, &data, &size)) << path;
        EXPECT_EQ(size, embedded.size) << embedded.name;
        EXPECT_TRUE(size == embedded.size && memcmp(data, embedded.data, size) == 0) << embedded.name;
        os_memory_release(data);

        Array<u8> glb = {};
        defer { glb.release(); };
        ASSERT_TRUE(w_pack_model(embedded.data, embedded.size, nullptr, glb)) << embedded.name;
        W_Model* model = w_create_model(glb.data, glb.length);
        ASSERT_NE(model, nullptr) << embedded.name;
        Vec3 lo = {};
        Vec3 hi = {};
        w_model_bounds(model, lo, hi);
        EXPECT_NEAR(lo.y, 0.0f, 1e-3f) << embedded.name << " stands on its origin";
        EXPECT_LT(zm::abs(lo.x + hi.x), 0.01f) << embedded.name << " is centred";
        EXPECT_LT(zm::abs(lo.z + hi.z), 0.01f) << embedded.name << " is centred";
        w_destroy_model(model);
        const u8* found      = nullptr;
        usize     found_size = 0;
        EXPECT_TRUE(w_find_embedded_model(embedded.name, found, found_size));
        EXPECT_EQ(found, embedded.data);
    }
    const u8* found      = nullptr;
    usize     found_size = 0;
    EXPECT_FALSE(w_find_embedded_model("no_such_model", found, found_size));
    EXPECT_FALSE(w_find_embedded_model(nullptr, found, found_size));
}
