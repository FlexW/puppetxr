#include "sim.h"

//
// Types
//

#define SIM_TIME_START       1000000000ll
#define SIM_MAX_REALTIME_DT  1000000000ll
#define SIM_MIN_HAPTIC_NS    10000000ll
#define SIM_MICROGESTURES    5 // SIM_Input::SwipeLeft to TapThumb
#define SIM_MICROGESTURE_NS  200000000ll // how long the thumb moves for one
#define SIM_WHEEL_PUSH       0.02f // m a hand moves for a notch of the wheel
#define SIM_WHEEL_ROLL       (15.0f * zm::DEG_TO_RAD) // a hand turns for a notch

namespace
{

struct SIM_DeviceData
{
    Pose           pose;
    Pose           previous_pose;
    Vec3           linear_velocity;
    Vec3           angular_velocity;
    b8             active;
    SIM_HandSource source;
    Pose           detached_pose; // see SIM_DeviceFrame::detached
    SIM_InputValue inputs[(u32)SIM_Input::Count];
    SIM_Haptic     haptics[(u32)SIM_HapticOutput::Count];
};

// Index, middle, ring and little finger of the right hand in its wrist frame (-Z towards the
// fingers, +Y out of the back of the hand, the thumb on -X)
struct SIM_FingerModel
{
    Vec3 base;    // metacarpal joint
    Vec3 knuckle; // proximal joint
    f32  lengths[3]; // proximal, intermediate, distal phalanx
};

// A body joint of the T-pose: where it is and how its axes lie
struct SIM_BodyRest
{
    Vec3 position;
    Quat basis;
};

// Finger curls and thumb angles of one hand pose
struct SIM_HandShape
{
    f32 curls[4];  // index to little, 0 open to 1 a fist
    f32 thumb[5];  // yaw, pitch, roll of the metacarpal, then proximal and distal flexion, radians
};

enum class SIM_HapticKind : u8
{
    Vibration = 0,
    Envelope,   // samples in SIM_Sim::envelopes spread over the duration
    Pcm,        // samples in SIM_Sim::envelopes at SIM_HAPTIC_PCM_RATE
    Parametric,
};

// What a hand's haptic output plays beyond SIM_Haptic
struct SIM_HapticEffect
{
    SIM_HapticKind      kind;
    u64                 pcm_offset;     // samples dropped from the front since the start, by appends
    u32                 amplitudes_count;
    u32                 frequencies_count;
    SIM_HapticPoint     amplitudes[SIM_MAX_HAPTIC_POINTS];
    SIM_HapticPoint     frequencies[SIM_MAX_HAPTIC_POINTS];
    f32                 min_hz;
    f32                 max_hz;
    b8                  stream_running; // a parametric stream waits for its next frame
    SIM_HapticStreamEnd stream_end;
};

} // namespace

struct SIM_Sim
{
    SIM_Config     config;

    i64            time;
    i64            display_period;
    u64            frame_index;
    u64            last_os_time;
    b8             has_last_os_time;
    i64            last_dt;

    SIM_DeviceData devices[(u32)SIM_Device::Count];
    Pose           local_origin;   // relative to STAGE
    Pose           tracking_drift; // STAGE in the simulator's frame
    Vec3           drift_velocity;
    f32            drift_yaw_rate; // degrees per second

    f32            envelopes[2][(u32)SIM_HapticOutput::Count][SIM_MAX_HAPTIC_SAMPLES]; // per hand and output
    u32            envelope_counts[2][(u32)SIM_HapticOutput::Count];
    SIM_HapticEffect haptic_effects[2][(u32)SIM_HapticOutput::Count];
    f32            refresh_rate;
    f32            default_refresh_rate;
    f32            refresh_rates[SIM_MAX_REFRESH_RATES];
    u32            refresh_rates_count;
    Pose           previous_local_origin; // in effect before local_origin_change_time
    Pose           stationary_origin;     // the LOCAL origin at creation; recentering leaves it
    u64            local_origin_generation;
    i64            local_origin_change_time;
    SIM_Controller controller;
    SIM_HandMesh   hand_meshes[2];
    b8             gesture_seen[2][SIM_MICROGESTURES];      // a pulse was visible for a frame and ends at the next
    b8             gesture_keys[2][SIM_MICROGESTURES];      // held by a key last frame, so holding does not repeat
    SIM_Input      gesture_kind[2];                         // the thumb animates the last microgesture
    b8             keys_down[(u32)OS_Key::Count];
    i64            gesture_start[2];

    SIM_Sample     history[SIM_HISTORY_SAMPLES]; // committed frames, oldest first from history_head
    u32            history_head;
    u32            history_count;

    b8              controls_enabled;
    u32             profile; // index into config.profiles
    b8              reset_hands_pending;
    b8              reset_poses_pending;
    b8              freeze_pending;
    SIM_ClockMode   clock_mode;
    Pose            start_poses[(u32)SIM_Device::Count];
    Pose            hand_offsets[2]; // configured hands relative to the level head frame, for resets
    b8              key_driven[2][(u32)SIM_Input::Count]; // per hand, inputs currently held by a key
    Vec2            ramped[2][(u32)SIM_Input::Count];     // key-driven value of analog inputs on its way to the keys' target
    b8              frozen[2][(u32)SIM_Input::Count];
    f32            mouse_dx;
    f32            mouse_dy;
    f32            wheel;    // notches since the last frame
    char           typed_pending[SIM_MAX_TYPED]; // queued by sim_type_text for the next frame
    u32            typed_pending_length;
    char           typed[SIM_MAX_TYPED];         // typed this frame
    u32            typed_length;
    SIM_Prop       props[SIM_MAX_PROPS];
    u32            props_count;
    u32            next_prop_id;
    SIM_BodyState  body;
    u32            body_generation; // raised by every shape change, XrBodyJointLocationsFB::skeletonChangedCount
    i64            skeleton_change_period; // ns, 0 never
    SIM_FaceState  face;
};

namespace
{

struct SIM_InputInfo
{
    const char*   name;
    SIM_InputType type;
};

// A headset as Meta's runtime presents it: what apps read to tell the models apart, and the
// eye buffer size it recommends
struct SIM_DevicePreset
{
    const char*    name;
    const char*    system_name;
    u8             headset_id[16];
    SIM_Controller controller;
    u32            view_width;
    u32            view_height;
    f32            refresh_rates[SIM_MAX_REFRESH_RATES];
    u32            refresh_rates_count;
};

// An engine plugin that needs the runtime to bend the spec, recognised by its library
struct SIM_KnownModule
{
    const char* module;
    const char* runtime_name;
    b8          accept_time_zero;
    b8          accept_older_frame_time;
    b8          discover_before_running;
};

} // namespace

//
// Statics
//

// System names and XR_META_headset_id ids of Meta's PC runtime; OVRPlugin 207 knows each model
// by its id (an unknown one is a Quest 2 to it). The field of view stays the config's (docs/config.md)
static const SIM_DevicePreset sim_device_presets[] = {
    { .name = "quest3",   .system_name = "Meta Quest 3",   .headset_id = { 0x64, 0x09, 0xe3, 0xb3, 0x50, 0x89, 0x4e, 0xd6, 0x86, 0x8e, 0xaa, 0xed, 0x82, 0xda, 0x36, 0x76 }, .controller = SIM_Controller::MetaTouchPlus, .view_width = 1680, .view_height = 1760, .refresh_rates = { 72.0f, 80.0f, 90.0f, 120.0f }, .refresh_rates_count = 4 },
    { .name = "quest3s",  .system_name = "Meta Quest 3S",  .headset_id = { 0x8c, 0x2e, 0xc4, 0x47, 0xac, 0x73, 0x41, 0x9f, 0x90, 0x96, 0x36, 0xa7, 0x72, 0x33, 0x78, 0x62 }, .controller = SIM_Controller::MetaTouchPlus, .view_width = 1440, .view_height = 1584, .refresh_rates = { 72.0f, 80.0f, 90.0f, 120.0f }, .refresh_rates_count = 4 },
    { .name = "quest2",   .system_name = "Oculus Quest2",  .headset_id = { 0x52, 0x50, 0x78, 0xa5, 0x5a, 0xab, 0x4d, 0xc9, 0xb8, 0x02, 0x1e, 0xbd, 0x6c, 0x7c, 0xff, 0xf4 }, .controller = SIM_Controller::OculusTouch,   .view_width = 1440, .view_height = 1584, .refresh_rates = { 72.0f, 80.0f, 90.0f, 120.0f }, .refresh_rates_count = 4 },
    { .name = "questpro", .system_name = "Meta Quest Pro", .headset_id = { 0x99, 0x09, 0x86, 0xc7, 0x9b, 0xad, 0x47, 0x5f, 0x89, 0x83, 0xc1, 0xc7, 0xbd, 0x49, 0xad, 0x51 }, .controller = SIM_Controller::MetaTouchPro,  .view_width = 1800, .view_height = 1920, .refresh_rates = { 72.0f, 90.0f },                .refresh_rates_count = 2 },
};

// Meta's USB vendor id, which its runtime reports for every headset
#define SIM_META_VENDOR_ID 0x2833u

// Meta's OVRPlugin (its Unity and Unreal SDKs) refuses runtimes it does not know as Meta's and
// locates at time 0. MRUK's shared library discovers the room before the session runs, on
// OVRPlugin and on Unreal's OpenXR plugin, and never retries. Meta's Unreal plugin asks for layer
// resolutions with the time of the frame its RHI thread submits, older than the last waited one;
// a packaged game links it into the executable, so only the editor's module is found. Config file
// and environment still override these defaults
static const SIM_KnownModule sim_known_modules[] = {
    { .module = "OVRPlugin.dll",                .runtime_name = "Oculus", .accept_time_zero = true, .discover_before_running = true },
    { .module = "libOVRPlugin.so",              .runtime_name = "Oculus", .accept_time_zero = true, .discover_before_running = true },
    { .module = "OVRPlugin.dylib",              .runtime_name = "Oculus", .accept_time_zero = true, .discover_before_running = true },
    { .module = "UnrealEditor-OculusXRHMD.dll", .accept_older_frame_time = true },
    { .module = "mrutilitykitshared.dll",       .discover_before_running = true },
    { .module = "libmrutilitykitshared.so",     .discover_before_running = true },
    { .module = "libmrutilitykitshared.dylib",  .discover_before_running = true },
};

static const SIM_InputInfo sim_input_infos[(u32)SIM_Input::Count] = {
    { "x/click",          SIM_InputType::Bool },
    { "x/touch",          SIM_InputType::Bool },
    { "y/click",          SIM_InputType::Bool },
    { "y/touch",          SIM_InputType::Bool },
    { "a/click",          SIM_InputType::Bool },
    { "a/touch",          SIM_InputType::Bool },
    { "b/click",          SIM_InputType::Bool },
    { "b/touch",          SIM_InputType::Bool },
    { "menu/click",       SIM_InputType::Bool },
    { "system/click",     SIM_InputType::Bool },
    { "squeeze/value",    SIM_InputType::Float },
    { "trigger/value",    SIM_InputType::Float },
    { "trigger/touch",    SIM_InputType::Bool },
    { "thumbstick",       SIM_InputType::Vec2 },
    { "thumbstick/click", SIM_InputType::Bool },
    { "thumbstick/touch", SIM_InputType::Bool },
    { "thumbrest/touch",  SIM_InputType::Bool },
    { "select/click",     SIM_InputType::Bool },
    { "trigger/proximity", SIM_InputType::Bool },
    { "thumb_resting_surfaces/proximity", SIM_InputType::Bool },
    { "trigger/curl",     SIM_InputType::Float },
    { "trigger/slide",    SIM_InputType::Float },
    { "trigger/force",    SIM_InputType::Float },
    { "thumbrest/force",  SIM_InputType::Float },
    { "stylus/force",     SIM_InputType::Float },
    { "swipe_left_meta/click",     SIM_InputType::Bool },
    { "swipe_right_meta/click",    SIM_InputType::Bool },
    { "swipe_forward_meta/click",  SIM_InputType::Bool },
    { "swipe_backward_meta/click", SIM_InputType::Bool },
    { "tap_thumb_meta/click",      SIM_InputType::Bool },
};

#define SIM_INPUT_BIT(input) (1u << (u32)SIM_Input::input)
#define SIM_TOUCH_INPUTS (SIM_INPUT_BIT(XClick) | SIM_INPUT_BIT(XTouch) | SIM_INPUT_BIT(YClick) | SIM_INPUT_BIT(YTouch) | SIM_INPUT_BIT(AClick) | SIM_INPUT_BIT(ATouch) | SIM_INPUT_BIT(BClick) | SIM_INPUT_BIT(BTouch) | SIM_INPUT_BIT(MenuClick) | SIM_INPUT_BIT(SystemClick) | SIM_INPUT_BIT(SqueezeValue) | SIM_INPUT_BIT(TriggerValue) | SIM_INPUT_BIT(TriggerTouch) | SIM_INPUT_BIT(Thumbstick) | SIM_INPUT_BIT(ThumbstickClick) | SIM_INPUT_BIT(ThumbstickTouch) | SIM_INPUT_BIT(ThumbrestTouch) | SIM_INPUT_BIT(TriggerProximity) | SIM_INPUT_BIT(ThumbProximity))
static_assert((u32)SIM_Input::Count <= 32, "controller input masks are 32 bits");

// The interaction profiles bind these on one hand only (oxr.cpp's L and R paths)
#define SIM_TOUCH_LEFT_ONLY  (SIM_INPUT_BIT(XClick) | SIM_INPUT_BIT(XTouch) | SIM_INPUT_BIT(YClick) | SIM_INPUT_BIT(YTouch) | SIM_INPUT_BIT(MenuClick))
#define SIM_TOUCH_RIGHT_ONLY (SIM_INPUT_BIT(AClick) | SIM_INPUT_BIT(ATouch) | SIM_INPUT_BIT(BClick) | SIM_INPUT_BIT(BTouch) | SIM_INPUT_BIT(SystemClick))

const static struct
{
    const char* name;
    u32         inputs;     // SIM_INPUT_BIT mask
    u32         left_only;
    u32         right_only;
} sim_controller_infos[(u32)SIM_Controller::Count] = {
    { "oculus_touch",    SIM_TOUCH_INPUTS, SIM_TOUCH_LEFT_ONLY, SIM_TOUCH_RIGHT_ONLY },
    { "khr_simple",      SIM_INPUT_BIT(SelectClick) | SIM_INPUT_BIT(MenuClick), 0, 0 },
    { "meta_touch_pro",  SIM_TOUCH_INPUTS | SIM_INPUT_BIT(TriggerCurl) | SIM_INPUT_BIT(TriggerSlide) | SIM_INPUT_BIT(ThumbrestForce) | SIM_INPUT_BIT(StylusForce), SIM_TOUCH_LEFT_ONLY, SIM_TOUCH_RIGHT_ONLY },
    { "meta_touch_plus", SIM_TOUCH_INPUTS | SIM_INPUT_BIT(TriggerCurl) | SIM_INPUT_BIT(TriggerSlide) | SIM_INPUT_BIT(TriggerForce), SIM_TOUCH_LEFT_ONLY, SIM_TOUCH_RIGHT_ONLY },
};

static const char* const sim_drive_names[(u32)SIM_Drive::Count] = {
    "none", "walk", "move_head", "move_left", "move_right", "turn_body", "turn_head", "turn_left", "turn_right",
    "left_stick", "right_stick", "push_left", "push_right", "roll_left", "roll_right", "turn_eyes",
};

static const char* const sim_slot_names[(u32)SIM_Slot::Count] = {
    "trigger", "grip", "primary", "secondary", "menu", "system", "stick", "stick_click",
};

#define SIM_NO_INPUT SIM_Input::Count

#define SIM_TOUCH_SLOTS                                                                                                                                                 \
    {                                                                                                                                                                   \
        { SIM_Input::TriggerValue, SIM_Input::SqueezeValue, SIM_Input::XClick, SIM_Input::YClick, SIM_Input::MenuClick, SIM_NO_INPUT,           SIM_Input::Thumbstick, SIM_Input::ThumbstickClick }, \
        { SIM_Input::TriggerValue, SIM_Input::SqueezeValue, SIM_Input::AClick, SIM_Input::BClick, SIM_NO_INPUT,         SIM_Input::SystemClick, SIM_Input::Thumbstick, SIM_Input::ThumbstickClick }, \
    }

// [controller][left, right][slot]
static const SIM_Input sim_slot_inputs[(u32)SIM_Controller::Count][2][(u32)SIM_Slot::Count] = {
    SIM_TOUCH_SLOTS,
    {
        { SIM_Input::SelectClick, SIM_NO_INPUT, SIM_NO_INPUT, SIM_NO_INPUT, SIM_Input::MenuClick, SIM_NO_INPUT, SIM_NO_INPUT, SIM_NO_INPUT },
        { SIM_Input::SelectClick, SIM_NO_INPUT, SIM_NO_INPUT, SIM_NO_INPUT, SIM_Input::MenuClick, SIM_NO_INPUT, SIM_NO_INPUT, SIM_NO_INPUT },
    },
    SIM_TOUCH_SLOTS,
    SIM_TOUCH_SLOTS,
};

static b8 device_valid (SIM_Device device)
{
    return (u32)device < (u32)SIM_Device::Count;
}

static b8 input_valid (SIM_Input input)
{
    return (u32)input < (u32)SIM_Input::Count;
}

// XR_INFINITE_DURATION and other durations past the end of time end there instead of overflowing
static i64 time_after (i64 time, i64 duration)
{
    return duration > INT64_MAX - time ? INT64_MAX : time + duration;
}

static b8 is_hand (SIM_Device device)
{
    return device == SIM_Device::LeftHand || device == SIM_Device::RightHand;
}

static SIM_InputValue clamp_input (SIM_Input input, SIM_InputValue value)
{
    switch (sim_input_type(input))
    {
    case SIM_InputType::Bool:
        return { .value = { value.value.x > 0.0f ? 1.0f : 0.0f, 0.0f } };
    case SIM_InputType::Float:
        return { .value = { zm::clamp(value.value.x, 0.0f, 1.0f), 0.0f } };
    case SIM_InputType::Vec2:
        return { .value = { zm::clamp(value.value.x, -1.0f, 1.0f), zm::clamp(value.value.y, -1.0f, 1.0f) } };
    }
    return {};
}

static Quat yaw_only (Quat q)
{
    const Vec3 forward = zm::rotate(q, { 0.0f, 0.0f, -1.0f });
    const Vec3 flat    = { forward.x, 0.0f, forward.z };
    if (zm::length_sq(flat) < 1e-6f)
    {
        const Vec3 up = zm::rotate(q, { 0.0f, 1.0f, 0.0f });
        return zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, zm::atan2(-up.x, -up.z));
    }
    return zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, zm::atan2(-flat.x, -flat.z));
}

static Pose level_frame (const Pose& head)
{
    return { .position = head.position, .orientation = yaw_only(head.orientation) };
}

static u32 hand_index (SIM_Device hand)
{
    return hand == SIM_Device::LeftHand ? 0u : 1u;
}

static SIM_Device hand_device (u32 index)
{
    return index == 0 ? SIM_Device::LeftHand : SIM_Device::RightHand;
}

// A new effect replaces whatever the output played, a running stream included
static void reset_effect (SIM_Sim* sim, SIM_Device hand, SIM_HapticOutput output, SIM_HapticKind kind)
{
    SIM_HapticEffect& effect = sim->haptic_effects[hand_index(hand)][(u32)output];
    effect.kind              = kind;
    effect.pcm_offset        = 0;
    effect.amplitudes_count  = 0;
    effect.frequencies_count = 0;
    effect.stream_running    = false;
}

// PCM samples played after `elapsed` ns, in integers so long effects stay exact
static i64 pcm_samples_elapsed (i64 elapsed)
{
    return elapsed <= 0 ? 0 : elapsed * (i64)SIM_HAPTIC_PCM_RATE / 1000000000LL;
}

static i64 pcm_samples_duration (u64 samples)
{
    return (i64)(samples * 1000000000ull / (u64)SIM_HAPTIC_PCM_RATE);
}

// Linear between points, the first value before them and the last after them
static f32 curve_at (const SIM_HapticPoint* points, u32 count, i64 t, f32 none)
{
    if (count == 0)
    {
        return none;
    }
    if (t <= points[0].time)
    {
        return points[0].value;
    }
    for (u32 i = 1; i < count; i++)
    {
        if (t <= points[i].time)
        {
            const i64 span = points[i].time - points[i - 1].time;
            const f32 frac = span > 0 ? (f32)((f64)(t - points[i - 1].time) / (f64)span) : 1.0f;
            return points[i - 1].value + (points[i].value - points[i - 1].value) * frac;
        }
    }
    return points[count - 1].value;
}

// Appends a frame's points to a stream; points before the stream's present are dropped except
// the last of them, which the curve still interpolates from
static void append_points (SIM_HapticPoint* points, u32& count, i64 now, const SIM_HapticPoint* added, u32 added_count)
{
    u32 keep_from = 0;
    while (keep_from + 1 < count && points[keep_from + 1].time <= now)
    {
        keep_from++;
    }
    memory_move(points, points + keep_from, (count - keep_from) * sizeof(SIM_HapticPoint));
    count -= keep_from;
    const u32 room = SIM_MAX_HAPTIC_POINTS - count;
    const u32 take = zm::min(added_count, room);
    memory_copy(points + count, added, take * sizeof(SIM_HapticPoint));
    count += take;
}

//
// Controller model
//

#define SIM_MODEL_SIDES 16

static const Vec4 SIM_MODEL_BODY    = { 0.12f, 0.12f, 0.14f, 1.0f };
static const Vec4 SIM_MODEL_RING    = { 0.22f, 0.22f, 0.25f, 1.0f };
static const Vec4 SIM_MODEL_TRIGGER = { 0.30f, 0.30f, 0.33f, 1.0f };
static const Vec4 SIM_MODEL_BUTTON  = { 0.80f, 0.80f, 0.82f, 1.0f };

static void begin_part (SIM_RenderModel& model, Vec4 color)
{
    if (model.parts_count >= SIM_MODEL_MAX_PARTS)
    {
        FAIL_MSG("render model has more than %d parts", SIM_MODEL_MAX_PARTS);
        return;
    }
    model.parts[model.parts_count++] = { .vertices_first = model.vertices_count, .indices_first = model.indices_count, .color = color };
}

// Part names become glTF node names; XR_EXT_render_model animates the trigger and the buttons by name
static void name_part (SIM_RenderModel& model, const char* name)
{
    ASSERT(model.parts_count > 0);
    cstr_copy(model.parts[model.parts_count - 1].name, sizeof(model.parts[0].name), name);
}

static void end_part (SIM_RenderModel& model)
{
    if (model.parts_count == 0)
    {
        return;
    }
    SIM_ModelPart& part  = model.parts[model.parts_count - 1];
    part.vertices_count  = model.vertices_count - part.vertices_first;
    part.indices_count   = model.indices_count - part.indices_first;
}

static u32 add_model_vertex (SIM_RenderModel& model, Vec3 position, Vec3 normal)
{
    if (model.vertices_count >= SIM_MODEL_MAX_VERTICES)
    {
        FAIL_MSG("render model has more than %d vertices", SIM_MODEL_MAX_VERTICES);
        return 0;
    }
    model.positions[model.vertices_count] = position;
    model.normals[model.vertices_count]   = normal;
    return model.vertices_count++;
}

static void add_model_triangle (SIM_RenderModel& model, u32 a, u32 b, u32 c)
{
    if (model.indices_count + 3 > SIM_MODEL_MAX_INDICES)
    {
        FAIL_MSG("render model has more than %d indices", SIM_MODEL_MAX_INDICES);
        return;
    }
    model.indices[model.indices_count++] = (u16)a;
    model.indices[model.indices_count++] = (u16)b;
    model.indices[model.indices_count++] = (u16)c;
}

// A closed cylinder along the frame's Z, with flat caps
static void add_cylinder (SIM_RenderModel& model, const Pose& frame, f32 radius, f32 half_length)
{
    const Vec3 axis = zm::rotate(frame.orientation, { 0.0f, 0.0f, 1.0f });
    u32        rings[2][SIM_MODEL_SIDES];
    u32        caps[2][SIM_MODEL_SIDES];
    for (u32 end = 0; end < 2; end++)
    {
        const f32 z = end == 0 ? -half_length : half_length;
        for (u32 k = 0; k < SIM_MODEL_SIDES; k++)
        {
            const f32  angle  = (f32)k * (2.0f * zm::PI / (f32)SIM_MODEL_SIDES);
            const Vec3 radial = zm::rotate(frame.orientation, { zm::cos(angle), zm::sin(angle), 0.0f });
            const Vec3 point  = zm::transform_point(frame, { radius * zm::cos(angle), radius * zm::sin(angle), z });
            rings[end][k]     = add_model_vertex(model, point, radial);
            caps[end][k]      = add_model_vertex(model, point, end == 0 ? -axis : axis);
        }
    }
    const u32 centers[2] = { add_model_vertex(model, zm::transform_point(frame, { 0.0f, 0.0f, -half_length }), -axis), add_model_vertex(model, zm::transform_point(frame, { 0.0f, 0.0f, half_length }), axis) };
    for (u32 k = 0; k < SIM_MODEL_SIDES; k++)
    {
        const u32 n = (k + 1) % SIM_MODEL_SIDES;
        add_model_triangle(model, rings[0][k], rings[0][n], rings[1][k]);
        add_model_triangle(model, rings[0][n], rings[1][n], rings[1][k]);
        add_model_triangle(model, centers[0], caps[0][n], caps[0][k]);
        add_model_triangle(model, centers[1], caps[1][k], caps[1][n]);
    }
}

static f32 axis_of (Vec3 v, u32 axis)
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

static void set_axis (Vec3& v, u32 axis, f32 value)
{
    if (axis == 0)
    {
        v.x = value;
    }
    else if (axis == 1)
    {
        v.y = value;
    }
    else
    {
        v.z = value;
    }
}

// An axis aligned box, four vertices per face for flat normals
static void add_box (SIM_RenderModel& model, Vec3 center, Vec3 half)
{
    for (u32 axis = 0; axis < 3; axis++)
    {
        for (u32 side = 0; side < 2; side++)
        {
            const f32 sign   = side == 0 ? -1.0f : 1.0f;
            Vec3      normal = {};
            set_axis(normal, axis, sign);
            const u32 u_axis  = (axis + 1) % 3;
            const u32 v_axis  = (axis + 2) % 3;
            u32       corners[4];
            for (u32 c = 0; c < 4; c++)
            {
                Vec3 offset = normal * axis_of(half, axis);
                set_axis(offset, u_axis, (c == 1 || c == 2 ? 1.0f : -1.0f) * axis_of(half, u_axis));
                set_axis(offset, v_axis, (c >= 2 ? 1.0f : -1.0f) * axis_of(half, v_axis));
                corners[c] = add_model_vertex(model, center + offset, normal);
            }
            // u x v is the normal of the positive face, so its winding reverses on the negative one
            if (side == 1)
            {
                add_model_triangle(model, corners[0], corners[1], corners[2]);
                add_model_triangle(model, corners[0], corners[2], corners[3]);
            }
            else
            {
                add_model_triangle(model, corners[0], corners[2], corners[1]);
                add_model_triangle(model, corners[0], corners[3], corners[2]);
            }
        }
    }
}

// A torus around the frame's Z
static void add_torus (SIM_RenderModel& model, const Pose& frame, f32 major, f32 minor)
{
    const u32 segments = 2 * SIM_MODEL_SIDES;
    const u32 sides    = 6;
    const u32 first    = model.vertices_count;
    for (u32 i = 0; i < segments; i++)
    {
        const f32  around = (f32)i * (2.0f * zm::PI / (f32)segments);
        const Vec3 out    = { zm::cos(around), zm::sin(around), 0.0f };
        for (u32 j = 0; j < sides; j++)
        {
            const f32  tube   = (f32)j * (2.0f * zm::PI / (f32)sides);
            const Vec3 normal = out * zm::cos(tube) + Vec3{ 0.0f, 0.0f, zm::sin(tube) };
            add_model_vertex(model, zm::transform_point(frame, out * major + normal * minor), zm::rotate(frame.orientation, normal));
        }
    }
    for (u32 i = 0; i < segments; i++)
    {
        for (u32 j = 0; j < sides; j++)
        {
            const u32 a = first + i * sides + j;
            const u32 b = first + ((i + 1) % segments) * sides + j;
            const u32 c = first + ((i + 1) % segments) * sides + (j + 1) % sides;
            const u32 d = first + i * sides + (j + 1) % sides;
            add_model_triangle(model, a, b, c);
            add_model_triangle(model, a, c, d);
        }
    }
}

//
// Virtual keyboard
//

namespace
{

// 5x7 glyphs, rows top to bottom, bit 4 the leftmost column
struct SIM_Glyph
{
    char code;
    u8   rows[7];
};

} // namespace

static const SIM_Glyph SIM_FONT[] = {
    { 'A', { 0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 } },
    { 'B', { 0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110 } },
    { 'C', { 0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110 } },
    { 'D', { 0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100 } },
    { 'E', { 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111 } },
    { 'F', { 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000 } },
    { 'G', { 0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111 } },
    { 'H', { 0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 } },
    { 'I', { 0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 } },
    { 'J', { 0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100 } },
    { 'K', { 0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001 } },
    { 'L', { 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111 } },
    { 'M', { 0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001 } },
    { 'N', { 0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001 } },
    { 'O', { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 } },
    { 'P', { 0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000 } },
    { 'Q', { 0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101 } },
    { 'R', { 0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001 } },
    { 'S', { 0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110 } },
    { 'T', { 0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100 } },
    { 'U', { 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 } },
    { 'V', { 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100 } },
    { 'W', { 0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010 } },
    { 'X', { 0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001 } },
    { 'Y', { 0b10001, 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100 } },
    { 'Z', { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111 } },
    { 'a', { 0b00000, 0b00000, 0b01110, 0b00001, 0b01111, 0b10001, 0b01111 } },
    { 'b', { 0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b11110 } },
    { 'c', { 0b00000, 0b00000, 0b01110, 0b10000, 0b10000, 0b10001, 0b01110 } },
    { 'd', { 0b00001, 0b00001, 0b01101, 0b10011, 0b10001, 0b10001, 0b01111 } },
    { 'e', { 0b00000, 0b00000, 0b01110, 0b10001, 0b11111, 0b10000, 0b01110 } },
    { 'f', { 0b00110, 0b01001, 0b01000, 0b11100, 0b01000, 0b01000, 0b01000 } },
    { 'g', { 0b00000, 0b01111, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110 } },
    { 'h', { 0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001 } },
    { 'i', { 0b00100, 0b00000, 0b01100, 0b00100, 0b00100, 0b00100, 0b01110 } },
    { 'j', { 0b00010, 0b00000, 0b00110, 0b00010, 0b00010, 0b10010, 0b01100 } },
    { 'k', { 0b10000, 0b10000, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010 } },
    { 'l', { 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 } },
    { 'm', { 0b00000, 0b00000, 0b11010, 0b10101, 0b10101, 0b10001, 0b10001 } },
    { 'n', { 0b00000, 0b00000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001 } },
    { 'o', { 0b00000, 0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110 } },
    { 'p', { 0b00000, 0b00000, 0b11110, 0b10001, 0b11110, 0b10000, 0b10000 } },
    { 'q', { 0b00000, 0b00000, 0b01101, 0b10011, 0b01111, 0b00001, 0b00001 } },
    { 'r', { 0b00000, 0b00000, 0b10110, 0b11001, 0b10000, 0b10000, 0b10000 } },
    { 's', { 0b00000, 0b00000, 0b01110, 0b10000, 0b01110, 0b00001, 0b11110 } },
    { 't', { 0b01000, 0b01000, 0b11100, 0b01000, 0b01000, 0b01001, 0b00110 } },
    { 'u', { 0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b10011, 0b01101 } },
    { 'v', { 0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100 } },
    { 'w', { 0b00000, 0b00000, 0b10001, 0b10001, 0b10101, 0b10101, 0b01010 } },
    { 'x', { 0b00000, 0b00000, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001 } },
    { 'y', { 0b00000, 0b00000, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110 } },
    { 'z', { 0b00000, 0b00000, 0b11111, 0b00010, 0b00100, 0b01000, 0b11111 } },
    { '0', { 0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110 } },
    { '1', { 0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 } },
    { '2', { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111 } },
    { '3', { 0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110 } },
    { '4', { 0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010 } },
    { '5', { 0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110 } },
    { '6', { 0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110 } },
    { '7', { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000 } },
    { '8', { 0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110 } },
    { '9', { 0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100 } },
    { ',', { 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b00100, 0b01000 } },
    { '.', { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100 } },
    { '?', { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100 } },
    { '!', { 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100 } },
    { '\b', { 0b00000, 0b00100, 0b01000, 0b11111, 0b01000, 0b00100, 0b00000 } }, // backspace arrow
    { '\n', { 0b00001, 0b00001, 0b00101, 0b01001, 0b11111, 0b01000, 0b00100 } }, // enter arrow
    { '\x0e', { 0b00100, 0b01110, 0b11111, 0b01110, 0b01110, 0b01110, 0b00000 } }, // shift arrow
    { ' ', { 0b00000, 0b00000, 0b00000, 0b00000, 0b10001, 0b11111, 0b00000 } },   // space bracket
};

#define SIM_VKB_PITCH      0.036f // key pitch, meters
#define SIM_VKB_GAP        0.004f
#define SIM_VKB_MARGIN     0.008f
#define SIM_VKB_CELL       32     // atlas cell height, and width of a one unit key
#define SIM_VKB_GLYPH_SCALE 3

static const u8 SIM_VKB_KEY_COLOR[4]    = { 64, 64, 72, 255 };
static const u8 SIM_VKB_BORDER_COLOR[4] = { 40, 40, 46, 255 };
static const u8 SIM_VKB_LABEL_COLOR[4]  = { 235, 235, 235, 255 };
static const u8 SIM_VKB_SHIFT_COLOR[4]  = { 255, 200, 60, 255 };
static const u8 SIM_VKB_PANEL_COLOR[4]  = { 28, 28, 32, 255 };

// Solid blocks below the label cells: the panel colour, and transparent texels for the collision mesh
#define SIM_VKB_PANEL_TEXEL_X 8
#define SIM_VKB_CLEAR_TEXEL_X 24
#define SIM_VKB_SOLID_TEXEL_Y 248

static const SIM_Glyph* find_glyph (char code)
{
    for (const SIM_Glyph& glyph : SIM_FONT)
    {
        if (glyph.code == code)
        {
            return &glyph;
        }
    }
    return nullptr;
}

static char vkb_label (const SIM_VkbKey& key, b8 shifted)
{
    switch (key.kind)
    {
    case SIM_VkbKeyKind::Char:
        return shifted ? key.upper : key.lower;
    case SIM_VkbKeyKind::Backspace:
        return '\b';
    case SIM_VkbKeyKind::Enter:
        return '\n';
    case SIM_VkbKeyKind::Shift:
        return '\x0e';
    case SIM_VkbKeyKind::Space:
        return ' ';
    }
    return ' ';
}

static void fill_texels (u8* rgba, u32 x, u32 y, u32 width, u32 height, const u8 color[4])
{
    for (u32 row = y; row < y + height; row++)
    {
        for (u32 column = x; column < x + width; column++)
        {
            memory_copy(rgba + ((usize)row * SIM_VKB_TEXTURE_WIDTH + column) * 4, color, 4);
        }
    }
}

static const char* const SIM_VKB_SPECIAL_NAMES[] = { "", "backspace", "enter", "shift", "space" };

static void add_vkb_quad (SIM_RenderModel& model, Vec2 center, Vec2 half, f32 z, Vec2 uv_low, Vec2 uv_high)
{
    const Vec3 normal = { 0.0f, 0.0f, 1.0f };
    u32        corners[4];
    for (u32 c = 0; c < 4; c++)
    {
        const f32 sx = c == 1 || c == 2 ? 1.0f : -1.0f;
        const f32 sy = c >= 2 ? 1.0f : -1.0f;
        corners[c]   = add_model_vertex(model, { center.x + sx * half.x, center.y + sy * half.y, z }, normal);
        // Texture rows run top down, keyboard y up
        model.uvs[corners[c]] = { sx > 0.0f ? uv_high.x : uv_low.x, sy > 0.0f ? uv_low.y : uv_high.y };
    }
    add_model_triangle(model, corners[0], corners[1], corners[2]);
    add_model_triangle(model, corners[0], corners[2], corners[3]);
}

//
// Props
//

static const char* const sim_prop_kind_names[(u32)SIM_PropKind::Count]     = { "keyboard", "qr" };
static const char* const sim_prop_motion_names[(u32)SIM_PropMotion::Count] = { "still", "linear", "circle" };

#define SIM_PROP_MAX_SPEED  100.0f
#define SIM_PROP_MAX_RADIUS 100.0f
#define SIM_PROP_MIN_SIZE   0.01f
#define SIM_PROP_MAX_SIZE   10.0f

// The sim_keyboard_model's bounding box
static const Vec3 SIM_KEYBOARD_EXTENT = { 0.30f, 0.018f, 0.112f };

static b8 prop_text_terminated (const SIM_Prop& prop)
{
    for (u32 i = 0; i < SIM_MAX_PROP_TEXT; i++)
    {
        if (prop.text[i] == 0)
        {
            return true;
        }
    }
    return false;
}

// A copy of `prop` fit to store: normalized orientation, no text on keyboards; false when invalid
static b8 normalize_prop (const SIM_Prop& prop, SIM_Prop& out)
{
    const b8 kind_valid   = (u32)prop.kind < (u32)SIM_PropKind::Count && (u32)prop.motion < (u32)SIM_PropMotion::Count;
    const b8 pose_valid   = zm::is_finite(prop.pose) && zm::length(prop.pose.orientation) > 1e-4f && zm::is_finite(prop.target.x) && zm::is_finite(prop.target.y) && zm::is_finite(prop.target.z);
    const b8 speed_valid  = zm::is_finite(prop.speed) && prop.speed >= 0.0f && prop.speed <= SIM_PROP_MAX_SPEED;
    const b8 radius_valid = zm::is_finite(prop.radius) && prop.radius >= 0.0f && prop.radius <= SIM_PROP_MAX_RADIUS && (prop.motion != SIM_PropMotion::Circle || prop.radius > 0.0f);
    const b8 size_valid   = prop.kind != SIM_PropKind::QrCode || (zm::is_finite(prop.size) && prop.size >= SIM_PROP_MIN_SIZE && prop.size <= SIM_PROP_MAX_SIZE);
    if (!kind_valid || !pose_valid || !speed_valid || !radius_valid || !size_valid || !prop_text_terminated(prop))
    {
        return false;
    }
    out = {
        .id     = prop.id,
        .kind   = prop.kind,
        .motion = prop.motion,
        .pose   = { prop.pose.position, zm::normalize(prop.pose.orientation) },
        .target = prop.target,
        .radius = prop.radius,
        .speed  = prop.speed,
        .size   = prop.kind == SIM_PropKind::QrCode ? prop.size : 0.0f,
        .start  = prop.start,
    };
    if (prop.kind == SIM_PropKind::QrCode)
    {
        cstr_copy(out.text, sizeof(out.text), prop.text);
    }
    return true;
}

static u64 hash_props (const SIM_Prop* props, u32 count)
{
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < count; i++)
    {
        const SIM_Prop& p = props[i];
        const f32 values[] = { p.pose.position.x, p.pose.position.y, p.pose.position.z, p.pose.orientation.x, p.pose.orientation.y, p.pose.orientation.z, p.pose.orientation.w, p.target.x, p.target.y, p.target.z, p.radius, p.speed, p.size };
        h = (h ^ hash((const u8*)values, sizeof(values))) * 0x100000001B3ull;
        h = (h ^ (((u64)p.id << 16) | ((u64)p.kind << 8) | (u64)p.motion)) * 0x100000001B3ull;
        h = (h ^ (u64)p.start) * 0x100000001B3ull;
        h = (h ^ hash((const u8*)p.text, cstr_length(p.text))) * 0x100000001B3ull;
    }
    return h;
}

//
// Hand model
//

static const SIM_FingerModel sim_fingers[4] = {
    { { -0.012f,  0.000f, -0.005f }, { -0.022f,  0.000f, -0.085f }, { 0.040f, 0.024f, 0.022f } },
    { {  0.000f,  0.000f, -0.005f }, { -0.002f,  0.000f, -0.090f }, { 0.045f, 0.028f, 0.024f } },
    { {  0.012f, -0.002f, -0.005f }, {  0.017f, -0.003f, -0.085f }, { 0.042f, 0.027f, 0.023f } },
    { {  0.022f, -0.005f, -0.005f }, {  0.034f, -0.007f, -0.075f }, { 0.033f, 0.020f, 0.020f } },
};

static const Vec3 SIM_THUMB_BASE              = { -0.020f, -0.012f, -0.020f };
static const f32  sim_thumb_lengths[3]        = { 0.045f, 0.032f, 0.026f };
static const Vec3 SIM_PALM_IN_WRIST           = { -0.001f, 0.0f, -0.0475f }; // middle of the middle metacarpal
static const Vec3 SIM_PALM_FROM_HANDLE        = { 0.0f, 0.025f, 0.015f };    // palm joint behind and above a held handle, in the palm frame
static const f32  sim_finger_flexion[3]       = { 85.0f, 100.0f, 70.0f };     // degrees at a full curl, proximal to distal
#define SIM_PINCH_INDEX_CURL                    0.6f
#define SIM_HELD_GRAB                           0.7f  // a held controller keeps the fingers at least this curled
#define SIM_HELD_PINCH                          0.35f

// Thumb angles in degrees: open, touching the index tip at SIM_PINCH_INDEX_CURL, and folded over
// the middle finger in a fist; fitted offline so the pinch closes
static const f32 sim_thumb_open[5]  = { 35.0f, -15.0f, 60.0f, 5.0f, 5.0f };
static const f32 sim_thumb_pinch[5] = { 43.2f, -27.9f, 60.0f, 43.1f, 54.4f };
static const f32 sim_thumb_fist[5]  = { 15.9f, -18.4f, 60.0f, 33.8f, 44.2f };

static const f32 sim_joint_radii[SIM_HAND_JOINT_COUNT] = {
    0.016f, 0.018f,
    0.013f, 0.011f, 0.009f, 0.008f,
    0.011f, 0.010f, 0.009f, 0.008f, 0.007f,
    0.0115f, 0.0105f, 0.0095f, 0.0085f, 0.0075f,
    0.010f, 0.0095f, 0.0085f, 0.0075f, 0.0065f,
    0.009f, 0.0085f, 0.0075f, 0.0065f, 0.0055f,
};

// The palm frame in the grip frame: spec grip +X points out of the back of the right hand and
// into the back of the left one, grip -Y runs along the metacarpals of a held controller
static const Quat SIM_RIGHT_PALM_IN_GRIP = { -0.5f, -0.5f, -0.5f, 0.5f };
static const Quat SIM_LEFT_PALM_IN_GRIP  = { -0.5f, 0.5f, 0.5f, 0.5f };

#define SIM_HAND_MESH_SIDES 8

static const char* const sim_hand_source_names[(u32)SIM_HandSource::Count] = { "controller", "hand" };

static u8 joint_parent (u32 joint)
{
    if (joint <= (u32)SIM_HandJoint::Wrist || joint == (u32)SIM_HandJoint::ThumbMetacarpal)
    {
        return (u8)SIM_HandJoint::Wrist;
    }
    const b8 metacarpal = joint >= (u32)SIM_HandJoint::IndexMetacarpal && (joint - (u32)SIM_HandJoint::IndexMetacarpal) % 5 == 0;
    return metacarpal ? (u8)SIM_HandJoint::Wrist : (u8)(joint - 1);
}

static Quat flexion (f32 radians)
{
    return zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -radians);
}

static SIM_HandShape hand_shape (f32 pinch, f32 grab)
{
    SIM_HandShape shape = {};
    shape.curls[0]      = pinch * (SIM_PINCH_INDEX_CURL + (1.0f - SIM_PINCH_INDEX_CURL) * grab);
    shape.curls[1]      = grab;
    shape.curls[2]      = grab;
    shape.curls[3]      = grab;
    const f32 toward_pinch = pinch * (1.0f - grab);
    for (u32 i = 0; i < 5; i++)
    {
        const f32 degrees = sim_thumb_open[i] + toward_pinch * (sim_thumb_pinch[i] - sim_thumb_open[i]) + grab * (sim_thumb_fist[i] - sim_thumb_open[i]);
        shape.thumb[i]    = degrees * zm::DEG_TO_RAD;
    }
    return shape;
}

// -Z along the direction, +Y as close to up as the direction allows
static Quat bone_frame (Vec3 direction)
{
    const f32 yaw   = zm::atan2(-direction.x, -direction.z);
    const f32 pitch = zm::asin(zm::clamp(direction.y, -1.0f, 1.0f));
    return zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, yaw) * zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, pitch);
}

// Each joint of a chain is placed one bone along its parent's -Z and bends about its own X
static void place_chain (Pose* joints, Vec3 position, Quat frame, const f32* flexions, const f32* lengths, u32 bones)
{
    joints[0] = { position, frame };
    for (u32 b = 0; b < bones; b++)
    {
        position     += zm::rotate(frame, { 0.0f, 0.0f, -lengths[b] });
        frame         = b + 1 < bones ? frame * flexion(flexions[b]) : frame;
        joints[b + 1] = { position, frame };
    }
}

// Joints of the right hand in its wrist frame; the left hand mirrors them across x
static void hand_local (const SIM_HandShape& shape, b8 left, Pose (&out)[SIM_HAND_JOINT_COUNT])
{
    out[(u32)SIM_HandJoint::Palm]  = { SIM_PALM_IN_WRIST, zm::QUAT_IDENTITY };
    out[(u32)SIM_HandJoint::Wrist] = zm::POSE_IDENTITY;

    const f32* t           = shape.thumb;
    const Quat thumb       = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, t[0]) * zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, t[1]) * zm::quat_from_axis_angle({ 0.0f, 0.0f, 1.0f }, t[2]);
    const f32  thumb_flex[2] = { t[3], t[4] };
    place_chain(&out[(u32)SIM_HandJoint::ThumbMetacarpal], SIM_THUMB_BASE, thumb, thumb_flex, sim_thumb_lengths, 3);

    for (u32 f = 0; f < 4; f++)
    {
        const SIM_FingerModel& finger  = sim_fingers[f];
        const u32              first   = (u32)SIM_HandJoint::IndexMetacarpal + f * 5;
        const Vec3             bone    = finger.knuckle - finger.base;
        const f32              lengths[4] = { zm::length(bone), finger.lengths[0], finger.lengths[1], finger.lengths[2] };
        f32                    flexions[3] = {};
        for (u32 k = 0; k < 3; k++)
        {
            flexions[k] = sim_finger_flexion[k] * zm::DEG_TO_RAD * shape.curls[f];
        }
        // The metacarpal is rigid: the first bend is at the knuckle
        out[first] = { finger.base, bone_frame(zm::normalize(bone)) };
        place_chain(&out[first + 1], finger.knuckle, out[first].orientation * flexion(flexions[0]), flexions + 1, lengths + 1, 3);
    }

    if (left)
    {
        for (Pose& joint : out)
        {
            joint.position.x    = -joint.position.x;
            joint.orientation.y = -joint.orientation.y;
            joint.orientation.z = -joint.orientation.z;
        }
    }
}

// Where the wrist frame lies relative to the grip pose
static Pose wrist_in_grip (b8 left)
{
    const Quat palm_orientation = left ? SIM_LEFT_PALM_IN_GRIP : SIM_RIGHT_PALM_IN_GRIP;
    const Pose palm             = { zm::rotate(palm_orientation, SIM_PALM_FROM_HANDLE), palm_orientation };
    const Vec3 palm_in_wrist    = { left ? -SIM_PALM_IN_WRIST.x : SIM_PALM_IN_WRIST.x, SIM_PALM_IN_WRIST.y, SIM_PALM_IN_WRIST.z };
    return zm::pose_mul(palm, { -palm_in_wrist, zm::QUAT_IDENTITY });
}

// A prism from a joint to its child, its rings in the joint's X/Y plane, capped at both ends,
// moving with the joint `skin`
static void add_bone (SIM_HandMesh& mesh, u32 joint, u32 skin, Vec3 end, f32 end_radius)
{
    const Pose& frame  = mesh.bind_poses[joint];
    const Vec3  x      = zm::rotate(frame.orientation, { 1.0f, 0.0f, 0.0f });
    const Vec3  y      = zm::rotate(frame.orientation, { 0.0f, 1.0f, 0.0f });
    const Vec3  along  = zm::normalize(end - frame.position);
    const Vec3  ends[2]  = { frame.position, end };
    const f32   radii[2] = { mesh.radii[joint], end_radius };
    const u32   first  = mesh.vertices_count;
    for (u32 r = 0; r < 2; r++)
    {
        for (u32 k = 0; k < SIM_HAND_MESH_SIDES; k++)
        {
            const f32  angle  = (f32)k * (2.0f * zm::PI / (f32)SIM_HAND_MESH_SIDES);
            const Vec3 normal = x * zm::cos(angle) + y * zm::sin(angle);
            const u32  v      = mesh.vertices_count++;
            mesh.positions[v] = ends[r] + normal * radii[r];
            mesh.normals[v]   = normal;
            mesh.uvs[v]       = { (f32)k / (f32)SIM_HAND_MESH_SIDES, (f32)r };
            mesh.joints[v]    = (u8)skin;
        }
    }
    const u32 base_center = mesh.vertices_count++;
    const u32 tip_center  = mesh.vertices_count++;
    mesh.positions[base_center] = ends[0] - along * (radii[0] * 0.5f);
    mesh.normals[base_center]   = -along;
    mesh.uvs[base_center]       = { 0.5f, 0.0f };
    mesh.joints[base_center]    = (u8)skin;
    mesh.positions[tip_center]  = ends[1] + along * (radii[1] * 0.8f);
    mesh.normals[tip_center]    = along;
    mesh.uvs[tip_center]        = { 0.5f, 1.0f };
    mesh.joints[tip_center]     = (u8)skin;

    for (u32 k = 0; k < SIM_HAND_MESH_SIDES; k++)
    {
        const u32 a0   = first + k;
        const u32 a1   = first + (k + 1) % SIM_HAND_MESH_SIDES;
        const u32 b0   = a0 + SIM_HAND_MESH_SIDES;
        const u32 b1   = a1 + SIM_HAND_MESH_SIDES;
        const u32 tris[12] = { a0, b0, a1, a1, b0, b1, b0, tip_center, b1, a1, base_center, a0 };
        for (const u32 index : tris)
        {
            mesh.indices[mesh.indices_count++] = (i16)index;
        }
    }
}

static void build_hand_mesh (SIM_HandMesh& mesh, b8 left)
{
    mesh = {};
    hand_local(hand_shape(0.0f, 0.0f), left, mesh.bind_poses);
    for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
    {
        mesh.radii[j]   = sim_joint_radii[j];
        mesh.parents[j] = joint_parent(j);
    }
    for (u32 j = (u32)SIM_HandJoint::ThumbMetacarpal; j < SIM_HAND_JOINT_COUNT; j++)
    {
        const b8 tip = j == (u32)SIM_HandJoint::ThumbTip || (j >= (u32)SIM_HandJoint::IndexMetacarpal && (j - (u32)SIM_HandJoint::IndexMetacarpal) % 5 == 4);
        // Meta's hand skeleton has no index, middle or ring metacarpal; OVRPlugin cannot map a
        // vertex skinned to one, and they move with the wrist anyway
        const b8 rigid = j == (u32)SIM_HandJoint::IndexMetacarpal || j == (u32)SIM_HandJoint::MiddleMetacarpal || j == (u32)SIM_HandJoint::RingMetacarpal;
        if (!tip)
        {
            add_bone(mesh, j, rigid ? (u32)SIM_HandJoint::Wrist : j, mesh.bind_poses[j + 1].position, mesh.radii[j + 1]);
        }
    }
    ASSERT(mesh.vertices_count <= SIM_HAND_MESH_MAX_VERTICES && mesh.indices_count <= SIM_HAND_MESH_MAX_INDICES);
}

// The thumb swings out and back over SIM_MICROGESTURE_NS: yaw for left and right (mirrored on
// the left hand so both swipe the same way in the world), flexion for forward, backward and tap
static void add_microgesture (const SIM_Sim* sim, SIM_Device hand, SIM_HandShape& shape)
{
    const u32 h       = hand == SIM_Device::LeftHand ? 0 : 1;
    const i64 elapsed = sim->time - sim->gesture_start[h];
    if (sim->gesture_kind[h] == SIM_NO_INPUT || elapsed < 0 || elapsed >= SIM_MICROGESTURE_NS)
    {
        return;
    }
    const f32 weight = zm::sin(zm::PI * (f32)elapsed / (f32)SIM_MICROGESTURE_NS) * 20.0f * zm::DEG_TO_RAD;
    const f32 side   = hand == SIM_Device::LeftHand ? -1.0f : 1.0f;
    switch (sim->gesture_kind[h])
    {
    case SIM_Input::SwipeLeft:
        shape.thumb[0] += weight * side;
        break;
    case SIM_Input::SwipeRight:
        shape.thumb[0] -= weight * side;
        break;
    case SIM_Input::SwipeForward:
        shape.thumb[3] -= weight;
        break;
    case SIM_Input::SwipeBackward:
        shape.thumb[3] += weight;
        break;
    case SIM_Input::TapThumb:
        shape.thumb[3] += weight;
        shape.thumb[4] += weight;
        break;
    default:
        break;
    }
}

// Pulses set between frames are seen for one frame; a key press is seen in the frame it lands in
static void age_microgestures (SIM_Sim* sim)
{
    for (u32 h = 0; h < 2; h++)
    {
        SIM_DeviceData& device = sim->devices[(u32)hand_device(h)];
        for (u32 g = 0; g < SIM_MICROGESTURES; g++)
        {
            SIM_InputValue& value = device.inputs[(u32)SIM_Input::SwipeLeft + g];
            if (value.value.x != 0.0f && sim->gesture_seen[h][g])
            {
                value                  = {};
                sim->gesture_seen[h][g] = false;
            }
            else if (value.value.x != 0.0f)
            {
                sim->gesture_seen[h][g] = true;
            }
        }
    }
}

static void start_microgestures (SIM_Sim* sim)
{
    for (u32 h = 0; h < 2; h++)
    {
        const SIM_DeviceData& device = sim->devices[(u32)hand_device(h)];
        for (u32 g = 0; g < SIM_MICROGESTURES; g++)
        {
            if (device.inputs[(u32)SIM_Input::SwipeLeft + g].value.x != 0.0f)
            {
                sim->gesture_kind[h]  = (SIM_Input)((u32)SIM_Input::SwipeLeft + g);
                sim->gesture_start[h] = sim->time;
            }
        }
    }
}

static b8 hand_input_exists (const SIM_Sim* sim, SIM_Device device, SIM_Input input)
{
    return is_hand(device) && (u32)input < (u32)SIM_Input::Count && sim_hand_has_input(sim->devices[(u32)device].source, sim->controller, device, input);
}

static b8 is_microgesture (SIM_Input input)
{
    return (u32)input >= (u32)SIM_Input::SwipeLeft && (u32)input < (u32)SIM_Input::SwipeLeft + SIM_MICROGESTURES;
}

// On a tracked hand the stick swipes the thumb along the index in the stick's direction
static SIM_Input swipe_of (Vec2 direction)
{
    if (zm::abs(direction.x) >= zm::abs(direction.y))
    {
        return direction.x < 0.0f ? SIM_Input::SwipeLeft : SIM_Input::SwipeRight;
    }
    return direction.y > 0.0f ? SIM_Input::SwipeForward : SIM_Input::SwipeBackward;
}

static SIM_Input slot_input (const SIM_Sim* sim, SIM_Device hand, SIM_Slot slot)
{
    if (sim->devices[(u32)hand].source == SIM_HandSource::Controller)
    {
        return sim_controller_slot_input(sim->controller, hand, slot);
    }
    switch (slot)
    {
    case SIM_Slot::Trigger:
        return SIM_Input::TriggerValue;
    case SIM_Slot::Grip:
        return SIM_Input::SqueezeValue;
    case SIM_Slot::Menu:
        return SIM_Input::MenuClick;
    case SIM_Slot::Primary:
        return SIM_Input::TapThumb;
    default:
        return SIM_NO_INPUT;
    }
}

static void zero_missing_inputs (SIM_Sim* sim)
{
    for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
    {
        for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
        {
            if (!hand_input_exists(sim, (SIM_Device)d, (SIM_Input)i))
            {
                sim->devices[d].inputs[i] = {};
            }
        }
    }
}

static const SIM_Profile& current_profile (const SIM_Sim* sim)
{
    return sim->config.profiles[sim->profile];
}

static SIM_Device active_hand (const SIM_Sim* sim)
{
    switch (current_profile(sim).hand)
    {
    case SIM_BindHand::Left:
        return SIM_Device::LeftHand;
    case SIM_BindHand::Right:
        return SIM_Device::RightHand;
    case SIM_BindHand::Active:
        break;
    }
    return sim->config.primary_hand == SIM_Device::LeftHand ? SIM_Device::LeftHand : SIM_Device::RightHand;
}

// The arrows are the turn keys, for keyboards where I/J/K/L are hard to reach and the other way round
static b8 key_down (const SIM_Sim* sim, OS_Key key)
{
    const SIM_Config& config = sim->config;
    if (sim->keys_down[(u32)key])
    {
        return true;
    }
    return (key == config.key_turn_up && sim->keys_down[(u32)OS_Key::ArrowUp]) || (key == config.key_turn_down && sim->keys_down[(u32)OS_Key::ArrowDown]) ||
           (key == config.key_turn_left && sim->keys_down[(u32)OS_Key::ArrowLeft]) || (key == config.key_turn_right && sim->keys_down[(u32)OS_Key::ArrowRight]);
}

static f32 key_axis (const SIM_Sim* sim, OS_Key positive, OS_Key negative)
{
    return (key_down(sim, positive) ? 1.0f : 0.0f) - (key_down(sim, negative) ? 1.0f : 0.0f);
}

static SIM_Device binding_hand (const SIM_Sim* sim, SIM_BindHand hand)
{
    switch (hand)
    {
    case SIM_BindHand::Left:
        return SIM_Device::LeftHand;
    case SIM_BindHand::Right:
        return SIM_Device::RightHand;
    case SIM_BindHand::Active:
        break;
    }
    return active_hand(sim);
}

static u8 held_modifiers (const SIM_Sim* sim)
{
    u8 modifiers = 0;
    if (sim->keys_down[(u32)OS_Key::LeftShift] || sim->keys_down[(u32)OS_Key::RightShift])
    {
        modifiers |= SIM_MOD_SHIFT;
    }
    if (sim->keys_down[(u32)OS_Key::LeftCtrl] || sim->keys_down[(u32)OS_Key::RightCtrl])
    {
        modifiers |= SIM_MOD_CTRL;
    }
    return modifiers;
}

static SIM_Input touch_of (SIM_Input input)
{
    switch (input)
    {
    case SIM_Input::XClick:
        return SIM_Input::XTouch;
    case SIM_Input::YClick:
        return SIM_Input::YTouch;
    case SIM_Input::AClick:
        return SIM_Input::ATouch;
    case SIM_Input::BClick:
        return SIM_Input::BTouch;
    case SIM_Input::TriggerValue:
        return SIM_Input::TriggerTouch;
    case SIM_Input::Thumbstick:
    case SIM_Input::ThumbstickClick:
        return SIM_Input::ThumbstickTouch;
    default:
        return SIM_NO_INPUT;
    }
}

static b8 refresh_rate_valid (f32 hz)
{
    return zm::is_finite(hz) && hz >= 1.0f && hz <= 1000.0f;
}

static i64 refresh_period (f32 hz)
{
    return (i64)(1e9 / (f64)hz + 0.5);
}

// Inserts into the sorted list of supported rates, once
static void add_refresh_rate (SIM_Sim* sim, f32 hz)
{
    if (!refresh_rate_valid(hz) || sim->refresh_rates_count == SIM_MAX_REFRESH_RATES)
    {
        return;
    }
    u32 at = 0;
    while (at < sim->refresh_rates_count && sim->refresh_rates[at] < hz)
    {
        at++;
    }
    if (at < sim->refresh_rates_count && sim->refresh_rates[at] == hz)
    {
        return;
    }
    for (u32 i = sim->refresh_rates_count; i > at; i--)
    {
        sim->refresh_rates[i] = sim->refresh_rates[i - 1];
    }
    sim->refresh_rates[at] = hz;
    sim->refresh_rates_count++;
}

// A touched trigger or thumb surface is also near the finger
static SIM_Input proximity_of (SIM_Input touch)
{
    switch (touch)
    {
    case SIM_Input::TriggerTouch:
        return SIM_Input::TriggerProximity;
    case SIM_Input::XTouch:
    case SIM_Input::YTouch:
    case SIM_Input::ATouch:
    case SIM_Input::BTouch:
    case SIM_Input::ThumbstickTouch:
    case SIM_Input::ThumbrestTouch:
        return SIM_Input::ThumbProximity;
    default:
        return SIM_NO_INPUT;
    }
}

static void turn_device (SIM_DeviceData& device, f32 yaw, f32 pitch)
{
    const Quat q = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, yaw) * device.pose.orientation * zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, pitch);
    device.pose.orientation = zm::normalize(q);
}

// Yaw turns the whole body around the head, pitch only tilts the head
static void turn_body (SIM_Sim* sim, f32 yaw, f32 pitch)
{
    SIM_DeviceData& head = sim->devices[(u32)SIM_Device::Head];
    if (yaw != 0.0f)
    {
        const Vec3 pivot = head.pose.position;
        const Quat turn  = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, yaw);
        for (SIM_DeviceData& device : sim->devices)
        {
            device.pose.position    = pivot + zm::rotate(turn, device.pose.position - pivot);
            device.pose.orientation = zm::normalize(turn * device.pose.orientation);
        }
    }
    // The hands turn by what the head did, which stops at its pitch limit
    if (pitch != 0.0f)
    {
        const Quat before = head.pose.orientation;
        turn_device(head, 0.0f, pitch);
        const Vec3 pivot = head.pose.position;
        const Quat turn  = zm::normalize(head.pose.orientation * zm::conjugate(before));
        for (SIM_DeviceData& device : sim->devices)
        {
            if (&device != &head)
            {
                device.pose.position    = pivot + zm::rotate(turn, device.pose.position - pivot);
                device.pose.orientation = zm::normalize(turn * device.pose.orientation);
            }
        }
    }
}

static void turn_drive (SIM_Sim* sim, SIM_Drive drive, f32 yaw, f32 pitch)
{
    if (yaw == 0.0f && pitch == 0.0f)
    {
        return;
    }
    switch (drive)
    {
    case SIM_Drive::TurnBody:
        turn_body(sim, yaw, pitch);
        break;
    case SIM_Drive::TurnHead:
        turn_device(sim->devices[(u32)SIM_Device::Head], yaw, pitch);
        break;
    case SIM_Drive::TurnLeft:
        turn_device(sim->devices[(u32)SIM_Device::LeftHand], yaw, pitch);
        break;
    case SIM_Drive::TurnRight:
        turn_device(sim->devices[(u32)SIM_Device::RightHand], yaw, pitch);
        break;
    case SIM_Drive::TurnEyes:
    {
        SIM_FaceState& face = sim->face;
        face.gaze_yaw       = zm::clamp(face.gaze_yaw + yaw * zm::RAD_TO_DEG, -SIM_FACE_MAX_GAZE_YAW, SIM_FACE_MAX_GAZE_YAW);
        face.gaze_pitch     = zm::clamp(face.gaze_pitch + pitch * zm::RAD_TO_DEG, -SIM_FACE_MAX_GAZE_PITCH, SIM_FACE_MAX_GAZE_PITCH);
        face.look_at        = false;
        face.target         = {};
        break;
    }
    default:
        break;
    }
}

// `axes`: x right, y up (Q/E), z forward of a key cluster
static void move_drive (SIM_Sim* sim, SIM_Drive drive, Vec3 axes, f32 dt)
{
    if (zm::length_sq(axes) == 0.0f)
    {
        return;
    }
    const SIM_Config& config = sim->config;
    const Quat        head   = sim->devices[(u32)SIM_Device::Head].pose.orientation;
    switch (drive)
    {
    case SIM_Drive::Walk:
    case SIM_Drive::MoveHead:
    {
        const Vec3 planar = zm::rotate(yaw_only(head), { axes.x, 0.0f, -axes.z });
        const Vec3 delta  = zm::normalize(Vec3{ planar.x, axes.y, planar.z }) * (config.move_speed * dt);
        for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
        {
            if (drive == SIM_Drive::Walk || d == (u32)SIM_Device::Head)
            {
                sim->devices[d].pose.position += delta;
            }
        }
        break;
    }
    case SIM_Drive::MoveLeft:
    case SIM_Drive::MoveRight:
    {
        const Vec3      view = zm::normalize(Vec3{ axes.x, axes.z, -axes.y });
        SIM_DeviceData& hand = sim->devices[(u32)(drive == SIM_Drive::MoveLeft ? SIM_Device::LeftHand : SIM_Device::RightHand)];
        hand.pose.position += zm::rotate(head, view) * (config.hand_speed * dt);
        break;
    }
    case SIM_Drive::TurnBody:
    case SIM_Drive::TurnHead:
    case SIM_Drive::TurnLeft:
    case SIM_Drive::TurnRight:
    case SIM_Drive::TurnEyes:
        turn_drive(sim, drive, -axes.x * config.turn_speed * dt, axes.z * config.turn_speed * dt);
        break;
    default:
        break;
    }
}

static void wheel_drive (SIM_Sim* sim, SIM_Drive drive, f32 notches)
{
    if (notches == 0.0f)
    {
        return;
    }
    const b8        left = drive == SIM_Drive::PushLeft || drive == SIM_Drive::RollLeft;
    SIM_DeviceData& hand = sim->devices[(u32)(left ? SIM_Device::LeftHand : SIM_Device::RightHand)];
    switch (drive)
    {
    case SIM_Drive::PushLeft:
    case SIM_Drive::PushRight:
        hand.pose.position += zm::rotate(sim->devices[(u32)SIM_Device::Head].pose.orientation, Vec3{ 0.0f, 0.0f, -1.0f }) * (notches * SIM_WHEEL_PUSH);
        break;
    case SIM_Drive::RollLeft:
    case SIM_Drive::RollRight:
        hand.pose.orientation = zm::normalize(hand.pose.orientation * zm::quat_from_axis_angle({ 0.0f, 0.0f, 1.0f }, notches * SIM_WHEEL_ROLL));
        break;
    default:
        break;
    }
}

// The turn keys act only without modifiers, which give them to the thumbsticks' bindings
static void apply_drives (SIM_Sim* sim, u8 modifiers, f32 dt)
{
    const SIM_Config&  config  = sim->config;
    const SIM_Profile& profile = current_profile(sim);
    const b8           shift   = (modifiers & SIM_MOD_SHIFT) != 0;

    const SIM_Drive mouse = shift && profile.shift_mouse != SIM_Drive::None ? profile.shift_mouse : profile.mouse;
    turn_drive(sim, mouse, -sim->mouse_dx * config.look_sensitivity, -sim->mouse_dy * config.look_sensitivity);
    sim->mouse_dx = 0.0f;
    sim->mouse_dy = 0.0f;

    if (modifiers == 0)
    {
        move_drive(sim, profile.ijkl, { key_axis(sim, config.key_turn_right, config.key_turn_left), 0.0f, key_axis(sim, config.key_turn_up, config.key_turn_down) }, dt);
    }
    move_drive(sim, profile.wasd, { key_axis(sim, config.key_move_right, config.key_move_left), key_axis(sim, config.key_move_up, config.key_move_down), key_axis(sim, config.key_move_forward, config.key_move_back) }, dt);

    const SIM_Drive wheel = shift && profile.shift_wheel != SIM_Drive::None ? profile.shift_wheel : profile.wheel;
    wheel_drive(sim, wheel, sim->wheel);
    sim->wheel = 0.0f;
}

static Vec2 move_toward (Vec2 from, Vec2 to, f32 max_step)
{
    const Vec2 delta    = to - from;
    const f32  distance = zm::length(delta);
    if (distance <= max_step)
    {
        return to;
    }
    return from + delta * (max_step / distance);
}

static f32 ramp_step (f32 seconds, f32 dt)
{
    return seconds > 0.0f ? dt / seconds : zm::F32_MAX;
}

static b8 is_analog (SIM_Input input)
{
    return sim_input_type(input) != SIM_InputType::Bool;
}

// Freezing toggles: if anything held is frozen, everything thaws, else every held ramp stops
static void apply_freeze (SIM_Sim* sim, const b8 (&held)[2][(u32)SIM_Input::Count])
{
    b8 any_frozen = false;
    for (u32 h = 0; h < 2; h++)
    {
        for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
        {
            any_frozen = any_frozen || (held[h][i] && sim->frozen[h][i]);
        }
    }
    for (u32 h = 0; h < 2; h++)
    {
        for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
        {
            sim->frozen[h][i] = held[h][i] && is_analog((SIM_Input)i) && !any_frozen;
        }
    }
}

static void hold_binding (const SIM_Sim* sim, const SIM_Binding& binding, Vec2 (&sums)[2][(u32)SIM_Input::Count], b8 (&held)[2][(u32)SIM_Input::Count])
{
    const SIM_Config& config = sim->config;
    SIM_Device        hand   = binding_hand(sim, binding.hand);
    const b8          swipe  = binding.is_slot && binding.slot == SIM_Slot::Stick && sim->devices[(u32)hand].source == SIM_HandSource::Hand;
    SIM_Input         input  = swipe ? swipe_of(binding.value) : (binding.is_slot ? slot_input(sim, hand, binding.slot) : binding.input);
    // The right Touch has no menu button; the menu key presses the left one's
    const SIM_Device other = hand == SIM_Device::LeftHand ? SIM_Device::RightHand : SIM_Device::LeftHand;
    if (binding.is_slot && binding.slot == SIM_Slot::Menu && !hand_input_exists(sim, hand, input) && hand_input_exists(sim, other, slot_input(sim, other, SIM_Slot::Menu)))
    {
        hand  = other;
        input = slot_input(sim, other, SIM_Slot::Menu);
    }
    if (!hand_input_exists(sim, hand, input))
    {
        return;
    }
    const u32 h = hand_index(hand);
    sums[h][(u32)input] += binding.value;
    held[h][(u32)input] = true;

    const SIM_Input touch     = config.auto_touch ? touch_of(input) : SIM_NO_INPUT;
    const SIM_Input proximity = config.auto_touch ? proximity_of(touch != SIM_NO_INPUT ? touch : input) : SIM_NO_INPUT;
    if (hand_input_exists(sim, hand, touch))
    {
        sums[h][(u32)touch] = { 1.0f, 0.0f };
        held[h][(u32)touch] = true;
    }
    if (hand_input_exists(sim, hand, proximity))
    {
        sums[h][(u32)proximity] = { 1.0f, 0.0f };
        held[h][(u32)proximity] = true;
    }
}

// A key cluster that drives a thumbstick holds it like the stick bindings
static void hold_stick_keys (const SIM_Sim* sim, SIM_Drive drive, const OS_Key (&keys)[4], Vec2 (&sums)[2][(u32)SIM_Input::Count], b8 (&held)[2][(u32)SIM_Input::Count])
{
    if (drive != SIM_Drive::LeftStick && drive != SIM_Drive::RightStick)
    {
        return;
    }
    const Vec2 directions[4] = { { 0.0f, 1.0f }, { 0.0f, -1.0f }, { -1.0f, 0.0f }, { 1.0f, 0.0f } };
    for (u32 i = 0; i < 4; i++)
    {
        if (key_down(sim, keys[i]))
        {
            hold_binding(sim, { .key = keys[i], .hand = drive == SIM_Drive::LeftStick ? SIM_BindHand::Left : SIM_BindHand::Right, .is_slot = true, .slot = SIM_Slot::Stick, .value = directions[i] }, sums, held);
        }
    }
}

static void apply_bindings (SIM_Sim* sim, u8 modifiers, f32 dt)
{
    const SIM_Config&  config  = sim->config;
    const SIM_Profile& profile = current_profile(sim);

    Vec2 sums[2][(u32)SIM_Input::Count] = {};
    b8   held[2][(u32)SIM_Input::Count] = {};
    for (u32 i = 0; i < config.bindings_count; i++)
    {
        const SIM_Binding& binding = config.bindings[i];
        if (key_down(sim, binding.key) && binding.modifiers == modifiers)
        {
            hold_binding(sim, binding, sums, held);
        }
    }
    hold_stick_keys(sim, profile.wasd, { config.key_move_forward, config.key_move_back, config.key_move_left, config.key_move_right }, sums, held);
    if (modifiers == 0)
    {
        hold_stick_keys(sim, profile.ijkl, { config.key_turn_up, config.key_turn_down, config.key_turn_left, config.key_turn_right }, sums, held);
    }

    if (sim->freeze_pending)
    {
        sim->freeze_pending = false;
        apply_freeze(sim, held);
    }

    const f32 rise = ramp_step(config.analog_ramp, dt);
    const f32 fall = ramp_step(config.analog_release, dt);
    for (u32 h = 0; h < 2; h++)
    {
        SIM_DeviceData& device = sim->devices[(u32)hand_device(h)];
        for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
        {
            const SIM_Input input = (SIM_Input)i;
            if (is_microgesture(input))
            {
                const u32 g = i - (u32)SIM_Input::SwipeLeft;
                if (held[h][i] && !sim->gesture_keys[h][g])
                {
                    device.inputs[i]        = { .value = { 1.0f, 0.0f } };
                    sim->gesture_seen[h][g] = true;
                }
                sim->gesture_keys[h][g] = held[h][i];
                continue;
            }
            if (!held[h][i])
            {
                sim->frozen[h][i] = false;
            }
            if (!held[h][i] && !sim->key_driven[h][i])
            {
                continue;
            }

            // Keys on two axes of a stick reach its rim, not the corner of the square
            Vec2 target = held[h][i] ? clamp_input(input, { .value = sums[h][i] }).value : Vec2{};
            if (sim_input_type(input) == SIM_InputType::Vec2 && zm::length_sq(sums[h][i]) > 1.0f)
            {
                target = sums[h][i] * (1.0f / zm::length(sums[h][i]));
            }
            if (is_analog(input))
            {
                if (!sim->frozen[h][i])
                {
                    sim->ramped[h][i] = move_toward(sim->ramped[h][i], target, held[h][i] ? rise : fall);
                }
                target = sim->ramped[h][i];
            }

            device.inputs[i]      = clamp_input(input, { .value = target });
            sim->key_driven[h][i] = held[h][i] || !zm::equal(target, Vec2{});
            if (!sim->key_driven[h][i])
            {
                sim->ramped[h][i] = {};
            }
        }
    }
}

static void apply_controls (SIM_Sim* sim, f32 dt)
{
    if (sim->reset_poses_pending)
    {
        sim->reset_poses_pending = false;
        sim->reset_hands_pending = false;
        sim_reset_poses(sim);
    }
    if (sim->reset_hands_pending)
    {
        sim->reset_hands_pending = false;
        sim_reset_hands(sim);
    }
    const u8 modifiers = held_modifiers(sim);
    apply_drives(sim, modifiers, dt);
    apply_bindings(sim, modifiers, dt);
}

static u64 parse_u64 (const char* text)
{
    u64 value = 0;
    for (usize i = 0; text && text[i] >= '0' && text[i] <= '9'; i++)
    {
        value = value * 10 + (u64)(text[i] - '0');
    }
    return value;
}

static Pose origin_offset_pose (f32 yaw_degrees, f32 x, f32 z)
{
    return { { x, 0.0f, z }, zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, yaw_degrees * zm::DEG_TO_RAD) };
}

// "a,b,c" into the list; empty entries are skipped
static void parse_extension_list (const char* text, SIM_Config& config)
{
    config.disabled_extensions_count = 0;
    while (text && *text)
    {
        const char* end = text;
        while (*end && *end != ',')
        {
            end++;
        }
        const usize length = (usize)(end - text);
        if (length > 0 && length < SIM_MAX_EXTENSION_NAME && config.disabled_extensions_count < SIM_MAX_DISABLED_EXTENSIONS)
        {
            char* name = config.disabled_extensions[config.disabled_extensions_count++];
            memory_copy(name, text, length);
            name[length] = 0;
        }
        text = *end ? end + 1 : end;
    }
}

static const char* const sim_graphics_api_names[] = { "vulkan", "opengl", "opengles", "d3d11", "d3d12", "metal" };
static_assert(ARRAY_SIZE(sim_graphics_api_names) == (usize)SIM_GraphicsApi::Count, "a graphics API has no name");

static u32 graphics_api_bit (const char* name, usize length)
{
    for (u32 i = 0; i < ARRAY_SIZE(sim_graphics_api_names); i++)
    {
        if (cstr_length(sim_graphics_api_names[i]) == length && memory_equal(sim_graphics_api_names[i], name, length))
        {
            return 1u << i;
        }
    }
    LOG_ERROR("Config: unknown graphics API \"%.*s\"; known are vulkan, opengl, opengles, d3d11, d3d12, metal", (int)length, name);
    return 0;
}

// A mask that names nothing known allows every API rather than none
static u32 graphics_mask_or_all (u32 mask)
{
    return mask ? mask : SIM_GRAPHICS_ALL;
}

// "a,b" into a mask
static u32 parse_graphics_list (const char* text)
{
    u32 mask = 0;
    while (text && *text)
    {
        const char* end = text;
        while (*end && *end != ',')
        {
            end++;
        }
        if (end > text)
        {
            mask |= graphics_api_bit(text, (usize)(end - text));
        }
        text = *end ? end + 1 : end;
    }
    return graphics_mask_or_all(mask);
}

static void read_vec3 (const JsonValue& array, Vec3& out)
{
    if (!json_is_array(array) || json_length(array) != 3)
    {
        return;
    }
    Vec3 v = {};
    if (json_as_float(json_at(array, 0), v.x) && json_as_float(json_at(array, 1), v.y) && json_as_float(json_at(array, 2), v.z))
    {
        out = v;
    }
}

static void read_quat (const JsonValue& array, Quat& out)
{
    if (!json_is_array(array) || json_length(array) != 4)
    {
        return;
    }
    Quat     q     = {};
    const b8 valid = json_as_float(json_at(array, 0), q.x) && json_as_float(json_at(array, 1), q.y) && json_as_float(json_at(array, 2), q.z) && json_as_float(json_at(array, 3), q.w);
    if (valid && zm::length(q) > 0.0f)
    {
        out = zm::normalize(q);
    }
}

// Props the config or PUPPETXR_PROPS spawn; invalid entries are skipped with a warning
static void read_props (const JsonValue& array, SIM_Config& config)
{
    config.props_count = 0;
    u32 index          = 0;
    for (const JsonValue entry : array)
    {
        SIM_Prop prop = { .kind = SIM_PropKind::Count, .pose = zm::POSE_IDENTITY, .size = 0.1f };
        if (config.props_count == SIM_MAX_PROPS)
        {
            LOG_WARNING("Config: up to %u props, the rest are skipped", SIM_MAX_PROPS);
            break;
        }
        if (sim_prop_from_json(entry, prop))
        {
            config.props[config.props_count++] = prop;
        }
        else
        {
            LOG_WARNING("Config: prop %u is invalid, skipped (docs/props.md)", index);
        }
        index++;
    }
}

static void read_key (const JsonValue& object, const char* name, OS_Key& out)
{
    char key_name[32] = {};
    if (json_find_string(object, name, key_name, nullptr, sizeof(key_name)))
    {
        OS_Key key = {};
        if (os_key_from_name(key_name, key))
        {
            out = key;
        }
        else
        {
            LOG_WARNING("Config: unknown key '%s' for %s", key_name, name);
        }
    }
}

static void read_device (const JsonValue& devices, const char* name, SIM_DeviceConfig& out)
{
    const JsonValue device = json_find(devices, name);
    if (!json_is_object(device))
    {
        return;
    }
    read_vec3(json_find(device, "position"), out.pose.position);
    read_quat(json_find(device, "orientation"), out.pose.orientation);
    json_find_bool(device, "active", out.active, out.active);
}

static void add_binding (SIM_Config& config, const SIM_Binding& binding)
{
    if (config.bindings_count < SIM_MAX_BINDINGS)
    {
        config.bindings[config.bindings_count++] = binding;
    }
}

// The bare key drives the active hand, Shift the left and Ctrl the right
static void add_slot (SIM_Config& config, OS_Key key, SIM_Slot slot, b8 bare = true, Vec2 value = { 1.0f, 0.0f })
{
    if (bare)
    {
        add_binding(config, { .key = key, .modifiers = 0, .hand = SIM_BindHand::Active, .is_slot = true, .slot = slot, .value = value });
    }
    add_binding(config, { .key = key, .modifiers = SIM_MOD_SHIFT, .hand = SIM_BindHand::Left, .is_slot = true, .slot = slot, .value = value });
    add_binding(config, { .key = key, .modifiers = SIM_MOD_CTRL, .hand = SIM_BindHand::Right, .is_slot = true, .slot = slot, .value = value });
}

static b8 modifiers_from_name (const char* name, u8& out_modifiers)
{
    if (name[0] == 0 || cstr_equal(name, "none"))
    {
        out_modifiers = 0;
        return true;
    }
    if (cstr_equal(name, "shift"))
    {
        out_modifiers = SIM_MOD_SHIFT;
        return true;
    }
    if (cstr_equal(name, "ctrl"))
    {
        out_modifiers = SIM_MOD_CTRL;
        return true;
    }
    return false;
}

static b8 bind_hand_from_name (const char* name, SIM_BindHand& out_hand)
{
    if (name[0] == 0 || cstr_equal(name, "active"))
    {
        out_hand = SIM_BindHand::Active;
        return true;
    }
    if (cstr_equal(name, "left"))
    {
        out_hand = SIM_BindHand::Left;
        return true;
    }
    if (cstr_equal(name, "right"))
    {
        out_hand = SIM_BindHand::Right;
        return true;
    }
    return false;
}

static u32 find_profile (const SIM_Config& config, const char* name)
{
    for (u32 i = 0; i < config.profiles_count; i++)
    {
        if (cstr_equal(config.profiles[i].name, name))
        {
            return i;
        }
    }
    return config.profiles_count;
}

static void read_drive (const JsonValue& entry, const char* field, SIM_Drive& out)
{
    char name[32] = {};
    if (json_find_string(entry, field, name, nullptr, sizeof(name)) && !sim_drive_from_name(name, out))
    {
        LOG_WARNING("Config: unknown drive '%s' for %s (docs/config.md, Profiles)", name, field);
    }
}

// An entry of an existing name changes that profile, others are added; `extends` starts from
// another one without its key
static void read_profiles (const JsonValue& profiles, SIM_Config& config)
{
    for (const JsonValue entry : profiles)
    {
        char name[SIM_MAX_PROFILE_NAME] = {};
        if (!json_find_string(entry, "name", name, nullptr, sizeof(name)) || !name[0])
        {
            LOG_WARNING("Config: a profile needs a name of 1 to %u characters", SIM_MAX_PROFILE_NAME - 1);
            continue;
        }
        const u32   index   = find_profile(config, name);
        SIM_Profile profile = index < config.profiles_count ? config.profiles[index] : SIM_Profile{};
        char        base[SIM_MAX_PROFILE_NAME] = {};
        if (json_find_string(entry, "extends", base, nullptr, sizeof(base)))
        {
            const u32 from = find_profile(config, base);
            if (from < config.profiles_count)
            {
                profile     = config.profiles[from];
                profile.key = OS_Key::None;
            }
            else
            {
                LOG_WARNING("Config: profile %s extends the unknown profile %s", name, base);
            }
        }
        cstr_copy(profile.name, sizeof(profile.name), name);
        read_key(entry, "key", profile.key);
        read_drive(entry, "wasd", profile.wasd);
        read_drive(entry, "ijkl", profile.ijkl);
        read_drive(entry, "mouse", profile.mouse);
        read_drive(entry, "shift_mouse", profile.shift_mouse);
        read_drive(entry, "wheel", profile.wheel);
        read_drive(entry, "shift_wheel", profile.shift_wheel);
        char hand[16] = {};
        if (json_find_string(entry, "hand", hand, nullptr, sizeof(hand)) && !bind_hand_from_name(cstr_equal(hand, "primary") ? "" : hand, profile.hand))
        {
            LOG_WARNING("Config: profile %s: hand must be primary, left or right, not '%s'", name, hand);
        }
        if (index < config.profiles_count)
        {
            config.profiles[index] = profile;
        }
        else if (config.profiles_count < SIM_MAX_PROFILES)
        {
            config.profiles[config.profiles_count++] = profile;
        }
        else
        {
            LOG_WARNING("Config: more than %u profiles; %s is ignored", SIM_MAX_PROFILES, name);
        }
    }
}

//
// Body model
//

#define SIM_BODY_REFERENCE_HEIGHT 1.75f
#define SIM_BODY_HAND_JOINTS      26u
#define SIM_BODY_ELBOW_FOLD       (20.0f * zm::DEG_TO_RAD) // the smallest angle between upper arm and forearm
#define SIM_BODY_KNEE_FOLD        (30.0f * zm::DEG_TO_RAD)

// The T-pose of a 1.75 m body, root on the floor at the origin facing -Z, +X to the right: Meta's
// default skeleton, each joint's position and the orientation of its axes (docs/body-tracking.md,
// Joint axes). Retargeting expects these proportions; a body only roughly like them made Meta's
// Movement SDK pull its avatar's elbows apart
static const SIM_BodyRest sim_body_torso_rest[18] = {
    { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } }, // root
    { { 0.0f, 0.923f, 0.0f }, { -0.5f, 0.5f, -0.5f, 0.5f } },
    { { 0.0f, 0.943f, 0.033f }, { -0.489931066f, 0.509870131f, -0.489931066f, 0.509870131f } },
    { { 0.0f, 1.054f, 0.021f }, { -0.489931066f, 0.509870131f, -0.489931066f, 0.509870131f } },
    { { 0.0f, 1.163f, 0.047f }, { -0.489931066f, 0.509870131f, -0.489931066f, 0.509870131f } },
    { { 0.0f, 1.347f, 0.037f }, { -0.489931066f, 0.509870131f, -0.489931066f, 0.509870131f } },
    { { 0.0f, 1.468f, -0.015f }, { -0.567504715f, 0.421827451f, -0.567504715f, 0.421827451f } },
    { { 0.0f, 1.536f, -0.035f }, { -0.5f, 0.5f, -0.5f, 0.5f } },
    { { -0.028f, 1.404f, -0.064f }, { -0.689267138f, 0.182875609f, 0.157831596f, 0.683049421f } },
    { { -0.16f, 1.447f, 0.032f }, { -0.675903444f, 0.154979161f, 0.0194281171f, 0.720248945f } },
    { { -0.18f, 1.421f, 0.033f }, { -0.493261305f, -0.0146754097f, 0.029265346f, 0.869264894f } },
    { { -0.436f, 1.404f, 0.035f }, { -0.492773815f, -0.0263827626f, 0.00861816291f, 0.869714691f } },
    { { -0.667f, 1.394f, 0.029f }, { -0.525596671f, -0.0598102434f, 0.0472539207f, 0.847312187f } },
    { { 0.028f, 1.404f, -0.064f }, { 0.683049421f, -0.157831596f, 0.182875609f, 0.689267138f } },
    { { 0.16f, 1.447f, 0.032f }, { 0.720250236f, -0.0194222051f, 0.154984919f, 0.675900917f } },
    { { 0.18f, 1.421f, 0.033f }, { 0.869264639f, -0.0292729317f, -0.0146711052f, 0.493261434f } },
    { { 0.436f, 1.404f, 0.035f }, { 0.869714691f, -0.00861816291f, -0.0263827626f, 0.492773815f } },
    { { 0.667f, 1.394f, 0.029f }, { 0.88561426f, -0.0517645318f, -0.0559508436f, 0.458123693f } },
};
static const Vec3 sim_body_wrist_rest[2] = { { -0.69f, 1.397f, 0.011f }, { 0.69f, 1.397f, 0.011f } };
static const SIM_BodyRest sim_body_leg_rest[14] = {
    { { -0.08f, 0.898f, 0.005f }, { -0.480717293f, -0.533576595f, -0.482211907f, -0.501675771f } },
    { { -0.066f, 0.479f, 0.028f }, { -0.472724062f, -0.540967035f, -0.476491143f, -0.506796625f } },
    { { -0.049f, 0.061f, 0.069f }, { -0.438758462f, -0.557280245f, -0.506052808f, -0.490754823f } },
    { { -0.049f, 0.061f, 0.069f }, { -0.119068867f, 0.736629312f, 0.123491536f, 0.654178647f } },
    { { -0.053f, 0.026f, 0.063f }, { -0.154590361f, -0.76400384f, -0.333508552f, -0.530256541f } },
    { { -0.056f, 0.037f, 0.003f }, { -0.0225014282f, -0.762119177f, -0.155062876f, -0.628190696f } },
    { { -0.057f, 0.007f, -0.069f }, { -0.0652037577f, 0.785052448f, 0.0547956599f, 0.613545891f } },
    { { 0.08f, 0.898f, 0.005f }, { 0.501675771f, -0.482211907f, 0.533576595f, -0.480717293f } },
    { { 0.066f, 0.479f, 0.028f }, { 0.506796625f, -0.476491143f, 0.540967035f, -0.472724062f } },
    { { 0.049f, 0.061f, 0.069f }, { 0.49074996f, -0.506048979f, 0.557284527f, -0.438762878f } },
    { { 0.049f, 0.061f, 0.069f }, { 0.654178647f, -0.123491536f, 0.736629312f, 0.119068867f } },
    { { 0.053f, 0.026f, 0.063f }, { 0.530256541f, -0.333508552f, 0.76400384f, -0.154590361f } },
    { { 0.056f, 0.037f, 0.003f }, { 0.628190696f, -0.155062876f, 0.762119177f, -0.0225014282f } },
    { { 0.057f, 0.007f, -0.069f }, { 0.613545891f, -0.0547956599f, 0.785052448f, 0.0652037577f } },
};
static const Vec3 SIM_BODY_EYES_IN_HEAD = { 0.0f, 0.064f, -0.09f }; // the view's centre from the head joint, level; standing at the default 1.6 m
static const Vec3 SIM_BODY_ELBOW_POLE   = { 0.5f, -1.0f, 0.5f };    // the right elbow bends down, out and back, in the chest's frame
static const Vec3 SIM_BODY_KNEE_POLE    = { 0.0f, 0.0f, -1.0f };    // in the body's frame

static const char* const sim_body_torso_names[18] = {
    "root", "hips", "spine_lower", "spine_middle", "spine_upper", "chest", "neck", "head",
    "left_shoulder", "left_scapula", "left_arm_upper", "left_arm_lower", "left_hand_wrist_twist",
    "right_shoulder", "right_scapula", "right_arm_upper", "right_arm_lower", "right_hand_wrist_twist",
};
static const char* const sim_body_hand_names[SIM_BODY_HAND_JOINTS] = {
    "left_hand_palm", "left_hand_wrist",
    "left_hand_thumb_metacarpal", "left_hand_thumb_proximal", "left_hand_thumb_distal", "left_hand_thumb_tip",
    "left_hand_index_metacarpal", "left_hand_index_proximal", "left_hand_index_intermediate", "left_hand_index_distal", "left_hand_index_tip",
    "left_hand_middle_metacarpal", "left_hand_middle_proximal", "left_hand_middle_intermediate", "left_hand_middle_distal", "left_hand_middle_tip",
    "left_hand_ring_metacarpal", "left_hand_ring_proximal", "left_hand_ring_intermediate", "left_hand_ring_distal", "left_hand_ring_tip",
    "left_hand_little_metacarpal", "left_hand_little_proximal", "left_hand_little_intermediate", "left_hand_little_distal", "left_hand_little_tip",
};
static const char* const sim_body_right_hand_names[SIM_BODY_HAND_JOINTS] = {
    "right_hand_palm", "right_hand_wrist",
    "right_hand_thumb_metacarpal", "right_hand_thumb_proximal", "right_hand_thumb_distal", "right_hand_thumb_tip",
    "right_hand_index_metacarpal", "right_hand_index_proximal", "right_hand_index_intermediate", "right_hand_index_distal", "right_hand_index_tip",
    "right_hand_middle_metacarpal", "right_hand_middle_proximal", "right_hand_middle_intermediate", "right_hand_middle_distal", "right_hand_middle_tip",
    "right_hand_ring_metacarpal", "right_hand_ring_proximal", "right_hand_ring_intermediate", "right_hand_ring_distal", "right_hand_ring_tip",
    "right_hand_little_metacarpal", "right_hand_little_proximal", "right_hand_little_intermediate", "right_hand_little_distal", "right_hand_little_tip",
};
static const char* const sim_body_leg_names[14] = {
    "left_upper_leg", "left_lower_leg", "left_foot_ankle_twist", "left_foot_ankle", "left_foot_subtalar", "left_foot_transverse", "left_foot_ball",
    "right_upper_leg", "right_lower_leg", "right_foot_ankle_twist", "right_foot_ankle", "right_foot_subtalar", "right_foot_transverse", "right_foot_ball",
};
static const char* const sim_body_pose_names[(u32)SIM_BodyPose::Count] = { "stand", "t_pose", "a_pose", "arms_forward", "sit" };
static const char* const sim_limb_names[(u32)SIM_Limb::Count]           = { "left_arm", "right_arm", "left_leg", "right_leg" };

static b8 body_hand_joint (u32 joint, u32& out_first)
{
    out_first = joint < (u32)SIM_BodyJoint::RightHandPalm ? (u32)SIM_BodyJoint::LeftHandPalm : (u32)SIM_BodyJoint::RightHandPalm;
    return joint >= (u32)SIM_BodyJoint::LeftHandPalm && joint < (u32)SIM_BodyJoint::LeftUpperLeg;
}

// Meta's hierarchy as a Quest 3 reports it: the hand's wrist hangs from the lower arm beside the
// wrist twist, the palm from the wrist, the ankle from the ankle twist
static u8 body_parent (u32 joint)
{
    u32 first = 0;
    if (body_hand_joint(joint, first))
    {
        const u32 local = joint - first;
        if (local == (u32)SIM_HandJoint::Wrist)
        {
            return (u8)(first == (u32)SIM_BodyJoint::LeftHandPalm ? SIM_BodyJoint::LeftArmLower : SIM_BodyJoint::RightArmLower);
        }
        const b8 base = local == (u32)SIM_HandJoint::Palm || local == (u32)SIM_HandJoint::ThumbMetacarpal || local == (u32)SIM_HandJoint::IndexMetacarpal || local == (u32)SIM_HandJoint::MiddleMetacarpal || local == (u32)SIM_HandJoint::RingMetacarpal || local == (u32)SIM_HandJoint::LittleMetacarpal;
        return (u8)(base ? first + (u32)SIM_HandJoint::Wrist : joint - 1);
    }
    switch ((SIM_BodyJoint)joint)
    {
    case SIM_BodyJoint::Root:
        return SIM_BODY_NO_PARENT;
    case SIM_BodyJoint::LeftShoulder:
    case SIM_BodyJoint::RightShoulder:
        return (u8)SIM_BodyJoint::Chest;
    case SIM_BodyJoint::LeftUpperLeg:
    case SIM_BodyJoint::RightUpperLeg:
        return (u8)SIM_BodyJoint::Hips;
    default:
        return (u8)(joint - 1);
    }
}

static Vec3 body_reference (u32 joint)
{
    if (joint < 18)
    {
        return sim_body_torso_rest[joint].position;
    }
    if (joint >= (u32)SIM_BodyJoint::LeftUpperLeg)
    {
        return sim_body_leg_rest[joint - (u32)SIM_BodyJoint::LeftUpperLeg].position;
    }
    return sim_body_wrist_rest[joint == (u32)SIM_BodyJoint::LeftHandWrist ? 0 : 1];
}

// A frame whose X and Y axes point along x and y
static Quat body_axes (Vec3 x, Vec3 y)
{
    return zm::quat_look_rotation(-zm::cross(x, y), y);
}

// Our joint frames are the body's axes in the T-pose; this turns them into Meta's
static Quat body_basis (u32 joint)
{
    if (joint < 18)
    {
        return zm::normalize(sim_body_torso_rest[joint].basis);
    }
    ASSERT(joint >= (u32)SIM_BodyJoint::LeftUpperLeg);
    return zm::normalize(sim_body_leg_rest[joint - (u32)SIM_BodyJoint::LeftUpperLeg].basis);
}

// From XR_EXT_hand_tracking's joint axes (-Z to the tip, +Y the back of the hand) to Meta's body
// hand: +X to the tip and +Y the back on the right hand, +X to the wrist and +Y the palm on the left
static Quat body_hand_basis (b8 left)
{
    return left ? body_axes({ 0.0f, 0.0f, 1.0f }, { 0.0f, -1.0f, 0.0f }) : body_axes({ 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f });
}

// The hand's wrist frame, in XR_EXT_hand_tracking's axes, in the T-pose: fingers out, palm down
static Quat body_hand_rest (b8 left)
{
    return zm::quat_look_rotation({ left ? -1.0f : 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
}

// Normalizing is not idempotent in the last bit, so a rotation of unit length stays as given and
// a saved body loads back bit-exactly; identity for one that is no rotation
static Quat body_unit (Quat rotation)
{
    const f32 length = zm::length(rotation);
    if (!zm::is_finite(length) || length < 0.5f || length > 2.0f)
    {
        return zm::QUAT_IDENTITY;
    }
    return zm::near(length, 1.0f, 1e-6f) ? rotation : zm::normalize(rotation);
}

static b8 body_scalable (u32 joint)
{
    switch ((SIM_BodyJoint)joint)
    {
    case SIM_BodyJoint::Hips:
    case SIM_BodyJoint::SpineLower:
    case SIM_BodyJoint::SpineMiddle:
    case SIM_BodyJoint::SpineUpper:
    case SIM_BodyJoint::Chest:
    case SIM_BodyJoint::Neck:
    case SIM_BodyJoint::LeftShoulder:
    case SIM_BodyJoint::LeftScapula:
    case SIM_BodyJoint::LeftArmUpper:
    case SIM_BodyJoint::LeftArmLower:
    case SIM_BodyJoint::RightShoulder:
    case SIM_BodyJoint::RightScapula:
    case SIM_BodyJoint::RightArmUpper:
    case SIM_BodyJoint::RightArmLower:
    case SIM_BodyJoint::LeftUpperLeg:
    case SIM_BodyJoint::LeftLowerLeg:
    case SIM_BodyJoint::LeftAnkle:
    case SIM_BodyJoint::LeftSubtalar:
    case SIM_BodyJoint::LeftTransverse:
    case SIM_BodyJoint::RightUpperLeg:
    case SIM_BodyJoint::RightLowerLeg:
    case SIM_BodyJoint::RightAnkle:
    case SIM_BodyJoint::RightSubtalar:
    case SIM_BodyJoint::RightTransverse:
        return true;
    default:
        return false;
    }
}

// The T-pose positions of the shape, the lower foot on the floor; the hands' joints in
// XR_EXT_hand_tracking's frames
static void body_rest (const SIM_Sim* sim, const SIM_BodyState& body, Vec3 (&out_positions)[SIM_BODY_JOINT_COUNT], Pose (&out_hands)[2][SIM_HAND_JOINT_COUNT])
{
    const f32 k = body.height / SIM_BODY_REFERENCE_HEIGHT;
    out_positions[0] = {};
    for (u32 j = 1; j < SIM_BODY_JOINT_COUNT; j++)
    {
        u32 first = 0;
        if (body_hand_joint(j, first) && j != first + (u32)SIM_HandJoint::Wrist)
        {
            continue;
        }
        const u32 parent = body_parent(j);
        out_positions[j] = out_positions[parent] + (body_reference(j) - body_reference(parent)) * (k * body.bone_scales[parent]);
    }
    f32 lowest = out_positions[(u32)SIM_BodyJoint::LeftAnkle].y;
    for (u32 j = 0; j < 4; j++)
    {
        lowest = zm::min(lowest, zm::min(out_positions[(u32)SIM_BodyJoint::LeftAnkle + j].y, out_positions[(u32)SIM_BodyJoint::RightAnkle + j].y));
    }
    const Vec3 lift = { 0.0f, k * sim_body_leg_rest[6].position.y - lowest, 0.0f };
    for (u32 j = 1; j < SIM_BODY_JOINT_COUNT; j++)
    {
        out_positions[j] += lift;
    }
    for (u32 h = 0; h < 2; h++)
    {
        const u32                 first = h == 0 ? (u32)SIM_BodyJoint::LeftHandPalm : (u32)SIM_BodyJoint::RightHandPalm;
        const SIM_HandMesh&       mesh  = sim->hand_meshes[h];
        const Pose                wrist = { out_positions[first + (u32)SIM_HandJoint::Wrist], body_hand_rest(h == 0) };
        for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
        {
            out_hands[h][j]          = zm::pose_mul(wrist, mesh.bind_poses[j]);
            out_positions[first + j] = out_hands[h][j].position;
        }
    }
}

// Two bones of lengths a and b from `start` reaching for `target`, the joint between them bending
// toward `pole`; the reach is kept between the fold limit and full length, so the end falls short
// of a target out of reach. out_bend is the unit direction the middle joint moved off the line
static void two_bone_ik (Vec3 start, Vec3 target, Vec3 pole, f32 a, f32 b, f32 fold, Vec3& out_middle, Vec3& out_end, Vec3& out_bend)
{
    const Vec3 to       = target - start;
    const f32  distance = zm::length(to);
    const Vec3 dir      = distance > 1e-6f ? to * (1.0f / distance) : zm::normalize(pole);
    const f32  shortest = zm::sqrt(zm::max(a * a + b * b - 2.0f * a * b * zm::cos(fold), 0.0f));
    const f32  reach    = zm::clamp(distance, zm::max(shortest, 1e-4f), a + b);
    const f32  cosine   = zm::clamp((a * a + reach * reach - b * b) / (2.0f * a * reach), -1.0f, 1.0f);
    const f32  sine     = zm::sqrt(zm::max(1.0f - cosine * cosine, 0.0f));
    Vec3       bend     = pole - dir * zm::dot(pole, dir);
    if (zm::length_sq(bend) < 1e-8f)
    {
        bend = zm::cross(dir, zm::length_sq(zm::cross(dir, { 0.0f, 1.0f, 0.0f })) > 1e-6f ? Vec3{ 0.0f, 1.0f, 0.0f } : Vec3{ 1.0f, 0.0f, 0.0f });
    }
    out_bend   = zm::normalize(bend);
    out_middle = start + (dir * cosine + out_bend * sine) * a;
    out_end    = start + dir * reach;
}

// The rotation that takes a bone along rest_dir, bending in the plane with normal rest_normal, to
// one along dir bending in the plane with normal `normal`
static Quat bone_rotation (Vec3 rest_dir, Vec3 rest_normal, Vec3 dir, Vec3 normal)
{
    return zm::normalize(zm::quat_look_rotation(dir, normal) * zm::inverse(zm::quat_look_rotation(rest_dir, rest_normal)));
}

// The bend plane's normal of a limb from its rest line and pole
static Vec3 limb_normal (Vec3 line, Vec3 pole)
{
    const Vec3 dir  = zm::normalize(line);
    Vec3       bend = pole - dir * zm::dot(pole, dir);
    return zm::normalize(zm::cross(dir, bend));
}

// Whether any joint of the hand starting at `first` past its wrist is rotated; an open hand keeps
// the exact rest hand
static b8 body_fingers_posed (const SIM_BodyState& body, u32 first)
{
    for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
    {
        if (j != (u32)SIM_HandJoint::Wrist && !zm::equal(body.rotations[first + j], zm::QUAT_IDENTITY))
        {
            return true;
        }
    }
    return false;
}

static SIM_Limb body_limb (u32 joint)
{
    u32 first = 0;
    if (body_hand_joint(joint, first))
    {
        return first == (u32)SIM_BodyJoint::LeftHandPalm ? SIM_Limb::LeftArm : SIM_Limb::RightArm;
    }
    if (joint >= (u32)SIM_BodyJoint::LeftArmUpper && joint <= (u32)SIM_BodyJoint::LeftWristTwist)
    {
        return SIM_Limb::LeftArm;
    }
    if (joint >= (u32)SIM_BodyJoint::RightArmUpper && joint <= (u32)SIM_BodyJoint::RightWristTwist)
    {
        return SIM_Limb::RightArm;
    }
    if (joint >= (u32)SIM_BodyJoint::LeftUpperLeg)
    {
        return joint < (u32)SIM_BodyJoint::RightUpperLeg ? SIM_Limb::LeftLeg : SIM_Limb::RightLeg;
    }
    return SIM_Limb::Count;
}

// Rotations of a preset, relative to the T-pose in the body's axes
static void body_preset (SIM_BodyPose pose, Quat (&out_rotations)[SIM_BODY_JOINT_COUNT])
{
    for (Quat& rotation : out_rotations)
    {
        rotation = zm::QUAT_IDENTITY;
    }
    const Vec3 X = { 1.0f, 0.0f, 0.0f };
    const Vec3 Y = { 0.0f, 1.0f, 0.0f };
    const Vec3 Z = { 0.0f, 0.0f, 1.0f };
    f32        down = 0.0f;
    switch (pose)
    {
    case SIM_BodyPose::Stand:
    case SIM_BodyPose::Sit:
        down = 80.0f;
        break;
    case SIM_BodyPose::APose:
        down = 45.0f;
        break;
    case SIM_BodyPose::ArmsForward:
        out_rotations[(u32)SIM_BodyJoint::LeftArmUpper]  = zm::quat_from_axis_angle(Y, -0.5f * zm::PI);
        out_rotations[(u32)SIM_BodyJoint::RightArmUpper] = zm::quat_from_axis_angle(Y, 0.5f * zm::PI);
        break;
    default:
        break;
    }
    if (down != 0.0f)
    {
        out_rotations[(u32)SIM_BodyJoint::LeftArmUpper]  = zm::quat_from_axis_angle(Z, down * zm::DEG_TO_RAD);
        out_rotations[(u32)SIM_BodyJoint::RightArmUpper] = zm::quat_from_axis_angle(Z, -down * zm::DEG_TO_RAD);
    }
    if (pose == SIM_BodyPose::Sit)
    {
        const u32 thighs[2] = { (u32)SIM_BodyJoint::LeftUpperLeg, (u32)SIM_BodyJoint::RightUpperLeg };
        for (const u32 thigh : thighs)
        {
            out_rotations[thigh]     = zm::quat_from_axis_angle(X, 0.5f * zm::PI);
            out_rotations[thigh + 1] = zm::quat_from_axis_angle(X, -0.5f * zm::PI);
        }
    }
}

//
// Face model
//

#define SIM_FACE_BLINK_PERIOD_NS 4000000000ll // a blink every 4 s
#define SIM_FACE_BLINK_CLOSING_NS 60000000ll  // the lids close in 60 ms, stay shut 30 ms, open in 110 ms
#define SIM_FACE_BLINK_SHUT_NS    90000000ll
#define SIM_FACE_BLINK_END_NS     200000000ll

static const char* const sim_face_expression_names[SIM_FACE_EXPRESSION_COUNT] = {
    "brow_lowerer_l", "brow_lowerer_r", "cheek_puff_l", "cheek_puff_r", "cheek_raiser_l", "cheek_raiser_r",
    "cheek_suck_l", "cheek_suck_r", "chin_raiser_b", "chin_raiser_t", "dimpler_l", "dimpler_r",
    "eyes_closed_l", "eyes_closed_r", "eyes_look_down_l", "eyes_look_down_r", "eyes_look_left_l", "eyes_look_left_r",
    "eyes_look_right_l", "eyes_look_right_r", "eyes_look_up_l", "eyes_look_up_r", "inner_brow_raiser_l", "inner_brow_raiser_r",
    "jaw_drop", "jaw_sideways_left", "jaw_sideways_right", "jaw_thrust", "lid_tightener_l", "lid_tightener_r",
    "lip_corner_depressor_l", "lip_corner_depressor_r", "lip_corner_puller_l", "lip_corner_puller_r", "lip_funneler_lb", "lip_funneler_lt",
    "lip_funneler_rb", "lip_funneler_rt", "lip_pressor_l", "lip_pressor_r", "lip_pucker_l", "lip_pucker_r",
    "lip_stretcher_l", "lip_stretcher_r", "lip_suck_lb", "lip_suck_lt", "lip_suck_rb", "lip_suck_rt",
    "lip_tightener_l", "lip_tightener_r", "lips_toward", "lower_lip_depressor_l", "lower_lip_depressor_r", "mouth_left",
    "mouth_right", "nose_wrinkler_l", "nose_wrinkler_r", "outer_brow_raiser_l", "outer_brow_raiser_r", "upper_lid_raiser_l",
    "upper_lid_raiser_r", "upper_lip_raiser_l", "upper_lip_raiser_r", "tongue_tip_interdental", "tongue_tip_alveolar", "tongue_front_dorsal_palate",
    "tongue_mid_dorsal_palate", "tongue_back_dorsal_velar", "tongue_out", "tongue_retreat",
};
static const char* const sim_face_viseme_names[SIM_FACE_VISEME_COUNT] = { "sil", "pp", "ff", "th", "dd", "kk", "ch", "ss", "nn", "rr", "aa", "e", "ih", "oh", "ou" };
static const char* const sim_face_preset_names[(u32)SIM_FacePreset::Count] = { "neutral", "smile", "frown", "surprise", "eyes_closed", "jaw_open" };

// An expression of the left side and its right one after it
static void face_both (f32 (&weights)[SIM_FACE_EXPRESSION_COUNT], SIM_FaceExpression left, f32 weight)
{
    weights[(u32)left]     = weight;
    weights[(u32)left + 1] = weight;
}

static void face_preset (SIM_FacePreset preset, f32 (&out_weights)[SIM_FACE_EXPRESSION_COUNT])
{
    for (f32& weight : out_weights)
    {
        weight = 0.0f;
    }
    switch (preset)
    {
    case SIM_FacePreset::Smile:
        face_both(out_weights, SIM_FaceExpression::LipCornerPullerL, 0.8f);
        face_both(out_weights, SIM_FaceExpression::CheekRaiserL, 0.5f);
        break;
    case SIM_FacePreset::Frown:
        face_both(out_weights, SIM_FaceExpression::LipCornerDepressorL, 0.7f);
        face_both(out_weights, SIM_FaceExpression::BrowLowererL, 0.6f);
        break;
    case SIM_FacePreset::Surprise:
        face_both(out_weights, SIM_FaceExpression::InnerBrowRaiserL, 0.8f);
        face_both(out_weights, SIM_FaceExpression::OuterBrowRaiserL, 0.8f);
        face_both(out_weights, SIM_FaceExpression::UpperLidRaiserL, 0.6f);
        out_weights[(u32)SIM_FaceExpression::JawDrop] = 0.4f;
        break;
    case SIM_FacePreset::EyesClosed:
        face_both(out_weights, SIM_FaceExpression::EyesClosedL, 1.0f);
        break;
    case SIM_FacePreset::JawOpen:
        out_weights[(u32)SIM_FaceExpression::JawDrop] = 0.8f;
        break;
    default:
        break;
    }
}

// How far the lids are shut by the blink schedule at `time`
static f32 face_blink (i64 time)
{
    const i64 phase = ((time % SIM_FACE_BLINK_PERIOD_NS) + SIM_FACE_BLINK_PERIOD_NS) % SIM_FACE_BLINK_PERIOD_NS;
    if (phase < SIM_FACE_BLINK_CLOSING_NS)
    {
        return (f32)phase / (f32)SIM_FACE_BLINK_CLOSING_NS;
    }
    if (phase < SIM_FACE_BLINK_SHUT_NS)
    {
        return 1.0f;
    }
    if (phase < SIM_FACE_BLINK_END_NS)
    {
        return 1.0f - (f32)(phase - SIM_FACE_BLINK_SHUT_NS) / (f32)(SIM_FACE_BLINK_END_NS - SIM_FACE_BLINK_SHUT_NS);
    }
    return 0.0f;
}

//
// Config
//

void sim_default_config (SIM_Config& config)
{
    config = {};

    cstr_copy(config.runtime_name, sizeof(config.runtime_name), "puppetxr");
    cstr_copy(config.room, sizeof(config.room), "living_room");
    cstr_copy(config.control_address, sizeof(config.control_address), "127.0.0.1");
    config.user_id       = 1;
    config.origin_offset = zm::POSE_IDENTITY;
    config.headless      = PUPPETXR_HEADLESS_ONLY != 0;
    config.show_ui          = false;
    config.show_welcome     = true;
    config.replay_exit      = true;
    config.window_width     = 1280;
    config.window_height    = 720;
    config.window_maximized = true;

    config.clock_mode        = SIM_ClockMode::Realtime;
    config.graphics_apis     = SIM_GRAPHICS_ALL;
    config.refresh_rate_hz   = 90.0f;
    config.display_period_ns = 11111111;
    sim_apply_device(config, "quest3");
    config.fov_degrees = 90.0f;
    config.mask_radius = 1.25f;
    config.ipd         = 0.064f;
    config.mirror_view = SIM_MirrorView::Left;

    config.stage_bounds = { 2.0f, 2.0f };

    // Hands held in front with the handle raised so that the aim pose is level
    const Quat held = zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, 60.0f * zm::DEG_TO_RAD);
    config.devices[(u32)SIM_Device::Head]      = { .pose = { { 0.0f, 1.6f, 0.0f }, zm::QUAT_IDENTITY }, .active = true };
    config.devices[(u32)SIM_Device::LeftHand]  = { .pose = { { -0.2f, 1.3f, -0.3f }, held }, .active = true };
    config.devices[(u32)SIM_Device::RightHand] = { .pose = { { 0.2f, 1.3f, -0.3f }, held }, .active = true };
    // The grip turned 60 degrees down, where OVRPlugin's controller pose points, which Meta's
    // samples aim with
    config.aim_offset  = { { 0.0f, 0.0f, -0.05f }, zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -60.0f * zm::DEG_TO_RAD) };
    config.palm_offset = { { 0.0f, 0.0f, 0.0f }, zm::QUAT_IDENTITY };

    config.move_speed           = 1.0f;
    config.look_sensitivity     = 0.003f;
    config.turn_speed           = 0.5f * zm::PI;
    config.primary_hand         = SIM_Device::RightHand;
    config.hand_tracking_fov_degrees = 120.0f;
    sim_default_body(SIM_BODY_REFERENCE_HEIGHT, config.body);
    sim_default_face(config.face);
    config.auto_touch           = true;
    config.keyboard_tracking    = true;
    config.analog_ramp          = 0.5f;
    config.analog_release       = 0.1f;
    config.hand_speed           = 0.3f;
    config.key_move_forward     = OS_Key::W;
    config.key_move_back        = OS_Key::S;
    config.key_move_left        = OS_Key::A;
    config.key_move_right       = OS_Key::D;
    config.key_move_up          = OS_Key::E;
    config.key_move_down        = OS_Key::Q;
    config.key_turn_left        = OS_Key::J;
    config.key_turn_right       = OS_Key::L;
    config.key_turn_up          = OS_Key::I;
    config.key_turn_down        = OS_Key::K;
    config.key_reset_hands      = OS_Key::Digit0;
    config.key_reset            = OS_Key::R;
    config.key_freeze           = OS_Key::Space;
    config.key_hand_source      = OS_Key::T;
    config.key_ui               = OS_Key::Tab;
    config.key_next_profile     = OS_Key::RightBracket;
    config.key_previous_profile = OS_Key::LeftBracket;

    // The ones with a key are cycled; head, left and right are the control modes before profiles
    const SIM_Profile profiles[] = {
        { .name = "body", .key = OS_Key::Digit1, .wasd = SIM_Drive::Walk, .ijkl = SIM_Drive::TurnBody, .mouse = SIM_Drive::TurnBody },
        { .name = "stick", .key = OS_Key::Digit2, .wasd = SIM_Drive::LeftStick, .ijkl = SIM_Drive::RightStick, .mouse = SIM_Drive::TurnBody },
        { .name = "hands", .key = OS_Key::Digit3, .wasd = SIM_Drive::MoveLeft, .ijkl = SIM_Drive::MoveRight, .mouse = SIM_Drive::TurnRight, .shift_mouse = SIM_Drive::TurnLeft, .wheel = SIM_Drive::PushRight, .shift_wheel = SIM_Drive::RollLeft, .hand = SIM_BindHand::Right },
        { .name = "head", .wasd = SIM_Drive::MoveHead, .ijkl = SIM_Drive::TurnHead, .mouse = SIM_Drive::TurnHead },
        { .name = "left", .wasd = SIM_Drive::MoveLeft, .ijkl = SIM_Drive::TurnLeft, .mouse = SIM_Drive::TurnLeft, .hand = SIM_BindHand::Left },
        { .name = "right", .wasd = SIM_Drive::MoveRight, .ijkl = SIM_Drive::TurnRight, .mouse = SIM_Drive::TurnRight, .hand = SIM_BindHand::Right },
        { .name = "eyes", .key = OS_Key::Digit4, .wasd = SIM_Drive::Walk, .ijkl = SIM_Drive::TurnBody, .mouse = SIM_Drive::TurnEyes },
    };
    static_assert(ARRAY_SIZE(profiles) <= SIM_MAX_PROFILES, "too many built-in profiles");
    memory_copy(config.profiles, profiles, sizeof(profiles));
    config.profiles_count = ARRAY_SIZE(profiles);

    add_slot(config, OS_Key::MouseLeft,   SIM_Slot::Trigger);
    add_slot(config, OS_Key::MouseRight,  SIM_Slot::Grip);
    add_slot(config, OS_Key::MouseMiddle, SIM_Slot::Primary);
    add_slot(config, OS_Key::U,           SIM_Slot::Trigger);
    add_slot(config, OS_Key::O,           SIM_Slot::Grip);
    add_slot(config, OS_Key::H,           SIM_Slot::Primary);
    add_slot(config, OS_Key::Y,           SIM_Slot::Secondary);
    add_slot(config, OS_Key::N,           SIM_Slot::StickClick);
    add_slot(config, OS_Key::M,           SIM_Slot::Menu);
    add_slot(config, OS_Key::M,           SIM_Slot::System);
    add_slot(config, OS_Key::I,           SIM_Slot::Stick, false, { 0.0f, 1.0f });
    add_slot(config, OS_Key::K,           SIM_Slot::Stick, false, { 0.0f, -1.0f });
    add_slot(config, OS_Key::J,           SIM_Slot::Stick, false, { -1.0f, 0.0f });
    add_slot(config, OS_Key::L,           SIM_Slot::Stick, false, { 1.0f, 0.0f });
}

b8 sim_parse_config (SIM_Config& config, const char* json)
{
    JsonParser* parser = json_parse_from_string(json);
    if (!parser)
    {
        LOG_ERROR("Config: invalid JSON");
        return false;
    }
    defer { json_release(parser); };

    const JsonValue root = json_get_root(parser);
    if (!json_is_object(root))
    {
        LOG_ERROR("Config: root is not an object");
        return false;
    }

    // The preset first, so the file's own keys win wherever it names it
    char device[sizeof(config.device)] = {};
    if (json_find_string(root, "device", device, nullptr, sizeof(device)) && !config.device_from_env && !sim_apply_device(config, device))
    {
        LOG_WARNING("Config: unknown device '%s', keeping %s", device, config.device);
    }
    char system_name[SIM_MAX_NAME] = {};
    if (json_find_string(root, "system_name", system_name, nullptr, sizeof(system_name)) && system_name[0])
    {
        cstr_copy(config.system_name, sizeof(config.system_name), system_name);
    }
    char runtime_name[SIM_MAX_NAME] = {};
    if (json_find_string(root, "runtime_name", runtime_name, nullptr, sizeof(runtime_name)) && runtime_name[0])
    {
        cstr_copy(config.runtime_name, sizeof(config.runtime_name), runtime_name);
    }
    json_find_bool(root, "accept_time_zero", config.accept_time_zero, config.accept_time_zero);
    json_find_bool(root, "accept_older_frame_time", config.accept_older_frame_time, config.accept_older_frame_time);
    config.discover_before_running_set = json_find_bool(root, "discover_before_running", config.discover_before_running, config.discover_before_running) || config.discover_before_running_set;
    json_find_bool(root, "headless", config.headless, config.headless);
    b8 trace_all = false;
    if (json_find_bool(root, "trace", trace_all))
    {
        cstr_copy(config.trace, sizeof(config.trace), trace_all ? "*" : "");
    }
    else
    {
        json_find_string(root, "trace", config.trace, nullptr, sizeof(config.trace), config.trace);
    }
    json_find_bool(root, "show_ui", config.show_ui, config.show_ui);
    json_find_bool(root, "show_welcome", config.show_welcome, config.show_welcome);
    char controller_name[32] = {};
    if (json_find_string(root, "controller", controller_name, nullptr, sizeof(controller_name)))
    {
        SIM_Controller controller = SIM_Controller::Count;
        if (sim_controller_from_name(controller_name, controller))
        {
            config.controller = controller;
        }
        else
        {
            LOG_WARNING("Config: unknown controller '%s'", controller_name);
        }
    }
    json_find_bool(root, "replay_exit", config.replay_exit, config.replay_exit);

    const JsonValue window = json_find(root, "window");
    json_find_uint(window, "width", config.window_width, config.window_width);
    json_find_bool(window, "maximized", config.window_maximized, config.window_maximized);
    json_find_uint(window, "height", config.window_height, config.window_height);

    const JsonValue clock = json_find(root, "clock");
    char            mode[32] = {};
    if (json_find_string(clock, "mode", mode, nullptr, sizeof(mode)))
    {
        if (cstr_equal(mode, "realtime"))
        {
            config.clock_mode = SIM_ClockMode::Realtime;
        }
        else if (cstr_equal(mode, "fixed_step"))
        {
            config.clock_mode = SIM_ClockMode::FixedStep;
        }
        else
        {
            LOG_WARNING("Config: unknown clock mode '%s'", mode);
        }
    }
    f32 refresh = 0.0f;
    if (json_find_float(clock, "refresh_rate_hz", refresh) && refresh_rate_valid(refresh))
    {
        config.refresh_rate_hz   = refresh;
        config.display_period_ns = refresh_period(refresh);
    }
    const JsonValue rates = json_find(clock, "refresh_rates");
    if (json_is_array(rates))
    {
        config.refresh_rates_count = 0;
        for (u32 i = 0; i < json_length(rates) && config.refresh_rates_count < SIM_MAX_REFRESH_RATES; i++)
        {
            f32 rate = 0.0f;
            if (json_as_float(json_at(rates, i), rate) && refresh_rate_valid(rate))
            {
                config.refresh_rates[config.refresh_rates_count++] = rate;
            }
            else
            {
                LOG_WARNING("Config: clock.refresh_rates[%u] is not a rate in 1..1000 Hz", i);
            }
        }
    }

    const JsonValue views = json_find(root, "views");
    json_find_uint(views, "width", config.view_width, config.view_width);
    json_find_uint(views, "height", config.view_height, config.view_height);
    json_find_float(views, "fov_degrees", config.fov_degrees, config.fov_degrees);
    json_find_float(views, "ipd", config.ipd, config.ipd);
    json_find_float(views, "mask_radius", config.mask_radius, config.mask_radius);
    json_find_bool(views, "lens_mask", config.lens_mask, config.lens_mask);
    char mirror[16] = {};
    if (json_find_string(views, "mirror", mirror, nullptr, sizeof(mirror)))
    {
        if (cstr_equal(mirror, "left"))
        {
            config.mirror_view = SIM_MirrorView::Left;
        }
        else if (cstr_equal(mirror, "right"))
        {
            config.mirror_view = SIM_MirrorView::Right;
        }
        else if (cstr_equal(mirror, "both"))
        {
            config.mirror_view = SIM_MirrorView::Both;
        }
        else
        {
            LOG_WARNING("Config: unknown mirror view '%s'", mirror);
        }
    }

    const JsonValue stage = json_find(root, "stage");
    json_find_float(stage, "width", config.stage_bounds.x, config.stage_bounds.x);
    json_find_float(stage, "depth", config.stage_bounds.y, config.stage_bounds.y);

    const JsonValue devices = json_find(root, "devices");
    read_device(devices, "head", config.devices[(u32)SIM_Device::Head]);
    read_device(devices, "left", config.devices[(u32)SIM_Device::LeftHand]);
    read_device(devices, "right", config.devices[(u32)SIM_Device::RightHand]);

    const JsonValue hands = json_find(root, "hands");
    json_find_float(hands, "tracking_fov_degrees", config.hand_tracking_fov_degrees, config.hand_tracking_fov_degrees);
    config.hand_tracking_fov_degrees = zm::clamp(config.hand_tracking_fov_degrees, 1.0f, 360.0f);

    const JsonValue body_tracking = json_find(root, "body_tracking");
    u64             period_ms     = (u64)config.skeleton_change_period_ns / 1000000;
    json_find_u64(body_tracking, "skeleton_change_period_ms", period_ms, period_ms);
    config.skeleton_change_period_ns = (i64)zm::min(period_ms, (u64)SIM_MAX_SKELETON_CHANGE_PERIOD_MS) * 1000000;

    const JsonValue body = json_find(root, "body");
    if (json_is_valid(body) && !sim_body_apply_json(body, config.body))
    {
        LOG_WARNING("Config: body is ignored: it names an unknown joint, limb or pose, or a value out of range (docs/body-tracking.md)");
    }
    const JsonValue face = json_find(root, "face");
    if (json_is_valid(face) && !sim_face_apply_json(face, config.face))
    {
        LOG_WARNING("Config: face is ignored: it names an unknown expression, viseme or preset, or a value out of range (docs/face-tracking.md)");
    }

    const JsonValue controls = json_find(root, "controls");
    json_find_float(controls, "move_speed", config.move_speed, config.move_speed);
    json_find_float(controls, "look_sensitivity", config.look_sensitivity, config.look_sensitivity);
    json_find_float(controls, "turn_speed", config.turn_speed, config.turn_speed);
    json_find_bool(controls, "auto_touch", config.auto_touch, config.auto_touch);
    json_find_float(controls, "analog_ramp", config.analog_ramp, config.analog_ramp);
    json_find_float(controls, "analog_release", config.analog_release, config.analog_release);
    char primary_hand[16] = {};
    if (json_find_string(controls, "primary_hand", primary_hand, nullptr, sizeof(primary_hand)))
    {
        if (cstr_equal(primary_hand, "left") || cstr_equal(primary_hand, "right"))
        {
            config.primary_hand = cstr_equal(primary_hand, "left") ? SIM_Device::LeftHand : SIM_Device::RightHand;
        }
        else
        {
            LOG_WARNING("Config: primary_hand must be left or right, not '%s'", primary_hand);
        }
    }
    json_find_float(controls, "hand_speed", config.hand_speed, config.hand_speed);
    if (json_is_valid(json_find(controls, "mode_body")) || json_is_valid(json_find(controls, "mode_head")) || json_is_valid(json_find(controls, "mode_left")) || json_is_valid(json_find(controls, "mode_right")))
    {
        LOG_WARNING("Config: controls.mode_* are gone; profiles say what the keys drive and which key picks them (controls.profiles)");
    }
    read_key(controls, "next_profile", config.key_next_profile);
    read_key(controls, "previous_profile", config.key_previous_profile);
    read_profiles(json_find(controls, "profiles"), config);
    read_key(controls, "turn_left", config.key_turn_left);
    read_key(controls, "turn_right", config.key_turn_right);
    read_key(controls, "turn_up", config.key_turn_up);
    read_key(controls, "turn_down", config.key_turn_down);
    read_key(controls, "reset_hands", config.key_reset_hands);
    read_key(controls, "reset", config.key_reset);
    read_key(controls, "freeze", config.key_freeze);
    read_key(controls, "hand_source", config.key_hand_source);
    read_key(controls, "ui", config.key_ui);
    read_key(controls, "move_forward", config.key_move_forward);
    read_key(controls, "move_back", config.key_move_back);
    read_key(controls, "move_left", config.key_move_left);
    read_key(controls, "move_right", config.key_move_right);
    read_key(controls, "move_up", config.key_move_up);
    read_key(controls, "move_down", config.key_move_down);
    if (json_is_valid(json_find(controls, "look")))
    {
        LOG_WARNING("Config: controls.look is gone; a click into the mirror window takes the mouse, Escape gives it back");
    }

    const JsonValue bindings = json_find(controls, "bindings");
    if (json_is_array(bindings))
    {
        SIM_Binding parsed[SIM_MAX_BINDINGS] = {};
        u32         parsed_count             = 0;
        for (const JsonValue entry : bindings)
        {
            char key_name[32]      = {};
            char input_name[32]    = {};
            char slot_name[32]     = {};
            char modifier_name[16] = {};
            char hand_name[16]     = {};
            json_find_string(entry, "key", key_name, nullptr, sizeof(key_name));
            json_find_string(entry, "input", input_name, nullptr, sizeof(input_name));
            json_find_string(entry, "slot", slot_name, nullptr, sizeof(slot_name));
            json_find_string(entry, "modifier", modifier_name, nullptr, sizeof(modifier_name));
            json_find_string(entry, "hand", hand_name, nullptr, sizeof(hand_name));

            SIM_Binding binding = { .is_slot = slot_name[0] != 0, .value = { 1.0f, 0.0f } };
            const b8    target  = binding.is_slot ? sim_slot_from_name(slot_name, binding.slot) : sim_input_from_name(input_name, binding.input);
            if (!os_key_from_name(key_name, binding.key) || !target || !modifiers_from_name(modifier_name, binding.modifiers) || !bind_hand_from_name(hand_name, binding.hand))
            {
                LOG_WARNING("Config: ignoring binding '%s' -> '%s%s'", key_name, slot_name, input_name);
                continue;
            }
            const JsonValue value = json_find(entry, "value");
            if (json_is_array(value) && json_length(value) == 2)
            {
                json_as_float(json_at(value, 0), binding.value.x, 1.0f);
                json_as_float(json_at(value, 1), binding.value.y, 0.0f);
            }
            if (parsed_count < SIM_MAX_BINDINGS)
            {
                parsed[parsed_count++] = binding;
            }
        }
        if (parsed_count > 0)
        {
            memory_copy(config.bindings, parsed, sizeof(parsed));
            config.bindings_count = parsed_count;
        }
    }

    u32 frame = 0;
    if (json_find_uint(root, "screenshot_at_frame", frame))
    {
        config.screenshot_at_frame = frame;
    }
    if (json_find_uint(root, "exit_at_frame", frame))
    {
        config.exit_at_frame = frame;
    }

    json_find_string(root, "record", config.record_path, nullptr, sizeof(config.record_path), config.record_path);
    json_find_string(root, "replay", config.replay_path, nullptr, sizeof(config.replay_path), config.replay_path);
    json_find_string(root, "screenshot_dir", config.screenshot_dir, nullptr, sizeof(config.screenshot_dir), config.screenshot_dir);
    json_find_string(root, "profile", config.profile_path, nullptr, sizeof(config.profile_path), config.profile_path);
    json_find_string(root, "rooms_dir", config.rooms_dir, nullptr, sizeof(config.rooms_dir), config.rooms_dir);
    config.room_explicit = config.room_explicit || json_is_valid(json_find(root, "room"));
    if (json_find_string(root, "room", config.room, nullptr, sizeof(config.room), config.room) && cstr_equal(config.room, "none"))
    {
        config.room[0] = 0;
    }

    const JsonValue control = json_find(root, "control");
    if (json_is_object(control))
    {
        json_find_bool(control, "enabled", config.control, config.control);
        json_find_uint(control, "port", config.control_port, config.control_port);
        json_find_string(control, "address", config.control_address, nullptr, sizeof(config.control_address), config.control_address);
        json_find_bool(control, "paused", config.control_paused, config.control_paused);
    }

    const JsonValue graphics = json_find(root, "graphics");
    if (json_is_string(graphics) || json_is_array(graphics))
    {
        u32  mask       = 0;
        char name[16]   = {};
        usize length    = 0;
        if (json_is_string(graphics))
        {
            mask = json_as_string(graphics, name, &length, sizeof(name)) ? graphics_api_bit(name, length) : 0;
        }
        for (const JsonValue entry : graphics)
        {
            if (json_as_string(entry, name, &length, sizeof(name)))
            {
                mask |= graphics_api_bit(name, length);
            }
        }
        config.graphics_apis = graphics_mask_or_all(mask);
    }

    const JsonValue disabled = json_find(root, "disabled_extensions");
    if (json_is_array(disabled))
    {
        config.disabled_extensions_count = 0;
        for (const JsonValue name : disabled)
        {
            char* out = config.disabled_extensions[config.disabled_extensions_count];
            if (config.disabled_extensions_count < SIM_MAX_DISABLED_EXTENSIONS && json_as_string(name, out, nullptr, SIM_MAX_EXTENSION_NAME) && out[0])
            {
                config.disabled_extensions_count++;
            }
            else
            {
                LOG_WARNING("Config: disabled_extensions takes up to %u extension names", SIM_MAX_DISABLED_EXTENSIONS);
            }
        }
    }

    const JsonValue offset = json_find(root, "origin_offset");
    char            offset_mode[16] = {};
    if (json_as_string(offset, offset_mode, nullptr, sizeof(offset_mode)))
    {
        if (cstr_equal(offset_mode, "per_run"))
        {
            config.origin_offset_mode = SIM_OriginOffset::PerRun;
        }
        else
        {
            if (!cstr_equal(offset_mode, "none"))
            {
                LOG_WARNING("Config: origin_offset is \"none\", \"per_run\" or an object, not '%s'", offset_mode);
            }
            config.origin_offset_mode = SIM_OriginOffset::None;
        }
    }
    else if (json_is_object(offset))
    {
        f32             yaw      = 0.0f;
        const JsonValue position = json_find(offset, "position");
        f32             xz[2]    = {};
        json_find_float(offset, "yaw_degrees", yaw);
        if (json_is_array(position) && json_length(position) == 2)
        {
            json_as_float(json_at(position, 0), xz[0]);
            json_as_float(json_at(position, 1), xz[1]);
        }
        config.origin_offset_mode = SIM_OriginOffset::Fixed;
        config.origin_offset      = origin_offset_pose(yaw, xz[0], xz[1]);
    }
    json_find_u64(root, "origin_seed", config.origin_seed, config.origin_seed);

    const JsonValue drift = json_find(root, "drift_rate");
    if (json_is_object(drift))
    {
        f32             yaw_rate = config.drift_yaw_rate;
        const JsonValue velocity = json_find(drift, "velocity");
        f32             xyz[3]   = {};
        if (json_find_float(drift, "yaw_degrees", yaw_rate, config.drift_yaw_rate) && zm::is_finite(yaw_rate))
        {
            config.drift_yaw_rate = yaw_rate;
        }
        if (json_is_array(velocity) && json_length(velocity) == 3 && json_as_float(json_at(velocity, 0), xyz[0]) && json_as_float(json_at(velocity, 1), xyz[1]) && json_as_float(json_at(velocity, 2), xyz[2]))
        {
            config.drift_velocity = { xyz[0], xyz[1], xyz[2] };
        }
        else if (json_is_valid(velocity))
        {
            LOG_WARNING("Config: drift_rate.velocity is [x, y, z] in m/s");
        }
    }

    json_find_bool(json_find(root, "passthrough"), "default_active", config.passthrough_default_active, config.passthrough_default_active);
    json_find_bool(json_find(root, "keyboard_tracking"), "enabled", config.keyboard_tracking, config.keyboard_tracking);

    const JsonValue props = json_find(root, "props");
    if (json_is_array(props))
    {
        read_props(props, config);
    }

    const JsonValue anchors = json_find(root, "anchors");
    if (json_is_object(anchors))
    {
        json_find_string(anchors, "dir", config.anchors_dir, nullptr, sizeof(config.anchors_dir), config.anchors_dir);
        json_find_string(anchors, "cloud_dir", config.cloud_dir, nullptr, sizeof(config.cloud_dir), config.cloud_dir);
        json_find_string(anchors, "app_id", config.app_id, nullptr, sizeof(config.app_id), config.app_id);
        u64 user = 0;
        if (json_find_u64(anchors, "user_id", user) && user != 0)
        {
            config.user_id = user;
        }
        json_find_uint(anchors, "latency_frames", config.anchor_latency_frames, config.anchor_latency_frames);
        json_find_uint(anchors, "localize_frames", config.anchor_localize_frames, config.anchor_localize_frames);
    }

#if PUPPETXR_HEADLESS_ONLY
    config.headless = true;
#endif
    return true;
}

b8 sim_load_config (SIM_Config& config, const char* path)
{
    u8*   data = nullptr;
    usize size = 0;
    if (!os_read_file(path, &data, &size))
    {
        LOG_ERROR("Config: cannot read %s", path ? path : "(null)");
        return false;
    }
    defer { os_memory_release(data); };

    char* text = (char*)os_memory_alloc(size + 1);
    defer { os_memory_release(text); };
    memory_copy(text, data, size);
    return sim_parse_config(config, text);
}

b8 sim_prop_from_json (const JsonValue& value, SIM_Prop& inout_prop)
{
    char     kind[16]   = {};
    char     motion[16] = {};
    SIM_Prop prop       = inout_prop;
    if (!json_is_object(value) || (json_find_string(value, "kind", kind, nullptr, sizeof(kind)) && !sim_prop_kind_from_name(kind, prop.kind)))
    {
        return false;
    }
    if (json_find_string(value, "motion", motion, nullptr, sizeof(motion)) && !sim_prop_motion_from_name(motion, prop.motion))
    {
        return false;
    }
    const JsonValue text = json_find(value, "text");
    if (json_is_valid(text) && !json_as_string(text, prop.text, nullptr, sizeof(prop.text)))
    {
        return false;
    }
    read_vec3(json_find(value, "position"), prop.pose.position);
    read_quat(json_find(value, "orientation"), prop.pose.orientation);
    f32 yaw = 0.0f;
    if (json_find_float(value, "yaw_degrees", yaw))
    {
        prop.pose.orientation = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, yaw * zm::DEG_TO_RAD);
    }
    read_vec3(json_find(value, "target"), prop.target);
    json_find_float(value, "radius", prop.radius, prop.radius);
    json_find_float(value, "speed", prop.speed, prop.speed);
    json_find_float(value, "size", prop.size, prop.size);
    return normalize_prop(prop, inout_prop);
}

void sim_apply_env (SIM_Config& config)
{
    if (const char* value = os_env("PUPPETXR_HEADLESS"); value && !cstr_equal(value, "0"))
    {
        config.headless = true;
    }
    if (const char* value = os_env("PUPPETXR_RUNTIME_NAME"); value && value[0])
    {
        cstr_copy(config.runtime_name, sizeof(config.runtime_name), value);
    }
    if (const char* value = os_env("PUPPETXR_LENS_MASK"); value && value[0])
    {
        config.lens_mask = !cstr_equal(value, "0");
    }
    if (const char* value = os_env("PUPPETXR_ACCEPT_TIME_ZERO"); value && value[0])
    {
        config.accept_time_zero = !cstr_equal(value, "0");
    }
    if (const char* value = os_env("PUPPETXR_ACCEPT_OLDER_FRAME_TIME"); value && value[0])
    {
        config.accept_older_frame_time = !cstr_equal(value, "0");
    }
    if (const char* value = os_env("PUPPETXR_DISCOVER_BEFORE_RUNNING"); value && value[0])
    {
        config.discover_before_running     = !cstr_equal(value, "0");
        config.discover_before_running_set = true;
    }
    if (const char* value = os_env("PUPPETXR_TRACE"); value && value[0])
    {
        const b8 all = cstr_equal(value, "1") || cstr_equal(value, "true");
        cstr_copy(config.trace, sizeof(config.trace), cstr_equal(value, "0") ? "" : all ? "*" : value);
    }
    if (const char* value = os_env("PUPPETXR_SHOW_WELCOME"))
    {
        config.show_welcome = !cstr_equal(value, "0");
    }
    if (const char* value = os_env("PUPPETXR_FIXED_STEP"); value && !cstr_equal(value, "0"))
    {
        config.clock_mode = SIM_ClockMode::FixedStep;
    }
    if (const char* value = os_env("PUPPETXR_PROFILE"))
    {
        cstr_copy(config.profile_path, sizeof(config.profile_path), value);
    }
    if (const char* value = os_env("PUPPETXR_RECORD"))
    {
        cstr_copy(config.record_path, sizeof(config.record_path), value);
    }
    if (const char* value = os_env("PUPPETXR_REPLAY"))
    {
        cstr_copy(config.replay_path, sizeof(config.replay_path), value);
    }
    if (const char* value = os_env("PUPPETXR_REPLAY_EXIT"))
    {
        config.replay_exit = !cstr_equal(value, "0");
    }
    if (const char* value = os_env("PUPPETXR_SCREENSHOT_DIR"))
    {
        cstr_copy(config.screenshot_dir, sizeof(config.screenshot_dir), value);
    }
    if (const char* value = os_env("PUPPETXR_SCREENSHOT_AT"))
    {
        config.screenshot_at_frame = parse_u64(value);
    }
    if (const char* value = os_env("PUPPETXR_EXIT_AT"))
    {
        config.exit_at_frame = parse_u64(value);
    }
    if (const char* value = os_env("PUPPETXR_ROOM"))
    {
        cstr_copy(config.room, sizeof(config.room), cstr_equal(value, "none") ? "" : value);
        config.room_explicit = true;
    }
    if (const char* value = os_env("PUPPETXR_ROOMS_DIR"))
    {
        cstr_copy(config.rooms_dir, sizeof(config.rooms_dir), value);
    }
    if (const char* value = os_env("PUPPETXR_CONTROL"))
    {
        config.control = !cstr_equal(value, "0");
    }
    if (const char* value = os_env("PUPPETXR_CONTROL_PORT"))
    {
        config.control_port = (u32)parse_u64(value);
    }
    if (const char* value = os_env("PUPPETXR_CONTROL_ADDRESS"))
    {
        cstr_copy(config.control_address, sizeof(config.control_address), value);
    }
    if (const char* value = os_env("PUPPETXR_CONTROL_PAUSED"))
    {
        config.control_paused = !cstr_equal(value, "0");
    }
    if (const char* value = os_env("PUPPETXR_DISABLE_EXTENSIONS"))
    {
        parse_extension_list(value, config);
    }
    if (const char* value = os_env("PUPPETXR_GRAPHICS"))
    {
        config.graphics_apis = parse_graphics_list(value);
    }
    if (const char* value = os_env("PUPPETXR_DRIFT_RATE"))
    {
        char array[96];
        cstr_format(array, sizeof(array), "[%s]", value);
        JsonParser*     parser = json_parse_from_string(array);
        const JsonValue values = parser ? json_get_root(parser) : JsonValue{};
        f32             rate[4] = {};
        b8              parsed  = json_is_array(values) && json_length(values) == 4;
        for (u32 i = 0; parsed && i < 4; i++)
        {
            parsed = json_as_float(json_at(values, i), rate[i]) && zm::is_finite(rate[i]);
        }
        if (parsed)
        {
            config.drift_yaw_rate = rate[0];
            config.drift_velocity = { rate[1], rate[2], rate[3] };
        }
        else
        {
            LOG_WARNING("PUPPETXR_DRIFT_RATE is <yaw degrees/s>,<x>,<y>,<z m/s>, not '%s'", value);
        }
        json_release(parser);
    }
    if (const char* value = os_env("PUPPETXR_ORIGIN_OFFSET"))
    {
        if (cstr_equal(value, "per_run"))
        {
            config.origin_offset_mode = SIM_OriginOffset::PerRun;
        }
        else if (cstr_equal(value, "none"))
        {
            config.origin_offset_mode = SIM_OriginOffset::None;
        }
        else
        {
            char array[96];
            cstr_format(array, sizeof(array), "[%s]", value);
            JsonParser*     parser = json_parse_from_string(array);
            const JsonValue values = parser ? json_get_root(parser) : JsonValue{};
            f32             yaw_x_z[3] = {};
            if (json_is_array(values) && json_length(values) == 3 && json_as_float(json_at(values, 0), yaw_x_z[0]) && json_as_float(json_at(values, 1), yaw_x_z[1]) && json_as_float(json_at(values, 2), yaw_x_z[2]))
            {
                config.origin_offset_mode = SIM_OriginOffset::Fixed;
                config.origin_offset      = origin_offset_pose(yaw_x_z[0], yaw_x_z[1], yaw_x_z[2]);
            }
            else
            {
                LOG_WARNING("PUPPETXR_ORIGIN_OFFSET is none, per_run or <yaw degrees>,<x>,<z>, not '%s'", value);
            }
            json_release(parser);
        }
    }
    if (const char* value = os_env("PUPPETXR_KEYBOARD_TRACKING"); value && value[0])
    {
        config.keyboard_tracking = !cstr_equal(value, "0");
    }
    if (const char* value = os_env("PUPPETXR_PROPS"))
    {
        JsonParser* parser = json_parse_from_string(value);
        if (parser && json_is_array(json_get_root(parser)))
        {
            read_props(json_get_root(parser), config);
        }
        else
        {
            LOG_WARNING("PUPPETXR_PROPS is a JSON array of props (docs/props.md)");
        }
        json_release(parser);
    }
    if (const char* value = os_env("PUPPETXR_HAND_TRACKING_FOV"); value && parse_u64(value) > 0)
    {
        config.hand_tracking_fov_degrees = (f32)zm::min(parse_u64(value), (u64)360);
    }
    if (const char* value = os_env("PUPPETXR_SKELETON_CHANGE_PERIOD_MS"); value && value[0])
    {
        config.skeleton_change_period_ns = (i64)zm::min(parse_u64(value), (u64)SIM_MAX_SKELETON_CHANGE_PERIOD_MS) * 1000000;
    }
    if (const char* value = os_env("PUPPETXR_ORIGIN_SEED"))
    {
        config.origin_seed = parse_u64(value);
    }
    if (const char* value = os_env("PUPPETXR_ANCHORS_DIR"))
    {
        cstr_copy(config.anchors_dir, sizeof(config.anchors_dir), value);
    }
    if (const char* value = os_env("PUPPETXR_CLOUD_DIR"))
    {
        cstr_copy(config.cloud_dir, sizeof(config.cloud_dir), value);
    }
    if (const char* value = os_env("PUPPETXR_APP_ID"))
    {
        cstr_copy(config.app_id, sizeof(config.app_id), value);
    }
    if (const char* value = os_env("PUPPETXR_USER_ID"); value && parse_u64(value) != 0)
    {
        config.user_id = parse_u64(value);
    }
#if PUPPETXR_HEADLESS_ONLY
    config.headless = true;
#endif
}

b8 sim_find_config_path (char* out_path, usize out_size)
{
    const char* env = os_env("PUPPETXR_CONFIG");
    if (env && os_file_exists(env))
    {
        return cstr_copy(out_path, out_size, env);
    }
    if (os_file_exists("puppetxr.json"))
    {
        return cstr_copy(out_path, out_size, "puppetxr.json");
    }
    out_path[0] = 0;
    return false;
}

b8 sim_apply_device (SIM_Config& config, const char* name)
{
    for (const SIM_DevicePreset& preset : sim_device_presets)
    {
        if (!name || !cstr_equal(preset.name, name))
        {
            continue;
        }
        cstr_copy(config.device, sizeof(config.device), preset.name);
        cstr_copy(config.system_name, sizeof(config.system_name), preset.system_name);
        config.vendor_id   = SIM_META_VENDOR_ID;
        memory_copy(config.headset_id, preset.headset_id, sizeof(config.headset_id));
        config.controller  = preset.controller;
        config.view_width  = preset.view_width;
        config.view_height = preset.view_height;
        memory_copy(config.refresh_rates, preset.refresh_rates, sizeof(config.refresh_rates));
        config.refresh_rates_count = preset.refresh_rates_count;
        return true;
    }
    return false;
}

u32 sim_device_count ()
{
    return ARRAY_SIZE(sim_device_presets);
}

const char* sim_device_name (u32 index)
{
    return index < ARRAY_SIZE(sim_device_presets) ? sim_device_presets[index].name : nullptr;
}

const char* sim_apply_known_modules (SIM_Config& config, b8 (*loaded)(const char* module))
{
    const char* first = nullptr;
    for (const SIM_KnownModule& known : sim_known_modules)
    {
        if (!loaded(known.module))
        {
            continue;
        }
        if (known.runtime_name)
        {
            cstr_copy(config.runtime_name, sizeof(config.runtime_name), known.runtime_name);
        }
        config.accept_time_zero        = config.accept_time_zero || known.accept_time_zero;
        config.accept_older_frame_time = config.accept_older_frame_time || known.accept_older_frame_time;
        config.discover_before_running = config.discover_before_running || known.discover_before_running;
        first                          = first ? first : known.module;
    }
    return first;
}

const char* sim_late_discovery_module (const SIM_Config& config, b8 (*loaded)(const char* module))
{
    if (config.discover_before_running_set)
    {
        return nullptr;
    }
    for (const SIM_KnownModule& known : sim_known_modules)
    {
        if (known.discover_before_running && loaded(known.module))
        {
            return known.module;
        }
    }
    return nullptr;
}

void sim_load_full_config (SIM_Config& out_config)
{
    sim_default_config(out_config);
    if (const char* device = os_env("PUPPETXR_DEVICE"); device && device[0])
    {
        out_config.device_from_env = sim_apply_device(out_config, device);
        if (!out_config.device_from_env)
        {
            LOG_WARNING("PUPPETXR_DEVICE: unknown device '%s', keeping %s", device, out_config.device);
        }
    }
    out_config.known_module = sim_apply_known_modules(out_config, os_module_loaded);
    char path[OS_MAX_PATH] = {};
    if (sim_find_config_path(path, sizeof(path)))
    {
        sim_load_config(out_config, path);
    }
    sim_apply_env(out_config);
}

b8 sim_graphics_api_allowed (const SIM_Config& config, SIM_GraphicsApi api)
{
    return (u32)api < (u32)SIM_GraphicsApi::Count && (config.graphics_apis & (1u << (u32)api)) != 0;
}

const char* sim_graphics_api_name (SIM_GraphicsApi api)
{
    return (u32)api < ARRAY_SIZE(sim_graphics_api_names) ? sim_graphics_api_names[(u32)api] : "unknown";
}

b8 sim_extension_disabled (const SIM_Config& config, const char* name)
{
    for (u32 i = 0; i < config.disabled_extensions_count; i++)
    {
        if (cstr_equal(config.disabled_extensions[i], name))
        {
            return true;
        }
    }
    return false;
}

b8 sim_parse_state (const char* json, SIM_State& out_state)
{
    out_state = {};
    JsonParser* parser = json_parse_from_string(json);
    if (!parser)
    {
        return false;
    }
    defer { json_release(parser); };

    const JsonValue root = json_get_root(parser);
    if (!json_is_object(root))
    {
        return false;
    }
    out_state.has_room = json_find_string(root, "room", out_state.room, nullptr, sizeof(out_state.room));
    if (!out_state.has_room || cstr_equal(out_state.room, "none"))
    {
        out_state.room[0] = 0;
    }
    json_find_bool(root, "welcome_dismissed", out_state.welcome_dismissed, false);
    return true;
}

b8 sim_load_state (const char* path, SIM_State& out_state)
{
    out_state  = {};
    u8*   data = nullptr;
    usize size = 0;
    if (!os_read_file(path, &data, &size))
    {
        return false;
    }
    defer { os_memory_release(data); };

    char* text = (char*)os_memory_alloc(size + 1);
    defer { os_memory_release(text); };
    memory_copy(text, data, size);
    return sim_parse_state(text, out_state);
}

// Room names are checked when the room loads; here they only must not break the JSON
b8 sim_save_state (const char* path, const SIM_State& state)
{
    for (const char* c = state.room; *c; c++)
    {
        if (*c == '"' || *c == '\\' || (u8)*c < 0x20)
        {
            return false;
        }
    }
    char room[SIM_MAX_NAME + 16] = {};
    if (state.has_room)
    {
        cstr_format(room, sizeof(room), "\"room\": \"%s\"", state.room[0] ? state.room : "none");
    }
    const char* welcome = state.welcome_dismissed ? "\"welcome_dismissed\": true" : "";
    char        text[160];
    cstr_format(text, sizeof(text), "{ %s%s%s }\n", room, room[0] && welcome[0] ? ", " : "", welcome);
    return os_write_file(path, text, cstr_length(text));
}

void sim_apply_state (SIM_Config& config, const SIM_State& state)
{
    if (config.headless || !state.has_room || config.room_explicit)
    {
        return;
    }
    cstr_copy(config.room, sizeof(config.room), state.room);
    LOG_INFO("Config: room '%s' from the last choice in the UI", state.room[0] ? state.room : "none");
}

//
// Lifecycle and frames
//

SIM_Sim* sim_create (const SIM_Config& config)
{
    SIM_Sim* sim          = (SIM_Sim*)os_memory_alloc(sizeof(SIM_Sim));
    sim->config           = config;
    sim->clock_mode       = config.clock_mode;
    sim->time             = SIM_TIME_START;
    sim->display_period   = config.display_period_ns > 0 ? config.display_period_ns : 11111111;
    sim->refresh_rate         = refresh_rate_valid(config.refresh_rate_hz) ? config.refresh_rate_hz : 90.0f;
    sim->default_refresh_rate = sim->refresh_rate;
    add_refresh_rate(sim, sim->refresh_rate);
    for (u32 i = 0; i < config.refresh_rates_count && i < SIM_MAX_REFRESH_RATES; i++)
    {
        add_refresh_rate(sim, config.refresh_rates[i]);
    }
    sim->controls_enabled = true;
    sim->profile          = 0;
    sim->controller       = (u32)config.controller < (u32)SIM_Controller::Count ? config.controller : SIM_Controller::MetaTouchPlus;

    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        SIM_DeviceData& device = sim->devices[i];
        device.pose            = zm::is_finite(config.devices[i].pose) && zm::length(config.devices[i].pose.orientation) > 0.0f ? config.devices[i].pose : zm::POSE_IDENTITY;
        device.pose.orientation = zm::normalize(device.pose.orientation);
        device.previous_pose   = device.pose;
        device.active          = config.devices[i].active;
    }

    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        sim->start_poses[i] = sim->devices[i].pose;
    }
    sim_default_body(config.body.height, sim->body);
    sim_set_body(sim, config.body);
    sim->body_generation = 0;
    sim_set_skeleton_change_period(sim, config.skeleton_change_period_ns);
    sim_set_face(sim, config.face);
    const Pose head = level_frame(sim->devices[(u32)SIM_Device::Head].pose);
    for (u32 h = 0; h < 2; h++)
    {
        sim->hand_offsets[h] = zm::pose_relative(head, sim->devices[(u32)hand_device(h)].pose);
    }

    sim->next_prop_id = 1;
    for (u32 i = 0; i < config.props_count && i < SIM_MAX_PROPS; i++)
    {
        if (sim_add_prop(sim, config.props[i]) == 0)
        {
            LOG_WARNING("Config: prop %u is invalid", i);
        }
    }

    sim->gesture_kind[0] = SIM_NO_INPUT;
    sim->gesture_kind[1] = SIM_NO_INPUT;
    build_hand_mesh(sim->hand_meshes[0], true);
    build_hand_mesh(sim->hand_meshes[1], false);

    sim->tracking_drift = zm::POSE_IDENTITY;
    sim->drift_velocity = zm::is_finite(config.drift_velocity.x) && zm::is_finite(config.drift_velocity.y) && zm::is_finite(config.drift_velocity.z) ? config.drift_velocity : Vec3{};
    sim->drift_yaw_rate = zm::is_finite(config.drift_yaw_rate) ? config.drift_yaw_rate : 0.0f;

    sim_recenter(sim);
    sim->previous_local_origin = sim->local_origin;
    sim->stationary_origin     = sim->local_origin;
    return sim;
}

void sim_destroy (SIM_Sim* sim)
{
    os_memory_release(sim);
}

const SIM_Config& sim_get_config (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->config;
}

void sim_start_clock (SIM_Sim* sim, u64 os_time_ns)
{
    ASSERT(sim);
    if (!sim->has_last_os_time)
    {
        sim->last_os_time     = os_time_ns;
        sim->has_last_os_time = true;
    }
}

b8 sim_tracking_drift_valid (const Pose& drift)
{
    const Quat q = drift.orientation;
    return zm::is_finite(drift) && zm::length(q) >= 0.5f && zm::length(q) <= 2.0f && zm::sqrt(q.x * q.x + q.z * q.z) <= SIM_DRIFT_MAX_TILT * zm::length(q);
}

static void advance_drift (SIM_Sim* sim, f32 dt)
{
    if (sim->drift_yaw_rate == 0.0f && zm::equal(sim->drift_velocity, Vec3{}))
    {
        return;
    }
    const Quat turn                 = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, sim->drift_yaw_rate * zm::DEG_TO_RAD * dt);
    sim->tracking_drift.position   += sim->drift_velocity * dt;
    sim->tracking_drift.orientation = zm::normalize(turn * sim->tracking_drift.orientation);
}

void sim_begin_frame (SIM_Sim* sim, u64 os_time_ns)
{
    ASSERT(sim);

    i64 dt = 0;
    if (sim->clock_mode == SIM_ClockMode::FixedStep)
    {
        dt = sim->display_period;
    }
    else if (sim->has_last_os_time && os_time_ns > sim->last_os_time)
    {
        dt = zm::min((i64)(os_time_ns - sim->last_os_time), SIM_MAX_REALTIME_DT);
    }
    sim->last_os_time     = os_time_ns;
    sim->has_last_os_time = true;
    sim->last_dt          = dt;
    sim->time += dt;

    advance_drift(sim, (f32)dt * 1e-9f);
    age_microgestures(sim);
    memory_copy(sim->typed, sim->typed_pending, sim->typed_pending_length);
    sim->typed_length         = sim->typed_pending_length;
    sim->typed_pending_length = 0;
    if (sim->controls_enabled)
    {
        apply_controls(sim, (f32)dt * 1e-9f);
    }
    else
    {
        sim->mouse_dx = 0.0f;
        sim->mouse_dy = 0.0f;
    }
    start_microgestures(sim);
}

static b8 pose_usable (const Pose& pose)
{
    return zm::is_finite(pose) && zm::length(pose.orientation) > 1e-4f;
}

// Spec 7.1: an origin change applies from its change time on. Between frames the app already
// holds the predicted display time of the committed frame, so the change waits for the next one.
static void set_local_origin (SIM_Sim* sim, const Pose& origin)
{
    if (memory_equal(&sim->local_origin, &origin, sizeof(Pose)))
    {
        return;
    }
    const i64 live      = sim->time + sim->display_period;
    const b8  committed = sim->history_count > 0 && sim->history[(sim->history_head + sim->history_count - 1) % SIM_HISTORY_SAMPLES].time == live;
    sim->previous_local_origin    = sim->local_origin;
    sim->local_origin             = origin;
    sim->local_origin_generation += 1;
    sim->local_origin_change_time = committed ? live + sim->display_period : live;
}

static void live_sample (const SIM_Sim* sim, SIM_Sample& out_sample)
{
    out_sample.time = sim->time + sim->display_period;
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        out_sample.devices[i] = sim_get_device_state(sim, (SIM_Device)i);
    }
    out_sample.local_origin      = out_sample.time >= sim->local_origin_change_time ? sim->local_origin : sim->previous_local_origin;
    out_sample.stationary_origin = sim->stationary_origin;
    out_sample.tracking_drift    = sim->tracking_drift;
}

static void push_history (SIM_Sim* sim)
{
    const u32 slot = (sim->history_head + sim->history_count) % SIM_HISTORY_SAMPLES;
    live_sample(sim, sim->history[slot]);
    if (sim->history_count < SIM_HISTORY_SAMPLES)
    {
        sim->history_count++;
    }
    else
    {
        sim->history_head = (sim->history_head + 1) % SIM_HISTORY_SAMPLES;
    }
}

static const SIM_Sample& history_at (const SIM_Sim* sim, u32 index)
{
    return sim->history[(sim->history_head + index) % SIM_HISTORY_SAMPLES];
}

static Pose reference_pose (const Pose& head, const Pose& local_origin, const Pose& stationary_origin, SIM_ReferenceSpace space)
{
    switch (space)
    {
    case SIM_ReferenceSpace::View:
        return head;
    case SIM_ReferenceSpace::Local:
        return local_origin;
    case SIM_ReferenceSpace::Stage:
        return zm::POSE_IDENTITY;
    case SIM_ReferenceSpace::LocalFloor:
        return { .position = { local_origin.position.x, 0.0f, local_origin.position.z }, .orientation = local_origin.orientation };
    case SIM_ReferenceSpace::Stationary:
        return stationary_origin;
    }
    ASSERT_MSG(false, "invalid reference space %u", (u32)space);
    return zm::POSE_IDENTITY;
}

static Pose hand_pose (const SIM_Sim* sim, SIM_Device hand, const Pose& grip, SIM_HandPose kind)
{
    switch (kind)
    {
    case SIM_HandPose::Grip:
        return grip;
    case SIM_HandPose::Aim:
        return zm::pose_mul(grip, sim->config.aim_offset);
    case SIM_HandPose::Palm:
        return zm::pose_mul(grip, sim->config.palm_offset);
    case SIM_HandPose::Pinch:
    case SIM_HandPose::Poke:
        break;
    }
    const SIM_HandMotion motion = sim->devices[(u32)hand].source == SIM_HandSource::Controller ? SIM_HandMotion::ConformingToController : SIM_HandMotion::Unobstructed;
    SIM_HandJoints       joints = {};
    sim_hand_joints(sim, hand, grip, motion, joints);
    const Pose& index = joints.poses[(u32)SIM_HandJoint::IndexTip];
    if (kind == SIM_HandPose::Poke)
    {
        return index;
    }
    const Vec3 between = (joints.poses[(u32)SIM_HandJoint::ThumbTip].position + index.position) * 0.5f;
    return { between, zm::pose_mul(grip, sim->config.aim_offset).orientation };
}

static void view_pose (const SIM_Sim* sim, const Pose& head, u32 view, Pose& out_pose, Fov& out_fov)
{
    const f32 half = sim->config.fov_degrees * 0.5f * zm::DEG_TO_RAD;
    out_fov        = { .angle_left = -half, .angle_right = half, .angle_up = half, .angle_down = -half };
    if (view >= SIM_MAX_VIEWS)
    {
        ASSERT_MSG(false, "invalid view %u", view);
        out_pose = head;
        return;
    }
    const f32 offset = (view == 0 ? -0.5f : 0.5f) * sim->config.ipd;
    out_pose         = zm::pose_mul(head, { .position = { offset, 0.0f, 0.0f }, .orientation = zm::QUAT_IDENTITY });
}

static void extrapolate (SIM_DeviceState& device, f32 dt)
{
    device.pose.position += device.linear_velocity * dt;
    const f32 speed       = zm::length(device.angular_velocity);
    if (speed > 1e-6f)
    {
        const Quat turn         = zm::quat_from_axis_angle(device.angular_velocity / speed, speed * dt);
        device.pose.orientation = zm::normalize(turn * device.pose.orientation);
    }
}

static void interpolate (const SIM_Sample& a, const SIM_Sample& b, i64 time, SIM_Sample& out_sample)
{
    const f32 t     = (f32)(time - a.time) / (f32)(b.time - a.time);
    out_sample      = a;
    out_sample.time = time;
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        const SIM_DeviceState& da = a.devices[i];
        const SIM_DeviceState& db = b.devices[i];
        SIM_DeviceState&       d  = out_sample.devices[i];
        d.pose.position    = zm::lerp(da.pose.position, db.pose.position, t);
        d.pose.orientation = zm::slerp(da.pose.orientation, db.pose.orientation, t);
        d.linear_velocity  = zm::lerp(da.linear_velocity, db.linear_velocity, t);
        d.angular_velocity = zm::lerp(da.angular_velocity, db.angular_velocity, t);
    }
}

// A recorded body as it was, without normalizing its rotations again; values a recording rejects
// keep the current ones
static void apply_body (SIM_Sim* sim, const SIM_BodyState& state)
{
    SIM_BodyState& body  = sim->body;
    const b8       shape = state.height != body.height || !memory_equal(state.bone_scales, body.bone_scales, sizeof(body.bone_scales));
    if (zm::is_finite(state.height) && state.height >= SIM_BODY_MIN_HEIGHT && state.height <= SIM_BODY_MAX_HEIGHT)
    {
        body.height = state.height;
    }
    for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
    {
        const f32  scale    = state.bone_scales[j];
        const Quat rotation = state.rotations[j];
        if (zm::is_finite(scale) && scale >= SIM_BODY_MIN_SCALE && scale <= SIM_BODY_MAX_SCALE && (scale == 1.0f || body_scalable(j)))
        {
            body.bone_scales[j] = scale;
        }
        if (zm::is_finite(zm::length(rotation)) && zm::near(zm::length(rotation), 1.0f, 1e-3f))
        {
            body.rotations[j] = rotation;
        }
        if (zm::is_finite(state.confidences[j]) && state.confidences[j] >= 0.0f && state.confidences[j] <= 1.0f)
        {
            body.confidences[j] = state.confidences[j];
        }
        body.tracked[j] = state.tracked[j];
    }
    for (u32 l = 0; l < (u32)SIM_Limb::Count; l++)
    {
        body.limbs[l] = (u32)state.limbs[l] < (u32)SIM_LimbMode::Count ? state.limbs[l] : body.limbs[l];
    }
    if (shape)
    {
        sim->body_generation++;
    }
}

// Poses are applied bit-exactly: normalizing an already unit quaternion again can change its
// last bit and make the replay diverge. Recordings reject implausible quaternions on load.
static void apply_state (SIM_Sim* sim, const SIM_FrameState& state)
{
    if (pose_usable(state.local_origin))
    {
        set_local_origin(sim, state.local_origin);
    }
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        const SIM_DeviceFrame& frame  = state.devices[i];
        SIM_DeviceData&        device = sim->devices[i];
        if (pose_usable(frame.pose))
        {
            device.pose = frame.pose;
        }
        device.active = frame.active;
        device.source = is_hand((SIM_Device)i) && (u32)frame.source < (u32)SIM_HandSource::Count ? frame.source : SIM_HandSource::Controller;
        if (device.source == SIM_HandSource::Hand && pose_usable(frame.detached))
        {
            device.detached_pose = frame.detached;
        }
        for (u32 k = 0; k < (u32)SIM_Input::Count; k++)
        {
            const SIM_InputValue value  = frame.inputs[k];
            const b8             finite = zm::is_finite(value.value.x) && zm::is_finite(value.value.y);
            const b8             exists = hand_input_exists(sim, (SIM_Device)i, (SIM_Input)k);
            device.inputs[k]            = exists && finite ? clamp_input((SIM_Input)k, value) : SIM_InputValue{};
        }
    }
    sim->typed_length = state.typed_length <= SIM_MAX_TYPED ? state.typed_length : 0;
    memory_copy(sim->typed, state.typed, sim->typed_length);
    if (state.props_present)
    {
        sim->props_count = 0;
        for (u32 i = 0; i < state.props_count && i < SIM_MAX_PROPS; i++)
        {
            SIM_Prop prop = {};
            if (state.props[i].id != 0 && normalize_prop(state.props[i], prop))
            {
                sim->props[sim->props_count++] = prop;
                sim->next_prop_id              = zm::max(sim->next_prop_id, prop.id + 1);
            }
        }
    }
    if (state.body_present)
    {
        apply_body(sim, state.body);
    }
    if (state.face_present)
    {
        sim_set_face(sim, state.face);
    }
    if (state.drift_present && sim_tracking_drift_valid(state.tracking_drift))
    {
        sim->tracking_drift = state.tracking_drift;
    }
}

void sim_capture_state (const SIM_Sim* sim, SIM_FrameState& out_state)
{
    ASSERT(sim);
    out_state              = {};
    out_state.dt           = sim->last_dt;
    out_state.local_origin = sim->local_origin;
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        out_state.devices[i].pose   = sim->devices[i].pose;
        out_state.devices[i].active = sim->devices[i].active;
        out_state.devices[i].source = sim->devices[i].source;
        out_state.devices[i].detached = sim->devices[i].source == SIM_HandSource::Hand ? sim->devices[i].detached_pose : zm::POSE_IDENTITY;
        memory_copy(out_state.devices[i].inputs, sim->devices[i].inputs, sizeof(sim->devices[i].inputs));
    }
    memory_copy(out_state.typed, sim->typed, sim->typed_length);
    out_state.typed_length  = sim->typed_length;
    out_state.props_present = true;
    out_state.props_count   = sim->props_count;
    memory_copy(out_state.props, sim->props, sizeof(SIM_Prop) * sim->props_count);
    out_state.body_present   = true;
    out_state.body           = sim->body;
    out_state.face_present   = true;
    out_state.face           = sim->face;
    out_state.drift_present  = true;
    out_state.tracking_drift = sim->tracking_drift;
}

void sim_capture_snapshot (const SIM_Sim* sim, SIM_Snapshot& out_snapshot)
{
    ASSERT(sim);
    out_snapshot = {};
    sim_capture_state(sim, out_snapshot.state);
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        out_snapshot.previous_poses[i] = sim->devices[i].previous_pose;
    }
    out_snapshot.time        = sim->time;
    out_snapshot.frame_index = sim->frame_index;
}

void sim_restore_snapshot (SIM_Sim* sim, const SIM_Snapshot& snapshot)
{
    ASSERT(sim);
    apply_state(sim, snapshot.state);
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        const Pose previous           = snapshot.previous_poses[i];
        sim->devices[i].previous_pose = pose_usable(previous) ? previous : sim->devices[i].pose;
        memory_zero(sim->devices[i].haptics, sizeof(sim->devices[i].haptics));
    }
    memory_zero(sim->envelope_counts, sizeof(sim->envelope_counts));
    memory_zero(sim->haptic_effects, sizeof(sim->haptic_effects));
    sim->time                  = snapshot.time >= 0 ? snapshot.time : sim->time;
    sim->frame_index           = snapshot.frame_index;
    sim->last_dt               = snapshot.state.dt;
    sim->history_count         = 0;
    sim->previous_local_origin = sim->local_origin;
}

void sim_replay_frame (SIM_Sim* sim, const SIM_FrameState& state)
{
    ASSERT(sim);
    const i64 dt  = state.dt < 0 ? 0 : zm::min(state.dt, SIM_MAX_REALTIME_DT);
    sim->time    += dt;
    sim->last_dt  = dt;
    sim->mouse_dx = 0.0f;
    sim->mouse_dy = 0.0f;
    apply_state(sim, state);
    start_microgestures(sim);
}

void sim_commit_frame (SIM_Sim* sim)
{
    ASSERT(sim);

    const f32 dt = (f32)sim->display_period * 1e-9f;
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        SIM_DeviceData& device = sim->devices[i];

        device.linear_velocity = (device.pose.position - device.previous_pose.position) / dt;

        // q and -q are the same rotation; the positive hemisphere gives the short arc
        Quat delta = zm::normalize(device.pose.orientation * zm::inverse(device.previous_pose.orientation));
        if (delta.w < 0.0f)
        {
            delta = -delta;
        }
        const f32  w     = zm::clamp(delta.w, -1.0f, 1.0f);
        const f32  angle = 2.0f * zm::acos(w);
        const f32  s     = zm::sqrt(1.0f - w * w);
        if (s > 1e-4f && angle > 1e-6f)
        {
            const Vec3 axis         = Vec3{ delta.x, delta.y, delta.z } / s;
            device.angular_velocity = axis * (angle / dt);
        }
        else
        {
            device.angular_velocity = {};
        }
        device.previous_pose = device.pose;

        for (u32 o = 0; o < (u32)SIM_HapticOutput::Count; o++)
        {
            SIM_Haptic& haptic = device.haptics[o];
            if (haptic.active && sim->time >= haptic.end_time)
            {
                haptic = {};
                if (is_hand((SIM_Device)i) && !sim->haptic_effects[hand_index((SIM_Device)i)][o].stream_running)
                {
                    sim->envelope_counts[hand_index((SIM_Device)i)][o] = 0;
                    sim->haptic_effects[hand_index((SIM_Device)i)][o].kind = SIM_HapticKind::Vibration;
                }
            }
        }
    }

    sim->frame_index++;
    push_history(sim);
}

i64 sim_time (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->time;
}

i64 sim_predicted_display_time (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->time + sim->display_period;
}

i64 sim_display_period (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->display_period;
}

f32 sim_refresh_rate (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->refresh_rate;
}

f32 sim_default_refresh_rate (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->default_refresh_rate;
}

u32 sim_refresh_rates (const SIM_Sim* sim, f32* out_rates, u32 capacity)
{
    ASSERT(sim);
    for (u32 i = 0; out_rates && i < sim->refresh_rates_count && i < capacity; i++)
    {
        out_rates[i] = sim->refresh_rates[i];
    }
    return sim->refresh_rates_count;
}

b8 sim_set_refresh_rate (SIM_Sim* sim, f32 hz)
{
    ASSERT(sim);
    b8 listed = false;
    for (u32 i = 0; i < sim->refresh_rates_count; i++)
    {
        listed |= sim->refresh_rates[i] == hz;
    }
    if (!listed)
    {
        return false;
    }
    sim->refresh_rate             = hz;
    sim->display_period           = refresh_period(hz);
    sim->config.refresh_rate_hz   = hz;
    sim->config.display_period_ns = sim->display_period;
    return true;
}

u64 sim_frame_index (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->frame_index;
}

SIM_ClockMode sim_clock_mode (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->clock_mode;
}

void sim_set_clock_mode (SIM_Sim* sim, SIM_ClockMode mode)
{
    ASSERT(sim);
    sim->clock_mode = mode;
}

//
// Devices and inputs
//

Pose sim_get_device_pose (const SIM_Sim* sim, SIM_Device device)
{
    ASSERT(sim);
    if (!device_valid(device))
    {
        ASSERT_MSG(false, "invalid device %u", (u32)device);
        return zm::POSE_IDENTITY;
    }
    return sim->devices[(u32)device].pose;
}

void sim_set_device_pose (SIM_Sim* sim, SIM_Device device, const Pose& pose)
{
    ASSERT(sim);
    if (!device_valid(device))
    {
        ASSERT_MSG(false, "invalid device %u", (u32)device);
        return;
    }
    if (!zm::is_finite(pose) || zm::length(pose.orientation) < 1e-4f)
    {
        LOG_ERROR("Ignoring non-finite or degenerate pose for device %u", (u32)device);
        ASSERT_MSG(false, "invalid pose");
        return;
    }
    sim->devices[(u32)device].pose = { .position = pose.position, .orientation = zm::normalize(pose.orientation) };
}

SIM_DeviceState sim_get_device_state (const SIM_Sim* sim, SIM_Device device)
{
    ASSERT(sim);
    if (!device_valid(device))
    {
        ASSERT_MSG(false, "invalid device %u", (u32)device);
        return { .pose = zm::POSE_IDENTITY };
    }
    const SIM_DeviceData& data = sim->devices[(u32)device];
    return {
        .pose             = data.pose,
        .linear_velocity  = data.linear_velocity,
        .angular_velocity = data.angular_velocity,
        .active           = data.active,
        .tracked          = data.active,
    };
}

void sim_set_device_active (SIM_Sim* sim, SIM_Device device, b8 active)
{
    ASSERT(sim);
    if (!device_valid(device))
    {
        ASSERT_MSG(false, "invalid device %u", (u32)device);
        return;
    }
    sim->devices[(u32)device].active = active;
}

Pose sim_get_hand_pose (const SIM_Sim* sim, SIM_Device hand, SIM_HandPose kind)
{
    ASSERT(sim);
    if (!is_hand(hand))
    {
        ASSERT_MSG(false, "device %u is not a hand", (u32)hand);
        return zm::POSE_IDENTITY;
    }
    return hand_pose(sim, hand, sim->devices[(u32)hand].pose, kind);
}

void sim_set_hand_pose (SIM_Sim* sim, SIM_Device hand, SIM_HandPose kind, const Pose& pose)
{
    ASSERT(sim);
    if (!is_hand(hand))
    {
        ASSERT_MSG(false, "device %u is not a hand", (u32)hand);
        return;
    }
    const Pose offset = zm::pose_relative(sim->devices[(u32)hand].pose, hand_pose(sim, hand, sim->devices[(u32)hand].pose, kind));
    sim_set_device_pose(sim, hand, zm::pose_mul(pose, zm::pose_inverse(offset)));
}

void sim_point_hand (SIM_Sim* sim, SIM_Device hand, u32 view, f32 u, f32 v, const Pose& ray)
{
    ASSERT(sim);
    if (!is_hand(hand) || view >= sim_view_count(sim))
    {
        ASSERT_MSG(false, "device %u is not a hand or view %u is invalid", (u32)hand, view);
        return;
    }
    Pose eye = {};
    Fov  fov = {};
    sim_get_view(sim, view, eye, fov);
    const f32  x       = zm::lerp(zm::tan(fov.angle_left), zm::tan(fov.angle_right), u);
    const f32  y       = zm::lerp(zm::tan(fov.angle_up), zm::tan(fov.angle_down), v);
    const Vec3 forward = zm::rotate(eye.orientation, { x, y, -1.0f });
    const Vec3 up      = zm::rotate(eye.orientation, { 0.0f, 1.0f, 0.0f });
    const Pose target  = { .position = eye.position, .orientation = zm::quat_look_rotation(forward, up) };
    sim_set_device_pose(sim, hand, zm::pose_mul(target, zm::pose_inverse(ray)));
}

SIM_InputValue sim_get_input (const SIM_Sim* sim, SIM_Device device, SIM_Input input)
{
    ASSERT(sim);
    if (!device_valid(device) || !input_valid(input))
    {
        ASSERT_MSG(false, "invalid device %u or input %u", (u32)device, (u32)input);
        return {};
    }
    return sim->devices[(u32)device].inputs[(u32)input];
}

void sim_set_input (SIM_Sim* sim, SIM_Device device, SIM_Input input, SIM_InputValue value)
{
    ASSERT(sim);
    if (!device_valid(device) || !input_valid(input))
    {
        ASSERT_MSG(false, "invalid device %u or input %u", (u32)device, (u32)input);
        return;
    }
    if (!hand_input_exists(sim, device, input))
    {
        return;
    }
    if (!zm::is_finite(value.value.x) || !zm::is_finite(value.value.y))
    {
        LOG_ERROR("Ignoring non-finite input value for %s", sim_input_name(input));
        return;
    }
    sim->devices[(u32)device].inputs[(u32)input] = clamp_input(input, value);
    if (is_microgesture(input))
    {
        sim->gesture_seen[hand_index(device)][(u32)input - (u32)SIM_Input::SwipeLeft] = false;
    }
}

static const char* const sim_device_names[(u32)SIM_Device::Count] = { "head", "left", "right" };

const char* sim_device_name (SIM_Device device)
{
    return device_valid(device) ? sim_device_names[(u32)device] : "";
}

b8 sim_device_from_name (const char* name, SIM_Device& out_device)
{
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        if (cstr_equal(name, sim_device_names[i]))
        {
            out_device = (SIM_Device)i;
            return true;
        }
    }
    out_device = SIM_Device::Count;
    return false;
}

SIM_Controller sim_get_controller (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->controller;
}

void sim_set_controller (SIM_Sim* sim, SIM_Controller controller)
{
    ASSERT(sim);
    if ((u32)controller >= (u32)SIM_Controller::Count)
    {
        ASSERT_MSG(false, "invalid controller %u", (u32)controller);
        return;
    }
    sim->controller = controller;
    zero_missing_inputs(sim);
}

SIM_HandSource sim_get_hand_source (const SIM_Sim* sim, SIM_Device hand)
{
    ASSERT(sim);
    if (!is_hand(hand))
    {
        ASSERT_MSG(false, "device %u is not a hand", (u32)hand);
        return SIM_HandSource::Controller;
    }
    return sim->devices[(u32)hand].source;
}

void sim_set_hand_source (SIM_Sim* sim, SIM_Device hand, SIM_HandSource source)
{
    ASSERT(sim);
    if (!is_hand(hand) || (u32)source >= (u32)SIM_HandSource::Count)
    {
        ASSERT_MSG(false, "invalid hand %u or source %u", (u32)hand, (u32)source);
        return;
    }
    SIM_DeviceData& device = sim->devices[(u32)hand];
    if (device.source == SIM_HandSource::Controller && source == SIM_HandSource::Hand)
    {
        device.detached_pose = device.pose;
    }
    device.source = source;
    zero_missing_inputs(sim);
}

Pose sim_get_detached_pose (const SIM_Sim* sim, SIM_Device hand)
{
    ASSERT(sim);
    if (!is_hand(hand))
    {
        ASSERT_MSG(false, "device %u is not a hand", (u32)hand);
        return zm::POSE_IDENTITY;
    }
    return sim->devices[(u32)hand].detached_pose;
}

Pose sim_detached_hand_pose (const SIM_Sim* sim, SIM_Device hand, SIM_HandPose kind)
{
    ASSERT(sim);
    if (!is_hand(hand))
    {
        ASSERT_MSG(false, "device %u is not a hand", (u32)hand);
        return zm::POSE_IDENTITY;
    }
    const Pose& grip = sim->devices[(u32)hand].detached_pose;
    switch (kind)
    {
    case SIM_HandPose::Grip:
        return grip;
    case SIM_HandPose::Palm:
        return zm::pose_mul(grip, sim->config.palm_offset);
    case SIM_HandPose::Aim:
    case SIM_HandPose::Pinch:
    case SIM_HandPose::Poke:
        break;
    }
    return zm::pose_mul(grip, sim->config.aim_offset);
}

void sim_set_detached_pose (SIM_Sim* sim, SIM_Device hand, const Pose& pose)
{
    ASSERT(sim);
    if (!is_hand(hand) || !zm::is_finite(pose) || zm::length(pose.orientation) < 0.5f)
    {
        ASSERT_MSG(false, "invalid detached pose of device %u", (u32)hand);
        return;
    }
    sim->devices[(u32)hand].detached_pose = { pose.position, zm::normalize(pose.orientation) };
}

const char* sim_hand_source_name (SIM_HandSource source)
{
    return (u32)source < (u32)SIM_HandSource::Count ? sim_hand_source_names[(u32)source] : "";
}

b8 sim_hand_source_from_name (const char* name, SIM_HandSource& out_source)
{
    for (u32 i = 0; i < (u32)SIM_HandSource::Count; i++)
    {
        if (cstr_equal(name, sim_hand_source_names[i]))
        {
            out_source = (SIM_HandSource)i;
            return true;
        }
    }
    return false;
}

b8 sim_hand_has_input (SIM_HandSource source, SIM_Controller controller, SIM_Device hand, SIM_Input input)
{
    if (source == SIM_HandSource::Controller)
    {
        const u32 other = hand == SIM_Device::LeftHand ? sim_controller_infos[(u32)controller].right_only : sim_controller_infos[(u32)controller].left_only;
        return is_hand(hand) && sim_controller_has_input(controller, input) && !((other >> (u32)input) & 1u);
    }
    return input == SIM_Input::TriggerValue || input == SIM_Input::SqueezeValue || input == SIM_Input::MenuClick || is_microgesture(input);
}

b8 sim_hand_in_view (const SIM_Sim* sim, const SIM_Sample& sample, SIM_Device hand)
{
    ASSERT(sim);
    if (!is_hand(hand))
    {
        ASSERT_MSG(false, "device %u is not a hand", (u32)hand);
        return false;
    }
    const Pose& head      = sample.devices[(u32)SIM_Device::Head].pose;
    const Vec3  to_hand   = sample.devices[(u32)hand].pose.position - head.position;
    const f32   distance  = zm::length(to_hand);
    if (distance < 1e-4f)
    {
        return true;
    }
    const Vec3 forward = zm::rotate(head.orientation, { 0.0f, 0.0f, -1.0f });
    const f32  half    = sim->config.hand_tracking_fov_degrees * 0.5f * zm::DEG_TO_RAD;
    return zm::dot(to_hand, forward) >= distance * zm::cos(half);
}

void sim_hand_joints (const SIM_Sim* sim, SIM_Device hand, const Pose& grip, SIM_HandMotion motion, SIM_HandJoints& out_joints)
{
    ASSERT(sim);
    out_joints = {};
    if (!is_hand(hand))
    {
        ASSERT_MSG(false, "device %u is not a hand", (u32)hand);
        return;
    }
    const SIM_DeviceData& device = sim->devices[(u32)hand];
    f32 pinch = device.inputs[(u32)SIM_Input::TriggerValue].value.x;
    f32 grab  = device.inputs[(u32)SIM_Input::SqueezeValue].value.x;
    if (motion == SIM_HandMotion::ConformingToController && device.source == SIM_HandSource::Controller)
    {
        pinch = zm::max(pinch, SIM_HELD_PINCH);
        grab  = zm::max(grab, SIM_HELD_GRAB);
    }
    const b8      left  = hand == SIM_Device::LeftHand;
    SIM_HandShape shape = hand_shape(pinch, grab);
    add_microgesture(sim, hand, shape);
    Pose local[SIM_HAND_JOINT_COUNT];
    hand_local(shape, left, local);
    const Pose wrist = zm::pose_mul(grip, wrist_in_grip(left));
    for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
    {
        out_joints.poses[j] = zm::pose_mul(wrist, local[j]);
        out_joints.radii[j] = sim_joint_radii[j];
    }
}

const SIM_HandMesh& sim_get_hand_mesh (const SIM_Sim* sim, SIM_Device hand)
{
    ASSERT(sim);
    ASSERT_MSG(is_hand(hand), "device %u is not a hand", (u32)hand);
    return sim->hand_meshes[hand == SIM_Device::LeftHand ? 0 : 1];
}

// A compact keyboard: a slab and five rows of caps, the last with a space bar
void sim_keyboard_model (SIM_RenderModel& out_model)
{
    out_model = {};
    const f32 pitch = 0.019f;
    const f32 cap   = 0.0085f; // half width of a cap
    const f32 base  = 0.006f;  // half height of the slab
    begin_part(out_model, SIM_MODEL_BODY);
    add_box(out_model, { 0.0f, 0.0f, 0.0f }, { 0.15f, base, 0.056f });
    end_part(out_model);

    begin_part(out_model, SIM_MODEL_BUTTON);
    static const u32 row_keys[4] = { 15, 14, 13, 12 };
    const f32        top         = base + 0.003f;
    for (u32 row = 0; row < 5; row++)
    {
        const f32 z = -0.038f + (f32)row * pitch;
        if (row == 4)
        {
            add_box(out_model, { 0.0f, top, z }, { 3.0f * pitch, 0.003f, cap });
            for (u32 k = 0; k < 4; k++)
            {
                const f32 x = (3.5f + (f32)k) * pitch;
                add_box(out_model, { -x, top, z }, { cap, 0.003f, cap });
                add_box(out_model, { x, top, z }, { cap, 0.003f, cap });
            }
            continue;
        }
        const f32 first = -0.5f * (f32)(row_keys[row] - 1) * pitch;
        for (u32 k = 0; k < row_keys[row]; k++)
        {
            add_box(out_model, { first + (f32)k * pitch, top, z }, { cap, 0.003f, cap });
        }
    }
    end_part(out_model);

    // Spec XR_FB_render_model: a keyboard's origin is the centre of its bounding box
    Vec3 low  = out_model.positions[0];
    Vec3 high = low;
    for (u32 v = 0; v < out_model.vertices_count; v++)
    {
        low  = zm::min(low, out_model.positions[v]);
        high = zm::max(high, out_model.positions[v]);
    }
    const Vec3 center = (low + high) * 0.5f;
    for (u32 v = 0; v < out_model.vertices_count; v++)
    {
        out_model.positions[v] -= center;
    }
}

void sim_vkb_layout (SIM_VkbLayout& out_layout)
{
    struct Row
    {
        const char*    chars;
        SIM_VkbKeyKind before; // a special key before the characters, Char for none
        SIM_VkbKeyKind after;  // and after them
        f32            special_width;
    };
    static const Row rows[] = {
        { "1234567890", SIM_VkbKeyKind::Char, SIM_VkbKeyKind::Char, 0.0f },
        { "qwertyuiop", SIM_VkbKeyKind::Char, SIM_VkbKeyKind::Backspace, 1.5f },
        { "asdfghjkl", SIM_VkbKeyKind::Char, SIM_VkbKeyKind::Enter, 1.75f },
        { "zxcvbnm,.", SIM_VkbKeyKind::Shift, SIM_VkbKeyKind::Char, 1.5f },
        { "", SIM_VkbKeyKind::Space, SIM_VkbKeyKind::Char, 5.0f },
    };

    out_layout     = {};
    u32 cell_x     = 0;
    u32 cell_y     = 0;
    f32 widest     = 0.0f;
    for (u32 r = 0; r < ARRAY_SIZE(rows); r++)
    {
        const Row&  row     = rows[r];
        const u32   chars   = (u32)cstr_length(row.chars);
        const u32   special = (row.before != SIM_VkbKeyKind::Char ? 1u : 0u) + (row.after != SIM_VkbKeyKind::Char ? 1u : 0u);
        const f32   units   = (f32)chars + (f32)special * row.special_width;
        const f32   y       = (2.0f - (f32)r) * SIM_VKB_PITCH;
        f32         x       = -0.5f * units * SIM_VKB_PITCH;
        widest              = zm::max(widest, units);
        for (u32 k = 0; k < chars + special; k++)
        {
            SIM_VkbKey key = { .kind = SIM_VkbKeyKind::Char };
            f32        width = 1.0f;
            if (k == 0 && row.before != SIM_VkbKeyKind::Char)
            {
                key.kind = row.before;
                width    = row.special_width;
            }
            else if (k == chars + special - 1 && row.after != SIM_VkbKeyKind::Char)
            {
                key.kind = row.after;
                width    = row.special_width;
            }
            else
            {
                const char c = row.chars[k - (row.before != SIM_VkbKeyKind::Char ? 1u : 0u)];
                key.lower    = c;
                key.upper    = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c == ',' ? '?' : c == '.' ? '!' : c;
            }
            key.center     = { x + 0.5f * width * SIM_VKB_PITCH, y };
            key.half       = { 0.5f * (width * SIM_VKB_PITCH - SIM_VKB_GAP), 0.5f * (SIM_VKB_PITCH - SIM_VKB_GAP) };
            key.cell_width = (u32)(width * (f32)SIM_VKB_CELL + 0.5f);
            if (cell_x + key.cell_width > SIM_VKB_TEXTURE_WIDTH)
            {
                cell_x = 0;
                cell_y += SIM_VKB_CELL;
            }
            key.cell_x = cell_x;
            key.cell_y = cell_y;
            cell_x += key.cell_width;
            x += width * SIM_VKB_PITCH;
            ASSERT(out_layout.keys_count < SIM_VKB_MAX_KEYS);
            out_layout.keys[out_layout.keys_count++] = key;
        }
    }
    ASSERT(cell_y + SIM_VKB_CELL <= SIM_VKB_SOLID_TEXEL_Y - 8);
    out_layout.half = { 0.5f * widest * SIM_VKB_PITCH + SIM_VKB_MARGIN, 2.5f * SIM_VKB_PITCH + SIM_VKB_MARGIN };
}

i32 sim_vkb_key_at (const SIM_VkbLayout& layout, Vec2 point)
{
    for (u32 k = 0; k < layout.keys_count; k++)
    {
        const SIM_VkbKey& key = layout.keys[k];
        if (zm::abs(point.x - key.center.x) <= key.half.x + 0.5f * SIM_VKB_GAP && zm::abs(point.y - key.center.y) <= key.half.y + 0.5f * SIM_VKB_GAP)
        {
            return (i32)k;
        }
    }
    return -1;
}

void sim_vkb_texture (const SIM_VkbLayout& layout, b8 shifted, u8* out_rgba)
{
    fill_texels(out_rgba, 0, 0, SIM_VKB_TEXTURE_WIDTH, SIM_VKB_TEXTURE_HEIGHT, SIM_VKB_PANEL_COLOR);
    const u8 clear[4] = {};
    fill_texels(out_rgba, SIM_VKB_CLEAR_TEXEL_X - 8, SIM_VKB_SOLID_TEXEL_Y - 8, 16, 16, clear);
    for (u32 k = 0; k < layout.keys_count; k++)
    {
        const SIM_VkbKey& key    = layout.keys[k];
        const b8          active = key.kind == SIM_VkbKeyKind::Shift && shifted;
        fill_texels(out_rgba, key.cell_x, key.cell_y, key.cell_width, SIM_VKB_CELL, SIM_VKB_BORDER_COLOR);
        fill_texels(out_rgba, key.cell_x + 1, key.cell_y + 1, key.cell_width - 2, SIM_VKB_CELL - 2, SIM_VKB_KEY_COLOR);
        const SIM_Glyph* glyph = find_glyph(vkb_label(key, shifted));
        ASSERT(glyph);
        const u32 left = key.cell_x + (key.cell_width - 5 * SIM_VKB_GLYPH_SCALE) / 2;
        const u32 top  = key.cell_y + (SIM_VKB_CELL - 7 * SIM_VKB_GLYPH_SCALE) / 2;
        for (u32 row = 0; row < 7; row++)
        {
            for (u32 column = 0; column < 5; column++)
            {
                if (glyph->rows[row] & (0x10u >> column))
                {
                    fill_texels(out_rgba, left + column * SIM_VKB_GLYPH_SCALE, top + row * SIM_VKB_GLYPH_SCALE, SIM_VKB_GLYPH_SCALE, SIM_VKB_GLYPH_SCALE, active ? SIM_VKB_SHIFT_COLOR : SIM_VKB_LABEL_COLOR);
                }
            }
        }
    }
}

void sim_vkb_model (const SIM_VkbLayout& layout, SIM_RenderModel& out_model)
{
    out_model = { .texture_id = SIM_VKB_TEXTURE_ID, .texture_width = SIM_VKB_TEXTURE_WIDTH, .texture_height = SIM_VKB_TEXTURE_HEIGHT };
    const Vec4 white = { 1.0f, 1.0f, 1.0f, 1.0f };
    const f32  w     = (f32)SIM_VKB_TEXTURE_WIDTH;
    const f32  h     = (f32)SIM_VKB_TEXTURE_HEIGHT;
    for (u32 k = 0; k < layout.keys_count; k++)
    {
        const SIM_VkbKey& key = layout.keys[k];
        begin_part(out_model, white);
        SIM_ModelPart& part = out_model.parts[out_model.parts_count - 1];
        part.press          = { 0.0f, 0.0f, -SIM_VKB_PRESS };
        cstr_format(part.name, sizeof(part.name), "key%u", k);
        // Half a texel in from the cell edges so filtering never reaches the next cell
        const Vec2 low  = { ((f32)key.cell_x + 0.5f) / w, ((f32)key.cell_y + 0.5f) / h };
        const Vec2 high = { ((f32)(key.cell_x + key.cell_width) - 0.5f) / w, ((f32)(key.cell_y + SIM_VKB_CELL) - 0.5f) / h };
        add_vkb_quad(out_model, key.center, key.half, SIM_VKB_KEY_HEIGHT, low, high);
        end_part(out_model);
    }

    const Vec2 panel = { (f32)SIM_VKB_PANEL_TEXEL_X / w, (f32)SIM_VKB_SOLID_TEXEL_Y / h };
    begin_part(out_model, white);
    cstr_copy(out_model.parts[out_model.parts_count - 1].name, sizeof(out_model.parts[0].name), "panel");
    add_vkb_quad(out_model, { 0.0f, 0.0f }, layout.half, 0.0f, panel, panel);
    end_part(out_model);

    // Spec XR_META_virtual_keyboard: the app's colliders come from the node named "collision"
    const Vec2 clear = { (f32)SIM_VKB_CLEAR_TEXEL_X / w, (f32)SIM_VKB_SOLID_TEXEL_Y / h };
    begin_part(out_model, white);
    cstr_copy(out_model.parts[out_model.parts_count - 1].name, sizeof(out_model.parts[0].name), "collision");
    add_vkb_quad(out_model, { 0.0f, 0.0f }, layout.half, -0.002f, clear, clear);
    end_part(out_model);
}

i32 sim_vkb_find_key (const SIM_VkbLayout& layout, const char* name)
{
    for (u32 k = 0; k < layout.keys_count; k++)
    {
        const SIM_VkbKey& key = layout.keys[k];
        const b8 match = key.kind == SIM_VkbKeyKind::Char ? name[0] == key.lower && name[1] == 0 : cstr_equal(name, SIM_VKB_SPECIAL_NAMES[(u32)key.kind]);
        if (match)
        {
            return (i32)k;
        }
    }
    return -1;
}
b8 sim_type_text (SIM_Sim* sim, const char* text)
{
    ASSERT(sim);
    if (!text)
    {
        return false;
    }
    const usize length = cstr_length(text);
    if (sim->typed_pending_length + length > SIM_MAX_TYPED)
    {
        return false;
    }
    for (usize i = 0; i < length; i++)
    {
        const u8 c = (u8)text[i];
        if (c < 0x20 && c != '\b' && c != '\n')
        {
            return false;
        }
        if (c == 0x7F)
        {
            return false;
        }
    }
    memory_copy(sim->typed_pending + sim->typed_pending_length, text, length);
    sim->typed_pending_length += (u32)length;
    return true;
}

const char* sim_typed_text (const SIM_Sim* sim, u32& out_length)
{
    ASSERT(sim);
    out_length = sim->typed_length;
    return sim->typed;
}

u32 sim_add_prop (SIM_Sim* sim, const SIM_Prop& prop)
{
    ASSERT(sim);
    SIM_Prop stored = {};
    if (sim->props_count == SIM_MAX_PROPS || !normalize_prop(prop, stored))
    {
        return 0;
    }
    stored.id    = sim->next_prop_id++;
    stored.start = sim->time;
    sim->props[sim->props_count++] = stored;
    return stored.id;
}

b8 sim_update_prop (SIM_Sim* sim, u32 id, const SIM_Prop& prop)
{
    ASSERT(sim);
    SIM_Prop stored = {};
    for (u32 i = 0; i < sim->props_count; i++)
    {
        if (sim->props[i].id == id)
        {
            if (!normalize_prop(prop, stored))
            {
                return false;
            }
            stored.id      = id;
            stored.start   = sim->time;
            sim->props[i] = stored;
            return true;
        }
    }
    return false;
}

b8 sim_remove_prop (SIM_Sim* sim, u32 id)
{
    ASSERT(sim);
    for (u32 i = 0; i < sim->props_count; i++)
    {
        if (sim->props[i].id == id)
        {
            for (u32 j = i + 1; j < sim->props_count; j++)
            {
                sim->props[j - 1] = sim->props[j];
            }
            sim->props_count--;
            return true;
        }
    }
    return false;
}

u32 sim_props (const SIM_Sim* sim, const SIM_Prop*& out_props)
{
    ASSERT(sim);
    out_props = sim->props;
    return sim->props_count;
}

// Distances along the path in f64: an f32 would lose the phase within minutes
Pose sim_prop_pose (const SIM_Prop& prop, i64 time)
{
    const f64 elapsed   = time > prop.start ? (f64)(time - prop.start) * 1e-9 : 0.0;
    const f64 travelled = elapsed * (f64)prop.speed;
    switch (prop.motion)
    {
    case SIM_PropMotion::Still:
        return prop.pose;
    case SIM_PropMotion::Linear:
    {
        const f32 length = zm::length(prop.target - prop.pose.position);
        if (length <= 0.0f)
        {
            return prop.pose;
        }
        const f64 period = 2.0 * (f64)length;
        const f64 phase  = travelled - (f64)(i64)(travelled / period) * period;
        const f64 along  = phase <= (f64)length ? phase : period - phase;
        return { zm::lerp(prop.pose.position, prop.target, (f32)(along / (f64)length)), prop.pose.orientation };
    }
    case SIM_PropMotion::Circle:
    {
        const f64 turn  = 2.0 * (f64)zm::PI;
        const f64 angle = travelled / (f64)prop.radius;
        const f32 theta = (f32)(angle - (f64)(i64)(angle / turn) * turn);
        const Vec3 offset = { prop.radius * zm::cos(theta), 0.0f, -prop.radius * zm::sin(theta) };
        return { prop.pose.position + offset, zm::normalize(zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, theta) * prop.pose.orientation) };
    }
    case SIM_PropMotion::Count:
        break;
    }
    return prop.pose;
}

Vec3 sim_prop_extent (const SIM_Prop& prop)
{
    return prop.kind == SIM_PropKind::Keyboard ? SIM_KEYBOARD_EXTENT : Vec3{ prop.size, prop.size, 0.0f };
}

const char* sim_prop_kind_name (SIM_PropKind kind)
{
    return (u32)kind < (u32)SIM_PropKind::Count ? sim_prop_kind_names[(u32)kind] : "";
}

b8 sim_prop_kind_from_name (const char* name, SIM_PropKind& out_kind)
{
    for (u32 i = 0; i < (u32)SIM_PropKind::Count; i++)
    {
        if (name && cstr_equal(name, sim_prop_kind_names[i]))
        {
            out_kind = (SIM_PropKind)i;
            return true;
        }
    }
    return false;
}

const char* sim_prop_motion_name (SIM_PropMotion motion)
{
    return (u32)motion < (u32)SIM_PropMotion::Count ? sim_prop_motion_names[(u32)motion] : "";
}

b8 sim_prop_motion_from_name (const char* name, SIM_PropMotion& out_motion)
{
    for (u32 i = 0; i < (u32)SIM_PropMotion::Count; i++)
    {
        if (name && cstr_equal(name, sim_prop_motion_names[i]))
        {
            out_motion = (SIM_PropMotion)i;
            return true;
        }
    }
    return false;
}

void sim_controller_model (SIM_Controller controller, SIM_Device hand, SIM_RenderModel& out_model)
{
    out_model = {};
    ASSERT_MSG(is_hand(hand), "device %u is not a hand", (u32)hand);

    // Right hand in grip space: the handle runs along Z (its top at -Z), the trigger faces -Y,
    // the face buttons sit on top; the thumb side is -X
    const Quat upright = zm::QUAT_IDENTITY;
    begin_part(out_model, SIM_MODEL_BODY);
    name_part(out_model, "body");
    add_cylinder(out_model, { { 0.0f, 0.0f, 0.0125f }, upright }, 0.017f, 0.0475f);
    add_cylinder(out_model, { { 0.0f, -0.008f, -0.05f }, upright }, controller == SIM_Controller::MetaTouchPro ? 0.036f : 0.032f, 0.012f);
    end_part(out_model);

    begin_part(out_model, SIM_MODEL_TRIGGER);
    name_part(out_model, "trigger");
    add_cylinder(out_model, { { 0.0f, -0.03f, -0.028f }, upright }, 0.007f, 0.012f);
    end_part(out_model);

    begin_part(out_model, SIM_MODEL_BUTTON);
    name_part(out_model, "buttons");
    add_cylinder(out_model, { { 0.012f, -0.016f, -0.064f }, upright }, 0.006f, 0.002f);
    if (controller != SIM_Controller::KhrSimple)
    {
        add_cylinder(out_model, { { 0.012f, 0.004f, -0.064f }, upright }, 0.006f, 0.002f);
        add_cylinder(out_model, { { -0.01f, -0.006f, -0.067f }, upright }, 0.008f, 0.005f);
    }
    end_part(out_model);

    // Only the first Touch generations track through a ring around the hand
    if (controller == SIM_Controller::OculusTouch)
    {
        begin_part(out_model, SIM_MODEL_RING);
        name_part(out_model, "ring");
        const Quat across = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.5f * zm::PI);
        add_torus(out_model, { { 0.0f, -0.035f, -0.03f }, across }, 0.05f, 0.006f);
        end_part(out_model);
    }

    // The left controller mirrors the right one across X; mirroring flips the winding back
    if (hand == SIM_Device::LeftHand)
    {
        for (u32 v = 0; v < out_model.vertices_count; v++)
        {
            out_model.positions[v].x = -out_model.positions[v].x;
            out_model.normals[v].x   = -out_model.normals[v].x;
        }
        for (u32 i = 0; i + 2 < out_model.indices_count; i += 3)
        {
            const u16 swap            = out_model.indices[i + 1];
            out_model.indices[i + 1]  = out_model.indices[i + 2];
            out_model.indices[i + 2]  = swap;
        }
    }
}


const char* sim_controller_name (SIM_Controller controller)
{
    return (u32)controller < (u32)SIM_Controller::Count ? sim_controller_infos[(u32)controller].name : "";
}

b8 sim_controller_from_name (const char* name, SIM_Controller& out_controller)
{
    for (u32 i = 0; i < (u32)SIM_Controller::Count; i++)
    {
        if (cstr_equal(name, sim_controller_infos[i].name))
        {
            out_controller = (SIM_Controller)i;
            return true;
        }
    }
    out_controller = SIM_Controller::Count;
    return false;
}

SIM_Input sim_controller_slot_input (SIM_Controller controller, SIM_Device hand, SIM_Slot slot)
{
    if ((u32)controller >= (u32)SIM_Controller::Count || !is_hand(hand) || (u32)slot >= (u32)SIM_Slot::Count)
    {
        return SIM_NO_INPUT;
    }
    return sim_slot_inputs[(u32)controller][hand_index(hand)][(u32)slot];
}

const char* sim_slot_name (SIM_Slot slot)
{
    return (u32)slot < (u32)SIM_Slot::Count ? sim_slot_names[(u32)slot] : "";
}

b8 sim_slot_from_name (const char* name, SIM_Slot& out_slot)
{
    for (u32 i = 0; i < (u32)SIM_Slot::Count; i++)
    {
        if (cstr_equal(name, sim_slot_names[i]))
        {
            out_slot = (SIM_Slot)i;
            return true;
        }
    }
    out_slot = SIM_Slot::Count;
    return false;
}

b8 sim_controller_has_input (SIM_Controller controller, SIM_Input input)
{
    if ((u32)controller >= (u32)SIM_Controller::Count || !input_valid(input))
    {
        return false;
    }
    return (sim_controller_infos[(u32)controller].inputs >> (u32)input) & 1u;
}

const char* sim_input_name (SIM_Input input)
{
    return input_valid(input) ? sim_input_infos[(u32)input].name : "";
}

SIM_InputType sim_input_type (SIM_Input input)
{
    return input_valid(input) ? sim_input_infos[(u32)input].type : SIM_InputType::Bool;
}

b8 sim_input_from_name (const char* name, SIM_Input& out_input)
{
    for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
    {
        if (cstr_equal(name, sim_input_infos[i].name))
        {
            out_input = (SIM_Input)i;
            return true;
        }
    }
    out_input = SIM_Input::Count;
    return false;
}

void sim_apply_haptic (SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output, f32 amplitude, i64 duration_ns, f32 frequency)
{
    ASSERT(sim);
    if (!is_hand(device) || (u32)output >= (u32)SIM_HapticOutput::Count)
    {
        ASSERT_MSG(false, "device %u has no haptic output %u", (u32)device, (u32)output);
        return;
    }
    if (!zm::is_finite(amplitude) || !zm::is_finite(frequency))
    {
        return;
    }
    if (duration_ns < 0)
    {
        duration_ns = SIM_MIN_HAPTIC_NS;
    }
    sim->envelope_counts[hand_index(device)][(u32)output] = 0;
    reset_effect(sim, device, output, SIM_HapticKind::Vibration);
    sim->devices[(u32)device].haptics[(u32)output] = {
        .active     = true,
        .amplitude  = zm::clamp(amplitude, 0.0f, 1.0f),
        .frequency  = zm::max(frequency, 0.0f),
        .start_time = sim->time,
        .end_time   = time_after(sim->time, duration_ns),
    };
}

void sim_apply_haptic_envelope (SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output, const f32* amplitudes, u32 count, i64 duration_ns)
{
    if (!sim || !is_hand(device) || (u32)output >= (u32)SIM_HapticOutput::Count || !amplitudes || count == 0)
    {
        ASSERT_MSG(false, "invalid haptic envelope on device %u output %u", (u32)device, (u32)output);
        return;
    }
    for (u32 i = 0; i < count; i++)
    {
        if (!zm::is_finite(amplitudes[i]))
        {
            ASSERT_MSG(false, "haptic envelope sample %u is not finite", i);
            return;
        }
    }
    f32*      samples = sim->envelopes[hand_index(device)][(u32)output];
    const u32 kept    = zm::min(count, (u32)SIM_MAX_HAPTIC_SAMPLES);
    for (u32 i = 0; i < kept; i++)
    {
        // Linear resampling when the app sent more samples than are kept
        const f64 source = kept > 1 ? (f64)i * (f64)(count - 1) / (f64)(kept - 1) : 0.0;
        const u32 first  = (u32)source;
        const u32 next   = zm::min(first + 1, count - 1);
        const f32 frac   = (f32)(source - (f64)first);
        samples[i]       = zm::clamp(amplitudes[first] + (amplitudes[next] - amplitudes[first]) * frac, 0.0f, 1.0f);
    }
    if (duration_ns <= 0)
    {
        duration_ns = SIM_MIN_HAPTIC_NS;
    }
    sim->devices[(u32)device].haptics[(u32)output] = {
        .active     = true,
        .amplitude  = samples[0],
        .start_time = sim->time,
        .end_time   = time_after(sim->time, duration_ns),
    };
    sim->envelope_counts[hand_index(device)][(u32)output] = kept;
    reset_effect(sim, device, output, SIM_HapticKind::Envelope);
}

u32 sim_apply_haptic_pcm (SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output, const f32* samples, u32 count, f32 sample_rate, b8 append)
{
    if (!sim || !is_hand(device) || (u32)output >= (u32)SIM_HapticOutput::Count || (count > 0 && !samples) || !zm::is_finite(sample_rate) || sample_rate <= 0.0f)
    {
        ASSERT_MSG(false, "invalid PCM haptic on device %u output %u", (u32)device, (u32)output);
        return 0;
    }
    SIM_Haptic&       haptic = sim->devices[(u32)device].haptics[(u32)output];
    SIM_HapticEffect& effect = sim->haptic_effects[hand_index(device)][(u32)output];
    f32*              buffer = sim->envelopes[hand_index(device)][(u32)output];
    u32&              stored = sim->envelope_counts[hand_index(device)][(u32)output];

    // Spec XR_FB_haptic_pcm: appending continues an unfinished PCM effect, anything else restarts
    if (!(append && haptic.active && effect.kind == SIM_HapticKind::Pcm))
    {
        reset_effect(sim, device, output, SIM_HapticKind::Pcm);
        stored = 0;
        haptic = { .active = true, .start_time = sim->time };
    }
    const u64 played = zm::min((u64)pcm_samples_elapsed(sim->time - haptic.start_time) - effect.pcm_offset, (u64)stored);
    memory_move(buffer, buffer + played, (stored - (u32)played) * sizeof(f32));
    stored -= (u32)played;
    effect.pcm_offset += played;

    // Linear resampling to the device rate; the input samples a full buffer cannot take are not consumed
    const f64 step     = (f64)sample_rate / (f64)SIM_HAPTIC_PCM_RATE;
    const u32 wanted   = count > 0 ? (u32)((f64)count / step) : 0;
    const u32 produced = zm::min(wanted, (u32)SIM_MAX_HAPTIC_SAMPLES - stored);
    for (u32 i = 0; i < produced; i++)
    {
        const f64 source       = (f64)i * step;
        const u32 first        = (u32)source;
        const u32 next         = zm::min(first + 1, count - 1);
        const f32 frac         = (f32)(source - (f64)first);
        const f32 value        = samples[first] + (samples[next] - samples[first]) * frac;
        buffer[stored + i]     = zm::is_finite(value) ? zm::min(zm::abs(value), 1.0f) : 0.0f;
    }
    stored += produced;
    haptic.end_time = haptic.start_time + pcm_samples_duration(effect.pcm_offset + stored);
    if (stored == 0)
    {
        haptic = {};
    }
    return produced == wanted ? count : zm::min(count, (u32)((f64)produced * step + 0.999));
}

void sim_apply_haptic_parametric (SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output, const SIM_HapticParametric& parametric)
{
    if (!sim || !is_hand(device) || (u32)output >= (u32)SIM_HapticOutput::Count || parametric.amplitudes_count == 0 || !parametric.amplitudes || (parametric.frequencies_count > 0 && !parametric.frequencies))
    {
        ASSERT_MSG(false, "invalid parametric haptic on device %u output %u", (u32)device, (u32)output);
        return;
    }
    SIM_Haptic&       haptic = sim->devices[(u32)device].haptics[(u32)output];
    SIM_HapticEffect& effect = sim->haptic_effects[hand_index(device)][(u32)output];
    const b8          frame  = parametric.stream == SIM_HapticStream::Intermediate || parametric.stream == SIM_HapticStream::Last;
    if (frame && !(effect.kind == SIM_HapticKind::Parametric && effect.stream_running))
    {
        ASSERT_MSG(false, "parametric stream frame without a running stream");
        return;
    }
    if (!frame)
    {
        reset_effect(sim, device, output, SIM_HapticKind::Parametric);
        sim->envelope_counts[hand_index(device)][(u32)output] = 0;
        haptic                = { .start_time = sim->time };
        const b8 unspecified  = parametric.min_hz <= 0.0f && parametric.max_hz <= 0.0f;
        effect.min_hz         = unspecified ? SIM_HAPTIC_MIN_HZ : zm::clamp(parametric.min_hz, SIM_HAPTIC_MIN_HZ, SIM_HAPTIC_MAX_HZ);
        effect.max_hz         = unspecified ? SIM_HAPTIC_MAX_HZ : zm::clamp(parametric.max_hz, effect.min_hz, SIM_HAPTIC_MAX_HZ);
        effect.stream_running = parametric.stream == SIM_HapticStream::First;
        effect.stream_end     = { .amplitude = -1, .frequency = -1, .transient = -1 };
    }
    const i64 now = sim->time - haptic.start_time;
    append_points(effect.amplitudes, effect.amplitudes_count, now, parametric.amplitudes, parametric.amplitudes_count);
    append_points(effect.frequencies, effect.frequencies_count, now, parametric.frequencies, parametric.frequencies_count);
    effect.stream_end.amplitude = parametric.amplitudes[parametric.amplitudes_count - 1].time;
    if (parametric.frequencies_count > 0)
    {
        effect.stream_end.frequency = parametric.frequencies[parametric.frequencies_count - 1].time;
    }
    if (parametric.last_transient >= 0)
    {
        effect.stream_end.transient = parametric.last_transient;
    }
    if (parametric.stream == SIM_HapticStream::Last)
    {
        effect.stream_running = false;
    }

    // Spec: a stream holds its last values for 50 ms waiting for a late frame; an event ends with its last point
    const i64 last  = effect.amplitudes[effect.amplitudes_count - 1].time;
    haptic.active   = true;
    haptic.end_time = haptic.start_time + last + (effect.stream_running ? SIM_HAPTIC_HOLD_NS : 0);
    if (haptic.end_time <= sim->time)
    {
        haptic.end_time = sim->time + SIM_MIN_HAPTIC_NS;
    }
}

b8 sim_haptic_stream_running (const SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output)
{
    ASSERT(sim);
    if (!is_hand(device) || (u32)output >= (u32)SIM_HapticOutput::Count)
    {
        return false;
    }
    const SIM_HapticEffect& effect = sim->haptic_effects[hand_index(device)][(u32)output];
    return effect.kind == SIM_HapticKind::Parametric && effect.stream_running;
}

b8 sim_haptic_stream_end (const SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output, SIM_HapticStreamEnd& out_end)
{
    if (!sim_haptic_stream_running(sim, device, output))
    {
        return false;
    }
    out_end = sim->haptic_effects[hand_index(device)][(u32)output].stream_end;
    return true;
}

void sim_stop_haptic (SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output)
{
    ASSERT(sim);
    if (!is_hand(device) || (u32)output >= (u32)SIM_HapticOutput::Count)
    {
        return;
    }
    sim->devices[(u32)device].haptics[(u32)output] = {};
    sim->envelope_counts[hand_index(device)][(u32)output] = 0;
    reset_effect(sim, device, output, SIM_HapticKind::Vibration);
}

SIM_Haptic sim_get_haptic (const SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output)
{
    ASSERT(sim);
    if (!is_hand(device) || (u32)output >= (u32)SIM_HapticOutput::Count)
    {
        return {};
    }
    SIM_Haptic              haptic = sim->devices[(u32)device].haptics[(u32)output];
    const u32               count  = sim->envelope_counts[hand_index(device)][(u32)output];
    const SIM_HapticEffect& effect = sim->haptic_effects[hand_index(device)][(u32)output];
    if (haptic.active && effect.kind == SIM_HapticKind::Pcm)
    {
        const u64 index  = (u64)pcm_samples_elapsed(sim->time - haptic.start_time) - effect.pcm_offset;
        haptic.amplitude = index < count ? sim->envelopes[hand_index(device)][(u32)output][index] : 0.0f;
    }
    else if (haptic.active && effect.kind == SIM_HapticKind::Parametric)
    {
        const i64 t      = sim->time - haptic.start_time;
        haptic.amplitude = curve_at(effect.amplitudes, effect.amplitudes_count, t, 0.0f);
        haptic.frequency = effect.min_hz + (effect.max_hz - effect.min_hz) * curve_at(effect.frequencies, effect.frequencies_count, t, 0.5f);
    }
    else if (haptic.active && count > 0)
    {
        const f32* samples  = sim->envelopes[hand_index(device)][(u32)output];
        const f64  elapsed  = (f64)(sim->time - haptic.start_time) / (f64)(haptic.end_time - haptic.start_time);
        const f64  progress = elapsed < 0.0 ? 0.0 : (elapsed > 1.0 ? 1.0 : elapsed);
        const f64  position = progress * (f64)(count - 1);
        const u32  first    = (u32)position;
        const u32  next     = zm::min(first + 1, count - 1);
        haptic.amplitude    = samples[first] + (samples[next] - samples[first]) * (f32)(position - (f64)first);
    }
    return haptic;
}

//
// Body tracking
//

const SIM_BodyState& sim_get_body (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->body;
}

void sim_default_body (f32 height, SIM_BodyState& out_state)
{
    out_state        = {};
    out_state.height = zm::is_finite(height) ? zm::clamp(height, SIM_BODY_MIN_HEIGHT, SIM_BODY_MAX_HEIGHT) : SIM_BODY_REFERENCE_HEIGHT;
    for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
    {
        out_state.bone_scales[j] = 1.0f;
        out_state.confidences[j] = 1.0f;
        out_state.tracked[j]     = true;
    }
    body_preset(SIM_BodyPose::Stand, out_state.rotations);
}

void sim_set_body (SIM_Sim* sim, const SIM_BodyState& state)
{
    ASSERT(sim);
    SIM_BodyState& body  = sim->body;
    b8             shape = false;
    const f32      height = zm::is_finite(state.height) ? zm::clamp(state.height, SIM_BODY_MIN_HEIGHT, SIM_BODY_MAX_HEIGHT) : body.height;
    shape                = height != body.height;
    body.height          = height;
    for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
    {
        const f32 scale = body_scalable(j) && zm::is_finite(state.bone_scales[j]) ? zm::clamp(state.bone_scales[j], SIM_BODY_MIN_SCALE, SIM_BODY_MAX_SCALE) : 1.0f;
        shape           = shape || scale != body.bone_scales[j];
        body.bone_scales[j] = scale;

        body.rotations[j]   = body_unit(state.rotations[j]);
        body.confidences[j] = zm::is_finite(state.confidences[j]) ? zm::clamp(state.confidences[j], 0.0f, 1.0f) : 1.0f;
        body.tracked[j]     = state.tracked[j];
    }
    for (u32 l = 0; l < (u32)SIM_Limb::Count; l++)
    {
        body.limbs[l] = (u32)state.limbs[l] < (u32)SIM_LimbMode::Count ? state.limbs[l] : SIM_LimbMode::Follow;
    }
    if (shape)
    {
        sim->body_generation++;
    }
}

void sim_set_body_pose (SIM_Sim* sim, SIM_BodyPose pose)
{
    ASSERT(sim);
    if ((u32)pose >= (u32)SIM_BodyPose::Count)
    {
        ASSERT_MSG(false, "body pose %u", (u32)pose);
        return;
    }
    body_preset(pose, sim->body.rotations);
    if (pose == SIM_BodyPose::Sit)
    {
        sim->body.limbs[(u32)SIM_Limb::LeftLeg]  = SIM_LimbMode::Posed;
        sim->body.limbs[(u32)SIM_Limb::RightLeg] = SIM_LimbMode::Posed;
    }
}

void sim_set_body_joint (SIM_Sim* sim, SIM_BodyJoint joint, Quat rotation)
{
    ASSERT(sim);
    const f32 length = zm::length(rotation);
    if ((u32)joint >= SIM_BODY_JOINT_COUNT || !zm::is_finite(length) || length < 0.5f || length > 2.0f)
    {
        ASSERT_MSG(false, "body joint %u", (u32)joint);
        return;
    }
    sim->body.rotations[(u32)joint] = body_unit(rotation);
    const SIM_Limb limb             = body_limb((u32)joint);
    if (limb != SIM_Limb::Count)
    {
        sim->body.limbs[(u32)limb] = SIM_LimbMode::Posed;
    }
}

u32 sim_body_generation (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->body_generation;
}

u32 sim_body_skeleton_changes (const SIM_Sim* sim, i64 time)
{
    ASSERT(sim);
    const u32 periods = sim->skeleton_change_period > 0 && time > 0 ? (u32)(time / sim->skeleton_change_period) : 0u;
    return sim->body_generation + periods;
}

i64 sim_get_skeleton_change_period (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->skeleton_change_period;
}

void sim_set_skeleton_change_period (SIM_Sim* sim, i64 period_ns)
{
    ASSERT(sim);
    sim->skeleton_change_period = zm::max(period_ns, (i64)0);
}

void sim_body_skeleton (const SIM_Sim* sim, SIM_BodySkeleton& out_skeleton)
{
    ASSERT(sim);
    out_skeleton = {};
    Vec3 rest[SIM_BODY_JOINT_COUNT];
    Pose hands[2][SIM_HAND_JOINT_COUNT];
    body_rest(sim, sim->body, rest, hands);
    for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
    {
        u32 first = 0;
        if (body_hand_joint(j, first))
        {
            const b8 left        = first == (u32)SIM_BodyJoint::LeftHandPalm;
            const Pose& hand     = hands[left ? 0 : 1][j - first];
            out_skeleton.poses[j] = { hand.position, zm::normalize(hand.orientation * body_hand_basis(left)) };
        }
        else
        {
            out_skeleton.poses[j] = { rest[j], body_basis(j) };
        }
        out_skeleton.parents[j] = body_parent(j);
    }
}

void sim_body_joints (const SIM_Sim* sim, const SIM_Sample& sample, SIM_BodyJoints& out_joints, b8 fingers)
{
    ASSERT(sim);
    out_joints               = {};
    const SIM_BodyState& body = sim->body;
    const f32            k    = body.height / SIM_BODY_REFERENCE_HEIGHT;
    Vec3                 rest[SIM_BODY_JOINT_COUNT];
    Pose                 bind[2][SIM_HAND_JOINT_COUNT];
    body_rest(sim, body, rest, bind);

    // Forward kinematics in the body's space; the hand joints past the wrist come from the hands
    Pose frames[SIM_BODY_JOINT_COUNT];
    frames[0] = zm::POSE_IDENTITY;
    for (u32 j = 1; j < SIM_BODY_JOINT_COUNT; j++)
    {
        u32 first = 0;
        if (body_hand_joint(j, first) && j != first + (u32)SIM_HandJoint::Wrist)
        {
            continue;
        }
        const Pose& parent = frames[body_parent(j)];
        frames[j]          = { parent.position + zm::rotate(parent.orientation, rest[j] - rest[body_parent(j)]), zm::normalize(parent.orientation * body.rotations[j]) };
    }

    // The head joint is where the view puts it; the body hangs from it, facing where the view does
    const Pose& view  = sample.devices[(u32)SIM_Device::Head].pose;
    const Quat  yaw   = yaw_only(view.orientation);
    const Vec3  head  = view.position - zm::rotate(yaw, SIM_BODY_EYES_IN_HEAD * k);
    const Pose  place = { head - zm::rotate(yaw, frames[(u32)SIM_BodyJoint::Head].position), yaw };
    for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
    {
        frames[j] = zm::pose_mul(place, frames[j]);
    }
    frames[(u32)SIM_BodyJoint::Head].orientation = view.orientation;

    for (u32 side = 0; side < 2; side++)
    {
        const b8         left   = side == 0;
        const SIM_Device device = left ? SIM_Device::LeftHand : SIM_Device::RightHand;
        const u32        upper  = (u32)(left ? SIM_BodyJoint::LeftArmUpper : SIM_BodyJoint::RightArmUpper);
        const u32        first  = (u32)(left ? SIM_BodyJoint::LeftHandPalm : SIM_BodyJoint::RightHandPalm);
        const u32        wrist  = first + (u32)SIM_HandJoint::Wrist;
        const SIM_DeviceState& state = sample.devices[(u32)device];
        const b8         held   = state.active && state.tracked;
        SIM_HandJoints   hand   = {};
        if (held)
        {
            sim_hand_joints(sim, device, state.pose, SIM_HandMotion::Unobstructed, hand);
        }

        Pose hand_poses[SIM_HAND_JOINT_COUNT];
        if (held && body.limbs[(u32)(left ? SIM_Limb::LeftArm : SIM_Limb::RightArm)] == SIM_LimbMode::Follow)
        {
            const Vec3 pole_rest = { left ? -SIM_BODY_ELBOW_POLE.x : SIM_BODY_ELBOW_POLE.x, SIM_BODY_ELBOW_POLE.y, SIM_BODY_ELBOW_POLE.z };
            const Vec3 pole      = zm::rotate(frames[(u32)SIM_BodyJoint::Chest].orientation, pole_rest);
            const Vec3 start     = frames[upper].position;
            const Vec3 target    = hand.poses[(u32)SIM_HandJoint::Wrist].position;
            const f32  a         = zm::length(rest[upper + 1] - rest[upper]);
            const f32  b         = zm::length(rest[wrist] - rest[upper + 1]);
            Vec3       middle    = {};
            Vec3       end       = {};
            Vec3       bend      = {};
            two_bone_ik(start, target, pole, a, b, SIM_BODY_ELBOW_FOLD, middle, end, bend);
            const Vec3 rest_normal = limb_normal(rest[wrist] - rest[upper], pole_rest);
            const Vec3 normal      = zm::normalize(zm::cross(zm::normalize(end - start), bend));
            frames[upper].orientation = bone_rotation(rest[upper + 1] - rest[upper], rest_normal, middle - start, normal);
            frames[upper + 1]         = { middle, bone_rotation(rest[wrist] - rest[upper + 1], rest_normal, end - middle, normal) };
            frames[upper + 2]         = { middle + zm::rotate(frames[upper + 1].orientation, rest[upper + 2] - rest[upper + 1]), frames[upper + 1].orientation };
            const Vec3 shift          = end - target;
            for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
            {
                hand_poses[j] = { hand.poses[j].position + shift, hand.poses[j].orientation };
            }
        }
        else if (!held && body_fingers_posed(body, first))
        {
            // Forward kinematics on the T-pose hand from the posed wrist
            Pose posed[SIM_HAND_JOINT_COUNT];
            posed[(u32)SIM_HandJoint::Wrist] = frames[wrist];
            for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
            {
                if (j == (u32)SIM_HandJoint::Wrist)
                {
                    continue;
                }
                const u32   p      = (u32)body_parent(first + j) - first;
                const Pose& parent = posed[p];
                posed[j]           = { parent.position + zm::rotate(parent.orientation, bind[side][j].position - bind[side][p].position), zm::normalize(parent.orientation * body.rotations[first + j]) };
            }
            for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
            {
                hand_poses[j] = { posed[j].position, zm::normalize(posed[j].orientation * bind[side][j].orientation) };
            }
        }
        else
        {
            const Pose  root   = { frames[wrist].position, zm::normalize(frames[wrist].orientation * body_hand_rest(left)) };
            const Pose& origin = held ? hand.poses[(u32)SIM_HandJoint::Wrist] : bind[side][(u32)SIM_HandJoint::Wrist];
            for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
            {
                hand_poses[j] = zm::pose_mul(root, zm::pose_relative(origin, held ? hand.poses[j] : bind[side][j]));
            }
        }
        if (!fingers)
        {
            const Pose  held_wrist = hand_poses[(u32)SIM_HandJoint::Wrist];
            const Pose& open_wrist = bind[side][(u32)SIM_HandJoint::Wrist];
            for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
            {
                hand_poses[j] = zm::pose_mul(held_wrist, zm::pose_relative(open_wrist, bind[side][j]));
            }
        }
        const Quat basis = body_hand_basis(left);
        for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
        {
            frames[first + j] = { hand_poses[j].position, zm::normalize(hand_poses[j].orientation * basis) };
        }
    }

    for (u32 side = 0; side < 2; side++)
    {
        const u32 thigh = (u32)(side == 0 ? SIM_BodyJoint::LeftUpperLeg : SIM_BodyJoint::RightUpperLeg);
        if (body.limbs[(u32)(side == 0 ? SIM_Limb::LeftLeg : SIM_Limb::RightLeg)] != SIM_LimbMode::Follow)
        {
            continue;
        }
        const u32  twist  = thigh + 2;
        const Vec3 start  = frames[thigh].position;
        const f32  a      = zm::length(rest[thigh + 1] - rest[thigh]);
        const f32  b      = zm::length(rest[twist] - rest[thigh + 1]);
        const f32  fold   = zm::sqrt(zm::max(a * a + b * b - 2.0f * a * b * zm::cos(SIM_BODY_KNEE_FOLD), 0.0f));
        Vec3       target = start + zm::rotate(yaw, rest[twist] - rest[thigh]);
        target.y          = zm::min(rest[twist].y, start.y - fold);
        const Vec3 pole   = zm::rotate(yaw, SIM_BODY_KNEE_POLE);
        Vec3       knee   = {};
        Vec3       ankle  = {};
        Vec3       bend   = {};
        two_bone_ik(start, target, pole, a, b, SIM_BODY_KNEE_FOLD, knee, ankle, bend);
        const Vec3 rest_normal = limb_normal(rest[twist] - rest[thigh], SIM_BODY_KNEE_POLE);
        const Vec3 normal      = zm::normalize(zm::cross(zm::normalize(ankle - start), bend));
        frames[thigh].orientation = bone_rotation(rest[thigh + 1] - rest[thigh], rest_normal, knee - start, normal);
        frames[thigh + 1]         = { knee, bone_rotation(rest[twist] - rest[thigh + 1], rest_normal, ankle - knee, normal) };
        frames[twist]             = { ankle, frames[thigh + 1].orientation };
        for (u32 j = twist + 1; j < thigh + 7; j++)
        {
            frames[j] = { frames[j - 1].position + zm::rotate(yaw, rest[j] - rest[j - 1]), yaw };
        }
    }

    const Vec3 hips = frames[(u32)SIM_BodyJoint::Hips].position;
    frames[0]       = { { hips.x, 0.0f, hips.z }, yaw };

    for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
    {
        u32 first = 0;
        out_joints.poses[j]   = body_hand_joint(j, first) ? frames[j] : Pose{ frames[j].position, zm::normalize(frames[j].orientation * body_basis(j)) };
        out_joints.tracked[j] = body.tracked[j];
    }
    out_joints.confidence = sample.devices[(u32)SIM_Device::Head].active ? 1.0f : 0.0f;
}

void sim_reset_body (SIM_Sim* sim)
{
    ASSERT(sim);
    sim_set_body(sim, sim->config.body);
}

static b8 body_key (const JsonMember& member, char (&out_key)[48])
{
    return json_as_string(member.key, out_key, nullptr, sizeof(out_key));
}

// A rotation given as [x, y, z, w]; one near unit length is kept as written so saved bodies load
// bit-exactly
static b8 body_rotation (const JsonValue& value, Quat& out_rotation)
{
    f32 parts[4] = {};
    if (!json_is_array(value) || json_length(value) != 4)
    {
        return false;
    }
    for (u32 i = 0; i < 4; i++)
    {
        if (!json_as_float(json_at(value, i), parts[i]) || !zm::is_finite(parts[i]))
        {
            return false;
        }
    }
    const Quat rotation = { parts[0], parts[1], parts[2], parts[3] };
    const f32  length   = zm::length(rotation);
    if (!zm::is_finite(length) || length < 0.5f || length > 2.0f)
    {
        return false;
    }
    out_rotation = body_unit(rotation);
    return true;
}

static b8 body_joint_json (SIM_BodyJoint joint, const JsonValue& value, SIM_BodyState& state)
{
    if (!json_is_object(value))
    {
        return false;
    }
    const u32 j = (u32)joint;
    for (const JsonMember member : json_members(value))
    {
        char key[48] = {};
        if (!body_key(member, key))
        {
            return false;
        }
        if (cstr_equal(key, "rotation"))
        {
            if (!body_rotation(member.value, state.rotations[j]))
            {
                return false;
            }
            const SIM_Limb limb = body_limb(j);
            if (limb != SIM_Limb::Count)
            {
                state.limbs[(u32)limb] = SIM_LimbMode::Posed;
            }
        }
        else if (cstr_equal(key, "tracked"))
        {
            if (!json_is_bool(member.value) || !json_as_bool(member.value, state.tracked[j]))
            {
                return false;
            }
        }
        else if (cstr_equal(key, "confidence"))
        {
            f32 confidence = 0.0f;
            if (!json_as_float(member.value, confidence) || !zm::is_finite(confidence) || confidence < 0.0f || confidence > 1.0f)
            {
                return false;
            }
            state.confidences[j] = confidence;
        }
        else
        {
            return false;
        }
    }
    return true;
}

// Applies height, pose, bones, joints and limbs in that order, so a joint edits the pose and a
// limb mode wins over the posing a joint rotation implies
static b8 body_from_json (const JsonValue& root, SIM_BodyState& state)
{
    if (!json_is_object(root))
    {
        return false;
    }
    const JsonValue height = json_find(root, "height");
    if (json_is_valid(height) && (!json_as_float(height, state.height) || !zm::is_finite(state.height) || state.height < SIM_BODY_MIN_HEIGHT || state.height > SIM_BODY_MAX_HEIGHT))
    {
        return false;
    }
    const JsonValue pose = json_find(root, "pose");
    if (json_is_valid(pose))
    {
        char         name[32] = {};
        SIM_BodyPose preset   = {};
        if (!json_as_string(pose, name, nullptr, sizeof(name)) || !sim_body_pose_from_name(name, preset))
        {
            return false;
        }
        body_preset(preset, state.rotations);
        if (preset == SIM_BodyPose::Sit)
        {
            state.limbs[(u32)SIM_Limb::LeftLeg]  = SIM_LimbMode::Posed;
            state.limbs[(u32)SIM_Limb::RightLeg] = SIM_LimbMode::Posed;
        }
    }
    const JsonValue bones = json_find(root, "bones");
    if (json_is_valid(bones))
    {
        if (!json_is_object(bones))
        {
            return false;
        }
        for (const JsonMember member : json_members(bones))
        {
            char          key[48] = {};
            SIM_BodyJoint joint   = {};
            f32           scale   = 0.0f;
            if (!body_key(member, key) || !sim_body_joint_from_name(key, joint) || !body_scalable((u32)joint) || !json_as_float(member.value, scale) || !zm::is_finite(scale) || scale < SIM_BODY_MIN_SCALE || scale > SIM_BODY_MAX_SCALE)
            {
                return false;
            }
            state.bone_scales[(u32)joint] = scale;
        }
    }
    const JsonValue joints = json_find(root, "joints");
    if (json_is_valid(joints))
    {
        if (!json_is_object(joints))
        {
            return false;
        }
        for (const JsonMember member : json_members(joints))
        {
            char          key[48] = {};
            SIM_BodyJoint joint   = {};
            if (!body_key(member, key) || !sim_body_joint_from_name(key, joint) || !body_joint_json(joint, member.value, state))
            {
                return false;
            }
        }
    }
    const JsonValue limbs = json_find(root, "limbs");
    if (json_is_valid(limbs))
    {
        if (!json_is_object(limbs))
        {
            return false;
        }
        for (const JsonMember member : json_members(limbs))
        {
            char     key[48]  = {};
            char     mode[16] = {};
            SIM_Limb limb     = {};
            if (!body_key(member, key) || !sim_limb_from_name(key, limb) || !json_as_string(member.value, mode, nullptr, sizeof(mode)))
            {
                return false;
            }
            if (!cstr_equal(mode, "follow") && !cstr_equal(mode, "posed"))
            {
                return false;
            }
            state.limbs[(u32)limb] = cstr_equal(mode, "posed") ? SIM_LimbMode::Posed : SIM_LimbMode::Follow;
        }
    }
    for (const JsonMember member : json_members(root))
    {
        char key[48] = {};
        if (!body_key(member, key) || (!cstr_equal(key, "height") && !cstr_equal(key, "pose") && !cstr_equal(key, "bones") && !cstr_equal(key, "joints") && !cstr_equal(key, "limbs")))
        {
            return false;
        }
    }
    return true;
}

b8 sim_body_apply_json (const JsonValue& root, SIM_BodyState& inout_state)
{
    SIM_BodyState state = inout_state;
    if (!body_from_json(root, state))
    {
        return false;
    }
    inout_state = state;
    return true;
}

namespace
{

struct BodyText
{
    char* out;
    usize size;
    usize used;
    b8    overflow;
};

} // namespace

static void body_text (BodyText& text, const char* fmt, ...) PRINTF_LIKE(2, 3);
static void body_text (BodyText& text, const char* fmt, ...)
{
    if (text.overflow)
    {
        return;
    }
    va_list args = {};
    va_start(args, fmt);
    const usize written = cstr_format_v(text.out + text.used, text.size - text.used, fmt, args);
    va_end(args);
    if (written >= text.size - text.used)
    {
        text.overflow = true;
        return;
    }
    text.used += written;
}

static void body_float (BodyText& text, f32 value)
{
    char number[32];
    json_format_f32(number, sizeof(number), value);
    body_text(text, "%s", number);
}

// What differs from the standing default; the floats read back bit-exactly
b8 sim_body_to_json (const SIM_BodyState& state, char* out, usize size)
{
    ASSERT(out && size > 0);
    SIM_BodyState defaults = {};
    sim_default_body(state.height, defaults);
    BodyText text = { .out = out, .size = size };
    body_text(text, "{\"height\":");
    body_float(text, state.height);
    body_text(text, ",\"bones\":{");
    const char* separator = "";
    for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
    {
        if (state.bone_scales[j] != 1.0f)
        {
            body_text(text, "%s\"%s\":", separator, sim_body_joint_name((SIM_BodyJoint)j));
            body_float(text, state.bone_scales[j]);
            separator = ",";
        }
    }
    body_text(text, "},\"joints\":{");
    separator = "";
    for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
    {
        const Quat r        = state.rotations[j];
        const b8   rotated  = !zm::equal(r, defaults.rotations[j]);
        const b8   tracking = !state.tracked[j] || state.confidences[j] != 1.0f;
        if (!rotated && !tracking)
        {
            continue;
        }
        body_text(text, "%s\"%s\":{", separator, sim_body_joint_name((SIM_BodyJoint)j));
        if (rotated)
        {
            const f32 parts[4] = { r.x, r.y, r.z, r.w };
            body_text(text, "\"rotation\":[");
            for (u32 i = 0; i < 4; i++)
            {
                body_text(text, i ? "," : "");
                body_float(text, parts[i]);
            }
            body_text(text, "]%s", tracking ? "," : "");
        }
        if (tracking)
        {
            body_text(text, "\"tracked\":%s,\"confidence\":", state.tracked[j] ? "true" : "false");
            body_float(text, state.confidences[j]);
        }
        body_text(text, "}");
        separator = ",";
    }
    body_text(text, "},\"limbs\":{");
    for (u32 l = 0; l < (u32)SIM_Limb::Count; l++)
    {
        body_text(text, "%s\"%s\":\"%s\"", l > 0 ? "," : "", sim_limb_name((SIM_Limb)l), state.limbs[l] == SIM_LimbMode::Posed ? "posed" : "follow");
    }
    body_text(text, "}}");
    return !text.overflow;
}

b8 sim_body_joint_scalable (SIM_BodyJoint joint)
{
    return body_scalable((u32)joint);
}

SIM_Limb sim_body_joint_limb (SIM_BodyJoint joint)
{
    return (u32)joint < SIM_BODY_JOINT_COUNT ? body_limb((u32)joint) : SIM_Limb::Count;
}

const char* sim_body_joint_name (SIM_BodyJoint joint)
{
    const u32 j = (u32)joint;
    if (j >= SIM_BODY_JOINT_COUNT)
    {
        return "none";
    }
    if (j < 18)
    {
        return sim_body_torso_names[j];
    }
    if (j >= (u32)SIM_BodyJoint::LeftUpperLeg)
    {
        return sim_body_leg_names[j - (u32)SIM_BodyJoint::LeftUpperLeg];
    }
    return j < (u32)SIM_BodyJoint::RightHandPalm ? sim_body_hand_names[j - (u32)SIM_BodyJoint::LeftHandPalm] : sim_body_right_hand_names[j - (u32)SIM_BodyJoint::RightHandPalm];
}

b8 sim_body_joint_from_name (const char* name, SIM_BodyJoint& out_joint)
{
    for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
    {
        if (cstr_equal(name, sim_body_joint_name((SIM_BodyJoint)j)))
        {
            out_joint = (SIM_BodyJoint)j;
            return true;
        }
    }
    return false;
}

const char* sim_body_pose_name (SIM_BodyPose pose)
{
    return (u32)pose < (u32)SIM_BodyPose::Count ? sim_body_pose_names[(u32)pose] : "none";
}

b8 sim_body_pose_from_name (const char* name, SIM_BodyPose& out_pose)
{
    for (u32 p = 0; p < (u32)SIM_BodyPose::Count; p++)
    {
        if (cstr_equal(name, sim_body_pose_names[p]))
        {
            out_pose = (SIM_BodyPose)p;
            return true;
        }
    }
    return false;
}

const char* sim_limb_name (SIM_Limb limb)
{
    return (u32)limb < (u32)SIM_Limb::Count ? sim_limb_names[(u32)limb] : "none";
}

b8 sim_limb_from_name (const char* name, SIM_Limb& out_limb)
{
    for (u32 l = 0; l < (u32)SIM_Limb::Count; l++)
    {
        if (cstr_equal(name, sim_limb_names[l]))
        {
            out_limb = (SIM_Limb)l;
            return true;
        }
    }
    return false;
}

//
// Face and eye tracking
//

const SIM_FaceState& sim_get_face (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->face;
}

void sim_default_face (SIM_FaceState& out_state)
{
    out_state                = {};
    out_state.confidences[0] = 1.0f;
    out_state.confidences[1] = 1.0f;
    out_state.blink          = true;
}

void sim_set_face (SIM_Sim* sim, const SIM_FaceState& state)
{
    ASSERT(sim);
    SIM_FaceState& face = sim->face;
    for (u32 e = 0; e < SIM_FACE_EXPRESSION_COUNT; e++)
    {
        face.weights[e] = zm::is_finite(state.weights[e]) ? zm::clamp(state.weights[e], 0.0f, 1.0f) : face.weights[e];
    }
    for (u32 v = 0; v < SIM_FACE_VISEME_COUNT; v++)
    {
        face.visemes[v] = zm::is_finite(state.visemes[v]) ? zm::clamp(state.visemes[v], 0.0f, 1.0f) : face.visemes[v];
    }
    for (u32 c = 0; c < 2; c++)
    {
        face.confidences[c] = zm::is_finite(state.confidences[c]) ? zm::clamp(state.confidences[c], 0.0f, 1.0f) : face.confidences[c];
    }
    face.blink      = state.blink;
    face.gaze_yaw   = zm::is_finite(state.gaze_yaw) ? zm::clamp(state.gaze_yaw, -SIM_FACE_MAX_GAZE_YAW, SIM_FACE_MAX_GAZE_YAW) : face.gaze_yaw;
    face.gaze_pitch = zm::is_finite(state.gaze_pitch) ? zm::clamp(state.gaze_pitch, -SIM_FACE_MAX_GAZE_PITCH, SIM_FACE_MAX_GAZE_PITCH) : face.gaze_pitch;
    face.look_at    = state.look_at && zm::is_finite(state.target.x) && zm::is_finite(state.target.y) && zm::is_finite(state.target.z);
    face.target     = face.look_at ? state.target : Vec3{};
}

void sim_reset_face (SIM_Sim* sim)
{
    ASSERT(sim);
    sim_set_face(sim, sim->config.face);
}

void sim_set_face_preset (SIM_Sim* sim, SIM_FacePreset preset)
{
    ASSERT(sim);
    if ((u32)preset >= (u32)SIM_FacePreset::Count)
    {
        ASSERT_MSG(false, "face preset %u", (u32)preset);
        return;
    }
    face_preset(preset, sim->face.weights);
}

void sim_face_frame (const SIM_Sim* sim, const SIM_Sample& sample, SIM_FaceFrame& out_frame)
{
    ASSERT(sim);
    const SIM_FaceState& face = sim->face;
    out_frame                 = {};
    memory_copy(out_frame.weights, face.weights, sizeof(face.weights));
    memory_copy(out_frame.visemes, face.visemes, sizeof(face.visemes));
    memory_copy(out_frame.confidences, face.confidences, sizeof(face.confidences));

    const Pose& head  = sample.devices[(u32)SIM_Device::Head].pose;
    const Quat  turn  = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, face.gaze_yaw * zm::DEG_TO_RAD) * zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, face.gaze_pitch * zm::DEG_TO_RAD);
    const Vec3  ahead = zm::rotate(head.orientation * turn, { 0.0f, 0.0f, -1.0f });
    const Vec3  up    = zm::rotate(head.orientation, { 0.0f, 1.0f, 0.0f });
    const f32   blink = face.blink ? face_blink(sample.time) : 0.0f;
    for (u32 e = 0; e < 2; e++)
    {
        const Vec3 eye = head.position + zm::rotate(head.orientation, { (e == 0 ? -0.5f : 0.5f) * sim->config.ipd, 0.0f, 0.0f });
        Vec3       dir = ahead;
        if (face.look_at && zm::length_sq(face.target - eye) > 1e-8f)
        {
            dir = zm::normalize(face.target - eye);
        }
        out_frame.eyes[e] = { eye, zm::quat_look_rotation(dir, up) };

        // The eye blendshapes are the gaze's angles from the head over the eyes' range
        const Vec3 local = zm::rotate(zm::inverse(head.orientation), dir);
        const f32  yaw   = zm::atan2(-local.x, -local.z) * zm::RAD_TO_DEG;
        const f32  pitch = zm::atan2(local.y, zm::sqrt(local.x * local.x + local.z * local.z)) * zm::RAD_TO_DEG;
        f32* const w     = out_frame.weights;
        w[(u32)SIM_FaceExpression::EyesLookLeftL + e]  = zm::clamp(yaw / SIM_FACE_MAX_GAZE_YAW, 0.0f, 1.0f);
        w[(u32)SIM_FaceExpression::EyesLookRightL + e] = zm::clamp(-yaw / SIM_FACE_MAX_GAZE_YAW, 0.0f, 1.0f);
        w[(u32)SIM_FaceExpression::EyesLookUpL + e]    = zm::clamp(pitch / SIM_FACE_MAX_GAZE_PITCH, 0.0f, 1.0f);
        w[(u32)SIM_FaceExpression::EyesLookDownL + e]  = zm::clamp(-pitch / SIM_FACE_MAX_GAZE_PITCH, 0.0f, 1.0f);
        w[(u32)SIM_FaceExpression::EyesClosedL + e]    = zm::max(w[(u32)SIM_FaceExpression::EyesClosedL + e], blink);
    }
}

static b8 face_key (const JsonMember& member, char (&out_key)[48])
{
    return json_as_string(member.key, out_key, nullptr, sizeof(out_key));
}

static b8 face_number (const JsonValue& value, f32 low, f32 high, f32& out_value)
{
    f32 number = 0.0f;
    if (!json_as_float(value, number) || !zm::is_finite(number) || number < low || number > high)
    {
        return false;
    }
    out_value = number;
    return true;
}

// Applies preset, then weights, visemes, confidences, blink and the gaze
static b8 face_from_json (const JsonValue& root, SIM_FaceState& state)
{
    if (!json_is_object(root))
    {
        return false;
    }
    for (const JsonMember member : json_members(root))
    {
        char key[48] = {};
        if (!face_key(member, key) || (!cstr_equal(key, "preset") && !cstr_equal(key, "weights") && !cstr_equal(key, "visemes") && !cstr_equal(key, "confidences") && !cstr_equal(key, "blink") && !cstr_equal(key, "gaze") && !cstr_equal(key, "look_at")))
        {
            return false;
        }
    }
    const JsonValue preset = json_find(root, "preset");
    if (json_is_valid(preset))
    {
        char           name[32] = {};
        SIM_FacePreset chosen   = {};
        if (!json_as_string(preset, name, nullptr, sizeof(name)) || !sim_face_preset_from_name(name, chosen))
        {
            return false;
        }
        face_preset(chosen, state.weights);
    }
    const JsonValue weights = json_find(root, "weights");
    if (json_is_valid(weights))
    {
        if (!json_is_object(weights))
        {
            return false;
        }
        for (const JsonMember member : json_members(weights))
        {
            char               key[48]    = {};
            SIM_FaceExpression expression = {};
            if (!face_key(member, key) || !sim_face_expression_from_name(key, expression) || !face_number(member.value, 0.0f, 1.0f, state.weights[(u32)expression]))
            {
                return false;
            }
        }
    }
    const JsonValue visemes = json_find(root, "visemes");
    if (json_is_valid(visemes))
    {
        if (!json_is_object(visemes))
        {
            return false;
        }
        for (const JsonMember member : json_members(visemes))
        {
            char key[48] = {};
            u32  viseme  = 0;
            if (!face_key(member, key) || !sim_face_viseme_from_name(key, viseme) || !face_number(member.value, 0.0f, 1.0f, state.visemes[viseme]))
            {
                return false;
            }
        }
    }
    const JsonValue confidences = json_find(root, "confidences");
    if (json_is_valid(confidences) && (!json_is_array(confidences) || json_length(confidences) != 2 || !face_number(json_at(confidences, 0), 0.0f, 1.0f, state.confidences[0]) || !face_number(json_at(confidences, 1), 0.0f, 1.0f, state.confidences[1])))
    {
        return false;
    }
    const JsonValue blink = json_find(root, "blink");
    if (json_is_valid(blink) && (!json_is_bool(blink) || !json_as_bool(blink, state.blink)))
    {
        return false;
    }
    const JsonValue gaze = json_find(root, "gaze");
    if (json_is_valid(gaze) && (!json_is_array(gaze) || json_length(gaze) != 2 || !face_number(json_at(gaze, 0), -SIM_FACE_MAX_GAZE_YAW, SIM_FACE_MAX_GAZE_YAW, state.gaze_yaw) || !face_number(json_at(gaze, 1), -SIM_FACE_MAX_GAZE_PITCH, SIM_FACE_MAX_GAZE_PITCH, state.gaze_pitch)))
    {
        return false;
    }
    const JsonValue look_at = json_find(root, "look_at");
    if (json_is_null(look_at))
    {
        state.look_at = false;
        state.target  = {};
    }
    else if (json_is_valid(look_at))
    {
        Vec3 target = {};
        if (!json_is_array(look_at) || json_length(look_at) != 3 || !face_number(json_at(look_at, 0), -1e6f, 1e6f, target.x) || !face_number(json_at(look_at, 1), -1e6f, 1e6f, target.y) || !face_number(json_at(look_at, 2), -1e6f, 1e6f, target.z))
        {
            return false;
        }
        state.look_at = true;
        state.target  = target;
    }
    return true;
}

b8 sim_face_apply_json (const JsonValue& root, SIM_FaceState& inout_state)
{
    SIM_FaceState state = inout_state;
    if (!face_from_json(root, state))
    {
        return false;
    }
    inout_state = state;
    return true;
}

// The body's JSON writer serves the face too
b8 sim_face_to_json (const SIM_FaceState& state, char* out, usize size)
{
    ASSERT(out && size > 0);
    BodyText text = { .out = out, .size = size };
    body_text(text, "{\"blink\":%s,\"weights\":{", state.blink ? "true" : "false");
    const char* separator = "";
    for (u32 e = 0; e < SIM_FACE_EXPRESSION_COUNT; e++)
    {
        if (state.weights[e] != 0.0f)
        {
            body_text(text, "%s\"%s\":", separator, sim_face_expression_names[e]);
            body_float(text, state.weights[e]);
            separator = ",";
        }
    }
    body_text(text, "},\"visemes\":{");
    separator = "";
    for (u32 v = 0; v < SIM_FACE_VISEME_COUNT; v++)
    {
        if (state.visemes[v] != 0.0f)
        {
            body_text(text, "%s\"%s\":", separator, sim_face_viseme_names[v]);
            body_float(text, state.visemes[v]);
            separator = ",";
        }
    }
    body_text(text, "},\"confidences\":[");
    body_float(text, state.confidences[0]);
    body_text(text, ",");
    body_float(text, state.confidences[1]);
    body_text(text, "],\"gaze\":[");
    body_float(text, state.gaze_yaw);
    body_text(text, ",");
    body_float(text, state.gaze_pitch);
    body_text(text, "]");
    if (state.look_at)
    {
        body_text(text, ",\"look_at\":[");
        body_float(text, state.target.x);
        body_text(text, ",");
        body_float(text, state.target.y);
        body_text(text, ",");
        body_float(text, state.target.z);
        body_text(text, "]");
    }
    body_text(text, "}");
    return !text.overflow;
}

const char* sim_face_expression_name (SIM_FaceExpression expression)
{
    return (u32)expression < SIM_FACE_EXPRESSION_COUNT ? sim_face_expression_names[(u32)expression] : "none";
}

b8 sim_face_expression_from_name (const char* name, SIM_FaceExpression& out_expression)
{
    for (u32 e = 0; e < SIM_FACE_EXPRESSION_COUNT; e++)
    {
        if (cstr_equal(name, sim_face_expression_names[e]))
        {
            out_expression = (SIM_FaceExpression)e;
            return true;
        }
    }
    return false;
}

const char* sim_face_viseme_name (u32 viseme)
{
    return viseme < SIM_FACE_VISEME_COUNT ? sim_face_viseme_names[viseme] : "none";
}

b8 sim_face_viseme_from_name (const char* name, u32& out_viseme)
{
    for (u32 v = 0; v < SIM_FACE_VISEME_COUNT; v++)
    {
        if (cstr_equal(name, sim_face_viseme_names[v]))
        {
            out_viseme = v;
            return true;
        }
    }
    return false;
}

const char* sim_face_preset_name (SIM_FacePreset preset)
{
    return (u32)preset < (u32)SIM_FacePreset::Count ? sim_face_preset_names[(u32)preset] : "none";
}

b8 sim_face_preset_from_name (const char* name, SIM_FacePreset& out_preset)
{
    for (u32 p = 0; p < (u32)SIM_FacePreset::Count; p++)
    {
        if (cstr_equal(name, sim_face_preset_names[p]))
        {
            out_preset = (SIM_FacePreset)p;
            return true;
        }
    }
    return false;
}

//
// Spaces and views
//

Pose sim_get_reference_space_pose (const SIM_Sim* sim, SIM_ReferenceSpace space)
{
    ASSERT(sim);
    return reference_pose(sim->devices[(u32)SIM_Device::Head].pose, sim->local_origin, sim->stationary_origin, space);
}

void sim_recenter (SIM_Sim* sim)
{
    ASSERT(sim);
    const Pose head = zm::pose_relative(sim->tracking_drift, sim->devices[(u32)SIM_Device::Head].pose);
    set_local_origin(sim, { .position = head.position, .orientation = yaw_only(head.orientation) });
}

b8 sim_set_tracking_drift (SIM_Sim* sim, const Pose& drift)
{
    ASSERT(sim);
    if (!sim_tracking_drift_valid(drift))
    {
        return false;
    }
    const Quat q        = zm::normalize(drift.orientation);
    sim->tracking_drift = { .position = drift.position, .orientation = zm::normalize(Quat{ 0.0f, q.y, 0.0f, q.w }) };
    return true;
}

Pose sim_get_tracking_drift (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->tracking_drift;
}

b8 sim_set_tracking_drift_rate (SIM_Sim* sim, Vec3 velocity, f32 yaw_degrees_per_second)
{
    ASSERT(sim);
    if (!zm::is_finite(velocity.x) || !zm::is_finite(velocity.y) || !zm::is_finite(velocity.z) || !zm::is_finite(yaw_degrees_per_second))
    {
        return false;
    }
    sim->drift_velocity = velocity;
    sim->drift_yaw_rate = yaw_degrees_per_second;
    return true;
}

void sim_get_tracking_drift_rate (const SIM_Sim* sim, Vec3& out_velocity, f32& out_yaw_degrees_per_second)
{
    ASSERT(sim);
    out_velocity               = sim->drift_velocity;
    out_yaw_degrees_per_second = sim->drift_yaw_rate;
}

void sim_local_origin_change (const SIM_Sim* sim, u64& out_generation, i64& out_change_time)
{
    ASSERT(sim);
    out_generation  = sim->local_origin_generation;
    out_change_time = sim->local_origin_change_time;
}

void sim_sample (const SIM_Sim* sim, i64 time, SIM_Sample& out_sample)
{
    ASSERT(sim);
    live_sample(sim, out_sample);
    if (time >= out_sample.time)
    {
        const f32 dt            = (f32)zm::min(time - out_sample.time, (i64)SIM_MAX_PREDICTION_NS) * 1e-9f;
        out_sample.time         = time;
        out_sample.local_origin = time >= sim->local_origin_change_time ? sim->local_origin : sim->previous_local_origin;
        for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
        {
            extrapolate(out_sample.devices[i], dt);
        }
        return;
    }
    if (sim->history_count == 0)
    {
        return;
    }
    if (time <= history_at(sim, 0).time)
    {
        out_sample = history_at(sim, 0);
        return;
    }
    for (u32 i = sim->history_count; i-- > 0;)
    {
        const SIM_Sample& older = history_at(sim, i);
        if (older.time > time)
        {
            continue;
        }
        if (older.time == time)
        {
            out_sample = older;
            return;
        }
        const SIM_Sample newer = i + 1 < sim->history_count ? history_at(sim, i + 1) : out_sample;
        interpolate(older, newer, time, out_sample);
        return;
    }
}

Pose sim_sample_reference_space_pose (const SIM_Sample& sample, SIM_ReferenceSpace space)
{
    const Pose origin = reference_pose(sample.devices[(u32)SIM_Device::Head].pose, sample.local_origin, sample.stationary_origin, space);
    return space == SIM_ReferenceSpace::View ? origin : zm::pose_mul(sample.tracking_drift, origin);
}

Pose sim_sample_hand_pose (const SIM_Sim* sim, const SIM_Sample& sample, SIM_Device hand, SIM_HandPose kind)
{
    ASSERT(sim);
    if (!is_hand(hand))
    {
        ASSERT_MSG(false, "device %u is not a hand", (u32)hand);
        return zm::POSE_IDENTITY;
    }
    return hand_pose(sim, hand, sample.devices[(u32)hand].pose, kind);
}

void sim_sample_view (const SIM_Sim* sim, const SIM_Sample& sample, u32 view, Pose& out_pose, Fov& out_fov)
{
    ASSERT(sim);
    view_pose(sim, sample.devices[(u32)SIM_Device::Head].pose, view, out_pose, out_fov);
}

Vec2 sim_get_stage_bounds (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->config.stage_bounds;
}

u32 sim_view_count (const SIM_Sim* sim)
{
    UNUSED(sim);
    return SIM_MAX_VIEWS;
}

void sim_get_view (const SIM_Sim* sim, u32 view, Pose& out_pose, Fov& out_fov)
{
    ASSERT(sim);
    view_pose(sim, sim->devices[(u32)SIM_Device::Head].pose, view, out_pose, out_fov);
}

//
// Controls
//

void sim_control_key (SIM_Sim* sim, OS_Key key, b8 down)
{
    ASSERT(sim);
    if ((u32)key >= (u32)OS_Key::Count)
    {
        return;
    }
    sim->keys_down[(u32)key] = down;

    if (!down)
    {
        return;
    }
    const SIM_Config& config = sim->config;
    for (u32 i = 0; i < config.profiles_count; i++)
    {
        if (config.profiles[i].key == key)
        {
            sim_set_profile(sim, i);
            return;
        }
    }
    if ((key == config.key_next_profile || key == config.key_previous_profile) && config.profiles_count > 0)
    {
        const u32 step = key == config.key_next_profile ? 1 : config.profiles_count - 1;
        for (u32 i = 1, at = (sim->profile + step) % config.profiles_count; i < config.profiles_count; i++, at = (at + step) % config.profiles_count)
        {
            if (config.profiles[at].key != OS_Key::None)
            {
                sim_set_profile(sim, at);
                break;
            }
        }
    }
    else if (key == config.key_reset_hands)
    {
        sim->reset_hands_pending = true;
    }
    else if (key == config.key_reset)
    {
        sim->reset_poses_pending = true;
    }
    else if (key == config.key_freeze)
    {
        sim->freeze_pending = true;
    }
    else if (key == config.key_hand_source)
    {
        const u8         modifiers = held_modifiers(sim);
        const SIM_Device hand      = modifiers == SIM_MOD_SHIFT ? SIM_Device::LeftHand : (modifiers == SIM_MOD_CTRL ? SIM_Device::RightHand : active_hand(sim));
        const b8         tracked   = sim->devices[(u32)hand].source == SIM_HandSource::Hand;
        sim_set_hand_source(sim, hand, tracked ? SIM_HandSource::Controller : SIM_HandSource::Hand);
    }
}

void sim_control_mouse_motion (SIM_Sim* sim, f32 dx, f32 dy)
{
    ASSERT(sim);
    if (!zm::is_finite(dx) || !zm::is_finite(dy))
    {
        return;
    }
    sim->mouse_dx += dx;
    sim->mouse_dy += dy;
}

void sim_control_wheel (SIM_Sim* sim, f32 notches)
{
    ASSERT(sim);
    if (zm::is_finite(notches))
    {
        sim->wheel += notches;
    }
}

void sim_set_controls_enabled (SIM_Sim* sim, b8 enabled)
{
    ASSERT(sim);
    sim->controls_enabled = enabled;
}

b8 sim_controls_enabled (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->controls_enabled;
}

u32 sim_get_profile (const SIM_Sim* sim)
{
    ASSERT(sim);
    return sim->profile;
}

// Inputs held by keys move to the new profile's hand on the next frame
void sim_set_profile (SIM_Sim* sim, u32 index)
{
    ASSERT(sim);
    if (index >= sim->config.profiles_count)
    {
        ASSERT_MSG(false, "invalid profile %u", index);
        return;
    }
    sim->profile = index;
}

b8 sim_profile_from_name (const SIM_Sim* sim, const char* name, u32& out_index)
{
    ASSERT(sim);
    out_index = name ? find_profile(sim->config, name) : sim->config.profiles_count;
    return out_index < sim->config.profiles_count;
}

SIM_Device sim_profile_device (const SIM_Profile& profile)
{
    switch (profile.mouse)
    {
    case SIM_Drive::TurnLeft:
        return SIM_Device::LeftHand;
    case SIM_Drive::TurnRight:
        return SIM_Device::RightHand;
    default:
        return SIM_Device::Head;
    }
}

const char* sim_drive_name (SIM_Drive drive)
{
    return (u32)drive < (u32)SIM_Drive::Count ? sim_drive_names[(u32)drive] : "";
}

b8 sim_drive_from_name (const char* name, SIM_Drive& out_drive)
{
    for (u32 i = 0; i < (u32)SIM_Drive::Count; i++)
    {
        if (cstr_equal(name, sim_drive_names[i]))
        {
            out_drive = (SIM_Drive)i;
            return true;
        }
    }
    return false;
}

// Resets are teleports: the previous pose moves along so no velocity spike reaches the app
void sim_reset_hands (SIM_Sim* sim)
{
    ASSERT(sim);
    const Pose head = level_frame(sim->devices[(u32)SIM_Device::Head].pose);
    for (u32 h = 0; h < 2; h++)
    {
        SIM_DeviceData& hand = sim->devices[(u32)hand_device(h)];
        hand.pose          = zm::pose_mul(head, sim->hand_offsets[h]);
        hand.previous_pose = hand.pose;
    }
}

void sim_reset_poses (SIM_Sim* sim)
{
    ASSERT(sim);
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        sim->devices[i].pose          = sim->start_poses[i];
        sim->devices[i].previous_pose = sim->start_poses[i];
    }
}

u64 sim_state_hash (const SIM_Sim* sim)
{
    ASSERT(sim);
    u64 h = hash((const u8*)&sim->time, sizeof(sim->time));
    h ^= hash((const u8*)&sim->frame_index, sizeof(sim->frame_index));
    h ^= hash((const u8*)&sim->local_origin, sizeof(sim->local_origin));
    if (!zm::equal(sim->tracking_drift, zm::POSE_IDENTITY))
    {
        h = (h ^ hash((const u8*)&sim->tracking_drift, sizeof(sim->tracking_drift))) * 0x100000001B3ull;
    }
    h  = (h ^ (u64)sim->controller) * 0x100000001B3ull;
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        const SIM_DeviceData& device = sim->devices[i];
        h = (h ^ hash((const u8*)&device.pose, sizeof(device.pose))) * 0x100000001B3ull;
        h = (h ^ hash((const u8*)&device.linear_velocity, sizeof(device.linear_velocity))) * 0x100000001B3ull;
        h = (h ^ hash((const u8*)&device.angular_velocity, sizeof(device.angular_velocity))) * 0x100000001B3ull;
        h = (h ^ (u64)device.active) * 0x100000001B3ull;
        h = (h ^ (u64)device.source) * 0x100000001B3ull;
        if (device.source == SIM_HandSource::Hand)
        {
            h = (h ^ hash((const u8*)&device.detached_pose, sizeof(device.detached_pose))) * 0x100000001B3ull;
        }
        h = (h ^ hash((const u8*)device.inputs, sizeof(device.inputs))) * 0x100000001B3ull;
    }
    if (sim->typed_length > 0)
    {
        h = (h ^ hash((const u8*)sim->typed, sim->typed_length)) * 0x100000001B3ull;
    }
    if (sim->props_count > 0)
    {
        h = (h ^ hash_props(sim->props, sim->props_count)) * 0x100000001B3ull;
    }
    SIM_BodyState unedited = {};
    sim_default_body(SIM_BODY_REFERENCE_HEIGHT, unedited);
    if (!memory_equal(&sim->body, &unedited, sizeof(unedited)))
    {
        h = (h ^ hash((const u8*)&sim->body, sizeof(sim->body))) * 0x100000001B3ull;
        h = (h ^ (u64)sim->body_generation) * 0x100000001B3ull;
    }
    SIM_FaceState neutral = {};
    sim_default_face(neutral);
    if (!memory_equal(&sim->face, &neutral, sizeof(neutral)))
    {
        h = (h ^ hash((const u8*)&sim->face, sizeof(sim->face))) * 0x100000001B3ull;
    }
    return h;
}
