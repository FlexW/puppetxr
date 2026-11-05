#define PUPPETXR_BUILDING_LIBRARY
#include "puppetxr.h"

#include "oxr.h"

#include <stddef.h>

//
// Types
//

#define API_MAX_CLIENTS 8
#define API_MAX_LINE    16384 // a body loaded from a file fits
#define API_MAX_BODY_JSON 12288
// How long a frame reply waits for the frames in flight to end, for an app that waits for frames
// it never ends
#define API_END_FRAME_WAIT_NS 1000000000ull
#define API_MAX_REPLY   16384
#define API_MAX_LISTED_ANCHORS 64

namespace
{

struct API_Client
{
    OS_Socket*  socket;
    char        input[API_MAX_LINE];
    usize       input_length;
    Array<char> output;       // what the socket did not take yet
    b8          greeted;      // sent a matching hello
    b8          closing;      // close once the output is flushed
    b8          waiting;      // a wait_frames reply is due at wait_frame, once its frames ended
    u64         wait_frame;
    u64         reached_ns;   // when wait_frame was reached, 0 before
    i64         wait_id;
};

struct API_Server
{
    b8           running;
    OS_Socket*   listener;
    OS_LockFile* discovery;
    char         app[128];
    API_Client   clients[API_MAX_CLIENTS];
    b8           paused;    // xrWaitFrame waits for steps or a resume
    u64          steps;     // frames a paused simulator may still run
    b8           connected; // some client greeted since the server started
};

struct API_Reply
{
    char  text[API_MAX_REPLY];
    usize length;
};

} // namespace

//
// Statics
//

static b8 device_from_api (PUPPETXR_Device device, SIM_Device& out_device)
{
    switch (device)
    {
    case PUPPETXR_DEVICE_HEAD:
        out_device = SIM_Device::Head;
        return true;
    case PUPPETXR_DEVICE_LEFT_HAND:
        out_device = SIM_Device::LeftHand;
        return true;
    case PUPPETXR_DEVICE_RIGHT_HAND:
        out_device = SIM_Device::RightHand;
        return true;
    case PUPPETXR_DEVICE_MAX_ENUM:
        break;
    }
    return false;
}

static b8 replaying ()
{
    return oxr_record_status().replaying;
}

// PUPPETXR_ControlMode by value: the profiles that were the control modes
static const char* const api_control_mode_profiles[] = { "body", "head", "left", "right" };

static i32 control_mode_of (const SIM_Sim* sim)
{
    const char* name = sim_get_config(sim).profiles[sim_get_profile(sim)].name;
    for (u32 i = 0; i < ARRAY_SIZE(api_control_mode_profiles); i++)
    {
        if (cstr_equal(name, api_control_mode_profiles[i]))
        {
            return (i32)i;
        }
    }
    return -1;
}

// The status layout of API version 1; later versions only append
#define API_STATUS_V1_SIZE (offsetof(PUPPETXR_Status, profile) + sizeof(PUPPETXR_Status::profile))

static PUPPETXR_Result api_get_status (PUPPETXR_Status* out_status)
{
    if (!out_status || out_status->struct_size < API_STATUS_V1_SIZE)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    const OXR_RecordStatus record = oxr_record_status();
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    const PUPPETXR_Status status = {
        .struct_size      = out_status->struct_size,
        .frame_index      = sim_frame_index(sim),
        .time_ns          = sim_time(sim),
        .state_hash       = sim_state_hash(sim),
        .recording        = record.recording ? 1 : 0,
        .recorded_frames  = record.recorded_frames,
        .replaying        = record.replaying ? 1 : 0,
        .replay_finished  = record.replay_finished ? 1 : 0,
        .replay_frame     = record.replay_frame,
        .replay_total     = record.replay_total,
        .divergences      = record.divergences,
        .first_divergence = record.first_divergence,
        .controller       = (int32_t)sim_get_controller(sim),
        .control_mode     = (int32_t)control_mode_of(sim),
        .profile          = {},
    };
    PUPPETXR_Status named = status;
    cstr_copy(named.profile, sizeof(named.profile), sim_get_config(sim).profiles[sim_get_profile(sim)].name);
    memory_copy(out_status, &named, zm::min((usize)out_status->struct_size, sizeof(named)));
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_get_device_pose (PUPPETXR_Device device, PUPPETXR_Pose* out_pose)
{
    SIM_Device sim_device = {};
    if (!device_from_api(device, sim_device) || !out_pose)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    const Pose pose = sim_get_device_pose(sim, sim_device);
    *out_pose       = { { pose.position.x, pose.position.y, pose.position.z }, { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w } };
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_device_pose (PUPPETXR_Device device, const PUPPETXR_Pose* pose)
{
    SIM_Device sim_device = {};
    if (!device_from_api(device, sim_device) || !pose)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    const Pose value  = { { pose->position[0], pose->position[1], pose->position[2] }, { pose->orientation[0], pose->orientation[1], pose->orientation[2], pose->orientation[3] } };
    const f32  length = zm::length(value.orientation);
    if (!zm::is_finite(value) || length < 0.5f || length > 2.0f)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_set_device_pose(sim, sim_device, value);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_device_active (PUPPETXR_Device device, int32_t active)
{
    SIM_Device sim_device = {};
    if (!device_from_api(device, sim_device))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_set_device_active(sim, sim_device, active != 0);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_get_input (PUPPETXR_Device device, const char* name, f32* out_x, f32* out_y)
{
    SIM_Device sim_device = {};
    SIM_Input  input = {};
    if (!device_from_api(device, sim_device) || sim_device == SIM_Device::Head || !sim_input_from_name(name, input))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    const SIM_InputValue value = sim_get_input(sim, sim_device, input);
    if (out_x)
    {
        *out_x = value.value.x;
    }
    if (out_y)
    {
        *out_y = value.value.y;
    }
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_input (PUPPETXR_Device device, const char* name, f32 x, f32 y)
{
    SIM_Device sim_device = {};
    SIM_Input  input = {};
    if (!device_from_api(device, sim_device) || sim_device == SIM_Device::Head || !sim_input_from_name(name, input) || !zm::is_finite(x) || !zm::is_finite(y))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_set_input(sim, sim_device, input, { .value = { x, y } });
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_fixed_step (int32_t enabled)
{
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_set_clock_mode(sim, enabled ? SIM_ClockMode::FixedStep : SIM_ClockMode::Realtime);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_controls_enabled (int32_t enabled)
{
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_set_controls_enabled(sim, enabled != 0);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_recenter ()
{
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_recenter(sim);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_mirror_view (PUPPETXR_MirrorView view)
{
    if (view != PUPPETXR_MIRROR_LEFT && view != PUPPETXR_MIRROR_RIGHT && view != PUPPETXR_MIRROR_BOTH)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    oxr_set_mirror_view((SIM_MirrorView)view);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_screenshot (const char* path)
{
    if (!path || !path[0])
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    u32 width = 0;
    u32 height = 0;
    u8  probe[4];
    oxr_read_frame(probe, 0, width, height);
    if (width == 0)
    {
        return PUPPETXR_ERROR_NO_GRAPHICS;
    }
    if (!oxr_frame_ready())
    {
        return PUPPETXR_ERROR_NOT_AVAILABLE;
    }
    return oxr_save_screenshot(path) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_IO;
}

static PUPPETXR_Result api_record_start (const char* path)
{
    if (!path || !path[0])
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    if (oxr_record_status().recording)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return oxr_record_start(path) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_IO;
}

static PUPPETXR_Result api_record_stop ()
{
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    if (!oxr_record_status().recording)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return oxr_record_stop() ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_IO;
}

static PUPPETXR_Result api_replay_start (const char* path)
{
    if (!path || !path[0])
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    return oxr_replay_start(path) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_IO;
}

static PUPPETXR_Result api_replay_stop ()
{
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    oxr_replay_stop();
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_request_exit ()
{
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    oxr_request_exit();
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_get_controller (PUPPETXR_Controller* out_controller)
{
    if (!out_controller)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    *out_controller = (PUPPETXR_Controller)sim_get_controller(sim);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_controller (PUPPETXR_Controller controller)
{
    if (controller != PUPPETXR_CONTROLLER_OCULUS_TOUCH && controller != PUPPETXR_CONTROLLER_KHR_SIMPLE && controller != PUPPETXR_CONTROLLER_META_TOUCH_PRO && controller != PUPPETXR_CONTROLLER_META_TOUCH_PLUS)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    return oxr_set_controller((SIM_Controller)controller) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_BUSY;
}

static PUPPETXR_Result api_get_hand_source (PUPPETXR_Device hand, PUPPETXR_HandSource* out_source)
{
    SIM_Device sim_device = {};
    if (!device_from_api(hand, sim_device) || sim_device == SIM_Device::Head || !out_source)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    *out_source = (PUPPETXR_HandSource)sim_get_hand_source(sim, sim_device);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_hand_source (PUPPETXR_Device hand, PUPPETXR_HandSource source)
{
    SIM_Device sim_device = {};
    if (!device_from_api(hand, sim_device) || sim_device == SIM_Device::Head || (source != PUPPETXR_HAND_SOURCE_CONTROLLER && source != PUPPETXR_HAND_SOURCE_HAND))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_set_hand_source(sim, sim_device, (SIM_HandSource)source);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_room (const char* name)
{
    switch (oxr_set_room(name))
    {
    case OXR_RoomResult::Ok:
        return PUPPETXR_SUCCESS;
    case OXR_RoomResult::NoInstance:
        return PUPPETXR_ERROR_NO_INSTANCE;
    case OXR_RoomResult::NotFound:
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    case OXR_RoomResult::Busy:
        return PUPPETXR_ERROR_BUSY;
    }
    return PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result api_get_room (char* out_name, uint32_t size)
{
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    if (!out_name || size == 0)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return oxr_get_room(out_name, size) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result api_get_control_mode (PUPPETXR_ControlMode* out_mode)
{
    if (!out_mode)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    const i32 mode = control_mode_of(sim);
    if (mode < 0)
    {
        return PUPPETXR_ERROR_NOT_AVAILABLE;
    }
    *out_mode = (PUPPETXR_ControlMode)mode;
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_profiler_start (const char* path)
{
    if (!path || !path[0])
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (os_profile_active())
    {
        return PUPPETXR_ERROR_BUSY;
    }
    return os_profile_start(path) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_IO;
}

static PUPPETXR_Result api_profiler_stop ()
{
    if (!os_profile_active())
    {
        return PUPPETXR_ERROR_NOT_AVAILABLE;
    }
    os_profile_stop();
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_profile (const char* name)
{
    if (!name)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    u32 index = 0;
    if (!sim_profile_from_name(sim, name, index))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    sim_set_profile(sim, index);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_control_mode (PUPPETXR_ControlMode mode)
{
    if ((u32)mode >= ARRAY_SIZE(api_control_mode_profiles))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_set_profile(api_control_mode_profiles[mode]);
}

static PUPPETXR_Result api_get_profile (char* out_name, uint32_t size)
{
    if (!out_name || size == 0)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    const char* name = sim_get_config(sim).profiles[sim_get_profile(sim)].name;
    if (cstr_length(name) >= size)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    cstr_copy(out_name, size, name);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_reset (b8 hands_only)
{
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    if (hands_only)
    {
        sim_reset_hands(sim);
    }
    else
    {
        sim_reset_poses(sim);
    }
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_reset_poses ()
{
    return api_reset(false);
}

static PUPPETXR_Result api_reset_hands ()
{
    return api_reset(true);
}

static PUPPETXR_Result api_point_hand (PUPPETXR_Device hand, uint32_t view, float u, float v, const PUPPETXR_Pose* ray)
{
    SIM_Device sim_hand = {};
    if (!device_from_api(hand, sim_hand) || sim_hand == SIM_Device::Head || view > 1 || !(u >= 0.0f && u <= 1.0f) || !(v >= 0.0f && v <= 1.0f))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    Pose grip_from_ray = {};
    if (ray)
    {
        grip_from_ray = { { ray->position[0], ray->position[1], ray->position[2] }, { ray->orientation[0], ray->orientation[1], ray->orientation[2], ray->orientation[3] } };
        const f32 length = zm::length(grip_from_ray.orientation);
        if (!zm::is_finite(grip_from_ray) || length < 0.5f || length > 2.0f)
        {
            return PUPPETXR_ERROR_INVALID_ARGUMENT;
        }
        grip_from_ray.orientation = zm::normalize(grip_from_ray.orientation);
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_point_hand(sim, sim_hand, view, u, v, ray ? grip_from_ray : sim_get_config(sim).aim_offset);
    return PUPPETXR_SUCCESS;
}

// The same sim entry points as the mirror window's events
static PUPPETXR_Result api_control_key (const char* name, int32_t down)
{
    OS_Key key = OS_Key::None;
    if (!name || !os_key_from_name(name, key) || key == OS_Key::None)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_control_key(sim, key, down != 0);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_control_mouse (f32 dx, f32 dy)
{
    if (!zm::is_finite(dx) || !zm::is_finite(dy))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_control_mouse_motion(sim, dx, dy);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result control_result (OXR_ControlResult result)
{
    switch (result)
    {
    case OXR_ControlResult::Ok:
        return PUPPETXR_SUCCESS;
    case OXR_ControlResult::NoInstance:
        return PUPPETXR_ERROR_NO_INSTANCE;
    case OXR_ControlResult::Busy:
        return PUPPETXR_ERROR_BUSY;
    case OXR_ControlResult::Invalid:
        break;
    }
    return PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result api_set_disabled_extensions (const char* names)
{
    oxr_set_disabled_extensions(names);
    return PUPPETXR_SUCCESS;
}

static void pose_to_api (const Pose& pose, PUPPETXR_Pose& out)
{
    out = { { pose.position.x, pose.position.y, pose.position.z }, { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w } };
}

static PUPPETXR_Result api_get_body_joints (PUPPETXR_BodyJoint* out_joints, uint32_t capacity, uint32_t* out_count)
{
    if (!out_count || (capacity > 0 && !out_joints))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    SIM_Sample sample = {};
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    SIM_BodyJoints joints = {};
    sim_body_joints(sim, sample, joints);
    const SIM_BodyState& body = sim_get_body(sim);
    *out_count                = SIM_BODY_JOINT_COUNT;
    for (u32 j = 0; j < capacity && j < SIM_BODY_JOINT_COUNT; j++)
    {
        out_joints[j] = {};
        pose_to_api(joints.poses[j], out_joints[j].pose);
        out_joints[j].tracked    = joints.tracked[j] ? 1 : 0;
        out_joints[j].confidence = body.confidences[j];
    }
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_get_body (char* out_json, uint32_t size, uint32_t* out_length)
{
    if (!out_length || (size > 0 && !out_json))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    char text[API_MAX_BODY_JSON];
    const b8 fits = sim_body_to_json(sim_get_body(sim), text, sizeof(text));
    ASSERT_MSG(fits, "the body's JSON outgrew API_MAX_BODY_JSON");
    const usize length = cstr_length(text);
    *out_length        = (u32)length + 1;
    if (!fits || length + 1 > size)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    memory_copy(out_json, text, length + 1);
    *out_length = (u32)length;
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result set_body_json (const JsonValue& root, b8 replace)
{
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    SIM_BodyState body = replace ? sim_get_config(sim).body : sim_get_body(sim);
    if (!sim_body_apply_json(root, body))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    sim_set_body(sim, body);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_body (const char* json)
{
    if (!json)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    JsonParser* parser = json_parse_from_string(json);
    if (!parser)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    defer { json_release(parser); };
    return set_body_json(json_get_root(parser), false);
}

static PUPPETXR_Result api_reset_body ()
{
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_reset_body(sim);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_get_face_weights (float* out_weights, uint32_t capacity, uint32_t* out_count)
{
    if (!out_count || (capacity > 0 && !out_weights))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    SIM_Sample sample = {};
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    SIM_FaceFrame frame = {};
    sim_face_frame(sim, sample, frame);
    *out_count = SIM_FACE_EXPRESSION_COUNT;
    memory_copy(out_weights, frame.weights, sizeof(f32) * zm::min(capacity, SIM_FACE_EXPRESSION_COUNT));
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_get_eye_gazes (PUPPETXR_Pose* out_eyes)
{
    if (!out_eyes)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    SIM_Sample sample = {};
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    SIM_FaceFrame frame = {};
    sim_face_frame(sim, sample, frame);
    pose_to_api(frame.eyes[0], out_eyes[0]);
    pose_to_api(frame.eyes[1], out_eyes[1]);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_get_face (char* out_json, uint32_t size, uint32_t* out_length)
{
    if (!out_length || (size > 0 && !out_json))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    char     text[API_MAX_BODY_JSON];
    const b8 fits = sim_face_to_json(sim_get_face(sim), text, sizeof(text));
    ASSERT_MSG(fits, "the face's JSON outgrew API_MAX_BODY_JSON");
    const usize length = cstr_length(text);
    *out_length        = (u32)length + 1;
    if (!fits || length + 1 > size)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    memory_copy(out_json, text, length + 1);
    *out_length = (u32)length;
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result set_face_json (const JsonValue& root, b8 replace)
{
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    SIM_FaceState face = replace ? sim_get_config(sim).face : sim_get_face(sim);
    if (!sim_face_apply_json(root, face))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    sim_set_face(sim, face);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_face (const char* json)
{
    if (!json)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    JsonParser* parser = json_parse_from_string(json);
    if (!parser)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    defer { json_release(parser); };
    return set_face_json(json_get_root(parser), false);
}

static PUPPETXR_Result api_reset_face ()
{
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    sim_reset_face(sim);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_get_anchors (PUPPETXR_Anchor* out_anchors, uint32_t capacity, uint32_t* out_count)
{
    if (!out_count || (capacity > 0 && !out_anchors))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    Array<OXR_AnchorEntry> entries = {};
    defer { entries.release(); };
    entries.resize(oxr_list_anchors(nullptr, 0));
    const u32 count = oxr_list_anchors(entries.data, (u32)entries.length);
    *out_count      = count;
    for (u32 i = 0; i < count && i < capacity && i < entries.length; i++)
    {
        const OXR_AnchorEntry& entry = entries[i];
        PUPPETXR_Anchor&       out   = out_anchors[i];
        out                          = {};
        char uuid[W_UUID_SIZE * 2 + 1];
        w_uuid_to_string(entry.anchor.uuid, uuid);
        cstr_copy(out.uuid, sizeof(out.uuid), uuid);
        cstr_copy(out.room, sizeof(out.room), entry.anchor.room);
        pose_to_api(entry.anchor.pose, out.pose);
        out.cloud = entry.cloud ? 1 : 0;
        out.owner = entry.owner;
    }
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_get_virtual_keyboard (PUPPETXR_VirtualKeyboard* out_keyboard)
{
    if (!out_keyboard || out_keyboard->struct_size < sizeof(PUPPETXR_VirtualKeyboard))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    OXR_KeyboardState state = {};
    if (!oxr_get_keyboard_state(state))
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    PUPPETXR_VirtualKeyboard keyboard = {
        .struct_size = out_keyboard->struct_size,
        .exists      = state.exists ? 1 : 0,
        .visible     = state.visible ? 1 : 0,
        .shifted     = state.shifted ? 1 : 0,
        .scale       = state.scale,
    };
    pose_to_api(state.pose, keyboard.pose);
    cstr_copy(keyboard.context, sizeof(keyboard.context), state.context);
    memory_copy(out_keyboard, &keyboard, sizeof(keyboard));
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_get_virtual_keyboard_key (const char* name, PUPPETXR_Pose* out_pose)
{
    if (!name || !out_pose)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    OXR_KeyboardState state = {};
    if (!oxr_get_keyboard_state(state))
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    if (!state.exists)
    {
        return PUPPETXR_ERROR_NOT_AVAILABLE;
    }
    Pose pose = {};
    if (!oxr_get_keyboard_key(name, pose))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    pose_to_api(pose, *out_pose);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_type_text (const char* text)
{
    if (!text)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    OXR_KeyboardState state = {};
    if (!oxr_get_keyboard_state(state))
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    if (!state.visible)
    {
        return PUPPETXR_ERROR_NOT_AVAILABLE;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    return sim_type_text(sim, text) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_INVALID_ARGUMENT;
}

// The layout of PUPPETXR_Prop in API version 1; later versions only append
#define API_PROP_V1_SIZE (offsetof(PUPPETXR_Prop, current) + sizeof(PUPPETXR_Pose))

static b8 prop_from_api (const PUPPETXR_Prop* prop, SIM_Prop& out)
{
    if (!prop || prop->struct_size < API_PROP_V1_SIZE || (u32)prop->kind >= (u32)SIM_PropKind::Count || (u32)prop->motion >= (u32)SIM_PropMotion::Count)
    {
        return false;
    }
    out = {
        .kind   = (SIM_PropKind)prop->kind,
        .motion = (SIM_PropMotion)prop->motion,
        .pose   = { { prop->pose.position[0], prop->pose.position[1], prop->pose.position[2] }, { prop->pose.orientation[0], prop->pose.orientation[1], prop->pose.orientation[2], prop->pose.orientation[3] } },
        .target = { prop->target[0], prop->target[1], prop->target[2] },
        .radius = prop->radius,
        .speed  = prop->speed,
        .size   = prop->size,
    };
    memory_copy(out.text, prop->text, sizeof(out.text));
    return true;
}

static void prop_to_api (const SIM_Prop& prop, i64 time, PUPPETXR_Prop& out)
{
    out = {
        .struct_size = sizeof(PUPPETXR_Prop),
        .id          = prop.id,
        .kind        = (PUPPETXR_PropKind)prop.kind,
        .motion      = (PUPPETXR_PropMotion)prop.motion,
        .target      = { prop.target.x, prop.target.y, prop.target.z },
        .radius      = prop.radius,
        .speed       = prop.speed,
        .size        = prop.size,
    };
    pose_to_api(prop.pose, out.pose);
    pose_to_api(sim_prop_pose(prop, time), out.current);
    cstr_copy(out.text, sizeof(out.text), prop.text);
}

static PUPPETXR_Result api_add_prop (const PUPPETXR_Prop* prop, uint32_t* out_id)
{
    SIM_Prop sim_prop = {};
    if (!out_id || !prop_from_api(prop, sim_prop))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    *out_id = sim_add_prop(sim, sim_prop);
    return *out_id != 0 ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result api_update_prop (uint32_t id, const PUPPETXR_Prop* prop)
{
    SIM_Prop sim_prop = {};
    if (!prop_from_api(prop, sim_prop))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    return sim_update_prop(sim, id, sim_prop) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result api_remove_prop (uint32_t id)
{
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    return sim_remove_prop(sim, id) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result api_get_props (PUPPETXR_Prop* out_props, uint32_t capacity, uint32_t* out_count)
{
    if (!out_count || (capacity > 0 && (!out_props || out_props->struct_size < API_PROP_V1_SIZE)))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    const usize stride = capacity > 0 ? out_props->struct_size : 0;
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    const SIM_Prop* props = nullptr;
    const u32       count = sim_props(sim, props);
    *out_count            = count;
    for (u32 i = 0; i < count && i < capacity; i++)
    {
        PUPPETXR_Prop prop = {};
        prop_to_api(props[i], sim_predicted_display_time(sim), prop);
        PUPPETXR_Prop* out = (PUPPETXR_Prop*)((u8*)out_props + stride * i);
        memory_copy(out, &prop, zm::min(stride, sizeof(prop)));
        out->struct_size = (u32)stride;
    }
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_wipe_anchors (int32_t include_cloud)
{
    return control_result(oxr_wipe_anchors(include_cloud != 0));
}

static PUPPETXR_Result api_set_anchor_fault (PUPPETXR_AnchorOp op, int32_t xr_result, uint32_t count)
{
    if ((u32)op >= (u32)OXR_AnchorOp::Count)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return control_result(oxr_set_anchor_fault((OXR_AnchorOp)op, xr_result, count));
}

static PUPPETXR_Result api_set_frame_fault (PUPPETXR_FrameOp op, int32_t xr_result, uint32_t count)
{
    if ((u32)op >= (u32)OXR_FrameOp::Count)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return control_result(oxr_set_frame_fault((OXR_FrameOp)op, xr_result, count));
}

static PUPPETXR_Result api_set_anchor_timing (uint32_t latency_frames, uint32_t localize_frames)
{
    return control_result(oxr_set_anchor_timing(latency_frames, localize_frames));
}

static PUPPETXR_Result api_get_origin_offset (PUPPETXR_Pose* out_room_from_stage)
{
    if (!out_room_from_stage)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    Pose pose = {};
    if (!oxr_get_origin_offset(pose))
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    pose_to_api(pose, *out_room_from_stage);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result api_set_tracking_drift (const PUPPETXR_Pose* drift)
{
    Pose value = zm::POSE_IDENTITY;
    if (drift)
    {
        value = { { drift->position[0], drift->position[1], drift->position[2] }, { drift->orientation[0], drift->orientation[1], drift->orientation[2], drift->orientation[3] } };
    }
    if (!sim_tracking_drift_valid(value))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    return sim_set_tracking_drift(sim, value) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result api_set_tracking_drift_rate (const float* velocity, float yaw_degrees_per_second)
{
    const Vec3 value = velocity ? Vec3{ velocity[0], velocity[1], velocity[2] } : Vec3{};
    if (!zm::is_finite(value.x) || !zm::is_finite(value.y) || !zm::is_finite(value.z) || !zm::is_finite(yaw_degrees_per_second))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (replaying())
    {
        return PUPPETXR_ERROR_REPLAYING;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    return sim_set_tracking_drift_rate(sim, value, yaw_degrees_per_second) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result api_get_tracking_drift (PUPPETXR_Pose* out_drift, float* out_velocity, float* out_yaw_degrees_per_second)
{
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    Vec3 velocity = {};
    f32  yaw_rate = 0.0f;
    sim_get_tracking_drift_rate(sim, velocity, yaw_rate);
    if (out_drift)
    {
        pose_to_api(sim_get_tracking_drift(sim), *out_drift);
    }
    if (out_velocity)
    {
        out_velocity[0] = velocity.x;
        out_velocity[1] = velocity.y;
        out_velocity[2] = velocity.z;
    }
    if (out_yaw_degrees_per_second)
    {
        *out_yaw_degrees_per_second = yaw_rate;
    }
    return PUPPETXR_SUCCESS;
}

// Log callback: the pair is swapped under its own lock, so a callback can change while other
// threads log
static OS_Mutex*         g_log_mutex;
static PUPPETXR_LogCallback g_log_callback;
static void*             g_log_user;

static PUPPETXR_LogLevel log_level_to_api (OS_LogLevel level)
{
    switch (level)
    {
    case OS_LogLevel::Debug:
        return PUPPETXR_LOG_DEBUG;
    case OS_LogLevel::Info:
        return PUPPETXR_LOG_INFO;
    case OS_LogLevel::Warning:
        return PUPPETXR_LOG_WARNING;
    case OS_LogLevel::Error:
        return PUPPETXR_LOG_ERROR;
    }
    return PUPPETXR_LOG_ERROR;
}

static void forward_log (OS_LogLevel level, const char* text, void*)
{
    os_mutex_lock(g_log_mutex);
    const PUPPETXR_LogCallback callback = g_log_callback;
    void* const             user     = g_log_user;
    os_mutex_unlock(g_log_mutex);
    if (callback)
    {
        callback(log_level_to_api(level), text, user);
    }
}

static PUPPETXR_Result api_set_log_callback (PUPPETXR_LogCallback callback, void* user, int32_t replay_history)
{
    if (!g_log_mutex)
    {
        g_log_mutex = os_mutex_create();
    }
    os_mutex_lock(g_log_mutex);
    g_log_callback = callback;
    g_log_user     = user;
    os_mutex_unlock(g_log_mutex);
    os_set_log_callback(callback ? forward_log : nullptr, nullptr);

    if (callback && replay_history)
    {
        for (u32 i = 0; i < os_log_history_count(); i++)
        {
            const OS_LogLine* line = os_log_history(i);
            if (line && line->source == OS_LogSource::Sim)
            {
                callback(log_level_to_api(line->level), line->text, user);
            }
        }
    }
    return PUPPETXR_SUCCESS;
}

// Runs when the loader unloads the runtime, like release_runtime_on_unload
namespace
{

struct ReleaseLogCallbackOnUnload
{
    ~ReleaseLogCallbackOnUnload ()
    {
        os_set_log_callback(nullptr, nullptr);
        if (g_log_mutex)
        {
            os_mutex_destroy(g_log_mutex);
            g_log_mutex = nullptr;
        }
    }
};

} // namespace

static const ReleaseLogCallbackOnUnload release_log_callback_on_unload;

//
// Control server: one JSON object per line each way, see docs/control-cli.md. Requests are
// handled at the start of xrWaitFrame and xrPollEvent, so they land between frames and the
// recording sees them like any other state change.
//

static API_Server g_server;

static void version_string (char* out, usize size)
{
    cstr_format(out, size, "%u.%u.%u", (u32)PUPPETXR_BUILD_VERSION_MAJOR, (u32)PUPPETXR_BUILD_VERSION_MINOR, (u32)PUPPETXR_BUILD_VERSION_PATCH);
}

static void reply_append (API_Reply& reply, const char* fmt, ...) PRINTF_LIKE(2, 3);
static void reply_append (API_Reply& reply, const char* fmt, ...)
{
    va_list args = {};
    va_start(args, fmt);
    const usize free = sizeof(reply.text) - reply.length;
    const usize used = cstr_format_v(reply.text + reply.length, free, fmt, args);
    va_end(args);
    reply.length += zm::min(used, free > 0 ? free - 1 : 0);
}

static void reply_key (API_Reply& reply, const char* key)
{
    reply_append(reply, ",\"%s\":", key);
}

static void reply_string (API_Reply& reply, const char* key, const char* value)
{
    reply_key(reply, key);
    reply_append(reply, "\"");
    for (const char* c = value ? value : ""; *c; c++)
    {
        if (*c == '"' || *c == '\\')
        {
            reply_append(reply, "\\%c", *c);
        }
        else if ((u8)*c < 0x20)
        {
            reply_append(reply, "\\u%04x", (u32)(u8)*c);
        }
        else
        {
            reply_append(reply, "%c", *c);
        }
    }
    reply_append(reply, "\"");
}

static void reply_u64 (API_Reply& reply, const char* key, u64 value)
{
    reply_key(reply, key);
    reply_append(reply, "%llu", (unsigned long long)value);
}

static void reply_bool (API_Reply& reply, const char* key, b8 value)
{
    reply_key(reply, key);
    reply_append(reply, value ? "true" : "false");
}

static void reply_number (API_Reply& reply, f32 value)
{
    char number[32];
    json_format_f32(number, sizeof(number), value);
    reply_append(reply, "%s", number);
}

static void reply_f32 (API_Reply& reply, const char* key, f32 value)
{
    reply_key(reply, key);
    reply_number(reply, value);
}

static void reply_floats (API_Reply& reply, const char* key, const f32* values, u32 count)
{
    reply_key(reply, key);
    reply_append(reply, "[");
    for (u32 i = 0; i < count; i++)
    {
        reply_append(reply, i ? "," : "");
        reply_number(reply, values[i]);
    }
    reply_append(reply, "]");
}

static const char* result_name (PUPPETXR_Result result)
{
    switch (result)
    {
    case PUPPETXR_SUCCESS:
        return "success";
    case PUPPETXR_ERROR_NOT_LOADED:
        return "not_loaded";
    case PUPPETXR_ERROR_VERSION:
        return "version";
    case PUPPETXR_ERROR_NO_INSTANCE:
        return "no_instance";
    case PUPPETXR_ERROR_INVALID_ARGUMENT:
        return "invalid_argument";
    case PUPPETXR_ERROR_REPLAYING:
        return "replaying";
    case PUPPETXR_ERROR_IO:
        return "io";
    case PUPPETXR_ERROR_NO_GRAPHICS:
        return "no_graphics";
    case PUPPETXR_ERROR_BUSY:
        return "busy";
    case PUPPETXR_ERROR_NOT_AVAILABLE:
        return "not_available";
    case PUPPETXR_RESULT_MAX_ENUM:
        break;
    }
    return "unknown";
}

static void client_send (API_Client& client, const char* data, usize size)
{
    if (client.output.length == 0)
    {
        usize sent = 0;
        if (os_socket_send(client.socket, data, size, sent) == OS_SocketResult::Closed)
        {
            client.closing = true;
            return;
        }
        data += sent;
        size -= sent;
    }
    client.output.add(data, size);
}

static void reply_begin (API_Reply& reply, b8 has_id, i64 id, b8 ok)
{
    reply.length = 0;
    if (has_id)
    {
        reply_append(reply, "{\"id\":%lld,\"ok\":%s", (long long)id, ok ? "true" : "false");
    }
    else
    {
        reply_append(reply, "{\"id\":null,\"ok\":%s", ok ? "true" : "false");
    }
}

static void reply_finish (API_Client& client, API_Reply& reply)
{
    reply_append(reply, "}\n");
    client_send(client, reply.text, reply.length);
}

static void reply_error (API_Client& client, b8 has_id, i64 id, const char* code, const char* message)
{
    API_Reply reply = {};
    reply_begin(reply, has_id, id, false);
    reply_string(reply, "error", code);
    reply_string(reply, "message", message);
    reply_finish(client, reply);
}

static b8 request_device (const JsonValue& request, PUPPETXR_Device& out_device)
{
    char       name[32] = {};
    SIM_Device device   = SIM_Device::Head;
    if (!json_find_string(request, "device", name, nullptr, sizeof(name)) || !sim_device_from_name(name, device))
    {
        return false;
    }
    out_device = device == SIM_Device::Head ? PUPPETXR_DEVICE_HEAD : device == SIM_Device::LeftHand ? PUPPETXR_DEVICE_LEFT_HAND : PUPPETXR_DEVICE_RIGHT_HAND;
    return true;
}

static b8 request_floats (const JsonValue& request, const char* key, f32* out_values, u32 count)
{
    const JsonValue array = json_find(request, key);
    if (count == 1 && json_is_number(array))
    {
        return json_as_float(array, out_values[0]);
    }
    if (!json_is_array(array) || json_length(array) == 0 || json_length(array) > count)
    {
        return false;
    }
    for (u32 i = 0; i < json_length(array); i++)
    {
        if (!json_as_float(json_at(array, i), out_values[i]))
        {
            return false;
        }
    }
    return true;
}

static PUPPETXR_Result command_status (const JsonValue&, API_Reply& reply, API_Client&)
{
    PUPPETXR_Status status = { .struct_size = sizeof(PUPPETXR_Status) };
    const PUPPETXR_Result result = api_get_status(&status);
    if (result != PUPPETXR_SUCCESS)
    {
        return result;
    }
    char room[SIM_MAX_NAME] = {};
    api_get_room(room, sizeof(room));
    OXR_DisplayState display = {};
    oxr_get_display_state(display);
    reply_u64(reply, "frame", status.frame_index);
    reply_u64(reply, "time_ns", (u64)status.time_ns);
    reply_u64(reply, "state_hash", status.state_hash);
    reply_bool(reply, "recording", status.recording != 0);
    reply_u64(reply, "recorded_frames", status.recorded_frames);
    reply_bool(reply, "replaying", status.replaying != 0);
    reply_bool(reply, "replay_finished", status.replay_finished != 0);
    reply_u64(reply, "replay_frame", status.replay_frame);
    reply_u64(reply, "replay_total", status.replay_total);
    reply_u64(reply, "divergences", status.divergences);
    reply_string(reply, "controller", sim_controller_name((SIM_Controller)status.controller));
    reply_string(reply, "room", room);
    reply_bool(reply, "controls", oxr_get_sim() && sim_controls_enabled(oxr_get_sim()));
    reply_string(reply, "control_mode", status.control_mode >= 0 ? api_control_mode_profiles[status.control_mode] : "");
    reply_string(reply, "profile", status.profile);
    reply_bool(reply, "paused", g_server.paused);
    reply_f32(reply, "refresh_rate_hz", display.refresh_rate);
    reply_string(reply, "color_space", display.color_space ? display.color_space : "rec709");
    reply_bool(reply, "local_dimming", display.local_dimming);
    reply_bool(reply, "boundary_suppressed", display.boundary_suppressed);
    reply_bool(reply, "mouse_captured", oxr_mouse_captured());
    return PUPPETXR_SUCCESS;
}

// What the app asked the controller to play, for scripts that check haptics
static PUPPETXR_Result command_get_haptic (const JsonValue& request, API_Reply& reply, API_Client&)
{
    PUPPETXR_Device device = PUPPETXR_DEVICE_HEAD;
    if (!request_device(request, device) || device == PUPPETXR_DEVICE_HEAD)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    oxr_lock();
    defer { oxr_unlock(); };
    const SIM_Sim* sim = oxr_get_sim();
    if (!sim)
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    static const char* const outputs[(u32)SIM_HapticOutput::Count] = { "main", "trigger", "thumb" };
    char                     name[16]                               = {};
    if (!json_find_string(request, "output", name, nullptr, sizeof(name)))
    {
        cstr_copy(name, sizeof(name), outputs[0]);
    }
    u32 output = 0;
    while (output < (u32)SIM_HapticOutput::Count && !cstr_equal(outputs[output], name))
    {
        output++;
    }
    if (output == (u32)SIM_HapticOutput::Count)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    const SIM_Haptic haptic    = sim_get_haptic(sim, device == PUPPETXR_DEVICE_LEFT_HAND ? SIM_Device::LeftHand : SIM_Device::RightHand, (SIM_HapticOutput)output);
    const i64        remaining = haptic.active ? haptic.end_time - sim_time(sim) : 0;
    reply_bool(reply, "active", haptic.active);
    reply_f32(reply, "amplitude", haptic.active ? haptic.amplitude : 0.0f);
    reply_f32(reply, "frequency", haptic.active ? haptic.frequency : 0.0f);
    reply_f32(reply, "remaining_s", (f32)((f64)(remaining > 0 ? remaining : 0) * 1e-9));
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result command_get_pose (const JsonValue& request, API_Reply& reply, API_Client&)
{
    PUPPETXR_Device device = {};
    if (!request_device(request, device))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    PUPPETXR_Pose      pose   = {};
    const PUPPETXR_Result result = api_get_device_pose(device, &pose);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_floats(reply, "position", pose.position, 3);
        reply_floats(reply, "orientation", pose.orientation, 4);
    }
    return result;
}

static PUPPETXR_Result command_set_pose (const JsonValue& request, API_Reply&, API_Client&)
{
    PUPPETXR_Device device = {};
    PUPPETXR_Pose pose = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } };
    if (!request_device(request, device) || !request_floats(request, "position", pose.position, 3))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (json_is_valid(json_find(request, "orientation")) && !request_floats(request, "orientation", pose.orientation, 4))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_set_device_pose(device, &pose);
}

static PUPPETXR_Result command_set_active (const JsonValue& request, API_Reply&, API_Client&)
{
    PUPPETXR_Device device = {};
    b8           active = true;
    if (!request_device(request, device) || !json_find_bool(request, "active", active))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_set_device_active(device, active ? 1 : 0);
}

static PUPPETXR_Result command_get_input (const JsonValue& request, API_Reply& reply, API_Client&)
{
    PUPPETXR_Device device = {};
    char         input[64] = {};
    if (!request_device(request, device) || !json_find_string(request, "input", input, nullptr, sizeof(input)))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    f32                value[2] = {};
    const PUPPETXR_Result result = api_get_input(device, input, &value[0], &value[1]);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_floats(reply, "value", value, 2);
    }
    return result;
}

static PUPPETXR_Result command_set_input (const JsonValue& request, API_Reply&, API_Client&)
{
    PUPPETXR_Device device = {};
    char         input[64] = {};
    f32          value[2]  = {};
    const JsonValue raw    = json_find(request, "value");
    b8           pressed   = false;
    if (!request_device(request, device) || !json_find_string(request, "input", input, nullptr, sizeof(input)))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (json_is_bool(raw) && json_as_bool(raw, pressed))
    {
        value[0] = pressed ? 1.0f : 0.0f;
    }
    else if (!(json_is_number(raw) ? json_as_float(raw, value[0]) : request_floats(request, "value", value, 2)))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_set_input(device, input, value[0], value[1]);
}

static PUPPETXR_Result request_enabled (const JsonValue& request, PUPPETXR_Result (*apply)(int32_t))
{
    b8 enabled = false;
    if (!json_find_bool(request, "enabled", enabled))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return apply(enabled ? 1 : 0);
}

static PUPPETXR_Result command_set_controls (const JsonValue& request, API_Reply&, API_Client&)
{
    return request_enabled(request, api_set_controls_enabled);
}

static PUPPETXR_Result command_set_fixed_step (const JsonValue& request, API_Reply&, API_Client&)
{
    return request_enabled(request, api_set_fixed_step);
}

static PUPPETXR_Result command_recenter (const JsonValue&, API_Reply&, API_Client&)
{
    return api_recenter();
}

static PUPPETXR_Result command_set_mirror_view (const JsonValue& request, API_Reply&, API_Client&)
{
    char view[16] = {};
    json_find_string(request, "view", view, nullptr, sizeof(view));
    if (cstr_equal(view, "left"))
    {
        return api_set_mirror_view(PUPPETXR_MIRROR_LEFT);
    }
    if (cstr_equal(view, "right"))
    {
        return api_set_mirror_view(PUPPETXR_MIRROR_RIGHT);
    }
    if (cstr_equal(view, "both"))
    {
        return api_set_mirror_view(PUPPETXR_MIRROR_BOTH);
    }
    return PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result request_path (const JsonValue& request, PUPPETXR_Result (*apply)(const char*))
{
    char path[OS_MAX_PATH] = {};
    if (!json_find_string(request, "path", path, nullptr, sizeof(path)) || !path[0])
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return apply(path);
}

static PUPPETXR_Result command_screenshot (const JsonValue& request, API_Reply& reply, API_Client&)
{
    const PUPPETXR_Result result = request_path(request, api_screenshot);
    u32                width = 0;
    u32                height = 0;
    u8                 probe[4];
    if (result == PUPPETXR_SUCCESS)
    {
        oxr_read_frame(probe, 0, width, height);
        reply_u64(reply, "width", width);
        reply_u64(reply, "height", height);
    }
    return result;
}

static PUPPETXR_Result command_record_start (const JsonValue& request, API_Reply&, API_Client&)
{
    return request_path(request, api_record_start);
}

static PUPPETXR_Result command_record_stop (const JsonValue&, API_Reply&, API_Client&)
{
    return api_record_stop();
}

static PUPPETXR_Result command_replay_start (const JsonValue& request, API_Reply&, API_Client&)
{
    return request_path(request, api_replay_start);
}

static PUPPETXR_Result command_replay_stop (const JsonValue&, API_Reply&, API_Client&)
{
    return api_replay_stop();
}

static PUPPETXR_Result command_request_exit (const JsonValue&, API_Reply&, API_Client&)
{
    return api_request_exit();
}

static PUPPETXR_Result command_get_controller (const JsonValue&, API_Reply& reply, API_Client&)
{
    PUPPETXR_Controller controller = PUPPETXR_CONTROLLER_META_TOUCH_PLUS;
    const PUPPETXR_Result result  = api_get_controller(&controller);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_string(reply, "controller", sim_controller_name((SIM_Controller)controller));
    }
    return result;
}

static PUPPETXR_Result command_get_virtual_keyboard (const JsonValue&, API_Reply& reply, API_Client&)
{
    PUPPETXR_VirtualKeyboard keyboard = { .struct_size = sizeof(PUPPETXR_VirtualKeyboard) };
    const PUPPETXR_Result result   = api_get_virtual_keyboard(&keyboard);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_bool(reply, "exists", keyboard.exists != 0);
        reply_bool(reply, "visible", keyboard.visible != 0);
        reply_bool(reply, "shifted", keyboard.shifted != 0);
        reply_floats(reply, "position", keyboard.pose.position, 3);
        reply_floats(reply, "orientation", keyboard.pose.orientation, 4);
        reply_f32(reply, "scale", keyboard.scale);
        reply_string(reply, "context", keyboard.context);
    }
    return result;
}

static PUPPETXR_Result command_get_virtual_keyboard_key (const JsonValue& request, API_Reply& reply, API_Client&)
{
    char name[16] = {};
    if (!json_find_string(request, "key", name, nullptr, sizeof(name)))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    PUPPETXR_Pose      pose   = {};
    const PUPPETXR_Result result = api_get_virtual_keyboard_key(name, &pose);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_floats(reply, "position", pose.position, 3);
        reply_floats(reply, "orientation", pose.orientation, 4);
    }
    return result;
}

static PUPPETXR_Result command_type_text (const JsonValue& request, API_Reply&, API_Client&)
{
    char text[SIM_MAX_TYPED + 1] = {};
    if (!json_find_string(request, "text", text, nullptr, sizeof(text)))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_type_text(text);
}

static PUPPETXR_Result command_get_props (const JsonValue&, API_Reply& reply, API_Client&)
{
    PUPPETXR_Prop props[SIM_MAX_PROPS] = {};
    props[0].struct_size            = sizeof(PUPPETXR_Prop);
    u32                count        = 0;
    const PUPPETXR_Result result    = api_get_props(props, SIM_MAX_PROPS, &count);
    if (result != PUPPETXR_SUCCESS)
    {
        return result;
    }
    reply_key(reply, "props");
    reply_append(reply, "[");
    for (u32 i = 0; i < count; i++)
    {
        const PUPPETXR_Prop& prop = props[i];
        reply_append(reply, "%s{\"id\":%u", i ? "," : "", prop.id);
        reply_string(reply, "kind", sim_prop_kind_name((SIM_PropKind)prop.kind));
        reply_string(reply, "motion", sim_prop_motion_name((SIM_PropMotion)prop.motion));
        reply_floats(reply, "position", prop.pose.position, 3);
        reply_floats(reply, "orientation", prop.pose.orientation, 4);
        reply_floats(reply, "target", prop.target, 3);
        reply_f32(reply, "radius", prop.radius);
        reply_f32(reply, "speed", prop.speed);
        reply_f32(reply, "size", prop.size);
        reply_string(reply, "text", prop.text);
        reply_floats(reply, "current_position", prop.current.position, 3);
        reply_floats(reply, "current_orientation", prop.current.orientation, 4);
        reply_append(reply, "}");
    }
    reply_append(reply, "]");
    return PUPPETXR_SUCCESS;
}

// The prop object of docs/props.md; update_prop applies only the fields given to the prop as it is
static PUPPETXR_Result command_add_prop (const JsonValue& request, API_Reply& reply, API_Client&)
{
    SIM_Prop prop = { .kind = SIM_PropKind::Count, .pose = zm::POSE_IDENTITY, .size = 0.1f };
    if (!sim_prop_from_json(request, prop))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    PUPPETXR_Prop api_prop = {};
    prop_to_api(prop, 0, api_prop);
    u32                id     = 0;
    const PUPPETXR_Result result = api_add_prop(&api_prop, &id);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_u64(reply, "prop", id);
    }
    return result;
}

static PUPPETXR_Result command_update_prop (const JsonValue& request, API_Reply&, API_Client&)
{
    u32 id = 0;
    if (!json_find_uint(request, "prop", id))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    SIM_Prop prop = {};
    {
        oxr_lock();
        defer { oxr_unlock(); };
        const SIM_Sim* sim = oxr_get_sim();
        if (!sim)
        {
            return PUPPETXR_ERROR_NO_INSTANCE;
        }
        const SIM_Prop* props = nullptr;
        const u32       count = sim_props(sim, props);
        u32             index = count;
        for (u32 i = 0; i < count; i++)
        {
            index = props[i].id == id ? i : index;
        }
        if (index == count)
        {
            return PUPPETXR_ERROR_INVALID_ARGUMENT;
        }
        prop = props[index];
    }
    if (!sim_prop_from_json(request, prop))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    PUPPETXR_Prop api_prop = {};
    prop_to_api(prop, 0, api_prop);
    return api_update_prop(id, &api_prop);
}

static PUPPETXR_Result command_remove_prop (const JsonValue& request, API_Reply&, API_Client&)
{
    u32 id = 0;
    if (!json_find_uint(request, "prop", id))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_remove_prop(id);
}

static PUPPETXR_Result command_get_hand_source (const JsonValue& request, API_Reply& reply, API_Client&)
{
    PUPPETXR_Device device = PUPPETXR_DEVICE_HEAD;
    if (!request_device(request, device))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    PUPPETXR_HandSource source = PUPPETXR_HAND_SOURCE_CONTROLLER;
    const PUPPETXR_Result result = api_get_hand_source(device, &source);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_string(reply, "source", sim_hand_source_name((SIM_HandSource)source));
    }
    return result;
}

static PUPPETXR_Result command_set_hand_source (const JsonValue& request, API_Reply&, API_Client&)
{
    PUPPETXR_Device device  = PUPPETXR_DEVICE_HEAD;
    char           name[16] = {};
    SIM_HandSource source   = SIM_HandSource::Controller;
    if (!request_device(request, device) || !json_find_string(request, "source", name, nullptr, sizeof(name)) || !sim_hand_source_from_name(name, source))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_set_hand_source(device, (PUPPETXR_HandSource)source);
}

static PUPPETXR_Result command_set_controller (const JsonValue& request, API_Reply&, API_Client&)
{
    char           name[32]   = {};
    SIM_Controller controller = SIM_Controller::OculusTouch;
    if (!json_find_string(request, "controller", name, nullptr, sizeof(name)) || !sim_controller_from_name(name, controller))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_set_controller((PUPPETXR_Controller)controller);
}

static PUPPETXR_Result command_get_room (const JsonValue&, API_Reply& reply, API_Client&)
{
    char               room[SIM_MAX_NAME] = {};
    const PUPPETXR_Result result          = api_get_room(room, sizeof(room));
    if (result == PUPPETXR_SUCCESS)
    {
        reply_string(reply, "room", room);
    }
    return result;
}

static PUPPETXR_Result command_set_room (const JsonValue& request, API_Reply&, API_Client&)
{
    char room[SIM_MAX_NAME] = {};
    json_find_string(request, "room", room, nullptr, sizeof(room));
    return api_set_room(room);
}

static PUPPETXR_Result command_get_control_mode (const JsonValue&, API_Reply& reply, API_Client&)
{
    PUPPETXR_ControlMode mode = PUPPETXR_CONTROL_MODE_BODY;
    const PUPPETXR_Result result = api_get_control_mode(&mode);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_string(reply, "mode", api_control_mode_profiles[mode]);
    }
    return result;
}

static PUPPETXR_Result command_set_control_mode (const JsonValue& request, API_Reply&, API_Client&)
{
    char name[16] = {};
    for (u32 i = 0; json_find_string(request, "mode", name, nullptr, sizeof(name)) && i < ARRAY_SIZE(api_control_mode_profiles); i++)
    {
        if (cstr_equal(name, api_control_mode_profiles[i]))
        {
            return api_set_control_mode((PUPPETXR_ControlMode)i);
        }
    }
    return PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result command_get_profile (const JsonValue&, API_Reply& reply, API_Client&)
{
    char               name[SIM_MAX_PROFILE_NAME] = {};
    const PUPPETXR_Result result                  = api_get_profile(name, sizeof(name));
    if (result == PUPPETXR_SUCCESS)
    {
        reply_string(reply, "profile", name);
    }
    return result;
}

static PUPPETXR_Result command_profiler_start (const JsonValue& request, API_Reply&, API_Client&)
{
    char path[OS_MAX_PATH] = {};
    if (!json_find_string(request, "path", path, nullptr, sizeof(path)))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_profiler_start(path);
}

static PUPPETXR_Result command_profiler_stop (const JsonValue&, API_Reply&, API_Client&)
{
    return api_profiler_stop();
}

static PUPPETXR_Result command_get_body_joints (const JsonValue&, API_Reply& reply, API_Client&)
{
    PUPPETXR_BodyJoint joints[SIM_BODY_JOINT_COUNT] = {};
    u32                count                        = 0;
    const PUPPETXR_Result result                    = api_get_body_joints(joints, SIM_BODY_JOINT_COUNT, &count);
    if (result != PUPPETXR_SUCCESS)
    {
        return result;
    }
    reply_key(reply, "joints");
    reply_append(reply, "[");
    for (u32 j = 0; j < count && j < SIM_BODY_JOINT_COUNT; j++)
    {
        reply_append(reply, "%s{\"name\":\"%s\"", j ? "," : "", sim_body_joint_name((SIM_BodyJoint)j));
        reply_floats(reply, "position", joints[j].pose.position, 3);
        reply_floats(reply, "orientation", joints[j].pose.orientation, 4);
        reply_bool(reply, "tracked", joints[j].tracked != 0);
        reply_f32(reply, "confidence", joints[j].confidence);
        reply_append(reply, "}");
    }
    reply_append(reply, "]");
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result command_get_body (const JsonValue&, API_Reply& reply, API_Client&)
{
    char               json[API_MAX_BODY_JSON];
    u32                length = 0;
    const PUPPETXR_Result result = api_get_body(json, sizeof(json), &length);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_key(reply, "body");
        reply_append(reply, "%s", json);
    }
    return result;
}

// The body object of docs/body-tracking.md as the request's "body"; with "replace" onto the
// configured body rather than the current one
static PUPPETXR_Result command_set_body (const JsonValue& request, API_Reply&, API_Client&)
{
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    b8 replace = false;
    json_find_bool(request, "replace", replace);
    return set_body_json(json_find(request, "body"), replace);
}

static PUPPETXR_Result command_reset_body (const JsonValue&, API_Reply&, API_Client&)
{
    return api_reset_body();
}

static PUPPETXR_Result command_get_face_frame (const JsonValue&, API_Reply& reply, API_Client&)
{
    f32                weights[SIM_FACE_EXPRESSION_COUNT] = {};
    u32                count                              = 0;
    PUPPETXR_Pose      eyes[2]                            = {};
    PUPPETXR_Result    result                             = api_get_face_weights(weights, SIM_FACE_EXPRESSION_COUNT, &count);
    if (result == PUPPETXR_SUCCESS)
    {
        result = api_get_eye_gazes(eyes);
    }
    if (result != PUPPETXR_SUCCESS)
    {
        return result;
    }
    reply_key(reply, "weights");
    reply_append(reply, "{");
    for (u32 e = 0; e < SIM_FACE_EXPRESSION_COUNT; e++)
    {
        reply_append(reply, "%s\"%s\":", e ? "," : "", sim_face_expression_name((SIM_FaceExpression)e));
        reply_number(reply, weights[e]);
    }
    reply_append(reply, "}");
    reply_key(reply, "eyes");
    reply_append(reply, "[");
    for (u32 e = 0; e < 2; e++)
    {
        reply_append(reply, "%s{", e ? "," : "");
        reply_append(reply, "\"position\":[");
        for (u32 i = 0; i < 3; i++)
        {
            reply_append(reply, i ? "," : "");
            reply_number(reply, eyes[e].position[i]);
        }
        reply_append(reply, "],\"orientation\":[");
        for (u32 i = 0; i < 4; i++)
        {
            reply_append(reply, i ? "," : "");
            reply_number(reply, eyes[e].orientation[i]);
        }
        reply_append(reply, "]}");
    }
    reply_append(reply, "]");
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result command_get_face (const JsonValue&, API_Reply& reply, API_Client&)
{
    char               json[API_MAX_BODY_JSON];
    u32                length = 0;
    const PUPPETXR_Result result = api_get_face(json, sizeof(json), &length);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_key(reply, "face");
        reply_append(reply, "%s", json);
    }
    return result;
}

// The face object of docs/face-tracking.md as the request's "face"; with "replace" onto the
// configured face rather than the current one
static PUPPETXR_Result command_set_face (const JsonValue& request, API_Reply&, API_Client&)
{
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    b8 replace = false;
    json_find_bool(request, "replace", replace);
    return set_face_json(json_find(request, "face"), replace);
}

static PUPPETXR_Result command_reset_face (const JsonValue&, API_Reply&, API_Client&)
{
    return api_reset_face();
}

static PUPPETXR_Result command_set_profile (const JsonValue& request, API_Reply&, API_Client&)
{
    char name[SIM_MAX_PROFILE_NAME] = {};
    if (!json_find_string(request, "profile", name, nullptr, sizeof(name)))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_set_profile(name);
}

static PUPPETXR_Result command_reset_poses (const JsonValue&, API_Reply&, API_Client&)
{
    return api_reset_poses();
}

static PUPPETXR_Result command_reset_hands (const JsonValue&, API_Reply&, API_Client&)
{
    return api_reset_hands();
}

static PUPPETXR_Result command_point_hand (const JsonValue& request, API_Reply&, API_Client&)
{
    PUPPETXR_Device device = {};
    char         eye[16] = "left";
    f32          u       = 0.0f;
    f32          v       = 0.0f;
    f32          pitch   = 0.0f;
    json_find_string(request, "eye", eye, nullptr, sizeof(eye), eye);
    if (!request_device(request, device) || !json_find_float(request, "u", u) || !json_find_float(request, "v", v) || !(cstr_equal(eye, "left") || cstr_equal(eye, "right")))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    f32 ahead = 0.0f;
    if (json_find_float(request, "ahead", ahead) && !(ahead >= 0.0f && ahead <= 10.0f))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    const b8 from_grip     = json_find_float(request, "grip_pitch", pitch);
    Pose     grip_from_ray = { .position = {}, .orientation = zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, pitch * zm::DEG_TO_RAD) };
    if (!from_grip)
    {
        oxr_lock();
        defer { oxr_unlock(); };
        const SIM_Sim* sim = oxr_get_sim();
        if (!sim)
        {
            return PUPPETXR_ERROR_NO_INSTANCE;
        }
        grip_from_ray = sim_get_config(sim).aim_offset;
    }
    // The ray's origin moved back along its +Z puts the hand that far ahead of the eye on the ray
    grip_from_ray = zm::pose_mul(grip_from_ray, Pose{ .position = { 0.0f, 0.0f, ahead }, .orientation = { 0.0f, 0.0f, 0.0f, 1.0f } });
    const PUPPETXR_Pose ray = { { grip_from_ray.position.x, grip_from_ray.position.y, grip_from_ray.position.z }, { grip_from_ray.orientation.x, grip_from_ray.orientation.y, grip_from_ray.orientation.z, grip_from_ray.orientation.w } };
    return api_point_hand(device, cstr_equal(eye, "left") ? 0 : 1, u, v, &ray);
}

static PUPPETXR_Result command_key (const JsonValue& request, API_Reply&, API_Client&)
{
    char key[32] = {};
    b8   down    = false;
    if (!json_find_string(request, "key", key, nullptr, sizeof(key)) || !json_find_bool(request, "down", down))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_control_key(key, down ? 1 : 0);
}

static PUPPETXR_Result command_ui_click (const JsonValue& request, API_Reply&, API_Client&)
{
    char label[48] = {};
    if (!json_find_string(request, "label", label, nullptr, sizeof(label)) || !label[0])
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    return oxr_ui_click(label) ? PUPPETXR_SUCCESS : PUPPETXR_ERROR_NOT_AVAILABLE;
}

static PUPPETXR_Result command_ui_find (const JsonValue& request, API_Reply& reply, API_Client&)
{
    char label[48] = {};
    if (!json_find_string(request, "label", label, nullptr, sizeof(label)) || !label[0])
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    f32 x       = 0.0f;
    f32 y       = 0.0f;
    b8  hovered  = false;
    f32 mouse[2] = {};
    if (!oxr_ui_find(label, x, y, hovered, mouse))
    {
        return PUPPETXR_ERROR_NOT_AVAILABLE;
    }
    reply_append(reply, ",\"x\":%.9g,\"y\":%.9g,\"hovered\":%s,\"mouse\":[%.9g,%.9g]", (f64)x, (f64)y, hovered ? "true" : "false", (f64)mouse[0], (f64)mouse[1]);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result command_set_lens_mask (const JsonValue& request, API_Reply&, API_Client&)
{
    b8 on = false;
    if (!json_find_bool(request, "on", on))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    oxr_set_lens_mask(on);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result command_show_ui (const JsonValue& request, API_Reply&, API_Client&)
{
    b8 shown = false;
    if (!json_find_bool(request, "shown", shown))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    oxr_show_ui(shown);
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result command_mouse (const JsonValue& request, API_Reply&, API_Client&)
{
    f32 delta[2] = {};
    if (!request_floats(request, "delta", delta, 2))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_control_mouse(delta[0], delta[1]);
}

static PUPPETXR_Result command_wait_frames (const JsonValue& request, API_Reply&, API_Client& client)
{
    u32 frames = 0;
    if (!json_find_uint(request, "frames", frames) || !oxr_get_sim())
    {
        return oxr_get_sim() ? PUPPETXR_ERROR_INVALID_ARGUMENT : PUPPETXR_ERROR_NO_INSTANCE;
    }
    client.waiting    = true;
    client.wait_frame = sim_frame_index(oxr_get_sim()) + frames;
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result command_pause (const JsonValue&, API_Reply&, API_Client&)
{
    g_server.paused = true;
    g_server.steps  = 0;
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result command_resume (const JsonValue&, API_Reply&, API_Client&)
{
    g_server.paused = false;
    g_server.steps  = 0;
    return PUPPETXR_SUCCESS;
}

// Pauses after that many frames and replies once they ran
static PUPPETXR_Result command_step (const JsonValue& request, API_Reply&, API_Client& client)
{
    u32 frames = 0;
    if (!oxr_get_sim())
    {
        return PUPPETXR_ERROR_NO_INSTANCE;
    }
    if (!json_find_uint(request, "frames", frames) || frames == 0)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    g_server.paused   = true;
    g_server.steps   += frames;
    client.waiting    = true;
    client.wait_frame = sim_frame_index(oxr_get_sim()) + g_server.steps;
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result command_get_anchors (const JsonValue&, API_Reply& reply, API_Client&)
{
    PUPPETXR_Anchor    anchors[API_MAX_LISTED_ANCHORS];
    uint32_t           count  = 0;
    const PUPPETXR_Result result = api_get_anchors(anchors, API_MAX_LISTED_ANCHORS, &count);
    if (result != PUPPETXR_SUCCESS)
    {
        return result;
    }
    reply_u64(reply, "count", count);
    reply_key(reply, "anchors");
    reply_append(reply, "[");
    for (u32 i = 0; i < count && i < API_MAX_LISTED_ANCHORS; i++)
    {
        const PUPPETXR_Anchor& anchor = anchors[i];
        reply_append(reply, "%s{\"uuid\":\"%s\",\"room\":\"%s\",\"cloud\":%s,\"owner\":%llu,\"position\":[%.9g,%.9g,%.9g],\"orientation\":[%.9g,%.9g,%.9g,%.9g]}", i ? "," : "", anchor.uuid, anchor.room, anchor.cloud ? "true" : "false", (unsigned long long)anchor.owner,
                     (f64)anchor.pose.position[0], (f64)anchor.pose.position[1], (f64)anchor.pose.position[2], (f64)anchor.pose.orientation[0], (f64)anchor.pose.orientation[1], (f64)anchor.pose.orientation[2], (f64)anchor.pose.orientation[3]);
    }
    reply_append(reply, "]");
    return PUPPETXR_SUCCESS;
}

static PUPPETXR_Result command_wipe_anchors (const JsonValue& request, API_Reply&, API_Client&)
{
    b8 cloud = false;
    json_find_bool(request, "cloud", cloud);
    return api_wipe_anchors(cloud ? 1 : 0);
}

static PUPPETXR_Result command_set_anchor_fault (const JsonValue& request, API_Reply&, API_Client&)
{
    static const char* const ops[] = { "save", "erase", "load", "share" };
    char op[16]   = {};
    i32  result   = 0;
    u32  count    = 1;
    json_find_string(request, "op", op, nullptr, sizeof(op));
    json_find_int(request, "result", result);
    json_find_uint(request, "count", count, count);
    for (u32 i = 0; i < ARRAY_SIZE(ops); i++)
    {
        if (cstr_equal(op, ops[i]))
        {
            return api_set_anchor_fault((PUPPETXR_AnchorOp)i, result, count);
        }
    }
    return PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result command_set_frame_fault (const JsonValue& request, API_Reply&, API_Client&)
{
    static const char* const ops[] = { "wait", "begin", "locate", "end" };
    char op[16]   = {};
    i32  result   = 0;
    u32  count    = 1;
    json_find_string(request, "op", op, nullptr, sizeof(op));
    json_find_int(request, "result", result);
    json_find_uint(request, "count", count, count);
    for (u32 i = 0; i < ARRAY_SIZE(ops); i++)
    {
        if (cstr_equal(op, ops[i]))
        {
            return api_set_frame_fault((PUPPETXR_FrameOp)i, result, count);
        }
    }
    return PUPPETXR_ERROR_INVALID_ARGUMENT;
}

static PUPPETXR_Result command_set_anchor_timing (const JsonValue& request, API_Reply&, API_Client&)
{
    u32 latency  = 0;
    u32 localize = 0;
    if (!json_find_uint(request, "latency_frames", latency) || !json_find_uint(request, "localize_frames", localize))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_set_anchor_timing(latency, localize);
}

static PUPPETXR_Result command_get_origin_offset (const JsonValue&, API_Reply& reply, API_Client&)
{
    PUPPETXR_Pose      pose   = {};
    const PUPPETXR_Result result = api_get_origin_offset(&pose);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_floats(reply, "position", pose.position, 3);
        reply_floats(reply, "orientation", pose.orientation, 4);
    }
    return result;
}

// The yaw of a drift, which turns about +Y only
static f32 drift_yaw_degrees (const PUPPETXR_Pose& drift)
{
    return 2.0f * zm::atan2(drift.orientation[1], drift.orientation[3]) * zm::RAD_TO_DEG;
}

static PUPPETXR_Result command_get_tracking_drift (const JsonValue&, API_Reply& reply, API_Client&)
{
    PUPPETXR_Pose      drift       = {};
    f32                velocity[3] = {};
    f32                yaw_rate    = 0.0f;
    const PUPPETXR_Result result   = api_get_tracking_drift(&drift, velocity, &yaw_rate);
    if (result == PUPPETXR_SUCCESS)
    {
        reply_floats(reply, "position", drift.position, 3);
        reply_floats(reply, "orientation", drift.orientation, 4);
        reply_f32(reply, "yaw_degrees", drift_yaw_degrees(drift));
        reply_floats(reply, "velocity", velocity, 3);
        reply_f32(reply, "yaw_rate", yaw_rate);
    }
    return result;
}

// Members are optional: position, then orientation or yaw_degrees; none removes the drift
static PUPPETXR_Result command_set_tracking_drift (const JsonValue& request, API_Reply&, API_Client&)
{
    PUPPETXR_Pose drift = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } };
    f32        yaw   = 0.0f;
    if (json_is_valid(json_find(request, "position")) && !request_floats(request, "position", drift.position, 3))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    const b8 has_orientation = json_is_valid(json_find(request, "orientation"));
    const b8 has_yaw         = json_is_valid(json_find(request, "yaw_degrees"));
    if ((has_orientation && (has_yaw || !request_floats(request, "orientation", drift.orientation, 4))) || (has_yaw && !json_find_float(request, "yaw_degrees", yaw)))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (has_yaw)
    {
        const Quat turn = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, yaw * zm::DEG_TO_RAD);
        drift.orientation[1] = turn.y;
        drift.orientation[3] = turn.w;
    }
    return api_set_tracking_drift(&drift);
}

static PUPPETXR_Result command_set_tracking_drift_rate (const JsonValue& request, API_Reply&, API_Client&)
{
    f32 velocity[3] = {};
    f32 yaw_rate    = 0.0f;
    if (json_is_valid(json_find(request, "velocity")) && !request_floats(request, "velocity", velocity, 3))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (json_is_valid(json_find(request, "yaw_rate")) && !json_find_float(request, "yaw_rate", yaw_rate))
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    return api_set_tracking_drift_rate(velocity, yaw_rate);
}

// Both at once, so the drift reached under a rate does not move on in between
static PUPPETXR_Result command_reset_tracking_drift (const JsonValue&, API_Reply&, API_Client&)
{
    const PUPPETXR_Result result = api_set_tracking_drift_rate(nullptr, 0.0f);
    return result == PUPPETXR_SUCCESS ? api_set_tracking_drift(nullptr) : result;
}

typedef PUPPETXR_Result (*API_Command)(const JsonValue& request, API_Reply& reply, API_Client& client);

const static struct
{
    const char* name;
    API_Command run;
} api_commands[] = {
    { "status",          command_status },
    { "get_pose",        command_get_pose },
    { "get_haptic",      command_get_haptic },
    { "set_pose",        command_set_pose },
    { "set_active",      command_set_active },
    { "get_input",       command_get_input },
    { "set_input",       command_set_input },
    { "set_controls",    command_set_controls },
    { "set_fixed_step",  command_set_fixed_step },
    { "recenter",        command_recenter },
    { "set_mirror_view", command_set_mirror_view },
    { "screenshot",      command_screenshot },
    { "record_start",    command_record_start },
    { "record_stop",     command_record_stop },
    { "replay_start",    command_replay_start },
    { "replay_stop",     command_replay_stop },
    { "request_exit",    command_request_exit },
    { "get_controller",  command_get_controller },
    { "set_controller",  command_set_controller },
    { "get_hand_source", command_get_hand_source },
    { "set_hand_source", command_set_hand_source },
    { "get_virtual_keyboard", command_get_virtual_keyboard },
    { "get_virtual_keyboard_key", command_get_virtual_keyboard_key },
    { "type_text",       command_type_text },
    { "get_props",       command_get_props },
    { "add_prop",        command_add_prop },
    { "update_prop",     command_update_prop },
    { "remove_prop",     command_remove_prop },
    { "get_room",        command_get_room },
    { "set_room",        command_set_room },
    { "wait_frames",     command_wait_frames },
    { "pause",           command_pause },
    { "resume",          command_resume },
    { "step",            command_step },
    { "get_control_mode", command_get_control_mode },
    { "set_control_mode", command_set_control_mode },
    { "reset_poses",     command_reset_poses },
    { "reset_hands",     command_reset_hands },
    { "point_hand",      command_point_hand },
    { "get_profile",     command_get_profile },
    { "set_profile",     command_set_profile },
    { "profiler_start",  command_profiler_start },
    { "profiler_stop",   command_profiler_stop },
    { "get_body_joints", command_get_body_joints },
    { "get_body",        command_get_body },
    { "set_body",        command_set_body },
    { "reset_body",      command_reset_body },
    { "get_face_frame",  command_get_face_frame },
    { "get_face",        command_get_face },
    { "set_face",        command_set_face },
    { "reset_face",      command_reset_face },
    { "key",             command_key },
    { "mouse",           command_mouse },
    { "ui_click",        command_ui_click },
    { "ui_find",         command_ui_find },
    { "show_ui",         command_show_ui },
    { "set_lens_mask",   command_set_lens_mask },
    { "get_anchors",     command_get_anchors },
    { "wipe_anchors",    command_wipe_anchors },
    { "set_anchor_fault", command_set_anchor_fault },
    { "set_frame_fault", command_set_frame_fault },
    { "set_anchor_timing", command_set_anchor_timing },
    { "get_origin_offset", command_get_origin_offset },
    { "get_tracking_drift", command_get_tracking_drift },
    { "set_tracking_drift", command_set_tracking_drift },
    { "set_tracking_drift_rate", command_set_tracking_drift_rate },
    { "reset_tracking_drift", command_reset_tracking_drift },
};

static void handle_hello (API_Client& client, const JsonValue& request, b8 has_id, i64 id)
{
    char version[32] = {};
    char wanted[32]  = {};
    version_string(version, sizeof(version));
    json_find_string(request, "version", wanted, nullptr, sizeof(wanted));
    if (!cstr_equal(version, wanted))
    {
        char message[128];
        cstr_format(message, sizeof(message), "simulator is %s, client is %s; both must be the same version", version, wanted);
        reply_error(client, has_id, id, "version", message);
        client.closing = true;
        return;
    }
    client.greeted     = true;
    g_server.connected = true;
    API_Reply reply = {};
    reply_begin(reply, has_id, id, true);
    reply_string(reply, "version", version);
    reply_u64(reply, "pid", os_process_id());
    reply_string(reply, "app", g_server.app);
    reply_u64(reply, "frame", oxr_get_sim() ? sim_frame_index(oxr_get_sim()) : 0);
    reply_finish(client, reply);
}

static void handle_line (API_Client& client, const char* line)
{
    JsonParser* parser = json_parse_from_string(line);
    defer { json_release(parser); };
    const JsonValue request = parser ? json_get_root(parser) : JsonValue{};
    if (!json_is_object(request))
    {
        reply_error(client, false, 0, "bad_request", "expected one JSON object per line");
        return;
    }
    i32       id32   = 0;
    const b8  has_id = json_find_int(request, "id", id32);
    const i64 id     = id32;
    char      cmd[32] = {};
    json_find_string(request, "cmd", cmd, nullptr, sizeof(cmd));
    if (cstr_equal(cmd, "hello"))
    {
        handle_hello(client, request, has_id, id);
        return;
    }
    if (!client.greeted)
    {
        reply_error(client, has_id, id, "hello_first", "send hello with the client version first");
        return;
    }
    for (const auto& command : api_commands)
    {
        if (!cstr_equal(command.name, cmd))
        {
            continue;
        }
        API_Reply reply = {};
        reply_begin(reply, has_id, id, true);
        const PUPPETXR_Result result = command.run(request, reply, client);
        if (result != PUPPETXR_SUCCESS)
        {
            reply_error(client, has_id, id, result_name(result), cmd);
        }
        else if (client.waiting)
        {
            client.wait_id = has_id ? id : -1;
        }
        else
        {
            reply_finish(client, reply);
        }
        return;
    }
    reply_error(client, has_id, id, "unknown_command", cmd);
}

static char* find_newline (char* data, usize size)
{
    for (usize i = 0; i < size; i++)
    {
        if (data[i] == '\n')
        {
            return data + i;
        }
    }
    return nullptr;
}

static void client_close (API_Client& client)
{
    os_socket_close(client.socket);
    client.output.release();
    client = {};
}

static void client_poll (API_Client& client)
{
    while (client.output.length > 0 && !client.closing)
    {
        usize                 sent   = 0;
        const OS_SocketResult result = os_socket_send(client.socket, client.output.data, client.output.length, sent);
        if (result == OS_SocketResult::Closed)
        {
            client.closing = true;
        }
        if (result != OS_SocketResult::Ok || sent == 0)
        {
            break;
        }
        memory_move(client.output.data, client.output.data + sent, client.output.length - sent);
        client.output.length -= sent;
    }
    if (client.closing)
    {
        if (client.output.length == 0 || !os_socket_wait(client.socket, 0, true))
        {
            client_close(client);
        }
        return;
    }

    // A frame counts once it ended: an app ending frames on another thread than the one waiting
    // for them (Unreal's render thread) has the last one in flight when the reply would be due,
    // and a screenshot after it showed the frame before in some runs
    const SIM_Sim* sim = oxr_get_sim();
    if (client.waiting && sim && sim_frame_index(sim) >= client.wait_frame && client.reached_ns == 0)
    {
        client.reached_ns = os_monotonic_ns();
    }
    if (client.waiting && client.reached_ns != 0 && (!oxr_frames_in_flight() || os_monotonic_ns() - client.reached_ns >= API_END_FRAME_WAIT_NS))
    {
        client.waiting    = false;
        client.reached_ns = 0;
        API_Reply reply = {};
        reply_begin(reply, client.wait_id >= 0, client.wait_id, true);
        reply_u64(reply, "frame", sim_frame_index(sim));
        reply_finish(client, reply);
    }

    // At most one buffer per poll: the rest waits in the socket until the buffered lines are
    // handled, so a client sending faster than frames run cannot stall the frame loop
    while (!client.closing && client.input_length < sizeof(client.input) - 1)
    {
        usize                 received = 0;
        const OS_SocketResult result   = os_socket_receive(client.socket, client.input + client.input_length, sizeof(client.input) - 1 - client.input_length, received);
        if (result == OS_SocketResult::Closed)
        {
            client_close(client);
            return;
        }
        if (result != OS_SocketResult::Ok)
        {
            break;
        }
        client.input_length += received;
        if (client.input_length == sizeof(client.input) - 1 && !find_newline(client.input, client.input_length))
        {
            reply_error(client, false, 0, "bad_request", "line too long");
            client.closing = true;
        }
    }

    // A pending wait holds back the client's next requests so replies keep their order
    while (!client.waiting && !client.closing)
    {
        char* end = find_newline(client.input, client.input_length);
        if (!end)
        {
            break;
        }
        *end = 0;
        handle_line(client, client.input);
        const usize consumed = (usize)(end + 1 - client.input);
        memory_move(client.input, end + 1, client.input_length - consumed);
        client.input_length -= consumed;
    }
}

static void server_accept_and_poll ()
{
    while (OS_Socket* socket = os_socket_accept(g_server.listener))
    {
        API_Client* slot = nullptr;
        for (API_Client& client : g_server.clients)
        {
            slot = !slot && !client.socket ? &client : slot;
        }
        if (!slot)
        {
            LOG_WARNING("Control server: more than %u clients, refusing one", API_MAX_CLIENTS);
            os_socket_close(socket);
            continue;
        }
        *slot = { .socket = socket };
    }
    for (API_Client& client : g_server.clients)
    {
        if (client.socket)
        {
            client_poll(client);
        }
    }
}

// While paused, xrWaitFrame stays here and keeps serving clients, with the runtime unlocked in
// between so that other threads end the frames in flight. When the last client leaves, the
// simulator resumes so that a crashed driver cannot hang the app.
static void server_poll (b8 frame)
{
    if (!g_server.running)
    {
        return;
    }
    server_accept_and_poll();
    while (frame && g_server.running && g_server.paused && g_server.steps == 0)
    {
        b8 any = false;
        for (const API_Client& client : g_server.clients)
        {
            any = any || client.socket != nullptr;
        }
        if (g_server.connected && !any)
        {
            LOG_INFO("Control server: the last client left, resuming");
            g_server.paused = false;
            break;
        }
        oxr_unlock();
        os_sleep_ns(1000000);
        oxr_lock();
        server_accept_and_poll();
    }
    if (frame && g_server.paused && g_server.steps > 0)
    {
        g_server.steps--;
    }
}

static b8 discovery_dir (char* out, usize size)
{
    char runtime[OS_MAX_PATH];
    return os_runtime_dir(runtime, sizeof(runtime)) && cstr_format(out, size, "%s/puppetxr", runtime) < size && os_create_directory(out);
}

static void sweep_entry (const char* name, void* user) // NOLINT(misc-const-correctness): os_list_directory callback
{
    const char* dir = (const char*)user;
    const usize length = cstr_length(name);
    if (length < 5 || !(cstr_equal(name + length - 5, ".json") || (length > 4 && cstr_equal(name + length - 4, ".tmp"))))
    {
        return;
    }
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/%s", dir, name);
    if (os_lock_file_is_stale(path))
    {
        os_delete_file(path);
    }
}

static void server_start (const SIM_Config& config, const char* application_name)
{
    if (!config.control || g_server.running)
    {
        return;
    }
    if (config.control_port > 65535)
    {
        LOG_ERROR("Control port %u is out of range", config.control_port);
        return;
    }
    char dir[OS_MAX_PATH];
    if (!discovery_dir(dir, sizeof(dir)))
    {
        LOG_ERROR("Control server: no runtime directory for discovery files");
        return;
    }
    os_list_directory(dir, sweep_entry, dir);

    OS_Socket* listener = os_socket_listen(config.control_address, (u16)config.control_port);
    if (!listener)
    {
        return;
    }
    cstr_copy(g_server.app, sizeof(g_server.app), application_name);
    char version[32];
    version_string(version, sizeof(version));
    API_Reply content = {};
    content.length = 0;
    reply_append(content, "{\"pid\":%u", os_process_id());
    reply_u64(content, "port", os_socket_port(listener));
    reply_string(content, "address", config.control_address);
    reply_string(content, "app", application_name);
    reply_string(content, "version", version);
    reply_append(content, "}\n");

    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/%u.json", dir, os_process_id());
    g_server.discovery = os_lock_file_create(path, content.text, content.length);
    g_server.listener  = listener;
    g_server.running   = true;
    g_server.paused    = config.control_paused;
    LOG_INFO("Control server on %s:%u (%s)", config.control_address, (u32)os_socket_port(listener), path);
    if (!cstr_equal(config.control_address, "127.0.0.1"))
    {
        LOG_WARNING("Control server listens on %s: anyone who reaches it can drive this simulator", config.control_address);
    }
}

static void server_stop ()
{
    if (!g_server.running)
    {
        return;
    }
    for (API_Client& client : g_server.clients)
    {
        if (client.socket)
        {
            client_close(client);
        }
    }
    os_socket_close(g_server.listener);
    os_lock_file_release(g_server.discovery);
    g_server = {};
}

// Registered when the library loads; the oxr layer below cannot call up into this one
[[maybe_unused]] static const b8 g_hooks_registered = (oxr_set_control_hooks({ .instance_created = server_start, .instance_destroyed = server_stop, .poll = server_poll }), true);

// Bytes of PUPPETXR_Api each API version defines; a client of version v gets only that prefix
static usize api_table_size (u32 version)
{
    switch (version)
    {
    case 1:
        return offsetof(PUPPETXR_Api, set_frame_fault) + sizeof(void*);
    default:
        return 0;
    }
}

//
// Public API
//

PUPPETXR_Result puppetxr_get_api (uint32_t version, PUPPETXR_Api* out_api)
{
    if (!out_api)
    {
        return PUPPETXR_ERROR_INVALID_ARGUMENT;
    }
    if (version == 0)
    {
        return PUPPETXR_ERROR_VERSION;
    }
    const u32       filled = zm::min(version, (u32)PUPPETXR_API_VERSION);
    const PUPPETXR_Api table = {
        .version              = filled,
        .runtime_version      = PUPPETXR_VERSION,
        .get_status           = api_get_status,
        .get_device_pose      = api_get_device_pose,
        .set_device_pose      = api_set_device_pose,
        .set_device_active    = api_set_device_active,
        .get_input            = api_get_input,
        .set_input            = api_set_input,
        .set_fixed_step       = api_set_fixed_step,
        .set_controls_enabled = api_set_controls_enabled,
        .recenter             = api_recenter,
        .set_mirror_view      = api_set_mirror_view,
        .screenshot           = api_screenshot,
        .record_start         = api_record_start,
        .record_stop          = api_record_stop,
        .replay_start         = api_replay_start,
        .replay_stop          = api_replay_stop,
        .request_exit         = api_request_exit,
        .get_controller       = api_get_controller,
        .set_controller       = api_set_controller,
        .set_room             = api_set_room,
        .get_room             = api_get_room,
        .set_log_callback     = api_set_log_callback,
        .get_control_mode     = api_get_control_mode,
        .set_control_mode     = api_set_control_mode,
        .reset_poses          = api_reset_poses,
        .reset_hands          = api_reset_hands,
        .control_key          = api_control_key,
        .control_mouse        = api_control_mouse,
        .set_disabled_extensions = api_set_disabled_extensions,
        .get_anchors          = api_get_anchors,
        .wipe_anchors         = api_wipe_anchors,
        .set_anchor_fault     = api_set_anchor_fault,
        .set_anchor_timing    = api_set_anchor_timing,
        .get_origin_offset    = api_get_origin_offset,
        .get_hand_source      = api_get_hand_source,
        .set_hand_source      = api_set_hand_source,
        .get_virtual_keyboard     = api_get_virtual_keyboard,
        .get_virtual_keyboard_key = api_get_virtual_keyboard_key,
        .type_text                = api_type_text,
        .add_prop                 = api_add_prop,
        .update_prop              = api_update_prop,
        .remove_prop              = api_remove_prop,
        .get_props                = api_get_props,
        .point_hand               = api_point_hand,
        .get_profile              = api_get_profile,
        .set_profile              = api_set_profile,
        .profiler_start           = api_profiler_start,
        .profiler_stop            = api_profiler_stop,
        .get_body_joints          = api_get_body_joints,
        .get_body                 = api_get_body,
        .set_body                 = api_set_body,
        .reset_body               = api_reset_body,
        .get_face_weights         = api_get_face_weights,
        .get_eye_gazes            = api_get_eye_gazes,
        .get_face                 = api_get_face,
        .set_face                 = api_set_face,
        .reset_face               = api_reset_face,
        .set_tracking_drift       = api_set_tracking_drift,
        .set_tracking_drift_rate  = api_set_tracking_drift_rate,
        .get_tracking_drift       = api_get_tracking_drift,
        .set_frame_fault          = api_set_frame_fault,
    };
    memory_copy(out_api, &table, api_table_size(filled));
    return PUPPETXR_SUCCESS;
}

PUPPETXR_Result puppetxr_get_version (uint32_t* out_runtime_version, uint32_t* out_api_version)
{
    if (out_runtime_version)
    {
        *out_runtime_version = PUPPETXR_VERSION;
    }
    if (out_api_version)
    {
        *out_api_version = PUPPETXR_API_VERSION;
    }
    return PUPPETXR_SUCCESS;
}
