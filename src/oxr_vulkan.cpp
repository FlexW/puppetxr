#include "oxr_internal.h"

#include <volk.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

//
// Types
//

#define OXR_VK_MIN_API XR_MAKE_VERSION(1, 0, 0)
#define OXR_VK_MAX_API XR_MAKE_VERSION(1, 4, 0)
#define OXR_VK_MAX_PHYSICAL_DEVICES 8

// Flags the runtime can add to its images; any other bit needs a device extension or feature puppetxr does not enable
#define OXR_VK_EXTRA_CREATE_FLAGS ((u32)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)
#define OXR_VK_EXTRA_USAGE_FLAGS  ((u32)(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))

//
// Statics
//

static b8 vk_filter_valid (VkFilter filter)
{
    return filter == VK_FILTER_NEAREST || filter == VK_FILTER_LINEAR;
}

static b8 vk_wrap_valid (VkSamplerAddressMode mode)
{
    return (u32)mode <= (u32)VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
}

static b8 vk_swizzle_valid (VkComponentSwizzle swizzle)
{
    return (u32)swizzle <= (u32)VK_COMPONENT_SWIZZLE_A;
}

static PFN_vkGetInstanceProcAddr instance_gipa (const OXR_Instance* inst)
{
    if (inst->vk_get_instance_proc_addr)
    {
        return (PFN_vkGetInstanceProcAddr)inst->vk_get_instance_proc_addr;
    }
    // The app holds its loader, so the pointer outlives our reference
    OS_Library*                     loader   = os_vulkan_loader();
    const PFN_vkGetInstanceProcAddr app_gipa = loader ? (PFN_vkGetInstanceProcAddr)os_library_symbol(loader, "vkGetInstanceProcAddr") : nullptr;
    os_library_close(loader);
    if (app_gipa)
    {
        volkInitializeCustom(app_gipa);
    }
    else if (volkInitialize() != VK_SUCCESS)
    {
        return nullptr;
    }
    return vkGetInstanceProcAddr;
}

static b8 device_can_present (PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice device, VkSurfaceKHR surface)
{
    const PFN_vkGetPhysicalDeviceQueueFamilyProperties get_families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    const PFN_vkGetPhysicalDeviceSurfaceSupportKHR     get_support  = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (!surface || !get_families || !get_support)
    {
        return false;
    }
    u32 count = 0;
    get_families(device, &count, nullptr);
    for (u32 i = 0; i < count; i++)
    {
        VkBool32 supported = VK_FALSE;
        if (get_support(device, i, surface, &supported) == VK_SUCCESS && supported)
        {
            return true;
        }
    }
    return false;
}

static VkPhysicalDevice pick_physical_device (PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkSurfaceKHR surface)
{
    const PFN_vkEnumeratePhysicalDevices    enumerate      = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    const PFN_vkGetPhysicalDeviceProperties get_properties = (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    if (!enumerate || !get_properties)
    {
        return VK_NULL_HANDLE;
    }

    VkPhysicalDevice devices[OXR_VK_MAX_PHYSICAL_DEVICES] = {};
    u32              count                                 = OXR_VK_MAX_PHYSICAL_DEVICES;
    if (enumerate(instance, &count, devices) < VK_SUCCESS || count == 0)
    {
        return VK_NULL_HANDLE;
    }

    VkPhysicalDevice best       = devices[0];
    i32              best_score = -1;
    for (u32 i = 0; i < count; i++)
    {
        VkPhysicalDeviceProperties properties = {};
        get_properties(devices[i], &properties);
        i32 score = 0;
        switch (properties.deviceType)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score = 3;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            score = 2;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            score = 1;
            break;
        default:
            score = 0;
            break;
        }
        if (device_can_present(gipa, instance, devices[i], surface))
        {
            score += 10;
        }
        if (score > best_score)
        {
            best       = devices[i];
            best_score = score;
        }
    }
    return best;
}

// Extensions the runtime needs on the app's instance and device for the mirror window
static u32 required_instance_extensions (const char** out_names, u32 capacity)
{
    u32 count = 0;
    if (os_is_headless())
    {
        return 0;
    }
    const char* const* window_names = nullptr;
    u32                window_count = 0;
    os_window_vulkan_extensions(window_names, window_count);
    for (u32 i = 0; i < window_count && count < capacity; i++)
    {
        out_names[count++] = window_names[i];
    }
    return count;
}

static u32 required_device_extensions (const char** out_names, u32 capacity)
{
    if (os_is_headless() || capacity == 0)
    {
        return 0;
    }
    out_names[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    return 1;
}

static void join_extensions (const char* const* names, u32 count, char* out, usize out_size)
{
    out[0] = 0;
    for (u32 i = 0; i < count; i++)
    {
        if (i > 0)
        {
            cstr_copy(out + cstr_length(out), out_size - cstr_length(out), " ");
        }
        cstr_copy(out + cstr_length(out), out_size - cstr_length(out), names[i]);
    }
}

// Appends `extra` to the app's enabled extension list, skipping duplicates
static u32 merge_extensions (const char* const* app_names, u32 app_count, const char* const* extra, u32 extra_count, const char** out, u32 capacity)
{
    u32 count = 0;
    for (u32 i = 0; i < app_count && count < capacity; i++)
    {
        out[count++] = app_names[i];
    }
    for (u32 i = 0; i < extra_count && count < capacity; i++)
    {
        b8 present = false;
        for (u32 k = 0; k < app_count; k++)
        {
            present |= cstr_equal(app_names[k], extra[i]);
        }
        if (!present)
        {
            out[count++] = extra[i];
        }
    }
    return count;
}

static b8 has_extension (const VkExtensionProperties* properties, u32 count, const char* name)
{
    for (u32 i = 0; i < count; i++)
    {
        if (cstr_equal(properties[i].extensionName, name))
        {
            return true;
        }
    }
    return false;
}

// Vulkan 1.3.216: the loader lists portability drivers such as MoltenVK only to an instance that
// asks for them; VK_KHR_portability_subset needs VK_KHR_get_physical_device_properties2 on 1.0
static u32 portability_instance_extensions (PFN_vkGetInstanceProcAddr get_proc, const char** out_names, u32 capacity)
{
    const PFN_vkEnumerateInstanceExtensionProperties enumerate = (PFN_vkEnumerateInstanceExtensionProperties)get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    u32 properties_count = 0;
    if (!enumerate || enumerate(nullptr, &properties_count, nullptr) < 0 || properties_count == 0)
    {
        return 0;
    }
    VkExtensionProperties* properties = (VkExtensionProperties*)os_memory_alloc(properties_count * sizeof(VkExtensionProperties));
    defer { os_memory_release(properties); };
    if (enumerate(nullptr, &properties_count, properties) < 0 || !has_extension(properties, properties_count, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    {
        return 0;
    }
    u32 count = 0;
    if (count < capacity)
    {
        out_names[count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    }
    if (count < capacity && has_extension(properties, properties_count, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
    {
        out_names[count++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    }
    return count;
}

// Vulkan spec, VK_KHR_portability_subset: a device that offers it must have it enabled
static u32 portability_device_extensions (PFN_vkGetInstanceProcAddr get_proc, VkInstance instance, VkPhysicalDevice physical, const char** out_names, u32 capacity)
{
    const PFN_vkEnumerateDeviceExtensionProperties enumerate = (PFN_vkEnumerateDeviceExtensionProperties)get_proc(instance, "vkEnumerateDeviceExtensionProperties");
    u32 properties_count = 0;
    if (capacity == 0 || !enumerate || enumerate(physical, nullptr, &properties_count, nullptr) < 0 || properties_count == 0)
    {
        return 0;
    }
    VkExtensionProperties* properties = (VkExtensionProperties*)os_memory_alloc(properties_count * sizeof(VkExtensionProperties));
    defer { os_memory_release(properties); };
    if (enumerate(physical, nullptr, &properties_count, properties) < 0 || !has_extension(properties, properties_count, "VK_KHR_portability_subset"))
    {
        return 0;
    }
    out_names[0] = "VK_KHR_portability_subset";
    return 1;
}

static XrResult graphics_requirements (XrInstance instance, XrSystemId system, XrGraphicsRequirementsVulkanKHR* requirements)
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    if (system != 1)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    OXR_CHECK_TYPE(requirements, XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR);
    requirements->minApiVersionSupported = OXR_VK_MIN_API;
    requirements->maxApiVersionSupported = OXR_VK_MAX_API;
    inst->vk_requirements_called         = true;
    return XR_SUCCESS;
}

static XrResult extension_string (XrInstance instance, XrSystemId system, uint32_t capacity, uint32_t* count_output, char* buffer, b8 device_level)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    UNUSED(inst);
    if (system != 1)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    const char* names[16];
    const u32   names_count = device_level ? required_device_extensions(names, 16) : required_instance_extensions(names, 16);
    char        extensions[1024];
    join_extensions(names, names_count, extensions, sizeof(extensions));
    const u32      length     = (u32)cstr_length(extensions) + 1;
    const XrResult result     = oxr_two_call(capacity, count_output, buffer, length);
    if (result == XR_SUCCESS && capacity > 0)
    {
        cstr_copy(buffer, capacity, extensions);
    }
    return result;
}

//
// XR_KHR_vulkan_enable
//

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetVulkanInstanceExtensionsKHR (XrInstance instance, XrSystemId system, uint32_t capacity, uint32_t* count_output, char* buffer)
{
    return extension_string(instance, system, capacity, count_output, buffer, false);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetVulkanDeviceExtensionsKHR (XrInstance instance, XrSystemId system, uint32_t capacity, uint32_t* count_output, char* buffer)
{
    return extension_string(instance, system, capacity, count_output, buffer, true);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetVulkanGraphicsDeviceKHR (XrInstance instance, XrSystemId system, VkInstance vk_instance, VkPhysicalDevice* out_device)
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    if (system != 1)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    OXR_CHECK(vk_instance && out_device);

    const PFN_vkGetInstanceProcAddr gipa = instance_gipa(inst);
    if (!gipa)
    {
        LOG_ERROR("No Vulkan loader to query the physical device with");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    inst->vk_instance = vk_instance;
    oxr_vk_ensure_surface(inst);
    const VkPhysicalDevice device = pick_physical_device(gipa, vk_instance, (VkSurfaceKHR)inst->vk_surface);
    if (!device)
    {
        LOG_ERROR("No Vulkan physical device found");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    inst->vk_physical_device = device;
    *out_device              = device;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetVulkanGraphicsRequirementsKHR (XrInstance instance, XrSystemId system, XrGraphicsRequirementsVulkanKHR* requirements)
{
    return graphics_requirements(instance, system, requirements);
}

//
// XR_KHR_vulkan_enable2
//

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateVulkanInstanceKHR (XrInstance instance, const XrVulkanInstanceCreateInfoKHR* info, VkInstance* out_instance, VkResult* out_result)
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    OXR_CHECK_TYPE(info, XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR);
    OXR_CHECK(out_instance && out_result && info->pfnGetInstanceProcAddr && info->vulkanCreateInfo);
    *out_instance = VK_NULL_HANDLE;
    *out_result   = VK_ERROR_INITIALIZATION_FAILED;
    if (info->systemId != 1)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }

    const PFN_vkCreateInstance create = (PFN_vkCreateInstance)info->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create)
    {
        LOG_ERROR("The app's vkGetInstanceProcAddr has no vkCreateInstance");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    const char* required[16];
    u32         required_count     = required_instance_extensions(required, 16);
    const u32   portability_count  = portability_instance_extensions(info->pfnGetInstanceProcAddr, required + required_count, 16 - required_count);
    required_count                += portability_count;
    const char* merged[64];
    const u32   merged_count = merge_extensions(info->vulkanCreateInfo->ppEnabledExtensionNames, info->vulkanCreateInfo->enabledExtensionCount, required, required_count, merged, 64);
    VkInstanceCreateInfo create_info    = *info->vulkanCreateInfo;
    create_info.enabledExtensionCount   = merged_count;
    create_info.ppEnabledExtensionNames = merged;
    if (portability_count > 0)
    {
        create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    *out_result = create(&create_info, info->vulkanAllocator, out_instance);
    if (*out_result != VK_SUCCESS)
    {
        LOG_ERROR("vkCreateInstance failed with %d", (i32)*out_result);
        return XR_SUCCESS;
    }
    inst->vk_get_instance_proc_addr = (void*)info->pfnGetInstanceProcAddr;
    inst->vk_instance               = *out_instance;
    oxr_vk_ensure_surface(inst);
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrCreateVulkanDeviceKHR (XrInstance instance, const XrVulkanDeviceCreateInfoKHR* info, VkDevice* out_device, VkResult* out_result)
{
    const OXR_Lock lock(__func__);
    OXR_GET_CONST_INSTANCE(inst, instance);
    OXR_CHECK_TYPE(info, XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR);
    OXR_CHECK(out_device && out_result && info->pfnGetInstanceProcAddr && info->vulkanCreateInfo && info->vulkanPhysicalDevice);
    *out_device = VK_NULL_HANDLE;
    *out_result = VK_ERROR_INITIALIZATION_FAILED;
    if (info->systemId != 1)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (info->vulkanPhysicalDevice != inst->vk_physical_device)
    {
        return XR_ERROR_HANDLE_INVALID;
    }

    const PFN_vkCreateDevice create = (PFN_vkCreateDevice)info->pfnGetInstanceProcAddr((VkInstance)inst->vk_instance, "vkCreateDevice");
    if (!create)
    {
        LOG_ERROR("The app's vkGetInstanceProcAddr has no vkCreateDevice for this instance");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    const char* required[16];
    u32         required_count  = required_device_extensions(required, 16);
    required_count             += portability_device_extensions(info->pfnGetInstanceProcAddr, (VkInstance)inst->vk_instance, info->vulkanPhysicalDevice, required + required_count, 16 - required_count);
    const char* merged[64];
    const u32   merged_count = merge_extensions(info->vulkanCreateInfo->ppEnabledExtensionNames, info->vulkanCreateInfo->enabledExtensionCount, required, required_count, merged, 64);
    VkDeviceCreateInfo create_info      = *info->vulkanCreateInfo;
    create_info.enabledExtensionCount   = merged_count;
    create_info.ppEnabledExtensionNames = merged;

    *out_result = create(info->vulkanPhysicalDevice, &create_info, info->vulkanAllocator, out_device);
    if (*out_result != VK_SUCCESS)
    {
        LOG_ERROR("vkCreateDevice failed with %d", (i32)*out_result);
    }
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetVulkanGraphicsDevice2KHR (XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR* info, VkPhysicalDevice* out_device)
{
    {
        const OXR_Lock lock(__func__);
        OXR_GET_CONST_INSTANCE(inst, instance);
        UNUSED(inst);
        OXR_CHECK_TYPE(info, XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR);
    }
    return oxr_xrGetVulkanGraphicsDeviceKHR(instance, info->systemId, info->vulkanInstance, out_device);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetVulkanGraphicsRequirements2KHR (XrInstance instance, XrSystemId system, XrGraphicsRequirementsVulkan2KHR* requirements)
{
    return graphics_requirements(instance, system, requirements);
}

//
// Public API (layer internal)
//

PFN_xrVoidFunction oxr_vk_get_proc (const char* name, b8 vulkan_enable, b8 vulkan_enable2)
{
    struct Entry
    {
        const char*        name;
        PFN_xrVoidFunction function;
        b8                 enable2;
    };
    static const Entry entries[] = {
        { "xrGetVulkanInstanceExtensionsKHR",    (PFN_xrVoidFunction)static_cast<PFN_xrGetVulkanInstanceExtensionsKHR>(oxr_xrGetVulkanInstanceExtensionsKHR),    false },
        { "xrGetVulkanDeviceExtensionsKHR",      (PFN_xrVoidFunction)static_cast<PFN_xrGetVulkanDeviceExtensionsKHR>(oxr_xrGetVulkanDeviceExtensionsKHR),      false },
        { "xrGetVulkanGraphicsDeviceKHR",        (PFN_xrVoidFunction)static_cast<PFN_xrGetVulkanGraphicsDeviceKHR>(oxr_xrGetVulkanGraphicsDeviceKHR),        false },
        { "xrGetVulkanGraphicsRequirementsKHR",  (PFN_xrVoidFunction)static_cast<PFN_xrGetVulkanGraphicsRequirementsKHR>(oxr_xrGetVulkanGraphicsRequirementsKHR),  false },
        { "xrCreateVulkanInstanceKHR",           (PFN_xrVoidFunction)static_cast<PFN_xrCreateVulkanInstanceKHR>(oxr_xrCreateVulkanInstanceKHR),           true },
        { "xrCreateVulkanDeviceKHR",             (PFN_xrVoidFunction)static_cast<PFN_xrCreateVulkanDeviceKHR>(oxr_xrCreateVulkanDeviceKHR),             true },
        { "xrGetVulkanGraphicsDevice2KHR",       (PFN_xrVoidFunction)static_cast<PFN_xrGetVulkanGraphicsDevice2KHR>(oxr_xrGetVulkanGraphicsDevice2KHR),       true },
        { "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction)static_cast<PFN_xrGetVulkanGraphicsRequirements2KHR>(oxr_xrGetVulkanGraphicsRequirements2KHR), true },
    };
    for (const Entry& entry : entries)
    {
        const b8 enabled = entry.enable2 ? vulkan_enable2 : vulkan_enable;
        if (enabled && cstr_equal(name, entry.name))
        {
            return entry.function;
        }
    }
    return nullptr;
}

XrResult oxr_vk_create_session_graphics (const OXR_Instance* inst, const XrBaseInStructure* graphics_binding, R_Device*& out_device)
{
    out_device = nullptr;
    const XrGraphicsBindingVulkanKHR* binding = (const XrGraphicsBindingVulkanKHR*)graphics_binding;
    if (!inst->vk_requirements_called)
    {
        return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    }
    if (!binding->instance || !binding->physicalDevice || !binding->device)
    {
        LOG_ERROR("The Vulkan graphics binding has a null instance, physical device or device");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    if (binding->physicalDevice != inst->vk_physical_device)
    {
        LOG_ERROR("Session physical device differs from the one returned by xrGetVulkanGraphicsDevice(2)KHR");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }

    const PFN_vkGetInstanceProcAddr gipa = instance_gipa(inst);
    if (!gipa)
    {
        LOG_ERROR("No Vulkan loader for the session's graphics binding");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    const PFN_vkGetPhysicalDeviceQueueFamilyProperties get_families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(binding->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    u32 family_count = 0;
    if (get_families)
    {
        get_families(binding->physicalDevice, &family_count, nullptr);
    }
    if (binding->queueFamilyIndex >= family_count)
    {
        LOG_ERROR("Queue family %u is out of range (%u families)", binding->queueFamilyIndex, family_count);
        return XR_ERROR_VALIDATION_FAILURE;
    }

    out_device = r_create_device({
        .backend = R_Backend::Vulkan,
        .vulkan  = {
            .get_instance_proc_addr = (void*)gipa,
            .instance               = binding->instance,
            .physical_device        = binding->physicalDevice,
            .device                 = binding->device,
            .queue_family_index     = binding->queueFamilyIndex,
            .queue_index            = binding->queueIndex,
        },
    });
    if (!out_device)
    {
        LOG_ERROR("Cannot wrap the app's Vulkan device");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    return XR_SUCCESS;
}

void oxr_vk_ensure_surface (OXR_Instance* inst)
{
    const OXR_Runtime* rt = oxr_get_runtime();
    if (inst->vk_surface || !rt->window || !inst->vk_instance)
    {
        return;
    }
    if (!os_window_create_vulkan_surface(rt->window, inst->vk_instance, inst->vk_get_instance_proc_addr, inst->vk_surface))
    {
        LOG_WARNING("No window surface; the mirror window stays blank");
    }
}

void oxr_vk_release_surface (OXR_Instance* inst)
{
    os_window_destroy_vulkan_surface(inst->vk_instance, inst->vk_surface);
    inst->vk_surface = 0;
}

XrResult oxr_vk_write_swapchain_images (const u64* native_images, u32 count, XrSwapchainImageBaseHeader* images)
{
    XrSwapchainImageVulkanKHR* out = (XrSwapchainImageVulkanKHR*)images;
    for (u32 i = 0; i < count; i++)
    {
        OXR_CHECK_TYPE(&out[i], XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR);
        out[i].image = (VkImage)native_images[i];
    }
    return XR_SUCCESS;
}

XrResult oxr_vk_swapchain_extra_flags (const XrBaseInStructure* chain, u32& out_create_flags, u32& out_usage_flags)
{
    out_create_flags = 0;
    out_usage_flags  = 0;
    for (const XrBaseInStructure* next = chain; next; next = next->next)
    {
        if (next->type != XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META)
        {
            continue;
        }
        const XrVulkanSwapchainCreateInfoMETA* info = (const XrVulkanSwapchainCreateInfoMETA*)next;
        if ((info->additionalCreateFlags & ~(VkImageCreateFlags)OXR_VK_EXTRA_CREATE_FLAGS) != 0 || (info->additionalUsageFlags & ~(VkImageUsageFlags)OXR_VK_EXTRA_USAGE_FLAGS) != 0)
        {
            return XR_ERROR_FEATURE_UNSUPPORTED;
        }
        out_create_flags |= (u32)info->additionalCreateFlags;
        out_usage_flags  |= (u32)info->additionalUsageFlags;
    }
    return XR_SUCCESS;
}

OXR_SamplerState oxr_vk_default_sampler_state ()
{
    return {
        .min_filter     = VK_FILTER_LINEAR,
        .mag_filter     = VK_FILTER_LINEAR,
        .mipmap_mode    = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .wrap_s         = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .wrap_t         = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .swizzle        = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .max_anisotropy = 1.0f,
        .border         = { 0.0f, 0.0f, 0.0f, 0.0f },
    };
}

XrResult oxr_vk_set_sampler_state (OXR_SamplerState& state, const XrSwapchainStateBaseHeaderFB* header)
{
    OXR_CHECK_TYPE(header, XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB);
    const XrSwapchainStateSamplerVulkanFB* sampler = (const XrSwapchainStateSamplerVulkanFB*)header;
    OXR_CHECK(vk_filter_valid(sampler->minFilter) && vk_filter_valid(sampler->magFilter));
    OXR_CHECK(sampler->mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST || sampler->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR);
    OXR_CHECK(vk_wrap_valid(sampler->wrapModeS) && vk_wrap_valid(sampler->wrapModeT));
    OXR_CHECK(vk_swizzle_valid(sampler->swizzleRed) && vk_swizzle_valid(sampler->swizzleGreen) && vk_swizzle_valid(sampler->swizzleBlue) && vk_swizzle_valid(sampler->swizzleAlpha));
    OXR_CHECK(sampler->maxAnisotropy >= 1.0f && zm::is_finite(sampler->maxAnisotropy)); // false for NaN
    const XrColor4f& border = sampler->borderColor;
    OXR_CHECK(zm::is_finite(border.r) && zm::is_finite(border.g) && zm::is_finite(border.b) && zm::is_finite(border.a));
    state = {
        .min_filter     = (u32)sampler->minFilter,
        .mag_filter     = (u32)sampler->magFilter,
        .mipmap_mode    = (u32)sampler->mipmapMode,
        .wrap_s         = (u32)sampler->wrapModeS,
        .wrap_t         = (u32)sampler->wrapModeT,
        .swizzle        = { (u32)sampler->swizzleRed, (u32)sampler->swizzleGreen, (u32)sampler->swizzleBlue, (u32)sampler->swizzleAlpha },
        .max_anisotropy = sampler->maxAnisotropy,
        .border         = { border.r, border.g, border.b, border.a },
    };
    return XR_SUCCESS;
}

XrResult oxr_vk_get_sampler_state (const OXR_SamplerState& state, XrSwapchainStateBaseHeaderFB* header)
{
    OXR_CHECK_TYPE(header, XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB);
    XrSwapchainStateSamplerVulkanFB* sampler = (XrSwapchainStateSamplerVulkanFB*)header;
    sampler->minFilter     = (VkFilter)state.min_filter;
    sampler->magFilter     = (VkFilter)state.mag_filter;
    sampler->mipmapMode    = (VkSamplerMipmapMode)state.mipmap_mode;
    sampler->wrapModeS     = (VkSamplerAddressMode)state.wrap_s;
    sampler->wrapModeT     = (VkSamplerAddressMode)state.wrap_t;
    sampler->swizzleRed    = (VkComponentSwizzle)state.swizzle[0];
    sampler->swizzleGreen  = (VkComponentSwizzle)state.swizzle[1];
    sampler->swizzleBlue   = (VkComponentSwizzle)state.swizzle[2];
    sampler->swizzleAlpha  = (VkComponentSwizzle)state.swizzle[3];
    sampler->maxAnisotropy = state.max_anisotropy;
    sampler->borderColor   = { state.border[0], state.border[1], state.border[2], state.border[3] };
    return XR_SUCCESS;
}
