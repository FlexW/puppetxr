// Minimal OpenXR app used as the end-to-end example: clears each eye to a colour driven by
// the right trigger and the head pose. It talks to puppetxr only through the OpenXR loader and,
// with --script, through the public control API in puppetxr.h.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <volk.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <puppetxr.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  i64;
typedef float    f32;
typedef bool     b8;

#define MAX_IMAGES 8

//
// Types
//

namespace
{

struct Options
{
    u32         frames;            // 0 runs until the runtime ends the session
    b8          script;            // drive input through the puppetxr control API
    b8          log;               // one line per frame on stdout
    const char* screenshot;
    u32         screenshot_frame;
};

struct App
{
    Options          options;
    PUPPETXR_Api     puppetxr;
    b8               has_puppetxr;

    XrInstance       instance;
    XrSystemId       system;
    XrSession        session;
    XrSpace          stage;
    XrSessionState   state;
    b8               running;
    b8               exit_requested;

    XrActionSet      action_set;
    XrAction         trigger;
    XrPath           right_hand;

    VkInstance       vk_instance;
    VkPhysicalDevice vk_physical;
    VkDevice         vk_device;
    VkQueue          queue;
    u32              queue_family;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;

    XrSwapchain      swapchain;
    VkImage          images[MAX_IMAGES];
    u32              images_count;
    u32              width;
    u32              height;

    u32              frame;
};

} // namespace

//
// Statics
//

static b8 check (XrResult result, const char* what)
{
    if (XR_FAILED(result))
    {
        (void)fprintf(stderr, "puppetxr_example: %s failed with %d\n", what, (int)result);
        return false;
    }
    return true;
}

static b8 parse_options (int argc, const char* const* argv, Options& out)
{
    out = {};
    for (int i = 1; i < argc; i++)
    {
        const b8 has_value = i + 1 < argc;
        if (strcmp(argv[i], "--frames") == 0 && has_value)
        {
            out.frames = (u32)strtoul(argv[++i], nullptr, 10);
        }
        else if (strcmp(argv[i], "--script") == 0)
        {
            out.script = true;
        }
        else if (strcmp(argv[i], "--log") == 0)
        {
            out.log = true;
        }
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 2 < argc)
        {
            out.screenshot       = argv[++i];
            out.screenshot_frame = (u32)strtoul(argv[++i], nullptr, 10);
        }
        else
        {
            (void)fprintf(stderr, "usage: puppetxr_example [--frames N] [--script] [--log] [--screenshot PATH FRAME]\n");
            return false;
        }
    }
    return true;
}

static b8 create_instance (App& app)
{
    const char*          extensions[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    XrInstanceCreateInfo info         = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(info.applicationInfo.applicationName, "puppetxr_example");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount      = 1;
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

    app.has_puppetxr = puppetxr_load_api(&app.puppetxr) == PUPPETXR_SUCCESS;
    if (app.options.script && !app.has_puppetxr)
    {
        (void)fprintf(stderr, "puppetxr_example: --script needs the puppetxr runtime\n");
        return false;
    }
    return true;
}

static b8 create_vulkan (App& app)
{
    if (volkInitialize() != VK_SUCCESS)
    {
        (void)fprintf(stderr, "puppetxr_example: no Vulkan loader\n");
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
        (void)fprintf(stderr, "puppetxr_example: XR_KHR_vulkan_enable2 functions missing\n");
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    if (!check(get_requirements(app.instance, app.system, &requirements), "xrGetVulkanGraphicsRequirements2KHR"))
    {
        return false;
    }

    VkResult                            vk_result        = VK_SUCCESS;
    const VkApplicationInfo             app_info         = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "puppetxr_example", .apiVersion = VK_API_VERSION_1_1 };
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
    app.queue_family = 0;
    while (app.queue_family < family_count && !(families[app.queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    {
        app.queue_family++;
    }
    if (app.queue_family == family_count)
    {
        (void)fprintf(stderr, "puppetxr_example: no graphics queue\n");
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
    vkGetDeviceQueue(app.vk_device, app.queue_family, 0, &app.queue);

    const VkCommandPoolCreateInfo     pool_info  = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = app.queue_family };
    const VkFenceCreateInfo           fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateCommandPool(app.vk_device, &pool_info, nullptr, &app.pool) != VK_SUCCESS || vkCreateFence(app.vk_device, &fence_info, nullptr, &app.fence) != VK_SUCCESS)
    {
        return false;
    }
    const VkCommandBufferAllocateInfo cmd_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = app.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    return vkAllocateCommandBuffers(app.vk_device, &cmd_info, &app.cmd) == VK_SUCCESS;
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
    if (!check(xrCreateReferenceSpace(app.session, &space_info, &app.stage), "xrCreateReferenceSpace"))
    {
        return false;
    }

    XrViewConfigurationView views[2]     = { { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW }, { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    u32                     views_count = 0;
    if (!check(xrEnumerateViewConfigurationViews(app.instance, app.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &views_count, views), "xrEnumerateViewConfigurationViews"))
    {
        return false;
    }
    app.width  = views[0].recommendedImageRectWidth;
    app.height = views[0].recommendedImageRectHeight;

    const XrSwapchainCreateInfo swapchain_info = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT, .format = VK_FORMAT_R8G8B8A8_SRGB, .sampleCount = 1, .width = app.width, .height = app.height, .faceCount = 1, .arraySize = 2, .mipCount = 1 };
    if (!check(xrCreateSwapchain(app.session, &swapchain_info, &app.swapchain), "xrCreateSwapchain"))
    {
        return false;
    }
    XrSwapchainImageVulkan2KHR images[MAX_IMAGES] = {};
    for (u32 i = 0; i < MAX_IMAGES; i++)
    {
        images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    }
    if (!check(xrEnumerateSwapchainImages(app.swapchain, MAX_IMAGES, &app.images_count, (XrSwapchainImageBaseHeader*)images), "xrEnumerateSwapchainImages"))
    {
        return false;
    }
    for (u32 i = 0; i < app.images_count; i++)
    {
        app.images[i] = images[i].image;
    }
    return true;
}

static b8 create_actions (App& app)
{
    XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(set_info.actionSetName, "example");
    strcpy(set_info.localizedActionSetName, "Example");
    if (!check(xrCreateActionSet(app.instance, &set_info, &app.action_set), "xrCreateActionSet"))
    {
        return false;
    }

    xrStringToPath(app.instance, "/user/hand/right", &app.right_hand);
    XrActionCreateInfo action_info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionType = XR_ACTION_TYPE_FLOAT_INPUT, .countSubactionPaths = 1, .subactionPaths = &app.right_hand };
    strcpy(action_info.actionName, "trigger");
    strcpy(action_info.localizedActionName, "Trigger");
    if (!check(xrCreateAction(app.action_set, &action_info, &app.trigger), "xrCreateAction"))
    {
        return false;
    }

    XrPath touch = XR_NULL_PATH;
    XrPath trigger_path = XR_NULL_PATH;
    xrStringToPath(app.instance, "/interaction_profiles/oculus/touch_controller", &touch);
    xrStringToPath(app.instance, "/user/hand/right/input/trigger/value", &trigger_path);
    const XrActionSuggestedBinding             binding   = { app.trigger, trigger_path };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = touch, .countSuggestedBindings = 1, .suggestedBindings = &binding };
    if (!check(xrSuggestInteractionProfileBindings(app.instance, &suggested), "xrSuggestInteractionProfileBindings"))
    {
        return false;
    }
    const XrSessionActionSetsAttachInfo attach = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &app.action_set };
    return check(xrAttachSessionActionSets(app.session, &attach), "xrAttachSessionActionSets");
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
            else if (app.state == XR_SESSION_STATE_STOPPING)
            {
                xrEndSession(app.session);
                app.running = false;
            }
        }
        event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// The scripted user: a deterministic function of the frame number
static void script_input (const App& app)
{
    const f32        trigger = (f32)(app.frame % 20) / 19.0f;
    const PUPPETXR_Pose head = { { 0.01f * (f32)app.frame, 1.6f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } };
    app.puppetxr.set_input(PUPPETXR_DEVICE_RIGHT_HAND, "trigger/value", trigger, 0.0f);
    app.puppetxr.set_device_pose(PUPPETXR_DEVICE_HEAD, &head);
}

static void clear_eyes (const App& app, VkImage image, const VkClearColorValue& color)
{
    vkResetFences(app.vk_device, 1, &app.fence);
    const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(app.cmd, &begin);
    const VkImageSubresourceRange range   = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2 };
    VkImageMemoryBarrier          barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = range };
    vkCmdPipelineBarrier(app.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkCmdClearColorImage(app.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(app.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkEndCommandBuffer(app.cmd);
    const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &app.cmd };
    vkQueueSubmit(app.queue, 1, &submit, app.fence);
    vkWaitForFences(app.vk_device, 1, &app.fence, VK_TRUE, UINT64_MAX);
}

static b8 render_frame (App& app)
{
    if (app.options.script)
    {
        script_input(app);
    }

    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    if (!check(xrWaitFrame(app.session, nullptr, &state), "xrWaitFrame") || !check(xrBeginFrame(app.session, nullptr), "xrBeginFrame"))
    {
        return false;
    }

    const XrActiveActionSet active = { app.action_set, XR_NULL_PATH };
    const XrActionsSyncInfo sync   = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active };
    xrSyncActions(app.session, &sync);
    XrActionStateFloat         trigger  = { .type = XR_TYPE_ACTION_STATE_FLOAT };
    const XrActionStateGetInfo get_info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = app.trigger, .subactionPath = app.right_hand };
    xrGetActionStateFloat(app.session, &get_info, &trigger);

    const XrViewLocateInfo locate     = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = state.predictedDisplayTime, .space = app.stage };
    XrViewState      view_state = { .type = XR_TYPE_VIEW_STATE };
    XrView           views[2]   = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
    u32              count      = 0;
    xrLocateViews(app.session, &locate, &view_state, 2, &count, views);

    u32 layers_count = 0;
    XrCompositionLayerProjectionView projection_views[2] = {};
    XrCompositionLayerProjection     layer               = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    if (state.shouldRender)
    {
        u32 index = 0;
        xrAcquireSwapchainImage(app.swapchain, nullptr, &index);
        const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        xrWaitSwapchainImage(app.swapchain, &wait_info);

        const f32               head_x = views[0].pose.position.x;
        const VkClearColorValue color  = { { trigger.currentState, 0.2f, head_x > 1.0f ? 1.0f : (head_x < 0.0f ? 0.0f : head_x), 1.0f } };
        clear_eyes(app, app.images[index], color);
        xrReleaseSwapchainImage(app.swapchain, nullptr);

        for (u32 eye = 0; eye < 2; eye++)
        {
            projection_views[eye] = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = views[eye].pose, .fov = views[eye].fov, .subImage = { .swapchain = app.swapchain, .imageRect = { { 0, 0 }, { (int32_t)app.width, (int32_t)app.height } }, .imageArrayIndex = eye } };
        }
        layer.space     = app.stage;
        layer.viewCount = 2;
        layer.views     = projection_views;
        layers_count    = 1;
    }

    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = layers_count, .layers = layers };
    if (!check(xrEndFrame(app.session, &end_info), "xrEndFrame"))
    {
        return false;
    }

    if (app.options.log)
    {
        printf("frame %u time %lld trigger %.4f head_x %.5f\n", app.frame, (long long)state.predictedDisplayTime, (double)trigger.currentState, (double)views[0].pose.position.x);
        (void)fflush(stdout);
    }
    const b8 screenshot_frame = app.options.screenshot && app.frame == app.options.screenshot_frame;
    if (screenshot_frame && (!app.has_puppetxr || app.puppetxr.screenshot(app.options.screenshot) != PUPPETXR_SUCCESS))
    {
        (void)fprintf(stderr, "puppetxr_example: screenshot failed\n");
        return false;
    }

    app.frame++;
    if (app.options.frames && app.frame >= app.options.frames && !app.exit_requested)
    {
        xrRequestExitSession(app.session);
        app.exit_requested = true;
    }
    return true;
}

static void destroy (const App& app)
{
    if (app.vk_device)
    {
        vkDeviceWaitIdle(app.vk_device);
    }
    if (app.swapchain)
    {
        xrDestroySwapchain(app.swapchain);
    }
    if (app.session)
    {
        xrDestroySession(app.session);
    }
    if (app.fence)
    {
        vkDestroyFence(app.vk_device, app.fence, nullptr);
    }
    if (app.pool)
    {
        vkDestroyCommandPool(app.vk_device, app.pool, nullptr);
    }
    if (app.vk_device)
    {
        vkDestroyDevice(app.vk_device, nullptr);
    }
    if (app.vk_instance)
    {
        vkDestroyInstance(app.vk_instance, nullptr);
    }
    if (app.instance)
    {
        xrDestroyInstance(app.instance);
    }
}

//
// Entry
//

int main (int argc, char** argv) // NOLINT(misc-const-correctness): the signature of main
{
    static App app = {};
    if (!parse_options(argc, argv, app.options))
    {
        return 2;
    }

    b8 ok = create_instance(app) && create_vulkan(app) && create_session(app) && create_actions(app);
    while (ok)
    {
        poll_events(app);
        if (app.state == XR_SESSION_STATE_EXITING || app.state == XR_SESSION_STATE_LOSS_PENDING)
        {
            break;
        }
        if (app.running)
        {
            ok = render_frame(app);
        }
    }

    destroy(app);
    return ok ? 0 : 1;
}
