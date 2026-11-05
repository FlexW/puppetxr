#include "render_internal.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include "blocks.h"
#include "blit.vert.h"
#include "blit.frag.h"

//
// Types
//
// Every command buffer slot (the frames, then the immediate buffer) has its own allocator, list,
// fence value and range of the shader visible descriptor heap; a slot is reused once its fence
// passed. Image layouts are resource states, tracked per image in recording order, which is the
// order the app's queue runs them in. Render passes move their attachments from the initial to
// the final layout as Vulkan's do.

#define R_D3D12_SLOTS              (R_MAX_FRAMES_IN_FLIGHT + 1)
#define R_D3D12_SLOT_DESCRIPTORS   1024
#define R_D3D12_VIEW_DESCRIPTORS   4096
#define R_D3D12_TARGET_DESCRIPTORS 1024
#define R_D3D12_DEPTH_DESCRIPTORS  512
#define R_D3D12_MAX_GARBAGE        16
#define R_D3D12_MAX_READBACKS      8
#define R_D3D12_MAX_BLIT_FORMATS   4
#define R_D3D12_WINDOW_BUFFERS     2
#define R_D3D12_NO_DESCRIPTOR      0xffffffffu

namespace
{

// A CPU descriptor heap with a free list
struct R_D3D12Descriptors
{
    ID3D12DescriptorHeap* heap;
    usize                 start;
    u32                   increment;
    u32                   capacity;
    u32                   next;
    Array<u32>            free_slots;
};

struct R_D3D12Image
{
    ID3D12Resource*       resource;
    D3D12_RESOURCE_STATES state;
    u32                   view;       // Texture2D, single-layer images only
    u32                   array_view; // Texture2DArray over every layer
    u32                   target;     // layer 0, for clears and blits; made on first use
    u32                   width;
    u32                   height;
    u32                   layers;
    u32                   mip_levels;
    R_Format              format;
    b8                    imported;
};

struct R_D3D12Buffer
{
    ID3D12Resource* resource; // an upload heap buffer for host visible device buffers; null for a buffer that only stages transfers
    u8*             mapped;   // the upload buffer mapped, or host memory of a staging buffer
    u64             size;
};

struct R_D3D12RenderPass
{
    R_Format      format;
    R_LoadOp      load_op;
    R_ImageLayout initial_layout;
    R_ImageLayout final_layout;
    R_Format      depth_format;
    R_LoadOp      depth_load_op;
    R_ImageLayout depth_initial_layout;
    R_ImageLayout depth_final_layout;
};

struct R_D3D12Framebuffer
{
    u32                target = {}; // R_D3D12_NO_DESCRIPTOR without color
    u32                depth = {};  // R_D3D12_NO_DESCRIPTOR without depth
    R_ImageHandle      image = {};
    R_ImageHandle      depth_image = {};
    u32                width = {};
    u32                height = {};
    R_RenderPassHandle render_pass = {};
};

struct R_D3D12Pipeline
{
    ID3D12PipelineState* state;
    u32                  textures_count;
    R_TextureType        texture_type;
    u32                  push_constants_size;
    u32                  vertex_stride;
};

struct R_D3D12WindowSwapchain
{
    IDXGISwapChain3* swapchain = {};
    R_ImageHandle    images[R_D3D12_WINDOW_BUFFERS];
    u32              width = {};
    u32              height = {};
};

struct R_D3D12CmdBuffer
{
    u32 slot;
};

struct R_D3D12Readback
{
    ID3D12Resource*                    resource = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    R_BufferHandle                     buffer = {};
    u32                                bytes_per_pixel = {};
};

struct R_D3D12Slot
{
    ID3D12CommandAllocator*    allocator = {};
    ID3D12GraphicsCommandList* list = {};
    u64                        fence_value = {};
    u32                        descriptors_used = {};
    ID3D12Resource*            garbage[R_D3D12_MAX_GARBAGE] = {}; // transient uploads, released once the fence passed
    u32                        garbage_count = {};
    R_D3D12Readback            readbacks[R_D3D12_MAX_READBACKS] = {};
    u32                        readbacks_count = {};
};

struct R_D3D12BlitState
{
    DXGI_FORMAT          format;
    ID3D12PipelineState* state;
};

struct R_D3D12Device
{
    R_Device                                         base = {};
    ID3D12Device*                                    device = {};
    ID3D12CommandQueue*                              queue = {};
    ID3D12Fence*                                     fence = {};
    u64                                              fence_value = {};
    u64                                              queue_accesses = {};
    ID3D12RootSignature*                             root_signature = {};
    R_D3D12Descriptors                               views = {};
    R_D3D12Descriptors                               targets = {};
    R_D3D12Descriptors                               depths = {};
    ID3D12DescriptorHeap*                            shader_views = {};    // shader visible, one range per slot
    ID3D12DescriptorHeap*                            shader_samplers = {}; // point, point, linear, linear
    u32                                              null_view = {};
    R_D3D12Slot                                      slots[R_D3D12_SLOTS] = {};
    b8                                               recording[R_D3D12_SLOTS] = {};
    R_CmdBufferHandle                                cmd_handles[R_D3D12_SLOTS] = {};
    u32                                              frame_index = {};
    R_D3D12BlitState                                 blits[R_D3D12_MAX_BLIT_FORMATS] = {};

    R_PipelineHandle                                 bound_pipeline = {};
    R_BufferHandle                                   vertex_buffer = {};
    u64                                              vertex_offset = {};
    b8                                               vertex_dirty = {};
    R_FramebufferHandle                              framebuffer = {};

    SparseArray<R_D3D12Image, R_Image>               images = {};
    SparseArray<R_D3D12Buffer, R_Buffer>             buffers = {};
    SparseArray<R_D3D12RenderPass, R_RenderPass>     render_passes = {};
    SparseArray<R_D3D12Framebuffer, R_Framebuffer>   framebuffers = {};
    SparseArray<R_D3D12Pipeline, R_Pipeline>         pipelines = {};
    SparseArray<R_D3D12CmdBuffer, R_CmdBuffer>       cmds = {};
    SparseArray<R_D3D12WindowSwapchain, R_WindowSwapchain> windows = {};
};

typedef HRESULT (WINAPI* R_D3D12SerializeRootSignature) (const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob, ID3DBlob** error);

} // namespace

//
// Statics
//

// Without the operator| of DEFINE_ENUM_FLAG_OPERATORS, whose cast back to the enum the analyzer
// takes for a value out of its range
static const D3D12_CLEAR_FLAGS R_D3D12_CLEAR_DEPTH_STENCIL = (D3D12_CLEAR_FLAGS)(D3D12_CLEAR_FLAG_DEPTH + D3D12_CLEAR_FLAG_STENCIL); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)

static R_D3D12Device* d3d12_device (R_Device* device)
{
    return (R_D3D12Device*)device;
}

static const R_D3D12Device* d3d12_device (const R_Device* device)
{
    return (const R_D3D12Device*)device;
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

static D3D12_COMPARISON_FUNC compare_func (R_CompareOp op)
{
    switch (op)
    {
    case R_CompareOp::LessOrEqual:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case R_CompareOp::Always:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    case R_CompareOp::Never:
        return D3D12_COMPARISON_FUNC_NEVER;
    case R_CompareOp::Less:
        return D3D12_COMPARISON_FUNC_LESS;
    case R_CompareOp::Equal:
        return D3D12_COMPARISON_FUNC_EQUAL;
    case R_CompareOp::Greater:
        return D3D12_COMPARISON_FUNC_GREATER;
    case R_CompareOp::NotEqual:
        return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case R_CompareOp::GreaterOrEqual:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    }
    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
}

static D3D12_BLEND blend_factor (R_BlendFactor factor)
{
    switch (factor)
    {
    case R_BlendFactor::Zero:
        return D3D12_BLEND_ZERO;
    case R_BlendFactor::One:
        return D3D12_BLEND_ONE;
    case R_BlendFactor::SrcAlpha:
        return D3D12_BLEND_SRC_ALPHA;
    case R_BlendFactor::OneMinusSrcAlpha:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case R_BlendFactor::DstAlpha:
        return D3D12_BLEND_DEST_ALPHA;
    case R_BlendFactor::OneMinusDstAlpha:
        return D3D12_BLEND_INV_DEST_ALPHA;
    }
    return D3D12_BLEND_ONE;
}

static DXGI_FORMAT dxgi (u32 format)
{
    return (DXGI_FORMAT)format;
}

static D3D12_RESOURCE_STATES layout_state (R_ImageLayout layout)
{
    switch (layout)
    {
    case R_ImageLayout::ColorAttachment:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case R_ImageLayout::DepthStencilAttachment:
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case R_ImageLayout::ShaderReadOnly:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case R_ImageLayout::TransferSrc:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case R_ImageLayout::TransferDst:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    case R_ImageLayout::Undefined:
    case R_ImageLayout::General:
    case R_ImageLayout::Present:
        break;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

static D3D12_HEAP_PROPERTIES heap_properties (D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type                  = type;
    return properties;
}

static D3D12_RESOURCE_DESC buffer_desc (u64 size)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension           = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width               = size;
    desc.Height              = 1;
    desc.DepthOrArraySize    = 1;
    desc.MipLevels           = 1;
    desc.SampleDesc.Count    = 1;
    desc.Layout              = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

static b8 descriptors_create (ID3D12Device* device, R_D3D12Descriptors& out, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type                       = type;
    desc.NumDescriptors             = capacity;
    if (FAILED(device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap), (void**)&out.heap)))
    {
        return false;
    }
    out.start     = out.heap->GetCPUDescriptorHandleForHeapStart().ptr;
    out.increment = device->GetDescriptorHandleIncrementSize(type);
    out.capacity  = capacity;
    return true;
}

static u32 descriptor_allocate (R_D3D12Descriptors& descriptors)
{
    if (descriptors.free_slots.length > 0)
    {
        const u32 slot = descriptors.free_slots.last();
        descriptors.free_slots.pop();
        return slot;
    }
    if (descriptors.next == descriptors.capacity)
    {
        LOG_ERROR("Out of D3D12 descriptors (%u)", descriptors.capacity);
        return R_D3D12_NO_DESCRIPTOR;
    }
    return descriptors.next++;
}

static void descriptor_free (R_D3D12Descriptors& descriptors, u32& slot)
{
    if (slot != R_D3D12_NO_DESCRIPTOR)
    {
        descriptors.free_slots.add(slot);
        slot = R_D3D12_NO_DESCRIPTOR;
    }
}

static D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle (const R_D3D12Descriptors& descriptors, u32 slot)
{
    return { descriptors.start + (usize)slot * descriptors.increment };
}

static b8 cmd_recording (const R_D3D12Device* device, R_CmdBufferHandle cmd)
{
    const R_D3D12CmdBuffer* buffer = device->cmds.try_get(cmd);
    return buffer && device->recording[buffer->slot];
}

static ID3D12GraphicsCommandList* cmd_list (const R_D3D12Device* device, R_CmdBufferHandle cmd)
{
    const R_D3D12CmdBuffer* buffer = device->cmds.try_get(cmd);
    return buffer && device->recording[buffer->slot] ? device->slots[buffer->slot].list : nullptr;
}

static R_D3D12Slot* cmd_slot (R_D3D12Device* device, R_CmdBufferHandle cmd)
{
    const R_D3D12CmdBuffer* buffer = device->cmds.try_get(cmd);
    return buffer && device->recording[buffer->slot] ? &device->slots[buffer->slot] : nullptr;
}

static void transition (ID3D12GraphicsCommandList* list, R_D3D12Image& image, D3D12_RESOURCE_STATES state)
{
    if (image.state == state)
    {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = image.resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = image.state;
    barrier.Transition.StateAfter  = state;
    list->ResourceBarrier(1, &barrier);
    image.state = state;
}

static u32 create_target_view (R_D3D12Device* device, const R_D3D12Image& image, u32 layer)
{
    const u32 slot = descriptor_allocate(device->targets);
    if (slot == R_D3D12_NO_DESCRIPTOR)
    {
        return slot;
    }
    D3D12_RENDER_TARGET_VIEW_DESC desc  = {};
    desc.Format                         = dxgi(r_dxgi_format(image.format).typed);
    desc.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.FirstArraySlice = layer;
    desc.Texture2DArray.ArraySize       = 1;
    device->device->CreateRenderTargetView(image.resource, &desc, descriptor_handle(device->targets, slot));
    return slot;
}

static u32 create_depth_view (R_D3D12Device* device, const R_D3D12Image& image, u32 layer)
{
    const u32 slot = descriptor_allocate(device->depths);
    if (slot == R_D3D12_NO_DESCRIPTOR)
    {
        return slot;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC desc  = {};
    desc.Format                         = dxgi(r_dxgi_format(image.format).depth);
    desc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.FirstArraySlice = layer;
    desc.Texture2DArray.ArraySize       = 1;
    device->device->CreateDepthStencilView(image.resource, &desc, descriptor_handle(device->depths, slot));
    return slot;
}

static b8 create_shader_views (R_D3D12Device* device, R_D3D12Image& image)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format                          = dxgi(r_dxgi_format(image.format).shader);
    desc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (image.layers == 1)
    {
        image.view = descriptor_allocate(device->views);
        if (image.view == R_D3D12_NO_DESCRIPTOR)
        {
            return false;
        }
        desc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels = image.mip_levels;
        device->device->CreateShaderResourceView(image.resource, &desc, descriptor_handle(device->views, image.view));
    }
    image.array_view = descriptor_allocate(device->views);
    if (image.array_view == R_D3D12_NO_DESCRIPTOR)
    {
        return false;
    }
    desc.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray           = {};
    desc.Texture2DArray.MipLevels = image.mip_levels;
    desc.Texture2DArray.ArraySize = image.layers;
    device->device->CreateShaderResourceView(image.resource, &desc, descriptor_handle(device->views, image.array_view));
    return true;
}

static void release_image (R_D3D12Device* device, R_D3D12Image& image)
{
    descriptor_free(device->views, image.view);
    descriptor_free(device->views, image.array_view);
    descriptor_free(device->targets, image.target);
    release(image.resource);
}

static u32 layer0_target (R_D3D12Device* device, R_D3D12Image& image)
{
    if (image.target == R_D3D12_NO_DESCRIPTOR)
    {
        image.target = create_target_view(device, image, 0);
    }
    return image.target;
}

static R_D3D12Image new_image (u32 width, u32 height, u32 layers, u32 mips, R_Format format)
{
    return { .view = R_D3D12_NO_DESCRIPTOR, .array_view = R_D3D12_NO_DESCRIPTOR, .target = R_D3D12_NO_DESCRIPTOR, .width = width, .height = height, .layers = layers, .mip_levels = mips, .format = format };
}

// Waits for a fence value; a null event makes SetEventOnCompletion block
static void wait_fence (const R_D3D12Device* device, u64 value)
{
    if (value > 0 && device->fence->GetCompletedValue() < value)
    {
        device->fence->SetEventOnCompletion(value, nullptr);
    }
}

static void resolve_readbacks (R_D3D12Device* device, R_D3D12Slot& slot)
{
    for (u32 i = 0; i < slot.readbacks_count; i++)
    {
        R_D3D12Readback&     readback = slot.readbacks[i];
        const R_D3D12Buffer* buffer   = device->buffers.try_get(readback.buffer);
        void*                mapped   = nullptr;
        const D3D12_RANGE    range    = { 0, (SIZE_T)readback.resource->GetDesc().Width };
        if (buffer && buffer->mapped && SUCCEEDED(readback.resource->Map(0, &range, &mapped)))
        {
            const usize row = (usize)readback.footprint.Footprint.Width * readback.bytes_per_pixel;
            for (u32 y = 0; y < readback.footprint.Footprint.Height; y++)
            {
                memory_copy(buffer->mapped + row * y, (const u8*)mapped + (usize)readback.footprint.Footprint.RowPitch * y, row);
            }
            const D3D12_RANGE written = {};
            readback.resource->Unmap(0, &written);
        }
        release(readback.resource);
    }
    slot.readbacks_count = 0;
}

// The slot's last submission is done: what it kept alive goes
static void retire_slot (R_D3D12Device* device, R_D3D12Slot& slot)
{
    wait_fence(device, slot.fence_value);
    resolve_readbacks(device, slot);
    for (u32 i = 0; i < slot.garbage_count; i++)
    {
        release(slot.garbage[i]);
    }
    slot.garbage_count = 0;
}

static void keep_until_done (R_D3D12Slot& slot, ID3D12Resource* resource)
{
    if (slot.garbage_count == R_D3D12_MAX_GARBAGE)
    {
        LOG_ERROR("Too many transient D3D12 resources in one command buffer");
        resource->Release();
        return;
    }
    slot.garbage[slot.garbage_count++] = resource;
}

static ID3D12PipelineState* create_state (const R_D3D12Device* device, const R_ShaderCode& vertex, const R_ShaderCode* fragment, DXGI_FORMAT color, DXGI_FORMAT depth, const D3D12_BLEND_DESC& blend, const D3D12_DEPTH_STENCIL_DESC& depth_state, const D3D12_INPUT_ELEMENT_DESC* elements, u32 elements_count)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature                     = device->root_signature;
    desc.VS                                 = { vertex.dxbc, vertex.dxbc_size };
    if (fragment)
    {
        desc.PS = { fragment->dxbc, fragment->dxbc_size };
    }
    desc.BlendState                         = blend;
    desc.SampleMask                         = 0xffffffffu;
    desc.RasterizerState.FillMode           = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode           = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable    = TRUE;
    desc.DepthStencilState                  = depth_state;
    desc.InputLayout                        = { elements, elements_count };
    desc.PrimitiveTopologyType              = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets                   = color != DXGI_FORMAT_UNKNOWN ? 1 : 0;
    desc.RTVFormats[0]                      = color;
    desc.DSVFormat                          = depth;
    desc.SampleDesc.Count                   = 1;
    ID3D12PipelineState* state              = nullptr;
    const HRESULT        result             = device->device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), (void**)&state);
    if (FAILED(result))
    {
        LOG_ERROR("Cannot create a D3D12 pipeline state (0x%08lx)", (unsigned long)result);
        return nullptr;
    }
    return state;
}

static D3D12_BLEND_DESC opaque_blend (b8 write_color)
{
    D3D12_BLEND_DESC blend                      = {};
    blend.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend             = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].LogicOp               = D3D12_LOGIC_OP_NOOP;
    blend.RenderTarget[0].RenderTargetWriteMask = write_color ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
    return blend;
}

static ID3D12PipelineState* blit_state (R_D3D12Device* device, DXGI_FORMAT format)
{
    for (const R_D3D12BlitState& blit : device->blits)
    {
        if (blit.state && blit.format == format)
        {
            return blit.state;
        }
    }
    for (R_D3D12BlitState& blit : device->blits)
    {
        if (!blit.state)
        {
            D3D12_DEPTH_STENCIL_DESC depth = {};
            depth.DepthFunc                = D3D12_COMPARISON_FUNC_ALWAYS;
            blit.format                    = format;
            blit.state                     = create_state(device, blit_vert_shader, &blit_frag_shader, format, DXGI_FORMAT_UNKNOWN, opaque_blend(true), depth, nullptr, 0);
            return blit.state;
        }
    }
    LOG_ERROR("Blits into more than %u formats", R_D3D12_MAX_BLIT_FORMATS);
    return nullptr;
}

// Copies the views of a binding into the slot's range of the shader visible heap
static b8 bind_views (R_D3D12Device* device, R_D3D12Slot& slot, ID3D12GraphicsCommandList* list, const u32* views, u32 count, R_Filter filter)
{
    if (slot.descriptors_used + R_MAX_TEXTURES > R_D3D12_SLOT_DESCRIPTORS)
    {
        LOG_ERROR("Out of shader visible D3D12 descriptors in one command buffer");
        return false;
    }
    const u32 slot_index = (u32)(&slot - device->slots);
    const u32 first      = slot_index * R_D3D12_SLOT_DESCRIPTORS + slot.descriptors_used;
    const u32 increment  = device->views.increment;
    for (u32 i = 0; i < R_MAX_TEXTURES; i++)
    {
        const u32                         source = i < count ? views[i] : device->null_view;
        const D3D12_CPU_DESCRIPTOR_HANDLE target = { device->shader_views->GetCPUDescriptorHandleForHeapStart().ptr + (usize)(first + i) * increment };
        device->device->CopyDescriptorsSimple(1, target, descriptor_handle(device->views, source), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    slot.descriptors_used += R_MAX_TEXTURES;
    const D3D12_GPU_DESCRIPTOR_HANDLE table    = { device->shader_views->GetGPUDescriptorHandleForHeapStart().ptr + (u64)first * increment };
    const u32                         samplers = device->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    const D3D12_GPU_DESCRIPTOR_HANDLE sampler  = { device->shader_samplers->GetGPUDescriptorHandleForHeapStart().ptr + (u64)(filter == R_Filter::Linear ? 2 : 0) * samplers };
    list->SetGraphicsRootDescriptorTable(1, table);
    list->SetGraphicsRootDescriptorTable(2, sampler);
    return true;
}

static void apply_pipeline_state (R_D3D12Device* device, ID3D12GraphicsCommandList* list, const R_D3D12Pipeline& pipeline)
{
    list->SetPipelineState(pipeline.state);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    device->vertex_dirty = true;
}

static b8 prepare_draw (R_D3D12Device* device, R_CmdBufferHandle cmd)
{
    ID3D12GraphicsCommandList* list     = cmd_list(device, cmd);
    const R_D3D12Pipeline*     pipeline = device->pipelines.try_get(device->bound_pipeline);
    if (!list || !pipeline || !device->framebuffers.is_valid(device->framebuffer))
    {
        ASSERT_MSG(false, "draw without a pipeline or outside a render pass");
        return false;
    }
    if (pipeline->vertex_stride == 0 || !device->vertex_dirty)
    {
        return true;
    }
    const R_D3D12Buffer* buffer = device->buffers.try_get(device->vertex_buffer);
    if (!buffer || !buffer->resource || device->vertex_offset >= buffer->size)
    {
        ASSERT_MSG(false, "draw without a vertex buffer");
        return false;
    }
    const D3D12_VERTEX_BUFFER_VIEW view = { buffer->resource->GetGPUVirtualAddress() + device->vertex_offset, (UINT)(buffer->size - device->vertex_offset), pipeline->vertex_stride };
    list->IASetVertexBuffers(0, 1, &view);
    device->vertex_dirty = false;
    return true;
}

static void destroy_window_resource (R_D3D12Device* device, R_D3D12WindowSwapchain& window)
{
    for (R_ImageHandle& handle : window.images)
    {
        R_D3D12Image* image = device->images.try_get(handle);
        if (image)
        {
            release_image(device, *image);
            device->images.remove(handle);
        }
        handle = NULL_HANDLE;
    }
    release(window.swapchain);
}

static void submit (R_D3D12Device* device, R_D3D12Slot& slot)
{
    ID3D12CommandList* lists[] = { slot.list }; // NOLINT(misc-const-correctness): D3D takes T* const*
    device->queue->ExecuteCommandLists(1, lists);
    device->queue->Signal(device->fence, ++device->fence_value);
    slot.fence_value = device->fence_value;
    device->queue_accesses++;
}

static b8 create_root_signature (R_D3D12Device* device)
{
    OS_Library*                         library   = os_library_open("d3d12.dll");
    const R_D3D12SerializeRootSignature serialize = (R_D3D12SerializeRootSignature)os_library_symbol(library, "D3D12SerializeRootSignature");
    if (!serialize)
    {
        LOG_ERROR("d3d12.dll has no D3D12SerializeRootSignature");
        return false;
    }
    const D3D12_DESCRIPTOR_RANGE views         = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, R_MAX_TEXTURES, 0, 0, 0 };
    const D3D12_DESCRIPTOR_RANGE samplers      = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, R_MAX_TEXTURES, 0, 0, 0 };
    D3D12_ROOT_PARAMETER         parameters[3] = {};
    parameters[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = R_MAX_PUSH_CONSTANTS / 4;
    parameters[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable          = { 1, &views };
    parameters[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable          = { 1, &samplers };
    parameters[2].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC desc = { ARRAY_SIZE(parameters), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3DBlob* blob  = nullptr;
    ID3DBlob* error = nullptr;
    if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
    {
        LOG_ERROR("Cannot serialize the root signature: %s", error ? (const char*)error->GetBufferPointer() : "no error text");
        release(error);
        return false;
    }
    release(error);
    const HRESULT result = device->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&device->root_signature);
    blob->Release();
    return SUCCEEDED(result);
}

static b8 create_heaps (R_D3D12Device* device)
{
    ID3D12Device* d = device->device;
    if (!descriptors_create(d, device->views, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, R_D3D12_VIEW_DESCRIPTORS) || !descriptors_create(d, device->targets, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, R_D3D12_TARGET_DESCRIPTORS) ||
        !descriptors_create(d, device->depths, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, R_D3D12_DEPTH_DESCRIPTORS))
    {
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC views = {};
    views.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    views.NumDescriptors             = R_D3D12_SLOTS * R_D3D12_SLOT_DESCRIPTORS;
    views.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC samplers = {};
    samplers.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samplers.NumDescriptors             = 4;
    samplers.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(d->CreateDescriptorHeap(&views, __uuidof(ID3D12DescriptorHeap), (void**)&device->shader_views)) || FAILED(d->CreateDescriptorHeap(&samplers, __uuidof(ID3D12DescriptorHeap), (void**)&device->shader_samplers)))
    {
        return false;
    }
    const u32 increment = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for (u32 i = 0; i < 4; i++)
    {
        D3D12_SAMPLER_DESC sampler = {};
        sampler.Filter             = i < 2 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU           = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV           = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW           = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc     = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MaxLOD             = D3D12_FLOAT32_MAX;
        d->CreateSampler(&sampler, { device->shader_samplers->GetCPUDescriptorHandleForHeapStart().ptr + (usize)i * increment });
    }
    // Bound in the unused places of a texture table
    device->null_view                    = descriptor_allocate(device->views);
    D3D12_SHADER_RESOURCE_VIEW_DESC none = {};
    none.Format                          = DXGI_FORMAT_R8G8B8A8_UNORM;
    none.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
    none.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    none.Texture2D.MipLevels             = 1;
    d->CreateShaderResourceView(nullptr, &none, descriptor_handle(device->views, device->null_view));
    return true;
}

static b8 create_slots (R_D3D12Device* device)
{
    for (R_D3D12Slot& slot : device->slots)
    {
        if (FAILED(device->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&slot.allocator)) ||
            FAILED(device->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&slot.list)))
        {
            return false;
        }
        slot.list->Close();
    }
    return SUCCEEDED(device->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&device->fence));
}

//
// Device
//

static void d3d12_wait_idle (const R_Device* base_device)
{
    const R_D3D12Device* device = d3d12_device(base_device);
    if (device->fence)
    {
        wait_fence(device, device->fence_value);
    }
}

static void d3d12_destroy_device (R_Device* base_device)
{
    R_D3D12Device* device = d3d12_device(base_device);
    if (!device)
    {
        return;
    }
    d3d12_wait_idle(base_device);
    for (R_D3D12Slot& slot : device->slots)
    {
        if (device->fence)
        {
            retire_slot(device, slot);
        }
        release(slot.list);
        release(slot.allocator);
    }
    for (R_D3D12WindowSwapchain& window : device->windows)
    {
        destroy_window_resource(device, window);
    }
    for (R_D3D12Image& image : device->images)
    {
        release_image(device, image);
    }
    for (R_D3D12Buffer& buffer : device->buffers)
    {
        if (buffer.resource)
        {
            release(buffer.resource);
        }
        else
        {
            os_memory_release(buffer.mapped);
        }
    }
    for (R_D3D12Pipeline& pipeline : device->pipelines)
    {
        release(pipeline.state);
    }
    for (R_D3D12BlitState& blit : device->blits)
    {
        release(blit.state);
    }
    release(device->views.heap);
    release(device->targets.heap);
    release(device->depths.heap);
    device->views.free_slots.release();
    device->targets.free_slots.release();
    device->depths.free_slots.release();
    release(device->shader_views);
    release(device->shader_samplers);
    release(device->root_signature);
    release(device->fence);
    release(device->queue);
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

// Images are created in their initial state
static void d3d12_flush_initial_layouts (R_Device* base_device) // NOLINT(misc-const-correctness): the backend table fixes the signature
{
    (void)base_device;
}

static u64 d3d12_queue_access_count (const R_Device* base_device)
{
    return d3d12_device(base_device)->queue_accesses;
}

static i64 d3d12_native_format (const R_Device* base_device, R_Format format)
{
    (void)base_device;
    return r_dxgi_format(format).resource != 0 ? (i64)r_dxgi_format(format).typed : 0;
}

static R_Format d3d12_format_from_native (const R_Device* base_device, i64 format)
{
    (void)base_device;
    return r_dxgi_format_from_native(format);
}

static u64 d3d12_adapter_luid (const R_Device* base_device)
{
    const LUID luid = d3d12_device(base_device)->device->GetAdapterLuid();
    return (u64)luid.LowPart | (u64)(u32)luid.HighPart << 32;
}

//
// Images
//

static R_ImageHandle d3d12_create_image (R_Device* base_device, const R_CreateImageInfo& info)
{
    R_D3D12Device*      device = d3d12_device(base_device);
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
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension           = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width               = info.width;
    desc.Height              = info.height;
    desc.DepthOrArraySize    = (UINT16)layers;
    desc.MipLevels           = (UINT16)mips;
    desc.Format              = dxgi(format.resource);
    desc.SampleDesc.Count    = 1;
    // Color images are render targets for clears and blits too, which draw
    desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (depth && !(info.usage_flags & (u32)R_ImageUsageFlags::DepthStencilAttachment))
    {
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    }
    if (!depth && (info.usage_flags & (u32)R_ImageUsageFlags::UnorderedAccess))
    {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    R_D3D12Image                image      = new_image(info.width, info.height, layers, mips, info.format);
    const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    image.state                            = layout_state(info.initial_layout);
    if (FAILED(device->device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, image.state, nullptr, __uuidof(ID3D12Resource), (void**)&image.resource)) || !create_shader_views(device, image))
    {
        LOG_ERROR("Cannot create a %ux%u texture of %u layers, format %u", info.width, info.height, layers, (u32)info.format);
        release_image(device, image);
        return NULL_HANDLE;
    }
    return device->images.add(image);
}

static b8 d3d12_image_flags_supported (const R_Device* base_device, const R_CreateImageInfo& info)
{
    (void)base_device;
    return (info.vk_extra_create_flags | info.vk_extra_usage_flags) == 0;
}

static R_ImageHandle d3d12_import_image (R_Device* base_device, const R_ImportImageInfo& info)
{
    R_D3D12Device* device = d3d12_device(base_device);
    if (!info.native_image || info.width == 0 || info.height == 0 || r_dxgi_format(info.format).resource == 0)
    {
        LOG_ERROR("Invalid image import");
        return NULL_HANDLE;
    }
    R_D3D12Image image = new_image(info.width, info.height, 1, 1, info.format);
    image.resource     = (ID3D12Resource*)(uintptr_t)info.native_image;
    image.imported     = true;
    image.resource->AddRef();
    return device->images.add(image);
}

static void d3d12_destroy_image (R_Device* base_device, R_ImageHandle handle)
{
    R_D3D12Device* device = d3d12_device(base_device);
    R_D3D12Image*  image  = device->images.try_get(handle);
    if (!image)
    {
        return;
    }
    // Resources die with their last reference, which a submitted command list holds while it runs
    release_image(device, *image);
    device->images.remove(handle);
}

static b8 d3d12_is_valid_image (const R_Device* base_device, R_ImageHandle image)
{
    return d3d12_device(base_device)->images.is_valid(image);
}

static u64 d3d12_get_image_native (const R_Device* base_device, R_ImageHandle handle)
{
    const R_D3D12Image* image = d3d12_device(base_device)->images.try_get(handle);
    return image ? (u64)(uintptr_t)image->resource : 0;
}

static void d3d12_get_image_size (const R_Device* base_device, R_ImageHandle handle, u32& out_width, u32& out_height, u32& out_layers)
{
    const R_D3D12Image* image = d3d12_device(base_device)->images.try_get(handle);
    out_width  = image ? image->width : 0;
    out_height = image ? image->height : 0;
    out_layers = image ? image->layers : 0;
}

static R_Format d3d12_get_image_format (const R_Device* base_device, R_ImageHandle handle)
{
    const R_D3D12Image* image = d3d12_device(base_device)->images.try_get(handle);
    return image ? image->format : R_Format::Unknown;
}

//
// Buffers
//

static R_BufferHandle d3d12_create_buffer (R_Device* base_device, const R_CreateBufferInfo& info)
{
    R_D3D12Device* device = d3d12_device(base_device);
    if (info.size == 0 || info.usage_flags == 0)
    {
        LOG_ERROR("Invalid buffer of %llu bytes", (unsigned long long)info.size);
        return NULL_HANDLE;
    }
    const u32     device_usage = (u32)R_BufferUsageFlags::Vertex | (u32)R_BufferUsageFlags::Index | (u32)R_BufferUsageFlags::Uniform;
    R_D3D12Buffer buffer       = { .size = info.size };
    if (info.usage_flags & device_usage)
    {
        // Host visible buffers sit in the upload heap, which the GPU reads directly
        const D3D12_HEAP_PROPERTIES properties = heap_properties(info.host_visible ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT);
        const D3D12_RESOURCE_DESC   desc       = buffer_desc(info.size);
        const D3D12_RESOURCE_STATES state      = info.host_visible ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
        const D3D12_RANGE           none       = {};
        if (FAILED(device->device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, __uuidof(ID3D12Resource), (void**)&buffer.resource)) ||
            (info.host_visible && FAILED(buffer.resource->Map(0, &none, (void**)&buffer.mapped))))
        {
            LOG_ERROR("Cannot create a buffer of %llu bytes", (unsigned long long)info.size);
            release(buffer.resource);
            return NULL_HANDLE;
        }
        if (buffer.mapped)
        {
            memory_zero(buffer.mapped, (usize)info.size);
        }
    }
    else if (info.host_visible)
    {
        buffer.mapped = (u8*)os_memory_alloc((usize)info.size);
        memory_zero(buffer.mapped, (usize)info.size);
    }
    return device->buffers.add(buffer);
}

static void d3d12_destroy_buffer (R_Device* base_device, R_BufferHandle handle)
{
    R_D3D12Device* device = d3d12_device(base_device);
    R_D3D12Buffer* buffer = device->buffers.try_get(handle);
    if (!buffer)
    {
        return;
    }
    if (buffer->resource)
    {
        release(buffer->resource);
    }
    else
    {
        os_memory_release(buffer->mapped);
    }
    device->buffers.remove(handle);
}

static b8 d3d12_is_valid_buffer (const R_Device* base_device, R_BufferHandle buffer)
{
    return d3d12_device(base_device)->buffers.is_valid(buffer);
}

static void* d3d12_get_buffer_mapped_data (const R_Device* base_device, R_BufferHandle handle)
{
    const R_D3D12Buffer* buffer = d3d12_device(base_device)->buffers.try_get(handle);
    return buffer ? buffer->mapped : nullptr;
}

// Upload heaps are coherent
static void d3d12_flush_buffer (R_Device* base_device, R_BufferHandle handle, u64 offset, u64 size)
{
    const R_D3D12Buffer* buffer = d3d12_device(base_device)->buffers.try_get(handle);
    if (!buffer || !buffer->mapped || offset > buffer->size || size > buffer->size - offset)
    {
        ASSERT_MSG(false, "invalid buffer flush");
    }
}

//
// Render passes and framebuffers
//

static R_RenderPassHandle d3d12_create_render_pass (R_Device* base_device, const R_CreateRenderPassInfo& info)
{
    R_D3D12Device* device    = d3d12_device(base_device);
    const b8       has_color = info.color_format != R_Format::Unknown;
    const b8       has_depth = info.depth_format != R_Format::Unknown;
    if ((!has_color && !has_depth) || (has_color && (r_dxgi_format(info.color_format).resource == 0 || r_format_is_depth(info.color_format))) || (has_depth && (!r_format_is_depth(info.depth_format) || r_dxgi_format(info.depth_format).depth == 0)))
    {
        LOG_ERROR("Invalid render pass formats: color %u, depth %u", (u32)info.color_format, (u32)info.depth_format);
        return NULL_HANDLE;
    }
    return device->render_passes.add({
        .format               = info.color_format,
        .load_op              = info.load_op,
        .initial_layout       = info.initial_layout,
        .final_layout         = info.final_layout,
        .depth_format         = info.depth_format,
        .depth_load_op        = info.depth_load_op,
        .depth_initial_layout = info.depth_initial_layout,
        .depth_final_layout   = info.depth_final_layout,
    });
}

static void d3d12_destroy_render_pass (R_Device* base_device, R_RenderPassHandle handle)
{
    d3d12_device(base_device)->render_passes.remove(handle);
}

static b8 d3d12_is_valid_render_pass (const R_Device* base_device, R_RenderPassHandle render_pass)
{
    return d3d12_device(base_device)->render_passes.is_valid(render_pass);
}

static R_FramebufferHandle d3d12_create_framebuffer (R_Device* base_device, const R_CreateFramebufferInfo& info)
{
    R_D3D12Device*           device = d3d12_device(base_device);
    const R_D3D12RenderPass* pass   = device->render_passes.try_get(info.render_pass);
    const R_D3D12Image*      color  = device->images.try_get(info.image);
    const R_D3D12Image*      depth  = device->images.try_get(info.depth_image);
    const R_D3D12Image*      sized  = color ? color : depth;
    const b8                 wants_color = pass && pass->format != R_Format::Unknown;
    const b8                 wants_depth = pass && pass->depth_format != R_Format::Unknown;
    if (!pass || !sized || wants_color != (color != nullptr) || wants_depth != (depth != nullptr) || (color && (info.layer >= color->layers || color->format != pass->format)) ||
        (depth && (info.depth_layer >= depth->layers || depth->format != pass->depth_format)) || (color && depth && (color->width != depth->width || color->height != depth->height)))
    {
        LOG_ERROR("Invalid framebuffer");
        return NULL_HANDLE;
    }
    R_D3D12Framebuffer framebuffer = { .target = R_D3D12_NO_DESCRIPTOR, .depth = R_D3D12_NO_DESCRIPTOR, .image = info.image, .depth_image = info.depth_image, .width = sized->width, .height = sized->height, .render_pass = info.render_pass };
    framebuffer.target             = color ? create_target_view(device, *color, info.layer) : R_D3D12_NO_DESCRIPTOR;
    framebuffer.depth              = depth ? create_depth_view(device, *depth, info.depth_layer) : R_D3D12_NO_DESCRIPTOR;
    if ((color && framebuffer.target == R_D3D12_NO_DESCRIPTOR) || (depth && framebuffer.depth == R_D3D12_NO_DESCRIPTOR))
    {
        descriptor_free(device->targets, framebuffer.target);
        descriptor_free(device->depths, framebuffer.depth);
        return NULL_HANDLE;
    }
    return device->framebuffers.add(framebuffer);
}

static void d3d12_destroy_framebuffer (R_Device* base_device, R_FramebufferHandle handle)
{
    R_D3D12Device*      device      = d3d12_device(base_device);
    R_D3D12Framebuffer* framebuffer = device->framebuffers.try_get(handle);
    if (!framebuffer)
    {
        return;
    }
    descriptor_free(device->targets, framebuffer->target);
    descriptor_free(device->depths, framebuffer->depth);
    device->framebuffers.remove(handle);
}

static b8 d3d12_is_valid_framebuffer (const R_Device* base_device, R_FramebufferHandle framebuffer)
{
    return d3d12_device(base_device)->framebuffers.is_valid(framebuffer);
}

//
// Pipelines
//

static R_PipelineHandle d3d12_create_pipeline (R_Device* base_device, const R_CreatePipelineInfo& info)
{
    R_D3D12Device*           device = d3d12_device(base_device);
    const R_D3D12RenderPass* pass   = device->render_passes.try_get(info.render_pass);
    if (!pass || info.push_constants_size > R_MAX_PUSH_CONSTANTS || info.push_constants_size % 4 != 0 || info.vertex_attributes_count > R_MAX_VERTEX_ATTRIBUTES || info.textures_count > R_MAX_TEXTURES ||
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
    D3D12_INPUT_ELEMENT_DESC elements[R_MAX_VERTEX_ATTRIBUTES] = {};
    for (u32 i = 0; i < info.vertex_attributes_count; i++)
    {
        const R_VertexAttribute& attribute = info.vertex_attributes[i];
        if ((u32)attribute.format >= (u32)R_Format::Count || r_dxgi_format(attribute.format).typed == 0 || r_format_is_depth(attribute.format) || attribute.location >= 32)
        {
            LOG_ERROR("Invalid vertex attribute %u", i);
            return NULL_HANDLE;
        }
        elements[i] = { "ATTRIBUTE", attribute.location, dxgi(r_dxgi_format(attribute.format).typed), 0, attribute.offset, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    }
    R_BlendFactor factors[4];
    r_pipeline_blend_factors(info, factors);
    D3D12_BLEND_DESC blend = opaque_blend(pass->format != R_Format::Unknown && !info.no_color_write);
    if (info.blend_enable)
    {
        blend.RenderTarget[0].BlendEnable    = TRUE;
        blend.RenderTarget[0].SrcBlend       = blend_factor(factors[0]);
        blend.RenderTarget[0].DestBlend      = blend_factor(factors[1]);
        blend.RenderTarget[0].SrcBlendAlpha  = blend_factor(factors[2]);
        blend.RenderTarget[0].DestBlendAlpha = blend_factor(factors[3]);
    }
    D3D12_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable              = info.depth_test || info.depth_write;
    depth.DepthWriteMask           = info.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc                = info.depth_test ? compare_func(info.depth_compare) : D3D12_COMPARISON_FUNC_ALWAYS;
    const DXGI_FORMAT     color_format = pass->format != R_Format::Unknown ? dxgi(r_dxgi_format(pass->format).typed) : DXGI_FORMAT_UNKNOWN;
    const DXGI_FORMAT     depth_format = pass->depth_format != R_Format::Unknown ? dxgi(r_dxgi_format(pass->depth_format).depth) : DXGI_FORMAT_UNKNOWN;
    const R_D3D12Pipeline pipeline     = {
        .state               = create_state(device, *info.vertex, info.fragment, color_format, depth_format, blend, depth, elements, info.vertex_attributes_count),
        .textures_count      = info.textures_count,
        .texture_type        = info.texture_type,
        .push_constants_size = info.push_constants_size,
        .vertex_stride       = info.vertex_stride,
    };
    if (!pipeline.state)
    {
        return NULL_HANDLE;
    }
    return device->pipelines.add(pipeline);
}

static void d3d12_destroy_pipeline (R_Device* base_device, R_PipelineHandle handle)
{
    R_D3D12Device*   device   = d3d12_device(base_device);
    R_D3D12Pipeline* pipeline = device->pipelines.try_get(handle);
    if (!pipeline)
    {
        return;
    }
    release(pipeline->state);
    device->pipelines.remove(handle);
}

static b8 d3d12_is_valid_pipeline (const R_Device* base_device, R_PipelineHandle pipeline)
{
    return d3d12_device(base_device)->pipelines.is_valid(pipeline);
}

//
// Window presentation
//

static b8 d3d12_device_supports_present (const R_Device* base_device, const R_WindowTarget& target)
{
    (void)base_device;
    return target.native_window != 0 && !target.wayland;
}

// A flip model swapchain on the app's queue; it never waits for the display, so it never paces the app
static R_WindowSwapchainHandle d3d12_create_window_swapchain (R_Device* base_device, const R_WindowTarget& target, u32 width, u32 height)
{
    R_D3D12Device* device = d3d12_device(base_device);
    if (!d3d12_device_supports_present(base_device, target) || width == 0 || height == 0)
    {
        LOG_WARNING("Cannot present to this window");
        return NULL_HANDLE;
    }
    typedef HRESULT (WINAPI* R_CreateDXGIFactory1) (REFIID riid, void** factory);
    const R_CreateDXGIFactory1 create  = (R_CreateDXGIFactory1)os_library_symbol(os_library_open("dxgi.dll"), "CreateDXGIFactory1");
    IDXGIFactory2*             factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory2), (void**)&factory)))
    {
        LOG_WARNING("No DXGI 1.2 factory; the mirror window stays blank");
        return NULL_HANDLE;
    }
    const HWND            hwnd = (HWND)(uintptr_t)target.native_window;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width                 = width;
    desc.Height                = height;
    desc.Format                = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count      = 1;
    desc.BufferUsage           = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount           = R_D3D12_WINDOW_BUFFERS;
    desc.SwapEffect            = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* swapchain = nullptr;
    HRESULT          result    = factory->CreateSwapChainForHwnd(device->queue, hwnd, &desc, nullptr, nullptr, &swapchain);
    if (SUCCEEDED(result))
    {
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }
    factory->Release();
    R_D3D12WindowSwapchain window = { .width = width, .height = height };
    if (SUCCEEDED(result))
    {
        result = swapchain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&window.swapchain);
        swapchain->Release();
    }
    for (u32 i = 0; i < R_D3D12_WINDOW_BUFFERS && SUCCEEDED(result); i++)
    {
        ID3D12Resource* buffer = nullptr;
        result                 = window.swapchain->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&buffer);
        if (SUCCEEDED(result))
        {
            // Flip model buffers take an sRGB view of their linear format
            R_D3D12Image image = new_image(width, height, 1, 1, R_Format::B8G8R8A8_SRGB);
            image.resource     = buffer;
            image.state        = D3D12_RESOURCE_STATE_PRESENT;
            image.imported     = true;
            window.images[i]   = device->images.add(image);
        }
    }
    if (FAILED(result))
    {
        LOG_WARNING("Cannot make a swapchain of the mirror window (0x%08lx)", (unsigned long)result);
        destroy_window_resource(device, window);
        return NULL_HANDLE;
    }
    return device->windows.add(window);
}

static void d3d12_destroy_window_swapchain (R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    R_D3D12Device*          device = d3d12_device(base_device);
    R_D3D12WindowSwapchain* window = device->windows.try_get(swapchain);
    if (!window)
    {
        return;
    }
    // The buffers may not die while a command list of ours still uses them
    d3d12_wait_idle(base_device);
    destroy_window_resource(device, *window);
    device->windows.remove(swapchain);
}

static b8 d3d12_is_valid_window_swapchain (const R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    return d3d12_device(base_device)->windows.is_valid(swapchain);
}

static R_Format d3d12_window_swapchain_format (const R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    return d3d12_device(base_device)->windows.is_valid(swapchain) ? R_Format::B8G8R8A8_SRGB : R_Format::Unknown;
}

static void d3d12_window_swapchain_size (const R_Device* base_device, R_WindowSwapchainHandle swapchain, u32& out_width, u32& out_height)
{
    const R_D3D12WindowSwapchain* window = d3d12_device(base_device)->windows.try_get(swapchain);
    out_width  = window ? window->width : 0;
    out_height = window ? window->height : 0;
}

static b8 d3d12_window_acquire (R_Device* base_device, R_WindowSwapchainHandle handle, R_ImageHandle& out_image)
{
    const R_D3D12WindowSwapchain* window = d3d12_device(base_device)->windows.try_get(handle);
    out_image                            = window ? window->images[window->swapchain->GetCurrentBackBufferIndex() % R_D3D12_WINDOW_BUFFERS] : R_ImageHandle{};
    return window != nullptr;
}

static b8 d3d12_window_present (R_Device* base_device, R_WindowSwapchainHandle handle)
{
    R_D3D12Device*                device = d3d12_device(base_device);
    const R_D3D12WindowSwapchain* window = device->windows.try_get(handle);
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

static R_CmdBufferHandle begin_recording (R_D3D12Device* device, u32 index)
{
    if (device->recording[index])
    {
        ASSERT_MSG(false, "command buffer already recording");
        return NULL_HANDLE;
    }
    R_D3D12Slot& slot = device->slots[index];
    retire_slot(device, slot);
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator, nullptr)))
    {
        LOG_ERROR("Cannot reset a D3D12 command list");
        return NULL_HANDLE;
    }
    ID3D12DescriptorHeap* heaps[] = { device->shader_views, device->shader_samplers }; // NOLINT(misc-const-correctness): D3D takes T* const*
    slot.list->SetDescriptorHeaps(ARRAY_SIZE(heaps), heaps);
    slot.list->SetGraphicsRootSignature(device->root_signature);
    slot.descriptors_used    = 0;
    device->recording[index] = true;
    device->bound_pipeline   = NULL_HANDLE;
    device->vertex_buffer    = NULL_HANDLE;
    device->framebuffer      = NULL_HANDLE;
    return device->cmd_handles[index];
}

static R_D3D12Slot* end_recording (R_D3D12Device* device, R_CmdBufferHandle cmd)
{
    const R_D3D12CmdBuffer* buffer = device->cmds.try_get(cmd);
    if (!buffer || !device->recording[buffer->slot])
    {
        ASSERT_MSG(false, "command buffer not recording");
        return nullptr;
    }
    device->recording[buffer->slot] = false;
    R_D3D12Slot& slot               = device->slots[buffer->slot];
    if (FAILED(slot.list->Close()))
    {
        LOG_ERROR("Cannot close the D3D12 command list");
        return nullptr;
    }
    submit(device, slot);
    return &slot;
}

static R_CmdBufferHandle d3d12_begin_frame (R_Device* base_device)
{
    R_D3D12Device* device = d3d12_device(base_device);
    const u32      index  = device->frame_index;
    device->frame_index   = (device->frame_index + 1) % R_MAX_FRAMES_IN_FLIGHT;
    return begin_recording(device, index);
}

static void d3d12_end_frame (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_D3D12Device* device = d3d12_device(base_device);
    R_D3D12Slot*   slot   = end_recording(device, cmd);
    if (slot && slot->readbacks_count > 0)
    {
        retire_slot(device, *slot);
    }
}

static void d3d12_end_frame_present (R_Device* base_device, R_CmdBufferHandle cmd, R_WindowSwapchainHandle swapchain)
{
    (void)swapchain;
    d3d12_end_frame(base_device, cmd);
}

static R_CmdBufferHandle d3d12_begin_immediate (R_Device* base_device)
{
    return begin_recording(d3d12_device(base_device), R_MAX_FRAMES_IN_FLIGHT);
}

static void d3d12_end_immediate (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_D3D12Device* device = d3d12_device(base_device);
    R_D3D12Slot*   slot   = end_recording(device, cmd);
    if (slot)
    {
        retire_slot(device, *slot);
    }
}

static u64 d3d12_get_cmd_native (const R_Device* base_device, R_CmdBufferHandle cmd)
{
    const R_D3D12Device*    device = d3d12_device(base_device);
    const R_D3D12CmdBuffer* buffer = device->cmds.try_get(cmd);
    return buffer ? (u64)(uintptr_t)device->slots[buffer->slot].list : 0;
}

//
// Commands
//

static void d3d12_cmd_transition_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle handle, R_ImageLayout old_layout, R_ImageLayout new_layout)
{
    R_D3D12Device*             device = d3d12_device(base_device);
    ID3D12GraphicsCommandList* list   = cmd_list(device, cmd);
    R_D3D12Image*              image  = device->images.try_get(handle);
    (void)old_layout;
    if (!list || !image)
    {
        ASSERT_MSG(false, "invalid transition");
        return;
    }
    transition(list, *image, layout_state(new_layout));
}

static void d3d12_cmd_begin_render_pass (R_Device* base_device, R_CmdBufferHandle cmd, R_RenderPassHandle pass_handle, R_FramebufferHandle fb_handle, const R_ClearColor& clear)
{
    R_D3D12Device*            device = d3d12_device(base_device);
    ID3D12GraphicsCommandList* list  = cmd_list(device, cmd);
    const R_D3D12RenderPass*  pass   = device->render_passes.try_get(pass_handle);
    const R_D3D12Framebuffer* fb     = device->framebuffers.try_get(fb_handle);
    R_D3D12Image*             color  = fb ? device->images.try_get(fb->image) : nullptr;
    R_D3D12Image*             depth  = fb ? device->images.try_get(fb->depth_image) : nullptr;
    if (!list || !pass || !fb || (fb->target != R_D3D12_NO_DESCRIPTOR && !color) || (fb->depth != R_D3D12_NO_DESCRIPTOR && !depth))
    {
        ASSERT_MSG(false, "invalid render pass or framebuffer");
        return;
    }
    device->framebuffer                     = fb_handle;
    const D3D12_CPU_DESCRIPTOR_HANDLE target = descriptor_handle(device->targets, fb->target);
    const D3D12_CPU_DESCRIPTOR_HANDLE stencil = descriptor_handle(device->depths, fb->depth);
    if (color)
    {
        transition(list, *color, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (depth)
    {
        transition(list, *depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    list->OMSetRenderTargets(color ? 1 : 0, color ? &target : nullptr, FALSE, depth ? &stencil : nullptr);
    const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (FLOAT)fb->width, (FLOAT)fb->height, 0.0f, 1.0f };
    const D3D12_RECT     scissor  = { 0, 0, (LONG)fb->width, (LONG)fb->height };
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    if (color && pass->load_op == R_LoadOp::Clear)
    {
        const FLOAT value[4] = { clear.r, clear.g, clear.b, clear.a };
        list->ClearRenderTargetView(target, value, 0, nullptr);
    }
    if (depth && pass->depth_load_op == R_LoadOp::Clear)
    {
        list->ClearDepthStencilView(stencil, R_D3D12_CLEAR_DEPTH_STENCIL, 1.0f, 0, 0, nullptr);
    }
}

static void d3d12_cmd_end_render_pass (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_D3D12Device*             device = d3d12_device(base_device);
    ID3D12GraphicsCommandList* list   = cmd_list(device, cmd);
    const R_D3D12Framebuffer*  fb     = device->framebuffers.try_get(device->framebuffer);
    const R_D3D12RenderPass*   pass   = fb ? device->render_passes.try_get(fb->render_pass) : nullptr;
    if (!list || !fb || !pass)
    {
        ASSERT_MSG(false, "render pass end without a render pass");
        return;
    }
    R_D3D12Image* color = device->images.try_get(fb->image);
    R_D3D12Image* depth = device->images.try_get(fb->depth_image);
    if (color && pass->final_layout != R_ImageLayout::Undefined)
    {
        transition(list, *color, layout_state(pass->final_layout));
    }
    if (depth && pass->depth_final_layout != R_ImageLayout::Undefined)
    {
        transition(list, *depth, layout_state(pass->depth_final_layout));
    }
    device->framebuffer = NULL_HANDLE;
}

static void d3d12_cmd_bind_pipeline (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle handle)
{
    R_D3D12Device*             device   = d3d12_device(base_device);
    ID3D12GraphicsCommandList* list     = cmd_list(device, cmd);
    const R_D3D12Pipeline*     pipeline = device->pipelines.try_get(handle);
    if (!list || !pipeline)
    {
        ASSERT_MSG(false, "invalid pipeline binding");
        return;
    }
    device->bound_pipeline = handle;
    apply_pipeline_state(device, list, *pipeline);
}

static void d3d12_cmd_bind_textures (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle pipeline_handle, const R_ImageHandle* images, u32 count, R_Filter filter)
{
    R_D3D12Device*         device   = d3d12_device(base_device);
    R_D3D12Slot*           slot     = cmd_slot(device, cmd);
    const R_D3D12Pipeline* pipeline = device->pipelines.try_get(pipeline_handle);
    if (!slot || !pipeline || !images || count == 0 || count != pipeline->textures_count)
    {
        ASSERT_MSG(false, "invalid texture binding");
        return;
    }
    u32 views[R_MAX_TEXTURES] = {};
    for (u32 i = 0; i < count; i++)
    {
        const R_D3D12Image* image = device->images.try_get(images[i]);
        views[i]                  = image ? (pipeline->texture_type == R_TextureType::Type2DArray ? image->array_view : image->view) : R_D3D12_NO_DESCRIPTOR;
        if (views[i] == R_D3D12_NO_DESCRIPTOR)
        {
            ASSERT_MSG(false, "invalid texture binding");
            return;
        }
    }
    bind_views(device, *slot, slot->list, views, count, filter);
}

static void d3d12_cmd_clear_depth (R_Device* base_device, R_CmdBufferHandle cmd, const R_Rect& rect, f32 depth)
{
    R_D3D12Device*             device = d3d12_device(base_device);
    ID3D12GraphicsCommandList* list   = cmd_list(device, cmd);
    const R_D3D12Framebuffer*  fb     = device->framebuffers.try_get(device->framebuffer);
    if (!list || !fb || fb->depth == R_D3D12_NO_DESCRIPTOR)
    {
        ASSERT_MSG(false, "depth clear outside a render pass with depth");
        return;
    }
    const D3D12_RECT area = { rect.x, rect.y, rect.x + (LONG)rect.width, rect.y + (LONG)rect.height };
    list->ClearDepthStencilView(descriptor_handle(device->depths, fb->depth), R_D3D12_CLEAR_DEPTH_STENCIL, depth, 0, 1, &area);
}

static void d3d12_cmd_push_constants (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle handle, const void* data, u32 size)
{
    R_D3D12Device*             device   = d3d12_device(base_device);
    ID3D12GraphicsCommandList* list     = cmd_list(device, cmd);
    const R_D3D12Pipeline*     pipeline = device->pipelines.try_get(handle);
    if (!list || !pipeline || !data || size == 0 || size > pipeline->push_constants_size || size % 4 != 0)
    {
        ASSERT_MSG(false, "invalid push constants");
        return;
    }
    list->SetGraphicsRoot32BitConstants(0, size / 4, data, 0);
}

static void d3d12_cmd_set_viewport (R_Device* base_device, R_CmdBufferHandle cmd, const R_Viewport& viewport)
{
    ID3D12GraphicsCommandList* list = cmd_list(d3d12_device(base_device), cmd);
    if (!list)
    {
        ASSERT_MSG(false, "invalid command buffer");
        return;
    }
    const D3D12_VIEWPORT value = { viewport.x, viewport.y, viewport.width, viewport.height, 0.0f, 1.0f };
    list->RSSetViewports(1, &value);
}

static void d3d12_cmd_set_scissor (R_Device* base_device, R_CmdBufferHandle cmd, const R_Rect& scissor)
{
    ID3D12GraphicsCommandList* list = cmd_list(d3d12_device(base_device), cmd);
    if (!list)
    {
        ASSERT_MSG(false, "invalid command buffer");
        return;
    }
    const D3D12_RECT value = { scissor.x, scissor.y, scissor.x + (LONG)scissor.width, scissor.y + (LONG)scissor.height };
    list->RSSetScissorRects(1, &value);
}

static void d3d12_cmd_bind_vertex_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset)
{
    R_D3D12Device* device = d3d12_device(base_device);
    if (!cmd_recording(device, cmd) || !device->buffers.is_valid(handle))
    {
        ASSERT_MSG(false, "invalid vertex buffer binding");
        return;
    }
    device->vertex_buffer = handle;
    device->vertex_offset = offset;
    device->vertex_dirty  = true;
}

static void d3d12_cmd_bind_index_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset, b8 u16_indices)
{
    R_D3D12Device*             device = d3d12_device(base_device);
    ID3D12GraphicsCommandList* list   = cmd_list(device, cmd);
    const R_D3D12Buffer*       buffer = device->buffers.try_get(handle);
    if (!list || !buffer || !buffer->resource || offset >= buffer->size)
    {
        ASSERT_MSG(false, "invalid index buffer binding");
        return;
    }
    const D3D12_INDEX_BUFFER_VIEW view = { buffer->resource->GetGPUVirtualAddress() + offset, (UINT)(buffer->size - offset), u16_indices ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
    list->IASetIndexBuffer(&view);
}

static void d3d12_cmd_draw (R_Device* base_device, R_CmdBufferHandle cmd, u32 vertex_count, u32 first_vertex)
{
    R_D3D12Device* device = d3d12_device(base_device);
    if (prepare_draw(device, cmd))
    {
        cmd_list(device, cmd)->DrawInstanced(vertex_count, 1, first_vertex, 0);
    }
}

static void d3d12_cmd_draw_indexed (R_Device* base_device, R_CmdBufferHandle cmd, u32 index_count, u32 first_index, i32 vertex_offset)
{
    R_D3D12Device* device = d3d12_device(base_device);
    if (prepare_draw(device, cmd))
    {
        cmd_list(device, cmd)->DrawIndexedInstanced(index_count, 1, first_index, vertex_offset, 0);
    }
}

static D3D12_TEXTURE_COPY_LOCATION subresource_location (const R_D3D12Image& image, u32 layer)
{
    D3D12_TEXTURE_COPY_LOCATION location = {};
    location.pResource                   = image.resource;
    location.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location.SubresourceIndex            = layer * image.mip_levels;
    return location;
}

// Copies go through a transient buffer with rows as D3D12 wants them aligned
static ID3D12Resource* create_transient (const R_D3D12Device* device, const R_D3D12Image& image, u32 layer, D3D12_HEAP_TYPE type, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& out_footprint)
{
    const D3D12_RESOURCE_DESC desc  = image.resource->GetDesc();
    u64                       total = 0;
    device->device->GetCopyableFootprints(&desc, layer * image.mip_levels, 1, 0, &out_footprint, nullptr, nullptr, &total);
    const D3D12_HEAP_PROPERTIES properties = heap_properties(type);
    const D3D12_RESOURCE_DESC   buffer     = buffer_desc(total);
    const D3D12_RESOURCE_STATES state      = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource*             resource   = nullptr;
    if (FAILED(device->device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer, state, nullptr, __uuidof(ID3D12Resource), (void**)&resource)))
    {
        LOG_ERROR("Cannot create a transient buffer of %llu bytes", (unsigned long long)total);
        return nullptr;
    }
    return resource;
}

static void d3d12_cmd_copy_buffer_to_image (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle buffer_handle, R_ImageHandle image_handle, u32 layer)
{
    R_D3D12Device*       device = d3d12_device(base_device);
    R_D3D12Slot*         slot   = cmd_slot(device, cmd);
    const R_D3D12Buffer* buffer = device->buffers.try_get(buffer_handle);
    R_D3D12Image*        image  = device->images.try_get(image_handle);
    const u32            bytes  = image ? r_format_bytes_per_pixel(image->format) : 0;
    if (!slot || !buffer || !buffer->mapped || !image || layer >= image->layers || r_format_is_depth(image->format) || buffer->size < (u64)image->width * image->height * bytes)
    {
        ASSERT_MSG(false, "invalid buffer to image copy");
        return;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    ID3D12Resource*                    upload    = create_transient(device, *image, layer, D3D12_HEAP_TYPE_UPLOAD, footprint);
    u8*                                mapped    = nullptr;
    const D3D12_RANGE                  none      = {};
    if (!upload || FAILED(upload->Map(0, &none, (void**)&mapped)))
    {
        release(upload);
        return;
    }
    const usize row = (usize)image->width * bytes;
    for (u32 y = 0; y < image->height; y++)
    {
        memory_copy(mapped + footprint.Offset + (usize)footprint.Footprint.RowPitch * y, buffer->mapped + row * y, row);
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource                   = upload;
    source.Type                        = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint             = footprint;
    const D3D12_TEXTURE_COPY_LOCATION target = subresource_location(*image, layer);
    transition(slot->list, *image, D3D12_RESOURCE_STATE_COPY_DEST);
    slot->list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    keep_until_done(*slot, upload);
}

static void d3d12_cmd_copy_image_to_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle image_handle, u32 layer, R_BufferHandle buffer_handle)
{
    R_D3D12Device*       device = d3d12_device(base_device);
    R_D3D12Slot*         slot   = cmd_slot(device, cmd);
    const R_D3D12Buffer* buffer = device->buffers.try_get(buffer_handle);
    R_D3D12Image*        image  = device->images.try_get(image_handle);
    const u32            bytes  = image ? r_format_bytes_per_pixel(image->format) : 0;
    if (!slot || !buffer || !buffer->mapped || !image || layer >= image->layers || buffer->size < (u64)image->width * image->height * bytes || slot->readbacks_count == R_D3D12_MAX_READBACKS)
    {
        ASSERT_MSG(false, "invalid image to buffer copy");
        return;
    }
    R_D3D12Readback readback = { .buffer = buffer_handle, .bytes_per_pixel = bytes };
    readback.resource        = create_transient(device, *image, layer, D3D12_HEAP_TYPE_READBACK, readback.footprint);
    if (!readback.resource)
    {
        return;
    }
    D3D12_TEXTURE_COPY_LOCATION target = {};
    target.pResource                   = readback.resource;
    target.Type                        = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint             = readback.footprint;
    const D3D12_TEXTURE_COPY_LOCATION source = subresource_location(*image, layer);
    transition(slot->list, *image, D3D12_RESOURCE_STATE_COPY_SOURCE);
    slot->list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    slot->readbacks[slot->readbacks_count++] = readback;
}

static void d3d12_cmd_clear_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle handle, const R_ClearColor& color)
{
    R_D3D12Device*             device = d3d12_device(base_device);
    ID3D12GraphicsCommandList* list   = cmd_list(device, cmd);
    R_D3D12Image*              image  = device->images.try_get(handle);
    if (!list || !image || r_format_is_depth(image->format))
    {
        ASSERT_MSG(false, "invalid clear");
        return;
    }
    const D3D12_RESOURCE_STATES state    = image->state;
    const FLOAT                 value[4] = { color.r, color.g, color.b, color.a };
    transition(list, *image, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (u32 layer = 0; layer < image->layers; layer++)
    {
        // The descriptor is read when the clear is recorded, so a temporary one is freed right after
        u32 view = layer == 0 ? layer0_target(device, *image) : create_target_view(device, *image, layer);
        if (view != R_D3D12_NO_DESCRIPTOR)
        {
            list->ClearRenderTargetView(descriptor_handle(device->targets, view), value, 0, nullptr);
        }
        if (layer > 0)
        {
            descriptor_free(device->targets, view);
        }
    }
    transition(list, *image, state);
}

// A draw of the source through the blit shaders into the destination rectangle
static void d3d12_cmd_blit_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle src_handle, u32 src_layer, R_ImageHandle dst_handle, const R_Rect& dst_rect, R_Filter filter)
{
    R_D3D12Device*       device = d3d12_device(base_device);
    R_D3D12Slot*         slot   = cmd_slot(device, cmd);
    R_D3D12Image*        src    = device->images.try_get(src_handle);
    R_D3D12Image*        dst    = device->images.try_get(dst_handle);
    const u32            target = dst ? layer0_target(device, *dst) : R_D3D12_NO_DESCRIPTOR;
    ID3D12PipelineState* state  = dst ? blit_state(device, dxgi(r_dxgi_format(dst->format).typed)) : nullptr;
    if (!slot || !src || !dst || target == R_D3D12_NO_DESCRIPTOR || !state || src_layer >= src->layers || r_format_is_depth(src->format) || r_format_is_depth(dst->format))
    {
        ASSERT_MSG(false, "invalid blit");
        return;
    }
    ID3D12GraphicsCommandList*        list         = slot->list;
    const D3D12_RESOURCE_STATES       src_state    = src->state;
    const D3D12_RESOURCE_STATES       dst_state    = dst->state;
    const D3D12_CPU_DESCRIPTOR_HANDLE target_view  = descriptor_handle(device->targets, target);
    const D3D12_VIEWPORT              viewport     = { (FLOAT)dst_rect.x, (FLOAT)dst_rect.y, (FLOAT)dst_rect.width, (FLOAT)dst_rect.height, 0.0f, 1.0f };
    const D3D12_RECT                  scissor      = { dst_rect.x, dst_rect.y, dst_rect.x + (LONG)dst_rect.width, dst_rect.y + (LONG)dst_rect.height };
    const R_BlitPush                  push         = { .src_rect = { 0.0f, 0.0f, 1.0f, 1.0f }, .layer = (i32)src_layer };
    transition(list, *src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, *dst, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->SetPipelineState(state);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(1, &target_view, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRoot32BitConstants(0, sizeof(push) / 4, &push, 0);
    if (bind_views(device, *slot, list, &src->array_view, 1, filter))
    {
        list->DrawInstanced(3, 1, 0, 0);
    }
    transition(list, *src, src_state);
    transition(list, *dst, dst_state);
    device->bound_pipeline = NULL_HANDLE;
}

static b8 d3d12_read_image (R_Device* base_device, R_ImageHandle handle, u32 layer, R_ImageLayout layout, void* out_pixels, usize out_size)
{
    R_D3D12Device* device = d3d12_device(base_device);
    R_D3D12Image*  image  = device->images.try_get(handle);
    (void)layout;
    const u32 bytes = image ? r_format_bytes_per_pixel(image->format) : 0;
    if (!image || !out_pixels || layer >= image->layers || out_size < (usize)image->width * image->height * bytes || device->recording[R_MAX_FRAMES_IN_FLIGHT])
    {
        return false;
    }
    const R_CmdBufferHandle cmd  = begin_recording(device, R_MAX_FRAMES_IN_FLIGHT);
    const R_D3D12Slot*      slot = cmd_slot(device, cmd);
    if (!slot)
    {
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    ID3D12Resource*                    readback  = create_transient(device, *image, layer, D3D12_HEAP_TYPE_READBACK, footprint);
    if (readback)
    {
        const D3D12_RESOURCE_STATES state  = image->state;
        D3D12_TEXTURE_COPY_LOCATION target = {};
        target.pResource                   = readback;
        target.Type                        = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint             = footprint;
        const D3D12_TEXTURE_COPY_LOCATION source = subresource_location(*image, layer);
        transition(slot->list, *image, D3D12_RESOURCE_STATE_COPY_SOURCE);
        slot->list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        transition(slot->list, *image, state);
    }
    d3d12_end_immediate(base_device, cmd);
    // The last row has no pitch padding, so the buffer ends before RowPitch * Height and a range
    // past its end fails on Windows' D3D12 (resolve_readbacks too)
    void*             mapped = nullptr;
    const D3D12_RANGE range  = { 0, readback ? (SIZE_T)readback->GetDesc().Width : 0 };
    const b8          read   = readback && SUCCEEDED(readback->Map(0, &range, &mapped));
    if (read)
    {
        const usize row = (usize)image->width * bytes;
        for (u32 y = 0; y < image->height; y++)
        {
            memory_copy((u8*)out_pixels + row * y, (const u8*)mapped + footprint.Offset + (usize)footprint.Footprint.RowPitch * y, row);
        }
        const D3D12_RANGE written = {};
        readback->Unmap(0, &written);
    }
    release(readback);
    return read;
}

//
// Backend
//

const static R_BackendFns d3d12_fns = {
    .destroy_device            = d3d12_destroy_device,
    .wait_idle                 = d3d12_wait_idle,
    .flush_initial_layouts     = d3d12_flush_initial_layouts,
    .queue_access_count        = d3d12_queue_access_count,
    .native_format             = d3d12_native_format,
    .format_from_native        = d3d12_format_from_native,
    .adapter_luid              = d3d12_adapter_luid,
    .create_image              = d3d12_create_image,
    .image_flags_supported     = d3d12_image_flags_supported,
    .import_image              = d3d12_import_image,
    .destroy_image             = d3d12_destroy_image,
    .is_valid_image            = d3d12_is_valid_image,
    .get_image_native          = d3d12_get_image_native,
    .get_image_size            = d3d12_get_image_size,
    .get_image_format          = d3d12_get_image_format,
    .create_buffer             = d3d12_create_buffer,
    .destroy_buffer            = d3d12_destroy_buffer,
    .is_valid_buffer           = d3d12_is_valid_buffer,
    .get_buffer_mapped_data    = d3d12_get_buffer_mapped_data,
    .flush_buffer              = d3d12_flush_buffer,
    .create_render_pass        = d3d12_create_render_pass,
    .destroy_render_pass       = d3d12_destroy_render_pass,
    .is_valid_render_pass      = d3d12_is_valid_render_pass,
    .create_framebuffer        = d3d12_create_framebuffer,
    .destroy_framebuffer       = d3d12_destroy_framebuffer,
    .is_valid_framebuffer      = d3d12_is_valid_framebuffer,
    .create_pipeline           = d3d12_create_pipeline,
    .destroy_pipeline          = d3d12_destroy_pipeline,
    .is_valid_pipeline         = d3d12_is_valid_pipeline,
    .device_supports_present   = d3d12_device_supports_present,
    .create_window_swapchain   = d3d12_create_window_swapchain,
    .destroy_window_swapchain  = d3d12_destroy_window_swapchain,
    .is_valid_window_swapchain = d3d12_is_valid_window_swapchain,
    .window_swapchain_format   = d3d12_window_swapchain_format,
    .window_swapchain_size     = d3d12_window_swapchain_size,
    .window_acquire            = d3d12_window_acquire,
    .window_present            = d3d12_window_present,
    .begin_frame               = d3d12_begin_frame,
    .end_frame                 = d3d12_end_frame,
    .end_frame_present         = d3d12_end_frame_present,
    .begin_immediate           = d3d12_begin_immediate,
    .end_immediate             = d3d12_end_immediate,
    .get_cmd_native            = d3d12_get_cmd_native,
    .cmd_transition_image      = d3d12_cmd_transition_image,
    .cmd_begin_render_pass     = d3d12_cmd_begin_render_pass,
    .cmd_end_render_pass       = d3d12_cmd_end_render_pass,
    .cmd_bind_pipeline         = d3d12_cmd_bind_pipeline,
    .cmd_bind_textures         = d3d12_cmd_bind_textures,
    .cmd_clear_depth           = d3d12_cmd_clear_depth,
    .cmd_push_constants        = d3d12_cmd_push_constants,
    .cmd_set_viewport          = d3d12_cmd_set_viewport,
    .cmd_set_scissor           = d3d12_cmd_set_scissor,
    .cmd_bind_vertex_buffer    = d3d12_cmd_bind_vertex_buffer,
    .cmd_bind_index_buffer     = d3d12_cmd_bind_index_buffer,
    .cmd_draw                  = d3d12_cmd_draw,
    .cmd_draw_indexed          = d3d12_cmd_draw_indexed,
    .cmd_copy_buffer_to_image  = d3d12_cmd_copy_buffer_to_image,
    .cmd_copy_image_to_buffer  = d3d12_cmd_copy_image_to_buffer,
    .cmd_clear_image           = d3d12_cmd_clear_image,
    .cmd_blit_image            = d3d12_cmd_blit_image,
    .read_image                = d3d12_read_image,
};

R_Device* r_d3d12_create_device (const R_DeviceCreateInfo& create_info)
{
    ID3D12Device*       app_device = (ID3D12Device*)create_info.d3d12.device;
    ID3D12CommandQueue* app_queue  = (ID3D12CommandQueue*)create_info.d3d12.queue;
    if (!app_device || !app_queue)
    {
        LOG_ERROR("Render device needs the app's ID3D12Device and ID3D12CommandQueue");
        return nullptr;
    }
    if (app_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        LOG_ERROR("The D3D12 queue is no direct queue");
        return nullptr;
    }
    if (!blit_vert_shader.dxbc || !blit_frag_shader.dxbc)
    {
        LOG_ERROR("This build has no DXBC shaders");
        return nullptr;
    }
    R_D3D12Device* device = (R_D3D12Device*)os_memory_alloc(sizeof(R_D3D12Device));
    memory_zero(device, sizeof(R_D3D12Device));
    device->base   = { .fns = &d3d12_fns, .backend = R_Backend::D3D12 };
    device->device = app_device;
    device->queue  = app_queue;
    app_device->AddRef();
    app_queue->AddRef();
    if (!create_root_signature(device) || !create_heaps(device) || !create_slots(device))
    {
        LOG_ERROR("Cannot set up the D3D12 device");
        d3d12_destroy_device(&device->base);
        return nullptr;
    }
    for (u32 i = 0; i < ARRAY_SIZE(device->cmd_handles); i++)
    {
        device->cmd_handles[i] = device->cmds.add({ .slot = i });
    }
    return &device->base;
}
