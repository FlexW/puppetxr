#include "oxr_internal.h"

// The platform types openxr_platform.h refers to, declared as the X11, GLX, EGL and Windows
// headers do; puppetxr only passes them on, so it needs none of those headers
typedef struct OXR_XDisplay      Display;
typedef unsigned long            XID;
typedef XID                      GLXDrawable;
typedef struct OXR_GLXFBConfig*  GLXFBConfig;
typedef struct OXR_GLXContext*   GLXContext;
typedef void*                    EGLDisplay;
typedef void*                    EGLConfig;
typedef void*                    EGLContext;
typedef u32                      EGLenum;
typedef i32                      EGLint;
typedef u32                      EGLBoolean;
typedef struct OXR_HDC*          HDC;
typedef struct OXR_HGLRC*        HGLRC;
typedef struct OXR_IUnknown      IUnknown;
namespace
{

typedef union OXR_LargeInteger
{
    i64 QuadPart;
} LARGE_INTEGER;

} // namespace

#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_PLATFORM_XLIB
#define XR_USE_PLATFORM_EGL
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr_platform.h>

//
// Types
//

#define OXR_GL_MIN_API   XR_MAKE_VERSION(3, 3, 0)
#define OXR_GL_MAX_API   XR_MAKE_VERSION(4, 6, 0)
#define OXR_GLES_MIN_API XR_MAKE_VERSION(3, 0, 0)
#define OXR_GLES_MAX_API XR_MAKE_VERSION(3, 2, 0)

#define OXR_EGL_CONTEXT_CLIENT_TYPE 0x3097
#define OXR_EGL_OPENGL_API          0x30A2
#define OXR_EGL_OPENGL_ES_API       0x30A0

typedef EGLBoolean (*OXR_EglQueryContext) (EGLDisplay display, EGLContext context, EGLint attribute, EGLint* value);

//
// Statics
//

static XrResult requirements (XrInstance instance, XrSystemId system, XrStructureType type, XrVersion min, XrVersion max, void* out_requirements, b8 es)
{
    const OXR_Lock lock(__func__);
    OXR_GET_INSTANCE(inst, instance);
    if (system != OXR_SYSTEM_ID)
    {
        return XR_ERROR_SYSTEM_INVALID;
    }
    // XrGraphicsRequirementsOpenGLKHR and XrGraphicsRequirementsOpenGLESKHR have the same layout
    XrGraphicsRequirementsOpenGLKHR* out = (XrGraphicsRequirementsOpenGLKHR*)out_requirements;
    OXR_CHECK_TYPE(out, type);
    out->minApiVersionSupported = min;
    out->maxApiVersionSupported = max;
    (es ? inst->gles_requirements_called : inst->gl_requirements_called) = true;
    return XR_SUCCESS;
}

// XrGraphicsBindingEGLMNDX names no API; the context does (EGL 1.5, 3.7.4)
static XrResult egl_context_api (const XrGraphicsBindingEGLMNDX* binding, b8& out_es)
{
    const OXR_EglQueryContext query = (OXR_EglQueryContext)binding->getProcAddress("eglQueryContext");
    EGLint                    api   = 0;
    if (!query || !query(binding->display, binding->context, OXR_EGL_CONTEXT_CLIENT_TYPE, &api) || (api != OXR_EGL_OPENGL_API && api != OXR_EGL_OPENGL_ES_API))
    {
        LOG_ERROR("The EGL context of the graphics binding is no OpenGL or OpenGL ES context");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    out_es = api == OXR_EGL_OPENGL_ES_API;
    return XR_SUCCESS;
}

// The API's extension must be enabled and its requirements queried before the session
static XrResult check_api (const OXR_Instance* inst, b8 es)
{
    if (es ? !inst->ext.KHR_opengl_es_enable : !inst->ext.KHR_opengl_enable)
    {
        LOG_ERROR("An %s context needs %s", es ? "OpenGL ES" : "OpenGL", es ? "XR_KHR_opengl_es_enable" : "XR_KHR_opengl_enable");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    if (es ? !inst->gles_requirements_called : !inst->gl_requirements_called)
    {
        return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    }
    return XR_SUCCESS;
}

//
// XR_KHR_opengl_enable and XR_KHR_opengl_es_enable
//

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetOpenGLGraphicsRequirementsKHR (XrInstance instance, XrSystemId system, XrGraphicsRequirementsOpenGLKHR* out_requirements)
{
    return requirements(instance, system, XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR, OXR_GL_MIN_API, OXR_GL_MAX_API, out_requirements, false);
}

static XRAPI_ATTR XrResult XRAPI_CALL oxr_xrGetOpenGLESGraphicsRequirementsKHR (XrInstance instance, XrSystemId system, XrGraphicsRequirementsOpenGLESKHR* out_requirements)
{
    return requirements(instance, system, XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR, OXR_GLES_MIN_API, OXR_GLES_MAX_API, out_requirements, true);
}

//
// Public API (layer internal)
//

PFN_xrVoidFunction oxr_gl_get_proc (const char* name, b8 opengl_enable, b8 opengl_es_enable)
{
    if (opengl_enable && cstr_equal(name, "xrGetOpenGLGraphicsRequirementsKHR"))
    {
        return (PFN_xrVoidFunction)static_cast<PFN_xrGetOpenGLGraphicsRequirementsKHR>(oxr_xrGetOpenGLGraphicsRequirementsKHR);
    }
    if (opengl_es_enable && cstr_equal(name, "xrGetOpenGLESGraphicsRequirementsKHR"))
    {
        return (PFN_xrVoidFunction)static_cast<PFN_xrGetOpenGLESGraphicsRequirementsKHR>(oxr_xrGetOpenGLESGraphicsRequirementsKHR);
    }
    return nullptr;
}

b8 oxr_gl_is_binding (const OXR_Instance* inst, XrStructureType type)
{
    return ((type == XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR || type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) && inst->ext.KHR_opengl_enable) || (type == XR_TYPE_GRAPHICS_BINDING_EGL_MNDX && inst->ext.MNDX_egl_enable);
}

XrResult oxr_gl_create_session_graphics (const OXR_Instance* inst, const XrBaseInStructure* graphics_binding, R_Device*& out_device, u32& out_x11_visual)
{
    out_device     = nullptr;
    out_x11_visual = 0;
    R_DeviceCreateInfo info = {};
    if (graphics_binding->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR)
    {
        const XrGraphicsBindingOpenGLXlibKHR* binding = (const XrGraphicsBindingOpenGLXlibKHR*)graphics_binding;
        const XrResult                        api     = check_api(inst, false);
        if (api != XR_SUCCESS)
        {
            return api;
        }
        if (!binding->xDisplay || !binding->glxDrawable || !binding->glxContext)
        {
            LOG_ERROR("The Xlib graphics binding has a null display, drawable or context");
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        info           = { .backend = R_Backend::OpenGL, .gl = { .platform = R_GLPlatform::GLX, .display = binding->xDisplay, .config = binding->glxFBConfig, .context = binding->glxContext, .drawable = (u64)binding->glxDrawable } };
        out_x11_visual = binding->visualid;
    }
    else if (graphics_binding->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR)
    {
        const XrGraphicsBindingOpenGLWin32KHR* binding = (const XrGraphicsBindingOpenGLWin32KHR*)graphics_binding;
        const XrResult                         api     = check_api(inst, false);
        if (api != XR_SUCCESS)
        {
            return api;
        }
        if (!binding->hDC || !binding->hGLRC)
        {
            LOG_ERROR("The Win32 graphics binding has a null device context or GL context");
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        info = { .backend = R_Backend::OpenGL, .gl = { .platform = R_GLPlatform::WGL, .context = binding->hGLRC, .drawable = (u64)(uintptr_t)binding->hDC } };
    }
    else
    {
        const XrGraphicsBindingEGLMNDX* binding = (const XrGraphicsBindingEGLMNDX*)graphics_binding;
        if (!binding->getProcAddress || !binding->display || !binding->context)
        {
            LOG_ERROR("The EGL graphics binding has a null getProcAddress, display or context");
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        b8             es     = false;
        const XrResult client = egl_context_api(binding, es);
        if (client != XR_SUCCESS)
        {
            return client;
        }
        const XrResult api = check_api(inst, es);
        if (api != XR_SUCCESS)
        {
            return api;
        }
        info = { .backend = es ? R_Backend::OpenGLES : R_Backend::OpenGL, .gl = { .platform = R_GLPlatform::EGL, .get_proc_address = (void*)binding->getProcAddress, .display = binding->display, .config = binding->config, .context = binding->context } };
    }

    out_device = r_create_device(info);
    if (!out_device)
    {
        LOG_ERROR("Cannot share objects with the app's GL context");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    if (!out_x11_visual)
    {
        out_x11_visual = r_gl_x11_visual(out_device);
    }
    return XR_SUCCESS;
}

XrResult oxr_gl_write_swapchain_images (const u64* native_images, u32 count, XrSwapchainImageBaseHeader* images, b8 es)
{
    // XrSwapchainImageOpenGLKHR and XrSwapchainImageOpenGLESKHR have the same layout
    XrSwapchainImageOpenGLKHR* out = (XrSwapchainImageOpenGLKHR*)images;
    for (u32 i = 0; i < count; i++)
    {
        OXR_CHECK_TYPE(&out[i], es ? XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR : XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR);
        out[i].image = (uint32_t)native_images[i];
    }
    return XR_SUCCESS;
}
