#include "oxr_internal.h"

// The types openxr_platform.h refers to, declared as the Windows SDK does; puppetxr only passes the
// interfaces on to the render layer, so it needs none of the D3D headers and builds everywhere.
// The extensions are offered where the render layer has the backends (r_backend_available).
struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace
{

typedef struct OXR_Luid
{
    u32 LowPart;
    i32 HighPart;
} LUID;

typedef enum D3D_FEATURE_LEVEL
{
    D3D_FEATURE_LEVEL_11_0 = 0xb000,
} D3D_FEATURE_LEVEL;

} // namespace

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr_platform.h>

//
// Statics
//

// What the requirements answer: the first adapter of DXGI, the feature level the backends need
static XrResult requirements (XrInstance instance, XrSystemId system, XrStructureType type, void* out_requirements, b8 d3d12)
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    if (system != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    // XrGraphicsRequirementsD3D11KHR and XrGraphicsRequirementsD3D12KHR have the same layout
    XrGraphicsRequirementsD3D11KHR* out = (XrGraphicsRequirementsD3D11KHR*)out_requirements;
    OXR_CHECK_TYPE(out, type);
    u64 luid = 0;
    if (!r_d3d_adapter_luid(luid))
    {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    out->adapterLuid     = { .LowPart = (u32)luid, .HighPart = (i32)(u32)(luid >> 32) };
    out->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    (d3d12 ? inst->d3d12_requirements_called : inst->d3d11_requirements_called) = true;
    return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetD3D11GraphicsRequirementsKHR (XrInstance instance, XrSystemId system, XrGraphicsRequirementsD3D11KHR* out_requirements)
{
    return requirements(instance, system, XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR, out_requirements, false);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetD3D12GraphicsRequirementsKHR (XrInstance instance, XrSystemId system, XrGraphicsRequirementsD3D12KHR* out_requirements)
{
    return requirements(instance, system, XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR, out_requirements, true);
}

//
// Public API (layer internal)
//

PFN_xrVoidFunction oxr_d3d_get_proc (const char* name, b8 d3d11_enable, b8 d3d12_enable)
{
    if (d3d11_enable && cstr_equal(name, "xrGetD3D11GraphicsRequirementsKHR"))
    {
        return (PFN_xrVoidFunction)static_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(oxr_xrGetD3D11GraphicsRequirementsKHR);
    }
    if (d3d12_enable && cstr_equal(name, "xrGetD3D12GraphicsRequirementsKHR"))
    {
        return (PFN_xrVoidFunction)static_cast<PFN_xrGetD3D12GraphicsRequirementsKHR>(oxr_xrGetD3D12GraphicsRequirementsKHR);
    }
    return nullptr;
}

b8 oxr_d3d_is_binding (const OXR_Instance* inst, XrStructureType type)
{
    return (type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR && inst->ext.KHR_D3D11_enable) || (type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR && inst->ext.KHR_D3D12_enable);
}

XrResult oxr_d3d_create_session_graphics (const OXR_Instance* inst, const XrBaseInStructure* graphics_binding, R_Device*& out_device)
{
    out_device              = nullptr;
    const b8           d3d12 = graphics_binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
    R_DeviceCreateInfo info  = {};
    if (d3d12)
    {
        const XrGraphicsBindingD3D12KHR* binding = (const XrGraphicsBindingD3D12KHR*)graphics_binding;
        if (!inst->d3d12_requirements_called)
        {
            return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
        }
        if (!binding->device || !binding->queue)
        {
            LOG_ERROR("The D3D12 graphics binding has a null device or queue");
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        info = { .backend = R_Backend::D3D12, .d3d12 = { .device = binding->device, .queue = binding->queue } };
    }
    else
    {
        const XrGraphicsBindingD3D11KHR* binding = (const XrGraphicsBindingD3D11KHR*)graphics_binding;
        if (!inst->d3d11_requirements_called)
        {
            return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
        }
        if (!binding->device)
        {
            LOG_ERROR("The D3D11 graphics binding has a null device");
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        info = { .backend = R_Backend::D3D11, .d3d11 = { .device = binding->device } };
    }

    out_device = r_create_device(info);
    if (!out_device)
    {
        LOG_ERROR("Cannot use the app's %s device", d3d12 ? "D3D12" : "D3D11");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    // The simulator renders on any adapter; a device on another than the requirements named
    // works, as on hybrid laptops, so it is only worth a warning
    u64 luid = 0;
    if (r_d3d_adapter_luid(luid) && r_d3d_device_luid(out_device) != luid)
    {
        LOG_WARNING("The %s device is not on the adapter xrGetD3D%sGraphicsRequirementsKHR named", d3d12 ? "D3D12" : "D3D11", d3d12 ? "12" : "11");
    }
    return XR_SUCCESS;
}

XrResult oxr_d3d_write_swapchain_images (const u64* native_images, u32 count, XrSwapchainImageBaseHeader* images, b8 d3d12)
{
    // XrSwapchainImageD3D11KHR and XrSwapchainImageD3D12KHR have the same layout
    XrSwapchainImageD3D11KHR* out = (XrSwapchainImageD3D11KHR*)images;
    for (u32 i = 0; i < count; i++)
    {
        OXR_CHECK_TYPE(&out[i], d3d12 ? XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR : XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR);
        out[i].texture = (ID3D11Texture2D*)(uintptr_t)native_images[i];
    }
    return XR_SUCCESS;
}
