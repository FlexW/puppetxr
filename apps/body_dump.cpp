// Prints what an OpenXR runtime reports for body tracking: the skeleton (xrGetBodySkeletonFB) and
// one frame of joints (xrLocateBodyJointsFB) in the stage space, for XR_FB_body_tracking and, when
// the runtime has it, XR_META_body_tracking_full_body. Run on puppetxr and on a headset, the two
// outputs show whether puppetxr's joint axes, parents and proportions are Meta's
// (docs/body-tracking.md). With --expect it compares its output with a file instead.

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
typedef int32_t  i32;
typedef float    f32;
typedef bool     b8;
typedef size_t   usize;

#define MAX_JOINTS  84
#define MAX_OUTPUT  (256 * 1024)

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
    XrSessionState   state;
    b8               running;
    b8               full_body;

    VkInstance       vk_instance;
    VkPhysicalDevice vk_physical;
    VkDevice         vk_device;
    u32              queue_family;

    PFN_xrCreateBodyTrackerFB  create_tracker;
    PFN_xrDestroyBodyTrackerFB destroy_tracker;
    PFN_xrLocateBodyJointsFB   locate_joints;
    PFN_xrGetBodySkeletonFB    get_skeleton;
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
        (void)fprintf(stderr, "body_dump: %s failed with %d\n", what, (int)result);
        return false;
    }
    return true;
}

#if defined(__GNUC__) || defined(__clang__)
static void print (const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
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

static void print_pose (const XrPosef& pose)
{
    print("\"position\":[%.9g,%.9g,%.9g],\"orientation\":[%.9g,%.9g,%.9g,%.9g]", (double)pose.position.x, (double)pose.position.y, (double)pose.position.z, (double)pose.orientation.x, (double)pose.orientation.y, (double)pose.orientation.z, (double)pose.orientation.w);
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
            (void)fprintf(stderr, "usage: body_dump [--frames N] [--out FILE] [--expect FILE]\n");
            return false;
        }
    }
    return true;
}

static b8 create_instance (App& app)
{
    u32                   count         = 0;
    XrExtensionProperties available[512] = {};
    for (XrExtensionProperties& properties : available)
    {
        properties.type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, 512, &count, available), "xrEnumerateInstanceExtensionProperties"))
    {
        return false;
    }
    b8 body = false;
    for (u32 i = 0; i < count; i++)
    {
        body          = body || strcmp(available[i].extensionName, XR_FB_BODY_TRACKING_EXTENSION_NAME) == 0;
        app.full_body = app.full_body || strcmp(available[i].extensionName, XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME) == 0;
    }
    if (!body)
    {
        (void)fprintf(stderr, "body_dump: the runtime has no %s\n", XR_FB_BODY_TRACKING_EXTENSION_NAME);
        return false;
    }

    const char*          extensions[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME };
    XrInstanceCreateInfo info         = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(info.applicationInfo.applicationName, "body_dump");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount      = app.full_body ? 3 : 2;
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
    xrGetInstanceProcAddr(app.instance, "xrCreateBodyTrackerFB", (PFN_xrVoidFunction*)&app.create_tracker);
    xrGetInstanceProcAddr(app.instance, "xrDestroyBodyTrackerFB", (PFN_xrVoidFunction*)&app.destroy_tracker);
    xrGetInstanceProcAddr(app.instance, "xrLocateBodyJointsFB", (PFN_xrVoidFunction*)&app.locate_joints);
    xrGetInstanceProcAddr(app.instance, "xrGetBodySkeletonFB", (PFN_xrVoidFunction*)&app.get_skeleton);
    return app.create_tracker && app.destroy_tracker && app.locate_joints && app.get_skeleton;
}

static b8 create_vulkan (App& app)
{
    if (volkInitialize() != VK_SUCCESS)
    {
        (void)fprintf(stderr, "body_dump: no Vulkan loader\n");
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
    const VkApplicationInfo             app_info         = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "body_dump", .apiVersion = VK_API_VERSION_1_1 };
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

static b8 create_session (App& app)
{
    const XrGraphicsBindingVulkan2KHR binding = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = app.vk_instance, .physicalDevice = app.vk_physical, .device = app.vk_device, .queueFamilyIndex = app.queue_family, .queueIndex = 0 };
    const XrSessionCreateInfo         info    = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = app.system };
    if (!check(xrCreateSession(app.instance, &info, &app.session), "xrCreateSession"))
    {
        return false;
    }
    const XrReferenceSpaceCreateInfo space_info = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE, .poseInReferenceSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    return check(xrCreateReferenceSpace(app.session, &space_info, &app.stage), "xrCreateReferenceSpace");
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
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    out_display_time              = state.predictedDisplayTime;
    return check(xrEndFrame(app.session, &end_info), "xrEndFrame");
}

static b8 dump_joint_set (App& app, XrBodyJointSetFB joint_set, XrTime time, b8 last)
{
    const b8                        full    = joint_set == XR_BODY_JOINT_SET_FULL_BODY_META;
    const u32                       count   = full ? (u32)XR_FULL_BODY_JOINT_COUNT_META : (u32)XR_BODY_JOINT_COUNT_FB;
    XrBodyTrackerFB                 tracker = XR_NULL_HANDLE;
    const XrBodyTrackerCreateInfoFB info    = { .type = XR_TYPE_BODY_TRACKER_CREATE_INFO_FB, .bodyJointSet = joint_set };
    if (!check(app.create_tracker(app.session, &info, &tracker), "xrCreateBodyTrackerFB"))
    {
        return false;
    }

    XrBodySkeletonJointFB skeleton_joints[MAX_JOINTS] = {};
    XrBodySkeletonFB      skeleton                    = { .type = XR_TYPE_BODY_SKELETON_FB, .jointCount = count, .joints = skeleton_joints };
    XrBodyJointLocationFB locations_joints[MAX_JOINTS] = {};
    XrBodyJointLocationsFB locations = { .type = XR_TYPE_BODY_JOINT_LOCATIONS_FB, .jointCount = count, .jointLocations = locations_joints };
    const XrBodyJointsLocateInfoFB locate_info = { .type = XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB, .baseSpace = app.stage, .time = time };
    const b8 ok = check(app.get_skeleton(tracker, &skeleton), "xrGetBodySkeletonFB") && check(app.locate_joints(tracker, &locate_info, &locations), "xrLocateBodyJointsFB");
    app.destroy_tracker(tracker);
    if (!ok)
    {
        return false;
    }

    print("  \"%s\": {\n    \"skeleton\": [\n", full ? "full_body" : "default");
    for (u32 j = 0; j < count; j++)
    {
        print("      {\"joint\":%d,\"parent\":%d,", (int)skeleton_joints[j].joint, (int)skeleton_joints[j].parentJoint);
        print_pose(skeleton_joints[j].pose);
        print("}%s\n", j + 1 < count ? "," : "");
    }
    print("    ],\n    \"active\": %s, \"confidence\": %.9g, \"skeleton_changed\": %u,\n    \"joints\": [\n", locations.isActive ? "true" : "false", (double)locations.confidence, locations.skeletonChangedCount);
    for (u32 j = 0; j < count; j++)
    {
        print("      {\"flags\":%u,", (u32)locations_joints[j].locationFlags);
        print_pose(locations_joints[j].pose);
        print("}%s\n", j + 1 < count ? "," : "");
    }
    print("    ]\n  }%s\n", last ? "" : ",");
    return true;
}

static int finish (const Options& options)
{
    if (options.out)
    {
        FILE* file = fopen(options.out, "wb");
        if (!file || fwrite(g_output.text, 1, g_output.length, file) != g_output.length)
        {
            (void)fprintf(stderr, "body_dump: cannot write %s\n", options.out);
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
        (void)fprintf(stderr, "body_dump: the output differs from %s%s%s\n", options.expect, options.out ? ", see " : "", options.out ? options.out : "");
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
        (void)fprintf(stderr, "body_dump: the session never got focused\n");
        return 1;
    }

    print("{\n  \"runtime\": \"%s\",\n", properties.runtimeName);
    if (!dump_joint_set(app, XR_BODY_JOINT_SET_DEFAULT_FB, time, !app.full_body) || (app.full_body && !dump_joint_set(app, XR_BODY_JOINT_SET_FULL_BODY_META, time, true)))
    {
        return 1;
    }
    print("}\n");
    const int result = finish(app.options);
    xrDestroySession(app.session);
    xrDestroyInstance(app.instance);
    return result;
}
