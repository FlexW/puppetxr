#include "render_internal.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include "blocks.h"
#include "blit.vert.h"
#include "blit.frag.h"

//
// Types
//
// Commands are recorded on deferred contexts and run with ExecuteCommandList(list, TRUE), which
// restores the app's state of the immediate context afterwards. A device created single threaded
// has no deferred contexts; then commands go to the immediate context inside a context state of
// our own (SwapDeviceContextState), swapped in at begin and out at end.

#define R_D3D11_CONTEXTS (R_MAX_FRAMES_IN_FLIGHT + 1) // the frames, then the immediate buffer

namespace
{

struct R_D3D11Image
{
    ID3D11Texture2D*          texture;
    ID3D11ShaderResourceView* view;       // Texture2D, single-layer images only
    ID3D11ShaderResourceView* array_view; // Texture2DArray over every layer
    ID3D11RenderTargetView*   target;     // layer 0, for clears and blits; made on first use
    u32                       width;
    u32                       height;
    u32                       layers;
    u32                       mip_levels;
    R_Format                  format;
    b8                        imported;
};

struct R_D3D11Buffer
{
    ID3D11Buffer* buffer;  // null for a buffer that only stages transfers
    u8*           shadow;  // what r_get_buffer_mapped_data hands out, host visible buffers only
    u64           size;
    u64           written; // bytes from the start ever flushed, uploaded when bound
    b8            dirty;
};

struct R_D3D11RenderPass
{
    R_Format format;
    R_LoadOp load_op;
    R_Format depth_format;
    R_LoadOp depth_load_op;
};

struct R_D3D11Framebuffer
{
    ID3D11RenderTargetView* target = {};
    ID3D11DepthStencilView* depth = {};
    u32                     width = {};
    u32                     height = {};
    R_RenderPassHandle      render_pass = {};
};

struct R_D3D11Pipeline
{
    ID3D11VertexShader*      vertex_shader;
    ID3D11PixelShader*       pixel_shader;
    ID3D11InputLayout*       input_layout;
    ID3D11BlendState*        blend;
    ID3D11DepthStencilState* depth;
    u32                      textures_count;
    R_TextureType            texture_type;
    u32                      push_constants_size;
    u32                      vertex_stride;
};

struct R_D3D11WindowSwapchain
{
    IDXGISwapChain1* swapchain = {};
    R_ImageHandle    image;        // buffer 0, which flip model swapchains keep current in D3D11
    u32              width = {};
    u32              height = {};
};

struct R_D3D11CmdBuffer
{
    u32 slot; // context index, R_MAX_FRAMES_IN_FLIGHT for the immediate buffer
};

// A copy into a staging texture, read into the buffer's shadow once the commands ran
struct R_D3D11Readback
{
    ID3D11Texture2D* staging = {};
    R_BufferHandle   buffer = {};
    u32              width = {};
    u32              height = {};
    u32              bytes_per_pixel = {};
};

#define R_D3D11_MAX_READBACKS 8

struct R_D3D11Device
{
    R_Device                                         base = {};
    ID3D11Device*                                    device = {};
    ID3D11DeviceContext*                             immediate = {};
    ID3D11DeviceContext1*                            immediate1 = {}; // without deferred contexts
    ID3DDeviceContextState*                          state = {};      // without deferred contexts
    ID3DDeviceContextState*                          saved_state = {};
    ID3D11DeviceContext*                             contexts[R_D3D11_CONTEXTS] = {}; // deferred; the immediate one without
    ID3D11Query*                                     idle_query = {};
    u64                                              queue_accesses = {};

    ID3D11Buffer*                                    push_buffer = {};
    ID3D11SamplerState*                              samplers[2] = {}; // by R_Filter
    ID3D11RasterizerState*                           rasterizer = {};
    ID3D11VertexShader*                              blit_vertex = {};
    ID3D11PixelShader*                               blit_pixel = {};
    ID3D11BlendState*                                no_blend = {};
    ID3D11BlendState*                                no_color = {};
    ID3D11DepthStencilState*                         no_depth = {};
    ID3D11DepthStencilState*                         depth_overwrite = {};

    b8                                               recording[R_D3D11_CONTEXTS] = {};
    R_CmdBufferHandle                                cmd_handles[R_D3D11_CONTEXTS] = {};
    u32                                              frame_index = {};

    R_PipelineHandle                                 bound_pipeline = {};
    R_BufferHandle                                   vertex_buffer = {};
    u64                                              vertex_offset = {};
    b8                                               vertex_dirty = {};
    R_FramebufferHandle                              framebuffer = {};
    D3D11_VIEWPORT                                   viewport = {};
    D3D11_RECT                                       scissor = {};

    R_D3D11Readback                                  readbacks[R_D3D11_MAX_READBACKS] = {};
    u32                                              readbacks_count = {};

    SparseArray<R_D3D11Image, R_Image>               images = {};
    SparseArray<R_D3D11Buffer, R_Buffer>             buffers = {};
    SparseArray<R_D3D11RenderPass, R_RenderPass>     render_passes = {};
    SparseArray<R_D3D11Framebuffer, R_Framebuffer>   framebuffers = {};
    SparseArray<R_D3D11Pipeline, R_Pipeline>         pipelines = {};
    SparseArray<R_D3D11CmdBuffer, R_CmdBuffer>       cmds = {};
    SparseArray<R_D3D11WindowSwapchain, R_WindowSwapchain> windows = {};
};

typedef HRESULT (WINAPI* R_CreateDXGIFactory1) (REFIID riid, void** factory);

} // namespace

//
// Statics
//

// Indexed by R_Format
const static struct
{
    R_Format     format;
    R_DxgiFormat dxgi;
} r_dxgi_formats[] = {
    { R_Format::Unknown,             { 0, 0, 0, 0 } },
    { R_Format::R8G8B8A8_UNORM,      { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, 0 } },
    { R_Format::B8G8R8A8_UNORM,      { DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, 0 } },
    { R_Format::R8G8B8A8_SRGB,       { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0 } },
    { R_Format::B8G8R8A8_SRGB,       { DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 0 } },
    { R_Format::R16G16B16A16_SFLOAT, { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, 0 } },
    { R_Format::R32G32_SFLOAT,       { 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0 } },
    { R_Format::R32G32B32_SFLOAT,    { 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0 } },
    { R_Format::D16_UNORM,           { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_D16_UNORM } },
    { R_Format::D24_UNORM_S8_UINT,   { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT } },
    { R_Format::D32_SFLOAT,          { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT } },
    { R_Format::D32_SFLOAT_S8_UINT,  { DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT } },
};
static_assert(ARRAY_SIZE(r_dxgi_formats) == (usize)R_Format::Count, "a format is missing");

static R_D3D11Device* d3d11_device (R_Device* device)
{
    return (R_D3D11Device*)device;
}

static const R_D3D11Device* d3d11_device (const R_Device* device)
{
    return (const R_D3D11Device*)device;
}

template <typename T>
static void release (T*& object)
{
    if (object)
    {
        object->Release();
        object = nullptr;
    }
}

static D3D11_COMPARISON_FUNC compare_func (R_CompareOp op)
{
    switch (op)
    {
    case R_CompareOp::LessOrEqual:
        return D3D11_COMPARISON_LESS_EQUAL;
    case R_CompareOp::Always:
        return D3D11_COMPARISON_ALWAYS;
    case R_CompareOp::Never:
        return D3D11_COMPARISON_NEVER;
    case R_CompareOp::Less:
        return D3D11_COMPARISON_LESS;
    case R_CompareOp::Equal:
        return D3D11_COMPARISON_EQUAL;
    case R_CompareOp::Greater:
        return D3D11_COMPARISON_GREATER;
    case R_CompareOp::NotEqual:
        return D3D11_COMPARISON_NOT_EQUAL;
    case R_CompareOp::GreaterOrEqual:
        return D3D11_COMPARISON_GREATER_EQUAL;
    }
    return D3D11_COMPARISON_LESS_EQUAL;
}

static D3D11_BLEND blend_factor (R_BlendFactor factor)
{
    switch (factor)
    {
    case R_BlendFactor::Zero:
        return D3D11_BLEND_ZERO;
    case R_BlendFactor::One:
        return D3D11_BLEND_ONE;
    case R_BlendFactor::SrcAlpha:
        return D3D11_BLEND_SRC_ALPHA;
    case R_BlendFactor::OneMinusSrcAlpha:
        return D3D11_BLEND_INV_SRC_ALPHA;
    case R_BlendFactor::DstAlpha:
        return D3D11_BLEND_DEST_ALPHA;
    case R_BlendFactor::OneMinusDstAlpha:
        return D3D11_BLEND_INV_DEST_ALPHA;
    }
    return D3D11_BLEND_ONE;
}

static DXGI_FORMAT dxgi (u32 format)
{
    return (DXGI_FORMAT)format;
}

static R_CreateDXGIFactory1 load_create_factory ()
{
    OS_Library* library = os_library_open("dxgi.dll");
    return (R_CreateDXGIFactory1)os_library_symbol(library, "CreateDXGIFactory1");
}

static u64 luid_value (const LUID& luid)
{
    return (u64)luid.LowPart | (u64)(u32)luid.HighPart << 32;
}

static b8 cmd_recording (const R_D3D11Device* device, R_CmdBufferHandle cmd)
{
    const R_D3D11CmdBuffer* buffer = device->cmds.try_get(cmd);
    return buffer && device->recording[buffer->slot];
}

static ID3D11DeviceContext* cmd_context (const R_D3D11Device* device, R_CmdBufferHandle cmd)
{
    const R_D3D11CmdBuffer* buffer = device->cmds.try_get(cmd);
    return buffer && device->recording[buffer->slot] ? device->contexts[buffer->slot] : nullptr;
}

static UINT subresource (const R_D3D11Image& image, u32 layer)
{
    return D3D11CalcSubresource(0, layer, image.mip_levels);
}

static ID3D11RenderTargetView* create_target_view (const R_D3D11Device* device, const R_D3D11Image& image, u32 layer)
{
    D3D11_RENDER_TARGET_VIEW_DESC desc = {};
    desc.Format                         = dxgi(r_dxgi_format(image.format).typed);
    desc.ViewDimension                  = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.FirstArraySlice = layer;
    desc.Texture2DArray.ArraySize       = 1;
    ID3D11RenderTargetView* view        = nullptr;
    if (FAILED(device->device->CreateRenderTargetView(image.texture, &desc, &view)))
    {
        LOG_ERROR("Cannot make a render target view of layer %u", layer);
        return nullptr;
    }
    return view;
}

static ID3D11DepthStencilView* create_depth_view (const R_D3D11Device* device, const R_D3D11Image& image, u32 layer)
{
    D3D11_DEPTH_STENCIL_VIEW_DESC desc  = {};
    desc.Format                         = dxgi(r_dxgi_format(image.format).depth);
    desc.ViewDimension                  = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.FirstArraySlice = layer;
    desc.Texture2DArray.ArraySize       = 1;
    ID3D11DepthStencilView* view        = nullptr;
    if (FAILED(device->device->CreateDepthStencilView(image.texture, &desc, &view)))
    {
        LOG_ERROR("Cannot make a depth stencil view of layer %u", layer);
        return nullptr;
    }
    return view;
}

// The shader views of a sampled image, made once with it
static b8 create_shader_views (const R_D3D11Device* device, R_D3D11Image& image)
{
    const DXGI_FORMAT format = dxgi(r_dxgi_format(image.format).shader);
    if (image.layers == 1)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format                          = format;
        desc.ViewDimension                   = D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels             = image.mip_levels;
        if (FAILED(device->device->CreateShaderResourceView(image.texture, &desc, &image.view)))
        {
            return false;
        }
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format                          = format;
    desc.ViewDimension                   = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MipLevels        = image.mip_levels;
    desc.Texture2DArray.ArraySize        = image.layers;
    return SUCCEEDED(device->device->CreateShaderResourceView(image.texture, &desc, &image.array_view));
}

static void release_image (R_D3D11Image& image)
{
    release(image.view);
    release(image.array_view);
    release(image.target);
    release(image.texture);
}

static ID3D11RenderTargetView* layer0_target (const R_D3D11Device* device, R_D3D11Image& image)
{
    if (!image.target)
    {
        image.target = create_target_view(device, image, 0);
    }
    return image.target;
}

static void destroy_pipeline_resource (R_D3D11Pipeline& pipeline)
{
    release(pipeline.vertex_shader);
    release(pipeline.pixel_shader);
    release(pipeline.input_layout);
    release(pipeline.blend);
    release(pipeline.depth);
}

static void destroy_window_resource (R_D3D11Device* device, R_D3D11WindowSwapchain& window)
{
    if (window.image)
    {
        R_D3D11Image* image = device->images.try_get(window.image);
        if (image)
        {
            release_image(*image);
        }
        device->images.remove(window.image);
        window.image = NULL_HANDLE;
    }
    if (window.swapchain)
    {
        release(window.swapchain);
        // A flip model swapchain is destroyed for real only once the context lets go of it; the
        // window gets no new one before. Flush keeps the app's bindings.
        device->immediate->Flush();
    }
}

static void apply_viewport (const R_D3D11Device* device, ID3D11DeviceContext* context)
{
    context->RSSetViewports(1, &device->viewport);
    context->RSSetScissorRects(1, &device->scissor);
}

static void apply_pipeline_state (R_D3D11Device* device, ID3D11DeviceContext* context, const R_D3D11Pipeline& pipeline)
{
    const FLOAT factor[4] = {};
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(pipeline.input_layout);
    context->VSSetShader(pipeline.vertex_shader, nullptr, 0);
    context->PSSetShader(pipeline.pixel_shader, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &device->push_buffer);
    context->PSSetConstantBuffers(0, 1, &device->push_buffer);
    context->OMSetBlendState(pipeline.blend, factor, 0xffffffffu);
    context->OMSetDepthStencilState(pipeline.depth, 0);
    context->RSSetState(device->rasterizer);
    device->vertex_dirty = true;
}

// Host visible buffers live in the shadow; what was flushed goes up when the buffer is bound,
// on the context that reads it
static b8 upload_buffer (ID3D11DeviceContext* context, R_D3D11Buffer& buffer)
{
    if (!buffer.dirty || !buffer.buffer || !buffer.shadow)
    {
        return true;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(buffer.buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        LOG_ERROR("Cannot map a buffer of %llu bytes", (unsigned long long)buffer.size);
        return false;
    }
    memory_copy(mapped.pData, buffer.shadow, (usize)buffer.written);
    context->Unmap(buffer.buffer, 0);
    buffer.dirty = false;
    return true;
}

static b8 prepare_draw (R_D3D11Device* device, R_CmdBufferHandle cmd)
{
    ID3D11DeviceContext*   context  = cmd_context(device, cmd);
    const R_D3D11Pipeline* pipeline = device->pipelines.try_get(device->bound_pipeline);
    if (!context || !pipeline || !device->framebuffers.is_valid(device->framebuffer))
    {
        ASSERT_MSG(false, "draw without a pipeline or outside a render pass");
        return false;
    }
    if (pipeline->vertex_stride == 0 || !device->vertex_dirty)
    {
        return true;
    }
    R_D3D11Buffer* buffer = device->buffers.try_get(device->vertex_buffer);
    if (!buffer || !buffer->buffer || !upload_buffer(context, *buffer))
    {
        ASSERT_MSG(false, "draw without a vertex buffer");
        return false;
    }
    const UINT stride = pipeline->vertex_stride;
    const UINT offset = (UINT)device->vertex_offset;
    context->IASetVertexBuffers(0, 1, &buffer->buffer, &stride, &offset);
    device->vertex_dirty = false;
    return true;
}

// Waits for everything the immediate context was given so far
static void wait_for_gpu (const R_D3D11Device* device)
{
    if (!device->idle_query)
    {
        return;
    }
    device->immediate->End(device->idle_query);
    BOOL done = FALSE;
    while (device->immediate->GetData(device->idle_query, &done, sizeof(done), 0) == S_FALSE)
    {
        os_sleep_ns(100000);
    }
}

static void resolve_readbacks (R_D3D11Device* device)
{
    for (u32 i = 0; i < device->readbacks_count; i++)
    {
        R_D3D11Readback&         readback = device->readbacks[i];
        const R_D3D11Buffer*     buffer   = device->buffers.try_get(readback.buffer);
        D3D11_MAPPED_SUBRESOURCE mapped   = {};
        if (buffer && buffer->shadow && SUCCEEDED(device->immediate->Map(readback.staging, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            const usize row = (usize)readback.width * readback.bytes_per_pixel;
            for (u32 y = 0; y < readback.height; y++)
            {
                memory_copy(buffer->shadow + row * y, (const u8*)mapped.pData + (usize)mapped.RowPitch * y, row);
            }
            device->immediate->Unmap(readback.staging, 0);
        }
        release(readback.staging);
    }
    device->readbacks_count = 0;
}

static ID3D11Texture2D* create_staging (const R_D3D11Device* device, const R_D3D11Image& image)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width                = image.width;
    desc.Height               = image.height;
    desc.MipLevels            = 1;
    desc.ArraySize            = 1;
    desc.Format               = dxgi(r_dxgi_format(image.format).resource);
    desc.SampleDesc.Count     = 1;
    desc.Usage                = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags       = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging  = nullptr;
    return SUCCEEDED(device->device->CreateTexture2D(&desc, nullptr, &staging)) ? staging : nullptr;
}

//
// DXGI, shared with render_d3d12.cpp
//

const R_DxgiFormat& r_dxgi_format (R_Format format)
{
    return r_dxgi_formats[(u32)format < ARRAY_SIZE(r_dxgi_formats) ? (u32)format : 0].dxgi;
}

R_Format r_dxgi_format_from_native (i64 format)
{
    for (u32 i = 1; i < ARRAY_SIZE(r_dxgi_formats); i++)
    {
        if (r_dxgi_formats[i].dxgi.resource != 0 && (i64)r_dxgi_formats[i].dxgi.typed == format)
        {
            return r_dxgi_formats[i].format;
        }
    }
    return R_Format::Unknown;
}

b8 r_dxgi_adapter_luid (u64& out_luid)
{
    out_luid                            = 0;
    const R_CreateDXGIFactory1 create   = load_create_factory();
    IDXGIFactory1*             factory  = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), (void**)&factory)))
    {
        LOG_ERROR("No DXGI factory");
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    const b8       found   = SUCCEEDED(factory->EnumAdapters1(0, &adapter));
    if (found)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        out_luid = luid_value(desc.AdapterLuid);
        adapter->Release();
    }
    factory->Release();
    return found;
}

u64 r_dxgi_device_luid (void* unknown_device)
{
    IUnknown*    unknown     = (IUnknown*)unknown_device;
    IDXGIDevice* dxgi_device = nullptr;
    if (unknown && SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device)))
    {
        IDXGIAdapter* adapter = nullptr;
        u64           luid    = 0;
        if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)))
        {
            DXGI_ADAPTER_DESC desc = {};
            adapter->GetDesc(&desc);
            luid = luid_value(desc.AdapterLuid);
            adapter->Release();
        }
        dxgi_device->Release();
        return luid;
    }
    return 0;
}

//
// Device
//

static void d3d11_destroy_device (R_Device* base_device)
{
    R_D3D11Device* device = d3d11_device(base_device);
    if (!device)
    {
        return;
    }
    for (R_D3D11WindowSwapchain& window : device->windows)
    {
        destroy_window_resource(device, window);
    }
    for (R_D3D11Image& image : device->images)
    {
        release_image(image);
    }
    for (R_D3D11Buffer& buffer : device->buffers)
    {
        release(buffer.buffer);
        os_memory_release(buffer.shadow);
    }
    for (R_D3D11Framebuffer& framebuffer : device->framebuffers)
    {
        release(framebuffer.target);
        release(framebuffer.depth);
    }
    for (R_D3D11Pipeline& pipeline : device->pipelines)
    {
        destroy_pipeline_resource(pipeline);
    }
    for (u32 i = 0; i < device->readbacks_count; i++)
    {
        release(device->readbacks[i].staging);
    }
    for (ID3D11DeviceContext*& context : device->contexts)
    {
        if (context != device->immediate)
        {
            release(context);
        }
        context = nullptr;
    }
    release(device->push_buffer);
    release(device->samplers[0]);
    release(device->samplers[1]);
    release(device->rasterizer);
    release(device->blit_vertex);
    release(device->blit_pixel);
    release(device->no_blend);
    release(device->no_color);
    release(device->no_depth);
    release(device->depth_overwrite);
    release(device->idle_query);
    release(device->state);
    release(device->immediate1);
    if (device->immediate)
    {
        device->immediate->Flush();
    }
    release(device->immediate);
    release(device->device);
    device->images.release();
    device->buffers.release();
    device->render_passes.release();
    device->framebuffers.release();
    device->pipelines.release();
    device->cmds.release();
    device->windows.release();
    os_memory_release(device);
}

static void d3d11_wait_idle (const R_Device* base_device)
{
    wait_for_gpu(d3d11_device(base_device));
}

// Images start in every layout at once
static void d3d11_flush_initial_layouts (R_Device* base_device) // NOLINT(misc-const-correctness): the backend table fixes the signature
{
    (void)base_device;
}

static u64 d3d11_queue_access_count (const R_Device* base_device)
{
    return d3d11_device(base_device)->queue_accesses;
}

static i64 d3d11_native_format (const R_Device* base_device, R_Format format)
{
    (void)base_device;
    return r_dxgi_format(format).resource != 0 ? (i64)r_dxgi_format(format).typed : 0;
}

static R_Format d3d11_format_from_native (const R_Device* base_device, i64 format)
{
    (void)base_device;
    return r_dxgi_format_from_native(format);
}

static u64 d3d11_adapter_luid (const R_Device* base_device)
{
    return r_dxgi_device_luid(d3d11_device(base_device)->device);
}

//
// Images
//

static R_ImageHandle d3d11_create_image (R_Device* base_device, const R_CreateImageInfo& info)
{
    R_D3D11Device*      device = d3d11_device(base_device);
    const u32           layers = info.layers ? info.layers : 1;
    const u32           mips   = info.mip_levels ? info.mip_levels : 1;
    const R_DxgiFormat& format = r_dxgi_format(info.format);
    const b8            depth  = r_format_is_depth(info.format);
    const b8            cube_invalid = info.cube_compatible && (info.width != info.height || layers % 6 != 0);
    if (info.width == 0 || info.height == 0 || layers > R_MAX_IMAGE_LAYERS || mips > r_max_mip_levels(info.width, info.height) || format.resource == 0 || cube_invalid || (info.vk_extra_create_flags | info.vk_extra_usage_flags) != 0)
    {
        LOG_ERROR("Invalid image: %ux%u, %u layers, %u mips, format %u", info.width, info.height, layers, mips, (u32)info.format);
        return NULL_HANDLE;
    }
    // Every image can be sampled: blits draw with it
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width                = info.width;
    desc.Height               = info.height;
    desc.MipLevels            = mips;
    desc.ArraySize            = layers;
    desc.Format               = dxgi(format.resource);
    desc.SampleDesc.Count     = 1;
    desc.Usage                = D3D11_USAGE_DEFAULT;
    desc.BindFlags            = D3D11_BIND_SHADER_RESOURCE;
    desc.BindFlags           |= (info.usage_flags & (u32)R_ImageUsageFlags::ColorAttachment) || (!depth && (info.usage_flags & (u32)R_ImageUsageFlags::TransferDst)) ? D3D11_BIND_RENDER_TARGET : 0;
    desc.BindFlags           |= (info.usage_flags & (u32)R_ImageUsageFlags::DepthStencilAttachment) ? D3D11_BIND_DEPTH_STENCIL : 0;
    desc.BindFlags           |= !depth && (info.usage_flags & (u32)R_ImageUsageFlags::UnorderedAccess) ? D3D11_BIND_UNORDERED_ACCESS : 0;
    desc.MiscFlags            = info.cube_compatible ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    R_D3D11Image image = { .width = info.width, .height = info.height, .layers = layers, .mip_levels = mips, .format = info.format };
    if (FAILED(device->device->CreateTexture2D(&desc, nullptr, &image.texture)) || !create_shader_views(device, image))
    {
        LOG_ERROR("Cannot create a %ux%u texture of %u layers, format %u", info.width, info.height, layers, (u32)info.format);
        release_image(image);
        return NULL_HANDLE;
    }
    return device->images.add(image);
}

static b8 d3d11_image_flags_supported (const R_Device* base_device, const R_CreateImageInfo& info)
{
    (void)base_device;
    return (info.vk_extra_create_flags | info.vk_extra_usage_flags) == 0;
}

static R_ImageHandle d3d11_import_image (R_Device* base_device, const R_ImportImageInfo& info)
{
    R_D3D11Device* device = d3d11_device(base_device);
    if (!info.native_image || info.width == 0 || info.height == 0 || r_dxgi_format(info.format).resource == 0)
    {
        LOG_ERROR("Invalid image import");
        return NULL_HANDLE;
    }
    ID3D11Texture2D* texture = (ID3D11Texture2D*)(uintptr_t)info.native_image;
    texture->AddRef();
    return device->images.add({ .texture = texture, .width = info.width, .height = info.height, .layers = 1, .mip_levels = 1, .format = info.format, .imported = true });
}

static void d3d11_destroy_image (R_Device* base_device, R_ImageHandle handle)
{
    R_D3D11Device* device = d3d11_device(base_device);
    R_D3D11Image*  image  = device->images.try_get(handle);
    if (!image)
    {
        return;
    }
    release_image(*image);
    device->images.remove(handle);
}

static b8 d3d11_is_valid_image (const R_Device* base_device, R_ImageHandle image)
{
    return d3d11_device(base_device)->images.is_valid(image);
}

static u64 d3d11_get_image_native (const R_Device* base_device, R_ImageHandle handle)
{
    const R_D3D11Image* image = d3d11_device(base_device)->images.try_get(handle);
    return image ? (u64)(uintptr_t)image->texture : 0;
}

static void d3d11_get_image_size (const R_Device* base_device, R_ImageHandle handle, u32& out_width, u32& out_height, u32& out_layers)
{
    const R_D3D11Image* image = d3d11_device(base_device)->images.try_get(handle);
    out_width  = image ? image->width : 0;
    out_height = image ? image->height : 0;
    out_layers = image ? image->layers : 0;
}

static R_Format d3d11_get_image_format (const R_Device* base_device, R_ImageHandle handle)
{
    const R_D3D11Image* image = d3d11_device(base_device)->images.try_get(handle);
    return image ? image->format : R_Format::Unknown;
}

//
// Buffers
//

static R_BufferHandle d3d11_create_buffer (R_Device* base_device, const R_CreateBufferInfo& info)
{
    R_D3D11Device* device = d3d11_device(base_device);
    if (info.size == 0 || info.usage_flags == 0 || info.size > 0xffffffffu)
    {
        LOG_ERROR("Invalid buffer of %llu bytes", (unsigned long long)info.size);
        return NULL_HANDLE;
    }
    R_D3D11Buffer buffer = { .size = info.size };
    UINT          bind   = 0;
    bind |= (info.usage_flags & (u32)R_BufferUsageFlags::Vertex) ? D3D11_BIND_VERTEX_BUFFER : 0;
    bind |= (info.usage_flags & (u32)R_BufferUsageFlags::Index) ? D3D11_BIND_INDEX_BUFFER : 0;
    bind |= (info.usage_flags & (u32)R_BufferUsageFlags::Uniform) ? D3D11_BIND_CONSTANT_BUFFER : 0;
    if (bind)
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth         = (UINT)info.size;
        desc.Usage             = info.host_visible ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
        desc.BindFlags         = bind;
        desc.CPUAccessFlags    = info.host_visible ? D3D11_CPU_ACCESS_WRITE : 0;
        if (FAILED(device->device->CreateBuffer(&desc, nullptr, &buffer.buffer)))
        {
            LOG_ERROR("Cannot create a buffer of %llu bytes", (unsigned long long)info.size);
            return NULL_HANDLE;
        }
    }
    if (info.host_visible)
    {
        buffer.shadow = (u8*)os_memory_alloc((usize)info.size);
        memory_zero(buffer.shadow, (usize)info.size);
    }
    return device->buffers.add(buffer);
}

static void d3d11_destroy_buffer (R_Device* base_device, R_BufferHandle handle)
{
    R_D3D11Device* device = d3d11_device(base_device);
    R_D3D11Buffer* buffer = device->buffers.try_get(handle);
    if (!buffer)
    {
        return;
    }
    release(buffer->buffer);
    os_memory_release(buffer->shadow);
    device->buffers.remove(handle);
}

static b8 d3d11_is_valid_buffer (const R_Device* base_device, R_BufferHandle buffer)
{
    return d3d11_device(base_device)->buffers.is_valid(buffer);
}

static void* d3d11_get_buffer_mapped_data (const R_Device* base_device, R_BufferHandle handle)
{
    const R_D3D11Buffer* buffer = d3d11_device(base_device)->buffers.try_get(handle);
    return buffer ? buffer->shadow : nullptr;
}

static void d3d11_flush_buffer (R_Device* base_device, R_BufferHandle handle, u64 offset, u64 size)
{
    R_D3D11Device* device = d3d11_device(base_device);
    R_D3D11Buffer* buffer = device->buffers.try_get(handle);
    if (!buffer || !buffer->shadow || offset > buffer->size || size > buffer->size - offset)
    {
        ASSERT_MSG(false, "invalid buffer flush");
        return;
    }
    buffer->written = zm::max(buffer->written, offset + size);
    buffer->dirty   = size > 0;
    if (buffer->dirty && device->vertex_buffer == handle)
    {
        device->vertex_dirty = true;
    }
}

//
// Render passes and framebuffers
//

static R_RenderPassHandle d3d11_create_render_pass (R_Device* base_device, const R_CreateRenderPassInfo& info)
{
    R_D3D11Device* device    = d3d11_device(base_device);
    const b8       has_color = info.color_format != R_Format::Unknown;
    const b8       has_depth = info.depth_format != R_Format::Unknown;
    if ((!has_color && !has_depth) || (has_color && (r_dxgi_format(info.color_format).resource == 0 || r_format_is_depth(info.color_format))) || (has_depth && (!r_format_is_depth(info.depth_format) || r_dxgi_format(info.depth_format).depth == 0)))
    {
        LOG_ERROR("Invalid render pass formats: color %u, depth %u", (u32)info.color_format, (u32)info.depth_format);
        return NULL_HANDLE;
    }
    return device->render_passes.add({ .format = info.color_format, .load_op = info.load_op, .depth_format = info.depth_format, .depth_load_op = info.depth_load_op });
}

static void d3d11_destroy_render_pass (R_Device* base_device, R_RenderPassHandle handle)
{
    d3d11_device(base_device)->render_passes.remove(handle);
}

static b8 d3d11_is_valid_render_pass (const R_Device* base_device, R_RenderPassHandle render_pass)
{
    return d3d11_device(base_device)->render_passes.is_valid(render_pass);
}

static R_FramebufferHandle d3d11_create_framebuffer (R_Device* base_device, const R_CreateFramebufferInfo& info)
{
    R_D3D11Device*           device = d3d11_device(base_device);
    const R_D3D11RenderPass* pass   = device->render_passes.try_get(info.render_pass);
    const R_D3D11Image*      color  = device->images.try_get(info.image);
    const R_D3D11Image*      depth  = device->images.try_get(info.depth_image);
    const R_D3D11Image*      sized  = color ? color : depth;
    const b8                 wants_color = pass && pass->format != R_Format::Unknown;
    const b8                 wants_depth = pass && pass->depth_format != R_Format::Unknown;
    if (!pass || !sized || wants_color != (color != nullptr) || wants_depth != (depth != nullptr) || (color && (info.layer >= color->layers || color->format != pass->format)) ||
        (depth && (info.depth_layer >= depth->layers || depth->format != pass->depth_format)) || (color && depth && (color->width != depth->width || color->height != depth->height)))
    {
        LOG_ERROR("Invalid framebuffer");
        return NULL_HANDLE;
    }
    R_D3D11Framebuffer framebuffer = { .width = sized->width, .height = sized->height, .render_pass = info.render_pass };
    framebuffer.target             = color ? create_target_view(device, *color, info.layer) : nullptr;
    framebuffer.depth              = depth ? create_depth_view(device, *depth, info.depth_layer) : nullptr;
    if ((color && !framebuffer.target) || (depth && !framebuffer.depth))
    {
        release(framebuffer.target);
        release(framebuffer.depth);
        return NULL_HANDLE;
    }
    return device->framebuffers.add(framebuffer);
}

static void d3d11_destroy_framebuffer (R_Device* base_device, R_FramebufferHandle handle)
{
    R_D3D11Device*      device      = d3d11_device(base_device);
    R_D3D11Framebuffer* framebuffer = device->framebuffers.try_get(handle);
    if (!framebuffer)
    {
        return;
    }
    release(framebuffer->target);
    release(framebuffer->depth);
    device->framebuffers.remove(handle);
}

static b8 d3d11_is_valid_framebuffer (const R_Device* base_device, R_FramebufferHandle framebuffer)
{
    return d3d11_device(base_device)->framebuffers.is_valid(framebuffer);
}

//
// Pipelines
//

static R_PipelineHandle d3d11_create_pipeline (R_Device* base_device, const R_CreatePipelineInfo& info)
{
    R_D3D11Device*           device = d3d11_device(base_device);
    const R_D3D11RenderPass* pass   = device->render_passes.try_get(info.render_pass);
    if (!pass || info.push_constants_size > R_MAX_PUSH_CONSTANTS || info.vertex_attributes_count > R_MAX_VERTEX_ATTRIBUTES || info.textures_count > R_MAX_TEXTURES ||
        ((info.depth_test || info.depth_write) && pass->depth_format == R_Format::Unknown))
    {
        LOG_ERROR("Invalid pipeline info");
        return NULL_HANDLE;
    }
    if (!info.vertex || !info.vertex->dxbc || !info.fragment || !info.fragment->dxbc)
    {
        LOG_ERROR("Shader without DXBC");
        return NULL_HANDLE;
    }
    D3D11_INPUT_ELEMENT_DESC elements[R_MAX_VERTEX_ATTRIBUTES] = {};
    for (u32 i = 0; i < info.vertex_attributes_count; i++)
    {
        const R_VertexAttribute& attribute = info.vertex_attributes[i];
        if ((u32)attribute.format >= (u32)R_Format::Count || r_dxgi_format(attribute.format).typed == 0 || r_format_is_depth(attribute.format) || attribute.location >= 32)
        {
            LOG_ERROR("Invalid vertex attribute %u", i);
            return NULL_HANDLE;
        }
        elements[i] = { "ATTRIBUTE", attribute.location, dxgi(r_dxgi_format(attribute.format).typed), 0, attribute.offset, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    }

    R_D3D11Pipeline pipeline = {
        .textures_count      = info.textures_count,
        .texture_type        = info.texture_type,
        .push_constants_size = info.push_constants_size,
        .vertex_stride       = info.vertex_stride,
    };
    R_BlendFactor factors[4];
    r_pipeline_blend_factors(info, factors);
    D3D11_BLEND_DESC blend = {};
    D3D11_RENDER_TARGET_BLEND_DESC& target = blend.RenderTarget[0];
    target.BlendEnable           = info.blend_enable;
    target.SrcBlend              = blend_factor(factors[0]);
    target.DestBlend             = blend_factor(factors[1]);
    target.BlendOp               = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha         = blend_factor(factors[2]);
    target.DestBlendAlpha        = blend_factor(factors[3]);
    target.BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = pass->format != R_Format::Unknown && !info.no_color_write ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable    = info.depth_test || info.depth_write;
    depth.DepthWriteMask = info.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc      = info.depth_test ? compare_func(info.depth_compare) : D3D11_COMPARISON_ALWAYS;
    const b8 created = SUCCEEDED(device->device->CreateVertexShader(info.vertex->dxbc, info.vertex->dxbc_size, nullptr, &pipeline.vertex_shader)) &&
                       SUCCEEDED(device->device->CreatePixelShader(info.fragment->dxbc, info.fragment->dxbc_size, nullptr, &pipeline.pixel_shader)) &&
                       (info.vertex_attributes_count == 0 || SUCCEEDED(device->device->CreateInputLayout(elements, info.vertex_attributes_count, info.vertex->dxbc, info.vertex->dxbc_size, &pipeline.input_layout))) &&
                       SUCCEEDED(device->device->CreateBlendState(&blend, &pipeline.blend)) && SUCCEEDED(device->device->CreateDepthStencilState(&depth, &pipeline.depth));
    if (!created)
    {
        LOG_ERROR("Cannot create the D3D11 pipeline");
        destroy_pipeline_resource(pipeline);
        return NULL_HANDLE;
    }
    return device->pipelines.add(pipeline);
}

static void d3d11_destroy_pipeline (R_Device* base_device, R_PipelineHandle handle)
{
    R_D3D11Device*   device   = d3d11_device(base_device);
    R_D3D11Pipeline* pipeline = device->pipelines.try_get(handle);
    if (!pipeline)
    {
        return;
    }
    destroy_pipeline_resource(*pipeline);
    device->pipelines.remove(handle);
}

static b8 d3d11_is_valid_pipeline (const R_Device* base_device, R_PipelineHandle pipeline)
{
    return d3d11_device(base_device)->pipelines.is_valid(pipeline);
}

//
// Window presentation
//

static b8 d3d11_device_supports_present (const R_Device* base_device, const R_WindowTarget& target)
{
    (void)base_device;
    return target.native_window != 0 && !target.wayland;
}

// A flip model swapchain of the app's device on the mirror window. It never waits for the
// display, so it never paces the app.
static R_WindowSwapchainHandle d3d11_create_window_swapchain (R_Device* base_device, const R_WindowTarget& target, u32 width, u32 height)
{
    R_D3D11Device* device = d3d11_device(base_device);
    if (!d3d11_device_supports_present(base_device, target) || width == 0 || height == 0)
    {
        LOG_WARNING("Cannot present to this window");
        return NULL_HANDLE;
    }
    IDXGIDevice*   dxgi_device = nullptr;
    IDXGIAdapter*  adapter     = nullptr;
    IDXGIFactory2* factory     = nullptr;
    if (FAILED(device->device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) || FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory)))
    {
        release(dxgi_device);
        release(adapter);
        LOG_WARNING("The D3D11 device has no DXGI 1.2 factory; the mirror window stays blank");
        return NULL_HANDLE;
    }
    const HWND            hwnd = (HWND)(uintptr_t)target.native_window;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width                 = width;
    desc.Height                = height;
    desc.Format                = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count      = 1;
    desc.BufferUsage           = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount           = 2;
    desc.SwapEffect            = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    R_D3D11WindowSwapchain window = { .width = width, .height = height };
    HRESULT result = factory->CreateSwapChainForHwnd(device->device, hwnd, &desc, nullptr, nullptr, &window.swapchain);
    if (FAILED(result))
    {
        // Windows 8 has flip model swapchains, but no discarding ones
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        result          = factory->CreateSwapChainForHwnd(device->device, hwnd, &desc, nullptr, nullptr, &window.swapchain);
    }
    if (SUCCEEDED(result))
    {
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }
    release(factory);
    release(adapter);
    release(dxgi_device);
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(result) || FAILED(window.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&buffer)))
    {
        LOG_WARNING("Cannot make a swapchain of the mirror window (0x%08lx)", (unsigned long)result);
        destroy_window_resource(device, window);
        return NULL_HANDLE;
    }
    // Flip model buffers take an sRGB view of their linear format
    window.image = device->images.add({ .texture = buffer, .width = width, .height = height, .layers = 1, .mip_levels = 1, .format = R_Format::B8G8R8A8_SRGB, .imported = true });
    return device->windows.add(window);
}

static void d3d11_destroy_window_swapchain (R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    R_D3D11Device*          device = d3d11_device(base_device);
    R_D3D11WindowSwapchain* window = device->windows.try_get(swapchain);
    if (!window)
    {
        return;
    }
    destroy_window_resource(device, *window);
    device->windows.remove(swapchain);
}

static b8 d3d11_is_valid_window_swapchain (const R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    return d3d11_device(base_device)->windows.is_valid(swapchain);
}

static R_Format d3d11_window_swapchain_format (const R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    return d3d11_device(base_device)->windows.is_valid(swapchain) ? R_Format::B8G8R8A8_SRGB : R_Format::Unknown;
}

static void d3d11_window_swapchain_size (const R_Device* base_device, R_WindowSwapchainHandle swapchain, u32& out_width, u32& out_height)
{
    const R_D3D11WindowSwapchain* window = d3d11_device(base_device)->windows.try_get(swapchain);
    out_width  = window ? window->width : 0;
    out_height = window ? window->height : 0;
}

static b8 d3d11_window_acquire (R_Device* base_device, R_WindowSwapchainHandle handle, R_ImageHandle& out_image)
{
    const R_D3D11WindowSwapchain* window = d3d11_device(base_device)->windows.try_get(handle);
    out_image                            = window ? window->image : R_ImageHandle{};
    return window != nullptr;
}

static b8 d3d11_window_present (R_Device* base_device, R_WindowSwapchainHandle handle)
{
    R_D3D11Device*                device = d3d11_device(base_device);
    const R_D3D11WindowSwapchain* window = device->windows.try_get(handle);
    if (!window)
    {
        return false;
    }
    device->queue_accesses++;
    const HRESULT result = window->swapchain->Present(0, 0);
    return SUCCEEDED(result) && result != DXGI_STATUS_OCCLUDED;
}

//
// Frames and immediate commands
//

static R_CmdBufferHandle begin_recording (R_D3D11Device* device, u32 slot)
{
    if (device->recording[slot])
    {
        ASSERT_MSG(false, "command buffer already recording");
        return NULL_HANDLE;
    }
    if (device->state)
    {
        device->immediate1->SwapDeviceContextState(device->state, &device->saved_state);
    }
    device->recording[slot] = true;
    device->bound_pipeline  = NULL_HANDLE;
    device->vertex_buffer   = NULL_HANDLE;
    device->framebuffer     = NULL_HANDLE;
    return device->cmd_handles[slot];
}

static b8 end_recording (R_D3D11Device* device, R_CmdBufferHandle cmd)
{
    const R_D3D11CmdBuffer* buffer = device->cmds.try_get(cmd);
    if (!buffer || !device->recording[buffer->slot])
    {
        ASSERT_MSG(false, "command buffer not recording");
        return false;
    }
    device->recording[buffer->slot] = false;
    device->queue_accesses++;
    if (device->state)
    {
        device->immediate1->SwapDeviceContextState(device->saved_state, nullptr);
        release(device->saved_state);
        return true;
    }
    ID3D11CommandList* list = nullptr;
    if (FAILED(device->contexts[buffer->slot]->FinishCommandList(FALSE, &list)))
    {
        LOG_ERROR("Cannot finish the D3D11 command list");
        return false;
    }
    device->immediate->ExecuteCommandList(list, TRUE);
    list->Release();
    return true;
}

static R_CmdBufferHandle d3d11_begin_frame (R_Device* base_device)
{
    R_D3D11Device* device = d3d11_device(base_device);
    const u32      slot   = device->frame_index;
    device->frame_index   = (device->frame_index + 1) % R_MAX_FRAMES_IN_FLIGHT;
    return begin_recording(device, slot);
}

static void d3d11_end_frame (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_D3D11Device* device = d3d11_device(base_device);
    if (end_recording(device, cmd) && device->readbacks_count > 0)
    {
        resolve_readbacks(device);
    }
}

static void d3d11_end_frame_present (R_Device* base_device, R_CmdBufferHandle cmd, R_WindowSwapchainHandle swapchain)
{
    (void)swapchain;
    d3d11_end_frame(base_device, cmd);
}

static R_CmdBufferHandle d3d11_begin_immediate (R_Device* base_device)
{
    return begin_recording(d3d11_device(base_device), R_MAX_FRAMES_IN_FLIGHT);
}

static void d3d11_end_immediate (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_D3D11Device* device = d3d11_device(base_device);
    if (end_recording(device, cmd))
    {
        resolve_readbacks(device);
        wait_for_gpu(device);
    }
}

static u64 d3d11_get_cmd_native (const R_Device* base_device, R_CmdBufferHandle cmd)
{
    (void)base_device;
    (void)cmd;
    return 0;
}

//
// Commands
//

static void d3d11_cmd_transition_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle handle, R_ImageLayout old_layout, R_ImageLayout new_layout)
{
    const R_D3D11Device* device = d3d11_device(base_device);
    (void)old_layout;
    (void)new_layout;
    if (!cmd_recording(device, cmd) || !device->images.is_valid(handle))
    {
        ASSERT_MSG(false, "invalid transition");
    }
}

static void d3d11_cmd_begin_render_pass (R_Device* base_device, R_CmdBufferHandle cmd, R_RenderPassHandle pass_handle, R_FramebufferHandle fb_handle, const R_ClearColor& clear)
{
    R_D3D11Device*            device  = d3d11_device(base_device);
    ID3D11DeviceContext*      context = cmd_context(device, cmd);
    const R_D3D11RenderPass*  pass    = device->render_passes.try_get(pass_handle);
    const R_D3D11Framebuffer* fb      = device->framebuffers.try_get(fb_handle);
    if (!context || !pass || !fb)
    {
        ASSERT_MSG(false, "invalid render pass or framebuffer");
        return;
    }
    device->framebuffer = fb_handle;
    context->OMSetRenderTargets(fb->target ? 1 : 0, &fb->target, fb->depth);
    device->viewport = { 0.0f, 0.0f, (FLOAT)fb->width, (FLOAT)fb->height, 0.0f, 1.0f };
    device->scissor  = { 0, 0, (LONG)fb->width, (LONG)fb->height };
    apply_viewport(device, context);
    if (fb->target && pass->load_op == R_LoadOp::Clear)
    {
        const FLOAT color[4] = { clear.r, clear.g, clear.b, clear.a };
        context->ClearRenderTargetView(fb->target, color);
    }
    if (fb->depth && pass->depth_load_op == R_LoadOp::Clear)
    {
        context->ClearDepthStencilView(fb->depth, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
}

static void d3d11_cmd_end_render_pass (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_D3D11Device*       device  = d3d11_device(base_device);
    ID3D11DeviceContext* context = cmd_context(device, cmd);
    if (!context)
    {
        ASSERT_MSG(false, "invalid command buffer");
        return;
    }
    device->framebuffer = NULL_HANDLE;
    context->OMSetRenderTargets(0, nullptr, nullptr);
}

static void d3d11_cmd_bind_pipeline (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle handle)
{
    R_D3D11Device*         device   = d3d11_device(base_device);
    ID3D11DeviceContext*   context  = cmd_context(device, cmd);
    const R_D3D11Pipeline* pipeline = device->pipelines.try_get(handle);
    if (!context || !pipeline)
    {
        ASSERT_MSG(false, "invalid pipeline binding");
        return;
    }
    device->bound_pipeline = handle;
    apply_pipeline_state(device, context, *pipeline);
}

static void d3d11_cmd_bind_textures (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle pipeline_handle, const R_ImageHandle* images, u32 count, R_Filter filter)
{
    R_D3D11Device*         device   = d3d11_device(base_device);
    ID3D11DeviceContext*   context  = cmd_context(device, cmd);
    const R_D3D11Pipeline* pipeline = device->pipelines.try_get(pipeline_handle);
    if (!context || !pipeline || !images || count == 0 || count != pipeline->textures_count)
    {
        ASSERT_MSG(false, "invalid texture binding");
        return;
    }
    ID3D11ShaderResourceView* views[R_MAX_TEXTURES]    = {}; // NOLINT(misc-const-correctness): D3D takes T* const*
    ID3D11SamplerState*       samplers[R_MAX_TEXTURES] = {}; // NOLINT(misc-const-correctness): D3D takes T* const*
    for (u32 i = 0; i < count; i++)
    {
        const R_D3D11Image* image = device->images.try_get(images[i]);
        views[i]                  = image ? (pipeline->texture_type == R_TextureType::Type2DArray ? image->array_view : image->view) : nullptr;
        samplers[i]               = device->samplers[filter == R_Filter::Linear ? 1 : 0];
        if (!views[i])
        {
            ASSERT_MSG(false, "invalid texture binding");
            return;
        }
    }
    context->PSSetShaderResources(0, count, views);
    context->PSSetSamplers(0, count, samplers);
}

// D3D11 clears whole views; a rectangle is a draw through a viewport with depth range [depth, depth],
// whose depth need not be the exact value a clear stores (Wine's D3D11 lands below it)
static void d3d11_cmd_clear_depth (R_Device* base_device, R_CmdBufferHandle cmd, const R_Rect& rect, f32 depth)
{
    R_D3D11Device*            device  = d3d11_device(base_device);
    ID3D11DeviceContext*      context = cmd_context(device, cmd);
    const R_D3D11Framebuffer* fb      = device->framebuffers.try_get(device->framebuffer);
    if (!context || !fb || !fb->depth)
    {
        ASSERT_MSG(false, "depth clear outside a render pass with depth");
        return;
    }
    if (rect.x == 0 && rect.y == 0 && rect.width == fb->width && rect.height == fb->height)
    {
        context->ClearDepthStencilView(fb->depth, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, depth, 0);
        return;
    }
    const FLOAT          factor[4] = {};
    const D3D11_VIEWPORT viewport  = { (FLOAT)rect.x, (FLOAT)rect.y, (FLOAT)rect.width, (FLOAT)rect.height, depth, depth };
    const D3D11_RECT     scissor   = { rect.x, rect.y, rect.x + (LONG)rect.width, rect.y + (LONG)rect.height };
    const R_BlitPush     push      = { .src_rect = { 0.0f, 0.0f, 1.0f, 1.0f } };
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context->Map(device->push_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memory_copy(mapped.pData, &push, sizeof(push));
        context->Unmap(device->push_buffer, 0);
    }
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(device->blit_vertex, nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &device->push_buffer);
    context->OMSetBlendState(device->no_color, factor, 0xffffffffu);
    context->OMSetDepthStencilState(device->depth_overwrite, 0);
    context->RSSetState(device->rasterizer);
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &scissor);
    context->Draw(3, 0);
    apply_viewport(device, context);
    const R_D3D11Pipeline* pipeline = device->pipelines.try_get(device->bound_pipeline);
    if (pipeline)
    {
        apply_pipeline_state(device, context, *pipeline);
    }
}

static void d3d11_cmd_push_constants (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle handle, const void* data, u32 size)
{
    R_D3D11Device*           device   = d3d11_device(base_device);
    ID3D11DeviceContext*     context  = cmd_context(device, cmd);
    const R_D3D11Pipeline*   pipeline = device->pipelines.try_get(handle);
    D3D11_MAPPED_SUBRESOURCE mapped   = {};
    if (!context || !pipeline || !data || size == 0 || size > pipeline->push_constants_size)
    {
        ASSERT_MSG(false, "invalid push constants");
        return;
    }
    if (FAILED(context->Map(device->push_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        LOG_ERROR("Cannot map the push constants");
        return;
    }
    memory_copy(mapped.pData, data, size);
    context->Unmap(device->push_buffer, 0);
}

static void d3d11_cmd_set_viewport (R_Device* base_device, R_CmdBufferHandle cmd, const R_Viewport& viewport)
{
    R_D3D11Device*       device  = d3d11_device(base_device);
    ID3D11DeviceContext* context = cmd_context(device, cmd);
    if (!context)
    {
        ASSERT_MSG(false, "invalid command buffer");
        return;
    }
    device->viewport = { viewport.x, viewport.y, viewport.width, viewport.height, 0.0f, 1.0f };
    context->RSSetViewports(1, &device->viewport);
}

static void d3d11_cmd_set_scissor (R_Device* base_device, R_CmdBufferHandle cmd, const R_Rect& scissor)
{
    R_D3D11Device*       device  = d3d11_device(base_device);
    ID3D11DeviceContext* context = cmd_context(device, cmd);
    if (!context)
    {
        ASSERT_MSG(false, "invalid command buffer");
        return;
    }
    device->scissor = { scissor.x, scissor.y, scissor.x + (LONG)scissor.width, scissor.y + (LONG)scissor.height };
    context->RSSetScissorRects(1, &device->scissor);
}

static void d3d11_cmd_bind_vertex_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset)
{
    R_D3D11Device* device = d3d11_device(base_device);
    if (!cmd_recording(device, cmd) || !device->buffers.is_valid(handle))
    {
        ASSERT_MSG(false, "invalid vertex buffer binding");
        return;
    }
    device->vertex_buffer = handle;
    device->vertex_offset = offset;
    device->vertex_dirty  = true;
}

static void d3d11_cmd_bind_index_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset, b8 u16_indices)
{
    R_D3D11Device*       device  = d3d11_device(base_device);
    ID3D11DeviceContext* context = cmd_context(device, cmd);
    R_D3D11Buffer*       buffer  = device->buffers.try_get(handle);
    if (!context || !buffer || !buffer->buffer || !upload_buffer(context, *buffer))
    {
        ASSERT_MSG(false, "invalid index buffer binding");
        return;
    }
    context->IASetIndexBuffer(buffer->buffer, u16_indices ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, (UINT)offset);
}

static void d3d11_cmd_draw (R_Device* base_device, R_CmdBufferHandle cmd, u32 vertex_count, u32 first_vertex)
{
    R_D3D11Device* device = d3d11_device(base_device);
    if (prepare_draw(device, cmd))
    {
        cmd_context(device, cmd)->Draw(vertex_count, first_vertex);
    }
}

static void d3d11_cmd_draw_indexed (R_Device* base_device, R_CmdBufferHandle cmd, u32 index_count, u32 first_index, i32 vertex_offset)
{
    R_D3D11Device* device = d3d11_device(base_device);
    if (prepare_draw(device, cmd))
    {
        cmd_context(device, cmd)->DrawIndexed(index_count, first_index, vertex_offset);
    }
}

static void d3d11_cmd_copy_buffer_to_image (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle buffer_handle, R_ImageHandle image_handle, u32 layer)
{
    R_D3D11Device*       device  = d3d11_device(base_device);
    ID3D11DeviceContext* context = cmd_context(device, cmd);
    const R_D3D11Buffer* buffer  = device->buffers.try_get(buffer_handle);
    const R_D3D11Image*  image   = device->images.try_get(image_handle);
    if (!context || !buffer || !buffer->shadow || !image || layer >= image->layers || r_format_is_depth(image->format) || buffer->size < (u64)image->width * image->height * r_format_bytes_per_pixel(image->format))
    {
        ASSERT_MSG(false, "invalid buffer to image copy");
        return;
    }
    context->UpdateSubresource(image->texture, subresource(*image, layer), nullptr, buffer->shadow, image->width * r_format_bytes_per_pixel(image->format), 0);
}

static void d3d11_cmd_copy_image_to_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle image_handle, u32 layer, R_BufferHandle buffer_handle)
{
    R_D3D11Device*       device  = d3d11_device(base_device);
    ID3D11DeviceContext* context = cmd_context(device, cmd);
    const R_D3D11Buffer* buffer  = device->buffers.try_get(buffer_handle);
    const R_D3D11Image*  image   = device->images.try_get(image_handle);
    if (!context || !buffer || !buffer->shadow || !image || layer >= image->layers || buffer->size < (u64)image->width * image->height * r_format_bytes_per_pixel(image->format) || device->readbacks_count == R_D3D11_MAX_READBACKS)
    {
        ASSERT_MSG(false, "invalid image to buffer copy");
        return;
    }
    ID3D11Texture2D* staging = create_staging(device, *image);
    if (!staging)
    {
        LOG_ERROR("Cannot create a staging texture of %ux%u", image->width, image->height);
        return;
    }
    context->CopySubresourceRegion(staging, 0, 0, 0, 0, image->texture, subresource(*image, layer), nullptr);
    device->readbacks[device->readbacks_count++] = { .staging = staging, .buffer = buffer_handle, .width = image->width, .height = image->height, .bytes_per_pixel = r_format_bytes_per_pixel(image->format) };
}

static void d3d11_cmd_clear_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle handle, const R_ClearColor& color)
{
    R_D3D11Device*       device  = d3d11_device(base_device);
    ID3D11DeviceContext* context = cmd_context(device, cmd);
    R_D3D11Image*        image   = device->images.try_get(handle);
    if (!context || !image || r_format_is_depth(image->format))
    {
        ASSERT_MSG(false, "invalid clear");
        return;
    }
    const FLOAT value[4] = { color.r, color.g, color.b, color.a };
    for (u32 layer = 0; layer < image->layers; layer++)
    {
        ID3D11RenderTargetView* view = layer == 0 ? layer0_target(device, *image) : create_target_view(device, *image, layer);
        if (view)
        {
            context->ClearRenderTargetView(view, value);
        }
        if (layer > 0)
        {
            release(view);
        }
    }
}

// A draw of the source through the blit shaders into the destination rectangle
static void d3d11_cmd_blit_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle src_handle, u32 src_layer, R_ImageHandle dst_handle, const R_Rect& dst_rect, R_Filter filter)
{
    R_D3D11Device*       device  = d3d11_device(base_device);
    ID3D11DeviceContext* context = cmd_context(device, cmd);
    const R_D3D11Image*  src     = device->images.try_get(src_handle);
    R_D3D11Image*        dst     = device->images.try_get(dst_handle);
    ID3D11RenderTargetView* target = dst ? layer0_target(device, *dst) : nullptr; // NOLINT(misc-const-correctness): D3D takes T* const*
    if (!context || !src || !dst || !target || !src->array_view || src_layer >= src->layers || r_format_is_depth(src->format) || r_format_is_depth(dst->format))
    {
        ASSERT_MSG(false, "invalid blit");
        return;
    }
    const FLOAT          factor[4] = {};
    const D3D11_VIEWPORT viewport  = { (FLOAT)dst_rect.x, (FLOAT)dst_rect.y, (FLOAT)dst_rect.width, (FLOAT)dst_rect.height, 0.0f, 1.0f };
    const D3D11_RECT     scissor   = { dst_rect.x, dst_rect.y, dst_rect.x + (LONG)dst_rect.width, dst_rect.y + (LONG)dst_rect.height };
    const R_BlitPush     push      = { .src_rect = { 0.0f, 0.0f, 1.0f, 1.0f }, .layer = (i32)src_layer };
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(device->push_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        LOG_ERROR("Cannot map the push constants");
        return;
    }
    memory_copy(mapped.pData, &push, sizeof(push));
    context->Unmap(device->push_buffer, 0);
    ID3D11SamplerState* sampler = device->samplers[filter == R_Filter::Linear ? 1 : 0]; // NOLINT(misc-const-correctness): D3D takes T* const*
    context->OMSetRenderTargets(1, &target, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(device->blit_vertex, nullptr, 0);
    context->PSSetShader(device->blit_pixel, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &device->push_buffer);
    context->PSSetConstantBuffers(0, 1, &device->push_buffer);
    context->PSSetShaderResources(0, 1, &src->array_view);
    context->PSSetSamplers(0, 1, &sampler);
    context->OMSetBlendState(device->no_blend, factor, 0xffffffffu);
    context->OMSetDepthStencilState(device->no_depth, 0);
    context->RSSetState(device->rasterizer);
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &scissor);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr; // NOLINT(misc-const-correctness): D3D takes T* const*
    context->PSSetShaderResources(0, 1, &none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    device->bound_pipeline = NULL_HANDLE;
}

static b8 d3d11_read_image (R_Device* base_device, R_ImageHandle handle, u32 layer, R_ImageLayout layout, void* out_pixels, usize out_size)
{
    R_D3D11Device*      device = d3d11_device(base_device);
    const R_D3D11Image* image  = device->images.try_get(handle);
    (void)layout;
    const u32 bytes = image ? r_format_bytes_per_pixel(image->format) : 0;
    if (!image || !out_pixels || layer >= image->layers || out_size < (usize)image->width * image->height * bytes)
    {
        return false;
    }
    ID3D11Texture2D* staging = create_staging(device, *image);
    if (!staging)
    {
        LOG_ERROR("Cannot create a staging texture of %ux%u", image->width, image->height);
        return false;
    }
    device->queue_accesses++;
    device->immediate->CopySubresourceRegion(staging, 0, 0, 0, 0, image->texture, subresource(*image, layer), nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const b8                 read   = SUCCEEDED(device->immediate->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    if (read)
    {
        const usize row = (usize)image->width * bytes;
        for (u32 y = 0; y < image->height; y++)
        {
            memory_copy((u8*)out_pixels + row * y, (const u8*)mapped.pData + (usize)mapped.RowPitch * y, row);
        }
        device->immediate->Unmap(staging, 0);
    }
    staging->Release();
    return read;
}

//
// Backend
//

const static R_BackendFns d3d11_fns = {
    .destroy_device            = d3d11_destroy_device,
    .wait_idle                 = d3d11_wait_idle,
    .flush_initial_layouts     = d3d11_flush_initial_layouts,
    .queue_access_count        = d3d11_queue_access_count,
    .native_format             = d3d11_native_format,
    .format_from_native        = d3d11_format_from_native,
    .adapter_luid              = d3d11_adapter_luid,
    .create_image              = d3d11_create_image,
    .image_flags_supported     = d3d11_image_flags_supported,
    .import_image              = d3d11_import_image,
    .destroy_image             = d3d11_destroy_image,
    .is_valid_image            = d3d11_is_valid_image,
    .get_image_native          = d3d11_get_image_native,
    .get_image_size            = d3d11_get_image_size,
    .get_image_format          = d3d11_get_image_format,
    .create_buffer             = d3d11_create_buffer,
    .destroy_buffer            = d3d11_destroy_buffer,
    .is_valid_buffer           = d3d11_is_valid_buffer,
    .get_buffer_mapped_data    = d3d11_get_buffer_mapped_data,
    .flush_buffer              = d3d11_flush_buffer,
    .create_render_pass        = d3d11_create_render_pass,
    .destroy_render_pass       = d3d11_destroy_render_pass,
    .is_valid_render_pass      = d3d11_is_valid_render_pass,
    .create_framebuffer        = d3d11_create_framebuffer,
    .destroy_framebuffer       = d3d11_destroy_framebuffer,
    .is_valid_framebuffer      = d3d11_is_valid_framebuffer,
    .create_pipeline           = d3d11_create_pipeline,
    .destroy_pipeline          = d3d11_destroy_pipeline,
    .is_valid_pipeline         = d3d11_is_valid_pipeline,
    .device_supports_present   = d3d11_device_supports_present,
    .create_window_swapchain   = d3d11_create_window_swapchain,
    .destroy_window_swapchain  = d3d11_destroy_window_swapchain,
    .is_valid_window_swapchain = d3d11_is_valid_window_swapchain,
    .window_swapchain_format   = d3d11_window_swapchain_format,
    .window_swapchain_size     = d3d11_window_swapchain_size,
    .window_acquire            = d3d11_window_acquire,
    .window_present            = d3d11_window_present,
    .begin_frame               = d3d11_begin_frame,
    .end_frame                 = d3d11_end_frame,
    .end_frame_present         = d3d11_end_frame_present,
    .begin_immediate           = d3d11_begin_immediate,
    .end_immediate             = d3d11_end_immediate,
    .get_cmd_native            = d3d11_get_cmd_native,
    .cmd_transition_image      = d3d11_cmd_transition_image,
    .cmd_begin_render_pass     = d3d11_cmd_begin_render_pass,
    .cmd_end_render_pass       = d3d11_cmd_end_render_pass,
    .cmd_bind_pipeline         = d3d11_cmd_bind_pipeline,
    .cmd_bind_textures         = d3d11_cmd_bind_textures,
    .cmd_clear_depth           = d3d11_cmd_clear_depth,
    .cmd_push_constants        = d3d11_cmd_push_constants,
    .cmd_set_viewport          = d3d11_cmd_set_viewport,
    .cmd_set_scissor           = d3d11_cmd_set_scissor,
    .cmd_bind_vertex_buffer    = d3d11_cmd_bind_vertex_buffer,
    .cmd_bind_index_buffer     = d3d11_cmd_bind_index_buffer,
    .cmd_draw                  = d3d11_cmd_draw,
    .cmd_draw_indexed          = d3d11_cmd_draw_indexed,
    .cmd_copy_buffer_to_image  = d3d11_cmd_copy_buffer_to_image,
    .cmd_copy_image_to_buffer  = d3d11_cmd_copy_image_to_buffer,
    .cmd_clear_image           = d3d11_cmd_clear_image,
    .cmd_blit_image            = d3d11_cmd_blit_image,
    .read_image                = d3d11_read_image,
};

// Deferred contexts where the device has them, else a context state of our own
static b8 create_contexts (R_D3D11Device* device)
{
    for (u32 i = 0; i < R_D3D11_CONTEXTS; i++)
    {
        if (FAILED(device->device->CreateDeferredContext(0, &device->contexts[i])))
        {
            break;
        }
    }
    if (device->contexts[R_D3D11_CONTEXTS - 1])
    {
        return true;
    }
    for (ID3D11DeviceContext*& context : device->contexts)
    {
        release(context);
    }
    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->device->QueryInterface(__uuidof(ID3D11Device1), (void**)&device1)) || FAILED(device->immediate->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&device->immediate1)))
    {
        release(device1);
        LOG_ERROR("The D3D11 device has neither deferred contexts nor D3D11.1 context states");
        return false;
    }
    const D3D_FEATURE_LEVEL level  = device->device->GetFeatureLevel();
    const UINT              flags  = (device->device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    const HRESULT           result = device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &device->state);
    device1->Release();
    if (FAILED(result))
    {
        LOG_ERROR("Cannot create a D3D11 context state (0x%08lx)", (unsigned long)result);
        return false;
    }
    for (ID3D11DeviceContext*& context : device->contexts)
    {
        context = device->immediate;
    }
    LOG_INFO("The D3D11 device is single threaded; commands run in a context state of their own");
    return true;
}

static b8 create_shared_state (R_D3D11Device* device)
{
    D3D11_BUFFER_DESC push = {};
    push.ByteWidth         = R_MAX_PUSH_CONSTANTS;
    push.Usage             = D3D11_USAGE_DYNAMIC;
    push.BindFlags         = D3D11_BIND_CONSTANT_BUFFER;
    push.CPUAccessFlags    = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sampler = {};
    sampler.AddressU           = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV           = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW           = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc     = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD             = D3D11_FLOAT32_MAX;
    D3D11_SAMPLER_DESC point   = sampler;
    point.Filter               = D3D11_FILTER_MIN_MAG_MIP_POINT;
    D3D11_SAMPLER_DESC linear  = sampler;
    linear.Filter              = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode              = D3D11_FILL_SOLID;
    raster.CullMode              = D3D11_CULL_NONE;
    raster.DepthClipEnable       = TRUE;
    raster.ScissorEnable         = TRUE;
    D3D11_BLEND_DESC no_blend                      = {};
    no_blend.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
    no_blend.RenderTarget[0].DestBlend             = D3D11_BLEND_ZERO;
    no_blend.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    no_blend.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    no_blend.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
    no_blend.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    no_blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_BLEND_DESC no_color                      = no_blend;
    no_color.RenderTarget[0].RenderTargetWriteMask = 0;
    D3D11_DEPTH_STENCIL_DESC no_depth              = {};
    no_depth.DepthWriteMask                        = D3D11_DEPTH_WRITE_MASK_ZERO;
    no_depth.DepthFunc                             = D3D11_COMPARISON_ALWAYS;
    D3D11_DEPTH_STENCIL_DESC overwrite             = {};
    overwrite.DepthEnable                          = TRUE;
    overwrite.DepthWriteMask                       = D3D11_DEPTH_WRITE_MASK_ALL;
    overwrite.DepthFunc                            = D3D11_COMPARISON_ALWAYS;
    const D3D11_QUERY_DESC idle                    = { D3D11_QUERY_EVENT, 0 };
    ID3D11Device* d = device->device;
    return SUCCEEDED(d->CreateBuffer(&push, nullptr, &device->push_buffer)) && SUCCEEDED(d->CreateSamplerState(&point, &device->samplers[0])) &&
           SUCCEEDED(d->CreateSamplerState(&linear, &device->samplers[1])) && SUCCEEDED(d->CreateRasterizerState(&raster, &device->rasterizer)) &&
           SUCCEEDED(d->CreateBlendState(&no_blend, &device->no_blend)) && SUCCEEDED(d->CreateBlendState(&no_color, &device->no_color)) &&
           SUCCEEDED(d->CreateDepthStencilState(&no_depth, &device->no_depth)) && SUCCEEDED(d->CreateDepthStencilState(&overwrite, &device->depth_overwrite)) &&
           SUCCEEDED(d->CreateQuery(&idle, &device->idle_query)) &&
           SUCCEEDED(d->CreateVertexShader(blit_vert_shader.dxbc, blit_vert_shader.dxbc_size, nullptr, &device->blit_vertex)) &&
           SUCCEEDED(d->CreatePixelShader(blit_frag_shader.dxbc, blit_frag_shader.dxbc_size, nullptr, &device->blit_pixel));
}

R_Device* r_d3d11_create_device (const R_DeviceCreateInfo& create_info)
{
    ID3D11Device* app_device = (ID3D11Device*)create_info.d3d11.device;
    if (!app_device)
    {
        LOG_ERROR("Render device needs the app's ID3D11Device");
        return nullptr;
    }
    if (app_device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
    {
        LOG_ERROR("The D3D11 device has feature level 0x%x; puppetxr needs 11_0", (u32)app_device->GetFeatureLevel());
        return nullptr;
    }
    if (!blit_vert_shader.dxbc || !blit_frag_shader.dxbc)
    {
        LOG_ERROR("This build has no DXBC shaders");
        return nullptr;
    }
    R_D3D11Device* device = (R_D3D11Device*)os_memory_alloc(sizeof(R_D3D11Device));
    memory_zero(device, sizeof(R_D3D11Device));
    device->base   = { .fns = &d3d11_fns, .backend = R_Backend::D3D11 };
    device->device = app_device;
    app_device->AddRef();
    app_device->GetImmediateContext(&device->immediate);
    if (!create_contexts(device) || !create_shared_state(device))
    {
        LOG_ERROR("Cannot set up the D3D11 device");
        d3d11_destroy_device(&device->base);
        return nullptr;
    }
    for (u32 i = 0; i < ARRAY_SIZE(device->cmd_handles); i++)
    {
        device->cmd_handles[i] = device->cmds.add({ .slot = i });
    }
    return &device->base;
}
