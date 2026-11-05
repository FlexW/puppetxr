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
