#include "world.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_ASSERT(x) ASSERT(x)
#define STBI_MALLOC(size) os_memory_alloc(size)
#define STBI_REALLOC(pointer, size) os_memory_realloc(pointer, size)
#define STBI_FREE(pointer) os_memory_release(pointer)
#include <stb_image.h>

//
// Types
//

#define W_MAX_COORDINATE  1000.0f // metres from the room origin; anything beyond is a corrupt file
#define W_MAX_IMAGE_SIZE  16384
#define W_BVH_LEAF_SIZE   4
#define W_BVH_STACK       64
#define W_MAX_ELEMENTS    (1u << 26)
#define W_NONE            0xffffffffu
#define W_ROOM_MESH_PLANAR 1e-3f // metres a room mesh face may stray from its plane

namespace
{

struct W_BvhNode
{
    Vec3 min;
    Vec3 max;
    u32  first; // leaf: first triangle in W_World::bvh_triangles; inner: index of the left child, the right one follows it
    u32  count; // triangles of a leaf, 0 for an inner node
};

} // namespace

struct W_World
{
    char                  name[W_MAX_NAME];
    u64                   content_hash;
    Array<W_Entity>       entities;
    Array<Vec2>           boundary_points;
    Array<W_Room>         rooms;
    Array<u32>            room_indices;
    Array<Vec3>           mesh_positions;
    Array<u32>            mesh_indices;
    Array<Vec3>           room_mesh_positions;
    Array<W_RoomMeshFace> room_mesh_faces;
    Array<u32>            room_mesh_indices;
    Array<W_VisualVertex> visual_vertices;
    Array<u32>            visual_indices;
    u8*                   albedo; // stbi allocation
    u32                   albedo_width;
    u32                   albedo_height;
    Array<W_BvhNode>      bvh_nodes;
    Array<u32>            bvh_triangles;
};

namespace
{

struct W_BufferView
{
    u32 offset;
    u32 count;
};

struct W_SortItem
{
    f32 key;
    u32 triangle;
};

struct W_ListContext
{
    const char* rooms_dir;
    char        (*names)[W_MAX_NAME];
    u32         capacity;
    u32         count;
};

// A QR code under construction: function modules are excluded from data and masking
struct W_QrBuilder
{
    u32 size;
    u8  modules[W_QR_MAX_SIZE][W_QR_MAX_SIZE];
    b8  function[W_QR_MAX_SIZE][W_QR_MAX_SIZE];
};

} // namespace

//
// Statics
//

static const char* const w_label_names[(u32)W_Label::Count] = {
    "other",
    "floor",
    "ceiling",
    "wall_face",
    "invisible_wall_face",
    "inner_wall_face",
    "door_frame",
    "window_frame",
    "opening",
    "wall_art",
    "table",
    "couch",
    "bed",
    "storage",
    "screen",
    "lamp",
    "plant",
    "global_mesh",
};

static u64 hash_combine (u64 seed, const u8* data, usize size)
{
    u64 h = seed;
    for (usize i = 0; i < size; i++)
    {
        h = (h ^ data[i]) * 0x100000001B3ull;
    }
    return (h ^ size) * 0x100000001B3ull;
}

static b8 coordinate_valid (f32 value)
{
    return zm::is_finite(value) && zm::abs(value) <= W_MAX_COORDINATE;
}

static b8 read_floats (const JsonValue& value, f32* out_values, u32 count)
{
    if (!json_is_array(value) || json_length(value) != count)
    {
        return false;
    }
    for (u32 i = 0; i < count; i++)
    {
        const JsonValue element = json_at(value, i);
        if (!json_is_number(element) || !json_as_float(element, out_values[i]) || !coordinate_valid(out_values[i]))
        {
            return false;
        }
    }
    return true;
}

static b8 read_vec2 (const JsonValue& value, Vec2& out_value)
{
    f32 v[2];
    if (!read_floats(value, v, 2))
    {
        return false;
    }
    out_value = { v[0], v[1] };
    return true;
}

static b8 read_vec3 (const JsonValue& value, Vec3& out_value)
{
    f32 v[3];
    if (!read_floats(value, v, 3))
    {
        return false;
    }
    out_value = { v[0], v[1], v[2] };
    return true;
}

static b8 read_index (const JsonValue& value, u32 limit, u32& out_index)
{
    return json_is_number(value) && json_as_uint(value, out_index) && out_index < limit;
}

static b8 read_view (const JsonValue& object, const char* key, u32 stride, usize binary_size, W_BufferView& out_view)
{
    const JsonValue view = json_find(object, key);
    if (!json_is_object(view) || !json_find_uint(view, "offset", out_view.offset) || !json_find_uint(view, "count", out_view.count))
    {
        LOG_ERROR("Room buffer %s is missing or malformed", key);
        return false;
    }
    if (out_view.offset % 4 != 0 || out_view.count > W_MAX_ELEMENTS || (u64)out_view.offset + (u64)out_view.count * stride > binary_size)
    {
        LOG_ERROR("Room buffer %s (offset %u, %u elements) is misaligned or past the end of the %zu byte binary", key, out_view.offset, out_view.count, (size_t)binary_size);
        return false;
    }
    return true;
}

static void copy_view (void* dest, const u8* binary, const W_BufferView& view, usize stride)
{
    if (binary && view.count > 0)
    {
        memory_copy(dest, binary + view.offset, view.count * stride);
    }
}

static b8 floats_valid (const f32* values, usize count)
{
    for (usize i = 0; i < count; i++)
    {
        if (!coordinate_valid(values[i]))
        {
            return false;
        }
    }
    return true;
}

static b8 indices_valid (const u32* indices, u32 count, u32 vertices_count)
{
    if (count % 3 != 0)
    {
        return false;
    }
    for (u32 i = 0; i < count; i++)
    {
        if (indices[i] >= vertices_count)
        {
            return false;
        }
    }
    return true;
}

static b8 read_meshes (W_World* world, const JsonValue& root, const W_RoomSource& source)
{
    const JsonValue visual = json_find(root, "visual");
    const JsonValue mesh   = json_find(root, "global_mesh");
    if (!json_is_object(visual) || !json_is_object(mesh))
    {
        LOG_ERROR("Room has no visual or global_mesh section");
        return false;
    }

    W_BufferView vertices  = {};
    W_BufferView vindices  = {};
    W_BufferView positions = {};
    W_BufferView mindices  = {};
    if (!read_view(visual, "vertices", sizeof(W_VisualVertex), source.binary_size, vertices) || !read_view(visual, "indices", sizeof(u32), source.binary_size, vindices) ||
        !read_view(mesh, "positions", sizeof(Vec3), source.binary_size, positions) || !read_view(mesh, "indices", sizeof(u32), source.binary_size, mindices))
    {
        return false;
    }

    world->visual_vertices.resize(vertices.count);
    world->visual_indices.resize(vindices.count);
    world->mesh_positions.resize(positions.count);
    world->mesh_indices.resize(mindices.count);
    copy_view(world->visual_vertices.data, source.binary, vertices, sizeof(W_VisualVertex));
    copy_view(world->visual_indices.data, source.binary, vindices, sizeof(u32));
    copy_view(world->mesh_positions.data, source.binary, positions, sizeof(Vec3));
    copy_view(world->mesh_indices.data, source.binary, mindices, sizeof(u32));

    // UVs may leave [0, 1] for repeating textures but must still be finite
    if (!floats_valid((const f32*)world->visual_vertices.data, world->visual_vertices.length * 5) || !floats_valid((const f32*)world->mesh_positions.data, world->mesh_positions.length * 3))
    {
        LOG_ERROR("Room mesh has non-finite or out of range coordinates");
        return false;
    }
    if (!indices_valid(world->visual_indices.data, vindices.count, vertices.count) || !indices_valid(world->mesh_indices.data, mindices.count, positions.count))
    {
        LOG_ERROR("Room mesh indices are out of range or not whole triangles");
        return false;
    }
    return true;
}

static b8 read_plane (W_World* world, const JsonValue& plane, W_Entity& entity)
{
    if (!read_vec2(json_find(plane, "min"), entity.plane_min) || !read_vec2(json_find(plane, "max"), entity.plane_max) ||
        entity.plane_min.x > entity.plane_max.x || entity.plane_min.y > entity.plane_max.y)
    {
        return false;
    }
    const JsonValue boundary = json_find(plane, "boundary");
    if (!json_is_valid(boundary))
    {
        return true;
    }
    const u32 count = json_length(boundary);
    if (!json_is_array(boundary) || count < 3 || count > W_MAX_ELEMENTS)
    {
        return false;
    }
    entity.boundary_first = (u32)world->boundary_points.length;
    entity.boundary_count = count;
    for (u32 i = 0; i < count; i++)
    {
        if (!read_vec2(json_at(boundary, i), world->boundary_points.add()))
        {
            return false;
        }
    }
    return true;
}

// Entities are reserved up front, so the reference stays valid while the boundary array grows
static b8 read_entity (W_World* world, const JsonValue& value, u32 index)
{
    W_Entity& entity = world->entities.add();
    entity.room      = W_NONE;

    char text[64];
    if (!json_is_object(value) || !json_find_string(value, "uuid", text, nullptr, sizeof(text)) || !w_uuid_from_string(text, entity.uuid) || w_uuid_is_zero(entity.uuid))
    {
        LOG_ERROR("Room entity %u has no valid uuid", index);
        return false;
    }
    if (!json_find_string(value, "label", text, nullptr, sizeof(text)) || !w_label_from_name(text, entity.label))
    {
        LOG_ERROR("Room entity %u has an unknown label", index);
        return false;
    }

    f32 q[4];
    if (!read_vec3(json_find(value, "position"), entity.pose.position) || !read_floats(json_find(value, "orientation"), q, 4))
    {
        LOG_ERROR("Room entity %u has no valid pose", index);
        return false;
    }
    entity.pose.orientation = { q[0], q[1], q[2], q[3] };
    const f32 length        = zm::length(entity.pose.orientation);
    if (!(length > 0.5f && length < 2.5f))
    {
        LOG_ERROR("Room entity %u orientation is not a rotation", index);
        return false;
    }
    entity.pose.orientation = zm::normalize(entity.pose.orientation);

    const JsonValue plane = json_find(value, "plane");
    if (json_is_valid(plane))
    {
        if (!json_is_object(plane) || !read_plane(world, plane, entity))
        {
            LOG_ERROR("Room entity %u has a malformed plane", index);
            return false;
        }
        entity.components |= (u32)W_ComponentFlags::Plane;
    }

    const JsonValue volume = json_find(value, "volume");
    if (json_is_valid(volume))
    {
        if (!json_is_object(volume) || !read_vec3(json_find(volume, "min"), entity.volume_min) || !read_vec3(json_find(volume, "max"), entity.volume_max) ||
            entity.volume_min.x > entity.volume_max.x || entity.volume_min.y > entity.volume_max.y || entity.volume_min.z > entity.volume_max.z)
        {
            LOG_ERROR("Room entity %u has a malformed volume", index);
            return false;
        }
        entity.components |= (u32)W_ComponentFlags::Volume;
    }

    b8 mesh = false;
    if (json_find_bool(value, "mesh", mesh) && mesh)
    {
        entity.components |= (u32)W_ComponentFlags::Mesh;
    }
    if (json_is_valid(json_find(value, "room")))
    {
        entity.components |= (u32)W_ComponentFlags::Room;
    }
    if (json_is_valid(json_find(value, "room_mesh")))
    {
        entity.components |= (u32)W_ComponentFlags::RoomMesh;
    }
    return true;
}

static b8 read_room (W_World* world, const JsonValue& value, u32 entity_index)
{
    const u32 entities_count = (u32)world->entities.length;
    const u32 room_index     = (u32)world->rooms.length;
    W_Room&   room           = world->rooms.add();
    room.entity              = entity_index;

    const JsonValue walls    = json_find(value, "walls");
    const JsonValue children = json_find(value, "children");
    if (!json_is_object(value) || !read_index(json_find(value, "floor"), entities_count, room.floor) || !read_index(json_find(value, "ceiling"), entities_count, room.ceiling) ||
        !json_is_array(walls) || !json_is_array(children))
    {
        return false;
    }

    const u32 plane = (u32)W_ComponentFlags::Plane;
    if (!(world->entities[room.floor].components & plane) || !(world->entities[room.ceiling].components & plane))
    {
        return false;
    }

    room.walls_first = (u32)world->room_indices.length;
    room.walls_count = json_length(walls);
    for (u32 i = 0; i < room.walls_count; i++)
    {
        u32 wall = 0;
        if (!read_index(json_at(walls, i), entities_count, wall) || !(world->entities[wall].components & plane))
        {
            return false;
        }
        world->room_indices.add(wall);
    }

    W_Room& r          = world->rooms[room_index];
    r.children_first   = (u32)world->room_indices.length;
    r.children_count   = json_length(children);
    for (u32 i = 0; i < r.children_count; i++)
    {
        u32 child = 0;
        if (!read_index(json_at(children, i), entities_count, child) || child == entity_index || world->entities[child].room != W_NONE ||
            (world->entities[child].components & (u32)W_ComponentFlags::Room))
        {
            return false;
        }
        world->entities[child].room = room_index;
        world->room_indices.add(child);
    }
    return true;
}

static b8 room_mesh_label (W_Label label)
{
    switch (label)
    {
    case W_Label::Floor:
    case W_Label::Ceiling:
    case W_Label::WallFace:
    case W_Label::InvisibleWallFace:
    case W_Label::InnerWallFace:
    case W_Label::DoorFrame:
    case W_Label::WindowFrame:
        return true;
    default:
        return false;
    }
}

// Spec XR_META_spatial_entity_room_mesh: each face is planar
static b8 face_planar (const Vec3* positions, const u32* indices, u32 count)
{
    Vec3 normal = {};
    Vec3 origin = {};
    f32  best   = 0.0f;
    for (u32 i = 0; i < count; i += 3)
    {
        const Vec3 a = positions[indices[i]];
        const Vec3 n = zm::cross(positions[indices[i + 1]] - a, positions[indices[i + 2]] - a);
        const f32  l = zm::length(n);
        if (l > best)
        {
            best   = l;
            normal = n * (1.0f / l);
            origin = a;
        }
    }
    if (best <= 0.0f)
    {
        return false;
    }
    for (u32 i = 0; i < count; i++)
    {
        const f32 distance = zm::dot(positions[indices[i]] - origin, normal);
        if (distance > W_ROOM_MESH_PLANAR || distance < -W_ROOM_MESH_PLANAR)
        {
            return false;
        }
    }
    return true;
}

static b8 read_room_mesh (W_World* world, const JsonValue& value, const W_RoomSource& source, W_Room& room)
{
    const JsonValue faces = json_find(value, "faces");
    W_BufferView    view  = {};
    if (!json_is_object(value) || !read_view(value, "positions", sizeof(Vec3), source.binary_size, view) || view.count < 4 || !json_is_array(faces) ||
        json_length(faces) == 0 || json_length(faces) > W_MAX_ELEMENTS)
    {
        return false;
    }
    room.mesh_positions_first = (u32)world->room_mesh_positions.length;
    room.mesh_positions_count = view.count;
    world->room_mesh_positions.resize(world->room_mesh_positions.length + view.count);
    const Vec3* positions = world->room_mesh_positions.data + room.mesh_positions_first;
    copy_view(world->room_mesh_positions.data + room.mesh_positions_first, source.binary, view, sizeof(Vec3));
    if (!floats_valid((const f32*)positions, (usize)view.count * 3))
    {
        return false;
    }

    room.mesh_faces_first = (u32)world->room_mesh_faces.length;
    room.mesh_faces_count = json_length(faces);
    for (u32 i = 0; i < room.mesh_faces_count; i++)
    {
        const JsonValue face_value = json_at(faces, i);
        W_RoomMeshFace  face       = {};
        char            text[64];
        if (!json_is_object(face_value) || !json_find_string(face_value, "uuid", text, nullptr, sizeof(text)) || !w_uuid_from_string(text, face.uuid) || w_uuid_is_zero(face.uuid))
        {
            return false;
        }
        if (json_find_string(face_value, "parent", text, nullptr, sizeof(text)) && (!w_uuid_from_string(text, face.parent) || w_uuid_is_zero(face.parent)))
        {
            return false;
        }
        if (!json_find_string(face_value, "label", text, nullptr, sizeof(text)) || !w_label_from_name(text, face.label) || !room_mesh_label(face.label))
        {
            return false;
        }
        W_BufferView indices = {};
        if (!read_view(face_value, "indices", sizeof(u32), source.binary_size, indices) || indices.count == 0)
        {
            return false;
        }
        face.indices_first = (u32)world->room_mesh_indices.length;
        face.indices_count = indices.count;
        world->room_mesh_indices.resize(world->room_mesh_indices.length + indices.count);
        copy_view(world->room_mesh_indices.data + face.indices_first, source.binary, indices, sizeof(u32));
        const u32* face_indices = world->room_mesh_indices.data + face.indices_first;
        if (!indices_valid(face_indices, indices.count, view.count) || !face_planar(positions, face_indices, indices.count))
        {
            return false;
        }
        world->room_mesh_faces.add(face);
    }

    // Uuids are unique within the mesh, and a parent is another face of it
    const W_RoomMeshFace* mesh = world->room_mesh_faces.data + room.mesh_faces_first;
    for (u32 i = 0; i < room.mesh_faces_count; i++)
    {
        b8 parent_found = w_uuid_is_zero(mesh[i].parent);
        for (u32 j = 0; j < room.mesh_faces_count; j++)
        {
            if (j != i && w_uuid_equal(mesh[i].uuid, mesh[j].uuid))
            {
                return false;
            }
            parent_found = parent_found || (j != i && w_uuid_equal(mesh[i].parent, mesh[j].uuid));
        }
        if (!parent_found)
        {
            return false;
        }
    }
    return true;
}

static b8 read_layout (W_World* world, const JsonValue& root, const W_RoomSource& source)
{
    const JsonValue entities = json_find(root, "entities");
    if (!json_is_array(entities) || json_length(entities) > W_MAX_ELEMENTS)
    {
        LOG_ERROR("Room has no entities array");
        return false;
    }

    const u32 count = json_length(entities);
    world->entities.reserve(count);
    for (u32 i = 0; i < count; i++)
    {
        if (!read_entity(world, json_at(entities, i), i))
        {
            return false;
        }
        for (u32 j = 0; j < i; j++)
        {
            if (w_uuid_equal(world->entities[j].uuid, world->entities[i].uuid))
            {
                LOG_ERROR("Room entities %u and %u share a uuid", j, i);
                return false;
            }
        }
    }

    for (u32 i = 0; i < count; i++)
    {
        W_Entity& entity = world->entities[i];
        if (!(entity.components & (u32)W_ComponentFlags::Room))
        {
            continue;
        }
        entity.room = (u32)world->rooms.length;
        if (!read_room(world, json_find(json_at(entities, i), "room"), i))
        {
            LOG_ERROR("Room entity %u has an inconsistent layout", i);
            return false;
        }
        if ((entity.components & (u32)W_ComponentFlags::RoomMesh) && !read_room_mesh(world, json_find(json_at(entities, i), "room_mesh"), source, world->rooms[world->rooms.length - 1]))
        {
            LOG_ERROR("Room entity %u has a malformed room mesh", i);
            return false;
        }
    }

    // Only a room entity carries a room mesh
    for (u32 i = 0; i < count; i++)
    {
        const u32 components = world->entities[i].components;
        if ((components & (u32)W_ComponentFlags::RoomMesh) && !(components & (u32)W_ComponentFlags::Room))
        {
            LOG_ERROR("Room entity %u has a room mesh but is no room", i);
            return false;
        }
    }

    for (u32 i = 0; i < count; i++)
    {
        if (world->entities[i].room == W_NONE)
        {
            LOG_ERROR("Room entity %u belongs to no room", i);
            return false;
        }
    }
    return true;
}

static b8 read_image (W_World* world, const W_RoomSource& source)
{
    if (!source.image)
    {
        return true;
    }
    if (source.image_size == 0 || source.image_size > 0x7fffffff)
    {
        return false;
    }

    int width    = 0;
    int height   = 0;
    int channels = 0;
    if (!stbi_info_from_memory(source.image, (int)source.image_size, &width, &height, &channels) || width <= 0 || height <= 0 || width > W_MAX_IMAGE_SIZE || height > W_MAX_IMAGE_SIZE)
    {
        LOG_ERROR("Room albedo image is not a PNG or JPEG of at most %u pixels per side", W_MAX_IMAGE_SIZE);
        return false;
    }
    world->albedo = stbi_load_from_memory(source.image, (int)source.image_size, &width, &height, &channels, 4);
    if (!world->albedo)
    {
        LOG_ERROR("Room albedo image cannot be decoded: %s", stbi_failure_reason());
        return false;
    }
    world->albedo_width  = (u32)width;
    world->albedo_height = (u32)height;
    return true;
}

//
// BVH
//

static void triangle_bounds (const W_World* world, u32 triangle, Vec3& out_min, Vec3& out_max)
{
    const u32* t = world->mesh_indices.data + (usize)triangle * 3;
    const Vec3 a = world->mesh_positions[t[0]];
    const Vec3 b = world->mesh_positions[t[1]];
    const Vec3 c = world->mesh_positions[t[2]];
    out_min      = zm::min(a, zm::min(b, c));
    out_max      = zm::max(a, zm::max(b, c));
}

static i32 compare_sort_items (const void* a, const void* b)
{
    const W_SortItem* x = (const W_SortItem*)a;
    const W_SortItem* y = (const W_SortItem*)b;
    if (x->key != y->key)
    {
        return x->key < y->key ? -1 : 1;
    }
    return x->triangle < y->triangle ? -1 : (x->triangle > y->triangle ? 1 : 0);
}

static f32 triangle_centroid (const W_World* world, u32 triangle, u32 axis)
{
    Vec3 lo;
    Vec3 hi;
    triangle_bounds(world, triangle, lo, hi);
    const Vec3 c = (lo + hi) * 0.5f;
    return axis == 0 ? c.x : (axis == 1 ? c.y : c.z);
}

// Median split on the longest axis of the centroid bounds; deterministic for a given mesh
static void build_bvh (W_World* world)
{
    const u32 triangles = (u32)(world->mesh_indices.length / 3);
    world->bvh_triangles.resize(triangles);
    for (u32 i = 0; i < triangles; i++)
    {
        world->bvh_triangles[i] = i;
    }
    if (triangles == 0)
    {
        return;
    }

    Array<W_SortItem> scratch = {};
    defer { scratch.release(); };
    scratch.resize(triangles);
    world->bvh_nodes.reserve((usize)triangles * 2);
    world->bvh_nodes.add() = { .first = 0, .count = triangles };

    u32 stack[W_BVH_STACK];
    u32 stack_count     = 0;
    stack[stack_count++] = 0;
    while (stack_count > 0)
    {
        const u32  node_index = stack[--stack_count];
        W_BvhNode& node       = world->bvh_nodes[node_index];

        Vec3 lo = { zm::F32_MAX, zm::F32_MAX, zm::F32_MAX };
        Vec3 hi = -lo;
        Vec3 clo = lo;
        Vec3 chi = hi;
        for (u32 i = 0; i < node.count; i++)
        {
            Vec3 tlo;
            Vec3 thi;
            triangle_bounds(world, world->bvh_triangles[node.first + i], tlo, thi);
            lo  = zm::min(lo, tlo);
            hi  = zm::max(hi, thi);
            clo = zm::min(clo, (tlo + thi) * 0.5f);
            chi = zm::max(chi, (tlo + thi) * 0.5f);
        }
        node.min = lo;
        node.max = hi;

        if (node.count <= W_BVH_LEAF_SIZE || stack_count + 2 > W_BVH_STACK)
        {
            continue;
        }

        const Vec3 extent = chi - clo;
        const u32  axis   = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);

        const u32 count = node.count;
        const u32 mid   = count / 2;
        u32*      tris  = world->bvh_triangles.data + node.first;
        for (u32 i = 0; i < count; i++)
        {
            scratch[i] = { .key = triangle_centroid(world, tris[i], axis), .triangle = tris[i] };
        }
        quicksort_raw(scratch.data, count, sizeof(W_SortItem), compare_sort_items);
        for (u32 i = 0; i < count; i++)
        {
            tris[i] = scratch[i].triangle;
        }

        const u32 first      = node.first;
        const u32 left_index = (u32)world->bvh_nodes.length;
        world->bvh_nodes.add() = { .first = first, .count = mid };
        world->bvh_nodes.add() = { .first = first + mid, .count = count - mid };
        world->bvh_nodes[node_index].first = left_index;
        world->bvh_nodes[node_index].count = 0;
        stack[stack_count++] = left_index;
        stack[stack_count++] = left_index + 1;
    }
}

// A ray parallel to an axis has an infinite inverse there, and on a face of the box the slab
// would be 0 * inf, NaN: that axis only asks whether the ray lies within the slab
static b8 ray_slab (f32 origin, f32 inv_direction, f32 lo, f32 hi, f32& inout_tmin, f32& inout_tmax)
{
    if (!zm::is_finite(inv_direction))
    {
        return origin >= lo && origin <= hi;
    }
    const f32 t1 = (lo - origin) * inv_direction;
    const f32 t2 = (hi - origin) * inv_direction;
    inout_tmin   = zm::max(inout_tmin, zm::min(t1, t2));
    inout_tmax   = zm::min(inout_tmax, zm::max(t1, t2));
    return true;
}

static b8 ray_box (Vec3 origin, Vec3 inv_direction, const W_BvhNode& node, f32 max_t)
{
    f32 tmin = 0.0f;
    f32 tmax = max_t;
    if (!ray_slab(origin.x, inv_direction.x, node.min.x, node.max.x, tmin, tmax) || !ray_slab(origin.y, inv_direction.y, node.min.y, node.max.y, tmin, tmax) ||
        !ray_slab(origin.z, inv_direction.z, node.min.z, node.max.z, tmin, tmax))
    {
        return false;
    }
    return tmin <= tmax;
}

// Moeller-Trumbore, both faces
static b8 ray_triangle (Vec3 origin, Vec3 direction, Vec3 a, Vec3 b, Vec3 c, f32& out_t)
{
    const Vec3 e1  = b - a;
    const Vec3 e2  = c - a;
    const Vec3 p   = zm::cross(direction, e2);
    const f32  det = zm::dot(e1, p);
    if (zm::abs(det) < 1e-12f)
    {
        return false;
    }
    const f32  inv = 1.0f / det;
    const Vec3 t   = origin - a;
    const f32  u   = zm::dot(t, p) * inv;
    if (u < 0.0f || u > 1.0f)
    {
        return false;
    }
    const Vec3 q = zm::cross(t, e1);
    const f32  v = zm::dot(direction, q) * inv;
    if (v < 0.0f || u + v > 1.0f)
    {
        return false;
    }
    out_t = zm::dot(e2, q) * inv;
    return out_t > 0.0f;
}

static i32 compare_names (const char* a, const char* b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (i32)(u8)*a - (i32)(u8)*b;
}

// Insertion keeps the first `capacity` names in order, each once
static void insert_name (W_ListContext* context, const char* name)
{
    u32 at = 0;
    while (at < context->count && compare_names(context->names[at], name) < 0)
    {
        at++;
    }
    if (at < context->count && compare_names(context->names[at], name) == 0)
    {
        return;
    }
    if (at >= context->capacity)
    {
        return;
    }
    const u32 last = context->count < context->capacity ? context->count : context->capacity - 1;
    for (u32 i = last; i > at; i--)
    {
        memory_copy(context->names[i], context->names[i - 1], W_MAX_NAME);
    }
    cstr_copy(context->names[at], W_MAX_NAME, name);
    if (context->count < context->capacity)
    {
        context->count++;
    }
}

static void list_entry (const char* name, void* user)
{
    W_ListContext* context = (W_ListContext*)user;
    char path[OS_MAX_PATH];
    if (w_find_room_file(context->rooms_dir, name, path, sizeof(path)))
    {
        insert_name(context, name);
    }
}


//
// Anchor storage
//

static void append (Array<char>& out, const char* fmt, ...) PRINTF_LIKE(2, 3);
static void append (Array<char>& out, const char* fmt, ...)
{
    char    text[256];
    va_list args = {};
    va_start(args, fmt);
    const usize length = cstr_format_v(text, sizeof(text), fmt, args);
    va_end(args);
    ASSERT(length < sizeof(text));
    out.add(text, zm::min(length, sizeof(text) - 1));
}

// %.9g round-trips every f32
static void append_anchor (Array<char>& out, const W_StoredAnchor& anchor)
{
    char uuid[W_UUID_SIZE * 2 + 1];
    w_uuid_to_string(anchor.uuid, uuid);
    const Pose& p = anchor.pose;
    append(out, "{ \"uuid\": \"%s\", \"room\": \"%s\", \"room_hash\": \"%016llx\", ", uuid, anchor.room, (unsigned long long)anchor.room_hash);
    append(out, "\"pose\": [%.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g] }", (f64)p.position.x, (f64)p.position.y, (f64)p.position.z, (f64)p.orientation.x, (f64)p.orientation.y, (f64)p.orientation.z, (f64)p.orientation.w);
}

static b8 read_hex_u64 (const char* text, u64& out_value)
{
    const usize length = cstr_length(text);
    u64         value  = 0;
    if (length == 0 || length > 16)
    {
        return false;
    }
    for (usize i = 0; i < length; i++)
    {
        const char c = text[i];
        u64        digit = 0;
        if (c >= '0' && c <= '9')
        {
            digit = (u64)(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            digit = (u64)(c - 'a') + 10;
        }
        else
        {
            return false;
        }
        value = (value << 4) | digit;
    }
    out_value = value;
    return true;
}

static b8 read_uuid (const JsonValue& value, W_Uuid& out_uuid)
{
    char text[W_UUID_SIZE * 2 + 1];
    return json_as_string(value, text, nullptr, sizeof(text)) && w_uuid_from_string(text, out_uuid) && !w_uuid_is_zero(out_uuid);
}

static b8 read_stored_anchor (const JsonValue& value, W_StoredAnchor& out_anchor)
{
    out_anchor   = {};
    char hash[24] = {};
    f32  pose[7]  = {};
    if (!json_is_object(value) || !read_uuid(json_find(value, "uuid"), out_anchor.uuid))
    {
        return false;
    }
    if (!json_find_string(value, "room", out_anchor.room, nullptr, sizeof(out_anchor.room)) || (out_anchor.room[0] && !w_room_name_valid(out_anchor.room)))
    {
        return false;
    }
    if (!json_find_string(value, "room_hash", hash, nullptr, sizeof(hash)) || !read_hex_u64(hash, out_anchor.room_hash))
    {
        return false;
    }
    if (!read_floats(json_find(value, "pose"), pose, 7))
    {
        return false;
    }
    const Quat orientation = { pose[3], pose[4], pose[5], pose[6] };
    const f32  length      = zm::length(orientation);
    if (length < 1e-3f)
    {
        return false;
    }
    // Stored orientations are unit already; normalizing again would move their last bits
    out_anchor.pose = { { pose[0], pose[1], pose[2] }, zm::near(length, 1.0f, 1e-5f) ? orientation : zm::normalize(orientation) };
    return true;
}

static b8 read_version (const JsonValue& root, const char* what)
{
    u32 version = 0;
    if (!json_is_object(root) || !json_find_uint(root, "version", version) || version != W_STORE_VERSION)
    {
        LOG_ERROR("%s: not a version %u file", what, W_STORE_VERSION);
        return false;
    }
    return true;
}

static void cloud_path (const char* dir, const W_Uuid& uuid, char* out_path, usize size)
{
    char text[W_UUID_SIZE * 2 + 1];
    w_uuid_to_string(uuid, text);
    cstr_format(out_path, size, "%s/%s.json", dir, text);
}

static b8 cloud_parse (const char* json, W_CloudAnchor& out_anchor)
{
    w_cloud_release(out_anchor);
    out_anchor = {};
    JsonParser* parser = json_parse_from_string(json);
    defer { json_release(parser); };
    const JsonValue root = parser ? json_get_root(parser) : JsonValue{};
    if (!read_version(root, "Cloud anchor") || !read_stored_anchor(json_find(root, "anchor"), out_anchor.anchor) || !json_find_u64(root, "owner", out_anchor.owner) || out_anchor.owner == 0)
    {
        return false;
    }
    const JsonValue users  = json_find(root, "users");
    const JsonValue groups = json_find(root, "groups");
    if (!json_is_array(users) || !json_is_array(groups))
    {
        return false;
    }
    for (const JsonValue user : users)
    {
        u64 id = 0;
        if (!json_as_u64(user, id))
        {
            return false;
        }
        out_anchor.users.add(id);
    }
    for (const JsonValue group : groups)
    {
        if (!read_uuid(group, out_anchor.groups.add()))
        {
            return false;
        }
    }
    return true;
}

static void cloud_write (const W_CloudAnchor& anchor, Array<char>& out)
{
    append(out, "{\n    \"version\": %u,\n    \"owner\": %llu,\n    \"users\": [", W_STORE_VERSION, (unsigned long long)anchor.owner);
    for (usize i = 0; i < anchor.users.length; i++)
    {
        append(out, "%s%llu", i ? ", " : "", (unsigned long long)anchor.users[i]);
    }
    append(out, "],\n    \"groups\": [");
    for (usize i = 0; i < anchor.groups.length; i++)
    {
        char text[W_UUID_SIZE * 2 + 1];
        w_uuid_to_string(anchor.groups[i], text);
        append(out, "%s\"%s\"", i ? ", " : "", text);
    }
    append(out, "],\n    \"anchor\": ");
    append_anchor(out, anchor.anchor);
    append(out, "\n}\n");
}

static b8 read_text_file (const char* path, char*& out_text)
{
    u8*   data = nullptr;
    usize size = 0;
    if (!os_read_file(path, &data, &size))
    {
        return false;
    }
    out_text = (char*)os_memory_realloc(data, size + 1);
    out_text[size] = 0;
    return true;
}

static b8 json_file_name (const char* name, W_Uuid& out_uuid)
{
    char        text[W_UUID_SIZE * 2 + 1] = {};
    const usize length                    = cstr_length(name);
    const usize digits                    = (usize)W_UUID_SIZE * 2;
    if (length != digits + 5 || !cstr_equal(name + digits, ".json"))
    {
        return false;
    }
    memory_copy(text, name, digits);
    return w_uuid_from_string(text, out_uuid);
}

static void collect_entry (const char* name, void* user)
{
    Array<W_Uuid>* uuids = (Array<W_Uuid>*)user;
    W_Uuid         uuid  = {};
    if (json_file_name(name, uuid))
    {
        uuids->add(uuid);
    }
}

static i32 compare_uuids (const W_Uuid* a, const W_Uuid* b)
{
    for (u32 i = 0; i < W_UUID_SIZE; i++)
    {
        if (a->bytes[i] != b->bytes[i])
        {
            return a->bytes[i] < b->bytes[i] ? -1 : 1;
        }
    }
    return 0;
}

// The uuids named by <uuid>.json files of a directory, sorted
static void scan_directory (const char* dir, Array<W_Uuid>& out_uuids)
{
    out_uuids.clear();
    os_list_directory(dir, collect_entry, &out_uuids);
    quicksort(out_uuids.data, out_uuids.length, compare_uuids);
}

static u64 mix (u64 x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

//
// QR codes: ISO/IEC 18004, byte mode, error correction level M
//

// Level M, by version: error correction codewords per block and number of blocks
static const u8 w_qr_ecc_per_block[W_QR_MAX_VERSION + 1] = { 0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
static const u8 w_qr_blocks[W_QR_MAX_VERSION + 1]        = { 0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 };

static void qr_put_bits (u8* data, u32& inout_bit, u32 value, u32 count)
{
    for (u32 i = count; i-- > 0;)
    {
        data[inout_bit >> 3] |= (u8)(((value >> i) & 1) << (7 - (inout_bit & 7)));
        inout_bit++;
    }
}

static u32 qr_raw_modules (u32 version)
{
    u32 result = (16 * version + 128) * version + 64;
    if (version >= 2)
    {
        const u32 aligns = version / 7 + 2;
        result -= (25 * aligns - 10) * aligns - 55;
        if (version >= 7)
        {
            result -= 36;
        }
    }
    return result;
}

static u32 qr_data_codewords (u32 version)
{
    return qr_raw_modules(version) / 8 - (u32)w_qr_ecc_per_block[version] * w_qr_blocks[version];
}

// The smallest version whose byte mode segment holds `length` bytes, 0 when none does
static u32 qr_version (usize length)
{
    for (u32 version = 1; version <= W_QR_MAX_VERSION; version++)
    {
        if (4 + (version < 10 ? 8u : 16u) + length * 8 <= (usize)qr_data_codewords(version) * 8)
        {
            return version;
        }
    }
    return 0;
}

static u8 qr_gf_multiply (u8 a, u8 b)
{
    u32 r = 0;
    for (i32 i = 7; i >= 0; i--)
    {
        r = (r << 1) ^ ((r >> 7) * 0x11D);
        r ^= ((u32)(b >> i) & 1) * a;
    }
    return (u8)r;
}

// Generator polynomial of the given degree, highest coefficient (1) dropped
static void qr_rs_divisor (u32 degree, u8* out)
{
    memory_zero(out, degree);
    out[degree - 1] = 1;
    u8 root = 1;
    for (u32 i = 0; i < degree; i++)
    {
        for (u32 j = 0; j < degree; j++)
        {
            out[j] = qr_gf_multiply(out[j], root);
            if (j + 1 < degree)
            {
                out[j] ^= out[j + 1];
            }
        }
        root = qr_gf_multiply(root, 0x02);
    }
}

static void qr_rs_remainder (const u8* data, u32 length, const u8* divisor, u32 degree, u8* out)
{
    memory_zero(out, degree);
    for (u32 i = 0; i < length; i++)
    {
        const u8 factor = data[i] ^ out[0];
        for (u32 j = 0; j + 1 < degree; j++)
        {
            out[j] = out[j + 1];
        }
        out[degree - 1] = 0;
        for (u32 j = 0; j < degree; j++)
        {
            out[j] ^= qr_gf_multiply(divisor[j], factor);
        }
    }
}

static i32 qr_chebyshev (i32 dx, i32 dy)
{
    const i32 x = dx < 0 ? -dx : dx;
    const i32 y = dy < 0 ? -dy : dy;
    return x > y ? x : y;
}

static void qr_set_function (W_QrBuilder& qr, i32 x, i32 y, b8 dark)
{
    qr.modules[y][x]  = dark ? 1 : 0;
    qr.function[y][x] = true;
}

static void qr_finder (W_QrBuilder& qr, i32 cx, i32 cy)
{
    for (i32 dy = -4; dy <= 4; dy++)
    {
        for (i32 dx = -4; dx <= 4; dx++)
        {
            const i32 x    = cx + dx;
            const i32 y    = cy + dy;
            const i32 dist = qr_chebyshev(dx, dy);
            if (x >= 0 && y >= 0 && x < (i32)qr.size && y < (i32)qr.size)
            {
                qr_set_function(qr, x, y, dist != 2 && dist != 4);
            }
        }
    }
}

static u32 qr_alignment_positions (u32 version, u32* out)
{
    if (version == 1)
    {
        return 0;
    }
    const u32 count = version / 7 + 2;
    const u32 step  = (version * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
    out[0]          = 6;
    u32 position    = version * 4 + 10;
    for (u32 i = count - 1; i >= 1; i--)
    {
        out[i] = position;
        position -= step;
    }
    return count;
}

static void qr_format_bits (W_QrBuilder& qr, u32 mask)
{
    // Level M is 00
    const u32 data = mask;
    u32       rem  = data;
    for (u32 i = 0; i < 10; i++)
    {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    }
    const u32 bits = ((data << 10) | rem) ^ 0x5412;
    const i32 size = (i32)qr.size;
    for (i32 i = 0; i <= 5; i++)
    {
        qr_set_function(qr, 8, i, (bits >> i) & 1);
    }
    qr_set_function(qr, 8, 7, (bits >> 6) & 1);
    qr_set_function(qr, 8, 8, (bits >> 7) & 1);
    qr_set_function(qr, 7, 8, (bits >> 8) & 1);
    for (i32 i = 9; i < 15; i++)
    {
        qr_set_function(qr, 14 - i, 8, (bits >> i) & 1);
    }
    for (i32 i = 0; i < 8; i++)
    {
        qr_set_function(qr, size - 1 - i, 8, (bits >> i) & 1);
    }
    for (i32 i = 8; i < 15; i++)
    {
        qr_set_function(qr, 8, size - 15 + i, (bits >> i) & 1);
    }
    qr_set_function(qr, 8, size - 8, true);
}

static void qr_function_patterns (W_QrBuilder& qr, u32 version)
{
    const i32 size = (i32)qr.size;
    for (i32 i = 0; i < size; i++)
    {
        qr_set_function(qr, 6, i, i % 2 == 0);
        qr_set_function(qr, i, 6, i % 2 == 0);
    }
    qr_finder(qr, 3, 3);
    qr_finder(qr, size - 4, 3);
    qr_finder(qr, 3, size - 4);
    u32       positions[7];
    const u32 count = qr_alignment_positions(version, positions);
    for (u32 i = 0; i < count; i++)
    {
        for (u32 j = 0; j < count; j++)
        {
            if ((i == 0 && j == 0) || (i == 0 && j == count - 1) || (i == count - 1 && j == 0))
            {
                continue;
            }
            for (i32 dy = -2; dy <= 2; dy++)
            {
                for (i32 dx = -2; dx <= 2; dx++)
                {
                    const i32 dist = qr_chebyshev(dx, dy);
                    qr_set_function(qr, (i32)positions[i] + dx, (i32)positions[j] + dy, dist != 1);
                }
            }
        }
    }
    qr_format_bits(qr, 0); // reserves the area; drawn again with the chosen mask
    if (version >= 7)
    {
        u32 rem = version;
        for (u32 i = 0; i < 12; i++)
        {
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        }
        const u32 bits = (version << 12) | rem;
        for (i32 i = 0; i < 18; i++)
        {
            const b8  dark = (bits >> i) & 1;
            const i32 a    = size - 11 + i % 3;
            const i32 b    = i / 3;
            qr_set_function(qr, a, b, dark);
            qr_set_function(qr, b, a, dark);
        }
    }
}

static b8 qr_mask_bit (u32 mask, i32 x, i32 y)
{
    switch (mask)
    {
    case 0:
        return (x + y) % 2 == 0;
    case 1:
        return y % 2 == 0;
    case 2:
        return x % 3 == 0;
    case 3:
        return (x + y) % 3 == 0;
    case 4:
        return (x / 3 + y / 2) % 2 == 0;
    case 5:
        return x * y % 2 + x * y % 3 == 0;
    case 6:
        return (x * y % 2 + x * y % 3) % 2 == 0;
    default:
        return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

static void qr_apply_mask (W_QrBuilder& qr, u32 mask)
{
    for (i32 y = 0; y < (i32)qr.size; y++)
    {
        for (i32 x = 0; x < (i32)qr.size; x++)
        {
            if (!qr.function[y][x] && qr_mask_bit(mask, x, y))
            {
                qr.modules[y][x] ^= 1;
            }
        }
    }
}

// The four penalty rules of ISO/IEC 18004 7.8.3; rule 3 looks for 1011101 with four light modules
// on one side, inside the symbol
static u32 qr_penalty (const W_QrBuilder& qr)
{
    const i32 size    = (i32)qr.size;
    u32       penalty = 0;
    if (size <= 0)
    {
        return 0;
    }
    for (u32 pass = 0; pass < 2; pass++)
    {
        for (i32 a = 0; a < size; a++)
        {
            i32 run = 1;
            for (i32 b = 1; b <= size; b++)
            {
                const u8 previous = pass == 0 ? qr.modules[a][b - 1] : qr.modules[b - 1][a];
                if (b < size && (pass == 0 ? qr.modules[a][b] : qr.modules[b][a]) == previous)
                {
                    run++;
                    continue;
                }
                penalty += run >= 5 ? (u32)(3 + run - 5) : 0;
                run = 1;
            }
            for (i32 b = 0; b + 11 <= size; b++)
            {
                static const u8 pattern_a[11] = { 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0 };
                static const u8 pattern_b[11] = { 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1 };
                b8 match_a = true;
                b8 match_b = true;
                for (i32 k = 0; k < 11; k++)
                {
                    const u8 m = pass == 0 ? qr.modules[a][b + k] : qr.modules[b + k][a];
                    match_a &= m == pattern_a[k];
                    match_b &= m == pattern_b[k];
                }
                penalty += (match_a ? 40u : 0u) + (match_b ? 40u : 0u);
            }
        }
    }
    u32 dark = 0;
    for (i32 y = 0; y < size; y++)
    {
        for (i32 x = 0; x < size; x++)
        {
            dark += qr.modules[y][x];
            if (x + 1 < size && y + 1 < size)
            {
                const u8 m = qr.modules[y][x];
                penalty += m == qr.modules[y][x + 1] && m == qr.modules[y + 1][x] && m == qr.modules[y + 1][x + 1] ? 3u : 0u;
            }
        }
    }
    const i32 total = size * size;
    const i32 skew  = (i32)dark * 20 - total * 10;
    const i32 k     = ((skew < 0 ? -skew : skew) + total - 1) / total - 1;
    penalty += (u32)k * 10;
    return penalty;
}

//
// Public API
//

W_World* w_parse_room (const W_RoomSource& source)
{
    if (!source.json || (!source.binary && source.binary_size > 0))
    {
        return nullptr;
    }

    JsonParser* parser = json_parse_from_string(source.json);
    defer { json_release(parser); };
    const JsonValue root = json_get_root(parser);
    if (!parser || !json_is_object(root))
    {
        LOG_ERROR("Room file is not a JSON object");
        return nullptr;
    }

    char format[32];
    u32  version = 0;
    if (!json_find_string(root, "format", format, nullptr, sizeof(format)) || !cstr_equal(format, "puppetxr_room") || !json_find_uint(root, "version", version))
    {
        LOG_ERROR("Not a puppetxr room file");
        return nullptr;
    }
    if (version < 1 || version > W_FORMAT_VERSION)
    {
        LOG_ERROR("Room format version %u is not supported (this build reads 1 to %u)", version, W_FORMAT_VERSION);
        return nullptr;
    }

    W_World* world = (W_World*)os_memory_alloc(sizeof(W_World));
    b8       success = false;
    defer {
        if (!success)
        {
            w_destroy(world);
        }
    };

    if (!json_find_string(root, "name", world->name, nullptr, sizeof(world->name)) || !world->name[0])
    {
        LOG_ERROR("Room has no name");
        return nullptr;
    }
    if (!read_meshes(world, root, source) || !read_layout(world, root, source) || !read_image(world, source))
    {
        return nullptr;
    }

    u64 h = hash_combine(0xCBF29CE484222325ull, (const u8*)source.json, cstr_length(source.json));
    h     = hash_combine(h, source.binary, source.binary_size);
    h     = hash_combine(h, source.image, source.image ? source.image_size : 0);
    world->content_hash = h;

    build_bvh(world);
    success = true;
    return world;
}

W_World* w_load_room (const char* json_path)
{
    if (!json_path)
    {
        return nullptr;
    }

    u8*   json      = nullptr;
    usize json_size = 0;
    if (!os_read_file(json_path, &json, &json_size))
    {
        LOG_ERROR("Cannot read room file %s", json_path);
        return nullptr;
    }
    defer { os_memory_release(json); };

    // os_read_file terminates the data, but the json must not contain an early terminator either
    char* text = (char*)os_memory_alloc(json_size + 1);
    defer { os_memory_release(text); };
    memory_copy(text, json, json_size);

    JsonParser* parser = json_parse_from_string(text);
    defer { json_release(parser); };
    char binary_name[W_MAX_NAME] = {};
    char image_name[W_MAX_NAME]  = {};
    const JsonValue root = json_get_root(parser);
    if (!parser || !json_find_string(root, "binary", binary_name, nullptr, sizeof(binary_name)) || cstr_find(binary_name, "/") || cstr_find(binary_name, "\\"))
    {
        LOG_ERROR("Room file %s names no binary next to it", json_path);
        return nullptr;
    }
    if (json_find_string(root, "albedo", image_name, nullptr, sizeof(image_name)) && (cstr_find(image_name, "/") || cstr_find(image_name, "\\")))
    {
        LOG_ERROR("Room file %s names an albedo image outside its directory", json_path);
        return nullptr;
    }

    char        dir[OS_MAX_PATH];
    const char* slash = json_path;
    for (const char* c = json_path; *c; c++)
    {
        if (*c == '/' || *c == '\\')
        {
            slash = c + 1;
        }
    }
    cstr_copy(dir, zm::min((usize)(slash - json_path) + 1, sizeof(dir)), json_path);

    char path[OS_MAX_PATH];
    u8*  binary      = nullptr;
    usize binary_size = 0;
    cstr_format(path, sizeof(path), "%s%s", dir, binary_name);
    if (!os_read_file(path, &binary, &binary_size))
    {
        LOG_ERROR("Cannot read room binary %s", path);
        return nullptr;
    }
    defer { os_memory_release(binary); };

    u8*   image      = nullptr;
    usize image_size = 0;
    if (image_name[0])
    {
        cstr_format(path, sizeof(path), "%s%s", dir, image_name);
        if (!os_read_file(path, &image, &image_size))
        {
            LOG_ERROR("Cannot read room albedo %s", path);
            return nullptr;
        }
    }
    defer { os_memory_release(image); };

    return w_parse_room({ .json = text, .binary = binary, .binary_size = binary_size, .image = image, .image_size = image_size });
}

void w_destroy (W_World* world)
{
    if (!world)
    {
        return;
    }
    world->entities.release();
    world->boundary_points.release();
    world->rooms.release();
    world->room_indices.release();
    world->mesh_positions.release();
    world->mesh_indices.release();
    world->room_mesh_positions.release();
    world->room_mesh_faces.release();
    world->room_mesh_indices.release();
    world->visual_vertices.release();
    world->visual_indices.release();
    world->bvh_nodes.release();
    world->bvh_triangles.release();
    if (world->albedo)
    {
        stbi_image_free(world->albedo);
    }
    os_memory_release(world);
}

b8 w_find_room_file (const char* rooms_dir, const char* name, char* out_path, usize out_size)
{
    if (!w_room_name_valid(name) || !rooms_dir)
    {
        return false;
    }
    if (cstr_format(out_path, out_size, "%s/%s/room.json", rooms_dir, name) >= out_size)
    {
        return false;
    }
    return os_file_exists(out_path);
}

b8 w_room_name_valid (const char* name)
{
    const usize length = name ? cstr_length(name) : 0;
    if (length == 0 || length >= W_MAX_NAME)
    {
        return false;
    }
    for (usize i = 0; i < length; i++)
    {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
        {
            return false;
        }
    }
    return true;
}

u32 w_list_rooms (const char* rooms_dir, char (*out_names)[W_MAX_NAME], u32 capacity)
{
    if (!rooms_dir || !out_names || capacity == 0)
    {
        return 0;
    }
    W_ListContext context = { .rooms_dir = rooms_dir, .names = out_names, .capacity = capacity };
    os_list_directory(rooms_dir, list_entry, &context);
    return context.count;
}

u32 w_list_all_rooms (const char* rooms_dir, char (*out_names)[W_MAX_NAME], u32 capacity)
{
    if (!out_names || capacity == 0)
    {
        return 0;
    }
    W_ListContext context = { .rooms_dir = rooms_dir, .names = out_names, .capacity = capacity };
    if (rooms_dir)
    {
        os_list_directory(rooms_dir, list_entry, &context);
    }
    for (u32 i = 0; i < w_embedded_rooms_count; i++)
    {
        insert_name(&context, w_embedded_rooms[i].name);
    }
    return context.count;
}

W_World* w_open_room (const char* rooms_dir, const char* name)
{
    if (!rooms_dir || !w_room_name_valid(name))
    {
        return nullptr;
    }
    char path[OS_MAX_PATH];
    if (w_find_room_file(rooms_dir, name, path, sizeof(path)))
    {
        return w_load_room(path);
    }
    for (u32 i = 0; i < w_embedded_rooms_count; i++)
    {
        if (cstr_equal(w_embedded_rooms[i].name, name))
        {
            return w_parse_room(w_embedded_rooms[i].source);
        }
    }
    return nullptr;
}

const char* w_name (const W_World* world)
{
    ASSERT(world);
    return world->name;
}

u64 w_content_hash (const W_World* world)
{
    ASSERT(world);
    return world->content_hash;
}

u32 w_entity_count (const W_World* world)
{
    ASSERT(world);
    return (u32)world->entities.length;
}

const W_Entity& w_entity (const W_World* world, u32 index)
{
    ASSERT(world && index < world->entities.length);
    return world->entities[index];
}

b8 w_find_entity (const W_World* world, const W_Uuid& uuid, u32& out_index)
{
    ASSERT(world);
    for (u32 i = 0; i < world->entities.length; i++)
    {
        if (w_uuid_equal(world->entities[i].uuid, uuid))
        {
            out_index = i;
            return true;
        }
    }
    return false;
}

const Vec2* w_boundary_points (const W_World* world)
{
    ASSERT(world);
    return world->boundary_points.data;
}

u32 w_room_count (const W_World* world)
{
    ASSERT(world);
    return (u32)world->rooms.length;
}

const W_Room& w_room (const W_World* world, u32 index)
{
    ASSERT(world && index < world->rooms.length);
    return world->rooms[index];
}

const u32* w_room_indices (const W_World* world)
{
    ASSERT(world);
    return world->room_indices.data;
}

const Vec3* w_room_mesh_positions (const W_World* world)
{
    ASSERT(world);
    return world->room_mesh_positions.data;
}

const W_RoomMeshFace* w_room_mesh_faces (const W_World* world)
{
    ASSERT(world);
    return world->room_mesh_faces.data;
}

const u32* w_room_mesh_indices (const W_World* world)
{
    ASSERT(world);
    return world->room_mesh_indices.data;
}

void w_global_mesh (const W_World* world, const Vec3*& out_positions, u32& out_positions_count, const u32*& out_indices, u32& out_indices_count)
{
    ASSERT(world);
    out_positions       = world->mesh_positions.data;
    out_positions_count = (u32)world->mesh_positions.length;
    out_indices         = world->mesh_indices.data;
    out_indices_count   = (u32)world->mesh_indices.length;
}

void w_visual_mesh (const W_World* world, const W_VisualVertex*& out_vertices, u32& out_vertices_count, const u32*& out_indices, u32& out_indices_count)
{
    ASSERT(world);
    out_vertices       = world->visual_vertices.data;
    out_vertices_count = (u32)world->visual_vertices.length;
    out_indices        = world->visual_indices.data;
    out_indices_count  = (u32)world->visual_indices.length;
}

b8 w_albedo (const W_World* world, const u8*& out_rgba, u32& out_width, u32& out_height)
{
    ASSERT(world);
    out_rgba   = world->albedo;
    out_width  = world->albedo_width;
    out_height = world->albedo_height;
    return world->albedo != nullptr;
}

b8 w_raycast (const W_World* world, Vec3 origin, Vec3 direction, f32 max_distance, W_RaycastHit& out_hit)
{
    ASSERT(world);
    const f32 length = zm::length(direction);
    if (!zm::is_finite(length) || length < zm::EPSILON || !(max_distance > 0.0f) || !zm::is_finite(origin.x) || !zm::is_finite(origin.y) || !zm::is_finite(origin.z) || world->bvh_nodes.length == 0)
    {
        return false;
    }
    if (!zm::is_finite(max_distance))
    {
        max_distance = zm::F32_MAX;
    }

    const Vec3 d   = direction / length;
    const Vec3 inv = { 1.0f / d.x, 1.0f / d.y, 1.0f / d.z };

    f32 best          = max_distance;
    u32 best_triangle = W_NONE;
    u32 stack[W_BVH_STACK];
    u32 stack_count      = 0;
    stack[stack_count++] = 0;
    while (stack_count > 0)
    {
        const W_BvhNode& node = world->bvh_nodes[stack[--stack_count]];
        if (!ray_box(origin, inv, node, best))
        {
            continue;
        }
        if (node.count == 0)
        {
            stack[stack_count++] = node.first;
            stack[stack_count++] = node.first + 1;
            continue;
        }
        for (u32 i = 0; i < node.count; i++)
        {
            const u32  triangle = world->bvh_triangles[node.first + i];
            const u32* t        = world->mesh_indices.data + (usize)triangle * 3;
            f32        hit_t    = 0.0f;
            if (ray_triangle(origin, d, world->mesh_positions[t[0]], world->mesh_positions[t[1]], world->mesh_positions[t[2]], hit_t) && (hit_t < best || (hit_t == best && triangle < best_triangle)))
            {
                best          = hit_t;
                best_triangle = triangle;
            }
        }
    }
    if (best_triangle == W_NONE)
    {
        return false;
    }

    const u32* t      = world->mesh_indices.data + (usize)best_triangle * 3;
    const Vec3 a      = world->mesh_positions[t[0]];
    Vec3       normal = zm::normalize(zm::cross(world->mesh_positions[t[1]] - a, world->mesh_positions[t[2]] - a));
    if (zm::dot(normal, d) > 0.0f)
    {
        normal = -normal;
    }
    out_hit = { .distance = best, .point = origin + d * best, .normal = normal };
    return true;
}

const char* w_label_name (W_Label label)
{
    return (u32)label < (u32)W_Label::Count ? w_label_names[(u32)label] : "";
}

b8 w_label_from_name (const char* name, W_Label& out_label)
{
    if (!name)
    {
        return false;
    }
    for (u32 i = 0; i < (u32)W_Label::Count; i++)
    {
        if (cstr_equal(name, w_label_names[i]))
        {
            out_label = (W_Label)i;
            return true;
        }
    }
    return false;
}

b8 w_uuid_from_string (const char* str, W_Uuid& out_uuid)
{
    if (!str || cstr_length(str) != (usize)W_UUID_SIZE * 2)
    {
        return false;
    }
    W_Uuid uuid = {};
    for (u32 i = 0; i < W_UUID_SIZE * 2; i++)
    {
        const char c     = str[i];
        u32        digit = 0;
        if (c >= '0' && c <= '9')
        {
            digit = (u32)(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            digit = (u32)(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            digit = (u32)(c - 'A' + 10);
        }
        else
        {
            return false;
        }
        uuid.bytes[i / 2] = (u8)(uuid.bytes[i / 2] | (digit << ((i % 2) ? 0 : 4)));
    }
    out_uuid = uuid;
    return true;
}

b8 w_uuid_equal (const W_Uuid& a, const W_Uuid& b)
{
    return memory_equal(a.bytes, b.bytes, W_UUID_SIZE);
}

b8 w_uuid_is_zero (const W_Uuid& uuid)
{
    const W_Uuid zero = {};
    return w_uuid_equal(uuid, zero);
}

void w_uuid_to_string (const W_Uuid& uuid, char (&out)[W_UUID_SIZE * 2 + 1])
{
    static const char digits[] = "0123456789abcdef";
    for (u32 i = 0; i < W_UUID_SIZE; i++)
    {
        out[(usize)2 * i]     = digits[uuid.bytes[i] >> 4];
        out[(usize)2 * i + 1] = digits[uuid.bytes[i] & 15];
    }
    out[(usize)W_UUID_SIZE * 2] = 0;
}

void w_store_release (W_Store& store)
{
    store.anchors.release();
    store = {};
}

b8 w_store_parse (const char* json, W_Store& out_store)
{
    w_store_release(out_store);
    JsonParser* parser = json_parse_from_string(json);
    defer { json_release(parser); };
    const JsonValue root = parser ? json_get_root(parser) : JsonValue{};
    if (!read_version(root, "Anchor store"))
    {
        return false;
    }
    json_find_u64(root, "next_serial", out_store.next_serial);
    json_find_u64(root, "runs", out_store.runs);
    const JsonValue anchors = json_find(root, "anchors");
    if (!json_is_array(anchors))
    {
        LOG_ERROR("Anchor store: no anchors array");
        return false;
    }
    for (const JsonValue value : anchors)
    {
        W_StoredAnchor anchor = {};
        if (!read_stored_anchor(value, anchor))
        {
            LOG_ERROR("Anchor store: anchor %u is invalid", (u32)out_store.anchors.length);
            w_store_release(out_store);
            return false;
        }
        w_store_put(out_store, anchor);
    }
    return true;
}

void w_store_write (const W_Store& store, Array<char>& out_json)
{
    out_json.clear();
    append(out_json, "{\n    \"version\": %u,\n    \"next_serial\": %llu,\n    \"runs\": %llu,\n    \"anchors\": [", W_STORE_VERSION, (unsigned long long)store.next_serial, (unsigned long long)store.runs);
    for (usize i = 0; i < store.anchors.length; i++)
    {
        append(out_json, "%s\n        ", i ? "," : "");
        append_anchor(out_json, store.anchors[i]);
    }
    append(out_json, "%s]\n}\n", store.anchors.length ? "\n    " : "");
}

b8 w_store_load (const char* path, W_Store& out_store)
{
    w_store_release(out_store);
    if (!path || !os_file_exists(path))
    {
        return path != nullptr;
    }
    char* text = nullptr;
    if (!read_text_file(path, text))
    {
        LOG_ERROR("Anchor store: cannot read %s", path);
        return false;
    }
    defer { os_memory_release(text); };
    if (!w_store_parse(text, out_store))
    {
        LOG_ERROR("Anchor store %s is corrupt", path);
        return false;
    }
    return true;
}

b8 w_store_save (const char* path, const W_Store& store)
{
    char dir[OS_MAX_PATH];
    if (!path || !cstr_copy(dir, sizeof(dir), path))
    {
        return false;
    }
    for (usize i = cstr_length(dir); i-- > 0;)
    {
        if (dir[i] == '/' || dir[i] == '\\')
        {
            dir[i] = 0;
            break;
        }
    }
    Array<char> json = {};
    defer { json.release(); };
    w_store_write(store, json);
    if (!os_create_directories(dir) || !os_write_file_atomic(path, json.data, json.length))
    {
        LOG_ERROR("Anchor store: cannot write %s", path);
        return false;
    }
    return true;
}

b8 w_store_find (const W_Store& store, const W_Uuid& uuid, u32& out_index)
{
    for (u32 i = 0; i < store.anchors.length; i++)
    {
        if (w_uuid_equal(store.anchors[i].uuid, uuid))
        {
            out_index = i;
            return true;
        }
    }
    return false;
}

void w_store_put (W_Store& store, const W_StoredAnchor& anchor)
{
    u32 index = 0;
    if (w_store_find(store, anchor.uuid, index))
    {
        store.anchors[index] = anchor;
        return;
    }
    store.anchors.add(anchor);
}

b8 w_store_erase (W_Store& store, const W_Uuid& uuid)
{
    u32 index = 0;
    if (!w_store_find(store, uuid, index))
    {
        return false;
    }
    store.anchors.remove(index);
    return true;
}

b8 w_store_key (const char* app_name, char* out_key, usize size)
{
    if (!app_name || !app_name[0] || !out_key || size == 0)
    {
        return false;
    }
    usize i = 0;
    for (; app_name[i] && i + 1 < size; i++)
    {
        const char c  = app_name[i];
        const b8   ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (c == '.' && i > 0);
        out_key[i]    = ok ? c : '_';
    }
    out_key[i] = 0;
    if (cstr_equal(out_key, "_."))
    {
        out_key[1] = '_';
    }
    return true;
}

W_Uuid w_anchor_uuid (const char* app_key, u64 user, u64 serial)
{
    const u64 app  = hash_cstr(app_key ? app_key : "");
    const u64 high = mix(app ^ mix(user + 0x9e3779b97f4a7c15ull));
    const u64 low  = mix(high ^ mix(serial + 0x632be59bd9b4e019ull));
    W_Uuid    uuid = {};
    for (u32 i = 0; i < 8; i++)
    {
        uuid.bytes[i]     = (u8)(high >> (8 * (7 - i)));
        uuid.bytes[8 + i] = (u8)(low >> (8 * (7 - i)));
    }
    // RFC 9562 version 8 (custom) and variant bits, so tools recognize a uuid
    uuid.bytes[6] = (u8)((uuid.bytes[6] & 0x0f) | 0x80);
    uuid.bytes[8] = (u8)((uuid.bytes[8] & 0x3f) | 0x80);
    return uuid;
}

void w_cloud_release (W_CloudAnchor& anchor)
{
    anchor.users.release();
    anchor.groups.release();
}

void w_cloud_release_all (Array<W_CloudAnchor>& anchors)
{
    for (W_CloudAnchor& anchor : anchors)
    {
        w_cloud_release(anchor);
    }
    anchors.release();
}

b8 w_cloud_visible (const W_CloudAnchor& anchor, u64 user)
{
    if (anchor.owner == user)
    {
        return true;
    }
    for (const u64 id : anchor.users)
    {
        if (id == user)
        {
            return true;
        }
    }
    return false;
}

b8 w_cloud_in_group (const W_CloudAnchor& anchor, const W_Uuid& group)
{
    for (const W_Uuid& id : anchor.groups)
    {
        if (w_uuid_equal(id, group))
        {
            return true;
        }
    }
    return false;
}

b8 w_cloud_save (const char* dir, const W_CloudAnchor& anchor)
{
    char path[OS_MAX_PATH];
    if (!dir || !os_create_directories(dir))
    {
        return false;
    }
    cloud_path(dir, anchor.anchor.uuid, path, sizeof(path));
    Array<char> json = {};
    defer { json.release(); };
    cloud_write(anchor, json);
    return os_write_file_atomic(path, json.data, json.length);
}

b8 w_cloud_load (const char* dir, const W_Uuid& uuid, W_CloudAnchor& out_anchor)
{
    char  path[OS_MAX_PATH];
    char* text = nullptr;
    if (!dir)
    {
        return false;
    }
    cloud_path(dir, uuid, path, sizeof(path));
    if (!read_text_file(path, text))
    {
        return false;
    }
    defer { os_memory_release(text); };
    if (!cloud_parse(text, out_anchor) || !w_uuid_equal(out_anchor.anchor.uuid, uuid))
    {
        LOG_WARNING("Cloud anchor %s is corrupt, ignoring it", path);
        w_cloud_release(out_anchor);
        return false;
    }
    return true;
}

b8 w_cloud_erase (const char* dir, const W_Uuid& uuid)
{
    char path[OS_MAX_PATH];
    if (!dir)
    {
        return false;
    }
    cloud_path(dir, uuid, path, sizeof(path));
    return os_delete_file(path);
}

u32 w_cloud_list (const char* dir, Array<W_CloudAnchor>& out_anchors)
{
    for (W_CloudAnchor& anchor : out_anchors)
    {
        w_cloud_release(anchor);
    }
    out_anchors.clear();
    if (!dir)
    {
        return 0;
    }
    Array<W_Uuid> uuids = {};
    defer { uuids.release(); };
    scan_directory(dir, uuids);
    for (const W_Uuid& uuid : uuids)
    {
        W_CloudAnchor anchor = {};
        if (w_cloud_load(dir, uuid, anchor))
        {
            out_anchors.add(anchor);
        }
    }
    return (u32)out_anchors.length;
}

OS_LockFile* w_advert_start (const char* dir, const W_Advert& advert)
{
    char path[OS_MAX_PATH];
    if (!dir || advert.size > W_ADVERT_MAX_SIZE || !os_create_directories(dir))
    {
        return nullptr;
    }
    cloud_path(dir, advert.uuid, path, sizeof(path));
    Array<char> json = {};
    defer { json.release(); };
    append(json, "{ \"version\": %u, \"owner\": %llu, \"buffer\": \"", W_STORE_VERSION, (unsigned long long)advert.owner);
    for (u32 i = 0; i < advert.size; i++)
    {
        append(json, "%02x", advert.buffer[i]);
    }
    append(json, "\" }\n");
    return os_lock_file_create(path, json.data, json.length);
}

void w_advert_stop (OS_LockFile* file)
{
    os_lock_file_release(file);
}

u32 w_advert_list (const char* dir, Array<W_Advert>& out_adverts)
{
    out_adverts.clear();
    if (!dir)
    {
        return 0;
    }
    Array<W_Uuid> uuids = {};
    defer { uuids.release(); };
    scan_directory(dir, uuids);
    for (const W_Uuid& uuid : uuids)
    {
        char path[OS_MAX_PATH];
        cloud_path(dir, uuid, path, sizeof(path));
        if (os_lock_file_is_stale(path))
        {
            os_delete_file(path);
            continue;
        }
        char* text = nullptr;
        if (!read_text_file(path, text))
        {
            continue;
        }
        defer { os_memory_release(text); };
        JsonParser* parser = json_parse_from_string(text);
        defer { json_release(parser); };
        const JsonValue root   = parser ? json_get_root(parser) : JsonValue{};
        W_Advert        advert = { .uuid = uuid };
        char            hex[W_ADVERT_MAX_SIZE * 2 + 1] = {};
        usize           length = 0;
        if (!read_version(root, "Colocation advert") || !json_find_u64(root, "owner", advert.owner) || !json_find_string(root, "buffer", hex, &length, sizeof(hex)) || length % 2 != 0)
        {
            continue;
        }
        b8 valid = true;
        for (usize i = 0; i < length; i += 2)
        {
            char pair[3] = { hex[i], hex[i + 1], 0 };
            u64  byte    = 0;
            valid        = valid && read_hex_u64(pair, byte);
            advert.buffer[i / 2] = (u8)byte;
        }
        advert.size = (u32)(length / 2);
        if (valid)
        {
            out_adverts.add(advert);
        }
    }
    return (u32)out_adverts.length;
}

Pose w_origin_offset_for_run (u64 seed, u64 run)
{
    const u64 bits     = mix(mix(seed + 0x9e3779b97f4a7c15ull) ^ (run * 0xbf58476d1ce4e5b9ull));
    const f32 yaw      = (f32)(bits & 0xffffffu) / 16777216.0f * zm::TAU;
    const f32 angle    = (f32)((bits >> 24) & 0xffffffu) / 16777216.0f * zm::TAU;
    const f32 distance = zm::sqrt((f32)((bits >> 48) & 0xffffu) / 65536.0f) * W_ORIGIN_OFFSET_MAX_DISTANCE;
    return { { distance * zm::cos(angle), 0.0f, distance * zm::sin(angle) }, { 0.0f, zm::sin(yaw * 0.5f), 0.0f, zm::cos(yaw * 0.5f) } };
}

u32 w_qr_size (const char* text)
{
    const u32 version = text ? qr_version(cstr_length(text)) : 0;
    return version ? 17 + 4 * version : 0;
}

b8 w_qr_encode (const char* text, W_QrCode& out_code)
{
    const u32 length  = text ? (u32)cstr_length(text) : 0;
    const u32 version = text ? qr_version(length) : 0;
    if (version == 0)
    {
        return false;
    }

    // Byte mode segment, terminator, pad to a byte, then the alternating pad codewords
    u8        data[W_QR_MAX_CODEWORDS] = {};
    const u32 capacity  = qr_data_codewords(version);
    u32       bit       = 0;
    qr_put_bits(data, bit, 0x4, 4);
    qr_put_bits(data, bit, length, version < 10 ? 8 : 16);
    for (u32 i = 0; i < length; i++)
    {
        qr_put_bits(data, bit, (u8)text[i], 8);
    }
    const u32 terminator = capacity * 8 - bit < 4 ? capacity * 8 - bit : 4;
    qr_put_bits(data, bit, 0, terminator);
    qr_put_bits(data, bit, 0, (8 - bit % 8) % 8);
    for (u32 pad = 0xEC; bit < capacity * 8; pad ^= 0xEC ^ 0x11)
    {
        qr_put_bits(data, bit, pad, 8);
    }

    // Blocks: the short ones first, each with its error correction, then interleaved
    const u32 blocks      = w_qr_blocks[version];
    const u32 ecc         = w_qr_ecc_per_block[version];
    const u32 raw         = qr_raw_modules(version) / 8;
    const u32 short_count = blocks - raw % blocks;
    const u32 short_len   = raw / blocks; // data plus ecc of a short block
    u8        divisor[W_QR_MAX_ECC];
    qr_rs_divisor(ecc, divisor);
    u8  block_data[W_QR_MAX_BLOCKS][W_QR_MAX_BLOCK_DATA] = {};
    u8  block_ecc[W_QR_MAX_BLOCKS][W_QR_MAX_ECC]         = {};
    u32 offset            = 0;
    for (u32 b = 0; b < blocks; b++)
    {
        const u32 data_len = short_len - ecc + (b >= short_count ? 1 : 0);
        memory_copy(block_data[b], data + offset, data_len);
        qr_rs_remainder(data + offset, data_len, divisor, ecc, block_ecc[b]);
        offset += data_len;
    }
    u8  codewords[W_QR_MAX_CODEWORDS] = {};
    u32 written        = 0;
    for (u32 i = 0; i <= short_len - ecc; i++)
    {
        for (u32 b = 0; b < blocks; b++)
        {
            if (i < short_len - ecc || b >= short_count)
            {
                codewords[written++] = block_data[b][i];
            }
        }
    }
    for (u32 i = 0; i < ecc; i++)
    {
        for (u32 b = 0; b < blocks; b++)
        {
            codewords[written++] = block_ecc[b][i];
        }
    }

    W_QrBuilder qr = { .size = 17 + 4 * version };
    qr_function_patterns(qr, version);
    const i32 size  = (i32)qr.size;
    u32       index = 0;
    for (i32 right = size - 1; right >= 1; right -= 2)
    {
        if (right == 6)
        {
            right = 5;
        }
        for (i32 vert = 0; vert < size; vert++)
        {
            for (i32 j = 0; j < 2; j++)
            {
                const i32 x      = right - j;
                const b8  upward = ((right + 1) & 2) == 0;
                const i32 y      = upward ? size - 1 - vert : vert;
                if (!qr.function[y][x] && index < written * 8)
                {
                    qr.modules[y][x] = (codewords[index >> 3] >> (7 - (index & 7))) & 1;
                    index++;
                }
            }
        }
    }

    u32 best         = 0;
    u32 best_penalty = 0xFFFFFFFFu;
    for (u32 mask = 0; mask < 8; mask++)
    {
        qr_apply_mask(qr, mask);
        qr_format_bits(qr, mask);
        const u32 penalty = qr_penalty(qr);
        if (penalty < best_penalty)
        {
            best         = mask;
            best_penalty = penalty;
        }
        qr_apply_mask(qr, mask);
    }
    qr_apply_mask(qr, best);
    qr_format_bits(qr, best);
    out_code.version = version;
    out_code.size    = qr.size;
    memory_copy(out_code.modules, qr.modules, sizeof(qr.modules));
    return true;
}
