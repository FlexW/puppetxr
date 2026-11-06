#include "oxr.h"
#include "oxr_internal.h"
#include "rec.h"
#include "puppetxr.h"

#include <gtest/gtest.h>
#include <time.h>
#include <volk.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_GRAPHICS_API_METAL
#define XR_USE_PLATFORM_EGL
#define XR_USE_TIMESPEC
// The EGL types openxr_platform.h refers to; the GL app of the tests loads EGL at runtime (test_gl.h)
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef u32   EGLenum;
typedef void (*PFN_eglGetProcAddress) ();
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <math.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "test_vulkan.h"
#include "test_gl.h"
#include "test_d3d.h"
#include "test_metal.h"
#include "test_room.h"

extern "C" XrResult xrNegotiateLoaderRuntimeInterface (const XrNegotiateLoaderInfo* loader_info, XrNegotiateRuntimeRequest* runtime_request);

namespace
{

// XrGraphicsBindingOpenGLWin32KHR without the Windows headers openxr_platform.h needs for it
struct TestWin32Binding
{
    XrStructureType type;
    const void*     next;
    void*           dc;      // HDC
    void*           context; // HGLRC
};

// The graphics binding of a test GL app: EGL, or Win32 for a WGL app
struct TestGLBinding
{
    XrGraphicsBindingEGLMNDX egl;
    TestWin32Binding         win32;

    explicit TestGLBinding (const TestGLApp& app)
        : egl({ .type = XR_TYPE_GRAPHICS_BINDING_EGL_MNDX, .getProcAddress = (PFN_xrEglGetProcAddressMNDX)app.get_proc_address, .display = app.display, .config = app.config, .context = app.context }),
          win32({ .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR, .next = nullptr, .dc = app.dc, .context = app.context }), wgl(app.wgl)
    {
    }

    const void* chain () const
    {
        return wgl ? (const void*)&win32 : (const void*)&egl;
    }

    b8 wgl;
};

} // namespace

//
// Harness: talks to the runtime exactly like the loader does
//

#define XR_FUNCTIONS(X)                     \
    X(xrEnumerateInstanceExtensionProperties) \
    X(xrEnumerateApiLayerProperties)        \
    X(xrCreateInstance)                     \
    X(xrDestroyInstance)                    \
    X(xrGetInstanceProperties)              \
    X(xrPollEvent)                          \
    X(xrResultToString)                     \
    X(xrStructureTypeToString)              \
    X(xrGetSystem)                          \
    X(xrGetSystemProperties)                \
    X(xrEnumerateEnvironmentBlendModes)     \
    X(xrCreateSession)                      \
    X(xrDestroySession)                     \
    X(xrEnumerateReferenceSpaces)           \
    X(xrCreateReferenceSpace)               \
    X(xrGetReferenceSpaceBoundsRect)        \
    X(xrCreateActionSpace)                  \
    X(xrLocateSpace)                        \
    X(xrDestroySpace)                       \
    X(xrEnumerateViewConfigurations)        \
    X(xrGetViewConfigurationProperties)     \
    X(xrEnumerateViewConfigurationViews)    \
    X(xrEnumerateSwapchainFormats)          \
    X(xrCreateSwapchain)                    \
    X(xrDestroySwapchain)                   \
    X(xrEnumerateSwapchainImages)           \
    X(xrAcquireSwapchainImage)              \
    X(xrWaitSwapchainImage)                 \
    X(xrReleaseSwapchainImage)              \
    X(xrBeginSession)                       \
    X(xrEndSession)                         \
    X(xrRequestExitSession)                 \
    X(xrWaitFrame)                          \
    X(xrBeginFrame)                         \
    X(xrEndFrame)                           \
    X(xrLocateViews)                        \
    X(xrStringToPath)                       \
    X(xrPathToString)                       \
    X(xrCreateActionSet)                    \
    X(xrDestroyActionSet)                   \
    X(xrCreateAction)                       \
    X(xrDestroyAction)                      \
    X(xrSuggestInteractionProfileBindings)  \
    X(xrAttachSessionActionSets)            \
    X(xrGetCurrentInteractionProfile)       \
    X(xrGetActionStateBoolean)              \
    X(xrGetActionStateFloat)                \
    X(xrGetActionStateVector2f)             \
    X(xrGetActionStatePose)                 \
    X(xrSyncActions)                        \
    X(xrEnumerateBoundSourcesForAction)     \
    X(xrGetInputSourceLocalizedName)        \
    X(xrApplyHapticFeedback)                \
    X(xrStopHapticFeedback)                 \
    X(xrLocateSpaces)

// The temporary directory of the platform, for files the tests write
static const char* temp_dir ()
{
    static char dir[OS_MAX_PATH] = {};
    if (!dir[0] && !os_temp_dir(dir, sizeof(dir)))
    {
        cstr_copy(dir, sizeof(dir), ".");
    }
    return dir;
}

namespace
{

struct Xr
{
#define DECLARE(fn) PFN_##fn fn;
    XR_FUNCTIONS(DECLARE)
#undef DECLARE
};

} // namespace

static PFN_xrGetInstanceProcAddr negotiate ()
{
    const XrNegotiateLoaderInfo loader_info = {
        .structType    = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
        .structVersion = XR_LOADER_INFO_STRUCT_VERSION,
        .structSize    = sizeof(XrNegotiateLoaderInfo),
        .minInterfaceVersion = 1,
        .maxInterfaceVersion = 1,
        .minApiVersion = XR_MAKE_VERSION(1, 0, 0),
        .maxApiVersion = XR_MAKE_VERSION(1, 1, 999),
    };
    XrNegotiateRuntimeRequest request = {
        .structType    = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
        .structVersion = XR_RUNTIME_INFO_STRUCT_VERSION,
        .structSize    = sizeof(XrNegotiateRuntimeRequest),
    };
    if (XR_FAILED(xrNegotiateLoaderRuntimeInterface(&loader_info, &request)))
    {
        return nullptr;
    }
    return request.getInstanceProcAddr;
}

static Xr load (PFN_xrGetInstanceProcAddr gipa, XrInstance instance)
{
    Xr xr = {};
#define LOAD(fn) gipa(instance, #fn, (PFN_xrVoidFunction*)&xr.fn);
    XR_FUNCTIONS(LOAD)
#undef LOAD
    return xr;
}

namespace
{

struct OxrTest : testing::Test
{
    PFN_xrGetInstanceProcAddr gipa     = nullptr;
    Xr                        xr       = {};
    XrInstance                instance = XR_NULL_HANDLE;
    XrSystemId                system   = XR_NULL_SYSTEM_ID;
    XrSession                 session  = XR_NULL_HANDLE;
    b8                        headless = true;    // derived fixtures may clear it in their constructor
    b8                        real_window = false; // with headless cleared: the platform's video driver instead of SDL's offscreen one
    const char*               extension = nullptr; // enabled next to XR_MND_headless, set in a derived constructor
    const char*               more_extensions[12] = {};
    u32                       more_extensions_count = 0;

    void SetUp () override
    {
        os_set_assert_mode(OS_AssertMode::Log);
        if (headless)
        {
            os_set_env("PUPPETXR_HEADLESS", "1");
        }
        else
        {
            os_unset_env("PUPPETXR_HEADLESS");
            if (!real_window)
            {
                os_set_env("SDL_VIDEODRIVER", "offscreen");
            }
        }
        os_set_env("PUPPETXR_FIXED_STEP", "1");
        gipa = negotiate();
        ASSERT_NE(gipa, nullptr);
        xr = load(gipa, XR_NULL_HANDLE);
        ASSERT_NE(xr.xrCreateInstance, nullptr);
    }

    void TearDown () override
    {
        if (instance != XR_NULL_HANDLE)
        {
            xr.xrDestroyInstance(instance);
        }
        os_unset_env("PUPPETXR_RECORD");
        os_unset_env("PUPPETXR_REPLAY");
        os_unset_env("PUPPETXR_REPLAY_EXIT");
        os_unset_env("SDL_VIDEODRIVER");
    }

    XrResult create_instance (const char* const* extensions, u32 extensions_count, XrVersion api_version = XR_API_VERSION_1_1)
    {
        const XrInstanceCreateInfo info = {
            .type                  = XR_TYPE_INSTANCE_CREATE_INFO,
            .applicationInfo       = { .applicationName = "puppetxr_tests", .applicationVersion = 1, .engineName = "", .engineVersion = 0, .apiVersion = api_version },
            .enabledExtensionCount = extensions_count,
            .enabledExtensionNames = extensions,
        };
        const XrResult result = xr.xrCreateInstance(&info, &instance);
        if (XR_SUCCEEDED(result))
        {
            xr = load(gipa, instance);
        }
        return result;
    }

    void create_headless_instance ()
    {
        const char* extensions[2 + ARRAY_SIZE(more_extensions)] = { XR_MND_HEADLESS_EXTENSION_NAME };
        u32         count = 1;
        if (extension)
        {
            extensions[count++] = extension;
        }
        for (u32 i = 0; i < more_extensions_count; i++)
        {
            extensions[count++] = more_extensions[i];
        }
        ASSERT_EQ(create_instance(extensions, count), XR_SUCCESS);
        const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
        ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);
    }

    void create_session ()
    {
        const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .systemId = system };
        ASSERT_EQ(xr.xrCreateSession(instance, &info, &session), XR_SUCCESS);
    }

    // Polls every queued event and keeps the last one of `type`
    b8 poll_for (XrStructureType type, XrEventDataBuffer& out_event) const
    {
        b8 found = false;
        while (true)
        {
            XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            if (xr.xrPollEvent(instance, &event) != XR_SUCCESS)
            {
                return found;
            }
            if (event.type == type)
            {
                out_event = event;
                found     = true;
            }
        }
    }

    XrSessionState drain_events () const
    {
        XrSessionState last = XR_SESSION_STATE_UNKNOWN;
        while (true)
        {
            XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            if (xr.xrPollEvent(instance, &event) != XR_SUCCESS)
            {
                break;
            }
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                last = ((XrEventDataSessionStateChanged*)&event)->state;
            }
        }
        return last;
    }

    // Graphics sessions synchronize with their first frame, so one empty frame is submitted for them
    void begin_session () const
    {
        drain_events();
        const XrSessionBeginInfo info = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
        ASSERT_EQ(xr.xrBeginSession(session, &info), XR_SUCCESS);
        XrSessionState state = drain_events();
        if (state == XR_SESSION_STATE_UNKNOWN)
        {
            frame();
            state = drain_events();
        }
        ASSERT_EQ(state, XR_SESSION_STATE_FOCUSED);
    }

    // Where the window thread is a thread of its own (Windows, Linux) the window's part of a frame
    // runs there after the frame; on macOS the test's main thread is the window thread
    void ticks () const
    {
        const u64 start = os_monotonic_ns();
        while (oxr_get_runtime() && oxr_get_runtime()->window_tick_pending && os_monotonic_ns() - start < 1000000000ull)
        {
            os_serve_posted(1000000);
        }
    }

    XrFrameState frame () const
    {
        const XrFrameState state = frame_only();
        ticks();
        return state;
    }

    // The app's frame without waiting for the window's part after it
    XrFrameState frame_only () const
    {
        XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
        const XrFrameWaitInfo wait_info = { .type = XR_TYPE_FRAME_WAIT_INFO };
        EXPECT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
        const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
        EXPECT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
        return state;
    }

    XrPath path (const char* str) const
    {
        XrPath p = XR_NULL_PATH;
        EXPECT_EQ(xr.xrStringToPath(instance, str, &p), XR_SUCCESS);
        return p;
    }

    XrSpace reference_space (XrReferenceSpaceType type, XrPosef pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }) const
    {
        XrSpace space = XR_NULL_HANDLE;
        const XrReferenceSpaceCreateInfo info = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = type, .poseInReferenceSpace = pose };
        EXPECT_EQ(xr.xrCreateReferenceSpace(session, &info, &space), XR_SUCCESS);
        return space;
    }

    // Submits one frame with these layers and reads both eyes of the mirror target back
    void submit_and_read (const XrCompositionLayerBaseHeader* const* layers, u32 layers_count, Array<u8>& pixels, u32& width, u32& height) const
    {
        XrFrameState           state      = { .type = XR_TYPE_FRAME_STATE };
        const XrFrameWaitInfo  wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
        const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
        ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
        ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = layers_count, .layers = layers };
        oxr_set_mirror_view(SIM_MirrorView::Both);
        ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
        u8 probe[4];
        ASSERT_FALSE(oxr_read_frame(probe, sizeof(probe), width, height));
        pixels.resize((usize)width * height * 4);
        ASSERT_TRUE(oxr_read_frame(pixels.data, pixels.length, width, height));
    }

    // Pixel at a position given in normalized device coordinates of one eye of a Both target
    static const u8* eye_pixel (const Array<u8>& pixels, u32 width, u32 height, u32 eye, f32 x, f32 y)
    {
        const u32 eye_width = width / 2;
        const u32 px        = eye * eye_width + (u32)((x * 0.5f + 0.5f) * (f32)(eye_width - 1));
        const u32 py        = (u32)((y * 0.5f + 0.5f) * (f32)(height - 1));
        return pixels.data + ((usize)py * width + px) * 4;
    }

    XrCompositionLayerProjectionView view (XrSwapchain swapchain, u32 layer, u32 width, u32 height)
    {
        return { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .fov = { -0.7f, 0.7f, 0.7f, -0.7f }, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { (i32)width, (i32)height } }, .imageArrayIndex = layer } };
    }
};

} // namespace

static XrInstance garbage_instance () { return (XrInstance)(uintptr_t)0xDEADBEEFCAFEBABEull; }

//
// Negotiation and instance
//

// Regression: the loader unloads the runtime after the last xrDestroyInstance, and the runtime's
// global state used to stay allocated with nothing pointing to it (found by LeakSanitizer)
TEST_F(OxrTest, runtime_state_is_released_once_no_instance_is_alive)
{
    create_headless_instance();
    EXPECT_FALSE(oxr_release_runtime());
    ASSERT_EQ(xr.xrDestroyInstance(instance), XR_SUCCESS);
    instance = XR_NULL_HANDLE;
    EXPECT_TRUE(oxr_release_runtime());
    EXPECT_TRUE(oxr_release_runtime());

    create_headless_instance();
    create_session();
    begin_session();
    frame();
}

// Every xrEndFrame after the first keeps the time since the last one and how much of it puppetxr
// held its lock, in a ring of the last UI_FRAME_TIMES frames for the performance tab
TEST_F(OxrTest, frames_are_timed_for_the_performance_tab)
{
    create_headless_instance();
    create_session();
    begin_session();
    frame();
    const UI_FrameTimes& times = oxr_get_runtime()->frame_times;
    const u32            start = times.count;
    for (u32 i = 0; i < 10; i++)
    {
        frame();
    }
    ASSERT_EQ(times.count, start + 10);
    // A headless frame's work can fit in one tick of macOS's 42 ns clock, so only the sum is above zero
    f32 puppetxr = 0.0f;
    for (u32 i = 0; i < times.count; i++)
    {
        EXPECT_GT(times.frame_ms[i], 0.0f) << i;
        EXPECT_GE(times.puppetxr_ms[i], 0.0f) << i;
        EXPECT_LE(times.puppetxr_ms[i], times.frame_ms[i]) << "the lock is held within the frame";
        puppetxr += times.puppetxr_ms[i];
    }
    EXPECT_GT(puppetxr, 0.0f);
    for (u32 i = 0; i < UI_FRAME_TIMES; i++)
    {
        frame();
    }
    EXPECT_EQ(times.count, (u32)UI_FRAME_TIMES) << "a ring of the latest";
    EXPECT_EQ(times.next, (start + 10 + UI_FRAME_TIMES) % UI_FRAME_TIMES);
}

TEST_F(OxrTest, negotiate_rejects_bad_structs)
{
    XrNegotiateLoaderInfo loader_info = { .structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, .structVersion = XR_LOADER_INFO_STRUCT_VERSION, .structSize = sizeof(XrNegotiateLoaderInfo), .minInterfaceVersion = 1, .maxInterfaceVersion = 1, .minApiVersion = XR_MAKE_VERSION(1, 0, 0), .maxApiVersion = XR_MAKE_VERSION(1, 1, 0) };
    XrNegotiateRuntimeRequest request = { .structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, .structVersion = XR_RUNTIME_INFO_STRUCT_VERSION, .structSize = sizeof(XrNegotiateRuntimeRequest) };

    EXPECT_EQ(xrNegotiateLoaderRuntimeInterface(nullptr, &request), XR_ERROR_INITIALIZATION_FAILED);
    EXPECT_EQ(xrNegotiateLoaderRuntimeInterface(&loader_info, nullptr), XR_ERROR_INITIALIZATION_FAILED);

    loader_info.structSize = 3;
    EXPECT_EQ(xrNegotiateLoaderRuntimeInterface(&loader_info, &request), XR_ERROR_INITIALIZATION_FAILED);
    loader_info.structSize = sizeof(XrNegotiateLoaderInfo);

    loader_info.minInterfaceVersion = 99;
    loader_info.maxInterfaceVersion = 99;
    EXPECT_EQ(xrNegotiateLoaderRuntimeInterface(&loader_info, &request), XR_ERROR_INITIALIZATION_FAILED);
    loader_info.minInterfaceVersion = 1;
    loader_info.maxInterfaceVersion = 1;

    EXPECT_EQ(xrNegotiateLoaderRuntimeInterface(&loader_info, &request), XR_SUCCESS);
    EXPECT_EQ(request.runtimeInterfaceVersion, 1u);
    EXPECT_EQ(XR_VERSION_MAJOR(request.runtimeApiVersion), 1u);
    EXPECT_NE(request.getInstanceProcAddr, nullptr);
}

TEST_F(OxrTest, proc_addr_without_instance_exposes_only_global_functions)
{
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(XR_NULL_HANDLE, "xrCreateInstance", &fn), XR_SUCCESS);
    EXPECT_NE(fn, nullptr);
    EXPECT_EQ(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &fn), XR_SUCCESS);
    EXPECT_EQ(gipa(XR_NULL_HANDLE, "xrEnumerateApiLayerProperties", &fn), XR_SUCCESS);

    fn = (PFN_xrVoidFunction)1;
    EXPECT_EQ(gipa(XR_NULL_HANDLE, "xrGetSystem", &fn), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(fn, nullptr);
    EXPECT_EQ(gipa(XR_NULL_HANDLE, "xrNotAFunction", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(XR_NULL_HANDLE, nullptr, &fn), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(gipa(XR_NULL_HANDLE, "xrCreateInstance", nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(gipa(garbage_instance(), "xrGetSystem", &fn), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrTest, enumerates_extensions_and_no_layers)
{
    u32 count = 0;
    EXPECT_EQ(xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_GT(count, 0u);

    XrExtensionProperties props[128] = {};
    for (u32 i = 0; i < ARRAY_SIZE(props); i++)
    {
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    u32 written = 0;
    EXPECT_EQ(xr.xrEnumerateInstanceExtensionProperties(nullptr, 1, &written, props), count > 1 ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS);
    EXPECT_EQ(xr.xrEnumerateInstanceExtensionProperties(nullptr, 128, &written, props), XR_SUCCESS);
    EXPECT_EQ(written, count);

    b8 has_headless = false;
    for (u32 i = 0; i < written; i++)
    {
        if (cstr_equal(props[i].extensionName, XR_MND_HEADLESS_EXTENSION_NAME))
        {
            has_headless = true;
            EXPECT_EQ(props[i].extensionVersion, (u32)XR_MND_headless_SPEC_VERSION);
        }
    }
    EXPECT_TRUE(has_headless);

    EXPECT_EQ(xr.xrEnumerateInstanceExtensionProperties("SomeLayer", 0, &count, nullptr), XR_ERROR_API_LAYER_NOT_PRESENT);
    EXPECT_EQ(xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, nullptr, nullptr), XR_ERROR_VALIDATION_FAILURE);

    u32 layers = 99;
    EXPECT_EQ(xr.xrEnumerateApiLayerProperties(0, &layers, nullptr), XR_SUCCESS);
    EXPECT_EQ(layers, 0u);
}

static b8 extension_offered (const Xr& xr, const char* name)
{
    XrExtensionProperties props[128] = {};
    for (XrExtensionProperties& prop : props)
    {
        prop.type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    u32 count = 0;
    EXPECT_EQ(xr.xrEnumerateInstanceExtensionProperties(nullptr, ARRAY_SIZE(props), &count, props), XR_SUCCESS);
    for (u32 i = 0; i < count; i++)
    {
        if (cstr_equal(props[i].extensionName, name))
        {
            return true;
        }
    }
    return false;
}

// XR_META_spatial_entity_semantic_label only defines XrSemanticLabelMETA, for XR_META_spatial_entity_room_mesh
TEST_F(OxrTest, semantic_label_extension_is_offered_and_enabled)
{
    EXPECT_TRUE(extension_offered(xr, XR_META_SPATIAL_ENTITY_SEMANTIC_LABEL_EXTENSION_NAME));
    more_extensions[more_extensions_count++] = XR_META_SPATIAL_ENTITY_SEMANTIC_LABEL_EXTENSION_NAME;
    create_headless_instance();
    EXPECT_NE(instance, XR_NULL_HANDLE);
}

TEST_F(OxrTest, disabled_extensions_are_neither_offered_nor_enabled)
{
    EXPECT_TRUE(extension_offered(xr, XR_FB_PASSTHROUGH_EXTENSION_NAME));
    os_set_env("PUPPETXR_DISABLE_EXTENSIONS", "XR_FB_passthrough,XR_NOT_an_extension");
    defer { os_unset_env("PUPPETXR_DISABLE_EXTENSIONS"); };
    EXPECT_FALSE(extension_offered(xr, XR_FB_PASSTHROUGH_EXTENSION_NAME));
    EXPECT_TRUE(extension_offered(xr, XR_MND_HEADLESS_EXTENSION_NAME));

    const char* passthrough[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_FB_PASSTHROUGH_EXTENSION_NAME };
    EXPECT_EQ(create_instance(passthrough, 2), XR_ERROR_EXTENSION_NOT_PRESENT);
    EXPECT_EQ(instance, XR_NULL_HANDLE);
    ASSERT_EQ(create_instance(passthrough, 1), XR_SUCCESS);
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrCreatePassthroughFB", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
}

// PUPPETXR_GRAPHICS offers only the graphics extensions of the named APIs, so engines that choose
// their API from the offered extensions run the forced one
TEST_F(OxrTest, forced_graphics_apis_offer_only_their_extensions)
{
    os_set_env("PUPPETXR_GRAPHICS", "opengl");
    defer { os_unset_env("PUPPETXR_GRAPHICS"); };
    EXPECT_FALSE(extension_offered(xr, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME));
    EXPECT_FALSE(extension_offered(xr, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME));
    EXPECT_FALSE(extension_offered(xr, XR_META_VULKAN_SWAPCHAIN_CREATE_INFO_EXTENSION_NAME));
    EXPECT_FALSE(extension_offered(xr, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME));
    const b8 have_gl = r_backend_available(R_Backend::OpenGL);
    EXPECT_EQ(extension_offered(xr, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME), have_gl);
    EXPECT_EQ(extension_offered(xr, XR_MNDX_EGL_ENABLE_EXTENSION_NAME), have_gl);
    EXPECT_TRUE(extension_offered(xr, XR_MND_HEADLESS_EXTENSION_NAME));
    const char* vulkan[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    EXPECT_EQ(create_instance(vulkan, 1), XR_ERROR_EXTENSION_NOT_PRESENT);
    const char* gl[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    EXPECT_EQ(create_instance(gl, 1), have_gl ? XR_SUCCESS : XR_ERROR_EXTENSION_NOT_PRESENT);
}

TEST_F(OxrTest, runtime_name_is_configurable)
{
    os_set_env("PUPPETXR_RUNTIME_NAME", "Oculus");
    defer { os_unset_env("PUPPETXR_RUNTIME_NAME"); };
    ASSERT_EQ(create_instance(nullptr, 0), XR_SUCCESS);
    XrInstanceProperties props = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    EXPECT_EQ(xr.xrGetInstanceProperties(instance, &props), XR_SUCCESS);
    EXPECT_STREQ(props.runtimeName, "Oculus");
}

TEST_F(OxrTest, unknown_forced_graphics_apis_restrict_nothing)
{
    os_set_env("PUPPETXR_GRAPHICS", "glide");
    defer { os_unset_env("PUPPETXR_GRAPHICS"); };
    EXPECT_TRUE(extension_offered(xr, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME));
    EXPECT_EQ(extension_offered(xr, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME), r_backend_available(R_Backend::OpenGL));
}

TEST_F(OxrTest, extensions_disabled_through_the_runtime_add_to_the_config)
{
    os_set_env("PUPPETXR_DISABLE_EXTENSIONS", "XR_FB_passthrough");
    defer { os_unset_env("PUPPETXR_DISABLE_EXTENSIONS"); };
    oxr_set_disabled_extensions("XR_EXT_local_floor");
    defer { oxr_set_disabled_extensions(nullptr); };
    EXPECT_FALSE(extension_offered(xr, XR_FB_PASSTHROUGH_EXTENSION_NAME));
    EXPECT_FALSE(extension_offered(xr, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME));
    const char* local_floor[] = { XR_EXT_LOCAL_FLOOR_EXTENSION_NAME };
    EXPECT_EQ(create_instance(local_floor, 1), XR_ERROR_EXTENSION_NOT_PRESENT);

    oxr_set_disabled_extensions("");
    EXPECT_TRUE(extension_offered(xr, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME));
    EXPECT_FALSE(extension_offered(xr, XR_FB_PASSTHROUGH_EXTENSION_NAME));
}

TEST_F(OxrTest, instance_creation_validates_inputs)
{
    const char* bad[] = { "XR_NOT_an_extension" };
    EXPECT_EQ(create_instance(bad, 1), XR_ERROR_EXTENSION_NOT_PRESENT);
    EXPECT_EQ(instance, XR_NULL_HANDLE);

    EXPECT_EQ(create_instance(nullptr, 0, XR_MAKE_VERSION(2, 0, 0)), XR_ERROR_API_VERSION_UNSUPPORTED);
    EXPECT_EQ(create_instance(nullptr, 1), XR_ERROR_VALIDATION_FAILURE);

    XrInstanceCreateInfo info = { .type = XR_TYPE_INSTANCE_CREATE_INFO, .applicationInfo = { .applicationName = "", .apiVersion = XR_API_VERSION_1_1 } };
    EXPECT_EQ(xr.xrCreateInstance(&info, &instance), XR_ERROR_NAME_INVALID);
    info.type = XR_TYPE_SESSION_CREATE_INFO;
    EXPECT_EQ(xr.xrCreateInstance(&info, &instance), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xr.xrCreateInstance(nullptr, &instance), XR_ERROR_VALIDATION_FAILURE);
    info.type = XR_TYPE_INSTANCE_CREATE_INFO;
    EXPECT_EQ(xr.xrCreateInstance(&info, nullptr), XR_ERROR_VALIDATION_FAILURE);

    // Regression: the loader passes the app's layer list through, so the runtime must ignore it.
    // puppetxr used to reject it, which broke every app running with an API layer, the CTS included.
    const char* layers[] = { "XR_APILAYER_KHRONOS_runtime_conformance" };
    info.applicationInfo.applicationName[0] = 'x';
    info.enabledApiLayerCount = 1;
    info.enabledApiLayerNames = layers;
    ASSERT_EQ(xr.xrCreateInstance(&info, &instance), XR_SUCCESS);
    EXPECT_NE(instance, XR_NULL_HANDLE);
    load(gipa, instance).xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;

    info.enabledApiLayerNames = nullptr;
    EXPECT_EQ(xr.xrCreateInstance(&info, &instance), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrTest, instance_lifecycle_and_properties)
{
    ASSERT_EQ(create_instance(nullptr, 0), XR_SUCCESS);
    ASSERT_NE(instance, XR_NULL_HANDLE);

    XrInstanceProperties props = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    EXPECT_EQ(xr.xrGetInstanceProperties(instance, &props), XR_SUCCESS);
    EXPECT_TRUE(cstr_equal(props.runtimeName, "puppetxr"));
    EXPECT_EQ(xr.xrGetInstanceProperties(garbage_instance(), &props), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(xr.xrGetInstanceProperties(instance, nullptr), XR_ERROR_VALIDATION_FAILURE);

    XrInstance second = XR_NULL_HANDLE;
    const XrInstanceCreateInfo info = { .type = XR_TYPE_INSTANCE_CREATE_INFO, .applicationInfo = { .applicationName = "second", .apiVersion = XR_API_VERSION_1_1 } };
    EXPECT_EQ(xr.xrCreateInstance(&info, &second), XR_ERROR_LIMIT_REACHED);

    char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
    EXPECT_EQ(xr.xrResultToString(instance, XR_ERROR_HANDLE_INVALID, buffer), XR_SUCCESS);
    EXPECT_TRUE(cstr_equal(buffer, "XR_ERROR_HANDLE_INVALID"));
    EXPECT_EQ(xr.xrResultToString(instance, (XrResult)-123456, buffer), XR_SUCCESS);
    EXPECT_TRUE(cstr_equal(buffer, "XR_UNKNOWN_FAILURE_-123456"));
    EXPECT_EQ(xr.xrResultToString(instance, (XrResult)123456, buffer), XR_SUCCESS);
    EXPECT_TRUE(cstr_equal(buffer, "XR_UNKNOWN_SUCCESS_123456"));
    EXPECT_EQ(xr.xrStructureTypeToString(instance, XR_TYPE_SESSION_CREATE_INFO, buffer), XR_SUCCESS);
    EXPECT_TRUE(cstr_equal(buffer, "XR_TYPE_SESSION_CREATE_INFO"));
    EXPECT_EQ(xr.xrStructureTypeToString(instance, (XrStructureType)999999, buffer), XR_SUCCESS);
    EXPECT_TRUE(cstr_equal(buffer, "XR_UNKNOWN_STRUCTURE_TYPE_999999"));

    const XrInstance old = instance;
    EXPECT_EQ(xr.xrDestroyInstance(instance), XR_SUCCESS);
    instance = XR_NULL_HANDLE;
    EXPECT_EQ(xr.xrDestroyInstance(old), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(xr.xrGetInstanceProperties(old, &props), XR_ERROR_HANDLE_INVALID);

    ASSERT_EQ(create_instance(nullptr, 0), XR_SUCCESS);
    EXPECT_NE(instance, old);
}

TEST_F(OxrTest, api_1_0_instance_hides_1_1_functions)
{
    ASSERT_EQ(create_instance(nullptr, 0, XR_API_VERSION_1_0), XR_SUCCESS);
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrLocateSpaces", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrLocateSpace", &fn), XR_SUCCESS);
    EXPECT_EQ(gipa(instance, "xrGetVulkanGraphicsRequirements2KHR", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
}

//
// System and view configuration
//

TEST_F(OxrTest, system)
{
    ASSERT_EQ(create_instance(nullptr, 0), XR_SUCCESS);

    XrSystemGetInfo info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY };
    EXPECT_EQ(xr.xrGetSystem(instance, &info, &system), XR_ERROR_FORM_FACTOR_UNSUPPORTED);
    info.formFactor = (XrFormFactor)77;
    EXPECT_EQ(xr.xrGetSystem(instance, &info, &system), XR_ERROR_FORM_FACTOR_UNSUPPORTED);
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    EXPECT_EQ(xr.xrGetSystem(instance, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xr.xrGetSystem(instance, &info, &system), XR_SUCCESS);
    EXPECT_NE(system, XR_NULL_SYSTEM_ID);

    XrSystemProperties props = { .type = XR_TYPE_SYSTEM_PROPERTIES };
    EXPECT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(props.systemId, system);
    EXPECT_GT(props.graphicsProperties.maxSwapchainImageWidth, 0u);
    EXPECT_GT(props.graphicsProperties.maxLayerCount, 0u);
    EXPECT_TRUE(props.trackingProperties.orientationTracking);
    EXPECT_TRUE(props.trackingProperties.positionTracking);
    EXPECT_STREQ(props.systemName, "Meta Quest 3") << "the default device preset, as OVRPlugin names it";
    EXPECT_EQ(props.vendorId, 0x2833u);
    EXPECT_EQ(xr.xrGetSystemProperties(instance, system + 5, &props), XR_ERROR_SYSTEM_INVALID);

    u32 count = 0;
    EXPECT_EQ(xr.xrEnumerateViewConfigurations(instance, system, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 1u);
    XrViewConfigurationType types[1] = {};
    EXPECT_EQ(xr.xrEnumerateViewConfigurations(instance, system, 1, &count, types), XR_SUCCESS);
    EXPECT_EQ(types[0], XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);

    XrViewConfigurationProperties vprops = { .type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES };
    EXPECT_EQ(xr.xrGetViewConfigurationProperties(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &vprops), XR_SUCCESS);
    EXPECT_EQ(vprops.viewConfigurationType, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
    EXPECT_TRUE(vprops.fovMutable); // the compositor places views of any fov, see projection_views_are_reprojected_to_the_display_pose
    EXPECT_EQ(xr.xrGetViewConfigurationProperties(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO, &vprops), XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);

    XrViewConfigurationView views[2] = { { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW }, { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    EXPECT_EQ(xr.xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(xr.xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1, &count, views), XR_ERROR_SIZE_INSUFFICIENT);
    EXPECT_EQ(xr.xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views), XR_SUCCESS);
    EXPECT_GT(views[0].recommendedImageRectWidth, 0u);
    EXPECT_EQ(views[0].recommendedImageRectWidth, views[1].recommendedImageRectWidth);
    EXPECT_GE(views[0].maxImageRectWidth, views[0].recommendedImageRectWidth);
    EXPECT_EQ(views[0].recommendedSwapchainSampleCount, 1u);

    XrEnvironmentBlendMode modes[2] = {};
    EXPECT_EQ(xr.xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1, &count, modes), XR_ERROR_SIZE_INSUFFICIENT);
    EXPECT_EQ(xr.xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, modes), XR_SUCCESS);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(modes[0], XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
    EXPECT_EQ(modes[1], XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND);
}

//
// Paths
//

TEST_F(OxrTest, paths)
{
    ASSERT_EQ(create_instance(nullptr, 0), XR_SUCCESS);

    const XrPath a  = path("/user/hand/left");
    const XrPath a2 = path("/user/hand/left");
    const XrPath b  = path("/user/hand/right");
    EXPECT_NE(a, XR_NULL_PATH);
    EXPECT_EQ(a, a2);
    EXPECT_NE(a, b);

    char buffer[64] = {};
    u32  written    = 0;
    EXPECT_EQ(xr.xrPathToString(instance, a, sizeof(buffer), &written, buffer), XR_SUCCESS);
    EXPECT_TRUE(cstr_equal(buffer, "/user/hand/left"));
    EXPECT_EQ(written, 16u);
    EXPECT_EQ(xr.xrPathToString(instance, a, 0, &written, nullptr), XR_SUCCESS);
    EXPECT_EQ(written, 16u);
    EXPECT_EQ(xr.xrPathToString(instance, a, 4, &written, buffer), XR_ERROR_SIZE_INSUFFICIENT);
    EXPECT_EQ(xr.xrPathToString(instance, (XrPath)123456789, sizeof(buffer), &written, buffer), XR_ERROR_PATH_INVALID);
    EXPECT_EQ(xr.xrPathToString(instance, XR_NULL_PATH, sizeof(buffer), &written, buffer), XR_ERROR_PATH_INVALID);

    XrPath p = XR_NULL_PATH;
    EXPECT_EQ(xr.xrStringToPath(instance, "no_leading_slash", &p), XR_ERROR_PATH_FORMAT_INVALID);
    EXPECT_EQ(xr.xrStringToPath(instance, "/trailing/", &p), XR_ERROR_PATH_FORMAT_INVALID);
    EXPECT_EQ(xr.xrStringToPath(instance, "/UPPER", &p), XR_ERROR_PATH_FORMAT_INVALID);
    EXPECT_EQ(xr.xrStringToPath(instance, "/double//slash", &p), XR_ERROR_PATH_FORMAT_INVALID);
    EXPECT_EQ(xr.xrStringToPath(instance, "/", &p), XR_ERROR_PATH_FORMAT_INVALID);
    EXPECT_EQ(xr.xrStringToPath(instance, "", &p), XR_ERROR_PATH_FORMAT_INVALID);
    EXPECT_EQ(xr.xrStringToPath(instance, nullptr, &p), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xr.xrStringToPath(instance, "/ok-path_1.2", &p), XR_SUCCESS);

    char long_path[XR_MAX_PATH_LENGTH + 8] = {};
    long_path[0] = '/';
    for (usize i = 1; i < sizeof(long_path) - 1; i++)
    {
        long_path[i] = 'a';
    }
    EXPECT_EQ(xr.xrStringToPath(instance, long_path, &p), XR_ERROR_PATH_FORMAT_INVALID);

    for (u32 i = 0; i < 2000; i++)
    {
        char unique[32];
        cstr_format(unique, sizeof(unique), "/unique/path/%u", i);
        EXPECT_EQ(xr.xrStringToPath(instance, unique, &p), XR_SUCCESS);
    }
    EXPECT_EQ(path("/unique/path/1999"), p);
    EXPECT_EQ(path("/user/hand/left"), a);
}

//
// Session state machine
//

TEST_F(OxrTest, session_requires_graphics_binding_unless_headless)
{
    ASSERT_EQ(create_instance(nullptr, 0), XR_SUCCESS);
    const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);

    XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .systemId = system };
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    EXPECT_EQ(session, XR_NULL_HANDLE);

    info.systemId = 42;
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_SYSTEM_INVALID);
    EXPECT_EQ(xr.xrCreateSession(instance, nullptr, &session), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xr.xrCreateSession(garbage_instance(), &info, &session), XR_ERROR_HANDLE_INVALID);
}

// Regression (found by fuzzing): xrPollEvent wrote a whole XrEventDataBuffer into any struct, whatever
// its type. A reused buffer that still holds the last event's type stays accepted.
TEST_F(OxrTest, poll_event_rejects_structs_that_are_no_event_buffer)
{
    create_headless_instance();
    create_session();
    XrFrameState frame_state = { .type = XR_TYPE_FRAME_STATE };
    EXPECT_EQ(xr.xrPollEvent(instance, (XrEventDataBuffer*)&frame_state), XR_ERROR_VALIDATION_FAILURE);
    XrBaseOutStructure unknown = { .type = (XrStructureType)0x7ffffff0 };
    EXPECT_EQ(xr.xrPollEvent(instance, (XrEventDataBuffer*)&unknown), XR_ERROR_VALIDATION_FAILURE);

    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    ASSERT_EQ(xr.xrPollEvent(instance, &event), XR_SUCCESS) << "rejected calls consume no event";
    EXPECT_EQ(((XrEventDataSessionStateChanged*)&event)->state, XR_SESSION_STATE_IDLE);
    ASSERT_EQ(event.type, XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED);
    ASSERT_EQ(xr.xrPollEvent(instance, &event), XR_SUCCESS);
    EXPECT_EQ(((XrEventDataSessionStateChanged*)&event)->state, XR_SESSION_STATE_READY);
}

// The OpenXR API trace (config "trace", PUPPETXR_TRACE): each call is one log line with its
// arguments, its result and what it returned
// Trace lines outgrow the log history's lines; the callback gets them whole
static std::vector<std::string> trace_lines;

static void collect_trace (OS_LogLevel, const char* text, void*)
{
    if (cstr_starts_with(text, "trace: "))
    {
        trace_lines.emplace_back(text);
    }
}

static const char* traced (const char* call)
{
    for (usize i = trace_lines.size(); i > 0; i--)
    {
        if (cstr_starts_with(trace_lines[i - 1].c_str() + 7, call))
        {
            return trace_lines[i - 1].c_str();
        }
    }
    return nullptr;
}

struct OxrTraceTest : OxrTest
{
    void SetUp () override
    {
        trace_lines.clear();
        os_set_log_callback(collect_trace, nullptr);
        os_set_env("PUPPETXR_TRACE", "1");
        OxrTest::SetUp();
    }

    void TearDown () override
    {
        OxrTest::TearDown();
        os_unset_env("PUPPETXR_TRACE");
        os_set_log_callback(nullptr, nullptr);
    }
};

TEST_F(OxrTraceTest, calls_are_logged_with_arguments_results_and_outputs)
{
    create_headless_instance();
    const char* create = traced("xrCreateInstance(");
    ASSERT_NE(create, nullptr);
    EXPECT_NE(cstr_find(create, "createInfo={type=XR_TYPE_INSTANCE_CREATE_INFO, next=NULL"), nullptr) << create;
    EXPECT_NE(cstr_find(create, "applicationName=\"puppetxr_tests\""), nullptr) << create;
    EXPECT_NE(cstr_find(create, "enabledExtensionNames=[\"XR_MND_headless\""), nullptr) << create;
    EXPECT_NE(cstr_find(create, ") -> XR_SUCCESS {instance=0x"), nullptr) << create;

    XrPath left = XR_NULL_PATH;
    ASSERT_EQ(xr.xrStringToPath(instance, "/user/hand/left", &left), XR_SUCCESS);
    char     buffer[XR_MAX_PATH_LENGTH];
    uint32_t length = 0;
    ASSERT_EQ(xr.xrPathToString(instance, left, sizeof(buffer), &length, buffer), XR_SUCCESS);
    const char* to_string = traced("xrPathToString(");
    ASSERT_NE(to_string, nullptr);
    EXPECT_NE(cstr_find(to_string, "path=/user/hand/left"), nullptr) << "paths print as their names: " << to_string;
    EXPECT_NE(cstr_find(to_string, "{bufferCountOutput=16, buffer=\"/user/hand/left\"}"), nullptr) << to_string;

    const XrSystemGetInfo wrong = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY };
    XrSystemId            id    = XR_NULL_SYSTEM_ID;
    EXPECT_EQ(xr.xrGetSystem(instance, &wrong, &id), XR_ERROR_FORM_FACTOR_UNSUPPORTED);
    const char* get_system = traced("xrGetSystem(");
    ASSERT_NE(get_system, nullptr);
    EXPECT_NE(cstr_find(get_system, "formFactor=XR_FORM_FACTOR_HANDHELD_DISPLAY"), nullptr) << get_system;
    EXPECT_NE(cstr_find(get_system, "-> XR_ERROR_FORM_FACTOR_UNSUPPORTED"), nullptr) << get_system;
    EXPECT_EQ(cstr_find(get_system, "systemId="), nullptr) << "outputs only on success: " << get_system;
}

// The loader looks xrCreateInstance up before the instance reads the config; a process that made an
// instance before traced the next one's xrCreateInstance with the first one's filter
TEST_F(OxrTraceTest, lookups_without_an_instance_read_the_filter_again)
{
    create_headless_instance();
    ASSERT_EQ(xr.xrDestroyInstance(instance), XR_SUCCESS);
    instance = XR_NULL_HANDLE;
    os_set_env("PUPPETXR_TRACE", "xrGetSystem");
    trace_lines.clear();
    PFN_xrCreateInstance create = nullptr;
    ASSERT_EQ(gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&create), XR_SUCCESS);
    const char*                extensions[] = { XR_MND_HEADLESS_EXTENSION_NAME };
    const XrInstanceCreateInfo info         = {
        .type                  = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo       = { .applicationName = "puppetxr_tests", .applicationVersion = 1, .engineName = "", .engineVersion = 0, .apiVersion = XR_API_VERSION_1_1 },
        .enabledExtensionCount = 1,
        .enabledExtensionNames = extensions,
    };
    ASSERT_EQ(create(&info, &instance), XR_SUCCESS);
    EXPECT_EQ(traced("xrCreateInstance("), nullptr);
}

// An engine suggests a hundred bindings per profile; past TRACE_MAX_ELEMENTS the line says how many it left out
TEST_F(OxrTraceTest, long_arrays_end_with_how_many_were_left_out)
{
    create_headless_instance();
    const XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .actionSetName = "set", .localizedActionSetName = "Set" };
    XrActionSet                 set      = XR_NULL_HANDLE;
    ASSERT_EQ(xr.xrCreateActionSet(instance, &set_info, &set), XR_SUCCESS);
    const XrActionCreateInfo action_info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionName = "select", .actionType = XR_ACTION_TYPE_BOOLEAN_INPUT, .localizedActionName = "Select" };
    XrAction                 action      = XR_NULL_HANDLE;
    ASSERT_EQ(xr.xrCreateAction(set, &action_info, &action), XR_SUCCESS);
    XrPath profile = XR_NULL_PATH;
    XrPath select  = XR_NULL_PATH;
    ASSERT_EQ(xr.xrStringToPath(instance, "/interaction_profiles/khr/simple_controller", &profile), XR_SUCCESS);
    ASSERT_EQ(xr.xrStringToPath(instance, "/user/hand/left/input/select/click", &select), XR_SUCCESS);
    XrActionSuggestedBinding bindings[130];
    for (XrActionSuggestedBinding& binding : bindings)
    {
        binding = { .action = action, .binding = select };
    }
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = profile, .countSuggestedBindings = 130, .suggestedBindings = bindings };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    const char* line = traced("xrSuggestInteractionProfileBindings(");
    ASSERT_NE(line, nullptr);
    EXPECT_NE(cstr_find(line, "binding=/user/hand/left/input/select/click}, ... 2 more]"), nullptr) << line;
}

// A trace of everything logs a hundred calls a frame; the filter keeps the ones a question needs
TEST(OxrTrace, the_filter_names_functions_with_wildcards)
{
    EXPECT_TRUE(oxr_trace_matches("*", "xrWaitFrame"));
    EXPECT_TRUE(oxr_trace_matches("xrWaitFrame", "xrWaitFrame"));
    EXPECT_FALSE(oxr_trace_matches("xrWaitFrame", "xrWaitFrameX"));
    EXPECT_TRUE(oxr_trace_matches("xrDiscover*, *Space*", "xrLocateSpace"));
    EXPECT_TRUE(oxr_trace_matches("xrDiscover*, *Space*", "xrDiscoverSpacesMETA"));
    EXPECT_FALSE(oxr_trace_matches("xrDiscover*, *Space*", "xrWaitFrame"));
    EXPECT_TRUE(oxr_trace_matches("xr*State*", "xrGetActionStateBoolean"));
    EXPECT_FALSE(oxr_trace_matches("", "xrWaitFrame"));
    EXPECT_FALSE(oxr_trace_matches(" , ", "xrWaitFrame"));
}

struct OxrTraceFilterTest : OxrTraceTest
{
    void SetUp () override
    {
        trace_lines.clear();
        os_set_log_callback(collect_trace, nullptr);
        os_set_env("PUPPETXR_TRACE", "xrGetSystem");
        OxrTest::SetUp();
    }
};

TEST_F(OxrTraceFilterTest, only_the_named_functions_are_traced)
{
    create_headless_instance();
    EXPECT_EQ(traced("xrCreateInstance("), nullptr);
    EXPECT_NE(traced("xrGetSystem("), nullptr);
}

TEST_F(OxrTest, calls_are_not_traced_by_default)
{
    trace_lines.clear();
    os_set_log_callback(collect_trace, nullptr);
    create_headless_instance();
    os_set_log_callback(nullptr, nullptr);
    EXPECT_EQ(traced("xrCreateInstance("), nullptr);
    EXPECT_TRUE(trace_lines.empty());
}

// A second xrWaitFrame blocks until the previous frame is begun (spec 10.4.1). On a single thread
// that would never happen, so after OXR_FRAME_GATE_TIMEOUT_NS (one second) it warns and returns.
TEST_F(OxrTest, a_second_wait_without_begin_times_out_with_a_warning)
{
    create_headless_instance();
    create_session();
    begin_session();
    const XrFrameWaitInfo wait_info = { .type = XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState          first     = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &first), XR_SUCCESS);

    const u64    sequence = os_log_sequence();
    const u64    start    = os_monotonic_ns();
    XrFrameState second   = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &second), XR_SUCCESS);
    const u64 waited = os_monotonic_ns() - start;
    EXPECT_GE(waited, 1000000000ull);
    EXPECT_LT(waited, 3000000000ull);
    EXPECT_GT(second.predictedDisplayTime, first.predictedDisplayTime);
    EXPECT_GT(os_log_sequence(), sequence);
    b8 warned = false;
    for (u32 i = 0; i < os_log_history_count(); i++)
    {
        warned |= os_log_history(i)->level == OS_LogLevel::Warning && cstr_find(os_log_history(i)->text, "xrBeginFrame") != nullptr;
    }
    EXPECT_TRUE(warned);

    const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    EXPECT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = second.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
}

// Unreal's game thread waits frame N+1 while its RHI thread has begun but not ended frame N; the
// next xrWaitFrame still blocks until N+1 is begun. Passing it let the game thread acquire two
// images per wait, which Unreal logged as waits on unacquired swapchain images.
TEST_F(OxrTest, a_wait_blocks_while_the_previous_frame_is_unbegun_and_an_older_one_unended)
{
    create_headless_instance();
    create_session();
    begin_session();
    const XrFrameWaitInfo  wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
    const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    XrFrameState           first      = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &first), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    XrFrameState second = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &second), XR_SUCCESS);

    const u64    start = os_monotonic_ns();
    XrFrameState third = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &third), XR_SUCCESS);
    EXPECT_GE(os_monotonic_ns() - start, 1000000000ull);
}

TEST_F(OxrTest, session_state_machine)
{
    create_headless_instance();
    create_session();
    ASSERT_NE(session, XR_NULL_HANDLE);

    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    ASSERT_EQ(xr.xrPollEvent(instance, &event), XR_SUCCESS);
    ASSERT_EQ(event.type, XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED);
    EXPECT_EQ(((XrEventDataSessionStateChanged*)&event)->state, XR_SESSION_STATE_IDLE);
    EXPECT_EQ(((XrEventDataSessionStateChanged*)&event)->session, session);
    ASSERT_EQ(xr.xrPollEvent(instance, &event), XR_SUCCESS);
    EXPECT_EQ(((XrEventDataSessionStateChanged*)&event)->state, XR_SESSION_STATE_READY);
    EXPECT_EQ(xr.xrPollEvent(instance, &event), XR_EVENT_UNAVAILABLE);
    EXPECT_EQ(xr.xrPollEvent(instance, nullptr), XR_ERROR_VALIDATION_FAILURE);

    const XrFrameWaitInfo wait_info = { .type = XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState          state     = { .type = XR_TYPE_FRAME_STATE };
    EXPECT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_ERROR_SESSION_NOT_RUNNING);
    EXPECT_EQ(xr.xrEndSession(session), XR_ERROR_SESSION_NOT_RUNNING);

    XrSessionBeginInfo begin_info = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO };
    EXPECT_EQ(xr.xrBeginSession(session, &begin_info), XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
    begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    EXPECT_EQ(xr.xrBeginSession(session, &begin_info), XR_SUCCESS);
    EXPECT_EQ(xr.xrBeginSession(session, &begin_info), XR_ERROR_SESSION_RUNNING);

    XrSessionState states[3] = {};
    for (u32 i = 0; i < 3; i++)
    {
        ASSERT_EQ(xr.xrPollEvent(instance, &event), XR_SUCCESS);
        ASSERT_EQ(event.type, XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED);
        states[i] = ((XrEventDataSessionStateChanged*)&event)->state;
    }
    EXPECT_EQ(states[0], XR_SESSION_STATE_SYNCHRONIZED);
    EXPECT_EQ(states[1], XR_SESSION_STATE_VISIBLE);
    EXPECT_EQ(states[2], XR_SESSION_STATE_FOCUSED);

    EXPECT_EQ(xr.xrRequestExitSession(session), XR_SUCCESS);
    for (u32 i = 0; i < 3; i++)
    {
        ASSERT_EQ(xr.xrPollEvent(instance, &event), XR_SUCCESS);
        states[i] = ((XrEventDataSessionStateChanged*)&event)->state;
    }
    EXPECT_EQ(states[0], XR_SESSION_STATE_VISIBLE);
    EXPECT_EQ(states[1], XR_SESSION_STATE_SYNCHRONIZED);
    EXPECT_EQ(states[2], XR_SESSION_STATE_STOPPING);

    EXPECT_EQ(xr.xrEndSession(session), XR_SUCCESS);
    for (u32 i = 0; i < 2; i++)
    {
        ASSERT_EQ(xr.xrPollEvent(instance, &event), XR_SUCCESS);
        states[i] = ((XrEventDataSessionStateChanged*)&event)->state;
    }
    EXPECT_EQ(states[0], XR_SESSION_STATE_IDLE);
    EXPECT_EQ(states[1], XR_SESSION_STATE_EXITING);
    EXPECT_EQ(xr.xrPollEvent(instance, &event), XR_EVENT_UNAVAILABLE);

    EXPECT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySession(session), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(xr.xrBeginSession(session, &begin_info), XR_ERROR_HANDLE_INVALID);
    session = XR_NULL_HANDLE;
}

TEST_F(OxrTest, ending_session_without_exit_request_returns_to_ready)
{
    create_headless_instance();
    create_session();
    begin_session();

    EXPECT_EQ(xr.xrRequestExitSession(session), XR_SUCCESS);
    EXPECT_EQ(drain_events(), XR_SESSION_STATE_STOPPING);
    EXPECT_EQ(xr.xrEndSession(session), XR_SUCCESS);
    EXPECT_EQ(drain_events(), XR_SESSION_STATE_EXITING);

    EXPECT_EQ(xr.xrRequestExitSession(session), XR_ERROR_SESSION_NOT_RUNNING);
}

TEST_F(OxrTest, destroying_instance_destroys_children)
{
    create_headless_instance();
    create_session();
    const XrSpace space = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    const XrSession old_session = session;

    EXPECT_EQ(xr.xrDestroyInstance(instance), XR_SUCCESS);
    instance = XR_NULL_HANDLE;
    session  = XR_NULL_HANDLE;

    ASSERT_EQ(create_instance(nullptr, 0), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySpace(space), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(xr.xrDestroySession(old_session), XR_ERROR_HANDLE_INVALID);
}

//
// Frames
//

TEST_F(OxrTest, frame_loop_timing_and_call_order)
{
    create_headless_instance();
    create_session();
    begin_session();

    const XrFrameWaitInfo  wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
    const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    XrFrameState           state      = { .type = XR_TYPE_FRAME_STATE };

    EXPECT_EQ(xr.xrBeginFrame(session, &begin_info), XR_ERROR_CALL_ORDER_INVALID);
    XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = 1, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_CALL_ORDER_INVALID);

    EXPECT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    EXPECT_GT(state.predictedDisplayTime, 0);
    EXPECT_GT(state.predictedDisplayPeriod, 0);
    EXPECT_FALSE(state.shouldRender);
    EXPECT_EQ(xr.xrWaitFrame(session, &wait_info, nullptr), XR_ERROR_VALIDATION_FAILURE);

    EXPECT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    EXPECT_EQ(xr.xrBeginFrame(session, nullptr), XR_ERROR_CALL_ORDER_INVALID);

    end_info.displayTime = 0;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_TIME_INVALID);
    end_info.displayTime = state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED);
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = 1;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_VALIDATION_FAILURE);
    const XrCompositionLayerBaseHeader* null_layer = nullptr;
    end_info.layers = &null_layer;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_LAYER_INVALID);
    end_info.layerCount = 0;
    end_info.layers = nullptr;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_CALL_ORDER_INVALID);

    XrFrameState second = { .type = XR_TYPE_FRAME_STATE };
    EXPECT_EQ(xr.xrWaitFrame(session, nullptr, &second), XR_SUCCESS);
    EXPECT_EQ(second.predictedDisplayTime, state.predictedDisplayTime + state.predictedDisplayPeriod);
    EXPECT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    EXPECT_EQ(xr.xrWaitFrame(session, &wait_info, &second), XR_SUCCESS);
    EXPECT_EQ(xr.xrBeginFrame(session, &begin_info), XR_FRAME_DISCARDED);
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
}

// Regression: the pacing measured from the previous wait's start, before its sleep, so every
// other wait saw a whole period gone and did not sleep; frames came in pairs at twice the rate
TEST_F(OxrTest, realtime_waits_pace_every_frame_to_the_display_period)
{
    os_unset_env("PUPPETXR_FIXED_STEP");
    create_headless_instance();
    create_session();
    begin_session();
    ASSERT_EQ(sim_clock_mode(oxr_get_sim()), SIM_ClockMode::Realtime);
    const u64 period = (u64)sim_display_period(oxr_get_sim());
    frame();
    const u32 frames = 16;
    const u64 start  = os_monotonic_ns();
    for (u32 i = 0; i < frames; i++)
    {
        frame();
    }
    EXPECT_GE(os_monotonic_ns() - start, (frames - 1) * period);
}

//
// Spaces
//

TEST_F(OxrTest, reference_spaces_and_locate)
{
    create_headless_instance();
    create_session();
    begin_session();
    const XrFrameState frame_state = frame();

    u32 count = 0;
    EXPECT_EQ(xr.xrEnumerateReferenceSpaces(session, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 4u);
    XrReferenceSpaceType types[4] = {};
    EXPECT_EQ(xr.xrEnumerateReferenceSpaces(session, 4, &count, types), XR_SUCCESS);

    const XrSpace view  = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    const XrSpace local = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    const XrSpace stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrSpace floor = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR);

    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    EXPECT_EQ(xr.xrLocateSpace(view, stage, frame_state.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
    EXPECT_NEAR(location.pose.position.y, 1.6f, 1e-5f);

    EXPECT_EQ(xr.xrLocateSpace(local, floor, frame_state.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_NEAR(location.pose.position.y, 1.6f, 1e-5f);
    EXPECT_NEAR(location.pose.position.x, 0.0f, 1e-5f);

    EXPECT_EQ(xr.xrLocateSpace(local, local, frame_state.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_NEAR(location.pose.position.x, 0.0f, 1e-6f);
    EXPECT_NEAR(location.pose.orientation.w, 1.0f, 1e-6f);

    XrSpaceVelocity velocity = { .type = XR_TYPE_SPACE_VELOCITY };
    location.next            = &velocity;
    EXPECT_EQ(xr.xrLocateSpace(view, stage, frame_state.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_TRUE(velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT);
    location.next = nullptr;

    EXPECT_EQ(xr.xrLocateSpace(view, stage, 0, &location), XR_ERROR_TIME_INVALID);
    EXPECT_EQ(xr.xrLocateSpace(view, stage, -5, &location), XR_ERROR_TIME_INVALID);
    EXPECT_EQ(xr.xrLocateSpace(view, (XrSpace)(uintptr_t)0x1234, frame_state.predictedDisplayTime, &location), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(xr.xrLocateSpace(view, stage, frame_state.predictedDisplayTime, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xr.xrLocateSpace(view, (XrSpace)(uintptr_t)session, frame_state.predictedDisplayTime, &location), XR_ERROR_HANDLE_INVALID);

    const XrSpace offset = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE, { { 0, 0, 0, 1 }, { 1, 2, 3 } });
    EXPECT_EQ(xr.xrLocateSpace(offset, stage, frame_state.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_NEAR(location.pose.position.x, 1.0f, 1e-6f);
    EXPECT_NEAR(location.pose.position.z, 3.0f, 1e-6f);

    XrExtent2Df bounds = {};
    EXPECT_EQ(xr.xrGetReferenceSpaceBoundsRect(session, XR_REFERENCE_SPACE_TYPE_STAGE, &bounds), XR_SUCCESS);
    EXPECT_GT(bounds.width, 0.0f);
    EXPECT_EQ(xr.xrGetReferenceSpaceBoundsRect(session, XR_REFERENCE_SPACE_TYPE_VIEW, &bounds), XR_SPACE_BOUNDS_UNAVAILABLE);
    EXPECT_EQ(xr.xrGetReferenceSpaceBoundsRect(session, (XrReferenceSpaceType)99, &bounds), XR_ERROR_REFERENCE_SPACE_UNSUPPORTED);

    XrSpace bad = XR_NULL_HANDLE;
    XrReferenceSpaceCreateInfo info = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = (XrReferenceSpaceType)99, .poseInReferenceSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    EXPECT_EQ(xr.xrCreateReferenceSpace(session, &info, &bad), XR_ERROR_REFERENCE_SPACE_UNSUPPORTED);
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    info.poseInReferenceSpace.orientation = { 0, 0, 0, 0 };
    EXPECT_EQ(xr.xrCreateReferenceSpace(session, &info, &bad), XR_ERROR_POSE_INVALID);
    info.poseInReferenceSpace.orientation = { 0, 0, 0, 1 };
    info.poseInReferenceSpace.position.x  = zm::nan();
    EXPECT_EQ(xr.xrCreateReferenceSpace(session, &info, &bad), XR_ERROR_POSE_INVALID);

    EXPECT_EQ(xr.xrDestroySpace(view), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySpace(view), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(xr.xrLocateSpace(view, stage, frame_state.predictedDisplayTime, &location), XR_ERROR_HANDLE_INVALID);

    const XrSpace      targets[2]  = { local, floor };
    const XrSpacesLocateInfo locate_info = { .type = XR_TYPE_SPACES_LOCATE_INFO, .baseSpace = stage, .time = frame_state.predictedDisplayTime, .spaceCount = 2, .spaces = targets };
    XrSpaceLocationData data[2] = {};
    XrSpaceLocations locations = { .type = XR_TYPE_SPACE_LOCATIONS, .locationCount = 2, .locations = data };
    EXPECT_EQ(xr.xrLocateSpaces(session, &locate_info, &locations), XR_SUCCESS);
    EXPECT_NEAR(data[0].pose.position.y, 1.6f, 1e-5f);
    EXPECT_NEAR(data[1].pose.position.y, 0.0f, 1e-5f);
    locations.locationCount = 1;
    EXPECT_EQ(xr.xrLocateSpaces(session, &locate_info, &locations), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrTest, locate_views)
{
    create_headless_instance();
    create_session();
    begin_session();
    const XrFrameState frame_state = frame();
    const XrSpace      stage       = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);

    XrViewLocateInfo info = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = frame_state.predictedDisplayTime, .space = stage };
    XrViewState      state = { .type = XR_TYPE_VIEW_STATE };
    u32              count = 0;
    EXPECT_EQ(xr.xrLocateViews(session, &info, &state, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 2u);

    XrView views[2] = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
    EXPECT_EQ(xr.xrLocateViews(session, &info, &state, 1, &count, views), XR_ERROR_SIZE_INSUFFICIENT);
    EXPECT_EQ(xr.xrLocateViews(session, &info, &state, 2, &count, views), XR_SUCCESS);
    EXPECT_TRUE(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
    EXPECT_NEAR(views[1].pose.position.x - views[0].pose.position.x, 0.064f, 1e-5f);
    EXPECT_LT(views[0].fov.angleLeft, 0.0f);
    EXPECT_GT(views[0].fov.angleRight, 0.0f);

    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
    EXPECT_EQ(xr.xrLocateViews(session, &info, &state, 2, &count, views), XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = 0;
    EXPECT_EQ(xr.xrLocateViews(session, &info, &state, 2, &count, views), XR_ERROR_TIME_INVALID);
    info.displayTime = frame_state.predictedDisplayTime;
    info.space = XR_NULL_HANDLE;
    EXPECT_EQ(xr.xrLocateViews(session, &info, &state, 2, &count, views), XR_ERROR_HANDLE_INVALID);
}

//
// Swapchains in headless mode
//

TEST_F(OxrTest, headless_session_has_no_swapchains)
{
    create_headless_instance();
    create_session();

    u32 count = 99;
    EXPECT_EQ(xr.xrEnumerateSwapchainFormats(session, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 0u);

    XrSwapchain swapchain = XR_NULL_HANDLE;
    const XrSwapchainCreateInfo info = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, .format = 1, .sampleCount = 1, .width = 64, .height = 64, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
    EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_ERROR_FEATURE_UNSUPPORTED);
    EXPECT_EQ(xr.xrDestroySwapchain((XrSwapchain)(uintptr_t)0x99), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(xr.xrEnumerateSwapchainImages((XrSwapchain)(uintptr_t)0x99, 0, &count, nullptr), XR_ERROR_HANDLE_INVALID);
}

//
// Actions
//

namespace
{

struct OxrActionTest : OxrTest
{
    XrActionSet set      = XR_NULL_HANDLE;
    XrAction    trigger  = XR_NULL_HANDLE;
    XrAction    click    = XR_NULL_HANDLE;
    XrAction    stick    = XR_NULL_HANDLE;
    XrAction    grip     = XR_NULL_HANDLE;
    XrAction    haptic   = XR_NULL_HANDLE;
    XrPath      hands[2] = {};

    void SetUp () override
    {
        OxrTest::SetUp();
        create_headless_instance();
        create_session();
        hands[0] = path("/user/hand/left");
        hands[1] = path("/user/hand/right");

        const XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .actionSetName = "gameplay", .localizedActionSetName = "Gameplay", .priority = 0 };
        ASSERT_EQ(xr.xrCreateActionSet(instance, &set_info, &set), XR_SUCCESS);

        trigger = make_action("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
        click   = make_action("click", XR_ACTION_TYPE_BOOLEAN_INPUT);
        stick   = make_action("stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
        grip    = make_action("grip", XR_ACTION_TYPE_POSE_INPUT);
        haptic  = make_action("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    }

    XrAction make_action (const char* name, XrActionType type, u32 subaction_count = 2)
    {
        XrAction           action = XR_NULL_HANDLE;
        XrActionCreateInfo info   = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionType = type, .countSubactionPaths = subaction_count, .subactionPaths = hands };
        cstr_copy(info.actionName, sizeof(info.actionName), name);
        cstr_copy(info.localizedActionName, sizeof(info.localizedActionName), name);
        EXPECT_EQ(xr.xrCreateAction(set, &info, &action), XR_SUCCESS);
        return action;
    }

    void suggest_touch_bindings ()
    {
        const XrActionSuggestedBinding bindings[] = {
            { trigger, path("/user/hand/left/input/trigger/value") },
            { trigger, path("/user/hand/right/input/trigger/value") },
            { click,   path("/user/hand/left/input/x/click") },
            { click,   path("/user/hand/right/input/a/click") },
            { stick,   path("/user/hand/left/input/thumbstick") },
            { stick,   path("/user/hand/right/input/thumbstick") },
            { grip,    path("/user/hand/left/input/grip/pose") },
            { grip,    path("/user/hand/right/input/grip/pose") },
            { haptic,  path("/user/hand/left/output/haptic") },
            { haptic,  path("/user/hand/right/output/haptic") },
        };
        const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = ARRAY_SIZE(bindings), .suggestedBindings = bindings };
        ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    }

    void attach ()
    {
        const XrSessionActionSetsAttachInfo info = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &set };
        ASSERT_EQ(xr.xrAttachSessionActionSets(session, &info), XR_SUCCESS);
    }

    XrResult sync ()
    {
        const XrActiveActionSet active = { .actionSet = set, .subactionPath = XR_NULL_PATH };
        const XrActionsSyncInfo info   = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active };
        return xr.xrSyncActions(session, &info);
    }

    XrActionStateFloat float_state (XrAction action, XrPath subaction)
    {
        XrActionStateFloat state = { .type = XR_TYPE_ACTION_STATE_FLOAT };
        const XrActionStateGetInfo info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = action, .subactionPath = subaction };
        EXPECT_EQ(xr.xrGetActionStateFloat(session, &info, &state), XR_SUCCESS);
        return state;
    }

    XrActionStateBoolean bool_state (XrAction action, XrPath subaction)
    {
        XrActionStateBoolean state = { .type = XR_TYPE_ACTION_STATE_BOOLEAN };
        const XrActionStateGetInfo info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = action, .subactionPath = subaction };
        EXPECT_EQ(xr.xrGetActionStateBoolean(session, &info, &state), XR_SUCCESS);
        return state;
    }
};

} // namespace

TEST_F(OxrActionTest, action_set_and_action_name_validation)
{
    XrActionSet           other = XR_NULL_HANDLE;
    XrActionSetCreateInfo info  = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .actionSetName = "gameplay", .localizedActionSetName = "Other", .priority = 0 };
    EXPECT_EQ(xr.xrCreateActionSet(instance, &info, &other), XR_ERROR_NAME_DUPLICATED);
    cstr_copy(info.actionSetName, sizeof(info.actionSetName), "other");
    cstr_copy(info.localizedActionSetName, sizeof(info.localizedActionSetName), "Gameplay");
    EXPECT_EQ(xr.xrCreateActionSet(instance, &info, &other), XR_ERROR_LOCALIZED_NAME_DUPLICATED);
    cstr_copy(info.actionSetName, sizeof(info.actionSetName), "Bad Name");
    EXPECT_EQ(xr.xrCreateActionSet(instance, &info, &other), XR_ERROR_PATH_FORMAT_INVALID);
    cstr_copy(info.actionSetName, sizeof(info.actionSetName), "");
    EXPECT_EQ(xr.xrCreateActionSet(instance, &info, &other), XR_ERROR_NAME_INVALID);
    cstr_copy(info.actionSetName, sizeof(info.actionSetName), "other");
    cstr_copy(info.localizedActionSetName, sizeof(info.localizedActionSetName), "");
    EXPECT_EQ(xr.xrCreateActionSet(instance, &info, &other), XR_ERROR_LOCALIZED_NAME_INVALID);
    cstr_copy(info.localizedActionSetName, sizeof(info.localizedActionSetName), "Other");
    EXPECT_EQ(xr.xrCreateActionSet(instance, &info, &other), XR_SUCCESS);

    XrAction           action      = XR_NULL_HANDLE;
    XrActionCreateInfo action_info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionName = "trigger", .actionType = XR_ACTION_TYPE_FLOAT_INPUT, .localizedActionName = "Trigger2" };
    EXPECT_EQ(xr.xrCreateAction(set, &action_info, &action), XR_ERROR_NAME_DUPLICATED);
    EXPECT_EQ(xr.xrCreateAction(other, &action_info, &action), XR_SUCCESS);
    cstr_copy(action_info.actionName, sizeof(action_info.actionName), "trigger2");
    cstr_copy(action_info.localizedActionName, sizeof(action_info.localizedActionName), "trigger");
    EXPECT_EQ(xr.xrCreateAction(set, &action_info, &action), XR_ERROR_LOCALIZED_NAME_DUPLICATED);
    action_info.actionType = (XrActionType)999;
    cstr_copy(action_info.localizedActionName, sizeof(action_info.localizedActionName), "x");
    EXPECT_EQ(xr.xrCreateAction(set, &action_info, &action), XR_ERROR_VALIDATION_FAILURE);
    action_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    const XrPath bad_sub = path("/user/head/input/x");
    action_info.countSubactionPaths = 1;
    action_info.subactionPaths = &bad_sub;
    EXPECT_EQ(xr.xrCreateAction(set, &action_info, &action), XR_ERROR_PATH_UNSUPPORTED);
    XrPath dup_sub[2] = { hands[0], hands[0] };
    action_info.countSubactionPaths = 2;
    action_info.subactionPaths = dup_sub;
    EXPECT_EQ(xr.xrCreateAction(set, &action_info, &action), XR_ERROR_PATH_UNSUPPORTED);

    EXPECT_EQ(xr.xrDestroyActionSet(other), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroyActionSet(other), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(xr.xrDestroyAction(action), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(xr.xrDestroyAction((XrAction)(uintptr_t)set), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrActionTest, suggest_bindings_follow_the_registry)
{
    XrActionSuggestedBinding             binding   = { click, path("/user/hand/left/input/select/click") };
    XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/khr/simple_controller"), .countSuggestedBindings = 1, .suggestedBindings = &binding };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);

    const char* bad_profiles[] = { "/interaction_profiles/khr/simple_controller/invalid", "/interaction_profiles/khr/another_controller", "/interaction_profiles", "/invalid" };
    for (const char* profile : bad_profiles)
    {
        suggested.interactionProfile = path(profile);
        EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED) << profile;
    }

    suggested.interactionProfile = path("/interaction_profiles/khr/simple_controller");
    const char* bad_bindings[] = { "/user/hand/right", "/user/hand/right/input", "/user/hand/left/output", "/user/hand/right/input/select/click/invalid", "/user/hand/left/input_bad/menu/click", "/user/head/input/select/click", "/user/hand/left/input/trigger/value" };
    for (const char* bad : bad_bindings)
    {
        binding.binding = path(bad);
        EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED) << bad;
    }

    // Parent paths of an input are valid, spec 6.3.2
    binding.binding = path("/user/hand/left/input/select");
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    binding = { haptic, path("/user/hand/right/output/haptic") };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);

    // Profiles the simulator does not emulate are still validated against their own inputs
    suggested.interactionProfile = path("/interaction_profiles/valve/index_controller");
    binding = { stick, path("/user/hand/left/input/thumbstick") };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    binding.binding = path("/user/hand/left/input/trackpad/force");
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    binding.binding = path("/user/hand/left/input/menu/click");
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED);

    suggested.interactionProfile = path("/interaction_profiles/meta/touch_plus_controller");
    binding = { grip, path("/user/hand/left/input/grip_surface/pose") };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    suggested.interactionProfile = path("/interaction_profiles/meta/touch_controller_plus");
    binding.binding              = path("/user/hand/left/input/grip/pose");
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED);
}

TEST_F(OxrTest, suggest_bindings_respect_the_api_version)
{
    const char* extensions[] = { XR_MND_HEADLESS_EXTENSION_NAME };
    ASSERT_EQ(create_instance(extensions, 1, XR_API_VERSION_1_0), XR_SUCCESS);
    XrActionSet                 set      = XR_NULL_HANDLE;
    const XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .actionSetName = "set", .localizedActionSetName = "Set", .priority = 0 };
    ASSERT_EQ(xr.xrCreateActionSet(instance, &set_info, &set), XR_SUCCESS);
    XrAction                 pose        = XR_NULL_HANDLE;
    const XrActionCreateInfo action_info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionName = "pose", .actionType = XR_ACTION_TYPE_POSE_INPUT, .localizedActionName = "Pose" };
    ASSERT_EQ(xr.xrCreateAction(set, &action_info, &pose), XR_SUCCESS);

    XrActionSuggestedBinding             binding   = { pose, path("/user/hand/left/input/grip/pose") };
    XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = 1, .suggestedBindings = &binding };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    binding.binding = path("/user/hand/left/input/grip_surface/pose");
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED);
    binding.binding              = path("/user/hand/left/input/grip/pose");
    suggested.interactionProfile = path("/interaction_profiles/meta/touch_plus_controller");
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED);
}

TEST_F(OxrActionTest, suggest_bindings_validation)
{
    XrActionSuggestedBinding binding = { trigger, path("/user/hand/left/input/trigger/value") };
    XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/user/hand/left"), .countSuggestedBindings = 1, .suggestedBindings = &binding };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED);

    // Profiles the simulator does not emulate are accepted so engines that suggest every controller keep working
    suggested.interactionProfile = path("/interaction_profiles/valve/index_controller");
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    binding.binding = path("/user/hand/left/not/an/input");
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED);
    binding.binding = path("/user/hand/left/input/trigger/value");

    suggested.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    binding.binding = path("/user/hand/left/input/not/a/thing");
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED);
    binding.binding = path("/user/hand/left/input/trigger/value");
    binding.action  = (XrAction)(uintptr_t)0x77;
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_HANDLE_INVALID);
    binding.action = trigger;
    suggested.countSuggestedBindings = 0;
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_VALIDATION_FAILURE);
    suggested.countSuggestedBindings = 1;
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);

    attach();
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_ACTIONSETS_ALREADY_ATTACHED);
    XrAction           late = XR_NULL_HANDLE;
    const XrActionCreateInfo info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionName = "late", .actionType = XR_ACTION_TYPE_FLOAT_INPUT, .localizedActionName = "Late" };
    EXPECT_EQ(xr.xrCreateAction(set, &info, &late), XR_ERROR_ACTIONSETS_ALREADY_ATTACHED);

    const XrSessionActionSetsAttachInfo attach_info = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &set };
    EXPECT_EQ(xr.xrAttachSessionActionSets(session, &attach_info), XR_ERROR_ACTIONSETS_ALREADY_ATTACHED);
}

// Spec xrSuggestInteractionProfileBindings: the error is for actions of an attached set. Unreal's
// VR Preview builds new action sets for each session of its instance and suggests their bindings,
// which failed after the first session
TEST_F(OxrActionTest, new_action_sets_suggest_bindings_after_another_set_was_attached)
{
    attach();
    ASSERT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
    session = XR_NULL_HANDLE;
    create_session();

    const XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .actionSetName = "second", .localizedActionSetName = "Second" };
    XrActionSet                 second   = XR_NULL_HANDLE;
    ASSERT_EQ(xr.xrCreateActionSet(instance, &set_info, &second), XR_SUCCESS);
    const XrActionCreateInfo action_info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionName = "select", .actionType = XR_ACTION_TYPE_BOOLEAN_INPUT, .localizedActionName = "Select" };
    XrAction                 select      = XR_NULL_HANDLE;
    ASSERT_EQ(xr.xrCreateAction(second, &action_info, &select), XR_SUCCESS);
    XrActionSuggestedBinding             binding   = { select, path("/user/hand/left/input/select/click") };
    XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/khr/simple_controller"), .countSuggestedBindings = 1, .suggestedBindings = &binding };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    const XrSessionActionSetsAttachInfo attach_info = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &second };
    EXPECT_EQ(xr.xrAttachSessionActionSets(session, &attach_info), XR_SUCCESS);

    // The first set stays attached for good, though its session is gone
    binding.action = trigger;
    binding.binding = path("/user/hand/left/input/select/click");
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_ACTIONSETS_ALREADY_ATTACHED);
}

// An editor plays many sessions in one instance, each with action sets of the same names that it
// destroys afterwards; nothing of them stays behind
TEST_F(OxrActionTest, sessions_with_new_action_sets_leave_nothing_behind)
{
    ASSERT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
    session = XR_NULL_HANDLE;
    const OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt, nullptr);
    usize sets = 0;
    usize actions = 0;
    for (u32 round = 0; round < 100; round++)
    {
        create_session();
        const XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .actionSetName = "ue", .localizedActionSetName = "Unreal Engine" };
        XrActionSet                 ue       = XR_NULL_HANDLE;
        ASSERT_EQ(xr.xrCreateActionSet(instance, &set_info, &ue), XR_SUCCESS) << "round " << round;
        const XrActionCreateInfo action_info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionName = "select", .actionType = XR_ACTION_TYPE_BOOLEAN_INPUT, .localizedActionName = "Select" };
        XrAction                 select      = XR_NULL_HANDLE;
        ASSERT_EQ(xr.xrCreateAction(ue, &action_info, &select), XR_SUCCESS) << "round " << round;
        const XrActionSuggestedBinding             binding   = { select, path("/user/hand/left/input/select/click") };
        const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/khr/simple_controller"), .countSuggestedBindings = 1, .suggestedBindings = &binding };
        ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS) << "round " << round;
        const XrSessionActionSetsAttachInfo attach_info = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &ue };
        ASSERT_EQ(xr.xrAttachSessionActionSets(session, &attach_info), XR_SUCCESS) << "round " << round;
        if (round == 0)
        {
            sets    = rt->action_sets.length();
            actions = rt->actions.length();
        }
        EXPECT_EQ(rt->action_sets.length(), sets) << "round " << round;
        EXPECT_EQ(rt->actions.length(), actions) << "round " << round;
        ASSERT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
        session = XR_NULL_HANDLE;
        ASSERT_EQ(xr.xrDestroyActionSet(ue), XR_SUCCESS);
    }
    EXPECT_EQ(rt->sessions.length(), 0u);
    create_session();
}

TEST_F(OxrActionTest, states_before_attach_and_sync)
{
    XrActionStateFloat state = { .type = XR_TYPE_ACTION_STATE_FLOAT };
    XrActionStateGetInfo info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = trigger, .subactionPath = hands[0] };
    EXPECT_EQ(xr.xrGetActionStateFloat(session, &info, &state), XR_ERROR_ACTIONSET_NOT_ATTACHED);
    EXPECT_EQ(sync(), XR_ERROR_ACTIONSET_NOT_ATTACHED);

    suggest_touch_bindings();
    attach();
    EXPECT_EQ(xr.xrGetActionStateFloat(session, &info, &state), XR_SUCCESS);
    EXPECT_FALSE(state.isActive);

    XrInteractionProfileState profile = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
    EXPECT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[0], &profile), XR_SUCCESS);
    EXPECT_EQ(profile.interactionProfile, XR_NULL_PATH);

    begin_session();
    EXPECT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[0], &profile), XR_SUCCESS);
    EXPECT_EQ(profile.interactionProfile, path("/interaction_profiles/oculus/touch_controller"));
    EXPECT_EQ(xr.xrGetCurrentInteractionProfile(session, path("/user/head"), &profile), XR_SUCCESS);
    EXPECT_EQ(profile.interactionProfile, XR_NULL_PATH);
    EXPECT_EQ(xr.xrGetCurrentInteractionProfile(session, path("/user/foot"), &profile), XR_ERROR_PATH_UNSUPPORTED);

    b8 saw_profile_event = false;
    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (xr.xrPollEvent(instance, &event) == XR_SUCCESS)
    {
        saw_profile_event |= event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
    }
    EXPECT_TRUE(saw_profile_event);

    info.action = click;
    EXPECT_EQ(xr.xrGetActionStateFloat(session, &info, &state), XR_ERROR_ACTION_TYPE_MISMATCH);
    info.action = trigger;
    info.subactionPath = path("/user/head");
    EXPECT_EQ(xr.xrGetActionStateFloat(session, &info, &state), XR_ERROR_PATH_UNSUPPORTED);
    info.subactionPath = XR_NULL_PATH;
    EXPECT_EQ(xr.xrGetActionStateFloat(session, &info, &state), XR_SUCCESS);
    EXPECT_TRUE(state.isActive);
}

TEST_F(OxrActionTest, action_values_follow_simulated_inputs)
{
    suggest_touch_bindings();
    attach();
    begin_session();

    SIM_Sim* sim = oxr_get_sim();
    ASSERT_NE(sim, nullptr);
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue, { .value = { 0.8f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::AClick, { .value = { 1.0f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::Thumbstick, { .value = { 0.5f, -0.5f } });
    frame();
    EXPECT_EQ(sync(), XR_SUCCESS);

    XrActionStateFloat left = float_state(trigger, hands[0]);
    EXPECT_TRUE(left.isActive);
    EXPECT_FLOAT_EQ(left.currentState, 0.8f);
    EXPECT_TRUE(left.changedSinceLastSync);
    EXPECT_GT(left.lastChangeTime, 0);
    EXPECT_FLOAT_EQ(float_state(trigger, hands[1]).currentState, 0.0f);
    EXPECT_FLOAT_EQ(float_state(trigger, XR_NULL_PATH).currentState, 0.8f);

    XrActionStateBoolean right_click = bool_state(click, hands[1]);
    EXPECT_TRUE(right_click.currentState);
    EXPECT_TRUE(right_click.changedSinceLastSync);
    EXPECT_FALSE(bool_state(click, hands[0]).currentState);

    XrActionStateVector2f stick_state = { .type = XR_TYPE_ACTION_STATE_VECTOR2F };
    const XrActionStateGetInfo stick_info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = stick, .subactionPath = hands[1] };
    EXPECT_EQ(xr.xrGetActionStateVector2f(session, &stick_info, &stick_state), XR_SUCCESS);
    EXPECT_FLOAT_EQ(stick_state.currentState.x, 0.5f);
    EXPECT_FLOAT_EQ(stick_state.currentState.y, -0.5f);

    frame();
    EXPECT_EQ(sync(), XR_SUCCESS);
    left = float_state(trigger, hands[0]);
    EXPECT_FALSE(left.changedSinceLastSync);
    EXPECT_FLOAT_EQ(left.currentState, 0.8f);

    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::AClick, { .value = { 0.0f, 0.0f } });
    frame();
    EXPECT_EQ(sync(), XR_SUCCESS);
    right_click = bool_state(click, hands[1]);
    EXPECT_FALSE(right_click.currentState);
    EXPECT_TRUE(right_click.changedSinceLastSync);

    XrActionStatePose pose_state = { .type = XR_TYPE_ACTION_STATE_POSE };
    const XrActionStateGetInfo pose_info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = grip, .subactionPath = hands[0] };
    EXPECT_EQ(xr.xrGetActionStatePose(session, &pose_info, &pose_state), XR_SUCCESS);
    EXPECT_TRUE(pose_state.isActive);

    const XrActiveActionSet active = { .actionSet = set, .subactionPath = hands[0] };
    const XrActionsSyncInfo left_only = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active };
    EXPECT_EQ(xr.xrSyncActions(session, &left_only), XR_SUCCESS);
    EXPECT_TRUE(float_state(trigger, hands[0]).isActive);
    EXPECT_FALSE(float_state(trigger, hands[1]).isActive);
}

TEST_F(OxrActionTest, simple_controller_profile_maps_select_to_trigger)
{
    const XrActionSuggestedBinding bindings[] = {
        { click, path("/user/hand/left/input/select/click") },
        { click, path("/user/hand/right/input/select/click") },
        { grip,  path("/user/hand/left/input/aim/pose") },
    };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/khr/simple_controller"), .countSuggestedBindings = ARRAY_SIZE(bindings), .suggestedBindings = bindings };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    attach();
    begin_session();

    SIM_Sim* sim = oxr_get_sim();
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue, { .value = { 0.9f, 0.0f } });
    frame();
    EXPECT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(bool_state(click, hands[0]).currentState);
    EXPECT_FALSE(bool_state(click, hands[1]).currentState);

    XrInteractionProfileState profile = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
    EXPECT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[1], &profile), XR_SUCCESS);
    EXPECT_EQ(profile.interactionProfile, path("/interaction_profiles/khr/simple_controller"));

    u32 count = 0;
    const XrBoundSourcesForActionEnumerateInfo bound_info = { .type = XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, .action = click };
    EXPECT_EQ(xr.xrEnumerateBoundSourcesForAction(session, &bound_info, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 2u);
    XrPath sources[2] = {};
    EXPECT_EQ(xr.xrEnumerateBoundSourcesForAction(session, &bound_info, 2, &count, sources), XR_SUCCESS);
    EXPECT_EQ(sources[0], path("/user/hand/left/input/select/click"));

    char name[128] = {};
    u32  written   = 0;
    const XrInputSourceLocalizedNameGetInfo name_info = { .type = XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO, .sourcePath = sources[0], .whichComponents = XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT | XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT | XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT };
    EXPECT_EQ(xr.xrGetInputSourceLocalizedName(session, &name_info, sizeof(name), &written, name), XR_SUCCESS);
    EXPECT_GT(written, 1u);
}

TEST_F(OxrActionTest, action_spaces_follow_hands)
{
    suggest_touch_bindings();
    attach();
    begin_session();

    XrSpace space = XR_NULL_HANDLE;
    XrActionSpaceCreateInfo info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = grip, .subactionPath = hands[1], .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    ASSERT_EQ(xr.xrCreateActionSpace(session, &info, &space), XR_SUCCESS);
    info.action = trigger;
    XrSpace bad = XR_NULL_HANDLE;
    EXPECT_EQ(xr.xrCreateActionSpace(session, &info, &bad), XR_ERROR_ACTION_TYPE_MISMATCH);
    info.action = grip;
    info.subactionPath = path("/user/head");
    EXPECT_EQ(xr.xrCreateActionSpace(session, &info, &bad), XR_ERROR_PATH_UNSUPPORTED);

    const XrSpace      stage       = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrFrameState frame_state = frame();
    XrSpaceLocation    location    = { .type = XR_TYPE_SPACE_LOCATION };

    EXPECT_EQ(xr.xrLocateSpace(space, stage, frame_state.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_EQ(location.locationFlags, 0u);

    EXPECT_EQ(sync(), XR_SUCCESS);
    SIM_Sim* sim = oxr_get_sim();
    sim_set_device_pose(sim, SIM_Device::RightHand, { .position = { 0.5f, 1.0f, -0.5f }, .orientation = zm::QUAT_IDENTITY });
    EXPECT_EQ(xr.xrLocateSpace(space, stage, frame_state.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
    EXPECT_NEAR(location.pose.position.x, 0.5f, 1e-6f);
    EXPECT_NEAR(location.pose.position.z, -0.5f, 1e-6f);

    EXPECT_EQ(xr.xrDestroySpace(space), XR_SUCCESS);
}

TEST_F(OxrActionTest, grip_surface_and_parent_pose_paths_are_emulated)
{
    const XrActionSuggestedBinding bindings[] = {
        { grip, path("/user/hand/left/input/grip_surface/pose") },
        { grip, path("/user/hand/right/input/aim") },
    };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = ARRAY_SIZE(bindings), .suggestedBindings = bindings };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    attach();
    begin_session();

    SIM_Sim* sim = oxr_get_sim();
    sim_set_device_pose(sim, SIM_Device::LeftHand, { .position = { -0.2f, 1.0f, -0.3f }, .orientation = zm::QUAT_IDENTITY });
    sim_set_device_pose(sim, SIM_Device::RightHand, { .position = { 0.2f, 1.0f, -0.3f }, .orientation = zm::QUAT_IDENTITY });
    EXPECT_EQ(sync(), XR_SUCCESS);

    const XrSpace      stage       = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrFrameState frame_state = frame();
    const SIM_Device   devices[2]  = { SIM_Device::LeftHand, SIM_Device::RightHand };
    const SIM_HandPose kinds[2]    = { SIM_HandPose::Palm, SIM_HandPose::Aim };
    for (u32 side = 0; side < 2; side++)
    {
        XrSpace                       space = XR_NULL_HANDLE;
        const XrActionSpaceCreateInfo info  = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = grip, .subactionPath = hands[side], .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
        ASSERT_EQ(xr.xrCreateActionSpace(session, &info, &space), XR_SUCCESS);
        XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
        EXPECT_EQ(xr.xrLocateSpace(space, stage, frame_state.predictedDisplayTime, &location), XR_SUCCESS);
        const Pose expected = sim_get_hand_pose(sim, devices[side], kinds[side]);
        EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) << side;
        EXPECT_NEAR(location.pose.position.x, expected.position.x, 1e-5f) << side;
        EXPECT_NEAR(location.pose.position.y, expected.position.y, 1e-5f) << side;
        EXPECT_NEAR(location.pose.position.z, expected.position.z, 1e-5f) << side;
        EXPECT_NEAR(location.pose.orientation.x, expected.orientation.x, 1e-5f) << side;
        EXPECT_NEAR(location.pose.orientation.w, expected.orientation.w, 1e-5f) << side;
        EXPECT_EQ(xr.xrDestroySpace(space), XR_SUCCESS);
    }
}

// Spec 7.4 xrLocateSpace: velocities are those of the space origin, expressed in the base space
TEST_F(OxrActionTest, velocities_account_for_offsets_and_rotating_bases)
{
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_device_pose(sim, SIM_Device::Head, { .position = { 0.0f, 1.5f, 0.0f }, .orientation = zm::QUAT_IDENTITY });
    frame();
    sim_set_device_pose(sim, SIM_Device::Head, { .position = { 0.0f, 1.5f, 0.0f }, .orientation = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.02f) });
    const XrFrameState frame_state = frame();

    const XrSpace ahead_of_view  = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW, { { 0, 0, 0, 1 }, { 0, 0, -1 } });
    const XrSpace stage          = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrSpace ahead_of_stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE, { { 0, 0, 0, 1 }, { 0, 1.5f, -1 } });
    const XrSpace view           = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);

    XrSpaceVelocity velocity = { .type = XR_TYPE_SPACE_VELOCITY };
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION, .next = &velocity };
    ASSERT_EQ(xr.xrLocateSpace(ahead_of_view, stage, frame_state.predictedDisplayTime, &location), XR_SUCCESS);
    ASSERT_TRUE(velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT);
    const Vec3 omega = { velocity.angularVelocity.x, velocity.angularVelocity.y, velocity.angularVelocity.z };
    ASSERT_GT(omega.y, 0.1f);
    const Vec3 lever    = { location.pose.position.x, location.pose.position.y - 1.5f, location.pose.position.z };
    const Vec3 expected = zm::cross(omega, lever);
    EXPECT_NEAR(velocity.linearVelocity.x, expected.x, 1e-3f);
    EXPECT_NEAR(velocity.linearVelocity.z, expected.z, 1e-3f);

    // A fixed point seen from the turning head sweeps the other way: v = -w x r in view coordinates
    ASSERT_EQ(xr.xrLocateSpace(ahead_of_stage, view, frame_state.predictedDisplayTime, &location), XR_SUCCESS);
    const Vec3 r            = { location.pose.position.x, location.pose.position.y, location.pose.position.z };
    const Vec3 omega_view   = { velocity.angularVelocity.x, velocity.angularVelocity.y, velocity.angularVelocity.z };
    const Vec3 sweep        = zm::cross(omega_view, r);
    EXPECT_NEAR(omega_view.y, -omega.y, 1e-3f);
    EXPECT_NEAR(velocity.linearVelocity.x, sweep.x, 1e-3f);
    EXPECT_NEAR(velocity.linearVelocity.z, sweep.z, 1e-3f);
    EXPECT_GT(zm::abs(velocity.linearVelocity.x), 0.05f);
}

// Spec 11.2 xrCreateActionSet: a higher priority set suppresses bindings to the same input
// source (same identifier, any component) in lower priority sets
TEST_F(OxrActionTest, higher_priority_sets_suppress_shared_input_sources)
{
    XrActionSet                 menu     = XR_NULL_HANDLE;
    const XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .actionSetName = "menu", .localizedActionSetName = "Menu", .priority = 1 };
    ASSERT_EQ(xr.xrCreateActionSet(instance, &set_info, &menu), XR_SUCCESS);
    XrAction           pick        = XR_NULL_HANDLE;
    const XrActionCreateInfo action_info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionName = "pick", .actionType = XR_ACTION_TYPE_BOOLEAN_INPUT, .countSubactionPaths = 2, .subactionPaths = hands, .localizedActionName = "Pick" };
    ASSERT_EQ(xr.xrCreateAction(menu, &action_info, &pick), XR_SUCCESS);

    const XrActionSuggestedBinding bindings[] = {
        { trigger, path("/user/hand/left/input/trigger/value") },
        { trigger, path("/user/hand/right/input/trigger/value") },
        { click,   path("/user/hand/left/input/x/click") },
        { pick,    path("/user/hand/left/input/trigger/touch") },
    };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = ARRAY_SIZE(bindings), .suggestedBindings = bindings };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    const XrActionSet                   sets[]      = { set, menu };
    const XrSessionActionSetsAttachInfo attach_info = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 2, .actionSets = sets };
    ASSERT_EQ(xr.xrAttachSessionActionSets(session, &attach_info), XR_SUCCESS);
    begin_session();

    SIM_Sim* sim = oxr_get_sim();
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue, { .value = { 0.8f, 0.0f } });
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerTouch, { .value = { 1.0f, 0.0f } });
    frame();

    const XrActiveActionSet both[]    = { { set, XR_NULL_PATH }, { menu, XR_NULL_PATH } };
    const XrActionsSyncInfo sync_both = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 2, .activeActionSets = both };
    ASSERT_EQ(xr.xrSyncActions(session, &sync_both), XR_SUCCESS);
    EXPECT_FALSE(float_state(trigger, hands[0]).isActive);
    EXPECT_TRUE(float_state(trigger, hands[1]).isActive);
    EXPECT_TRUE(bool_state(click, hands[0]).isActive);
    EXPECT_TRUE(bool_state(pick, hands[0]).isActive);
    EXPECT_TRUE(bool_state(pick, hands[0]).currentState);
    u32                                        count      = 0;
    const XrBoundSourcesForActionEnumerateInfo bound_info = { .type = XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, .action = trigger };
    EXPECT_EQ(xr.xrEnumerateBoundSourcesForAction(session, &bound_info, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 1u);

    // Without the menu set active nothing is suppressed
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(float_state(trigger, hands[0]).isActive);
    EXPECT_FLOAT_EQ(float_state(trigger, hands[0]).currentState, 0.8f);
}

TEST_F(OxrActionTest, spaces_are_located_at_the_requested_time)
{
    suggest_touch_bindings();
    attach();
    begin_session();
    EXPECT_EQ(sync(), XR_SUCCESS);
    SIM_Sim* sim = oxr_get_sim();

    XrSpace                       hand = XR_NULL_HANDLE;
    const XrActionSpaceCreateInfo info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = grip, .subactionPath = hands[0], .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    ASSERT_EQ(xr.xrCreateActionSpace(session, &info, &hand), XR_SUCCESS);
    const XrSpace stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);

    sim_set_device_pose(sim, SIM_Device::LeftHand, { .position = { 0.0f, 1.0f, 0.0f }, .orientation = zm::QUAT_IDENTITY });
    const XrFrameState first = frame();
    sim_set_device_pose(sim, SIM_Device::LeftHand, { .position = { 0.1f, 1.0f, 0.0f }, .orientation = zm::QUAT_IDENTITY });
    const XrFrameState second = frame();
    EXPECT_EQ(sync(), XR_SUCCESS);

    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(hand, stage, second.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_NEAR(location.pose.position.x, 0.1f, 1e-5f);
    ASSERT_EQ(xr.xrLocateSpace(hand, stage, first.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_NEAR(location.pose.position.x, 0.0f, 1e-5f);
    ASSERT_EQ(xr.xrLocateSpace(hand, stage, (first.predictedDisplayTime + second.predictedDisplayTime) / 2, &location), XR_SUCCESS);
    EXPECT_NEAR(location.pose.position.x, 0.05f, 1e-4f);

    XrView           views[2]    = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
    XrViewState      view_state  = { .type = XR_TYPE_VIEW_STATE };
    u32              views_count = 0;
    XrViewLocateInfo locate_info = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = first.predictedDisplayTime, .space = stage };
    sim_set_device_pose(sim, SIM_Device::Head, { .position = { 0.0f, 2.0f, 0.0f }, .orientation = zm::QUAT_IDENTITY });
    ASSERT_EQ(xr.xrLocateViews(session, &locate_info, &view_state, 2, &views_count, views), XR_SUCCESS);
    EXPECT_NE(views[0].pose.position.y, 2.0f);
    locate_info.displayTime = second.predictedDisplayTime;
    ASSERT_EQ(xr.xrLocateViews(session, &locate_info, &view_state, 2, &views_count, views), XR_SUCCESS);
    EXPECT_EQ(views[0].pose.position.y, 2.0f);
}

TEST_F(OxrActionTest, recentering_queues_a_reference_space_change)
{
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    const XrSpace local = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    const XrSpace stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrFrameState before = frame();
    drain_events();
    XrSpaceLocation old_location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(local, stage, before.predictedDisplayTime, &old_location), XR_SUCCESS);

    sim_set_device_pose(sim, SIM_Device::Head, { .position = { 1.0f, 1.6f, -2.0f }, .orientation = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.5f) });
    sim_recenter(sim);
    const XrFrameState after = frame();

    XrEventDataBuffer buffer = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    u32               found  = 0;
    while (xr.xrPollEvent(instance, &buffer) == XR_SUCCESS)
    {
        if (buffer.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
        {
            const XrEventDataReferenceSpaceChangePending* change = (const XrEventDataReferenceSpaceChangePending*)&buffer;
            EXPECT_EQ(change->session, session);
            EXPECT_EQ(change->referenceSpaceType, found == 0 ? XR_REFERENCE_SPACE_TYPE_LOCAL : XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR);
            EXPECT_GT(change->changeTime, before.predictedDisplayTime);
            EXPECT_LE(change->changeTime, after.predictedDisplayTime);
            EXPECT_TRUE(change->poseValid);
            if (found == 0)
            {
                EXPECT_NEAR(change->poseInPreviousSpace.position.x, 1.0f - old_location.pose.position.x, 1e-4f);
            }
            found++;
        }
        buffer = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
    EXPECT_EQ(found, 2u);

    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(local, stage, before.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_EQ(location.pose.position.x, old_location.pose.position.x);
    ASSERT_EQ(xr.xrLocateSpace(local, stage, after.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_NEAR(location.pose.position.x, 1.0f, 1e-5f);
}

TEST_F(OxrActionTest, haptics)
{
    suggest_touch_bindings();
    attach();
    begin_session();
    EXPECT_EQ(sync(), XR_SUCCESS);

    const XrHapticVibration vibration = { .type = XR_TYPE_HAPTIC_VIBRATION, .duration = 50000000, .frequency = XR_FREQUENCY_UNSPECIFIED, .amplitude = 0.7f };
    const XrHapticActionInfo info = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hands[0] };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&vibration), XR_SUCCESS);

    const SIM_Sim* sim = oxr_get_sim();
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).amplitude, 0.7f);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active);

    EXPECT_EQ(xr.xrStopHapticFeedback(session, &info), XR_SUCCESS);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);

    const XrHapticActionInfo wrong = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = trigger, .subactionPath = hands[0] };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &wrong, (const XrHapticBaseHeader*)&vibration), XR_ERROR_ACTION_TYPE_MISMATCH);
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
}

//
// Vulkan sessions
//

namespace
{

struct OxrVulkanTest : OxrTest
{
    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR           create_vk_instance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR       get_vk_device = nullptr;
    PFN_xrCreateVulkanDeviceKHR             create_vk_device = nullptr;
    VkInstance                              vk_instance = VK_NULL_HANDLE;
    TestValidation                          validation = {};
    VkPhysicalDevice                        vk_physical = VK_NULL_HANDLE;
    VkDevice                                vk_device = VK_NULL_HANDLE;
    u32                                     queue_family = 0;
    VkQueue                                 queue = VK_NULL_HANDLE;
    VkCommandPool                           pool = VK_NULL_HANDLE;
    const char*                             extensions[12] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME }; // derived fixtures append in their constructor
    u32                                     extensions_count = 1;

    void SetUp () override
    {
        OxrTest::SetUp();
        if (volkInitialize() != VK_SUCCESS)
        {
            GTEST_SKIP() << "no Vulkan loader";
        }
        ASSERT_EQ(create_instance(extensions, extensions_count), XR_SUCCESS);
        const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
        ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);

        ASSERT_EQ(gipa(instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction*)&get_requirements), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction*)&create_vk_instance), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction*)&get_vk_device), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction*)&create_vk_device), XR_SUCCESS);
        PFN_xrVoidFunction legacy = nullptr;
        EXPECT_EQ(gipa(instance, "xrGetVulkanGraphicsDeviceKHR", &legacy), XR_ERROR_FUNCTION_UNSUPPORTED);
    }

    void TearDown () override
    {
        if (session != XR_NULL_HANDLE)
        {
            xr.xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }
        if (pool)
        {
            vkDestroyCommandPool(vk_device, pool, nullptr);
        }
        if (vk_device)
        {
            vkDestroyDevice(vk_device, nullptr);
        }
        if (vk_instance)
        {
            test_validation_detach(validation, vk_instance);
            vkDestroyInstance(vk_instance, nullptr);
        }
        OxrTest::TearDown();
        EXPECT_EQ(test_validation_errors, 0u);
    }

    void create_vulkan ()
    {
        XrGraphicsRequirementsVulkan2KHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
        ASSERT_EQ(get_requirements(instance, system, &requirements), XR_SUCCESS);
        EXPECT_GE(requirements.maxApiVersionSupported, requirements.minApiVersionSupported);
        // Regression: the CTS and older apps use Vulkan 1.0; puppetxr used to require 1.1
        EXPECT_LE(requirements.minApiVersionSupported, XR_MAKE_VERSION(1, 0, 0));

        // Vulkan 1.0 on purpose, so the validation layer reports any 1.1 dependency in the runtime
        const VkApplicationInfo    app_info      = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "puppetxr_tests", .apiVersion = VK_API_VERSION_1_0 };
        VkInstanceCreateInfo       instance_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app_info };
        test_validation_prepare(validation, instance_info, nullptr, 0);
        const XrVulkanInstanceCreateInfoKHR xr_instance_info = { .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = system, .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = &instance_info };
        VkResult vk_result = VK_ERROR_UNKNOWN;
        ASSERT_EQ(create_vk_instance(instance, &xr_instance_info, &vk_instance, &vk_result), XR_SUCCESS);
        if (vk_result != VK_SUCCESS)
        {
            GTEST_SKIP() << "vkCreateInstance failed";
        }
        volkLoadInstanceOnly(vk_instance);
        test_validation_attach(validation, vk_instance);

        const XrVulkanGraphicsDeviceGetInfoKHR get_info = { .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = system, .vulkanInstance = vk_instance };
        ASSERT_EQ(get_vk_device(instance, &get_info, &vk_physical), XR_SUCCESS);
        ASSERT_NE(vk_physical, VK_NULL_HANDLE);

        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(vk_physical, &family_count, nullptr);
        VkQueueFamilyProperties families[16] = {};
        family_count = family_count > 16 ? 16 : family_count;
        vkGetPhysicalDeviceQueueFamilyProperties(vk_physical, &family_count, families);
        queue_family = 0;
        while (queue_family < family_count && !(families[queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            queue_family++;
        }
        ASSERT_LT(queue_family, family_count);

        const f32 priority = 1.0f;
        const VkDeviceQueueCreateInfo queue_info  = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = queue_family, .queueCount = 1, .pQueuePriorities = &priority };
        const VkDeviceCreateInfo      device_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info };
        const XrVulkanDeviceCreateInfoKHR xr_device_info = { .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = system, .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanPhysicalDevice = vk_physical, .vulkanCreateInfo = &device_info };
        ASSERT_EQ(create_vk_device(instance, &xr_device_info, &vk_device, &vk_result), XR_SUCCESS);
        ASSERT_EQ(vk_result, VK_SUCCESS);
        volkLoadDevice(vk_device);
        vkGetDeviceQueue(vk_device, queue_family, 0, &queue);

        const VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = queue_family };
        ASSERT_EQ(vkCreateCommandPool(vk_device, &pool_info, nullptr, &pool), VK_SUCCESS);
    }

    void create_vulkan_session ()
    {
        const XrGraphicsBindingVulkan2KHR binding = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = vk_instance, .physicalDevice = vk_physical, .device = vk_device, .queueFamilyIndex = queue_family, .queueIndex = 0 };
        const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = system };
        ASSERT_EQ(xr.xrCreateSession(instance, &info, &session), XR_SUCCESS);
    }

    XrSwapchain create_swapchain (u32 width, u32 height, u32 array_size, i64 format = VK_FORMAT_R8G8B8A8_SRGB)
    {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        const XrSwapchainCreateInfo info = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, .format = format, .sampleCount = 1, .width = width, .height = height, .faceCount = 1, .arraySize = array_size, .mipCount = 1 };
        EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_SUCCESS);
        return swapchain;
    }

    b8 format_offered (i64 format)
    {
        u32     count       = 0;
        int64_t formats[16] = {};
        EXPECT_EQ(xr.xrEnumerateSwapchainFormats(session, 16, &count, formats), XR_SUCCESS);
        for (u32 i = 0; i < count; i++)
        {
            if (formats[i] == format)
            {
                return true;
            }
        }
        return false;
    }

    // Acquires, clears every layer of the image to `color` (as the app would render) and releases
    void render_into (XrSwapchain swapchain, VkClearColorValue color)
    {
        u32 count = 0;
        EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr), XR_SUCCESS);
        XrSwapchainImageVulkan2KHR images[8] = {};
        for (u32 i = 0; i < 8; i++)
        {
            images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        }
        ASSERT_LE(count, 8u);
        EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, count, &count, (XrSwapchainImageBaseHeader*)images), XR_SUCCESS);

        u32 index = 0;
        EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index), XR_SUCCESS);
        const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);

        const VkCommandBufferAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        ASSERT_EQ(vkAllocateCommandBuffers(vk_device, &alloc_info, &cmd), VK_SUCCESS);
        const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cmd, &begin_info);

        const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
        VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[index].image, .subresourceRange = range };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkCmdClearColorImage(cmd, images[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(cmd);

        const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        ASSERT_EQ(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), VK_SUCCESS);
        vkQueueWaitIdle(queue);
        vkFreeCommandBuffers(vk_device, pool, 1, &cmd);

        EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
    }
};

} // namespace

TEST_F(OxrVulkanTest, session_needs_requirements_call_and_matching_device)
{
    create_vulkan();
    const XrGraphicsBindingVulkan2KHR binding = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = vk_instance, .physicalDevice = (VkPhysicalDevice)(uintptr_t)0x1234, .device = vk_device, .queueFamilyIndex = queue_family };
    XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = system };
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    EXPECT_EQ(session, XR_NULL_HANDLE);

    const XrGraphicsBindingVulkan2KHR bad_family = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = vk_instance, .physicalDevice = vk_physical, .device = vk_device, .queueFamilyIndex = 999 };
    info.next = &bad_family;
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_VALIDATION_FAILURE);

    create_vulkan_session();
    ASSERT_NE(session, XR_NULL_HANDLE);
    u32 count = 0;
    EXPECT_EQ(xr.xrEnumerateSwapchainFormats(session, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_GT(count, 0u);
    int64_t formats[16] = {};
    ASSERT_LE(count, 16u);
    EXPECT_EQ(xr.xrEnumerateSwapchainFormats(session, 16, &count, formats), XR_SUCCESS);
    b8 has_srgb = false;
    for (u32 i = 0; i < count; i++)
    {
        has_srgb |= formats[i] == VK_FORMAT_R8G8B8A8_SRGB;
    }
    EXPECT_TRUE(has_srgb);
}

TEST_F(OxrVulkanTest, requirements_missing_is_reported)
{
    const XrGraphicsRequirementsVulkan2KHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    UNUSED(requirements);
    const XrGraphicsBindingVulkan2KHR binding = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = (VkInstance)(uintptr_t)1, .physicalDevice = (VkPhysicalDevice)(uintptr_t)1, .device = (VkDevice)(uintptr_t)1, .queueFamilyIndex = 0 };
    const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = system };
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
}

TEST_F(OxrVulkanTest, swapchain_lifecycle_and_ordering)
{
    create_vulkan();
    create_vulkan_session();

    XrSwapchain bad = XR_NULL_HANDLE;
    XrSwapchainCreateInfo info = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, .format = VK_FORMAT_R5G6B5_UNORM_PACK16, .sampleCount = 1, .width = 32, .height = 32, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
    EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &bad), XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
    info.format = VK_FORMAT_R8G8B8A8_SRGB;
    info.faceCount = 2;
    EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &bad), XR_ERROR_VALIDATION_FAILURE);
    info.faceCount = 1;
    info.width = 0;
    EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &bad), XR_ERROR_VALIDATION_FAILURE);
    info.width = 32;
    info.usageFlags = 0x80000000;
    EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &bad), XR_ERROR_VALIDATION_FAILURE);

    const XrSwapchain swapchain = create_swapchain(32, 32, 2);
    ASSERT_NE(swapchain, XR_NULL_HANDLE);

    u32 count = 0;
    EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 3u);
    XrSwapchainImageVulkan2KHR images[3] = { { .type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR }, { .type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR }, { .type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR } };
    EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, 3, &count, (XrSwapchainImageBaseHeader*)images), XR_SUCCESS);
    EXPECT_NE(images[0].image, VK_NULL_HANDLE);
    EXPECT_NE(images[0].image, images[1].image);

    const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 0 };
    EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_ERROR_CALL_ORDER_INVALID);

    u32 index[4] = {};
    EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index[0]), XR_SUCCESS);
    EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index[1]), XR_SUCCESS);
    EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index[2]), XR_SUCCESS);
    EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index[3]), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_NE(index[0], index[1]);
    EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);
    // Regression, spec 10.1: a waited image must be released before the next wait
    EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
    EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);
    EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
    EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);
    EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
    EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index[3]), XR_SUCCESS);
    EXPECT_EQ(index[3], index[0]);

    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_ERROR_HANDLE_INVALID);
}

// Spec 10.1 XrSwapchainCreateInfo: array size and mip count are only bounded by the graphics API
// The formats offered are the ones the device can make swapchain images of: AMD's Vulkan drivers
// have no D24_UNORM_S8_UINT, which the API knows
TEST_F(OxrVulkanTest, offered_formats_are_ones_the_device_supports)
{
    create_vulkan();
    create_vulkan_session();
    u32 count = 0;
    ASSERT_EQ(xr.xrEnumerateSwapchainFormats(session, 0, &count, nullptr), XR_SUCCESS);
    int64_t formats[16] = {};
    ASSERT_LE(count, 16u);
    ASSERT_EQ(xr.xrEnumerateSwapchainFormats(session, 16, &count, formats), XR_SUCCESS);
    for (u32 i = 0; i < count; i++)
    {
        const b8 depth = formats[i] == VK_FORMAT_D32_SFLOAT || formats[i] == VK_FORMAT_D16_UNORM || formats[i] == VK_FORMAT_D24_UNORM_S8_UINT || formats[i] == VK_FORMAT_D32_SFLOAT_S8_UINT;
        XrSwapchain                 swapchain = XR_NULL_HANDLE;
        const XrSwapchainCreateInfo info      = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = depth ? (XrSwapchainUsageFlags)XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : (XrSwapchainUsageFlags)XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, .format = formats[i], .sampleCount = 1, .width = 16, .height = 16, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
        EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_SUCCESS) << formats[i];
        xr.xrDestroySwapchain(swapchain);
    }

    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(vk_physical, VK_FORMAT_D24_UNORM_S8_UINT, &properties);
    const b8 has_d24 = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    b8       listed  = false;
    for (u32 i = 0; i < count; i++)
    {
        listed |= formats[i] == VK_FORMAT_D24_UNORM_S8_UINT;
    }
    EXPECT_EQ(listed, has_d24);
    if (!has_d24)
    {
        XrSwapchain                 swapchain = XR_NULL_HANDLE;
        const XrSwapchainCreateInfo info      = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .format = VK_FORMAT_D24_UNORM_S8_UINT, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
        EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
    }
}

TEST_F(OxrVulkanTest, swapchains_take_mip_chains_and_large_arrays)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();

    XrSwapchain           swapchain = XR_NULL_HANDLE;
    XrSwapchainCreateInfo info      = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, .format = VK_FORMAT_R8G8B8A8_SRGB, .sampleCount = 1, .width = 64, .height = 64, .faceCount = 1, .arraySize = 6, .mipCount = 7 };
    ASSERT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_SUCCESS);
    render_into(swapchain, { { 0.0f, 1.0f, 0.0f, 1.0f } });

    const XrSpace          view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    XrCompositionLayerQuad quad       = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 64, 64 } }, .imageArrayIndex = 5 }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 0.5f, 0.5f } };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[1], 255);
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);

    info.mipCount = 8;
    EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_ERROR_VALIDATION_FAILURE);
}

// Unity's OpenXR plugin can switch its app space within a frame and submit the frame with the
// space it just destroyed; on that path every later frame failed and the app never showed again
TEST_F(OxrVulkanTest, layers_of_a_frame_may_use_a_space_destroyed_during_it)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();

    const XrSwapchain swapchain = create_swapchain(64, 64, 2);
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    const XrSpace local = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    const XrSpace stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);

    XrFrameState           state      = { .type = XR_TYPE_FRAME_STATE };
    const XrFrameWaitInfo  wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
    const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    XrCompositionLayerProjectionView    views[2]  = { view(swapchain, 0, 64, 64), view(swapchain, 1, 64, 64) };
    const XrCompositionLayerProjection  layer     = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = stage, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (const XrCompositionLayerBaseHeader*)&layer };
    XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = layers };

    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    ASSERT_EQ(xr.xrDestroySpace(stage), XR_SUCCESS);
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    EXPECT_EQ(xr.xrLocateSpace(stage, local, state.predictedDisplayTime, &location), XR_ERROR_HANDLE_INVALID);
    end_info.displayTime = state.predictedDisplayTime;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);

    u32 width  = 0;
    u32 height = 0;
    u8  probe[4];
    ASSERT_FALSE(oxr_read_frame(probe, sizeof(probe), width, height));
    const usize size   = (usize)width * height * 4;
    u8*         pixels = (u8*)os_memory_alloc(size);
    defer { os_memory_release(pixels); };
    ASSERT_TRUE(oxr_read_frame(pixels, size, width, height));
    EXPECT_EQ(pixels[((usize)(height / 2) * width + width / 4) * 4], 255);

    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    end_info.displayTime = state.predictedDisplayTime;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrVulkanTest, projection_layers_are_composited_and_screenshot_written)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();

    const XrSwapchain swapchain = create_swapchain(64, 64, 2);
    const XrSpace     stage     = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);

    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    const XrFrameWaitInfo  wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
    const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    EXPECT_TRUE(state.shouldRender);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);

    XrCompositionLayerProjectionView views[2] = { view(swapchain, 0, 64, 64), view(swapchain, 1, 64, 64) };
    XrCompositionLayerProjection layer = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = stage, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
    XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = layers };

    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_LAYER_INVALID); // never released
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });

    views[0].subImage.imageRect.extent.width = 65;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_SWAPCHAIN_RECT_INVALID);
    views[0].subImage.imageRect.extent.width = 64;
    views[0].subImage.imageArrayIndex = 2;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_VALIDATION_FAILURE);
    views[0].subImage.imageArrayIndex = 0;
    views[0].pose.orientation = { 0, 0, 0, 0 };
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_POSE_INVALID);
    views[0].pose.orientation = { 0, 0, 0, 1 };
    layer.viewCount = 1;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_VALIDATION_FAILURE);
    layer.viewCount = 2;
    views[1].subImage.swapchain = (XrSwapchain)(uintptr_t)0x77;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_HANDLE_INVALID);
    views[1].subImage.swapchain = swapchain;
    layers[0] = nullptr;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_LAYER_INVALID);
    layers[0] = (XrCompositionLayerBaseHeader*)&layer;

    oxr_set_mirror_view(SIM_MirrorView::Both);
    ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);

    u32 width = 0;
    u32 height = 0;
    u8  probe[4];
    ASSERT_FALSE(oxr_read_frame(probe, sizeof(probe), width, height));
    ASSERT_GT(width, 0u);
    const usize size   = (usize)width * height * 4;
    u8*         pixels = (u8*)os_memory_alloc(size);
    defer { os_memory_release(pixels); };
    ASSERT_TRUE(oxr_read_frame(pixels, size, width, height));

    const u8* left  = pixels + ((usize)(height / 2) * width + width / 4) * 4;
    const u8* right = pixels + ((usize)(height / 2) * width + width * 3 / 4) * 4;
    EXPECT_EQ(left[0], 255);
    EXPECT_EQ(left[1], 0);
    EXPECT_EQ(left[2], 0);
    EXPECT_EQ(right[0], 255);
    const u8* corner = pixels;
    EXPECT_LT(corner[0], 100);
    EXPECT_EQ(corner[0], corner[1]);
    // The view's fov (tan 0.84) covers the middle of the 90 degree eye, whatever its pixel size
    const u32 eye_width = width / 2;
    const u8* inside    = pixels + ((usize)(height / 2) * width + eye_width / 5) * 4;
    const u8* outside   = pixels + ((usize)(height / 2) * width + eye_width / 32) * 4;
    EXPECT_EQ(inside[0], 255);
    EXPECT_EQ(outside[0], corner[0]);

    // Left-eye mode fills the single slot with view 0 only
    oxr_set_mirror_view(SIM_MirrorView::Left);
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    end_info.displayTime = state.predictedDisplayTime;
    ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    u32 left_width = 0;
    u32 left_height = 0;
    ASSERT_TRUE(oxr_read_frame(pixels, size, left_width, left_height));
    EXPECT_EQ(left_width * 2, width);
    EXPECT_EQ(pixels[((usize)(left_height / 2) * left_width + left_width / 2) * 4], 255);
    EXPECT_EQ(pixels[((usize)(left_height / 2) * left_width + left_width / 4) * 4], 255);
    EXPECT_EQ(pixels[((usize)(left_height / 2) * left_width + left_width / 32) * 4], corner[0]);
    oxr_set_mirror_view(SIM_MirrorView::Both);

    // puppetxrctl's screenshot asks for the size alone; that is no failed readback
    u32 probe_width  = 0;
    u32 probe_height = 0;
    test_gl_errors   = 0;
    os_set_log_callback(test_gl_log_callback, nullptr);
    EXPECT_TRUE(oxr_read_frame(probe, 0, probe_width, probe_height));
    EXPECT_TRUE(oxr_read_mirror_frame(probe, 0, probe_width, probe_height));
    os_set_log_callback(nullptr, nullptr);
    EXPECT_GT(probe_width, 0u);
    EXPECT_EQ(test_gl_errors, 0u);

    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_test_screenshot.png", temp_dir());
    EXPECT_TRUE(oxr_save_screenshot(path));
    EXPECT_TRUE(os_file_exists(path));
    u8*   png      = nullptr;
    usize png_size = 0;
    ASSERT_TRUE(os_read_file(path, &png, &png_size));
    EXPECT_GT(png_size, 8u);
    EXPECT_EQ(png[1], 'P');
    EXPECT_EQ(png[2], 'N');
    EXPECT_EQ(png[3], 'G');
    os_memory_release(png);

    // a second frame with a different colour replaces the first
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    render_into(swapchain, { { 0.0f, 0.0f, 1.0f, 1.0f } });
    end_info.displayTime = state.predictedDisplayTime;
    ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    ASSERT_TRUE(oxr_read_frame(pixels, size, width, height));
    EXPECT_EQ(left[0], 0);
    EXPECT_EQ(left[2], 255);

    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

static u32 log_lines_with (const char* text)
{
    u32 count = 0;
    for (u32 i = 0; i < os_log_history_count(); i++)
    {
        count += cstr_find(os_log_history(i)->text, text) != nullptr ? 1u : 0u;
    }
    return count;
}

// Startup time is the engine's or ours: the first frame with layers says which, once a run
TEST_F(OxrVulkanTest, startup_is_logged_at_the_first_frame_with_layers)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const u32 before = log_lines_with("Startup:");
    frame();
    EXPECT_EQ(log_lines_with("Startup:"), before) << "a frame without layers shows nothing yet";
    const XrSwapchain swapchain = create_swapchain(32, 32, 1);
    const XrSpace     view      = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    XrCompositionLayerQuad quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 1.0f, 1.0f } };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    submit_and_read(layers, 1, pixels, width, height);
    const u32 after = log_lines_with("Startup:");
    EXPECT_LE(after, before + 1) << "once a run";
    if (before == 0)
    {
        EXPECT_EQ(after, 1u);
        EXPECT_EQ(log_lines_with("puppetxr's calls took"), 1u);
    }
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

// The lens hides the corners of the display: the composite shows them black, as the headset's
// wearer sees them, over whatever the app drew there. 0 for mask_radius has no lens mask.
static void expect_lens_mask (OxrVulkanTest& test, b8 masked)
{
    test.create_vulkan();
    test.create_vulkan_session();
    test.begin_session();
    const XrSwapchain swapchain = test.create_swapchain(32, 32, 1);
    const XrSpace     view      = test.reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    test.render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    XrCompositionLayerQuad quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 10.0f, 10.0f } };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    test.submit_and_read(layers, 1, pixels, width, height);
    for (u32 eye = 0; eye < 2; eye++)
    {
        EXPECT_EQ(OxrVulkanTest::eye_pixel(pixels, width, height, eye, 0.0f, 0.0f)[0], 255) << eye;
        EXPECT_EQ(OxrVulkanTest::eye_pixel(pixels, width, height, eye, 0.98f, 0.0f)[0], 255) << eye;
        EXPECT_EQ(OxrVulkanTest::eye_pixel(pixels, width, height, eye, 0.0f, -0.98f)[0], 255) << eye;
        for (const f32 corner : { -0.97f, 0.97f })
        {
            EXPECT_EQ(OxrVulkanTest::eye_pixel(pixels, width, height, eye, corner, corner)[0], masked ? 0 : 255) << eye;
            EXPECT_EQ(OxrVulkanTest::eye_pixel(pixels, width, height, eye, corner, -corner)[0], masked ? 0 : 255) << eye;
        }
        EXPECT_EQ(OxrVulkanTest::eye_pixel(pixels, width, height, eye, 0.97f, 0.97f)[3], 255) << eye;
    }
    EXPECT_EQ(test.xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

namespace
{
struct OxrLensMaskTest : OxrVulkanTest
{
    void SetUp () override
    {
        os_set_env("PUPPETXR_LENS_MASK", "1");
        OxrVulkanTest::SetUp();
    }

    void TearDown () override
    {
        OxrVulkanTest::TearDown();
        os_unset_env("PUPPETXR_LENS_MASK");
    }
};
} // namespace

TEST_F(OxrLensMaskTest, lens_mask_is_black)
{
    expect_lens_mask(*this, true);
}

TEST_F(OxrVulkanTest, no_lens_mask_by_default)
{
    expect_lens_mask(*this, false);
}

namespace
{
struct OxrNoLensMaskTest : OxrVulkanTest
{
    char config[OS_MAX_PATH] = {};

    void SetUp () override
    {
        cstr_format(config, sizeof(config), "%s/puppetxr_no_lens_mask.json", temp_dir());
        ASSERT_TRUE(os_write_file(config, "{ \"views\": { \"mask_radius\": 0 } }", 33));
        os_set_env("PUPPETXR_CONFIG", config);
        os_set_env("PUPPETXR_LENS_MASK", "1");
        OxrVulkanTest::SetUp();
    }

    void TearDown () override
    {
        OxrVulkanTest::TearDown();
        os_unset_env("PUPPETXR_CONFIG");
        os_unset_env("PUPPETXR_LENS_MASK");
        os_delete_file(config);
    }
};
} // namespace

TEST_F(OxrNoLensMaskTest, no_lens_mask_without_a_radius)
{
    expect_lens_mask(*this, false);
}

// Spec 10.6.3: quads are placed in their space and only their front (+Z) face is drawn
TEST_F(OxrVulkanTest, quad_layers_are_placed_in_space)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain = create_swapchain(32, 32, 1);
    const XrSpace     view      = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });

    XrCompositionLayerQuad quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 0.5f, 0.5f } };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width = 0;
    u32 height = 0;

    submit_and_read(layers, 1, pixels, width, height);
    for (u32 eye = 0; eye < 2; eye++)
    {
        EXPECT_EQ(eye_pixel(pixels, width, height, eye, 0.0f, 0.0f)[0], 255) << eye;
        EXPECT_EQ(eye_pixel(pixels, width, height, eye, 0.0f, 0.0f)[1], 0) << eye;
        EXPECT_EQ(eye_pixel(pixels, width, height, eye, 0.6f, 0.0f)[0], 0) << eye;
        EXPECT_EQ(eye_pixel(pixels, width, height, eye, 0.0f, 0.6f)[0], 0) << eye;
    }

    // Half a metre wide at one metre spans tan = 0.25 of the 90 degree eye
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.2f, 0.2f)[0], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.3f)[0], 0);

    quad.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[0], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 1, 0.0f, 0.0f)[0], 0);

    quad.eyeVisibility    = XR_EYE_VISIBILITY_BOTH;
    quad.pose.orientation = { 0.0f, 1.0f, 0.0f, 0.0f };
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[0], 0);
    EXPECT_EQ(eye_pixel(pixels, width, height, 1, 0.0f, 0.0f)[0], 0);

    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

// Spec 10.6.2: alpha is one unless the layer asks for source alpha; unpremultiplied colors
// are multiplied by alpha before blending over the layers below
TEST_F(OxrVulkanTest, layer_alpha_follows_the_layer_flags)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain background = create_swapchain(64, 64, 2);
    const XrSwapchain overlay    = create_swapchain(32, 32, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(background, { { 0.0f, 0.0f, 1.0f, 1.0f } });
    render_into(overlay, { { 1.0f, 0.0f, 0.0f, 0.5f } });

    XrCompositionLayerProjectionView views[2]   = { view(background, 0, 64, 64), view(background, 1, 64, 64) };
    XrCompositionLayerProjection     projection = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = view_space, .viewCount = 2, .views = views };
    XrCompositionLayerQuad           quad       = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = overlay, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 0.5f, 0.5f } };
    const XrCompositionLayerBaseHeader* layers[2] = { (XrCompositionLayerBaseHeader*)&projection, (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width = 0;
    u32 height = 0;

    submit_and_read(layers, 2, pixels, width, height);
    const u8* center = eye_pixel(pixels, width, height, 0, 0.0f, 0.0f);
    EXPECT_EQ(center[0], 255);
    EXPECT_EQ(center[2], 0);

    // Unpremultiplied red at half alpha over blue: 0.5 linear in each channel, 188 in sRGB
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    submit_and_read(layers, 2, pixels, width, height);
    center = eye_pixel(pixels, width, height, 0, 0.0f, 0.0f);
    EXPECT_NEAR(center[0], 188, 2);
    EXPECT_NEAR(center[2], 188, 2);

    // Premultiplied: the red is taken as is and blue keeps half its weight
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    submit_and_read(layers, 2, pixels, width, height);
    center = eye_pixel(pixels, width, height, 0, 0.0f, 0.0f);
    EXPECT_EQ(center[0], 255);
    EXPECT_NEAR(center[2], 188, 2);

    EXPECT_EQ(xr.xrDestroySwapchain(overlay), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(background), XR_SUCCESS);
}

// A view rendered from another orientation than the display's is reprojected
TEST_F(OxrVulkanTest, projection_views_are_reprojected_to_the_display_pose)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain  = create_swapchain(64, 64, 2);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });

    XrCompositionLayerProjectionView views[2] = { view(swapchain, 0, 64, 64), view(swapchain, 1, 64, 64) };
    const Quat yaw = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.6f);
    for (XrCompositionLayerProjectionView& v : views)
    {
        v.pose.orientation = { yaw.x, yaw.y, yaw.z, yaw.w };
    }
    XrCompositionLayerProjection        layer     = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = view_space, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width = 0;
    u32 height = 0;

    // Rendered looking 0.6 rad to the left, the image spans -1.3 to 0.1 rad: the right of the eye is empty
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, -0.5f, 0.0f)[0], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.05f, 0.0f)[0], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.2f, 0.0f)[0], 0);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.5f, 0.0f)[0], 0);

    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

// Regression, CTS ProjectionMutableFieldOfView: a flipped fov (left > right, down > up) is a
// mirrored image of the same region, not an invalid one
TEST_F(OxrVulkanTest, projection_views_with_flipped_fov_are_drawn_mirrored)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain  = create_swapchain(64, 64, 2);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });

    XrCompositionLayerProjectionView views[2] = { view(swapchain, 0, 64, 64), view(swapchain, 1, 64, 64) };
    for (XrCompositionLayerProjectionView& v : views)
    {
        v.fov = { .angleLeft = 0.2f, .angleRight = -0.6f, .angleUp = -0.3f, .angleDown = 0.3f };
    }
    XrCompositionLayerProjection        layer     = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = view_space, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, -0.5f, 0.0f)[0], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.1f, 0.0f)[0], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.4f, 0.0f)[0], 0);
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

//
// Simulated controller types
//

static u32 count_profile_events (const OxrTest& t)
{
    u32               events = 0;
    XrEventDataBuffer event  = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (t.xr.xrPollEvent(t.instance, &event) == XR_SUCCESS)
    {
        events += event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
        event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
    return events;
}

// Spec 2.22.1: an overflowing queue reports how many events were dropped where they were dropped
TEST_F(OxrActionTest, overflowing_event_queue_reports_lost_events)
{
    suggest_touch_bindings();
    const XrActionSuggestedBinding             simple_bindings[] = { { click, path("/user/hand/left/input/select/click") } };
    const XrInteractionProfileSuggestedBinding simple            = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/khr/simple_controller"), .countSuggestedBindings = 1, .suggestedBindings = simple_bindings };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &simple), XR_SUCCESS);
    attach();
    begin_session();
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    drain_events();

    SIM_Sim*  sim     = oxr_get_sim();
    const u32 changes = 300;
    for (u32 i = 0; i < changes; i++)
    {
        sim_set_controller(sim, i % 2 == 0 ? SIM_Controller::KhrSimple : SIM_Controller::OculusTouch);
        ASSERT_EQ(sync(), XR_SUCCESS);
    }

    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    ASSERT_EQ(xr.xrPollEvent(instance, &event), XR_SUCCESS);
    ASSERT_EQ(event.type, XR_TYPE_EVENT_DATA_EVENTS_LOST);
    const u32 lost = ((XrEventDataEventsLost*)&event)->lostEventCount;
    EXPECT_GT(lost, 0u);
    u32 delivered = 0;
    event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (xr.xrPollEvent(instance, &event) == XR_SUCCESS)
    {
        EXPECT_EQ(event.type, XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED);
        delivered++;
        event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
    EXPECT_EQ(lost + delivered, changes);
}

TEST_F(OxrActionTest, controller_type_selects_the_interaction_profile)
{
    // The app supports both profiles, like most do
    suggest_touch_bindings();
    const XrActionSuggestedBinding simple_bindings[] = {
        { click, path("/user/hand/left/input/select/click") },
        { click, path("/user/hand/right/input/select/click") },
        { grip,  path("/user/hand/left/input/grip/pose") },
        { grip,  path("/user/hand/right/input/grip/pose") },
    };
    const XrInteractionProfileSuggestedBinding simple = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/khr/simple_controller"), .countSuggestedBindings = ARRAY_SIZE(simple_bindings), .suggestedBindings = simple_bindings };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &simple), XR_SUCCESS);
    attach();
    begin_session();
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    count_profile_events(*this);

    XrInteractionProfileState state = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
    ASSERT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[1], &state), XR_SUCCESS);
    EXPECT_EQ(state.interactionProfile, path("/interaction_profiles/oculus/touch_controller"));

    SIM_Sim* sim = oxr_get_sim();
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::AClick, { .value = { 1.0f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(bool_state(click, hands[1]).currentState);

    sim_set_controller(sim, SIM_Controller::KhrSimple);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(count_profile_events(*this), 1u);
    ASSERT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[1], &state), XR_SUCCESS);
    EXPECT_EQ(state.interactionProfile, path("/interaction_profiles/khr/simple_controller"));
    EXPECT_FALSE(bool_state(click, hands[1]).currentState);
    EXPECT_FALSE(float_state(trigger, hands[1]).isActive);

    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::SelectClick, { .value = { 1.0f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(bool_state(click, hands[1]).currentState);
    EXPECT_FALSE(bool_state(click, hands[0]).currentState);
}

TEST_F(OxrActionTest, simple_profile_on_touch_hardware_reads_the_trigger)
{
    const XrActionSuggestedBinding bindings[] = {
        { click, path("/user/hand/right/input/select/click") },
    };
    const XrInteractionProfileSuggestedBinding simple = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/khr/simple_controller"), .countSuggestedBindings = 1, .suggestedBindings = bindings };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &simple), XR_SUCCESS);
    attach();
    begin_session();

    SIM_Sim* sim = oxr_get_sim();
    ASSERT_EQ(sim_get_controller(sim), SIM_Controller::MetaTouchPlus);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 0.9f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(bool_state(click, hands[1]).currentState);

    // Same profile on different hardware: the bindings move to the hardware's own select button
    count_profile_events(*this);
    sim_set_controller(sim, SIM_Controller::KhrSimple);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(count_profile_events(*this), 0u);
    EXPECT_FALSE(bool_state(click, hands[1]).currentState);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::SelectClick, { .value = { 1.0f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(bool_state(click, hands[1]).currentState);
}

TEST_F(OxrActionTest, touch_only_app_has_no_profile_on_simple_hardware)
{
    suggest_touch_bindings();
    attach();
    begin_session();
    sim_set_controller(oxr_get_sim(), SIM_Controller::KhrSimple);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);

    XrInteractionProfileState state = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
    ASSERT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[0], &state), XR_SUCCESS);
    EXPECT_EQ(state.interactionProfile, XR_NULL_PATH);
    EXPECT_FALSE(float_state(trigger, hands[0]).isActive);
    EXPECT_FALSE(bool_state(click, hands[1]).isActive);
}

//
// Meta controllers: XR_FB_touch_controller_pro, XR_META_touch_controller_plus,
// XR_FB_touch_controller_proximity
//

namespace
{
struct OxrMetaControllerTest : OxrActionTest
{
    OxrMetaControllerTest ()
    {
        more_extensions[more_extensions_count++] = XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_FB_TOUCH_CONTROLLER_PROXIMITY_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME;
    }

    XrResult suggest (const char* profile, const XrActionSuggestedBinding* bindings, u32 count)
    {
        const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path(profile), .countSuggestedBindings = count, .suggestedBindings = bindings };
        return xr.xrSuggestInteractionProfileBindings(instance, &suggested);
    }

    XrPath current_profile ()
    {
        XrInteractionProfileState state = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
        EXPECT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[1], &state), XR_SUCCESS);
        return state.interactionProfile;
    }
};
} // namespace

TEST_F(OxrActionTest, meta_profiles_need_their_extensions)
{
    const XrActionSuggestedBinding plus = { trigger, path("/user/hand/right/input/trigger/curl_meta") };
    const XrInteractionProfileSuggestedBinding plus_suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/meta/touch_controller_plus"), .countSuggestedBindings = 1, .suggestedBindings = &plus };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &plus_suggested), XR_ERROR_PATH_UNSUPPORTED);
    const XrActionSuggestedBinding proximity = { click, path("/user/hand/right/input/trigger/proximity_fb") };
    const XrInteractionProfileSuggestedBinding touch_suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = 1, .suggestedBindings = &proximity };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &touch_suggested), XR_ERROR_PATH_UNSUPPORTED);
}

// Quest 3 Touch Plus serves its own profile first, then Touch and simple
TEST_F(OxrMetaControllerTest, touch_plus_reads_curl_force_and_proximity)
{
    const XrAction curl  = make_action("curl", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrAction force = make_action("force", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrAction near  = make_action("near", XR_ACTION_TYPE_BOOLEAN_INPUT);
    const XrActionSuggestedBinding plus[] = {
        { curl,    path("/user/hand/right/input/trigger/curl_meta") },
        { force,   path("/user/hand/right/input/trigger/force") },
        { near,    path("/user/hand/right/input/thumb_meta/proximity_meta") },
        { trigger, path("/user/hand/right/input/trigger/slide_meta") },
    };
    ASSERT_EQ(suggest("/interaction_profiles/meta/touch_controller_plus", plus, ARRAY_SIZE(plus)), XR_SUCCESS);
    suggest_touch_bindings();
    attach();
    begin_session();

    SIM_Sim* sim = oxr_get_sim();
    ASSERT_EQ(sim_get_controller(sim), SIM_Controller::MetaTouchPlus);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerCurl, { .value = { 0.4f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerForce, { .value = { 0.6f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::ThumbProximity, { .value = { 1.0f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerSlide, { .value = { 0.2f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(current_profile(), path("/interaction_profiles/meta/touch_controller_plus"));
    EXPECT_FLOAT_EQ(float_state(curl, hands[1]).currentState, 0.4f);
    EXPECT_FLOAT_EQ(float_state(force, hands[1]).currentState, 0.6f);
    EXPECT_TRUE(bool_state(near, hands[1]).currentState);
    EXPECT_FLOAT_EQ(float_state(trigger, hands[1]).currentState, 0.2f);

    // Touch hardware has no Plus profile, so the app's Touch bindings take over
    sim_set_controller(sim, SIM_Controller::OculusTouch);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(current_profile(), path("/interaction_profiles/oculus/touch_controller"));
    EXPECT_FALSE(float_state(curl, hands[1]).isActive);
}

TEST_F(OxrMetaControllerTest, touch_pro_reads_its_inputs_and_haptic_outputs)
{
    const XrAction curl    = make_action("curl", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrAction stylus  = make_action("stylus", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrAction rest    = make_action("rest", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrAction near    = make_action("near", XR_ACTION_TYPE_BOOLEAN_INPUT);
    const XrAction buzz    = make_action("buzz", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    const XrActionSuggestedBinding pro[] = {
        { curl,   path("/user/hand/left/input/trigger/curl_fb") },
        { stylus, path("/user/hand/left/input/stylus_fb/force") },
        { rest,   path("/user/hand/left/input/thumbrest/force") },
        { near,   path("/user/hand/left/input/trigger/proximity_fb") },
        { buzz,   path("/user/hand/left/output/haptic_trigger_fb") },
        { haptic, path("/user/hand/left/output/haptic") },
    };
    ASSERT_EQ(suggest("/interaction_profiles/facebook/touch_controller_pro", pro, ARRAY_SIZE(pro)), XR_SUCCESS);
    attach();
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_controller(sim, SIM_Controller::MetaTouchPro);
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerCurl, { .value = { 0.3f, 0.0f } });
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::StylusForce, { .value = { 0.7f, 0.0f } });
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::ThumbrestForce, { .value = { 0.9f, 0.0f } });
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerProximity, { .value = { 1.0f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(current_profile(), path("/interaction_profiles/facebook/touch_controller_pro"));
    EXPECT_FLOAT_EQ(float_state(curl, hands[0]).currentState, 0.3f);
    EXPECT_FLOAT_EQ(float_state(stylus, hands[0]).currentState, 0.7f);
    EXPECT_FLOAT_EQ(float_state(rest, hands[0]).currentState, 0.9f);
    EXPECT_TRUE(bool_state(near, hands[0]).currentState);

    const XrHapticVibration  vibration = { .type = XR_TYPE_HAPTIC_VIBRATION, .duration = 50000000, .frequency = XR_FREQUENCY_UNSPECIFIED, .amplitude = 0.5f };
    const XrHapticActionInfo info      = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = buzz, .subactionPath = hands[0] };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&vibration), XR_SUCCESS);
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Trigger).active);
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Trigger).amplitude, 0.5f);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);
    EXPECT_EQ(xr.xrStopHapticFeedback(session, &info), XR_SUCCESS);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Trigger).active);
}

// OVRPlugin binds two paths outside the registry that Meta's runtime accepts; rejecting them left
// its Touch Plus and Pro bindings out and its input dead
TEST_F(OxrMetaControllerTest, ovrplugin_paths_bind_the_inputs_they_stand_for)
{
    const XrAction near  = make_action("near", XR_ACTION_TYPE_BOOLEAN_INPUT);
    const XrAction thumb = make_action("thumb", XR_ACTION_TYPE_BOOLEAN_INPUT);
    const XrAction buzz  = make_action("buzz", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    const XrAction tap   = make_action("tap", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    const XrActionSuggestedBinding plus[] = { { near, path("/user/hand/right/input/trigger/proximity_fb") }, { thumb, path("/user/hand/right/input/thumb_fb/proximity_fb") } };
    ASSERT_EQ(suggest("/interaction_profiles/meta/touch_controller_plus", plus, ARRAY_SIZE(plus)), XR_SUCCESS);
    const XrActionSuggestedBinding pro[] = { { buzz, path("/user/hand/left/output/trigger_haptic_fb") }, { tap, path("/user/hand/left/output/thumb_haptic_fb") } };
    ASSERT_EQ(suggest("/interaction_profiles/facebook/touch_controller_pro", pro, ARRAY_SIZE(pro)), XR_SUCCESS);
    attach();
    begin_session();

    SIM_Sim* sim = oxr_get_sim();
    ASSERT_EQ(sim_get_controller(sim), SIM_Controller::MetaTouchPlus);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerProximity, { .value = { 1.0f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::ThumbProximity, { .value = { 1.0f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(bool_state(near, hands[1]).currentState);
    EXPECT_TRUE(bool_state(thumb, hands[1]).currentState);

    sim_set_controller(sim, SIM_Controller::MetaTouchPro);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    const XrHapticVibration  vibration = { .type = XR_TYPE_HAPTIC_VIBRATION, .duration = 50000000, .frequency = XR_FREQUENCY_UNSPECIFIED, .amplitude = 0.5f };
    const XrHapticActionInfo info      = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = buzz, .subactionPath = hands[0] };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&vibration), XR_SUCCESS);
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Trigger).active);
    const XrHapticActionInfo thumb_info = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = tap, .subactionPath = hands[0] };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &thumb_info, (const XrHapticBaseHeader*)&vibration), XR_SUCCESS);
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Thumb).active);
}

// The component path of an input on a controller's own profile; the simulator names its inputs
// after the plain paths
static const char* native_component (SIM_Controller controller, SIM_Input input)
{
    switch (input)
    {
    case SIM_Input::TriggerCurl:
        return "trigger_curl/value";
    case SIM_Input::TriggerSlide:
        return "trigger_slide/value";
    case SIM_Input::TriggerProximity:
        return controller == SIM_Controller::OculusTouch ? "trigger/proximity_fb" : "trigger/proximity";
    case SIM_Input::ThumbProximity:
        return controller == SIM_Controller::OculusTouch ? "thumb_fb/proximity_fb" : "thumb_resting_surfaces/proximity";
    default:
        return sim_input_name(input);
    }
}

// Every input the Devices panel offers on a hand reaches an app through its controller's own
// profile on that hand; the panel offered the other hand's buttons, which reached nothing
TEST_F(OxrMetaControllerTest, every_input_of_each_hand_reaches_its_own_profile)
{
    const struct { SIM_Controller controller; const char* profile; } controllers[] = {
        { SIM_Controller::OculusTouch,   "/interaction_profiles/oculus/touch_controller" },
        { SIM_Controller::MetaTouchPro,  "/interaction_profiles/meta/touch_pro_controller" },
        { SIM_Controller::MetaTouchPlus, "/interaction_profiles/meta/touch_plus_controller" },
        { SIM_Controller::KhrSimple,     "/interaction_profiles/khr/simple_controller" },
    };
    const SIM_Device devices[2] = { SIM_Device::LeftHand, SIM_Device::RightHand };
    const char*      sides[2]   = { "left", "right" };
    XrAction actions[(u32)SIM_Input::Count] = {};
    for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
    {
        char name[16];
        cstr_format(name, sizeof(name), "input_%u", i);
        const SIM_InputType type = sim_input_type((SIM_Input)i);
        actions[i] = make_action(name, type == SIM_InputType::Bool ? XR_ACTION_TYPE_BOOLEAN_INPUT : type == SIM_InputType::Float ? XR_ACTION_TYPE_FLOAT_INPUT : XR_ACTION_TYPE_VECTOR2F_INPUT);
    }
    for (const auto& c : controllers)
    {
        XrActionSuggestedBinding bindings[2 * (u32)SIM_Input::Count] = {};
        u32                      count = 0;
        for (u32 h = 0; h < 2; h++)
        {
            for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
            {
                if (!sim_hand_has_input(SIM_HandSource::Controller, c.controller, devices[h], (SIM_Input)i))
                {
                    continue;
                }
                char binding[XR_MAX_PATH_LENGTH];
                cstr_format(binding, sizeof(binding), "/user/hand/%s/input/%s", sides[h], native_component(c.controller, (SIM_Input)i));
                const XrActionSuggestedBinding one = { actions[i], path(binding) };
                EXPECT_EQ(suggest(c.profile, &one, 1), XR_SUCCESS) << c.profile << " has " << binding;
                bindings[count++] = one;
            }
        }
        ASSERT_EQ(suggest(c.profile, bindings, count), XR_SUCCESS) << c.profile;
    }
    attach();
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    for (const auto& c : controllers)
    {
        sim_set_controller(sim, c.controller);
        frame();
        ASSERT_EQ(sync(), XR_SUCCESS);
        ASSERT_EQ(current_profile(), path(c.profile));
        for (u32 h = 0; h < 2; h++)
        {
            for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
            {
                const SIM_Input input = (SIM_Input)i;
                if (!sim_hand_has_input(SIM_HandSource::Controller, c.controller, devices[h], input))
                {
                    continue;
                }
                SCOPED_TRACE(testing::Message() << c.profile << " " << sides[h] << " " << sim_input_name(input));
                sim_set_input(sim, devices[h], input, { .value = { 0.75f, -0.5f } });
                frame();
                ASSERT_EQ(sync(), XR_SUCCESS);
                switch (sim_input_type(input))
                {
                case SIM_InputType::Bool:
                    EXPECT_TRUE(bool_state(actions[i], hands[h]).currentState);
                    break;
                case SIM_InputType::Float:
                    EXPECT_FLOAT_EQ(float_state(actions[i], hands[h]).currentState, 0.75f);
                    break;
                case SIM_InputType::Vec2:
                {
                    XrActionStateVector2f      state = { .type = XR_TYPE_ACTION_STATE_VECTOR2F };
                    const XrActionStateGetInfo info  = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = actions[i], .subactionPath = hands[h] };
                    EXPECT_EQ(xr.xrGetActionStateVector2f(session, &info, &state), XR_SUCCESS);
                    EXPECT_FLOAT_EQ(state.currentState.x, 0.75f);
                    EXPECT_FLOAT_EQ(state.currentState.y, -0.5f);
                    break;
                }
                }
                sim_set_input(sim, devices[h], input, {});
            }
        }
    }
}

// OpenXR 1.1 promoted the Meta profiles under new names with plain paths (spec 6.4)
TEST_F(OxrMetaControllerTest, core_touch_pro_profile_reads_its_inputs_and_haptic_outputs)
{
    const XrAction curl   = make_action("curl", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrAction slide  = make_action("slide", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrAction stylus = make_action("stylus", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrAction near   = make_action("near", XR_ACTION_TYPE_BOOLEAN_INPUT);
    const XrAction buzz   = make_action("buzz", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    const XrActionSuggestedBinding pro[] = {
        { curl,   path("/user/hand/left/input/trigger_curl/value") },
        { slide,  path("/user/hand/left/input/trigger_slide") },
        { stylus, path("/user/hand/left/input/stylus/force") },
        { near,   path("/user/hand/left/input/thumb_resting_surfaces/proximity") },
        { buzz,   path("/user/hand/left/output/haptic_thumb") },
    };
    ASSERT_EQ(suggest("/interaction_profiles/meta/touch_pro_controller", pro, ARRAY_SIZE(pro)), XR_SUCCESS);
    const XrActionSuggestedBinding old_name = { curl, path("/user/hand/left/input/trigger/curl_fb") };
    EXPECT_EQ(suggest("/interaction_profiles/meta/touch_pro_controller", &old_name, 1), XR_ERROR_PATH_UNSUPPORTED);
    attach();
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_controller(sim, SIM_Controller::MetaTouchPro);
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerCurl, { .value = { 0.3f, 0.0f } });
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerSlide, { .value = { 0.2f, 0.0f } });
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::StylusForce, { .value = { 0.7f, 0.0f } });
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::ThumbProximity, { .value = { 1.0f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(current_profile(), path("/interaction_profiles/meta/touch_pro_controller"));
    EXPECT_FLOAT_EQ(float_state(curl, hands[0]).currentState, 0.3f);
    EXPECT_FLOAT_EQ(float_state(slide, hands[0]).currentState, 0.2f);
    EXPECT_FLOAT_EQ(float_state(stylus, hands[0]).currentState, 0.7f);
    EXPECT_TRUE(bool_state(near, hands[0]).currentState);

    const XrHapticVibration  vibration = { .type = XR_TYPE_HAPTIC_VIBRATION, .duration = 50000000, .frequency = XR_FREQUENCY_UNSPECIFIED, .amplitude = 0.5f };
    const XrHapticActionInfo info      = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = buzz, .subactionPath = hands[0] };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&vibration), XR_SUCCESS);
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Thumb).active);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);
}

TEST_F(OxrMetaControllerTest, core_touch_plus_profile_reads_curl_and_force)
{
    const XrAction curl  = make_action("curl", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrAction force = make_action("force", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrActionSuggestedBinding plus[] = {
        { curl,  path("/user/hand/right/input/trigger_curl/value") },
        { force, path("/user/hand/right/input/trigger/force") },
    };
    ASSERT_EQ(suggest("/interaction_profiles/meta/touch_plus_controller", plus, ARRAY_SIZE(plus)), XR_SUCCESS);
    attach();
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    ASSERT_EQ(sim_get_controller(sim), SIM_Controller::MetaTouchPlus);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerCurl, { .value = { 0.4f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerForce, { .value = { 0.6f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(current_profile(), path("/interaction_profiles/meta/touch_plus_controller"));
    EXPECT_FLOAT_EQ(float_state(curl, hands[1]).currentState, 0.4f);
    EXPECT_FLOAT_EQ(float_state(force, hands[1]).currentState, 0.6f);
}

// Every Touch generation serves the Quest 2 profile, so an app suggesting only 1.1 names
// gets input on any of them
TEST_F(OxrMetaControllerTest, quest_2_profile_is_served_by_every_touch_generation)
{
    const XrAction near = make_action("near", XR_ACTION_TYPE_BOOLEAN_INPUT);
    const XrActionSuggestedBinding quest_2[] = {
        { trigger, path("/user/hand/right/input/trigger/value") },
        { near,    path("/user/hand/right/input/trigger/proximity") },
    };
    ASSERT_EQ(suggest("/interaction_profiles/meta/touch_controller_quest_2", quest_2, ARRAY_SIZE(quest_2)), XR_SUCCESS);
    attach();
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    const SIM_Controller generations[] = { SIM_Controller::OculusTouch, SIM_Controller::MetaTouchPro, SIM_Controller::MetaTouchPlus };
    for (SIM_Controller hardware : generations)
    {
        sim_set_controller(sim, hardware);
        sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 0.5f, 0.0f } });
        sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerProximity, { .value = { 1.0f, 0.0f } });
        frame();
        ASSERT_EQ(sync(), XR_SUCCESS);
        EXPECT_EQ(current_profile(), path("/interaction_profiles/meta/touch_controller_quest_2"));
        EXPECT_FLOAT_EQ(float_state(trigger, hands[1]).currentState, 0.5f);
        EXPECT_TRUE(bool_state(near, hands[1]).currentState);
    }
    sim_set_controller(sim, SIM_Controller::KhrSimple);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(current_profile(), (XrPath)XR_NULL_PATH);
}

TEST_F(OxrMetaControllerTest, touch_profile_gains_proximity)
{
    const XrAction trigger_near = make_action("trigger_near", XR_ACTION_TYPE_BOOLEAN_INPUT);
    const XrAction thumb_near   = make_action("thumb_near", XR_ACTION_TYPE_BOOLEAN_INPUT);
    const XrActionSuggestedBinding touch[] = {
        { trigger_near, path("/user/hand/right/input/trigger/proximity_fb") },
        { thumb_near,   path("/user/hand/right/input/thumb_fb/proximity_fb") },
    };
    ASSERT_EQ(suggest("/interaction_profiles/oculus/touch_controller", touch, ARRAY_SIZE(touch)), XR_SUCCESS);
    attach();
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_controller(sim, SIM_Controller::OculusTouch);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::ThumbProximity, { .value = { 1.0f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_FALSE(bool_state(trigger_near, hands[1]).currentState);
    EXPECT_TRUE(bool_state(thumb_near, hands[1]).currentState);
}

// XR_EXT_conformance_automation activating a profile selects the hardware that serves it
TEST_F(OxrMetaControllerTest, automation_selects_meta_hardware)
{
    attach();
    begin_session();
    const SIM_Sim* sim = oxr_get_sim();
    PFN_xrSetInputDeviceActiveEXT set_active = nullptr;
    ASSERT_EQ(gipa(instance, "xrSetInputDeviceActiveEXT", (PFN_xrVoidFunction*)&set_active), XR_SUCCESS);
    EXPECT_EQ(set_active(session, path("/interaction_profiles/facebook/touch_controller_pro"), hands[0], XR_TRUE), XR_SUCCESS);
    EXPECT_EQ(sim_get_controller(sim), SIM_Controller::MetaTouchPro);
    EXPECT_EQ(set_active(session, path("/interaction_profiles/meta/touch_controller_plus"), hands[0], XR_TRUE), XR_SUCCESS);
    EXPECT_EQ(sim_get_controller(sim), SIM_Controller::MetaTouchPlus);
    EXPECT_EQ(set_active(session, path("/interaction_profiles/oculus/touch_controller"), hands[0], XR_TRUE), XR_SUCCESS);
    EXPECT_EQ(sim_get_controller(sim), SIM_Controller::OculusTouch);
}

//
// XR_KHR_binding_modification, XR_EXT_dpad_binding, XR_VALVE_analog_threshold
//

namespace
{
struct OxrBindingModifierTest : OxrActionTest
{
    XrAction pressed = XR_NULL_HANDLE;
    XrAction dpad[4] = {}; // up, down, left, right

    OxrBindingModifierTest ()
    {
        more_extensions[more_extensions_count++] = XR_KHR_BINDING_MODIFICATION_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_EXT_DPAD_BINDING_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_VALVE_ANALOG_THRESHOLD_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrActionTest::SetUp();
        pressed = make_action("pressed", XR_ACTION_TYPE_BOOLEAN_INPUT);
        dpad[0] = make_action("up", XR_ACTION_TYPE_BOOLEAN_INPUT);
        dpad[1] = make_action("down", XR_ACTION_TYPE_BOOLEAN_INPUT);
        dpad[2] = make_action("left", XR_ACTION_TYPE_BOOLEAN_INPUT);
        dpad[3] = make_action("right", XR_ACTION_TYPE_BOOLEAN_INPUT);
    }

    XrResult suggest (const XrBindingModificationBaseHeaderKHR* const* modifications, u32 count, const void* chain = nullptr)
    {
        const XrActionSuggestedBinding bindings[] = {
            { pressed, path("/user/hand/left/input/trigger/value") },
            { pressed, path("/user/hand/right/input/trigger/value") },
            { dpad[0], path("/user/hand/right/input/thumbstick/dpad_up") },
            { dpad[1], path("/user/hand/right/input/thumbstick/dpad_down") },
            { dpad[2], path("/user/hand/right/input/thumbstick/dpad_left") },
            { dpad[3], path("/user/hand/right/input/thumbstick/dpad_right") },
            { haptic,  path("/user/hand/right/output/haptic") },
        };
        const XrBindingModificationsKHR            mods      = { .type = XR_TYPE_BINDING_MODIFICATIONS_KHR, .next = chain, .bindingModificationCount = count, .bindingModifications = modifications };
        const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .next = count ? &mods : chain, .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = ARRAY_SIZE(bindings), .suggestedBindings = bindings };
        return xr.xrSuggestInteractionProfileBindings(instance, &suggested);
    }

    void start ()
    {
        attach();
        begin_session();
        ASSERT_EQ(sync(), XR_SUCCESS);
    }

    void set_input (SIM_Device device, SIM_Input input, f32 x, f32 y = 0.0f)
    {
        sim_set_input(oxr_get_sim(), device, input, { .value = { x, y } });
        ASSERT_EQ(sync(), XR_SUCCESS);
    }

    u32 dpad_bits ()
    {
        u32 bits = 0;
        for (u32 i = 0; i < 4; i++)
        {
            bits |= bool_state(dpad[i], hands[1]).currentState ? 1u << i : 0u;
        }
        return bits;
    }
};

#define DPAD_UP    1u
#define DPAD_DOWN  2u
#define DPAD_LEFT  4u
#define DPAD_RIGHT 8u

} // namespace

TEST_F(OxrActionTest, dpad_paths_need_their_extension)
{
    const XrActionSuggestedBinding             binding   = { click, path("/user/hand/right/input/thumbstick/dpad_up") };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = 1, .suggestedBindings = &binding };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED);
}

TEST_F(OxrBindingModifierTest, analog_thresholds_apply_hysteresis_and_haptics)
{
    const XrHapticVibration                        buzz      = { .type = XR_TYPE_HAPTIC_VIBRATION, .duration = 100000000, .frequency = XR_FREQUENCY_UNSPECIFIED, .amplitude = 0.5f };
    const XrInteractionProfileAnalogThresholdVALVE threshold = { .type = XR_TYPE_INTERACTION_PROFILE_ANALOG_THRESHOLD_VALVE, .action = pressed, .binding = path("/user/hand/right/input/trigger/value"), .onThreshold = 0.7f, .offThreshold = 0.3f, .onHaptic = (const XrHapticBaseHeader*)&buzz };
    const XrBindingModificationBaseHeaderKHR*      mods[]    = { (const XrBindingModificationBaseHeaderKHR*)&threshold };
    ASSERT_EQ(suggest(mods, 1), XR_SUCCESS);
    start();
    const SIM_Sim* sim = oxr_get_sim();

    set_input(SIM_Device::LeftHand, SIM_Input::TriggerValue, 0.6f);
    set_input(SIM_Device::RightHand, SIM_Input::TriggerValue, 0.6f);
    EXPECT_TRUE(bool_state(pressed, hands[0]).currentState) << "the runtime's own threshold";
    EXPECT_FALSE(bool_state(pressed, hands[1]).currentState) << "below onThreshold";
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active);
    set_input(SIM_Device::RightHand, SIM_Input::TriggerValue, 0.75f);
    EXPECT_TRUE(bool_state(pressed, hands[1]).currentState);
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active) << "onHaptic";
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).amplitude, 0.5f);
    set_input(SIM_Device::RightHand, SIM_Input::TriggerValue, 0.4f);
    EXPECT_TRUE(bool_state(pressed, hands[1]).currentState) << "stays true above offThreshold";
    set_input(SIM_Device::RightHand, SIM_Input::TriggerValue, 0.29f);
    EXPECT_FALSE(bool_state(pressed, hands[1]).currentState);
}

TEST_F(OxrBindingModifierTest, a_threshold_chained_directly_still_works)
{
    const XrInteractionProfileAnalogThresholdVALVE threshold = { .type = XR_TYPE_INTERACTION_PROFILE_ANALOG_THRESHOLD_VALVE, .action = pressed, .binding = path("/user/hand/right/input/trigger/value"), .onThreshold = 0.9f, .offThreshold = 0.8f };
    ASSERT_EQ(suggest(nullptr, 0, &threshold), XR_SUCCESS);
    start();
    set_input(SIM_Device::RightHand, SIM_Input::TriggerValue, 0.85f);
    EXPECT_FALSE(bool_state(pressed, hands[1]).currentState);
    set_input(SIM_Device::RightHand, SIM_Input::TriggerValue, 0.95f);
    EXPECT_TRUE(bool_state(pressed, hands[1]).currentState);
}

TEST_F(OxrBindingModifierTest, thresholds_are_validated)
{
    XrInteractionProfileAnalogThresholdVALVE  threshold = { .type = XR_TYPE_INTERACTION_PROFILE_ANALOG_THRESHOLD_VALVE, .action = pressed, .binding = path("/user/hand/right/input/trigger/value"), .onThreshold = 0.3f, .offThreshold = 0.7f };
    const XrBindingModificationBaseHeaderKHR* mods[]    = { (const XrBindingModificationBaseHeaderKHR*)&threshold };
    EXPECT_EQ(suggest(mods, 1), XR_ERROR_VALIDATION_FAILURE) << "on below off";
    threshold.onThreshold = 1.5f;
    EXPECT_EQ(suggest(mods, 1), XR_ERROR_VALIDATION_FAILURE) << "out of range";
    threshold.onThreshold = 0.8f;
    threshold.binding     = path("/user/hand/right/input/squeeze/value");
    EXPECT_EQ(suggest(mods, 1), XR_ERROR_PATH_UNSUPPORTED) << "not a suggested binding of the action";
    threshold.binding = path("/user/hand/right/input/trigger/value");
    threshold.action  = click;
    EXPECT_EQ(suggest(mods, 1), XR_ERROR_PATH_UNSUPPORTED);
    threshold.action = pressed;
    const XrHapticVibration bad_haptic = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    threshold.onHaptic = (const XrHapticBaseHeader*)&bad_haptic;
    EXPECT_EQ(suggest(mods, 1), XR_ERROR_VALIDATION_FAILURE) << "no haptic type";
    threshold.onHaptic = nullptr;
    EXPECT_EQ(suggest(mods, 1), XR_SUCCESS);
}

TEST_F(OxrBindingModifierTest, dpads_follow_the_thumbstick)
{
    ASSERT_EQ(suggest(nullptr, 0), XR_SUCCESS);
    start();
    set_input(SIM_Device::RightHand, SIM_Input::Thumbstick, 0.0f, 0.3f);
    EXPECT_EQ(dpad_bits(), 0u) << "inside the default center region of 0.5";
    set_input(SIM_Device::RightHand, SIM_Input::Thumbstick, 0.3f, 0.9f);
    EXPECT_EQ(dpad_bits(), DPAD_UP);
    set_input(SIM_Device::RightHand, SIM_Input::Thumbstick, 0.9f, -0.2f);
    EXPECT_EQ(dpad_bits(), DPAD_RIGHT);
    set_input(SIM_Device::RightHand, SIM_Input::Thumbstick, -0.2f, -0.9f);
    EXPECT_EQ(dpad_bits(), DPAD_DOWN);
    set_input(SIM_Device::RightHand, SIM_Input::Thumbstick, -0.9f, 0.0f);
    EXPECT_EQ(dpad_bits(), DPAD_LEFT);
    set_input(SIM_Device::RightHand, SIM_Input::Thumbstick, 0.64f, 0.64f);
    EXPECT_EQ(dpad_bits() & (DPAD_DOWN | DPAD_LEFT), 0u) << "never opposite directions";
}

TEST_F(OxrBindingModifierTest, wide_wedges_overlap_and_sticky_dpads_latch)
{
    const XrHapticVibration                  buzz   = { .type = XR_TYPE_HAPTIC_VIBRATION, .duration = 50000000, .frequency = XR_FREQUENCY_UNSPECIFIED, .amplitude = 0.25f };
    const XrInteractionProfileDpadBindingEXT dpad_mod = { .type = XR_TYPE_INTERACTION_PROFILE_DPAD_BINDING_EXT, .binding = path("/user/hand/right/input/thumbstick"), .actionSet = set, .forceThreshold = 0.5f, .forceThresholdReleased = 0.4f, .centerRegion = 0.3f, .wedgeAngle = 0.75f * zm::PI, .isSticky = XR_TRUE, .onHaptic = (const XrHapticBaseHeader*)&buzz };
    const XrBindingModificationBaseHeaderKHR* mods[] = { (const XrBindingModificationBaseHeaderKHR*)&dpad_mod };
    ASSERT_EQ(suggest(mods, 1), XR_SUCCESS);
    start();
    const SIM_Sim* sim = oxr_get_sim();

    set_input(SIM_Device::RightHand, SIM_Input::Thumbstick, 0.0f, 0.4f);
    EXPECT_EQ(dpad_bits(), DPAD_UP) << "outside the smaller center region";
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active) << "onHaptic";
    set_input(SIM_Device::RightHand, SIM_Input::Thumbstick, 0.9f, -0.1f);
    EXPECT_EQ(dpad_bits(), DPAD_UP) << "latched while the stick stays out of the center";
    set_input(SIM_Device::RightHand, SIM_Input::Thumbstick, 0.0f, 0.0f);
    EXPECT_EQ(dpad_bits(), 0u);
    set_input(SIM_Device::RightHand, SIM_Input::Thumbstick, 0.64f, 0.64f);
    EXPECT_EQ(dpad_bits(), DPAD_UP | DPAD_RIGHT) << "135 degree wedges overlap at the diagonal";
}

TEST_F(OxrBindingModifierTest, dpads_are_validated)
{
    XrInteractionProfileDpadBindingEXT        dpad_mod = { .type = XR_TYPE_INTERACTION_PROFILE_DPAD_BINDING_EXT, .binding = path("/user/hand/right/input/thumbstick"), .actionSet = set, .forceThreshold = 0.5f, .forceThresholdReleased = 0.4f, .centerRegion = 0.5f, .wedgeAngle = 0.5f * zm::PI };
    const XrBindingModificationBaseHeaderKHR* mods[]   = { (const XrBindingModificationBaseHeaderKHR*)&dpad_mod, (const XrBindingModificationBaseHeaderKHR*)&dpad_mod };
    EXPECT_EQ(suggest(mods, 2), XR_ERROR_VALIDATION_FAILURE) << "the same input and action set twice";
    dpad_mod.centerRegion = 0.0f;
    EXPECT_EQ(suggest(mods, 1), XR_ERROR_VALIDATION_FAILURE);
    dpad_mod.centerRegion = 0.5f;
    dpad_mod.wedgeAngle   = zm::PI;
    EXPECT_EQ(suggest(mods, 1), XR_ERROR_VALIDATION_FAILURE);
    dpad_mod.wedgeAngle             = 0.5f * zm::PI;
    dpad_mod.forceThresholdReleased = 0.6f;
    EXPECT_EQ(suggest(mods, 1), XR_ERROR_VALIDATION_FAILURE);
    dpad_mod.forceThresholdReleased = 0.4f;
    dpad_mod.forceThreshold         = 0.0f;
    EXPECT_EQ(suggest(mods, 1), XR_ERROR_VALIDATION_FAILURE);
    dpad_mod.forceThreshold = 0.5f;
    EXPECT_EQ(suggest(mods, 1), XR_SUCCESS);
}

namespace
{
struct OxrHapticEnvelopeTest : OxrActionTest
{
    OxrHapticEnvelopeTest ()
    {
        more_extensions[more_extensions_count++] = XR_FB_HAPTIC_AMPLITUDE_ENVELOPE_EXTENSION_NAME;
    }
};
} // namespace

TEST_F(OxrHapticEnvelopeTest, envelopes_play_and_are_validated)
{
    suggest_touch_bindings();
    attach();
    begin_session();
    EXPECT_EQ(sync(), XR_SUCCESS);

    const f32                            amplitudes[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
    XrHapticAmplitudeEnvelopeVibrationFB envelope      = { .type = XR_TYPE_HAPTIC_AMPLITUDE_ENVELOPE_VIBRATION_FB, .duration = 400000000, .amplitudeCount = 4, .amplitudes = amplitudes };
    const XrHapticActionInfo             info          = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hands[1] };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&envelope), XR_SUCCESS);
    const SIM_Sim* sim = oxr_get_sim();
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active);
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).amplitude, 0.2f);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);
    frame();
    EXPECT_GT(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).amplitude, 0.2f);
    EXPECT_EQ(xr.xrStopHapticFeedback(session, &info), XR_SUCCESS);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active);

    envelope.amplitudeCount = 0;
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&envelope), XR_ERROR_VALIDATION_FAILURE);
    envelope.amplitudeCount = 4;
    envelope.amplitudes     = nullptr;
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&envelope), XR_ERROR_VALIDATION_FAILURE);
    const f32 broken[2] = { 0.5f, zm::nan() };
    envelope.amplitudes     = broken;
    envelope.amplitudeCount = 2;
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&envelope), XR_ERROR_VALIDATION_FAILURE);
    envelope.amplitudes = amplitudes;
    envelope.duration   = -5;
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&envelope), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active);
}

TEST_F(OxrActionTest, haptic_envelopes_need_their_extension)
{
    suggest_touch_bindings();
    attach();
    begin_session();
    EXPECT_EQ(sync(), XR_SUCCESS);
    const f32                                  amplitudes[2] = { 0.5f, 1.0f };
    const XrHapticAmplitudeEnvelopeVibrationFB envelope      = { .type = XR_TYPE_HAPTIC_AMPLITUDE_ENVELOPE_VIBRATION_FB, .duration = 10000000, .amplitudeCount = 2, .amplitudes = amplitudes };
    const XrHapticActionInfo                   info          = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hands[1] };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&envelope), XR_ERROR_VALIDATION_FAILURE);
}

//
// XR_FB_haptic_pcm, XR_EXT_haptic_parametric and its preview XR_EXTX1_haptic_parametric
//

#define XR_EXTX1_HAPTIC_PARAMETRIC_TEST_NAME "XR_EXTX1_haptic_parametric"

namespace
{
struct OxrHapticPcmTest : OxrActionTest
{
    PFN_xrGetDeviceSampleRateFB get_rate = nullptr;

    OxrHapticPcmTest ()
    {
        more_extensions[more_extensions_count++] = XR_FB_HAPTIC_PCM_EXTENSION_NAME;
    }

    void start ()
    {
        ASSERT_EQ(gipa(instance, "xrGetDeviceSampleRateFB", (PFN_xrVoidFunction*)&get_rate), XR_SUCCESS);
        suggest_touch_bindings();
        attach();
        begin_session();
        EXPECT_EQ(sync(), XR_SUCCESS);
    }
};

struct OxrHapticParametricTest : OxrActionTest
{
    PFN_xrHapticParametricGetPropertiesEXT get_properties = nullptr;

    OxrHapticParametricTest ()
    {
        more_extensions[more_extensions_count++] = XR_EXT_HAPTIC_PARAMETRIC_EXTENSION_NAME;
    }

    void start (const char* get_properties_name = "xrHapticParametricGetPropertiesEXT")
    {
        ASSERT_EQ(gipa(instance, get_properties_name, (PFN_xrVoidFunction*)&get_properties), XR_SUCCESS);
        suggest_touch_bindings();
        attach();
        begin_session();
        EXPECT_EQ(sync(), XR_SUCCESS);
    }

    XrResult apply (const XrHapticParametricVibrationEXT& vibration, XrPath hand) const
    {
        const XrHapticActionInfo info = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hand };
        return xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&vibration);
    }
};

struct OxrHapticParametricPreviewTest : OxrHapticParametricTest
{
    OxrHapticParametricPreviewTest ()
    {
        more_extensions[more_extensions_count - 1] = XR_EXTX1_HAPTIC_PARAMETRIC_TEST_NAME;
    }
};
} // namespace

TEST_F(OxrActionTest, pcm_and_parametric_haptics_need_their_extensions)
{
    suggest_touch_bindings();
    attach();
    begin_session();
    EXPECT_EQ(sync(), XR_SUCCESS);
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrGetDeviceSampleRateFB", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrHapticParametricGetPropertiesEXT", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);

    const f32                      samples[2] = { 0.5f, 1.0f };
    u32                            consumed   = 0;
    const XrHapticPcmVibrationFB   pcm        = { .type = XR_TYPE_HAPTIC_PCM_VIBRATION_FB, .bufferSize = 2, .buffer = samples, .sampleRate = 2000.0f, .samplesConsumed = &consumed };
    const XrHapticActionInfo       info       = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hands[1] };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&pcm), XR_ERROR_VALIDATION_FAILURE);
    const XrHapticParametricPointEXT points[2]  = { { 0, 0.5f }, { 10000000, 0.5f } };
    const XrHapticParametricVibrationEXT param = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 2, .amplitudePoints = points };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&param), XR_ERROR_VALIDATION_FAILURE);

    XrSystemHapticParametricPropertiesEXT support = { .type = XR_TYPE_SYSTEM_HAPTIC_PARAMETRIC_PROPERTIES_EXT, .supportsParametricHaptics = XR_TRUE };
    XrSystemProperties                    props   = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &support };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_FALSE(support.supportsParametricHaptics);
}

TEST_F(OxrHapticPcmTest, sample_rate_of_the_bound_controller)
{
    start();
    XrDevicePcmSampleRateStateFB rate = { .type = XR_TYPE_DEVICE_PCM_SAMPLE_RATE_STATE_FB };
    const XrHapticActionInfo     info = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hands[0] };
    ASSERT_EQ(get_rate(session, &info, &rate), XR_SUCCESS);
    EXPECT_FLOAT_EQ(rate.sampleRate, SIM_HAPTIC_PCM_RATE);
    const XrHapticActionInfo any = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic };
    ASSERT_EQ(get_rate(session, &any, &rate), XR_SUCCESS);
    EXPECT_FLOAT_EQ(rate.sampleRate, SIM_HAPTIC_PCM_RATE);
    EXPECT_EQ(get_rate(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    XrDevicePcmSampleRateStateFB wrong = { .type = XR_TYPE_HAPTIC_PCM_VIBRATION_FB };
    EXPECT_EQ(get_rate(session, &info, &wrong), XR_ERROR_VALIDATION_FAILURE);
    const XrHapticActionInfo other = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = trigger };
    EXPECT_EQ(get_rate(session, &other, &rate), XR_ERROR_ACTION_TYPE_MISMATCH);
}

TEST_F(OxrHapticPcmTest, samples_play_and_append)
{
    start();
    f32 samples[200];
    for (u32 i = 0; i < 200; i++)
    {
        samples[i] = i % 2 ? 0.8f : -0.8f;
    }
    u32                      consumed = 0;
    XrHapticPcmVibrationFB   pcm      = { .type = XR_TYPE_HAPTIC_PCM_VIBRATION_FB, .bufferSize = 200, .buffer = samples, .sampleRate = 2000.0f, .append = XR_FALSE, .samplesConsumed = &consumed };
    const XrHapticActionInfo info     = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hands[1] };
    ASSERT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&pcm), XR_SUCCESS);
    EXPECT_EQ(consumed, 200u);
    const SIM_Sim* sim = oxr_get_sim();
    const SIM_Haptic played = sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main);
    EXPECT_TRUE(played.active);
    EXPECT_FLOAT_EQ(played.amplitude, 0.8f);
    EXPECT_EQ(played.end_time - played.start_time, 100000000);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);

    // Spec: append continues after the unfinished samples
    pcm.append = XR_TRUE;
    ASSERT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&pcm), XR_SUCCESS);
    EXPECT_EQ(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).end_time - played.start_time, 200000000);

    // More than the buffer holds is consumed in part
    static f32 many[XR_MAX_HAPTIC_PCM_BUFFER_SIZE_FB + 100];
    pcm = { .type = XR_TYPE_HAPTIC_PCM_VIBRATION_FB, .bufferSize = ARRAY_SIZE(many), .buffer = many, .sampleRate = 2000.0f, .append = XR_FALSE, .samplesConsumed = &consumed };
    ASSERT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&pcm), XR_SUCCESS);
    EXPECT_EQ(consumed, (u32)XR_MAX_HAPTIC_PCM_BUFFER_SIZE_FB);
    EXPECT_EQ(xr.xrStopHapticFeedback(session, &info), XR_SUCCESS);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active);
}

TEST_F(OxrHapticPcmTest, pcm_is_validated)
{
    start();
    const f32                samples[2] = { 0.5f, 1.0f };
    u32                      consumed   = 0;
    XrHapticPcmVibrationFB   pcm        = { .type = XR_TYPE_HAPTIC_PCM_VIBRATION_FB, .bufferSize = 2, .buffer = samples, .sampleRate = 2000.0f, .samplesConsumed = nullptr };
    const XrHapticActionInfo info       = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hands[1] };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&pcm), XR_ERROR_VALIDATION_FAILURE);
    pcm.samplesConsumed = &consumed;
    pcm.buffer          = nullptr;
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&pcm), XR_ERROR_VALIDATION_FAILURE);
    pcm.buffer     = samples;
    pcm.sampleRate = 0.0f;
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&pcm), XR_ERROR_VALIDATION_FAILURE);
    pcm.sampleRate = zm::nan();
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&pcm), XR_ERROR_VALIDATION_FAILURE);
    pcm.sampleRate              = 2000.0f;
    const f32 broken[2]         = { 0.5f, zm::nan() };
    pcm.buffer                  = broken;
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&pcm), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_FALSE(sim_get_haptic(oxr_get_sim(), SIM_Device::RightHand, SIM_HapticOutput::Main).active);
}

TEST_F(OxrHapticParametricTest, system_and_output_properties)
{
    start();
    XrSystemHapticParametricPropertiesEXT support = { .type = XR_TYPE_SYSTEM_HAPTIC_PARAMETRIC_PROPERTIES_EXT };
    XrSystemProperties                    props   = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &support };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_TRUE(support.supportsParametricHaptics);

    XrHapticParametricPropertiesEXT properties = { .type = XR_TYPE_HAPTIC_PARAMETRIC_PROPERTIES_EXT };
    const XrHapticActionInfo        info       = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hands[0] };
    ASSERT_EQ(get_properties(session, &info, &properties), XR_SUCCESS);
    EXPECT_FLOAT_EQ(properties.minFrequencyHz, SIM_HAPTIC_MIN_HZ);
    EXPECT_FLOAT_EQ(properties.maxFrequencyHz, SIM_HAPTIC_MAX_HZ);
    EXPECT_GE(properties.idealFrameSubmissionRate, 0);
    EXPECT_EQ(properties.minimumFirstFrameDuration, 0);
    EXPECT_EQ(get_properties(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    const XrHapticActionInfo other = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = trigger };
    EXPECT_EQ(get_properties(session, &other, &properties), XR_ERROR_ACTION_TYPE_MISMATCH);
}

TEST_F(OxrHapticParametricTest, events_play_their_curves)
{
    start();
    const XrHapticParametricPointEXT     amplitudes[2]  = { { 0, 0.0f }, { 100000000, 1.0f } };
    const XrHapticParametricPointEXT     frequencies[2] = { { 0, 0.0f }, { 100000000, 1.0f } };
    const XrHapticParametricVibrationEXT vibration      = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 2, .amplitudePoints = amplitudes, .frequencyPointCount = 2, .frequencyPoints = frequencies, .minFrequencyHz = 100.0f, .maxFrequencyHz = 200.0f };
    ASSERT_EQ(apply(vibration, hands[0]), XR_SUCCESS);
    const SIM_Sim*   sim   = oxr_get_sim();
    const SIM_Haptic start_state = sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main);
    EXPECT_TRUE(start_state.active);
    EXPECT_FLOAT_EQ(start_state.amplitude, 0.0f);
    EXPECT_FLOAT_EQ(start_state.frequency, 100.0f);
    frame();
    frame();
    EXPECT_GT(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).amplitude, 0.0f);
    EXPECT_GT(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).frequency, 100.0f);
    const XrHapticActionInfo info = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hands[0] };
    EXPECT_EQ(xr.xrStopHapticFeedback(session, &info), XR_SUCCESS);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);
}

TEST_F(OxrHapticParametricTest, streams_need_a_first_frame)
{
    start();
    const XrHapticParametricPointEXT amplitudes[2] = { { 0, 0.5f }, { 20000000, 0.5f } };
    const XrHapticParametricPointEXT later[1]      = { { 40000000, 0.25f } };
    XrHapticParametricVibrationEXT   vibration     = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 1, .amplitudePoints = later, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_INTERMEDIATE_FRAME_EXT };
    EXPECT_EQ(apply(vibration, hands[1]), XR_ERROR_VALIDATION_FAILURE) << "no stream runs";
    vibration = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 2, .amplitudePoints = amplitudes, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_FIRST_FRAME_EXT };
    ASSERT_EQ(apply(vibration, hands[1]), XR_SUCCESS);
    const SIM_Sim* sim = oxr_get_sim();
    EXPECT_TRUE(sim_haptic_stream_running(sim, SIM_Device::RightHand, SIM_HapticOutput::Main));
    vibration = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 1, .amplitudePoints = later, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_INTERMEDIATE_FRAME_EXT };
    EXPECT_EQ(apply(vibration, hands[1]), XR_SUCCESS);
    vibration.minFrequencyHz = 100.0f;
    vibration.maxFrequencyHz = 200.0f;
    EXPECT_EQ(apply(vibration, hands[1]), XR_ERROR_VALIDATION_FAILURE) << "only first frames set the range";
    vibration.minFrequencyHz  = 0.0f;
    vibration.maxFrequencyHz  = 0.0f;
    vibration.streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_LAST_FRAME_EXT;
    EXPECT_EQ(apply(vibration, hands[1]), XR_SUCCESS);
    EXPECT_FALSE(sim_haptic_stream_running(sim, SIM_Device::RightHand, SIM_HapticOutput::Main));
}

// CTS XR_EXT_haptic_parametric "Frequency point after last amplitude point in first frame"
TEST_F(OxrHapticParametricTest, stream_frames_may_reach_past_their_amplitudes)
{
    start();
    const XrHapticParametricPointEXT     amplitudes[2]  = { { 0, 0.0f }, { 50000000, 1.0f } };
    const XrHapticParametricPointEXT     frequencies[3] = { { 0, 0.0f }, { 50000000, 0.5f }, { 100000000, 1.0f } };
    const XrHapticParametricTransientEXT transients[1]  = { { 100000000, 1.0f, 1.0f } };
    const XrHapticParametricPointEXT     end[1]         = { { 150000000, 0.0f } };
    const XrHapticParametricPointEXT     past_end[1]    = { { 200000000, 1.0f } };
    XrHapticParametricVibrationEXT       vibration      = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 2, .amplitudePoints = amplitudes, .frequencyPointCount = 3, .frequencyPoints = frequencies, .transientCount = 1, .transients = transients, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_FIRST_FRAME_EXT };
    ASSERT_EQ(apply(vibration, hands[1]), XR_SUCCESS);
    vibration = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 1, .amplitudePoints = end, .frequencyPointCount = 1, .frequencyPoints = past_end, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_LAST_FRAME_EXT };
    EXPECT_EQ(apply(vibration, hands[1]), XR_ERROR_VALIDATION_FAILURE) << "the last frame ends with the amplitudes";
    vibration.frequencyPointCount = 0;
    vibration.streamFrameType     = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_FIRST_FRAME_EXT;
    vibration.amplitudePointCount = 2;
    vibration.amplitudePoints     = amplitudes;
    ASSERT_EQ(apply(vibration, hands[1]), XR_SUCCESS);
    vibration = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 1, .amplitudePoints = end, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_LAST_FRAME_EXT };
    EXPECT_EQ(apply(vibration, hands[1]), XR_SUCCESS);
}

// CTS XR_EXT_haptic_parametric "... out of order across frames", "Transients at same time across frames"
TEST_F(OxrHapticParametricTest, frames_continue_their_stream_in_time)
{
    start();
    const XrHapticParametricPointEXT     amplitudes[2] = { { 0, 0.0f }, { 50000000, 1.0f } };
    const XrHapticParametricTransientEXT at_end[1]     = { { 50000000, 1.0f, 1.0f } };
    const XrHapticParametricPointEXT     earlier[1]    = { { 25000000, 1.0f } };
    const XrHapticParametricPointEXT     later[1]      = { { 100000000, 1.0f } };
    const XrHapticParametricTransientEXT same[1]       = { { 50000000, 1.0f, 1.0f } };
    const XrHapticParametricTransientEXT next[1]       = { { 75000000, 1.0f, 1.0f } };
    XrHapticParametricVibrationEXT       vibration     = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 2, .amplitudePoints = amplitudes, .frequencyPointCount = 2, .frequencyPoints = amplitudes, .transientCount = 1, .transients = at_end, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_FIRST_FRAME_EXT };
    ASSERT_EQ(apply(vibration, hands[1]), XR_SUCCESS);
    const XrHapticParametricVibrationEXT frame = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 1, .amplitudePoints = later, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_INTERMEDIATE_FRAME_EXT };
    vibration                 = frame;
    vibration.amplitudePoints = earlier;
    EXPECT_EQ(apply(vibration, hands[1]), XR_ERROR_VALIDATION_FAILURE) << "amplitudes go back in time";
    vibration                     = frame;
    vibration.frequencyPointCount = 1;
    vibration.frequencyPoints     = earlier;
    EXPECT_EQ(apply(vibration, hands[1]), XR_ERROR_VALIDATION_FAILURE) << "frequencies go back in time";
    vibration                = frame;
    vibration.transientCount = 1;
    vibration.transients     = same;
    EXPECT_EQ(apply(vibration, hands[1]), XR_ERROR_VALIDATION_FAILURE) << "transients come strictly later";
    vibration.transients = next;
    EXPECT_EQ(apply(vibration, hands[1]), XR_SUCCESS);

    // The frequencies of earlier frames end with the amplitudes of the last one too
    const XrHapticParametricPointEXT long_frequencies[2] = { { 0, 0.0f }, { 200000000, 1.0f } };
    vibration = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 2, .amplitudePoints = amplitudes, .frequencyPointCount = 2, .frequencyPoints = long_frequencies, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_FIRST_FRAME_EXT };
    ASSERT_EQ(apply(vibration, hands[1]), XR_SUCCESS);
    vibration                 = frame;
    vibration.streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_LAST_FRAME_EXT;
    EXPECT_EQ(apply(vibration, hands[1]), XR_ERROR_VALIDATION_FAILURE);
}

// CTS XR_EXT_haptic_parametric "Incorrect frame type": the stream the rejected last frames of
// the cases before it meant to end runs no more
TEST_F(OxrHapticParametricTest, a_rejected_last_frame_ends_the_stream)
{
    start();
    const XrHapticParametricPointEXT     amplitudes[2] = { { 0, 0.0f }, { 50000000, 1.0f } };
    const XrHapticParametricPointEXT     end[1]        = { { 100000000, 1.0f } };
    const XrHapticParametricTransientEXT late[1]       = { { 150000000, 1.0f, 1.0f } };
    XrHapticParametricVibrationEXT       vibration     = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 2, .amplitudePoints = amplitudes, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_FIRST_FRAME_EXT };
    ASSERT_EQ(apply(vibration, hands[1]), XR_SUCCESS);
    const SIM_Sim* sim = oxr_get_sim();
    ASSERT_TRUE(sim_haptic_stream_running(sim, SIM_Device::RightHand, SIM_HapticOutput::Main));
    vibration = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 1, .amplitudePoints = end, .transientCount = 1, .transients = late, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_LAST_FRAME_EXT };
    EXPECT_EQ(apply(vibration, hands[1]), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_FALSE(sim_haptic_stream_running(sim, SIM_Device::RightHand, SIM_HapticOutput::Main));
    vibration = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 2, .amplitudePoints = amplitudes, .streamFrameType = XR_HAPTIC_PARAMETRIC_STREAM_FRAME_TYPE_INTERMEDIATE_FRAME_EXT };
    EXPECT_EQ(apply(vibration, hands[1]), XR_ERROR_VALIDATION_FAILURE) << "no stream runs";
}

TEST_F(OxrHapticParametricTest, vibrations_are_validated)
{
    start();
    const XrHapticParametricPointEXT     good[2]     = { { 0, 0.5f }, { 20000000, 0.5f } };
    const XrHapticParametricPointEXT     late[2]     = { { 1, 0.5f }, { 20000000, 0.5f } };
    const XrHapticParametricPointEXT     backwards[2] = { { 0, 0.5f }, { -5, 0.5f } };
    const XrHapticParametricPointEXT     loud[2]     = { { 0, 0.5f }, { 20000000, 1.5f } };
    const XrHapticParametricPointEXT     long_freq[2] = { { 0, 0.5f }, { 30000000, 0.5f } };
    const XrHapticParametricTransientEXT twice[2]    = { { 5000000, 1.0f, 1.0f }, { 5000000, 1.0f, 1.0f } };
    static XrHapticParametricPointEXT    many[XR_HAPTIC_PARAMETRIC_MAX_POINTS_TRANSIENTS_EXT + 1];
    for (u32 i = 0; i < ARRAY_SIZE(many); i++)
    {
        many[i] = { (XrDuration)i * 1000, 0.5f };
    }
    const XrHapticParametricVibrationEXT base = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 2, .amplitudePoints = good };
    XrHapticParametricVibrationEXT       v    = base;
    EXPECT_EQ(apply(v, hands[0]), XR_SUCCESS);
    v.amplitudePointCount = 1;
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE) << "an event needs two amplitude points";
    v = base;
    v.amplitudePoints = late;
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE) << "an event starts at 0";
    v.amplitudePoints = backwards;
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE);
    v.amplitudePoints = loud;
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE);
    v.amplitudePoints     = many;
    v.amplitudePointCount = ARRAY_SIZE(many);
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE);
    v = base;
    v.frequencyPointCount = 2;
    v.frequencyPoints     = long_freq;
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE) << "frequencies end with the amplitudes";
    v = base;
    v.transientCount = 2;
    v.transients     = twice;
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE) << "transients strictly increase";
    v = base;
    v.minFrequencyHz = 300.0f;
    v.maxFrequencyHz = 200.0f;
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE);
    v.minFrequencyHz = 0.0f;
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE) << "both or neither are unspecified";
    v.minFrequencyHz = 100.0f;
    v.maxFrequencyHz = 2000.0f;
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE);
    v = base;
    v.streamFrameType = (XrHapticParametricStreamFrameTypeEXT)7;
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE);
    v = base;
    v.amplitudePoints = nullptr;
    EXPECT_EQ(apply(v, hands[0]), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrHapticParametricPreviewTest, the_preview_extension_is_the_same)
{
    start("xrHapticParametricGetPropertiesEXTX1");
    XrHapticParametricPropertiesEXT properties = { .type = XR_TYPE_HAPTIC_PARAMETRIC_PROPERTIES_EXT };
    const XrHapticActionInfo        info       = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hands[0] };
    ASSERT_EQ(get_properties(session, &info, &properties), XR_SUCCESS);
    const XrHapticParametricPointEXT     amplitudes[2] = { { 0, 0.5f }, { 20000000, 0.5f } };
    const XrHapticParametricVibrationEXT vibration     = { .type = XR_TYPE_HAPTIC_PARAMETRIC_VIBRATION_EXT, .amplitudePointCount = 2, .amplitudePoints = amplitudes };
    EXPECT_EQ(apply(vibration, hands[0]), XR_SUCCESS);
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrHapticParametricGetPropertiesEXT", &fn), XR_ERROR_FUNCTION_UNSUPPORTED) << "the names follow the enabled extension";
}

//
// Display and system: XR_FB_display_refresh_rate, XR_FB_color_space, XR_META_headset_id,
// XR_META_local_dimming, XR_META_tile_properties_hint, XR_META_recommended_layer_resolution
//

namespace
{
struct OxrDisplayTest : OxrTest
{
    OxrDisplayTest ()
    {
        more_extensions[more_extensions_count++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_FB_COLOR_SPACE_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_META_HEADSET_ID_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_META_LOCAL_DIMMING_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_META_TILE_PROPERTIES_HINT_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrTest::SetUp();
        create_headless_instance();
        create_session();
    }

    template <typename T>
    T function (const char* name) const
    {
        T fn = nullptr;
        EXPECT_EQ(gipa(instance, name, (PFN_xrVoidFunction*)&fn), XR_SUCCESS) << name;
        return fn;
    }
};
} // namespace

TEST_F(OxrDisplayTest, refresh_rates_can_be_listed_and_requested)
{
    begin_session();
    const PFN_xrEnumerateDisplayRefreshRatesFB enumerate = function<PFN_xrEnumerateDisplayRefreshRatesFB>("xrEnumerateDisplayRefreshRatesFB");
    const PFN_xrGetDisplayRefreshRateFB        get       = function<PFN_xrGetDisplayRefreshRateFB>("xrGetDisplayRefreshRateFB");
    const PFN_xrRequestDisplayRefreshRateFB    request   = function<PFN_xrRequestDisplayRefreshRateFB>("xrRequestDisplayRefreshRateFB");
    ASSERT_TRUE(enumerate && get && request);

    u32 count = 0;
    ASSERT_EQ(enumerate(session, 0, &count, nullptr), XR_SUCCESS);
    ASSERT_EQ(count, 4u);
    f32 rates[4] = {};
    u32 small    = 0;
    EXPECT_EQ(enumerate(session, 2, &small, rates), XR_ERROR_SIZE_INSUFFICIENT);
    ASSERT_EQ(enumerate(session, 4, &count, rates), XR_SUCCESS);
    EXPECT_FLOAT_EQ(rates[0], 72.0f);
    EXPECT_FLOAT_EQ(rates[3], 120.0f);
    EXPECT_EQ(enumerate(session, 0, nullptr, nullptr), XR_ERROR_VALIDATION_FAILURE);

    f32 rate = 0.0f;
    ASSERT_EQ(get(session, &rate), XR_SUCCESS);
    EXPECT_FLOAT_EQ(rate, 90.0f);
    EXPECT_EQ(get(session, nullptr), XR_ERROR_VALIDATION_FAILURE);

    drain_events();
    ASSERT_EQ(request(session, 120.0f), XR_SUCCESS);
    XrEventDataBuffer event = {};
    ASSERT_TRUE(poll_for(XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB, event));
    const XrEventDataDisplayRefreshRateChangedFB* changed = (const XrEventDataDisplayRefreshRateChangedFB*)&event;
    EXPECT_FLOAT_EQ(changed->fromDisplayRefreshRate, 90.0f);
    EXPECT_FLOAT_EQ(changed->toDisplayRefreshRate, 120.0f);
    EXPECT_EQ(frame().predictedDisplayPeriod, 8333333);
    ASSERT_EQ(get(session, &rate), XR_SUCCESS);
    EXPECT_FLOAT_EQ(rate, 120.0f);

    // The same rate again changes nothing; 0 returns to the configured rate (spec 12.114)
    ASSERT_EQ(request(session, 120.0f), XR_SUCCESS);
    EXPECT_FALSE(poll_for(XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB, event));
    ASSERT_EQ(request(session, 0.0f), XR_SUCCESS);
    ASSERT_TRUE(poll_for(XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB, event));
    EXPECT_FLOAT_EQ(changed->toDisplayRefreshRate, 90.0f);
    EXPECT_EQ(request(session, 75.0f), XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB);
    EXPECT_EQ(request(session, zm::nan()), XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB);

    // Recordings keep one rate, so it cannot change while one is written
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_refresh_rate.xrrec", temp_dir());
    ASSERT_TRUE(oxr_record_start(path));
    EXPECT_EQ(request(session, 72.0f), XR_ERROR_FEATURE_UNSUPPORTED);
    ASSERT_TRUE(oxr_record_stop());
    os_delete_file(path);
    EXPECT_EQ(request(session, 72.0f), XR_SUCCESS);
}

TEST_F(OxrDisplayTest, color_space_headset_id_dimming_and_tile_hints)
{
    XrSystemHeadsetIdPropertiesMETA  headset = { .type = XR_TYPE_SYSTEM_HEADSET_ID_PROPERTIES_META };
    XrSystemColorSpacePropertiesFB   color   = { .type = XR_TYPE_SYSTEM_COLOR_SPACE_PROPERTIES_FB, .next = &headset };
    XrSystemProperties               props   = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &color };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(color.colorSpace, XR_COLOR_SPACE_REC709_FB);
    const XrUuidEXT empty = {};
    EXPECT_NE(memcmp(&headset.id, &empty, sizeof(empty)), 0);
    // OVRPlugin knows a Quest 3 by this id of Meta's runtime; the default preset is one
    const u8 quest3[XR_UUID_SIZE_EXT] = { 0x64, 0x09, 0xe3, 0xb3, 0x50, 0x89, 0x4e, 0xd6, 0x86, 0x8e, 0xaa, 0xed, 0x82, 0xda, 0x36, 0x76 };
    EXPECT_EQ(memcmp(headset.id.data, quest3, sizeof(quest3)), 0);
    XrSystemHeadsetIdPropertiesMETA again = { .type = XR_TYPE_SYSTEM_HEADSET_ID_PROPERTIES_META };
    props.next = &again;
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(memcmp(&headset.id, &again.id, sizeof(empty)), 0);

    const PFN_xrEnumerateColorSpacesFB enumerate = function<PFN_xrEnumerateColorSpacesFB>("xrEnumerateColorSpacesFB");
    const PFN_xrSetColorSpaceFB        set       = function<PFN_xrSetColorSpaceFB>("xrSetColorSpaceFB");
    ASSERT_TRUE(enumerate && set);
    u32 count = 0;
    ASSERT_EQ(enumerate(session, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 8u);
    XrColorSpaceFB spaces[8] = {};
    ASSERT_EQ(enumerate(session, 8, &count, spaces), XR_SUCCESS);
    EXPECT_EQ(spaces[0], XR_COLOR_SPACE_UNMANAGED_FB);
    EXPECT_EQ(set(session, XR_COLOR_SPACE_REC2020_FB), XR_SUCCESS);
    EXPECT_EQ(set(session, XR_COLOR_SPACE_QUEST_FB), XR_SUCCESS);
    EXPECT_EQ(set(session, (XrColorSpaceFB)99), XR_ERROR_COLOR_SPACE_UNSUPPORTED_FB);

    const PFN_xrSetTilePropertiesHintMETA tiles = function<PFN_xrSetTilePropertiesHintMETA>("xrSetTilePropertiesHintMETA");
    ASSERT_TRUE(tiles);
    XrTilePropertiesMETA     tile = { .type = XR_TYPE_TILE_PROPERTIES_META, .tileDimensions = { 32, 32, 1 } };
    XrTilePropertiesHintMETA hint = { .type = XR_TYPE_TILE_PROPERTIES_HINT_META, .propertiesCount = 1, .properties = &tile };
    EXPECT_EQ(tiles(session, &hint), XR_SUCCESS);
    hint.propertiesCount = 0;
    EXPECT_EQ(tiles(session, &hint), XR_ERROR_VALIDATION_FAILURE);
    hint.propertiesCount = 1;
    tile.type            = XR_TYPE_TILE_PROPERTIES_HINT_META; // as the CTS sends it
    EXPECT_EQ(tiles(session, &hint), XR_SUCCESS);
    tile.type = XR_TYPE_SPACE_LOCATION;
    EXPECT_EQ(tiles(session, &hint), XR_ERROR_VALIDATION_FAILURE);
    hint.properties = nullptr;
    EXPECT_EQ(tiles(session, &hint), XR_ERROR_VALIDATION_FAILURE);

    begin_session();
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
    XrLocalDimmingFrameEndInfoMETA dimming  = { .type = XR_TYPE_LOCAL_DIMMING_FRAME_END_INFO_META, .localDimmingMode = XR_LOCAL_DIMMING_MODE_ON_META };
    XrFrameEndInfo                 end_info = { .type = XR_TYPE_FRAME_END_INFO, .next = &dimming, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
    dimming.localDimmingMode = (XrLocalDimmingModeMETA)7;
    end_info.displayTime     = state.predictedDisplayTime;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_VALIDATION_FAILURE);
    end_info.next = nullptr;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
}

TEST_F(OxrTest, display_extensions_need_enabling)
{
    create_headless_instance();
    XrSystemHeadsetIdPropertiesMETA headset = { .type = XR_TYPE_SYSTEM_HEADSET_ID_PROPERTIES_META };
    XrSystemProperties              props   = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &headset };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    const XrUuidEXT empty = {};
    EXPECT_EQ(memcmp(&headset.id, &empty, sizeof(empty)), 0);
    const char* const names[] = { "xrEnumerateDisplayRefreshRatesFB", "xrGetDisplayRefreshRateFB", "xrRequestDisplayRefreshRateFB", "xrEnumerateColorSpacesFB", "xrSetColorSpaceFB", "xrSetTilePropertiesHintMETA", "xrGetRecommendedLayerResolutionMETA" };
    for (const char* name : names)
    {
        PFN_xrVoidFunction fn = nullptr;
        EXPECT_EQ(gipa(instance, name, &fn), XR_ERROR_FUNCTION_UNSUPPORTED) << name;
    }
}

namespace
{
struct OxrLayerResolutionTest : OxrVulkanTest
{
    OxrLayerResolutionTest ()
    {
        extensions[extensions_count++] = XR_META_RECOMMENDED_LAYER_RESOLUTION_EXTENSION_NAME;
    }
};
} // namespace

// The simulated GPU never needs to scale down, so the recommendation is the size submitted
TEST_F(OxrLayerResolutionTest, recommended_resolution_is_the_sub_image)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    PFN_xrGetRecommendedLayerResolutionMETA recommend = nullptr;
    ASSERT_EQ(gipa(instance, "xrGetRecommendedLayerResolutionMETA", (PFN_xrVoidFunction*)&recommend), XR_SUCCESS);
    const XrSwapchain swapchain  = create_swapchain(64, 48, 2);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });

    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    XrCompositionLayerProjectionView        views[2]   = { view(swapchain, 0, 64, 48), view(swapchain, 1, 64, 48) };
    views[0].subImage.imageRect.extent                 = { 40, 30 };
    const XrCompositionLayerProjection      projection = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = view_space, .viewCount = 2, .views = views };
    XrRecommendedLayerResolutionGetInfoMETA info       = { .type = XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META, .layer = (const XrCompositionLayerBaseHeader*)&projection, .predictedDisplayTime = state.predictedDisplayTime };
    XrRecommendedLayerResolutionMETA        result     = { .type = XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META };
    ASSERT_EQ(recommend(session, &info, &result), XR_SUCCESS);
    EXPECT_TRUE(result.isValid);
    EXPECT_EQ(result.recommendedImageDimensions.width, 40);
    EXPECT_EQ(result.recommendedImageDimensions.height, 30);

    const XrCompositionLayerQuad quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 16, 8 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 1, 1 } };
    info.layer = (const XrCompositionLayerBaseHeader*)&quad;
    ASSERT_EQ(recommend(session, &info, &result), XR_SUCCESS);
    EXPECT_EQ(result.recommendedImageDimensions.width, 16);
    EXPECT_EQ(result.recommendedImageDimensions.height, 8);

    info.predictedDisplayTime = state.predictedDisplayTime - 1;
    EXPECT_EQ(recommend(session, &info, &result), XR_ERROR_TIME_INVALID);
    info.predictedDisplayTime = state.predictedDisplayTime;
    info.layer                = nullptr;
    EXPECT_EQ(recommend(session, &info, &result), XR_ERROR_VALIDATION_FAILURE);
    const XrSpaceLocation not_a_layer = { .type = XR_TYPE_SPACE_LOCATION };
    info.layer                        = (const XrCompositionLayerBaseHeader*)&not_a_layer;
    EXPECT_EQ(recommend(session, &info, &result), XR_ERROR_LAYER_INVALID);
    EXPECT_EQ(recommend(session, nullptr, &result), XR_ERROR_VALIDATION_FAILURE);
}

// Meta's Unreal plugin asks on its RHI thread with the display time of the frame it submits,
// while the game thread has already waited for the next; it logs an ensure for each refusal
struct OxrLayerResolutionOlderFrameTest : OxrLayerResolutionTest
{
    void SetUp () override
    {
        os_set_env("PUPPETXR_ACCEPT_OLDER_FRAME_TIME", "1");
        OxrLayerResolutionTest::SetUp();
        os_unset_env("PUPPETXR_ACCEPT_OLDER_FRAME_TIME");
    }
};

static XrResult recommend_for_frames (OxrLayerResolutionTest& test, XrResult& ended_result)
{
    PFN_xrGetRecommendedLayerResolutionMETA recommend = nullptr;
    EXPECT_EQ(test.gipa(test.instance, "xrGetRecommendedLayerResolutionMETA", (PFN_xrVoidFunction*)&recommend), XR_SUCCESS);
    const XrSwapchain swapchain  = test.create_swapchain(64, 48, 2);
    const XrSpace     view_space = test.reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    test.render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });

    XrFrameState submitted = { .type = XR_TYPE_FRAME_STATE };
    EXPECT_EQ(test.xr.xrWaitFrame(test.session, nullptr, &submitted), XR_SUCCESS);
    EXPECT_EQ(test.xr.xrBeginFrame(test.session, nullptr), XR_SUCCESS);
    XrFrameState next = { .type = XR_TYPE_FRAME_STATE };
    EXPECT_EQ(test.xr.xrWaitFrame(test.session, nullptr, &next), XR_SUCCESS);
    EXPECT_GT(next.predictedDisplayTime, submitted.predictedDisplayTime);

    XrCompositionLayerProjectionView        views[2]   = { test.view(swapchain, 0, 64, 48), test.view(swapchain, 1, 64, 48) };
    const XrCompositionLayerProjection      projection = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = view_space, .viewCount = 2, .views = views };
    const XrRecommendedLayerResolutionGetInfoMETA info = { .type = XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META, .layer = (const XrCompositionLayerBaseHeader*)&projection, .predictedDisplayTime = submitted.predictedDisplayTime };
    XrRecommendedLayerResolutionMETA        result     = { .type = XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META };
    const XrResult                          older      = recommend(test.session, &info, &result);

    const XrCompositionLayerBaseHeader* layers[] = { (const XrCompositionLayerBaseHeader*)&projection };
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = submitted.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = layers };
    EXPECT_EQ(test.xr.xrEndFrame(test.session, &end_info), XR_SUCCESS);
    ended_result = recommend(test.session, &info, &result);
    return older;
}

TEST_F(OxrLayerResolutionTest, a_frame_older_than_the_last_waited_is_invalid)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    XrResult ended = XR_SUCCESS;
    EXPECT_EQ(recommend_for_frames(*this, ended), XR_ERROR_TIME_INVALID);
    EXPECT_EQ(ended, XR_ERROR_TIME_INVALID);
}

TEST_F(OxrLayerResolutionOlderFrameTest, accept_older_frame_time_answers_for_a_frame_not_yet_ended)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    XrResult ended = XR_SUCCESS;
    EXPECT_EQ(recommend_for_frames(*this, ended), XR_SUCCESS);
    EXPECT_EQ(ended, XR_ERROR_TIME_INVALID) << "an ended frame stays invalid";
}

//
// Record and replay
//

static void test_path (char* out, usize size, const char* name)
{
    cstr_format(out, size, "%s/%s", temp_dir(), name);
}

namespace
{

struct OxrRecordTest : OxrActionTest
{
    static constexpr u32 FRAMES = 60;

    struct Observed
    {
        u64 hash;
        u64 frame;
        f32 left_trigger;
        b8  right_click;
        f32 grip_x;
    };

    XrSpace grip_space = XR_NULL_HANDLE;
    XrSpace stage      = XR_NULL_HANDLE;

    void SetUp () override
    {
        OxrActionTest::SetUp();
    }

    void restart ()
    {
        xr.xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        session  = XR_NULL_HANDLE;
        xr       = load(gipa, XR_NULL_HANDLE);
        OxrActionTest::SetUp();
    }

    void prepare ()
    {
        suggest_touch_bindings();
        attach();
        begin_session();
        const XrActionSpaceCreateInfo info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = grip, .subactionPath = hands[0], .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
        ASSERT_EQ(xr.xrCreateActionSpace(session, &info, &grip_space), XR_SUCCESS);
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    }

    // The scripted input stands in for a user at the keyboard; it only acts while recording
    void script (u32 f)
    {
        SIM_Sim* sim = oxr_get_sim();
        sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue, { .value = { (f32)(f % 11) / 10.0f, 0.0f } });
        sim_set_input(sim, SIM_Device::RightHand, SIM_Input::AClick, { .value = { (f % 7) < 3 ? 1.0f : 0.0f, 0.0f } });
        sim_set_device_pose(sim, SIM_Device::LeftHand, { .position = { -0.2f + 0.01f * (f32)f, 1.3f, -0.3f }, .orientation = zm::quat_from_euler({ 0.0f, 0.02f * (f32)f, 0.0f }) });
        if (f == 20)
        {
            sim_recenter(sim);
        }
    }

    Observed observe (const XrFrameState& state)
    {
        EXPECT_EQ(sync(), XR_SUCCESS);
        XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
        EXPECT_EQ(xr.xrLocateSpace(grip_space, stage, state.predictedDisplayTime, &location), XR_SUCCESS);
        return {
            .hash         = sim_state_hash(oxr_get_sim()),
            .frame        = sim_frame_index(oxr_get_sim()),
            .left_trigger = float_state(trigger, hands[0]).currentState,
            .right_click  = bool_state(click, hands[1]).currentState != XR_FALSE,
            .grip_x       = location.pose.position.x,
        };
    }

    void run (Observed* out, u32 frames, b8 scripted)
    {
        for (u32 f = 0; f < frames; f++)
        {
            if (scripted)
            {
                script(f);
            }
            out[f] = observe(frame());
        }
    }
};

} // namespace

static void expect_same (const OxrRecordTest::Observed* a, const OxrRecordTest::Observed* b, u32 count)
{
    for (u32 f = 0; f < count; f++)
    {
        EXPECT_EQ(a[f].hash, b[f].hash) << "frame " << f;
        EXPECT_EQ(a[f].frame, b[f].frame) << "frame " << f;
        EXPECT_EQ(a[f].left_trigger, b[f].left_trigger) << "frame " << f;
        EXPECT_EQ(a[f].right_click, b[f].right_click) << "frame " << f;
        EXPECT_EQ(a[f].grip_x, b[f].grip_x) << "frame " << f;
    }
}

TEST_F(OxrRecordTest, replay_twice_reproduces_every_frame)
{
    char path[OS_MAX_PATH];
    test_path(path, sizeof(path), "puppetxr_test_session.xrrec");

    os_set_env("PUPPETXR_RECORD", path);
    restart();
    prepare();
    Observed recorded[FRAMES];
    run(recorded, FRAMES, true);
    EXPECT_TRUE(oxr_record_status().recording);
    EXPECT_EQ(oxr_record_status().recorded_frames, (u64)FRAMES);
    os_unset_env("PUPPETXR_RECORD");

    for (u32 replay = 0; replay < 2; replay++)
    {
        os_set_env("PUPPETXR_REPLAY", path);
        restart();
        prepare();
        EXPECT_TRUE(oxr_record_status().replaying);
        Observed replayed[FRAMES];
        run(replayed, FRAMES, false);
        expect_same(recorded, replayed, FRAMES);

        const OXR_RecordStatus status = oxr_record_status();
        EXPECT_EQ(status.divergences, 0u) << "replay " << replay;
        EXPECT_TRUE(status.replay_complete);
        EXPECT_EQ(status.replay_frame, (u64)FRAMES);
        os_unset_env("PUPPETXR_REPLAY");
    }
}

// The drift a recording saw, steps and a steady rate, replays bit for bit, also where the
// replaying instance has a drift rate of its own
TEST_F(OxrRecordTest, tracking_drift_replays_bit_identically)
{
    char path[OS_MAX_PATH];
    test_path(path, sizeof(path), "puppetxr_test_session_drift.xrrec");
    const u32 frames = 30;
    XrPosef   recorded[frames] = {};
    XrPosef   replayed[frames] = {};

    os_set_env("PUPPETXR_RECORD", path);
    restart();
    prepare();
    const XrSpace view = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    for (u32 f = 0; f < frames; f++)
    {
        if (f == 5)
        {
            ASSERT_TRUE(sim_set_tracking_drift(oxr_get_sim(), { { 0.1f, 0.0f, -0.05f }, zm::quat_from_axis_angle({ 0, 1, 0 }, 0.2f) }));
        }
        if (f == 12)
        {
            ASSERT_TRUE(sim_set_tracking_drift_rate(oxr_get_sim(), { 0.02f, 0.0f, 0.01f }, 4.0f));
        }
        XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
        ASSERT_EQ(xr.xrLocateSpace(view, stage, frame().predictedDisplayTime, &location), XR_SUCCESS);
        recorded[f] = location.pose;
    }
    os_unset_env("PUPPETXR_RECORD");
    EXPECT_FALSE(memory_equal(&recorded[frames - 1], &recorded[frames - 2], sizeof(XrPosef))) << "the rate drifts every frame";

    os_set_env("PUPPETXR_REPLAY", path);
    os_set_env("PUPPETXR_DRIFT_RATE", "-10,1,0,1");
    restart();
    prepare();
    os_unset_env("PUPPETXR_DRIFT_RATE");
    const XrSpace replay_view = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    for (u32 f = 0; f < frames; f++)
    {
        XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
        ASSERT_EQ(xr.xrLocateSpace(replay_view, stage, frame().predictedDisplayTime, &location), XR_SUCCESS);
        replayed[f] = location.pose;
    }
    os_unset_env("PUPPETXR_REPLAY");
    for (u32 f = 0; f < frames; f++)
    {
        EXPECT_TRUE(memory_equal(&recorded[f], &replayed[f], sizeof(XrPosef))) << "frame " << f;
    }
    EXPECT_EQ(oxr_record_status().divergences, 0u);
}

TEST_F(OxrRecordTest, replay_end_requests_exit)
{
    char path[OS_MAX_PATH];
    test_path(path, sizeof(path), "puppetxr_test_exit.xrrec");
    os_set_env("PUPPETXR_RECORD", path);
    restart();
    prepare();
    Observed recorded[10];
    run(recorded, 10, true);
    os_unset_env("PUPPETXR_RECORD");

    os_set_env("PUPPETXR_REPLAY", path);
    restart();
    prepare();
    Observed replayed[11];
    run(replayed, 10, false);
    EXPECT_FALSE(oxr_record_status().replay_finished);
    frame();
    EXPECT_TRUE(oxr_record_status().replay_finished);
    EXPECT_EQ(drain_events(), XR_SESSION_STATE_STOPPING);
    EXPECT_TRUE(sim_controls_enabled(oxr_get_sim()));
}

TEST_F(OxrRecordTest, replay_without_exit_continues_live)
{
    char path[OS_MAX_PATH];
    test_path(path, sizeof(path), "puppetxr_test_continue.xrrec");
    os_set_env("PUPPETXR_RECORD", path);
    restart();
    prepare();
    Observed recorded[5];
    run(recorded, 5, true);
    os_unset_env("PUPPETXR_RECORD");

    os_set_env("PUPPETXR_REPLAY", path);
    os_set_env("PUPPETXR_REPLAY_EXIT", "0");
    restart();
    prepare();
    Observed replayed[8];
    run(replayed, 8, false);
    EXPECT_TRUE(oxr_record_status().replay_finished);
    EXPECT_NE(drain_events(), XR_SESSION_STATE_STOPPING);
    EXPECT_GT(replayed[7].frame, replayed[5].frame);
}

TEST_F(OxrRecordTest, mid_session_recording_replays_through_the_api)
{
    prepare();
    Observed warmup[10];
    run(warmup, 10, true);

    char path[OS_MAX_PATH];
    test_path(path, sizeof(path), "puppetxr_test_mid.xrrec");
    ASSERT_TRUE(oxr_record_start(path));
    EXPECT_FALSE(oxr_record_start(path));
    Observed recorded[20];
    for (u32 f = 0; f < 20; f++)
    {
        script(f + 10);
        recorded[f] = observe(frame());
    }
    ASSERT_TRUE(oxr_record_stop());
    EXPECT_FALSE(oxr_record_stop());

    restart();
    prepare();
    ASSERT_TRUE(oxr_replay_start(path));
    Observed replayed[20];
    run(replayed, 20, false);
    expect_same(recorded, replayed, 20);
    EXPECT_EQ(oxr_record_status().divergences, 0u);
}

TEST_F(OxrRecordTest, divergence_is_reported)
{
    char path[OS_MAX_PATH];
    test_path(path, sizeof(path), "puppetxr_test_diverge.xrrec");

    SIM_Config config = {};
    sim_default_config(config);
    SIM_Sim* sim = sim_create(config);
    SIM_Snapshot initial = {};
    sim_capture_snapshot(sim, initial);
    REC_Header header = {};
    rec_header_from_config(config, initial, header);
    REC_Writer* writer = rec_writer_create(path, header);
    ASSERT_NE(writer, nullptr);
    for (u32 f = 0; f < 6; f++)
    {
        sim_begin_frame(sim, 0);
        sim_commit_frame(sim);
        SIM_FrameState state = {};
        sim_capture_state(sim, state);
        const u64 hash = f == 3 ? 12345 : sim_state_hash(sim);
        rec_writer_append(writer, sim_frame_index(sim), hash, state);
    }
    ASSERT_TRUE(rec_writer_close(writer));
    sim_destroy(sim);

    os_set_env("PUPPETXR_REPLAY", path);
    restart();
    prepare();
    Observed replayed[6];
    run(replayed, 6, false);
    const OXR_RecordStatus status = oxr_record_status();
    EXPECT_EQ(status.divergences, 1u);
    EXPECT_EQ(status.first_divergence, 4u);
}

TEST_F(OxrRecordTest, bad_replays_are_rejected)
{
    char path[OS_MAX_PATH];
    test_path(path, sizeof(path), "puppetxr_test_garbage.xrrec");
    ASSERT_TRUE(os_write_file(path, "not a recording", 15));

    xr.xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;
    session  = XR_NULL_HANDLE;
    os_set_env("PUPPETXR_REPLAY", path);
    xr = load(gipa, XR_NULL_HANDLE);
    const char* extensions[] = { XR_MND_HEADLESS_EXTENSION_NAME };
    EXPECT_EQ(create_instance(extensions, 1), XR_ERROR_RUNTIME_FAILURE);
    os_unset_env("PUPPETXR_REPLAY");
    EXPECT_EQ(create_instance(extensions, 1), XR_SUCCESS);
    EXPECT_FALSE(oxr_replay_start(path));
    EXPECT_FALSE(oxr_replay_start("/nonexistent.xrrec"));

    // A recording made at another resolution cannot be applied to a running instance
    char other[OS_MAX_PATH];
    test_path(other, sizeof(other), "puppetxr_test_other_res.xrrec");
    SIM_Config config = {};
    sim_default_config(config);
    config.view_width = 640;
    SIM_Sim*     sim = sim_create(config);
    SIM_Snapshot initial = {};
    sim_capture_snapshot(sim, initial);
    REC_Header header = {};
    rec_header_from_config(config, initial, header);
    ASSERT_TRUE(rec_writer_close(rec_writer_create(other, header)));
    sim_destroy(sim);
    EXPECT_FALSE(oxr_replay_start(other));
    EXPECT_FALSE(oxr_record_status().replaying);
}

//
// Frame faults
//

namespace
{

struct OxrFrameFaultTest : OxrTest
{
    XrTime display_time = 0;

    void SetUp () override
    {
        OxrTest::SetUp();
        create_headless_instance();
        create_session();
        begin_session();
    }

    XrResult wait ()
    {
        XrFrameState          state     = { .type = XR_TYPE_FRAME_STATE };
        const XrFrameWaitInfo wait_info = { .type = XR_TYPE_FRAME_WAIT_INFO };
        const XrResult        result    = xr.xrWaitFrame(session, &wait_info, &state);
        if (XR_SUCCEEDED(result))
        {
            display_time = state.predictedDisplayTime;
        }
        return result;
    }

    XrResult begin () const
    {
        const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
        return xr.xrBeginFrame(session, &begin_info);
    }

    XrResult end () const
    {
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = display_time, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        return xr.xrEndFrame(session, &end_info);
    }
};

} // namespace

TEST_F(OxrFrameFaultTest, a_failed_wait_does_not_advance_the_frame)
{
    const XrTime last   = frame().predictedDisplayTime;
    const u64    before = sim_frame_index(oxr_get_sim());
    ASSERT_EQ(oxr_set_frame_fault(OXR_FrameOp::Wait, XR_ERROR_RUNTIME_FAILURE, 2), OXR_ControlResult::Ok);
    EXPECT_EQ(wait(), XR_ERROR_RUNTIME_FAILURE);
    EXPECT_EQ(wait(), XR_ERROR_RUNTIME_FAILURE);
    EXPECT_EQ(sim_frame_index(oxr_get_sim()), before);
    EXPECT_EQ(begin(), XR_ERROR_CALL_ORDER_INVALID) << "no frame was waited";

    EXPECT_EQ(wait(), XR_SUCCESS) << "the fault was used up";
    EXPECT_EQ(sim_frame_index(oxr_get_sim()), before + 1);
    EXPECT_EQ(display_time - last, sim_display_period(oxr_get_sim())) << "the failed waits skipped no display period";
    EXPECT_EQ(begin(), XR_SUCCESS);
    EXPECT_EQ(end(), XR_SUCCESS);
}

// Spec 10.4.2: xrEndFrame without a begun frame is XR_ERROR_CALL_ORDER_INVALID. The waited frame
// may be begun again, and the next xrWaitFrame does not wait for it to be begun.
TEST_F(OxrFrameFaultTest, a_failed_begin_begins_no_frame)
{
    frame();
    ASSERT_EQ(oxr_set_frame_fault(OXR_FrameOp::Begin, XR_ERROR_SESSION_LOST, 2), OXR_ControlResult::Ok);
    EXPECT_EQ(begin(), XR_ERROR_CALL_ORDER_INVALID) << "a call that fails its checks keeps the fault";
    EXPECT_EQ(wait(), XR_SUCCESS);
    EXPECT_EQ(begin(), XR_ERROR_SESSION_LOST);
    EXPECT_EQ(end(), XR_ERROR_CALL_ORDER_INVALID);

    const u64 start = os_monotonic_ns();
    EXPECT_EQ(wait(), XR_SUCCESS);
    EXPECT_LT(os_monotonic_ns() - start, (u64)OXR_FRAME_GATE_TIMEOUT_NS / 2) << "the frame whose begin failed holds no wait";
    EXPECT_EQ(begin(), XR_ERROR_SESSION_LOST);
    EXPECT_EQ(begin(), XR_SUCCESS) << "the waited frame is begun on a second try";
    EXPECT_EQ(end(), XR_SUCCESS);
    frame();
}

TEST_F(OxrFrameFaultTest, failed_locate_views_return_no_views)
{
    const XrTime     time     = frame().predictedDisplayTime;
    const XrSpace    stage    = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    XrViewLocateInfo info     = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = time, .space = stage };
    XrViewState      state    = { .type = XR_TYPE_VIEW_STATE };
    XrView           views[2] = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
    u32              count    = 7;
    ASSERT_EQ(oxr_set_frame_fault(OXR_FrameOp::LocateViews, XR_ERROR_TIME_INVALID, 1), OXR_ControlResult::Ok);
    state.viewStateFlags = XR_VIEW_STATE_POSITION_VALID_BIT;
    EXPECT_EQ(xr.xrLocateViews(session, &info, &state, 2, &count, views), XR_ERROR_TIME_INVALID);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(state.viewStateFlags, 0u);

    EXPECT_EQ(xr.xrLocateViews(session, &info, &state, 2, &count, views), XR_SUCCESS);
    EXPECT_EQ(count, 2u);
    EXPECT_TRUE(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
}

TEST_F(OxrFrameFaultTest, a_failed_end_keeps_the_frame_begun)
{
    frame();
    ASSERT_EQ(oxr_set_frame_fault(OXR_FrameOp::End, XR_ERROR_LAYER_INVALID, 1), OXR_ControlResult::Ok);
    EXPECT_EQ(end(), XR_ERROR_CALL_ORDER_INVALID) << "a call that fails its checks keeps the fault";
    EXPECT_EQ(wait(), XR_SUCCESS);
    EXPECT_EQ(begin(), XR_SUCCESS);
    EXPECT_EQ(end(), XR_ERROR_LAYER_INVALID);
    EXPECT_EQ(wait(), XR_SUCCESS);
    EXPECT_EQ(begin(), XR_FRAME_DISCARDED) << "the frame that failed to end was never ended";
    EXPECT_EQ(end(), XR_SUCCESS);
    frame();
}

TEST_F(OxrFrameFaultTest, faults_are_validated_and_cleared)
{
    EXPECT_EQ(oxr_set_frame_fault(OXR_FrameOp::Wait, XR_SESSION_LOSS_PENDING, 1), OXR_ControlResult::Invalid) << "faults are errors";
    EXPECT_EQ(oxr_set_frame_fault(OXR_FrameOp::Wait, XR_SUCCESS, 1), OXR_ControlResult::Invalid);
    EXPECT_EQ(oxr_set_frame_fault(OXR_FrameOp::Wait, XR_ERROR_CALL_ORDER_INVALID, 1), OXR_ControlResult::Invalid) << "not an error of xrWaitFrame";
    EXPECT_EQ(oxr_set_frame_fault(OXR_FrameOp::Begin, XR_ERROR_TIME_INVALID, 1), OXR_ControlResult::Invalid);
    EXPECT_EQ(oxr_set_frame_fault(OXR_FrameOp::LocateViews, XR_ERROR_SESSION_NOT_RUNNING, 1), OXR_ControlResult::Invalid);
    EXPECT_EQ(oxr_set_frame_fault(OXR_FrameOp::End, XR_ERROR_SIZE_INSUFFICIENT, 1), OXR_ControlResult::Invalid);
    EXPECT_EQ(oxr_set_frame_fault(OXR_FrameOp::Count, XR_ERROR_RUNTIME_FAILURE, 1), OXR_ControlResult::Invalid);
    frame();

    ASSERT_EQ(oxr_set_frame_fault(OXR_FrameOp::Wait, XR_ERROR_INSTANCE_LOST, 5), OXR_ControlResult::Ok);
    ASSERT_EQ(oxr_set_frame_fault(OXR_FrameOp::Wait, XR_SUCCESS, 0), OXR_ControlResult::Ok);
    frame();

    ASSERT_EQ(oxr_set_frame_fault(OXR_FrameOp::End, XR_ERROR_RUNTIME_FAILURE, 5), OXR_ControlResult::Ok);
    ASSERT_EQ(xr.xrDestroyInstance(instance), XR_SUCCESS);
    instance = XR_NULL_HANDLE;
    create_headless_instance();
    create_session();
    begin_session();
    frame();
}

// A recording keeps the faults pending as it starts, so the replay fails the same calls
TEST_F(OxrFrameFaultTest, pending_faults_replay_and_cannot_change_while_recording)
{
    char path[OS_MAX_PATH];
    test_path(path, sizeof(path), "puppetxr_test_frame_faults.xrrec");
    const u32 frames = 8;
    struct Observed
    {
        XrResult wait;
        XrResult begin;
        XrResult end;
        u64      hash;
    };
    Observed recorded[frames] = {};
    Observed replayed[frames] = {};
    auto run = [&](Observed* out)
    {
        for (u32 f = 0; f < frames; f++)
        {
            out[f].wait = wait();
            if (XR_SUCCEEDED(out[f].wait))
            {
                out[f].begin = begin();
                out[f].end   = end();
            }
            out[f].hash = sim_state_hash(oxr_get_sim());
        }
    };
    auto relaunch = [&]()
    {
        xr.xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        create_headless_instance();
        create_session();
        begin_session();
    };

    xr.xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;
    ASSERT_EQ(oxr_set_frame_fault(OXR_FrameOp::Wait, XR_ERROR_SESSION_LOST, 2), OXR_ControlResult::Ok) << "without an instance, for the next one";
    ASSERT_EQ(oxr_set_frame_fault(OXR_FrameOp::End, XR_ERROR_RUNTIME_FAILURE, 1), OXR_ControlResult::Ok);
    os_set_env("PUPPETXR_RECORD", path);
    relaunch();
    os_unset_env("PUPPETXR_RECORD");
    EXPECT_EQ(oxr_set_frame_fault(OXR_FrameOp::Begin, XR_ERROR_RUNTIME_FAILURE, 1), OXR_ControlResult::Busy);
    run(recorded);
    EXPECT_EQ(recorded[0].wait, XR_ERROR_SESSION_LOST);
    EXPECT_EQ(recorded[1].wait, XR_ERROR_SESSION_LOST);
    EXPECT_EQ(recorded[2].end, XR_ERROR_RUNTIME_FAILURE);
    EXPECT_EQ(recorded[3].begin, XR_FRAME_DISCARDED);
    EXPECT_EQ(recorded[3].end, XR_SUCCESS);

    relaunch();
    frame();
    os_set_env("PUPPETXR_REPLAY", path);
    relaunch();
    os_unset_env("PUPPETXR_REPLAY");
    ASSERT_EQ(oxr_set_frame_fault(OXR_FrameOp::Wait, XR_SUCCESS, 0), OXR_ControlResult::Busy);
    ASSERT_EQ(oxr_set_frame_fault(OXR_FrameOp::End, XR_SUCCESS, 0), OXR_ControlResult::Busy);
    run(replayed);
    for (u32 f = 0; f < frames; f++)
    {
        EXPECT_EQ(recorded[f].wait, replayed[f].wait) << "frame " << f;
        EXPECT_EQ(recorded[f].begin, replayed[f].begin) << "frame " << f;
        EXPECT_EQ(recorded[f].end, replayed[f].end) << "frame " << f;
        EXPECT_EQ(recorded[f].hash, replayed[f].hash) << "frame " << f;
    }
    EXPECT_EQ(oxr_record_status().divergences, 0u);
}

//
// Record and replay with rendering
//

namespace
{

struct OxrVulkanReplayTest : OxrVulkanTest
{
    void restart ()
    {
        if (session != XR_NULL_HANDLE)
        {
            xr.xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }
        if (pool)
        {
            vkDestroyCommandPool(vk_device, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
        if (vk_device)
        {
            vkDestroyDevice(vk_device, nullptr);
            vk_device = VK_NULL_HANDLE;
        }
        if (vk_instance)
        {
            test_validation_detach(validation, vk_instance);
            vkDestroyInstance(vk_instance, nullptr);
            vk_instance = VK_NULL_HANDLE;
        }
        xr.xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        xr       = load(gipa, XR_NULL_HANDLE);
        OxrVulkanTest::SetUp();
    }

    // Renders a colour derived from the head pose the app locates, like a real app would
    void run_session (b8 scripted, const char* screenshot)
    {
        create_vulkan();
        create_vulkan_session();
        begin_session();
        const XrSwapchain swapchain = create_swapchain(64, 64, 2);
        const XrSpace     stage     = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);

        for (u32 f = 0; f < 30; f++)
        {
            if (scripted)
            {
                const Quat turn = zm::quat_from_euler({ 0.0f, 0.05f * (f32)f, 0.0f });
                sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { .position = { 0.02f * (f32)f, 1.6f, 0.0f }, .orientation = turn });
            }

            XrFrameState           state      = { .type = XR_TYPE_FRAME_STATE };
            const XrFrameWaitInfo  wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
            const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
            ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
            ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);

            const XrViewLocateInfo locate = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = state.predictedDisplayTime, .space = stage };
            XrViewState      view_state = { .type = XR_TYPE_VIEW_STATE };
            XrView           views[2]   = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
            u32              count      = 0;
            ASSERT_EQ(xr.xrLocateViews(session, &locate, &view_state, 2, &count, views), XR_SUCCESS);

            const f32 red  = zm::clamp(views[0].pose.position.x, 0.0f, 1.0f);
            const f32 blue = zm::clamp(views[0].pose.orientation.y * 2.0f, 0.0f, 1.0f);
            render_into(swapchain, { { red, 0.25f, blue, 1.0f } });

            XrCompositionLayerProjectionView projection_views[2] = { view(swapchain, 0, 64, 64), view(swapchain, 1, 64, 64) };
            projection_views[0].pose = views[0].pose;
            projection_views[1].pose = views[1].pose;
            XrCompositionLayerProjection        layer     = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = stage, .viewCount = 2, .views = projection_views };
            const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
            const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = layers };
            ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);

            if (f == 20)
            {
                ASSERT_TRUE(oxr_save_screenshot(screenshot));
            }
        }
        EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
    }
};

} // namespace

static b8 files_equal (const char* a, const char* b, usize& out_size)
{
    u8*   data_a = nullptr;
    u8*   data_b = nullptr;
    usize size_a = 0;
    usize size_b = 0;
    const b8 read = os_read_file(a, &data_a, &size_a) && os_read_file(b, &data_b, &size_b);
    const b8 same = read && size_a == size_b && memory_equal(data_a, data_b, size_a);
    out_size      = size_a;
    os_memory_release(data_a);
    os_memory_release(data_b);
    return same;
}

TEST_F(OxrVulkanReplayTest, replayed_screenshots_are_byte_identical)
{
    char recording[OS_MAX_PATH];
    char shots[3][OS_MAX_PATH];
    test_path(recording, sizeof(recording), "puppetxr_test_render.xrrec");
    test_path(shots[0], sizeof(shots[0]), "puppetxr_test_render_live.png");
    test_path(shots[1], sizeof(shots[1]), "puppetxr_test_render_replay1.png");
    test_path(shots[2], sizeof(shots[2]), "puppetxr_test_render_replay2.png");

    os_set_env("PUPPETXR_RECORD", recording);
    restart();
    run_session(true, shots[0]);
    os_unset_env("PUPPETXR_RECORD");

    for (u32 replay = 1; replay <= 2; replay++)
    {
        os_set_env("PUPPETXR_REPLAY", recording);
        os_set_env("PUPPETXR_REPLAY_EXIT", "0");
        restart();
        run_session(false, shots[replay]);
        EXPECT_EQ(oxr_record_status().divergences, 0u);
        os_unset_env("PUPPETXR_REPLAY");
        os_unset_env("PUPPETXR_REPLAY_EXIT");
    }

    usize size = 0;
    EXPECT_TRUE(files_equal(shots[0], shots[1], size));
    EXPECT_TRUE(files_equal(shots[1], shots[2], size));
    EXPECT_GT(size, 100u);

    // The scripted head movement must have reached the image, or the comparison proves nothing
    u32 width = 0;
    u32 height = 0;
    u8  probe[4];
    oxr_read_frame(probe, 0, width, height);
    const usize bytes  = (usize)width * height * 4;
    u8*         pixels = (u8*)os_memory_alloc(bytes);
    defer { os_memory_release(pixels); };
    ASSERT_TRUE(oxr_read_frame(pixels, bytes, width, height));
    const u8* center = pixels + ((usize)(height / 2) * width + width / 2) * 4;
    EXPECT_GT(center[0], 100);
    EXPECT_GT(center[2], 100);
}

namespace
{

// Records with a Vulkan app and replays with an OpenGL one
struct OxrCrossBackendReplayTest : OxrVulkanReplayTest
{
    TestGLApp gl_app = {};

    OxrCrossBackendReplayTest ()
    {
        extensions[extensions_count++] = XR_MNDX_EGL_ENABLE_EXTENSION_NAME;
        extensions[extensions_count++] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
    }

    void SetUp () override
    {
        if (!r_backend_available(R_Backend::OpenGL))
        {
            GTEST_SKIP() << "no OpenGL backend";
        }
        OxrVulkanReplayTest::SetUp();
    }

    void TearDown () override
    {
        OxrVulkanReplayTest::TearDown();
        gl_app.destroy();
        EXPECT_EQ(test_gl_errors, 0u);
    }

    void run_gl_session (u32 frames)
    {
        ASSERT_TRUE(gl_app.create(false));
        PFN_xrGetOpenGLGraphicsRequirementsKHR get = nullptr;
        ASSERT_EQ(gipa(instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&get), XR_SUCCESS);
        XrGraphicsRequirementsOpenGLKHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
        ASSERT_EQ(get(instance, system, &requirements), XR_SUCCESS);
        const TestGLBinding       binding(gl_app);
        const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = binding.chain(), .systemId = system };
        ASSERT_EQ(xr.xrCreateSession(instance, &info, &session), XR_SUCCESS);
        begin_session();
        const XrSpace stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
        for (u32 f = 0; f < frames; f++)
        {
            XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
            ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
            ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
            const XrViewLocateInfo locate     = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = state.predictedDisplayTime, .space = stage };
            XrViewState            view_state = { .type = XR_TYPE_VIEW_STATE };
            XrView                 views[2]   = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
            u32                    count      = 0;
            ASSERT_EQ(xr.xrLocateViews(session, &locate, &view_state, 2, &count, views), XR_SUCCESS);
            const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
            ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
        }
    }
};

} // namespace

// Recordings hold simulator state only, so they replay with any graphics API
TEST_F(OxrCrossBackendReplayTest, vulkan_recordings_replay_in_gl_sessions)
{
    char recording[OS_MAX_PATH];
    char shot[OS_MAX_PATH];
    test_path(recording, sizeof(recording), "puppetxr_test_cross_backend.xrrec");
    test_path(shot, sizeof(shot), "puppetxr_test_cross_backend.png");

    os_set_env("PUPPETXR_RECORD", recording);
    restart();
    run_session(true, shot);
    os_unset_env("PUPPETXR_RECORD");

    os_set_env("PUPPETXR_REPLAY", recording);
    os_set_env("PUPPETXR_REPLAY_EXIT", "0");
    restart();
    run_gl_session(30);
    const OXR_RecordStatus status = oxr_record_status();
    os_unset_env("PUPPETXR_REPLAY");
    os_unset_env("PUPPETXR_REPLAY_EXIT");
    EXPECT_GE(status.replay_frame, 25u);
    EXPECT_EQ(status.divergences, 0u);

    // The replayed head moved as recorded
    const Pose head = sim_get_device_pose(oxr_get_sim(), SIM_Device::Head);
    EXPECT_GT(head.position.x, 0.4f);
}

TEST_F(OxrRecordTest, recordings_keep_their_controller_type)
{
    char path[OS_MAX_PATH];
    test_path(path, sizeof(path), "puppetxr_test_controller.xrrec");

    prepare();
    ASSERT_TRUE(oxr_set_controller(SIM_Controller::KhrSimple));
    ASSERT_TRUE(oxr_record_start(path));
    EXPECT_FALSE(oxr_set_controller(SIM_Controller::OculusTouch));
    Observed recorded[5];
    run(recorded, 5, false);
    ASSERT_TRUE(oxr_record_stop());
    EXPECT_TRUE(oxr_set_controller(SIM_Controller::OculusTouch));

    os_set_env("PUPPETXR_REPLAY", path);
    restart();
    EXPECT_EQ(sim_get_controller(oxr_get_sim()), SIM_Controller::KhrSimple);
    EXPECT_FALSE(oxr_set_controller(SIM_Controller::OculusTouch));
    prepare();
    Observed replayed[5];
    run(replayed, 5, false);
    expect_same(recorded, replayed, 5);
    EXPECT_EQ(oxr_record_status().divergences, 0u);

    XrInteractionProfileState state = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
    ASSERT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[0], &state), XR_SUCCESS);
    EXPECT_EQ(state.interactionProfile, XR_NULL_PATH);
    EXPECT_FALSE(oxr_set_controller((SIM_Controller)42));
}

//
// Mirror window
//

namespace
{

struct OxrWindowedTest : OxrVulkanTest
{
    OxrWindowedTest ()
    {
        headless = false;
    }

    void session_with_frames ()
    {
        create_vulkan_session();
        begin_session();
        const XrSwapchain swapchain = create_swapchain(64, 64, 2);
        const XrSpace     stage     = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
        for (u32 f = 0; f < 3; f++)
        {
            XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
            ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
            ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
            render_into(swapchain, { { 0.2f, 0.4f, 0.6f, 1.0f } });
            XrCompositionLayerProjectionView    views[2]  = { view(swapchain, 0, 64, 64), view(swapchain, 1, 64, 64) };
            XrCompositionLayerProjection        layer     = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = stage, .viewCount = 2, .views = views };
            const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
            const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = layers };
            ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
        }
        xr.xrDestroySwapchain(swapchain);
        xr.xrDestroySession(session);
        session = XR_NULL_HANDLE;
    }
};

} // namespace

// An editor keeps its instance between play sessions (Unreal's VR Preview, Unity's play mode): the
// window leaves with the session instead of holding its last frame, and comes back with the next
TEST_F(OxrWindowedTest, the_window_hides_with_its_session_and_returns_with_the_next)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    create_vulkan();
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt, nullptr);
    ASSERT_NE(rt->window, nullptr);
    EXPECT_FALSE(os_window_shown(rt->window));
    for (u32 round = 0; round < 2; round++)
    {
        create_vulkan_session();
        begin_session();
        EXPECT_TRUE(os_window_shown(rt->window)) << "round " << round;
        ASSERT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
        session = XR_NULL_HANDLE;
        EXPECT_FALSE(os_window_shown(rt->window)) << "round " << round;
    }
}

// Unity runs xrWaitFrame and xrEndFrame on its render thread while its main thread, the window
// thread on macOS, blocks until the render thread is done. Frames run on a thread of their own
// here while the test's main thread waits for that thread without serving window calls
struct OxrThreadWindowedTest : OxrWindowedTest
{
    struct FrameThread
    {
        OxrThreadWindowedTest* test;
        u32                    frames;
        u64                    longest_ns;
        b8                     threads_differ;
    };

    static i32 frame_thread (void* user)
    {
        FrameThread& run = *(FrameThread*)user;
        run.threads_differ = !os_is_window_thread() || os_is_headless();
        for (u32 i = 0; i < run.frames; i++)
        {
            const u64 start = os_monotonic_ns();
            run.test->frame_only();
            run.longest_ns = zm::max(run.longest_ns, os_monotonic_ns() - start);
        }
        return 0;
    }

    // Returns the longest frame
    u64 frames_on_a_thread (u32 count)
    {
        FrameThread run    = { .test = this, .frames = count };
        OS_Thread*  thread = os_thread_create("frames", frame_thread, &run);
        os_thread_join(thread);
        EXPECT_TRUE(run.threads_differ);
        return run.longest_ns;
    }
};

TEST_F(OxrThreadWindowedTest, frames_never_wait_for_a_main_thread_blocked_on_them)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt->window, nullptr);
    EXPECT_LT(frames_on_a_thread(30), 50000000ull) << "every frame under 50 ms";
    if (os_is_window_thread())
    {
        EXPECT_TRUE(rt->window_tick_pending) << "the window's part waits for the main thread";
    }
    ticks();
    EXPECT_FALSE(rt->window_tick_pending);
    EXPECT_TRUE(os_window_shown(rt->window));
}

namespace
{

struct SplitFrameEnd
{
    OxrThreadWindowedTest* test;
    XrTime                 display_time;
};

} // namespace

static i32 end_frame_elsewhere (void* user)
{
    const SplitFrameEnd& end = *(const SplitFrameEnd*)user;
    const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    EXPECT_EQ(end.test->xr.xrBeginFrame(end.test->session, &begin_info), XR_SUCCESS);
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = end.display_time, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    EXPECT_EQ(end.test->xr.xrEndFrame(end.test->session, &end_info), XR_SUCCESS);
    return 0;
}

// Engines that wait for frames on one thread and end them on another (Unity) are named in the log
// once, which tells what a profile or a hang report of theirs is looking at
TEST_F(OxrThreadWindowedTest, a_split_frame_loop_names_its_threads_once)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    EXPECT_EQ(log_lines_with("Frames are waited on thread"), 0u);
    for (u32 i = 0; i < 2; i++)
    {
        XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
        ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
        SplitFrameEnd end = { .test = this, .display_time = state.predictedDisplayTime };
        os_thread_join(os_thread_create("ender", end_frame_elsewhere, &end));
    }
    EXPECT_EQ(log_lines_with("Frames are waited on thread"), 1u);
}

// Events reach the frames through what is queued: a frame thread never pumps
TEST_F(OxrThreadWindowedTest, input_reaches_frames_while_the_main_thread_is_blocked_on_them)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    rt->show_welcome = false;
    SIM_Sim*  sim    = oxr_get_sim();
    const f32 before = sim_get_device_pose(sim, SIM_Device::Head).position.z;
    os_push_key_event(rt->window, OS_Key::W, true, false);
    frames_on_a_thread(20);
    EXPECT_LT(sim_get_device_pose(sim, SIM_Device::Head).position.z, before - 0.05f) << "W walks forward";
    os_push_key_event(rt->window, OS_Key::W, false, false);
    frames_on_a_thread(2);
    const f32 stopped = sim_get_device_pose(sim, SIM_Device::Head).position.z;
    frames_on_a_thread(10);
    EXPECT_NEAR(sim_get_device_pose(sim, SIM_Device::Head).position.z, stopped, 1e-5f) << "and stops when released";
}

// The click that takes the mouse is read on the frame thread; relative mode is the window's,
// so the tick turns it on, and Escape off again
TEST_F(OxrThreadWindowedTest, taking_the_mouse_never_waits_for_a_main_thread_blocked_on_frames)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    rt->show_welcome = false;
    ticks();
    os_push_mouse_event(rt->window, 30.0f, 10.0f, OS_Key::MouseLeft, true);
    os_push_mouse_event(rt->window, 30.0f, 10.0f, OS_Key::MouseLeft, false);
    EXPECT_LT(frames_on_a_thread(3), 50000000ull);
    EXPECT_TRUE(rt->mouse_captured);
    EXPECT_TRUE(rt->window_relative_pending || !os_is_window_thread()) << "the window thread of its own may have applied it";
    ticks();
    EXPECT_FALSE(rt->window_relative_pending) << "the tick applied it";

    os_push_key_event(rt->window, OS_Key::Escape, true, false);
    os_push_key_event(rt->window, OS_Key::Escape, false, false);
    EXPECT_LT(frames_on_a_thread(3), 50000000ull);
    EXPECT_FALSE(rt->mouse_captured);
    EXPECT_TRUE(rt->window_relative_pending || !os_is_window_thread());
    ticks();
    EXPECT_FALSE(rt->window_relative_pending);
}

// Once the mouse is held, moves turn the head with and without the left button down
TEST_F(OxrThreadWindowedTest, the_held_mouse_turns_with_the_trigger_held)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    rt->show_welcome = false;
    SIM_Sim*   sim = oxr_get_sim();
    const auto yaw = [&] { return zm::euler_from_quat(sim_get_device_pose(sim, SIM_Device::Head).orientation).y; };
    os_push_mouse_event(rt->window, 30.0f, 10.0f, OS_Key::MouseLeft, true);
    os_push_mouse_event(rt->window, 30.0f, 10.0f, OS_Key::MouseLeft, false);
    frames_on_a_thread(2);
    ASSERT_TRUE(rt->mouse_captured);
    const f32 start = yaw();
    os_push_mouse_event(rt->window, 40.0f, 10.0f, OS_Key::None, false);
    frames_on_a_thread(2);
    const f32 turned = yaw();
    EXPECT_LT(turned, start - 1e-4f);
    os_push_mouse_event(rt->window, 40.0f, 10.0f, OS_Key::MouseLeft, true);
    os_push_mouse_event(rt->window, 50.0f, 10.0f, OS_Key::None, false);
    frames_on_a_thread(2);
    EXPECT_LT(yaw(), turned - 1e-4f) << "the trigger held does not stop the turn";
    os_push_mouse_event(rt->window, 50.0f, 10.0f, OS_Key::MouseLeft, false);
    frames_on_a_thread(1);
}

struct OxrVulkanRealWindowTest : OxrWindowedTest
{
    OxrVulkanRealWindowTest ()
    {
        real_window = true;
    }
};

// The mirror window never paces the app: with FIFO, MoltenVK waited in vkQueueSubmit for a drawable
// and Godot ran at 53 instead of 100 frames a second on a 60 Hz display
TEST_F(OxrVulkanRealWindowTest, frames_never_wait_for_the_display)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_TRUE(rt->window_shown);
    for (const OXR_Session& live : rt->sessions)
    {
        if (!live.window_swapchain)
        {
            GTEST_SKIP() << "no mirror window swapchain";
        }
    }
    for (u32 i = 0; i < 5; i++)
    {
        frame();
    }
    const u64 start = os_monotonic_ns();
    for (u32 i = 0; i < 60; i++)
    {
        frame_only();
    }
    EXPECT_LT(os_monotonic_ns() - start, 300000000ull) << "60 frames take under 300 ms on any display rate up to 200 Hz";
}

// The window server holds a presented drawable until its next composite: presenting faster than
// the display held all of them and the next acquire waited up to 20 ms
TEST_F(OxrVulkanRealWindowTest, the_mirror_presents_at_most_at_the_display_rate)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    if (rt->window_refresh_hz <= 0.0f)
    {
        GTEST_SKIP() << "the display's rate is unknown";
    }
    const OXR_Session* live = nullptr;
    for (const OXR_Session& session_state : rt->sessions)
    {
        live = &session_state;
    }
    ASSERT_NE(live, nullptr);
    u32       presents  = 0;
    u64       presented = live->window_presented_ns;
    const u64 start     = os_monotonic_ns();
    for (u32 i = 0; i < 60; i++)
    {
        frame();
        presents += live->window_presented_ns != presented;
        presented = live->window_presented_ns;
    }
    const f64 seconds = (f64)(os_monotonic_ns() - start) * 1e-9;
    EXPECT_GT(presents, 0u);
    EXPECT_LE((f64)presents, seconds * (f64)rt->window_refresh_hz + 2.0);
}

// A window whose last present is still on the GPU makes the next acquire wait; frames then skip
// the mirror instead. Once the GPU is done it is free again
TEST_F(OxrVulkanRealWindowTest, the_window_is_busy_only_while_its_last_present_is_on_the_gpu)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    for (const OXR_Session& live : rt->sessions)
    {
        if (!live.window_swapchain)
        {
            GTEST_SKIP() << "no mirror window swapchain";
        }
        r_wait_idle(live.device);
        EXPECT_FALSE(r_window_busy(live.device, live.window_swapchain));
        EXPECT_FALSE(r_window_busy(live.device, R_WindowSwapchainHandle{}));
    }
}

// Regression: the busy check read the fence of the frame slot that presented, which the
// compositor's next frames reuse; their work still on the GPU kept the window busy, so with
// frames coming faster than the GPU finished them the mirror never presented again
TEST_F(OxrVulkanRealWindowTest, frames_after_a_present_do_not_keep_the_window_busy)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    for (const OXR_Session& live : rt->sessions)
    {
        if (!live.window_swapchain)
        {
            GTEST_SKIP() << "no mirror window swapchain";
        }
        R_Device* device = live.device;
        r_wait_idle(device);

        R_ImageHandle window_image;
        ASSERT_TRUE(r_window_acquire(device, live.window_swapchain, window_image));
        const R_CmdBufferHandle present = r_begin_frame(device);
        r_cmd_transition_image(device, present, window_image, R_ImageLayout::Undefined, R_ImageLayout::Present);
        r_end_frame_present(device, present, live.window_swapchain);
        r_window_present(device, live.window_swapchain);
        r_wait_idle(device);
        ASSERT_FALSE(r_window_busy(device, live.window_swapchain));

        // Enough GPU work that the frames still run when the check comes
        const R_CreateImageInfo info = {
            .width          = 4096,
            .height         = 4096,
            .format         = R_Format::R8G8B8A8_UNORM,
            .usage_flags    = (u32)R_ImageUsageFlags::TransferDst,
            .initial_layout = R_ImageLayout::TransferDst,
        };
        const R_ImageHandle heavy = r_create_image(device, info);
        ASSERT_TRUE(heavy);
        for (u32 i = 0; i < R_MAX_FRAMES_IN_FLIGHT; i++)
        {
            const R_CmdBufferHandle cmd = r_begin_frame(device);
            for (u32 j = 0; j < 64; j++)
            {
                r_cmd_clear_image(device, cmd, heavy, { (f32)j / 64.0f, 0.0f, 0.0f, 1.0f });
            }
            r_end_frame(device, cmd);
        }
        EXPECT_FALSE(r_window_busy(device, live.window_swapchain));
        r_wait_idle(device);
        r_destroy_image(device, heavy);
    }
}

// PUPPETXR_PROFILE captures from xrCreateInstance to the runtime's shutdown: each xr* call is a zone
// by its name, and the parts of a frame have zones of their own
TEST_F(OxrVulkanRealWindowTest, profile_names_the_calls_and_the_frame_parts)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    char path[OS_MAX_PATH];
    test_path(path, sizeof(path), "puppetxr_test_profile.json");
    xr.xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;
    os_set_env("PUPPETXR_PROFILE", path);
    ASSERT_EQ(create_instance(extensions, extensions_count), XR_SUCCESS);
    os_unset_env("PUPPETXR_PROFILE");
    EXPECT_TRUE(os_profile_active());
    const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);
    create_vulkan();
    create_vulkan_session();
    begin_session();
    for (u32 i = 0; i < 10; i++)
    {
        frame();
    }
    xr.xrDestroySession(session);
    session = XR_NULL_HANDLE;
    xr.xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;
    EXPECT_FALSE(os_profile_active()) << "the capture ends with the runtime";

    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(path, &data, &size));
    char* text = (char*)os_memory_alloc(size + 1);
    memory_copy(text, data, size);
    text[size] = 0;
    os_memory_release(data);
    defer { os_memory_release(text); };
    for (const char* zone : { "oxr_xrCreateSession", "oxr_xrWaitFrame", "oxr_xrEndFrame", "runtime lock wait", "sim step", "compose", "mirror window", "window tick", "gpu submit" })
    {
        char needle[64];
        cstr_format(needle, sizeof(needle), "\"name\":\"%s\"", zone);
        EXPECT_NE(cstr_find(text, needle), nullptr) << zone;
    }
    os_delete_file(path);
}

// Closing the window ends the session it showed, not the next one: Unreal's second VR Preview
// after a window closed went to EXITING at its first frame
TEST_F(OxrWindowedTest, a_closed_window_ends_its_session_only)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    create_vulkan();
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt, nullptr);
    create_vulkan_session();
    begin_session();
    rt->window_closed = true;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    for (u32 i = 0; i < 5 && state != XR_SESSION_STATE_STOPPING; i++)
    {
        frame();
        state = drain_events();
    }
    EXPECT_EQ(state, XR_SESSION_STATE_STOPPING);
    ASSERT_EQ(xr.xrEndSession(session), XR_SUCCESS);
    EXPECT_EQ(drain_events(), XR_SESSION_STATE_EXITING);
    ASSERT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
    session = XR_NULL_HANDLE;

    create_vulkan_session();
    begin_session();
    for (u32 i = 0; i < 3; i++)
    {
        frame();
    }
    EXPECT_EQ(drain_events(), XR_SESSION_STATE_UNKNOWN) << "the next session runs on";
}

// A click into the view takes the mouse without pressing anything; then moves turn and buttons
// press. Escape and the loss of focus give it back. The move that brought the mouse back into the
// window came in the same frame as a press and turned the head many times round
TEST_F(OxrWindowedTest, a_click_into_the_view_takes_the_mouse_and_escape_gives_it_back)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt, nullptr);
    ASSERT_NE(rt->window, nullptr);
    SIM_Sim*  sim         = oxr_get_sim();
    const f32 sensitivity = sim_get_config(sim).look_sensitivity;
    const f32 yaw_before  = zm::euler_from_quat(sim_get_device_pose(sim, SIM_Device::Head).orientation).y;
    const auto yaw        = [&] { return zm::euler_from_quat(sim_get_device_pose(sim, SIM_Device::Head).orientation).y; };
    const auto trigger    = [&] { return sim_get_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue).value.x; };
    // Escape closes the welcome overlay first, which tests running beside this one may show
    rt->show_welcome = false;

    os_push_mouse_event(rt->window, 10.0f, 10.0f, OS_Key::None, false);
    os_push_mouse_event(rt->window, 30.0f, 10.0f, OS_Key::None, false);
    frame();
    EXPECT_FALSE(rt->mouse_captured);
    EXPECT_NEAR(yaw(), yaw_before, 1e-4f) << "moves do not turn before a click";

    os_push_mouse_event(rt->window, 30.0f, 10.0f, OS_Key::MouseLeft, true);
    os_push_mouse_event(rt->window, 30.0f, 10.0f, OS_Key::MouseLeft, false);
    frame();
    frame();
    EXPECT_TRUE(rt->mouse_captured);
    EXPECT_EQ(trigger(), 0.0f) << "the click that takes the mouse presses nothing";

    // A jump in one event is how the switch to relative mode or a fast cursor reads, not a turn
    os_push_mouse_event(rt->window, 1510.0f, 10.0f, OS_Key::None, false);
    os_push_mouse_event(rt->window, 1520.0f, 10.0f, OS_Key::None, false);
    frame();
    frame();
    EXPECT_NEAR(yaw(), yaw_before - 10.0f * sensitivity, 1e-4f) << "only the 10 pixel move turns";

    os_push_mouse_event(rt->window, 1520.0f, 10.0f, OS_Key::MouseLeft, true);
    frame();
    EXPECT_GT(trigger(), 0.0f) << "a held mouse presses the trigger";

    os_push_key_event(rt->window, OS_Key::Escape, true, false);
    os_push_key_event(rt->window, OS_Key::Escape, false, false);
    os_push_mouse_event(rt->window, 1530.0f, 10.0f, OS_Key::None, false);
    for (u32 i = 0; i < 30; i++)
    {
        frame();
    }
    EXPECT_FALSE(rt->mouse_captured);
    EXPECT_EQ(trigger(), 0.0f) << "the trigger is let go with the mouse";
    EXPECT_NEAR(yaw(), yaw_before - 10.0f * sensitivity, 1e-4f) << "nor do moves after Escape turn";

    os_push_mouse_event(rt->window, 300.0f, 10.0f, OS_Key::MouseLeft, true);
    os_push_mouse_event(rt->window, 300.0f, 10.0f, OS_Key::MouseLeft, false);
    frame();
    EXPECT_TRUE(rt->mouse_captured);
    os_push_focus_lost(rt->window);
    frame();
    EXPECT_FALSE(rt->mouse_captured) << "another window took the focus";
}

// Showing the panels gives the mouse back, as Escape does; a click outside the client area, as the
// focus click on the title bar's close button, takes nothing
TEST_F(OxrWindowedTest, showing_the_panels_or_a_title_bar_click_leaves_the_mouse)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt, nullptr);
    rt->show_welcome = false;
    rt->show_ui      = false;
    os_push_mouse_event(rt->window, 200.0f, 200.0f, OS_Key::MouseLeft, true);
    os_push_mouse_event(rt->window, 200.0f, 200.0f, OS_Key::MouseLeft, false);
    frame();
    ASSERT_TRUE(rt->mouse_captured);
    os_push_key_event(rt->window, OS_Key::Tab, true, false);
    os_push_key_event(rt->window, OS_Key::Tab, false, false);
    frame();
    EXPECT_TRUE(rt->show_ui);
    EXPECT_FALSE(rt->mouse_captured) << "the panels are for the mouse";

    rt->show_ui = false;
    frame();
    os_push_mouse_event(rt->window, 200.0f, -10.0f, OS_Key::MouseLeft, true);
    os_push_mouse_event(rt->window, 200.0f, -10.0f, OS_Key::MouseLeft, false);
    frame();
    EXPECT_FALSE(rt->mouse_captured) << "a click on the title bar";
}

TEST_F(OxrWindowedTest, tab_shows_and_hides_the_panels)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    create_vulkan();
    create_vulkan_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt, nullptr);
    const b8 shown = rt->show_ui;
    os_push_key_event(rt->window, OS_Key::Tab, true, false);
    os_push_key_event(rt->window, OS_Key::Tab, false, false);
    frame();
    EXPECT_NE(rt->show_ui, shown);
    os_push_key_event(rt->window, OS_Key::Grave, true, false);
    os_push_key_event(rt->window, OS_Key::Grave, false, false);
    frame();
    EXPECT_NE(rt->show_ui, shown) << "the key left of 1 no longer does";
}

// Regression: the mirror window's VkSurfaceKHR lives on the app's VkInstance and used to leak,
// so the validation layer reported it when the app destroyed its instance
TEST_F(OxrWindowedTest, window_surface_is_released_with_the_session)
{
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    create_vulkan();
    if (oxr_get_runtime() == nullptr)
    {
        GTEST_SKIP();
    }
    session_with_frames();
    session_with_frames();
}

//
// Engine threading patterns
//

#define THREAD_TEST_FRAMES 120
#define THREAD_TEST_SIZE   64

namespace
{

// Calls the runtime from several threads as engines do: one thread waits frames, others render
// and query input at the same time. Run under the clang-tsan preset these find data races.
struct OxrThreadTest : OxrVulkanTest
{
    XrActionSet  set       = XR_NULL_HANDLE;
    XrAction     trigger   = XR_NULL_HANDLE;
    XrAction     grip      = XR_NULL_HANDLE;
    XrAction     haptic    = XR_NULL_HANDLE;
    XrPath       hands[2]  = {};
    XrSpace      stage     = XR_NULL_HANDLE;
    XrSpace      grip_space = XR_NULL_HANDLE;
    XrSwapchain  swapchain = XR_NULL_HANDLE;

    // Unreal: the game thread waits a frame and hands it to the RHI thread through two slots
    OS_Semaphore* slots_free  = nullptr;
    OS_Semaphore* slots_full  = nullptr;
    XrFrameState  slots[2]    = {};

    // Written by the rendering thread, read by the others
    OS_Mutex*     shared_mutex = nullptr;
    u32           frames_ended = 0;
    XrTime        last_display_time = 0;
    u64           ending_thread = 0;

    // Unity: the main thread queries input until the render thread is told to stop
    OS_Semaphore* stop        = nullptr;
    OS_Semaphore* stopped     = nullptr;

    void SetUp () override
    {
        OxrVulkanTest::SetUp();
        if (IsSkipped())
        {
            return;
        }
        create_vulkan();
        if (IsSkipped() || HasFatalFailure())
        {
            return;
        }
        create_vulkan_session();
        hands[0] = path("/user/hand/left");
        hands[1] = path("/user/hand/right");

        const XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .actionSetName = "gameplay", .localizedActionSetName = "Gameplay" };
        ASSERT_EQ(xr.xrCreateActionSet(instance, &set_info, &set), XR_SUCCESS);
        trigger = make_action("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
        grip    = make_action("grip", XR_ACTION_TYPE_POSE_INPUT);
        haptic  = make_action("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);
        const XrActionSuggestedBinding bindings[] = {
            { trigger, path("/user/hand/left/input/trigger/value") },
            { trigger, path("/user/hand/right/input/trigger/value") },
            { grip,    path("/user/hand/left/input/grip/pose") },
            { grip,    path("/user/hand/right/input/grip/pose") },
            { haptic,  path("/user/hand/left/output/haptic") },
            { haptic,  path("/user/hand/right/output/haptic") },
        };
        const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = ARRAY_SIZE(bindings), .suggestedBindings = bindings };
        ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
        const XrSessionActionSetsAttachInfo attach = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &set };
        ASSERT_EQ(xr.xrAttachSessionActionSets(session, &attach), XR_SUCCESS);

        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
        const XrActionSpaceCreateInfo space_info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = grip, .subactionPath = hands[1], .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
        ASSERT_EQ(xr.xrCreateActionSpace(session, &space_info, &grip_space), XR_SUCCESS);
        swapchain = create_swapchain(THREAD_TEST_SIZE, THREAD_TEST_SIZE, 2);
        begin_session();

        slots_free = os_semaphore_create(ARRAY_SIZE(slots));
        slots_full = os_semaphore_create(0);
        stop       = os_semaphore_create(0);
        stopped    = os_semaphore_create(0);
        shared_mutex = os_mutex_create();
    }

    void TearDown () override
    {
        for (OS_Semaphore* semaphore : { slots_free, slots_full, stop, stopped })
        {
            if (semaphore)
            {
                os_semaphore_destroy(semaphore);
            }
        }
        if (shared_mutex)
        {
            os_mutex_destroy(shared_mutex);
        }
        OxrVulkanTest::TearDown();
    }

    XrAction make_action (const char* name, XrActionType type)
    {
        XrAction           action = XR_NULL_HANDLE;
        XrActionCreateInfo info   = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionType = type, .countSubactionPaths = 2, .subactionPaths = hands };
        cstr_copy(info.actionName, sizeof(info.actionName), name);
        cstr_copy(info.localizedActionName, sizeof(info.localizedActionName), name);
        EXPECT_EQ(xr.xrCreateAction(set, &info, &action), XR_SUCCESS);
        return action;
    }

    // What a game or main thread does every tick
    void query_input (XrTime time)
    {
        const XrActiveActionSet active = { .actionSet = set };
        const XrActionsSyncInfo sync   = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active };
        EXPECT_EQ(xr.xrSyncActions(session, &sync), XR_SUCCESS);

        XrActionStateFloat         value      = { .type = XR_TYPE_ACTION_STATE_FLOAT };
        const XrActionStateGetInfo value_info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = trigger, .subactionPath = hands[0] };
        EXPECT_EQ(xr.xrGetActionStateFloat(session, &value_info, &value), XR_SUCCESS);
        XrActionStatePose          pose      = { .type = XR_TYPE_ACTION_STATE_POSE };
        const XrActionStateGetInfo pose_info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = grip, .subactionPath = hands[1] };
        EXPECT_EQ(xr.xrGetActionStatePose(session, &pose_info, &pose), XR_SUCCESS);
        EXPECT_TRUE(pose.isActive);

        XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
        EXPECT_EQ(xr.xrLocateSpace(grip_space, stage, time, &location), XR_SUCCESS);
        EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);

        XrViewState                 view_state  = { .type = XR_TYPE_VIEW_STATE };
        XrView                      views[2]    = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
        u32                         views_count = 0;
        const XrViewLocateInfo      locate_info = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = time, .space = stage };
        EXPECT_EQ(xr.xrLocateViews(session, &locate_info, &view_state, 2, &views_count, views), XR_SUCCESS);

        const XrHapticActionInfo haptic_info = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = hands[0] };
        const XrHapticVibration  vibration   = { .type = XR_TYPE_HAPTIC_VIBRATION, .duration = XR_MIN_HAPTIC_DURATION, .frequency = XR_FREQUENCY_UNSPECIFIED, .amplitude = 0.5f };
        EXPECT_EQ(xr.xrApplyHapticFeedback(session, &haptic_info, (const XrHapticBaseHeader*)&vibration), XR_SUCCESS);
    }

    // What a render or RHI thread does with a waited frame
    void render_frame (const XrFrameState& state)
    {
        EXPECT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
        render_into(swapchain, { { 0.2f, 0.4f, 0.6f, 1.0f } });
        XrCompositionLayerProjectionView    views[2]  = { view(swapchain, 0, THREAD_TEST_SIZE, THREAD_TEST_SIZE), view(swapchain, 1, THREAD_TEST_SIZE, THREAD_TEST_SIZE) };
        XrCompositionLayerProjection        layer     = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = stage, .viewCount = 2, .views = views };
        const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = layers };
        EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
        os_mutex_lock(shared_mutex);
        frames_ended++;
        last_display_time = state.predictedDisplayTime;
        ending_thread     = os_thread_current_id();
        os_mutex_unlock(shared_mutex);
    }

    void read_shared (u32& out_frames_ended, XrTime& out_display_time) const
    {
        os_mutex_lock(shared_mutex);
        out_frames_ended = frames_ended;
        out_display_time = last_display_time;
        os_mutex_unlock(shared_mutex);
    }

    static i32 rhi_thread (void* user)
    {
        OxrThreadTest& t = *(OxrThreadTest*)user;
        for (u32 f = 0; f < THREAD_TEST_FRAMES; f++)
        {
            os_semaphore_wait(t.slots_full);
            t.render_frame(t.slots[f % ARRAY_SIZE(t.slots)]);
            os_semaphore_signal(t.slots_free);
        }
        return 0;
    }

    static i32 render_thread (void* user)
    {
        OxrThreadTest& t = *(OxrThreadTest*)user;
        while (!os_semaphore_try_wait(t.stop))
        {
            XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
            EXPECT_EQ(t.xr.xrWaitFrame(t.session, nullptr, &state), XR_SUCCESS);
            t.render_frame(state);
        }
        os_semaphore_signal(t.stopped);
        return 0;
    }
};

} // namespace

// Unreal: xrWaitFrame and input on the game thread, xrBeginFrame and xrEndFrame on the RHI
// thread, the game thread one frame ahead. Spec 10.4.1: the next xrWaitFrame blocks until the
// previous frame was begun, from whichever thread.
TEST_F(OxrThreadTest, unreal_waits_on_the_game_thread_and_renders_on_the_rhi_thread)
{
    OS_Thread* rhi = os_thread_create("rhi", rhi_thread, this);
    ASSERT_NE(rhi, nullptr);
    XrTime last = 0;
    for (u32 f = 0; f < THREAD_TEST_FRAMES; f++)
    {
        os_semaphore_wait(slots_free);
        XrFrameState& state = slots[f % ARRAY_SIZE(slots)];
        state = { .type = XR_TYPE_FRAME_STATE };
        EXPECT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
        EXPECT_GT(state.predictedDisplayTime, last);
        EXPECT_TRUE(state.shouldRender);
        last = state.predictedDisplayTime;
        query_input(state.predictedDisplayTime);
        os_semaphore_signal(slots_full);
    }
    EXPECT_EQ(os_thread_join(rhi), 0);
    u32    ended        = 0;
    XrTime display_time = 0;
    read_shared(ended, display_time);
    EXPECT_EQ(ended, (u32)THREAD_TEST_FRAMES);
    EXPECT_EQ(display_time, last);
}

// The control server answers in xrWaitFrame, on Unreal's game thread, while the RHI thread may
// use the app's queue or immediate context; D3D11's is not thread-safe, and AMD's driver crashed.
// With the frame loop split, frames are read back in xrEndFrame and screenshots come from there.
struct OxrThreadControlTest : OxrThreadTest
{
    void SetUp () override
    {
        os_set_env("PUPPETXR_CONTROL", "1");
        OxrThreadTest::SetUp();
        os_unset_env("PUPPETXR_CONTROL");
    }
};

TEST_F(OxrThreadControlTest, unreal_frames_are_read_back_on_the_rhi_thread)
{
    OS_Thread* rhi = os_thread_create("rhi", rhi_thread, this);
    ASSERT_NE(rhi, nullptr);
    for (u32 f = 0; f < THREAD_TEST_FRAMES; f++)
    {
        os_semaphore_wait(slots_free);
        XrFrameState& state = slots[f % ARRAY_SIZE(slots)];
        state = { .type = XR_TYPE_FRAME_STATE };
        EXPECT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
        os_semaphore_signal(slots_full);
    }
    EXPECT_EQ(os_thread_join(rhi), 0);

    u32 width  = 0;
    u32 height = 0;
    ASSERT_TRUE(oxr_read_frame(nullptr, 0, width, height));
    Array<u8> pixels = {};
    defer { pixels.release(); };
    pixels.resize((usize)width * height * 4);
    ASSERT_TRUE(oxr_read_frame(pixels.data, pixels.length, width, height));
    EXPECT_EQ(oxr_frame_read_thread(), ending_thread);
    EXPECT_NE(oxr_frame_read_thread(), os_thread_current_id());
    EXPECT_GT(pixels.data[((height / 2) * width + width / 2) * 4 + 2], 100) << "the frame's blue";
}

// The control server also answers in xrPollEvent, on Unity's main thread while its render thread
// submits, and screenshots came before the first frame, while Unreal's RHI thread rendered: two
// threads on one queue lost the Vulkan device. With control on, only xrEndFrame reads the GPU.
struct OxrControlVulkanTest : OxrVulkanTest
{
    void SetUp () override
    {
        os_set_env("PUPPETXR_CONTROL", "1");
        OxrVulkanTest::SetUp();
        os_unset_env("PUPPETXR_CONTROL");
    }
};

TEST_F(OxrControlVulkanTest, frames_are_read_off_the_gpu_only_in_xrEndFrame)
{
    create_vulkan();
    create_vulkan_session();
    R_Device* device = nullptr;
    for (const OXR_Session& live : oxr_get_runtime()->sessions)
    {
        device = live.device;
    }
    ASSERT_NE(device, nullptr);
    u32 width  = 0;
    u32 height = 0;
    ASSERT_TRUE(oxr_read_frame(nullptr, 0, width, height));
    Array<u8> pixels = {};
    defer { pixels.release(); };
    pixels.resize((usize)width * height * 4);
    // A screenshot then says there is no frame yet, so a driver steps frames instead of waiting
    PUPPETXR_Api api = {};
    ASSERT_EQ(puppetxr_get_api(PUPPETXR_API_VERSION, &api), PUPPETXR_SUCCESS);
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_first_frame.png", temp_dir());
    defer { os_delete_file(path); };
    u64 before = r_queue_access_count(device);
    EXPECT_FALSE(oxr_read_frame(pixels.data, pixels.length, width, height)) << "no frame before the first xrEndFrame";
    EXPECT_EQ(api.screenshot(path), PUPPETXR_ERROR_NOT_AVAILABLE);
    EXPECT_EQ(r_queue_access_count(device), before);

    begin_session();
    frame();
    before = r_queue_access_count(device);
    EXPECT_TRUE(oxr_read_frame(pixels.data, pixels.length, width, height));
    EXPECT_EQ(api.screenshot(path), PUPPETXR_SUCCESS);
    EXPECT_EQ(r_queue_access_count(device), before);
}

// PUPPETXR_SCREENSHOT_DIR named a directory that did not exist yet: "Cannot write" and no picture
struct OxrScreenshotDirTest : OxrVulkanTest
{
    char dir[OS_MAX_PATH] = {};

    void SetUp () override
    {
        cstr_format(dir, sizeof(dir), "%s/puppetxr_missing_shots/nested", temp_dir());
        char parent[OS_MAX_PATH];
        cstr_format(parent, sizeof(parent), "%s/puppetxr_missing_shots", temp_dir());
        os_delete_tree(parent);
        os_set_env("PUPPETXR_SCREENSHOT_DIR", dir);
        os_set_env("PUPPETXR_SCREENSHOT_AT", "2");
        OxrVulkanTest::SetUp();
        os_unset_env("PUPPETXR_SCREENSHOT_DIR");
        os_unset_env("PUPPETXR_SCREENSHOT_AT");
    }

    void TearDown () override
    {
        OxrVulkanTest::TearDown();
        char parent[OS_MAX_PATH];
        cstr_format(parent, sizeof(parent), "%s/puppetxr_missing_shots", temp_dir());
        os_delete_tree(parent);
    }
};

TEST_F(OxrScreenshotDirTest, a_missing_screenshot_directory_is_created)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    for (u32 i = 0; i < 3; i++)
    {
        frame();
    }
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_frame_2.png", dir);
    EXPECT_TRUE(os_file_exists(path));
}

// Unity: the render thread runs the frame loop while the main thread polls events and queries
// input; on exit the main thread sees STOPPING, stops the render thread and ends the session
TEST_F(OxrThreadTest, unity_queries_input_on_the_main_thread_while_the_render_thread_runs_frames)
{
    OS_Thread* render = os_thread_create("render", render_thread, this);
    ASSERT_NE(render, nullptr);
    u32            ticks         = 0;
    b8             exit_requested = false;
    XrSessionState state         = XR_SESSION_STATE_FOCUSED;
    while (state != XR_SESSION_STATE_STOPPING)
    {
        XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        while (xr.xrPollEvent(instance, &event) == XR_SUCCESS)
        {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                state = ((XrEventDataSessionStateChanged*)&event)->state;
            }
            event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        }
        u32    ended        = 0;
        XrTime display_time = 0;
        read_shared(ended, display_time);
        if (state == XR_SESSION_STATE_FOCUSED && display_time > 0)
        {
            query_input(display_time);
        }
        ticks++;
        if (!exit_requested && ended >= THREAD_TEST_FRAMES)
        {
            EXPECT_EQ(xr.xrRequestExitSession(session), XR_SUCCESS);
            exit_requested = true;
        }
    }
    os_semaphore_signal(stop);
    os_semaphore_wait(stopped);
    EXPECT_EQ(os_thread_join(render), 0);
    EXPECT_EQ(xr.xrEndSession(session), XR_SUCCESS);
    EXPECT_GT(ticks, 1u);
    u32    ended        = 0;
    XrTime display_time = 0;
    read_shared(ended, display_time);
    EXPECT_GE(ended, (u32)THREAD_TEST_FRAMES);
}

//
// Conformance regressions
//

TEST_F(OxrVulkanTest, static_swapchains_have_one_image_acquired_once)
{
    create_vulkan();
    create_vulkan_session();
    XrSwapchain swapchain = XR_NULL_HANDLE;
    const XrSwapchainCreateInfo info = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, .format = VK_FORMAT_R8G8B8A8_SRGB, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
    ASSERT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_SUCCESS);
    u32 count = 0;
    EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 1u);

    u32 index = 9;
    const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
    EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index), XR_SUCCESS);
    EXPECT_EQ(index, 0u);
    EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);
    EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
    EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index), XR_ERROR_CALL_ORDER_INVALID);
    xr.xrDestroySwapchain(swapchain);
}

TEST_F(OxrVulkanTest, null_vulkan_handles_in_the_binding_are_a_device_error)
{
    create_vulkan();
    XrGraphicsBindingVulkan2KHR binding = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = vk_instance, .physicalDevice = vk_physical, .device = VK_NULL_HANDLE, .queueFamilyIndex = queue_family };
    const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = system };
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    binding.device         = vk_device;
    binding.physicalDevice = VK_NULL_HANDLE;
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    binding.physicalDevice = vk_physical;
    binding.instance       = VK_NULL_HANDLE;
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    EXPECT_EQ(session, XR_NULL_HANDLE);
}

TEST_F(OxrVulkanTest, graphics_sessions_synchronize_on_the_first_frame)
{
    create_vulkan();
    create_vulkan_session();
    drain_events();
    const XrSessionBeginInfo begin_info = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
    ASSERT_EQ(xr.xrBeginSession(session, &begin_info), XR_SUCCESS);
    EXPECT_EQ(drain_events(), XR_SESSION_STATE_UNKNOWN);

    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    EXPECT_FALSE(state.shouldRender);
    ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
    EXPECT_EQ(drain_events(), XR_SESSION_STATE_UNKNOWN);
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    EXPECT_EQ(drain_events(), XR_SESSION_STATE_FOCUSED);

    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    EXPECT_TRUE(state.shouldRender);
}

TEST_F(OxrVulkanTest, exit_before_the_first_frame_passes_synchronized)
{
    create_vulkan();
    create_vulkan_session();
    drain_events();
    const XrSessionBeginInfo begin_info = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
    ASSERT_EQ(xr.xrBeginSession(session, &begin_info), XR_SUCCESS);
    ASSERT_EQ(xr.xrRequestExitSession(session), XR_SUCCESS);

    XrSessionState states[4] = {};
    u32 states_count         = 0;
    XrEventDataBuffer event  = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (xr.xrPollEvent(instance, &event) == XR_SUCCESS && states_count < 4)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            states[states_count++] = ((XrEventDataSessionStateChanged*)&event)->state;
        }
        event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
    ASSERT_EQ(states_count, 2u);
    EXPECT_EQ(states[0], XR_SESSION_STATE_SYNCHRONIZED);
    EXPECT_EQ(states[1], XR_SESSION_STATE_STOPPING);

    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    EXPECT_EQ(drain_events(), XR_SESSION_STATE_UNKNOWN);
}

TEST_F(OxrTest, events_of_destroyed_sessions_are_dropped)
{
    create_headless_instance();
    create_session();
    const XrSession first = session;
    EXPECT_EQ(xr.xrDestroySession(first), XR_SUCCESS);
    session = XR_NULL_HANDLE;
    create_session();

    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (xr.xrPollEvent(instance, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            EXPECT_NE(((XrEventDataSessionStateChanged*)&event)->session, first);
        }
        event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
}

TEST_F(OxrTest, unknown_form_factors_are_unsupported)
{
    ASSERT_EQ(create_instance(nullptr, 0), XR_SUCCESS);
    const XrSystemGetInfo info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = (XrFormFactor)0x7FFFFFF0 };
    EXPECT_EQ(xr.xrGetSystem(instance, &info, &system), XR_ERROR_FORM_FACTOR_UNSUPPORTED);
}

TEST_F(OxrTest, period_only_path_components_are_invalid)
{
    ASSERT_EQ(create_instance(nullptr, 0), XR_SUCCESS);
    XrPath p = XR_NULL_PATH;
    for (const char* bad : { "/../foo", "/./foo", "/foo/..", "/foo/.", "/foo/.../bar", "/." })
    {
        EXPECT_EQ(xr.xrStringToPath(instance, bad, &p), XR_ERROR_PATH_FORMAT_INVALID) << bad;
    }
    for (const char* good : { "/foo.bar", "/.foo", "/foo./bar", "/a/b.c/..d" })
    {
        EXPECT_EQ(xr.xrStringToPath(instance, good, &p), XR_SUCCESS) << good;
    }
}

// Regression, CTS xrSyncActions: syncing no action sets is valid before anything is attached
TEST_F(OxrActionTest, syncing_nothing_before_attach_succeeds)
{
    const XrActionsSyncInfo info   = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 0, .activeActionSets = nullptr };
    const XrResult          result = xr.xrSyncActions(session, &info);
    EXPECT_TRUE(result == XR_SUCCESS || result == XR_SESSION_NOT_FOCUSED) << result;

    const XrActiveActionSet active    = { .actionSet = set, .subactionPath = XR_NULL_PATH };
    const XrActionsSyncInfo with_set  = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active };
    EXPECT_EQ(xr.xrSyncActions(session, &with_set), XR_ERROR_ACTIONSET_NOT_ATTACHED);
}

// Regression, CTS xrSyncActions, spec 11.8: a subaction path no action of the set declared
TEST_F(OxrActionTest, syncing_an_undeclared_subaction_path_is_unsupported)
{
    XrActionSet                 plain    = XR_NULL_HANDLE;
    const XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .actionSetName = "plain", .localizedActionSetName = "Plain", .priority = 0 };
    ASSERT_EQ(xr.xrCreateActionSet(instance, &set_info, &plain), XR_SUCCESS);
    XrAction                 free_action = XR_NULL_HANDLE;
    const XrActionCreateInfo info        = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionName = "free", .actionType = XR_ACTION_TYPE_BOOLEAN_INPUT, .countSubactionPaths = 0, .localizedActionName = "Free" };
    ASSERT_EQ(xr.xrCreateAction(plain, &info, &free_action), XR_SUCCESS);
    const XrActionSet                   sets[]      = { set, plain };
    const XrSessionActionSetsAttachInfo attach_info = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 2, .actionSets = sets };
    ASSERT_EQ(xr.xrAttachSessionActionSets(session, &attach_info), XR_SUCCESS);

    XrActiveActionSet       active = { .actionSet = plain, .subactionPath = hands[0] };
    const XrActionsSyncInfo sync   = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active };
    EXPECT_EQ(xr.xrSyncActions(session, &sync), XR_ERROR_PATH_UNSUPPORTED);
    active.actionSet = set;
    EXPECT_NE(xr.xrSyncActions(session, &sync), XR_ERROR_PATH_UNSUPPORTED);
}

// Regression, CTS ActionSpaces, spec 7.2.1: a space without a subaction path keeps the device
// it was tracking until a sync, and the runtime keeps that device while it stays bound
TEST_F(OxrActionTest, action_spaces_without_subaction_path_stick_to_their_device)
{
    suggest_touch_bindings();
    attach();
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_device_active(sim, SIM_Device::LeftHand, true);
    sim_set_device_active(sim, SIM_Device::RightHand, true);
    sim_set_device_pose(sim, SIM_Device::LeftHand, { .position = { -0.3f, 1.0f, -0.3f }, .orientation = zm::QUAT_IDENTITY });
    sim_set_device_pose(sim, SIM_Device::RightHand, { .position = { 0.3f, 1.0f, -0.3f }, .orientation = zm::QUAT_IDENTITY });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);

    XrSpace                       any  = XR_NULL_HANDLE;
    const XrActionSpaceCreateInfo info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = grip, .subactionPath = XR_NULL_PATH, .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    ASSERT_EQ(xr.xrCreateActionSpace(session, &info, &any), XR_SUCCESS);
    const XrSpace      stage    = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrFrameState state    = frame();
    XrSpaceLocation    location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(any, stage, state.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_NEAR(location.pose.position.x, -0.3f, 1e-5f);

    sim_set_device_active(sim, SIM_Device::LeftHand, false);
    const XrFrameState later = frame();
    ASSERT_EQ(xr.xrLocateSpace(any, stage, later.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_EQ(location.locationFlags, 0u);
    ASSERT_EQ(sync(), XR_SUCCESS);
    ASSERT_EQ(xr.xrLocateSpace(any, stage, later.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_EQ(location.locationFlags, 0u);

    sim_set_device_active(sim, SIM_Device::LeftHand, true);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    const XrFrameState back = frame();
    ASSERT_EQ(xr.xrLocateSpace(any, stage, back.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_NE(location.locationFlags, 0u);
    EXPECT_NEAR(location.pose.position.x, -0.3f, 1e-5f);
}

// Regression, CTS ActionSpaces, spec 11.3 xrDestroyAction: the resources outlive the handle,
// action spaces stay locatable
TEST_F(OxrActionTest, destroyed_actions_keep_their_spaces_locatable)
{
    suggest_touch_bindings();
    attach();
    begin_session();
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    XrSpace                       space = XR_NULL_HANDLE;
    const XrActionSpaceCreateInfo info  = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = grip, .subactionPath = hands[0], .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    ASSERT_EQ(xr.xrCreateActionSpace(session, &info, &space), XR_SUCCESS);

    ASSERT_EQ(xr.xrDestroyAction(grip), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroyAction(grip), XR_ERROR_HANDLE_INVALID);
    const XrActionStateGetInfo get_info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = grip };
    XrActionStatePose          pose     = { .type = XR_TYPE_ACTION_STATE_POSE };
    EXPECT_EQ(xr.xrGetActionStatePose(session, &get_info, &pose), XR_ERROR_HANDLE_INVALID);

    const XrSpace      stage    = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrFrameState state    = frame();
    XrSpaceLocation    location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(space, stage, state.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_NE(location.locationFlags, 0u);
    ASSERT_EQ(sync(), XR_SUCCESS);
    ASSERT_EQ(xr.xrLocateSpace(space, stage, state.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_NE(location.locationFlags, 0u);
}

TEST_F(OxrActionTest, actions_without_subaction_paths_reject_subaction_queries)
{
    XrAction           plain = XR_NULL_HANDLE;
    const XrActionCreateInfo info  = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionName = "plain", .actionType = XR_ACTION_TYPE_FLOAT_INPUT, .localizedActionName = "Plain" };
    ASSERT_EQ(xr.xrCreateAction(set, &info, &plain), XR_SUCCESS);
    const XrActionSuggestedBinding bindings[] = { { plain, path("/user/hand/left/input/trigger/value") }, { plain, path("/user/hand/right/input/trigger/value") } };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = 2, .suggestedBindings = bindings };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    attach();
    begin_session();
    sim_set_input(oxr_get_sim(), SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 0.5f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);

    XrActionStateFloat         state    = { .type = XR_TYPE_ACTION_STATE_FLOAT };
    XrActionStateGetInfo       get_info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = plain, .subactionPath = hands[0] };
    EXPECT_EQ(xr.xrGetActionStateFloat(session, &get_info, &state), XR_ERROR_PATH_UNSUPPORTED);
    get_info.subactionPath = XR_NULL_PATH;
    EXPECT_EQ(xr.xrGetActionStateFloat(session, &get_info, &state), XR_SUCCESS);
    EXPECT_TRUE(state.isActive);
    EXPECT_FLOAT_EQ(state.currentState, 0.5f);
}

// OVRPlugin locates at time 0, which Meta's runtime takes as the latest time; it then could not
// tell its tracking origin, and the conformance tests want 0 rejected, so it is opt-in
// XR_OCULUS_audio_device_guid: the PC's default devices are the simulated headset's. OVRManager
// asks every frame and logged a warning with a stack trace each time it was missing.
TEST_F(OxrTest, audio_device_guids_are_the_pcs_default_devices)
{
    const char*    extensions[] = { XR_MND_HEADLESS_EXTENSION_NAME, "XR_OCULUS_audio_device_guid" };
    const XrResult created      = create_instance(extensions, 2);
    if (created == XR_ERROR_EXTENSION_NOT_PRESENT)
    {
        EXPECT_FALSE(os_has_audio_device_ids());
        GTEST_SKIP() << "Windows only";
    }
    ASSERT_EQ(created, XR_SUCCESS);
    typedef XrResult (XRAPI_PTR * PFN_AudioGuid)(XrInstance, wchar_t*);
    PFN_AudioGuid output = nullptr;
    PFN_AudioGuid input  = nullptr;
    ASSERT_EQ(gipa(instance, "xrGetAudioOutputDeviceGuidOculus", (PFN_xrVoidFunction*)&output), XR_SUCCESS);
    ASSERT_EQ(gipa(instance, "xrGetAudioInputDeviceGuidOculus", (PFN_xrVoidFunction*)&input), XR_SUCCESS);
    for (PFN_AudioGuid get : { output, input })
    {
        wchar_t buffer[128];
        for (wchar_t& c : buffer)
        {
            c = L'x';
        }
        EXPECT_EQ(get(instance, buffer), XR_SUCCESS);
        b8 terminated = false;
        for (const wchar_t c : buffer)
        {
            terminated |= c == 0;
        }
        EXPECT_TRUE(terminated);
        EXPECT_EQ(get(instance, nullptr), XR_ERROR_VALIDATION_FAILURE);
        EXPECT_EQ(get((XrInstance)(uintptr_t)0x77, buffer), XR_ERROR_HANDLE_INVALID);
    }
}

// OVRPlugin locates its controllers at time 0 while Unity draws the frame at the predicted
// display time: both must give the same pose, or a moving controller shows twice
TEST_F(OxrTest, time_zero_is_the_frame_being_drawn_when_accepted)
{
    os_set_env("PUPPETXR_ACCEPT_TIME_ZERO", "1");
    defer { os_unset_env("PUPPETXR_ACCEPT_TIME_ZERO"); };
    create_headless_instance();
    create_session();
    begin_session();
    const XrSpace stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrSpace view  = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    SIM_Sim*      sim   = oxr_get_sim();
    for (u32 i = 0; i < 4; i++)
    {
        sim_set_device_pose(sim, SIM_Device::Head, { .position = { 0.1f * (f32)i, 1.6f, 0.0f }, .orientation = zm::QUAT_IDENTITY });
        const XrFrameState frame_state = frame();
        XrSpaceLocation    at_zero     = { .type = XR_TYPE_SPACE_LOCATION };
        XrSpaceLocation    at_display  = { .type = XR_TYPE_SPACE_LOCATION };
        ASSERT_EQ(xr.xrLocateSpace(view, stage, 0, &at_zero), XR_SUCCESS);
        ASSERT_EQ(xr.xrLocateSpace(view, stage, frame_state.predictedDisplayTime, &at_display), XR_SUCCESS);
        EXPECT_FLOAT_EQ(at_zero.pose.position.x, at_display.pose.position.x) << "frame " << i;
    }
}

TEST_F(OxrTest, time_zero_is_the_latest_time_when_accepted)
{
    os_set_env("PUPPETXR_ACCEPT_TIME_ZERO", "1");
    defer { os_unset_env("PUPPETXR_ACCEPT_TIME_ZERO"); };
    create_headless_instance();
    create_session();
    begin_session();
    const XrFrameState frame_state = frame();
    const XrSpace      stage       = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrSpace      view        = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);

    XrSpaceLocation at_zero = { .type = XR_TYPE_SPACE_LOCATION };
    XrSpaceLocation at_now  = { .type = XR_TYPE_SPACE_LOCATION };
    EXPECT_EQ(xr.xrLocateSpace(view, stage, 0, &at_zero), XR_SUCCESS);
    EXPECT_EQ(xr.xrLocateSpace(view, stage, frame_state.predictedDisplayTime, &at_now), XR_SUCCESS);
    EXPECT_TRUE(at_zero.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    EXPECT_NEAR(at_zero.pose.position.y, at_now.pose.position.y, 1e-5f);
    EXPECT_EQ(xr.xrLocateSpace(view, stage, -5, &at_zero), XR_ERROR_TIME_INVALID);

    XrViewLocateInfo info  = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = 0, .space = stage };
    XrViewState      state = { .type = XR_TYPE_VIEW_STATE };
    XrView           views[2] = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
    u32              count = 0;
    EXPECT_EQ(xr.xrLocateViews(session, &info, &state, 2, &count, views), XR_SUCCESS);
    EXPECT_TRUE(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
}

TEST_F(OxrTest, locate_spaces_extension_entry_point)
{
    const char* extensions[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_KHR_LOCATE_SPACES_EXTENSION_NAME };
    ASSERT_EQ(create_instance(extensions, 2, XR_API_VERSION_1_0), XR_SUCCESS);
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrLocateSpacesKHR", &fn), XR_SUCCESS);
    EXPECT_NE(fn, nullptr);
}

//
// XR_EXT_stationary_reference_space and its preview XR_EXTX2_stationary_reference_space
//

#define XR_EXTX2_STATIONARY_REFERENCE_SPACE_TEST_NAME "XR_EXTX2_stationary_reference_space"

namespace
{
// The preview's structs have the registry's layout and type values
typedef XrResult (XRAPI_PTR* PFN_xrGetStationaryReferenceSpaceIdEXTX2)(XrSession session, const XrStationaryReferenceSpaceGenerationIdGetInfoEXT* info, XrStationaryReferenceSpaceGenerationIdResultEXT* result);

struct OxrStationaryTest : OxrTest
{
    OxrStationaryTest ()
    {
        extension = XR_EXT_STATIONARY_REFERENCE_SPACE_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrTest::SetUp();
        create_headless_instance();
        create_session();
    }
};

struct OxrStationaryPreviewTest : OxrStationaryTest
{
    OxrStationaryPreviewTest ()
    {
        extension = XR_EXTX2_STATIONARY_REFERENCE_SPACE_TEST_NAME;
    }
};

b8 lists_stationary (const Xr& xr, XrSession session)
{
    u32 count = 0;
    EXPECT_EQ(xr.xrEnumerateReferenceSpaces(session, 0, &count, nullptr), XR_SUCCESS);
    XrReferenceSpaceType types[8] = {};
    EXPECT_LE(count, 8u);
    EXPECT_EQ(xr.xrEnumerateReferenceSpaces(session, count, &count, types), XR_SUCCESS);
    b8 found = false;
    for (u32 i = 0; i < count; i++)
    {
        found = found || types[i] == XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT;
    }
    return found;
}
} // namespace

TEST_F(OxrTest, stationary_space_needs_its_extension)
{
    create_headless_instance();
    create_session();
    EXPECT_FALSE(lists_stationary(xr, session));
    XrSpace                          space = XR_NULL_HANDLE;
    const XrReferenceSpaceCreateInfo info  = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT, .poseInReferenceSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    EXPECT_EQ(xr.xrCreateReferenceSpace(session, &info, &space), XR_ERROR_REFERENCE_SPACE_UNSUPPORTED);
    XrExtent2Df bounds = {};
    EXPECT_EQ(xr.xrGetReferenceSpaceBoundsRect(session, XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT, &bounds), XR_ERROR_REFERENCE_SPACE_UNSUPPORTED);
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrGetStationaryReferenceSpaceGenerationIdEXT", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrGetStationaryReferenceSpaceIdEXTX2", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
}

TEST_F(OxrStationaryTest, stationary_space_stays_where_local_started)
{
    EXPECT_TRUE(lists_stationary(xr, session));
    begin_session();
    const XrSpace      stationary = reference_space(XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT);
    const XrSpace      local      = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    const XrSpace      stage      = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrFrameState before     = frame();
    drain_events();
    XrSpaceLocation start = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(local, stage, before.predictedDisplayTime, &start), XR_SUCCESS);
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(stationary, local, before.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
    EXPECT_NEAR(location.pose.position.x, 0.0f, 1e-6f);
    EXPECT_NEAR(location.pose.orientation.w, 1.0f, 1e-6f);

    SIM_Sim* sim = oxr_get_sim();
    sim_set_device_pose(sim, SIM_Device::Head, { .position = { 1.0f, 1.6f, -2.0f }, .orientation = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.5f) });
    sim_recenter(sim);
    const XrFrameState after = frame();
    ASSERT_EQ(xr.xrLocateSpace(stationary, stage, after.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_EQ(location.pose.position.x, start.pose.position.x);
    EXPECT_EQ(location.pose.position.z, start.pose.position.z);
    EXPECT_EQ(location.pose.orientation.y, start.pose.orientation.y);
    ASSERT_EQ(xr.xrLocateSpace(local, stage, after.predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_NEAR(location.pose.position.x, 1.0f, 1e-5f);

    while (true)
    {
        XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        if (xr.xrPollEvent(instance, &event) != XR_SUCCESS)
        {
            break;
        }
        if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
        {
            EXPECT_NE(((const XrEventDataReferenceSpaceChangePending*)&event)->referenceSpaceType, XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT);
        }
    }

    XrExtent2Df bounds = {};
    EXPECT_EQ(xr.xrGetReferenceSpaceBoundsRect(session, XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT, &bounds), XR_SPACE_BOUNDS_UNAVAILABLE);
}

TEST_F(OxrStationaryTest, generation_id_is_constant_and_validated)
{
    PFN_xrGetStationaryReferenceSpaceGenerationIdEXT get = nullptr;
    ASSERT_EQ(gipa(instance, "xrGetStationaryReferenceSpaceGenerationIdEXT", (PFN_xrVoidFunction*)&get), XR_SUCCESS);
    PFN_xrVoidFunction preview = nullptr;
    EXPECT_EQ(gipa(instance, "xrGetStationaryReferenceSpaceIdEXTX2", &preview), XR_ERROR_FUNCTION_UNSUPPORTED);

    const XrStationaryReferenceSpaceGenerationIdGetInfoEXT info   = { .type = XR_TYPE_STATIONARY_REFERENCE_SPACE_GENERATION_ID_GET_INFO_EXT };
    XrStationaryReferenceSpaceGenerationIdResultEXT        result = { .type = XR_TYPE_STATIONARY_REFERENCE_SPACE_GENERATION_ID_RESULT_EXT };
    EXPECT_EQ(get(session, &info, &result), XR_ERROR_SESSION_NOT_RUNNING);
    begin_session();
    ASSERT_EQ(get(session, &info, &result), XR_SUCCESS);
    const XrUuid none = {};
    EXPECT_NE(memcmp(&result.generationId, &none, sizeof(XrUuid)), 0);

    XrStationaryReferenceSpaceGenerationIdResultEXT again = { .type = XR_TYPE_STATIONARY_REFERENCE_SPACE_GENERATION_ID_RESULT_EXT };
    sim_recenter(oxr_get_sim());
    frame();
    ASSERT_EQ(get(session, nullptr, &again), XR_SUCCESS);
    EXPECT_EQ(memcmp(&result.generationId, &again.generationId, sizeof(XrUuid)), 0);

    const XrStationaryReferenceSpaceGenerationIdGetInfoEXT wrong_info = { .type = XR_TYPE_SPACE_LOCATION };
    EXPECT_EQ(get(session, &wrong_info, &again), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(get(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    XrStationaryReferenceSpaceGenerationIdResultEXT wrong_result = { .type = XR_TYPE_STATIONARY_REFERENCE_SPACE_GENERATION_ID_GET_INFO_EXT };
    EXPECT_EQ(get(session, &info, &wrong_result), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(get((XrSession)(uintptr_t)0x1234, &info, &again), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrStationaryPreviewTest, preview_has_the_space_and_its_own_function_name)
{
    EXPECT_TRUE(lists_stationary(xr, session));
    PFN_xrVoidFunction registry = nullptr;
    EXPECT_EQ(gipa(instance, "xrGetStationaryReferenceSpaceGenerationIdEXT", &registry), XR_ERROR_FUNCTION_UNSUPPORTED);
    PFN_xrGetStationaryReferenceSpaceIdEXTX2 get = nullptr;
    ASSERT_EQ(gipa(instance, "xrGetStationaryReferenceSpaceIdEXTX2", (PFN_xrVoidFunction*)&get), XR_SUCCESS);

    begin_session();
    reference_space(XR_REFERENCE_SPACE_TYPE_STATIONARY_EXT);
    XrStationaryReferenceSpaceGenerationIdResultEXT result = { .type = XR_TYPE_STATIONARY_REFERENCE_SPACE_GENERATION_ID_RESULT_EXT };
    ASSERT_EQ(get(session, nullptr, &result), XR_SUCCESS);
    const XrUuid none = {};
    EXPECT_NE(memcmp(&result.generationId, &none, sizeof(XrUuid)), 0);
    EXPECT_EQ(get(session, nullptr, nullptr), XR_ERROR_VALIDATION_FAILURE);
}

//
// XR_META_performance_metrics
//

namespace
{
struct OxrPerfMetricsTest : OxrTest
{
    PFN_xrEnumeratePerformanceMetricsCounterPathsMETA enumerate = nullptr;
    PFN_xrSetPerformanceMetricsStateMETA              set_state = nullptr;
    PFN_xrGetPerformanceMetricsStateMETA              get_state = nullptr;
    PFN_xrQueryPerformanceMetricsCounterMETA          query     = nullptr;

    OxrPerfMetricsTest ()
    {
        extension = XR_META_PERFORMANCE_METRICS_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrTest::SetUp();
        create_headless_instance();
        create_session();
        ASSERT_EQ(gipa(instance, "xrEnumeratePerformanceMetricsCounterPathsMETA", (PFN_xrVoidFunction*)&enumerate), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrSetPerformanceMetricsStateMETA", (PFN_xrVoidFunction*)&set_state), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetPerformanceMetricsStateMETA", (PFN_xrVoidFunction*)&get_state), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrQueryPerformanceMetricsCounterMETA", (PFN_xrVoidFunction*)&query), XR_SUCCESS);
    }

    XrResult enable (b8 on) const
    {
        const XrPerformanceMetricsStateMETA state = { .type = XR_TYPE_PERFORMANCE_METRICS_STATE_META, .enabled = (XrBool32)(on ? XR_TRUE : XR_FALSE) };
        return set_state(session, &state);
    }

    XrPerformanceMetricsCounterMETA counter (const char* name) const
    {
        XrPerformanceMetricsCounterMETA out = { .type = XR_TYPE_PERFORMANCE_METRICS_COUNTER_META };
        EXPECT_EQ(query(session, path(name), &out), XR_SUCCESS) << name;
        return out;
    }
};
} // namespace

TEST_F(OxrTest, performance_metrics_need_their_extension)
{
    create_headless_instance();
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrEnumeratePerformanceMetricsCounterPathsMETA", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrQueryPerformanceMetricsCounterMETA", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
}

TEST_F(OxrPerfMetricsTest, counter_paths_are_enumerated)
{
    u32 count = 0;
    ASSERT_EQ(enumerate(instance, 0, &count, nullptr), XR_SUCCESS);
    ASSERT_EQ(count, 4u);
    XrPath paths[4] = {};
    u32    small    = 0;
    EXPECT_EQ(enumerate(instance, 2, &small, paths), XR_ERROR_SIZE_INSUFFICIENT);
    EXPECT_EQ(small, 4u);
    ASSERT_EQ(enumerate(instance, 4, &count, paths), XR_SUCCESS);
    const char* expected[] = { "/perfmetrics_meta/app/cpu_frametime", "/perfmetrics_meta/app/gpu_frametime", "/perfmetrics_meta/compositor/dropped_frame_count", "/perfmetrics_meta/compositor/spacewarp_mode" };
    for (u32 i = 0; i < 4; i++)
    {
        char     name[XR_MAX_PATH_LENGTH] = {};
        uint32_t length                   = 0;
        ASSERT_EQ(xr.xrPathToString(instance, paths[i], sizeof(name), &length, name), XR_SUCCESS);
        EXPECT_STREQ(name, expected[i]);
    }
    EXPECT_EQ(enumerate(instance, 0, nullptr, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(enumerate((XrInstance)(uintptr_t)0x1234, 0, &count, nullptr), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrPerfMetricsTest, queries_need_the_metrics_enabled_and_a_listed_path)
{
    XrPerformanceMetricsCounterMETA out = { .type = XR_TYPE_PERFORMANCE_METRICS_COUNTER_META };
    EXPECT_EQ(query(session, path("/perfmetrics_meta/app/cpu_frametime"), &out), XR_ERROR_VALIDATION_FAILURE);

    XrPerformanceMetricsStateMETA state = { .type = XR_TYPE_PERFORMANCE_METRICS_STATE_META, .enabled = XR_TRUE };
    ASSERT_EQ(get_state(session, &state), XR_SUCCESS);
    EXPECT_FALSE(state.enabled);
    ASSERT_EQ(enable(true), XR_SUCCESS);
    ASSERT_EQ(get_state(session, &state), XR_SUCCESS);
    EXPECT_TRUE(state.enabled);

    EXPECT_EQ(query(session, path("/perfmetrics_meta/app/cpu_frametime"), &out), XR_SUCCESS);
    EXPECT_EQ(query(session, path("/user/hand/left"), &out), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(query(session, path("/perfmetrics_meta/device/gpu_utilization"), &out), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(query(session, (XrPath)99999, &out), XR_ERROR_PATH_INVALID);
    EXPECT_EQ(query(session, XR_NULL_PATH, &out), XR_ERROR_PATH_INVALID);
    EXPECT_EQ(query(session, path("/perfmetrics_meta/app/cpu_frametime"), nullptr), XR_ERROR_VALIDATION_FAILURE);
    XrPerformanceMetricsCounterMETA wrong = { .type = XR_TYPE_PERFORMANCE_METRICS_STATE_META };
    EXPECT_EQ(query(session, path("/perfmetrics_meta/app/cpu_frametime"), &wrong), XR_ERROR_VALIDATION_FAILURE);

    const XrPerformanceMetricsStateMETA bad = { .type = XR_TYPE_PERFORMANCE_METRICS_COUNTER_META, .enabled = XR_TRUE };
    EXPECT_EQ(set_state(session, &bad), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(set_state(session, nullptr), XR_ERROR_VALIDATION_FAILURE);
    XrPerformanceMetricsStateMETA bad_out = { .type = XR_TYPE_PERFORMANCE_METRICS_COUNTER_META };
    EXPECT_EQ(get_state(session, &bad_out), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(get_state((XrSession)(uintptr_t)0x1234, &state), XR_ERROR_HANDLE_INVALID);

    ASSERT_EQ(enable(false), XR_SUCCESS);
    EXPECT_EQ(query(session, path("/perfmetrics_meta/app/cpu_frametime"), &out), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrPerfMetricsTest, counters_follow_the_simulated_frames)
{
    ASSERT_EQ(enable(true), XR_SUCCESS);
    const XrPerformanceMetricsCounterMETA before = counter("/perfmetrics_meta/app/cpu_frametime");
    EXPECT_EQ(before.counterFlags, XR_PERFORMANCE_METRICS_COUNTER_ANY_VALUE_VALID_BIT_META | XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META);
    EXPECT_EQ(before.counterUnit, XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META);
    EXPECT_EQ(before.floatValue, 0.0f);

    begin_session();
    frame();
    frame();
    const f32 period_ms = (f32)sim_display_period(oxr_get_sim()) * 1e-6f;
    EXPECT_FLOAT_EQ(counter("/perfmetrics_meta/app/cpu_frametime").floatValue, period_ms);
    EXPECT_FLOAT_EQ(counter("/perfmetrics_meta/app/gpu_frametime").floatValue, period_ms);

    const XrPerformanceMetricsCounterMETA dropped = counter("/perfmetrics_meta/compositor/dropped_frame_count");
    EXPECT_EQ(dropped.counterFlags, XR_PERFORMANCE_METRICS_COUNTER_ANY_VALUE_VALID_BIT_META | XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META);
    EXPECT_EQ(dropped.counterUnit, XR_PERFORMANCE_METRICS_COUNTER_UNIT_GENERIC_META);
    EXPECT_EQ(dropped.uintValue, 0u);
    XrFrameState          state      = { .type = XR_TYPE_FRAME_STATE };
    const XrFrameWaitInfo wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
    const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_FRAME_DISCARDED);
    EXPECT_EQ(counter("/perfmetrics_meta/compositor/dropped_frame_count").uintValue, 1u);
    EXPECT_EQ(counter("/perfmetrics_meta/compositor/spacewarp_mode").uintValue, 0u);
}

//
// XR_EXTX1_event_channel
//

namespace
{
const XrStructureType EC_CREATE_INFO = (XrStructureType)OXR_TYPE_EVENT_CHANNEL_CREATE_INFO_EXTX1;
const XrStructureType EC_TARGET      = (XrStructureType)OXR_TYPE_EVENT_CHANNEL_TARGET_EXTX1;
const XrStructureType EC_SELECT_INFO = (XrStructureType)OXR_TYPE_SELECT_EVENT_CHANNEL_INFO_EXTX1;

struct OxrEventChannelTest : OxrTest
{
    PFN_xrCreateEventChannelEXTX1     create_channel  = nullptr;
    PFN_xrDestroyEventChannelEXTX1    destroy_channel = nullptr;
    PFN_xrPollEventChannelEXTX1       poll_channel    = nullptr;
    PFN_xrSelectEventChannelEXTX1     select_channel  = nullptr;
    PFN_xrSetDefaultEventChannelEXTX1 set_default     = nullptr;

    OxrEventChannelTest ()
    {
        extension = "XR_EXTX1_event_channel";
    }

    void SetUp () override
    {
        OxrTest::SetUp();
        create_headless_instance();
        ASSERT_EQ(gipa(instance, "xrCreateEventChannelEXTX1", (PFN_xrVoidFunction*)&create_channel), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrDestroyEventChannelEXTX1", (PFN_xrVoidFunction*)&destroy_channel), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrPollEventChannelEXTX1", (PFN_xrVoidFunction*)&poll_channel), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrSelectEventChannelEXTX1", (PFN_xrVoidFunction*)&select_channel), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrSetDefaultEventChannelEXTX1", (PFN_xrVoidFunction*)&set_default), XR_SUCCESS);
    }

    u64 channel () const
    {
        const OXR_EventChannelCreateInfoEXTX1 info = { .type = EC_CREATE_INFO };
        u64                                   out  = 0;
        EXPECT_EQ(create_channel(instance, &info, &out), XR_SUCCESS);
        EXPECT_NE(out, 0u);
        return out;
    }

    // Session state changes queued in a channel, in order
    u32 states_in (u64 from, XrSessionState* states, u32 capacity) const
    {
        u32 count = 0;
        while (true)
        {
            XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            if (poll_channel(from, &event) != XR_SUCCESS)
            {
                return count;
            }
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED && count < capacity)
            {
                states[count++] = ((const XrEventDataSessionStateChanged*)&event)->state;
            }
        }
    }

    b8 instance_queue_empty () const
    {
        XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        return xr.xrPollEvent(instance, &event) == XR_EVENT_UNAVAILABLE;
    }

    XrResult select (const u64* channels, u32 count, XrDuration timeout, u32& out_index) const
    {
        OXR_SelectEventChannelInfoEXTX1 info = { .type = EC_SELECT_INFO, .timeout = timeout, .eventChannelCount = count, .eventChannels = channels };
        return select_channel(instance, &info, &out_index);
    }
};
} // namespace

TEST_F(OxrTest, event_channels_need_their_extension)
{
    create_headless_instance();
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrCreateEventChannelEXTX1", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrSelectEventChannelEXTX1", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
}

TEST_F(OxrEventChannelTest, channels_are_created_validated_and_destroyed)
{
    const OXR_EventChannelCreateInfoEXTX1 info  = { .type = EC_CREATE_INFO };
    const OXR_EventChannelCreateInfoEXTX1 wrong = { .type = XR_TYPE_SESSION_CREATE_INFO };
    u64                                   out   = 0;
    EXPECT_EQ(create_channel(instance, nullptr, &out), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_channel(instance, &wrong, &out), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_channel(instance, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_channel((XrInstance)(uintptr_t)0x1234, &info, &out), XR_ERROR_HANDLE_INVALID);

    u64 channels[OXR_MAX_EVENT_CHANNELS] = {};
    for (u64& c : channels)
    {
        c = channel();
    }
    EXPECT_EQ(create_channel(instance, &info, &out), XR_ERROR_LIMIT_REACHED);
    EXPECT_EQ(destroy_channel(channels[3]), XR_SUCCESS);
    EXPECT_EQ(destroy_channel(channels[3]), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(destroy_channel(0x1234), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(create_channel(instance, &info, &out), XR_SUCCESS);

    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    EXPECT_EQ(poll_channel(channels[0], &event), XR_EVENT_UNAVAILABLE);
    EXPECT_EQ(poll_channel(channels[3], &event), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(poll_channel(channels[0], nullptr), XR_ERROR_VALIDATION_FAILURE);
    XrEventDataBuffer bad = { .type = XR_TYPE_SESSION_CREATE_INFO };
    EXPECT_EQ(poll_channel(channels[0], &bad), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(set_default(instance, channels[3]), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrEventChannelTest, the_default_channel_takes_new_events)
{
    const u64 a = channel();
    ASSERT_EQ(set_default(instance, a), XR_SUCCESS);
    create_session();
    EXPECT_TRUE(instance_queue_empty());
    XrSessionState states[4] = {};
    ASSERT_EQ(states_in(a, states, 4), 2u);
    EXPECT_EQ(states[0], XR_SESSION_STATE_IDLE);
    EXPECT_EQ(states[1], XR_SESSION_STATE_READY);

    ASSERT_EQ(set_default(instance, 0), XR_SUCCESS);
    const XrSessionBeginInfo begin = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
    ASSERT_EQ(xr.xrBeginSession(session, &begin), XR_SUCCESS);
    EXPECT_EQ(states_in(a, states, 4), 0u);
    EXPECT_EQ(drain_events(), XR_SESSION_STATE_FOCUSED);
}

TEST_F(OxrEventChannelTest, a_session_target_wins_over_the_default)
{
    const u64 a = channel();
    const u64 b = channel();
    ASSERT_EQ(set_default(instance, a), XR_SUCCESS);
    OXR_EventChannelTargetEXTX1 target = { .type = EC_TARGET, .channel = 0x1234 };
    XrSessionCreateInfo         info   = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &target, .systemId = system };
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_HANDLE_INVALID);
    target.channel = b;
    ASSERT_EQ(xr.xrCreateSession(instance, &info, &session), XR_SUCCESS);

    XrSessionState states[4] = {};
    EXPECT_EQ(states_in(a, states, 4), 0u);
    EXPECT_TRUE(instance_queue_empty());
    u32       index    = 99;
    const u64 both[2]  = { a, b };
    ASSERT_EQ(select(both, 2, 0, index), XR_SUCCESS);
    EXPECT_EQ(index, 1u);
    ASSERT_EQ(states_in(b, states, 4), 2u);
    EXPECT_EQ(states[1], XR_SESSION_STATE_READY);

    // Destroying the target sends the session's events to the default again
    ASSERT_EQ(destroy_channel(b), XR_SUCCESS);
    const XrSessionBeginInfo begin = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
    ASSERT_EQ(xr.xrBeginSession(session, &begin), XR_SUCCESS);
    ASSERT_EQ(states_in(a, states, 4), 3u);
    EXPECT_EQ(states[2], XR_SESSION_STATE_FOCUSED);
}

TEST_F(OxrEventChannelTest, select_waits_for_an_event_or_times_out)
{
    const u64 a       = channel();
    const u64 b       = channel();
    const u64 both[2] = { a, b };
    u32       index   = 99;
    EXPECT_EQ(select(both, 2, 0, index), XR_TIMEOUT_EXPIRED);
    EXPECT_EQ(select(both, 2, 2000000, index), XR_TIMEOUT_EXPIRED);

    EXPECT_EQ(select(both, 0, 0, index), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(select(nullptr, 2, 0, index), XR_ERROR_VALIDATION_FAILURE);
    const u64 stale[2] = { a, 0x1234 };
    EXPECT_EQ(select(stale, 2, 0, index), XR_ERROR_HANDLE_INVALID);
    OXR_SelectEventChannelInfoEXTX1 info = { .type = EC_SELECT_INFO, .flags = 1, .eventChannelCount = 2, .eventChannels = both };
    EXPECT_EQ(select_channel(instance, &info, &index), XR_ERROR_VALIDATION_FAILURE);
    info.flags = 0;
    EXPECT_EQ(select_channel(instance, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    info.type = XR_TYPE_SESSION_CREATE_INFO;
    EXPECT_EQ(select_channel(instance, &info, &index), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(select_channel(instance, nullptr, &index), XR_ERROR_VALIDATION_FAILURE);

    ASSERT_EQ(set_default(instance, a), XR_SUCCESS);
    create_session();
    ASSERT_EQ(select(both, 2, XR_INFINITE_DURATION, index), XR_SUCCESS);
    EXPECT_EQ(index, 0u);

    // A destroyed channel drops what it held
    ASSERT_EQ(destroy_channel(a), XR_SUCCESS);
    EXPECT_EQ(select(&b, 1, 0, index), XR_TIMEOUT_EXPIRED);
    EXPECT_TRUE(instance_queue_empty());
}

//
// Scene: XR_FB_spatial_entity, _query, _container, XR_FB_scene, _capture, XR_META_spatial_entity_discovery, _mesh, XR_EXT_future
//

#define SCENE_FUNCTIONS(X)                     \
    X(xrCreateSpatialAnchorFB)                 \
    X(xrGetSpaceUuidFB)                        \
    X(xrEnumerateSpaceSupportedComponentsFB)   \
    X(xrSetSpaceComponentStatusFB)             \
    X(xrGetSpaceComponentStatusFB)             \
    X(xrQuerySpacesFB)                         \
    X(xrRetrieveSpaceQueryResultsFB)           \
    X(xrGetSpaceContainerFB)                   \
    X(xrGetSpaceBoundingBox2DFB)               \
    X(xrGetSpaceBoundingBox3DFB)               \
    X(xrGetSpaceSemanticLabelsFB)              \
    X(xrGetSpaceBoundary2DFB)                  \
    X(xrGetSpaceRoomLayoutFB)                  \
    X(xrRequestSceneCaptureFB)                 \
    X(xrDiscoverSpacesMETA)                    \
    X(xrRetrieveSpaceDiscoveryResultsMETA)     \
    X(xrGetSpaceTriangleMeshMETA)              \
    X(xrPollFutureEXT)                         \
    X(xrCancelFutureEXT)

namespace
{

struct SceneXr
{
#define DECLARE(fn) PFN_##fn fn;
    SCENE_FUNCTIONS(DECLARE)
#undef DECLARE
};

} // namespace

static const char* const scene_extensions[] = {
    XR_MND_HEADLESS_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_CONTAINER_EXTENSION_NAME,
    XR_FB_SCENE_EXTENSION_NAME,
    XR_FB_SCENE_CAPTURE_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_DISCOVERY_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_MESH_EXTENSION_NAME,
    XR_EXT_FUTURE_EXTENSION_NAME,
};

static b8 uuid_is (const XrUuidEXT& uuid, const char* hex)
{
    W_Uuid expected = {};
    return w_uuid_from_string(hex, expected) && memory_equal(uuid.data, expected.bytes, XR_UUID_SIZE_EXT);
}

static const char* test_entity_uuid (u32 index)
{
    static char text[33];
    cstr_format(text, sizeof(text), "a00000000000000000000000000000%02x", index);
    return text;
}

namespace
{

struct OxrSceneTest : OxrTest
{
    SceneXr            scene    = {};
    const char* const* start_extensions       = scene_extensions; // derived fixtures add theirs in their constructor
    u32                start_extensions_count = ARRAY_SIZE(scene_extensions);
    char    rooms[OS_MAX_PATH] = {};
    XrSpace stage    = XR_NULL_HANDLE;

    void SetUp () override
    {
        OxrTest::SetUp();
        ASSERT_TRUE(os_create_temp_directory("puppetxr_scene_rooms", rooms, sizeof(rooms)));
        TestRoom room = {};
        test_room_build(room);
        ASSERT_TRUE(test_room_write(room, rooms, "test_box"));
        os_set_env("PUPPETXR_ROOMS_DIR", rooms);
        os_set_env("PUPPETXR_ROOM", "test_box");
    }

    void TearDown () override
    {
        OxrTest::TearDown();
        os_delete_tree(rooms);
        os_unset_env("PUPPETXR_ROOMS_DIR");
        os_unset_env("PUPPETXR_ROOM");
    }

    void start (b8 begin = true)
    {
        ASSERT_EQ(create_instance(start_extensions, start_extensions_count), XR_SUCCESS);
#define LOAD(fn) ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&scene.fn), XR_SUCCESS) << #fn;
        SCENE_FUNCTIONS(LOAD)
#undef LOAD
        const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
        ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);
        create_session();
        if (begin)
        {
            begin_session();
        }
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    }

    // Polls until an event of `type` arrives; earlier events of other types are dropped
    b8 wait_event (XrStructureType type, XrEventDataBuffer& out_event)
    {
        for (u32 i = 0; i < 64; i++)
        {
            out_event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            if (xr.xrPollEvent(instance, &out_event) != XR_SUCCESS)
            {
                return false;
            }
            if (out_event.type == type)
            {
                return true;
            }
        }
        return false;
    }

    XrAsyncRequestIdFB discover (const XrSpaceFilterBaseHeaderMETA* filter)
    {
        const XrSpaceDiscoveryInfoMETA info       = { .type = XR_TYPE_SPACE_DISCOVERY_INFO_META, .filterCount = filter ? 1u : 0u, .filters = filter ? &filter : nullptr };
        XrAsyncRequestIdFB             request_id = 0;
        EXPECT_EQ(scene.xrDiscoverSpacesMETA(session, &info, &request_id), XR_SUCCESS);
        EXPECT_NE(request_id, 0u);
        return request_id;
    }

    // Runs a discovery to completion and retrieves its spaces
    u32 discover_all (const XrSpaceFilterBaseHeaderMETA* filter, XrSpaceDiscoveryResultMETA* out_results, u32 capacity)
    {
        const XrAsyncRequestIdFB request_id = discover(filter);
        XrEventDataBuffer        event      = {};
        u32                      count      = 0;
        while (xr.xrPollEvent(instance, &(event = { .type = XR_TYPE_EVENT_DATA_BUFFER })) == XR_SUCCESS)
        {
            if (event.type == XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_RESULTS_AVAILABLE_META)
            {
                EXPECT_EQ(((XrEventDataSpaceDiscoveryResultsAvailableMETA*)&event)->requestId, request_id);
                XrSpaceDiscoveryResultsMETA results = { .type = XR_TYPE_SPACE_DISCOVERY_RESULTS_META };
                EXPECT_EQ(scene.xrRetrieveSpaceDiscoveryResultsMETA(session, request_id, &results), XR_SUCCESS);
                EXPECT_LE(count + results.resultCountOutput, capacity);
                results.resultCapacityInput = capacity - count;
                results.results             = out_results + count;
                EXPECT_EQ(scene.xrRetrieveSpaceDiscoveryResultsMETA(session, request_id, &results), XR_SUCCESS);
                count += results.resultCountOutput;
            }
            if (event.type == XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_COMPLETE_META)
            {
                const XrEventDataSpaceDiscoveryCompleteMETA* complete = (XrEventDataSpaceDiscoveryCompleteMETA*)&event;
                EXPECT_EQ(complete->requestId, request_id);
                EXPECT_EQ(complete->result, XR_SUCCESS);
                return count;
            }
        }
        ADD_FAILURE() << "no completion event";
        return count;
    }

    XrSpace discover_room ()
    {
        const XrSpaceFilterComponentMETA filter = { .type = XR_TYPE_SPACE_FILTER_COMPONENT_META, .componentType = XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB };
        XrSpaceDiscoveryResultMETA       result = {};
        EXPECT_EQ(discover_all((const XrSpaceFilterBaseHeaderMETA*)&filter, &result, 1), 1u);
        EXPECT_TRUE(uuid_is(result.uuid, test_entity_uuid(TEST_ROOM_ENTITY_ROOM)));
        return result.space;
    }

    XrSpace discover_entity (u32 index)
    {
        XrUuidEXT uuid = {};
        W_Uuid    w    = {};
        EXPECT_TRUE(w_uuid_from_string(test_entity_uuid(index), w));
        memory_copy(uuid.data, w.bytes, XR_UUID_SIZE_EXT);
        const XrSpaceFilterUuidMETA filter = { .type = XR_TYPE_SPACE_FILTER_UUID_META, .uuidCount = 1, .uuids = &uuid };
        XrSpaceDiscoveryResultMETA  result = {};
        EXPECT_EQ(discover_all((const XrSpaceFilterBaseHeaderMETA*)&filter, &result, 1), 1u);
        return result.space;
    }

    const char* labels (XrSpace space, const XrSemanticLabelsSupportInfoFB* support = nullptr)
    {
        static char buffer[256];
        XrSemanticLabelsFB out = { .type = XR_TYPE_SEMANTIC_LABELS_FB, .next = support };
        EXPECT_EQ(scene.xrGetSpaceSemanticLabelsFB(session, space, &out), XR_SUCCESS);
        EXPECT_LT(out.bufferCountOutput, sizeof(buffer));
        out.bufferCapacityInput = sizeof(buffer);
        out.buffer              = buffer;
        buffer[0]               = 0;
        EXPECT_EQ(scene.xrGetSpaceSemanticLabelsFB(session, space, &out), XR_SUCCESS);
        EXPECT_EQ(buffer[out.bufferCountOutput], 0) << "a buffer with room for it gets the terminator";
        return buffer;
    }

    XrSpaceLocation locate (XrSpace space, XrSpace base)
    {
        XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
        EXPECT_EQ(xr.xrLocateSpace(space, base, frame().predictedDisplayTime, &location), XR_SUCCESS);
        return location;
    }
};

} // namespace

TEST_F(OxrSceneTest, scene_functions_need_their_extensions)
{
    create_headless_instance();
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrDiscoverSpacesMETA", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrCreateSpatialAnchorFB", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrGetSpaceTriangleMeshMETA", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrPollFutureEXT", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(fn, nullptr);
}

TEST_F(OxrSceneTest, system_reports_spatial_entity_support)
{
    start(false);
    XrSystemSpaceDiscoveryPropertiesMETA discovery = { .type = XR_TYPE_SYSTEM_SPACE_DISCOVERY_PROPERTIES_META };
    XrSystemSpatialEntityPropertiesFB    entity    = { .type = XR_TYPE_SYSTEM_SPATIAL_ENTITY_PROPERTIES_FB, .next = &discovery };
    XrSystemProperties                   props     = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &entity };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_TRUE(entity.supportsSpatialEntity);
    EXPECT_TRUE(discovery.supportsSpaceDiscovery);
}

TEST_F(OxrSceneTest, discovers_the_room_by_component)
{
    start();
    const XrSpaceFilterComponentMETA filter     = { .type = XR_TYPE_SPACE_FILTER_COMPONENT_META, .componentType = XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB };
    const XrAsyncRequestIdFB         request_id = discover((const XrSpaceFilterBaseHeaderMETA*)&filter);

    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_RESULTS_AVAILABLE_META, event));
    XrSpaceDiscoveryResultsMETA results = { .type = XR_TYPE_SPACE_DISCOVERY_RESULTS_META };
    ASSERT_EQ(scene.xrRetrieveSpaceDiscoveryResultsMETA(session, request_id, &results), XR_SUCCESS);
    ASSERT_EQ(results.resultCountOutput, 1u);
    XrSpaceDiscoveryResultMETA result = {};
    results.resultCapacityInput       = 1;
    results.results                   = &result;
    ASSERT_EQ(scene.xrRetrieveSpaceDiscoveryResultsMETA(session, request_id, &results), XR_SUCCESS);
    EXPECT_NE(result.space, XR_NULL_HANDLE);
    EXPECT_TRUE(uuid_is(result.uuid, test_entity_uuid(TEST_ROOM_ENTITY_ROOM)));

    XrUuidEXT uuid = {};
    EXPECT_EQ(scene.xrGetSpaceUuidFB(result.space, &uuid), XR_SUCCESS);
    EXPECT_TRUE(memory_equal(uuid.data, result.uuid.data, XR_UUID_SIZE_EXT));

    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_COMPLETE_META, event));
    EXPECT_EQ(((XrEventDataSpaceDiscoveryCompleteMETA*)&event)->requestId, request_id);

    // Retrieved results are gone
    results = { .type = XR_TYPE_SPACE_DISCOVERY_RESULTS_META };
    EXPECT_EQ(scene.xrRetrieveSpaceDiscoveryResultsMETA(session, request_id, &results), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrRetrieveSpaceDiscoveryResultsMETA(session, request_id + 100, &results), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xr.xrDestroySpace(result.space), XR_SUCCESS);
}

TEST_F(OxrSceneTest, discovery_without_filters_finds_every_entity)
{
    start();
    XrSpaceDiscoveryResultMETA results[16] = {};
    EXPECT_EQ(discover_all(nullptr, results, 16), (u32)TEST_ROOM_ENTITIES);
}

TEST_F(OxrSceneTest, discovery_results_need_enough_capacity)
{
    start();
    const XrAsyncRequestIdFB request_id = discover(nullptr);
    XrEventDataBuffer        event      = {};
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_RESULTS_AVAILABLE_META, event));
    XrSpaceDiscoveryResultMETA  results[4] = {};
    XrSpaceDiscoveryResultsMETA out        = { .type = XR_TYPE_SPACE_DISCOVERY_RESULTS_META, .resultCapacityInput = 4, .results = results };
    EXPECT_EQ(scene.xrRetrieveSpaceDiscoveryResultsMETA(session, request_id, &out), XR_ERROR_SIZE_INSUFFICIENT);
    EXPECT_EQ(out.resultCountOutput, (u32)TEST_ROOM_ENTITIES);
    out.results = nullptr;
    EXPECT_EQ(scene.xrRetrieveSpaceDiscoveryResultsMETA(session, request_id, &out), XR_ERROR_VALIDATION_FAILURE);
}

// MRUK discovers the room in the first scene's Awake, before Unity begins the session
TEST_F(OxrSceneTest, discovery_before_running_when_allowed)
{
    os_set_env("PUPPETXR_DISCOVER_BEFORE_RUNNING", "1");
    defer { os_unset_env("PUPPETXR_DISCOVER_BEFORE_RUNNING"); };
    start(false);
    XrSpaceDiscoveryResultMETA results[16] = {};
    EXPECT_EQ(discover_all(nullptr, results, 16), (u32)TEST_ROOM_ENTITIES);
}

TEST_F(OxrSceneTest, discovery_validates_inputs)
{
    start(false);
    XrAsyncRequestIdFB             request_id = 0;
    const XrSpaceDiscoveryInfoMETA info       = { .type = XR_TYPE_SPACE_DISCOVERY_INFO_META };
    EXPECT_EQ(scene.xrDiscoverSpacesMETA(session, &info, &request_id), XR_ERROR_SESSION_NOT_RUNNING);
    begin_session();
    EXPECT_EQ(scene.xrDiscoverSpacesMETA(XR_NULL_HANDLE, &info, &request_id), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(scene.xrDiscoverSpacesMETA(session, nullptr, &request_id), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrDiscoverSpacesMETA(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    const XrSpaceDiscoveryInfoMETA wrong = { .type = XR_TYPE_SPACE_QUERY_INFO_FB };
    EXPECT_EQ(scene.xrDiscoverSpacesMETA(session, &wrong, &request_id), XR_ERROR_VALIDATION_FAILURE);

    const XrSpaceFilterUuidMETA        empty_uuids = { .type = XR_TYPE_SPACE_FILTER_UUID_META, .uuidCount = 0 };
    const XrSpaceFilterBaseHeaderMETA* filters[]   = { (const XrSpaceFilterBaseHeaderMETA*)&empty_uuids };
    XrSpaceDiscoveryInfoMETA           with_filter = { .type = XR_TYPE_SPACE_DISCOVERY_INFO_META, .filterCount = 1, .filters = filters };
    EXPECT_EQ(scene.xrDiscoverSpacesMETA(session, &with_filter, &request_id), XR_ERROR_VALIDATION_FAILURE);
    with_filter.filters = nullptr;
    EXPECT_EQ(scene.xrDiscoverSpacesMETA(session, &with_filter, &request_id), XR_ERROR_VALIDATION_FAILURE);
    const XrSpaceFilterComponentMETA bad_component = { .type = XR_TYPE_SPACE_FILTER_COMPONENT_META, .componentType = (XrSpaceComponentTypeFB)12345 };
    filters[0]          = (const XrSpaceFilterBaseHeaderMETA*)&bad_component;
    with_filter.filters = filters;
    EXPECT_EQ(scene.xrDiscoverSpacesMETA(session, &with_filter, &request_id), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrSceneTest, room_layout_and_container)
{
    start();
    const XrSpace room = discover_room();

    XrRoomLayoutFB layout = { .type = XR_TYPE_ROOM_LAYOUT_FB };
    ASSERT_EQ(scene.xrGetSpaceRoomLayoutFB(session, room, &layout), XR_SUCCESS);
    ASSERT_EQ(layout.wallUuidCountOutput, 4u);
    XrUuidEXT walls[4] = {};
    layout.wallUuidCapacityInput = 2;
    layout.wallUuids             = walls;
    EXPECT_EQ(scene.xrGetSpaceRoomLayoutFB(session, room, &layout), XR_ERROR_SIZE_INSUFFICIENT);
    layout.wallUuidCapacityInput = 4;
    ASSERT_EQ(scene.xrGetSpaceRoomLayoutFB(session, room, &layout), XR_SUCCESS);
    EXPECT_TRUE(uuid_is(layout.floorUuid, test_entity_uuid(TEST_ROOM_ENTITY_FLOOR)));
    EXPECT_TRUE(uuid_is(layout.ceilingUuid, test_entity_uuid(TEST_ROOM_ENTITY_CEILING)));
    for (u32 i = 0; i < 4; i++)
    {
        EXPECT_TRUE(uuid_is(walls[i], test_entity_uuid(TEST_ROOM_ENTITY_WALL0 + i))) << i;
    }

    XrSpaceContainerFB container = { .type = XR_TYPE_SPACE_CONTAINER_FB };
    ASSERT_EQ(scene.xrGetSpaceContainerFB(session, room, &container), XR_SUCCESS);
    ASSERT_EQ(container.uuidCountOutput, 8u);
    XrUuidEXT children[8] = {};
    container.uuidCapacityInput = 8;
    container.uuids             = children;
    ASSERT_EQ(scene.xrGetSpaceContainerFB(session, room, &container), XR_SUCCESS);
    for (u32 i = 0; i < 8; i++)
    {
        EXPECT_TRUE(uuid_is(children[i], test_entity_uuid(1 + i))) << i;
    }

    // A floor has neither component
    const XrSpace floor = discover_entity(TEST_ROOM_ENTITY_FLOOR);
    EXPECT_EQ(scene.xrGetSpaceRoomLayoutFB(session, floor, &layout), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
    EXPECT_EQ(scene.xrGetSpaceContainerFB(session, floor, &container), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
    EXPECT_EQ(scene.xrGetSpaceRoomLayoutFB(session, stage, &layout), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
    EXPECT_EQ(scene.xrGetSpaceRoomLayoutFB(session, room, nullptr), XR_ERROR_VALIDATION_FAILURE);
    XrRoomLayoutFB wrong = { .type = XR_TYPE_SPACE_CONTAINER_FB };
    EXPECT_EQ(scene.xrGetSpaceRoomLayoutFB(session, room, &wrong), XR_ERROR_VALIDATION_FAILURE);
}

// MRUK and Meta's XrSceneModel sample size the buffer by the count and build their string from
// it; a count with the terminator made every label unknown to MRUK, which then drew the global
// mesh with its wall effect over the whole view
TEST_F(OxrSceneTest, semantic_labels_read_as_meta_reads_them)
{
    start();
    const XrSpace      table = discover_entity(TEST_ROOM_ENTITY_TABLE);
    XrSemanticLabelsFB out   = { .type = XR_TYPE_SEMANTIC_LABELS_FB };
    ASSERT_EQ(scene.xrGetSpaceSemanticLabelsFB(session, table, &out), XR_SUCCESS);
    ASSERT_EQ(out.bufferCountOutput, 5u);
    char exact[5] = {};
    out.bufferCapacityInput = sizeof(exact);
    out.buffer              = exact;
    ASSERT_EQ(scene.xrGetSpaceSemanticLabelsFB(session, table, &out), XR_SUCCESS);
    EXPECT_EQ(out.bufferCountOutput, 5u);
    EXPECT_EQ(memcmp(exact, "TABLE", 5), 0);
}

TEST_F(OxrSceneTest, semantic_labels_follow_the_support_info)
{
    start();
    EXPECT_STREQ(labels(discover_entity(TEST_ROOM_ENTITY_FLOOR)), "FLOOR");
    EXPECT_STREQ(labels(discover_entity(TEST_ROOM_ENTITY_CEILING)), "CEILING");
    EXPECT_STREQ(labels(discover_entity(TEST_ROOM_ENTITY_WALL0)), "WALL_FACE");
    EXPECT_STREQ(labels(discover_entity(TEST_ROOM_ENTITY_MESH)), "GLOBAL_MESH");

    const XrSpace invisible = discover_entity(TEST_ROOM_ENTITY_WALL0 + 3);
    const XrSpace table     = discover_entity(TEST_ROOM_ENTITY_TABLE);
    EXPECT_STREQ(labels(invisible), "WALL_FACE");
    EXPECT_STREQ(labels(table), "TABLE");

    XrSemanticLabelsSupportInfoFB support = { .type = XR_TYPE_SEMANTIC_LABELS_SUPPORT_INFO_FB, .flags = XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_INVISIBLE_WALL_FACE_BIT_FB | XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_DESK_TO_TABLE_MIGRATION_BIT_FB,
                                              .recognizedLabels = "TABLE,WALL_FACE,INVISIBLE_WALL_FACE,OTHER" };
    EXPECT_STREQ(labels(invisible, &support), "INVISIBLE_WALL_FACE");
    EXPECT_STREQ(labels(table, &support), "TABLE");
    EXPECT_STREQ(labels(discover_entity(TEST_ROOM_ENTITY_FLOOR), &support), "OTHER");

    support.flags            = 0;
    support.recognizedLabels = "DESK,WALL_FACE,OTHER";
    EXPECT_STREQ(labels(invisible, &support), "WALL_FACE");
    EXPECT_STREQ(labels(table, &support), "DESK");

    char               small[3];
    XrSemanticLabelsFB out = { .type = XR_TYPE_SEMANTIC_LABELS_FB, .bufferCapacityInput = sizeof(small), .buffer = small };
    EXPECT_EQ(scene.xrGetSpaceSemanticLabelsFB(session, table, &out), XR_ERROR_SIZE_INSUFFICIENT);
    EXPECT_EQ(out.bufferCountOutput, 5u);
    EXPECT_EQ(scene.xrGetSpaceSemanticLabelsFB(session, discover_room(), &out), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
    support.recognizedLabels = nullptr;
    out                      = { .type = XR_TYPE_SEMANTIC_LABELS_FB, .next = &support };
    EXPECT_EQ(scene.xrGetSpaceSemanticLabelsFB(session, table, &out), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrSceneTest, bounds_and_boundaries)
{
    start();
    const XrSpace floor = discover_entity(TEST_ROOM_ENTITY_FLOOR);
    XrRect2Df     rect  = {};
    ASSERT_EQ(scene.xrGetSpaceBoundingBox2DFB(session, floor, &rect), XR_SUCCESS);
    EXPECT_FLOAT_EQ(rect.offset.x, -2.0f);
    EXPECT_FLOAT_EQ(rect.offset.y, -1.5f);
    EXPECT_FLOAT_EQ(rect.extent.width, 4.0f);
    EXPECT_FLOAT_EQ(rect.extent.height, 3.0f);

    XrBoundary2DFB boundary = { .type = XR_TYPE_BOUNDARY_2D_FB };
    ASSERT_EQ(scene.xrGetSpaceBoundary2DFB(session, floor, &boundary), XR_SUCCESS);
    ASSERT_EQ(boundary.vertexCountOutput, 4u);
    XrVector2f vertices[4] = {};
    boundary.vertexCapacityInput = 4;
    boundary.vertices            = vertices;
    ASSERT_EQ(scene.xrGetSpaceBoundary2DFB(session, floor, &boundary), XR_SUCCESS);
    EXPECT_FLOAT_EQ(vertices[2].x, 2.0f);
    EXPECT_FLOAT_EQ(vertices[2].y, 1.5f);

    // Planes without a polygon report their rectangle, counter-clockwise
    const XrSpace wall = discover_entity(TEST_ROOM_ENTITY_WALL0);
    boundary           = { .type = XR_TYPE_BOUNDARY_2D_FB, .vertexCapacityInput = 4, .vertices = vertices };
    ASSERT_EQ(scene.xrGetSpaceBoundary2DFB(session, wall, &boundary), XR_SUCCESS);
    ASSERT_EQ(boundary.vertexCountOutput, 4u);
    EXPECT_FLOAT_EQ(vertices[0].x, -2.0f);
    EXPECT_FLOAT_EQ(vertices[0].y, -1.25f);
    EXPECT_FLOAT_EQ(vertices[1].x, 2.0f);
    EXPECT_FLOAT_EQ(vertices[1].y, -1.25f);

    const XrSpace table = discover_entity(TEST_ROOM_ENTITY_TABLE);
    XrRect3DfFB   box   = {};
    ASSERT_EQ(scene.xrGetSpaceBoundingBox3DFB(session, table, &box), XR_SUCCESS);
    EXPECT_FLOAT_EQ(box.offset.z, -0.75f);
    EXPECT_FLOAT_EQ(box.extent.width, 1.2f);
    EXPECT_FLOAT_EQ(box.extent.height, 0.8f);
    EXPECT_FLOAT_EQ(box.extent.depth, 0.75f);

    EXPECT_EQ(scene.xrGetSpaceBoundingBox3DFB(session, floor, &box), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
    EXPECT_EQ(scene.xrGetSpaceBoundingBox2DFB(session, floor, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrGetSpaceBoundingBox2DFB(session, XR_NULL_HANDLE, &rect), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrSceneTest, global_mesh_triangles)
{
    start();
    const XrSpace           mesh = discover_entity(TEST_ROOM_ENTITY_MESH);
    XrSpaceTriangleMeshMETA out  = { .type = XR_TYPE_SPACE_TRIANGLE_MESH_META };
    const XrSpaceTriangleMeshGetInfoMETA info = { .type = XR_TYPE_SPACE_TRIANGLE_MESH_GET_INFO_META };
    ASSERT_EQ(scene.xrGetSpaceTriangleMeshMETA(mesh, &info, &out), XR_SUCCESS);
    ASSERT_EQ(out.vertexCountOutput, 12u);
    ASSERT_EQ(out.indexCountOutput, 42u);
    XrVector3f vertices[12] = {};
    u32        indices[42]  = {};
    out.vertexCapacityInput = 12;
    out.vertices            = vertices;
    out.indexCapacityInput  = 41;
    out.indices             = indices;
    EXPECT_EQ(scene.xrGetSpaceTriangleMeshMETA(mesh, &info, &out), XR_ERROR_SIZE_INSUFFICIENT);
    out.indexCapacityInput = 42;
    ASSERT_EQ(scene.xrGetSpaceTriangleMeshMETA(mesh, &info, &out), XR_SUCCESS);
    EXPECT_FLOAT_EQ(vertices[6].x, 2.0f);
    EXPECT_FLOAT_EQ(vertices[6].y, 2.5f);
    EXPECT_EQ(indices[41], 10u);

    EXPECT_EQ(scene.xrGetSpaceTriangleMeshMETA(discover_entity(TEST_ROOM_ENTITY_TABLE), &info, &out), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
    EXPECT_EQ(scene.xrGetSpaceTriangleMeshMETA(mesh, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    const XrSpaceTriangleMeshGetInfoMETA wrong = { .type = XR_TYPE_SPACE_TRIANGLE_MESH_META };
    EXPECT_EQ(scene.xrGetSpaceTriangleMeshMETA(mesh, &wrong, &out), XR_ERROR_VALIDATION_FAILURE);
}

//
// Room mesh: XR_META_spatial_entity_room_mesh
//

static const char* const room_mesh_extensions[] = {
    XR_MND_HEADLESS_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,
    XR_FB_SCENE_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_DISCOVERY_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_SEMANTIC_LABEL_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_ROOM_MESH_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_CONTAINER_EXTENSION_NAME,
    XR_FB_SCENE_CAPTURE_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_MESH_EXTENSION_NAME,
    XR_EXT_FUTURE_EXTENSION_NAME,
};

static const XrSemanticLabelMETA room_mesh_all_labels[] = {
    XR_SEMANTIC_LABEL_FLOOR_META, XR_SEMANTIC_LABEL_CEILING_META, XR_SEMANTIC_LABEL_WALL_FACE_META, XR_SEMANTIC_LABEL_INNER_WALL_FACE_META,
    XR_SEMANTIC_LABEL_INVISIBLE_WALL_FACE_META, XR_SEMANTIC_LABEL_DOOR_FRAME_META, XR_SEMANTIC_LABEL_WINDOW_FRAME_META,
};

namespace
{

struct OxrRoomMeshTest : OxrSceneTest
{
    PFN_xrGetSpaceRoomMeshMETA            xrGetSpaceRoomMeshMETA            = nullptr;
    PFN_xrGetSpaceRoomMeshFaceIndicesMETA xrGetSpaceRoomMeshFaceIndicesMETA = nullptr;

    OxrRoomMeshTest ()
    {
        start_extensions       = room_mesh_extensions;
        start_extensions_count = ARRAY_SIZE(room_mesh_extensions);
    }

    void start_room_mesh ()
    {
        start();
        ASSERT_EQ(gipa(instance, "xrGetSpaceRoomMeshMETA", (PFN_xrVoidFunction*)&xrGetSpaceRoomMeshMETA), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetSpaceRoomMeshFaceIndicesMETA", (PFN_xrVoidFunction*)&xrGetSpaceRoomMeshFaceIndicesMETA), XR_SUCCESS);
    }

    XrResult get (XrSpace room, const XrSemanticLabelMETA* labels, u32 labels_count, XrVector3f* vertices, u32 vertex_capacity, XrRoomMeshFaceMETA* faces, u32 face_capacity, XrRoomMeshMETA& out)
    {
        const XrSpaceRoomMeshGetInfoMETA info = { .type = XR_TYPE_SPACE_ROOM_MESH_GET_INFO_META, .recognizedSemanticLabelCount = labels_count, .recognizedSemanticLabels = labels };
        out = { .type = XR_TYPE_ROOM_MESH_META, .vertexCapacityInput = vertex_capacity, .vertices = vertices, .faceCapacityInput = face_capacity, .faces = faces };
        return xrGetSpaceRoomMeshMETA(room, &info, &out);
    }
};

} // namespace

TEST_F(OxrRoomMeshTest, functions_need_the_extension)
{
    create_headless_instance();
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrGetSpaceRoomMeshMETA", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrGetSpaceRoomMeshFaceIndicesMETA", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
}

TEST_F(OxrRoomMeshTest, the_room_entity_carries_the_room_mesh)
{
    start_room_mesh();
    const XrSpace room = discover_room();
    u32 count = 0;
    ASSERT_EQ(scene.xrEnumerateSpaceSupportedComponentsFB(room, 0, &count, nullptr), XR_SUCCESS);
    XrSpaceComponentTypeFB types[16] = {};
    ASSERT_LE(count, 16u);
    ASSERT_EQ(scene.xrEnumerateSpaceSupportedComponentsFB(room, count, &count, types), XR_SUCCESS);
    b8 found = false;
    for (u32 i = 0; i < count; i++)
    {
        found = found || types[i] == XR_SPACE_COMPONENT_TYPE_ROOM_MESH_META;
    }
    EXPECT_TRUE(found);

    // Spec: the room mesh of an entity without the component enabled is an error
    XrRoomMeshMETA out = {};
    EXPECT_EQ(get(discover_entity(TEST_ROOM_ENTITY_FLOOR), room_mesh_all_labels, ARRAY_SIZE(room_mesh_all_labels), nullptr, 0, nullptr, 0, out), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
}

TEST_F(OxrRoomMeshTest, vertices_and_faces_in_the_room_space)
{
    start_room_mesh();
    const XrSpace  room = discover_room();
    XrRoomMeshMETA out  = {};
    ASSERT_EQ(get(room, room_mesh_all_labels, ARRAY_SIZE(room_mesh_all_labels), nullptr, 0, nullptr, 0, out), XR_SUCCESS);
    ASSERT_EQ(out.vertexCountOutput, (u32)TEST_ROOM_MESH_POSITIONS);
    ASSERT_EQ(out.faceCountOutput, (u32)TEST_ROOM_MESH_FACES);

    XrVector3f         vertices[TEST_ROOM_MESH_POSITIONS] = {};
    XrRoomMeshFaceMETA faces[TEST_ROOM_MESH_FACES]        = {};
    EXPECT_EQ(get(room, room_mesh_all_labels, ARRAY_SIZE(room_mesh_all_labels), vertices, TEST_ROOM_MESH_POSITIONS - 1, faces, TEST_ROOM_MESH_FACES, out), XR_ERROR_SIZE_INSUFFICIENT);
    EXPECT_EQ(get(room, room_mesh_all_labels, ARRAY_SIZE(room_mesh_all_labels), vertices, TEST_ROOM_MESH_POSITIONS, faces, TEST_ROOM_MESH_FACES - 1, out), XR_ERROR_SIZE_INSUFFICIENT);
    ASSERT_EQ(get(room, room_mesh_all_labels, ARRAY_SIZE(room_mesh_all_labels), vertices, TEST_ROOM_MESH_POSITIONS, faces, TEST_ROOM_MESH_FACES, out), XR_SUCCESS);

    // The room entity is turned -90 degrees about x, so room (x, y, z) is entity (x, -z, y)
    EXPECT_NEAR(vertices[10].x, 0.5f, 1e-5f);
    EXPECT_NEAR(vertices[10].y, TEST_ROOM_HALF_Z, 1e-5f);
    EXPECT_NEAR(vertices[10].z, 2.0f, 1e-5f);

    EXPECT_EQ(faces[0].semanticLabel, XR_SEMANTIC_LABEL_FLOOR_META);
    EXPECT_EQ(faces[1].semanticLabel, XR_SEMANTIC_LABEL_CEILING_META);
    EXPECT_EQ(faces[TEST_ROOM_MESH_WALL0].semanticLabel, XR_SEMANTIC_LABEL_WALL_FACE_META);
    EXPECT_EQ(faces[5].semanticLabel, XR_SEMANTIC_LABEL_INVISIBLE_WALL_FACE_META);
    EXPECT_EQ(faces[TEST_ROOM_MESH_WINDOW].semanticLabel, XR_SEMANTIC_LABEL_WINDOW_FRAME_META);
    EXPECT_TRUE(uuid_is(faces[TEST_ROOM_MESH_WINDOW].uuid, "b0000000000000000000000000000007"));
    EXPECT_TRUE(memory_equal(faces[TEST_ROOM_MESH_WINDOW].parentUuid.data, faces[TEST_ROOM_MESH_WALL0].uuid.data, XR_UUID_SIZE));
    const XrUuid zero = {};
    EXPECT_TRUE(memory_equal(faces[0].parentUuid.data, zero.data, XR_UUID_SIZE));
}

TEST_F(OxrRoomMeshTest, labels_outside_the_recognized_list_are_unknown)
{
    start_room_mesh();
    const XrSpace            room   = discover_room();
    const XrSemanticLabelMETA floor = XR_SEMANTIC_LABEL_FLOOR_META;
    XrVector3f               vertices[TEST_ROOM_MESH_POSITIONS] = {};
    XrRoomMeshFaceMETA       faces[TEST_ROOM_MESH_FACES]        = {};
    XrRoomMeshMETA           out    = {};
    ASSERT_EQ(get(room, &floor, 1, vertices, TEST_ROOM_MESH_POSITIONS, faces, TEST_ROOM_MESH_FACES, out), XR_SUCCESS);
    EXPECT_EQ(faces[0].semanticLabel, XR_SEMANTIC_LABEL_FLOOR_META);
    for (u32 i = 1; i < TEST_ROOM_MESH_FACES; i++)
    {
        EXPECT_EQ(faces[i].semanticLabel, XR_SEMANTIC_LABEL_UNKNOWN_META) << i;
    }
    ASSERT_EQ(get(room, nullptr, 0, vertices, TEST_ROOM_MESH_POSITIONS, faces, TEST_ROOM_MESH_FACES, out), XR_SUCCESS);
    EXPECT_EQ(faces[0].semanticLabel, XR_SEMANTIC_LABEL_UNKNOWN_META);
}

TEST_F(OxrRoomMeshTest, face_indices)
{
    start_room_mesh();
    const XrSpace      room = discover_room();
    XrVector3f         vertices[TEST_ROOM_MESH_POSITIONS] = {};
    XrRoomMeshFaceMETA faces[TEST_ROOM_MESH_FACES]        = {};
    XrRoomMeshMETA     out  = {};
    ASSERT_EQ(get(room, room_mesh_all_labels, ARRAY_SIZE(room_mesh_all_labels), vertices, TEST_ROOM_MESH_POSITIONS, faces, TEST_ROOM_MESH_FACES, out), XR_SUCCESS);

    XrRoomMeshFaceIndicesMETA indices = { .type = XR_TYPE_ROOM_MESH_FACE_INDICES_META };
    ASSERT_EQ(xrGetSpaceRoomMeshFaceIndicesMETA(room, &faces[TEST_ROOM_MESH_WINDOW].uuid, &indices), XR_SUCCESS);
    ASSERT_EQ(indices.indexCountOutput, 6u);
    u32 buffer[24]             = {};
    indices.indexCapacityInput = 5;
    indices.indices            = buffer;
    EXPECT_EQ(xrGetSpaceRoomMeshFaceIndicesMETA(room, &faces[TEST_ROOM_MESH_WINDOW].uuid, &indices), XR_ERROR_SIZE_INSUFFICIENT);
    indices.indexCapacityInput = 24;
    ASSERT_EQ(xrGetSpaceRoomMeshFaceIndicesMETA(room, &faces[TEST_ROOM_MESH_WINDOW].uuid, &indices), XR_SUCCESS);
    const u32 window[6] = { 8, 9, 10, 8, 10, 11 };
    EXPECT_TRUE(memory_equal(buffer, window, sizeof(window)));
    ASSERT_EQ(xrGetSpaceRoomMeshFaceIndicesMETA(room, &faces[TEST_ROOM_MESH_WALL0].uuid, &indices), XR_SUCCESS);
    EXPECT_EQ(indices.indexCountOutput, 24u);

    const XrUuid unknown = {};
    EXPECT_EQ(xrGetSpaceRoomMeshFaceIndicesMETA(room, &unknown, &indices), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xrGetSpaceRoomMeshFaceIndicesMETA(room, nullptr, &indices), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xrGetSpaceRoomMeshFaceIndicesMETA(room, &faces[0].uuid, nullptr), XR_ERROR_VALIDATION_FAILURE);
    indices.type = XR_TYPE_ROOM_MESH_META;
    EXPECT_EQ(xrGetSpaceRoomMeshFaceIndicesMETA(room, &faces[0].uuid, &indices), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xrGetSpaceRoomMeshFaceIndicesMETA(XR_NULL_HANDLE, &faces[0].uuid, &indices), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrRoomMeshTest, disabled_component_and_bad_input)
{
    start_room_mesh();
    const XrSpace room = discover_room();
    const XrSpaceComponentStatusSetInfoFB off = { .type = XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB, .componentType = XR_SPACE_COMPONENT_TYPE_ROOM_MESH_META, .enabled = XR_FALSE };
    XrAsyncRequestIdFB request = 0;
    ASSERT_EQ(scene.xrSetSpaceComponentStatusFB(room, &off, &request), XR_SUCCESS);
    XrRoomMeshMETA out = {};
    EXPECT_EQ(get(room, room_mesh_all_labels, ARRAY_SIZE(room_mesh_all_labels), nullptr, 0, nullptr, 0, out), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
    const XrUuid face = {};
    XrRoomMeshFaceIndicesMETA indices = { .type = XR_TYPE_ROOM_MESH_FACE_INDICES_META };
    EXPECT_EQ(xrGetSpaceRoomMeshFaceIndicesMETA(room, &face, &indices), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);

    const XrSpaceComponentStatusSetInfoFB on = { .type = XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB, .componentType = XR_SPACE_COMPONENT_TYPE_ROOM_MESH_META, .enabled = XR_TRUE };
    ASSERT_EQ(scene.xrSetSpaceComponentStatusFB(room, &on, &request), XR_SUCCESS);
    EXPECT_EQ(xrGetSpaceRoomMeshMETA(room, nullptr, &out), XR_ERROR_VALIDATION_FAILURE);
    const XrSpaceRoomMeshGetInfoMETA bad_labels = { .type = XR_TYPE_SPACE_ROOM_MESH_GET_INFO_META, .recognizedSemanticLabelCount = 1, .recognizedSemanticLabels = nullptr };
    EXPECT_EQ(xrGetSpaceRoomMeshMETA(room, &bad_labels, &out), XR_ERROR_VALIDATION_FAILURE);
    const XrSpaceRoomMeshGetInfoMETA info = { .type = XR_TYPE_SPACE_ROOM_MESH_GET_INFO_META };
    EXPECT_EQ(xrGetSpaceRoomMeshMETA(room, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    out = { .type = XR_TYPE_ROOM_MESH_META, .vertexCapacityInput = 4, .vertices = nullptr };
    EXPECT_EQ(xrGetSpaceRoomMeshMETA(room, &info, &out), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrSceneTest, component_status)
{
    start();
    const XrSpace table = discover_entity(TEST_ROOM_ENTITY_TABLE);

    u32 count = 0;
    ASSERT_EQ(scene.xrEnumerateSpaceSupportedComponentsFB(table, 0, &count, nullptr), XR_SUCCESS);
    ASSERT_EQ(count, 4u);
    XrSpaceComponentTypeFB types[8] = {};
    ASSERT_EQ(scene.xrEnumerateSpaceSupportedComponentsFB(table, 8, &count, types), XR_SUCCESS);
    EXPECT_EQ(types[0], XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB);
    EXPECT_EQ(types[1], XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB);
    EXPECT_EQ(types[2], XR_SPACE_COMPONENT_TYPE_BOUNDED_3D_FB);
    EXPECT_EQ(types[3], XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB);
    EXPECT_EQ(scene.xrEnumerateSpaceSupportedComponentsFB(table, 2, &count, types), XR_ERROR_SIZE_INSUFFICIENT);

    XrSpaceComponentStatusFB status = { .type = XR_TYPE_SPACE_COMPONENT_STATUS_FB };
    ASSERT_EQ(scene.xrGetSpaceComponentStatusFB(table, XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB, &status), XR_SUCCESS);
    EXPECT_TRUE(status.enabled);
    EXPECT_FALSE(status.changePending);
    EXPECT_EQ(scene.xrGetSpaceComponentStatusFB(table, XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB, &status), XR_ERROR_SPACE_COMPONENT_NOT_SUPPORTED_FB);

    XrAsyncRequestIdFB              request_id = 0;
    XrSpaceComponentStatusSetInfoFB set        = { .type = XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB, .componentType = XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB, .enabled = XR_FALSE };
    ASSERT_EQ(scene.xrSetSpaceComponentStatusFB(table, &set, &request_id), XR_SUCCESS);
    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB, event));
    const XrEventDataSpaceSetStatusCompleteFB* complete = (XrEventDataSpaceSetStatusCompleteFB*)&event;
    EXPECT_EQ(complete->requestId, request_id);
    EXPECT_EQ(complete->result, XR_SUCCESS);
    EXPECT_EQ(complete->space, table);
    EXPECT_EQ(complete->componentType, XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB);
    EXPECT_FALSE(complete->enabled);
    EXPECT_TRUE(uuid_is(complete->uuid, test_entity_uuid(TEST_ROOM_ENTITY_TABLE)));

    ASSERT_EQ(scene.xrGetSpaceComponentStatusFB(table, XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB, &status), XR_SUCCESS);
    EXPECT_FALSE(status.enabled);
    XrRect2Df rect = {};
    EXPECT_EQ(scene.xrGetSpaceBoundingBox2DFB(session, table, &rect), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
    EXPECT_EQ(scene.xrSetSpaceComponentStatusFB(table, &set, &request_id), XR_ERROR_SPACE_COMPONENT_STATUS_ALREADY_SET_FB);
    set.componentType = XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB;
    EXPECT_EQ(scene.xrSetSpaceComponentStatusFB(table, &set, &request_id), XR_ERROR_SPACE_COMPONENT_NOT_SUPPORTED_FB);
    set.componentType = (XrSpaceComponentTypeFB)99999;
    EXPECT_EQ(scene.xrSetSpaceComponentStatusFB(table, &set, &request_id), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrGetSpaceComponentStatusFB(table, (XrSpaceComponentTypeFB)99999, &status), XR_ERROR_VALIDATION_FAILURE);

    // Reference spaces are no spatial entities
    EXPECT_EQ(scene.xrEnumerateSpaceSupportedComponentsFB(stage, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(scene.xrGetSpaceComponentStatusFB(stage, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB, &status), XR_ERROR_SPACE_COMPONENT_NOT_SUPPORTED_FB);
}

// Regression: OVRPlugin creates anchors at time 0 (Meta's Shared Spatial Anchors sample)
TEST_F(OxrSceneTest, spatial_anchor_at_time_zero_when_accepted)
{
    os_set_env("PUPPETXR_ACCEPT_TIME_ZERO", "1");
    defer { os_unset_env("PUPPETXR_ACCEPT_TIME_ZERO"); };
    start();
    frame();
    const XrSpatialAnchorCreateInfoFB info       = { .type = XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_FB, .space = stage, .poseInSpace = { { 0, 0, 0, 1 }, { 1, 0.5f, -2 } }, .time = 0 };
    XrAsyncRequestIdFB                request_id = 0;
    ASSERT_EQ(scene.xrCreateSpatialAnchorFB(session, &info, &request_id), XR_SUCCESS);
    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPATIAL_ANCHOR_CREATE_COMPLETE_FB, event));
    const XrEventDataSpatialAnchorCreateCompleteFB complete = *(XrEventDataSpatialAnchorCreateCompleteFB*)&event;
    EXPECT_EQ(complete.result, XR_SUCCESS);
    const XrSpaceLocation location = locate(complete.space, stage);
    EXPECT_NEAR(location.pose.position.x, 1.0f, 1e-5f);
    EXPECT_NEAR(location.pose.position.z, -2.0f, 1e-5f);
}

TEST_F(OxrSceneTest, entities_locate_at_their_room_pose)
{
    start();
    const XrSpace   table    = discover_entity(TEST_ROOM_ENTITY_TABLE);
    XrSpaceLocation location = locate(table, stage);
    EXPECT_EQ(location.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT), (XrSpaceLocationFlags)(XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT));
    EXPECT_NEAR(location.pose.position.x, 0.5f, 1e-5f);
    EXPECT_NEAR(location.pose.position.y, 0.75f, 1e-5f);
    EXPECT_NEAR(location.pose.orientation.x, -0.70710678f, 1e-5f);

    XrAsyncRequestIdFB                    request_id = 0;
    const XrSpaceComponentStatusSetInfoFB set        = { .type = XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB, .componentType = XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB, .enabled = XR_FALSE };
    ASSERT_EQ(scene.xrSetSpaceComponentStatusFB(table, &set, &request_id), XR_SUCCESS);
    location = locate(table, stage);
    EXPECT_EQ(location.locationFlags, 0u);
}

TEST_F(OxrSceneTest, spatial_anchors)
{
    start();
    const XrTime                time = frame().predictedDisplayTime;
    XrSpatialAnchorCreateInfoFB info = { .type = XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_FB, .space = stage, .poseInSpace = { { 0, 0.38268343f, 0, 0.92387953f }, { 1, 0.5f, -2 } }, .time = time };
    XrAsyncRequestIdFB          request_id = 0;
    ASSERT_EQ(scene.xrCreateSpatialAnchorFB(session, &info, &request_id), XR_SUCCESS);

    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPATIAL_ANCHOR_CREATE_COMPLETE_FB, event));
    const XrEventDataSpatialAnchorCreateCompleteFB complete = *(XrEventDataSpatialAnchorCreateCompleteFB*)&event;
    EXPECT_EQ(complete.requestId, request_id);
    EXPECT_EQ(complete.result, XR_SUCCESS);
    ASSERT_NE(complete.space, XR_NULL_HANDLE);

    XrUuidEXT uuid = {};
    ASSERT_EQ(scene.xrGetSpaceUuidFB(complete.space, &uuid), XR_SUCCESS);
    EXPECT_TRUE(memory_equal(uuid.data, complete.uuid.data, XR_UUID_SIZE_EXT));
    const XrUuidEXT zero = {};
    EXPECT_FALSE(memory_equal(uuid.data, zero.data, XR_UUID_SIZE_EXT));

    // The anchor stays put while the head moves
    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 1, 1.7f, 1 }, zm::QUAT_IDENTITY });
    XrSpaceLocation location = locate(complete.space, stage);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
    EXPECT_NEAR(location.pose.position.x, 1.0f, 1e-5f);
    EXPECT_NEAR(location.pose.position.z, -2.0f, 1e-5f);
    EXPECT_NEAR(location.pose.orientation.y, 0.38268343f, 1e-5f);

    u32 count = 0;
    ASSERT_EQ(scene.xrEnumerateSpaceSupportedComponentsFB(complete.space, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 1u);

    // A second anchor gets a different uuid; both survive a room change
    XrAsyncRequestIdFB second_id = 0;
    ASSERT_EQ(scene.xrCreateSpatialAnchorFB(session, &info, &second_id), XR_SUCCESS);
    EXPECT_NE(second_id, request_id);
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPATIAL_ANCHOR_CREATE_COMPLETE_FB, event));
    EXPECT_FALSE(memory_equal(((XrEventDataSpatialAnchorCreateCompleteFB*)&event)->uuid.data, uuid.data, XR_UUID_SIZE_EXT));
    ASSERT_EQ(oxr_set_room(nullptr), OXR_RoomResult::Ok);
    location = locate(complete.space, stage);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);

    info.poseInSpace.orientation = { 0, 0, 0, 0 };
    EXPECT_EQ(scene.xrCreateSpatialAnchorFB(session, &info, &request_id), XR_ERROR_POSE_INVALID);
    info.poseInSpace.orientation = { 0, 0, 0, 1 };
    info.time                    = 0;
    EXPECT_EQ(scene.xrCreateSpatialAnchorFB(session, &info, &request_id), XR_ERROR_TIME_INVALID);
    info.time  = time;
    info.space = XR_NULL_HANDLE;
    EXPECT_EQ(scene.xrCreateSpatialAnchorFB(session, &info, &request_id), XR_ERROR_HANDLE_INVALID);
    info.space = stage;
    EXPECT_EQ(scene.xrCreateSpatialAnchorFB(session, nullptr, &request_id), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrCreateSpatialAnchorFB(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrGetSpaceUuidFB(stage, &uuid), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrGetSpaceUuidFB(complete.space, nullptr), XR_ERROR_VALIDATION_FAILURE);
}

// Regression: a component filter without a storage location was taken as a local query. Horizon OS
// (Quest 3) refuses it, so an app that only ever ran here found nothing on a headset
TEST_F(OxrSceneTest, a_component_filter_needs_a_storage_location_as_on_a_quest)
{
    start();
    const XrSpaceComponentFilterInfoFB bare  = { .type = XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB, .componentType = XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB };
    XrSpaceQueryInfoFB                 info  = { .type = XR_TYPE_SPACE_QUERY_INFO_FB, .queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB, .maxResultCount = 100, .timeout = XR_INFINITE_DURATION,
                                                 .filter = (const XrSpaceFilterInfoBaseHeaderFB*)&bare };
    XrAsyncRequestIdFB                 request_id = 0;
    EXPECT_EQ(scene.xrQuerySpacesFB(session, (const XrSpaceQueryInfoBaseHeaderFB*)&info, &request_id), XR_ERROR_VALIDATION_FAILURE);

    const XrSpaceStorageLocationFilterInfoFB local  = { .type = XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB, .location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB };
    const XrSpaceComponentFilterInfoFB       filter = { .type = XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB, .next = &local, .componentType = XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB };
    info.filter = (const XrSpaceFilterInfoBaseHeaderFB*)&filter;
    EXPECT_EQ(scene.xrQuerySpacesFB(session, (const XrSpaceQueryInfoBaseHeaderFB*)&info, &request_id), XR_SUCCESS);

    info.filter = nullptr;
    EXPECT_EQ(scene.xrQuerySpacesFB(session, (const XrSpaceQueryInfoBaseHeaderFB*)&info, &request_id), XR_SUCCESS) << "no filter at all stays a query of everything";
}

TEST_F(OxrSceneTest, space_queries)
{
    start();
    const XrSpaceStorageLocationFilterInfoFB local  = { .type = XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB, .location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB };
    const XrSpaceComponentFilterInfoFB       filter = { .type = XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB, .next = &local, .componentType = XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB };
    XrUuidEXT                          table  = {};
    W_Uuid                             w      = {};
    ASSERT_TRUE(w_uuid_from_string(test_entity_uuid(TEST_ROOM_ENTITY_TABLE), w));
    memory_copy(table.data, w.bytes, XR_UUID_SIZE_EXT);
    const XrSpaceUuidFilterInfoFB exclude = { .type = XR_TYPE_SPACE_UUID_FILTER_INFO_FB, .uuidCount = 1, .uuids = &table };
    const XrSpaceQueryInfoFB      info    = { .type = XR_TYPE_SPACE_QUERY_INFO_FB, .queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB, .maxResultCount = 100, .timeout = XR_INFINITE_DURATION,
                                              .filter = (const XrSpaceFilterInfoBaseHeaderFB*)&filter, .excludeFilter = (const XrSpaceFilterInfoBaseHeaderFB*)&exclude };
    XrAsyncRequestIdFB request_id = 0;
    ASSERT_EQ(scene.xrQuerySpacesFB(session, (const XrSpaceQueryInfoBaseHeaderFB*)&info, &request_id), XR_SUCCESS);

    XrSpaceQueryResultsFB results = { .type = XR_TYPE_SPACE_QUERY_RESULTS_FB };
    XrEventDataBuffer     event   = {};
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB, event));
    EXPECT_EQ(((XrEventDataSpaceQueryResultsAvailableFB*)&event)->requestId, request_id);
    ASSERT_EQ(scene.xrRetrieveSpaceQueryResultsFB(session, request_id, &results), XR_SUCCESS);
    EXPECT_EQ(results.resultCountOutput, 6u); // floor, ceiling, four walls
    XrSpaceQueryResultFB found[8] = {};
    results.resultCapacityInput   = 8;
    results.results               = found;
    ASSERT_EQ(scene.xrRetrieveSpaceQueryResultsFB(session, request_id, &results), XR_SUCCESS);
    EXPECT_TRUE(uuid_is(found[0].uuid, test_entity_uuid(TEST_ROOM_ENTITY_FLOOR)));
    EXPECT_NE(found[0].space, XR_NULL_HANDLE);
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB, event));
    EXPECT_EQ(((XrEventDataSpaceQueryCompleteFB*)&event)->result, XR_SUCCESS);
    EXPECT_EQ(scene.xrRetrieveSpaceQueryResultsFB(session, request_id, &results), XR_ERROR_VALIDATION_FAILURE);

    const XrSpaceQueryInfoFB limited = { .type = XR_TYPE_SPACE_QUERY_INFO_FB, .queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB, .maxResultCount = 2 };
    ASSERT_EQ(scene.xrQuerySpacesFB(session, (const XrSpaceQueryInfoBaseHeaderFB*)&limited, &request_id), XR_SUCCESS);
    results = { .type = XR_TYPE_SPACE_QUERY_RESULTS_FB };
    ASSERT_EQ(scene.xrRetrieveSpaceQueryResultsFB(session, request_id, &results), XR_SUCCESS);
    EXPECT_EQ(results.resultCountOutput, 2u);

    const XrSpaceQueryInfoFB bad_action = { .type = XR_TYPE_SPACE_QUERY_INFO_FB, .queryAction = (XrSpaceQueryActionFB)7 };
    EXPECT_EQ(scene.xrQuerySpacesFB(session, (const XrSpaceQueryInfoBaseHeaderFB*)&bad_action, &request_id), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrQuerySpacesFB(session, nullptr, &request_id), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrSceneTest, scene_capture_always_succeeds)
{
    start();
    const XrSceneCaptureRequestInfoFB info       = { .type = XR_TYPE_SCENE_CAPTURE_REQUEST_INFO_FB };
    XrAsyncRequestIdFB                request_id = 0;
    ASSERT_EQ(scene.xrRequestSceneCaptureFB(session, &info, &request_id), XR_SUCCESS);
    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SCENE_CAPTURE_COMPLETE_FB, event));
    EXPECT_EQ(((XrEventDataSceneCaptureCompleteFB*)&event)->requestId, request_id);
    EXPECT_EQ(((XrEventDataSceneCaptureCompleteFB*)&event)->result, XR_SUCCESS);

    const XrSceneCaptureRequestInfoFB with_request = { .type = XR_TYPE_SCENE_CAPTURE_REQUEST_INFO_FB, .requestByteCount = 4, .request = nullptr };
    EXPECT_EQ(scene.xrRequestSceneCaptureFB(session, &with_request, &request_id), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrRequestSceneCaptureFB(session, nullptr, &request_id), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrRequestSceneCaptureFB(XR_NULL_HANDLE, &info, &request_id), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrSceneTest, without_a_room_nothing_is_discovered)
{
    os_set_env("PUPPETXR_ROOM", "none");
    start();
    const XrAsyncRequestIdFB request_id = discover(nullptr);
    XrEventDataBuffer        event      = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    ASSERT_EQ(xr.xrPollEvent(instance, &event), XR_SUCCESS);
    ASSERT_EQ(event.type, XR_TYPE_EVENT_DATA_SPACE_DISCOVERY_COMPLETE_META);
    EXPECT_EQ(((XrEventDataSpaceDiscoveryCompleteMETA*)&event)->requestId, request_id);
    EXPECT_EQ(((XrEventDataSpaceDiscoveryCompleteMETA*)&event)->result, XR_SUCCESS);
    XrSpaceDiscoveryResultsMETA results = { .type = XR_TYPE_SPACE_DISCOVERY_RESULTS_META };
    EXPECT_EQ(scene.xrRetrieveSpaceDiscoveryResultsMETA(session, request_id, &results), XR_SUCCESS);
    EXPECT_EQ(results.resultCountOutput, 0u);
}

TEST_F(OxrSceneTest, entity_spaces_of_an_unloaded_room_go_stale)
{
    start();
    const XrSpace table = discover_entity(TEST_ROOM_ENTITY_TABLE);
    ASSERT_EQ(oxr_set_room("test_box"), OXR_RoomResult::Ok); // reloading counts as a new room
    const XrSpaceLocation location = locate(table, stage);
    EXPECT_EQ(location.locationFlags, 0u);
    u32 count = 99;
    EXPECT_EQ(scene.xrEnumerateSpaceSupportedComponentsFB(table, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 0u);
    XrRect2Df rect = {};
    EXPECT_EQ(scene.xrGetSpaceBoundingBox2DFB(session, table, &rect), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
    XrUuidEXT uuid = {};
    EXPECT_EQ(scene.xrGetSpaceUuidFB(table, &uuid), XR_SUCCESS);
    EXPECT_TRUE(uuid_is(uuid, test_entity_uuid(TEST_ROOM_ENTITY_TABLE)));
    EXPECT_EQ(xr.xrDestroySpace(table), XR_SUCCESS);
}

TEST_F(OxrSceneTest, spaces_of_another_session_are_rejected)
{
    start();
    const XrSpace table   = discover_entity(TEST_ROOM_ENTITY_TABLE);
    XrSession     other   = XR_NULL_HANDLE;
    XrSession     current = session;
    const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .systemId = system };
    if (xr.xrCreateSession(instance, &info, &other) != XR_SUCCESS)
    {
        GTEST_SKIP() << "one session at a time";
    }
    XrRect2Df rect = {};
    EXPECT_EQ(scene.xrGetSpaceBoundingBox2DFB(other, table, &rect), XR_ERROR_VALIDATION_FAILURE);
    xr.xrDestroySession(other);
    session = current;
}

TEST_F(OxrSceneTest, destroying_the_session_destroys_its_entity_spaces)
{
    start();
    const XrSpace table = discover_entity(TEST_ROOM_ENTITY_TABLE);
    ASSERT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
    session = XR_NULL_HANDLE;
    XrUuidEXT uuid = {};
    EXPECT_EQ(scene.xrGetSpaceUuidFB(table, &uuid), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrSceneTest, futures_reject_invalid_values)
{
    start();
    const XrFuturePollInfoEXT poll   = { .type = XR_TYPE_FUTURE_POLL_INFO_EXT, .future = (XrFutureEXT)0x1234 };
    XrFuturePollResultEXT     result = { .type = XR_TYPE_FUTURE_POLL_RESULT_EXT };
    EXPECT_EQ(scene.xrPollFutureEXT(instance, &poll, &result), XR_ERROR_FUTURE_INVALID_EXT);
    const XrFuturePollInfoEXT null_future = { .type = XR_TYPE_FUTURE_POLL_INFO_EXT, .future = XR_NULL_FUTURE_EXT };
    EXPECT_EQ(scene.xrPollFutureEXT(instance, &null_future, &result), XR_ERROR_FUTURE_INVALID_EXT);
    EXPECT_EQ(scene.xrPollFutureEXT(instance, nullptr, &result), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrPollFutureEXT(instance, &poll, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(scene.xrPollFutureEXT(XR_NULL_HANDLE, &poll, &result), XR_ERROR_HANDLE_INVALID);
    const XrFutureCancelInfoEXT cancel = { .type = XR_TYPE_FUTURE_CANCEL_INFO_EXT, .future = (XrFutureEXT)0x1234 };
    EXPECT_EQ(scene.xrCancelFutureEXT(instance, &cancel), XR_ERROR_FUTURE_INVALID_EXT);
    EXPECT_EQ(scene.xrCancelFutureEXT(instance, nullptr), XR_ERROR_VALIDATION_FAILURE);
}


//
// Anchor storage: XR_FB_spatial_entity_storage, _storage_batch, XR_META_spatial_entity_persistence
//

#define ANCHOR_FUNCTIONS(X) \
    X(xrSaveSpaceFB)        \
    X(xrEraseSpaceFB)       \
    X(xrSaveSpaceListFB)    \
    X(xrSaveSpacesMETA)     \
    X(xrEraseSpacesMETA)

namespace
{

struct AnchorXr
{
#define DECLARE(fn) PFN_##fn fn;
    ANCHOR_FUNCTIONS(DECLARE)
#undef DECLARE
};

} // namespace

static const char* const anchor_extensions[] = {
    XR_MND_HEADLESS_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_CONTAINER_EXTENSION_NAME,
    XR_FB_SCENE_EXTENSION_NAME,
    XR_FB_SCENE_CAPTURE_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_DISCOVERY_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_MESH_EXTENSION_NAME,
    XR_EXT_FUTURE_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_STORAGE_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_STORAGE_BATCH_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_PERSISTENCE_EXTENSION_NAME,
};

static b8 same_uuid (const XrUuidEXT& a, const XrUuidEXT& b)
{
    return memory_equal(a.data, b.data, XR_UUID_SIZE_EXT);
}

namespace
{

struct OxrAnchorTest : OxrSceneTest
{
    AnchorXr anchors   = {};
    char     store[OS_MAX_PATH] = {};

    OxrAnchorTest ()
    {
        start_extensions       = anchor_extensions;
        start_extensions_count = ARRAY_SIZE(anchor_extensions);
    }

    void SetUp () override
    {
        OxrSceneTest::SetUp();
        ASSERT_TRUE(os_create_temp_directory("puppetxr_anchors", store, sizeof(store)));
        os_set_env("PUPPETXR_ANCHORS_DIR", store);
    }

    void TearDown () override
    {
        OxrSceneTest::TearDown();
        oxr_set_anchor_timing(0, 0);
        for (u32 op = 0; op < (u32)OXR_AnchorOp::Count; op++)
        {
            oxr_set_anchor_fault((OXR_AnchorOp)op, XR_SUCCESS, 0);
        }
        os_unset_env("PUPPETXR_ANCHORS_DIR");
        os_delete_tree(store);
        os_unset_env("PUPPETXR_ORIGIN_OFFSET");
        os_unset_env("PUPPETXR_ORIGIN_SEED");
        os_unset_env("PUPPETXR_USER_ID");
        os_unset_env("PUPPETXR_APP_ID");
    }

    void start_anchors (b8 begin = true)
    {
        start(begin);
#define LOAD(fn) ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&anchors.fn), XR_SUCCESS) << #fn;
        ANCHOR_FUNCTIONS(LOAD)
#undef LOAD
    }

    // The app quits and starts again: everything but the stores is gone
    void relaunch ()
    {
        xr.xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        session  = XR_NULL_HANDLE;
        start_anchors();
    }

    // Before the session runs there are no frames, so the anchor is made at time 1
    XrSpace create_anchor (XrPosef pose, XrUuidEXT* out_uuid = nullptr, b8 running = true)
    {
        const XrSpatialAnchorCreateInfoFB info       = { .type = XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_FB, .space = stage, .poseInSpace = pose, .time = running ? frame().predictedDisplayTime : 1 };
        XrAsyncRequestIdFB                request_id = 0;
        EXPECT_EQ(scene.xrCreateSpatialAnchorFB(session, &info, &request_id), XR_SUCCESS);
        XrEventDataBuffer event = {};
        EXPECT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPATIAL_ANCHOR_CREATE_COMPLETE_FB, event));
        const XrEventDataSpatialAnchorCreateCompleteFB* complete = (XrEventDataSpatialAnchorCreateCompleteFB*)&event;
        EXPECT_EQ(complete->result, XR_SUCCESS);
        if (out_uuid)
        {
            *out_uuid = complete->uuid;
        }
        return complete->space;
    }

    XrResult set_component (XrSpace space, XrSpaceComponentTypeFB type, b8 enabled = true)
    {
        const XrSpaceComponentStatusSetInfoFB info       = { .type = XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB, .componentType = type, .enabled = (XrBool32)enabled };
        XrAsyncRequestIdFB                    request_id = 0;
        const XrResult                        result     = scene.xrSetSpaceComponentStatusFB(space, &info, &request_id);
        XrEventDataBuffer                     event      = {};
        if (result == XR_SUCCESS)
        {
            EXPECT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB, event));
        }
        return result;
    }

    XrSpace storable_anchor (XrPosef pose, XrUuidEXT* out_uuid = nullptr)
    {
        const XrSpace space = create_anchor(pose, out_uuid);
        EXPECT_EQ(set_component(space, XR_SPACE_COMPONENT_TYPE_STORABLE_FB), XR_SUCCESS);
        return space;
    }

    // Polls until the event of the request arrives
    b8 wait_request (XrStructureType type, XrAsyncRequestIdFB request_id, XrEventDataBuffer& out_event)
    {
        for (u32 i = 0; i < 64; i++)
        {
            if (!wait_event(type, out_event))
            {
                return false;
            }
            if (((XrEventDataSpaceSaveCompleteFB*)&out_event)->requestId == request_id)
            {
                return true;
            }
        }
        return false;
    }

    XrResult save (XrSpace space, XrSpaceStorageLocationFB location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB)
    {
        const XrSpaceSaveInfoFB info       = { .type = XR_TYPE_SPACE_SAVE_INFO_FB, .space = space, .location = location, .persistenceMode = XR_SPACE_PERSISTENCE_MODE_INDEFINITE_FB };
        XrAsyncRequestIdFB      request_id = 0;
        const XrResult          result     = anchors.xrSaveSpaceFB(session, &info, &request_id);
        if (result != XR_SUCCESS)
        {
            return result;
        }
        XrEventDataBuffer event = {};
        EXPECT_TRUE(wait_request(XR_TYPE_EVENT_DATA_SPACE_SAVE_COMPLETE_FB, request_id, event));
        const XrEventDataSpaceSaveCompleteFB* complete = (XrEventDataSpaceSaveCompleteFB*)&event;
        EXPECT_EQ(complete->space, space);
        EXPECT_EQ(complete->location, location);
        XrUuidEXT uuid = {};
        EXPECT_EQ(scene.xrGetSpaceUuidFB(space, &uuid), XR_SUCCESS);
        EXPECT_TRUE(same_uuid(complete->uuid, uuid));
        return complete->result;
    }

    XrResult erase (XrSpace space, XrSpaceStorageLocationFB location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB)
    {
        const XrSpaceEraseInfoFB info       = { .type = XR_TYPE_SPACE_ERASE_INFO_FB, .space = space, .location = location };
        XrAsyncRequestIdFB       request_id = 0;
        const XrResult           result     = anchors.xrEraseSpaceFB(session, &info, &request_id);
        if (result != XR_SUCCESS)
        {
            return result;
        }
        XrEventDataBuffer event = {};
        EXPECT_TRUE(wait_request(XR_TYPE_EVENT_DATA_SPACE_ERASE_COMPLETE_FB, request_id, event));
        return ((XrEventDataSpaceEraseCompleteFB*)&event)->result;
    }

    // Queries anchors by uuid; returns how many were found and the completion result
    u32 query (const XrUuidEXT& uuid, XrSpaceQueryResultFB* out_results, u32 capacity, XrResult* out_result = nullptr, XrSpaceStorageLocationFB location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB, const void* next = nullptr)
    {
        const XrSpaceStorageLocationFilterInfoFB where    = { .type = XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB, .location = location };
        XrUuidEXT                                uuids[1] = { uuid };
        const XrSpaceUuidFilterInfoFB            filter   = { .type = XR_TYPE_SPACE_UUID_FILTER_INFO_FB, .next = &where, .uuidCount = 1, .uuids = uuids };
        const XrSpaceQueryInfoFB                 info   = { .type = XR_TYPE_SPACE_QUERY_INFO_FB, .next = next, .queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB, .maxResultCount = 32,
                                                            .filter = (const XrSpaceFilterInfoBaseHeaderFB*)&filter };
        return run_query(info, out_results, capacity, out_result);
    }

    u32 run_query (const XrSpaceQueryInfoFB& info, XrSpaceQueryResultFB* out_results, u32 capacity, XrResult* out_result = nullptr)
    {
        XrAsyncRequestIdFB request_id = 0;
        EXPECT_EQ(scene.xrQuerySpacesFB(session, (const XrSpaceQueryInfoBaseHeaderFB*)&info, &request_id), XR_SUCCESS);
        u32 count = 0;
        for (u32 i = 0; i < 64; i++)
        {
            XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            if (xr.xrPollEvent(instance, &event) != XR_SUCCESS)
            {
                frame();
                continue;
            }
            if (event.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB && ((XrEventDataSpaceQueryResultsAvailableFB*)&event)->requestId == request_id)
            {
                XrSpaceQueryResultsFB results = { .type = XR_TYPE_SPACE_QUERY_RESULTS_FB, .resultCapacityInput = capacity, .results = out_results };
                EXPECT_EQ(scene.xrRetrieveSpaceQueryResultsFB(session, request_id, &results), XR_SUCCESS);
                count = results.resultCountOutput;
            }
            if (event.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB && ((XrEventDataSpaceQueryCompleteFB*)&event)->requestId == request_id)
            {
                if (out_result)
                {
                    *out_result = ((XrEventDataSpaceQueryCompleteFB*)&event)->result;
                }
                return count;
            }
        }
        ADD_FAILURE() << "the query never completed";
        return count;
    }

    XrPosef locate_pose (XrSpace space, XrSpaceLocationFlags expected_flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)
    {
        const XrSpaceLocation location = locate(space, stage);
        EXPECT_EQ(location.locationFlags, expected_flags);
        return location.pose;
    }
};

} // namespace

static const XrPosef anchor_pose = { { 0, 0.38268343f, 0, 0.92387953f }, { 1, 0.5f, -1 } };

static void expect_pose_near (const XrPosef& a, const XrPosef& b, f32 tolerance = 1e-5f)
{
    EXPECT_NEAR(a.position.x, b.position.x, tolerance);
    EXPECT_NEAR(a.position.y, b.position.y, tolerance);
    EXPECT_NEAR(a.position.z, b.position.z, tolerance);
    const f32 dot = a.orientation.x * b.orientation.x + a.orientation.y * b.orientation.y + a.orientation.z * b.orientation.z + a.orientation.w * b.orientation.w;
    EXPECT_NEAR(zm::abs(dot), 1.0f, tolerance);
}

TEST_F(OxrAnchorTest, storage_functions_need_their_extensions)
{
    start_extensions       = scene_extensions;
    start_extensions_count = ARRAY_SIZE(scene_extensions);
    start();
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrSaveSpaceFB", &fn), XR_ERROR_FUNCTION_UNSUPPORTED) << "scene extensions only";
    EXPECT_EQ(gipa(instance, "xrSaveSpaceListFB", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrSaveSpacesMETA", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
}

TEST_F(OxrAnchorTest, anchors_become_storable_with_a_storage_extension)
{
    start_anchors();
    XrSystemSpacePersistencePropertiesMETA persistence = { .type = XR_TYPE_SYSTEM_SPACE_PERSISTENCE_PROPERTIES_META };
    XrSystemProperties                     props       = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &persistence };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_TRUE(persistence.supportsSpacePersistence);

    const XrSpace          space = create_anchor(anchor_pose);
    XrSpaceComponentTypeFB types[8] = {};
    u32                    count    = 0;
    ASSERT_EQ(scene.xrEnumerateSpaceSupportedComponentsFB(space, 8, &count, types), XR_SUCCESS);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(types[0], XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB);
    EXPECT_EQ(types[1], XR_SPACE_COMPONENT_TYPE_STORABLE_FB);

    XrSpaceComponentStatusFB status = { .type = XR_TYPE_SPACE_COMPONENT_STATUS_FB };
    ASSERT_EQ(scene.xrGetSpaceComponentStatusFB(space, XR_SPACE_COMPONENT_TYPE_STORABLE_FB, &status), XR_SUCCESS);
    EXPECT_FALSE(status.enabled) << "apps enable storable themselves";
    EXPECT_EQ(save(space), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
    EXPECT_EQ(set_component(space, XR_SPACE_COMPONENT_TYPE_STORABLE_FB), XR_SUCCESS);
    EXPECT_EQ(save(space), XR_SUCCESS);

    // Scene entities are stored by the system, never by the app
    const XrSpace table = discover_entity(TEST_ROOM_ENTITY_TABLE);
    EXPECT_EQ(save(table), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
}

TEST_F(OxrAnchorTest, saved_anchors_load_after_the_app_restarts)
{
    start_anchors();
    XrUuidEXT     uuid  = {};
    const XrSpace space = storable_anchor(anchor_pose, &uuid);
    ASSERT_EQ(save(space), XR_SUCCESS);

    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_tests/user_1.json", store);
    EXPECT_TRUE(os_file_exists(path));

    relaunch();
    XrSpaceQueryResultFB found[4] = {};
    XrResult             result   = XR_ERROR_RUNTIME_FAILURE;
    ASSERT_EQ(query(uuid, found, 4, &result), 1u);
    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_TRUE(same_uuid(found[0].uuid, uuid));
    XrUuidEXT loaded_uuid = {};
    ASSERT_EQ(scene.xrGetSpaceUuidFB(found[0].space, &loaded_uuid), XR_SUCCESS);
    EXPECT_TRUE(same_uuid(loaded_uuid, uuid));
    expect_pose_near(locate_pose(found[0].space), anchor_pose);

    XrSpaceComponentStatusFB status = { .type = XR_TYPE_SPACE_COMPONENT_STATUS_FB };
    ASSERT_EQ(scene.xrGetSpaceComponentStatusFB(found[0].space, XR_SPACE_COMPONENT_TYPE_STORABLE_FB, &status), XR_SUCCESS);
    EXPECT_TRUE(status.enabled) << "loaded anchors are stored already";

    // A cloud query does not see local anchors
    EXPECT_EQ(query(uuid, found, 4, &result, XR_SPACE_STORAGE_LOCATION_CLOUD_FB), 0u);
}

TEST_F(OxrAnchorTest, discovery_finds_anchors_saved_through_meta_persistence)
{
    start_anchors();
    XrUuidEXT     uuid  = {};
    const XrSpace space = storable_anchor(anchor_pose, &uuid);
    // NOLINTNEXTLINE(misc-const-correctness): the info structs take a non-const XrSpace*
    XrSpace                    spaces[1]  = { space };
    const XrSpacesSaveInfoMETA info       = { .type = XR_TYPE_SPACES_SAVE_INFO_META, .spaceCount = 1, .spaces = spaces };
    XrAsyncRequestIdFB         request_id = 0;
    ASSERT_EQ(anchors.xrSaveSpacesMETA(session, &info, &request_id), XR_SUCCESS);
    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_request(XR_TYPE_EVENT_DATA_SPACES_SAVE_RESULT_META, request_id, event));
    EXPECT_EQ(((XrEventDataSpacesSaveResultMETA*)&event)->result, XR_SUCCESS);

    relaunch();
    const XrSpaceFilterUuidMETA filter = { .type = XR_TYPE_SPACE_FILTER_UUID_META, .uuidCount = 1, .uuids = &uuid };
    XrSpaceDiscoveryResultMETA  result = {};
    ASSERT_EQ(discover_all((const XrSpaceFilterBaseHeaderMETA*)&filter, &result, 1), 1u);
    EXPECT_TRUE(same_uuid(result.uuid, uuid));
    expect_pose_near(locate_pose(result.space), anchor_pose);

    // Unfiltered discovery returns the room's entities and the anchor
    XrSpaceDiscoveryResultMETA all[TEST_ROOM_ENTITIES + 2] = {};
    EXPECT_EQ(discover_all(nullptr, all, TEST_ROOM_ENTITIES + 2), (u32)TEST_ROOM_ENTITIES + 1);
}

TEST_F(OxrAnchorTest, erased_anchors_are_gone)
{
    start_anchors();
    XrUuidEXT     first_uuid  = {};
    XrUuidEXT     second_uuid = {};
    const XrSpace first       = storable_anchor(anchor_pose, &first_uuid);
    const XrSpace second      = storable_anchor(anchor_pose, &second_uuid);
    ASSERT_EQ(save(first), XR_SUCCESS);
    ASSERT_EQ(save(second), XR_SUCCESS);
    EXPECT_EQ(erase(first), XR_SUCCESS);
    EXPECT_EQ(erase(first), XR_SUCCESS) << "erasing twice is harmless";

    // XR_META_spatial_entity_persistence erases by uuid without a space
    const XrSpacesEraseInfoMETA info       = { .type = XR_TYPE_SPACES_ERASE_INFO_META, .uuidCount = 1, .uuids = &second_uuid };
    XrAsyncRequestIdFB          request_id = 0;
    ASSERT_EQ(anchors.xrEraseSpacesMETA(session, &info, &request_id), XR_SUCCESS);
    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_request(XR_TYPE_EVENT_DATA_SPACES_ERASE_RESULT_META, request_id, event));
    EXPECT_EQ(((XrEventDataSpacesEraseResultMETA*)&event)->result, XR_SUCCESS);

    relaunch();
    XrSpaceQueryResultFB found[2] = {};
    EXPECT_EQ(query(first_uuid, found, 2), 0u);
    EXPECT_EQ(query(second_uuid, found, 2), 0u);
}

TEST_F(OxrAnchorTest, batch_saves_every_anchor)
{
    start_anchors();
    XrUuidEXT uuids[2] = {};
    // NOLINTNEXTLINE(misc-const-correctness): the info structs take a non-const XrSpace*
    XrSpace                     spaces[2]  = { storable_anchor(anchor_pose, &uuids[0]), storable_anchor(anchor_pose, &uuids[1]) };
    const XrSpaceListSaveInfoFB info       = { .type = XR_TYPE_SPACE_LIST_SAVE_INFO_FB, .spaceCount = 2, .spaces = spaces, .location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB };
    XrAsyncRequestIdFB          request_id = 0;
    ASSERT_EQ(anchors.xrSaveSpaceListFB(session, &info, &request_id), XR_SUCCESS);
    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_request(XR_TYPE_EVENT_DATA_SPACE_LIST_SAVE_COMPLETE_FB, request_id, event));
    EXPECT_EQ(((XrEventDataSpaceListSaveCompleteFB*)&event)->result, XR_SUCCESS);

    relaunch();
    XrSpaceQueryResultFB found[2] = {};
    EXPECT_EQ(query(uuids[0], found, 2), 1u);
    EXPECT_EQ(query(uuids[1], found, 2), 1u);

    // NOLINTNEXTLINE(misc-const-correctness): the info structs take a non-const XrSpace*
    XrSpace                     not_storable[1] = { create_anchor(anchor_pose) };
    const XrSpaceListSaveInfoFB refused         = { .type = XR_TYPE_SPACE_LIST_SAVE_INFO_FB, .spaceCount = 1, .spaces = not_storable, .location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB };
    EXPECT_EQ(anchors.xrSaveSpaceListFB(session, &refused, &request_id), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);
}

TEST_F(OxrAnchorTest, storage_requests_validate_their_input)
{
    start_anchors(false);
    const XrSpace space = create_anchor(anchor_pose, nullptr, false);
    ASSERT_EQ(set_component(space, XR_SPACE_COMPONENT_TYPE_STORABLE_FB), XR_SUCCESS);
    XrAsyncRequestIdFB request_id = 0;
    XrSpaceSaveInfoFB  info       = { .type = XR_TYPE_SPACE_SAVE_INFO_FB, .space = space, .location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB, .persistenceMode = XR_SPACE_PERSISTENCE_MODE_INDEFINITE_FB };
    EXPECT_EQ(anchors.xrSaveSpaceFB(session, nullptr, &request_id), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(anchors.xrSaveSpaceFB(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(anchors.xrSaveSpaceFB(XR_NULL_HANDLE, &info, &request_id), XR_ERROR_HANDLE_INVALID);
    info.location = XR_SPACE_STORAGE_LOCATION_INVALID_FB;
    EXPECT_EQ(anchors.xrSaveSpaceFB(session, &info, &request_id), XR_ERROR_VALIDATION_FAILURE);
    info.location        = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
    info.persistenceMode = XR_SPACE_PERSISTENCE_MODE_INVALID_FB;
    EXPECT_EQ(anchors.xrSaveSpaceFB(session, &info, &request_id), XR_ERROR_VALIDATION_FAILURE);
    info.persistenceMode = XR_SPACE_PERSISTENCE_MODE_INDEFINITE_FB;
    info.space           = XR_NULL_HANDLE;
    EXPECT_EQ(anchors.xrSaveSpaceFB(session, &info, &request_id), XR_ERROR_HANDLE_INVALID);

    const XrSpaceEraseInfoFB erase_info = { .type = XR_TYPE_SPACE_ERASE_INFO_FB, .space = space, .location = (XrSpaceStorageLocationFB)9 };
    EXPECT_EQ(anchors.xrEraseSpaceFB(session, &erase_info, &request_id), XR_ERROR_VALIDATION_FAILURE);
    const XrSpaceListSaveInfoFB empty = { .type = XR_TYPE_SPACE_LIST_SAVE_INFO_FB, .spaceCount = 0, .location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB };
    EXPECT_EQ(anchors.xrSaveSpaceListFB(session, &empty, &request_id), XR_ERROR_VALIDATION_FAILURE);

    // XR_META_spatial_entity_persistence needs a running session
    // NOLINTNEXTLINE(misc-const-correctness): the info structs take a non-const XrSpace*
    XrSpace                    spaces[1] = { space };
    const XrSpacesSaveInfoMETA meta      = { .type = XR_TYPE_SPACES_SAVE_INFO_META, .spaceCount = 1, .spaces = spaces };
    EXPECT_EQ(anchors.xrSaveSpacesMETA(session, &meta, &request_id), XR_ERROR_SESSION_NOT_RUNNING);
    begin_session();
    EXPECT_EQ(anchors.xrSaveSpacesMETA(session, &meta, &request_id), XR_SUCCESS);
    const XrSpacesSaveInfoMETA none = { .type = XR_TYPE_SPACES_SAVE_INFO_META, .spaceCount = 0 };
    EXPECT_EQ(anchors.xrSaveSpacesMETA(session, &none, &request_id), XR_ERROR_VALIDATION_FAILURE);
    const XrSpacesEraseInfoMETA nothing = { .type = XR_TYPE_SPACES_ERASE_INFO_META };
    EXPECT_EQ(anchors.xrEraseSpacesMETA(session, &nothing, &request_id), XR_ERROR_VALIDATION_FAILURE);
    const XrSpacesEraseInfoMETA dangling = { .type = XR_TYPE_SPACES_ERASE_INFO_META, .uuidCount = 1, .uuids = nullptr };
    EXPECT_EQ(anchors.xrEraseSpacesMETA(session, &dangling, &request_id), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrAnchorTest, anchors_load_only_in_the_room_they_were_saved_in)
{
    start_anchors();
    XrUuidEXT uuid = {};
    ASSERT_EQ(save(storable_anchor(anchor_pose, &uuid)), XR_SUCCESS);
    XrSpaceQueryResultFB found[2] = {};

    ASSERT_EQ(oxr_set_room(nullptr), OXR_RoomResult::Ok);
    EXPECT_EQ(query(uuid, found, 2), 0u) << "another room";
    ASSERT_EQ(oxr_set_room("test_box"), OXR_RoomResult::Ok);
    EXPECT_EQ(query(uuid, found, 2), 1u);

    // An edited room is another room: its anchors would sit anywhere
    TestRoom room = {};
    test_room_build(room);
    ASSERT_TRUE(test_room_replace(room, "\"table\"", "\"storage\""));
    ASSERT_TRUE(test_room_write(room, rooms, "test_box"));
    ASSERT_EQ(oxr_set_room("test_box"), OXR_RoomResult::Ok);
    EXPECT_EQ(query(uuid, found, 2), 0u);
}

TEST_F(OxrAnchorTest, anchor_uuids_never_repeat_across_runs)
{
    start_anchors();
    XrUuidEXT saved = {};
    ASSERT_EQ(save(storable_anchor(anchor_pose, &saved)), XR_SUCCESS);
    relaunch();
    XrUuidEXT fresh = {};
    create_anchor(anchor_pose, &fresh);
    EXPECT_FALSE(same_uuid(fresh, saved));
}

TEST_F(OxrAnchorTest, stores_belong_to_one_app_and_user)
{
    start_anchors();
    XrUuidEXT uuid = {};
    ASSERT_EQ(save(storable_anchor(anchor_pose, &uuid)), XR_SUCCESS);
    XrSpaceQueryResultFB found[2] = {};

    os_set_env("PUPPETXR_USER_ID", "2");
    relaunch();
    EXPECT_EQ(query(uuid, found, 2), 0u) << "another headset";

    os_unset_env("PUPPETXR_USER_ID");
    os_set_env("PUPPETXR_APP_ID", "com.example.other");
    relaunch();
    EXPECT_EQ(query(uuid, found, 2), 0u) << "another app";

    os_unset_env("PUPPETXR_APP_ID");
    relaunch();
    EXPECT_EQ(query(uuid, found, 2), 1u);
}

TEST_F(OxrAnchorTest, wiping_empties_the_store)
{
    start_anchors();
    XrUuidEXT uuid = {};
    ASSERT_EQ(save(storable_anchor(anchor_pose, &uuid)), XR_SUCCESS);
    OXR_AnchorEntry entries[4] = {};
    ASSERT_EQ(oxr_list_anchors(entries, 4), 1u);
    EXPECT_FALSE(entries[0].cloud);
    EXPECT_STREQ(entries[0].anchor.room, "test_box");

    ASSERT_EQ(oxr_wipe_anchors(false), OXR_ControlResult::Ok);
    EXPECT_EQ(oxr_list_anchors(entries, 4), 0u);
    XrSpaceQueryResultFB found[2] = {};
    EXPECT_EQ(query(uuid, found, 2), 0u);
    relaunch();
    EXPECT_EQ(query(uuid, found, 2), 0u);
}

TEST_F(OxrAnchorTest, the_stage_can_lie_anywhere_in_the_room)
{
    os_set_env("PUPPETXR_ORIGIN_OFFSET", "90,1,0.5");
    start_anchors();
    Pose room_from_stage = {};
    ASSERT_TRUE(oxr_get_origin_offset(room_from_stage));
    EXPECT_NEAR(room_from_stage.position.x, 1.0f, 1e-6f);
    EXPECT_NEAR(room_from_stage.position.z, 0.5f, 1e-6f);

    // The table sits where the room has it, so in stage space it moved
    const XrSpace table    = discover_entity(TEST_ROOM_ENTITY_TABLE);
    const XrPosef in_stage = locate(table, stage).pose;
    const Vec3    in_room  = zm::transform_point(room_from_stage, { in_stage.position.x, in_stage.position.y, in_stage.position.z });
    EXPECT_NEAR(in_room.x, 0.5f, 1e-5f);
    EXPECT_NEAR(in_room.y, TEST_ROOM_TABLE_Y, 1e-5f);
    EXPECT_NEAR(in_room.z, 0.0f, 1e-5f);
    EXPECT_GT(zm::abs(in_stage.position.x - 0.5f) + zm::abs(in_stage.position.z), 0.1f);
}

TEST_F(OxrAnchorTest, anchors_stay_in_the_room_when_the_stage_moves)
{
    start_anchors();
    XrUuidEXT uuid = {};
    ASSERT_EQ(save(storable_anchor(anchor_pose, &uuid)), XR_SUCCESS);

    // The next run tracks from another place: a pose kept in stage space would be off
    os_set_env("PUPPETXR_ORIGIN_OFFSET", "-90,0.5,0.5");
    relaunch();
    XrSpaceQueryResultFB found[2] = {};
    ASSERT_EQ(query(uuid, found, 2), 1u);
    Pose room_from_stage = {};
    ASSERT_TRUE(oxr_get_origin_offset(room_from_stage));
    const XrPosef in_stage = locate_pose(found[0].space);
    const Pose    in_room  = zm::pose_mul(room_from_stage, Pose{ { in_stage.position.x, in_stage.position.y, in_stage.position.z }, { in_stage.orientation.x, in_stage.orientation.y, in_stage.orientation.z, in_stage.orientation.w } });
    expect_pose_near({ { in_room.orientation.x, in_room.orientation.y, in_room.orientation.z, in_room.orientation.w }, { in_room.position.x, in_room.position.y, in_room.position.z } }, anchor_pose);
    EXPECT_GT(zm::abs(in_stage.position.x - anchor_pose.position.x) + zm::abs(in_stage.position.z - anchor_pose.position.z), 0.1f);
}

static Pose pose_from_xr (const XrPosef& pose)
{
    return { { pose.position.x, pose.position.y, pose.position.z }, { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w } };
}

static XrPosef pose_to_xr (const Pose& pose)
{
    return { { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w }, { pose.position.x, pose.position.y, pose.position.z } };
}

static const Pose drift_step = { { 0.3f, 0.02f, -0.2f }, { 0.0f, 0.25881904f, 0.0f, 0.96592583f } }; // 30 degrees about +Y

static u32 reference_space_changes (XrInstance instance, const Xr& xr)
{
    u32 changes = 0;
    while (true)
    {
        XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        if (xr.xrPollEvent(instance, &event) != XR_SUCCESS)
        {
            return changes;
        }
        changes += event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING ? 1u : 0u;
    }
}

// Real drift: the tracking origin slips, the room and the anchors fixed to it stay. An anchor
// made this session and one loaded from the store both move in STAGE by exactly the drift
TEST_F(OxrAnchorTest, tracking_drift_keeps_anchors_in_the_room)
{
    os_set_env("PUPPETXR_ORIGIN_OFFSET", "20,0.4,-0.3");
    start_anchors();
    XrUuidEXT stored = {};
    ASSERT_EQ(save(storable_anchor(anchor_pose, &stored)), XR_SUCCESS);
    relaunch();
    XrSpaceQueryResultFB found[2] = {};
    ASSERT_EQ(query(stored, found, 2), 1u);
    const XrSpace loaded = found[0].space;
    const XrPosef other  = { { 0, 0, 0, 1 }, { -0.5f, 1.0f, -1.5f } };
    XrUuidEXT     fresh_uuid = {};
    const XrSpace fresh  = storable_anchor(other, &fresh_uuid);
    Pose room_from_stage = {};
    ASSERT_TRUE(oxr_get_origin_offset(room_from_stage));
    const Pose loaded_before = pose_from_xr(locate_pose(loaded));
    const Pose fresh_before  = pose_from_xr(locate_pose(fresh));
    reference_space_changes(instance, xr);

    ASSERT_TRUE(sim_set_tracking_drift(oxr_get_sim(), drift_step));
    const Pose loaded_after = pose_from_xr(locate_pose(loaded));
    const Pose fresh_after  = pose_from_xr(locate_pose(fresh));
    expect_pose_near(pose_to_xr(loaded_after), pose_to_xr(zm::pose_relative(drift_step, loaded_before)));
    expect_pose_near(pose_to_xr(fresh_after), pose_to_xr(zm::pose_relative(drift_step, fresh_before)));
    // Through the drifted stage origin both are where they were in the room
    const Pose tracked_origin = zm::pose_mul(room_from_stage, drift_step);
    expect_pose_near(pose_to_xr(zm::pose_mul(tracked_origin, loaded_after)), pose_to_xr(zm::pose_mul(room_from_stage, pose_from_xr(anchor_pose))));
    expect_pose_near(pose_to_xr(zm::pose_mul(tracked_origin, fresh_after)), pose_to_xr(zm::pose_mul(room_from_stage, pose_from_xr(other))));
    EXPECT_GT(zm::length(fresh_after.position - fresh_before.position), 0.1f);
    EXPECT_EQ(reference_space_changes(instance, xr), 0u) << "drift comes without a reference space change event";

    // Saved under drift, an anchor keeps its room pose
    ASSERT_EQ(save(fresh), XR_SUCCESS);
    OXR_AnchorEntry entries[4] = {};
    ASSERT_EQ(oxr_list_anchors(entries, 4), 2u);
    const OXR_AnchorEntry& saved = memory_equal(entries[0].anchor.uuid.bytes, fresh_uuid.data, XR_UUID_SIZE_EXT) ? entries[0] : entries[1];
    expect_pose_near(pose_to_xr(saved.anchor.pose), pose_to_xr(zm::pose_mul(room_from_stage, pose_from_xr(other))));

    // A new anchor made in the drifted STAGE lies at the drifted place in the room
    const XrSpace placed = create_anchor(other);
    expect_pose_near(locate_pose(placed), other);
}

// The head stays put in the room while its STAGE and LOCAL poses change; what is fixed to the
// room (the table) keeps its place relative to the head
TEST_F(OxrAnchorTest, tracking_drift_moves_the_head_and_the_room_in_stage_space)
{
    start_anchors();
    const XrSpace view  = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    const XrSpace local = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    const XrSpace table = discover_entity(TEST_ROOM_ENTITY_TABLE);
    const Pose    head  = sim_get_device_pose(oxr_get_sim(), SIM_Device::Head);
    const Pose head_in_stage  = pose_from_xr(locate(view, stage).pose);
    const Pose head_in_local  = pose_from_xr(locate(view, local).pose);
    const Pose table_in_stage = pose_from_xr(locate(table, stage).pose);
    const Pose table_in_view  = pose_from_xr(locate(table, view).pose);
    const Pose local_in_stage = pose_from_xr(locate(local, stage).pose);

    ASSERT_TRUE(sim_set_tracking_drift(oxr_get_sim(), drift_step));
    expect_pose_near(locate(view, stage).pose, pose_to_xr(zm::pose_relative(drift_step, head_in_stage)));
    expect_pose_near(locate(table, stage).pose, pose_to_xr(zm::pose_relative(drift_step, table_in_stage)));
    expect_pose_near(locate(view, local).pose, pose_to_xr(zm::pose_relative(zm::pose_mul(drift_step, local_in_stage), head_in_stage)));
    expect_pose_near(locate(table, view).pose, pose_to_xr(table_in_view));
    expect_pose_near(locate(local, stage).pose, pose_to_xr(local_in_stage)); // LOCAL drifts with STAGE
    EXPECT_TRUE(zm::equal(sim_get_device_pose(oxr_get_sim(), SIM_Device::Head), head));
    EXPECT_GT(zm::length(pose_from_xr(locate(view, local).pose).position - head_in_local.position), 0.1f);

    ASSERT_TRUE(sim_set_tracking_drift(oxr_get_sim(), zm::POSE_IDENTITY));
    expect_pose_near(locate(view, stage).pose, pose_to_xr(head_in_stage));
    expect_pose_near(locate(table, stage).pose, pose_to_xr(table_in_stage));
}

TEST_F(OxrAnchorTest, per_run_offsets_change_with_every_run)
{
    os_set_env("PUPPETXR_ORIGIN_OFFSET", "per_run");
    os_set_env("PUPPETXR_ORIGIN_SEED", "5");
    start_anchors();
    Pose first = {};
    ASSERT_TRUE(oxr_get_origin_offset(first));
    relaunch();
    Pose second = {};
    ASSERT_TRUE(oxr_get_origin_offset(second));
    EXPECT_FALSE(memory_equal(&first, &second, sizeof(Pose)));
    const Pose expected = w_origin_offset_for_run(5, 2);
    EXPECT_TRUE(memory_equal(&second, &expected, sizeof(Pose))) << "the second run of seed 5";
}

// A replay runs against the store as the recording started and leaves the disk alone
TEST_F(OxrAnchorTest, replays_use_the_recorded_store_and_origin)
{
    char recording[OS_MAX_PATH];
    cstr_format(recording, sizeof(recording), "%s/anchors.xrrec", store);
    XrUuidEXT kept = {};
    start_anchors();
    ASSERT_EQ(save(storable_anchor(anchor_pose, &kept)), XR_SUCCESS);
    xr.xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;

    os_set_env("PUPPETXR_ORIGIN_OFFSET", "per_run");
    os_set_env("PUPPETXR_RECORD", recording);
    start_anchors();
    os_unset_env("PUPPETXR_RECORD");
    Pose recorded_origin = {};
    ASSERT_TRUE(oxr_get_origin_offset(recorded_origin));
    XrUuidEXT recorded_uuid = {};
    ASSERT_EQ(save(storable_anchor(anchor_pose, &recorded_uuid)), XR_SUCCESS);
    XrSpaceQueryResultFB found[2] = {};
    ASSERT_EQ(query(kept, found, 2), 1u);
    const XrPosef recorded_pose = locate_pose(found[0].space);
    xr.xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;

    // Someone deletes the store, and the next run would place the stage elsewhere
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr_tests/user_1.json", store);
    ASSERT_TRUE(os_delete_file(path));
    os_set_env("PUPPETXR_REPLAY", recording);
    os_set_env("PUPPETXR_REPLAY_EXIT", "0");
    start_anchors();
    Pose replayed_origin = {};
    ASSERT_TRUE(oxr_get_origin_offset(replayed_origin));
    EXPECT_TRUE(zm::equal(replayed_origin, recorded_origin));
    XrUuidEXT replayed_uuid = {};
    ASSERT_EQ(save(storable_anchor(anchor_pose, &replayed_uuid)), XR_SUCCESS);
    EXPECT_TRUE(same_uuid(replayed_uuid, recorded_uuid));
    ASSERT_EQ(query(kept, found, 2), 1u) << "the recorded store still holds it";
    expect_pose_near(locate_pose(found[0].space), recorded_pose);
    EXPECT_FALSE(os_file_exists(path)) << "nothing is written during a replay";
}

TEST_F(OxrAnchorTest, loaded_anchors_localize_after_a_while)
{
    start_anchors();
    XrUuidEXT uuid = {};
    ASSERT_EQ(save(storable_anchor(anchor_pose, &uuid)), XR_SUCCESS);
    relaunch();
    ASSERT_EQ(oxr_set_anchor_timing(0, 3), OXR_ControlResult::Ok);
    XrSpaceQueryResultFB found[2] = {};
    ASSERT_EQ(query(uuid, found, 2), 1u);
    locate_pose(found[0].space, 0);
    locate_pose(found[0].space, 0);
    expect_pose_near(locate_pose(found[0].space), anchor_pose);
}

TEST_F(OxrAnchorTest, latency_delays_completions_by_frames)
{
    start_anchors();
    const XrSpace space = storable_anchor(anchor_pose);
    ASSERT_EQ(oxr_set_anchor_timing(2, 0), OXR_ControlResult::Ok);
    const XrSpaceSaveInfoFB info       = { .type = XR_TYPE_SPACE_SAVE_INFO_FB, .space = space, .location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB, .persistenceMode = XR_SPACE_PERSISTENCE_MODE_INDEFINITE_FB };
    XrAsyncRequestIdFB      request_id = 0;
    ASSERT_EQ(anchors.xrSaveSpaceFB(session, &info, &request_id), XR_SUCCESS);
    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    EXPECT_EQ(xr.xrPollEvent(instance, &event), XR_EVENT_UNAVAILABLE);
    frame();
    EXPECT_EQ(xr.xrPollEvent(instance, &(event = { .type = XR_TYPE_EVENT_DATA_BUFFER })), XR_EVENT_UNAVAILABLE);
    frame();
    ASSERT_EQ(xr.xrPollEvent(instance, &(event = { .type = XR_TYPE_EVENT_DATA_BUFFER })), XR_SUCCESS);
    EXPECT_EQ(event.type, XR_TYPE_EVENT_DATA_SPACE_SAVE_COMPLETE_FB);
    EXPECT_EQ(((XrEventDataSpaceSaveCompleteFB*)&event)->requestId, request_id);
}

TEST_F(OxrAnchorTest, injected_faults_fail_the_next_requests)
{
    start_anchors();
    XrUuidEXT     uuid  = {};
    const XrSpace space = storable_anchor(anchor_pose, &uuid);
    ASSERT_EQ(oxr_set_anchor_fault(OXR_AnchorOp::Save, XR_ERROR_SPACE_MAPPING_INSUFFICIENT_FB, 1), OXR_ControlResult::Ok);
    EXPECT_EQ(save(space), XR_ERROR_SPACE_MAPPING_INSUFFICIENT_FB);
    XrSpaceQueryResultFB found[2] = {};
    EXPECT_EQ(query(uuid, found, 2), 0u) << "a failed save stores nothing";
    EXPECT_EQ(save(space), XR_SUCCESS) << "the fault was used up";

    ASSERT_EQ(oxr_set_anchor_fault(OXR_AnchorOp::Load, XR_ERROR_SPACE_LOCALIZATION_FAILED_FB, 1), OXR_ControlResult::Ok);
    XrResult result = XR_SUCCESS;
    EXPECT_EQ(query(uuid, found, 2, &result), 0u);
    EXPECT_EQ(result, XR_ERROR_SPACE_LOCALIZATION_FAILED_FB);
    EXPECT_EQ(query(uuid, found, 2, &result), 1u);
    EXPECT_EQ(result, XR_SUCCESS);

    EXPECT_EQ(oxr_set_anchor_fault(OXR_AnchorOp::Erase, XR_SUCCESS + 1, 1), OXR_ControlResult::Invalid) << "faults are errors";
    EXPECT_EQ(oxr_set_anchor_fault(OXR_AnchorOp::Count, XR_ERROR_RUNTIME_FAILURE, 1), OXR_ControlResult::Invalid);
}


//
// Anchor sharing: XR_FB_spatial_entity_user, _sharing, XR_META_spatial_entity_sharing, _group_sharing
//

#define SHARING_FUNCTIONS(X)  \
    X(xrCreateSpaceUserFB)    \
    X(xrGetSpaceUserIdFB)     \
    X(xrDestroySpaceUserFB)   \
    X(xrShareSpacesFB)        \
    X(xrShareSpacesMETA)

namespace
{

struct SharingXr
{
#define DECLARE(fn) PFN_##fn fn;
    SHARING_FUNCTIONS(DECLARE)
#undef DECLARE
};

} // namespace

static const char* const sharing_extensions[] = {
    XR_MND_HEADLESS_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_CONTAINER_EXTENSION_NAME,
    XR_FB_SCENE_EXTENSION_NAME,
    XR_FB_SCENE_CAPTURE_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_DISCOVERY_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_MESH_EXTENSION_NAME,
    XR_EXT_FUTURE_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_STORAGE_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_STORAGE_BATCH_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_PERSISTENCE_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_USER_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_SHARING_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_SHARING_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_GROUP_SHARING_EXTENSION_NAME,
};

static XrUuidEXT group_uuid (u8 tag)
{
    XrUuidEXT uuid = {};
    uuid.data[0]   = 0x42;
    uuid.data[15]  = tag;
    return uuid;
}

namespace
{

struct OxrSharingTest : OxrAnchorTest
{
    SharingXr sharing = {};

    OxrSharingTest ()
    {
        start_extensions       = sharing_extensions;
        start_extensions_count = ARRAY_SIZE(sharing_extensions);
    }

    void start_sharing ()
    {
        start_anchors();
#define LOAD(fn) ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&sharing.fn), XR_SUCCESS) << #fn;
        SHARING_FUNCTIONS(LOAD)
#undef LOAD
    }

    // Another person starts the app on their own headset
    void relaunch_as (const char* user)
    {
        os_set_env("PUPPETXR_USER_ID", user);
        xr.xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        session  = XR_NULL_HANDLE;
        start_sharing();
    }

    XrSpace sharable_anchor (XrUuidEXT* out_uuid)
    {
        const XrSpace space = storable_anchor(anchor_pose, out_uuid);
        EXPECT_EQ(set_component(space, XR_SPACE_COMPONENT_TYPE_SHARABLE_FB), XR_SUCCESS);
        return space;
    }

    XrResult share_with_user (XrSpace space, XrSpaceUserIdFB id)
    {
        XrSpaceUserFB                 user        = XR_NULL_HANDLE;
        const XrSpaceUserCreateInfoFB create_info = { .type = XR_TYPE_SPACE_USER_CREATE_INFO_FB, .userId = id };
        EXPECT_EQ(sharing.xrCreateSpaceUserFB(session, &create_info, &user), XR_SUCCESS);
        defer { sharing.xrDestroySpaceUserFB(user); };
        // NOLINTNEXTLINE(misc-const-correctness): the info struct takes non-const handles
        XrSpace                  spaces[1]  = { space };
        // NOLINTNEXTLINE(misc-const-correctness): the info struct takes non-const handles
        XrSpaceUserFB            users[1]   = { user };
        const XrSpaceShareInfoFB info       = { .type = XR_TYPE_SPACE_SHARE_INFO_FB, .spaceCount = 1, .spaces = spaces, .userCount = 1, .users = users };
        XrAsyncRequestIdFB       request_id = 0;
        const XrResult           result     = sharing.xrShareSpacesFB(session, &info, &request_id);
        if (result != XR_SUCCESS)
        {
            return result;
        }
        XrEventDataBuffer event = {};
        EXPECT_TRUE(wait_request(XR_TYPE_EVENT_DATA_SPACE_SHARE_COMPLETE_FB, request_id, event));
        return ((XrEventDataSpaceShareCompleteFB*)&event)->result;
    }

    XrResult share_with_group (XrSpace space, const XrUuidEXT& group)
    {
        // NOLINTNEXTLINE(misc-const-correctness): the info struct takes non-const handles
        XrSpace                                spaces[1]  = { space };
        XrUuidEXT                              groups[1]  = { group };
        const XrShareSpacesRecipientGroupsMETA recipients = { .type = XR_TYPE_SHARE_SPACES_RECIPIENT_GROUPS_META, .groupCount = 1, .groups = groups };
        const XrShareSpacesInfoMETA            info       = { .type = XR_TYPE_SHARE_SPACES_INFO_META, .spaceCount = 1, .spaces = spaces, .recipientInfo = (const XrShareSpacesRecipientBaseHeaderMETA*)&recipients };
        XrAsyncRequestIdFB                     request_id = 0;
        const XrResult                         result     = sharing.xrShareSpacesMETA(session, &info, &request_id);
        if (result != XR_SUCCESS)
        {
            return result;
        }
        XrEventDataBuffer event = {};
        EXPECT_TRUE(wait_request(XR_TYPE_EVENT_DATA_SHARE_SPACES_COMPLETE_META, request_id, event));
        return ((XrEventDataShareSpacesCompleteMETA*)&event)->result;
    }

    u32 query_group (const XrUuidEXT& group, XrSpaceQueryResultFB* out_results, u32 capacity, XrResult* out_result)
    {
        const XrSpaceGroupUuidFilterInfoMETA in_group = { .type = XR_TYPE_SPACE_GROUP_UUID_FILTER_INFO_META, .groupUuid = group };
        const XrSpaceQueryInfoFB             info     = { .type = XR_TYPE_SPACE_QUERY_INFO_FB, .next = &in_group, .queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB, .maxResultCount = 32 };
        return run_query(info, out_results, capacity, out_result);
    }
};

} // namespace

TEST_F(OxrSharingTest, system_reports_sharing_support)
{
    start_sharing();
    XrSystemSpatialEntityGroupSharingPropertiesMETA groups  = { .type = XR_TYPE_SYSTEM_SPATIAL_ENTITY_GROUP_SHARING_PROPERTIES_META };
    XrSystemSpatialEntitySharingPropertiesMETA      sharing_props = { .type = XR_TYPE_SYSTEM_SPATIAL_ENTITY_SHARING_PROPERTIES_META, .next = &groups };
    XrSystemProperties                              props   = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &sharing_props };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_TRUE(sharing_props.supportsSpatialEntitySharing);
    EXPECT_TRUE(groups.supportsSpatialEntityGroupSharing);

    const XrSpace          space    = create_anchor(anchor_pose);
    XrSpaceComponentTypeFB types[8] = {};
    u32                    count    = 0;
    ASSERT_EQ(scene.xrEnumerateSpaceSupportedComponentsFB(space, 8, &count, types), XR_SUCCESS);
    ASSERT_EQ(count, 3u);
    EXPECT_EQ(types[2], XR_SPACE_COMPONENT_TYPE_SHARABLE_FB);
}

TEST_F(OxrSharingTest, space_users_hold_their_id)
{
    start_sharing();
    XrSpaceUserFB                 user = XR_NULL_HANDLE;
    const XrSpaceUserCreateInfoFB info = { .type = XR_TYPE_SPACE_USER_CREATE_INFO_FB, .userId = 123456789012ull };
    ASSERT_EQ(sharing.xrCreateSpaceUserFB(session, &info, &user), XR_SUCCESS);
    XrSpaceUserIdFB id = 0;
    ASSERT_EQ(sharing.xrGetSpaceUserIdFB(user, &id), XR_SUCCESS);
    EXPECT_EQ(id, 123456789012ull);
    EXPECT_EQ(sharing.xrGetSpaceUserIdFB(user, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(sharing.xrCreateSpaceUserFB(session, nullptr, &user), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(sharing.xrCreateSpaceUserFB(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(sharing.xrCreateSpaceUserFB(XR_NULL_HANDLE, &info, &user), XR_ERROR_HANDLE_INVALID);
    ASSERT_EQ(sharing.xrDestroySpaceUserFB(user), XR_SUCCESS);
    EXPECT_EQ(sharing.xrGetSpaceUserIdFB(user, &id), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(sharing.xrDestroySpaceUserFB(user), XR_ERROR_HANDLE_INVALID);

    XrSpaceUserFB orphan = XR_NULL_HANDLE;
    ASSERT_EQ(sharing.xrCreateSpaceUserFB(session, &info, &orphan), XR_SUCCESS);
    ASSERT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
    session = XR_NULL_HANDLE;
    EXPECT_EQ(sharing.xrGetSpaceUserIdFB(orphan, &id), XR_ERROR_HANDLE_INVALID) << "users go with their session";
}

TEST_F(OxrSharingTest, anchors_shared_with_a_user_load_for_that_user_only)
{
    start_sharing();
    XrUuidEXT     uuid  = {};
    const XrSpace space = sharable_anchor(&uuid);
    EXPECT_EQ(share_with_user(space, 2), XR_SUCCESS);

    relaunch_as("2");
    XrSpaceQueryResultFB found[2] = {};
    XrResult             result   = XR_ERROR_RUNTIME_FAILURE;
    ASSERT_EQ(query(uuid, found, 2, &result, XR_SPACE_STORAGE_LOCATION_CLOUD_FB), 1u);
    EXPECT_EQ(result, XR_SUCCESS);
    expect_pose_near(locate_pose(found[0].space), anchor_pose);
    EXPECT_EQ(query(uuid, found, 2, &result, XR_SPACE_STORAGE_LOCATION_LOCAL_FB), 0u) << "not on this headset";

    relaunch_as("3");
    EXPECT_EQ(query(uuid, found, 2, &result, XR_SPACE_STORAGE_LOCATION_CLOUD_FB), 0u);

    OXR_AnchorEntry entries[4] = {};
    relaunch_as("1");
    ASSERT_EQ(oxr_list_anchors(entries, 4), 1u);
    EXPECT_TRUE(entries[0].cloud);
    EXPECT_EQ(entries[0].owner, 1u);
}

TEST_F(OxrSharingTest, sharing_needs_the_sharable_component_and_valid_input)
{
    start_sharing();
    XrUuidEXT     uuid  = {};
    const XrSpace space = storable_anchor(anchor_pose, &uuid);
    EXPECT_EQ(share_with_user(space, 2), XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB);

    XrAsyncRequestIdFB       request_id = 0;
    const XrSpaceShareInfoFB empty      = { .type = XR_TYPE_SPACE_SHARE_INFO_FB };
    EXPECT_EQ(sharing.xrShareSpacesFB(session, &empty, &request_id), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(sharing.xrShareSpacesFB(session, nullptr, &request_id), XR_ERROR_VALIDATION_FAILURE);
    // NOLINTNEXTLINE(misc-const-correctness): the info struct takes non-const handles
    XrSpace                  spaces[1] = { space };
    // NOLINTNEXTLINE(misc-const-correctness): the info struct takes non-const handles
    XrSpaceUserFB            users[1]  = { (XrSpaceUserFB)(uintptr_t)0xBAD };
    const XrSpaceShareInfoFB bad_user  = { .type = XR_TYPE_SPACE_SHARE_INFO_FB, .spaceCount = 1, .spaces = spaces, .userCount = 1, .users = users };
    EXPECT_EQ(sharing.xrShareSpacesFB(session, &bad_user, &request_id), XR_ERROR_HANDLE_INVALID);

    const XrShareSpacesInfoMETA no_recipient = { .type = XR_TYPE_SHARE_SPACES_INFO_META, .spaceCount = 1, .spaces = spaces };
    EXPECT_EQ(sharing.xrShareSpacesMETA(session, &no_recipient, &request_id), XR_ERROR_VALIDATION_FAILURE);
    const XrShareSpacesRecipientGroupsMETA no_groups = { .type = XR_TYPE_SHARE_SPACES_RECIPIENT_GROUPS_META };
    const XrShareSpacesInfoMETA            empty_groups = { .type = XR_TYPE_SHARE_SPACES_INFO_META, .spaceCount = 1, .spaces = spaces, .recipientInfo = (const XrShareSpacesRecipientBaseHeaderMETA*)&no_groups };
    EXPECT_EQ(sharing.xrShareSpacesMETA(session, &empty_groups, &request_id), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrSharingTest, groups_load_every_anchor_shared_with_them)
{
    start_sharing();
    XrUuidEXT first  = {};
    XrUuidEXT second = {};
    EXPECT_EQ(share_with_group(sharable_anchor(&first), group_uuid(1)), XR_SUCCESS);
    EXPECT_EQ(share_with_group(sharable_anchor(&second), group_uuid(1)), XR_SUCCESS);

    relaunch_as("7");
    XrSpaceQueryResultFB found[4] = {};
    XrResult             result   = XR_ERROR_RUNTIME_FAILURE;
    EXPECT_EQ(query_group(group_uuid(1), found, 4, &result), 2u);
    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_EQ(query_group(group_uuid(2), found, 4, &result), 0u);
    EXPECT_EQ(result, XR_ERROR_SPACE_GROUP_NOT_FOUND_META);
}

// Regression: OVRPlugin passes the group filter as the query's filter, as the extension's text
// says ("in the query filters"); the registry also lets it extend the query info
TEST_F(OxrSharingTest, group_filter_may_be_the_query_filter)
{
    start_sharing();
    EXPECT_EQ(share_with_group(sharable_anchor(nullptr), group_uuid(1)), XR_SUCCESS);

    relaunch_as("7");
    const XrSpaceGroupUuidFilterInfoMETA in_group  = { .type = XR_TYPE_SPACE_GROUP_UUID_FILTER_INFO_META, .groupUuid = group_uuid(1) };
    const XrSpaceQueryInfoFB             info      = { .type = XR_TYPE_SPACE_QUERY_INFO_FB, .queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB, .maxResultCount = 32, .filter = (const XrSpaceFilterInfoBaseHeaderFB*)&in_group };
    XrSpaceQueryResultFB                 found[4]  = {};
    XrResult                             result    = XR_ERROR_RUNTIME_FAILURE;
    EXPECT_EQ(run_query(info, found, 4, &result), 1u);
    EXPECT_EQ(result, XR_SUCCESS);
}

TEST_F(OxrSharingTest, cloud_saves_and_erases_belong_to_their_owner)
{
    start_sharing();
    XrUuidEXT     uuid  = {};
    const XrSpace space = storable_anchor(anchor_pose, &uuid);
    ASSERT_EQ(save(space, XR_SPACE_STORAGE_LOCATION_CLOUD_FB), XR_SUCCESS);
    XrSpaceQueryResultFB found[2] = {};
    EXPECT_EQ(query(uuid, found, 2, nullptr, XR_SPACE_STORAGE_LOCATION_CLOUD_FB), 1u);
    EXPECT_EQ(query(uuid, found, 2, nullptr, XR_SPACE_STORAGE_LOCATION_LOCAL_FB), 0u);
    ASSERT_EQ(erase(space, XR_SPACE_STORAGE_LOCATION_CLOUD_FB), XR_SUCCESS);
    EXPECT_EQ(query(uuid, found, 2, nullptr, XR_SPACE_STORAGE_LOCATION_CLOUD_FB), 0u);
}

TEST_F(OxrSharingTest, wiping_with_the_cloud_removes_owned_cloud_anchors)
{
    start_sharing();
    XrUuidEXT uuid = {};
    ASSERT_EQ(share_with_group(sharable_anchor(&uuid), group_uuid(3)), XR_SUCCESS);
    ASSERT_EQ(oxr_wipe_anchors(false), OXR_ControlResult::Ok);
    XrSpaceQueryResultFB found[2] = {};
    EXPECT_EQ(query_group(group_uuid(3), found, 2, nullptr), 1u) << "a local wipe keeps the cloud";
    ASSERT_EQ(oxr_wipe_anchors(true), OXR_ControlResult::Ok);
    EXPECT_EQ(query_group(group_uuid(3), found, 2, nullptr), 0u);
}

TEST_F(OxrSharingTest, share_faults_fail_the_share)
{
    start_sharing();
    XrUuidEXT     uuid  = {};
    const XrSpace space = sharable_anchor(&uuid);
    ASSERT_EQ(oxr_set_anchor_fault(OXR_AnchorOp::Share, XR_ERROR_SPACE_NETWORK_TIMEOUT_FB, 2), OXR_ControlResult::Ok);
    EXPECT_EQ(share_with_user(space, 2), XR_ERROR_SPACE_NETWORK_TIMEOUT_FB);
    EXPECT_EQ(share_with_group(space, group_uuid(4)), XR_ERROR_SPACE_NETWORK_TIMEOUT_FB);
    XrSpaceQueryResultFB found[2] = {};
    EXPECT_EQ(query_group(group_uuid(4), found, 2, nullptr), 0u);
    EXPECT_EQ(share_with_group(space, group_uuid(4)), XR_SUCCESS);
}

// What another user shared is read once while recording; the replay gets it from the recording
TEST_F(OxrSharingTest, replays_see_the_cloud_as_it_was_recorded)
{
    start_sharing();
    XrUuidEXT uuid = {};
    ASSERT_EQ(share_with_group(sharable_anchor(&uuid), group_uuid(5)), XR_SUCCESS);

    char recording[OS_MAX_PATH];
    cstr_format(recording, sizeof(recording), "%s/cloud.xrrec", store);
    os_set_env("PUPPETXR_RECORD", recording);
    relaunch_as("2");
    os_unset_env("PUPPETXR_RECORD");
    XrSpaceQueryResultFB found[2] = {};
    ASSERT_EQ(query_group(group_uuid(5), found, 2, nullptr), 1u);
    frame();
    xr.xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;

    // The owner takes the anchor back, but the replay still sees it shared
    os_set_env("PUPPETXR_USER_ID", "1");
    start_sharing();
    ASSERT_EQ(oxr_wipe_anchors(true), OXR_ControlResult::Ok);
    os_set_env("PUPPETXR_REPLAY", recording);
    os_set_env("PUPPETXR_REPLAY_EXIT", "0");
    relaunch_as("2");
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    ASSERT_EQ(query_group(group_uuid(5), found, 2, &result), 1u);
    EXPECT_EQ(result, XR_SUCCESS);
    EXPECT_TRUE(same_uuid(found[0].uuid, uuid));
}


//
// XR_META_colocation_discovery
//

#define COLOCATION_FUNCTIONS(X)              \
    X(xrStartColocationAdvertisementMETA)    \
    X(xrStopColocationAdvertisementMETA)     \
    X(xrStartColocationDiscoveryMETA)        \
    X(xrStopColocationDiscoveryMETA)

namespace
{

struct ColocationXr
{
#define DECLARE(fn) PFN_##fn fn;
    COLOCATION_FUNCTIONS(DECLARE)
#undef DECLARE
};

} // namespace

static const char* const colocation_extensions[] = {
    XR_MND_HEADLESS_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_CONTAINER_EXTENSION_NAME,
    XR_FB_SCENE_EXTENSION_NAME,
    XR_FB_SCENE_CAPTURE_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_DISCOVERY_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_MESH_EXTENSION_NAME,
    XR_EXT_FUTURE_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_STORAGE_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_STORAGE_BATCH_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_PERSISTENCE_EXTENSION_NAME,
    XR_META_COLOCATION_DISCOVERY_EXTENSION_NAME,
};

namespace
{

struct OxrColocationTest : OxrAnchorTest
{
    ColocationXr colocation = {};
    char         adverts[OS_MAX_PATH] = {};

    OxrColocationTest ()
    {
        start_extensions       = colocation_extensions;
        start_extensions_count = ARRAY_SIZE(colocation_extensions);
    }

    void start_colocation ()
    {
        start_anchors();
#define LOAD(fn) ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&colocation.fn), XR_SUCCESS) << #fn;
        COLOCATION_FUNCTIONS(LOAD)
#undef LOAD
        cstr_format(adverts, sizeof(adverts), "%s/cloud/puppetxr_tests/adverts", store);
    }

    // Another headset nearby advertises, as a process of its own would
    OS_LockFile* advertise_elsewhere (u8 tag, const char* message)
    {
        W_Advert advert = { .owner = 9, .size = (u32)cstr_length(message) };
        advert.uuid.bytes[0]  = 0xad;
        advert.uuid.bytes[15] = tag;
        memory_copy(advert.buffer, message, advert.size);
        OS_LockFile* file = w_advert_start(adverts, advert);
        EXPECT_NE(file, nullptr);
        return file;
    }

    XrAsyncRequestIdFB start_discovery (XrResult expected = XR_SUCCESS)
    {
        const XrColocationDiscoveryStartInfoMETA info       = { .type = XR_TYPE_COLOCATION_DISCOVERY_START_INFO_META };
        XrAsyncRequestIdFB                       request_id = 0;
        EXPECT_EQ(colocation.xrStartColocationDiscoveryMETA(session, &info, &request_id), XR_SUCCESS);
        XrEventDataBuffer event = {};
        EXPECT_TRUE(wait_event(XR_TYPE_EVENT_DATA_START_COLOCATION_DISCOVERY_COMPLETE_META, event));
        const XrEventDataStartColocationDiscoveryCompleteMETA* complete = (XrEventDataStartColocationDiscoveryCompleteMETA*)&event;
        EXPECT_EQ(complete->discoveryRequestId, request_id);
        EXPECT_EQ(complete->result, expected);
        return request_id;
    }

    // Runs frames until a discovery result arrives
    b8 discovered (XrEventDataColocationDiscoveryResultMETA& out_result)
    {
        for (u32 i = 0; i < 40; i++)
        {
            XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            while (xr.xrPollEvent(instance, &event) == XR_SUCCESS)
            {
                if (event.type == XR_TYPE_EVENT_DATA_COLOCATION_DISCOVERY_RESULT_META)
                {
                    out_result = *(XrEventDataColocationDiscoveryResultMETA*)&event;
                    return true;
                }
                event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            }
            frame();
        }
        return false;
    }

    XrUuid start_advertising (const char* message, XrResult expected = XR_SUCCESS)
    {
        u8 buffer[64] = {};
        memory_copy(buffer, message, cstr_length(message));
        const XrColocationAdvertisementStartInfoMETA info       = { .type = XR_TYPE_COLOCATION_ADVERTISEMENT_START_INFO_META, .bufferSize = (u32)cstr_length(message), .buffer = buffer };
        XrAsyncRequestIdFB                           request_id = 0;
        EXPECT_EQ(colocation.xrStartColocationAdvertisementMETA(session, &info, &request_id), XR_SUCCESS);
        XrEventDataBuffer event = {};
        EXPECT_TRUE(wait_event(XR_TYPE_EVENT_DATA_START_COLOCATION_ADVERTISEMENT_COMPLETE_META, event));
        const XrEventDataStartColocationAdvertisementCompleteMETA* complete = (XrEventDataStartColocationAdvertisementCompleteMETA*)&event;
        EXPECT_EQ(complete->advertisementRequestId, request_id);
        EXPECT_EQ(complete->result, expected);
        return complete->advertisementUuid;
    }
};

} // namespace

TEST_F(OxrColocationTest, system_reports_colocation_support)
{
    start_colocation();
    XrSystemColocationDiscoveryPropertiesMETA colocation_props = { .type = XR_TYPE_SYSTEM_COLOCATION_DISCOVERY_PROPERTIES_META };
    XrSystemProperties                        props            = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &colocation_props };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_TRUE(colocation_props.supportsColocationDiscovery);
}

TEST_F(OxrColocationTest, discovery_finds_nearby_advertisements_until_stopped)
{
    start_colocation();
    OS_LockFile* nearby = advertise_elsewhere(1, "group-1234");
    defer { w_advert_stop(nearby); };
    const XrAsyncRequestIdFB request_id = start_discovery();

    XrEventDataColocationDiscoveryResultMETA result = {};
    ASSERT_TRUE(discovered(result));
    EXPECT_EQ(result.discoveryRequestId, request_id);
    EXPECT_EQ(result.advertisementUuid.data[15], 1);
    ASSERT_EQ(result.bufferSize, 10u);
    EXPECT_TRUE(memory_equal(result.buffer, "group-1234", 10));
    EXPECT_FALSE(discovered(result)) << "each advertisement is reported once";

    start_discovery(XR_COLOCATION_DISCOVERY_ALREADY_DISCOVERING_META);

    const XrColocationDiscoveryStopInfoMETA stop       = { .type = XR_TYPE_COLOCATION_DISCOVERY_STOP_INFO_META };
    XrAsyncRequestIdFB                      stop_id    = 0;
    ASSERT_EQ(colocation.xrStopColocationDiscoveryMETA(session, &stop, &stop_id), XR_SUCCESS);
    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_STOP_COLOCATION_DISCOVERY_COMPLETE_META, event));
    EXPECT_EQ(((XrEventDataStopColocationDiscoveryCompleteMETA*)&event)->requestId, stop_id);
    EXPECT_EQ(((XrEventDataStopColocationDiscoveryCompleteMETA*)&event)->result, XR_SUCCESS);
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_COLOCATION_DISCOVERY_COMPLETE_META, event));
    EXPECT_EQ(((XrEventDataColocationDiscoveryCompleteMETA*)&event)->discoveryRequestId, request_id);

    OS_LockFile* later = advertise_elsewhere(2, "late");
    defer { w_advert_stop(later); };
    EXPECT_FALSE(discovered(result)) << "discovery stopped";
}

TEST_F(OxrColocationTest, advertisements_are_visible_while_they_run)
{
    start_colocation();
    const XrUuid uuid = start_advertising("hello");
    Array<W_Advert> listed = {};
    defer { listed.release(); };
    ASSERT_EQ(w_advert_list(adverts, listed), 1u);
    EXPECT_TRUE(memory_equal(listed[0].uuid.bytes, uuid.data, XR_UUID_SIZE));
    EXPECT_EQ(listed[0].owner, 1u);
    EXPECT_TRUE(memory_equal(listed[0].buffer, "hello", 5));
    start_advertising("again", XR_COLOCATION_DISCOVERY_ALREADY_ADVERTISING_META);

    // Discovery does not report the headset's own advertisement
    start_discovery();
    XrEventDataColocationDiscoveryResultMETA result = {};
    EXPECT_FALSE(discovered(result));

    const XrColocationAdvertisementStopInfoMETA stop    = { .type = XR_TYPE_COLOCATION_ADVERTISEMENT_STOP_INFO_META };
    XrAsyncRequestIdFB                          stop_id = 0;
    ASSERT_EQ(colocation.xrStopColocationAdvertisementMETA(session, &stop, &stop_id), XR_SUCCESS);
    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_STOP_COLOCATION_ADVERTISEMENT_COMPLETE_META, event));
    EXPECT_EQ(((XrEventDataStopColocationAdvertisementCompleteMETA*)&event)->requestId, stop_id);
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_COLOCATION_ADVERTISEMENT_COMPLETE_META, event));
    EXPECT_EQ(w_advert_list(adverts, listed), 0u);
}

TEST_F(OxrColocationTest, advertisements_end_with_their_session)
{
    start_colocation();
    start_advertising("bye");
    Array<W_Advert> listed = {};
    defer { listed.release(); };
    ASSERT_EQ(w_advert_list(adverts, listed), 1u);
    ASSERT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
    session = XR_NULL_HANDLE;
    EXPECT_EQ(w_advert_list(adverts, listed), 0u);
}

TEST_F(OxrColocationTest, colocation_requests_validate_their_input)
{
    start_colocation();
    XrAsyncRequestIdFB                     request_id = 0;
    u8                                     big[XR_MAX_COLOCATION_DISCOVERY_BUFFER_SIZE_META + 1] = {};
    XrColocationAdvertisementStartInfoMETA info       = { .type = XR_TYPE_COLOCATION_ADVERTISEMENT_START_INFO_META, .bufferSize = sizeof(big), .buffer = big };
    EXPECT_EQ(colocation.xrStartColocationAdvertisementMETA(session, &info, &request_id), XR_ERROR_VALIDATION_FAILURE);
    info.bufferSize = 4;
    info.buffer     = nullptr;
    EXPECT_EQ(colocation.xrStartColocationAdvertisementMETA(session, &info, &request_id), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(colocation.xrStartColocationAdvertisementMETA(session, nullptr, &request_id), XR_ERROR_VALIDATION_FAILURE);
    const XrColocationDiscoveryStartInfoMETA discovery = { .type = XR_TYPE_COLOCATION_DISCOVERY_START_INFO_META };
    EXPECT_EQ(colocation.xrStartColocationDiscoveryMETA(session, &discovery, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(colocation.xrStartColocationDiscoveryMETA(XR_NULL_HANDLE, &discovery, &request_id), XR_ERROR_HANDLE_INVALID);
    const XrColocationDiscoveryStopInfoMETA wrong = { .type = XR_TYPE_COLOCATION_ADVERTISEMENT_STOP_INFO_META };
    EXPECT_EQ(colocation.xrStopColocationDiscoveryMETA(session, &wrong, &request_id), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrColocationTest, replays_discover_what_the_recording_discovered)
{
    char recording[OS_MAX_PATH];
    cstr_format(recording, sizeof(recording), "%s/colocation.xrrec", store);
    os_set_env("PUPPETXR_RECORD", recording);
    start_colocation();
    os_unset_env("PUPPETXR_RECORD");
    OS_LockFile* nearby = advertise_elsewhere(3, "recorded");
    start_discovery();
    XrEventDataColocationDiscoveryResultMETA result = {};
    ASSERT_TRUE(discovered(result));
    frame();
    xr.xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;
    w_advert_stop(nearby);

    os_set_env("PUPPETXR_REPLAY", recording);
    os_set_env("PUPPETXR_REPLAY_EXIT", "0");
    start_colocation();
    start_discovery();
    ASSERT_TRUE(discovered(result)) << "the advertisement is gone, the recording still has it";
    EXPECT_EQ(result.advertisementUuid.data[15], 3);
    EXPECT_TRUE(memory_equal(result.buffer, "recorded", 8));
}

//
// XR_FB_passthrough
//

#define PASSTHROUGH_FUNCTIONS(X)        \
    X(xrCreatePassthroughFB)            \
    X(xrDestroyPassthroughFB)           \
    X(xrPassthroughStartFB)             \
    X(xrPassthroughPauseFB)             \
    X(xrCreatePassthroughLayerFB)       \
    X(xrDestroyPassthroughLayerFB)      \
    X(xrPassthroughLayerPauseFB)        \
    X(xrPassthroughLayerResumeFB)       \
    X(xrPassthroughLayerSetStyleFB)     \
    X(xrCreateGeometryInstanceFB)       \
    X(xrDestroyGeometryInstanceFB)      \
    X(xrGeometryInstanceSetTransformFB)

namespace
{

struct PassthroughXr
{
#define DECLARE(fn) PFN_##fn fn;
    PASSTHROUGH_FUNCTIONS(DECLARE)
#undef DECLARE
};

struct OxrPassthroughTest : OxrVulkanTest
{
    PassthroughXr        pt          = {};
    char                 rooms[OS_MAX_PATH]   = {};
    XrPassthroughFB      passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB layer       = XR_NULL_HANDLE;

    OxrPassthroughTest ()
    {
        extensions[extensions_count++] = XR_FB_PASSTHROUGH_EXTENSION_NAME;
    }

    void SetUp () override
    {
        if (!os_create_temp_directory("puppetxr_rooms", rooms, sizeof(rooms)))
        {
            FAIL() << "no temporary directory";
        }
        TestRoom room = {};
        test_room_build(room);
        ASSERT_TRUE(test_room_write(room, rooms, "test_box"));
        os_set_env("PUPPETXR_ROOMS_DIR", rooms);
        os_set_env("PUPPETXR_ROOM", "test_box");
        OxrVulkanTest::SetUp();
        if (IsSkipped())
        {
            return;
        }
#define LOAD(fn) ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&pt.fn), XR_SUCCESS) << #fn;
        PASSTHROUGH_FUNCTIONS(LOAD)
#undef LOAD
    }

    void TearDown () override
    {
        OxrVulkanTest::TearDown();
        os_delete_tree(rooms);
        os_unset_env("PUPPETXR_ROOMS_DIR");
        os_unset_env("PUPPETXR_ROOM");
    }

    void start (XrPassthroughFlagsFB flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB)
    {
        create_vulkan();
        create_vulkan_session();
        begin_session();
        const XrPassthroughCreateInfoFB info = { .type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, .flags = flags };
        ASSERT_EQ(pt.xrCreatePassthroughFB(session, &info, &passthrough), XR_SUCCESS);
        const XrPassthroughLayerCreateInfoFB layer_info = { .type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, .passthrough = passthrough, .flags = flags, .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB };
        ASSERT_EQ(pt.xrCreatePassthroughLayerFB(session, &layer_info, &layer), XR_SUCCESS);
    }

    // The head looks at the red -z wall from the room's center, or down at the blue table
    void look (b8 at_table)
    {
        const Pose pose = at_table ? Pose{ { 0.5f, 1.6f, 0.0f }, { -0.70710678f, 0.0f, 0.0f, 0.70710678f } } : Pose{ { 0.0f, 1.6f, 0.0f }, zm::QUAT_IDENTITY };
        sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, pose);
    }

    const u8* center (const XrCompositionLayerBaseHeader* const* layers, u32 count, Array<u8>& pixels)
    {
        u32 width = 0;
        u32 height = 0;
        submit_and_read(layers, count, pixels, width, height);
        return eye_pixel(pixels, width, height, 0, 0.0f, 0.0f);
    }

    XrCompositionLayerPassthroughFB passthrough_layer (XrCompositionLayerFlags flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT)
    {
        return { .type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB, .flags = flags, .space = XR_NULL_HANDLE, .layerHandle = layer };
    }
};

} // namespace

#define EXPECT_RGB(p, r, g, b)            \
    do                                    \
    {                                     \
        EXPECT_NEAR((p)[0], r, 2) << "r"; \
        EXPECT_NEAR((p)[1], g, 2) << "g"; \
        EXPECT_NEAR((p)[2], b, 2) << "b"; \
    }                                     \
    while (0)

// Props are part of what the cameras see: drawn into passthrough with the room, in depth order
TEST_F(OxrPassthroughTest, props_show_in_passthrough)
{
    start();
    if (IsSkipped())
    {
        return;
    }
    look(false);
    const XrCompositionLayerPassthroughFB layer_struct = passthrough_layer();
    const XrCompositionLayerBaseHeader*   layers[]     = { (const XrCompositionLayerBaseHeader*)&layer_struct };
    Array<u8>                             pixels       = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    const u8 wall[3] = { eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[0], eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[1], eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[2] };

    // A 30 cm QR code half a metre ahead: black and white modules on a white quiet zone
    SIM_Prop qr = { .kind = SIM_PropKind::QrCode, .pose = { { 0.0f, 1.6f, -0.5f }, zm::QUAT_IDENTITY }, .size = 0.3f };
    cstr_copy(qr.text, sizeof(qr.text), "https://example.com/puppetxr");
    const u32 id = sim_add_prop(oxr_get_sim(), qr);
    ASSERT_NE(id, 0u);
    submit_and_read(layers, 1, pixels, width, height);
    u32 dark  = 0;
    u32 light = 0;
    for (f32 y = -0.2f; y <= 0.2f; y += 0.02f)
    {
        for (f32 x = -0.2f; x <= 0.2f; x += 0.02f)
        {
            const u8* p = eye_pixel(pixels, width, height, 0, x, y);
            dark  += p[0] < 40 && p[1] < 40 && p[2] < 40 ? 1u : 0u;
            light += p[0] > 200 && p[1] > 200 && p[2] > 200 ? 1u : 0u;
        }
    }
    EXPECT_GT(dark, 60u);
    EXPECT_GT(light, 60u);
    // PUPPETXR_TEST_DUMP_DIR=<dir> keeps the frame, for reading the code with a real decoder
    if (os_env("PUPPETXR_TEST_DUMP_DIR"))
    {
        char path[OS_MAX_PATH];
        cstr_format(path, sizeof(path), "%s/passthrough_qr.png", os_env("PUPPETXR_TEST_DUMP_DIR"));
        EXPECT_TRUE(oxr_save_screenshot(path));
    }
    const u8* beside = eye_pixel(pixels, width, height, 0, 0.8f, 0.0f); // inside the lens, outside the code
    EXPECT_RGB(beside, wall[0], wall[1], wall[2]);

    // Removed, the wall shows again; a keyboard shows its keys and body
    ASSERT_TRUE(sim_remove_prop(oxr_get_sim(), id));
    const SIM_Prop keyboard = { .kind = SIM_PropKind::Keyboard, .pose = { { 0.0f, 1.6f, -0.4f }, zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, 0.5f * zm::PI) } };
    ASSERT_NE(sim_add_prop(oxr_get_sim(), keyboard), 0u);
    submit_and_read(layers, 1, pixels, width, height);
    u32 keys = 0;
    u32 body = 0;
    for (f32 x = -0.3f; x <= 0.3f; x += 0.01f)
    {
        const u8* p = eye_pixel(pixels, width, height, 0, x, 0.0f);
        keys += p[0] > 150 && p[0] < 230 && p[0] == p[1] ? 1u : 0u;
        body += p[0] < 70 && p[1] < 70 && p[2] < 80 ? 1u : 0u;
    }
    EXPECT_GT(keys, 5u);
    EXPECT_GT(body, 5u);
}

TEST_F(OxrPassthroughTest, system_reports_color_passthrough)
{
    XrSystemPassthroughProperties2FB properties2 = { .type = XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB };
    XrSystemPassthroughPropertiesFB  properties  = { .type = XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB, .next = &properties2 };
    XrSystemProperties               system_props = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &properties };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &system_props), XR_SUCCESS);
    EXPECT_TRUE(properties.supportsPassthrough);
    EXPECT_EQ(properties2.capabilities, (XrPassthroughCapabilityFlagsFB)(XR_PASSTHROUGH_CAPABILITY_BIT_FB | XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB));
}

TEST_F(OxrPassthroughTest, one_passthrough_per_session_and_layers_need_it)
{
    create_vulkan();
    create_vulkan_session();
    const XrPassthroughLayerCreateInfoFB layer_info = { .type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, .passthrough = XR_NULL_HANDLE, .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB };
    EXPECT_EQ(pt.xrCreatePassthroughLayerFB(session, &layer_info, &layer), XR_ERROR_HANDLE_INVALID);

    const XrPassthroughCreateInfoFB info = { .type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
    ASSERT_EQ(pt.xrCreatePassthroughFB(session, &info, &passthrough), XR_SUCCESS);
    XrPassthroughFB second = XR_NULL_HANDLE;
    EXPECT_EQ(pt.xrCreatePassthroughFB(session, &info, &second), XR_ERROR_FEATURE_ALREADY_CREATED_PASSTHROUGH_FB);
    const XrPassthroughCreateInfoFB bad_flags = { .type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, .flags = 0x100 };
    EXPECT_EQ(pt.xrCreatePassthroughFB(session, &bad_flags, &second), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(pt.xrCreatePassthroughFB(session, nullptr, &second), XR_ERROR_VALIDATION_FAILURE);

    XrPassthroughLayerCreateInfoFB valid = { .type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, .passthrough = passthrough, .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB };
    ASSERT_EQ(pt.xrCreatePassthroughLayerFB(session, &valid, &layer), XR_SUCCESS);
    valid.purpose = (XrPassthroughLayerPurposeFB)77;
    XrPassthroughLayerFB other = XR_NULL_HANDLE;
    EXPECT_EQ(pt.xrCreatePassthroughLayerFB(session, &valid, &other), XR_ERROR_VALIDATION_FAILURE);
    valid.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB;
    EXPECT_EQ(pt.xrCreatePassthroughLayerFB(session, &valid, &other), XR_SUCCESS);
    EXPECT_EQ(pt.xrDestroyPassthroughLayerFB(other), XR_SUCCESS);
    EXPECT_EQ(pt.xrDestroyPassthroughLayerFB(other), XR_ERROR_HANDLE_INVALID);

    // A destroyed feature can be created again
    EXPECT_EQ(pt.xrDestroyPassthroughLayerFB(layer), XR_SUCCESS);
    EXPECT_EQ(pt.xrDestroyPassthroughFB(passthrough), XR_SUCCESS);
    EXPECT_EQ(pt.xrPassthroughStartFB(passthrough), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(pt.xrCreatePassthroughFB(session, &info, &passthrough), XR_SUCCESS);
}

TEST_F(OxrPassthroughTest, the_room_shows_through_a_passthrough_layer)
{
    start();
    XrCompositionLayerPassthroughFB      composition = passthrough_layer();
    const XrCompositionLayerBaseHeader*  layers[1]   = { (XrCompositionLayerBaseHeader*)&composition };
    Array<u8>                            pixels      = {};
    defer { pixels.release(); };

    look(false);
    EXPECT_RGB(center(layers, 1, pixels), 255, 0, 0);
    look(true);
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 255);
    EXPECT_RGB(center(nullptr, 0, pixels), 0, 0, 0);

    // Paused features and layers show nothing
    EXPECT_EQ(pt.xrPassthroughLayerPauseFB(layer), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 0);
    EXPECT_EQ(pt.xrPassthroughLayerResumeFB(layer), XR_SUCCESS);
    EXPECT_EQ(pt.xrPassthroughPauseFB(passthrough), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 0);
    EXPECT_EQ(pt.xrPassthroughStartFB(passthrough), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 255);

    // Without a room the cameras see nothing simulated
    ASSERT_EQ(oxr_set_room(nullptr), OXR_RoomResult::Ok);
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 0);
    ASSERT_EQ(oxr_set_room("test_box"), OXR_RoomResult::Ok);
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 255);
}

TEST_F(OxrPassthroughTest, layers_created_paused_need_starting)
{
    start(0);
    XrCompositionLayerPassthroughFB     composition = passthrough_layer();
    const XrCompositionLayerBaseHeader* layers[1]   = { (XrCompositionLayerBaseHeader*)&composition };
    Array<u8>                           pixels      = {};
    defer { pixels.release(); };
    look(false);
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 0);
    EXPECT_EQ(pt.xrPassthroughStartFB(passthrough), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 0);
    EXPECT_EQ(pt.xrPassthroughLayerResumeFB(layer), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 255, 0, 0);
}

TEST_F(OxrPassthroughTest, styles)
{
    start();
    XrCompositionLayerPassthroughFB     composition = passthrough_layer();
    const XrCompositionLayerBaseHeader* layers[1]   = { (XrCompositionLayerBaseHeader*)&composition };
    Array<u8>                           pixels      = {};
    defer { pixels.release(); };
    look(false);

    XrPassthroughStyleFB style = { .type = XR_TYPE_PASSTHROUGH_STYLE_FB, .textureOpacityFactor = 0.5f, .edgeColor = { 0, 0, 0, 0 } };
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 188, 0, 0);
    composition.flags = XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT; // without source alpha the layer is opaque
    EXPECT_RGB(center(layers, 1, pixels), 255, 0, 0);
    composition.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;

    XrPassthroughColorMapMonoToMonoFB mono = { .type = XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_MONO_FB };
    memset(mono.textureColorMap, 128, sizeof(mono.textureColorMap));
    style = { .type = XR_TYPE_PASSTHROUGH_STYLE_FB, .next = &mono, .textureOpacityFactor = 1.0f };
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 128, 128, 128);

    XrPassthroughColorMapMonoToRgbaFB rgba = { .type = XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_RGBA_FB };
    for (XrColor4f& color : rgba.textureColorMap)
    {
        color = { 0.0f, 1.0f, 0.0f, 1.0f };
    }
    style.next = &rgba;
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 0, 255, 0);

    XrPassthroughBrightnessContrastSaturationFB bcs = { .type = XR_TYPE_PASSTHROUGH_BRIGHTNESS_CONTRAST_SATURATION_FB, .brightness = 0.0f, .contrast = 1.0f, .saturation = 0.0f };
    style.next = &bcs;
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    const u8* gray = center(layers, 1, pixels);
    EXPECT_NEAR(gray[0], gray[1], 3);
    EXPECT_NEAR(gray[1], gray[2], 3);
    EXPECT_GT(gray[0], 60);
    bcs.brightness = -100.0f;
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 0);

    // Maps are mutually exclusive, values have ranges
    mono.next  = &rgba;
    style.next = &mono;
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_ERROR_VALIDATION_FAILURE);
    mono.next                  = nullptr;
    style.textureOpacityFactor = 1.5f;
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_ERROR_VALIDATION_FAILURE);
    style.textureOpacityFactor = 1.0f;
    bcs.brightness             = 101.0f;
    style.next                 = &bcs;
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_ERROR_VALIDATION_FAILURE);
    bcs.brightness = 0.0f;
    bcs.contrast   = -1.0f;
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(XR_NULL_HANDLE, &style), XR_ERROR_HANDLE_INVALID);
}

//
// XR_META_passthrough_color_lut
//

#define COLOR_LUT_FUNCTIONS(X)            \
    X(xrCreatePassthroughColorLutMETA)    \
    X(xrDestroyPassthroughColorLutMETA)   \
    X(xrUpdatePassthroughColorLutMETA)

namespace
{

struct OxrColorLutTest : OxrPassthroughTest
{
#define DECLARE(fn) PFN_##fn fn = nullptr;
    COLOR_LUT_FUNCTIONS(DECLARE)
#undef DECLARE

    OxrColorLutTest ()
    {
        extensions[extensions_count++] = XR_META_PASSTHROUGH_COLOR_LUT_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrPassthroughTest::SetUp();
        if (IsSkipped() || HasFatalFailure())
        {
            return;
        }
#define LOAD(fn) ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&fn), XR_SUCCESS) << #fn;
        COLOR_LUT_FUNCTIONS(LOAD)
#undef LOAD
    }

    // Every input color maps to out(r, g, b), each input channel 0 or 255 at the LUT's corners
    template <typename F>
    static void fill_lut (u8* data, u32 resolution, u32 channels, F out)
    {
        for (u32 b = 0; b < resolution; b++)
        {
            for (u32 g = 0; g < resolution; g++)
            {
                for (u32 r = 0; r < resolution; r++)
                {
                    const f32 max   = (f32)(resolution - 1);
                    u8        c[4]  = { 0, 0, 0, 255 };
                    out((f32)r / max, (f32)g / max, (f32)b / max, c);
                    memcpy(data + channels * (r + g * resolution + b * resolution * resolution), c, channels);
                }
            }
        }
    }

    XrPassthroughColorLutMETA create_lut (u32 resolution, XrPassthroughColorLutChannelsMETA channels, const u8* data, u32 size) const
    {
        const XrPassthroughColorLutCreateInfoMETA info = { .type = XR_TYPE_PASSTHROUGH_COLOR_LUT_CREATE_INFO_META, .channels = channels, .resolution = resolution, .data = { .bufferSize = size, .buffer = data } };
        XrPassthroughColorLutMETA                 lut  = XR_NULL_HANDLE;
        EXPECT_EQ(xrCreatePassthroughColorLutMETA(passthrough, &info, &lut), XR_SUCCESS);
        return lut;
    }
};

} // namespace

TEST_F(OxrPassthroughTest, color_luts_need_their_extension)
{
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrCreatePassthroughColorLutMETA", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
}

TEST_F(OxrColorLutTest, luts_are_validated)
{
    XrSystemPassthroughColorLutPropertiesMETA properties   = { .type = XR_TYPE_SYSTEM_PASSTHROUGH_COLOR_LUT_PROPERTIES_META };
    XrSystemProperties                        system_props = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &properties };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &system_props), XR_SUCCESS);
    EXPECT_EQ(properties.maxColorLutResolution, 64u);

    start();
    // The largest LUT is 1 MiB, the whole default stack on Windows
    const u32 data_size = 64 * 64 * 64 * 4;
    u8*       data      = (u8*)os_memory_alloc(data_size);
    defer { os_memory_release(data); };
    memory_zero(data, data_size);
    XrPassthroughColorLutCreateInfoMETA info = { .type = XR_TYPE_PASSTHROUGH_COLOR_LUT_CREATE_INFO_META, .channels = XR_PASSTHROUGH_COLOR_LUT_CHANNELS_RGB_META, .resolution = 4, .data = { .bufferSize = 4 * 4 * 4 * 3, .buffer = data } };
    XrPassthroughColorLutMETA           lut  = XR_NULL_HANDLE;
    EXPECT_EQ(xrCreatePassthroughColorLutMETA(passthrough, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xrCreatePassthroughColorLutMETA(XR_NULL_HANDLE, &info, &lut), XR_ERROR_HANDLE_INVALID);
    info.resolution = 3; // not a power of 2
    EXPECT_EQ(xrCreatePassthroughColorLutMETA(passthrough, &info, &lut), XR_ERROR_VALIDATION_FAILURE);
    info.resolution = 0;
    EXPECT_EQ(xrCreatePassthroughColorLutMETA(passthrough, &info, &lut), XR_ERROR_VALIDATION_FAILURE);
    info.resolution = 128;
    EXPECT_EQ(xrCreatePassthroughColorLutMETA(passthrough, &info, &lut), XR_ERROR_VALIDATION_FAILURE);
    info.resolution = 4;
    info.channels   = (XrPassthroughColorLutChannelsMETA)3;
    EXPECT_EQ(xrCreatePassthroughColorLutMETA(passthrough, &info, &lut), XR_ERROR_VALIDATION_FAILURE);
    info.channels = XR_PASSTHROUGH_COLOR_LUT_CHANNELS_RGBA_META; // 4 bytes per element now
    EXPECT_EQ(xrCreatePassthroughColorLutMETA(passthrough, &info, &lut), XR_ERROR_PASSTHROUGH_COLOR_LUT_BUFFER_SIZE_MISMATCH_META);
    info.data.bufferSize = 4 * 4 * 4 * 4;
    info.data.buffer     = nullptr;
    EXPECT_EQ(xrCreatePassthroughColorLutMETA(passthrough, &info, &lut), XR_ERROR_VALIDATION_FAILURE);
    info.data.buffer = data;
    ASSERT_EQ(xrCreatePassthroughColorLutMETA(passthrough, &info, &lut), XR_SUCCESS);
    info.resolution      = 64;
    info.data.bufferSize = data_size;
    XrPassthroughColorLutMETA largest = XR_NULL_HANDLE;
    EXPECT_EQ(xrCreatePassthroughColorLutMETA(passthrough, &info, &largest), XR_SUCCESS);
    EXPECT_EQ(xrDestroyPassthroughColorLutMETA(largest), XR_SUCCESS);

    XrPassthroughColorLutUpdateInfoMETA update = { .type = XR_TYPE_PASSTHROUGH_COLOR_LUT_UPDATE_INFO_META, .data = { .bufferSize = 4 * 4 * 4 * 3, .buffer = data } };
    EXPECT_EQ(xrUpdatePassthroughColorLutMETA(lut, &update), XR_ERROR_PASSTHROUGH_COLOR_LUT_BUFFER_SIZE_MISMATCH_META);
    update.data.bufferSize = 4 * 4 * 4 * 4;
    EXPECT_EQ(xrUpdatePassthroughColorLutMETA(lut, &update), XR_SUCCESS);
    EXPECT_EQ(xrUpdatePassthroughColorLutMETA(lut, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xrUpdatePassthroughColorLutMETA(XR_NULL_HANDLE, &update), XR_ERROR_HANDLE_INVALID);

    // Styles: weights in [0, 1], valid LUTs, one color map
    XrPassthroughColorMapLutMETA map   = { .type = XR_TYPE_PASSTHROUGH_COLOR_MAP_LUT_META, .colorLut = lut, .weight = 1.5f };
    XrPassthroughStyleFB         style = { .type = XR_TYPE_PASSTHROUGH_STYLE_FB, .next = &map, .textureOpacityFactor = 1.0f };
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_ERROR_VALIDATION_FAILURE);
    map.weight = zm::nan();
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_ERROR_VALIDATION_FAILURE);
    map.weight   = 1.0f;
    map.colorLut = largest;
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_ERROR_HANDLE_INVALID);
    map.colorLut                                   = lut;
    XrPassthroughColorMapInterpolatedLutMETA lerp  = { .type = XR_TYPE_PASSTHROUGH_COLOR_MAP_INTERPOLATED_LUT_META, .sourceColorLut = lut, .targetColorLut = XR_NULL_HANDLE, .weight = 0.5f };
    style.next                                     = &lerp;
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_ERROR_HANDLE_INVALID);
    lerp.targetColorLut = lut;
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    map.next = &lerp;
    style.next = &map;
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_ERROR_VALIDATION_FAILURE);

    EXPECT_EQ(xrDestroyPassthroughColorLutMETA(lut), XR_SUCCESS);
    EXPECT_EQ(xrDestroyPassthroughColorLutMETA(lut), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(xrUpdatePassthroughColorLutMETA(lut, &update), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrColorLutTest, luts_map_the_imagery)
{
    start();
    XrCompositionLayerPassthroughFB     composition = passthrough_layer();
    const XrCompositionLayerBaseHeader* layers[1]   = { (XrCompositionLayerBaseHeader*)&composition };
    Array<u8>                           pixels      = {};
    defer { pixels.release(); };
    look(false);

    // Red and blue swap; the red wall turns blue
    u8 swap[2 * 2 * 2 * 3] = {};
    fill_lut(swap, 2, 3, [] (f32 r, f32 g, f32 b, u8* c) { c[0] = (u8)(b * 255.0f); c[1] = (u8)(g * 255.0f); c[2] = (u8)(r * 255.0f); });
    const XrPassthroughColorLutMETA swap_lut = create_lut(2, XR_PASSTHROUGH_COLOR_LUT_CHANNELS_RGB_META, swap, sizeof(swap));
    XrPassthroughColorMapLutMETA    map      = { .type = XR_TYPE_PASSTHROUGH_COLOR_MAP_LUT_META, .colorLut = swap_lut, .weight = 1.0f };
    XrPassthroughStyleFB            style    = { .type = XR_TYPE_PASSTHROUGH_STYLE_FB, .next = &map, .textureOpacityFactor = 1.0f };
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 255);

    // The weight blends the imagery and the mapped colors, in sRGB like the LUT
    map.weight = 0.5f;
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 128, 0, 128);
    map.weight = 1.0f;
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);

    // A 16-element identity keeps the colors (tests_render.cpp checks the interpolation)
    static u8 identity[16 * 16 * 16 * 4] = {};
    fill_lut(identity, 16, 4, [] (f32 r, f32 g, f32 b, u8* c) { c[0] = (u8)(r * 255.0f + 0.5f); c[1] = (u8)(g * 255.0f + 0.5f); c[2] = (u8)(b * 255.0f + 0.5f); });
    const XrPassthroughColorLutMETA identity_lut = create_lut(16, XR_PASSTHROUGH_COLOR_LUT_CHANNELS_RGBA_META, identity, sizeof(identity));
    map.colorLut                                 = identity_lut;
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 255, 0, 0);

    // Updates show on the layers using the LUT
    map.colorLut = swap_lut;
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    const XrPassthroughColorLutUpdateInfoMETA update = { .type = XR_TYPE_PASSTHROUGH_COLOR_LUT_UPDATE_INFO_META, .data = { .bufferSize = sizeof(swap), .buffer = swap } };
    fill_lut(swap, 2, 3, [] (f32, f32, f32, u8* c) { c[0] = 255; c[1] = 255; });
    ASSERT_EQ(xrUpdatePassthroughColorLutMETA(swap_lut, &update), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 255, 255, 0);

    // Interpolated LUTs blend source and target, here of different resolutions
    XrPassthroughColorMapInterpolatedLutMETA lerp = { .type = XR_TYPE_PASSTHROUGH_COLOR_MAP_INTERPOLATED_LUT_META, .sourceColorLut = identity_lut, .targetColorLut = swap_lut, .weight = 0.5f };
    style.next                                    = &lerp;
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 255, 128, 0);

    // Spec: a destroyed LUT still applies to the layers using it until their style changes
    EXPECT_EQ(xrDestroyPassthroughColorLutMETA(identity_lut), XR_SUCCESS);
    EXPECT_EQ(xrDestroyPassthroughColorLutMETA(swap_lut), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 255, 128, 0);
    style.next = nullptr;
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 255, 0, 0);
}

// Destroying the passthrough, a layer or the session with LUTs in use frees them
TEST_F(OxrColorLutTest, luts_in_use_go_with_their_owners)
{
    start();
    u8                              data[2 * 2 * 2 * 3] = {};
    const XrPassthroughColorLutMETA lut                 = create_lut(2, XR_PASSTHROUGH_COLOR_LUT_CHANNELS_RGB_META, data, sizeof(data));
    XrPassthroughColorMapLutMETA    map                 = { .type = XR_TYPE_PASSTHROUGH_COLOR_MAP_LUT_META, .colorLut = lut, .weight = 1.0f };
    const XrPassthroughStyleFB      style               = { .type = XR_TYPE_PASSTHROUGH_STYLE_FB, .next = &map, .textureOpacityFactor = 1.0f };
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    EXPECT_EQ(pt.xrDestroyPassthroughFB(passthrough), XR_SUCCESS);
    EXPECT_EQ(xrDestroyPassthroughColorLutMETA(lut), XR_ERROR_HANDLE_INVALID); // a child of the passthrough
    EXPECT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(pt.xrDestroyPassthroughLayerFB(layer), XR_SUCCESS);

    const XrPassthroughCreateInfoFB info = { .type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB };
    ASSERT_EQ(pt.xrCreatePassthroughFB(session, &info, &passthrough), XR_SUCCESS);
    const XrPassthroughLayerCreateInfoFB layer_info = { .type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, .passthrough = passthrough, .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB, .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB };
    ASSERT_EQ(pt.xrCreatePassthroughLayerFB(session, &layer_info, &layer), XR_SUCCESS);
    map.colorLut = create_lut(2, XR_PASSTHROUGH_COLOR_LUT_CHANNELS_RGB_META, data, sizeof(data));
    ASSERT_EQ(pt.xrPassthroughLayerSetStyleFB(layer, &style), XR_SUCCESS);
    EXPECT_EQ(xrDestroyPassthroughColorLutMETA(map.colorLut), XR_SUCCESS);
    // The session ends with the layer still using the destroyed LUT
}

TEST_F(OxrPassthroughTest, app_content_composites_over_passthrough)
{
    start();
    look(false);
    const XrSwapchain swapchain = create_swapchain(32, 32, 1);
    const XrSpace     view      = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(swapchain, { { 0.0f, 1.0f, 0.0f, 1.0f } });
    XrCompositionLayerQuad              quad        = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 0.5f, 0.5f } };
    XrCompositionLayerPassthroughFB     composition = passthrough_layer();
    const XrCompositionLayerBaseHeader* layers[2]   = { (XrCompositionLayerBaseHeader*)&composition, (XrCompositionLayerBaseHeader*)&quad };
    Array<u8>                           pixels      = {};
    defer { pixels.release(); };
    u32 width = 0;
    u32 height = 0;
    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_RGB(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f), 0, 255, 0);
    EXPECT_RGB(eye_pixel(pixels, width, height, 0, 0.6f, 0.0f), 255, 0, 0);

    // The passthrough layer on top hides the quad
    const XrCompositionLayerBaseHeader* reversed[2] = { (XrCompositionLayerBaseHeader*)&quad, (XrCompositionLayerBaseHeader*)&composition };
    submit_and_read(reversed, 2, pixels, width, height);
    EXPECT_RGB(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f), 255, 0, 0);
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

TEST_F(OxrPassthroughTest, passthrough_layers_are_validated)
{
    start();
    XrFrameState           state      = { .type = XR_TYPE_FRAME_STATE };
    const XrFrameWaitInfo  wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
    const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);

    XrCompositionLayerPassthroughFB     composition = passthrough_layer(0);
    const XrCompositionLayerBaseHeader* layers[1]   = { (XrCompositionLayerBaseHeader*)&composition };
    const XrFrameEndInfo                      end_info    = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = layers };
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_VALIDATION_FAILURE);
    composition = passthrough_layer();
    composition.layerHandle = (XrPassthroughLayerFB)(uintptr_t)0x1234;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_HANDLE_INVALID);
    composition = passthrough_layer();
    composition.space = (XrSpace)(uintptr_t)0x5678;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_HANDLE_INVALID);
    composition.space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
}

TEST_F(OxrPassthroughTest, geometry_instances_need_triangle_meshes)
{
    start();
    const XrGeometryInstanceCreateInfoFB info = { .type = XR_TYPE_GEOMETRY_INSTANCE_CREATE_INFO_FB, .layer = layer, .mesh = (XrTriangleMeshFB)(uintptr_t)0x42, .baseSpace = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE), .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .scale = { 1, 1, 1 } };
    XrGeometryInstanceFB geometry = XR_NULL_HANDLE;
    EXPECT_EQ(pt.xrCreateGeometryInstanceFB(session, &info, &geometry), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(pt.xrDestroyGeometryInstanceFB((XrGeometryInstanceFB)(uintptr_t)0x42), XR_ERROR_HANDLE_INVALID);
    const XrGeometryInstanceTransformFB transform = { .type = XR_TYPE_GEOMETRY_INSTANCE_TRANSFORM_FB, .baseSpace = info.baseSpace, .time = 1, .pose = info.pose, .scale = info.scale };
    EXPECT_EQ(pt.xrGeometryInstanceSetTransformFB((XrGeometryInstanceFB)(uintptr_t)0x42, &transform), XR_ERROR_HANDLE_INVALID);
}

//
// XR_FB_triangle_mesh and projected passthrough
//

#define TRIANGLE_MESH_FUNCTIONS(X)              \
    X(xrCreateTriangleMeshFB)                   \
    X(xrDestroyTriangleMeshFB)                  \
    X(xrTriangleMeshGetVertexBufferFB)          \
    X(xrTriangleMeshGetIndexBufferFB)           \
    X(xrTriangleMeshBeginUpdateFB)              \
    X(xrTriangleMeshEndUpdateFB)                \
    X(xrTriangleMeshBeginVertexBufferUpdateFB)  \
    X(xrTriangleMeshEndVertexBufferUpdateFB)

namespace
{

struct TriangleMeshXr
{
#define DECLARE(fn) PFN_##fn fn;
    TRIANGLE_MESH_FUNCTIONS(DECLARE)
#undef DECLARE
};

// A square of side 0.4 m in its XY plane, facing +z
static const XrVector3f projected_square[4] = { { -0.2f, -0.2f, 0.0f }, { 0.2f, -0.2f, 0.0f }, { 0.2f, 0.2f, 0.0f }, { -0.2f, 0.2f, 0.0f } };
static const u32        projected_square_indices[6] = { 0, 1, 2, 0, 2, 3 };

struct OxrProjectedTest : OxrPassthroughTest
{
    TriangleMeshXr       tm        = {};
    XrPassthroughLayerFB projected = XR_NULL_HANDLE;

    OxrProjectedTest ()
    {
        extensions[extensions_count++] = XR_FB_TRIANGLE_MESH_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrPassthroughTest::SetUp();
        if (IsSkipped() || HasFatalFailure())
        {
            return;
        }
#define LOAD(fn) ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&tm.fn), XR_SUCCESS) << #fn;
        TRIANGLE_MESH_FUNCTIONS(LOAD)
#undef LOAD
    }

    void start_projected ()
    {
        start();
        const XrPassthroughLayerCreateInfoFB info = { .type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, .passthrough = passthrough, .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB, .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB };
        ASSERT_EQ(pt.xrCreatePassthroughLayerFB(session, &info, &projected), XR_SUCCESS);
    }

    XrTriangleMeshFB square ()
    {
        const XrTriangleMeshCreateInfoFB info = { .type = XR_TYPE_TRIANGLE_MESH_CREATE_INFO_FB, .windingOrder = XR_WINDING_ORDER_CCW_FB, .vertexCount = 4, .vertexBuffer = projected_square, .triangleCount = 2, .indexBuffer = projected_square_indices };
        XrTriangleMeshFB                 mesh = XR_NULL_HANDLE;
        EXPECT_EQ(tm.xrCreateTriangleMeshFB(session, &info, &mesh), XR_SUCCESS);
        return mesh;
    }

    XrGeometryInstanceFB instance_of (XrTriangleMeshFB mesh, XrSpace base, XrPosef pose)
    {
        const XrGeometryInstanceCreateInfoFB info = { .type = XR_TYPE_GEOMETRY_INSTANCE_CREATE_INFO_FB, .layer = projected, .mesh = mesh, .baseSpace = base, .pose = pose, .scale = { 1, 1, 1 } };
        XrGeometryInstanceFB                 out  = XR_NULL_HANDLE;
        EXPECT_EQ(pt.xrCreateGeometryInstanceFB(session, &info, &out), XR_SUCCESS);
        return out;
    }
};

} // namespace

TEST_F(OxrPassthroughTest, triangle_mesh_functions_need_their_extension)
{
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrCreateTriangleMeshFB", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
}

TEST_F(OxrProjectedTest, immutable_meshes_keep_their_data)
{
    start_projected();
    const XrTriangleMeshFB mesh = square();
    XrVector3f* vertices = nullptr;
    uint32_t*   indices  = nullptr;
    ASSERT_EQ(tm.xrTriangleMeshGetVertexBufferFB(mesh, &vertices), XR_SUCCESS);
    ASSERT_EQ(tm.xrTriangleMeshGetIndexBufferFB(mesh, &indices), XR_SUCCESS);
    ASSERT_NE(vertices, nullptr);
    ASSERT_NE(indices, nullptr);
    EXPECT_FLOAT_EQ(vertices[2].x, 0.2f);
    EXPECT_EQ(indices[5], 3u);

    // Spec: the update functions are for mutable meshes only
    uint32_t count = 0;
    EXPECT_EQ(tm.xrTriangleMeshBeginUpdateFB(mesh), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(tm.xrTriangleMeshEndUpdateFB(mesh, 4, 2), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(tm.xrTriangleMeshBeginVertexBufferUpdateFB(mesh, &count), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(tm.xrTriangleMeshEndVertexBufferUpdateFB(mesh), XR_ERROR_VALIDATION_FAILURE);

    EXPECT_EQ(tm.xrDestroyTriangleMeshFB(mesh), XR_SUCCESS);
    EXPECT_EQ(tm.xrDestroyTriangleMeshFB(mesh), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(tm.xrTriangleMeshGetVertexBufferFB(mesh, &vertices), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrProjectedTest, mesh_creation_is_validated)
{
    start_projected();
    XrTriangleMeshFB           mesh = XR_NULL_HANDLE;
    XrTriangleMeshCreateInfoFB info = { .type = XR_TYPE_TRIANGLE_MESH_CREATE_INFO_FB, .windingOrder = XR_WINDING_ORDER_CCW_FB, .vertexCount = 4, .vertexBuffer = projected_square, .triangleCount = 2, .indexBuffer = projected_square_indices };
    EXPECT_EQ(tm.xrCreateTriangleMeshFB(session, nullptr, &mesh), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(tm.xrCreateTriangleMeshFB(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(tm.xrCreateTriangleMeshFB(XR_NULL_HANDLE, &info, &mesh), XR_ERROR_HANDLE_INVALID);
    info.flags = 0x80;
    EXPECT_EQ(tm.xrCreateTriangleMeshFB(session, &info, &mesh), XR_ERROR_VALIDATION_FAILURE);
    info.flags        = 0;
    info.windingOrder = (XrWindingOrderFB)9;
    EXPECT_EQ(tm.xrCreateTriangleMeshFB(session, &info, &mesh), XR_ERROR_VALIDATION_FAILURE);
    info.windingOrder = XR_WINDING_ORDER_UNKNOWN_FB;
    info.vertexBuffer = nullptr;
    EXPECT_EQ(tm.xrCreateTriangleMeshFB(session, &info, &mesh), XR_ERROR_VALIDATION_FAILURE);
    info.vertexBuffer  = projected_square;
    info.triangleCount = 0;
    EXPECT_EQ(tm.xrCreateTriangleMeshFB(session, &info, &mesh), XR_ERROR_VALIDATION_FAILURE);

    // Indices past the vertices would read outside the mesh
    const u32 bad[6] = { 0, 1, 2, 0, 2, 4 };
    info.triangleCount = 2;
    info.indexBuffer   = bad;
    EXPECT_EQ(tm.xrCreateTriangleMeshFB(session, &info, &mesh), XR_ERROR_VALIDATION_FAILURE);

    // Mutable meshes get their data later, so the buffers must be null
    info.flags       = XR_TRIANGLE_MESH_MUTABLE_BIT_FB;
    info.indexBuffer = projected_square_indices;
    EXPECT_EQ(tm.xrCreateTriangleMeshFB(session, &info, &mesh), XR_ERROR_VALIDATION_FAILURE);
    info.vertexBuffer = nullptr;
    info.indexBuffer  = nullptr;
    info.vertexCount  = 1u << 30;
    EXPECT_EQ(tm.xrCreateTriangleMeshFB(session, &info, &mesh), XR_ERROR_LIMIT_REACHED);
}

TEST_F(OxrProjectedTest, mutable_meshes_follow_the_state_machine)
{
    start_projected();
    const XrTriangleMeshCreateInfoFB info = { .type = XR_TYPE_TRIANGLE_MESH_CREATE_INFO_FB, .flags = XR_TRIANGLE_MESH_MUTABLE_BIT_FB, .windingOrder = XR_WINDING_ORDER_CCW_FB, .vertexCount = 8, .triangleCount = 4 };
    XrTriangleMeshFB mesh = XR_NULL_HANDLE;
    ASSERT_EQ(tm.xrCreateTriangleMeshFB(session, &info, &mesh), XR_SUCCESS);
    XrVector3f* vertices = nullptr;
    uint32_t*   indices  = nullptr;
    ASSERT_EQ(tm.xrTriangleMeshGetVertexBufferFB(mesh, &vertices), XR_SUCCESS);
    ASSERT_EQ(tm.xrTriangleMeshGetIndexBufferFB(mesh, &indices), XR_SUCCESS);

    // Undefined topology: only a full update may start
    uint32_t count = 0;
    EXPECT_EQ(tm.xrTriangleMeshBeginVertexBufferUpdateFB(mesh, &count), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(tm.xrTriangleMeshEndUpdateFB(mesh, 4, 2), XR_ERROR_CALL_ORDER_INVALID);
    ASSERT_EQ(tm.xrTriangleMeshBeginUpdateFB(mesh), XR_SUCCESS);
    EXPECT_EQ(tm.xrTriangleMeshBeginUpdateFB(mesh), XR_ERROR_CALL_ORDER_INVALID);
    memory_copy(vertices, projected_square, sizeof(projected_square));
    memory_copy(indices, projected_square_indices, sizeof(projected_square_indices));
    EXPECT_EQ(tm.xrTriangleMeshEndUpdateFB(mesh, 0, 2), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(tm.xrTriangleMeshEndUpdateFB(mesh, 9, 2), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(tm.xrTriangleMeshEndUpdateFB(mesh, 4, 5), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(tm.xrTriangleMeshEndUpdateFB(mesh, 4, 2), XR_SUCCESS);

    // Ready: vertices may move, keeping their count
    ASSERT_EQ(tm.xrTriangleMeshBeginVertexBufferUpdateFB(mesh, &count), XR_SUCCESS);
    EXPECT_EQ(count, 4u);
    EXPECT_EQ(tm.xrTriangleMeshBeginUpdateFB(mesh), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(tm.xrTriangleMeshBeginVertexBufferUpdateFB(mesh, &count), XR_ERROR_CALL_ORDER_INVALID);
    ASSERT_EQ(tm.xrTriangleMeshEndVertexBufferUpdateFB(mesh), XR_SUCCESS);
    EXPECT_EQ(tm.xrTriangleMeshEndVertexBufferUpdateFB(mesh), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(tm.xrTriangleMeshBeginVertexBufferUpdateFB(mesh, nullptr), XR_ERROR_VALIDATION_FAILURE);

    // Indices out of range are refused at the end of an update, the mesh stays as it was
    ASSERT_EQ(tm.xrTriangleMeshBeginUpdateFB(mesh), XR_SUCCESS);
    indices[0] = 7;
    EXPECT_EQ(tm.xrTriangleMeshEndUpdateFB(mesh, 4, 2), XR_ERROR_VALIDATION_FAILURE);
    indices[0] = 0;
    EXPECT_EQ(tm.xrTriangleMeshEndUpdateFB(mesh, 4, 2), XR_SUCCESS);
    EXPECT_EQ(tm.xrDestroyTriangleMeshFB(mesh), XR_SUCCESS);
}

TEST_F(OxrProjectedTest, geometry_instances_are_validated)
{
    start_projected();
    const XrTriangleMeshFB mesh  = square();
    const XrSpace          local = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    XrGeometryInstanceCreateInfoFB info = { .type = XR_TYPE_GEOMETRY_INSTANCE_CREATE_INFO_FB, .layer = layer, .mesh = mesh, .baseSpace = local, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .scale = { 1, 1, 1 } };
    XrGeometryInstanceFB geometry = XR_NULL_HANDLE;
    EXPECT_EQ(pt.xrCreateGeometryInstanceFB(session, &info, &geometry), XR_ERROR_VALIDATION_FAILURE) << "a reconstruction layer takes no geometry";
    info.layer = projected;
    info.pose.orientation = { 0, 0, 0, 2 };
    EXPECT_EQ(pt.xrCreateGeometryInstanceFB(session, &info, &geometry), XR_ERROR_POSE_INVALID);
    info.pose.orientation = { 0, 0, 0, 1 };
    info.baseSpace        = XR_NULL_HANDLE;
    EXPECT_EQ(pt.xrCreateGeometryInstanceFB(session, &info, &geometry), XR_ERROR_HANDLE_INVALID);
    info.baseSpace = local;
    EXPECT_EQ(pt.xrCreateGeometryInstanceFB(session, nullptr, &geometry), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(pt.xrCreateGeometryInstanceFB(session, &info, &geometry), XR_SUCCESS);

    XrGeometryInstanceTransformFB transform = { .type = XR_TYPE_GEOMETRY_INSTANCE_TRANSFORM_FB, .baseSpace = local, .time = 0, .pose = info.pose, .scale = info.scale };
    EXPECT_EQ(pt.xrGeometryInstanceSetTransformFB(geometry, &transform), XR_ERROR_TIME_INVALID);
    transform.time             = 1;
    transform.pose.orientation = { 0, 0, 0, 0 };
    EXPECT_EQ(pt.xrGeometryInstanceSetTransformFB(geometry, &transform), XR_ERROR_POSE_INVALID);
    transform.pose.orientation = { 0, 0, 0, 1 };
    EXPECT_EQ(pt.xrGeometryInstanceSetTransformFB(geometry, &transform), XR_SUCCESS);
    EXPECT_EQ(pt.xrGeometryInstanceSetTransformFB(geometry, nullptr), XR_ERROR_VALIDATION_FAILURE);

    // Spec: destroying the mesh or the layer destroys their instances, not the other way round
    EXPECT_EQ(pt.xrDestroyGeometryInstanceFB(geometry), XR_SUCCESS);
    EXPECT_EQ(pt.xrDestroyGeometryInstanceFB(geometry), XR_ERROR_HANDLE_INVALID);
    ASSERT_EQ(pt.xrCreateGeometryInstanceFB(session, &info, &geometry), XR_SUCCESS);
    EXPECT_EQ(tm.xrDestroyTriangleMeshFB(mesh), XR_SUCCESS);
    EXPECT_EQ(pt.xrGeometryInstanceSetTransformFB(geometry, &transform), XR_ERROR_HANDLE_INVALID);
    info.mesh = square();
    ASSERT_EQ(pt.xrCreateGeometryInstanceFB(session, &info, &geometry), XR_SUCCESS);
    EXPECT_EQ(pt.xrDestroyPassthroughLayerFB(projected), XR_SUCCESS);
    EXPECT_EQ(pt.xrDestroyGeometryInstanceFB(geometry), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrProjectedTest, projected_layers_show_the_room_on_their_geometry_only)
{
    start_projected();
    look(false);
    const XrSpace view = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    XrCompositionLayerPassthroughFB     composition = { .type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB, .flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, .layerHandle = projected };
    const XrCompositionLayerBaseHeader* layers[1]   = { (XrCompositionLayerBaseHeader*)&composition };
    Array<u8>                           pixels      = {};
    defer { pixels.release(); };

    // Without geometry a projected layer shows nothing
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 0);

    // A square a metre ahead: the red wall shows through it, nothing beside it
    const XrTriangleMeshFB     mesh     = square();
    const XrGeometryInstanceFB geometry = instance_of(mesh, view, { { 0, 0, 0, 1 }, { 0, 0, -1 } });
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_RGB(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f), 255, 0, 0);
    EXPECT_RGB(eye_pixel(pixels, width, height, 0, 0.6f, 0.0f), 0, 0, 0);
    EXPECT_RGB(eye_pixel(pixels, width, height, 1, 0.0f, 0.0f), 255, 0, 0);

    // Moved aside and scaled up
    const XrGeometryInstanceTransformFB transform = { .type = XR_TYPE_GEOMETRY_INSTANCE_TRANSFORM_FB, .baseSpace = view, .time = 1, .pose = { { 0, 0, 0, 1 }, { 0.6f, 0, -1 } }, .scale = { 2, 2, 1 } };
    ASSERT_EQ(pt.xrGeometryInstanceSetTransformFB(geometry, &transform), XR_SUCCESS);
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_RGB(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f), 0, 0, 0);
    EXPECT_RGB(eye_pixel(pixels, width, height, 0, 0.6f, 0.0f), 255, 0, 0);

    // A reconstruction layer ignores geometry
    composition.layerHandle = layer;
    EXPECT_RGB(center(layers, 1, pixels), 255, 0, 0);
}

TEST_F(OxrProjectedTest, mutable_meshes_show_their_last_finished_update)
{
    start_projected();
    look(false);
    const XrTriangleMeshCreateInfoFB info = { .type = XR_TYPE_TRIANGLE_MESH_CREATE_INFO_FB, .flags = XR_TRIANGLE_MESH_MUTABLE_BIT_FB, .vertexCount = 4, .triangleCount = 2 };
    XrTriangleMeshFB mesh = XR_NULL_HANDLE;
    ASSERT_EQ(tm.xrCreateTriangleMeshFB(session, &info, &mesh), XR_SUCCESS);
    XrVector3f* vertices = nullptr;
    uint32_t*   indices  = nullptr;
    ASSERT_EQ(tm.xrTriangleMeshGetVertexBufferFB(mesh, &vertices), XR_SUCCESS);
    ASSERT_EQ(tm.xrTriangleMeshGetIndexBufferFB(mesh, &indices), XR_SUCCESS);
    instance_of(mesh, reference_space(XR_REFERENCE_SPACE_TYPE_VIEW), { { 0, 0, 0, 1 }, { 0, 0, -1 } });

    XrCompositionLayerPassthroughFB     composition = { .type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB, .flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, .layerHandle = projected };
    const XrCompositionLayerBaseHeader* layers[1]   = { (XrCompositionLayerBaseHeader*)&composition };
    Array<u8>                           pixels      = {};
    defer { pixels.release(); };

    // Nothing shows before the first update ends
    ASSERT_EQ(tm.xrTriangleMeshBeginUpdateFB(mesh), XR_SUCCESS);
    memory_copy(vertices, projected_square, sizeof(projected_square));
    memory_copy(indices, projected_square_indices, sizeof(projected_square_indices));
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 0);
    ASSERT_EQ(tm.xrTriangleMeshEndUpdateFB(mesh, 4, 2), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 255, 0, 0);

    // Vertices moved off centre show once the vertex update ends
    uint32_t count = 0;
    ASSERT_EQ(tm.xrTriangleMeshBeginVertexBufferUpdateFB(mesh, &count), XR_SUCCESS);
    for (u32 i = 0; i < 4; i++)
    {
        vertices[i].x += 0.5f;
    }
    EXPECT_RGB(center(layers, 1, pixels), 255, 0, 0);
    ASSERT_EQ(tm.xrTriangleMeshEndVertexBufferUpdateFB(mesh), XR_SUCCESS);
    EXPECT_RGB(center(layers, 1, pixels), 0, 0, 0);
}

TEST_F(OxrPassthroughTest, alpha_blend_shows_the_room_behind_every_layer)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    u32 count = 0;
    XrEnvironmentBlendMode modes[4] = {};
    ASSERT_EQ(xr.xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 4, &count, modes), XR_SUCCESS);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(modes[0], XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
    EXPECT_EQ(modes[1], XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND);

    look(false);
    XrFrameState           state      = { .type = XR_TYPE_FRAME_STATE };
    const XrFrameWaitInfo  wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
    const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    oxr_set_mirror_view(SIM_MirrorView::Both);
    XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND };
    ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    u32 width = 0;
    u32 height = 0;
    u8  probe[4];
    ASSERT_FALSE(oxr_read_frame(probe, sizeof(probe), width, height));
    Array<u8> pixels = {};
    defer { pixels.release(); };
    pixels.resize((usize)width * height * 4);
    ASSERT_TRUE(oxr_read_frame(pixels.data, pixels.length, width, height));
    EXPECT_RGB(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f), 255, 0, 0);

    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    end_info.displayTime          = state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED);
}

//
// XR_META_environment_depth
//

#define DEPTH_FUNCTIONS(X)                            \
    X(xrCreateEnvironmentDepthProviderMETA)           \
    X(xrDestroyEnvironmentDepthProviderMETA)          \
    X(xrStartEnvironmentDepthProviderMETA)            \
    X(xrStopEnvironmentDepthProviderMETA)             \
    X(xrCreateEnvironmentDepthSwapchainMETA)          \
    X(xrDestroyEnvironmentDepthSwapchainMETA)         \
    X(xrEnumerateEnvironmentDepthSwapchainImagesMETA) \
    X(xrGetEnvironmentDepthSwapchainStateMETA)        \
    X(xrAcquireEnvironmentDepthImageMETA)             \
    X(xrSetEnvironmentDepthHandRemovalMETA)

namespace
{

struct DepthXr
{
#define DECLARE(fn) PFN_##fn fn;
    DEPTH_FUNCTIONS(DECLARE)
#undef DECLARE
};

struct OxrDepthTest : OxrVulkanTest
{
    DepthXr                         dx        = {};
    char                            rooms[OS_MAX_PATH] = {};
    XrEnvironmentDepthProviderMETA  provider  = XR_NULL_HANDLE;
    XrEnvironmentDepthSwapchainMETA depth     = XR_NULL_HANDLE;

    OxrDepthTest ()
    {
        extensions[extensions_count++] = XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME;
    }

    void SetUp () override
    {
        if (!os_create_temp_directory("puppetxr_rooms", rooms, sizeof(rooms)))
        {
            FAIL() << "no temporary directory";
        }
        TestRoom room = {};
        test_room_build(room);
        ASSERT_TRUE(test_room_write(room, rooms, "test_box"));
        os_set_env("PUPPETXR_ROOMS_DIR", rooms);
        os_set_env("PUPPETXR_ROOM", "test_box");
        OxrVulkanTest::SetUp();
        if (IsSkipped())
        {
            return;
        }
#define LOAD(fn) ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&dx.fn), XR_SUCCESS) << #fn;
        DEPTH_FUNCTIONS(LOAD)
#undef LOAD
    }

    void TearDown () override
    {
        OxrVulkanTest::TearDown();
        os_delete_tree(rooms);
        os_unset_env("PUPPETXR_ROOMS_DIR");
        os_unset_env("PUPPETXR_ROOM");
    }

    void start ()
    {
        create_vulkan();
        create_vulkan_session();
        begin_session();
        const XrEnvironmentDepthProviderCreateInfoMETA info = { .type = XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META };
        ASSERT_EQ(dx.xrCreateEnvironmentDepthProviderMETA(session, &info, &provider), XR_SUCCESS);
        const XrEnvironmentDepthSwapchainCreateInfoMETA swapchain_info = { .type = XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META };
        ASSERT_EQ(dx.xrCreateEnvironmentDepthSwapchainMETA(provider, &swapchain_info, &depth), XR_SUCCESS);
    }

    // Frames in which the provider acquires; the frame stays begun so the image can be read
    XrResult begin_and_acquire (XrSpace space, XrEnvironmentDepthImageMETA& out_image, XrTime& out_time)
    {
        XrFrameState           state      = { .type = XR_TYPE_FRAME_STATE };
        const XrFrameWaitInfo  wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
        const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
        EXPECT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
        EXPECT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
        out_time = state.predictedDisplayTime;
        out_image = { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META, .views = { { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META }, { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META } } };
        const XrEnvironmentDepthImageAcquireInfoMETA acquire = { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META, .space = space, .displayTime = out_time };
        return dx.xrAcquireEnvironmentDepthImageMETA(provider, &acquire, &out_image);
    }

    void end (XrTime time)
    {
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = time, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    }

    // Copies one layer of a D16 depth image, as the app would sample it
    void read_depth (VkImage image, u32 layer, u32 width, u32 height, Array<u16>& out_texels)
    {
        out_texels.resize((usize)width * height);
        const VkBufferCreateInfo buffer_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)width * height * 2, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        VkBuffer buffer = VK_NULL_HANDLE;
        ASSERT_EQ(vkCreateBuffer(vk_device, &buffer_info, nullptr, &buffer), VK_SUCCESS);
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(vk_device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(vk_physical, &memory);
        u32 type = 0;
        while (type < memory.memoryTypeCount && !((requirements.memoryTypeBits & (1u << type)) && (memory.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
        {
            type++;
        }
        const VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type };
        VkDeviceMemory device_memory = VK_NULL_HANDLE;
        ASSERT_EQ(vkAllocateMemory(vk_device, &alloc, nullptr, &device_memory), VK_SUCCESS);
        vkBindBufferMemory(vk_device, buffer, device_memory, 0);

        const VkCommandBufferAllocateInfo cmd_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(vk_device, &cmd_info, &cmd);
        const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cmd, &begin);
        VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2 } };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        const VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1 }, .imageExtent = { width, height, 1 } };
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(cmd);
        const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);

        void* mapped = nullptr;
        vkMapMemory(vk_device, device_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        memory_copy(out_texels.data, mapped, (usize)width * height * 2);
        vkUnmapMemory(vk_device, device_memory);
        vkFreeCommandBuffers(vk_device, pool, 1, &cmd);
        vkDestroyBuffer(vk_device, buffer, nullptr);
        vkFreeMemory(vk_device, device_memory, nullptr);
    }
};

} // namespace

TEST_F(OxrDepthTest, system_reports_depth_and_hand_removal)
{
    XrSystemEnvironmentDepthPropertiesMETA depth_props = { .type = XR_TYPE_SYSTEM_ENVIRONMENT_DEPTH_PROPERTIES_META };
    XrSystemProperties                     props       = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &depth_props };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_TRUE(depth_props.supportsEnvironmentDepth);
    EXPECT_TRUE(depth_props.supportsHandRemoval);
}

TEST_F(OxrDepthTest, providers_start_stopped_and_toggle_once)
{
    create_vulkan();
    create_vulkan_session();
    const XrEnvironmentDepthProviderCreateInfoMETA bad = { .type = XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META, .createFlags = 1 };
    EXPECT_EQ(dx.xrCreateEnvironmentDepthProviderMETA(session, &bad, &provider), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(dx.xrCreateEnvironmentDepthProviderMETA(session, nullptr, &provider), XR_ERROR_VALIDATION_FAILURE);
    const XrEnvironmentDepthProviderCreateInfoMETA info = { .type = XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META };
    ASSERT_EQ(dx.xrCreateEnvironmentDepthProviderMETA(session, &info, &provider), XR_SUCCESS);
    XrEnvironmentDepthProviderMETA second = XR_NULL_HANDLE;
    EXPECT_EQ(dx.xrCreateEnvironmentDepthProviderMETA(session, &info, &second), XR_ERROR_LIMIT_REACHED);

    EXPECT_EQ(dx.xrStopEnvironmentDepthProviderMETA(provider), XR_ERROR_UNEXPECTED_STATE_PASSTHROUGH_FB);
    EXPECT_EQ(dx.xrStartEnvironmentDepthProviderMETA(provider), XR_SUCCESS);
    EXPECT_EQ(dx.xrStartEnvironmentDepthProviderMETA(provider), XR_ERROR_UNEXPECTED_STATE_PASSTHROUGH_FB);
    EXPECT_EQ(dx.xrStopEnvironmentDepthProviderMETA(provider), XR_SUCCESS);

    const XrEnvironmentDepthHandRemovalSetInfoMETA removal = { .type = XR_TYPE_ENVIRONMENT_DEPTH_HAND_REMOVAL_SET_INFO_META, .enabled = XR_TRUE };
    EXPECT_EQ(dx.xrSetEnvironmentDepthHandRemovalMETA(provider, &removal), XR_SUCCESS);
    EXPECT_EQ(dx.xrSetEnvironmentDepthHandRemovalMETA(provider, nullptr), XR_ERROR_VALIDATION_FAILURE);

    EXPECT_EQ(dx.xrDestroyEnvironmentDepthProviderMETA(provider), XR_SUCCESS);
    EXPECT_EQ(dx.xrStartEnvironmentDepthProviderMETA(provider), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(dx.xrCreateEnvironmentDepthProviderMETA(session, &info, &provider), XR_SUCCESS);
}

TEST_F(OxrDepthTest, swapchain_has_d16_array_images)
{
    start();
    XrEnvironmentDepthSwapchainMETA second = XR_NULL_HANDLE;
    const XrEnvironmentDepthSwapchainCreateInfoMETA info = { .type = XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META };
    EXPECT_EQ(dx.xrCreateEnvironmentDepthSwapchainMETA(provider, &info, &second), XR_ERROR_LIMIT_REACHED);

    XrEnvironmentDepthSwapchainStateMETA state = { .type = XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META };
    ASSERT_EQ(dx.xrGetEnvironmentDepthSwapchainStateMETA(depth, &state), XR_SUCCESS);
    EXPECT_GT(state.width, 0u);
    EXPECT_GT(state.height, 0u);

    u32 count = 0;
    ASSERT_EQ(dx.xrEnumerateEnvironmentDepthSwapchainImagesMETA(depth, 0, &count, nullptr), XR_SUCCESS);
    ASSERT_GE(count, 2u);
    XrSwapchainImageVulkan2KHR images[8] = {};
    for (XrSwapchainImageVulkan2KHR& image : images)
    {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    }
    ASSERT_EQ(dx.xrEnumerateEnvironmentDepthSwapchainImagesMETA(depth, count, &count, (XrSwapchainImageBaseHeader*)images), XR_SUCCESS);
    EXPECT_NE(images[0].image, VK_NULL_HANDLE);
    images[0].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
    EXPECT_EQ(dx.xrEnumerateEnvironmentDepthSwapchainImagesMETA(depth, count, &count, (XrSwapchainImageBaseHeader*)images), XR_ERROR_VALIDATION_FAILURE);

    EXPECT_EQ(dx.xrDestroyEnvironmentDepthSwapchainMETA(depth), XR_SUCCESS);
    EXPECT_EQ(dx.xrGetEnvironmentDepthSwapchainStateMETA(depth, &state), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(dx.xrCreateEnvironmentDepthSwapchainMETA(provider, &info, &depth), XR_SUCCESS);
}

TEST_F(OxrDepthTest, acquire_follows_the_frame_and_provider_state)
{
    start();
    const XrSpace               stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    XrEnvironmentDepthImageMETA image = { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META };
    XrTime                      time  = 0;

    const XrEnvironmentDepthImageAcquireInfoMETA outside = { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META, .space = stage, .displayTime = 1 };
    EXPECT_EQ(dx.xrAcquireEnvironmentDepthImageMETA(provider, &outside, &image), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(begin_and_acquire(stage, image, time), XR_ERROR_CALL_ORDER_INVALID); // stopped
    end(time);

    ASSERT_EQ(dx.xrStartEnvironmentDepthProviderMETA(provider), XR_SUCCESS);
    EXPECT_EQ(begin_and_acquire(stage, image, time), XR_ENVIRONMENT_DEPTH_NOT_AVAILABLE_META);
    end(time);
    ASSERT_EQ(begin_and_acquire(stage, image, time), XR_SUCCESS);
    const XrEnvironmentDepthImageAcquireInfoMETA again = { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META, .space = stage, .displayTime = time };
    EXPECT_EQ(dx.xrAcquireEnvironmentDepthImageMETA(provider, &again, &image), XR_ERROR_LIMIT_REACHED);
    end(time);

    const XrEnvironmentDepthImageAcquireInfoMETA bad_time = { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META, .space = stage, .displayTime = 0 };
    EXPECT_EQ(begin_and_acquire(XR_NULL_HANDLE, image, time), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(dx.xrAcquireEnvironmentDepthImageMETA(provider, &bad_time, &image), XR_ERROR_TIME_INVALID);
    EXPECT_EQ(dx.xrAcquireEnvironmentDepthImageMETA(provider, &again, nullptr), XR_ERROR_VALIDATION_FAILURE);
    end(time);
}

// XR_KHR_vulkan_enable lets the runtime use the app's queue only in xrBeginFrame, xrEndFrame and
// the swapchain image calls. Unreal's OVRPlugin acquires depth on one thread while another submits,
// and the simultaneous submits lost the device.
TEST_F(OxrDepthTest, acquire_leaves_the_apps_queue_alone)
{
    start();
    ASSERT_EQ(dx.xrStartEnvironmentDepthProviderMETA(provider), XR_SUCCESS);
    const XrSpace               stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    XrEnvironmentDepthImageMETA image = {};
    XrTime                      time  = 0;
    EXPECT_EQ(begin_and_acquire(stage, image, time), XR_ENVIRONMENT_DEPTH_NOT_AVAILABLE_META);
    end(time);

    R_Device* device = nullptr;
    for (const OXR_Session& live : oxr_get_runtime()->sessions)
    {
        device = live.device;
    }
    ASSERT_NE(device, nullptr);
    XrFrameState           state      = { .type = XR_TYPE_FRAME_STATE };
    const XrFrameWaitInfo  wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
    const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
    const u64 before = r_queue_access_count(device);
    image = { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META, .views = { { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META }, { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META } } };
    const XrEnvironmentDepthImageAcquireInfoMETA acquire = { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META, .space = stage, .displayTime = state.predictedDisplayTime };
    EXPECT_EQ(dx.xrAcquireEnvironmentDepthImageMETA(provider, &acquire, &image), XR_SUCCESS);
    EXPECT_EQ(r_queue_access_count(device), before);
    end(state.predictedDisplayTime);
}

TEST_F(OxrDepthTest, depth_of_the_room_from_the_eyes)
{
    start();
    ASSERT_EQ(dx.xrStartEnvironmentDepthProviderMETA(provider), XR_SUCCESS);
    const XrSpace stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0.0f, 1.6f, 0.0f }, zm::QUAT_IDENTITY });

    XrEnvironmentDepthImageMETA image = {};
    XrTime                      time  = 0;
    EXPECT_EQ(begin_and_acquire(stage, image, time), XR_ENVIRONMENT_DEPTH_NOT_AVAILABLE_META);
    end(time);
    ASSERT_EQ(begin_and_acquire(stage, image, time), XR_SUCCESS);

    EXPECT_FLOAT_EQ(image.nearZ, 0.1f);
    EXPECT_FALSE(zm::is_finite(image.farZ));
    EXPECT_NEAR(image.views[0].pose.position.y, 1.6f, 1e-4f);
    EXPECT_LT(image.views[0].pose.position.x, image.views[1].pose.position.x);
    EXPECT_LT(image.views[0].fov.angleLeft, 0.0f);

    XrEnvironmentDepthSwapchainStateMETA state = { .type = XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META };
    ASSERT_EQ(dx.xrGetEnvironmentDepthSwapchainStateMETA(depth, &state), XR_SUCCESS);
    u32                        count      = 0;
    XrSwapchainImageVulkan2KHR images[8] = {};
    for (XrSwapchainImageVulkan2KHR& entry : images)
    {
        entry.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    }
    ASSERT_EQ(dx.xrEnumerateEnvironmentDepthSwapchainImagesMETA(depth, 8, &count, (XrSwapchainImageBaseHeader*)images), XR_SUCCESS);
    ASSERT_LT(image.swapchainIndex, count);

    // Infinite projection: depth = 1 - near / distance. The image's last rows look up, like
    // the projections apps build from nearZ, farZ and fov with y up.
    Array<u16> texels = {};
    defer { texels.release(); };
    read_depth(images[image.swapchainIndex].image, 0, state.width, state.height, texels);
    const u32 w = state.width;
    const u32 h = state.height;
    const f32 wall    = 1.0f - 0.1f / 1.5f;
    const f32 ceiling = 1.0f - 0.1f / 1.0f; // looking up at tan 0.9 the ceiling is 1 m ahead
    EXPECT_NEAR(texels[(h / 2) * w + w / 2] / 65535.0f, wall, 2e-3f);
    const u32 up_row = (u32)((0.9f * 0.5f + 0.5f) * (f32)h);
    EXPECT_NEAR(texels[up_row * w + w / 2] / 65535.0f, ceiling, 4e-3f);
    const u32 down_row = h - 1 - up_row;
    EXPECT_NEAR(texels[down_row * w + w / 2] / 65535.0f, wall, 2e-3f);
    end(time);

    // Without a room everything is infinitely far, from the image the next xrEndFrame renders
    ASSERT_EQ(oxr_set_room(nullptr), OXR_RoomResult::Ok);
    ASSERT_EQ(begin_and_acquire(stage, image, time), XR_SUCCESS);
    end(time);
    ASSERT_EQ(begin_and_acquire(stage, image, time), XR_SUCCESS);
    read_depth(images[image.swapchainIndex].image, 1, state.width, state.height, texels);
    EXPECT_EQ(texels[(h / 2) * w + w / 2], 0xFFFF);
    end(time);
}

namespace
{

// The stage origin is 0.5 m along +Z in the room; the instance exists before a test body runs
struct OxrMovedStageDepthTest : OxrDepthTest
{
    OxrMovedStageDepthTest ()           { os_set_env("PUPPETXR_ORIGIN_OFFSET", "0,0,0.5"); }
    ~OxrMovedStageDepthTest () override { os_unset_env("PUPPETXR_ORIGIN_OFFSET"); }
};

} // namespace

TEST_F(OxrMovedStageDepthTest, depth_sees_the_room_where_it_lies_around_the_moved_stage)
{
    // The -Z wall is 2 m ahead of the stage origin, not 1.5
    start();
    ASSERT_EQ(dx.xrStartEnvironmentDepthProviderMETA(provider), XR_SUCCESS);
    const XrSpace stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0.0f, 1.6f, 0.0f }, zm::QUAT_IDENTITY });

    XrEnvironmentDepthImageMETA image = {};
    XrTime                      time  = 0;
    begin_and_acquire(stage, image, time);
    end(time);
    ASSERT_EQ(begin_and_acquire(stage, image, time), XR_SUCCESS);
    XrEnvironmentDepthSwapchainStateMETA state = { .type = XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META };
    ASSERT_EQ(dx.xrGetEnvironmentDepthSwapchainStateMETA(depth, &state), XR_SUCCESS);
    u32                        count      = 0;
    XrSwapchainImageVulkan2KHR images[8] = {};
    for (XrSwapchainImageVulkan2KHR& entry : images)
    {
        entry.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    }
    ASSERT_EQ(dx.xrEnumerateEnvironmentDepthSwapchainImagesMETA(depth, 8, &count, (XrSwapchainImageBaseHeader*)images), XR_SUCCESS);
    Array<u16> texels = {};
    defer { texels.release(); };
    read_depth(images[image.swapchainIndex].image, 0, state.width, state.height, texels);
    EXPECT_NEAR(texels[(state.height / 2) * state.width + state.width / 2] / 65535.0f, 1.0f - 0.1f / 2.0f, 2e-3f);
    end(time);
}

// The depth image is of the physical room around the head, so drift leaves it alone and only
// moves the views' poses in STAGE
TEST_F(OxrDepthTest, depth_stays_with_the_room_when_tracking_drifts)
{
    start();
    ASSERT_EQ(dx.xrStartEnvironmentDepthProviderMETA(provider), XR_SUCCESS);
    const XrSpace stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0.0f, 1.6f, 0.0f }, zm::QUAT_IDENTITY });
    ASSERT_TRUE(sim_set_tracking_drift(oxr_get_sim(), { { 0.0f, 0.0f, 0.5f }, zm::QUAT_IDENTITY }));

    XrEnvironmentDepthImageMETA image = {};
    XrTime                      time  = 0;
    begin_and_acquire(stage, image, time);
    end(time);
    ASSERT_EQ(begin_and_acquire(stage, image, time), XR_SUCCESS);
    EXPECT_NEAR(image.views[0].pose.position.z, -0.5f, 1e-4f);
    EXPECT_NEAR(image.views[0].pose.position.y, 1.6f, 1e-4f);
    XrEnvironmentDepthSwapchainStateMETA state = { .type = XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META };
    ASSERT_EQ(dx.xrGetEnvironmentDepthSwapchainStateMETA(depth, &state), XR_SUCCESS);
    u32                        count      = 0;
    XrSwapchainImageVulkan2KHR images[8] = {};
    for (XrSwapchainImageVulkan2KHR& entry : images)
    {
        entry.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    }
    ASSERT_EQ(dx.xrEnumerateEnvironmentDepthSwapchainImagesMETA(depth, 8, &count, (XrSwapchainImageBaseHeader*)images), XR_SUCCESS);
    Array<u16> texels = {};
    defer { texels.release(); };
    read_depth(images[image.swapchainIndex].image, 0, state.width, state.height, texels);
    EXPECT_NEAR(texels[(state.height / 2) * state.width + state.width / 2] / 65535.0f, 1.0f - 0.1f / 1.5f, 2e-3f);
    end(time);
}

//
// XR_METAX1_passthrough_camera_data
//

#define CAMERA_FUNCTIONS(X)                                    \
    X(xrEnumeratePassthroughCameraSourcePropertiesMETAX1)      \
    X(xrEnumeratePassthroughCameraSupportedResolutionsMETAX1)  \
    X(xrGetPassthroughCameraIntrinsicsMETAX1)                  \
    X(xrGetPassthroughCameraExtrinsicsMETAX1)                  \
    X(xrCreatePassthroughCameraProviderMETAX1)                 \
    X(xrDestroyPassthroughCameraProviderMETAX1)                \
    X(xrStartPassthroughCameraProviderMETAX1)                  \
    X(xrStopPassthroughCameraProviderMETAX1)                   \
    X(xrAcquirePassthroughCameraImageMETAX1)                   \
    X(xrGetPassthroughCameraImageDataMETAX1)                   \
    X(xrReleasePassthroughCameraImageMETAX1)

namespace
{

struct CameraXr
{
#define DECLARE(fn) PFN_##fn fn;
    CAMERA_FUNCTIONS(DECLARE)
#undef DECLARE
};

const XrStructureType CAMERA_INTRINSICS  = (XrStructureType)OXR_TYPE_PASSTHROUGH_CAMERA_INTRINSICS_METAX1;
const XrStructureType CAMERA_EXTRINSICS  = (XrStructureType)OXR_TYPE_PASSTHROUGH_CAMERA_EXTRINSICS_METAX1;
const XrStructureType CAMERA_CREATE_INFO = (XrStructureType)OXR_TYPE_PASSTHROUGH_CAMERA_PROVIDER_CREATE_INFO_METAX1;
const XrStructureType CAMERA_IMAGE_DATA  = (XrStructureType)OXR_TYPE_PASSTHROUGH_CAMERA_IMAGE_DATA_METAX1;

// The depth tests' room, seen by the left camera at its smallest resolution, as MR Utility Kit
// drives it: an image acquired, its timestamp converted back with XR_KHR_convert_timespec_time
struct OxrCameraTest : OxrDepthTest
{
    CameraXr                           cx              = {};
    PFN_xrConvertTimespecTimeToTimeKHR to_time         = nullptr;
    u64                                camera          = 0;
    u64                                camera_provider = 0;

    OxrCameraTest ()
    {
        extensions[extensions_count++] = "XR_METAX1_passthrough_camera_data";
        extensions[extensions_count++] = XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrDepthTest::SetUp();
        if (IsSkipped() || HasFatalFailure())
        {
            return;
        }
#define LOAD(fn) ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&cx.fn), XR_SUCCESS) << #fn;
        CAMERA_FUNCTIONS(LOAD)
#undef LOAD
        ASSERT_EQ(gipa(instance, "xrConvertTimespecTimeToTimeKHR", (PFN_xrVoidFunction*)&to_time), XR_SUCCESS);
    }

    void start_camera ()
    {
        create_vulkan();
        create_vulkan_session();
        begin_session();
        OXR_PassthroughCameraSourcePropertiesMETAX1 sources[2] = {};
        u32                                         count      = 0;
        ASSERT_EQ(cx.xrEnumeratePassthroughCameraSourcePropertiesMETAX1(session, 2, &count, sources), XR_SUCCESS);
        camera = sources[0].cameraId;
        const OXR_PassthroughCameraProviderCreateInfoMETAX1 info = { .type = CAMERA_CREATE_INFO, .cameraId = camera, .resolution = { 320, 240 }, .unknown = { 1, 2 } };
        ASSERT_EQ(cx.xrCreatePassthroughCameraProviderMETAX1(session, &info, &camera_provider), XR_SUCCESS);
        ASSERT_EQ(cx.xrStartPassthroughCameraProviderMETAX1(camera_provider), XR_SUCCESS);
    }

    XrTime frame ()
    {
        XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
        EXPECT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
        EXPECT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
        end(state.predictedDisplayTime);
        return state.predictedDisplayTime;
    }

    // Frames until an image newer than `after` is out; the display time of each in out_times
    u64 frames_until_image (u64 after, XrTime* out_times, u32& out_frames)
    {
        out_frames = 0;
        for (; out_frames < 8; out_frames++)
        {
            out_times[out_frames] = frame();
            u64 image = 0;
            EXPECT_EQ(cx.xrAcquirePassthroughCameraImageMETAX1(camera_provider, &image), XR_SUCCESS);
            EXPECT_EQ(cx.xrReleasePassthroughCameraImageMETAX1(camera_provider, image), XR_SUCCESS);
            if (image != after)
            {
                out_frames++;
                return image;
            }
        }
        return after;
    }

    XrTime capture_time (const OXR_PassthroughCameraImageDataMETAX1& data)
    {
        const timespec spec = { .tv_sec = (time_t)(data.timestamp / 1000000), .tv_nsec = (long)(data.timestamp % 1000000 * 1000) };
        XrTime         time = 0;
        EXPECT_EQ(to_time(instance, &spec, &time), XR_SUCCESS);
        return time;
    }
};

const u8* center_pixel (const OXR_PassthroughCameraImageDataMETAX1& data)
{
    return data.data + ((usize)120 * 320 + 160) * 4;
}

} // namespace

TEST_F(OxrCameraTest, cameras_are_listed_with_their_lenses)
{
    create_vulkan();
    create_vulkan_session();
    OXR_PassthroughCameraSourcePropertiesMETAX1 sources[2] = {};
    u32                                         count      = 0;
    ASSERT_EQ(cx.xrEnumeratePassthroughCameraSourcePropertiesMETAX1(session, 0, &count, nullptr), XR_SUCCESS);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(cx.xrEnumeratePassthroughCameraSourcePropertiesMETAX1(session, 1, &count, sources), XR_ERROR_SIZE_INSUFFICIENT);
    ASSERT_EQ(cx.xrEnumeratePassthroughCameraSourcePropertiesMETAX1(session, 2, &count, sources), XR_SUCCESS);
    EXPECT_EQ(sources[0].position, OXR_PASSTHROUGH_CAMERA_POSITION_LEFT_METAX1);
    EXPECT_EQ(sources[1].position, OXR_PASSTHROUGH_CAMERA_POSITION_RIGHT_METAX1);
    EXPECT_NE(sources[0].cameraId, sources[1].cameraId);

    XrExtent2Di resolutions[8] = {};
    ASSERT_EQ(cx.xrEnumeratePassthroughCameraSupportedResolutionsMETAX1(session, sources[0].cameraId, 8, &count, resolutions), XR_SUCCESS);
    ASSERT_EQ(count, 4u);
    EXPECT_EQ(resolutions[0].width, 320);
    EXPECT_EQ(resolutions[3].width, 1280);
    EXPECT_EQ(resolutions[3].height, 960);
    EXPECT_EQ(cx.xrEnumeratePassthroughCameraSupportedResolutionsMETAX1(session, 7, 8, &count, resolutions), XR_ERROR_VALIDATION_FAILURE);

    OXR_PassthroughCameraIntrinsicsMETAX1 intrinsics = { .type = CAMERA_INTRINSICS };
    ASSERT_EQ(cx.xrGetPassthroughCameraIntrinsicsMETAX1(session, sources[0].cameraId, &intrinsics), XR_SUCCESS);
    EXPECT_EQ(intrinsics.sensorResolution.width, 1280);
    EXPECT_FLOAT_EQ(intrinsics.principalPoint.x, 640.0f);
    EXPECT_GT(intrinsics.focalLength.x, 0.0f);
    intrinsics.type = XR_TYPE_UNKNOWN;
    EXPECT_EQ(cx.xrGetPassthroughCameraIntrinsicsMETAX1(session, sources[0].cameraId, &intrinsics), XR_ERROR_VALIDATION_FAILURE);

    // Both look ahead, Camera2's frame half turned around x from the sensor's; the left one is left
    OXR_PassthroughCameraExtrinsicsMETAX1 left  = { .type = CAMERA_EXTRINSICS };
    OXR_PassthroughCameraExtrinsicsMETAX1 right = { .type = CAMERA_EXTRINSICS };
    ASSERT_EQ(cx.xrGetPassthroughCameraExtrinsicsMETAX1(session, sources[0].cameraId, &left), XR_SUCCESS);
    ASSERT_EQ(cx.xrGetPassthroughCameraExtrinsicsMETAX1(session, sources[1].cameraId, &right), XR_SUCCESS);
    EXPECT_FLOAT_EQ(left.pose.orientation.x, 1.0f);
    EXPECT_FLOAT_EQ(left.pose.orientation.w, 0.0f);
    EXPECT_LT(left.pose.position.x, 0.0f);
    EXPECT_GT(right.pose.position.x, 0.0f);
    EXPECT_LT(left.pose.position.z, 0.0f);
    EXPECT_EQ(cx.xrGetPassthroughCameraExtrinsicsMETAX1(session, 7, &left), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrCameraTest, providers_take_a_listed_resolution_and_toggle_once)
{
    create_vulkan();
    create_vulkan_session();
    OXR_PassthroughCameraSourcePropertiesMETAX1 sources[2] = {};
    u32                                         count      = 0;
    ASSERT_EQ(cx.xrEnumeratePassthroughCameraSourcePropertiesMETAX1(session, 2, &count, sources), XR_SUCCESS);
    OXR_PassthroughCameraProviderCreateInfoMETAX1 info = { .type = CAMERA_CREATE_INFO, .cameraId = sources[1].cameraId, .resolution = { 321, 240 } };
    EXPECT_EQ(cx.xrCreatePassthroughCameraProviderMETAX1(session, &info, &camera_provider), XR_ERROR_VALIDATION_FAILURE);
    info.resolution = { 320, 240 };
    info.cameraId   = 7;
    EXPECT_EQ(cx.xrCreatePassthroughCameraProviderMETAX1(session, &info, &camera_provider), XR_ERROR_VALIDATION_FAILURE);
    info.cameraId = sources[1].cameraId;
    EXPECT_EQ(cx.xrCreatePassthroughCameraProviderMETAX1(session, nullptr, &camera_provider), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(cx.xrCreatePassthroughCameraProviderMETAX1(session, &info, &camera_provider), XR_SUCCESS);

    u64 image = 0;
    EXPECT_EQ(cx.xrAcquirePassthroughCameraImageMETAX1(camera_provider, &image), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(cx.xrStopPassthroughCameraProviderMETAX1(camera_provider), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(cx.xrStartPassthroughCameraProviderMETAX1(camera_provider), XR_SUCCESS);
    EXPECT_EQ(cx.xrStartPassthroughCameraProviderMETAX1(camera_provider), XR_ERROR_CALL_ORDER_INVALID);
    EXPECT_EQ(cx.xrAcquirePassthroughCameraImageMETAX1(camera_provider, nullptr), XR_ERROR_VALIDATION_FAILURE);

    // Before the first frame the image has no data, which the app releases
    ASSERT_EQ(cx.xrAcquirePassthroughCameraImageMETAX1(camera_provider, &image), XR_SUCCESS);
    OXR_PassthroughCameraImageDataMETAX1 data = { .type = CAMERA_IMAGE_DATA };
    ASSERT_EQ(cx.xrGetPassthroughCameraImageDataMETAX1(camera_provider, image, &data), XR_SUCCESS);
    EXPECT_EQ(data.dataLength, 0u);
    EXPECT_EQ(data.data, nullptr);
    EXPECT_EQ(cx.xrReleasePassthroughCameraImageMETAX1(camera_provider, image), XR_SUCCESS);
    EXPECT_EQ(cx.xrReleasePassthroughCameraImageMETAX1(camera_provider, 99), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(cx.xrGetPassthroughCameraImageDataMETAX1(camera_provider, 99, &data), XR_ERROR_HANDLE_INVALID);

    EXPECT_EQ(cx.xrStopPassthroughCameraProviderMETAX1(camera_provider), XR_SUCCESS);
    EXPECT_EQ(cx.xrDestroyPassthroughCameraProviderMETAX1(camera_provider), XR_SUCCESS);
    EXPECT_EQ(cx.xrStartPassthroughCameraProviderMETAX1(camera_provider), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(cx.xrDestroyPassthroughCameraProviderMETAX1(camera_provider), XR_ERROR_HANDLE_INVALID);
}

// Regression: MR Utility Kit places what an image shows with the head pose at the image's
// timestamp. Without the extension its timestamps were the wall clock, which puppetxr predicted up
// to 100 ms ahead, so a screen showing the camera swung around the head as it turned.
TEST_F(OxrCameraTest, an_image_shows_the_room_from_the_head_pose_at_its_timestamp)
{
    start_camera();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_device_pose(sim, SIM_Device::Head, { { 0.0f, 1.6f, 0.0f }, zm::QUAT_IDENTITY });
    XrTime times[8] = {};
    u32    frames   = 0;
    const u64 first = frames_until_image(0, times, frames);
    ASSERT_NE(first, 0u);
    const XrTime first_time = times[frames - 1];

    // Looking down at the table now; the held image still shows the wall ahead
    u64 image = 0;
    ASSERT_EQ(cx.xrAcquirePassthroughCameraImageMETAX1(camera_provider, &image), XR_SUCCESS);
    ASSERT_EQ(image, first);
    sim_set_device_pose(sim, SIM_Device::Head, { { 0.5f, 1.6f, 0.0f }, { -0.70710678f, 0.0f, 0.0f, 0.70710678f } });
    const u64 second = frames_until_image(first, times, frames);
    ASSERT_NE(second, first);
    const XrTime second_time = times[frames - 1];

    OXR_PassthroughCameraImageDataMETAX1 data = { .type = CAMERA_IMAGE_DATA };
    ASSERT_EQ(cx.xrGetPassthroughCameraImageDataMETAX1(camera_provider, image, &data), XR_SUCCESS);
    ASSERT_EQ(data.dataLength, 320u * 240u * 4u);
    const u8* wall = center_pixel(data);
    EXPECT_GT(wall[0], 200);
    EXPECT_LT(wall[2], 60);
    EXPECT_EQ(wall[3], 255);
    EXPECT_NEAR((f64)capture_time(data), (f64)first_time, 1000.0);
    EXPECT_EQ(cx.xrReleasePassthroughCameraImageMETAX1(camera_provider, image), XR_SUCCESS);

    ASSERT_EQ(cx.xrAcquirePassthroughCameraImageMETAX1(camera_provider, &image), XR_SUCCESS);
    ASSERT_EQ(image, second);
    ASSERT_EQ(cx.xrGetPassthroughCameraImageDataMETAX1(camera_provider, image, &data), XR_SUCCESS);
    const u8* table = center_pixel(data);
    EXPECT_LT(table[0], 60);
    EXPECT_GT(table[2], 200);
    const XrTime captured = capture_time(data);
    EXPECT_NEAR((f64)captured, (f64)second_time, 1000.0);

    // The head the app locates at that time is the one the image was taken from
    const XrSpace   view     = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    const XrSpace   stage    = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(view, stage, captured, &location), XR_SUCCESS);
    EXPECT_NEAR(location.pose.orientation.x, -0.70710678f, 1e-3f);
    EXPECT_NEAR(location.pose.position.x, 0.5f, 1e-3f);
    EXPECT_EQ(cx.xrReleasePassthroughCameraImageMETAX1(camera_provider, image), XR_SUCCESS);
}

namespace
{

// The functions are looked up after the filter is set, so they come wrapped
struct OxrTracedCameraTest : OxrCameraTest
{
    OxrTracedCameraTest ()
    {
        trace_lines.clear();
        os_set_log_callback(collect_trace, nullptr);
        os_set_env("PUPPETXR_TRACE", "*PassthroughCamera*");
    }

    ~OxrTracedCameraTest () override
    {
        os_unset_env("PUPPETXR_TRACE");
        os_set_log_callback(nullptr, nullptr);
    }
};

} // namespace

TEST_F(OxrTracedCameraTest, camera_calls_are_traced_with_their_structs)
{
    start_camera();
    XrTime    times[8] = {};
    u32       frames   = 0;
    const u64 image    = frames_until_image(0, times, frames);
    ASSERT_NE(image, 0u);
    u64 acquired = 0;
    ASSERT_EQ(cx.xrAcquirePassthroughCameraImageMETAX1(camera_provider, &acquired), XR_SUCCESS);
    OXR_PassthroughCameraImageDataMETAX1 data = { .type = CAMERA_IMAGE_DATA };
    ASSERT_EQ(cx.xrGetPassthroughCameraImageDataMETAX1(camera_provider, acquired, &data), XR_SUCCESS);
    OXR_PassthroughCameraIntrinsicsMETAX1 intrinsics = { .type = CAMERA_INTRINSICS };
    ASSERT_EQ(cx.xrGetPassthroughCameraIntrinsicsMETAX1(session, camera, &intrinsics), XR_SUCCESS);

    const char* sources = traced("xrEnumeratePassthroughCameraSourcePropertiesMETAX1(");
    ASSERT_NE(sources, nullptr);
    EXPECT_NE(cstr_find(sources, "(session=0x"), nullptr) << sources;
    EXPECT_NE(cstr_find(sources, "properties=[{cameraId=50, position=1}, {cameraId=51, position=2}]"), nullptr) << sources;
    const char* create = traced("xrCreatePassthroughCameraProviderMETAX1(");
    ASSERT_NE(create, nullptr);
    EXPECT_NE(cstr_find(create, "cameraId=50, resolution={320, 240}}) -> XR_SUCCESS {provider=0x"), nullptr) << create;
    const char* get_data = traced("xrGetPassthroughCameraImageDataMETAX1(");
    ASSERT_NE(get_data, nullptr);
    EXPECT_NE(cstr_find(get_data, "dataLength=307200"), nullptr) << get_data;
    const char* get_intrinsics = traced("xrGetPassthroughCameraIntrinsicsMETAX1(");
    ASSERT_NE(get_intrinsics, nullptr);
    EXPECT_NE(cstr_find(get_intrinsics, "cameraId=50"), nullptr) << get_intrinsics;
    EXPECT_NE(cstr_find(get_intrinsics, "sensorResolution={1280, 1280}"), nullptr) << get_intrinsics;
    EXPECT_EQ(cx.xrReleasePassthroughCameraImageMETAX1(camera_provider, acquired), XR_SUCCESS);
}

// The cameras run at 60 Hz as Quest 3's; at 30 Hz the image lagged visibly behind the passthrough
// around it. Over a second of 90 Hz frames that is 60 images, on frames one or two apart
TEST_F(OxrCameraTest, images_come_at_the_camera_rate_over_a_second)
{
    start_camera();
    u64    last   = 0;
    u32    images = 0;
    u32    gap    = 0;
    u32    widest = 0;
    XrTime first  = 0;
    XrTime time   = 0;
    while (first == 0 || time - first < 1000000000ll)
    {
        time   = frame();
        first  = first == 0 ? time : first;
        u64 image = 0;
        ASSERT_EQ(cx.xrAcquirePassthroughCameraImageMETAX1(camera_provider, &image), XR_SUCCESS);
        ASSERT_EQ(cx.xrReleasePassthroughCameraImageMETAX1(camera_provider, image), XR_SUCCESS);
        gap++;
        if (image != last)
        {
            images++;
            widest = gap > widest ? gap : widest;
            gap    = 0;
            last   = image;
        }
    }
    EXPECT_GE(images, 59u);
    EXPECT_LE(images, 61u);
    EXPECT_LE(widest, 2u);
}

// A held image is never written over
TEST_F(OxrCameraTest, images_come_at_the_camera_rate_and_held_ones_stay)
{
    start_camera();
    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0.0f, 1.6f, 0.0f }, zm::QUAT_IDENTITY });
    XrTime times[8] = {};
    u32    frames   = 0;
    const u64 first = frames_until_image(0, times, frames);
    u64       image = 0;
    ASSERT_EQ(cx.xrAcquirePassthroughCameraImageMETAX1(camera_provider, &image), XR_SUCCESS);
    OXR_PassthroughCameraImageDataMETAX1 held = { .type = CAMERA_IMAGE_DATA };
    ASSERT_EQ(cx.xrGetPassthroughCameraImageDataMETAX1(camera_provider, image, &held), XR_SUCCESS);
    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0.5f, 1.6f, 0.0f }, { -0.70710678f, 0.0f, 0.0f, 0.70710678f } });

    // Each image is a frame nearest the camera's schedule
    u64          latest         = first;
    XrTime       captured       = times[frames - 1];
    const XrTime frame_interval = sim_display_period(oxr_get_sim());
    for (u32 i = 0; i < 4; i++)
    {
        const u64 next = frames_until_image(latest, times, frames);
        ASSERT_NE(next, latest);
        ASSERT_GE(frames, 1u);
        const XrTime period         = times[frames - 1] - captured;
        // The schedule keeps the average: single gaps are a frame shorter or longer
        EXPECT_GE(period, OXR_CAMERA_FRAME_INTERVAL_NS - frame_interval);
        EXPECT_LE(period, OXR_CAMERA_FRAME_INTERVAL_NS + frame_interval);
        latest   = next;
        captured = times[frames - 1];
    }
    OXR_PassthroughCameraImageDataMETAX1 again = { .type = CAMERA_IMAGE_DATA };
    ASSERT_EQ(cx.xrGetPassthroughCameraImageDataMETAX1(camera_provider, image, &again), XR_SUCCESS);
    EXPECT_EQ(again.data, held.data);
    EXPECT_GT(center_pixel(again)[0], 200); // still the wall, not the table the head turned to
    EXPECT_EQ(cx.xrReleasePassthroughCameraImageMETAX1(camera_provider, image), XR_SUCCESS);
}

//
// XR_META_environment_raycast
//

namespace
{

struct OxrRaycastTest : OxrSceneTest
{
    PFN_xrCreateEnvironmentRaycasterAsyncMETA    create_async   = nullptr;
    PFN_xrCreateEnvironmentRaycasterCompleteMETA create_complete = nullptr;
    PFN_xrDestroyEnvironmentRaycasterMETA        destroy_raycaster = nullptr;
    PFN_xrPerformEnvironmentRaycastMETA          perform        = nullptr;

    void start_raycast ()
    {
        const char* exts[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_EXT_FUTURE_EXTENSION_NAME, XR_META_ENVIRONMENT_RAYCAST_EXTENSION_NAME };
        ASSERT_EQ(create_instance(exts, 3), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrPollFutureEXT", (PFN_xrVoidFunction*)&scene.xrPollFutureEXT), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrCancelFutureEXT", (PFN_xrVoidFunction*)&scene.xrCancelFutureEXT), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrCreateEnvironmentRaycasterAsyncMETA", (PFN_xrVoidFunction*)&create_async), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrCreateEnvironmentRaycasterCompleteMETA", (PFN_xrVoidFunction*)&create_complete), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrDestroyEnvironmentRaycasterMETA", (PFN_xrVoidFunction*)&destroy_raycaster), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrPerformEnvironmentRaycastMETA", (PFN_xrVoidFunction*)&perform), XR_SUCCESS);
        const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
        ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);
        create_session();
        begin_session();
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    }

    XrEnvironmentRaycasterMETA make_raycaster ()
    {
        const XrEnvironmentRaycasterCreateInfoMETA info   = { .type = XR_TYPE_ENVIRONMENT_RAYCASTER_CREATE_INFO_META };
        XrFutureEXT                                future = XR_NULL_FUTURE_EXT;
        EXPECT_EQ(create_async(session, &info, &future), XR_SUCCESS);
        XrEnvironmentRaycasterCreateCompletionMETA completion = { .type = XR_TYPE_ENVIRONMENT_RAYCASTER_CREATE_COMPLETION_META };
        EXPECT_EQ(create_complete(session, future, &completion), XR_SUCCESS);
        EXPECT_EQ(completion.futureResult, XR_SUCCESS);
        return completion.environmentRaycaster;
    }

    XrEnvironmentRaycastHitMETA cast (XrEnvironmentRaycasterMETA raycaster, XrSpace space, XrVector3f origin, XrVector3f direction, f32 max_distance = 0.0f)
    {
        const XrEnvironmentRaycastFilterDistanceMETA    distance = { .type = XR_TYPE_ENVIRONMENT_RAYCAST_FILTER_DISTANCE_META, .maxDistance = max_distance };
        const XrEnvironmentRaycastFilterBaseHeaderMETA* filters[1] = { (const XrEnvironmentRaycastFilterBaseHeaderMETA*)&distance };
        const XrEnvironmentRaycastHitGetInfoMETA        info = { .type = XR_TYPE_ENVIRONMENT_RAYCAST_HIT_GET_INFO_META, .baseSpace = space, .time = frame().predictedDisplayTime, .origin = origin, .direction = direction,
                                                                 .filterCount = max_distance > 0.0f ? 1u : 0u, .filters = filters };
        XrEnvironmentRaycastHitMETA hit = { .type = XR_TYPE_ENVIRONMENT_RAYCAST_HIT_META };
        EXPECT_EQ(perform(raycaster, &info, &hit), XR_SUCCESS);
        return hit;
    }
};

} // namespace

TEST_F(OxrRaycastTest, raycast_needs_futures)
{
    const char* exts[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_META_ENVIRONMENT_RAYCAST_EXTENSION_NAME };
    EXPECT_EQ(create_instance(exts, 2), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
}

TEST_F(OxrRaycastTest, system_reports_raycast_support)
{
    start_raycast();
    XrSystemEnvironmentRaycastPropertiesMETA raycast = { .type = XR_TYPE_SYSTEM_ENVIRONMENT_RAYCAST_PROPERTIES_META };
    XrSystemProperties                       props   = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &raycast };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_TRUE(raycast.supportsEnvironmentRaycast);
}

TEST_F(OxrRaycastTest, raycasters_are_created_through_a_future)
{
    start_raycast();
    const XrEnvironmentRaycasterCreateInfoMETA info   = { .type = XR_TYPE_ENVIRONMENT_RAYCASTER_CREATE_INFO_META };
    XrFutureEXT                                future = XR_NULL_FUTURE_EXT;
    ASSERT_EQ(create_async(session, &info, &future), XR_SUCCESS);
    EXPECT_NE((u64)(uintptr_t)future, 0u);
    const XrFuturePollInfoEXT poll   = { .type = XR_TYPE_FUTURE_POLL_INFO_EXT, .future = future };
    XrFuturePollResultEXT     result = { .type = XR_TYPE_FUTURE_POLL_RESULT_EXT };
    ASSERT_EQ(scene.xrPollFutureEXT(instance, &poll, &result), XR_SUCCESS);
    EXPECT_EQ(result.state, XR_FUTURE_STATE_READY_EXT);

    XrEnvironmentRaycasterCreateCompletionMETA completion = { .type = XR_TYPE_ENVIRONMENT_RAYCASTER_CREATE_COMPLETION_META };
    EXPECT_EQ(create_complete(session, future, nullptr), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(create_complete(session, future, &completion), XR_SUCCESS);
    EXPECT_EQ(completion.futureResult, XR_SUCCESS);
    EXPECT_NE(completion.environmentRaycaster, XR_NULL_HANDLE);
    EXPECT_EQ(create_complete(session, future, &completion), XR_ERROR_FUTURE_INVALID_EXT);
    EXPECT_EQ(scene.xrPollFutureEXT(instance, &poll, &result), XR_ERROR_FUTURE_INVALID_EXT);

    // A cancelled future takes its raycaster along
    ASSERT_EQ(create_async(session, nullptr, &future), XR_SUCCESS);
    const XrFutureCancelInfoEXT cancel = { .type = XR_TYPE_FUTURE_CANCEL_INFO_EXT, .future = future };
    EXPECT_EQ(scene.xrCancelFutureEXT(instance, &cancel), XR_SUCCESS);
    EXPECT_EQ(create_complete(session, future, &completion), XR_ERROR_FUTURE_INVALID_EXT);

    EXPECT_EQ(destroy_raycaster(completion.environmentRaycaster), XR_SUCCESS);
    EXPECT_EQ(destroy_raycaster(completion.environmentRaycaster), XR_ERROR_HANDLE_INVALID);
    const XrEnvironmentRaycasterCreateInfoMETA wrong = { .type = XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META };
    EXPECT_EQ(create_async(session, &wrong, &future), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_async(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrRaycastTest, rays_hit_the_global_mesh_in_view)
{
    start_raycast();
    const XrEnvironmentRaycasterMETA raycaster = make_raycaster();
    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0.0f, 1.6f, 0.0f }, zm::QUAT_IDENTITY });

    XrEnvironmentRaycastHitMETA hit = cast(raycaster, stage, { 0.0f, 1.6f, 0.0f }, { 0.0f, 0.0f, -1.0f });
    ASSERT_EQ(hit.status, XR_ENVIRONMENT_RAYCAST_HIT_STATUS_HIT_META);
    EXPECT_NEAR(hit.pose.position.z, -1.5f, 1e-4f);
    EXPECT_NEAR(hit.pose.position.y, 1.6f, 1e-4f);
    const Quat orientation = { hit.pose.orientation.x, hit.pose.orientation.y, hit.pose.orientation.z, hit.pose.orientation.w };
    EXPECT_TRUE(zm::near(zm::rotate(orientation, { 0, 0, 1 }), Vec3{ 0, 0, 1 }, 1e-4f));
    EXPECT_GT(zm::dot(zm::rotate(orientation, { 0, 1, 0 }), Vec3{ 0, 0, 1 }), -1e-4f);

    // The table below the head, seen when looking down
    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0.5f, 1.6f, 0.0f }, { -0.70710678f, 0.0f, 0.0f, 0.70710678f } });
    hit = cast(raycaster, stage, { 0.5f, 1.6f, 0.0f }, { 0.0f, -2.0f, 0.0f });
    ASSERT_EQ(hit.status, XR_ENVIRONMENT_RAYCAST_HIT_STATUS_HIT_META);
    EXPECT_NEAR(hit.pose.position.y, 0.75f, 1e-4f);
    const Quat up = { hit.pose.orientation.x, hit.pose.orientation.y, hit.pose.orientation.z, hit.pose.orientation.w };
    EXPECT_TRUE(zm::near(zm::rotate(up, { 0, 0, 1 }), Vec3{ 0, 1, 0 }, 1e-4f));

    EXPECT_EQ(cast(raycaster, stage, { 0.5f, 1.6f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 0.5f).status, XR_ENVIRONMENT_RAYCAST_HIT_STATUS_NO_HIT_META);

    // Behind the head the cameras see nothing
    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0.0f, 1.6f, 0.0f }, zm::QUAT_IDENTITY });
    EXPECT_EQ(cast(raycaster, stage, { 0.0f, 1.6f, 0.0f }, { 0.0f, 0.0f, 1.0f }).status, XR_ENVIRONMENT_RAYCAST_HIT_STATUS_HIT_POINT_OUTSIDE_OF_FOV_META);

    // Rays and hits are in the base space
    const XrSpace lifted = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE, { { 0, 0, 0, 1 }, { 0, 1, 0 } });
    hit = cast(raycaster, lifted, { 0.0f, 0.6f, 0.0f }, { 0.0f, 0.0f, -1.0f });
    ASSERT_EQ(hit.status, XR_ENVIRONMENT_RAYCAST_HIT_STATUS_HIT_META);
    EXPECT_NEAR(hit.pose.position.y, 0.6f, 1e-4f);
    EXPECT_NEAR(hit.pose.position.z, -1.5f, 1e-4f);

    ASSERT_EQ(oxr_set_room(nullptr), OXR_RoomResult::Ok);
    EXPECT_EQ(cast(raycaster, stage, { 0.0f, 1.6f, 0.0f }, { 0.0f, 0.0f, -1.0f }).status, XR_ENVIRONMENT_RAYCAST_HIT_STATUS_NO_HIT_META);
}

TEST_F(OxrRaycastTest, rays_hit_the_room_where_it_lies_around_the_moved_stage)
{
    // The stage origin is 1 m along +X in the room, so the +X wall is 1 m away, not 2
    os_set_env("PUPPETXR_ORIGIN_OFFSET", "0,1,0");
    defer { os_unset_env("PUPPETXR_ORIGIN_OFFSET"); };
    start_raycast();
    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0, 1.6f, 0 }, zm::quat_from_axis_angle({ 0, 1, 0 }, -zm::PI / 2) });
    const XrEnvironmentRaycasterMETA  raycaster = make_raycaster();
    const XrEnvironmentRaycastHitMETA hit       = cast(raycaster, stage, { 0, 1.6f, 0 }, { 1, 0, 0 });
    ASSERT_EQ(hit.status, XR_ENVIRONMENT_RAYCAST_HIT_STATUS_HIT_META);
    EXPECT_NEAR(hit.pose.position.x, 1.0f, 1e-4f);
    EXPECT_NEAR(hit.pose.position.y, 1.6f, 1e-4f);
}

// Drift moves STAGE 1 m along +X in the room, as the origin offset does, while the head stays
TEST_F(OxrRaycastTest, rays_hit_the_room_where_it_lies_around_the_drifted_stage)
{
    start_raycast();
    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0, 1.6f, 0 }, zm::quat_from_axis_angle({ 0, 1, 0 }, -zm::PI / 2) });
    ASSERT_TRUE(sim_set_tracking_drift(oxr_get_sim(), { { 1, 0, 0 }, zm::QUAT_IDENTITY }));
    const XrEnvironmentRaycasterMETA raycaster = make_raycaster();
    XrEnvironmentRaycastHitMETA      hit       = cast(raycaster, stage, { 0, 1.6f, 0 }, { 1, 0, 0 });
    ASSERT_EQ(hit.status, XR_ENVIRONMENT_RAYCAST_HIT_STATUS_HIT_META);
    EXPECT_NEAR(hit.pose.position.x, 1.0f, 1e-4f);
    EXPECT_NEAR(hit.pose.position.y, 1.6f, 1e-4f);

    // From the head the wall is as far as ever
    const XrSpace view = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    hit                = cast(raycaster, view, { 0, 0, 0 }, { 0, 0, -1 });
    ASSERT_EQ(hit.status, XR_ENVIRONMENT_RAYCAST_HIT_STATUS_HIT_META);
    EXPECT_NEAR(hit.pose.position.z, -2.0f, 1e-4f);
}

TEST_F(OxrRaycastTest, raycasts_validate_their_input)
{
    start_raycast();
    const XrEnvironmentRaycasterMETA raycaster = make_raycaster();
    const XrTime                     time      = frame().predictedDisplayTime;
    XrEnvironmentRaycastHitGetInfoMETA info = { .type = XR_TYPE_ENVIRONMENT_RAYCAST_HIT_GET_INFO_META, .baseSpace = stage, .time = time, .origin = { 0, 1.6f, 0 }, .direction = { 0, 0, -1 } };
    XrEnvironmentRaycastHitMETA        hit  = { .type = XR_TYPE_ENVIRONMENT_RAYCAST_HIT_META };
    EXPECT_EQ(perform(XR_NULL_HANDLE, &info, &hit), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(perform(raycaster, nullptr, &hit), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(perform(raycaster, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    info.time = 0;
    EXPECT_EQ(perform(raycaster, &info, &hit), XR_ERROR_TIME_INVALID);
    info.time      = time;
    info.direction = { 0, 0, 0 };
    EXPECT_EQ(perform(raycaster, &info, &hit), XR_ERROR_VALIDATION_FAILURE);
    info.direction = { 0, 0, -1 };
    info.baseSpace = XR_NULL_HANDLE;
    EXPECT_EQ(perform(raycaster, &info, &hit), XR_ERROR_HANDLE_INVALID);
    info.baseSpace = stage;
    const XrEnvironmentRaycastFilterDistanceMETA    negative   = { .type = XR_TYPE_ENVIRONMENT_RAYCAST_FILTER_DISTANCE_META, .maxDistance = -1.0f };
    const XrEnvironmentRaycastFilterBaseHeaderMETA* filters[1] = { (const XrEnvironmentRaycastFilterBaseHeaderMETA*)&negative };
    info.filterCount = 1;
    info.filters     = filters;
    EXPECT_EQ(perform(raycaster, &info, &hit), XR_ERROR_VALIDATION_FAILURE);
    info.filters = nullptr;
    EXPECT_EQ(perform(raycaster, &info, &hit), XR_ERROR_VALIDATION_FAILURE);

    ASSERT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
    session          = XR_NULL_HANDLE;
    info.filterCount = 0;
    EXPECT_EQ(perform(raycaster, &info, &hit), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrTest, headless_sessions_have_no_depth_swapchain)
{
    const char* exts[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME };
    ASSERT_EQ(create_instance(exts, 2), XR_SUCCESS);
    const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);
    create_session();
    PFN_xrCreateEnvironmentDepthProviderMETA  create_provider  = nullptr;
    PFN_xrCreateEnvironmentDepthSwapchainMETA create_swapchain = nullptr;
    ASSERT_EQ(gipa(instance, "xrCreateEnvironmentDepthProviderMETA", (PFN_xrVoidFunction*)&create_provider), XR_SUCCESS);
    ASSERT_EQ(gipa(instance, "xrCreateEnvironmentDepthSwapchainMETA", (PFN_xrVoidFunction*)&create_swapchain), XR_SUCCESS);
    const XrEnvironmentDepthProviderCreateInfoMETA  provider_info  = { .type = XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META };
    const XrEnvironmentDepthSwapchainCreateInfoMETA swapchain_info = { .type = XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META };
    XrEnvironmentDepthProviderMETA  provider = XR_NULL_HANDLE;
    XrEnvironmentDepthSwapchainMETA depth    = XR_NULL_HANDLE;
    ASSERT_EQ(create_provider(session, &provider_info, &provider), XR_SUCCESS);
    EXPECT_EQ(create_swapchain(provider, &swapchain_info, &depth), XR_ERROR_FEATURE_UNSUPPORTED);
}

namespace
{

struct OxrStarterRoomsTest : OxrVulkanTest
{
    void SetUp () override
    {
        os_set_env("PUPPETXR_ROOMS_DIR", PUPPETXR_SOURCE_ROOMS_DIR);
        OxrVulkanTest::SetUp();
    }

    void TearDown () override
    {
        OxrVulkanTest::TearDown();
        os_unset_env("PUPPETXR_ROOMS_DIR");
    }
};

} // namespace

// Writes <build>/room_<name>_<view>.png per starter room for inspection, seen from the stage origin
TEST_F(OxrStarterRoomsTest, starter_rooms_render_as_passthrough)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    char names[16][W_MAX_NAME] = {};
    const u32 count = w_list_rooms(PUPPETXR_SOURCE_ROOMS_DIR, names, 16);
    ASSERT_GE(count, 4u);
    Array<u8> pixels = {};
    defer { pixels.release(); };
    for (u32 i = 0; i < count; i++)
    {
        ASSERT_EQ(oxr_set_room(names[i]), OXR_RoomResult::Ok) << names[i];
        for (u32 yaw = 0; yaw < 4; yaw++)
        {
            sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0.0f, 1.6f, 0.0f }, zm::quat_from_axis_angle({ 0, 1, 0 }, (f32)yaw * zm::PI * 0.5f) });
            XrFrameState           state      = { .type = XR_TYPE_FRAME_STATE };
            const XrFrameWaitInfo  wait_info  = { .type = XR_TYPE_FRAME_WAIT_INFO };
            const XrFrameBeginInfo begin_info = { .type = XR_TYPE_FRAME_BEGIN_INFO };
            ASSERT_EQ(xr.xrWaitFrame(session, &wait_info, &state), XR_SUCCESS);
            ASSERT_EQ(xr.xrBeginFrame(session, &begin_info), XR_SUCCESS);
            oxr_set_mirror_view(SIM_MirrorView::Left);
            const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND };
            ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
            char path[OS_MAX_PATH];
            cstr_format(path, sizeof(path), "%s/room_%s_%u.png", PUPPETXR_TEST_OUTPUT_DIR, names[i], yaw);
            EXPECT_TRUE(oxr_save_screenshot(path));
            u32 width = 0;
            u32 height = 0;
            u8  probe[4];
            oxr_read_frame(probe, sizeof(probe), width, height);
            pixels.resize((usize)width * height * 4);
            ASSERT_TRUE(oxr_read_frame(pixels.data, pixels.length, width, height));
            const u8* center = pixels.data + ((usize)(height / 2) * width + width / 2) * 4;
            EXPECT_GT(center[0] + center[1] + center[2], 30) << names[i] << " yaw " << yaw;
        }
    }
}

//
// XR_EXT_conformance_automation
//

namespace
{

struct OxrAutomationTest : OxrActionTest
{
    PFN_xrSetInputDeviceActiveEXT        set_active   = nullptr;
    PFN_xrSetInputDeviceStateBoolEXT     set_bool     = nullptr;
    PFN_xrSetInputDeviceStateFloatEXT    set_float    = nullptr;
    PFN_xrSetInputDeviceStateVector2fEXT set_vector   = nullptr;
    PFN_xrSetInputDeviceLocationEXT      set_location = nullptr;

    OxrAutomationTest ()
    {
        extension = XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrActionTest::SetUp();
        ASSERT_EQ(gipa(instance, "xrSetInputDeviceActiveEXT", (PFN_xrVoidFunction*)&set_active), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrSetInputDeviceStateBoolEXT", (PFN_xrVoidFunction*)&set_bool), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrSetInputDeviceStateFloatEXT", (PFN_xrVoidFunction*)&set_float), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrSetInputDeviceStateVector2fEXT", (PFN_xrVoidFunction*)&set_vector), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrSetInputDeviceLocationEXT", (PFN_xrVoidFunction*)&set_location), XR_SUCCESS);
    }
};

} // namespace

TEST_F(OxrActionTest, conformance_automation_needs_its_extension)
{
    PFN_xrVoidFunction function = nullptr;
    EXPECT_EQ(gipa(instance, "xrSetInputDeviceActiveEXT", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(function, nullptr);
}

TEST_F(OxrAutomationTest, inputs_are_driven_and_take_over_from_the_keyboard)
{
    suggest_touch_bindings();
    attach();
    begin_session();
    const SIM_Sim* sim = oxr_get_sim();
    EXPECT_TRUE(sim_controls_enabled(sim));

    const XrPath touch = path("/interaction_profiles/oculus/touch_controller");
    ASSERT_EQ(set_active(session, touch, hands[0], XR_TRUE), XR_SUCCESS);
    EXPECT_FALSE(sim_controls_enabled(sim));
    ASSERT_EQ(set_bool(session, hands[0], path("/user/hand/left/input/x/click"), XR_TRUE), XR_SUCCESS);
    ASSERT_EQ(set_float(session, hands[1], path("/user/hand/right/input/trigger/value"), 0.75f), XR_SUCCESS);
    ASSERT_EQ(set_vector(session, hands[0], path("/user/hand/left/input/thumbstick"), { 0.5f, -0.25f }), XR_SUCCESS);
    ASSERT_EQ(set_float(session, hands[1], path("/user/hand/right/input/thumbstick/y"), 0.4f), XR_SUCCESS);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);

    EXPECT_TRUE(bool_state(click, hands[0]).currentState);
    EXPECT_FLOAT_EQ(float_state(trigger, hands[1]).currentState, 0.75f);
    const XrActionStateGetInfo  stick_info  = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = stick, .subactionPath = hands[0] };
    XrActionStateVector2f       stick_state = { .type = XR_TYPE_ACTION_STATE_VECTOR2F };
    ASSERT_EQ(xr.xrGetActionStateVector2f(session, &stick_info, &stick_state), XR_SUCCESS);
    EXPECT_FLOAT_EQ(stick_state.currentState.x, 0.5f);
    EXPECT_FLOAT_EQ(stick_state.currentState.y, -0.25f);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::Thumbstick).value.y, 0.4f);

    // A bool on a float input, as the simple profile's select is on Touch
    ASSERT_EQ(set_bool(session, hands[1], path("/user/hand/right/input/trigger/value"), XR_FALSE), XR_SUCCESS);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_FLOAT_EQ(float_state(trigger, hands[1]).currentState, 0.0f);

    ASSERT_EQ(set_active(session, touch, hands[0], XR_FALSE), XR_SUCCESS);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_FALSE(bool_state(click, hands[0]).isActive);
    EXPECT_TRUE(float_state(trigger, hands[1]).isActive);
}

TEST_F(OxrAutomationTest, activating_a_profile_switches_to_its_hardware)
{
    const XrActionSuggestedBinding             bindings[] = { { click, path("/user/hand/left/input/select/click") }, { click, path("/user/hand/right/input/select/click") } };
    const XrInteractionProfileSuggestedBinding simple     = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/khr/simple_controller"), .countSuggestedBindings = 2, .suggestedBindings = bindings };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &simple), XR_SUCCESS);
    attach();
    begin_session();
    const SIM_Sim* sim = oxr_get_sim();

    ASSERT_EQ(set_active(session, path("/interaction_profiles/khr/simple_controller"), hands[1], XR_TRUE), XR_SUCCESS);
    EXPECT_EQ(sim_get_controller(sim), SIM_Controller::KhrSimple);
    ASSERT_EQ(set_bool(session, hands[1], path("/user/hand/right/input/select/click"), XR_TRUE), XR_SUCCESS);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(bool_state(click, hands[1]).currentState);
    EXPECT_FALSE(bool_state(click, hands[0]).currentState);
}

TEST_F(OxrAutomationTest, locations_place_the_named_pose)
{
    suggest_touch_bindings();
    attach();
    begin_session();
    EXPECT_EQ(sync(), XR_SUCCESS);
    const SIM_Sim* sim = oxr_get_sim();

    const XrSpace local = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    const XrSpace stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrPosef target = { { 0.0f, 0.38268343f, 0.0f, 0.92387953f }, { 0.3f, -0.2f, -0.5f } };
    ASSERT_EQ(set_location(session, hands[1], path("/user/hand/right/input/aim/pose"), local, target), XR_SUCCESS);

    // The grip is placed so that the aim pose lands on the target
    const Pose aim      = sim_get_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Aim);
    const Pose expected = zm::pose_mul(sim_get_reference_space_pose(sim, SIM_ReferenceSpace::Local), { .position = { 0.3f, -0.2f, -0.5f }, .orientation = { 0.0f, 0.38268343f, 0.0f, 0.92387953f } });
    EXPECT_NEAR(aim.position.x, expected.position.x, 1e-4f);
    EXPECT_NEAR(aim.position.y, expected.position.y, 1e-4f);
    EXPECT_NEAR(aim.position.z, expected.position.z, 1e-4f);
    EXPECT_NEAR(zm::abs(zm::dot(aim.orientation, expected.orientation)), 1.0f, 1e-4f);

    ASSERT_EQ(set_location(session, hands[0], path("/user/hand/left/input/grip/pose"), stage, { { 0, 0, 0, 1 }, { -0.1f, 1.2f, -0.4f } }), XR_SUCCESS);
    EXPECT_NEAR(sim_get_device_pose(sim, SIM_Device::LeftHand).position.y, 1.2f, 1e-5f);
}

TEST_F(OxrAutomationTest, invalid_arguments_are_rejected)
{
    suggest_touch_bindings();
    attach();
    const XrPath  touch = path("/interaction_profiles/oculus/touch_controller");
    const XrSpace stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrPath  click_path = path("/user/hand/left/input/x/click");

    EXPECT_EQ(set_active((XrSession)(uintptr_t)0x77, touch, hands[0], XR_TRUE), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(set_active(session, touch, (XrPath)0x7777, XR_TRUE), XR_ERROR_PATH_INVALID);
    EXPECT_EQ(set_active(session, touch, path("/user/head"), XR_TRUE), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(set_active(session, path("/interaction_profiles/valve/index_controller"), hands[0], XR_TRUE), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(set_active(session, path("/user/hand/left"), hands[0], XR_TRUE), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(set_bool(session, hands[1], click_path, XR_TRUE), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(set_bool(session, hands[0], path("/user/hand/left/input/nothing/click"), XR_TRUE), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(set_bool(session, hands[0], path("/user/hand/left/input/grip/pose"), XR_TRUE), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(set_float(session, hands[0], click_path, INFINITY), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(set_vector(session, hands[0], path("/user/hand/left/input/thumbstick"), { zm::nan(), 0.0f }), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(set_location(session, hands[0], path("/user/hand/left/input/grip/pose"), stage, { { 0, 0, 0, 0 }, { 0, 0, 0 } }), XR_ERROR_POSE_INVALID);
    EXPECT_EQ(set_location(session, hands[0], path("/user/hand/left/input/grip/pose"), (XrSpace)(uintptr_t)0x77, { { 0, 0, 0, 1 }, { 0, 0, 0 } }), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(set_location(session, hands[0], click_path, stage, { { 0, 0, 0, 1 }, { 0, 0, 0 } }), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_TRUE(sim_controls_enabled(oxr_get_sim()));
}

// Regression (found by fuzzing): poses and haptics have no input value, and reading one asserted
TEST_F(OxrAutomationTest, values_on_poses_and_haptics_are_rejected_without_asserting)
{
    suggest_touch_bindings();
    attach();
    const u32 asserts = os_assert_count();
    const XrPath grip_pose   = path("/user/hand/right/input/grip/pose");
    const XrPath haptic_path = path("/user/hand/right/output/haptic");
    EXPECT_EQ(set_bool(session, hands[1], grip_pose, XR_TRUE), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(set_float(session, hands[1], grip_pose, 0.5f), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(set_vector(session, hands[1], grip_pose, { 0.5f, 0.5f }), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(set_bool(session, hands[1], haptic_path, XR_TRUE), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(set_float(session, hands[1], haptic_path, 0.5f), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(set_vector(session, hands[1], haptic_path, { 0.5f, 0.5f }), XR_ERROR_PATH_UNSUPPORTED);
    EXPECT_EQ(os_assert_count(), asserts);
}

//
// XR_KHR_convert_timespec_time
//

namespace
{
struct OxrTimespecTest : OxrTest
{
    PFN_xrConvertTimespecTimeToTimeKHR to_time     = nullptr;
    PFN_xrConvertTimeToTimespecTimeKHR to_timespec = nullptr;
    b8                                 realtime    = false; // the clock follows the wall clock, as apps and the CTS run it

    OxrTimespecTest ()
    {
        extension = XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrTest::SetUp();
        if (realtime)
        {
            os_unset_env("PUPPETXR_FIXED_STEP");
        }
        create_headless_instance();
        ASSERT_EQ(gipa(instance, "xrConvertTimespecTimeToTimeKHR", (PFN_xrVoidFunction*)&to_time), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrConvertTimeToTimespecTimeKHR", (PFN_xrVoidFunction*)&to_timespec), XR_SUCCESS);
    }

    // The clock apps read timespecs from (Platform.timespec_clock_is_the_one_apps_read)
    static timespec timespec_now ()
    {
        const u64 ns = os_timespec_ns();
        return { .tv_sec = (time_t)(ns / 1000000000ull), .tv_nsec = (long)(ns % 1000000000ull) };
    }

    XrTime now ()
    {
        const timespec monotonic = timespec_now();
        XrTime         time      = 0;
        EXPECT_EQ(to_time(instance, &monotonic, &time), XR_SUCCESS);
        return time;
    }
};
} // namespace

TEST_F(OxrTest, timespec_conversion_needs_its_extension)
{
    create_headless_instance();
    PFN_xrVoidFunction function = nullptr;
    EXPECT_EQ(gipa(instance, "xrConvertTimespecTimeToTimeKHR", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
}

TEST_F(OxrTimespecTest, round_trips_exactly)
{
    const timespec monotonic = timespec_now();
    XrTime         first     = 0;
    ASSERT_EQ(to_time(instance, &monotonic, &first), XR_SUCCESS);
    EXPECT_GT(first, 0);
    timespec back = {};
    ASSERT_EQ(to_timespec(instance, first, &back), XR_SUCCESS);
    EXPECT_EQ(back.tv_sec, monotonic.tv_sec);
    EXPECT_EQ(back.tv_nsec, monotonic.tv_nsec);
    XrTime second = 0;
    ASSERT_EQ(to_time(instance, &back, &second), XR_SUCCESS);
    EXPECT_EQ(second, first);
}

TEST_F(OxrTimespecTest, invalid_arguments_are_rejected)
{
    timespec monotonic = {};
    XrTime   time      = 0;
    EXPECT_EQ(to_timespec(instance, 0, &monotonic), XR_ERROR_TIME_INVALID);
    EXPECT_EQ(to_timespec(instance, -1, &monotonic), XR_ERROR_TIME_INVALID);
    EXPECT_EQ(to_timespec(instance, 1000, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(to_time(instance, nullptr, &time), XR_ERROR_VALIDATION_FAILURE);
    monotonic = { .tv_sec = 1, .tv_nsec = 1000000000 };
    EXPECT_EQ(to_time(instance, &monotonic, &time), XR_ERROR_VALIDATION_FAILURE);
    monotonic = { .tv_sec = 0, .tv_nsec = 0 };
    EXPECT_EQ(to_time(instance, &monotonic, &time), XR_ERROR_TIME_INVALID); // long before the simulated epoch
    EXPECT_EQ(to_time((XrInstance)(uintptr_t)0x77, &monotonic, &time), XR_ERROR_HANDLE_INVALID);
}

// Regression (found by fuzzing): times past the range of the other clock overflowed int64 arithmetic,
// undefined behaviour that UBSan reports in clang-asan
TEST_F(OxrTimespecTest, times_out_of_range_are_rejected)
{
    timespec monotonic = {};
    XrTime   time      = 0;
    EXPECT_EQ(to_timespec(instance, INT64_MAX, &monotonic), XR_ERROR_TIME_INVALID);
    EXPECT_EQ(to_timespec(instance, INT64_MAX - 1000000000, &monotonic), XR_ERROR_TIME_INVALID);
    monotonic = { .tv_sec = INT64_MAX, .tv_nsec = 0 };
    EXPECT_EQ(to_time(instance, &monotonic, &time), XR_ERROR_TIME_INVALID);
    monotonic = { .tv_sec = INT64_MAX / 1000000000, .tv_nsec = 999999999 };
    EXPECT_EQ(to_time(instance, &monotonic, &time), XR_ERROR_TIME_INVALID);
    monotonic = { .tv_sec = 18446744074, .tv_nsec = 0 };
    EXPECT_EQ(to_time(instance, &monotonic, &time), XR_ERROR_TIME_INVALID);
}

// CTS XR_KHR_convert_timespec_time "Matches frame timing": frames are predicted after now
TEST_F(OxrTimespecTest, predicted_display_times_lie_after_now)
{
    create_session();
    const XrTime before = now();
    begin_session();
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    for (u32 i = 0; i < 3; i++)
    {
        ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
        ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    }
    EXPECT_GE(state.predictedDisplayTime, before);
    EXPECT_GT(now(), before);
    EXPECT_GE(state.predictedDisplayTime, now());
}

namespace
{
struct OxrRealtimeTimespecTest : OxrTimespecTest
{
    OxrRealtimeTimespecTest ()
    {
        realtime = true;
    }
};
} // namespace

// Regression (CTS "Matches frame timing" on OpenGL, whose session creation compiles shaders): the
// realtime clock counted its first frame from that frame, so time spent between xrCreateInstance
// and the first xrWaitFrame was missing and the first predicted display time lay before a now
// converted in between
TEST_F(OxrRealtimeTimespecTest, first_frame_after_a_slow_start_is_predicted_after_now)
{
    os_sleep_ns(50000000);
    const XrTime before = now();
    create_session();
    begin_session();
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    EXPECT_GE(state.predictedDisplayTime, before);
    ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
}

//
// XR_KHR_win32_convert_performance_counter_time
//

// The counter is a LARGE_INTEGER, one 64-bit count; the functions are called through their exact
// type, which UBSan's function check compares
union _LARGE_INTEGER;

namespace
{
typedef XrResult (XRAPI_PTR* PFN_ToTime)    (XrInstance instance, const _LARGE_INTEGER* counter, XrTime* time);
typedef XrResult (XRAPI_PTR* PFN_ToCounter) (XrInstance instance, XrTime time, _LARGE_INTEGER* counter);

struct OxrPerformanceCounterTest : OxrTest
{
    PFN_ToTime    to_time_fn    = nullptr;
    PFN_ToCounter to_counter_fn = nullptr;

    XrResult to_time (XrInstance inst, const i64* counter, XrTime* time) const
    {
        return to_time_fn(inst, (const _LARGE_INTEGER*)counter, time);
    }

    XrResult to_counter (XrInstance inst, XrTime time, i64* counter) const
    {
        return to_counter_fn(inst, time, (_LARGE_INTEGER*)counter);
    }

    OxrPerformanceCounterTest ()
    {
        extension = "XR_KHR_win32_convert_performance_counter_time";
    }

    void SetUp () override
    {
        OxrTest::SetUp();
        if (os_native_clock() != OS_NativeClock::PerformanceCounter)
        {
            GTEST_SKIP() << "the platform's clock is no performance counter";
        }
        create_headless_instance();
        ASSERT_EQ(gipa(instance, "xrConvertWin32PerformanceCounterToTimeKHR", (PFN_xrVoidFunction*)&to_time_fn), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrConvertTimeToWin32PerformanceCounterKHR", (PFN_xrVoidFunction*)&to_counter_fn), XR_SUCCESS);
    }

    XrTime now ()
    {
        const i64 counter = os_ns_to_native_ticks(os_monotonic_ns());
        XrTime    time    = 0;
        EXPECT_EQ(to_time(instance, &counter, &time), XR_SUCCESS);
        return time;
    }
};
} // namespace

TEST_F(OxrTest, performance_counter_conversion_needs_its_extension)
{
    create_headless_instance();
    PFN_xrVoidFunction function = nullptr;
    EXPECT_EQ(gipa(instance, "xrConvertWin32PerformanceCounterToTimeKHR", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
}

// Timespecs everywhere: MR Utility Kit converts passthrough camera timestamps with them on Windows too
TEST_F(OxrTest, timespecs_and_the_platforms_clock_are_offered)
{
    u32 count = 0;
    ASSERT_EQ(xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), XR_SUCCESS);
    XrExtensionProperties properties[128];
    for (XrExtensionProperties& property : properties)
    {
        property = { .type = XR_TYPE_EXTENSION_PROPERTIES };
    }
    ASSERT_LE(count, ARRAY_SIZE(properties));
    ASSERT_EQ(xr.xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties), XR_SUCCESS);
    b8 timespec = false;
    b8 counter  = false;
    for (u32 i = 0; i < count; i++)
    {
        timespec = timespec || cstr_equal(properties[i].extensionName, "XR_KHR_convert_timespec_time");
        counter  = counter || cstr_equal(properties[i].extensionName, "XR_KHR_win32_convert_performance_counter_time");
    }
    EXPECT_TRUE(timespec);
    EXPECT_EQ(counter, os_native_clock() == OS_NativeClock::PerformanceCounter);
}

TEST_F(OxrPerformanceCounterTest, round_trips_within_a_tick)
{
    const i64 counter = os_ns_to_native_ticks(os_monotonic_ns());
    XrTime    first   = 0;
    ASSERT_EQ(to_time(instance, &counter, &first), XR_SUCCESS);
    EXPECT_GT(first, 0);
    i64 back = 0;
    ASSERT_EQ(to_counter(instance, first, &back), XR_SUCCESS);
    EXPECT_LE(back > counter ? back - counter : counter - back, 1);
    XrTime second = 0;
    ASSERT_EQ(to_time(instance, &back, &second), XR_SUCCESS);
    i64 again = 0;
    ASSERT_EQ(to_counter(instance, second, &again), XR_SUCCESS);
    EXPECT_EQ(again, back) << "a converted counter is stable";
}

TEST_F(OxrPerformanceCounterTest, invalid_arguments_are_rejected)
{
    i64    counter = 0;
    XrTime time    = 0;
    EXPECT_EQ(to_counter(instance, 0, &counter), XR_ERROR_TIME_INVALID);
    EXPECT_EQ(to_counter(instance, -1, &counter), XR_ERROR_TIME_INVALID);
    EXPECT_EQ(to_counter(instance, 1000, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(to_time(instance, nullptr, &time), XR_ERROR_VALIDATION_FAILURE);
    counter = 0;
    EXPECT_EQ(to_time(instance, &counter, &time), XR_ERROR_TIME_INVALID);
    EXPECT_EQ(to_time((XrInstance)(uintptr_t)0x77, &counter, &time), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrPerformanceCounterTest, predicted_display_times_lie_after_now)
{
    create_session();
    const XrTime before = now();
    begin_session();
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    for (u32 i = 0; i < 3; i++)
    {
        ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
        ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    }
    EXPECT_GE(state.predictedDisplayTime, before);
    EXPECT_GE(state.predictedDisplayTime, now());
}

//
// XR_KHR_composition_layer_depth
//

namespace
{
struct OxrLayerDepthTest : OxrVulkanTest
{
    OxrLayerDepthTest ()
    {
        extensions[extensions_count++] = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;
    }

    XrSwapchain released_depth_swapchain (u32 width, u32 height, u32 array_size)
    {
        XrSwapchain                 swapchain = XR_NULL_HANDLE;
        const XrSwapchainCreateInfo info      = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .format = VK_FORMAT_D32_SFLOAT, .sampleCount = 1, .width = width, .height = height, .faceCount = 1, .arraySize = array_size, .mipCount = 1 };
        EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_SUCCESS);
        u32                            index     = 0;
        const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index), XR_SUCCESS);
        EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);
        EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
        return swapchain;
    }
};

XrResult end_frame_with (const OxrVulkanTest& test, const XrCompositionLayerBaseHeader* const* layers)
{
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    EXPECT_EQ(test.xr.xrWaitFrame(test.session, nullptr, &state), XR_SUCCESS);
    EXPECT_EQ(test.xr.xrBeginFrame(test.session, nullptr), XR_SUCCESS);
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = layers };
    const XrResult       result   = test.xr.xrEndFrame(test.session, &end_info);
    if (result != XR_SUCCESS)
    {
        // Leave the frame loop where it was: the next xrBeginFrame of a wait discards this one
        const XrFrameEndInfo empty = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        EXPECT_EQ(test.xr.xrEndFrame(test.session, &empty), XR_SUCCESS);
    }
    return result;
}
} // namespace

// Spec 12.17 XrCompositionLayerDepthInfoKHR: one depth sub-image per projection view
TEST_F(OxrLayerDepthTest, depth_infos_are_validated_and_accepted)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain color = create_swapchain(64, 64, 2);
    const XrSwapchain depth_swapchain = released_depth_swapchain(64, 64, 2);
    const XrSpace     local = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    render_into(color, { { 1.0f, 0.0f, 0.0f, 1.0f } });

    XrCompositionLayerDepthInfoKHR depth_infos[2] = {};
    XrCompositionLayerProjectionView views[2]       = { view(color, 0, 64, 64), view(color, 1, 64, 64) };
    for (u32 v = 0; v < 2; v++)
    {
        depth_infos[v] = { .type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR, .subImage = { .swapchain = depth_swapchain, .imageRect = { { 0, 0 }, { 64, 64 } }, .imageArrayIndex = v }, .minDepth = 0.0f, .maxDepth = 1.0f, .nearZ = 0.1f, .farZ = 100.0f };
        views[v].next  = &depth_infos[v];
    }
    XrCompositionLayerProjection        layer     = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = local, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
    EXPECT_EQ(end_frame_with(*this, layers), XR_SUCCESS);

    // Reversed z (near beyond far) and an infinite far plane are both fine
    depth_infos[0].nearZ = 100.0f;
    depth_infos[0].farZ  = 0.1f;
    depth_infos[1].farZ  = INFINITY;
    EXPECT_EQ(end_frame_with(*this, layers), XR_SUCCESS);
    depth_infos[0] = depth_infos[1];

    depth_infos[1].minDepth = 0.8f;
    depth_infos[1].maxDepth = 0.2f;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    depth_infos[1].minDepth = -0.1f;
    depth_infos[1].maxDepth = 1.0f;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    depth_infos[1].minDepth = 0.0f;
    depth_infos[1].nearZ    = 1.0f;
    depth_infos[1].farZ     = 1.0f;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    depth_infos[1].farZ = 0.0f;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    depth_infos[1].farZ = 100.0f;

    depth_infos[1].subImage.imageRect.extent.width = 65;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_SWAPCHAIN_RECT_INVALID);
    depth_infos[1].subImage.imageRect.extent.width = 64;
    depth_infos[1].subImage.swapchain              = color;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);

    const XrSwapchain never_released = create_swapchain(64, 64, 2, VK_FORMAT_D32_SFLOAT);
    depth_infos[1].subImage.swapchain = never_released;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_LAYER_INVALID);
}

// Without the extension the structure is unknown and ignored, as every unknown next struct is
TEST_F(OxrVulkanTest, depth_infos_are_ignored_without_the_extension)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain color = create_swapchain(64, 64, 2);
    const XrSpace     local = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    render_into(color, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    XrCompositionLayerDepthInfoKHR   garbage  = { .type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR, .minDepth = 5.0f };
    XrCompositionLayerProjectionView views[2] = { view(color, 0, 64, 64), view(color, 1, 64, 64) };
    views[0].next                             = &garbage;
    XrCompositionLayerProjection        layer     = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = local, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
    EXPECT_EQ(end_frame_with(*this, layers), XR_SUCCESS);
}

//
// XR_FB_composition_layer_depth_test
//

namespace
{
struct OxrDepthCompositionTest : OxrLayerDepthTest
{
    OxrDepthCompositionTest ()
    {
        extensions[extensions_count++] = XR_FB_COMPOSITION_LAYER_DEPTH_TEST_EXTENSION_NAME;
    }

    // Acquires, clears array layer i of the depth image to depths[i] (as the app would render) and releases
    void depth_into (XrSwapchain swapchain, const f32* depths, u32 layers, b8 stencil = false)
    {
        u32                        count     = 0;
        XrSwapchainImageVulkan2KHR images[8] = {};
        for (XrSwapchainImageVulkan2KHR& image : images)
        {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        }
        ASSERT_EQ(xr.xrEnumerateSwapchainImages(swapchain, 8, &count, (XrSwapchainImageBaseHeader*)images), XR_SUCCESS);
        u32 index = 0;
        ASSERT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index), XR_SUCCESS);
        const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        ASSERT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);

        const VkCommandBufferAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer                   cmd        = VK_NULL_HANDLE;
        ASSERT_EQ(vkAllocateCommandBuffers(vk_device, &alloc_info, &cmd), VK_SUCCESS);
        const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cmd, &begin_info);
        const VkImageAspectFlags      aspect  = VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? (VkImageAspectFlags)VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
        const VkImageSubresourceRange all     = { aspect, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
        VkImageMemoryBarrier          barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[index].image, .subresourceRange = all };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        for (u32 layer = 0; layer < layers; layer++)
        {
            const VkClearDepthStencilValue value = { depths[layer], 0 };
            const VkImageSubresourceRange  range = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1 };
            vkCmdClearDepthStencilImage(cmd, images[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        }
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(cmd);
        const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        ASSERT_EQ(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), VK_SUCCESS);
        vkQueueWaitIdle(queue);
        vkFreeCommandBuffers(vk_device, pool, 1, &cmd);
        EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
    }

    // One projection layer of a color and a depth swapchain, both eyes in their array layers
    struct DepthLayer
    {
        XrCompositionLayerDepthInfoKHR   depth[2];
        XrCompositionLayerProjectionView views[2];
        XrCompositionLayerDepthTestFB    test;
        XrCompositionLayerProjection     layer;
    };

    void make_layer (DepthLayer& out, XrSwapchain color, XrSwapchain depth, XrSpace space, XrCompareOpFB op, b8 mask)
    {
        out.test = { .type = XR_TYPE_COMPOSITION_LAYER_DEPTH_TEST_FB, .depthMask = (XrBool32)(mask ? XR_TRUE : XR_FALSE), .compareOp = op };
        for (u32 v = 0; v < 2; v++)
        {
            out.depth[v]      = { .type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR, .subImage = { .swapchain = depth, .imageRect = { { 0, 0 }, { 64, 64 } }, .imageArrayIndex = v }, .minDepth = 0.0f, .maxDepth = 1.0f, .nearZ = 0.1f, .farZ = 100.0f };
            out.views[v]      = view(color, v, 64, 64);
            out.views[v].next = &out.depth[v];
        }
        out.layer = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .next = &out.test, .space = space, .viewCount = 2, .views = out.views };
    }

    // The red or blue a layer drew at the center of an eye
    static char color_at (const Array<u8>& pixels, u32 width, u32 height, u32 eye)
    {
        const u8* p = eye_pixel(pixels, width, height, eye, 0.0f, 0.0f);
        return p[0] > 200 && p[2] < 50 ? 'r' : p[2] > 200 && p[0] < 50 ? 'b' : '?';
    }
};
} // namespace

// Depth 0.2 is 0.125 m, 0.8 is 0.5 m with near 0.1 m and far 100 m; reversed (near 100 m,
// far 0.1 m) 0.1 is 0.99 m and 0.5 is 0.2 m
TEST_F(OxrDepthCompositionTest, projection_layers_keep_the_nearer_surface)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSpace     local  = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    const XrSwapchain red    = create_swapchain(64, 64, 2);
    const XrSwapchain blue   = create_swapchain(64, 64, 2);
    const XrSwapchain depth1 = create_swapchain(64, 64, 2, VK_FORMAT_D32_SFLOAT);
    const XrSwapchain depth2 = create_swapchain(64, 64, 2, VK_FORMAT_D32_SFLOAT);
    render_into(red, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    render_into(blue, { { 0.0f, 0.0f, 1.0f, 1.0f } });
    const f32 near_far[2] = { 0.2f, 0.8f };
    const f32 far_near[2] = { 0.8f, 0.2f };
    depth_into(depth1, near_far, 2);
    depth_into(depth2, far_near, 2);

    DepthLayer first  = {};
    DepthLayer second = {};
    make_layer(first, red, depth1, local, XR_COMPARE_OP_LESS_FB, true);
    make_layer(second, blue, depth2, local, XR_COMPARE_OP_LESS_FB, true);
    const XrCompositionLayerBaseHeader* layers[2] = { (XrCompositionLayerBaseHeader*)&first.layer, (XrCompositionLayerBaseHeader*)&second.layer };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;

    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_EQ(color_at(pixels, width, height, 0), 'r');
    EXPECT_EQ(color_at(pixels, width, height, 1), 'b');

    second.test.compareOp = XR_COMPARE_OP_GREATER_FB;
    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_EQ(color_at(pixels, width, height, 0), 'b');
    EXPECT_EQ(color_at(pixels, width, height, 1), 'r');

    // Without writes the first layer leaves the depth far, so the second always passes
    second.test.compareOp = XR_COMPARE_OP_LESS_FB;
    first.test.depthMask  = XR_FALSE;
    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_EQ(color_at(pixels, width, height, 0), 'b');
    EXPECT_EQ(color_at(pixels, width, height, 1), 'b');
    first.test.depthMask = XR_TRUE;

    // Each layer's depth is converted with its own range
    const f32 reversed[2] = { 0.1f, 0.5f };
    depth_into(depth2, reversed, 2);
    for (XrCompositionLayerDepthInfoKHR& info : second.depth)
    {
        info.nearZ = 100.0f;
        info.farZ  = 0.1f;
    }
    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_EQ(color_at(pixels, width, height, 0), 'r');
    EXPECT_EQ(color_at(pixels, width, height, 1), 'b');

    // Without depth tests the second layer covers the first
    first.layer.next  = nullptr;
    second.layer.next = nullptr;
    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_EQ(color_at(pixels, width, height, 0), 'b');
    EXPECT_EQ(color_at(pixels, width, height, 1), 'b');

    // Spec: a depth test on a layer without depth is ignored
    first.layer.next  = &first.test;
    second.layer.next = &second.test;
    second.views[0].next = nullptr;
    second.views[1].next = nullptr;
    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_EQ(color_at(pixels, width, height, 0), 'b');
    EXPECT_EQ(color_at(pixels, width, height, 1), 'b');

    second.test.compareOp = (XrCompareOpFB)8;
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 2, .layers = layers };
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_VALIDATION_FAILURE);
}

// Spec: the runtime computes the depth of geometric layers itself. The projection's depth has a
// stencil aspect, which its layout transitions must include.
TEST_F(OxrDepthCompositionTest, quads_are_depth_tested_against_projection_depth)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSpace     local = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    const XrSwapchain red   = create_swapchain(64, 64, 2);
    const XrSwapchain blue  = create_swapchain(32, 32, 1);
    // A stencil format where the device has one; AMD's Vulkan drivers have no D24_UNORM_S8_UINT
    const b8          stencil = format_offered(VK_FORMAT_D24_UNORM_S8_UINT);
    const XrSwapchain depth   = create_swapchain(64, 64, 2, stencil ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT);
    render_into(red, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    render_into(blue, { { 0.0f, 0.0f, 1.0f, 1.0f } });
    const f32 half_metre[2] = { 0.8f, 0.8f };
    depth_into(depth, half_metre, 2, stencil);

    DepthLayer projection = {};
    make_layer(projection, red, depth, local, XR_COMPARE_OP_LESS_FB, true);
    XrCompositionLayerDepthTestFB test = { .type = XR_TYPE_COMPOSITION_LAYER_DEPTH_TEST_FB, .depthMask = XR_TRUE, .compareOp = XR_COMPARE_OP_LESS_FB };
    XrCompositionLayerQuad        quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .next = &test, .space = local, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = blue, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 1.0f, 1.0f } };
    const XrCompositionLayerBaseHeader* layers[2] = { (XrCompositionLayerBaseHeader*)&projection.layer, (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;

    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_EQ(color_at(pixels, width, height, 0), 'r');
    EXPECT_EQ(color_at(pixels, width, height, 1), 'r');

    quad.pose.position.z = -0.3f;
    quad.size            = { 0.3f, 0.3f };
    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_EQ(color_at(pixels, width, height, 0), 'b');
    EXPECT_EQ(color_at(pixels, width, height, 1), 'b');

    quad.next            = nullptr;
    quad.pose.position.z = -1.0f;
    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_EQ(color_at(pixels, width, height, 0), 'b');
}

// Without the extension the structure is unknown and ignored, as every unknown next struct is
TEST_F(OxrLayerDepthTest, depth_tests_are_ignored_without_the_extension)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain color = create_swapchain(64, 64, 2);
    const XrSpace     local = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    render_into(color, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    XrCompositionLayerDepthTestFB    garbage  = { .type = XR_TYPE_COMPOSITION_LAYER_DEPTH_TEST_FB, .compareOp = (XrCompareOpFB)99 };
    XrCompositionLayerProjectionView views[2] = { view(color, 0, 64, 64), view(color, 1, 64, 64) };
    XrCompositionLayerProjection        layer     = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .next = &garbage, .space = local, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
    EXPECT_EQ(end_frame_with(*this, layers), XR_SUCCESS);
}

//
// XR_FB_foveation, XR_FB_foveation_configuration, XR_META_foveation_eye_tracked
//

namespace
{
struct OxrFoveationTest : OxrVulkanTest
{
    PFN_xrCreateFoveationProfileFB          create_profile  = nullptr;
    PFN_xrDestroyFoveationProfileFB         destroy_profile = nullptr;
    PFN_xrUpdateSwapchainFB                 update          = nullptr;
    PFN_xrGetSwapchainStateFB               get_state       = nullptr;
    PFN_xrGetFoveationEyeTrackedStateMETA   eye_state       = nullptr;

    OxrFoveationTest ()
    {
        extensions[extensions_count++] = XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME;
        extensions[extensions_count++] = XR_FB_FOVEATION_EXTENSION_NAME;
        extensions[extensions_count++] = XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME;
        extensions[extensions_count++] = XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME;
    }

    void start ()
    {
        create_vulkan();
        create_vulkan_session();
        ASSERT_EQ(gipa(instance, "xrCreateFoveationProfileFB", (PFN_xrVoidFunction*)&create_profile), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrDestroyFoveationProfileFB", (PFN_xrVoidFunction*)&destroy_profile), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrUpdateSwapchainFB", (PFN_xrVoidFunction*)&update), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetSwapchainStateFB", (PFN_xrVoidFunction*)&get_state), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetFoveationEyeTrackedStateMETA", (PFN_xrVoidFunction*)&eye_state), XR_SUCCESS);
    }

    XrResult foveated_swapchain (XrSwapchainCreateFoveationFlagsFB flags, XrSwapchain& out_swapchain) const
    {
        const XrSwapchainCreateInfoFoveationFB foveation = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB, .flags = flags };
        const XrSwapchainCreateInfo            info      = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .next = &foveation, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, .format = VK_FORMAT_R8G8B8A8_SRGB, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 1, .arraySize = 2, .mipCount = 1 };
        return xr.xrCreateSwapchain(session, &info, &out_swapchain);
    }
};
} // namespace

TEST_F(OxrVulkanTest, foveation_needs_its_extensions)
{
    create_vulkan();
    create_vulkan_session();
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrCreateFoveationProfileFB", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrGetFoveationEyeTrackedStateMETA", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    XrSystemFoveationEyeTrackedPropertiesMETA eye = { .type = XR_TYPE_SYSTEM_FOVEATION_EYE_TRACKED_PROPERTIES_META, .supportsFoveationEyeTracked = XR_TRUE };
    XrSystemProperties                        props = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &eye };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_FALSE(eye.supportsFoveationEyeTracked);
}

TEST_F(OxrTest, foveation_needs_the_swapchain_update_state)
{
    const char* extensions[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_FB_FOVEATION_EXTENSION_NAME };
    EXPECT_EQ(create_instance(extensions, 2), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
    const char* configuration[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME, XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME };
    EXPECT_EQ(create_instance(configuration, 3), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
}

TEST_F(OxrFoveationTest, profiles_are_validated_and_applied_to_swapchains)
{
    start();
    XrFoveationEyeTrackedProfileCreateInfoMETA eye_tracked = { .type = XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META };
    XrFoveationLevelProfileCreateInfoFB        level       = { .type = XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB, .next = &eye_tracked, .level = XR_FOVEATION_LEVEL_HIGH_FB, .verticalOffset = 5.0f, .dynamic = XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB };
    XrFoveationProfileCreateInfoFB             info        = { .type = XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB, .next = &level };
    XrFoveationProfileFB                       profile     = XR_NULL_HANDLE;
    ASSERT_EQ(create_profile(session, &info, &profile), XR_SUCCESS);
    EXPECT_NE(profile, XR_NULL_HANDLE);

    // Spec: without a level the profile applies no foveation
    const XrFoveationProfileCreateInfoFB plain = { .type = XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB };
    XrFoveationProfileFB                 none  = XR_NULL_HANDLE;
    ASSERT_EQ(create_profile(session, &plain, &none), XR_SUCCESS);

    XrFoveationProfileFB bad = XR_NULL_HANDLE;
    level.level = (XrFoveationLevelFB)4;
    EXPECT_EQ(create_profile(session, &info, &bad), XR_ERROR_VALIDATION_FAILURE);
    level.level   = XR_FOVEATION_LEVEL_LOW_FB;
    level.dynamic = (XrFoveationDynamicFB)2;
    EXPECT_EQ(create_profile(session, &info, &bad), XR_ERROR_VALIDATION_FAILURE);
    level.dynamic        = XR_FOVEATION_DYNAMIC_DISABLED_FB;
    level.verticalOffset = zm::nan();
    EXPECT_EQ(create_profile(session, &info, &bad), XR_ERROR_VALIDATION_FAILURE);
    level.verticalOffset = 0.0f;
    eye_tracked.flags    = 1;
    EXPECT_EQ(create_profile(session, &info, &bad), XR_ERROR_VALIDATION_FAILURE);
    eye_tracked.flags = 0;
    const XrFoveationProfileCreateInfoFB wrong = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO };
    EXPECT_EQ(create_profile(session, &wrong, &bad), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_profile(session, nullptr, &bad), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_profile(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_profile((XrSession)(uintptr_t)0x1234, &info, &bad), XR_ERROR_HANDLE_INVALID);

    XrSwapchain swapchain = XR_NULL_HANDLE;
    ASSERT_EQ(foveated_swapchain(XR_SWAPCHAIN_CREATE_FOVEATION_SCALED_BIN_BIT_FB, swapchain), XR_SUCCESS);
    XrSwapchain other = XR_NULL_HANDLE;
    EXPECT_EQ(foveated_swapchain(4, other), XR_ERROR_VALIDATION_FAILURE);
    // OVRPlugin on Vulkan asks for a fragment density map without XR_FB_foveation_vulkan, which
    // alone hands the map out; without it the request changes nothing the app can see
    ASSERT_EQ(foveated_swapchain(XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB, other), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(other), XR_SUCCESS);

    XrSwapchainStateFoveationFB state = { .type = XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB, .profile = profile };
    ASSERT_EQ(update(swapchain, (const XrSwapchainStateBaseHeaderFB*)&state), XR_SUCCESS);
    // Spec: destroying an applied profile leaves the swapchain's foveation as it is
    ASSERT_EQ(destroy_profile(profile), XR_SUCCESS);
    XrSwapchainStateFoveationFB stored = { .type = XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB };
    ASSERT_EQ(get_state(swapchain, (XrSwapchainStateBaseHeaderFB*)&stored), XR_SUCCESS);
    EXPECT_EQ(stored.profile, profile);
    EXPECT_EQ(stored.flags, 0u);

    EXPECT_EQ(update(swapchain, (const XrSwapchainStateBaseHeaderFB*)&state), XR_ERROR_HANDLE_INVALID);
    state.profile = none;
    state.flags   = 1;
    EXPECT_EQ(update(swapchain, (const XrSwapchainStateBaseHeaderFB*)&state), XR_ERROR_VALIDATION_FAILURE);
    state.flags = 0;
    ASSERT_EQ(update(swapchain, (const XrSwapchainStateBaseHeaderFB*)&state), XR_SUCCESS);
    // The sampler state is XR_FB_swapchain_update_state_vulkan's, not enabled here
    XrSwapchainStateSamplerVulkanFB sampler = { .type = XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB };
    EXPECT_EQ(update(swapchain, (const XrSwapchainStateBaseHeaderFB*)&sampler), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(destroy_profile(none), XR_SUCCESS);
    EXPECT_EQ(destroy_profile(none), XR_ERROR_HANDLE_INVALID);
}

// The simulated user looks straight ahead: foveation centers on each view
TEST_F(OxrFoveationTest, eye_tracked_foveation_centers_on_the_views)
{
    start();
    XrSystemFoveationEyeTrackedPropertiesMETA eye   = { .type = XR_TYPE_SYSTEM_FOVEATION_EYE_TRACKED_PROPERTIES_META };
    XrSystemProperties                        props = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &eye };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_TRUE(eye.supportsFoveationEyeTracked);

    XrFoveationEyeTrackedStateMETA state = { .type = XR_TYPE_FOVEATION_EYE_TRACKED_STATE_META, .foveationCenter = { { 7.0f, 7.0f }, { 7.0f, 7.0f } } };
    ASSERT_EQ(eye_state(session, &state), XR_SUCCESS);
    EXPECT_EQ(state.flags, XR_FOVEATION_EYE_TRACKED_STATE_VALID_BIT_META);
    for (const XrVector2f& center : state.foveationCenter)
    {
        EXPECT_EQ(center.x, 0.0f);
        EXPECT_EQ(center.y, 0.0f);
    }
    XrFoveationEyeTrackedStateMETA wrong = { .type = XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB };
    EXPECT_EQ(eye_state(session, &wrong), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(eye_state(session, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(eye_state((XrSession)(uintptr_t)0x1234, &state), XR_ERROR_HANDLE_INVALID);
}

//
// XR_FB_space_warp
//

namespace
{
struct OxrSpaceWarpTest : OxrLayerDepthTest
{
    OxrSpaceWarpTest ()
    {
        extensions[extensions_count - 1] = XR_FB_SPACE_WARP_EXTENSION_NAME;
        extensions[extensions_count++]   = XR_META_PERFORMANCE_METRICS_EXTENSION_NAME;
    }

    u32 space_warp_mode () const
    {
        PFN_xrSetPerformanceMetricsStateMETA     set_state = nullptr;
        PFN_xrQueryPerformanceMetricsCounterMETA query     = nullptr;
        EXPECT_EQ(gipa(instance, "xrSetPerformanceMetricsStateMETA", (PFN_xrVoidFunction*)&set_state), XR_SUCCESS);
        EXPECT_EQ(gipa(instance, "xrQueryPerformanceMetricsCounterMETA", (PFN_xrVoidFunction*)&query), XR_SUCCESS);
        const XrPerformanceMetricsStateMETA state = { .type = XR_TYPE_PERFORMANCE_METRICS_STATE_META, .enabled = XR_TRUE };
        EXPECT_EQ(set_state(session, &state), XR_SUCCESS);
        XrPerformanceMetricsCounterMETA counter = { .type = XR_TYPE_PERFORMANCE_METRICS_COUNTER_META };
        EXPECT_EQ(query(session, path("/perfmetrics_meta/compositor/spacewarp_mode"), &counter), XR_SUCCESS);
        return counter.uintValue;
    }
};
} // namespace

TEST_F(OxrSpaceWarpTest, system_recommends_a_motion_vector_size)
{
    XrSystemSpaceWarpPropertiesFB space_warp = { .type = XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB };
    XrSystemProperties            props      = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &space_warp };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    u32 count = 0;
    XrViewConfigurationView views[2] = { { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW }, { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    ASSERT_EQ(xr.xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views), XR_SUCCESS);
    EXPECT_EQ(space_warp.recommendedMotionVectorImageRectWidth, views[0].recommendedImageRectWidth / 2);
    EXPECT_EQ(space_warp.recommendedMotionVectorImageRectHeight, views[0].recommendedImageRectHeight / 2);
}

// The compositor does not extrapolate frames, so the infos are validated and dropped
TEST_F(OxrSpaceWarpTest, space_warp_infos_are_validated_and_accepted)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain color   = create_swapchain(64, 64, 2);
    const XrSwapchain motion  = create_swapchain(32, 32, 2, VK_FORMAT_R16G16B16A16_SFLOAT);
    const XrSwapchain depth   = released_depth_swapchain(32, 32, 2);
    const XrSpace     local   = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    render_into(color, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    render_into(motion, { { 0.0f, 0.0f, 0.0f, 0.0f } });
    EXPECT_EQ(space_warp_mode(), 0u);

    XrCompositionLayerSpaceWarpInfoFB infos[2] = {};
    XrCompositionLayerProjectionView  views[2] = { view(color, 0, 64, 64), view(color, 1, 64, 64) };
    for (u32 v = 0; v < 2; v++)
    {
        // An all-zero delta pose as CTS 1.1.63 sends it: the runtime ignores the pose
        infos[v] = { .type = XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB, .motionVectorSubImage = { .swapchain = motion, .imageRect = { { 0, 0 }, { 32, 32 } }, .imageArrayIndex = v }, .appSpaceDeltaPose = {}, .depthSubImage = { .swapchain = depth, .imageRect = { { 0, 0 }, { 32, 32 } }, .imageArrayIndex = v }, .minDepth = 0.0f, .maxDepth = 1.0f, .nearZ = 0.01f, .farZ = 100.0f };
        views[v].next = &infos[v];
    }
    XrCompositionLayerProjection        layer     = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = local, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
    EXPECT_EQ(end_frame_with(*this, layers), XR_SUCCESS);
    EXPECT_EQ(space_warp_mode(), 1u);

    infos[0].layerFlags = XR_COMPOSITION_LAYER_SPACE_WARP_INFO_FRAME_SKIP_BIT_FB;
    infos[0].minDepth   = 0.5f;
    infos[0].maxDepth   = 0.5f;
    infos[0].nearZ      = INFINITY;
    infos[0].farZ       = 0.01f;
    infos[1].farZ       = INFINITY;
    EXPECT_EQ(end_frame_with(*this, layers), XR_SUCCESS);
    infos[0] = infos[1];

    infos[1].layerFlags = 0x8;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    infos[1].layerFlags = 0;
    infos[1].nearZ      = INFINITY;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    infos[1].nearZ = 100.0f;
    infos[1].farZ  = 100.0f;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    infos[1].farZ = -1.0f;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    infos[1].farZ = zm::nan();
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    infos[1].farZ     = 0.01f;
    infos[1].minDepth = 0.7f;
    infos[1].maxDepth = 0.6f;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    infos[1].minDepth = 0.0f;
    infos[1].maxDepth = 1.5f;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    infos[1].maxDepth = 1.0f;
    EXPECT_EQ(end_frame_with(*this, layers), XR_SUCCESS);

    infos[1].motionVectorSubImage.imageRect.extent.width = 33;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_SWAPCHAIN_RECT_INVALID);
    infos[1].motionVectorSubImage.imageRect.extent.width = 32;
    infos[1].motionVectorSubImage.imageArrayIndex        = 2;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    infos[1].motionVectorSubImage.imageArrayIndex = 1;
    infos[1].motionVectorSubImage.swapchain       = depth;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_LAYER_INVALID);
    infos[1].motionVectorSubImage.swapchain = (XrSwapchain)(uintptr_t)0x1234;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_HANDLE_INVALID);
    infos[1].motionVectorSubImage.swapchain = motion;
    infos[1].depthSubImage.swapchain        = motion;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_VALIDATION_FAILURE);
    infos[1].depthSubImage.swapchain          = depth;
    infos[1].depthSubImage.imageRect.offset.x = 1;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_SWAPCHAIN_RECT_INVALID);
    infos[1].depthSubImage.imageRect.offset.x = 0;
    const XrSwapchain never_released          = create_swapchain(32, 32, 2, VK_FORMAT_D32_SFLOAT);
    infos[1].depthSubImage.swapchain          = never_released;
    EXPECT_EQ(end_frame_with(*this, layers), XR_ERROR_LAYER_INVALID);

    views[0].next = nullptr;
    views[1].next = nullptr;
    EXPECT_EQ(end_frame_with(*this, layers), XR_SUCCESS);
    EXPECT_EQ(space_warp_mode(), 0u);
}

// Without the extension the structure is unknown and ignored, as every unknown next struct is
TEST_F(OxrVulkanTest, space_warp_infos_are_ignored_without_the_extension)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain color = create_swapchain(64, 64, 2);
    const XrSpace     local = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    render_into(color, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    XrCompositionLayerSpaceWarpInfoFB garbage  = { .type = XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB, .layerFlags = 0xff, .nearZ = 1.0f, .farZ = 1.0f };
    XrCompositionLayerProjectionView  views[2] = { view(color, 0, 64, 64), view(color, 1, 64, 64) };
    views[1].next                              = &garbage;
    XrCompositionLayerProjection        layer     = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = local, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
    EXPECT_EQ(end_frame_with(*this, layers), XR_SUCCESS);
}

//
// XR_KHR_visibility_mask
//

namespace
{
struct OxrMaskTest : OxrTest
{
    PFN_xrGetVisibilityMaskKHR get_mask = nullptr;
    b8                         lens     = true; // the lens mask switched on

    OxrMaskTest ()
    {
        extension = XR_KHR_VISIBILITY_MASK_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrTest::SetUp();
        os_set_env("PUPPETXR_LENS_MASK", lens ? "1" : "0");
        create_headless_instance();
        create_session();
        ASSERT_EQ(gipa(instance, "xrGetVisibilityMaskKHR", (PFN_xrVoidFunction*)&get_mask), XR_SUCCESS);
    }

    void TearDown () override
    {
        OxrTest::TearDown();
        os_unset_env("PUPPETXR_LENS_MASK");
    }

    void fetch (u32 view, XrVisibilityMaskTypeKHR type, XrVector2f* vertices, u32 vertex_capacity, u32* indices, u32 index_capacity, XrVisibilityMaskKHR& mask) const
    {
        mask = { .type = XR_TYPE_VISIBILITY_MASK_KHR };
        ASSERT_EQ(get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view, type, &mask), XR_SUCCESS);
        ASSERT_LE(mask.vertexCountOutput, vertex_capacity);
        ASSERT_LE(mask.indexCountOutput, index_capacity);
        mask.vertexCapacityInput = vertex_capacity;
        mask.vertices            = vertices;
        mask.indexCapacityInput  = index_capacity;
        mask.indices             = indices;
        ASSERT_EQ(get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view, type, &mask), XR_SUCCESS);
    }
};

f32 cross_2d (XrVector2f o, XrVector2f a, XrVector2f b)
{
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}
} // namespace

// Spec 12.23: triangles and the loop are counter-clockwise, indices in range, the loop surrounds
// the visible area and the hidden triangles lie outside it but inside the view
TEST_F(OxrMaskTest, lens_mask_meshes_are_consistent)
{
    XrVector2f vertices[512] = {};
    u32        indices[1024] = {};
    for (u32 view = 0; view < 2; view++)
    {
        XrVisibilityMaskKHR mask = {};
        fetch(view, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, vertices, 512, indices, 1024, mask);
        ASSERT_GT(mask.vertexCountOutput, 8u);
        EXPECT_EQ(mask.indexCountOutput, mask.vertexCountOutput);
        for (u32 i = 0; i < mask.indexCountOutput; i++)
        {
            const XrVector2f a = vertices[indices[i]];
            const XrVector2f b = vertices[indices[(i + 1) % mask.indexCountOutput]];
            EXPECT_GT(cross_2d({ 0.0f, 0.0f }, a, b), 0.0f) << i;
            EXPECT_LE(zm::abs(a.x), 1.0f + 1e-5f); // the 90 degree view spans tan 1
            EXPECT_LE(zm::abs(a.y), 1.0f + 1e-5f);
        }

        for (const XrVisibilityMaskTypeKHR type : { XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR })
        {
            fetch(view, type, vertices, 512, indices, 1024, mask);
            ASSERT_GT(mask.indexCountOutput, 0u);
            ASSERT_EQ(mask.indexCountOutput % 3, 0u);
            for (u32 i = 0; i < mask.indexCountOutput; i += 3)
            {
                ASSERT_LT(indices[i], mask.vertexCountOutput);
                ASSERT_LT(indices[i + 1], mask.vertexCountOutput);
                ASSERT_LT(indices[i + 2], mask.vertexCountOutput);
                EXPECT_GT(cross_2d(vertices[indices[i]], vertices[indices[i + 1]], vertices[indices[i + 2]]), 0.0f) << type << " " << i;
            }
        }
        // The corners are hidden, the center is not
        bool corner_hidden = false;
        for (u32 i = 0; i < mask.vertexCountOutput; i++)
        {
            corner_hidden = corner_hidden || (zm::abs(vertices[i].x) > 0.999f && zm::abs(vertices[i].y) > 0.999f);
        }
        EXPECT_TRUE(corner_hidden);
    }
}

// Switched in the UI while the app runs: the app hears of each view's new mask (spec 12.27)
TEST_F(OxrMaskTest, switching_the_lens_mask_tells_the_app)
{
    for (const b8 on : { false, true })
    {
        oxr_set_lens_mask(on);
        u32 changed[2] = {};
        for (u32 i = 0; i < 16; i++)
        {
            XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            if (xr.xrPollEvent(instance, &event) != XR_SUCCESS)
            {
                break;
            }
            if (event.type == XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR)
            {
                const XrEventDataVisibilityMaskChangedKHR& mask = *(const XrEventDataVisibilityMaskChangedKHR*)&event;
                EXPECT_EQ(mask.session, session);
                EXPECT_EQ(mask.viewConfigurationType, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
                ASSERT_LT(mask.viewIndex, 2u);
                changed[mask.viewIndex]++;
            }
        }
        EXPECT_EQ(changed[0], 1u) << on;
        EXPECT_EQ(changed[1], 1u) << on;
        XrVisibilityMaskKHR mask = { .type = XR_TYPE_VISIBILITY_MASK_KHR };
        ASSERT_EQ(get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask), XR_SUCCESS);
        EXPECT_EQ(mask.vertexCountOutput > 3, on) << "off hides nothing: one triangle without area";
    }
}

TEST_F(OxrMaskTest, two_call_and_invalid_arguments)
{
    XrVisibilityMaskKHR mask = { .type = XR_TYPE_VISIBILITY_MASK_KHR };
    ASSERT_EQ(get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, &mask), XR_SUCCESS);
    const u32 vertex_count = mask.vertexCountOutput;
    const u32 index_count  = mask.indexCountOutput;
    ASSERT_GT(vertex_count, 0u);

    XrVector2f vertices[512] = {};
    u32        indices[1024] = {};
    // Either capacity 0 means both are queries
    mask = { .type = XR_TYPE_VISIBILITY_MASK_KHR, .vertexCapacityInput = 512, .vertices = vertices, .indexCapacityInput = 0, .indices = indices };
    EXPECT_EQ(get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, &mask), XR_SUCCESS);
    EXPECT_EQ(mask.vertexCountOutput, vertex_count);
    EXPECT_EQ(mask.indexCountOutput, index_count);
    EXPECT_EQ(vertices[0].x, 0.0f);
    mask = { .type = XR_TYPE_VISIBILITY_MASK_KHR, .vertexCapacityInput = vertex_count - 1, .vertices = vertices, .indexCapacityInput = 1024, .indices = indices };
    EXPECT_EQ(get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, &mask), XR_ERROR_SIZE_INSUFFICIENT);
    EXPECT_EQ(mask.vertexCountOutput, vertex_count);

    mask = { .type = XR_TYPE_VISIBILITY_MASK_KHR };
    EXPECT_EQ(get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, &mask), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO, 0, XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, &mask), XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
    EXPECT_EQ(get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, (XrVisibilityMaskTypeKHR)77, &mask), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, nullptr), XR_ERROR_VALIDATION_FAILURE);
    mask.type = XR_TYPE_VIEW;
    EXPECT_EQ(get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, &mask), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(get_mask((XrSession)(uintptr_t)0x77, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, &mask), XR_ERROR_HANDLE_INVALID);
}

namespace
{
struct OxrNoMaskTest : OxrMaskTest
{
    char config[OS_MAX_PATH] = {};

    void SetUp () override
    {
        cstr_format(config, sizeof(config), "%s/puppetxr_no_mask.json", temp_dir());
        ASSERT_TRUE(os_write_file(config, "{ \"views\": { \"mask_radius\": 0 } }", 33));
        os_set_env("PUPPETXR_CONFIG", config);
        OxrMaskTest::SetUp();
    }

    void TearDown () override
    {
        OxrMaskTest::TearDown();
        os_unset_env("PUPPETXR_CONFIG");
        os_delete_file(config);
    }
};
} // namespace

// Without a lens mask the whole view is visible, which is a mask too: the visible mesh and the line
// loop are the view's rectangle and the hidden mesh one triangle without area. Empty meshes, the
// spec's answer for a view without a mask, crashed Meta's Unreal plugin (OculusXRHMD's
// BuildOcclusionMesh creates a zero-sized buffer), and Meta's runtime always has one
TEST_F(OxrNoMaskTest, the_whole_view_is_visible_when_the_lens_has_no_mask)
{
    XrVector2f vertices[8] = {};
    u32        indices[8]  = {};
    const auto get = [&](XrVisibilityMaskTypeKHR type, XrVisibilityMaskKHR& mask) {
        mask = { .type = XR_TYPE_VISIBILITY_MASK_KHR, .vertexCapacityInput = 8, .vertices = vertices, .indexCapacityInput = 8, .indices = indices };
        return get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, type, &mask);
    };
    XrVisibilityMaskKHR hidden = {};
    ASSERT_EQ(get(XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, hidden), XR_SUCCESS);
    ASSERT_EQ(hidden.vertexCountOutput, 3u);
    ASSERT_EQ(hidden.indexCountOutput, 3u);
    const f32 area = (vertices[indices[1]].x - vertices[indices[0]].x) * (vertices[indices[2]].y - vertices[indices[0]].y) -
                     (vertices[indices[1]].y - vertices[indices[0]].y) * (vertices[indices[2]].x - vertices[indices[0]].x);
    EXPECT_EQ(area, 0.0f);

    XrVisibilityMaskKHR visible = {};
    ASSERT_EQ(get(XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, visible), XR_SUCCESS);
    ASSERT_EQ(visible.vertexCountOutput, 4u);
    ASSERT_EQ(visible.indexCountOutput, 6u);
    Pose eye = zm::POSE_IDENTITY;
    Fov  fov = {};
    sim_get_view(oxr_get_sim(), 0, eye, fov);
    f32 min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (u32 i = 0; i < 4; i++)
    {
        min_x = zm::min(min_x, vertices[i].x);
        max_x = zm::max(max_x, vertices[i].x);
        min_y = zm::min(min_y, vertices[i].y);
        max_y = zm::max(max_y, vertices[i].y);
    }
    EXPECT_NEAR(min_x, zm::tan(fov.angle_left), 1e-6f);
    EXPECT_NEAR(max_x, zm::tan(fov.angle_right), 1e-6f);
    EXPECT_NEAR(min_y, zm::tan(fov.angle_down), 1e-6f);
    EXPECT_NEAR(max_y, zm::tan(fov.angle_up), 1e-6f);
    for (u32 t = 0; t < 2; t++)
    {
        const XrVector2f a = vertices[indices[t * 3]];
        const XrVector2f b = vertices[indices[t * 3 + 1]];
        const XrVector2f c = vertices[indices[t * 3 + 2]];
        EXPECT_GT((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x), 0.0f) << "counter-clockwise, as the lens masks";
    }

    XrVisibilityMaskKHR loop = {};
    ASSERT_EQ(get(XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, loop), XR_SUCCESS);
    EXPECT_EQ(loop.vertexCountOutput, 4u);
    EXPECT_EQ(loop.indexCountOutput, 4u);
}

//
// Layer options: XR_KHR_composition_layer_color_scale_bias, XR_EXT_composition_layer_inverted_alpha,
// XR_FB_composition_layer_image_layout
//

namespace
{
struct OxrLayerOptionsTest : OxrVulkanTest
{
    OxrLayerOptionsTest ()
    {
        extensions[extensions_count++] = XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME;
        extensions[extensions_count++] = XR_EXT_COMPOSITION_LAYER_INVERTED_ALPHA_EXTENSION_NAME;
        extensions[extensions_count++] = XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME;
    }

    // Acquires, fills the top half with one color and the bottom half with another, releases
    void upload_halves (XrSwapchain swapchain, u32 width, u32 height, const u8* top, const u8* bottom)
    {
        Array<u8> texels = {};
        defer { texels.release(); };
        texels.resize((usize)width * height * 4);
        for (u32 y = 0; y < height; y++)
        {
            for (u32 x = 0; x < width; x++)
            {
                memory_copy(texels.data + ((usize)y * width + x) * 4, y < height / 2 ? top : bottom, 4);
            }
        }
        upload_texels(swapchain, width, height, 1, texels.data);
    }

    // Acquires, copies tightly packed RGBA texels of `layers` array layers into the image, releases
    void upload_texels (XrSwapchain swapchain, u32 width, u32 height, u32 layers, const u8* source)
    {
        u32 count = 0;
        EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr), XR_SUCCESS);
        XrSwapchainImageVulkan2KHR images[8] = {};
        for (XrSwapchainImageVulkan2KHR& image : images)
        {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        }
        ASSERT_LE(count, 8u);
        EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, count, &count, (XrSwapchainImageBaseHeader*)images), XR_SUCCESS);
        u32 index = 0;
        EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index), XR_SUCCESS);
        const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);

        const VkDeviceSize       size        = (VkDeviceSize)width * height * 4 * layers;
        const VkBufferCreateInfo buffer_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
        VkBuffer                 buffer      = VK_NULL_HANDLE;
        ASSERT_EQ(vkCreateBuffer(vk_device, &buffer_info, nullptr, &buffer), VK_SUCCESS);
        VkMemoryRequirements requirements = {};
        vkGetBufferMemoryRequirements(vk_device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory = {};
        vkGetPhysicalDeviceMemoryProperties(vk_physical, &memory);
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        u32                         type = 0;
        while (type < memory.memoryTypeCount && !((requirements.memoryTypeBits & (1u << type)) && (memory.memoryTypes[type].propertyFlags & host) == host))
        {
            type++;
        }
        const VkMemoryAllocateInfo alloc         = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type };
        VkDeviceMemory             device_memory = VK_NULL_HANDLE;
        ASSERT_EQ(vkAllocateMemory(vk_device, &alloc, nullptr, &device_memory), VK_SUCCESS);
        ASSERT_EQ(vkBindBufferMemory(vk_device, buffer, device_memory, 0), VK_SUCCESS);
        void* mapped = nullptr;
        ASSERT_EQ(vkMapMemory(vk_device, device_memory, 0, VK_WHOLE_SIZE, 0, &mapped), VK_SUCCESS);
        memory_copy(mapped, source, (usize)size);
        vkUnmapMemory(vk_device, device_memory);

        const VkCommandBufferAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer                   cmd        = VK_NULL_HANDLE;
        ASSERT_EQ(vkAllocateCommandBuffers(vk_device, &alloc_info, &cmd), VK_SUCCESS);
        const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cmd, &begin_info);
        const VkImageSubresourceRange range   = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
        VkImageMemoryBarrier          barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[index].image, .subresourceRange = range };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        const VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers }, .imageExtent = { width, height, 1 } };
        vkCmdCopyBufferToImage(cmd, buffer, images[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(cmd);
        const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        ASSERT_EQ(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), VK_SUCCESS);
        vkQueueWaitIdle(queue);
        vkFreeCommandBuffers(vk_device, pool, 1, &cmd);
        vkDestroyBuffer(vk_device, buffer, nullptr);
        vkFreeMemory(vk_device, device_memory, nullptr);
        EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
    }

    XrCompositionLayerQuad centered_quad (XrSwapchain swapchain, XrSpace space) const
    {
        return { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 1.0f, 1.0f } };
    }
};
} // namespace

//
// XR_FB_composition_layer_alpha_blend
//

namespace
{
struct OxrAlphaBlendTest : OxrLayerOptionsTest
{
    OxrAlphaBlendTest ()
    {
        extensions[extensions_count++] = XR_FB_COMPOSITION_LAYER_ALPHA_BLEND_EXTENSION_NAME;
    }
};
} // namespace

// Spec: with the extension, opaque is the only environment blend mode
TEST_F(OxrAlphaBlendTest, only_opaque_environments)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    u32                    count   = 0;
    XrEnvironmentBlendMode modes[4] = {};
    ASSERT_EQ(xr.xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 4, &count, modes), XR_SUCCESS);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(modes[0], XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND };
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED);
}

// Spec: the factors replace the layer blending, and the target's alpha starts at 0
TEST_F(OxrAlphaBlendTest, factors_replace_the_layer_blending)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain red   = create_swapchain(32, 32, 1);
    const XrSwapchain blue  = create_swapchain(32, 32, 1);
    const XrSpace     view  = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(red, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    render_into(blue, { { 0.0f, 0.0f, 1.0f, 0.25f } });

    XrCompositionLayerAlphaBlendFB      blend  = { .type = XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB, .srcFactorColor = XR_BLEND_FACTOR_ZERO_FB, .dstFactorColor = XR_BLEND_FACTOR_ONE_FB, .srcFactorAlpha = XR_BLEND_FACTOR_ZERO_FB, .dstFactorAlpha = XR_BLEND_FACTOR_ONE_FB };
    XrCompositionLayerQuad              bottom = centered_quad(red, view);
    XrCompositionLayerQuad              top    = centered_quad(blue, view);
    top.next                                   = &blend;
    const XrCompositionLayerBaseHeader* layers[2] = { (XrCompositionLayerBaseHeader*)&bottom, (XrCompositionLayerBaseHeader*)&top };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;

    // Without source alpha the layer would cover; zero and one keep what is below
    submit_and_read(layers, 2, pixels, width, height);
    const u8* kept = eye_pixel(pixels, width, height, 0, 0.0f, 0.0f);
    EXPECT_EQ(kept[0], 255);
    EXPECT_EQ(kept[2], 0);

    // The texture's alpha counts without XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
    blend.srcFactorColor = XR_BLEND_FACTOR_SRC_ALPHA_FB;
    blend.dstFactorColor = XR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA_FB;
    submit_and_read(layers, 2, pixels, width, height);
    const u8* mixed = eye_pixel(pixels, width, height, 0, 0.0f, 0.0f);
    EXPECT_NEAR(mixed[0], 225, 2); // sRGB of 0.75 linear
    EXPECT_NEAR(mixed[2], 137, 2); // sRGB of 0.25 linear

    // The red layer wrote alpha 1 over the initial 0: one minus target alpha hides the blue one
    blend.srcFactorColor = XR_BLEND_FACTOR_ONE_MINUS_DST_ALPHA_FB;
    blend.dstFactorColor = XR_BLEND_FACTOR_ONE_FB;
    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[2], 0);
    const XrCompositionLayerBaseHeader* alone[1] = { (XrCompositionLayerBaseHeader*)&top };
    submit_and_read(alone, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[2], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[3], 255); // screenshots are opaque

    blend.dstFactorAlpha = (XrBlendFactorFB)6;
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = alone };
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_VALIDATION_FAILURE);
}

// Without the extension the structure is unknown and ignored, as every unknown next struct is
TEST_F(OxrLayerOptionsTest, alpha_blend_is_ignored_without_the_extension)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain blue = create_swapchain(32, 32, 1);
    const XrSpace     view = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(blue, { { 0.0f, 0.0f, 1.0f, 1.0f } });
    XrCompositionLayerAlphaBlendFB      blend     = { .type = XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB, .srcFactorColor = (XrBlendFactorFB)99 };
    XrCompositionLayerQuad              quad      = centered_quad(blue, view);
    quad.next                                     = &blend;
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[2], 255);
}

TEST_F(OxrLayerOptionsTest, image_layout_flips_vertically)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain = create_swapchain(32, 32, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    const u8          red[4]     = { 255, 0, 0, 255 };
    const u8          blue[4]    = { 0, 0, 255, 255 };
    upload_halves(swapchain, 32, 32, red, blue);

    XrCompositionLayerQuad              quad      = centered_quad(swapchain, view_space);
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, -0.3f)[0], 255); // NDC y points down: top of the quad
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.3f)[2], 255);

    // Spec 12.111 XrCompositionLayerImageLayoutFB: the image's origin is its bottom left
    XrCompositionLayerImageLayoutFB layout = { .type = XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB, .flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB };
    quad.next                              = &layout;
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, -0.3f)[2], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.3f)[0], 255);
}

// Spec 12.5: scale and bias apply to the unpremultiplied color, which is premultiplied again
TEST_F(OxrLayerOptionsTest, color_scale_and_bias_are_applied)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain  = create_swapchain(32, 32, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });

    XrCompositionLayerColorScaleBiasKHR scale_bias = { .type = XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR, .colorScale = { 0.5f, 1.0f, 1.0f, 1.0f }, .colorBias = { 0.0f, 0.0f, 0.5f, 0.0f } };
    XrCompositionLayerQuad              quad       = centered_quad(swapchain, view_space);
    quad.next                                      = &scale_bias;
    const XrCompositionLayerBaseHeader* layers[1]  = { (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    const u8* center = eye_pixel(pixels, width, height, 0, 0.0f, 0.0f);
    EXPECT_NEAR(center[0], 188, 2); // 0.5 linear in sRGB
    EXPECT_EQ(center[1], 0);
    EXPECT_NEAR(center[2], 188, 2);
}

// XR_EXT_composition_layer_inverted_alpha: alpha means transparency, inverted before blending
TEST_F(OxrLayerOptionsTest, inverted_alpha_and_flag_validation)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain background = create_swapchain(64, 64, 2);
    const XrSwapchain overlay    = create_swapchain(32, 32, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(background, { { 0.0f, 0.0f, 1.0f, 1.0f } });
    render_into(overlay, { { 1.0f, 0.0f, 0.0f, 0.75f } }); // 75 % transparent

    XrCompositionLayerProjectionView    views[2]   = { view(background, 0, 64, 64), view(background, 1, 64, 64) };
    XrCompositionLayerProjection        projection = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = view_space, .viewCount = 2, .views = views };
    XrCompositionLayerQuad              quad       = centered_quad(overlay, view_space);
    quad.layerFlags                                = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT | XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT;
    const XrCompositionLayerBaseHeader* layers[2]  = { (XrCompositionLayerBaseHeader*)&projection, (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 2, pixels, width, height);
    const u8* center = eye_pixel(pixels, width, height, 0, 0.0f, 0.0f);
    EXPECT_NEAR(center[0], 137, 2); // red at 0.25 opacity, linear 0.25
    EXPECT_NEAR(center[2], 225, 2); // blue keeps 0.75

    // Bits nobody defined are invalid; spec 10.6.1 layerFlags valid usage
    quad.layerFlags = 0x100;
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
    XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 2, .layers = layers };
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_VALIDATION_FAILURE);
    end_info.layerCount = 0;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
}

TEST_F(OxrVulkanTest, inverted_alpha_needs_its_extension)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain overlay    = create_swapchain(32, 32, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(overlay, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    XrCompositionLayerQuad quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .layerFlags = XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = overlay, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 1.0f, 1.0f } };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
    XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = layers };
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_VALIDATION_FAILURE);
    end_info.layerCount = 0;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
}


//
// Curved layers: XR_KHR_composition_layer_cylinder, XR_KHR_composition_layer_equirect,
// XR_KHR_composition_layer_equirect2
//

namespace
{
struct OxrCurvedLayerTest : OxrLayerOptionsTest
{
    OxrCurvedLayerTest ()
    {
        extensions[extensions_count++] = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
        extensions[extensions_count++] = XR_KHR_COMPOSITION_LAYER_EQUIRECT_EXTENSION_NAME;
        extensions[extensions_count++] = XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME;
    }

    // Red top half, blue bottom half
    XrSwapchain halves_swapchain ()
    {
        const XrSwapchain swapchain = create_swapchain(32, 32, 1);
        const u8          red[4]    = { 255, 0, 0, 255 };
        const u8          blue[4]   = { 0, 0, 255, 255 };
        upload_halves(swapchain, 32, 32, red, blue);
        return swapchain;
    }

    // A rejected frame is ended without layers, so the next one begins cleanly
    XrResult end_frame (const XrCompositionLayerBaseHeader* const* layers, u32 layers_count)
    {
        XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
        EXPECT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
        EXPECT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
        XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = layers_count, .layers = layers };
        const XrResult result   = xr.xrEndFrame(session, &end_info);
        if (result != XR_SUCCESS)
        {
            end_info.layerCount = 0;
            EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
        }
        return result;
    }

    static b8 is_red (const u8* pixel)
    {
        return pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0;
    }

    static b8 is_blue (const u8* pixel)
    {
        return pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255;
    }

    static b8 is_black (const u8* pixel)
    {
        return pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0;
    }
};
} // namespace

// Spec 12.7: the image covers centralAngle around the pose's -Z and a height of
// radius * centralAngle / aspectRatio; radius 0 and infinity are a cylinder at infinity
TEST_F(OxrCurvedLayerTest, cylinder_maps_the_image_inside)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain  = halves_swapchain();
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    const f32 radii[3] = { 1.0f, 0.0f, INFINITY };
    for (const f32 radius : radii)
    {
        XrCompositionLayerCylinderKHR       cylinder  = { .type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .radius = radius, .centralAngle = 1.0f, .aspectRatio = 1.0f };
        const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&cylinder };
        submit_and_read(layers, 1, pixels, width, height);
        for (u32 eye = 0; eye < 2; eye++)
        {
            EXPECT_TRUE(is_red(eye_pixel(pixels, width, height, eye, 0.0f, -0.3f))) << radius << " " << eye;
            EXPECT_TRUE(is_blue(eye_pixel(pixels, width, height, eye, 0.0f, 0.3f))) << radius << " " << eye;
            EXPECT_TRUE(is_red(eye_pixel(pixels, width, height, eye, 0.4f, -0.3f))) << radius << " " << eye;
            EXPECT_TRUE(is_black(eye_pixel(pixels, width, height, eye, 0.8f, -0.3f))) << radius << " " << eye;  // beyond half the central angle
            EXPECT_TRUE(is_black(eye_pixel(pixels, width, height, eye, 0.0f, -0.8f))) << radius << " " << eye;  // above the top edge
        }
    }
}

// Spec 12.7.1: only the interior of the cylinder is visible
TEST_F(OxrCurvedLayerTest, cylinder_exterior_is_not_drawn)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain  = halves_swapchain();
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    XrCompositionLayerCylinderKHR       cylinder  = { .type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -3 } }, .radius = 1.0f, .centralAngle = 1.0f, .aspectRatio = 1.0f };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&cylinder };
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_TRUE(is_blue(eye_pixel(pixels, width, height, 0, 0.0f, 0.05f))); // the far wall, seen from inside

    cylinder.pose.orientation = { 0, 1, 0, 0 }; // the section now faces the viewer from outside
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_TRUE(is_black(eye_pixel(pixels, width, height, 0, 0.0f, 0.05f)));
    EXPECT_TRUE(is_black(eye_pixel(pixels, width, height, 0, 0.0f, -0.05f)));
}

// Spec 12.10: centralHorizontalAngle around -Z, latitudes from upperVerticalAngle at the top
// of the image to lowerVerticalAngle at the bottom
TEST_F(OxrCurvedLayerTest, equirect2_maps_angles)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain  = halves_swapchain();
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    XrCompositionLayerEquirect2KHR      equirect  = { .type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .radius = 0.0f, .centralHorizontalAngle = 2.0f * zm::PI, .upperVerticalAngle = 0.5f * zm::PI, .lowerVerticalAngle = -0.5f * zm::PI };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&equirect };
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_TRUE(is_red(eye_pixel(pixels, width, height, 0, 0.8f, -0.3f)));
    EXPECT_TRUE(is_blue(eye_pixel(pixels, width, height, 0, -0.8f, 0.3f)));

    equirect.centralHorizontalAngle = 1.0f;
    equirect.upperVerticalAngle     = 0.25f * zm::PI;
    equirect.lowerVerticalAngle     = 0.0f; // the image spans 45 degrees above the horizon
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_TRUE(is_blue(eye_pixel(pixels, width, height, 0, 0.0f, -0.3f))); // 17 degrees up, in the lower half of the image
    EXPECT_TRUE(is_red(eye_pixel(pixels, width, height, 0, 0.0f, -0.9f)));  // 42 degrees up
    EXPECT_TRUE(is_black(eye_pixel(pixels, width, height, 0, 0.0f, 0.3f)));
    EXPECT_TRUE(is_black(eye_pixel(pixels, width, height, 0, 0.8f, -0.3f)));

    equirect.radius                 = 2.0f;
    equirect.centralHorizontalAngle = 2.0f * zm::PI;
    equirect.upperVerticalAngle     = 0.5f * zm::PI;
    equirect.lowerVerticalAngle     = -0.5f * zm::PI;
    equirect.pose.position          = { 0.0f, -2.5f, 0.0f }; // seen from above, from outside
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_TRUE(is_black(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)));
    EXPECT_TRUE(is_red(eye_pixel(pixels, width, height, 0, 0.0f, 0.9f))); // the inside of the far side, 26 degrees above its equator
}

// XR_KHR_composition_layer_equirect: scale is the fraction of the sphere the image covers,
// centered on -Z; bias shifts the texture coordinates
TEST_F(OxrCurvedLayerTest, equirect_scale_and_bias)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain  = halves_swapchain();
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    XrCompositionLayerEquirectKHR       equirect  = { .type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .radius = INFINITY, .scale = { 1.0f, 1.0f }, .bias = { 0.0f, 0.0f } };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&equirect };
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_TRUE(is_red(eye_pixel(pixels, width, height, 0, 0.8f, -0.3f)));
    EXPECT_TRUE(is_blue(eye_pixel(pixels, width, height, 0, -0.8f, 0.3f)));

    equirect.scale = { 0.125f, 0.25f }; // 45 degrees by 45 degrees
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_TRUE(is_red(eye_pixel(pixels, width, height, 0, 0.0f, -0.3f)));
    EXPECT_TRUE(is_black(eye_pixel(pixels, width, height, 0, 0.0f, -0.6f)));
    EXPECT_TRUE(is_black(eye_pixel(pixels, width, height, 0, 0.6f, -0.3f)));

    equirect.bias = { 0.0f, 0.5f }; // the image moves up by half its height
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_TRUE(is_blue(eye_pixel(pixels, width, height, 0, 0.0f, -0.3f)));
    EXPECT_TRUE(is_black(eye_pixel(pixels, width, height, 0, 0.0f, 0.1f)));
}

TEST_F(OxrCurvedLayerTest, curved_layer_parameters_are_validated)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain  = halves_swapchain();
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    XrCompositionLayerCylinderKHR cylinder = { .type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .radius = 1.0f, .centralAngle = 1.0f, .aspectRatio = 1.0f };
    XrCompositionLayerEquirect2KHR equirect = { .type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .radius = 1.0f, .centralHorizontalAngle = 1.0f, .upperVerticalAngle = 0.5f, .lowerVerticalAngle = -0.5f };
    const XrCompositionLayerBaseHeader* cylinder_layers[1] = { (XrCompositionLayerBaseHeader*)&cylinder };
    const XrCompositionLayerBaseHeader* equirect_layers[1] = { (XrCompositionLayerBaseHeader*)&equirect };
    EXPECT_EQ(end_frame(cylinder_layers, 1), XR_SUCCESS);
    EXPECT_EQ(end_frame(equirect_layers, 1), XR_SUCCESS);

    cylinder.radius = -1.0f;
    EXPECT_EQ(end_frame(cylinder_layers, 1), XR_ERROR_VALIDATION_FAILURE);
    cylinder.radius       = 1.0f;
    cylinder.centralAngle = NAN;
    EXPECT_EQ(end_frame(cylinder_layers, 1), XR_ERROR_VALIDATION_FAILURE);
    cylinder.centralAngle = 1.0f;
    cylinder.aspectRatio  = NAN;
    EXPECT_EQ(end_frame(cylinder_layers, 1), XR_ERROR_VALIDATION_FAILURE);
    cylinder.aspectRatio = 1.0f;
    cylinder.pose.orientation = { 0, 0, 0, 0 };
    EXPECT_EQ(end_frame(cylinder_layers, 1), XR_ERROR_POSE_INVALID);

    equirect.upperVerticalAngle = NAN;
    EXPECT_EQ(end_frame(equirect_layers, 1), XR_ERROR_VALIDATION_FAILURE);
    equirect.upperVerticalAngle = 0.5f;
    equirect.radius             = NAN;
    EXPECT_EQ(end_frame(equirect_layers, 1), XR_ERROR_VALIDATION_FAILURE);
    equirect.radius        = 1.0f;
    equirect.eyeVisibility = (XrEyeVisibility)7;
    EXPECT_EQ(end_frame(equirect_layers, 1), XR_ERROR_VALIDATION_FAILURE);
}

static u32 count_log_lines (const char* needle)
{
    u32 count = 0;
    for (u32 i = 0; i < os_log_history_count(); i++)
    {
        count += cstr_find(os_log_history(i)->text, needle) != nullptr;
    }
    return count;
}

// Regression (xrgears): its sky had 2 * 3.142 as centralHorizontalAngle and 3.142 / 2 as its
// latitudes, just past the spec ranges, and every frame was rejected. Angles outside the
// ranges are clamped and logged once, so the sphere is drawn whole.
TEST_F(OxrCurvedLayerTest, angles_outside_their_range_are_clamped_and_logged_once)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain  = halves_swapchain();
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    XrCompositionLayerEquirect2KHR      equirect = { .type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .radius = 0.0f, .centralHorizontalAngle = 2.0f * 3.142f, .upperVerticalAngle = 3.142f / 2.0f, .lowerVerticalAngle = -3.142f / 2.0f };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&equirect };
    const u32                           before    = count_log_lines("clamped");
    EXPECT_EQ(end_frame(layers, 1), XR_SUCCESS);
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_TRUE(is_red(eye_pixel(pixels, width, height, 0, 0.8f, -0.3f)));
    EXPECT_TRUE(is_blue(eye_pixel(pixels, width, height, 0, -0.8f, 0.3f)));
    EXPECT_EQ(count_log_lines("clamped"), before + 1) << "logged once, not every frame";

    XrCompositionLayerCylinderKHR       cylinder        = { .type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .radius = 1.0f, .centralAngle = 7.0f, .aspectRatio = 1.0f };
    const XrCompositionLayerBaseHeader* cylinder_layers[1] = { (XrCompositionLayerBaseHeader*)&cylinder };
    EXPECT_EQ(end_frame(cylinder_layers, 1), XR_SUCCESS);
    cylinder.centralAngle = -1.0f;
    EXPECT_EQ(end_frame(cylinder_layers, 1), XR_SUCCESS);
}

// Rule 2: a rejected call is logged with its failed condition, once per check, so an app that
// repeats it every frame shows up in the log without flooding it
TEST_F(OxrCurvedLayerTest, rejections_are_logged_once_per_check)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain                   swapchain  = halves_swapchain();
    const XrSpace                       view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    XrCompositionLayerCylinderKHR       cylinder   = { .type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR, .space = view_space, .eyeVisibility = (XrEyeVisibility)7, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .radius = 1.0f, .centralAngle = 1.0f, .aspectRatio = 1.0f };
    const XrCompositionLayerBaseHeader* layers[1]  = { (XrCompositionLayerBaseHeader*)&cylinder };
    const u32                           checks     = count_log_lines("eye_visibility_valid(visibility)");
    const u32                           frames     = count_log_lines("xrEndFrame rejected layer");
    EXPECT_EQ(end_frame(layers, 1), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(end_frame(layers, 1), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(count_log_lines("eye_visibility_valid(visibility)"), checks + (checks == 0 ? 1u : 0u));
    EXPECT_EQ(count_log_lines("xrEndFrame rejected layer"), frames + (frames == 0 ? 1u : 0u));
    EXPECT_GE(count_log_lines("eye_visibility_valid(visibility)"), 1u);
}

TEST_F(OxrVulkanTest, curved_layers_need_their_extensions)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain  = create_swapchain(32, 32, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    const XrCompositionLayerCylinderKHR  cylinder  = { .type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .radius = 1.0f, .centralAngle = 1.0f, .aspectRatio = 1.0f };
    const XrCompositionLayerEquirectKHR  equirect  = { .type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .radius = 1.0f, .scale = { 1, 1 } };
    const XrCompositionLayerEquirect2KHR equirect2 = { .type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } }, .radius = 1.0f, .centralHorizontalAngle = 1.0f };
    const XrCompositionLayerBaseHeader*  all[3]    = { (const XrCompositionLayerBaseHeader*)&cylinder, (const XrCompositionLayerBaseHeader*)&equirect, (const XrCompositionLayerBaseHeader*)&equirect2 };
    for (const XrCompositionLayerBaseHeader* layer : all)
    {
        XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
        ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
        ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
        XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = &layer };
        EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_LAYER_INVALID);
        end_info.layerCount = 0;
        EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    }
}

//
// XR_KHR_composition_layer_cube
//

namespace
{
struct OxrCubeLayerTest : OxrCurvedLayerTest
{
    OxrCubeLayerTest ()
    {
        extensions[extensions_count++] = XR_KHR_COMPOSITION_LAYER_CUBE_EXTENSION_NAME;
    }

    XrResult create_cube_swapchain (u32 width, u32 height, u32 array_size, XrSwapchain& out_swapchain)
    {
        const XrSwapchainCreateInfo info = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT, .format = VK_FORMAT_R8G8B8A8_SRGB, .sampleCount = 1, .width = width, .height = height, .faceCount = 6, .arraySize = array_size, .mipCount = 1 };
        out_swapchain = XR_NULL_HANDLE;
        return xr.xrCreateSwapchain(session, &info, &out_swapchain);
    }
};

const u8 cube_red[4]     = { 255, 0, 0, 255 };
const u8 cube_green[4]   = { 0, 255, 0, 255 };
const u8 cube_blue[4]    = { 0, 0, 255, 255 };
const u8 cube_white[4]   = { 255, 255, 255, 255 };
const u8 cube_yellow[4]  = { 255, 255, 0, 255 };
const u8 cube_magenta[4] = { 255, 0, 255, 255 };
const u8 cube_cyan[4]    = { 0, 255, 255, 255 };
} // namespace

// Faces are array layers imageArrayIndex * 6 + face in Vulkan order (+X, -X, +Y, -Y, +Z, -Z) and
// are sampled like a samplerCube with the view direction in the layer's frame
TEST_F(OxrCubeLayerTest, faces_are_sampled_like_a_cube_map)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    XrSwapchain swapchain = XR_NULL_HANDLE;
    ASSERT_EQ(create_cube_swapchain(16, 16, 2, swapchain), XR_SUCCESS);

    const u32 size = 16;
    Array<u8> texels = {};
    defer { texels.release(); };
    texels.resize((usize)size * size * 4 * 12);
    const u8* solid[12] = { cube_yellow, cube_magenta, cube_cyan, cube_white, cube_green, nullptr, cube_cyan, cube_cyan, cube_cyan, cube_cyan, cube_cyan, cube_cyan };
    for (u32 layer = 0; layer < 12; layer++)
    {
        for (u32 y = 0; y < size; y++)
        {
            for (u32 x = 0; x < size; x++)
            {
                const u8* quadrant = y < size / 2 ? (x < size / 2 ? cube_red : cube_green) : (x < size / 2 ? cube_blue : cube_white);
                memory_copy(texels.data + (((usize)layer * size + y) * size + x) * 4, solid[layer] ? solid[layer] : quadrant, 4);
            }
        }
    }
    upload_texels(swapchain, size, size, 12, texels.data);

    const XrSpace                       view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    XrCompositionLayerCubeKHR           cube       = { .type = XR_TYPE_COMPOSITION_LAYER_CUBE_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .swapchain = swapchain, .imageArrayIndex = 0, .orientation = { 0, 0, 0, 1 } };
    const XrCompositionLayerBaseHeader* layers[1]  = { (XrCompositionLayerBaseHeader*)&cube };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    // -Z face: s = 0.5 (1 - x / |z|), t = 0.5 (1 - y / |z|)
    EXPECT_TRUE(is_red(eye_pixel(pixels, width, height, 0, 0.3f, -0.3f)));  // up and right
    EXPECT_TRUE(eye_pixel(pixels, width, height, 0, -0.3f, -0.3f)[1] == 255 && eye_pixel(pixels, width, height, 0, -0.3f, -0.3f)[0] == 0);
    EXPECT_TRUE(is_blue(eye_pixel(pixels, width, height, 0, 0.3f, 0.3f)));

    struct Turn
    {
        XrQuaternionf orientation;
        const u8*     color;
    };
    const f32  h        = 0.70710678f;
    const Turn turns[5] = { { { 0, h, 0, h }, cube_yellow }, { { 0, -h, 0, h }, cube_magenta }, { { h, 0, 0, h }, cube_white }, { { -h, 0, 0, h }, cube_cyan }, { { 0, 1, 0, 0 }, cube_green } };
    for (const Turn& turn : turns)
    {
        cube.orientation = turn.orientation;
        submit_and_read(layers, 1, pixels, width, height);
        for (u32 eye = 0; eye < 2; eye++)
        {
            const u8* center = eye_pixel(pixels, width, height, eye, 0.0f, 0.0f);
            EXPECT_EQ(memcmp(center, turn.color, 3), 0) << (u32)center[0] << " " << (u32)center[1] << " " << (u32)center[2];
        }
    }

    cube.orientation     = { 0, 0, 0, 1 };
    cube.imageArrayIndex = 1;
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(memcmp(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f), cube_cyan, 3), 0);
}

TEST_F(OxrCubeLayerTest, cube_swapchains_and_layers_are_validated)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    XrSwapchain swapchain = XR_NULL_HANDLE;
    EXPECT_EQ(create_cube_swapchain(16, 8, 1, swapchain), XR_ERROR_FEATURE_UNSUPPORTED); // faces are square
    EXPECT_EQ(create_cube_swapchain(16, 16, 43, swapchain), XR_ERROR_FEATURE_UNSUPPORTED); // 258 layers
    const XrSwapchainCreateInfo three = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, .format = VK_FORMAT_R8G8B8A8_SRGB, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 3, .arraySize = 1, .mipCount = 1 };
    EXPECT_EQ(xr.xrCreateSwapchain(session, &three, &swapchain), XR_ERROR_VALIDATION_FAILURE); // spec 10.1.2: 6 or 1

    ASSERT_EQ(create_cube_swapchain(16, 16, 1, swapchain), XR_SUCCESS);
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    const XrSwapchain flat = halves_swapchain();
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);

    XrCompositionLayerCubeKHR           cube          = { .type = XR_TYPE_COMPOSITION_LAYER_CUBE_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .swapchain = swapchain, .orientation = { 0, 0, 0, 1 } };
    const XrCompositionLayerBaseHeader* cube_layers[1] = { (XrCompositionLayerBaseHeader*)&cube };
    EXPECT_EQ(end_frame(cube_layers, 1), XR_SUCCESS);
    cube.imageArrayIndex = 1;
    EXPECT_EQ(end_frame(cube_layers, 1), XR_ERROR_VALIDATION_FAILURE);
    cube.imageArrayIndex = 0;
    cube.orientation     = { 0, 0, 0, 0 };
    EXPECT_EQ(end_frame(cube_layers, 1), XR_ERROR_POSE_INVALID);
    cube.orientation   = { 0, 0, 0, 1 };
    cube.eyeVisibility = (XrEyeVisibility)5;
    EXPECT_EQ(end_frame(cube_layers, 1), XR_ERROR_VALIDATION_FAILURE);
    cube.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    cube.swapchain     = flat; // spec 12.6: the swapchain has 6 faces
    EXPECT_EQ(end_frame(cube_layers, 1), XR_ERROR_VALIDATION_FAILURE);

    // Every other layer takes swapchains of one face
    XrCompositionLayerQuad              quad          = centered_quad(swapchain, view_space);
    const XrCompositionLayerBaseHeader* quad_layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    EXPECT_EQ(end_frame(quad_layers, 1), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrVulkanTest, cube_layer_needs_its_extension)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    XrSwapchain                 swapchain = XR_NULL_HANDLE;
    const XrSwapchainCreateInfo info      = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, .format = VK_FORMAT_R8G8B8A8_SRGB, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 6, .arraySize = 1, .mipCount = 1 };
    ASSERT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_SUCCESS); // faceCount 6 is core
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    const XrSpace                       view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    const XrCompositionLayerCubeKHR     cube       = { .type = XR_TYPE_COMPOSITION_LAYER_CUBE_KHR, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .swapchain = swapchain, .orientation = { 0, 0, 0, 1 } };
    const XrCompositionLayerBaseHeader* layer      = (const XrCompositionLayerBaseHeader*)&cube;
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
    ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
    XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = 1, .layers = &layer };
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_ERROR_LAYER_INVALID);
    end_info.layerCount = 0;
    EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
}

//
// Passthrough and boundary: XR_META_boundary_visibility, XR_META_passthrough_preferences,
// XR_FB_passthrough_keyboard_hands, XR_META_passthrough_layer_resumed_event
//

namespace
{
struct OxrPassthroughExtrasTest : OxrPassthroughTest
{
    OxrPassthroughExtrasTest ()
    {
        extensions[extensions_count++] = XR_META_BOUNDARY_VISIBILITY_EXTENSION_NAME;
        extensions[extensions_count++] = XR_META_PASSTHROUGH_PREFERENCES_EXTENSION_NAME;
        extensions[extensions_count++] = XR_FB_PASSTHROUGH_KEYBOARD_HANDS_EXTENSION_NAME;
        extensions[extensions_count++] = XR_META_PASSTHROUGH_LAYER_RESUMED_EVENT_EXTENSION_NAME;
    }

    void empty_frame () const
    {
        XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
        EXPECT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
        EXPECT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    }

    u32 resumed_events (XrPassthroughLayerFB& out_layer) const
    {
        u32 count = 0;
        while (true)
        {
            XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            if (xr.xrPollEvent(instance, &event) != XR_SUCCESS)
            {
                return count;
            }
            if (event.type == XR_TYPE_EVENT_DATA_PASSTHROUGH_LAYER_RESUMED_META)
            {
                out_layer = ((const XrEventDataPassthroughLayerResumedMETA*)&event)->layer;
                count++;
            }
        }
    }
};
} // namespace

// Like Quest, the boundary may only be hidden while the app shows passthrough
TEST_F(OxrPassthroughExtrasTest, boundary_suppression_needs_running_passthrough)
{
    XrSystemBoundaryVisibilityPropertiesMETA boundary_props = { .type = XR_TYPE_SYSTEM_BOUNDARY_VISIBILITY_PROPERTIES_META };
    XrSystemProperties                       props          = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &boundary_props };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_TRUE(boundary_props.supportsBoundaryVisibility);

    start(0);
    PFN_xrRequestBoundaryVisibilityMETA request = nullptr;
    ASSERT_EQ(gipa(instance, "xrRequestBoundaryVisibilityMETA", (PFN_xrVoidFunction*)&request), XR_SUCCESS);
    drain_events();
    EXPECT_EQ(request(session, XR_BOUNDARY_VISIBILITY_SUPPRESSED_META), XR_BOUNDARY_VISIBILITY_SUPPRESSION_NOT_ALLOWED_META);
    XrEventDataBuffer event = {};
    EXPECT_FALSE(poll_for(XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META, event));
    EXPECT_EQ(request(session, (XrBoundaryVisibilityMETA)0), XR_ERROR_VALIDATION_FAILURE);

    ASSERT_EQ(pt.xrPassthroughStartFB(passthrough), XR_SUCCESS);
    ASSERT_EQ(pt.xrPassthroughLayerResumeFB(layer), XR_SUCCESS);
    EXPECT_EQ(request(session, XR_BOUNDARY_VISIBILITY_SUPPRESSED_META), XR_SUCCESS);
    ASSERT_TRUE(poll_for(XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META, event));
    EXPECT_EQ(((const XrEventDataBoundaryVisibilityChangedMETA*)&event)->boundaryVisibility, XR_BOUNDARY_VISIBILITY_SUPPRESSED_META);
    EXPECT_EQ(request(session, XR_BOUNDARY_VISIBILITY_SUPPRESSED_META), XR_SUCCESS);
    EXPECT_FALSE(poll_for(XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META, event));

    // Passthrough stops, so the boundary comes back on the next frame
    ASSERT_EQ(pt.xrPassthroughLayerPauseFB(layer), XR_SUCCESS);
    empty_frame();
    ASSERT_TRUE(poll_for(XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META, event));
    EXPECT_EQ(((const XrEventDataBoundaryVisibilityChangedMETA*)&event)->boundaryVisibility, XR_BOUNDARY_VISIBILITY_NOT_SUPPRESSED_META);
    empty_frame();
    EXPECT_FALSE(poll_for(XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META, event));
}

// Spec 12.171: queued once a resumed layer is ready, after a resume or a running creation
TEST_F(OxrPassthroughExtrasTest, resumed_layers_queue_one_event)
{
    start();
    XrPassthroughLayerFB reported = XR_NULL_HANDLE;
    drain_events();
    empty_frame();
    EXPECT_EQ(resumed_events(reported), 1u);
    EXPECT_EQ(reported, layer);
    empty_frame();
    EXPECT_EQ(resumed_events(reported), 0u);

    ASSERT_EQ(pt.xrPassthroughLayerPauseFB(layer), XR_SUCCESS);
    ASSERT_EQ(pt.xrPassthroughLayerResumeFB(layer), XR_SUCCESS);
    empty_frame();
    EXPECT_EQ(resumed_events(reported), 1u);

    // Not ready while the passthrough feature itself is paused
    ASSERT_EQ(pt.xrPassthroughPauseFB(passthrough), XR_SUCCESS);
    ASSERT_EQ(pt.xrPassthroughLayerPauseFB(layer), XR_SUCCESS);
    ASSERT_EQ(pt.xrPassthroughLayerResumeFB(layer), XR_SUCCESS);
    empty_frame();
    EXPECT_EQ(resumed_events(reported), 0u);
    ASSERT_EQ(pt.xrPassthroughStartFB(passthrough), XR_SUCCESS);
    empty_frame();
    EXPECT_EQ(resumed_events(reported), 1u);
}

TEST_F(OxrPassthroughTest, resumed_event_needs_its_extension)
{
    start();
    XrEventDataBuffer event = {};
    for (u32 i = 0; i < 3; i++)
    {
        XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
        ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
        ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    }
    EXPECT_FALSE(poll_for(XR_TYPE_EVENT_DATA_PASSTHROUGH_LAYER_RESUMED_META, event));
    const XrPassthroughLayerCreateInfoFB hands = { .type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, .passthrough = passthrough, .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_TRACKED_KEYBOARD_HANDS_FB };
    XrPassthroughLayerFB                 other = XR_NULL_HANDLE;
    EXPECT_EQ(pt.xrCreatePassthroughLayerFB(session, &hands, &other), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrPassthroughExtrasTest, keyboard_hands_intensity)
{
    start();
    PFN_xrPassthroughLayerSetKeyboardHandsIntensityFB set_intensity = nullptr;
    ASSERT_EQ(gipa(instance, "xrPassthroughLayerSetKeyboardHandsIntensityFB", (PFN_xrVoidFunction*)&set_intensity), XR_SUCCESS);
    const XrPassthroughLayerPurposeFB purposes[2] = { XR_PASSTHROUGH_LAYER_PURPOSE_TRACKED_KEYBOARD_HANDS_FB, XR_PASSTHROUGH_LAYER_PURPOSE_TRACKED_KEYBOARD_MASKED_HANDS_FB };
    for (const XrPassthroughLayerPurposeFB purpose : purposes)
    {
        const XrPassthroughLayerCreateInfoFB info  = { .type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, .passthrough = passthrough, .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB, .purpose = purpose };
        XrPassthroughLayerFB                 hands = XR_NULL_HANDLE;
        ASSERT_EQ(pt.xrCreatePassthroughLayerFB(session, &info, &hands), XR_SUCCESS);
        XrPassthroughKeyboardHandsIntensityFB intensity = { .type = XR_TYPE_PASSTHROUGH_KEYBOARD_HANDS_INTENSITY_FB, .leftHandIntensity = 0.5f, .rightHandIntensity = 1.0f };
        EXPECT_EQ(set_intensity(hands, &intensity), XR_SUCCESS);
        intensity.rightHandIntensity = 1.5f;
        EXPECT_EQ(set_intensity(hands, &intensity), XR_ERROR_VALIDATION_FAILURE);
        intensity.rightHandIntensity = zm::nan();
        EXPECT_EQ(set_intensity(hands, &intensity), XR_ERROR_VALIDATION_FAILURE);
        EXPECT_EQ(set_intensity(hands, nullptr), XR_ERROR_VALIDATION_FAILURE);
        EXPECT_EQ(pt.xrDestroyPassthroughLayerFB(hands), XR_SUCCESS);
    }
    const XrPassthroughKeyboardHandsIntensityFB intensity = { .type = XR_TYPE_PASSTHROUGH_KEYBOARD_HANDS_INTENSITY_FB, .leftHandIntensity = 0.5f, .rightHandIntensity = 0.5f };
    EXPECT_EQ(set_intensity(layer, &intensity), XR_ERROR_VALIDATION_FAILURE); // a reconstruction layer
    EXPECT_EQ(set_intensity(XR_NULL_HANDLE, &intensity), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrPassthroughExtrasTest, passthrough_preferences_default_to_inactive)
{
    start();
    PFN_xrGetPassthroughPreferencesMETA get = nullptr;
    ASSERT_EQ(gipa(instance, "xrGetPassthroughPreferencesMETA", (PFN_xrVoidFunction*)&get), XR_SUCCESS);
    XrPassthroughPreferencesMETA preferences = { .type = XR_TYPE_PASSTHROUGH_PREFERENCES_META, .flags = 0xFF };
    ASSERT_EQ(get(session, &preferences), XR_SUCCESS);
    EXPECT_EQ(preferences.flags, 0u);
    EXPECT_EQ(get(session, nullptr), XR_ERROR_VALIDATION_FAILURE);
}

namespace
{
struct OxrPassthroughActiveTest : OxrPassthroughExtrasTest
{
    char config[OS_MAX_PATH] = {};

    void SetUp () override
    {
        cstr_format(config, sizeof(config), "%s/puppetxr_passthrough_active.json", temp_dir());
        const char* text = "{ \"passthrough\": { \"default_active\": true } }";
        ASSERT_TRUE(os_write_file(config, text, cstr_length(text)));
        os_set_env("PUPPETXR_CONFIG", config);
        OxrPassthroughExtrasTest::SetUp();
    }

    void TearDown () override
    {
        OxrPassthroughExtrasTest::TearDown();
        os_unset_env("PUPPETXR_CONFIG");
        os_delete_file(config);
    }
};
} // namespace

TEST_F(OxrPassthroughActiveTest, passthrough_preferences_follow_the_config)
{
    start();
    PFN_xrGetPassthroughPreferencesMETA get = nullptr;
    ASSERT_EQ(gipa(instance, "xrGetPassthroughPreferencesMETA", (PFN_xrVoidFunction*)&get), XR_SUCCESS);
    XrPassthroughPreferencesMETA preferences = { .type = XR_TYPE_PASSTHROUGH_PREFERENCES_META };
    ASSERT_EQ(get(session, &preferences), XR_SUCCESS);
    EXPECT_EQ(preferences.flags, (XrPassthroughPreferenceFlagsMETA)XR_PASSTHROUGH_PREFERENCE_DEFAULT_TO_ACTIVE_BIT_META);
}

//
// Layers and swapchains: XR_FB_composition_layer_settings, XR_META_automatic_layer_filter,
// XR_FB_composition_layer_secure_content, XR_FB_swapchain_update_state(_vulkan),
// XR_META_vulkan_swapchain_create_info
//

namespace
{
struct OxrLayerExtrasTest : OxrLayerOptionsTest
{
    OxrLayerExtrasTest ()
    {
        extensions[extensions_count++] = XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME;
        extensions[extensions_count++] = XR_META_AUTOMATIC_LAYER_FILTER_EXTENSION_NAME;
        extensions[extensions_count++] = XR_FB_COMPOSITION_LAYER_SECURE_CONTENT_EXTENSION_NAME;
        extensions[extensions_count++] = XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME;
        extensions[extensions_count++] = XR_FB_SWAPCHAIN_UPDATE_STATE_VULKAN_EXTENSION_NAME;
        extensions[extensions_count++] = XR_META_VULKAN_SWAPCHAIN_CREATE_INFO_EXTENSION_NAME;
    }

    XrResult end_frame (const XrCompositionLayerBaseHeader* const* layers, u32 layers_count)
    {
        XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
        EXPECT_EQ(xr.xrWaitFrame(session, nullptr, &state), XR_SUCCESS);
        EXPECT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
        XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = layers_count, .layers = layers };
        const XrResult result   = xr.xrEndFrame(session, &end_info);
        if (result != XR_SUCCESS)
        {
            end_info.layerCount = 0;
            EXPECT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
        }
        return result;
    }

    // What the headset shows (the mirror window) and what a screenshot gets
    void read_both (const XrCompositionLayerBaseHeader* const* layers, u32 count, Array<u8>& mirror, Array<u8>& capture, u32& width, u32& height)
    {
        submit_and_read(layers, count, capture, width, height);
        mirror.resize(capture.length);
        ASSERT_TRUE(oxr_read_mirror_frame(mirror.data, mirror.length, width, height));
    }
};
} // namespace

TEST_F(OxrLayerExtrasTest, layer_settings_flags_are_validated)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain swapchain  = create_swapchain(32, 32, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    XrCompositionLayerSettingsFB        settings  = { .type = XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB, .layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB | XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB };
    XrCompositionLayerQuad              quad      = centered_quad(swapchain, view_space);
    quad.next                                     = &settings;
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    EXPECT_EQ(end_frame(layers, 1), XR_SUCCESS);
    settings.layerFlags = 0x10; // no such bit
    EXPECT_EQ(end_frame(layers, 1), XR_ERROR_VALIDATION_FAILURE);
    // XR_META_automatic_layer_filter: the automatic bit picks among the others, so it is never alone
    settings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META;
    EXPECT_EQ(end_frame(layers, 1), XR_ERROR_VALIDATION_FAILURE);
    settings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META | XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
    EXPECT_EQ(end_frame(layers, 1), XR_SUCCESS);
}

TEST_F(OxrLayerExtrasTest, secure_layers_are_left_out_of_captures)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    const XrSwapchain background = create_swapchain(64, 64, 2);
    const XrSwapchain overlay    = create_swapchain(32, 32, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(background, { { 0.0f, 0.0f, 1.0f, 1.0f } });
    render_into(overlay, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    XrCompositionLayerProjectionView    views[2]   = { view(background, 0, 64, 64), view(background, 1, 64, 64) };
    XrCompositionLayerProjection        projection = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = view_space, .viewCount = 2, .views = views };
    XrCompositionLayerSecureContentFB   secure     = { .type = XR_TYPE_COMPOSITION_LAYER_SECURE_CONTENT_FB, .flags = XR_COMPOSITION_LAYER_SECURE_CONTENT_EXCLUDE_LAYER_BIT_FB };
    XrCompositionLayerQuad              quad       = centered_quad(overlay, view_space);
    quad.next                                      = &secure;
    const XrCompositionLayerBaseHeader* layers[2]  = { (XrCompositionLayerBaseHeader*)&projection, (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> mirror  = {};
    Array<u8> capture = {};
    defer { mirror.release(); capture.release(); };
    u32 width  = 0;
    u32 height = 0;

    read_both(layers, 2, mirror, capture, width, height);
    EXPECT_EQ(eye_pixel(mirror, width, height, 0, 0.0f, 0.0f)[0], 255); // the headset shows it
    EXPECT_EQ(eye_pixel(capture, width, height, 0, 0.0f, 0.0f)[0], 0);  // a capture shows what is behind
    EXPECT_EQ(eye_pixel(capture, width, height, 0, 0.0f, 0.0f)[2], 255);

    secure.flags = XR_COMPOSITION_LAYER_SECURE_CONTENT_REPLACE_LAYER_BIT_FB;
    read_both(layers, 2, mirror, capture, width, height);
    EXPECT_EQ(eye_pixel(mirror, width, height, 0, 0.0f, 0.0f)[0], 255);
    const u8* replaced = eye_pixel(capture, width, height, 0, 0.0f, 0.0f);
    EXPECT_EQ(replaced[0], 0); // proxy content: opaque black
    EXPECT_EQ(replaced[1], 0);
    EXPECT_EQ(replaced[2], 0);
    EXPECT_EQ(eye_pixel(capture, width, height, 0, 0.7f, 0.7f)[2], 255); // outside the quad, inside the projection

    quad.next = nullptr;
    read_both(layers, 2, mirror, capture, width, height);
    EXPECT_EQ(memcmp(mirror.data, capture.data, mirror.length), 0);

    quad.next    = &secure;
    secure.flags = 0x4;
    EXPECT_EQ(end_frame(layers, 2), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrLayerExtrasTest, swapchain_sampler_state_round_trips_and_filters)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    PFN_xrUpdateSwapchainFB   update    = nullptr;
    PFN_xrGetSwapchainStateFB get_state = nullptr;
    ASSERT_EQ(gipa(instance, "xrUpdateSwapchainFB", (PFN_xrVoidFunction*)&update), XR_SUCCESS);
    ASSERT_EQ(gipa(instance, "xrGetSwapchainStateFB", (PFN_xrVoidFunction*)&get_state), XR_SUCCESS);
    const XrSwapchain swapchain  = create_swapchain(2, 2, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    const u8          red[4]     = { 255, 0, 0, 255 };
    const u8          blue[4]    = { 0, 0, 255, 255 };
    upload_halves(swapchain, 2, 2, red, blue);

    XrSwapchainStateSamplerVulkanFB state = { .type = XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB };
    ASSERT_EQ(get_state(swapchain, (XrSwapchainStateBaseHeaderFB*)&state), XR_SUCCESS);
    EXPECT_EQ(state.minFilter, VK_FILTER_LINEAR);
    EXPECT_EQ(state.magFilter, VK_FILTER_LINEAR);
    EXPECT_EQ(state.wrapModeS, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    EXPECT_EQ(state.swizzleRed, VK_COMPONENT_SWIZZLE_IDENTITY);
    EXPECT_FLOAT_EQ(state.maxAnisotropy, 1.0f);

    // The texel edge is at the quad's center: linear blends red and blue, nearest picks one
    XrCompositionLayerQuad quad = centered_quad(swapchain, view_space);
    quad.subImage.imageRect     = { { 0, 0 }, { 2, 2 } };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    const u8* blended = eye_pixel(pixels, width, height, 0, 0.0f, 0.0f);
    EXPECT_GT(blended[0], 40);
    EXPECT_GT(blended[2], 40);

    state.minFilter    = VK_FILTER_NEAREST;
    state.magFilter    = VK_FILTER_NEAREST;
    state.wrapModeS    = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    state.swizzleAlpha = VK_COMPONENT_SWIZZLE_ONE;
    state.borderColor  = { 0.5f, 0.5f, 0.5f, 1.0f };
    ASSERT_EQ(update(swapchain, (const XrSwapchainStateBaseHeaderFB*)&state), XR_SUCCESS);
    XrSwapchainStateSamplerVulkanFB stored = { .type = XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB };
    ASSERT_EQ(get_state(swapchain, (XrSwapchainStateBaseHeaderFB*)&stored), XR_SUCCESS);
    EXPECT_EQ(stored.magFilter, VK_FILTER_NEAREST);
    EXPECT_EQ(stored.wrapModeS, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    EXPECT_EQ(stored.swizzleAlpha, VK_COMPONENT_SWIZZLE_ONE);
    EXPECT_FLOAT_EQ(stored.borderColor.r, 0.5f);
    submit_and_read(layers, 1, pixels, width, height);
    const u8* nearest = eye_pixel(pixels, width, height, 0, 0.0f, 0.0f);
    EXPECT_TRUE((nearest[0] == 255 && nearest[2] == 0) || (nearest[0] == 0 && nearest[2] == 255)) << (u32)nearest[0] << " " << (u32)nearest[2];

    state.magFilter = (VkFilter)7;
    EXPECT_EQ(update(swapchain, (const XrSwapchainStateBaseHeaderFB*)&state), XR_ERROR_VALIDATION_FAILURE);
    state.magFilter     = VK_FILTER_LINEAR;
    state.maxAnisotropy = 0.5f;
    EXPECT_EQ(update(swapchain, (const XrSwapchainStateBaseHeaderFB*)&state), XR_ERROR_VALIDATION_FAILURE);
    const XrSwapchainStateBaseHeaderFB unknown = { .type = XR_TYPE_SPACE_LOCATION };
    EXPECT_EQ(update(swapchain, &unknown), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(update(XR_NULL_HANDLE, (const XrSwapchainStateBaseHeaderFB*)&state), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(get_state(swapchain, nullptr), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrLayerExtrasTest, extra_vulkan_image_flags)
{
    create_vulkan();
    create_vulkan_session();
    XrVulkanSwapchainCreateInfoMETA extra = { .type = XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META, .additionalCreateFlags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, .additionalUsageFlags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    const XrSwapchainCreateInfo     info  = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .next = &extra, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, .format = VK_FORMAT_R8G8B8A8_SRGB, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
    XrSwapchain                     swapchain = XR_NULL_HANDLE;
    ASSERT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
    // Fragment density maps need a device extension the simulator does not enable
    extra.additionalCreateFlags = VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT;
    EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_ERROR_FEATURE_UNSUPPORTED);
    extra.additionalCreateFlags = 0;
    extra.additionalUsageFlags  = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT;
    EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_ERROR_FEATURE_UNSUPPORTED);
}

TEST_F(OxrVulkanTest, layer_and_swapchain_extras_need_their_extensions)
{
    create_vulkan();
    create_vulkan_session();
    begin_session();
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrUpdateSwapchainFB", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrGetSwapchainStateFB", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);

    // Without their extensions these structs are unknown and ignored
    const XrSwapchain swapchain  = create_swapchain(32, 32, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    render_into(swapchain, { { 1.0f, 0.0f, 0.0f, 1.0f } });
    XrCompositionLayerSettingsFB      settings = { .type = XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB, .layerFlags = 0x10 };
    XrCompositionLayerSecureContentFB secure   = { .type = XR_TYPE_COMPOSITION_LAYER_SECURE_CONTENT_FB, .next = &settings, .flags = XR_COMPOSITION_LAYER_SECURE_CONTENT_EXCLUDE_LAYER_BIT_FB };
    XrCompositionLayerQuad quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .next = &secure, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 1.0f, 1.0f } };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[0], 255);

    XrVulkanSwapchainCreateInfoMETA extra = { .type = XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META, .additionalCreateFlags = VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT };
    const XrSwapchainCreateInfo info = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .next = &extra, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, .format = VK_FORMAT_R8G8B8A8_SRGB, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
    XrSwapchain other = XR_NULL_HANDLE;
    EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &other), XR_SUCCESS);
}

//
// OpenGL and OpenGL ES sessions through XR_MNDX_egl_enable
//

#define TEST_GL_SRGB8_ALPHA8     0x8C43
#define TEST_GL_RGBA8            0x8058
#define TEST_GL_FRAMEBUFFER      0x8D40
#define TEST_GL_COLOR_ATTACHMENT 0x8CE0
#define TEST_GL_COLOR            0x1800
#define TEST_GL_SCISSOR_TEST     0x0C11
#define TEST_GL_TEXTURE_2D       0x0DE1
#define TEST_GL_DEPTH_COMPONENT32F 0x8CAC
#define TEST_GL_DEPTH_ATTACHMENT 0x8D00
#define TEST_GL_DEPTH            0x1801

namespace
{

// The app renders with GL in its own context; GetParam() picks OpenGL ES
struct OxrGLTest : OxrTest, testing::WithParamInterface<b8>
{
    TestGLApp                                app = {};
    PFN_xrGetOpenGLGraphicsRequirementsKHR   get_gl_requirements = nullptr;
    PFN_xrGetOpenGLESGraphicsRequirementsKHR get_gles_requirements = nullptr;

    void (*glGenFramebuffers)         (i32 n, u32* framebuffers) = {};
    void (*glDeleteFramebuffers)      (i32 n, const u32* framebuffers) = {};
    void (*glBindFramebuffer)         (u32 target, u32 framebuffer) = {};
    void (*glFramebufferTexture2D)    (u32 target, u32 attachment, u32 textarget, u32 texture, i32 level) = {};
    void (*glFramebufferTextureLayer) (u32 target, u32 attachment, u32 texture, i32 level, i32 layer) = {};
    void (*glClearBufferfv)           (u32 buffer, i32 drawbuffer, const f32* value) = {};
    void (*glScissor)                 (i32 x, i32 y, i32 width, i32 height) = {};
    void (*glEnable)                  (u32 cap) = {};
    void (*glDisable)                 (u32 cap) = {};
    u8   (*glIsTexture)               (u32 texture) = {};
    void (*glFinish)                  () = {};

    void SetUp () override
    {
        OxrTest::SetUp();
        if (!app.create(GetParam()))
        {
            GTEST_SKIP() << "no EGL context";
        }
        const char* extensions[] = { XR_MNDX_EGL_ENABLE_EXTENSION_NAME, GetParam() ? XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME : XR_KHR_OPENGL_ENABLE_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_FB_COMPOSITION_LAYER_DEPTH_TEST_EXTENSION_NAME };
        ASSERT_EQ(create_instance(extensions, 4), XR_SUCCESS);
        const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
        ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);
        PFN_xrVoidFunction gl = nullptr;
        PFN_xrVoidFunction gles = nullptr;
        EXPECT_EQ(gipa(instance, "xrGetOpenGLGraphicsRequirementsKHR", &gl), GetParam() ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_SUCCESS);
        EXPECT_EQ(gipa(instance, "xrGetOpenGLESGraphicsRequirementsKHR", &gles), GetParam() ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED);
        get_gl_requirements   = (PFN_xrGetOpenGLGraphicsRequirementsKHR)gl;
        get_gles_requirements = (PFN_xrGetOpenGLESGraphicsRequirementsKHR)gles;

        void* (*proc) (const char*) = (void* (*)(const char*))app.get_proc_address;
        glGenFramebuffers         = (decltype(glGenFramebuffers))proc("glGenFramebuffers");
        glDeleteFramebuffers      = (decltype(glDeleteFramebuffers))proc("glDeleteFramebuffers");
        glBindFramebuffer         = (decltype(glBindFramebuffer))proc("glBindFramebuffer");
        glFramebufferTexture2D    = (decltype(glFramebufferTexture2D))proc("glFramebufferTexture2D");
        glFramebufferTextureLayer = (decltype(glFramebufferTextureLayer))proc("glFramebufferTextureLayer");
        glClearBufferfv           = (decltype(glClearBufferfv))proc("glClearBufferfv");
        glScissor                 = (decltype(glScissor))proc("glScissor");
        glEnable                  = (decltype(glEnable))proc("glEnable");
        glDisable                 = (decltype(glDisable))proc("glDisable");
        glIsTexture               = (decltype(glIsTexture))proc("glIsTexture");
        glFinish                  = (decltype(glFinish))proc("glFinish");
        ASSERT_TRUE(glGenFramebuffers && glBindFramebuffer && glFramebufferTexture2D && glFramebufferTextureLayer && glClearBufferfv && glScissor && glIsTexture && glFinish);
    }

    void TearDown () override
    {
        if (session != XR_NULL_HANDLE)
        {
            xr.xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }
        OxrTest::TearDown();
        app.destroy();
        EXPECT_EQ(test_gl_errors, 0u);
    }

    XrGraphicsBindingEGLMNDX binding () const
    {
        return { .type = XR_TYPE_GRAPHICS_BINDING_EGL_MNDX, .getProcAddress = (PFN_xrEglGetProcAddressMNDX)app.get_proc_address, .display = app.display, .config = app.config, .context = app.context };
    }

    void call_requirements () const
    {
        if (GetParam())
        {
            XrGraphicsRequirementsOpenGLESKHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
            ASSERT_EQ(get_gles_requirements(instance, system, &requirements), XR_SUCCESS);
            EXPECT_EQ(requirements.minApiVersionSupported, XR_MAKE_VERSION(3, 0, 0));
            EXPECT_GE(requirements.maxApiVersionSupported, XR_MAKE_VERSION(3, 2, 0));
        }
        else
        {
            XrGraphicsRequirementsOpenGLKHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
            ASSERT_EQ(get_gl_requirements(instance, system, &requirements), XR_SUCCESS);
            EXPECT_EQ(requirements.minApiVersionSupported, XR_MAKE_VERSION(3, 3, 0));
            EXPECT_GE(requirements.maxApiVersionSupported, XR_MAKE_VERSION(4, 6, 0));
        }
    }

    void create_gl_session ()
    {
        call_requirements();
        const TestGLBinding       gl(app);
        const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = gl.chain(), .systemId = system };
        ASSERT_EQ(xr.xrCreateSession(instance, &info, &session), XR_SUCCESS);
    }

    XrSwapchain create_swapchain (u32 width, u32 height, u32 array_size, i64 format = TEST_GL_SRGB8_ALPHA8)
    {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        const XrSwapchainCreateInfo info = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, .format = format, .sampleCount = 1, .width = width, .height = height, .faceCount = 1, .arraySize = array_size, .mipCount = 1 };
        EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_SUCCESS);
        return swapchain;
    }

    // Textures of the swapchain, through the image struct of the session's API
    u32 images (XrSwapchain swapchain, u32* out_textures, u32 capacity)
    {
        u32 count = 0;
        EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr), XR_SUCCESS);
        XrSwapchainImageOpenGLKHR gl[8] = {};
        for (XrSwapchainImageOpenGLKHR& image : gl)
        {
            image.type = GetParam() ? XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR : XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        }
        EXPECT_LE(count, 8u);
        EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, count, &count, (XrSwapchainImageBaseHeader*)gl), XR_SUCCESS);
        for (u32 i = 0; i < count && i < capacity; i++)
        {
            out_textures[i] = gl[i].image;
        }
        return count;
    }

    // Acquires and renders as a GL app does: `bottom` over the lower half of every layer, `top`
    // over the upper half. GL's image origin is the bottom-left corner.
    void render_into (XrSwapchain swapchain, u32 width, u32 height, u32 layers, const f32 bottom[4], const f32 top[4])
    {
        u32       textures[8] = {};
        const u32 count       = images(swapchain, textures, 8);
        u32       index       = 0;
        EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index), XR_SUCCESS);
        const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);
        ASSERT_LT(index, count);

        u32 framebuffer = 0;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(TEST_GL_FRAMEBUFFER, framebuffer);
        glEnable(TEST_GL_SCISSOR_TEST);
        for (u32 layer = 0; layer < layers; layer++)
        {
            if (layers == 1)
            {
                glFramebufferTexture2D(TEST_GL_FRAMEBUFFER, TEST_GL_COLOR_ATTACHMENT, TEST_GL_TEXTURE_2D, textures[index], 0);
            }
            else
            {
                glFramebufferTextureLayer(TEST_GL_FRAMEBUFFER, TEST_GL_COLOR_ATTACHMENT, textures[index], 0, (i32)layer);
            }
            glScissor(0, 0, (i32)width, (i32)height / 2);
            glClearBufferfv(TEST_GL_COLOR, 0, bottom);
            glScissor(0, (i32)height / 2, (i32)width, (i32)height / 2);
            glClearBufferfv(TEST_GL_COLOR, 0, top);
        }
        glDisable(TEST_GL_SCISSOR_TEST);
        glBindFramebuffer(TEST_GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
    }

    // The same for a depth swapchain of array layers: `bottom` depth below, `top` above
    void depth_into (XrSwapchain swapchain, u32 width, u32 height, u32 layers, f32 bottom, f32 top)
    {
        u32       textures[8] = {};
        const u32 count       = images(swapchain, textures, 8);
        u32       index       = 0;
        EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index), XR_SUCCESS);
        const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);
        ASSERT_LT(index, count);

        u32 framebuffer = 0;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(TEST_GL_FRAMEBUFFER, framebuffer);
        glEnable(TEST_GL_SCISSOR_TEST);
        for (u32 layer = 0; layer < layers; layer++)
        {
            glFramebufferTextureLayer(TEST_GL_FRAMEBUFFER, TEST_GL_DEPTH_ATTACHMENT, textures[index], 0, (i32)layer);
            glScissor(0, 0, (i32)width, (i32)height / 2);
            glClearBufferfv(TEST_GL_DEPTH, 0, &bottom);
            glScissor(0, (i32)height / 2, (i32)width, (i32)height / 2);
            glClearBufferfv(TEST_GL_DEPTH, 0, &top);
        }
        glDisable(TEST_GL_SCISSOR_TEST);
        glBindFramebuffer(TEST_GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
    }
};

} // namespace

namespace
{
struct GLSessionThread
{
    OxrGLTest* test;
    XrResult   result;
};
} // namespace

static i32 create_gl_session_on_thread (void* user)
{
    GLSessionThread&          job  = *(GLSessionThread*)user;
    const TestGLBinding       gl(job.test->app);
    const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = gl.chain(), .systemId = job.test->system };
    job.result                     = job.test->xr.xrCreateSession(job.test->instance, &info, &job.test->session);
    return 0;
}

// The CTS multithreading test creates sessions on threads that have no context current, where
// wglGetProcAddress finds nothing
TEST_P(OxrGLTest, sessions_are_created_on_a_thread_without_a_current_context)
{
    call_requirements();
    GLSessionThread job    = { .test = this, .result = XR_ERROR_RUNTIME_FAILURE };
    OS_Thread*      thread = os_thread_create("gl_session", create_gl_session_on_thread, &job);
    ASSERT_NE(thread, nullptr);
    os_thread_join(thread);
    EXPECT_EQ(job.result, XR_SUCCESS);
    const XrSwapchain swapchain = create_swapchain(16, 16, 1);
    EXPECT_NE(swapchain, XR_NULL_HANDLE);
    xr.xrDestroySwapchain(swapchain);
}

TEST_P(OxrGLTest, extensions_are_offered)
{
    EXPECT_TRUE(extension_offered(xr, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME));
    EXPECT_TRUE(extension_offered(xr, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME));
    EXPECT_TRUE(extension_offered(xr, XR_MNDX_EGL_ENABLE_EXTENSION_NAME));
}

TEST_P(OxrGLTest, requirements_validate_their_arguments)
{
    if (GetParam())
    {
        XrGraphicsRequirementsOpenGLESKHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
        EXPECT_EQ(get_gles_requirements(instance, 42, &requirements), XR_ERROR_SYSTEM_INVALID);
        EXPECT_EQ(get_gles_requirements(instance, system, nullptr), XR_ERROR_VALIDATION_FAILURE);
        requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;
        EXPECT_EQ(get_gles_requirements(instance, system, &requirements), XR_ERROR_VALIDATION_FAILURE);
        EXPECT_EQ(get_gles_requirements(garbage_instance(), system, &requirements), XR_ERROR_HANDLE_INVALID);
    }
    else
    {
        XrGraphicsRequirementsOpenGLKHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
        EXPECT_EQ(get_gl_requirements(instance, 42, &requirements), XR_ERROR_SYSTEM_INVALID);
        EXPECT_EQ(get_gl_requirements(instance, system, nullptr), XR_ERROR_VALIDATION_FAILURE);
        requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
        EXPECT_EQ(get_gl_requirements(instance, system, &requirements), XR_ERROR_VALIDATION_FAILURE);
        EXPECT_EQ(get_gl_requirements(garbage_instance(), system, &requirements), XR_ERROR_HANDLE_INVALID);
    }
}

TEST_P(OxrGLTest, session_needs_the_requirements_call_and_a_complete_binding)
{
    if (app.wgl)
    {
        TestGLBinding             gl(app);
        const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = gl.chain(), .systemId = system };
        EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
        call_requirements();
        gl.win32.dc = nullptr;
        EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
        gl.win32.dc      = app.dc;
        gl.win32.context = nullptr;
        EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
        EXPECT_EQ(session, XR_NULL_HANDLE);
        gl.win32.context = app.context;
        ASSERT_EQ(xr.xrCreateSession(instance, &info, &session), XR_SUCCESS);
        return;
    }
    XrGraphicsBindingEGLMNDX  egl  = binding();
    const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &egl, .systemId = system };
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
    EXPECT_EQ(session, XR_NULL_HANDLE);
    call_requirements();

    egl.context = nullptr;
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    egl = binding();
    egl.display = nullptr;
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    egl = binding();
    egl.getProcAddress = nullptr;
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    EXPECT_EQ(session, XR_NULL_HANDLE);

    egl = binding();
    ASSERT_EQ(xr.xrCreateSession(instance, &info, &session), XR_SUCCESS);
    EXPECT_NE(session, XR_NULL_HANDLE);
}

// The other API's extension is not enabled, so a context of that API is no device for this instance
TEST_P(OxrGLTest, context_of_the_other_api_is_rejected)
{
    TestGLApp other = {};
    if (app.wgl)
    {
        GTEST_SKIP() << "WGL has no OpenGL ES";
    }
    ASSERT_TRUE(other.create(!GetParam()));
    defer { other.destroy(); };
    call_requirements();
    const XrGraphicsBindingEGLMNDX egl  = { .type = XR_TYPE_GRAPHICS_BINDING_EGL_MNDX, .getProcAddress = (PFN_xrEglGetProcAddressMNDX)other.get_proc_address, .display = other.display, .config = other.config, .context = other.context };
    const XrSessionCreateInfo      info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &egl, .systemId = system };
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
}

TEST_P(OxrGLTest, swapchains_are_gl_textures_of_gl_formats)
{
    create_gl_session();
    u32 count = 0;
    EXPECT_EQ(xr.xrEnumerateSwapchainFormats(session, 0, &count, nullptr), XR_SUCCESS);
    int64_t formats[16] = {};
    ASSERT_LE(count, 16u);
    EXPECT_EQ(xr.xrEnumerateSwapchainFormats(session, 16, &count, formats), XR_SUCCESS);
    ASSERT_GT(count, 0u);
    EXPECT_EQ(formats[0], TEST_GL_SRGB8_ALPHA8);
    for (u32 i = 0; i < count; i++)
    {
        EXPECT_NE(formats[i], 0);
    }

    const XrSwapchain swapchain = create_swapchain(32, 32, 1, TEST_GL_RGBA8);
    ASSERT_NE(swapchain, XR_NULL_HANDLE);
    u32       textures[8] = {};
    const u32 images_count = images(swapchain, textures, 8);
    EXPECT_EQ(images_count, 3u);
    for (u32 i = 0; i < images_count; i++)
    {
        EXPECT_NE(textures[i], 0u);
        EXPECT_TRUE(glIsTexture(textures[i])) << "the texture lives in the app's share group";
    }

    // The image struct must be the session's API's
    XrSwapchainImageOpenGLKHR wrong[3] = {};
    for (XrSwapchainImageOpenGLKHR& image : wrong)
    {
        image.type = GetParam() ? XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR : XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    u32 written = 0;
    EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, 3, &written, (XrSwapchainImageBaseHeader*)wrong), XR_ERROR_VALIDATION_FAILURE);

    const XrSwapchainCreateInfo bgra = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, .format = 0x93A1, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
    XrSwapchain other = XR_NULL_HANDLE;
    EXPECT_EQ(xr.xrCreateSwapchain(session, &bgra, &other), XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
    const XrSwapchainCreateInfo cube_array = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT, .format = TEST_GL_RGBA8, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 6, .arraySize = 2, .mipCount = 1 };
    EXPECT_EQ(xr.xrCreateSwapchain(session, &cube_array, &other), XR_ERROR_FEATURE_UNSUPPORTED);
    const XrSwapchainCreateInfo cube = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT, .format = TEST_GL_RGBA8, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 6, .arraySize = 1, .mipCount = 1 };
    EXPECT_EQ(xr.xrCreateSwapchain(session, &cube, &other), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(other), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

// GL images have their origin at the bottom: the app's upper half must show on top
TEST_P(OxrGLTest, projection_layers_are_composited_upright)
{
    create_gl_session();
    begin_session();
    const XrSwapchain swapchain = create_swapchain(64, 64, 2);
    const XrSpace     stage     = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const f32         green[4]  = { 0.0f, 1.0f, 0.0f, 1.0f };
    const f32         red[4]    = { 1.0f, 0.0f, 0.0f, 1.0f };
    render_into(swapchain, 64, 64, 2, green, red);

    XrCompositionLayerProjectionView views[2] = { view(swapchain, 0, 64, 64), view(swapchain, 1, 64, 64) };
    const XrCompositionLayerProjection layer = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = stage, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (const XrCompositionLayerBaseHeader*)&layer };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    for (u32 eye = 0; eye < 2; eye++)
    {
        const u8* upper = eye_pixel(pixels, width, height, eye, 0.0f, -0.3f);
        const u8* lower = eye_pixel(pixels, width, height, eye, 0.0f, 0.3f);
        EXPECT_EQ(upper[0], 255) << "eye " << eye;
        EXPECT_EQ(upper[1], 0) << "eye " << eye;
        EXPECT_EQ(lower[0], 0) << "eye " << eye;
        EXPECT_EQ(lower[1], 255) << "eye " << eye;
    }

    // A second frame with other colours replaces the first: the compositor sees the app's latest rendering
    const f32 blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    render_into(swapchain, 64, 64, 2, blue, blue);
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, -0.3f)[2], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, -0.3f)[0], 0);
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

// XR_FB_composition_layer_depth_test: GL depth images have their origin at the bottom left too
TEST_P(OxrGLTest, projection_depth_is_tested_upright)
{
    create_gl_session();
    begin_session();
    const XrSpace     local  = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    const XrSwapchain red    = create_swapchain(64, 64, 2);
    const XrSwapchain blue   = create_swapchain(64, 64, 2);
    const XrSwapchain depth1 = create_swapchain(64, 64, 2, TEST_GL_DEPTH_COMPONENT32F);
    const XrSwapchain depth2 = create_swapchain(64, 64, 2, TEST_GL_DEPTH_COMPONENT32F);
    const f32         r[4]   = { 1.0f, 0.0f, 0.0f, 1.0f };
    const f32         b[4]   = { 0.0f, 0.0f, 1.0f, 1.0f };
    render_into(red, 64, 64, 2, r, r);
    render_into(blue, 64, 64, 2, b, b);
    depth_into(depth1, 64, 64, 2, 0.2f, 0.8f);
    depth_into(depth2, 64, 64, 2, 0.8f, 0.2f);

    XrCompositionLayerDepthInfoKHR   depth[2][2] = {};
    XrCompositionLayerProjectionView views[2][2] = { { view(red, 0, 64, 64), view(red, 1, 64, 64) }, { view(blue, 0, 64, 64), view(blue, 1, 64, 64) } };
    const XrSwapchain                depths[2]   = { depth1, depth2 };
    const XrCompositionLayerDepthTestFB test     = { .type = XR_TYPE_COMPOSITION_LAYER_DEPTH_TEST_FB, .depthMask = XR_TRUE, .compareOp = XR_COMPARE_OP_LESS_FB };
    XrCompositionLayerProjection     layer[2]    = {};
    for (u32 l = 0; l < 2; l++)
    {
        for (u32 v = 0; v < 2; v++)
        {
            depth[l][v]      = { .type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR, .subImage = { .swapchain = depths[l], .imageRect = { { 0, 0 }, { 64, 64 } }, .imageArrayIndex = v }, .minDepth = 0.0f, .maxDepth = 1.0f, .nearZ = 0.1f, .farZ = 100.0f };
            views[l][v].next = &depth[l][v];
        }
        layer[l] = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .next = &test, .space = local, .viewCount = 2, .views = views[l] };
    }
    const XrCompositionLayerBaseHeader* layers[2] = { (const XrCompositionLayerBaseHeader*)&layer[0], (const XrCompositionLayerBaseHeader*)&layer[1] };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 2, pixels, width, height);
    for (u32 eye = 0; eye < 2; eye++)
    {
        const u8* upper = eye_pixel(pixels, width, height, eye, 0.0f, -0.3f);
        const u8* lower = eye_pixel(pixels, width, height, eye, 0.0f, 0.3f);
        EXPECT_EQ(upper[2], 255) << "eye " << eye;
        EXPECT_EQ(upper[0], 0) << "eye " << eye;
        EXPECT_EQ(lower[0], 255) << "eye " << eye;
        EXPECT_EQ(lower[2], 0) << "eye " << eye;
    }
}

namespace
{
// With a mirror window, here SDL's offscreen one
struct OxrGLWindowedTest : OxrGLTest
{
    OxrGLWindowedTest ()
    {
        headless = false;
    }
};
} // namespace

// A window without a native window to draw into leaves the mirror blank; compositing goes on
TEST_P(OxrGLWindowedTest, sessions_without_a_native_window_composite_offscreen)
{
    create_gl_session();
    begin_session();
    const XrSwapchain swapchain = create_swapchain(32, 32, 1);
    const XrSpace     view      = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    const f32         red[4]    = { 1.0f, 0.0f, 0.0f, 1.0f };
    render_into(swapchain, 32, 32, 1, red, red);
    const XrCompositionLayerQuad quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 1.0f, 1.0f } };
    const XrCompositionLayerBaseHeader* layers[1] = { (const XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[0], 255);
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

static std::string gl_api_name (const testing::TestParamInfo<b8>& param)
{
    return param.param ? "OpenGLES" : "OpenGL";
}

INSTANTIATE_TEST_SUITE_P(Apis, OxrGLTest, testing::Values(false, true), gl_api_name);
INSTANTIATE_TEST_SUITE_P(Apis, OxrGLWindowedTest, testing::Values(false, true), gl_api_name);

namespace
{
struct OxrWelcomeTest : OxrTest
{
    char state_dir[OS_MAX_PATH]  = {};
    char state_file[OS_MAX_PATH] = {};
    char saved_dir[OS_MAX_PATH]  = {};
    b8   had_saved_dir           = false;

    OxrWelcomeTest ()
    {
        headless = false;
    }

    void SetUp () override
    {
        OxrTest::SetUp();
        const char* saved = os_env("PUPPETXR_STATE_DIR");
        had_saved_dir     = saved != nullptr;
        cstr_copy(saved_dir, sizeof(saved_dir), saved ? saved : "");
        cstr_format(state_dir, sizeof(state_dir), "%s/welcome_state_%u", PUPPETXR_TEST_OUTPUT_DIR, os_process_id());
        cstr_format(state_file, sizeof(state_file), "%s/state.json", state_dir);
        os_set_env("PUPPETXR_STATE_DIR", state_dir);
        os_delete_file(state_file);
    }

    void TearDown () override
    {
        OxrTest::TearDown();
        os_delete_file(state_file);
        had_saved_dir ? os_set_env("PUPPETXR_STATE_DIR", saved_dir) : os_unset_env("PUPPETXR_STATE_DIR");
        os_unset_env("PUPPETXR_SHOW_WELCOME");
    }

    void restart ()
    {
        if (instance != XR_NULL_HANDLE)
        {
            xr.xrDestroyInstance(instance);
            instance = XR_NULL_HANDLE;
        }
        ASSERT_EQ(create_instance(nullptr, 0), XR_SUCCESS);
    }
};
} // namespace

TEST_F(OxrWelcomeTest, welcome_shows_at_start_until_dismissed_for_good)
{
    restart();
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    EXPECT_TRUE(oxr_welcome_shown());
    EXPECT_FALSE(oxr_welcome_key(OS_Key::W));

    // Escape closes it for now only
    EXPECT_TRUE(oxr_welcome_key(OS_Key::Escape));
    EXPECT_FALSE(oxr_welcome_shown());
    EXPECT_FALSE(oxr_welcome_key(OS_Key::Escape));
    EXPECT_FALSE(oxr_welcome_key(OS_Key::Enter));
    EXPECT_FALSE(os_file_exists(state_file));
    restart();
    EXPECT_TRUE(oxr_welcome_shown());

    // F1 toggles it at any time
    EXPECT_TRUE(oxr_welcome_key(OS_Key::F1));
    EXPECT_FALSE(oxr_welcome_shown());
    EXPECT_TRUE(oxr_welcome_key(OS_Key::F1));
    EXPECT_TRUE(oxr_welcome_shown());

    // Enter closes it for good
    EXPECT_TRUE(oxr_welcome_key(OS_Key::Enter));
    EXPECT_FALSE(oxr_welcome_shown());
    SIM_State state = {};
    ASSERT_TRUE(sim_load_state(state_file, state));
    EXPECT_TRUE(state.welcome_dismissed);
    restart();
    EXPECT_FALSE(oxr_welcome_shown());
    EXPECT_TRUE(oxr_welcome_key(OS_Key::F1));
    EXPECT_TRUE(oxr_welcome_shown());
}

// Regression: Enter submitting the virtual keyboard's text field dismissed the overlay for good
TEST_F(OxrWelcomeTest, keys_typed_into_a_text_field_leave_the_welcome_alone)
{
    restart();
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    ASSERT_TRUE(oxr_welcome_shown());
    EXPECT_FALSE(oxr_welcome_key(OS_Key::Enter, true));
    EXPECT_FALSE(oxr_welcome_key(OS_Key::Escape, true));
    EXPECT_TRUE(oxr_welcome_shown());
    EXPECT_FALSE(os_file_exists(state_file));
    EXPECT_TRUE(oxr_welcome_key(OS_Key::F1, true));
    EXPECT_FALSE(oxr_welcome_shown());
}

TEST_F(OxrWelcomeTest, welcome_can_be_switched_off_for_development)
{
    os_set_env("PUPPETXR_SHOW_WELCOME", "0");
    restart();
    if (os_is_headless())
    {
        GTEST_SKIP() << "no offscreen video driver";
    }
    EXPECT_FALSE(oxr_welcome_shown());
    EXPECT_TRUE(oxr_welcome_key(OS_Key::F1));
    EXPECT_TRUE(oxr_welcome_shown());
    EXPECT_FALSE(os_file_exists(state_file));
}

TEST_F(OxrWelcomeTest, headless_runs_never_show_the_welcome)
{
    os_set_env("PUPPETXR_HEADLESS", "1");
    restart();
    os_unset_env("PUPPETXR_HEADLESS");
    EXPECT_FALSE(oxr_welcome_shown());
}

//
// XR_KHR_D3D11_enable and XR_KHR_D3D12_enable
//

namespace
{

// The structs of openxr_platform.h without the D3D headers it needs
struct TestD3DRequirements
{
    XrStructureType type;
    void*           next;
    u32             luid_low;
    i32             luid_high;
    u32             min_feature_level;
};

struct TestD3DBinding
{
    XrStructureType type;
    const void*     next;
    void*           device;
    void*           queue; // D3D12 only
};

struct TestD3DImage
{
    XrStructureType type;
    void*           next;
    void*           texture;
};

typedef XrResult (XRAPI_PTR* PFN_TestD3DRequirements) (XrInstance instance, XrSystemId system, TestD3DRequirements* requirements);

#define TEST_DXGI_R8G8B8A8_TYPELESS   27
#define TEST_DXGI_R8G8B8A8_UNORM      28
#define TEST_DXGI_R8G8B8A8_UNORM_SRGB 29
#define TEST_DXGI_D32_FLOAT           40
#define TEST_DXGI_R32_TYPELESS        39
#define TEST_DXGI_D32_FLOAT_S8X24_UINT 20

// The app renders with D3D11 or, for GetParam(), D3D12
struct OxrD3DTest : OxrTest, testing::WithParamInterface<b8>
{
    TestD3DApp              app             = {};
    PFN_TestD3DRequirements requirements_fn = nullptr;
    const char*             extra_extension = nullptr; // enabled besides the binding's and depth's

    // The runtime takes the real XrGraphicsRequirementsD3D1xKHR, which UBSan's function check
    // tells apart from the stand-in of the same layout
#if defined(__clang__)
    __attribute__((no_sanitize("function")))
#endif
    XrResult requirements (XrInstance inst, XrSystemId id, TestD3DRequirements* out) const
    {
        return requirements_fn(inst, id, out);
    }

    void SetUp () override
    {
        OxrTest::SetUp();
        if (!r_backend_available(GetParam() ? R_Backend::D3D12 : R_Backend::D3D11) || !app.create(GetParam()))
        {
            GTEST_SKIP() << "no D3D device";
        }
        const char* extensions[] = { GetParam() ? "XR_KHR_D3D12_enable" : "XR_KHR_D3D11_enable", XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_FB_COMPOSITION_LAYER_DEPTH_TEST_EXTENSION_NAME, extra_extension };
        ASSERT_EQ(create_instance(extensions, extra_extension ? 4 : 3), XR_SUCCESS);
        const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
        ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);
        PFN_xrVoidFunction function = nullptr;
        ASSERT_EQ(gipa(instance, GetParam() ? "xrGetD3D12GraphicsRequirementsKHR" : "xrGetD3D11GraphicsRequirementsKHR", &function), XR_SUCCESS);
        requirements_fn = (PFN_TestD3DRequirements)function;
        PFN_xrVoidFunction other = nullptr;
        EXPECT_EQ(gipa(instance, GetParam() ? "xrGetD3D11GraphicsRequirementsKHR" : "xrGetD3D12GraphicsRequirementsKHR", &other), XR_ERROR_FUNCTION_UNSUPPORTED);
    }

    void TearDown () override
    {
        if (session != XR_NULL_HANDLE)
        {
            xr.xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }
        OxrTest::TearDown();
        app.destroy();
    }

    XrStructureType requirements_type () const
    {
        return GetParam() ? XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR : XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
    }

    TestD3DBinding binding () const
    {
        return { .type = GetParam() ? XR_TYPE_GRAPHICS_BINDING_D3D12_KHR : XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, .device = app.device, .queue = app.queue };
    }

    void create_d3d_session ()
    {
        TestD3DRequirements result = { .type = requirements_type() };
        ASSERT_EQ(requirements(instance, system, &result), XR_SUCCESS);
        const TestD3DBinding      d3d  = binding();
        const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &d3d, .systemId = system };
        ASSERT_EQ(xr.xrCreateSession(instance, &info, &session), XR_SUCCESS);
    }

    XrSwapchain create_swapchain (u32 width, u32 height, u32 array_size, i64 format = TEST_DXGI_R8G8B8A8_UNORM_SRGB, XrSwapchainUsageFlags usage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT)
    {
        XrSwapchain                 swapchain = XR_NULL_HANDLE;
        const XrSwapchainCreateInfo info      = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = usage, .format = format, .sampleCount = 1, .width = width, .height = height, .faceCount = 1, .arraySize = array_size, .mipCount = 1 };
        EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_SUCCESS);
        return swapchain;
    }

    u32 images (XrSwapchain swapchain, u64* out_textures, u32 capacity)
    {
        u32 count = 0;
        EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr), XR_SUCCESS);
        TestD3DImage d3d[8] = {};
        for (TestD3DImage& image : d3d)
        {
            image.type = GetParam() ? XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR : XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        }
        EXPECT_LE(count, 8u);
        EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, count, &count, (XrSwapchainImageBaseHeader*)d3d), XR_SUCCESS);
        for (u32 i = 0; i < count && i < capacity; i++)
        {
            out_textures[i] = (u64)(uintptr_t)d3d[i].texture;
        }
        return count;
    }

    void render_into (XrSwapchain swapchain, u32 width, u32 height, u32 layers, const f32 top[4], const f32 bottom[4])
    {
        u64       textures[8] = {};
        const u32 count       = images(swapchain, textures, 8);
        u32       index       = 0;
        EXPECT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index), XR_SUCCESS);
        const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        EXPECT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);
        ASSERT_LT(index, count);
        EXPECT_TRUE(app.fill_halves(textures[index], width, height, layers, TEST_DXGI_R8G8B8A8_UNORM_SRGB, top, bottom));
        EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
    }
};

} // namespace

TEST_F(OxrTest, d3d_extensions_are_offered_where_the_build_has_d3d)
{
    EXPECT_EQ(extension_offered(xr, "XR_KHR_D3D11_enable"), r_backend_available(R_Backend::D3D11));
    EXPECT_EQ(extension_offered(xr, "XR_KHR_D3D12_enable"), r_backend_available(R_Backend::D3D12));
}

TEST_F(OxrTest, gl_extensions_are_offered_where_the_build_has_gl)
{
    EXPECT_EQ(extension_offered(xr, "XR_KHR_opengl_enable"), r_backend_available(R_Backend::OpenGL));
    EXPECT_EQ(extension_offered(xr, "XR_MNDX_egl_enable"), r_backend_available(R_Backend::OpenGL));
    EXPECT_EQ(extension_offered(xr, "XR_KHR_opengl_es_enable"), r_backend_available(R_Backend::OpenGLES));
}

TEST_P(OxrD3DTest, requirements_name_the_adapter_and_validate_their_arguments)
{
    TestD3DRequirements result = { .type = requirements_type() };
    ASSERT_EQ(requirements(instance, system, &result), XR_SUCCESS);
    u64 luid = 0;
    ASSERT_TRUE(r_d3d_adapter_luid(luid));
    EXPECT_EQ(result.luid_low, (u32)luid);
    EXPECT_EQ(result.luid_high, (i32)(u32)(luid >> 32));
    EXPECT_EQ(result.min_feature_level, 0xb000u); // D3D_FEATURE_LEVEL_11_0

    EXPECT_EQ(requirements(instance, 42, &result), XR_ERROR_SYSTEM_INVALID);
    EXPECT_EQ(requirements(instance, system, nullptr), XR_ERROR_VALIDATION_FAILURE);
    result.type = GetParam() ? XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR : XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
    EXPECT_EQ(requirements(instance, system, &result), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(requirements(garbage_instance(), system, &result), XR_ERROR_HANDLE_INVALID);
}

TEST_P(OxrD3DTest, session_needs_the_requirements_call_and_a_complete_binding)
{
    TestD3DBinding            d3d  = binding();
    const XrSessionCreateInfo info = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &d3d, .systemId = system };
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
    TestD3DRequirements result = { .type = requirements_type() };
    ASSERT_EQ(requirements(instance, system, &result), XR_SUCCESS);

    d3d.device = nullptr;
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    if (GetParam())
    {
        d3d       = binding();
        d3d.queue = nullptr;
        EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    }
    EXPECT_EQ(session, XR_NULL_HANDLE);

    // A binding of the API whose extension is not enabled is no graphics binding for this instance
    d3d      = binding();
    d3d.type = GetParam() ? XR_TYPE_GRAPHICS_BINDING_D3D11_KHR : XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);

    d3d = binding();
    ASSERT_EQ(xr.xrCreateSession(instance, &info, &session), XR_SUCCESS);
    EXPECT_NE(session, XR_NULL_HANDLE);
}

// Swapchain images are typeless, so that apps view them as sRGB or linear as they choose
TEST_P(OxrD3DTest, swapchains_are_typeless_textures_of_dxgi_formats)
{
    create_d3d_session();
    u32 count = 0;
    EXPECT_EQ(xr.xrEnumerateSwapchainFormats(session, 0, &count, nullptr), XR_SUCCESS);
    int64_t formats[16] = {};
    ASSERT_LE(count, 16u);
    EXPECT_EQ(xr.xrEnumerateSwapchainFormats(session, 16, &count, formats), XR_SUCCESS);
    ASSERT_GT(count, 0u);
    EXPECT_EQ(formats[0], TEST_DXGI_R8G8B8A8_UNORM_SRGB);

    const XrSwapchain swapchain = create_swapchain(32, 16, 2, TEST_DXGI_R8G8B8A8_UNORM);
    ASSERT_NE(swapchain, XR_NULL_HANDLE);
    u64       textures[8] = {};
    const u32 images_count = images(swapchain, textures, 8);
    EXPECT_EQ(images_count, 3u);
    for (u32 i = 0; i < images_count; i++)
    {
        u32 width = 0;
        u32 height = 0;
        u32 layers = 0;
        u32 format = 0;
        ASSERT_TRUE(app.describe(textures[i], width, height, layers, format));
        EXPECT_EQ(width, 32u);
        EXPECT_EQ(height, 16u);
        EXPECT_EQ(layers, 2u);
        EXPECT_EQ(format, (u32)TEST_DXGI_R8G8B8A8_TYPELESS);
    }

    const XrSwapchain depth = create_swapchain(16, 16, 1, TEST_DXGI_D32_FLOAT, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    ASSERT_NE(depth, XR_NULL_HANDLE);
    ASSERT_EQ(images(depth, textures, 8), 3u);
    u32 width = 0;
    u32 height = 0;
    u32 layers = 0;
    u32 format = 0;
    ASSERT_TRUE(app.describe(textures[0], width, height, layers, format));
    EXPECT_EQ(format, (u32)TEST_DXGI_R32_TYPELESS);

    // The image struct must be the session's API's
    TestD3DImage wrong[3] = {};
    for (TestD3DImage& image : wrong)
    {
        image.type = GetParam() ? XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR : XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
    }
    u32 written = 0;
    EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, 3, &written, (XrSwapchainImageBaseHeader*)wrong), XR_ERROR_VALIDATION_FAILURE);

    XrSwapchain                 other   = XR_NULL_HANDLE;
    const XrSwapchainCreateInfo typeless = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, .format = TEST_DXGI_R8G8B8A8_TYPELESS, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
    EXPECT_EQ(xr.xrCreateSwapchain(session, &typeless, &other), XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
    const XrSwapchainCreateInfo cube_array = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT, .format = TEST_DXGI_R8G8B8A8_UNORM, .sampleCount = 1, .width = 16, .height = 16, .faceCount = 6, .arraySize = 2, .mipCount = 1 };
    EXPECT_EQ(xr.xrCreateSwapchain(session, &cube_array, &other), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(other), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(depth), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

// The CTS SwapchainsUnorderedAccess test binds the images as UAVs for a compute shader
TEST_P(OxrD3DTest, unordered_access_usage_makes_writable_images)
{
    create_d3d_session();
    const XrSwapchain plain     = create_swapchain(16, 16, 1, TEST_DXGI_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain writable  = create_swapchain(16, 16, 1, TEST_DXGI_R8G8B8A8_UNORM_SRGB, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT);
    ASSERT_NE(plain, XR_NULL_HANDLE);
    ASSERT_NE(writable, XR_NULL_HANDLE);
    u64 textures[8] = {};
    ASSERT_EQ(images(plain, textures, 8), 3u);
    EXPECT_FALSE(app.allows_unordered_access(textures[0]));
    ASSERT_EQ(images(writable, textures, 8), 3u);
    for (u32 i = 0; i < 3; i++)
    {
        EXPECT_TRUE(app.allows_unordered_access(textures[i])) << i;
    }
    EXPECT_EQ(xr.xrDestroySwapchain(writable), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(plain), XR_SUCCESS);
}

// OVRPlugin's own D3D11 path asks for a sampled 32-bit float depth with stencil; without it it
// had no eye buffers and showed nothing
TEST_P(OxrD3DTest, float_depth_with_stencil_is_offered_and_sampled)
{
    create_d3d_session();
    u32 count = 0;
    ASSERT_EQ(xr.xrEnumerateSwapchainFormats(session, 0, &count, nullptr), XR_SUCCESS);
    int64_t formats[16] = {};
    ASSERT_LE(count, 16u);
    ASSERT_EQ(xr.xrEnumerateSwapchainFormats(session, 16, &count, formats), XR_SUCCESS);
    b8 listed = false;
    for (u32 i = 0; i < count; i++)
    {
        listed |= formats[i] == TEST_DXGI_D32_FLOAT_S8X24_UINT;
    }
    EXPECT_TRUE(listed);
    const XrSwapchain depth = create_swapchain(16, 16, 1, TEST_DXGI_D32_FLOAT_S8X24_UINT, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
    ASSERT_NE(depth, XR_NULL_HANDLE);
    EXPECT_EQ(xr.xrDestroySwapchain(depth), XR_SUCCESS);
}

// D3D images have their origin at the top as Vulkan's: the app's upper half shows on top
TEST_P(OxrD3DTest, projection_layers_are_composited_upright)
{
    create_d3d_session();
    begin_session();
    const XrSwapchain swapchain = create_swapchain(64, 64, 2);
    const XrSpace     stage     = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const f32         green[4]  = { 0.0f, 1.0f, 0.0f, 1.0f };
    const f32         red[4]    = { 1.0f, 0.0f, 0.0f, 1.0f };
    render_into(swapchain, 64, 64, 2, red, green);

    XrCompositionLayerProjectionView views[2] = { view(swapchain, 0, 64, 64), view(swapchain, 1, 64, 64) };
    const XrCompositionLayerProjection layer = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = stage, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (const XrCompositionLayerBaseHeader*)&layer };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    for (u32 eye = 0; eye < 2; eye++)
    {
        const u8* upper = eye_pixel(pixels, width, height, eye, 0.0f, -0.3f);
        const u8* lower = eye_pixel(pixels, width, height, eye, 0.0f, 0.3f);
        EXPECT_EQ(upper[0], 255) << "eye " << eye;
        EXPECT_EQ(upper[1], 0) << "eye " << eye;
        EXPECT_EQ(lower[0], 0) << "eye " << eye;
        EXPECT_EQ(lower[1], 255) << "eye " << eye;
    }

    // A second frame with other colours replaces the first: the compositor sees the app's latest rendering
    const f32 blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    render_into(swapchain, 64, 64, 2, blue, blue);
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, -0.3f)[2], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, -0.3f)[0], 0);
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

// XR_FB_composition_layer_depth_test: D3D samples the typeless depth through an R32_FLOAT view
TEST_P(OxrD3DTest, projection_depth_is_tested)
{
    create_d3d_session();
    begin_session();
    const XrSpace     local     = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    const XrSwapchain red       = create_swapchain(64, 64, 2);
    const XrSwapchain blue      = create_swapchain(64, 64, 2);
    const XrSwapchain depth1    = create_swapchain(64, 64, 2, TEST_DXGI_D32_FLOAT, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
    const XrSwapchain depth2    = create_swapchain(64, 64, 2, TEST_DXGI_D32_FLOAT, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
    const f32         r[4]      = { 1.0f, 0.0f, 0.0f, 1.0f };
    const f32         b[4]      = { 0.0f, 0.0f, 1.0f, 1.0f };
    const f32         near_far[2] = { 0.2f, 0.8f };
    const f32         far_near[2] = { 0.8f, 0.2f };
    render_into(red, 64, 64, 2, r, r);
    render_into(blue, 64, 64, 2, b, b);
    const XrSwapchain depths[2] = { depth1, depth2 };
    for (u32 l = 0; l < 2; l++)
    {
        u64 textures[8] = {};
        u32 index       = 0;
        images(depths[l], textures, 8);
        ASSERT_EQ(xr.xrAcquireSwapchainImage(depths[l], nullptr, &index), XR_SUCCESS);
        const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        ASSERT_EQ(xr.xrWaitSwapchainImage(depths[l], &wait_info), XR_SUCCESS);
        EXPECT_TRUE(app.fill_depth(textures[index], 2, l == 0 ? near_far : far_near));
        ASSERT_EQ(xr.xrReleaseSwapchainImage(depths[l], nullptr), XR_SUCCESS);
    }

    XrCompositionLayerDepthInfoKHR      depth[2][2] = {};
    XrCompositionLayerProjectionView    views[2][2] = { { view(red, 0, 64, 64), view(red, 1, 64, 64) }, { view(blue, 0, 64, 64), view(blue, 1, 64, 64) } };
    const XrCompositionLayerDepthTestFB test        = { .type = XR_TYPE_COMPOSITION_LAYER_DEPTH_TEST_FB, .depthMask = XR_TRUE, .compareOp = XR_COMPARE_OP_LESS_FB };
    XrCompositionLayerProjection        layer[2]    = {};
    for (u32 l = 0; l < 2; l++)
    {
        for (u32 v = 0; v < 2; v++)
        {
            depth[l][v]      = { .type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR, .subImage = { .swapchain = depths[l], .imageRect = { { 0, 0 }, { 64, 64 } }, .imageArrayIndex = v }, .minDepth = 0.0f, .maxDepth = 1.0f, .nearZ = 0.1f, .farZ = 100.0f };
            views[l][v].next = &depth[l][v];
        }
        layer[l] = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .next = &test, .space = local, .viewCount = 2, .views = views[l] };
    }
    const XrCompositionLayerBaseHeader* layers[2] = { (const XrCompositionLayerBaseHeader*)&layer[0], (const XrCompositionLayerBaseHeader*)&layer[1] };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 2, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[0], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[2], 0);
    EXPECT_EQ(eye_pixel(pixels, width, height, 1, 0.0f, 0.0f)[0], 0);
    EXPECT_EQ(eye_pixel(pixels, width, height, 1, 0.0f, 0.0f)[2], 255);
}

namespace
{
// With a mirror window: SDL's offscreen driver on Linux, a real window on Windows
struct OxrD3DWindowedTest : OxrD3DTest
{
    OxrD3DWindowedTest ()
    {
        headless = false;
    }
};
} // namespace

TEST_P(OxrD3DWindowedTest, frames_composite_with_a_mirror_window)
{
    create_d3d_session();
    begin_session();
    const XrSwapchain swapchain = create_swapchain(32, 32, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    const f32         red[4]     = { 1.0f, 0.0f, 0.0f, 1.0f };
    const XrCompositionLayerQuad quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 1.0f, 1.0f } };
    const XrCompositionLayerBaseHeader* layers[1] = { (const XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    for (u32 frame = 0; frame < 3; frame++)
    {
        render_into(swapchain, 32, 32, 1, red, red);
        submit_and_read(layers, 1, pixels, width, height);
        EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[0], 255) << "frame " << frame;
    }
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

// A resized mirror window gets a new swapchain; D3D refuses one while anything still holds the
// old one, which the UI's framebuffer of its back buffer did, and the window stayed black
namespace
{
// A window D3D can present to, which SDL's offscreen driver has none of
struct OxrD3DRealWindowTest : OxrD3DTest
{
    OxrD3DRealWindowTest ()
    {
        headless    = false;
        real_window = true;
    }
};
} // namespace

// The window is shown after the first frame and maximized then (a hidden window gets no frame);
// the first frame it shows must be drawn at its size, or the stretched frame shows the welcome
// text zoomed in
TEST_P(OxrD3DRealWindowTest, first_frame_has_the_shown_windows_size)
{
    create_d3d_session();
    begin_session(); // submits the first frame, whose tick shows the window
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt, nullptr);
    ASSERT_TRUE(rt->window_shown);
    frame();
    u32 window_width  = 0;
    u32 window_height = 0;
    os_window_size(rt->window, window_width, window_height);
    for (const OXR_Session& live : rt->sessions)
    {
        if (!live.window_swapchain)
        {
            GTEST_SKIP() << "no mirror window swapchain";
        }
        u32 width  = 0;
        u32 height = 0;
        r_window_swapchain_size(live.device, live.window_swapchain, width, height);
        EXPECT_EQ(width, window_width);
        EXPECT_EQ(height, window_height);
    }
}

#if !PUPPETXR_HEADLESS_ONLY
// A click on a panel is the panel's; one into the view takes the mouse, and then the panels get
// nothing: no hover, no keys
TEST_P(OxrD3DRealWindowTest, a_click_on_a_panel_leaves_the_mouse_to_the_panels)
{
    create_d3d_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt, nullptr);
    oxr_show_ui(true);
    UI_Context* ui = nullptr;
    for (const OXR_Session& live : rt->sessions)
    {
        ui = live.ui;
    }
    ASSERT_NE(ui, nullptr);
    ui_set_probe(ui, true);
    f32 x       = 0.0f;
    f32 y       = 0.0f;
    b8  hovered = false;
    f32 mouse[2] = {};
    for (u32 i = 0; i < 100 && !oxr_ui_find("fixed step clock", x, y, hovered, mouse); i++)
    {
        frame();
    }
    // A new panel shows once ImGui knows its size
    for (u32 i = 0; i < 100 && !ui_wants_click(ui, x, y); i++)
    {
        frame();
        oxr_ui_find("fixed step clock", x, y, hovered, mouse);
    }
    ASSERT_TRUE(ui_wants_click(ui, x, y));
    // Pushed events reach the simulator through the window thread, a frame or more later
    const b8 fixed = sim_clock_mode(oxr_get_sim()) == SIM_ClockMode::FixedStep;
    ASSERT_TRUE(oxr_ui_click("fixed step clock"));
    for (u32 i = 0; i < 100 && (sim_clock_mode(oxr_get_sim()) == SIM_ClockMode::FixedStep) == fixed; i++)
    {
        frame();
    }
    EXPECT_NE(sim_clock_mode(oxr_get_sim()) == SIM_ClockMode::FixedStep, fixed) << "the panel got the click";
    EXPECT_FALSE(rt->mouse_captured) << "clicked on the Simulator panel";

    os_push_mouse_event(rt->window, 1000.0f, 600.0f, OS_Key::None, false);
    os_push_mouse_event(rt->window, 1000.0f, 600.0f, OS_Key::MouseLeft, true);
    os_push_mouse_event(rt->window, 1000.0f, 600.0f, OS_Key::MouseLeft, false);
    for (u32 i = 0; i < 100 && !rt->mouse_captured; i++)
    {
        frame();
    }
    ASSERT_TRUE(rt->mouse_captured) << "clicked into the view";
    os_push_mouse_event(rt->window, x, y, OS_Key::None, false);
    for (u32 i = 0; i < 10; i++)
    {
        frame();
    }
    ASSERT_TRUE(oxr_ui_find("fixed step clock", x, y, hovered, mouse));
    EXPECT_FALSE(hovered) << "the panels see no mouse while it is held";
    EXPECT_LT(mouse[0], -1e30f);
}
#endif

#if defined(_WIN32)
// Unreal's game window clips the cursor to itself while it is in front and lifts that again,
// which let the cursor out of the mirror window while it held the mouse and left it caged in a
// hidden window after Escape
TEST_P(OxrD3DRealWindowTest, the_held_mouse_stays_in_the_window_and_goes_free_with_escape)
{
    create_d3d_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt, nullptr);
    rt->show_welcome = false;
    defer { ClipCursor(nullptr); };
    OS_WindowNative native = {};
    ASSERT_TRUE(os_window_native_handles(rt->window, native));
    const HWND hwnd = (HWND)(uintptr_t)native.window;
    os_push_mouse_event(rt->window, 400.0f, 300.0f, OS_Key::None, false);
    os_push_mouse_event(rt->window, 400.0f, 300.0f, OS_Key::MouseLeft, true);
    os_push_mouse_event(rt->window, 400.0f, 300.0f, OS_Key::MouseLeft, false);
    for (u32 i = 0; i < 100 && !rt->mouse_captured; i++)
    {
        frame();
    }
    ASSERT_TRUE(rt->mouse_captured);

    RECT client = {};
    POINT corner = {};
    ASSERT_TRUE(GetClientRect(hwnd, &client) && ClientToScreen(hwnd, &corner));
    OffsetRect(&client, corner.x, corner.y);
    const auto clipped_inside = [&]
    {
        RECT clip = {};
        return GetClipCursor(&clip) && clip.left >= client.left && clip.top >= client.top && clip.right <= client.right && clip.bottom <= client.bottom;
    };
    const RECT other = { client.right, client.top, client.right + 100, client.top + 100 };
    ClipCursor(&other);
    for (u32 i = 0; i < 20 && !clipped_inside(); i++)
    {
        frame();
    }
    EXPECT_TRUE(clipped_inside()) << "the window took the cursor back";

    os_push_key_event(rt->window, OS_Key::Escape, true, false);
    os_push_key_event(rt->window, OS_Key::Escape, false, false);
    for (u32 i = 0; i < 100 && (rt->mouse_captured || clipped_inside()); i++)
    {
        frame();
    }
    EXPECT_FALSE(rt->mouse_captured);
    EXPECT_FALSE(clipped_inside()) << "the cursor is free again";
}
#endif

#if !PUPPETXR_HEADLESS_ONLY
// The click that took the mouse reached the panels as a press whose release they never got, so
// the first click on a panel after Escape only ended that press
TEST_P(OxrD3DRealWindowTest, the_first_click_after_escape_reaches_a_panel)
{
    create_d3d_session();
    begin_session();
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt, nullptr);
    rt->show_welcome = false;
    UI_Context* ui = nullptr;
    for (const OXR_Session& live : rt->sessions)
    {
        ui = live.ui;
    }
    ASSERT_NE(ui, nullptr);
    ui_set_probe(ui, true);
    // A person's release comes frames after the press, once the window holds the mouse
    os_push_mouse_event(rt->window, 1000.0f, 600.0f, OS_Key::None, false);
    os_push_mouse_event(rt->window, 1000.0f, 600.0f, OS_Key::MouseLeft, true);
    for (u32 i = 0; i < 100 && !rt->mouse_captured; i++)
    {
        frame();
    }
    ASSERT_TRUE(rt->mouse_captured);
    os_push_mouse_event(rt->window, 1000.0f, 600.0f, OS_Key::MouseLeft, false);
    frame();
    frame();
    oxr_show_ui(true);
    os_push_key_event(rt->window, OS_Key::Escape, true, false);
    os_push_key_event(rt->window, OS_Key::Escape, false, false);
    for (u32 i = 0; i < 100 && rt->mouse_captured; i++)
    {
        frame();
    }
    ASSERT_FALSE(rt->mouse_captured);

    f32 x        = 0.0f;
    f32 y        = 0.0f;
    b8  hovered  = false;
    f32 mouse[2] = {};
    for (u32 i = 0; i < 100 && !(oxr_ui_find("fixed step clock", x, y, hovered, mouse) && ui_wants_click(ui, x, y)); i++)
    {
        frame();
    }
    ASSERT_TRUE(ui_wants_click(ui, x, y));
    const b8 fixed = sim_clock_mode(oxr_get_sim()) == SIM_ClockMode::FixedStep;
    ASSERT_TRUE(oxr_ui_click("fixed step clock"));
    for (u32 i = 0; i < 100 && (sim_clock_mode(oxr_get_sim()) == SIM_ClockMode::FixedStep) == fixed; i++)
    {
        frame();
    }
    EXPECT_NE(sim_clock_mode(oxr_get_sim()) == SIM_ClockMode::FixedStep, fixed);
    EXPECT_FALSE(rt->mouse_captured);
}
#endif

TEST_P(OxrD3DRealWindowTest, mirror_window_swapchain_is_recreated_on_resize)
{
    create_d3d_session();
    begin_session();
    const XrSwapchain swapchain = create_swapchain(32, 32, 1);
    const XrSpace     view_space = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    const f32         red[4]     = { 1.0f, 0.0f, 0.0f, 1.0f };
    const XrCompositionLayerQuad quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 1.0f, 1.0f } };
    const XrCompositionLayerBaseHeader* layers[1] = { (const XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    render_into(swapchain, 32, 32, 1, red, red);
    submit_and_read(layers, 1, pixels, width, height);
    OXR_Runtime* rt = oxr_get_runtime();
    ASSERT_NE(rt, nullptr);
    b8 windowed = false;
    for (OXR_Session& live : rt->sessions)
    {
        windowed |= (b8)live.window_swapchain;
        live.window_resized = true;
    }
    if (!windowed)
    {
        GTEST_SKIP() << "no mirror window swapchain";
    }
    for (u32 frame = 0; frame < 3; frame++)
    {
        render_into(swapchain, 32, 32, 1, red, red);
        submit_and_read(layers, 1, pixels, width, height);
    }
    for (const OXR_Session& live : rt->sessions)
    {
        EXPECT_TRUE(live.window_swapchain) << "the window lost its swapchain";
    }
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

static std::string d3d_api_name (const testing::TestParamInfo<b8>& param)
{
    return param.param ? "D3D12" : "D3D11";
}

namespace
{

// XR_META_environment_depth on D3D, as Unreal's Occlusion sample reads it: a 16-bit texture array
// of two layers, viewed as R16_UNORM
struct OxrD3DDepthTest : OxrD3DTest
{
    char rooms[OS_MAX_PATH] = {};

    OxrD3DDepthTest ()
    {
        extra_extension = XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME;
    }

    void SetUp () override
    {
        if (!os_create_temp_directory("puppetxr_rooms", rooms, sizeof(rooms)))
        {
            FAIL() << "no temporary directory";
        }
        TestRoom room = {};
        test_room_build(room);
        ASSERT_TRUE(test_room_write(room, rooms, "test_box"));
        os_set_env("PUPPETXR_ROOMS_DIR", rooms);
        os_set_env("PUPPETXR_ROOM", "test_box");
        OxrD3DTest::SetUp();
    }

    void TearDown () override
    {
        OxrD3DTest::TearDown();
        os_delete_tree(rooms);
        os_unset_env("PUPPETXR_ROOMS_DIR");
        os_unset_env("PUPPETXR_ROOM");
    }
};

} // namespace

TEST_P(OxrD3DDepthTest, environment_depth_of_the_room_reads_as_apps_sample_it)
{
#define LOAD(fn) PFN_##fn fn = nullptr; ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&fn), XR_SUCCESS) << #fn;
    DEPTH_FUNCTIONS(LOAD)
#undef LOAD
    create_d3d_session();
    begin_session();
    const XrEnvironmentDepthProviderCreateInfoMETA provider_info = { .type = XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META };
    XrEnvironmentDepthProviderMETA                 provider      = XR_NULL_HANDLE;
    ASSERT_EQ(xrCreateEnvironmentDepthProviderMETA(session, &provider_info, &provider), XR_SUCCESS);
    const XrEnvironmentDepthSwapchainCreateInfoMETA swapchain_info = { .type = XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META };
    XrEnvironmentDepthSwapchainMETA                 depth          = XR_NULL_HANDLE;
    ASSERT_EQ(xrCreateEnvironmentDepthSwapchainMETA(provider, &swapchain_info, &depth), XR_SUCCESS);
    ASSERT_EQ(xrStartEnvironmentDepthProviderMETA(provider), XR_SUCCESS);
    XrEnvironmentDepthSwapchainStateMETA state = { .type = XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META };
    ASSERT_EQ(xrGetEnvironmentDepthSwapchainStateMETA(depth, &state), XR_SUCCESS);
    u32          count = 0;
    TestD3DImage textures[8] = {};
    for (TestD3DImage& image : textures)
    {
        image.type = GetParam() ? XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR : XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    }
    ASSERT_EQ(xrEnumerateEnvironmentDepthSwapchainImagesMETA(depth, 8, &count, (XrSwapchainImageBaseHeader*)textures), XR_SUCCESS);
    u32 width = 0;
    u32 height = 0;
    u32 layers = 0;
    u32 format = 0;
    ASSERT_TRUE(app.describe((u64)(uintptr_t)textures[0].texture, width, height, layers, format));
    EXPECT_EQ(layers, 2u);
    EXPECT_EQ(format, 53u) << "DXGI_FORMAT_R16_TYPELESS, which Unreal views as R16_UNORM";

    sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { { 0.0f, 1.6f, 0.0f }, zm::QUAT_IDENTITY });
    const XrSpace               stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    XrEnvironmentDepthImageMETA image = {};
    XrResult                    acquired = XR_ERROR_RUNTIME_FAILURE;
    for (u32 frame = 0; frame < 3 && acquired != XR_SUCCESS; frame++)
    {
        XrFrameState state_out = { .type = XR_TYPE_FRAME_STATE };
        ASSERT_EQ(xr.xrWaitFrame(session, nullptr, &state_out), XR_SUCCESS);
        ASSERT_EQ(xr.xrBeginFrame(session, nullptr), XR_SUCCESS);
        image = { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META, .views = { { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META }, { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META } } };
        const XrEnvironmentDepthImageAcquireInfoMETA acquire = { .type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META, .space = stage, .displayTime = state_out.predictedDisplayTime };
        acquired = xrAcquireEnvironmentDepthImageMETA(provider, &acquire, &image);
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state_out.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        ASSERT_EQ(xr.xrEndFrame(session, &end_info), XR_SUCCESS);
    }
    ASSERT_EQ(acquired, XR_SUCCESS);
    ASSERT_LT(image.swapchainIndex, count);

    // The wall ahead at 1.5 m, the ceiling 1 m ahead looking up at tan 0.9, as on Vulkan
    Array<u16> texels = {};
    defer { texels.release(); };
    texels.resize((usize)state.width * state.height);
    ASSERT_TRUE(app.read_r16((u64)(uintptr_t)textures[image.swapchainIndex].texture, 0, state.width, state.height, texels.data));
    const u32 w = state.width;
    const u32 h = state.height;
    EXPECT_NEAR(texels[(h / 2) * w + w / 2] / 65535.0f, 1.0f - 0.1f / 1.5f, 2e-3f);
    const u32 up_row = (u32)((0.9f * 0.5f + 0.5f) * (f32)h);
    EXPECT_NEAR(texels[up_row * w + w / 2] / 65535.0f, 1.0f - 0.1f / 1.0f, 4e-3f);
}

INSTANTIATE_TEST_SUITE_P(Apis, OxrD3DDepthTest, testing::Values(false, true), d3d_api_name);
INSTANTIATE_TEST_SUITE_P(Apis, OxrD3DTest, testing::Values(false, true), d3d_api_name);
INSTANTIATE_TEST_SUITE_P(Apis, OxrD3DWindowedTest, testing::Values(false, true), d3d_api_name);
INSTANTIATE_TEST_SUITE_P(Apis, OxrD3DRealWindowTest, testing::Values(false, true), d3d_api_name);

//
// XR_KHR_metal_enable
//

namespace
{

#define TEST_MTL_RGBA8_UNORM_SRGB      71
#define TEST_MTL_DEPTH32_FLOAT         252
#define TEST_MTL_TEXTURE_TYPE_2D       2
#define TEST_MTL_TEXTURE_TYPE_2D_ARRAY 3
#define TEST_MTL_TEXTURE_TYPE_CUBE     5

// What the runtime hands out, asked of the texture as an app would
static u64 metal_property (u64 texture, const char* name)
{
#if PUPPETXR_HAVE_METAL
    return test_objc_send<u64>((void*)(uintptr_t)texture, name);
#else
    (void)texture;
    (void)name;
    return 0;
#endif
}

// The app renders with Metal on the queue it binds, and fills swapchain images through a device of
// its own over that queue
struct OxrMetalTest : OxrTest
{
    TestMetalApp app       = {};
    R_Device*    app_draw  = nullptr;
    const char*  metal_extension = "XR_KHR_metal_enable";

    void SetUp () override
    {
        OxrTest::SetUp();
        if (!r_backend_available(R_Backend::Metal) || !app.create())
        {
            GTEST_SKIP() << "no Metal device";
        }
        app_draw = r_create_device(app.info());
        ASSERT_NE(app_draw, nullptr);
        const char* extensions[] = { metal_extension };
        ASSERT_EQ(create_instance(extensions, 1), XR_SUCCESS);
        const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
        ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);
    }

    void TearDown () override
    {
        if (session != XR_NULL_HANDLE)
        {
            xr.xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }
        OxrTest::TearDown();
        if (app_draw)
        {
            r_destroy_device(app_draw);
        }
        app.destroy();
    }

    XrResult requirements (XrGraphicsRequirementsMetalKHR* out)
    {
        PFN_xrGetMetalGraphicsRequirementsKHR get = nullptr;
        EXPECT_EQ(gipa(instance, "xrGetMetalGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&get), XR_SUCCESS);
        return get ? get(instance, system, out) : XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    void create_metal_session ()
    {
        XrGraphicsRequirementsMetalKHR result = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR };
        ASSERT_EQ(requirements(&result), XR_SUCCESS);
        const XrGraphicsBindingMetalKHR binding = { .type = XR_TYPE_GRAPHICS_BINDING_METAL_KHR, .commandQueue = app.queue };
        const XrSessionCreateInfo       info    = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = system };
        ASSERT_EQ(xr.xrCreateSession(instance, &info, &session), XR_SUCCESS);
    }

    XrSwapchain create_swapchain (u32 width, u32 height, u32 array_size, i64 format = TEST_MTL_RGBA8_UNORM_SRGB, XrSwapchainUsageFlags usage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, u32 faces = 1)
    {
        XrSwapchain                 swapchain = XR_NULL_HANDLE;
        const XrSwapchainCreateInfo info      = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = usage, .format = format, .sampleCount = 1, .width = width, .height = height, .faceCount = faces, .arraySize = array_size, .mipCount = 1 };
        EXPECT_EQ(xr.xrCreateSwapchain(session, &info, &swapchain), XR_SUCCESS);
        return swapchain;
    }

    u32 images (XrSwapchain swapchain, u64* out_textures, u32 capacity)
    {
        u32 count = 0;
        EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr), XR_SUCCESS);
        XrSwapchainImageMetalKHR metal[8] = {};
        for (XrSwapchainImageMetalKHR& image : metal)
        {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
        }
        EXPECT_LE(count, 8u);
        EXPECT_EQ(xr.xrEnumerateSwapchainImages(swapchain, count, &count, (XrSwapchainImageBaseHeader*)metal), XR_SUCCESS);
        for (u32 i = 0; i < count && i < capacity; i++)
        {
            out_textures[i] = (u64)(uintptr_t)metal[i].texture;
        }
        return count;
    }

    // Every layer of the acquired image gets `top` in its upper half and `bottom` in the lower
    void render_into (XrSwapchain swapchain, u32 width, u32 height, u32 layers, const u8 top[4], const u8 bottom[4])
    {
        u64       textures[8] = {};
        const u32 count       = images(swapchain, textures, 8);
        u32       index       = 0;
        ASSERT_EQ(xr.xrAcquireSwapchainImage(swapchain, nullptr, &index), XR_SUCCESS);
        const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        ASSERT_EQ(xr.xrWaitSwapchainImage(swapchain, &wait_info), XR_SUCCESS);
        ASSERT_LT(index, count);
        const R_ImageHandle  image  = r_import_image(app_draw, { .native_image = textures[index], .width = width, .height = height, .format = R_Format::R8G8B8A8_SRGB });
        const R_BufferHandle buffer = r_create_buffer(app_draw, { .size = (u64)width * height * 4, .usage_flags = (u32)R_BufferUsageFlags::TransferSrc, .host_visible = true });
        u8*                  texels = (u8*)r_get_buffer_mapped_data(app_draw, buffer);
        for (u32 i = 0; i < width * height; i++)
        {
            memory_copy(texels + (usize)i * 4, i / width < height / 2 ? top : bottom, 4);
        }
        const R_CmdBufferHandle cmd = r_begin_immediate(app_draw);
        for (u32 layer = 0; layer < layers; layer++)
        {
            r_cmd_copy_buffer_to_image(app_draw, cmd, buffer, image, layer);
        }
        r_end_immediate(app_draw, cmd);
        r_destroy_buffer(app_draw, buffer);
        r_destroy_image(app_draw, image);
        EXPECT_EQ(xr.xrReleaseSwapchainImage(swapchain, nullptr), XR_SUCCESS);
    }
};

// Unity's OpenXR plugin asks for the extension by its provisional name
struct OxrMetalProvisionalTest : OxrMetalTest
{
    OxrMetalProvisionalTest ()
    {
        metal_extension = "XR_KHRX2_metal_enable";
    }
};

struct OxrMetalWindowedTest : OxrMetalTest
{
    OxrMetalWindowedTest ()
    {
        headless = false;
    }
};

// A window with a CAMetalLayer, which SDL's offscreen driver has none of
struct OxrMetalRealWindowTest : OxrMetalTest
{
    OxrMetalRealWindowTest ()
    {
        headless    = false;
        real_window = true;
    }
};

} // namespace

TEST_F(OxrTest, metal_extensions_are_offered_where_the_build_has_metal)
{
    EXPECT_EQ(extension_offered(xr, "XR_KHR_metal_enable"), r_backend_available(R_Backend::Metal));
    EXPECT_EQ(extension_offered(xr, "XR_KHRX2_metal_enable"), r_backend_available(R_Backend::Metal));
}

TEST_F(OxrMetalTest, requirements_name_the_default_device_and_validate_their_arguments)
{
    XrGraphicsRequirementsMetalKHR result = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR };
    ASSERT_EQ(requirements(&result), XR_SUCCESS);
    EXPECT_EQ(result.metalDevice, r_metal_default_device());
    EXPECT_EQ(result.metalDevice, app.device);

    PFN_xrGetMetalGraphicsRequirementsKHR get = nullptr;
    ASSERT_EQ(gipa(instance, "xrGetMetalGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&get), XR_SUCCESS);
    EXPECT_EQ(get(instance, 42, &result), XR_ERROR_SYSTEM_INVALID);
    EXPECT_EQ(get(instance, system, nullptr), XR_ERROR_VALIDATION_FAILURE);
    result.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    EXPECT_EQ(get(instance, system, &result), XR_ERROR_VALIDATION_FAILURE);
    result.type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR;
    EXPECT_EQ(get(garbage_instance(), system, &result), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrMetalTest, session_needs_the_requirements_call_and_a_command_queue)
{
    XrGraphicsBindingMetalKHR binding = { .type = XR_TYPE_GRAPHICS_BINDING_METAL_KHR, .commandQueue = app.queue };
    const XrSessionCreateInfo info    = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = system };
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
    XrGraphicsRequirementsMetalKHR result = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR };
    ASSERT_EQ(requirements(&result), XR_SUCCESS);
    binding.commandQueue = nullptr;
    EXPECT_EQ(xr.xrCreateSession(instance, &info, &session), XR_ERROR_GRAPHICS_DEVICE_INVALID);
    EXPECT_EQ(session, XR_NULL_HANDLE);
    binding.commandQueue = app.queue;
    ASSERT_EQ(xr.xrCreateSession(instance, &info, &session), XR_SUCCESS);
    EXPECT_NE(session, XR_NULL_HANDLE);
}

TEST_F(OxrMetalProvisionalTest, the_provisional_name_binds_metal_too)
{
    create_metal_session();
    EXPECT_NE(session, XR_NULL_HANDLE);
}

// Unity's OpenXR plugin asks for the requirements under the provisional extension's own name
TEST_F(OxrMetalProvisionalTest, requirements_are_found_under_the_provisional_name)
{
    PFN_xrGetMetalGraphicsRequirementsKHR get = nullptr;
    ASSERT_EQ(gipa(instance, "xrGetMetalGraphicsRequirementsKHRX2", (PFN_xrVoidFunction*)&get), XR_SUCCESS);
    ASSERT_NE(get, nullptr);
    XrGraphicsRequirementsMetalKHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR };
    EXPECT_EQ(get(instance, system, &requirements), XR_SUCCESS);
    EXPECT_NE(requirements.metalDevice, nullptr);
}

// Apps see a one-layer image as a 2D texture, more layers as a 2D array and six faces as a cube
TEST_F(OxrMetalTest, swapchains_are_metal_textures_of_their_type)
{
    create_metal_session();
    u32 count = 0;
    EXPECT_EQ(xr.xrEnumerateSwapchainFormats(session, 0, &count, nullptr), XR_SUCCESS);
    int64_t formats[16] = {};
    ASSERT_LE(count, 16u);
    EXPECT_EQ(xr.xrEnumerateSwapchainFormats(session, 16, &count, formats), XR_SUCCESS);
    ASSERT_GT(count, 0u);
    EXPECT_EQ(formats[0], TEST_MTL_RGBA8_UNORM_SRGB);

    const XrSwapchain flat    = create_swapchain(32, 16, 1);
    const XrSwapchain layered = create_swapchain(32, 16, 2);
    const XrSwapchain cube    = create_swapchain(16, 16, 1, TEST_MTL_RGBA8_UNORM_SRGB, XR_SWAPCHAIN_USAGE_SAMPLED_BIT, 6);
    const XrSwapchain depth   = create_swapchain(16, 16, 1, TEST_MTL_DEPTH32_FLOAT, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    u64 textures[8] = {};
    ASSERT_EQ(images(flat, textures, 8), 3u);
    for (u32 i = 0; i < 3; i++)
    {
        EXPECT_EQ(metal_property(textures[i], "textureType"), (u64)TEST_MTL_TEXTURE_TYPE_2D);
        EXPECT_EQ(metal_property(textures[i], "width"), 32u);
        EXPECT_EQ(metal_property(textures[i], "height"), 16u);
        EXPECT_EQ(metal_property(textures[i], "pixelFormat"), (u64)TEST_MTL_RGBA8_UNORM_SRGB);
    }
    ASSERT_EQ(images(layered, textures, 8), 3u);
    EXPECT_EQ(metal_property(textures[0], "textureType"), (u64)TEST_MTL_TEXTURE_TYPE_2D_ARRAY);
    EXPECT_EQ(metal_property(textures[0], "arrayLength"), 2u);
    ASSERT_EQ(images(cube, textures, 8), 3u);
    EXPECT_EQ(metal_property(textures[0], "textureType"), (u64)TEST_MTL_TEXTURE_TYPE_CUBE);
    ASSERT_EQ(images(depth, textures, 8), 3u);
    EXPECT_EQ(metal_property(textures[0], "pixelFormat"), (u64)TEST_MTL_DEPTH32_FLOAT);

    // The image struct must be the session's API's
    XrSwapchainImageVulkanKHR wrong[3] = {};
    for (XrSwapchainImageVulkanKHR& image : wrong)
    {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    }
    u32 written = 0;
    EXPECT_EQ(xr.xrEnumerateSwapchainImages(flat, 3, &written, (XrSwapchainImageBaseHeader*)wrong), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(xr.xrDestroySwapchain(depth), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(cube), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(layered), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySwapchain(flat), XR_SUCCESS);
}

// Metal textures have their origin at the top as Vulkan's: the app's upper half shows on top
TEST_F(OxrMetalTest, projection_layers_are_composited_upright)
{
    create_metal_session();
    begin_session();
    const XrSwapchain swapchain = create_swapchain(64, 64, 2);
    const XrSpace     stage     = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    const u8          green[4]  = { 0, 255, 0, 255 };
    const u8          red[4]    = { 255, 0, 0, 255 };
    render_into(swapchain, 64, 64, 2, red, green);

    XrCompositionLayerProjectionView views[2] = { view(swapchain, 0, 64, 64), view(swapchain, 1, 64, 64) };
    const XrCompositionLayerProjection layer = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = stage, .viewCount = 2, .views = views };
    const XrCompositionLayerBaseHeader* layers[1] = { (const XrCompositionLayerBaseHeader*)&layer };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    submit_and_read(layers, 1, pixels, width, height);
    for (u32 eye = 0; eye < 2; eye++)
    {
        const u8* upper = eye_pixel(pixels, width, height, eye, 0.0f, -0.3f);
        const u8* lower = eye_pixel(pixels, width, height, eye, 0.0f, 0.3f);
        EXPECT_EQ(upper[0], 255) << "eye " << eye;
        EXPECT_EQ(upper[1], 0) << "eye " << eye;
        EXPECT_EQ(lower[0], 0) << "eye " << eye;
        EXPECT_EQ(lower[1], 255) << "eye " << eye;
    }

    // A second frame with other colours replaces the first: the compositor sees the app's latest rendering
    const u8 blue[4] = { 0, 0, 255, 255 };
    render_into(swapchain, 64, 64, 2, blue, blue);
    submit_and_read(layers, 1, pixels, width, height);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, -0.3f)[2], 255);
    EXPECT_EQ(eye_pixel(pixels, width, height, 0, 0.0f, -0.3f)[0], 0);
    EXPECT_EQ(xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

static void metal_quad_frames (OxrMetalTest& test)
{
    test.create_metal_session();
    test.begin_session();
    const XrSwapchain swapchain = test.create_swapchain(32, 32, 1);
    const XrSpace     view_space = test.reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    const u8          red[4]     = { 255, 0, 0, 255 };
    const XrCompositionLayerQuad quad = { .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH, .subImage = { .swapchain = swapchain, .imageRect = { { 0, 0 }, { 32, 32 } } }, .pose = { { 0, 0, 0, 1 }, { 0, 0, -1 } }, .size = { 1.0f, 1.0f } };
    const XrCompositionLayerBaseHeader* layers[1] = { (const XrCompositionLayerBaseHeader*)&quad };
    Array<u8> pixels = {};
    defer { pixels.release(); };
    u32 width  = 0;
    u32 height = 0;
    for (u32 frame = 0; frame < 3; frame++)
    {
        test.render_into(swapchain, 32, 32, 1, red, red);
        test.submit_and_read(layers, 1, pixels, width, height);
        EXPECT_EQ(OxrTest::eye_pixel(pixels, width, height, 0, 0.0f, 0.0f)[0], 255) << "frame " << frame;
    }
    EXPECT_EQ(test.xr.xrDestroySwapchain(swapchain), XR_SUCCESS);
}

TEST_F(OxrMetalWindowedTest, frames_composite_with_a_mirror_window)
{
    metal_quad_frames(*this);
}

// The mirror window presents through a CAMetalLayer of the app's device
TEST_F(OxrMetalRealWindowTest, frames_composite_and_present_to_the_mirror_window)
{
    metal_quad_frames(*this);
    const OXR_Session* live = nullptr;
    for (const OXR_Session& session_state : oxr_get_runtime()->sessions)
    {
        live = &session_state;
    }
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(live->window_swapchain) << "the window has no swapchain";
    EXPECT_NE(live->window_presented_ns, 0u) << "no frame was presented";
    EXPECT_GT(oxr_get_runtime()->frame_times.mirror_presents, 0u) << "the performance tab counts the presents";
}

//
// Face and eye tracking: XR_FB_face_tracking2, XR_FB_face_tracking, XR_META_face_tracking_visemes,
// XR_FB_eye_tracking_social
//

namespace
{
struct OxrFaceTest : OxrActionTest
{
    PFN_xrCreateFaceTracker2FB         create_face2  = nullptr;
    PFN_xrDestroyFaceTracker2FB        destroy_face2 = nullptr;
    PFN_xrGetFaceExpressionWeights2FB  weights2      = nullptr;
    PFN_xrCreateFaceTrackerFB          create_face   = nullptr;
    PFN_xrDestroyFaceTrackerFB         destroy_face  = nullptr;
    PFN_xrGetFaceExpressionWeightsFB   weights1      = nullptr;
    PFN_xrCreateEyeTrackerFB           create_eyes   = nullptr;
    PFN_xrDestroyEyeTrackerFB          destroy_eyes  = nullptr;
    PFN_xrGetEyeGazesFB                gazes         = nullptr;
    XrSpace                            stage         = XR_NULL_HANDLE;

    OxrFaceTest ()
    {
        more_extensions[more_extensions_count++] = XR_FB_FACE_TRACKING2_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_FB_FACE_TRACKING_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_META_FACE_TRACKING_VISEMES_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrActionTest::SetUp();
        ASSERT_EQ(gipa(instance, "xrCreateFaceTracker2FB", (PFN_xrVoidFunction*)&create_face2), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrDestroyFaceTracker2FB", (PFN_xrVoidFunction*)&destroy_face2), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetFaceExpressionWeights2FB", (PFN_xrVoidFunction*)&weights2), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrCreateFaceTrackerFB", (PFN_xrVoidFunction*)&create_face), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrDestroyFaceTrackerFB", (PFN_xrVoidFunction*)&destroy_face), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetFaceExpressionWeightsFB", (PFN_xrVoidFunction*)&weights1), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrCreateEyeTrackerFB", (PFN_xrVoidFunction*)&create_eyes), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrDestroyEyeTrackerFB", (PFN_xrVoidFunction*)&destroy_eyes), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetEyeGazesFB", (PFN_xrVoidFunction*)&gazes), XR_SUCCESS);
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    }

    XrFaceTracker2FB face2 (XrFaceTrackingDataSource2FB* sources, u32 count)
    {
        XrFaceTracker2FB                 out  = XR_NULL_HANDLE;
        const XrFaceTrackerCreateInfo2FB info = { .type = XR_TYPE_FACE_TRACKER_CREATE_INFO2_FB, .faceExpressionSet = XR_FACE_EXPRESSION_SET2_DEFAULT_FB, .requestedDataSourceCount = count, .requestedDataSources = sources };
        EXPECT_EQ(create_face2(session, &info, &out), XR_SUCCESS);
        return out;
    }

    static XrTime now ()
    {
        return sim_predicted_display_time(oxr_get_sim());
    }
};
} // namespace

TEST_F(OxrFaceTest, system_supports_face_and_eye_tracking)
{
    XrSystemFaceTrackingVisemesPropertiesMETA visemes = { .type = XR_TYPE_SYSTEM_FACE_TRACKING_VISEMES_PROPERTIES_META };
    XrSystemEyeTrackingPropertiesFB           eyes    = { .type = XR_TYPE_SYSTEM_EYE_TRACKING_PROPERTIES_FB, .next = &visemes };
    XrSystemFaceTrackingPropertiesFB          face1   = { .type = XR_TYPE_SYSTEM_FACE_TRACKING_PROPERTIES_FB, .next = &eyes };
    XrSystemFaceTrackingProperties2FB         face2   = { .type = XR_TYPE_SYSTEM_FACE_TRACKING_PROPERTIES2_FB, .next = &face1 };
    XrSystemProperties                        props   = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &face2 };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(face2.supportsVisualFaceTracking, XR_TRUE);
    EXPECT_EQ(face2.supportsAudioFaceTracking, XR_TRUE);
    EXPECT_EQ(face1.supportsFaceTracking, XR_TRUE);
    EXPECT_EQ(eyes.supportsEyeTracking, XR_TRUE);
    EXPECT_EQ(visemes.supportsVisemes, XR_TRUE);
}

TEST_F(OxrActionTest, face_and_eye_tracking_need_their_extensions)
{
    PFN_xrVoidFunction function = nullptr;
    EXPECT_EQ(gipa(instance, "xrCreateFaceTracker2FB", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrCreateFaceTrackerFB", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrCreateEyeTrackerFB", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
}

TEST_F(OxrFaceTest, trackers_are_validated)
{
    XrFaceTracker2FB            out     = XR_NULL_HANDLE;
    XrFaceTrackingDataSource2FB sources[2] = { XR_FACE_TRACKING_DATA_SOURCE2_VISUAL_FB, XR_FACE_TRACKING_DATA_SOURCE2_VISUAL_FB };
    XrFaceTrackerCreateInfo2FB  info    = { .type = XR_TYPE_FACE_TRACKER_CREATE_INFO2_FB, .faceExpressionSet = XR_FACE_EXPRESSION_SET2_DEFAULT_FB, .requestedDataSourceCount = 1, .requestedDataSources = sources };
    EXPECT_EQ(create_face2(XR_NULL_HANDLE, &info, &out), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(create_face2(session, nullptr, &out), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_face2(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    info.faceExpressionSet = (XrFaceExpressionSet2FB)1;
    EXPECT_EQ(create_face2(session, &info, &out), XR_ERROR_VALIDATION_FAILURE);
    info.faceExpressionSet        = XR_FACE_EXPRESSION_SET2_DEFAULT_FB;
    info.requestedDataSourceCount = 2;
    EXPECT_EQ(create_face2(session, &info, &out), XR_ERROR_VALIDATION_FAILURE) << "a source twice";
    sources[1] = (XrFaceTrackingDataSource2FB)7;
    EXPECT_EQ(create_face2(session, &info, &out), XR_ERROR_VALIDATION_FAILURE);
    info.requestedDataSources = nullptr;
    EXPECT_EQ(create_face2(session, &info, &out), XR_ERROR_VALIDATION_FAILURE);
    sources[1]                = XR_FACE_TRACKING_DATA_SOURCE2_AUDIO_FB;
    info.requestedDataSources = sources;
    ASSERT_EQ(create_face2(session, &info, &out), XR_SUCCESS);
    EXPECT_EQ(destroy_face2(out), XR_SUCCESS);
    EXPECT_EQ(destroy_face2(out), XR_ERROR_HANDLE_INVALID);

    XrFaceTrackerFB                 face   = XR_NULL_HANDLE;
    XrFaceTrackerCreateInfoFB       info1  = { .type = XR_TYPE_FACE_TRACKER_CREATE_INFO_FB, .faceExpressionSet = (XrFaceExpressionSetFB)3 };
    EXPECT_EQ(create_face(session, &info1, &face), XR_ERROR_VALIDATION_FAILURE);
    info1.faceExpressionSet = XR_FACE_EXPRESSION_SET_DEFAULT_FB;
    ASSERT_EQ(create_face(session, &info1, &face), XR_SUCCESS);

    XrEyeTrackerFB                 eyes      = XR_NULL_HANDLE;
    const XrEyeTrackerCreateInfoFB eyes_info = { .type = XR_TYPE_EYE_TRACKER_CREATE_INFO_FB };
    EXPECT_EQ(create_eyes(session, nullptr, &eyes), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(create_eyes(session, &eyes_info, &eyes), XR_SUCCESS);

    ASSERT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
    session = XR_NULL_HANDLE;
    EXPECT_EQ(destroy_face(face), XR_ERROR_HANDLE_INVALID) << "the session's trackers go with it";
    EXPECT_EQ(destroy_eyes(eyes), XR_ERROR_HANDLE_INVALID);
}

// The weights are the simulated face's at the asked time, the visemes chained to them
TEST_F(OxrFaceTest, weights_are_the_simulated_face)
{
    begin_session();
    frame();
    SIM_Sim*      sim   = oxr_get_sim();
    SIM_FaceState state = sim_get_face(sim);
    state.weights[(u32)SIM_FaceExpression::JawDrop] = 0.6f;
    state.visemes[10]                               = 0.4f;
    state.confidences[0]                            = 0.5f;
    state.gaze_yaw                                  = 10.0f;
    sim_set_face(sim, state);

    XrFaceTrackingDataSource2FB audio[1] = { XR_FACE_TRACKING_DATA_SOURCE2_AUDIO_FB };
    const XrFaceTracker2FB      tracker  = face2(audio, 1);
    f32                         weights[XR_FACE_EXPRESSION2_COUNT_FB] = {};
    f32                         confidences[XR_FACE_CONFIDENCE2_COUNT_FB] = {};
    XrFaceTrackingVisemesMETA   visemes = { .type = XR_TYPE_FACE_TRACKING_VISEMES_META };
    XrFaceExpressionWeights2FB  out     = { .type = XR_TYPE_FACE_EXPRESSION_WEIGHTS2_FB, .next = &visemes, .weightCount = XR_FACE_EXPRESSION2_COUNT_FB, .weights = weights, .confidenceCount = XR_FACE_CONFIDENCE2_COUNT_FB, .confidences = confidences };
    XrFaceExpressionInfo2FB     info    = { .type = XR_TYPE_FACE_EXPRESSION_INFO2_FB, .time = now() };
    EXPECT_EQ(weights2(XR_NULL_HANDLE, &info, &out), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(weights2(tracker, nullptr, &out), XR_ERROR_VALIDATION_FAILURE);
    out.weightCount = 63;
    EXPECT_EQ(weights2(tracker, &info, &out), XR_ERROR_VALIDATION_FAILURE);
    out.weightCount     = XR_FACE_EXPRESSION2_COUNT_FB;
    out.confidenceCount = 1;
    EXPECT_EQ(weights2(tracker, &info, &out), XR_ERROR_VALIDATION_FAILURE);
    out.confidenceCount = XR_FACE_CONFIDENCE2_COUNT_FB;
    info.time           = 0;
    EXPECT_EQ(weights2(tracker, &info, &out), XR_ERROR_TIME_INVALID);
    info.time = now();
    ASSERT_EQ(weights2(tracker, &info, &out), XR_SUCCESS);
    EXPECT_EQ(out.isValid, XR_TRUE);
    EXPECT_EQ(out.isEyeFollowingBlendshapesValid, XR_TRUE);
    EXPECT_EQ(out.dataSource, XR_FACE_TRACKING_DATA_SOURCE2_AUDIO_FB) << "the only source asked for";
    EXPECT_EQ(out.time, info.time);

    SIM_Sample sample = {};
    sim_sample(sim, info.time, sample);
    SIM_FaceFrame expected = {};
    sim_face_frame(sim, sample, expected);
    for (u32 e = 0; e < XR_FACE_EXPRESSION2_COUNT_FB; e++)
    {
        EXPECT_EQ(weights[e], expected.weights[e]) << sim_face_expression_name((SIM_FaceExpression)e);
    }
    EXPECT_GT(weights[XR_FACE_EXPRESSION2_EYES_LOOK_LEFT_L_FB], 0.2f) << "the eyes follow the gaze";
    EXPECT_FLOAT_EQ(confidences[XR_FACE_CONFIDENCE2_LOWER_FACE_FB], 0.5f);
    EXPECT_EQ(visemes.isValid, XR_TRUE);
    EXPECT_FLOAT_EQ(visemes.visemes[XR_FACE_TRACKING_VISEME_AA_META], 0.4f);

    // XR_FB_face_tracking: the first 63
    XrFaceTrackerFB                 face1 = XR_NULL_HANDLE;
    const XrFaceTrackerCreateInfoFB info1 = { .type = XR_TYPE_FACE_TRACKER_CREATE_INFO_FB, .faceExpressionSet = XR_FACE_EXPRESSION_SET_DEFAULT_FB };
    ASSERT_EQ(create_face(session, &info1, &face1), XR_SUCCESS);
    f32                        weights_v1[XR_FACE_EXPRESSION_COUNT_FB] = {};
    XrFaceExpressionWeightsFB  out1  = { .type = XR_TYPE_FACE_EXPRESSION_WEIGHTS_FB, .weightCount = XR_FACE_EXPRESSION_COUNT_FB, .weights = weights_v1, .confidenceCount = XR_FACE_CONFIDENCE_COUNT_FB, .confidences = confidences };
    const XrFaceExpressionInfoFB info_v1 = { .type = XR_TYPE_FACE_EXPRESSION_INFO_FB, .time = info.time };
    ASSERT_EQ(weights1(face1, &info_v1, &out1), XR_SUCCESS);
    EXPECT_EQ(out1.status.isValid, XR_TRUE);
    EXPECT_EQ(weights_v1[XR_FACE_EXPRESSION_JAW_DROP_FB], 0.6f);
    EXPECT_EQ(memcmp(weights_v1, weights, sizeof(weights_v1)), 0);
}

// The gazes are the simulated eyes' in the base space
TEST_F(OxrFaceTest, gazes_are_the_simulated_eyes)
{
    begin_session();
    frame();
    SIM_Sim*      sim   = oxr_get_sim();
    SIM_FaceState state = sim_get_face(sim);
    state.look_at       = true;
    state.target        = { 0.5f, 1.2f, -1.0f };
    sim_set_face(sim, state);
    XrEyeTrackerFB                 tracker = XR_NULL_HANDLE;
    const XrEyeTrackerCreateInfoFB create  = { .type = XR_TYPE_EYE_TRACKER_CREATE_INFO_FB };
    ASSERT_EQ(create_eyes(session, &create, &tracker), XR_SUCCESS);
    XrEyeGazesFB     out  = { .type = XR_TYPE_EYE_GAZES_FB };
    XrEyeGazesInfoFB info = { .type = XR_TYPE_EYE_GAZES_INFO_FB, .baseSpace = XR_NULL_HANDLE, .time = now() };
    EXPECT_EQ(gazes(tracker, &info, &out), XR_ERROR_HANDLE_INVALID);
    info.baseSpace = stage;
    info.time      = 0;
    EXPECT_EQ(gazes(tracker, &info, &out), XR_ERROR_TIME_INVALID);
    info.time = now();
    EXPECT_EQ(gazes(tracker, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(gazes(tracker, &info, &out), XR_SUCCESS);
    EXPECT_EQ(out.time, info.time);
    for (u32 e = 0; e < 2; e++)
    {
        EXPECT_EQ(out.gaze[e].isValid, XR_TRUE);
        EXPECT_FLOAT_EQ(out.gaze[e].gazeConfidence, 1.0f);
        const Vec3 position = { out.gaze[e].gazePose.position.x, out.gaze[e].gazePose.position.y, out.gaze[e].gazePose.position.z };
        const Quat rotation = { out.gaze[e].gazePose.orientation.x, out.gaze[e].gazePose.orientation.y, out.gaze[e].gazePose.orientation.z, out.gaze[e].gazePose.orientation.w };
        const Vec3 gaze     = zm::rotate(rotation, { 0.0f, 0.0f, -1.0f });
        EXPECT_GT(zm::dot(gaze, zm::normalize(state.target - position)), 0.9999f) << "eye " << e;
    }
    EXPECT_LT(out.gaze[0].gazePose.position.x, out.gaze[1].gazePose.position.x) << "left, then right";
}

//
// XR_EXT_eye_gaze_interaction
//

namespace
{
struct OxrEyeGazeTest : OxrActionTest
{
    XrAction gaze  = XR_NULL_HANDLE;
    XrPath   eyes  = XR_NULL_PATH;
    XrSpace  stage = XR_NULL_HANDLE;

    OxrEyeGazeTest ()
    {
        more_extensions[more_extensions_count++] = XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrActionTest::SetUp();
        eyes = path("/user/eyes_ext");
        XrActionCreateInfo info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionType = XR_ACTION_TYPE_POSE_INPUT };
        cstr_copy(info.actionName, sizeof(info.actionName), "gaze");
        cstr_copy(info.localizedActionName, sizeof(info.localizedActionName), "Gaze");
        ASSERT_EQ(xr.xrCreateAction(set, &info, &gaze), XR_SUCCESS);
        const XrActionSuggestedBinding             binding   = { gaze, path("/user/eyes_ext/input/gaze_ext/pose") };
        const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/ext/eye_gaze_interaction"), .countSuggestedBindings = 1, .suggestedBindings = &binding };
        ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
        suggest_touch_bindings();
        attach();
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    }
};
} // namespace

TEST_F(OxrEyeGazeTest, system_supports_eye_gaze_interaction)
{
    XrSystemEyeGazeInteractionPropertiesEXT gaze_props = { .type = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT };
    XrSystemProperties                      props      = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &gaze_props };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(gaze_props.supportsEyeGazeInteraction, XR_TRUE);
}

TEST_F(OxrActionTest, eye_gaze_interaction_needs_its_extension)
{
    XrSystemEyeGazeInteractionPropertiesEXT gaze_props = { .type = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT, .supportsEyeGazeInteraction = XR_TRUE };
    XrSystemProperties                      props      = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &gaze_props };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(gaze_props.supportsEyeGazeInteraction, XR_FALSE);
    const XrActionSuggestedBinding             binding   = { grip, path("/user/eyes_ext/input/gaze_ext/pose") };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/ext/eye_gaze_interaction"), .countSuggestedBindings = 1, .suggestedBindings = &binding };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED);
}

// The gaze pose is between the eyes, looking where the simulated eyes look; the hands keep
// their own profile
TEST_F(OxrEyeGazeTest, the_gaze_pose_follows_the_simulated_eyes)
{
    begin_session();
    SIM_Sim*      sim   = oxr_get_sim();
    SIM_FaceState state = sim_get_face(sim);
    state.gaze_yaw      = 20.0f;
    sim_set_face(sim, state);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);

    XrInteractionProfileState profile = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
    ASSERT_EQ(xr.xrGetCurrentInteractionProfile(session, eyes, &profile), XR_SUCCESS);
    EXPECT_EQ(profile.interactionProfile, path("/interaction_profiles/ext/eye_gaze_interaction"));
    ASSERT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[1], &profile), XR_SUCCESS);
    EXPECT_NE(profile.interactionProfile, path("/interaction_profiles/ext/eye_gaze_interaction"));

    XrActionStatePose        pose_state = { .type = XR_TYPE_ACTION_STATE_POSE };
    const XrActionStateGetInfo get      = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = gaze };
    ASSERT_EQ(xr.xrGetActionStatePose(session, &get, &pose_state), XR_SUCCESS);
    EXPECT_EQ(pose_state.isActive, XR_TRUE);

    XrSpace                             space      = XR_NULL_HANDLE;
    const XrActionSpaceCreateInfo       space_info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = gaze, .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    ASSERT_EQ(xr.xrCreateActionSpace(session, &space_info, &space), XR_SUCCESS);
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    const XrTime    time     = sim_predicted_display_time(sim);
    ASSERT_EQ(xr.xrLocateSpace(space, stage, time, &location), XR_SUCCESS);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);

    SIM_Sample sample = {};
    sim_sample(sim, time, sample);
    SIM_FaceFrame face = {};
    sim_face_frame(sim, sample, face);
    const Vec3 between = (face.eyes[0].position + face.eyes[1].position) * 0.5f;
    EXPECT_TRUE(zm::near(Vec3{ location.pose.position.x, location.pose.position.y, location.pose.position.z }, between, 1e-5f));
    const Quat orientation = { location.pose.orientation.x, location.pose.orientation.y, location.pose.orientation.z, location.pose.orientation.w };
    const Vec3 forward     = zm::rotate(orientation, { 0.0f, 0.0f, -1.0f });
    EXPECT_GT(zm::dot(forward, zm::rotate(face.eyes[0].orientation, { 0.0f, 0.0f, -1.0f })), 0.9999f);
    EXPECT_LT(forward.x, -0.3f) << "20 degrees to the left";

    // Without a tracked head there is no gaze
    sim_set_device_active(sim, SIM_Device::Head, false);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    ASSERT_EQ(xr.xrGetActionStatePose(session, &get, &pose_state), XR_SUCCESS);
    EXPECT_EQ(pose_state.isActive, XR_FALSE);
}

// Spec XR_EXT_eye_gaze_interaction: XrEyeGazeSampleTimeEXT chained to the location of the gaze
// action space carries the time the gaze was sampled; other spaces leave it alone
TEST_F(OxrEyeGazeTest, locating_the_gaze_fills_the_sample_time)
{
    begin_session();
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);

    XrSpace                       space      = XR_NULL_HANDLE;
    const XrActionSpaceCreateInfo space_info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = gaze, .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    ASSERT_EQ(xr.xrCreateActionSpace(session, &space_info, &space), XR_SUCCESS);

    const XrTime           time     = sim_predicted_display_time(oxr_get_sim());
    XrEyeGazeSampleTimeEXT sample   = { .type = XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT };
    XrSpaceLocation        location = { .type = XR_TYPE_SPACE_LOCATION, .next = &sample };
    ASSERT_EQ(xr.xrLocateSpace(space, stage, time, &location), XR_SUCCESS);
    EXPECT_EQ(sample.time, time);

    // A head pose is not a gaze, so the runtime has no sample time to report
    XrEyeGazeSampleTimeEXT untouched = { .type = XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT, .time = 7 };
    XrSpaceLocation        view      = { .type = XR_TYPE_SPACE_LOCATION, .next = &untouched };
    ASSERT_EQ(xr.xrLocateSpace(reference_space(XR_REFERENCE_SPACE_TYPE_VIEW), stage, time, &view), XR_SUCCESS);
    EXPECT_EQ(untouched.time, 7);

    // No sample while the head is untracked: the gaze pose is not valid either
    sim_set_device_active(oxr_get_sim(), SIM_Device::Head, false);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    XrEyeGazeSampleTimeEXT gone    = { .type = XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT, .time = 7 };
    XrSpaceLocation        dropped = { .type = XR_TYPE_SPACE_LOCATION, .next = &gone };
    ASSERT_EQ(xr.xrLocateSpace(space, stage, sim_predicted_display_time(oxr_get_sim()), &dropped), XR_SUCCESS);
    EXPECT_FALSE(dropped.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    EXPECT_EQ(gone.time, 0) << "no valid gaze, no sample time";
}

//
// Body tracking: XR_FB_body_tracking, XR_META_body_tracking_full_body
//

namespace
{
struct OxrBodyTest : OxrActionTest
{
    PFN_xrCreateBodyTrackerFB  create_tracker  = nullptr;
    PFN_xrDestroyBodyTrackerFB destroy_tracker = nullptr;
    PFN_xrLocateBodyJointsFB   locate_joints   = nullptr;
    PFN_xrGetBodySkeletonFB    get_skeleton    = nullptr;
    XrSpace                    stage           = XR_NULL_HANDLE;

    explicit OxrBodyTest (b8 full_body = true)
    {
        more_extensions[more_extensions_count++] = XR_FB_BODY_TRACKING_EXTENSION_NAME;
        if (full_body)
        {
            more_extensions[more_extensions_count++] = XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME;
            more_extensions[more_extensions_count++] = XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME;
            more_extensions[more_extensions_count++] = XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME;
        }
    }

    void SetUp () override
    {
        OxrActionTest::SetUp();
        ASSERT_EQ(gipa(instance, "xrCreateBodyTrackerFB", (PFN_xrVoidFunction*)&create_tracker), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrDestroyBodyTrackerFB", (PFN_xrVoidFunction*)&destroy_tracker), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrLocateBodyJointsFB", (PFN_xrVoidFunction*)&locate_joints), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetBodySkeletonFB", (PFN_xrVoidFunction*)&get_skeleton), XR_SUCCESS);
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    }

    XrBodyTrackerFB tracker (XrBodyJointSetFB joint_set)
    {
        XrBodyTrackerFB                 out  = XR_NULL_HANDLE;
        const XrBodyTrackerCreateInfoFB info = { .type = XR_TYPE_BODY_TRACKER_CREATE_INFO_FB, .bodyJointSet = joint_set };
        EXPECT_EQ(create_tracker(session, &info, &out), XR_SUCCESS);
        return out;
    }

    XrResult locate (XrBodyTrackerFB body_tracker, XrBodyJointLocationFB* joints, u32 count, XrBodyJointLocationsFB& out_locations)
    {
        const XrBodyJointsLocateInfoFB info = { .type = XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB, .baseSpace = stage, .time = sim_predicted_display_time(oxr_get_sim()) };
        out_locations                       = { .type = XR_TYPE_BODY_JOINT_LOCATIONS_FB, .jointCount = count, .jointLocations = joints };
        return locate_joints(body_tracker, &info, &out_locations);
    }
};

struct OxrBodyFbTest : OxrBodyTest
{
    OxrBodyFbTest () : OxrBodyTest(false) {}
};

Vec3 body_vec (const XrVector3f& v)
{
    return { v.x, v.y, v.z };
}

Quat body_quat (const XrQuaternionf& q)
{
    return { q.x, q.y, q.z, q.w };
}
} // namespace

TEST_F(OxrBodyTest, system_supports_body_tracking)
{
    XrSystemPropertiesBodyTrackingFullBodyMETA full     = { .type = XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META };
    XrSystemBodyTrackingPropertiesFB           tracking = { .type = XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB, .next = &full };
    XrSystemProperties                         props    = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &tracking };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(tracking.supportsBodyTracking, XR_TRUE);
    EXPECT_EQ(full.supportsFullBodyTracking, XR_TRUE);
}

TEST_F(OxrActionTest, body_tracking_needs_its_extension)
{
    PFN_xrVoidFunction function = nullptr;
    EXPECT_EQ(gipa(instance, "xrCreateBodyTrackerFB", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrGetBodySkeletonFB", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
    XrSystemBodyTrackingPropertiesFB tracking = { .type = XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB, .supportsBodyTracking = XR_TRUE };
    XrSystemProperties               props    = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &tracking };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(tracking.supportsBodyTracking, XR_FALSE);
}

TEST_F(OxrBodyFbTest, fidelity_and_calibration_need_their_extensions)
{
    PFN_xrVoidFunction function = nullptr;
    EXPECT_EQ(gipa(instance, "xrRequestBodyTrackingFidelityMETA", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrSuggestBodyTrackingCalibrationOverrideMETA", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrResetBodyTrackingCalibrationMETA", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
}

// XR_META_body_tracking_fidelity: high moves the fingers with the simulated hands, low keeps
// them at rest; the status reports the tracker's
TEST_F(OxrBodyTest, fidelity_decides_whether_the_fingers_move)
{
    PFN_xrRequestBodyTrackingFidelityMETA request = nullptr;
    ASSERT_EQ(gipa(instance, "xrRequestBodyTrackingFidelityMETA", (PFN_xrVoidFunction*)&request), XR_SUCCESS);
    XrSystemPropertiesBodyTrackingFidelityMETA supported = { .type = XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META };
    XrSystemProperties                         props     = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &supported };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(supported.supportsBodyTrackingFidelity, XR_TRUE);

    begin_session();
    frame();
    const XrBodyTrackerFB body_tracker = tracker(XR_BODY_JOINT_SET_FULL_BODY_META);
    EXPECT_EQ(request(XR_NULL_HANDLE, XR_BODY_TRACKING_FIDELITY_LOW_META), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(request(body_tracker, (XrBodyTrackingFidelityMETA)3), XR_ERROR_VALIDATION_FAILURE);

    sim_set_input(oxr_get_sim(), SIM_Device::RightHand, SIM_Input::SqueezeValue, { { 1.0f, 0.0f } });
    frame();
    XrBodyTrackingFidelityStatusMETA status    = { .type = XR_TYPE_BODY_TRACKING_FIDELITY_STATUS_META };
    XrBodyJointLocationFB            high[XR_FULL_BODY_JOINT_COUNT_META] = {};
    XrBodyJointLocationsFB           locations = {};
    ASSERT_EQ(locate(body_tracker, high, XR_FULL_BODY_JOINT_COUNT_META, locations), XR_SUCCESS);
    locations.next = &status;
    const XrBodyJointsLocateInfoFB info = { .type = XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB, .baseSpace = stage, .time = sim_predicted_display_time(oxr_get_sim()) };
    ASSERT_EQ(locate_joints(body_tracker, &info, &locations), XR_SUCCESS);
    EXPECT_EQ(status.fidelity, XR_BODY_TRACKING_FIDELITY_HIGH_META);

    ASSERT_EQ(request(body_tracker, XR_BODY_TRACKING_FIDELITY_LOW_META), XR_SUCCESS);
    XrBodyJointLocationFB low[XR_FULL_BODY_JOINT_COUNT_META] = {};
    locations = { .type = XR_TYPE_BODY_JOINT_LOCATIONS_FB, .next = &status, .jointCount = XR_FULL_BODY_JOINT_COUNT_META, .jointLocations = low };
    ASSERT_EQ(locate_joints(body_tracker, &info, &locations), XR_SUCCESS);
    EXPECT_EQ(status.fidelity, XR_BODY_TRACKING_FIDELITY_LOW_META);
    EXPECT_TRUE(zm::near(body_vec(low[XR_BODY_JOINT_RIGHT_HAND_WRIST_FB].pose.position), body_vec(high[XR_BODY_JOINT_RIGHT_HAND_WRIST_FB].pose.position), 1e-5f)) << "the wrist stays";
    EXPECT_GT(zm::length(body_vec(low[XR_BODY_JOINT_RIGHT_HAND_MIDDLE_TIP_FB].pose.position) - body_vec(high[XR_BODY_JOINT_RIGHT_HAND_MIDDLE_TIP_FB].pose.position)), 0.02f) << "the fist opens";
}

// XR_META_body_tracking_calibration: the height an app suggests reshapes the body, a reset goes
// back to the configured one
TEST_F(OxrBodyTest, calibration_overrides_the_height)
{
    PFN_xrSuggestBodyTrackingCalibrationOverrideMETA suggest = nullptr;
    PFN_xrResetBodyTrackingCalibrationMETA           reset   = nullptr;
    ASSERT_EQ(gipa(instance, "xrSuggestBodyTrackingCalibrationOverrideMETA", (PFN_xrVoidFunction*)&suggest), XR_SUCCESS);
    ASSERT_EQ(gipa(instance, "xrResetBodyTrackingCalibrationMETA", (PFN_xrVoidFunction*)&reset), XR_SUCCESS);
    XrSystemPropertiesBodyTrackingCalibrationMETA supported = { .type = XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_CALIBRATION_META };
    XrSystemProperties                            props     = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &supported };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(supported.supportsHeightOverride, XR_TRUE);

    begin_session();
    frame();
    const XrBodyTrackerFB body_tracker = tracker(XR_BODY_JOINT_SET_FULL_BODY_META);
    XrBodyTrackingCalibrationInfoMETA calibration = { .type = XR_TYPE_BODY_TRACKING_CALIBRATION_INFO_META, .bodyHeight = 1.6f };
    EXPECT_EQ(suggest(XR_NULL_HANDLE, &calibration), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(suggest(body_tracker, nullptr), XR_ERROR_VALIDATION_FAILURE);
    calibration.bodyHeight = 0.4f;
    EXPECT_EQ(suggest(body_tracker, &calibration), XR_ERROR_VALIDATION_FAILURE) << "spec: out of (0.5 m, 3 m)";
    calibration.bodyHeight = 3.5f;
    EXPECT_EQ(suggest(body_tracker, &calibration), XR_ERROR_VALIDATION_FAILURE);
    calibration.type = XR_TYPE_BODY_TRACKER_CREATE_INFO_FB;
    EXPECT_EQ(suggest(body_tracker, &calibration), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(reset(XR_NULL_HANDLE), XR_ERROR_HANDLE_INVALID);

    const u32 before   = sim_body_generation(oxr_get_sim());
    calibration        = { .type = XR_TYPE_BODY_TRACKING_CALIBRATION_INFO_META, .bodyHeight = 1.6f };
    ASSERT_EQ(suggest(body_tracker, &calibration), XR_SUCCESS);
    EXPECT_FLOAT_EQ(sim_get_body(oxr_get_sim()).height, 1.6f);
    XrBodyTrackingCalibrationStatusMETA status    = { .type = XR_TYPE_BODY_TRACKING_CALIBRATION_STATUS_META };
    XrBodyJointLocationFB               joints[XR_FULL_BODY_JOINT_COUNT_META] = {};
    XrBodyJointLocationsFB              locations = { .type = XR_TYPE_BODY_JOINT_LOCATIONS_FB, .next = &status, .jointCount = XR_FULL_BODY_JOINT_COUNT_META, .jointLocations = joints };
    const XrBodyJointsLocateInfoFB      info      = { .type = XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB, .baseSpace = stage, .time = sim_predicted_display_time(oxr_get_sim()) };
    ASSERT_EQ(locate_joints(body_tracker, &info, &locations), XR_SUCCESS);
    EXPECT_EQ(status.status, XR_BODY_TRACKING_CALIBRATION_STATE_VALID_META);
    EXPECT_GT(locations.skeletonChangedCount, before) << "the skeleton changed";

    ASSERT_EQ(reset(body_tracker), XR_SUCCESS);
    EXPECT_FLOAT_EQ(sim_get_body(oxr_get_sim()).height, 1.75f);
}

TEST_F(OxrBodyFbTest, tracker_creation_is_validated)
{
    XrBodyTrackerFB           out  = XR_NULL_HANDLE;
    XrBodyTrackerCreateInfoFB info = { .type = XR_TYPE_BODY_TRACKER_CREATE_INFO_FB, .bodyJointSet = XR_BODY_JOINT_SET_DEFAULT_FB };
    EXPECT_EQ(create_tracker(XR_NULL_HANDLE, &info, &out), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(create_tracker(session, nullptr, &out), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_tracker(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    info.type = XR_TYPE_SESSION_CREATE_INFO;
    EXPECT_EQ(create_tracker(session, &info, &out), XR_ERROR_VALIDATION_FAILURE);
    info.type         = XR_TYPE_BODY_TRACKER_CREATE_INFO_FB;
    info.bodyJointSet = (XrBodyJointSetFB)5;
    EXPECT_EQ(create_tracker(session, &info, &out), XR_ERROR_VALIDATION_FAILURE);
    info.bodyJointSet = XR_BODY_JOINT_SET_FULL_BODY_META;
    EXPECT_EQ(create_tracker(session, &info, &out), XR_ERROR_VALIDATION_FAILURE) << "XR_META_body_tracking_full_body is not enabled";
    info.bodyJointSet = XR_BODY_JOINT_SET_DEFAULT_FB;
    ASSERT_EQ(create_tracker(session, &info, &out), XR_SUCCESS);
    EXPECT_EQ(destroy_tracker(out), XR_SUCCESS);
    EXPECT_EQ(destroy_tracker(out), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(destroy_tracker(XR_NULL_HANDLE), XR_ERROR_HANDLE_INVALID);

    ASSERT_EQ(create_tracker(session, &info, &out), XR_SUCCESS);
    ASSERT_EQ(xr.xrDestroySession(session), XR_SUCCESS);
    session = XR_NULL_HANDLE;
    EXPECT_EQ(destroy_tracker(out), XR_ERROR_HANDLE_INVALID) << "the session's trackers go with it";
}

TEST_F(OxrBodyTest, locate_and_skeleton_are_validated)
{
    begin_session();
    const XrBodyTrackerFB  body_tracker = tracker(XR_BODY_JOINT_SET_FULL_BODY_META);
    XrBodyJointLocationFB  joints[XR_FULL_BODY_JOINT_COUNT_META] = {};
    XrBodyJointsLocateInfoFB info      = { .type = XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB, .baseSpace = stage, .time = sim_predicted_display_time(oxr_get_sim()) };
    XrBodyJointLocationsFB   locations = { .type = XR_TYPE_BODY_JOINT_LOCATIONS_FB, .jointCount = XR_FULL_BODY_JOINT_COUNT_META, .jointLocations = joints };
    EXPECT_EQ(locate_joints(XR_NULL_HANDLE, &info, &locations), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(locate_joints(body_tracker, nullptr, &locations), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(locate_joints(body_tracker, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    locations.jointCount = XR_BODY_JOINT_COUNT_FB;
    EXPECT_EQ(locate_joints(body_tracker, &info, &locations), XR_ERROR_VALIDATION_FAILURE) << "a full body tracker has 84 joints";
    locations.jointCount     = XR_FULL_BODY_JOINT_COUNT_META;
    locations.jointLocations = nullptr;
    EXPECT_EQ(locate_joints(body_tracker, &info, &locations), XR_ERROR_VALIDATION_FAILURE);
    locations.jointLocations = joints;
    locations.type           = XR_TYPE_HAND_JOINT_LOCATIONS_EXT;
    EXPECT_EQ(locate_joints(body_tracker, &info, &locations), XR_ERROR_VALIDATION_FAILURE);
    locations.type = XR_TYPE_BODY_JOINT_LOCATIONS_FB;
    info.time      = 0;
    EXPECT_EQ(locate_joints(body_tracker, &info, &locations), XR_ERROR_TIME_INVALID);
    info.time      = sim_predicted_display_time(oxr_get_sim());
    info.baseSpace = XR_NULL_HANDLE;
    EXPECT_EQ(locate_joints(body_tracker, &info, &locations), XR_ERROR_HANDLE_INVALID);
    info.baseSpace = stage;
    EXPECT_EQ(locate_joints(body_tracker, &info, &locations), XR_SUCCESS);

    XrBodySkeletonJointFB skeleton_joints[XR_FULL_BODY_JOINT_COUNT_META] = {};
    XrBodySkeletonFB      skeleton = { .type = XR_TYPE_BODY_SKELETON_FB, .jointCount = XR_BODY_JOINT_COUNT_FB, .joints = skeleton_joints };
    EXPECT_EQ(get_skeleton(XR_NULL_HANDLE, &skeleton), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(get_skeleton(body_tracker, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(get_skeleton(body_tracker, &skeleton), XR_ERROR_VALIDATION_FAILURE);
    skeleton.jointCount = XR_FULL_BODY_JOINT_COUNT_META;
    skeleton.joints     = nullptr;
    EXPECT_EQ(get_skeleton(body_tracker, &skeleton), XR_ERROR_VALIDATION_FAILURE);
    skeleton.joints = skeleton_joints;
    skeleton.type   = XR_TYPE_BODY_JOINT_LOCATIONS_FB;
    EXPECT_EQ(get_skeleton(body_tracker, &skeleton), XR_ERROR_VALIDATION_FAILURE);
    skeleton.type = XR_TYPE_BODY_SKELETON_FB;
    EXPECT_EQ(get_skeleton(body_tracker, &skeleton), XR_SUCCESS);
}

// The skeleton is the simulator's T-pose; the default joint set is the first 70 of the full body
TEST_F(OxrBodyTest, skeleton_is_the_simulated_t_pose)
{
    SIM_BodySkeleton expected = {};
    sim_body_skeleton(oxr_get_sim(), expected);
    for (const XrBodyJointSetFB joint_set : { XR_BODY_JOINT_SET_DEFAULT_FB, XR_BODY_JOINT_SET_FULL_BODY_META })
    {
        const b8              full         = joint_set == XR_BODY_JOINT_SET_FULL_BODY_META;
        const u32             count        = full ? (u32)XR_FULL_BODY_JOINT_COUNT_META : (u32)XR_BODY_JOINT_COUNT_FB;
        const XrBodyTrackerFB body_tracker = tracker(joint_set);
        XrBodySkeletonJointFB joints[XR_FULL_BODY_JOINT_COUNT_META] = {};
        XrBodySkeletonFB      skeleton = { .type = XR_TYPE_BODY_SKELETON_FB, .jointCount = count, .joints = joints };
        ASSERT_EQ(get_skeleton(body_tracker, &skeleton), XR_SUCCESS);
        for (u32 j = 0; j < count; j++)
        {
            EXPECT_EQ(joints[j].joint, (i32)j);
            const i32 none = full ? (i32)XR_FULL_BODY_JOINT_NONE_META : -1;
            EXPECT_EQ(joints[j].parentJoint, expected.parents[j] == SIM_BODY_NO_PARENT ? none : (i32)expected.parents[j]) << j;
            EXPECT_TRUE(zm::near(body_vec(joints[j].pose.position), expected.poses[j].position, 1e-6f)) << j;
            EXPECT_TRUE(zm::near(body_quat(joints[j].pose.orientation), expected.poses[j].orientation, 1e-6f)) << j;
        }
        EXPECT_EQ(joints[XR_BODY_JOINT_LEFT_HAND_WRIST_FB].parentJoint, XR_BODY_JOINT_LEFT_HAND_WRIST_TWIST_FB) << "Meta's hierarchy";
        EXPECT_EQ(destroy_tracker(body_tracker), XR_SUCCESS);
    }
}

TEST_F(OxrBodyTest, joints_are_the_simulated_body_in_the_base_space)
{
    begin_session();
    frame();
    const XrBodyTrackerFB full    = tracker(XR_BODY_JOINT_SET_FULL_BODY_META);
    const XrBodyTrackerFB upper   = tracker(XR_BODY_JOINT_SET_DEFAULT_FB);
    XrBodyJointLocationFB joints[XR_FULL_BODY_JOINT_COUNT_META] = {};
    XrBodyJointLocationsFB locations = {};
    ASSERT_EQ(locate(full, joints, XR_FULL_BODY_JOINT_COUNT_META, locations), XR_SUCCESS);
    EXPECT_EQ(locations.isActive, XR_TRUE);
    EXPECT_FLOAT_EQ(locations.confidence, 1.0f);
    EXPECT_EQ(locations.skeletonChangedCount, sim_body_generation(oxr_get_sim()));
    EXPECT_EQ(locations.time, sim_predicted_display_time(oxr_get_sim()));

    SIM_Sim*   sim    = oxr_get_sim();
    SIM_Sample sample = {};
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    SIM_BodyJoints expected = {};
    sim_body_joints(sim, sample, expected);
    const XrSpaceLocationFlags all = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    for (u32 j = 0; j < XR_FULL_BODY_JOINT_COUNT_META; j++)
    {
        EXPECT_EQ(joints[j].locationFlags, all) << j;
        EXPECT_TRUE(zm::near(body_vec(joints[j].pose.position), expected.poses[j].position, 1e-4f)) << sim_body_joint_name((SIM_BodyJoint)j);
    }

    XrBodyJointLocationFB fewer[XR_BODY_JOINT_COUNT_FB] = {};
    ASSERT_EQ(locate(upper, fewer, XR_BODY_JOINT_COUNT_FB, locations), XR_SUCCESS);
    for (u32 j = 0; j < XR_BODY_JOINT_COUNT_FB; j++)
    {
        EXPECT_EQ(fewer[j].locationFlags, joints[j].locationFlags);
        EXPECT_TRUE(zm::equal(body_vec(fewer[j].pose.position), body_vec(joints[j].pose.position)) && zm::equal(body_quat(fewer[j].pose.orientation), body_quat(joints[j].pose.orientation))) << "the default set is the full body's first 70, " << j;
    }

    SIM_BodyState state = sim_get_body(sim);
    state.tracked[(u32)SIM_BodyJoint::LeftArmLower]          = false;
    state.bone_scales[(u32)SIM_BodyJoint::RightArmUpper] = 1.2f;
    sim_set_body(sim, state);
    ASSERT_EQ(locate(full, joints, XR_FULL_BODY_JOINT_COUNT_META, locations), XR_SUCCESS);
    EXPECT_EQ(locations.skeletonChangedCount, sim_body_generation(oxr_get_sim()));
    EXPECT_NE(locations.skeletonChangedCount, 0u) << "a shape change raises the count";
    EXPECT_EQ(joints[XR_BODY_JOINT_LEFT_ARM_LOWER_FB].locationFlags, XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) << "an untracked joint stays valid while the body is active";
}

//
// Hand tracking: XR_EXT_hand_tracking, XR_FB_hand_tracking_aim, _capsules, _mesh,
// XR_EXT_hand_tracking_data_source, XR_EXT_hand_joints_motion_range, XR_EXT_hand_interaction
//

namespace
{
struct OxrHandTest : OxrActionTest
{
    PFN_xrCreateHandTrackerEXT  create_tracker  = nullptr;
    PFN_xrDestroyHandTrackerEXT destroy_tracker = nullptr;
    PFN_xrLocateHandJointsEXT   locate_joints   = nullptr;
    PFN_xrGetHandMeshFB         get_mesh        = nullptr;
    XrSpace                     stage           = XR_NULL_HANDLE;

    OxrHandTest ()
    {
        more_extensions[more_extensions_count++] = XR_EXT_HAND_TRACKING_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_FB_HAND_TRACKING_CAPSULES_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_EXT_HAND_JOINTS_MOTION_RANGE_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_EXT_HAND_INTERACTION_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrActionTest::SetUp();
        ASSERT_EQ(gipa(instance, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction*)&create_tracker), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrDestroyHandTrackerEXT", (PFN_xrVoidFunction*)&destroy_tracker), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrLocateHandJointsEXT", (PFN_xrVoidFunction*)&locate_joints), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetHandMeshFB", (PFN_xrVoidFunction*)&get_mesh), XR_SUCCESS);
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    }

    XrHandTrackerEXT tracker (XrHandEXT hand, const void* next = nullptr)
    {
        XrHandTrackerEXT                    out  = XR_NULL_HANDLE;
        const XrHandTrackerCreateInfoEXT    info = { .type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, .next = next, .hand = hand, .handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT };
        EXPECT_EQ(create_tracker(session, &info, &out), XR_SUCCESS);
        return out;
    }

    XrResult locate (XrHandTrackerEXT hand_tracker, XrHandJointLocationEXT (&joints)[XR_HAND_JOINT_COUNT_EXT], XrBool32& active, void* next = nullptr, const void* info_next = nullptr)
    {
        const XrHandJointsLocateInfoEXT info      = { .type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, .next = info_next, .baseSpace = stage, .time = sim_predicted_display_time(oxr_get_sim()) };
        XrHandJointLocationsEXT         locations = { .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT, .next = next, .jointCount = XR_HAND_JOINT_COUNT_EXT, .jointLocations = joints };
        const XrResult                  result    = locate_joints(hand_tracker, &info, &locations);
        active                                    = locations.isActive;
        return result;
    }

    static Vec3 vec (const XrVector3f& v)
    {
        return { v.x, v.y, v.z };
    }
};
} // namespace

TEST_F(OxrHandTest, system_supports_hand_tracking)
{
    XrSystemHandTrackingPropertiesEXT tracking = { .type = XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT };
    XrSystemProperties                props    = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &tracking };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(tracking.supportsHandTracking, XR_TRUE);
}

TEST_F(OxrActionTest, hand_tracking_needs_its_extension)
{
    PFN_xrVoidFunction function = nullptr;
    EXPECT_EQ(gipa(instance, "xrCreateHandTrackerEXT", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrGetHandMeshFB", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
    const XrActionSuggestedBinding pinch = { trigger, path("/user/hand/right/input/pinch_ext/value") };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/ext/hand_interaction_ext"), .countSuggestedBindings = 1, .suggestedBindings = &pinch };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED);
}

TEST_F(OxrHandTest, tracker_creation_is_validated)
{
    XrHandTrackerEXT           out  = XR_NULL_HANDLE;
    XrHandTrackerCreateInfoEXT info = { .type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, .hand = XR_HAND_LEFT_EXT, .handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT };
    EXPECT_EQ(create_tracker(XR_NULL_HANDLE, &info, &out), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(create_tracker(session, nullptr, &out), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_tracker(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    info.hand = (XrHandEXT)3;
    EXPECT_EQ(create_tracker(session, &info, &out), XR_ERROR_VALIDATION_FAILURE);
    info.hand         = XR_HAND_RIGHT_EXT;
    info.handJointSet = XR_HAND_JOINT_SET_HAND_WITH_FOREARM_ULTRALEAP;
    EXPECT_EQ(create_tracker(session, &info, &out), XR_ERROR_VALIDATION_FAILURE) << "XR_ULTRALEAP_hand_tracking_forearm is not enabled";
    info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
    info.type         = XR_TYPE_SESSION_CREATE_INFO;
    EXPECT_EQ(create_tracker(session, &info, &out), XR_ERROR_VALIDATION_FAILURE);
    info.type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT;

    XrHandTrackingDataSourceEXT       sources[2] = { XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT, XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT };
    XrHandTrackingDataSourceInfoEXT   data       = { .type = XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT, .requestedDataSourceCount = 0, .requestedDataSources = sources };
    info.next = &data;
    EXPECT_EQ(create_tracker(session, &info, &out), XR_ERROR_VALIDATION_FAILURE);
    data.requestedDataSourceCount = 2;
    EXPECT_EQ(create_tracker(session, &info, &out), XR_ERROR_VALIDATION_FAILURE) << "duplicate data sources";
    sources[1] = (XrHandTrackingDataSourceEXT)7;
    EXPECT_EQ(create_tracker(session, &info, &out), XR_ERROR_VALIDATION_FAILURE);
    sources[1] = XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT;
    ASSERT_EQ(create_tracker(session, &info, &out), XR_SUCCESS);

    EXPECT_EQ(destroy_tracker(out), XR_SUCCESS);
    EXPECT_EQ(destroy_tracker(out), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(destroy_tracker(XR_NULL_HANDLE), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrHandTest, locate_is_validated)
{
    begin_session();
    const XrHandTrackerEXT  hand_tracker = tracker(XR_HAND_RIGHT_EXT);
    XrHandJointLocationEXT  joints[XR_HAND_JOINT_COUNT_EXT] = {};
    XrHandJointsLocateInfoEXT info      = { .type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, .baseSpace = stage, .time = sim_predicted_display_time(oxr_get_sim()) };
    XrHandJointLocationsEXT   locations = { .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT, .jointCount = XR_HAND_JOINT_COUNT_EXT, .jointLocations = joints };
    EXPECT_EQ(locate_joints(XR_NULL_HANDLE, &info, &locations), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(locate_joints(hand_tracker, nullptr, &locations), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(locate_joints(hand_tracker, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    locations.jointCount = 25;
    EXPECT_EQ(locate_joints(hand_tracker, &info, &locations), XR_ERROR_VALIDATION_FAILURE);
    locations.jointCount     = XR_HAND_JOINT_COUNT_EXT;
    locations.jointLocations = nullptr;
    EXPECT_EQ(locate_joints(hand_tracker, &info, &locations), XR_ERROR_VALIDATION_FAILURE);
    locations.jointLocations = joints;
    info.time                = 0;
    EXPECT_EQ(locate_joints(hand_tracker, &info, &locations), XR_ERROR_TIME_INVALID);
    info.time      = sim_predicted_display_time(oxr_get_sim());
    info.baseSpace = XR_NULL_HANDLE;
    EXPECT_EQ(locate_joints(hand_tracker, &info, &locations), XR_ERROR_HANDLE_INVALID);
    info.baseSpace = stage;

    XrHandJointVelocityEXT  velocities[XR_HAND_JOINT_COUNT_EXT] = {};
    XrHandJointVelocitiesEXT chained = { .type = XR_TYPE_HAND_JOINT_VELOCITIES_EXT, .jointCount = 3, .jointVelocities = velocities };
    locations.next = &chained;
    EXPECT_EQ(locate_joints(hand_tracker, &info, &locations), XR_ERROR_VALIDATION_FAILURE);
    chained.jointCount = XR_HAND_JOINT_COUNT_EXT;
    EXPECT_EQ(locate_joints(hand_tracker, &info, &locations), XR_SUCCESS);
}

TEST_F(OxrHandTest, tracked_hand_joints_follow_the_simulated_hand)
{
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 0.5f, 0.0f } });
    frame();

    const XrHandTrackerEXT hand_tracker = tracker(XR_HAND_RIGHT_EXT);
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT] = {};
    XrBool32               active = XR_FALSE;
    ASSERT_EQ(locate(hand_tracker, joints, active), XR_SUCCESS);
    ASSERT_TRUE(active);

    SIM_HandJoints expected = {};
    sim_hand_joints(sim, SIM_Device::RightHand, sim_get_device_pose(sim, SIM_Device::RightHand), SIM_HandMotion::Unobstructed, expected);
    const XrSpaceLocationFlags all = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    for (u32 j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++)
    {
        EXPECT_EQ(joints[j].locationFlags, all);
        EXPECT_TRUE(zm::near(vec(joints[j].pose.position), expected.poses[j].position, 1e-5f)) << j;
        EXPECT_FLOAT_EQ(joints[j].radius, expected.radii[j]);
    }

    // The left hand holds its controller, which plain hand tracking does not report
    const XrHandTrackerEXT left = tracker(XR_HAND_LEFT_EXT);
    ASSERT_EQ(locate(left, joints, active), XR_SUCCESS);
    EXPECT_FALSE(active);
    for (const XrHandJointLocationEXT& joint : joints)
    {
        EXPECT_EQ(joint.locationFlags, 0u);
    }

    sim_set_device_active(sim, SIM_Device::RightHand, false);
    frame();
    ASSERT_EQ(locate(hand_tracker, joints, active), XR_SUCCESS);
    EXPECT_FALSE(active);
}

TEST_F(OxrHandTest, data_sources_and_motion_range)
{
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    XrHandTrackingDataSourceEXT controller = XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT;
    const XrHandTrackingDataSourceInfoEXT from_controller = { .type = XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT, .requestedDataSourceCount = 1, .requestedDataSources = &controller };
    const XrHandTrackerEXT hand_tracker = tracker(XR_HAND_LEFT_EXT, &from_controller);

    XrHandTrackingDataSourceStateEXT state  = { .type = XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT };
    XrHandJointLocationEXT           joints[XR_HAND_JOINT_COUNT_EXT] = {};
    XrBool32                         active = XR_FALSE;
    ASSERT_EQ(locate(hand_tracker, joints, active, &state), XR_SUCCESS);
    EXPECT_TRUE(active);
    EXPECT_TRUE(state.isActive);
    EXPECT_EQ(state.dataSource, XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT);
    const Vec3 open_tip = vec(joints[XR_HAND_JOINT_MIDDLE_TIP_EXT].pose.position);

    const XrHandJointsMotionRangeInfoEXT conforming = { .type = XR_TYPE_HAND_JOINTS_MOTION_RANGE_INFO_EXT, .handJointsMotionRange = XR_HAND_JOINTS_MOTION_RANGE_CONFORMING_TO_CONTROLLER_EXT };
    ASSERT_EQ(locate(hand_tracker, joints, active, nullptr, &conforming), XR_SUCCESS);
    const Vec3 held_tip = vec(joints[XR_HAND_JOINT_MIDDLE_TIP_EXT].pose.position);
    const Vec3 palm     = vec(joints[XR_HAND_JOINT_PALM_EXT].pose.position);
    EXPECT_LT(zm::length(held_tip - palm), zm::length(open_tip - palm) - 0.03f);

    // A tracked hand is not what this tracker asked for
    sim_set_hand_source(sim, SIM_Device::LeftHand, SIM_HandSource::Hand);
    frame();
    ASSERT_EQ(locate(hand_tracker, joints, active, &state), XR_SUCCESS);
    EXPECT_FALSE(active);
    EXPECT_FALSE(state.isActive);

    const XrHandTrackerEXT unobstructed = tracker(XR_HAND_LEFT_EXT);
    ASSERT_EQ(locate(unobstructed, joints, active, &state), XR_SUCCESS);
    EXPECT_TRUE(active);
    EXPECT_EQ(state.dataSource, XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT);
}

TEST_F(OxrHandTest, fb_aim_capsules_scale_and_velocities)
{
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 1.0f, 0.0f } });
    Pose moved = sim_get_device_pose(sim, SIM_Device::RightHand);
    frame();
    moved.position.x += 0.01f;
    sim_set_device_pose(sim, SIM_Device::RightHand, moved);
    frame();

    const XrHandTrackerEXT         hand_tracker = tracker(XR_HAND_RIGHT_EXT);
    XrHandJointVelocityEXT         velocities[XR_HAND_JOINT_COUNT_EXT] = {};
    XrHandTrackingScaleFB          scale    = { .type = XR_TYPE_HAND_TRACKING_SCALE_FB };
    XrHandTrackingCapsulesStateFB  capsules = { .type = XR_TYPE_HAND_TRACKING_CAPSULES_STATE_FB, .next = &scale };
    XrHandTrackingAimStateFB       aim      = { .type = XR_TYPE_HAND_TRACKING_AIM_STATE_FB, .next = &capsules };
    XrHandJointVelocitiesEXT       chained  = { .type = XR_TYPE_HAND_JOINT_VELOCITIES_EXT, .next = &aim, .jointCount = XR_HAND_JOINT_COUNT_EXT, .jointVelocities = velocities };
    XrHandJointLocationEXT         joints[XR_HAND_JOINT_COUNT_EXT] = {};
    XrBool32                       active = XR_FALSE;
    ASSERT_EQ(locate(hand_tracker, joints, active, &chained), XR_SUCCESS);
    ASSERT_TRUE(active);

    EXPECT_TRUE(aim.status & XR_HAND_TRACKING_AIM_COMPUTED_BIT_FB);
    EXPECT_TRUE(aim.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB);
    EXPECT_TRUE(aim.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB);
    EXPECT_TRUE(aim.status & XR_HAND_TRACKING_AIM_DOMINANT_HAND_BIT_FB) << "the primary hand is the right one";
    EXPECT_FLOAT_EQ(aim.pinchStrengthIndex, 1.0f);
    EXPECT_FLOAT_EQ(aim.pinchStrengthMiddle, 0.0f);
    EXPECT_TRUE(zm::near(vec(aim.aimPose.position), sim_get_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Aim).position, 1e-5f));

    for (u32 c = 0; c < XR_HAND_TRACKING_CAPSULE_COUNT_FB; c++)
    {
        const XrHandCapsuleFB& capsule = capsules.capsules[c];
        ASSERT_LT((u32)capsule.joint, (u32)XR_HAND_JOINT_COUNT_EXT);
        EXPECT_TRUE(zm::near(vec(capsule.points[0]), vec(joints[capsule.joint].pose.position), 1e-6f)) << c;
        EXPECT_TRUE(zm::near(vec(capsule.points[1]), vec(joints[capsule.joint + 1].pose.position), 1e-6f)) << c;
        EXPECT_FLOAT_EQ(capsule.radius, joints[capsule.joint].radius);
    }
    EXPECT_EQ(capsules.capsules[0].joint, XR_HAND_JOINT_THUMB_METACARPAL_EXT);
    EXPECT_FLOAT_EQ(scale.sensorOutput, 1.0f);
    EXPECT_FLOAT_EQ(scale.currentOutput, 1.0f);

    const f32 speed = 0.01f / ((f32)sim_display_period(sim) * 1e-9f);
    for (const XrHandJointVelocityEXT& velocity : velocities)
    {
        EXPECT_EQ(velocity.velocityFlags, XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT);
        EXPECT_NEAR(velocity.linearVelocity.x, speed, speed * 1e-3f);
        EXPECT_NEAR(velocity.angularVelocity.y, 0.0f, 1e-4f);
    }

    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Controller);
    frame();
    ASSERT_EQ(locate(hand_tracker, joints, active, &chained), XR_SUCCESS);
    EXPECT_FALSE(active);
    EXPECT_EQ(aim.status, 0u);
}

TEST_F(OxrHandTest, fb_mesh_two_call)
{
    const XrHandTrackerEXT hand_tracker = tracker(XR_HAND_LEFT_EXT);
    XrHandTrackingMeshFB   mesh = { .type = XR_TYPE_HAND_TRACKING_MESH_FB };
    EXPECT_EQ(get_mesh(XR_NULL_HANDLE, &mesh), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(get_mesh(hand_tracker, nullptr), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(get_mesh(hand_tracker, &mesh), XR_SUCCESS);
    const SIM_HandMesh& expected = sim_get_hand_mesh(oxr_get_sim(), SIM_Device::LeftHand);
    ASSERT_EQ(mesh.jointCountOutput, (u32)XR_HAND_JOINT_COUNT_EXT);
    ASSERT_EQ(mesh.vertexCountOutput, expected.vertices_count);
    ASSERT_EQ(mesh.indexCountOutput, expected.indices_count);

    Array<XrPosef>      bind_poses = {};
    Array<f32>          radii = {};
    Array<XrHandJointEXT> parents = {};
    Array<XrVector3f>   positions = {};
    Array<XrVector3f>   normals = {};
    Array<XrVector2f>   uvs = {};
    Array<XrVector4sFB> blend_indices = {};
    Array<XrVector4f>   blend_weights = {};
    Array<i16>          indices = {};
    defer
    {
        bind_poses.release();
        radii.release();
        parents.release();
        positions.release();
        normals.release();
        uvs.release();
        blend_indices.release();
        blend_weights.release();
        indices.release();
    };
    bind_poses.resize(mesh.jointCountOutput);
    radii.resize(mesh.jointCountOutput);
    parents.resize(mesh.jointCountOutput);
    positions.resize(mesh.vertexCountOutput);
    normals.resize(mesh.vertexCountOutput);
    uvs.resize(mesh.vertexCountOutput);
    blend_indices.resize(mesh.vertexCountOutput);
    blend_weights.resize(mesh.vertexCountOutput);
    indices.resize(mesh.indexCountOutput);
    mesh = { .type = XR_TYPE_HAND_TRACKING_MESH_FB, .jointCapacityInput = XR_HAND_JOINT_COUNT_EXT, .jointBindPoses = bind_poses.data, .jointRadii = radii.data, .jointParents = parents.data,
             .vertexCapacityInput = expected.vertices_count - 1, .vertexPositions = positions.data, .vertexNormals = normals.data, .vertexUVs = uvs.data, .vertexBlendIndices = blend_indices.data, .vertexBlendWeights = blend_weights.data,
             .indexCapacityInput = expected.indices_count, .indices = indices.data };
    EXPECT_EQ(get_mesh(hand_tracker, &mesh), XR_ERROR_SIZE_INSUFFICIENT);
    mesh.vertexCapacityInput = expected.vertices_count;
    mesh.vertexNormals       = nullptr;
    EXPECT_EQ(get_mesh(hand_tracker, &mesh), XR_ERROR_VALIDATION_FAILURE);
    mesh.vertexNormals = normals.data;
    ASSERT_EQ(get_mesh(hand_tracker, &mesh), XR_SUCCESS);

    // Regression: the wrist named itself its parent, and Unity's OVRSkeleton, which takes only a
    // parent outside the joints for the root, parented the wrist to itself; Meta's hand meshes then
    // came out ten times too large, away from the hands (Meta XR Core SDK's ControllerDrivenHandPoses)
    EXPECT_EQ(parents[XR_HAND_JOINT_WRIST_EXT], XR_HAND_JOINT_MAX_ENUM_EXT);
    EXPECT_EQ(parents[XR_HAND_JOINT_PALM_EXT], XR_HAND_JOINT_WRIST_EXT);
    EXPECT_EQ(parents[XR_HAND_JOINT_RING_TIP_EXT], XR_HAND_JOINT_RING_DISTAL_EXT);
    EXPECT_TRUE(zm::near(vec(bind_poses[XR_HAND_JOINT_INDEX_TIP_EXT].position), expected.bind_poses[XR_HAND_JOINT_INDEX_TIP_EXT].position, 0.0f));
    for (u32 v = 0; v < expected.vertices_count; v++)
    {
        EXPECT_EQ(blend_indices[v].x, (i16)expected.joints[v]);
        EXPECT_FLOAT_EQ(blend_weights[v].x, 1.0f);
        EXPECT_FLOAT_EQ(blend_weights[v].y + blend_weights[v].z + blend_weights[v].w, 0.0f);
        EXPECT_TRUE(zm::near(vec(positions[v]), expected.positions[v], 0.0f));
    }
    for (u32 i = 0; i < expected.indices_count; i++)
    {
        EXPECT_EQ(indices[i], expected.indices[i]);
    }
}

// Regression: Meta's OVRPlugin turns the mesh into its 24 bone hand skeleton, which lacks the palm
// and the index, middle and ring metacarpals; a vertex skinned to one of them, in any slot, got
// bone 65535 and Unreal's hand component (Meta's Occlusion sample) failed an array check building it
TEST_F(OxrHandTest, fb_mesh_skins_only_to_joints_of_metas_hand_skeleton)
{
    const XrHandTrackerEXT hand_tracker = tracker(XR_HAND_RIGHT_EXT);
    const SIM_HandMesh&    expected     = sim_get_hand_mesh(oxr_get_sim(), SIM_Device::RightHand);
    XrPosef                bind_poses[XR_HAND_JOINT_COUNT_EXT];
    f32                    radii[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointEXT         parents[XR_HAND_JOINT_COUNT_EXT];
    Array<XrVector3f>      positions = {};
    Array<XrVector3f>      normals = {};
    Array<XrVector2f>      uvs = {};
    Array<XrVector4sFB>    blend_indices = {};
    Array<XrVector4f>      blend_weights = {};
    Array<i16>             indices = {};
    defer
    {
        positions.release();
        normals.release();
        uvs.release();
        blend_indices.release();
        blend_weights.release();
        indices.release();
    };
    positions.resize(expected.vertices_count);
    normals.resize(expected.vertices_count);
    uvs.resize(expected.vertices_count);
    blend_indices.resize(expected.vertices_count);
    blend_weights.resize(expected.vertices_count);
    indices.resize(expected.indices_count);
    XrHandTrackingMeshFB mesh = { .type = XR_TYPE_HAND_TRACKING_MESH_FB, .jointCapacityInput = XR_HAND_JOINT_COUNT_EXT, .jointBindPoses = bind_poses, .jointRadii = radii, .jointParents = parents,
                                  .vertexCapacityInput = expected.vertices_count, .vertexPositions = positions.data, .vertexNormals = normals.data, .vertexUVs = uvs.data, .vertexBlendIndices = blend_indices.data, .vertexBlendWeights = blend_weights.data,
                                  .indexCapacityInput = expected.indices_count, .indices = indices.data };
    ASSERT_EQ(get_mesh(hand_tracker, &mesh), XR_SUCCESS);

    const i16 missing[] = { XR_HAND_JOINT_PALM_EXT, XR_HAND_JOINT_INDEX_METACARPAL_EXT, XR_HAND_JOINT_MIDDLE_METACARPAL_EXT, XR_HAND_JOINT_RING_METACARPAL_EXT };
    b8        wrist     = false;
    for (u32 v = 0; v < expected.vertices_count; v++)
    {
        const i16 slots[4] = { blend_indices[v].x, blend_indices[v].y, blend_indices[v].z, blend_indices[v].w };
        for (const i16 joint : slots)
        {
            for (const i16 none : missing)
            {
                ASSERT_NE(joint, none) << "vertex " << v;
            }
        }
        wrist = wrist || slots[0] == XR_HAND_JOINT_WRIST_EXT;
    }
    EXPECT_TRUE(wrist) << "the palm's capsules move with the wrist";
}

// Each hand gets its own interaction profile: a tracked hand serves ext/hand_interaction_ext
// while the other one keeps its controller (spec 6.3.2, xrGetCurrentInteractionProfile per top level path)
TEST_F(OxrHandTest, a_tracked_hand_switches_only_its_own_profile)
{
    const XrAction pinch     = make_action("pinch", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrAction ready     = make_action("ready", XR_ACTION_TYPE_BOOLEAN_INPUT);
    const XrAction grasp     = make_action("grasp", XR_ACTION_TYPE_FLOAT_INPUT);
    const XrAction pinch_pos = make_action("pinch_pose", XR_ACTION_TYPE_POSE_INPUT);
    const XrActionSuggestedBinding hand_bindings[] = {
        { pinch,     path("/user/hand/left/input/pinch_ext/value") },
        { pinch,     path("/user/hand/right/input/pinch_ext/value") },
        { ready,     path("/user/hand/right/input/pinch_ext/ready_ext") },
        { grasp,     path("/user/hand/right/input/grasp_ext/value") },
        { pinch_pos, path("/user/hand/right/input/pinch_ext/pose") },
    };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/ext/hand_interaction_ext"), .countSuggestedBindings = ARRAY_SIZE(hand_bindings), .suggestedBindings = hand_bindings };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    suggest_touch_bindings();
    attach();
    begin_session();

    SIM_Sim* sim = oxr_get_sim();
    ASSERT_EQ(sync(), XR_SUCCESS);
    XrInteractionProfileState state = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
    ASSERT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[1], &state), XR_SUCCESS);
    EXPECT_EQ(state.interactionProfile, path("/interaction_profiles/oculus/touch_controller"));

    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 0.75f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::SqueezeValue, { .value = { 0.25f, 0.0f } });
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue, { .value = { 0.5f, 0.0f } });
    frame();
    drain_events();
    ASSERT_EQ(sync(), XR_SUCCESS);
    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    ASSERT_EQ(xr.xrPollEvent(instance, &event), XR_SUCCESS);
    EXPECT_EQ(event.type, XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED);

    ASSERT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[1], &state), XR_SUCCESS);
    EXPECT_EQ(state.interactionProfile, path("/interaction_profiles/ext/hand_interaction_ext"));
    ASSERT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[0], &state), XR_SUCCESS);
    EXPECT_EQ(state.interactionProfile, path("/interaction_profiles/oculus/touch_controller"));

    EXPECT_FLOAT_EQ(float_state(pinch, hands[1]).currentState, 0.75f);
    EXPECT_TRUE(bool_state(ready, hands[1]).currentState);
    EXPECT_FLOAT_EQ(float_state(grasp, hands[1]).currentState, 0.25f);
    EXPECT_FALSE(float_state(trigger, hands[1]).isActive) << "the right controller is put down";
    EXPECT_FLOAT_EQ(float_state(trigger, hands[0]).currentState, 0.5f);
    EXPECT_FALSE(float_state(pinch, hands[0]).isActive);

    XrSpace                     pinch_space = XR_NULL_HANDLE;
    const XrActionSpaceCreateInfo space_info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = pinch_pos, .subactionPath = hands[1], .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    ASSERT_EQ(xr.xrCreateActionSpace(session, &space_info, &pinch_space), XR_SUCCESS);
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(pinch_space, stage, sim_predicted_display_time(sim), &location), XR_SUCCESS);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    EXPECT_TRUE(zm::near(vec(location.pose.position), sim_get_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Pinch).position, 1e-4f));
}

// Hands also serve khr/simple_controller, its select being a pinch
TEST_F(OxrHandTest, tracked_hands_serve_the_simple_profile)
{
    const XrActionSuggestedBinding simple[] = {
        { click, path("/user/hand/right/input/select/click") },
        { grip,  path("/user/hand/right/input/grip/pose") },
    };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/khr/simple_controller"), .countSuggestedBindings = ARRAY_SIZE(simple), .suggestedBindings = simple };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    attach();
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 1.0f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    XrInteractionProfileState state = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
    ASSERT_EQ(xr.xrGetCurrentInteractionProfile(session, hands[1], &state), XR_SUCCESS);
    EXPECT_EQ(state.interactionProfile, path("/interaction_profiles/khr/simple_controller"));
    EXPECT_TRUE(bool_state(click, hands[1]).currentState);
}

// XR_EXT_hand_interaction adds pinch, poke and palm poses to the controller profiles too
TEST_F(OxrHandTest, controllers_have_pinch_and_poke_poses)
{
    const XrAction poke = make_action("poke", XR_ACTION_TYPE_POSE_INPUT);
    const XrActionSuggestedBinding touch[] = {
        { poke, path("/user/hand/left/input/poke_ext/pose") },
    };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = ARRAY_SIZE(touch), .suggestedBindings = touch };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    attach();
    begin_session();
    ASSERT_EQ(sync(), XR_SUCCESS);
    XrActionStatePose  pose_state = { .type = XR_TYPE_ACTION_STATE_POSE };
    const XrActionStateGetInfo info = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = poke, .subactionPath = hands[0] };
    ASSERT_EQ(xr.xrGetActionStatePose(session, &info, &pose_state), XR_SUCCESS);
    EXPECT_TRUE(pose_state.isActive);
}

TEST_F(OxrTest, hand_extensions_need_hand_tracking)
{
    const char* exts[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME };
    EXPECT_EQ(create_instance(exts, 2), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
}

// XR_META_simultaneous_hands_and_controllers: every simulated hand already switches between its
// controller and hand tracking on its own, so resuming and pausing only need to be valid calls
TEST_F(OxrHandTest, simultaneous_hands_and_controllers)
{
    xr.xrDestroySession(session);
    xr.xrDestroyInstance(instance);
    session  = XR_NULL_HANDLE;
    instance = XR_NULL_HANDLE;
    const char* exts[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME };
    ASSERT_EQ(create_instance(exts, 3), XR_SUCCESS);
    const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);

    XrSystemSimultaneousHandsAndControllersPropertiesMETA simultaneous = { .type = XR_TYPE_SYSTEM_SIMULTANEOUS_HANDS_AND_CONTROLLERS_PROPERTIES_META };
    XrSystemProperties props = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &simultaneous };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(simultaneous.supportsSimultaneousHandsAndControllers, XR_TRUE);

    const XrSessionCreateInfo session_info = { .type = XR_TYPE_SESSION_CREATE_INFO, .systemId = system };
    ASSERT_EQ(xr.xrCreateSession(instance, &session_info, &session), XR_SUCCESS);
    PFN_xrResumeSimultaneousHandsAndControllersTrackingMETA resume = nullptr;
    PFN_xrPauseSimultaneousHandsAndControllersTrackingMETA  pause  = nullptr;
    ASSERT_EQ(gipa(instance, "xrResumeSimultaneousHandsAndControllersTrackingMETA", (PFN_xrVoidFunction*)&resume), XR_SUCCESS);
    ASSERT_EQ(gipa(instance, "xrPauseSimultaneousHandsAndControllersTrackingMETA", (PFN_xrVoidFunction*)&pause), XR_SUCCESS);

    const XrSimultaneousHandsAndControllersTrackingResumeInfoMETA resume_info = { .type = XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_RESUME_INFO_META };
    const XrSimultaneousHandsAndControllersTrackingPauseInfoMETA  pause_info  = { .type = XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_PAUSE_INFO_META };
    EXPECT_EQ(resume(XR_NULL_HANDLE, &resume_info), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(resume(session, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(resume(session, (const XrSimultaneousHandsAndControllersTrackingResumeInfoMETA*)&pause_info), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(resume(session, &resume_info), XR_SUCCESS);
    EXPECT_EQ(pause(session, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(pause(session, &pause_info), XR_SUCCESS);
}

TEST_F(OxrTest, simultaneous_hands_and_controllers_need_hand_tracking)
{
    const char* exts[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME };
    EXPECT_EQ(create_instance(exts, 2), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
}

//
// XR_META_hand_tracking_microgestures, XR_META_hand_tracking_wide_motion_mode (preview) and _mode2
//

TEST_F(OxrTest, microgestures_need_hand_interaction)
{
    const char* exts[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME };
    EXPECT_EQ(create_instance(exts, 3), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
    const char* wide[] = { XR_MND_HEADLESS_EXTENSION_NAME, "XR_META_hand_tracking_wide_motion_mode" };
    EXPECT_EQ(create_instance(wide, 2), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
}

namespace
{
struct OxrHandHintsTest : OxrHandTest
{
    OxrHandHintsTest ()
    {
        more_extensions[more_extensions_count++] = XR_META_HAND_TRACKING_FREQUENCY_HINT_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_META_HAND_TRACKING_UNEXTRAPOLATED_POSES_EXTENSION_NAME;
    }
};
} // namespace

TEST_F(OxrHandTest, hand_hints_need_their_extensions)
{
    PFN_xrVoidFunction function = nullptr;
    EXPECT_EQ(gipa(instance, "xrSetHandTrackingFrequencyHintMETA", &function), XR_ERROR_FUNCTION_UNSUPPORTED);

    // Without the extension the request is ignored and the capture time left alone
    begin_session();
    sim_set_hand_source(oxr_get_sim(), SIM_Device::RightHand, SIM_HandSource::Hand);
    frame();
    const XrHandTrackerEXT                             hand_tracker = tracker(XR_HAND_RIGHT_EXT);
    const XrHandTrackingUnextrapolatedPosesRequestMETA request      = { .type = XR_TYPE_HAND_TRACKING_UNEXTRAPOLATED_POSES_REQUEST_META };
    XrHandTrackingUnextrapolatedPosesMETA              poses        = { .type = XR_TYPE_HAND_TRACKING_UNEXTRAPOLATED_POSES_META, .captureTime = 7 };
    XrHandJointLocationEXT                             joints[XR_HAND_JOINT_COUNT_EXT] = {};
    XrBool32                                           active = XR_FALSE;
    ASSERT_EQ(locate(hand_tracker, joints, active, &poses, &request), XR_SUCCESS);
    EXPECT_TRUE(active);
    EXPECT_EQ(poses.captureTime, 7);
}

TEST_F(OxrTest, hand_hints_need_hand_tracking)
{
    const char* frequency[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_META_HAND_TRACKING_FREQUENCY_HINT_EXTENSION_NAME };
    EXPECT_EQ(create_instance(frequency, 2), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
    const char* unextrapolated[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_META_HAND_TRACKING_UNEXTRAPOLATED_POSES_EXTENSION_NAME };
    EXPECT_EQ(create_instance(unextrapolated, 2), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
}

TEST_F(OxrHandHintsTest, frequency_hints_are_validated)
{
    PFN_xrSetHandTrackingFrequencyHintMETA set_hint = nullptr;
    ASSERT_EQ(gipa(instance, "xrSetHandTrackingFrequencyHintMETA", (PFN_xrVoidFunction*)&set_hint), XR_SUCCESS);
    EXPECT_EQ(set_hint(session, XR_HAND_TRACKING_FREQUENCY_HINT_HIGH_META), XR_SUCCESS);
    EXPECT_EQ(set_hint(session, XR_HAND_TRACKING_FREQUENCY_HINT_DEFAULT_META), XR_SUCCESS);
    EXPECT_EQ(set_hint(session, (XrHandTrackingFrequencyHintMETA)0), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(set_hint(session, (XrHandTrackingFrequencyHintMETA)3), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(set_hint(XR_NULL_HANDLE, XR_HAND_TRACKING_FREQUENCY_HINT_HIGH_META), XR_ERROR_HANDLE_INVALID);
}

// The simulated hand's latest state, never extrapolated to the given time, captured at the
// display time the simulation computed it for
TEST_F(OxrHandHintsTest, unextrapolated_poses_ignore_the_time)
{
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    Pose moved = sim_get_device_pose(sim, SIM_Device::RightHand);
    frame();
    moved.position.x += 0.01f;
    sim_set_device_pose(sim, SIM_Device::RightHand, moved);
    frame();

    const XrHandTrackerEXT                             hand_tracker = tracker(XR_HAND_RIGHT_EXT);
    const XrHandTrackingUnextrapolatedPosesRequestMETA request      = { .type = XR_TYPE_HAND_TRACKING_UNEXTRAPOLATED_POSES_REQUEST_META };
    XrHandTrackingUnextrapolatedPosesMETA              poses        = { .type = XR_TYPE_HAND_TRACKING_UNEXTRAPOLATED_POSES_META, .captureTime = 7 };
    XrHandJointLocationEXT                             joints[XR_HAND_JOINT_COUNT_EXT] = {};
    XrBool32                                           active = XR_FALSE;
    const i64                                          later  = sim_predicted_display_time(sim) + 50000000;
    XrHandJointLocationsEXT                            locations = { .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT, .next = &poses, .jointCount = XR_HAND_JOINT_COUNT_EXT, .jointLocations = joints };
    XrHandJointsLocateInfoEXT                          info      = { .type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, .baseSpace = stage, .time = later };

    // Without the request the joints move on with the hand's velocity, and the capture time stays
    ASSERT_EQ(locate_joints(hand_tracker, &info, &locations), XR_SUCCESS);
    SIM_HandJoints latest = {};
    sim_hand_joints(sim, SIM_Device::RightHand, sim_get_device_pose(sim, SIM_Device::RightHand), SIM_HandMotion::Unobstructed, latest);
    EXPECT_GT(joints[XR_HAND_JOINT_WRIST_EXT].pose.position.x, latest.poses[XR_HAND_JOINT_WRIST_EXT].position.x + 0.001f);
    EXPECT_EQ(poses.captureTime, 7);

    info.next = &request;
    ASSERT_EQ(locate_joints(hand_tracker, &info, &locations), XR_SUCCESS);
    ASSERT_TRUE(locations.isActive);
    for (u32 j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++)
    {
        EXPECT_TRUE(zm::near(vec(joints[j].pose.position), latest.poses[j].position, 1e-5f)) << j;
    }
    EXPECT_EQ(poses.captureTime, sim_predicted_display_time(sim));

    // Spec: zero without valid poses
    sim_set_device_active(sim, SIM_Device::RightHand, false);
    frame();
    ASSERT_EQ(locate(hand_tracker, joints, active, &poses, &request), XR_SUCCESS);
    EXPECT_FALSE(active);
    EXPECT_EQ(poses.captureTime, 0);
}

namespace
{

struct OxrMicrogestureTest : OxrHandTest
{
    OxrMicrogestureTest ()
    {
        more_extensions[more_extensions_count++] = XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME;
    }
};

// Not in the OpenXR headers: Meta's preview header meta_hand_tracking_wide_motion_mode.h
struct WideMotionModeInfo
{
    XrStructureType type;
    const void*     next;
    u32             requested_mode; // 1: high fidelity body tracking
};
const XrStructureType WIDE_MOTION_MODE_INFO = (XrStructureType)1000539000;

struct OxrWideMotionTest : OxrActionTest
{
    PFN_xrCreateHandTrackerEXT create_tracker = nullptr;
    PFN_xrLocateHandJointsEXT  locate_joints  = nullptr;
    XrSpace                    stage          = XR_NULL_HANDLE;

    OxrWideMotionTest ()
    {
        more_extensions[more_extensions_count++] = XR_EXT_HAND_TRACKING_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = "XR_META_hand_tracking_wide_motion_mode";
    }

    void SetUp () override
    {
        OxrActionTest::SetUp();
        ASSERT_EQ(gipa(instance, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction*)&create_tracker), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrLocateHandJointsEXT", (PFN_xrVoidFunction*)&locate_joints), XR_SUCCESS);
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    }

    XrHandJointLocationsEXT locate (XrHandTrackerEXT tracker, XrHandJointLocationEXT (&joints)[XR_HAND_JOINT_COUNT_EXT])
    {
        const XrHandJointsLocateInfoEXT info      = { .type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, .baseSpace = stage, .time = sim_predicted_display_time(oxr_get_sim()) };
        XrHandJointLocationsEXT         locations = { .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT, .jointCount = XR_HAND_JOINT_COUNT_EXT, .jointLocations = joints };
        EXPECT_EQ(locate_joints(tracker, &info, &locations), XR_SUCCESS);
        return locations;
    }
};
} // namespace

TEST_F(OxrMicrogestureTest, swipes_and_taps_pulse_on_the_hand_interaction_profile)
{
    const XrAction swipe = make_action("swipe", XR_ACTION_TYPE_BOOLEAN_INPUT);
    const XrAction tap   = make_action("tap", XR_ACTION_TYPE_BOOLEAN_INPUT);
    const XrActionSuggestedBinding bindings[] = {
        { swipe, path("/user/hand/right/input/swipe_left_meta/click") },
        { tap,   path("/user/hand/right/input/tap_thumb_meta/click") },
    };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/ext/hand_interaction_ext"), .countSuggestedBindings = ARRAY_SIZE(bindings), .suggestedBindings = bindings };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
    attach();
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(bool_state(swipe, hands[1]).isActive);
    EXPECT_FALSE(bool_state(swipe, hands[1]).currentState);

    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::SwipeLeft, { .value = { 1.0f, 0.0f } });
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(bool_state(swipe, hands[1]).currentState);
    EXPECT_FALSE(bool_state(tap, hands[1]).currentState);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_FALSE(bool_state(swipe, hands[1]).currentState) << "a microgesture lasts one frame";
}

TEST_F(OxrHandTest, microgesture_paths_need_their_extension)
{
    const XrActionSuggestedBinding swipe = { click, path("/user/hand/right/input/swipe_left_meta/click") };
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/ext/hand_interaction_ext"), .countSuggestedBindings = 1, .suggestedBindings = &swipe };
    EXPECT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &suggested), XR_ERROR_PATH_UNSUPPORTED);
}

// A tracked hand outside the hand tracking cone is lost, unless the tracker asked for wide motion
// mode, which keeps estimating it: valid but not tracked
TEST_F(OxrWideMotionTest, wide_motion_keeps_hands_outside_the_cameras)
{
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);

    XrHandTrackerEXT                 plain = XR_NULL_HANDLE;
    XrHandTrackerEXT                 wide  = XR_NULL_HANDLE;
    WideMotionModeInfo               mode  = { .type = WIDE_MOTION_MODE_INFO, .requested_mode = 1 };
    XrHandTrackerCreateInfoEXT       info  = { .type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, .hand = XR_HAND_RIGHT_EXT, .handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT };
    ASSERT_EQ(create_tracker(session, &info, &plain), XR_SUCCESS);
    info.next           = &mode;
    mode.requested_mode = 7;
    EXPECT_EQ(create_tracker(session, &info, &wide), XR_ERROR_VALIDATION_FAILURE);
    mode.requested_mode = 1;
    ASSERT_EQ(create_tracker(session, &info, &wide), XR_SUCCESS);

    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT] = {};
    frame();
    EXPECT_TRUE(locate(plain, joints).isActive);
    EXPECT_TRUE(joints[0].locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
    EXPECT_TRUE(locate(wide, joints).isActive);

    // Behind the head
    Pose behind     = sim_get_device_pose(sim, SIM_Device::RightHand);
    behind.position = sim_get_device_pose(sim, SIM_Device::Head).position + Vec3{ 0.2f, -0.2f, 0.3f };
    sim_set_device_pose(sim, SIM_Device::RightHand, behind);
    frame();
    frame();
    EXPECT_FALSE(locate(plain, joints).isActive);
    EXPECT_TRUE(locate(wide, joints).isActive);
    EXPECT_TRUE(joints[0].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    EXPECT_FALSE(joints[0].locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
}

namespace
{
struct OxrWideMotion2Test : OxrWideMotionTest
{
    OxrWideMotion2Test ()
    {
        more_extensions[more_extensions_count - 1] = XR_META_HAND_TRACKING_WIDE_MOTION_MODE2_EXTENSION_NAME;
    }
};
} // namespace

// Mode 2 has no structs: enabled, every tracker is in wide motion mode
TEST_F(OxrWideMotion2Test, every_tracker_is_wide)
{
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::LeftHand, SIM_HandSource::Hand);
    XrHandTrackerEXT                 tracker = XR_NULL_HANDLE;
    const XrHandTrackerCreateInfoEXT info    = { .type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, .hand = XR_HAND_LEFT_EXT, .handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT };
    ASSERT_EQ(create_tracker(session, &info, &tracker), XR_SUCCESS);
    Pose behind     = sim_get_device_pose(sim, SIM_Device::LeftHand);
    behind.position = sim_get_device_pose(sim, SIM_Device::Head).position + Vec3{ -0.2f, -0.2f, 0.3f };
    sim_set_device_pose(sim, SIM_Device::LeftHand, behind);
    frame();
    frame();
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT] = {};
    EXPECT_TRUE(locate(tracker, joints).isActive);
    EXPECT_FALSE(joints[0].locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
}

// Action poses of a tracked hand are lost with the hand; a controller is tracked wherever it is
TEST_F(OxrHandTest, hand_poses_are_lost_outside_the_cameras)
{
    const XrActionSuggestedBinding hand_grip = { grip, path("/user/hand/right/input/grip/pose") };
    const XrInteractionProfileSuggestedBinding hand_suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/ext/hand_interaction_ext"), .countSuggestedBindings = 1, .suggestedBindings = &hand_grip };
    ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &hand_suggested), XR_SUCCESS);
    suggest_touch_bindings();
    attach();
    begin_session();
    SIM_Sim* sim = oxr_get_sim();
    XrSpace                       grip_space = XR_NULL_HANDLE;
    const XrActionSpaceCreateInfo space_info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = grip, .subactionPath = hands[1], .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    ASSERT_EQ(xr.xrCreateActionSpace(session, &space_info, &grip_space), XR_SUCCESS);

    Pose behind     = sim_get_device_pose(sim, SIM_Device::RightHand);
    behind.position = sim_get_device_pose(sim, SIM_Device::Head).position + Vec3{ 0.2f, -0.2f, 0.3f };
    sim_set_device_pose(sim, SIM_Device::RightHand, behind);
    frame();
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(grip_space, stage, sim_predicted_display_time(sim), &location), XR_SUCCESS);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) << "the controller behind the head is still tracked";

    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    ASSERT_EQ(xr.xrLocateSpace(grip_space, stage, sim_predicted_display_time(sim), &location), XR_SUCCESS);
    EXPECT_FALSE(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
}

//
// XR_META_detached_controllers
//

TEST_F(OxrActionTest, detached_controller_paths_need_their_extension)
{
    XrAction                 action  = XR_NULL_HANDLE;
    const XrPath             detached = path("/user/detached_controller_meta/left");
    XrActionCreateInfo       info    = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionType = XR_ACTION_TYPE_POSE_INPUT, .countSubactionPaths = 1, .subactionPaths = &detached };
    cstr_copy(info.actionName, sizeof(info.actionName), "detached");
    cstr_copy(info.localizedActionName, sizeof(info.localizedActionName), "Detached");
    EXPECT_EQ(xr.xrCreateAction(set, &info, &action), XR_ERROR_PATH_UNSUPPORTED);
}

namespace
{
struct OxrDetachedTest : OxrHandTest
{
    XrPath   detached[2] = {};
    XrAction aim         = XR_NULL_HANDLE;
    XrAction press       = XR_NULL_HANDLE;
    XrAction buzz        = XR_NULL_HANDLE;

    OxrDetachedTest ()
    {
        more_extensions[more_extensions_count++] = XR_META_DETACHED_CONTROLLERS_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrHandTest::SetUp();
        detached[0] = path("/user/detached_controller_meta/left");
        detached[1] = path("/user/detached_controller_meta/right");
        const XrPath sides[4] = { hands[0], hands[1], detached[0], detached[1] };
        aim   = make_action4("aim", XR_ACTION_TYPE_POSE_INPUT, sides);
        press = make_action4("press", XR_ACTION_TYPE_FLOAT_INPUT, sides);
        buzz  = make_action4("buzz", XR_ACTION_TYPE_VIBRATION_OUTPUT, sides);

        const XrActionSuggestedBinding touch[] = {
            { aim,   path("/user/hand/left/input/aim/pose") },
            { aim,   path("/user/hand/right/input/aim/pose") },
            { aim,   path("/user/detached_controller_meta/left/input/aim/pose") },
            { aim,   path("/user/detached_controller_meta/right/input/aim/pose") },
            { press, path("/user/hand/right/input/trigger/value") },
            { press, path("/user/detached_controller_meta/right/input/trigger/value") },
            { buzz,  path("/user/detached_controller_meta/right/output/haptic") },
        };
        const XrInteractionProfileSuggestedBinding touch_suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/oculus/touch_controller"), .countSuggestedBindings = ARRAY_SIZE(touch), .suggestedBindings = touch };
        ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &touch_suggested), XR_SUCCESS);
        const XrActionSuggestedBinding hand[] = {
            { aim,   path("/user/hand/right/input/aim/pose") },
            { press, path("/user/hand/right/input/pinch_ext/value") },
        };
        const XrInteractionProfileSuggestedBinding hand_suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = path("/interaction_profiles/ext/hand_interaction_ext"), .countSuggestedBindings = ARRAY_SIZE(hand), .suggestedBindings = hand };
        ASSERT_EQ(xr.xrSuggestInteractionProfileBindings(instance, &hand_suggested), XR_SUCCESS);
        attach();
        begin_session();
    }

    XrAction make_action4 (const char* name, XrActionType type, const XrPath (&sides)[4])
    {
        XrAction           action = XR_NULL_HANDLE;
        XrActionCreateInfo info   = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionType = type, .countSubactionPaths = 4, .subactionPaths = sides };
        cstr_copy(info.actionName, sizeof(info.actionName), name);
        cstr_copy(info.localizedActionName, sizeof(info.localizedActionName), name);
        EXPECT_EQ(xr.xrCreateAction(set, &info, &action), XR_SUCCESS);
        return action;
    }

    XrPath current (XrPath top)
    {
        XrInteractionProfileState state = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
        EXPECT_EQ(xr.xrGetCurrentInteractionProfile(session, top, &state), XR_SUCCESS);
        return state.interactionProfile;
    }
};
} // namespace

// Spec XR_META_detached_controllers: a profile is current on at most one of a hand path and its
// detached path; a controller lying down reports under the detached path
TEST_F(OxrDetachedTest, a_put_down_controller_reports_under_its_detached_path)
{
    SIM_Sim* sim = oxr_get_sim();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(current(hands[1]), path("/interaction_profiles/oculus/touch_controller"));
    EXPECT_EQ(current(detached[1]), (XrPath)XR_NULL_PATH);
    EXPECT_FALSE(float_state(press, detached[1]).isActive);

    const Pose put_down = sim_get_device_pose(sim, SIM_Device::RightHand);
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 0.8f, 0.0f } });
    Pose hand_pose = put_down;
    hand_pose.position.y += 0.2f;
    sim_set_device_pose(sim, SIM_Device::RightHand, hand_pose);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(current(hands[1]), path("/interaction_profiles/ext/hand_interaction_ext"));
    EXPECT_EQ(current(detached[1]), path("/interaction_profiles/oculus/touch_controller"));
    EXPECT_EQ(current(detached[0]), (XrPath)XR_NULL_PATH) << "the left controller is still held";

    EXPECT_FLOAT_EQ(float_state(press, hands[1]).currentState, 0.8f) << "the hand pinches";
    EXPECT_TRUE(float_state(press, detached[1]).isActive);
    EXPECT_FLOAT_EQ(float_state(press, detached[1]).currentState, 0.0f) << "nobody pulls the trigger of a controller on the table";

    XrSpace                       space = XR_NULL_HANDLE;
    const XrActionSpaceCreateInfo space_info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = aim, .subactionPath = detached[1], .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    ASSERT_EQ(xr.xrCreateActionSpace(session, &space_info, &space), XR_SUCCESS);
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(space, stage, sim_predicted_display_time(sim), &location), XR_SUCCESS);
    ASSERT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    const Pose expected = zm::pose_mul(put_down, sim_get_config(sim).aim_offset);
    EXPECT_TRUE(zm::near(vec(location.pose.position), expected.position, 1e-4f));

    const XrHapticVibration  vibration = { .type = XR_TYPE_HAPTIC_VIBRATION, .duration = 100000000, .frequency = XR_FREQUENCY_UNSPECIFIED, .amplitude = 0.5f };
    const XrHapticActionInfo info      = { .type = XR_TYPE_HAPTIC_ACTION_INFO, .action = buzz, .subactionPath = detached[1] };
    EXPECT_EQ(xr.xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader*)&vibration), XR_SUCCESS);
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active) << "the controller on the table vibrates";

    // Picked up again, the controller is back under the hand path
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Controller);
    frame();
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_EQ(current(hands[1]), path("/interaction_profiles/oculus/touch_controller"));
    EXPECT_EQ(current(detached[1]), (XrPath)XR_NULL_PATH);
    EXPECT_FALSE(float_state(press, detached[1]).isActive);
}

//
// XR_FB_render_model
//

TEST_F(OxrActionTest, render_models_need_their_extension)
{
    PFN_xrVoidFunction function = nullptr;
    EXPECT_EQ(gipa(instance, "xrLoadRenderModelFB", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
}

namespace
{
struct OxrRenderModelTest : OxrActionTest
{
    PFN_xrEnumerateRenderModelPathsFB enumerate  = nullptr;
    PFN_xrGetRenderModelPropertiesFB  properties = nullptr;
    PFN_xrLoadRenderModelFB           load       = nullptr;

    OxrRenderModelTest ()
    {
        more_extensions[more_extensions_count++] = XR_FB_RENDER_MODEL_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrActionTest::SetUp();
        ASSERT_EQ(gipa(instance, "xrEnumerateRenderModelPathsFB", (PFN_xrVoidFunction*)&enumerate), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetRenderModelPropertiesFB", (PFN_xrVoidFunction*)&properties), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrLoadRenderModelFB", (PFN_xrVoidFunction*)&load), XR_SUCCESS);
    }

    void enumerate_paths ()
    {
        u32 count = 0;
        ASSERT_EQ(enumerate(session, 0, &count, nullptr), XR_SUCCESS);
        ASSERT_EQ(count, 4u);
    }

    XrRenderModelPropertiesFB model_of (const char* model_path, XrResult expected = XR_SUCCESS)
    {
        XrRenderModelPropertiesFB props = { .type = XR_TYPE_RENDER_MODEL_PROPERTIES_FB };
        EXPECT_EQ(properties(session, path(model_path), &props), expected);
        return props;
    }

    static u32 read_u32 (const u8* bytes)
    {
        return (u32)bytes[0] | ((u32)bytes[1] << 8) | ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);
    }
};
} // namespace

TEST_F(OxrRenderModelTest, system_supports_render_models)
{
    XrSystemRenderModelPropertiesFB models = { .type = XR_TYPE_SYSTEM_RENDER_MODEL_PROPERTIES_FB };
    XrSystemProperties              props  = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &models };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(models.supportsRenderModelLoading, XR_TRUE);
}

TEST_F(OxrRenderModelTest, paths_enumerate_with_two_calls)
{
    u32 count = 0;
    EXPECT_EQ(enumerate(XR_NULL_HANDLE, 0, &count, nullptr), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(enumerate(session, 0, nullptr, nullptr), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(enumerate(session, 0, &count, nullptr), XR_SUCCESS);
    ASSERT_EQ(count, 4u);
    XrRenderModelPathInfoFB paths[4] = {};
    for (XrRenderModelPathInfoFB& info : paths)
    {
        info.type = XR_TYPE_RENDER_MODEL_PATH_INFO_FB;
    }
    EXPECT_EQ(enumerate(session, 3, &count, paths), XR_ERROR_SIZE_INSUFFICIENT);
    ASSERT_EQ(enumerate(session, 4, &count, paths), XR_SUCCESS);
    EXPECT_EQ(paths[0].path, path("/model_fb/controller/left"));
    EXPECT_EQ(paths[1].path, path("/model_fb/controller/right"));
    EXPECT_EQ(paths[2].path, path("/model_fb/keyboard/local"));
    EXPECT_EQ(paths[3].path, path("/model_fb/keyboard/remote"));
}

TEST_F(OxrRenderModelTest, properties_follow_the_simulated_controller)
{
    model_of("/model_fb/controller/left", XR_ERROR_CALL_ORDER_INVALID);
    enumerate_paths();
    model_of("/model_meta/keyboard/virtual", XR_ERROR_PATH_INVALID);
    EXPECT_EQ(properties(session, path("/model_fb/controller/left"), nullptr), XR_ERROR_VALIDATION_FAILURE);

    const XrRenderModelPropertiesFB left  = model_of("/model_fb/controller/left");
    const XrRenderModelPropertiesFB right = model_of("/model_fb/controller/right");
    EXPECT_NE(left.modelKey, XR_NULL_RENDER_MODEL_KEY_FB);
    EXPECT_NE(left.modelKey, right.modelKey);
    EXPECT_EQ(left.flags, XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB);
    EXPECT_GT(cstr_length(left.modelName), 0u);

    // Controllers are offered at both levels (Godot's vendors plugin asks for subset 2 only); the
    // reported level is the lowest one requested
    XrRenderModelCapabilitiesRequestFB subset2 = { .type = XR_TYPE_RENDER_MODEL_CAPABILITIES_REQUEST_FB, .flags = XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_2_BIT_FB };
    XrRenderModelPropertiesFB          props   = { .type = XR_TYPE_RENDER_MODEL_PROPERTIES_FB, .next = &subset2 };
    EXPECT_EQ(properties(session, path("/model_fb/controller/left"), &props), XR_SUCCESS);
    EXPECT_EQ(props.modelKey, left.modelKey);
    EXPECT_EQ(props.flags, XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_2_BIT_FB);
    subset2.flags |= XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB;
    EXPECT_EQ(properties(session, path("/model_fb/controller/left"), &props), XR_SUCCESS);
    EXPECT_EQ(props.modelKey, left.modelKey);
    EXPECT_EQ(props.flags, XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB);

    sim_set_controller(oxr_get_sim(), SIM_Controller::OculusTouch);
    EXPECT_NE(model_of("/model_fb/controller/left").modelKey, left.modelKey) << "another controller is another model";
}

TEST_F(OxrRenderModelTest, models_load_as_binary_gltf)
{
    enumerate_paths();
    const XrRenderModelPropertiesFB right = model_of("/model_fb/controller/right");
    XrRenderModelLoadInfoFB         info  = { .type = XR_TYPE_RENDER_MODEL_LOAD_INFO_FB, .modelKey = right.modelKey };
    XrRenderModelBufferFB           buffer = { .type = XR_TYPE_RENDER_MODEL_BUFFER_FB };
    EXPECT_EQ(load(session, nullptr, &buffer), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(load(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(load(session, &info, &buffer), XR_SUCCESS);
    ASSERT_GT(buffer.bufferCountOutput, 1000u);

    Array<u8> glb = {};
    defer { glb.release(); };
    glb.resize(buffer.bufferCountOutput);
    buffer.buffer              = glb.data;
    buffer.bufferCapacityInput = buffer.bufferCountOutput - 1;
    EXPECT_EQ(load(session, &info, &buffer), XR_ERROR_SIZE_INSUFFICIENT);
    buffer.bufferCapacityInput = (u32)glb.length;
    ASSERT_EQ(load(session, &info, &buffer), XR_SUCCESS);

    // glTF 2.0, binary: header, a JSON chunk and a BIN chunk, every chunk 4-byte aligned
    ASSERT_TRUE(memory_equal(glb.data, "glTF", 4));
    EXPECT_EQ(read_u32(glb.data + 4), 2u);
    EXPECT_EQ(read_u32(glb.data + 8), (u32)glb.length);
    const u32 chunk_length = read_u32(glb.data + 12);
    EXPECT_EQ(chunk_length % 4, 0u);
    EXPECT_TRUE(memory_equal(glb.data + 16, "JSON", 4));
    const u8* bin = glb.data + 20 + chunk_length;
    EXPECT_TRUE(memory_equal(bin + 4, "BIN\0", 4));
    EXPECT_EQ(20u + chunk_length + 8u + read_u32(bin), (u32)glb.length);

    Array<char> text = {};
    defer { text.release(); };
    text.resize(chunk_length + 1);
    memory_copy(text.data, glb.data + 20, chunk_length);
    text.data[chunk_length] = 0;
    JsonParser* json = json_parse_from_string(text.data);
    ASSERT_NE(json, nullptr);
    defer { json_release(json); };
    const JsonValue root = json_get_root(json);
    char version[8] = {};
    EXPECT_TRUE(json_find_string(json_find(root, "asset"), "version", version, nullptr, sizeof(version)));
    EXPECT_STREQ(version, "2.0");
    // A node, mesh and primitive per part
    const JsonValue meshes = json_find(root, "meshes");
    EXPECT_GE(json_length(meshes), 3u);
    EXPECT_EQ(json_length(json_find(root, "nodes")), json_length(meshes));
    EXPECT_EQ(json_length(json_find(json_at(meshes, 0), "primitives")), 1u);
    EXPECT_FALSE(json_is_valid(json_find(root, "images"))) << "controllers are untextured";

    // A key that names no model the runtime handed out, and one of a controller no longer simulated
    info.modelKey = 0xDEADull;
    EXPECT_EQ(load(session, &info, &buffer), XR_ERROR_RENDER_MODEL_KEY_INVALID_FB);
    sim_set_controller(oxr_get_sim(), SIM_Controller::OculusTouch);
    info.modelKey = right.modelKey;
    EXPECT_EQ(load(session, &info, &buffer), XR_RENDER_MODEL_UNAVAILABLE_FB);
    EXPECT_EQ(buffer.bufferCountOutput, 0u);
}

TEST_F(OxrRenderModelTest, keyboards_do_not_depend_on_the_controllers)
{
    enumerate_paths();
    const XrRenderModelPropertiesFB local  = model_of("/model_fb/keyboard/local");
    const XrRenderModelPropertiesFB remote = model_of("/model_fb/keyboard/remote");
    EXPECT_NE(local.modelKey, XR_NULL_RENDER_MODEL_KEY_FB);
    EXPECT_NE(local.modelKey, remote.modelKey);
    EXPECT_NE(local.modelKey, model_of("/model_fb/controller/left").modelKey);

    sim_set_controller(oxr_get_sim(), SIM_Controller::OculusTouch);
    EXPECT_EQ(model_of("/model_fb/keyboard/local").modelKey, local.modelKey);
    const XrRenderModelLoadInfoFB info   = { .type = XR_TYPE_RENDER_MODEL_LOAD_INFO_FB, .modelKey = local.modelKey };
    XrRenderModelBufferFB         buffer = { .type = XR_TYPE_RENDER_MODEL_BUFFER_FB };
    ASSERT_EQ(load(session, &info, &buffer), XR_SUCCESS);
    EXPECT_GT(buffer.bufferCountOutput, 10000u);
}
//
// XR_META_virtual_keyboard
//

TEST_F(OxrActionTest, virtual_keyboard_needs_its_extension)
{
    PFN_xrVoidFunction function = nullptr;
    EXPECT_EQ(gipa(instance, "xrCreateVirtualKeyboardMETA", &function), XR_ERROR_FUNCTION_UNSUPPORTED);
}

namespace
{
struct OxrVirtualKeyboardTest : OxrActionTest
{
    PFN_xrCreateVirtualKeyboardMETA                  create_keyboard   = nullptr;
    PFN_xrDestroyVirtualKeyboardMETA                 destroy_keyboard  = nullptr;
    PFN_xrCreateVirtualKeyboardSpaceMETA             create_space      = nullptr;
    PFN_xrSuggestVirtualKeyboardLocationMETA         suggest_location  = nullptr;
    PFN_xrGetVirtualKeyboardScaleMETA                get_scale         = nullptr;
    PFN_xrSetVirtualKeyboardModelVisibilityMETA      set_visibility    = nullptr;
    PFN_xrGetVirtualKeyboardModelAnimationStatesMETA animation_states  = nullptr;
    PFN_xrGetVirtualKeyboardDirtyTexturesMETA        dirty_textures    = nullptr;
    PFN_xrGetVirtualKeyboardTextureDataMETA          texture_data      = nullptr;
    PFN_xrSendVirtualKeyboardInputMETA               send_input        = nullptr;
    PFN_xrChangeVirtualKeyboardTextContextMETA       change_context    = nullptr;
    XrSpace                                          stage             = XR_NULL_HANDLE;
    XrVirtualKeyboardMETA                            keyboard          = XR_NULL_HANDLE;
    XrSpace                                          keyboard_space    = XR_NULL_HANDLE;
    SIM_VkbLayout                                    layout            = {};

    OxrVirtualKeyboardTest ()
    {
        more_extensions[more_extensions_count++] = XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_FB_RENDER_MODEL_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrActionTest::SetUp();
        ASSERT_EQ(gipa(instance, "xrCreateVirtualKeyboardMETA", (PFN_xrVoidFunction*)&create_keyboard), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrDestroyVirtualKeyboardMETA", (PFN_xrVoidFunction*)&destroy_keyboard), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrCreateVirtualKeyboardSpaceMETA", (PFN_xrVoidFunction*)&create_space), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrSuggestVirtualKeyboardLocationMETA", (PFN_xrVoidFunction*)&suggest_location), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetVirtualKeyboardScaleMETA", (PFN_xrVoidFunction*)&get_scale), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrSetVirtualKeyboardModelVisibilityMETA", (PFN_xrVoidFunction*)&set_visibility), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetVirtualKeyboardModelAnimationStatesMETA", (PFN_xrVoidFunction*)&animation_states), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetVirtualKeyboardDirtyTexturesMETA", (PFN_xrVoidFunction*)&dirty_textures), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetVirtualKeyboardTextureDataMETA", (PFN_xrVoidFunction*)&texture_data), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrSendVirtualKeyboardInputMETA", (PFN_xrVoidFunction*)&send_input), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrChangeVirtualKeyboardTextContextMETA", (PFN_xrVoidFunction*)&change_context), XR_SUCCESS);
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
        sim_vkb_layout(layout);
    }

    // A keyboard at 1 m height, 1 m in front of the stage origin, facing the origin
    void create_shown_keyboard (f32 scale = 1.0f)
    {
        const XrVirtualKeyboardCreateInfoMETA info = { .type = XR_TYPE_VIRTUAL_KEYBOARD_CREATE_INFO_META };
        ASSERT_EQ(create_keyboard(session, &info, &keyboard), XR_SUCCESS);
        const XrVirtualKeyboardSpaceCreateInfoMETA space_info = { .type = XR_TYPE_VIRTUAL_KEYBOARD_SPACE_CREATE_INFO_META, .locationType = XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_CUSTOM_META, .space = stage, .poseInSpace = { { 0, 0, 0, 1 }, { 0, 1, -1 } } };
        ASSERT_EQ(create_space(session, keyboard, &space_info, &keyboard_space), XR_SUCCESS);
        if (scale != 1.0f)
        {
            const XrVirtualKeyboardLocationInfoMETA location = { .type = XR_TYPE_VIRTUAL_KEYBOARD_LOCATION_INFO_META, .locationType = XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_CUSTOM_META, .space = stage, .poseInSpace = { { 0, 0, 0, 1 }, { 0, 1, -1 } }, .scale = scale };
            ASSERT_EQ(suggest_location(keyboard, &location), XR_SUCCESS);
        }
        const XrVirtualKeyboardModelVisibilitySetInfoMETA visible = { .type = XR_TYPE_VIRTUAL_KEYBOARD_MODEL_VISIBILITY_SET_INFO_META, .visible = XR_TRUE };
        ASSERT_EQ(set_visibility(keyboard, &visible), XR_SUCCESS);
        drain_events();
    }

    // Stage pose of a key's centre on its top
    XrVector3f key_point (const char* name, f32 scale = 1.0f) const
    {
        const i32 k = sim_vkb_find_key(layout, name);
        EXPECT_GE(k, 0) << name;
        const SIM_VkbKey& key = layout.keys[k < 0 ? 0 : k];
        return { key.center.x * scale, 1.0f + key.center.y * scale, -1.0f + SIM_VKB_KEY_HEIGHT * scale };
    }

    XrResult ray (XrVirtualKeyboardInputSourceMETA source, XrVector3f target, b8 pressed)
    {
        const XrPosef                        aim  = { { 0, 0, 0, 1 }, { target.x, target.y, target.z + 0.5f } };
        const XrVirtualKeyboardInputInfoMETA info = { .type = XR_TYPE_VIRTUAL_KEYBOARD_INPUT_INFO_META, .inputSource = source, .inputSpace = stage, .inputPoseInSpace = aim, .inputState = pressed ? XR_VIRTUAL_KEYBOARD_INPUT_STATE_PRESSED_BIT_META : 0 };
        XrPosef                              root = aim;
        return send_input(keyboard, &info, &root);
    }

    XrResult poke (XrVirtualKeyboardInputSourceMETA source, XrVector3f tip, XrPosef& inout_root)
    {
        const XrVirtualKeyboardInputInfoMETA info = { .type = XR_TYPE_VIRTUAL_KEYBOARD_INPUT_INFO_META, .inputSource = source, .inputSpace = stage, .inputPoseInSpace = { { 0, 0, 0, 1 }, tip } };
        return send_input(keyboard, &info, &inout_root);
    }

    static void append (char* out, usize size, const char* text)
    {
        const usize length = cstr_length(out);
        cstr_copy(out + length, size - length, text);
    }

    // The events of the keyboard as text: commits as they are, backspace as '<', enter as '>'
    void typed (char* out, usize size) const
    {
        out[0] = 0;
        while (true)
        {
            XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            if (xr.xrPollEvent(instance, &event) != XR_SUCCESS)
            {
                return;
            }
            switch ((u32)event.type)
            {
            case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_COMMIT_TEXT_META:
                EXPECT_EQ(((XrEventDataVirtualKeyboardCommitTextMETA*)&event)->keyboard, keyboard);
                append(out, size, ((XrEventDataVirtualKeyboardCommitTextMETA*)&event)->text);
                break;
            case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_BACKSPACE_META:
                append(out, size, "<");
                break;
            case XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_ENTER_META:
                append(out, size, ">");
                break;
            default:
                break;
            }
        }
    }

    u32 read_states (XrVirtualKeyboardAnimationStateMETA* out, u32 capacity)
    {
        XrVirtualKeyboardModelAnimationStatesMETA states = { .type = XR_TYPE_VIRTUAL_KEYBOARD_MODEL_ANIMATION_STATES_META };
        EXPECT_EQ(animation_states(keyboard, &states), XR_SUCCESS);
        if (states.stateCountOutput == 0)
        {
            return 0;
        }
        EXPECT_LE(states.stateCountOutput, capacity);
        for (u32 i = 0; i < capacity; i++)
        {
            out[i] = { .type = XR_TYPE_VIRTUAL_KEYBOARD_ANIMATION_STATE_META };
        }
        states.stateCapacityInput = capacity;
        states.states             = out;
        EXPECT_EQ(animation_states(keyboard, &states), XR_SUCCESS);
        return states.stateCountOutput;
    }
};
} // namespace

TEST_F(OxrVirtualKeyboardTest, system_supports_the_keyboard)
{
    XrSystemVirtualKeyboardPropertiesMETA keyboards = { .type = XR_TYPE_SYSTEM_VIRTUAL_KEYBOARD_PROPERTIES_META };
    XrSystemProperties                    props     = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &keyboards };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(keyboards.supportsVirtualKeyboard, XR_TRUE);
}

TEST_F(OxrVirtualKeyboardTest, one_keyboard_per_session)
{
    const XrVirtualKeyboardCreateInfoMETA info = { .type = XR_TYPE_VIRTUAL_KEYBOARD_CREATE_INFO_META };
    XrVirtualKeyboardMETA                 second = XR_NULL_HANDLE;
    EXPECT_EQ(create_keyboard(XR_NULL_HANDLE, &info, &keyboard), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(create_keyboard(session, nullptr, &keyboard), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_keyboard(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(create_keyboard(session, &info, &keyboard), XR_SUCCESS);
    EXPECT_EQ(create_keyboard(session, &info, &second), XR_ERROR_LIMIT_REACHED);
    EXPECT_EQ(destroy_keyboard(keyboard), XR_SUCCESS);
    EXPECT_EQ(destroy_keyboard(keyboard), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(create_keyboard(session, &info, &second), XR_SUCCESS);
}

TEST_F(OxrVirtualKeyboardTest, space_follows_the_suggested_location)
{
    create_shown_keyboard();
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    const XrTime    now      = sim_predicted_display_time(oxr_get_sim());
    ASSERT_EQ(xr.xrLocateSpace(keyboard_space, stage, now, &location), XR_SUCCESS);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    EXPECT_NEAR(location.pose.position.y, 1.0f, 1e-5f);
    EXPECT_NEAR(location.pose.position.z, -1.0f, 1e-5f);
    f32 scale = 0.0f;
    ASSERT_EQ(get_scale(keyboard, &scale), XR_SUCCESS);
    EXPECT_EQ(scale, 1.0f) << "spec: the scale starts at 1";
    EXPECT_EQ(get_scale(keyboard, nullptr), XR_ERROR_VALIDATION_FAILURE);

    XrVirtualKeyboardLocationInfoMETA info = { .type = XR_TYPE_VIRTUAL_KEYBOARD_LOCATION_INFO_META, .locationType = XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_CUSTOM_META, .space = stage, .poseInSpace = { { 0, 0, 0, 1 }, { 0.5f, 1.2f, -0.7f } }, .scale = 0.5f };
    ASSERT_EQ(suggest_location(keyboard, &info), XR_SUCCESS);
    ASSERT_EQ(xr.xrLocateSpace(keyboard_space, stage, now, &location), XR_SUCCESS);
    EXPECT_NEAR(location.pose.position.x, 0.5f, 1e-5f);
    ASSERT_EQ(get_scale(keyboard, &scale), XR_SUCCESS);
    EXPECT_EQ(scale, 0.5f);

    info.poseInSpace.orientation = { 0, 0, 0, 0 };
    EXPECT_EQ(suggest_location(keyboard, &info), XR_ERROR_POSE_INVALID);
    info.poseInSpace.orientation = { 0, 0, 0, 1 };
    info.scale                   = 0.0f;
    EXPECT_EQ(suggest_location(keyboard, &info), XR_ERROR_VALIDATION_FAILURE);
    info.locationType = (XrVirtualKeyboardLocationTypeMETA)7;
    EXPECT_EQ(suggest_location(keyboard, &info), XR_ERROR_VALIDATION_FAILURE);

    // Spec: the default places ignore the pose and scale; both are in front of the head and below it
    for (const XrVirtualKeyboardLocationTypeMETA type : { XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_FAR_META, XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_DIRECT_META })
    {
        info.locationType = type;
        ASSERT_EQ(suggest_location(keyboard, &info), XR_SUCCESS);
        ASSERT_EQ(xr.xrLocateSpace(keyboard_space, stage, now, &location), XR_SUCCESS);
        ASSERT_EQ(get_scale(keyboard, &scale), XR_SUCCESS);
        EXPECT_EQ(scale, 1.0f);
        const Pose head = sim_get_device_pose(oxr_get_sim(), SIM_Device::Head);
        EXPECT_LT(location.pose.position.z, head.position.z - 0.2f);
        EXPECT_LT(location.pose.position.y, head.position.y);
    }

    ASSERT_EQ(destroy_keyboard(keyboard), XR_SUCCESS);
    ASSERT_EQ(xr.xrLocateSpace(keyboard_space, stage, now, &location), XR_SUCCESS);
    EXPECT_EQ(location.locationFlags, 0u) << "the space of a destroyed keyboard is lost";
}

TEST_F(OxrVirtualKeyboardTest, visibility_changes_send_events)
{
    const XrVirtualKeyboardCreateInfoMETA info = { .type = XR_TYPE_VIRTUAL_KEYBOARD_CREATE_INFO_META };
    ASSERT_EQ(create_keyboard(session, &info, &keyboard), XR_SUCCESS);
    drain_events();
    XrVirtualKeyboardModelVisibilitySetInfoMETA visibility = { .type = XR_TYPE_VIRTUAL_KEYBOARD_MODEL_VISIBILITY_SET_INFO_META, .visible = XR_FALSE };
    ASSERT_EQ(set_visibility(keyboard, &visibility), XR_SUCCESS);
    XrEventDataBuffer event = {};
    EXPECT_FALSE(poll_for(XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_HIDDEN_META, event)) << "spec: hidden by default";
    visibility.visible = XR_TRUE;
    ASSERT_EQ(set_visibility(keyboard, &visibility), XR_SUCCESS);
    ASSERT_TRUE(poll_for(XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_SHOWN_META, event));
    EXPECT_EQ(((XrEventDataVirtualKeyboardShownMETA*)&event)->keyboard, keyboard);
    ASSERT_EQ(set_visibility(keyboard, &visibility), XR_SUCCESS);
    EXPECT_FALSE(poll_for(XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_SHOWN_META, event));
    visibility.visible = XR_FALSE;
    ASSERT_EQ(set_visibility(keyboard, &visibility), XR_SUCCESS);
    EXPECT_TRUE(poll_for(XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_HIDDEN_META, event));
    EXPECT_EQ(set_visibility(keyboard, nullptr), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrVirtualKeyboardTest, model_loads_with_texture_uri_and_key_animations)
{
    PFN_xrEnumerateRenderModelPathsFB enumerate  = nullptr;
    PFN_xrGetRenderModelPropertiesFB  properties = nullptr;
    PFN_xrLoadRenderModelFB           load       = nullptr;
    ASSERT_EQ(gipa(instance, "xrEnumerateRenderModelPathsFB", (PFN_xrVoidFunction*)&enumerate), XR_SUCCESS);
    ASSERT_EQ(gipa(instance, "xrGetRenderModelPropertiesFB", (PFN_xrVoidFunction*)&properties), XR_SUCCESS);
    ASSERT_EQ(gipa(instance, "xrLoadRenderModelFB", (PFN_xrVoidFunction*)&load), XR_SUCCESS);
    u32                     count    = 0;
    XrRenderModelPathInfoFB paths[8] = {};
    for (XrRenderModelPathInfoFB& info : paths)
    {
        info.type = XR_TYPE_RENDER_MODEL_PATH_INFO_FB;
    }
    ASSERT_EQ(enumerate(session, 8, &count, paths), XR_SUCCESS);
    ASSERT_EQ(count, 5u);
    EXPECT_EQ(paths[4].path, path("/model_meta/keyboard/virtual"));

    // Spec XR_FB_render_model: without a model of a requested level, the model is unavailable;
    // the keyboard is textured, so only subset 2 loaders get it
    XrRenderModelCapabilitiesRequestFB request = { .type = XR_TYPE_RENDER_MODEL_CAPABILITIES_REQUEST_FB, .flags = XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB };
    XrRenderModelPropertiesFB          props   = { .type = XR_TYPE_RENDER_MODEL_PROPERTIES_FB, .next = &request };
    EXPECT_EQ(properties(session, path("/model_meta/keyboard/virtual"), &props), XR_RENDER_MODEL_UNAVAILABLE_FB);
    request.flags = XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_2_BIT_FB;
    ASSERT_EQ(properties(session, path("/model_meta/keyboard/virtual"), &props), XR_SUCCESS);
    EXPECT_EQ(props.flags, XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_2_BIT_FB);
    EXPECT_EQ(properties(session, path("/model_fb/controller/left"), &props), XR_SUCCESS) << "controllers are subset 2 as well";

    ASSERT_EQ(properties(session, path("/model_meta/keyboard/virtual"), &props), XR_SUCCESS);
    const XrRenderModelLoadInfoFB info   = { .type = XR_TYPE_RENDER_MODEL_LOAD_INFO_FB, .modelKey = props.modelKey };
    XrRenderModelBufferFB         buffer = { .type = XR_TYPE_RENDER_MODEL_BUFFER_FB };
    ASSERT_EQ(load(session, &info, &buffer), XR_SUCCESS);
    Array<u8> glb = {};
    defer { glb.release(); };
    glb.resize(buffer.bufferCountOutput);
    buffer.buffer              = glb.data;
    buffer.bufferCapacityInput = (u32)glb.length;
    ASSERT_EQ(load(session, &info, &buffer), XR_SUCCESS);

    const u32   chunk_length = (u32)glb[12] | ((u32)glb[13] << 8) | ((u32)glb[14] << 16) | ((u32)glb[15] << 24);
    Array<char> json         = {};
    defer { json.release(); };
    json.add((const char*)glb.data + 20, chunk_length);
    json.add('\0');
    // Spec XR_META_virtual_keyboard: textures the runtime updates are named by this URI
    EXPECT_NE(cstr_find(json.data, "\"uri\":\"metaVirtualKeyboard://texture/1?w=256&h=256&fmt=RGBA32\""), nullptr);
    EXPECT_NE(cstr_find(json.data, "\"name\":\"collision\""), nullptr);
    JsonParser* parser = json_parse_from_string(json.data);
    ASSERT_NE(parser, nullptr);
    defer { json_release(parser); };
    const JsonValue animations = json_find(json_get_root(parser), "animations");
    ASSERT_EQ(json_length(animations), layout.keys_count) << "one press animation per key, in key order";
    u32 node = 99;
    ASSERT_TRUE(json_find_uint(json_at(json_find(json_at(animations, 5), "channels"), 0), "sampler", node));
    EXPECT_EQ(node, 0u);
    ASSERT_TRUE(json_find_uint(json_find(json_at(json_find(json_at(animations, 5), "channels"), 0), "target"), "node", node));
    EXPECT_EQ(node, 5u);
}

TEST_F(OxrVirtualKeyboardTest, textures_are_dirty_until_read)
{
    create_shown_keyboard();
    u64 ids[2]  = {};
    u32 count   = 0;
    ASSERT_EQ(dirty_textures(keyboard, 0, &count, nullptr), XR_SUCCESS);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(dirty_textures(keyboard, 2, nullptr, ids), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(dirty_textures(keyboard, 2, &count, ids), XR_SUCCESS);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(ids[0], (u64)SIM_VKB_TEXTURE_ID);
    ASSERT_EQ(dirty_textures(keyboard, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 0u);

    XrVirtualKeyboardTextureDataMETA data = { .type = XR_TYPE_VIRTUAL_KEYBOARD_TEXTURE_DATA_META };
    EXPECT_EQ(texture_data(keyboard, 99, &data), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(texture_data(keyboard, SIM_VKB_TEXTURE_ID, &data), XR_SUCCESS);
    EXPECT_EQ(data.textureWidth, (u32)SIM_VKB_TEXTURE_WIDTH);
    EXPECT_EQ(data.textureHeight, (u32)SIM_VKB_TEXTURE_HEIGHT);
    ASSERT_EQ(data.bufferCountOutput, (u32)(SIM_VKB_TEXTURE_WIDTH * SIM_VKB_TEXTURE_HEIGHT * 4));
    Array<u8> pixels = {};
    defer { pixels.release(); };
    pixels.resize(data.bufferCountOutput);
    data.buffer              = pixels.data;
    data.bufferCapacityInput = data.bufferCountOutput - 1;
    EXPECT_EQ(texture_data(keyboard, SIM_VKB_TEXTURE_ID, &data), XR_ERROR_SIZE_INSUFFICIENT);
    data.bufferCapacityInput = data.bufferCountOutput;
    ASSERT_EQ(texture_data(keyboard, SIM_VKB_TEXTURE_ID, &data), XR_SUCCESS);
    Array<u8> expected = {};
    defer { expected.release(); };
    expected.resize(pixels.length);
    sim_vkb_texture(layout, false, expected.data);
    EXPECT_TRUE(memory_equal(pixels.data, expected.data, pixels.length));
}

TEST_F(OxrVirtualKeyboardTest, ray_press_commits_once_and_animates_the_key)
{
    create_shown_keyboard();
    XrVirtualKeyboardAnimationStateMETA states[SIM_VKB_MAX_KEYS] = {};
    EXPECT_EQ(read_states(states, SIM_VKB_MAX_KEYS), layout.keys_count) << "the first read has every key";
    EXPECT_EQ(read_states(states, SIM_VKB_MAX_KEYS), 0u) << "then only changes";

    const XrVector3f q    = key_point("q");
    char             text[64];
    ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_RIGHT_META, q, false), XR_SUCCESS);
    typed(text, sizeof(text));
    EXPECT_STREQ(text, "") << "pointing alone types nothing";
    ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_RIGHT_META, q, true), XR_SUCCESS);
    ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_RIGHT_META, q, true), XR_SUCCESS);
    typed(text, sizeof(text));
    EXPECT_STREQ(text, "q") << "a held press commits once";
    ASSERT_EQ(read_states(states, SIM_VKB_MAX_KEYS), 1u);
    EXPECT_EQ(states[0].animationIndex, sim_vkb_find_key(layout, "q"));
    EXPECT_EQ(states[0].fraction, 1.0f);
    ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_RIGHT_META, q, false), XR_SUCCESS);
    ASSERT_EQ(read_states(states, SIM_VKB_MAX_KEYS), 1u);
    EXPECT_EQ(states[0].fraction, 0.0f);

    // A press that starts beside the keys and slides onto one types nothing
    ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_HAND_RAY_LEFT_META, { 2.0f, 1.0f, -1.0f }, true), XR_SUCCESS);
    ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_HAND_RAY_LEFT_META, q, true), XR_SUCCESS);
    typed(text, sizeof(text));
    EXPECT_STREQ(text, "");
}

TEST_F(OxrVirtualKeyboardTest, shift_backspace_enter_and_space)
{
    create_shown_keyboard();
    const char* const presses[] = { "h", "shift", "i", "j", "space", "backspace", "enter", "," };
    for (const char* key : presses)
    {
        ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_LEFT_META, key_point(key), true), XR_SUCCESS);
        ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_LEFT_META, key_point(key), false), XR_SUCCESS);
    }
    char text[64];
    typed(text, sizeof(text));
    EXPECT_STREQ(text, "hIj <>,") << "shift holds for one character";
}

TEST_F(OxrVirtualKeyboardTest, shift_redraws_the_labels)
{
    create_shown_keyboard();
    u64 id    = 0;
    u32 count = 0;
    ASSERT_EQ(dirty_textures(keyboard, 1, &count, &id), XR_SUCCESS);
    ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_LEFT_META, key_point("shift"), true), XR_SUCCESS);
    ASSERT_EQ(dirty_textures(keyboard, 1, &count, &id), XR_SUCCESS);
    ASSERT_EQ(count, 1u);

    Array<u8> pixels   = {};
    Array<u8> expected = {};
    defer { pixels.release(); };
    defer { expected.release(); };
    pixels.resize(SIM_VKB_TEXTURE_WIDTH * SIM_VKB_TEXTURE_HEIGHT * 4);
    expected.resize(pixels.length);
    XrVirtualKeyboardTextureDataMETA data = { .type = XR_TYPE_VIRTUAL_KEYBOARD_TEXTURE_DATA_META, .bufferCapacityInput = (u32)pixels.length, .buffer = pixels.data };
    ASSERT_EQ(texture_data(keyboard, SIM_VKB_TEXTURE_ID, &data), XR_SUCCESS);
    sim_vkb_texture(layout, true, expected.data);
    EXPECT_TRUE(memory_equal(pixels.data, expected.data, pixels.length));
}

TEST_F(OxrVirtualKeyboardTest, direct_touch_types_and_limits_the_poke)
{
    create_shown_keyboard();
    const XrVector3f a     = key_point("a");
    XrPosef          root  = { { 0, 0, 0, 1 }, { a.x, a.y, a.z + 0.2f } };
    XrPosef          above = root;
    ASSERT_EQ(poke(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_HAND_DIRECT_INDEX_TIP_RIGHT_META, { a.x, a.y, a.z + 0.02f }, root), XR_SUCCESS);
    EXPECT_EQ(root.position.z, above.position.z) << "hovering is not limited";
    char text[64];
    typed(text, sizeof(text));
    EXPECT_STREQ(text, "");

    // 2 cm into the keyboard: the key is pressed and the hand is held at its pressed depth
    ASSERT_EQ(poke(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_HAND_DIRECT_INDEX_TIP_RIGHT_META, { a.x, a.y, a.z - 0.02f }, root), XR_SUCCESS);
    typed(text, sizeof(text));
    EXPECT_STREQ(text, "a");
    EXPECT_NEAR(root.position.z, above.position.z + 0.02f - SIM_VKB_PRESS, 1e-5f);

    // A near direct input silences its side's ray
    ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_HAND_RAY_RIGHT_META, key_point("p"), true), XR_SUCCESS);
    typed(text, sizeof(text));
    EXPECT_STREQ(text, "");
}

TEST_F(OxrVirtualKeyboardTest, input_follows_the_scale)
{
    create_shown_keyboard(0.5f);
    ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_RIGHT_META, key_point("m", 0.5f), true), XR_SUCCESS);
    char text[64];
    typed(text, sizeof(text));
    EXPECT_STREQ(text, "m");
}

TEST_F(OxrVirtualKeyboardTest, hidden_keyboard_ignores_input)
{
    create_shown_keyboard();
    const XrVirtualKeyboardModelVisibilitySetInfoMETA hidden = { .type = XR_TYPE_VIRTUAL_KEYBOARD_MODEL_VISIBILITY_SET_INFO_META, .visible = XR_FALSE };
    ASSERT_EQ(set_visibility(keyboard, &hidden), XR_SUCCESS);
    ASSERT_EQ(ray(XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_RIGHT_META, key_point("q"), true), XR_SUCCESS);
    char text[64];
    typed(text, sizeof(text));
    EXPECT_STREQ(text, "");
}

TEST_F(OxrVirtualKeyboardTest, input_is_validated)
{
    create_shown_keyboard();
    XrPosef                        root = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
    XrVirtualKeyboardInputInfoMETA info = { .type = XR_TYPE_VIRTUAL_KEYBOARD_INPUT_INFO_META, .inputSource = XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_LEFT_META, .inputSpace = stage, .inputPoseInSpace = root };
    EXPECT_EQ(send_input(XR_NULL_HANDLE, &info, &root), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(send_input(keyboard, nullptr, &root), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(send_input(keyboard, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    info.inputSource = (XrVirtualKeyboardInputSourceMETA)0;
    EXPECT_EQ(send_input(keyboard, &info, &root), XR_ERROR_VALIDATION_FAILURE);
    info.inputSource = (XrVirtualKeyboardInputSourceMETA)9;
    EXPECT_EQ(send_input(keyboard, &info, &root), XR_ERROR_VALIDATION_FAILURE);
    info.inputSource = XR_VIRTUAL_KEYBOARD_INPUT_SOURCE_CONTROLLER_RAY_LEFT_META;
    info.inputSpace  = XR_NULL_HANDLE;
    EXPECT_EQ(send_input(keyboard, &info, &root), XR_ERROR_HANDLE_INVALID);
    info.inputSpace                   = stage;
    info.inputPoseInSpace.orientation = { 0, 0, 0, 0 };
    EXPECT_EQ(send_input(keyboard, &info, &root), XR_ERROR_POSE_INVALID);

    const XrVirtualKeyboardTextContextChangeInfoMETA context = { .type = XR_TYPE_VIRTUAL_KEYBOARD_TEXT_CONTEXT_CHANGE_INFO_META, .textContext = "hello" };
    EXPECT_EQ(change_context(keyboard, &context), XR_SUCCESS);
    const XrVirtualKeyboardTextContextChangeInfoMETA missing = { .type = XR_TYPE_VIRTUAL_KEYBOARD_TEXT_CONTEXT_CHANGE_INFO_META };
    EXPECT_EQ(change_context(keyboard, &missing), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrVirtualKeyboardTest, typed_text_arrives_at_the_next_frame)
{
    begin_session();
    create_shown_keyboard();
    ASSERT_TRUE(sim_type_text(oxr_get_sim(), "hi \bthere\n"));
    char text[64];
    typed(text, sizeof(text));
    EXPECT_STREQ(text, "") << "typing waits for the next frame";
    frame();
    typed(text, sizeof(text));
    EXPECT_STREQ(text, "hi <there>");

    OXR_KeyboardState state = {};
    ASSERT_TRUE(oxr_get_keyboard_state(state));
    EXPECT_TRUE(state.exists);
    EXPECT_TRUE(state.visible);
}

//
// XR_META_dynamic_object_tracker, XR_META_dynamic_object_keyboard (preview: Meta's SDK has the
// only header, so the values and layouts are written out here)
//

namespace
{

const XrStructureType DOT_CREATE_INFO         = (XrStructureType)OXR_TYPE_DYNAMIC_OBJECT_TRACKER_CREATE_INFO_META;
const XrStructureType DOT_CLASSES_SET_INFO    = (XrStructureType)OXR_TYPE_DYNAMIC_OBJECT_TRACKED_CLASSES_SET_INFO_META;
const XrStructureType DOT_DATA                = (XrStructureType)OXR_TYPE_DYNAMIC_OBJECT_DATA_META;
const XrStructureType DOT_CREATE_RESULT       = (XrStructureType)OXR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_TRACKER_CREATE_RESULT_META;
const XrStructureType DOT_CLASSES_RESULT      = (XrStructureType)OXR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_SET_TRACKED_CLASSES_RESULT_META;
const XrStructureType DOT_SYSTEM_PROPERTIES   = (XrStructureType)OXR_TYPE_SYSTEM_DYNAMIC_OBJECT_TRACKER_PROPERTIES_META;
const XrStructureType DOT_KEYBOARD_PROPERTIES = (XrStructureType)OXR_TYPE_SYSTEM_DYNAMIC_OBJECT_KEYBOARD_PROPERTIES_META;
const u32             DOT_COMPONENT           = OXR_SPACE_COMPONENT_TYPE_DYNAMIC_OBJECT_DATA_META;
const u32             DOT_CLASS_KEYBOARD      = OXR_DYNAMIC_OBJECT_CLASS_KEYBOARD_META;

// The runtime's declarations, so calls go through the exact function types
using DotCreateInfo     = OXR_DynamicObjectTrackerCreateInfoMETA;
using DotClassesSetInfo = OXR_DynamicObjectTrackedClassesSetInfoMETA;
using DotData           = OXR_DynamicObjectDataMETA;
using DotResultEvent    = OXR_EventDataDynamicObjectResultMETA;
using DotSupported      = OXR_SystemDynamicObjectPropertiesMETA;
using PFN_CreateDot     = PFN_xrCreateDynamicObjectTrackerMETA;
using PFN_DestroyDot    = PFN_xrDestroyDynamicObjectTrackerMETA;
using PFN_SetDotClasses = PFN_xrSetDynamicObjectTrackedClassesMETA;
using PFN_GetDotData    = PFN_xrGetSpaceDynamicObjectDataMETA;

const char* const dynamic_object_extensions[] = {
    XR_MND_HEADLESS_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_CONTAINER_EXTENSION_NAME,
    XR_FB_SCENE_EXTENSION_NAME,
    XR_FB_SCENE_CAPTURE_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_DISCOVERY_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_MESH_EXTENSION_NAME,
    XR_EXT_FUTURE_EXTENSION_NAME,
    "XR_META_dynamic_object_tracker",
    "XR_META_dynamic_object_keyboard",
};

struct OxrDynamicObjectTest : OxrSceneTest
{
    PFN_CreateDot     create_tracker  = nullptr;
    PFN_DestroyDot    destroy_tracker = nullptr;
    PFN_SetDotClasses set_classes     = nullptr;
    PFN_GetDotData    get_data        = nullptr;
    u64               tracker         = 0;

    OxrDynamicObjectTest ()
    {
        start_extensions       = dynamic_object_extensions;
        start_extensions_count = ARRAY_SIZE(dynamic_object_extensions);
    }

    void load ()
    {
        ASSERT_EQ(gipa(instance, "xrCreateDynamicObjectTrackerMETA", (PFN_xrVoidFunction*)&create_tracker), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrDestroyDynamicObjectTrackerMETA", (PFN_xrVoidFunction*)&destroy_tracker), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrSetDynamicObjectTrackedClassesMETA", (PFN_xrVoidFunction*)&set_classes), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrGetSpaceDynamicObjectDataMETA", (PFN_xrVoidFunction*)&get_data), XR_SUCCESS);
    }

    // A tracker that looks for keyboards, both of its results received
    void track_keyboards ()
    {
        const DotCreateInfo info = { .type = DOT_CREATE_INFO };
        ASSERT_EQ(create_tracker(session, &info, &tracker), XR_SUCCESS);
        XrEventDataBuffer event = {};
        ASSERT_TRUE(wait_event(DOT_CREATE_RESULT, event));
        EXPECT_EQ(((DotResultEvent*)&event)->handle, tracker);
        EXPECT_EQ(((DotResultEvent*)&event)->result, XR_SUCCESS);
        const DotClassesSetInfo set = { .type = DOT_CLASSES_SET_INFO, .classCount = 1, .classes = &DOT_CLASS_KEYBOARD };
        ASSERT_EQ(set_classes(tracker, &set), XR_SUCCESS);
        ASSERT_TRUE(wait_event(DOT_CLASSES_RESULT, event));
        EXPECT_EQ(((DotResultEvent*)&event)->result, XR_SUCCESS);
    }

    u32 discover_dynamic (XrSpaceDiscoveryResultMETA* out, u32 capacity)
    {
        const XrSpaceFilterComponentMETA filter = { .type = XR_TYPE_SPACE_FILTER_COMPONENT_META, .componentType = (XrSpaceComponentTypeFB)DOT_COMPONENT };
        return discover_all((const XrSpaceFilterBaseHeaderMETA*)&filter, out, capacity);
    }
};

} // namespace

TEST_F(OxrDynamicObjectTest, keyboards_need_the_tracker)
{
    const char* const extensions[] = { XR_MND_HEADLESS_EXTENSION_NAME, "XR_META_dynamic_object_keyboard" };
    EXPECT_EQ(create_instance(extensions, 2), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
}

TEST_F(OxrDynamicObjectTest, system_supports_keyboard_tracking)
{
    start();
    DotSupported              keyboard = { .type = DOT_KEYBOARD_PROPERTIES };
    DotSupported              tracking = { .type = DOT_SYSTEM_PROPERTIES, .next = &keyboard };
    XrSystemProperties        props    = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &tracking };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(tracking.supported, XR_TRUE);
    EXPECT_EQ(keyboard.supported, XR_TRUE);
}

// Meta's docs: a create result of XR_ERROR_INITIALIZATION_FAILED means the user turned the
// tracker off in the system settings; apps destroy it and try again later
TEST_F(OxrDynamicObjectTest, a_tracker_turned_off_in_the_settings_fails_to_initialize)
{
    os_set_env("PUPPETXR_KEYBOARD_TRACKING", "0");
    defer { os_unset_env("PUPPETXR_KEYBOARD_TRACKING"); };
    start();
    load();
    const DotCreateInfo info = { .type = DOT_CREATE_INFO };
    ASSERT_EQ(create_tracker(session, &info, &tracker), XR_SUCCESS) << "the result comes as an event";
    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_event(DOT_CREATE_RESULT, event));
    EXPECT_EQ(((DotResultEvent*)&event)->handle, tracker);
    EXPECT_EQ(((DotResultEvent*)&event)->result, XR_ERROR_INITIALIZATION_FAILED);
    EXPECT_EQ(destroy_tracker(tracker), XR_SUCCESS);
}

TEST_F(OxrDynamicObjectTest, trackers_are_validated)
{
    start();
    load();
    const DotCreateInfo info = { .type = DOT_CREATE_INFO };
    EXPECT_EQ(create_tracker(XR_NULL_HANDLE, &info, &tracker), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(create_tracker(session, nullptr, &tracker), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_tracker(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);
    ASSERT_EQ(create_tracker(session, &info, &tracker), XR_SUCCESS);
    const u32               unknown = 7;
    const DotClassesSetInfo bad     = { .type = DOT_CLASSES_SET_INFO, .classCount = 1, .classes = &unknown };
    EXPECT_EQ(set_classes(tracker, &bad), XR_ERROR_VALIDATION_FAILURE);
    const DotClassesSetInfo none = { .type = DOT_CLASSES_SET_INFO, .classCount = 1 };
    EXPECT_EQ(set_classes(tracker, &none), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(set_classes(0, &bad), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(destroy_tracker(tracker), XR_SUCCESS);
    EXPECT_EQ(destroy_tracker(tracker), XR_ERROR_HANDLE_INVALID);
}

// MRUK queries the spaces every second and takes a new handle for a new entity: a space per
// query made it remove and add the keyboard every second. A query returns the session's live space
// of an entity, a new one only after the app destroyed it
TEST_F(OxrDynamicObjectTest, queries_return_the_live_space_of_an_entity)
{
    start();
    load();
    const SIM_Prop keyboard = { .kind = SIM_PropKind::Keyboard, .pose = { { 0.2f, 0.75f, -0.5f }, zm::QUAT_IDENTITY } };
    ASSERT_NE(sim_add_prop(oxr_get_sim(), keyboard), 0u);
    track_keyboards();

    XrSpaceDiscoveryResultMETA first[4]  = {};
    XrSpaceDiscoveryResultMETA second[4] = {};
    ASSERT_EQ(discover_dynamic(first, 4), 1u);
    ASSERT_EQ(discover_dynamic(second, 4), 1u);
    EXPECT_EQ(second[0].space, first[0].space) << "the keyboard keeps its handle";

    XrSpaceDiscoveryResultMETA room_first[64]  = {};
    XrSpaceDiscoveryResultMETA room_second[64] = {};
    const u32 entities = discover_all(nullptr, room_first, 64);
    ASSERT_GT(entities, 1u);
    ASSERT_EQ(discover_all(nullptr, room_second, 64), entities);
    for (u32 i = 0; i < entities; i++)
    {
        EXPECT_EQ(room_second[i].space, room_first[i].space) << "entity " << i;
    }

    // MRUK's own call: a query without filters
    const XrSpaceQueryInfoFB info       = { .type = XR_TYPE_SPACE_QUERY_INFO_FB, .queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB, .maxResultCount = 64 };
    XrAsyncRequestIdFB       request_id = 0;
    ASSERT_EQ(scene.xrQuerySpacesFB(session, (const XrSpaceQueryInfoBaseHeaderFB*)&info, &request_id), XR_SUCCESS);
    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB, event));
    XrSpaceQueryResultFB  queried[64] = {};
    XrSpaceQueryResultsFB results     = { .type = XR_TYPE_SPACE_QUERY_RESULTS_FB, .resultCapacityInput = 64, .results = queried };
    ASSERT_EQ(scene.xrRetrieveSpaceQueryResultsFB(session, request_id, &results), XR_SUCCESS);
    b8 found = false;
    for (u32 i = 0; i < results.resultCountOutput; i++)
    {
        found |= queried[i].space == first[0].space;
    }
    EXPECT_TRUE(found) << "the query returns the discovered keyboard's space";

    ASSERT_EQ(xr.xrDestroySpace(first[0].space), XR_SUCCESS);
    XrSpaceDiscoveryResultMETA third[4] = {};
    ASSERT_EQ(discover_dynamic(third, 4), 1u);
    EXPECT_NE(third[0].space, XR_NULL_HANDLE);
    DotData data = { .type = DOT_DATA };
    EXPECT_EQ(get_data(third[0].space, &data), XR_SUCCESS) << "a destroyed space's entity gets a new one";
}

TEST_F(OxrDynamicObjectTest, tracked_keyboards_are_discovered_as_spaces)
{
    start();
    load();
    const SIM_Prop keyboard = { .kind = SIM_PropKind::Keyboard, .pose = { { 0.2f, 0.75f, -0.5f }, zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.3f) } };
    SIM_Prop       qr       = { .kind = SIM_PropKind::QrCode, .pose = { { 0.0f, 1.5f, -1.0f }, zm::QUAT_IDENTITY }, .size = 0.1f };
    const u32      id       = sim_add_prop(oxr_get_sim(), keyboard);
    ASSERT_NE(id, 0u);
    ASSERT_NE(sim_add_prop(oxr_get_sim(), qr), 0u);

    XrSpaceDiscoveryResultMETA results[4] = {};
    EXPECT_EQ(discover_dynamic(results, 4), 0u) << "nothing is tracked before the tracker looks for keyboards";
    track_keyboards();
    ASSERT_EQ(discover_dynamic(results, 4), 1u) << "only the keyboard, not the QR code";
    const XrSpace space = results[0].space;

    DotData data = { .type = DOT_DATA };
    ASSERT_EQ(get_data(space, &data), XR_SUCCESS);
    EXPECT_EQ(data.classType, DOT_CLASS_KEYBOARD);
    EXPECT_EQ(get_data(stage, &data), XR_ERROR_SPACE_COMPONENT_NOT_SUPPORTED_FB);
    EXPECT_EQ(get_data(space, nullptr), XR_ERROR_VALIDATION_FAILURE);

    XrSpaceComponentTypeFB types[8] = {};
    u32                    count    = 0;
    ASSERT_EQ(scene.xrEnumerateSpaceSupportedComponentsFB(space, 8, &count, types), XR_SUCCESS);
    b8 has_data = false;
    for (u32 i = 0; i < count; i++)
    {
        has_data |= (u32)types[i] == DOT_COMPONENT;
    }
    EXPECT_TRUE(has_data);
    XrUuidEXT uuid = {};
    ASSERT_EQ(scene.xrGetSpaceUuidFB(space, &uuid), XR_SUCCESS);
    EXPECT_TRUE(memory_equal(uuid.data, results[0].uuid.data, XR_UUID_SIZE_EXT));

    // Enabled, it is located on the keyboard: +Z up out of the keys, +Y away from the user
    const XrSpaceComponentStatusSetInfoFB enable = { .type = XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB, .componentType = XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB, .enabled = XR_TRUE };
    XrAsyncRequestIdFB                    request = 0;
    XrSpaceComponentStatusFB              status  = { .type = XR_TYPE_SPACE_COMPONENT_STATUS_FB };
    ASSERT_EQ(scene.xrGetSpaceComponentStatusFB(space, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB, &status), XR_SUCCESS);
    if (!status.enabled)
    {
        ASSERT_EQ(scene.xrSetSpaceComponentStatusFB(space, &enable, &request), XR_SUCCESS);
    }
    const XrSpaceComponentStatusSetInfoFB bounded = { .type = XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB, .componentType = XR_SPACE_COMPONENT_TYPE_BOUNDED_3D_FB, .enabled = XR_TRUE };
    scene.xrSetSpaceComponentStatusFB(space, &bounded, &request);
    const XrSpaceLocation location = locate(space, stage);
    ASSERT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
    const Pose pose = { { location.pose.position.x, location.pose.position.y, location.pose.position.z }, { location.pose.orientation.x, location.pose.orientation.y, location.pose.orientation.z, location.pose.orientation.w } };
    EXPECT_TRUE(zm::near(pose.position, keyboard.pose.position, 1e-5f));
    EXPECT_TRUE(zm::near(zm::rotate(pose.orientation, { 0.0f, 0.0f, 1.0f }), Vec3{ 0.0f, 1.0f, 0.0f }, 1e-5f));
    EXPECT_TRUE(zm::near(zm::rotate(pose.orientation, { 0.0f, 1.0f, 0.0f }), zm::rotate(keyboard.pose.orientation, { 0.0f, 0.0f, -1.0f }), 1e-5f));
    XrRect3DfFB box = {};
    ASSERT_EQ(scene.xrGetSpaceBoundingBox3DFB(session, space, &box), XR_SUCCESS);
    EXPECT_NEAR(box.extent.width, 0.30f, 1e-5f);
    EXPECT_NEAR(box.extent.height, 0.112f, 1e-5f);
    EXPECT_NEAR(box.extent.depth, 0.018f, 1e-5f);
    EXPECT_NEAR(box.offset.x, -0.15f, 1e-5f);

    // It follows the prop, and is lost with it
    SIM_Prop moved = keyboard;
    moved.pose.position.x += 0.5f;
    ASSERT_TRUE(sim_update_prop(oxr_get_sim(), id, moved));
    EXPECT_NEAR(locate(space, stage).pose.position.x, 0.7f, 1e-5f);
    ASSERT_TRUE(sim_remove_prop(oxr_get_sim(), id));
    EXPECT_EQ(locate(space, stage).locationFlags, 0u);
}

TEST_F(OxrDynamicObjectTest, fb_queries_find_keyboards_too)
{
    start();
    load();
    track_keyboards();
    ASSERT_NE(sim_add_prop(oxr_get_sim(), { .kind = SIM_PropKind::Keyboard, .pose = zm::POSE_IDENTITY }), 0u);
    const XrSpaceStorageLocationFilterInfoFB local = { .type = XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB, .location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB };
    const XrSpaceComponentFilterInfoFB filter     = { .type = XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB, .next = &local, .componentType = (XrSpaceComponentTypeFB)DOT_COMPONENT };
    const XrSpaceQueryInfoFB           query      = { .type = XR_TYPE_SPACE_QUERY_INFO_FB, .queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB, .maxResultCount = 8, .filter = (const XrSpaceFilterInfoBaseHeaderFB*)&filter };
    XrAsyncRequestIdFB                 request_id = 0;
    ASSERT_EQ(scene.xrQuerySpacesFB(session, (const XrSpaceQueryInfoBaseHeaderFB*)&query, &request_id), XR_SUCCESS);
    XrEventDataBuffer event = {};
    ASSERT_TRUE(wait_event(XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB, event));
    XrSpaceQueryResultFB  result  = {};
    XrSpaceQueryResultsFB results = { .type = XR_TYPE_SPACE_QUERY_RESULTS_FB, .resultCapacityInput = 1, .results = &result };
    ASSERT_EQ(scene.xrRetrieveSpaceQueryResultsFB(session, request_id, &results), XR_SUCCESS);
    EXPECT_EQ(results.resultCountOutput, 1u);
}

//
// XR_FB_keyboard_tracking: the first keyboard prop as the system's tracked keyboard
//

namespace
{

struct OxrKeyboardTrackingTest : OxrTest
{
    PFN_xrQuerySystemTrackedKeyboardFB query        = nullptr;
    PFN_xrCreateKeyboardSpaceFB        create_space = nullptr;
    XrSpace                            stage        = XR_NULL_HANDLE;

    void start ()
    {
        extension = XR_FB_KEYBOARD_TRACKING_EXTENSION_NAME;
        create_headless_instance();
        ASSERT_EQ(gipa(instance, "xrQuerySystemTrackedKeyboardFB", (PFN_xrVoidFunction*)&query), XR_SUCCESS);
        ASSERT_EQ(gipa(instance, "xrCreateKeyboardSpaceFB", (PFN_xrVoidFunction*)&create_space), XR_SUCCESS);
        create_session();
        begin_session();
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    }

    XrKeyboardTrackingDescriptionFB query_keyboard (XrKeyboardTrackingQueryFlagsFB flags = XR_KEYBOARD_TRACKING_QUERY_LOCAL_BIT_FB) const
    {
        const XrKeyboardTrackingQueryFB info     = { .type = XR_TYPE_KEYBOARD_TRACKING_QUERY_FB, .flags = flags };
        XrKeyboardTrackingDescriptionFB keyboard = {};
        EXPECT_EQ(query(session, &info, &keyboard), XR_SUCCESS);
        return keyboard;
    }

    XrSpaceLocation locate (XrSpace space) const
    {
        XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
        EXPECT_EQ(xr.xrLocateSpace(space, stage, frame().predictedDisplayTime, &location), XR_SUCCESS);
        return location;
    }
};

Pose keyboard_pose_of (const XrPosef& pose)
{
    return { { pose.position.x, pose.position.y, pose.position.z }, { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w } };
}

} // namespace

TEST_F(OxrKeyboardTrackingTest, functions_and_support_need_the_extension)
{
    create_headless_instance();
    PFN_xrVoidFunction fn = nullptr;
    EXPECT_EQ(gipa(instance, "xrQuerySystemTrackedKeyboardFB", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    EXPECT_EQ(gipa(instance, "xrCreateKeyboardSpaceFB", &fn), XR_ERROR_FUNCTION_UNSUPPORTED);
    XrSystemKeyboardTrackingPropertiesFB keyboard = { .type = XR_TYPE_SYSTEM_KEYBOARD_TRACKING_PROPERTIES_FB, .supportsKeyboardTracking = XR_TRUE };
    XrSystemProperties                   props    = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &keyboard };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(keyboard.supportsKeyboardTracking, XR_FALSE);
}

TEST_F(OxrKeyboardTrackingTest, system_supports_keyboard_tracking)
{
    start();
    XrSystemKeyboardTrackingPropertiesFB keyboard = { .type = XR_TYPE_SYSTEM_KEYBOARD_TRACKING_PROPERTIES_FB };
    XrSystemProperties                   props    = { .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &keyboard };
    ASSERT_EQ(xr.xrGetSystemProperties(instance, system, &props), XR_SUCCESS);
    EXPECT_EQ(keyboard.supportsKeyboardTracking, XR_TRUE);
}

TEST_F(OxrKeyboardTrackingTest, the_system_keyboard_is_the_first_keyboard_prop)
{
    start();
    EXPECT_EQ(query_keyboard().flags, 0u) << "no keyboard prop, no keyboard";

    const SIM_Prop qr       = { .kind = SIM_PropKind::QrCode, .pose = zm::POSE_IDENTITY, .size = 0.1f };
    const SIM_Prop keyboard = { .kind = SIM_PropKind::Keyboard, .pose = { { 0.0f, 0.75f, -0.5f }, zm::QUAT_IDENTITY } };
    ASSERT_NE(sim_add_prop(oxr_get_sim(), qr), 0u);
    EXPECT_EQ(query_keyboard().flags, 0u) << "a QR code is no keyboard";
    const u32 first  = sim_add_prop(oxr_get_sim(), keyboard);
    const u32 second = sim_add_prop(oxr_get_sim(), keyboard);
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);

    const XrKeyboardTrackingDescriptionFB found = query_keyboard();
    EXPECT_EQ(found.flags, XR_KEYBOARD_TRACKING_EXISTS_BIT_FB | XR_KEYBOARD_TRACKING_LOCAL_BIT_FB | XR_KEYBOARD_TRACKING_CONNECTED_BIT_FB);
    EXPECT_EQ(found.trackedKeyboardId, (u64)first);
    EXPECT_NEAR(found.size.x, 0.30f, 1e-5f);
    EXPECT_NEAR(found.size.y, 0.018f, 1e-5f);
    EXPECT_NEAR(found.size.z, 0.112f, 1e-5f);
    EXPECT_STREQ(found.name, "puppetxr Keyboard");
    EXPECT_EQ(query_keyboard(XR_KEYBOARD_TRACKING_QUERY_LOCAL_BIT_FB | XR_KEYBOARD_TRACKING_QUERY_REMOTE_BIT_FB).trackedKeyboardId, (u64)first);
    EXPECT_EQ(query_keyboard(XR_KEYBOARD_TRACKING_QUERY_REMOTE_BIT_FB).flags, 0u) << "every keyboard is local";

    ASSERT_TRUE(sim_remove_prop(oxr_get_sim(), first));
    EXPECT_EQ(query_keyboard().trackedKeyboardId, (u64)second) << "the next keyboard takes over";
}

TEST_F(OxrKeyboardTrackingTest, queries_and_spaces_are_validated)
{
    start();
    const u32 id = sim_add_prop(oxr_get_sim(), { .kind = SIM_PropKind::Keyboard, .pose = zm::POSE_IDENTITY });
    ASSERT_NE(id, 0u);
    const XrKeyboardTrackingQueryFB info     = { .type = XR_TYPE_KEYBOARD_TRACKING_QUERY_FB, .flags = XR_KEYBOARD_TRACKING_QUERY_LOCAL_BIT_FB };
    const XrKeyboardTrackingQueryFB wrong    = { .type = XR_TYPE_SPACE_LOCATION, .flags = XR_KEYBOARD_TRACKING_QUERY_LOCAL_BIT_FB };
    const XrKeyboardTrackingQueryFB unknown  = { .type = XR_TYPE_KEYBOARD_TRACKING_QUERY_FB, .flags = 1 };
    XrKeyboardTrackingDescriptionFB keyboard = {};
    EXPECT_EQ(query(XR_NULL_HANDLE, &info, &keyboard), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(query(session, nullptr, &keyboard), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(query(session, &wrong, &keyboard), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(query(session, &unknown, &keyboard), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(query(session, &info, nullptr), XR_ERROR_VALIDATION_FAILURE);

    XrSpace                           space   = XR_NULL_HANDLE;
    const XrKeyboardSpaceCreateInfoFB create  = { .type = XR_TYPE_KEYBOARD_SPACE_CREATE_INFO_FB, .trackedKeyboardId = id };
    const XrKeyboardSpaceCreateInfoFB missing = { .type = XR_TYPE_KEYBOARD_SPACE_CREATE_INFO_FB, .trackedKeyboardId = id + 1 };
    const XrKeyboardSpaceCreateInfoFB untyped = { .type = XR_TYPE_SPACE_LOCATION, .trackedKeyboardId = id };
    EXPECT_EQ(create_space(XR_NULL_HANDLE, &create, &space), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(create_space(session, nullptr, &space), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_space(session, &untyped, &space), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_space(session, &create, nullptr), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(create_space(session, &missing, &space), XR_ERROR_VALIDATION_FAILURE) << "no keyboard of that id";
    const u32 qr = sim_add_prop(oxr_get_sim(), { .kind = SIM_PropKind::QrCode, .pose = zm::POSE_IDENTITY, .size = 0.1f });
    ASSERT_NE(qr, 0u);
    const XrKeyboardSpaceCreateInfoFB code = { .type = XR_TYPE_KEYBOARD_SPACE_CREATE_INFO_FB, .trackedKeyboardId = qr };
    EXPECT_EQ(create_space(session, &code, &space), XR_ERROR_VALIDATION_FAILURE) << "a QR code is no keyboard";
    ASSERT_EQ(create_space(session, &create, &space), XR_SUCCESS);
    EXPECT_EQ(xr.xrDestroySpace(space), XR_SUCCESS);
}

// Spec XR_FB_keyboard_tracking: the keyboard space's origin is the centre of the keyboard's
// bounding box with +Y up; the render model /model_fb/keyboard/local is drawn in it
TEST_F(OxrKeyboardTrackingTest, keyboard_spaces_follow_the_keyboard)
{
    start();
    const SIM_Prop prop = { .kind = SIM_PropKind::Keyboard, .pose = { { 0.2f, 0.75f, -0.5f }, zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.3f) } };
    const u32      id   = sim_add_prop(oxr_get_sim(), prop);
    ASSERT_NE(id, 0u);
    const XrKeyboardSpaceCreateInfoFB create = { .type = XR_TYPE_KEYBOARD_SPACE_CREATE_INFO_FB, .trackedKeyboardId = query_keyboard().trackedKeyboardId };
    XrSpace                           space  = XR_NULL_HANDLE;
    ASSERT_EQ(create_space(session, &create, &space), XR_SUCCESS);

    XrSpaceLocation location = locate(space);
    EXPECT_EQ(location.locationFlags, XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
    EXPECT_TRUE(zm::near(keyboard_pose_of(location.pose), prop.pose, 1e-5f));

    SIM_Prop moved = prop;
    moved.pose.position.x += 0.5f;
    ASSERT_TRUE(sim_update_prop(oxr_get_sim(), id, moved));
    location = locate(space);
    EXPECT_TRUE(zm::near(keyboard_pose_of(location.pose), moved.pose, 1e-5f));

    ASSERT_TRUE(sim_remove_prop(oxr_get_sim(), id));
    EXPECT_EQ(locate(space).locationFlags, 0u) << "a removed keyboard is not located";
    ASSERT_NE(sim_add_prop(oxr_get_sim(), prop), 0u);
    EXPECT_EQ(locate(space).locationFlags, 0u) << "a new keyboard is another keyboard";
}

TEST_F(OxrKeyboardTrackingTest, moving_keyboards_are_located_where_they_are_at_the_time)
{
    start();
    const SIM_Prop prop = { .kind = SIM_PropKind::Keyboard, .motion = SIM_PropMotion::Linear, .pose = { { 0.0f, 0.75f, -0.5f }, zm::QUAT_IDENTITY }, .target = { 2.0f, 0.75f, -0.5f }, .speed = 1.0f };
    ASSERT_NE(sim_add_prop(oxr_get_sim(), prop), 0u);
    const XrKeyboardSpaceCreateInfoFB create = { .type = XR_TYPE_KEYBOARD_SPACE_CREATE_INFO_FB, .trackedKeyboardId = query_keyboard().trackedKeyboardId };
    XrSpace                           space  = XR_NULL_HANDLE;
    ASSERT_EQ(create_space(session, &create, &space), XR_SUCCESS);
    const XrTime    now   = frame().predictedDisplayTime;
    XrSpaceLocation early = { .type = XR_TYPE_SPACE_LOCATION };
    XrSpaceLocation later = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(space, stage, now, &early), XR_SUCCESS);
    ASSERT_EQ(xr.xrLocateSpace(space, stage, now + 500000000, &later), XR_SUCCESS);
    EXPECT_NEAR(later.pose.position.x - early.pose.position.x, 0.5f, 1e-4f) << "half a second at 1 m/s";
}

//
// XR_EXT_spatial_entity, XR_EXT_spatial_marker_tracking: QR code props as markers
//

namespace
{

#define SPATIAL_FUNCTIONS(X)                          \
    X(xrEnumerateSpatialCapabilitiesEXT)              \
    X(xrEnumerateSpatialCapabilityComponentTypesEXT)  \
    X(xrEnumerateSpatialCapabilityFeaturesEXT)        \
    X(xrCreateSpatialContextAsyncEXT)                 \
    X(xrCreateSpatialContextCompleteEXT)              \
    X(xrDestroySpatialContextEXT)                     \
    X(xrCreateSpatialDiscoverySnapshotAsyncEXT)       \
    X(xrCreateSpatialDiscoverySnapshotCompleteEXT)    \
    X(xrQuerySpatialComponentDataEXT)                 \
    X(xrDestroySpatialSnapshotEXT)                    \
    X(xrCreateSpatialEntityFromIdEXT)                 \
    X(xrDestroySpatialEntityEXT)                      \
    X(xrCreateSpatialUpdateSnapshotEXT)               \
    X(xrGetSpatialBufferStringEXT)                    \
    X(xrGetSpatialBufferUint8EXT)                     \
    X(xrGetSpatialBufferFloatEXT)                     \
    X(xrPollFutureEXT)                                \
    X(xrCancelFutureEXT)

struct SpatialXr
{
#define MEMBER(fn) PFN_##fn fn = nullptr;
    SPATIAL_FUNCTIONS(MEMBER)
#undef MEMBER
};

const XrSpatialComponentTypeEXT marker_components[] = { XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT, XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT };

struct OxrSpatialTest : OxrTest
{
    SpatialXr spatial = {};
    XrSpace   stage   = XR_NULL_HANDLE;
    XrTime    now     = 0;

    void start (b8 markers = true)
    {
        const char* const extensions[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_EXT_FUTURE_EXTENSION_NAME, XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME, XR_EXT_SPATIAL_MARKER_TRACKING_EXTENSION_NAME };
        ASSERT_EQ(create_instance(extensions, markers ? 4 : 3), XR_SUCCESS);
#define LOAD(fn) ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&spatial.fn), XR_SUCCESS) << #fn;
        SPATIAL_FUNCTIONS(LOAD)
#undef LOAD
        const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
        ASSERT_EQ(xr.xrGetSystem(instance, &system_info, &system), XR_SUCCESS);
        create_session();
        begin_session();
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
        look({ 0.0f, 1.6f, 0.0f }, 0.0f);
    }

    // The head at `position`, turned `yaw` radians to the left from looking along -Z
    void look (Vec3 position, f32 yaw)
    {
        sim_set_device_pose(oxr_get_sim(), SIM_Device::Head, { position, zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, yaw) });
        now = frame().predictedDisplayTime;
    }

    u32 add_qr (Vec3 position, const char* text, Quat orientation = zm::QUAT_IDENTITY)
    {
        SIM_Prop prop = { .kind = SIM_PropKind::QrCode, .pose = { position, orientation }, .size = 0.15f };
        cstr_copy(prop.text, sizeof(prop.text), text);
        const u32 id = sim_add_prop(oxr_get_sim(), prop);
        EXPECT_NE(id, 0u);
        return id;
    }

    XrResult create_context (XrSpatialContextEXT& out_context, const XrSpatialComponentTypeEXT* components = marker_components, u32 count = 2, const void* features = nullptr)
    {
        const XrSpatialCapabilityConfigurationQrCodeEXT config  = { .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_QR_CODE_EXT, .next = features, .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, .enabledComponentCount = count, .enabledComponents = components };
        const XrSpatialCapabilityConfigurationBaseHeaderEXT* configs[] = { (const XrSpatialCapabilityConfigurationBaseHeaderEXT*)&config };
        const XrSpatialContextCreateInfoEXT info   = { .type = XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT, .capabilityConfigCount = 1, .capabilityConfigs = configs };
        XrFutureEXT                         future = XR_NULL_FUTURE_EXT;
        const XrResult                      result = spatial.xrCreateSpatialContextAsyncEXT(session, &info, &future);
        if (result != XR_SUCCESS)
        {
            return result;
        }
        XrCreateSpatialContextCompletionEXT completion = { .type = XR_TYPE_CREATE_SPATIAL_CONTEXT_COMPLETION_EXT };
        EXPECT_EQ(spatial.xrCreateSpatialContextCompleteEXT(session, future, &completion), XR_SUCCESS);
        EXPECT_EQ(completion.futureResult, XR_SUCCESS);
        out_context = completion.spatialContext;
        return XR_SUCCESS;
    }

    XrSpatialSnapshotEXT discover (XrSpatialContextEXT context, const XrSpatialComponentTypeEXT* components = nullptr, u32 count = 0, const void* filter = nullptr)
    {
        const XrSpatialDiscoverySnapshotCreateInfoEXT info   = { .type = XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT, .next = filter, .componentTypeCount = count, .componentTypes = components };
        XrFutureEXT                                   future = XR_NULL_FUTURE_EXT;
        EXPECT_EQ(spatial.xrCreateSpatialDiscoverySnapshotAsyncEXT(context, &info, &future), XR_SUCCESS);
        const XrFuturePollInfoEXT poll  = { .type = XR_TYPE_FUTURE_POLL_INFO_EXT, .future = future };
        XrFuturePollResultEXT     state = { .type = XR_TYPE_FUTURE_POLL_RESULT_EXT };
        EXPECT_EQ(spatial.xrPollFutureEXT(instance, &poll, &state), XR_SUCCESS);
        EXPECT_EQ(state.state, XR_FUTURE_STATE_READY_EXT);
        const XrCreateSpatialDiscoverySnapshotCompletionInfoEXT complete   = { .type = XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_INFO_EXT, .baseSpace = stage, .time = now, .future = future };
        XrCreateSpatialDiscoverySnapshotCompletionEXT           completion = { .type = XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_EXT };
        EXPECT_EQ(spatial.xrCreateSpatialDiscoverySnapshotCompleteEXT(context, &complete, &completion), XR_SUCCESS);
        EXPECT_EQ(completion.futureResult, XR_SUCCESS);
        return completion.snapshot;
    }

    XrSpatialSnapshotEXT update (XrSpatialContextEXT context, const XrSpatialEntityEXT* entities, u32 count)
    {
        const XrSpatialUpdateSnapshotCreateInfoEXT info     = { .type = XR_TYPE_SPATIAL_UPDATE_SNAPSHOT_CREATE_INFO_EXT, .entityCount = count, .entities = entities, .baseSpace = stage, .time = now };
        XrSpatialSnapshotEXT                       snapshot = XR_NULL_HANDLE;
        EXPECT_EQ(spatial.xrCreateSpatialUpdateSnapshotEXT(context, &info, &snapshot), XR_SUCCESS);
        return snapshot;
    }

    // Up to 8 entities with their bounds and markers; untouched data keeps 0xCD bytes
    struct Query
    {
        u32                             count;
        XrSpatialEntityIdEXT            ids[8];
        XrSpatialEntityTrackingStateEXT states[8];
        XrSpatialBounded2DDataEXT       bounds[8];
        XrSpatialMarkerDataEXT          markers[8];
    };

    XrResult query (XrSpatialSnapshotEXT snapshot, Query& out, const void* filter = nullptr)
    {
        memset(&out, 0xCD, sizeof(out));
        const XrSpatialComponentDataQueryConditionEXT condition   = { .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT, .next = filter, .componentTypeCount = 2, .componentTypes = marker_components };
        XrSpatialComponentMarkerListEXT               marker_list = { .type = XR_TYPE_SPATIAL_COMPONENT_MARKER_LIST_EXT, .markerCount = 8, .markers = out.markers };
        XrSpatialComponentBounded2DListEXT            bound_list  = { .type = XR_TYPE_SPATIAL_COMPONENT_BOUNDED_2D_LIST_EXT, .next = &marker_list, .boundCount = 8, .bounds = out.bounds };
        XrSpatialComponentDataQueryResultEXT          result      = { .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT, .next = &bound_list, .entityIdCapacityInput = 8, .entityIds = out.ids, .entityStateCapacityInput = 8, .entityStates = out.states };
        const XrResult                                status      = spatial.xrQuerySpatialComponentDataEXT(snapshot, &condition, &result);
        out.count = result.entityIdCountOutput;
        EXPECT_EQ(result.entityStateCountOutput, result.entityIdCountOutput);
        return status;
    }

    b8 discovery_recommended (XrSpatialContextEXT context)
    {
        b8 found = false;
        while (true)
        {
            XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            if (xr.xrPollEvent(instance, &event) != XR_SUCCESS)
            {
                return found;
            }
            if (event.type == XR_TYPE_EVENT_DATA_SPATIAL_DISCOVERY_RECOMMENDED_EXT)
            {
                found = found || ((XrEventDataSpatialDiscoveryRecommendedEXT*)&event)->spatialContext == context;
            }
        }
    }

    std::string text (XrSpatialSnapshotEXT snapshot, XrSpatialBufferIdEXT id)
    {
        const XrSpatialBufferGetInfoEXT info  = { .type = XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT, .bufferId = id };
        u32                             count = 0;
        EXPECT_EQ(spatial.xrGetSpatialBufferStringEXT(snapshot, &info, 0, &count, nullptr), XR_SUCCESS);
        char buffer[256] = {};
        EXPECT_LE(count, sizeof(buffer));
        EXPECT_EQ(spatial.xrGetSpatialBufferStringEXT(snapshot, &info, sizeof(buffer), &count, buffer), XR_SUCCESS);
        EXPECT_EQ(cstr_length(buffer) + 1, count);
        return buffer;
    }
};

} // namespace

TEST_F(OxrSpatialTest, extensions_need_their_dependencies)
{
    const char* const no_future[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME };
    EXPECT_EQ(create_instance(no_future, 2), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
    const char* const no_entity[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_EXT_FUTURE_EXTENSION_NAME, XR_EXT_SPATIAL_MARKER_TRACKING_EXTENSION_NAME };
    EXPECT_EQ(create_instance(no_entity, 3), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
}

TEST_F(OxrSpatialTest, qr_codes_are_the_only_capability)
{
    start();
    u32                    count           = 0;
    XrSpatialCapabilityEXT capabilities[4] = {};
    ASSERT_EQ(spatial.xrEnumerateSpatialCapabilitiesEXT(instance, system, 0, &count, nullptr), XR_SUCCESS);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(spatial.xrEnumerateSpatialCapabilitiesEXT(instance, system + 1, 4, &count, capabilities), XR_ERROR_SYSTEM_INVALID);
    ASSERT_EQ(spatial.xrEnumerateSpatialCapabilitiesEXT(instance, system, 4, &count, capabilities), XR_SUCCESS);
    EXPECT_EQ(capabilities[0], XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT);

    XrSpatialComponentTypeEXT            types[4]   = {};
    XrSpatialCapabilityComponentTypesEXT components = { .type = XR_TYPE_SPATIAL_CAPABILITY_COMPONENT_TYPES_EXT, .componentTypeCapacityInput = 1, .componentTypes = types };
    EXPECT_EQ(spatial.xrEnumerateSpatialCapabilityComponentTypesEXT(instance, system, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, &components), XR_ERROR_SIZE_INSUFFICIENT);
    components.componentTypeCapacityInput = 4;
    ASSERT_EQ(spatial.xrEnumerateSpatialCapabilityComponentTypesEXT(instance, system, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, &components), XR_SUCCESS);
    ASSERT_EQ(components.componentTypeCountOutput, 2u);
    EXPECT_EQ(types[0], XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT);
    EXPECT_EQ(types[1], XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT);
    EXPECT_EQ(spatial.xrEnumerateSpatialCapabilityComponentTypesEXT(instance, system, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_APRIL_TAG_EXT, &components), XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT);

    XrSpatialCapabilityFeatureEXT features[4] = {};
    ASSERT_EQ(spatial.xrEnumerateSpatialCapabilityFeaturesEXT(instance, system, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, 4, &count, features), XR_SUCCESS);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(features[0], XR_SPATIAL_CAPABILITY_FEATURE_MARKER_TRACKING_FIXED_SIZE_MARKERS_EXT);
    EXPECT_EQ(features[1], XR_SPATIAL_CAPABILITY_FEATURE_MARKER_TRACKING_STATIC_MARKERS_EXT);
    EXPECT_EQ(spatial.xrEnumerateSpatialCapabilityFeaturesEXT(instance, system, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_ARUCO_MARKER_EXT, 4, &count, features), XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT);
}

TEST_F(OxrSpatialTest, without_marker_tracking_nothing_is_supported)
{
    start(false);
    u32 count = 7;
    ASSERT_EQ(spatial.xrEnumerateSpatialCapabilitiesEXT(instance, system, 0, &count, nullptr), XR_SUCCESS);
    EXPECT_EQ(count, 0u);
    XrSpatialContextEXT context = XR_NULL_HANDLE;
    EXPECT_EQ(create_context(context), XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT);
}

TEST_F(OxrSpatialTest, contexts_are_validated)
{
    start();
    const XrSpatialCapabilityConfigurationQrCodeEXT qr      = { .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_QR_CODE_EXT, .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, .enabledComponentCount = 2, .enabledComponents = marker_components };
    const XrSpatialCapabilityConfigurationBaseHeaderEXT* twice[] = { (const XrSpatialCapabilityConfigurationBaseHeaderEXT*)&qr, (const XrSpatialCapabilityConfigurationBaseHeaderEXT*)&qr };
    XrSpatialContextCreateInfoEXT info   = { .type = XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT, .capabilityConfigCount = 0, .capabilityConfigs = twice };
    XrFutureEXT                   future = XR_NULL_FUTURE_EXT;
    EXPECT_EQ(spatial.xrCreateSpatialContextAsyncEXT(session, &info, &future), XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT);
    info.capabilityConfigCount = 2;
    EXPECT_EQ(spatial.xrCreateSpatialContextAsyncEXT(session, &info, &future), XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT) << "the same capability twice";
    EXPECT_EQ(spatial.xrCreateSpatialContextAsyncEXT(XR_NULL_HANDLE, &info, &future), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(spatial.xrCreateSpatialContextAsyncEXT(session, nullptr, &future), XR_ERROR_VALIDATION_FAILURE);

    XrSpatialContextEXT context = XR_NULL_HANDLE;
    EXPECT_EQ(create_context(context, marker_components, 0), XR_ERROR_SPATIAL_CAPABILITY_CONFIGURATION_INVALID_EXT);
    const XrSpatialComponentTypeEXT mesh = XR_SPATIAL_COMPONENT_TYPE_MESH_3D_EXT;
    EXPECT_EQ(create_context(context, &mesh, 1), XR_ERROR_SPATIAL_COMPONENT_UNSUPPORTED_FOR_CAPABILITY_EXT);
    const XrSpatialMarkerSizeEXT bad_size = { .type = XR_TYPE_SPATIAL_MARKER_SIZE_EXT, .markerSideLength = -1.0f };
    EXPECT_EQ(create_context(context, marker_components, 2, &bad_size), XR_ERROR_VALIDATION_FAILURE);
    const XrSpatialMarkerStaticOptimizationEXT still = { .type = XR_TYPE_SPATIAL_MARKER_STATIC_OPTIMIZATION_EXT, .optimizeForStaticMarker = XR_TRUE };
    EXPECT_EQ(create_context(context, marker_components, 2, &still), XR_SUCCESS);

    XrSpatialCapabilityConfigurationArucoMarkerEXT aruco = { .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_ARUCO_MARKER_EXT, .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_ARUCO_MARKER_EXT, .enabledComponentCount = 2, .enabledComponents = marker_components, .arUcoDict = XR_SPATIAL_MARKER_ARUCO_DICT_5X5_50_EXT };
    const XrSpatialCapabilityConfigurationBaseHeaderEXT* aruco_config[] = { (const XrSpatialCapabilityConfigurationBaseHeaderEXT*)&aruco };
    info.capabilityConfigCount = 1;
    info.capabilityConfigs     = aruco_config;
    EXPECT_EQ(spatial.xrCreateSpatialContextAsyncEXT(session, &info, &future), XR_ERROR_SPATIAL_CAPABILITY_UNSUPPORTED_EXT);
    aruco.capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT;
    EXPECT_EQ(spatial.xrCreateSpatialContextAsyncEXT(session, &info, &future), XR_ERROR_VALIDATION_FAILURE) << "an ArUco struct for the QR capability";

    // A future completes once
    info.capabilityConfigs = twice;
    ASSERT_EQ(spatial.xrCreateSpatialContextAsyncEXT(session, &info, &future), XR_SUCCESS);
    XrCreateSpatialContextCompletionEXT completion = { .type = XR_TYPE_CREATE_SPATIAL_CONTEXT_COMPLETION_EXT };
    ASSERT_EQ(spatial.xrCreateSpatialContextCompleteEXT(session, future, &completion), XR_SUCCESS);
    EXPECT_EQ(spatial.xrCreateSpatialContextCompleteEXT(session, future, &completion), XR_ERROR_FUTURE_INVALID_EXT);
    EXPECT_EQ(spatial.xrDestroySpatialContextEXT(completion.spatialContext), XR_SUCCESS);
    EXPECT_EQ(spatial.xrDestroySpatialContextEXT(completion.spatialContext), XR_ERROR_HANDLE_INVALID);

    // A cancelled one never completes
    ASSERT_EQ(spatial.xrCreateSpatialContextAsyncEXT(session, &info, &future), XR_SUCCESS);
    const XrFutureCancelInfoEXT cancel = { .type = XR_TYPE_FUTURE_CANCEL_INFO_EXT, .future = future };
    ASSERT_EQ(spatial.xrCancelFutureEXT(instance, &cancel), XR_SUCCESS);
    EXPECT_EQ(spatial.xrCreateSpatialContextCompleteEXT(session, future, &completion), XR_ERROR_FUTURE_INVALID_EXT);
}

TEST_F(OxrSpatialTest, qr_codes_in_view_are_discovered_with_their_text)
{
    start();
    const u32           id      = add_qr({ 0.1f, 1.5f, -1.0f }, "OpenXR");
    XrSpatialContextEXT context = XR_NULL_HANDLE;
    ASSERT_EQ(create_context(context), XR_SUCCESS);
    EXPECT_FALSE(discovery_recommended(context)) << "not before a frame has looked";
    look({ 0.0f, 1.6f, 0.0f }, 0.0f);
    EXPECT_TRUE(discovery_recommended(context));
    look({ 0.0f, 1.6f, 0.0f }, 0.0f);
    EXPECT_FALSE(discovery_recommended(context)) << "once until the app discovers";

    const XrSpatialSnapshotEXT snapshot = discover(context);
    Query                      result   = {};
    ASSERT_EQ(query(snapshot, result), XR_SUCCESS);
    ASSERT_EQ(result.count, 1u);
    EXPECT_NE(result.ids[0], XR_NULL_SPATIAL_ENTITY_ID_EXT);
    EXPECT_EQ(result.states[0], XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT);
    EXPECT_NEAR(result.bounds[0].center.position.x, 0.1f, 1e-5f);
    EXPECT_NEAR(result.bounds[0].center.position.y, 1.5f, 1e-5f);
    EXPECT_NEAR(result.bounds[0].center.position.z, -1.0f, 1e-5f);
    EXPECT_NEAR(result.bounds[0].center.orientation.w, 1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(result.bounds[0].extents.width, 0.15f);
    EXPECT_FLOAT_EQ(result.bounds[0].extents.height, 0.15f);
    EXPECT_EQ(result.markers[0].capability, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT);
    EXPECT_EQ(result.markers[0].markerId, 0u);
    ASSERT_EQ(result.markers[0].data.bufferType, XR_SPATIAL_BUFFER_TYPE_STRING_EXT);
    EXPECT_EQ(text(snapshot, result.markers[0].data.bufferId), "OpenXR");

    const XrSpatialBufferGetInfoEXT info  = { .type = XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT, .bufferId = result.markers[0].data.bufferId };
    u32                             count = 0;
    char                            small[3];
    EXPECT_EQ(spatial.xrGetSpatialBufferStringEXT(snapshot, &info, 3, &count, small), XR_ERROR_SIZE_INSUFFICIENT);
    EXPECT_EQ(count, 7u);
    u8 bytes[16];
    EXPECT_EQ(spatial.xrGetSpatialBufferUint8EXT(snapshot, &info, 16, &count, bytes), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(spatial.xrGetSpatialBufferFloatEXT(snapshot, &info, 0, &count, nullptr), XR_ERROR_VALIDATION_FAILURE);
    const XrSpatialBufferGetInfoEXT none = { .type = XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT, .bufferId = XR_NULL_SPATIAL_BUFFER_ID_EXT };
    EXPECT_EQ(spatial.xrGetSpatialBufferStringEXT(snapshot, &none, 0, &count, nullptr), XR_ERROR_SPATIAL_BUFFER_ID_INVALID_EXT);

    // The same text keeps its buffer; a new text gets a new one
    const XrSpatialSnapshotEXT again        = discover(context);
    Query                      again_result = {};
    ASSERT_EQ(query(again, again_result), XR_SUCCESS);
    ASSERT_EQ(again_result.count, 1u);
    EXPECT_EQ(again_result.ids[0], result.ids[0]);
    EXPECT_EQ(again_result.markers[0].data.bufferId, result.markers[0].data.bufferId);
    SIM_Prop changed = {};
    cstr_copy(changed.text, sizeof(changed.text), "changed");
    const SIM_Prop* props = nullptr;
    const u32       props_count = sim_props(oxr_get_sim(), props);
    ASSERT_EQ(props_count, 1u);
    changed      = props[0];
    cstr_copy(changed.text, sizeof(changed.text), "changed");
    ASSERT_TRUE(sim_update_prop(oxr_get_sim(), id, changed));
    const XrSpatialSnapshotEXT third        = discover(context);
    Query                      third_result = {};
    ASSERT_EQ(query(third, third_result), XR_SUCCESS);
    EXPECT_NE(third_result.markers[0].data.bufferId, result.markers[0].data.bufferId);
    EXPECT_EQ(text(third, third_result.markers[0].data.bufferId), "changed");
    EXPECT_EQ(text(snapshot, result.markers[0].data.bufferId), "OpenXR") << "snapshots never change";
    EXPECT_EQ(spatial.xrGetSpatialBufferStringEXT(third, &info, 0, &count, nullptr), XR_ERROR_SPATIAL_BUFFER_ID_INVALID_EXT) << "not in this snapshot";

    EXPECT_EQ(spatial.xrDestroySpatialSnapshotEXT(snapshot), XR_SUCCESS);
    EXPECT_EQ(spatial.xrDestroySpatialSnapshotEXT(snapshot), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(query(snapshot, result), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrSpatialTest, codes_pause_out_of_view_and_stop_once_removed)
{
    start();
    const u32           id      = add_qr({ 0.0f, 1.6f, -1.0f }, "here");
    XrSpatialContextEXT context = XR_NULL_HANDLE;
    ASSERT_EQ(create_context(context), XR_SUCCESS);
    Query result = {};
    ASSERT_EQ(query(discover(context), result), XR_SUCCESS);
    ASSERT_EQ(result.count, 1u);
    const XrSpatialEntityFromIdCreateInfoEXT from_id = { .type = XR_TYPE_SPATIAL_ENTITY_FROM_ID_CREATE_INFO_EXT, .entityId = result.ids[0] };
    XrSpatialEntityEXT                       entity  = XR_NULL_HANDLE;
    ASSERT_EQ(spatial.xrCreateSpatialEntityFromIdEXT(context, &from_id, &entity), XR_SUCCESS);

    look({ 0.0f, 1.6f, 0.0f }, zm::PI);
    ASSERT_EQ(query(update(context, &entity, 1), result), XR_SUCCESS);
    ASSERT_EQ(result.count, 1u);
    EXPECT_EQ(result.ids[0], from_id.entityId);
    EXPECT_EQ(result.states[0], XR_SPATIAL_ENTITY_TRACKING_STATE_PAUSED_EXT) << "behind the head";
    u8 untouched[sizeof(XrSpatialBounded2DDataEXT)];
    memset(untouched, 0xCD, sizeof(untouched));
    EXPECT_EQ(memcmp(&result.bounds[0], untouched, sizeof(untouched)), 0) << "data of entities not tracked stays";
    EXPECT_EQ(memcmp(&result.markers[0], untouched, sizeof(XrSpatialMarkerDataEXT)), 0);

    look({ 0.0f, 1.6f, -5.0f }, zm::PI);
    ASSERT_EQ(query(update(context, &entity, 1), result), XR_SUCCESS);
    EXPECT_EQ(result.states[0], XR_SPATIAL_ENTITY_TRACKING_STATE_PAUSED_EXT) << "its back faces the head";

    look({ 0.0f, 1.6f, 4.0f }, 0.0f);
    ASSERT_EQ(query(update(context, &entity, 1), result), XR_SUCCESS);
    EXPECT_EQ(result.states[0], XR_SPATIAL_ENTITY_TRACKING_STATE_PAUSED_EXT) << "5 m is too far for 15 cm";

    look({ 0.0f, 1.6f, 0.0f }, 0.0f);
    ASSERT_EQ(query(update(context, &entity, 1), result), XR_SUCCESS);
    EXPECT_EQ(result.states[0], XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT);

    // Spec: a stopped entity never changes state again, even if its prop turns back into a code
    const SIM_Prop* props = nullptr;
    ASSERT_EQ(sim_props(oxr_get_sim(), props), 1u);
    SIM_Prop prop = props[0];
    prop.kind     = SIM_PropKind::Keyboard;
    ASSERT_TRUE(sim_update_prop(oxr_get_sim(), id, prop));
    ASSERT_EQ(query(update(context, &entity, 1), result), XR_SUCCESS);
    EXPECT_EQ(result.states[0], XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT);
    prop.kind = SIM_PropKind::QrCode;
    ASSERT_TRUE(sim_update_prop(oxr_get_sim(), id, prop));
    ASSERT_EQ(query(update(context, &entity, 1), result), XR_SUCCESS);
    EXPECT_EQ(result.states[0], XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT);
    ASSERT_TRUE(sim_remove_prop(oxr_get_sim(), id));
    ASSERT_EQ(query(update(context, &entity, 1), result), XR_SUCCESS);
    EXPECT_EQ(result.states[0], XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT);
    const XrSpatialFilterTrackingStateEXT tracking = { .type = XR_TYPE_SPATIAL_FILTER_TRACKING_STATE_EXT, .trackingState = XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT };
    ASSERT_EQ(query(discover(context), result, &tracking), XR_SUCCESS);
    EXPECT_EQ(result.count, 0u);
    ASSERT_EQ(query(discover(context, nullptr, 0, &tracking), result), XR_SUCCESS);
    EXPECT_EQ(result.count, 0u);
    EXPECT_EQ(spatial.xrDestroySpatialEntityEXT(entity), XR_SUCCESS);
    EXPECT_EQ(spatial.xrDestroySpatialEntityEXT(entity), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrSpatialTest, moving_codes_are_followed_without_new_recommendations)
{
    start();
    SIM_Prop prop = { .kind = SIM_PropKind::QrCode, .motion = SIM_PropMotion::Linear, .pose = { { -0.2f, 1.6f, -1.0f }, zm::QUAT_IDENTITY }, .target = { 0.2f, 1.6f, -1.0f }, .speed = 0.1f, .size = 0.1f };
    cstr_copy(prop.text, sizeof(prop.text), "moving");
    ASSERT_NE(sim_add_prop(oxr_get_sim(), prop), 0u);
    const XrSpatialMarkerSizeEXT size    = { .type = XR_TYPE_SPATIAL_MARKER_SIZE_EXT, .markerSideLength = 0.12f };
    XrSpatialContextEXT          context = XR_NULL_HANDLE;
    ASSERT_EQ(create_context(context, marker_components, 2, &size), XR_SUCCESS);
    look({ 0.0f, 1.6f, 0.0f }, 0.0f);
    ASSERT_TRUE(discovery_recommended(context));
    Query first = {};
    ASSERT_EQ(query(discover(context), first), XR_SUCCESS);
    ASSERT_EQ(first.count, 1u);
    EXPECT_FLOAT_EQ(first.bounds[0].extents.width, 0.12f) << "fixed size markers";

    const XrSpatialEntityFromIdCreateInfoEXT from_id = { .type = XR_TYPE_SPATIAL_ENTITY_FROM_ID_CREATE_INFO_EXT, .entityId = first.ids[0] };
    XrSpatialEntityEXT                       entity  = XR_NULL_HANDLE;
    ASSERT_EQ(spatial.xrCreateSpatialEntityFromIdEXT(context, &from_id, &entity), XR_SUCCESS);
    for (u32 i = 0; i < 30; i++)
    {
        look({ 0.0f, 1.6f, 0.0f }, 0.0f);
    }
    EXPECT_FALSE(discovery_recommended(context)) << "moving is no reason to discover";
    Query later = {};
    const XrSpatialEntityEXT twice[] = { entity, entity };
    ASSERT_EQ(query(update(context, twice, 2), later), XR_SUCCESS);
    ASSERT_EQ(later.count, 1u);
    EXPECT_GT(later.bounds[0].center.position.x, first.bounds[0].center.position.x + 0.01f);
}

TEST_F(OxrSpatialTest, queries_follow_the_spec)
{
    start();
    add_qr({ 0.0f, 1.6f, -1.0f }, "a");
    add_qr({ 0.3f, 1.6f, -1.0f }, "b");
    const XrSpatialComponentTypeEXT bounded = XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT;
    const XrSpatialComponentTypeEXT marker  = XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT;
    XrSpatialContextEXT             context = XR_NULL_HANDLE;
    ASSERT_EQ(create_context(context, &bounded, 1), XR_SUCCESS);

    const XrSpatialDiscoverySnapshotCreateInfoEXT ask_marker = { .type = XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT, .componentTypeCount = 1, .componentTypes = &marker };
    XrFutureEXT                                   future     = XR_NULL_FUTURE_EXT;
    EXPECT_EQ(spatial.xrCreateSpatialDiscoverySnapshotAsyncEXT(context, &ask_marker, &future), XR_ERROR_SPATIAL_COMPONENT_NOT_ENABLED_EXT);

    const XrSpatialSnapshotEXT snapshot = discover(context);
    XrSpatialEntityIdEXT       ids[2]   = {};
    XrSpatialBounded2DDataEXT  bounds[2] = {};
    XrSpatialMarkerDataEXT     markers[2] = {};
    XrSpatialComponentBounded2DListEXT   bound_list  = { .type = XR_TYPE_SPATIAL_COMPONENT_BOUNDED_2D_LIST_EXT, .boundCount = 1, .bounds = bounds };
    XrSpatialComponentMarkerListEXT      marker_list = { .type = XR_TYPE_SPATIAL_COMPONENT_MARKER_LIST_EXT, .markerCount = 2, .markers = markers };
    XrSpatialComponentDataQueryResultEXT result      = { .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT };
    XrSpatialComponentDataQueryConditionEXT condition = { .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT, .componentTypeCount = 1, .componentTypes = &bounded };
    ASSERT_EQ(spatial.xrQuerySpatialComponentDataEXT(snapshot, &condition, &result), XR_SUCCESS);
    EXPECT_EQ(result.entityIdCountOutput, 2u);
    EXPECT_EQ(result.entityStateCountOutput, 2u);

    result.entityIdCapacityInput = 1;
    result.entityIds             = ids;
    EXPECT_EQ(spatial.xrQuerySpatialComponentDataEXT(snapshot, &condition, &result), XR_ERROR_SIZE_INSUFFICIENT);
    result.entityIdCapacityInput = 2;
    ASSERT_EQ(spatial.xrQuerySpatialComponentDataEXT(snapshot, &condition, &result), XR_SUCCESS) << "no lists is fine";
    EXPECT_LT(ids[0], ids[1]);
    result.next = &bound_list;
    EXPECT_EQ(spatial.xrQuerySpatialComponentDataEXT(snapshot, &condition, &result), XR_ERROR_SIZE_INSUFFICIENT);
    bound_list.boundCount = 2;
    ASSERT_EQ(spatial.xrQuerySpatialComponentDataEXT(snapshot, &condition, &result), XR_SUCCESS);
    EXPECT_NEAR(bounds[1].center.position.x, 0.3f, 1e-5f);
    bound_list.next = &marker_list;
    EXPECT_EQ(spatial.xrQuerySpatialComponentDataEXT(snapshot, &condition, &result), XR_ERROR_VALIDATION_FAILURE) << "a marker list without the marker component";

    // The marker component is not enabled, so no entity has it
    condition.componentTypes = &marker;
    bound_list.next          = nullptr;
    result.next              = nullptr;
    ASSERT_EQ(spatial.xrQuerySpatialComponentDataEXT(snapshot, &condition, &result), XR_SUCCESS);
    EXPECT_EQ(result.entityIdCountOutput, 0u);

    // Entity ids are unique in the session, across contexts
    XrSpatialContextEXT other = XR_NULL_HANDLE;
    ASSERT_EQ(create_context(other), XR_SUCCESS);
    Query other_result = {};
    ASSERT_EQ(query(discover(other), other_result), XR_SUCCESS);
    ASSERT_EQ(other_result.count, 2u);
    EXPECT_GT(other_result.ids[0], ids[1]);
    const XrSpatialEntityFromIdCreateInfoEXT foreign = { .type = XR_TYPE_SPATIAL_ENTITY_FROM_ID_CREATE_INFO_EXT, .entityId = other_result.ids[0] };
    XrSpatialEntityEXT                       entity  = XR_NULL_HANDLE;
    EXPECT_EQ(spatial.xrCreateSpatialEntityFromIdEXT(context, &foreign, &entity), XR_ERROR_SPATIAL_ENTITY_ID_INVALID_EXT);
    const XrSpatialEntityFromIdCreateInfoEXT null_id = { .type = XR_TYPE_SPATIAL_ENTITY_FROM_ID_CREATE_INFO_EXT, .entityId = XR_NULL_SPATIAL_ENTITY_ID_EXT };
    EXPECT_EQ(spatial.xrCreateSpatialEntityFromIdEXT(context, &null_id, &entity), XR_ERROR_SPATIAL_ENTITY_ID_INVALID_EXT);
    ASSERT_EQ(spatial.xrCreateSpatialEntityFromIdEXT(other, &foreign, &entity), XR_SUCCESS);
    const XrSpatialUpdateSnapshotCreateInfoEXT wrong_context = { .type = XR_TYPE_SPATIAL_UPDATE_SNAPSHOT_CREATE_INFO_EXT, .entityCount = 1, .entities = &entity, .baseSpace = stage, .time = now };
    XrSpatialSnapshotEXT                       updated       = XR_NULL_HANDLE;
    EXPECT_EQ(spatial.xrCreateSpatialUpdateSnapshotEXT(context, &wrong_context, &updated), XR_ERROR_VALIDATION_FAILURE);
    XrSpatialUpdateSnapshotCreateInfoEXT bad_time = wrong_context;
    bad_time.time                                  = 0;
    EXPECT_EQ(spatial.xrCreateSpatialUpdateSnapshotEXT(other, &bad_time, &updated), XR_ERROR_TIME_INVALID);

    // Destroying a context destroys what came from it
    const XrSpatialSnapshotEXT other_snapshot = discover(other);
    ASSERT_EQ(spatial.xrDestroySpatialContextEXT(other), XR_SUCCESS);
    EXPECT_EQ(spatial.xrDestroySpatialEntityEXT(entity), XR_ERROR_HANDLE_INVALID);
    EXPECT_EQ(spatial.xrDestroySpatialSnapshotEXT(other_snapshot), XR_ERROR_HANDLE_INVALID);
}

TEST_F(OxrSpatialTest, recommendations_of_destroyed_contexts_are_dropped)
{
    start();
    add_qr({ 0.0f, 1.6f, -1.0f }, "dropped");
    XrSpatialContextEXT context = XR_NULL_HANDLE;
    ASSERT_EQ(create_context(context), XR_SUCCESS);
    look({ 0.0f, 1.6f, 0.0f }, 0.0f);
    ASSERT_EQ(spatial.xrDestroySpatialContextEXT(context), XR_SUCCESS);
    EXPECT_FALSE(discovery_recommended(context));
}
//
// XR_EXT_render_model, XR_EXT_interaction_render_model
//

namespace
{

#define RENDER_MODEL_FUNCTIONS(X)                    \
    X(xrCreateRenderModelEXT)                        \
    X(xrDestroyRenderModelEXT)                       \
    X(xrGetRenderModelPropertiesEXT)                 \
    X(xrCreateRenderModelSpaceEXT)                   \
    X(xrCreateRenderModelAssetEXT)                   \
    X(xrDestroyRenderModelAssetEXT)                  \
    X(xrGetRenderModelAssetDataEXT)                  \
    X(xrGetRenderModelAssetPropertiesEXT)            \
    X(xrGetRenderModelStateEXT)                      \
    X(xrEnumerateInteractionRenderModelIdsEXT)       \
    X(xrEnumerateRenderModelSubactionPathsEXT)       \
    X(xrGetRenderModelPoseTopLevelUserPathEXT)

struct RenderModelXr
{
#define MEMBER(fn) PFN_##fn fn = nullptr;
    RENDER_MODEL_FUNCTIONS(MEMBER)
#undef MEMBER
};

struct OxrExtRenderModelTest : OxrActionTest
{
    RenderModelXr rm    = {};
    XrSpace       stage = XR_NULL_HANDLE;

    OxrExtRenderModelTest ()
    {
        more_extensions[more_extensions_count++] = XR_EXT_RENDER_MODEL_EXTENSION_NAME;
        more_extensions[more_extensions_count++] = XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME;
    }

    void SetUp () override
    {
        OxrActionTest::SetUp();
#define LOAD(fn) ASSERT_EQ(gipa(instance, #fn, (PFN_xrVoidFunction*)&rm.fn), XR_SUCCESS) << #fn;
        RENDER_MODEL_FUNCTIONS(LOAD)
#undef LOAD
        suggest_touch_bindings();
        attach();
        begin_session();
        stage = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    }

    u32 ids (XrRenderModelIdEXT (&out)[4])
    {
        u32 count = 0;
        EXPECT_EQ(rm.xrEnumerateInteractionRenderModelIdsEXT(session, nullptr, 4, &count, out), XR_SUCCESS);
        return count;
    }

    b8 models_changed ()
    {
        b8 found = false;
        while (true)
        {
            XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            if (xr.xrPollEvent(instance, &event) != XR_SUCCESS)
            {
                return found;
            }
            found = found || event.type == XR_TYPE_EVENT_DATA_INTERACTION_RENDER_MODELS_CHANGED_EXT;
        }
    }

    XrRenderModelEXT create (XrRenderModelIdEXT id)
    {
        const XrRenderModelCreateInfoEXT info  = { .type = XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT, .renderModelId = id };
        XrRenderModelEXT                 model = XR_NULL_HANDLE;
        EXPECT_EQ(rm.xrCreateRenderModelEXT(session, &info, &model), XR_SUCCESS);
        return model;
    }

    XrRenderModelPropertiesEXT properties (XrRenderModelEXT model)
    {
        XrRenderModelPropertiesEXT props = { .type = XR_TYPE_RENDER_MODEL_PROPERTIES_EXT };
        EXPECT_EQ(rm.xrGetRenderModelPropertiesEXT(model, nullptr, &props), XR_SUCCESS);
        return props;
    }

    // The model of the right hand, which holds a Touch controller from the start
    XrRenderModelEXT right_model ()
    {
        EXPECT_EQ(sync(), XR_SUCCESS);
        XrRenderModelIdEXT list[4] = {};
        const u32          count   = ids(list);
        const XrPath       right   = hands[1];
        for (u32 i = 0; i < count; i++)
        {
            const XrRenderModelEXT                                   model = create(list[i]);
            const XrInteractionRenderModelTopLevelUserPathGetInfoEXT info  = { .type = XR_TYPE_INTERACTION_RENDER_MODEL_TOP_LEVEL_USER_PATH_GET_INFO_EXT, .topLevelUserPathCount = 1, .topLevelUserPaths = &right };
            XrPath                                                   path  = XR_NULL_PATH;
            EXPECT_EQ(rm.xrGetRenderModelPoseTopLevelUserPathEXT(model, &info, &path), XR_SUCCESS);
            if (path == right)
            {
                return model;
            }
            rm.xrDestroyRenderModelEXT(model);
        }
        ADD_FAILURE() << "no model in the right hand";
        return XR_NULL_HANDLE;
    }
};

} // namespace

TEST_F(OxrActionTest, interaction_render_models_need_render_models)
{
    xr.xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;
    const char* const extensions[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME };
    EXPECT_EQ(create_instance(extensions, 2), XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
}

TEST_F(OxrExtRenderModelTest, models_appear_at_the_first_sync_with_an_event)
{
    XrRenderModelIdEXT list[4] = {};
    EXPECT_EQ(ids(list), 0u) << "none before the first xrSyncActions";
    EXPECT_FALSE(models_changed());
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(models_changed());
    ASSERT_EQ(ids(list), 2u) << "both hands hold a Touch controller";
    EXPECT_NE(list[0], list[1]);
    EXPECT_NE(list[0], XR_NULL_RENDER_MODEL_ID_EXT);
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_FALSE(models_changed()) << "nothing changed";
    XrRenderModelIdEXT again[4] = {};
    ASSERT_EQ(ids(again), 2u);
    EXPECT_EQ(again[0], list[0]);

    u32 count = 0;
    EXPECT_EQ(rm.xrEnumerateInteractionRenderModelIdsEXT(session, nullptr, 1, &count, list), XR_ERROR_SIZE_INSUFFICIENT);
    EXPECT_EQ(rm.xrEnumerateInteractionRenderModelIdsEXT(session, nullptr, 0, nullptr, nullptr), XR_ERROR_VALIDATION_FAILURE);
    const XrRenderModelCreateInfoEXT bad   = { .type = XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT, .renderModelId = 12345 };
    XrRenderModelEXT                 model = XR_NULL_HANDLE;
    EXPECT_EQ(rm.xrCreateRenderModelEXT(session, &bad, &model), XR_ERROR_RENDER_MODEL_ID_INVALID_EXT);
    const XrRenderModelCreateInfoEXT null_id = { .type = XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT, .renderModelId = XR_NULL_RENDER_MODEL_ID_EXT };
    EXPECT_EQ(rm.xrCreateRenderModelEXT(session, &null_id, &model), XR_ERROR_RENDER_MODEL_ID_INVALID_EXT);
    const XrRenderModelCreateInfoEXT no_list = { .type = XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT, .renderModelId = list[0], .gltfExtensionCount = 1 };
    EXPECT_EQ(rm.xrCreateRenderModelEXT(session, &no_list, &model), XR_ERROR_VALIDATION_FAILURE);
}

TEST_F(OxrExtRenderModelTest, assets_are_glb_with_the_animatable_nodes)
{
    const XrRenderModelEXT           model = right_model();
    const XrRenderModelPropertiesEXT props = properties(model);
    EXPECT_EQ(props.animatableNodeCount, 2u);
    b8 nonzero = false;
    for (u8 value : props.cacheId.data)
    {
        nonzero = nonzero || value != 0;
    }
    EXPECT_TRUE(nonzero);
    EXPECT_EQ(memcmp(properties(model).cacheId.data, props.cacheId.data, 16), 0) << "immutable";

    const XrRenderModelAssetCreateInfoEXT unknown = { .type = XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT, .cacheId = { { 1, 2, 3 } } };
    XrRenderModelAssetEXT                 asset   = XR_NULL_HANDLE;
    EXPECT_EQ(rm.xrCreateRenderModelAssetEXT(session, &unknown, &asset), XR_ERROR_RENDER_MODEL_ASSET_UNAVAILABLE_EXT) << "never returned in this session";
    const XrRenderModelAssetCreateInfoEXT info = { .type = XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT, .cacheId = props.cacheId };
    ASSERT_EQ(rm.xrCreateRenderModelAssetEXT(session, &info, &asset), XR_SUCCESS);

    XrRenderModelAssetDataEXT data = { .type = XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT };
    ASSERT_EQ(rm.xrGetRenderModelAssetDataEXT(asset, nullptr, &data), XR_SUCCESS);
    ASSERT_GT(data.bufferCountOutput, 20u);
    std::vector<u8> glb(data.bufferCountOutput);
    data.bufferCapacityInput = 4;
    data.buffer              = glb.data();
    EXPECT_EQ(rm.xrGetRenderModelAssetDataEXT(asset, nullptr, &data), XR_ERROR_SIZE_INSUFFICIENT);
    data.bufferCapacityInput = (u32)glb.size();
    ASSERT_EQ(rm.xrGetRenderModelAssetDataEXT(asset, nullptr, &data), XR_SUCCESS);
    EXPECT_EQ(memcmp(glb.data(), "glTF", 4), 0);
    const std::string json((const char*)glb.data() + 20, glb.size() - 20);
    EXPECT_NE(json.find("\"name\":\"trigger\""), std::string::npos);
    EXPECT_NE(json.find("\"name\":\"buttons\""), std::string::npos);
    EXPECT_EQ(json.find("\"animations\""), std::string::npos) << "node states animate, not glTF animations";

    XrRenderModelAssetNodePropertiesEXT nodes[3] = {};
    XrRenderModelAssetPropertiesEXT     asset_props = { .type = XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT, .nodePropertyCount = 3, .nodeProperties = nodes };
    EXPECT_EQ(rm.xrGetRenderModelAssetPropertiesEXT(asset, nullptr, &asset_props), XR_ERROR_VALIDATION_FAILURE) << "the count must be the node count";
    asset_props.nodePropertyCount = 2;
    ASSERT_EQ(rm.xrGetRenderModelAssetPropertiesEXT(asset, nullptr, &asset_props), XR_SUCCESS);
    EXPECT_STREQ(nodes[0].uniqueName, "trigger");
    EXPECT_STREQ(nodes[1].uniqueName, "buttons");
    EXPECT_EQ(rm.xrDestroyRenderModelAssetEXT(asset), XR_SUCCESS);
    EXPECT_EQ(rm.xrDestroyRenderModelAssetEXT(asset), XR_ERROR_HANDLE_INVALID);

    // The same hardware gives the same bytes under the same UUID
    XrRenderModelAssetEXT again = XR_NULL_HANDLE;
    ASSERT_EQ(rm.xrCreateRenderModelAssetEXT(session, &info, &again), XR_SUCCESS);
    std::vector<u8> glb2(glb.size());
    XrRenderModelAssetDataEXT data2 = { .type = XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT, .bufferCapacityInput = (u32)glb2.size(), .buffer = glb2.data() };
    ASSERT_EQ(rm.xrGetRenderModelAssetDataEXT(again, nullptr, &data2), XR_SUCCESS);
    EXPECT_EQ(glb, glb2);
}

TEST_F(OxrExtRenderModelTest, node_states_follow_the_inputs)
{
    const XrRenderModelEXT          model    = right_model();
    XrRenderModelNodeStateEXT       nodes[2] = {};
    XrRenderModelStateEXT           state    = { .type = XR_TYPE_RENDER_MODEL_STATE_EXT, .nodeStateCount = 2, .nodeStates = nodes };
    const XrTime                    now      = frame().predictedDisplayTime;
    XrRenderModelStateGetInfoEXT    info     = { .type = XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT, .displayTime = now };
    ASSERT_EQ(rm.xrGetRenderModelStateEXT(model, &info, &state), XR_SUCCESS);
    EXPECT_TRUE(nodes[0].isVisible);
    EXPECT_FLOAT_EQ(nodes[0].nodePose.position.y, 0.0f) << "trigger at rest";
    EXPECT_FLOAT_EQ(nodes[0].nodePose.orientation.w, 1.0f);

    sim_set_input(oxr_get_sim(), SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 1.0f, 0.0f } });
    sim_set_input(oxr_get_sim(), SIM_Device::RightHand, SIM_Input::AClick, { .value = { 1.0f, 0.0f } });
    ASSERT_EQ(rm.xrGetRenderModelStateEXT(model, &info, &state), XR_SUCCESS);
    EXPECT_GT(zm::length(Vec3{ nodes[0].nodePose.position.x, nodes[0].nodePose.position.y, nodes[0].nodePose.position.z }), 0.001f) << "trigger pulled";
    EXPECT_GT(zm::length(Vec3{ nodes[1].nodePose.position.x, nodes[1].nodePose.position.y, nodes[1].nodePose.position.z }), 0.001f) << "a button pressed";

    state.nodeStateCount = 1;
    EXPECT_EQ(rm.xrGetRenderModelStateEXT(model, &info, &state), XR_ERROR_VALIDATION_FAILURE);
    state.nodeStateCount = 2;
    info.displayTime     = 0;
    EXPECT_EQ(rm.xrGetRenderModelStateEXT(model, &info, &state), XR_ERROR_TIME_INVALID);
}

TEST_F(OxrExtRenderModelTest, spaces_follow_the_grip_until_the_model_is_gone)
{
    const XrRenderModelEXT                model = right_model();
    const XrRenderModelSpaceCreateInfoEXT info  = { .type = XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT, .renderModel = model };
    XrSpace                               space = XR_NULL_HANDLE;
    ASSERT_EQ(rm.xrCreateRenderModelSpaceEXT(session, &info, &space), XR_SUCCESS);
    XrSpace                     grip_space = XR_NULL_HANDLE;
    const XrActionSpaceCreateInfo grip_info = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = grip, .subactionPath = hands[1], .poseInActionSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    ASSERT_EQ(xr.xrCreateActionSpace(session, &grip_info, &grip_space), XR_SUCCESS);

    const XrTime    now      = frame().predictedDisplayTime;
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(space, grip_space, now, &location), XR_SUCCESS);
    EXPECT_TRUE(location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
    EXPECT_NEAR(location.pose.position.x, 0.0f, 1e-5f) << "the model is in grip space";
    EXPECT_NEAR(location.pose.position.z, 0.0f, 1e-5f);
    EXPECT_NEAR(location.pose.orientation.w, 1.0f, 1e-5f);

    ASSERT_EQ(rm.xrDestroyRenderModelEXT(model), XR_SUCCESS);
    ASSERT_EQ(xr.xrLocateSpace(space, stage, now, &location), XR_SUCCESS);
    EXPECT_EQ(location.locationFlags, 0u) << "spec: no flags once the render model is destroyed";
}

TEST_F(OxrExtRenderModelTest, paths_tell_which_hand_and_actions_a_model_serves)
{
    const XrRenderModelEXT model = right_model();
    XrPath                 paths[4] = {};
    u32                    count    = 0;
    ASSERT_EQ(rm.xrEnumerateRenderModelSubactionPathsEXT(model, nullptr, 4, &count, paths), XR_SUCCESS);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(paths[0], hands[1]);

    const XrPath both[2] = { hands[0], hands[1] };
    XrInteractionRenderModelTopLevelUserPathGetInfoEXT info = { .type = XR_TYPE_INTERACTION_RENDER_MODEL_TOP_LEVEL_USER_PATH_GET_INFO_EXT, .topLevelUserPathCount = 2, .topLevelUserPaths = both };
    XrPath top = XR_NULL_PATH;
    ASSERT_EQ(rm.xrGetRenderModelPoseTopLevelUserPathEXT(model, &info, &top), XR_SUCCESS);
    EXPECT_EQ(top, hands[1]);
    info.topLevelUserPaths = &hands[0];
    info.topLevelUserPathCount = 1;
    ASSERT_EQ(rm.xrGetRenderModelPoseTopLevelUserPathEXT(model, &info, &top), XR_SUCCESS);
    EXPECT_EQ(top, XR_NULL_PATH) << "not in the left hand";
    const XrPath twice[2] = { hands[1], hands[1] };
    info.topLevelUserPaths     = twice;
    info.topLevelUserPathCount = 2;
    EXPECT_EQ(rm.xrGetRenderModelPoseTopLevelUserPathEXT(model, &info, &top), XR_ERROR_VALIDATION_FAILURE);
    const XrPath not_top = path("/user/hand/right/input");
    info.topLevelUserPaths     = &not_top;
    info.topLevelUserPathCount = 1;
    EXPECT_EQ(rm.xrGetRenderModelPoseTopLevelUserPathEXT(model, &info, &top), XR_ERROR_PATH_INVALID);
}

TEST_F(OxrExtRenderModelTest, another_controller_is_another_model)
{
    const XrRenderModelEXT old_model = right_model();
    XrRenderModelIdEXT     before[4] = {};
    ASSERT_EQ(ids(before), 2u);
    models_changed();
    const XrRenderModelSpaceCreateInfoEXT space_info = { .type = XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT, .renderModel = old_model };
    XrSpace                               space      = XR_NULL_HANDLE;
    ASSERT_EQ(rm.xrCreateRenderModelSpaceEXT(session, &space_info, &space), XR_SUCCESS);

    const SIM_Controller other = sim_get_controller(oxr_get_sim()) == SIM_Controller::OculusTouch ? SIM_Controller::MetaTouchPlus : SIM_Controller::OculusTouch;
    sim_set_controller(oxr_get_sim(), other);
    ASSERT_EQ(sync(), XR_SUCCESS);
    EXPECT_TRUE(models_changed());
    XrRenderModelIdEXT after[4] = {};
    ASSERT_EQ(ids(after), 2u);
    for (XrRenderModelIdEXT id : after)
    {
        EXPECT_NE(id, before[0]) << "spec: an id that left the list never comes back";
        EXPECT_NE(id, before[1]);
    }
    const XrRenderModelCreateInfoEXT stale = { .type = XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT, .renderModelId = before[0] };
    XrRenderModelEXT                 model = XR_NULL_HANDLE;
    EXPECT_EQ(rm.xrCreateRenderModelEXT(session, &stale, &model), XR_ERROR_RENDER_MODEL_ID_INVALID_EXT);
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    ASSERT_EQ(xr.xrLocateSpace(space, stage, frame().predictedDisplayTime, &location), XR_SUCCESS);
    EXPECT_FALSE(location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) << "an inactive model is not located";
    EXPECT_NE(memcmp(properties(old_model).cacheId.data, properties(create(after[0])).cacheId.data, 16), 0);
}
