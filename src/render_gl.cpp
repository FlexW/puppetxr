#include "render_internal.h"

//
// Types
//
// No GL or EGL headers: the few types and constants are declared here and every entry point is
// loaded at runtime from the libraries the app already loaded.

#if defined(_WIN32) && !defined(_WIN64)
#define R_GLAPI __stdcall
#else
#define R_GLAPI
#endif

typedef u32               GLenum;
typedef u32               GLuint;
typedef i32               GLint;
typedef i32               GLsizei;
typedef u8                GLboolean;
typedef u32               GLbitfield;
typedef f32               GLfloat;
typedef intptr_t          GLintptr;
typedef intptr_t          GLsizeiptr;
typedef char              GLchar;
typedef u64               GLuint64;
typedef struct R_GLSync_* GLsync;

#define GL_NO_ERROR                     0
#define GL_FALSE                        0
#define GL_TRUE                         1
#define GL_TRIANGLES                    0x0004
#define GL_NEVER                        0x0200
#define GL_LESS                         0x0201
#define GL_EQUAL                        0x0202
#define GL_LEQUAL                       0x0203
#define GL_GREATER                      0x0204
#define GL_NOTEQUAL                     0x0205
#define GL_GEQUAL                       0x0206
#define GL_ALWAYS                       0x0207
#define GL_ZERO                         0
#define GL_ONE                          1
#define GL_DST_ALPHA                    0x0304
#define GL_ONE_MINUS_DST_ALPHA          0x0305
#define GL_SRC_ALPHA                    0x0302
#define GL_ONE_MINUS_SRC_ALPHA          0x0303
#define GL_FUNC_ADD                     0x8006
#define GL_BLEND                        0x0BE2
#define GL_DEPTH_TEST                   0x0B71
#define GL_SCISSOR_TEST                 0x0C11
#define GL_CULL_FACE                    0x0B44
#define GL_FRAMEBUFFER_SRGB             0x8DB9
#define GL_TEXTURE_CUBE_MAP_SEAMLESS    0x884F
#define GL_UNPACK_ALIGNMENT             0x0CF5
#define GL_PACK_ALIGNMENT               0x0D05
#define GL_MAJOR_VERSION                0x821B
#define GL_MINOR_VERSION                0x821C
#define GL_NUM_EXTENSIONS               0x821D
#define GL_EXTENSIONS                   0x1F03
#define GL_MAX_ARRAY_TEXTURE_LAYERS     0x88FF
#define GL_UNSIGNED_BYTE                0x1401
#define GL_UNSIGNED_SHORT               0x1403
#define GL_UNSIGNED_INT                 0x1405
#define GL_FLOAT                        0x1406
#define GL_HALF_FLOAT                   0x140B
#define GL_UNSIGNED_INT_24_8            0x84FA
#define GL_RGBA                         0x1908
#define GL_DEPTH_COMPONENT              0x1902
#define GL_DEPTH_STENCIL                0x84F9
#define GL_RGBA8                        0x8058
#define GL_SRGB8_ALPHA8                 0x8C43
#define GL_RGBA16F                      0x881A
#define GL_DEPTH_COMPONENT16            0x81A5
#define GL_DEPTH_COMPONENT32F           0x8CAC
#define GL_DEPTH24_STENCIL8             0x88F0
#define GL_DEPTH32F_STENCIL8            0x8CAD
#define GL_FLOAT_32_UNSIGNED_INT_24_8_REV 0x8DAD
#define GL_TEXTURE_2D                   0x0DE1
#define GL_TEXTURE_2D_ARRAY             0x8C1A
#define GL_TEXTURE_CUBE_MAP             0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X  0x8515
#define GL_TEXTURE_MAX_LEVEL            0x813D
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803
#define GL_TEXTURE_WRAP_R               0x8072
#define GL_NEAREST                      0x2600
#define GL_LINEAR                       0x2601
#define GL_CLAMP_TO_EDGE                0x812F
#define GL_TEXTURE0                     0x84C0
#define GL_ARRAY_BUFFER                 0x8892
#define GL_ELEMENT_ARRAY_BUFFER         0x8893
#define GL_UNIFORM_BUFFER               0x8A11
#define GL_COPY_WRITE_BUFFER            0x8F37
#define GL_STATIC_DRAW                  0x88E4
#define GL_DYNAMIC_DRAW                 0x88E8
#define GL_FRAMEBUFFER                  0x8D40
#define GL_READ_FRAMEBUFFER             0x8CA8
#define GL_DRAW_FRAMEBUFFER             0x8CA9
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_DEPTH_ATTACHMENT             0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT     0x821A
#define GL_NONE                         0
#define GL_COLOR                        0x1800
#define GL_DEPTH                        0x1801
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_VERTEX_SHADER                0x8B31
#define GL_FRAGMENT_SHADER              0x8B30
#define GL_COMPILE_STATUS               0x8B81
#define GL_LINK_STATUS                  0x8B82
#define GL_INVALID_INDEX                0xFFFFFFFFu
#define GL_SYNC_GPU_COMMANDS_COMPLETE   0x9117
#define GL_TIMEOUT_IGNORED              0xFFFFFFFFFFFFFFFFull
#define GL_LOWER_LEFT                   0x8CA1
#define GL_ZERO_TO_ONE                  0x935F

// Entry points of GL 3.3 core that GLES 3.0 has too
#define R_GL_FUNCTIONS(X)                                                                                                                                     \
    X(GLenum,         glGetError,                ())                                                                                                      \
    X(void,           glGetIntegerv,             (GLenum pname, GLint* data))                                                                               \
    X(const u8*,      glGetString,               (GLenum name))                                                                                             \
    X(const u8*,      glGetStringi,              (GLenum name, GLuint index))                                                                               \
    X(void,           glEnable,                  (GLenum cap))                                                                                              \
    X(void,           glDisable,                 (GLenum cap))                                                                                              \
    X(void,           glViewport,                (GLint x, GLint y, GLsizei width, GLsizei height))                                                         \
    X(void,           glScissor,                 (GLint x, GLint y, GLsizei width, GLsizei height))                                                         \
    X(void,           glColorMask,               (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                                                      \
    X(void,           glDepthMask,               (GLboolean flag))                                                                                          \
    X(void,           glDepthFunc,               (GLenum func))                                                                                             \
    X(void,           glBlendFuncSeparate,       (GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha, GLenum dst_alpha))                                      \
    X(void,           glBlendEquation,           (GLenum mode))                                                                                             \
    X(void,           glPixelStorei,             (GLenum pname, GLint param))                                                                               \
    X(void,           glReadPixels,              (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels))                \
    X(void,           glFinish,                  ())                                                                                                      \
    X(void,           glFlush,                   ())                                                                                                      \
    X(void,           glClearBufferfv,           (GLenum buffer, GLint drawbuffer, const GLfloat* value))                                                   \
    X(void,           glGenTextures,             (GLsizei n, GLuint* textures))                                                                             \
    X(void,           glDeleteTextures,          (GLsizei n, const GLuint* textures))                                                                       \
    X(void,           glBindTexture,             (GLenum target, GLuint texture))                                                                           \
    X(void,           glActiveTexture,           (GLenum texture))                                                                                          \
    X(void,           glTexParameteri,           (GLenum target, GLenum pname, GLint param))                                                                \
    X(void,           glTexImage2D,              (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void* data)) \
    X(void,           glTexImage3D,              (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLsizei d, GLint border, GLenum format, GLenum type, const void* data)) \
    X(void,           glTexSubImage2D,           (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* data)) \
    X(void,           glTexSubImage3D,           (GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, const void* data)) \
    X(void,           glGenSamplers,             (GLsizei n, GLuint* samplers))                                                                             \
    X(void,           glDeleteSamplers,          (GLsizei n, const GLuint* samplers))                                                                       \
    X(void,           glBindSampler,             (GLuint unit, GLuint sampler))                                                                             \
    X(void,           glSamplerParameteri,       (GLuint sampler, GLenum pname, GLint param))                                                               \
    X(void,           glGenBuffers,              (GLsizei n, GLuint* buffers))                                                                              \
    X(void,           glDeleteBuffers,           (GLsizei n, const GLuint* buffers))                                                                        \
    X(void,           glBindBuffer,              (GLenum target, GLuint buffer))                                                                            \
    X(void,           glBindBufferBase,          (GLenum target, GLuint index, GLuint buffer))                                                              \
    X(void,           glBufferData,              (GLenum target, GLsizeiptr size, const void* data, GLenum usage))                                          \
    X(void,           glBufferSubData,           (GLenum target, GLintptr offset, GLsizeiptr size, const void* data))                                       \
    X(void,           glGenFramebuffers,         (GLsizei n, GLuint* framebuffers))                                                                         \
    X(void,           glDeleteFramebuffers,      (GLsizei n, const GLuint* framebuffers))                                                                   \
    X(void,           glBindFramebuffer,         (GLenum target, GLuint framebuffer))                                                                       \
    X(void,           glFramebufferTexture2D,    (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level))                         \
    X(void,           glFramebufferTextureLayer, (GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer))                              \
    X(GLenum,         glCheckFramebufferStatus,  (GLenum target))                                                                                           \
    X(void,           glDrawBuffers,             (GLsizei n, const GLenum* buffers))                                                                        \
    X(void,           glReadBuffer,              (GLenum buffer))                                                                                           \
    X(void,           glBlitFramebuffer,         (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)) \
    X(GLuint,         glCreateShader,            (GLenum type))                                                                                             \
    X(void,           glShaderSource,            (GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths))                        \
    X(void,           glCompileShader,           (GLuint shader))                                                                                           \
    X(void,           glGetShaderiv,             (GLuint shader, GLenum pname, GLint* params))                                                              \
    X(void,           glGetShaderInfoLog,        (GLuint shader, GLsizei size, GLsizei* length, GLchar* log))                                               \
    X(void,           glDeleteShader,            (GLuint shader))                                                                                           \
    X(GLuint,         glCreateProgram,           ())                                                                                                      \
    X(void,           glAttachShader,            (GLuint program, GLuint shader))                                                                           \
    X(void,           glLinkProgram,             (GLuint program))                                                                                          \
    X(void,           glGetProgramiv,            (GLuint program, GLenum pname, GLint* params))                                                             \
    X(void,           glGetProgramInfoLog,       (GLuint program, GLsizei size, GLsizei* length, GLchar* log))                                              \
    X(void,           glDeleteProgram,           (GLuint program))                                                                                          \
    X(void,           glUseProgram,              (GLuint program))                                                                                          \
    X(GLint,          glGetUniformLocation,      (GLuint program, const GLchar* name))                                                                      \
    X(void,           glUniform1i,               (GLint location, GLint value))                                                                             \
    X(GLuint,         glGetUniformBlockIndex,    (GLuint program, const GLchar* name))                                                                      \
    X(void,           glUniformBlockBinding,     (GLuint program, GLuint index, GLuint binding))                                                            \
    X(void,           glGenVertexArrays,         (GLsizei n, GLuint* arrays))                                                                               \
    X(void,           glDeleteVertexArrays,      (GLsizei n, const GLuint* arrays))                                                                         \
    X(void,           glBindVertexArray,         (GLuint array))                                                                                            \
    X(void,           glEnableVertexAttribArray, (GLuint index))                                                                                            \
    X(void,           glDisableVertexAttribArray,(GLuint index))                                                                                            \
    X(void,           glVertexAttribPointer,     (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer))        \
    X(void,           glDrawArrays,              (GLenum mode, GLint first, GLsizei count))                                                                 \
    X(void,           glDrawElements,            (GLenum mode, GLsizei count, GLenum type, const void* indices))                                            \
    X(GLsync,         glFenceSync,               (GLenum condition, GLbitfield flags))                                                                      \
    X(void,           glWaitSync,                (GLsync sync, GLbitfield flags, GLuint64 timeout))                                                         \
    X(void,           glDeleteSync,              (GLsync sync))

// Entry points that only newer versions or extensions have
#define R_GL_OPTIONAL_FUNCTIONS(X)                                                                                \
    X(void, glTexStorage2D, (GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h))               \
    X(void, glTexStorage3D, (GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h, GLsizei d))    \
    X(void, glClipControl,  (GLenum origin, GLenum depth))                                                        \
    X(void, glXSwapIntervalEXT, (GLXDisplay display, GLXDrawable drawable, i32 interval))                       \
    X(WGLBool, wglSwapIntervalEXT, (i32 interval))

typedef void*    EGLDisplay;
typedef void*    EGLContext;
typedef void*    EGLSurface;
typedef void*    EGLConfig;
typedef i32      EGLint;
typedef u32      EGLBoolean;
typedef u32      EGLenum;

#define EGL_NONE                            0x3038
#define EGL_EXTENSIONS                      0x3055
#define EGL_DRAW                            0x3059
#define EGL_READ                            0x305A
#define EGL_WIDTH                           0x3057
#define EGL_HEIGHT                          0x3056
#define EGL_OPENGL_API                      0x30A2
#define EGL_OPENGL_ES_API                   0x30A0
#define EGL_CONTEXT_MAJOR_VERSION           0x3098
#define EGL_CONTEXT_MINOR_VERSION           0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK     0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x0001
#define EGL_GL_COLORSPACE_KHR               0x309D
#define EGL_GL_COLORSPACE_SRGB_KHR          0x3089

#define R_EGL_FUNCTIONS(X)                                                                                                    \
    X(EGLContext, eglGetCurrentContext,   ())                                                                               \
    X(EGLDisplay, eglGetCurrentDisplay,   ())                                                                               \
    X(EGLSurface, eglGetCurrentSurface,   (EGLint which))                                                                     \
    X(EGLBoolean, eglMakeCurrent,         (EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context))         \
    X(EGLContext, eglCreateContext,       (EGLDisplay display, EGLConfig config, EGLContext share, const EGLint* attributes)) \
    X(EGLBoolean, eglDestroyContext,      (EGLDisplay display, EGLContext context))                                           \
    X(EGLBoolean, eglBindAPI,             (EGLenum api))                                                                      \
    X(EGLenum,    eglQueryAPI,            ())                                                                               \
    X(const char*, eglQueryString,        (EGLDisplay display, EGLint name))                                                  \
    X(EGLint,     eglGetError,            ())                                                                               \
    X(EGLSurface, eglCreatePbufferSurface,(EGLDisplay display, EGLConfig config, const EGLint* attributes))                   \
    X(EGLBoolean, eglDestroySurface,      (EGLDisplay display, EGLSurface surface))                                           \
    X(EGLSurface, eglCreateWindowSurface, (EGLDisplay display, EGLConfig config, uintptr_t window, const EGLint* attributes))  \
    X(EGLBoolean, eglSwapBuffers,         (EGLDisplay display, EGLSurface surface))                                           \
    X(EGLBoolean, eglSwapInterval,        (EGLDisplay display, EGLint interval))

typedef void* GLXDisplay; // Display*
typedef void* GLXContext;
typedef void* GLXFBConfig;
typedef u64   GLXDrawable; // XID
typedef i32   GLXBool;

#define GLX_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB    0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB     0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001
#define GLX_FBCONFIG_ID                  0x8013
#define GLX_SCREEN                       0x800C
#define GLX_VISUAL_ID                    0x800B

#define R_GLX_FUNCTIONS(X)                                                                                                                      \
    X(GLXContext,  glXCreateContextAttribsARB, (GLXDisplay display, GLXFBConfig config, GLXContext share, GLXBool direct, const i32* attributes)) \
    X(GLXBool,     glXMakeContextCurrent,      (GLXDisplay display, GLXDrawable draw, GLXDrawable read, GLXContext context))                  \
    X(GLXContext,  glXGetCurrentContext,       ())                                                                                            \
    X(GLXDrawable, glXGetCurrentDrawable,      ())                                                                                            \
    X(GLXDrawable, glXGetCurrentReadDrawable,  ())                                                                                            \
    X(GLXDisplay,  glXGetCurrentDisplay,       ())                                                                                            \
    X(void,        glXDestroyContext,          (GLXDisplay display, GLXContext context))                                                      \
    X(GLXDrawable, glXCreateWindow,            (GLXDisplay display, GLXFBConfig config, u64 window, const i32* attributes))                   \
    X(void,        glXDestroyWindow,           (GLXDisplay display, GLXDrawable window))                                                      \
    X(void,        glXSwapBuffers,             (GLXDisplay display, GLXDrawable drawable))                                                    \
    X(i32,         glXQueryContext,            (GLXDisplay display, GLXContext context, i32 attribute, i32* value))                          \
    X(GLXFBConfig*, glXGetFBConfigs,           (GLXDisplay display, i32 screen, i32* count))                                                 \
    X(i32,         glXGetFBConfigAttrib,       (GLXDisplay display, GLXFBConfig config, i32 attribute, i32* value))

// WGL: the context functions are exports of opengl32.dll, the pixel format and swap functions
// exports of gdi32.dll; descriptors are only passed between them
typedef void* WGLDC;      // HDC
typedef void* WGLContext; // HGLRC
typedef i32   WGLBool;

namespace
{

struct R_WGLPixelFormat
{
    u8 bytes[40]; // PIXELFORMATDESCRIPTOR
};

} // namespace

#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

#define R_WGL_FUNCTIONS(X)                                                                                   \
    X(WGLContext, wglCreateContextAttribsARB, (WGLDC dc, WGLContext share, const i32* attributes))          \
    X(WGLBool,    wglMakeCurrent,             (WGLDC dc, WGLContext context))                                \
    X(WGLContext, wglGetCurrentContext,       ())                                                            \
    X(WGLDC,      wglGetCurrentDC,            ())                                                            \
    X(WGLBool,    wglDeleteContext,           (WGLContext context))

#define R_GDI_FUNCTIONS(X)                                                                                   \
    X(WGLBool,    SwapBuffers,                (WGLDC dc))                                                    \
    X(i32,        GetPixelFormat,             (WGLDC dc))                                                    \
    X(i32,        DescribePixelFormat,        (WGLDC dc, i32 format, u32 size, R_WGLPixelFormat* descriptor)) \
    X(WGLBool,    SetPixelFormat,             (WGLDC dc, i32 format, const R_WGLPixelFormat* descriptor))

typedef void* (R_GLAPI R_GLGetProcAddress) (const char* name);

namespace
{

// NOLINTBEGIN(bugprone-macro-parentheses): the arguments are a type, a name and a parameter list
struct R_GLFunctions
{
#define R_GL_DECLARE(ret, name, params) ret (R_GLAPI* name) params;
    R_GL_FUNCTIONS(R_GL_DECLARE)
    R_GL_OPTIONAL_FUNCTIONS(R_GL_DECLARE)
    R_EGL_FUNCTIONS(R_GL_DECLARE)
    R_GLX_FUNCTIONS(R_GL_DECLARE)
    R_WGL_FUNCTIONS(R_GL_DECLARE)
    R_GDI_FUNCTIONS(R_GL_DECLARE)
#undef R_GL_DECLARE
};
// NOLINTEND(bugprone-macro-parentheses)

enum class R_GLSampler
{
    Array = 0, // the default of R_TEXTURE_LAYERED
    Plain,
    Cube,
    Count,
};

struct R_GLImage
{
    GLuint      texture;
    GLenum      target;
    u32         width;
    u32         height;
    u32         layers;
    u32         mip_levels;
    R_Format    format;
    b8          imported;
};

struct R_GLBuffer
{
    GLuint buffer; // 0 for a buffer that only stages transfers
    u8*    shadow; // what r_get_buffer_mapped_data hands out, host visible buffers only
    u64    size;
};

struct R_GLRenderPass
{
    R_Format format;
    R_LoadOp load_op;
    R_Format depth_format;
    R_LoadOp depth_load_op;
};

struct R_GLFramebuffer
{
    GLuint             framebuffer = {};
    u32                width = {};
    u32                height = {};
    R_RenderPassHandle render_pass;
};

struct R_GLPipeline
{
    GLuint            programs[(u32)R_GLSampler::Count]; // by the sampler of texture 0; pipelines of plain 2D textures have Array only
    u32               textures_count;
    R_TextureType     texture_type;
    u32               push_constants_size;
    b8                has_color;
    b8                blend_enable;
    R_BlendFactor     blend_factors[4];
    b8                depth_test;
    R_CompareOp       depth_compare;
    b8                depth_write;
    u32               vertex_stride;
    R_VertexAttribute vertex_attributes[R_MAX_VERTEX_ATTRIBUTES];
    u32               vertex_attributes_count;
};

struct R_GLWindowSwapchain
{
    u64           drawable = {};   // EGLSurface, GLXDrawable or HDC of the window
    void*         egl_window = {}; // wl_egl_window* on Wayland
    u32           width = {};
    u32           height = {};
    R_ImageHandle image;           // what the frame draws into; presenting blits it to the window
};

struct R_GLCmdBuffer
{
    u32 frame; // slot in R_GLDevice::recording, R_MAX_FRAMES_IN_FLIGHT for the immediate buffer
};

// The context that was current when puppetxr switched to its own. Drawables are EGLSurface
// pointers, GLX XIDs or HDCs.
struct R_GLCurrent
{
    EGLenum api; // EGL only
    void*   display;
    u64     draw;
    u64     read;
    void*   context;
};

struct R_GLDevice
{
    R_Device                                      base = {};
    R_GLFunctions                                 gl = {};
    R_GLDeviceInfo                                info = {};
    OS_Library*                                   library = {}; // the GL library, when puppetxr had to open it
    b8                                            es = {};
    u32                                           version = {}; // major * 10 + minor
    b8                                            clip_control = {};
    b8                                            texture_storage = {};
    u32                                           max_layers = {};

    OS_Library*                                   gdi = {};     // gdi32.dll on WGL
    void*                                         context = {}; // EGLContext, GLXContext or HGLRC
    u64                                           drawable = {}; // what it is current with: an EGL pbuffer when the display cannot go surfaceless, the app's GLX drawable or HDC
    mutable u32                                   depth = {}; // nesting of enter/leave, which every call may do
    mutable R_GLCurrent                           saved = {};
    u64                                           queue_accesses = {};

    GLuint                                        vertex_array = {};
    GLuint                                        push_buffer = {};
    GLuint                                        samplers[2] = {}; // by R_Filter
    GLuint                                        read_framebuffer = {};
    GLuint                                        draw_framebuffer = {};

    b8                                            recording[R_MAX_FRAMES_IN_FLIGHT + 1] = {};
    R_CmdBufferHandle                             cmd_handles[R_MAX_FRAMES_IN_FLIGHT + 1] = {};
    u32                                           frame_index = {};

    R_PipelineHandle                              bound_pipeline = {};
    GLenum                                        bound_targets[R_MAX_TEXTURES] = {};
    R_BufferHandle                                vertex_buffer = {};
    u64                                           vertex_offset = {};
    R_BufferHandle                                index_buffer = {};
    u64                                           index_offset = {};
    b8                                            index_u16 = {};
    u32                                           enabled_attributes = {}; // bit per location
    R_Rect                                        scissor = {};
    R_FramebufferHandle                           framebuffer = {};

    SparseArray<R_GLImage, R_Image>               images = {};
    SparseArray<R_GLBuffer, R_Buffer>             buffers = {};
    SparseArray<R_GLRenderPass, R_RenderPass>     render_passes = {};
    SparseArray<R_GLFramebuffer, R_Framebuffer>   framebuffers = {};
    SparseArray<R_GLPipeline, R_Pipeline>         pipelines = {};
    SparseArray<R_GLCmdBuffer, R_CmdBuffer>       cmds = {};
    SparseArray<R_GLWindowSwapchain, R_WindowSwapchain> windows = {};
};

} // namespace

//
// Statics
//

// Indexed by R_Format; internal format 0 is a format GL images do not have
const static struct
{
    R_Format format;
    GLenum   internal;
    GLenum   pixel_format;
    GLenum   pixel_type;
    GLint    components; // as a vertex attribute
    GLenum   attribute_type;
    b8       normalized;
} r_gl_formats[] = {
    { R_Format::Unknown,             0,                     0,                  0,                    0, 0,                0 },
    { R_Format::R8G8B8A8_UNORM,      GL_RGBA8,              GL_RGBA,            GL_UNSIGNED_BYTE,     4, GL_UNSIGNED_BYTE, true },
    { R_Format::B8G8R8A8_UNORM,      0,                     0,                  0,                    0, 0,                false },
    { R_Format::R8G8B8A8_SRGB,       GL_SRGB8_ALPHA8,       GL_RGBA,            GL_UNSIGNED_BYTE,     0, 0,                false },
    { R_Format::B8G8R8A8_SRGB,       0,                     0,                  0,                    0, 0,                false },
    { R_Format::R16G16B16A16_SFLOAT, GL_RGBA16F,            GL_RGBA,            GL_HALF_FLOAT,        0, 0,                false },
    { R_Format::R32G32_SFLOAT,       0,                     0,                  0,                    2, GL_FLOAT,         false },
    { R_Format::R32G32B32_SFLOAT,    0,                     0,                  0,                    3, GL_FLOAT,         false },
    { R_Format::D16_UNORM,           GL_DEPTH_COMPONENT16,  GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT,    0, 0,                false },
    { R_Format::D24_UNORM_S8_UINT,   GL_DEPTH24_STENCIL8,   GL_DEPTH_STENCIL,   GL_UNSIGNED_INT_24_8, 0, 0,                false },
    { R_Format::D32_SFLOAT,          GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT,             0, 0,                false },
    { R_Format::D32_SFLOAT_S8_UINT,  GL_DEPTH32F_STENCIL8,  GL_DEPTH_STENCIL,   GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 0, 0,  false },
};
static_assert(ARRAY_SIZE(r_gl_formats) == (usize)R_Format::Count, "a format is missing");

static R_GLDevice* gl_device (R_Device* device)
{
    return (R_GLDevice*)device;
}

static const R_GLDevice* gl_device (const R_Device* device)
{
    return (const R_GLDevice*)device;
}

static GLenum gl_internal_format (R_Format format)
{
    return (u32)format < ARRAY_SIZE(r_gl_formats) ? r_gl_formats[(u32)format].internal : 0;
}

static R_GLCurrent current_context (const R_GLDevice* device)
{
    const R_GLFunctions& gl      = device->gl;
    R_GLCurrent          current = {};
    if (device->info.platform == R_GLPlatform::GLX)
    {
        current = { .display = gl.glXGetCurrentDisplay(), .draw = gl.glXGetCurrentDrawable(), .read = gl.glXGetCurrentReadDrawable(), .context = gl.glXGetCurrentContext() };
        return current;
    }
    if (device->info.platform == R_GLPlatform::WGL)
    {
        const u64 dc = (u64)(uintptr_t)gl.wglGetCurrentDC();
        current      = { .draw = dc, .read = dc, .context = gl.wglGetCurrentContext() };
        return current;
    }
    // EGL keeps a current context per client API (EGL 1.5, 3.7.2)
    current.api       = gl.eglQueryAPI();
    const EGLenum api = device->es ? EGL_OPENGL_ES_API : EGL_OPENGL_API;
    if (current.api != api)
    {
        gl.eglBindAPI(api);
    }
    current.display = gl.eglGetCurrentDisplay();
    current.context = gl.eglGetCurrentContext();
    current.draw    = (u64)(uintptr_t)gl.eglGetCurrentSurface(EGL_DRAW);
    current.read    = (u64)(uintptr_t)gl.eglGetCurrentSurface(EGL_READ);
    return current;
}

static b8 make_current (const R_GLDevice* device, void* display, u64 draw, u64 read, void* context)
{
    const R_GLFunctions& gl = device->gl;
    display                 = display ? display : device->info.display;
    if (device->info.platform == R_GLPlatform::GLX)
    {
        return gl.glXMakeContextCurrent(display, draw, read, context) != 0;
    }
    if (device->info.platform == R_GLPlatform::WGL)
    {
        (void)read;
        return gl.wglMakeCurrent((WGLDC)(uintptr_t)draw, context) != 0;
    }
    if (!gl.eglMakeCurrent(display, (EGLSurface)(uintptr_t)draw, (EGLSurface)(uintptr_t)read, context))
    {
        LOG_ERROR("eglMakeCurrent failed with EGL error 0x%x", (u32)gl.eglGetError());
        return false;
    }
    return true;
}

// The context of a device is only current inside enter/leave. At the outermost level enter saves
// the app's context and makes the device's current, leave restores the app's. A fence in the
// context that is left, waited for in the one that is entered, orders the app's rendering into
// shared images before puppetxr reads them and puppetxr's work before the app's next commands
// (OpenGL 4.6, appendix D.3.1).
static b8 enter (const R_GLDevice* device)
{
    if (device->depth++ > 0)
    {
        return true;
    }
    const R_GLFunctions& gl = device->gl;
    device->saved           = current_context(device);

    GLsync app_done = nullptr;
    if (device->saved.context && device->saved.context != device->context)
    {
        app_done = gl.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        gl.glFlush();
    }
    if (!make_current(device, device->info.display, device->drawable, device->drawable, device->context))
    {
        LOG_ERROR("Cannot make puppetxr's GL context current");
        if (app_done)
        {
            gl.glDeleteSync(app_done);
        }
        device->depth--;
        return false;
    }
    if (app_done)
    {
        gl.glWaitSync(app_done, 0, GL_TIMEOUT_IGNORED);
        gl.glDeleteSync(app_done);
    }
    return true;
}

static void leave (const R_GLDevice* device, const char* what)
{
    ASSERT(device->depth > 0);
    if (--device->depth > 0)
    {
        return;
    }
    const R_GLFunctions& gl = device->gl;
    for (GLenum error = gl.glGetError(); error != GL_NO_ERROR; error = gl.glGetError())
    {
        LOG_ERROR("GL error 0x%04x in %s", error, what);
        ASSERT_MSG(false, "GL error");
    }

    const R_GLCurrent& saved      = device->saved;
    const b8           to_app     = saved.context && saved.context != device->context;
    GLsync             puppetxr_done = to_app ? gl.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0) : nullptr;
    gl.glFlush();
    if (!make_current(device, saved.display, saved.draw, saved.read, saved.context))
    {
        LOG_ERROR("Cannot make the app's GL context current again");
    }
    if (puppetxr_done)
    {
        gl.glWaitSync(puppetxr_done, 0, GL_TIMEOUT_IGNORED);
        gl.glDeleteSync(puppetxr_done);
    }
    const EGLenum api = device->es ? EGL_OPENGL_ES_API : EGL_OPENGL_API;
    if (device->info.platform == R_GLPlatform::EGL && saved.api != api)
    {
        gl.eglBindAPI(saved.api);
    }
}

static b8 has_extension (const R_GLDevice* device, const char* name)
{
    GLint count = 0;
    device->gl.glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; i++)
    {
        const char* extension = (const char*)device->gl.glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (extension && cstr_equal(extension, name))
        {
            return true;
        }
    }
    return false;
}

static b8 egl_has_extension (const char* extensions, const char* name)
{
    const usize length = cstr_length(name);
    for (const char* p = extensions; p && *p;)
    {
        const char* end = p;
        while (*end && *end != ' ')
        {
            end++;
        }
        if ((usize)(end - p) == length && memory_equal(p, name, length))
        {
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

// Apps often load their GL library with RTLD_LOCAL, so its symbols are not in the process's
// scope; opening it again by name finds the copy already loaded
static R_GLGetProcAddress* find_get_proc_address (R_GLDevice* device)
{
    if (device->info.get_proc_address)
    {
        return (R_GLGetProcAddress*)device->info.get_proc_address;
    }
    struct Source
    {
        R_GLPlatform platform;
        const char*  library;
        const char*  symbol;
    };
    static const Source sources[] = {
        { R_GLPlatform::EGL, "libEGL.so.1",  "eglGetProcAddress" },
        { R_GLPlatform::GLX, "libGLX.so.0",  "glXGetProcAddressARB" },
        { R_GLPlatform::GLX, "libGL.so.1",   "glXGetProcAddressARB" },
        { R_GLPlatform::WGL, "opengl32.dll", "wglGetProcAddress" },
    };
    for (const Source& source : sources)
    {
        if (source.platform != device->info.platform)
        {
            continue;
        }
        void* function = os_library_symbol(nullptr, source.symbol);
        if (function)
        {
            return (R_GLGetProcAddress*)function;
        }
        OS_Library* library = os_library_open(source.library);
        function            = os_library_symbol(library, source.symbol);
        if (function)
        {
            device->library = library;
            return (R_GLGetProcAddress*)function;
        }
        os_library_close(library);
    }
    return nullptr;
}

static void* load (const R_GLDevice* device, R_GLGetProcAddress* get_proc_address, const char* name)
{
    void* function = get_proc_address(name);
    // wglGetProcAddress answers 1, 2, 3 or -1 for some unknown names, and null for GL 1.1, which
    // opengl32.dll exports itself
    if (device->info.platform == R_GLPlatform::WGL && ((uintptr_t)function <= 3 || (uintptr_t)function == UINTPTR_MAX))
    {
        function = nullptr;
    }
    if (!function && device->library)
    {
        function = os_library_symbol(device->library, name);
    }
    return function ? function : os_library_symbol(nullptr, name);
}

static b8 load_functions (R_GLDevice* device, R_GLGetProcAddress* get_proc_address)
{
    R_GLFunctions& gl      = device->gl;
    b8             missing = false;
// NOLINTBEGIN(bugprone-macro-parentheses): the arguments are a type, a name and a parameter list
#define R_GL_LOAD(ret, name, params)                                   \
    gl.name = (ret (R_GLAPI*) params)load(device, get_proc_address, #name);    \
    if (!gl.name)                                                      \
    {                                                                  \
        LOG_ERROR("The GL driver has no %s", #name);                   \
        missing = true;                                                \
    }
    R_GL_FUNCTIONS(R_GL_LOAD)
    if (device->info.platform == R_GLPlatform::EGL)
    {
        R_EGL_FUNCTIONS(R_GL_LOAD)
    }
    else if (device->info.platform == R_GLPlatform::GLX)
    {
        R_GLX_FUNCTIONS(R_GL_LOAD)
    }
    else
    {
        R_WGL_FUNCTIONS(R_GL_LOAD)
    }
#undef R_GL_LOAD
#define R_GDI_LOAD(ret, name, params)                                  \
    gl.name = (ret (R_GLAPI*) params)os_library_symbol(device->gdi, #name); \
    if (!gl.name)                                                      \
    {                                                                  \
        LOG_ERROR("gdi32.dll has no %s", #name);                       \
        missing = true;                                                \
    }
    if (device->info.platform == R_GLPlatform::WGL)
    {
        device->gdi = os_library_open("gdi32.dll");
        R_GDI_FUNCTIONS(R_GDI_LOAD)
    }
#undef R_GDI_LOAD
#define R_GL_LOAD_OPTIONAL(ret, name, params) gl.name = (ret (R_GLAPI*) params)load(device, get_proc_address, #name);
    R_GL_OPTIONAL_FUNCTIONS(R_GL_LOAD_OPTIONAL)
#undef R_GL_LOAD_OPTIONAL
// NOLINTEND(bugprone-macro-parentheses)
    if (!gl.glClipControl)
    {
        gl.glClipControl = (void (R_GLAPI*)(GLenum, GLenum))load(device, get_proc_address, "glClipControlEXT");
    }
    return !missing;
}

// The device's own context, sharing objects with the app's (EGL 1.5, 3.7.1)
static b8 create_egl_context (R_GLDevice* device)
{
    const R_GLFunctions& gl      = device->gl;
    const EGLDisplay     display = (EGLDisplay)device->info.display;
    const EGLenum        api     = device->es ? EGL_OPENGL_ES_API : EGL_OPENGL_API;
    const EGLenum        old_api = gl.eglQueryAPI();
    gl.eglBindAPI(api);
    const EGLint gl_attributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    const EGLint es_attributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    device->context              = gl.eglCreateContext(display, (EGLConfig)device->info.config, (EGLContext)device->info.context, device->es ? es_attributes : gl_attributes);
    gl.eglBindAPI(old_api);
    if (!device->context)
    {
        LOG_ERROR("eglCreateContext failed with EGL error 0x%x", (u32)gl.eglGetError());
        return false;
    }
    if (!egl_has_extension(gl.eglQueryString(display, EGL_EXTENSIONS), "EGL_KHR_surfaceless_context"))
    {
        const EGLint pbuffer_attributes[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        const EGLSurface pbuffer          = gl.eglCreatePbufferSurface(display, (EGLConfig)device->info.config, pbuffer_attributes);
        device->drawable                  = (u64)(uintptr_t)pbuffer;
        if (!pbuffer)
        {
            LOG_ERROR("The EGL display can neither go surfaceless nor make a pbuffer of the app's config");
            return false;
        }
    }
    return true;
}

// Godot's compatibility renderer passes no config, though the spec requires it (XrGraphicsBindingOpenGLXlibKHR):
// the context names its own. The array is Xlib's; apps load libX11 privately, so XFree comes from opening it.
static b8 resolve_glx_config (R_GLDevice* device)
{
    const R_GLFunctions& gl     = device->gl;
    i32                  id     = 0;
    i32                  screen = 0;
    if (gl.glXQueryContext(device->info.display, device->info.context, GLX_FBCONFIG_ID, &id) != 0 || gl.glXQueryContext(device->info.display, device->info.context, GLX_SCREEN, &screen) != 0)
    {
        LOG_ERROR("glXQueryContext cannot tell the app's config");
        return false;
    }
    // Mesa's glXChooseFBConfig still filters an id by the default drawable type, so search them all
    i32          count    = 0;
    GLXFBConfig* configs  = gl.glXGetFBConfigs(device->info.display, screen, &count);
    OS_Library*  x11      = os_library_open("libX11.so.6");
    i32 (*x_free) (void*) = x11 ? (i32 (*)(void*))os_library_symbol(x11, "XFree") : nullptr;
    defer { os_library_close(x11); };
    for (i32 i = 0; configs && i < count; i++)
    {
        i32 config_id = 0;
        if (gl.glXGetFBConfigAttrib(device->info.display, configs[i], GLX_FBCONFIG_ID, &config_id) == 0 && config_id == id)
        {
            device->info.config = configs[i];
        }
    }
    if (configs && x_free)
    {
        x_free((void*)configs);
    }
    if (!device->info.config)
    {
        LOG_ERROR("No GLX config with the app context's id 0x%x", id);
        return false;
    }
    LOG_WARNING("The Xlib graphics binding has no config; using the app context's, 0x%x", id);
    return true;
}

static u32 gl_x11_visual (const R_Device* base_device)
{
    const R_GLDevice* device = (const R_GLDevice*)base_device;
    i32               visual = 0;
    if (device->info.platform != R_GLPlatform::GLX || device->gl.glXGetFBConfigAttrib(device->info.display, device->info.config, GLX_VISUAL_ID, &visual) != 0)
    {
        return 0;
    }
    return (u32)visual;
}

// The device's own context, sharing objects with the app's (GLX_ARB_create_context). It is
// current with the app's drawable, which matches the app's config: a failing GLX call raises an
// X error, which Xlib's default handler turns into an exit.
static b8 create_glx_context (R_GLDevice* device)
{
    const R_GLFunctions& gl         = device->gl;
    const i32            attributes[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3, GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    device->context                  = gl.glXCreateContextAttribsARB(device->info.display, device->info.config, device->info.context, 1, attributes);
    device->drawable                 = device->info.drawable;
    if (!device->context)
    {
        LOG_ERROR("glXCreateContextAttribsARB failed");
        return false;
    }
    return true;
}

// wglGetProcAddress answers only while a context is current on the calling thread, which an app
// creating its session on a thread of its own has not; a legacy context on the app's device
// context stands in while the functions load. The four functions are exports of opengl32.dll.
static WGLContext begin_wgl_bootstrap (const R_GLDevice* device, R_GLGetProcAddress* get_proc_address)
{
    const auto get_current  = (WGLContext (R_GLAPI*)())load(device, get_proc_address, "wglGetCurrentContext");
    const auto create       = (WGLContext (R_GLAPI*)(WGLDC))load(device, get_proc_address, "wglCreateContext");
    const auto make_current = (WGLBool (R_GLAPI*)(WGLDC, WGLContext))load(device, get_proc_address, "wglMakeCurrent");
    const auto destroy      = (WGLBool (R_GLAPI*)(WGLContext))load(device, get_proc_address, "wglDeleteContext");
    if (!get_current || !create || !make_current || !destroy || get_current())
    {
        return nullptr;
    }
    const WGLDC      dc      = (WGLDC)(uintptr_t)device->info.drawable;
    const WGLContext context = create(dc);
    if (context && !make_current(dc, context))
    {
        destroy(context);
        return nullptr;
    }
    return context;
}

static void end_wgl_bootstrap (const R_GLDevice* device, WGLContext context)
{
    if (context && device->gl.wglMakeCurrent && device->gl.wglDeleteContext)
    {
        device->gl.wglMakeCurrent(nullptr, nullptr);
        device->gl.wglDeleteContext(context);
    }
}

// The device's own context, sharing objects with the app's (WGL_ARB_create_context). It is
// current with the app's device context, whose pixel format the app's context was made for.
static b8 create_wgl_context (R_GLDevice* device)
{
    const R_GLFunctions& gl           = device->gl;
    const i32            attributes[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    device->drawable                  = device->info.drawable;
    device->context                   = gl.wglCreateContextAttribsARB((WGLDC)(uintptr_t)device->info.drawable, device->info.context, attributes);
    if (!device->context)
    {
        LOG_ERROR("wglCreateContextAttribsARB failed");
        return false;
    }
    return true;
}

// State of the device's context that stays as it is, set once
static b8 initialize_context (R_GLDevice* device)
{
    const R_GLFunctions& gl = device->gl;
    GLint major = 0;
    GLint minor = 0;
    gl.glGetIntegerv(GL_MAJOR_VERSION, &major);
    gl.glGetIntegerv(GL_MINOR_VERSION, &minor);
    device->version = (u32)(major * 10 + minor);
    if (device->version < (device->es ? 30u : 33u))
    {
        LOG_ERROR("%s %d.%d is too old, puppetxr needs %s", device->es ? "OpenGL ES" : "OpenGL", major, minor, device->es ? "3.0" : "3.3");
        return false;
    }
    device->clip_control    = gl.glClipControl && (device->es ? has_extension(device, "GL_EXT_clip_control") : device->version >= 45 || has_extension(device, "GL_ARB_clip_control"));
    device->texture_storage = gl.glTexStorage2D && gl.glTexStorage3D && (device->es || device->version >= 42 || has_extension(device, "GL_ARB_texture_storage"));
    GLint max_layers = 0;
    gl.glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
    device->max_layers = zm::min((u32)max_layers, (u32)R_MAX_IMAGE_LAYERS);

    if (device->clip_control)
    {
        gl.glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    }
    if (!device->es)
    {
        // sRGB targets encode on write as in Vulkan; ES always does
        gl.glEnable(GL_FRAMEBUFFER_SRGB);
        gl.glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    }
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.glBlendEquation(GL_FUNC_ADD);
    gl.glDisable(GL_CULL_FACE);

    gl.glGenVertexArrays(1, &device->vertex_array);
    gl.glBindVertexArray(device->vertex_array);
    gl.glGenBuffers(1, &device->push_buffer);
    gl.glBindBuffer(GL_UNIFORM_BUFFER, device->push_buffer);
    gl.glBufferData(GL_UNIFORM_BUFFER, R_MAX_PUSH_CONSTANTS, nullptr, GL_DYNAMIC_DRAW);
    gl.glBindBufferBase(GL_UNIFORM_BUFFER, 0, device->push_buffer);
    gl.glGenFramebuffers(1, &device->read_framebuffer);
    gl.glGenFramebuffers(1, &device->draw_framebuffer);
    gl.glGenSamplers(2, device->samplers);
    for (u32 i = 0; i < 2; i++)
    {
        const GLint filter = i == (u32)R_Filter::Linear ? GL_LINEAR : GL_NEAREST;
        gl.glSamplerParameteri(device->samplers[i], GL_TEXTURE_MIN_FILTER, filter);
        gl.glSamplerParameteri(device->samplers[i], GL_TEXTURE_MAG_FILTER, filter);
        gl.glSamplerParameteri(device->samplers[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.glSamplerParameteri(device->samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.glSamplerParameteri(device->samplers[i], GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }
    return true;
}

static void attach (const R_GLDevice* device, GLenum target, GLenum attachment, const R_GLImage* image, u32 layer)
{
    const R_GLFunctions& gl = device->gl;
    if (!image)
    {
        gl.glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, 0, 0);
        return;
    }
    switch (image->target)
    {
    case GL_TEXTURE_2D:
        gl.glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, image->texture, 0);
        break;
    case GL_TEXTURE_CUBE_MAP:
        gl.glFramebufferTexture2D(target, attachment, GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, image->texture, 0);
        break;
    default:
        gl.glFramebufferTextureLayer(target, attachment, image->texture, 0, (GLint)layer);
        break;
    }
}

static GLenum depth_attachment (R_Format format)
{
    return r_format_has_stencil(format) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
}

// Binds the scratch framebuffer `target` (read or draw) to one layer of an image, colour or depth
static b8 bind_scratch (const R_GLDevice* device, GLenum target, const R_GLImage& image, u32 layer)
{
    const R_GLFunctions& gl          = device->gl;
    const GLuint         framebuffer = target == GL_READ_FRAMEBUFFER ? device->read_framebuffer : device->draw_framebuffer;
    const b8             depth       = r_format_is_depth(image.format);
    gl.glBindFramebuffer(target, framebuffer);
    attach(device, target, GL_COLOR_ATTACHMENT0, depth ? nullptr : &image, layer);
    attach(device, target, GL_DEPTH_STENCIL_ATTACHMENT, nullptr, 0);
    if (depth)
    {
        attach(device, target, depth_attachment(image.format), &image, layer);
    }
    const GLenum buffer = depth ? GL_NONE : GL_COLOR_ATTACHMENT0;
    if (target == GL_READ_FRAMEBUFFER)
    {
        gl.glReadBuffer(buffer);
    }
    else
    {
        gl.glDrawBuffers(1, &buffer);
    }
    return gl.glCheckFramebufferStatus(target) == GL_FRAMEBUFFER_COMPLETE;
}

static b8 cmd_recording (const R_GLDevice* device, R_CmdBufferHandle cmd)
{
    const R_GLCmdBuffer* buffer = device->cmds.try_get(cmd);
    return buffer && device->recording[buffer->frame];
}

static GLuint compile_shader (const R_GLDevice* device, GLenum stage, const R_ShaderCode& code, R_GLSampler sampler)
{
    const R_GLFunctions& gl       = device->gl;
    const char*          version  = device->es ? "#version 300 es\n" : "#version 330 core\n";
    const char*          backend  = device->es ? "#define R_SHADER_GLES 1\n" : "#define R_SHADER_GL 1\n";
    const char*          stage_define = stage == GL_VERTEX_SHADER ? "#define R_STAGE_VERTEX 1\n" : "#define R_STAGE_FRAGMENT 1\n";
    const char*          clip     = device->clip_control ? "#define R_CLIP_ZERO_TO_ONE 1\n" : "";
    const char*          samplers[] = { "", "#define R_SAMPLER_2D 1\n", "#define R_SAMPLER_CUBE 1\n" };
    const GLchar* const  sources[] = { version, backend, stage_define, clip, samplers[(u32)sampler], "#line 1\n", code.glsl };

    const GLuint shader = gl.glCreateShader(stage);
    gl.glShaderSource(shader, (GLsizei)ARRAY_SIZE(sources), sources, nullptr);
    gl.glCompileShader(shader);
    GLint status = 0;
    gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status)
    {
        char log[2048] = {};
        gl.glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOG_ERROR("Compiling a %s shader failed: %s", stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        gl.glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program (const R_GLDevice* device, const R_CreatePipelineInfo& info, R_GLSampler sampler)
{
    const R_GLFunctions& gl       = device->gl;
    const GLuint         vertex   = compile_shader(device, GL_VERTEX_SHADER, *info.vertex, R_GLSampler::Array);
    const GLuint         fragment = vertex ? compile_shader(device, GL_FRAGMENT_SHADER, *info.fragment, sampler) : 0;
    if (!fragment)
    {
        if (vertex)
        {
            gl.glDeleteShader(vertex);
        }
        return 0;
    }
    const GLuint program = gl.glCreateProgram();
    gl.glAttachShader(program, vertex);
    gl.glAttachShader(program, fragment);
    gl.glLinkProgram(program);
    gl.glDeleteShader(vertex);
    gl.glDeleteShader(fragment);
    GLint status = 0;
    gl.glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status)
    {
        char log[2048] = {};
        gl.glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOG_ERROR("Linking a program failed: %s", log);
        gl.glDeleteProgram(program);
        return 0;
    }

    // Bindings by name: GL 3.3 and ES 3.0 have no layout(binding)
    const GLuint block = gl.glGetUniformBlockIndex(program, "R_Push");
    if (block != GL_INVALID_INDEX)
    {
        gl.glUniformBlockBinding(program, block, 0);
    }
    gl.glUseProgram(program);
    for (u32 i = 0; i < info.textures_count; i++)
    {
        const GLint location = info.fragment->textures[i] ? gl.glGetUniformLocation(program, info.fragment->textures[i]) : -1;
        if (location >= 0)
        {
            gl.glUniform1i(location, (GLint)i);
        }
    }
    gl.glUseProgram(0);
    return program;
}

static void destroy_pipeline_resource (const R_GLDevice* device, const R_GLPipeline& pipeline)
{
    for (const GLuint program : pipeline.programs)
    {
        if (program)
        {
            device->gl.glDeleteProgram(program);
        }
    }
}

static GLenum gl_blend_factor (R_BlendFactor factor)
{
    switch (factor)
    {
    case R_BlendFactor::Zero:
        return GL_ZERO;
    case R_BlendFactor::One:
        return GL_ONE;
    case R_BlendFactor::SrcAlpha:
        return GL_SRC_ALPHA;
    case R_BlendFactor::OneMinusSrcAlpha:
        return GL_ONE_MINUS_SRC_ALPHA;
    case R_BlendFactor::DstAlpha:
        return GL_DST_ALPHA;
    case R_BlendFactor::OneMinusDstAlpha:
        return GL_ONE_MINUS_DST_ALPHA;
    }
    return GL_ONE;
}

static GLenum gl_compare_op (R_CompareOp op)
{
    switch (op)
    {
    case R_CompareOp::LessOrEqual:
        return GL_LEQUAL;
    case R_CompareOp::Always:
        return GL_ALWAYS;
    case R_CompareOp::Never:
        return GL_NEVER;
    case R_CompareOp::Less:
        return GL_LESS;
    case R_CompareOp::Equal:
        return GL_EQUAL;
    case R_CompareOp::Greater:
        return GL_GREATER;
    case R_CompareOp::NotEqual:
        return GL_NOTEQUAL;
    case R_CompareOp::GreaterOrEqual:
        return GL_GEQUAL;
    }
    return GL_LEQUAL;
}

static void apply_pipeline_state (const R_GLDevice* device, const R_GLPipeline& pipeline)
{
    const R_GLFunctions& gl = device->gl;
    if (pipeline.blend_enable)
    {
        gl.glEnable(GL_BLEND);
        gl.glBlendFuncSeparate(gl_blend_factor(pipeline.blend_factors[0]), gl_blend_factor(pipeline.blend_factors[1]), gl_blend_factor(pipeline.blend_factors[2]), gl_blend_factor(pipeline.blend_factors[3]));
    }
    else
    {
        gl.glDisable(GL_BLEND);
    }
    if (pipeline.depth_test)
    {
        gl.glEnable(GL_DEPTH_TEST);
        gl.glDepthFunc(gl_compare_op(pipeline.depth_compare));
    }
    else
    {
        gl.glDisable(GL_DEPTH_TEST);
    }
    gl.glDepthMask(pipeline.depth_write ? GL_TRUE : GL_FALSE);
    const GLboolean color = pipeline.has_color ? GL_TRUE : GL_FALSE;
    gl.glColorMask(color, color, color, color);
}

// Selects the program for the bound textures and points the attributes at the bound vertex
// buffer, `base_vertex` vertices in: ES 3.0 has no base vertex for indexed draws
static b8 prepare_draw (R_GLDevice* device, R_CmdBufferHandle cmd, i64 base_vertex)
{
    const R_GLFunctions& gl       = device->gl;
    const R_GLPipeline*  pipeline = device->pipelines.try_get(device->bound_pipeline);
    if (!cmd_recording(device, cmd) || !pipeline)
    {
        ASSERT_MSG(false, "draw without a pipeline");
        return false;
    }
    R_GLSampler sampler = R_GLSampler::Array;
    if (pipeline->texture_type == R_TextureType::Type2DArray && pipeline->textures_count > 0)
    {
        sampler = device->bound_targets[0] == GL_TEXTURE_2D ? R_GLSampler::Plain : device->bound_targets[0] == GL_TEXTURE_CUBE_MAP ? R_GLSampler::Cube : R_GLSampler::Array;
    }
    gl.glUseProgram(pipeline->programs[(u32)sampler]);

    u32 enabled = 0;
    if (pipeline->vertex_stride)
    {
        const R_GLBuffer* buffer = device->buffers.try_get(device->vertex_buffer);
        if (!buffer || !buffer->buffer)
        {
            ASSERT_MSG(false, "draw without a vertex buffer");
            return false;
        }
        gl.glBindBuffer(GL_ARRAY_BUFFER, buffer->buffer);
        for (u32 i = 0; i < pipeline->vertex_attributes_count; i++)
        {
            const R_VertexAttribute& attribute = pipeline->vertex_attributes[i];
            const auto&              format    = r_gl_formats[(u32)attribute.format];
            const i64                offset    = (i64)device->vertex_offset + base_vertex * pipeline->vertex_stride + attribute.offset;
            gl.glEnableVertexAttribArray(attribute.location);
            gl.glVertexAttribPointer(attribute.location, format.components, format.attribute_type, format.normalized ? GL_TRUE : GL_FALSE, (GLsizei)pipeline->vertex_stride, (const void*)(intptr_t)offset);
            enabled |= 1u << attribute.location;
        }
    }
    for (u32 location = 0; location < 32; location++)
    {
        if ((device->enabled_attributes & ~enabled) & (1u << location))
        {
            gl.glDisableVertexAttribArray(location);
        }
    }
    device->enabled_attributes = enabled;
    return true;
}

// Xlib reports a failing GLX request through its error handler, whose default exits the process;
// requests that may fail run with this one installed, then XSync collects the outcome
static b8 r_gl_x_error = false;

static i32 trap_x_error (void* display, void* event) // NOLINT(misc-const-correctness): XErrorHandler
{
    (void)display;
    (void)event;
    r_gl_x_error = true;
    return 0;
}

typedef i32 (*R_XErrorHandler) (void* display, void* event);

static GLXDrawable create_glx_window (const R_GLDevice* device, u64 window)
{
    R_XErrorHandler (*set_handler) (R_XErrorHandler handler) = (R_XErrorHandler (*)(R_XErrorHandler))os_library_symbol(nullptr, "XSetErrorHandler");
    i32 (*sync) (void* display, i32 discard)                 = (i32 (*)(void*, i32))os_library_symbol(nullptr, "XSync");
    if (!set_handler || !sync)
    {
        LOG_WARNING("libX11 is not loaded; no GLX window");
        return 0;
    }
    r_gl_x_error                   = false;
    const R_XErrorHandler previous = set_handler(trap_x_error);
    GLXDrawable           drawable = device->gl.glXCreateWindow(device->info.display, device->info.config, window, nullptr);
    sync(device->info.display, 0);
    set_handler(previous);
    if (r_gl_x_error && drawable)
    {
        device->gl.glXDestroyWindow(device->info.display, drawable);
        drawable = 0;
    }
    return r_gl_x_error ? 0 : drawable;
}

// A context draws only into windows of its pixel format; a window has one for its life, set
// before first use. The mirror window gets the app's.
static b8 match_pixel_format (const R_GLDevice* device, void* window_dc)
{
    const R_GLFunctions& gl     = device->gl;
    const WGLDC          app_dc = (WGLDC)(uintptr_t)device->info.drawable;
    const i32            format = gl.GetPixelFormat(app_dc);
    const i32            window = gl.GetPixelFormat(window_dc);
    if (format == 0)
    {
        LOG_WARNING("The app's device context has no pixel format");
        return false;
    }
    if (window == format)
    {
        return true;
    }
    R_WGLPixelFormat descriptor = {};
    if (window != 0 || gl.DescribePixelFormat(app_dc, format, sizeof(descriptor), &descriptor) == 0 || !gl.SetPixelFormat(window_dc, format, &descriptor))
    {
        LOG_WARNING("Cannot give the mirror window the app's pixel format %d (it has %d)", format, window);
        return false;
    }
    return true;
}

static void destroy_window_resource (R_GLDevice* device, R_GLWindowSwapchain& window)
{
    if (window.drawable)
    {
        if (device->info.platform == R_GLPlatform::GLX)
        {
            device->gl.glXDestroyWindow(device->info.display, window.drawable);
        }
        else if (device->info.platform == R_GLPlatform::EGL)
        {
            device->gl.eglDestroySurface(device->info.display, (EGLSurface)(uintptr_t)window.drawable);
        }
    }
    if (window.egl_window)
    {
        void (*destroy) (void* window) = (void (*)(void*))os_library_symbol(nullptr, "wl_egl_window_destroy");
        if (destroy)
        {
            destroy(window.egl_window);
        }
    }
    r_destroy_image(&device->base, window.image);
    window = {};
}

//
// Device
//

static void gl_destroy_device (R_Device* base_device)
{
    R_GLDevice* device = gl_device(base_device);
    if (!device)
    {
        return;
    }
    const R_GLFunctions& gl = device->gl;
    for (R_GLWindowSwapchain& window : device->windows)
    {
        destroy_window_resource(device, window);
    }
    if (device->context && enter(device))
    {
        for (const R_GLImage& image : device->images)
        {
            if (!image.imported)
            {
                gl.glDeleteTextures(1, &image.texture);
            }
        }
        for (const R_GLBuffer& buffer : device->buffers)
        {
            if (buffer.buffer)
            {
                gl.glDeleteBuffers(1, &buffer.buffer);
            }
        }
        for (const R_GLFramebuffer& framebuffer : device->framebuffers)
        {
            gl.glDeleteFramebuffers(1, &framebuffer.framebuffer);
        }
        for (const R_GLPipeline& pipeline : device->pipelines)
        {
            destroy_pipeline_resource(device, pipeline);
        }
        gl.glDeleteSamplers(2, device->samplers);
        gl.glDeleteFramebuffers(1, &device->read_framebuffer);
        gl.glDeleteFramebuffers(1, &device->draw_framebuffer);
        gl.glDeleteBuffers(1, &device->push_buffer);
        gl.glDeleteVertexArrays(1, &device->vertex_array);
        gl.glFinish();
        leave(device, "destroy_device");
    }
    for (const R_GLBuffer& buffer : device->buffers)
    {
        os_memory_release(buffer.shadow);
    }
    if (device->info.platform == R_GLPlatform::EGL)
    {
        if (device->drawable)
        {
            gl.eglDestroySurface(device->info.display, (EGLSurface)(uintptr_t)device->drawable);
        }
        if (device->context)
        {
            gl.eglDestroyContext(device->info.display, device->context);
        }
    }
    else if (device->context && device->info.platform == R_GLPlatform::GLX)
    {
        gl.glXDestroyContext(device->info.display, device->context);
    }
    else if (device->context)
    {
        gl.wglDeleteContext(device->context);
    }
    os_library_close(device->library);
    os_library_close(device->gdi);
    device->images.release();
    device->buffers.release();
    device->render_passes.release();
    device->framebuffers.release();
    device->pipelines.release();
    device->cmds.release();
    device->windows.release();
    os_memory_release(device);
}

static void gl_wait_idle (const R_Device* base_device)
{
    const R_GLDevice* device = gl_device(base_device);
    if (enter(device))
    {
        device->gl.glFinish();
        leave(device, "wait_idle");
    }
}

static void gl_flush_initial_layouts (R_Device* base_device) // NOLINT(misc-const-correctness): the backend table fixes the signature
{
    (void)base_device;
}

static u64 gl_queue_access_count (const R_Device* base_device)
{
    return gl_device(base_device)->queue_accesses;
}

static i64 gl_native_format (const R_Device* base_device, R_Format format)
{
    (void)base_device;
    return (i64)gl_internal_format(format);
}

static R_Format gl_format_from_native (const R_Device* base_device, i64 format)
{
    (void)base_device;
    for (u32 i = 1; i < ARRAY_SIZE(r_gl_formats); i++)
    {
        if (r_gl_formats[i].internal != 0 && (i64)r_gl_formats[i].internal == format)
        {
            return r_gl_formats[i].format;
        }
    }
    return R_Format::Unknown;
}

//
// Images
//

static R_ImageHandle gl_create_image (R_Device* base_device, const R_CreateImageInfo& info)
{
    R_GLDevice*  device = gl_device(base_device);
    const u32    layers = info.layers ? info.layers : 1;
    const u32    mips   = info.mip_levels ? info.mip_levels : 1;
    const GLenum internal = gl_internal_format(info.format);
    // Cube arrays would need samplerCubeArray in the compositor's GL variant
    const b8     cube_invalid = info.cube_compatible && (info.width != info.height || layers != 6);
    if (info.width == 0 || info.height == 0 || layers > device->max_layers || mips > r_max_mip_levels(info.width, info.height) || internal == 0 || cube_invalid || (info.vk_extra_create_flags | info.vk_extra_usage_flags) != 0)
    {
        LOG_ERROR("Invalid image: %ux%u, %u layers, %u mips, format %u", info.width, info.height, layers, mips, (u32)info.format);
        return NULL_HANDLE;
    }
    if (!enter(device))
    {
        return NULL_HANDLE;
    }
    const R_GLFunctions& gl    = device->gl;
    R_GLImage            image = { .width = info.width, .height = info.height, .layers = layers, .mip_levels = mips, .format = info.format };
    image.target               = info.cube_compatible ? GL_TEXTURE_CUBE_MAP : layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    gl.glGenTextures(1, &image.texture);
    gl.glBindTexture(image.target, image.texture);
    const b8 layered = image.target == GL_TEXTURE_2D_ARRAY;
    if (device->texture_storage)
    {
        if (layered)
        {
            gl.glTexStorage3D(image.target, (GLsizei)mips, internal, (GLsizei)info.width, (GLsizei)info.height, (GLsizei)layers);
        }
        else
        {
            gl.glTexStorage2D(image.target, (GLsizei)mips, internal, (GLsizei)info.width, (GLsizei)info.height);
        }
    }
    else
    {
        const auto& format = r_gl_formats[(u32)info.format];
        for (u32 level = 0; level < mips; level++)
        {
            const GLsizei width  = (GLsizei)zm::max(info.width >> level, 1u);
            const GLsizei height = (GLsizei)zm::max(info.height >> level, 1u);
            if (layered)
            {
                gl.glTexImage3D(image.target, (GLint)level, (GLint)internal, width, height, (GLsizei)layers, 0, format.pixel_format, format.pixel_type, nullptr);
            }
            else if (image.target == GL_TEXTURE_CUBE_MAP)
            {
                for (u32 face = 0; face < 6; face++)
                {
                    gl.glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, (GLint)level, (GLint)internal, width, height, 0, format.pixel_format, format.pixel_type, nullptr);
                }
            }
            else
            {
                gl.glTexImage2D(image.target, (GLint)level, (GLint)internal, width, height, 0, format.pixel_format, format.pixel_type, nullptr);
            }
        }
        gl.glTexParameteri(image.target, GL_TEXTURE_MAX_LEVEL, (GLint)mips - 1);
    }
    gl.glBindTexture(image.target, 0);
    leave(device, "create_image");
    return device->images.add(image);
}

static b8 gl_image_flags_supported (const R_Device* base_device, const R_CreateImageInfo& info)
{
    (void)base_device;
    return (info.vk_extra_create_flags | info.vk_extra_usage_flags) == 0;
}

static R_ImageHandle gl_import_image (R_Device* base_device, const R_ImportImageInfo& info)
{
    R_GLDevice* device = gl_device(base_device);
    if (!info.native_image || info.width == 0 || info.height == 0 || gl_internal_format(info.format) == 0)
    {
        LOG_ERROR("Invalid image import");
        return NULL_HANDLE;
    }
    return device->images.add({ .texture = (GLuint)info.native_image, .target = GL_TEXTURE_2D, .width = info.width, .height = info.height, .layers = 1, .mip_levels = 1, .format = info.format, .imported = true });
}

static void gl_destroy_image (R_Device* base_device, R_ImageHandle handle)
{
    R_GLDevice*      device = gl_device(base_device);
    const R_GLImage* image  = device->images.try_get(handle);
    if (!image)
    {
        return;
    }
    if (!image->imported && enter(device))
    {
        device->gl.glDeleteTextures(1, &image->texture);
        leave(device, "destroy_image");
    }
    device->images.remove(handle);
}

static b8 gl_is_valid_image (const R_Device* base_device, R_ImageHandle image)
{
    return gl_device(base_device)->images.is_valid(image);
}

static u64 gl_get_image_native (const R_Device* base_device, R_ImageHandle handle)
{
    const R_GLImage* image = gl_device(base_device)->images.try_get(handle);
    return image ? image->texture : 0;
}

static void gl_get_image_size (const R_Device* base_device, R_ImageHandle handle, u32& out_width, u32& out_height, u32& out_layers)
{
    const R_GLImage* image = gl_device(base_device)->images.try_get(handle);
    out_width  = image ? image->width : 0;
    out_height = image ? image->height : 0;
    out_layers = image ? image->layers : 0;
}

static R_Format gl_get_image_format (const R_Device* base_device, R_ImageHandle handle)
{
    const R_GLImage* image = gl_device(base_device)->images.try_get(handle);
    return image ? image->format : R_Format::Unknown;
}

//
// Buffers
//

static R_BufferHandle gl_create_buffer (R_Device* base_device, const R_CreateBufferInfo& info)
{
    R_GLDevice* device = gl_device(base_device);
    if (info.size == 0 || info.usage_flags == 0)
    {
        LOG_ERROR("Invalid buffer of %llu bytes", (unsigned long long)info.size);
        return NULL_HANDLE;
    }
    R_GLBuffer buffer = { .size = info.size };
    const u32  device_usage = (u32)R_BufferUsageFlags::Vertex | (u32)R_BufferUsageFlags::Index | (u32)R_BufferUsageFlags::Uniform;
    if (info.usage_flags & device_usage)
    {
        if (!enter(device))
        {
            return NULL_HANDLE;
        }
        const R_GLFunctions& gl = device->gl;
        gl.glGenBuffers(1, &buffer.buffer);
        gl.glBindBuffer(GL_COPY_WRITE_BUFFER, buffer.buffer);
        gl.glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)info.size, nullptr, info.host_visible ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        gl.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        leave(device, "create_buffer");
    }
    if (info.host_visible)
    {
        buffer.shadow = (u8*)os_memory_alloc((usize)info.size);
        memory_zero(buffer.shadow, (usize)info.size);
    }
    return device->buffers.add(buffer);
}

static void gl_destroy_buffer (R_Device* base_device, R_BufferHandle handle)
{
    R_GLDevice*       device = gl_device(base_device);
    const R_GLBuffer* buffer = device->buffers.try_get(handle);
    if (!buffer)
    {
        return;
    }
    if (buffer->buffer && enter(device))
    {
        device->gl.glDeleteBuffers(1, &buffer->buffer);
        leave(device, "destroy_buffer");
    }
    os_memory_release(buffer->shadow);
    device->buffers.remove(handle);
}

static b8 gl_is_valid_buffer (const R_Device* base_device, R_BufferHandle buffer)
{
    return gl_device(base_device)->buffers.is_valid(buffer);
}

static void* gl_get_buffer_mapped_data (const R_Device* base_device, R_BufferHandle handle)
{
    const R_GLBuffer* buffer = gl_device(base_device)->buffers.try_get(handle);
    return buffer ? buffer->shadow : nullptr;
}

static void gl_flush_buffer (R_Device* base_device, R_BufferHandle handle, u64 offset, u64 size)
{
    R_GLDevice*       device = gl_device(base_device);
    const R_GLBuffer* buffer = device->buffers.try_get(handle);
    if (!buffer || !buffer->shadow || offset > buffer->size || size > buffer->size - offset)
    {
        ASSERT_MSG(false, "invalid buffer flush");
        return;
    }
    if (!buffer->buffer || size == 0 || !enter(device))
    {
        return;
    }
    const R_GLFunctions& gl = device->gl;
    gl.glBindBuffer(GL_COPY_WRITE_BUFFER, buffer->buffer);
    gl.glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)offset, (GLsizeiptr)size, buffer->shadow + offset);
    gl.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    leave(device, "flush_buffer");
}

//
// Render passes and framebuffers
//

static R_RenderPassHandle gl_create_render_pass (R_Device* base_device, const R_CreateRenderPassInfo& info)
{
    R_GLDevice* device    = gl_device(base_device);
    const b8    has_color = info.color_format != R_Format::Unknown;
    const b8    has_depth = info.depth_format != R_Format::Unknown;
    if ((!has_color && !has_depth) || (has_color && (gl_internal_format(info.color_format) == 0 || r_format_is_depth(info.color_format))) || (has_depth && (!r_format_is_depth(info.depth_format) || gl_internal_format(info.depth_format) == 0)))
    {
        LOG_ERROR("Invalid render pass formats: color %u, depth %u", (u32)info.color_format, (u32)info.depth_format);
        return NULL_HANDLE;
    }
    return device->render_passes.add({ .format = info.color_format, .load_op = info.load_op, .depth_format = info.depth_format, .depth_load_op = info.depth_load_op });
}

static void gl_destroy_render_pass (R_Device* base_device, R_RenderPassHandle handle)
{
    gl_device(base_device)->render_passes.remove(handle);
}

static b8 gl_is_valid_render_pass (const R_Device* base_device, R_RenderPassHandle render_pass)
{
    return gl_device(base_device)->render_passes.is_valid(render_pass);
}

static R_FramebufferHandle gl_create_framebuffer (R_Device* base_device, const R_CreateFramebufferInfo& info)
{
    R_GLDevice*           device = gl_device(base_device);
    const R_GLRenderPass* pass   = device->render_passes.try_get(info.render_pass);
    const R_GLImage*      color  = device->images.try_get(info.image);
    const R_GLImage*      depth  = device->images.try_get(info.depth_image);
    const R_GLImage*      sized  = color ? color : depth;
    const b8              wants_color = pass && pass->format != R_Format::Unknown;
    const b8              wants_depth = pass && pass->depth_format != R_Format::Unknown;
    if (!pass || !sized || wants_color != (color != nullptr) || wants_depth != (depth != nullptr) || (color && (info.layer >= color->layers || color->format != pass->format)) ||
        (depth && (info.depth_layer >= depth->layers || depth->format != pass->depth_format)) || (color && depth && (color->width != depth->width || color->height != depth->height)))
    {
        LOG_ERROR("Invalid framebuffer");
        return NULL_HANDLE;
    }
    if (!enter(device))
    {
        return NULL_HANDLE;
    }
    const R_GLFunctions& gl          = device->gl;
    R_GLFramebuffer      framebuffer = { .width = sized->width, .height = sized->height, .render_pass = info.render_pass };
    gl.glGenFramebuffers(1, &framebuffer.framebuffer);
    gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer.framebuffer);
    if (color)
    {
        attach(device, GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, info.layer);
    }
    if (depth)
    {
        attach(device, GL_DRAW_FRAMEBUFFER, depth_attachment(depth->format), depth, info.depth_layer);
    }
    const GLenum buffer = color ? GL_COLOR_ATTACHMENT0 : GL_NONE;
    gl.glDrawBuffers(1, &buffer);
    const b8 complete = gl.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (!complete)
    {
        LOG_ERROR("The GL framebuffer is incomplete");
        gl.glDeleteFramebuffers(1, &framebuffer.framebuffer);
        leave(device, "create_framebuffer");
        return NULL_HANDLE;
    }
    leave(device, "create_framebuffer");
    return device->framebuffers.add(framebuffer);
}

static void gl_destroy_framebuffer (R_Device* base_device, R_FramebufferHandle handle)
{
    R_GLDevice*            device      = gl_device(base_device);
    const R_GLFramebuffer* framebuffer = device->framebuffers.try_get(handle);
    if (!framebuffer)
    {
        return;
    }
    if (enter(device))
    {
        device->gl.glDeleteFramebuffers(1, &framebuffer->framebuffer);
        leave(device, "destroy_framebuffer");
    }
    device->framebuffers.remove(handle);
}

static b8 gl_is_valid_framebuffer (const R_Device* base_device, R_FramebufferHandle framebuffer)
{
    return gl_device(base_device)->framebuffers.is_valid(framebuffer);
}

//
// Pipelines
//

static R_PipelineHandle gl_create_pipeline (R_Device* base_device, const R_CreatePipelineInfo& info)
{
    R_GLDevice*           device = gl_device(base_device);
    const R_GLRenderPass* pass   = device->render_passes.try_get(info.render_pass);
    if (!pass || info.push_constants_size > R_MAX_PUSH_CONSTANTS || info.vertex_attributes_count > R_MAX_VERTEX_ATTRIBUTES || info.textures_count > R_MAX_TEXTURES ||
        ((info.depth_test || info.depth_write) && pass->depth_format == R_Format::Unknown))
    {
        LOG_ERROR("Invalid pipeline info");
        return NULL_HANDLE;
    }
    if (!info.vertex || !info.vertex->glsl || !info.fragment || !info.fragment->glsl)
    {
        LOG_ERROR("Shader without GLSL");
        return NULL_HANDLE;
    }
    for (u32 i = 0; i < info.vertex_attributes_count; i++)
    {
        if ((u32)info.vertex_attributes[i].format >= ARRAY_SIZE(r_gl_formats) || r_gl_formats[(u32)info.vertex_attributes[i].format].components == 0 || info.vertex_attributes[i].location >= 32)
        {
            LOG_ERROR("Invalid vertex attribute %u", i);
            return NULL_HANDLE;
        }
    }
    if (!enter(device))
    {
        return NULL_HANDLE;
    }
    R_GLPipeline pipeline = {
        .textures_count          = info.textures_count,
        .texture_type            = info.texture_type,
        .push_constants_size     = info.push_constants_size,
        .has_color               = pass->format != R_Format::Unknown && !info.no_color_write,
        .blend_enable            = info.blend_enable,
        .depth_test              = info.depth_test,
        .depth_compare           = info.depth_compare,
        .depth_write             = info.depth_write,
        .vertex_stride           = info.vertex_stride,
        .vertex_attributes_count = info.vertex_attributes_count,
    };
    memory_copy(pipeline.vertex_attributes, info.vertex_attributes, sizeof(pipeline.vertex_attributes));
    r_pipeline_blend_factors(info, pipeline.blend_factors);
    const u32 variants = info.texture_type == R_TextureType::Type2DArray && info.textures_count > 0 ? (u32)R_GLSampler::Count : 1;
    b8        linked   = true;
    for (u32 i = 0; i < variants && linked; i++)
    {
        pipeline.programs[i] = link_program(device, info, (R_GLSampler)i);
        linked               = pipeline.programs[i] != 0;
    }
    if (!linked)
    {
        destroy_pipeline_resource(device, pipeline);
        leave(device, "create_pipeline");
        return NULL_HANDLE;
    }
    leave(device, "create_pipeline");
    return device->pipelines.add(pipeline);
}

static void gl_destroy_pipeline (R_Device* base_device, R_PipelineHandle handle)
{
    R_GLDevice*         device   = gl_device(base_device);
    const R_GLPipeline* pipeline = device->pipelines.try_get(handle);
    if (!pipeline)
    {
        return;
    }
    if (enter(device))
    {
        destroy_pipeline_resource(device, *pipeline);
        leave(device, "destroy_pipeline");
    }
    device->pipelines.remove(handle);
}

static b8 gl_is_valid_pipeline (const R_Device* base_device, R_PipelineHandle pipeline)
{
    return gl_device(base_device)->pipelines.is_valid(pipeline);
}

//
// Window presentation
//

static b8 gl_device_supports_present (const R_Device* base_device, const R_WindowTarget& target)
{
    const R_GLDevice* device = gl_device(base_device);
    if (device->info.platform == R_GLPlatform::WGL)
    {
        return target.native_display != nullptr;
    }
    return target.native_window != 0 && (!target.wayland || device->info.platform == R_GLPlatform::EGL);
}

static R_WindowSwapchainHandle gl_create_window_swapchain (R_Device* base_device, const R_WindowTarget& target, u32 width, u32 height)
{
    R_GLDevice* device = gl_device(base_device);
    if (!gl_device_supports_present(base_device, target) || width == 0 || height == 0)
    {
        LOG_WARNING("Cannot present to this window");
        return NULL_HANDLE;
    }
    R_GLWindowSwapchain window = { .width = width, .height = height };
    if (device->info.platform == R_GLPlatform::GLX)
    {
        window.drawable = create_glx_window(device, target.native_window);
    }
    else if (device->info.platform == R_GLPlatform::WGL)
    {
        window.drawable = match_pixel_format(device, target.native_display) ? (u64)(uintptr_t)target.native_display : 0;
    }
    else
    {
        uintptr_t native = (uintptr_t)target.native_window;
        if (target.wayland)
        {
            OS_Library* library = os_library_open("libwayland-egl.so.1");
            void* (*create) (void* surface, i32 width, i32 height) = (void* (*)(void*, i32, i32))os_library_symbol(library, "wl_egl_window_create");
            window.egl_window = create ? create((void*)native, (i32)width, (i32)height) : nullptr;
            native            = (uintptr_t)window.egl_window;
        }
        // GLES has no switch for sRGB writes: the window must be an sRGB one (EGL_KHR_gl_colorspace)
        const b8     srgb         = device->es && egl_has_extension(device->gl.eglQueryString(device->info.display, EGL_EXTENSIONS), "EGL_KHR_gl_colorspace");
        const EGLint attributes[] = { srgb ? EGL_GL_COLORSPACE_KHR : EGL_NONE, EGL_GL_COLORSPACE_SRGB_KHR, EGL_NONE };
        window.drawable           = native ? (u64)(uintptr_t)device->gl.eglCreateWindowSurface(device->info.display, (EGLConfig)device->info.config, native, attributes) : 0;
    }
    window.image = r_create_image(base_device, { .width = width, .height = height, .format = R_Format::R8G8B8A8_SRGB, .usage_flags = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::TransferSrc | (u32)R_ImageUsageFlags::Sampled });
    if (!window.drawable || !window.image)
    {
        LOG_WARNING("Cannot make a GL drawable of the window; the mirror window stays blank");
        destroy_window_resource(device, window);
        return NULL_HANDLE;
    }

    // The mirror never waits for the display, so it never paces the app
    if (enter(device))
    {
        if (make_current(device, device->info.display, window.drawable, window.drawable, device->context))
        {
            if (device->info.platform == R_GLPlatform::GLX && device->gl.glXSwapIntervalEXT)
            {
                device->gl.glXSwapIntervalEXT(device->info.display, window.drawable, 0);
            }
            else if (device->info.platform == R_GLPlatform::WGL && device->gl.wglSwapIntervalEXT)
            {
                device->gl.wglSwapIntervalEXT(0);
            }
            else if (device->info.platform == R_GLPlatform::EGL)
            {
                device->gl.eglSwapInterval(device->info.display, 0);
            }
            make_current(device, device->info.display, device->drawable, device->drawable, device->context);
        }
        leave(device, "create_window_swapchain");
    }
    return device->windows.add(window);
}

static void gl_destroy_window_swapchain (R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    R_GLDevice*          device = gl_device(base_device);
    R_GLWindowSwapchain* window = device->windows.try_get(swapchain);
    if (!window)
    {
        return;
    }
    destroy_window_resource(device, *window);
    device->windows.remove(swapchain);
}

static b8 gl_is_valid_window_swapchain (const R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    return gl_device(base_device)->windows.is_valid(swapchain);
}

static R_Format gl_window_swapchain_format (const R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    return gl_device(base_device)->windows.is_valid(swapchain) ? R_Format::R8G8B8A8_SRGB : R_Format::Unknown;
}

static void gl_window_swapchain_size (const R_Device* base_device, R_WindowSwapchainHandle swapchain, u32& out_width, u32& out_height)
{
    const R_GLWindowSwapchain* window = gl_device(base_device)->windows.try_get(swapchain);
    out_width  = window ? window->width : 0;
    out_height = window ? window->height : 0;
}

static b8 gl_window_acquire (R_Device* base_device, R_WindowSwapchainHandle handle, R_ImageHandle& out_image)
{
    const R_GLWindowSwapchain* window = gl_device(base_device)->windows.try_get(handle);
    out_image                         = window ? window->image : R_ImageHandle{};
    return window != nullptr;
}

// The image has Vulkan's row order, the window GL's: the blit flips it
static b8 gl_window_present (R_Device* base_device, R_WindowSwapchainHandle handle)
{
    R_GLDevice*                device = gl_device(base_device);
    const R_GLWindowSwapchain* window = device->windows.try_get(handle);
    const R_GLImage*           image  = window ? device->images.try_get(window->image) : nullptr;
    if (!image || !enter(device))
    {
        return false;
    }
    const R_GLFunctions& gl      = device->gl;
    b8                   present = make_current(device, device->info.display, window->drawable, window->drawable, device->context) && bind_scratch(device, GL_READ_FRAMEBUFFER, *image, 0);
    if (present)
    {
        gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        gl.glDisable(GL_SCISSOR_TEST);
        if (!device->es)
        {
            // The window is no sRGB framebuffer: copy the encoded values as they are
            gl.glDisable(GL_FRAMEBUFFER_SRGB);
        }
        gl.glBlitFramebuffer(0, 0, (GLint)window->width, (GLint)window->height, 0, (GLint)window->height, (GLint)window->width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (!device->es)
        {
            gl.glEnable(GL_FRAMEBUFFER_SRGB);
        }
        gl.glEnable(GL_SCISSOR_TEST);
        gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        if (device->info.platform == R_GLPlatform::GLX)
        {
            gl.glXSwapBuffers(device->info.display, window->drawable);
        }
        else if (device->info.platform == R_GLPlatform::WGL)
        {
            present = gl.SwapBuffers((WGLDC)(uintptr_t)window->drawable) != 0;
        }
        else
        {
            present = gl.eglSwapBuffers(device->info.display, (EGLSurface)(uintptr_t)window->drawable) != 0;
        }
        device->queue_accesses++;
    }
    make_current(device, device->info.display, device->drawable, device->drawable, device->context);
    leave(device, "window_present");
    return present;
}

//
// Frames and immediate commands
//

static R_CmdBufferHandle begin_recording (R_GLDevice* device, u32 slot)
{
    if (device->recording[slot])
    {
        ASSERT_MSG(false, "command buffer already recording");
        return NULL_HANDLE;
    }
    if (!enter(device))
    {
        return NULL_HANDLE;
    }
    device->recording[slot] = true;
    device->bound_pipeline  = NULL_HANDLE;
    device->vertex_buffer   = NULL_HANDLE;
    device->index_buffer    = NULL_HANDLE;
    return device->cmd_handles[slot];
}

static void end_recording (R_GLDevice* device, R_CmdBufferHandle cmd, const char* what)
{
    const R_GLCmdBuffer* buffer = device->cmds.try_get(cmd);
    if (!buffer || !device->recording[buffer->frame])
    {
        ASSERT_MSG(false, "command buffer not recording");
        return;
    }
    device->recording[buffer->frame] = false;
    device->queue_accesses++;
    leave(device, what);
}

static R_CmdBufferHandle gl_begin_frame (R_Device* base_device)
{
    R_GLDevice* device  = gl_device(base_device);
    const u32   slot    = device->frame_index;
    device->frame_index = (device->frame_index + 1) % R_MAX_FRAMES_IN_FLIGHT;
    return begin_recording(device, slot);
}

static void gl_end_frame (R_Device* base_device, R_CmdBufferHandle cmd)
{
    end_recording(gl_device(base_device), cmd, "end_frame");
}

static void gl_end_frame_present (R_Device* base_device, R_CmdBufferHandle cmd, R_WindowSwapchainHandle swapchain)
{
    (void)swapchain;
    end_recording(gl_device(base_device), cmd, "end_frame_present");
}

static R_CmdBufferHandle gl_begin_immediate (R_Device* base_device)
{
    return begin_recording(gl_device(base_device), R_MAX_FRAMES_IN_FLIGHT);
}

static void gl_end_immediate (R_Device* base_device, R_CmdBufferHandle cmd)
{
    end_recording(gl_device(base_device), cmd, "end_immediate");
}

static u64 gl_get_cmd_native (const R_Device* base_device, R_CmdBufferHandle cmd)
{
    (void)base_device;
    (void)cmd;
    return 0;
}

//
// Commands
//

static void gl_cmd_transition_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle handle, R_ImageLayout old_layout, R_ImageLayout new_layout)
{
    const R_GLDevice* device = gl_device(base_device);
    (void)old_layout;
    (void)new_layout;
    if (!cmd_recording(device, cmd) || !device->images.is_valid(handle))
    {
        ASSERT_MSG(false, "invalid transition");
    }
}

static void set_scissor (R_GLDevice* device, const R_Rect& rect)
{
    device->scissor = rect;
    device->gl.glScissor(rect.x, rect.y, (GLsizei)rect.width, (GLsizei)rect.height);
}

static void gl_cmd_begin_render_pass (R_Device* base_device, R_CmdBufferHandle cmd, R_RenderPassHandle pass_handle, R_FramebufferHandle fb_handle, const R_ClearColor& clear)
{
    R_GLDevice*            device = gl_device(base_device);
    const R_GLRenderPass*  pass   = device->render_passes.try_get(pass_handle);
    const R_GLFramebuffer* fb     = device->framebuffers.try_get(fb_handle);
    if (!cmd_recording(device, cmd) || !pass || !fb)
    {
        ASSERT_MSG(false, "invalid render pass or framebuffer");
        return;
    }
    const R_GLFunctions& gl = device->gl;
    device->framebuffer     = fb_handle;
    gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb->framebuffer);
    gl.glViewport(0, 0, (GLsizei)fb->width, (GLsizei)fb->height);
    gl.glEnable(GL_SCISSOR_TEST);
    set_scissor(device, { 0, 0, fb->width, fb->height });
    if (pass->format != R_Format::Unknown && pass->load_op == R_LoadOp::Clear)
    {
        const GLfloat color[4] = { clear.r, clear.g, clear.b, clear.a };
        gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.glClearBufferfv(GL_COLOR, 0, color);
    }
    if (pass->depth_format != R_Format::Unknown && pass->depth_load_op == R_LoadOp::Clear)
    {
        const GLfloat one = 1.0f;
        gl.glDepthMask(GL_TRUE);
        gl.glClearBufferfv(GL_DEPTH, 0, &one);
    }
    const R_GLPipeline* pipeline = device->pipelines.try_get(device->bound_pipeline);
    if (pipeline)
    {
        apply_pipeline_state(device, *pipeline);
    }
}

static void gl_cmd_end_render_pass (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_GLDevice* device = gl_device(base_device);
    if (!cmd_recording(device, cmd))
    {
        ASSERT_MSG(false, "invalid command buffer");
        return;
    }
    device->framebuffer = NULL_HANDLE;
    device->gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

static void gl_cmd_bind_pipeline (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle handle)
{
    R_GLDevice*         device   = gl_device(base_device);
    const R_GLPipeline* pipeline = device->pipelines.try_get(handle);
    if (!cmd_recording(device, cmd) || !pipeline)
    {
        ASSERT_MSG(false, "invalid pipeline binding");
        return;
    }
    device->bound_pipeline = handle;
    apply_pipeline_state(device, *pipeline);
}

static void gl_cmd_bind_textures (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle pipeline_handle, const R_ImageHandle* images, u32 count, R_Filter filter)
{
    R_GLDevice*         device   = gl_device(base_device);
    const R_GLPipeline* pipeline = device->pipelines.try_get(pipeline_handle);
    if (!cmd_recording(device, cmd) || !pipeline || !images || count == 0 || count != pipeline->textures_count)
    {
        ASSERT_MSG(false, "invalid texture binding");
        return;
    }
    const R_GLFunctions& gl = device->gl;
    for (u32 i = 0; i < count; i++)
    {
        const R_GLImage* image = device->images.try_get(images[i]);
        if (!image)
        {
            ASSERT_MSG(false, "invalid texture binding");
            return;
        }
        gl.glActiveTexture(GL_TEXTURE0 + i);
        gl.glBindTexture(image->target, image->texture);
        gl.glBindSampler(i, device->samplers[filter == R_Filter::Linear ? 1 : 0]);
        device->bound_targets[i] = image->target;
    }
    gl.glActiveTexture(GL_TEXTURE0);
}

static void gl_cmd_clear_depth (R_Device* base_device, R_CmdBufferHandle cmd, const R_Rect& rect, f32 depth)
{
    R_GLDevice* device = gl_device(base_device);
    if (!cmd_recording(device, cmd) || !device->framebuffers.is_valid(device->framebuffer))
    {
        ASSERT_MSG(false, "depth clear outside a render pass");
        return;
    }
    const R_GLFunctions& gl     = device->gl;
    const R_Rect         saved  = device->scissor;
    const GLfloat        value  = depth;
    set_scissor(device, rect);
    gl.glDepthMask(GL_TRUE);
    gl.glClearBufferfv(GL_DEPTH, 0, &value);
    set_scissor(device, saved);
    const R_GLPipeline* pipeline = device->pipelines.try_get(device->bound_pipeline);
    if (pipeline)
    {
        gl.glDepthMask(pipeline->depth_write ? GL_TRUE : GL_FALSE);
    }
}

static void gl_cmd_push_constants (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle handle, const void* data, u32 size)
{
    R_GLDevice*         device   = gl_device(base_device);
    const R_GLPipeline* pipeline = device->pipelines.try_get(handle);
    if (!cmd_recording(device, cmd) || !pipeline || !data || size == 0 || size > pipeline->push_constants_size)
    {
        ASSERT_MSG(false, "invalid push constants");
        return;
    }
    const R_GLFunctions& gl = device->gl;
    gl.glBindBuffer(GL_UNIFORM_BUFFER, device->push_buffer);
    gl.glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)size, data);
}

static void gl_cmd_set_viewport (R_Device* base_device, R_CmdBufferHandle cmd, const R_Viewport& viewport)
{
    const R_GLDevice* device = gl_device(base_device);
    if (!cmd_recording(device, cmd))
    {
        ASSERT_MSG(false, "invalid command buffer");
        return;
    }
    device->gl.glViewport((GLint)viewport.x, (GLint)viewport.y, (GLsizei)viewport.width, (GLsizei)viewport.height);
}

static void gl_cmd_set_scissor (R_Device* base_device, R_CmdBufferHandle cmd, const R_Rect& scissor)
{
    R_GLDevice* device = gl_device(base_device);
    if (!cmd_recording(device, cmd))
    {
        ASSERT_MSG(false, "invalid command buffer");
        return;
    }
    set_scissor(device, scissor);
}

static void gl_cmd_bind_vertex_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset)
{
    R_GLDevice* device = gl_device(base_device);
    if (!cmd_recording(device, cmd) || !device->buffers.is_valid(handle))
    {
        ASSERT_MSG(false, "invalid vertex buffer binding");
        return;
    }
    device->vertex_buffer = handle;
    device->vertex_offset = offset;
}

static void gl_cmd_bind_index_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset, b8 u16_indices)
{
    R_GLDevice* device = gl_device(base_device);
    if (!cmd_recording(device, cmd) || !device->buffers.is_valid(handle))
    {
        ASSERT_MSG(false, "invalid index buffer binding");
        return;
    }
    device->index_buffer = handle;
    device->index_offset = offset;
    device->index_u16    = u16_indices;
}

static void gl_cmd_draw (R_Device* base_device, R_CmdBufferHandle cmd, u32 vertex_count, u32 first_vertex)
{
    R_GLDevice* device = gl_device(base_device);
    if (prepare_draw(device, cmd, 0))
    {
        device->gl.glDrawArrays(GL_TRIANGLES, (GLint)first_vertex, (GLsizei)vertex_count);
    }
}

static void gl_cmd_draw_indexed (R_Device* base_device, R_CmdBufferHandle cmd, u32 index_count, u32 first_index, i32 vertex_offset)
{
    R_GLDevice*       device = gl_device(base_device);
    const R_GLBuffer* buffer = device->buffers.try_get(device->index_buffer);
    if (!buffer || !buffer->buffer)
    {
        ASSERT_MSG(false, "indexed draw without an index buffer");
        return;
    }
    if (prepare_draw(device, cmd, vertex_offset))
    {
        const u64 index_size = device->index_u16 ? 2 : 4;
        device->gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer->buffer);
        device->gl.glDrawElements(GL_TRIANGLES, (GLsizei)index_count, device->index_u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, (const void*)(uintptr_t)(device->index_offset + first_index * index_size));
    }
}

static void gl_cmd_copy_buffer_to_image (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle buffer_handle, R_ImageHandle image_handle, u32 layer)
{
    R_GLDevice*       device = gl_device(base_device);
    const R_GLBuffer* buffer = device->buffers.try_get(buffer_handle);
    const R_GLImage*  image  = device->images.try_get(image_handle);
    if (!cmd_recording(device, cmd) || !buffer || !buffer->shadow || !image || layer >= image->layers || r_format_is_depth(image->format) || buffer->size < (u64)image->width * image->height * r_format_bytes_per_pixel(image->format))
    {
        ASSERT_MSG(false, "invalid buffer to image copy");
        return;
    }
    const R_GLFunctions& gl     = device->gl;
    const auto&          format = r_gl_formats[(u32)image->format];
    gl.glBindTexture(image->target, image->texture);
    switch (image->target)
    {
    case GL_TEXTURE_2D:
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)image->width, (GLsizei)image->height, format.pixel_format, format.pixel_type, buffer->shadow);
        break;
    case GL_TEXTURE_CUBE_MAP:
        gl.glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, 0, 0, 0, (GLsizei)image->width, (GLsizei)image->height, format.pixel_format, format.pixel_type, buffer->shadow);
        break;
    default:
        gl.glTexSubImage3D(image->target, 0, 0, 0, (GLint)layer, (GLsizei)image->width, (GLsizei)image->height, 1, format.pixel_format, format.pixel_type, buffer->shadow);
        break;
    }
    gl.glBindTexture(image->target, 0);
}

// ES 3.0 reads colour only (OpenGL ES 3.0, 4.3.2)
static b8 read_layer (const R_GLDevice* device, const R_GLImage& image, u32 layer, void* out_pixels)
{
    if (device->es && r_format_is_depth(image.format))
    {
        LOG_WARNING("OpenGL ES cannot read depth images back");
        return false;
    }
    if (!bind_scratch(device, GL_READ_FRAMEBUFFER, image, layer))
    {
        LOG_ERROR("Cannot read this image back");
        return false;
    }
    const auto& format = r_gl_formats[(u32)image.format];
    device->gl.glReadPixels(0, 0, (GLsizei)image.width, (GLsizei)image.height, format.pixel_format, format.pixel_type, out_pixels);
    device->gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return true;
}

static void gl_cmd_copy_image_to_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle image_handle, u32 layer, R_BufferHandle buffer_handle)
{
    R_GLDevice*       device = gl_device(base_device);
    const R_GLBuffer* buffer = device->buffers.try_get(buffer_handle);
    const R_GLImage*  image  = device->images.try_get(image_handle);
    if (!cmd_recording(device, cmd) || !buffer || !buffer->shadow || !image || layer >= image->layers || buffer->size < (u64)image->width * image->height * r_format_bytes_per_pixel(image->format))
    {
        ASSERT_MSG(false, "invalid image to buffer copy");
        return;
    }
    read_layer(device, *image, layer, buffer->shadow);
}

static void gl_cmd_clear_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle handle, const R_ClearColor& color)
{
    R_GLDevice*      device = gl_device(base_device);
    const R_GLImage* image  = device->images.try_get(handle);
    if (!cmd_recording(device, cmd) || !image || r_format_is_depth(image->format))
    {
        ASSERT_MSG(false, "invalid clear");
        return;
    }
    const R_GLFunctions& gl    = device->gl;
    const GLfloat        value[4] = { color.r, color.g, color.b, color.a };
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (u32 layer = 0; layer < image->layers; layer++)
    {
        if (bind_scratch(device, GL_DRAW_FRAMEBUFFER, *image, layer))
        {
            gl.glClearBufferfv(GL_COLOR, 0, value);
        }
    }
    gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl.glEnable(GL_SCISSOR_TEST);
}

static void gl_cmd_blit_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle src_handle, u32 src_layer, R_ImageHandle dst_handle, const R_Rect& dst_rect, R_Filter filter)
{
    R_GLDevice*      device = gl_device(base_device);
    const R_GLImage* src    = device->images.try_get(src_handle);
    const R_GLImage* dst    = device->images.try_get(dst_handle);
    if (!cmd_recording(device, cmd) || !src || !dst || src_layer >= src->layers || r_format_is_depth(src->format) || r_format_is_depth(dst->format))
    {
        ASSERT_MSG(false, "invalid blit");
        return;
    }
    const R_GLFunctions& gl = device->gl;
    if (!bind_scratch(device, GL_READ_FRAMEBUFFER, *src, src_layer) || !bind_scratch(device, GL_DRAW_FRAMEBUFFER, *dst, 0))
    {
        LOG_ERROR("Cannot blit between these images");
        return;
    }
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glBlitFramebuffer(0, 0, (GLint)src->width, (GLint)src->height, dst_rect.x, dst_rect.y, dst_rect.x + (GLint)dst_rect.width, dst_rect.y + (GLint)dst_rect.height, GL_COLOR_BUFFER_BIT, filter == R_Filter::Linear ? GL_LINEAR : GL_NEAREST);
    gl.glEnable(GL_SCISSOR_TEST);
    gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

static b8 gl_read_image (R_Device* base_device, R_ImageHandle handle, u32 layer, R_ImageLayout layout, void* out_pixels, usize out_size)
{
    R_GLDevice*      device = gl_device(base_device);
    const R_GLImage* image  = device->images.try_get(handle);
    (void)layout;
    if (!image || !out_pixels || layer >= image->layers || out_size < (usize)image->width * image->height * r_format_bytes_per_pixel(image->format))
    {
        return false;
    }
    if (!enter(device))
    {
        return false;
    }
    const b8 read = read_layer(device, *image, layer, out_pixels);
    leave(device, "read_image");
    return read;
}

//
// Backend
//

const static R_BackendFns gl_fns = {
    .destroy_device            = gl_destroy_device,
    .wait_idle                 = gl_wait_idle,
    .flush_initial_layouts     = gl_flush_initial_layouts,
    .queue_access_count        = gl_queue_access_count,
    .native_format             = gl_native_format,
    .format_from_native        = gl_format_from_native,
    .x11_visual                = gl_x11_visual,
    .create_image              = gl_create_image,
    .image_flags_supported     = gl_image_flags_supported,
    .import_image              = gl_import_image,
    .destroy_image             = gl_destroy_image,
    .is_valid_image            = gl_is_valid_image,
    .get_image_native          = gl_get_image_native,
    .get_image_size            = gl_get_image_size,
    .get_image_format          = gl_get_image_format,
    .create_buffer             = gl_create_buffer,
    .destroy_buffer            = gl_destroy_buffer,
    .is_valid_buffer           = gl_is_valid_buffer,
    .get_buffer_mapped_data    = gl_get_buffer_mapped_data,
    .flush_buffer              = gl_flush_buffer,
    .create_render_pass        = gl_create_render_pass,
    .destroy_render_pass       = gl_destroy_render_pass,
    .is_valid_render_pass      = gl_is_valid_render_pass,
    .create_framebuffer        = gl_create_framebuffer,
    .destroy_framebuffer       = gl_destroy_framebuffer,
    .is_valid_framebuffer      = gl_is_valid_framebuffer,
    .create_pipeline           = gl_create_pipeline,
    .destroy_pipeline          = gl_destroy_pipeline,
    .is_valid_pipeline         = gl_is_valid_pipeline,
    .device_supports_present   = gl_device_supports_present,
    .create_window_swapchain   = gl_create_window_swapchain,
    .destroy_window_swapchain  = gl_destroy_window_swapchain,
    .is_valid_window_swapchain = gl_is_valid_window_swapchain,
    .window_swapchain_format   = gl_window_swapchain_format,
    .window_swapchain_size     = gl_window_swapchain_size,
    .window_acquire            = gl_window_acquire,
    .window_present            = gl_window_present,
    .begin_frame               = gl_begin_frame,
    .end_frame                 = gl_end_frame,
    .end_frame_present         = gl_end_frame_present,
    .begin_immediate           = gl_begin_immediate,
    .end_immediate             = gl_end_immediate,
    .get_cmd_native            = gl_get_cmd_native,
    .cmd_transition_image      = gl_cmd_transition_image,
    .cmd_begin_render_pass     = gl_cmd_begin_render_pass,
    .cmd_end_render_pass       = gl_cmd_end_render_pass,
    .cmd_bind_pipeline         = gl_cmd_bind_pipeline,
    .cmd_bind_textures         = gl_cmd_bind_textures,
    .cmd_clear_depth           = gl_cmd_clear_depth,
    .cmd_push_constants        = gl_cmd_push_constants,
    .cmd_set_viewport          = gl_cmd_set_viewport,
    .cmd_set_scissor           = gl_cmd_set_scissor,
    .cmd_bind_vertex_buffer    = gl_cmd_bind_vertex_buffer,
    .cmd_bind_index_buffer     = gl_cmd_bind_index_buffer,
    .cmd_draw                  = gl_cmd_draw,
    .cmd_draw_indexed          = gl_cmd_draw_indexed,
    .cmd_copy_buffer_to_image  = gl_cmd_copy_buffer_to_image,
    .cmd_copy_image_to_buffer  = gl_cmd_copy_image_to_buffer,
    .cmd_clear_image           = gl_cmd_clear_image,
    .cmd_blit_image            = gl_cmd_blit_image,
    .read_image                = gl_read_image,
};

R_Device* r_gl_create_device (const R_DeviceCreateInfo& create_info)
{
    const R_GLDeviceInfo& info = create_info.gl;
    if (info.platform == R_GLPlatform::WGL ? (!info.context || !info.drawable || create_info.backend != R_Backend::OpenGL) : (!info.display || !info.context || (info.platform == R_GLPlatform::GLX && !info.drawable)))
    {
        LOG_ERROR("Render device needs the app's display and context, on GLX its drawable, on WGL its device context and no GLES");
        return nullptr;
    }
    R_GLDevice* device = (R_GLDevice*)os_memory_alloc(sizeof(R_GLDevice));
    memory_zero(device, sizeof(R_GLDevice));
    device->base = { .fns = &gl_fns, .backend = create_info.backend };
    device->info = info;
    device->es   = create_info.backend == R_Backend::OpenGLES;
    b8 success   = false;
    defer {
        if (!success)
        {
            r_destroy_device(&device->base);
        }
    };
    R_GLGetProcAddress* get_proc_address = find_get_proc_address(device);
    if (!get_proc_address)
    {
        LOG_ERROR("No GetProcAddress of the GL platform");
        return nullptr;
    }
    const WGLContext bootstrap = info.platform == R_GLPlatform::WGL ? begin_wgl_bootstrap(device, get_proc_address) : nullptr;
    b8               created   = load_functions(device, get_proc_address);
    if (created)
    {
        switch (info.platform)
        {
        case R_GLPlatform::EGL:
            created = create_egl_context(device);
            break;
        case R_GLPlatform::GLX:
            created = (device->info.config || resolve_glx_config(device)) && create_glx_context(device);
            break;
        case R_GLPlatform::WGL:
            created = create_wgl_context(device);
            break;
        }
    }
    end_wgl_bootstrap(device, bootstrap);
    if (!created || !enter(device))
    {
        return nullptr;
    }
    const b8 initialized = initialize_context(device);
    leave(device, "create_device");
    if (!initialized)
    {
        return nullptr;
    }
    for (u32 i = 0; i < ARRAY_SIZE(device->cmd_handles); i++)
    {
        device->cmd_handles[i] = device->cmds.add({ .frame = i });
    }
    success = true;
    return &device->base;
}
