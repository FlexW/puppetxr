#pragma once

#include "base.h"
#include "zmath.h"

#define R_MAX_FRAMES_IN_FLIGHT  2
#define R_MAX_IMAGE_LAYERS      256 // the minimum maxImageArrayLayers Vulkan guarantees
#define R_MAX_LAYER_VIEWS       2   // layers that get their own view, for framebuffers
#define R_MAX_VERTEX_ATTRIBUTES 4
#define R_MAX_PUSH_CONSTANTS    128
#define R_MAX_TEXTURES          2

struct R_Device;
struct R_Image;
struct R_Buffer;
struct R_Pipeline;
struct R_RenderPass;
struct R_Framebuffer;
struct R_CmdBuffer;
struct R_WindowSwapchain;

using R_ImageHandle       = Handle<R_Image>;
using R_BufferHandle      = Handle<R_Buffer>;
using R_PipelineHandle    = Handle<R_Pipeline>;
using R_RenderPassHandle  = Handle<R_RenderPass>;
using R_FramebufferHandle = Handle<R_Framebuffer>;
using R_CmdBufferHandle   = Handle<R_CmdBuffer>;
using R_WindowSwapchainHandle = Handle<R_WindowSwapchain>;

//
// Enums
//

enum class R_Backend
{
    Vulkan = 0,
    OpenGL,   // 3.3 core or later
    OpenGLES, // 3.0 or later
    D3D11,    // feature level 11_0 or later
    D3D12,    // feature level 11_0 or later
    Metal,    // macOS 11 or later
    Count,
};

enum class R_GLPlatform
{
    EGL = 0,
    GLX,
    WGL,
};

enum class R_Format
{
    Unknown = 0,
    R8G8B8A8_UNORM,
    B8G8R8A8_UNORM,
    R8G8B8A8_SRGB,
    B8G8R8A8_SRGB,
    R16G16B16A16_SFLOAT,
    R32G32_SFLOAT,
    R32G32B32_SFLOAT, // vertex attributes only
    D16_UNORM,
    D24_UNORM_S8_UINT,
    D32_SFLOAT,
    D32_SFLOAT_S8_UINT,
    Count,
};

enum class R_ImageUsageFlags : u32
{
    TransferSrc            = 1 << 0,
    TransferDst            = 1 << 1,
    Sampled                = 1 << 2,
    ColorAttachment        = 1 << 3,
    DepthStencilAttachment = 1 << 4,
    UnorderedAccess        = 1 << 5, // D3D color images only; Vulkan and GL ignore it
};

enum class R_BufferUsageFlags : u32
{
    TransferSrc = 1 << 0,
    TransferDst = 1 << 1,
    Vertex      = 1 << 2,
    Index       = 1 << 3,
    Uniform     = 1 << 4,
};

enum class R_ImageLayout
{
    Undefined = 0,
    General,
    ColorAttachment,
    DepthStencilAttachment,
    ShaderReadOnly,
    TransferSrc,
    TransferDst,
    Present,
};

enum class R_LoadOp
{
    DontCare = 0,
    Load,
    Clear,
};

enum class R_TextureType
{
    Type2D = 0,
    Type2DArray,
};

enum class R_Filter
{
    Nearest = 0,
    Linear,
};

enum class R_CompareOp
{
    LessOrEqual = 0,
    Always,
    Never,
    Less,
    Equal,
    Greater,
    NotEqual,
    GreaterOrEqual,
};

enum class R_BlendFactor
{
    Zero = 0,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
};

//
// Infos
//

// The app's Vulkan objects. The device never owns them.
struct R_VulkanDeviceInfo
{
    void* get_instance_proc_addr; // PFN_vkGetInstanceProcAddr, null means the system loader
    void* instance;
    void* physical_device;
    void* device;
    u32   queue_family_index;
    u32   queue_index;
};

// The app's OpenGL or OpenGL ES context, current on the calling thread whenever the device is used.
// The device makes a context of its own in the app's share group and never changes the app's.
struct R_GLDeviceInfo
{
    R_GLPlatform platform;
    void*        get_proc_address; // eglGetProcAddress, glXGetProcAddressARB or wglGetProcAddress; null looks it up in the process
    void*        display;          // EGLDisplay or Display*; null on WGL
    void*        config;           // EGLConfig or GLXFBConfig; null on WGL
    void*        context;          // EGLContext, GLXContext or HGLRC
    u64          drawable;         // GLXDrawable or HDC the app's context is current with; 0 on EGL
};

// The app's D3D11 device. Commands are recorded on deferred contexts and run on the immediate
// context only where render.h allows queue access; the app's context state is left as it was.
struct R_D3D11DeviceInfo
{
    void* device; // ID3D11Device*
};

// The app's Metal command queue; the device is the queue's
struct R_MetalDeviceInfo
{
    void* queue; // id<MTLCommandQueue>
};

// The app's D3D12 device and the queue it submits to
struct R_D3D12DeviceInfo
{
    void* device; // ID3D12Device*
    void* queue;  // ID3D12CommandQueue*, of type DIRECT
};

struct R_DeviceCreateInfo
{
    R_Backend          backend;
    R_VulkanDeviceInfo vulkan; // R_Backend::Vulkan
    R_GLDeviceInfo     gl;     // R_Backend::OpenGL and OpenGLES
    R_D3D11DeviceInfo  d3d11;  // R_Backend::D3D11
    R_D3D12DeviceInfo  d3d12;  // R_Backend::D3D12
    R_MetalDeviceInfo  metal;  // R_Backend::Metal
};

// One stage of a shader for every backend, generated per file of shaders/ as `<file>_shader` (docs/shaders.md)
struct R_ShaderCode
{
    const void* spirv;
    usize       spirv_size;
    const char* glsl;                     // without #version; the backend prepends it and its defines
    const void* dxbc;                     // shader model 5.0 bytecode for D3D11 and D3D12; null in builds without D3D
    usize       dxbc_size;
    const char* textures[R_MAX_TEXTURES]; // names by binding, for backends that bind by name
    const char* msl;                      // Metal Shading Language 2.3 with entry point main0; null in builds without Metal
};

// What a window swapchain presents to
struct R_WindowTarget
{
    u64   vk_surface;     // Vulkan: VkSurfaceKHR the platform layer created
    u64   native_window;  // GL: X11 Window on the app's display, wl_surface* when `wayland`, or HWND; D3D: HWND
    void* native_display; // WGL: the window's own HDC
    b8    wayland;
    void* metal_layer;    // Metal: the window's CAMetalLayer
};

struct R_CreateImageInfo
{
    u32           width;
    u32           height;
    u32           layers;     // 0 means 1
    u32           mip_levels; // 0 means 1; only level 0 is sampled and rendered to
    R_Format      format;
    u32           usage_flags; // R_ImageUsageFlags
    R_ImageLayout initial_layout; // applied at the start of the next recorded command buffer
    b8            cube_compatible; // layers are faces of cube maps; width equals height, layers a multiple of 6
    u32           vk_extra_create_flags; // VkImageCreateFlags added as they are, XR_META_vulkan_swapchain_create_info
    u32           vk_extra_usage_flags;  // VkImageUsageFlags
};

struct R_ImportImageInfo
{
    u64      native_image; // VkImage
    u32      width;
    u32      height;
    R_Format format;
};

struct R_CreateBufferInfo
{
    u64 size;
    u32 usage_flags; // R_BufferUsageFlags
    b8  host_visible;
};

// A color attachment, a depth attachment or both. Depth is cleared to 1 when its load op is Clear.
struct R_CreateRenderPassInfo
{
    R_Format      color_format; // Unknown for a depth-only pass
    R_LoadOp      load_op;
    R_ImageLayout initial_layout;
    R_ImageLayout final_layout;
    R_Format      depth_format; // Unknown for no depth attachment
    R_LoadOp      depth_load_op;
    R_ImageLayout depth_initial_layout;
    R_ImageLayout depth_final_layout;
};

struct R_CreateFramebufferInfo
{
    R_RenderPassHandle render_pass;
    R_ImageHandle      image; // color, null for a depth-only pass
    u32                layer = {};
    R_ImageHandle      depth_image;
    u32                depth_layer = {};
};

struct R_VertexAttribute
{
    u32      location;
    R_Format format;
    u32      offset;
};

struct R_CreatePipelineInfo
{
    const R_ShaderCode* vertex = {};
    const R_ShaderCode* fragment = {};
    R_RenderPassHandle render_pass;
    u32                textures_count = {}; // combined image samplers at bindings 0..count-1, up to R_MAX_TEXTURES
    R_TextureType      texture_type = {};
    u32                push_constants_size = {};
    b8                 blend_enable = {};
    b8                 blend_premultiplied = {}; // source color is already multiplied by its alpha
    b8                 blend_custom = {};        // blend_factors instead of the premultiplied or straight alpha ones
    R_BlendFactor      blend_factors[4] = {};    // source color, target color, source alpha, target alpha
    b8                 depth_test = {};          // needs a render pass with depth
    R_CompareOp        depth_compare = {};
    b8                 depth_write = {};
    b8                 no_color_write = {}; // depth only, the color attachment keeps its values
    u32                vertex_stride = {}; // 0 means no vertex buffer
    R_VertexAttribute  vertex_attributes[R_MAX_VERTEX_ATTRIBUTES] = {};
    u32                vertex_attributes_count = {};
};

struct R_Rect
{
    i32 x;
    i32 y;
    u32 width;
    u32 height;
};

struct R_Viewport
{
    f32 x;
    f32 y;
    f32 width;
    f32 height;
};

struct R_ClearColor
{
    f32 r;
    f32 g;
    f32 b;
    f32 a;
};

//
// Device
//

// The device never touches the app's queue except in r_end_frame, r_end_immediate,
// r_flush_initial_layouts and r_window_present, so the OpenXR layer can keep queue access
// inside the calls the spec allows. Waits use puppetxr's own fences, never vkQueueWaitIdle.
R_Device*           r_create_device              (const R_DeviceCreateInfo& info); // null when the backend is unknown or fails
b8                  r_backend_available          (R_Backend backend); // built into this library (D3D only on Windows)
b8                  r_d3d_adapter_luid           (u64& out_luid); // the adapter D3D apps should create their device on: DXGI's first; false without D3D
u64                 r_d3d_device_luid            (const R_Device* device); // the adapter of a D3D11 or D3D12 device, 0 otherwise
void*               r_metal_default_device       (); // id<MTLDevice> Metal apps should make their queue on, the system default; null without Metal
void                r_destroy_device             (R_Device* device);
R_Backend           r_device_backend             (const R_Device* device);
u32                 r_gl_x11_visual              (const R_Device* device); // GLX: the visual of the app's config, which a window needs to show the app's drawables; 0 otherwise
void                r_wait_idle                  (const R_Device* device); // waits for puppetxr's submitted work only
void                r_flush_initial_layouts      (R_Device* device); // submits pending initial layout transitions now
u64                 r_queue_access_count         (const R_Device* device); // submits and presents so far, for tests
u32                 r_format_bytes_per_pixel     (R_Format format);
b8                  r_format_is_depth            (R_Format format);
b8                  r_format_has_stencil         (R_Format format);
i64                 r_native_format              (const R_Device* device, R_Format format); // the format enum of the backend's API (VkFormat, GL internal format, DXGI_FORMAT), 0 when unknown
R_Format            r_format_from_native         (const R_Device* device, i64 format);      // Unknown when unsupported

//
// Resources
//

R_ImageHandle       r_create_image               (R_Device* device, const R_CreateImageInfo& info);
R_ImageHandle       r_import_image               (R_Device* device, const R_ImportImageInfo& info);
void                r_destroy_image              (R_Device* device, R_ImageHandle handle);
b8                  r_is_valid                   (const R_Device* device, R_ImageHandle image);
u64                 r_get_image_native           (const R_Device* device, R_ImageHandle handle); // VkImage, GL texture name, ID3D11Texture2D* or ID3D12Resource*
void                r_get_image_size             (const R_Device* device, R_ImageHandle handle, u32& out_width, u32& out_height, u32& out_layers);
R_Format            r_get_image_format           (const R_Device* device, R_ImageHandle handle);

R_BufferHandle      r_create_buffer              (R_Device* device, const R_CreateBufferInfo& info);
void                r_destroy_buffer             (R_Device* device, R_BufferHandle handle);
b8                  r_is_valid                   (const R_Device* device, R_BufferHandle buffer);
void*               r_get_buffer_mapped_data     (const R_Device* device, R_BufferHandle handle); // host visible buffers only
void                r_flush_buffer               (R_Device* device, R_BufferHandle handle, u64 offset, u64 size); // after writing mapped data, before recording commands that read it

R_RenderPassHandle  r_create_render_pass         (R_Device* device, const R_CreateRenderPassInfo& info);
void                r_destroy_render_pass        (R_Device* device, R_RenderPassHandle handle);
b8                  r_is_valid                   (const R_Device* device, R_RenderPassHandle render_pass);

R_FramebufferHandle r_create_framebuffer         (R_Device* device, const R_CreateFramebufferInfo& info);
void                r_destroy_framebuffer        (R_Device* device, R_FramebufferHandle handle);
b8                  r_is_valid                   (const R_Device* device, R_FramebufferHandle framebuffer);

R_PipelineHandle    r_create_pipeline            (R_Device* device, const R_CreatePipelineInfo& info);
void                r_destroy_pipeline           (R_Device* device, R_PipelineHandle handle);
b8                  r_is_valid                   (const R_Device* device, R_PipelineHandle pipeline);

//
// Window presentation
//
// A window swapchain presents to a window of the platform layer (a VkSwapchainKHR on Vulkan, a
// window drawable of puppetxr's context on GL).
// Acquire before recording the frame, present after r_end_frame. Acquire fails when the
// swapchain is out of date (resize); destroy and recreate it then.

b8                      r_device_supports_present   (const R_Device* device, const R_WindowTarget& target);
R_WindowSwapchainHandle r_create_window_swapchain   (R_Device* device, const R_WindowTarget& target, u32 width, u32 height);
void                    r_destroy_window_swapchain  (R_Device* device, R_WindowSwapchainHandle swapchain);
b8                      r_is_valid                  (const R_Device* device, R_WindowSwapchainHandle swapchain);
R_Format                r_window_swapchain_format   (const R_Device* device, R_WindowSwapchainHandle swapchain);
void                    r_window_swapchain_size     (const R_Device* device, R_WindowSwapchainHandle swapchain, u32& out_width, u32& out_height);
b8                      r_window_acquire            (R_Device* device, R_WindowSwapchainHandle handle, R_ImageHandle& out_image); // image is in Undefined layout
b8                      r_window_present            (R_Device* device, R_WindowSwapchainHandle handle);                          // image must be in Present layout
b8                      r_window_busy               (const R_Device* device, R_WindowSwapchainHandle handle);                    // the last frame presented to it is still on the GPU; the next acquire would wait for it

//
// Frames and immediate commands
//
// A frame submits one command buffer on the app's queue; at most R_MAX_FRAMES_IN_FLIGHT
// are outstanding. Immediate commands submit and wait for completion before returning.
// Commands are recorded on one thread between begin and end, within one call of the app;
// backends without command buffers execute them as they are recorded. Layouts are explicit for
// backends that track them and ignored by the others.

R_CmdBufferHandle   r_begin_frame                (R_Device* device);
void                r_end_frame                  (R_Device* device, R_CmdBufferHandle cmd);
void                r_end_frame_present          (R_Device* device, R_CmdBufferHandle cmd, R_WindowSwapchainHandle swapchain); // waits for the acquire, signals the present
R_CmdBufferHandle   r_begin_immediate            (R_Device* device);
void                r_end_immediate              (R_Device* device, R_CmdBufferHandle cmd);
u64                 r_get_cmd_native             (const R_Device* device, R_CmdBufferHandle cmd); // VkCommandBuffer, 0 on backends without command buffers

//
// Commands
//

void                r_cmd_transition_image       (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle handle, R_ImageLayout old_layout, R_ImageLayout new_layout);
void                r_cmd_begin_render_pass      (R_Device* device, R_CmdBufferHandle cmd, R_RenderPassHandle pass_handle, R_FramebufferHandle fb_handle, const R_ClearColor& clear);
void                r_cmd_end_render_pass        (R_Device* device, R_CmdBufferHandle cmd);
void                r_cmd_bind_pipeline          (R_Device* device, R_CmdBufferHandle cmd, R_PipelineHandle handle);
void                r_cmd_bind_texture           (R_Device* device, R_CmdBufferHandle cmd, R_PipelineHandle pipeline, R_ImageHandle image, R_Filter filter);
void                r_cmd_bind_textures          (R_Device* device, R_CmdBufferHandle cmd, R_PipelineHandle pipeline, const R_ImageHandle* images, u32 count, R_Filter filter); // count must match the pipeline
void                r_cmd_clear_depth            (R_Device* device, R_CmdBufferHandle cmd, const R_Rect& rect, f32 depth); // inside a render pass with depth
void                r_cmd_push_constants         (R_Device* device, R_CmdBufferHandle cmd, R_PipelineHandle handle, const void* data, u32 size);
void                r_cmd_set_viewport           (R_Device* device, R_CmdBufferHandle cmd, const R_Viewport& viewport);
void                r_cmd_set_scissor            (R_Device* device, R_CmdBufferHandle cmd, const R_Rect& scissor);
void                r_cmd_bind_vertex_buffer     (R_Device* device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset);
void                r_cmd_bind_index_buffer      (R_Device* device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset, b8 u16_indices);
void                r_cmd_draw                   (R_Device* device, R_CmdBufferHandle cmd, u32 vertex_count, u32 first_vertex);
void                r_cmd_draw_indexed           (R_Device* device, R_CmdBufferHandle cmd, u32 index_count, u32 first_index, i32 vertex_offset);
void                r_cmd_copy_buffer_to_image   (R_Device* device, R_CmdBufferHandle cmd, R_BufferHandle buffer, R_ImageHandle image, u32 layer); // image in TransferDst
void                r_cmd_copy_image_to_buffer   (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle image, u32 layer, R_BufferHandle buffer); // image in TransferSrc
void                r_cmd_clear_image            (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle handle, const R_ClearColor& color); // image in TransferDst
void                r_cmd_blit_image             (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle src, u32 src_layer, R_ImageHandle dst, const R_Rect& dst_rect, R_Filter filter); // src TransferSrc, dst TransferDst

// Synchronous readback of one layer, depth formats included; out_pixels must hold width * height * bytes_per_pixel. The image must be in `layout` and is left there.
b8                  r_read_image                 (R_Device* device, R_ImageHandle handle, u32 layer, R_ImageLayout layout, void* out_pixels, usize out_size);
u32                 r_max_mip_levels             (u32 width, u32 height); // a full chain down to 1x1
// Whether the device can create such a 2D image with these extra VkImageCreateFlags and VkImageUsageFlags
b8                  r_image_flags_supported      (const R_Device* device, const R_CreateImageInfo& info);
