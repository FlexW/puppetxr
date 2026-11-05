// Apple's headers come first, their Handle and Point renamed: MacTypes.h declares both at global
// scope, where puppetxr's base.h has its own
#define Handle MacHandle
#define Point  MacPoint
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#undef Handle
#undef Point
// os/object.h's macros, which name functions of puppetxr's platform layer
#undef os_retain
#undef os_release

#include "render_internal.h"

#include "blocks.h"
#include "blit.vert.h"
#include "blit.frag.h"

//
// Types
//
// Manual reference counting: every object kept in a struct is retained, and every call that
// makes autoreleased objects runs in a pool of its own, since the app's thread may have none.
// Shaders are MSL translated from the Vulkan SPIR-V by spirv-cross (docs/shaders.md, Metal):
// push constants are buffer 0 of both stages, texture and sampler N binding N, the vertex
// buffer goes to R_METAL_VERTEX_BUFFER. State is kept and applied at each draw, so commands may
// come in any order within a render pass.

#define R_METAL_CMDS          (R_MAX_FRAMES_IN_FLIGHT + 1) // the frames, then the immediate buffer
#define R_METAL_MAX_FUNCTIONS 32
#define R_METAL_MAX_VARIANTS  16
#define R_METAL_VERTEX_BUFFER 30
#define R_METAL_MAX_SIZE      16384 // texture width and height every Apple GPU supports

namespace
{

struct R_MetalImage
{
    id<MTLTexture> texture;
    id<MTLTexture> array_view; // a 2D array view of 2D and cube textures, made on first use
    u32            width;
    u32            height;
    u32            layers;
    R_Format       format;
};

struct R_MetalBuffer
{
    id<MTLBuffer> buffer;
    u64           size;
};

struct R_MetalRenderPass
{
    R_Format color_format;
    R_LoadOp load_op;
    R_Format depth_format;
    R_LoadOp depth_load_op;
};

struct R_MetalFramebuffer
{
    R_RenderPassHandle render_pass = {};
    R_ImageHandle      image = {};
    u32                layer = {};
    R_ImageHandle      depth_image = {};
    u32                depth_layer = {};
    u32                width = {};
    u32                height = {};
};

struct R_MetalPipeline
{
    id<MTLRenderPipelineState> state;
    id<MTLDepthStencilState>   depth;
    u32                        textures_count;
    R_TextureType              texture_type;
    u32                        push_constants_size;
};

struct R_MetalWindowSwapchain
{
    CAMetalLayer*       layer = {};
    id<CAMetalDrawable> drawable = {};
    R_ImageHandle       image = {}; // the acquired drawable's texture
    id<MTLCommandBuffer> presenting = {}; // the command buffer that presented last, retained
    u32                 width = {};
    u32                 height = {};
};

struct R_MetalCmdBuffer
{
    u32 slot; // R_MAX_FRAMES_IN_FLIGHT for the immediate buffer
};

// What a draw needs, set by the commands in any order and applied at the draw
struct R_MetalDrawState
{
    R_PipelineHandle pipeline = {};
    R_ImageHandle    textures[R_MAX_TEXTURES] = {};
    u32              textures_count = {};
    R_Filter         filter = {};
    u8               push[R_MAX_PUSH_CONSTANTS] = {};
    u32              push_size = {};
    MTLViewport      viewport = {};
    MTLScissorRect   scissor = {};
    R_BufferHandle   vertex_buffer = {};
    u64              vertex_offset = {};
    R_BufferHandle   index_buffer = {};
    u64              index_offset = {};
    b8               u16_indices = {};
};

struct R_MetalSlot
{
    id<MTLCommandBuffer>        cmd = {};       // recording, then committed until the slot is reused
    id<MTLRenderCommandEncoder> render = {};
    id<MTLBlitCommandEncoder>   blit = {};
    b8                          recording = {};
    R_FramebufferHandle         framebuffer = {};
    R_MetalDrawState            draw = {};
    R_WindowSwapchainHandle     present = {};
};

struct R_MetalFunction
{
    const R_ShaderCode* code;
    id<MTLFunction>     function;
};

// Internal pipelines by the formats of the pass they draw in: blits and depth clears
struct R_MetalVariant
{
    R_Format                   color_format;
    R_Format                   depth_format;
    b8                         depth_clear;
    id<MTLRenderPipelineState> state;
};

struct R_MetalDevice
{
    R_Device                                             base = {};
    id<MTLDevice>                                        device = {};
    id<MTLCommandQueue>                                  queue = {};
    id<MTLSamplerState>                                  samplers[2] = {}; // by R_Filter
    id<MTLDepthStencilState>                             no_depth = {};
    id<MTLDepthStencilState>                             depth_overwrite = {};
    u64                                                  queue_accesses = {};
    u32                                                  frame_index = {};
    R_MetalSlot                                          slots[R_METAL_CMDS] = {};
    R_CmdBufferHandle                                    cmd_handles[R_METAL_CMDS] = {};
    R_MetalFunction                                      functions[R_METAL_MAX_FUNCTIONS] = {};
    u32                                                  functions_count = {};
    R_MetalVariant                                       variants[R_METAL_MAX_VARIANTS] = {};
    u32                                                  variants_count = {};

    SparseArray<R_MetalImage, R_Image>                   images = {};
    SparseArray<R_MetalBuffer, R_Buffer>                 buffers = {};
    SparseArray<R_MetalRenderPass, R_RenderPass>         render_passes = {};
    SparseArray<R_MetalFramebuffer, R_Framebuffer>       framebuffers = {};
    SparseArray<R_MetalPipeline, R_Pipeline>             pipelines = {};
    SparseArray<R_MetalCmdBuffer, R_CmdBuffer>           cmds = {};
    SparseArray<R_MetalWindowSwapchain, R_WindowSwapchain> windows = {};
};

} // namespace

//
// Statics
//

// Indexed by R_Format; 0 where Metal on Apple GPUs has none
const static MTLPixelFormat metal_formats[] = {
    MTLPixelFormatInvalid,
    MTLPixelFormatRGBA8Unorm,
    MTLPixelFormatBGRA8Unorm,
    MTLPixelFormatRGBA8Unorm_sRGB,
    MTLPixelFormatBGRA8Unorm_sRGB,
    MTLPixelFormatRGBA16Float,
    MTLPixelFormatRG32Float,
    MTLPixelFormatInvalid, // R32G32B32_SFLOAT, vertex attributes only
    MTLPixelFormatDepth16Unorm,
    MTLPixelFormatInvalid, // D24_UNORM_S8_UINT, which Apple GPUs lack
    MTLPixelFormatDepth32Float,
    MTLPixelFormatDepth32Float_Stencil8,
};
static_assert(ARRAY_SIZE(metal_formats) == (usize)R_Format::Count, "a format is missing");

static R_MetalDevice* metal_device (R_Device* device)
{
    return (R_MetalDevice*)device;
}

static const R_MetalDevice* metal_device (const R_Device* device)
{
    return (const R_MetalDevice*)device;
}

template <typename T>
static void metal_release (T& object)
{
    if (object)
    {
        [object release];
        object = nil;
    }
}

static MTLPixelFormat pixel_format (R_Format format)
{
    return (u32)format < ARRAY_SIZE(metal_formats) ? metal_formats[(u32)format] : MTLPixelFormatInvalid;
}

static R_Format format_from_pixel (MTLPixelFormat pixel)
{
    for (u32 i = 1; i < ARRAY_SIZE(metal_formats); i++)
    {
        if (metal_formats[i] == pixel && pixel != MTLPixelFormatInvalid)
        {
            return (R_Format)i;
        }
    }
    return R_Format::Unknown;
}

static MTLCompareFunction compare_function (R_CompareOp op)
{
    switch (op)
    {
    case R_CompareOp::LessOrEqual:
        return MTLCompareFunctionLessEqual;
    case R_CompareOp::Always:
        return MTLCompareFunctionAlways;
    case R_CompareOp::Never:
        return MTLCompareFunctionNever;
    case R_CompareOp::Less:
        return MTLCompareFunctionLess;
    case R_CompareOp::Equal:
        return MTLCompareFunctionEqual;
    case R_CompareOp::Greater:
        return MTLCompareFunctionGreater;
    case R_CompareOp::NotEqual:
        return MTLCompareFunctionNotEqual;
    case R_CompareOp::GreaterOrEqual:
        return MTLCompareFunctionGreaterEqual;
    }
    return MTLCompareFunctionAlways;
}

static MTLBlendFactor blend_factor (R_BlendFactor factor)
{
    switch (factor)
    {
    case R_BlendFactor::Zero:
        return MTLBlendFactorZero;
    case R_BlendFactor::One:
        return MTLBlendFactorOne;
    case R_BlendFactor::SrcAlpha:
        return MTLBlendFactorSourceAlpha;
    case R_BlendFactor::OneMinusSrcAlpha:
        return MTLBlendFactorOneMinusSourceAlpha;
    case R_BlendFactor::DstAlpha:
        return MTLBlendFactorDestinationAlpha;
    case R_BlendFactor::OneMinusDstAlpha:
        return MTLBlendFactorOneMinusDestinationAlpha;
    }
    return MTLBlendFactorOne;
}

static MTLVertexFormat vertex_format (R_Format format)
{
    switch (format)
    {
    case R_Format::R32G32_SFLOAT:
        return MTLVertexFormatFloat2;
    case R_Format::R32G32B32_SFLOAT:
        return MTLVertexFormatFloat3;
    case R_Format::R8G8B8A8_UNORM:
        return MTLVertexFormatUChar4Normalized;
    case R_Format::R16G16B16A16_SFLOAT:
        return MTLVertexFormatHalf4;
    default:
        return MTLVertexFormatInvalid;
    }
}

static R_MetalSlot* cmd_slot (R_MetalDevice* device, R_CmdBufferHandle cmd)
{
    const R_MetalCmdBuffer* buffer = device->cmds.try_get(cmd);
    R_MetalSlot*            slot   = buffer ? &device->slots[buffer->slot] : nullptr;
    return slot && slot->recording ? slot : nullptr;
}

static void end_encoders (R_MetalSlot& slot)
{
    if (slot.render)
    {
        [slot.render endEncoding];
        metal_release(slot.render);
    }
    if (slot.blit)
    {
        [slot.blit endEncoding];
        metal_release(slot.blit);
    }
}

static id<MTLBlitCommandEncoder> blit_encoder (R_MetalSlot& slot)
{
    if (!slot.blit)
    {
        end_encoders(slot);
        @autoreleasepool
        {
            slot.blit = [[slot.cmd blitCommandEncoder] retain];
        }
    }
    return slot.blit;
}

static void wait_slot (R_MetalSlot& slot)
{
    if (slot.cmd && !slot.recording)
    {
        [slot.cmd waitUntilCompleted];
        metal_release(slot.cmd);
    }
}

// Compiled when a pipeline first needs it; the source is fixed, so each shader compiles once
static id<MTLFunction> shader_function (R_MetalDevice* device, const R_ShaderCode* code)
{
    if (!code || !code->msl)
    {
        return nil;
    }
    for (u32 i = 0; i < device->functions_count; i++)
    {
        if (device->functions[i].code == code)
        {
            return device->functions[i].function;
        }
    }
    if (device->functions_count == R_METAL_MAX_FUNCTIONS)
    {
        LOG_ERROR("More than %u Metal shaders", R_METAL_MAX_FUNCTIONS);
        return nil;
    }
    @autoreleasepool
    {
        // Determinism (docs/determinism.md): no fast math, so results do not depend on the compiler's whims
        MTLCompileOptions* const options = [[MTLCompileOptions new] autorelease];
        options.fastMathEnabled    = NO;
        options.languageVersion    = MTLLanguageVersion2_3;
        NSString* const      source  = [NSString stringWithUTF8String:code->msl];
        NSError*             error   = nil;
        const id<MTLLibrary> library = source ? [device->device newLibraryWithSource:source options:options error:&error] : nil;
        if (!library)
        {
            LOG_ERROR("Cannot compile a Metal shader: %s", error ? error.localizedDescription.UTF8String : "unknown error");
            return nil;
        }
        const id<MTLFunction> function = [library newFunctionWithName:@"main0"];
        [library release];
        if (!function)
        {
            LOG_ERROR("A Metal shader has no main0");
            return nil;
        }
        device->functions[device->functions_count++] = { .code = code, .function = function };
        return function;
    }
}

static id<MTLTexture> sampled_texture (R_MetalImage& image, b8 array)
{
    const MTLTextureType type = image.texture.textureType;
    if (!array || type == MTLTextureType2DArray)
    {
        return image.texture;
    }
    if (!image.array_view)
    {
        image.array_view = [image.texture newTextureViewWithPixelFormat:image.texture.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, image.texture.mipmapLevelCount) slices:NSMakeRange(0, image.layers)];
    }
    return image.array_view;
}

static MTLLoadAction load_action (R_LoadOp op)
{
    switch (op)
    {
    case R_LoadOp::Load:
        return MTLLoadActionLoad;
    case R_LoadOp::Clear:
        return MTLLoadActionClear;
    case R_LoadOp::DontCare:
        return MTLLoadActionDontCare;
    }
    return MTLLoadActionLoad;
}

// The blit and depth clear pipelines for a pass of these formats, made once
static id<MTLRenderPipelineState> variant_state (R_MetalDevice* device, R_Format color_format, R_Format depth_format, b8 depth_clear)
{
    for (u32 i = 0; i < device->variants_count; i++)
    {
        const R_MetalVariant& variant = device->variants[i];
        if (variant.color_format == color_format && variant.depth_format == depth_format && variant.depth_clear == depth_clear)
        {
            return variant.state;
        }
    }
    if (device->variants_count == R_METAL_MAX_VARIANTS)
    {
        LOG_ERROR("More than %u internal Metal pipelines", R_METAL_MAX_VARIANTS);
        return nil;
    }
    const id<MTLFunction> vertex   = shader_function(device, &blit_vert_shader);
    const id<MTLFunction> fragment = depth_clear ? nil : shader_function(device, &blit_frag_shader);
    if (!vertex || (!depth_clear && !fragment))
    {
        return nil;
    }
    @autoreleasepool
    {
        MTLRenderPipelineDescriptor* const desc = [[MTLRenderPipelineDescriptor new] autorelease];
        desc.vertexFunction               = vertex;
        desc.fragmentFunction             = fragment;
        desc.colorAttachments[0].pixelFormat = pixel_format(color_format);
        desc.colorAttachments[0].writeMask   = depth_clear ? MTLColorWriteMaskNone : MTLColorWriteMaskAll;
        desc.depthAttachmentPixelFormat      = pixel_format(depth_format);
        desc.stencilAttachmentPixelFormat    = r_format_has_stencil(depth_format) ? pixel_format(depth_format) : MTLPixelFormatInvalid;
        NSError* error = nil;
        const id<MTLRenderPipelineState> state = [device->device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!state)
        {
            LOG_ERROR("Cannot create an internal Metal pipeline: %s", error ? error.localizedDescription.UTF8String : "unknown error");
            return nil;
        }
        device->variants[device->variants_count++] = { .color_format = color_format, .depth_format = depth_format, .depth_clear = depth_clear, .state = state };
        return state;
    }
}

static MTLRenderPassDescriptor* pass_descriptor (R_MetalDevice* device, const R_MetalRenderPass& pass, const R_MetalFramebuffer& fb, const R_ClearColor& clear)
{
    MTLRenderPassDescriptor* const desc  = [MTLRenderPassDescriptor renderPassDescriptor];
    const R_MetalImage*      color = device->images.try_get(fb.image);
    const R_MetalImage*      depth = device->images.try_get(fb.depth_image);
    if (color)
    {
        desc.colorAttachments[0].texture     = color->texture;
        desc.colorAttachments[0].slice       = fb.layer;
        desc.colorAttachments[0].loadAction  = load_action(pass.load_op);
        desc.colorAttachments[0].storeAction = MTLStoreActionStore;
        desc.colorAttachments[0].clearColor  = MTLClearColorMake((f64)clear.r, (f64)clear.g, (f64)clear.b, (f64)clear.a);
    }
    if (depth)
    {
        desc.depthAttachment.texture     = depth->texture;
        desc.depthAttachment.slice       = fb.depth_layer;
        desc.depthAttachment.loadAction  = load_action(pass.depth_load_op);
        desc.depthAttachment.storeAction = MTLStoreActionStore;
        desc.depthAttachment.clearDepth  = 1.0;
        if (r_format_has_stencil(depth->format))
        {
            desc.stencilAttachment.texture     = depth->texture;
            desc.stencilAttachment.slice       = fb.depth_layer;
            desc.stencilAttachment.loadAction  = load_action(pass.depth_load_op);
            desc.stencilAttachment.storeAction = MTLStoreActionStore;
            desc.stencilAttachment.clearStencil = 0;
        }
    }
    return desc;
}

static void destroy_window_resource (R_MetalDevice* device, R_MetalWindowSwapchain& window)
{
    metal_release(window.drawable);
    metal_release(window.layer);
    metal_release(window.presenting);
    if (device->images.is_valid(window.image))
    {
        metal_release(device->images.get(window.image).texture);
        device->images.remove(window.image);
    }
    window.image = {};
}

//
// Device
//

static void metal_wait_idle (const R_Device* base_device)
{
    for (const R_MetalSlot& slot : metal_device(base_device)->slots)
    {
        if (slot.cmd && !slot.recording)
        {
            [slot.cmd waitUntilCompleted];
        }
    }
}

static void metal_destroy_device (R_Device* base_device)
{
    R_MetalDevice* device = metal_device(base_device);
    if (!device)
    {
        return;
    }
    for (R_MetalSlot& slot : device->slots)
    {
        end_encoders(slot);
        if (slot.recording)
        {
            [slot.cmd commit];
            slot.recording = false;
        }
        wait_slot(slot);
    }
    for (R_MetalWindowSwapchain& window : device->windows)
    {
        destroy_window_resource(device, window);
    }
    for (R_MetalImage& image : device->images)
    {
        metal_release(image.texture);
        metal_release(image.array_view);
    }
    for (R_MetalBuffer& buffer : device->buffers)
    {
        metal_release(buffer.buffer);
    }
    for (R_MetalPipeline& pipeline : device->pipelines)
    {
        metal_release(pipeline.state);
        metal_release(pipeline.depth);
    }
    for (u32 i = 0; i < device->functions_count; i++)
    {
        metal_release(device->functions[i].function);
    }
    for (u32 i = 0; i < device->variants_count; i++)
    {
        metal_release(device->variants[i].state);
    }
    metal_release(device->samplers[0]);
    metal_release(device->samplers[1]);
    metal_release(device->no_depth);
    metal_release(device->depth_overwrite);
    metal_release(device->queue);
    metal_release(device->device);
    device->images.release();
    device->buffers.release();
    device->render_passes.release();
    device->framebuffers.release();
    device->pipelines.release();
    device->cmds.release();
    device->windows.release();
    os_memory_release(device);
}

static void metal_flush_initial_layouts (R_Device* base_device) // NOLINT(misc-const-correctness): the backend table fixes the signature
{
    (void)base_device;
}

static u64 metal_queue_access_count (const R_Device* base_device)
{
    return metal_device(base_device)->queue_accesses;
}

static i64 metal_native_format (const R_Device* base_device, R_Format format)
{
    (void)base_device;
    return (i64)pixel_format(format);
}

static R_Format metal_format_from_native (const R_Device* base_device, i64 format)
{
    (void)base_device;
    return format > 0 ? format_from_pixel((MTLPixelFormat)format) : R_Format::Unknown;
}

//
// Images
//

static R_ImageHandle metal_create_image (R_Device* base_device, const R_CreateImageInfo& info)
{
    R_MetalDevice*       device = metal_device(base_device);
    const MTLPixelFormat format = pixel_format(info.format);
    const u32            layers = info.layers ? info.layers : 1;
    const u32            levels = info.mip_levels ? info.mip_levels : 1;
    // Metal's validation aborts the process on a descriptor it refuses, so everything is checked here
    if (format == MTLPixelFormatInvalid || info.width == 0 || info.height == 0 || info.width > R_METAL_MAX_SIZE || info.height > R_METAL_MAX_SIZE || layers > R_MAX_IMAGE_LAYERS ||
        levels > r_max_mip_levels(info.width, info.height) || (info.cube_compatible && (layers % 6 != 0 || info.width != info.height)))
    {
        LOG_ERROR("Invalid Metal image %ux%u, %u layers, format %u", info.width, info.height, layers, (u32)info.format);
        return NULL_HANDLE;
    }
    @autoreleasepool
    {
        // Apps see one-layer images as 2D textures and cube images as cube textures
        MTLTextureDescriptor* const desc = [[MTLTextureDescriptor new] autorelease];
        desc.pixelFormat           = format;
        desc.width                 = info.width;
        desc.height                = info.height;
        desc.mipmapLevelCount      = levels;
        desc.storageMode           = MTLStorageModePrivate;
        desc.usage                 = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
        if (info.usage_flags & (u32)R_ImageUsageFlags::UnorderedAccess)
        {
            desc.usage |= MTLTextureUsageShaderWrite;
        }
        if (info.cube_compatible)
        {
            desc.textureType = layers == 6 ? MTLTextureTypeCube : MTLTextureTypeCubeArray;
            desc.arrayLength = layers / 6;
        }
        else
        {
            desc.textureType = layers == 1 ? MTLTextureType2D : MTLTextureType2DArray;
            desc.arrayLength = layers;
        }
        const id<MTLTexture> texture = [device->device newTextureWithDescriptor:desc];
        if (!texture)
        {
            LOG_ERROR("Cannot create a Metal texture of %ux%u", info.width, info.height);
            return NULL_HANDLE;
        }
        return device->images.add({ .texture = texture, .array_view = nil, .width = info.width, .height = info.height, .layers = layers, .format = info.format });
    }
}

static b8 metal_image_flags_supported (const R_Device* base_device, const R_CreateImageInfo& info)
{
    (void)base_device;
    return info.vk_extra_create_flags == 0 && info.vk_extra_usage_flags == 0;
}

static R_ImageHandle metal_import_image (R_Device* base_device, const R_ImportImageInfo& info)
{
    R_MetalDevice*       device  = metal_device(base_device);
    const id<MTLTexture> texture = (id<MTLTexture>)(uintptr_t)info.native_image;
    if (!texture)
    {
        return NULL_HANDLE;
    }
    const MTLTextureType type   = texture.textureType;
    const u32            layers = (u32)texture.arrayLength * (type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray ? 6u : 1u);
    const R_Format       format = info.format != R_Format::Unknown ? info.format : format_from_pixel(texture.pixelFormat);
    return device->images.add({ .texture = [texture retain], .array_view = nil, .width = info.width ? info.width : (u32)texture.width, .height = info.height ? info.height : (u32)texture.height, .layers = layers, .format = format });
}

static void metal_destroy_image (R_Device* base_device, R_ImageHandle handle)
{
    R_MetalDevice* device = metal_device(base_device);
    R_MetalImage*  image  = device->images.try_get(handle);
    if (!image)
    {
        return;
    }
    metal_release(image->texture);
    metal_release(image->array_view);
    device->images.remove(handle);
}

static b8 metal_is_valid_image (const R_Device* base_device, R_ImageHandle image)
{
    return metal_device(base_device)->images.is_valid(image);
}

static u64 metal_get_image_native (const R_Device* base_device, R_ImageHandle handle)
{
    const R_MetalImage* image = metal_device(base_device)->images.try_get(handle);
    return image ? (u64)(uintptr_t)image->texture : 0;
}

static void metal_get_image_size (const R_Device* base_device, R_ImageHandle handle, u32& out_width, u32& out_height, u32& out_layers)
{
    const R_MetalImage* image = metal_device(base_device)->images.try_get(handle);
    out_width                 = image ? image->width : 0;
    out_height                = image ? image->height : 0;
    out_layers                = image ? image->layers : 0;
}

static R_Format metal_get_image_format (const R_Device* base_device, R_ImageHandle handle)
{
    const R_MetalImage* image = metal_device(base_device)->images.try_get(handle);
    return image ? image->format : R_Format::Unknown;
}

//
// Buffers
//

// Apple GPUs share memory with the CPU, so host visible buffers are shared and flushes are free
static R_BufferHandle metal_create_buffer (R_Device* base_device, const R_CreateBufferInfo& info)
{
    R_MetalDevice* device = metal_device(base_device);
    if (info.size == 0)
    {
        LOG_ERROR("Metal buffers need a size");
        return NULL_HANDLE;
    }
    const id<MTLBuffer> buffer = [device->device newBufferWithLength:info.size options:info.host_visible ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate];
    if (!buffer)
    {
        LOG_ERROR("Cannot create a Metal buffer of %llu bytes", (unsigned long long)info.size);
        return NULL_HANDLE;
    }
    return device->buffers.add({ .buffer = buffer, .size = info.size });
}

static void metal_destroy_buffer (R_Device* base_device, R_BufferHandle handle)
{
    R_MetalDevice* device = metal_device(base_device);
    R_MetalBuffer* buffer = device->buffers.try_get(handle);
    if (!buffer)
    {
        return;
    }
    metal_release(buffer->buffer);
    device->buffers.remove(handle);
}

static b8 metal_is_valid_buffer (const R_Device* base_device, R_BufferHandle buffer)
{
    return metal_device(base_device)->buffers.is_valid(buffer);
}

static void* metal_get_buffer_mapped_data (const R_Device* base_device, R_BufferHandle handle)
{
    const R_MetalBuffer* buffer = metal_device(base_device)->buffers.try_get(handle);
    return buffer && buffer->buffer.storageMode == MTLStorageModeShared ? buffer->buffer.contents : nullptr;
}

// Shared buffers are coherent; the others are never mapped
static void metal_flush_buffer (R_Device* base_device, R_BufferHandle handle, u64 offset, u64 size) // NOLINT(misc-const-correctness): the backend table fixes the signature
{
    (void)base_device;
    (void)handle;
    (void)offset;
    (void)size;
}

//
// Render passes and framebuffers
//

static R_RenderPassHandle metal_create_render_pass (R_Device* base_device, const R_CreateRenderPassInfo& info)
{
    R_MetalDevice* device = metal_device(base_device);
    if ((info.color_format == R_Format::Unknown && info.depth_format == R_Format::Unknown) || (info.color_format != R_Format::Unknown && (r_format_is_depth(info.color_format) || pixel_format(info.color_format) == MTLPixelFormatInvalid)) ||
        (info.depth_format != R_Format::Unknown && (!r_format_is_depth(info.depth_format) || pixel_format(info.depth_format) == MTLPixelFormatInvalid)))
    {
        LOG_ERROR("Invalid Metal render pass");
        return NULL_HANDLE;
    }
    return device->render_passes.add({ .color_format = info.color_format, .load_op = info.load_op, .depth_format = info.depth_format, .depth_load_op = info.depth_load_op });
}

static void metal_destroy_render_pass (R_Device* base_device, R_RenderPassHandle handle)
{
    metal_device(base_device)->render_passes.remove(handle);
}

static b8 metal_is_valid_render_pass (const R_Device* base_device, R_RenderPassHandle render_pass)
{
    return metal_device(base_device)->render_passes.is_valid(render_pass);
}

static R_FramebufferHandle metal_create_framebuffer (R_Device* base_device, const R_CreateFramebufferInfo& info)
{
    R_MetalDevice*           device = metal_device(base_device);
    const R_MetalRenderPass* pass   = device->render_passes.try_get(info.render_pass);
    const R_MetalImage*      color  = device->images.try_get(info.image);
    const R_MetalImage*      depth  = device->images.try_get(info.depth_image);
    const b8 color_ok = pass && (pass->color_format == R_Format::Unknown ? !color : color && info.layer < color->layers);
    const b8 depth_ok = pass && (pass->depth_format == R_Format::Unknown ? !depth : depth && info.depth_layer < depth->layers && r_format_is_depth(depth->format));
    if (!color_ok || !depth_ok || (!color && !depth) || (color && depth && (color->width != depth->width || color->height != depth->height)))
    {
        LOG_ERROR("Invalid Metal framebuffer");
        return NULL_HANDLE;
    }
    const R_MetalImage* sized = color ? color : depth;
    return device->framebuffers.add({ .render_pass = info.render_pass, .image = info.image, .layer = info.layer, .depth_image = info.depth_image, .depth_layer = info.depth_layer, .width = sized->width, .height = sized->height });
}

static void metal_destroy_framebuffer (R_Device* base_device, R_FramebufferHandle handle)
{
    metal_device(base_device)->framebuffers.remove(handle);
}

static b8 metal_is_valid_framebuffer (const R_Device* base_device, R_FramebufferHandle framebuffer)
{
    return metal_device(base_device)->framebuffers.is_valid(framebuffer);
}

//
// Pipelines
//

static R_PipelineHandle metal_create_pipeline (R_Device* base_device, const R_CreatePipelineInfo& info)
{
    R_MetalDevice*           device = metal_device(base_device);
    const R_MetalRenderPass* pass   = device->render_passes.try_get(info.render_pass);
    if (!pass || !info.vertex || !info.fragment || info.textures_count > R_MAX_TEXTURES || info.push_constants_size > R_MAX_PUSH_CONSTANTS || info.vertex_attributes_count > R_MAX_VERTEX_ATTRIBUTES ||
        (info.depth_test && pass->depth_format == R_Format::Unknown))
    {
        LOG_ERROR("Invalid Metal pipeline");
        return NULL_HANDLE;
    }
    const id<MTLFunction> vertex   = shader_function(device, info.vertex);
    const id<MTLFunction> fragment = shader_function(device, info.fragment);
    if (!vertex || !fragment)
    {
        LOG_ERROR("The pipeline's shaders have no Metal code");
        return NULL_HANDLE;
    }
    @autoreleasepool
    {
        MTLRenderPipelineDescriptor* const desc = [[MTLRenderPipelineDescriptor new] autorelease];
        desc.vertexFunction               = vertex;
        desc.fragmentFunction             = fragment;
        if (info.vertex_stride > 0)
        {
            MTLVertexDescriptor* const vertices = [MTLVertexDescriptor vertexDescriptor];
            for (u32 i = 0; i < info.vertex_attributes_count; i++)
            {
                const R_VertexAttribute& attribute = info.vertex_attributes[i];
                const MTLVertexFormat    format    = vertex_format(attribute.format);
                if (format == MTLVertexFormatInvalid)
                {
                    LOG_ERROR("Vertex attribute format %u has no Metal equivalent", (u32)attribute.format);
                    return NULL_HANDLE;
                }
                vertices.attributes[attribute.location].format      = format;
                vertices.attributes[attribute.location].offset      = attribute.offset;
                vertices.attributes[attribute.location].bufferIndex = R_METAL_VERTEX_BUFFER;
            }
            vertices.layouts[R_METAL_VERTEX_BUFFER].stride = info.vertex_stride;
            desc.vertexDescriptor                          = vertices;
        }
        MTLRenderPipelineColorAttachmentDescriptor* const color = desc.colorAttachments[0];
        color.pixelFormat = pixel_format(pass->color_format);
        color.writeMask   = info.no_color_write ? MTLColorWriteMaskNone : MTLColorWriteMaskAll;
        if (info.blend_enable && pass->color_format != R_Format::Unknown)
        {
            R_BlendFactor factors[4];
            r_pipeline_blend_factors(info, factors);
            color.blendingEnabled             = YES;
            color.sourceRGBBlendFactor        = blend_factor(factors[0]);
            color.destinationRGBBlendFactor   = blend_factor(factors[1]);
            color.sourceAlphaBlendFactor      = blend_factor(factors[2]);
            color.destinationAlphaBlendFactor = blend_factor(factors[3]);
            color.rgbBlendOperation           = MTLBlendOperationAdd;
            color.alphaBlendOperation         = MTLBlendOperationAdd;
        }
        desc.depthAttachmentPixelFormat   = pixel_format(pass->depth_format);
        desc.stencilAttachmentPixelFormat = r_format_has_stencil(pass->depth_format) ? pixel_format(pass->depth_format) : MTLPixelFormatInvalid;
        NSError*                         error = nil;
        const id<MTLRenderPipelineState> state = [device->device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!state)
        {
            LOG_ERROR("Cannot create a Metal pipeline: %s", error ? error.localizedDescription.UTF8String : "unknown error");
            return NULL_HANDLE;
        }
        MTLDepthStencilDescriptor* const depth_desc = [[MTLDepthStencilDescriptor new] autorelease];
        depth_desc.depthCompareFunction       = info.depth_test ? compare_function(info.depth_compare) : MTLCompareFunctionAlways;
        depth_desc.depthWriteEnabled          = info.depth_test && info.depth_write;
        return device->pipelines.add({ .state = state, .depth = [device->device newDepthStencilStateWithDescriptor:depth_desc], .textures_count = info.textures_count, .texture_type = info.texture_type, .push_constants_size = info.push_constants_size });
    }
}

static void metal_destroy_pipeline (R_Device* base_device, R_PipelineHandle handle)
{
    R_MetalDevice*   device   = metal_device(base_device);
    R_MetalPipeline* pipeline = device->pipelines.try_get(handle);
    if (!pipeline)
    {
        return;
    }
    metal_release(pipeline->state);
    metal_release(pipeline->depth);
    device->pipelines.remove(handle);
}

static b8 metal_is_valid_pipeline (const R_Device* base_device, R_PipelineHandle pipeline)
{
    return metal_device(base_device)->pipelines.is_valid(pipeline);
}

//
// Window presentation
//

static b8 metal_device_supports_present (const R_Device* base_device, const R_WindowTarget& target)
{
    (void)base_device;
    return target.metal_layer != nullptr;
}

// The layer never waits for the display (displaySyncEnabled), so the mirror never paces the app
static R_WindowSwapchainHandle metal_create_window_swapchain (R_Device* base_device, const R_WindowTarget& target, u32 width, u32 height)
{
    R_MetalDevice* device = metal_device(base_device);
    CAMetalLayer* const layer = (CAMetalLayer*)target.metal_layer;
    if (!layer || width == 0 || height == 0)
    {
        return NULL_HANDLE;
    }
    layer.device             = device->device;
    layer.pixelFormat        = MTLPixelFormatBGRA8Unorm_sRGB;
    layer.framebufferOnly    = NO;
    layer.drawableSize       = CGSizeMake(width, height);
    layer.displaySyncEnabled = NO;
    return device->windows.add({ .layer = [layer retain], .drawable = nil, .image = {}, .width = width, .height = height });
}

static void metal_destroy_window_swapchain (R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    R_MetalDevice*          device = metal_device(base_device);
    R_MetalWindowSwapchain* window = device->windows.try_get(swapchain);
    if (!window)
    {
        return;
    }
    destroy_window_resource(device, *window);
    device->windows.remove(swapchain);
}

static b8 metal_is_valid_window_swapchain (const R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    return metal_device(base_device)->windows.is_valid(swapchain);
}

static R_Format metal_window_swapchain_format (const R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    return metal_device(base_device)->windows.is_valid(swapchain) ? R_Format::B8G8R8A8_SRGB : R_Format::Unknown;
}

static void metal_window_swapchain_size (const R_Device* base_device, R_WindowSwapchainHandle swapchain, u32& out_width, u32& out_height)
{
    const R_MetalWindowSwapchain* window = metal_device(base_device)->windows.try_get(swapchain);
    out_width                            = window ? window->width : 0;
    out_height                           = window ? window->height : 0;
}

static b8 metal_window_acquire (R_Device* base_device, R_WindowSwapchainHandle handle, R_ImageHandle& out_image)
{
    R_MetalDevice*          device = metal_device(base_device);
    R_MetalWindowSwapchain* window = device->windows.try_get(handle);
    out_image                      = NULL_HANDLE;
    if (!window)
    {
        return false;
    }
    metal_release(window->drawable);
    @autoreleasepool
    {
        window->drawable = [[window->layer nextDrawable] retain];
    }
    if (!window->drawable)
    {
        return false;
    }
    const id<MTLTexture> texture = window->drawable.texture;
    if (!device->images.is_valid(window->image))
    {
        window->image = device->images.add({ .texture = nil, .array_view = nil, .width = 0, .height = 0, .layers = 1, .format = R_Format::B8G8R8A8_SRGB });
    }
    R_MetalImage& image = device->images.get(window->image);
    metal_release(image.texture);
    metal_release(image.array_view);
    image.texture = [texture retain];
    image.width   = (u32)texture.width;
    image.height  = (u32)texture.height;
    out_image     = window->image;
    return true;
}

// The drawable was presented by the command buffer of r_end_frame_present
static b8 metal_window_present (R_Device* base_device, R_WindowSwapchainHandle handle)
{
    R_MetalDevice*          device = metal_device(base_device);
    R_MetalWindowSwapchain* window = device->windows.try_get(handle);
    if (!window)
    {
        return false;
    }
    device->queue_accesses++;
    metal_release(window->drawable);
    return true;
}

//
// Frames and immediate commands
//

static R_CmdBufferHandle begin_recording (R_MetalDevice* device, u32 index)
{
    R_MetalSlot& slot = device->slots[index];
    if (slot.recording)
    {
        ASSERT_MSG(false, "command buffer %u is still recording", index);
        return NULL_HANDLE;
    }
    wait_slot(slot);
    @autoreleasepool
    {
        slot.cmd = [[device->queue commandBuffer] retain];
    }
    if (!slot.cmd)
    {
        LOG_ERROR("Cannot create a Metal command buffer");
        return NULL_HANDLE;
    }
    slot.recording   = true;
    slot.framebuffer = NULL_HANDLE;
    slot.draw        = {};
    slot.present     = NULL_HANDLE;
    return device->cmd_handles[index];
}

static b8 end_recording (R_MetalDevice* device, R_CmdBufferHandle cmd)
{
    R_MetalSlot* slot = cmd_slot(device, cmd);
    if (!slot)
    {
        ASSERT_MSG(false, "invalid command buffer");
        return false;
    }
    end_encoders(*slot);
    R_MetalWindowSwapchain* window = device->windows.try_get(slot->present);
    if (window && window->drawable)
    {
        [slot->cmd presentDrawable:window->drawable];
        metal_release(window->presenting);
        window->presenting = [slot->cmd retain];
    }
    [slot->cmd commit];
    slot->recording = false;
    device->queue_accesses++;
    return true;
}

static R_CmdBufferHandle metal_begin_frame (R_Device* base_device)
{
    R_MetalDevice* device = metal_device(base_device);
    const u32      index  = device->frame_index;
    device->frame_index   = (device->frame_index + 1) % R_MAX_FRAMES_IN_FLIGHT;
    return begin_recording(device, index);
}

static void metal_end_frame (R_Device* base_device, R_CmdBufferHandle cmd)
{
    end_recording(metal_device(base_device), cmd);
}

// nextDrawable waits while the GPU holds the drawables; it held Unity's render thread, and the
// runtime lock with it, for up to 20 ms
static b8 metal_window_busy (const R_Device* base_device, R_WindowSwapchainHandle handle)
{
    const R_MetalWindowSwapchain* window = metal_device(base_device)->windows.try_get(handle);
    return window && window->presenting && [window->presenting status] < MTLCommandBufferStatusCompleted;
}

static void metal_end_frame_present (R_Device* base_device, R_CmdBufferHandle cmd, R_WindowSwapchainHandle swapchain)
{
    R_MetalDevice* device = metal_device(base_device);
    R_MetalSlot*   slot   = cmd_slot(device, cmd);
    if (slot)
    {
        slot->present = swapchain;
    }
    end_recording(device, cmd);
}

static R_CmdBufferHandle metal_begin_immediate (R_Device* base_device)
{
    return begin_recording(metal_device(base_device), R_MAX_FRAMES_IN_FLIGHT);
}

static void metal_end_immediate (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_MetalDevice* device = metal_device(base_device);
    if (end_recording(device, cmd))
    {
        wait_slot(device->slots[R_MAX_FRAMES_IN_FLIGHT]);
    }
}

static u64 metal_get_cmd_native (const R_Device* base_device, R_CmdBufferHandle cmd)
{
    const R_MetalCmdBuffer* buffer = metal_device(base_device)->cmds.try_get(cmd);
    return buffer ? (u64)(uintptr_t)metal_device(base_device)->slots[buffer->slot].cmd : 0;
}

//
// Commands
//

static void metal_cmd_transition_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle handle, R_ImageLayout old_layout, R_ImageLayout new_layout)
{
    R_MetalDevice* device = metal_device(base_device);
    (void)old_layout;
    (void)new_layout;
    if (!cmd_slot(device, cmd) || !device->images.is_valid(handle))
    {
        ASSERT_MSG(false, "invalid transition");
    }
}

static void metal_cmd_begin_render_pass (R_Device* base_device, R_CmdBufferHandle cmd, R_RenderPassHandle pass_handle, R_FramebufferHandle fb_handle, const R_ClearColor& clear)
{
    R_MetalDevice*            device = metal_device(base_device);
    R_MetalSlot*              slot   = cmd_slot(device, cmd);
    const R_MetalRenderPass*  pass   = device->render_passes.try_get(pass_handle);
    const R_MetalFramebuffer* fb     = device->framebuffers.try_get(fb_handle);
    if (!slot || !pass || !fb || slot->render)
    {
        ASSERT_MSG(false, "invalid render pass or framebuffer");
        return;
    }
    end_encoders(*slot);
    @autoreleasepool
    {
        slot->render = [[slot->cmd renderCommandEncoderWithDescriptor:pass_descriptor(device, *pass, *fb, clear)] retain];
    }
    slot->framebuffer   = fb_handle;
    slot->draw.viewport = { 0.0, 0.0, (f64)fb->width, (f64)fb->height, 0.0, 1.0 };
    slot->draw.scissor  = { 0, 0, fb->width, fb->height };
}

static void metal_cmd_end_render_pass (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_MetalSlot* slot = cmd_slot(metal_device(base_device), cmd);
    if (!slot || !slot->render)
    {
        ASSERT_MSG(false, "no render pass to end");
        return;
    }
    end_encoders(*slot);
    slot->framebuffer = NULL_HANDLE;
}

static void metal_cmd_bind_pipeline (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle handle)
{
    R_MetalDevice* device = metal_device(base_device);
    R_MetalSlot*   slot   = cmd_slot(device, cmd);
    if (!slot || !device->pipelines.is_valid(handle))
    {
        ASSERT_MSG(false, "invalid pipeline binding");
        return;
    }
    slot->draw.pipeline = handle;
}

static void metal_cmd_bind_textures (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle pipeline_handle, const R_ImageHandle* images, u32 count, R_Filter filter)
{
    R_MetalDevice*         device   = metal_device(base_device);
    R_MetalSlot*           slot     = cmd_slot(device, cmd);
    const R_MetalPipeline* pipeline = device->pipelines.try_get(pipeline_handle);
    if (!slot || !pipeline || !images || count == 0 || count != pipeline->textures_count)
    {
        ASSERT_MSG(false, "invalid texture binding");
        return;
    }
    for (u32 i = 0; i < count; i++)
    {
        const R_MetalImage* image = device->images.try_get(images[i]);
        if (!image || (pipeline->texture_type == R_TextureType::Type2D && image->texture.textureType != MTLTextureType2D))
        {
            ASSERT_MSG(false, "invalid texture binding");
            return;
        }
        slot->draw.textures[i] = images[i];
    }
    slot->draw.textures_count = count;
    slot->draw.filter         = filter;
}

static void metal_cmd_push_constants (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle handle, const void* data, u32 size)
{
    R_MetalDevice*         device   = metal_device(base_device);
    R_MetalSlot*           slot     = cmd_slot(device, cmd);
    const R_MetalPipeline* pipeline = device->pipelines.try_get(handle);
    if (!slot || !pipeline || !data || size == 0 || size > pipeline->push_constants_size)
    {
        ASSERT_MSG(false, "invalid push constants");
        return;
    }
    memory_copy(slot->draw.push, data, size);
    slot->draw.push_size = zm::max(slot->draw.push_size, size);
}

static void metal_cmd_set_viewport (R_Device* base_device, R_CmdBufferHandle cmd, const R_Viewport& viewport)
{
    R_MetalSlot* slot = cmd_slot(metal_device(base_device), cmd);
    if (!slot)
    {
        ASSERT_MSG(false, "invalid command buffer");
        return;
    }
    slot->draw.viewport = { (f64)viewport.x, (f64)viewport.y, (f64)viewport.width, (f64)viewport.height, 0.0, 1.0 };
}

static void metal_cmd_set_scissor (R_Device* base_device, R_CmdBufferHandle cmd, const R_Rect& scissor)
{
    R_MetalSlot* slot = cmd_slot(metal_device(base_device), cmd);
    if (!slot)
    {
        ASSERT_MSG(false, "invalid command buffer");
        return;
    }
    const u32 x         = (u32)zm::max(scissor.x, 0);
    const u32 y         = (u32)zm::max(scissor.y, 0);
    slot->draw.scissor = { x, y, scissor.width, scissor.height };
}

static void metal_cmd_bind_vertex_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset)
{
    R_MetalDevice* device = metal_device(base_device);
    R_MetalSlot*   slot   = cmd_slot(device, cmd);
    if (!slot || !device->buffers.is_valid(handle))
    {
        ASSERT_MSG(false, "invalid vertex buffer");
        return;
    }
    slot->draw.vertex_buffer = handle;
    slot->draw.vertex_offset = offset;
}

static void metal_cmd_bind_index_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset, b8 u16_indices)
{
    R_MetalDevice* device = metal_device(base_device);
    R_MetalSlot*   slot   = cmd_slot(device, cmd);
    if (!slot || !device->buffers.is_valid(handle))
    {
        ASSERT_MSG(false, "invalid index buffer");
        return;
    }
    slot->draw.index_buffer = handle;
    slot->draw.index_offset = offset;
    slot->draw.u16_indices  = u16_indices;
}

// Metal requires the scissor inside the attachment
static MTLScissorRect clamp_scissor (MTLScissorRect scissor, u32 width, u32 height)
{
    const NSUInteger x = zm::min((u32)scissor.x, width);
    const NSUInteger y = zm::min((u32)scissor.y, height);
    return { x, y, zm::min((u32)scissor.width, width - (u32)x), zm::min((u32)scissor.height, height - (u32)y) };
}

static b8 prepare_draw (R_MetalDevice* device, R_MetalSlot& slot)
{
    R_MetalDrawState&         draw     = slot.draw;
    const R_MetalPipeline*    pipeline = device->pipelines.try_get(draw.pipeline);
    const R_MetalFramebuffer* fb       = device->framebuffers.try_get(slot.framebuffer);
    if (!slot.render || !pipeline || !fb || draw.textures_count != pipeline->textures_count)
    {
        ASSERT_MSG(false, "draw without a render pass, a pipeline or its textures");
        return false;
    }
    const MTLScissorRect scissor = clamp_scissor(draw.scissor, fb->width, fb->height);
    if (scissor.width == 0 || scissor.height == 0)
    {
        return false;
    }
    const id<MTLRenderCommandEncoder> encoder = slot.render;
    [encoder setRenderPipelineState:pipeline->state];
    [encoder setDepthStencilState:pipeline->depth];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setViewport:draw.viewport];
    [encoder setScissorRect:scissor];
    if (pipeline->push_constants_size > 0)
    {
        [encoder setVertexBytes:draw.push length:pipeline->push_constants_size atIndex:0];
        [encoder setFragmentBytes:draw.push length:pipeline->push_constants_size atIndex:0];
    }
    for (u32 i = 0; i < draw.textures_count; i++)
    {
        R_MetalImage* image = device->images.try_get(draw.textures[i]);
        if (!image)
        {
            ASSERT_MSG(false, "a bound texture was destroyed");
            return false;
        }
        [encoder setFragmentTexture:sampled_texture(*image, pipeline->texture_type == R_TextureType::Type2DArray) atIndex:i];
        [encoder setFragmentSamplerState:device->samplers[draw.filter == R_Filter::Linear ? 1 : 0] atIndex:i];
    }
    const R_MetalBuffer* vertices = device->buffers.try_get(draw.vertex_buffer);
    if (vertices)
    {
        [encoder setVertexBuffer:vertices->buffer offset:draw.vertex_offset atIndex:R_METAL_VERTEX_BUFFER];
    }
    return true;
}

static void metal_cmd_draw (R_Device* base_device, R_CmdBufferHandle cmd, u32 vertex_count, u32 first_vertex)
{
    R_MetalDevice* device = metal_device(base_device);
    R_MetalSlot*   slot   = cmd_slot(device, cmd);
    if (!slot || vertex_count == 0 || !prepare_draw(device, *slot))
    {
        return;
    }
    [slot->render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:first_vertex vertexCount:vertex_count];
}

static void metal_cmd_draw_indexed (R_Device* base_device, R_CmdBufferHandle cmd, u32 index_count, u32 first_index, i32 vertex_offset)
{
    R_MetalDevice*       device  = metal_device(base_device);
    R_MetalSlot*         slot    = cmd_slot(device, cmd);
    const R_MetalBuffer* indices = slot ? device->buffers.try_get(slot->draw.index_buffer) : nullptr;
    if (!slot || !indices)
    {
        ASSERT_MSG(false, "indexed draw without an index buffer");
        return;
    }
    if (index_count == 0 || !prepare_draw(device, *slot))
    {
        return;
    }
    const u64 index_size = slot->draw.u16_indices ? 2 : 4;
    [slot->render drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:index_count indexType:slot->draw.u16_indices ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32 indexBuffer:indices->buffer
                      indexBufferOffset:slot->draw.index_offset + first_index * index_size instanceCount:1 baseVertex:vertex_offset baseInstance:0];
}

// Inside a render pass: a draw of the blit vertices without fragments, through a viewport with
// depth range [depth, depth] and the scissor of the rectangle
static void metal_cmd_clear_depth (R_Device* base_device, R_CmdBufferHandle cmd, const R_Rect& rect, f32 depth)
{
    R_MetalDevice*            device = metal_device(base_device);
    const R_MetalSlot*        slot   = cmd_slot(device, cmd);
    const R_MetalFramebuffer* fb     = slot ? device->framebuffers.try_get(slot->framebuffer) : nullptr;
    const R_MetalRenderPass*  pass   = fb ? device->render_passes.try_get(fb->render_pass) : nullptr;
    if (!slot || !slot->render || !pass || pass->depth_format == R_Format::Unknown)
    {
        ASSERT_MSG(false, "depth clear outside a render pass with depth");
        return;
    }
    const id<MTLRenderPipelineState> state = variant_state(device, pass->color_format, pass->depth_format, true);
    const MTLScissorRect             scissor = clamp_scissor({ (NSUInteger)zm::max(rect.x, 0), (NSUInteger)zm::max(rect.y, 0), rect.width, rect.height }, fb->width, fb->height);
    if (!state || scissor.width == 0 || scissor.height == 0)
    {
        return;
    }
    const R_BlitPush push = { .src_rect = { 0.0f, 0.0f, 1.0f, 1.0f }, .layer = 0 };
    [slot->render setRenderPipelineState:state];
    [slot->render setDepthStencilState:device->depth_overwrite];
    [slot->render setCullMode:MTLCullModeNone];
    [slot->render setViewport:{ (f64)rect.x, (f64)rect.y, (f64)rect.width, (f64)rect.height, (f64)depth, (f64)depth }];
    [slot->render setScissorRect:scissor];
    [slot->render setVertexBytes:&push length:sizeof(push) atIndex:0];
    [slot->render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

static void metal_cmd_copy_buffer_to_image (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle buffer_handle, R_ImageHandle image_handle, u32 layer)
{
    R_MetalDevice*       device = metal_device(base_device);
    R_MetalSlot*         slot   = cmd_slot(device, cmd);
    const R_MetalBuffer* buffer = device->buffers.try_get(buffer_handle);
    const R_MetalImage*  image  = device->images.try_get(image_handle);
    const u64            row    = image ? (u64)image->width * r_format_bytes_per_pixel(image->format) : 0;
    if (!slot || slot->render || !buffer || !image || layer >= image->layers || r_format_is_depth(image->format) || buffer->size < row * image->height)
    {
        ASSERT_MSG(false, "invalid buffer to image copy");
        return;
    }
    [blit_encoder(*slot) copyFromBuffer:buffer->buffer sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * image->height sourceSize:MTLSizeMake(image->width, image->height, 1)
                              toTexture:image->texture destinationSlice:layer destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
}

// Depth with stencil comes out as its depth only, 4 bytes a pixel
static void metal_cmd_copy_image_to_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle image_handle, u32 layer, R_BufferHandle buffer_handle)
{
    R_MetalDevice*       device  = metal_device(base_device);
    R_MetalSlot*         slot    = cmd_slot(device, cmd);
    const R_MetalBuffer* buffer  = device->buffers.try_get(buffer_handle);
    const R_MetalImage*  image   = device->images.try_get(image_handle);
    const b8             stencil = image && r_format_has_stencil(image->format);
    const u64            row     = image ? (u64)image->width * (stencil ? 4 : r_format_bytes_per_pixel(image->format)) : 0;
    if (!slot || slot->render || !buffer || !image || layer >= image->layers || buffer->size < row * image->height)
    {
        ASSERT_MSG(false, "invalid image to buffer copy");
        return;
    }
    [blit_encoder(*slot) copyFromTexture:image->texture sourceSlice:layer sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(image->width, image->height, 1)
                                toBuffer:buffer->buffer destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * image->height
                                 options:stencil ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
}

// A render pass of its own per layer that clears and draws nothing
static void metal_cmd_clear_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle handle, const R_ClearColor& color)
{
    R_MetalDevice*      device = metal_device(base_device);
    R_MetalSlot*        slot   = cmd_slot(device, cmd);
    const R_MetalImage* image  = device->images.try_get(handle);
    if (!slot || slot->render || !image || r_format_is_depth(image->format))
    {
        ASSERT_MSG(false, "invalid clear");
        return;
    }
    end_encoders(*slot);
    for (u32 layer = 0; layer < image->layers; layer++)
    {
        @autoreleasepool
        {
            MTLRenderPassDescriptor* const desc  = [MTLRenderPassDescriptor renderPassDescriptor];
            desc.colorAttachments[0].texture     = image->texture;
            desc.colorAttachments[0].slice       = layer;
            desc.colorAttachments[0].loadAction  = MTLLoadActionClear;
            desc.colorAttachments[0].storeAction = MTLStoreActionStore;
            desc.colorAttachments[0].clearColor  = MTLClearColorMake((f64)color.r, (f64)color.g, (f64)color.b, (f64)color.a);
            [[slot->cmd renderCommandEncoderWithDescriptor:desc] endEncoding];
        }
    }
}

// A draw of the source through the blit shaders into the destination rectangle
static void metal_cmd_blit_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle src_handle, u32 src_layer, R_ImageHandle dst_handle, const R_Rect& dst_rect, R_Filter filter)
{
    R_MetalDevice*      device = metal_device(base_device);
    R_MetalSlot*        slot   = cmd_slot(device, cmd);
    R_MetalImage*       src    = device->images.try_get(src_handle);
    const R_MetalImage* dst    = device->images.try_get(dst_handle);
    if (!slot || slot->render || !src || !dst || src_layer >= src->layers || r_format_is_depth(src->format) || r_format_is_depth(dst->format))
    {
        ASSERT_MSG(false, "invalid blit");
        return;
    }
    const id<MTLRenderPipelineState> state   = variant_state(device, dst->format, R_Format::Unknown, false);
    const MTLScissorRect             scissor = clamp_scissor({ (NSUInteger)zm::max(dst_rect.x, 0), (NSUInteger)zm::max(dst_rect.y, 0), dst_rect.width, dst_rect.height }, dst->width, dst->height);
    if (!state || scissor.width == 0 || scissor.height == 0)
    {
        return;
    }
    end_encoders(*slot);
    const R_BlitPush push = { .src_rect = { 0.0f, 0.0f, 1.0f, 1.0f }, .layer = (i32)src_layer };
    @autoreleasepool
    {
        MTLRenderPassDescriptor* const desc  = [MTLRenderPassDescriptor renderPassDescriptor];
        desc.colorAttachments[0].texture     = dst->texture;
        desc.colorAttachments[0].loadAction  = MTLLoadActionLoad;
        desc.colorAttachments[0].storeAction = MTLStoreActionStore;
        const id<MTLRenderCommandEncoder> encoder = [slot->cmd renderCommandEncoderWithDescriptor:desc];
        [encoder setRenderPipelineState:state];
        [encoder setDepthStencilState:device->no_depth];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setViewport:{ (f64)dst_rect.x, (f64)dst_rect.y, (f64)dst_rect.width, (f64)dst_rect.height, 0.0, 1.0 }];
        [encoder setScissorRect:scissor];
        [encoder setVertexBytes:&push length:sizeof(push) atIndex:0];
        [encoder setFragmentBytes:&push length:sizeof(push) atIndex:0];
        [encoder setFragmentTexture:sampled_texture(*src, true) atIndex:0];
        [encoder setFragmentSamplerState:device->samplers[filter == R_Filter::Linear ? 1 : 0] atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
    }
}

static b8 metal_read_image (R_Device* base_device, R_ImageHandle handle, u32 layer, R_ImageLayout layout, void* out_pixels, usize out_size)
{
    R_MetalDevice*      device = metal_device(base_device);
    const R_MetalImage* image  = device->images.try_get(handle);
    (void)layout;
    if (!image || !out_pixels || layer >= image->layers)
    {
        LOG_ERROR("Invalid image readback");
        return false;
    }
    const usize size = (usize)image->width * image->height * r_format_bytes_per_pixel(image->format);
    if (out_size < size)
    {
        LOG_ERROR("Readback buffer too small: %llu < %llu", (unsigned long long)out_size, (unsigned long long)size);
        return false;
    }
    const R_BufferHandle staging = metal_create_buffer(base_device, { .size = size, .usage_flags = (u32)R_BufferUsageFlags::TransferDst, .host_visible = true });
    if (!staging)
    {
        return false;
    }
    const R_CmdBufferHandle cmd = metal_begin_immediate(base_device);
    metal_cmd_copy_image_to_buffer(base_device, cmd, handle, layer, staging);
    metal_end_immediate(base_device, cmd);
    memory_copy(out_pixels, metal_get_buffer_mapped_data(base_device, staging), size);
    metal_destroy_buffer(base_device, staging);
    return true;
}

//
// Backend
//

const static R_BackendFns metal_fns = {
    .destroy_device            = metal_destroy_device,
    .wait_idle                 = metal_wait_idle,
    .flush_initial_layouts     = metal_flush_initial_layouts,
    .queue_access_count        = metal_queue_access_count,
    .native_format             = metal_native_format,
    .format_from_native        = metal_format_from_native,
    .adapter_luid              = nullptr,
    .x11_visual                = nullptr,
    .create_image              = metal_create_image,
    .image_flags_supported     = metal_image_flags_supported,
    .import_image              = metal_import_image,
    .destroy_image             = metal_destroy_image,
    .is_valid_image            = metal_is_valid_image,
    .get_image_native          = metal_get_image_native,
    .get_image_size            = metal_get_image_size,
    .get_image_format          = metal_get_image_format,
    .create_buffer             = metal_create_buffer,
    .destroy_buffer            = metal_destroy_buffer,
    .is_valid_buffer           = metal_is_valid_buffer,
    .get_buffer_mapped_data    = metal_get_buffer_mapped_data,
    .flush_buffer              = metal_flush_buffer,
    .create_render_pass        = metal_create_render_pass,
    .destroy_render_pass       = metal_destroy_render_pass,
    .is_valid_render_pass      = metal_is_valid_render_pass,
    .create_framebuffer        = metal_create_framebuffer,
    .destroy_framebuffer       = metal_destroy_framebuffer,
    .is_valid_framebuffer      = metal_is_valid_framebuffer,
    .create_pipeline           = metal_create_pipeline,
    .destroy_pipeline          = metal_destroy_pipeline,
    .is_valid_pipeline         = metal_is_valid_pipeline,
    .device_supports_present   = metal_device_supports_present,
    .create_window_swapchain   = metal_create_window_swapchain,
    .destroy_window_swapchain  = metal_destroy_window_swapchain,
    .is_valid_window_swapchain = metal_is_valid_window_swapchain,
    .window_swapchain_format   = metal_window_swapchain_format,
    .window_swapchain_size     = metal_window_swapchain_size,
    .window_acquire            = metal_window_acquire,
    .window_present            = metal_window_present,
    .window_busy               = metal_window_busy,
    .begin_frame               = metal_begin_frame,
    .end_frame                 = metal_end_frame,
    .end_frame_present         = metal_end_frame_present,
    .begin_immediate           = metal_begin_immediate,
    .end_immediate             = metal_end_immediate,
    .get_cmd_native            = metal_get_cmd_native,
    .cmd_transition_image      = metal_cmd_transition_image,
    .cmd_begin_render_pass     = metal_cmd_begin_render_pass,
    .cmd_end_render_pass       = metal_cmd_end_render_pass,
    .cmd_bind_pipeline         = metal_cmd_bind_pipeline,
    .cmd_bind_textures         = metal_cmd_bind_textures,
    .cmd_clear_depth           = metal_cmd_clear_depth,
    .cmd_push_constants        = metal_cmd_push_constants,
    .cmd_set_viewport          = metal_cmd_set_viewport,
    .cmd_set_scissor           = metal_cmd_set_scissor,
    .cmd_bind_vertex_buffer    = metal_cmd_bind_vertex_buffer,
    .cmd_bind_index_buffer     = metal_cmd_bind_index_buffer,
    .cmd_draw                  = metal_cmd_draw,
    .cmd_draw_indexed          = metal_cmd_draw_indexed,
    .cmd_copy_buffer_to_image  = metal_cmd_copy_buffer_to_image,
    .cmd_copy_image_to_buffer  = metal_cmd_copy_image_to_buffer,
    .cmd_clear_image           = metal_cmd_clear_image,
    .cmd_blit_image            = metal_cmd_blit_image,
    .read_image                = metal_read_image,
};

static b8 create_shared_state (R_MetalDevice* device)
{
    @autoreleasepool
    {
        for (u32 i = 0; i < 2; i++)
        {
            MTLSamplerDescriptor* const sampler = [[MTLSamplerDescriptor new] autorelease];
            sampler.minFilter             = i ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sampler.magFilter             = i ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sampler.mipFilter             = MTLSamplerMipFilterNotMipmapped;
            sampler.sAddressMode          = MTLSamplerAddressModeClampToEdge;
            sampler.tAddressMode          = MTLSamplerAddressModeClampToEdge;
            sampler.rAddressMode          = MTLSamplerAddressModeClampToEdge;
            device->samplers[i]           = [device->device newSamplerStateWithDescriptor:sampler];
        }
        MTLDepthStencilDescriptor* const none = [[MTLDepthStencilDescriptor new] autorelease];
        none.depthCompareFunction       = MTLCompareFunctionAlways;
        none.depthWriteEnabled          = NO;
        device->no_depth                = [device->device newDepthStencilStateWithDescriptor:none];
        MTLDepthStencilDescriptor* const overwrite = [[MTLDepthStencilDescriptor new] autorelease];
        overwrite.depthCompareFunction       = MTLCompareFunctionAlways;
        overwrite.depthWriteEnabled          = YES;
        device->depth_overwrite              = [device->device newDepthStencilStateWithDescriptor:overwrite];
    }
    return device->samplers[0] && device->samplers[1] && device->no_depth && device->depth_overwrite;
}

//
// Public API (render layer internal)
//

void* r_metal_system_device ()
{
    static id<MTLDevice> system_device = nil;
    if (!system_device)
    {
        system_device = MTLCreateSystemDefaultDevice();
    }
    return (void*)system_device;
}

R_Device* r_metal_create_device (const R_DeviceCreateInfo& create_info)
{
    const id<MTLCommandQueue> queue = (id<MTLCommandQueue>)create_info.metal.queue;
    if (!queue)
    {
        LOG_ERROR("Render device needs the app's MTLCommandQueue");
        return nullptr;
    }
    R_MetalDevice* device = (R_MetalDevice*)os_memory_alloc(sizeof(R_MetalDevice));
    memory_zero(device, sizeof(R_MetalDevice));
    device->base   = { .fns = &metal_fns, .backend = R_Backend::Metal };
    device->queue  = [queue retain];
    device->device = [queue.device retain];
    if (!create_shared_state(device))
    {
        LOG_ERROR("Cannot set up the Metal device");
        metal_destroy_device(&device->base);
        return nullptr;
    }
    for (u32 i = 0; i < ARRAY_SIZE(device->cmd_handles); i++)
    {
        device->cmd_handles[i] = device->cmds.add({ .slot = i });
    }
    return &device->base;
}
