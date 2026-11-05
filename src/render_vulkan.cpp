#include "render_internal.h"

#include <volk.h>

//
// Types
//

#define R_MAX_DESCRIPTOR_SETS 256

namespace
{

struct R_VkImage
{
    VkImage        image;
    VkDeviceMemory memory; // null for imported images
    VkImageView    array_view;
    VkImageView    layer_views[R_MAX_LAYER_VIEWS];
    u32            width;
    u32            height;
    u32            layers;
    u32            mip_levels;
    R_Format       format;
    VkImageAspectFlags aspect;
    b8             has_views;
    R_ImageLayout  pending_layout; // initial layout not yet applied, Undefined when none
};

struct R_VkBuffer
{
    VkBuffer       buffer;
    VkDeviceMemory memory;
    void*          mapped;
    u64            size;
};

struct R_VkRenderPass
{
    VkRenderPass render_pass;
    R_Format     format;       // Unknown for a depth-only pass
    R_Format     depth_format; // Unknown without depth
};

struct R_VkFramebuffer
{
    VkFramebuffer framebuffer;
    u32           width;
    u32           height;
};

struct R_VkPipeline
{
    VkPipeline       pipeline;
    VkPipelineLayout layout;
    u32              textures_count;
    R_TextureType    texture_type;
    u32              push_constants_size;
};

#define R_MAX_WINDOW_IMAGES 4

struct R_VkWindowSwapchain
{
    VkSwapchainKHR swapchain = {};
    VkSurfaceKHR   surface = {};
    R_ImageHandle  images[R_MAX_WINDOW_IMAGES];
    u32            images_count = {};
    u32            width = {};
    u32            height = {};
    R_Format       format = {};
    VkSemaphore    acquire_semaphores[R_MAX_FRAMES_IN_FLIGHT] = {};
    VkSemaphore    present_semaphores[R_MAX_WINDOW_IMAGES] = {};
    u32            acquire_index = {};   // which acquire semaphore the current frame used
    u32            current_image = {};
    b8             acquired = {};
    u32            presented_frame = R_MAX_FRAMES_IN_FLIGHT; // the frame slot that presented last, none at first
    u64            presented_submit = {};                    // that slot's submit count after the present
};

struct R_VkCmdBuffer
{
    VkCommandBuffer cmd;
    u32             frame; // index into frames, or R_MAX_FRAMES_IN_FLIGHT for the immediate buffer
};

struct R_VkFrame
{
    VkCommandPool    pool = {};
    VkCommandBuffer  cmd = {};
    VkFence          fence = {};
    VkDescriptorPool descriptors = {};
    R_CmdBufferHandle handle;
    b8               recording = {};
    u64              submits = {};
};

struct R_VkDevice
{
    R_Device                                            base = {};
    VolkDeviceTable                                     vk = {};
    VkInstance                                          instance = {};
    VkPhysicalDevice                                    physical_device = {};
    VkDevice                                            device = {};
    VkQueue                                             queue = {};
    u32                                                 queue_family = {};
    VkPhysicalDeviceMemoryProperties                    memory_properties = {};

    VkDescriptorSetLayout                               texture_layouts[R_MAX_TEXTURES] = {}; // index n holds n + 1 samplers
    VkSampler                                           samplers[2] = {};
    R_VkFrame                                           frames[R_MAX_FRAMES_IN_FLIGHT + 1]; // last one is the immediate buffer
    u32                                                 frame_index = {};

    SparseArray<R_VkImage, R_Image>                     images = {};
    SparseArray<R_VkBuffer, R_Buffer>                   buffers = {};
    SparseArray<R_VkRenderPass, R_RenderPass>           render_passes = {};
    SparseArray<R_VkFramebuffer, R_Framebuffer>         framebuffers = {};
    SparseArray<R_VkPipeline, R_Pipeline>               pipelines = {};
    SparseArray<R_VkCmdBuffer, R_CmdBuffer>             cmds = {};
    SparseArray<R_VkWindowSwapchain, R_WindowSwapchain> windows = {};
    Array<R_ImageHandle>                                pending_layouts = {};
    u64                                                 queue_accesses = {};
};

} // namespace

//
// Statics
//

#define VK_CHECK(expr)                                                     \
    do                                                                     \
    {                                                                      \
        const VkResult vk_result = (expr);                                 \
        if (vk_result != VK_SUCCESS)                                       \
        {                                                                  \
            FAIL_MSG("%s failed with VkResult %d", #expr, (i32)vk_result); \
        }                                                                  \
    }                                                                      \
    while (0)

const static struct
{
    R_Format format;
    VkFormat vk;
    u32      bytes;
    b8       depth;
} r_formats[] = {
    { R_Format::Unknown,             VK_FORMAT_UNDEFINED,           0, false },
    { R_Format::R8G8B8A8_UNORM,      VK_FORMAT_R8G8B8A8_UNORM,      4, false },
    { R_Format::B8G8R8A8_UNORM,      VK_FORMAT_B8G8R8A8_UNORM,      4, false },
    { R_Format::R8G8B8A8_SRGB,       VK_FORMAT_R8G8B8A8_SRGB,       4, false },
    { R_Format::B8G8R8A8_SRGB,       VK_FORMAT_B8G8R8A8_SRGB,       4, false },
    { R_Format::R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8, false },
    { R_Format::R32G32_SFLOAT,       VK_FORMAT_R32G32_SFLOAT,       8, false },
    { R_Format::R32G32B32_SFLOAT,    VK_FORMAT_R32G32B32_SFLOAT,    12, false },
    { R_Format::D16_UNORM,           VK_FORMAT_D16_UNORM,           2, true },
    { R_Format::D24_UNORM_S8_UINT,   VK_FORMAT_D24_UNORM_S8_UINT,   4, true },
    { R_Format::D32_SFLOAT,          VK_FORMAT_D32_SFLOAT,          4, true },
    { R_Format::D32_SFLOAT_S8_UINT,  VK_FORMAT_D32_SFLOAT_S8_UINT,  8, true },
};

static R_VkDevice* vk_device (R_Device* device)
{
    return (R_VkDevice*)device;
}

static const R_VkDevice* vk_device (const R_Device* device)
{
    return (const R_VkDevice*)device;
}

static b8 format_is_depth (R_Format format)
{
    return (u32)format < ARRAY_SIZE(r_formats) && r_formats[(u32)format].depth;
}

static VkFormat vk_format (R_Format format)
{
    return (u32)format < ARRAY_SIZE(r_formats) ? r_formats[(u32)format].vk : VK_FORMAT_UNDEFINED;
}

// Compares as integers: the app's value need not be a valid VkFormat
static R_Format format_from_vk (i64 format)
{
    for (u32 i = 1; i < ARRAY_SIZE(r_formats); i++)
    {
        if ((i64)r_formats[i].vk == format)
        {
            return r_formats[i].format;
        }
    }
    return R_Format::Unknown;
}

static VkCompareOp vk_compare_op (R_CompareOp op)
{
    switch (op)
    {
    case R_CompareOp::LessOrEqual:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case R_CompareOp::Always:
        return VK_COMPARE_OP_ALWAYS;
    case R_CompareOp::Never:
        return VK_COMPARE_OP_NEVER;
    case R_CompareOp::Less:
        return VK_COMPARE_OP_LESS;
    case R_CompareOp::Equal:
        return VK_COMPARE_OP_EQUAL;
    case R_CompareOp::Greater:
        return VK_COMPARE_OP_GREATER;
    case R_CompareOp::NotEqual:
        return VK_COMPARE_OP_NOT_EQUAL;
    case R_CompareOp::GreaterOrEqual:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    }
    return VK_COMPARE_OP_LESS_OR_EQUAL;
}

static VkBlendFactor vk_blend_factor (R_BlendFactor factor)
{
    switch (factor)
    {
    case R_BlendFactor::Zero:
        return VK_BLEND_FACTOR_ZERO;
    case R_BlendFactor::One:
        return VK_BLEND_FACTOR_ONE;
    case R_BlendFactor::SrcAlpha:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case R_BlendFactor::OneMinusSrcAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case R_BlendFactor::DstAlpha:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case R_BlendFactor::OneMinusDstAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

static VkImageLayout vk_layout (R_ImageLayout layout, b8 depth)
{
    switch (layout)
    {
    case R_ImageLayout::Undefined:
        return VK_IMAGE_LAYOUT_UNDEFINED;
    case R_ImageLayout::General:
        return VK_IMAGE_LAYOUT_GENERAL;
    case R_ImageLayout::ColorAttachment:
        return depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case R_ImageLayout::DepthStencilAttachment:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case R_ImageLayout::ShaderReadOnly:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case R_ImageLayout::TransferSrc:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case R_ImageLayout::TransferDst:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case R_ImageLayout::Present:
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

static VkAccessFlags layout_access (VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return VK_ACCESS_SHADER_READ_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_GENERAL:
        return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    default:
        return 0;
    }
}

static u32 find_memory_type (const R_VkDevice* device, u32 type_bits, VkMemoryPropertyFlags required)
{
    for (u32 i = 0; i < device->memory_properties.memoryTypeCount; i++)
    {
        if ((type_bits & (1u << i)) && (device->memory_properties.memoryTypes[i].propertyFlags & required) == required)
        {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

static VkImageView create_view (const R_VkDevice* device, VkImage image, VkFormat format, VkImageAspectFlags aspect, VkImageViewType type, u32 base_layer, u32 layers)
{
    const VkImageViewCreateInfo info = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = image,
        .viewType         = type,
        .format           = format,
        .subresourceRange = { .aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = base_layer, .layerCount = layers },
    };
    VkImageView view = VK_NULL_HANDLE;
    VK_CHECK(device->vk.vkCreateImageView(device->device, &info, nullptr, &view));
    return view;
}

// Vulkan only allows views of images with a sampled, storage or attachment usage
static b8 create_views (const R_VkDevice* device, R_VkImage& image)
{
    if (!image.has_views)
    {
        return true;
    }
    const VkFormat format = vk_format(image.format);
    image.array_view      = create_view(device, image.image, format, image.aspect, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, image.layers);
    for (u32 i = 0; i < image.layers && i < R_MAX_LAYER_VIEWS; i++)
    {
        image.layer_views[i] = create_view(device, image.image, format, image.aspect, VK_IMAGE_VIEW_TYPE_2D, i, 1);
    }
    return image.array_view != VK_NULL_HANDLE;
}

static VkImageUsageFlags image_usage (u32 usage_flags)
{
    VkImageUsageFlags usage = 0;
    if (usage_flags & (u32)R_ImageUsageFlags::TransferSrc)
    {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (usage_flags & (u32)R_ImageUsageFlags::TransferDst)
    {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (usage_flags & (u32)R_ImageUsageFlags::Sampled)
    {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (usage_flags & (u32)R_ImageUsageFlags::ColorAttachment)
    {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (usage_flags & (u32)R_ImageUsageFlags::DepthStencilAttachment)
    {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    return usage;
}

static b8 usage_allows_views (u32 usage_flags)
{
    const u32 view_usages = (u32)R_ImageUsageFlags::Sampled | (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::DepthStencilAttachment;
    return (usage_flags & view_usages) != 0;
}

static void destroy_image_resource (const R_VkDevice* device, R_VkImage& image)
{
    for (u32 i = 0; i < R_MAX_LAYER_VIEWS; i++)
    {
        if (image.layer_views[i])
        {
            device->vk.vkDestroyImageView(device->device, image.layer_views[i], nullptr);
        }
    }
    if (image.array_view)
    {
        device->vk.vkDestroyImageView(device->device, image.array_view, nullptr);
    }
    if (image.memory)
    {
        device->vk.vkDestroyImage(device->device, image.image, nullptr);
        device->vk.vkFreeMemory(device->device, image.memory, nullptr);
    }
    image = {};
}

static void destroy_buffer_resource (const R_VkDevice* device, R_VkBuffer& buffer)
{
    if (buffer.mapped)
    {
        device->vk.vkUnmapMemory(device->device, buffer.memory);
    }
    if (buffer.buffer)
    {
        device->vk.vkDestroyBuffer(device->device, buffer.buffer, nullptr);
    }
    if (buffer.memory)
    {
        device->vk.vkFreeMemory(device->device, buffer.memory, nullptr);
    }
    buffer = {};
}

static VkShaderModule create_shader (const R_VkDevice* device, const R_ShaderCode* code)
{
    if (!code || !code->spirv || code->spirv_size == 0 || (code->spirv_size % 4) != 0)
    {
        LOG_ERROR("Shader without SPIR-V");
        return VK_NULL_HANDLE;
    }
    const usize                    size   = code->spirv_size;
    const VkShaderModuleCreateInfo info   = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = (const u32*)code->spirv };
    VkShaderModule                 module = VK_NULL_HANDLE;
    if (device->vk.vkCreateShaderModule(device->device, &info, nullptr, &module) != VK_SUCCESS)
    {
        LOG_ERROR("Failed to create a shader module of %llu bytes", (unsigned long long)size);
        return VK_NULL_HANDLE;
    }
    return module;
}

static b8 create_frame (R_VkDevice* device, R_VkFrame& frame, u32 index)
{
    const VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = device->queue_family };
    if (device->vk.vkCreateCommandPool(device->device, &pool_info, nullptr, &frame.pool) != VK_SUCCESS)
    {
        return false;
    }
    const VkCommandBufferAllocateInfo cmd_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = frame.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (device->vk.vkAllocateCommandBuffers(device->device, &cmd_info, &frame.cmd) != VK_SUCCESS)
    {
        return false;
    }
    const VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    if (device->vk.vkCreateFence(device->device, &fence_info, nullptr, &frame.fence) != VK_SUCCESS)
    {
        return false;
    }
    const VkDescriptorPoolSize size = { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = R_MAX_DESCRIPTOR_SETS * R_MAX_TEXTURES };
    const VkDescriptorPoolCreateInfo desc_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = R_MAX_DESCRIPTOR_SETS, .poolSizeCount = 1, .pPoolSizes = &size };
    if (device->vk.vkCreateDescriptorPool(device->device, &desc_info, nullptr, &frame.descriptors) != VK_SUCCESS)
    {
        return false;
    }
    frame.handle = device->cmds.add({ .cmd = frame.cmd, .frame = index });
    return true;
}

static void destroy_frame (const R_VkDevice* device, R_VkFrame& frame)
{
    if (frame.descriptors)
    {
        device->vk.vkDestroyDescriptorPool(device->device, frame.descriptors, nullptr);
    }
    if (frame.fence)
    {
        device->vk.vkDestroyFence(device->device, frame.fence, nullptr);
    }
    if (frame.pool)
    {
        device->vk.vkDestroyCommandPool(device->device, frame.pool, nullptr);
    }
    frame = {};
}

static R_VkFrame* cmd_frame (R_VkDevice* device, R_CmdBufferHandle cmd)
{
    const R_VkCmdBuffer* buffer = device->cmds.try_get(cmd);
    if (!buffer)
    {
        ASSERT_MSG(false, "invalid command buffer handle");
        return nullptr;
    }
    return &device->frames[buffer->frame];
}

static VkCommandBuffer cmd_vk (R_VkDevice* device, R_CmdBufferHandle cmd)
{
    const R_VkCmdBuffer* buffer = device->cmds.try_get(cmd);
    if (!buffer || !device->frames[buffer->frame].recording)
    {
        ASSERT_MSG(false, "command buffer is not recording");
        return VK_NULL_HANDLE;
    }
    return buffer->cmd;
}

// Views of a depth and stencil format see depth only, but without separateDepthStencilLayouts
// its layout changes must name both aspects (VUID-VkImageMemoryBarrier-image-03320)
static void transition (const R_VkDevice* device, VkCommandBuffer cmd, const R_VkImage& image, VkImageLayout old_layout, VkImageLayout new_layout)
{
    const VkImageAspectFlags aspect = r_format_has_stencil(image.format) ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : image.aspect;
    const VkImageMemoryBarrier barrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = layout_access(old_layout),
        .dstAccessMask       = layout_access(new_layout),
        .oldLayout           = old_layout,
        .newLayout           = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image.image,
        .subresourceRange    = { .aspectMask = aspect, .baseMipLevel = 0, .levelCount = image.mip_levels, .baseArrayLayer = 0, .layerCount = image.layers },
    };
    device->vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

static void begin_frame_recording (R_VkDevice* device, R_VkFrame& frame)
{
    VK_CHECK(device->vk.vkWaitForFences(device->device, 1, &frame.fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(device->vk.vkResetFences(device->device, 1, &frame.fence));
    VK_CHECK(device->vk.vkResetDescriptorPool(device->device, frame.descriptors, 0));
    const VkCommandBufferBeginInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(device->vk.vkBeginCommandBuffer(frame.cmd, &info));
    frame.recording = true;

    for (const R_ImageHandle& handle : device->pending_layouts)
    {
        R_VkImage* image = device->images.try_get(handle);
        if (image && image->pending_layout != R_ImageLayout::Undefined)
        {
            transition(device, frame.cmd, *image, VK_IMAGE_LAYOUT_UNDEFINED, vk_layout(image->pending_layout, format_is_depth(image->format)));
            image->pending_layout = R_ImageLayout::Undefined;
        }
    }
    device->pending_layouts.clear();
}

static void submit_frame (R_VkDevice* device, R_VkFrame& frame, VkSemaphore wait, VkSemaphore signal)
{
    VK_CHECK(device->vk.vkEndCommandBuffer(frame.cmd));
    frame.recording = false;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo info = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = wait ? 1u : 0u,
        .pWaitSemaphores      = &wait,
        .pWaitDstStageMask    = &wait_stage,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &frame.cmd,
        .signalSemaphoreCount = signal ? 1u : 0u,
        .pSignalSemaphores    = &signal,
    };
    VK_CHECK(device->vk.vkQueueSubmit(device->queue, 1, &info, frame.fence));
    device->queue_accesses++;
    frame.submits++;
}

// Waits for every submitted command buffer of puppetxr's own; a recording one is skipped since its fence is unsignaled until submit
static void wait_submitted (const R_VkDevice* device)
{
    VkFence fences[R_MAX_FRAMES_IN_FLIGHT + 1];
    u32     count = 0;
    for (u32 i = 0; i < ARRAY_SIZE(device->frames); i++)
    {
        if (device->frames[i].fence && !device->frames[i].recording)
        {
            fences[count++] = device->frames[i].fence;
        }
    }
    if (count > 0)
    {
        VK_CHECK(device->vk.vkWaitForFences(device->device, count, fences, VK_TRUE, UINT64_MAX));
    }
}

static void destroy_window_resource (R_VkDevice* device, R_VkWindowSwapchain& window)
{
    for (u32 i = 0; i < window.images_count; i++)
    {
        R_VkImage* image = device->images.try_get(window.images[i]);
        if (image)
        {
            destroy_image_resource(device, *image);
            device->images.remove(window.images[i]);
        }
    }
    for (u32 i = 0; i < R_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (window.acquire_semaphores[i])
        {
            device->vk.vkDestroySemaphore(device->device, window.acquire_semaphores[i], nullptr);
        }
    }
    for (u32 i = 0; i < R_MAX_WINDOW_IMAGES; i++)
    {
        if (window.present_semaphores[i])
        {
            device->vk.vkDestroySemaphore(device->device, window.present_semaphores[i], nullptr);
        }
    }
    if (window.swapchain)
    {
        device->vk.vkDestroySwapchainKHR(device->device, window.swapchain, nullptr);
    }
    window = {};
}


//
// Device
//

static void vk_destroy_device (R_Device* base_device)
{
    R_VkDevice* device = vk_device(base_device);
    if (!device)
    {
        return;
    }
    if (device->device)
    {
        wait_submitted(device);
        if (device->windows.length() > 0)
        {
            device->vk.vkQueueWaitIdle(device->queue);
        }
    }

    for (R_VkWindowSwapchain& window : device->windows)
    {
        destroy_window_resource(device, window);
    }
    for (const R_VkPipeline& p : device->pipelines)
    {
        device->vk.vkDestroyPipeline(device->device, p.pipeline, nullptr);
        device->vk.vkDestroyPipelineLayout(device->device, p.layout, nullptr);
    }
    for (const R_VkFramebuffer& f : device->framebuffers)
    {
        device->vk.vkDestroyFramebuffer(device->device, f.framebuffer, nullptr);
    }
    for (const R_VkRenderPass& r : device->render_passes)
    {
        device->vk.vkDestroyRenderPass(device->device, r.render_pass, nullptr);
    }
    for (R_VkImage& image : device->images)
    {
        destroy_image_resource(device, image);
    }
    for (R_VkBuffer& buffer : device->buffers)
    {
        destroy_buffer_resource(device, buffer);
    }
    for (u32 i = 0; i < ARRAY_SIZE(device->frames); i++)
    {
        destroy_frame(device, device->frames[i]);
    }
    for (u32 i = 0; i < 2; i++)
    {
        if (device->samplers[i])
        {
            device->vk.vkDestroySampler(device->device, device->samplers[i], nullptr);
        }
    }
    for (VkDescriptorSetLayout layout : device->texture_layouts)
    {
        if (layout)
        {
            device->vk.vkDestroyDescriptorSetLayout(device->device, layout, nullptr);
        }
    }

    device->pipelines.release();
    device->framebuffers.release();
    device->render_passes.release();
    device->images.release();
    device->buffers.release();
    device->cmds.release();
    device->windows.release();
    device->pending_layouts.release();
    os_memory_release(device);
}

static void vk_wait_idle (const R_Device* base_device)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    wait_submitted(device);
}

static void vk_flush_initial_layouts (R_Device* base_device)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    if (device->pending_layouts.length == 0)
    {
        return;
    }
    const R_CmdBufferHandle cmd = r_begin_immediate(&device->base);
    r_end_immediate(&device->base, cmd);
}

static u64 vk_queue_access_count (const R_Device* base_device)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    return device->queue_accesses;
}


static i64 vk_native_format (const R_Device* base_device, R_Format format)
{
    ASSERT(base_device);
    return (i64)vk_format(format);
}

static R_Format vk_format_from_native (const R_Device* base_device, i64 format)
{
    ASSERT(base_device);
    return format_from_vk(format);
}

//
// Images
//

static R_ImageHandle vk_create_image (R_Device* base_device, const R_CreateImageInfo& info)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const u32 layers = info.layers ? info.layers : 1;
    const u32 mips   = info.mip_levels ? info.mip_levels : 1;
    const b8  cube_invalid = info.cube_compatible && (info.width != info.height || layers % 6 != 0);
    if (info.width == 0 || info.height == 0 || layers > R_MAX_IMAGE_LAYERS || mips > r_max_mip_levels(info.width, info.height) || vk_format(info.format) == VK_FORMAT_UNDEFINED || cube_invalid)
    {
        LOG_ERROR("Invalid image: %ux%u, %u layers, %u mips, format %u", info.width, info.height, layers, mips, (u32)info.format);
        return NULL_HANDLE;
    }

    const VkImageUsageFlags usage = image_usage(info.usage_flags) | info.vk_extra_usage_flags;

    const b8  depth = format_is_depth(info.format);
    R_VkImage image = {
        .width      = info.width,
        .height     = info.height,
        .layers     = layers,
        .mip_levels = mips,
        .format     = info.format,
        .aspect     = depth ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT,
        .has_views = usage_allows_views(info.usage_flags),
    };

    const VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags         = (info.cube_compatible ? (VkImageCreateFlags)VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u) | info.vk_extra_create_flags,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = vk_format(info.format),
        .extent        = { info.width, info.height, 1 },
        .mipLevels     = mips,
        .arrayLayers   = layers,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (device->vk.vkCreateImage(device->device, &image_info, nullptr, &image.image) != VK_SUCCESS)
    {
        LOG_ERROR("vkCreateImage failed for %ux%u", info.width, info.height);
        return NULL_HANDLE;
    }

    VkMemoryRequirements requirements = {};
    device->vk.vkGetImageMemoryRequirements(device->device, image.image, &requirements);
    const VkMemoryAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = find_memory_type(device, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (alloc_info.memoryTypeIndex == 0xFFFFFFFFu || device->vk.vkAllocateMemory(device->device, &alloc_info, nullptr, &image.memory) != VK_SUCCESS)
    {
        LOG_ERROR("Failed to allocate %llu bytes of image memory", (unsigned long long)requirements.size);
        device->vk.vkDestroyImage(device->device, image.image, nullptr);
        return NULL_HANDLE;
    }
    VK_CHECK(device->vk.vkBindImageMemory(device->device, image.image, image.memory, 0));

    if (!create_views(device, image))
    {
        destroy_image_resource(device, image);
        return NULL_HANDLE;
    }

    image.pending_layout       = info.initial_layout;
    const R_ImageHandle handle = device->images.add(image);
    if (info.initial_layout != R_ImageLayout::Undefined)
    {
        device->pending_layouts.add(handle);
    }
    return handle;
}

static b8 vk_image_flags_supported (const R_Device* base_device, const R_CreateImageInfo& info)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const VkImageUsageFlags  usage      = image_usage(info.usage_flags) | info.vk_extra_usage_flags;
    const VkImageCreateFlags flags      = (info.cube_compatible ? (VkImageCreateFlags)VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u) | info.vk_extra_create_flags;
    VkImageFormatProperties  properties = {};
    return vkGetPhysicalDeviceImageFormatProperties(device->physical_device, vk_format(info.format), VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags, &properties) == VK_SUCCESS;
}

static R_ImageHandle vk_import_image (R_Device* base_device, const R_ImportImageInfo& info)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    if (!info.native_image || info.width == 0 || info.height == 0 || vk_format(info.format) == VK_FORMAT_UNDEFINED)
    {
        LOG_ERROR("Invalid imported image");
        return NULL_HANDLE;
    }
    R_VkImage image = {
        .image  = (VkImage)info.native_image,
        .width      = info.width,
        .height     = info.height,
        .layers     = 1,
        .mip_levels = 1,
        .format     = info.format,
        .aspect = format_is_depth(info.format) ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT,
        .has_views = true,
    };
    if (!create_views(device, image))
    {
        destroy_image_resource(device, image);
        return NULL_HANDLE;
    }
    return device->images.add(image);
}


static void vk_destroy_image (R_Device* base_device, R_ImageHandle handle)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    R_VkImage* image = device->images.try_get(handle);
    if (!image)
    {
        return;
    }
    wait_submitted(device);
    destroy_image_resource(device, *image);
    device->images.remove(handle);
}

static b8 vk_is_valid_image (const R_Device* base_device, R_ImageHandle image)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    return device->images.is_valid(image);
}

static u64 vk_get_image_native (const R_Device* base_device, R_ImageHandle handle)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkImage* image = device->images.try_get(handle);
    return image ? (u64)image->image : 0;
}

static void vk_get_image_size (const R_Device* base_device, R_ImageHandle handle, u32& out_width, u32& out_height, u32& out_layers)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkImage* image = device->images.try_get(handle);
    out_width  = image ? image->width : 0;
    out_height = image ? image->height : 0;
    out_layers = image ? image->layers : 0;
}

static R_Format vk_get_image_format (const R_Device* base_device, R_ImageHandle handle)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkImage* image = device->images.try_get(handle);
    return image ? image->format : R_Format::Unknown;
}

//
// Buffers
//

static R_BufferHandle vk_create_buffer (R_Device* base_device, const R_CreateBufferInfo& info)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    if (info.size == 0)
    {
        LOG_ERROR("Invalid buffer size 0");
        return NULL_HANDLE;
    }

    VkBufferUsageFlags usage = 0;
    if (info.usage_flags & (u32)R_BufferUsageFlags::TransferSrc)
    {
        usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (info.usage_flags & (u32)R_BufferUsageFlags::TransferDst)
    {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (info.usage_flags & (u32)R_BufferUsageFlags::Vertex)
    {
        usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (info.usage_flags & (u32)R_BufferUsageFlags::Index)
    {
        usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (info.usage_flags & (u32)R_BufferUsageFlags::Uniform)
    {
        usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }

    R_VkBuffer               buffer      = { .size = info.size };
    const VkBufferCreateInfo buffer_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = info.size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (device->vk.vkCreateBuffer(device->device, &buffer_info, nullptr, &buffer.buffer) != VK_SUCCESS)
    {
        LOG_ERROR("vkCreateBuffer failed for %llu bytes", (unsigned long long)info.size);
        return NULL_HANDLE;
    }

    VkMemoryRequirements requirements = {};
    device->vk.vkGetBufferMemoryRequirements(device->device, buffer.buffer, &requirements);
    const VkMemoryPropertyFlags properties = info.host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkMemoryAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = find_memory_type(device, requirements.memoryTypeBits, properties) };
    if (alloc_info.memoryTypeIndex == 0xFFFFFFFFu || device->vk.vkAllocateMemory(device->device, &alloc_info, nullptr, &buffer.memory) != VK_SUCCESS)
    {
        LOG_ERROR("Failed to allocate %llu bytes of buffer memory", (unsigned long long)requirements.size);
        destroy_buffer_resource(device, buffer);
        return NULL_HANDLE;
    }
    VK_CHECK(device->vk.vkBindBufferMemory(device->device, buffer.buffer, buffer.memory, 0));

    if (info.host_visible && device->vk.vkMapMemory(device->device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped) != VK_SUCCESS)
    {
        LOG_ERROR("vkMapMemory failed");
        destroy_buffer_resource(device, buffer);
        return NULL_HANDLE;
    }
    return device->buffers.add(buffer);
}

static void vk_destroy_buffer (R_Device* base_device, R_BufferHandle handle)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    R_VkBuffer* buffer = device->buffers.try_get(handle);
    if (!buffer)
    {
        return;
    }
    wait_submitted(device);
    destroy_buffer_resource(device, *buffer);
    device->buffers.remove(handle);
}

static b8 vk_is_valid_buffer (const R_Device* base_device, R_BufferHandle buffer)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    return device->buffers.is_valid(buffer);
}

static void* vk_get_buffer_mapped_data (const R_Device* base_device, R_BufferHandle handle)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkBuffer* buffer = device->buffers.try_get(handle);
    return buffer ? buffer->mapped : nullptr;
}

// Host visible memory is coherent
static void vk_flush_buffer (R_Device* base_device, R_BufferHandle handle, u64 offset, u64 size)
{
    const R_VkDevice* device = vk_device(base_device);
    const R_VkBuffer* buffer = device->buffers.try_get(handle);
    if (!buffer || !buffer->mapped || offset > buffer->size || size > buffer->size - offset)
    {
        ASSERT_MSG(false, "invalid buffer flush");
    }
}

//
// Render passes and framebuffers
//

static VkAttachmentLoadOp vk_load_op (R_LoadOp load_op)
{
    switch (load_op)
    {
    case R_LoadOp::DontCare:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    case R_LoadOp::Load:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case R_LoadOp::Clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

static R_RenderPassHandle vk_create_render_pass (R_Device* base_device, const R_CreateRenderPassInfo& info)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const b8 has_color = info.color_format != R_Format::Unknown;
    const b8 has_depth = info.depth_format != R_Format::Unknown;
    if ((!has_color && !has_depth) || (has_color && (vk_format(info.color_format) == VK_FORMAT_UNDEFINED || format_is_depth(info.color_format))) || (has_depth && !format_is_depth(info.depth_format)))
    {
        LOG_ERROR("Invalid render pass formats: color %u, depth %u", (u32)info.color_format, (u32)info.depth_format);
        return NULL_HANDLE;
    }

    VkAttachmentDescription attachments[2] = {};
    u32                     attachments_count = 0;
    VkAttachmentReference   color_reference   = {};
    VkAttachmentReference   depth_reference   = {};
    if (has_color)
    {
        color_reference                   = { .attachment = attachments_count, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        attachments[attachments_count++] = {
            .format         = vk_format(info.color_format),
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = vk_load_op(info.load_op),
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = vk_layout(info.initial_layout, false),
            .finalLayout    = vk_layout(info.final_layout, false),
        };
    }
    if (has_depth)
    {
        depth_reference                   = { .attachment = attachments_count, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        attachments[attachments_count++] = {
            .format         = vk_format(info.depth_format),
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = vk_load_op(info.depth_load_op),
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = vk_layout(info.depth_initial_layout, true),
            .finalLayout    = vk_layout(info.depth_final_layout, true),
        };
    }

    const VkPipelineStageFlags  stages   = (has_color ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : 0) | (has_depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT : 0);
    const VkAccessFlags         accesses = (has_color ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0) | (has_depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0);
    const VkSubpassDescription  subpass  = {
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount    = has_color ? 1u : 0u,
        .pColorAttachments       = has_color ? &color_reference : nullptr,
        .pDepthStencilAttachment = has_depth ? &depth_reference : nullptr,
    };
    const VkSubpassDependency dependencies[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, .dstStageMask = stages, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, .dstAccessMask = accesses },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = stages, .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, .srcAccessMask = accesses, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT },
    };
    const VkRenderPassCreateInfo pass_info = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = attachments_count, .pAttachments = attachments, .subpassCount = 1, .pSubpasses = &subpass, .dependencyCount = 2, .pDependencies = dependencies };

    R_VkRenderPass pass = { .format = info.color_format, .depth_format = info.depth_format };
    if (device->vk.vkCreateRenderPass(device->device, &pass_info, nullptr, &pass.render_pass) != VK_SUCCESS)
    {
        LOG_ERROR("vkCreateRenderPass failed");
        return NULL_HANDLE;
    }
    return device->render_passes.add(pass);
}

static void vk_destroy_render_pass (R_Device* base_device, R_RenderPassHandle handle)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkRenderPass* pass = device->render_passes.try_get(handle);
    if (!pass)
    {
        return;
    }
    wait_submitted(device);
    device->vk.vkDestroyRenderPass(device->device, pass->render_pass, nullptr);
    device->render_passes.remove(handle);
}

static b8 vk_is_valid_render_pass (const R_Device* base_device, R_RenderPassHandle render_pass)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    return device->render_passes.is_valid(render_pass);
}

static R_FramebufferHandle vk_create_framebuffer (R_Device* base_device, const R_CreateFramebufferInfo& info)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkRenderPass* pass = device->render_passes.try_get(info.render_pass);
    if (!pass)
    {
        LOG_ERROR("Invalid framebuffer render pass");
        return NULL_HANDLE;
    }

    VkImageView views[2]    = {}; // NOLINT(misc-const-correctness): false positive, the handles are assigned below
    u32         views_count = 0;
    u32         width       = 0;
    u32         height      = 0;
    const struct
    {
        R_Format      format = {};
        R_ImageHandle handle;
        u32           layer = {};
    } attachments[2] = { { pass->format, info.image, info.layer }, { pass->depth_format, info.depth_image, info.depth_layer } };
    for (const auto& attachment : attachments)
    {
        const R_VkImage* image = device->images.try_get(attachment.handle);
        if (attachment.format == R_Format::Unknown)
        {
            if (image)
            {
                LOG_ERROR("Framebuffer image given for an attachment the render pass lacks");
                return NULL_HANDLE;
            }
            continue;
        }
        if (!image || attachment.layer >= image->layers || attachment.layer >= R_MAX_LAYER_VIEWS || !image->has_views)
        {
            LOG_ERROR("Invalid framebuffer image or layer %u", attachment.layer);
            return NULL_HANDLE;
        }
        if (image->format != attachment.format)
        {
            LOG_ERROR("Framebuffer image format %u does not match render pass format %u", (u32)image->format, (u32)attachment.format);
            return NULL_HANDLE;
        }
        if (views_count > 0 && (image->width != width || image->height != height))
        {
            LOG_ERROR("Framebuffer attachments differ in size");
            return NULL_HANDLE;
        }
        width                = image->width;
        height               = image->height;
        views[views_count++] = image->layer_views[attachment.layer];
    }

    const VkFramebufferCreateInfo fb_info = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass->render_pass, .attachmentCount = views_count, .pAttachments = views, .width = width, .height = height, .layers = 1 };
    R_VkFramebuffer               fb      = { .width = width, .height = height };
    if (device->vk.vkCreateFramebuffer(device->device, &fb_info, nullptr, &fb.framebuffer) != VK_SUCCESS)
    {
        LOG_ERROR("vkCreateFramebuffer failed");
        return NULL_HANDLE;
    }
    return device->framebuffers.add(fb);
}

static void vk_destroy_framebuffer (R_Device* base_device, R_FramebufferHandle handle)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkFramebuffer* fb = device->framebuffers.try_get(handle);
    if (!fb)
    {
        return;
    }
    wait_submitted(device);
    device->vk.vkDestroyFramebuffer(device->device, fb->framebuffer, nullptr);
    device->framebuffers.remove(handle);
}

static b8 vk_is_valid_framebuffer (const R_Device* base_device, R_FramebufferHandle framebuffer)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    return device->framebuffers.is_valid(framebuffer);
}

//
// Pipelines
//

static R_PipelineHandle vk_create_pipeline (R_Device* base_device, const R_CreatePipelineInfo& info)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkRenderPass* pass = device->render_passes.try_get(info.render_pass);
    if (!pass || info.push_constants_size > R_MAX_PUSH_CONSTANTS || info.vertex_attributes_count > R_MAX_VERTEX_ATTRIBUTES || info.textures_count > R_MAX_TEXTURES ||
        ((info.depth_test || info.depth_write) && pass->depth_format == R_Format::Unknown))
    {
        LOG_ERROR("Invalid pipeline info");
        return NULL_HANDLE;
    }

    const VkShaderModule vertex   = create_shader(device, info.vertex);
    const VkShaderModule fragment = create_shader(device, info.fragment);
    defer {
        if (vertex)
        {
            device->vk.vkDestroyShaderModule(device->device, vertex, nullptr);
        }
        if (fragment)
        {
            device->vk.vkDestroyShaderModule(device->device, fragment, nullptr);
        }
    };
    if (!vertex || !fragment)
    {
        return NULL_HANDLE;
    }

    R_VkPipeline pipeline = { .textures_count = info.textures_count, .texture_type = info.texture_type, .push_constants_size = info.push_constants_size };

    const VkPushConstantRange push_range = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = info.push_constants_size };
    const VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = info.textures_count > 0 ? 1u : 0u,
        .pSetLayouts            = info.textures_count > 0 ? &device->texture_layouts[info.textures_count - 1] : nullptr,
        .pushConstantRangeCount = info.push_constants_size ? 1u : 0u,
        .pPushConstantRanges    = &push_range,
    };
    if (device->vk.vkCreatePipelineLayout(device->device, &layout_info, nullptr, &pipeline.layout) != VK_SUCCESS)
    {
        LOG_ERROR("vkCreatePipelineLayout failed");
        return NULL_HANDLE;
    }

    const VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main" },
    };

    const VkVertexInputBindingDescription binding = { .binding = 0, .stride = info.vertex_stride, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription     attributes[R_MAX_VERTEX_ATTRIBUTES] = {};
    for (u32 i = 0; i < info.vertex_attributes_count; i++)
    {
        attributes[i] = { .location = info.vertex_attributes[i].location, .binding = 0, .format = vk_format(info.vertex_attributes[i].format), .offset = info.vertex_attributes[i].offset };
    }
    const VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = info.vertex_stride ? 1u : 0u,
        .pVertexBindingDescriptions      = &binding,
        .vertexAttributeDescriptionCount = info.vertex_stride ? info.vertex_attributes_count : 0u,
        .pVertexAttributeDescriptions    = attributes,
    };
    const VkPipelineInputAssemblyStateCreateInfo input_assembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    const VkPipelineViewportStateCreateInfo      viewport       = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    const VkPipelineRasterizationStateCreateInfo raster         = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    const VkPipelineMultisampleStateCreateInfo   multisample    = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    R_BlendFactor factors[4];
    r_pipeline_blend_factors(info, factors);
    const VkPipelineColorBlendAttachmentState    blend_attachment = {
        .blendEnable         = info.blend_enable ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = vk_blend_factor(factors[0]),
        .dstColorBlendFactor = vk_blend_factor(factors[1]),
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = vk_blend_factor(factors[2]),
        .dstAlphaBlendFactor = vk_blend_factor(factors[3]),
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      = info.no_color_write ? 0u : (VkColorComponentFlags)(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT),
    };
    const VkPipelineColorBlendStateCreateInfo blend = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = pass->format != R_Format::Unknown ? 1u : 0u, .pAttachments = &blend_attachment };
    const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = info.depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = info.depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp   = vk_compare_op(info.depth_compare),
        .maxDepthBounds   = 1.0f,
    };
    const VkDynamicState                      dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    const VkPipelineDynamicStateCreateInfo    dynamic = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dynamic_states };

    const VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState      = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState   = &multisample,
        .pDepthStencilState  = pass->depth_format != R_Format::Unknown ? &depth_stencil : nullptr,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dynamic,
        .layout              = pipeline.layout,
        .renderPass          = pass->render_pass,
        .subpass             = 0,
    };
    if (device->vk.vkCreateGraphicsPipelines(device->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline.pipeline) != VK_SUCCESS)
    {
        LOG_ERROR("vkCreateGraphicsPipelines failed");
        device->vk.vkDestroyPipelineLayout(device->device, pipeline.layout, nullptr);
        return NULL_HANDLE;
    }
    return device->pipelines.add(pipeline);
}

static void vk_destroy_pipeline (R_Device* base_device, R_PipelineHandle handle)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkPipeline* pipeline = device->pipelines.try_get(handle);
    if (!pipeline)
    {
        return;
    }
    wait_submitted(device);
    device->vk.vkDestroyPipeline(device->device, pipeline->pipeline, nullptr);
    device->vk.vkDestroyPipelineLayout(device->device, pipeline->layout, nullptr);
    device->pipelines.remove(handle);
}

static b8 vk_is_valid_pipeline (const R_Device* base_device, R_PipelineHandle pipeline)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    return device->pipelines.is_valid(pipeline);
}

//
// Window presentation
//

static b8 vk_device_supports_present (const R_Device* base_device, const R_WindowTarget& target)
{
    const R_VkDevice*  device  = vk_device(base_device);
    const VkSurfaceKHR surface = (VkSurfaceKHR)target.vk_surface;
    ASSERT(device);
    if (!surface || !vkGetPhysicalDeviceSurfaceSupportKHR || !device->vk.vkCreateSwapchainKHR)
    {
        return false;
    }
    VkBool32 supported = VK_FALSE;
    return vkGetPhysicalDeviceSurfaceSupportKHR(device->physical_device, device->queue_family, surface, &supported) == VK_SUCCESS && supported;
}

static R_WindowSwapchainHandle vk_create_window_swapchain (R_Device* base_device, const R_WindowTarget& target, u32 width, u32 height)
{
    R_VkDevice*        device  = vk_device(base_device);
    const VkSurfaceKHR surface = (VkSurfaceKHR)target.vk_surface;
    ASSERT(device);
    if (!r_device_supports_present(&device->base, target) || width == 0 || height == 0)
    {
        LOG_ERROR("Cannot present to this surface");
        return NULL_HANDLE;
    }

    VkSurfaceCapabilitiesKHR capabilities = {};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical_device, surface, &capabilities));
    VkSurfaceFormatKHR formats[64]    = {};
    u32                formats_count  = 64;
    const VkResult     formats_result = vkGetPhysicalDeviceSurfaceFormatsKHR(device->physical_device, surface, &formats_count, formats);
    if ((formats_result != VK_SUCCESS && formats_result != VK_INCOMPLETE) || formats_count == 0)
    {
        LOG_ERROR("vkGetPhysicalDeviceSurfaceFormatsKHR failed with %d", (i32)formats_result);
        return NULL_HANDLE;
    }

    VkSurfaceFormatKHR chosen = formats[0];
    for (u32 i = 0; i < formats_count; i++)
    {
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_SRGB || formats[i].format == VK_FORMAT_R8G8B8A8_SRGB) && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosen = formats[i];
            break;
        }
    }
    const R_Format format = format_from_vk((i64)chosen.format);
    if (format == R_Format::Unknown)
    {
        LOG_ERROR("No usable surface format");
        return NULL_HANDLE;
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0xFFFFFFFFu)
    {
        extent = { zm::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width), zm::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height) };
    }
    u32 image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount)
    {
        image_count = capabilities.maxImageCount;
    }
    image_count = zm::min(image_count, (u32)R_MAX_WINDOW_IMAGES);

    // The mirror never waits for the display: with FIFO, MoltenVK waits in vkQueueSubmit for a
    // drawable, which paced the app's frames to the display
    VkPresentModeKHR modes[16]    = {};
    u32              modes_count  = 16;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(device->physical_device, surface, &modes_count, modes) >= VK_SUCCESS)
    {
        for (u32 i = 0; i < modes_count; i++)
        {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR || (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && present_mode == VK_PRESENT_MODE_FIFO_KHR))
            {
                present_mode = modes[i];
            }
        }
    }

    const VkSwapchainCreateInfoKHR info = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = surface,
        .minImageCount    = image_count,
        .imageFormat      = chosen.format,
        .imageColorSpace  = chosen.colorSpace,
        .imageExtent      = extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = capabilities.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = present_mode,
        .clipped          = VK_TRUE,
    };
    R_VkWindowSwapchain window = { .surface = surface, .width = extent.width, .height = extent.height, .format = format };
    if (device->vk.vkCreateSwapchainKHR(device->device, &info, nullptr, &window.swapchain) != VK_SUCCESS)
    {
        LOG_ERROR("vkCreateSwapchainKHR failed");
        return NULL_HANDLE;
    }

    VkImage vk_images[R_MAX_WINDOW_IMAGES] = {};
    window.images_count = R_MAX_WINDOW_IMAGES;
    if (device->vk.vkGetSwapchainImagesKHR(device->device, window.swapchain, &window.images_count, vk_images) < VK_SUCCESS)
    {
        destroy_window_resource(device, window);
        return NULL_HANDLE;
    }
    for (u32 i = 0; i < window.images_count; i++)
    {
        window.images[i] = r_import_image(&device->base, { .native_image = (u64)vk_images[i], .width = extent.width, .height = extent.height, .format = format });
    }

    const VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < R_MAX_FRAMES_IN_FLIGHT; i++)
    {
        VK_CHECK(device->vk.vkCreateSemaphore(device->device, &semaphore_info, nullptr, &window.acquire_semaphores[i]));
    }
    for (u32 i = 0; i < window.images_count; i++)
    {
        VK_CHECK(device->vk.vkCreateSemaphore(device->device, &semaphore_info, nullptr, &window.present_semaphores[i]));
    }
    return device->windows.add(window);
}

static void vk_destroy_window_swapchain (R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    R_VkWindowSwapchain* window = device->windows.try_get(swapchain);
    if (!window)
    {
        return;
    }
    // Presents are not fenced, so their semaphores need the queue drained. Window swapchains
    // exist only in windowed mode and are destroyed on resize inside xrEndFrame or with the session.
    device->vk.vkQueueWaitIdle(device->queue);
    destroy_window_resource(device, *window);
    device->windows.remove(swapchain);
}

static b8 vk_is_valid_window_swapchain (const R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    return device->windows.is_valid(swapchain);
}

static R_Format vk_window_swapchain_format (const R_Device* base_device, R_WindowSwapchainHandle swapchain)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkWindowSwapchain* window = device->windows.try_get(swapchain);
    return window ? window->format : R_Format::Unknown;
}

static void vk_window_swapchain_size (const R_Device* base_device, R_WindowSwapchainHandle swapchain, u32& out_width, u32& out_height)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkWindowSwapchain* window = device->windows.try_get(swapchain);
    out_width  = window ? window->width : 0;
    out_height = window ? window->height : 0;
}

static b8 vk_window_acquire (R_Device* base_device, R_WindowSwapchainHandle handle, R_ImageHandle& out_image)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    out_image = NULL_HANDLE;
    R_VkWindowSwapchain* window = device->windows.try_get(handle);
    if (!window || window->acquired)
    {
        ASSERT_MSG(false, "invalid window swapchain or image already acquired");
        return false;
    }

    // The frame fence of the slot we are about to record guards the acquire semaphore reuse
    const R_VkFrame& frame = device->frames[device->frame_index];
    VK_CHECK(device->vk.vkWaitForFences(device->device, 1, &frame.fence, VK_TRUE, UINT64_MAX));

    window->acquire_index = device->frame_index;
    const VkResult result = device->vk.vkAcquireNextImageKHR(device->device, window->swapchain, UINT64_MAX, window->acquire_semaphores[window->acquire_index], VK_NULL_HANDLE, &window->current_image);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        return false;
    }
    if (result != VK_SUCCESS)
    {
        LOG_ERROR("vkAcquireNextImageKHR failed with %d", (i32)result);
        return false;
    }
    window->acquired = true;
    out_image        = window->images[window->current_image];
    return true;
}

static b8 vk_window_present (R_Device* base_device, R_WindowSwapchainHandle handle)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    R_VkWindowSwapchain* window = device->windows.try_get(handle);
    if (!window || !window->acquired)
    {
        ASSERT_MSG(false, "no acquired window image to present");
        return false;
    }
    window->acquired = false;
    const VkPresentInfoKHR info = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &window->present_semaphores[window->current_image],
        .swapchainCount     = 1,
        .pSwapchains        = &window->swapchain,
        .pImageIndices      = &window->current_image,
    };
    const VkResult result = device->vk.vkQueuePresentKHR(device->queue, &info);
    device->queue_accesses++;
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

//
// Frames and immediate commands
//

static R_CmdBufferHandle vk_begin_frame (R_Device* base_device)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    R_VkFrame& frame = device->frames[device->frame_index];
    ASSERT_MSG(!frame.recording, "previous frame was not submitted");
    begin_frame_recording(device, frame);
    return frame.handle;
}

static void vk_end_frame (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    R_VkFrame* frame = cmd_frame(device, cmd);
    if (!frame || !frame->recording)
    {
        return;
    }
    submit_frame(device, *frame, VK_NULL_HANDLE, VK_NULL_HANDLE);
    device->frame_index = (device->frame_index + 1) % R_MAX_FRAMES_IN_FLIGHT;
}

static b8 vk_window_busy (const R_Device* base_device, R_WindowSwapchainHandle handle)
{
    const R_VkDevice*          device = vk_device(base_device);
    const R_VkWindowSwapchain* window = device->windows.try_get(handle);
    if (!window || window->presented_frame >= R_MAX_FRAMES_IN_FLIGHT)
    {
        return false;
    }
    // A slot is begun again only once its fence signaled, so a later submit means the present is done
    const R_VkFrame& frame = device->frames[window->presented_frame];
    return !frame.recording && frame.submits == window->presented_submit && device->vk.vkGetFenceStatus(device->device, frame.fence) == VK_NOT_READY;
}

static void vk_end_frame_present (R_Device* base_device, R_CmdBufferHandle cmd, R_WindowSwapchainHandle swapchain)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    R_VkFrame*                 frame  = cmd_frame(device, cmd);
    const R_VkWindowSwapchain* window = device->windows.try_get(swapchain);
    if (!frame || !frame->recording)
    {
        return;
    }
    if (!window || !window->acquired)
    {
        ASSERT_MSG(false, "no acquired window image");
        submit_frame(device, *frame, VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    else
    {
        submit_frame(device, *frame, window->acquire_semaphores[window->acquire_index], window->present_semaphores[window->current_image]);
        R_VkWindowSwapchain& presented = device->windows.get(swapchain);
        presented.presented_frame      = (u32)(frame - device->frames);
        presented.presented_submit     = frame->submits;
    }
    device->frame_index = (device->frame_index + 1) % R_MAX_FRAMES_IN_FLIGHT;
}

static R_CmdBufferHandle vk_begin_immediate (R_Device* base_device)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    R_VkFrame& frame = device->frames[R_MAX_FRAMES_IN_FLIGHT];
    ASSERT_MSG(!frame.recording, "immediate commands already recording");
    begin_frame_recording(device, frame);
    return frame.handle;
}

static void vk_end_immediate (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    R_VkFrame* frame = cmd_frame(device, cmd);
    if (!frame || !frame->recording)
    {
        return;
    }
    submit_frame(device, *frame, VK_NULL_HANDLE, VK_NULL_HANDLE);
    VK_CHECK(device->vk.vkWaitForFences(device->device, 1, &frame->fence, VK_TRUE, UINT64_MAX));
}

static u64 vk_get_cmd_native (const R_Device* base_device, R_CmdBufferHandle cmd)
{
    const R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkCmdBuffer* buffer = device->cmds.try_get(cmd);
    return buffer ? (u64)buffer->cmd : 0;
}

//
// Commands
//

static void vk_cmd_transition_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle handle, R_ImageLayout old_layout, R_ImageLayout new_layout)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    const R_VkImage*      image  = device->images.try_get(handle);
    if (!vk_cmd || !image)
    {
        return;
    }
    const b8 depth = format_is_depth(image->format);
    transition(device, vk_cmd, *image, vk_layout(old_layout, depth), vk_layout(new_layout, depth));
}

static void vk_cmd_begin_render_pass (R_Device* base_device, R_CmdBufferHandle cmd, R_RenderPassHandle pass_handle, R_FramebufferHandle fb_handle, const R_ClearColor& clear)
{
    R_VkDevice*            device = vk_device(base_device);
    const VkCommandBuffer  vk_cmd = cmd_vk(device, cmd);
    const R_VkRenderPass*  pass   = device->render_passes.try_get(pass_handle);
    const R_VkFramebuffer* fb     = device->framebuffers.try_get(fb_handle);
    if (!vk_cmd || !pass || !fb)
    {
        ASSERT_MSG(false, "invalid render pass or framebuffer");
        return;
    }
    VkClearValue clear_values[2] = {};
    u32          clear_count     = 0;
    if (pass->format != R_Format::Unknown)
    {
        clear_values[clear_count++].color = { { clear.r, clear.g, clear.b, clear.a } };
    }
    if (pass->depth_format != R_Format::Unknown)
    {
        clear_values[clear_count++].depthStencil = { 1.0f, 0 };
    }
    const VkRenderPassBeginInfo info = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass->render_pass, .framebuffer = fb->framebuffer, .renderArea = { { 0, 0 }, { fb->width, fb->height } }, .clearValueCount = clear_count, .pClearValues = clear_values };
    device->vk.vkCmdBeginRenderPass(vk_cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
}

static void vk_cmd_end_render_pass (R_Device* base_device, R_CmdBufferHandle cmd)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    if (vk_cmd)
    {
        device->vk.vkCmdEndRenderPass(vk_cmd);
    }
}

static void vk_cmd_bind_pipeline (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle handle)
{
    R_VkDevice*           device   = vk_device(base_device);
    const VkCommandBuffer vk_cmd   = cmd_vk(device, cmd);
    const R_VkPipeline*   pipeline = device->pipelines.try_get(handle);
    if (!vk_cmd || !pipeline)
    {
        ASSERT_MSG(false, "invalid pipeline");
        return;
    }
    device->vk.vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
}


static void vk_cmd_bind_textures (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle pipeline_handle, const R_ImageHandle* images, u32 count, R_Filter filter)
{
    R_VkDevice*           device   = vk_device(base_device);
    const VkCommandBuffer vk_cmd   = cmd_vk(device, cmd);
    const R_VkPipeline*   pipeline = device->pipelines.try_get(pipeline_handle);
    const R_VkFrame*      frame    = cmd_frame(device, cmd);
    if (!vk_cmd || !pipeline || !frame || !images || count == 0 || count != pipeline->textures_count)
    {
        ASSERT_MSG(false, "invalid texture binding");
        return;
    }

    VkDescriptorImageInfo image_infos[R_MAX_TEXTURES] = {};
    for (u32 i = 0; i < count; i++)
    {
        const R_VkImage* image = device->images.try_get(images[i]);
        if (!image || !image->has_views)
        {
            ASSERT_MSG(false, "invalid texture binding");
            return;
        }
        image_infos[i] = {
            .sampler     = device->samplers[filter == R_Filter::Linear ? 1 : 0],
            .imageView   = pipeline->texture_type == R_TextureType::Type2DArray ? image->array_view : image->layer_views[0],
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
    }

    const VkDescriptorSetAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = frame->descriptors, .descriptorSetCount = 1, .pSetLayouts = &device->texture_layouts[count - 1] };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (device->vk.vkAllocateDescriptorSets(device->device, &alloc_info, &set) != VK_SUCCESS)
    {
        LOG_ERROR("Out of descriptor sets for this frame");
        return;
    }
    const VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = count, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = image_infos };
    device->vk.vkUpdateDescriptorSets(device->device, 1, &write, 0, nullptr);
    device->vk.vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 0, 1, &set, 0, nullptr);
}

static void vk_cmd_clear_depth (R_Device* base_device, R_CmdBufferHandle cmd, const R_Rect& rect, f32 depth)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    if (!vk_cmd)
    {
        ASSERT_MSG(false, "invalid command buffer");
        return;
    }
    const VkClearAttachment attachment = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .clearValue = { .depthStencil = { depth, 0 } } };
    const VkClearRect       clear_rect = { .rect = { { rect.x, rect.y }, { rect.width, rect.height } }, .baseArrayLayer = 0, .layerCount = 1 };
    device->vk.vkCmdClearAttachments(vk_cmd, 1, &attachment, 1, &clear_rect);
}

static void vk_cmd_push_constants (R_Device* base_device, R_CmdBufferHandle cmd, R_PipelineHandle handle, const void* data, u32 size)
{
    R_VkDevice*           device   = vk_device(base_device);
    const VkCommandBuffer vk_cmd   = cmd_vk(device, cmd);
    const R_VkPipeline*   pipeline = device->pipelines.try_get(handle);
    if (!vk_cmd || !pipeline || !data || size == 0 || size > pipeline->push_constants_size)
    {
        ASSERT_MSG(false, "invalid push constants");
        return;
    }
    device->vk.vkCmdPushConstants(vk_cmd, pipeline->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

static void vk_cmd_set_viewport (R_Device* base_device, R_CmdBufferHandle cmd, const R_Viewport& viewport)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    if (!vk_cmd)
    {
        return;
    }
    const VkViewport vk_viewport = { viewport.x, viewport.y, viewport.width, viewport.height, 0.0f, 1.0f };
    device->vk.vkCmdSetViewport(vk_cmd, 0, 1, &vk_viewport);
}

static void vk_cmd_set_scissor (R_Device* base_device, R_CmdBufferHandle cmd, const R_Rect& scissor)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    if (!vk_cmd)
    {
        return;
    }
    const VkRect2D rect = { { scissor.x, scissor.y }, { scissor.width, scissor.height } };
    device->vk.vkCmdSetScissor(vk_cmd, 0, 1, &rect);
}

static void vk_cmd_bind_vertex_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    const R_VkBuffer*     buffer = device->buffers.try_get(handle);
    if (!vk_cmd || !buffer)
    {
        ASSERT_MSG(false, "invalid vertex buffer");
        return;
    }
    device->vk.vkCmdBindVertexBuffers(vk_cmd, 0, 1, &buffer->buffer, &offset);
}

static void vk_cmd_bind_index_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle handle, u64 offset, b8 u16_indices)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    const R_VkBuffer*     buffer = device->buffers.try_get(handle);
    if (!vk_cmd || !buffer)
    {
        ASSERT_MSG(false, "invalid index buffer");
        return;
    }
    device->vk.vkCmdBindIndexBuffer(vk_cmd, buffer->buffer, offset, u16_indices ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

static void vk_cmd_draw (R_Device* base_device, R_CmdBufferHandle cmd, u32 vertex_count, u32 first_vertex)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    if (vk_cmd)
    {
        device->vk.vkCmdDraw(vk_cmd, vertex_count, 1, first_vertex, 0);
    }
}

static void vk_cmd_draw_indexed (R_Device* base_device, R_CmdBufferHandle cmd, u32 index_count, u32 first_index, i32 vertex_offset)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    if (vk_cmd)
    {
        device->vk.vkCmdDrawIndexed(vk_cmd, index_count, 1, first_index, vertex_offset, 0);
    }
}

static void vk_cmd_copy_buffer_to_image (R_Device* base_device, R_CmdBufferHandle cmd, R_BufferHandle buffer_handle, R_ImageHandle image_handle, u32 layer)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    const R_VkBuffer*     buffer = device->buffers.try_get(buffer_handle);
    const R_VkImage*      image  = device->images.try_get(image_handle);
    if (!vk_cmd || !buffer || !image || layer >= image->layers || buffer->size < (u64)image->width * image->height * r_format_bytes_per_pixel(image->format))
    {
        ASSERT_MSG(false, "invalid buffer to image copy");
        return;
    }
    const VkBufferImageCopy region = { .imageSubresource = { image->aspect, 0, layer, 1 }, .imageExtent = { image->width, image->height, 1 } };
    device->vk.vkCmdCopyBufferToImage(vk_cmd, buffer->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static void vk_cmd_copy_image_to_buffer (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle image_handle, u32 layer, R_BufferHandle buffer_handle)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    const R_VkBuffer*     buffer = device->buffers.try_get(buffer_handle);
    const R_VkImage*      image  = device->images.try_get(image_handle);
    if (!vk_cmd || !buffer || !image || layer >= image->layers || buffer->size < (u64)image->width * image->height * r_format_bytes_per_pixel(image->format))
    {
        ASSERT_MSG(false, "invalid image to buffer copy");
        return;
    }
    const VkBufferImageCopy region = { .imageSubresource = { image->aspect, 0, layer, 1 }, .imageExtent = { image->width, image->height, 1 } };
    device->vk.vkCmdCopyImageToBuffer(vk_cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->buffer, 1, &region);
}

static void vk_cmd_clear_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle handle, const R_ClearColor& color)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    const R_VkImage*      image  = device->images.try_get(handle);
    if (!vk_cmd || !image)
    {
        ASSERT_MSG(false, "invalid clear");
        return;
    }
    const VkClearColorValue       value = { { color.r, color.g, color.b, color.a } };
    const VkImageSubresourceRange range = { image->aspect, 0, 1, 0, image->layers };
    device->vk.vkCmdClearColorImage(vk_cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
}

static void vk_cmd_blit_image (R_Device* base_device, R_CmdBufferHandle cmd, R_ImageHandle src_handle, u32 src_layer, R_ImageHandle dst_handle, const R_Rect& dst_rect, R_Filter filter)
{
    R_VkDevice*           device = vk_device(base_device);
    const VkCommandBuffer vk_cmd = cmd_vk(device, cmd);
    const R_VkImage*      src    = device->images.try_get(src_handle);
    const R_VkImage*      dst    = device->images.try_get(dst_handle);
    if (!vk_cmd || !src || !dst || src_layer >= src->layers)
    {
        ASSERT_MSG(false, "invalid blit");
        return;
    }
    const VkImageBlit region = {
        .srcSubresource = { src->aspect, 0, src_layer, 1 },
        .srcOffsets     = { { 0, 0, 0 }, { (i32)src->width, (i32)src->height, 1 } },
        .dstSubresource = { dst->aspect, 0, 0, 1 },
        .dstOffsets     = { { dst_rect.x, dst_rect.y, 0 }, { dst_rect.x + (i32)dst_rect.width, dst_rect.y + (i32)dst_rect.height, 1 } },
    };
    device->vk.vkCmdBlitImage(vk_cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter == R_Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

static b8 vk_read_image (R_Device* base_device, R_ImageHandle handle, u32 layer, R_ImageLayout layout, void* out_pixels, usize out_size)
{
    R_VkDevice* device = vk_device(base_device);
    ASSERT(device);
    const R_VkImage* image = device->images.try_get(handle);
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

    const R_BufferHandle staging = r_create_buffer(&device->base, { .size = size, .usage_flags = (u32)R_BufferUsageFlags::TransferDst, .host_visible = true });
    if (!staging)
    {
        return false;
    }
    defer { r_destroy_buffer(&device->base, staging); };

    const R_CmdBufferHandle cmd = r_begin_immediate(&device->base);
    if (layout != R_ImageLayout::TransferSrc)
    {
        r_cmd_transition_image(&device->base, cmd, handle, layout, R_ImageLayout::TransferSrc);
    }
    r_cmd_copy_image_to_buffer(&device->base, cmd, handle, layer, staging);
    if (layout != R_ImageLayout::TransferSrc)
    {
        r_cmd_transition_image(&device->base, cmd, handle, R_ImageLayout::TransferSrc, layout);
    }
    r_end_immediate(&device->base, cmd);

    memory_copy(out_pixels, r_get_buffer_mapped_data(&device->base, staging), size);
    return true;
}

//
// Backend
//

const static R_BackendFns vk_fns = {
    .destroy_device            = vk_destroy_device,
    .wait_idle                 = vk_wait_idle,
    .flush_initial_layouts     = vk_flush_initial_layouts,
    .queue_access_count        = vk_queue_access_count,
    .native_format             = vk_native_format,
    .format_from_native        = vk_format_from_native,
    .create_image              = vk_create_image,
    .image_flags_supported     = vk_image_flags_supported,
    .import_image              = vk_import_image,
    .destroy_image             = vk_destroy_image,
    .is_valid_image            = vk_is_valid_image,
    .get_image_native          = vk_get_image_native,
    .get_image_size            = vk_get_image_size,
    .get_image_format          = vk_get_image_format,
    .create_buffer             = vk_create_buffer,
    .destroy_buffer            = vk_destroy_buffer,
    .is_valid_buffer           = vk_is_valid_buffer,
    .get_buffer_mapped_data    = vk_get_buffer_mapped_data,
    .flush_buffer              = vk_flush_buffer,
    .create_render_pass        = vk_create_render_pass,
    .destroy_render_pass       = vk_destroy_render_pass,
    .is_valid_render_pass      = vk_is_valid_render_pass,
    .create_framebuffer        = vk_create_framebuffer,
    .destroy_framebuffer       = vk_destroy_framebuffer,
    .is_valid_framebuffer      = vk_is_valid_framebuffer,
    .create_pipeline           = vk_create_pipeline,
    .destroy_pipeline          = vk_destroy_pipeline,
    .is_valid_pipeline         = vk_is_valid_pipeline,
    .device_supports_present   = vk_device_supports_present,
    .create_window_swapchain   = vk_create_window_swapchain,
    .destroy_window_swapchain  = vk_destroy_window_swapchain,
    .is_valid_window_swapchain = vk_is_valid_window_swapchain,
    .window_swapchain_format   = vk_window_swapchain_format,
    .window_swapchain_size     = vk_window_swapchain_size,
    .window_acquire            = vk_window_acquire,
    .window_present            = vk_window_present,
    .window_busy               = vk_window_busy,
    .begin_frame               = vk_begin_frame,
    .end_frame                 = vk_end_frame,
    .end_frame_present         = vk_end_frame_present,
    .begin_immediate           = vk_begin_immediate,
    .end_immediate             = vk_end_immediate,
    .get_cmd_native            = vk_get_cmd_native,
    .cmd_transition_image      = vk_cmd_transition_image,
    .cmd_begin_render_pass     = vk_cmd_begin_render_pass,
    .cmd_end_render_pass       = vk_cmd_end_render_pass,
    .cmd_bind_pipeline         = vk_cmd_bind_pipeline,
    .cmd_bind_textures         = vk_cmd_bind_textures,
    .cmd_clear_depth           = vk_cmd_clear_depth,
    .cmd_push_constants        = vk_cmd_push_constants,
    .cmd_set_viewport          = vk_cmd_set_viewport,
    .cmd_set_scissor           = vk_cmd_set_scissor,
    .cmd_bind_vertex_buffer    = vk_cmd_bind_vertex_buffer,
    .cmd_bind_index_buffer     = vk_cmd_bind_index_buffer,
    .cmd_draw                  = vk_cmd_draw,
    .cmd_draw_indexed          = vk_cmd_draw_indexed,
    .cmd_copy_buffer_to_image  = vk_cmd_copy_buffer_to_image,
    .cmd_copy_image_to_buffer  = vk_cmd_copy_image_to_buffer,
    .cmd_clear_image           = vk_cmd_clear_image,
    .cmd_blit_image            = vk_cmd_blit_image,
    .read_image                = vk_read_image,
};

R_Device* r_vk_create_device (const R_DeviceCreateInfo& create_info)
{
    const R_VulkanDeviceInfo& info = create_info.vulkan;
    if (!info.instance || !info.physical_device || !info.device)
    {
        LOG_ERROR("Render device needs a Vulkan instance, physical device and device");
        return nullptr;
    }

    const VkResult volk_result = info.get_instance_proc_addr ? (volkInitializeCustom((PFN_vkGetInstanceProcAddr)info.get_instance_proc_addr), VK_SUCCESS) : volkInitialize();
    if (volk_result != VK_SUCCESS)
    {
        LOG_ERROR("Vulkan loader not found");
        return nullptr;
    }
    volkLoadInstanceOnly((VkInstance)info.instance);

    R_VkDevice* device = (R_VkDevice*)os_memory_alloc(sizeof(R_VkDevice));
    device->base            = { .fns = &vk_fns, .backend = R_Backend::Vulkan };
    device->instance        = (VkInstance)info.instance;
    device->physical_device = (VkPhysicalDevice)info.physical_device;
    device->device          = (VkDevice)info.device;
    device->queue_family    = info.queue_family_index;
    volkLoadDeviceTable(&device->vk, device->device);
    device->vk.vkGetDeviceQueue(device->device, info.queue_family_index, info.queue_index, &device->queue);
    vkGetPhysicalDeviceMemoryProperties(device->physical_device, &device->memory_properties);

    b8 success = false;
    defer {
        if (!success)
        {
            r_destroy_device(&device->base);
        }
    };

    VkDescriptorSetLayoutBinding bindings[R_MAX_TEXTURES] = {};
    for (u32 i = 0; i < R_MAX_TEXTURES; i++)
    {
        bindings[i] = { .binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
        const VkDescriptorSetLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = i + 1, .pBindings = bindings };
        if (device->vk.vkCreateDescriptorSetLayout(device->device, &layout_info, nullptr, &device->texture_layouts[i]) != VK_SUCCESS)
        {
            LOG_ERROR("Failed to create the descriptor set layout");
            return nullptr;
        }
    }

    for (u32 i = 0; i < 2; i++)
    {
        const VkFilter            filter       = i == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        const VkSamplerCreateInfo sampler_info = {
            .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter    = filter,
            .minFilter    = filter,
            .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod       = 1.0f,
        };
        if (device->vk.vkCreateSampler(device->device, &sampler_info, nullptr, &device->samplers[i]) != VK_SUCCESS)
        {
            LOG_ERROR("Failed to create a sampler");
            return nullptr;
        }
    }

    for (u32 i = 0; i < ARRAY_SIZE(device->frames); i++)
    {
        if (!create_frame(device, device->frames[i], i))
        {
            LOG_ERROR("Failed to create frame resources");
            return nullptr;
        }
    }

    success = true;
    return &device->base;
}
