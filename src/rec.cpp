#include "rec.h"

//
// Types
//

#define REC_MAGIC             "PUPPETXR"
#define REC_MAGIC_SIZE        8
#define REC_WRITE_BUFFER      ((usize)64 * 1024)
#define REC_MAX_HEADER_SIZE   (64 * 1024)
#define REC_MAX_CHUNK_SIZE    (16 * 1024 * 1024)
#define REC_SCRATCH_SIZE      12288 // a frame with every prop, a full typed text and the body fits
#define REC_MAX_DT            10000000000ll
#define REC_MAX_START_TIME    (1ll << 62)
#define REC_MAX_NAME          63
#define REC_MAX_TABLE         255
#define REC_MAX_MASK_BYTES    32
#define REC_NONE              0xFF

static_assert((u32)SIM_Input::Count <= REC_MAX_MASK_BYTES * 8 && (u32)SIM_Input::Count < REC_NONE, "inputs no longer fit the recording mask");
static_assert((u32)SIM_Device::Count < REC_NONE, "devices no longer fit the recording table");

static const u8 REC_TAG_FRAME[4] = { 'F', 'R', 'A', 'M' };
static const u8 REC_TAG_END[4]   = { 'E', 'N', 'D', '_' };
static const u8 REC_TAG_STORE[4] = { 'S', 'T', 'O', 'R' };
static const u8 REC_TAG_EXTERNAL[4] = { 'E', 'X', 'T', 'N' };

struct REC_Writer
{
    OS_File* file;
    u8       buffer[REC_WRITE_BUFFER];
    usize    used;
    u64      frames;
    b8       failed;
    SIM_Prop props[SIM_MAX_PROPS]; // as last written; frames repeat them only when they change
    u32      props_count;
    SIM_BodyState body;            // as last written, the same way
    SIM_FaceState face;
    Pose          tracking_drift;
};

namespace
{

// How a file's device and input tables map to this build: entry i of the file's table maps
// to the SIM_Device or SIM_Input index here, or REC_NONE when this build does not know it
struct REC_Layout
{
    u32 devices_count;
    u8  devices[REC_MAX_TABLE];
    u32 inputs_count;
    u8  inputs[REC_MAX_TABLE];
    u32 unknown_devices;
    u32 unknown_inputs;
};

} // namespace

struct REC_Recording
{
    u8*        data;
    usize      size;
    REC_Header header;
    REC_Layout layout;
    Array<u64> frame_offsets; // offset of each FRAM payload in data
    Array<u64> external_offsets; // offset of each EXTN payload in data
    W_Store    store;
    b8         complete;
};

namespace
{

// Little-endian encoder into a bounded buffer; overflow is sticky
struct REC_Out
{
    u8*   data;
    usize capacity;
    usize size;
    b8    overflow;
};

// Bounds-checked little-endian decoder; any read past the end fails the whole decode
struct REC_In
{
    const u8* data;
    usize     size;
    usize     pos;
    b8        failed;
};

} // namespace

//
// Statics
//

static void put_bytes (REC_Out& out, const void* data, usize size)
{
    if (out.overflow || out.size + size > out.capacity)
    {
        out.overflow = true;
        return;
    }
    memory_copy(out.data + out.size, data, size);
    out.size += size;
}

static void put_u8 (REC_Out& out, u8 value)
{
    put_bytes(out, &value, 1);
}

static void put_u16 (REC_Out& out, u16 value)
{
    const u8 bytes[2] = { (u8)value, (u8)(value >> 8) };
    put_bytes(out, bytes, 2);
}

static void put_u32 (REC_Out& out, u32 value)
{
    const u8 bytes[4] = { (u8)value, (u8)(value >> 8), (u8)(value >> 16), (u8)(value >> 24) };
    put_bytes(out, bytes, 4);
}

static void put_u64 (REC_Out& out, u64 value)
{
    put_u32(out, (u32)value);
    put_u32(out, (u32)(value >> 32));
}

static void put_f32 (REC_Out& out, f32 value)
{
    u32 bits = 0;
    memory_copy(&bits, &value, 4);
    put_u32(out, bits);
}

static void put_pose (REC_Out& out, const Pose& pose)
{
    put_f32(out, pose.position.x);
    put_f32(out, pose.position.y);
    put_f32(out, pose.position.z);
    put_f32(out, pose.orientation.x);
    put_f32(out, pose.orientation.y);
    put_f32(out, pose.orientation.z);
    put_f32(out, pose.orientation.w);
}

static const u8* get_bytes (REC_In& in, usize size)
{
    if (in.failed || size > in.size - in.pos)
    {
        in.failed = true;
        return nullptr;
    }
    const u8* bytes = in.data + in.pos;
    in.pos += size;
    return bytes;
}

static u8 get_u8 (REC_In& in)
{
    const u8* b = get_bytes(in, 1);
    return b ? b[0] : 0;
}

static u16 get_u16 (REC_In& in)
{
    const u8* b = get_bytes(in, 2);
    return b ? (u16)(b[0] | (b[1] << 8)) : 0;
}

static u32 get_u32 (REC_In& in)
{
    const u8* b = get_bytes(in, 4);
    return b ? (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24) : 0;
}

static u64 get_u64 (REC_In& in)
{
    const u64 low  = get_u32(in);
    const u64 high = get_u32(in);
    return low | (high << 32);
}

static f32 get_f32 (REC_In& in)
{
    const u32 bits = get_u32(in);
    f32       value = 0.0f;
    memory_copy(&value, &bits, 4);
    return value;
}

static Pose get_pose (REC_In& in)
{
    Pose pose;
    pose.position.x    = get_f32(in);
    pose.position.y    = get_f32(in);
    pose.position.z    = get_f32(in);
    pose.orientation.x = get_f32(in);
    pose.orientation.y = get_f32(in);
    pose.orientation.z = get_f32(in);
    pose.orientation.w = get_f32(in);
    return pose;
}

static b8 pose_valid (const Pose& pose)
{
    const f32 length = zm::length(pose.orientation);
    return zm::is_finite(pose) && length > 0.5f && length < 2.0f;
}

static void put_name (REC_Out& out, const char* name)
{
    const usize length = cstr_length(name);
    ASSERT(length > 0 && length <= REC_MAX_NAME);
    put_u8(out, (u8)length);
    put_bytes(out, name, length);
}

static b8 get_name (REC_In& in, char (&out)[REC_MAX_NAME + 1])
{
    const u8  length = get_u8(in);
    const u8* bytes  = length >= 1 && length <= REC_MAX_NAME ? get_bytes(in, length) : nullptr;
    if (!bytes)
    {
        return false;
    }
    for (u32 i = 0; i < length; i++)
    {
        const char c  = (char)bytes[i];
        const b8   ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '.' || c == '-';
        if (!ok)
        {
            return false;
        }
        out[i] = c;
    }
    out[length] = 0;
    return true;
}

// A room name that may be empty: u8 length, then the bytes
static void put_room (REC_Out& out, const char* room)
{
    const usize length = cstr_length(room);
    put_u8(out, (u8)length);
    put_bytes(out, room, length);
}

static b8 get_room (REC_In& in, char (&out)[W_MAX_NAME])
{
    const u8  length = get_u8(in);
    const u8* bytes  = length < W_MAX_NAME ? get_bytes(in, length) : nullptr;
    if (!bytes && length > 0)
    {
        return false;
    }
    memory_copy(out, bytes, length);
    out[length] = 0;
    return length == 0 || w_room_name_valid(out);
}

static void put_anchor (REC_Out& out, const W_StoredAnchor& anchor)
{
    put_bytes(out, anchor.uuid.bytes, W_UUID_SIZE);
    put_room(out, anchor.room);
    put_u64(out, anchor.room_hash);
    put_pose(out, anchor.pose);
}

static b8 get_anchor (REC_In& in, W_StoredAnchor& out)
{
    out             = {};
    const u8* bytes = get_bytes(in, W_UUID_SIZE);
    if (!bytes || !get_room(in, out.room))
    {
        return false;
    }
    memory_copy(out.uuid.bytes, bytes, W_UUID_SIZE);
    out.room_hash = get_u64(in);
    out.pose      = get_pose(in);
    return !in.failed && pose_valid(out.pose);
}

static usize anchor_size (const W_StoredAnchor& anchor)
{
    return W_UUID_SIZE + 1 + cstr_length(anchor.room) + 8 + (usize)7 * 4;
}

static void put_vec3 (REC_Out& out, Vec3 v)
{
    put_f32(out, v.x);
    put_f32(out, v.y);
    put_f32(out, v.z);
}

static void put_prop (REC_Out& out, const SIM_Prop& prop)
{
    put_u32(out, prop.id);
    put_name(out, sim_prop_kind_name(prop.kind));
    put_name(out, sim_prop_motion_name(prop.motion));
    put_pose(out, prop.pose);
    put_vec3(out, prop.target);
    put_f32(out, prop.radius);
    put_f32(out, prop.speed);
    put_f32(out, prop.size);
    put_u64(out, (u64)prop.start);
    const usize length = cstr_length(prop.text);
    put_u8(out, (u8)length);
    put_bytes(out, prop.text, length);
}

// State: per device of the layout an active flag, the hand source, the pose of the put down
// controller while the source is a tracked hand, a pose and a sparse input mask; then the typed
// text, the props when `with_props`, the body when `with_body`, the face when `with_face` and the
// tracking drift when `with_drift`.
static void put_state (REC_Out& out, const SIM_FrameState& state, b8 with_props, b8 with_body, b8 with_face, b8 with_drift)
{
    put_u64(out, (u64)state.dt);
    put_pose(out, state.local_origin);
    for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
    {
        const SIM_DeviceFrame& device = state.devices[d];
        put_u8(out, device.active ? 1 : 0);
        put_u8(out, (u8)device.source);
        if (device.source == SIM_HandSource::Hand)
        {
            put_pose(out, device.detached);
        }
        put_pose(out, device.pose);

        u8  mask[REC_MAX_MASK_BYTES] = {};
        u32 mask_bytes               = 0;
        for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
        {
            if (device.inputs[i].value.x != 0.0f || device.inputs[i].value.y != 0.0f)
            {
                mask[i / 8] |= (u8)(1u << (i % 8));
                mask_bytes   = i / 8 + 1;
            }
        }
        put_u8(out, (u8)mask_bytes);
        put_bytes(out, mask, mask_bytes);
        for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
        {
            if (mask[i / 8] & (1u << (i % 8)))
            {
                put_f32(out, device.inputs[i].value.x);
                put_f32(out, device.inputs[i].value.y);
            }
        }
    }
    put_u16(out, (u16)state.typed_length);
    put_bytes(out, (const u8*)state.typed, state.typed_length);
    put_u8(out, with_props ? 1 : 0);
    if (with_props)
    {
        put_u8(out, (u8)state.props_count);
        for (u32 i = 0; i < state.props_count; i++)
        {
            put_prop(out, state.props[i]);
        }
    }
    put_u8(out, with_body ? 1 : 0);
    if (with_body)
    {
        const SIM_BodyState& body = state.body;
        put_f32(out, body.height);
        put_u8(out, (u8)SIM_BODY_JOINT_COUNT);
        for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
        {
            put_f32(out, body.bone_scales[j]);
            put_f32(out, body.rotations[j].x);
            put_f32(out, body.rotations[j].y);
            put_f32(out, body.rotations[j].z);
            put_f32(out, body.rotations[j].w);
            put_f32(out, body.confidences[j]);
            put_u8(out, body.tracked[j] ? 1 : 0);
        }
        put_u8(out, (u8)SIM_Limb::Count);
        for (u32 l = 0; l < (u32)SIM_Limb::Count; l++)
        {
            put_u8(out, (u8)body.limbs[l]);
        }
    }
    put_u8(out, with_face ? 1 : 0);
    if (with_face)
    {
        const SIM_FaceState& face = state.face;
        put_u8(out, (u8)SIM_FACE_EXPRESSION_COUNT);
        for (const f32 weight : face.weights)
        {
            put_f32(out, weight);
        }
        put_u8(out, (u8)SIM_FACE_VISEME_COUNT);
        for (const f32 viseme : face.visemes)
        {
            put_f32(out, viseme);
        }
        put_f32(out, face.confidences[0]);
        put_f32(out, face.confidences[1]);
        put_u8(out, face.blink ? 1 : 0);
        put_f32(out, face.gaze_yaw);
        put_f32(out, face.gaze_pitch);
        put_u8(out, face.look_at ? 1 : 0);
        put_vec3(out, face.target);
    }
    put_u8(out, with_drift ? 1 : 0);
    if (with_drift)
    {
        put_pose(out, state.tracking_drift);
    }
}

static Vec3 get_vec3 (REC_In& in)
{
    Vec3 v = {};
    v.x    = get_f32(in);
    v.y    = get_f32(in);
    v.z    = get_f32(in);
    return v;
}

// Kind and motion by name; values as sim_add_prop would take them, a text of any non-zero bytes
static b8 get_prop (REC_In& in, SIM_Prop& out)
{
    char kind[REC_MAX_NAME + 1]   = {};
    char motion[REC_MAX_NAME + 1] = {};
    out    = {};
    out.id = get_u32(in);
    if (!get_name(in, kind) || !get_name(in, motion) || !sim_prop_kind_from_name(kind, out.kind) || !sim_prop_motion_from_name(motion, out.motion))
    {
        return false;
    }
    out.pose          = get_pose(in);
    out.target        = get_vec3(in);
    out.radius        = get_f32(in);
    out.speed         = get_f32(in);
    out.size          = get_f32(in);
    out.start         = (i64)get_u64(in);
    const u8  length  = get_u8(in);
    const u8* text    = length < SIM_MAX_PROP_TEXT ? get_bytes(in, length) : nullptr;
    if ((!text && length > 0) || out.id == 0 || !pose_valid(out.pose))
    {
        return false;
    }
    for (u32 i = 0; i < length; i++)
    {
        if (text[i] == 0)
        {
            return false;
        }
    }
    memory_copy(out.text, text, length);
    const f32 values[] = { out.target.x, out.target.y, out.target.z, out.radius, out.speed, out.size };
    for (const f32 value : values)
    {
        if (!zm::is_finite(value))
        {
            return false;
        }
    }
    return !in.failed;
}

// Joints past the reader's count keep the default; a value the setters would not produce rejects
// the frame
static b8 get_body (REC_In& in, SIM_BodyState& out)
{
    const f32 height = get_f32(in);
    sim_default_body(height, out);
    const u8 joints = get_u8(in);
    if (!zm::is_finite(height) || height < SIM_BODY_MIN_HEIGHT || height > SIM_BODY_MAX_HEIGHT || joints > SIM_BODY_JOINT_COUNT)
    {
        return false;
    }
    for (u32 j = 0; j < joints; j++)
    {
        const f32  scale      = get_f32(in);
        const Quat rotation   = { get_f32(in), get_f32(in), get_f32(in), get_f32(in) };
        const f32  confidence = get_f32(in);
        const u8   tracked    = get_u8(in);
        const b8   unit       = zm::is_finite(zm::length(rotation)) && zm::near(zm::length(rotation), 1.0f, 1e-3f);
        if (!zm::is_finite(scale) || scale < SIM_BODY_MIN_SCALE || scale > SIM_BODY_MAX_SCALE || (scale != 1.0f && !sim_body_joint_scalable((SIM_BodyJoint)j)) || !unit || !zm::is_finite(confidence) || confidence < 0.0f || confidence > 1.0f || tracked > 1)
        {
            return false;
        }
        out.bone_scales[j] = scale;
        out.rotations[j]   = rotation;
        out.confidences[j] = confidence;
        out.tracked[j]     = tracked == 1;
    }
    const u8 limbs = get_u8(in);
    if (limbs > (u8)SIM_Limb::Count)
    {
        return false;
    }
    for (u32 l = 0; l < limbs; l++)
    {
        const u8 mode = get_u8(in);
        if (mode >= (u8)SIM_LimbMode::Count)
        {
            return false;
        }
        out.limbs[l] = (SIM_LimbMode)mode;
    }
    return !in.failed;
}

static b8 unit (f32 value)
{
    return zm::is_finite(value) && value >= 0.0f && value <= 1.0f;
}

// Expressions and visemes past the reader's counts are skipped, missing ones read zero; a value
// sim_set_face would not keep rejects the frame
static b8 get_face (REC_In& in, SIM_FaceState& out)
{
    sim_default_face(out);
    const u8 expressions = get_u8(in);
    for (u32 e = 0; e < expressions; e++)
    {
        const f32 weight = get_f32(in);
        if (!unit(weight))
        {
            return false;
        }
        if (e < SIM_FACE_EXPRESSION_COUNT)
        {
            out.weights[e] = weight;
        }
    }
    const u8 visemes = get_u8(in);
    for (u32 v = 0; v < visemes; v++)
    {
        const f32 viseme = get_f32(in);
        if (!unit(viseme))
        {
            return false;
        }
        if (v < SIM_FACE_VISEME_COUNT)
        {
            out.visemes[v] = viseme;
        }
    }
    out.confidences[0]  = get_f32(in);
    out.confidences[1]  = get_f32(in);
    const u8 blink      = get_u8(in);
    out.gaze_yaw        = get_f32(in);
    out.gaze_pitch      = get_f32(in);
    const u8 look_at    = get_u8(in);
    out.target          = get_vec3(in);
    out.blink           = blink == 1;
    out.look_at         = look_at == 1;
    const b8 gaze       = zm::is_finite(out.gaze_yaw) && zm::abs(out.gaze_yaw) <= SIM_FACE_MAX_GAZE_YAW && zm::is_finite(out.gaze_pitch) && zm::abs(out.gaze_pitch) <= SIM_FACE_MAX_GAZE_PITCH;
    const b8 target     = zm::is_finite(out.target.x) && zm::is_finite(out.target.y) && zm::is_finite(out.target.z);
    return !in.failed && unit(out.confidences[0]) && unit(out.confidences[1]) && blink <= 1 && look_at <= 1 && gaze && target;
}

static b8 get_state (REC_In& in, const REC_Layout& layout, SIM_FrameState& out)
{
    out    = {};
    out.dt = (i64)get_u64(in);
    if (out.dt < 0 || out.dt > REC_MAX_DT)
    {
        return false;
    }
    out.local_origin = get_pose(in);
    if (!pose_valid(out.local_origin))
    {
        return false;
    }
    for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
    {
        out.devices[d].pose = zm::POSE_IDENTITY;
    }

    for (u32 d = 0; d < layout.devices_count; d++)
    {
        SIM_DeviceFrame  discard = {};
        const b8         known   = layout.devices[d] != REC_NONE;
        SIM_DeviceFrame& device  = known ? out.devices[layout.devices[d]] : discard;
        const u8         active  = get_u8(in);
        const u8         source  = get_u8(in);
        device.active            = active == 1;
        device.source            = (SIM_HandSource)source;
        if (source == (u8)SIM_HandSource::Hand)
        {
            device.detached = get_pose(in);
            if (!pose_valid(device.detached))
            {
                return false;
            }
        }
        device.pose = get_pose(in);
        if (active > 1 || source >= (u8)SIM_HandSource::Count || !pose_valid(device.pose))
        {
            return false;
        }

        const u8  mask_bytes = get_u8(in);
        const u8* mask       = mask_bytes <= REC_MAX_MASK_BYTES ? get_bytes(in, mask_bytes) : nullptr;
        if (!mask && mask_bytes > 0)
        {
            return false;
        }
        for (u32 i = 0; i < (u32)mask_bytes * 8; i++)
        {
            if (!(mask[i / 8] & (1u << (i % 8))))
            {
                continue;
            }
            if (i >= layout.inputs_count)
            {
                return false;
            }
            const Vec2 value = { get_f32(in), get_f32(in) };
            if (!zm::is_finite(value.x) || !zm::is_finite(value.y))
            {
                return false;
            }
            if (known && layout.inputs[i] != REC_NONE)
            {
                device.inputs[layout.inputs[i]].value = value;
            }
        }
    }
    out.typed_length = get_u16(in);
    const u8* typed  = out.typed_length <= SIM_MAX_TYPED ? get_bytes(in, out.typed_length) : nullptr;
    if (!typed && out.typed_length > 0)
    {
        return false;
    }
    for (u32 i = 0; i < out.typed_length; i++)
    {
        if ((typed[i] < 0x20 && typed[i] != '\b' && typed[i] != '\n') || typed[i] == 0x7F)
        {
            return false;
        }
    }
    memory_copy(out.typed, typed, out.typed_length);
    const u8 props    = get_u8(in);
    out.props_present = props == 1;
    out.props_count   = out.props_present ? get_u8(in) : 0;
    if (props > 1 || out.props_count > SIM_MAX_PROPS)
    {
        return false;
    }
    for (u32 i = 0; i < out.props_count; i++)
    {
        if (!get_prop(in, out.props[i]))
        {
            return false;
        }
    }
    const u8 body    = get_u8(in);
    out.body_present = body == 1;
    if (body > 1 || (out.body_present && !get_body(in, out.body)))
    {
        return false;
    }
    const u8 face    = get_u8(in);
    out.face_present = face == 1;
    if (face > 1 || (out.face_present && !get_face(in, out.face)))
    {
        return false;
    }
    const u8 drift     = get_u8(in);
    out.drift_present  = drift == 1;
    out.tracking_drift = zm::POSE_IDENTITY;
    if (drift > 1)
    {
        return false;
    }
    if (out.drift_present)
    {
        out.tracking_drift = get_pose(in);
        if (!sim_tracking_drift_valid(out.tracking_drift))
        {
            return false;
        }
    }
    return !in.failed;
}

// After the palm offset the room: u8 present, then a Name and a u64 content hash. After the
// initial state the stage pose in the room, anchor timing, pending anchor faults, the hand
// tracking cone and pending frame faults.
static void put_header (REC_Out& out, const REC_Header& header)
{
    put_u32(out, header.writer_version);
    put_name(out, sim_controller_name(header.controller));
    put_u32(out, header.eye_width);
    put_u32(out, header.eye_height);
    put_f32(out, header.refresh_rate_hz);
    put_u64(out, (u64)header.display_period_ns);
    put_f32(out, header.fov_degrees);
    put_f32(out, header.ipd);
    put_f32(out, header.stage_bounds.x);
    put_f32(out, header.stage_bounds.y);
    put_pose(out, header.aim_offset);
    put_pose(out, header.palm_offset);
    put_u8(out, header.room[0] ? 1 : 0);
    if (header.room[0])
    {
        put_name(out, header.room);
        put_u64(out, header.room_hash);
    }

    put_u8(out, (u8)SIM_Device::Count);
    for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
    {
        put_name(out, sim_device_name((SIM_Device)d));
    }
    put_u8(out, (u8)SIM_Input::Count);
    for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
    {
        put_name(out, sim_input_name((SIM_Input)i));
    }

    put_u64(out, (u64)header.initial.time);
    put_u64(out, header.initial.frame_index);
    for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
    {
        put_pose(out, header.initial.previous_poses[d]);
    }
    put_state(out, header.initial.state, true, true, true, true);

    put_pose(out, header.room_from_stage);
    put_u32(out, header.anchor_latency_frames);
    put_u32(out, header.anchor_localize_frames);
    put_u8(out, REC_ANCHOR_OPS);
    for (const REC_Fault& fault : header.anchor_faults)
    {
        put_u32(out, (u32)fault.result);
        put_u32(out, fault.remaining);
    }
    put_f32(out, header.hand_tracking_fov_degrees);
    put_u8(out, REC_FRAME_OPS);
    for (const REC_Fault& fault : header.frame_faults)
    {
        put_u32(out, (u32)fault.result);
        put_u32(out, fault.remaining);
    }
}

static b8 get_table (REC_In& in, b8 devices, REC_Layout& layout)
{
    const u32 count = get_u8(in);
    if (in.failed || count == 0)
    {
        return false;
    }
    b8 seen[REC_MAX_TABLE] = {};
    for (u32 i = 0; i < count; i++)
    {
        char name[REC_MAX_NAME + 1];
        if (!get_name(in, name))
        {
            return false;
        }
        u32 index = REC_NONE;
        if (devices)
        {
            SIM_Device device = {};
            index = sim_device_from_name(name, device) ? (u32)device : REC_NONE;
        }
        else
        {
            SIM_Input input = {};
            index = sim_input_from_name(name, input) ? (u32)input : REC_NONE;
        }
        if (index != REC_NONE)
        {
            if (seen[index])
            {
                LOG_ERROR("Recording: '%s' appears twice in the layout", name);
                return false;
            }
            seen[index] = true;
        }
        else
        {
            LOG_WARNING("Recording: %s '%s' is unknown to this build, its values are ignored", devices ? "device" : "input", name);
        }
        (devices ? layout.devices : layout.inputs)[i] = (u8)index;
        (devices ? layout.unknown_devices : layout.unknown_inputs) += index == REC_NONE;
    }
    (devices ? layout.devices_count : layout.inputs_count) = count;
    return true;
}

static b8 get_header (u32 version, REC_In& in, REC_Header& out, REC_Layout& layout)
{
    out                = {};
    out.format_version = version;
    out.writer_version = get_u32(in);

    char controller[REC_MAX_NAME + 1];
    if (!get_name(in, controller))
    {
        return false;
    }
    if (!sim_controller_from_name(controller, out.controller))
    {
        LOG_ERROR("Recording: made with controller '%s', which this build cannot simulate", controller);
        return false;
    }

    out.eye_width         = get_u32(in);
    out.eye_height        = get_u32(in);
    out.refresh_rate_hz   = get_f32(in);
    out.display_period_ns = (i64)get_u64(in);
    out.fov_degrees       = get_f32(in);
    out.ipd               = get_f32(in);
    out.stage_bounds.x    = get_f32(in);
    out.stage_bounds.y    = get_f32(in);
    out.aim_offset        = get_pose(in);
    out.palm_offset       = get_pose(in);
    if (get_u8(in))
    {
        char room[REC_MAX_NAME + 1];
        if (!get_name(in, room))
        {
            LOG_ERROR("Recording: invalid room name");
            return false;
        }
        cstr_copy(out.room, sizeof(out.room), room);
        out.room_hash = get_u64(in);
    }

    const b8 sizes_ok  = out.eye_width >= 1 && out.eye_width <= 16384 && out.eye_height >= 1 && out.eye_height <= 16384;
    const b8 timing_ok = out.refresh_rate_hz >= 1.0f && out.refresh_rate_hz <= 1000.0f && out.display_period_ns > 0 && out.display_period_ns <= 1000000000LL;
    const b8 optics_ok = out.fov_degrees > 1.0f && out.fov_degrees < 179.0f && out.ipd >= 0.0f && out.ipd <= 0.2f;
    const b8 stage_ok  = zm::is_finite(out.stage_bounds.x) && zm::is_finite(out.stage_bounds.y) && out.stage_bounds.x >= 0.0f && out.stage_bounds.y >= 0.0f;
    const b8 poses_ok  = pose_valid(out.aim_offset) && pose_valid(out.palm_offset);
    if (in.failed || !sizes_ok || !timing_ok || !optics_ok || !stage_ok || !poses_ok)
    {
        LOG_ERROR("Recording: header values out of range");
        return false;
    }

    layout = {};
    if (!get_table(in, true, layout) || !get_table(in, false, layout))
    {
        LOG_ERROR("Recording: invalid device or input table");
        return false;
    }

    out.initial.time        = (i64)get_u64(in);
    out.initial.frame_index = get_u64(in);
    if (in.failed || out.initial.time < 0 || out.initial.time > REC_MAX_START_TIME)
    {
        LOG_ERROR("Recording: start time out of range");
        return false;
    }
    for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
    {
        out.initial.previous_poses[d] = zm::POSE_IDENTITY;
    }
    for (u32 d = 0; d < layout.devices_count; d++)
    {
        const Pose previous = get_pose(in);
        if (!pose_valid(previous))
        {
            return false;
        }
        if (layout.devices[d] != REC_NONE)
        {
            out.initial.previous_poses[layout.devices[d]] = previous;
        }
    }
    if (!get_state(in, layout, out.initial.state))
    {
        return false;
    }

    out.room_from_stage        = get_pose(in);
    out.anchor_latency_frames  = get_u32(in);
    out.anchor_localize_frames = get_u32(in);
    const u32 ops              = get_u8(in);
    for (u32 i = 0; i < ops; i++)
    {
        const REC_Fault fault = { .result = (i32)get_u32(in), .remaining = get_u32(in) };
        if (i < REC_ANCHOR_OPS)
        {
            out.anchor_faults[i] = fault;
        }
        if (fault.remaining > 0 && fault.result >= 0)
        {
            return false;
        }
    }
    out.hand_tracking_fov_degrees = get_f32(in);
    if (!(out.hand_tracking_fov_degrees >= 1.0f && out.hand_tracking_fov_degrees <= 360.0f))
    {
        return false;
    }
    const u32 frame_ops = get_u8(in);
    for (u32 i = 0; i < frame_ops; i++)
    {
        const REC_Fault fault = { .result = (i32)get_u32(in), .remaining = get_u32(in) };
        if (i < REC_FRAME_OPS)
        {
            out.frame_faults[i] = fault;
        }
        if (fault.remaining > 0 && fault.result >= 0)
        {
            return false;
        }
    }
    return !in.failed && pose_valid(out.room_from_stage);
}

// Every format version this build reads. New versions add a case; old cases are never
// removed once puppetxr reaches 1.0.
static b8 read_header (u32 version, REC_In& in, REC_Header& out, REC_Layout& layout)
{
    switch (version)
    {
    case 1:
        return get_header(version, in, out, layout);
    default:
        return false;
    }
}

static b8 read_state (u32 version, REC_In& in, const REC_Layout& layout, SIM_FrameState& out)
{
    switch (version)
    {
    case 1:
        return get_state(in, layout, out);
    default:
        return false;
    }
}

static b8 version_supported (u32 version)
{
    if (version >= 1 && version <= REC_FORMAT_VERSION)
    {
        return true;
    }
    LOG_ERROR("Recording: format version %u is not one this build reads (1 to %u)", version, REC_FORMAT_VERSION);
    return false;
}

static b8 writer_flush (REC_Writer* writer)
{
    if (writer->used > 0 && !os_file_write(writer->file, writer->buffer, writer->used))
    {
        writer->failed = true;
    }
    writer->used = 0;
    return !writer->failed;
}

static void writer_put (REC_Writer* writer, const u8* data, usize size)
{
    if (writer->used + size > REC_WRITE_BUFFER)
    {
        writer_flush(writer);
    }
    memory_copy(writer->buffer + writer->used, data, size);
    writer->used += size;
}

static void writer_chunk (REC_Writer* writer, const u8 tag[4], const REC_Out& payload)
{
    u8      head_bytes[8];
    REC_Out head = { .data = head_bytes, .capacity = sizeof(head_bytes) };
    put_bytes(head, tag, 4);
    put_u32(head, (u32)payload.size);
    u8      crc_bytes[4];
    REC_Out crc = { .data = crc_bytes, .capacity = sizeof(crc_bytes) };
    put_u32(crc, crc32(payload.data, payload.size));

    writer_put(writer, head.data, head.size);
    writer_put(writer, payload.data, payload.size);
    writer_put(writer, crc.data, crc.size);
}

static b8 get_store (REC_In& in, W_Store& out)
{
    w_store_release(out);
    out.next_serial = get_u64(in);
    out.runs        = get_u64(in);
    const u32 count = get_u32(in);
    if (in.failed || count > in.size / (W_UUID_SIZE + 37))
    {
        return false;
    }
    for (u32 i = 0; i < count; i++)
    {
        W_StoredAnchor anchor = {};
        if (!get_anchor(in, anchor))
        {
            return false;
        }
        out.anchors.add(anchor);
    }
    return true;
}

static b8 get_external (REC_In& in, REC_External& out)
{
    out.anchors.clear();
    out.adverts.clear();
    out.frame_index = get_u64(in);
    const u8  kind  = get_u8(in);
    const u32 count = get_u32(in);
    if (in.failed || count > in.size || (kind != (u8)REC_ExternalKind::CloudAnchors && kind != (u8)REC_ExternalKind::Adverts))
    {
        return false;
    }
    out.kind = (REC_ExternalKind)kind;
    switch (out.kind)
    {
    case REC_ExternalKind::CloudAnchors:
        for (u32 i = 0; i < count; i++)
        {
            if (!get_anchor(in, out.anchors.add()))
            {
                return false;
            }
        }
        return true;
    case REC_ExternalKind::Adverts:
        for (u32 i = 0; i < count; i++)
        {
            W_Advert& advert = out.adverts.add();
            const u8* uuid   = get_bytes(in, W_UUID_SIZE);
            advert.owner     = get_u64(in);
            advert.size      = get_u32(in);
            const u8* bytes  = uuid && advert.size <= W_ADVERT_MAX_SIZE ? get_bytes(in, advert.size) : nullptr;
            if (!bytes)
            {
                return false;
            }
            memory_copy(advert.uuid.bytes, uuid, W_UUID_SIZE);
            memory_copy(advert.buffer, bytes, advert.size);
        }
        return !in.failed;
    }
    return false;
}

static REC_Recording* load_owned (u8* data, usize size)
{
    REC_Recording* recording = (REC_Recording*)os_memory_alloc(sizeof(REC_Recording));
    recording->data          = data;
    recording->size          = size;

    b8 success = false;
    defer {
        if (!success)
        {
            rec_release(recording);
        }
    };

    REC_In    in    = { .data = data, .size = size };
    const u8* magic = get_bytes(in, REC_MAGIC_SIZE);
    if (!magic || !memory_equal(magic, REC_MAGIC, REC_MAGIC_SIZE))
    {
        LOG_ERROR("Recording: not a puppetxr recording");
        return nullptr;
    }
    const u32 version = get_u32(in);
    if (in.failed || !version_supported(version))
    {
        return nullptr;
    }
    const u32 header_size = get_u32(in);
    const u8* header_data = header_size <= REC_MAX_HEADER_SIZE ? get_bytes(in, header_size) : nullptr;
    const u32 header_crc  = get_u32(in);
    if (in.failed || !header_data || crc32(header_data, header_size) != header_crc)
    {
        LOG_ERROR("Recording: header is truncated or corrupt");
        return nullptr;
    }
    REC_In header_in = { .data = header_data, .size = header_size };
    if (!read_header(version, header_in, recording->header, recording->layout) || header_in.pos != header_size)
    {
        LOG_ERROR("Recording: header is invalid");
        return nullptr;
    }

    recording->frame_offsets.reserve(size / 64 + 1);
    while (in.pos < size)
    {
        const u8* tag          = get_bytes(in, 4);
        const u32 payload_size = get_u32(in);
        if (in.failed || payload_size > REC_MAX_CHUNK_SIZE)
        {
            break;
        }
        const usize payload_offset = in.pos;
        const u8*   payload        = get_bytes(in, payload_size);
        const u32   payload_crc    = get_u32(in);
        if (in.failed || crc32(payload, payload_size) != payload_crc)
        {
            break;
        }

        REC_In chunk = { .data = payload, .size = payload_size };
        if (memory_equal(tag, REC_TAG_FRAME, 4))
        {
            SIM_FrameState state = {};
            get_u64(chunk);
            get_u64(chunk);
            if (!read_state(version, chunk, recording->layout, state) || chunk.pos != payload_size)
            {
                break;
            }
            recording->frame_offsets.add(payload_offset);
        }
        else if (memory_equal(tag, REC_TAG_STORE, 4))
        {
            if (!get_store(chunk, recording->store) || chunk.pos != payload_size)
            {
                break;
            }
        }
        else if (memory_equal(tag, REC_TAG_EXTERNAL, 4))
        {
            REC_External external = {};
            const b8     valid    = get_external(chunk, external) && chunk.pos == payload_size;
            rec_external_release(external);
            if (!valid)
            {
                break;
            }
            recording->external_offsets.add(payload_offset);
        }
        else if (memory_equal(tag, REC_TAG_END, 4))
        {
            const u64 frames    = get_u64(chunk);
            recording->complete = !chunk.failed && chunk.pos == payload_size && frames == recording->frame_offsets.length;
            break;
        }
        else
        {
            break;
        }
    }

    if (!recording->complete)
    {
        LOG_WARNING("Recording: truncated or corrupt after %llu frames, replaying those", (unsigned long long)recording->frame_offsets.length);
    }
    success = true;
    return recording;
}

//
// Public API
//

void rec_header_from_config (const SIM_Config& config, const SIM_Snapshot& initial, REC_Header& out_header)
{
    out_header = {
        .format_version    = REC_FORMAT_VERSION,
        .writer_version    = PUPPETXR_VERSION,
        .controller        = config.controller,
        .eye_width         = config.view_width,
        .eye_height        = config.view_height,
        .refresh_rate_hz   = config.refresh_rate_hz,
        .display_period_ns = config.display_period_ns,
        .fov_degrees       = config.fov_degrees,
        .ipd               = config.ipd,
        .stage_bounds      = config.stage_bounds,
        .aim_offset        = config.aim_offset,
        .palm_offset       = config.palm_offset,
        .initial           = initial,
        .room_from_stage   = config.origin_offset_mode == SIM_OriginOffset::Fixed ? config.origin_offset : zm::POSE_IDENTITY,
        .anchor_latency_frames  = config.anchor_latency_frames,
        .anchor_localize_frames = config.anchor_localize_frames,
        .hand_tracking_fov_degrees = config.hand_tracking_fov_degrees,
    };
    cstr_copy(out_header.room, sizeof(out_header.room), config.room);
}

void rec_apply_header (const REC_Header& header, SIM_Config& config)
{
    config.controller        = header.controller;
    config.view_width        = header.eye_width;
    config.view_height       = header.eye_height;
    config.refresh_rate_hz   = header.refresh_rate_hz;
    config.display_period_ns = header.display_period_ns;
    config.fov_degrees       = header.fov_degrees;
    config.ipd               = header.ipd;
    config.stage_bounds      = header.stage_bounds;
    config.aim_offset        = header.aim_offset;
    config.palm_offset       = header.palm_offset;
    cstr_copy(config.room, sizeof(config.room), header.room);
    config.origin_offset_mode     = SIM_OriginOffset::Fixed;
    config.origin_offset          = header.room_from_stage;
    config.anchor_latency_frames  = header.anchor_latency_frames;
    config.anchor_localize_frames = header.anchor_localize_frames;
    config.hand_tracking_fov_degrees = header.hand_tracking_fov_degrees;
}

REC_Writer* rec_writer_create (const char* path, const REC_Header& header, const W_Store* store)
{
    OS_File* file = os_file_create(path);
    if (!file)
    {
        return nullptr;
    }

    REC_Writer* writer   = (REC_Writer*)os_memory_alloc(sizeof(REC_Writer));
    writer->file         = file;
    writer->props_count  = zm::min(header.initial.state.props_count, (u32)SIM_MAX_PROPS);
    memory_copy(writer->props, header.initial.state.props, sizeof(SIM_Prop) * writer->props_count);
    writer->body         = header.initial.state.body;
    writer->face           = header.initial.state.face;
    writer->tracking_drift = header.initial.state.tracking_drift;

    u8      payload_bytes[REC_SCRATCH_SIZE];
    REC_Out payload = { .data = payload_bytes, .capacity = sizeof(payload_bytes) };
    put_header(payload, header);

    u8      prefix_bytes[16];
    REC_Out prefix = { .data = prefix_bytes, .capacity = sizeof(prefix_bytes) };
    put_bytes(prefix, REC_MAGIC, REC_MAGIC_SIZE);
    put_u32(prefix, REC_FORMAT_VERSION);
    put_u32(prefix, (u32)payload.size);

    u8      crc_bytes[4];
    REC_Out crc = { .data = crc_bytes, .capacity = sizeof(crc_bytes) };
    put_u32(crc, crc32(payload.data, payload.size));

    ASSERT(!payload.overflow && !prefix.overflow);
    writer_put(writer, prefix.data, prefix.size);
    writer_put(writer, payload.data, payload.size);
    writer_put(writer, crc.data, crc.size);
    if (store)
    {
        usize size = 8 + 8 + 4;
        for (const W_StoredAnchor& anchor : store->anchors)
        {
            size += anchor_size(anchor);
        }
        Array<u8> bytes = {};
        defer { bytes.release(); };
        bytes.resize(size);
        REC_Out out = { .data = bytes.data, .capacity = size };
        put_u64(out, store->next_serial);
        put_u64(out, store->runs);
        put_u32(out, (u32)store->anchors.length);
        for (const W_StoredAnchor& anchor : store->anchors)
        {
            put_anchor(out, anchor);
        }
        ASSERT(!out.overflow && out.size == size);
        writer_chunk(writer, REC_TAG_STORE, out);
    }
    if (!writer_flush(writer))
    {
        LOG_ERROR("Recording: cannot write the header to %s", path);
        rec_writer_close(writer);
        return nullptr;
    }
    return writer;
}

b8 rec_writer_append_external (REC_Writer* writer, const REC_External& external)
{
    ASSERT(writer);
    usize size = 8 + 1 + 4;
    for (const W_StoredAnchor& anchor : external.anchors)
    {
        size += anchor_size(anchor);
    }
    for (const W_Advert& advert : external.adverts)
    {
        size += W_UUID_SIZE + 8 + 4 + advert.size;
    }
    Array<u8> bytes = {};
    defer { bytes.release(); };
    bytes.resize(size);
    REC_Out out = { .data = bytes.data, .capacity = size };
    put_u64(out, external.frame_index);
    put_u8(out, (u8)external.kind);
    const b8 anchors = external.kind == REC_ExternalKind::CloudAnchors;
    put_u32(out, (u32)(anchors ? external.anchors.length : external.adverts.length));
    if (anchors)
    {
        for (const W_StoredAnchor& anchor : external.anchors)
        {
            put_anchor(out, anchor);
        }
    }
    else
    {
        for (const W_Advert& advert : external.adverts)
        {
            put_bytes(out, advert.uuid.bytes, W_UUID_SIZE);
            put_u64(out, advert.owner);
            put_u32(out, advert.size);
            put_bytes(out, advert.buffer, advert.size);
        }
    }
    ASSERT(!out.overflow);
    writer_chunk(writer, REC_TAG_EXTERNAL, out);
    if (writer_flush(writer) && !os_file_flush(writer->file))
    {
        writer->failed = true;
    }
    return !writer->failed;
}

b8 rec_writer_append (REC_Writer* writer, u64 frame_index, u64 state_hash, const SIM_FrameState& state)
{
    ASSERT(writer);
    u8      payload_bytes[REC_SCRATCH_SIZE];
    REC_Out payload = { .data = payload_bytes, .capacity = sizeof(payload_bytes) };
    put_u64(payload, frame_index);
    put_u64(payload, state_hash);
    b8 changed = state.props_present && state.props_count != writer->props_count;
    for (u32 i = 0; state.props_present && !changed && i < state.props_count; i++)
    {
        changed = !memory_equal(&state.props[i], &writer->props[i], sizeof(SIM_Prop));
    }
    if (changed)
    {
        writer->props_count = state.props_count;
        memory_copy(writer->props, state.props, sizeof(SIM_Prop) * state.props_count);
    }
    const b8 body_changed = state.body_present && !memory_equal(&state.body, &writer->body, sizeof(state.body));
    if (body_changed)
    {
        writer->body = state.body;
    }
    const b8 face_changed = state.face_present && !memory_equal(&state.face, &writer->face, sizeof(state.face));
    if (face_changed)
    {
        writer->face = state.face;
    }
    const b8 drift_changed = state.drift_present && !memory_equal(&state.tracking_drift, &writer->tracking_drift, sizeof(Pose));
    if (drift_changed)
    {
        writer->tracking_drift = state.tracking_drift;
    }
    put_state(payload, state, changed, body_changed, face_changed, drift_changed);
    ASSERT(!payload.overflow);

    writer_chunk(writer, REC_TAG_FRAME, payload);
    writer->frames++;
    if (writer_flush(writer) && !os_file_flush(writer->file))
    {
        writer->failed = true;
    }
    return !writer->failed;
}

u64 rec_writer_frame_count (const REC_Writer* writer)
{
    ASSERT(writer);
    return writer->frames;
}

b8 rec_writer_close (REC_Writer* writer)
{
    if (!writer)
    {
        return false;
    }
    u8      payload_bytes[8];
    REC_Out payload = { .data = payload_bytes, .capacity = sizeof(payload_bytes) };
    put_u64(payload, writer->frames);
    writer_chunk(writer, REC_TAG_END, payload);
    writer_flush(writer);

    const b8 closed  = os_file_close(writer->file);
    const b8 success = closed && !writer->failed;
    os_memory_release(writer);
    return success;
}

REC_Recording* rec_load (const char* path)
{
    u8*   data = nullptr;
    usize size = 0;
    if (!os_read_file(path, &data, &size))
    {
        LOG_ERROR("Recording: cannot read %s", path ? path : "(null)");
        return nullptr;
    }
    return load_owned(data, size);
}

REC_Recording* rec_load_from_memory (const u8* data, usize size)
{
    if (!data && size > 0)
    {
        return nullptr;
    }
    u8* copy = (u8*)os_memory_alloc(size);
    memory_copy(copy, data, size);
    return load_owned(copy, size);
}

void rec_release (REC_Recording* recording)
{
    if (!recording)
    {
        return;
    }
    os_memory_release(recording->data);
    recording->frame_offsets.release();
    recording->external_offsets.release();
    w_store_release(recording->store);
    os_memory_release(recording);
}

const REC_Header& rec_header (const REC_Recording* recording)
{
    ASSERT(recording);
    return recording->header;
}

u64 rec_frame_count (const REC_Recording* recording)
{
    ASSERT(recording);
    return recording->frame_offsets.length;
}

b8 rec_is_complete (const REC_Recording* recording)
{
    ASSERT(recording);
    return recording->complete;
}

b8 rec_read_frame (const REC_Recording* recording, u64 index, u64& out_frame_index, u64& out_state_hash, SIM_FrameState& out_state)
{
    ASSERT(recording);
    if (index >= recording->frame_offsets.length)
    {
        return false;
    }
    const usize offset = recording->frame_offsets[index];
    REC_In      in     = { .data = recording->data + offset, .size = recording->size - offset };
    out_frame_index    = get_u64(in);
    out_state_hash     = get_u64(in);
    return read_state(recording->header.format_version, in, recording->layout, out_state);
}

u32 rec_unknown_inputs (const REC_Recording* recording)
{
    ASSERT(recording);
    return recording->layout.unknown_inputs;
}

u32 rec_unknown_devices (const REC_Recording* recording)
{
    ASSERT(recording);
    return recording->layout.unknown_devices;
}

const W_Store& rec_store (const REC_Recording* recording)
{
    ASSERT(recording);
    return recording->store;
}

u64 rec_external_count (const REC_Recording* recording)
{
    ASSERT(recording);
    return recording->external_offsets.length;
}

b8 rec_read_external (const REC_Recording* recording, u64 index, REC_External& out_external)
{
    ASSERT(recording);
    if (index >= recording->external_offsets.length)
    {
        return false;
    }
    const usize offset = recording->external_offsets[index];
    REC_In      in     = { .data = recording->data + offset, .size = recording->size - offset };
    return get_external(in, out_external);
}

void rec_external_release (REC_External& external)
{
    external.anchors.release();
    external.adverts.release();
}
