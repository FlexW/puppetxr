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
#define W_BVH_BINS        16
#define W_MAX_ELEMENTS    (1u << 26)
#define W_NONE            0xffffffffu
#define W_ROOM_MESH_PLANAR 1e-3f // metres a room mesh face may stray from its plane

namespace
{

struct W_BvhNode
{
    Vec3 min;
    Vec3 max;
    u32  first; // leaf: first entry in W_Bvh::triangles; inner: index of the left child, the right one follows it
    u32  count; // entries of a leaf, 0 for an inner node
};

// A triangle's bounds as the build sorts it
struct W_BvhRef
{
    Vec3 min;
    Vec3 max;
    u32  triangle;
};

struct W_Bvh
{
    const Vec3*      positions;
    const u32*       indices;
    Array<W_BvhNode> nodes;
    Array<u32>       triangles;
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
    Array<Vec3>           visual_positions; // of visual_vertices, for visual_bvh
    W_Bvh                 mesh_bvh;
    W_Bvh                 visual_bvh;
};

namespace
{

struct W_BufferView
{
    u32 offset;
    u32 count;
};

struct W_BvhBin
{
    Vec3 min;
    Vec3 max;
    u32  count;
};

// A ray with what every box test needs; `parallel` marks the axes it does not move along
struct W_Ray
{
    Vec3 origin;
    Vec3 inv_direction;
    b8   parallel[3];
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

// A model as the glTF reader leaves it: every mesh placed by its nodes, a part per primitive
struct W_ModelMaterial
{
    f32 factor[4]; // linear RGBA
    u32 image;     // into W_ModelSource::images, W_NONE untextured
};

struct W_ModelImage
{
    u32 offset; // into W_ModelSource::image_bytes
    u32 size;
    u32 texels; // decoded
};

struct W_SourcePart
{
    u32 vertices_first; // the part's own vertices; its indices point into them
    u32 vertices_count;
    u32 indices_first;
    u32 indices_count;
    u32 material;
};

struct W_ModelSource
{
    Array<Vec3>            positions;
    Array<Vec2>            uvs; // one per position, zero where a primitive has none
    b8                     has_uvs;
    Array<u32>             indices;
    Array<W_SourcePart>    parts;
    Array<W_ModelMaterial> materials;
    Array<W_ModelImage>    images;
    Array<u8>              image_bytes; // PNG or JPEG files as the model holds them
};

struct W_GltfBuffer
{
    const u8* data;
    u32       size;
};

struct W_GltfView
{
    u32 buffer;
    u32 offset;
    u32 length;
    u32 stride; // 0 for tightly packed
};

struct W_Accessor
{
    const u8* data;
    u32       count;
    u32       stride;
    u32       component_type;
    b8        normalized;
};

// An allocation a glTF document read owns: a file or a decoded data URI
struct W_OwnedBytes
{
    u8* data;
};

// A glTF document being read: buffers either point into the GLB or are files and data URIs it owns
struct W_Gltf
{
    JsonValue           root;
    const char*         dir;
    Array<W_GltfBuffer> buffers;
    Array<W_OwnedBytes> owned;
    Array<W_GltfView>   views;
    Array<u32>          material_map; // glTF material to source material, W_NONE before its first use
    Array<u32>          image_map;
    u32                 default_material;
    u32                 visits;
    b8                  warned_points;
};

struct W_ModelTexture
{
    u8* rgba;
    u32 width;
    u32 height;
};

// An instance prepared for the rays of one origin: that origin in its model space, and a sphere
// around it in room space that rays which miss it skip
struct W_TraceInstance
{
    const W_Model* model;
    Quat           inverse;
    Vec3           inv_scale;
    Vec3           origin;
    Vec3           to_center; // from the rays' origin to the sphere's centre
    f32            radius;
};

// Where a node of the scene is visited from: its node index, its transform to the model, its depth
struct W_NodeVisit
{
    u32  node;
    Mat4 model_from_node;
    u32  depth;
};

} // namespace

struct W_Model
{
    Array<W_VisualVertex> vertices;
    Array<Vec3>           positions; // of vertices, for bvh
    Array<u32>            indices;
    Array<W_ModelPart>    parts;
    Array<W_ModelTexture> images;
    Vec3                  min;
    Vec3                  max;
    W_Bvh                 bvh;
};

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

static f32 axis_of (Vec3 v, u32 axis)
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

static f32 surface_area (Vec3 lo, Vec3 hi)
{
    const Vec3 e = hi - lo;
    return e.x * e.y + e.y * e.z + e.z * e.x;
}

static u32 bin_of (const W_BvhRef& ref, u32 axis, f32 lo, f32 span)
{
    const f32 c = axis_of((ref.min + ref.max) * 0.5f, axis);
    return (u32)zm::min((c - lo) / span * (f32)W_BVH_BINS, (f32)(W_BVH_BINS - 1));
}

// Deterministic for a given mesh
static void build_bvh (W_Bvh& bvh, const Vec3* positions, const u32* indices, u32 triangles)
{
    bvh.positions = positions;
    bvh.indices   = indices;
    if (triangles == 0)
    {
        return;
    }

    Array<W_BvhRef> refs = {};
    defer { refs.release(); };
    refs.resize(triangles);
    for (u32 t = 0; t < triangles; t++)
    {
        const u32* v = indices + (usize)t * 3;
        const Vec3 a = positions[v[0]];
        const Vec3 b = positions[v[1]];
        const Vec3 c = positions[v[2]];
        refs[t]      = { .min = zm::min(a, zm::min(b, c)), .max = zm::max(a, zm::max(b, c)), .triangle = t };
    }
    const u32  count_refs = (u32)refs.length;
    Array<u32> order      = {};
    defer { order.release(); };
    order.resize(count_refs);
    for (u32 i = 0; i < count_refs; i++)
    {
        order[i] = i;
    }

    Array<u32> scratch = {};
    defer { scratch.release(); };
    scratch.resize(count_refs);
    bvh.nodes.reserve((usize)count_refs * 2);
    bvh.nodes.add() = { .first = 0, .count = count_refs };

    u32 stack[W_BVH_STACK];
    u32 stack_count     = 0;
    stack[stack_count++] = 0;
    while (stack_count > 0)
    {
        const u32  node_index = stack[--stack_count];
        W_BvhNode& node       = bvh.nodes[node_index];

        Vec3 lo = { zm::F32_MAX, zm::F32_MAX, zm::F32_MAX };
        Vec3 hi = -lo;
        for (u32 i = 0; i < node.count; i++)
        {
            const W_BvhRef& ref = refs[order[node.first + i]];
            lo                  = zm::min(lo, ref.min);
            hi                  = zm::max(hi, ref.max);
        }
        node.min = lo;
        node.max = hi;

        if (node.count <= W_BVH_LEAF_SIZE || stack_count + 2 > W_BVH_STACK)
        {
            continue;
        }

        // Binned surface area heuristic: the triangles go into W_BVH_BINS bins by centroid along
        // each axis, and the boundary between bins with the least cost, the areas of both sides
        // weighted by their counts, splits them. Without one (all centroids in one place) the half
        // in list order goes left
        const u32 count = node.count;
        u32*      items = order.data + node.first;
        Vec3      clo   = { zm::F32_MAX, zm::F32_MAX, zm::F32_MAX };
        Vec3      chi   = -clo;
        for (u32 i = 0; i < count; i++)
        {
            const Vec3 c = (refs[items[i]].min + refs[items[i]].max) * 0.5f;
            clo          = zm::min(clo, c);
            chi          = zm::max(chi, c);
        }
        f32 best_cost  = zm::F32_MAX;
        u32 best_axis  = 3;
        u32 best_split = 0;
        for (u32 axis = 0; axis < 3; axis++)
        {
            const f32 lo_c = axis_of(clo, axis);
            const f32 span = axis_of(chi, axis) - lo_c;
            if (!(span > 0.0f))
            {
                continue;
            }
            W_BvhBin bins[W_BVH_BINS];
            for (W_BvhBin& bin : bins)
            {
                bin = { .min = { zm::F32_MAX, zm::F32_MAX, zm::F32_MAX }, .max = { -zm::F32_MAX, -zm::F32_MAX, -zm::F32_MAX } };
            }
            for (u32 i = 0; i < count; i++)
            {
                const W_BvhRef& ref = refs[items[i]];
                W_BvhBin&       bin = bins[bin_of(ref, axis, lo_c, span)];
                bin.count++;
                bin.min = zm::min(bin.min, ref.min);
                bin.max = zm::max(bin.max, ref.max);
            }
            f32  right_areas[W_BVH_BINS];
            u32  right_counts[W_BVH_BINS];
            Vec3 rlo = { zm::F32_MAX, zm::F32_MAX, zm::F32_MAX };
            Vec3 rhi = -rlo;
            u32  rn  = 0;
            for (u32 b = W_BVH_BINS; b-- > 1;)
            {
                rlo             = zm::min(rlo, bins[b].min);
                rhi             = zm::max(rhi, bins[b].max);
                rn             += bins[b].count;
                right_areas[b]  = surface_area(rlo, rhi);
                right_counts[b] = rn;
            }
            Vec3 llo = { zm::F32_MAX, zm::F32_MAX, zm::F32_MAX };
            Vec3 lhi = -llo;
            u32  ln  = 0;
            for (u32 b = 1; b < W_BVH_BINS; b++)
            {
                llo = zm::min(llo, bins[b - 1].min);
                lhi = zm::max(lhi, bins[b - 1].max);
                ln += bins[b - 1].count;
                if (ln == 0 || right_counts[b] == 0)
                {
                    continue;
                }
                const f32 cost = surface_area(llo, lhi) * (f32)ln + right_areas[b] * (f32)right_counts[b];
                if (cost < best_cost)
                {
                    best_cost  = cost;
                    best_axis  = axis;
                    best_split = b;
                }
            }
        }

        // A stable partition keeps the build deterministic
        u32 mid = count / 2;
        if (best_axis < 3)
        {
            const f32 lo_c = axis_of(clo, best_axis);
            const f32 span = axis_of(chi, best_axis) - lo_c;
            u32       left = 0;
            for (u32 i = 0; i < count; i++)
            {
                left += bin_of(refs[items[i]], best_axis, lo_c, span) < best_split ? 1u : 0u;
            }
            u32 l = 0;
            u32 r = left;
            for (u32 i = 0; i < count; i++)
            {
                scratch[bin_of(refs[items[i]], best_axis, lo_c, span) < best_split ? l++ : r++] = items[i];
            }
            memory_copy(items, scratch.data, (usize)count * sizeof(u32));
            mid = left;
        }

        const u32 first      = node.first;
        const u32 left_index = (u32)bvh.nodes.length;
        bvh.nodes.add() = { .first = first, .count = mid };
        bvh.nodes.add() = { .first = first + mid, .count = count - mid };
        bvh.nodes[node_index].first = left_index;
        bvh.nodes[node_index].count = 0;
        stack[stack_count++] = left_index;
        stack[stack_count++] = left_index + 1;
    }

    bvh.triangles.resize(count_refs);
    for (u32 i = 0; i < count_refs; i++)
    {
        bvh.triangles[i] = refs[order[i]].triangle;
    }
}

static W_Ray make_ray (Vec3 origin, Vec3 direction)
{
    W_Ray ray         = { .origin = origin, .inv_direction = { 1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z } };
    ray.parallel[0]   = !zm::is_finite(ray.inv_direction.x);
    ray.parallel[1]   = !zm::is_finite(ray.inv_direction.y);
    ray.parallel[2]   = !zm::is_finite(ray.inv_direction.z);
    return ray;
}

// A ray parallel to an axis has an infinite inverse there, and on a face of the box the slab
// would be 0 * inf, NaN: that axis only asks whether the ray lies within the slab
static b8 ray_slab (f32 origin, f32 inv_direction, b8 parallel, f32 lo, f32 hi, f32& inout_tmin, f32& inout_tmax)
{
    if (parallel)
    {
        return origin >= lo && origin <= hi;
    }
    const f32 t1 = (lo - origin) * inv_direction;
    const f32 t2 = (hi - origin) * inv_direction;
    inout_tmin   = zm::max(inout_tmin, zm::min(t1, t2));
    inout_tmax   = zm::min(inout_tmax, zm::max(t1, t2));
    return true;
}

static b8 ray_box (const W_Ray& ray, const W_BvhNode& node, f32 max_t, f32& out_tmin)
{
    f32 tmin = 0.0f;
    f32 tmax = max_t;
    if (!ray_slab(ray.origin.x, ray.inv_direction.x, ray.parallel[0], node.min.x, node.max.x, tmin, tmax) || !ray_slab(ray.origin.y, ray.inv_direction.y, ray.parallel[1], node.min.y, node.max.y, tmin, tmax) ||
        !ray_slab(ray.origin.z, ray.inv_direction.z, ray.parallel[2], node.min.z, node.max.z, tmin, tmax))
    {
        return false;
    }
    out_tmin = tmin;
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

// The nearest triangle within (0, inout_best] in units of `direction`, which need not be unit
// length; ties go to the lower triangle index. Nearer children are visited first, so most boxes
// behind the first hit are skipped
static u32 nearest_hit (const W_Bvh& bvh, Vec3 origin, Vec3 direction, f32& inout_best)
{
    if (bvh.nodes.length == 0)
    {
        return W_NONE;
    }
    const W_Ray ray          = make_ray(origin, direction);
    u32        best_triangle = W_NONE;
    u32        stack[W_BVH_STACK];
    f32        stack_tmin[W_BVH_STACK];
    u32        stack_count = 0;
    f32        root_tmin   = 0.0f;
    if (!ray_box(ray, bvh.nodes[0], inout_best, root_tmin))
    {
        return W_NONE;
    }
    stack[stack_count]        = 0;
    stack_tmin[stack_count++] = root_tmin;
    while (stack_count > 0)
    {
        stack_count--;
        if (stack_tmin[stack_count] > inout_best)
        {
            continue;
        }
        const W_BvhNode& node = bvh.nodes[stack[stack_count]];
        if (node.count == 0)
        {
            f32      tmin_left  = 0.0f;
            f32      tmin_right = 0.0f;
            const b8 left       = ray_box(ray, bvh.nodes[node.first], inout_best, tmin_left);
            const b8 right      = ray_box(ray, bvh.nodes[node.first + 1], inout_best, tmin_right);
            // The nearer child goes on top
            const b8 left_first = !right || (left && tmin_left <= tmin_right);
            if (left && right)
            {
                stack[stack_count]        = left_first ? node.first + 1 : node.first;
                stack_tmin[stack_count++] = left_first ? tmin_right : tmin_left;
            }
            if (left || right)
            {
                stack[stack_count]        = left_first ? node.first : node.first + 1;
                stack_tmin[stack_count++] = left_first ? tmin_left : tmin_right;
            }
            continue;
        }
        for (u32 i = 0; i < node.count; i++)
        {
            const u32  triangle = bvh.triangles[node.first + i];
            const u32* t        = bvh.indices + (usize)triangle * 3;
            f32        hit_t    = 0.0f;
            if (ray_triangle(origin, direction, bvh.positions[t[0]], bvh.positions[t[1]], bvh.positions[t[2]], hit_t) && (hit_t < inout_best || (hit_t == inout_best && triangle < best_triangle)))
            {
                inout_best    = hit_t;
                best_triangle = triangle;
            }
        }
    }
    return best_triangle;
}

//
// Models: glTF 2.0 (Khronos glTF 2.0 specification)
//

#define W_GLB_MAGIC      0x46546C67u // "glTF"
#define W_GLB_JSON       0x4E4F534Au
#define W_GLB_BIN        0x004E4942u
#define W_GLTF_MAX_DEPTH 32
#define W_GLTF_MAX_VISITS 65536
#define W_GLTF_FLOAT     5126u
#define W_GLTF_UBYTE     5121u
#define W_GLTF_USHORT    5123u
#define W_GLTF_UINT      5125u

static void release_source (W_ModelSource& source)
{
    source.positions.release();
    source.uvs.release();
    source.indices.release();
    source.parts.release();
    source.materials.release();
    source.images.release();
    source.image_bytes.release();
}

static void release_gltf (W_Gltf& gltf)
{
    for (const W_OwnedBytes& owned : gltf.owned)
    {
        os_memory_release(owned.data);
    }
    gltf.owned.release();
    gltf.buffers.release();
    gltf.views.release();
    gltf.material_map.release();
    gltf.image_map.release();
}

static u32 read_le32 (const u8* p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void put_le32 (Array<u8>& out, u32 value)
{
    for (u32 i = 0; i < 4; i++)
    {
        out.add((u8)(value >> (8 * i)));
    }
}

// The JSON text, terminated, and the BIN chunk of a GLB, or the whole file as the JSON of a .gltf
static b8 split_gltf (const u8* data, usize size, Array<char>& out_json, const u8*& out_bin, usize& out_bin_size)
{
    out_bin      = nullptr;
    out_bin_size = 0;
    out_json.clear();
    if (!data || size == 0 || size > W_MODEL_MAX_FILE_SIZE)
    {
        LOG_ERROR("Model: empty or larger than %u bytes", W_MODEL_MAX_FILE_SIZE);
        return false;
    }
    if (size >= 4 && read_le32(data) == W_GLB_MAGIC)
    {
        // Spec glTF 2.0 4.4 (GLB): a 12 byte header, then a JSON chunk, then an optional BIN chunk
        if (size < 20 || read_le32(data + 4) != 2 || read_le32(data + 8) > size || read_le32(data + 8) < 20)
        {
            LOG_ERROR("Model: not a GLB of version 2, or cut short");
            return false;
        }
        const usize length    = read_le32(data + 8);
        const usize json_size = read_le32(data + 12);
        if (read_le32(data + 16) != W_GLB_JSON || json_size > length - 20)
        {
            LOG_ERROR("Model: the GLB's first chunk is not JSON or runs past the file");
            return false;
        }
        out_json.add((const char*)data + 20, json_size);
        const usize next = 20 + ((json_size + 3) & ~(usize)3);
        if (next + 8 <= length && read_le32(data + next + 4) == W_GLB_BIN)
        {
            const usize bin_size = read_le32(data + next);
            if (bin_size > length - next - 8)
            {
                LOG_ERROR("Model: the GLB's BIN chunk runs past the file");
                return false;
            }
            out_bin      = data + next + 8;
            out_bin_size = bin_size;
        }
    }
    else
    {
        out_json.add((const char*)data, size);
    }
    for (const char c : out_json)
    {
        if (c == 0)
        {
            LOG_ERROR("Model: its JSON holds a zero byte");
            return false;
        }
    }
    out_json.add(0);
    return true;
}

static i32 base64_digit (char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z')
    {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9')
    {
        return c - '0' + 52;
    }
    return c == '+' ? 62 : (c == '/' ? 63 : -1);
}

// RFC 4648 base64; JSON may escape the slash, padding ends the data
static b8 decode_base64 (const char* text, usize length, Array<u8>& out)
{
    u32 bits  = 0;
    u32 count = 0;
    for (usize i = 0; i < length; i++)
    {
        const char c = text[i];
        if (c == '\\')
        {
            continue;
        }
        if (c == '=')
        {
            break;
        }
        const i32 digit = base64_digit(c);
        if (digit < 0)
        {
            return false;
        }
        bits = bits << 6 | (u32)digit;
        count += 6;
        if (count >= 8)
        {
            count -= 8;
            out.add((u8)(bits >> count));
        }
        if (out.length > W_MODEL_MAX_FILE_SIZE)
        {
            return false;
        }
    }
    return true;
}

static i32 hex_digit (char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// A data URI, or a file relative to the document's directory (percent-decoded, no scheme, not absolute)
static b8 read_uri (W_Gltf& gltf, const JsonValue& value, const u8*& out_data, usize& out_size)
{
    const char* text   = nullptr;
    usize       length = 0;
    if (!json_is_string(value) || !json_raw(value, text, length))
    {
        return false;
    }
    if (length >= 5 && memory_equal(text, "data:", 5))
    {
        usize start = 0;
        for (usize i = 5; i + 8 <= length && start == 0; i++)
        {
            start = memory_equal(text + i, ";base64,", 8) ? i + 8 : 0;
        }
        Array<u8> bytes = {};
        if (start == 0 || !decode_base64(text + start, length - start, bytes))
        {
            bytes.release();
            LOG_ERROR("Model: a data URI that is not base64");
            return false;
        }
        gltf.owned.add({ bytes.data });
        out_data = bytes.data;
        out_size = bytes.length;
        return true;
    }

    char name[OS_MAX_PATH];
    u32  n = 0;
    for (usize i = 0; i < length; i++)
    {
        char c = text[i];
        if (c == '%' && i + 2 < length && hex_digit(text[i + 1]) >= 0 && hex_digit(text[i + 2]) >= 0)
        {
            c = (char)(hex_digit(text[i + 1]) * 16 + hex_digit(text[i + 2]));
            i += 2;
        }
        else if (c == '\\' && i + 1 < length && text[i + 1] == '/')
        {
            continue;
        }
        if (c == 0 || c == ':' || n + 1 >= sizeof(name))
        {
            LOG_ERROR("Model: a URI that is neither a data URI nor a relative file");
            return false;
        }
        name[n++] = c;
    }
    name[n] = 0;
    if (!gltf.dir || n == 0 || name[0] == '/' || name[0] == '\\')
    {
        LOG_ERROR("Model: %s names a file but has no directory to find it in", n ? name : "a URI");
        return false;
    }
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/%s", gltf.dir, name);
    u8*   data = nullptr;
    usize size = 0;
    if (!os_read_file(path, &data, &size))
    {
        LOG_ERROR("Model: cannot read %s", path);
        return false;
    }
    gltf.owned.add({ data });
    if (size > W_MODEL_MAX_FILE_SIZE)
    {
        LOG_ERROR("Model: %s is larger than %u bytes", path, W_MODEL_MAX_FILE_SIZE);
        return false;
    }
    out_data = data;
    out_size = size;
    return true;
}

static b8 read_buffers (W_Gltf& gltf, const u8* bin, usize bin_size)
{
    const JsonValue buffers = json_find(gltf.root, "buffers");
    const u32       count   = json_is_array(buffers) ? json_length(buffers) : 0;
    for (u32 i = 0; i < count; i++)
    {
        const JsonValue buffer = json_at(buffers, i);
        u32             length = 0;
        if (!json_is_object(buffer) || !json_find_uint(buffer, "byteLength", length))
        {
            LOG_ERROR("Model: buffer %u has no byteLength", i);
            return false;
        }
        const JsonValue uri  = json_find(buffer, "uri");
        const u8*       data = nullptr;
        usize           size = 0;
        if (json_is_valid(uri))
        {
            if (!read_uri(gltf, uri, data, size))
            {
                return false;
            }
        }
        else if (i == 0 && bin)
        {
            data = bin;
            size = bin_size;
        }
        if (size < length)
        {
            LOG_ERROR("Model: buffer %u holds %zu bytes of its %u", i, size, length);
            return false;
        }
        gltf.buffers.add({ data, length });
    }

    const JsonValue views       = json_find(gltf.root, "bufferViews");
    const u32       views_count = json_is_array(views) ? json_length(views) : 0;
    for (u32 i = 0; i < views_count; i++)
    {
        const JsonValue view   = json_at(views, i);
        W_GltfView      out    = {};
        if (!json_is_object(view) || !json_find_uint(view, "buffer", out.buffer) || !json_find_uint(view, "byteLength", out.length) || out.buffer >= gltf.buffers.length)
        {
            LOG_ERROR("Model: buffer view %u names no buffer or length", i);
            return false;
        }
        json_find_uint(view, "byteOffset", out.offset, 0);
        json_find_uint(view, "byteStride", out.stride, 0);
        if ((u64)out.offset + out.length > gltf.buffers[out.buffer].size || (out.stride != 0 && (out.stride < 4 || out.stride > 252)))
        {
            LOG_ERROR("Model: buffer view %u runs past its buffer or has a bad stride", i);
            return false;
        }
        gltf.views.add(out);
    }
    return true;
}

static u32 component_size (u32 type)
{
    switch (type)
    {
        case 5120:
        case W_GLTF_UBYTE:
            return 1;
        case 5122:
        case W_GLTF_USHORT:
            return 2;
        case W_GLTF_UINT:
        case W_GLTF_FLOAT:
            return 4;
        default:
            return 0;
    }
}

static b8 read_accessor (const W_Gltf& gltf, u32 index, const char* type, u32 components, W_Accessor& out)
{
    const JsonValue accessors = json_find(gltf.root, "accessors");
    const JsonValue accessor  = json_is_array(accessors) && index < json_length(accessors) ? json_at(accessors, index) : JsonValue{};
    char            got[8]    = {};
    u32             view      = 0;
    u32             offset    = 0;
    if (!json_is_object(accessor))
    {
        LOG_ERROR("Model: accessor %u does not exist", index);
        return false;
    }
    if (json_is_valid(json_find(accessor, "sparse")))
    {
        LOG_ERROR("Model: accessor %u is sparse, which puppetxr does not read", index);
        return false;
    }
    out = {};
    if (!json_find_uint(accessor, "bufferView", view) || view >= gltf.views.length || !json_find_uint(accessor, "componentType", out.component_type) || !json_find_uint(accessor, "count", out.count) || out.count == 0 ||
        !json_find_string(accessor, "type", got, nullptr, sizeof(got)) || !cstr_equal(got, type) || component_size(out.component_type) == 0)
    {
        LOG_ERROR("Model: accessor %u is not a %s in a buffer view", index, type);
        return false;
    }
    json_find_uint(accessor, "byteOffset", offset, 0);
    json_find_bool(accessor, "normalized", out.normalized, false);
    const W_GltfView& v       = gltf.views[view];
    const u32         element = components * component_size(out.component_type);
    out.stride                = v.stride ? v.stride : element;
    if (out.stride < element || (u64)offset + (u64)(out.count - 1) * out.stride + element > v.length)
    {
        LOG_ERROR("Model: accessor %u runs past its buffer view", index);
        return false;
    }
    out.data = gltf.buffers[v.buffer].data + v.offset + offset;
    return true;
}

static f32 accessor_f32 (const W_Accessor& accessor, u32 index, u32 component)
{
    const u8* p = accessor.data + (usize)index * accessor.stride;
    switch (accessor.component_type)
    {
        case W_GLTF_FLOAT:
        {
            f32 value = 0.0f;
            memory_copy(&value, p + (usize)component * 4, 4);
            return value;
        }
        case W_GLTF_UBYTE:
            return (f32)p[component] / 255.0f;
        case W_GLTF_USHORT:
        {
            u16 value = 0;
            memory_copy(&value, p + (usize)component * 2, 2);
            return (f32)value / 65535.0f;
        }
        default:
            return 0.0f;
    }
}

static u32 accessor_index (const W_Accessor& accessor, u32 index)
{
    const u8* p = accessor.data + (usize)index * accessor.stride;
    switch (accessor.component_type)
    {
        case W_GLTF_UBYTE:
            return p[0];
        case W_GLTF_USHORT:
        {
            u16 value = 0;
            memory_copy(&value, p, 2);
            return value;
        }
        default:
        {
            u32 value = 0;
            memory_copy(&value, p, 4);
            return value;
        }
    }
}

static b8 read_image_bytes (W_Gltf& gltf, u32 index, W_ModelSource& source, u32& out_image)
{
    if (index < gltf.image_map.length && gltf.image_map[index] != W_NONE)
    {
        out_image = gltf.image_map[index];
        return true;
    }
    const JsonValue images = json_find(gltf.root, "images");
    const JsonValue image  = json_is_array(images) && index < json_length(images) ? json_at(images, index) : JsonValue{};
    if (!json_is_object(image))
    {
        LOG_ERROR("Model: image %u does not exist", index);
        return false;
    }
    const u8* data = nullptr;
    usize     size = 0;
    u32       view = 0;
    if (json_find_uint(image, "bufferView", view))
    {
        if (view >= gltf.views.length)
        {
            LOG_ERROR("Model: image %u names no buffer view", index);
            return false;
        }
        data = gltf.buffers[gltf.views[view].buffer].data + gltf.views[view].offset;
        size = gltf.views[view].length;
    }
    else if (!read_uri(gltf, json_find(image, "uri"), data, size))
    {
        LOG_ERROR("Model: image %u has neither a buffer view nor a URI", index);
        return false;
    }
    int width    = 0;
    int height   = 0;
    int channels = 0;
    if (size == 0 || size > 0x7fffffff || !stbi_info_from_memory(data, (int)size, &width, &height, &channels) || width <= 0 || height <= 0 || width > W_MODEL_MAX_IMAGE_SIZE || height > W_MODEL_MAX_IMAGE_SIZE)
    {
        LOG_ERROR("Model: image %u is not a PNG or JPEG of at most %u texels per side", index, W_MODEL_MAX_IMAGE_SIZE);
        return false;
    }
    const W_ModelImage out = { .offset = (u32)source.image_bytes.length, .size = (u32)size, .texels = (u32)width * (u32)height };
    source.image_bytes.add(data, size);
    if (source.image_bytes.length > W_MODEL_MAX_FILE_SIZE)
    {
        LOG_ERROR("Model: its images hold more than %u bytes", W_MODEL_MAX_FILE_SIZE);
        return false;
    }
    while (gltf.image_map.length <= index)
    {
        gltf.image_map.add(W_NONE);
    }
    gltf.image_map[index] = (u32)source.images.length;
    out_image             = (u32)source.images.length;
    source.images.add(out);
    return true;
}

// The source material of a glTF material, -1 (none) being the spec's default: white, untextured
static b8 read_material (W_Gltf& gltf, i64 index, W_ModelSource& source, u32& out_material)
{
    if (index < 0)
    {
        if (gltf.default_material == W_NONE)
        {
            gltf.default_material = (u32)source.materials.length;
            source.materials.add({ .factor = { 1.0f, 1.0f, 1.0f, 1.0f }, .image = W_NONE });
        }
        out_material = gltf.default_material;
        return true;
    }
    if ((u64)index < gltf.material_map.length && gltf.material_map[(usize)index] != W_NONE)
    {
        out_material = gltf.material_map[(usize)index];
        return true;
    }
    const JsonValue materials = json_find(gltf.root, "materials");
    const JsonValue material  = json_is_array(materials) && (u64)index < json_length(materials) ? json_at(materials, (u32)index) : JsonValue{};
    if (!json_is_object(material))
    {
        LOG_ERROR("Model: material %lld does not exist", (long long)index);
        return false;
    }
    W_ModelMaterial out    = { .factor = { 1.0f, 1.0f, 1.0f, 1.0f }, .image = W_NONE };
    const JsonValue pbr    = json_find(material, "pbrMetallicRoughness");
    const JsonValue factor = json_find(pbr, "baseColorFactor");
    if (json_is_valid(factor) && !read_floats(factor, out.factor, 4))
    {
        LOG_ERROR("Model: material %lld has a bad baseColorFactor", (long long)index);
        return false;
    }
    for (f32& f : out.factor)
    {
        f = zm::clamp(f, 0.0f, 1.0f);
    }
    const JsonValue texture   = json_find(pbr, "baseColorTexture");
    u32             tex_index = 0;
    u32             tex_coord = 0;
    json_find_uint(texture, "texCoord", tex_coord, 0);
    if (json_is_object(texture) && json_find_uint(texture, "index", tex_index) && tex_coord == 0)
    {
        const JsonValue textures = json_find(gltf.root, "textures");
        const JsonValue tex      = json_is_array(textures) && tex_index < json_length(textures) ? json_at(textures, tex_index) : JsonValue{};
        u32             image    = 0;
        if (!json_is_object(tex) || !json_find_uint(tex, "source", image) || !read_image_bytes(gltf, image, source, out.image))
        {
            LOG_ERROR("Model: material %lld names a texture without an image", (long long)index);
            return false;
        }
    }
    while (gltf.material_map.length <= (usize)index)
    {
        gltf.material_map.add(W_NONE);
    }
    gltf.material_map[(usize)index] = (u32)source.materials.length;
    out_material                    = (u32)source.materials.length;
    source.materials.add(out);
    return true;
}

static b8 read_primitive (W_Gltf& gltf, const JsonValue& primitive, const Mat4& model_from_node, b8 flip, W_ModelSource& source)
{
    u32 mode = 4;
    json_find_uint(primitive, "mode", mode, 4);
    if (mode <= 3)
    {
        if (!gltf.warned_points)
        {
            LOG_WARNING("Model: points and lines are skipped");
            gltf.warned_points = true;
        }
        return true;
    }
    if (mode > 6)
    {
        LOG_ERROR("Model: primitive mode %u does not exist", mode);
        return false;
    }
    if (json_is_valid(json_find(primitive, "targets")))
    {
        LOG_ERROR("Model: morph targets are not supported");
        return false;
    }
    const JsonValue attributes = json_find(primitive, "attributes");
    u32             index      = 0;
    W_Accessor      positions  = {};
    if (!json_is_object(attributes) || !json_find_uint(attributes, "POSITION", index) || !read_accessor(gltf, index, "VEC3", 3, positions) || positions.component_type != W_GLTF_FLOAT)
    {
        LOG_ERROR("Model: a primitive without float positions");
        return false;
    }
    W_Accessor uvs     = {};
    const b8   has_uvs = json_find_uint(attributes, "TEXCOORD_0", index);
    if (has_uvs && (!read_accessor(gltf, index, "VEC2", 2, uvs) || uvs.count != positions.count ||
                    !(uvs.component_type == W_GLTF_FLOAT || (uvs.normalized && (uvs.component_type == W_GLTF_UBYTE || uvs.component_type == W_GLTF_USHORT)))))
    {
        LOG_ERROR("Model: TEXCOORD_0 is not float or normalized unsigned, or not one per position");
        return false;
    }
    W_Accessor indices     = {};
    const b8   has_indices = json_find_uint(primitive, "indices", index);
    if (has_indices && (!read_accessor(gltf, index, "SCALAR", 1, indices) || !(indices.component_type == W_GLTF_UBYTE || indices.component_type == W_GLTF_USHORT || indices.component_type == W_GLTF_UINT)))
    {
        LOG_ERROR("Model: indices are not unsigned integers");
        return false;
    }
    i32 material_index = -1;
    json_find_int(primitive, "material", material_index, -1);
    u32 material = 0;
    if (!read_material(gltf, material_index, source, material))
    {
        return false;
    }

    const u32 base = (u32)source.positions.length;
    if ((u64)base + positions.count > W_MODEL_MAX_VERTICES)
    {
        LOG_ERROR("Model: more than %u vertices", W_MODEL_MAX_VERTICES);
        return false;
    }
    for (u32 v = 0; v < positions.count; v++)
    {
        const Vec3 p = zm::transform_point(model_from_node, { accessor_f32(positions, v, 0), accessor_f32(positions, v, 1), accessor_f32(positions, v, 2) });
        if (!coordinate_valid(p.x) || !coordinate_valid(p.y) || !coordinate_valid(p.z))
        {
            LOG_ERROR("Model: a position that is not finite or beyond %g m", (f64)W_MAX_COORDINATE);
            return false;
        }
        source.positions.add(p);
        source.uvs.add(has_uvs ? Vec2{ accessor_f32(uvs, v, 0), accessor_f32(uvs, v, 1) } : Vec2{});
        if (has_uvs && (!zm::is_finite(source.uvs.last().x) || !zm::is_finite(source.uvs.last().y)))
        {
            LOG_ERROR("Model: a texture coordinate that is not finite");
            return false;
        }
    }
    source.has_uvs = source.has_uvs || has_uvs;

    const u32 count = has_indices ? indices.count : positions.count;
    const u32 first = (u32)source.indices.length;
    u32       tri[3];
    for (u32 i = 0; i < count; i++)
    {
        const u32 vertex = has_indices ? accessor_index(indices, i) : i;
        if (vertex >= positions.count)
        {
            LOG_ERROR("Model: an index past the primitive's %u vertices", positions.count);
            return false;
        }
        b8 emit = false;
        if (mode == 4)
        {
            tri[i % 3] = vertex;
            emit       = i % 3 == 2;
        }
        else if (i >= 2)
        {
            // Spec glTF 2.0 3.7.2.1: strips alternate their winding, fans share the first vertex
            const u32 previous = has_indices ? accessor_index(indices, i - 1) : i - 1;
            const u32 anchor   = mode == 5 ? (has_indices ? accessor_index(indices, i - 2) : i - 2) : (has_indices ? accessor_index(indices, 0) : 0);
            const b8  odd      = mode == 5 && (i % 2) == 1;
            tri[0]             = odd ? previous : anchor;
            tri[1]             = odd ? anchor : previous;
            tri[2]             = vertex;
            emit               = true;
        }
        if (emit)
        {
            source.indices.add(base + tri[0]);
            source.indices.add(base + (flip ? tri[2] : tri[1]));
            source.indices.add(base + (flip ? tri[1] : tri[2]));
        }
    }
    if (source.indices.length / 3 > W_MODEL_MAX_TRIANGLES)
    {
        LOG_ERROR("Model: more than %u triangles", W_MODEL_MAX_TRIANGLES);
        return false;
    }
    if (source.indices.length > first)
    {
        if (source.parts.length == W_MODEL_MAX_PARTS)
        {
            LOG_ERROR("Model: more than %u primitives", W_MODEL_MAX_PARTS);
            return false;
        }
        source.parts.add({ .vertices_first = base, .vertices_count = positions.count, .indices_first = first, .indices_count = (u32)source.indices.length - first, .material = material });
    }
    return true;
}

static b8 node_transform (const JsonValue& node, Mat4& out)
{
    f32 values[16];
    if (json_is_valid(json_find(node, "matrix")))
    {
        if (!read_floats(json_find(node, "matrix"), values, 16))
        {
            return false;
        }
        memory_copy(out.m, values, sizeof(values));
        return true;
    }
    Vec3 t = {};
    Quat r = zm::QUAT_IDENTITY;
    Vec3 s = { 1.0f, 1.0f, 1.0f };
    if ((json_is_valid(json_find(node, "translation")) && !read_vec3(json_find(node, "translation"), t)) || (json_is_valid(json_find(node, "scale")) && !read_vec3(json_find(node, "scale"), s)))
    {
        return false;
    }
    if (json_is_valid(json_find(node, "rotation")))
    {
        if (!read_floats(json_find(node, "rotation"), values, 4))
        {
            return false;
        }
        r                = { values[0], values[1], values[2], values[3] };
        const f32 length = zm::length(r);
        if (!(length > zm::EPSILON) || !zm::is_finite(length))
        {
            return false;
        }
        r = r * (1.0f / length);
    }
    out = zm::mat4_translation(t) * zm::mat4_from_quat(r) * zm::mat4_scale(s);
    return true;
}

static f32 determinant3 (const Mat4& m)
{
    return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[2][1] * m.m[1][2]) - m.m[1][0] * (m.m[0][1] * m.m[2][2] - m.m[2][1] * m.m[0][2]) + m.m[2][0] * (m.m[0][1] * m.m[1][2] - m.m[1][1] * m.m[0][2]);
}

static b8 read_node (W_Gltf& gltf, const W_NodeVisit& visit, W_ModelSource& source, Array<W_NodeVisit>& stack)
{
    const JsonValue nodes = json_find(gltf.root, "nodes");
    const JsonValue node  = json_is_array(nodes) && visit.node < json_length(nodes) ? json_at(nodes, visit.node) : JsonValue{};
    Mat4            local = zm::MAT4_IDENTITY;
    if (!json_is_object(node) || !node_transform(node, local))
    {
        LOG_ERROR("Model: node %u does not exist or has a bad transform", visit.node);
        return false;
    }
    if (visit.depth > W_GLTF_MAX_DEPTH || ++gltf.visits > W_GLTF_MAX_VISITS)
    {
        LOG_ERROR("Model: its nodes nest deeper than %u or form a cycle", W_GLTF_MAX_DEPTH);
        return false;
    }
    if (json_is_valid(json_find(node, "skin")))
    {
        LOG_ERROR("Model: skins are not supported");
        return false;
    }
    const Mat4 model_from_node = visit.model_from_node * local;
    u32        mesh_index      = 0;
    if (json_find_uint(node, "mesh", mesh_index))
    {
        const JsonValue meshes     = json_find(gltf.root, "meshes");
        const JsonValue mesh       = json_is_array(meshes) && mesh_index < json_length(meshes) ? json_at(meshes, mesh_index) : JsonValue{};
        const JsonValue primitives = json_find(mesh, "primitives");
        if (!json_is_array(primitives))
        {
            LOG_ERROR("Model: mesh %u does not exist or has no primitives", mesh_index);
            return false;
        }
        const b8 flip = determinant3(model_from_node) < 0.0f;
        for (u32 p = 0; p < json_length(primitives); p++)
        {
            if (!json_is_object(json_at(primitives, p)) || !read_primitive(gltf, json_at(primitives, p), model_from_node, flip, source))
            {
                return false;
            }
        }
    }
    const JsonValue children = json_find(node, "children");
    for (u32 c = 0; json_is_array(children) && c < json_length(children); c++)
    {
        u32 child = 0;
        if (!json_as_uint(json_at(children, c), child))
        {
            return false;
        }
        stack.add({ .node = child, .model_from_node = model_from_node, .depth = visit.depth + 1 });
    }
    return true;
}

// Every mesh of the default scene placed by its nodes, depth first in document order
static b8 read_gltf (const u8* data, usize size, const char* dir, W_ModelSource& out_source)
{
    Array<char> json     = {};
    const u8*   bin      = nullptr;
    usize       bin_size = 0;
    defer { json.release(); };
    if (!split_gltf(data, size, json, bin, bin_size))
    {
        return false;
    }
    JsonParser* parser = json_parse_from_string(json.data);
    defer { json_release(parser); };
    W_Gltf gltf = { .root = json_get_root(parser), .dir = dir, .default_material = W_NONE };
    defer { release_gltf(gltf); };
    char version[16] = {};
    if (!parser || !json_is_object(gltf.root) || !json_find_string(json_find(gltf.root, "asset"), "version", version, nullptr, sizeof(version)) || version[0] != '2' || version[1] != '.')
    {
        LOG_ERROR("Model: not a glTF 2.0 file");
        return false;
    }
    const JsonValue required = json_find(gltf.root, "extensionsRequired");
    if (json_is_array(required) && json_length(required) > 0)
    {
        char name[64] = {};
        json_as_string(json_at(required, 0), name, nullptr, sizeof(name));
        LOG_ERROR("Model: it requires the extension %s, which puppetxr does not read", name);
        return false;
    }
    if (json_is_array(json_find(gltf.root, "animations")) && json_length(json_find(gltf.root, "animations")) > 0)
    {
        LOG_WARNING("Model: its animations are ignored, it stays in its rest pose");
    }
    if (!read_buffers(gltf, bin, bin_size))
    {
        return false;
    }

    // Spec glTF 2.0 3.5.1: without a scene the roots are the nodes no node names as a child
    Array<W_NodeVisit> stack = {};
    defer { stack.release(); };
    const JsonValue scenes = json_find(gltf.root, "scenes");
    const JsonValue nodes  = json_find(gltf.root, "nodes");
    u32             scene  = 0;
    json_find_uint(gltf.root, "scene", scene, 0);
    if (json_is_array(scenes) && json_length(scenes) > 0)
    {
        const JsonValue roots = json_find(json_at(scenes, scene), "nodes");
        if (scene >= json_length(scenes) || !json_is_array(roots))
        {
            LOG_ERROR("Model: scene %u does not exist or has no nodes", scene);
            return false;
        }
        for (u32 i = json_length(roots); i > 0; i--)
        {
            u32 node = 0;
            if (!json_as_uint(json_at(roots, i - 1), node))
            {
                return false;
            }
            stack.add({ .node = node, .model_from_node = zm::MAT4_IDENTITY, .depth = 0 });
        }
    }
    else if (json_is_array(nodes))
    {
        Array<b8> child = {};
        defer { child.release(); };
        child.resize(json_length(nodes));
        for (u32 n = 0; n < json_length(nodes); n++)
        {
            const JsonValue children = json_find(json_at(nodes, n), "children");
            for (u32 c = 0; json_is_array(children) && c < json_length(children); c++)
            {
                u32 index = 0;
                if (json_as_uint(json_at(children, c), index) && index < child.length)
                {
                    child[index] = true;
                }
            }
        }
        for (u32 n = json_length(nodes); n > 0; n--)
        {
            if (!child[n - 1])
            {
                stack.add({ .node = n - 1, .model_from_node = zm::MAT4_IDENTITY, .depth = 0 });
            }
        }
    }
    while (stack.length > 0)
    {
        const W_NodeVisit visit = stack.last();
        stack.remove(stack.length - 1);
        const usize children_first = stack.length;
        if (!read_node(gltf, visit, out_source, stack))
        {
            return false;
        }
        // Children were pushed in order; reversed, the first child is visited next
        for (usize a = children_first, b = stack.length; a + 1 < b; a++, b--)
        {
            const W_NodeVisit swap = stack[a];
            stack[a]               = stack[b - 1];
            stack[b - 1]           = swap;
        }
    }
    // Each material decodes its texture on its own, so one small file used by many materials
    // would decode to gigabytes
    u64 texels = 0;
    for (const W_ModelMaterial& material : out_source.materials)
    {
        texels += material.image != W_NONE ? out_source.images[material.image].texels : 0;
    }
    if (texels > W_MODEL_MAX_TEXELS)
    {
        LOG_ERROR("Model: its materials' textures decode to more than %u texels", W_MODEL_MAX_TEXELS);
        return false;
    }
    if (out_source.parts.length == 0)
    {
        LOG_ERROR("Model: its scene has no triangles");
        return false;
    }
    return true;
}

static void append_json (Array<char>& out, const char* fmt, ...) PRINTF_LIKE(2, 3);
static void append_json (Array<char>& out, const char* fmt, ...)
{
    char    text[512];
    va_list args = {};
    va_start(args, fmt);
    const usize length = cstr_format_v(text, sizeof(text), fmt, args);
    va_end(args);
    ASSERT(length < sizeof(text));
    out.add(text, zm::min(length, sizeof(text) - 1));
}

static void append_f32 (Array<char>& out, f32 value)
{
    char text[32];
    out.add(text, json_format_f32(text, sizeof(text), value));
}

// The canonical GLB of a source: one node, one mesh, a primitive per part with its own vertices,
// so reading it back gives the same source
static void write_glb (const W_ModelSource& source, Array<u8>& out)
{
    usize vertices = 0;
    for (const W_SourcePart& part : source.parts)
    {
        vertices += part.vertices_count;
    }
    const usize uvs_at    = vertices * sizeof(Vec3);
    const usize index_at  = uvs_at + (source.has_uvs ? vertices * sizeof(Vec2) : 0);
    const usize images_at = index_at + source.indices.length * sizeof(u32);
    const u32   per_part  = source.has_uvs ? 3u : 2u; // accessors
    const u32   index_view = source.has_uvs ? 2u : 1u;

    Array<char> json = {};
    defer { json.release(); };
    append_json(json, "{\"asset\":{\"generator\":\"puppetxr\",\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[");
    for (usize p = 0; p < source.parts.length; p++)
    {
        const u32 a = (u32)p * per_part;
        if (source.has_uvs)
        {
            append_json(json, "%s{\"attributes\":{\"POSITION\":%u,\"TEXCOORD_0\":%u},\"indices\":%u,\"material\":%u}", p ? "," : "", a, a + 1, a + 2, source.parts[p].material);
        }
        else
        {
            append_json(json, "%s{\"attributes\":{\"POSITION\":%u},\"indices\":%u,\"material\":%u}", p ? "," : "", a, a + 1, source.parts[p].material);
        }
    }
    append_json(json, "]}],\"materials\":[");
    for (usize m = 0; m < source.materials.length; m++)
    {
        const W_ModelMaterial& material = source.materials[m];
        append_json(json, "%s{\"pbrMetallicRoughness\":{\"baseColorFactor\":[", m ? "," : "");
        for (u32 c = 0; c < 4; c++)
        {
            if (c)
            {
                json.add(',');
            }
            append_f32(json, material.factor[c]);
        }
        json.add(']');
        if (material.image != W_NONE)
        {
            append_json(json, ",\"baseColorTexture\":{\"index\":%u}", material.image);
        }
        append_json(json, "}}");
    }
    append_json(json, "]");
    const u32 image_view = index_view + 1;
    if (source.images.length > 0)
    {
        append_json(json, ",\"textures\":[");
        for (usize i = 0; i < source.images.length; i++)
        {
            append_json(json, "%s{\"source\":%u}", i ? "," : "", (u32)i);
        }
        append_json(json, "],\"images\":[");
        for (usize i = 0; i < source.images.length; i++)
        {
            const u8* bytes = source.image_bytes.data + source.images[i].offset;
            append_json(json, "%s{\"bufferView\":%u,\"mimeType\":\"%s\"}", i ? "," : "", image_view + (u32)i, bytes[0] == 0x89 ? "image/png" : "image/jpeg");
        }
        append_json(json, "]");
    }
    append_json(json, ",\"accessors\":[");
    u32 vertex = 0;
    for (usize p = 0; p < source.parts.length; p++)
    {
        const W_SourcePart& part = source.parts[p];
        Vec3                lo   = source.positions[part.vertices_first];
        Vec3                hi   = lo;
        for (u32 v = part.vertices_first; v < part.vertices_first + part.vertices_count; v++)
        {
            lo = zm::min(lo, source.positions[v]);
            hi = zm::max(hi, source.positions[v]);
        }
        append_json(json, "%s{\"bufferView\":0,\"byteOffset\":%u,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\",\"min\":[", p ? "," : "", vertex * 12, part.vertices_count);
        const f32 bounds[6] = { lo.x, lo.y, lo.z, hi.x, hi.y, hi.z };
        for (u32 b = 0; b < 6; b++)
        {
            append_json(json, "%s", b == 0 ? "" : (b == 3 ? "],\"max\":[" : ","));
            append_f32(json, bounds[b]);
        }
        append_json(json, "]}");
        if (source.has_uvs)
        {
            append_json(json, ",{\"bufferView\":1,\"byteOffset\":%u,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"}", vertex * 8, part.vertices_count);
        }
        append_json(json, ",{\"bufferView\":%u,\"byteOffset\":%u,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}", index_view, part.indices_first * 4, part.indices_count);
        vertex += part.vertices_count;
    }
    append_json(json, "],\"bufferViews\":[{\"buffer\":0,\"byteLength\":%u}", (u32)uvs_at);
    if (source.has_uvs)
    {
        append_json(json, ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}", (u32)uvs_at, (u32)(index_at - uvs_at));
    }
    append_json(json, ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}", (u32)index_at, (u32)(images_at - index_at));
    usize at = images_at;
    for (const W_ModelImage& image : source.images)
    {
        append_json(json, ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}", (u32)at, image.size);
        at = (at + image.size + 3) & ~(usize)3;
    }
    const usize bin_size = at;
    append_json(json, "],\"buffers\":[{\"byteLength\":%u}]}", (u32)bin_size);
    while (json.length % 4 != 0)
    {
        json.add(' ');
    }

    out.clear();
    out.reserve(28 + json.length + bin_size);
    put_le32(out, W_GLB_MAGIC);
    put_le32(out, 2);
    put_le32(out, (u32)(28 + json.length + bin_size));
    put_le32(out, (u32)json.length);
    put_le32(out, W_GLB_JSON);
    out.add((const u8*)json.data, json.length);
    put_le32(out, (u32)bin_size);
    put_le32(out, W_GLB_BIN);
    for (const W_SourcePart& part : source.parts)
    {
        out.add((const u8*)(source.positions.data + part.vertices_first), (usize)part.vertices_count * sizeof(Vec3));
    }
    for (usize p = 0; source.has_uvs && p < source.parts.length; p++)
    {
        out.add((const u8*)(source.uvs.data + source.parts[p].vertices_first), (usize)source.parts[p].vertices_count * sizeof(Vec2));
    }
    // Parts are in index order, so the indices stay in place, local to their part's vertices
    for (const W_SourcePart& part : source.parts)
    {
        for (u32 i = part.indices_first; i < part.indices_first + part.indices_count; i++)
        {
            const u32 local = source.indices[i] - part.vertices_first;
            out.add((const u8*)&local, sizeof(local));
        }
    }
    for (const W_ModelImage& image : source.images)
    {
        out.add(source.image_bytes.data + image.offset, image.size);
        while ((out.length - 28 - json.length) % 4 != 0)
        {
            out.add(0);
        }
    }
}

static f32 srgb_to_linear (f32 c)
{
    return c <= 0.04045f ? c / 12.92f : zm::pow((c + 0.055f) / 1.055f, 2.4f);
}

static u8 linear_to_srgb (f32 c)
{
    c             = zm::clamp(c, 0.0f, 1.0f);
    const f32 s   = c <= 0.0031308f ? c * 12.92f : 1.055f * zm::pow(c, 1.0f / 2.4f) - 0.055f;
    return (u8)zm::round(zm::clamp(s, 0.0f, 1.0f) * 255.0f);
}

static void destroy_model_data (W_Model* model)
{
    model->vertices.release();
    model->positions.release();
    model->indices.release();
    model->parts.release();
    for (const W_ModelTexture& image : model->images)
    {
        os_memory_release(image.rgba);
    }
    model->images.release();
    model->bvh.nodes.release();
    model->bvh.triangles.release();
}

#define W_SRGB_ENCODE_STEPS 4096
#define W_TRACE_INSTANCES   64 // instances w_trace_depth sees; props are at most SIM_MAX_PROPS

// The base colour of every material as one image: the texture times the factor in linear light,
// or a solid of the factor
static b8 make_model_texture (const W_ModelSource& source, const W_ModelMaterial& material, const f32* decode, const u8* encode, W_ModelTexture& out)
{
    if (material.image == W_NONE)
    {
        out = { .rgba = (u8*)os_memory_alloc((usize)W_MODEL_SOLID_SIZE * W_MODEL_SOLID_SIZE * 4), .width = W_MODEL_SOLID_SIZE, .height = W_MODEL_SOLID_SIZE };
        const u8 texel[4] = { linear_to_srgb(material.factor[0]), linear_to_srgb(material.factor[1]), linear_to_srgb(material.factor[2]), (u8)zm::round(material.factor[3] * 255.0f) };
        for (u32 t = 0; t < W_MODEL_SOLID_SIZE * W_MODEL_SOLID_SIZE; t++)
        {
            memory_copy(out.rgba + (usize)t * 4, texel, 4);
        }
        return true;
    }
    const W_ModelImage& image    = source.images[material.image];
    int                 width    = 0;
    int                 height   = 0;
    int                 channels = 0;
    u8*                 pixels   = stbi_load_from_memory(source.image_bytes.data + image.offset, (int)image.size, &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0 || width > W_MODEL_MAX_IMAGE_SIZE || height > W_MODEL_MAX_IMAGE_SIZE)
    {
        LOG_ERROR("Model: a texture cannot be decoded: %s", pixels ? "too large" : stbi_failure_reason());
        stbi_image_free(pixels);
        return false;
    }
    const usize texels = (usize)width * (usize)height;
    out                = { .rgba = (u8*)os_memory_alloc(texels * 4), .width = (u32)width, .height = (u32)height };
    memory_copy(out.rgba, pixels, texels * 4);
    stbi_image_free(pixels);
    const f32* f = material.factor;
    if (f[0] == 1.0f && f[1] == 1.0f && f[2] == 1.0f && f[3] == 1.0f)
    {
        return true;
    }
    for (usize t = 0; t < texels; t++)
    {
        u8* texel = out.rgba + t * 4;
        for (u32 c = 0; c < 3; c++)
        {
            texel[c] = encode[(u32)zm::round(decode[texel[c]] * f[c] * (f32)(W_SRGB_ENCODE_STEPS - 1))];
        }
        texel[3] = (u8)zm::round((f32)texel[3] * f[3]);
    }
    return true;
}

static W_Model* model_from_source (const W_ModelSource& source)
{
    W_Model* model   = (W_Model*)os_memory_alloc(sizeof(W_Model));
    b8       success = false;
    defer {
        if (!success)
        {
            destroy_model_data(model);
            os_memory_release(model);
        }
    };
    const usize vertices = source.positions.length;
    model->vertices.resize(vertices);
    model->positions.resize(vertices);
    model->min = source.positions[0];
    model->max = source.positions[0];
    for (usize v = 0; v < vertices; v++)
    {
        model->vertices[v]  = { source.positions[v], source.uvs[v] };
        model->positions[v] = source.positions[v];
        model->min          = zm::min(model->min, source.positions[v]);
        model->max          = zm::max(model->max, source.positions[v]);
    }
    model->indices.add(source.indices.data, source.indices.length);

    f32* decode = (f32*)os_memory_alloc(256 * sizeof(f32));
    u8*  encode = (u8*)os_memory_alloc(W_SRGB_ENCODE_STEPS);
    defer {
        os_memory_release(decode);
        os_memory_release(encode);
    };
    for (u32 i = 0; i < 256; i++)
    {
        decode[i] = srgb_to_linear((f32)i / 255.0f);
    }
    for (u32 i = 0; i < W_SRGB_ENCODE_STEPS; i++)
    {
        encode[i] = linear_to_srgb((f32)i / (f32)(W_SRGB_ENCODE_STEPS - 1));
    }
    for (const W_ModelMaterial& material : source.materials)
    {
        W_ModelTexture texture = {};
        if (!make_model_texture(source, material, decode, encode, texture))
        {
            return nullptr;
        }
        model->images.add(texture);
    }
    for (const W_SourcePart& part : source.parts)
    {
        model->parts.add({ .indices_first = part.indices_first, .indices_count = part.indices_count, .image = part.material });
    }
    build_bvh(model->bvh, model->positions.data, model->indices.data, (u32)(model->indices.length / 3));
    success = true;
    return model;
}

// The ray of a room space origin and direction in an instance's model space; false for an
// instance that has no extent, whose model space has no inverse
static b8 ray_into_instance (const W_Instance& instance, Vec3 origin, Vec3 direction, Vec3& out_origin, Vec3& out_direction)
{
    const Vec3 s = instance.scale;
    if (!instance.model || !(s.x > 0.0f) || !(s.y > 0.0f) || !(s.z > 0.0f) || !zm::is_finite(s.x) || !zm::is_finite(s.y) || !zm::is_finite(s.z))
    {
        return false;
    }
    const Quat inverse = zm::conjugate(instance.pose.orientation);
    const Vec3 o       = zm::rotate(inverse, origin - instance.pose.position);
    const Vec3 d       = zm::rotate(inverse, direction);
    out_origin         = { o.x / s.x, o.y / s.y, o.z / s.z };
    out_direction      = { d.x / s.x, d.y / s.y, d.z / s.z };
    return true;
}

// The instances that have an extent, prepared for rays from `origin`; returns how many
static u32 prepare_instances (const W_Instance* instances, u32 count, Vec3 origin, W_TraceInstance* out)
{
    u32 prepared = 0;
    for (u32 i = 0; instances && i < count && prepared < W_TRACE_INSTANCES; i++)
    {
        const W_Instance& instance = instances[i];
        Vec3              o        = {};
        Vec3              unused   = {};
        if (!ray_into_instance(instance, origin, { 0.0f, 0.0f, -1.0f }, o, unused) || !zm::is_finite(o.x) || !zm::is_finite(o.y) || !zm::is_finite(o.z))
        {
            continue;
        }
        const Vec3 s        = instance.scale;
        const Vec3 half     = (instance.model->max - instance.model->min) * 0.5f;
        const Vec3 middle   = (instance.model->max + instance.model->min) * 0.5f;
        const Vec3 center   = instance.pose.position + zm::rotate(instance.pose.orientation, { middle.x * s.x, middle.y * s.y, middle.z * s.z });
        const f32  radius   = zm::length(Vec3{ half.x * s.x, half.y * s.y, half.z * s.z });
        out[prepared++]     = {
            .model     = instance.model,
            .inverse   = zm::conjugate(instance.pose.orientation),
            .inv_scale = { 1.0f / s.x, 1.0f / s.y, 1.0f / s.z },
            .origin    = o,
            .to_center = center - origin,
            .radius    = radius * 1.001f + 1e-5f,
        };
    }
    return prepared;
}

// The nearest hit of prepared instances within (0, inout_best] in units of `direction`; false for none
static b8 trace_instances (const W_TraceInstance* instances, u32 count, Vec3 direction, f32& inout_best)
{
    const f32 dd     = zm::dot(direction, direction);
    const f32 length = zm::sqrt(dd);
    b8        hit    = false;
    for (u32 i = 0; i < count; i++)
    {
        const W_TraceInstance& instance = instances[i];
        const f32              along    = zm::dot(instance.to_center, direction) / dd;
        const f32              reach    = instance.radius / length;
        const f32              off      = zm::dot(instance.to_center, instance.to_center) - along * along * dd;
        if (off > instance.radius * instance.radius || along + reach < 0.0f || along - reach > inout_best)
        {
            continue;
        }
        const Vec3 d = zm::rotate(instance.inverse, direction);
        hit |= nearest_hit(instance.model->bvh, instance.origin, { d.x * instance.inv_scale.x, d.y * instance.inv_scale.y, d.z * instance.inv_scale.z }, inout_best) != W_NONE;
    }
    return hit;
}

// Nearest instance hit within (0, inout_best] in units of `direction`; W_NONE for none
static u32 nearest_instance (const W_Instance* instances, u32 count, Vec3 origin, Vec3 direction, f32& inout_best, u32& out_triangle)
{
    u32 best = W_NONE;
    for (u32 i = 0; i < count; i++)
    {
        Vec3 o = {};
        Vec3 d = {};
        if (!ray_into_instance(instances[i], origin, direction, o, d))
        {
            continue;
        }
        const u32 triangle = nearest_hit(instances[i].model->bvh, o, d, inout_best);
        if (triangle != W_NONE)
        {
            best         = i;
            out_triangle = triangle;
        }
    }
    return best;
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

    const u32 visual_count = (u32)world->visual_vertices.length;
    world->visual_positions.resize(visual_count);
    for (u32 i = 0; i < visual_count; i++)
    {
        world->visual_positions[i] = world->visual_vertices[i].position;
    }
    build_bvh(world->mesh_bvh, world->mesh_positions.data, world->mesh_indices.data, (u32)(world->mesh_indices.length / 3));
    build_bvh(world->visual_bvh, world->visual_positions.data, world->visual_indices.data, (u32)(world->visual_indices.length / 3));
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
    world->visual_positions.release();
    world->mesh_bvh.nodes.release();
    world->mesh_bvh.triangles.release();
    world->visual_bvh.nodes.release();
    world->visual_bvh.triangles.release();
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
    if (!zm::is_finite(length) || length < zm::EPSILON || !(max_distance > 0.0f) || !zm::is_finite(origin.x) || !zm::is_finite(origin.y) || !zm::is_finite(origin.z) || world->mesh_bvh.nodes.length == 0)
    {
        return false;
    }
    if (!zm::is_finite(max_distance))
    {
        max_distance = zm::F32_MAX;
    }

    const Vec3 d             = direction / length;
    f32        best          = max_distance;
    const u32  best_triangle = nearest_hit(world->mesh_bvh, origin, d, best);
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

void w_trace_depth (const W_World* world, const W_Instance* instances, u32 instances_count, const Pose& view, const Fov& fov, f32 near, u32 width, u32 height, u32 row_first, u32 row_count, u16* out_image)
{
    ASSERT(out_image && near > 0.0f);
    const u32 row_end = (u32)zm::min((u64)row_first + row_count, (u64)height);
    const f32 tl      = zm::tan(fov.angle_left);
    const f32 tr      = zm::tan(fov.angle_right);
    const f32 tu      = zm::tan(fov.angle_up);
    const f32 td      = zm::tan(fov.angle_down);
    const b8  valid   = zm::is_finite(view.position.x) && zm::is_finite(view.position.y) && zm::is_finite(view.position.z) && zm::is_finite(zm::length(view.orientation)) && zm::is_finite(tl) && zm::is_finite(tr) &&
                     zm::is_finite(tu) && zm::is_finite(td) && near > 0.0f;
    W_TraceInstance prepared[W_TRACE_INSTANCES];
    const u32       prepared_count = valid ? prepare_instances(instances, instances_count, view.position, prepared) : 0;
    for (u32 y = row_first; y < row_end; y++)
    {
        u16*      row = out_image + (usize)y * width;
        const f32 ty  = td + (tu - td) * ((f32)y + 0.5f) / (f32)height;
        for (u32 x = 0; x < width; x++)
        {
            if (!valid)
            {
                row[x] = 0xffff;
                continue;
            }
            // Along (tx, ty, -1) in the view's frame the ray parameter is the distance along -Z
            const f32  tx        = tl + (tr - tl) * ((f32)x + 0.5f) / (f32)width;
            const Vec3 direction = zm::rotate(view.orientation, { tx, ty, -1.0f });
            f32        z         = zm::F32_MAX;
            const b8   room      = world && nearest_hit(world->visual_bvh, view.position, direction, z) != W_NONE;
            const b8   instance  = trace_instances(prepared, prepared_count, direction, z);
            if (!room && !instance)
            {
                row[x] = 0xffff;
                continue;
            }
            row[x] = z < near ? (u16)0 : (u16)zm::min(zm::round((1.0f - near / z) * 65535.0f), 65535.0f);
        }
    }
}

b8 w_raycast_instances (const W_Instance* instances, u32 instances_count, Vec3 origin, Vec3 direction, f32 max_distance, W_RaycastHit& out_hit, u32& out_index)
{
    const f32 length = zm::length(direction);
    if (!instances || instances_count == 0 || !zm::is_finite(length) || length < zm::EPSILON || !(max_distance > 0.0f) || !zm::is_finite(origin.x) || !zm::is_finite(origin.y) || !zm::is_finite(origin.z))
    {
        return false;
    }
    const Vec3 d        = direction / length;
    f32        best     = zm::is_finite(max_distance) ? max_distance : zm::F32_MAX;
    u32        triangle = W_NONE;
    const u32  index    = nearest_instance(instances, instances_count, origin, d, best, triangle);
    if (index == W_NONE)
    {
        return false;
    }
    // Normals go through the inverse transpose: the inverse scale, then the rotation
    const W_Instance& instance = instances[index];
    const W_Model*    model    = instance.model;
    const u32*        t        = model->indices.data + (usize)triangle * 3;
    const Vec3        a        = model->positions[t[0]];
    const Vec3        local    = zm::cross(model->positions[t[1]] - a, model->positions[t[2]] - a);
    Vec3              normal   = zm::normalize(zm::rotate(instance.pose.orientation, { local.x / instance.scale.x, local.y / instance.scale.y, local.z / instance.scale.z }));
    if (zm::dot(normal, d) > 0.0f)
    {
        normal = -normal;
    }
    out_hit   = { .distance = best, .point = origin + d * best, .normal = normal };
    out_index = index;
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

b8 w_pack_model (const u8* data, usize size, const char* dir, Array<u8>& out_glb)
{
    W_ModelSource source = {};
    defer { release_source(source); };
    if (!read_gltf(data, size, dir, source))
    {
        return false;
    }
    write_glb(source, out_glb);
    if (out_glb.length > W_MODEL_MAX_FILE_SIZE)
    {
        LOG_ERROR("Model: packed it is larger than %u bytes", W_MODEL_MAX_FILE_SIZE);
        return false;
    }
    return true;
}

b8 w_pack_model_file (const char* path, Array<u8>& out_glb)
{
    if (!path)
    {
        return false;
    }
    u8*   data = nullptr;
    usize size = 0;
    if (!os_read_file(path, &data, &size))
    {
        LOG_ERROR("Model: cannot read %s", path);
        return false;
    }
    defer { os_memory_release(data); };
    char        dir[OS_MAX_PATH];
    const char* slash = nullptr;
    for (const char* c = path; *c; c++)
    {
        slash = *c == '/' || *c == '\\' ? c : slash;
    }
    cstr_copy(dir, sizeof(dir), slash ? path : ".");
    if (slash && (usize)(slash - path) < sizeof(dir))
    {
        dir[slash - path] = 0;
    }
    if (!w_pack_model(data, size, dir, out_glb))
    {
        LOG_ERROR("Model: %s is refused (docs/props.md, Models)", path);
        return false;
    }
    return true;
}

W_Model* w_create_model (const u8* glb, usize size)
{
    W_ModelSource source = {};
    defer { release_source(source); };
    if (!read_gltf(glb, size, nullptr, source))
    {
        return nullptr;
    }
    return model_from_source(source);
}

W_Model* w_create_mesh_model (const Vec3* positions, u32 positions_count, const u32* indices, u32 indices_count)
{
    if (!positions || !indices || positions_count == 0 || indices_count == 0 || indices_count % 3 != 0 || !indices_valid(indices, indices_count, positions_count))
    {
        return nullptr;
    }
    for (u32 i = 0; i < positions_count; i++)
    {
        if (!coordinate_valid(positions[i].x) || !coordinate_valid(positions[i].y) || !coordinate_valid(positions[i].z))
        {
            return nullptr;
        }
    }
    W_ModelSource source = {};
    defer { release_source(source); };
    source.positions.add(positions, positions_count);
    source.uvs.resize(positions_count);
    source.indices.add(indices, indices_count);
    source.materials.add({ .factor = { 1.0f, 1.0f, 1.0f, 1.0f }, .image = W_NONE });
    source.parts.add({ .vertices_first = 0, .vertices_count = positions_count, .indices_first = 0, .indices_count = indices_count, .material = 0 });
    return model_from_source(source);
}

void w_destroy_model (W_Model* model)
{
    if (!model)
    {
        return;
    }
    destroy_model_data(model);
    os_memory_release(model);
}

void w_model_mesh (const W_Model* model, const W_VisualVertex*& out_vertices, u32& out_vertices_count, const u32*& out_indices, u32& out_indices_count)
{
    ASSERT(model);
    out_vertices       = model->vertices.data;
    out_vertices_count = (u32)model->vertices.length;
    out_indices        = model->indices.data;
    out_indices_count  = (u32)model->indices.length;
}

u32 w_model_parts (const W_Model* model, const W_ModelPart*& out_parts)
{
    ASSERT(model);
    out_parts = model->parts.data;
    return (u32)model->parts.length;
}

u32 w_model_image_count (const W_Model* model)
{
    ASSERT(model);
    return (u32)model->images.length;
}

void w_model_image (const W_Model* model, u32 index, const u8*& out_rgba, u32& out_width, u32& out_height)
{
    ASSERT(model && index < model->images.length);
    const W_ModelTexture& image = model->images[index];
    out_rgba                    = image.rgba;
    out_width                   = image.width;
    out_height                  = image.height;
}

void w_model_bounds (const W_Model* model, Vec3& out_min, Vec3& out_max)
{
    ASSERT(model);
    out_min = model->min;
    out_max = model->max;
}

b8 w_find_embedded_model (const char* name, const u8*& out_data, usize& out_size)
{
    for (u32 i = 0; name && i < w_embedded_models_count; i++)
    {
        if (w_embedded_models[i].name && cstr_equal(w_embedded_models[i].name, name))
        {
            out_data = w_embedded_models[i].data;
            out_size = w_embedded_models[i].size;
            return true;
        }
    }
    return false;
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
