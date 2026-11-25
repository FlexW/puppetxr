#include "oxr.h"
#include "sim.h"
#include "recorder.h"

#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif
#include <string.h>
#include <string>
#include <volk.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

extern "C" XrResult xrNegotiateLoaderRuntimeInterface (const XrNegotiateLoaderInfo* loader_info, XrNegotiateRuntimeRequest* runtime_request);

//
// The motion recorder (tools/recorder, docs/recorder.md) on the runtime in this process
//

namespace
{

struct ExpectedFrame
{
    b8             active;
    SIM_HandJoints joints;
    f32            pinch;
    f32            grasp;
};

struct RecorderTest : testing::Test
{
    char    out_dir[512] = {};
    RC_App* app          = nullptr;

    void SetUp () override
    {
        os_set_assert_mode(OS_AssertMode::Log);
        os_set_env("PUPPETXR_HEADLESS", "1");
        os_set_env("PUPPETXR_FIXED_STEP", "1");
        if (volkInitialize() != VK_SUCCESS)
        {
            GTEST_SKIP() << "no Vulkan loader";
        }
        (void)snprintf(out_dir, sizeof(out_dir), "%s/recorder_%s", PUPPETXR_TEST_OUTPUT_DIR, testing::UnitTest::GetInstance()->current_test_info()->name());
        os_delete_tree(out_dir);
        ASSERT_TRUE(os_create_directories(out_dir));
    }

    void TearDown () override
    {
        if (app)
        {
            EXPECT_EQ(rc_vulkan_errors(app), 0u) << "validation errors of the recorder's Vulkan use";
        }
        rc_destroy(app);
    }

    static PFN_xrGetInstanceProcAddr negotiate ()
    {
        const XrNegotiateLoaderInfo loader_info = {
            .structType          = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
            .structVersion       = XR_LOADER_INFO_STRUCT_VERSION,
            .structSize          = sizeof(XrNegotiateLoaderInfo),
            .minInterfaceVersion = 1,
            .maxInterfaceVersion = 1,
            .minApiVersion       = XR_MAKE_VERSION(1, 0, 0),
            .maxApiVersion       = XR_MAKE_VERSION(1, 1, 999),
        };
        XrNegotiateRuntimeRequest request = { .structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, .structVersion = XR_RUNTIME_INFO_STRUCT_VERSION, .structSize = sizeof(XrNegotiateRuntimeRequest) };
        return XR_SUCCEEDED(xrNegotiateLoaderRuntimeInterface(&loader_info, &request)) ? request.getInstanceProcAddr : nullptr;
    }

    RC_Options options (const char* protocol)
    {
        return { .get_instance_proc_addr = (void*)negotiate(), .out_dir = out_dir, .protocol = protocol, .space = RC_Space::Stage, .passthrough = false, .countdown_seconds = 0.0f, .vulkan_validation = true };
    }

    void write_file (const char* path, const char* text)
    {
        FILE* file = fopen(path, "wb");
        ASSERT_NE(file, nullptr);
        fputs(text, file);
        fclose(file);
    }

    static ExpectedFrame expected_right ()
    {
        const SIM_Sim* sim   = oxr_get_sim();
        SIM_Sample     sample = {};
        sim_sample(sim, sim_predicted_display_time(sim), sample);
        ExpectedFrame out = {};
        out.active        = sample.devices[(u32)SIM_Device::RightHand].active;
        out.pinch         = sim_reported_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue).value.x;
        out.grasp         = sim_reported_input(sim, SIM_Device::RightHand, SIM_Input::SqueezeValue).value.x;
        sim_hand_joints(sim, SIM_Device::RightHand, sample.devices[(u32)SIM_Device::RightHand].pose, SIM_HandMotion::Unobstructed, out.joints);
        return out;
    }

    static f32 number (const JsonValue& array, u32 index)
    {
        f32 out = 0.0f;
        EXPECT_TRUE(json_as_float(json_at(array, index), out));
        return out;
    }

    static f32 field (const JsonValue& object, const char* key)
    {
        f32 out = -1.0f;
        EXPECT_TRUE(json_find_float(object, key, out)) << key;
        return out;
    }

    static std::string text (const JsonValue& object, const char* key)
    {
        char  out[512] = {};
        usize size     = 0;
        EXPECT_TRUE(json_find_string(object, key, out, &size, sizeof(out))) << key;
        return out;
    }
};

} // namespace

TEST_F(RecorderTest, takes_hold_the_joints_the_runtime_reports)
{
    char protocol[600];
    (void)snprintf(protocol, sizeof(protocol), "%s/protocol.json", out_dir);
    write_file(protocol, R"({"steps": [
        {"name": "first", "instruction": "Pinch \"slowly\"", "seconds": 0.1, "hands": "right"},
        {"name": "second", "instruction": "Lose the hand", "seconds": 0.1}
    ]})");
    RC_Options opts = options(protocol);
    opts.automatic  = true;
    app             = rc_create(opts);
    ASSERT_NE(app, nullptr);
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);

    static ExpectedFrame expected[2][64];
    u32                  expected_count[2] = {};
    RC_Status            status            = RC_Status::Running;
    for (u32 frame = 0; frame < 200 && status == RC_Status::Running; frame++)
    {
        sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { { (f32)(frame % 10) / 9.0f, 0.0f } });
        sim_set_device_active(sim, SIM_Device::RightHand, !(rc_takes_count(app) == 1 && frame % 3 == 0));
        status = rc_frame(app);
        if (rc_recording(app))
        {
            const u32 take = rc_takes_count(app);
            ASSERT_LT(take, 2u);
            ASSERT_LT(expected_count[take], 64u);
            expected[take][expected_count[take]++] = expected_right();
        }
    }
    ASSERT_EQ(status, RC_Status::Ended);
    ASSERT_EQ(rc_takes_count(app), 2u);
    EXPECT_NE(strstr(rc_take_path(app, 0), "001_first.json"), nullptr);
    EXPECT_NE(strstr(rc_take_path(app, 1), "002_second.json"), nullptr);

    u32 inactive = 0;
    for (u32 take = 0; take < 2; take++)
    {
        JsonParser* parser = json_parse_from_file(rc_take_path(app, take));
        ASSERT_NE(parser, nullptr) << rc_take_path(app, take);
        const JsonValue root = json_get_root(parser);
        EXPECT_EQ(text(root, "format"), "puppetxr-take");
        EXPECT_EQ(field(root, "version"), 1.0f);
        EXPECT_EQ(text(root, "space"), "stage");
        EXPECT_EQ(text(root, "runtime"), "puppetxr");
        EXPECT_EQ(text(root, "hands"), take == 0 ? "right" : "both");
        EXPECT_EQ(text(root, "instruction"), take == 0 ? "Pinch \"slowly\"" : "Lose the hand");
        EXPECT_GT(field(root, "display_period_ns"), 0.0f);

        const JsonValue frames = json_find(root, "frames");
        ASSERT_EQ(json_length(frames), expected_count[take]);
        EXPECT_GE(expected_count[take], 5u);
        for (u32 f = 0; f < expected_count[take]; f++)
        {
            SCOPED_TRACE(testing::Message() << "take " << take << " frame " << f);
            const JsonValue      frame = json_at(frames, f);
            const ExpectedFrame& want  = expected[take][f];
            EXPECT_EQ(field(json_find(frame, "left"), "active"), 0.0f) << "a hand held by its controller is not unobstructed";
            const JsonValue right = json_find(frame, "right");
            ASSERT_EQ(field(right, "active"), want.active ? 1.0f : 0.0f);
            if (!want.active)
            {
                inactive++;
                continue;
            }
            EXPECT_EQ(text(right, "source"), "unobstructed");
            const JsonValue joints = json_find(right, "joints");
            ASSERT_EQ(json_length(joints), (u32)SIM_HAND_JOINT_COUNT);
            for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
            {
                const JsonValue joint = json_at(joints, j);
                const Pose&     pose  = want.joints.poses[j];
                EXPECT_NEAR(number(joint, 0), pose.position.x, 1e-5f);
                EXPECT_NEAR(number(joint, 1), pose.position.y, 1e-5f);
                EXPECT_NEAR(number(joint, 2), pose.position.z, 1e-5f);
                const f32 sign = number(joint, 6) * pose.orientation.w < 0.0f ? -1.0f : 1.0f;
                EXPECT_NEAR(sign * number(joint, 3), pose.orientation.x, 1e-6f);
                EXPECT_NEAR(sign * number(joint, 6), pose.orientation.w, 1e-6f);
                EXPECT_NEAR(number(joint, 7), want.joints.radii[j], 1e-5f);
            }
            const JsonValue pinch = json_find(json_find(right, "aim"), "pinch");
            EXPECT_NEAR(number(pinch, 0), want.pinch, 1e-4f);
            // The runtime chooses the hand interaction profile at the first xrSyncActions, so the
            // actions are active from the take's second frame on
            const JsonValue interaction = json_find(right, "interaction");
            if (take == 0 && f == 0)
            {
                continue;
            }
            ASSERT_TRUE(json_is_object(interaction)) << "XR_EXT_hand_interaction's values of the frame";
            EXPECT_NEAR(field(interaction, "pinch"), want.pinch, 1e-4f);
            EXPECT_NEAR(field(interaction, "grasp"), want.grasp, 1e-4f);
        }
        json_release(parser);
    }
    EXPECT_GT(inactive, 0u) << "the second take lost the hand on every third frame";
}

TEST_F(RecorderTest, poking_record_starts_a_take_that_ends_after_its_seconds)
{
    char protocol[600];
    (void)snprintf(protocol, sizeof(protocol), "%s/protocol.json", out_dir);
    write_file(protocol, R"({"steps": [{"name": "poked", "instruction": "", "seconds": 0.1}]})");
    app = rc_create(options(protocol));
    ASSERT_NE(app, nullptr);
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);

    f32 record[3] = {};
    for (u32 frame = 0; frame < 20 && !rc_button_center(app, "RECORD", record); frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
    }
    ASSERT_TRUE(rc_button_center(app, "RECORD", record));
    for (u32 frame = 0; frame < 5; frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
        EXPECT_FALSE(rc_recording(app)) << "nothing records before the poke";
    }

    sim_set_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Poke, { { record[0], record[1], record[2] }, zm::QUAT_IDENTITY });
    b8 started = false;
    for (u32 frame = 0; frame < 3 && !started; frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
        started = rc_recording(app);
    }
    ASSERT_TRUE(started);
    sim_set_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Poke, { { record[0], record[1], record[2] + 0.2f }, zm::QUAT_IDENTITY });
    for (u32 frame = 0; frame < 30 && rc_takes_count(app) == 0; frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
    }
    EXPECT_EQ(rc_takes_count(app), 1u);
    EXPECT_FALSE(rc_recording(app));
}

TEST_F(RecorderTest, a_poke_beside_the_buttons_records_nothing)
{
    app = rc_create(options(nullptr));
    ASSERT_NE(app, nullptr);
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    f32 record[3] = {};
    for (u32 frame = 0; frame < 20 && !rc_button_center(app, "RECORD", record); frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
    }
    sim_set_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Poke, { { record[0], record[1] + 0.12f, record[2] }, zm::QUAT_IDENTITY });
    for (u32 frame = 0; frame < 5; frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
        EXPECT_FALSE(rc_recording(app));
    }
}

// The hand's aim ray from 15 cm in front of the panel to a point on it, where hand tracking sees
// the hand; the panel faces +Z
static void aim_at (SIM_Sim* sim, const f32 (&target)[3], f32 dx, f32 dy)
{
    sim_set_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Aim, { { target[0] + dx, target[1] + dy, target[2] + 0.15f }, zm::QUAT_IDENTITY });
}

static void pinch (SIM_Sim* sim, f32 strength)
{
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { { strength, 0.0f } });
}

TEST_F(RecorderTest, pinching_with_the_ray_on_record_starts_a_take)
{
    char protocol[600];
    (void)snprintf(protocol, sizeof(protocol), "%s/protocol.json", out_dir);
    write_file(protocol, R"({"steps": [{"name": "aimed", "instruction": "", "seconds": 0.1}]})");
    app = rc_create(options(protocol));
    ASSERT_NE(app, nullptr);
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    f32 record[3] = {};
    for (u32 frame = 0; frame < 20 && !rc_button_center(app, "RECORD", record); frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
    }
    ASSERT_TRUE(rc_button_center(app, "RECORD", record));

    aim_at(sim, record, 0.0f, 0.0f);
    for (u32 frame = 0; frame < 3; frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
        EXPECT_FALSE(rc_recording(app)) << "aiming alone records nothing";
    }
    pinch(sim, 1.0f);
    b8 started = false;
    for (u32 frame = 0; frame < 3 && !started; frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
        started = rc_recording(app);
    }
    EXPECT_TRUE(started);
}

// Regression: rays were hidden during every take, so a free take could not be stopped with a pinch
TEST_F(RecorderTest, a_free_take_stops_with_a_pinch_on_stop)
{
    app = rc_create(options(nullptr));
    ASSERT_NE(app, nullptr);
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    // Looking down at the panel, so a hand aiming at its bottom row stays in the tracking cone
    Pose head        = sim_get_device_pose(sim, SIM_Device::Head);
    head.orientation = zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -30.0f * zm::DEG_TO_RAD);
    sim_set_device_pose(sim, SIM_Device::Head, head);
    f32 free_take[3] = {};
    for (u32 frame = 0; frame < 20 && !rc_button_center(app, "FREE TAKE", free_take); frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
    }
    aim_at(sim, free_take, 0.0f, 0.0f);
    ASSERT_EQ(rc_frame(app), RC_Status::Running);
    pinch(sim, 1.0f);
    for (u32 frame = 0; frame < 3 && !rc_recording(app); frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
    }
    ASSERT_TRUE(rc_recording(app));
    pinch(sim, 0.0f);
    for (u32 frame = 0; frame < 3; frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
    }
    EXPECT_TRUE(rc_recording(app)) << "a free take runs until stopped";
    pinch(sim, 1.0f);
    for (u32 frame = 0; frame < 3 && rc_recording(app); frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
    }
    EXPECT_FALSE(rc_recording(app));
    EXPECT_EQ(rc_takes_count(app), 1u);
}

TEST_F(RecorderTest, a_pinch_held_or_aimed_beside_the_buttons_clicks_nothing)
{
    app = rc_create(options(nullptr));
    ASSERT_NE(app, nullptr);
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    f32 record[3] = {};
    for (u32 frame = 0; frame < 20 && !rc_button_center(app, "RECORD", record); frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
    }
    aim_at(sim, record, 0.0f, 0.12f);
    pinch(sim, 1.0f);
    for (u32 frame = 0; frame < 3; frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
    }
    aim_at(sim, record, 0.0f, 0.0f);
    for (u32 frame = 0; frame < 3; frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
        EXPECT_FALSE(rc_recording(app)) << "a pinch that began beside the button is no click on it";
    }
}

// A runtime whose aim state never reports a pinch, and optionally a hand active without joints,
// as a Quest 3 did for the recorder
namespace
{
PFN_xrGetInstanceProcAddr g_quest_gipa     = nullptr;
PFN_xrLocateHandJointsEXT g_quest_locate   = nullptr;
b8                        g_without_joints = false;

XRAPI_ATTR XrResult XRAPI_CALL quest_locate (XrHandTrackerEXT tracker, const XrHandJointsLocateInfoEXT* info, XrHandJointLocationsEXT* locations)
{
    const XrResult result = g_quest_locate(tracker, info, locations);
    for (XrBaseOutStructure* next = (XrBaseOutStructure*)locations->next; next; next = next->next)
    {
        if (next->type == XR_TYPE_HAND_TRACKING_AIM_STATE_FB)
        {
            XrHandTrackingAimStateFB* aim = (XrHandTrackingAimStateFB*)next;
            aim->status &= ~(XrHandTrackingAimFlagsFB)XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB;
            aim->pinchStrengthIndex = 0.0f;
        }
    }
    if (g_without_joints && locations->isActive)
    {
        for (u32 j = 0; j < locations->jointCount; j++)
        {
            locations->jointLocations[j] = {};
        }
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL quest_gipa (XrInstance instance, const char* name, PFN_xrVoidFunction* out_function)
{
    const XrResult result = g_quest_gipa(instance, name, out_function);
    if (XR_SUCCEEDED(result) && strcmp(name, "xrLocateHandJointsEXT") == 0)
    {
        g_quest_locate = (PFN_xrLocateHandJointsEXT)*out_function;
        *out_function  = (PFN_xrVoidFunction)quest_locate;
    }
    return result;
}

b8 pinch_on_record_starts_a_take (RecorderTest& test, b8 without_joints)
{
    char protocol[600];
    (void)snprintf(protocol, sizeof(protocol), "%s/protocol.json", test.out_dir);
    test.write_file(protocol, R"({"steps": [{"name": "aimed", "instruction": "", "seconds": 0.1}]})");
    RC_Options opts             = test.options(protocol);
    g_quest_gipa                = (PFN_xrGetInstanceProcAddr)opts.get_instance_proc_addr;
    g_without_joints            = without_joints;
    opts.get_instance_proc_addr = (void*)&quest_gipa;
    test.app                    = rc_create(opts);
    EXPECT_NE(test.app, nullptr);
    if (!test.app)
    {
        return false;
    }
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    f32 record[3] = {};
    for (u32 frame = 0; frame < 20 && !rc_button_center(test.app, "RECORD", record); frame++)
    {
        rc_frame(test.app);
    }
    aim_at(sim, record, 0.0f, 0.0f);
    rc_frame(test.app);
    pinch(sim, 1.0f);
    b8 started = false;
    for (u32 frame = 0; frame < 3 && !started; frame++)
    {
        rc_frame(test.app);
        started = rc_recording(test.app);
    }
    return started;
}
} // namespace

// Regression: on a Quest 3 the aim state reported no pinch while the tips touched, so a pinch
// clicked nothing; the recorder reads the pinch from the joints too
TEST_F(RecorderTest, a_pinch_of_the_joints_clicks_where_the_runtime_reports_none)
{
    EXPECT_TRUE(pinch_on_record_starts_a_take(*this, false));
}

// Regression: a Quest 3 reported hands active with every joint zero while it drove them from the
// controllers; tips at the same place are no pinch
TEST_F(RecorderTest, a_hand_without_joints_never_pinches)
{
    EXPECT_FALSE(pinch_on_record_starts_a_take(*this, true));
}

// The right hand's bones are orange (skeleton.vert), a colour nothing else on the screen has: not
// the grey panel, its blue and red, the white ray and cursor, nor the room
static u32 orange_pixels (const Array<u8>& rgba)
{
    u32 count = 0;
    for (usize i = 0; i + 3 < rgba.length; i += 4)
    {
        const i32 r = rgba[i], g = rgba[i + 1], b = rgba[i + 2];
        count += (r > 150 && r - b > 80 && g > b + 30 && g < r) ? 1u : 0u;
    }
    return count;
}

static void read_frame (Array<u8>& out)
{
    u32 width  = 0;
    u32 height = 0;
    u8  probe[4];
    oxr_read_frame(probe, 0, width, height);
    out.resize((usize)width * height * 4);
    ASSERT_TRUE(oxr_read_frame(out.data, out.length, width, height));
}

TEST_F(RecorderTest, the_hand_skeleton_is_drawn_in_3d_over_the_panel)
{
    app = rc_create(options(nullptr));
    ASSERT_NE(app, nullptr);
    SIM_Sim* sim = oxr_get_sim();
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    f32 record[3] = {};
    for (u32 frame = 0; frame < 20 && !rc_button_center(app, "RECORD", record); frame++)
    {
        ASSERT_EQ(rc_frame(app), RC_Status::Running);
    }
    // The index tip just in front of the button: the hand covers part of the panel
    sim_set_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Poke, { { record[0], record[1], record[2] + 0.03f }, zm::QUAT_IDENTITY });
    ASSERT_EQ(rc_frame(app), RC_Status::Running);
    EXPECT_EQ(rc_skeleton_instances(app), 26u + 24u) << "a box per joint and per bone of the right hand; the left one holds its controller";
    Array<u8> with_hand = {};
    defer { with_hand.release(); };
    read_frame(with_hand);

    sim_set_device_active(sim, SIM_Device::RightHand, false);
    ASSERT_EQ(rc_frame(app), RC_Status::Running);
    EXPECT_EQ(rc_skeleton_instances(app), 0u);
    Array<u8> without_hand = {};
    defer { without_hand.release(); };
    read_frame(without_hand);
    EXPECT_GT(orange_pixels(with_hand), 200u) << "the bones show in the composited frame";
    EXPECT_EQ(orange_pixels(without_hand), 0u);
}

// The checked-in SPIR-V (recorder_shaders.h) is what compile_shaders.sh makes of the GLSL next to it
TEST(RecorderShaders, the_checked_in_spirv_matches_the_glsl)
{
#if defined(_WIN32)
    GTEST_SKIP() << "compile_shaders.sh is a shell script";
#else
    if (system("command -v glslangValidator > /dev/null 2>&1") != 0)
    {
        GTEST_SKIP() << "no glslangValidator";
    }
    char command[1024];
    (void)snprintf(command, sizeof(command), "%s/tools/recorder/compile_shaders.sh --check", PUPPETXR_SOURCE_DIR);
    int status = system(command); // not const: macOS's WIFEXITED casts its address to int*
    if (WIFEXITED(status) && WEXITSTATUS(status) == 125)
    {
        GTEST_SKIP() << "another glslangValidator than the one that made recorder_shaders.h";
    }
    EXPECT_EQ(status, 0) << "run tools/recorder/compile_shaders.sh after changing the shaders";
#endif
}

// A runtime that offers many swapchain formats, the one the panel needs last, as a Quest 3 does
namespace
{
PFN_xrGetInstanceProcAddr        g_runtime_gipa      = nullptr;
PFN_xrEnumerateSwapchainFormats g_runtime_enumerate = nullptr;

XRAPI_ATTR XrResult XRAPI_CALL many_formats (XrSession session, uint32_t capacity, uint32_t* out_count, int64_t* formats)
{
    const u32 extra = 100;
    u32       count = 0;
    XrResult  result = g_runtime_enumerate(session, 0, &count, nullptr);
    if (XR_FAILED(result))
    {
        return result;
    }
    *out_count = count + extra;
    if (capacity == 0)
    {
        return XR_SUCCESS;
    }
    if (capacity < count + extra)
    {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    for (u32 i = 0; i < extra; i++)
    {
        formats[i] = VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    return g_runtime_enumerate(session, count, &count, formats + extra);
}

XRAPI_ATTR XrResult XRAPI_CALL many_formats_gipa (XrInstance instance, const char* name, PFN_xrVoidFunction* out_function)
{
    const XrResult result = g_runtime_gipa(instance, name, out_function);
    if (XR_SUCCEEDED(result) && strcmp(name, "xrEnumerateSwapchainFormats") == 0)
    {
        g_runtime_enumerate = (PFN_xrEnumerateSwapchainFormats)*out_function;
        *out_function       = (PFN_xrVoidFunction)many_formats;
    }
    return result;
}
} // namespace

// Regression: the recorder asked for at most 64 formats and failed on a Quest 3, which offers more
TEST_F(RecorderTest, finds_its_format_among_more_than_64)
{
    RC_Options opts             = options(nullptr);
    g_runtime_gipa              = (PFN_xrGetInstanceProcAddr)opts.get_instance_proc_addr;
    opts.get_instance_proc_addr = (void*)&many_formats_gipa;
    app                         = rc_create(opts);
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(rc_frame(app), RC_Status::Running);
}

TEST(RecorderProtocol, builtin_steps_have_unique_valid_names)
{
    const RC_Step* steps = nullptr;
    const u32      count = rc_builtin_protocol(&steps);
    ASSERT_GT(count, 10u);
    ASSERT_LE(count, (u32)RC_MAX_STEPS);
    for (u32 i = 0; i < count; i++)
    {
        SCOPED_TRACE(steps[i].name);
        EXPECT_GT(steps[i].seconds, 0.0f);
        EXPECT_GT(strlen(steps[i].instruction), 0u);
        for (const char* c = steps[i].name; *c; c++)
        {
            EXPECT_TRUE((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_');
        }
        for (u32 k = 0; k < i; k++)
        {
            EXPECT_STRNE(steps[i].name, steps[k].name);
        }
    }
}

TEST(RecorderProtocol, a_bad_step_names_its_index)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/recorder_bad_protocol.json", PUPPETXR_TEST_OUTPUT_DIR);
    FILE* file = fopen(path, "wb");
    ASSERT_NE(file, nullptr);
    fputs(R"({"steps": [{"name": "ok", "seconds": 1}, {"name": "Not OK", "seconds": 1}]})", file);
    fclose(file);
    RC_Step steps[RC_MAX_STEPS] = {};
    u32     count               = 0;
    char    error[256]          = {};
    EXPECT_FALSE(rc_load_protocol(path, steps, count, error, sizeof(error)));
    EXPECT_NE(strstr(error, "steps[1]"), nullptr) << error;

    EXPECT_FALSE(rc_load_protocol("/nonexistent/protocol.json", steps, count, error, sizeof(error)));
}
