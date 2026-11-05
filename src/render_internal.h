#pragma once

// Shared by render.cpp and the backends (render_*.cpp) only. Everything else uses render.h.

#include "render.h"

//
// Backends
//
// Every backend fills one table; render.cpp forwards each public r_* function that takes a
// device through it. Entries are never called with a null device.

struct R_BackendFns
{
    void                    (*destroy_device)            (R_Device* device);
    void                    (*wait_idle)                 (const R_Device* device);
    void                    (*flush_initial_layouts)     (R_Device* device);
    u64                     (*queue_access_count)        (const R_Device* device);
    i64                     (*native_format)             (const R_Device* device, R_Format format);
    R_Format                (*format_from_native)        (const R_Device* device, i64 format);
    u64                     (*adapter_luid)              (const R_Device* device); // D3D backends only; null on the others
    u32                     (*x11_visual)                (const R_Device* device); // GL only; null on the others

    R_ImageHandle           (*create_image)              (R_Device* device, const R_CreateImageInfo& info);
    b8                      (*image_flags_supported)     (const R_Device* device, const R_CreateImageInfo& info);
    R_ImageHandle           (*import_image)              (R_Device* device, const R_ImportImageInfo& info);
    void                    (*destroy_image)             (R_Device* device, R_ImageHandle handle);
    b8                      (*is_valid_image)            (const R_Device* device, R_ImageHandle image);
    u64                     (*get_image_native)          (const R_Device* device, R_ImageHandle handle);
    void                    (*get_image_size)            (const R_Device* device, R_ImageHandle handle, u32& out_width, u32& out_height, u32& out_layers);
    R_Format                (*get_image_format)          (const R_Device* device, R_ImageHandle handle);

    R_BufferHandle          (*create_buffer)             (R_Device* device, const R_CreateBufferInfo& info);
    void                    (*destroy_buffer)            (R_Device* device, R_BufferHandle handle);
    b8                      (*is_valid_buffer)           (const R_Device* device, R_BufferHandle buffer);
    void*                   (*get_buffer_mapped_data)    (const R_Device* device, R_BufferHandle handle);
    void                    (*flush_buffer)              (R_Device* device, R_BufferHandle handle, u64 offset, u64 size);

    R_RenderPassHandle      (*create_render_pass)        (R_Device* device, const R_CreateRenderPassInfo& info);
    void                    (*destroy_render_pass)       (R_Device* device, R_RenderPassHandle handle);
    b8                      (*is_valid_render_pass)      (const R_Device* device, R_RenderPassHandle render_pass);
    R_FramebufferHandle     (*create_framebuffer)        (R_Device* device, const R_CreateFramebufferInfo& info);
    void                    (*destroy_framebuffer)       (R_Device* device, R_FramebufferHandle handle);
    b8                      (*is_valid_framebuffer)      (const R_Device* device, R_FramebufferHandle framebuffer);
    R_PipelineHandle        (*create_pipeline)           (R_Device* device, const R_CreatePipelineInfo& info);
    void                    (*destroy_pipeline)          (R_Device* device, R_PipelineHandle handle);
    b8                      (*is_valid_pipeline)         (const R_Device* device, R_PipelineHandle pipeline);

    b8                      (*device_supports_present)   (const R_Device* device, const R_WindowTarget& target);
    R_WindowSwapchainHandle (*create_window_swapchain)   (R_Device* device, const R_WindowTarget& target, u32 width, u32 height);
    void                    (*destroy_window_swapchain)  (R_Device* device, R_WindowSwapchainHandle swapchain);
    b8                      (*is_valid_window_swapchain) (const R_Device* device, R_WindowSwapchainHandle swapchain);
    R_Format                (*window_swapchain_format)   (const R_Device* device, R_WindowSwapchainHandle swapchain);
    void                    (*window_swapchain_size)     (const R_Device* device, R_WindowSwapchainHandle swapchain, u32& out_width, u32& out_height);
    b8                      (*window_acquire)            (R_Device* device, R_WindowSwapchainHandle handle, R_ImageHandle& out_image);
    b8                      (*window_present)            (R_Device* device, R_WindowSwapchainHandle handle);
    b8                      (*window_busy)               (const R_Device* device, R_WindowSwapchainHandle handle); // null where acquiring never waits for the GPU (GL, D3D)

    R_CmdBufferHandle       (*begin_frame)               (R_Device* device);
    void                    (*end_frame)                 (R_Device* device, R_CmdBufferHandle cmd);
    void                    (*end_frame_present)         (R_Device* device, R_CmdBufferHandle cmd, R_WindowSwapchainHandle swapchain);
    R_CmdBufferHandle       (*begin_immediate)           (R_Device* device);
    void                    (*end_immediate)             (R_Device* device, R_CmdBufferHandle cmd);
    u64                     (*get_cmd_native)            (const R_Device* device, R_CmdBufferHandle cmd);

    void                    (*cmd_transition_image)      (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle handle, R_ImageLayout old_layout, R_ImageLayout new_layout);
    void                    (*cmd_begin_render_pass)     (R_Device* device, R_CmdBufferHandle cmd, R_RenderPassHandle pass_handle, R_FramebufferHandle fb_handle, const R_ClearColor& clear);
    void                    (*cmd_end_render_pass)       (R_Device* device, R_CmdBufferHandle cmd);
    void                    (*cmd_bind_pipeline)         (R_Device* device, R_CmdBufferHandle cmd, R_PipelineHandle handle);
    void                    (*cmd_bind_textures)         (R_Device* device, R_CmdBufferHandle cmd, R_PipelineHandle pipeline, const R_ImageHandle* images, u32 count, R_Filter filter);
    void                    (*cmd_clear_depth)           (R_Device* device, R_CmdBufferHandle cmd, const R_Rect& rect, f32 depth);
    void                    (*cmd_push_constants)        (R_Device* device, R_CmdBufferHandle cmd, R_PipelineHandle handle, const void* data, u32 size);
    void                    (*cmd_set_viewport)          (R_Device* device, R_CmdBufferHandle cmd, const R_Viewport& viewport);
    void                    (*cmd_set_scissor)           (R_Device* device, R_CmdBufferHandle cmd, const R_Rect& scissor);
    void                    (*cmd_bind_vertex_buffer)    (R_Device* device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset);
    void                    (*cmd_bind_index_buffer)     (R_Device* device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset, b8 u16_indices);
    void                    (*cmd_draw)                  (R_Device* device, R_CmdBufferHandle cmd, u32 vertex_count, u32 first_vertex);
    void                    (*cmd_draw_indexed)          (R_Device* device, R_CmdBufferHandle cmd, u32 index_count, u32 first_index, i32 vertex_offset);
    void                    (*cmd_copy_buffer_to_image)  (R_Device* device, R_CmdBufferHandle cmd, R_BufferHandle buffer, R_ImageHandle image, u32 layer);
    void                    (*cmd_copy_image_to_buffer)  (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle image, u32 layer, R_BufferHandle buffer);
    void                    (*cmd_clear_image)           (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle handle, const R_ClearColor& color);
    void                    (*cmd_blit_image)            (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle src, u32 src_layer, R_ImageHandle dst, const R_Rect& dst_rect, R_Filter filter);
    b8                      (*read_image)                (R_Device* device, R_ImageHandle handle, u32 layer, R_ImageLayout layout, void* out_pixels, usize out_size);
};

// Every backend's device struct starts with this, so render.cpp dispatches without knowing it
struct R_Device
{
    const R_BackendFns* fns;
    R_Backend           backend;
};

R_Device* r_vk_create_device    (const R_DeviceCreateInfo& info);
R_Device* r_gl_create_device    (const R_DeviceCreateInfo& info);
R_Device* r_d3d11_create_device (const R_DeviceCreateInfo& info);
R_Device* r_d3d12_create_device (const R_DeviceCreateInfo& info);
R_Device* r_metal_create_device (const R_DeviceCreateInfo& info);
void*     r_metal_system_device ();

// The four blend factors of a pipeline (source color, target color, source alpha, target alpha):
// blend_factors when blend_custom, else premultiplied or straight alpha over the target
void r_pipeline_blend_factors (const R_CreatePipelineInfo& info, R_BlendFactor out_factors[4]);

//
// DXGI, shared by the D3D backends (render_d3d11.cpp)
//

// DXGI_FORMAT values of an R_Format. Color images are typeless so that apps view them as sRGB
// or linear; depth images are typeless so that they can be sampled.
struct R_DxgiFormat
{
    u32 resource;
    u32 typed;  // what apps see in xrEnumerateSwapchainFormats, and vertex attributes
    u32 shader; // the view to sample it through
    u32 depth;  // the depth-stencil view, depth formats only
};

const R_DxgiFormat& r_dxgi_format             (R_Format format); // all 0 for formats D3D lacks
R_Format            r_dxgi_format_from_native (i64 format);
b8                  r_dxgi_adapter_luid       (u64& out_luid);
u64                 r_dxgi_device_luid        (void* unknown_device); // the adapter of a DXGI device (an ID3D11Device); 0 when unknown
