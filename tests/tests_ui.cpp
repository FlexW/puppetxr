#include "ui.h"

#include <gtest/gtest.h>
#include <volk.h>

#include "test_vulkan.h"
#include "test_gl.h"
#include "test_d3d.h"
#include "test_metal.h"
#include <stdlib.h>

namespace
{

// The UI renders into an offscreen image here, on every backend; the offscreen video driver provides the window
struct UiTest : testing::TestWithParam<R_Backend>
{
    VkInstance       instance = VK_NULL_HANDLE;
    TestValidation   validation = {};
    VkDevice         vk_device = VK_NULL_HANDLE;
    TestGLApp        gl_app   = {};
    TestD3DApp       d3d_app  = {};
    TestMetalApp     metal_app = {};
    R_Device*        device   = nullptr;
    OS_Window*       window   = nullptr;
    SIM_Sim*         sim      = nullptr;

    void SetUp () override
    {
        os_set_assert_mode(OS_AssertMode::Log);
        os_set_env("PUPPETXR_VIDEO_DRIVER", "offscreen");
        os_shutdown();
        ASSERT_TRUE(os_initialize(false));
        if (os_is_headless())
        {
            GTEST_SKIP() << "no offscreen video driver";
        }
        if (GetParam() == R_Backend::Vulkan)
        {
            create_vulkan();
        }
        else if (GetParam() == R_Backend::D3D11 || GetParam() == R_Backend::D3D12)
        {
            device = d3d_app.create(GetParam() == R_Backend::D3D12) ? r_create_device(d3d_app.info()) : nullptr;
        }
        else if (GetParam() == R_Backend::Metal)
        {
            device = metal_app.create() ? r_create_device(metal_app.info()) : nullptr;
        }
        else if (gl_app.create(GetParam() == R_Backend::OpenGLES))
        {
            device = r_create_device(gl_app.info());
        }
        if (!device)
        {
            GTEST_SKIP() << "no device of this backend";
        }
        window = os_create_window("ui test", 640, 480, false);
        ASSERT_NE(window, nullptr);

        SIM_Config config = {};
        sim_default_config(config);
        config.clock_mode = SIM_ClockMode::FixedStep;
        sim = sim_create(config);
    }

    void create_vulkan ()
    {
        if (volkInitialize() != VK_SUCCESS)
        {
            return;
        }

        const VkApplicationInfo    app_info      = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "puppetxr_tests", .apiVersion = VK_API_VERSION_1_1 };
        VkInstanceCreateInfo       instance_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app_info };
        const char*                instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME };
        test_validation_prepare(validation, instance_info, instance_extensions, 1);
        if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
        {
            instance = VK_NULL_HANDLE;
            return;
        }
        volkLoadInstanceOnly(instance);
        test_validation_attach(validation, instance);
        u32 count = 1;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        vkEnumeratePhysicalDevices(instance, &count, &physical);
        if (!physical)
        {
            return;
        }
        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
        VkQueueFamilyProperties families[16] = {};
        family_count = family_count > 16 ? 16 : family_count;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
        u32 family = 0;
        while (family < family_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            family++;
        }
        ASSERT_LT(family, family_count);
        const f32 priority = 1.0f;
        const VkDeviceQueueCreateInfo queue_info  = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
        // The UI renders into window swapchain images, so the runtime's device always has VK_KHR_swapchain
        const char*                   device_extensions[2] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        const u32                     device_extensions_count = test_portability_device_extensions(physical, device_extensions, 1);
        const VkDeviceCreateInfo      device_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info, .enabledExtensionCount = device_extensions_count, .ppEnabledExtensionNames = device_extensions };
        ASSERT_EQ(vkCreateDevice(physical, &device_info, nullptr, &vk_device), VK_SUCCESS);
        volkLoadDevice(vk_device);

        device = r_create_device({ .backend = R_Backend::Vulkan, .vulkan = { .get_instance_proc_addr = (void*)vkGetInstanceProcAddr, .instance = instance, .physical_device = physical, .device = vk_device, .queue_family_index = family } });
    }

    // GL has no BGRA images
    R_Format target_format () const
    {
        return GetParam() == R_Backend::OpenGL || GetParam() == R_Backend::OpenGLES ? R_Format::R8G8B8A8_SRGB : R_Format::B8G8R8A8_SRGB;
    }

    void TearDown () override
    {
        if (sim)
        {
            sim_destroy(sim);
        }
        os_destroy_window(window);
        if (device)
        {
            r_destroy_device(device);
        }
        if (vk_device)
        {
            vkDestroyDevice(vk_device, nullptr);
        }
        if (instance)
        {
            test_validation_detach(validation, instance);
            vkDestroyInstance(instance, nullptr);
        }
        gl_app.destroy();
        d3d_app.destroy();
        metal_app.destroy();
        os_shutdown();
        EXPECT_EQ(test_validation_errors, 0u);
        EXPECT_EQ(test_gl_errors, 0u);
        os_unset_env("PUPPETXR_VIDEO_DRIVER");
    }
};

static void ui_event_hook (const OS_RawEvent& event, void* user)
{
    ui_handle_raw_event((const UI_Context*)user, event);
}

// Drives the panels as a person does: mouse events into the window, one UI frame after each,
// widgets found by their labels in the frame before
struct UiClicker
{
    UiTest&        test;
    UI_Context*    ui     = nullptr;
    OS_Window*     window = nullptr;
    R_ImageHandle  target = {};
    SIM_MirrorView mirror = SIM_MirrorView::Left;
    UI_Requests    requests = {};
    u32            frames = 0;
    b8             controller_requested = false; // a frame asked for another controller type, which the runtime applies
    SIM_Controller requested_controller = {};

    UiClicker (UiTest& owner, u32 width, u32 height) : test(owner)
    {
        window = os_create_window("ui clicks", width, height, false);
        EXPECT_NE(window, nullptr);
        ui = ui_create({ .device = test.device, .window = window, .target_format = test.target_format() });
        EXPECT_NE(ui, nullptr);
        target = r_create_image(test.device, { .width = width, .height = height, .format = test.target_format(), .usage_flags = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::TransferSrc, .initial_layout = R_ImageLayout::TransferDst });
        os_set_raw_event_hook(ui_event_hook, ui);
        ui_set_probe(ui, true);
    }

    ~UiClicker ()
    {
        os_set_raw_event_hook(nullptr, nullptr);
        r_destroy_image(test.device, target);
        ui_destroy(ui);
        os_destroy_window(window);
    }

    void frame (const UI_Status& status = { .room_from_stage = zm::POSE_IDENTITY })
    {
        OS_Event events[64];
        os_poll_events(events, ARRAY_SIZE(events));
        R_CmdBufferHandle cmd = {};
        if (frames > 0)
        {
            cmd = r_begin_immediate(test.device);
            r_cmd_transition_image(test.device, cmd, target, R_ImageLayout::Present, R_ImageLayout::TransferDst);
            r_end_immediate(test.device, cmd);
        }
        cmd      = r_begin_frame(test.device);
        requests = ui_draw(ui, test.sim, cmd, target, true, mirror, status);
        r_end_frame(test.device, cmd);
        if (requests.set_controller)
        {
            controller_requested = true;
            requested_controller = requests.controller;
        }
        r_wait_idle(test.device);
        frames++;
    }

    b8 click (const char* label)
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        if (!ui_find_item(ui, label, x, y))
        {
            return false;
        }
        os_push_mouse_event(window, x, y, OS_Key::None, false);
        frame();
        os_push_mouse_event(window, x, y, OS_Key::MouseLeft, true);
        frame();
        os_push_mouse_event(window, x, y, OS_Key::MouseLeft, false);
        frame();
        // A popup opened by the release shows its items from the frame after
        frame();
        return true;
    }

    // Presses at a fraction of the item's rectangle, keeps it pressed for a frame and calls back
    // before the release, so a test sees what holding it does
    template <typename F>
    b8 hold (const char* label, f32 fx, F&& while_held)
    {
        f32 x0 = 0.0f;
        f32 y0 = 0.0f;
        f32 x1 = 0.0f;
        f32 y1 = 0.0f;
        if (!ui_find_item_rect(ui, label, x0, y0, x1, y1))
        {
            return false;
        }
        const f32 x = x0 + (x1 - x0) * fx;
        const f32 y = (y0 + y1) * 0.5f;
        os_push_mouse_event(window, x, y, OS_Key::None, false);
        frame();
        os_push_mouse_event(window, x, y, OS_Key::MouseLeft, true);
        frame();
        frame();
        while_held();
        os_push_mouse_event(window, x, y, OS_Key::MouseLeft, false);
        frame();
        frame();
        return true;
    }
};

} // namespace

// Windows may give the window clicks without the moves before them (seen in Unity's editor): a
// click acts where it happens, not where the last move was
TEST_P(UiTest, clicks_act_where_they_happen_without_moves)
{
    UiClicker clicker(*this, 1280, 900);
    clicker.frame();
    clicker.frame();
    f32 x = 0.0f;
    f32 y = 0.0f;
    ASSERT_TRUE(ui_find_item(clicker.ui, "add keyboard", x, y));
    os_push_mouse_event(clicker.window, 5.0f, 5.0f, OS_Key::None, false);
    clicker.frame();
    os_push_mouse_event(clicker.window, x, y, OS_Key::MouseLeft, true);
    clicker.frame();
    os_push_mouse_event(clicker.window, x, y, OS_Key::MouseLeft, false);
    clicker.frame();
    clicker.frame();
    const SIM_Prop* props = nullptr;
    EXPECT_EQ(sim_props(sim, props), 1u) << "the button was clicked";
}

// The motion of a prop is picked from a drop-down; picking one keeps the panel open and edits the prop
TEST_P(UiTest, prop_motion_is_picked_from_its_drop_down)
{
    UiClicker clicker(*this, 1280, 900);
    SIM_Prop  qr = { .kind = SIM_PropKind::QrCode, .pose = { { 0.0f, 1.5f, -1.0f }, zm::QUAT_IDENTITY }, .size = 0.15f };
    const u32 id = sim_add_prop(sim, qr);
    ASSERT_NE(id, 0u);
    clicker.frame();
    clicker.frame();

    ASSERT_TRUE(clicker.click("motion"));
    ASSERT_TRUE(clicker.click("circle")) << "the drop-down shows the motions";
    clicker.frame();
    const SIM_Prop* props = nullptr;
    ASSERT_EQ(sim_props(sim, props), 1u);
    EXPECT_EQ(props[0].motion, SIM_PropMotion::Circle);
    f32 x = 0.0f;
    f32 y = 0.0f;
    EXPECT_TRUE(ui_find_item(clicker.ui, "radius", x, y)) << "the prop's editor stays open";
}

// Every input the Devices panel shows for a controller does what its label says: a button is
// pressed while held, a slider sets its value, a stick both axes
TEST_P(UiTest, every_controller_input_of_the_devices_panel_reaches_the_simulator)
{
    UiClicker clicker(*this, 1400, 1500);
    clicker.frame();
    clicker.frame();
    f32 x = 0.0f;
    f32 y = 0.0f;
    ASSERT_TRUE(ui_find_item(clicker.ui, "active", x, y)) << "the Devices panel shows";
    const SIM_Controller controllers[] = { SIM_Controller::MetaTouchPlus, SIM_Controller::MetaTouchPro, SIM_Controller::OculusTouch, SIM_Controller::KhrSimple };
    const struct { const char* tab; SIM_Device device; } hands[] = { { "Left", SIM_Device::LeftHand }, { "Right", SIM_Device::RightHand } };
    for (SIM_Controller controller : controllers)
    {
        sim_set_controller(sim, controller);
        clicker.frame();
        for (const auto& hand : hands)
        {
            ASSERT_TRUE(clicker.click(hand.tab));
            for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
            {
                const SIM_Input input = (SIM_Input)i;
                if (!sim_hand_has_input(SIM_HandSource::Controller, controller, hand.device, input))
                {
                    continue;
                }
                const char* name = sim_input_name(input);
                SCOPED_TRACE(testing::Message() << sim_controller_name(controller) << " " << hand.tab << " " << name);
                auto value = [&] () { return sim_get_input(sim, hand.device, input).value; };
                switch (sim_input_type(input))
                {
                case SIM_InputType::Bool:
                    EXPECT_TRUE(clicker.hold(name, 0.5f, [&] () { EXPECT_EQ(value().x, 1.0f) << "pressed while held"; })) << "shown";
                    EXPECT_EQ(value().x, 0.0f) << "released";
                    break;
                case SIM_InputType::Float:
                    EXPECT_TRUE(clicker.hold(name, 0.95f, [] () {})) << "shown";
                    EXPECT_GT(value().x, 0.8f);
                    EXPECT_TRUE(clicker.hold(name, 0.02f, [] () {}));
                    EXPECT_LT(value().x, 0.1f);
                    break;
                case SIM_InputType::Vec2:
                    EXPECT_TRUE(clicker.hold(name, 0.47f, [] () {})) << "shown";
                    EXPECT_GT(value().x, 0.7f) << "the first half is x";
                    EXPECT_TRUE(clicker.hold(name, 0.98f, [] () {}));
                    EXPECT_GT(value().y, 0.7f) << "the second half is y";
                    sim_set_input(sim, hand.device, input, {});
                    break;
                }
            }
        }
    }

    // The rest of a hand's controls: whether it is tracked and what tracks it
    sim_set_controller(sim, SIM_Controller::MetaTouchPlus);
    for (const auto& hand : hands)
    {
        SCOPED_TRACE(hand.tab);
        ASSERT_TRUE(clicker.click(hand.tab));
        ASSERT_TRUE(clicker.click("active"));
        EXPECT_FALSE(sim_get_device_state(sim, hand.device).active);
        ASSERT_TRUE(clicker.click("active"));
        EXPECT_TRUE(sim_get_device_state(sim, hand.device).active);

        ASSERT_TRUE(clicker.click("source"));
        ASSERT_TRUE(clicker.click("tracked hand"));
        EXPECT_EQ(sim_get_hand_source(sim, hand.device), SIM_HandSource::Hand);
        for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
        {
            const SIM_Input input = (SIM_Input)i;
            if (!sim_hand_has_input(SIM_HandSource::Hand, SIM_Controller::MetaTouchPlus, hand.device, input))
            {
                continue;
            }
            SCOPED_TRACE(sim_input_name(input));
            if (sim_input_type(input) == SIM_InputType::Float)
            {
                EXPECT_TRUE(clicker.hold(sim_input_name(input), 0.95f, [] () {})) << "shown";
                EXPECT_GT(sim_get_input(sim, hand.device, input).value.x, 0.8f);
                sim_set_input(sim, hand.device, input, {});
            }
            else
            {
                // A microgesture is a pulse that the next frame ends, so it is seen while held
                b8 seen = false;
                EXPECT_TRUE(clicker.hold(sim_input_name(input), 0.5f, [&] () { seen = sim_get_input(sim, hand.device, input).value.x == 1.0f; })) << "shown";
                EXPECT_TRUE(seen);
            }
        }
        ASSERT_TRUE(clicker.click("source"));
        ASSERT_TRUE(clicker.click("controller"));
        EXPECT_EQ(sim_get_hand_source(sim, hand.device), SIM_HandSource::Controller);
    }

    const struct { const char* item; SIM_Controller controller; } types[] = {
        { "Oculus Touch", SIM_Controller::OculusTouch }, { "KHR simple", SIM_Controller::KhrSimple },
        { "Meta Touch Pro", SIM_Controller::MetaTouchPro }, { "Meta Touch Plus", SIM_Controller::MetaTouchPlus },
    };
    for (const auto& type : types)
    {
        SCOPED_TRACE(type.item);
        clicker.controller_requested = false;
        ASSERT_TRUE(clicker.click("type"));
        ASSERT_TRUE(clicker.click(type.item));
        EXPECT_TRUE(clicker.controller_requested || sim_get_controller(sim) == type.controller);
        EXPECT_EQ(clicker.controller_requested ? clicker.requested_controller : sim_get_controller(sim), type.controller);
        sim_set_controller(sim, type.controller);
        clicker.frame();
    }
}

TEST_P(UiTest, draws_panels_into_target)
{
    UI_Context* ui = ui_create({ .device = device, .window = window, .target_format = target_format() });
    ASSERT_NE(ui, nullptr);
    defer { ui_destroy(ui); };

    const R_ImageHandle target = r_create_image(device, { .width = 640, .height = 480, .format = target_format(), .usage_flags = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::TransferSrc, .initial_layout = R_ImageLayout::TransferDst });
    ASSERT_TRUE(r_is_valid(device, target));

    // Clear through a transfer so the panel pixels are distinguishable from the background
    const R_BufferHandle fill = r_create_buffer(device, { .size = (usize)640 * 480 * 4, .usage_flags = (u32)R_BufferUsageFlags::TransferSrc, .host_visible = true });
    memory_zero(r_get_buffer_mapped_data(device, fill), (usize)640 * 480 * 4);
    r_flush_buffer(device, fill, 0, (u64)640 * 480 * 4);
    R_CmdBufferHandle cmd = r_begin_immediate(device);
    r_cmd_copy_buffer_to_image(device, cmd, fill, target, 0);
    r_end_immediate(device, cmd);

    EXPECT_FALSE(ui_wants_mouse(ui));
    EXPECT_FALSE(ui_wants_keyboard(ui));

    // Auto-sized windows are hidden on their first frame while ImGui measures them
    cmd = r_begin_frame(device);
    SIM_MirrorView  mirror = SIM_MirrorView::Left;
    const char      rooms[2][W_MAX_NAME] = { "bedroom", "office" };
    const UI_Status status = { .recording = true, .recorded_frames = 12, .record_path = "/tmp/a.xrrec", .rooms = rooms, .rooms_count = 2, .room = "office", .anchors_stored = 3, .room_from_stage = zm::POSE_IDENTITY, .keyboard_exists = true, .keyboard_visible = true, .keyboard_context = "hello" };
    SIM_Prop qr = { .kind = SIM_PropKind::QrCode, .motion = SIM_PropMotion::Circle, .pose = zm::POSE_IDENTITY, .radius = 0.3f, .speed = 0.1f, .size = 0.1f };
    cstr_copy(qr.text, sizeof(qr.text), "hello");
    ASSERT_NE(sim_add_prop(sim, qr), 0u);
    ASSERT_NE(sim_add_prop(sim, { .kind = SIM_PropKind::Keyboard, .pose = zm::POSE_IDENTITY }), 0u);
    ui_draw(ui, sim, cmd, target, true, mirror, status);
    r_end_frame(device, cmd);
    r_wait_idle(device);
    cmd = r_begin_frame(device);
    r_cmd_transition_image(device, cmd, target, R_ImageLayout::Present, R_ImageLayout::TransferDst);
    const UI_Requests requests = ui_draw(ui, sim, cmd, target, true, mirror, status);
    r_end_frame(device, cmd);
    r_wait_idle(device);
    EXPECT_FALSE(requests.screenshot);
    EXPECT_FALSE(requests.quit);
    EXPECT_FALSE(requests.record_toggle);
    EXPECT_FALSE(requests.set_room);
    EXPECT_FALSE(requests.wipe_anchors);

    u8* pixels = (u8*)os_memory_alloc((usize)640 * 480 * 4);
    defer { os_memory_release(pixels); };
    ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::Present, pixels, (usize)640 * 480 * 4));

    u32 lit = 0;
    for (u32 i = 0; i < 640 * 480; i++)
    {
        lit += pixels[(usize)i * 4 + 3] != 0 && (pixels[(usize)i * 4] | pixels[(usize)i * 4 + 1] | pixels[(usize)i * 4 + 2]) != 0;
    }
    EXPECT_GT(lit, 1000u);

    // PUPPETXR_TEST_DUMP_DIR=<dir> writes the frame as raw BGRA for eyeballing the panels
    if (os_env("PUPPETXR_TEST_DUMP_DIR"))
    {
        char path[OS_MAX_PATH];
        cstr_format(path, sizeof(path), "%s/ui_640x480.bgra", os_env("PUPPETXR_TEST_DUMP_DIR"));
        os_write_file(path, pixels, (usize)640 * 480 * 4);
    }
    EXPECT_EQ(pixels[((usize)479 * 640 + 320) * 4], 0);

    // With the panels hidden only the replay badge is drawn, in the top-right corner
    const R_CmdBufferHandle clear = r_begin_immediate(device);
    r_cmd_transition_image(device, clear, target, R_ImageLayout::Present, R_ImageLayout::TransferDst);
    r_cmd_copy_buffer_to_image(device, clear, fill, target, 0);
    r_end_immediate(device, clear);
    cmd = r_begin_frame(device);
    const UI_Status replay = { .replaying = true, .replay_frame = 5, .replay_total = 10, .divergences = 1, .first_divergence = 3 };
    ui_draw(ui, sim, cmd, target, false, mirror, replay);
    r_end_frame(device, cmd);
    r_wait_idle(device);
    ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::Present, pixels, (usize)640 * 480 * 4));
    u32 badge = 0;
    u32 elsewhere = 0;
    for (u32 y = 0; y < 480; y++)
    {
        for (u32 x = 0; x < 640; x++)
        {
            const b8 drawn = pixels[(y * 640 + x) * 4 + 3] != 0;
            badge     += drawn && x > 540 && y < 30;
            elsewhere += drawn && !(x > 540 && y < 30);
        }
    }
    EXPECT_GT(badge, 20u);
    EXPECT_EQ(elsewhere, 0u);

    r_destroy_buffer(device, fill);
    r_destroy_image(device, target);
}

// The welcome overlay dims the whole window, with or without the panels
// Draws the welcome overlay three frames over a target filled with `fill` and reads it back
static void draw_welcome (R_Device* device, R_Format format, SIM_Sim* sim, u32 width, u32 height, u8 fill, u8* out_pixels)
{
    const usize bytes = (usize)width * height * 4;
    OS_Window*  sized = os_create_window("welcome test", width, height, false);
    ASSERT_NE(sized, nullptr);
    defer { os_destroy_window(sized); };
    UI_Context* ui = ui_create({ .device = device, .window = sized, .target_format = format });
    ASSERT_NE(ui, nullptr);
    defer { ui_destroy(ui); };
    const R_ImageHandle target = r_create_image(device, { .width = width, .height = height, .format = format, .usage_flags = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::TransferSrc, .initial_layout = R_ImageLayout::TransferDst });
    ASSERT_TRUE(r_is_valid(device, target));
    defer { r_destroy_image(device, target); };
    const R_BufferHandle buffer = r_create_buffer(device, { .size = bytes, .usage_flags = (u32)R_BufferUsageFlags::TransferSrc, .host_visible = true });
    defer { r_destroy_buffer(device, buffer); };
    memset(r_get_buffer_mapped_data(device, buffer), fill, bytes);
    r_flush_buffer(device, buffer, 0, bytes);

    SIM_MirrorView  mirror = SIM_MirrorView::Left;
    const UI_Status status = { .room_from_stage = zm::POSE_IDENTITY, .show_welcome = true };
    for (u32 frame = 0; frame < 3; frame++)
    {
        R_CmdBufferHandle cmd = r_begin_immediate(device);
        if (frame > 0)
        {
            r_cmd_transition_image(device, cmd, target, R_ImageLayout::Present, R_ImageLayout::TransferDst);
        }
        r_cmd_copy_buffer_to_image(device, cmd, buffer, target, 0);
        r_end_immediate(device, cmd);
        cmd = r_begin_frame(device);
        const UI_Requests requests = ui_draw(ui, sim, cmd, target, false, mirror, status);
        r_end_frame(device, cmd);
        r_wait_idle(device);
        EXPECT_FALSE(requests.toggle_welcome);
        EXPECT_FALSE(requests.set_welcome_at_start);
    }
    ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::Present, out_pixels, bytes));
    if (os_env("PUPPETXR_TEST_DUMP_DIR"))
    {
        char path[OS_MAX_PATH];
        cstr_format(path, sizeof(path), "%s/ui_welcome_%ux%u_%u.bgra", os_env("PUPPETXR_TEST_DUMP_DIR"), width, height, fill);
        os_write_file(path, out_pixels, bytes);
    }
}

TEST_P(UiTest, welcome_overlay_covers_the_window)
{
    // A small window and the default one; the footer with the keys that close it must show in both
    const u32 sizes[2][2] = { { 640, 480 }, { 1280, 720 } };
    for (const u32* size : sizes)
    {
        const u32   width  = size[0];
        const u32   height = size[1];
        const usize bytes  = (usize)width * height * 4;
        u8*         pixels = (u8*)os_memory_alloc(bytes);
        defer { os_memory_release(pixels); };
        draw_welcome(device, target_format(), sim, width, height, 0, pixels);

        u32 dimmed = 0;
        u32 bright = 0;
        u32 footer = 0;
        for (u32 y = 0; y < height; y++)
        {
            for (u32 x = 0; x < width; x++)
            {
                const u8* p   = pixels + ((usize)y * width + x) * 4;
                const b8  lit = p[0] > 160 && p[1] > 160 && p[2] > 160;
                dimmed += (p[0] | p[1] | p[2]) != 0;
                bright += lit;
                footer += lit && y > height / 4 * 3;
            }
        }
        EXPECT_GT(dimmed, width * height / 10 * 9);
        EXPECT_GT(bright, 500u);
        EXPECT_GT(footer, 50u);
        EXPECT_NE(pixels[0] | pixels[1] | pixels[2], 0);
        EXPECT_NE(pixels[bytes - 4], 0);
    }
}

// The keys of the help are found like the panels' items, by puppetxrctl's `ui find` and tests
TEST_P(UiTest, help_names_its_keys)
{
    UI_Context* ui = ui_create({ .device = device, .window = window, .target_format = target_format() });
    ASSERT_NE(ui, nullptr);
    defer { ui_destroy(ui); };
    ui_set_probe(ui, true);
    const R_ImageHandle target = r_create_image(device, { .width = 640, .height = 480, .format = target_format(), .usage_flags = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::TransferSrc, .initial_layout = R_ImageLayout::TransferDst });
    defer { r_destroy_image(device, target); };
    SIM_MirrorView  mirror = SIM_MirrorView::Left;
    const UI_Status status = { .room_from_stage = zm::POSE_IDENTITY, .show_welcome = true };
    for (u32 frame = 0; frame < 3; frame++)
    {
        R_CmdBufferHandle cmd = r_begin_immediate(device);
        if (frame > 0)
        {
            r_cmd_transition_image(device, cmd, target, R_ImageLayout::Present, R_ImageLayout::TransferDst);
        }
        r_end_immediate(device, cmd);
        cmd = r_begin_frame(device);
        ui_draw(ui, sim, cmd, target, false, mirror, status);
        r_end_frame(device, cmd);
        r_wait_idle(device);
    }
    for (const char* label : { "click the view", "F1", "Enter" })
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        EXPECT_TRUE(ui_find_item(ui, label, x, y)) << label;
    }
}

// ImGui works in points and a window of high pixel density has more pixels than points: the
// panels clipped to the top-left quarter of a Retina window
TEST_P(UiTest, panels_fill_windows_of_high_pixel_density)
{
    os_destroy_window(window);
    window = nullptr;
    os_shutdown();
    os_unset_env("PUPPETXR_VIDEO_DRIVER");
    ASSERT_TRUE(os_initialize(false));
    if (os_is_headless())
    {
        GTEST_SKIP() << "no display";
    }
    OS_Window* dense = os_create_window("density test", 320, 240, false, 0, true);
    ASSERT_NE(dense, nullptr);
    defer { os_destroy_window(dense); };
    if (os_window_display_density(dense) <= 1.0f)
    {
        GTEST_SKIP() << "the display has one pixel per point";
    }
    u32 width  = 0;
    u32 height = 0;
    os_window_size(dense, width, height);
    EXPECT_GT(width, 320u);
    EXPECT_GT(height, 240u);

    UI_Context* ui = ui_create({ .device = device, .window = dense, .target_format = target_format() });
    ASSERT_NE(ui, nullptr);
    defer { ui_destroy(ui); };
    const usize         bytes  = (usize)width * height * 4;
    const R_ImageHandle target = r_create_image(device, { .width = width, .height = height, .format = target_format(), .usage_flags = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::TransferSrc, .initial_layout = R_ImageLayout::TransferDst });
    defer { r_destroy_image(device, target); };
    const R_BufferHandle buffer = r_create_buffer(device, { .size = bytes, .usage_flags = (u32)R_BufferUsageFlags::TransferSrc, .host_visible = true });
    defer { r_destroy_buffer(device, buffer); };
    memset(r_get_buffer_mapped_data(device, buffer), 0, bytes);
    r_flush_buffer(device, buffer, 0, bytes);

    SIM_MirrorView  mirror = SIM_MirrorView::Left;
    const UI_Status status = { .room_from_stage = zm::POSE_IDENTITY, .show_welcome = true };
    for (u32 frame = 0; frame < 3; frame++)
    {
        R_CmdBufferHandle cmd = r_begin_immediate(device);
        if (frame > 0)
        {
            r_cmd_transition_image(device, cmd, target, R_ImageLayout::Present, R_ImageLayout::TransferDst);
        }
        r_cmd_copy_buffer_to_image(device, cmd, buffer, target, 0);
        r_end_immediate(device, cmd);
        cmd = r_begin_frame(device);
        ui_draw(ui, sim, cmd, target, false, mirror, status);
        r_end_frame(device, cmd);
        r_wait_idle(device);
    }
    u8* pixels = (u8*)os_memory_alloc(bytes);
    defer { os_memory_release(pixels); };
    ASSERT_TRUE(r_read_image(device, target, 0, R_ImageLayout::Present, pixels, bytes));

    // The overlay's backdrop dims the whole window, its bottom-right corner too
    u32 dimmed = 0;
    for (usize i = 0; i < (usize)width * height; i++)
    {
        dimmed += (pixels[i * 4] | pixels[i * 4 + 1] | pixels[i * 4 + 2]) != 0;
    }
    EXPECT_GT(dimmed, width * height / 10 * 9);
    EXPECT_NE(pixels[bytes - 4] | pixels[bytes - 3] | pixels[bytes - 2], 0);
}

// Over a bright app the see-through backdrop let so much through that the grey descriptions
// were hard to read: the backdrop stays dark and the descriptions are light
TEST_P(UiTest, welcome_overlay_reads_over_a_white_scene)
{
    const u32   width  = 1280;
    const u32   height = 720;
    const usize bytes  = (usize)width * height * 4;
    u8*         pixels = (u8*)os_memory_alloc(bytes);
    defer { os_memory_release(pixels); };
    draw_welcome(device, target_format(), sim, width, height, 255, pixels);
    const u8* corner = pixels;
    EXPECT_LT(corner[0] + corner[1] + corner[2], 3 * 75) << "the backdrop over white";
    // The descriptions column of the left table, from its first row down
    u32 light = 0;
    for (u32 y = height / 4; y < height / 2; y++)
    {
        for (u32 x = width / 4; x < width / 2; x++)
        {
            const u8* p = pixels + ((usize)y * width + x) * 4;
            light += p[0] > 165 && p[1] > 165 && p[2] > 165;
        }
    }
    EXPECT_GT(light, 300u) << "light description text";
}

// Switching the profile names it and its keys at the top for a few seconds; the start profile is
// not announced
TEST_P(UiTest, a_new_profile_is_named_at_the_top)
{
    const u32   width  = 1280;
    const u32   height = 720;
    const usize bytes  = (usize)width * height * 4;
    OS_Window*  sized  = os_create_window("toast test", width, height, false);
    ASSERT_NE(sized, nullptr);
    defer { os_destroy_window(sized); };
    UI_Context* ui = ui_create({ .device = device, .window = sized, .target_format = target_format() });
    ASSERT_NE(ui, nullptr);
    defer { ui_destroy(ui); };
    const R_ImageHandle target = r_create_image(device, { .width = width, .height = height, .format = target_format(), .usage_flags = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::TransferSrc, .initial_layout = R_ImageLayout::TransferDst });
    ASSERT_TRUE(r_is_valid(device, target));
    defer { r_destroy_image(device, target); };
    const R_BufferHandle black = r_create_buffer(device, { .size = bytes, .usage_flags = (u32)R_BufferUsageFlags::TransferSrc, .host_visible = true });
    defer { r_destroy_buffer(device, black); };
    memset(r_get_buffer_mapped_data(device, black), 0, bytes);
    r_flush_buffer(device, black, 0, bytes);
    u8* pixels = (u8*)os_memory_alloc(bytes);
    defer { os_memory_release(pixels); };

    SIM_MirrorView  mirror = SIM_MirrorView::Left;
    const UI_Status status = { .room_from_stage = zm::POSE_IDENTITY };
    b8              first  = true;
    // Lit pixels in the top third's middle, after a frame without panels
    const auto lit_at_top = [&]
    {
        R_CmdBufferHandle cmd = r_begin_immediate(device);
        if (!first)
        {
            r_cmd_transition_image(device, cmd, target, R_ImageLayout::Present, R_ImageLayout::TransferDst);
        }
        first = false;
        r_cmd_copy_buffer_to_image(device, cmd, black, target, 0);
        r_end_immediate(device, cmd);
        cmd = r_begin_frame(device);
        ui_draw(ui, sim, cmd, target, false, mirror, status);
        r_end_frame(device, cmd);
        r_wait_idle(device);
        EXPECT_TRUE(r_read_image(device, target, 0, R_ImageLayout::Present, pixels, bytes));
        if (os_env("PUPPETXR_TEST_DUMP_DIR"))
        {
            char path[OS_MAX_PATH];
            cstr_format(path, sizeof(path), "%s/ui_toast_%ux%u.bgra", os_env("PUPPETXR_TEST_DUMP_DIR"), width, height);
            os_write_file(path, pixels, bytes);
        }
        u32 lit = 0;
        for (u32 y = 0; y < height / 3; y++)
        {
            for (u32 x = width / 4; x < width / 4 * 3; x++)
            {
                const u8* pixel = pixels + ((usize)y * width + x) * 4;
                lit += pixel[0] > 120 && pixel[1] > 120 && pixel[2] > 120;
            }
        }
        return lit;
    };
    EXPECT_EQ(lit_at_top(), 0u) << "the start profile is not announced";
    EXPECT_EQ(lit_at_top(), 0u);
    u32 hands = 0;
    ASSERT_TRUE(sim_profile_from_name(sim, "hands", hands));
    sim_set_profile(sim, hands);
    EXPECT_GT(lit_at_top(), 300u) << "the profile's name and keys";
}

// The Simulator panel's Performance tab: the frame rate and a graph of the frame times, so spikes show
TEST_P(UiTest, performance_tab_shows_the_frame_rate_and_the_frame_times)
{
    UI_FrameTimes times = {};
    for (u32 i = 0; i < 120; i++)
    {
        times.frame_ms[times.next] = i == 60 ? 50.0f : 10.0f;
        times.puppetxr_ms[times.next] = 2.0f;
        times.next++;
        times.count++;
    }
    const UI_Status status = { .room_from_stage = zm::POSE_IDENTITY, .frame_times = &times };
    UiClicker       clicker(*this, 1280, 900);
    clicker.frame(status);
    clicker.frame(status);
    f32 x = 0.0f;
    f32 y = 0.0f;
    EXPECT_FALSE(ui_find_item(clicker.ui, "frame times", x, y)) << "the Simulator tab is the first";
    ASSERT_TRUE(clicker.click("Performance"));
    clicker.frame(status);
    clicker.frame(status);
    for (const char* label : { "frame rate", "frame times", "puppetxr", "mirror" })
    {
        EXPECT_TRUE(ui_find_item(clicker.ui, label, x, y)) << label;
    }
    EXPECT_FALSE(ui_find_item(clicker.ui, "screenshot", x, y)) << "the Simulator tab is hidden";
    if (os_env("PUPPETXR_TEST_DUMP_DIR"))
    {
        const usize bytes  = (usize)1280 * 900 * 4;
        u8*         pixels = (u8*)os_memory_alloc(bytes);
        defer { os_memory_release(pixels); };
        ASSERT_TRUE(r_read_image(device, clicker.target, 0, R_ImageLayout::Present, pixels, bytes));
        char path[OS_MAX_PATH];
        cstr_format(path, sizeof(path), "%s/ui_performance_1280x900.bgra", os_env("PUPPETXR_TEST_DUMP_DIR"));
        os_write_file(path, pixels, bytes);
    }
}

// The Body tab edits the body as the control API does, draws the skeleton front and side, and
// its toggle draws the joints over the mirror view
TEST_P(UiTest, body_tab_shapes_poses_and_draws_the_body)
{
    UiClicker clicker(*this, 1280, 1400);
    clicker.frame();
    clicker.frame();
    ASSERT_TRUE(clicker.click("Body"));
    f32 x = 0.0f;
    f32 y = 0.0f;
    EXPECT_TRUE(ui_find_item(clicker.ui, "skeleton front and side", x, y));

    EXPECT_TRUE(clicker.hold("height", 0.999f, [] () {}));
    EXPECT_GT(sim_get_body(sim).height, 2.4f);
    ASSERT_TRUE(clicker.click("pose"));
    ASSERT_TRUE(clicker.click("t_pose"));
    EXPECT_TRUE(zm::equal(sim_get_body(sim).rotations[(u32)SIM_BodyJoint::LeftArmUpper], zm::QUAT_IDENTITY));
    ASSERT_TRUE(clicker.click("left arm posed"));
    EXPECT_EQ(sim_get_body(sim).limbs[(u32)SIM_Limb::LeftArm], SIM_LimbMode::Posed);
    ASSERT_TRUE(clicker.click("joint"));
    ASSERT_TRUE(clicker.click("neck"));
    ASSERT_TRUE(clicker.click("tracked"));
    EXPECT_FALSE(sim_get_body(sim).tracked[(u32)SIM_BodyJoint::Neck]);
    ASSERT_TRUE(clicker.click("Bone scales"));
    EXPECT_TRUE(clicker.hold("left_arm_upper", 0.999f, [] () {}));
    EXPECT_FLOAT_EQ(sim_get_body(sim).bone_scales[(u32)SIM_BodyJoint::LeftArmUpper], SIM_BODY_MAX_SCALE);
    EXPECT_FLOAT_EQ(sim_get_body(sim).bone_scales[(u32)SIM_BodyJoint::RightArmUpper], SIM_BODY_MAX_SCALE) << "sides are linked";

    // Looking down at the feet, the toggle draws the joints over the view
    sim_set_device_pose(sim, SIM_Device::Head, { { 0.0f, 1.6f, 0.0f }, zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -1.2f) });
    clicker.frame();
    const usize bytes  = (usize)1280 * 1400 * 4;
    u8*         before = (u8*)os_memory_alloc(bytes);
    u8*         after  = (u8*)os_memory_alloc(bytes);
    defer { os_memory_release(before); os_memory_release(after); };
    ASSERT_TRUE(r_read_image(device, clicker.target, 0, R_ImageLayout::Present, before, bytes));
    ASSERT_TRUE(clicker.click("draw skeleton"));
    ASSERT_TRUE(r_read_image(device, clicker.target, 0, R_ImageLayout::Present, after, bytes));
    u32 changed = 0;
    for (usize i = 0; i < bytes; i += 4)
    {
        changed += memory_equal(before + i, after + i, 4) ? 0u : 1u;
    }
    EXPECT_GT(changed, 1000u) << "the skeleton's lines";
    if (os_env("PUPPETXR_TEST_DUMP_DIR"))
    {
        char path[OS_MAX_PATH];
        cstr_format(path, sizeof(path), "%s/ui_body_1280x1400.bgra", os_env("PUPPETXR_TEST_DUMP_DIR"));
        os_write_file(path, after, bytes);
    }

    ASSERT_TRUE(clicker.click("reset body"));
    EXPECT_FLOAT_EQ(sim_get_body(sim).height, 1.75f);
    EXPECT_TRUE(sim_get_body(sim).tracked[(u32)SIM_BodyJoint::Neck]);
}

// The Face tab edits the face as the control API does
TEST_P(UiTest, face_tab_sets_expression_blinks_and_gaze)
{
    UiClicker clicker(*this, 1280, 1400);
    clicker.frame();
    clicker.frame();
    ASSERT_TRUE(clicker.click("Face"));
    if (os_env("PUPPETXR_TEST_DUMP_DIR"))
    {
        const usize bytes  = (usize)1280 * 1400 * 4;
        u8*         pixels = (u8*)os_memory_alloc(bytes);
        defer { os_memory_release(pixels); };
        ASSERT_TRUE(r_read_image(device, clicker.target, 0, R_ImageLayout::Present, pixels, bytes));
        char path[OS_MAX_PATH];
        cstr_format(path, sizeof(path), "%s/ui_face_1280x1400.bgra", os_env("PUPPETXR_TEST_DUMP_DIR"));
        os_write_file(path, pixels, bytes);
    }
    ASSERT_TRUE(clicker.click("preset"));
    ASSERT_TRUE(clicker.click("smile"));
    EXPECT_GT(sim_get_face(sim).weights[(u32)SIM_FaceExpression::LipCornerPullerL], 0.5f);
    ASSERT_TRUE(clicker.click("blink"));
    EXPECT_FALSE(sim_get_face(sim).blink);
    EXPECT_TRUE(clicker.hold("gaze yaw", 0.999f, [] () {}));
    EXPECT_FLOAT_EQ(sim_get_face(sim).gaze_yaw, SIM_FACE_MAX_GAZE_YAW);
    ASSERT_TRUE(clicker.click("Expressions"));
    EXPECT_TRUE(clicker.hold("jaw_drop", 0.999f, [] () {}));
    EXPECT_FLOAT_EQ(sim_get_face(sim).weights[(u32)SIM_FaceExpression::JawDrop], 1.0f);
    ASSERT_TRUE(clicker.click("reset face"));
    EXPECT_TRUE(sim_get_face(sim).blink);
    EXPECT_EQ(sim_get_face(sim).weights[(u32)SIM_FaceExpression::JawDrop], 0.0f);
}

// Regression: the Simulator panel's hint named Tab whatever key controls.ui was
TEST_P(UiTest, simulator_panel_names_the_configured_panel_key)
{
    SIM_Config config = {};
    sim_default_config(config);
    config.clock_mode = SIM_ClockMode::FixedStep;
    config.key_ui     = OS_Key::F5;
    sim_destroy(sim);
    sim = sim_create(config);
    UiClicker clicker(*this, 1280, 900);
    clicker.frame();
    clicker.frame();
    f32 x = 0.0f;
    f32 y = 0.0f;
    EXPECT_TRUE(ui_find_item(clicker.ui, "F1 help   F5 hide", x, y));
    EXPECT_FALSE(ui_find_item(clicker.ui, "F1 help   Tab hide", x, y));
}

static std::string backend_name (const testing::TestParamInfo<R_Backend>& info)
{
    static const char* const names[] = { "Vulkan", "OpenGL", "OpenGLES", "D3D11", "D3D12", "Metal" };
    static_assert(ARRAY_SIZE(names) == (usize)R_Backend::Count, "a backend has no name");
    return (u32)info.param < ARRAY_SIZE(names) ? names[(u32)info.param] : "Unknown";
}

INSTANTIATE_TEST_SUITE_P(Backends, UiTest, testing::Values(R_Backend::Vulkan, R_Backend::OpenGL, R_Backend::OpenGLES, R_Backend::D3D11, R_Backend::D3D12, R_Backend::Metal), backend_name);

// The numbers of the Performance tab, over the last second of frames
TEST(UiFrameStats, cover_the_last_second_of_frames)
{
    UI_FrameTimes times = {};
    EXPECT_EQ(ui_frame_stats(times).fps, 0.0f) << "no frames";
    // 200 frames of 20 ms, then 100 of 10 ms: the last second is the 100 and 0 of the older
    for (u32 i = 0; i < 300; i++)
    {
        times.frame_ms[times.next] = i < 200 ? 20.0f : 10.0f;
        times.puppetxr_ms[times.next] = i < 200 ? 5.0f : 1.0f;
        times.next  = (times.next + 1) % UI_FRAME_TIMES;
        times.count = zm::min(times.count + 1, (u32)UI_FRAME_TIMES);
    }
    const UI_FrameStats stats = ui_frame_stats(times);
    EXPECT_FLOAT_EQ(stats.fps, 100.0f);
    EXPECT_FLOAT_EQ(stats.mean_ms, 10.0f);
    EXPECT_FLOAT_EQ(stats.max_ms, 10.0f);
    EXPECT_FLOAT_EQ(stats.puppetxr_mean_ms, 1.0f);

    // A spike in the last second is its slowest frame
    times.frame_ms[(times.next + UI_FRAME_TIMES - 1) % UI_FRAME_TIMES] = 40.0f;
    EXPECT_FLOAT_EQ(ui_frame_stats(times).max_ms, 40.0f);

    // Fewer frames than a second: all of them
    UI_FrameTimes few = {};
    few.frame_ms[0] = 30.0f;
    few.frame_ms[1] = 10.0f;
    few.next        = 2;
    few.count       = 2;
    EXPECT_FLOAT_EQ(ui_frame_stats(few).mean_ms, 20.0f);
    EXPECT_FLOAT_EQ(ui_frame_stats(few).fps, 50.0f);
}
