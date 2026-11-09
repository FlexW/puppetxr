#include "oxr_internal.h"
#include "blocks.h" // COMP_DEPTH_NEAR, COMP_COLOR_LUT_MAX_RESOLUTION

#include <time.h>
#define XR_USE_TIMESPEC
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection_parent_structs.h>

#include <math.h>

//
// Types
//

namespace
{

// From the first xrCreateInstance of a run to its first frame with layers (startup_log); a run
// ends with the instance destroyed after that frame.
// durations and milestones in ns, milestones from `origin`
struct OXR_Startup
{
    u64 origin;
    u64 in_calls; // the lock held
    u64 entered;
    u32 lock_depth;
    b8  logged;
    u64 room;
    u64 window;
    u64 device;
    u64 compositor;
    u64 ui;
    u64 room_upload;
    u64 session_created;
    u64 session_running;
    u64 first_wait;
    u64 first_end;
};

} // namespace

//
// Statics
//

static OXR_Runtime*     g_runtime;
static OXR_ControlHooks g_control_hooks;
static OXR_Startup      oxr_startup;

static const char* const oxr_top_level_paths[(u32)OXR_TopLevel::Count] = {
    "/user/hand/left",
    "/user/hand/right",
    "/user/head",
    "/user/gamepad",
    "/user/detached_controller_meta/left",
    "/user/detached_controller_meta/right",
    "/user/eyes_ext",
};

static const char* const oxr_profile_paths[(u32)OXR_Profile::Count] = {
    "/interaction_profiles/khr/simple_controller",
    "/interaction_profiles/oculus/touch_controller",
    "/interaction_profiles/facebook/touch_controller_pro",
    "/interaction_profiles/meta/touch_controller_plus",
    "/interaction_profiles/meta/touch_controller_quest_2",
    "/interaction_profiles/meta/touch_pro_controller",
    "/interaction_profiles/meta/touch_plus_controller",
    "/interaction_profiles/ext/hand_interaction_ext",
    "/interaction_profiles/ext/eye_gaze_interaction",
};

#define L 1
#define R 2
#define LR 3

static const OXR_ProfileComponent oxr_simple_components[] = {
    { "/input/select/click",      OXR_SourceKind::FloatAsBool, SIM_Input::TriggerValue, SIM_HandPose::Grip, LR },
    { "/input/select",            OXR_SourceKind::FloatAsBool, SIM_Input::TriggerValue, SIM_HandPose::Grip, LR },
    { "/input/menu/click",        OXR_SourceKind::Bool,        SIM_Input::MenuClick,    SIM_HandPose::Grip, LR },
    { "/input/menu",              OXR_SourceKind::Bool,        SIM_Input::MenuClick,    SIM_HandPose::Grip, LR },
    { "/input/grip/pose",         OXR_SourceKind::Pose,        SIM_Input::Count,        SIM_HandPose::Grip, LR },
    { "/input/aim/pose",          OXR_SourceKind::Pose,        SIM_Input::Count,        SIM_HandPose::Aim,  LR },
    { "/input/grip",              OXR_SourceKind::Pose,        SIM_Input::Count,        SIM_HandPose::Grip, LR },
    { "/input/aim",               OXR_SourceKind::Pose,        SIM_Input::Count,        SIM_HandPose::Aim,  LR },
    { "/input/grip_surface/pose", OXR_SourceKind::Pose,        SIM_Input::Count,        SIM_HandPose::Palm, LR },
    { "/input/grip_surface",      OXR_SourceKind::Pose,        SIM_Input::Count,        SIM_HandPose::Palm, LR },
    { "/input/palm_ext/pose",     OXR_SourceKind::Pose,        SIM_Input::Count,        SIM_HandPose::Palm, LR },
    { "/input/pinch_ext/pose",    OXR_SourceKind::Pose,        SIM_Input::Count,        SIM_HandPose::Pinch, LR },
    { "/input/poke_ext/pose",     OXR_SourceKind::Pose,        SIM_Input::Count,        SIM_HandPose::Poke, LR },
    { "/output/haptic",           OXR_SourceKind::Haptic,      SIM_Input::Count,        SIM_HandPose::Grip, LR },
};

static const OXR_ProfileComponent oxr_simple_on_simple_components[] = {
    { "/input/select/click",      OXR_SourceKind::Bool,   SIM_Input::SelectClick, SIM_HandPose::Grip, LR },
    { "/input/select",            OXR_SourceKind::Bool,   SIM_Input::SelectClick, SIM_HandPose::Grip, LR },
    { "/input/menu/click",        OXR_SourceKind::Bool,   SIM_Input::MenuClick,   SIM_HandPose::Grip, LR },
    { "/input/menu",              OXR_SourceKind::Bool,   SIM_Input::MenuClick,   SIM_HandPose::Grip, LR },
    { "/input/grip/pose",         OXR_SourceKind::Pose,   SIM_Input::Count,       SIM_HandPose::Grip, LR },
    { "/input/aim/pose",          OXR_SourceKind::Pose,   SIM_Input::Count,       SIM_HandPose::Aim,  LR },
    { "/input/grip",              OXR_SourceKind::Pose,   SIM_Input::Count,       SIM_HandPose::Grip, LR },
    { "/input/aim",               OXR_SourceKind::Pose,   SIM_Input::Count,       SIM_HandPose::Aim,  LR },
    { "/input/grip_surface/pose", OXR_SourceKind::Pose,   SIM_Input::Count,       SIM_HandPose::Palm, LR },
    { "/input/grip_surface",      OXR_SourceKind::Pose,   SIM_Input::Count,       SIM_HandPose::Palm, LR },
    { "/output/haptic",           OXR_SourceKind::Haptic, SIM_Input::Count,       SIM_HandPose::Grip, LR },
};

// Every Touch generation shares these; the profile tables differ in what follows
#define OXR_TOUCH_COMPONENTS                                                                                     \
    { "/input/x/click",           OXR_SourceKind::Bool,   SIM_Input::XClick,          SIM_HandPose::Grip, L },   \
    { "/input/x",                 OXR_SourceKind::Bool,   SIM_Input::XClick,          SIM_HandPose::Grip, L },   \
    { "/input/x/touch",           OXR_SourceKind::Bool,   SIM_Input::XTouch,          SIM_HandPose::Grip, L },   \
    { "/input/y/click",           OXR_SourceKind::Bool,   SIM_Input::YClick,          SIM_HandPose::Grip, L },   \
    { "/input/y",                 OXR_SourceKind::Bool,   SIM_Input::YClick,          SIM_HandPose::Grip, L },   \
    { "/input/y/touch",           OXR_SourceKind::Bool,   SIM_Input::YTouch,          SIM_HandPose::Grip, L },   \
    { "/input/menu/click",        OXR_SourceKind::Bool,   SIM_Input::MenuClick,       SIM_HandPose::Grip, L },   \
    { "/input/menu",              OXR_SourceKind::Bool,   SIM_Input::MenuClick,       SIM_HandPose::Grip, L },   \
    { "/input/a/click",           OXR_SourceKind::Bool,   SIM_Input::AClick,          SIM_HandPose::Grip, R },   \
    { "/input/a",                 OXR_SourceKind::Bool,   SIM_Input::AClick,          SIM_HandPose::Grip, R },   \
    { "/input/a/touch",           OXR_SourceKind::Bool,   SIM_Input::ATouch,          SIM_HandPose::Grip, R },   \
    { "/input/b/click",           OXR_SourceKind::Bool,   SIM_Input::BClick,          SIM_HandPose::Grip, R },   \
    { "/input/b",                 OXR_SourceKind::Bool,   SIM_Input::BClick,          SIM_HandPose::Grip, R },   \
    { "/input/b/touch",           OXR_SourceKind::Bool,   SIM_Input::BTouch,          SIM_HandPose::Grip, R },   \
    { "/input/system/click",      OXR_SourceKind::Bool,   SIM_Input::SystemClick,     SIM_HandPose::Grip, R },   \
    { "/input/system",            OXR_SourceKind::Bool,   SIM_Input::SystemClick,     SIM_HandPose::Grip, R },   \
    { "/input/squeeze/value",     OXR_SourceKind::Float,  SIM_Input::SqueezeValue,    SIM_HandPose::Grip, LR },  \
    { "/input/squeeze",           OXR_SourceKind::Float,  SIM_Input::SqueezeValue,    SIM_HandPose::Grip, LR },  \
    { "/input/trigger/value",     OXR_SourceKind::Float,  SIM_Input::TriggerValue,    SIM_HandPose::Grip, LR },  \
    { "/input/trigger",           OXR_SourceKind::Float,  SIM_Input::TriggerValue,    SIM_HandPose::Grip, LR },  \
    { "/input/trigger/touch",     OXR_SourceKind::Bool,   SIM_Input::TriggerTouch,    SIM_HandPose::Grip, LR },  \
    { "/input/thumbstick",        OXR_SourceKind::Vec2,   SIM_Input::Thumbstick,      SIM_HandPose::Grip, LR },  \
    { "/input/thumbstick/x",      OXR_SourceKind::Vec2X,  SIM_Input::Thumbstick,      SIM_HandPose::Grip, LR },  \
    { "/input/thumbstick/y",      OXR_SourceKind::Vec2Y,  SIM_Input::Thumbstick,      SIM_HandPose::Grip, LR },  \
    { "/input/thumbstick/click",  OXR_SourceKind::Bool,   SIM_Input::ThumbstickClick, SIM_HandPose::Grip, LR },  \
    { "/input/thumbstick/touch",  OXR_SourceKind::Bool,   SIM_Input::ThumbstickTouch, SIM_HandPose::Grip, LR },  \
    { "/input/thumbrest/touch",   OXR_SourceKind::Bool,   SIM_Input::ThumbrestTouch,  SIM_HandPose::Grip, LR },  \
    { "/input/grip/pose",         OXR_SourceKind::Pose,   SIM_Input::Count,           SIM_HandPose::Grip, LR },  \
    { "/input/aim/pose",          OXR_SourceKind::Pose,   SIM_Input::Count,           SIM_HandPose::Aim,  LR },  \
    { "/input/grip",              OXR_SourceKind::Pose,   SIM_Input::Count,           SIM_HandPose::Grip, LR },  \
    { "/input/aim",               OXR_SourceKind::Pose,   SIM_Input::Count,           SIM_HandPose::Aim,  LR },  \
    { "/input/grip_surface/pose", OXR_SourceKind::Pose,   SIM_Input::Count,           SIM_HandPose::Palm, LR },  \
    { "/input/grip_surface",      OXR_SourceKind::Pose,   SIM_Input::Count,           SIM_HandPose::Palm, LR },  \
    { "/input/palm_ext/pose",     OXR_SourceKind::Pose,   SIM_Input::Count,           SIM_HandPose::Palm, LR },  \
    { "/input/pinch_ext/pose",    OXR_SourceKind::Pose,   SIM_Input::Count,           SIM_HandPose::Pinch, LR }, \
    { "/input/poke_ext/pose",     OXR_SourceKind::Pose,   SIM_Input::Count,           SIM_HandPose::Poke, LR },  \

// Each table serves the extension profile and its OpenXR 1.1 name; the registry decides which paths
// are available on which
static const OXR_ProfileComponent oxr_touch_components[] = {
    OXR_TOUCH_COMPONENTS
    { "/input/trigger/proximity_fb",           OXR_SourceKind::Bool,   SIM_Input::TriggerProximity, SIM_HandPose::Grip, LR },
    { "/input/thumb_fb/proximity_fb",          OXR_SourceKind::Bool,   SIM_Input::ThumbProximity,   SIM_HandPose::Grip, LR },
    { "/input/trigger/proximity",              OXR_SourceKind::Bool,   SIM_Input::TriggerProximity, SIM_HandPose::Grip, LR },
    { "/input/thumb_resting_surfaces/proximity", OXR_SourceKind::Bool, SIM_Input::ThumbProximity,   SIM_HandPose::Grip, LR },
    { "/output/haptic",                        OXR_SourceKind::Haptic, SIM_Input::Count,            SIM_HandPose::Grip, LR, SIM_HapticOutput::Main },
};

static const OXR_ProfileComponent oxr_touch_pro_components[] = {
    OXR_TOUCH_COMPONENTS
    { "/input/trigger/proximity_fb",           OXR_SourceKind::Bool,   SIM_Input::TriggerProximity, SIM_HandPose::Grip, LR },
    { "/input/thumb_fb/proximity_fb",          OXR_SourceKind::Bool,   SIM_Input::ThumbProximity,   SIM_HandPose::Grip, LR },
    { "/input/trigger/curl_fb",                OXR_SourceKind::Float,  SIM_Input::TriggerCurl,      SIM_HandPose::Grip, LR },
    { "/input/trigger/slide_fb",               OXR_SourceKind::Float,  SIM_Input::TriggerSlide,     SIM_HandPose::Grip, LR },
    { "/input/thumbrest/force",                OXR_SourceKind::Float,  SIM_Input::ThumbrestForce,   SIM_HandPose::Grip, LR },
    { "/input/stylus_fb/force",                OXR_SourceKind::Float,  SIM_Input::StylusForce,      SIM_HandPose::Grip, LR },
    { "/input/trigger/proximity",              OXR_SourceKind::Bool,   SIM_Input::TriggerProximity, SIM_HandPose::Grip, LR },
    { "/input/thumb_resting_surfaces/proximity", OXR_SourceKind::Bool, SIM_Input::ThumbProximity,   SIM_HandPose::Grip, LR },
    { "/input/trigger_curl/value",             OXR_SourceKind::Float,  SIM_Input::TriggerCurl,      SIM_HandPose::Grip, LR },
    { "/input/trigger_curl",                   OXR_SourceKind::Float,  SIM_Input::TriggerCurl,      SIM_HandPose::Grip, LR },
    { "/input/trigger_slide/value",            OXR_SourceKind::Float,  SIM_Input::TriggerSlide,     SIM_HandPose::Grip, LR },
    { "/input/trigger_slide",                  OXR_SourceKind::Float,  SIM_Input::TriggerSlide,     SIM_HandPose::Grip, LR },
    { "/input/stylus/force",                   OXR_SourceKind::Float,  SIM_Input::StylusForce,      SIM_HandPose::Grip, LR },
    { "/output/haptic",                        OXR_SourceKind::Haptic, SIM_Input::Count,            SIM_HandPose::Grip, LR, SIM_HapticOutput::Main },
    { "/output/haptic_trigger_fb",             OXR_SourceKind::Haptic, SIM_Input::Count,            SIM_HandPose::Grip, LR, SIM_HapticOutput::Trigger },
    { "/output/haptic_thumb_fb",               OXR_SourceKind::Haptic, SIM_Input::Count,            SIM_HandPose::Grip, LR, SIM_HapticOutput::Thumb },
    { "/output/haptic_trigger",                OXR_SourceKind::Haptic, SIM_Input::Count,            SIM_HandPose::Grip, LR, SIM_HapticOutput::Trigger },
    { "/output/haptic_thumb",                  OXR_SourceKind::Haptic, SIM_Input::Count,            SIM_HandPose::Grip, LR, SIM_HapticOutput::Thumb },
};

static const OXR_ProfileComponent oxr_touch_plus_components[] = {
    OXR_TOUCH_COMPONENTS
    { "/input/trigger/proximity_meta",         OXR_SourceKind::Bool,   SIM_Input::TriggerProximity, SIM_HandPose::Grip, LR },
    { "/input/thumb_meta/proximity_meta",      OXR_SourceKind::Bool,   SIM_Input::ThumbProximity,   SIM_HandPose::Grip, LR },
    { "/input/trigger/curl_meta",              OXR_SourceKind::Float,  SIM_Input::TriggerCurl,      SIM_HandPose::Grip, LR },
    { "/input/trigger/slide_meta",             OXR_SourceKind::Float,  SIM_Input::TriggerSlide,     SIM_HandPose::Grip, LR },
    { "/input/trigger/force",                  OXR_SourceKind::Float,  SIM_Input::TriggerForce,     SIM_HandPose::Grip, LR },
    { "/input/trigger/proximity",              OXR_SourceKind::Bool,   SIM_Input::TriggerProximity, SIM_HandPose::Grip, LR },
    { "/input/thumb_resting_surfaces/proximity", OXR_SourceKind::Bool, SIM_Input::ThumbProximity,   SIM_HandPose::Grip, LR },
    { "/input/trigger_curl/value",             OXR_SourceKind::Float,  SIM_Input::TriggerCurl,      SIM_HandPose::Grip, LR },
    { "/input/trigger_curl",                   OXR_SourceKind::Float,  SIM_Input::TriggerCurl,      SIM_HandPose::Grip, LR },
    { "/input/trigger_slide/value",            OXR_SourceKind::Float,  SIM_Input::TriggerSlide,     SIM_HandPose::Grip, LR },
    { "/input/trigger_slide",                  OXR_SourceKind::Float,  SIM_Input::TriggerSlide,     SIM_HandPose::Grip, LR },
    { "/output/haptic",                        OXR_SourceKind::Haptic, SIM_Input::Count,            SIM_HandPose::Grip, LR, SIM_HapticOutput::Main },
};

#undef OXR_TOUCH_COMPONENTS

// A tracked hand pinches with trigger/value and grabs with squeeze/value (sim_hand_has_input)
static const OXR_ProfileComponent oxr_hand_interaction_components[] = {
    { "/input/aim/pose",                  OXR_SourceKind::Pose,  SIM_Input::Count,        SIM_HandPose::Aim,   LR },
    { "/input/grip/pose",                 OXR_SourceKind::Pose,  SIM_Input::Count,        SIM_HandPose::Grip,  LR },
    { "/input/grip_surface/pose",         OXR_SourceKind::Pose,  SIM_Input::Count,        SIM_HandPose::Palm,  LR },
    { "/input/palm_ext/pose",             OXR_SourceKind::Pose,  SIM_Input::Count,        SIM_HandPose::Palm,  LR },
    { "/input/pinch_ext/pose",            OXR_SourceKind::Pose,  SIM_Input::Count,        SIM_HandPose::Pinch, LR },
    { "/input/poke_ext/pose",             OXR_SourceKind::Pose,  SIM_Input::Count,        SIM_HandPose::Poke,  LR },
    { "/input/pinch_ext/value",           OXR_SourceKind::Float, SIM_Input::TriggerValue, SIM_HandPose::Grip,  LR },
    { "/input/pinch_ext",                 OXR_SourceKind::Float, SIM_Input::TriggerValue, SIM_HandPose::Grip,  LR },
    { "/input/pinch_ext/ready_ext",       OXR_SourceKind::Ready, SIM_Input::Count,        SIM_HandPose::Grip,  LR },
    { "/input/aim_activate_ext/value",    OXR_SourceKind::Float, SIM_Input::TriggerValue, SIM_HandPose::Grip,  LR },
    { "/input/aim_activate_ext",          OXR_SourceKind::Float, SIM_Input::TriggerValue, SIM_HandPose::Grip,  LR },
    { "/input/aim_activate_ext/ready_ext", OXR_SourceKind::Ready, SIM_Input::Count,       SIM_HandPose::Grip,  LR },
    { "/input/grasp_ext/value",           OXR_SourceKind::Float, SIM_Input::SqueezeValue, SIM_HandPose::Grip,  LR },
    { "/input/grasp_ext",                 OXR_SourceKind::Float, SIM_Input::SqueezeValue, SIM_HandPose::Grip,  LR },
    { "/input/grasp_ext/ready_ext",       OXR_SourceKind::Ready, SIM_Input::Count,        SIM_HandPose::Grip,  LR },
    { "/input/swipe_left_meta/click",     OXR_SourceKind::Bool,  SIM_Input::SwipeLeft,     SIM_HandPose::Grip, LR },
    { "/input/swipe_right_meta/click",    OXR_SourceKind::Bool,  SIM_Input::SwipeRight,    SIM_HandPose::Grip, LR },
    { "/input/swipe_forward_meta/click",  OXR_SourceKind::Bool,  SIM_Input::SwipeForward,  SIM_HandPose::Grip, LR },
    { "/input/swipe_backward_meta/click", OXR_SourceKind::Bool,  SIM_Input::SwipeBackward, SIM_HandPose::Grip, LR },
    { "/input/tap_thumb_meta/click",      OXR_SourceKind::Bool,  SIM_Input::TapThumb,      SIM_HandPose::Grip, LR },
};

// XR_EXT_eye_gaze_interaction: its one pose, on /user/eyes_ext (side bit 4)
static const OXR_ProfileComponent oxr_eye_gaze_components[] = {
    { "/input/gaze_ext/pose", OXR_SourceKind::Pose, SIM_Input::Count, SIM_HandPose::Grip, 4 },
    { "/input/gaze_ext",      OXR_SourceKind::Pose, SIM_Input::Count, SIM_HandPose::Grip, 4 },
};

#undef L
#undef R
#undef LR

#include "oxr_profiles.inc"

static u64 spec_features (XrVersion api_version, const char* const* extensions, u32 extensions_count)
{
    u64 features = 1; // XR_VERSION_1_0
    if (XR_VERSION_MINOR(api_version) >= 1)
    {
        features |= 2;
    }
    for (u32 i = 0; i < extensions_count; i++)
    {
        for (u32 f = 0; f < OXR_SPEC_FEATURES_COUNT; f++)
        {
            if (cstr_equal(extensions[i], oxr_spec_features[f]))
            {
                features |= (u64)1 << f;
            }
        }
    }
    return features;
}

static b8 spec_available (u64 features, u16 avail)
{
    const OXR_SpecAvail& terms = oxr_spec_avails[avail];
    for (u32 i = terms.first; i < (u32)terms.first + terms.count; i++)
    {
        if ((oxr_spec_terms[i] & features) == oxr_spec_terms[i])
        {
            return true;
        }
    }
    return false;
}

static const OXR_SpecProfile* spec_profile (const OXR_Instance* inst, const char* path)
{
    for (u32 i = 0; i < OXR_SPEC_PROFILES_COUNT; i++)
    {
        if (cstr_equal(path, oxr_spec_profiles[i].path))
        {
            return spec_available(inst->spec_features, oxr_spec_profiles[i].avail) ? &oxr_spec_profiles[i] : nullptr;
        }
    }
    return nullptr;
}

// A binding names an input or output of the profile, or the parent of one such as
// .../input/trigger for .../input/trigger/value (spec 6.3.2, suggested bindings)
static b8 spec_binding_valid (const OXR_Instance* inst, const OXR_SpecProfile* profile, const char* path)
{
    const usize length = cstr_length(path);
    const b8    parent = cstr_find(path, "/input/") || cstr_find(path, "/output/");
    for (u32 i = profile->first; i < (u32)profile->first + profile->count; i++)
    {
        const OXR_SpecBinding& binding = oxr_spec_bindings[i];
        if (!spec_available(inst->spec_features, binding.avail))
        {
            continue;
        }
        if (cstr_equal(binding.path, path))
        {
            return true;
        }
        if (parent && cstr_starts_with(binding.path, path) && binding.path[length] == '/' && !cstr_find(binding.path + length + 1, "/"))
        {
            return true;
        }
    }
    return false;
}

// The interaction profiles each simulated controller supports, preferred first. Every Touch
// generation also serves the profiles of the ones before it and the simple profile, as Quest
// runtimes do. A tracked hand serves the hand interaction profile and the simple one.
static u32 hardware_profiles (SIM_Controller hardware, SIM_HandSource source, OXR_Profile* out_profiles)
{
    if (source == SIM_HandSource::Hand)
    {
        out_profiles[0] = OXR_Profile::HandInteraction;
        out_profiles[1] = OXR_Profile::Simple;
        return 2;
    }
    switch (hardware)
    {
    case SIM_Controller::OculusTouch:
        out_profiles[0] = OXR_Profile::MetaQuest2;
        out_profiles[1] = OXR_Profile::Touch;
        out_profiles[2] = OXR_Profile::Simple;
        return 3;
    case SIM_Controller::KhrSimple:
        out_profiles[0] = OXR_Profile::Simple;
        return 1;
    case SIM_Controller::MetaTouchPro:
        out_profiles[0] = OXR_Profile::MetaTouchPro;
        out_profiles[1] = OXR_Profile::TouchPro;
        out_profiles[2] = OXR_Profile::MetaQuest2;
        out_profiles[3] = OXR_Profile::Touch;
        out_profiles[4] = OXR_Profile::Simple;
        return 5;
    case SIM_Controller::MetaTouchPlus:
        out_profiles[0] = OXR_Profile::MetaTouchPlus;
        out_profiles[1] = OXR_Profile::TouchPlus;
        out_profiles[2] = OXR_Profile::MetaQuest2;
        out_profiles[3] = OXR_Profile::Touch;
        out_profiles[4] = OXR_Profile::Simple;
        return 5;
    default:
        return 0;
    }
}

// The hardware a profile is native to, for XR_EXT_conformance_automation
static SIM_Controller profile_hardware (OXR_Profile profile)
{
    switch (profile)
    {
    case OXR_Profile::Simple:
        return SIM_Controller::KhrSimple;
    case OXR_Profile::TouchPro:
    case OXR_Profile::MetaTouchPro:
        return SIM_Controller::MetaTouchPro;
    case OXR_Profile::TouchPlus:
    case OXR_Profile::MetaTouchPlus:
        return SIM_Controller::MetaTouchPlus;
    case OXR_Profile::Touch:
    case OXR_Profile::MetaQuest2:
    case OXR_Profile::HandInteraction:
    default:
        return SIM_Controller::OculusTouch;
    }
}

static b8 hardware_serves (SIM_Controller hardware, SIM_HandSource source, OXR_Profile profile)
{
    OXR_Profile order[(u32)OXR_Profile::Count];
    const u32   count = hardware_profiles(hardware, source, order);
    for (u32 i = 0; i < count; i++)
    {
        if (order[i] == profile)
        {
            return true;
        }
    }
    return false;
}

static void profile_components (OXR_Profile profile, SIM_Controller hardware, SIM_HandSource source, const OXR_ProfileComponent*& out_components, u32& out_count)
{
    out_components = nullptr;
    out_count      = 0;
    if (profile == OXR_Profile::EyeGaze)
    {
        out_components = oxr_eye_gaze_components;
        out_count      = ARRAY_SIZE(oxr_eye_gaze_components);
        return;
    }
    if (!hardware_serves(hardware, source, profile))
    {
        return;
    }
    const b8 simple_hardware = hardware == SIM_Controller::KhrSimple && source == SIM_HandSource::Controller;
    switch (profile)
    {
    case OXR_Profile::Simple:
        out_components = simple_hardware ? oxr_simple_on_simple_components : oxr_simple_components;
        out_count      = simple_hardware ? ARRAY_SIZE(oxr_simple_on_simple_components) : ARRAY_SIZE(oxr_simple_components);
        return;
    case OXR_Profile::HandInteraction:
        out_components = oxr_hand_interaction_components;
        out_count      = ARRAY_SIZE(oxr_hand_interaction_components);
        return;
    case OXR_Profile::Touch:
    case OXR_Profile::MetaQuest2:
        out_components = oxr_touch_components;
        out_count      = ARRAY_SIZE(oxr_touch_components);
        return;
    case OXR_Profile::TouchPro:
    case OXR_Profile::MetaTouchPro:
        out_components = oxr_touch_pro_components;
        out_count      = ARRAY_SIZE(oxr_touch_pro_components);
        return;
    case OXR_Profile::TouchPlus:
    case OXR_Profile::MetaTouchPlus:
        out_components = oxr_touch_plus_components;
        out_count      = ARRAY_SIZE(oxr_touch_plus_components);
        return;
    default:
        return;
    }
}

// Handles

static OXR_Runtime* runtime ()
{
    if (!g_runtime)
    {
        g_runtime        = (OXR_Runtime*)os_memory_alloc(sizeof(OXR_Runtime));
        g_runtime->mutex = os_mutex_create();
    }
    return g_runtime;
}

static u64 encode_handle (OXR_HandleType type, u64 value)
{
    return ((u64)type << 56) | value;
}

template <typename T>
static Handle<T> decode_handle (OXR_HandleType type, u64 value)
{
    if ((value >> 56) != (u64)type)
    {
        return {};
    }
    return Handle<T>(value & 0x00FFFFFFFFFFFFFFull);
}

OXR_Instance*  oxr_find_instance  (XrInstance instance)  { return runtime()->instances.try_get(decode_handle<OXR_Instance>(OXR_HandleType::Instance, (u64)(uintptr_t)instance)); }
OXR_Session*   oxr_find_session   (XrSession session)   { return runtime()->sessions.try_get(decode_handle<OXR_Session>(OXR_HandleType::Session, (u64)(uintptr_t)session)); }
OXR_Space*     oxr_find_space     (XrSpace space)     { return runtime()->spaces.try_get(decode_handle<OXR_Space>(OXR_HandleType::Space, (u64)(uintptr_t)space)); }
OXR_Swapchain* oxr_find_swapchain (XrSwapchain swapchain) { return runtime()->swapchains.try_get(decode_handle<OXR_Swapchain>(OXR_HandleType::Swapchain, (u64)(uintptr_t)swapchain)); }
OXR_ActionSet* oxr_find_action_set(XrActionSet action_set) { return runtime()->action_sets.try_get(decode_handle<OXR_ActionSet>(OXR_HandleType::ActionSet, (u64)(uintptr_t)action_set)); }
OXR_Action*    oxr_find_action    (XrAction action)    { OXR_Action* a = runtime()->actions.try_get(decode_handle<OXR_Action>(OXR_HandleType::Action, (u64)(uintptr_t)action)); return a && !a->destroyed ? a : nullptr; }
OXR_Future*    oxr_find_future    (XrFutureEXT f) { return runtime()->futures.try_get(decode_handle<OXR_Future>(OXR_HandleType::Future, (u64)(uintptr_t)f)); }
OXR_Passthrough*      oxr_find_passthrough       (XrPassthroughFB h)      { return runtime()->passthroughs.try_get(decode_handle<OXR_Passthrough>(OXR_HandleType::Passthrough, (u64)(uintptr_t)h)); }
OXR_PassthroughLayer* oxr_find_passthrough_layer (XrPassthroughLayerFB layer) { return runtime()->passthrough_layers.try_get(decode_handle<OXR_PassthroughLayer>(OXR_HandleType::PassthroughLayer, (u64)(uintptr_t)layer)); }
OXR_DepthProvider*    oxr_find_depth_provider    (XrEnvironmentDepthProviderMETA provider)  { return runtime()->depth_providers.try_get(decode_handle<OXR_DepthProvider>(OXR_HandleType::DepthProvider, (u64)(uintptr_t)provider)); }
OXR_DepthSwapchain*   oxr_find_depth_swapchain   (XrEnvironmentDepthSwapchainMETA swapchain) { return runtime()->depth_swapchains.try_get(decode_handle<OXR_DepthSwapchain>(OXR_HandleType::DepthSwapchain, (u64)(uintptr_t)swapchain)); }
OXR_Raycaster*        oxr_find_raycaster         (XrEnvironmentRaycasterMETA raycaster)      { return runtime()->raycasters.try_get(decode_handle<OXR_Raycaster>(OXR_HandleType::Raycaster, (u64)(uintptr_t)raycaster)); }
OXR_SpaceUser*        oxr_find_space_user        (XrSpaceUserFB user)                         { return runtime()->space_users.try_get(decode_handle<OXR_SpaceUser>(OXR_HandleType::SpaceUser, (u64)(uintptr_t)user)); }

static OXR_InstanceHandle  instance_handle   (XrInstance h)  { return decode_handle<OXR_Instance>(OXR_HandleType::Instance, (u64)(uintptr_t)h); }
static OXR_SessionHandle   session_handle    (XrSession h)   { return decode_handle<OXR_Session>(OXR_HandleType::Session, (u64)(uintptr_t)h); }
static OXR_ActionSetHandle action_set_handle (XrActionSet h) { return decode_handle<OXR_ActionSet>(OXR_HandleType::ActionSet, (u64)(uintptr_t)h); }
static OXR_ActionHandle    action_handle     (XrAction h)    { return decode_handle<OXR_Action>(OXR_HandleType::Action, (u64)(uintptr_t)h); }

static XrSession   session_xr   (OXR_SessionHandle h)   { return (XrSession)(uintptr_t)encode_handle(OXR_HandleType::Session, h.value); }
static XrInstance  instance_xr  (OXR_InstanceHandle h)  { return (XrInstance)(uintptr_t)encode_handle(OXR_HandleType::Instance, h.value); }
static XrSpace     space_xr     (OXR_SpaceHandle h)     { return (XrSpace)(uintptr_t)encode_handle(OXR_HandleType::Space, h.value); }
static XrActionSet action_set_xr(OXR_ActionSetHandle h) { return (XrActionSet)(uintptr_t)encode_handle(OXR_HandleType::ActionSet, h.value); }
static XrAction    action_xr    (OXR_ActionHandle h)    { return (XrAction)(uintptr_t)encode_handle(OXR_HandleType::Action, h.value); }
static OXR_FutureHandle future_handle (XrFutureEXT f)   { return decode_handle<OXR_Future>(OXR_HandleType::Future, (u64)(uintptr_t)f); }
static OXR_PassthroughHandle      passthrough_handle       (XrPassthroughFB h)      { return decode_handle<OXR_Passthrough>(OXR_HandleType::Passthrough, (u64)(uintptr_t)h); }
static OXR_PassthroughLayerHandle passthrough_layer_handle (XrPassthroughLayerFB h) { return decode_handle<OXR_PassthroughLayer>(OXR_HandleType::PassthroughLayer, (u64)(uintptr_t)h); }
static XrPassthroughFB            passthrough_xr           (OXR_PassthroughHandle h)      { return (XrPassthroughFB)(uintptr_t)encode_handle(OXR_HandleType::Passthrough, h.value); }
static XrPassthroughLayerFB       passthrough_layer_xr     (OXR_PassthroughLayerHandle h) { return (XrPassthroughLayerFB)(uintptr_t)encode_handle(OXR_HandleType::PassthroughLayer, h.value); }
static OXR_DepthProviderHandle         depth_provider_handle  (XrEnvironmentDepthProviderMETA h)  { return decode_handle<OXR_DepthProvider>(OXR_HandleType::DepthProvider, (u64)(uintptr_t)h); }
static OXR_DepthSwapchainHandle        depth_swapchain_handle (XrEnvironmentDepthSwapchainMETA h) { return decode_handle<OXR_DepthSwapchain>(OXR_HandleType::DepthSwapchain, (u64)(uintptr_t)h); }
static OXR_RaycasterHandle             raycaster_handle       (XrEnvironmentRaycasterMETA h)      { return decode_handle<OXR_Raycaster>(OXR_HandleType::Raycaster, (u64)(uintptr_t)h); }
static XrEnvironmentDepthProviderMETA  depth_provider_xr      (OXR_DepthProviderHandle h)  { return (XrEnvironmentDepthProviderMETA)(uintptr_t)encode_handle(OXR_HandleType::DepthProvider, h.value); }
static XrEnvironmentDepthSwapchainMETA depth_swapchain_xr     (OXR_DepthSwapchainHandle h) { return (XrEnvironmentDepthSwapchainMETA)(uintptr_t)encode_handle(OXR_HandleType::DepthSwapchain, h.value); }
static XrEnvironmentRaycasterMETA      raycaster_xr           (OXR_RaycasterHandle h)      { return (XrEnvironmentRaycasterMETA)(uintptr_t)encode_handle(OXR_HandleType::Raycaster, h.value); }
static OXR_SpaceUserHandle             space_user_handle      (XrSpaceUserFB h)                   { return decode_handle<OXR_SpaceUser>(OXR_HandleType::SpaceUser, (u64)(uintptr_t)h); }
static XrSpaceUserFB                   space_user_xr          (OXR_SpaceUserHandle h)      { return (XrSpaceUserFB)(uintptr_t)encode_handle(OXR_HandleType::SpaceUser, h.value); }
static XrFutureEXT                     future_xr              (OXR_FutureHandle h)         { return (XrFutureEXT)(uintptr_t)encode_handle(OXR_HandleType::Future, h.value); }
static OXR_TriangleMeshHandle          triangle_mesh_handle   (XrTriangleMeshFB h)         { return decode_handle<OXR_TriangleMesh>(OXR_HandleType::TriangleMesh, (u64)(uintptr_t)h); }
static XrTriangleMeshFB                triangle_mesh_xr       (OXR_TriangleMeshHandle h)   { return (XrTriangleMeshFB)(uintptr_t)encode_handle(OXR_HandleType::TriangleMesh, h.value); }
static OXR_HandTrackerHandle           hand_tracker_handle    (XrHandTrackerEXT h)         { return decode_handle<OXR_HandTracker>(OXR_HandleType::HandTracker, (u64)(uintptr_t)h); }
static XrHandTrackerEXT                hand_tracker_xr        (OXR_HandTrackerHandle h)    { return (XrHandTrackerEXT)(uintptr_t)encode_handle(OXR_HandleType::HandTracker, h.value); }
static OXR_BodyTrackerHandle           body_tracker_handle    (XrBodyTrackerFB h)          { return decode_handle<OXR_BodyTracker>(OXR_HandleType::BodyTracker, (u64)(uintptr_t)h); }
static XrBodyTrackerFB                 body_tracker_xr        (OXR_BodyTrackerHandle h)    { return (XrBodyTrackerFB)(uintptr_t)encode_handle(OXR_HandleType::BodyTracker, h.value); }
static OXR_FaceTrackerHandle           face_tracker_handle    (XrFaceTrackerFB h)          { return decode_handle<OXR_FaceTracker>(OXR_HandleType::FaceTracker, (u64)(uintptr_t)h); }
static XrFaceTrackerFB                 face_tracker_xr        (OXR_FaceTrackerHandle h)    { return (XrFaceTrackerFB)(uintptr_t)encode_handle(OXR_HandleType::FaceTracker, h.value); }
static OXR_FaceTrackerHandle           face_tracker2_handle   (XrFaceTracker2FB h)         { return decode_handle<OXR_FaceTracker>(OXR_HandleType::FaceTracker2, (u64)(uintptr_t)h); }
static XrFaceTracker2FB                face_tracker2_xr       (OXR_FaceTrackerHandle h)    { return (XrFaceTracker2FB)(uintptr_t)encode_handle(OXR_HandleType::FaceTracker2, h.value); }
static OXR_EyeTrackerHandle            eye_tracker_handle     (XrEyeTrackerFB h)           { return decode_handle<OXR_EyeTracker>(OXR_HandleType::EyeTracker, (u64)(uintptr_t)h); }
static XrEyeTrackerFB                  eye_tracker_xr         (OXR_EyeTrackerHandle h)     { return (XrEyeTrackerFB)(uintptr_t)encode_handle(OXR_HandleType::EyeTracker, h.value); }
static OXR_VirtualKeyboardHandle       keyboard_handle        (XrVirtualKeyboardMETA h)    { return decode_handle<OXR_VirtualKeyboard>(OXR_HandleType::VirtualKeyboard, (u64)(uintptr_t)h); }
static OXR_DynamicObjectTrackerHandle  dynamic_tracker_handle (u64 h)                      { return decode_handle<OXR_DynamicObjectTracker>(OXR_HandleType::DynamicObjectTracker, h); }
static u64                             dynamic_tracker_xr     (OXR_DynamicObjectTrackerHandle h) { return encode_handle(OXR_HandleType::DynamicObjectTracker, h.value); }
static OXR_SpatialContextHandle        spatial_context_handle  (XrSpatialContextEXT h)       { return decode_handle<OXR_SpatialContext>(OXR_HandleType::SpatialContext, (u64)(uintptr_t)h); }
static OXR_SpatialEntityHandle         spatial_entity_handle   (XrSpatialEntityEXT h)        { return decode_handle<OXR_SpatialEntity>(OXR_HandleType::SpatialEntity, (u64)(uintptr_t)h); }
static OXR_SpatialSnapshotHandle       spatial_snapshot_handle (XrSpatialSnapshotEXT h)      { return decode_handle<OXR_SpatialSnapshot>(OXR_HandleType::SpatialSnapshot, (u64)(uintptr_t)h); }
static u64                             spatial_context_xr      (OXR_SpatialContextHandle h)  { return encode_handle(OXR_HandleType::SpatialContext, h.value); }
static u64                             spatial_entity_xr       (OXR_SpatialEntityHandle h)   { return encode_handle(OXR_HandleType::SpatialEntity, h.value); }
static u64                             spatial_snapshot_xr     (OXR_SpatialSnapshotHandle h) { return encode_handle(OXR_HandleType::SpatialSnapshot, h.value); }
static OXR_RenderModelHandle           render_model_handle       (XrRenderModelEXT h)           { return decode_handle<OXR_RenderModel>(OXR_HandleType::RenderModel, (u64)(uintptr_t)h); }
static OXR_RenderModelAssetHandle      render_model_asset_handle (XrRenderModelAssetEXT h)      { return decode_handle<OXR_RenderModelAsset>(OXR_HandleType::RenderModelAsset, (u64)(uintptr_t)h); }
static u64                             render_model_xr           (OXR_RenderModelHandle h)      { return encode_handle(OXR_HandleType::RenderModel, h.value); }
static u64                             render_model_asset_xr     (OXR_RenderModelAssetHandle h) { return encode_handle(OXR_HandleType::RenderModelAsset, h.value); }
static OXR_SpatialContext*             find_spatial_context    (XrSpatialContextEXT h)       { return runtime()->spatial_contexts.try_get(spatial_context_handle(h)); }
static OXR_SpatialEntity*              find_spatial_entity     (XrSpatialEntityEXT h)        { return runtime()->spatial_entities.try_get(spatial_entity_handle(h)); }
static OXR_SpatialSnapshot*            find_spatial_snapshot   (XrSpatialSnapshotEXT h)      { return runtime()->spatial_snapshots.try_get(spatial_snapshot_handle(h)); }
static OXR_EventChannelHandle          event_channel_handle    (u64 h)                       { return decode_handle<OXR_EventChannel>(OXR_HandleType::EventChannel, h); }
static u64                             event_channel_xr        (OXR_EventChannelHandle h)    { return encode_handle(OXR_HandleType::EventChannel, h.value); }
static OXR_FoveationProfileHandle      foveation_profile_handle (XrFoveationProfileFB h)    { return decode_handle<OXR_FoveationProfile>(OXR_HandleType::FoveationProfile, (u64)(uintptr_t)h); }
static XrFoveationProfileFB            foveation_profile_xr     (OXR_FoveationProfileHandle h) { return (XrFoveationProfileFB)(uintptr_t)encode_handle(OXR_HandleType::FoveationProfile, h.value); }
static OXR_PassthroughColorLutHandle   color_lut_handle         (XrPassthroughColorLutMETA h) { return decode_handle<OXR_PassthroughColorLut>(OXR_HandleType::PassthroughColorLut, (u64)(uintptr_t)h); }
static XrPassthroughColorLutMETA       color_lut_xr             (OXR_PassthroughColorLutHandle h) { return (XrPassthroughColorLutMETA)(uintptr_t)encode_handle(OXR_HandleType::PassthroughColorLut, h.value); }
static XrVirtualKeyboardMETA           keyboard_xr            (OXR_VirtualKeyboardHandle h) { return (XrVirtualKeyboardMETA)(uintptr_t)encode_handle(OXR_HandleType::VirtualKeyboard, h.value); }
static OXR_GeometryInstanceHandle      geometry_handle        (XrGeometryInstanceFB h)     { return decode_handle<OXR_GeometryInstance>(OXR_HandleType::GeometryInstance, (u64)(uintptr_t)h); }
static XrGeometryInstanceFB            geometry_xr            (OXR_GeometryInstanceHandle h) { return (XrGeometryInstanceFB)(uintptr_t)encode_handle(OXR_HandleType::GeometryInstance, h.value); }
static OXR_CameraProviderHandle        camera_provider_handle (u64 h)                      { return decode_handle<OXR_CameraProvider>(OXR_HandleType::CameraProvider, h); }
static u64                             camera_provider_xr     (OXR_CameraProviderHandle h) { return encode_handle(OXR_HandleType::CameraProvider, h.value); }

static OXR_Instance* session_instance (const OXR_Session* session)
{
    return runtime()->instances.try_get(session->instance);
}

#if !PUPPETXR_HEADLESS_ONLY
// The panels get no input while the window holds the mouse
static void ui_raw_event_hook (const void* native_event, b8 input, void* user)
{
    if (!input || !runtime()->mouse_captured)
    {
        ui_handle_raw_event((UI_Context*)user, native_event);
    }
}
#endif

static OXR_SessionHandle session_handle_of (const OXR_Session* session)
{
    const OXR_Runtime* rt    = runtime();
    const usize        index = (usize)(session - rt->sessions.data());
    return index < rt->sessions.length() ? rt->sessions.handle_at(index) : OXR_SessionHandle{};
}

// Two-call idiom

XrResult oxr_two_call (u32 capacity, u32* count_output, const void* output, u32 count)
{
    OXR_CHECK(count_output);
    *count_output = count;
    if (capacity == 0)
    {
        return XR_SUCCESS;
    }
    OXR_CHECK(output);
    if (capacity < count)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    return XR_SUCCESS;
}

// Paths

static b8 path_string_valid (const char* str)
{
    const usize length = cstr_length(str);
    if (length < 2 || length >= XR_MAX_PATH_LENGTH || str[0] != '/' || str[length - 1] == '/')
    {
        return false;
    }
    b8 component_only_periods = true;
    for (usize i = 1; i < length; i++)
    {
        const char c = str[i];
        const b8   ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/';
        if (!ok || (c == '/' && str[i - 1] == '/'))
        {
            return false;
        }
        // Spec 6.2: no component may consist of periods only, like "." or ".."
        if (c == '/')
        {
            if (component_only_periods)
            {
                return false;
            }
            component_only_periods = true;
        }
        else if (c != '.')
        {
            component_only_periods = false;
        }
    }
    return !component_only_periods;
}

static const char* path_string (const OXR_Instance* instance, XrPath path)
{
    if (path == XR_NULL_PATH || path > instance->path_offsets.length)
    {
        return nullptr;
    }
    return instance->path_chars.data + instance->path_offsets[(usize)path - 1];
}

static XrPath path_find (const OXR_Instance* instance, const char* str)
{
    const u64  h     = hash_cstr(str);
    const u32* found = instance->path_lookup.at(h ? h : 1);
    if (found && cstr_equal(path_string(instance, *found), str))
    {
        return *found;
    }
    for (usize i = 0; i < instance->path_offsets.length; i++)
    {
        if (cstr_equal(instance->path_chars.data + instance->path_offsets[i], str))
        {
            return (XrPath)(i + 1);
        }
    }
    return XR_NULL_PATH;
}

static XrPath path_intern (OXR_Instance* instance, const char* str)
{
    const XrPath existing = path_find(instance, str);
    if (existing != XR_NULL_PATH)
    {
        return existing;
    }

    const u32 offset = (u32)instance->path_chars.length;
    instance->path_chars.add(str, cstr_length(str) + 1);
    instance->path_offsets.add(offset);

    const XrPath path = (XrPath)instance->path_offsets.length;
    const u64    h    = hash_cstr(str);
    if (!instance->path_lookup.contains(h ? h : 1))
    {
        instance->path_lookup.insert(h ? h : 1, (u32)path);
    }
    return path;
}

static SIM_Device side_device (OXR_TopLevel side)
{
    return side == OXR_TopLevel::LeftHand || side == OXR_TopLevel::DetachedLeft ? SIM_Device::LeftHand : SIM_Device::RightHand;
}

static b8 side_is_hand (OXR_TopLevel side)
{
    return side == OXR_TopLevel::LeftHand || side == OXR_TopLevel::RightHand;
}

static b8 side_is_detached (OXR_TopLevel side)
{
    return side == OXR_TopLevel::DetachedLeft || side == OXR_TopLevel::DetachedRight;
}

// A side backed by a simulated hand: the hand itself, or the controller it put down
static b8 side_has_device (OXR_TopLevel side)
{
    return side_is_hand(side) || side_is_detached(side);
}

// A side an action can be bound on: the hands, their put down controllers and the eyes
static b8 side_has_profile (OXR_TopLevel side)
{
    return side_has_device(side) || side == OXR_TopLevel::Eyes;
}

static b8 path_top_level (const OXR_Instance* instance, XrPath path, OXR_TopLevel& out_top)
{
    const char* str = path_string(instance, path);
    for (u32 i = 0; str && i < (u32)OXR_TopLevel::Count; i++)
    {
        if (cstr_equal(str, oxr_top_level_paths[i]))
        {
            const b8 detached = i == (u32)OXR_TopLevel::DetachedLeft || i == (u32)OXR_TopLevel::DetachedRight;
            out_top           = (OXR_TopLevel)i;
            if (out_top == OXR_TopLevel::Eyes)
            {
                return instance->ext.EXT_eye_gaze_interaction;
            }
            return !detached || instance->ext.META_detached_controllers;
        }
    }
    return false;
}

static b8 path_profile (const OXR_Instance* instance, XrPath path, OXR_Profile& out_profile)
{
    const char* str = path_string(instance, path);
    for (u32 i = 0; str && i < (u32)OXR_Profile::Count; i++)
    {
        if (cstr_equal(str, oxr_profile_paths[i]))
        {
            out_profile = (OXR_Profile)i;
            return true;
        }
    }
    return false;
}

// Splits "/user/hand/left/input/trigger/value" into the side and the profile component
static const OXR_ProfileComponent* resolve_binding_str (const OXR_Instance* instance, OXR_Profile profile, SIM_Controller hardware, const SIM_HandSource (&sources)[2], const char* str, OXR_TopLevel& out_side)
{
    if (!str)
    {
        return nullptr;
    }

    u8 side_bit = 0;
    if (cstr_starts_with(str, "/user/hand/left/"))
    {
        out_side = OXR_TopLevel::LeftHand;
        side_bit = 1;
        str += cstr_length("/user/hand/left");
    }
    else if (cstr_starts_with(str, "/user/hand/right/"))
    {
        out_side = OXR_TopLevel::RightHand;
        side_bit = 2;
        str += cstr_length("/user/hand/right");
    }
    else if (cstr_starts_with(str, "/user/detached_controller_meta/left/") && instance->ext.META_detached_controllers)
    {
        out_side = OXR_TopLevel::DetachedLeft;
        side_bit = 1;
        str += cstr_length("/user/detached_controller_meta/left");
    }
    else if (cstr_starts_with(str, "/user/detached_controller_meta/right/") && instance->ext.META_detached_controllers)
    {
        out_side = OXR_TopLevel::DetachedRight;
        side_bit = 2;
        str += cstr_length("/user/detached_controller_meta/right");
    }
    else if (cstr_starts_with(str, "/user/eyes_ext/") && instance->ext.EXT_eye_gaze_interaction && profile == OXR_Profile::EyeGaze)
    {
        out_side = OXR_TopLevel::Eyes;
        side_bit = 4;
        str += cstr_length("/user/eyes_ext");
    }
    else
    {
        return nullptr;
    }

    // A put down controller is a controller, whatever tracks its hand
    const SIM_HandSource        source     = side_is_detached(out_side) || side_bit == 4 ? SIM_HandSource::Controller : sources[side_bit - 1];
    const OXR_ProfileComponent* components = nullptr;
    u32                         count      = 0;
    profile_components(profile, hardware, source, components, count);
    for (u32 i = 0; i < count; i++)
    {
        if ((components[i].sides & side_bit) && cstr_equal(components[i].path, str))
        {
            return &components[i];
        }
    }
    return nullptr;
}

static const OXR_ProfileComponent* resolve_binding (const OXR_Instance* instance, OXR_Profile profile, SIM_Controller hardware, const SIM_HandSource (&sources)[2], XrPath path, OXR_TopLevel& out_side)
{
    return resolve_binding_str(instance, profile, hardware, sources, path_string(instance, path), out_side);
}

// XR_EXT_dpad_binding: ".../input/thumbstick/dpad_up" is the thumbstick and a direction
static u8 dpad_direction (const char* path, usize& out_prefix_length)
{
    static const struct
    {
        const char* suffix;
        u8          direction;
    } directions[] = { { "/dpad_up", OXR_DPAD_UP }, { "/dpad_down", OXR_DPAD_DOWN }, { "/dpad_left", OXR_DPAD_LEFT }, { "/dpad_right", OXR_DPAD_RIGHT } };
    const usize length = cstr_length(path);
    for (const auto& entry : directions)
    {
        const usize suffix = cstr_length(entry.suffix);
        if (length > suffix && cstr_equal(path + length - suffix, entry.suffix))
        {
            out_prefix_length = length - suffix;
            return entry.direction;
        }
    }
    return 0;
}

static const OXR_ProfileComponent* resolve_dpad_binding (const OXR_Instance* instance, OXR_Profile profile, SIM_Controller hardware, const SIM_HandSource (&sources)[2], XrPath path, OXR_TopLevel& out_side, u8& out_direction, char (&out_input)[XR_MAX_PATH_LENGTH])
{
    const char* str    = path_string(instance, path);
    usize       prefix = 0;
    out_direction      = str && instance->ext.EXT_dpad_binding ? dpad_direction(str, prefix) : 0;
    if (!out_direction)
    {
        return nullptr;
    }
    cstr_copy(out_input, prefix + 1, str);
    const OXR_ProfileComponent* component = resolve_binding_str(instance, profile, hardware, sources, out_input, out_side);
    return component && component->kind == OXR_SourceKind::Vec2 ? component : nullptr;
}

static b8 source_compatible (XrActionType type, OXR_SourceKind kind)
{
    switch (type)
    {
    case XR_ACTION_TYPE_BOOLEAN_INPUT:
    case XR_ACTION_TYPE_FLOAT_INPUT:
        return kind == OXR_SourceKind::Bool || kind == OXR_SourceKind::FloatAsBool || kind == OXR_SourceKind::Float || kind == OXR_SourceKind::Vec2X || kind == OXR_SourceKind::Vec2Y || kind == OXR_SourceKind::Ready;
    case XR_ACTION_TYPE_VECTOR2F_INPUT:
        return kind == OXR_SourceKind::Vec2;
    case XR_ACTION_TYPE_POSE_INPUT:
        return kind == OXR_SourceKind::Pose;
    case XR_ACTION_TYPE_VIBRATION_OUTPUT:
        return kind == OXR_SourceKind::Haptic;
    default:
        return false;
    }
}

static b8 name_valid (const char* name, usize max_size)
{
    const usize length = cstr_length(name);
    if (length == 0 || length >= max_size)
    {
        return false;
    }
    for (usize i = 0; i < length; i++)
    {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
        {
            return false;
        }
    }
    return true;
}

// Events

// XR_EXTX1_event_channel: one queue holds the events of every channel, each tagged with the
// channel it went to when it was queued: its session's target, else the instance's default
static void push_event (OXR_Instance* instance, const OXR_Event& event)
{
    if (instance->events_count == OXR_MAX_EVENTS)
    {
        LOG_WARNING("Event queue full, dropping the oldest event");
        instance->events_head = (instance->events_head + 1) % OXR_MAX_EVENTS;
        instance->events_count--;
        instance->events_lost++;
    }
    const OXR_Session* sess = event.session != XR_NULL_HANDLE ? oxr_find_session(event.session) : nullptr;
    OXR_Event&         slot = instance->events[(instance->events_head + instance->events_count) % OXR_MAX_EVENTS];
    slot                    = event;
    slot.channel            = sess && sess->event_channel ? sess->event_channel : instance->default_channel;
    instance->events_count++;
}

// Removes the event at `index` in queue order, keeping the order of the others
static void remove_event (OXR_Instance* instance, u32 index)
{
    ASSERT(index < instance->events_count);
    for (u32 i = index; i + 1 < instance->events_count; i++)
    {
        instance->events[(instance->events_head + i) % OXR_MAX_EVENTS] = instance->events[(instance->events_head + i + 1) % OXR_MAX_EVENTS];
    }
    instance->events_count--;
}

// The index in queue order of the first event waiting in `channel`, or events_count
static u32 find_event (const OXR_Instance* instance, u64 channel)
{
    u32 i = 0;
    while (i < instance->events_count && instance->events[(instance->events_head + i) % OXR_MAX_EVENTS].channel != channel)
    {
        i++;
    }
    return i;
}

static OXR_EventChannel* find_event_channel (u64 channel, OXR_InstanceHandle instance)
{
    OXR_EventChannel* found = channel ? runtime()->event_channels.try_get(event_channel_handle(channel)) : nullptr;
    return found && found->instance == instance ? found : nullptr;
}

static void destroy_event_channel (OXR_EventChannelHandle handle)
{
    OXR_Runtime*            rt      = runtime();
    const OXR_EventChannel* channel = rt->event_channels.try_get(handle);
    OXR_Instance*           inst    = channel ? rt->instances.try_get(channel->instance) : nullptr;
    const u64               xr      = event_channel_xr(handle);
    if (inst)
    {
        for (u32 index = find_event(inst, xr); index < inst->events_count; index = find_event(inst, xr))
        {
            remove_event(inst, index);
        }
        inst->default_channel = inst->default_channel == xr ? 0 : inst->default_channel;
    }
    for (usize i = 0; i < rt->sessions.length(); i++)
    {
        OXR_Session& sess  = rt->sessions.data()[i];
        sess.event_channel = sess.event_channel == xr ? 0 : sess.event_channel;
    }
    rt->event_channels.remove(handle);
}

// Anchor completions arrive anchor_latency_frames after the request, at an xrWaitFrame
static void push_anchor_event (OXR_Instance* instance, const OXR_Event& event)
{
    const OXR_Runtime* rt = runtime();
    if (rt->anchor_latency_frames == 0 || !rt->sim)
    {
        push_event(instance, event);
        return;
    }
    if (instance->delayed_count == OXR_MAX_EVENTS)
    {
        LOG_WARNING("Too many delayed anchor events, delivering one early");
        push_event(instance, instance->delayed[0]);
        memory_move(instance->delayed, instance->delayed + 1, (OXR_MAX_EVENTS - 1) * sizeof(OXR_Event));
        memory_move(instance->delayed_frames, instance->delayed_frames + 1, (OXR_MAX_EVENTS - 1) * sizeof(u64));
        instance->delayed_count--;
    }
    instance->delayed[instance->delayed_count]        = event;
    instance->delayed_frames[instance->delayed_count] = sim_frame_index(rt->sim) + rt->anchor_latency_frames;
    instance->delayed_count++;
}

static void release_delayed_events (OXR_Instance* instance, u64 frame)
{
    u32 kept = 0;
    for (u32 i = 0; i < instance->delayed_count; i++)
    {
        if (instance->delayed_frames[i] <= frame)
        {
            push_event(instance, instance->delayed[i]);
            continue;
        }
        instance->delayed[kept]        = instance->delayed[i];
        instance->delayed_frames[kept] = instance->delayed_frames[i];
        kept++;
    }
    instance->delayed_count = kept;
}

// Spec 7.1: a moved LOCAL origin, and LOCAL_FLOOR which follows it, is announced per session
static void queue_reference_space_changes (OXR_InstanceHandle instance)
{
    OXR_Runtime*  rt   = runtime();
    OXR_Instance* inst = rt->instances.try_get(instance);
    if (!inst || !rt->sim)
    {
        return;
    }
    u64 generation  = 0;
    i64 change_time = 0;
    sim_local_origin_change(rt->sim, generation, change_time);
    const Pose origin = sim_get_reference_space_pose(rt->sim, SIM_ReferenceSpace::Local);
    for (usize i = 0; i < rt->sessions.length(); i++)
    {
        OXR_Session& session = rt->sessions.data()[i];
        if (session.instance != instance || session.origin_generation == generation)
        {
            continue;
        }
        const XrSession xr_session = session_xr(rt->sessions.handle_at(i));
        const Pose      floor_seen = { .position = { session.origin_seen.position.x, 0.0f, session.origin_seen.position.z }, .orientation = session.origin_seen.orientation };
        const Pose      floor      = sim_get_reference_space_pose(rt->sim, SIM_ReferenceSpace::LocalFloor);
        push_event(inst, { .type = XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING, .session = xr_session, .time = change_time, .space_type = XR_REFERENCE_SPACE_TYPE_LOCAL, .pose = zm::pose_relative(session.origin_seen, origin) });
        if (inst->ext.EXT_local_floor || XR_VERSION_MINOR(inst->api_version) >= 1)
        {
            push_event(inst, { .type = XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING, .session = xr_session, .time = change_time, .space_type = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR, .pose = zm::pose_relative(floor_seen, floor) });
        }
        session.origin_generation = generation;
        session.origin_seen       = origin;
    }
}

static void session_set_state (OXR_Session* session, OXR_SessionHandle handle, XrSessionState state)
{
    if (session->state == state)
    {
        return;
    }
    session->state = state;
    OXR_Instance* instance = session_instance(session);
    if (instance)
    {
        push_event(instance, { .type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED, .session = session_xr(handle), .state = state, .time = sim_time(runtime()->sim) });
    }
}

// Poses

static b8 xr_pose_valid (const XrPosef& pose)
{
    const Quat q = { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w };
    const Vec3 p = { pose.position.x, pose.position.y, pose.position.z };
    if (!zm::is_finite(Pose{ p, q }))
    {
        return false;
    }
    return zm::near(zm::length(q), 1.0f, 1e-3f);
}

static Pose pose_from_xr (const XrPosef& pose)
{
    return { { pose.position.x, pose.position.y, pose.position.z }, zm::normalize({ pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w }) };
}

static XrPosef pose_to_xr (const Pose& pose)
{
    return { { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w }, { pose.position.x, pose.position.y, pose.position.z } };
}

static XrVector3f vec3_to_xr (Vec3 v)
{
    return { v.x, v.y, v.z };
}

// Actions

static OXR_Instance* action_instance (const OXR_Action* action)
{
    const OXR_ActionSet* set = runtime()->action_sets.try_get(action->set);
    return set ? runtime()->instances.try_get(set->instance) : nullptr;
}

static Vec2 source_value (const SIM_Sim* sim, OXR_TopLevel side, const OXR_ProfileComponent* component, XrActionType type)
{
    if (component->kind == OXR_SourceKind::Ready)
    {
        return { sim_get_device_state(sim, side_device(side)).tracked ? 1.0f : 0.0f, 0.0f };
    }
    if (side_is_detached(side))
    {
        return {};
    }
    const Vec2 raw = sim_get_input(sim, side_device(side), component->input).value;
    f32        scalar = 0.0f;
    switch (component->kind)
    {
    case OXR_SourceKind::Bool:
        scalar = raw.x;
        break;
    case OXR_SourceKind::FloatAsBool:
        scalar = raw.x > 0.5f ? 1.0f : 0.0f;
        break;
    case OXR_SourceKind::Float:
        scalar = raw.x;
        break;
    case OXR_SourceKind::Vec2:
        return raw;
    case OXR_SourceKind::Vec2X:
        scalar = raw.x;
        break;
    case OXR_SourceKind::Vec2Y:
        scalar = raw.y;
        break;
    default:
        return {};
    }
    if (type == XR_ACTION_TYPE_BOOLEAN_INPUT)
    {
        scalar = (component->kind == OXR_SourceKind::Float ? scalar > 0.5f : scalar != 0.0f) ? 1.0f : 0.0f;
    }
    return { scalar, 0.0f };
}

// XR_EXT_dpad_binding: the directions whose wedge of the given angle holds the deflection
static u8 dpad_regions (Vec2 xy, f32 wedge)
{
    static const struct
    {
        f32 angle;
        u8  direction;
    } directions[] = { { 0.0f, OXR_DPAD_RIGHT }, { 0.5f * zm::PI, OXR_DPAD_UP }, { zm::PI, OXR_DPAD_LEFT }, { -0.5f * zm::PI, OXR_DPAD_DOWN } };
    const f32 angle   = zm::atan2(xy.y, xy.x);
    u8        regions = 0;
    for (const auto& entry : directions)
    {
        f32 delta = zm::abs(angle - entry.angle);
        delta     = delta > zm::PI ? 2.0f * zm::PI - delta : delta;
        regions  |= delta <= 0.5f * wedge ? entry.direction : 0;
    }
    return regions;
}

// A source read through its binding modifier, which keeps state across syncs and plays its
// haptics when the boolean changes
static Vec2 bound_value (OXR_BoundSource& source, const SIM_Sim* sim, OXR_TopLevel side, XrActionType type)
{
    const OXR_BindingModifier& modifier = source.modifier;
    b8                         on       = false;
    switch (modifier.kind)
    {
    case OXR_ModifierKind::None:
        return source_value(sim, side, source.component, type);
    case OXR_ModifierKind::Dpad:
    {
        // Spec: a sticky dpad latches the regions it entered until the stick returns to center
        const Vec2 xy = source_value(sim, side, source.component, XR_ACTION_TYPE_VECTOR2F_INPUT);
        if (zm::length(xy) < modifier.center)
        {
            source.latched = 0;
        }
        else if (!modifier.sticky || !source.latched)
        {
            source.latched = dpad_regions(xy, modifier.wedge);
        }
        on = (source.latched & modifier.direction) != 0;
        break;
    }
    case OXR_ModifierKind::Threshold:
    {
        // Spec: true from onThreshold until the value falls below offThreshold
        const f32 value = source_value(sim, side, source.component, XR_ACTION_TYPE_FLOAT_INPUT).x;
        on              = source.on ? value >= modifier.off : value >= modifier.on;
        break;
    }
    }
    if (on != source.on)
    {
        const XrHapticVibration& haptic = on ? modifier.on_haptic : modifier.off_haptic;
        if (haptic.type == XR_TYPE_HAPTIC_VIBRATION && !side_is_detached(side))
        {
            sim_apply_haptic(runtime()->sim, side_device(side), SIM_HapticOutput::Main, haptic.amplitude, haptic.duration, haptic.frequency);
        }
        source.on = on;
    }
    return { on ? 1.0f : 0.0f, 0.0f };
}

static void update_action_state (OXR_ActionState& state, b8 active, Vec2 value, XrTime now)
{
    if (!active)
    {
        value = {};
    }
    const b8 changed = active && (value.x != state.value.x || value.y != state.value.y);
    if (changed || active != state.active)
    {
        state.last_change_time = now;
    }
    state.changed = changed;
    state.value   = value;
    state.active  = active;
}

static b8 side_bound (const OXR_Action* action, u32 side)
{
    for (u32 i = 0; i < action->sources_count[side]; i++)
    {
        if (!action->sources[side][i].suppressed)
        {
            return true;
        }
    }
    return false;
}

// Bit i set when the action drives SIM_HapticOutput i of that side
static u8 bound_haptics (const OXR_Action* action, u32 side)
{
    u8 outputs = 0;
    for (u32 i = 0; i < action->sources_count[side]; i++)
    {
        const OXR_BoundSource& source = action->sources[side][i];
        if (!source.suppressed && source.component && source.component->kind == OXR_SourceKind::Haptic)
        {
            outputs |= (u8)(1u << (u32)source.component->haptic);
        }
    }
    return outputs;
}

// Spec 11.2: sources share an input source when they have the same identifier and location,
// whatever their component
static u64 source_identifier (const char* component_path)
{
    static const char* const components[] = { "click", "touch", "force", "value", "x", "y", "twist", "pose" };
    const char* last = component_path;
    for (const char* c = component_path; *c; c++)
    {
        if (*c == '/')
        {
            last = c;
        }
    }
    usize length = cstr_length(component_path);
    for (const char* component : components)
    {
        if (cstr_equal(last + 1, component))
        {
            length = (usize)(last - component_path);
        }
    }
    return hash((const u8*)component_path, length);
}

static b8 action_attached_to (const OXR_Session* session, const OXR_Action* action)
{
    for (const OXR_ActionSetHandle& set : session->attached_sets)
    {
        if (set == action->set)
        {
            return true;
        }
    }
    return false;
}

static b8 set_active_on (const OXR_ActionSet* set, u32 side)
{
    return set->active && (set->active_sides & (1 << side));
}

static void suppress_by_priority (const OXR_Session* session)
{
    OXR_Runtime* rt = runtime();
    for (usize a = 0; a < rt->actions.length(); a++)
    {
        OXR_Action&          action = rt->actions.data()[a];
        const OXR_ActionSet* set    = rt->action_sets.try_get(action.set);
        if (!set || !action_attached_to(session, &action))
        {
            continue;
        }
        for (u32 side = 0; side < (u32)OXR_TopLevel::Count; side++)
        {
            for (u32 i = 0; i < action.sources_count[side]; i++)
            {
                OXR_BoundSource& source = action.sources[side][i];
                source.suppressed       = false;
                for (usize b = 0; b < rt->actions.length() && set_active_on(set, side) && !source.suppressed; b++)
                {
                    const OXR_Action&    other     = rt->actions.data()[b];
                    const OXR_ActionSet* other_set = rt->action_sets.try_get(other.set);
                    if (!other_set || other_set->priority <= set->priority || !set_active_on(other_set, side) || !action_attached_to(session, &other))
                    {
                        continue;
                    }
                    for (u32 k = 0; k < other.sources_count[side]; k++)
                    {
                        source.suppressed |= other.sources[side][k].identifier == source.identifier;
                    }
                }
            }
        }
    }
}

static void sync_action (OXR_Action* action, const OXR_ActionSet* set, const SIM_Sim* sim, XrTime now)
{
    Vec2   combined     = {};
    b8     any_active   = false;
    b8     any_changed  = false;
    XrTime latest       = 0;

    for (u32 side = 0; side < (u32)OXR_TopLevel::Count; side++)
    {
        OXR_ActionState& state  = action->states[side];
        const b8         bound  = side_bound(action, side);
        const b8         eyes   = side == (u32)OXR_TopLevel::Eyes && sim_get_device_state(sim, SIM_Device::Head).active;
        const b8         hand   = side_has_device((OXR_TopLevel)side) && sim_get_device_state(sim, side_device((OXR_TopLevel)side)).active;
        const b8         active = set->active && (set->active_sides & (1 << side)) && bound && (eyes || hand);

        Vec2 value = {};
        if (active && action->type != XR_ACTION_TYPE_POSE_INPUT && action->type != XR_ACTION_TYPE_VIBRATION_OUTPUT)
        {
            for (u32 i = 0; i < action->sources_count[side]; i++)
            {
                if (action->sources[side][i].suppressed)
                {
                    continue;
                }
                const Vec2 v = bound_value(action->sources[side][i], sim, (OXR_TopLevel)side, action->type);
                if (zm::length_sq(v) > zm::length_sq(value))
                {
                    value = v;
                }
            }
        }
        update_action_state(state, active, value, now);

        if (active)
        {
            if (!any_active || zm::length_sq(state.value) > zm::length_sq(combined))
            {
                combined = state.value;
            }
            any_active  = true;
            any_changed |= state.changed;
            latest      = zm::max(latest, state.last_change_time);
        }
    }

    // Spec 7.2.1: a space without a subaction path changes device only at a sync. It keeps the
    // device it tracks while that stays bound, so a device that drops out is not replaced.
    if (action->type == XR_ACTION_TYPE_POSE_INPUT && (action->pose_side == OXR_TopLevel::Count || !side_bound(action, (u32)action->pose_side)))
    {
        action->pose_side = OXR_TopLevel::Count;
        for (u32 side = 0; side < (u32)OXR_TopLevel::Count && action->pose_side == OXR_TopLevel::Count; side++)
        {
            if (action->states[side].active)
            {
                action->pose_side = (OXR_TopLevel)side;
            }
        }
    }

    OXR_ActionState& all = action->states[(u32)OXR_TopLevel::Count];
    update_action_state(all, any_active, combined, now);
    all.changed = any_changed;
    if (any_active)
    {
        all.last_change_time = latest;
    }
}

// The app suggested bindings for the profile of an action of an attached set
static b8 profile_attached (const OXR_Instance* instance, const OXR_Session* session, OXR_Profile profile)
{
    for (const OXR_SuggestedBinding& suggested : instance->suggested[(u32)profile])
    {
        const OXR_Action* action = runtime()->actions.try_get(suggested.action);
        if (!action)
        {
            continue;
        }
        for (const OXR_ActionSetHandle& set : session->attached_sets)
        {
            if (set == action->set)
            {
                return true;
            }
        }
    }
    return false;
}

static OXR_Profile choose_profile (const OXR_Instance* instance, const OXR_Session* session, SIM_Controller hardware, SIM_HandSource source)
{
    OXR_Profile order[(u32)OXR_Profile::Count];
    const u32   order_count = hardware_profiles(hardware, source, order);
    for (u32 p = 0; p < order_count; p++)
    {
        if (profile_attached(instance, session, order[p]))
        {
            return order[p];
        }
    }
    return OXR_Profile::None;
}

static void hand_sources (SIM_HandSource (&out_sources)[2])
{
    out_sources[0] = sim_get_hand_source(runtime()->sim, SIM_Device::LeftHand);
    out_sources[1] = sim_get_hand_source(runtime()->sim, SIM_Device::RightHand);
}

static OXR_BindingModifier binding_modifier (const OXR_Instance* instance, OXR_Profile profile, const OXR_Action& action, const OXR_SuggestedBinding& suggested, const OXR_ProfileComponent* component, u8 direction, const char* input)
{
    if (direction)
    {
        // Spec XR_EXT_dpad_binding: without a struct for the input and action set, these values
        OXR_BindingModifier modifier = { .kind = OXR_ModifierKind::Dpad, .on = 0.5f, .off = 0.4f, .center = 0.5f, .wedge = 0.5f * zm::PI };
        for (const OXR_SuggestedModifier& entry : instance->modifiers[(u32)profile])
        {
            const char* path = path_string(instance, entry.path);
            if (entry.modifier.kind == OXR_ModifierKind::Dpad && entry.set == action.set && path && cstr_equal(path, input))
            {
                modifier = entry.modifier;
            }
        }
        modifier.direction = direction;
        return modifier;
    }
    if (action.type == XR_ACTION_TYPE_BOOLEAN_INPUT && component->kind == OXR_SourceKind::Float)
    {
        for (const OXR_SuggestedModifier& entry : instance->modifiers[(u32)profile])
        {
            if (entry.modifier.kind == OXR_ModifierKind::Threshold && entry.action == suggested.action && entry.path == suggested.path)
            {
                return entry.modifier;
            }
        }
    }
    return {};
}

// Each hand binds from its own profile; a binding of the other hand's paths is skipped
static void bind_actions (const OXR_Instance* instance, OXR_Session* session, const OXR_Profile (&profiles)[(u32)OXR_TopLevel::Count], SIM_Controller hardware, const SIM_HandSource (&sources)[2])
{
    session->profile_hardware   = hardware;
    session->profile_sources[0] = sources[0];
    session->profile_sources[1] = sources[1];
    for (const OXR_ActionSetHandle& set_handle : session->attached_sets)
    {
        for (usize i = 0; i < runtime()->actions.length(); i++)
        {
            OXR_Action& action = runtime()->actions.data()[i];
            if (action.set != set_handle)
            {
                continue;
            }
            memory_zero(action.sources_count, sizeof(action.sources_count));
            action.pose_side = OXR_TopLevel::Count;
        }
    }
    for (u32 hand = 0; hand < (u32)OXR_TopLevel::Count; hand++)
    {
        const OXR_Profile profile = profiles[hand];
        if (profile == OXR_Profile::None)
        {
            continue;
        }
        for (const OXR_SuggestedBinding& suggested : instance->suggested[(u32)profile])
        {
            OXR_Action* action = runtime()->actions.try_get(suggested.action);
            if (!action)
            {
                continue;
            }
            b8 attached = false;
            for (const OXR_ActionSetHandle& set : session->attached_sets)
            {
                attached |= set == action->set;
            }
            if (!attached)
            {
                continue;
            }

            OXR_TopLevel                side      = OXR_TopLevel::Count;
            u8                          direction = 0;
            char                        input[XR_MAX_PATH_LENGTH] = {};
            const OXR_ProfileComponent* component = resolve_binding(instance, profile, hardware, sources, suggested.path, side);
            if (!component)
            {
                component = resolve_dpad_binding(instance, profile, hardware, sources, suggested.path, side, direction, input);
            }
            if (!component || (u32)side != hand || !(action->subaction_mask & (1 << (u32)side)) || !source_compatible(action->type, direction ? OXR_SourceKind::Bool : component->kind))
            {
                continue;
            }
            u32& count = action->sources_count[(u32)side];
            if (count < OXR_MAX_BOUND_SOURCES)
            {
                const OXR_BindingModifier modifier = binding_modifier(instance, profile, *action, suggested, component, direction, input);
                action->sources[(u32)side][count++] = { .path = suggested.path, .component = component, .identifier = source_identifier(component->path), .suppressed = false, .modifier = modifier };
            }
        }
    }
}

// Spec XR_META_detached_controllers: while a hand is tracked, its put down controller serves its
// controller profile under the detached path; a profile is current on at most one of the two
static void choose_profiles (const OXR_Instance* instance, const OXR_Session* session, SIM_Controller hardware, const SIM_HandSource (&sources)[2], OXR_Profile (&out_profiles)[(u32)OXR_TopLevel::Count])
{
    for (u32 side = 0; side < (u32)OXR_TopLevel::Count; side++)
    {
        out_profiles[side] = OXR_Profile::None;
    }
    for (u32 hand = 0; hand < 2; hand++)
    {
        out_profiles[hand]             = choose_profile(instance, session, hardware, sources[hand]);
        const OXR_TopLevel detached    = hand == 0 ? OXR_TopLevel::DetachedLeft : OXR_TopLevel::DetachedRight;
        const b8           put_down    = instance->ext.META_detached_controllers && sources[hand] == SIM_HandSource::Hand;
        out_profiles[(u32)detached]    = put_down ? choose_profile(instance, session, hardware, SIM_HandSource::Controller) : OXR_Profile::None;
    }
    if (instance->ext.EXT_eye_gaze_interaction && profile_attached(instance, session, OXR_Profile::EyeGaze))
    {
        out_profiles[(u32)OXR_TopLevel::Eyes] = OXR_Profile::EyeGaze;
    }
}

static const OXR_ProfileComponent* bound_pose_source (const OXR_Action* action, OXR_TopLevel side)
{
    for (u32 i = 0; i < action->sources_count[(u32)side]; i++)
    {
        if (action->sources[(u32)side][i].component->kind == OXR_SourceKind::Pose && !action->sources[(u32)side][i].suppressed)
        {
            return action->sources[(u32)side][i].component;
        }
    }
    return nullptr;
}

// Spaces

namespace
{

struct OXR_Located
{
    Pose pose;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    b8   valid;
    b8   tracked;
};

} // namespace

static const SIM_Prop* find_prop (const SIM_Sim* sim, u32 id)
{
    const SIM_Prop* props = nullptr;
    const u32       count = sim ? sim_props(sim, props) : 0;
    for (u32 i = 0; i < count; i++)
    {
        if (props[i].id == id)
        {
            return &props[i];
        }
    }
    return nullptr;
}

// A prop's UUID is "puppetxrprop" and its id, so it names the prop for good
static W_Uuid prop_uuid (u32 id)
{
    W_Uuid uuid = { .bytes = { 'x', 'r', 's', 'i', 'm', 'p', 'r', 'o', 'p' } };
    uuid.bytes[12] = (u8)(id >> 24);
    uuid.bytes[13] = (u8)(id >> 16);
    uuid.bytes[14] = (u8)(id >> 8);
    uuid.bytes[15] = (u8)id;
    return uuid;
}

// A tracked keyboard's space: +Z up out of the keys, +X to the right, +Y away from the user,
// at the centre of its box; the prop has +Y up and its front at +Z
static Pose tracked_prop_pose (const SIM_Prop& prop, i64 time)
{
    return zm::pose_mul(sim_prop_pose(prop, time), { {}, zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -0.5f * zm::PI) });
}

static Vec3 tracked_prop_extent (const SIM_Prop& prop)
{
    const Vec3 extent = sim_prop_extent(prop);
    return { extent.x, extent.z, extent.y };
}

static OXR_Located locate_world (const OXR_Space* space, const SIM_Sample& sample)
{
    const OXR_Runtime* rt  = runtime();
    const SIM_Sim*     sim = rt->sim;
    OXR_Located        out = {};

    if (space->kind == OXR_SpaceKind::Entity || space->kind == OXR_SpaceKind::Anchor)
    {
        const b8 locatable = space->components_enabled & (1u << (u32)OXR_Component::Locatable);
        const b8 fresh     = space->kind == OXR_SpaceKind::Anchor || (rt->world && space->world_generation == rt->world_generation);
        const b8 localized = space->kind != OXR_SpaceKind::Anchor || sim_frame_index(sim) >= space->localized_frame;
        if (locatable && fresh && localized)
        {
            out.pose    = space->kind == OXR_SpaceKind::Anchor ? space->anchor_pose : zm::pose_mul(rt->stage_from_room, w_entity(rt->world, space->entity).pose);
            out.valid   = true;
            out.tracked = true;
        }
        return out;
    }

    if (space->kind == OXR_SpaceKind::Prop)
    {
        const SIM_Prop* prop = find_prop(sim, space->prop);
        if (prop && (space->components_enabled & (1u << (u32)OXR_Component::Locatable)))
        {
            out.pose    = tracked_prop_pose(*prop, sample.time);
            out.valid   = true;
            out.tracked = true;
        }
        return out;
    }

    // Spec XR_FB_keyboard_tracking: the origin is the bounding box centre, +Y up, as the prop
    if (space->kind == OXR_SpaceKind::TrackedKeyboard)
    {
        const SIM_Prop* prop = find_prop(sim, space->prop);
        if (prop)
        {
            out.pose    = sim_prop_pose(*prop, sample.time);
            out.valid   = true;
            out.tracked = true;
        }
        return out;
    }

    // Spec XR_EXT_render_model: located only while the model is listed and the session has
    // focus; no flags at all once the model is destroyed
    if (space->kind == OXR_SpaceKind::RenderModel)
    {
        const OXR_RenderModel* model   = rt->render_models.try_get(space->render_model);
        const OXR_Session*     session = rt->sessions.try_get(space->session);
        if (model && session && session->state == XR_SESSION_STATE_FOCUSED && session->render_model_id[model->hand] == model->id)
        {
            const SIM_Device       device = model->hand == 0 ? SIM_Device::LeftHand : SIM_Device::RightHand;
            const SIM_DeviceState& state  = sample.devices[(u32)device];
            out.pose             = sim_sample_hand_pose(sim, sample, device, SIM_HandPose::Grip);
            out.linear_velocity  = state.linear_velocity + zm::cross(state.angular_velocity, out.pose.position - state.pose.position);
            out.angular_velocity = state.angular_velocity;
            out.valid            = true;
            out.tracked          = state.tracked;
        }
        return out;
    }

    if (space->kind == OXR_SpaceKind::VirtualKeyboard)
    {
        const OXR_VirtualKeyboard* keyboard = rt->keyboards.try_get(space->keyboard);
        if (keyboard)
        {
            out.pose    = keyboard->pose;
            out.valid   = true;
            out.tracked = true;
        }
        return out;
    }

    if (space->kind == OXR_SpaceKind::Reference)
    {
        SIM_ReferenceSpace kind = SIM_ReferenceSpace::Stage;
        switch (space->reference_type)
        {
        case XR_REFERENCE_SPACE_TYPE_VIEW:
            kind = SIM_ReferenceSpace::View;
            break;
        case XR_REFERENCE_SPACE_TYPE_LOCAL:
            kind = SIM_ReferenceSpace::Local;
            break;
        case XR_REFERENCE_SPACE_TYPE_STAGE:
            kind = SIM_ReferenceSpace::Stage;
            break;
        case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
            kind = SIM_ReferenceSpace::LocalFloor;
            break;
        case XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT:
            kind = SIM_ReferenceSpace::Stationary;
            break;
        default:
            break;
        }
        out.pose    = zm::pose_mul(sim_sample_reference_space_pose(sample, kind), space->offset);
        out.valid   = true;
        out.tracked = true;
        if (kind == SIM_ReferenceSpace::View)
        {
            const SIM_DeviceState& head = sample.devices[(u32)SIM_Device::Head];
            out.linear_velocity  = head.linear_velocity + zm::cross(head.angular_velocity, out.pose.position - head.pose.position);
            out.angular_velocity = head.angular_velocity;
            out.tracked          = head.tracked;
        }
        return out;
    }

    const OXR_Action* action = rt->actions.try_get(space->action);
    if (!action)
    {
        return out;
    }

    for (u32 side = 0; side < (u32)OXR_TopLevel::Count; side++)
    {
        const OXR_TopLevel tracked = space->subaction != OXR_TopLevel::Count ? space->subaction : action->pose_side;
        if (tracked != (OXR_TopLevel)side)
        {
            continue;
        }
        const OXR_ProfileComponent* component = bound_pose_source(action, (OXR_TopLevel)side);
        if (component && action->states[side].active && side == (u32)OXR_TopLevel::Eyes)
        {
            // Between the eyes, looking along their gaze (docs/face-tracking.md)
            SIM_FaceFrame face = {};
            sim_face_frame(sim, sample, face);
            const SIM_DeviceState& head    = sample.devices[(u32)SIM_Device::Head];
            const Vec3             forward = zm::normalize(zm::rotate(face.eyes[0].orientation, { 0.0f, 0.0f, -1.0f }) + zm::rotate(face.eyes[1].orientation, { 0.0f, 0.0f, -1.0f }));
            const Pose             gaze    = { (face.eyes[0].position + face.eyes[1].position) * 0.5f, zm::quat_look_rotation(forward, zm::rotate(head.pose.orientation, { 0.0f, 1.0f, 0.0f })) };
            out.pose             = zm::pose_mul(gaze, space->offset);
            out.linear_velocity  = head.linear_velocity + zm::cross(head.angular_velocity, out.pose.position - head.pose.position);
            out.angular_velocity = head.angular_velocity;
            out.valid            = head.active;
            out.tracked          = head.tracked;
            return out;
        }
        if (!component || !action->states[side].active || !side_has_device((OXR_TopLevel)side))
        {
            continue;
        }
        if (side_is_detached((OXR_TopLevel)side))
        {
            // A controller lying down is tracked where it lies and does not move
            const SIM_Device hand = side_device((OXR_TopLevel)side);
            out.pose    = zm::pose_mul(sim_detached_hand_pose(sim, hand, component->pose), space->offset);
            out.valid   = sample.devices[(u32)hand].active && sim_get_hand_source(sim, hand) == SIM_HandSource::Hand;
            out.tracked = out.valid;
            return out;
        }
        const SIM_Device       hand   = side_device((OXR_TopLevel)side);
        const SIM_DeviceState& device = sample.devices[(u32)hand];
        const b8               seen   = sim_get_hand_source(sim, hand) == SIM_HandSource::Controller || sim_hand_in_view(sim, sample, hand);
        out.pose             = zm::pose_mul(sim_sample_hand_pose(sim, sample, hand, component->pose), space->offset);
        out.linear_velocity  = device.linear_velocity + zm::cross(device.angular_velocity, out.pose.position - device.pose.position);
        out.angular_velocity = device.angular_velocity;
        out.valid            = device.active && seen;
        out.tracked          = device.tracked;
        return out;
    }
    return out;
}

// Spec XR_EXT_eye_gaze_interaction: an action space of a pose action bound on /user/eyes_ext
static b8 space_is_eye_gaze (const OXR_Space* space)
{
    if (space->kind != OXR_SpaceKind::Action)
    {
        return false;
    }
    const OXR_Action* action = runtime()->actions.try_get(space->action);
    return action && (space->subaction == OXR_TopLevel::Eyes || (space->subaction == OXR_TopLevel::Count && action->pose_side == OXR_TopLevel::Eyes));
}

static void locate_relative (const OXR_Located& target, const OXR_Located& base, XrPosef& out_pose, XrSpaceLocationFlags& out_flags, XrSpaceVelocity* out_velocity)
{
    out_flags = 0;
    out_pose  = pose_to_xr(zm::POSE_IDENTITY);
    if (out_velocity)
    {
        out_velocity->velocityFlags = 0;
    }
    if (!target.valid || !base.valid)
    {
        return;
    }

    out_pose  = pose_to_xr(zm::pose_relative(base.pose, target.pose));
    out_flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (target.tracked && base.tracked)
    {
        out_flags |= XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    }

    if (out_velocity)
    {
        const Quat inv = zm::inverse(base.pose.orientation);
        const Vec3 lever              = target.pose.position - base.pose.position;
        out_velocity->linearVelocity  = vec3_to_xr(zm::rotate(inv, target.linear_velocity - base.linear_velocity - zm::cross(base.angular_velocity, lever)));
        out_velocity->angularVelocity = vec3_to_xr(zm::rotate(inv, target.angular_velocity - base.angular_velocity));
        out_velocity->velocityFlags   = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
    }
}

// Destruction

static void destroy_space (OXR_SpaceHandle handle)
{
    runtime()->spaces.remove(handle);
}

static void destroy_spatial_context (OXR_SpatialContextHandle handle);
static void poll_spatial_contexts   (OXR_Runtime* rt, OXR_Session* session, OXR_SessionHandle handle);
static void sync_render_models      (OXR_Runtime* rt, OXR_Instance* inst, OXR_Session* session, OXR_SessionHandle handle);

static void destroy_future (OXR_FutureHandle handle)
{
    OXR_Runtime*      rt     = runtime();
    const OXR_Future* future = rt->futures.try_get(handle);
    if (future && future->kind == OXR_FutureKind::CreateRaycaster)
    {
        rt->raycasters.remove(OXR_RaycasterHandle{ future->payload });
    }
    if (future && future->kind == OXR_FutureKind::CreateSpatialContext)
    {
        destroy_spatial_context(OXR_SpatialContextHandle{ future->payload });
    }
    rt->futures.remove(handle);
}

static void destroy_depth_swapchain (OXR_DepthSwapchainHandle handle)
{
    OXR_Runtime*        rt        = runtime();
    const OXR_DepthSwapchain* swapchain = rt->depth_swapchains.try_get(handle);
    if (!swapchain)
    {
        return;
    }
    const OXR_Session* session = rt->sessions.try_get(swapchain->session);
    if (session && session->compositor)
    {
        comp_destroy_depth_target(session->compositor, swapchain->target);
    }
    OXR_DepthProvider* provider = rt->depth_providers.try_get(swapchain->provider);
    if (provider)
    {
        provider->swapchain = {};
    }
    rt->depth_swapchains.remove(handle);
}

static void destroy_camera_provider (OXR_CameraProviderHandle handle)
{
    OXR_Runtime*              rt       = runtime();
    const OXR_CameraProvider* provider = rt->camera_providers.try_get(handle);
    if (!provider)
    {
        return;
    }
    const OXR_Session* session = rt->sessions.try_get(provider->session);
    if (session && session->compositor)
    {
        comp_destroy_camera_target(session->compositor, provider->target);
    }
    for (const OXR_CameraImage& image : provider->images)
    {
        os_memory_release(image.rgba);
    }
    rt->camera_providers.remove(handle);
}

static void destroy_swapchain (OXR_SwapchainHandle handle)
{
    OXR_Runtime*   rt        = runtime();
    const OXR_Swapchain* swapchain = rt->swapchains.try_get(handle);
    if (!swapchain)
    {
        return;
    }
    const OXR_Session* session = rt->sessions.try_get(swapchain->session);
    if (session && session->device)
    {
        for (u32 i = 0; i < swapchain->images_count; i++)
        {
            r_destroy_image(session->device, swapchain->images[i]);
        }
    }
    rt->swapchains.remove(handle);
}

// Spec XR_FB_passthrough: destroying a mesh or a layer destroys the geometry instances using it
static void destroy_geometry_using (OXR_TriangleMeshHandle mesh, OXR_PassthroughLayerHandle layer)
{
    OXR_Runtime* rt = runtime();
    for (usize i = rt->geometry_instances.length(); i-- > 0;)
    {
        const OXR_GeometryInstance& instance = rt->geometry_instances.data()[i];
        if ((mesh.value && instance.mesh == mesh) || (layer.value && instance.layer == layer))
        {
            rt->geometry_instances.remove(rt->geometry_instances.handle_at(i));
        }
    }
}

static void destroy_triangle_mesh (OXR_TriangleMeshHandle handle)
{
    OXR_Runtime*      rt   = runtime();
    OXR_TriangleMesh* mesh = rt->triangle_meshes.try_get(handle);
    if (!mesh)
    {
        return;
    }
    destroy_geometry_using(handle, {});
    mesh->vertices.release();
    mesh->indices.release();
    mesh->drawn_vertices.release();
    mesh->drawn_indices.release();
    rt->triangle_meshes.remove(handle);
}

static void request_exit (OXR_Session* session, OXR_SessionHandle handle);

//
// Lens mask
//

// The lens mask's radius while it is on (views.lens_mask, switched in the UI), 0 while off
static f32 lens_mask_radius (const OXR_Runtime* rt)
{
    return rt->lens_mask ? sim_get_config(rt->sim).mask_radius : 0.0f;
}

// Spec 12.27: each session hears that both views' masks changed and gets them again
static void set_lens_mask_locked (OXR_Runtime* rt, b8 on)
{
    if (rt->lens_mask == on)
    {
        return;
    }
    rt->lens_mask = on;
    for (const OXR_Session& session : rt->sessions)
    {
        if (session.compositor)
        {
            comp_set_lens_mask(session.compositor, lens_mask_radius(rt));
        }
        OXR_Instance* inst = rt->instances.try_get(session.instance);
        if (!inst || !inst->ext.KHR_visibility_mask)
        {
            continue;
        }
        for (u32 view = 0; view < OXR_VIEW_COUNT; view++)
        {
            push_event(inst, { .type = XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR, .session = session_xr(session_handle_of(&session)), .view_index = view });
        }
    }
}

//
// Rooms
//

static void resolve_rooms_dir (const SIM_Config& config, char* out_dir, usize out_size)
{
    if (config.rooms_dir[0])
    {
        cstr_copy(out_dir, out_size, config.rooms_dir);
        return;
    }
    char module_dir[OS_MAX_PATH] = {};
    char next_to_library[OS_MAX_PATH] = {};
    if (os_module_directory(module_dir, sizeof(module_dir)))
    {
        cstr_format(next_to_library, sizeof(next_to_library), "%s/%s", module_dir, PUPPETXR_RELATIVE_ROOMS_DIR);
    }
    const char* const candidates[] = { next_to_library, PUPPETXR_SOURCE_ROOMS_DIR, PUPPETXR_INSTALL_ROOMS_DIR };
    for (const char* candidate : candidates)
    {
        if (candidate[0] && os_directory_exists(candidate))
        {
            cstr_copy(out_dir, out_size, candidate);
            return;
        }
    }
    cstr_copy(out_dir, out_size, PUPPETXR_INSTALL_ROOMS_DIR);
}

// Loads the named room in place of the current one; an empty name unloads it. The current room
// stays when loading fails or the content hash differs from a non-zero `expected_hash`.
static b8 load_room_locked (OXR_Runtime* rt, const char* name, u64 expected_hash)
{
    W_World* world = nullptr;
    if (name && name[0])
    {
        world = w_open_room(rt->rooms_dir, name);
        if (!world)
        {
            LOG_ERROR("Room '%s' is neither in %s nor compiled in, or it is invalid", name, rt->rooms_dir);
            return false;
        }
        if (expected_hash && w_content_hash(world) != expected_hash)
        {
            LOG_ERROR("Room '%s' changed since it was recorded", name);
            w_destroy(world);
            return false;
        }
    }

    w_destroy(rt->world);
    rt->world = world;
    cstr_copy(rt->room, sizeof(rt->room), world ? name : "");
    rt->world_generation++;
    if (world)
    {
        LOG_INFO("Room %s loaded", rt->room);
    }
    return true;
}

//
// Spatial entities
//

static const XrSpaceComponentTypeFB oxr_component_types[(u32)OXR_Component::Count] = {
    XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB,
    XR_SPACE_COMPONENT_TYPE_STORABLE_FB,
    XR_SPACE_COMPONENT_TYPE_SHARABLE_FB,
    XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB,
    XR_SPACE_COMPONENT_TYPE_BOUNDED_3D_FB,
    XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB,
    XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB,
    XR_SPACE_COMPONENT_TYPE_SPACE_CONTAINER_FB,
    XR_SPACE_COMPONENT_TYPE_TRIANGLE_MESH_META,
    XR_SPACE_COMPONENT_TYPE_ROOM_MESH_META,
    (XrSpaceComponentTypeFB)OXR_SPACE_COMPONENT_TYPE_DYNAMIC_OBJECT_DATA_META,
};

// XR_FB_scene label strings; the world's labels map one to one
static const char* const oxr_scene_labels[(u32)W_Label::Count] = {
    "OTHER",
    "FLOOR",
    "CEILING",
    "WALL_FACE",
    "INVISIBLE_WALL_FACE",
    "INNER_WALL_FACE",
    "DOOR_FRAME",
    "WINDOW_FRAME",
    "OPENING",
    "WALL_ART",
    "TABLE",
    "COUCH",
    "BED",
    "STORAGE",
    "SCREEN",
    "LAMP",
    "PLANT",
    "GLOBAL_MESH",
};

namespace
{

struct OXR_EntityFilter
{
    const XrUuidEXT* uuids;
    u32              uuids_count;
    b8               has_component;
    OXR_Component    component;
};

} // namespace

static u32 component_bit (OXR_Component component)
{
    return 1u << (u32)component;
}

static b8 component_from_xr (XrSpaceComponentTypeFB type, OXR_Component& out_component)
{
    for (u32 i = 0; i < (u32)OXR_Component::Count; i++)
    {
        if (oxr_component_types[i] == type)
        {
            out_component = (OXR_Component)i;
            return true;
        }
    }
    return false;
}

static XrUuidEXT uuid_to_xr (const W_Uuid& uuid)
{
    XrUuidEXT out = {};
    memory_copy(out.data, uuid.bytes, XR_UUID_SIZE_EXT);
    return out;
}

static W_Uuid uuid_from_xr (const XrUuidEXT& uuid)
{
    W_Uuid out = {};
    memory_copy(out.bytes, uuid.data, W_UUID_SIZE);
    return out;
}

static u64 next_request_id (OXR_Instance* instance)
{
    return ++instance->next_request_id;
}

// Components an entity of the loaded room supports, given the enabled extensions
static u32 entity_components (const OXR_Instance* instance, const W_Entity& entity)
{
    u32 mask = component_bit(OXR_Component::Locatable);
    if (instance->ext.FB_scene)
    {
        mask |= (entity.components & (u32)W_ComponentFlags::Plane) ? component_bit(OXR_Component::Bounded2D) : 0;
        mask |= (entity.components & (u32)W_ComponentFlags::Volume) ? component_bit(OXR_Component::Bounded3D) : 0;
        mask |= (entity.components & (u32)W_ComponentFlags::Room) ? component_bit(OXR_Component::RoomLayout) : component_bit(OXR_Component::SemanticLabels);
    }
    if (instance->ext.FB_spatial_entity_container && (entity.components & (u32)W_ComponentFlags::Room))
    {
        mask |= component_bit(OXR_Component::SpaceContainer);
    }
    if (instance->ext.META_spatial_entity_mesh && (entity.components & (u32)W_ComponentFlags::Mesh))
    {
        mask |= component_bit(OXR_Component::TriangleMesh);
    }
    if (instance->ext.META_spatial_entity_room_mesh && (entity.components & (u32)W_ComponentFlags::RoomMesh))
    {
        mask |= component_bit(OXR_Component::RoomMesh);
    }
    return mask;
}

// Components of anchors the app created or loaded, given the enabled extensions
static u32 anchor_components (const OXR_Instance* instance)
{
    u32 mask = component_bit(OXR_Component::Locatable);
    if (instance->ext.FB_spatial_entity_storage || instance->ext.META_spatial_entity_persistence)
    {
        mask |= component_bit(OXR_Component::Storable);
    }
    if (instance->ext.FB_spatial_entity_sharing || instance->ext.META_spatial_entity_sharing)
    {
        mask |= component_bit(OXR_Component::Sharable);
    }
    return mask;
}

// Props a dynamic object tracker of the session finds: keyboards while one looks for keyboards
static u32 prop_components (const OXR_Instance* instance)
{
    u32 mask = component_bit(OXR_Component::Locatable) | component_bit(OXR_Component::DynamicObjectData);
    if (instance->ext.FB_scene)
    {
        mask |= component_bit(OXR_Component::Bounded2D) | component_bit(OXR_Component::Bounded3D);
    }
    return mask;
}

static b8 session_tracks_keyboards (OXR_SessionHandle session)
{
    const OXR_Runtime* rt = runtime();
    for (usize i = 0; i < rt->dynamic_trackers.length(); i++)
    {
        const OXR_DynamicObjectTracker& tracker = rt->dynamic_trackers.data()[i];
        if (tracker.session == session && tracker.keyboards)
        {
            return true;
        }
    }
    return false;
}


static b8 space_entity_fresh (const OXR_Space* space)
{
    const OXR_Runtime* rt = runtime();
    return space->kind == OXR_SpaceKind::Entity && rt->world && space->world_generation == rt->world_generation;
}

static u32 supported_components (const OXR_Space* space)
{
    const OXR_Runtime*  rt      = runtime();
    const OXR_Session*  session = rt->sessions.try_get(space->session);
    const OXR_Instance* inst    = session ? rt->instances.try_get(session->instance) : nullptr;
    if (!inst)
    {
        return 0;
    }
    if (space->kind == OXR_SpaceKind::Anchor)
    {
        return anchor_components(inst);
    }
    if (space->kind == OXR_SpaceKind::Prop)
    {
        return prop_components(inst);
    }
    return space_entity_fresh(space) ? entity_components(inst, w_entity(rt->world, space->entity)) : 0;
}

static b8 component_enabled (const OXR_Space* space, OXR_Component component)
{
    return (supported_components(space) & space->components_enabled & component_bit(component)) != 0;
}

static OXR_SpaceHandle create_entity_space (OXR_SessionHandle session, u32 entity)
{
    OXR_Runtime*          rt     = runtime();
    const OXR_SpaceHandle handle = rt->spaces.add();
    OXR_Space*            space  = &rt->spaces.get(handle);
    space->session               = session;
    space->kind                  = OXR_SpaceKind::Entity;
    space->subaction             = OXR_TopLevel::Count;
    space->entity                = entity;
    space->world_generation      = rt->world_generation;
    space->uuid                  = w_entity(rt->world, entity).uuid;
    space->components_enabled    = 0xffffffffu;
    return handle;
}

static OXR_SpaceHandle create_prop_space (OXR_SessionHandle session, u32 prop)
{
    OXR_Runtime*          rt     = runtime();
    const OXR_SpaceHandle handle = rt->spaces.add();
    OXR_Space*            space  = &rt->spaces.get(handle);
    space->session               = session;
    space->kind                  = OXR_SpaceKind::Prop;
    space->subaction             = OXR_TopLevel::Count;
    space->prop                  = prop;
    space->uuid                  = prop_uuid(prop);
    space->components_enabled    = 0xffffffffu;
    return handle;
}

static OXR_SpaceHandle create_anchor_space (OXR_SessionHandle session, const W_Uuid& uuid, const Pose& stage_pose, u32 components_enabled, u64 localized_frame)
{
    OXR_Runtime*          rt     = runtime();
    const OXR_SpaceHandle handle = rt->spaces.add();
    OXR_Space*            space  = &rt->spaces.get(handle);
    space->session               = session;
    space->kind                  = OXR_SpaceKind::Anchor;
    space->subaction             = OXR_TopLevel::Count;
    space->uuid                  = uuid;
    space->anchor_pose           = stage_pose;
    space->localized_frame       = localized_frame;
    space->components_enabled    = components_enabled;
    return handle;
}

static b8 filter_matches (u32 components, const W_Uuid& uuid, const OXR_EntityFilter& filter)
{
    if (filter.has_component && !(components & component_bit(filter.component)))
    {
        return false;
    }
    if (filter.uuids)
    {
        for (u32 i = 0; i < filter.uuids_count; i++)
        {
            if (w_uuid_equal(uuid_from_xr(filter.uuids[i]), uuid))
            {
                return true;
            }
        }
        return false;
    }
    return true;
}

static b8 matches_all (u32 components, const W_Uuid& uuid, const OXR_EntityFilter* filters, u32 filters_count, const OXR_EntityFilter* exclude)
{
    for (u32 f = 0; f < filters_count; f++)
    {
        if (!filter_matches(components, uuid, filters[f]))
        {
            return false;
        }
    }
    return !exclude || !filter_matches(components, uuid, *exclude);
}

// Spec XR_FB_spatial_entity_query and XR_META_spatial_entity_discovery filters: a component
// filter or a non-empty uuid list
static XrResult parse_entity_filter (const XrBaseInStructure* header, XrStructureType component_type, XrStructureType uuid_type, OXR_EntityFilter& out_filter)
{
    out_filter = {};
    OXR_CHECK(header);
    if (header->type == component_type)
    {
        // XrSpaceComponentFilterInfoFB and XrSpaceFilterComponentMETA share this layout
        const XrSpaceComponentFilterInfoFB* filter = (const XrSpaceComponentFilterInfoFB*)header;
        OXR_CHECK(component_from_xr(filter->componentType, out_filter.component));
        out_filter.has_component = true;
        return XR_SUCCESS;
    }
    if (header->type == uuid_type)
    {
        const XrSpaceFilterUuidMETA* filter = (const XrSpaceFilterUuidMETA*)header;
        OXR_CHECK(filter->uuidCount > 0 && filter->uuids);
        out_filter.uuids       = filter->uuids;
        out_filter.uuids_count = filter->uuidCount;
        return XR_SUCCESS;
    }
    OXR_FAIL("a filter of an unknown type");
}

static OXR_Query* add_query (OXR_SessionHandle session, u64 request_id, b8 discovery)
{
    OXR_Runtime* rt = runtime();
    if (rt->queries_count == OXR_MAX_QUERIES)
    {
        LOG_WARNING("%u spatial entity requests were never retrieved, dropping the oldest", OXR_MAX_QUERIES);
        rt->queries[0].entities.release();
        rt->queries[0].props.release();
        rt->queries[0].anchors.release();
        memory_move(rt->queries, rt->queries + 1, (OXR_MAX_QUERIES - 1) * sizeof(OXR_Query));
        rt->queries_count--;
    }
    OXR_Query* query = &rt->queries[rt->queries_count++];
    *query = { .session = session, .request_id = request_id, .discovery = discovery, .world_generation = rt->world_generation };
    return query;
}

static void remove_query (u32 index)
{
    OXR_Runtime* rt = runtime();
    rt->queries[index].entities.release();
    rt->queries[index].props.release();
    rt->queries[index].anchors.release();
    memory_move(rt->queries + index, rt->queries + index + 1, (rt->queries_count - index - 1) * sizeof(OXR_Query));
    rt->queries_count--;
}

namespace
{

// Where a query looks: the room and the local store, or the cloud
enum class OXR_QuerySource : u8
{
    Local = 0,
    Cloud,
};

} // namespace

// A stored anchor belongs to the loaded room, the same version of it, or to no room at all
static b8 stored_in_this_room (const OXR_Runtime* rt, const W_StoredAnchor& anchor)
{
    if (!cstr_equal(anchor.room, rt->room))
    {
        return false;
    }
    const u64 hash = rt->world ? w_content_hash(rt->world) : 0;
    if (anchor.room_hash != hash)
    {
        char uuid[W_UUID_SIZE * 2 + 1];
        w_uuid_to_string(anchor.uuid, uuid);
        LOG_WARNING("Anchor %s was saved in another version of room '%s', not loading it", uuid, anchor.room);
        return false;
    }
    return true;
}

// Takes one use of an injected fault; XR_SUCCESS when none is set
static XrResult take_fault (OXR_Fault& fault)
{
    if (fault.remaining == 0)
    {
        return XR_SUCCESS;
    }
    fault.remaining--;
    return fault.result;
}

// The errors the registry lists for each frame call, so an app sees only what a runtime may return
static b8 frame_fault_allowed (OXR_FrameOp op, XrResult result)
{
    switch (result)
    {
    case XR_ERROR_VALIDATION_FAILURE:
    case XR_ERROR_RUNTIME_FAILURE:
    case XR_ERROR_HANDLE_INVALID:
    case XR_ERROR_INSTANCE_LOST:
    case XR_ERROR_SESSION_LOST:
        return (u32)op < (u32)OXR_FrameOp::Count;
    case XR_ERROR_SESSION_NOT_RUNNING:
        return op == OXR_FrameOp::Wait || op == OXR_FrameOp::Begin || op == OXR_FrameOp::End;
    case XR_ERROR_CALL_ORDER_INVALID:
        return op == OXR_FrameOp::Begin || op == OXR_FrameOp::End;
    case XR_ERROR_TIME_INVALID:
        return op == OXR_FrameOp::LocateViews || op == OXR_FrameOp::End;
    case XR_ERROR_SIZE_INSUFFICIENT:
    case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED:
        return op == OXR_FrameOp::LocateViews;
    case XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED:
    case XR_ERROR_LAYER_INVALID:
    case XR_ERROR_LAYER_LIMIT_EXCEEDED:
    case XR_ERROR_POSE_INVALID:
    case XR_ERROR_SWAPCHAIN_RECT_INVALID:
        return op == OXR_FrameOp::End;
    default:
        return false;
    }
}

// The cloud anchors the user may load, or those shared with a group. Read once while recording,
// the list goes into the recording; a replay takes it from there and never reads the cloud.
static void read_cloud (OXR_Runtime* rt, const W_Uuid* group, Array<W_StoredAnchor>& out_anchors)
{
    out_anchors.clear();
    if (rt->replay && !rt->replay_finished)
    {
        REC_External external = { .kind = REC_ExternalKind::CloudAnchors };
        defer { rec_external_release(external); };
        if (rt->replay_external < rec_external_count(rt->replay) && rec_read_external(rt->replay, rt->replay_external, external) && external.kind == REC_ExternalKind::CloudAnchors)
        {
            rt->replay_external++;
            out_anchors.add(external.anchors.data, external.anchors.length);
            return;
        }
        LOG_ERROR("Replay: the app read the cloud where the recording has no such read");
        rt->replay_divergences++;
        return;
    }

    Array<W_CloudAnchor> cloud = {};
    defer { w_cloud_release_all(cloud); };
    w_cloud_list(rt->cloud_anchors_dir[0] ? rt->cloud_anchors_dir : nullptr, cloud);
    for (const W_CloudAnchor& entry : cloud)
    {
        if (group ? w_cloud_in_group(entry, *group) : w_cloud_visible(entry, rt->user_id))
        {
            out_anchors.add(entry.anchor);
        }
    }
    if (rt->recorder)
    {
        const REC_External external = { .kind = REC_ExternalKind::CloudAnchors, .frame_index = sim_frame_index(rt->sim), .anchors = out_anchors };
        rec_writer_append_external(rt->recorder, external);
    }
}

// Runs a query against the loaded room and the stores and queues its events: results available
// when anything matched, then completion. Both XR_FB_spatial_entity_query and
// XR_META_spatial_entity_discovery require the results event first.
static void run_query (OXR_Instance* instance, OXR_SessionHandle session, u64 request_id, b8 discovery, OXR_QuerySource source, const W_Uuid* group, const OXR_EntityFilter* filters, u32 filters_count, const OXR_EntityFilter* exclude, u32 max_results)
{
    OXR_Runtime*    rt         = runtime();
    OXR_Query*      query      = add_query(session, request_id, discovery);
    const XrSession xr_session = session_xr(session);
    const XrResult  fault      = take_fault(rt->anchor_faults[(u32)OXR_AnchorOp::Load]);
    if (XR_FAILED(fault))
    {
        push_anchor_event(instance, { .type = discovery ? XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_COMPLETE_META : XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB, .session = xr_session, .request_id = request_id, .result = fault });
        return;
    }

    if (source == OXR_QuerySource::Local)
    {
        const u32 count = rt->world ? w_entity_count(rt->world) : 0;
        for (u32 i = 0; i < count && query->entities.length < max_results; i++)
        {
            const W_Entity& entity = w_entity(rt->world, i);
            if (matches_all(entity_components(instance, entity), entity.uuid, filters, filters_count, exclude))
            {
                query->entities.add(i);
            }
        }
        for (const W_StoredAnchor& anchor : rt->store.anchors)
        {
            if (query->entities.length + query->anchors.length < max_results && matches_all(anchor_components(instance), anchor.uuid, filters, filters_count, exclude) && stored_in_this_room(rt, anchor))
            {
                query->anchors.add(anchor);
            }
        }
        const SIM_Prop* props       = nullptr;
        const u32       props_count = instance->ext.META_dynamic_object_keyboard && session_tracks_keyboards(session) ? sim_props(rt->sim, props) : 0;
        for (u32 i = 0; i < props_count; i++)
        {
            const b8 room = query->entities.length + query->anchors.length + query->props.length < max_results;
            if (room && props[i].kind == SIM_PropKind::Keyboard && matches_all(prop_components(instance), prop_uuid(props[i].id), filters, filters_count, exclude))
            {
                query->props.add(props[i].id);
            }
        }
    }
    else
    {
        Array<W_StoredAnchor> cloud = {};
        defer { cloud.release(); };
        read_cloud(rt, group, cloud);
        if (group && cloud.length == 0)
        {
            push_anchor_event(instance, { .type = XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB, .session = xr_session, .request_id = request_id, .result = XR_ERROR_SPACE_GROUP_NOT_FOUND_META });
            return;
        }
        for (const W_StoredAnchor& anchor : cloud)
        {
            if (query->anchors.length < max_results && matches_all(anchor_components(instance), anchor.uuid, filters, filters_count, exclude) && stored_in_this_room(rt, anchor))
            {
                query->anchors.add(anchor);
            }
        }
    }

    if (query->entities.length + query->anchors.length + query->props.length > 0)
    {
        push_anchor_event(instance, { .type = discovery ? XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_RESULTS_AVAILABLE_META : XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB, .session = xr_session, .request_id = request_id });
    }
    push_anchor_event(instance, { .type = discovery ? XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_COMPLETE_META : XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB, .session = xr_session, .request_id = request_id, .result = XR_SUCCESS });
}

// The session's live space of an entity, as a query or discovery result: apps such as MRUK know
// an entity by its handle and took a new handle for a new entity, so a space per query made the
// keyboard leave and come back every second. Null when the app has none (or destroyed it)
static OXR_SpaceHandle live_space (OXR_SessionHandle session, OXR_SpaceKind kind, const W_Uuid& uuid)
{
    OXR_Runtime* rt = runtime();
    for (usize i = 0; i < rt->spaces.length(); i++)
    {
        const OXR_Space& space = rt->spaces.data()[i];
        if (space.session == session && space.kind == kind && w_uuid_equal(space.uuid, uuid) && (kind != OXR_SpaceKind::Entity || space_entity_fresh(&space)))
        {
            return rt->spaces.handle_at(i);
        }
    }
    return {};
}

// XrSpaceQueryResultFB and XrSpaceDiscoveryResultMETA share this layout
static XrResult retrieve_query (OXR_SessionHandle session, u64 request_id, b8 discovery, u32 capacity, u32* count_output, XrSpaceQueryResultFB* results)
{
    OXR_Runtime* rt = runtime();
    for (u32 q = 0; q < rt->queries_count; q++)
    {
        OXR_Query& query = rt->queries[q];
        if (query.session != session || query.request_id != request_id || query.discovery != discovery)
        {
            continue;
        }
        const u32 entities = (u32)query.entities.length;
        const u32 anchors  = entities + (u32)query.anchors.length;
        const u32 count    = anchors + (u32)query.props.length;
        *count_output      = count;
        if (capacity == 0 && count > 0)
        {
            return XR_SUCCESS;
        }
        OXR_CHECK(capacity == 0 || results);
        if (capacity < count)
        {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        const b8                   fresh    = query.world_generation == rt->world_generation;
        const OXR_Session*         owner    = rt->sessions.try_get(session);
        const OXR_Instance*        instance = owner ? session_instance(owner) : nullptr;
        const u64                  frame    = rt->sim ? sim_frame_index(rt->sim) : 0;
        for (u32 i = 0; i < count; i++)
        {
            results[i] = {};
            if (!fresh || !instance)
            {
                continue;
            }
            if (i < entities)
            {
                if (rt->world)
                {
                    const W_Uuid&   uuid  = w_entity(rt->world, query.entities[i]).uuid;
                    OXR_SpaceHandle space = live_space(session, OXR_SpaceKind::Entity, uuid);
                    space                 = space.value ? space : create_entity_space(session, query.entities[i]);
                    results[i]            = { .space = space_xr(space), .uuid = uuid_to_xr(uuid) };
                }
                continue;
            }
            if (i >= anchors)
            {
                const u32       prop  = query.props[i - anchors];
                OXR_SpaceHandle space = live_space(session, OXR_SpaceKind::Prop, prop_uuid(prop));
                space                 = space.value ? space : create_prop_space(session, prop);
                results[i]            = { .space = space_xr(space), .uuid = uuid_to_xr(prop_uuid(prop)) };
                continue;
            }
            const W_StoredAnchor& anchor = query.anchors[i - entities];
            OXR_SpaceHandle       space  = live_space(session, OXR_SpaceKind::Anchor, anchor.uuid);
            space      = space.value ? space : create_anchor_space(session, anchor.uuid, zm::pose_mul(rt->stage_from_room, anchor.pose), anchor_components(instance), frame + rt->anchor_localize_frames);
            results[i] = { .space = space_xr(space), .uuid = uuid_to_xr(anchor.uuid) };
        }
        remove_query(q);
        return XR_SUCCESS;
    }
    OXR_FAIL("no query of this request id");
}

static void drop_session_requests (OXR_SessionHandle session)
{
    OXR_Runtime* rt = runtime();
    for (u32 q = rt->queries_count; q-- > 0;)
    {
        if (rt->queries[q].session == session)
        {
            remove_query(q);
        }
    }
    for (usize i = rt->futures.length(); i-- > 0;)
    {
        if (rt->futures.data()[i].session == session)
        {
            destroy_future(rt->futures.handle_at(i));
        }
    }
}

static b8 label_recognized (const char* recognized, const char* label)
{
    const usize length = cstr_length(label);
    for (const char* at = recognized; *at;)
    {
        const char* end = at;
        while (*end && *end != ',')
        {
            end++;
        }
        if ((usize)(end - at) == length && memory_equal(at, label, length))
        {
            return true;
        }
        at = *end ? end + 1 : end;
    }
    return false;
}

// Spec XR_FB_scene XrSemanticLabelsSupportInfoFB: without it any label may be returned
static const char* scene_label (W_Label label, const XrSemanticLabelsSupportInfoFB* support)
{
    const char* name = oxr_scene_labels[(u32)label];
    if (!support)
    {
        return label == W_Label::InvisibleWallFace ? "WALL_FACE" : name;
    }
    if (label == W_Label::InvisibleWallFace && !(support->flags & XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_INVISIBLE_WALL_FACE_BIT_FB))
    {
        name = "WALL_FACE";
    }
    if (label == W_Label::Table && !(support->flags & XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_DESK_TO_TABLE_MIGRATION_BIT_FB))
    {
        name = "DESK";
    }
    return label_recognized(support->recognizedLabels, name) ? name : "OTHER";
}

// The serial lives in the store, so uuids of a new run never repeat stored ones
static W_Uuid make_anchor_uuid (OXR_Runtime* rt)
{
    return w_anchor_uuid(rt->app_key, rt->user_id, ++rt->store.next_serial);
}

//
// Anchor stores
//

// Nothing reaches the disk while a replay runs: it would repeat writes of the recorded run
static b8 storage_writable (const OXR_Runtime* rt)
{
    return rt->store_path[0] && !(rt->replay && !rt->replay_finished);
}

static b8 commit_store (const OXR_Runtime* rt)
{
    return !storage_writable(rt) || w_store_save(rt->store_path, rt->store);
}

// Opens the app's store for the simulated user and places the stage in the room for this run
static void open_anchor_store_locked (OXR_Runtime* rt, const SIM_Config& config, const char* application_name)
{
    char anchors_dir[OS_MAX_PATH] = {};
    char cloud_root[OS_MAX_PATH]  = {};
    char state[OS_MAX_PATH]       = {};
    if (config.anchors_dir[0])
    {
        cstr_copy(anchors_dir, sizeof(anchors_dir), config.anchors_dir);
    }
    else if (os_state_dir(state, sizeof(state)))
    {
        cstr_format(anchors_dir, sizeof(anchors_dir), "%s/anchors", state);
    }
    if (config.cloud_dir[0])
    {
        cstr_copy(cloud_root, sizeof(cloud_root), config.cloud_dir);
    }
    else if (anchors_dir[0])
    {
        cstr_format(cloud_root, sizeof(cloud_root), "%s/cloud", anchors_dir);
    }

    w_store_key(config.app_id[0] ? config.app_id : application_name, rt->app_key, sizeof(rt->app_key));
    rt->user_id                = config.user_id;
    rt->anchor_latency_frames  = config.anchor_latency_frames;
    rt->anchor_localize_frames = config.anchor_localize_frames;
    rt->store_path[0]          = 0;
    rt->cloud_anchors_dir[0]   = 0;
    rt->cloud_adverts_dir[0]   = 0;
    if (cloud_root[0])
    {
        cstr_format(rt->cloud_anchors_dir, sizeof(rt->cloud_anchors_dir), "%s/%s/anchors", cloud_root, rt->app_key);
        cstr_format(rt->cloud_adverts_dir, sizeof(rt->cloud_adverts_dir), "%s/%s/adverts", cloud_root, rt->app_key);
    }
    if (anchors_dir[0])
    {
        char path[OS_MAX_PATH];
        cstr_format(path, sizeof(path), "%s/%s/user_%llu.json", anchors_dir, rt->app_key, (unsigned long long)rt->user_id);
        if (w_store_load(path, rt->store))
        {
            cstr_copy(rt->store_path, sizeof(rt->store_path), path);
        }
        else
        {
            LOG_ERROR("Anchors of %s stay unsaved this run so the corrupt store is kept for inspection", path);
        }
    }

    switch (config.origin_offset_mode)
    {
    case SIM_OriginOffset::None:
        rt->room_from_stage = zm::POSE_IDENTITY;
        break;
    case SIM_OriginOffset::Fixed:
        rt->room_from_stage = config.origin_offset;
        break;
    case SIM_OriginOffset::PerRun:
        rt->store.runs++;
        commit_store(rt);
        rt->room_from_stage = w_origin_offset_for_run(config.origin_seed, rt->store.runs);
        break;
    }
    rt->stage_from_room = zm::pose_inverse(rt->room_from_stage);
    if (config.origin_offset_mode != SIM_OriginOffset::None)
    {
        const Vec3 forward = zm::rotate(rt->room_from_stage.orientation, { 0.0f, 0.0f, -1.0f });
        LOG_INFO("Tracking origin at (%.2f, %.2f) in the room, facing %.0f degrees from -Z", (f64)rt->room_from_stage.position.x, (f64)rt->room_from_stage.position.z, (f64)(zm::atan2(-forward.x, -forward.z) * zm::RAD_TO_DEG));
    }
}

static void close_anchor_store_locked (OXR_Runtime* rt)
{
    w_store_release(rt->store);
    rt->store_path[0]   = 0;
    rt->room_from_stage = zm::POSE_IDENTITY;
    rt->stage_from_room = zm::POSE_IDENTITY;
    for (OXR_Fault& fault : rt->anchor_faults)
    {
        fault = {};
    }
    for (OXR_Fault& fault : rt->frame_faults)
    {
        fault = {};
    }
}

//
// Colocation
//

// Advertisements are lock files in the cloud directory, held while their process advertises;
// discovery looks for them every OXR_COLOCATION_POLL_FRAMES.

static void stop_advertising (OXR_Session* session)
{
    w_advert_stop(session->advert_file);
    session->advert_file = nullptr;
    session->advertising = false;
}

// The advertisements nearby, read once per poll while recording and taken from the recording in a replay
static void read_adverts (OXR_Runtime* rt, Array<W_Advert>& out_adverts)
{
    out_adverts.clear();
    if (rt->replay && !rt->replay_finished)
    {
        REC_External external = { .kind = REC_ExternalKind::Adverts };
        defer { rec_external_release(external); };
        if (rt->replay_external < rec_external_count(rt->replay) && rec_read_external(rt->replay, rt->replay_external, external) && external.kind == REC_ExternalKind::Adverts)
        {
            rt->replay_external++;
            out_adverts.add(external.adverts.data, external.adverts.length);
            return;
        }
        LOG_ERROR("Replay: the app looked for colocation advertisements where the recording did not");
        rt->replay_divergences++;
        return;
    }
    w_advert_list(rt->cloud_adverts_dir[0] ? rt->cloud_adverts_dir : nullptr, out_adverts);
    if (rt->recorder)
    {
        const REC_External external = { .kind = REC_ExternalKind::Adverts, .frame_index = sim_frame_index(rt->sim), .adverts = out_adverts };
        rec_writer_append_external(rt->recorder, external);
    }
}

static void poll_colocation (OXR_Runtime* rt, OXR_Session* session, OXR_SessionHandle handle)
{
    const u64 frame = sim_frame_index(rt->sim);
    if (!session->discovering || frame < session->discovery_next_poll)
    {
        return;
    }
    session->discovery_next_poll = frame + OXR_COLOCATION_POLL_FRAMES;
    OXR_Instance*   inst    = session_instance(session);
    Array<W_Advert> adverts = {};
    defer { adverts.release(); };
    read_adverts(rt, adverts);
    for (const W_Advert& advert : adverts)
    {
        b8 known = session->advertising && w_uuid_equal(advert.uuid, session->advert_uuid);
        for (const W_Uuid& uuid : session->discovered)
        {
            known = known || w_uuid_equal(uuid, advert.uuid);
        }
        if (known || !inst)
        {
            continue;
        }
        session->discovered.add(advert.uuid);
        inst->advert_payloads.add(advert);
        push_event(inst, { .type = XR_TYPE_EVENT_DATA_COLOCATION_DISCOVERY_RESULT_META, .session = session_xr(handle), .request_id = session->discovery_request, .uuid = advert.uuid, .advert = (u32)inst->advert_payloads.length - 1 });
    }
}

//
// Record and replay
//

static b8 record_start_locked (OXR_Runtime* rt, const char* path)
{
    if (!rt->sim || !path || !path[0])
    {
        return false;
    }
    if (rt->recorder)
    {
        LOG_WARNING("Already recording to %s", rt->record_path);
        return false;
    }
    SIM_Snapshot initial = {};
    sim_capture_snapshot(rt->sim, initial);
    REC_Header header = {};
    rec_header_from_config(sim_get_config(rt->sim), initial, header);
    header.controller                = sim_get_controller(rt->sim);
    header.skeleton_change_period_ns = sim_get_skeleton_change_period(rt->sim);
    cstr_copy(header.room, sizeof(header.room), rt->room);
    header.room_hash              = rt->world ? w_content_hash(rt->world) : 0;
    header.room_from_stage        = rt->room_from_stage;
    header.anchor_latency_frames  = rt->anchor_latency_frames;
    header.anchor_localize_frames = rt->anchor_localize_frames;
    for (u32 i = 0; i < (u32)OXR_AnchorOp::Count; i++)
    {
        header.anchor_faults[i] = { .result = rt->anchor_faults[i].result, .remaining = rt->anchor_faults[i].remaining };
    }
    for (u32 i = 0; i < (u32)OXR_FrameOp::Count; i++)
    {
        header.frame_faults[i] = { .result = rt->frame_faults[i].result, .remaining = rt->frame_faults[i].remaining };
    }
    rt->recorder = rec_writer_create(path, header, &rt->store);
    if (!rt->recorder)
    {
        LOG_ERROR("Cannot start recording to %s", path);
        return false;
    }
    cstr_copy(rt->record_path, sizeof(rt->record_path), path);
    LOG_INFO("Recording to %s", path);
    return true;
}

static b8 record_stop_locked (OXR_Runtime* rt)
{
    if (!rt->recorder)
    {
        return false;
    }
    const u64 frames = rec_writer_frame_count(rt->recorder);
    const b8  ok     = rec_writer_close(rt->recorder);
    rt->recorder     = nullptr;
    if (ok)
    {
        LOG_INFO("Recorded %llu frames to %s", (unsigned long long)frames, rt->record_path);
    }
    else
    {
        LOG_ERROR("Recording to %s failed, the file may be incomplete", rt->record_path);
    }
    return ok;
}

// After a replay the store on disk is the truth again, not the recorded one
static void reload_store_locked (OXR_Runtime* rt)
{
    if (rt->store_path[0] && !w_store_load(rt->store_path, rt->store))
    {
        rt->store_path[0] = 0;
    }
}

static void replay_stop_locked (OXR_Runtime* rt)
{
    if (!rt->replay)
    {
        return;
    }
    rec_release(rt->replay);
    rt->replay = nullptr;
    if (rt->sim)
    {
        sim_set_controls_enabled(rt->sim, true);
    }
    reload_store_locked(rt);
}

static void replay_begin_locked (OXR_Runtime* rt, REC_Recording* recording)
{
    replay_stop_locked(rt);
    rt->replay                  = recording;
    rt->replay_cursor           = 0;
    rt->replay_finished         = false;
    rt->replay_divergences      = 0;
    rt->replay_first_divergence = 0;
    const REC_Header& header = rec_header(recording);
    sim_set_controller(rt->sim, header.controller);
    sim_set_skeleton_change_period(rt->sim, header.skeleton_change_period_ns);
    sim_restore_snapshot(rt->sim, header.initial);
    sim_set_controls_enabled(rt->sim, false);

    // Anchors replay against the recorded store, which nothing writes back during the replay
    const W_Store& store = rec_store(recording);
    w_store_release(rt->store);
    rt->store.next_serial = store.next_serial;
    rt->store.runs        = store.runs;
    rt->store.anchors.add(store.anchors.data, store.anchors.length);
    rt->room_from_stage        = header.room_from_stage;
    rt->stage_from_room        = zm::pose_inverse(header.room_from_stage);
    rt->anchor_latency_frames  = header.anchor_latency_frames;
    rt->anchor_localize_frames = header.anchor_localize_frames;
    rt->replay_external        = 0;
    for (u32 i = 0; i < (u32)OXR_AnchorOp::Count; i++)
    {
        rt->anchor_faults[i] = { .result = (XrResult)header.anchor_faults[i].result, .remaining = header.anchor_faults[i].remaining };
    }
    for (u32 i = 0; i < (u32)OXR_FrameOp::Count; i++)
    {
        rt->frame_faults[i] = { .result = (XrResult)header.frame_faults[i].result, .remaining = header.frame_faults[i].remaining };
    }
    LOG_INFO("Replaying %llu frames%s", (unsigned long long)rec_frame_count(recording), rec_is_complete(recording) ? "" : " (recording is incomplete)");
}

// Virtual keyboard (XR_META_virtual_keyboard) state changes; the xr* functions follow the render models

#define OXR_VKB_NEAR_FRONT  0.06f // a direct input this close in front of the keys silences its side's ray
#define OXR_VKB_NEAR_BEHIND 0.05f
#define OXR_VKB_NEAR_MARGIN 0.02f

static void vkb_event (OXR_Instance* inst, const OXR_VirtualKeyboard* keyboard, XrVirtualKeyboardMETA handle, XrStructureType type, const char* text, usize length)
{
    OXR_Event event = { .type = type, .session = session_xr(keyboard->session) };
    event.keyboard  = handle;
    memory_copy(event.text, text, length);
    push_event(inst, event);
}

// Events hold OXR_MAX_EVENT_TEXT - 1 bytes; longer text goes in pieces cut between UTF-8 sequences
static void vkb_commit (OXR_Instance* inst, const OXR_VirtualKeyboard* keyboard, XrVirtualKeyboardMETA handle, const char* text, usize length)
{
    while (length > 0)
    {
        usize piece = length < OXR_MAX_EVENT_TEXT - 1 ? length : OXR_MAX_EVENT_TEXT - 1;
        while (piece < length && piece > 1 && ((u8)text[piece] & 0xC0) == 0x80)
        {
            piece--;
        }
        vkb_event(inst, keyboard, handle, XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_COMMIT_TEXT_META, text, piece);
        text   += piece;
        length -= piece;
    }
}

static void vkb_set_shifted (OXR_VirtualKeyboard* keyboard, b8 shifted)
{
    keyboard->texture_dirty |= keyboard->shifted != shifted;
    keyboard->shifted        = shifted;
}

// Hiding drops every press; the runtime resets the keyboard while it is not shown
static void vkb_set_visible (OXR_Instance* inst, OXR_VirtualKeyboard* keyboard, XrVirtualKeyboardMETA handle, b8 visible)
{
    if (keyboard->visible == visible)
    {
        return;
    }
    keyboard->visible = visible;
    for (u32 source = 0; source < OXR_VKB_SOURCES; source++)
    {
        keyboard->pressing[source]    = -1;
        keyboard->was_pressed[source] = false;
    }
    for (u32 k = 0; k < keyboard->layout.keys_count; k++)
    {
        keyboard->changed_keys |= keyboard->held[k] ? 1ull << k : 0;
        keyboard->held[k]       = false;
    }
    vkb_set_shifted(keyboard, false);
    vkb_event(inst, keyboard, handle, visible ? XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_SHOWN_META : XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_HIDDEN_META, nullptr, 0);
}

static void vkb_press_key (OXR_Instance* inst, OXR_VirtualKeyboard* keyboard, XrVirtualKeyboardMETA handle, u32 index)
{
    const SIM_VkbKey& key = keyboard->layout.keys[index];
    switch (key.kind)
    {
    case SIM_VkbKeyKind::Char:
    {
        const char c = keyboard->shifted ? key.upper : key.lower;
        vkb_commit(inst, keyboard, handle, &c, 1);
        vkb_set_shifted(keyboard, false);
        return;
    }
    case SIM_VkbKeyKind::Space:
        vkb_commit(inst, keyboard, handle, " ", 1);
        return;
    case SIM_VkbKeyKind::Backspace:
        vkb_event(inst, keyboard, handle, XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_BACKSPACE_META, nullptr, 0);
        return;
    case SIM_VkbKeyKind::Enter:
        vkb_event(inst, keyboard, handle, XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_ENTER_META, nullptr, 0);
        return;
    case SIM_VkbKeyKind::Shift:
        vkb_set_shifted(keyboard, !keyboard->shifted);
        return;
    }
}

// Text typed through the simulator goes to the session's keyboard while it is shown: runs of
// characters as one commit, \b as backspace, \n as enter
static void deliver_typed_text (OXR_Runtime* rt, const OXR_Session* sess, OXR_SessionHandle handle)
{
    u32         length = 0;
    const char* text   = sim_typed_text(rt->sim, length);
    if (length == 0)
    {
        return;
    }
    for (usize i = 0; i < rt->keyboards.length(); i++)
    {
        const OXR_VirtualKeyboard* keyboard = &rt->keyboards.data()[i];
        if (keyboard->session != handle || !keyboard->visible)
        {
            continue;
        }
        OXR_Instance*               inst = session_instance(sess);
        const XrVirtualKeyboardMETA xr   = keyboard_xr(rt->keyboards.handle_at(i));
        u32                         run  = 0;
        for (u32 c = 0; c <= length; c++)
        {
            if (c < length && text[c] != '\b' && text[c] != '\n')
            {
                continue;
            }
            vkb_commit(inst, keyboard, xr, text + run, c - run);
            if (c < length)
            {
                vkb_event(inst, keyboard, xr, text[c] == '\b' ? XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_BACKSPACE_META : XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_ENTER_META, nullptr, 0);
            }
            run = c + 1;
        }
        return;
    }
    LOG_WARNING("Typed text dropped: the app shows no virtual keyboard");
}

// Advances the simulator by one frame, from the replay while it has frames, live otherwise
static void advance_frame (OXR_Runtime* rt, OXR_Session* sess, OXR_SessionHandle handle, u64 now)
{
    b8 replayed = false;
    if (rt->replay && !rt->replay_finished)
    {
        u64            recorded_frame = 0;
        u64            recorded_hash  = 0;
        SIM_FrameState state = {};
        if (rt->replay_cursor < rec_frame_count(rt->replay) && rec_read_frame(rt->replay, rt->replay_cursor, recorded_frame, recorded_hash, state))
        {
            sim_replay_frame(rt->sim, state);
            sim_commit_frame(rt->sim);
            rt->replay_cursor++;
            replayed = true;

            const u64 frame = sim_frame_index(rt->sim);
            if (sim_state_hash(rt->sim) != recorded_hash || frame != recorded_frame)
            {
                if (rt->replay_divergences == 0)
                {
                    rt->replay_first_divergence = frame;
                    LOG_ERROR("Replay diverged from the recording at frame %llu", (unsigned long long)frame);
                }
                rt->replay_divergences++;
            }
        }
        else
        {
            rt->replay_finished = true;
            sim_set_controls_enabled(rt->sim, true);
            reload_store_locked(rt);
            LOG_INFO("Replay finished after %llu frames, %u diverged", (unsigned long long)rt->replay_cursor, rt->replay_divergences);
            if (sim_get_config(rt->sim).replay_exit && sess->running && !sess->exit_requested)
            {
                request_exit(sess, handle);
            }
        }
    }

    if (!replayed)
    {
        sim_begin_frame(rt->sim, now);
        sim_commit_frame(rt->sim);
    }
    deliver_typed_text(rt, sess, handle);

    if (rt->recorder)
    {
        SIM_FrameState state = {};
        sim_capture_state(rt->sim, state);
        if (!rec_writer_append(rt->recorder, sim_frame_index(rt->sim), sim_state_hash(rt->sim), state))
        {
            LOG_ERROR("Writing the recording failed, stopping it");
            record_stop_locked(rt);
        }
    }
}

static void request_exit (OXR_Session* session, OXR_SessionHandle handle)
{
    session->exit_requested      = true;
    session->synchronize_pending = false;
    if (session->state == XR_SESSION_STATE_READY)
    {
        session_set_state(session, handle, XR_SESSION_STATE_SYNCHRONIZED);
    }
    if (session->state == XR_SESSION_STATE_FOCUSED)
    {
        session_set_state(session, handle, XR_SESSION_STATE_VISIBLE);
    }
    if (session->state == XR_SESSION_STATE_VISIBLE)
    {
        session_set_state(session, handle, XR_SESSION_STATE_SYNCHRONIZED);
    }
    session_set_state(session, handle, XR_SESSION_STATE_STOPPING);
}

// Events that name a destroyed session must not reach the app
static void drop_session_events (OXR_Instance* instance, XrSession session)
{
    u32 kept = 0;
    for (u32 i = 0; i < instance->events_count; i++)
    {
        const OXR_Event& event = instance->events[(instance->events_head + i) % OXR_MAX_EVENTS];
        if (event.session != session)
        {
            instance->events[(instance->events_head + kept) % OXR_MAX_EVENTS] = event;
            kept++;
        }
    }
    instance->events_count = kept;

    kept = 0;
    for (u32 i = 0; i < instance->delayed_count; i++)
    {
        if (instance->delayed[i].session != session)
        {
            instance->delayed[kept]        = instance->delayed[i];
            instance->delayed_frames[kept] = instance->delayed_frames[i];
            kept++;
        }
    }
    instance->delayed_count = kept;
}

// XR_META_passthrough_color_lut

static OXR_PassthroughColorLut* find_color_lut (XrPassthroughColorLutMETA lut)
{
    OXR_PassthroughColorLut* found = runtime()->color_luts.try_get(color_lut_handle(lut));
    return found && !found->destroyed ? found : nullptr;
}

static void free_color_lut (OXR_PassthroughColorLutHandle handle)
{
    OXR_Runtime*             rt  = runtime();
    const OXR_PassthroughColorLut* lut = rt->color_luts.try_get(handle);
    if (lut)
    {
        os_memory_release(lut->rgba);
        rt->color_luts.remove(handle);
    }
}

// Spec: a destroyed LUT stays applied to the layers using it until their style changes
static void destroy_color_lut (OXR_PassthroughColorLutHandle handle)
{
    OXR_PassthroughColorLut* lut = runtime()->color_luts.try_get(handle);
    if (!lut)
    {
        return;
    }
    lut->destroyed = true;
    if (lut->users == 0)
    {
        free_color_lut(handle);
    }
}

static void set_layer_color_luts (OXR_PassthroughLayer& layer, OXR_PassthroughColorLutHandle first, OXR_PassthroughColorLutHandle second)
{
    OXR_Runtime*                        rt      = runtime();
    const OXR_PassthroughColorLutHandle uses[2] = { first, second };
    for (const OXR_PassthroughColorLutHandle use : uses)
    {
        OXR_PassthroughColorLut* lut = rt->color_luts.try_get(use);
        if (lut)
        {
            lut->users++;
        }
    }
    for (const OXR_PassthroughColorLutHandle used : layer.color_luts)
    {
        OXR_PassthroughColorLut* lut = rt->color_luts.try_get(used);
        if (lut && --lut->users == 0 && lut->destroyed)
        {
            free_color_lut(used);
        }
    }
    layer.color_luts[0] = first;
    layer.color_luts[1] = second;
}

static void destroy_session (OXR_SessionHandle handle)
{
    OXR_Runtime* rt      = runtime();
    OXR_Session* session = rt->sessions.try_get(handle);
    if (!session)
    {
        return;
    }
    OXR_Instance* events_owner = rt->instances.try_get(session->instance);
    if (events_owner)
    {
        drop_session_events(events_owner, session_xr(handle));
    }

    for (usize i = rt->spaces.length(); i-- > 0;)
    {
        if (rt->spaces.data()[i].session == handle)
        {
            rt->spaces.remove(rt->spaces.handle_at(i));
        }
    }
    drop_session_requests(handle);
    for (usize i = rt->geometry_instances.length(); i-- > 0;)
    {
        if (rt->geometry_instances.data()[i].session == handle)
        {
            rt->geometry_instances.remove(rt->geometry_instances.handle_at(i));
        }
    }
    for (usize i = rt->triangle_meshes.length(); i-- > 0;)
    {
        if (rt->triangle_meshes.data()[i].session == handle)
        {
            destroy_triangle_mesh(rt->triangle_meshes.handle_at(i));
        }
    }
    for (usize i = rt->hand_trackers.length(); i-- > 0;)
    {
        if (rt->hand_trackers.data()[i].session == handle)
        {
            rt->hand_trackers.remove(rt->hand_trackers.handle_at(i));
        }
    }
    for (usize i = rt->body_trackers.length(); i-- > 0;)
    {
        if (rt->body_trackers.data()[i].session == handle)
        {
            rt->body_trackers.remove(rt->body_trackers.handle_at(i));
        }
    }
    SparseArray<OXR_FaceTracker>* const face_trackers[2] = { &rt->face_trackers, &rt->face_trackers2 };
    for (SparseArray<OXR_FaceTracker>* trackers : face_trackers)
    {
        for (usize i = trackers->length(); i-- > 0;)
        {
            if (trackers->data()[i].session == handle)
            {
                trackers->remove(trackers->handle_at(i));
            }
        }
    }
    for (usize i = rt->eye_trackers.length(); i-- > 0;)
    {
        if (rt->eye_trackers.data()[i].session == handle)
        {
            rt->eye_trackers.remove(rt->eye_trackers.handle_at(i));
        }
    }
    for (usize i = rt->keyboards.length(); i-- > 0;)
    {
        if (rt->keyboards.data()[i].session == handle)
        {
            rt->keyboards.remove(rt->keyboards.handle_at(i));
        }
    }
    for (usize i = rt->dynamic_trackers.length(); i-- > 0;)
    {
        if (rt->dynamic_trackers.data()[i].session == handle)
        {
            rt->dynamic_trackers.remove(rt->dynamic_trackers.handle_at(i));
        }
    }
    for (usize i = rt->foveation_profiles.length(); i-- > 0;)
    {
        if (rt->foveation_profiles.data()[i].session == handle)
        {
            rt->foveation_profiles.remove(rt->foveation_profiles.handle_at(i));
        }
    }
    for (usize i = rt->spatial_contexts.length(); i-- > 0;)
    {
        if (rt->spatial_contexts.data()[i].session == handle)
        {
            destroy_spatial_context(rt->spatial_contexts.handle_at(i));
        }
    }
    session->spatial_texts.release();
    for (usize i = rt->render_models.length(); i-- > 0;)
    {
        if (rt->render_models.data()[i].session == handle)
        {
            rt->render_models.remove(rt->render_models.handle_at(i));
        }
    }
    for (usize i = rt->render_model_assets.length(); i-- > 0;)
    {
        if (rt->render_model_assets.data()[i].session == handle)
        {
            rt->render_model_assets.data()[i].glb.release();
            rt->render_model_assets.remove(rt->render_model_assets.handle_at(i));
        }
    }
    for (usize i = rt->color_luts.length(); i-- > 0;)
    {
        if (rt->color_luts.data()[i].session == handle)
        {
            free_color_lut(rt->color_luts.handle_at(i));
        }
    }
    for (usize i = rt->passthrough_layers.length(); i-- > 0;)
    {
        if (rt->passthrough_layers.data()[i].session == handle)
        {
            rt->passthrough_layers.remove(rt->passthrough_layers.handle_at(i));
        }
    }
    rt->passthroughs.remove(session->passthrough);
    for (usize i = rt->depth_swapchains.length(); i-- > 0;)
    {
        if (rt->depth_swapchains.data()[i].session == handle)
        {
            destroy_depth_swapchain(rt->depth_swapchains.handle_at(i));
        }
    }
    rt->depth_providers.remove(session->depth_provider);
    for (usize i = rt->camera_providers.length(); i-- > 0;)
    {
        if (rt->camera_providers.data()[i].session == handle)
        {
            destroy_camera_provider(rt->camera_providers.handle_at(i));
        }
    }
    for (usize i = rt->raycasters.length(); i-- > 0;)
    {
        if (rt->raycasters.data()[i].session == handle)
        {
            rt->raycasters.remove(rt->raycasters.handle_at(i));
        }
    }
    for (usize i = rt->space_users.length(); i-- > 0;)
    {
        if (rt->space_users.data()[i].session == handle)
        {
            rt->space_users.remove(rt->space_users.handle_at(i));
        }
    }
    for (usize i = rt->swapchains.length(); i-- > 0;)
    {
        if (rt->swapchains.data()[i].session == handle)
        {
            destroy_swapchain(rt->swapchains.handle_at(i));
        }
    }
#if !PUPPETXR_HEADLESS_ONLY
    if (session->ui)
    {
        os_set_raw_event_hook(nullptr, nullptr);
        ui_destroy(session->ui);
    }
#endif
    if (session->device)
    {
        r_destroy_window_swapchain(session->device, session->window_swapchain);
    }
    // Apps destroy their VkInstance after the session but not necessarily after the XrInstance
    OXR_Instance* owner = rt->instances.try_get(session->instance);
    if (owner && session->window_swapchain)
    {
        oxr_vk_release_surface(owner);
    }
    session->window_swapchain = {};
    comp_destroy(session->compositor);
    r_destroy_device(session->device);
    session->ui         = nullptr;
    session->compositor = nullptr;
    session->device     = nullptr;
    for (const OXR_ActionSetHandle& set_handle : session->attached_sets)
    {
        OXR_ActionSet* set = rt->action_sets.try_get(set_handle);
        if (set)
        {
            set->attached = false;
            set->active   = false;
        }
    }
    session->attached_sets.release();
    stop_advertising(session);
    session->discovered.release();
    rt->sessions.remove(handle);
    // An editor keeps its instance between play sessions; the next session's first frame shows
    // the window again, and a close or quit that ended this session leaves the next one running
    if (rt->sessions.length() == 0)
    {
        if (rt->window && rt->window_shown)
        {
            os_window_hide(rt->window);
            rt->window_shown = false;
        }
        rt->window_closed  = false;
        rt->exit_requested = false;
    }
}

static void destroy_action_set (OXR_ActionSetHandle handle)
{
    OXR_Runtime* rt = runtime();
    for (usize i = rt->actions.length(); i-- > 0;)
    {
        if (rt->actions.data()[i].set == handle)
        {
            rt->actions.remove(rt->actions.handle_at(i));
        }
    }
    rt->action_sets.remove(handle);
}

// True when it was the live instance: the caller shuts the platform down after the lock
static b8 destroy_instance (OXR_InstanceHandle handle)
{
    OXR_Runtime*  rt       = runtime();
    OXR_Instance* instance = rt->instances.try_get(handle);
    if (!instance)
    {
        return false;
    }
    if (g_control_hooks.instance_destroyed && rt->live_instance == handle)
    {
        g_control_hooks.instance_destroyed();
    }

    for (usize i = rt->sessions.length(); i-- > 0;)
    {
        if (rt->sessions.data()[i].instance == handle)
        {
            destroy_session(rt->sessions.handle_at(i));
        }
    }
    for (usize i = rt->action_sets.length(); i-- > 0;)
    {
        if (rt->action_sets.data()[i].instance == handle)
        {
            destroy_action_set(rt->action_sets.handle_at(i));
        }
    }

    for (usize i = rt->event_channels.length(); i-- > 0;)
    {
        if (rt->event_channels.data()[i].instance == handle)
        {
            destroy_event_channel(rt->event_channels.handle_at(i));
        }
    }

    oxr_vk_release_surface(instance);
    instance->path_chars.release();
    instance->path_offsets.release();
    instance->path_lookup.release();
    instance->advert_payloads.release();
    for (u32 i = 0; i < (u32)OXR_Profile::Count; i++)
    {
        instance->suggested[i].release();
        instance->modifiers[i].release();
    }
    rt->instances.remove(handle);

    if (rt->live_instance == handle)
    {
        rt->live_instance = {};
        record_stop_locked(rt);
        replay_stop_locked(rt);
        sim_destroy(rt->sim);
        rt->sim = nullptr;
        close_anchor_store_locked(rt);
        load_room_locked(rt, "", 0);
        os_capture_output_end();
        os_destroy_window(rt->window);
        rt->window = nullptr;
        os_profile_stop();
        return true;
    }
    return false;
}

//
// Public API: runtime access
//

OXR_Runtime* oxr_get_runtime ()
{
    return runtime();
}

b8 oxr_release_runtime ()
{
    OXR_Runtime* rt = g_runtime;
    if (!rt)
    {
        return true;
    }
    if (rt->instances.length() > 0)
    {
        return false;
    }
    rt->instances.release();
    rt->sessions.release();
    rt->spaces.release();
    rt->swapchains.release();
    rt->action_sets.release();
    rt->actions.release();
    rt->futures.release();
    rt->passthroughs.release();
    rt->passthrough_layers.release();
    for (const OXR_PassthroughColorLut& lut : rt->color_luts)
    {
        os_memory_release(lut.rgba);
    }
    rt->color_luts.release();
    rt->depth_providers.release();
    rt->depth_swapchains.release();
    rt->camera_providers.release();
    rt->raycasters.release();
    rt->space_users.release();
    rt->triangle_meshes.release();
    rt->geometry_instances.release();
    rt->hand_trackers.release();
    rt->body_trackers.release();
    rt->face_trackers.release();
    rt->face_trackers2.release();
    rt->eye_trackers.release();
    rt->keyboards.release();
    rt->dynamic_trackers.release();
    rt->event_channels.release();
    rt->foveation_profiles.release();
    rt->spatial_contexts.release();
    rt->spatial_entities.release();
    rt->spatial_snapshots.release();
    rt->render_models.release();
    for (usize i = 0; i < rt->render_model_assets.length(); i++)
    {
        rt->render_model_assets.data()[i].glb.release();
    }
    rt->render_model_assets.release();
    os_mutex_destroy(rt->mutex);
    os_memory_release(rt);
    g_runtime = nullptr;
    os_release();
    return true;
}

// The loader unloads the runtime after the last xrDestroyInstance; without this the global
// state would leak on every load. Instances still alive at process exit are left alone. A
// static destructor runs at dlclose and at FreeLibrary alike; on Windows it runs under the
// loader lock, so it only frees memory: threads and windows end with the last instance.
namespace
{

struct ReleaseRuntimeOnUnload
{
    ~ReleaseRuntimeOnUnload ()
    {
        oxr_release_runtime();
    }
};

} // namespace

static const ReleaseRuntimeOnUnload release_runtime_on_unload;

SIM_Sim* oxr_get_sim ()
{
    return runtime()->sim;
}

void oxr_set_control_hooks (const OXR_ControlHooks& hooks)
{
    g_control_hooks = hooks;
}

// Until the first frame with layers, the time spent holding the lock is puppetxr's share of startup
void oxr_lock ()
{
    {
        PROF_ZONE("runtime lock wait");
        os_mutex_lock(runtime()->mutex);
    }
    if (oxr_startup.lock_depth++ == 0)
    {
        const u64 now          = os_monotonic_ns();
        runtime()->locked_since = now;
        if (oxr_startup.origin && !oxr_startup.logged)
        {
            oxr_startup.entered = now;
        }
    }
}

void oxr_unlock ()
{
    if (--oxr_startup.lock_depth == 0)
    {
        const u64 now = os_monotonic_ns();
        OXR_Runtime* rt = runtime();
        rt->frame_locked_ns += now - rt->locked_since;
        if (oxr_startup.entered)
        {
            oxr_startup.in_calls += now - oxr_startup.entered;
            oxr_startup.entered   = 0;
        }
    }
    os_mutex_unlock(runtime()->mutex);
}

static void startup_mark (u64& milestone)
{
    if (!milestone && oxr_startup.origin)
    {
        milestone = os_monotonic_ns() - oxr_startup.origin;
    }
}

static f64 startup_ms (u64 ns)
{
    return (f64)ns * 1e-6;
}

// Once a run, at the first frame with layers: how long since the first instance, and how
// much of that puppetxr's calls took, so a slow start is known to be the app's or ours
static void startup_log ()
{
    if (oxr_startup.logged || !oxr_startup.origin)
    {
        return;
    }
    oxr_startup.logged = true;
    const u64 total    = os_monotonic_ns() - oxr_startup.origin;
    LOG_INFO("Startup: first frame with layers %.0f ms after the first xrCreateInstance; puppetxr's calls took %.0f ms of it, the app the rest",
             startup_ms(total), startup_ms(oxr_startup.in_calls));
    LOG_INFO("Startup in puppetxr: rooms %.0f, room upload %.0f, window %.0f, graphics device %.0f, compositor %.0f, UI %.0f ms",
             startup_ms(oxr_startup.room), startup_ms(oxr_startup.room_upload), startup_ms(oxr_startup.window), startup_ms(oxr_startup.device), startup_ms(oxr_startup.compositor), startup_ms(oxr_startup.ui));
    LOG_INFO("Startup milestones: session created at %.0f ms, running at %.0f, first xrWaitFrame at %.0f, first xrEndFrame at %.0f",
             startup_ms(oxr_startup.session_created), startup_ms(oxr_startup.session_running), startup_ms(oxr_startup.first_wait), startup_ms(oxr_startup.first_end));
}

static COMP_Compositor* first_compositor ()
{
    for (const OXR_Session& session : runtime()->sessions)
    {
        if (session.compositor)
        {
            return session.compositor;
        }
    }
    return nullptr;
}

// A read off the GPU happens on the calling thread unless the control server is on; then it comes
// from the snapshot the frame's xrEndFrame took
static void note_frame_read ()
{
    OXR_Runtime* rt = runtime();
    if (!rt->sim || !sim_get_config(rt->sim).control)
    {
        rt->frame_read_thread = os_thread_current_id();
    }
}

b8 oxr_read_frame (u8* out_rgba, usize out_size, u32& out_width, u32& out_height)
{
    const OXR_Lock lock(__func__);
    const COMP_Compositor* compositor = first_compositor();
    out_width  = 0;
    out_height = 0;
    if (!compositor)
    {
        return false;
    }
    comp_target_size(compositor, out_width, out_height);
    if (out_size == 0)
    {
        return true;
    }
    note_frame_read();
    return comp_read_target(compositor, out_rgba, out_size);
}

b8 oxr_read_mirror_frame (u8* out_rgba, usize out_size, u32& out_width, u32& out_height)
{
    const OXR_Lock lock(__func__);
    const COMP_Compositor* compositor = first_compositor();
    out_width  = 0;
    out_height = 0;
    if (!compositor)
    {
        return false;
    }
    comp_target_size(compositor, out_width, out_height);
    if (out_size == 0)
    {
        return true;
    }
    note_frame_read();
    return comp_read_mirror(compositor, out_rgba, out_size);
}

u64 oxr_frame_read_thread ()
{
    const OXR_Lock lock(__func__);
    return runtime()->frame_read_thread;
}

b8 oxr_ui_click (const char* label)
{
#if PUPPETXR_HEADLESS_ONLY
    UNUSED(label);
    return false;
#else
    const OXR_Lock     lock(__func__);
    const OXR_Runtime* rt = runtime();
    for (const OXR_Session& session : rt->sessions)
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        if (!session.ui || !label || !ui_find_item(session.ui, label, x, y))
        {
            continue;
        }
        // ImGui takes one button change a frame, so the press and release land in two frames
        return os_push_mouse_event(rt->window, x, y, OS_Key::None, false) && os_push_mouse_event(rt->window, x, y, OS_Key::MouseLeft, true) && os_push_mouse_event(rt->window, x, y, OS_Key::MouseLeft, false);
    }
    return false;
#endif
}

b8 oxr_ui_find (const char* label, f32& out_x, f32& out_y, b8& out_hovered, f32* out_mouse)
{
    out_hovered = false;
#if PUPPETXR_HEADLESS_ONLY
    UNUSED(label);
    UNUSED(out_x);
    UNUSED(out_y);
    UNUSED(out_mouse);
    return false;
#else
    const OXR_Lock lock(__func__);
    for (const OXR_Session& session : runtime()->sessions)
    {
        if (session.ui && label && ui_find_item(session.ui, label, out_x, out_y, &out_hovered))
        {
            if (out_mouse)
            {
                ui_probed_mouse(session.ui, out_mouse[0], out_mouse[1]);
            }
            return true;
        }
    }
    return false;
#endif
}

void oxr_set_lens_mask (b8 on)
{
    const OXR_Lock lock(__func__);
    set_lens_mask_locked(runtime(), on);
}

b8 oxr_mouse_captured ()
{
    const OXR_Lock lock(__func__);
    return runtime()->mouse_captured;
}

void oxr_show_ui (b8 shown)
{
    const OXR_Lock lock(__func__);
    runtime()->show_ui = shown;
}

b8 oxr_record_start (const char* path)
{
    const OXR_Lock lock(__func__);
    return record_start_locked(runtime(), path);
}

b8 oxr_record_stop ()
{
    const OXR_Lock lock(__func__);
    return record_stop_locked(runtime());
}

b8 oxr_replay_start (const char* path)
{
    const OXR_Lock     lock(__func__);
    OXR_Runtime* rt = runtime();
    if (!rt->sim)
    {
        return false;
    }
    REC_Recording* recording = rec_load(path);
    if (!recording)
    {
        return false;
    }
    const REC_Header& header = rec_header(recording);
    const SIM_Config& config = sim_get_config(rt->sim);
    if (header.eye_width != config.view_width || header.eye_height != config.view_height || header.display_period_ns != config.display_period_ns)
    {
        LOG_ERROR("Cannot replay %s on this instance: recorded at %ux%u, %lld ns per frame; running %ux%u, %lld ns", path, header.eye_width, header.eye_height, (long long)header.display_period_ns, config.view_width, config.view_height, (long long)config.display_period_ns);
        rec_release(recording);
        return false;
    }
    const b8 other_room = !cstr_equal(header.room, rt->room) || (rt->world && w_content_hash(rt->world) != header.room_hash);
    if (other_room && !load_room_locked(rt, header.room, header.room_hash))
    {
        LOG_ERROR("Cannot replay %s: its room '%s' is missing or changed", path, header.room);
        rec_release(recording);
        return false;
    }
    replay_begin_locked(rt, recording);
    return true;
}

void oxr_replay_stop ()
{
    const OXR_Lock lock(__func__);
    replay_stop_locked(runtime());
}

OXR_RecordStatus oxr_record_status ()
{
    const OXR_Lock           lock(__func__);
    const OXR_Runtime* rt = runtime();
    OXR_RecordStatus   status = {};
    status.recording          = rt->recorder != nullptr;
    status.recorded_frames    = rt->recorder ? rec_writer_frame_count(rt->recorder) : 0;
    if (rt->replay)
    {
        status.replaying        = !rt->replay_finished;
        status.replay_finished  = rt->replay_finished;
        status.replay_frame     = rt->replay_cursor;
        status.replay_total     = rec_frame_count(rt->replay);
        status.replay_complete  = rec_is_complete(rt->replay);
        status.divergences      = rt->replay_divergences;
        status.first_divergence = rt->replay_first_divergence;
    }
    return status;
}

static b8 set_controller_locked (const OXR_Runtime* rt, SIM_Controller controller)
{
    if (!rt->sim || (u32)controller >= (u32)SIM_Controller::Count)
    {
        return false;
    }
    if (rt->recorder || (rt->replay && !rt->replay_finished))
    {
        LOG_WARNING("The controller type cannot change while recording or replaying");
        return false;
    }
    if (controller != sim_get_controller(rt->sim))
    {
        sim_set_controller(rt->sim, controller);
        LOG_INFO("Simulating %s controllers", sim_controller_name(controller));
    }
    return true;
}

b8 oxr_set_controller (SIM_Controller controller)
{
    const OXR_Lock lock(__func__);
    return set_controller_locked(runtime(), controller);
}

static b8 state_path (char* out_path, usize size)
{
    char dir[OS_MAX_PATH];
    return os_state_dir(dir, sizeof(dir)) && cstr_format(out_path, size, "%s/state.json", dir) < size;
}

static OXR_RoomResult set_room_locked (OXR_Runtime* rt, const char* name)
{
    if (!rt->sim)
    {
        return OXR_RoomResult::NoInstance;
    }
    if (rt->recorder || (rt->replay && !rt->replay_finished))
    {
        LOG_WARNING("The room cannot change while recording or replaying");
        return OXR_RoomResult::Busy;
    }
    if (name && name[0] && !w_room_name_valid(name))
    {
        return OXR_RoomResult::NotFound;
    }
    return load_room_locked(rt, name, 0) ? OXR_RoomResult::Ok : OXR_RoomResult::NotFound;
}

static OXR_RoomResult choose_room_locked (OXR_Runtime* rt, const char* name)
{
    const OXR_RoomResult result = set_room_locked(rt, name);
    char                 path[OS_MAX_PATH];
    if (result != OXR_RoomResult::Ok || !state_path(path, sizeof(path)))
    {
        return result;
    }
    SIM_State state = {};
    sim_load_state(path, state);
    state.has_room = true;
    cstr_copy(state.room, sizeof(state.room), rt->room);
    if (!sim_save_state(path, state))
    {
        LOG_WARNING("Cannot remember the room in %s", path);
    }
    return result;
}

static void remember_welcome_locked (OXR_Runtime* rt, b8 dismissed)
{
    rt->welcome_dismissed = dismissed;
    char path[OS_MAX_PATH];
    if (!state_path(path, sizeof(path)))
    {
        return;
    }
    SIM_State state = {};
    sim_load_state(path, state);
    state.welcome_dismissed = dismissed;
    if (!sim_save_state(path, state))
    {
        LOG_WARNING("Cannot remember the welcome choice in %s", path);
    }
}

// F1 toggles the overlay; while it is shown Enter closes it for good and Escape only for now,
// unless they go to a text field being edited
static b8 welcome_key_locked (OXR_Runtime* rt, OS_Key key, b8 editing_text)
{
    if (key == OS_Key::F1)
    {
        rt->show_welcome = !rt->show_welcome;
        return true;
    }
    if (!rt->show_welcome || editing_text || (key != OS_Key::Enter && key != OS_Key::Escape))
    {
        return false;
    }
    rt->show_welcome = false;
    if (key == OS_Key::Enter && !rt->welcome_dismissed)
    {
        remember_welcome_locked(rt, true);
    }
    return true;
}

b8 oxr_welcome_key (OS_Key key, b8 editing_text)
{
    const OXR_Lock lock(__func__);
    return welcome_key_locked(runtime(), key, editing_text);
}

b8 oxr_welcome_shown ()
{
    const OXR_Lock lock(__func__);
    return runtime()->show_welcome;
}

OXR_RoomResult oxr_set_room (const char* name)
{
    const OXR_Lock lock(__func__);
    return set_room_locked(runtime(), name);
}

OXR_RoomResult oxr_choose_room (const char* name)
{
    const OXR_Lock lock(__func__);
    return choose_room_locked(runtime(), name);
}

b8 oxr_get_room (char* out_name, usize out_size)
{
    const OXR_Lock           lock(__func__);
    const OXR_Runtime* rt = runtime();
    return rt->sim && cstr_copy(out_name, out_size, rt->room);
}

b8 oxr_get_display_state (OXR_DisplayState& out_state)
{
    static const char* const names[] = { "unmanaged", "rec2020", "rec709", "rift_cv1", "rift_s", "quest", "p3", "adobe_rgb" };
    const OXR_Lock     lock(__func__);
    const OXR_Runtime* rt = runtime();
    if (!rt->sim)
    {
        return false;
    }
    out_state = { .refresh_rate = sim_refresh_rate(rt->sim), .color_space = names[XR_COLOR_SPACE_REC709_FB], .local_dimming = true };
    b8 first = true;
    for (const OXR_Session& session : rt->sessions)
    {
        if (first)
        {
            const u32 space         = (u32)session.color_space;
            out_state.color_space   = space < ARRAY_SIZE(names) ? names[space] : "unknown";
            out_state.local_dimming       = session.local_dimming == XR_LOCAL_DIMMING_MODE_ON_META;
            out_state.boundary_suppressed = session.boundary == XR_BOUNDARY_VISIBILITY_SUPPRESSED_META;
            first                         = false;
        }
    }
    return true;
}

static const OXR_VirtualKeyboard* first_keyboard (const OXR_Runtime* rt)
{
    if (rt->sessions.length() == 0)
    {
        return nullptr;
    }
    const OXR_SessionHandle first = rt->sessions.handle_at(0);
    for (usize i = 0; i < rt->keyboards.length(); i++)
    {
        if (rt->keyboards.data()[i].session == first)
        {
            return &rt->keyboards.data()[i];
        }
    }
    return nullptr;
}

b8 oxr_get_keyboard_state (OXR_KeyboardState& out_state)
{
    const OXR_Lock     lock(__func__);
    const OXR_Runtime* rt = runtime();
    out_state             = { .pose = zm::POSE_IDENTITY, .scale = 1.0f };
    if (!rt->sim)
    {
        return false;
    }
    const OXR_VirtualKeyboard* keyboard = first_keyboard(rt);
    if (keyboard)
    {
        out_state.exists  = true;
        out_state.visible = keyboard->visible;
        out_state.shifted = keyboard->shifted;
        out_state.pose    = keyboard->pose;
        out_state.scale   = keyboard->scale;
        cstr_copy(out_state.context, sizeof(out_state.context), keyboard->context);
    }
    return true;
}

b8 oxr_get_keyboard_key (const char* name, Pose& out_pose)
{
    const OXR_Lock             lock(__func__);
    const OXR_VirtualKeyboard* keyboard = first_keyboard(runtime());
    const i32                  key      = keyboard && name ? sim_vkb_find_key(keyboard->layout, name) : -1;
    if (key < 0)
    {
        return false;
    }
    const Vec2 center = keyboard->layout.keys[key].center;
    out_pose          = zm::pose_mul(keyboard->pose, { Vec3{ center.x, center.y, SIM_VKB_KEY_HEIGHT } * keyboard->scale, zm::QUAT_IDENTITY });
    return true;
}

static b8 recording_or_replaying (const OXR_Runtime* rt)
{
    return rt->recorder || (rt->replay && !rt->replay_finished);
}

u32 oxr_list_anchors (OXR_AnchorEntry* out_entries, u32 capacity)
{
    const OXR_Lock     lock(__func__);
    const OXR_Runtime* rt    = runtime();
    u32                count = 0;
    if (!rt->sim)
    {
        return 0;
    }
    for (const W_StoredAnchor& anchor : rt->store.anchors)
    {
        if (out_entries && count < capacity)
        {
            out_entries[count] = { .anchor = anchor };
        }
        count++;
    }
    Array<W_CloudAnchor> cloud = {};
    defer { w_cloud_release_all(cloud); };
    w_cloud_list(rt->cloud_anchors_dir[0] ? rt->cloud_anchors_dir : nullptr, cloud);
    for (const W_CloudAnchor& entry : cloud)
    {
        if (!w_cloud_visible(entry, rt->user_id))
        {
            continue;
        }
        if (out_entries && count < capacity)
        {
            out_entries[count] = { .anchor = entry.anchor, .cloud = true, .owner = entry.owner };
        }
        count++;
    }
    return count;
}

static OXR_ControlResult wipe_anchors_locked (OXR_Runtime* rt, b8 include_cloud)
{
    if (!rt->sim)
    {
        return OXR_ControlResult::NoInstance;
    }
    if (recording_or_replaying(rt))
    {
        return OXR_ControlResult::Busy;
    }
    const u32 local = (u32)rt->store.anchors.length;
    rt->store.anchors.clear();
    commit_store(rt);
    u32 cloud_count = 0;
    if (include_cloud && rt->cloud_anchors_dir[0])
    {
        Array<W_CloudAnchor> cloud = {};
        defer { w_cloud_release_all(cloud); };
        w_cloud_list(rt->cloud_anchors_dir, cloud);
        for (const W_CloudAnchor& entry : cloud)
        {
            if (entry.owner == rt->user_id)
            {
                w_cloud_erase(rt->cloud_anchors_dir, entry.anchor.uuid);
                cloud_count++;
            }
        }
    }
    LOG_INFO("Wiped %u stored anchors and %u cloud anchors of %s", local, cloud_count, rt->app_key);
    return OXR_ControlResult::Ok;
}

OXR_ControlResult oxr_wipe_anchors (b8 include_cloud)
{
    const OXR_Lock lock(__func__);
    return wipe_anchors_locked(runtime(), include_cloud);
}

OXR_ControlResult oxr_set_anchor_fault (OXR_AnchorOp op, i32 result, u32 count)
{
    const OXR_Lock lock(__func__);
    OXR_Runtime*   rt = runtime();
    if ((u32)op >= (u32)OXR_AnchorOp::Count || (count > 0 && result >= 0))
    {
        return OXR_ControlResult::Invalid;
    }
    if (recording_or_replaying(rt))
    {
        return OXR_ControlResult::Busy;
    }
    rt->anchor_faults[(u32)op] = { .result = count > 0 ? (XrResult)result : XR_SUCCESS, .remaining = count };
    return OXR_ControlResult::Ok;
}

OXR_ControlResult oxr_set_frame_fault (OXR_FrameOp op, i32 result, u32 count)
{
    const OXR_Lock lock(__func__);
    OXR_Runtime*   rt = runtime();
    if ((u32)op >= (u32)OXR_FrameOp::Count || (count > 0 && !frame_fault_allowed(op, (XrResult)result)))
    {
        return OXR_ControlResult::Invalid;
    }
    if (recording_or_replaying(rt))
    {
        return OXR_ControlResult::Busy;
    }
    rt->frame_faults[(u32)op] = { .result = count > 0 ? (XrResult)result : XR_SUCCESS, .remaining = count };
    return OXR_ControlResult::Ok;
}

OXR_ControlResult oxr_set_anchor_timing (u32 latency_frames, u32 localize_frames)
{
    const OXR_Lock lock(__func__);
    OXR_Runtime*   rt = runtime();
    if (recording_or_replaying(rt))
    {
        return OXR_ControlResult::Busy;
    }
    rt->anchor_latency_frames  = latency_frames;
    rt->anchor_localize_frames = localize_frames;
    return OXR_ControlResult::Ok;
}

b8 oxr_get_origin_offset (Pose& out_room_from_stage)
{
    const OXR_Lock     lock(__func__);
    const OXR_Runtime* rt = runtime();
    out_room_from_stage   = rt->room_from_stage;
    return rt->sim != nullptr;
}

void oxr_set_disabled_extensions (const char* names)
{
    const OXR_Lock lock(__func__);
    OXR_Runtime*   rt = runtime();
    cstr_copy(rt->disabled_extensions, sizeof(rt->disabled_extensions), names ? names : "");
}

void oxr_request_exit ()
{
    const OXR_Lock lock(__func__);
    runtime()->exit_requested = true;
}

void oxr_set_mirror_view (SIM_MirrorView view)
{
    const OXR_Lock lock(__func__);
    runtime()->mirror_view = view;
}

b8 oxr_frame_ready ()
{
    const OXR_Lock lock(__func__);
    const COMP_Compositor* compositor = first_compositor();
    return compositor && comp_frame_ready(compositor);
}

b8 oxr_frames_in_flight ()
{
    const OXR_Lock lock(__func__);
    for (const OXR_Session& sess : runtime()->sessions)
    {
        if (sess.running && (sess.frame_waited || sess.frame_begun))
        {
            return true;
        }
    }
    return false;
}

b8 oxr_save_screenshot (const char* path)
{
    const OXR_Lock lock(__func__);
    const COMP_Compositor* compositor = first_compositor();
    if (!compositor)
    {
        return false;
    }
    note_frame_read();
    return comp_save_png(compositor, path);
}

//
// Public API: xr* entry points
//

// Instance

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateApiLayerProperties (uint32_t capacity, uint32_t* count_output, XrApiLayerProperties* properties) // NOLINT(misc-const-correctness): the spec signature
{
    return oxr_two_call(capacity, count_output, properties, 0);
}

static const XrExtensionProperties oxr_extension_properties[] = {
// NOLINTNEXTLINE(bugprone-macro-parentheses): a string literal initializing a char array cannot be parenthesized
#define X(field, name, version) { .type = XR_TYPE_EXTENSION_PROPERTIES, .next = nullptr, .extensionName = name, .extensionVersion = (version) },
    OXR_EXTENSIONS(X)
#undef X
};

// Extensions that bind or extend a graphics API, with the APIs that keep them offered (PUPPETXR_GRAPHICS)
static const struct
{
    const char* name;
    u32         apis; // bits of SIM_GraphicsApi, any of them
} oxr_graphics_extensions[] = {
    { "XR_KHR_vulkan_enable",                 1u << (u32)SIM_GraphicsApi::Vulkan },
    { "XR_KHR_vulkan_enable2",                1u << (u32)SIM_GraphicsApi::Vulkan },
    { "XR_FB_swapchain_update_state_vulkan",  1u << (u32)SIM_GraphicsApi::Vulkan },
    { "XR_META_vulkan_swapchain_create_info", 1u << (u32)SIM_GraphicsApi::Vulkan },
    { "XR_KHR_opengl_enable",                 1u << (u32)SIM_GraphicsApi::OpenGL },
    { "XR_KHR_opengl_es_enable",              1u << (u32)SIM_GraphicsApi::OpenGLES },
    { "XR_MNDX_egl_enable",                   (1u << (u32)SIM_GraphicsApi::OpenGL) | (1u << (u32)SIM_GraphicsApi::OpenGLES) },
    { "XR_KHR_D3D11_enable",                  1u << (u32)SIM_GraphicsApi::D3D11 },
    { "XR_KHR_D3D12_enable",                  1u << (u32)SIM_GraphicsApi::D3D12 },
    { "XR_KHR_metal_enable",                  1u << (u32)SIM_GraphicsApi::Metal },
    { "XR_KHRX2_metal_enable",                1u << (u32)SIM_GraphicsApi::Metal },
};

static b8 graphics_disabled (const SIM_Config& config, const char* name)
{
    for (const auto& extension : oxr_graphics_extensions)
    {
        if (cstr_equal(extension.name, name))
        {
            return (config.graphics_apis & extension.apis) == 0;
        }
    }
    return false;
}

// Extensions of another platform's clock or of a graphics API the build lacks are never offered.
// Timespecs are offered everywhere (os_timespec_ns): MR Utility Kit converts passthrough camera
// timestamps with them on Windows too.
static b8 extension_unavailable (const char* name)
{
    const OS_NativeClock clock = os_native_clock();
    if (cstr_equal(name, "XR_KHR_win32_convert_performance_counter_time"))
    {
        return clock != OS_NativeClock::PerformanceCounter;
    }
    if (cstr_equal(name, "XR_KHR_D3D11_enable"))
    {
        return !r_backend_available(R_Backend::D3D11);
    }
    if (cstr_equal(name, "XR_OCULUS_audio_device_guid"))
    {
        return !os_has_audio_device_ids();
    }
    if (cstr_equal(name, "XR_KHR_D3D12_enable"))
    {
        return !r_backend_available(R_Backend::D3D12);
    }
    if (cstr_equal(name, "XR_KHR_opengl_enable") || cstr_equal(name, "XR_MNDX_egl_enable"))
    {
        return !r_backend_available(R_Backend::OpenGL);
    }
    if (cstr_equal(name, "XR_KHR_opengl_es_enable"))
    {
        return !r_backend_available(R_Backend::OpenGLES);
    }
    if (cstr_equal(name, "XR_KHR_metal_enable") || cstr_equal(name, "XR_KHRX2_metal_enable"))
    {
        return !r_backend_available(R_Backend::Metal);
    }
    return false;
}

static b8 extension_disabled (const SIM_Config& config, const char* name)
{
    return extension_unavailable(name) || sim_extension_disabled(config, name) || graphics_disabled(config, name) || label_recognized(runtime()->disabled_extensions, name);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateInstanceExtensionProperties (const char* layer_name, uint32_t capacity, uint32_t* count_output, XrExtensionProperties* properties)
{
    if (layer_name)
    {
        return XR_ERROR_API_LAYER_NOT_PRESENT;
    }

    SIM_Config config = {};
    sim_load_full_config(config);
    const OXR_Lock lock(__func__);
    const XrExtensionProperties* offered[ARRAY_SIZE(oxr_extension_properties)];
    u32                          count = 0;
    for (const XrExtensionProperties& extension : oxr_extension_properties)
    {
        if (!extension_disabled(config, extension.extensionName))
        {
            offered[count++] = &extension;
        }
    }

    const XrResult result = oxr_two_call(capacity, count_output, properties, count);
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    for (u32 i = 0; i < count; i++)
    {
        OXR_CHECK_TYPE(&properties[i], XR_TYPE_EXTENSION_PROPERTIES);
        cstr_copy(properties[i].extensionName, sizeof(properties[i].extensionName), offered[i]->extensionName);
        properties[i].extensionVersion = offered[i]->extensionVersion;
    }
    return XR_SUCCESS;
}

// The config's "trace": read by every lookup without an instance, which comes before the instance
// it creates, and again by each xrCreateInstance
static char oxr_trace_filter[sizeof(SIM_Config::trace)] = {};
static b8   oxr_trace_read = false;

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateInstance (const XrInstanceCreateInfo* info, XrInstance* out_instance)
{
    const OXR_Lock lock(__func__);
    if (!oxr_startup.origin)
    {
        oxr_startup.origin  = os_monotonic_ns();
        oxr_startup.entered = oxr_startup.origin;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_INSTANCE_CREATE_INFO);
    OXR_CHECK(out_instance);
    *out_instance = XR_NULL_HANDLE;
    OXR_CHECK(info->enabledApiLayerCount == 0 || info->enabledApiLayerNames);
    OXR_CHECK(info->enabledExtensionCount == 0 || info->enabledExtensionNames);

    // API layers are the loader's business: it validates and loads them and passes the create
    // info through unchanged, so the names it lists are ignored here
    if (XR_VERSION_MAJOR(info->applicationInfo.apiVersion) != 1 || XR_VERSION_MINOR(info->applicationInfo.apiVersion) > 1)
    {
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    }
    if (info->applicationInfo.applicationName[0] == 0)
    {
        return XR_ERROR_NAME_INVALID;
    }

    SIM_Config config = {};
    sim_load_full_config(config);
    cstr_copy(oxr_trace_filter, sizeof(oxr_trace_filter), config.trace);
    oxr_trace_read = true;
    OXR_Extensions ext = {};
    for (u32 i = 0; i < info->enabledExtensionCount; i++)
    {
        const char* name  = info->enabledExtensionNames[i];
        b8          found = false;
        OXR_CHECK(name);
#define X(field, ext_name, version) if (cstr_equal(name, ext_name)) { ext.field = true; found = true; }
        OXR_EXTENSIONS(X)
#undef X
        if (!found)
        {
            LOG_ERROR("Extension %s is not supported", name);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
        if (extension_disabled(config, name))
        {
            LOG_ERROR("Extension %s is disabled in the simulator", name);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    // Spec XR_META_environment_raycast: it needs XR_EXT_future enabled as well
    if (ext.META_environment_raycast && !ext.EXT_future)
    {
        LOG_ERROR("XR_META_environment_raycast needs XR_EXT_future");
        return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
    }
    if (ext.EXT_spatial_entity && !ext.EXT_future)
    {
        LOG_ERROR("XR_EXT_spatial_entity needs XR_EXT_future");
        return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
    }
    if (ext.EXT_spatial_marker_tracking && !ext.EXT_spatial_entity)
    {
        LOG_ERROR("XR_EXT_spatial_marker_tracking needs XR_EXT_spatial_entity");
        return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
    }
    if (ext.EXT_interaction_render_model && !ext.EXT_render_model)
    {
        LOG_ERROR("XR_EXT_interaction_render_model needs XR_EXT_render_model");
        return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
    }
    if (ext.EXT_dpad_binding && !ext.KHR_binding_modification)
    {
        LOG_ERROR("XR_EXT_dpad_binding needs XR_KHR_binding_modification");
        return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
    }
    if ((ext.FB_foveation && !ext.FB_swapchain_update_state) || (ext.FB_foveation_configuration && !ext.FB_foveation) || (ext.META_foveation_eye_tracked && !ext.FB_foveation_configuration))
    {
        LOG_ERROR("XR_FB_foveation needs XR_FB_swapchain_update_state, XR_FB_foveation_configuration XR_FB_foveation, XR_META_foveation_eye_tracked XR_FB_foveation_configuration");
        return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
    }
    if (ext.META_passthrough_color_lut && !ext.FB_passthrough)
    {
        LOG_ERROR("XR_META_passthrough_color_lut needs XR_FB_passthrough");
        return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
    }
    if (ext.META_dynamic_object_keyboard && !ext.META_dynamic_object_tracker)
    {
        LOG_ERROR("XR_META_dynamic_object_keyboard needs XR_META_dynamic_object_tracker");
        return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
    }
    const b8 on_hand_tracking = ext.FB_hand_tracking_aim || ext.FB_hand_tracking_capsules || ext.FB_hand_tracking_mesh || ext.EXT_hand_tracking_data_source || ext.EXT_hand_joints_motion_range || ext.META_simultaneous_hands_and_controllers;
    if (ext.META_hand_tracking_microgestures && !ext.EXT_hand_interaction)
    {
        LOG_ERROR("XR_META_hand_tracking_microgestures needs XR_EXT_hand_interaction");
        return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
    }
    const b8 on_hand_tracking_meta = ext.META_hand_tracking_wide_motion_mode || ext.META_hand_tracking_wide_motion_mode2 || ext.META_hand_tracking_frequency_hint || ext.META_hand_tracking_unextrapolated_poses;
    if ((on_hand_tracking || on_hand_tracking_meta) && !ext.EXT_hand_tracking)
    {
        LOG_ERROR("The FB and META hand tracking extensions, XR_EXT_hand_tracking_data_source, XR_EXT_hand_joints_motion_range and XR_META_simultaneous_hands_and_controllers need XR_EXT_hand_tracking");
        return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
    }

    OXR_Runtime* rt = runtime();
    if (rt->live_instance)
    {
        LOG_ERROR("Only one XrInstance can be alive at a time");
        return XR_ERROR_LIMIT_REACHED;
    }

    for (u32 i = 0; i < config.disabled_extensions_count; i++)
    {
        b8 known = false;
        for (const XrExtensionProperties& extension : oxr_extension_properties)
        {
            known = known || cstr_equal(extension.extensionName, config.disabled_extensions[i]);
        }
        if (!known)
        {
            LOG_WARNING("Disabled extension %s is not one puppetxr offers", config.disabled_extensions[i]);
        }
    }
    if (config.graphics_apis != SIM_GRAPHICS_ALL)
    {
        char apis[64] = {};
        for (u32 i = 0; i < (u32)SIM_GraphicsApi::Count; i++)
        {
            if (sim_graphics_api_allowed(config, (SIM_GraphicsApi)i))
            {
                const usize length = cstr_length(apis);
                cstr_format(apis + length, sizeof(apis) - length, "%s%s", length ? ", " : "", sim_graphics_api_name((SIM_GraphicsApi)i));
            }
        }
        LOG_INFO("Graphics APIs offered: %s", apis);
    }
    char      state_file[OS_MAX_PATH] = {};
    SIM_State state                   = {};
    if (!config.headless && state_path(state_file, sizeof(state_file)) && sim_load_state(state_file, state))
    {
        sim_apply_state(config, state);
    }

    REC_Recording* replay = nullptr;
    if (config.replay_path[0])
    {
        replay = rec_load(config.replay_path);
        if (!replay)
        {
            LOG_ERROR("Cannot replay %s", config.replay_path);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        rec_apply_header(rec_header(replay), config);
    }

    const u64 room_start = os_monotonic_ns();
    resolve_rooms_dir(config, rt->rooms_dir, sizeof(rt->rooms_dir));
    rt->room_names_count = w_list_all_rooms(rt->rooms_dir, rt->room_names, OXR_MAX_ROOMS);
    const b8 room_loaded = load_room_locked(rt, config.room, replay ? rec_header(replay).room_hash : 0);
    oxr_startup.room    += os_monotonic_ns() - room_start;
    if (!room_loaded)
    {
        load_room_locked(rt, "", 0);
        if (replay)
        {
            LOG_ERROR("Cannot replay %s: its room '%s' is missing or changed", config.replay_path, config.room);
            rec_release(replay);
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }

    if (!os_initialize(config.headless))
    {
        rec_release(replay);
        load_room_locked(rt, "", 0);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    config.headless = os_is_headless();
    if (!config.headless && !rt->window)
    {
        const u64 window_start = os_monotonic_ns();
        rt->window             = os_create_window("puppetxr", config.window_width, config.window_height, true, 0, true);
        rt->window_shown       = false;
        oxr_startup.window    += os_monotonic_ns() - window_start;
        if (!rt->window)
        {
            LOG_WARNING("Window creation failed, continuing headless");
            config.headless = true;
        }
        else if (config.window_maximized)
        {
            os_window_maximize(rt->window);
        }
    }
    rt->window_closed     = false;
    rt->mouse_captured    = false;
    rt->capture_click     = false;
    rt->show_ui           = config.show_ui;
    rt->lens_mask         = config.lens_mask;
    rt->welcome_dismissed = state.welcome_dismissed;
    rt->show_welcome      = rt->window && config.show_welcome && !state.welcome_dismissed;
    if (rt->window)
    {
        os_capture_output_begin();
    }
    rt->mirror_view   = config.mirror_view;

    const OXR_InstanceHandle handle = rt->instances.add();
    OXR_Instance*            inst   = &rt->instances.get(handle);
    inst->api_version               = info->applicationInfo.apiVersion;
    inst->ext                       = ext;
    inst->spec_features             = spec_features(info->applicationInfo.apiVersion, info->enabledExtensionNames, info->enabledExtensionCount);
    cstr_copy(inst->application_name, sizeof(inst->application_name), info->applicationInfo.applicationName);
    open_anchor_store_locked(rt, config, inst->application_name);

    rt->sim                    = sim_create(config);
    // Time conversions anchor here, so the clock's first frame counts from here too
    sim_start_clock(rt->sim, os_time_ns());
    rt->clock_anchor_time      = sim_time(rt->sim);
    rt->clock_anchor_monotonic = os_monotonic_ns();
    rt->clock_anchor_timespec  = os_timespec_ns();
    rt->live_instance  = handle;
    rt->exit_requested = false;
    if (replay)
    {
        replay_begin_locked(rt, replay);
    }
    if (config.record_path[0])
    {
        record_start_locked(rt, config.record_path);
    }
    if (config.profile_path[0] && !os_profile_active())
    {
        os_profile_start(config.profile_path);
    }

    *out_instance = instance_xr(handle);
    if (g_control_hooks.instance_created)
    {
        g_control_hooks.instance_created(config, inst->application_name);
    }
    LOG_INFO("Instance created for %s (api %u.%u.%u, %s)", inst->application_name, XR_VERSION_MAJOR(inst->api_version), XR_VERSION_MINOR(inst->api_version), XR_VERSION_PATCH(inst->api_version), config.headless ? "headless" : "windowed");
    if (config.known_module)
    {
        LOG_INFO("Known module %s loaded: runtime name %s, time 0 %s, older frame times %s, discovery before running %s", config.known_module, config.runtime_name, config.accept_time_zero ? "accepted" : "invalid", config.accept_older_frame_time ? "accepted" : "invalid", config.discover_before_running ? "answered" : "refused");
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyInstance (XrInstance instance)
{
    b8 last = false;
    {
        const OXR_Lock lock(__func__);
        OXR_GET_CONST_INSTANCE(inst, instance);
        UNUSED(inst);
        last = destroy_instance(instance_handle(instance));
        // A run that showed frames is over; the next instance starts the next startup, as each
        // Play in an editor does
        if (oxr_startup.logged)
        {
            const u32 depth        = oxr_startup.lock_depth;
            oxr_startup            = {};
            oxr_startup.lock_depth = depth;
        }
    }
    // A window tick posted by the last frame may wait on the window thread for the lock, and the
    // thread quits only after it
    if (last)
    {
        os_shutdown();
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetInstanceProperties (XrInstance instance, XrInstanceProperties* properties)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK_TYPE(properties, XR_TYPE_INSTANCE_PROPERTIES);
    properties->runtimeVersion = OXR_RUNTIME_VERSION;
    cstr_copy(properties->runtimeName, sizeof(properties->runtimeName), sim_get_config(runtime()->sim).runtime_name);
    return XR_SUCCESS;
}

// A buffer an app reuses still carries the type of the event it last received; anything else
// is no XrEventDataBuffer and too small for one
static b8 event_buffer_type_valid (XrStructureType type)
{
    if (type == XR_TYPE_EVENT_DATA_BUFFER)
    {
        return true;
    }
#define OXR_EVENT_TYPE(name, value) \
    if (type == (value))            \
    {                               \
        return true;                \
    }
    XR_LIST_ALL_CHILD_STRUCTURE_TYPES_XrEventDataBaseHeader(OXR_EVENT_TYPE, OXR_EVENT_TYPE)
#undef OXR_EVENT_TYPE
    return false;
}

// Queues what is due at a poll and drops what may not be returned; returns the index of the next
// event waiting in `channel`, or events_count when there is none
static u32 next_event (XrInstance instance, OXR_Instance* inst, u64 channel)
{
    queue_reference_space_changes(instance_handle(instance));
    while (true)
    {
        const u32 index = find_event(inst, channel);
        if (index == inst->events_count)
        {
            return index;
        }
        // Spec XR_EXT_spatial_entity: recommendations for destroyed contexts are not returned
        const OXR_Event& e = inst->events[(inst->events_head + index) % OXR_MAX_EVENTS];
        if (e.type != XR_TYPE_EVENT_DATA_SPATIAL_DISCOVERY_RECOMMENDED_EXT || find_spatial_context((XrSpatialContextEXT)(uintptr_t)e.spatial_context))
        {
            return index;
        }
        remove_event(inst, index);
    }
}

static XrResult poll_event (XrInstance instance, OXR_Instance* inst, u64 channel, XrEventDataBuffer* event)
{
    const u32 index = next_event(instance, inst, channel);
    if (inst->events_lost > 0 && channel == 0)
    {
        XrEventDataEventsLost* lost = (XrEventDataEventsLost*)event;
        memory_zero(event, sizeof(*event));
        lost->type           = XR_TYPE_EVENT_DATA_EVENTS_LOST;
        lost->lostEventCount = inst->events_lost;
        inst->events_lost    = 0;
        return XR_SUCCESS;
    }
    if (index == inst->events_count)
    {
        if (inst->events_count == 0)
        {
            inst->advert_payloads.clear();
        }
        return XR_EVENT_UNAVAILABLE;
    }
    const OXR_Event e = inst->events[(inst->events_head + index) % OXR_MAX_EVENTS];
    remove_event(inst, index);

    memory_zero(event, sizeof(*event));
    if (e.type == XR_TYPE_EVENT_DATA_INTERACTION_RENDER_MODELS_CHANGED_EXT)
    {
        event->type = e.type;
        return XR_SUCCESS;
    }
    if (e.type == XR_TYPE_EVENT_DATA_SPATIAL_DISCOVERY_RECOMMENDED_EXT)
    {
        XrEventDataSpatialDiscoveryRecommendedEXT* data = (XrEventDataSpatialDiscoveryRecommendedEXT*)event;
        data->type           = e.type;
        data->spatialContext = (XrSpatialContextEXT)(uintptr_t)e.spatial_context;
        return XR_SUCCESS;
    }
    if ((u32)e.type == OXR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_TRACKER_CREATE_RESULT_META || (u32)e.type == OXR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_SET_TRACKED_CLASSES_RESULT_META)
    {
        OXR_EventDataDynamicObjectResultMETA* data = (OXR_EventDataDynamicObjectResultMETA*)event;
        data->type   = e.type;
        data->handle = e.tracker;
        data->result = e.result;
        return XR_SUCCESS;
    }
    switch (e.type)
    {
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
    {
        XrEventDataSessionStateChanged* data = (XrEventDataSessionStateChanged*)event;
        data->type    = e.type;
        data->session = e.session;
        data->state   = e.state;
        data->time    = e.time;
        break;
    }
    case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
    {
        XrEventDataInteractionProfileChanged* data = (XrEventDataInteractionProfileChanged*)event;
        data->type    = e.type;
        data->session = e.session;
        break;
    }
    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
    {
        XrEventDataReferenceSpaceChangePending* data = (XrEventDataReferenceSpaceChangePending*)event;
        data->type                = e.type;
        data->session             = e.session;
        data->referenceSpaceType  = e.space_type;
        data->changeTime          = e.time;
        data->poseValid           = XR_TRUE;
        data->poseInPreviousSpace = pose_to_xr(e.pose);
        break;
    }
    case XR_TYPE_EVENT_DATA_SPATIAL_ANCHOR_CREATE_COMPLETE_FB:
    {
        XrEventDataSpatialAnchorCreateCompleteFB* data = (XrEventDataSpatialAnchorCreateCompleteFB*)event;
        data->type      = e.type;
        data->requestId = e.request_id;
        data->result    = e.result;
        data->space     = e.space;
        data->uuid      = uuid_to_xr(e.uuid);
        break;
    }
    case XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB:
    {
        XrEventDataSpaceSetStatusCompleteFB* data = (XrEventDataSpaceSetStatusCompleteFB*)event;
        data->type          = e.type;
        data->requestId     = e.request_id;
        data->result        = e.result;
        data->space         = e.space;
        data->uuid          = uuid_to_xr(e.uuid);
        data->componentType = (XrSpaceComponentTypeFB)e.component;
        data->enabled       = e.enabled ? XR_TRUE : XR_FALSE;
        break;
    }
    case XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB:
    {
        XrEventDataSpaceQueryResultsAvailableFB* data = (XrEventDataSpaceQueryResultsAvailableFB*)event;
        data->type      = e.type;
        data->requestId = e.request_id;
        break;
    }
    case XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB:
    {
        XrEventDataSpaceQueryCompleteFB* data = (XrEventDataSpaceQueryCompleteFB*)event;
        data->type      = e.type;
        data->requestId = e.request_id;
        data->result    = e.result;
        break;
    }
    case XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_RESULTS_AVAILABLE_META:
    {
        XrEventDataSpaceDiscoveryResultsAvailableMETA* data = (XrEventDataSpaceDiscoveryResultsAvailableMETA*)event;
        data->type      = e.type;
        data->requestId = e.request_id;
        break;
    }
    case XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_COMPLETE_META:
    {
        XrEventDataSpaceDiscoveryCompleteMETA* data = (XrEventDataSpaceDiscoveryCompleteMETA*)event;
        data->type      = e.type;
        data->requestId = e.request_id;
        data->result    = e.result;
        break;
    }
    case XR_TYPE_EVENT_DATA_SPACE_SAVE_COMPLETE_FB:
    case XR_TYPE_EVENT_DATA_SPACE_ERASE_COMPLETE_FB:
    {
        static_assert(sizeof(XrEventDataSpaceSaveCompleteFB) == sizeof(XrEventDataSpaceEraseCompleteFB), "save and erase events share a layout");
        XrEventDataSpaceSaveCompleteFB* data = (XrEventDataSpaceSaveCompleteFB*)event;
        data->type      = e.type;
        data->requestId = e.request_id;
        data->result    = e.result;
        data->space     = e.space;
        data->uuid      = uuid_to_xr(e.uuid);
        data->location  = (XrSpaceStorageLocationFB)e.location;
        break;
    }
    case XR_TYPE_EVENT_DATA_START_COLOCATION_ADVERTISEMENT_COMPLETE_META:
    {
        XrEventDataStartColocationAdvertisementCompleteMETA* data = (XrEventDataStartColocationAdvertisementCompleteMETA*)event;
        data->type                   = e.type;
        data->advertisementRequestId = e.request_id;
        data->result                 = e.result;
        data->advertisementUuid      = uuid_to_xr(e.uuid);
        break;
    }
    case XR_TYPE_EVENT_DATA_COLOCATION_DISCOVERY_RESULT_META:
    {
        XrEventDataColocationDiscoveryResultMETA* data = (XrEventDataColocationDiscoveryResultMETA*)event;
        static_assert(sizeof(XrEventDataColocationDiscoveryResultMETA) <= sizeof(XrEventDataBuffer), "discovery results fit the event buffer");
        data->type               = e.type;
        data->discoveryRequestId = e.request_id;
        data->advertisementUuid  = uuid_to_xr(e.uuid);
        if (e.advert < inst->advert_payloads.length)
        {
            const W_Advert& advert = inst->advert_payloads[e.advert];
            data->bufferSize       = advert.size;
            memory_copy(data->buffer, advert.buffer, advert.size);
        }
        break;
    }
    case XR_TYPE_EVENT_DATA_STOP_COLOCATION_ADVERTISEMENT_COMPLETE_META:
    case XR_TYPE_EVENT_DATA_COLOCATION_ADVERTISEMENT_COMPLETE_META:
    case XR_TYPE_EVENT_DATA_START_COLOCATION_DISCOVERY_COMPLETE_META:
    case XR_TYPE_EVENT_DATA_COLOCATION_DISCOVERY_COMPLETE_META:
    case XR_TYPE_EVENT_DATA_STOP_COLOCATION_DISCOVERY_COMPLETE_META:
    case XR_TYPE_EVENT_DATA_SPACE_LIST_SAVE_COMPLETE_FB:
    case XR_TYPE_EVENT_DATA_SPACE_SHARE_COMPLETE_FB:
    case XR_TYPE_EVENT_DATA_SHARE_SPACES_COMPLETE_META:
    case XR_TYPE_EVENT_DATA_SPACES_SAVE_RESULT_META:
    case XR_TYPE_EVENT_DATA_SPACES_ERASE_RESULT_META:
    case XR_TYPE_EVENT_DATA_SCENE_CAPTURE_COMPLETE_FB:
    {
        XrEventDataSceneCaptureCompleteFB* data = (XrEventDataSceneCaptureCompleteFB*)event;
        data->type      = e.type;
        data->requestId = e.request_id;
        data->result    = e.result;
        break;
    }
    case XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR:
    {
        XrEventDataVisibilityMaskChangedKHR* data = (XrEventDataVisibilityMaskChangedKHR*)event;
        data->type                  = e.type;
        data->session               = e.session;
        data->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        data->viewIndex             = e.view_index;
        break;
    }
    case XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META:
    {
        XrEventDataBoundaryVisibilityChangedMETA* data = (XrEventDataBoundaryVisibilityChangedMETA*)event;
        data->type               = e.type;
        data->boundaryVisibility = e.boundary;
        break;
    }
    case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_COMMIT_TEXT_META:
    {
        XrEventDataVirtualKeyboardCommitTextMETA* data = (XrEventDataVirtualKeyboardCommitTextMETA*)event;
        data->type     = e.type;
        data->keyboard = e.keyboard;
        cstr_copy(data->text, sizeof(data->text), e.text);
        break;
    }
    case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_BACKSPACE_META:
    case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_ENTER_META:
    case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_SHOWN_META:
    case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_HIDDEN_META:
    {
        // The four share their layout
        XrEventDataVirtualKeyboardShownMETA* data = (XrEventDataVirtualKeyboardShownMETA*)event;
        data->type     = e.type;
        data->keyboard = e.keyboard;
        break;
    }
    case XR_TYPE_EVENT_DATA_PASSTHROUGH_LAYER_RESUMED_META:
    {
        XrEventDataPassthroughLayerResumedMETA* data = (XrEventDataPassthroughLayerResumedMETA*)event;
        data->type  = e.type;
        data->layer = e.layer;
        break;
    }
    case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
    {
        XrEventDataDisplayRefreshRateChangedFB* data = (XrEventDataDisplayRefreshRateChangedFB*)event;
        data->type                   = e.type;
        data->fromDisplayRefreshRate = e.from_rate;
        data->toDisplayRefreshRate   = e.to_rate;
        break;
    }
    default:
        event->type = e.type;
        break;
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPollEvent (XrInstance instance, XrEventDataBuffer* event)
{
    const OXR_Lock lock(__func__);
    if (g_control_hooks.poll)
    {
        g_control_hooks.poll(false);
    }
    OXR_GET_INSTANCE(inst, instance);
    OXR_CHECK(event && event_buffer_type_valid(event->type));
    return poll_event(instance, inst, 0, event);
}

//
// XR_EXTX1_event_channel: channels split the instance's events. The preview header leaves the
// semantics open; these are the simplest consistent ones (docs/openxr.md).
//

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateEventChannelEXTX1 (XrInstance instance, const OXR_EventChannelCreateInfoEXTX1* info, u64* out_channel)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK(info && (u32)info->type == OXR_TYPE_EVENT_CHANNEL_CREATE_INFO_EXTX1);
    OXR_CHECK(out_channel);
    *out_channel = 0;
    OXR_Runtime*             rt     = runtime();
    const OXR_InstanceHandle handle = instance_handle(instance);
    u32                      count  = 0;
    for (usize i = 0; i < rt->event_channels.length(); i++)
    {
        count += rt->event_channels.data()[i].instance == handle ? 1u : 0u;
    }
    if (count >= OXR_MAX_EVENT_CHANNELS)
    {
        return XR_ERROR_LIMIT_REACHED;
    }
    *out_channel = event_channel_xr(rt->event_channels.add({ .instance = handle }));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyEventChannelEXTX1 (u64 channel)
{
    const OXR_Lock lock(__func__);
    if (!channel || !runtime()->event_channels.try_get(event_channel_handle(channel)))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    destroy_event_channel(event_channel_handle(channel));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPollEventChannelEXTX1 (u64 channel, XrEventDataBuffer* event)
{
    const OXR_Lock    lock(__func__);
    const OXR_EventChannel* found = channel ? runtime()->event_channels.try_get(event_channel_handle(channel)) : nullptr;
    OXR_Instance*     inst  = found ? runtime()->instances.try_get(found->instance) : nullptr;
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (g_control_hooks.poll)
    {
        g_control_hooks.poll(false);
    }
    OXR_CHECK(event && event_buffer_type_valid(event->type));
    return poll_event(instance_xr(found->instance), inst, channel, event);
}

// Waits until one of the channels holds an event and returns the first such in the array. Events
// arise in the app's own calls, so the wait only ends early when another thread makes one.
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSelectEventChannelEXTX1 (XrInstance instance, OXR_SelectEventChannelInfoEXTX1* info, uint32_t* channel_with_event) // NOLINT(misc-const-correctness): PFN_xrSelectEventChannelEXTX1
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    OXR_CHECK(info && (u32)info->type == OXR_TYPE_SELECT_EVENT_CHANNEL_INFO_EXTX1);
    OXR_CHECK(info->flags == 0 && info->eventChannelCount > 0 && info->eventChannels);
    OXR_CHECK(channel_with_event);
    const OXR_InstanceHandle handle = instance_handle(instance);
    for (u32 i = 0; i < info->eventChannelCount; i++)
    {
        if (!find_event_channel(info->eventChannels[i], handle))
        {
            return XR_ERROR_HANDLE_INVALID;
        }
    }

    OXR_Runtime* rt    = runtime();
    const u64    start = os_monotonic_ns();
    while (true)
    {
        if (g_control_hooks.poll)
        {
            g_control_hooks.poll(false);
        }
        for (u32 i = 0; i < info->eventChannelCount; i++)
        {
            const u64 channel = info->eventChannels[i];
            if (find_event_channel(channel, handle) && next_event(instance, inst, channel) < inst->events_count)
            {
                *channel_with_event = i;
                return XR_SUCCESS;
            }
        }
        if (info->timeout <= 0 || (info->timeout != XR_INFINITE_DURATION && os_monotonic_ns() - start >= (u64)info->timeout))
        {
            return XR_TIMEOUT_EXPIRED;
        }
        oxr_unlock();
        os_sleep_ns(OXR_FRAME_GATE_POLL_NS);
        oxr_lock();
        inst = rt->instances.try_get(handle);
        if (!inst)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetDefaultEventChannelEXTX1 (XrInstance instance, u64 channel)
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    if (channel && !find_event_channel(channel, instance_handle(instance)))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    inst->default_channel = channel;
    return XR_SUCCESS;
}

static const char* result_name (XrResult value)
{
    switch (value)
    {
#define X(name, val) case name: return #name;
        XR_LIST_ENUM_XrResult(X)
#undef X
    default:
        return nullptr;
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrResultToString (XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE])
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK(buffer);
    const char* name = result_name(value);
    if (name)
    {
        cstr_copy(buffer, XR_MAX_RESULT_STRING_SIZE, name);
        return XR_SUCCESS;
    }
    cstr_format(buffer, XR_MAX_RESULT_STRING_SIZE, value < 0 ? "XR_UNKNOWN_FAILURE_%d" : "XR_UNKNOWN_SUCCESS_%d", (i32)value);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrStructureTypeToString (XrInstance instance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE])
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK(buffer);
    switch (value)
    {
#define X(name, val) case name: cstr_copy(buffer, XR_MAX_STRUCTURE_NAME_SIZE, #name); return XR_SUCCESS;
        XR_LIST_ENUM_XrStructureType(X)
#undef X
    default:
        cstr_format(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", (i32)value);
        return XR_SUCCESS;
    }
}

// System

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSystem (XrInstance instance, const XrSystemGetInfo* info, XrSystemId* out_system)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK_TYPE(info, XR_TYPE_SYSTEM_GET_INFO);
    OXR_CHECK(out_system);
    *out_system = XR_NULL_SYSTEM_ID;

    switch (info->formFactor)
    {
    case XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY:
        *out_system = OXR_SYSTEM_ID;
        return XR_SUCCESS;
    default:
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSystemProperties (XrInstance instance, XrSystemId system, XrSystemProperties* properties)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    if (system != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    OXR_CHECK_TYPE(properties, XR_TYPE_SYSTEM_PROPERTIES);

    const SIM_Config& config = sim_get_config(runtime()->sim);
    properties->systemId     = OXR_SYSTEM_ID;
    properties->vendorId     = config.vendor_id;
    cstr_copy(properties->systemName, sizeof(properties->systemName), config.system_name);
    properties->graphicsProperties = { .maxSwapchainImageHeight = OXR_MAX_IMAGE_SIZE, .maxSwapchainImageWidth = OXR_MAX_IMAGE_SIZE, .maxLayerCount = OXR_MAX_LAYERS };
    properties->trackingProperties = { .orientationTracking = XR_TRUE, .positionTracking = XR_TRUE };
    for (XrBaseOutStructure* next = (XrBaseOutStructure*)properties->next; next; next = next->next)
    {
        // Preview structs, outside XrStructureType
        if ((u32)next->type == OXR_TYPE_SYSTEM_DYNAMIC_OBJECT_TRACKER_PROPERTIES_META)
        {
            ((OXR_SystemDynamicObjectPropertiesMETA*)next)->supported = inst->ext.META_dynamic_object_tracker ? XR_TRUE : XR_FALSE;
            continue;
        }
        if ((u32)next->type == OXR_TYPE_SYSTEM_DYNAMIC_OBJECT_KEYBOARD_PROPERTIES_META)
        {
            ((OXR_SystemDynamicObjectPropertiesMETA*)next)->supported = inst->ext.META_dynamic_object_keyboard ? XR_TRUE : XR_FALSE;
            continue;
        }
        switch (next->type)
        {
        case XR_TYPE_SYSTEM_SPATIAL_ENTITY_PROPERTIES_FB:
            ((XrSystemSpatialEntityPropertiesFB*)next)->supportsSpatialEntity = inst->ext.FB_spatial_entity ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_SPACE_DISCOVERY_PROPERTIES_META:
            ((XrSystemSpaceDiscoveryPropertiesMETA*)next)->supportsSpaceDiscovery = inst->ext.META_spatial_entity_discovery ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_COLOCATION_DISCOVERY_PROPERTIES_META:
            ((XrSystemColocationDiscoveryPropertiesMETA*)next)->supportsColocationDiscovery = inst->ext.META_colocation_discovery ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_SPATIAL_ENTITY_SHARING_PROPERTIES_META:
            ((XrSystemSpatialEntitySharingPropertiesMETA*)next)->supportsSpatialEntitySharing = inst->ext.META_spatial_entity_sharing ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_SPATIAL_ENTITY_GROUP_SHARING_PROPERTIES_META:
            ((XrSystemSpatialEntityGroupSharingPropertiesMETA*)next)->supportsSpatialEntityGroupSharing = inst->ext.META_spatial_entity_group_sharing ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_SPACE_PERSISTENCE_PROPERTIES_META:
            ((XrSystemSpacePersistencePropertiesMETA*)next)->supportsSpacePersistence = inst->ext.META_spatial_entity_persistence ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB:
            ((XrSystemPassthroughPropertiesFB*)next)->supportsPassthrough = inst->ext.FB_passthrough ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_ENVIRONMENT_DEPTH_PROPERTIES_META:
            ((XrSystemEnvironmentDepthPropertiesMETA*)next)->supportsEnvironmentDepth = inst->ext.META_environment_depth ? XR_TRUE : XR_FALSE;
            ((XrSystemEnvironmentDepthPropertiesMETA*)next)->supportsHandRemoval      = inst->ext.META_environment_depth ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_ENVIRONMENT_RAYCAST_PROPERTIES_META:
            ((XrSystemEnvironmentRaycastPropertiesMETA*)next)->supportsEnvironmentRaycast = inst->ext.META_environment_raycast ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_SIMULTANEOUS_HANDS_AND_CONTROLLERS_PROPERTIES_META:
            ((XrSystemSimultaneousHandsAndControllersPropertiesMETA*)next)->supportsSimultaneousHandsAndControllers = inst->ext.META_simultaneous_hands_and_controllers ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_RENDER_MODEL_PROPERTIES_FB:
            ((XrSystemRenderModelPropertiesFB*)next)->supportsRenderModelLoading = inst->ext.FB_render_model ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_VIRTUAL_KEYBOARD_PROPERTIES_META:
            ((XrSystemVirtualKeyboardPropertiesMETA*)next)->supportsVirtualKeyboard = inst->ext.META_virtual_keyboard ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_KEYBOARD_TRACKING_PROPERTIES_FB:
            ((XrSystemKeyboardTrackingPropertiesFB*)next)->supportsKeyboardTracking = inst->ext.FB_keyboard_tracking ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT:
            ((XrSystemHandTrackingPropertiesEXT*)next)->supportsHandTracking = inst->ext.EXT_hand_tracking ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB:
            ((XrSystemBodyTrackingPropertiesFB*)next)->supportsBodyTracking = inst->ext.FB_body_tracking ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META:
            ((XrSystemPropertiesBodyTrackingFullBodyMETA*)next)->supportsFullBodyTracking = inst->ext.FB_body_tracking && inst->ext.META_body_tracking_full_body ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_FACE_TRACKING_PROPERTIES2_FB:
            ((XrSystemFaceTrackingProperties2FB*)next)->supportsVisualFaceTracking = inst->ext.FB_face_tracking2 ? XR_TRUE : XR_FALSE;
            ((XrSystemFaceTrackingProperties2FB*)next)->supportsAudioFaceTracking  = inst->ext.FB_face_tracking2 ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_FACE_TRACKING_PROPERTIES_FB:
            ((XrSystemFaceTrackingPropertiesFB*)next)->supportsFaceTracking = inst->ext.FB_face_tracking ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_FACE_TRACKING_VISEMES_PROPERTIES_META:
            ((XrSystemFaceTrackingVisemesPropertiesMETA*)next)->supportsVisemes = inst->ext.FB_face_tracking2 && inst->ext.META_face_tracking_visemes ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT:
            ((XrSystemEyeGazeInteractionPropertiesEXT*)next)->supportsEyeGazeInteraction = inst->ext.EXT_eye_gaze_interaction ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_EYE_TRACKING_PROPERTIES_FB:
            ((XrSystemEyeTrackingPropertiesFB*)next)->supportsEyeTracking = inst->ext.FB_eye_tracking_social ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META:
            ((XrSystemPropertiesBodyTrackingFidelityMETA*)next)->supportsBodyTrackingFidelity = inst->ext.FB_body_tracking && inst->ext.META_body_tracking_fidelity ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_CALIBRATION_META:
            ((XrSystemPropertiesBodyTrackingCalibrationMETA*)next)->supportsHeightOverride = inst->ext.FB_body_tracking && inst->ext.META_body_tracking_calibration ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_HAPTIC_PARAMETRIC_PROPERTIES_EXT:
            ((XrSystemHapticParametricPropertiesEXT*)next)->supportsParametricHaptics = inst->ext.EXT_haptic_parametric || inst->ext.EXTX1_haptic_parametric ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB:
            ((XrSystemPassthroughProperties2FB*)next)->capabilities = inst->ext.FB_passthrough ? XR_PASSTHROUGH_CAPABILITY_BIT_FB | XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB : 0;
            break;
        case XR_TYPE_SYSTEM_BOUNDARY_VISIBILITY_PROPERTIES_META:
            ((XrSystemBoundaryVisibilityPropertiesMETA*)next)->supportsBoundaryVisibility = inst->ext.META_boundary_visibility ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_COLOR_SPACE_PROPERTIES_FB:
            if (inst->ext.FB_color_space)
            {
                ((XrSystemColorSpacePropertiesFB*)next)->colorSpace = XR_COLOR_SPACE_REC709_FB; // the mirror is an sRGB monitor
            }
            break;
        case XR_TYPE_SYSTEM_PASSTHROUGH_COLOR_LUT_PROPERTIES_META:
            ((XrSystemPassthroughColorLutPropertiesMETA*)next)->maxColorLutResolution = inst->ext.META_passthrough_color_lut ? COMP_COLOR_LUT_MAX_RESOLUTION : 0;
            break;
        case XR_TYPE_SYSTEM_FOVEATION_EYE_TRACKED_PROPERTIES_META:
            ((XrSystemFoveationEyeTrackedPropertiesMETA*)next)->supportsFoveationEyeTracked = inst->ext.META_foveation_eye_tracked ? XR_TRUE : XR_FALSE;
            break;
        case XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB:
            if (inst->ext.FB_space_warp)
            {
                ((XrSystemSpaceWarpPropertiesFB*)next)->recommendedMotionVectorImageRectWidth  = config.view_width / 2;
                ((XrSystemSpaceWarpPropertiesFB*)next)->recommendedMotionVectorImageRectHeight = config.view_height / 2;
            }
            break;
        case XR_TYPE_SYSTEM_HEADSET_ID_PROPERTIES_META:
            if (inst->ext.META_headset_id)
            {
                static_assert(sizeof(config.headset_id) == XR_UUID_SIZE_EXT, "XR_META_headset_id is a UUID");
                memory_copy(((XrSystemHeadsetIdPropertiesMETA*)next)->id.data, config.headset_id, sizeof(config.headset_id));
            }
            break;
        default:
            break;
        }
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateEnvironmentBlendModes (XrInstance instance, XrSystemId system, XrViewConfigurationType view_type, uint32_t capacity, uint32_t* count_output, XrEnvironmentBlendMode* modes)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    if (system != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (view_type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    // Alpha blend shows the simulated room behind every layer, as if the display were see-through.
    // Spec XR_FB_composition_layer_alpha_blend: with it the runtime reports only opaque, since
    // the layers' blending decides the target's alpha
    const XrEnvironmentBlendMode supported[2] = { XR_ENVIRONMENT_BLEND_MODE_OPAQUE, XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND };
    const u32                    count        = inst->ext.FB_composition_layer_alpha_blend ? 1u : 2u;
    const XrResult               result       = oxr_two_call(capacity, count_output, modes, count);
    if (result == XR_SUCCESS && capacity > 0)
    {
        memory_copy(modes, supported, count * sizeof(supported[0]));
    }
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateViewConfigurations (XrInstance instance, XrSystemId system, uint32_t capacity, uint32_t* count_output, XrViewConfigurationType* types)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    if (system != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    const XrResult result = oxr_two_call(capacity, count_output, types, 1);
    if (result == XR_SUCCESS && capacity > 0)
    {
        types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    }
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetViewConfigurationProperties (XrInstance instance, XrSystemId system, XrViewConfigurationType view_type, XrViewConfigurationProperties* properties)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    if (system != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (view_type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    OXR_CHECK_TYPE(properties, XR_TYPE_VIEW_CONFIGURATION_PROPERTIES);
    properties->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    properties->fovMutable            = XR_TRUE; // projection views are reprojected from their own fov (place_projection_view)
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateViewConfigurationViews (XrInstance instance, XrSystemId system, XrViewConfigurationType view_type, uint32_t capacity, uint32_t* count_output, XrViewConfigurationView* views)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    if (system != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (view_type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    const XrResult result = oxr_two_call(capacity, count_output, views, OXR_VIEW_COUNT);
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }

    const SIM_Config& config = sim_get_config(runtime()->sim);
    for (u32 i = 0; i < OXR_VIEW_COUNT; i++)
    {
        OXR_CHECK_TYPE(&views[i], XR_TYPE_VIEW_CONFIGURATION_VIEW);
        views[i].recommendedImageRectWidth       = config.view_width;
        views[i].maxImageRectWidth               = OXR_MAX_IMAGE_SIZE;
        views[i].recommendedImageRectHeight      = config.view_height;
        views[i].maxImageRectHeight              = OXR_MAX_IMAGE_SIZE;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount         = 4;
    }
    return XR_SUCCESS;
}

// Paths

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrStringToPath (XrInstance instance, const char* str, XrPath* out_path)
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    OXR_CHECK(str && out_path);
    *out_path = XR_NULL_PATH;
    if (!path_string_valid(str))
    {
        return XR_ERROR_PATH_FORMAT_INVALID;
    }
    *out_path = path_intern(inst, str);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPathToString (XrInstance instance, XrPath path, uint32_t capacity, uint32_t* count_output, char* buffer)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    const char* str = path_string(inst, path);
    if (!str)
    {
        return XR_ERROR_PATH_INVALID;
    }
    const u32      length = (u32)cstr_length(str) + 1;
    const XrResult result = oxr_two_call(capacity, count_output, buffer, length);
    if (result == XR_SUCCESS && capacity > 0)
    {
        cstr_copy(buffer, capacity, str);
    }
    return result;
}

// Session

// What the mirror window presents through for a session of this backend. GLX draws only into X11
// windows of the app's visual, so for an Xlib binding the window moves to X11 (also on Wayland,
// through Xwayland) and is made again with that visual.
static b8 window_target_set (const R_WindowTarget& target)
{
    return target.vk_surface || target.native_window || target.metal_layer;
}

static R_WindowTarget mirror_window_target (OXR_Runtime* rt, OXR_Instance* inst, R_Backend backend, u32 x11_visual)
{
    if (backend == R_Backend::Vulkan)
    {
        oxr_vk_ensure_surface(inst);
        return { .vk_surface = inst->vk_surface };
    }
    if (backend == R_Backend::Metal)
    {
        return { .metal_layer = os_window_metal_layer(rt->window) };
    }
    const char* driver = os_video_driver();
    const b8    on_x11 = driver && cstr_equal(driver, "x11");
    if (x11_visual && (!on_x11 || rt->window_x11_visual != x11_visual))
    {
        u32 width = 0;
        u32 height = 0;
        os_window_size(rt->window, width, height);
        const b8 maximized = os_window_maximized(rt->window);
        oxr_vk_release_surface(inst);
        os_set_raw_event_hook(nullptr, nullptr);
        os_destroy_window(rt->window);
        if (!on_x11 && !os_set_video_driver("x11"))
        {
            LOG_WARNING("No X11 video for the GLX mirror window");
        }
        rt->window            = os_create_window("puppetxr", width, height, true, x11_visual, true);
        rt->window_shown      = false;
        rt->window_x11_visual = rt->window ? x11_visual : 0;
        if (!rt->window)
        {
            LOG_WARNING("Cannot make the mirror window with the app's visual 0x%x", x11_visual);
            rt->window = os_create_window("puppetxr", width, height, true, 0, true);
            if (maximized)
            {
                os_window_maximize(rt->window);
            }
            return {};
        }
        if (maximized)
        {
            os_window_maximize(rt->window);
        }
    }
    LOG_DEBUG("Mirror window for GL: driver %s, visual 0x%x", os_video_driver() ? os_video_driver() : "none", x11_visual);
    OS_WindowNative native = {};
    if (!os_window_native_handles(rt->window, native) || (x11_visual && native.wayland))
    {
        LOG_WARNING("The mirror window has no native window for GL; compositing offscreen only");
        return {};
    }
    return { .native_window = native.window, .native_display = native.display, .wayland = native.wayland };
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateSession (XrInstance instance, const XrSessionCreateInfo* info, XrSession* out_session)
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    OXR_CHECK_TYPE(info, XR_TYPE_SESSION_CREATE_INFO);
    OXR_CHECK(out_session);
    *out_session = XR_NULL_HANDLE;
    if (info->systemId != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }

    const XrBaseInStructure* graphics_binding = nullptr;
    u64                      event_channel    = 0;
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)info->next; next; next = next->next)
    {
        if ((next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR && (inst->ext.KHR_vulkan_enable || inst->ext.KHR_vulkan_enable2)) || oxr_gl_is_binding(inst, next->type) || oxr_d3d_is_binding(inst, next->type) || oxr_metal_is_binding(inst, next->type))
        {
            graphics_binding = next;
        }
        if ((u32)next->type == OXR_TYPE_EVENT_CHANNEL_TARGET_EXTX1 && inst->ext.EXTX1_event_channel)
        {
            event_channel = ((const OXR_EventChannelTargetEXTX1*)next)->channel;
            if (!find_event_channel(event_channel, instance_handle(instance)))
            {
                return XR_ERROR_HANDLE_INVALID;
            }
        }
    }
    if (!graphics_binding && !inst->ext.MND_headless)
    {
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }

    R_Device*        device     = nullptr;
    COMP_Compositor* compositor = nullptr;
    u32              x11_visual = 0;
    if (graphics_binding)
    {
        XrResult  result       = XR_SUCCESS;
        const u64 device_start = os_monotonic_ns();
        if (graphics_binding->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR)
        {
            result = oxr_vk_create_session_graphics(inst, graphics_binding, device);
        }
        else if (oxr_d3d_is_binding(inst, graphics_binding->type))
        {
            result = oxr_d3d_create_session_graphics(inst, graphics_binding, device);
        }
        else if (oxr_metal_is_binding(inst, graphics_binding->type))
        {
            result = oxr_metal_create_session_graphics(inst, graphics_binding, device);
        }
        else
        {
            result = oxr_gl_create_session_graphics(inst, graphics_binding, device, x11_visual);
        }
        oxr_startup.device += os_monotonic_ns() - device_start;
        if (result != XR_SUCCESS)
        {
            return result;
        }
        const SIM_Config& config           = sim_get_config(runtime()->sim);
        const u64         compositor_start = os_monotonic_ns();
        compositor                         = comp_create({ .device = device, .eye_width = config.view_width, .eye_height = config.view_height, .lens_mask_radius = lens_mask_radius(runtime()) });
        oxr_startup.compositor            += os_monotonic_ns() - compositor_start;
        if (!compositor)
        {
            LOG_ERROR("Cannot create the compositor for %ux%u per eye", config.view_width, config.view_height);
            r_destroy_device(device);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        // The control server answers in xrWaitFrame and xrPollEvent, on threads that may not use
        // the app's queue then (XR_KHR_vulkan_enable, Concurrency), so its readers take the copy
        // xrEndFrame reads back, and before the first frame there is none
        comp_set_snapshots(compositor, config.control);
    }

    R_WindowSwapchainHandle window_swapchain = {};
    R_WindowTarget          window_target    = {};
    UI_Context*             ui               = nullptr;
    if (device && runtime()->window)
    {
        window_target = mirror_window_target(runtime(), inst, r_device_backend(device), x11_visual);
    }
    if (device && runtime()->window && window_target_set(window_target))
    {
        u32 width = 0;
        u32 height = 0;
        os_window_size(runtime()->window, width, height);
        runtime()->window_pixel_width  = width;
        runtime()->window_pixel_height = height;
        runtime()->window_refresh_hz   = os_window_refresh_rate(runtime()->window);
        window_swapchain = r_create_window_swapchain(device, window_target, width, height);
        if (!window_swapchain)
        {
            LOG_WARNING("The app's queue cannot present to the mirror window; compositing offscreen only");
        }
#if !PUPPETXR_HEADLESS_ONLY
        else
        {
            const u64 ui_start = os_monotonic_ns();
            ui                 = ui_create({ .device = device, .window = runtime()->window, .target_format = r_window_swapchain_format(device, window_swapchain) });
            oxr_startup.ui    += os_monotonic_ns() - ui_start;
            os_set_raw_event_hook(ui ? ui_raw_event_hook : nullptr, ui);
            // puppetxrctl finds UI items by their labels (oxr_ui_click)
            if (ui)
            {
                ui_set_probe(ui, sim_get_config(runtime()->sim).control);
            }
        }
#endif
    }

    OXR_Runtime*            rt      = runtime();
    const OXR_SessionHandle handle  = rt->sessions.add();
    OXR_Session*            session = &rt->sessions.get(handle);
    session->instance               = instance_handle(instance);
    session->headless               = graphics_binding == nullptr;
    session->device                 = device;
    session->compositor             = compositor;
    session->window_swapchain       = window_swapchain;
    session->window_target          = window_target;
    rt->frame_end_ns                = 0; // the time since an earlier session is no frame
    session->ui                     = ui;
#if !PUPPETXR_HEADLESS_ONLY
    if (ui && rt->mouse_captured)
    {
        ui_set_captured(ui, true);
    }
#endif
    for (OXR_Profile& top_profile : session->profiles)
    {
        top_profile = OXR_Profile::None;
    }
    session->automation_profile     = OXR_Profile::None;
    session->color_space            = XR_COLOR_SPACE_REC709_FB;
    session->local_dimming          = XR_LOCAL_DIMMING_MODE_ON_META;
    session->boundary               = XR_BOUNDARY_VISIBILITY_NOT_SUPPRESSED_META;
    i64 origin_change_time          = 0;
    sim_local_origin_change(rt->sim, session->origin_generation, origin_change_time);
    session->origin_seen            = sim_get_reference_space_pose(rt->sim, SIM_ReferenceSpace::Local);
    session->state                  = XR_SESSION_STATE_UNKNOWN;
    session->event_channel          = event_channel;

    session_set_state(session, handle, XR_SESSION_STATE_IDLE);
    session_set_state(session, handle, XR_SESSION_STATE_READY);

    *out_session = session_xr(handle);
    startup_mark(oxr_startup.session_created);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroySession (XrSession session)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    destroy_session(session_handle(session));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrBeginSession (XrSession session, const XrSessionBeginInfo* info)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SESSION_BEGIN_INFO);
    if (info->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (sess->running)
    {
        return XR_ERROR_SESSION_RUNNING;
    }
    if (sess->state != XR_SESSION_STATE_READY)
    {
        return XR_ERROR_SESSION_NOT_READY;
    }

    sess->running = true;
    startup_mark(oxr_startup.session_running);
    if (sess->headless)
    {
        // Sessions without graphics may never submit frames, so they are synchronized at once
        const OXR_SessionHandle handle = session_handle(session);
        session_set_state(sess, handle, XR_SESSION_STATE_SYNCHRONIZED);
        session_set_state(sess, handle, XR_SESSION_STATE_VISIBLE);
        session_set_state(sess, handle, XR_SESSION_STATE_FOCUSED);
    }
    else
    {
        sess->synchronize_pending = true;
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEndSession (XrSession session)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    if (!sess->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (sess->state != XR_SESSION_STATE_STOPPING)
    {
        return XR_ERROR_SESSION_NOT_STOPPING;
    }

    sess->running             = false;
    sess->frame_waited        = false;
    sess->frame_begun         = false;
    sess->begin_failed        = false;
    sess->synchronize_pending = false;
    const OXR_SessionHandle handle = session_handle(session);
    session_set_state(sess, handle, XR_SESSION_STATE_IDLE);
    session_set_state(sess, handle, sess->exit_requested ? XR_SESSION_STATE_EXITING : XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrRequestExitSession (XrSession session)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    if (!sess->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    request_exit(sess, session_handle(session));
    return XR_SUCCESS;
}

// Frames

static b8 ui_takes_click (const OXR_Session* sess, f32 x, f32 y)
{
#if !PUPPETXR_HEADLESS_ONLY
    return sess->ui && ui_wants_click(sess->ui, x, y);
#else
    (void)sess;
    (void)x;
    (void)y;
    return false;
#endif
}

// Events are in points, the cache in pixels: on a window of high pixel density this takes a
// little beyond its right and bottom edges too
static b8 in_client_area (const OXR_Runtime* rt, f32 x, f32 y)
{
    return x >= 0.0f && y >= 0.0f && x < (f32)rt->window_pixel_width && y < (f32)rt->window_pixel_height;
}

static void capture_mouse (OXR_Runtime* rt, const OXR_Session* sess, b8 captured)
{
    if (rt->mouse_captured == captured)
    {
        return;
    }
    rt->mouse_captured          = captured;
    rt->window_relative_pending = true;
#if !PUPPETXR_HEADLESS_ONLY
    if (sess->ui)
    {
        ui_set_captured(sess->ui, captured);
    }
#else
    (void)sess;
#endif
    // Buttons held when the mouse goes are released, so no trigger stays pressed
    if (!captured)
    {
        rt->capture_click = false;
        sim_control_key(rt->sim, OS_Key::MouseLeft, false);
        sim_control_key(rt->sim, OS_Key::MouseRight, false);
        sim_control_key(rt->sim, OS_Key::MouseMiddle, false);
    }
}

// Mouse buttons reach the simulator only while the window holds the mouse, which a click into
// the view (not on a panel) takes and Escape or the loss of focus gives back. The panels get no
// input meanwhile (ui_set_captured)
static void pump_window_events (OXR_Runtime* rt, OXR_Session* sess)
{
    os_capture_output_poll();
    OS_Event  events[64];
    const u32 count = os_poll_queued_events(events, ARRAY_SIZE(events));
#if !PUPPETXR_HEADLESS_ONLY
    const b8 ui_keyboard = sess->ui && ui_wants_keyboard(sess->ui);
#else
    const b8 ui_keyboard = false;
#endif
    const SIM_Config& config = sim_get_config(rt->sim);

    for (u32 i = 0; i < count; i++)
    {
        const OS_Event& event    = events[i];
        const b8        is_mouse = event.key == OS_Key::MouseLeft || event.key == OS_Key::MouseRight || event.key == OS_Key::MouseMiddle;
        const b8        down     = event.type == OS_EventType::KeyDown;
        const b8        typing   = ui_keyboard && !rt->mouse_captured;
        switch (event.type)
        {
        case OS_EventType::KeyDown:
        case OS_EventType::KeyUp:
            // The panels are for the mouse: showing them gives it back, as Escape does
            if (event.key == config.key_ui && !typing)
            {
                rt->show_ui = down ? !rt->show_ui : rt->show_ui;
                if (down && rt->show_ui)
                {
                    capture_mouse(rt, sess, false);
                }
                break;
            }
            if ((down && welcome_key_locked(rt, event.key, typing)) || event.key == OS_Key::F1)
            {
                break;
            }
            if (down && event.key == OS_Key::Escape && rt->mouse_captured)
            {
                capture_mouse(rt, sess, false);
                break;
            }
            if (!rt->mouse_captured && is_mouse)
            {
                // Where the click is, not where the UI saw the mouse last frame: a click may come
                // with the move that brought it over a panel or away from one. The click that
                // focuses the window comes on the title bar too, as on its close button
                if (down && event.key == OS_Key::MouseLeft && rt->window && in_client_area(rt, event.x, event.y) && !ui_takes_click(sess, event.x, event.y))
                {
                    capture_mouse(rt, sess, true);
                    rt->capture_click = true;
                }
                break;
            }
            if (rt->capture_click && !down && event.key == OS_Key::MouseLeft)
            {
                rt->capture_click = false;
                break;
            }
            if (typing)
            {
                break;
            }
            sim_control_key(rt->sim, event.key, down);
            break;
        // The move that brings the mouse back into the window carries all the way it went
        // outside. A mouse reports small steps however fast it moves; a jump in one event is the
        // switch into relative mode or a cursor that went elsewhere
        case OS_EventType::MouseMotion:
            if (rt->mouse_captured && zm::abs(event.dx) <= OXR_MAX_LOOK_STEP && zm::abs(event.dy) <= OXR_MAX_LOOK_STEP)
            {
                sim_control_mouse_motion(rt->sim, event.dx, event.dy);
            }
            break;
        case OS_EventType::MouseWheel:
            if (rt->mouse_captured)
            {
                sim_control_wheel(rt->sim, event.dy);
            }
            break;
        case OS_EventType::FocusLost:
            capture_mouse(rt, sess, false);
            break;
        case OS_EventType::WindowClose:
            rt->window_closed = true;
            break;
        case OS_EventType::WindowResize:
            sess->window_resized     = true;
            rt->window_pixel_width   = event.width;
            rt->window_pixel_height  = event.height;
            break;
        default:
            break;
        }
    }
}

// A file in the screenshot directory, which is created when missing
static void output_path (char* out, usize size, const char* name, u64 frame, const char* extension)
{
    const SIM_Config& config = sim_get_config(runtime()->sim);
    if (config.screenshot_dir[0] && !os_create_directories(config.screenshot_dir))
    {
        LOG_ERROR("Cannot create %s", config.screenshot_dir);
    }
    cstr_format(out, size, "%s%s%s_%llu.%s", config.screenshot_dir, config.screenshot_dir[0] ? "/" : "", name, (unsigned long long)frame, extension);
}

#if !PUPPETXR_HEADLESS_ONLY
static UI_Status ui_status_locked (OXR_Runtime* rt)
{
    const OXR_VirtualKeyboard* keyboard = first_keyboard(rt);
    return {
        .recording        = rt->recorder != nullptr,
        .recorded_frames  = rt->recorder ? rec_writer_frame_count(rt->recorder) : 0,
        .record_path      = rt->record_path,
        .replaying        = rt->replay && !rt->replay_finished,
        .replay_finished  = rt->replay && rt->replay_finished,
        .replay_frame     = rt->replay_cursor,
        .replay_total     = rt->replay ? rec_frame_count(rt->replay) : 0,
        .divergences      = rt->replay_divergences,
        .first_divergence = rt->replay_first_divergence,
        .rooms            = rt->room_names,
        .rooms_count      = rt->room_names_count,
        .room             = rt->room,
        .anchors_stored   = (u32)rt->store.anchors.length,
        .room_from_stage  = rt->room_from_stage,
        .keyboard_exists   = keyboard != nullptr,
        .keyboard_visible  = keyboard && keyboard->visible,
        .keyboard_shifted  = keyboard && keyboard->shifted,
        .show_welcome      = rt->show_welcome,
        .welcome_dismissed = rt->welcome_dismissed,
        .keyboard_context  = keyboard ? keyboard->context : nullptr,
        .lens_mask         = rt->lens_mask,
        .mouse_captured    = rt->mouse_captured,
        .frame_times       = &rt->frame_times,
    };
}

static void apply_ui_requests_locked (OXR_Runtime* rt, const OXR_Session* sess, const UI_Requests& requests)
{
    if (requests.screenshot)
    {
        char path[OS_MAX_PATH];
        output_path(path, sizeof(path), "puppetxr_frame", sim_frame_index(rt->sim), "png");
        comp_save_png(sess->compositor, path);
    }
    if (requests.recenter)
    {
        sim_recenter(rt->sim);
    }
    if (requests.record_toggle)
    {
        if (rt->recorder)
        {
            record_stop_locked(rt);
        }
        else
        {
            char path[OS_MAX_PATH];
            output_path(path, sizeof(path), "puppetxr_recording", sim_frame_index(rt->sim), "xrrec");
            record_start_locked(rt, path);
        }
    }
    if (requests.replay_stop)
    {
        replay_stop_locked(rt);
    }
    if (requests.set_controller)
    {
        set_controller_locked(rt, requests.controller);
    }
    if (requests.set_room)
    {
        choose_room_locked(rt, requests.room);
    }
    if (requests.wipe_anchors)
    {
        wipe_anchors_locked(rt, requests.wipe_cloud);
    }
    if (requests.toggle_welcome)
    {
        welcome_key_locked(rt, OS_Key::F1, false);
    }
    if (requests.set_welcome_at_start)
    {
        remember_welcome_locked(rt, !requests.welcome_at_start);
    }
    if (requests.set_lens_mask)
    {
        set_lens_mask_locked(rt, requests.lens_mask);
    }
    if (requests.quit)
    {
        rt->window_closed = true;
    }
}
#endif

// From one xrEndFrame to the next, and the part of it the lock was held, the current hold up to now
static void time_frame (OXR_Runtime* rt)
{
    const u64 now = os_monotonic_ns();
    UI_FrameTimes& times = rt->frame_times;
    if (rt->frame_end_ns)
    {
        times.frame_ms[times.next] = (f32)((f64)(now - rt->frame_end_ns) * 1e-6);
        times.puppetxr_ms[times.next] = (f32)((f64)(rt->frame_locked_ns + (now - rt->locked_since)) * 1e-6);
        times.next                 = (times.next + 1) % UI_FRAME_TIMES;
        times.count                = zm::min(times.count + 1, (u32)UI_FRAME_TIMES);
    }
    rt->frame_end_ns    = now;
    rt->frame_locked_ns = 0;
    rt->locked_since    = now;
    if (now - rt->mirror_second_start >= 1000000000ull)
    {
        const u64 presents         = times.mirror_presents - rt->mirror_second_presents;
        times.mirror_fps           = rt->mirror_second_start ? (f32)((f64)presents * 1e9 / (f64)(now - rt->mirror_second_start)) : 0.0f;
        rt->mirror_second_start    = now;
        rt->mirror_second_presents = times.mirror_presents;
    }
}

static void present_frame (OXR_Runtime* rt, OXR_Session* sess)
{
    PROF_ZONE("mirror window");
    // A window whose swapchain could not be made tries again at its next resize; a hidden one gets
    // no frame, so the first one shown is drawn at the shown size
    if (!rt->window_shown || !sess->device || (!sess->window_swapchain && !sess->window_resized) || !window_target_set(sess->window_target))
    {
        return;
    }
    R_Device* device = sess->device;

    if (sess->window_resized)
    {
        sess->window_resized = false;
#if !PUPPETXR_HEADLESS_ONLY
        if (sess->ui)
        {
            ui_release_targets(sess->ui);
        }
#endif
        r_destroy_window_swapchain(device, sess->window_swapchain);
        const u32 width        = rt->window_pixel_width;
        const u32 height       = rt->window_pixel_height;
        sess->window_swapchain = width > 0 && height > 0 ? r_create_window_swapchain(device, sess->window_target, width, height) : R_WindowSwapchainHandle{};
        if (!sess->window_swapchain)
        {
            return;
        }
    }

    // At most at the display's rate: the window server holds a presented drawable until its next
    // composite, and at 124 presents a second all three were held and the acquire waited
    const u64 now = os_monotonic_ns();
    if (rt->window_refresh_hz > 0.0f && now - sess->window_presented_ns < (u64)(1e9f / rt->window_refresh_hz))
    {
        return;
    }
    // Nor does it wait for the GPU: while its last present is still there, this frame goes without
    if (sess->window_swapchain && r_window_busy(device, sess->window_swapchain))
    {
        PROF_ZONE("mirror skipped");
        return;
    }
    sess->window_presented_ns = now;

    R_ImageHandle window_image;
    if (!r_window_acquire(device, sess->window_swapchain, window_image))
    {
        sess->window_resized = true;
        return;
    }

    // Fit the native-resolution target into the window without ever upscaling it
    u32 width = 0;
    u32 height = 0;
    u32 target_width = 0;
    u32 target_height = 0;
    r_window_swapchain_size(device, sess->window_swapchain, width, height);
    comp_target_size(sess->compositor, target_width, target_height);
    const f32    fit   = zm::min((f32)width / (f32)target_width, (f32)height / (f32)target_height);
    const f32    scale = zm::min(fit, 1.0f);
    const u32    dst_w = zm::max((u32)((f32)target_width * scale), 1u);
    const u32    dst_h = zm::max((u32)((f32)target_height * scale), 1u);
    const R_Rect dst   = { (i32)(width - dst_w) / 2, (i32)(height - dst_h) / 2, dst_w, dst_h };

    const R_CmdBufferHandle cmd = r_begin_frame(device);
    r_cmd_transition_image(device, cmd, comp_target(sess->compositor), R_ImageLayout::ColorAttachment, R_ImageLayout::TransferSrc);
    r_cmd_transition_image(device, cmd, window_image, R_ImageLayout::Undefined, R_ImageLayout::TransferDst);
    r_cmd_clear_image(device, cmd, window_image, { 0.0f, 0.0f, 0.0f, 1.0f });
    r_cmd_blit_image(device, cmd, comp_target(sess->compositor), 0, window_image, dst, R_Filter::Linear);
    r_cmd_transition_image(device, cmd, comp_target(sess->compositor), R_ImageLayout::TransferSrc, R_ImageLayout::ColorAttachment);

#if !PUPPETXR_HEADLESS_ONLY
    if (sess->ui)
    {
        PROF_ZONE("ui render");
        ui_render(sess->ui, cmd, window_image);
    }
    else
#endif
    {
        r_cmd_transition_image(device, cmd, window_image, R_ImageLayout::TransferDst, R_ImageLayout::Present);
    }
    r_end_frame_present(device, cmd, sess->window_swapchain);
    if (!r_window_present(device, sess->window_swapchain))
    {
        sess->window_resized = true;
        return;
    }
    rt->frame_times.mirror_presents++;

}

// The window's part of a frame, on the window thread after the frame: on a frame thread of
// its own every window call would wait for a main thread that may be blocked on that thread
// (Unity's), so frames only post this. Events are pumped before the runtime lock is taken: the
// pump runs the app's run loop, where an editor's tick may wait for a frame holding the lock
static void window_tick (void* user) // NOLINT(misc-const-correctness): OS_WindowThreadFunction
{
    (void)user;
    PROF_ZONE("window tick");
    os_pump_window_events();
    const OXR_Lock lock(__func__);
    OXR_Runtime*   rt      = runtime();
    rt->window_tick_pending = false;
    if (!rt->window)
    {
        return;
    }
    if (!rt->window_shown && rt->sessions.length() > 0)
    {
        PROF_ZONE("window show");
        os_window_show(rt->window);
        rt->window_shown = true;
        for (OXR_Session& shown : rt->sessions)
        {
            shown.window_resized = shown.window_resized || shown.window_swapchain;
        }
    }
    if (rt->window_relative_pending)
    {
        rt->window_relative_pending = false;
        os_window_set_relative_mouse(rt->window, rt->mouse_captured);
    }
    os_window_keep_cursor(rt->window, rt->mouse_captured);
    rt->window_refresh_hz = os_window_refresh_rate(rt->window);
    u32 width  = 0;
    u32 height = 0;
    os_window_size(rt->window, width, height);
    if (width > 0 && height > 0 && (width != rt->window_pixel_width || height != rt->window_pixel_height))
    {
        rt->window_pixel_width  = width;
        rt->window_pixel_height = height;
        for (OXR_Session& resized : rt->sessions)
        {
            resized.window_resized = resized.window_resized || resized.window_swapchain;
        }
    }
#if !PUPPETXR_HEADLESS_ONLY
    for (const OXR_Session& sess : rt->sessions)
    {
        if (sess.ui)
        {
            PROF_ZONE("ui build");
            const UI_Requests requests = ui_build(sess.ui, rt->sim, rt->show_ui, rt->mirror_view, ui_status_locked(rt));
            apply_ui_requests_locked(rt, &sess, requests);
            break;
        }
    }
#endif
}

// Runs the tick at once where the frame ended on the window thread, else posts one
static void schedule_window_tick (OXR_Runtime* rt)
{
    if (!rt->window || rt->window_tick_pending)
    {
        return;
    }
    if (os_is_window_thread())
    {
        window_tick(nullptr);
        return;
    }
    rt->window_tick_pending = true;
    os_post_to_window_thread(window_tick, nullptr);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrWaitFrame (XrSession session, const XrFrameWaitInfo* info, XrFrameState* state)
{
    PROF_ZONE(__func__);
    OXR_Runtime* rt = runtime();
    oxr_lock();
    defer { oxr_unlock(); };
    if (g_control_hooks.poll)
    {
        g_control_hooks.poll(true);
    }

    OXR_GET_SESSION(sess, session);
    OXR_CHECK(!info || info->type == XR_TYPE_FRAME_WAIT_INFO);
    OXR_CHECK_TYPE(state, XR_TYPE_FRAME_STATE);
    if (!sess->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    const XrResult fault = take_fault(rt->frame_faults[(u32)OXR_FrameOp::Wait]);
    if (fault != XR_SUCCESS)
    {
        return fault;
    }

    // Spec 10.4.1: a second xrWaitFrame blocks until the previous frame was begun. With no other
    // thread to begin it that is forever, so after OXR_FRAME_GATE_TIMEOUT_NS it warns and goes on.
    // A paused simulator waits for its steps in the poll hook above, which this does not count.
    // An older frame begun and not ended yet does not open the gate; a waited frame whose
    // xrBeginFrame a frame fault failed holds it no longer.
    const OXR_SessionHandle handle     = session_handle(session);
    const u64               gate_start = os_monotonic_ns();
    while (sess->frame_waited && !sess->begin_failed)
    {
        if (os_monotonic_ns() - gate_start >= (u64)OXR_FRAME_GATE_TIMEOUT_NS)
        {
            LOG_WARNING("xrWaitFrame waited %lld ms for xrBeginFrame of the previous frame, which no other thread called; every waited frame must be begun. Continuing.", OXR_FRAME_GATE_TIMEOUT_NS / 1000000);
            break;
        }
        oxr_unlock();
        {
            PROF_ZONE("frame gate wait");
            os_sleep_ns(OXR_FRAME_GATE_POLL_NS);
        }
        oxr_lock();
        sess = rt->sessions.try_get(handle);
        if (!sess)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (!sess->running)
        {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
    }

    startup_mark(oxr_startup.first_wait);
    if (rt->window)
    {
        PROF_ZONE("window events");
        pump_window_events(rt, sess);
    }

    const u64 now = os_time_ns();
    {
        PROF_ZONE("sim step");
        advance_frame(rt, sess, handle, now);
    }
    OXR_Instance* events_owner = session_instance(sess);
    if (events_owner)
    {
        release_delayed_events(events_owner, sim_frame_index(rt->sim));
    }
    poll_colocation(rt, sess, handle);
    poll_spatial_contexts(rt, sess, handle);
    rt->clock_anchor_time      = sim_time(rt->sim);
    rt->clock_anchor_monotonic = os_monotonic_ns();
    rt->clock_anchor_timespec  = os_timespec_ns();
    if (rt->exit_requested && !sess->exit_requested)
    {
        request_exit(sess, handle);
    }
    if (rt->window_closed && !sess->exit_requested)
    {
        LOG_INFO("Window closed, requesting session exit");
        request_exit(sess, handle);
    }

    sess->frame_waited        = true;
    sess->begin_failed        = false;
    sess->wait_thread         = os_thread_current_id();
    sess->waited_display_time = sim_predicted_display_time(rt->sim);

    state->predictedDisplayTime   = sess->waited_display_time;
    state->predictedDisplayPeriod = sim_display_period(rt->sim);
    state->shouldRender           = (!sess->headless && (sess->state == XR_SESSION_STATE_VISIBLE || sess->state == XR_SESSION_STATE_FOCUSED)) ? XR_TRUE : XR_FALSE;

    if (sim_clock_mode(rt->sim) == SIM_ClockMode::Realtime)
    {
        const u64 period  = (u64)sim_display_period(rt->sim);
        const u64 elapsed = !sess->last_wait_os_time ? period : now > sess->last_wait_os_time ? now - sess->last_wait_os_time : 0;
        // Measured from where this wait returns: from its start, the next wait counts this sleep
        // as the app's frame and skips its own
        sess->last_wait_os_time = elapsed < period ? now + (period - elapsed) : now;
        if (elapsed < period)
        {
            oxr_unlock();
            {
                PROF_ZONE("frame pacing");
                os_sleep_ns(period - elapsed);
            }
            oxr_lock();
        }
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrBeginFrame (XrSession session, const XrFrameBeginInfo* info)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK(!info || info->type == XR_TYPE_FRAME_BEGIN_INFO);
    if (!sess->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (!sess->frame_waited)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    const XrResult fault = take_fault(runtime()->frame_faults[(u32)OXR_FrameOp::Begin]);
    if (fault != XR_SUCCESS)
    {
        sess->begin_failed = true;
        return fault;
    }
    const b8 discarded = sess->frame_begun;
    sess->frame_waited      = false;
    sess->begin_failed      = false;
    sess->frame_begun       = true;
    sess->frames_begun++;
    sess->frames_discarded += discarded ? 1 : 0;
    return discarded ? XR_FRAME_DISCARDED : XR_SUCCESS;
}

static XrResult validate_depth_sub_image (const OXR_Session* sess, const XrSwapchainSubImage& sub_image, COMP_DepthView& out_view)
{
    OXR_GET_CONST_SWAPCHAIN(swapchain, sub_image.swapchain);
    if (swapchain->session != session_handle_of(sess))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(swapchain->depth && swapchain->face_count == 1 && sub_image.imageArrayIndex < swapchain->array_size);
    const XrRect2Di& rect = sub_image.imageRect;
    if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0 || (u32)(rect.offset.x + rect.extent.width) > swapchain->width || (u32)(rect.offset.y + rect.extent.height) > swapchain->height)
    {
        return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    }
    if (swapchain->last_released < 0)
    {
        return XR_ERROR_LAYER_INVALID;
    }
    out_view = { .image = swapchain->images[swapchain->last_released], .array_layer = sub_image.imageArrayIndex, .rect = { rect.offset.x, rect.offset.y, (u32)rect.extent.width, (u32)rect.extent.height } };
    return XR_SUCCESS;
}

// Spec 12.17: the depth sub-image of a projection view. Its values are affine in the inverse view
// depth between nearZ at minDepth and farZ at maxDepth, either of them infinite, and so is the
// compositor's depth 1 - COMP_DEPTH_NEAR / view depth, which XR_FB_composition_layer_depth_test
// compares: the map is the one affine function between them.
static XrResult validate_depth_info (const OXR_Session* sess, const XrCompositionLayerDepthInfoKHR& depth, COMP_DepthView& out_view)
{
    OXR_CHECK(depth.minDepth >= 0.0f && depth.maxDepth <= 1.0f && depth.minDepth < depth.maxDepth);
    OXR_CHECK(depth.nearZ > 0.0f && depth.farZ > 0.0f && depth.nearZ != depth.farZ);
    const XrResult result = validate_depth_sub_image(sess, depth.subImage, out_view);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    const f32 comp_near = (f32)COMP_DEPTH_NEAR;
    const f32 inv_near  = zm::is_finite(depth.nearZ) ? 1.0f / depth.nearZ : 0.0f;
    const f32 inv_far   = zm::is_finite(depth.farZ) ? 1.0f / depth.farZ : 0.0f;
    const f32 slope     = -comp_near * (inv_far - inv_near) / (depth.maxDepth - depth.minDepth);
    out_view.map[0]     = 1.0f - comp_near * inv_near - slope * depth.minDepth;
    out_view.map[1]     = slope;
    out_view.valid      = true;
    return XR_SUCCESS;
}

static XrResult validate_sub_image (const OXR_Session* sess, const XrSwapchainSubImage& sub_image, COMP_View& out_view)
{
    OXR_GET_CONST_SWAPCHAIN(swapchain, sub_image.swapchain);
    if (swapchain->session != session_handle_of(sess))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (sub_image.imageArrayIndex >= swapchain->array_size || swapchain->face_count != 1)
    {
        OXR_FAIL("sub_image.imageArrayIndex >= swapchain->array_size || swapchain->face_count != 1");
    }
    const XrRect2Di& rect = sub_image.imageRect;
    if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0 || (u32)(rect.offset.x + rect.extent.width) > swapchain->width || (u32)(rect.offset.y + rect.extent.height) > swapchain->height)
    {
        return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    }
    if (swapchain->last_released < 0 || swapchain->depth)
    {
        return XR_ERROR_LAYER_INVALID;
    }
    out_view = { .image = swapchain->images[swapchain->last_released], .array_layer = sub_image.imageArrayIndex, .rect = { rect.offset.x, rect.offset.y, (u32)rect.extent.width, (u32)rect.extent.height }, .nearest = swapchain->sampler.mag_filter == 0 };
    return XR_SUCCESS;
}

// XR_FB_space_warp: motion vectors and depth for frame extrapolation, which the compositor does
// not do, so they are validated and dropped. The delta pose is not validated: the spec has no rule
// for it and CTS 1.1.63 sends an all-zero one.
static XrResult validate_space_warp_info (const OXR_Session* sess, const XrCompositionLayerSpaceWarpInfoFB& info)
{
    OXR_CHECK((info.layerFlags & ~(XrCompositionLayerSpaceWarpInfoFlagsFB)XR_COMPOSITION_LAYER_SPACE_WARP_INFO_FRAME_SKIP_BIT_FB) == 0);
    OXR_CHECK(info.minDepth >= 0.0f && info.maxDepth <= 1.0f && info.minDepth <= info.maxDepth);
    OXR_CHECK(info.nearZ >= 0.0f && info.farZ >= 0.0f && info.nearZ != info.farZ);
    COMP_View      motion = {};
    const XrResult result = validate_sub_image(sess, info.motionVectorSubImage, motion);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    COMP_DepthView depth = {};
    return validate_depth_sub_image(sess, info.depthSubImage, depth);
}

namespace
{

struct OXR_Eye
{
    Pose pose; // in stage space at the frame's display time
    Fov  fov;
};

} // namespace

// Vulkan clip coordinates of a point in eye space (looking down -Z) for an asymmetric fov,
// y pointing down and depth in [0, w]
static void eye_clip (Vec3 point, const Fov& fov, f32* out_clip)
{
    const f32 left  = zm::tan(fov.angle_left);
    const f32 right = zm::tan(fov.angle_right);
    const f32 up    = zm::tan(fov.angle_up);
    const f32 down  = zm::tan(fov.angle_down);
    const f32 w     = -point.z;
    out_clip[0]     = (2.0f * point.x - w * (right + left)) / (right - left);
    out_clip[1]     = -(2.0f * point.y - w * (up + down)) / (up - down);
    out_clip[2]     = w - (f32)COMP_DEPTH_NEAR;
    out_clip[3]     = w;
}

static b8 fov_angle_valid (f32 angle)
{
    return zm::is_finite(angle) && angle > -1.57f && angle < 1.57f;
}

// A flipped fov (left > right or down > up) is a mirrored image of the same region; only an
// empty or unprojectable one is not drawn
static b8 fov_valid (const XrFovf& fov)
{
    return fov_angle_valid(fov.angleLeft) && fov_angle_valid(fov.angleRight) && fov_angle_valid(fov.angleUp) && fov_angle_valid(fov.angleDown) && fov.angleLeft != fov.angleRight && fov.angleDown != fov.angleUp;
}

// The app rendered this view from its own pose; it is reprojected for rotation only, which
// places the image at infinity and is exact when the poses agree
static void place_projection_view (const Pose& view_world, const XrFovf& fov, const OXR_Eye& eye, COMP_View& view)
{
    const Quat rotation = zm::inverse(eye.pose.orientation) * view_world.orientation;
    const f32  left     = zm::tan(fov.angleLeft);
    const f32  right    = zm::tan(fov.angleRight);
    const f32  up       = zm::tan(fov.angleUp);
    const f32  down     = zm::tan(fov.angleDown);
    const Vec3 corners[4] = { { left, up, -1.0f }, { right, up, -1.0f }, { left, down, -1.0f }, { right, down, -1.0f } };
    // At infinity, depth 1: the image is not clipped near the eye as quads are
    for (u32 c = 0; c < 4; c++)
    {
        eye_clip(zm::rotate(rotation, corners[c]), eye.fov, view.corners[c]);
        view.corners[c][2] = view.corners[c][3];
    }
    view.visible = true;
}

// Spec 10.6.3: only the front face, the +Z side of the quad, is drawn
static void place_quad (const Pose& quad_world, const XrExtent2Df& size, const OXR_Eye& eye, COMP_View& view)
{
    const Vec3 normal = zm::rotate(quad_world.orientation, { 0.0f, 0.0f, 1.0f });
    if (zm::dot(normal, eye.pose.position - quad_world.position) <= 0.0f || size.width <= 0.0f || size.height <= 0.0f)
    {
        view.visible = false;
        return;
    }
    const f32  x          = size.width * 0.5f;
    const f32  y          = size.height * 0.5f;
    const Vec3 corners[4] = { { -x, y, 0.0f }, { x, y, 0.0f }, { -x, -y, 0.0f }, { x, -y, 0.0f } };
    const Quat to_eye     = zm::inverse(eye.pose.orientation);
    for (u32 c = 0; c < 4; c++)
    {
        const Vec3 world = quad_world.position + zm::rotate(quad_world.orientation, corners[c]);
        eye_clip(zm::rotate(to_eye, world - eye.pose.position), eye.fov, view.corners[c]);
    }
    view.visible = true;
}

// Spec 12.7, 12.10: the ranges of layer angles are in their member descriptions, not in valid
// usage, so an angle outside is clamped (xrgears' sky uses 2 * 3.142). One past by more than an
// app's rounding of 2 pi or pi / 2 is reported through out_far.
static f32 clamp_layer_angle (f32 angle, f32 low, f32 high, b8& out_far)
{
    const f32 clamped = zm::clamp(angle, low, high);
    out_far           = out_far || zm::abs(angle - clamped) > 1e-4f;
    return clamped;
}

// Cylinder and equirect layers are ray cast by the compositor in the layer's frame: the ray of a
// pixel is linear in its NDC, from the tangents of the eye's fov
static void place_curved (const Pose& layer_world, const OXR_Eye& eye, COMP_View& view)
{
    const f32  left     = zm::tan(eye.fov.angle_left);
    const f32  right    = zm::tan(eye.fov.angle_right);
    const f32  up       = zm::tan(eye.fov.angle_up);
    const f32  down     = zm::tan(eye.fov.angle_down);
    const Vec3 rays[3]  = { { 0.5f * (right - left), 0.0f, 0.0f }, { 0.0f, -0.5f * (up - down), 0.0f }, { 0.5f * (right + left), 0.5f * (up + down), -1.0f } };
    const Quat to_layer = zm::inverse(layer_world.orientation);
    const Vec3 origin   = zm::rotate(to_layer, eye.pose.position - layer_world.position);
    for (u32 i = 0; i < 3; i++)
    {
        const Vec3 ray  = zm::rotate(to_layer * eye.pose.orientation, rays[i]);
        view.rays[i][0] = ray.x;
        view.rays[i][1] = ray.y;
        view.rays[i][2] = ray.z;
    }
    view.rays[0][3] = origin.x;
    view.rays[1][3] = origin.y;
    view.rays[2][3] = origin.z;
    view.visible    = true;
}

static b8 eye_visibility_valid (XrEyeVisibility visibility)
{
    return visibility == XR_EYE_VISIBILITY_BOTH || visibility == XR_EYE_VISIBILITY_LEFT || visibility == XR_EYE_VISIBILITY_RIGHT;
}

static b8 eye_shown (XrEyeVisibility visibility, u32 eye)
{
    return visibility == XR_EYE_VISIBILITY_BOTH || (visibility == XR_EYE_VISIBILITY_LEFT && eye == 0) || (visibility == XR_EYE_VISIBILITY_RIGHT && eye == 1);
}

static void forget_retired_spaces (OXR_SessionHandle session)
{
    OXR_Runtime* rt   = runtime();
    u32          kept = 0;
    for (u32 i = 0; i < rt->retired_spaces_count; i++)
    {
        if (!(rt->retired_spaces[i].space.session == session))
        {
            rt->retired_spaces[kept++] = rt->retired_spaces[i];
        }
    }
    rt->retired_spaces_count = kept;
}

// A layer's space: a live one, or one destroyed since the session's last xrEndFrame
static const OXR_Space* layer_space (XrSpace handle)
{
    if (const OXR_Space* space = oxr_find_space(handle))
    {
        return space;
    }
    const OXR_Runtime*    rt      = runtime();
    const OXR_SpaceHandle decoded = decode_handle<OXR_Space>(OXR_HandleType::Space, (u64)(uintptr_t)handle);
    for (u32 i = 0; i < rt->retired_spaces_count; i++)
    {
        if (rt->retired_spaces[i].handle == decoded)
        {
            LOG_WARNING("A layer names space 0x%llx, destroyed during this frame; composited where it was", (unsigned long long)(uintptr_t)handle);
            return &rt->retired_spaces[i].space;
        }
    }
    return nullptr;
}

// Spec 12.7, 12.9, 12.10: radius 0 and +infinity are an infinite surface; uv_map is only drawn
// when every coefficient is finite, so degenerate angles, aspect ratios and scales show nothing
static XrResult place_curved_layer (const OXR_Session* sess, XrSpace space_handle, XrEyeVisibility visibility, const XrSwapchainSubImage& sub_image, const XrPosef& pose, f32 radius, const SIM_Sample& sample, const OXR_Eye* eyes, COMP_Layer& out_layer)
{
    const OXR_Space* space = layer_space(space_handle);
    if (!space)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!xr_pose_valid(pose))
    {
        return XR_ERROR_POSE_INVALID;
    }
    OXR_CHECK(eye_visibility_valid(visibility) && radius >= 0.0f); // false for NaN
    COMP_View      image  = {};
    const XrResult result = validate_sub_image(sess, sub_image, image);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    out_layer.radius = zm::is_finite(radius) ? radius : 0.0f;
    b8 drawable      = true;
    for (const f32 coefficient : out_layer.uv_map)
    {
        drawable &= zm::is_finite(coefficient);
    }
    const OXR_Located located = locate_world(space, sample);
    for (u32 e = 0; e < OXR_VIEW_COUNT; e++)
    {
        out_layer.views[e] = image;
        if (located.valid && drawable && eye_shown(visibility, e))
        {
            place_curved(zm::pose_mul(located.pose, pose_from_xr(pose)), eyes[e], out_layer.views[e]);
        }
    }
    return XR_SUCCESS;
}

// Passthrough shows the room from the eyes at the display time; near and far bound a room
#define OXR_PASSTHROUGH_NEAR 0.05f
#define OXR_PASSTHROUGH_FAR  1000.0f

static void render_cameras (const OXR_Session* sess, OXR_SessionHandle session, XrTime display_time, const SIM_Sample& sample);
static void render_depth   (const OXR_Session* sess, OXR_SessionHandle session, XrTime display_time, const SIM_Sample& sample);

static const COMP_PassthroughStyle oxr_default_passthrough_style = { .opacity = 1.0f, .contrast = 1.0f, .saturation = 1.0f };

static void place_passthrough (const OXR_Eye* eyes, COMP_Layer& out_layer)
{
    out_layer.type = COMP_LayerType::Passthrough;
    for (u32 e = 0; e < OXR_VIEW_COUNT; e++)
    {
        const Pose eye_in_room    = zm::pose_mul(runtime()->room_from_stage, eyes[e].pose);
        const Mat4 clip_from_room = zm::mat4_projection(eyes[e].fov, OXR_PASSTHROUGH_NEAR, OXR_PASSTHROUGH_FAR) * zm::inverse(zm::mat4_from_pose(eye_in_room));
        memory_copy(out_layer.clip_from_room[e], clip_from_room.m, sizeof(clip_from_room.m));
        out_layer.eye_visible[e] = true;
    }
}

// Projected passthrough: the layer's geometry instances whose mesh finished an update, located at
// the display time; the room shows only on them
static void place_geometry (OXR_Session* sess, OXR_PassthroughLayerHandle layer, const SIM_Sample& sample, COMP_Layer& out_layer)
{
    OXR_Runtime* rt     = runtime();
    out_layer.projected = true;
    out_layer.geometry  = sess->geometry + sess->geometry_count;
    for (const OXR_GeometryInstance& instance : rt->geometry_instances)
    {
        const OXR_TriangleMesh* mesh  = rt->triangle_meshes.try_get(instance.mesh);
        const OXR_Space*        space = rt->spaces.try_get(instance.base_space);
        if (!(instance.layer == layer) || !mesh || mesh->triangles_count == 0 || !space || sess->geometry_count == OXR_MAX_GEOMETRY_INSTANCES)
        {
            continue;
        }
        const OXR_Located located = locate_world(space, sample);
        if (!located.valid)
        {
            continue;
        }
        const Pose     room_from_mesh = zm::pose_mul(rt->room_from_stage, zm::pose_mul(located.pose, instance.pose));
        const Mat4     model          = zm::mat4_from_pose(room_from_mesh) * zm::mat4_scale(instance.scale);
        COMP_Geometry& geometry       = sess->geometry[sess->geometry_count++];
        geometry                      = {
            .positions      = mesh->drawn_vertices.data,
            .indices        = mesh->drawn_indices.data,
            .vertices_count = mesh->vertices_count,
            .indices_count  = mesh->triangles_count * 3,
        };
        for (u32 e = 0; e < OXR_VIEW_COUNT; e++)
        {
            Mat4 clip_from_room = {};
            memory_copy(clip_from_room.m, out_layer.clip_from_room[e], sizeof(clip_from_room.m));
            const Mat4 clip_from_mesh = clip_from_room * model;
            memory_copy(geometry.clip_from_mesh[e], clip_from_mesh.m, sizeof(clip_from_mesh.m));
        }
        out_layer.geometry_count++;
    }
}

static XrResult validate_layer (OXR_Session* sess, const XrCompositionLayerBaseHeader* header, const SIM_Sample& sample, const OXR_Eye* eyes, COMP_Layer& out_layer)
{
    if (!header)
    {
        return XR_ERROR_LAYER_INVALID;
    }
    out_layer                 = {};
    out_layer.source_alpha    = (header->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
    out_layer.unpremultiplied = (header->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) != 0;
    out_layer.inverted_alpha  = (header->layerFlags & XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT) != 0;

    // Spec 10.6.1: layerFlags holds only defined bits, of core and of enabled extensions
    const OXR_Instance* options = session_instance(sess);
    if (header->type != XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB)
    {
        const XrCompositionLayerFlags inverted = options && options->ext.EXT_composition_layer_inverted_alpha ? XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT : 0;
        const XrCompositionLayerFlags valid    = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT | XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT | inverted;
        OXR_CHECK((header->layerFlags & ~valid) == 0);
    }
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)header->next; next && options; next = next->next)
    {
        if (next->type == XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR && options->ext.KHR_composition_layer_color_scale_bias)
        {
            const XrCompositionLayerColorScaleBiasKHR* color = (const XrCompositionLayerColorScaleBiasKHR*)next;
            out_layer.color_transform = true;
            out_layer.color_scale[0]  = color->colorScale.r;
            out_layer.color_scale[1]  = color->colorScale.g;
            out_layer.color_scale[2]  = color->colorScale.b;
            out_layer.color_scale[3]  = color->colorScale.a;
            out_layer.color_bias[0]   = color->colorBias.r;
            out_layer.color_bias[1]   = color->colorBias.g;
            out_layer.color_bias[2]   = color->colorBias.b;
            out_layer.color_bias[3]   = color->colorBias.a;
        }
        if (next->type == XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB && options->ext.FB_composition_layer_settings)
        {
            // Supersampling and sharpening hints; the simulated compositor has no use for them
            const XrCompositionLayerSettingsFlagsFB fb     = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB | XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB | XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB | XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
            const XrCompositionLayerSettingsFlagsFB autom  = options->ext.META_automatic_layer_filter ? XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META : 0;
            const XrCompositionLayerSettingsFlagsFB flags  = ((const XrCompositionLayerSettingsFB*)next)->layerFlags;
            OXR_CHECK((flags & ~(fb | autom)) == 0);
            // XR_META_automatic_layer_filter: the automatic bit picks among the others, so it is never alone
            OXR_CHECK(!(flags & XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META) || (flags & fb) != 0);
        }
        if (next->type == XR_TYPE_COMPOSITION_LAYER_SECURE_CONTENT_FB && options->ext.FB_composition_layer_secure_content)
        {
            const XrCompositionLayerSecureContentFlagsFB flags = ((const XrCompositionLayerSecureContentFB*)next)->flags;
            OXR_CHECK((flags & ~(XrCompositionLayerSecureContentFlagsFB)(XR_COMPOSITION_LAYER_SECURE_CONTENT_EXCLUDE_LAYER_BIT_FB | XR_COMPOSITION_LAYER_SECURE_CONTENT_REPLACE_LAYER_BIT_FB)) == 0);
            out_layer.secure = (flags & XR_COMPOSITION_LAYER_SECURE_CONTENT_EXCLUDE_LAYER_BIT_FB) ? COMP_Secure::Exclude : (flags & XR_COMPOSITION_LAYER_SECURE_CONTENT_REPLACE_LAYER_BIT_FB) ? COMP_Secure::Replace : COMP_Secure::None;
        }
        if (next->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_TEST_FB && options->ext.FB_composition_layer_depth_test)
        {
            const XrCompositionLayerDepthTestFB* test = (const XrCompositionLayerDepthTestFB*)next;
            OXR_CHECK((u32)test->compareOp <= (u32)XR_COMPARE_OP_ALWAYS_FB);
            const R_CompareOp ops[] = { R_CompareOp::Never, R_CompareOp::Less, R_CompareOp::Equal, R_CompareOp::LessOrEqual, R_CompareOp::Greater, R_CompareOp::NotEqual, R_CompareOp::GreaterOrEqual, R_CompareOp::Always };
            out_layer.depth_test    = header->type != XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB;
            out_layer.depth_write   = test->depthMask != XR_FALSE;
            out_layer.depth_compare = ops[test->compareOp];
        }
        if (next->type == XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB && options->ext.FB_composition_layer_alpha_blend)
        {
            const XrCompositionLayerAlphaBlendFB* blend     = (const XrCompositionLayerAlphaBlendFB*)next;
            const XrBlendFactorFB                 factors[] = { blend->srcFactorColor, blend->dstFactorColor, blend->srcFactorAlpha, blend->dstFactorAlpha };
            const R_BlendFactor                   map[]     = { R_BlendFactor::Zero, R_BlendFactor::One, R_BlendFactor::SrcAlpha, R_BlendFactor::OneMinusSrcAlpha, R_BlendFactor::DstAlpha, R_BlendFactor::OneMinusDstAlpha };
            for (u32 i = 0; i < 4; i++)
            {
                OXR_CHECK((u32)factors[i] <= (u32)XR_BLEND_FACTOR_ONE_MINUS_DST_ALPHA_FB);
                out_layer.blend_factors[i] = map[factors[i]];
            }
            out_layer.blend_custom = header->type != XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB;
        }
        if (next->type == XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB && options->ext.FB_composition_layer_image_layout)
        {
            const XrCompositionLayerImageLayoutFB* layout = (const XrCompositionLayerImageLayoutFB*)next;
            OXR_CHECK((layout->flags & ~(XrCompositionLayerImageLayoutFlagsFB)XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB) == 0);
            out_layer.flip_y = (layout->flags & XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB) != 0;
        }
    }
    // GL images have their origin at the bottom left (OpenGL 4.6, 8.5); cube faces have the same orientation in GL and Vulkan
    const R_Backend backend = sess->device ? r_device_backend(sess->device) : R_Backend::Vulkan;
    if ((backend == R_Backend::OpenGL || backend == R_Backend::OpenGLES) && header->type != XR_TYPE_COMPOSITION_LAYER_CUBE_KHR)
    {
        out_layer.flip_y = !out_layer.flip_y;
    }

    switch (header->type)
    {
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
    {
        const XrCompositionLayerProjection* layer = (const XrCompositionLayerProjection*)header;
        const OXR_Space* space = layer_space(layer->space);
        if (!space)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (layer->viewCount != OXR_VIEW_COUNT || !layer->views)
        {
            OXR_FAIL("layer->viewCount != OXR_VIEW_COUNT || !layer->views");
        }
        const OXR_Located   located            = locate_world(space, sample);
        const OXR_Instance* inst               = session_instance(sess);
        const b8            depth_enabled      = inst && inst->ext.KHR_composition_layer_depth;
        const b8            space_warp_enabled = inst && inst->ext.FB_space_warp;
        for (u32 v = 0; v < OXR_VIEW_COUNT; v++)
        {
            const XrCompositionLayerProjectionView& view = layer->views[v];
            if (view.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW)
            {
                OXR_FAIL("view.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW");
            }
            if (!xr_pose_valid(view.pose))
            {
                return XR_ERROR_POSE_INVALID;
            }
            const XrResult result = validate_sub_image(sess, view.subImage, out_layer.views[v]);
            if (result != XR_SUCCESS)
            {
                return result;
            }
            for (const XrBaseInStructure* next = (const XrBaseInStructure*)view.next; next; next = next->next)
            {
                XrResult next_result = XR_SUCCESS;
                if (next->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR && depth_enabled)
                {
                    next_result = validate_depth_info(sess, *(const XrCompositionLayerDepthInfoKHR*)next, out_layer.depth[v]);
                }
                if (next->type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB && space_warp_enabled)
                {
                    next_result      = validate_space_warp_info(sess, *(const XrCompositionLayerSpaceWarpInfoFB*)next);
                    sess->space_warp = sess->space_warp || next_result == XR_SUCCESS;
                }
                if (next_result != XR_SUCCESS)
                {
                    return next_result;
                }
            }
            if (located.valid && fov_valid(view.fov))
            {
                place_projection_view(zm::pose_mul(located.pose, pose_from_xr(view.pose)), view.fov, eyes[v], out_layer.views[v]);
            }
        }
        // Spec XR_FB_composition_layer_depth_test: ignored on layers without depth
        out_layer.depth_test = out_layer.depth_test && (out_layer.depth[0].valid || out_layer.depth[1].valid);
        return XR_SUCCESS;
    }
    case XR_TYPE_COMPOSITION_LAYER_QUAD:
    {
        const XrCompositionLayerQuad* layer = (const XrCompositionLayerQuad*)header;
        const OXR_Space* space = layer_space(layer->space);
        if (!space)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (!xr_pose_valid(layer->pose))
        {
            return XR_ERROR_POSE_INVALID;
        }
        if (!zm::is_finite(layer->size.width) || !zm::is_finite(layer->size.height) || !eye_visibility_valid(layer->eyeVisibility))
        {
            OXR_FAIL("a quad size that is not finite or an invalid eyeVisibility");
        }
        COMP_View      image  = {};
        const XrResult result = validate_sub_image(sess, layer->subImage, image);
        if (result != XR_SUCCESS)
        {
            return result;
        }
        const OXR_Located located = locate_world(space, sample);
        for (u32 e = 0; e < OXR_VIEW_COUNT && located.valid; e++)
        {
            out_layer.views[e] = image;
            if (eye_shown(layer->eyeVisibility, e))
            {
                place_quad(zm::pose_mul(located.pose, pose_from_xr(layer->pose)), layer->size, eyes[e], out_layer.views[e]);
            }
        }
        return XR_SUCCESS;
    }
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
    {
        const XrCompositionLayerCylinderKHR* layer = (const XrCompositionLayerCylinderKHR*)header;
        if (!options || !options->ext.KHR_composition_layer_cylinder)
        {
            return XR_ERROR_LAYER_INVALID;
        }
        OXR_CHECK(zm::is_finite(layer->centralAngle) && zm::is_finite(layer->aspectRatio) && layer->aspectRatio >= 0.0f);
        b8        far   = false;
        const f32 angle = clamp_layer_angle(layer->centralAngle, 0.0f, 2.0f * zm::PI, far);
        if (far)
        {
            OXR_LOG_ONCE(LOG_WARNING, "Cylinder layer centralAngle %g clamped to [0, 2 pi]", (f64)layer->centralAngle);
        }
        // u from the angle around y, v from the height in radii: radius * centralAngle / aspectRatio is the height
        out_layer.type      = COMP_LayerType::Cylinder;
        out_layer.uv_map[0] = 1.0f / angle;
        out_layer.uv_map[1] = 0.5f;
        out_layer.uv_map[2] = -layer->aspectRatio / angle;
        out_layer.uv_map[3] = 0.5f;
        return place_curved_layer(sess, layer->space, layer->eyeVisibility, layer->subImage, layer->pose, layer->radius, sample, eyes, out_layer);
    }
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
    {
        // The spec leaves scale and bias open. As the CTS uses them, scale is the fraction of the
        // sphere the image covers, centered on -Z, and bias offsets the texture coordinates.
        const XrCompositionLayerEquirectKHR* layer = (const XrCompositionLayerEquirectKHR*)header;
        if (!options || !options->ext.KHR_composition_layer_equirect)
        {
            return XR_ERROR_LAYER_INVALID;
        }
        OXR_CHECK(zm::is_finite(layer->scale.x) && zm::is_finite(layer->scale.y) && zm::is_finite(layer->bias.x) && zm::is_finite(layer->bias.y));
        out_layer.type      = COMP_LayerType::Sphere;
        out_layer.uv_map[0] = 1.0f / (2.0f * zm::PI * layer->scale.x);
        out_layer.uv_map[1] = 0.5f + layer->bias.x;
        out_layer.uv_map[2] = -1.0f / (zm::PI * layer->scale.y);
        out_layer.uv_map[3] = 0.5f + layer->bias.y;
        return place_curved_layer(sess, layer->space, layer->eyeVisibility, layer->subImage, layer->pose, layer->radius, sample, eyes, out_layer);
    }
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
    {
        const XrCompositionLayerEquirect2KHR* layer = (const XrCompositionLayerEquirect2KHR*)header;
        if (!options || !options->ext.KHR_composition_layer_equirect2)
        {
            return XR_ERROR_LAYER_INVALID;
        }
        OXR_CHECK(zm::is_finite(layer->centralHorizontalAngle) && zm::is_finite(layer->upperVerticalAngle) && zm::is_finite(layer->lowerVerticalAngle));
        b8        far   = false;
        const f32 angle = clamp_layer_angle(layer->centralHorizontalAngle, 0.0f, 2.0f * zm::PI, far);
        const f32 upper = clamp_layer_angle(layer->upperVerticalAngle, -0.5f * zm::PI, 0.5f * zm::PI, far);
        const f32 lower = clamp_layer_angle(layer->lowerVerticalAngle, -0.5f * zm::PI, 0.5f * zm::PI, far);
        if (far)
        {
            OXR_LOG_ONCE(LOG_WARNING, "Equirect2 layer angles %g, %g, %g clamped to [0, 2 pi] and [-pi / 2, pi / 2]", (f64)layer->centralHorizontalAngle, (f64)layer->upperVerticalAngle, (f64)layer->lowerVerticalAngle);
        }
        out_layer.type      = COMP_LayerType::Sphere;
        out_layer.uv_map[0] = 1.0f / angle;
        out_layer.uv_map[1] = 0.5f;
        out_layer.uv_map[2] = upper > lower ? -1.0f / (upper - lower) : INFINITY;
        out_layer.uv_map[3] = upper > lower ? upper / (upper - lower) : INFINITY;
        return place_curved_layer(sess, layer->space, layer->eyeVisibility, layer->subImage, layer->pose, layer->radius, sample, eyes, out_layer);
    }
    case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
    {
        const XrCompositionLayerCubeKHR* layer = (const XrCompositionLayerCubeKHR*)header;
        if (!options || !options->ext.KHR_composition_layer_cube)
        {
            return XR_ERROR_LAYER_INVALID;
        }
        const OXR_Space* space = layer_space(layer->space);
        if (!space)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        OXR_GET_CONST_SWAPCHAIN(swapchain, layer->swapchain);
        if (swapchain->session != session_handle_of(sess) || space->session != session_handle_of(sess))
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        const XrPosef pose = { layer->orientation, { 0.0f, 0.0f, 0.0f } };
        if (!xr_pose_valid(pose))
        {
            return XR_ERROR_POSE_INVALID;
        }
        OXR_CHECK(eye_visibility_valid(layer->eyeVisibility) && swapchain->face_count == 6 && layer->imageArrayIndex < swapchain->array_size);
        if (swapchain->last_released < 0 || swapchain->depth)
        {
            return XR_ERROR_LAYER_INVALID;
        }
        // An environment map at infinity: only the orientation places it
        out_layer.type                  = COMP_LayerType::Cube;
        const COMP_View   image         = { .image = swapchain->images[swapchain->last_released], .array_layer = layer->imageArrayIndex * 6, .rect = { 0, 0, swapchain->width, swapchain->height }, .nearest = swapchain->sampler.mag_filter == 0 };
        const OXR_Located located       = locate_world(space, sample);
        for (u32 e = 0; e < OXR_VIEW_COUNT; e++)
        {
            out_layer.views[e] = image;
            if (located.valid && eye_shown(layer->eyeVisibility, e))
            {
                const Pose cube_world = { .position = eyes[e].pose.position, .orientation = located.pose.orientation * pose_from_xr(pose).orientation };
                place_curved(cube_world, eyes[e], out_layer.views[e]);
            }
        }
        return XR_SUCCESS;
    }
    case XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB:
    {
        // Errata of XR_FB_passthrough: the flags field is named flags, at the place of layerFlags
        const XrCompositionLayerPassthroughFB* layer = (const XrCompositionLayerPassthroughFB*)header;
        const XrCompositionLayerFlags          valid = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT | XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT | XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT;
        const OXR_Instance*                    inst  = runtime()->instances.try_get(sess->instance);
        if (!inst || !inst->ext.FB_passthrough)
        {
            return XR_ERROR_LAYER_INVALID;
        }
        OXR_CHECK(layer->flags != 0 && (layer->flags & ~valid) == 0);
        if (layer->space != XR_NULL_HANDLE)
        {
            const OXR_Space* space = layer_space(layer->space);
            if (!space)
            {
                return XR_ERROR_HANDLE_INVALID;
            }
            OXR_CHECK(space->session == session_handle_of(sess));
        }
        const OXR_PassthroughLayer* passthrough_layer = oxr_find_passthrough_layer(layer->layerHandle);
        if (!passthrough_layer)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        OXR_CHECK(passthrough_layer->session == session_handle_of(sess));

        const OXR_Passthrough* feature   = runtime()->passthroughs.try_get(passthrough_layer->passthrough);
        const b8               projected = passthrough_layer->purpose == XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB;
        const b8               shown     = feature && feature->running && passthrough_layer->running && (passthrough_layer->purpose == XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB || projected);
        if (shown)
        {
            place_passthrough(eyes, out_layer);
            out_layer.style = &passthrough_layer->style;
            if (projected)
            {
                place_geometry(sess, passthrough_layer_handle(layer->layerHandle), sample, out_layer);
            }
        }
        return XR_SUCCESS;
    }
    default:
        return XR_ERROR_LAYER_INVALID;
    }
}

// A running reconstruction layer of a running passthrough: what Quest requires to hide the boundary
static b8 session_shows_passthrough (OXR_SessionHandle session)
{
    for (const OXR_PassthroughLayer& layer : runtime()->passthrough_layers)
    {
        const OXR_Passthrough* feature = runtime()->passthroughs.try_get(layer.passthrough);
        if (layer.session == session && layer.running && layer.purpose == XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB && feature && feature->running)
        {
            return true;
        }
    }
    return false;
}

// Once per frame: resumed layers that are ready now, and a boundary that can no longer be hidden
static void passthrough_frame_events (OXR_Session* sess, XrSession session)
{
    OXR_Runtime*  rt   = runtime();
    OXR_Instance* inst = rt->instances.try_get(sess->instance);
    if (!inst)
    {
        return;
    }
    for (usize i = 0; i < rt->passthrough_layers.length(); i++)
    {
        const OXR_PassthroughLayerHandle handle  = rt->passthrough_layers.handle_at(i);
        OXR_PassthroughLayer&            layer   = rt->passthrough_layers.get(handle);
        const OXR_Passthrough*           feature = rt->passthroughs.try_get(layer.passthrough);
        if (layer.session != session_handle(session) || !layer.resume_pending || !layer.running || !feature || !feature->running)
        {
            continue;
        }
        layer.resume_pending = false;
        if (inst->ext.META_passthrough_layer_resumed_event)
        {
            push_event(inst, { .type = XR_TYPE_EVENT_DATA_PASSTHROUGH_LAYER_RESUMED_META, .session = session, .layer = passthrough_layer_xr(handle) });
        }
    }
    if (sess->boundary == XR_BOUNDARY_VISIBILITY_SUPPRESSED_META && !session_shows_passthrough(session_handle(session)))
    {
        sess->boundary = XR_BOUNDARY_VISIBILITY_NOT_SUPPRESSED_META;
        push_event(inst, { .type = XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META, .session = session, .boundary = sess->boundary });
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEndFrame (XrSession session, const XrFrameEndInfo* info)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    defer { forget_retired_spaces(session_handle(session)); };
    OXR_CHECK_TYPE(info, XR_TYPE_FRAME_END_INFO);
    if (!sess->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (!sess->frame_begun)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    if (info->displayTime <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    const b8 alpha_blend = info->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND && !(session_instance(sess) && session_instance(sess)->ext.FB_composition_layer_alpha_blend);
    if (info->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE && !alpha_blend)
    {
        return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
    }
    if (info->layerCount > OXR_MAX_LAYERS)
    {
        return XR_ERROR_LAYER_LIMIT_EXCEEDED;
    }
    OXR_CHECK(info->layerCount == 0 || info->layers);
    if (sess->headless && info->layerCount > 0)
    {
        return XR_ERROR_LAYER_INVALID;
    }
    const XrResult fault = take_fault(runtime()->frame_faults[(u32)OXR_FrameOp::End]);
    if (fault != XR_SUCCESS)
    {
        return fault;
    }
    const OXR_Instance*    frame_inst = session_instance(sess);
    XrLocalDimmingModeMETA dimming    = sess->local_dimming;
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)info->next; next && frame_inst && frame_inst->ext.META_local_dimming; next = next->next)
    {
        if (next->type == XR_TYPE_LOCAL_DIMMING_FRAME_END_INFO_META)
        {
            dimming = ((const XrLocalDimmingFrameEndInfoMETA*)next)->localDimmingMode;
            OXR_CHECK(dimming == XR_LOCAL_DIMMING_MODE_OFF_META || dimming == XR_LOCAL_DIMMING_MODE_ON_META);
        }
    }
    sess->local_dimming = dimming;

    SIM_Sample sample = {};
    sim_sample(runtime()->sim, info->displayTime, sample);
    OXR_Eye eyes[OXR_VIEW_COUNT];
    for (u32 e = 0; e < OXR_VIEW_COUNT; e++)
    {
        sim_sample_view(runtime()->sim, sample, e, eyes[e].pose, eyes[e].fov);
    }

    COMP_Layer layers[OXR_MAX_LAYERS + 1];
    u32        layers_count = 0;
    sess->geometry_count    = 0;
    sess->space_warp        = false;
    if (info->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
    {
        layers[layers_count]       = {};
        layers[layers_count].style = &oxr_default_passthrough_style;
        place_passthrough(eyes, layers[layers_count++]);
    }
    for (u32 i = 0; i < info->layerCount; i++)
    {
        const XrResult result = validate_layer(sess, info->layers[i], sample, eyes, layers[layers_count]);
        if (result != XR_SUCCESS)
        {
            const char* name = result_name(result);
            OXR_LOG_ONCE(LOG_ERROR, "xrEndFrame rejected layer %u of type %d: %s (%d)", i, info->layers[i] ? (i32)info->layers[i]->type : 0, name ? name : "", (i32)result);
            return result;
        }
        b8 visible = false;
        for (u32 e = 0; e < OXR_VIEW_COUNT; e++)
        {
            visible |= layers[layers_count].views[e].visible || layers[layers_count].eye_visible[e];
        }
        layers_count += visible ? 1 : 0;
    }

    sess->frame_begun       = false;
    sess->frame_interval    = sess->last_display_time ? info->displayTime - sess->last_display_time : 0;
    sess->last_display_time = info->displayTime;
    // Timed at its end, the composition and the mirror included, headless frames too
    defer { time_frame(runtime()); };
    passthrough_frame_events(sess, session);
    if (sess->synchronize_pending)
    {
        sess->synchronize_pending      = false;
        const OXR_SessionHandle handle = session_handle(session);
        session_set_state(sess, handle, XR_SESSION_STATE_SYNCHRONIZED);
        session_set_state(sess, handle, XR_SESSION_STATE_VISIBLE);
        session_set_state(sess, handle, XR_SESSION_STATE_FOCUSED);
    }
    if (!sess->compositor)
    {
        return XR_SUCCESS;
    }

    OXR_Runtime* rt = runtime();
    startup_mark(oxr_startup.first_end);
    if (!sess->split_logged && sess->wait_thread && sess->wait_thread != os_thread_current_id())
    {
        sess->split_logged = true;
        LOG_INFO("Frames are waited on thread %llu and ended on thread %llu; the window's part runs on the window thread", (unsigned long long)sess->wait_thread, (unsigned long long)os_thread_current_id());
    }
    if (sess->room_uploaded != rt->world_generation)
    {
        const u64 upload_start = os_monotonic_ns();
        comp_set_room(sess->compositor, rt->world);
        sess->room_uploaded       = rt->world_generation;
        oxr_startup.room_upload  += oxr_startup.logged ? 0 : os_monotonic_ns() - upload_start;
    }
    const SIM_Prop* props = nullptr;
    const u32       props_count = sim_props(rt->sim, props);
    comp_set_props(sess->compositor, props, props_count, info->displayTime, rt->room_from_stage);
    {
        PROF_ZONE("compose");
        comp_compose(sess->compositor, layers, layers_count, (COMP_ViewMode)rt->mirror_view);
    }
    render_cameras(sess, session_handle(session), info->displayTime, sample);
    render_depth(sess, session_handle(session), info->displayTime, sample);
    // Read back here for the control server's readers (docs/openxr.md, Queue access)
    if (sim_get_config(rt->sim).control)
    {
        PROF_ZONE("compositor snapshot");
        if (comp_snapshot(sess->compositor))
        {
            rt->frame_read_thread = os_thread_current_id();
        }
    }
    // Shown by the tick after its first frame: before, Windows shows whatever the unpainted window
    // holds for the seconds an engine takes to start. Showing maximizes it, so the frame after is
    // drawn at the shown size, not stretched from the hidden one
    present_frame(rt, sess);
    schedule_window_tick(rt);
    if (info->layerCount > 0)
    {
        startup_log();
    }

    const SIM_Config& config = sim_get_config(rt->sim);
    const u64         frame  = sim_frame_index(rt->sim);
    if (config.screenshot_at_frame != 0 && frame == config.screenshot_at_frame)
    {
        char path[OS_MAX_PATH];
        output_path(path, sizeof(path), "puppetxr_frame", frame, "png");
        comp_save_png(sess->compositor, path);
    }
    if (config.exit_at_frame != 0 && frame >= config.exit_at_frame && !sess->exit_requested)
    {
        LOG_INFO("Requesting session exit at frame %llu", (unsigned long long)frame);
        request_exit(sess, session_handle(session));
    }
    return XR_SUCCESS;
}

// Time 0 is invalid (the CTS checks it); with accept_time_zero it is the latest time, for
// OVRPlugin, which locates and creates anchors at 0
static XrTime locate_time (XrTime time)
{
    if (time != 0 || !sim_get_config(runtime()->sim).accept_time_zero)
    {
        return time;
    }
    // The frame being drawn, where the app's other locates of this frame are; a frame behind,
    // a moving controller located at both shows twice
    XrTime latest = 0;
    for (const OXR_Session& session : runtime()->sessions)
    {
        latest = zm::max(latest, session.waited_display_time);
    }
    return latest > 0 ? latest : sim_time(runtime()->sim);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrLocateViews (XrSession session, const XrViewLocateInfo* info, XrViewState* view_state, uint32_t capacity, uint32_t* count_output, XrView* views)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_VIEW_LOCATE_INFO);
    OXR_CHECK_TYPE(view_state, XR_TYPE_VIEW_STATE);
    OXR_GET_CONST_SPACE(space, info->space);
    if (info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    const XrTime display_time = locate_time(info->displayTime);
    if (display_time <= 0)
    {
        OXR_LOG_ONCE(LOG_ERROR, "xrLocateViews rejected display time %lld", (long long)info->displayTime);
        return XR_ERROR_TIME_INVALID;
    }

    const XrResult result = oxr_two_call(capacity, count_output, views, OXR_VIEW_COUNT);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    const XrResult fault = take_fault(runtime()->frame_faults[(u32)OXR_FrameOp::LocateViews]);
    if (fault != XR_SUCCESS)
    {
        *count_output              = 0;
        view_state->viewStateFlags = 0;
        return fault;
    }
    if (capacity == 0)
    {
        return XR_SUCCESS;
    }

    SIM_Sample sample = {};
    sim_sample(runtime()->sim, display_time, sample);
    const OXR_Located base = locate_world(space, sample);
    view_state->viewStateFlags = 0;
    for (u32 i = 0; i < OXR_VIEW_COUNT; i++)
    {
        OXR_CHECK_TYPE(&views[i], XR_TYPE_VIEW);
        Pose world;
        Fov  fov;
        sim_sample_view(runtime()->sim, sample, i, world, fov);
        const OXR_Located    target = { .pose = world, .valid = true, .tracked = sample.devices[(u32)SIM_Device::Head].tracked };
        XrSpaceLocationFlags flags  = 0;
        locate_relative(target, base, views[i].pose, flags, nullptr);
        views[i].fov = { fov.angle_left, fov.angle_right, fov.angle_up, fov.angle_down };
        if (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        {
            view_state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
            if (flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)
            {
                view_state->viewStateFlags |= XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
            }
        }
    }
    return XR_SUCCESS;
}

// Spaces

static b8 reference_space_supported (const OXR_Instance* inst, XrReferenceSpaceType type)
{
    switch (type)
    {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return true;
    case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
        return inst->ext.EXT_local_floor || XR_VERSION_MINOR(inst->api_version) >= 1;
    case XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT:
        return inst->ext.EXT_stationary_reference_space || inst->ext.EXTX2_stationary_reference_space;
    default:
        return false;
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateReferenceSpaces (XrSession session, uint32_t capacity, uint32_t* count_output, XrReferenceSpaceType* spaces)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);

    const XrReferenceSpaceType all[] = { XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR, XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT };
    XrReferenceSpaceType       supported[ARRAY_SIZE(all)] = {};
    u32                        count = 0;
    for (const XrReferenceSpaceType type : all)
    {
        if (reference_space_supported(inst, type))
        {
            supported[count++] = type;
        }
    }
    const XrResult result = oxr_two_call(capacity, count_output, spaces, count);
    if (result == XR_SUCCESS && capacity > 0)
    {
        memory_copy(spaces, supported, count * sizeof(XrReferenceSpaceType));
    }
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateReferenceSpace (XrSession session, const XrReferenceSpaceCreateInfo* info, XrSpace* out_space)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    OXR_CHECK(out_space);
    *out_space = XR_NULL_HANDLE;
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    if (!reference_space_supported(inst, info->referenceSpaceType))
    {
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }
    if (!xr_pose_valid(info->poseInReferenceSpace))
    {
        return XR_ERROR_POSE_INVALID;
    }

    OXR_Runtime*          rt     = runtime();
    const OXR_SpaceHandle handle = rt->spaces.add();
    OXR_Space*            space  = &rt->spaces.get(handle);
    space->session                = session_handle(session);
    space->reference_type         = info->referenceSpaceType;
    space->offset                 = pose_from_xr(info->poseInReferenceSpace);
    space->subaction              = OXR_TopLevel::Count;
    *out_space = space_xr(handle);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetReferenceSpaceBoundsRect (XrSession session, XrReferenceSpaceType type, XrExtent2Df* bounds)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    OXR_CHECK(bounds);
    if (!reference_space_supported(inst, type))
    {
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }
    *bounds = {};
    if (type != XR_REFERENCE_SPACE_TYPE_STAGE)
    {
        return XR_SPACE_BOUNDS_UNAVAILABLE;
    }
    const Vec2 stage = sim_get_stage_bounds(runtime()->sim);
    *bounds = { stage.x, stage.y };
    return XR_SUCCESS;
}

// XR_EXT_stationary_reference_space, also as xrGetStationaryReferenceSpaceIdEXTX2. The simulator
// never loses tracking, so there is one generation, the same in every run.
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetStationaryReferenceSpaceGenerationIdEXT (XrSession session, const XrStationaryReferenceSpaceGenerationIdGetInfoEXT* info, XrStationaryReferenceSpaceGenerationIdResultEXT* result)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK(!info || info->type == XR_TYPE_STATIONARY_REFERENCE_SPACE_GENERATION_ID_GET_INFO_EXT);
    OXR_CHECK_TYPE(result, XR_TYPE_STATIONARY_REFERENCE_SPACE_GENERATION_ID_RESULT_EXT);
    if (!sess->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    const XrUuid generation = { .data = { 'x', 'r', 's', 'i', 'm', 's', 't', 'a', 't', 'i', 'o', 'n', 'a', 'r', 'y', 1 } };
    result->generationId    = generation;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateActionSpace (XrSession session, const XrActionSpaceCreateInfo* info, XrSpace* out_space)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_ACTION_SPACE_CREATE_INFO);
    OXR_CHECK(out_space);
    *out_space = XR_NULL_HANDLE;
    OXR_GET_CONST_ACTION(action, info->action);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    if (action->type != XR_ACTION_TYPE_POSE_INPUT)
    {
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }

    OXR_TopLevel subaction = OXR_TopLevel::Count;
    if (info->subactionPath != XR_NULL_PATH)
    {
        if (!path_string(inst, info->subactionPath))
        {
            return XR_ERROR_PATH_INVALID;
        }
        if (!action->has_subaction_paths || !path_top_level(inst, info->subactionPath, subaction) || !(action->subaction_mask & (1 << (u32)subaction)))
        {
            return XR_ERROR_PATH_UNSUPPORTED;
        }
    }
    if (!xr_pose_valid(info->poseInActionSpace))
    {
        return XR_ERROR_POSE_INVALID;
    }

    OXR_Runtime*          rt     = runtime();
    const OXR_SpaceHandle handle = rt->spaces.add();
    OXR_Space*            space  = &rt->spaces.get(handle);
    space->session                = session_handle(session);
    space->kind                   = OXR_SpaceKind::Action;
    space->action                 = action_handle(info->action);
    space->subaction              = subaction;
    space->offset                 = pose_from_xr(info->poseInActionSpace);
    *out_space = space_xr(handle);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrLocateSpace (XrSpace space, XrSpace base_space, XrTime time, XrSpaceLocation* location)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SPACE(target, space);
    OXR_GET_CONST_SPACE(base, base_space);
    OXR_CHECK_TYPE(location, XR_TYPE_SPACE_LOCATION);
    time = locate_time(time);
    if (time <= 0)
    {
        OXR_LOG_ONCE(LOG_ERROR, "xrLocateSpace rejected time %lld", (long long)time);
        return XR_ERROR_TIME_INVALID;
    }

    XrSpaceVelocity*        velocity    = nullptr;
    XrEyeGazeSampleTimeEXT* sample_time = nullptr;
    for (XrBaseOutStructure* next = (XrBaseOutStructure*)location->next; next; next = next->next)
    {
        if (next->type == XR_TYPE_SPACE_VELOCITY)
        {
            velocity = (XrSpaceVelocity*)next;
        }
        else if (next->type == XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT)
        {
            sample_time = (XrEyeGazeSampleTimeEXT*)next;
        }
    }

    SIM_Sample sample = {};
    sim_sample(runtime()->sim, time, sample);
    const OXR_Located located = locate_world(target, sample);
    locate_relative(located, locate_world(base, sample), location->pose, location->locationFlags, velocity);
    // Spec XR_EXT_eye_gaze_interaction: the gaze is sampled at the time it is located for
    if (sample_time && space_is_eye_gaze(target))
    {
        sample_time->time = located.valid ? time : 0;
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrLocateSpaces (XrSession session, const XrSpacesLocateInfo* info, XrSpaceLocations* locations) // NOLINT(misc-const-correctness): the spec signature
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACES_LOCATE_INFO);
    OXR_CHECK_TYPE(locations, XR_TYPE_SPACE_LOCATIONS);
    OXR_GET_CONST_SPACE(base, info->baseSpace);
    OXR_CHECK(info->spaceCount > 0 && info->spaces);
    OXR_CHECK(locations->locationCount == info->spaceCount && locations->locations);
    const XrTime time = locate_time(info->time);
    if (time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }

    const XrSpaceVelocities* velocities = nullptr;
    for (XrBaseOutStructure* next = (XrBaseOutStructure*)locations->next; next; next = next->next)
    {
        if (next->type == XR_TYPE_SPACE_VELOCITIES)
        {
            velocities = (XrSpaceVelocities*)next;
            OXR_CHECK(velocities->velocityCount == info->spaceCount && velocities->velocities);
        }
    }

    SIM_Sample sample = {};
    sim_sample(runtime()->sim, time, sample);
    const OXR_Located base_located = locate_world(base, sample);
    for (u32 i = 0; i < info->spaceCount; i++)
    {
        OXR_GET_CONST_SPACE(target, info->spaces[i]);
        XrSpaceVelocity velocity = {};
        locate_relative(locate_world(target, sample), base_located, locations->locations[i].pose, locations->locations[i].locationFlags, &velocity);
        if (velocities)
        {
            velocities->velocities[i] = { .velocityFlags = velocity.velocityFlags, .linearVelocity = velocity.linearVelocity, .angularVelocity = velocity.angularVelocity };
        }
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroySpace (XrSpace space)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SPACE(target, space);
    OXR_Runtime*          rt     = runtime();
    const OXR_SpaceHandle handle = decode_handle<OXR_Space>(OXR_HandleType::Space, (u64)(uintptr_t)space);
    if (rt->retired_spaces_count == OXR_MAX_RETIRED_SPACES)
    {
        memory_move(rt->retired_spaces, rt->retired_spaces + 1, sizeof(rt->retired_spaces) - sizeof(rt->retired_spaces[0]));
        rt->retired_spaces_count--;
    }
    rt->retired_spaces[rt->retired_spaces_count++] = { .handle = handle, .space = *target };
    destroy_space(handle);
    return XR_SUCCESS;
}

// Swapchains

// The image structs of the session's graphics API
static XrResult write_swapchain_images (const OXR_Session* sess, const u64* natives, u32 count, XrSwapchainImageBaseHeader* images)
{
    const R_Backend backend = r_device_backend(sess->device);
    if (backend == R_Backend::Vulkan)
    {
        return oxr_vk_write_swapchain_images(natives, count, images);
    }
    if (backend == R_Backend::D3D11 || backend == R_Backend::D3D12)
    {
        return oxr_d3d_write_swapchain_images(natives, count, images, backend == R_Backend::D3D12);
    }
    if (backend == R_Backend::Metal)
    {
        return oxr_metal_write_swapchain_images(natives, count, images);
    }
    return oxr_gl_write_swapchain_images(natives, count, images, backend == R_Backend::OpenGLES);
}

static const R_Format oxr_swapchain_formats[] = {
    R_Format::R8G8B8A8_SRGB,
    R_Format::B8G8R8A8_SRGB,
    R_Format::R8G8B8A8_UNORM,
    R_Format::B8G8R8A8_UNORM,
    R_Format::R16G16B16A16_SFLOAT,
    R_Format::D32_SFLOAT,
    R_Format::D16_UNORM,
    R_Format::D24_UNORM_S8_UINT,
    R_Format::D32_SFLOAT_S8_UINT,
};

// Every swapchain image is sampled by the compositor, a transfer source and target, and an
// attachment of its kind
static u32 oxr_swapchain_image_usage (b8 depth)
{
    const u32 usage = (u32)R_ImageUsageFlags::TransferSrc | (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::Sampled;
    return usage | (depth ? (u32)R_ImageUsageFlags::DepthStencilAttachment : (u32)R_ImageUsageFlags::ColorAttachment);
}

// The API knowing a format is not enough: Vulkan devices of AMD have no D24_UNORM_S8_UINT
static b8 oxr_swapchain_format_supported (const R_Device* device, R_Format format)
{
    const R_CreateImageInfo info = { .width = 1, .height = 1, .format = format, .usage_flags = oxr_swapchain_image_usage(r_format_is_depth(format)) };
    return r_native_format(device, format) != 0 && r_image_flags_supported(device, info);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateSwapchainFormats (XrSession session, uint32_t capacity, uint32_t* count_output, int64_t* formats)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    if (sess->headless)
    {
        return oxr_two_call(capacity, count_output, formats, 0);
    }
    // Only the formats of the session's graphics API, in order of preference
    int64_t supported[ARRAY_SIZE(oxr_swapchain_formats)];
    u32     supported_count = 0;
    for (const R_Format format : oxr_swapchain_formats)
    {
        if (oxr_swapchain_format_supported(sess->device, format))
        {
            supported[supported_count++] = r_native_format(sess->device, format);
        }
    }
    const XrResult result = oxr_two_call(capacity, count_output, formats, supported_count);
    if (result == XR_SUCCESS && capacity > 0)
    {
        memory_copy(formats, supported, supported_count * sizeof(int64_t));
    }
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateSwapchain (XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* out_swapchain)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SWAPCHAIN_CREATE_INFO);
    OXR_CHECK(out_swapchain);
    *out_swapchain = XR_NULL_HANDLE;
    if (sess->headless)
    {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }

    const R_Format format = r_format_from_native(sess->device, info->format);
    b8 listed = false;
    for (u32 i = 0; i < ARRAY_SIZE(oxr_swapchain_formats); i++)
    {
        listed |= oxr_swapchain_formats[i] == format;
    }
    if (!listed || !oxr_swapchain_format_supported(sess->device, format))
    {
        OXR_LOG_ONCE(LOG_ERROR, "xrCreateSwapchain rejected format %lld (usage 0x%llx)", (long long)info->format, (unsigned long long)info->usageFlags);
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }

    const XrSwapchainUsageFlags known_usage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT | XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR;
    OXR_CHECK((info->usageFlags & ~known_usage) == 0);
    OXR_CHECK(info->width > 0 && info->height > 0 && info->width <= OXR_MAX_IMAGE_SIZE && info->height <= OXR_MAX_IMAGE_SIZE);
    OXR_CHECK(info->arraySize > 0 && info->mipCount > 0 && info->sampleCount > 0 && (info->faceCount == 1 || info->faceCount == 6));
    OXR_CHECK(info->mipCount <= r_max_mip_levels(info->width, info->height));
    // Cube faces are array layers imageArrayIndex * 6 + face, in Vulkan order; Vulkan needs square faces
    const b8  cube   = info->faceCount == 6;
    const u64 layers = (u64)info->arraySize * info->faceCount;
    if (layers > R_MAX_IMAGE_LAYERS || (cube && info->width != info->height) || info->sampleCount != 1 || (info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT))
    {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    // The GL compositor samples cube images as samplerCube, which has no arrays before GL 4.0 and ES 3.2
    const R_Backend backend = r_device_backend(sess->device);
    if (cube && info->arraySize > 1 && (backend == R_Backend::OpenGL || backend == R_Backend::OpenGLES))
    {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }

    const b8 depth = r_format_is_depth(format);
    u32 usage = oxr_swapchain_image_usage(depth);
    usage |= (info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) ? (u32)R_ImageUsageFlags::UnorderedAccess : 0u;

    R_CreateImageInfo image_info = { .width = info->width, .height = info->height, .layers = (u32)layers, .mip_levels = info->mipCount, .format = format, .usage_flags = usage, .initial_layout = depth ? R_ImageLayout::DepthStencilAttachment : R_ImageLayout::ColorAttachment, .cube_compatible = cube };
    const OXR_Instance* inst = session_instance(sess);
    if (inst && inst->ext.META_vulkan_swapchain_create_info)
    {
        const XrResult extra = oxr_vk_swapchain_extra_flags((const XrBaseInStructure*)info->next, image_info.vk_extra_create_flags, image_info.vk_extra_usage_flags);
        if (extra != XR_SUCCESS)
        {
            return extra;
        }
        if ((image_info.vk_extra_create_flags | image_info.vk_extra_usage_flags) != 0 && !r_image_flags_supported(sess->device, image_info))
        {
            return XR_ERROR_FEATURE_UNSUPPORTED;
        }
    }

    // XR_FB_foveation: the compositor shows the images unchanged, so foveation only needs to be
    // accepted. A fragment density map is handed out only by XR_FB_foveation_vulkan, which is
    // not offered, so asking for one (OVRPlugin on Vulkan does) changes nothing visible either.
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)info->next; next && inst && inst->ext.FB_foveation; next = next->next)
    {
        if (next->type == XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB)
        {
            const XrSwapchainCreateFoveationFlagsFB flags = ((const XrSwapchainCreateInfoFoveationFB*)next)->flags;
            OXR_CHECK((flags & ~(XrSwapchainCreateFoveationFlagsFB)(XR_SWAPCHAIN_CREATE_FOVEATION_SCALED_BIN_BIT_FB | XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB)) == 0);
        }
    }

    OXR_Swapchain swapchain = {
        .session       = session_handle(session),
        .images_count  = (info->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1u : (u32)OXR_MAX_SWAPCHAIN_IMAGES,
        .width         = info->width,
        .height        = info->height,
        .array_size    = info->arraySize,
        .face_count    = info->faceCount,
        .format        = format,
        .vk_format     = info->format,
        .depth         = depth,
        .last_released = -1,
        .is_static     = (info->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0,
    };
    swapchain.sampler = oxr_vk_default_sampler_state();
    for (u32 i = 0; i < swapchain.images_count; i++)
    {
        swapchain.images[i] = r_create_image(sess->device, image_info);
        if (!swapchain.images[i])
        {
            LOG_ERROR("Cannot create swapchain image %u of %ux%u", i, info->width, info->height);
            for (u32 k = 0; k < i; k++)
            {
                r_destroy_image(sess->device, swapchain.images[k]);
            }
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }

    *out_swapchain = (XrSwapchain)(uintptr_t)encode_handle(OXR_HandleType::Swapchain, runtime()->swapchains.add(swapchain).value);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroySwapchain (XrSwapchain swapchain)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SWAPCHAIN(chain, swapchain);
    UNUSED(chain);
    destroy_swapchain(decode_handle<OXR_Swapchain>(OXR_HandleType::Swapchain, (u64)(uintptr_t)swapchain));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateSwapchainImages (XrSwapchain swapchain, uint32_t capacity, uint32_t* count_output, XrSwapchainImageBaseHeader* images)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SWAPCHAIN(chain, swapchain);
    const XrResult result = oxr_two_call(capacity, count_output, images, chain->images_count);
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    const OXR_Session* sess = runtime()->sessions.try_get(chain->session);
    OXR_CHECK(sess && sess->device);
    u64 natives[OXR_MAX_SWAPCHAIN_IMAGES];
    for (u32 i = 0; i < chain->images_count; i++)
    {
        natives[i] = r_get_image_native(sess->device, chain->images[i]);
    }
    return write_swapchain_images(sess, natives, chain->images_count, images);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrAcquireSwapchainImage (XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* info, uint32_t* index)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SWAPCHAIN(chain, swapchain);
    OXR_CHECK(!info || info->type == XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    OXR_CHECK(index);
    if (chain->acquired_count == chain->images_count || (chain->is_static && chain->ever_acquired))
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    chain->ever_acquired = true;
    // The images were created without touching the app's queue; the spec allows it here
    const OXR_Session* owner = runtime()->sessions.try_get(chain->session);
    if (owner && owner->device)
    {
        r_flush_initial_layouts(owner->device);
    }
    *index                                   = chain->next_index;
    chain->acquired[chain->acquired_count++] = chain->next_index;
    chain->next_index                        = (chain->next_index + 1) % chain->images_count;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrWaitSwapchainImage (XrSwapchain swapchain, const XrSwapchainImageWaitInfo* info)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SWAPCHAIN(chain, swapchain);
    OXR_CHECK_TYPE(info, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    // Spec 10.1: a waited image must be released before the next one is waited on
    if (chain->acquired_count == 0 || chain->waited_count > 0)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    chain->waited_count = 1;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrReleaseSwapchainImage (XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SWAPCHAIN(chain, swapchain);
    OXR_CHECK(!info || info->type == XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
    if (chain->waited_count == 0)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    chain->last_released = (i32)chain->acquired[0];
    memory_move(chain->acquired, chain->acquired + 1, (chain->acquired_count - 1) * sizeof(u32));
    chain->acquired_count--;
    chain->waited_count = 0;
    return XR_SUCCESS;
}

// Actions

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateActionSet (XrInstance instance, const XrActionSetCreateInfo* info, XrActionSet* out_set)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    OXR_CHECK_TYPE(info, XR_TYPE_ACTION_SET_CREATE_INFO);
    OXR_CHECK(out_set);
    *out_set = XR_NULL_HANDLE;

    if (info->actionSetName[0] == 0)
    {
        return XR_ERROR_NAME_INVALID;
    }
    if (!name_valid(info->actionSetName, sizeof(info->actionSetName)))
    {
        return XR_ERROR_PATH_FORMAT_INVALID;
    }
    if (info->localizedActionSetName[0] == 0)
    {
        return XR_ERROR_LOCALIZED_NAME_INVALID;
    }

    OXR_Runtime*             rt     = runtime();
    const OXR_InstanceHandle handle = instance_handle(instance);
    for (const OXR_ActionSet& set : rt->action_sets)
    {
        if (set.instance != handle)
        {
            continue;
        }
        if (cstr_equal(set.name, info->actionSetName))
        {
            return XR_ERROR_NAME_DUPLICATED;
        }
        if (cstr_equal(set.localized_name, info->localizedActionSetName))
        {
            return XR_ERROR_LOCALIZED_NAME_DUPLICATED;
        }
    }

    const OXR_ActionSetHandle set_handle = rt->action_sets.add();
    OXR_ActionSet*            set        = &rt->action_sets.get(set_handle);
    set->instance                        = handle;
    set->priority                        = info->priority;
    cstr_copy(set->name, sizeof(set->name), info->actionSetName);
    cstr_copy(set->localized_name, sizeof(set->localized_name), info->localizedActionSetName);
    *out_set = action_set_xr(set_handle);
    UNUSED(inst);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyActionSet (XrActionSet action_set)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_ACTION_SET(set, action_set);
    UNUSED(set);
    destroy_action_set(action_set_handle(action_set));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateAction (XrActionSet action_set, const XrActionCreateInfo* info, XrAction* out_action)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_ACTION_SET(set, action_set);
    OXR_CHECK_TYPE(info, XR_TYPE_ACTION_CREATE_INFO);
    OXR_CHECK(out_action);
    *out_action = XR_NULL_HANDLE;
    const OXR_Instance* inst = runtime()->instances.try_get(set->instance);
    OXR_CHECK(inst);

    if (set->ever_attached)
    {
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }
    if (info->actionName[0] == 0)
    {
        return XR_ERROR_NAME_INVALID;
    }
    if (!name_valid(info->actionName, sizeof(info->actionName)))
    {
        return XR_ERROR_PATH_FORMAT_INVALID;
    }
    if (info->localizedActionName[0] == 0)
    {
        return XR_ERROR_LOCALIZED_NAME_INVALID;
    }
    switch (info->actionType)
    {
    case XR_ACTION_TYPE_BOOLEAN_INPUT:
    case XR_ACTION_TYPE_FLOAT_INPUT:
    case XR_ACTION_TYPE_VECTOR2F_INPUT:
    case XR_ACTION_TYPE_POSE_INPUT:
    case XR_ACTION_TYPE_VIBRATION_OUTPUT:
        break;
    default:
        OXR_FAIL("an unknown actionType");
    }
    OXR_CHECK(info->countSubactionPaths == 0 || info->subactionPaths);

    u8 mask = 0;
    for (u32 i = 0; i < info->countSubactionPaths; i++)
    {
        OXR_TopLevel top = {};
        if (!path_string(inst, info->subactionPaths[i]))
        {
            return XR_ERROR_PATH_INVALID;
        }
        if (!path_top_level(inst, info->subactionPaths[i], top) || (mask & (1 << (u32)top)))
        {
            return XR_ERROR_PATH_UNSUPPORTED;
        }
        mask |= (u8)(1 << (u32)top);
    }
    const b8 has_subaction_paths = info->countSubactionPaths > 0;
    if (!has_subaction_paths)
    {
        mask = (1 << (u32)OXR_TopLevel::LeftHand) | (1 << (u32)OXR_TopLevel::RightHand) | (1 << (u32)OXR_TopLevel::Eyes);
    }

    OXR_Runtime*              rt         = runtime();
    const OXR_ActionSetHandle set_handle = action_set_handle(action_set);
    for (const OXR_Action& action : rt->actions)
    {
        if (action.set != set_handle || action.destroyed)
        {
            continue;
        }
        if (cstr_equal(action.name, info->actionName))
        {
            return XR_ERROR_NAME_DUPLICATED;
        }
        if (cstr_equal(action.localized_name, info->localizedActionName))
        {
            return XR_ERROR_LOCALIZED_NAME_DUPLICATED;
        }
    }

    const OXR_ActionHandle handle = rt->actions.add();
    OXR_Action*            action = &rt->actions.get(handle);
    action->set                   = set_handle;
    action->type                  = info->actionType;
    action->subaction_mask        = mask;
    action->has_subaction_paths   = has_subaction_paths;
    action->pose_side             = OXR_TopLevel::Count;
    cstr_copy(action->name, sizeof(action->name), info->actionName);
    cstr_copy(action->localized_name, sizeof(action->localized_name), info->localizedActionName);
    *out_action = action_xr(handle);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyAction (XrAction action)
{
    const OXR_Lock lock(__func__);
    OXR_GET_ACTION(act, action);
    act->destroyed = true;
    return XR_SUCCESS;
}

// Binding modifiers play XrHapticVibration; the other haptic types are accepted and not played
static XrResult modifier_haptic (const OXR_Instance* inst, const XrHapticBaseHeader* haptic, XrHapticVibration& out)
{
    out = {};
    if (!haptic)
    {
        return XR_SUCCESS;
    }
    if (haptic->type == XR_TYPE_HAPTIC_VIBRATION)
    {
        out      = *(const XrHapticVibration*)haptic;
        out.next = nullptr;
        return XR_SUCCESS;
    }
    const b8 known = (haptic->type == XR_TYPE_HAPTIC_AMPLITUDE_ENVELOPE_VIBRATION_FB && inst->ext.FB_haptic_amplitude_envelope) || (haptic->type == XR_TYPE_HAPTIC_PCM_VIBRATION_FB && inst->ext.FB_haptic_pcm) || (haptic->type == XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT && (inst->ext.EXT_haptic_parametric || inst->ext.EXTX1_haptic_parametric));
    OXR_CHECK(known);
    LOG_WARNING("Binding modifier haptics other than XrHapticVibration are not played");
    return XR_SUCCESS;
}

static b8 binding_suggested (const XrInteractionProfileSuggestedBinding* suggested, XrAction action, XrPath binding)
{
    for (u32 i = 0; i < suggested->countSuggestedBindings; i++)
    {
        if (suggested->suggestedBindings[i].action == action && suggested->suggestedBindings[i].binding == binding)
        {
            return true;
        }
    }
    return false;
}

// Spec XR_VALVE_analog_threshold
static XrResult parse_threshold (const OXR_Instance* inst, const XrInteractionProfileSuggestedBinding* suggested, const XrInteractionProfileAnalogThresholdVALVE* threshold, Array<OXR_SuggestedModifier>& out)
{
    OXR_CHECK(zm::is_finite(threshold->onThreshold) && zm::is_finite(threshold->offThreshold));
    OXR_CHECK(threshold->onThreshold >= 0.0f && threshold->onThreshold <= 1.0f && threshold->offThreshold >= 0.0f && threshold->offThreshold <= 1.0f);
    OXR_CHECK(threshold->onThreshold >= threshold->offThreshold);
    if (!binding_suggested(suggested, threshold->action, threshold->binding))
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }
    OXR_SuggestedModifier entry = { .action = action_handle(threshold->action), .path = (u32)threshold->binding, .modifier = { .kind = OXR_ModifierKind::Threshold, .on = threshold->onThreshold, .off = threshold->offThreshold } };
    XrResult result = modifier_haptic(inst, threshold->onHaptic, entry.modifier.on_haptic);
    if (result == XR_SUCCESS)
    {
        result = modifier_haptic(inst, threshold->offHaptic, entry.modifier.off_haptic);
    }
    if (result == XR_SUCCESS)
    {
        out.add(entry);
    }
    return result;
}

// Spec XR_EXT_dpad_binding: a thumbstick or trackpad without component, the ranges of its
// parameters and one struct per input and action set
static XrResult parse_dpad (const OXR_Instance* inst, const OXR_SpecProfile* spec, const XrInteractionProfileDpadBindingEXT* dpad, Array<OXR_SuggestedModifier>& out)
{
    OXR_GET_CONST_ACTION_SET(set, dpad->actionSet);
    UNUSED(set);
    OXR_CHECK(zm::is_finite(dpad->forceThreshold) && zm::is_finite(dpad->forceThresholdReleased) && zm::is_finite(dpad->centerRegion) && zm::is_finite(dpad->wedgeAngle));
    OXR_CHECK(dpad->forceThreshold > 0.0f && dpad->forceThreshold <= 1.0f && dpad->forceThresholdReleased > 0.0f && dpad->forceThresholdReleased <= 1.0f);
    OXR_CHECK(dpad->forceThreshold >= dpad->forceThresholdReleased);
    OXR_CHECK(dpad->centerRegion > 0.0f && dpad->centerRegion < 1.0f && dpad->wedgeAngle >= 0.0f && dpad->wedgeAngle < zm::PI);
    const char* path = path_string(inst, dpad->binding);
    if (!path)
    {
        return XR_ERROR_PATH_INVALID;
    }
    const char* last = path;
    for (const char* c = path; *c; c++)
    {
        last = *c == '/' ? c + 1 : last;
    }
    if (!(cstr_starts_with(last, "thumbstick") || cstr_starts_with(last, "trackpad")) || !spec_binding_valid(inst, spec, path))
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }
    const OXR_ActionSetHandle set_handle = action_set_handle(dpad->actionSet);
    for (const OXR_SuggestedModifier& other : out)
    {
        OXR_CHECK(other.modifier.kind != OXR_ModifierKind::Dpad || other.path != (u32)dpad->binding || other.set != set_handle);
    }
    OXR_SuggestedModifier entry = { .set = set_handle, .path = (u32)dpad->binding, .modifier = { .kind = OXR_ModifierKind::Dpad, .sticky = dpad->isSticky != XR_FALSE, .on = dpad->forceThreshold, .off = dpad->forceThresholdReleased, .center = dpad->centerRegion, .wedge = dpad->wedgeAngle } };
    XrResult result = modifier_haptic(inst, dpad->onHaptic, entry.modifier.on_haptic);
    if (result == XR_SUCCESS)
    {
        result = modifier_haptic(inst, dpad->offHaptic, entry.modifier.off_haptic);
    }
    if (result == XR_SUCCESS)
    {
        out.add(entry);
    }
    return result;
}

// Spec XR_KHR_binding_modification: the modifications of a suggestion; XR_VALVE_analog_threshold
// may also chain one threshold directly, which revision 2 deprecates
static XrResult parse_modifiers (const OXR_Instance* inst, const OXR_SpecProfile* spec, const XrInteractionProfileSuggestedBinding* suggested, Array<OXR_SuggestedModifier>& out)
{
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)suggested->next; next; next = next->next)
    {
        XrResult result = XR_SUCCESS;
        if (next->type == XR_TYPE_INTERACTION_PROFILE_ANALOG_THRESHOLD_VALVE && inst->ext.VALVE_analog_threshold)
        {
            result = parse_threshold(inst, suggested, (const XrInteractionProfileAnalogThresholdVALVE*)next, out);
        }
        else if (next->type == XR_TYPE_BINDING_MODIFICATIONS_KHR && inst->ext.KHR_binding_modification)
        {
            const XrBindingModificationsKHR* mods = (const XrBindingModificationsKHR*)next;
            OXR_CHECK(mods->bindingModificationCount == 0 || mods->bindingModifications);
            for (u32 i = 0; i < mods->bindingModificationCount && result == XR_SUCCESS; i++)
            {
                const XrBindingModificationBaseHeaderKHR* mod = mods->bindingModifications[i];
                OXR_CHECK(mod);
                if (mod->type == XR_TYPE_INTERACTION_PROFILE_ANALOG_THRESHOLD_VALVE && inst->ext.VALVE_analog_threshold)
                {
                    result = parse_threshold(inst, suggested, (const XrInteractionProfileAnalogThresholdVALVE*)mod, out);
                }
                else if (mod->type == XR_TYPE_INTERACTION_PROFILE_DPAD_BINDING_EXT && inst->ext.EXT_dpad_binding)
                {
                    result = parse_dpad(inst, spec, (const XrInteractionProfileDpadBindingEXT*)mod, out);
                }
                else
                {
                    LOG_ERROR("Binding modification of type %d needs an extension that is not enabled", (int)mod->type);
                    result = XR_ERROR_VALIDATION_FAILURE;
                }
            }
        }
        if (result != XR_SUCCESS)
        {
            return result;
        }
    }
    return XR_SUCCESS;
}

// Paths outside the registry that Meta's runtime accepts and OVRPlugin binds, with the registry
// path each stands for
static const struct
{
    const char* profile;
    const char* alias;
    const char* path;
} oxr_binding_aliases[] = {
    { "/interaction_profiles/meta/touch_controller_plus",    "/input/trigger/proximity_fb",  "/input/trigger/proximity_meta" },
    { "/interaction_profiles/meta/touch_controller_plus",    "/input/thumb_fb/proximity_fb", "/input/thumb_meta/proximity_meta" },
    { "/interaction_profiles/facebook/touch_controller_pro", "/output/trigger_haptic_fb",    "/output/haptic_trigger_fb" },
    { "/interaction_profiles/facebook/touch_controller_pro", "/output/thumb_haptic_fb",      "/output/haptic_thumb_fb" },
};

static XrPath binding_alias (OXR_Instance* inst, const char* profile, XrPath binding)
{
    const char* str = path_string(inst, binding);
    for (const auto& alias : oxr_binding_aliases)
    {
        const usize length = cstr_length(str);
        const usize tail   = cstr_length(alias.alias);
        if (cstr_equal(profile, alias.profile) && length > tail && cstr_equal(str + length - tail, alias.alias))
        {
            char full[XR_MAX_PATH_LENGTH];
            cstr_format(full, sizeof(full), "%.*s%s", (int)(length - tail), str, alias.path);
            return path_intern(inst, full);
        }
    }
    return binding;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSuggestInteractionProfileBindings (XrInstance instance, const XrInteractionProfileSuggestedBinding* suggested)
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    OXR_CHECK_TYPE(suggested, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
    OXR_CHECK(suggested->countSuggestedBindings > 0 && suggested->suggestedBindings);

    // A copy: aliases intern paths, which may move the path strings
    char profile_str[XR_MAX_PATH_LENGTH];
    if (!path_string(inst, suggested->interactionProfile))
    {
        return XR_ERROR_PATH_INVALID;
    }
    cstr_copy(profile_str, sizeof(profile_str), path_string(inst, suggested->interactionProfile));
    const OXR_SpecProfile* spec = spec_profile(inst, profile_str);
    if (!spec)
    {
        LOG_WARNING("Interaction profile %s is unknown or needs an extension that is not enabled", profile_str);
        return XR_ERROR_PATH_UNSUPPORTED;
    }
    OXR_Profile profile  = OXR_Profile::None;
    const b8    emulated = path_profile(inst, suggested->interactionProfile, profile);

    const OXR_InstanceHandle handle = instance_handle(instance);
    for (u32 i = 0; i < suggested->countSuggestedBindings; i++)
    {
        const XrActionSuggestedBinding& binding = suggested->suggestedBindings[i];
        OXR_GET_CONST_ACTION(action, binding.action);
        if (!action_instance(action) || runtime()->action_sets.get(action->set).instance != handle)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        // Spec: an action of a set ever attached, by any session of the instance
        if (runtime()->action_sets.get(action->set).ever_attached)
        {
            return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
        }
        if (!path_string(inst, binding.binding))
        {
            return XR_ERROR_PATH_INVALID;
        }
        const XrPath path = binding_alias(inst, profile_str, binding.binding);
        if (!spec_binding_valid(inst, spec, path_string(inst, path)))
        {
            LOG_WARNING("Binding %s is not part of %s", path_string(inst, binding.binding), profile_str);
            return XR_ERROR_PATH_UNSUPPORTED;
        }
    }

    Array<OXR_SuggestedModifier> modifiers = {};
    defer { modifiers.release(); };
    const XrResult modified = parse_modifiers(inst, spec, suggested, modifiers);
    if (modified != XR_SUCCESS)
    {
        return modified;
    }

    if (!emulated)
    {
        LOG_INFO("Accepting bindings for %s, which the simulator does not emulate", profile_str);
        return XR_SUCCESS;
    }

    inst->modifiers[(u32)profile].clear();
    inst->modifiers[(u32)profile].add(modifiers.data, modifiers.length);
    Array<OXR_SuggestedBinding>& list = inst->suggested[(u32)profile];
    list.clear();
    for (u32 i = 0; i < suggested->countSuggestedBindings; i++)
    {
        list.add({ .action = action_handle(suggested->suggestedBindings[i].action), .path = (u32)binding_alias(inst, profile_str, suggested->suggestedBindings[i].binding) });
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrAttachSessionActionSets (XrSession session, const XrSessionActionSetsAttachInfo* info)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
    OXR_CHECK(info->countActionSets > 0 && info->actionSets);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    if (sess->attached)
    {
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }

    for (u32 i = 0; i < info->countActionSets; i++)
    {
        OXR_GET_CONST_ACTION_SET(set, info->actionSets[i]);
        if (set->instance != sess->instance)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (set->attached)
        {
            return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
        }
    }

    for (u32 i = 0; i < info->countActionSets; i++)
    {
        const OXR_ActionSetHandle handle = action_set_handle(info->actionSets[i]);
        OXR_ActionSet*            set    = runtime()->action_sets.try_get(handle);
        set->attached                    = true;
        set->ever_attached               = true;
        sess->attached_sets.add(handle);
    }
    sess->attached          = true;
    const SIM_Controller hardware = sim_get_controller(runtime()->sim);
    SIM_HandSource       sources[2];
    OXR_Profile          profiles[(u32)OXR_TopLevel::Count];
    hand_sources(sources);
    choose_profiles(inst, sess, hardware, sources, profiles);
    bind_actions(inst, sess, profiles, hardware, sources);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetCurrentInteractionProfile (XrSession session, XrPath top_level, XrInteractionProfileState* profile)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(profile, XR_TYPE_INTERACTION_PROFILE_STATE);
    OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    if (!path_string(inst, top_level))
    {
        return XR_ERROR_PATH_INVALID;
    }
    OXR_TopLevel top = {};
    if (!path_top_level(inst, top_level, top))
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }
    if (!sess->attached)
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }

    profile->interactionProfile = XR_NULL_PATH;
    if (side_has_profile(top) && sess->profiles[(u32)top] != OXR_Profile::None)
    {
        profile->interactionProfile = path_intern(inst, oxr_profile_paths[(u32)sess->profiles[(u32)top]]);
    }
    return XR_SUCCESS;
}

static XrResult get_action_state (XrSession session, const XrActionStateGetInfo* info, XrActionType expected, const OXR_ActionState*& out_state)
{
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_ACTION_STATE_GET_INFO);
    OXR_GET_CONST_ACTION(action, info->action);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    const OXR_ActionSet* set = runtime()->action_sets.try_get(action->set);
    OXR_CHECK(set);
    if (action->type != expected)
    {
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }

    u32 index = (u32)OXR_TopLevel::Count;
    if (info->subactionPath != XR_NULL_PATH)
    {
        if (!path_string(inst, info->subactionPath))
        {
            return XR_ERROR_PATH_INVALID;
        }
        OXR_TopLevel top = {};
        if (!action->has_subaction_paths || !path_top_level(inst, info->subactionPath, top) || !(action->subaction_mask & (1 << (u32)top)))
        {
            return XR_ERROR_PATH_UNSUPPORTED;
        }
        index = (u32)top;
    }
    if (!set->attached || !sess->attached)
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    b8 attached_here = false;
    for (const OXR_ActionSetHandle& h : sess->attached_sets)
    {
        attached_here |= h == action->set;
    }
    if (!attached_here)
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }

    out_state = &action->states[index];
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetActionStateBoolean (XrSession session, const XrActionStateGetInfo* info, XrActionStateBoolean* state)
{
    const OXR_Lock lock(__func__);
    OXR_CHECK_TYPE(state, XR_TYPE_ACTION_STATE_BOOLEAN);
    const OXR_ActionState* s      = nullptr;
    const XrResult         result = get_action_state(session, info, XR_ACTION_TYPE_BOOLEAN_INPUT, s);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    state->currentState         = s->value.x != 0.0f ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = s->changed ? XR_TRUE : XR_FALSE;
    state->lastChangeTime       = s->last_change_time;
    state->isActive             = s->active ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetActionStateFloat (XrSession session, const XrActionStateGetInfo* info, XrActionStateFloat* state)
{
    const OXR_Lock lock(__func__);
    OXR_CHECK_TYPE(state, XR_TYPE_ACTION_STATE_FLOAT);
    const OXR_ActionState* s      = nullptr;
    const XrResult         result = get_action_state(session, info, XR_ACTION_TYPE_FLOAT_INPUT, s);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    state->currentState         = s->value.x;
    state->changedSinceLastSync = s->changed ? XR_TRUE : XR_FALSE;
    state->lastChangeTime       = s->last_change_time;
    state->isActive             = s->active ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetActionStateVector2f (XrSession session, const XrActionStateGetInfo* info, XrActionStateVector2f* state)
{
    const OXR_Lock lock(__func__);
    OXR_CHECK_TYPE(state, XR_TYPE_ACTION_STATE_VECTOR2F);
    const OXR_ActionState* s      = nullptr;
    const XrResult         result = get_action_state(session, info, XR_ACTION_TYPE_VECTOR2F_INPUT, s);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    state->currentState         = { s->value.x, s->value.y };
    state->changedSinceLastSync = s->changed ? XR_TRUE : XR_FALSE;
    state->lastChangeTime       = s->last_change_time;
    state->isActive             = s->active ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetActionStatePose (XrSession session, const XrActionStateGetInfo* info, XrActionStatePose* state)
{
    const OXR_Lock lock(__func__);
    OXR_CHECK_TYPE(state, XR_TYPE_ACTION_STATE_POSE);
    const OXR_ActionState* s      = nullptr;
    const XrResult         result = get_action_state(session, info, XR_ACTION_TYPE_POSE_INPUT, s);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    state->isActive = s->active ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSyncActions (XrSession session, const XrActionsSyncInfo* info)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_ACTIONS_SYNC_INFO);
    OXR_CHECK(info->countActiveActionSets == 0 || info->activeActionSets);
    OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    if (!sess->attached)
    {
        if (info->countActiveActionSets > 0)
        {
            return XR_ERROR_ACTIONSET_NOT_ATTACHED;
        }
        return sess->state == XR_SESSION_STATE_FOCUSED ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
    }

    OXR_Runtime* rt = runtime();
    for (u32 i = 0; i < info->countActiveActionSets; i++)
    {
        OXR_GET_CONST_ACTION_SET(set, info->activeActionSets[i].actionSet);
        b8 attached_here = false;
        for (const OXR_ActionSetHandle& h : sess->attached_sets)
        {
            attached_here |= h == action_set_handle(info->activeActionSets[i].actionSet);
        }
        if (!set->attached || !attached_here)
        {
            return XR_ERROR_ACTIONSET_NOT_ATTACHED;
        }
        if (info->activeActionSets[i].subactionPath != XR_NULL_PATH)
        {
            if (!path_string(inst, info->activeActionSets[i].subactionPath))
            {
                return XR_ERROR_PATH_INVALID;
            }
            OXR_TopLevel top = {};
            if (!path_top_level(inst, info->activeActionSets[i].subactionPath, top))
            {
                return XR_ERROR_PATH_UNSUPPORTED;
            }
            // Spec 11.8 xrSyncActions: some action of the set must have declared the path
            const OXR_ActionSetHandle handle   = action_set_handle(info->activeActionSets[i].actionSet);
            b8                        declared = false;
            for (usize a = 0; a < rt->actions.length() && !declared; a++)
            {
                const OXR_Action& action = rt->actions.data()[a];
                declared                 = action.set == handle && action.has_subaction_paths && (action.subaction_mask & (1 << (u32)top));
            }
            if (!declared)
            {
                return XR_ERROR_PATH_UNSUPPORTED;
            }
        }
    }

    const b8 focused = sess->state == XR_SESSION_STATE_FOCUSED;
    for (const OXR_ActionSetHandle& h : sess->attached_sets)
    {
        OXR_ActionSet* set = rt->action_sets.try_get(h);
        if (set)
        {
            set->active       = false;
            set->active_sides = 0;
        }
    }
    if (focused)
    {
        for (u32 i = 0; i < info->countActiveActionSets; i++)
        {
            OXR_ActionSet* set = oxr_find_action_set(info->activeActionSets[i].actionSet);
            set->active        = true;
            OXR_TopLevel top = {};
            if (info->activeActionSets[i].subactionPath != XR_NULL_PATH && path_top_level(inst, info->activeActionSets[i].subactionPath, top))
            {
                set->active_sides |= (u8)(1 << (u32)top);
            }
            else
            {
                set->active_sides = 0xFF;
            }
        }
    }

    const SIM_Controller hardware = sim_get_controller(rt->sim);
    SIM_HandSource       sources[2];
    OXR_Profile          profiles[(u32)OXR_TopLevel::Count];
    hand_sources(sources);
    choose_profiles(inst, sess, hardware, sources, profiles);
    const b8 changed = !memory_equal(profiles, sess->profiles, sizeof(profiles));
    if (changed || hardware != sess->profile_hardware || sources[0] != sess->profile_sources[0] || sources[1] != sess->profile_sources[1])
    {
        memory_copy(sess->profiles, profiles, sizeof(profiles));
        bind_actions(inst, sess, profiles, hardware, sources);
        if (changed)
        {
            push_event(inst, { .type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED, .session = session });
        }
    }

    suppress_by_priority(sess);
    const XrTime now = sim_time(rt->sim);
    for (usize i = 0; i < rt->actions.length(); i++)
    {
        OXR_Action*          action = &rt->actions.data()[i];
        const OXR_ActionSet* set    = rt->action_sets.try_get(action->set);
        if (!set || !set->attached)
        {
            continue;
        }
        b8 attached_here = false;
        for (const OXR_ActionSetHandle& h : sess->attached_sets)
        {
            attached_here |= h == action->set;
        }
        if (attached_here)
        {
            sync_action(action, set, rt->sim, now);
        }
    }
    sync_render_models(rt, inst, sess, session_handle(session));
    return focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateBoundSourcesForAction (XrSession session, const XrBoundSourcesForActionEnumerateInfo* info, uint32_t capacity, uint32_t* count_output, XrPath* sources)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO);
    OXR_GET_CONST_ACTION(action, info->action);
    if (!sess->attached)
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }

    XrPath all[(u32)OXR_TopLevel::Count * OXR_MAX_BOUND_SOURCES];
    u32    count = 0;
    for (u32 side = 0; side < (u32)OXR_TopLevel::Count; side++)
    {
        for (u32 i = 0; i < action->sources_count[side]; i++)
        {
            if (!action->sources[side][i].suppressed)
            {
                all[count++] = action->sources[side][i].path;
            }
        }
    }
    const XrResult result = oxr_two_call(capacity, count_output, sources, count);
    if (result == XR_SUCCESS && capacity > 0)
    {
        memory_copy(sources, all, count * sizeof(XrPath));
    }
    return result;
}

static void append_words (char* out, usize out_size, const char* text)
{
    usize length = cstr_length(out);
    if (length > 0 && length + 1 < out_size)
    {
        out[length++] = ' ';
        out[length]   = 0;
    }
    b8 capitalize = true;
    for (usize i = 0; text[i] && length + 1 < out_size; i++)
    {
        char c = text[i];
        if (c == '/' || c == '_' || c == '-')
        {
            if (length > 0 && out[length - 1] != ' ')
            {
                out[length++] = ' ';
            }
            capitalize = true;
            continue;
        }
        if (capitalize && c >= 'a' && c <= 'z')
        {
            c = (char)(c - 'a' + 'A');
        }
        capitalize    = false;
        out[length++] = c;
    }
    out[length] = 0;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetInputSourceLocalizedName (XrSession session, const XrInputSourceLocalizedNameGetInfo* info, uint32_t capacity, uint32_t* count_output, char* buffer)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    if (!sess->attached)
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    const char* source = path_string(inst, info->sourcePath);
    if (!source)
    {
        return XR_ERROR_PATH_INVALID;
    }
    if (info->whichComponents == 0)
    {
        OXR_FAIL("info->whichComponents == 0");
    }

    const char*       user      = nullptr;
    const char*       component = nullptr;
    OXR_Profile       profile   = OXR_Profile::None;
    for (u32 i = 0; i < (u32)OXR_TopLevel::Count; i++)
    {
        if (cstr_starts_with(source, oxr_top_level_paths[i]) && source[cstr_length(oxr_top_level_paths[i])] == '/')
        {
            user      = oxr_top_level_paths[i] + cstr_length("/user/");
            component = source + cstr_length(oxr_top_level_paths[i]) + 1;
            profile   = side_has_profile((OXR_TopLevel)i) ? sess->profiles[i] : OXR_Profile::None;
        }
    }
    if (!user)
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    char name[256] = {};
    if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT)
    {
        append_words(name, sizeof(name), user);
    }
    if ((info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT) && profile != OXR_Profile::None)
    {
        append_words(name, sizeof(name), oxr_profile_paths[(u32)profile] + cstr_length("/interaction_profiles/"));
    }
    if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT)
    {
        append_words(name, sizeof(name), cstr_starts_with(component, "input/") ? component + 6 : component);
    }

    const u32      length = (u32)cstr_length(name) + 1;
    const XrResult result = oxr_two_call(capacity, count_output, buffer, length);
    if (result == XR_SUCCESS && capacity > 0)
    {
        cstr_copy(buffer, capacity, name);
    }
    return result;
}

static XrResult haptic_sides (XrSession session, const XrHapticActionInfo* info, OXR_Action*& out_action, u8& out_sides)
{
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_HAPTIC_ACTION_INFO);
    OXR_GET_ACTION(action, info->action);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    if (action->type != XR_ACTION_TYPE_VIBRATION_OUTPUT)
    {
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }
    u8 sides = action->subaction_mask;
    if (info->subactionPath != XR_NULL_PATH)
    {
        if (!path_string(inst, info->subactionPath))
        {
            return XR_ERROR_PATH_INVALID;
        }
        OXR_TopLevel top = {};
        if (!action->has_subaction_paths || !path_top_level(inst, info->subactionPath, top) || !(action->subaction_mask & (1 << (u32)top)))
        {
            return XR_ERROR_PATH_UNSUPPORTED;
        }
        sides = (u8)(1 << (u32)top);
    }
    const OXR_ActionSet* set = runtime()->action_sets.try_get(action->set);
    if (!set || !set->attached || !sess->attached)
    {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    out_action = action;
    out_sides  = sides;
    return XR_SUCCESS;
}

// XR_FB_haptic_pcm: samples within the buffer the app said it has, at a usable rate
static XrResult validate_pcm (const XrHapticPcmVibrationFB* pcm)
{
    OXR_CHECK(pcm->samplesConsumed && (pcm->bufferSize == 0 || pcm->buffer));
    OXR_CHECK(zm::is_finite(pcm->sampleRate) && pcm->sampleRate > 0.0f);
    for (u32 i = 0; i < pcm->bufferSize; i++)
    {
        OXR_CHECK(zm::is_finite(pcm->buffer[i]));
    }
    return XR_SUCCESS;
}

// Spec XR_EXT_haptic_parametric points: times never decrease, values in [0, 1]
static b8 points_valid (const XrHapticParametricPointEXT* points, u32 count)
{
    for (u32 i = 0; i < count; i++)
    {
        if (!zm::is_finite(points[i].value) || points[i].value < 0.0f || points[i].value > 1.0f || points[i].time < 0 || (i > 0 && points[i].time < points[i - 1].time))
        {
            return false;
        }
    }
    return true;
}

// Spec XR_EXT_haptic_parametric, validation of an XrHapticParametricVibrationEXT
static XrResult validate_parametric (const XrHapticParametricVibrationEXT* v)
{
    const XrHapticParametricStreamFrameTypeEXT type  = v->streamFrameType;
    const b8                                   start = type == XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_NONE_EXT || type == XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_FIRST_FRAME_EXT;
    const b8                                   ends  = type == XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_NONE_EXT || type == XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_LAST_FRAME_EXT;
    OXR_CHECK(start || type == XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_INTERMEDIATE_FRAME_EXT || type == XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_LAST_FRAME_EXT);
    OXR_CHECK(v->amplitudePoints && v->amplitudePointCount >= (start ? 2u : 1u));
    OXR_CHECK(v->frequencyPointCount == 0 || v->frequencyPoints);
    OXR_CHECK(v->transientCount == 0 || v->transients);
    const u32 max = XR_HAPTIC_PARAMETRIC_MAX_POINTS_TRANSIENTS_EXT;
    OXR_CHECK(v->amplitudePointCount <= max && v->frequencyPointCount <= max && v->transientCount <= max);
    OXR_CHECK(points_valid(v->amplitudePoints, v->amplitudePointCount) && points_valid(v->frequencyPoints, v->frequencyPointCount));
    OXR_CHECK(!start || (v->amplitudePoints[0].time == 0 && (v->frequencyPointCount == 0 || v->frequencyPoints[0].time == 0)));
    // Spec: frequencies and transients end with the amplitudes of the stream, so earlier frames may reach past their own
    const XrDuration last = v->amplitudePoints[v->amplitudePointCount - 1].time;
    OXR_CHECK(!ends || v->frequencyPointCount == 0 || v->frequencyPoints[v->frequencyPointCount - 1].time <= last);
    for (u32 i = 0; i < v->transientCount; i++)
    {
        const XrHapticParametricTransientEXT& t = v->transients[i];
        OXR_CHECK(t.time >= 0 && (!ends || t.time <= last) && (i == 0 || t.time > v->transients[i - 1].time));
        OXR_CHECK(zm::is_finite(t.amplitude) && t.amplitude >= 0.0f && t.amplitude <= 1.0f && zm::is_finite(t.frequency) && t.frequency >= 0.0f && t.frequency <= 1.0f);
    }
    // Both frequencies unspecified, or both in range with max >= min; later frames of a stream keep the first's
    const b8 unspecified = v->minFrequencyHz == XR_FREQUENCY_UNSPECIFIED && v->maxFrequencyHz == XR_FREQUENCY_UNSPECIFIED;
    const f32 low        = XR_HAPTIC_PARAMETRIC_FREQUENCY_MIN_HZ_EXT;
    const f32 high       = XR_HAPTIC_PARAMETRIC_FREQUENCY_MAX_HZ_EXT;
    OXR_CHECK(unspecified || (start && v->minFrequencyHz >= low && v->maxFrequencyHz <= high && v->maxFrequencyHz >= v->minFrequencyHz));
    return XR_SUCCESS;
}

// Spec: a frame continuing a stream goes on in the stream's time, and the last frame's
// amplitudes outlast the frequencies and transients of every frame
static XrResult validate_stream_frame (const OXR_Action* action, u8 sides, const XrHapticParametricVibrationEXT* v)
{
    const b8 last = v->streamFrameType == XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_LAST_FRAME_EXT;
    if (!last && v->streamFrameType != XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_INTERMEDIATE_FRAME_EXT)
    {
        return XR_SUCCESS;
    }
    const XrDuration end = v->amplitudePoints[v->amplitudePointCount - 1].time;
    for (u32 side = 0; side < (u32)OXR_TopLevel::Count; side++)
    {
        if ((sides & (1 << side)) && side_has_device((OXR_TopLevel)side) && side_bound(action, side))
        {
            const u8 outputs = bound_haptics(action, side);
            for (u32 output = 0; output < (u32)SIM_HapticOutput::Count; output++)
            {
                if (!(outputs & (1u << output)))
                {
                    continue;
                }
                SIM_HapticStreamEnd stream = {};
                OXR_CHECK(sim_haptic_stream_end(runtime()->sim, side_device((OXR_TopLevel)side), (SIM_HapticOutput)output, stream));
                OXR_CHECK(v->amplitudePoints[0].time >= stream.amplitude);
                OXR_CHECK(v->frequencyPointCount == 0 || v->frequencyPoints[0].time >= stream.frequency);
                OXR_CHECK(v->transientCount == 0 || v->transients[0].time > stream.transient);
                OXR_CHECK(!last || (stream.frequency <= end && stream.transient <= end));
            }
        }
    }
    return XR_SUCCESS;
}

// A last frame ends its stream even when it is rejected; the CTS starts no new stream before
// checking that frames continuing one are refused
static void end_parametric_streams (const OXR_Action* action, u8 sides)
{
    for (u32 side = 0; side < (u32)OXR_TopLevel::Count; side++)
    {
        if ((sides & (1 << side)) && side_has_device((OXR_TopLevel)side) && side_bound(action, side))
        {
            const u8 outputs = bound_haptics(action, side);
            for (u32 output = 0; output < (u32)SIM_HapticOutput::Count; output++)
            {
                const SIM_Device device = side_device((OXR_TopLevel)side);
                if ((outputs & (1u << output)) && sim_haptic_stream_running(runtime()->sim, device, (SIM_HapticOutput)output))
                {
                    sim_stop_haptic(runtime()->sim, device, (SIM_HapticOutput)output);
                }
            }
        }
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrApplyHapticFeedback (XrSession session, const XrHapticActionInfo* info, const XrHapticBaseHeader* feedback)
{
    const OXR_Lock lock(__func__);
    OXR_Action* action = nullptr;
    u8          sides  = 0;
    const XrResult result = haptic_sides(session, info, action, sides);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    OXR_CHECK(feedback);
    const OXR_Session*  sess     = oxr_find_session(session);
    const OXR_Instance* inst     = sess ? session_instance(sess) : nullptr;
    const b8            envelope = feedback->type == XR_TYPE_HAPTIC_AMPLITUDE_ENVELOPE_VIBRATION_FB && inst && inst->ext.FB_haptic_amplitude_envelope;
    const b8            pcm      = feedback->type == XR_TYPE_HAPTIC_PCM_VIBRATION_FB && inst && inst->ext.FB_haptic_pcm;
    const b8            param    = feedback->type == XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT && inst && (inst->ext.EXT_haptic_parametric || inst->ext.EXTX1_haptic_parametric);
    OXR_CHECK(feedback->type == XR_TYPE_HAPTIC_VIBRATION || envelope || pcm || param);
    const XrHapticVibration*                    vibration = (const XrHapticVibration*)feedback;
    const XrHapticAmplitudeEnvelopeVibrationFB* samples   = (const XrHapticAmplitudeEnvelopeVibrationFB*)feedback;
    const XrHapticPcmVibrationFB*               pcm_data  = (const XrHapticPcmVibrationFB*)feedback;
    const XrHapticParametricVibrationEXT*       curves    = (const XrHapticParametricVibrationEXT*)feedback;
    if (pcm && validate_pcm(pcm_data) != XR_SUCCESS)
    {
        OXR_FAIL("an invalid XrHapticPcmVibrationFB");
    }
    if (param && (validate_parametric(curves) != XR_SUCCESS || validate_stream_frame(action, sides, curves) != XR_SUCCESS))
    {
        if (curves->streamFrameType == XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_LAST_FRAME_EXT)
        {
            end_parametric_streams(action, sides);
        }
        OXR_FAIL("an invalid XrHapticParametricVibrationEXT or stream frame");
    }
    if (envelope)
    {
        OXR_CHECK(samples->amplitudeCount > 0 && samples->amplitudes && samples->duration >= 0);
        for (u32 i = 0; i < samples->amplitudeCount; i++)
        {
            OXR_CHECK(zm::is_finite(samples->amplitudes[i]));
        }
    }

    // Parametric points the sim takes
    SIM_HapticPoint amplitude_points[XR_HAPTIC_PARAMETRIC_MAX_POINTS_TRANSIENTS_EXT];
    SIM_HapticPoint frequency_points[XR_HAPTIC_PARAMETRIC_MAX_POINTS_TRANSIENTS_EXT];
    SIM_HapticParametric effect = {};
    if (param)
    {
        for (u32 i = 0; i < curves->amplitudePointCount; i++)
        {
            amplitude_points[i] = { curves->amplitudePoints[i].time, curves->amplitudePoints[i].value };
        }
        for (u32 i = 0; i < curves->frequencyPointCount; i++)
        {
            frequency_points[i] = { curves->frequencyPoints[i].time, curves->frequencyPoints[i].value };
        }
        effect = {
            .amplitudes        = amplitude_points,
            .amplitudes_count  = curves->amplitudePointCount,
            .frequencies       = frequency_points,
            .frequencies_count = curves->frequencyPointCount,
            .min_hz            = curves->minFrequencyHz,
            .max_hz            = curves->maxFrequencyHz,
            .stream            = (SIM_HapticStream)curves->streamFrameType,
            .last_transient    = curves->transientCount > 0 ? curves->transients[curves->transientCount - 1].time : -1,
        };
    }

    // Spec XR_FB_haptic_pcm: samplesConsumed is written before returning; with several outputs, what all of them took
    u32 consumed = pcm ? pcm_data->bufferSize : 0;
    b8  played   = false;
    for (u32 side = 0; side < (u32)OXR_TopLevel::Count; side++)
    {
        if ((sides & (1 << side)) && side_has_device((OXR_TopLevel)side) && side_bound(action, side))
        {
            const u8 outputs = bound_haptics(action, side);
            for (u32 output = 0; output < (u32)SIM_HapticOutput::Count; output++)
            {
                if (outputs & (1u << output))
                {
                    played = true;
                    if (pcm)
                    {
                        consumed = zm::min(consumed, sim_apply_haptic_pcm(runtime()->sim, side_device((OXR_TopLevel)side), (SIM_HapticOutput)output, pcm_data->buffer, pcm_data->bufferSize, pcm_data->sampleRate, pcm_data->append != XR_FALSE));
                    }
                    else if (param)
                    {
                        sim_apply_haptic_parametric(runtime()->sim, side_device((OXR_TopLevel)side), (SIM_HapticOutput)output, effect);
                    }
                    else if (envelope)
                    {
                        sim_apply_haptic_envelope(runtime()->sim, side_device((OXR_TopLevel)side), (SIM_HapticOutput)output, samples->amplitudes, samples->amplitudeCount, samples->duration);
                    }
                    else
                    {
                        sim_apply_haptic(runtime()->sim, side_device((OXR_TopLevel)side), (SIM_HapticOutput)output, vibration->amplitude, vibration->duration, vibration->frequency);
                    }
                }
            }
        }
    }
    if (pcm)
    {
        *pcm_data->samplesConsumed = played ? consumed : 0;
    }
    return XR_SUCCESS;
}

// The simulated controllers play PCM at one rate on every haptic output
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetDeviceSampleRateFB (XrSession session, const XrHapticActionInfo* info, XrDevicePcmSampleRateGetInfoFB* rate)
{
    const OXR_Lock lock(__func__);
    OXR_Action*    action = nullptr;
    u8             sides  = 0;
    const XrResult result = haptic_sides(session, info, action, sides);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    OXR_CHECK_TYPE(rate, XR_TYPE_DEVICE_PCM_SAMPLE_RATE_STATE_FB);
    rate->sampleRate = 0.0f;
    for (u32 side = 0; side < (u32)OXR_TopLevel::Count; side++)
    {
        if ((sides & (1 << side)) && side_has_device((OXR_TopLevel)side) && side_bound(action, side) && bound_haptics(action, side))
        {
            rate->sampleRate = SIM_HAPTIC_PCM_RATE;
        }
    }
    return XR_SUCCESS;
}

// XR_EXT_haptic_parametric, also as xrHapticParametricGetPropertiesEXTX1
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrHapticParametricGetPropertiesEXT (XrSession session, const XrHapticActionInfo* info, XrHapticParametricPropertiesEXT* properties)
{
    const OXR_Lock lock(__func__);
    OXR_Action*    action = nullptr;
    u8             sides  = 0;
    const XrResult result = haptic_sides(session, info, action, sides);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    OXR_CHECK_TYPE(properties, XR_TYPE_HAPTIC_PARAMETRIC_PROPERTIES_EXT);
    properties->idealFrameSubmissionRate  = OXR_HAPTIC_FRAME_PERIOD;
    properties->minimumFirstFrameDuration = 0;
    properties->minFrequencyHz            = SIM_HAPTIC_MIN_HZ;
    properties->maxFrequencyHz            = SIM_HAPTIC_MAX_HZ;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrStopHapticFeedback (XrSession session, const XrHapticActionInfo* info)
{
    const OXR_Lock lock(__func__);
    OXR_Action* action = nullptr;
    u8          sides  = 0;
    const XrResult result = haptic_sides(session, info, action, sides);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    for (u32 side = 0; side < (u32)OXR_TopLevel::Count; side++)
    {
        if ((sides & (1 << side)) && side_has_device((OXR_TopLevel)side) && side_bound(action, side))
        {
            const u8 outputs = bound_haptics(action, side);
            for (u32 output = 0; output < (u32)SIM_HapticOutput::Count; output++)
            {
                if (outputs & (1u << output))
                {
                    sim_stop_haptic(runtime()->sim, side_device((OXR_TopLevel)side), (SIM_HapticOutput)output);
                }
            }
        }
    }
    return XR_SUCCESS;
}

// Spatial entities: XR_FB_spatial_entity, _query, _container, XR_FB_scene, _capture, XR_META_spatial_entity_discovery, _mesh

// A space handed to a function that also takes a session must have been created from that session
// NOLINTBEGIN(bugprone-macro-parentheses): var names the variable the macro declares
#define OXR_GET_SESSION_SPACE(var, sess_handle, h) \
    OXR_GET_SPACE(var, h);                         \
    OXR_CHECK(var->session == session_handle(sess_handle))
#define OXR_GET_CONST_SESSION_SPACE(var, sess_handle, h) \
    OXR_GET_CONST_SPACE(var, h);                         \
    OXR_CHECK(var->session == session_handle(sess_handle))
// NOLINTEND(bugprone-macro-parentheses)

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateSpatialAnchorFB (XrSession session, const XrSpatialAnchorCreateInfoFB* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_FB);
    OXR_CHECK(request_id);
    OXR_GET_CONST_SPACE(base, info->space);
    if (base->session != session_handle(session))
    {
        OXR_FAIL("a space of another session");
    }
    if (!xr_pose_valid(info->poseInSpace))
    {
        return XR_ERROR_POSE_INVALID;
    }
    const XrTime time = locate_time(info->time);
    if (time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }

    OXR_Instance* inst   = session_instance(sess);
    SIM_Sample    sample = {};
    sim_sample(runtime()->sim, time, sample);
    const OXR_Located located = locate_world(base, sample);

    OXR_Event event  = { .type = XR_TYPE_EVENT_DATA_SPATIAL_ANCHOR_CREATE_COMPLETE_FB, .session = session, .request_id = next_request_id(inst), .result = XR_ERROR_RUNTIME_FAILURE };
    if (located.valid)
    {
        const W_Uuid          uuid   = make_anchor_uuid(runtime());
        const OXR_SpaceHandle handle = create_anchor_space(session_handle(session), uuid, zm::pose_mul(located.pose, pose_from_xr(info->poseInSpace)), component_bit(OXR_Component::Locatable), 0);
        event.result                 = XR_SUCCESS;
        event.space                  = space_xr(handle);
        event.uuid                   = uuid;
    }
    *request_id = event.request_id;
    push_anchor_event(inst, event);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceUuidFB (XrSpace space, XrUuidEXT* uuid)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SPACE(target, space);
    OXR_CHECK(uuid);
    OXR_CHECK(target->kind == OXR_SpaceKind::Entity || target->kind == OXR_SpaceKind::Anchor || target->kind == OXR_SpaceKind::Prop);
    *uuid = uuid_to_xr(target->uuid);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateSpaceSupportedComponentsFB (XrSpace space, uint32_t capacity, uint32_t* count_output, XrSpaceComponentTypeFB* types)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SPACE(target, space);
    OXR_CHECK(count_output);
    XrSpaceComponentTypeFB supported[(u32)OXR_Component::Count];
    u32                    count = 0;
    const u32              mask  = supported_components(target);
    for (u32 i = 0; i < (u32)OXR_Component::Count; i++)
    {
        if (mask & (1u << i))
        {
            supported[count++] = oxr_component_types[i];
        }
    }
    const XrResult result = oxr_two_call(capacity, count_output, types, count);
    if (result == XR_SUCCESS && capacity > 0)
    {
        memory_copy(types, supported, count * sizeof(XrSpaceComponentTypeFB));
    }
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetSpaceComponentStatusFB (XrSpace space, const XrSpaceComponentStatusSetInfoFB* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SPACE(target, space);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB);
    OXR_CHECK(request_id);
    OXR_Component component = OXR_Component::Count;
    OXR_CHECK(component_from_xr(info->componentType, component));
    if (!(supported_components(target) & component_bit(component)))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_SUPPORTED_FB;
    }
    if (component_enabled(target, component) == (info->enabled != XR_FALSE))
    {
        return XR_ERROR_SPACE_COMPONENT_STATUS_ALREADY_SET_FB;
    }

    target->components_enabled ^= component_bit(component);
    const OXR_Session*  sess = runtime()->sessions.try_get(target->session);
    OXR_Instance* inst = sess ? session_instance(sess) : nullptr;
    if (!inst)
    {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *request_id = next_request_id(inst);
    push_event(inst, { .type = XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB, .session = session_xr(target->session), .request_id = *request_id, .result = XR_SUCCESS, .space = space,
                       .uuid = target->uuid, .component = (u32)info->componentType, .enabled = info->enabled != XR_FALSE });
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceComponentStatusFB (XrSpace space, XrSpaceComponentTypeFB type, XrSpaceComponentStatusFB* status)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SPACE(target, space);
    OXR_CHECK_TYPE(status, XR_TYPE_SPACE_COMPONENT_STATUS_FB);
    OXR_Component component = OXR_Component::Count;
    OXR_CHECK(component_from_xr(type, component));
    if (!(supported_components(target) & component_bit(component)))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_SUPPORTED_FB;
    }
    status->enabled       = component_enabled(target, component) ? XR_TRUE : XR_FALSE;
    status->changePending = XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrQuerySpacesFB (XrSession session, const XrSpaceQueryInfoBaseHeaderFB* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACE_QUERY_INFO_FB);
    OXR_CHECK(request_id);
    const XrSpaceQueryInfoFB* query = (const XrSpaceQueryInfoFB*)info;
    OXR_CHECK(query->queryAction == XR_SPACE_QUERY_ACTION_LOAD_FB);

    OXR_EntityFilter filter     = {};
    OXR_EntityFilter exclude    = {};
    OXR_QuerySource  source     = OXR_QuerySource::Local;
    W_Uuid           group_uuid = {};
    b8               group      = false;
    // XR_META_spatial_entity_group_sharing: XrSpaceGroupUuidFilterInfoMETA extends the query in the
    // registry, the extension's text puts it among the query filters, where OVRPlugin passes it
    const b8 group_sharing = session_instance(sess)->ext.META_spatial_entity_group_sharing;
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)query->next; next; next = next->next)
    {
        if (next->type == XR_TYPE_SPACE_GROUP_UUID_FILTER_INFO_META && group_sharing)
        {
            group_uuid = uuid_from_xr(((const XrSpaceGroupUuidFilterInfoMETA*)next)->groupUuid);
            group      = true;
        }
    }
    if (query->filter && query->filter->type == XR_TYPE_SPACE_GROUP_UUID_FILTER_INFO_META && group_sharing)
    {
        group_uuid = uuid_from_xr(((const XrSpaceGroupUuidFilterInfoMETA*)query->filter)->groupUuid);
        group      = true;
    }
    else if (query->filter)
    {
        // XrSpaceStorageLocationFilterInfoFB extends the filter
        b8 has_location = false;
        for (const XrBaseInStructure* next = ((const XrBaseInStructure*)query->filter)->next; next; next = next->next)
        {
            if (next->type == XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB)
            {
                const XrSpaceStorageLocationFB location = ((const XrSpaceStorageLocationFilterInfoFB*)next)->location;
                OXR_CHECK(location == XR_SPACE_STORAGE_LOCATION_LOCAL_FB || location == XR_SPACE_STORAGE_LOCATION_CLOUD_FB);
                source       = location == XR_SPACE_STORAGE_LOCATION_CLOUD_FB ? OXR_QuerySource::Cloud : OXR_QuerySource::Local;
                has_location = true;
            }
        }
        // The spec does not ask for it, Horizon OS does: a Quest 3 returns XR_ERROR_VALIDATION_FAILURE
        if (query->filter->type == XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB && !has_location)
        {
            OXR_FAIL("a component filter without XrSpaceStorageLocationFilterInfoFB");
        }
        const XrResult result = parse_entity_filter((const XrBaseInStructure*)query->filter, XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB, XR_TYPE_SPACE_UUID_FILTER_INFO_FB, filter);
        if (XR_FAILED(result))
        {
            return result;
        }
    }
    if (query->excludeFilter)
    {
        const XrResult result = parse_entity_filter((const XrBaseInStructure*)query->excludeFilter, XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB, XR_TYPE_SPACE_UUID_FILTER_INFO_FB, exclude);
        if (XR_FAILED(result))
        {
            return result;
        }
    }

    OXR_Instance* inst = session_instance(sess);
    *request_id        = next_request_id(inst);
    run_query(inst, session_handle(session), *request_id, false, group ? OXR_QuerySource::Cloud : source, group ? &group_uuid : nullptr, &filter, 1, query->excludeFilter ? &exclude : nullptr, query->maxResultCount);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrRetrieveSpaceQueryResultsFB (XrSession session, XrAsyncRequestIdFB request_id, XrSpaceQueryResultsFB* results)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(results, XR_TYPE_SPACE_QUERY_RESULTS_FB);
    return retrieve_query(session_handle(session), request_id, false, results->resultCapacityInput, &results->resultCountOutput, results->results);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDiscoverSpacesMETA (XrSession session, const XrSpaceDiscoveryInfoMETA* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACE_DISCOVERY_INFO_META);
    OXR_CHECK(request_id);
    OXR_CHECK(info->filterCount == 0 || info->filters);

    OXR_EntityFilter filters[8];
    OXR_CHECK(info->filterCount <= ARRAY_SIZE(filters));
    for (u32 i = 0; i < info->filterCount; i++)
    {
        const XrResult result = parse_entity_filter((const XrBaseInStructure*)info->filters[i], XR_TYPE_SPACE_FILTER_COMPONENT_META, XR_TYPE_SPACE_FILTER_UUID_META, filters[i]);
        if (XR_FAILED(result))
        {
            return result;
        }
    }
    if (!sess->running)
    {
        const SIM_Config& config = sim_get_config(runtime()->sim);
        const char*       late   = config.discover_before_running ? nullptr : sim_late_discovery_module(config, os_module_loaded);
        if (!config.discover_before_running && !late)
        {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
        // The spec allows XR_ERROR_SESSION_NOT_RUNNING; MRUK never retries after it
        LOG_ERROR("xrDiscoverSpacesMETA before the session runs, answered as %s", late ? late : "discover_before_running is set");
    }

    OXR_Instance* inst = session_instance(sess);
    *request_id        = next_request_id(inst);
    run_query(inst, session_handle(session), *request_id, true, OXR_QuerySource::Local, nullptr, filters, info->filterCount, nullptr, 0xffffffffu);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrRetrieveSpaceDiscoveryResultsMETA (XrSession session, XrAsyncRequestIdFB request_id, XrSpaceDiscoveryResultsMETA* results)
{
    static_assert(sizeof(XrSpaceDiscoveryResultMETA) == sizeof(XrSpaceQueryResultFB), "discovery and query results share a layout");
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(results, XR_TYPE_SPACE_DISCOVERY_RESULTS_META);
    return retrieve_query(session_handle(session), request_id, true, results->resultCapacityInput, &results->resultCountOutput, (XrSpaceQueryResultFB*)results->results);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceContainerFB (XrSession session, XrSpace space, XrSpaceContainerFB* container)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_GET_CONST_SESSION_SPACE(target, session, space);
    OXR_CHECK_TYPE(container, XR_TYPE_SPACE_CONTAINER_FB);
    if (!component_enabled(target, OXR_Component::SpaceContainer))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
    }
    const W_World*  world  = runtime()->world;
    const W_Room&   room   = w_room(world, w_entity(world, target->entity).room);
    const XrResult  result = oxr_two_call(container->uuidCapacityInput, &container->uuidCountOutput, container->uuids, room.children_count);
    if (result == XR_SUCCESS && container->uuidCapacityInput > 0)
    {
        for (u32 i = 0; i < room.children_count; i++)
        {
            container->uuids[i] = uuid_to_xr(w_entity(world, w_room_indices(world)[room.children_first + i]).uuid);
        }
    }
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceBoundingBox2DFB (XrSession session, XrSpace space, XrRect2Df* box)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_GET_CONST_SESSION_SPACE(target, session, space);
    OXR_CHECK(box);
    if (!component_enabled(target, OXR_Component::Bounded2D))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
    }
    if (target->kind == OXR_SpaceKind::Prop)
    {
        const SIM_Prop* prop = find_prop(runtime()->sim, target->prop);
        if (!prop)
        {
            return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
        }
        const Vec3 extent = tracked_prop_extent(*prop);
        *box              = { .offset = { -0.5f * extent.x, -0.5f * extent.y }, .extent = { extent.x, extent.y } };
        return XR_SUCCESS;
    }
    const W_Entity& entity = w_entity(runtime()->world, target->entity);
    *box = { .offset = { entity.plane_min.x, entity.plane_min.y }, .extent = { entity.plane_max.x - entity.plane_min.x, entity.plane_max.y - entity.plane_min.y } };
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceBoundingBox3DFB (XrSession session, XrSpace space, XrRect3DfFB* box)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_GET_CONST_SESSION_SPACE(target, session, space);
    OXR_CHECK(box);
    if (!component_enabled(target, OXR_Component::Bounded3D))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
    }
    if (target->kind == OXR_SpaceKind::Prop)
    {
        const SIM_Prop* prop = find_prop(runtime()->sim, target->prop);
        if (!prop)
        {
            return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
        }
        const Vec3 extent = tracked_prop_extent(*prop);
        *box              = { .offset = { -0.5f * extent.x, -0.5f * extent.y, -0.5f * extent.z }, .extent = { extent.x, extent.y, extent.z } };
        return XR_SUCCESS;
    }
    const W_Entity& entity = w_entity(runtime()->world, target->entity);
    const Vec3      extent = entity.volume_max - entity.volume_min;
    *box = { .offset = { entity.volume_min.x, entity.volume_min.y, entity.volume_min.z }, .extent = { extent.x, extent.y, extent.z } };
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceSemanticLabelsFB (XrSession session, XrSpace space, XrSemanticLabelsFB* labels)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_GET_CONST_SESSION_SPACE(target, session, space);
    OXR_CHECK_TYPE(labels, XR_TYPE_SEMANTIC_LABELS_FB);
    const XrSemanticLabelsSupportInfoFB* support = nullptr;
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)labels->next; next; next = next->next)
    {
        if (next->type == XR_TYPE_SEMANTIC_LABELS_SUPPORT_INFO_FB)
        {
            support = (const XrSemanticLabelsSupportInfoFB*)next;
            OXR_CHECK(support->recognizedLabels);
        }
    }
    if (!component_enabled(target, OXR_Component::SemanticLabels))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
    }
    const char*    label  = scene_label(w_entity(runtime()->world, target->entity).label, support);
    // Meta's runtime counts the characters without the terminator, and Meta's code builds its
    // strings from the count (MRUK, the XrSceneModel sample): with it no label matched
    const u32      length = (u32)cstr_length(label);
    const XrResult result = oxr_two_call(labels->bufferCapacityInput, &labels->bufferCountOutput, labels->buffer, length);
    if (result == XR_SUCCESS && labels->bufferCapacityInput > 0)
    {
        memory_copy(labels->buffer, label, length);
        if (labels->bufferCapacityInput > length)
        {
            labels->buffer[length] = 0;
        }
    }
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceBoundary2DFB (XrSession session, XrSpace space, XrBoundary2DFB* boundary)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_GET_CONST_SESSION_SPACE(target, session, space);
    OXR_CHECK_TYPE(boundary, XR_TYPE_BOUNDARY_2D_FB);
    if (!component_enabled(target, OXR_Component::Bounded2D))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
    }
    const W_World*  world  = runtime()->world;
    const W_Entity& entity = w_entity(world, target->entity);
    const Vec2      rect[4] = { entity.plane_min, { entity.plane_max.x, entity.plane_min.y }, entity.plane_max, { entity.plane_min.x, entity.plane_max.y } };
    const Vec2*     points  = entity.boundary_count > 0 ? w_boundary_points(world) + entity.boundary_first : rect;
    const u32       count   = entity.boundary_count > 0 ? entity.boundary_count : 4;
    const XrResult  result  = oxr_two_call(boundary->vertexCapacityInput, &boundary->vertexCountOutput, boundary->vertices, count);
    if (result == XR_SUCCESS && boundary->vertexCapacityInput > 0)
    {
        for (u32 i = 0; i < count; i++)
        {
            boundary->vertices[i] = { points[i].x, points[i].y };
        }
    }
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceRoomLayoutFB (XrSession session, XrSpace space, XrRoomLayoutFB* layout)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_GET_CONST_SESSION_SPACE(target, session, space);
    OXR_CHECK_TYPE(layout, XR_TYPE_ROOM_LAYOUT_FB);
    if (!component_enabled(target, OXR_Component::RoomLayout))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
    }
    const W_World* world = runtime()->world;
    const W_Room&  room  = w_room(world, w_entity(world, target->entity).room);
    layout->floorUuid    = uuid_to_xr(w_entity(world, room.floor).uuid);
    layout->ceilingUuid  = uuid_to_xr(w_entity(world, room.ceiling).uuid);
    const XrResult result = oxr_two_call(layout->wallUuidCapacityInput, &layout->wallUuidCountOutput, layout->wallUuids, room.walls_count);
    if (result == XR_SUCCESS && layout->wallUuidCapacityInput > 0)
    {
        for (u32 i = 0; i < room.walls_count; i++)
        {
            layout->wallUuids[i] = uuid_to_xr(w_entity(world, w_room_indices(world)[room.walls_first + i]).uuid);
        }
    }
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrRequestSceneCaptureFB (XrSession session, const XrSceneCaptureRequestInfoFB* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SCENE_CAPTURE_REQUEST_INFO_FB);
    OXR_CHECK(request_id);
    OXR_CHECK(info->requestByteCount == 0 || info->request);

    // The simulated room is always captured already, so the capture flow finishes at once
    OXR_Instance* inst = session_instance(sess);
    *request_id        = next_request_id(inst);
    push_event(inst, { .type = XR_TYPE_EVENT_DATA_SCENE_CAPTURE_COMPLETE_FB, .session = session, .request_id = *request_id, .result = XR_SUCCESS });
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceTriangleMeshMETA (XrSpace space, const XrSpaceTriangleMeshGetInfoMETA* info, XrSpaceTriangleMeshMETA* mesh)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SPACE(target, space);
    OXR_CHECK(!info || info->type == XR_TYPE_SPACE_TRIANGLE_MESH_GET_INFO_META);
    OXR_CHECK_TYPE(mesh, XR_TYPE_SPACE_TRIANGLE_MESH_META);
    if (!component_enabled(target, OXR_Component::TriangleMesh))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
    }

    const W_World* world           = runtime()->world;
    const Vec3*    positions       = nullptr;
    const u32*     indices         = nullptr;
    u32            positions_count = 0;
    u32            indices_count   = 0;
    w_global_mesh(world, positions, positions_count, indices, indices_count);
    mesh->vertexCountOutput = positions_count;
    mesh->indexCountOutput  = indices_count;
    if (mesh->vertexCapacityInput == 0 && mesh->indexCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    OXR_CHECK((mesh->vertexCapacityInput == 0 || mesh->vertices) && (mesh->indexCapacityInput == 0 || mesh->indices));
    if (mesh->vertexCapacityInput < positions_count || mesh->indexCapacityInput < indices_count)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    const Pose to_entity = zm::pose_inverse(w_entity(world, target->entity).pose);
    for (u32 i = 0; i < positions_count; i++)
    {
        mesh->vertices[i] = vec3_to_xr(zm::transform_point(to_entity, positions[i]));
    }
    memory_copy(mesh->indices, indices, indices_count * sizeof(u32));
    return XR_SUCCESS;
}

// XR_META_spatial_entity_room_mesh

static XrSemanticLabelMETA room_mesh_label (W_Label label)
{
    switch (label)
    {
    case W_Label::Floor:
        return XR_SEMANTIC_LABEL_FLOOR_META;
    case W_Label::Ceiling:
        return XR_SEMANTIC_LABEL_CEILING_META;
    case W_Label::WallFace:
        return XR_SEMANTIC_LABEL_WALL_FACE_META;
    case W_Label::InnerWallFace:
        return XR_SEMANTIC_LABEL_INNER_WALL_FACE_META;
    case W_Label::InvisibleWallFace:
        return XR_SEMANTIC_LABEL_INVISIBLE_WALL_FACE_META;
    case W_Label::DoorFrame:
        return XR_SEMANTIC_LABEL_DOOR_FRAME_META;
    case W_Label::WindowFrame:
        return XR_SEMANTIC_LABEL_WINDOW_FRAME_META;
    default:
        return XR_SEMANTIC_LABEL_UNKNOWN_META;
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceRoomMeshMETA (XrSpace space, const XrSpaceRoomMeshGetInfoMETA* info, XrRoomMeshMETA* mesh)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SPACE(target, space);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACE_ROOM_MESH_GET_INFO_META);
    OXR_CHECK(info->recognizedSemanticLabelCount == 0 || info->recognizedSemanticLabels);
    OXR_CHECK_TYPE(mesh, XR_TYPE_ROOM_MESH_META);
    if (!component_enabled(target, OXR_Component::RoomMesh))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
    }

    const W_World*        world = runtime()->world;
    const W_Entity&       room_entity = w_entity(world, target->entity);
    const W_Room&         room  = w_room(world, room_entity.room);
    const Vec3*           positions = w_room_mesh_positions(world) + room.mesh_positions_first;
    const W_RoomMeshFace* faces     = w_room_mesh_faces(world) + room.mesh_faces_first;
    mesh->vertexCountOutput = room.mesh_positions_count;
    mesh->faceCountOutput   = room.mesh_faces_count;
    if (mesh->vertexCapacityInput == 0 && mesh->faceCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    OXR_CHECK((mesh->vertexCapacityInput == 0 || mesh->vertices) && (mesh->faceCapacityInput == 0 || mesh->faces));
    if (mesh->vertexCapacityInput < room.mesh_positions_count || mesh->faceCapacityInput < room.mesh_faces_count)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    // Spec: vertices are in the space's frame, and labels outside the recognized list are unknown
    const Pose to_entity = zm::pose_inverse(room_entity.pose);
    for (u32 i = 0; i < room.mesh_positions_count; i++)
    {
        mesh->vertices[i] = vec3_to_xr(zm::transform_point(to_entity, positions[i]));
    }
    for (u32 i = 0; i < room.mesh_faces_count; i++)
    {
        const XrSemanticLabelMETA label      = room_mesh_label(faces[i].label);
        b8                        recognized = false;
        for (u32 j = 0; j < info->recognizedSemanticLabelCount; j++)
        {
            recognized = recognized || info->recognizedSemanticLabels[j] == label;
        }
        mesh->faces[i] = {
            .uuid          = uuid_to_xr(faces[i].uuid),
            .parentUuid    = uuid_to_xr(faces[i].parent),
            .semanticLabel = recognized ? label : XR_SEMANTIC_LABEL_UNKNOWN_META,
        };
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceRoomMeshFaceIndicesMETA (XrSpace space, const XrUuid* face_uuid, XrRoomMeshFaceIndicesMETA* indices)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SPACE(target, space);
    OXR_CHECK(face_uuid);
    OXR_CHECK_TYPE(indices, XR_TYPE_ROOM_MESH_FACE_INDICES_META);
    if (!component_enabled(target, OXR_Component::RoomMesh))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
    }

    const W_World*        world = runtime()->world;
    const W_Room&         room  = w_room(world, w_entity(world, target->entity).room);
    const W_RoomMeshFace* faces = w_room_mesh_faces(world) + room.mesh_faces_first;
    const W_Uuid          uuid  = uuid_from_xr(*face_uuid);
    for (u32 i = 0; i < room.mesh_faces_count; i++)
    {
        if (!w_uuid_equal(faces[i].uuid, uuid))
        {
            continue;
        }
        const XrResult result = oxr_two_call(indices->indexCapacityInput, &indices->indexCountOutput, indices->indices, faces[i].indices_count);
        if (result == XR_SUCCESS && indices->indexCapacityInput > 0)
        {
            memory_copy(indices->indices, w_room_mesh_indices(world) + faces[i].indices_first, faces[i].indices_count * sizeof(u32));
        }
        return result;
    }
    // No face of this room has the uuid; the spec names no error but validation failure fits
    OXR_FAIL("no face of the room mesh has this uuid");
}


// Anchor storage: XR_FB_spatial_entity_storage, _storage_batch, XR_META_spatial_entity_persistence

static W_StoredAnchor stored_anchor_of (const OXR_Runtime* rt, const OXR_Space* space)
{
    W_StoredAnchor anchor = { .uuid = space->uuid, .room_hash = rt->world ? w_content_hash(rt->world) : 0, .pose = zm::pose_mul(rt->room_from_stage, space->anchor_pose) };
    cstr_copy(anchor.room, sizeof(anchor.room), rt->room);
    return anchor;
}

// A space the app may store: an anchor of this session with the storable component enabled
static XrResult storable_space (XrSession session, XrSpace handle, const OXR_Space*& out_space)
{
    out_space = oxr_find_space(handle);
    if (!out_space)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (out_space->session != session_handle(session))
    {
        OXR_FAIL("a space of another session");
    }
    return component_enabled(out_space, OXR_Component::Storable) ? XR_SUCCESS : XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
}

static b8 location_valid (XrSpaceStorageLocationFB location)
{
    return location == XR_SPACE_STORAGE_LOCATION_LOCAL_FB || location == XR_SPACE_STORAGE_LOCATION_CLOUD_FB;
}

// Uploads an anchor to the simulated cloud, merging recipients with an earlier upload
static XrResult cloud_put (const OXR_Runtime* rt, const W_StoredAnchor& anchor, const u64* users, u32 users_count, const W_Uuid* groups, u32 groups_count)
{
    if (!storage_writable(rt))
    {
        return XR_SUCCESS;
    }
    W_CloudAnchor entry = {};
    defer { w_cloud_release(entry); };
    if (!w_cloud_load(rt->cloud_anchors_dir, anchor.uuid, entry))
    {
        entry.owner = rt->user_id;
    }
    entry.anchor = anchor;
    for (u32 i = 0; i < users_count; i++)
    {
        if (!w_cloud_visible(entry, users[i]))
        {
            entry.users.add(users[i]);
        }
    }
    for (u32 i = 0; i < groups_count; i++)
    {
        if (!w_cloud_in_group(entry, groups[i]))
        {
            entry.groups.add(groups[i]);
        }
    }
    return w_cloud_save(rt->cloud_anchors_dir, entry) ? XR_SUCCESS : XR_ERROR_SPACE_NETWORK_REQUEST_FAILED_FB;
}

// The owner deletes a cloud anchor; anyone else only stops seeing it
static void cloud_remove (const OXR_Runtime* rt, const W_Uuid& uuid)
{
    W_CloudAnchor entry = {};
    defer { w_cloud_release(entry); };
    if (!storage_writable(rt) || !w_cloud_load(rt->cloud_anchors_dir, uuid, entry))
    {
        return;
    }
    if (entry.owner == rt->user_id)
    {
        w_cloud_erase(rt->cloud_anchors_dir, uuid);
        return;
    }
    for (usize i = entry.users.length; i-- > 0;)
    {
        if (entry.users[i] == rt->user_id)
        {
            entry.users.remove(i);
        }
    }
    w_cloud_save(rt->cloud_anchors_dir, entry);
}

static XrResult save_anchors (OXR_Runtime* rt, const W_StoredAnchor* anchors, u32 count, b8 cloud)
{
    const XrResult fault = take_fault(rt->anchor_faults[(u32)OXR_AnchorOp::Save]);
    if (XR_FAILED(fault))
    {
        return fault;
    }
    for (u32 i = 0; i < count; i++)
    {
        const W_StoredAnchor& anchor = anchors[i];
        if (cloud)
        {
            const XrResult result = cloud_put(rt, anchor, nullptr, 0, nullptr, 0);
            if (XR_FAILED(result))
            {
                return result;
            }
            continue;
        }
        w_store_put(rt->store, anchor);
    }
    return cloud || commit_store(rt) ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}

static XrResult erase_anchors (OXR_Runtime* rt, const W_Uuid* uuids, u32 count, b8 cloud)
{
    const XrResult fault = take_fault(rt->anchor_faults[(u32)OXR_AnchorOp::Erase]);
    if (XR_FAILED(fault))
    {
        return fault;
    }
    for (u32 i = 0; i < count; i++)
    {
        if (cloud)
        {
            cloud_remove(rt, uuids[i]);
        }
        else
        {
            w_store_erase(rt->store, uuids[i]);
        }
    }
    return cloud || commit_store(rt) ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSaveSpaceFB (XrSession session, const XrSpaceSaveInfoFB* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACE_SAVE_INFO_FB);
    OXR_CHECK(request_id);
    OXR_CHECK(location_valid(info->location) && info->persistenceMode == XR_SPACE_PERSISTENCE_MODE_INDEFINITE_FB);
    const OXR_Space* space  = nullptr;
    const XrResult   usable = storable_space(session, info->space, space);
    if (XR_FAILED(usable))
    {
        return usable;
    }

    OXR_Runtime*         rt     = runtime();
    OXR_Instance*        inst   = session_instance(sess);
    const W_StoredAnchor anchor = stored_anchor_of(rt, space);
    const XrResult       result = save_anchors(rt, &anchor, 1, info->location == XR_SPACE_STORAGE_LOCATION_CLOUD_FB);
    *request_id           = next_request_id(inst);
    push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_SPACE_SAVE_COMPLETE_FB, .session = session, .request_id = *request_id, .result = result, .space = info->space, .uuid = space->uuid, .location = (u32)info->location });
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEraseSpaceFB (XrSession session, const XrSpaceEraseInfoFB* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACE_ERASE_INFO_FB);
    OXR_CHECK(request_id);
    OXR_CHECK(location_valid(info->location));
    const OXR_Space* space  = nullptr;
    const XrResult   usable = storable_space(session, info->space, space);
    if (XR_FAILED(usable))
    {
        return usable;
    }

    OXR_Instance*  inst   = session_instance(sess);
    const XrResult result = erase_anchors(runtime(), &space->uuid, 1, info->location == XR_SPACE_STORAGE_LOCATION_CLOUD_FB);
    *request_id           = next_request_id(inst);
    push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_SPACE_ERASE_COMPLETE_FB, .session = session, .request_id = *request_id, .result = result, .space = info->space, .uuid = space->uuid, .location = (u32)info->location });
    return XR_SUCCESS;
}

// Every space of a list request must be storable before anything is stored
static XrResult storable_spaces (XrSession session, const XrSpace* handles, u32 count, Array<W_StoredAnchor>& out_anchors)
{
    OXR_CHECK(count > 0 && handles);
    for (u32 i = 0; i < count; i++)
    {
        const OXR_Space* space  = nullptr;
        const XrResult   usable = storable_space(session, handles[i], space);
        if (XR_FAILED(usable))
        {
            return usable;
        }
        out_anchors.add(stored_anchor_of(runtime(), space));
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSaveSpaceListFB (XrSession session, const XrSpaceListSaveInfoFB* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACE_LIST_SAVE_INFO_FB);
    OXR_CHECK(request_id);
    OXR_CHECK(location_valid(info->location));
    Array<W_StoredAnchor> stored = {};
    defer { stored.release(); };
    const XrResult usable = storable_spaces(session, info->spaces, info->spaceCount, stored);
    if (XR_FAILED(usable))
    {
        return usable;
    }

    OXR_Instance*  inst   = session_instance(sess);
    const XrResult result = save_anchors(runtime(), stored.data, (u32)stored.length, info->location == XR_SPACE_STORAGE_LOCATION_CLOUD_FB);
    *request_id           = next_request_id(inst);
    push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_SPACE_LIST_SAVE_COMPLETE_FB, .session = session, .request_id = *request_id, .result = result });
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSaveSpacesMETA (XrSession session, const XrSpacesSaveInfoMETA* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACES_SAVE_INFO_META);
    OXR_CHECK(request_id);
    Array<W_StoredAnchor> stored = {};
    defer { stored.release(); };
    const XrResult usable = storable_spaces(session, info->spaces, info->spaceCount, stored);
    if (XR_FAILED(usable))
    {
        return usable;
    }
    if (!sess->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    OXR_Instance*  inst   = session_instance(sess);
    const XrResult result = save_anchors(runtime(), stored.data, (u32)stored.length, false);
    *request_id           = next_request_id(inst);
    push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_SPACES_SAVE_RESULT_META, .session = session, .request_id = *request_id, .result = result });
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEraseSpacesMETA (XrSession session, const XrSpacesEraseInfoMETA* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACES_ERASE_INFO_META);
    OXR_CHECK(request_id);
    OXR_CHECK(info->spaceCount + info->uuidCount > 0);
    OXR_CHECK((info->spaceCount == 0 || info->spaces) && (info->uuidCount == 0 || info->uuids));
    Array<W_Uuid> uuids = {};
    defer { uuids.release(); };
    for (u32 i = 0; i < info->spaceCount; i++)
    {
        const OXR_Space* space  = nullptr;
        const XrResult   usable = storable_space(session, info->spaces[i], space);
        if (XR_FAILED(usable))
        {
            return usable;
        }
        uuids.add(space->uuid);
    }
    for (u32 i = 0; i < info->uuidCount; i++)
    {
        uuids.add(uuid_from_xr(info->uuids[i]));
    }
    if (!sess->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    OXR_Instance*  inst   = session_instance(sess);
    const XrResult result = erase_anchors(runtime(), uuids.data, (u32)uuids.length, false);
    *request_id           = next_request_id(inst);
    push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_SPACES_ERASE_RESULT_META, .session = session, .request_id = *request_id, .result = result });
    return XR_SUCCESS;
}


// Anchor sharing: XR_FB_spatial_entity_user, _sharing, XR_META_spatial_entity_sharing, _group_sharing

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateSpaceUserFB (XrSession session, const XrSpaceUserCreateInfoFB* info, XrSpaceUserFB* user)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACE_USER_CREATE_INFO_FB);
    OXR_CHECK(user);
    *user = space_user_xr(runtime()->space_users.add({ .session = session_handle(session), .user_id = info->userId }));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceUserIdFB (XrSpaceUserFB user, XrSpaceUserIdFB* user_id)
{
    const OXR_Lock       lock(__func__);
    const OXR_SpaceUser* target = oxr_find_space_user(user);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(user_id);
    *user_id = target->user_id;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroySpaceUserFB (XrSpaceUserFB user)
{
    const OXR_Lock lock(__func__);
    if (!oxr_find_space_user(user))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->space_users.remove(space_user_handle(user));
    return XR_SUCCESS;
}

// The anchors of a share request, each of this session with the sharable component enabled
static XrResult sharable_spaces (XrSession session, const XrSpace* handles, u32 count, Array<W_StoredAnchor>& out_anchors)
{
    OXR_CHECK(count > 0 && handles);
    for (u32 i = 0; i < count; i++)
    {
        const OXR_Space* space = oxr_find_space(handles[i]);
        if (!space)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        OXR_CHECK(space->session == session_handle(session));
        if (!component_enabled(space, OXR_Component::Sharable))
        {
            return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
        }
        out_anchors.add(stored_anchor_of(runtime(), space));
    }
    return XR_SUCCESS;
}

static XrResult share_anchors (OXR_Runtime* rt, const Array<W_StoredAnchor>& anchors, const u64* users, u32 users_count, const W_Uuid* groups, u32 groups_count)
{
    const XrResult fault = take_fault(rt->anchor_faults[(u32)OXR_AnchorOp::Share]);
    if (XR_FAILED(fault))
    {
        return fault;
    }
    for (const W_StoredAnchor& anchor : anchors)
    {
        const XrResult result = cloud_put(rt, anchor, users, users_count, groups, groups_count);
        if (XR_FAILED(result))
        {
            return result;
        }
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrShareSpacesFB (XrSession session, const XrSpaceShareInfoFB* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SPACE_SHARE_INFO_FB);
    OXR_CHECK(request_id);
    OXR_CHECK(info->userCount > 0 && info->users);
    Array<W_StoredAnchor> anchors = {};
    Array<u64>            users   = {};
    defer { anchors.release(); };
    defer { users.release(); };
    for (u32 i = 0; i < info->userCount; i++)
    {
        const OXR_SpaceUser* user = oxr_find_space_user(info->users[i]);
        if (!user)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        users.add(user->user_id);
    }
    const XrResult usable = sharable_spaces(session, info->spaces, info->spaceCount, anchors);
    if (XR_FAILED(usable))
    {
        return usable;
    }

    OXR_Instance*  inst   = session_instance(sess);
    const XrResult result = share_anchors(runtime(), anchors, users.data, (u32)users.length, nullptr, 0);
    *request_id           = next_request_id(inst);
    push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_SPACE_SHARE_COMPLETE_FB, .session = session, .request_id = *request_id, .result = result });
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrShareSpacesMETA (XrSession session, const XrShareSpacesInfoMETA* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SHARE_SPACES_INFO_META);
    OXR_CHECK(request_id);
    OXR_CHECK(info->spaceCount <= XR_MAX_SPACES_PER_SHARE_REQUEST_META);
    // Groups are the only recipients defined, and only with XR_META_spatial_entity_group_sharing
    OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(info->recipientInfo && info->recipientInfo->type == XR_TYPE_SHARE_SPACES_RECIPIENT_GROUPS_META && inst->ext.META_spatial_entity_group_sharing);
    const XrShareSpacesRecipientGroupsMETA* recipients = (const XrShareSpacesRecipientGroupsMETA*)info->recipientInfo;
    OXR_CHECK(recipients->groupCount > 0 && recipients->groups);
    Array<W_StoredAnchor> anchors = {};
    Array<W_Uuid>         groups  = {};
    defer { anchors.release(); };
    defer { groups.release(); };
    const XrResult usable = sharable_spaces(session, info->spaces, info->spaceCount, anchors);
    if (XR_FAILED(usable))
    {
        return usable;
    }
    for (u32 i = 0; i < recipients->groupCount; i++)
    {
        groups.add(uuid_from_xr(recipients->groups[i]));
    }

    const XrResult result = share_anchors(runtime(), anchors, nullptr, 0, groups.data, (u32)groups.length);
    *request_id           = next_request_id(inst);
    push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_SHARE_SPACES_COMPLETE_META, .session = session, .request_id = *request_id, .result = result });
    return XR_SUCCESS;
}


// Colocation: XR_META_colocation_discovery

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrStartColocationAdvertisementMETA (XrSession session, const XrColocationAdvertisementStartInfoMETA* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_COLOCATION_ADVERTISEMENT_START_INFO_META);
    OXR_CHECK(request_id);
    OXR_CHECK(info->bufferSize <= XR_MAX_COLOCATION_DISCOVERY_BUFFER_SIZE_META && (info->bufferSize == 0 || info->buffer));

    OXR_Runtime*  rt    = runtime();
    OXR_Instance* inst  = session_instance(sess);
    *request_id         = next_request_id(inst);
    OXR_Event     event = { .type = XR_TYPE_EVENT_DATA_START_COLOCATION_ADVERTISEMENT_COMPLETE_META, .session = session, .request_id = *request_id, .result = XR_SUCCESS };
    if (sess->advertising)
    {
        event.result = XR_COLOCATION_DISCOVERY_ALREADY_ADVERTISING_META;
        event.uuid   = sess->advert_uuid;
        push_anchor_event(inst, event);
        return XR_SUCCESS;
    }

    W_Advert advert = { .uuid = make_anchor_uuid(rt), .owner = rt->user_id, .size = info->bufferSize };
    memory_copy(advert.buffer, info->buffer, info->bufferSize);
    if (storage_writable(rt))
    {
        sess->advert_file = w_advert_start(rt->cloud_adverts_dir, advert);
        if (!sess->advert_file)
        {
            event.result = XR_ERROR_COLOCATION_DISCOVERY_NETWORK_FAILED_META;
            push_anchor_event(inst, event);
            return XR_SUCCESS;
        }
    }
    sess->advertising    = true;
    sess->advert_uuid    = advert.uuid;
    sess->advert_request = *request_id;
    event.uuid           = advert.uuid;
    push_anchor_event(inst, event);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrStopColocationAdvertisementMETA (XrSession session, const XrColocationAdvertisementStopInfoMETA* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_COLOCATION_ADVERTISEMENT_STOP_INFO_META);
    OXR_CHECK(request_id);
    OXR_Instance* inst = session_instance(sess);
    *request_id        = next_request_id(inst);
    push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_STOP_COLOCATION_ADVERTISEMENT_COMPLETE_META, .session = session, .request_id = *request_id, .result = XR_SUCCESS });
    if (sess->advertising)
    {
        stop_advertising(sess);
        push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_COLOCATION_ADVERTISEMENT_COMPLETE_META, .session = session, .request_id = sess->advert_request, .result = XR_SUCCESS });
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrStartColocationDiscoveryMETA (XrSession session, const XrColocationDiscoveryStartInfoMETA* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_COLOCATION_DISCOVERY_START_INFO_META);
    OXR_CHECK(request_id);
    OXR_Instance* inst = session_instance(sess);
    *request_id        = next_request_id(inst);
    if (sess->discovering)
    {
        push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_START_COLOCATION_DISCOVERY_COMPLETE_META, .session = session, .request_id = *request_id, .result = XR_COLOCATION_DISCOVERY_ALREADY_DISCOVERING_META });
        return XR_SUCCESS;
    }
    sess->discovering         = true;
    sess->discovery_request   = *request_id;
    sess->discovery_next_poll = sim_frame_index(runtime()->sim) + 1;
    sess->discovered.clear();
    push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_START_COLOCATION_DISCOVERY_COMPLETE_META, .session = session, .request_id = *request_id, .result = XR_SUCCESS });
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrStopColocationDiscoveryMETA (XrSession session, const XrColocationDiscoveryStopInfoMETA* info, XrAsyncRequestIdFB* request_id)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_COLOCATION_DISCOVERY_STOP_INFO_META);
    OXR_CHECK(request_id);
    OXR_Instance* inst = session_instance(sess);
    *request_id        = next_request_id(inst);
    push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_STOP_COLOCATION_DISCOVERY_COMPLETE_META, .session = session, .request_id = *request_id, .result = XR_SUCCESS });
    if (sess->discovering)
    {
        sess->discovering = false;
        push_anchor_event(inst, { .type = XR_TYPE_EVENT_DATA_COLOCATION_DISCOVERY_COMPLETE_META, .session = session, .request_id = sess->discovery_request, .result = XR_SUCCESS });
    }
    return XR_SUCCESS;
}

// Passthrough: XR_FB_passthrough

static XrResult passthrough_flags_valid (XrPassthroughFlagsFB flags)
{
    return (flags & ~(XrPassthroughFlagsFB)(XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB | XR_PASSTHROUGH_LAYER_DEPTH_BIT_FB)) == 0 ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreatePassthroughFB (XrSession session, const XrPassthroughCreateInfoFB* info, XrPassthroughFB* out_passthrough)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_PASSTHROUGH_CREATE_INFO_FB);
    OXR_CHECK(out_passthrough);
    OXR_CHECK(passthrough_flags_valid(info->flags) == XR_SUCCESS);
    OXR_Runtime* rt = runtime();
    if (rt->passthroughs.is_valid(sess->passthrough))
    {
        return XR_ERROR_FEATURE_ALREADY_CREATED_PASSTHROUGH_FB;
    }
    sess->passthrough = rt->passthroughs.add({ .session = session_handle(session), .running = (info->flags & XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB) != 0 });
    *out_passthrough  = passthrough_xr(sess->passthrough);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyPassthroughFB (XrPassthroughFB passthrough)
{
    const OXR_Lock lock(__func__);
    if (!oxr_find_passthrough(passthrough))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    // Its LUTs are its children
    OXR_Runtime*                rt     = runtime();
    const OXR_PassthroughHandle handle = passthrough_handle(passthrough);
    for (usize i = rt->color_luts.length(); i-- > 0;)
    {
        if (rt->color_luts.data()[i].passthrough == handle)
        {
            destroy_color_lut(rt->color_luts.handle_at(i));
        }
    }
    rt->passthroughs.remove(handle);
    return XR_SUCCESS;
}

// Starting a running feature or pausing a paused one changes nothing and succeeds
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPassthroughStartFB (XrPassthroughFB passthrough)
{
    const OXR_Lock         lock(__func__);
    OXR_Passthrough* feature = oxr_find_passthrough(passthrough);
    if (!feature)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    feature->running = true;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPassthroughPauseFB (XrPassthroughFB passthrough)
{
    const OXR_Lock         lock(__func__);
    OXR_Passthrough* feature = oxr_find_passthrough(passthrough);
    if (!feature)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    feature->running = false;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreatePassthroughLayerFB (XrSession session, const XrPassthroughLayerCreateInfoFB* info, XrPassthroughLayerFB* out_layer)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB);
    OXR_CHECK(out_layer);
    OXR_CHECK(passthrough_flags_valid(info->flags) == XR_SUCCESS);
    const OXR_Instance* inst           = session_instance(sess);
    const b8            keyboard_hands = inst && inst->ext.FB_passthrough_keyboard_hands && (info->purpose == XR_PASSTHROUGH_LAYER_PURPOSE_TRACKED_KEYBOARD_HANDS_FB || info->purpose == XR_PASSTHROUGH_LAYER_PURPOSE_TRACKED_KEYBOARD_MASKED_HANDS_FB);
    OXR_CHECK(info->purpose == XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB || info->purpose == XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB || keyboard_hands);
    const OXR_Passthrough* feature = oxr_find_passthrough(info->passthrough);
    if (!feature)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(feature->session == session_handle(session));

    const b8                   running = (info->flags & XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB) != 0;
    const OXR_PassthroughLayer layer   = {
        .session        = session_handle(session),
        .passthrough    = passthrough_handle(info->passthrough),
        .purpose        = info->purpose,
        .running        = running,
        .resume_pending = running,
        .style          = oxr_default_passthrough_style,
    };
    *out_layer = passthrough_layer_xr(runtime()->passthrough_layers.add(layer));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyPassthroughLayerFB (XrPassthroughLayerFB layer)
{
    const OXR_Lock lock(__func__);
    if (!oxr_find_passthrough_layer(layer))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    destroy_geometry_using({}, passthrough_layer_handle(layer));
    set_layer_color_luts(*oxr_find_passthrough_layer(layer), {}, {});
    runtime()->passthrough_layers.remove(passthrough_layer_handle(layer));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPassthroughLayerPauseFB (XrPassthroughLayerFB layer)
{
    const OXR_Lock              lock(__func__);
    OXR_PassthroughLayer* target = oxr_find_passthrough_layer(layer);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    target->running = false;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPassthroughLayerResumeFB (XrPassthroughLayerFB layer)
{
    const OXR_Lock              lock(__func__);
    OXR_PassthroughLayer* target = oxr_find_passthrough_layer(layer);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    target->running        = true;
    target->resume_pending = true;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPassthroughLayerSetKeyboardHandsIntensityFB (XrPassthroughLayerFB layer, const XrPassthroughKeyboardHandsIntensityFB* intensity)
{
    const OXR_Lock        lock(__func__);
    OXR_PassthroughLayer* target = oxr_find_passthrough_layer(layer);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(intensity, XR_TYPE_PASSTHROUGH_KEYBOARD_HANDS_INTENSITY_FB);
    OXR_CHECK(target->purpose == XR_PASSTHROUGH_LAYER_PURPOSE_TRACKED_KEYBOARD_HANDS_FB || target->purpose == XR_PASSTHROUGH_LAYER_PURPOSE_TRACKED_KEYBOARD_MASKED_HANDS_FB);
    const f32 left  = intensity->leftHandIntensity;
    const f32 right = intensity->rightHandIntensity;
    OXR_CHECK(left >= 0.0f && left <= 1.0f && right >= 0.0f && right <= 1.0f); // false for NaN
    target->keyboard_hands[0] = left;
    target->keyboard_hands[1] = right;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrRequestBoundaryVisibilityMETA (XrSession session, XrBoundaryVisibilityMETA visibility)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK(visibility == XR_BOUNDARY_VISIBILITY_NOT_SUPPRESSED_META || visibility == XR_BOUNDARY_VISIBILITY_SUPPRESSED_META);
    if (visibility == XR_BOUNDARY_VISIBILITY_SUPPRESSED_META && !session_shows_passthrough(session_handle(session)))
    {
        return XR_BOUNDARY_VISIBILITY_SUPPRESSION_NOT_ALLOWED_META;
    }
    if (sess->boundary != visibility)
    {
        sess->boundary     = visibility;
        OXR_Instance* inst = runtime()->instances.try_get(sess->instance);
        if (inst)
        {
            push_event(inst, { .type = XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META, .session = session, .boundary = visibility });
        }
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetPassthroughPreferencesMETA (XrSession session, XrPassthroughPreferencesMETA* preferences)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(preferences, XR_TYPE_PASSTHROUGH_PREFERENCES_META);
    preferences->flags = sim_get_config(runtime()->sim).passthrough_default_active ? XR_PASSTHROUGH_PREFERENCE_DEFAULT_TO_ACTIVE_BIT_META : 0;
    return XR_SUCCESS;
}


static u8 color_channel (f32 value)
{
    return (u8)(zm::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

//
// XR_META_passthrough_color_lut
//

// The app's RGB or RGBA elements as RGBA
static void copy_color_lut (OXR_PassthroughColorLut& lut, const XrPassthroughColorLutDataMETA& data)
{
    const u32 elements = lut.resolution * lut.resolution * lut.resolution;
    const u32 stride   = data.bufferSize / elements;
    for (u32 i = 0; i < elements; i++)
    {
        lut.rgba[i * 4 + 0] = data.buffer[i * stride + 0];
        lut.rgba[i * 4 + 1] = data.buffer[i * stride + 1];
        lut.rgba[i * 4 + 2] = data.buffer[i * stride + 2];
        lut.rgba[i * 4 + 3] = stride == 4 ? data.buffer[i * stride + 3] : 255;
    }
    lut.generation = ++runtime()->color_lut_generation;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreatePassthroughColorLutMETA (XrPassthroughFB passthrough, const XrPassthroughColorLutCreateInfoMETA* info, XrPassthroughColorLutMETA* out_lut)
{
    const OXR_Lock         lock(__func__);
    const OXR_Passthrough* feature = oxr_find_passthrough(passthrough);
    if (!feature)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_PASSTHROUGH_COLOR_LUT_CREATE_INFO_META);
    OXR_CHECK(out_lut);
    OXR_CHECK(info->channels == XR_PASSTHROUGH_COLOR_LUT_CHANNELS_RGB_META || info->channels == XR_PASSTHROUGH_COLOR_LUT_CHANNELS_RGBA_META);
    const u32 n = info->resolution;
    OXR_CHECK(n > 0 && (n & (n - 1)) == 0 && n <= COMP_COLOR_LUT_MAX_RESOLUTION);
    const u32 size = n * n * n * (info->channels == XR_PASSTHROUGH_COLOR_LUT_CHANNELS_RGB_META ? 3u : 4u);
    if (info->data.bufferSize != size)
    {
        return XR_ERROR_PASSTHROUGH_COLOR_LUT_BUFFER_SIZE_MISMATCH_META;
    }
    OXR_CHECK(info->data.buffer);

    OXR_PassthroughColorLut lut = { .session = feature->session, .passthrough = passthrough_handle(passthrough), .resolution = n, .size = size };
    lut.rgba                    = (u8*)os_memory_alloc((usize)n * n * n * 4);
    if (!lut.rgba)
    {
        return XR_ERROR_OUT_OF_MEMORY;
    }
    copy_color_lut(lut, info->data);
    *out_lut = color_lut_xr(runtime()->color_luts.add(lut));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyPassthroughColorLutMETA (XrPassthroughColorLutMETA lut)
{
    const OXR_Lock lock(__func__);
    if (!find_color_lut(lut))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    destroy_color_lut(color_lut_handle(lut));
    return XR_SUCCESS;
}

// Spec: the new data shows on every layer the LUT is applied to
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrUpdatePassthroughColorLutMETA (XrPassthroughColorLutMETA lut, const XrPassthroughColorLutUpdateInfoMETA* info)
{
    const OXR_Lock           lock(__func__);
    OXR_PassthroughColorLut* target = find_color_lut(lut);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_PASSTHROUGH_COLOR_LUT_UPDATE_INFO_META);
    if (info->data.bufferSize != target->size)
    {
        return XR_ERROR_PASSTHROUGH_COLOR_LUT_BUFFER_SIZE_MISMATCH_META;
    }
    OXR_CHECK(info->data.buffer);
    copy_color_lut(*target, info->data);
    const OXR_PassthroughColorLutHandle handle = color_lut_handle(lut);
    for (OXR_PassthroughLayer& layer : runtime()->passthrough_layers)
    {
        for (u32 i = 0; i < 2; i++)
        {
            if (layer.color_luts[i] == handle)
            {
                layer.style.color_lut_generations[i] = target->generation;
            }
        }
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPassthroughLayerSetStyleFB (XrPassthroughLayerFB layer, const XrPassthroughStyleFB* style)
{
    const OXR_Lock              lock(__func__);
    OXR_PassthroughLayer* target = oxr_find_passthrough_layer(layer);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(style, XR_TYPE_PASSTHROUGH_STYLE_FB);
    OXR_CHECK(zm::is_finite(style->textureOpacityFactor) && style->textureOpacityFactor >= 0.0f && style->textureOpacityFactor <= 1.0f);
    const XrColor4f& edge = style->edgeColor;
    OXR_CHECK(zm::is_finite(edge.r) && zm::is_finite(edge.g) && zm::is_finite(edge.b) && zm::is_finite(edge.a));

    COMP_PassthroughStyle result = {
        .opacity    = style->textureOpacityFactor,
        .edge_color = { zm::clamp(edge.r, 0.0f, 1.0f), zm::clamp(edge.g, 0.0f, 1.0f), zm::clamp(edge.b, 0.0f, 1.0f), zm::clamp(edge.a, 0.0f, 1.0f) },
        .contrast   = 1.0f,
        .saturation = 1.0f,
    };
    // Spec XR_FB_passthrough: the color map structures are mutually exclusive
    const OXR_Session*            sess         = runtime()->sessions.try_get(target->session);
    const OXR_Instance*           inst         = sess ? session_instance(sess) : nullptr;
    const b8                      luts_enabled = inst && inst->ext.META_passthrough_color_lut;
    OXR_PassthroughColorLutHandle luts[2]      = {};
    u32                           maps         = 0;
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)style->next; next; next = next->next)
    {
        switch (next->type)
        {
        case XR_TYPE_PASSTHROUGH_COLOR_MAP_LUT_META:
        case XR_TYPE_PASSTHROUGH_COLOR_MAP_INTERPOLATED_LUT_META:
        {
            if (!luts_enabled)
            {
                break;
            }
            const b8                                        interpolated = next->type == XR_TYPE_PASSTHROUGH_COLOR_MAP_INTERPOLATED_LUT_META;
            const XrPassthroughColorMapLutMETA*             single       = (const XrPassthroughColorMapLutMETA*)next;
            const XrPassthroughColorMapInterpolatedLutMETA* pair         = (const XrPassthroughColorMapInterpolatedLutMETA*)next;
            const XrPassthroughColorLutMETA                 given[2]     = { interpolated ? pair->sourceColorLut : single->colorLut, interpolated ? pair->targetColorLut : XR_NULL_HANDLE };
            const f32                                       weight       = interpolated ? pair->weight : single->weight;
            OXR_CHECK(weight >= 0.0f && weight <= 1.0f); // false for NaN
            for (u32 i = 0; i < (interpolated ? 2u : 1u); i++)
            {
                const OXR_PassthroughColorLut* lut = find_color_lut(given[i]);
                if (!lut || lut->session != target->session)
                {
                    return XR_ERROR_HANDLE_INVALID;
                }
                luts[i]                         = color_lut_handle(given[i]);
                result.color_luts[i]            = lut->rgba;
                result.color_lut_generations[i] = lut->generation;
                result.color_lut_resolutions[i] = lut->resolution;
            }
            result.color_map        = interpolated ? COMP_ColorMap::InterpolatedLut : COMP_ColorMap::Lut;
            result.color_lut_weight = weight;
            maps++;
            break;
        }
        case XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_RGBA_FB:
        {
            const XrPassthroughColorMapMonoToRgbaFB* map = (const XrPassthroughColorMapMonoToRgbaFB*)next;
            result.color_map = COMP_ColorMap::MonoToRgba;
            for (u32 i = 0; i < XR_PASSTHROUGH_COLOR_MAP_MONO_SIZE_FB; i++)
            {
                const XrColor4f& c = map->textureColorMap[i];
                OXR_CHECK(zm::is_finite(c.r) && zm::is_finite(c.g) && zm::is_finite(c.b) && zm::is_finite(c.a));
                result.lut[i][0] = color_channel(c.r);
                result.lut[i][1] = color_channel(c.g);
                result.lut[i][2] = color_channel(c.b);
                result.lut[i][3] = color_channel(c.a);
            }
            maps++;
            break;
        }
        case XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_MONO_FB:
        {
            const XrPassthroughColorMapMonoToMonoFB* map = (const XrPassthroughColorMapMonoToMonoFB*)next;
            result.color_map = COMP_ColorMap::MonoToMono;
            for (u32 i = 0; i < XR_PASSTHROUGH_COLOR_MAP_MONO_SIZE_FB; i++)
            {
                result.lut[i][0] = map->textureColorMap[i];
                result.lut[i][1] = map->textureColorMap[i];
                result.lut[i][2] = map->textureColorMap[i];
                result.lut[i][3] = 255;
            }
            maps++;
            break;
        }
        case XR_TYPE_PASSTHROUGH_BRIGHTNESS_CONTRAST_SATURATION_FB:
        {
            const XrPassthroughBrightnessContrastSaturationFB* bcs = (const XrPassthroughBrightnessContrastSaturationFB*)next;
            OXR_CHECK(zm::is_finite(bcs->brightness) && bcs->brightness >= -100.0f && bcs->brightness <= 100.0f);
            OXR_CHECK(zm::is_finite(bcs->contrast) && bcs->contrast >= 0.0f && zm::is_finite(bcs->saturation) && bcs->saturation >= 0.0f);
            result.color_map  = COMP_ColorMap::BrightnessContrastSaturation;
            result.brightness = bcs->brightness;
            result.contrast   = bcs->contrast;
            result.saturation = bcs->saturation;
            maps++;
            break;
        }
        default:
            break;
        }
    }
    OXR_CHECK(maps <= 1);
    set_layer_color_luts(*target, luts[0], luts[1]);
    target->style = result;
    return XR_SUCCESS;
}

// Triangle meshes: XR_FB_triangle_mesh

static OXR_TriangleMesh* find_triangle_mesh (XrTriangleMeshFB mesh)
{
    return runtime()->triangle_meshes.try_get(triangle_mesh_handle(mesh));
}

static b8 mesh_data_valid (const Vec3* vertices, u32 vertices_count, const u32* indices, u32 indices_count)
{
    for (u32 i = 0; i < vertices_count; i++)
    {
        if (!zm::is_finite(vertices[i].x) || !zm::is_finite(vertices[i].y) || !zm::is_finite(vertices[i].z))
        {
            return false;
        }
    }
    for (u32 i = 0; i < indices_count; i++)
    {
        if (indices[i] >= vertices_count)
        {
            return false;
        }
    }
    return true;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateTriangleMeshFB (XrSession session, const XrTriangleMeshCreateInfoFB* info, XrTriangleMeshFB* out_mesh)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_TRIANGLE_MESH_CREATE_INFO_FB);
    OXR_CHECK(out_mesh);
    OXR_CHECK((info->flags & ~(XrTriangleMeshFlagsFB)XR_TRIANGLE_MESH_MUTABLE_BIT_FB) == 0);
    OXR_CHECK(info->windingOrder == XR_WINDING_ORDER_UNKNOWN_FB || info->windingOrder == XR_WINDING_ORDER_CW_FB || info->windingOrder == XR_WINDING_ORDER_CCW_FB);
    OXR_CHECK(info->vertexCount > 0 && info->triangleCount > 0);
    if (info->vertexCount > OXR_MAX_TRIANGLE_MESH_VERTICES || info->triangleCount > OXR_MAX_TRIANGLE_MESH_TRIANGLES)
    {
        return XR_ERROR_LIMIT_REACHED;
    }

    // Spec: a mutable mesh is filled through its buffers later, an immutable one at creation
    const b8 mutable_mesh = (info->flags & XR_TRIANGLE_MESH_MUTABLE_BIT_FB) != 0;
    if (mutable_mesh)
    {
        OXR_CHECK(!info->vertexBuffer && !info->indexBuffer);
    }
    else
    {
        OXR_CHECK(info->vertexBuffer && info->indexBuffer);
        OXR_CHECK(mesh_data_valid((const Vec3*)info->vertexBuffer, info->vertexCount, info->indexBuffer, info->triangleCount * 3));
    }

    OXR_Runtime*                 rt     = runtime();
    const OXR_TriangleMeshHandle handle = rt->triangle_meshes.add({ .session = session_handle(session), .mutable_mesh = mutable_mesh, .max_vertices = info->vertexCount, .max_triangles = info->triangleCount });
    OXR_TriangleMesh&            mesh   = rt->triangle_meshes.get(handle);
    mesh.vertices.resize(info->vertexCount);
    mesh.indices.resize((usize)info->triangleCount * 3);
    mesh.drawn_vertices.resize(info->vertexCount);
    mesh.drawn_indices.resize((usize)info->triangleCount * 3);
    if (!mutable_mesh)
    {
        memory_copy(mesh.vertices.data, info->vertexBuffer, info->vertexCount * sizeof(Vec3));
        memory_copy(mesh.indices.data, info->indexBuffer, (usize)info->triangleCount * 3 * sizeof(u32));
        memory_copy(mesh.drawn_vertices.data, mesh.vertices.data, mesh.vertices.length * sizeof(Vec3));
        memory_copy(mesh.drawn_indices.data, mesh.indices.data, mesh.indices.length * sizeof(u32));
        mesh.vertices_count  = info->vertexCount;
        mesh.triangles_count = info->triangleCount;
        mesh.state           = OXR_MeshState::Ready;
    }
    *out_mesh = triangle_mesh_xr(handle);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyTriangleMeshFB (XrTriangleMeshFB mesh)
{
    const OXR_Lock lock(__func__);
    if (!find_triangle_mesh(mesh))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    destroy_triangle_mesh(triangle_mesh_handle(mesh));
    return XR_SUCCESS;
}

// Spec: the runtime owns the buffers; their address stays the same for the mesh's lifetime
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrTriangleMeshGetVertexBufferFB (XrTriangleMeshFB mesh, XrVector3f** out_buffer)
{
    const OXR_Lock    lock(__func__);
    const OXR_TriangleMesh* target = find_triangle_mesh(mesh);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(out_buffer);
    *out_buffer = (XrVector3f*)target->vertices.data;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrTriangleMeshGetIndexBufferFB (XrTriangleMeshFB mesh, uint32_t** out_buffer)
{
    const OXR_Lock    lock(__func__);
    const OXR_TriangleMesh* target = find_triangle_mesh(mesh);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(out_buffer);
    *out_buffer = target->indices.data;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrTriangleMeshBeginUpdateFB (XrTriangleMeshFB mesh)
{
    const OXR_Lock    lock(__func__);
    OXR_TriangleMesh* target = find_triangle_mesh(mesh);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(target->mutable_mesh);
    if (target->state != OXR_MeshState::Undefined && target->state != OXR_MeshState::Ready)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    target->state = target->state == OXR_MeshState::Undefined ? OXR_MeshState::DefiningTopology : OXR_MeshState::UpdatingMesh;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrTriangleMeshEndUpdateFB (XrTriangleMeshFB mesh, uint32_t vertex_count, uint32_t triangle_count)
{
    const OXR_Lock    lock(__func__);
    OXR_TriangleMesh* target = find_triangle_mesh(mesh);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(target->mutable_mesh);
    if (target->state != OXR_MeshState::DefiningTopology && target->state != OXR_MeshState::UpdatingMesh)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    OXR_CHECK(vertex_count > 0 && vertex_count <= target->max_vertices && triangle_count > 0 && triangle_count <= target->max_triangles);
    OXR_CHECK(mesh_data_valid(target->vertices.data, vertex_count, target->indices.data, triangle_count * 3));
    memory_copy(target->drawn_vertices.data, target->vertices.data, vertex_count * sizeof(Vec3));
    memory_copy(target->drawn_indices.data, target->indices.data, (usize)triangle_count * 3 * sizeof(u32));
    target->vertices_count  = vertex_count;
    target->triangles_count = triangle_count;
    target->state           = OXR_MeshState::Ready;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrTriangleMeshBeginVertexBufferUpdateFB (XrTriangleMeshFB mesh, uint32_t* out_vertex_count)
{
    const OXR_Lock    lock(__func__);
    OXR_TriangleMesh* target = find_triangle_mesh(mesh);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(target->mutable_mesh);
    OXR_CHECK(out_vertex_count);
    if (target->state != OXR_MeshState::Ready)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    *out_vertex_count = target->vertices_count;
    target->state     = OXR_MeshState::UpdatingVertices;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrTriangleMeshEndVertexBufferUpdateFB (XrTriangleMeshFB mesh)
{
    const OXR_Lock    lock(__func__);
    OXR_TriangleMesh* target = find_triangle_mesh(mesh);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(target->mutable_mesh);
    if (target->state != OXR_MeshState::UpdatingVertices)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    OXR_CHECK(mesh_data_valid(target->vertices.data, target->vertices_count, nullptr, 0));
    memory_copy(target->drawn_vertices.data, target->vertices.data, target->vertices_count * sizeof(Vec3));
    target->state = OXR_MeshState::Ready;
    return XR_SUCCESS;
}

// Geometry instances: XR_FB_passthrough with XR_FB_triangle_mesh

static b8 scale_valid (const XrVector3f& scale)
{
    return zm::is_finite(scale.x) && zm::is_finite(scale.y) && zm::is_finite(scale.z);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateGeometryInstanceFB (XrSession session, const XrGeometryInstanceCreateInfoFB* info, XrGeometryInstanceFB* out_instance)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_GEOMETRY_INSTANCE_CREATE_INFO_FB);
    OXR_CHECK(out_instance);
    const OXR_PassthroughLayer* layer = oxr_find_passthrough_layer(info->layer);
    const OXR_TriangleMesh*     mesh  = find_triangle_mesh(info->mesh);
    if (!layer || !mesh)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_GET_CONST_SPACE(base, info->baseSpace);
    const OXR_SessionHandle handle = session_handle(session);
    OXR_CHECK(layer->session == handle && mesh->session == handle && base->session == handle);
    if (!xr_pose_valid(info->pose))
    {
        return XR_ERROR_POSE_INVALID;
    }
    OXR_CHECK(scale_valid(info->scale));
    // The spec leaves other purposes open; only projected layers show geometry
    OXR_CHECK(layer->purpose == XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB);
    const OXR_Instance* inst = session_instance(sess);
    if (!inst || !inst->ext.FB_triangle_mesh)
    {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }

    OXR_Runtime* rt    = runtime();
    u32          count = 0;
    for (const OXR_GeometryInstance& instance : rt->geometry_instances)
    {
        count += instance.session == handle ? 1u : 0u;
    }
    if (count == OXR_MAX_GEOMETRY_INSTANCES)
    {
        return XR_ERROR_LIMIT_REACHED;
    }
    *out_instance = geometry_xr(rt->geometry_instances.add({
        .session    = handle,
        .layer      = passthrough_layer_handle(info->layer),
        .mesh       = triangle_mesh_handle(info->mesh),
        .base_space = decode_handle<OXR_Space>(OXR_HandleType::Space, (u64)(uintptr_t)info->baseSpace),
        .pose       = pose_from_xr(info->pose),
        .scale      = { info->scale.x, info->scale.y, info->scale.z },
    }));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyGeometryInstanceFB (XrGeometryInstanceFB instance)
{
    const OXR_Lock lock(__func__);
    OXR_Runtime*   rt = runtime();
    if (!rt->geometry_instances.is_valid(geometry_handle(instance)))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    rt->geometry_instances.remove(geometry_handle(instance));
    return XR_SUCCESS;
}

// The base space is located at every frame's display time, so instances follow moving spaces
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGeometryInstanceSetTransformFB (XrGeometryInstanceFB instance, const XrGeometryInstanceTransformFB* transformation)
{
    const OXR_Lock        lock(__func__);
    OXR_GeometryInstance* target = runtime()->geometry_instances.try_get(geometry_handle(instance));
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(transformation, XR_TYPE_GEOMETRY_INSTANCE_TRANSFORM_FB);
    OXR_GET_CONST_SPACE(base, transformation->baseSpace);
    OXR_CHECK(base->session == target->session);
    if (locate_time(transformation->time) <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    if (!xr_pose_valid(transformation->pose))
    {
        return XR_ERROR_POSE_INVALID;
    }
    OXR_CHECK(scale_valid(transformation->scale));
    target->base_space = decode_handle<OXR_Space>(OXR_HandleType::Space, (u64)(uintptr_t)transformation->baseSpace);
    target->pose       = pose_from_xr(transformation->pose);
    target->scale      = { transformation->scale.x, transformation->scale.y, transformation->scale.z };
    return XR_SUCCESS;
}

// Environment depth: XR_META_environment_depth

// Infinite projection with y up, the convention apps rebuild from nearZ, farZ and fov:
// depth = 1 - near / distance, and the image's last row looks up
static Mat4 depth_projection (const Fov& fov)
{
    const f32 l = zm::tan(fov.angle_left);
    const f32 r = zm::tan(fov.angle_right);
    const f32 u = zm::tan(fov.angle_up);
    const f32 d = zm::tan(fov.angle_down);
    Mat4      m = {};
    m.m[0][0]   = 2.0f / (r - l);
    m.m[2][0]   = (r + l) / (r - l);
    m.m[1][1]   = 2.0f / (u - d);
    m.m[2][1]   = (u + d) / (u - d);
    m.m[2][2]   = -1.0f;
    m.m[3][2]   = -OXR_DEPTH_NEAR;
    m.m[2][3]   = -1.0f;
    return m;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateEnvironmentDepthProviderMETA (XrSession session, const XrEnvironmentDepthProviderCreateInfoMETA* info, XrEnvironmentDepthProviderMETA* out_provider)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META);
    OXR_CHECK(info->createFlags == 0);
    OXR_CHECK(out_provider);
    OXR_Runtime* rt = runtime();
    if (rt->depth_providers.is_valid(sess->depth_provider))
    {
        return XR_ERROR_LIMIT_REACHED;
    }
    sess->depth_provider = rt->depth_providers.add({ .session = session_handle(session) });
    *out_provider        = depth_provider_xr(sess->depth_provider);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyEnvironmentDepthProviderMETA (XrEnvironmentDepthProviderMETA provider)
{
    const OXR_Lock                 lock(__func__);
    const OXR_DepthProvider* target = oxr_find_depth_provider(provider);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    destroy_depth_swapchain(target->swapchain);
    runtime()->depth_providers.remove(depth_provider_handle(provider));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrStartEnvironmentDepthProviderMETA (XrEnvironmentDepthProviderMETA provider)
{
    const OXR_Lock           lock(__func__);
    OXR_DepthProvider* target = oxr_find_depth_provider(provider);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (target->running)
    {
        return XR_ERROR_UNEXPECTED_STATE_PASSTHROUGH_FB;
    }
    target->running        = true;
    target->acquired_frame = 0;
    target->rendered_time  = 0;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrStopEnvironmentDepthProviderMETA (XrEnvironmentDepthProviderMETA provider)
{
    const OXR_Lock           lock(__func__);
    OXR_DepthProvider* target = oxr_find_depth_provider(provider);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!target->running)
    {
        return XR_ERROR_UNEXPECTED_STATE_PASSTHROUGH_FB;
    }
    target->running = false;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetEnvironmentDepthHandRemovalMETA (XrEnvironmentDepthProviderMETA provider, const XrEnvironmentDepthHandRemovalSetInfoMETA* info)
{
    const OXR_Lock           lock(__func__);
    OXR_DepthProvider* target = oxr_find_depth_provider(provider);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_ENVIRONMENT_DEPTH_HAND_REMOVAL_SET_INFO_META);
    target->hand_removal = info->enabled != XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateEnvironmentDepthSwapchainMETA (XrEnvironmentDepthProviderMETA provider, const XrEnvironmentDepthSwapchainCreateInfoMETA* info, XrEnvironmentDepthSwapchainMETA* out_swapchain)
{
    const OXR_Lock           lock(__func__);
    OXR_DepthProvider* target = oxr_find_depth_provider(provider);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META);
    OXR_CHECK(info->createFlags == 0);
    OXR_CHECK(out_swapchain);
    OXR_Runtime* rt   = runtime();
    const OXR_Session* sess = rt->sessions.try_get(target->session);
    if (rt->depth_swapchains.is_valid(target->swapchain))
    {
        return XR_ERROR_LIMIT_REACHED;
    }
    if (!sess || !sess->compositor)
    {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    COMP_DepthTarget* depth = comp_create_depth_target(sess->compositor, OXR_DEPTH_WIDTH, OXR_DEPTH_HEIGHT, OXR_DEPTH_IMAGES);
    if (!depth)
    {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    target->swapchain = rt->depth_swapchains.add({ .session = target->session, .provider = depth_provider_handle(provider), .target = depth });
    *out_swapchain    = depth_swapchain_xr(target->swapchain);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyEnvironmentDepthSwapchainMETA (XrEnvironmentDepthSwapchainMETA swapchain)
{
    const OXR_Lock lock(__func__);
    if (!oxr_find_depth_swapchain(swapchain))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    destroy_depth_swapchain(depth_swapchain_handle(swapchain));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetEnvironmentDepthSwapchainStateMETA (XrEnvironmentDepthSwapchainMETA swapchain, XrEnvironmentDepthSwapchainStateMETA* state)
{
    const OXR_Lock lock(__func__);
    if (!oxr_find_depth_swapchain(swapchain))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(state, XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META);
    state->width  = OXR_DEPTH_WIDTH;
    state->height = OXR_DEPTH_HEIGHT;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateEnvironmentDepthSwapchainImagesMETA (XrEnvironmentDepthSwapchainMETA swapchain, uint32_t capacity, uint32_t* count_output, XrSwapchainImageBaseHeader* images)
{
    const OXR_Lock            lock(__func__);
    const OXR_DepthSwapchain* target = oxr_find_depth_swapchain(swapchain);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(count_output);
    const XrResult result = oxr_two_call(capacity, count_output, images, OXR_DEPTH_IMAGES);
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    const OXR_Session* sess = runtime()->sessions.try_get(target->session);
    if (!sess || !sess->compositor)
    {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    u64 natives[OXR_DEPTH_IMAGES];
    for (u32 i = 0; i < OXR_DEPTH_IMAGES; i++)
    {
        natives[i] = comp_depth_image_native(sess->compositor, target->target, i);
    }
    return write_swapchain_images(sess, natives, OXR_DEPTH_IMAGES, images);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrAcquireEnvironmentDepthImageMETA (XrEnvironmentDepthProviderMETA provider, const XrEnvironmentDepthImageAcquireInfoMETA* info, XrEnvironmentDepthImageMETA* image)
{
    const OXR_Lock           lock(__func__);
    OXR_DepthProvider* target = oxr_find_depth_provider(provider);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META);
    OXR_CHECK_TYPE(image, XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META);
    OXR_GET_CONST_SPACE(space, info->space);
    const XrTime display_time = locate_time(info->displayTime);
    if (display_time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    OXR_Runtime*              rt        = runtime();
    const OXR_Session*        sess      = rt->sessions.try_get(target->session);
    const OXR_DepthSwapchain* swapchain = rt->depth_swapchains.try_get(target->swapchain);
    OXR_CHECK(space->session == target->session);
    if (!sess || !sess->frame_begun || !target->running)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    if (target->acquired_frame == sess->frames_begun)
    {
        return XR_ERROR_LIMIT_REACHED;
    }
    if (!swapchain || !sess->compositor)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    target->acquired_frame = sess->frames_begun;
    if (target->rendered_time == 0)
    {
        return XR_ENVIRONMENT_DEPTH_NOT_AVAILABLE_META;
    }

    // The newest image xrEndFrame rendered, seen from the eyes at its own display time
    SIM_Sample sample = {};
    sim_sample(rt->sim, target->rendered_time, sample);
    const OXR_Located base = locate_world(space, sample);
    for (u32 v = 0; v < OXR_VIEW_COUNT; v++)
    {
        Pose eye_pose = {};
        Fov  fov      = {};
        sim_sample_view(rt->sim, sample, v, eye_pose, fov);
        image->views[v].fov  = { fov.angle_left, fov.angle_right, fov.angle_up, fov.angle_down };
        image->views[v].pose = pose_to_xr(base.valid ? zm::pose_relative(base.pose, eye_pose) : zm::POSE_IDENTITY);
    }
    image->swapchainIndex = target->rendered_index;
    image->nearZ          = OXR_DEPTH_NEAR;
    image->farZ           = INFINITY;
    return XR_SUCCESS;
}

// In xrEndFrame, the only call besides xrBeginFrame and the swapchain image calls that may use
// the app's queue (XR_KHR_vulkan_enable, Concurrency); apps acquire depth on any thread. The image
// in use by the frame being ended is not the next one, as the images rotate through three.
static void render_depth (const OXR_Session* sess, OXR_SessionHandle session, XrTime display_time, const SIM_Sample& sample)
{
    OXR_Runtime* rt = runtime();
    for (OXR_DepthProvider& provider : rt->depth_providers)
    {
        OXR_DepthSwapchain* swapchain = rt->depth_swapchains.try_get(provider.swapchain);
        if (!(provider.session == session) || !provider.running || !swapchain)
        {
            continue;
        }
        f32 clip_from_room[COMP_MAX_VIEWS][16];
        for (u32 v = 0; v < OXR_VIEW_COUNT; v++)
        {
            Pose eye_pose = {};
            Fov  fov      = {};
            sim_sample_view(rt->sim, sample, v, eye_pose, fov);
            const Mat4 clip = depth_projection(fov) * zm::inverse(zm::mat4_from_pose(zm::pose_mul(rt->room_from_stage, eye_pose)));
            memory_copy(clip_from_room[v], clip.m, sizeof(clip.m));
        }
        const u32 index        = swapchain->next_index;
        swapchain->next_index  = (index + 1) % OXR_DEPTH_IMAGES;
        comp_render_depth(sess->compositor, swapchain->target, index, clip_from_room);
        provider.rendered_index = index;
        provider.rendered_time  = display_time;
    }
}

//
// XR_METAX1_passthrough_camera_data
//
// Two forward cameras below the eyes like Quest 3's, pinhole, delivering RGBA8 at 60 Hz. An image
// is rendered in xrEndFrame, on the thread owning the app's queue, of the room as passthrough
// shows it at the frame's display time, which is its capture time.

static const struct
{
    u64  id;
    u32  position;
    Vec3 offset; // in the head's frame
} oxr_cameras[OXR_CAMERA_COUNT] = {
    { 50, OXR_PASSTHROUGH_CAMERA_POSITION_LEFT_METAX1,  { -0.032f, -0.02f, -0.06f } },
    { 51, OXR_PASSTHROUGH_CAMERA_POSITION_RIGHT_METAX1, { 0.032f, -0.02f, -0.06f } },
};

static XrResult time_to_clock (XrTime time, u64 anchor, i64& out_clock);

// Quest 3's resolutions; an image smaller or of another aspect than the sensor is its centre
// scaled to fit (MRUK's CalcSensorCropRegion)
static const XrExtent2Di oxr_camera_resolutions[] = { { 320, 240 }, { 640, 480 }, { 800, 600 }, { 1280, 960 } };

static i32 camera_index (u64 id)
{
    for (u32 i = 0; i < OXR_CAMERA_COUNT; i++)
    {
        if (oxr_cameras[i].id == id)
        {
            return (i32)i;
        }
    }
    return -1;
}

static OXR_CameraProvider* find_camera_provider (u64 provider)
{
    return runtime()->camera_providers.try_get(camera_provider_handle(provider));
}

static Mat4 camera_clip_from_room (const OXR_CameraProvider& provider, const SIM_Sample& sample)
{
    const f32  pixel = zm::max((f32)provider.width, (f32)provider.height) / (f32)OXR_CAMERA_SENSOR_SIZE;
    const f32  half_x = (f32)provider.width * 0.5f / (OXR_CAMERA_FOCAL * pixel);
    const f32  half_y = (f32)provider.height * 0.5f / (OXR_CAMERA_FOCAL * pixel);
    const Fov  fov    = { -zm::atan(half_x), zm::atan(half_x), zm::atan(half_y), -zm::atan(half_y) };
    const Pose head   = sample.devices[(u32)SIM_Device::Head].pose;
    const Pose camera = zm::pose_mul(runtime()->room_from_stage, zm::pose_mul(head, { .position = oxr_cameras[provider.camera].offset, .orientation = zm::QUAT_IDENTITY }));
    return zm::mat4_projection(fov, OXR_PASSTHROUGH_NEAR, OXR_PASSTHROUGH_FAR) * zm::inverse(zm::mat4_from_pose(camera));
}

// The newest image, 0 before the first
static u32 newest_camera_image (const OXR_CameraProvider& provider)
{
    for (u32 i = 0; i < OXR_CAMERA_IMAGES; i++)
    {
        if (provider.images[i].number != 0 && provider.images[i].number == provider.images_rendered)
        {
            return i + 1;
        }
    }
    return 0;
}

static void render_cameras (const OXR_Session* sess, OXR_SessionHandle session, XrTime display_time, const SIM_Sample& sample)
{
    for (OXR_CameraProvider& provider : runtime()->camera_providers)
    {
        // Due on the frame nearest each camera period; counted from the schedule, not from the
        // frame that rendered, or a period shorter than two frames rendered on every one
        const b8 due = provider.images_rendered == 0 || display_time >= provider.next_capture - sess->frame_interval / 2;
        if (!(provider.session == session) || !provider.running || !due)
        {
            continue;
        }
        // Not the newest and not held by the app
        const u32 newest = newest_camera_image(provider);
        u32       free   = OXR_CAMERA_IMAGES;
        for (u32 i = 0; i < OXR_CAMERA_IMAGES && free == OXR_CAMERA_IMAGES; i++)
        {
            free = i + 1 != newest && provider.images[i].holds == 0 ? i : free;
        }
        if (free == OXR_CAMERA_IMAGES)
        {
            continue;
        }
        OXR_CameraImage& image = provider.images[free];
        const Mat4       clip  = camera_clip_from_room(provider, sample);
        if (!comp_render_camera(sess->compositor, provider.target, &clip.m[0][0], image.rgba, (usize)provider.width * provider.height * 4))
        {
            continue;
        }
        image.number          = ++provider.images_rendered;
        image.time            = display_time;
        // A schedule that fell behind (a pause, dropped frames) starts again from this image
        provider.next_capture = provider.images_rendered == 1 || provider.next_capture < display_time - OXR_CAMERA_FRAME_INTERVAL_NS ? display_time : provider.next_capture;
        provider.next_capture += OXR_CAMERA_FRAME_INTERVAL_NS;
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumeratePassthroughCameraSourcePropertiesMETAX1 (XrSession session, uint32_t capacity, uint32_t* count_output, OXR_PassthroughCameraSourcePropertiesMETAX1* properties)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK(count_output);
    const XrResult result = oxr_two_call(capacity, count_output, properties, OXR_CAMERA_COUNT);
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    for (u32 i = 0; i < OXR_CAMERA_COUNT; i++)
    {
        properties[i].cameraId = oxr_cameras[i].id;
        properties[i].position = oxr_cameras[i].position;
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumeratePassthroughCameraSupportedResolutionsMETAX1 (XrSession session, u64 camera_id, uint32_t capacity, uint32_t* count_output, XrExtent2Di* resolutions)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK(camera_index(camera_id) >= 0);
    OXR_CHECK(count_output);
    const XrResult result = oxr_two_call(capacity, count_output, resolutions, ARRAY_SIZE(oxr_camera_resolutions));
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    memory_copy(resolutions, oxr_camera_resolutions, sizeof(oxr_camera_resolutions));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetPassthroughCameraIntrinsicsMETAX1 (XrSession session, u64 camera_id, OXR_PassthroughCameraIntrinsicsMETAX1* intrinsics)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK(camera_index(camera_id) >= 0);
    OXR_CHECK(intrinsics && (u32)intrinsics->type == OXR_TYPE_PASSTHROUGH_CAMERA_INTRINSICS_METAX1);
    intrinsics->focalLength      = { OXR_CAMERA_FOCAL, OXR_CAMERA_FOCAL };
    intrinsics->principalPoint   = { OXR_CAMERA_SENSOR_SIZE * 0.5f, OXR_CAMERA_SENSOR_SIZE * 0.5f };
    intrinsics->sensorResolution = { OXR_CAMERA_SENSOR_SIZE, OXR_CAMERA_SENSOR_SIZE };
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetPassthroughCameraExtrinsicsMETAX1 (XrSession session, u64 camera_id, OXR_PassthroughCameraExtrinsicsMETAX1* extrinsics)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    const i32 camera = camera_index(camera_id);
    OXR_CHECK(camera >= 0);
    OXR_CHECK(extrinsics && (u32)extrinsics->type == OXR_TYPE_PASSTHROUGH_CAMERA_EXTRINSICS_METAX1);
    // Camera2 turns the sensor frame (y up, z back) half around x into the camera's (y down, z ahead)
    const Vec3 offset            = oxr_cameras[camera].offset;
    extrinsics->pose.orientation = { 1.0f, 0.0f, 0.0f, 0.0f };
    extrinsics->pose.position    = { offset.x, offset.y, offset.z };
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreatePassthroughCameraProviderMETAX1 (XrSession session, const OXR_PassthroughCameraProviderCreateInfoMETAX1* info, u64* out_provider)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK(info && (u32)info->type == OXR_TYPE_PASSTHROUGH_CAMERA_PROVIDER_CREATE_INFO_METAX1);
    OXR_CHECK(out_provider);
    const i32 camera = camera_index(info->cameraId);
    OXR_CHECK(camera >= 0);
    b8 supported = false;
    for (const XrExtent2Di& resolution : oxr_camera_resolutions)
    {
        supported |= resolution.width == info->resolution.width && resolution.height == info->resolution.height;
    }
    OXR_CHECK(supported);
    OXR_Runtime*            rt     = runtime();
    const OXR_SessionHandle handle = session_handle(session);
    u32                     count  = 0;
    for (const OXR_CameraProvider& provider : rt->camera_providers)
    {
        count += provider.session == handle ? 1u : 0u;
    }
    if (count == OXR_MAX_CAMERA_PROVIDERS)
    {
        return XR_ERROR_LIMIT_REACHED;
    }
    if (!sess->compositor)
    {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    OXR_CameraProvider provider = {
        .session = handle,
        .camera  = (u32)camera,
        .width   = (u32)info->resolution.width,
        .height  = (u32)info->resolution.height,
        .target  = comp_create_camera_target(sess->compositor, (u32)info->resolution.width, (u32)info->resolution.height),
    };
    if (!provider.target)
    {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    for (OXR_CameraImage& image : provider.images)
    {
        image.rgba = (u8*)os_memory_alloc((usize)provider.width * provider.height * 4);
    }
    *out_provider = camera_provider_xr(rt->camera_providers.add(provider));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyPassthroughCameraProviderMETAX1 (u64 provider)
{
    const OXR_Lock lock(__func__);
    if (!find_camera_provider(provider))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    destroy_camera_provider(camera_provider_handle(provider));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrStartPassthroughCameraProviderMETAX1 (u64 provider)
{
    const OXR_Lock      lock(__func__);
    OXR_CameraProvider* target = find_camera_provider(provider);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (target->running)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    target->running = true;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrStopPassthroughCameraProviderMETAX1 (u64 provider)
{
    const OXR_Lock      lock(__func__);
    OXR_CameraProvider* target = find_camera_provider(provider);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!target->running)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    target->running = false;
    return XR_SUCCESS;
}

// The newest image again until a newer one is rendered; before the first, image 0, which has no data
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrAcquirePassthroughCameraImageMETAX1 (u64 provider, u64* out_image)
{
    const OXR_Lock      lock(__func__);
    OXR_CameraProvider* target = find_camera_provider(provider);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(out_image);
    if (!target->running)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    const u32 newest = newest_camera_image(*target);
    *out_image       = newest ? target->images[newest - 1].number : 0;
    target->images[newest ? newest - 1 : 0].holds += newest ? 1u : 0u;
    return XR_SUCCESS;
}

static OXR_CameraImage* find_held_image (OXR_CameraProvider& provider, u64 image)
{
    for (OXR_CameraImage& held : provider.images)
    {
        if (held.number == image && held.holds > 0)
        {
            return &held;
        }
    }
    return nullptr;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetPassthroughCameraImageDataMETAX1 (u64 provider, u64 image, OXR_PassthroughCameraImageDataMETAX1* data)
{
    const OXR_Lock      lock(__func__);
    OXR_CameraProvider* target = find_camera_provider(provider);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(data && (u32)data->type == OXR_TYPE_PASSTHROUGH_CAMERA_IMAGE_DATA_METAX1);
    data->dataLength = 0;
    data->data       = nullptr;
    data->timestamp  = 0;
    if (image == 0)
    {
        return XR_SUCCESS;
    }
    const OXR_CameraImage* held = find_held_image(*target, image);
    if (!held)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    // Converted now, at the clock relation of this frame, as the app converts it back right away
    i64 ns = 0;
    if (time_to_clock(held->time, runtime()->clock_anchor_timespec, ns) == XR_SUCCESS)
    {
        data->timestamp = (u64)ns / 1000;
    }
    data->dataLength = target->width * target->height * 4;
    data->data       = held->rgba;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrReleasePassthroughCameraImageMETAX1 (u64 provider, u64 image)
{
    const OXR_Lock      lock(__func__);
    OXR_CameraProvider* target = find_camera_provider(provider);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (image == 0)
    {
        return XR_SUCCESS;
    }
    OXR_CameraImage* held = find_held_image(*target, image);
    if (!held)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    held->holds--;
    return XR_SUCCESS;
}

// Environment raycast: XR_META_environment_raycast

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateEnvironmentRaycasterAsyncMETA (XrSession session, const XrEnvironmentRaycasterCreateInfoMETA* info, XrFutureEXT* out_future)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK(!info || info->type == XR_TYPE_ENVIRONMENT_RAYCASTER_CREATE_INFO_META);
    OXR_CHECK(out_future);
    OXR_Runtime*              rt        = runtime();
    const OXR_RaycasterHandle raycaster = rt->raycasters.add({ .session = session_handle(session) });
    const OXR_FutureHandle    future    = rt->futures.add({ .session = session_handle(session), .kind = OXR_FutureKind::CreateRaycaster, .result = XR_SUCCESS, .payload = raycaster.value });
    *out_future = future_xr(future);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateEnvironmentRaycasterCompleteMETA (XrSession session, XrFutureEXT future, XrEnvironmentRaycasterCreateCompletionMETA* completion)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(completion, XR_TYPE_ENVIRONMENT_RAYCASTER_CREATE_COMPLETION_META);
    const OXR_Future* target = oxr_find_future(future);
    if (!target || target->kind != OXR_FutureKind::CreateRaycaster || target->session != session_handle(session))
    {
        return XR_ERROR_FUTURE_INVALID_EXT;
    }
    completion->futureResult         = target->result;
    completion->environmentRaycaster = raycaster_xr(OXR_RaycasterHandle{ target->payload });
    runtime()->futures.remove(future_handle(future));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyEnvironmentRaycasterMETA (XrEnvironmentRaycasterMETA raycaster)
{
    const OXR_Lock lock(__func__);
    if (!oxr_find_raycaster(raycaster))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->raycasters.remove(raycaster_handle(raycaster));
    return XR_SUCCESS;
}

// The simulated depth cameras see what either eye sees
static b8 point_in_view (const SIM_Sim* sim, const SIM_Sample& sample, Vec3 point)
{
    for (u32 v = 0; v < OXR_VIEW_COUNT; v++)
    {
        Pose eye_pose = {};
        Fov  fov      = {};
        sim_sample_view(sim, sample, v, eye_pose, fov);
        const Vec3 p = zm::transform_point(zm::pose_inverse(eye_pose), point);
        if (p.z >= 0.0f)
        {
            continue;
        }
        const f32 x = p.x / -p.z;
        const f32 y = p.y / -p.z;
        if (x >= zm::tan(fov.angle_left) && x <= zm::tan(fov.angle_right) && y >= zm::tan(fov.angle_down) && y <= zm::tan(fov.angle_up))
        {
            return true;
        }
    }
    return false;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPerformEnvironmentRaycastMETA (XrEnvironmentRaycasterMETA raycaster, const XrEnvironmentRaycastHitGetInfoMETA* info, XrEnvironmentRaycastHitMETA* hit)
{
    const OXR_Lock             lock(__func__);
    const OXR_Raycaster* target = oxr_find_raycaster(raycaster);
    if (!target || !runtime()->sessions.is_valid(target->session))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_ENVIRONMENT_RAYCAST_HIT_GET_INFO_META);
    OXR_CHECK_TYPE(hit, XR_TYPE_ENVIRONMENT_RAYCAST_HIT_META);
    OXR_GET_CONST_SPACE(space, info->baseSpace);
    OXR_CHECK(space->session == target->session);
    const XrTime time = locate_time(info->time);
    if (time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    OXR_CHECK(info->filterCount == 0 || info->filters);
    f32 max_distance = W_RAYCAST_MAX_DISTANCE;
    for (u32 i = 0; i < info->filterCount; i++)
    {
        const XrEnvironmentRaycastFilterDistanceMETA* filter = (const XrEnvironmentRaycastFilterDistanceMETA*)info->filters[i];
        OXR_CHECK(filter && filter->type == XR_TYPE_ENVIRONMENT_RAYCAST_FILTER_DISTANCE_META);
        OXR_CHECK(zm::is_finite(filter->maxDistance) && filter->maxDistance > 0.0f);
        max_distance = zm::min(max_distance, filter->maxDistance);
    }
    const Vec3 origin    = { info->origin.x, info->origin.y, info->origin.z };
    const Vec3 direction = { info->direction.x, info->direction.y, info->direction.z };
    OXR_CHECK(zm::is_finite(origin.x) && zm::is_finite(origin.y) && zm::is_finite(origin.z));
    OXR_CHECK(zm::is_finite(direction.x) && zm::is_finite(direction.y) && zm::is_finite(direction.z) && zm::length(direction) > zm::EPSILON);

    hit->status = XR_ENVIRONMENT_RAYCAST_HIT_STATUS_NO_HIT_META;
    hit->pose   = pose_to_xr(zm::POSE_IDENTITY);

    const OXR_Runtime* rt     = runtime();
    SIM_Sample   sample = {};
    sim_sample(rt->sim, time, sample);
    const OXR_Located base = locate_world(space, sample);
    W_RaycastHit      room_hit = {};
    const Pose        ray_base = zm::pose_mul(rt->room_from_stage, base.pose);
    if (!rt->world || !base.valid || !w_raycast(rt->world, zm::transform_point(ray_base, origin), zm::rotate(ray_base.orientation, direction), max_distance, room_hit))
    {
        return XR_SUCCESS;
    }
    room_hit.point  = zm::transform_point(rt->stage_from_room, room_hit.point);
    room_hit.normal = zm::rotate(rt->stage_from_room.orientation, room_hit.normal);
    if (!point_in_view(rt->sim, sample, room_hit.point))
    {
        hit->status = XR_ENVIRONMENT_RAYCAST_HIT_STATUS_HIT_POINT_OUTSIDE_OF_FOV_META;
        return XR_SUCCESS;
    }

    // Spec: +Z of the orientation is the surface normal, +Y points roughly back along the ray
    const Vec3 world_direction = zm::normalize(zm::rotate(base.pose.orientation, direction));
    const Quat orientation     = zm::quat_look_rotation(-room_hit.normal, -world_direction);
    hit->status = XR_ENVIRONMENT_RAYCAST_HIT_STATUS_HIT_META;
    hit->pose   = pose_to_xr(zm::pose_relative(base.pose, { room_hit.point, orientation }));
    return XR_SUCCESS;
}

// Futures: XR_EXT_future

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPollFutureEXT (XrInstance instance, const XrFuturePollInfoEXT* info, XrFuturePollResultEXT* result)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK_TYPE(info, XR_TYPE_FUTURE_POLL_INFO_EXT);
    OXR_CHECK_TYPE(result, XR_TYPE_FUTURE_POLL_RESULT_EXT);
    if (!oxr_find_future(info->future))
    {
        return XR_ERROR_FUTURE_INVALID_EXT;
    }
    // Simulated operations finish when they start
    result->state = XR_FUTURE_STATE_READY_EXT;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCancelFutureEXT (XrInstance instance, const XrFutureCancelInfoEXT* info)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK_TYPE(info, XR_TYPE_FUTURE_CANCEL_INFO_EXT);
    if (!oxr_find_future(info->future))
    {
        return XR_ERROR_FUTURE_INVALID_EXT;
    }
    destroy_future(future_handle(info->future));
    return XR_SUCCESS;
}

//
// XR_KHR_visibility_mask
//

#define OXR_MASK_POINTS 64 // a multiple of 8, so the rectangle's corners are among the directions

// Spec 12.23: the view is the rectangle of tangents its fov spans on the z = -1 plane. The lens is
// an ellipse of mask_radius half-extents around its center, which cuts the corners off. Each
// direction gives a point where the visible area ends (inner) and where the view ends (outer).
static void mask_boundary (const Fov& fov, f32 radius, XrVector2f* out_inner, XrVector2f* out_outer)
{
    const f32 left    = zm::tan(fov.angle_left);
    const f32 right   = zm::tan(fov.angle_right);
    const f32 down    = zm::tan(fov.angle_down);
    const f32 up      = zm::tan(fov.angle_up);
    const f32 cx      = (left + right) * 0.5f;
    const f32 cy      = (down + up) * 0.5f;
    const f32 half_x  = (right - left) * 0.5f;
    const f32 half_y  = (up - down) * 0.5f;
    for (u32 i = 0; i < OXR_MASK_POINTS; i++)
    {
        const f32 angle = 2.0f * zm::PI * (f32)i / (f32)OXR_MASK_POINTS;
        const f32 c     = zm::cos(angle);
        const f32 sn    = zm::sin(angle);
        const f32 edge  = 1.0f / zm::max(zm::abs(c), zm::abs(sn));
        const f32 reach = zm::min(radius, edge);
        out_inner[i]    = { cx + c * reach * half_x, cy + sn * reach * half_y };
        out_outer[i]    = { cx + c * edge * half_x, cy + sn * edge * half_y };
    }
}

static b8 mask_triangle_has_area (XrVector2f a, XrVector2f b, XrVector2f c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 1e-7f;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetVisibilityMaskKHR (XrSession session, XrViewConfigurationType view_type, uint32_t view_index, XrVisibilityMaskTypeKHR mask_type, XrVisibilityMaskKHR* mask)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(mask, XR_TYPE_VISIBILITY_MASK_KHR);
    OXR_CHECK(mask_type == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR || mask_type == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR || mask_type == XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR);
    if (view_type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    OXR_CHECK(view_index < OXR_VIEW_COUNT);

    XrVector2f vertices[OXR_MASK_POINTS * 2 + 1] = {};
    u32        indices[OXR_MASK_POINTS * 6]      = {};
    u32        vertex_count                      = 0;
    u32        index_count                       = 0;
    const SIM_Sim* sim    = runtime()->sim;
    const f32      radius = lens_mask_radius(runtime());
    Pose           eye    = zm::POSE_IDENTITY;
    Fov            fov    = {};
    sim_get_view(sim, view_index, eye, fov);
    // Without corners to cut the whole view is visible: its rectangle, and a hidden mesh of one
    // triangle without area. The spec's empty meshes are for a view without any mask; Meta's
    // runtime always has one, and Meta's Unreal plugin crashed on them (a zero-sized buffer)
    if (!(radius > 0.0f && radius < 1.41421356f))
    {
        const f32 left  = zm::tan(fov.angle_left);
        const f32 right = zm::tan(fov.angle_right);
        const f32 down  = zm::tan(fov.angle_down);
        const f32 up    = zm::tan(fov.angle_up);
        if (mask_type == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR)
        {
            vertices[0] = vertices[1] = vertices[2] = { left, up };
            vertex_count = 3;
        }
        else
        {
            vertices[0]  = { left, down };
            vertices[1]  = { right, down };
            vertices[2]  = { right, up };
            vertices[3]  = { left, up };
            vertex_count = 4;
        }
        static const u32 rectangle[6] = { 0, 1, 2, 0, 2, 3 };
        index_count = mask_type == XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR ? 4 : mask_type == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR ? 3 : 6;
        for (u32 i = 0; i < index_count; i++)
        {
            indices[i] = mask_type == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR ? rectangle[i] : i;
        }
    }
    else
    {
        XrVector2f inner[OXR_MASK_POINTS];
        XrVector2f outer[OXR_MASK_POINTS];
        mask_boundary(fov, radius, inner, outer);
        switch (mask_type)
        {
        case XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR:
            for (u32 i = 0; i < OXR_MASK_POINTS; i++)
            {
                vertices[vertex_count++] = inner[i];
                indices[index_count++]   = i;
            }
            break;
        case XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR:
            vertices[vertex_count++] = { (inner[0].x + inner[OXR_MASK_POINTS / 2].x) * 0.5f, (inner[OXR_MASK_POINTS / 4].y + inner[OXR_MASK_POINTS * 3 / 4].y) * 0.5f };
            for (u32 i = 0; i < OXR_MASK_POINTS; i++)
            {
                vertices[vertex_count++] = inner[i];
                indices[index_count++]   = 0;
                indices[index_count++]   = 1 + i;
                indices[index_count++]   = 1 + (i + 1) % OXR_MASK_POINTS;
            }
            break;
        default:
            for (u32 i = 0; i < OXR_MASK_POINTS; i++)
            {
                vertices[i]                   = inner[i];
                vertices[OXR_MASK_POINTS + i] = outer[i];
            }
            vertex_count = OXR_MASK_POINTS * 2;
            for (u32 i = 0; i < OXR_MASK_POINTS; i++)
            {
                const u32 next = (i + 1) % OXR_MASK_POINTS;
                if (mask_triangle_has_area(inner[i], outer[i], outer[next]))
                {
                    indices[index_count++] = i;
                    indices[index_count++] = OXR_MASK_POINTS + i;
                    indices[index_count++] = OXR_MASK_POINTS + next;
                }
                if (mask_triangle_has_area(inner[i], outer[next], inner[next]))
                {
                    indices[index_count++] = i;
                    indices[index_count++] = OXR_MASK_POINTS + next;
                    indices[index_count++] = next;
                }
            }
            break;
        }
    }

    // Spec 12.23: a zero capacity in either array makes both a count query
    mask->vertexCountOutput = vertex_count;
    mask->indexCountOutput  = index_count;
    if (mask->vertexCapacityInput == 0 || mask->indexCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (mask->vertexCapacityInput < vertex_count || mask->indexCapacityInput < index_count)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    OXR_CHECK(mask->vertices && mask->indices);
    memory_copy(mask->vertices, vertices, vertex_count * sizeof(XrVector2f));
    memory_copy(mask->indices, indices, index_count * sizeof(u32));
    return XR_SUCCESS;
}

//
// XR_KHR_convert_timespec_time
//

// The simulated clock runs at the rate of the native clocks between frames but can fall behind or
// run ahead of them (fixed step, clamped gaps), so they are related at the last frame: `anchor` is
// the other clock's reading there, in ns
static XrResult clock_to_time (i64 clock, u64 anchor, XrTime* time)
{
    // Times neither clock can hold are invalid, rather than overflowing
    const OXR_Runtime* rt = runtime();
    if (clock < 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    const i64 elapsed = clock - (i64)anchor;
    if (elapsed > INT64_MAX - rt->clock_anchor_time)
    {
        return XR_ERROR_TIME_INVALID;
    }
    const i64 converted = rt->clock_anchor_time + elapsed;
    if (converted <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    *time = converted;
    return XR_SUCCESS;
}

static XrResult time_to_clock (XrTime time, u64 anchor, i64& out_clock)
{
    if (time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    const OXR_Runtime* rt      = runtime();
    const i64          elapsed = time - rt->clock_anchor_time;
    if (elapsed > INT64_MAX - (i64)anchor)
    {
        return XR_ERROR_TIME_INVALID;
    }
    out_clock = (i64)anchor + elapsed;
    return out_clock < 0 ? XR_ERROR_TIME_INVALID : XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrConvertTimespecTimeToTimeKHR (XrInstance instance, const struct timespec* timespec_time, XrTime* time)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK(timespec_time && time && timespec_time->tv_sec >= 0 && timespec_time->tv_nsec >= 0 && timespec_time->tv_nsec < 1000000000);
    if ((i64)timespec_time->tv_sec > (INT64_MAX - (i64)timespec_time->tv_nsec) / 1000000000LL)
    {
        return XR_ERROR_TIME_INVALID;
    }
    return clock_to_time((i64)timespec_time->tv_sec * 1000000000LL + (i64)timespec_time->tv_nsec, runtime()->clock_anchor_timespec, time);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrConvertTimeToTimespecTimeKHR (XrInstance instance, XrTime time, struct timespec* timespec_time)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK(timespec_time);
    i64            ns     = 0;
    const XrResult result = time_to_clock(time, runtime()->clock_anchor_timespec, ns);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    timespec_time->tv_sec  = (time_t)(ns / 1000000000LL);
    timespec_time->tv_nsec = (long)(ns % 1000000000LL);
    return XR_SUCCESS;
}

//
// XR_KHR_win32_convert_performance_counter_time
//
// The counter is a LARGE_INTEGER, a union over one 64-bit count; it is read as that count so
// that no Windows header is needed. The parameters still have its type: apps call through
// PFN_xrConvertWin32PerformanceCounterToTimeKHR, and calling through another type is undefined
// (UBSan's function check reports it)

union _LARGE_INTEGER; // NOLINT(bugprone-reserved-identifier): the Windows type apps call through

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrConvertWin32PerformanceCounterToTimeKHR (XrInstance instance, const _LARGE_INTEGER* performance_counter, XrTime* time)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK(performance_counter && time);
    i64 ticks = 0;
    memory_copy(&ticks, performance_counter, sizeof(ticks));
    if (ticks <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    return clock_to_time((i64)os_native_ticks_to_ns(ticks), runtime()->clock_anchor_monotonic, time);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrConvertTimeToWin32PerformanceCounterKHR (XrInstance instance, XrTime time, _LARGE_INTEGER* performance_counter)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK(performance_counter);
    i64            monotonic = 0;
    const XrResult result    = time_to_clock(time, runtime()->clock_anchor_monotonic, monotonic);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    const i64 ticks = os_ns_to_native_ticks((u64)monotonic);
    memory_copy(performance_counter, &ticks, sizeof(ticks));
    return XR_SUCCESS;
}

//
// XR_EXT_conformance_automation
//

namespace
{

enum class OXR_AutomationValue : u8
{
    Bool = 0,
    Float,
    Vector,
};

} // namespace

// Spec 12.32: once the app drives input, the keyboard and mouse no longer do
static void automation_take_over ()
{
    sim_set_controls_enabled(runtime()->sim, false);
}

static XrResult automation_hand (const OXR_Instance* inst, XrPath top_level, OXR_TopLevel& out_side)
{
    if (!path_string(inst, top_level))
    {
        return XR_ERROR_PATH_INVALID;
    }
    if (!path_top_level(inst, top_level, out_side) || !side_is_hand(out_side))
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }
    return XR_SUCCESS;
}

// The input is looked up in the profile the app activated, else the current one, else the
// hardware's preferred one
static XrResult automation_component (const OXR_Session* sess, XrPath top_level, XrPath input, OXR_TopLevel& out_side, const OXR_ProfileComponent*& out_component)
{
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    const XrResult result = automation_hand(inst, top_level, out_side);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    if (!path_string(inst, input))
    {
        return XR_ERROR_PATH_INVALID;
    }
    const SIM_Controller hardware = sim_get_controller(runtime()->sim);
    SIM_HandSource       sources[2];
    hand_sources(sources);
    OXR_Profile profile = sess->automation_profile != OXR_Profile::None ? sess->automation_profile : sess->profiles[(u32)out_side];
    if (profile == OXR_Profile::None)
    {
        OXR_Profile order[(u32)OXR_Profile::Count];
        profile = hardware_profiles(hardware, sources[(u32)out_side], order) > 0 ? order[0] : OXR_Profile::None;
    }
    OXR_TopLevel input_side = OXR_TopLevel::Count;
    out_component           = resolve_binding(inst, profile, hardware, sources, input, input_side);
    if (!out_component || input_side != out_side)
    {
        LOG_WARNING("Conformance automation: %s is not an input of %s on this hardware", path_string(inst, input), path_string(inst, top_level));
        return XR_ERROR_PATH_UNSUPPORTED;
    }
    return XR_SUCCESS;
}

static XrResult automation_set_input (XrSession session, XrPath top_level, XrPath input, OXR_AutomationValue type, Vec2 value)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK(zm::is_finite(value.x) && zm::is_finite(value.y));
    OXR_TopLevel                side      = OXR_TopLevel::Count;
    const OXR_ProfileComponent* component = nullptr;
    const XrResult              result    = automation_component(sess, top_level, input, side, component);
    if (result != XR_SUCCESS)
    {
        return result;
    }

    // Poses and haptics have no value to set
    if (component->kind == OXR_SourceKind::Pose || component->kind == OXR_SourceKind::Haptic)
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }
    SIM_Sim*         sim     = runtime()->sim;
    const SIM_Device device  = side_device(side);
    Vec2             current = sim_get_input(sim, device, component->input).value;
    switch (component->kind)
    {
    case OXR_SourceKind::Bool:
    case OXR_SourceKind::FloatAsBool:
    case OXR_SourceKind::Float:
        if (type == OXR_AutomationValue::Vector)
        {
            return XR_ERROR_PATH_UNSUPPORTED;
        }
        current = { value.x, 0.0f };
        break;
    case OXR_SourceKind::Vec2:
        if (type != OXR_AutomationValue::Vector)
        {
            return XR_ERROR_PATH_UNSUPPORTED;
        }
        current = value;
        break;
    case OXR_SourceKind::Vec2X:
    case OXR_SourceKind::Vec2Y:
        if (type == OXR_AutomationValue::Vector)
        {
            return XR_ERROR_PATH_UNSUPPORTED;
        }
        (component->kind == OXR_SourceKind::Vec2X ? current.x : current.y) = value.x;
        break;
    default:
        return XR_ERROR_PATH_UNSUPPORTED;
    }
    automation_take_over();
    sim_set_input(sim, device, component->input, { .value = current });
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetInputDeviceActiveEXT (XrSession session, XrPath interaction_profile, XrPath top_level, XrBool32 is_active)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    OXR_TopLevel   side   = OXR_TopLevel::Count;
    const XrResult result = automation_hand(inst, top_level, side);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    if (!path_string(inst, interaction_profile))
    {
        return XR_ERROR_PATH_INVALID;
    }
    OXR_Profile profile = OXR_Profile::None;
    if (!path_profile(inst, interaction_profile, profile))
    {
        LOG_WARNING("Conformance automation: %s is not emulated", path_string(inst, interaction_profile));
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    SIM_Sim*             sim      = runtime()->sim;
    const SIM_Controller hardware = profile_hardware(profile);
    automation_take_over();
    if (profile == OXR_Profile::HandInteraction)
    {
        sim_set_hand_source(sim, side_device(side), SIM_HandSource::Hand);
    }
    else
    {
        sim_set_hand_source(sim, side_device(side), SIM_HandSource::Controller);
        if (sim_get_controller(sim) != hardware)
        {
            sim_set_controller(sim, hardware);
        }
    }
    sess->automation_profile = profile;
    sim_set_device_active(sim, side_device(side), is_active != XR_FALSE);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetInputDeviceStateBoolEXT (XrSession session, XrPath top_level, XrPath input, XrBool32 state)
{
    return automation_set_input(session, top_level, input, OXR_AutomationValue::Bool, { state != XR_FALSE ? 1.0f : 0.0f, 0.0f });
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetInputDeviceStateFloatEXT (XrSession session, XrPath top_level, XrPath input, float state)
{
    return automation_set_input(session, top_level, input, OXR_AutomationValue::Float, { state, 0.0f });
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetInputDeviceStateVector2fEXT (XrSession session, XrPath top_level, XrPath input, XrVector2f state)
{
    return automation_set_input(session, top_level, input, OXR_AutomationValue::Vector, { state.x, state.y });
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetInputDeviceLocationEXT (XrSession session, XrPath top_level, XrPath input, XrSpace space, XrPosef pose)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_GET_CONST_SPACE(base, space);
    OXR_CHECK(base->session == session_handle(session));
    if (!xr_pose_valid(pose))
    {
        return XR_ERROR_POSE_INVALID;
    }
    OXR_TopLevel                side      = OXR_TopLevel::Count;
    const OXR_ProfileComponent* component = nullptr;
    const XrResult              result    = automation_component(sess, top_level, input, side, component);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    if (component->kind != OXR_SourceKind::Pose)
    {
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    SIM_Sim*   sim    = runtime()->sim;
    SIM_Sample sample = {};
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    const OXR_Located located = locate_world(base, sample);
    automation_take_over();
    if (located.valid)
    {
        sim_set_hand_pose(sim, side_device(side), component->pose, zm::pose_mul(located.pose, pose_from_xr(pose)));
    }
    return XR_SUCCESS;
}

//
// Dynamic objects: XR_META_dynamic_object_tracker, XR_META_dynamic_object_keyboard (previews)
//

// Creation and class changes complete at once; the results arrive as events, as on a headset
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateDynamicObjectTrackerMETA (XrSession session, const OXR_DynamicObjectTrackerCreateInfoMETA* info, u64* out_tracker)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK(info && (u32)info->type == OXR_TYPE_DYNAMIC_OBJECT_TRACKER_CREATE_INFO_META);
    OXR_CHECK(out_tracker);
    const OXR_DynamicObjectTracker tracker = { .session = session_handle(session) };
    *out_tracker                           = dynamic_tracker_xr(runtime()->dynamic_trackers.add(tracker));
    // Meta's docs: XR_ERROR_INITIALIZATION_FAILED when the user turned the tracker off in the settings
    const XrResult result = sim_get_config(runtime()->sim).keyboard_tracking ? XR_SUCCESS : XR_ERROR_INITIALIZATION_FAILED;
    push_event(session_instance(sess), { .type = (XrStructureType)OXR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_TRACKER_CREATE_RESULT_META, .session = session, .result = result, .tracker = *out_tracker }); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange): a META type the OpenXR headers lack
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyDynamicObjectTrackerMETA (u64 tracker)
{
    const OXR_Lock lock(__func__);
    if (!runtime()->dynamic_trackers.try_get(dynamic_tracker_handle(tracker)))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->dynamic_trackers.remove(dynamic_tracker_handle(tracker));
    return XR_SUCCESS;
}

// Keyboards are the only class, and only with XR_META_dynamic_object_keyboard
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetDynamicObjectTrackedClassesMETA (u64 tracker_handle, const OXR_DynamicObjectTrackedClassesSetInfoMETA* info)
{
    const OXR_Lock            lock(__func__);
    OXR_DynamicObjectTracker* tracker = runtime()->dynamic_trackers.try_get(dynamic_tracker_handle(tracker_handle));
    if (!tracker)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(info && (u32)info->type == OXR_TYPE_DYNAMIC_OBJECT_TRACKED_CLASSES_SET_INFO_META);
    OXR_CHECK(info->classCount == 0 || info->classes);
    const OXR_Session* sess = runtime()->sessions.try_get(tracker->session);
    OXR_CHECK(sess);
    OXR_Instance* inst      = session_instance(sess);
    b8            keyboards = false;
    for (u32 i = 0; i < info->classCount; i++)
    {
        OXR_CHECK(info->classes[i] == OXR_DYNAMIC_OBJECT_CLASS_KEYBOARD_META && inst->ext.META_dynamic_object_keyboard);
        keyboards = true;
    }
    tracker->keyboards = keyboards;
    push_event(inst, { .type = (XrStructureType)OXR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_SET_TRACKED_CLASSES_RESULT_META, .session = session_xr(tracker->session), .result = XR_SUCCESS, .tracker = tracker_handle }); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange): a META type the OpenXR headers lack
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpaceDynamicObjectDataMETA (XrSpace space, OXR_DynamicObjectDataMETA* data)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SPACE(target, space);
    OXR_CHECK(data && (u32)data->type == OXR_TYPE_DYNAMIC_OBJECT_DATA_META);
    if (!(supported_components(target) & component_bit(OXR_Component::DynamicObjectData)))
    {
        return XR_ERROR_SPACE_COMPONENT_NOT_SUPPORTED_FB;
    }
    data->classType = OXR_DYNAMIC_OBJECT_CLASS_KEYBOARD_META;
    return XR_SUCCESS;
}

//
// Tracked keyboards: XR_FB_keyboard_tracking on keyboard props
//

#define OXR_KEYBOARD_QUERY_FLAGS (XR_KEYBOARD_TRACKING_QUERY_LOCAL_BIT_FB | XR_KEYBOARD_TRACKING_QUERY_REMOTE_BIT_FB)

static const SIM_Prop* find_keyboard_prop (const SIM_Sim* sim, u32 id)
{
    const SIM_Prop* prop = find_prop(sim, id);
    return prop && prop->kind == SIM_PropKind::Keyboard ? prop : nullptr;
}

// Every keyboard is local and connected; the system's keyboard is the oldest keyboard prop
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrQuerySystemTrackedKeyboardFB (XrSession session, const XrKeyboardTrackingQueryFB* info, XrKeyboardTrackingDescriptionFB* out_keyboard)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_KEYBOARD_TRACKING_QUERY_FB);
    OXR_CHECK((info->flags & ~(XrKeyboardTrackingQueryFlagsFB)OXR_KEYBOARD_QUERY_FLAGS) == 0);
    OXR_CHECK(out_keyboard);
    *out_keyboard = {};
    if (!(info->flags & XR_KEYBOARD_TRACKING_QUERY_LOCAL_BIT_FB))
    {
        return XR_SUCCESS;
    }
    const SIM_Prop* props = nullptr;
    const u32       count = sim_props(runtime()->sim, props);
    for (u32 i = 0; i < count; i++)
    {
        if (props[i].kind == SIM_PropKind::Keyboard)
        {
            const Vec3 size                 = sim_prop_extent(props[i]);
            out_keyboard->trackedKeyboardId = props[i].id;
            out_keyboard->size              = { size.x, size.y, size.z };
            out_keyboard->flags             = XR_KEYBOARD_TRACKING_EXISTS_BIT_FB | XR_KEYBOARD_TRACKING_LOCAL_BIT_FB | XR_KEYBOARD_TRACKING_CONNECTED_BIT_FB;
            cstr_copy(out_keyboard->name, sizeof(out_keyboard->name), "puppetxr Keyboard");
            break;
        }
    }
    return XR_SUCCESS;
}

// An id that names no keyboard prop is a validation failure: the spec names no error for it
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateKeyboardSpaceFB (XrSession session, const XrKeyboardSpaceCreateInfoFB* info, XrSpace* out_space)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_KEYBOARD_SPACE_CREATE_INFO_FB);
    OXR_CHECK(out_space);
    OXR_CHECK(info->trackedKeyboardId <= 0xffffffffu && find_keyboard_prop(runtime()->sim, (u32)info->trackedKeyboardId));
    const OXR_Space space = { .session = session_handle(session), .kind = OXR_SpaceKind::TrackedKeyboard, .prop = (u32)info->trackedKeyboardId };
    *out_space            = space_xr(runtime()->spaces.add(space));
    return XR_SUCCESS;
}

//
// Spatial entities: XR_EXT_spatial_entity, XR_EXT_spatial_marker_tracking on QR code props
//

// A QR code is found while either eye sees it, it faces the head and it is no further away than
// this many times its size: a 15 cm code up to 3.75 m
#define OXR_MARKER_RANGE_PER_SIZE 25.0f
#define OXR_MARKER_MAX_TILT       1.2217305f // 70 degrees between its normal and the direction to the head

static b8 spatial_capability_supported (const OXR_Instance* inst, XrSpatialCapabilityEXT capability)
{
    return inst && inst->ext.EXT_spatial_marker_tracking && capability == XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT;
}

static u32 spatial_component_bit (XrSpatialComponentTypeEXT type)
{
    switch (type)
    {
    case XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT:
        return 1u << (u32)OXR_SpatialComponent::Bounded2D;
    case XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT:
        return 1u << (u32)OXR_SpatialComponent::Marker;
    default:
        return 0;
    }
}

// Spec: discovery and update snapshots refuse components the context did not enable; none means all
static XrResult spatial_components (const OXR_SpatialContext& context, u32 count, const XrSpatialComponentTypeEXT* types, u32& out_bits)
{
    out_bits = 0;
    if (count == 0)
    {
        return XR_SUCCESS;
    }
    OXR_CHECK(types);
    for (u32 i = 0; i < count; i++)
    {
        const u32 bit = spatial_component_bit(types[i]);
        if (!(bit & context.components))
        {
            return XR_ERROR_SPATIAL_COMPONENT_NOT_ENABLED_EXT;
        }
        out_bits |= bit;
    }
    return XR_SUCCESS;
}

static XrResult spatial_tracking_filter (const void* chain, XrSpatialEntityTrackingStateEXT& out_state)
{
    out_state = {};
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)chain; next; next = next->next)
    {
        if (next->type == XR_TYPE_SPATIAL_FILTER_TRACKING_STATE_EXT)
        {
            const XrSpatialEntityTrackingStateEXT state = ((const XrSpatialFilterTrackingStateEXT*)next)->trackingState;
            OXR_CHECK(state == XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT || state == XR_SPATIAL_ENTITY_TRACKING_STATE_PAUSED_EXT || state == XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT);
            out_state = state;
        }
    }
    return XR_SUCCESS;
}

static b8 marker_in_view (const SIM_Sim* sim, const SIM_Sample& sample, const SIM_Prop& prop, const Pose& pose)
{
    const SIM_DeviceState& head     = sample.devices[(u32)SIM_Device::Head];
    const Vec3             to_head  = head.pose.position - pose.position;
    const f32              distance = zm::length(to_head);
    if (!head.tracked || distance <= 0.0f || distance > OXR_MARKER_RANGE_PER_SIZE * prop.size)
    {
        return false;
    }
    const Vec3 normal = zm::rotate(pose.orientation, { 0.0f, 0.0f, 1.0f });
    if (zm::dot(normal, to_head) < distance * zm::cos(OXR_MARKER_MAX_TILT))
    {
        return false;
    }
    return point_in_view(sim, sample, pose.position);
}

static b8 marker_known (const OXR_SpatialContext& context, u32 prop)
{
    for (u32 i = 0; i < context.markers_count; i++)
    {
        if (context.markers[i].prop == prop)
        {
            return true;
        }
    }
    return false;
}

static OXR_SpatialMarker* find_marker (OXR_SpatialContext& context, u64 id)
{
    for (u32 i = 0; i < context.markers_count; i++)
    {
        if (context.markers[i].id == id)
        {
            return &context.markers[i];
        }
    }
    return nullptr;
}

// Codes in view the context has no entity for yet; discovery gives them one, the frame loop recommends discovery
static b8 visit_new_markers (OXR_Session* session, OXR_SpatialContext& context, const SIM_Sample& sample, b8 discover)
{
    const SIM_Sim*  sim   = runtime()->sim;
    const SIM_Prop* props = nullptr;
    const u32       count = sim_props(sim, props);
    b8              found = false;
    for (u32 i = 0; i < count && context.markers_count < OXR_MAX_SPATIAL_ENTITIES; i++)
    {
        const SIM_Prop& prop = props[i];
        if (prop.kind != SIM_PropKind::QrCode || marker_known(context, prop.id) || !marker_in_view(sim, sample, prop, sim_prop_pose(prop, sample.time)))
        {
            continue;
        }
        found = true;
        if (!discover)
        {
            break;
        }
        context.markers[context.markers_count++] = { .id = ++session->spatial_entity_ids, .prop = prop.id };
    }
    return found;
}

static b8 utf8_valid (const char* text)
{
    const u8* s = (const u8*)text;
    while (*s)
    {
        u32 continuation = 0;
        if (*s < 0x80)
        {
            continuation = 0;
        }
        else if (*s >= 0xC2 && *s <= 0xDF)
        {
            continuation = 1;
        }
        else if (*s >= 0xE0 && *s <= 0xEF)
        {
            continuation = 2;
        }
        else if (*s >= 0xF0 && *s <= 0xF4)
        {
            continuation = 3;
        }
        else
        {
            return false;
        }
        s++;
        for (u32 i = 0; i < continuation; i++, s++)
        {
            if ((*s & 0xC0) != 0x80)
            {
                return false;
            }
        }
    }
    return true;
}

// Spec: the same buffer id always returns the same data, so ids are indices into texts never changed
static u64 spatial_text_buffer (OXR_Session* session, const char* text)
{
    for (usize i = 0; i < session->spatial_texts.length; i++)
    {
        if (cstr_equal(session->spatial_texts.data[i].text, text))
        {
            return i + 1;
        }
    }
    OXR_SpatialText entry = {};
    cstr_copy(entry.text, sizeof(entry.text), text);
    session->spatial_texts.add(entry);
    return session->spatial_texts.length;
}

// Spec: tracking pauses while a code is out of view and stops for good once it is gone
static OXR_SnapshotEntity snapshot_entity (OXR_Session* session, const OXR_SpatialContext& context, OXR_SpatialMarker& marker, const SIM_Sample& sample, const OXR_Located& base, u32 components)
{
    OXR_SnapshotEntity entity = { .id = marker.id, .state = XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT, .components = components };
    const SIM_Sim*     sim    = runtime()->sim;
    const SIM_Prop*    prop   = find_prop(sim, marker.prop);
    marker.stopped            = marker.stopped || !prop || prop->kind != SIM_PropKind::QrCode;
    if (marker.stopped)
    {
        return entity;
    }
    const Pose pose = sim_prop_pose(*prop, sample.time);
    entity.state    = XR_SPATIAL_ENTITY_TRACKING_STATE_PAUSED_EXT;
    if (!base.valid || !marker_in_view(sim, sample, *prop, pose))
    {
        return entity;
    }
    entity.state       = XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT;
    entity.center      = zm::pose_relative(base.pose, pose);
    entity.extent      = context.marker_size > 0.0f ? context.marker_size : prop->size;
    entity.buffer      = spatial_text_buffer(session, prop->text);
    entity.buffer_type = utf8_valid(prop->text) ? XR_SPATIAL_BUFFER_TYPE_STRING_EXT : XR_SPATIAL_BUFFER_TYPE_UINT8_EXT;
    return entity;
}

static void destroy_spatial_context (OXR_SpatialContextHandle handle)
{
    OXR_Runtime* rt = runtime();
    for (usize i = rt->spatial_entities.length(); i-- > 0;)
    {
        if (rt->spatial_entities.data()[i].context == handle)
        {
            rt->spatial_entities.remove(rt->spatial_entities.handle_at(i));
        }
    }
    for (usize i = rt->spatial_snapshots.length(); i-- > 0;)
    {
        if (rt->spatial_snapshots.data()[i].context == handle)
        {
            rt->spatial_snapshots.remove(rt->spatial_snapshots.handle_at(i));
        }
    }
    for (usize i = rt->futures.length(); i-- > 0;)
    {
        const OXR_Future& future = rt->futures.data()[i];
        if (future.kind == OXR_FutureKind::CreateSpatialSnapshot && future.payload == handle.value)
        {
            rt->futures.remove(rt->futures.handle_at(i));
        }
    }
    rt->spatial_contexts.remove(handle);
}

// Spec: discovery is recommended only once the app holds the context, and not for codes that moved
static void poll_spatial_contexts (OXR_Runtime* rt, OXR_Session* session, OXR_SessionHandle handle)
{
    OXR_Instance* inst = session_instance(session);
    if (!inst || !inst->ext.EXT_spatial_entity)
    {
        return;
    }
    SIM_Sample sample  = {};
    b8         sampled = false;
    for (usize i = 0; i < rt->spatial_contexts.length(); i++)
    {
        OXR_SpatialContext& context = rt->spatial_contexts.data()[i];
        if (context.session != handle || !context.completed || context.recommended)
        {
            continue;
        }
        if (!sampled)
        {
            sim_sample(rt->sim, sim_predicted_display_time(rt->sim), sample);
            sampled = true;
        }
        if (visit_new_markers(session, context, sample, false))
        {
            context.recommended = true;
            push_event(inst, { .type = XR_TYPE_EVENT_DATA_SPATIAL_DISCOVERY_RECOMMENDED_EXT, .spatial_context = spatial_context_xr(rt->spatial_contexts.handle_at(i)) });
        }
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateSpatialCapabilitiesEXT (XrInstance instance, XrSystemId system, u32 capacity, u32* count_output, XrSpatialCapabilityEXT* capabilities)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    if (system != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    const u32      count  = inst->ext.EXT_spatial_marker_tracking ? 1 : 0;
    const XrResult result = oxr_two_call(capacity, count_output, capabilities, count);
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    if (count > 0)
    {
        capabilities[0] = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT;
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateSpatialCapabilityComponentTypesEXT (XrInstance instance, XrSystemId system, XrSpatialCapabilityEXT capability, XrSpatialCapabilityComponentTypesEXT* components)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    if (system != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    OXR_CHECK_TYPE(components, XR_TYPE_SPATIAL_CAPABILITY_COMPONENT_TYPES_EXT);
    if (!spatial_capability_supported(inst, capability))
    {
        return XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT;
    }
    static const XrSpatialComponentTypeEXT types[] = { XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT, XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT };
    const XrResult result = oxr_two_call(components->componentTypeCapacityInput, &components->componentTypeCountOutput, components->componentTypes, ARRAY_SIZE(types));
    if (result != XR_SUCCESS || components->componentTypeCapacityInput == 0)
    {
        return result;
    }
    memory_copy(components->componentTypes, types, sizeof(types));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateSpatialCapabilityFeaturesEXT (XrInstance instance, XrSystemId system, XrSpatialCapabilityEXT capability, u32 capacity, u32* count_output, XrSpatialCapabilityFeatureEXT* features)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    if (system != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (!spatial_capability_supported(inst, capability))
    {
        return XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT;
    }
    static const XrSpatialCapabilityFeatureEXT supported[] = { XR_SPATIAL_CAPABILITY_FEATURE_MARKER_TRACKING_FIXED_SIZE_MARKERS_EXT, XR_SPATIAL_CAPABILITY_FEATURE_MARKER_TRACKING_STATIC_MARKERS_EXT };
    const XrResult result = oxr_two_call(capacity, count_output, features, ARRAY_SIZE(supported));
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    memory_copy(features, supported, sizeof(supported));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateSpatialContextAsyncEXT (XrSession session, const XrSpatialContextCreateInfoEXT* info, XrFutureEXT* out_future)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT);
    OXR_CHECK(out_future);
    if (info->capabilityConfigCount == 0)
    {
        return XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT;
    }
    OXR_CHECK(info->capabilityConfigs);

    const OXR_Instance* inst    = session_instance(sess);
    OXR_SpatialContext  context = { .session = session_handle(session) };
    for (u32 i = 0; i < info->capabilityConfigCount; i++)
    {
        const XrSpatialCapabilityConfigurationBaseHeaderEXT* config = info->capabilityConfigs[i];
        OXR_CHECK(config);
        if (!spatial_capability_supported(inst, config->capability))
        {
            return XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT;
        }
        OXR_CHECK(config->type == XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_QR_CODE_EXT);
        if (config->enabledComponentCount == 0)
        {
            return XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT;
        }
        OXR_CHECK(config->enabledComponents);
        for (u32 j = 0; j < config->enabledComponentCount; j++)
        {
            const u32 bit = spatial_component_bit(config->enabledComponents[j]);
            if (!bit)
            {
                return XR_ERROR_SPATIAL_COMPONENT_UNSUPPORTED_FOR_CAPABILITY_EXT;
            }
            context.components |= bit;
        }
        for (u32 j = 0; j < i; j++)
        {
            if (info->capabilityConfigs[j] && info->capabilityConfigs[j]->capability == config->capability)
            {
                return XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT;
            }
        }
        // Static markers change nothing: simulated codes are found every frame anyway
        for (const XrBaseInStructure* next = (const XrBaseInStructure*)config->next; next; next = next->next)
        {
            if (next->type == XR_TYPE_SPATIAL_MARKER_SIZE_EXT)
            {
                const f32 size = ((const XrSpatialMarkerSizeEXT*)next)->markerSideLength;
                OXR_CHECK(zm::is_finite(size) && size > 0.0f);
                context.marker_size = size;
            }
        }
    }

    OXR_Runtime*                   rt     = runtime();
    const OXR_SpatialContextHandle handle = rt->spatial_contexts.add(context);
    const OXR_FutureHandle         future = rt->futures.add({ .session = session_handle(session), .kind = OXR_FutureKind::CreateSpatialContext, .result = XR_SUCCESS, .payload = handle.value });
    *out_future = future_xr(future);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateSpatialContextCompleteEXT (XrSession session, XrFutureEXT future, XrCreateSpatialContextCompletionEXT* completion)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(completion, XR_TYPE_CREATE_SPATIAL_CONTEXT_COMPLETION_EXT);
    const OXR_Future* target = oxr_find_future(future);
    if (!target || target->kind != OXR_FutureKind::CreateSpatialContext || target->session != session_handle(session))
    {
        return XR_ERROR_FUTURE_INVALID_EXT;
    }
    OXR_Runtime*                   rt      = runtime();
    const OXR_SpatialContextHandle handle{ target->payload };
    OXR_SpatialContext*            context = rt->spatial_contexts.try_get(handle);
    ASSERT(context);
    context->completed         = true;
    completion->futureResult   = XR_SUCCESS;
    completion->spatialContext = (XrSpatialContextEXT)(uintptr_t)spatial_context_xr(handle);
    rt->futures.remove(future_handle(future));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroySpatialContextEXT (XrSpatialContextEXT context)
{
    const OXR_Lock lock(__func__);
    if (!find_spatial_context(context))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    destroy_spatial_context(spatial_context_handle(context));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateSpatialDiscoverySnapshotAsyncEXT (XrSpatialContextEXT context, const XrSpatialDiscoverySnapshotCreateInfoEXT* info, XrFutureEXT* out_future)
{
    const OXR_Lock      lock(__func__);
    const OXR_SpatialContext* target = find_spatial_context(context);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT);
    OXR_CHECK(out_future);
    u32            components = 0;
    XrResult       result     = spatial_components(*target, info->componentTypeCount, info->componentTypes, components);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    XrSpatialEntityTrackingStateEXT filter = {};
    result = spatial_tracking_filter(info->next, filter);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    const OXR_Future future = { .session = target->session, .kind = OXR_FutureKind::CreateSpatialSnapshot, .result = XR_SUCCESS, .payload = spatial_context_handle(context).value, .components = components, .tracking_filter = filter };
    *out_future = future_xr(runtime()->futures.add(future));
    return XR_SUCCESS;
}

// The base space must belong to the context's session and the time be valid
static XrResult spatial_base (const OXR_SpatialContext& context, XrSpace space, XrTime time, OXR_Located& out_base, SIM_Sample& out_sample)
{
    const OXR_Space* base = oxr_find_space(space);
    if (!base)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(base->session == context.session);
    if (time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    sim_sample(runtime()->sim, time, out_sample);
    out_base = locate_world(base, out_sample);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateSpatialDiscoverySnapshotCompleteEXT (XrSpatialContextEXT context, const XrCreateSpatialDiscoverySnapshotCompletionInfoEXT* info, XrCreateSpatialDiscoverySnapshotCompletionEXT* completion)
{
    const OXR_Lock      lock(__func__);
    OXR_Runtime*        rt     = runtime();
    OXR_SpatialContext* target = find_spatial_context(context);
    OXR_Session*        sess   = target ? rt->sessions.try_get(target->session) : nullptr;
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_INFO_EXT);
    OXR_CHECK_TYPE(completion, XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_EXT);
    OXR_Located    base   = {};
    SIM_Sample     sample = {};
    const XrResult result = spatial_base(*target, info->baseSpace, info->time, base, sample);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    const OXR_SpatialContextHandle handle = spatial_context_handle(context);
    const OXR_Future*              future = oxr_find_future(info->future);
    if (!future || future->kind != OXR_FutureKind::CreateSpatialSnapshot || future->payload != handle.value)
    {
        return XR_ERROR_FUTURE_INVALID_EXT;
    }
    const u32                             components = future->components ? future->components : target->components;
    const XrSpatialEntityTrackingStateEXT filter     = future->tracking_filter;
    rt->futures.remove(future_handle(info->future));

    visit_new_markers(sess, *target, sample, true);
    target->recommended = false;
    OXR_SpatialSnapshot snapshot = { .context = handle };
    for (u32 i = 0; i < target->markers_count; i++)
    {
        const OXR_SnapshotEntity entity = snapshot_entity(sess, *target, target->markers[i], sample, base, components);
        if ((u32)filter == 0 || entity.state == filter)
        {
            snapshot.entities[snapshot.entities_count++] = entity;
        }
    }
    completion->futureResult = XR_SUCCESS;
    completion->snapshot     = (XrSpatialSnapshotEXT)(uintptr_t)spatial_snapshot_xr(rt->spatial_snapshots.add(snapshot));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateSpatialUpdateSnapshotEXT (XrSpatialContextEXT context, const XrSpatialUpdateSnapshotCreateInfoEXT* info, XrSpatialSnapshotEXT* out_snapshot)
{
    const OXR_Lock      lock(__func__);
    OXR_Runtime*        rt     = runtime();
    OXR_SpatialContext* target = find_spatial_context(context);
    OXR_Session*        sess   = target ? rt->sessions.try_get(target->session) : nullptr;
    if (!sess)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_SPATIAL_UPDATE_SNAPSHOT_CREATE_INFO_EXT);
    OXR_CHECK(out_snapshot);
    OXR_CHECK(info->entityCount > 0 && info->entities);
    u32      components = 0;
    XrResult result     = spatial_components(*target, info->componentTypeCount, info->componentTypes, components);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    const OXR_SpatialContextHandle handle = spatial_context_handle(context);
    for (u32 i = 0; i < info->entityCount; i++)
    {
        const OXR_SpatialEntity* entity = find_spatial_entity(info->entities[i]);
        if (!entity)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        OXR_CHECK(entity->context == handle);
    }
    OXR_Located base   = {};
    SIM_Sample  sample = {};
    result = spatial_base(*target, info->baseSpace, info->time, base, sample);
    if (result != XR_SUCCESS)
    {
        return result;
    }

    OXR_SpatialSnapshot snapshot = { .context = handle };
    for (u32 i = 0; i < info->entityCount; i++)
    {
        const u64 id        = find_spatial_entity(info->entities[i])->id;
        b8        duplicate = false;
        for (u32 j = 0; j < snapshot.entities_count; j++)
        {
            duplicate = duplicate || snapshot.entities[j].id == id;
        }
        OXR_SpatialMarker* marker = find_marker(*target, id);
        ASSERT(marker);
        if (!duplicate && marker)
        {
            snapshot.entities[snapshot.entities_count++] = snapshot_entity(sess, *target, *marker, sample, base, components ? components : target->components);
        }
    }
    *out_snapshot = (XrSpatialSnapshotEXT)(uintptr_t)spatial_snapshot_xr(rt->spatial_snapshots.add(snapshot));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroySpatialSnapshotEXT (XrSpatialSnapshotEXT snapshot)
{
    const OXR_Lock lock(__func__);
    if (!find_spatial_snapshot(snapshot))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->spatial_snapshots.remove(spatial_snapshot_handle(snapshot));
    return XR_SUCCESS;
}

static b8 condition_lists (const XrSpatialComponentDataQueryConditionEXT* condition, XrSpatialComponentTypeEXT type)
{
    for (u32 i = 0; i < condition->componentTypeCount; i++)
    {
        if (condition->componentTypes[i] == type)
        {
            return true;
        }
    }
    return false;
}

// The component a list struct of the query result holds, or 0 for structs that are no list
static XrSpatialComponentTypeEXT list_component (XrStructureType type)
{
    switch (type)
    {
    case XR_TYPE_SPATIAL_COMPONENT_BOUNDED_2D_LIST_EXT:
        return XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT;
    case XR_TYPE_SPATIAL_COMPONENT_BOUNDED_3D_LIST_EXT:
        return XR_SPATIAL_COMPONENT_TYPE_BOUNDED_3D_EXT;
    case XR_TYPE_SPATIAL_COMPONENT_PARENT_LIST_EXT:
        return XR_SPATIAL_COMPONENT_TYPE_PARENT_EXT;
    case XR_TYPE_SPATIAL_COMPONENT_MESH_3D_LIST_EXT:
        return XR_SPATIAL_COMPONENT_TYPE_MESH_3D_EXT;
    case XR_TYPE_SPATIAL_COMPONENT_MARKER_LIST_EXT:
        return XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT;
    default:
        return XR_SPATIAL_COMPONENT_TYPE_MAX_ENUM_EXT;
    }
}

namespace
{

// Every list struct has its element count right after the header
struct OXR_ComponentList
{
    XrStructureType type;
    void*           next;
    u32             count;
    void*           data;
};

} // namespace

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrQuerySpatialComponentDataEXT (XrSpatialSnapshotEXT snapshot, const XrSpatialComponentDataQueryConditionEXT* condition, XrSpatialComponentDataQueryResultEXT* result)
{
    const OXR_Lock             lock(__func__);
    const OXR_SpatialSnapshot* target = find_spatial_snapshot(snapshot);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(condition, XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT);
    OXR_CHECK_TYPE(result, XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT);
    OXR_CHECK(condition->componentTypeCount == 0 || condition->componentTypes);
    XrSpatialEntityTrackingStateEXT filter = {};
    const XrResult                  status = spatial_tracking_filter(condition->next, filter);
    if (status != XR_SUCCESS)
    {
        return status;
    }

    // Spec: lists only for components the condition names, unless it names none
    for (const XrBaseOutStructure* next = (const XrBaseOutStructure*)result->next; next; next = next->next)
    {
        const XrSpatialComponentTypeEXT component = list_component(next->type);
        OXR_CHECK(component == XR_SPATIAL_COMPONENT_TYPE_MAX_ENUM_EXT || condition->componentTypeCount == 0 || condition_lists(condition, component));
    }

    // Spec: entities with all the condition's components
    u32  wanted  = 0;
    b8   unknown = false;
    for (u32 i = 0; i < condition->componentTypeCount; i++)
    {
        const u32 bit = spatial_component_bit(condition->componentTypes[i]);
        unknown       = unknown || !bit;
        wanted       |= bit;
    }
    u32 matches[OXR_MAX_SPATIAL_ENTITIES];
    u32 count = 0;
    for (u32 i = 0; i < target->entities_count && !unknown; i++)
    {
        const OXR_SnapshotEntity& entity = target->entities[i];
        if ((entity.components & wanted) == wanted && ((u32)filter == 0 || entity.state == filter))
        {
            matches[count++] = i;
        }
    }
    result->entityIdCountOutput    = count;
    result->entityStateCountOutput = count;
    if (result->entityIdCapacityInput == 0 && result->entityStateCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if ((result->entityIdCapacityInput > 0 && result->entityIdCapacityInput < count) || (result->entityStateCapacityInput > 0 && result->entityStateCapacityInput < count))
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    OXR_CHECK(result->entityIdCapacityInput == 0 || result->entityIds);
    OXR_CHECK(result->entityStateCapacityInput == 0 || result->entityStates);
    XrSpatialComponentBounded2DListEXT* bounds  = nullptr;
    XrSpatialComponentMarkerListEXT*    markers = nullptr;
    for (XrBaseOutStructure* next = (XrBaseOutStructure*)result->next; next; next = next->next)
    {
        if (list_component(next->type) == XR_SPATIAL_COMPONENT_TYPE_MAX_ENUM_EXT)
        {
            continue;
        }
        const OXR_ComponentList* list = (const OXR_ComponentList*)next;
        if (list->count < count)
        {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        OXR_CHECK(count == 0 || list->data);
        bounds  = next->type == XR_TYPE_SPATIAL_COMPONENT_BOUNDED_2D_LIST_EXT ? (XrSpatialComponentBounded2DListEXT*)next : bounds;
        markers = next->type == XR_TYPE_SPATIAL_COMPONENT_MARKER_LIST_EXT ? (XrSpatialComponentMarkerListEXT*)next : markers;
    }

    // Spec: data of entities not tracked stays as the app left it
    for (u32 i = 0; i < count; i++)
    {
        const OXR_SnapshotEntity& entity = target->entities[matches[i]];
        if (result->entityIdCapacityInput > 0)
        {
            result->entityIds[i] = entity.id;
        }
        if (result->entityStateCapacityInput > 0)
        {
            result->entityStates[i] = entity.state;
        }
        if (entity.state != XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT)
        {
            continue;
        }
        if (bounds && (entity.components & spatial_component_bit(XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT)))
        {
            bounds->bounds[i] = { .center = pose_to_xr(entity.center), .extents = { entity.extent, entity.extent } };
        }
        if (markers && (entity.components & spatial_component_bit(XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT)))
        {
            markers->markers[i] = { .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, .markerId = 0, .data = { .bufferId = entity.buffer, .bufferType = entity.buffer_type } };
        }
    }
    return XR_SUCCESS;
}

// Spec: buffer ids of other snapshots are invalid, and each getter reads one buffer type
static XrResult spatial_buffer (XrSpatialSnapshotEXT snapshot, const XrSpatialBufferGetInfoEXT* info, XrSpatialBufferTypeEXT type, const char*& out_text)
{
    out_text = nullptr;
    const OXR_SpatialSnapshot* target = find_spatial_snapshot(snapshot);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT);
    const OXR_SnapshotEntity* entity = nullptr;
    for (u32 i = 0; i < target->entities_count && info->bufferId != XR_NULL_SPATIAL_BUFFER_ID_EXT; i++)
    {
        const b8 has_marker = target->entities[i].components & spatial_component_bit(XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT);
        entity              = has_marker && target->entities[i].buffer == info->bufferId ? &target->entities[i] : entity;
    }
    if (!entity)
    {
        return XR_ERROR_SPATIAL_BUFFER_ID_INVALID_EXT;
    }
    OXR_CHECK(entity->buffer_type == type);
    const OXR_SpatialContext* context = runtime()->spatial_contexts.try_get(target->context);
    const OXR_Session*        session = context ? runtime()->sessions.try_get(context->session) : nullptr;
    ASSERT(session && entity->buffer <= session->spatial_texts.length);
    if (!session || entity->buffer > session->spatial_texts.length)
    {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    out_text = session->spatial_texts.data[entity->buffer - 1].text;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpatialBufferStringEXT (XrSpatialSnapshotEXT snapshot, const XrSpatialBufferGetInfoEXT* info, u32 capacity, u32* count_output, char* buffer)
{
    const OXR_Lock lock(__func__);
    const char*    text   = nullptr;
    XrResult       result = spatial_buffer(snapshot, info, XR_SPATIAL_BUFFER_TYPE_STRING_EXT, text);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    const u32 length = (u32)cstr_length(text) + 1;
    result           = oxr_two_call(capacity, count_output, buffer, length);
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    memory_copy(buffer, text, length);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpatialBufferUint8EXT (XrSpatialSnapshotEXT snapshot, const XrSpatialBufferGetInfoEXT* info, u32 capacity, u32* count_output, u8* buffer)
{
    const OXR_Lock lock(__func__);
    const char*    text   = nullptr;
    XrResult       result = spatial_buffer(snapshot, info, XR_SPATIAL_BUFFER_TYPE_UINT8_EXT, text);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    const u32 length = (u32)cstr_length(text);
    result           = oxr_two_call(capacity, count_output, buffer, length);
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    memory_copy(buffer, text, length);
    return XR_SUCCESS;
}

// Markers carry no buffers of these types, so these only tell which error applies
static XrResult no_spatial_buffer (XrSpatialSnapshotEXT snapshot, const XrSpatialBufferGetInfoEXT* info, XrSpatialBufferTypeEXT type)
{
    const OXR_Lock lock(__func__);
    const char*    text   = nullptr;
    const XrResult result = spatial_buffer(snapshot, info, type, text);
    ASSERT(result != XR_SUCCESS);
    return result == XR_SUCCESS ? XR_ERROR_RUNTIME_FAILURE : result;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpatialBufferUint16EXT (XrSpatialSnapshotEXT snapshot, const XrSpatialBufferGetInfoEXT* info, u32 capacity, u32* count_output, u16* buffer) // NOLINT(misc-const-correctness,readability-non-const-parameter): PFN_xrGetSpatialBufferUint16EXT
{
    UNUSED(capacity);
    UNUSED(count_output);
    UNUSED(buffer);
    return no_spatial_buffer(snapshot, info, XR_SPATIAL_BUFFER_TYPE_UINT16_EXT);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpatialBufferUint32EXT (XrSpatialSnapshotEXT snapshot, const XrSpatialBufferGetInfoEXT* info, u32 capacity, u32* count_output, u32* buffer) // NOLINT(misc-const-correctness,readability-non-const-parameter): PFN_xrGetSpatialBufferUint32EXT
{
    UNUSED(capacity);
    UNUSED(count_output);
    UNUSED(buffer);
    return no_spatial_buffer(snapshot, info, XR_SPATIAL_BUFFER_TYPE_UINT32_EXT);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpatialBufferFloatEXT (XrSpatialSnapshotEXT snapshot, const XrSpatialBufferGetInfoEXT* info, u32 capacity, u32* count_output, f32* buffer) // NOLINT(misc-const-correctness,readability-non-const-parameter): PFN_xrGetSpatialBufferFloatEXT
{
    UNUSED(capacity);
    UNUSED(count_output);
    UNUSED(buffer);
    return no_spatial_buffer(snapshot, info, XR_SPATIAL_BUFFER_TYPE_FLOAT_EXT);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpatialBufferVector2fEXT (XrSpatialSnapshotEXT snapshot, const XrSpatialBufferGetInfoEXT* info, u32 capacity, u32* count_output, XrVector2f* buffer) // NOLINT(misc-const-correctness,readability-non-const-parameter): PFN_xrGetSpatialBufferVector2fEXT
{
    UNUSED(capacity);
    UNUSED(count_output);
    UNUSED(buffer);
    return no_spatial_buffer(snapshot, info, XR_SPATIAL_BUFFER_TYPE_VECTOR2F_EXT);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSpatialBufferVector3fEXT (XrSpatialSnapshotEXT snapshot, const XrSpatialBufferGetInfoEXT* info, u32 capacity, u32* count_output, XrVector3f* buffer) // NOLINT(misc-const-correctness,readability-non-const-parameter): PFN_xrGetSpatialBufferVector3fEXT
{
    UNUSED(capacity);
    UNUSED(count_output);
    UNUSED(buffer);
    return no_spatial_buffer(snapshot, info, XR_SPATIAL_BUFFER_TYPE_VECTOR3F_EXT);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateSpatialEntityFromIdEXT (XrSpatialContextEXT context, const XrSpatialEntityFromIdCreateInfoEXT* info, XrSpatialEntityEXT* out_entity)
{
    const OXR_Lock      lock(__func__);
    OXR_SpatialContext* target = find_spatial_context(context);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_SPATIAL_ENTITY_FROM_ID_CREATE_INFO_EXT);
    OXR_CHECK(out_entity);
    if (info->entityId == XR_NULL_SPATIAL_ENTITY_ID_EXT || !find_marker(*target, info->entityId))
    {
        return XR_ERROR_SPATIAL_ENTITY_ID_INVALID_EXT;
    }
    const OXR_SpatialEntity entity = { .context = spatial_context_handle(context), .id = info->entityId };
    *out_entity = (XrSpatialEntityEXT)(uintptr_t)spatial_entity_xr(runtime()->spatial_entities.add(entity));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroySpatialEntityEXT (XrSpatialEntityEXT entity)
{
    const OXR_Lock lock(__func__);
    if (!find_spatial_entity(entity))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->spatial_entities.remove(spatial_entity_handle(entity));
    return XR_SUCCESS;
}

//
// Public API: negotiation and function table
//

//
// Display and system: XR_FB_display_refresh_rate, XR_FB_color_space, XR_META_recommended_layer_resolution, XR_META_tile_properties_hint
//
// Render models
//

// XR_FB_render_model keys: "XRSM", the controller type and the model path's index plus one, so
// a key names one model for good and a different simulated controller is a different key;
// keyboards do not depend on the controller and carry type 0
#define OXR_RENDER_MODEL_KEY_TAG 0x5852534D00000000ull
#define OXR_RENDER_MODEL_KEYBOARDS 2 // the paths from here on are keyboards

static const char* const oxr_render_model_paths[5] = { "/model_fb/controller/left", "/model_fb/controller/right", "/model_fb/keyboard/local", "/model_fb/keyboard/remote", "/model_meta/keyboard/virtual" };

#define OXR_RENDER_MODEL_VIRTUAL_KEYBOARD 4 // only with XR_META_virtual_keyboard

static u32 render_model_paths_count (const OXR_Instance* instance)
{
    return instance->ext.META_virtual_keyboard ? ARRAY_SIZE(oxr_render_model_paths) : OXR_RENDER_MODEL_VIRTUAL_KEYBOARD;
}

// The levels a model is offered at: the textured virtual keyboard needs glTF subset 2, the others
// fit both, as subset 2 loaders take subset 1 models (Godot's vendors plugin asks for 2 only)
static XrRenderModelFlagsFB render_model_levels (u32 model)
{
    const XrRenderModelFlagsFB both = XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB | XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_2_BIT_FB;
    return model == OXR_RENDER_MODEL_VIRTUAL_KEYBOARD ? XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_2_BIT_FB : both;
}

static XrRenderModelKeyFB render_model_key (SIM_Controller controller, u32 model)
{
    const u64 type = model < OXR_RENDER_MODEL_KEYBOARDS ? (u64)controller : 0;
    return OXR_RENDER_MODEL_KEY_TAG | (type << 8) | (u64)(model + 1);
}

static b8 render_model_of_key (XrRenderModelKeyFB key, SIM_Controller& out_controller, u32& out_model)
{
    const u64 controller = (key >> 8) & 0xFFu;
    const u64 model      = key & 0xFFu;
    if ((key & 0xFFFFFFFF00000000ull) != OXR_RENDER_MODEL_KEY_TAG || (key & 0xFFFF0000ull) != 0 || controller >= (u64)SIM_Controller::Count || model < 1 || model > ARRAY_SIZE(oxr_render_model_paths))
    {
        return false;
    }
    if (model > OXR_RENDER_MODEL_KEYBOARDS && controller != 0)
    {
        return false;
    }
    out_controller = (SIM_Controller)controller;
    out_model      = (u32)model - 1;
    return true;
}

static void render_model_name (SIM_Controller controller, u32 model, char* out_name, usize size)
{
    if (model < OXR_RENDER_MODEL_KEYBOARDS)
    {
        cstr_format(out_name, size, "puppetxr %s %s", sim_controller_name(controller), model == 0 ? "left" : "right");
    }
    else if (model == OXR_RENDER_MODEL_VIRTUAL_KEYBOARD)
    {
        cstr_copy(out_name, size, "puppetxr virtual keyboard");
    }
    else
    {
        cstr_format(out_name, size, "puppetxr keyboard %s", model == OXR_RENDER_MODEL_KEYBOARDS ? "local" : "remote");
    }
}

PRINTF_LIKE(2, 3) static void append_text (Array<char>& out, const char* format, ...) // NOLINT(cert-dcl50-cpp): printf style like cstr_format
{
    char    line[256];
    va_list args = {};
    va_start(args, format);
    const usize length = cstr_format_v(line, sizeof(line), format, args);
    va_end(args);
    out.add(line, length < sizeof(line) ? length : sizeof(line) - 1);
}

static void append_u32 (Array<u8>& out, u32 value)
{
    const u8 bytes[4] = { (u8)value, (u8)(value >> 8), (u8)(value >> 16), (u8)(value >> 24) };
    out.add(bytes, 4);
}

static void append_f32 (Array<u8>& out, f32 value)
{
    u32 bits = 0;
    memory_copy(&bits, &value, 4);
    append_u32(out, bits);
}

// glTF 2.0 binary (GLB): a node, mesh and material per model part. Positions, normals, texture
// coordinates and 16-bit indices relative to the part in one buffer; then the key times 0 and 1
// shared by every animation and, per part with a press, its translation at both. A textured
// model's texture is an image the app fills itself, named by the URI of XR_META_virtual_keyboard.
static void write_render_model (const SIM_RenderModel& model, const char* name, Array<u8>& out)
{
    const b8    textured       = model.texture_id != 0;
    const u32   per_part       = textured ? 4 : 3; // accessors
    const usize vec3_length    = (usize)model.vertices_count * sizeof(Vec3);
    const usize uvs_offset     = vec3_length * 2;
    const usize uvs_length     = textured ? (usize)model.vertices_count * sizeof(Vec2) : 0;
    const usize indices_offset = uvs_offset + uvs_length;
    const usize indices_length = (usize)model.indices_count * sizeof(u16);
    const usize anim_offset    = (indices_offset + indices_length + 3) & ~(usize)3;
    u32         animated       = 0;
    for (u32 p = 0; p < model.parts_count; p++)
    {
        animated += model.parts[p].press.x != 0.0f || model.parts[p].press.y != 0.0f || model.parts[p].press.z != 0.0f ? 1 : 0;
    }
    const usize anim_length = animated > 0 ? 8 + (usize)animated * 24 : 0;
    const usize bin_length  = anim_offset + anim_length;
    const u32   times       = model.parts_count * per_part; // accessor of the key times

    Array<char> json = {};
    defer { json.release(); };
    append_text(json, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"puppetxr\"},\"scene\":0,\"scenes\":[{\"name\":\"%s\",\"nodes\":[", name);
    for (u32 p = 0; p < model.parts_count; p++)
    {
        append_text(json, "%s%u", p ? "," : "", p);
    }
    append_text(json, "]}],\"nodes\":[");
    for (u32 p = 0; p < model.parts_count; p++)
    {
        append_text(json, "%s{\"mesh\":%u", p ? "," : "", p);
        if (model.parts[p].name[0])
        {
            append_text(json, ",\"name\":\"%s\"", model.parts[p].name);
        }
        append_text(json, "}");
    }
    append_text(json, "],\"meshes\":[");
    for (u32 p = 0; p < model.parts_count; p++)
    {
        const u32 a = p * per_part;
        append_text(json, "%s{\"primitives\":[{\"attributes\":{\"POSITION\":%u,\"NORMAL\":%u", p ? "," : "", a, a + 1);
        if (textured)
        {
            append_text(json, ",\"TEXCOORD_0\":%u", a + 2);
        }
        append_text(json, "},\"indices\":%u,\"material\":%u}]}", a + per_part - 1, p);
    }
    append_text(json, "],\"materials\":[");
    for (u32 p = 0; p < model.parts_count; p++)
    {
        const Vec4& c = model.parts[p].color;
        append_text(json, "%s{\"pbrMetallicRoughness\":{\"baseColorFactor\":[%.6g,%.6g,%.6g,%.6g],", p ? "," : "", (f64)c.x, (f64)c.y, (f64)c.z, (f64)c.w);
        if (textured)
        {
            append_text(json, "\"baseColorTexture\":{\"index\":0},");
        }
        append_text(json, "\"metallicFactor\":0,\"roughnessFactor\":0.6}%s}", textured ? ",\"alphaMode\":\"BLEND\"" : "");
    }
    append_text(json, "],");
    if (textured)
    {
        // Spec XR_META_virtual_keyboard: metaVirtualKeyboard://texture/{textureID}?w={width}&h={height}&fmt=RGBA32
        append_text(json, "\"samplers\":[{\"magFilter\":9729,\"minFilter\":9729,\"wrapS\":33071,\"wrapT\":33071}],\"textures\":[{\"sampler\":0,\"source\":0}],");
        append_text(json, "\"images\":[{\"uri\":\"metaVirtualKeyboard://texture/%llu?w=%u&h=%u&fmt=RGBA32\"}],", (unsigned long long)model.texture_id, model.texture_width, model.texture_height);
    }
    if (animated > 0)
    {
        append_text(json, "\"animations\":[");
        u32 a = 0;
        for (u32 p = 0; p < model.parts_count; p++)
        {
            const Vec3 press = model.parts[p].press;
            if (press.x == 0.0f && press.y == 0.0f && press.z == 0.0f)
            {
                continue;
            }
            append_text(json, "%s{\"name\":\"press_%u\",\"samplers\":[{\"input\":%u,\"output\":%u,\"interpolation\":\"LINEAR\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":%u,\"path\":\"translation\"}}]}", a ? "," : "", p, times, times + 1 + a, p);
            a++;
        }
        append_text(json, "],");
    }
    append_text(json, "\"buffers\":[{\"byteLength\":%zu}],\"bufferViews\":[", bin_length);
    append_text(json, "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%zu,\"target\":34962},", vec3_length);
    append_text(json, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962},", vec3_length, vec3_length);
    if (textured)
    {
        append_text(json, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962},", uvs_offset, uvs_length);
    }
    append_text(json, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}", indices_offset, indices_length);
    if (animated > 0)
    {
        append_text(json, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", anim_offset, anim_length);
    }
    append_text(json, "],\"accessors\":[");
    const u32 indices_view = textured ? 3 : 2;
    for (u32 p = 0; p < model.parts_count; p++)
    {
        const SIM_ModelPart& part = model.parts[p];
        Vec3 low  = model.positions[part.vertices_first];
        Vec3 high = low;
        for (u32 v = part.vertices_first; v < part.vertices_first + part.vertices_count; v++)
        {
            low  = zm::min(low, model.positions[v]);
            high = zm::max(high, model.positions[v]);
        }
        append_text(json, "%s{\"bufferView\":0,\"byteOffset\":%zu,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\",", p ? "," : "", (usize)part.vertices_first * sizeof(Vec3), part.vertices_count);
        append_text(json, "\"min\":[%.9g,%.9g,%.9g],\"max\":[%.9g,%.9g,%.9g]},", (f64)low.x, (f64)low.y, (f64)low.z, (f64)high.x, (f64)high.y, (f64)high.z);
        append_text(json, "{\"bufferView\":1,\"byteOffset\":%zu,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},", (usize)part.vertices_first * sizeof(Vec3), part.vertices_count);
        if (textured)
        {
            append_text(json, "{\"bufferView\":2,\"byteOffset\":%zu,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"},", (usize)part.vertices_first * sizeof(Vec2), part.vertices_count);
        }
        append_text(json, "{\"bufferView\":%u,\"byteOffset\":%zu,\"componentType\":5123,\"count\":%u,\"type\":\"SCALAR\"}", indices_view, (usize)part.indices_first * sizeof(u16), part.indices_count);
    }
    if (animated > 0)
    {
        append_text(json, ",{\"bufferView\":%u,\"byteOffset\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\",\"min\":[0],\"max\":[1]}", indices_view + 1);
        for (u32 a = 0; a < animated; a++)
        {
            append_text(json, ",{\"bufferView\":%u,\"byteOffset\":%u,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}", indices_view + 1, 8 + a * 24);
        }
    }
    append_text(json, "]}");
    while (json.length % 4 != 0)
    {
        json.add(' ');
    }

    out.length = 0;
    out.reserve(12 + 8 + json.length + 8 + bin_length);
    out.add((const u8*)"glTF", 4);
    append_u32(out, 2);
    append_u32(out, (u32)(12 + 8 + json.length + 8 + bin_length));
    append_u32(out, (u32)json.length);
    out.add((const u8*)"JSON", 4);
    out.add((const u8*)json.data, json.length);
    append_u32(out, (u32)bin_length);
    out.add((const u8*)"BIN\0", 4);
    const usize bin_start = out.length;
    out.add((const u8*)model.positions, vec3_length);
    out.add((const u8*)model.normals, vec3_length);
    if (textured)
    {
        out.add((const u8*)model.uvs, uvs_length);
    }
    for (u32 p = 0; p < model.parts_count; p++)
    {
        const SIM_ModelPart& part = model.parts[p];
        for (u32 i = part.indices_first; i < part.indices_first + part.indices_count; i++)
        {
            const u16 local = (u16)(model.indices[i] - part.vertices_first);
            out.add((const u8*)&local, sizeof(local));
        }
    }
    while (out.length - bin_start < anim_offset)
    {
        out.add(0);
    }
    if (animated > 0)
    {
        append_f32(out, 0.0f);
        append_f32(out, 1.0f);
        for (u32 p = 0; p < model.parts_count; p++)
        {
            const Vec3 press = model.parts[p].press;
            if (press.x != 0.0f || press.y != 0.0f || press.z != 0.0f)
            {
                const f32 keys[6] = { 0.0f, 0.0f, 0.0f, press.x, press.y, press.z };
                for (const f32 key : keys)
                {
                    append_f32(out, key);
                }
            }
        }
    }
    ASSERT(out.length - bin_start == bin_length);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateRenderModelPathsFB (XrSession session, uint32_t capacity, uint32_t* count_output, XrRenderModelPathInfoFB* paths)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    OXR_CHECK(count_output);
    const u32 count                     = render_model_paths_count(inst);
    *count_output                       = count;
    sess->render_model_paths_enumerated = true;
    if (capacity == 0)
    {
        return XR_SUCCESS;
    }
    OXR_CHECK(paths);
    if (capacity < count)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    for (u32 i = 0; i < count; i++)
    {
        OXR_CHECK(paths[i].type == XR_TYPE_RENDER_MODEL_PATH_INFO_FB);
        paths[i].path = path_intern(inst, oxr_render_model_paths[i]);
    }
    return XR_SUCCESS;
}

// Spec XR_FB_render_model: paths only after xrEnumerateRenderModelPathsFB, and only those; a model
// of none of the requested glTF levels is unavailable
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetRenderModelPropertiesFB (XrSession session, XrPath path, XrRenderModelPropertiesFB* properties)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    OXR_CHECK_TYPE(properties, XR_TYPE_RENDER_MODEL_PROPERTIES_FB);
    if (!sess->render_model_paths_enumerated)
    {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    const char* str   = path_string(inst, path);
    const u32   count = render_model_paths_count(inst);
    u32         index = count;
    for (u32 i = 0; str && i < count; i++)
    {
        index = cstr_equal(str, oxr_render_model_paths[i]) ? i : index;
    }
    if (index == count)
    {
        return XR_ERROR_PATH_INVALID;
    }

    XrRenderModelFlagsFB requested = XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB;
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)properties->next; next; next = next->next)
    {
        if (next->type == XR_TYPE_RENDER_MODEL_CAPABILITIES_REQUEST_FB)
        {
            requested = ((const XrRenderModelCapabilitiesRequestFB*)next)->flags;
        }
    }
    properties->vendorId     = 0;
    properties->modelKey     = XR_NULL_RENDER_MODEL_KEY_FB;
    properties->modelVersion = 1;
    properties->flags        = 0;
    const XrRenderModelFlagsFB levels = requested & render_model_levels(index);
    if (!levels)
    {
        return XR_RENDER_MODEL_UNAVAILABLE_FB;
    }
    const XrRenderModelFlagsFB level  = (levels & XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB) ? XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB : XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_2_BIT_FB;
    const SIM_Controller controller = sim_get_controller(runtime()->sim);
    render_model_name(controller, index, properties->modelName, sizeof(properties->modelName));
    properties->modelKey = render_model_key(controller, index);
    properties->flags    = level;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrLoadRenderModelFB (XrSession session, const XrRenderModelLoadInfoFB* info, XrRenderModelBufferFB* buffer)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_RENDER_MODEL_LOAD_INFO_FB);
    OXR_CHECK_TYPE(buffer, XR_TYPE_RENDER_MODEL_BUFFER_FB);
    SIM_Controller controller = SIM_Controller::Count;
    u32            index       = 0;
    if (!render_model_of_key(info->modelKey, controller, index) || index >= render_model_paths_count(session_instance(sess)))
    {
        return XR_ERROR_RENDER_MODEL_KEY_INVALID_FB;
    }
    buffer->bufferCountOutput = 0;
    const b8 keyboard         = index >= OXR_RENDER_MODEL_KEYBOARDS;
    if (!keyboard && controller != sim_get_controller(runtime()->sim))
    {
        return XR_RENDER_MODEL_UNAVAILABLE_FB;
    }

    SIM_RenderModel* model = (SIM_RenderModel*)os_memory_alloc(sizeof(SIM_RenderModel));
    defer { os_memory_release(model); };
    if (index == OXR_RENDER_MODEL_VIRTUAL_KEYBOARD)
    {
        SIM_VkbLayout layout = {};
        sim_vkb_layout(layout);
        sim_vkb_model(layout, *model);
    }
    else if (keyboard)
    {
        sim_keyboard_model(*model);
    }
    else
    {
        sim_controller_model(controller, index == 0 ? SIM_Device::LeftHand : SIM_Device::RightHand, *model);
    }
    char name[XR_MAX_RENDER_MODEL_NAME_SIZE_FB];
    render_model_name(controller, index, name, sizeof(name));
    Array<u8> glb = {};
    defer { glb.release(); };
    write_render_model(*model, name, glb);

    buffer->bufferCountOutput = (u32)glb.length;
    if (buffer->bufferCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    OXR_CHECK(buffer->buffer);
    if (buffer->bufferCapacityInput < glb.length)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    memory_copy(buffer->buffer, glb.data, glb.length);
    return XR_SUCCESS;
}

//
// Render models: XR_EXT_render_model, XR_EXT_interaction_render_model
//

static const char* const oxr_render_model_nodes[OXR_RENDER_MODEL_NODES] = { "trigger", "buttons" };

// The asset of a controller in a hand: the XR_FB_render_model mesh without glTF animations,
// whose trigger and buttons nodes the runtime moves (xrGetRenderModelStateEXT)
static void render_model_glb (SIM_Controller controller, u32 hand, Array<u8>& out)
{
    SIM_RenderModel* model = (SIM_RenderModel*)os_memory_alloc(sizeof(SIM_RenderModel));
    defer { os_memory_release(model); };
    sim_controller_model(controller, hand == 0 ? SIM_Device::LeftHand : SIM_Device::RightHand, *model);
    char name[64];
    render_model_name(controller, hand, name, sizeof(name));
    write_render_model(*model, name, out);
}

// Spec: the same UUID always names the same bytes, across sessions, so it is a hash of them
static XrUuid render_model_uuid (const Array<u8>& glb)
{
    const u64 low  = hash(glb.data, glb.length);
    const u64 high = hash(glb.data + 1, glb.length - 1);
    XrUuid    uuid = {};
    memory_copy(uuid.data, &low, 8);
    memory_copy(uuid.data + 8, &high, 8);
    uuid.data[6] = (u8)((uuid.data[6] & 0x0F) | 0x80); // RFC 9562 version 8, custom
    uuid.data[8] = (u8)((uuid.data[8] & 0x3F) | 0x80);
    return uuid;
}

static b8 render_model_hand_active (const OXR_Session* session, u32 hand)
{
    const OXR_Runtime* rt = runtime();
    for (usize i = 0; i < rt->actions.length(); i++)
    {
        const OXR_Action& action = rt->actions.data()[i];
        if (action_attached_to(session, &action) && action.sources_count[hand] > 0)
        {
            return true;
        }
    }
    return false;
}

// Spec XR_EXT_interaction_render_model: the list, subaction paths and hands change only here.
// A hand holding a controller with bound actions has a model; another controller or a hand that
// let go and took it again is a new id
static void sync_render_models (OXR_Runtime* rt, OXR_Instance* inst, OXR_Session* session, OXR_SessionHandle handle)
{
    if (!inst->ext.EXT_interaction_render_model)
    {
        return;
    }
    const SIM_Controller controller = sim_get_controller(rt->sim);
    b8                   changed    = false;
    for (u32 hand = 0; hand < 2; hand++)
    {
        const SIM_Device device  = hand == 0 ? SIM_Device::LeftHand : SIM_Device::RightHand;
        const b8         present = sim_get_hand_source(rt->sim, device) == SIM_HandSource::Controller && render_model_hand_active(session, hand);
        const b8         had     = session->render_model_id[hand] != 0;
        if (present != had || (present && session->render_model_controller[hand] != controller))
        {
            session->render_model_id[hand]         = present ? ++session->render_model_ids : 0;
            session->render_model_controller[hand] = controller;
            changed                                = changed || present || had;
        }

        // Spec: the hand's path, when an action of an active set lists it and binds it
        b8 subaction = false;
        for (usize i = 0; i < rt->actions.length() && present; i++)
        {
            const OXR_Action&    action = rt->actions.data()[i];
            const OXR_ActionSet* set    = rt->action_sets.try_get(action.set);
            subaction                   = subaction || (set && set->active && action_attached_to(session, &action) && action.has_subaction_paths && (action.subaction_mask & (1 << hand)) && action.sources_count[hand] > 0);
        }
        session->render_model_subaction[hand] = subaction;
    }
    session->render_models_synced = true;
    if (changed)
    {
        push_event(inst, { .type = XR_TYPE_EVENT_DATA_INTERACTION_RENDER_MODELS_CHANGED_EXT, .session = session_xr(handle) });
    }
}

static OXR_RenderModel* find_render_model (XrRenderModelEXT model)
{
    return runtime()->render_models.try_get(render_model_handle(model));
}

// A model of a hand that no longer lists its id is inactive: spec, it stays valid but is not located
static b8 render_model_active (const OXR_RenderModel& model)
{
    const OXR_Session* session = runtime()->sessions.try_get(model.session);
    return session && session->render_model_id[model.hand] == model.id;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateInteractionRenderModelIdsEXT (XrSession session, const XrInteractionRenderModelIdsEnumerateInfoEXT* info, u32 capacity, u32* count_output, XrRenderModelIdEXT* ids)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK(!info || info->type == XR_TYPE_INTERACTION_RENDER_MODEL_IDS_ENUMERATE_INFO_EXT);
    if (!sess->running)
    {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    XrRenderModelIdEXT list[2] = {};
    u32                count   = 0;
    for (u32 hand = 0; hand < 2; hand++)
    {
        if (sess->render_model_id[hand])
        {
            list[count++] = sess->render_model_id[hand];
        }
    }
    const XrResult result = oxr_two_call(capacity, count_output, ids, count);
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    memory_copy(ids, list, count * sizeof(XrRenderModelIdEXT));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateRenderModelEXT (XrSession session, const XrRenderModelCreateInfoEXT* info, XrRenderModelEXT* out_model)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT);
    OXR_CHECK(out_model);
    OXR_CHECK(info->gltfExtensionCount == 0 || info->gltfExtensions);
    for (u32 i = 0; i < info->gltfExtensionCount; i++)
    {
        OXR_CHECK(info->gltfExtensions[i]);
    }
    // Spec: the assets need no glTF extension, so the app's list changes nothing
    u32 hand = 2;
    for (u32 h = 0; h < 2; h++)
    {
        hand = info->renderModelId != XR_NULL_RENDER_MODEL_ID_EXT && sess->render_model_id[h] == info->renderModelId ? h : hand;
    }
    if (hand == 2)
    {
        return XR_ERROR_RENDER_MODEL_ID_INVALID_EXT;
    }
    OXR_RenderModel model = { .session = session_handle(session), .id = info->renderModelId, .hand = hand, .controller = sess->render_model_controller[hand] };
    Array<u8>       glb   = {};
    defer { glb.release(); };
    render_model_glb(model.controller, hand, glb);
    model.cache_id = render_model_uuid(glb);
    *out_model     = (XrRenderModelEXT)(uintptr_t)render_model_xr(runtime()->render_models.add(model));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyRenderModelEXT (XrRenderModelEXT model)
{
    const OXR_Lock lock(__func__);
    if (!find_render_model(model))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->render_models.remove(render_model_handle(model));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetRenderModelPropertiesEXT (XrRenderModelEXT model, const XrRenderModelPropertiesGetInfoEXT* info, XrRenderModelPropertiesEXT* properties)
{
    const OXR_Lock         lock(__func__);
    const OXR_RenderModel* target  = find_render_model(model);
    OXR_Session*           session = target ? runtime()->sessions.try_get(target->session) : nullptr;
    if (!session)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(!info || info->type == XR_TYPE_RENDER_MODEL_PROPERTIES_GET_INFO_EXT);
    OXR_CHECK_TYPE(properties, XR_TYPE_RENDER_MODEL_PROPERTIES_EXT);
    properties->cacheId             = target->cache_id;
    properties->animatableNodeCount = OXR_RENDER_MODEL_NODES;

    // Spec: the session remembers the UUIDs it returned; only those make assets
    b8 known = false;
    for (u32 i = 0; i < session->render_model_uuids_count; i++)
    {
        known = known || memory_equal(session->render_model_uuids[i].uuid.data, target->cache_id.data, sizeof(XrUuid));
    }
    if (!known && session->render_model_uuids_count < OXR_MAX_RENDER_MODEL_ASSETS)
    {
        session->render_model_uuids[session->render_model_uuids_count++] = { .uuid = target->cache_id, .controller = target->controller, .hand = target->hand };
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateRenderModelSpaceEXT (XrSession session, const XrRenderModelSpaceCreateInfoEXT* info, XrSpace* out_space)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT);
    OXR_CHECK(out_space);
    const OXR_RenderModel* model = find_render_model(info->renderModel);
    if (!model)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(model->session == session_handle(session));
    OXR_Space space    = { .session = session_handle(session), .kind = OXR_SpaceKind::RenderModel, .offset = zm::POSE_IDENTITY };
    space.render_model = render_model_handle(info->renderModel);
    *out_space         = space_xr(runtime()->spaces.add(space));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateRenderModelAssetEXT (XrSession session, const XrRenderModelAssetCreateInfoEXT* info, XrRenderModelAssetEXT* out_asset)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT);
    OXR_CHECK(out_asset);
    const OXR_RenderModelUuid* entry = nullptr;
    for (u32 i = 0; i < sess->render_model_uuids_count; i++)
    {
        entry = memory_equal(sess->render_model_uuids[i].uuid.data, info->cacheId.data, sizeof(XrUuid)) ? &sess->render_model_uuids[i] : entry;
    }
    if (!entry)
    {
        return XR_ERROR_RENDER_MODEL_ASSET_UNAVAILABLE_EXT;
    }
    OXR_RenderModelAsset asset = { .session = session_handle(session) };
    render_model_glb(entry->controller, entry->hand, asset.glb);
    *out_asset = (XrRenderModelAssetEXT)(uintptr_t)render_model_asset_xr(runtime()->render_model_assets.add(asset));
    return XR_SUCCESS;
}

static void destroy_render_model_asset (OXR_RenderModelAssetHandle handle)
{
    OXR_RenderModelAsset* asset = runtime()->render_model_assets.try_get(handle);
    if (asset)
    {
        asset->glb.release();
        runtime()->render_model_assets.remove(handle);
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyRenderModelAssetEXT (XrRenderModelAssetEXT asset)
{
    const OXR_Lock lock(__func__);
    if (!runtime()->render_model_assets.try_get(render_model_asset_handle(asset)))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    destroy_render_model_asset(render_model_asset_handle(asset));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetRenderModelAssetDataEXT (XrRenderModelAssetEXT asset, const XrRenderModelAssetDataGetInfoEXT* info, XrRenderModelAssetDataEXT* data)
{
    const OXR_Lock              lock(__func__);
    const OXR_RenderModelAsset* target = runtime()->render_model_assets.try_get(render_model_asset_handle(asset));
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(!info || info->type == XR_TYPE_RENDER_MODEL_ASSET_DATA_GET_INFO_EXT);
    OXR_CHECK_TYPE(data, XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT);
    const XrResult result = oxr_two_call(data->bufferCapacityInput, &data->bufferCountOutput, data->buffer, (u32)target->glb.length);
    if (result != XR_SUCCESS || data->bufferCapacityInput == 0)
    {
        return result;
    }
    memory_copy(data->buffer, target->glb.data, target->glb.length);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetRenderModelAssetPropertiesEXT (XrRenderModelAssetEXT asset, const XrRenderModelAssetPropertiesGetInfoEXT* info, XrRenderModelAssetPropertiesEXT* properties) // NOLINT(misc-const-correctness): signature of the spec
{
    const OXR_Lock lock(__func__);
    if (!runtime()->render_model_assets.try_get(render_model_asset_handle(asset)))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(!info || info->type == XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT);
    OXR_CHECK_TYPE(properties, XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT);
    OXR_CHECK(properties->nodePropertyCount == OXR_RENDER_MODEL_NODES && properties->nodeProperties);
    for (u32 i = 0; i < OXR_RENDER_MODEL_NODES; i++)
    {
        cstr_copy(properties->nodeProperties[i].uniqueName, sizeof(properties->nodeProperties[i].uniqueName), oxr_render_model_nodes[i]);
    }
    return XR_SUCCESS;
}

// The trigger swings into the handle, a pressed face button sinks into the head; spec, the node
// poses replace the nodes' identity transforms in the asset
#define OXR_RENDER_MODEL_TRIGGER_TRAVEL 0.008f
#define OXR_RENDER_MODEL_BUTTON_TRAVEL  0.002f

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetRenderModelStateEXT (XrRenderModelEXT model, const XrRenderModelStateGetInfoEXT* info, XrRenderModelStateEXT* state) // NOLINT(misc-const-correctness): signature of the spec
{
    const OXR_Lock         lock(__func__);
    const OXR_RenderModel* target = find_render_model(model);
    if (!target)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT);
    OXR_CHECK_TYPE(state, XR_TYPE_RENDER_MODEL_STATE_EXT);
    OXR_CHECK(state->nodeStateCount == OXR_RENDER_MODEL_NODES && state->nodeStates);
    if (locate_time(info->displayTime) <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    // The inputs of the last device update: the sim does not predict buttons
    const SIM_Sim*   sim     = runtime()->sim;
    const SIM_Device device  = target->hand == 0 ? SIM_Device::LeftHand : SIM_Device::RightHand;
    const f32        trigger = sim_get_input(sim, device, SIM_Input::TriggerValue).value.x;
    const SIM_Input  buttons[] = { SIM_Input::AClick, SIM_Input::BClick, SIM_Input::XClick, SIM_Input::YClick, SIM_Input::ThumbstickClick };
    f32              pressed = 0.0f;
    for (const SIM_Input input : buttons)
    {
        pressed = zm::max(pressed, sim_get_input(sim, device, input).value.x);
    }
    state->nodeStates[0] = { .nodePose = pose_to_xr({ { 0.0f, OXR_RENDER_MODEL_TRIGGER_TRAVEL * trigger, 0.0f }, zm::QUAT_IDENTITY }), .isVisible = XR_TRUE };
    state->nodeStates[1] = { .nodePose = pose_to_xr({ { 0.0f, -OXR_RENDER_MODEL_BUTTON_TRAVEL * pressed, 0.0f }, zm::QUAT_IDENTITY }), .isVisible = XR_TRUE };
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateRenderModelSubactionPathsEXT (XrRenderModelEXT model, const XrInteractionRenderModelSubactionPathInfoEXT* info, u32 capacity, u32* count_output, XrPath* paths)
{
    const OXR_Lock         lock(__func__);
    const OXR_RenderModel* target  = find_render_model(model);
    const OXR_Session*     session = target ? runtime()->sessions.try_get(target->session) : nullptr;
    OXR_Instance*          inst    = session ? session_instance(session) : nullptr;
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(!info || info->type == XR_TYPE_INTERACTION_RENDER_MODEL_SUBACTION_PATH_INFO_EXT);
    const b8       listed = render_model_active(*target) && session->render_model_subaction[target->hand];
    const XrResult result = oxr_two_call(capacity, count_output, paths, listed ? 1 : 0);
    if (result != XR_SUCCESS || capacity == 0 || !listed)
    {
        return result;
    }
    paths[0] = path_intern(inst, target->hand == 0 ? "/user/hand/left" : "/user/hand/right");
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetRenderModelPoseTopLevelUserPathEXT (XrRenderModelEXT model, const XrInteractionRenderModelTopLevelUserPathGetInfoEXT* info, XrPath* out_path)
{
    const OXR_Lock         lock(__func__);
    const OXR_RenderModel* target  = find_render_model(model);
    const OXR_Session*     session = target ? runtime()->sessions.try_get(target->session) : nullptr;
    const OXR_Instance*    inst    = session ? session_instance(session) : nullptr;
    if (!inst)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_INTERACTION_RENDER_MODEL_TOP_LEVEL_USER_PATH_GET_INFO_EXT);
    OXR_CHECK(out_path && info->topLevelUserPathCount > 0 && info->topLevelUserPaths);
    for (u32 i = 0; i < info->topLevelUserPathCount; i++)
    {
        for (u32 j = 0; j < i; j++)
        {
            OXR_CHECK(info->topLevelUserPaths[i] != info->topLevelUserPaths[j]);
        }
        OXR_TopLevel top = {};
        if (!path_top_level(inst, info->topLevelUserPaths[i], top))
        {
            return XR_ERROR_PATH_INVALID;
        }
    }
    // The hand that holds the controller
    *out_path = XR_NULL_PATH;
    for (u32 i = 0; i < info->topLevelUserPathCount && render_model_active(*target); i++)
    {
        OXR_TopLevel top = {};
        path_top_level(inst, info->topLevelUserPaths[i], top);
        *out_path = (u32)top == target->hand ? info->topLevelUserPaths[i] : *out_path;
    }
    return XR_SUCCESS;
}

//
// Virtual keyboard: XR_META_virtual_keyboard
//

static OXR_VirtualKeyboard* find_keyboard (XrVirtualKeyboardMETA keyboard)
{
    return runtime()->keyboards.try_get(keyboard_handle(keyboard));
}

// The default places: far in front of the head and a little below it, or within reach and
// tilted for poking; yaw only, so a tilted head still gets a level keyboard
static Pose vkb_default_pose (const SIM_Sample& sample, XrVirtualKeyboardLocationTypeMETA type)
{
    const Pose& head     = sample.devices[(u32)SIM_Device::Head].pose;
    const Vec3  forward  = zm::rotate(head.orientation, { 0.0f, 0.0f, -1.0f });
    const f32   yaw      = zm::atan2(-forward.x, -forward.z);
    const b8    direct   = type == XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_DIRECT_META;
    const f32   distance = direct ? 0.35f : 0.8f;
    const f32   drop     = direct ? 0.45f : 0.25f;
    const f32   tilt     = direct ? -45.0f : -20.0f;
    const Quat  turn     = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, yaw);
    const Vec3  ahead    = zm::rotate(turn, { 0.0f, 0.0f, -distance });
    return { head.position + ahead + Vec3{ 0.0f, -drop, 0.0f }, turn * zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, tilt * zm::DEG_TO_RAD) };
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateVirtualKeyboardMETA (XrSession session, const XrVirtualKeyboardCreateInfoMETA* info, XrVirtualKeyboardMETA* out_keyboard)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_VIRTUAL_KEYBOARD_CREATE_INFO_META);
    OXR_CHECK(out_keyboard);
    OXR_Runtime*            rt     = runtime();
    const OXR_SessionHandle handle = session_handle(session);
    for (usize i = 0; i < rt->keyboards.length(); i++)
    {
        if (rt->keyboards.data()[i].session == handle)
        {
            return XR_ERROR_LIMIT_REACHED;
        }
    }
    OXR_VirtualKeyboard keyboard = { .session = handle, .texture_dirty = true };
    sim_vkb_layout(keyboard.layout);
    keyboard.changed_keys = (1ull << keyboard.layout.keys_count) - 1; // the first read sets every key
    for (i32& key : keyboard.pressing)
    {
        key = -1;
    }
    *out_keyboard = keyboard_xr(rt->keyboards.add(keyboard));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyVirtualKeyboardMETA (XrVirtualKeyboardMETA keyboard)
{
    const OXR_Lock lock(__func__);
    if (!find_keyboard(keyboard))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->keyboards.remove(keyboard_handle(keyboard));
    return XR_SUCCESS;
}

// Where the app asks the keyboard to be: a custom pose in a space, or one of the default places
static XrResult vkb_place (OXR_VirtualKeyboard* keyboard, XrVirtualKeyboardLocationTypeMETA type, XrSpace space_handle, const XrPosef& pose_in_space, f32 scale)
{
    OXR_CHECK(type == XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_CUSTOM_META || type == XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_FAR_META || type == XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_DIRECT_META);
    OXR_GET_CONST_SPACE(space, space_handle);
    if (space->session != keyboard->session)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    const SIM_Sim* sim    = runtime()->sim;
    SIM_Sample     sample = {};
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    if (type != XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_CUSTOM_META)
    {
        keyboard->pose  = vkb_default_pose(sample, type);
        keyboard->scale = 1.0f;
        return XR_SUCCESS;
    }
    if (!xr_pose_valid(pose_in_space))
    {
        return XR_ERROR_POSE_INVALID;
    }
    OXR_CHECK(scale > 0.0f && zm::is_finite(scale));
    const OXR_Located located = locate_world(space, sample);
    if (located.valid)
    {
        keyboard->pose = zm::pose_mul(located.pose, pose_from_xr(pose_in_space));
    }
    keyboard->scale = scale;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateVirtualKeyboardSpaceMETA (XrSession session, XrVirtualKeyboardMETA keyboard_handle_xr, const XrVirtualKeyboardSpaceCreateInfoMETA* info, XrSpace* out_space)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_VirtualKeyboard* keyboard = find_keyboard(keyboard_handle_xr);
    if (!keyboard)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_VIRTUAL_KEYBOARD_SPACE_CREATE_INFO_META);
    OXR_CHECK(out_space);
    // Spec XR_META_virtual_keyboard: the session must be the keyboard's
    if (keyboard->session != session_handle(session))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    const XrResult result = vkb_place(keyboard, info->locationType, info->space, info->poseInSpace, 1.0f);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    const OXR_Space space = { .session = keyboard->session, .kind = OXR_SpaceKind::VirtualKeyboard, .keyboard = keyboard_handle(keyboard_handle_xr) };
    *out_space            = space_xr(runtime()->spaces.add(space));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSuggestVirtualKeyboardLocationMETA (XrVirtualKeyboardMETA keyboard_handle_xr, const XrVirtualKeyboardLocationInfoMETA* info)
{
    const OXR_Lock lock(__func__);
    OXR_VirtualKeyboard* keyboard = find_keyboard(keyboard_handle_xr);
    if (!keyboard)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_VIRTUAL_KEYBOARD_LOCATION_INFO_META);
    return vkb_place(keyboard, info->locationType, info->space, info->poseInSpace, info->scale);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetVirtualKeyboardScaleMETA (XrVirtualKeyboardMETA keyboard_handle_xr, float* scale)
{
    const OXR_Lock lock(__func__);
    const OXR_VirtualKeyboard* keyboard = find_keyboard(keyboard_handle_xr);
    if (!keyboard)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(scale);
    *scale = keyboard->scale;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetVirtualKeyboardModelVisibilityMETA (XrVirtualKeyboardMETA keyboard_handle_xr, const XrVirtualKeyboardModelVisibilitySetInfoMETA* info)
{
    const OXR_Lock lock(__func__);
    OXR_VirtualKeyboard* keyboard = find_keyboard(keyboard_handle_xr);
    if (!keyboard)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_VIRTUAL_KEYBOARD_MODEL_VISIBILITY_SET_INFO_META);
    const OXR_Session* sess = oxr_find_session(session_xr(keyboard->session));
    OXR_CHECK(sess);
    vkb_set_visible(session_instance(sess), keyboard, keyboard_handle_xr, info->visible != XR_FALSE);
    return XR_SUCCESS;
}

// Spec XR_META_virtual_keyboard: the states to apply since the last call, by glTF animation index;
// key k is animation k
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetVirtualKeyboardModelAnimationStatesMETA (XrVirtualKeyboardMETA keyboard_handle_xr, XrVirtualKeyboardModelAnimationStatesMETA* states)
{
    const OXR_Lock lock(__func__);
    OXR_VirtualKeyboard* keyboard = find_keyboard(keyboard_handle_xr);
    if (!keyboard)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(states, XR_TYPE_VIRTUAL_KEYBOARD_MODEL_ANIMATION_STATES_META);
    u32 count = 0;
    for (u32 k = 0; k < keyboard->layout.keys_count; k++)
    {
        count += (keyboard->changed_keys >> k) & 1u ? 1u : 0u;
    }
    states->stateCountOutput = count;
    if (states->stateCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    OXR_CHECK(states->states);
    if (states->stateCapacityInput < count)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    u32 written = 0;
    for (u32 k = 0; k < keyboard->layout.keys_count; k++)
    {
        if (keyboard->changed_keys & (1ull << k))
        {
            OXR_CHECK(states->states[written].type == XR_TYPE_VIRTUAL_KEYBOARD_ANIMATION_STATE_META);
            states->states[written].animationIndex = (i32)k;
            states->states[written].fraction       = keyboard->held[k] ? 1.0f : 0.0f;
            written++;
        }
    }
    keyboard->changed_keys = 0;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetVirtualKeyboardDirtyTexturesMETA (XrVirtualKeyboardMETA keyboard_handle_xr, uint32_t capacity, uint32_t* count_output, uint64_t* texture_ids)
{
    const OXR_Lock lock(__func__);
    OXR_VirtualKeyboard* keyboard = find_keyboard(keyboard_handle_xr);
    if (!keyboard)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(count_output);
    *count_output = keyboard->texture_dirty ? 1 : 0;
    if (capacity == 0)
    {
        return XR_SUCCESS;
    }
    OXR_CHECK(texture_ids);
    if (keyboard->texture_dirty)
    {
        texture_ids[0]          = SIM_VKB_TEXTURE_ID;
        keyboard->texture_dirty = false;
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetVirtualKeyboardTextureDataMETA (XrVirtualKeyboardMETA keyboard_handle_xr, uint64_t texture_id, XrVirtualKeyboardTextureDataMETA* data)
{
    const OXR_Lock lock(__func__);
    const OXR_VirtualKeyboard* keyboard = find_keyboard(keyboard_handle_xr);
    if (!keyboard)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(data, XR_TYPE_VIRTUAL_KEYBOARD_TEXTURE_DATA_META);
    OXR_CHECK(texture_id == SIM_VKB_TEXTURE_ID);
    const u32 size          = SIM_VKB_TEXTURE_WIDTH * SIM_VKB_TEXTURE_HEIGHT * 4;
    data->textureWidth      = SIM_VKB_TEXTURE_WIDTH;
    data->textureHeight     = SIM_VKB_TEXTURE_HEIGHT;
    data->bufferCountOutput = size;
    if (data->bufferCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    OXR_CHECK(data->buffer);
    if (data->bufferCapacityInput < size)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    sim_vkb_texture(keyboard->layout, keyboard->shifted, data->buffer);
    return XR_SUCCESS;
}

static b8 vkb_source_is_direct (XrVirtualKeyboardInputSourceMETA source)
{
    return source >= XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_DIRECT_LEFT_META;
}

// Ray sources press the key the ray meets while their button is down; direct sources press the
// key they touch. A direct input near the keys silences its side's ray, and a direct input pushed
// past a pressed key is held on it by moving the interactor root back (poke limiting).
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSendVirtualKeyboardInputMETA (XrVirtualKeyboardMETA keyboard_handle_xr, const XrVirtualKeyboardInputInfoMETA* info, XrPosef* interactor_root)
{
    const OXR_Lock lock(__func__);
    OXR_VirtualKeyboard* keyboard = find_keyboard(keyboard_handle_xr);
    if (!keyboard)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_VIRTUAL_KEYBOARD_INPUT_INFO_META);
    OXR_CHECK(interactor_root);
    OXR_CHECK(info->inputSource >= XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_LEFT_META && info->inputSource <= XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_HAND_DIRECT_INDEX_TIP_RIGHT_META);
    OXR_CHECK((info->inputState & ~XR_VIRTUAL_KEYBOARD_INPUT_STATE_PRESSED_BIT_META) == 0);
    OXR_GET_CONST_SPACE(space, info->inputSpace);
    if (space->session != keyboard->session)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!xr_pose_valid(info->inputPoseInSpace) || !xr_pose_valid(*interactor_root))
    {
        return XR_ERROR_POSE_INVALID;
    }
    const OXR_Session* sess = oxr_find_session(session_xr(keyboard->session));
    OXR_CHECK(sess);
    if (!keyboard->visible)
    {
        return XR_SUCCESS;
    }

    const SIM_Sim* sim    = runtime()->sim;
    SIM_Sample     sample = {};
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    const OXR_Located located = locate_world(space, sample);
    if (!located.valid)
    {
        return XR_SUCCESS;
    }
    const u32  source = (u32)info->inputSource - 1;
    const u32  side   = source % 2;
    const b8   direct = vkb_source_is_direct(info->inputSource);
    const Pose input  = zm::pose_mul(located.pose, pose_from_xr(info->inputPoseInSpace));
    // In keyboard space at scale 1
    const Pose local  = zm::pose_mul(zm::pose_inverse(keyboard->pose), input);
    const Vec3 point  = local.position / keyboard->scale;
    const Vec2 half   = keyboard->layout.half;

    i32 key     = -1;
    b8  pressed = false;
    if (direct)
    {
        const b8 near = zm::abs(point.x) <= half.x + OXR_VKB_NEAR_MARGIN && zm::abs(point.y) <= half.y + OXR_VKB_NEAR_MARGIN && point.z <= OXR_VKB_NEAR_FRONT && point.z >= -OXR_VKB_NEAR_BEHIND;
        keyboard->direct_near[side] = near;
        pressed = near && point.z <= SIM_VKB_KEY_HEIGHT;
        key     = pressed ? sim_vkb_key_at(keyboard->layout, { point.x, point.y }) : -1;
        const f32 floor = SIM_VKB_KEY_HEIGHT - SIM_VKB_PRESS;
        if (pressed && point.z < floor)
        {
            const Vec3 push_stage = zm::rotate(keyboard->pose.orientation, { 0.0f, 0.0f, (floor - point.z) * keyboard->scale });
            const Vec3 push_input = zm::rotate(zm::inverse(located.pose.orientation), push_stage);
            interactor_root->position.x += push_input.x;
            interactor_root->position.y += push_input.y;
            interactor_root->position.z += push_input.z;
        }
    }
    else if (!keyboard->direct_near[side])
    {
        const Vec3 direction = zm::rotate(local.orientation, { 0.0f, 0.0f, -1.0f });
        pressed              = (info->inputState & XR_VIRTUAL_KEYBOARD_INPUT_STATE_PRESSED_BIT_META) != 0;
        if (direction.z < 0.0f && point.z > SIM_VKB_KEY_HEIGHT)
        {
            const f32  t   = (SIM_VKB_KEY_HEIGHT - point.z) / direction.z;
            const Vec3 hit = point + direction * t;
            key            = sim_vkb_key_at(keyboard->layout, { hit.x, hit.y });
        }
    }

    // A press starts on the key under the input and stays on it until released
    if (pressed && !keyboard->was_pressed[source] && key >= 0)
    {
        keyboard->pressing[source] = key;
        vkb_press_key(session_instance(sess), keyboard, keyboard_handle_xr, (u32)key);
    }
    if (!pressed)
    {
        keyboard->pressing[source] = -1;
    }
    keyboard->was_pressed[source] = pressed;
    for (u32 k = 0; k < keyboard->layout.keys_count; k++)
    {
        b8 held = false;
        for (const i32 holding : keyboard->pressing)
        {
            held |= holding == (i32)k;
        }
        if (held != keyboard->held[k])
        {
            keyboard->held[k]       = held;
            keyboard->changed_keys |= 1ull << k;
        }
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrChangeVirtualKeyboardTextContextMETA (XrVirtualKeyboardMETA keyboard_handle_xr, const XrVirtualKeyboardTextContextChangeInfoMETA* info)
{
    const OXR_Lock lock(__func__);
    OXR_VirtualKeyboard* keyboard = find_keyboard(keyboard_handle_xr);
    if (!keyboard)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_VIRTUAL_KEYBOARD_TEXT_CONTEXT_CHANGE_INFO_META);
    OXR_CHECK(info->textContext);
    // Only the end matters: it is the text just before the cursor
    const usize length = cstr_length(info->textContext);
    usize       keep   = length < OXR_VKB_MAX_CONTEXT - 1 ? length : OXR_VKB_MAX_CONTEXT - 1;
    while (keep > 0 && keep < length && ((u8)info->textContext[length - keep] & 0xC0) == 0x80)
    {
        keep--;
    }
    cstr_copy(keyboard->context, sizeof(keyboard->context), info->textContext + (length - keep));
    return XR_SUCCESS;
}

//
// Hand tracking
//

#define OXR_PINCHING 0.9f // XR_FB_hand_tracking_aim reports a pinch from this strength on

// Meta's preview header meta_hand_tracking_wide_motion_mode.h, not in the OpenXR headers
#define OXR_TYPE_HAND_TRACKING_WIDE_MOTION_MODE_INFO_META                  1000539000u // an XrStructureType the headers do not name
#define OXR_HAND_TRACKING_WIDE_MOTION_MODE_HIGH_FIDELITY_BODY_TRACKING_META 1u

namespace
{

struct OXR_HandTrackingWideMotionModeInfoMETA
{
    XrStructureType type;
    const void*     next;
    u32             requestedWideMotionMode;
};

} // namespace

static OXR_HandTracker* find_hand_tracker (XrHandTrackerEXT tracker)
{
    return runtime()->hand_trackers.try_get(hand_tracker_handle(tracker));
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateHandTrackerEXT (XrSession session, const XrHandTrackerCreateInfoEXT* info, XrHandTrackerEXT* out_tracker)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    OXR_CHECK_TYPE(info, XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT);
    OXR_CHECK(out_tracker);
    OXR_CHECK(info->hand == XR_HAND_LEFT_EXT || info->hand == XR_HAND_RIGHT_EXT);
    OXR_CHECK(info->handJointSet == XR_HAND_JOINT_SET_DEFAULT_EXT);

    OXR_HandTracker tracker = { .session = session_handle(session), .hand = info->hand == XR_HAND_LEFT_EXT ? SIM_Device::LeftHand : SIM_Device::RightHand, .unobstructed = true, .wide_motion = inst->ext.META_hand_tracking_wide_motion_mode2 };
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)info->next; next; next = next->next)
    {
        if ((u32)next->type == OXR_TYPE_HAND_TRACKING_WIDE_MOTION_MODE_INFO_META && inst->ext.META_hand_tracking_wide_motion_mode)
        {
            OXR_CHECK(((const OXR_HandTrackingWideMotionModeInfoMETA*)next)->requestedWideMotionMode == OXR_HAND_TRACKING_WIDE_MOTION_MODE_HIGH_FIDELITY_BODY_TRACKING_META);
            tracker.wide_motion = true;
            continue;
        }
        if (next->type != XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT || !inst->ext.EXT_hand_tracking_data_source)
        {
            continue;
        }
        // Spec XR_EXT_hand_tracking_data_source: at least one source, none twice
        const XrHandTrackingDataSourceInfoEXT* data = (const XrHandTrackingDataSourceInfoEXT*)next;
        OXR_CHECK(data->requestedDataSourceCount > 0 && data->requestedDataSources);
        tracker.unobstructed = false;
        for (u32 i = 0; i < data->requestedDataSourceCount; i++)
        {
            const XrHandTrackingDataSourceEXT source = data->requestedDataSources[i];
            OXR_CHECK(source == XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT || source == XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT);
            b8& requested = source == XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT ? tracker.unobstructed : tracker.controller;
            OXR_CHECK(!requested);
            requested = true;
        }
    }
    *out_tracker = hand_tracker_xr(runtime()->hand_trackers.add(tracker));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyHandTrackerEXT (XrHandTrackerEXT tracker)
{
    const OXR_Lock lock(__func__);
    if (!find_hand_tracker(tracker))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->hand_trackers.remove(hand_tracker_handle(tracker));
    return XR_SUCCESS;
}

namespace
{

// The structs an app may chain to XrHandJointLocationsEXT
struct OXR_HandOutputs
{
    XrHandJointVelocitiesEXT*         velocities;
    XrHandTrackingAimStateFB*         aim;
    XrHandTrackingCapsulesStateFB*    capsules;
    XrHandTrackingScaleFB*            scale;
    XrHandTrackingDataSourceStateEXT* data_source;
    XrHandTrackingUnextrapolatedPosesMETA* unextrapolated; // only when requested
};

} // namespace

static XrResult hand_outputs (const OXR_Instance* inst, const XrHandJointLocationsEXT* locations, OXR_HandOutputs& out)
{
    out = {};
    for (XrBaseOutStructure* next = (XrBaseOutStructure*)locations->next; next; next = next->next)
    {
        switch (next->type)
        {
        case XR_TYPE_HAND_JOINT_VELOCITIES_EXT:
            out.velocities = (XrHandJointVelocitiesEXT*)next;
            OXR_CHECK(out.velocities->jointCount == XR_HAND_JOINT_COUNT_EXT && out.velocities->jointVelocities);
            break;
        case XR_TYPE_HAND_TRACKING_AIM_STATE_FB:
            out.aim = inst->ext.FB_hand_tracking_aim ? (XrHandTrackingAimStateFB*)next : nullptr;
            break;
        case XR_TYPE_HAND_TRACKING_CAPSULES_STATE_FB:
            out.capsules = inst->ext.FB_hand_tracking_capsules ? (XrHandTrackingCapsulesStateFB*)next : nullptr;
            break;
        case XR_TYPE_HAND_TRACKING_SCALE_FB:
            out.scale = inst->ext.FB_hand_tracking_mesh ? (XrHandTrackingScaleFB*)next : nullptr;
            break;
        case XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT:
            out.data_source = inst->ext.EXT_hand_tracking_data_source ? (XrHandTrackingDataSourceStateEXT*)next : nullptr;
            break;
        case XR_TYPE_HAND_TRACKING_UNEXTRAPOLATED_POSES_META:
            out.unextrapolated = inst->ext.META_hand_tracking_unextrapolated_poses ? (XrHandTrackingUnextrapolatedPosesMETA*)next : nullptr;
            break;
        default:
            break;
        }
    }
    return XR_SUCCESS;
}

static void clear_hand_outputs (XrHandJointLocationsEXT* locations, const OXR_HandOutputs& out)
{
    locations->isActive = XR_FALSE;
    for (u32 j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++)
    {
        locations->jointLocations[j] = { .locationFlags = 0, .pose = pose_to_xr(zm::POSE_IDENTITY), .radius = 0.0f };
        if (out.velocities)
        {
            out.velocities->jointVelocities[j] = {};
        }
    }
    if (out.aim)
    {
        out.aim->status              = 0;
        out.aim->aimPose             = pose_to_xr(zm::POSE_IDENTITY);
        out.aim->pinchStrengthIndex  = 0.0f;
        out.aim->pinchStrengthMiddle = 0.0f;
        out.aim->pinchStrengthRing   = 0.0f;
        out.aim->pinchStrengthLittle = 0.0f;
    }
    if (out.capsules)
    {
        memory_zero(out.capsules->capsules, sizeof(out.capsules->capsules));
    }
    if (out.data_source)
    {
        out.data_source->isActive = XR_FALSE;
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrLocateHandJointsEXT (XrHandTrackerEXT hand_tracker, const XrHandJointsLocateInfoEXT* info, XrHandJointLocationsEXT* locations)
{
    const OXR_Lock         lock(__func__);
    const OXR_HandTracker* tracker = find_hand_tracker(hand_tracker);
    if (!tracker)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_Runtime*       rt   = runtime();
    const OXR_Session* sess = rt->sessions.try_get(tracker->session);
    OXR_CHECK(sess);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    OXR_CHECK_TYPE(info, XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT);
    OXR_CHECK_TYPE(locations, XR_TYPE_HAND_JOINT_LOCATIONS_EXT);
    OXR_CHECK(locations->jointCount == XR_HAND_JOINT_COUNT_EXT && locations->jointLocations);
    OXR_GET_CONST_SPACE(base, info->baseSpace);
    OXR_CHECK(base->session == tracker->session);
    const XrTime time = locate_time(info->time);
    if (time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }

    SIM_HandMotion motion         = SIM_HandMotion::Unobstructed;
    b8             unextrapolated = false;
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)info->next; next; next = next->next)
    {
        unextrapolated = unextrapolated || (next->type == XR_TYPE_HAND_TRACKING_UNEXTRAPOLATED_POSES_REQUEST_META && inst->ext.META_hand_tracking_unextrapolated_poses);
        if (next->type == XR_TYPE_HAND_JOINTS_MOTION_RANGE_INFO_EXT && inst->ext.EXT_hand_joints_motion_range)
        {
            const XrHandJointsMotionRangeEXT range = ((const XrHandJointsMotionRangeInfoEXT*)next)->handJointsMotionRange;
            OXR_CHECK(range == XR_HAND_JOINTS_MOTION_RANGE_UNOBSTRUCTED_EXT || range == XR_HAND_JOINTS_MOTION_RANGE_CONFORMING_TO_CONTROLLER_EXT);
            motion = range == XR_HAND_JOINTS_MOTION_RANGE_CONFORMING_TO_CONTROLLER_EXT ? SIM_HandMotion::ConformingToController : SIM_HandMotion::Unobstructed;
        }
    }
    OXR_HandOutputs out = {};
    const XrResult  outputs_result = hand_outputs(inst, locations, out);
    if (outputs_result != XR_SUCCESS)
    {
        return outputs_result;
    }
    // Spec XR_META_hand_tracking_unextrapolated_poses: left alone unless requested, zero without poses
    out.unextrapolated = unextrapolated ? out.unextrapolated : nullptr;
    if (out.unextrapolated)
    {
        out.unextrapolated->captureTime = 0;
    }
    if (out.scale)
    {
        out.scale->sensorOutput  = 1.0f;
        out.scale->currentOutput = out.scale->overrideHandScale ? out.scale->overrideValueInput : 1.0f;
    }

    const SIM_Sim*       sim    = rt->sim;
    const SIM_Device     hand   = tracker->hand;
    const SIM_HandSource source = sim_get_hand_source(sim, hand);
    SIM_Sample           sample = {};
    sim_sample(sim, time, sample);
    // Spec XR_META_hand_tracking_unextrapolated_poses: the time still places the base space. The
    // simulation computes the hands for the predicted display time, so that is when they are captured.
    SIM_Sample latest = sample;
    if (unextrapolated)
    {
        sim_sample(sim, sim_predicted_display_time(sim), latest);
    }
    const SIM_DeviceState& device  = latest.devices[(u32)hand];
    const b8               wanted  = source == SIM_HandSource::Hand ? tracker->unobstructed : tracker->controller;
    const OXR_Located      located = locate_world(base, sample);
    const b8               seen    = source == SIM_HandSource::Controller || sim_hand_in_view(sim, latest, hand);
    clear_hand_outputs(locations, out);
    if (!wanted || !device.active || !located.valid || (!seen && !tracker->wide_motion))
    {
        return XR_SUCCESS;
    }
    const b8 tracked = device.tracked && seen;

    SIM_HandJoints joints = {};
    sim_hand_joints(sim, hand, device.pose, motion, joints);
    locations->isActive = XR_TRUE;
    for (u32 j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++)
    {
        const Pose&       pose   = joints.poses[j];
        const OXR_Located target = { .pose = pose, .linear_velocity = device.linear_velocity + zm::cross(device.angular_velocity, pose.position - device.pose.position), .angular_velocity = device.angular_velocity, .valid = true, .tracked = tracked };
        XrSpaceVelocity   velocity = { .type = XR_TYPE_SPACE_VELOCITY };
        XrHandJointLocationEXT& location = locations->jointLocations[j];
        locate_relative(target, located, location.pose, location.locationFlags, &velocity);
        location.radius = joints.radii[j];
        if (out.velocities)
        {
            out.velocities->jointVelocities[j] = { .velocityFlags = velocity.velocityFlags, .linearVelocity = velocity.linearVelocity, .angularVelocity = velocity.angularVelocity };
        }
    }

    if (out.data_source)
    {
        out.data_source->isActive   = XR_TRUE;
        out.data_source->dataSource = source == SIM_HandSource::Hand ? XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT : XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT;
    }
    if (out.aim)
    {
        const f32 pinch = sim_get_input(sim, hand, SIM_Input::TriggerValue).value.x;
        const b8  menu  = sim_get_input(sim, hand, SIM_Input::MenuClick).value.x > 0.5f;
        XrHandTrackingAimFlagsFB status = XR_HAND_TRACKING_AIM_COMPUTED_BIT_FB | XR_HAND_TRACKING_AIM_VALID_BIT_FB;
        status |= pinch >= OXR_PINCHING ? XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB : 0;
        status |= hand == sim_get_config(sim).primary_hand ? XR_HAND_TRACKING_AIM_DOMINANT_HAND_BIT_FB : 0;
        // Quest: the left palm-up pinch opens the app menu, the right one the system menu
        status |= menu ? (hand == SIM_Device::LeftHand ? XR_HAND_TRACKING_AIM_MENU_PRESSED_BIT_FB : XR_HAND_TRACKING_AIM_SYSTEM_GESTURE_BIT_FB) : 0;
        const OXR_Located aim  = { .pose = sim_sample_hand_pose(sim, latest, hand, SIM_HandPose::Aim), .valid = true, .tracked = tracked };
        XrSpaceLocationFlags flags = 0;
        locate_relative(aim, located, out.aim->aimPose, flags, nullptr);
        out.aim->status             = status;
        out.aim->pinchStrengthIndex = pinch;
    }
    if (out.capsules)
    {
        u32 c = 0;
        for (u32 j = XR_HAND_JOINT_THUMB_METACARPAL_EXT; j < XR_HAND_JOINT_COUNT_EXT && c < XR_HAND_TRACKING_CAPSULE_COUNT_FB; j++)
        {
            const b8 tip = j == XR_HAND_JOINT_THUMB_TIP_EXT || j == XR_HAND_JOINT_INDEX_TIP_EXT || j == XR_HAND_JOINT_MIDDLE_TIP_EXT || j == XR_HAND_JOINT_RING_TIP_EXT || j == XR_HAND_JOINT_LITTLE_TIP_EXT;
            if (tip)
            {
                continue;
            }
            XrHandCapsuleFB& capsule = out.capsules->capsules[c++];
            capsule.points[0]        = locations->jointLocations[j].pose.position;
            capsule.points[1]        = locations->jointLocations[j + 1].pose.position;
            capsule.radius           = joints.radii[j];
            capsule.joint            = (XrHandJointEXT)j;
        }
    }
    if (out.unextrapolated)
    {
        out.unextrapolated->captureTime = latest.time;
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetHandTrackingFrequencyHintMETA (XrSession session, XrHandTrackingFrequencyHintMETA hint)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK(hint == XR_HAND_TRACKING_FREQUENCY_HINT_DEFAULT_META || hint == XR_HAND_TRACKING_FREQUENCY_HINT_HIGH_META);
    sess->hand_frequency_hint = hint;
    return XR_SUCCESS;
}

// XR_META_simultaneous_hands_and_controllers: each simulated hand already switches between its
// controller and hand tracking alone (SIM_HandSource), so the mode has nothing to change
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrResumeSimultaneousHandsAndControllersTrackingMETA (XrSession session, const XrSimultaneousHandsAndControllersTrackingResumeInfoMETA* info)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_RESUME_INFO_META);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrPauseSimultaneousHandsAndControllersTrackingMETA (XrSession session, const XrSimultaneousHandsAndControllersTrackingPauseInfoMETA* info)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_PAUSE_INFO_META);
    return XR_SUCCESS;
}

// Spec XR_FB_hand_tracking_mesh: zero capacities ask for the counts
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetHandMeshFB (XrHandTrackerEXT hand_tracker, XrHandTrackingMeshFB* mesh)
{
    const OXR_Lock         lock(__func__);
    const OXR_HandTracker* tracker = find_hand_tracker(hand_tracker);
    if (!tracker)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(mesh, XR_TYPE_HAND_TRACKING_MESH_FB);
    const SIM_HandMesh& source = sim_get_hand_mesh(runtime()->sim, tracker->hand);
    mesh->jointCountOutput     = XR_HAND_JOINT_COUNT_EXT;
    mesh->vertexCountOutput    = source.vertices_count;
    mesh->indexCountOutput     = source.indices_count;
    if (mesh->jointCapacityInput == 0 && mesh->vertexCapacityInput == 0 && mesh->indexCapacityInput == 0)
    {
        return XR_SUCCESS;
    }
    if (mesh->jointCapacityInput < XR_HAND_JOINT_COUNT_EXT || mesh->vertexCapacityInput < source.vertices_count || mesh->indexCapacityInput < source.indices_count)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    OXR_CHECK(mesh->jointBindPoses && mesh->jointRadii && mesh->jointParents);
    OXR_CHECK(mesh->vertexPositions && mesh->vertexNormals && mesh->vertexUVs && mesh->vertexBlendIndices && mesh->vertexBlendWeights && mesh->indices);
    // The root's parent is no joint: OVRPlugin's skeletons (Unity's OVRSkeleton, Unreal's hand
    // component) take an index outside the joints as none, the wrist as its own parent literally
    for (u32 j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++)
    {
        mesh->jointBindPoses[j] = pose_to_xr(source.bind_poses[j]);
        mesh->jointRadii[j]     = source.radii[j];
        mesh->jointParents[j]   = j == XR_HAND_JOINT_WRIST_EXT ? XR_HAND_JOINT_MAX_ENUM_EXT : (XrHandJointEXT)source.parents[j];
    }
    for (u32 v = 0; v < source.vertices_count; v++)
    {
        const Vec3 p = source.positions[v];
        const Vec3 n = source.normals[v];
        mesh->vertexPositions[v]    = { p.x, p.y, p.z };
        mesh->vertexNormals[v]      = { n.x, n.y, n.z };
        mesh->vertexUVs[v]          = { source.uvs[v].x, source.uvs[v].y };
        // Unused slots repeat the joint: OVRPlugin maps every slot, and the palm (0) has no bone
        const i16 joint             = (i16)source.joints[v];
        mesh->vertexBlendIndices[v] = { joint, joint, joint, joint };
        mesh->vertexBlendWeights[v] = { 1.0f, 0.0f, 0.0f, 0.0f };
    }
    memory_copy(mesh->indices, source.indices, source.indices_count * sizeof(i16));
    return XR_SUCCESS;
}

//
// Body tracking (docs/body-tracking.md)
//

static OXR_BodyTracker* find_body_tracker (XrBodyTrackerFB tracker)
{
    return runtime()->body_trackers.try_get(body_tracker_handle(tracker));
}

static u32 body_joint_count (const OXR_BodyTracker* tracker)
{
    return tracker->full_body ? (u32)XR_FULL_BODY_JOINT_COUNT_META : (u32)XR_BODY_JOINT_COUNT_FB;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateBodyTrackerFB (XrSession session, const XrBodyTrackerCreateInfoFB* info, XrBodyTrackerFB* out_tracker)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    OXR_CHECK_TYPE(info, XR_TYPE_BODY_TRACKER_CREATE_INFO_FB);
    OXR_CHECK(out_tracker);
    const b8 full_body = info->bodyJointSet == XR_BODY_JOINT_SET_FULL_BODY_META;
    OXR_CHECK(info->bodyJointSet == XR_BODY_JOINT_SET_DEFAULT_FB || (full_body && inst->ext.META_body_tracking_full_body));
    const OXR_BodyTracker tracker = { .session = session_handle(session), .full_body = full_body };
    *out_tracker                  = body_tracker_xr(runtime()->body_trackers.add(tracker));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyBodyTrackerFB (XrBodyTrackerFB tracker)
{
    const OXR_Lock lock(__func__);
    if (!find_body_tracker(tracker))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->body_trackers.remove(body_tracker_handle(tracker));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrLocateBodyJointsFB (XrBodyTrackerFB body_tracker, const XrBodyJointsLocateInfoFB* info, XrBodyJointLocationsFB* locations)
{
    const OXR_Lock         lock(__func__);
    const OXR_BodyTracker* tracker = find_body_tracker(body_tracker);
    if (!tracker)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_Runtime*       rt   = runtime();
    const OXR_Session* sess = rt->sessions.try_get(tracker->session);
    OXR_CHECK(sess);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    OXR_CHECK_TYPE(info, XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB);
    OXR_CHECK_TYPE(locations, XR_TYPE_BODY_JOINT_LOCATIONS_FB);
    const u32 count = body_joint_count(tracker);
    OXR_CHECK(locations->jointCount == count && locations->jointLocations);
    OXR_GET_CONST_SPACE(base, info->baseSpace);
    OXR_CHECK(base->session == tracker->session);
    const XrTime time = locate_time(info->time);
    if (time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }

    const SIM_Sim* sim    = rt->sim;
    SIM_Sample     sample = {};
    sim_sample(sim, time, sample);
    const OXR_Located located = locate_world(base, sample);
    locations->isActive             = XR_FALSE;
    locations->confidence           = 0.0f;
    locations->skeletonChangedCount = sim_body_skeleton_changes(sim, time);
    locations->time                 = time;
    for (XrBaseOutStructure* next = (XrBaseOutStructure*)locations->next; next; next = next->next)
    {
        if (next->type == XR_TYPE_BODY_TRACKING_FIDELITY_STATUS_META && inst->ext.META_body_tracking_fidelity)
        {
            ((XrBodyTrackingFidelityStatusMETA*)next)->fidelity = tracker->low_fidelity ? XR_BODY_TRACKING_FIDELITY_LOW_META : XR_BODY_TRACKING_FIDELITY_HIGH_META;
        }
        else if (next->type == XR_TYPE_BODY_TRACKING_CALIBRATION_STATUS_META && inst->ext.META_body_tracking_calibration)
        {
            ((XrBodyTrackingCalibrationStatusMETA*)next)->status = XR_BODY_TRACKING_CALIBRATION_STATE_VALID_META;
        }
    }
    for (u32 j = 0; j < count; j++)
    {
        locations->jointLocations[j] = { .locationFlags = 0, .pose = pose_to_xr(zm::POSE_IDENTITY) };
    }
    if (!sample.devices[(u32)SIM_Device::Head].active || !located.valid)
    {
        return XR_SUCCESS;
    }

    SIM_BodyJoints joints = {};
    sim_body_joints(sim, sample, joints, !tracker->low_fidelity);
    // Spec XR_FB_body_tracking: while active every joint is valid, tracked or not
    locations->isActive   = XR_TRUE;
    locations->confidence = joints.confidence;
    for (u32 j = 0; j < count; j++)
    {
        const OXR_Located target = { .pose = joints.poses[j], .valid = true, .tracked = joints.tracked[j] && sample.devices[(u32)SIM_Device::Head].tracked };
        XrBodyJointLocationFB& location = locations->jointLocations[j];
        locate_relative(target, located, location.pose, location.locationFlags, nullptr);
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetBodySkeletonFB (XrBodyTrackerFB body_tracker, XrBodySkeletonFB* skeleton) // NOLINT(misc-const-correctness): the signature of the OpenXR function
{
    const OXR_Lock         lock(__func__);
    const OXR_BodyTracker* tracker = find_body_tracker(body_tracker);
    if (!tracker)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(skeleton, XR_TYPE_BODY_SKELETON_FB);
    const u32 count = body_joint_count(tracker);
    OXR_CHECK(skeleton->jointCount == count && skeleton->joints);

    SIM_BodySkeleton source = {};
    sim_body_skeleton(runtime()->sim, source);
    // -1 in both joint sets, as a Quest 3 reports, not XR_FULL_BODY_JOINT_NONE_META
    const i32 none = (i32)XR_BODY_JOINT_NONE_FB;
    for (u32 j = 0; j < count; j++)
    {
        skeleton->joints[j] = { .joint = (i32)j, .parentJoint = source.parents[j] == SIM_BODY_NO_PARENT ? none : (i32)source.parents[j], .pose = pose_to_xr(source.poses[j]) };
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrRequestBodyTrackingFidelityMETA (XrBodyTrackerFB body_tracker, const XrBodyTrackingFidelityMETA fidelity)
{
    const OXR_Lock   lock(__func__);
    OXR_BodyTracker* tracker = find_body_tracker(body_tracker);
    if (!tracker)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK(fidelity == XR_BODY_TRACKING_FIDELITY_LOW_META || fidelity == XR_BODY_TRACKING_FIDELITY_HIGH_META);
    tracker->low_fidelity = fidelity == XR_BODY_TRACKING_FIDELITY_LOW_META;
    return XR_SUCCESS;
}

// The height reshapes the simulated body, which the recording carries, so a replay sees it too
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSuggestBodyTrackingCalibrationOverrideMETA (XrBodyTrackerFB body_tracker, const XrBodyTrackingCalibrationInfoMETA* calibration)
{
    const OXR_Lock lock(__func__);
    if (!find_body_tracker(body_tracker))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(calibration, XR_TYPE_BODY_TRACKING_CALIBRATION_INFO_META);
    // Spec XR_META_body_tracking_calibration: a height outside (0.5 m, 3 m) is a validation failure
    OXR_CHECK(zm::is_finite(calibration->bodyHeight) && calibration->bodyHeight >= SIM_BODY_MIN_HEIGHT && calibration->bodyHeight <= SIM_BODY_MAX_HEIGHT);
    SIM_Sim*      sim  = runtime()->sim;
    SIM_BodyState body = sim_get_body(sim);
    body.height        = calibration->bodyHeight;
    sim_set_body(sim, body);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrResetBodyTrackingCalibrationMETA (XrBodyTrackerFB body_tracker)
{
    const OXR_Lock lock(__func__);
    if (!find_body_tracker(body_tracker))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    SIM_Sim*      sim  = runtime()->sim;
    SIM_BodyState body = sim_get_body(sim);
    body.height        = sim_get_config(sim).body.height;
    sim_set_body(sim, body);
    return XR_SUCCESS;
}

//
// Face and eye tracking (docs/face-tracking.md)
//

static OXR_FaceTracker* find_face_tracker (XrFaceTrackerFB tracker)
{
    return runtime()->face_trackers.try_get(face_tracker_handle(tracker));
}

static OXR_FaceTracker* find_face_tracker2 (XrFaceTracker2FB tracker)
{
    return runtime()->face_trackers2.try_get(face_tracker2_handle(tracker));
}

static OXR_EyeTracker* find_eye_tracker (XrEyeTrackerFB tracker)
{
    return runtime()->eye_trackers.try_get(eye_tracker_handle(tracker));
}

// The face at `time`; false while the headset is not tracked
static b8 face_at (XrTime time, SIM_FaceFrame& out_frame)
{
    const SIM_Sim* sim    = runtime()->sim;
    SIM_Sample     sample = {};
    sim_sample(sim, time, sample);
    sim_face_frame(sim, sample, out_frame);
    return sample.devices[(u32)SIM_Device::Head].active && sample.devices[(u32)SIM_Device::Head].tracked;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateFaceTracker2FB (XrSession session, const XrFaceTrackerCreateInfo2FB* info, XrFaceTracker2FB* out_tracker)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_FACE_TRACKER_CREATE_INFO2_FB);
    OXR_CHECK(out_tracker);
    OXR_CHECK(info->faceExpressionSet == XR_FACE_EXPRESSION_SET2_DEFAULT_FB);
    OXR_CHECK(info->requestedDataSourceCount == 0 || info->requestedDataSources);
    b8 visual = info->requestedDataSourceCount == 0;
    b8 audio  = false;
    for (u32 i = 0; i < info->requestedDataSourceCount; i++)
    {
        const XrFaceTrackingDataSource2FB source = info->requestedDataSources[i];
        OXR_CHECK(source == XR_FACE_TRACKING_DATA_SOURCE2_VISUAL_FB || source == XR_FACE_TRACKING_DATA_SOURCE2_AUDIO_FB);
        b8& requested = source == XR_FACE_TRACKING_DATA_SOURCE2_VISUAL_FB ? visual : audio;
        OXR_CHECK(!requested);
        requested = true;
    }
    // The simulated face is seen by the cameras; audio only when that is all the app asked for
    const OXR_FaceTracker tracker = { .session = session_handle(session), .audio = !visual };
    *out_tracker                  = face_tracker2_xr(runtime()->face_trackers2.add(tracker));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyFaceTracker2FB (XrFaceTracker2FB tracker)
{
    const OXR_Lock lock(__func__);
    if (!find_face_tracker2(tracker))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->face_trackers2.remove(face_tracker2_handle(tracker));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetFaceExpressionWeights2FB (XrFaceTracker2FB face_tracker, const XrFaceExpressionInfo2FB* info, XrFaceExpressionWeights2FB* weights)
{
    const OXR_Lock         lock(__func__);
    const OXR_FaceTracker* tracker = find_face_tracker2(face_tracker);
    if (!tracker)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    const OXR_Session* sess = runtime()->sessions.try_get(tracker->session);
    OXR_CHECK(sess);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    OXR_CHECK_TYPE(info, XR_TYPE_FACE_EXPRESSION_INFO2_FB);
    OXR_CHECK_TYPE(weights, XR_TYPE_FACE_EXPRESSION_WEIGHTS2_FB);
    OXR_CHECK(weights->weightCount == XR_FACE_EXPRESSION2_COUNT_FB && weights->weights);
    OXR_CHECK(weights->confidenceCount == XR_FACE_CONFIDENCE2_COUNT_FB && weights->confidences);
    const XrTime time = locate_time(info->time);
    if (time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    SIM_FaceFrame frame = {};
    const b8      valid = face_at(time, frame);
    memory_copy(weights->weights, frame.weights, sizeof(frame.weights));
    memory_copy(weights->confidences, frame.confidences, sizeof(frame.confidences));
    if (!valid)
    {
        memory_zero(weights->weights, sizeof(frame.weights));
        memory_zero(weights->confidences, sizeof(frame.confidences));
    }
    weights->isValid                        = valid ? XR_TRUE : XR_FALSE;
    weights->isEyeFollowingBlendshapesValid = valid ? XR_TRUE : XR_FALSE;
    weights->dataSource                     = tracker->audio ? XR_FACE_TRACKING_DATA_SOURCE2_AUDIO_FB : XR_FACE_TRACKING_DATA_SOURCE2_VISUAL_FB;
    weights->time                           = time;
    for (XrBaseOutStructure* next = (XrBaseOutStructure*)weights->next; next; next = next->next)
    {
        if (next->type == XR_TYPE_FACE_TRACKING_VISEMES_META && inst->ext.META_face_tracking_visemes)
        {
            XrFaceTrackingVisemesMETA* visemes = (XrFaceTrackingVisemesMETA*)next;
            visemes->isValid                   = valid ? XR_TRUE : XR_FALSE;
            for (u32 v = 0; v < XR_FACE_TRACKING_VISEME_COUNT_META; v++)
            {
                visemes->visemes[v] = valid ? frame.visemes[v] : 0.0f;
            }
        }
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateFaceTrackerFB (XrSession session, const XrFaceTrackerCreateInfoFB* info, XrFaceTrackerFB* out_tracker)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_FACE_TRACKER_CREATE_INFO_FB);
    OXR_CHECK(out_tracker);
    OXR_CHECK(info->faceExpressionSet == XR_FACE_EXPRESSION_SET_DEFAULT_FB);
    const OXR_FaceTracker tracker = { .session = session_handle(session) };
    *out_tracker                  = face_tracker_xr(runtime()->face_trackers.add(tracker));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyFaceTrackerFB (XrFaceTrackerFB tracker)
{
    const OXR_Lock lock(__func__);
    if (!find_face_tracker(tracker))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->face_trackers.remove(face_tracker_handle(tracker));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetFaceExpressionWeightsFB (XrFaceTrackerFB face_tracker, const XrFaceExpressionInfoFB* info, XrFaceExpressionWeightsFB* weights)
{
    const OXR_Lock lock(__func__);
    if (!find_face_tracker(face_tracker))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_FACE_EXPRESSION_INFO_FB);
    OXR_CHECK_TYPE(weights, XR_TYPE_FACE_EXPRESSION_WEIGHTS_FB);
    OXR_CHECK(weights->weightCount == XR_FACE_EXPRESSION_COUNT_FB && weights->weights);
    OXR_CHECK(weights->confidenceCount == XR_FACE_CONFIDENCE_COUNT_FB && weights->confidences);
    const XrTime time = locate_time(info->time);
    if (time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    SIM_FaceFrame frame = {};
    const b8      valid = face_at(time, frame);
    for (u32 e = 0; e < XR_FACE_EXPRESSION_COUNT_FB; e++)
    {
        weights->weights[e] = valid ? frame.weights[e] : 0.0f;
    }
    for (u32 c = 0; c < XR_FACE_CONFIDENCE_COUNT_FB; c++)
    {
        weights->confidences[c] = valid ? frame.confidences[c] : 0.0f;
    }
    weights->status = { .isValid = valid ? (XrBool32)XR_TRUE : (XrBool32)XR_FALSE, .isEyeFollowingBlendshapesValid = valid ? (XrBool32)XR_TRUE : (XrBool32)XR_FALSE };
    weights->time   = time;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateEyeTrackerFB (XrSession session, const XrEyeTrackerCreateInfoFB* info, XrEyeTrackerFB* out_tracker)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(info, XR_TYPE_EYE_TRACKER_CREATE_INFO_FB);
    OXR_CHECK(out_tracker);
    const OXR_EyeTracker tracker = { .session = session_handle(session) };
    *out_tracker                 = eye_tracker_xr(runtime()->eye_trackers.add(tracker));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyEyeTrackerFB (XrEyeTrackerFB tracker)
{
    const OXR_Lock lock(__func__);
    if (!find_eye_tracker(tracker))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->eye_trackers.remove(eye_tracker_handle(tracker));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetEyeGazesFB (XrEyeTrackerFB eye_tracker, const XrEyeGazesInfoFB* info, XrEyeGazesFB* gazes)
{
    const OXR_Lock        lock(__func__);
    const OXR_EyeTracker* tracker = find_eye_tracker(eye_tracker);
    if (!tracker)
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    OXR_CHECK_TYPE(info, XR_TYPE_EYE_GAZES_INFO_FB);
    OXR_CHECK_TYPE(gazes, XR_TYPE_EYE_GAZES_FB);
    OXR_GET_CONST_SPACE(base, info->baseSpace);
    OXR_CHECK(base->session == tracker->session);
    const XrTime time = locate_time(info->time);
    if (time <= 0)
    {
        return XR_ERROR_TIME_INVALID;
    }
    const SIM_Sim* sim    = runtime()->sim;
    SIM_Sample     sample = {};
    sim_sample(sim, time, sample);
    SIM_FaceFrame frame = {};
    sim_face_frame(sim, sample, frame);
    const OXR_Located located = locate_world(base, sample);
    const SIM_DeviceState& head = sample.devices[(u32)SIM_Device::Head];
    for (u32 e = 0; e < XR_EYE_POSITION_COUNT_FB; e++)
    {
        const OXR_Located target = { .pose = frame.eyes[e], .valid = head.active, .tracked = head.tracked };
        XrSpaceLocationFlags flags = 0;
        XrEyeGazeFB&         gaze  = gazes->gaze[e];
        gaze.gazePose              = pose_to_xr(zm::POSE_IDENTITY);
        if (located.valid)
        {
            locate_relative(target, located, gaze.gazePose, flags, nullptr);
        }
        const b8 valid      = (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        gaze.isValid        = valid ? XR_TRUE : XR_FALSE;
        gaze.gazeConfidence = valid ? 1.0f : 0.0f;
    }
    gazes->time = time;
    return XR_SUCCESS;
}

//

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateDisplayRefreshRatesFB (XrSession session, uint32_t capacity, uint32_t* count_output, float* rates)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    f32            supported[SIM_MAX_REFRESH_RATES];
    const u32      count  = sim_refresh_rates(runtime()->sim, supported, SIM_MAX_REFRESH_RATES);
    const XrResult result = oxr_two_call(capacity, count_output, rates, count);
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    memory_copy(rates, supported, count * sizeof(f32));
    return XR_SUCCESS;
}

//
// XR_OCULUS_audio_device_guid
//

// openxr_platform.h declares these only with XR_USE_PLATFORM_WIN32; the same types everywhere
typedef XrResult (XRAPI_PTR* PFN_xrGetAudioOutputDeviceGuidOculus)(XrInstance instance, wchar_t* buffer);
typedef XrResult (XRAPI_PTR* PFN_xrGetAudioInputDeviceGuidOculus)(XrInstance instance, wchar_t* buffer);

// The simulated headset plays and records through the PC's default devices; without one the id is
// empty, which apps take as the system default
static XrResult audio_device_guid (XrInstance instance, wchar_t* buffer, b8 input)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    OXR_CHECK(buffer);
    os_default_audio_device(input, buffer, OXR_AUDIO_DEVICE_ID_SIZE);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetAudioOutputDeviceGuidOculus (XrInstance instance, wchar_t* buffer)
{
    return audio_device_guid(instance, buffer, false);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetAudioInputDeviceGuidOculus (XrInstance instance, wchar_t* buffer)
{
    return audio_device_guid(instance, buffer, true);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetDisplayRefreshRateFB (XrSession session, float* rate)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK(rate);
    *rate = sim_refresh_rate(runtime()->sim);
    return XR_SUCCESS;
}

// Spec 12.114: 0 means no preference, the runtime's default. A recording keeps the rate it
// started with, so the rate is fixed while one is written or replayed.
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrRequestDisplayRefreshRateFB (XrSession session, float rate)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_Runtime* rt   = runtime();
    SIM_Sim*     sim  = rt->sim;
    const f32    from = sim_refresh_rate(sim);
    const f32    to   = rate == 0.0f ? sim_default_refresh_rate(sim) : rate;
    f32          supported[SIM_MAX_REFRESH_RATES];
    const u32    count  = sim_refresh_rates(sim, supported, SIM_MAX_REFRESH_RATES);
    b8           listed = false;
    for (u32 i = 0; i < count; i++)
    {
        listed |= supported[i] == to;
    }
    if (!listed)
    {
        return XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB;
    }
    if (to == from)
    {
        return XR_SUCCESS;
    }
    if (recording_or_replaying(rt))
    {
        LOG_WARNING("The refresh rate cannot change while recording or replaying");
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    sim_set_refresh_rate(sim, to);
    OXR_Instance* inst = rt->instances.try_get(sess->instance);
    if (inst)
    {
        push_event(inst, { .type = XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB, .session = session, .from_rate = from, .to_rate = to });
    }
    return XR_SUCCESS;
}

static const XrColorSpaceFB oxr_color_spaces[] = {
    XR_COLOR_SPACE_UNMANAGED_FB, XR_COLOR_SPACE_REC2020_FB, XR_COLOR_SPACE_REC709_FB, XR_COLOR_SPACE_RIFT_CV1_FB,
    XR_COLOR_SPACE_RIFT_S_FB,    XR_COLOR_SPACE_QUEST_FB,   XR_COLOR_SPACE_P3_FB,     XR_COLOR_SPACE_ADOBE_RGB_FB,
};

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumerateColorSpacesFB (XrSession session, uint32_t capacity, uint32_t* count_output, XrColorSpaceFB* spaces)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    const XrResult result = oxr_two_call(capacity, count_output, spaces, ARRAY_SIZE(oxr_color_spaces));
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    memory_copy(spaces, oxr_color_spaces, sizeof(oxr_color_spaces));
    return XR_SUCCESS;
}

// The choice is kept and reported; the mirror shows the app's colors unconverted
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetColorSpaceFB (XrSession session, const XrColorSpaceFB space)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    for (const XrColorSpaceFB supported : oxr_color_spaces)
    {
        if (supported == space)
        {
            sess->color_space = space;
            return XR_SUCCESS;
        }
    }
    return XR_ERROR_COLOR_SPACE_UNSUPPORTED_FB;
}

// The simulated GPU never needs to scale down, so the recommendation is the sub-image the app
// submits (the first view of a projection). The extension takes the time of the last waited frame
// or later; with accept_older_frame_time any frame not yet ended will do
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetRecommendedLayerResolutionMETA (XrSession session, const XrRecommendedLayerResolutionGetInfoMETA* info, XrRecommendedLayerResolutionMETA* resolution)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(info, XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META);
    OXR_CHECK_TYPE(resolution, XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META);
    OXR_CHECK(info->layer);
    const b8 older = sim_get_config(runtime()->sim).accept_older_frame_time && info->predictedDisplayTime > sess->last_display_time;
    if (info->predictedDisplayTime < sess->waited_display_time && !older)
    {
        return XR_ERROR_TIME_INVALID;
    }
    const XrSwapchainSubImage* sub_image = nullptr;
    switch (info->layer->type)
    {
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
    {
        const XrCompositionLayerProjection* layer = (const XrCompositionLayerProjection*)info->layer;
        OXR_CHECK(layer->viewCount > 0 && layer->views);
        sub_image = &layer->views[0].subImage;
        break;
    }
    case XR_TYPE_COMPOSITION_LAYER_QUAD:
        sub_image = &((const XrCompositionLayerQuad*)info->layer)->subImage;
        break;
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
        sub_image = &((const XrCompositionLayerCylinderKHR*)info->layer)->subImage;
        break;
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
        sub_image = &((const XrCompositionLayerEquirectKHR*)info->layer)->subImage;
        break;
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
        sub_image = &((const XrCompositionLayerEquirect2KHR*)info->layer)->subImage;
        break;
    default:
        return XR_ERROR_LAYER_INVALID;
    }
    COMP_View      view   = {};
    const XrResult result = validate_sub_image(sess, *sub_image, view);
    if (result != XR_SUCCESS)
    {
        return result;
    }
    resolution->recommendedImageDimensions = sub_image->imageRect.extent;
    resolution->isValid                    = XR_TRUE;
    return XR_SUCCESS;
}

namespace
{

// XR_META_performance_metrics: counters of the simulated frame loop, so they are the same in
// every run; the simulator has no separate CPU and GPU timelines
enum class OXR_PerfCounter : u8
{
    AppCpuFrametime = 0,
    AppGpuFrametime,
    DroppedFrames,
    SpaceWarpMode,
    Count,
};

} // namespace

static const char* const oxr_perf_counter_paths[(u32)OXR_PerfCounter::Count] = {
    "/perfmetrics_meta/app/cpu_frametime",
    "/perfmetrics_meta/app/gpu_frametime",
    "/perfmetrics_meta/compositor/dropped_frame_count",
    "/perfmetrics_meta/compositor/spacewarp_mode",
};

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrEnumeratePerformanceMetricsCounterPathsMETA (XrInstance instance, uint32_t capacity, uint32_t* count_output, XrPath* paths)
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    const XrResult result = oxr_two_call(capacity, count_output, paths, (u32)OXR_PerfCounter::Count);
    if (result != XR_SUCCESS || capacity == 0)
    {
        return result;
    }
    for (u32 i = 0; i < (u32)OXR_PerfCounter::Count; i++)
    {
        paths[i] = path_intern(inst, oxr_perf_counter_paths[i]);
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetPerformanceMetricsStateMETA (XrSession session, const XrPerformanceMetricsStateMETA* state)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SESSION(sess, session);
    OXR_CHECK_TYPE(state, XR_TYPE_PERFORMANCE_METRICS_STATE_META);
    sess->perf_metrics = state->enabled != XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetPerformanceMetricsStateMETA (XrSession session, XrPerformanceMetricsStateMETA* state)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    OXR_CHECK_TYPE(state, XR_TYPE_PERFORMANCE_METRICS_STATE_META);
    state->enabled = sess->perf_metrics ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrQueryPerformanceMetricsCounterMETA (XrSession session, XrPath path, XrPerformanceMetricsCounterMETA* counter)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    OXR_CHECK_TYPE(counter, XR_TYPE_PERFORMANCE_METRICS_COUNTER_META);
    const char* name = path_string(inst, path);
    if (!name)
    {
        return XR_ERROR_PATH_INVALID;
    }
    // Spec: querying with the system disabled is a validation failure
    OXR_CHECK(sess->perf_metrics);
    u32 index = 0;
    while (index < (u32)OXR_PerfCounter::Count && !cstr_equal(name, oxr_perf_counter_paths[index]))
    {
        index++;
    }

    const f32 interval_ms = (f32)sess->frame_interval * 1e-6f;
    switch ((OXR_PerfCounter)index)
    {
    case OXR_PerfCounter::AppCpuFrametime:
    case OXR_PerfCounter::AppGpuFrametime:
        counter->counterFlags = XR_PERFORMANCE_METRICS_COUNTER_ANY_VALUE_VALID_BIT_META | XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META;
        counter->counterUnit  = XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META;
        counter->uintValue    = 0;
        counter->floatValue   = interval_ms;
        return XR_SUCCESS;
    case OXR_PerfCounter::DroppedFrames:
    case OXR_PerfCounter::SpaceWarpMode:
        counter->counterFlags = XR_PERFORMANCE_METRICS_COUNTER_ANY_VALUE_VALID_BIT_META | XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META;
        counter->counterUnit  = XR_PERFORMANCE_METRICS_COUNTER_UNIT_GENERIC_META;
        counter->uintValue    = (OXR_PerfCounter)index == OXR_PerfCounter::DroppedFrames ? sess->frames_discarded : (sess->space_warp ? 1 : 0);
        counter->floatValue   = 0.0f;
        return XR_SUCCESS;
    case OXR_PerfCounter::Count:
        break;
    }
    return XR_ERROR_PATH_UNSUPPORTED;
}

// XR_FB_swapchain_update_state: the Vulkan sampler (XR_FB_swapchain_update_state_vulkan) and the
// foveation profile (XR_FB_foveation) are the state types
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrUpdateSwapchainFB (XrSwapchain swapchain, const XrSwapchainStateBaseHeaderFB* state)
{
    const OXR_Lock lock(__func__);
    OXR_GET_SWAPCHAIN(chain, swapchain);
    const OXR_Session*  sess = runtime()->sessions.try_get(chain->session);
    const OXR_Instance* inst = sess ? session_instance(sess) : nullptr;
    OXR_CHECK(state && inst);
    if (state->type == XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB && inst->ext.FB_foveation)
    {
        const XrSwapchainStateFoveationFB* foveation = (const XrSwapchainStateFoveationFB*)state;
        OXR_CHECK(foveation->flags == 0);
        const OXR_FoveationProfile* profile = runtime()->foveation_profiles.try_get(foveation_profile_handle(foveation->profile));
        if (!profile || profile->session != chain->session)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
        chain->foveation_profile = (u64)(uintptr_t)foveation->profile;
        chain->foveation         = profile->foveation;
        return XR_SUCCESS;
    }
    OXR_CHECK(inst->ext.FB_swapchain_update_state_vulkan && state->type == XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB);
    return oxr_vk_set_sampler_state(chain->sampler, state);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetSwapchainStateFB (XrSwapchain swapchain, XrSwapchainStateBaseHeaderFB* state)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SWAPCHAIN(chain, swapchain);
    const OXR_Session*  sess = runtime()->sessions.try_get(chain->session);
    const OXR_Instance* inst = sess ? session_instance(sess) : nullptr;
    OXR_CHECK(state && inst);
    if (state->type == XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB && inst->ext.FB_foveation)
    {
        XrSwapchainStateFoveationFB* foveation = (XrSwapchainStateFoveationFB*)state;
        foveation->flags                       = 0;
        foveation->profile                     = (XrFoveationProfileFB)(uintptr_t)chain->foveation_profile;
        return XR_SUCCESS;
    }
    OXR_CHECK(inst->ext.FB_swapchain_update_state_vulkan && state->type == XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB);
    return oxr_vk_get_sampler_state(chain->sampler, state);
}

// XR_FB_foveation, XR_FB_foveation_configuration, XR_META_foveation_eye_tracked: profiles are
// validated and kept on the swapchains they are applied to; nothing is foveated
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateFoveationProfileFB (XrSession session, const XrFoveationProfileCreateInfoFB* info, XrFoveationProfileFB* out_profile)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    const OXR_Instance* inst = session_instance(sess);
    OXR_CHECK(inst);
    OXR_CHECK_TYPE(info, XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB);
    OXR_CHECK(out_profile);
    *out_profile = XR_NULL_HANDLE;

    // Spec: a profile without a level applies no foveation
    OXR_Foveation foveation = { .level = XR_FOVEATION_LEVEL_NONE_FB, .dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB };
    for (const XrBaseInStructure* next = (const XrBaseInStructure*)info->next; next && inst->ext.FB_foveation_configuration; next = next->next)
    {
        if (next->type != XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB)
        {
            continue;
        }
        const XrFoveationLevelProfileCreateInfoFB* level = (const XrFoveationLevelProfileCreateInfoFB*)next;
        OXR_CHECK((u32)level->level <= (u32)XR_FOVEATION_LEVEL_HIGH_FB && (u32)level->dynamic <= (u32)XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB && zm::is_finite(level->verticalOffset));
        foveation.level           = level->level;
        foveation.vertical_offset = level->verticalOffset;
        foveation.dynamic         = level->dynamic;
        // Spec XR_META_foveation_eye_tracked: chained to the level info
        for (const XrBaseInStructure* eye = (const XrBaseInStructure*)level->next; eye && inst->ext.META_foveation_eye_tracked; eye = eye->next)
        {
            if (eye->type == XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META)
            {
                OXR_CHECK(((const XrFoveationEyeTrackedProfileCreateInfoMETA*)eye)->flags == 0);
                foveation.eye_tracked = true;
            }
        }
    }
    *out_profile = foveation_profile_xr(runtime()->foveation_profiles.add({ .session = session_handle(session), .foveation = foveation }));
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrDestroyFoveationProfileFB (XrFoveationProfileFB profile)
{
    const OXR_Lock lock(__func__);
    const OXR_FoveationProfileHandle handle = foveation_profile_handle(profile);
    if (!runtime()->foveation_profiles.try_get(handle))
    {
        return XR_ERROR_HANDLE_INVALID;
    }
    runtime()->foveation_profiles.remove(handle);
    return XR_SUCCESS;
}

// The simulated user looks straight ahead: the foveal region is the middle of each view
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetFoveationEyeTrackedStateMETA (XrSession session, XrFoveationEyeTrackedStateMETA* state)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(state, XR_TYPE_FOVEATION_EYE_TRACKED_STATE_META);
    for (XrVector2f& center : state->foveationCenter)
    {
        center = { 0.0f, 0.0f };
    }
    state->flags = XR_FOVEATION_EYE_TRACKED_STATE_VALID_BIT_META;
    return XR_SUCCESS;
}

// Tile sizes for fragment density maps; puppetxr renders nothing of the app's, so they are checked and dropped
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrSetTilePropertiesHintMETA (XrSession session, const XrTilePropertiesHintMETA* hint)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_SESSION(sess, session);
    UNUSED(sess);
    OXR_CHECK_TYPE(hint, XR_TYPE_TILE_PROPERTIES_HINT_META);
    OXR_CHECK(hint->propertiesCount > 0 && hint->properties);
    for (u32 i = 0; i < hint->propertiesCount; i++)
    {
        // Workaround: CTS 1.1.63 (test_XR_META_tile_properties_hint.cpp) types its entries with the hint's type
        const XrStructureType type = hint->properties[i].type;
        OXR_CHECK(type == XR_TYPE_TILE_PROPERTIES_META || type == XR_TYPE_TILE_PROPERTIES_HINT_META);
    }
    return XR_SUCCESS;
}

#define OXR_FUNCTIONS(X)                                                   \
    X(xrEnumerateApiLayerProperties,         true)                         \
    X(xrEnumerateInstanceExtensionProperties, true)                        \
    X(xrCreateInstance,                      true)                         \
    X(xrDestroyInstance,                     inst)                         \
    X(xrGetInstanceProperties,               inst)                         \
    X(xrPollEvent,                           inst)                         \
    X(xrResultToString,                      inst)                         \
    X(xrStructureTypeToString,               inst)                         \
    X(xrGetSystem,                           inst)                         \
    X(xrGetSystemProperties,                 inst)                         \
    X(xrEnumerateEnvironmentBlendModes,      inst)                         \
    X(xrCreateSession,                       inst)                         \
    X(xrDestroySession,                      inst)                         \
    X(xrEnumerateReferenceSpaces,            inst)                         \
    X(xrCreateReferenceSpace,                inst)                         \
    X(xrGetReferenceSpaceBoundsRect,         inst)                         \
    X(xrCreateActionSpace,                   inst)                         \
    X(xrLocateSpace,                         inst)                         \
    X(xrDestroySpace,                        inst)                         \
    X(xrEnumerateViewConfigurations,         inst)                         \
    X(xrGetViewConfigurationProperties,      inst)                         \
    X(xrEnumerateViewConfigurationViews,     inst)                         \
    X(xrEnumerateSwapchainFormats,           inst)                         \
    X(xrCreateSwapchain,                     inst)                         \
    X(xrDestroySwapchain,                    inst)                         \
    X(xrEnumerateSwapchainImages,            inst)                         \
    X(xrAcquireSwapchainImage,               inst)                         \
    X(xrWaitSwapchainImage,                  inst)                         \
    X(xrReleaseSwapchainImage,               inst)                         \
    X(xrBeginSession,                        inst)                         \
    X(xrEndSession,                          inst)                         \
    X(xrRequestExitSession,                  inst)                         \
    X(xrWaitFrame,                           inst)                         \
    X(xrBeginFrame,                          inst)                         \
    X(xrEndFrame,                            inst)                         \
    X(xrLocateViews,                         inst)                         \
    X(xrStringToPath,                        inst)                         \
    X(xrPathToString,                        inst)                         \
    X(xrCreateActionSet,                     inst)                         \
    X(xrDestroyActionSet,                    inst)                         \
    X(xrCreateAction,                        inst)                         \
    X(xrDestroyAction,                       inst)                         \
    X(xrSuggestInteractionProfileBindings,   inst)                         \
    X(xrAttachSessionActionSets,             inst)                         \
    X(xrGetCurrentInteractionProfile,        inst)                         \
    X(xrGetActionStateBoolean,               inst)                         \
    X(xrGetActionStateFloat,                 inst)                         \
    X(xrGetActionStateVector2f,              inst)                         \
    X(xrGetActionStatePose,                  inst)                         \
    X(xrSyncActions,                         inst)                         \
    X(xrEnumerateBoundSourcesForAction,      inst)                         \
    X(xrGetInputSourceLocalizedName,         inst)                         \
    X(xrApplyHapticFeedback,                 inst)                         \
    X(xrStopHapticFeedback,                  inst)                         \
    X(xrGetDeviceSampleRateFB,               inst && inst->ext.FB_haptic_pcm) \
    X(xrHapticParametricGetPropertiesEXT,    inst && inst->ext.EXT_haptic_parametric) \
    X(xrGetStationaryReferenceSpaceGenerationIdEXT, inst && inst->ext.EXT_stationary_reference_space) \
    X(xrLocateSpaces,                        inst && XR_VERSION_MINOR(inst->api_version) >= 1) \
    X(xrGetVisibilityMaskKHR,                inst && inst->ext.KHR_visibility_mask)             \
    X(xrConvertTimespecTimeToTimeKHR,        inst && inst->ext.KHR_convert_timespec_time)       \
    X(xrConvertTimeToTimespecTimeKHR,        inst && inst->ext.KHR_convert_timespec_time)       \
    X(xrSetInputDeviceActiveEXT,             inst && inst->ext.EXT_conformance_automation)      \
    X(xrSetInputDeviceStateBoolEXT,          inst && inst->ext.EXT_conformance_automation)      \
    X(xrSetInputDeviceStateFloatEXT,         inst && inst->ext.EXT_conformance_automation)      \
    X(xrSetInputDeviceStateVector2fEXT,      inst && inst->ext.EXT_conformance_automation)      \
    X(xrSetInputDeviceLocationEXT,           inst && inst->ext.EXT_conformance_automation)      \
    X(xrCreateSpatialAnchorFB,               inst && inst->ext.FB_spatial_entity)               \
    X(xrGetSpaceUuidFB,                      inst && inst->ext.FB_spatial_entity)               \
    X(xrEnumerateSpaceSupportedComponentsFB, inst && inst->ext.FB_spatial_entity)               \
    X(xrSetSpaceComponentStatusFB,           inst && inst->ext.FB_spatial_entity)               \
    X(xrGetSpaceComponentStatusFB,           inst && inst->ext.FB_spatial_entity)               \
    X(xrQuerySpacesFB,                       inst && inst->ext.FB_spatial_entity_query)         \
    X(xrRetrieveSpaceQueryResultsFB,         inst && inst->ext.FB_spatial_entity_query)         \
    X(xrGetSpaceContainerFB,                 inst && inst->ext.FB_spatial_entity_container)     \
    X(xrGetSpaceBoundingBox2DFB,             inst && inst->ext.FB_scene)                        \
    X(xrGetSpaceBoundingBox3DFB,             inst && inst->ext.FB_scene)                        \
    X(xrGetSpaceSemanticLabelsFB,            inst && inst->ext.FB_scene)                        \
    X(xrGetSpaceBoundary2DFB,                inst && inst->ext.FB_scene)                        \
    X(xrGetSpaceRoomLayoutFB,                inst && inst->ext.FB_scene)                        \
    X(xrRequestSceneCaptureFB,               inst && inst->ext.FB_scene_capture)                \
    X(xrDiscoverSpacesMETA,                  inst && inst->ext.META_spatial_entity_discovery)   \
    X(xrRetrieveSpaceDiscoveryResultsMETA,   inst && inst->ext.META_spatial_entity_discovery)   \
    X(xrGetSpaceTriangleMeshMETA,            inst && inst->ext.META_spatial_entity_mesh)        \
    X(xrGetSpaceRoomMeshMETA,                inst && inst->ext.META_spatial_entity_room_mesh)   \
    X(xrGetSpaceRoomMeshFaceIndicesMETA,     inst && inst->ext.META_spatial_entity_room_mesh)   \
    X(xrSaveSpaceFB,                         inst && inst->ext.FB_spatial_entity_storage)       \
    X(xrEraseSpaceFB,                        inst && inst->ext.FB_spatial_entity_storage)       \
    X(xrSaveSpaceListFB,                     inst && inst->ext.FB_spatial_entity_storage_batch) \
    X(xrSaveSpacesMETA,                      inst && inst->ext.META_spatial_entity_persistence) \
    X(xrEraseSpacesMETA,                     inst && inst->ext.META_spatial_entity_persistence) \
    X(xrCreateSpaceUserFB,                   inst && inst->ext.FB_spatial_entity_user)          \
    X(xrGetSpaceUserIdFB,                    inst && inst->ext.FB_spatial_entity_user)          \
    X(xrDestroySpaceUserFB,                  inst && inst->ext.FB_spatial_entity_user)          \
    X(xrShareSpacesFB,                       inst && inst->ext.FB_spatial_entity_sharing)       \
    X(xrShareSpacesMETA,                     inst && inst->ext.META_spatial_entity_sharing)     \
    X(xrStartColocationAdvertisementMETA,    inst && inst->ext.META_colocation_discovery)       \
    X(xrStopColocationAdvertisementMETA,     inst && inst->ext.META_colocation_discovery)       \
    X(xrStartColocationDiscoveryMETA,        inst && inst->ext.META_colocation_discovery)       \
    X(xrStopColocationDiscoveryMETA,         inst && inst->ext.META_colocation_discovery)       \
    X(xrCreatePassthroughFB,                 inst && inst->ext.FB_passthrough)                  \
    X(xrDestroyPassthroughFB,                inst && inst->ext.FB_passthrough)                  \
    X(xrPassthroughStartFB,                  inst && inst->ext.FB_passthrough)                  \
    X(xrPassthroughPauseFB,                  inst && inst->ext.FB_passthrough)                  \
    X(xrCreatePassthroughLayerFB,            inst && inst->ext.FB_passthrough)                  \
    X(xrDestroyPassthroughLayerFB,           inst && inst->ext.FB_passthrough)                  \
    X(xrPassthroughLayerPauseFB,             inst && inst->ext.FB_passthrough)                  \
    X(xrPassthroughLayerResumeFB,            inst && inst->ext.FB_passthrough)                  \
    X(xrPassthroughLayerSetStyleFB,          inst && inst->ext.FB_passthrough)                  \
    X(xrCreateGeometryInstanceFB,            inst && inst->ext.FB_passthrough)                  \
    X(xrDestroyGeometryInstanceFB,           inst && inst->ext.FB_passthrough)                  \
    X(xrGeometryInstanceSetTransformFB,      inst && inst->ext.FB_passthrough)                  \
    X(xrCreateHandTrackerEXT,                inst && inst->ext.EXT_hand_tracking)               \
    X(xrDestroyHandTrackerEXT,               inst && inst->ext.EXT_hand_tracking)               \
    X(xrLocateHandJointsEXT,                 inst && inst->ext.EXT_hand_tracking)               \
    X(xrGetHandMeshFB,                       inst && inst->ext.FB_hand_tracking_mesh)           \
    X(xrCreateBodyTrackerFB,                 inst && inst->ext.FB_body_tracking)                \
    X(xrDestroyBodyTrackerFB,                inst && inst->ext.FB_body_tracking)                \
    X(xrLocateBodyJointsFB,                  inst && inst->ext.FB_body_tracking)                \
    X(xrGetBodySkeletonFB,                   inst && inst->ext.FB_body_tracking)                \
    X(xrRequestBodyTrackingFidelityMETA,     inst && inst->ext.FB_body_tracking && inst->ext.META_body_tracking_fidelity) \
    X(xrSuggestBodyTrackingCalibrationOverrideMETA, inst && inst->ext.FB_body_tracking && inst->ext.META_body_tracking_calibration) \
    X(xrResetBodyTrackingCalibrationMETA,    inst && inst->ext.FB_body_tracking && inst->ext.META_body_tracking_calibration) \
    X(xrCreateFaceTracker2FB,                inst && inst->ext.FB_face_tracking2)               \
    X(xrDestroyFaceTracker2FB,               inst && inst->ext.FB_face_tracking2)               \
    X(xrGetFaceExpressionWeights2FB,         inst && inst->ext.FB_face_tracking2)               \
    X(xrCreateFaceTrackerFB,                 inst && inst->ext.FB_face_tracking)                \
    X(xrDestroyFaceTrackerFB,                inst && inst->ext.FB_face_tracking)                \
    X(xrGetFaceExpressionWeightsFB,          inst && inst->ext.FB_face_tracking)                \
    X(xrCreateEyeTrackerFB,                  inst && inst->ext.FB_eye_tracking_social)          \
    X(xrDestroyEyeTrackerFB,                 inst && inst->ext.FB_eye_tracking_social)          \
    X(xrGetEyeGazesFB,                       inst && inst->ext.FB_eye_tracking_social)          \
    X(xrResumeSimultaneousHandsAndControllersTrackingMETA, inst && inst->ext.META_simultaneous_hands_and_controllers) \
    X(xrPauseSimultaneousHandsAndControllersTrackingMETA,  inst && inst->ext.META_simultaneous_hands_and_controllers) \
    X(xrEnumerateRenderModelPathsFB,         inst && inst->ext.FB_render_model)                 \
    X(xrGetRenderModelPropertiesFB,          inst && inst->ext.FB_render_model)                 \
    X(xrLoadRenderModelFB,                   inst && inst->ext.FB_render_model)                 \
    X(xrEnumerateSpatialCapabilitiesEXT,         inst && inst->ext.EXT_spatial_entity) \
    X(xrEnumerateSpatialCapabilityComponentTypesEXT, inst && inst->ext.EXT_spatial_entity) \
    X(xrEnumerateSpatialCapabilityFeaturesEXT,   inst && inst->ext.EXT_spatial_entity) \
    X(xrCreateSpatialContextAsyncEXT,            inst && inst->ext.EXT_spatial_entity) \
    X(xrCreateSpatialContextCompleteEXT,         inst && inst->ext.EXT_spatial_entity) \
    X(xrDestroySpatialContextEXT,                inst && inst->ext.EXT_spatial_entity) \
    X(xrCreateSpatialDiscoverySnapshotAsyncEXT,  inst && inst->ext.EXT_spatial_entity) \
    X(xrCreateSpatialDiscoverySnapshotCompleteEXT, inst && inst->ext.EXT_spatial_entity) \
    X(xrQuerySpatialComponentDataEXT,            inst && inst->ext.EXT_spatial_entity) \
    X(xrDestroySpatialSnapshotEXT,               inst && inst->ext.EXT_spatial_entity) \
    X(xrCreateSpatialEntityFromIdEXT,            inst && inst->ext.EXT_spatial_entity) \
    X(xrDestroySpatialEntityEXT,                 inst && inst->ext.EXT_spatial_entity) \
    X(xrCreateSpatialUpdateSnapshotEXT,          inst && inst->ext.EXT_spatial_entity) \
    X(xrGetSpatialBufferStringEXT,               inst && inst->ext.EXT_spatial_entity) \
    X(xrGetSpatialBufferUint8EXT,                inst && inst->ext.EXT_spatial_entity) \
    X(xrGetSpatialBufferUint16EXT,               inst && inst->ext.EXT_spatial_entity) \
    X(xrGetSpatialBufferUint32EXT,               inst && inst->ext.EXT_spatial_entity) \
    X(xrGetSpatialBufferFloatEXT,                inst && inst->ext.EXT_spatial_entity) \
    X(xrGetSpatialBufferVector2fEXT,             inst && inst->ext.EXT_spatial_entity) \
    X(xrGetSpatialBufferVector3fEXT,             inst && inst->ext.EXT_spatial_entity) \
    X(xrCreateRenderModelEXT,                    inst && inst->ext.EXT_render_model) \
    X(xrDestroyRenderModelEXT,                   inst && inst->ext.EXT_render_model) \
    X(xrGetRenderModelPropertiesEXT,             inst && inst->ext.EXT_render_model) \
    X(xrCreateRenderModelSpaceEXT,               inst && inst->ext.EXT_render_model) \
    X(xrCreateRenderModelAssetEXT,               inst && inst->ext.EXT_render_model) \
    X(xrDestroyRenderModelAssetEXT,              inst && inst->ext.EXT_render_model) \
    X(xrGetRenderModelAssetDataEXT,              inst && inst->ext.EXT_render_model) \
    X(xrGetRenderModelAssetPropertiesEXT,        inst && inst->ext.EXT_render_model) \
    X(xrGetRenderModelStateEXT,                  inst && inst->ext.EXT_render_model) \
    X(xrEnumerateInteractionRenderModelIdsEXT,   inst && inst->ext.EXT_interaction_render_model) \
    X(xrEnumerateRenderModelSubactionPathsEXT,   inst && inst->ext.EXT_interaction_render_model) \
    X(xrGetRenderModelPoseTopLevelUserPathEXT,   inst && inst->ext.EXT_interaction_render_model) \
    X(xrCreateDynamicObjectTrackerMETA,      inst && inst->ext.META_dynamic_object_tracker)     \
    X(xrCreateEventChannelEXTX1,             inst && inst->ext.EXTX1_event_channel)             \
    X(xrDestroyEventChannelEXTX1,            inst && inst->ext.EXTX1_event_channel)             \
    X(xrPollEventChannelEXTX1,               inst && inst->ext.EXTX1_event_channel)             \
    X(xrSelectEventChannelEXTX1,             inst && inst->ext.EXTX1_event_channel)             \
    X(xrSetDefaultEventChannelEXTX1,         inst && inst->ext.EXTX1_event_channel)             \
    X(xrDestroyDynamicObjectTrackerMETA,     inst && inst->ext.META_dynamic_object_tracker)     \
    X(xrSetDynamicObjectTrackedClassesMETA,  inst && inst->ext.META_dynamic_object_tracker)     \
    X(xrGetSpaceDynamicObjectDataMETA,       inst && inst->ext.META_dynamic_object_tracker)     \
    X(xrQuerySystemTrackedKeyboardFB,        inst && inst->ext.FB_keyboard_tracking)            \
    X(xrCreateKeyboardSpaceFB,               inst && inst->ext.FB_keyboard_tracking)            \
    X(xrCreateVirtualKeyboardMETA,           inst && inst->ext.META_virtual_keyboard)           \
    X(xrDestroyVirtualKeyboardMETA,          inst && inst->ext.META_virtual_keyboard)           \
    X(xrCreateVirtualKeyboardSpaceMETA,      inst && inst->ext.META_virtual_keyboard)           \
    X(xrSuggestVirtualKeyboardLocationMETA,  inst && inst->ext.META_virtual_keyboard)           \
    X(xrGetVirtualKeyboardScaleMETA,         inst && inst->ext.META_virtual_keyboard)           \
    X(xrSetVirtualKeyboardModelVisibilityMETA, inst && inst->ext.META_virtual_keyboard)         \
    X(xrGetVirtualKeyboardModelAnimationStatesMETA, inst && inst->ext.META_virtual_keyboard)    \
    X(xrGetVirtualKeyboardDirtyTexturesMETA, inst && inst->ext.META_virtual_keyboard)           \
    X(xrGetVirtualKeyboardTextureDataMETA,   inst && inst->ext.META_virtual_keyboard)           \
    X(xrSendVirtualKeyboardInputMETA,        inst && inst->ext.META_virtual_keyboard)           \
    X(xrChangeVirtualKeyboardTextContextMETA, inst && inst->ext.META_virtual_keyboard)          \
    X(xrCreateTriangleMeshFB,                inst && inst->ext.FB_triangle_mesh)                \
    X(xrDestroyTriangleMeshFB,               inst && inst->ext.FB_triangle_mesh)                \
    X(xrTriangleMeshGetVertexBufferFB,       inst && inst->ext.FB_triangle_mesh)                \
    X(xrTriangleMeshGetIndexBufferFB,        inst && inst->ext.FB_triangle_mesh)                \
    X(xrTriangleMeshBeginUpdateFB,           inst && inst->ext.FB_triangle_mesh)                \
    X(xrTriangleMeshEndUpdateFB,             inst && inst->ext.FB_triangle_mesh)                \
    X(xrTriangleMeshBeginVertexBufferUpdateFB, inst && inst->ext.FB_triangle_mesh)              \
    X(xrTriangleMeshEndVertexBufferUpdateFB, inst && inst->ext.FB_triangle_mesh)                \
    X(xrCreateEnvironmentDepthProviderMETA,           inst && inst->ext.META_environment_depth)   \
    X(xrDestroyEnvironmentDepthProviderMETA,          inst && inst->ext.META_environment_depth)   \
    X(xrStartEnvironmentDepthProviderMETA,            inst && inst->ext.META_environment_depth)   \
    X(xrStopEnvironmentDepthProviderMETA,             inst && inst->ext.META_environment_depth)   \
    X(xrSetEnvironmentDepthHandRemovalMETA,           inst && inst->ext.META_environment_depth)   \
    X(xrCreateEnvironmentDepthSwapchainMETA,          inst && inst->ext.META_environment_depth)   \
    X(xrDestroyEnvironmentDepthSwapchainMETA,         inst && inst->ext.META_environment_depth)   \
    X(xrGetEnvironmentDepthSwapchainStateMETA,        inst && inst->ext.META_environment_depth)   \
    X(xrEnumerateEnvironmentDepthSwapchainImagesMETA, inst && inst->ext.META_environment_depth)   \
    X(xrAcquireEnvironmentDepthImageMETA,             inst && inst->ext.META_environment_depth)   \
    X(xrCreateEnvironmentRaycasterAsyncMETA,          inst && inst->ext.META_environment_raycast) \
    X(xrCreateEnvironmentRaycasterCompleteMETA,       inst && inst->ext.META_environment_raycast) \
    X(xrDestroyEnvironmentRaycasterMETA,              inst && inst->ext.META_environment_raycast) \
    X(xrPerformEnvironmentRaycastMETA,                inst && inst->ext.META_environment_raycast) \
    X(xrPollFutureEXT,                       inst && inst->ext.EXT_future)                      \
    X(xrCancelFutureEXT,                     inst && inst->ext.EXT_future)                      \
    X(xrGetAudioOutputDeviceGuidOculus,      inst && inst->ext.OCULUS_audio_device_guid)        \
    X(xrGetAudioInputDeviceGuidOculus,       inst && inst->ext.OCULUS_audio_device_guid)        \
    X(xrEnumerateDisplayRefreshRatesFB,      inst && inst->ext.FB_display_refresh_rate)         \
    X(xrGetDisplayRefreshRateFB,             inst && inst->ext.FB_display_refresh_rate)         \
    X(xrRequestDisplayRefreshRateFB,         inst && inst->ext.FB_display_refresh_rate)         \
    X(xrEnumerateColorSpacesFB,              inst && inst->ext.FB_color_space)                  \
    X(xrSetColorSpaceFB,                     inst && inst->ext.FB_color_space)                  \
    X(xrGetRecommendedLayerResolutionMETA,   inst && inst->ext.META_recommended_layer_resolution) \
    X(xrEnumeratePerformanceMetricsCounterPathsMETA, inst && inst->ext.META_performance_metrics) \
    X(xrSetPerformanceMetricsStateMETA,      inst && inst->ext.META_performance_metrics) \
    X(xrGetPerformanceMetricsStateMETA,      inst && inst->ext.META_performance_metrics) \
    X(xrQueryPerformanceMetricsCounterMETA,  inst && inst->ext.META_performance_metrics) \
    X(xrSetTilePropertiesHintMETA,           inst && inst->ext.META_tile_properties_hint)       \
    X(xrRequestBoundaryVisibilityMETA,       inst && inst->ext.META_boundary_visibility)        \
    X(xrGetPassthroughPreferencesMETA,       inst && inst->ext.META_passthrough_preferences)    \
    X(xrPassthroughLayerSetKeyboardHandsIntensityFB, inst && inst->ext.FB_passthrough_keyboard_hands) \
    X(xrUpdateSwapchainFB,                   inst && inst->ext.FB_swapchain_update_state)       \
    X(xrGetSwapchainStateFB,                 inst && inst->ext.FB_swapchain_update_state)       \
    X(xrCreateFoveationProfileFB,            inst && inst->ext.FB_foveation)                    \
    X(xrDestroyFoveationProfileFB,           inst && inst->ext.FB_foveation)                    \
    X(xrGetFoveationEyeTrackedStateMETA,     inst && inst->ext.META_foveation_eye_tracked)      \
    X(xrCreatePassthroughColorLutMETA,       inst && inst->ext.META_passthrough_color_lut)      \
    X(xrDestroyPassthroughColorLutMETA,      inst && inst->ext.META_passthrough_color_lut)      \
    X(xrUpdatePassthroughColorLutMETA,       inst && inst->ext.META_passthrough_color_lut)      \
    X(xrSetHandTrackingFrequencyHintMETA,    inst && inst->ext.META_hand_tracking_frequency_hint) \
    X(xrEnumeratePassthroughCameraSourcePropertiesMETAX1,     inst && inst->ext.METAX1_passthrough_camera_data) \
    X(xrEnumeratePassthroughCameraSupportedResolutionsMETAX1, inst && inst->ext.METAX1_passthrough_camera_data) \
    X(xrGetPassthroughCameraIntrinsicsMETAX1,                 inst && inst->ext.METAX1_passthrough_camera_data) \
    X(xrGetPassthroughCameraExtrinsicsMETAX1,                 inst && inst->ext.METAX1_passthrough_camera_data) \
    X(xrCreatePassthroughCameraProviderMETAX1,                inst && inst->ext.METAX1_passthrough_camera_data) \
    X(xrDestroyPassthroughCameraProviderMETAX1,               inst && inst->ext.METAX1_passthrough_camera_data) \
    X(xrStartPassthroughCameraProviderMETAX1,                 inst && inst->ext.METAX1_passthrough_camera_data) \
    X(xrStopPassthroughCameraProviderMETAX1,                  inst && inst->ext.METAX1_passthrough_camera_data) \
    X(xrAcquirePassthroughCameraImageMETAX1,                  inst && inst->ext.METAX1_passthrough_camera_data) \
    X(xrGetPassthroughCameraImageDataMETAX1,                  inst && inst->ext.METAX1_passthrough_camera_data) \
    X(xrReleasePassthroughCameraImageMETAX1,                  inst && inst->ext.METAX1_passthrough_camera_data)

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetInstanceProcAddr (XrInstance instance, const char* name, PFN_xrVoidFunction* function);

static b8 trace_enabled (XrInstance instance, const char* name)
{
    if (!oxr_trace_read || instance == XR_NULL_HANDLE)
    {
        SIM_Config config = {};
        sim_load_full_config(config);
        cstr_copy(oxr_trace_filter, sizeof(oxr_trace_filter), config.trace);
        oxr_trace_read = true;
    }
    return oxr_trace_matches(oxr_trace_filter, name);
}

static XrResult get_proc_address (XrInstance instance, const char* name, PFN_xrVoidFunction* function)
{
    OXR_CHECK(function);
    *function = nullptr;
    OXR_CHECK(name);

    const OXR_Instance* inst = nullptr;
    if (instance != XR_NULL_HANDLE)
    {
        inst = oxr_find_instance(instance);
        if (!inst)
        {
            return XR_ERROR_HANDLE_INVALID;
        }
    }

#define X(fn, cond)                                                   \
    if (cstr_equal(name, #fn))                                        \
    {                                                                 \
        if (!(cond))                                                  \
        {                                                             \
            return inst ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_ERROR_HANDLE_INVALID; \
        }                                                             \
        *function = (PFN_xrVoidFunction)static_cast<PFN_##fn>(oxr_##fn); \
        return XR_SUCCESS;                                            \
    }
    OXR_FUNCTIONS(X)
#undef X

    if (cstr_equal(name, "xrGetInstanceProcAddr"))
    {
        *function = (PFN_xrVoidFunction)oxr_xrGetInstanceProcAddr;
        return XR_SUCCESS;
    }
    if (cstr_equal(name, "xrNegotiateLoaderRuntimeInterface"))
    {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (inst && inst->ext.KHR_locate_spaces && cstr_equal(name, "xrLocateSpacesKHR"))
    {
        *function = (PFN_xrVoidFunction)oxr_xrLocateSpaces;
        return XR_SUCCESS;
    }
    // Their PFN types need the Windows headers; the parameters are ABI compatible
    if (cstr_equal(name, "xrConvertWin32PerformanceCounterToTimeKHR") || cstr_equal(name, "xrConvertTimeToWin32PerformanceCounterKHR"))
    {
        if (!inst || !inst->ext.KHR_win32_convert_performance_counter_time)
        {
            return inst ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_ERROR_HANDLE_INVALID;
        }
        *function = cstr_equal(name, "xrConvertWin32PerformanceCounterToTimeKHR") ? (PFN_xrVoidFunction)oxr_xrConvertWin32PerformanceCounterToTimeKHR : (PFN_xrVoidFunction)oxr_xrConvertTimeToWin32PerformanceCounterKHR;
        return XR_SUCCESS;
    }
    if (inst && inst->ext.EXTX1_haptic_parametric && cstr_equal(name, "xrHapticParametricGetPropertiesEXTX1"))
    {
        *function = (PFN_xrVoidFunction)oxr_xrHapticParametricGetPropertiesEXT;
        return XR_SUCCESS;
    }
    if (inst && inst->ext.EXTX2_stationary_reference_space && cstr_equal(name, "xrGetStationaryReferenceSpaceIdEXTX2"))
    {
        *function = (PFN_xrVoidFunction)oxr_xrGetStationaryReferenceSpaceGenerationIdEXT;
        return XR_SUCCESS;
    }
    if (inst)
    {
        *function = oxr_vk_get_proc(name, inst->ext.KHR_vulkan_enable, inst->ext.KHR_vulkan_enable2);
        if (!*function)
        {
            *function = oxr_gl_get_proc(name, inst->ext.KHR_opengl_enable, inst->ext.KHR_opengl_es_enable);
        }
        if (!*function)
        {
            *function = oxr_d3d_get_proc(name, inst->ext.KHR_D3D11_enable, inst->ext.KHR_D3D12_enable);
        }
        if (!*function)
        {
            *function = oxr_metal_get_proc(name, inst->ext.KHR_metal_enable || inst->ext.KHRX2_metal_enable);
        }
        if (*function)
        {
            return XR_SUCCESS;
        }
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetInstanceProcAddr (XrInstance instance, const char* name, PFN_xrVoidFunction* function)
{
    const OXR_Lock lock(__func__);
    const XrResult result = get_proc_address(instance, name, function);
    if (XR_SUCCEEDED(result) && !cstr_equal(name, "xrGetInstanceProcAddr") && trace_enabled(instance, name))
    {
        *function = oxr_trace_wrap(name, *function);
    }
    return result;
}

b8 oxr_trace_path_name (XrPath path, char* out, usize size)
{
    const OXR_Lock      lock(__func__);
    const OXR_Instance* only  = nullptr;
    u32                 count = 0;
    for (const OXR_Instance& inst : runtime()->instances)
    {
        only = &inst;
        count++;
    }
    const char* name = count == 1 ? path_string(only, path) : nullptr;
    if (!name)
    {
        return false;
    }
    cstr_copy(out, size, name);
    return true;
}

PUPPETXR_EXPORT XrResult xrNegotiateLoaderRuntimeInterface (const XrNegotiateLoaderInfo* loader_info, XrNegotiateRuntimeRequest* runtime_request);

PUPPETXR_EXPORT XrResult xrNegotiateLoaderRuntimeInterface (const XrNegotiateLoaderInfo* loader_info, XrNegotiateRuntimeRequest* runtime_request)
{
    if (!loader_info || !runtime_request ||
        loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO || loader_info->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader_info->structSize != sizeof(XrNegotiateLoaderInfo) ||
        runtime_request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST || runtime_request->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION || runtime_request->structSize != sizeof(XrNegotiateRuntimeRequest))
    {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (loader_info->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION || loader_info->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
    {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (XR_VERSION_MAJOR(loader_info->minApiVersion) > 1 || XR_VERSION_MAJOR(loader_info->maxApiVersion) < 1)
    {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    runtime();
    runtime_request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtime_request->runtimeApiVersion       = XR_CURRENT_API_VERSION;
    runtime_request->getInstanceProcAddr     = oxr_xrGetInstanceProcAddr;
    return XR_SUCCESS;
}
