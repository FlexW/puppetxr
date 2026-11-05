// Prints what an OpenXR runtime reports for eye tracking: the gaze action space of
// XR_EXT_eye_gaze_interaction, the per-eye poses of XR_FB_eye_tracking_social and the expression
// weights of XR_FB_face_tracking2, the poses in the stage and in the view space, next to the views
// themselves. Run on puppetxr and on a headset with eye tracking, the two outputs show whether
// puppetxr's gaze axes, eye positions and weight ranges are the hardware's (docs/face-tracking.md).
// With --expect it compares its output with a file instead.
//
// What a runtime does not have is left out, so a device with only part of these still dumps.
// The system name is not printed: a dump is pasted into reports and committed as a reference, and
// neither should name the headset it came from.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <volk.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

typedef uint32_t u32;
typedef float    f32;
typedef bool     b8;
typedef size_t   usize;

#define MAX_VIEWS   4
#define MAX_OUTPUT  (64 * 1024)

//
// Types
//

namespace
{

struct Options
{
    u32         frames; // run after the session is focused, so a headset's tracking settles
    const char* out;    // also write the output here
    const char* expect; // compare the output with this file
};

struct Output
{
    char text[MAX_OUTPUT];
    u32  length;
};

struct App
{
    Options          options;
    XrInstance       instance;
    XrSystemId       system;
    XrSession        session;
    XrSpace          stage;
    XrSpace          view;
    XrSessionState   state;
    b8               running;
    b8               eye_gaze;
    b8               eye_social;
    b8               face2;

    XrActionSet      action_set;
    XrAction         gaze_action;
    XrPath           eyes_path;
    XrSpace          gaze_space;

    VkInstance       vk_instance;
    VkPhysicalDevice vk_physical;
    VkDevice         vk_device;
    u32              queue_family;

    PFN_xrCreateEyeTrackerFB          create_eye_tracker;
    PFN_xrDestroyEyeTrackerFB         destroy_eye_tracker;
    PFN_xrGetEyeGazesFB               get_eye_gazes;
    PFN_xrCreateFaceTracker2FB        create_face_tracker;
    PFN_xrDestroyFaceTracker2FB       destroy_face_tracker;
    PFN_xrGetFaceExpressionWeights2FB get_face_weights;
};

} // namespace

//
// Statics
//

static Output g_output;

static b8 check (XrResult result, const char* what)
{
    if (XR_FAILED(result))
    {
        (void)fprintf(stderr, "eye_dump: %s failed with %d\n", what, (int)result);
        return false;
    }
    return true;
}

static void print (const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void print (const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(g_output.text + g_output.length, MAX_OUTPUT - g_output.length, fmt, args);
    va_end(args);
    if (written > 0 && g_output.length + (u32)written < MAX_OUTPUT)
    {
        g_output.length += (u32)written;
    }
}

// -0 is the same weight and the same rotation as 0; a reference file should not tell them apart
static double number (f32 value)
{
    return value == 0.0f ? 0.0 : (double)value;
}

static void print_pose (const XrPosef& pose)
{
    print("\"position\":[%.9g,%.9g,%.9g],\"orientation\":[%.9g,%.9g,%.9g,%.9g]", number(pose.position.x), number(pose.position.y), number(pose.position.z), number(pose.orientation.x), number(pose.orientation.y), number(pose.orientation.z), number(pose.orientation.w));
}

static void print_fov (const XrFovf& fov)
{
    print("\"fov\":[%.9g,%.9g,%.9g,%.9g]", number(fov.angleLeft), number(fov.angleRight), number(fov.angleUp), number(fov.angleDown));
}

// The gaze turns with the eyes, so the pose in the view space is what shows the axes: puppetxr
// looks along -Z with +Y the head's up (docs/face-tracking.md)
static void print_in_spaces (App& app, XrSpace space, XrTime time, b8 sample_time)
{
    XrSpace spaces[2] = { app.stage, app.view };
    for (u32 s = 0; s < 2; s++)
    {
        XrEyeGazeSampleTimeEXT sample   = { .type = XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT };
        XrSpaceLocation        location = { .type = XR_TYPE_SPACE_LOCATION, .next = sample_time ? &sample : nullptr };
        const XrResult         result   = xrLocateSpace(space, spaces[s], time, &location);
        print("\"%s\":{\"result\":%d,\"flags\":%u,", s == 0 ? "stage" : "view", (int)result, (u32)location.locationFlags);
        print_pose(location.pose);
        if (sample_time)
        {
            print(",\"sample_time_delta\":%lld", (long long)(sample.time - time));
        }
        print("}%s", s == 0 ? "," : "");
    }
}

static b8 parse_options (int argc, const char* const* argv, Options& out)
{
    out = { .frames = 10 };
    for (int i = 1; i < argc; i++)
    {
        const b8 has_value = i + 1 < argc;
        if (strcmp(argv[i], "--frames") == 0 && has_value)
        {
            out.frames = (u32)strtoul(argv[++i], nullptr, 10);
        }
        else if (strcmp(argv[i], "--out") == 0 && has_value)
        {
            out.out = argv[++i];
        }
        else if (strcmp(argv[i], "--expect") == 0 && has_value)
        {
            out.expect = argv[++i];
        }
        else
        {
            (void)fprintf(stderr, "usage: eye_dump [--frames N] [--out FILE] [--expect FILE]\n");
            return false;
        }
    }
    return true;
}

static b8 create_instance (App& app)
{
    u32                   count          = 0;
    XrExtensionProperties available[512] = {};
    for (XrExtensionProperties& properties : available)
    {
        properties.type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, 512, &count, available), "xrEnumerateInstanceExtensionProperties"))
    {
        return false;
    }
    for (u32 i = 0; i < count; i++)
    {
        app.eye_gaze   = app.eye_gaze || strcmp(available[i].extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0;
        app.eye_social = app.eye_social || strcmp(available[i].extensionName, XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME) == 0;
        app.face2      = app.face2 || strcmp(available[i].extensionName, XR_FB_FACE_TRACKING2_EXTENSION_NAME) == 0;
    }
    if (!app.eye_gaze && !app.eye_social && !app.face2)
    {
        (void)fprintf(stderr, "eye_dump: the runtime has none of %s, %s, %s\n", XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME, XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME, XR_FB_FACE_TRACKING2_EXTENSION_NAME);
        return false;
    }

    const char*          extensions[4] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    u32                  enabled       = 1;
    XrInstanceCreateInfo info          = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
    extensions[enabled]                = XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME;
    enabled += app.eye_gaze ? 1 : 0;
    extensions[enabled] = XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME;
    enabled += app.eye_social ? 1 : 0;
    extensions[enabled] = XR_FB_FACE_TRACKING2_EXTENSION_NAME;
    enabled += app.face2 ? 1 : 0;
    strcpy(info.applicationInfo.applicationName, "eye_dump");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount      = enabled;
    info.enabledExtensionNames      = extensions;
    if (!check(xrCreateInstance(&info, &app.instance), "xrCreateInstance"))
    {
        return false;
    }
    const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    if (!check(xrGetSystem(app.instance, &system_info, &app.system), "xrGetSystem"))
    {
        return false;
    }
    if (app.eye_social)
    {
        xrGetInstanceProcAddr(app.instance, "xrCreateEyeTrackerFB", (PFN_xrVoidFunction*)&app.create_eye_tracker);
        xrGetInstanceProcAddr(app.instance, "xrDestroyEyeTrackerFB", (PFN_xrVoidFunction*)&app.destroy_eye_tracker);
        xrGetInstanceProcAddr(app.instance, "xrGetEyeGazesFB", (PFN_xrVoidFunction*)&app.get_eye_gazes);
        app.eye_social = app.create_eye_tracker && app.destroy_eye_tracker && app.get_eye_gazes;
    }
    if (app.face2)
    {
        xrGetInstanceProcAddr(app.instance, "xrCreateFaceTracker2FB", (PFN_xrVoidFunction*)&app.create_face_tracker);
        xrGetInstanceProcAddr(app.instance, "xrDestroyFaceTracker2FB", (PFN_xrVoidFunction*)&app.destroy_face_tracker);
        xrGetInstanceProcAddr(app.instance, "xrGetFaceExpressionWeights2FB", (PFN_xrVoidFunction*)&app.get_face_weights);
        app.face2 = app.create_face_tracker && app.destroy_face_tracker && app.get_face_weights;
    }
    return true;
}

static b8 create_vulkan (App& app)
{
    if (volkInitialize() != VK_SUCCESS)
    {
        (void)fprintf(stderr, "eye_dump: no Vulkan loader\n");
        return false;
    }
    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR           create_instance  = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR       get_device       = nullptr;
    PFN_xrCreateVulkanDeviceKHR             create_device    = nullptr;
    xrGetInstanceProcAddr(app.instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction*)&get_requirements);
    xrGetInstanceProcAddr(app.instance, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction*)&create_instance);
    xrGetInstanceProcAddr(app.instance, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction*)&get_device);
    xrGetInstanceProcAddr(app.instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction*)&create_device);
    if (!get_requirements || !create_instance || !get_device || !create_device)
    {
        return false;
    }
    XrGraphicsRequirementsVulkan2KHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    if (!check(get_requirements(app.instance, app.system, &requirements), "xrGetVulkanGraphicsRequirements2KHR"))
    {
        return false;
    }

    VkResult                            vk_result        = VK_SUCCESS;
    const VkApplicationInfo             app_info         = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "eye_dump", .apiVersion = VK_API_VERSION_1_1 };
    const VkInstanceCreateInfo          instance_info    = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app_info };
    const XrVulkanInstanceCreateInfoKHR xr_instance_info = { .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = app.system, .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = &instance_info };
    if (!check(create_instance(app.instance, &xr_instance_info, &app.vk_instance, &vk_result), "xrCreateVulkanInstanceKHR") || vk_result != VK_SUCCESS)
    {
        return false;
    }
    volkLoadInstanceOnly(app.vk_instance);
    const XrVulkanGraphicsDeviceGetInfoKHR device_get_info = { .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = app.system, .vulkanInstance = app.vk_instance };
    if (!check(get_device(app.instance, &device_get_info, &app.vk_physical), "xrGetVulkanGraphicsDevice2KHR"))
    {
        return false;
    }

    VkQueueFamilyProperties families[16]  = {};
    u32                     family_count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(app.vk_physical, &family_count, families);
    while (app.queue_family < family_count && !(families[app.queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    {
        app.queue_family++;
    }
    if (app.queue_family == family_count)
    {
        return false;
    }
    const f32                         priority       = 1.0f;
    const VkDeviceQueueCreateInfo     queue_info     = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = app.queue_family, .queueCount = 1, .pQueuePriorities = &priority };
    const VkDeviceCreateInfo          device_info    = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info };
    const XrVulkanDeviceCreateInfoKHR xr_device_info = { .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = app.system, .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanPhysicalDevice = app.vk_physical, .vulkanCreateInfo = &device_info };
    if (!check(create_device(app.instance, &xr_device_info, &app.vk_device, &vk_result), "xrCreateVulkanDeviceKHR") || vk_result != VK_SUCCESS)
    {
        return false;
    }
    volkLoadDevice(app.vk_device);
    return true;
}

static b8 create_gaze_action (App& app)
{
    XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .priority = 0 };
    strcpy(set_info.actionSetName, "eye_dump");
    strcpy(set_info.localizedActionSetName, "eye dump");
    if (!check(xrCreateActionSet(app.instance, &set_info, &app.action_set), "xrCreateActionSet"))
    {
        return false;
    }
    if (!check(xrStringToPath(app.instance, "/user/eyes_ext", &app.eyes_path), "xrStringToPath"))
    {
        return false;
    }
    XrActionCreateInfo action_info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionType = XR_ACTION_TYPE_POSE_INPUT, .countSubactionPaths = 1, .subactionPaths = &app.eyes_path };
    strcpy(action_info.actionName, "gaze");
    strcpy(action_info.localizedActionName, "gaze");
    if (!check(xrCreateAction(app.action_set, &action_info, &app.gaze_action), "xrCreateAction"))
    {
        return false;
    }

    XrPath profile = XR_NULL_PATH;
    XrPath binding = XR_NULL_PATH;
    if (!check(xrStringToPath(app.instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile), "xrStringToPath") || !check(xrStringToPath(app.instance, "/user/eyes_ext/input/gaze_ext/pose", &binding), "xrStringToPath"))
    {
        return false;
    }
    const XrActionSuggestedBinding              suggested = { .action = app.gaze_action, .binding = binding };
    const XrInteractionProfileSuggestedBinding  suggestion = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = profile, .countSuggestedBindings = 1, .suggestedBindings = &suggested };
    return check(xrSuggestInteractionProfileBindings(app.instance, &suggestion), "xrSuggestInteractionProfileBindings");
}

static b8 create_session (App& app)
{
    const XrGraphicsBindingVulkan2KHR binding = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = app.vk_instance, .physicalDevice = app.vk_physical, .device = app.vk_device, .queueFamilyIndex = app.queue_family, .queueIndex = 0 };
    const XrSessionCreateInfo         info    = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = app.system };
    if (!check(xrCreateSession(app.instance, &info, &app.session), "xrCreateSession"))
    {
        return false;
    }
    const XrReferenceSpaceCreateInfo stage_info = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE, .poseInReferenceSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    const XrReferenceSpaceCreateInfo view_info  = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW, .poseInReferenceSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    if (!check(xrCreateReferenceSpace(app.session, &stage_info, &app.stage), "xrCreateReferenceSpace") || !check(xrCreateReferenceSpace(app.session, &view_info, &app.view), "xrCreateReferenceSpace"))
    {
        return false;
    }
    if (app.eye_gaze && !create_gaze_action(app))
    {
        return false;
    }
    if (app.eye_gaze)
    {
        const XrSessionActionSetsAttachInfo attach = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &app.action_set };
        if (!check(xrAttachSessionActionSets(app.session, &attach), "xrAttachSessionActionSets"))
        {
            return false;
        }
        const XrActionSpaceCreateInfo space_info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = app.gaze_action, .subactionPath = app.eyes_path, .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
        if (!check(xrCreateActionSpace(app.session, &space_info, &app.gaze_space), "xrCreateActionSpace"))
        {
            return false;
        }
    }
    return true;
}

static void poll_events (App& app)
{
    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(app.instance, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            app.state = ((XrEventDataSessionStateChanged*)&event)->state;
            if (app.state == XR_SESSION_STATE_READY)
            {
                const XrSessionBeginInfo begin_info = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
                app.running = check(xrBeginSession(app.session, &begin_info), "xrBeginSession");
            }
        }
        event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// An empty frame: no layers, so no swapchain is needed
static b8 frame (App& app, XrTime& out_display_time)
{
    XrFrameState          state     = { .type = XR_TYPE_FRAME_STATE };
    const XrFrameWaitInfo wait_info = { .type = XR_TYPE_FRAME_WAIT_INFO };
    if (!check(xrWaitFrame(app.session, &wait_info, &state), "xrWaitFrame"))
    {
        return false;
    }
    const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    if (!check(xrBeginFrame(app.session, &begin_info), "xrBeginFrame"))
    {
        return false;
    }
    if (app.eye_gaze && app.state == XR_SESSION_STATE_FOCUSED)
    {
        const XrActiveActionSet     active = { .actionSet = app.action_set, .subactionPath = XR_NULL_PATH };
        const XrActionsSyncInfo     sync   = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active };
        if (!check(xrSyncActions(app.session, &sync), "xrSyncActions"))
        {
            return false;
        }
    }
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    out_display_time              = state.predictedDisplayTime;
    return check(xrEndFrame(app.session, &end_info), "xrEndFrame");
}

static b8 dump_views (App& app, XrTime time)
{
    XrView                views[MAX_VIEWS] = {};
    for (XrView& v : views)
    {
        v.type = XR_TYPE_VIEW;
    }
    XrViewState           view_state  = { .type = XR_TYPE_VIEW_STATE };
    const XrViewLocateInfo locate_info = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = time, .space = app.view };
    u32                   count       = 0;
    if (!check(xrLocateViews(app.session, &locate_info, &view_state, MAX_VIEWS, &count, views), "xrLocateViews"))
    {
        return false;
    }
    print("  \"views\": {\n    \"flags\": %u,\n    \"in_view_space\": [\n", (u32)view_state.viewStateFlags);
    for (u32 v = 0; v < count; v++)
    {
        print("      {");
        print_pose(views[v].pose);
        print(",");
        print_fov(views[v].fov);
        print("}%s\n", v + 1 < count ? "," : "");
    }
    print("    ]\n  },\n");
    return true;
}

static void dump_gaze_action (App& app, XrTime time)
{
    const XrActionStateGetInfo get_info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = app.gaze_action, .subactionPath = app.eyes_path };
    XrActionStatePose          pose     = { .type = XR_TYPE_ACTION_STATE_POSE };
    const XrResult             result   = xrGetActionStatePose(app.session, &get_info, &pose);

    XrInteractionProfileState  profile  = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
    char                       name[XR_MAX_PATH_LENGTH] = {};
    u32                        name_length              = 0;
    if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(app.session, app.eyes_path, &profile)) && profile.interactionProfile != XR_NULL_PATH)
    {
        (void)xrPathToString(app.instance, profile.interactionProfile, XR_MAX_PATH_LENGTH, &name_length, name);
    }
    print("  \"ext_eye_gaze_interaction\": {\n    \"profile\": \"%s\", \"state_result\": %d, \"active\": %s,\n    ", name, (int)result, pose.isActive ? "true" : "false");
    print_in_spaces(app, app.gaze_space, time, true);
    print("\n  }%s\n", app.eye_social || app.face2 ? "," : "");
}

static b8 dump_eye_social (App& app, XrTime time)
{
    XrEyeTrackerFB                  tracker = XR_NULL_HANDLE;
    const XrEyeTrackerCreateInfoFB  info    = { .type = XR_TYPE_EYE_TRACKER_CREATE_INFO_FB };
    if (!check(app.create_eye_tracker(app.session, &info, &tracker), "xrCreateEyeTrackerFB"))
    {
        return false;
    }
    print("  \"fb_eye_tracking_social\": [\n");
    b8 ok = true;
    for (u32 e = 0; e < (u32)XR_EYE_POSITION_COUNT_FB && ok; e++)
    {
        // One tracker, located once per base space: the poses come back in the info's space
        for (u32 s = 0; s < 2 && ok; s++)
        {
            XrEyeGazesFB              gazes       = { .type = XR_TYPE_EYE_GAZES_FB };
            const XrEyeGazesInfoFB    gazes_info  = { .type = XR_TYPE_EYE_GAZES_INFO_FB, .baseSpace = s == 0 ? app.stage : app.view, .time = time };
            ok                                    = check(app.get_eye_gazes(tracker, &gazes_info, &gazes), "xrGetEyeGazesFB");
            if (!ok)
            {
                break;
            }
            if (s == 0)
            {
                print("    {\"eye\":%u,\"valid\":%s,\"confidence\":%.9g,\n     ", e, gazes.gaze[e].isValid ? "true" : "false", number(gazes.gaze[e].gazeConfidence));
            }
            print("\"%s\":{", s == 0 ? "stage" : "view");
            print_pose(gazes.gaze[e].gazePose);
            print("}%s", s == 0 ? ",\n     " : "");
        }
        print("}%s\n", e + 1 < (u32)XR_EYE_POSITION_COUNT_FB ? "," : "");
    }
    app.destroy_eye_tracker(tracker);
    print("  ]%s\n", app.face2 ? "," : "");
    return ok;
}

// The weights are the extension's enum order; docs/face-tracking.md names them
static b8 dump_face2 (App& app, XrTime time)
{
    XrFaceTracker2FB                 tracker = XR_NULL_HANDLE;
    XrFaceTrackingDataSource2FB      source  = XR_FACE_TRACKING_DATA_SOURCE2_VISUAL_FB;
    const XrFaceTrackerCreateInfo2FB info    = { .type = XR_TYPE_FACE_TRACKER_CREATE_INFO2_FB, .faceExpressionSet = XR_FACE_EXPRESSION_SET2_DEFAULT_FB, .requestedDataSourceCount = 1, .requestedDataSources = &source };
    if (!check(app.create_face_tracker(app.session, &info, &tracker), "xrCreateFaceTracker2FB"))
    {
        return false;
    }
    f32                            weights[XR_FACE_EXPRESSION2_COUNT_FB] = {};
    f32                            confidences[XR_FACE_CONFIDENCE2_COUNT_FB] = {};
    XrFaceExpressionWeights2FB     expression  = { .type = XR_TYPE_FACE_EXPRESSION_WEIGHTS2_FB, .weightCount = (u32)XR_FACE_EXPRESSION2_COUNT_FB, .weights = weights, .confidenceCount = (u32)XR_FACE_CONFIDENCE2_COUNT_FB, .confidences = confidences };
    const XrFaceExpressionInfo2FB  expression_info = { .type = XR_TYPE_FACE_EXPRESSION_INFO2_FB, .time = time };
    const b8                       ok = check(app.get_face_weights(tracker, &expression_info, &expression), "xrGetFaceExpressionWeights2FB");
    app.destroy_face_tracker(tracker);
    if (!ok)
    {
        return false;
    }
    print("  \"fb_face_tracking2\": {\n    \"valid\": %s, \"eye_following_valid\": %s, \"data_source\": %d,\n", expression.isValid ? "true" : "false", expression.isEyeFollowingBlendshapesValid ? "true" : "false", (int)expression.dataSource);
    print("    \"confidences\": [%.9g,%.9g],\n    \"weights\": [\n", number(confidences[0]), number(confidences[1]));
    for (u32 w = 0; w < (u32)XR_FACE_EXPRESSION2_COUNT_FB; w++)
    {
        print("%s%.9g%s", w % 10 == 0 ? "      " : "", number(weights[w]), w + 1 < (u32)XR_FACE_EXPRESSION2_COUNT_FB ? "," : "");
        print("%s", w % 10 == 9 ? "\n" : "");
    }
    print("    ]\n  }\n");
    return true;
}

static int finish (const Options& options)
{
    if (options.out)
    {
        FILE* file = fopen(options.out, "wb");
        if (!file || fwrite(g_output.text, 1, g_output.length, file) != g_output.length)
        {
            (void)fprintf(stderr, "eye_dump: cannot write %s\n", options.out);
        }
        if (file)
        {
            (void)fclose(file);
        }
    }
    if (!options.expect)
    {
        (void)fwrite(g_output.text, 1, g_output.length, stdout);
        return 0;
    }
    static char expected[MAX_OUTPUT];
    FILE*       file   = fopen(options.expect, "rb");
    const usize length = file ? fread(expected, 1, MAX_OUTPUT, file) : 0;
    if (file)
    {
        (void)fclose(file);
    }
    if (length != g_output.length || memcmp(expected, g_output.text, length) != 0)
    {
        (void)fprintf(stderr, "eye_dump: the output differs from %s%s%s\n", options.expect, options.out ? ", see " : "", options.out ? options.out : "");
        return 1;
    }
    return 0;
}

//
// Main
//

int main (int argc, char** argv)
{
    App app = {};
    if (!parse_options(argc, argv, app.options) || !create_instance(app) || !create_vulkan(app) || !create_session(app))
    {
        return 1;
    }
    XrInstanceProperties properties = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(app.instance, &properties);

    XrSystemEyeGazeInteractionPropertiesEXT gaze_properties   = { .type = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT };
    XrSystemEyeTrackingPropertiesFB         social_properties = { .type = XR_TYPE_SYSTEM_EYE_TRACKING_PROPERTIES_FB, .next = &gaze_properties };
    XrSystemFaceTrackingProperties2FB       face_properties   = { .type = XR_TYPE_SYSTEM_FACE_TRACKING_PROPERTIES2_FB, .next = &social_properties };
    XrSystemProperties                      system            = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &face_properties };
    xrGetSystemProperties(app.instance, app.system, &system);

    XrTime time    = 0;
    u32    focused = 0;
    for (u32 guard = 0; guard < 10000 && focused < app.options.frames; guard++)
    {
        poll_events(app);
        if (!app.running)
        {
            continue;
        }
        if (!frame(app, time))
        {
            return 1;
        }
        focused += app.state == XR_SESSION_STATE_FOCUSED ? 1 : 0;
    }
    if (focused < app.options.frames)
    {
        (void)fprintf(stderr, "eye_dump: the session never got focused\n");
        return 1;
    }

    print("{\n  \"runtime\": \"%s\",\n", properties.runtimeName);
    print("  \"system\": {\"eye_gaze_interaction\": %s, \"eye_tracking\": %s, \"visual_face_tracking\": %s, \"audio_face_tracking\": %s},\n", gaze_properties.supportsEyeGazeInteraction ? "true" : "false", social_properties.supportsEyeTracking ? "true" : "false", face_properties.supportsVisualFaceTracking ? "true" : "false", face_properties.supportsAudioFaceTracking ? "true" : "false");
    if (!dump_views(app, time))
    {
        return 1;
    }
    if (app.eye_gaze)
    {
        dump_gaze_action(app, time);
    }
    if (app.eye_social && !dump_eye_social(app, time))
    {
        return 1;
    }
    if (app.face2 && !dump_face2(app, time))
    {
        return 1;
    }
    print("}\n");
    const int result = finish(app.options);
    xrDestroySession(app.session);
    xrDestroyInstance(app.instance);
    return result;
}
