#include "render.h"

#include <gtest/gtest.h>
#include <volk.h>

#include "test_vulkan.h"
#include "test_gl.h"
#include "test_d3d.h"
#include "test_metal.h"

#include "composite.vert.h"
#include "composite.frag.h"
#include "passthrough.vert.h"
#include "passthrough.frag.h"
#include "environment_depth.vert.h"
#include "environment_depth.frag.h"
#include "blocks.h"

namespace
{

// Plays the role of the app: owns the Vulkan instance and device the runtime wraps
struct VulkanApp
{
    VkInstance       instance        = VK_NULL_HANDLE;
    TestValidation   validation      = {};
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice         device          = VK_NULL_HANDLE;
    u32              queue_family    = 0;

    b8 create ()
    {
        if (volkInitialize() != VK_SUCCESS)
        {
            return false;
        }
        const VkApplicationInfo app_info = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "puppetxr_tests", .apiVersion = VK_API_VERSION_1_1 };
        VkInstanceCreateInfo    instance_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app_info };
        test_validation_prepare(validation, instance_info, nullptr, 0);
        if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
        {
            return false;
        }
        volkLoadInstanceOnly(instance);
        test_validation_attach(validation, instance);

        u32 count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0)
        {
            return false;
        }
        VkPhysicalDevice devices[8] = {};
        count = count > 8 ? 8 : count;
        vkEnumeratePhysicalDevices(instance, &count, devices);
        physical_device = devices[0];

        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
        VkQueueFamilyProperties families[16] = {};
        family_count = family_count > 16 ? 16 : family_count;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families);
        queue_family = 0xFFFFFFFF;
        for (u32 i = 0; i < family_count; i++)
        {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                queue_family = i;
                break;
            }
        }
        if (queue_family == 0xFFFFFFFF)
        {
            return false;
        }

        const f32 priority = 1.0f;
        const VkDeviceQueueCreateInfo queue_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = queue_family, .queueCount = 1, .pQueuePriorities = &priority };
        const char*              device_extensions[1] = {};
        const u32                device_extensions_count = test_portability_device_extensions(physical_device, device_extensions, 0);
        const VkDeviceCreateInfo device_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info, .enabledExtensionCount = device_extensions_count, .ppEnabledExtensionNames = device_extensions };
        if (vkCreateDevice(physical_device, &device_info, nullptr, &device) != VK_SUCCESS)
        {
            return false;
        }
        volkLoadDevice(device);
        return true;
    }

    void destroy ()
    {
        if (device)
        {
            vkDestroyDevice(device, nullptr);
        }
        if (instance)
        {
            test_validation_detach(validation, instance);
            vkDestroyInstance(instance, nullptr);
        }
    }

    R_DeviceCreateInfo info () const
    {
        return { .backend = R_Backend::Vulkan, .vulkan = { .get_instance_proc_addr = (void*)vkGetInstanceProcAddr, .instance = instance, .physical_device = physical_device, .device = device, .queue_family_index = queue_family, .queue_index = 0 } };
    }
};

// Every test runs on every backend the machine has
struct RenderTest : testing::TestWithParam<R_Backend>
{
    VulkanApp  app;
    TestGLApp  gl_app;
    TestD3DApp d3d_app;
    TestMetalApp metal_app;
    R_Device*  device = nullptr;

    void SetUp () override
    {
        os_set_assert_mode(OS_AssertMode::Log);
        os_initialize(true);
        if (GetParam() == R_Backend::Vulkan)
        {
            if (!app.create())
            {
                GTEST_SKIP() << "no Vulkan device";
            }
            device = r_create_device(app.info());
        }
        else if (GetParam() == R_Backend::D3D11 || GetParam() == R_Backend::D3D12)
        {
            if (!d3d_app.create(GetParam() == R_Backend::D3D12))
            {
                GTEST_SKIP() << "no D3D device";
            }
            device = r_create_device(d3d_app.info());
        }
        else if (GetParam() == R_Backend::Metal)
        {
            if (!metal_app.create())
            {
                GTEST_SKIP() << "no Metal device";
            }
            device = r_create_device(metal_app.info());
        }
        else
        {
            if (!gl_app.create(GetParam() == R_Backend::OpenGLES))
            {
                GTEST_SKIP() << "no EGL context";
            }
            device = r_create_device(gl_app.info());
        }
        ASSERT_NE(device, nullptr);
    }

    void TearDown () override
    {
        if (device)
        {
            r_destroy_device(device);
        }
        app.destroy();
        gl_app.destroy();
        d3d_app.destroy();
        metal_app.destroy();
        EXPECT_EQ(test_validation_errors, 0u);
        EXPECT_EQ(test_gl_errors, 0u);
    }

    R_ImageHandle make_target (u32 width, u32 height, u32 layers) const
    {
        return r_create_image(device, { .width = width, .height = height, .layers = layers, .format = R_Format::R8G8B8A8_UNORM, .usage_flags = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::Sampled | (u32)R_ImageUsageFlags::TransferSrc, .initial_layout = R_ImageLayout::ColorAttachment });
    }

    void clear_layer (R_ImageHandle image, u32 layer, R_ClearColor color) const
    {
        const R_RenderPassHandle  pass = r_create_render_pass(device, { .color_format = R_Format::R8G8B8A8_UNORM, .load_op = R_LoadOp::Clear, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment });
        const R_FramebufferHandle fb   = r_create_framebuffer(device, { .render_pass = pass, .image = image, .layer = layer });
        const R_CmdBufferHandle   cmd  = r_begin_immediate(device);
        r_cmd_begin_render_pass(device, cmd, pass, fb, color);
        r_cmd_end_render_pass(device, cmd);
        r_end_immediate(device, cmd);
        r_destroy_framebuffer(device, fb);
        r_destroy_render_pass(device, pass);
    }
};

} // namespace

static void expect_pixel (const u8* pixels, u32 width, u32 x, u32 y, u8 r, u8 g, u8 b, u8 a)
{
    const u8* p = pixels + ((usize)y * width + x) * 4;
    EXPECT_EQ(p[0], r) << x << "," << y;
    EXPECT_EQ(p[1], g) << x << "," << y;
    EXPECT_EQ(p[2], b) << x << "," << y;
    EXPECT_EQ(p[3], a) << x << "," << y;
}

// The offsets std140 and HLSL constant buffers give these fields, so that one C struct feeds every backend
TEST(RenderBlockTest, push_blocks_have_the_std140_layout)
{
    EXPECT_EQ(offsetof(COMP_Push, geometry), 0u);
    EXPECT_EQ(offsetof(COMP_Push, src_rect), 64u);
    EXPECT_EQ(offsetof(COMP_Push, color_scale), 80u);
    EXPECT_EQ(offsetof(COMP_Push, color_bias), 96u);
    EXPECT_EQ(offsetof(COMP_Push, layer), 112u);
    EXPECT_EQ(offsetof(COMP_Push, flags), 116u);
    EXPECT_EQ(offsetof(COMP_Push, radius), 120u);
    EXPECT_EQ(offsetof(COMP_Push, depth_layer), 124u);
    EXPECT_EQ(sizeof(COMP_Push), 128u);

    EXPECT_EQ(offsetof(COMP_PassthroughPush, edge_color), 64u);
    EXPECT_EQ(offsetof(COMP_PassthroughPush, adjust), 80u);
    EXPECT_EQ(offsetof(COMP_PassthroughPush, color_map), 96u);
    EXPECT_EQ(offsetof(COMP_PassthroughPush, source_alpha), 100u);
    EXPECT_EQ(sizeof(COMP_PassthroughPush), 112u);

    EXPECT_EQ(sizeof(COMP_DepthPush), 64u);

    EXPECT_EQ(offsetof(UI_Push, translate), 8u);
    EXPECT_EQ(offsetof(UI_Push, srgb_target), 16u);
    EXPECT_EQ(sizeof(UI_Push), 32u);

    EXPECT_LE(sizeof(COMP_Push), (usize)R_MAX_PUSH_CONSTANTS);
}

TEST_P(RenderTest, formats)
{
    EXPECT_EQ(r_format_bytes_per_pixel(R_Format::R8G8B8A8_UNORM), 4u);
    EXPECT_EQ(r_format_bytes_per_pixel(R_Format::R16G16B16A16_SFLOAT), 8u);
    EXPECT_EQ(r_format_bytes_per_pixel(R_Format::D32_SFLOAT), 4u);
    EXPECT_TRUE(r_format_is_depth(R_Format::D16_UNORM));
    EXPECT_TRUE(r_format_is_depth(R_Format::D32_SFLOAT_S8_UINT));
    EXPECT_FALSE(r_format_is_depth(R_Format::R8G8B8A8_SRGB));
    EXPECT_FALSE(r_format_is_depth(R_Format::Count));
    EXPECT_EQ(r_format_bytes_per_pixel(R_Format::Count), 0u);
    if (GetParam() == R_Backend::Vulkan)
    {
        EXPECT_EQ(r_native_format(device, R_Format::R8G8B8A8_SRGB), (i64)VK_FORMAT_R8G8B8A8_SRGB);
        EXPECT_EQ(r_native_format(device, R_Format::D32_SFLOAT_S8_UINT), (i64)VK_FORMAT_D32_SFLOAT_S8_UINT);
        EXPECT_EQ(r_format_from_native(device, VK_FORMAT_B8G8R8A8_UNORM), R_Format::B8G8R8A8_UNORM);
        EXPECT_EQ(r_format_from_native(device, VK_FORMAT_R5G6B5_UNORM_PACK16), R_Format::Unknown);
    }
    else if (GetParam() == R_Backend::D3D11 || GetParam() == R_Backend::D3D12)
    {
        EXPECT_EQ(r_native_format(device, R_Format::R8G8B8A8_SRGB), 29);           // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        EXPECT_EQ(r_native_format(device, R_Format::D24_UNORM_S8_UINT), 45);       // DXGI_FORMAT_D24_UNORM_S8_UINT
        EXPECT_EQ(r_native_format(device, R_Format::D32_SFLOAT_S8_UINT), 20);      // DXGI_FORMAT_D32_FLOAT_S8X24_UINT
        EXPECT_EQ(r_format_from_native(device, 87), R_Format::B8G8R8A8_UNORM);     // DXGI_FORMAT_B8G8R8A8_UNORM
        EXPECT_EQ(r_format_from_native(device, 27), R_Format::Unknown);            // typeless, which apps never name
        EXPECT_EQ(r_native_format(device, R_Format::R32G32_SFLOAT), 0);            // a vertex format only
        EXPECT_EQ(r_format_from_native(device, 85), R_Format::Unknown);            // DXGI_FORMAT_B5G6R5_UNORM
    }
    else if (GetParam() == R_Backend::Metal)
    {
        EXPECT_EQ(r_native_format(device, R_Format::R8G8B8A8_SRGB), 71);           // MTLPixelFormatRGBA8Unorm_sRGB
        EXPECT_EQ(r_native_format(device, R_Format::D32_SFLOAT_S8_UINT), 260);     // MTLPixelFormatDepth32Float_Stencil8
        EXPECT_EQ(r_native_format(device, R_Format::D24_UNORM_S8_UINT), 0);        // Apple GPUs have no 24-bit depth
        EXPECT_EQ(r_format_from_native(device, 80), R_Format::B8G8R8A8_UNORM);     // MTLPixelFormatBGRA8Unorm
        EXPECT_EQ(r_format_from_native(device, 40), R_Format::Unknown);            // MTLPixelFormatB5G6R5Unorm
    }
    else
    {
        EXPECT_EQ(r_native_format(device, R_Format::R8G8B8A8_SRGB), 0x8C43);       // GL_SRGB8_ALPHA8
        EXPECT_EQ(r_native_format(device, R_Format::D24_UNORM_S8_UINT), 0x88F0);   // GL_DEPTH24_STENCIL8
        EXPECT_EQ(r_native_format(device, R_Format::D32_SFLOAT_S8_UINT), 0x8CAD);  // GL_DEPTH32F_STENCIL8
        EXPECT_EQ(r_format_from_native(device, 0x881A), R_Format::R16G16B16A16_SFLOAT); // GL_RGBA16F
        EXPECT_EQ(r_native_format(device, R_Format::B8G8R8A8_UNORM), 0);           // GL has no BGRA internal format
        EXPECT_EQ(r_format_from_native(device, 0x8D62), R_Format::Unknown);        // GL_RGB565
    }
    EXPECT_EQ(r_format_from_native(device, -1), R_Format::Unknown);
    EXPECT_EQ(r_format_from_native(device, 0), R_Format::Unknown);
    EXPECT_EQ(r_native_format(device, R_Format::Unknown), 0);
    EXPECT_EQ(r_native_format(device, R_Format::Count), 0);
}

TEST_P(RenderTest, device_reports_its_backend)
{
    EXPECT_EQ(r_device_backend(device), GetParam());
}

TEST(RenderBackendTest, unknown_backend_creates_no_device)
{
    os_set_assert_mode(OS_AssertMode::Log);
    EXPECT_EQ(r_create_device({ .backend = R_Backend::Count }), nullptr);
    EXPECT_EQ(r_create_device({ .backend = (R_Backend)1000 }), nullptr);
    r_destroy_device(nullptr);
}

// The GL text of every shader is embedded next to its SPIR-V, with the sampler names by binding
TEST(RenderBackendTest, shader_code_carries_every_backend)
{
    EXPECT_NE(composite_frag_shader.spirv, nullptr);
    EXPECT_GT(composite_frag_shader.spirv_size, 0u);
    ASSERT_NE(composite_frag_shader.glsl, nullptr);
    EXPECT_NE(strstr(composite_frag_shader.glsl, "R_FRAGMENT_MAIN_BEGIN"), nullptr);
    EXPECT_EQ(strstr(composite_frag_shader.glsl, "#include \"shader.h\""), nullptr);
    EXPECT_STREQ(composite_frag_shader.textures[0], "source");
    EXPECT_EQ(composite_frag_shader.textures[1], nullptr);
    EXPECT_STREQ(passthrough_frag_shader.textures[0], "albedo");
    EXPECT_STREQ(passthrough_frag_shader.textures[1], "color_map");
    EXPECT_EQ(composite_vert_shader.textures[0], nullptr);
    // Metal source translated from the SPIR-V where the build has Metal (docs/shaders.md)
    EXPECT_EQ(composite_frag_shader.msl != nullptr, r_backend_available(R_Backend::Metal));
    if (composite_frag_shader.msl)
    {
        EXPECT_NE(strstr(composite_frag_shader.msl, "fragment main0_out main0("), nullptr);
        EXPECT_NE(strstr(composite_vert_shader.msl, "vertex main0_out main0("), nullptr);
    }
}

TEST_P(RenderTest, pipeline_without_code_for_the_backend_is_rejected)
{
    const R_RenderPassHandle pass  = r_create_render_pass(device, { .color_format = R_Format::R8G8B8A8_UNORM, .load_op = R_LoadOp::Clear, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment });
    const R_ShaderCode       empty = {};
    EXPECT_FALSE(r_is_valid(device, r_create_pipeline(device, { .vertex = &composite_vert_shader, .render_pass = pass, .textures_count = 1 })));
    EXPECT_FALSE(r_is_valid(device, r_create_pipeline(device, { .vertex = &empty, .fragment = &composite_frag_shader, .render_pass = pass, .textures_count = 1 })));
    EXPECT_FALSE(r_is_valid(device, r_create_pipeline(device, { .render_pass = pass })));
    r_destroy_render_pass(device, pass);
}

TEST_P(RenderTest, clear_and_read_back)
{
    const R_ImageHandle image = make_target(8, 4, 1);
    ASSERT_TRUE(r_is_valid(device, image));

    u32 w = 0;
    u32 h = 0;
    u32 layers = 0;
    r_get_image_size(device, image, w, h, layers);
    EXPECT_EQ(w, 8u);
    EXPECT_EQ(h, 4u);
    EXPECT_EQ(layers, 1u);
    EXPECT_NE(r_get_image_native(device, image), 0u);

    clear_layer(image, 0, { 1.0f, 0.0f, 0.0f, 1.0f });

    u8 pixels[8 * 4 * 4] = {};
    ASSERT_TRUE(r_read_image(device, image, 0, R_ImageLayout::ColorAttachment, pixels, sizeof(pixels)));
    expect_pixel(pixels, 8, 0, 0, 255, 0, 0, 255);
    expect_pixel(pixels, 8, 7, 3, 255, 0, 0, 255);

    EXPECT_FALSE(r_read_image(device, image, 0, R_ImageLayout::ColorAttachment, pixels, 4));
    EXPECT_FALSE(r_read_image(device, image, 3, R_ImageLayout::ColorAttachment, pixels, sizeof(pixels)));

    r_destroy_image(device, image);
    EXPECT_FALSE(r_is_valid(device, image));
}

TEST_P(RenderTest, composite_samples_layers_into_target)
{
    const R_ImageHandle source = make_target(16, 16, 2);
    clear_layer(source, 0, { 0.0f, 1.0f, 0.0f, 1.0f });
    clear_layer(source, 1, { 0.0f, 0.0f, 1.0f, 1.0f });

    const R_ImageHandle target = make_target(32, 16, 1);
    clear_layer(target, 0, { 0.0f, 0.0f, 0.0f, 1.0f });

    const R_RenderPassHandle pass = r_create_render_pass(device, { .color_format = R_Format::R8G8B8A8_UNORM, .load_op = R_LoadOp::Load, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment });
    const R_FramebufferHandle fb = r_create_framebuffer(device, { .render_pass = pass, .image = target, .layer = 0 });
    const R_PipelineHandle pipeline = r_create_pipeline(device, {
        .vertex              = &composite_vert_shader,
        .fragment            = &composite_frag_shader,
        .render_pass         = pass,
        .textures_count      = 1,
        .texture_type        = R_TextureType::Type2DArray,
        .push_constants_size = 128,
    });
    ASSERT_TRUE(r_is_valid(device, pipeline));

    const R_CmdBufferHandle cmd = r_begin_frame(device);
    r_cmd_transition_image(device, cmd, source, R_ImageLayout::ColorAttachment, R_ImageLayout::ShaderReadOnly);
    r_cmd_begin_render_pass(device, cmd, pass, fb, {});
    r_cmd_bind_pipeline(device, cmd, pipeline);
    r_cmd_bind_texture(device, cmd, pipeline, source, R_Filter::Nearest);
    for (u32 eye = 0; eye < 2; eye++)
    {
        r_cmd_set_viewport(device, cmd, { (f32)(eye * 16), 0.0f, 16.0f, 16.0f });
        r_cmd_set_scissor(device, cmd, { (i32)(eye * 16), 0, 16, 16 });
        const COMP_Push push = { { { -1.0f, -1.0f, 0.5f, 1.0f }, { 1.0f, -1.0f, 0.5f, 1.0f }, { -1.0f, 1.0f, 0.5f, 1.0f }, { 1.0f, 1.0f, 0.5f, 1.0f } }, { 0.0f, 0.0f, 1.0f, 1.0f }, {}, {}, (i32)eye, 0, 0.0f, 0 };
        r_cmd_push_constants(device, cmd, pipeline, &push, sizeof(push));
        r_cmd_draw(device, cmd, 6, 0);
    }
    r_cmd_end_render_pass(device, cmd);
    r_cmd_transition_image(device, cmd, source, R_ImageLayout::ShaderReadOnly, R_ImageLayout::ColorAttachment);
    r_end_frame(device, cmd);
    r_wait_idle(device);

    u8 pixels[32 * 16 * 4] = {};
    ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::ColorAttachment, pixels, sizeof(pixels)));
    expect_pixel(pixels, 32, 2, 2, 0, 255, 0, 255);
    expect_pixel(pixels, 32, 15, 15, 0, 255, 0, 255);
    expect_pixel(pixels, 32, 16, 0, 0, 0, 255, 255);
    expect_pixel(pixels, 32, 31, 15, 0, 0, 255, 255);

    r_destroy_pipeline(device, pipeline);
    r_destroy_framebuffer(device, fb);
    r_destroy_render_pass(device, pass);
    r_destroy_image(device, source);
    r_destroy_image(device, target);
}

// XR_FB_composition_layer_alpha_blend: every factor for color and alpha separately
TEST_P(RenderTest, custom_blend_factors_combine_source_and_target)
{
    const R_ImageHandle source = make_target(4, 4, 1);
    const R_ImageHandle target = make_target(4, 4, 1);
    const R_RenderPassHandle  pass = r_create_render_pass(device, { .color_format = R_Format::R8G8B8A8_UNORM, .load_op = R_LoadOp::Load, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment });
    const R_FramebufferHandle fb   = r_create_framebuffer(device, { .render_pass = pass, .image = target, .layer = 0 });
    const f32 src[4] = { 1.0f, 0.0f, 0.0f, 0.2f };
    const f32 dst[4] = { 0.0f, 0.0f, 1.0f, 0.6f };
    clear_layer(source, 0, { src[0], src[1], src[2], src[3] });

    struct Case
    {
        R_BlendFactor factors[4]; // source color, target color, source alpha, target alpha
    };
    const Case cases[] = {
        { { R_BlendFactor::SrcAlpha, R_BlendFactor::OneMinusSrcAlpha, R_BlendFactor::One, R_BlendFactor::Zero } },
        { { R_BlendFactor::DstAlpha, R_BlendFactor::OneMinusDstAlpha, R_BlendFactor::Zero, R_BlendFactor::One } },
        { { R_BlendFactor::One, R_BlendFactor::One, R_BlendFactor::OneMinusSrcAlpha, R_BlendFactor::DstAlpha } },
        { { R_BlendFactor::Zero, R_BlendFactor::SrcAlpha, R_BlendFactor::OneMinusDstAlpha, R_BlendFactor::OneMinusSrcAlpha } },
    };
    for (const Case& test : cases)
    {
        const R_PipelineHandle pipeline = r_create_pipeline(device, {
            .vertex = &composite_vert_shader, .fragment = &composite_frag_shader, .render_pass = pass, .textures_count = 1,
            .texture_type = R_TextureType::Type2DArray, .push_constants_size = 128, .blend_enable = true,
            .blend_custom = true, .blend_factors = { test.factors[0], test.factors[1], test.factors[2], test.factors[3] },
        });
        ASSERT_TRUE(r_is_valid(device, pipeline));
        clear_layer(target, 0, { dst[0], dst[1], dst[2], dst[3] });
        const R_CmdBufferHandle cmd = r_begin_immediate(device);
        r_cmd_transition_image(device, cmd, source, R_ImageLayout::ColorAttachment, R_ImageLayout::ShaderReadOnly);
        r_cmd_begin_render_pass(device, cmd, pass, fb, {});
        r_cmd_bind_pipeline(device, cmd, pipeline);
        r_cmd_bind_texture(device, cmd, pipeline, source, R_Filter::Nearest);
        r_cmd_set_viewport(device, cmd, { 0.0f, 0.0f, 4.0f, 4.0f });
        r_cmd_set_scissor(device, cmd, { 0, 0, 4, 4 });
        const COMP_Push push = { { { -1.0f, -1.0f, 0.5f, 1.0f }, { 1.0f, -1.0f, 0.5f, 1.0f }, { -1.0f, 1.0f, 0.5f, 1.0f }, { 1.0f, 1.0f, 0.5f, 1.0f } }, { 0.0f, 0.0f, 1.0f, 1.0f }, {}, {}, 0, COMP_FLAG_SOURCE_ALPHA, 0.0f, 0 };
        r_cmd_push_constants(device, cmd, pipeline, &push, sizeof(push));
        r_cmd_draw(device, cmd, 6, 0);
        r_cmd_end_render_pass(device, cmd);
        r_cmd_transition_image(device, cmd, source, R_ImageLayout::ShaderReadOnly, R_ImageLayout::ColorAttachment);
        r_end_immediate(device, cmd);

        u8 pixels[4 * 4 * 4] = {};
        ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::ColorAttachment, pixels, sizeof(pixels)));
        // The stored inputs: 8-bit values of src and dst
        const f32 s[4] = { 1.0f, 0.0f, 0.0f, 51.0f / 255.0f };
        const f32 d[4] = { 0.0f, 0.0f, 1.0f, 153.0f / 255.0f };
        auto factor = [&] (R_BlendFactor f)
        {
            switch (f)
            {
            case R_BlendFactor::Zero:
                return 0.0f;
            case R_BlendFactor::One:
                return 1.0f;
            case R_BlendFactor::SrcAlpha:
                return s[3];
            case R_BlendFactor::OneMinusSrcAlpha:
                return 1.0f - s[3];
            case R_BlendFactor::DstAlpha:
                return d[3];
            case R_BlendFactor::OneMinusDstAlpha:
                return 1.0f - d[3];
            }
            return 0.0f;
        };
        for (u32 c = 0; c < 4; c++)
        {
            const b8  alpha    = c == 3;
            const f32 expected = zm::clamp(s[c] * factor(test.factors[alpha ? 2 : 0]) + d[c] * factor(test.factors[alpha ? 3 : 1]), 0.0f, 1.0f) * 255.0f;
            EXPECT_NEAR((f32)pixels[(1 * 4 + 1) * 4 + c], expected, 1.0f) << "channel " << c << " factors " << (u32)test.factors[0] << (u32)test.factors[1] << (u32)test.factors[2] << (u32)test.factors[3];
        }
        r_destroy_pipeline(device, pipeline);
    }

    r_destroy_framebuffer(device, fb);
    r_destroy_render_pass(device, pass);
    r_destroy_image(device, source);
    r_destroy_image(device, target);
}

TEST_P(RenderTest, buffers_and_upload)
{
    const R_BufferHandle staging = r_create_buffer(device, { .size = (u64)4 * 4 * 4, .usage_flags = (u32)R_BufferUsageFlags::TransferSrc, .host_visible = true });
    ASSERT_TRUE(r_is_valid(device, staging));
    u8* data = (u8*)r_get_buffer_mapped_data(device, staging);
    ASSERT_NE(data, nullptr);
    for (u32 i = 0; i < 16; i++)
    {
        data[i * 4 + 0] = (u8)i;
        data[i * 4 + 1] = 100;
        data[i * 4 + 2] = 200;
        data[i * 4 + 3] = 255;
    }
    r_flush_buffer(device, staging, 0, (u64)4 * 4 * 4);

    const R_ImageHandle image = r_create_image(device, { .width = 4, .height = 4, .format = R_Format::R8G8B8A8_UNORM, .usage_flags = (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::TransferSrc | (u32)R_ImageUsageFlags::Sampled, .initial_layout = R_ImageLayout::TransferDst });
    const R_CmdBufferHandle cmd = r_begin_immediate(device);
    r_cmd_copy_buffer_to_image(device, cmd, staging, image, 0);
    r_cmd_transition_image(device, cmd, image, R_ImageLayout::TransferDst, R_ImageLayout::ShaderReadOnly);
    r_end_immediate(device, cmd);

    u8 pixels[4 * 4 * 4] = {};
    ASSERT_TRUE(r_read_image(device, image, 0, R_ImageLayout::ShaderReadOnly, pixels, sizeof(pixels)));
    expect_pixel(pixels, 4, 3, 3, 15, 100, 200, 255);
    expect_pixel(pixels, 4, 1, 0, 1, 100, 200, 255);

    const R_BufferHandle gpu = r_create_buffer(device, { .size = 256, .usage_flags = (u32)R_BufferUsageFlags::Vertex | (u32)R_BufferUsageFlags::TransferDst, .host_visible = false });
    EXPECT_TRUE(r_is_valid(device, gpu));
    EXPECT_EQ(r_get_buffer_mapped_data(device, gpu), nullptr);
    r_flush_buffer(device, gpu, 0, 16);
    r_flush_buffer(device, staging, 0, 1024);

    r_destroy_buffer(device, gpu);
    r_destroy_buffer(device, staging);
    r_destroy_image(device, image);
}

TEST_P(RenderTest, blit_scales_between_images)
{
    const R_ImageHandle src = make_target(4, 4, 1);
    clear_layer(src, 0, { 1.0f, 1.0f, 0.0f, 1.0f });
    const R_ImageHandle dst = r_create_image(device, { .width = 8, .height = 8, .format = R_Format::R8G8B8A8_UNORM, .usage_flags = (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::TransferSrc, .initial_layout = R_ImageLayout::TransferDst });

    const R_CmdBufferHandle cmd = r_begin_immediate(device);
    r_cmd_transition_image(device, cmd, src, R_ImageLayout::ColorAttachment, R_ImageLayout::TransferSrc);
    r_cmd_blit_image(device, cmd, src, 0, dst, { 2, 2, 4, 4 }, R_Filter::Nearest);
    r_end_immediate(device, cmd);

    u8 pixels[8 * 8 * 4] = {};
    ASSERT_TRUE(r_read_image(device, dst, 0, R_ImageLayout::TransferDst, pixels, sizeof(pixels)));
    expect_pixel(pixels, 8, 3, 3, 255, 255, 0, 255);
    expect_pixel(pixels, 8, 5, 5, 255, 255, 0, 255);

    r_destroy_image(device, src);
    r_destroy_image(device, dst);
}

TEST_P(RenderTest, garbage_handles_are_rejected)
{
    const R_ImageHandle      bad_image = R_ImageHandle(0xDEADBEEF12345678ull);
    const R_BufferHandle     bad_buffer = R_BufferHandle(0xDEADBEEF12345678ull);
    const R_PipelineHandle   bad_pipeline = R_PipelineHandle(7);
    const R_RenderPassHandle bad_pass = R_RenderPassHandle(7);
    EXPECT_FALSE(r_is_valid(device, bad_image));
    EXPECT_FALSE(r_is_valid(device, bad_buffer));
    EXPECT_FALSE(r_is_valid(device, bad_pipeline));
    EXPECT_FALSE(r_is_valid(device, bad_pass));
    EXPECT_EQ(r_get_image_native(device, bad_image), 0u);
    EXPECT_EQ(r_get_buffer_mapped_data(device, bad_buffer), nullptr);
    r_destroy_image(device, bad_image);
    r_destroy_buffer(device, bad_buffer);
    r_destroy_pipeline(device, bad_pipeline);
    r_destroy_render_pass(device, bad_pass);
    EXPECT_FALSE(r_is_valid(device, r_create_framebuffer(device, { .render_pass = bad_pass, .image = bad_image })));
    EXPECT_FALSE(r_is_valid(device, r_create_image(device, { .width = 0, .height = 0, .format = R_Format::R8G8B8A8_UNORM, .usage_flags = (u32)R_ImageUsageFlags::Sampled })));
    EXPECT_FALSE(r_is_valid(device, r_create_image(device, { .width = 4, .height = 4, .layers = R_MAX_IMAGE_LAYERS + 1, .format = R_Format::R8G8B8A8_UNORM, .usage_flags = (u32)R_ImageUsageFlags::Sampled })));
    EXPECT_FALSE(r_is_valid(device, r_create_image(device, { .width = 4, .height = 4, .mip_levels = 4, .format = R_Format::R8G8B8A8_UNORM, .usage_flags = (u32)R_ImageUsageFlags::Sampled })));
    EXPECT_FALSE(r_is_valid(device, r_create_buffer(device, { .size = 0, .usage_flags = (u32)R_BufferUsageFlags::Vertex })));
}

// Regression: the OpenXR Vulkan binding only allows queue access in xrBeginFrame, xrEndFrame,
// xrAcquireSwapchainImage and xrReleaseSwapchainImage. Creating and destroying resources used to
// submit layout transitions and call vkQueueWaitIdle, which the conformance layer reports. The
// D3D bindings have the same rule for the D3D12 queue and the D3D11 immediate context.
TEST_P(RenderTest, resource_lifetime_never_touches_the_queue)
{
    if (GetParam() == R_Backend::OpenGL || GetParam() == R_Backend::OpenGLES)
    {
        GTEST_SKIP() << "GL has no queue rules";
    }
    const u64           before = r_queue_access_count(device);
    const R_ImageHandle image  = make_target(16, 16, 2);
    const R_ImageHandle depth  = r_create_image(device, { .width = 16, .height = 16, .format = R_Format::D32_SFLOAT, .usage_flags = (u32)R_ImageUsageFlags::DepthStencilAttachment, .initial_layout = R_ImageLayout::DepthStencilAttachment });
    const R_BufferHandle buffer = r_create_buffer(device, { .size = 256, .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
    const R_RenderPassHandle pass = r_create_render_pass(device, { .color_format = R_Format::R8G8B8A8_UNORM, .load_op = R_LoadOp::Clear, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment });
    const R_FramebufferHandle fb = r_create_framebuffer(device, { .render_pass = pass, .image = image, .layer = 1 });
    ASSERT_TRUE(r_is_valid(device, image) && r_is_valid(device, depth) && r_is_valid(device, buffer) && r_is_valid(device, fb));
    r_wait_idle(device);
    EXPECT_EQ(r_queue_access_count(device), before);

    // Vulkan submits the initial layouts once; D3D creates images in theirs
    const u64 flushes = GetParam() == R_Backend::Vulkan ? 1 : 0;
    r_flush_initial_layouts(device);
    EXPECT_EQ(r_queue_access_count(device), before + flushes);
    r_flush_initial_layouts(device);
    EXPECT_EQ(r_queue_access_count(device), before + flushes);

    r_destroy_framebuffer(device, fb);
    r_destroy_render_pass(device, pass);
    r_destroy_buffer(device, buffer);
    r_destroy_image(device, depth);
    r_destroy_image(device, image);
    EXPECT_EQ(r_queue_access_count(device), before + flushes);
}

//
// Depth
//

namespace
{

struct MeshVertex
{
    f32 position[3];
    f32 uv[2];
};

} // namespace

static COMP_PassthroughPush identity_push ()
{
    COMP_PassthroughPush push = {};
    for (u32 i = 0; i < 4; i++)
    {
        push.clip_from_room[(usize)i * 5] = 1.0f;
    }
    push.adjust[0] = 1.0f;
    push.adjust[2] = 1.0f;
    push.adjust[3] = 1.0f;
    return push;
}

// Two quads in clip space: a near one on the left half, a far one over everything
static void depth_quads (MeshVertex* out, b8 near_first)
{
    const MeshVertex near_quad[6] = {
        { { -1, -1, 0.25f }, { 0.25f, 0.5f } }, { { 0, -1, 0.25f }, { 0.25f, 0.5f } }, { { -1, 1, 0.25f }, { 0.25f, 0.5f } },
        { { -1, 1, 0.25f }, { 0.25f, 0.5f } }, { { 0, -1, 0.25f }, { 0.25f, 0.5f } }, { { 0, 1, 0.25f }, { 0.25f, 0.5f } },
    };
    const MeshVertex far_quad[6] = {
        { { -1, -1, 0.75f }, { 0.75f, 0.5f } }, { { 1, -1, 0.75f }, { 0.75f, 0.5f } }, { { -1, 1, 0.75f }, { 0.75f, 0.5f } },
        { { -1, 1, 0.75f }, { 0.75f, 0.5f } }, { { 1, -1, 0.75f }, { 0.75f, 0.5f } }, { { 1, 1, 0.75f }, { 0.75f, 0.5f } },
    };
    memory_copy(out, near_first ? near_quad : far_quad, sizeof(near_quad));
    memory_copy(out + 6, near_first ? far_quad : near_quad, sizeof(near_quad));
}

namespace
{

struct RenderDepthTest : RenderTest
{
    R_ImageHandle upload_rgba (u32 width, u32 height, const u8* rgba)
    {
        const R_BufferHandle staging = r_create_buffer(device, { .size = (u64)width * height * 4, .usage_flags = (u32)R_BufferUsageFlags::TransferSrc, .host_visible = true });
        memory_copy(r_get_buffer_mapped_data(device, staging), rgba, (usize)width * height * 4);
        r_flush_buffer(device, staging, 0, (u64)width * height * 4);
        const R_ImageHandle image = r_create_image(device, { .width = width, .height = height, .format = R_Format::R8G8B8A8_UNORM, .usage_flags = (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::Sampled, .initial_layout = R_ImageLayout::TransferDst });
        const R_CmdBufferHandle cmd = r_begin_immediate(device);
        r_cmd_copy_buffer_to_image(device, cmd, staging, image, 0);
        r_cmd_transition_image(device, cmd, image, R_ImageLayout::TransferDst, R_ImageLayout::ShaderReadOnly);
        r_end_immediate(device, cmd);
        r_destroy_buffer(device, staging);
        return image;
    }
};

} // namespace

TEST_P(RenderDepthTest, depth_test_keeps_the_nearest_surface_in_any_draw_order)
{
    const u8            albedo_pixels[8] = { 255, 0, 0, 255, 0, 0, 255, 255 };
    u8                  lut_pixels[256 * 4] = {};
    const R_ImageHandle albedo = upload_rgba(2, 1, albedo_pixels);
    const R_ImageHandle lut    = upload_rgba(256, 1, lut_pixels);
    const R_ImageHandle target = make_target(16, 8, 1);
    const R_ImageHandle depth  = r_create_image(device, { .width = 16, .height = 8, .format = R_Format::D32_SFLOAT, .usage_flags = (u32)R_ImageUsageFlags::DepthStencilAttachment, .initial_layout = R_ImageLayout::DepthStencilAttachment });
    ASSERT_TRUE(r_is_valid(device, depth));

    const R_RenderPassHandle pass = r_create_render_pass(device, {
        .color_format = R_Format::R8G8B8A8_UNORM, .load_op = R_LoadOp::Clear, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment,
        .depth_format = R_Format::D32_SFLOAT, .depth_load_op = R_LoadOp::Clear, .depth_initial_layout = R_ImageLayout::DepthStencilAttachment, .depth_final_layout = R_ImageLayout::DepthStencilAttachment,
    });
    ASSERT_TRUE(r_is_valid(device, pass));
    const R_FramebufferHandle fb = r_create_framebuffer(device, { .render_pass = pass, .image = target, .layer = 0, .depth_image = depth, .depth_layer = 0 });
    ASSERT_TRUE(r_is_valid(device, fb));
    const R_PipelineHandle pipeline = r_create_pipeline(device, {
        .vertex = &passthrough_vert_shader, .fragment = &passthrough_frag_shader,
        .render_pass = pass, .textures_count = 2, .push_constants_size = sizeof(COMP_PassthroughPush), .depth_test = true, .depth_write = true,
        .vertex_stride = sizeof(MeshVertex), .vertex_attributes = { { 0, R_Format::R32G32B32_SFLOAT, 0 }, { 1, R_Format::R32G32_SFLOAT, 12 } }, .vertex_attributes_count = 2,
    });
    ASSERT_TRUE(r_is_valid(device, pipeline));

    const R_BufferHandle vertices = r_create_buffer(device, { .size = sizeof(MeshVertex) * 12, .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
    for (const b8 near_first : { true, false })
    {
        depth_quads((MeshVertex*)r_get_buffer_mapped_data(device, vertices), near_first);
        r_flush_buffer(device, vertices, 0, sizeof(MeshVertex) * 12);
        const R_ImageHandle     textures[2] = { albedo, lut };
        const COMP_PassthroughPush push        = identity_push();
        const R_CmdBufferHandle cmd         = r_begin_immediate(device);
        r_cmd_begin_render_pass(device, cmd, pass, fb, { 0, 0, 0, 1 });
        r_cmd_bind_pipeline(device, cmd, pipeline);
        r_cmd_bind_textures(device, cmd, pipeline, textures, 2, R_Filter::Nearest);
        r_cmd_set_viewport(device, cmd, { 0, 0, 16, 8 });
        r_cmd_set_scissor(device, cmd, { 0, 0, 16, 8 });
        r_cmd_push_constants(device, cmd, pipeline, &push, sizeof(push));
        r_cmd_bind_vertex_buffer(device, cmd, vertices, 0);
        r_cmd_draw(device, cmd, 12, 0);
        r_cmd_end_render_pass(device, cmd);
        r_end_immediate(device, cmd);

        u8 pixels[16 * 8 * 4] = {};
        ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::ColorAttachment, pixels, sizeof(pixels)));
        expect_pixel(pixels, 16, 2, 4, 255, 0, 0, 255);
        expect_pixel(pixels, 16, 12, 4, 0, 0, 255, 255);
    }

    r_destroy_buffer(device, vertices);
    r_destroy_pipeline(device, pipeline);
    r_destroy_framebuffer(device, fb);
    r_destroy_render_pass(device, pass);
    r_destroy_image(device, depth);
    r_destroy_image(device, target);
    r_destroy_image(device, lut);
    r_destroy_image(device, albedo);
}

// XR_META_passthrough_color_lut: an atlas of 3D LUT slices (shaders/blocks.h), interpolated
// trilinearly between elements in sRGB
TEST_P(RenderDepthTest, passthrough_color_luts_interpolate_trilinearly)
{
    // UNORM 55 is linear 0.216, sRGB 0.502: between the elements of a 2-element LUT
    const u8   albedo_pixels[4] = { 55, 55, 0, 255 };
    static u8  atlas[COMP_COLOR_LUT_ATLAS_WIDTH * COMP_COLOR_LUT_ATLAS_HEIGHT * 4];
    const u32  n                = 2;
    const u32  tiles            = COMP_COLOR_LUT_ATLAS_WIDTH / n;
    memset(atlas, 0, sizeof(atlas));
    for (u32 b = 0; b < n; b++)
    {
        for (u32 g = 0; g < n; g++)
        {
            for (u32 r = 0; r < n; r++)
            {
                // First LUT: out = (r, r * g, 0); second: blue everywhere
                const u32 x = (b % tiles) * n + r;
                const u32 y = (b / tiles) * n + g;
                u8*       a = atlas + ((usize)y * COMP_COLOR_LUT_ATLAS_WIDTH + x) * 4;
                u8*       c = atlas + ((usize)(y + COMP_COLOR_LUT_ATLAS_HEIGHT / 2) * COMP_COLOR_LUT_ATLAS_WIDTH + x) * 4;
                a[0]        = (u8)(r * 255);
                a[1]        = (u8)(r * g * 255);
                a[3]        = 255;
                c[2]        = 255;
                c[3]        = 255;
            }
        }
    }
    const R_ImageHandle albedo = upload_rgba(1, 1, albedo_pixels);
    const R_ImageHandle lut    = upload_rgba(COMP_COLOR_LUT_ATLAS_WIDTH, COMP_COLOR_LUT_ATLAS_HEIGHT, atlas);
    const R_ImageHandle target = make_target(4, 4, 1);
    const R_RenderPassHandle pass = r_create_render_pass(device, { .color_format = R_Format::R8G8B8A8_UNORM, .load_op = R_LoadOp::Clear, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment });
    ASSERT_TRUE(r_is_valid(device, pass));
    const R_FramebufferHandle fb = r_create_framebuffer(device, { .render_pass = pass, .image = target, .layer = 0 });
    const R_PipelineHandle pipeline = r_create_pipeline(device, {
        .vertex = &passthrough_vert_shader, .fragment = &passthrough_frag_shader,
        .render_pass = pass, .textures_count = 2, .push_constants_size = sizeof(COMP_PassthroughPush),
        .vertex_stride = sizeof(MeshVertex), .vertex_attributes = { { 0, R_Format::R32G32B32_SFLOAT, 0 }, { 1, R_Format::R32G32_SFLOAT, 12 } }, .vertex_attributes_count = 2,
    });
    ASSERT_TRUE(r_is_valid(device, pipeline));
    const MeshVertex     quad[6]  = {
        { { -1, -1, 0.5f }, { 0.5f, 0.5f } }, { { 1, -1, 0.5f }, { 0.5f, 0.5f } }, { { -1, 1, 0.5f }, { 0.5f, 0.5f } },
        { { -1, 1, 0.5f }, { 0.5f, 0.5f } }, { { 1, -1, 0.5f }, { 0.5f, 0.5f } }, { { 1, 1, 0.5f }, { 0.5f, 0.5f } },
    };
    const R_BufferHandle vertices = r_create_buffer(device, { .size = sizeof(quad), .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
    memory_copy(r_get_buffer_mapped_data(device, vertices), quad, sizeof(quad));
    r_flush_buffer(device, vertices, 0, sizeof(quad));

    struct Case
    {
        i32 color_map;
        f32 weight;
        u8  rgb[3];
    };
    // Nearest elements would give 0 or 255; sRGB 0.502 * 0.502 is linear 13
    const Case cases[] = { { 4, 1.0f, { 55, 13, 0 } }, { 4, 0.5f, { 55, 30, 0 } }, { 5, 0.0f, { 55, 13, 0 } }, { 5, 0.5f, { 13, 4, 55 } } };
    for (const Case& c : cases)
    {
        const R_ImageHandle  textures[2] = { albedo, lut };
        COMP_PassthroughPush push        = identity_push();
        push.color_map                   = c.color_map;
        push.lut_resolution              = (i32)(n | n << 8);
        push.lut_weight                  = c.weight;
        const R_CmdBufferHandle cmd = r_begin_immediate(device);
        r_cmd_begin_render_pass(device, cmd, pass, fb, { 0, 0, 0, 1 });
        r_cmd_bind_pipeline(device, cmd, pipeline);
        r_cmd_bind_textures(device, cmd, pipeline, textures, 2, R_Filter::Linear);
        r_cmd_set_viewport(device, cmd, { 0, 0, 4, 4 });
        r_cmd_set_scissor(device, cmd, { 0, 0, 4, 4 });
        r_cmd_push_constants(device, cmd, pipeline, &push, sizeof(push));
        r_cmd_bind_vertex_buffer(device, cmd, vertices, 0);
        r_cmd_draw(device, cmd, 6, 0);
        r_cmd_end_render_pass(device, cmd);
        r_end_immediate(device, cmd);

        u8 pixels[4 * 4 * 4] = {};
        ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::ColorAttachment, pixels, sizeof(pixels)));
        const u8* p = pixels + (2 * 4 + 2) * 4;
        for (u32 i = 0; i < 3; i++)
        {
            EXPECT_NEAR(p[i], c.rgb[i], 2) << "map " << c.color_map << " weight " << c.weight << " channel " << i;
        }
    }

    r_destroy_buffer(device, vertices);
    r_destroy_pipeline(device, pipeline);
    r_destroy_framebuffer(device, fb);
    r_destroy_render_pass(device, pass);
    r_destroy_image(device, target);
    r_destroy_image(device, lut);
    r_destroy_image(device, albedo);
}

TEST_P(RenderDepthTest, depth_only_pass_into_an_array_layer)
{
    const R_ImageHandle depth = r_create_image(device, { .width = 8, .height = 4, .layers = 2, .format = R_Format::D16_UNORM, .usage_flags = (u32)R_ImageUsageFlags::DepthStencilAttachment | (u32)R_ImageUsageFlags::Sampled | (u32)R_ImageUsageFlags::TransferSrc, .initial_layout = R_ImageLayout::ShaderReadOnly });
    ASSERT_TRUE(r_is_valid(device, depth));
    const R_RenderPassHandle pass = r_create_render_pass(device, { .depth_format = R_Format::D16_UNORM, .depth_load_op = R_LoadOp::Clear, .depth_initial_layout = R_ImageLayout::ShaderReadOnly, .depth_final_layout = R_ImageLayout::ShaderReadOnly });
    ASSERT_TRUE(r_is_valid(device, pass));
    const R_FramebufferHandle fb = r_create_framebuffer(device, { .render_pass = pass, .depth_image = depth, .depth_layer = 1 });
    ASSERT_TRUE(r_is_valid(device, fb));
    const R_PipelineHandle pipeline = r_create_pipeline(device, {
        .vertex = &environment_depth_vert_shader, .fragment = &environment_depth_frag_shader,
        .render_pass = pass, .push_constants_size = 64, .depth_test = true, .depth_write = true,
        .vertex_stride = sizeof(MeshVertex), .vertex_attributes = { { 0, R_Format::R32G32B32_SFLOAT, 0 } }, .vertex_attributes_count = 1,
    });
    ASSERT_TRUE(r_is_valid(device, pipeline));

    const R_BufferHandle vertices = r_create_buffer(device, { .size = sizeof(MeshVertex) * 12, .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
    depth_quads((MeshVertex*)r_get_buffer_mapped_data(device, vertices), false);
    r_flush_buffer(device, vertices, 0, sizeof(MeshVertex) * 12);
    const COMP_PassthroughPush push = identity_push();
    const R_CmdBufferHandle cmd  = r_begin_immediate(device);
    r_cmd_begin_render_pass(device, cmd, pass, fb, {});
    r_cmd_bind_pipeline(device, cmd, pipeline);
    r_cmd_set_viewport(device, cmd, { 0, 0, 8, 4 });
    r_cmd_set_scissor(device, cmd, { 0, 0, 8, 4 });
    r_cmd_push_constants(device, cmd, pipeline, push.clip_from_room, 64);
    r_cmd_bind_vertex_buffer(device, cmd, vertices, 0);
    r_cmd_draw(device, cmd, 6, 6); // the near quad only
    r_cmd_end_render_pass(device, cmd);
    r_end_immediate(device, cmd);

    u16 texels[8 * 4] = {};
    if (GetParam() == R_Backend::OpenGLES)
    {
        // OpenGL ES 3.0 reads colour only (4.3.2); nothing of the runtime reads depth back
        EXPECT_FALSE(r_read_image(device, depth, 1, R_ImageLayout::ShaderReadOnly, texels, sizeof(texels)));
    }
    else
    {
        ASSERT_TRUE(r_read_image(device, depth, 1, R_ImageLayout::ShaderReadOnly, texels, sizeof(texels)));
        EXPECT_NEAR(texels[2 * 8 + 1], 0.25f * 65535.0f, 2.0f);
        EXPECT_EQ(texels[2 * 8 + 6], 0xFFFF);
    }

    r_destroy_buffer(device, vertices);
    r_destroy_pipeline(device, pipeline);
    r_destroy_framebuffer(device, fb);
    r_destroy_render_pass(device, pass);
    r_destroy_image(device, depth);
}

TEST_P(RenderDepthTest, clear_depth_inside_a_pass)
{
    const u8            white[4]  = { 255, 255, 255, 255 };
    u8                  lut[256 * 4] = {};
    const R_ImageHandle albedo    = upload_rgba(1, 1, white);
    const R_ImageHandle map       = upload_rgba(256, 1, lut);
    const R_ImageHandle target    = make_target(8, 8, 1);
    const R_ImageHandle depth     = r_create_image(device, { .width = 8, .height = 8, .format = R_Format::D32_SFLOAT, .usage_flags = (u32)R_ImageUsageFlags::DepthStencilAttachment, .initial_layout = R_ImageLayout::DepthStencilAttachment });
    const R_RenderPassHandle pass = r_create_render_pass(device, {
        .color_format = R_Format::R8G8B8A8_UNORM, .load_op = R_LoadOp::Clear, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment,
        .depth_format = R_Format::D32_SFLOAT, .depth_load_op = R_LoadOp::Clear, .depth_initial_layout = R_ImageLayout::DepthStencilAttachment, .depth_final_layout = R_ImageLayout::DepthStencilAttachment,
    });
    const R_FramebufferHandle fb       = r_create_framebuffer(device, { .render_pass = pass, .image = target, .depth_image = depth });
    const R_PipelineHandle    pipeline = r_create_pipeline(device, {
        .vertex = &passthrough_vert_shader, .fragment = &passthrough_frag_shader,
        .render_pass = pass, .textures_count = 2, .push_constants_size = sizeof(COMP_PassthroughPush), .depth_test = true, .depth_write = true,
        .vertex_stride = sizeof(MeshVertex), .vertex_attributes = { { 0, R_Format::R32G32B32_SFLOAT, 0 }, { 1, R_Format::R32G32_SFLOAT, 12 } }, .vertex_attributes_count = 2,
    });
    ASSERT_TRUE(r_is_valid(device, pipeline));

    // The far quad drawn after the near one shows only because depth was cleared in between
    const R_BufferHandle vertices = r_create_buffer(device, { .size = sizeof(MeshVertex) * 12, .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
    depth_quads((MeshVertex*)r_get_buffer_mapped_data(device, vertices), true);
    r_flush_buffer(device, vertices, 0, sizeof(MeshVertex) * 12);
    const R_ImageHandle     textures[2] = { albedo, map };
    COMP_PassthroughPush       push        = identity_push();
    const R_CmdBufferHandle cmd         = r_begin_immediate(device);
    r_cmd_begin_render_pass(device, cmd, pass, fb, { 0, 0, 0, 1 });
    r_cmd_bind_pipeline(device, cmd, pipeline);
    r_cmd_bind_textures(device, cmd, pipeline, textures, 2, R_Filter::Nearest);
    r_cmd_set_viewport(device, cmd, { 0, 0, 8, 8 });
    r_cmd_set_scissor(device, cmd, { 0, 0, 8, 8 });
    r_cmd_bind_vertex_buffer(device, cmd, vertices, 0);
    r_cmd_push_constants(device, cmd, pipeline, &push, sizeof(push));
    r_cmd_draw(device, cmd, 6, 0);
    r_cmd_clear_depth(device, cmd, { 0, 0, 8, 8 }, 1.0f);
    push.color_map = 2; // mono to mono through an all-black map
    r_cmd_push_constants(device, cmd, pipeline, &push, sizeof(push));
    r_cmd_draw(device, cmd, 6, 6);
    r_cmd_end_render_pass(device, cmd);
    r_end_immediate(device, cmd);

    u8 pixels[8 * 8 * 4] = {};
    ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::ColorAttachment, pixels, sizeof(pixels)));
    expect_pixel(pixels, 8, 1, 4, 0, 0, 0, 255);
    expect_pixel(pixels, 8, 6, 4, 0, 0, 0, 255);

    r_destroy_buffer(device, vertices);
    r_destroy_pipeline(device, pipeline);
    r_destroy_framebuffer(device, fb);
    r_destroy_render_pass(device, pass);
    r_destroy_image(device, depth);
    r_destroy_image(device, target);
    r_destroy_image(device, map);
    r_destroy_image(device, albedo);
}

// Projected passthrough's mask: depth cleared to 0 blocks every less-or-equal draw until an
// always-passing draw opens it
TEST_P(RenderDepthTest, clear_value_and_always_compare_make_a_mask)
{
    const u8            albedo_pixels[8] = { 255, 0, 0, 255, 0, 0, 255, 255 };
    u8                  lut_pixels[256 * 4] = {};
    const R_ImageHandle albedo = upload_rgba(2, 1, albedo_pixels);
    const R_ImageHandle lut    = upload_rgba(256, 1, lut_pixels);
    const R_ImageHandle target = make_target(16, 8, 1);
    const R_ImageHandle depth  = r_create_image(device, { .width = 16, .height = 8, .format = R_Format::D32_SFLOAT, .usage_flags = (u32)R_ImageUsageFlags::DepthStencilAttachment, .initial_layout = R_ImageLayout::DepthStencilAttachment });
    const R_RenderPassHandle pass = r_create_render_pass(device, {
        .color_format = R_Format::R8G8B8A8_UNORM, .load_op = R_LoadOp::Clear, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment,
        .depth_format = R_Format::D32_SFLOAT, .depth_load_op = R_LoadOp::Clear, .depth_initial_layout = R_ImageLayout::DepthStencilAttachment, .depth_final_layout = R_ImageLayout::DepthStencilAttachment,
    });
    const R_FramebufferHandle fb = r_create_framebuffer(device, { .render_pass = pass, .image = target, .depth_image = depth });
    R_CreatePipelineInfo info = {
        .vertex = &passthrough_vert_shader, .fragment = &passthrough_frag_shader,
        .render_pass = pass, .textures_count = 2, .push_constants_size = sizeof(COMP_PassthroughPush), .depth_test = true, .depth_write = true,
        .vertex_stride = sizeof(MeshVertex), .vertex_attributes = { { 0, R_Format::R32G32B32_SFLOAT, 0 }, { 1, R_Format::R32G32_SFLOAT, 12 } }, .vertex_attributes_count = 2,
    };
    const R_PipelineHandle less_equal = r_create_pipeline(device, info);
    info.depth_compare                = R_CompareOp::Always;
    const R_PipelineHandle always     = r_create_pipeline(device, info);
    ASSERT_TRUE(r_is_valid(device, less_equal) && r_is_valid(device, always));

    const R_BufferHandle vertices = r_create_buffer(device, { .size = sizeof(MeshVertex) * 12, .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
    depth_quads((MeshVertex*)r_get_buffer_mapped_data(device, vertices), true); // near left half, then far everywhere
    r_flush_buffer(device, vertices, 0, sizeof(MeshVertex) * 12);
    const R_ImageHandle        textures[2] = { albedo, lut };
    const COMP_PassthroughPush push        = identity_push();
    const R_CmdBufferHandle    cmd         = r_begin_immediate(device);
    r_cmd_begin_render_pass(device, cmd, pass, fb, { 0, 0, 0, 1 });
    r_cmd_set_viewport(device, cmd, { 0, 0, 16, 8 });
    r_cmd_set_scissor(device, cmd, { 0, 0, 16, 8 });
    r_cmd_bind_vertex_buffer(device, cmd, vertices, 0);
    r_cmd_clear_depth(device, cmd, { 0, 0, 16, 8 }, 0.0f);
    r_cmd_bind_pipeline(device, cmd, less_equal);
    r_cmd_bind_textures(device, cmd, less_equal, textures, 2, R_Filter::Nearest);
    r_cmd_push_constants(device, cmd, less_equal, &push, sizeof(push));
    r_cmd_draw(device, cmd, 6, 6); // far quad: blocked everywhere
    r_cmd_bind_pipeline(device, cmd, always);
    r_cmd_bind_textures(device, cmd, always, textures, 2, R_Filter::Nearest);
    r_cmd_push_constants(device, cmd, always, &push, sizeof(push));
    r_cmd_draw(device, cmd, 6, 0); // near quad: passes, opens the left half to 0.25
    r_cmd_bind_pipeline(device, cmd, less_equal);
    r_cmd_bind_textures(device, cmd, less_equal, textures, 2, R_Filter::Nearest);
    r_cmd_push_constants(device, cmd, less_equal, &push, sizeof(push));
    r_cmd_draw(device, cmd, 6, 6); // far quad again: behind the near one on the left, blocked on the right
    r_cmd_end_render_pass(device, cmd);
    r_end_immediate(device, cmd);

    u8 pixels[16 * 8 * 4] = {};
    ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::ColorAttachment, pixels, sizeof(pixels)));
    expect_pixel(pixels, 16, 2, 4, 255, 0, 0, 255);
    expect_pixel(pixels, 16, 12, 4, 0, 0, 0, 255);

    r_destroy_buffer(device, vertices);
    r_destroy_pipeline(device, always);
    r_destroy_pipeline(device, less_equal);
    r_destroy_framebuffer(device, fb);
    r_destroy_render_pass(device, pass);
    r_destroy_image(device, depth);
    r_destroy_image(device, target);
    r_destroy_image(device, lut);
    r_destroy_image(device, albedo);
}

// XR_FB_composition_layer_depth_test compares with any of the eight ops
TEST_P(RenderDepthTest, every_compare_op_tests_against_the_depth_buffer)
{
    const u8            albedo_pixels[8] = { 255, 0, 0, 255, 0, 0, 255, 255 };
    u8                  lut_pixels[256 * 4] = {};
    const R_ImageHandle albedo = upload_rgba(2, 1, albedo_pixels);
    const R_ImageHandle lut    = upload_rgba(256, 1, lut_pixels);
    const R_ImageHandle target = make_target(12, 4, 1);
    const R_ImageHandle depth  = r_create_image(device, { .width = 12, .height = 4, .format = R_Format::D32_SFLOAT, .usage_flags = (u32)R_ImageUsageFlags::DepthStencilAttachment, .initial_layout = R_ImageLayout::DepthStencilAttachment });
    const R_RenderPassHandle pass = r_create_render_pass(device, {
        .color_format = R_Format::R8G8B8A8_UNORM, .load_op = R_LoadOp::Clear, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment,
        .depth_format = R_Format::D32_SFLOAT, .depth_load_op = R_LoadOp::Clear, .depth_initial_layout = R_ImageLayout::DepthStencilAttachment, .depth_final_layout = R_ImageLayout::DepthStencilAttachment,
    });
    const R_FramebufferHandle fb = r_create_framebuffer(device, { .render_pass = pass, .image = target, .depth_image = depth });

    // Three columns nearer than, at and beyond the cleared depth 0.5
    MeshVertex columns[18] = {};
    for (u32 c = 0; c < 3; c++)
    {
        const f32 x0 = -1.0f + (f32)c * (2.0f / 3.0f);
        const f32 x1 = x0 + 2.0f / 3.0f;
        const f32 z  = 0.25f + 0.25f * (f32)c;
        const f32 corners[6][2] = { { x0, -1 }, { x1, -1 }, { x0, 1 }, { x0, 1 }, { x1, -1 }, { x1, 1 } };
        for (u32 v = 0; v < 6; v++)
        {
            columns[c * 6 + v] = { { corners[v][0], corners[v][1], z }, { 0.25f, 0.5f } };
        }
    }
    const R_BufferHandle vertices = r_create_buffer(device, { .size = sizeof(columns), .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
    memory_copy(r_get_buffer_mapped_data(device, vertices), columns, sizeof(columns));
    r_flush_buffer(device, vertices, 0, sizeof(columns));

    struct Case
    {
        R_CompareOp op;
        b8          drawn[3];
    };
    const Case cases[] = {
        { R_CompareOp::Never, { false, false, false } },        { R_CompareOp::Less, { true, false, false } },
        { R_CompareOp::Equal, { false, true, false } },         { R_CompareOp::LessOrEqual, { true, true, false } },
        { R_CompareOp::Greater, { false, false, true } },       { R_CompareOp::NotEqual, { true, false, true } },
        { R_CompareOp::GreaterOrEqual, { false, true, true } }, { R_CompareOp::Always, { true, true, true } },
    };
    for (const Case& test : cases)
    {
        const R_PipelineHandle pipeline = r_create_pipeline(device, {
            .vertex = &passthrough_vert_shader, .fragment = &passthrough_frag_shader,
            .render_pass = pass, .textures_count = 2, .push_constants_size = sizeof(COMP_PassthroughPush), .depth_test = true, .depth_compare = test.op,
            .vertex_stride = sizeof(MeshVertex), .vertex_attributes = { { 0, R_Format::R32G32B32_SFLOAT, 0 }, { 1, R_Format::R32G32_SFLOAT, 12 } }, .vertex_attributes_count = 2,
        });
        ASSERT_TRUE(r_is_valid(device, pipeline));
        const R_ImageHandle        textures[2] = { albedo, lut };
        const COMP_PassthroughPush push        = identity_push();
        const R_CmdBufferHandle    cmd         = r_begin_immediate(device);
        r_cmd_begin_render_pass(device, cmd, pass, fb, { 0, 0, 0, 1 });
        r_cmd_set_viewport(device, cmd, { 0, 0, 12, 4 });
        r_cmd_set_scissor(device, cmd, { 0, 0, 12, 4 });
        r_cmd_clear_depth(device, cmd, { 0, 0, 12, 4 }, 0.5f);
        r_cmd_bind_pipeline(device, cmd, pipeline);
        r_cmd_bind_textures(device, cmd, pipeline, textures, 2, R_Filter::Nearest);
        r_cmd_push_constants(device, cmd, pipeline, &push, sizeof(push));
        r_cmd_bind_vertex_buffer(device, cmd, vertices, 0);
        r_cmd_draw(device, cmd, 18, 0);
        r_cmd_end_render_pass(device, cmd);
        r_end_immediate(device, cmd);

        u8 pixels[12 * 4 * 4] = {};
        ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::ColorAttachment, pixels, sizeof(pixels)));
        for (u32 c = 0; c < 3; c++)
        {
            SCOPED_TRACE(testing::Message() << "op " << (u32)test.op << " column " << c);
            expect_pixel(pixels, 12, c * 4 + 2, 2, test.drawn[c] ? 255 : 0, 0, 0, 255);
        }
        r_destroy_pipeline(device, pipeline);
    }

    r_destroy_buffer(device, vertices);
    r_destroy_framebuffer(device, fb);
    r_destroy_render_pass(device, pass);
    r_destroy_image(device, depth);
    r_destroy_image(device, target);
    r_destroy_image(device, lut);
    r_destroy_image(device, albedo);
}

// A pipeline without color writes leaves the color attachment as it was and still writes depth
TEST_P(RenderDepthTest, no_color_write_draws_only_depth)
{
    const u8            albedo_pixels[8] = { 255, 0, 0, 255, 0, 0, 255, 255 };
    u8                  lut_pixels[256 * 4] = {};
    const R_ImageHandle albedo = upload_rgba(2, 1, albedo_pixels);
    const R_ImageHandle lut    = upload_rgba(256, 1, lut_pixels);
    const R_ImageHandle target = make_target(16, 8, 1);
    const R_ImageHandle depth  = r_create_image(device, { .width = 16, .height = 8, .format = R_Format::D32_SFLOAT, .usage_flags = (u32)R_ImageUsageFlags::DepthStencilAttachment, .initial_layout = R_ImageLayout::DepthStencilAttachment });
    const R_RenderPassHandle pass = r_create_render_pass(device, {
        .color_format = R_Format::R8G8B8A8_UNORM, .load_op = R_LoadOp::Clear, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment,
        .depth_format = R_Format::D32_SFLOAT, .depth_load_op = R_LoadOp::Clear, .depth_initial_layout = R_ImageLayout::DepthStencilAttachment, .depth_final_layout = R_ImageLayout::DepthStencilAttachment,
    });
    const R_FramebufferHandle fb = r_create_framebuffer(device, { .render_pass = pass, .image = target, .depth_image = depth });
    R_CreatePipelineInfo info = {
        .vertex = &passthrough_vert_shader, .fragment = &passthrough_frag_shader,
        .render_pass = pass, .textures_count = 2, .push_constants_size = sizeof(COMP_PassthroughPush), .depth_test = true, .depth_write = true,
        .vertex_stride = sizeof(MeshVertex), .vertex_attributes = { { 0, R_Format::R32G32B32_SFLOAT, 0 }, { 1, R_Format::R32G32_SFLOAT, 12 } }, .vertex_attributes_count = 2,
    };
    const R_PipelineHandle color      = r_create_pipeline(device, info);
    info.no_color_write               = true;
    const R_PipelineHandle depth_only = r_create_pipeline(device, info);
    ASSERT_TRUE(r_is_valid(device, color) && r_is_valid(device, depth_only));

    const R_BufferHandle vertices = r_create_buffer(device, { .size = sizeof(MeshVertex) * 12, .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
    depth_quads((MeshVertex*)r_get_buffer_mapped_data(device, vertices), true);
    r_flush_buffer(device, vertices, 0, sizeof(MeshVertex) * 12);
    const R_ImageHandle        textures[2] = { albedo, lut };
    const COMP_PassthroughPush push        = identity_push();
    const R_CmdBufferHandle    cmd         = r_begin_immediate(device);
    r_cmd_begin_render_pass(device, cmd, pass, fb, { 0, 0, 0, 1 });
    r_cmd_set_viewport(device, cmd, { 0, 0, 16, 8 });
    r_cmd_set_scissor(device, cmd, { 0, 0, 16, 8 });
    r_cmd_bind_vertex_buffer(device, cmd, vertices, 0);
    r_cmd_bind_pipeline(device, cmd, depth_only);
    r_cmd_bind_textures(device, cmd, depth_only, textures, 2, R_Filter::Nearest);
    r_cmd_push_constants(device, cmd, depth_only, &push, sizeof(push));
    r_cmd_draw(device, cmd, 6, 0); // near quad: depth only on the left
    r_cmd_bind_pipeline(device, cmd, color);
    r_cmd_bind_textures(device, cmd, color, textures, 2, R_Filter::Nearest);
    r_cmd_push_constants(device, cmd, color, &push, sizeof(push));
    r_cmd_draw(device, cmd, 6, 6); // far quad: hidden on the left, drawn on the right
    r_cmd_end_render_pass(device, cmd);
    r_end_immediate(device, cmd);

    u8 pixels[16 * 8 * 4] = {};
    ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::ColorAttachment, pixels, sizeof(pixels)));
    expect_pixel(pixels, 16, 2, 4, 0, 0, 0, 255);
    expect_pixel(pixels, 16, 12, 4, 0, 0, 255, 255);

    r_destroy_buffer(device, vertices);
    r_destroy_pipeline(device, depth_only);
    r_destroy_pipeline(device, color);
    r_destroy_framebuffer(device, fb);
    r_destroy_render_pass(device, pass);
    r_destroy_image(device, depth);
    r_destroy_image(device, target);
    r_destroy_image(device, lut);
    r_destroy_image(device, albedo);
}

TEST_P(RenderDepthTest, invalid_depth_setups_are_rejected)
{
    const R_ImageHandle color = make_target(8, 8, 1);
    const R_ImageHandle depth = r_create_image(device, { .width = 8, .height = 8, .format = R_Format::D16_UNORM, .usage_flags = (u32)R_ImageUsageFlags::DepthStencilAttachment, .initial_layout = R_ImageLayout::DepthStencilAttachment });
    const R_ImageHandle small = r_create_image(device, { .width = 4, .height = 4, .format = R_Format::D16_UNORM, .usage_flags = (u32)R_ImageUsageFlags::DepthStencilAttachment, .initial_layout = R_ImageLayout::DepthStencilAttachment });

    EXPECT_FALSE(r_is_valid(device, r_create_render_pass(device, {})));
    EXPECT_FALSE(r_is_valid(device, r_create_render_pass(device, { .depth_format = R_Format::R8G8B8A8_UNORM })));
    EXPECT_FALSE(r_is_valid(device, r_create_render_pass(device, { .color_format = R_Format::D16_UNORM })));

    const R_RenderPassHandle with_depth = r_create_render_pass(device, { .color_format = R_Format::R8G8B8A8_UNORM, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment, .depth_format = R_Format::D16_UNORM, .depth_initial_layout = R_ImageLayout::DepthStencilAttachment, .depth_final_layout = R_ImageLayout::DepthStencilAttachment });
    const R_RenderPassHandle color_only = r_create_render_pass(device, { .color_format = R_Format::R8G8B8A8_UNORM, .initial_layout = R_ImageLayout::ColorAttachment, .final_layout = R_ImageLayout::ColorAttachment });
    ASSERT_TRUE(r_is_valid(device, with_depth));
    EXPECT_FALSE(r_is_valid(device, r_create_framebuffer(device, { .render_pass = with_depth, .image = color })));
    EXPECT_FALSE(r_is_valid(device, r_create_framebuffer(device, { .render_pass = with_depth, .image = color, .depth_image = small })));
    EXPECT_FALSE(r_is_valid(device, r_create_framebuffer(device, { .render_pass = with_depth, .image = color, .depth_image = depth, .depth_layer = 1 })));
    EXPECT_FALSE(r_is_valid(device, r_create_framebuffer(device, { .render_pass = color_only, .image = color, .depth_image = depth })));

    EXPECT_FALSE(r_is_valid(device, r_create_pipeline(device, { .vertex = &environment_depth_vert_shader, .fragment = &environment_depth_frag_shader, .render_pass = color_only, .depth_test = true })));
    EXPECT_FALSE(r_is_valid(device, r_create_pipeline(device, { .vertex = &passthrough_vert_shader, .fragment = &passthrough_frag_shader, .render_pass = with_depth, .textures_count = 3 })));

    r_destroy_render_pass(device, with_depth);
    r_destroy_render_pass(device, color_only);
    r_destroy_image(device, small);
    r_destroy_image(device, depth);
    r_destroy_image(device, color);
}

static std::string backend_name (const testing::TestParamInfo<R_Backend>& info)
{
    switch (info.param)
    {
    case R_Backend::Vulkan:
        return "Vulkan";
    case R_Backend::OpenGL:
        return "OpenGL";
    case R_Backend::OpenGLES:
        return "OpenGLES";
    case R_Backend::D3D11:
        return "D3D11";
    case R_Backend::D3D12:
        return "D3D12";
    case R_Backend::Metal:
        return "Metal";
    case R_Backend::Count:
        break;
    }
    return "Unknown";
}

INSTANTIATE_TEST_SUITE_P(Backends, RenderTest, testing::Values(R_Backend::Vulkan, R_Backend::OpenGL, R_Backend::OpenGLES, R_Backend::D3D11, R_Backend::D3D12, R_Backend::Metal), backend_name);
INSTANTIATE_TEST_SUITE_P(Backends, RenderDepthTest, testing::Values(R_Backend::Vulkan, R_Backend::OpenGL, R_Backend::OpenGLES, R_Backend::D3D11, R_Backend::D3D12, R_Backend::Metal), backend_name);

TEST(RenderGLXTest, a_glx_app_without_a_config_gets_it_from_its_context)
{
    TestGLXApp app = {};
    defer { app.destroy(); };
    if (!app.create())
    {
        GTEST_SKIP() << "no X display with GLX";
    }
    R_Device* device = r_create_device(app.info(false));
    ASSERT_NE(device, nullptr) << "Godot's Xlib binding has no glxFBConfig";
    EXPECT_EQ(r_gl_x11_visual(device), app.visual);
    r_destroy_device(device);
}
