#include "oxr_internal.h"

#define XR_USE_GRAPHICS_API_METAL
#include <openxr/openxr_platform.h>

//
// Statics
//

// Metal apps should make their queue on the device the requirements name: the system default
static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetMetalGraphicsRequirementsKHR (XrInstance instance, XrSystemId system, XrGraphicsRequirementsMetalKHR* out_requirements)
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    if (system != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    OXR_CHECK_TYPE(out_requirements, XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR);
    out_requirements->metalDevice = r_metal_default_device();
    if (!out_requirements->metalDevice)
    {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    inst->metal_requirements_called = true;
    return XR_SUCCESS;
}

//
// Public API (layer internal)
//

PFN_xrVoidFunction oxr_metal_get_proc (const char* name, b8 metal_enable)
{
    // XR_KHRX2_metal_enable, the provisional extension Unity's OpenXR plugin asks for, names it
    // with its own suffix and has the same structures
    if (metal_enable && (cstr_equal(name, "xrGetMetalGraphicsRequirementsKHR") || cstr_equal(name, "xrGetMetalGraphicsRequirementsKHRX2")))
    {
        return (PFN_xrVoidFunction)static_cast<PFN_xrGetMetalGraphicsRequirementsKHR>(oxr_xrGetMetalGraphicsRequirementsKHR);
    }
    return nullptr;
}

b8 oxr_metal_is_binding (const OXR_Instance* inst, XrStructureType type)
{
    return type == XR_TYPE_GRAPHICS_BINDING_METAL_KHR && (inst->ext.KHR_metal_enable || inst->ext.KHRX2_metal_enable);
}

XrResult oxr_metal_create_session_graphics (const OXR_Instance* inst, const XrBaseInStructure* graphics_binding, R_Device*& out_device)
{
    out_device                               = nullptr;
    const XrGraphicsBindingMetalKHR* binding = (const XrGraphicsBindingMetalKHR*)graphics_binding;
    if (!inst->metal_requirements_called)
    {
        return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    }
    if (!binding->commandQueue)
    {
        LOG_ERROR("The Metal graphics binding has a null command queue");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    out_device = r_create_device({ .backend = R_Backend::Metal, .metal = { .queue = binding->commandQueue } });
    if (!out_device)
    {
        LOG_ERROR("Cannot use the app's Metal command queue");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    return XR_SUCCESS;
}

XrResult oxr_metal_write_swapchain_images (const u64* native_images, u32 count, XrSwapchainImageBaseHeader* images)
{
    XrSwapchainImageMetalKHR* out = (XrSwapchainImageMetalKHR*)images;
    for (u32 i = 0; i < count; i++)
    {
        OXR_CHECK_TYPE(&out[i], XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR);
        out[i].texture = (void*)(uintptr_t)native_images[i];
    }
    return XR_SUCCESS;
}
