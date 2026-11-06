#include "render_internal.h"

//
// Statics
//

// Indexed by R_Format
const static struct
{
    R_Format format;
    u32      bytes;
    b8       depth;
} r_formats[] = {
    { R_Format::Unknown,              0,  false },
    { R_Format::R8G8B8A8_UNORM,       4,  false },
    { R_Format::B8G8R8A8_UNORM,       4,  false },
    { R_Format::R8G8B8A8_SRGB,        4,  false },
    { R_Format::B8G8R8A8_SRGB,        4,  false },
    { R_Format::R16G16B16A16_SFLOAT,  8,  false },
    { R_Format::R32G32_SFLOAT,        8,  false },
    { R_Format::R32G32B32_SFLOAT,     12, false },
    { R_Format::D16_UNORM,            2,  true },
    { R_Format::D24_UNORM_S8_UINT,    4,  true },
    { R_Format::D32_SFLOAT,           4,  true },
    { R_Format::D32_SFLOAT_S8_UINT,   8,  true },
};
static_assert(ARRAY_SIZE(r_formats) == (usize)R_Format::Count, "a format is missing");

//
// Device
//

R_Device* r_create_device (const R_DeviceCreateInfo& info)
{
    switch (info.backend)
    {
    case R_Backend::Vulkan:
        return r_vk_create_device(info);
    case R_Backend::OpenGL:
    case R_Backend::OpenGLES:
        return r_gl_create_device(info);
#if PUPPETXR_HAVE_D3D
    case R_Backend::D3D11:
        return r_d3d11_create_device(info);
    case R_Backend::D3D12:
        return r_d3d12_create_device(info);
#else
    case R_Backend::D3D11:
    case R_Backend::D3D12:
        LOG_ERROR("This build has no D3D backends");
        return nullptr;
#endif
#if PUPPETXR_HAVE_METAL
    case R_Backend::Metal:
        return r_metal_create_device(info);
#else
    case R_Backend::Metal:
        LOG_ERROR("This build has no Metal backend");
        return nullptr;
#endif
    case R_Backend::Count:
        break;
    }
    LOG_ERROR("Unknown render backend %u", (u32)info.backend);
    return nullptr;
}

b8 r_backend_available (R_Backend backend)
{
    switch (backend)
    {
    case R_Backend::Vulkan:
    case R_Backend::OpenGL:
    case R_Backend::OpenGLES:
        return backend == R_Backend::Vulkan || PUPPETXR_HAVE_GL;
    case R_Backend::D3D11: // NOLINT(bugprone-branch-clone): the same only where both macros are
    case R_Backend::D3D12:
        return PUPPETXR_HAVE_D3D;
    case R_Backend::Metal:
        return PUPPETXR_HAVE_METAL;
    case R_Backend::Count:
        break;
    }
    return false;
}

b8 r_d3d_adapter_luid (u64& out_luid)
{
    out_luid = 0;
#if PUPPETXR_HAVE_D3D
    return r_dxgi_adapter_luid(out_luid);
#else
    return false;
#endif
}

u64 r_d3d_device_luid (const R_Device* device)
{
    return device && device->fns->adapter_luid ? device->fns->adapter_luid(device) : 0;
}

void* r_metal_default_device ()
{
#if PUPPETXR_HAVE_METAL
    return r_metal_system_device();
#else
    return nullptr;
#endif
}

u32 r_gl_x11_visual (const R_Device* device)
{
    return device && device->fns->x11_visual ? device->fns->x11_visual(device) : 0;
}

R_Backend r_device_backend (const R_Device* device)
{
    ASSERT(device);
    return device->backend;
}

u32 r_format_bytes_per_pixel (R_Format format)
{
    return (u32)format < ARRAY_SIZE(r_formats) ? r_formats[(u32)format].bytes : 0;
}

b8 r_format_is_depth (R_Format format)
{
    return (u32)format < ARRAY_SIZE(r_formats) && r_formats[(u32)format].depth;
}

b8 r_format_has_stencil (R_Format format)
{
    return format == R_Format::D24_UNORM_S8_UINT || format == R_Format::D32_SFLOAT_S8_UINT;
}

void r_pipeline_blend_factors (const R_CreatePipelineInfo& info, R_BlendFactor out_factors[4])
{
    if (info.blend_custom)
    {
        for (u32 i = 0; i < 4; i++)
        {
            out_factors[i] = (u32)info.blend_factors[i] <= (u32)R_BlendFactor::OneMinusDstAlpha ? info.blend_factors[i] : R_BlendFactor::One;
        }
        return;
    }
    out_factors[0] = info.blend_premultiplied ? R_BlendFactor::One : R_BlendFactor::SrcAlpha;
    out_factors[1] = R_BlendFactor::OneMinusSrcAlpha;
    out_factors[2] = R_BlendFactor::One;
    out_factors[3] = R_BlendFactor::OneMinusSrcAlpha;
}

u32 r_max_mip_levels (u32 width, u32 height)
{
    u32 levels = 1;
    for (u32 size = zm::max(width, height); size > 1; size >>= 1)
    {
        levels++;
    }
    return levels;
}


void r_destroy_device (R_Device* device)
{
    if (device)
    {
        device->fns->destroy_device(device);
    }
}

void r_wait_idle (const R_Device* device)
{
    ASSERT(device);
    PROF_ZONE("gpu wait idle");
    device->fns->wait_idle(device);
}

void r_flush_initial_layouts (R_Device* device)
{
    ASSERT(device);
    device->fns->flush_initial_layouts(device);
}

u64 r_queue_access_count (const R_Device* device)
{
    ASSERT(device);
    return device->fns->queue_access_count(device);
}

i64 r_native_format (const R_Device* device, R_Format format)
{
    ASSERT(device);
    return device->fns->native_format(device, format);
}

R_Format r_format_from_native (const R_Device* device, i64 format)
{
    ASSERT(device);
    return device->fns->format_from_native(device, format);
}

//
// Resources
//

R_ImageHandle r_create_image (R_Device* device, const R_CreateImageInfo& info)
{
    ASSERT(device);
    return device->fns->create_image(device, info);
}

b8 r_image_flags_supported (const R_Device* device, const R_CreateImageInfo& info)
{
    ASSERT(device);
    return device->fns->image_flags_supported(device, info);
}

R_ImageHandle r_import_image (R_Device* device, const R_ImportImageInfo& info)
{
    ASSERT(device);
    return device->fns->import_image(device, info);
}

void r_destroy_image (R_Device* device, R_ImageHandle handle)
{
    ASSERT(device);
    device->fns->destroy_image(device, handle);
}

b8 r_is_valid (const R_Device* device, R_ImageHandle image)
{
    ASSERT(device);
    return device->fns->is_valid_image(device, image);
}

u64 r_get_image_native (const R_Device* device, R_ImageHandle handle)
{
    ASSERT(device);
    return device->fns->get_image_native(device, handle);
}

void r_get_image_size (const R_Device* device, R_ImageHandle handle, u32& out_width, u32& out_height, u32& out_layers)
{
    ASSERT(device);
    device->fns->get_image_size(device, handle, out_width, out_height, out_layers);
}

R_Format r_get_image_format (const R_Device* device, R_ImageHandle handle)
{
    ASSERT(device);
    return device->fns->get_image_format(device, handle);
}

R_BufferHandle r_create_buffer (R_Device* device, const R_CreateBufferInfo& info)
{
    ASSERT(device);
    return device->fns->create_buffer(device, info);
}

void r_destroy_buffer (R_Device* device, R_BufferHandle handle)
{
    ASSERT(device);
    device->fns->destroy_buffer(device, handle);
}

b8 r_is_valid (const R_Device* device, R_BufferHandle buffer)
{
    ASSERT(device);
    return device->fns->is_valid_buffer(device, buffer);
}

void* r_get_buffer_mapped_data (const R_Device* device, R_BufferHandle handle)
{
    ASSERT(device);
    return device->fns->get_buffer_mapped_data(device, handle);
}

void r_flush_buffer (R_Device* device, R_BufferHandle handle, u64 offset, u64 size)
{
    ASSERT(device);
    device->fns->flush_buffer(device, handle, offset, size);
}

R_RenderPassHandle r_create_render_pass (R_Device* device, const R_CreateRenderPassInfo& info)
{
    ASSERT(device);
    return device->fns->create_render_pass(device, info);
}

void r_destroy_render_pass (R_Device* device, R_RenderPassHandle handle)
{
    ASSERT(device);
    device->fns->destroy_render_pass(device, handle);
}

b8 r_is_valid (const R_Device* device, R_RenderPassHandle render_pass)
{
    ASSERT(device);
    return device->fns->is_valid_render_pass(device, render_pass);
}

R_FramebufferHandle r_create_framebuffer (R_Device* device, const R_CreateFramebufferInfo& info)
{
    ASSERT(device);
    return device->fns->create_framebuffer(device, info);
}

void r_destroy_framebuffer (R_Device* device, R_FramebufferHandle handle)
{
    ASSERT(device);
    device->fns->destroy_framebuffer(device, handle);
}

b8 r_is_valid (const R_Device* device, R_FramebufferHandle framebuffer)
{
    ASSERT(device);
    return device->fns->is_valid_framebuffer(device, framebuffer);
}

R_PipelineHandle r_create_pipeline (R_Device* device, const R_CreatePipelineInfo& info)
{
    ASSERT(device);
    return device->fns->create_pipeline(device, info);
}

void r_destroy_pipeline (R_Device* device, R_PipelineHandle handle)
{
    ASSERT(device);
    device->fns->destroy_pipeline(device, handle);
}

b8 r_is_valid (const R_Device* device, R_PipelineHandle pipeline)
{
    ASSERT(device);
    return device->fns->is_valid_pipeline(device, pipeline);
}

//
// Window presentation
//

b8 r_device_supports_present (const R_Device* device, const R_WindowTarget& target)
{
    ASSERT(device);
    return device->fns->device_supports_present(device, target);
}

R_WindowSwapchainHandle r_create_window_swapchain (R_Device* device, const R_WindowTarget& target, u32 width, u32 height)
{
    ASSERT(device);
    PROF_ZONE("window swapchain create");
    return device->fns->create_window_swapchain(device, target, width, height);
}

void r_destroy_window_swapchain (R_Device* device, R_WindowSwapchainHandle swapchain)
{
    ASSERT(device);
    device->fns->destroy_window_swapchain(device, swapchain);
}

b8 r_is_valid (const R_Device* device, R_WindowSwapchainHandle swapchain)
{
    ASSERT(device);
    return device->fns->is_valid_window_swapchain(device, swapchain);
}

R_Format r_window_swapchain_format (const R_Device* device, R_WindowSwapchainHandle swapchain)
{
    ASSERT(device);
    return device->fns->window_swapchain_format(device, swapchain);
}

void r_window_swapchain_size (const R_Device* device, R_WindowSwapchainHandle swapchain, u32& out_width, u32& out_height)
{
    ASSERT(device);
    device->fns->window_swapchain_size(device, swapchain, out_width, out_height);
}

b8 r_window_acquire (R_Device* device, R_WindowSwapchainHandle handle, R_ImageHandle& out_image)
{
    ASSERT(device);
    PROF_ZONE("window acquire");
    return device->fns->window_acquire(device, handle, out_image);
}

b8 r_window_present (R_Device* device, R_WindowSwapchainHandle handle)
{
    ASSERT(device);
    PROF_ZONE("window present");
    return device->fns->window_present(device, handle);
}

b8 r_window_busy (const R_Device* device, R_WindowSwapchainHandle handle)
{
    ASSERT(device);
    return device->fns->window_busy && device->fns->window_busy(device, handle);
}

//
// Frames and immediate commands
//

R_CmdBufferHandle r_begin_frame (R_Device* device)
{
    ASSERT(device);
    PROF_ZONE("gpu frame begin");
    return device->fns->begin_frame(device);
}

void r_end_frame (R_Device* device, R_CmdBufferHandle cmd)
{
    ASSERT(device);
    PROF_ZONE("gpu submit");
    device->fns->end_frame(device, cmd);
}

void r_end_frame_present (R_Device* device, R_CmdBufferHandle cmd, R_WindowSwapchainHandle swapchain)
{
    ASSERT(device);
    PROF_ZONE("gpu submit present");
    device->fns->end_frame_present(device, cmd, swapchain);
}

R_CmdBufferHandle r_begin_immediate (R_Device* device)
{
    ASSERT(device);
    return device->fns->begin_immediate(device);
}

void r_end_immediate (R_Device* device, R_CmdBufferHandle cmd)
{
    ASSERT(device);
    PROF_ZONE("gpu immediate");
    device->fns->end_immediate(device, cmd);
}

u64 r_get_cmd_native (const R_Device* device, R_CmdBufferHandle cmd)
{
    ASSERT(device);
    return device->fns->get_cmd_native(device, cmd);
}

//
// Commands
//

void r_cmd_transition_image (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle handle, R_ImageLayout old_layout, R_ImageLayout new_layout)
{
    ASSERT(device);
    device->fns->cmd_transition_image(device, cmd, handle, old_layout, new_layout);
}

void r_cmd_begin_render_pass (R_Device* device, R_CmdBufferHandle cmd, R_RenderPassHandle pass_handle, R_FramebufferHandle fb_handle, const R_ClearColor& clear)
{
    ASSERT(device);
    device->fns->cmd_begin_render_pass(device, cmd, pass_handle, fb_handle, clear);
}

void r_cmd_end_render_pass (R_Device* device, R_CmdBufferHandle cmd)
{
    ASSERT(device);
    device->fns->cmd_end_render_pass(device, cmd);
}

void r_cmd_bind_pipeline (R_Device* device, R_CmdBufferHandle cmd, R_PipelineHandle handle)
{
    ASSERT(device);
    device->fns->cmd_bind_pipeline(device, cmd, handle);
}

void r_cmd_bind_texture (R_Device* device, R_CmdBufferHandle cmd, R_PipelineHandle pipeline, R_ImageHandle image, R_Filter filter)
{
    ASSERT(device);
    device->fns->cmd_bind_textures(device, cmd, pipeline, &image, 1, filter);
}

void r_cmd_bind_textures (R_Device* device, R_CmdBufferHandle cmd, R_PipelineHandle pipeline, const R_ImageHandle* images, u32 count, R_Filter filter)
{
    ASSERT(device);
    device->fns->cmd_bind_textures(device, cmd, pipeline, images, count, filter);
}

void r_cmd_clear_depth (R_Device* device, R_CmdBufferHandle cmd, const R_Rect& rect, f32 depth)
{
    ASSERT(device);
    ASSERT(depth >= 0.0f && depth <= 1.0f);
    device->fns->cmd_clear_depth(device, cmd, rect, depth);
}

void r_cmd_push_constants (R_Device* device, R_CmdBufferHandle cmd, R_PipelineHandle handle, const void* data, u32 size)
{
    ASSERT(device);
    device->fns->cmd_push_constants(device, cmd, handle, data, size);
}

void r_cmd_set_viewport (R_Device* device, R_CmdBufferHandle cmd, const R_Viewport& viewport)
{
    ASSERT(device);
    device->fns->cmd_set_viewport(device, cmd, viewport);
}

void r_cmd_set_scissor (R_Device* device, R_CmdBufferHandle cmd, const R_Rect& scissor)
{
    ASSERT(device);
    device->fns->cmd_set_scissor(device, cmd, scissor);
}

void r_cmd_bind_vertex_buffer (R_Device* device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset)
{
    ASSERT(device);
    device->fns->cmd_bind_vertex_buffer(device, cmd, handle, offset);
}

void r_cmd_bind_index_buffer (R_Device* device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset, b8 u16_indices)
{
    ASSERT(device);
    device->fns->cmd_bind_index_buffer(device, cmd, handle, offset, u16_indices);
}

void r_cmd_draw (R_Device* device, R_CmdBufferHandle cmd, u32 vertex_count, u32 first_vertex)
{
    ASSERT(device);
    device->fns->cmd_draw(device, cmd, vertex_count, first_vertex);
}

void r_cmd_draw_indexed (R_Device* device, R_CmdBufferHandle cmd, u32 index_count, u32 first_index, i32 vertex_offset)
{
    ASSERT(device);
    device->fns->cmd_draw_indexed(device, cmd, index_count, first_index, vertex_offset);
}

void r_cmd_copy_buffer_to_image (R_Device* device, R_CmdBufferHandle cmd, R_BufferHandle buffer, R_ImageHandle image, u32 layer)
{
    ASSERT(device);
    device->fns->cmd_copy_buffer_to_image(device, cmd, buffer, image, layer);
}

void r_cmd_copy_image_to_buffer (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle image, u32 layer, R_BufferHandle buffer)
{
    ASSERT(device);
    device->fns->cmd_copy_image_to_buffer(device, cmd, image, layer, buffer);
}

void r_cmd_clear_image (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle handle, const R_ClearColor& color)
{
    ASSERT(device);
    device->fns->cmd_clear_image(device, cmd, handle, color);
}

void r_cmd_blit_image (R_Device* device, R_CmdBufferHandle cmd, R_ImageHandle src, u32 src_layer, R_ImageHandle dst, const R_Rect& dst_rect, R_Filter filter)
{
    ASSERT(device);
    device->fns->cmd_blit_image(device, cmd, src, src_layer, dst, dst_rect, filter);
}

b8 r_read_image (R_Device* device, R_ImageHandle handle, u32 layer, R_ImageLayout layout, void* out_pixels, usize out_size)
{
    ASSERT(device);
    PROF_ZONE("gpu readback");
    return device->fns->read_image(device, handle, layer, layout, out_pixels, out_size);
}
