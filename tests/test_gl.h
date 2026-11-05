#pragma once

// Plays the role of an OpenGL or OpenGL ES app for tests: a surfaceless EGL context on the
// default display, current on the calling thread. Where there is no EGL (Windows) it is a WGL
// core context on a hidden window instead, for OpenGL only. EGL and WGL are loaded at runtime as
// the runtime does it, so the tests need no EGL, GL or Windows headers. GL errors the backend
// reports fail the test.

#include "render.h"

#include <string.h>

typedef void* TestEGLDisplay;
typedef void* TestEGLConfig;
typedef void* TestEGLContext;
typedef i32   TestEGLint;

#define TEST_EGL_NONE                         0x3038
#define TEST_EGL_RED_SIZE                     0x3024
#define TEST_EGL_GREEN_SIZE                   0x3023
#define TEST_EGL_BLUE_SIZE                    0x3022
#define TEST_EGL_ALPHA_SIZE                   0x3021
#define TEST_EGL_SURFACE_TYPE                 0x3033
#define TEST_EGL_RENDERABLE_TYPE              0x3040
#define TEST_EGL_OPENGL_BIT                   0x0008
#define TEST_EGL_OPENGL_ES3_BIT               0x0040
#define TEST_EGL_OPENGL_API                   0x30A2
#define TEST_EGL_OPENGL_ES_API                0x30A0
#define TEST_EGL_CONTEXT_MAJOR_VERSION        0x3098
#define TEST_EGL_CONTEXT_MINOR_VERSION        0x30FB
#define TEST_EGL_CONTEXT_OPENGL_PROFILE_MASK  0x30FD
#define TEST_EGL_CONTEXT_OPENGL_CORE_PROFILE  0x0001
#define TEST_EGL_PLATFORM_SURFACELESS_MESA    0x31DD

inline u32 test_gl_errors = 0;

inline void test_gl_log_callback (OS_LogLevel level, const char* text, void* user) // NOLINT(misc-const-correctness): OS_LogCallback
{
    (void)user;
    if (level == OS_LogLevel::Error && text && strstr(text, "GL error"))
    {
        test_gl_errors++;
    }
}

// PIXELFORMATDESCRIPTOR
struct TestPixelFormat
{
    u16 size;
    u16 version;
    u32 flags;
    u8  pixel_type;
    u8  color_bits;
    u8  channel_bits[13]; // red, green, blue, alpha and accumulation bits and shifts
    u8  depth_bits;
    u8  stencil_bits;
    u8  aux_buffers;
    u8  layer_type;
    u8  reserved;
    u32 layer_mask;
    u32 visible_mask;
    u32 damage_mask;
};
static_assert(sizeof(TestPixelFormat) == 40, "PIXELFORMATDESCRIPTOR has 40 bytes");

#define TEST_PFD_DOUBLEBUFFER                 0x0001
#define TEST_PFD_DRAW_TO_WINDOW               0x0004
#define TEST_PFD_SUPPORT_OPENGL               0x0020
#define TEST_WS_POPUP                         0x80000000u
#define TEST_WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define TEST_WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define TEST_WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define TEST_WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

inline OS_Library* test_opengl32 = nullptr;

// wglGetProcAddress knows extensions and GL above 1.1; opengl32.dll exports the rest
inline void* test_wgl_get_proc_address (const char* name)
{
    void* (*get) (const char*) = (void* (*)(const char*))os_library_symbol(test_opengl32, "wglGetProcAddress");
    void* function             = get ? get(name) : nullptr;
    if ((uintptr_t)function <= 3 || (uintptr_t)function == UINTPTR_MAX)
    {
        function = os_library_symbol(test_opengl32, name);
    }
    return function;
}

struct TestGLApp
{
    b8             wgl = {};
    void*          window = {}; // HWND of a WGL app
    void*          dc = {};     // its HDC
    OS_Library*    egl = {};
    TestEGLDisplay display = {};
    TestEGLConfig  config = {};
    TestEGLContext context = {};
    b8             es = {};
    void*          get_proc_address = {}; // eglGetProcAddress

    TestEGLDisplay (*eglGetPlatformDisplay) (u32 platform, void* native, const intptr_t* attributes) = {};
    TestEGLDisplay (*eglGetDisplay)         (void* native) = {};
    u32            (*eglInitialize)         (TestEGLDisplay display, TestEGLint* major, TestEGLint* minor) = {};
    u32            (*eglChooseConfig)       (TestEGLDisplay display, const TestEGLint* attributes, TestEGLConfig* configs, TestEGLint size, TestEGLint* count) = {};
    u32            (*eglBindAPI)            (u32 api) = {};
    TestEGLContext (*eglCreateContext)      (TestEGLDisplay display, TestEGLConfig config, TestEGLContext share, const TestEGLint* attributes) = {};
    u32            (*eglDestroyContext)     (TestEGLDisplay display, TestEGLContext context) = {};
    u32            (*eglMakeCurrent)        (TestEGLDisplay display, void* draw, void* read, TestEGLContext context) = {};

    // A hidden window with an OpenGL pixel format, a legacy context to find
    // wglCreateContextAttribsARB, then the 3.3 core context the app renders with
    b8 create_wgl ()
    {
        OS_Library* user32 = os_library_open("user32.dll");
        OS_Library* gdi32  = os_library_open("gdi32.dll");
        test_opengl32      = os_library_open("opengl32.dll");
        void* (*create_window) (u32, const wchar_t*, const wchar_t*, u32, i32, i32, i32, i32, void*, void*, void*, void*) = (decltype(create_window))os_library_symbol(user32, "CreateWindowExW");
        void* (*get_dc) (void*)                                  = (decltype(get_dc))os_library_symbol(user32, "GetDC");
        i32   (*choose) (void*, const TestPixelFormat*)          = (decltype(choose))os_library_symbol(gdi32, "ChoosePixelFormat");
        i32   (*set) (void*, i32, const TestPixelFormat*)        = (decltype(set))os_library_symbol(gdi32, "SetPixelFormat");
        void* (*create_legacy) (void*)                           = (decltype(create_legacy))os_library_symbol(test_opengl32, "wglCreateContext");
        i32   (*make_current) (void*, void*)                     = (decltype(make_current))os_library_symbol(test_opengl32, "wglMakeCurrent");
        i32   (*delete_context) (void*)                          = (decltype(delete_context))os_library_symbol(test_opengl32, "wglDeleteContext");
        if (!create_window || !get_dc || !choose || !set || !create_legacy || !make_current || !delete_context)
        {
            return false;
        }
        window = create_window(0, L"STATIC", L"puppetxr test", TEST_WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
        dc     = window ? get_dc(window) : nullptr;
        const TestPixelFormat format = { .size = sizeof(TestPixelFormat), .version = 1, .flags = TEST_PFD_DRAW_TO_WINDOW | TEST_PFD_SUPPORT_OPENGL | TEST_PFD_DOUBLEBUFFER, .pixel_type = 0, .color_bits = 32, .depth_bits = 24, .stencil_bits = 8 };
        const i32             index  = dc ? choose(dc, &format) : 0;
        if (index == 0 || !set(dc, index, &format))
        {
            return false;
        }
        void* legacy = create_legacy(dc);
        if (!legacy || !make_current(dc, legacy))
        {
            return false;
        }
        void* (*create_attribs) (void*, void*, const i32*) = (decltype(create_attribs))test_wgl_get_proc_address("wglCreateContextAttribsARB");
        const i32 attributes[] = { TEST_WGL_CONTEXT_MAJOR_VERSION_ARB, 3, TEST_WGL_CONTEXT_MINOR_VERSION_ARB, 3, TEST_WGL_CONTEXT_PROFILE_MASK_ARB, TEST_WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
        context = create_attribs ? create_attribs(dc, nullptr, attributes) : nullptr;
        make_current(nullptr, nullptr);
        delete_context(legacy);
        if (!context || !make_current(dc, context))
        {
            return false;
        }
        wgl              = true;
        get_proc_address = (void*)test_wgl_get_proc_address;
        test_gl_errors   = 0;
        os_set_log_callback(test_gl_log_callback, nullptr);
        return true;
    }

    void destroy_wgl ()
    {
        i32 (*make_current) (void*, void*) = (decltype(make_current))os_library_symbol(test_opengl32, "wglMakeCurrent");
        i32 (*delete_context) (void*)      = (decltype(delete_context))os_library_symbol(test_opengl32, "wglDeleteContext");
        i32 (*destroy_window) (void*)      = (decltype(destroy_window))os_library_symbol(os_library_open("user32.dll"), "DestroyWindow");
        if (context && make_current && delete_context)
        {
            make_current(nullptr, nullptr);
            delete_context(context);
        }
        if (window && destroy_window)
        {
            destroy_window(window);
        }
        context = nullptr;
        window  = nullptr;
        dc      = nullptr;
        wgl     = false;
    }

    b8 create (b8 use_es)
    {
        es  = use_es;
        egl = os_library_open("libEGL.so.1");
        if (!egl)
        {
            return !use_es && os_library_open("opengl32.dll") && create_wgl();
        }
        get_proc_address      = os_library_symbol(egl, "eglGetProcAddress");
        eglGetPlatformDisplay = (decltype(eglGetPlatformDisplay))os_library_symbol(egl, "eglGetPlatformDisplay");
        eglGetDisplay         = (decltype(eglGetDisplay))os_library_symbol(egl, "eglGetDisplay");
        eglInitialize         = (decltype(eglInitialize))os_library_symbol(egl, "eglInitialize");
        eglChooseConfig       = (decltype(eglChooseConfig))os_library_symbol(egl, "eglChooseConfig");
        eglBindAPI            = (decltype(eglBindAPI))os_library_symbol(egl, "eglBindAPI");
        eglCreateContext      = (decltype(eglCreateContext))os_library_symbol(egl, "eglCreateContext");
        eglDestroyContext     = (decltype(eglDestroyContext))os_library_symbol(egl, "eglDestroyContext");
        eglMakeCurrent        = (decltype(eglMakeCurrent))os_library_symbol(egl, "eglMakeCurrent");
        if (!get_proc_address || !eglGetDisplay || !eglInitialize || !eglChooseConfig || !eglBindAPI || !eglCreateContext || !eglMakeCurrent)
        {
            return false;
        }

        display = eglGetPlatformDisplay ? eglGetPlatformDisplay(TEST_EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr) : nullptr;
        if (!display)
        {
            display = eglGetDisplay(nullptr);
        }
        if (!display || !eglInitialize(display, nullptr, nullptr))
        {
            display = nullptr;
            return false;
        }

        const TestEGLint config_attributes[] = { TEST_EGL_RENDERABLE_TYPE, es ? TEST_EGL_OPENGL_ES3_BIT : TEST_EGL_OPENGL_BIT, TEST_EGL_SURFACE_TYPE, 0, TEST_EGL_RED_SIZE, 8, TEST_EGL_GREEN_SIZE, 8, TEST_EGL_BLUE_SIZE, 8, TEST_EGL_ALPHA_SIZE, 8, TEST_EGL_NONE };
        TestEGLint       count                = 0;
        if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count == 0)
        {
            return false;
        }
        eglBindAPI(es ? TEST_EGL_OPENGL_ES_API : TEST_EGL_OPENGL_API);
        const TestEGLint gl_attributes[] = { TEST_EGL_CONTEXT_MAJOR_VERSION, 3, TEST_EGL_CONTEXT_MINOR_VERSION, 3, TEST_EGL_CONTEXT_OPENGL_PROFILE_MASK, TEST_EGL_CONTEXT_OPENGL_CORE_PROFILE, TEST_EGL_NONE };
        const TestEGLint es_attributes[] = { TEST_EGL_CONTEXT_MAJOR_VERSION, 3, TEST_EGL_NONE };
        context = eglCreateContext(display, config, nullptr, es ? es_attributes : gl_attributes);
        if (!context || !eglMakeCurrent(display, nullptr, nullptr, context))
        {
            return false;
        }
        test_gl_errors = 0;
        os_set_log_callback(test_gl_log_callback, nullptr);
        return true;
    }

    void destroy ()
    {
        os_set_log_callback(nullptr, nullptr);
        if (wgl)
        {
            destroy_wgl();
            return;
        }
        if (context)
        {
            eglMakeCurrent(display, nullptr, nullptr, nullptr);
            eglDestroyContext(display, context);
            context = nullptr;
        }
        // EGL and its driver stay loaded: drivers keep allocations for the process, and their
        // frames need names for the leak suppressions (tests/lsan.supp)
        display = nullptr;
        egl     = nullptr;
    }

    R_DeviceCreateInfo info () const
    {
        if (wgl)
        {
            return { .backend = R_Backend::OpenGL, .gl = { .platform = R_GLPlatform::WGL, .get_proc_address = get_proc_address, .context = context, .drawable = (u64)(uintptr_t)dc } };
        }
        return { .backend = es ? R_Backend::OpenGLES : R_Backend::OpenGL, .gl = { .platform = R_GLPlatform::EGL, .get_proc_address = get_proc_address, .display = display, .config = config, .context = context } };
    }
};

// An app on GLX, as Godot's compatibility renderer: a 3.3 core context current with a pbuffer on
// the X display of DISPLAY. Absent without an X server with GLX.
#define TEST_GLX_DRAWABLE_TYPE              0x8010
#define TEST_GLX_PBUFFER_BIT                0x00000004
#define TEST_GLX_RENDER_TYPE                0x8011
#define TEST_GLX_RGBA_BIT                   0x00000001
#define TEST_GLX_PBUFFER_WIDTH              0x8041
#define TEST_GLX_PBUFFER_HEIGHT             0x8040
#define TEST_GLX_VISUAL_ID                  0x800B
#define TEST_GLX_CONTEXT_MAJOR_VERSION_ARB  0x2091
#define TEST_GLX_CONTEXT_MINOR_VERSION_ARB  0x2092
#define TEST_GLX_CONTEXT_PROFILE_MASK_ARB   0x9126
#define TEST_GLX_CONTEXT_CORE_PROFILE_BIT   0x0001

struct TestGLXApp
{
    OS_Library* x11 = {};
    OS_Library* glx = {};
    void*       display = {};
    void*       config = {};
    void*       context = {};
    u64         pbuffer = {};
    u32         visual = {};
    void*       get_proc_address = {}; // glXGetProcAddressARB

    void*  (*XOpenDisplay)          (const char* name) = {};
    i32    (*XCloseDisplay)         (void* display) = {};
    i32    (*XFree)                 (void* data) = {};
    void** (*glXChooseFBConfig)     (void* display, i32 screen, const i32* attributes, i32* count) = {};
    i32    (*glXGetFBConfigAttrib)  (void* display, void* config, i32 attribute, i32* value) = {};
    u64    (*glXCreatePbuffer)      (void* display, void* config, const i32* attributes) = {};
    void   (*glXDestroyPbuffer)     (void* display, u64 pbuffer) = {};
    i32    (*glXMakeContextCurrent) (void* display, u64 draw, u64 read, void* context) = {};
    void   (*glXDestroyContext)     (void* display, void* context) = {};

    b8 create ()
    {
        x11 = os_library_open("libX11.so.6");
        glx = os_library_open("libGL.so.1");
        if (!x11 || !glx || !os_env("DISPLAY"))
        {
            return false;
        }
        XOpenDisplay          = (decltype(XOpenDisplay))os_library_symbol(x11, "XOpenDisplay");
        XCloseDisplay         = (decltype(XCloseDisplay))os_library_symbol(x11, "XCloseDisplay");
        XFree                 = (decltype(XFree))os_library_symbol(x11, "XFree");
        get_proc_address      = os_library_symbol(glx, "glXGetProcAddressARB");
        glXChooseFBConfig     = (decltype(glXChooseFBConfig))os_library_symbol(glx, "glXChooseFBConfig");
        glXGetFBConfigAttrib  = (decltype(glXGetFBConfigAttrib))os_library_symbol(glx, "glXGetFBConfigAttrib");
        glXCreatePbuffer      = (decltype(glXCreatePbuffer))os_library_symbol(glx, "glXCreatePbuffer");
        glXDestroyPbuffer     = (decltype(glXDestroyPbuffer))os_library_symbol(glx, "glXDestroyPbuffer");
        glXMakeContextCurrent = (decltype(glXMakeContextCurrent))os_library_symbol(glx, "glXMakeContextCurrent");
        glXDestroyContext     = (decltype(glXDestroyContext))os_library_symbol(glx, "glXDestroyContext");
        typedef void* (*CreateContext) (void* display, void* config, void* share, i32 direct, const i32* attributes);
        typedef void* (*GetProcAddress) (const char* name);
        if (!XOpenDisplay || !get_proc_address || !glXChooseFBConfig || !glXCreatePbuffer || !glXMakeContextCurrent)
        {
            return false;
        }
        const CreateContext create_context = (CreateContext)((GetProcAddress)get_proc_address)("glXCreateContextAttribsARB");
        display                            = XOpenDisplay(nullptr);
        if (!display || !create_context)
        {
            return false;
        }
        const i32 config_attributes[] = { TEST_GLX_DRAWABLE_TYPE, TEST_GLX_PBUFFER_BIT, TEST_GLX_RENDER_TYPE, TEST_GLX_RGBA_BIT, 0 };
        i32       count               = 0;
        void**    configs             = glXChooseFBConfig(display, 0, config_attributes, &count);
        if (!configs || count == 0)
        {
            return false;
        }
        config = configs[0];
        XFree(configs);
        i32 visual_id = 0;
        glXGetFBConfigAttrib(display, config, TEST_GLX_VISUAL_ID, &visual_id);
        visual                          = (u32)visual_id;
        const i32 pbuffer_attributes[]  = { TEST_GLX_PBUFFER_WIDTH, 16, TEST_GLX_PBUFFER_HEIGHT, 16, 0 };
        pbuffer                         = glXCreatePbuffer(display, config, pbuffer_attributes);
        const i32 context_attributes[]  = { TEST_GLX_CONTEXT_MAJOR_VERSION_ARB, 3, TEST_GLX_CONTEXT_MINOR_VERSION_ARB, 3, TEST_GLX_CONTEXT_PROFILE_MASK_ARB, TEST_GLX_CONTEXT_CORE_PROFILE_BIT, 0 };
        context                         = create_context(display, config, nullptr, 1, context_attributes);
        return pbuffer && context && glXMakeContextCurrent(display, pbuffer, pbuffer, context);
    }

    void destroy ()
    {
        if (context)
        {
            glXMakeContextCurrent(display, 0, 0, nullptr);
            glXDestroyContext(display, context);
        }
        if (pbuffer)
        {
            glXDestroyPbuffer(display, pbuffer);
        }
        if (display)
        {
            XCloseDisplay(display);
        }
        context = nullptr;
        pbuffer = 0;
        display = nullptr;
    }

    // Godot's compatibility renderer passes no config; config_known adds it as the spec asks
    R_DeviceCreateInfo info (b8 config_known) const
    {
        return { .backend = R_Backend::OpenGL, .gl = { .platform = R_GLPlatform::GLX, .get_proc_address = get_proc_address, .display = display, .config = config_known ? config : nullptr, .context = context, .drawable = pbuffer } };
    }
};
