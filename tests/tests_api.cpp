#include "oxr.h"

#include <gtest/gtest.h>
#include <math.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <stdlib.h>

#include "test_room.h"

// Last: on Windows it brings windows.h, whose near and far macros break zmath.h
#include "puppetxr.h"

extern "C" int puppetxr_header_is_c (void);
extern "C" XrResult xrNegotiateLoaderRuntimeInterface (const XrNegotiateLoaderInfo* loader_info, XrNegotiateRuntimeRequest* runtime_request);

// The temporary directory of the platform, for files the tests write
static const char* temp_dir ()
{
    static char dir[OS_MAX_PATH] = {};
    if (!dir[0] && !os_temp_dir(dir, sizeof(dir)))
    {
        cstr_copy(dir, sizeof(dir), ".");
    }
    return dir;
}

namespace
{

// A headless instance and session driven frame by frame, like a minimal app
struct ApiTest : testing::Test
{
    PUPPETXR_Api           api                = {};
    PFN_xrGetInstanceProcAddr gipa            = nullptr;
    XrInstance             instance           = XR_NULL_HANDLE;
    XrSession              session            = XR_NULL_HANDLE;
    PFN_xrDestroyInstance  destroy_instance   = nullptr;
    PFN_xrWaitFrame        wait_frame         = nullptr;
    PFN_xrBeginFrame       begin_frame        = nullptr;
    PFN_xrEndFrame         end_frame          = nullptr;
    PFN_xrPollEvent        poll_event         = nullptr;
    char                   app_name[64]       = {}; // per process: ctest runs tests in parallel and puppetxrctl matches apps by name
    char                   anchors_store[OS_MAX_PATH] = {}; // a store of its own, not the user's state shared by parallel tests

    void SetUp () override
    {
        os_set_assert_mode(OS_AssertMode::Log);
        os_set_env("PUPPETXR_HEADLESS", "1");
        os_set_env("PUPPETXR_FIXED_STEP", "1");
        ASSERT_TRUE(os_create_temp_directory("puppetxr_api_anchors", anchors_store, sizeof(anchors_store)));
        os_set_env("PUPPETXR_ANCHORS_DIR", anchors_store);
        ASSERT_EQ(puppetxr_get_api(PUPPETXR_API_VERSION, &api), PUPPETXR_SUCCESS);

        const XrNegotiateLoaderInfo     loader_info = { .structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, .structVersion = XR_LOADER_INFO_STRUCT_VERSION, .structSize = sizeof(XrNegotiateLoaderInfo), .minInterfaceVersion = 1, .maxInterfaceVersion = 1, .minApiVersion = XR_MAKE_VERSION(1, 0, 0), .maxApiVersion = XR_MAKE_VERSION(1, 1, 999) };
        XrNegotiateRuntimeRequest request     = { .structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, .structVersion = XR_RUNTIME_INFO_STRUCT_VERSION, .structSize = sizeof(XrNegotiateRuntimeRequest) };
        ASSERT_EQ(xrNegotiateLoaderRuntimeInterface(&loader_info, &request), XR_SUCCESS);
        gipa = request.getInstanceProcAddr;
    }

    void TearDown () override
    {
        if (instance != XR_NULL_HANDLE)
        {
            destroy_instance(instance);
        }
        os_unset_env("PUPPETXR_ANCHORS_DIR");
        os_delete_tree(anchors_store);
    }

    template <typename F>
    F proc (const char* name) const
    {
        PFN_xrVoidFunction fn = nullptr;
        gipa(instance, name, &fn);
        return (F)fn;
    }

    void start (const char* extension = nullptr)
    {
        PFN_xrCreateInstance create = nullptr;
        gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&create);
        const char*          extensions[] = { XR_MND_HEADLESS_EXTENSION_NAME, extension };
        cstr_format(app_name, sizeof(app_name), "api_test_%u", os_process_id());
        XrInstanceCreateInfo info = { .type = XR_TYPE_INSTANCE_CREATE_INFO, .applicationInfo = { .apiVersion = XR_API_VERSION_1_1 }, .enabledExtensionCount = extension ? 2u : 1u, .enabledExtensionNames = extensions };
        cstr_copy(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName), app_name);
        ASSERT_EQ(create(&info, &instance), XR_SUCCESS);
        destroy_instance = proc<PFN_xrDestroyInstance>("xrDestroyInstance");
        wait_frame       = proc<PFN_xrWaitFrame>("xrWaitFrame");
        begin_frame      = proc<PFN_xrBeginFrame>("xrBeginFrame");
        end_frame        = proc<PFN_xrEndFrame>("xrEndFrame");
        poll_event       = proc<PFN_xrPollEvent>("xrPollEvent");

        XrSystemId            system      = XR_NULL_SYSTEM_ID;
        const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
        ASSERT_EQ(proc<PFN_xrGetSystem>("xrGetSystem")(instance, &system_info, &system), XR_SUCCESS);
        const XrSessionCreateInfo session_info = { .type = XR_TYPE_SESSION_CREATE_INFO, .systemId = system };
        ASSERT_EQ(proc<PFN_xrCreateSession>("xrCreateSession")(instance, &session_info, &session), XR_SUCCESS);
        const XrSessionBeginInfo begin_info = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
        ASSERT_EQ(proc<PFN_xrBeginSession>("xrBeginSession")(session, &begin_info), XR_SUCCESS);
    }

    void stop ()
    {
        destroy_instance(instance);
        instance = XR_NULL_HANDLE;
        session  = XR_NULL_HANDLE;
    }

    void frame () const
    {
        XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
        ASSERT_EQ(wait_frame(session, nullptr, &state), XR_SUCCESS);
        ASSERT_EQ(begin_frame(session, nullptr), XR_SUCCESS);
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        ASSERT_EQ(end_frame(session, &end_info), XR_SUCCESS);
    }

    XrSessionState last_state () const
    {
        XrSessionState last = XR_SESSION_STATE_UNKNOWN;
        XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        while (poll_event(instance, &event) == XR_SUCCESS)
        {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                last = ((XrEventDataSessionStateChanged*)&event)->state;
            }
        }
        return last;
    }
};

} // namespace

static void api_path (char* out, usize size, const char* name)
{
    cstr_format(out, size, "%s/%s", temp_dir(), name);
}

TEST_F(ApiTest, versioning)
{
    EXPECT_EQ(PUPPETXR_MAKE_VERSION(PUPPETXR_BUILD_VERSION_MAJOR, PUPPETXR_BUILD_VERSION_MINOR, PUPPETXR_BUILD_VERSION_PATCH), PUPPETXR_VERSION);
    EXPECT_EQ(PUPPETXR_VERSION_GET_MAJOR(PUPPETXR_MAKE_VERSION(3, 1000, 4000)), 3u);
    EXPECT_EQ(PUPPETXR_VERSION_GET_MINOR(PUPPETXR_MAKE_VERSION(3, 1000, 4000)), 1000u);
    EXPECT_EQ(PUPPETXR_VERSION_GET_PATCH(PUPPETXR_MAKE_VERSION(3, 1000, 4000)), 4000u);

    u32 runtime_version = 0;
    u32 api_version = 0;
    EXPECT_EQ(puppetxr_get_version(&runtime_version, &api_version), PUPPETXR_SUCCESS);
    EXPECT_EQ(runtime_version, (u32)PUPPETXR_VERSION);
    EXPECT_EQ(api_version, (u32)PUPPETXR_API_VERSION);
    EXPECT_EQ(puppetxr_get_version(nullptr, nullptr), PUPPETXR_SUCCESS);

    EXPECT_EQ(api.version, (u32)PUPPETXR_API_VERSION);
    EXPECT_EQ(api.runtime_version, (u32)PUPPETXR_VERSION);
    EXPECT_NE(api.set_controller, nullptr);

    // A client built against a newer header gets what this runtime has, not an error
    PUPPETXR_Api newer = {};
    EXPECT_EQ(puppetxr_get_api(PUPPETXR_API_VERSION + 5, &newer), PUPPETXR_SUCCESS);
    EXPECT_EQ(newer.version, (u32)PUPPETXR_API_VERSION);
    EXPECT_EQ(puppetxr_get_api(0, &newer), PUPPETXR_ERROR_VERSION);
    EXPECT_EQ(puppetxr_get_api(PUPPETXR_API_VERSION, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);

    // The runtime never writes past what the requested version defines
    struct Guarded
    {
        PUPPETXR_Api api;
        u8        canary[32];
    } guarded = {};
    memory_zero(&guarded, sizeof(guarded));
    for (u8& byte : guarded.canary)
    {
        byte = 0xAB;
    }
    EXPECT_EQ(puppetxr_get_api(1, &guarded.api), PUPPETXR_SUCCESS);
    for (const u8 byte : guarded.canary)
    {
        EXPECT_EQ(byte, 0xAB);
    }
}

// The runtime is linked into the test binary, not loaded by the OpenXR loader, so the C
// loader helper must report it as not loaded rather than find something else
namespace
{

struct ApiLogSink
{
    u32            count;
    u32            created;
    PUPPETXR_LogLevel level;
    char           last[256];
};

} // namespace

static void api_log_sink (PUPPETXR_LogLevel level, const char* message, void* user)
{
    ApiLogSink& sink = *(ApiLogSink*)user;
    sink.count++;
    sink.level = level;
    sink.created += cstr_find(message, "Instance created") != nullptr;
    cstr_copy(sink.last, sizeof(sink.last), message);
}

static b8 file_has (const u8* data, usize size, const char* text)
{
    const usize length = cstr_length(text);
    for (usize i = 0; i + length <= size; i++)
    {
        if (memory_equal(data + i, text, length))
        {
            return true;
        }
    }
    return false;
}

// A profile capture (docs/profiling.md), which needs no instance
TEST_F(ApiTest, profiler_starts_and_stops)
{
    ASSERT_NE(api.profiler_start, nullptr);
    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxr_api_profile.json");
    EXPECT_EQ(api.profiler_start(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.profiler_start("/nonexistent/dir/profile.json"), PUPPETXR_ERROR_IO);
    EXPECT_EQ(api.profiler_stop(), PUPPETXR_ERROR_NOT_AVAILABLE) << "no capture runs";
    ASSERT_EQ(api.profiler_start(path), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.profiler_start(path), PUPPETXR_ERROR_BUSY);
    {
        PROF_ZONE("api test zone");
    }
    EXPECT_EQ(api.profiler_stop(), PUPPETXR_SUCCESS);
    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(path, &data, &size));
    EXPECT_TRUE(file_has(data, size, "api test zone"));
    os_memory_release(data);
    os_delete_file(path);
}

TEST_F(ApiTest, log_callback_receives_runtime_messages)
{
    ApiLogSink live = {};
    ASSERT_EQ(api.set_log_callback(api_log_sink, &live, 0), PUPPETXR_SUCCESS);
    start();
    EXPECT_EQ(live.created, 1u);
    LOG_ERROR("api log %d", 3);
    EXPECT_EQ(live.level, PUPPETXR_LOG_ERROR);
    EXPECT_TRUE(cstr_equal(live.last, "api log 3"));

    // Replay delivers what was logged before, such as instance creation
    ApiLogSink replayed = {};
    ASSERT_EQ(api.set_log_callback(api_log_sink, &replayed, 1), PUPPETXR_SUCCESS);
    EXPECT_GE(replayed.count, 2u);
    EXPECT_GE(replayed.created, 1u);
    EXPECT_TRUE(cstr_equal(replayed.last, "api log 3"));

    const u32 before = live.count + replayed.count;
    ASSERT_EQ(api.set_log_callback(nullptr, nullptr, 0), PUPPETXR_SUCCESS);
    LOG_INFO("nobody listens");
    EXPECT_EQ(live.count + replayed.count, before);
}

TEST_F(ApiTest, header_compiles_as_c)
{
    EXPECT_EQ(puppetxr_header_is_c(), 1);
}

TEST_F(ApiTest, status_respects_the_callers_struct_size)
{
    start();
    struct Guarded
    {
        PUPPETXR_Status status;
        u8           canary[32];
    } guarded = {};
    for (usize i = 0; i < sizeof(guarded); i++)
    {
        ((u8*)&guarded)[i] = 0xCD;
    }
    guarded.status.struct_size = sizeof(PUPPETXR_Status);
    EXPECT_EQ(api.get_status(&guarded.status), PUPPETXR_SUCCESS);
    EXPECT_EQ(guarded.status.struct_size, (u32)sizeof(PUPPETXR_Status));
    EXPECT_EQ(guarded.status.controller, (i32)PUPPETXR_CONTROLLER_META_TOUCH_PLUS);
    for (const u8 byte : guarded.canary)
    {
        EXPECT_EQ(byte, 0xCD);
    }

    PUPPETXR_Status tiny = { .struct_size = 8 };
    EXPECT_EQ(api.get_status(&tiny), PUPPETXR_ERROR_INVALID_ARGUMENT);
}

TEST_F(ApiTest, controller_type)
{
    start();
    PUPPETXR_Controller controller = PUPPETXR_CONTROLLER_KHR_SIMPLE;
    EXPECT_EQ(api.get_controller(&controller), PUPPETXR_SUCCESS);
    EXPECT_EQ(controller, PUPPETXR_CONTROLLER_META_TOUCH_PLUS);
    EXPECT_EQ(api.get_controller(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_controller((PUPPETXR_Controller)7), PUPPETXR_ERROR_INVALID_ARGUMENT);

    EXPECT_EQ(api.set_controller(PUPPETXR_CONTROLLER_KHR_SIMPLE), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.get_controller(&controller), PUPPETXR_SUCCESS);
    EXPECT_EQ(controller, PUPPETXR_CONTROLLER_KHR_SIMPLE);
    EXPECT_EQ(api.set_input(PUPPETXR_DEVICE_RIGHT_HAND, "select/click", 1.0f, 0.0f), PUPPETXR_SUCCESS);
    f32 x = 0.0f;
    EXPECT_EQ(api.get_input(PUPPETXR_DEVICE_RIGHT_HAND, "select/click", &x, nullptr), PUPPETXR_SUCCESS);
    EXPECT_FLOAT_EQ(x, 1.0f);

    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxr_api_controller.xrrec");
    ASSERT_EQ(api.record_start(path), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_controller(PUPPETXR_CONTROLLER_OCULUS_TOUCH), PUPPETXR_ERROR_BUSY);
    ASSERT_EQ(api.record_stop(), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_controller(PUPPETXR_CONTROLLER_OCULUS_TOUCH), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_controller(PUPPETXR_CONTROLLER_META_TOUCH_PRO), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.get_controller(&controller), PUPPETXR_SUCCESS);
    EXPECT_EQ(controller, PUPPETXR_CONTROLLER_META_TOUCH_PRO);
    EXPECT_EQ(api.set_input(PUPPETXR_DEVICE_LEFT_HAND, "stylus/force", 0.5f, 0.0f), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.get_input(PUPPETXR_DEVICE_LEFT_HAND, "stylus/force", &x, nullptr), PUPPETXR_SUCCESS);
    EXPECT_FLOAT_EQ(x, 0.5f);
}

// The skeleton change period decides the skeletonChangedCount an app reads, so a replay takes the
// recording's
TEST_F(ApiTest, replay_takes_the_skeleton_change_period)
{
    start();
    sim_set_skeleton_change_period(oxr_get_sim(), 2000000000);
    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxr_api_skeleton_period.xrrec");
    ASSERT_EQ(api.record_start(path), PUPPETXR_SUCCESS);
    ASSERT_EQ(api.record_stop(), PUPPETXR_SUCCESS);
    sim_set_skeleton_change_period(oxr_get_sim(), 0);

    ASSERT_EQ(api.replay_start(path), PUPPETXR_SUCCESS);
    EXPECT_EQ(sim_get_skeleton_change_period(oxr_get_sim()), 2000000000);
    ASSERT_EQ(api.replay_stop(), PUPPETXR_SUCCESS);
}

TEST_F(ApiTest, hand_source)
{
    start();
    PUPPETXR_HandSource source = PUPPETXR_HAND_SOURCE_HAND;
    EXPECT_EQ(api.get_hand_source(PUPPETXR_DEVICE_RIGHT_HAND, &source), PUPPETXR_SUCCESS);
    EXPECT_EQ(source, PUPPETXR_HAND_SOURCE_CONTROLLER);
    EXPECT_EQ(api.get_hand_source(PUPPETXR_DEVICE_HEAD, &source), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_hand_source(PUPPETXR_DEVICE_RIGHT_HAND, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_hand_source(PUPPETXR_DEVICE_LEFT_HAND, (PUPPETXR_HandSource)5), PUPPETXR_ERROR_INVALID_ARGUMENT);

    EXPECT_EQ(api.set_hand_source(PUPPETXR_DEVICE_LEFT_HAND, PUPPETXR_HAND_SOURCE_HAND), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.get_hand_source(PUPPETXR_DEVICE_LEFT_HAND, &source), PUPPETXR_SUCCESS);
    EXPECT_EQ(source, PUPPETXR_HAND_SOURCE_HAND);
    EXPECT_EQ(sim_get_hand_source(oxr_get_sim(), SIM_Device::LeftHand), SIM_HandSource::Hand);
    EXPECT_EQ(api.set_input(PUPPETXR_DEVICE_LEFT_HAND, "trigger/value", 1.0f, 0.0f), PUPPETXR_SUCCESS);

    // The source is part of every recorded frame, so it may change while recording
    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxr_api_hand_source.xrrec");
    ASSERT_EQ(api.record_start(path), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_hand_source(PUPPETXR_DEVICE_LEFT_HAND, PUPPETXR_HAND_SOURCE_CONTROLLER), PUPPETXR_SUCCESS);
    ASSERT_EQ(api.record_stop(), PUPPETXR_SUCCESS);
}

// Shows the app's keyboard 1 m in front of the stage origin at 1 m height
static XrVirtualKeyboardMETA show_keyboard (const ApiTest& test)
{
    XrVirtualKeyboardMETA                 keyboard = XR_NULL_HANDLE;
    const XrVirtualKeyboardCreateInfoMETA info     = { .type = XR_TYPE_VIRTUAL_KEYBOARD_CREATE_INFO_META };
    EXPECT_EQ(test.proc<PFN_xrCreateVirtualKeyboardMETA>("xrCreateVirtualKeyboardMETA")(test.session, &info, &keyboard), XR_SUCCESS);
    XrSpace                          stage      = XR_NULL_HANDLE;
    const XrReferenceSpaceCreateInfo stage_info = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE, .poseInReferenceSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    EXPECT_EQ(test.proc<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(test.session, &stage_info, &stage), XR_SUCCESS);
    XrSpace                                    space      = XR_NULL_HANDLE;
    const XrVirtualKeyboardSpaceCreateInfoMETA space_info = { .type = XR_TYPE_VIRTUAL_KEYBOARD_SPACE_CREATE_INFO_META, .locationType = XR_VIRTUAL_KEYBOARD_LOCATION_TYPE_CUSTOM_META, .space = stage, .poseInSpace = { { 0, 0, 0, 1 }, { 0, 1, -1 } } };
    EXPECT_EQ(test.proc<PFN_xrCreateVirtualKeyboardSpaceMETA>("xrCreateVirtualKeyboardSpaceMETA")(test.session, keyboard, &space_info, &space), XR_SUCCESS);
    const XrVirtualKeyboardModelVisibilitySetInfoMETA visible = { .type = XR_TYPE_VIRTUAL_KEYBOARD_MODEL_VISIBILITY_SET_INFO_META, .visible = XR_TRUE };
    EXPECT_EQ(test.proc<PFN_xrSetVirtualKeyboardModelVisibilityMETA>("xrSetVirtualKeyboardModelVisibilityMETA")(keyboard, &visible), XR_SUCCESS);
    return keyboard;
}

TEST_F(ApiTest, virtual_keyboard)
{
    start(XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME);
    PUPPETXR_VirtualKeyboard keyboard = { .struct_size = sizeof(PUPPETXR_VirtualKeyboard) };
    ASSERT_EQ(api.get_virtual_keyboard(&keyboard), PUPPETXR_SUCCESS);
    EXPECT_EQ(keyboard.exists, 0);
    EXPECT_EQ(api.type_text("hi"), PUPPETXR_ERROR_NOT_AVAILABLE) << "no keyboard is shown";
    PUPPETXR_Pose key = {};
    EXPECT_EQ(api.get_virtual_keyboard_key("q", &key), PUPPETXR_ERROR_NOT_AVAILABLE);

    show_keyboard(*this);
    ASSERT_EQ(api.get_virtual_keyboard(&keyboard), PUPPETXR_SUCCESS);
    EXPECT_EQ(keyboard.exists, 1);
    EXPECT_EQ(keyboard.visible, 1);
    EXPECT_FLOAT_EQ(keyboard.pose.position[2], -1.0f);
    EXPECT_FLOAT_EQ(keyboard.scale, 1.0f);
    ASSERT_EQ(api.get_virtual_keyboard_key("q", &key), PUPPETXR_SUCCESS);
    EXPECT_LT(key.position[0], 0.0f) << "q is on the left";
    EXPECT_GT(key.position[2], -1.0f) << "on the key tops";
    EXPECT_EQ(api.get_virtual_keyboard_key("tab", &key), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_virtual_keyboard_key(nullptr, &key), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_virtual_keyboard_key("q", nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_virtual_keyboard(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    PUPPETXR_VirtualKeyboard tiny = { .struct_size = 4 };
    EXPECT_EQ(api.get_virtual_keyboard(&tiny), PUPPETXR_ERROR_INVALID_ARGUMENT);

    EXPECT_EQ(api.type_text(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.type_text("bell\a"), PUPPETXR_ERROR_INVALID_ARGUMENT);
    last_state();
    ASSERT_EQ(api.type_text("hi\n"), PUPPETXR_SUCCESS);
    frame();
    XrEventDataBuffer event   = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    char              text[8] = {};
    b8                enter   = false;
    while (poll_event(instance, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_COMMIT_TEXT_META)
        {
            cstr_copy(text, sizeof(text), ((XrEventDataVirtualKeyboardCommitTextMETA*)&event)->text);
        }
        enter |= event.type == XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_ENTER_META;
        event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
    EXPECT_STREQ(text, "hi");
    EXPECT_TRUE(enter);
}

static PUPPETXR_Prop api_qr (const char* text, float x, float y, float z)
{
    PUPPETXR_Prop prop = { .struct_size = sizeof(PUPPETXR_Prop), .kind = PUPPETXR_PROP_QR_CODE, .motion = PUPPETXR_PROP_STILL, .pose = { { x, y, z }, { 0, 0, 0, 1 } }, .size = 0.1f };
    cstr_copy(prop.text, sizeof(prop.text), text);
    return prop;
}

TEST_F(ApiTest, props)
{
    start();
    PUPPETXR_U32 id = 0;
    PUPPETXR_Prop prop = api_qr("hello", 0.0f, 1.5f, -1.0f);
    ASSERT_EQ(api.add_prop(&prop, &id), PUPPETXR_SUCCESS);
    EXPECT_EQ(id, 1u);
    PUPPETXR_Prop keyboard = { .struct_size = sizeof(PUPPETXR_Prop), .kind = PUPPETXR_PROP_KEYBOARD, .motion = PUPPETXR_PROP_LINEAR, .pose = { { 0, 0.8f, -0.5f }, { 0, 0, 0, 1 } }, .target = { 0.5f, 0.8f, -0.5f }, .speed = 0.25f };
    ASSERT_EQ(api.add_prop(&keyboard, &id), PUPPETXR_SUCCESS);
    EXPECT_EQ(id, 2u);

    PUPPETXR_Prop bad = api_qr("x", 0, 0, 0);
    bad.size       = -1.0f;
    EXPECT_EQ(api.add_prop(&bad, &id), PUPPETXR_ERROR_INVALID_ARGUMENT);
    bad        = api_qr("x", 0, 0, 0);
    bad.kind   = (PUPPETXR_PropKind)7;
    EXPECT_EQ(api.add_prop(&bad, &id), PUPPETXR_ERROR_INVALID_ARGUMENT);
    bad             = api_qr("x", 0, 0, 0);
    bad.struct_size = 8;
    EXPECT_EQ(api.add_prop(&bad, &id), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.add_prop(nullptr, &id), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.add_prop(&prop, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);

    PUPPETXR_Prop listed[4] = {};
    listed[0].struct_size = sizeof(PUPPETXR_Prop);
    PUPPETXR_U32 count    = 0;
    ASSERT_EQ(api.get_props(listed, 4, &count), PUPPETXR_SUCCESS);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(listed[0].id, 1u);
    EXPECT_STREQ(listed[0].text, "hello");
    EXPECT_EQ(listed[1].kind, PUPPETXR_PROP_KEYBOARD);
    EXPECT_EQ(listed[1].motion, PUPPETXR_PROP_LINEAR);
    EXPECT_FLOAT_EQ(listed[1].current.position[1], 0.8f);
    ASSERT_EQ(api.get_props(nullptr, 0, &count), PUPPETXR_SUCCESS);
    EXPECT_EQ(count, 2u);
    listed[0].struct_size = 4;
    EXPECT_EQ(api.get_props(listed, 4, &count), PUPPETXR_ERROR_INVALID_ARGUMENT);

    prop = api_qr("moved", 1.0f, 1.5f, -1.0f);
    EXPECT_EQ(api.update_prop(1, &prop), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.update_prop(9, &prop), PUPPETXR_ERROR_INVALID_ARGUMENT);
    const SIM_Prop* props = nullptr;
    ASSERT_EQ(sim_props(oxr_get_sim(), props), 2u);
    EXPECT_STREQ(props[0].text, "moved");
    EXPECT_FLOAT_EQ(props[0].pose.position.x, 1.0f);
    EXPECT_EQ(api.remove_prop(2), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.remove_prop(2), PUPPETXR_ERROR_INVALID_ARGUMENT);

    // Props are recorded, so they may change while recording
    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxr_api_props.xrrec");
    ASSERT_EQ(api.record_start(path), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.add_prop(&keyboard, &id), PUPPETXR_SUCCESS);
    ASSERT_EQ(api.record_stop(), PUPPETXR_SUCCESS);
}

TEST_F(ApiTest, every_call_reports_a_missing_instance)
{
    PUPPETXR_Status status = { .struct_size = sizeof(PUPPETXR_Status) };
    PUPPETXR_Pose pose  = { { 0, 1, 0 }, { 0, 0, 0, 1 } };
    PUPPETXR_Controller controller = {};
    f32          x = 0.0f;
    f32          y = 0.0f;
    EXPECT_EQ(api.get_status(&status), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.get_device_pose(PUPPETXR_DEVICE_HEAD, &pose), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_device_pose(PUPPETXR_DEVICE_HEAD, &pose), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_device_active(PUPPETXR_DEVICE_LEFT_HAND, 0), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.get_input(PUPPETXR_DEVICE_LEFT_HAND, "trigger/value", &x, &y), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_input(PUPPETXR_DEVICE_LEFT_HAND, "trigger/value", 1.0f, 0.0f), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_fixed_step(1), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_controls_enabled(1), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.recenter(), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_mirror_view(PUPPETXR_MIRROR_BOTH), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.screenshot("/tmp/x.png"), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.record_start("/tmp/x.xrrec"), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.record_stop(), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.replay_start("/tmp/x.xrrec"), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.replay_stop(), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.request_exit(), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.get_controller(&controller), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_controller(PUPPETXR_CONTROLLER_KHR_SIMPLE), PUPPETXR_ERROR_NO_INSTANCE);
    char room[64];
    EXPECT_EQ(api.get_room(room, sizeof(room)), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_room("bedroom"), PUPPETXR_ERROR_NO_INSTANCE);
    PUPPETXR_ControlMode mode = {};
    EXPECT_EQ(api.get_control_mode(&mode), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_control_mode(PUPPETXR_CONTROL_MODE_HEAD), PUPPETXR_ERROR_NO_INSTANCE);
    char profile[32];
    EXPECT_EQ(api.get_profile(profile, sizeof(profile)), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_profile("hands"), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.reset_poses(), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.reset_hands(), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.point_hand(PUPPETXR_DEVICE_RIGHT_HAND, 0, 0.5f, 0.5f, nullptr), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.control_key("w", 1), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.control_mouse(1.0f, 0.0f), PUPPETXR_ERROR_NO_INSTANCE);
    PUPPETXR_U32 count = 0;
    EXPECT_EQ(api.get_anchors(nullptr, 0, &count), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.wipe_anchors(0), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.get_origin_offset(&pose), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_tracking_drift(&pose), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_tracking_drift_rate(nullptr, 1.0f), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.get_tracking_drift(&pose, nullptr, nullptr), PUPPETXR_ERROR_NO_INSTANCE);
    PUPPETXR_HandSource source = PUPPETXR_HAND_SOURCE_CONTROLLER;
    EXPECT_EQ(api.get_hand_source(PUPPETXR_DEVICE_LEFT_HAND, &source), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_hand_source(PUPPETXR_DEVICE_LEFT_HAND, PUPPETXR_HAND_SOURCE_HAND), PUPPETXR_ERROR_NO_INSTANCE);
    PUPPETXR_VirtualKeyboard keyboard = { .struct_size = sizeof(PUPPETXR_VirtualKeyboard) };
    EXPECT_EQ(api.get_virtual_keyboard(&keyboard), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.get_virtual_keyboard_key("q", &pose), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.type_text("a"), PUPPETXR_ERROR_NO_INSTANCE);
    PUPPETXR_Prop prop = api_qr("a", 0, 0, 0);
    PUPPETXR_U32 id = 0;
    EXPECT_EQ(api.add_prop(&prop, &id), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.update_prop(1, &prop), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.remove_prop(1), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.get_props(nullptr, 0, &count), PUPPETXR_ERROR_NO_INSTANCE);
}

TEST_F(ApiTest, poses_inputs_and_status)
{
    start();
    PUPPETXR_Status status = { .struct_size = sizeof(PUPPETXR_Status) };
    ASSERT_EQ(api.get_status(&status), PUPPETXR_SUCCESS);
    const u64 hash = status.state_hash;
    EXPECT_EQ(status.recording, 0);
    EXPECT_EQ(status.replaying, 0);

    const PUPPETXR_Pose pose = { { 0.5f, 1.25f, -0.75f }, { 0.0f, 0.0f, 0.0f, 1.0f } };
    EXPECT_EQ(api.set_device_pose(PUPPETXR_DEVICE_RIGHT_HAND, &pose), PUPPETXR_SUCCESS);
    PUPPETXR_Pose read = {};
    EXPECT_EQ(api.get_device_pose(PUPPETXR_DEVICE_RIGHT_HAND, &read), PUPPETXR_SUCCESS);
    EXPECT_FLOAT_EQ(read.position[0], 0.5f);
    EXPECT_FLOAT_EQ(read.position[2], -0.75f);

    EXPECT_EQ(api.set_input(PUPPETXR_DEVICE_LEFT_HAND, "trigger/value", 0.25f, 0.0f), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_input(PUPPETXR_DEVICE_RIGHT_HAND, "thumbstick", -2.0f, 0.5f), PUPPETXR_SUCCESS);
    f32 x = 0;
    f32 y = 0;
    EXPECT_EQ(api.get_input(PUPPETXR_DEVICE_LEFT_HAND, "trigger/value", &x, &y), PUPPETXR_SUCCESS);
    EXPECT_FLOAT_EQ(x, 0.25f);
    EXPECT_EQ(api.get_input(PUPPETXR_DEVICE_RIGHT_HAND, "thumbstick", &x, &y), PUPPETXR_SUCCESS);
    EXPECT_FLOAT_EQ(x, -1.0f);
    EXPECT_FLOAT_EQ(y, 0.5f);
    EXPECT_EQ(api.set_device_active(PUPPETXR_DEVICE_LEFT_HAND, 0), PUPPETXR_SUCCESS);
    EXPECT_FALSE(sim_get_device_state(oxr_get_sim(), SIM_Device::LeftHand).active);

    ASSERT_EQ(api.get_status(&status), PUPPETXR_SUCCESS);
    EXPECT_NE(status.state_hash, hash);
    const u64 frame_before = status.frame_index;
    frame();
    ASSERT_EQ(api.get_status(&status), PUPPETXR_SUCCESS);
    EXPECT_EQ(status.frame_index, frame_before + 1);

    EXPECT_EQ(api.recenter(), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_fixed_step(0), PUPPETXR_SUCCESS);
    EXPECT_EQ(sim_clock_mode(oxr_get_sim()), SIM_ClockMode::Realtime);
    EXPECT_EQ(api.set_controls_enabled(0), PUPPETXR_SUCCESS);
    EXPECT_FALSE(sim_controls_enabled(oxr_get_sim()));
    EXPECT_EQ(api.set_mirror_view(PUPPETXR_MIRROR_RIGHT), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.screenshot("/tmp/puppetxr_api_headless.png"), PUPPETXR_ERROR_NO_GRAPHICS);
}

TEST_F(ApiTest, keyboard_controls_through_the_api)
{
    start();
    const SIM_Sim*    sim  = oxr_get_sim();
    PUPPETXR_ControlMode mode = PUPPETXR_CONTROL_MODE_MAX_ENUM;
    EXPECT_EQ(api.get_control_mode(&mode), PUPPETXR_SUCCESS);
    EXPECT_EQ(mode, PUPPETXR_CONTROL_MODE_BODY);
    EXPECT_EQ(api.set_control_mode(PUPPETXR_CONTROL_MODE_LEFT_HAND), PUPPETXR_SUCCESS);
    EXPECT_STREQ(sim_get_config(sim).profiles[sim_get_profile(sim)].name, "left");
    PUPPETXR_Status status = { .struct_size = sizeof(PUPPETXR_Status) };
    ASSERT_EQ(api.get_status(&status), PUPPETXR_SUCCESS);
    EXPECT_EQ(status.control_mode, (i32)PUPPETXR_CONTROL_MODE_LEFT_HAND);

    const Pose left = sim_get_device_pose(sim, SIM_Device::LeftHand);
    EXPECT_EQ(api.control_key("w", 1), PUPPETXR_SUCCESS);
    frame();
    EXPECT_EQ(api.control_key("w", 0), PUPPETXR_SUCCESS);
    EXPECT_GT(sim_get_device_pose(sim, SIM_Device::LeftHand).position.y, left.position.y);

    // Keys drive what a person would: Shift then I ramps the left stick
    EXPECT_EQ(api.control_key("left_shift", 1), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.control_key("i", 1), PUPPETXR_SUCCESS);
    frame();
    frame();
    EXPECT_GT(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y, 0.0f);
    EXPECT_LT(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y, 1.0f);
    EXPECT_EQ(api.control_key("i", 0), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.control_key("left_shift", 0), PUPPETXR_SUCCESS);

    const Pose head = sim_get_device_pose(sim, SIM_Device::Head);
    EXPECT_EQ(api.set_control_mode(PUPPETXR_CONTROL_MODE_HEAD), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.control_mouse(100.0f, 0.0f), PUPPETXR_SUCCESS);
    frame();
    EXPECT_FALSE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head).orientation, head.orientation));

    EXPECT_EQ(api.reset_poses(), PUPPETXR_SUCCESS);
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::LeftHand), left));
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head), head));

    EXPECT_EQ(api.control_key("w", 1), PUPPETXR_SUCCESS);
    frame();
    EXPECT_EQ(api.control_key("w", 0), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.reset_hands(), PUPPETXR_SUCCESS);
    const Pose moved = sim_get_device_pose(sim, SIM_Device::Head);
    EXPECT_NEAR(sim_get_device_pose(sim, SIM_Device::LeftHand).position.z - moved.position.z, left.position.z - head.position.z, 1e-5f);

    EXPECT_EQ(api.control_key("not_a_key", 1), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.control_key(nullptr, 1), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.control_key("none", 1), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.control_mouse(zm::nan(), 0.0f), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_control_mode((PUPPETXR_ControlMode)4), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_control_mode(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
}

TEST_F(ApiTest, profiles_by_name)
{
    start();
    const SIM_Sim* sim = oxr_get_sim();
    char           name[32] = {};
    EXPECT_EQ(api.get_profile(name, sizeof(name)), PUPPETXR_SUCCESS);
    EXPECT_STREQ(name, "body");
    EXPECT_EQ(api.set_profile("hands"), PUPPETXR_SUCCESS);
    EXPECT_STREQ(sim_get_config(sim).profiles[sim_get_profile(sim)].name, "hands");
    EXPECT_EQ(api.get_profile(name, sizeof(name)), PUPPETXR_SUCCESS);
    EXPECT_STREQ(name, "hands");

    PUPPETXR_ControlMode mode = PUPPETXR_CONTROL_MODE_BODY;
    EXPECT_EQ(api.get_control_mode(&mode), PUPPETXR_ERROR_NOT_AVAILABLE) << "no control mode is called hands";
    PUPPETXR_Status status = { .struct_size = sizeof(PUPPETXR_Status) };
    ASSERT_EQ(api.get_status(&status), PUPPETXR_SUCCESS);
    EXPECT_EQ(status.control_mode, -1);
    EXPECT_STREQ(status.profile, "hands");

    EXPECT_EQ(api.set_profile("nonsense"), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_profile(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_profile(nullptr, 32), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_profile(name, 3), PUPPETXR_ERROR_INVALID_ARGUMENT) << "hands does not fit";
    EXPECT_STREQ(sim_get_config(sim).profiles[sim_get_profile(sim)].name, "hands");
}

TEST_F(ApiTest, pointing_aims_a_hand_through_an_image_point)
{
    start();
    SIM_Sim* sim = oxr_get_sim();
    EXPECT_EQ(api.point_hand(PUPPETXR_DEVICE_LEFT_HAND, 1, 0.25f, 0.5f, nullptr), PUPPETXR_SUCCESS);
    Pose eye = {};
    Fov  fov = {};
    sim_get_view(sim, 1, eye, fov);
    const Pose aim   = sim_get_hand_pose(sim, SIM_Device::LeftHand, SIM_HandPose::Aim);
    const Vec3 local = zm::rotate(zm::conjugate(eye.orientation), zm::rotate(aim.orientation, { 0.0f, 0.0f, -1.0f }));
    EXPECT_NEAR(zm::length(aim.position - eye.position), 0.0f, 1e-5f);
    EXPECT_NEAR(local.x / -local.z, -0.5f * zm::tan(fov.angle_right), 1e-5f);

    EXPECT_EQ(api.point_hand(PUPPETXR_DEVICE_HEAD, 0, 0.5f, 0.5f, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.point_hand(PUPPETXR_DEVICE_LEFT_HAND, 2, 0.5f, 0.5f, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.point_hand(PUPPETXR_DEVICE_LEFT_HAND, 0, zm::nan(), 0.5f, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.point_hand(PUPPETXR_DEVICE_LEFT_HAND, 0, 0.5f, 1.5f, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    const PUPPETXR_Pose zero_quat = { { 0, 0, 0 }, { 0, 0, 0, 0 } };
    EXPECT_EQ(api.point_hand(PUPPETXR_DEVICE_LEFT_HAND, 0, 0.5f, 0.5f, &zero_quat), PUPPETXR_ERROR_INVALID_ARGUMENT);

    // The ray from the grip itself lands the grip on the eye
    const PUPPETXR_Pose grip = { { 0, 0, 0 }, { 0, 0, 0, 1 } };
    EXPECT_EQ(api.point_hand(PUPPETXR_DEVICE_LEFT_HAND, 0, 0.5f, 0.5f, &grip), PUPPETXR_SUCCESS);
    sim_get_view(sim, 0, eye, fov);
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::LeftHand), eye));
}

TEST_F(ApiTest, invalid_arguments_are_rejected)
{
    start();
    const PUPPETXR_Pose nan_pose = { { zm::nan(), 0, 0 }, { 0, 0, 0, 1 } };
    const PUPPETXR_Pose zero_quat = { { 0, 0, 0 }, { 0, 0, 0, 0 } };
    const PUPPETXR_Pose ok     = { { 0, 1, 0 }, { 0, 0, 0, 1 } };
    PUPPETXR_Pose    out;
    f32              x = 0.0f;
    EXPECT_EQ(api.set_device_pose(PUPPETXR_DEVICE_HEAD, &nan_pose), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_device_pose(PUPPETXR_DEVICE_HEAD, &zero_quat), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_device_pose(PUPPETXR_DEVICE_HEAD, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_device_pose((PUPPETXR_Device)7, &ok), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_device_pose((PUPPETXR_Device)0x7ffffffe, &out), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_device_pose(PUPPETXR_DEVICE_HEAD, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_device_active((PUPPETXR_Device)3, 1), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_input(PUPPETXR_DEVICE_LEFT_HAND, "not/an/input", 1, 0), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_input(PUPPETXR_DEVICE_LEFT_HAND, nullptr, 1, 0), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_input(PUPPETXR_DEVICE_HEAD, "a/click", 1, 0), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_input(PUPPETXR_DEVICE_LEFT_HAND, "trigger/value", INFINITY, 0), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_input(PUPPETXR_DEVICE_LEFT_HAND, "nope", &x, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_input(PUPPETXR_DEVICE_LEFT_HAND, "trigger/value", nullptr, nullptr), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_mirror_view((PUPPETXR_MirrorView)9), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_status(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.screenshot(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.screenshot(""), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.record_start(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.record_stop(), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.record_start("/nonexistent/dir/a.xrrec"), PUPPETXR_ERROR_IO);
    EXPECT_EQ(api.replay_start("/nonexistent/a.xrrec"), PUPPETXR_ERROR_IO);
    EXPECT_EQ(api.replay_start(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
}

TEST_F(ApiTest, record_and_replay_through_the_api)
{
    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxr_api_session.xrrec");

    start();
    ASSERT_EQ(api.record_start(path), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.record_start(path), PUPPETXR_ERROR_INVALID_ARGUMENT);
    u64 hashes[30] = {};
    for (u32 f = 0; f < 30; f++)
    {
        const PUPPETXR_Pose pose = { { 0.01f * (f32)f, 1.6f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } };
        api.set_device_pose(PUPPETXR_DEVICE_HEAD, &pose);
        api.set_input(PUPPETXR_DEVICE_RIGHT_HAND, "squeeze/value", (f32)(f % 5) * 0.25f, 0.0f);
        frame();
        PUPPETXR_Status status = { .struct_size = sizeof(PUPPETXR_Status) };
        api.get_status(&status);
        hashes[f] = status.state_hash;
    }
    PUPPETXR_Status status = { .struct_size = sizeof(PUPPETXR_Status) };
    api.get_status(&status);
    EXPECT_EQ(status.recording, 1);
    EXPECT_EQ(status.recorded_frames, 30u);
    ASSERT_EQ(api.record_stop(), PUPPETXR_SUCCESS);
    stop();

    start();
    ASSERT_EQ(api.replay_start(path), PUPPETXR_SUCCESS);
    api.get_status(&status);
    EXPECT_EQ(status.replaying, 1);
    EXPECT_EQ(status.replay_total, 30u);

    const PUPPETXR_Pose pose = { { 0, 1, 0 }, { 0, 0, 0, 1 } };
    EXPECT_EQ(api.set_device_pose(PUPPETXR_DEVICE_HEAD, &pose), PUPPETXR_ERROR_REPLAYING);
    EXPECT_EQ(api.set_input(PUPPETXR_DEVICE_LEFT_HAND, "a/click", 1, 0), PUPPETXR_ERROR_REPLAYING);
    EXPECT_EQ(api.set_device_active(PUPPETXR_DEVICE_LEFT_HAND, 0), PUPPETXR_ERROR_REPLAYING);
    EXPECT_EQ(api.set_controls_enabled(1), PUPPETXR_ERROR_REPLAYING);
    EXPECT_EQ(api.recenter(), PUPPETXR_ERROR_REPLAYING);
    EXPECT_EQ(api.reset_poses(), PUPPETXR_ERROR_REPLAYING);
    EXPECT_EQ(api.reset_hands(), PUPPETXR_ERROR_REPLAYING);
    EXPECT_EQ(api.point_hand(PUPPETXR_DEVICE_RIGHT_HAND, 0, 0.5f, 0.5f, nullptr), PUPPETXR_ERROR_REPLAYING);
    EXPECT_EQ(api.control_key("w", 1), PUPPETXR_ERROR_REPLAYING);
    EXPECT_EQ(api.control_mouse(1.0f, 0.0f), PUPPETXR_ERROR_REPLAYING);
    EXPECT_EQ(api.set_tracking_drift(&pose), PUPPETXR_ERROR_REPLAYING);
    EXPECT_EQ(api.set_tracking_drift_rate(nullptr, 1.0f), PUPPETXR_ERROR_REPLAYING);

    for (u32 f = 0; f < 30; f++)
    {
        frame();
        api.get_status(&status);
        EXPECT_EQ(status.state_hash, hashes[f]) << "frame " << f;
    }
    EXPECT_EQ(status.divergences, 0u);
    EXPECT_EQ(status.replay_frame, 30u);

    frame();
    api.get_status(&status);
    EXPECT_EQ(status.replaying, 0);
    EXPECT_EQ(status.replay_finished, 1);
    EXPECT_EQ(last_state(), XR_SESSION_STATE_STOPPING);
    EXPECT_EQ(api.set_device_pose(PUPPETXR_DEVICE_HEAD, &pose), PUPPETXR_SUCCESS);
}

TEST_F(ApiTest, tracking_drift_steps_and_drifts)
{
    start();
    PUPPETXR_Pose drift    = { { 1, 1, 1 }, { 1, 0, 0, 0 } };
    f32        velocity[3] = { 1, 1, 1 };
    f32        yaw_rate    = 1.0f;
    ASSERT_EQ(api.get_tracking_drift(&drift, velocity, &yaw_rate), PUPPETXR_SUCCESS);
    EXPECT_FLOAT_EQ(drift.orientation[3], 1.0f);
    EXPECT_FLOAT_EQ(drift.position[0], 0.0f);
    EXPECT_FLOAT_EQ(velocity[0], 0.0f);
    EXPECT_FLOAT_EQ(yaw_rate, 0.0f);
    EXPECT_EQ(api.get_tracking_drift(nullptr, nullptr, nullptr), PUPPETXR_SUCCESS);

    const PUPPETXR_Pose step = { { 0.25f, 0.0f, -0.5f }, { 0.0f, 0.38268343f, 0.0f, 0.92387953f } };
    ASSERT_EQ(api.set_tracking_drift(&step), PUPPETXR_SUCCESS);
    const Pose set = sim_get_tracking_drift(oxr_get_sim());
    EXPECT_FLOAT_EQ(set.position.x, 0.25f);
    EXPECT_NEAR(set.orientation.y, 0.38268343f, 1e-6f);
    PUPPETXR_Pose head_before = {};
    ASSERT_EQ(api.get_device_pose(PUPPETXR_DEVICE_HEAD, &head_before), PUPPETXR_SUCCESS);
    frame();
    PUPPETXR_Pose head_after = {};
    ASSERT_EQ(api.get_device_pose(PUPPETXR_DEVICE_HEAD, &head_after), PUPPETXR_SUCCESS);
    EXPECT_TRUE(memory_equal(&head_before, &head_after, sizeof(PUPPETXR_Pose))) << "device poses are physical, drift leaves them";

    const f32        nan   = zm::sqrt(-1.0f);
    const PUPPETXR_Pose bad[] = {
        { { nan, 0, 0 }, { 0, 0, 0, 1 } },
        { { 0, 0, 0 }, { 0, 0, 0, 0 } },
        { { 0, 0, 0 }, { 0, 0, 0, 3 } },
        { { 0, 0, 0 }, { 0.1f, 0, 0, 0.995f } },
    };
    for (const PUPPETXR_Pose& pose : bad)
    {
        EXPECT_EQ(api.set_tracking_drift(&pose), PUPPETXR_ERROR_INVALID_ARGUMENT);
    }
    const f32 bad_velocity[3] = { 0, nan, 0 };
    EXPECT_EQ(api.set_tracking_drift_rate(bad_velocity, 0.0f), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_tracking_drift_rate(nullptr, nan), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_TRUE(zm::equal(sim_get_tracking_drift(oxr_get_sim()), set)) << "refused input changes nothing";

    const f32 rate[3] = { 0.03f, 0.0f, 0.0f };
    ASSERT_EQ(api.set_tracking_drift_rate(rate, -6.0f), PUPPETXR_SUCCESS);
    ASSERT_EQ(api.get_tracking_drift(nullptr, velocity, &yaw_rate), PUPPETXR_SUCCESS);
    EXPECT_FLOAT_EQ(velocity[0], 0.03f);
    EXPECT_FLOAT_EQ(yaw_rate, -6.0f);
    for (u32 f = 0; f < 90; f++)
    {
        frame();
    }
    ASSERT_EQ(api.get_tracking_drift(&drift, nullptr, nullptr), PUPPETXR_SUCCESS);
    EXPECT_NEAR(drift.position[0], 0.28f, 1e-4f) << "a second at 3 cm a second";

    ASSERT_EQ(api.set_tracking_drift(nullptr), PUPPETXR_SUCCESS);
    ASSERT_EQ(api.set_tracking_drift_rate(nullptr, 0.0f), PUPPETXR_SUCCESS);
    frame();
    ASSERT_EQ(api.get_tracking_drift(&drift, velocity, &yaw_rate), PUPPETXR_SUCCESS);
    EXPECT_FLOAT_EQ(drift.position[0], 0.0f);
    EXPECT_FLOAT_EQ(drift.orientation[3], 1.0f);
    EXPECT_FLOAT_EQ(velocity[0], 0.0f);
    EXPECT_FLOAT_EQ(yaw_rate, 0.0f);
}

TEST_F(ApiTest, frame_faults_fail_the_next_frame_calls)
{
    ASSERT_EQ(api.set_frame_fault(PUPPETXR_FRAME_OP_WAIT, XR_ERROR_SESSION_LOST, 1), PUPPETXR_SUCCESS) << "for the next instance";
    start();
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    EXPECT_EQ(wait_frame(session, nullptr, &state), XR_ERROR_SESSION_LOST);
    frame();

    ASSERT_EQ(api.set_frame_fault(PUPPETXR_FRAME_OP_BEGIN, XR_ERROR_CALL_ORDER_INVALID, 2), PUPPETXR_SUCCESS);
    ASSERT_EQ(wait_frame(session, nullptr, &state), XR_SUCCESS);
    EXPECT_EQ(begin_frame(session, nullptr), XR_ERROR_CALL_ORDER_INVALID);
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    EXPECT_EQ(end_frame(session, &end_info), XR_ERROR_CALL_ORDER_INVALID) << "no frame was begun";
    ASSERT_EQ(api.set_frame_fault(PUPPETXR_FRAME_OP_BEGIN, 0, 0), PUPPETXR_SUCCESS);
    frame();

    ASSERT_EQ(api.set_frame_fault(PUPPETXR_FRAME_OP_END, XR_ERROR_LAYER_LIMIT_EXCEEDED, 1), PUPPETXR_SUCCESS);
    ASSERT_EQ(wait_frame(session, nullptr, &state), XR_SUCCESS);
    ASSERT_EQ(begin_frame(session, nullptr), XR_SUCCESS);
    const XrFrameEndInfo next_end = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    EXPECT_EQ(end_frame(session, &next_end), XR_ERROR_LAYER_LIMIT_EXCEEDED);
    EXPECT_EQ(end_frame(session, &next_end), XR_SUCCESS);

    EXPECT_EQ(api.set_frame_fault(PUPPETXR_FRAME_OP_WAIT, XR_ERROR_CALL_ORDER_INVALID, 1), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_frame_fault(PUPPETXR_FRAME_OP_LOCATE_VIEWS, XR_SUCCESS, 1), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_frame_fault((PUPPETXR_FrameOp)4, XR_ERROR_RUNTIME_FAILURE, 1), PUPPETXR_ERROR_INVALID_ARGUMENT);
    // The largest value the enum holds; C++ cannot pass -1 as one, which C callers turn into this
    // range check's 0xffffffff
    EXPECT_EQ(api.set_frame_fault(PUPPETXR_FRAME_OP_MAX_ENUM, XR_ERROR_RUNTIME_FAILURE, 1), PUPPETXR_ERROR_INVALID_ARGUMENT);
    frame();

    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxr_api_frame_fault_busy.xrrec");
    ASSERT_EQ(api.record_start(path), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_frame_fault(PUPPETXR_FRAME_OP_WAIT, XR_ERROR_RUNTIME_FAILURE, 1), PUPPETXR_ERROR_BUSY);
    frame();
    ASSERT_EQ(api.record_stop(), PUPPETXR_SUCCESS);
    os_delete_file(path);
}

TEST_F(ApiTest, replay_can_be_stopped_and_exit_requested)
{
    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxr_api_stop.xrrec");
    start();
    ASSERT_EQ(api.record_start(path), PUPPETXR_SUCCESS);
    for (u32 f = 0; f < 10; f++)
    {
        frame();
    }
    ASSERT_EQ(api.record_stop(), PUPPETXR_SUCCESS);
    ASSERT_EQ(api.replay_start(path), PUPPETXR_SUCCESS);
    frame();
    EXPECT_EQ(api.replay_stop(), PUPPETXR_SUCCESS);
    PUPPETXR_Status status = { .struct_size = sizeof(PUPPETXR_Status) };
    api.get_status(&status);
    EXPECT_EQ(status.replaying, 0);
    EXPECT_TRUE(sim_controls_enabled(oxr_get_sim()));

    last_state();
    EXPECT_EQ(api.request_exit(), PUPPETXR_SUCCESS);
    frame();
    EXPECT_EQ(last_state(), XR_SESSION_STATE_STOPPING);
}

//
// Rooms
//

namespace
{

struct ApiRoomTest : ApiTest
{
    char rooms_dir[OS_MAX_PATH] = {};

    void SetUp () override
    {
        ApiTest::SetUp();
        ASSERT_TRUE(os_create_temp_directory("puppetxr_api_rooms", rooms_dir, sizeof(rooms_dir)));
        TestRoom room = {};
        test_room_build(room);
        ASSERT_TRUE(test_room_write(room, rooms_dir, "test_box"));
        ASSERT_TRUE(test_room_replace(room, "\"name\": \"test_box\"", "\"name\": \"other_box\""));
        ASSERT_TRUE(test_room_write(room, rooms_dir, "other_box"));
        os_set_env("PUPPETXR_ROOMS_DIR", rooms_dir);
        os_unset_env("PUPPETXR_ROOM");
    }

    void TearDown () override
    {
        ApiTest::TearDown();
        os_delete_tree(rooms_dir);
        os_unset_env("PUPPETXR_ROOMS_DIR");
        os_unset_env("PUPPETXR_ROOM");
    }

    char room_name[64] = {};

    const char* room ()
    {
        cstr_copy(room_name, sizeof(room_name), "unset");
        EXPECT_EQ(api.get_room(room_name, sizeof(room_name)), PUPPETXR_SUCCESS);
        return room_name;
    }
};

} // namespace

TEST_F(ApiRoomTest, rooms_chosen_in_the_ui_are_remembered)
{
    const char* saved = os_env("PUPPETXR_STATE_DIR");
    char        saved_value[OS_MAX_PATH] = {};
    cstr_copy(saved_value, sizeof(saved_value), saved ? saved : "");
    char state_dir[OS_MAX_PATH];
    cstr_format(state_dir, sizeof(state_dir), "%s/puppetxr", rooms_dir);
    os_set_env("PUPPETXR_STATE_DIR", state_dir);
    defer { saved ? os_set_env("PUPPETXR_STATE_DIR", saved_value) : os_unset_env("PUPPETXR_STATE_DIR"); };
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/state.json", state_dir);
    SIM_State state = {};

    start();
    EXPECT_EQ(oxr_choose_room("test_box"), OXR_RoomResult::Ok);
    EXPECT_STREQ(room(), "test_box");
    ASSERT_TRUE(sim_load_state(path, state));
    EXPECT_STREQ(state.room, "test_box");

    // Agents and scripts change the room without touching the user's default
    EXPECT_EQ(api.set_room("other_box"), PUPPETXR_SUCCESS);
    ASSERT_TRUE(sim_load_state(path, state));
    EXPECT_STREQ(state.room, "test_box");

    EXPECT_EQ(oxr_choose_room(""), OXR_RoomResult::Ok);
    ASSERT_TRUE(sim_load_state(path, state));
    EXPECT_TRUE(state.has_room);
    EXPECT_STREQ(state.room, "");
    EXPECT_EQ(oxr_choose_room("no_such_room"), OXR_RoomResult::NotFound);
    EXPECT_EQ(oxr_choose_room("../escape"), OXR_RoomResult::NotFound);
    ASSERT_TRUE(sim_load_state(path, state));
    EXPECT_STREQ(state.room, "");

    // Headless starts ignore the remembered room
    EXPECT_EQ(oxr_choose_room("test_box"), OXR_RoomResult::Ok);
    stop();
    start();
    EXPECT_STREQ(room(), "living_room");
}

// The rooms dir lacks it: the default room is the one compiled into the library
TEST_F(ApiRoomTest, living_room_unless_configured)
{
    start();
    EXPECT_STREQ(room(), "living_room");
    EXPECT_EQ(api.set_room("office"), PUPPETXR_SUCCESS);
    EXPECT_STREQ(room(), "office");
}

TEST_F(ApiRoomTest, room_from_the_environment)
{
    os_set_env("PUPPETXR_ROOM", "test_box");
    start();
    EXPECT_STREQ(room(), "test_box");
}

TEST_F(ApiRoomTest, a_missing_configured_room_leaves_none)
{
    os_set_env("PUPPETXR_ROOM", "nowhere");
    start();
    EXPECT_STREQ(room(), "");
}

TEST_F(ApiRoomTest, rooms_switch_at_runtime)
{
    start();
    EXPECT_EQ(api.set_room("test_box"), PUPPETXR_SUCCESS);
    EXPECT_STREQ(room(), "test_box");
    frame();
    EXPECT_EQ(api.set_room("other_box"), PUPPETXR_SUCCESS);
    EXPECT_STREQ(room(), "other_box");
    EXPECT_EQ(api.set_room("nowhere"), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_room("../test_box"), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_STREQ(room(), "other_box");
    EXPECT_EQ(api.set_room(nullptr), PUPPETXR_SUCCESS);
    EXPECT_STREQ(room(), "");
    EXPECT_EQ(api.set_room("test_box"), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_room(""), PUPPETXR_SUCCESS);
    EXPECT_STREQ(room(), "");

    char tiny[4];
    EXPECT_EQ(api.set_room("test_box"), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.get_room(tiny, sizeof(tiny)), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_room(nullptr, 16), PUPPETXR_ERROR_INVALID_ARGUMENT);
}

TEST_F(ApiRoomTest, recordings_carry_their_room)
{
    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxr_api_room.xrrec");

    os_set_env("PUPPETXR_ROOM", "test_box");
    start();
    ASSERT_EQ(api.record_start(path), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_room("other_box"), PUPPETXR_ERROR_BUSY);
    for (u32 f = 0; f < 5; f++)
    {
        frame();
    }
    ASSERT_EQ(api.record_stop(), PUPPETXR_SUCCESS);
    stop();

    os_unset_env("PUPPETXR_ROOM");
    start();
    EXPECT_STREQ(room(), "living_room");
    ASSERT_EQ(api.replay_start(path), PUPPETXR_SUCCESS);
    EXPECT_STREQ(room(), "test_box");
    EXPECT_EQ(api.set_room("other_box"), PUPPETXR_ERROR_BUSY);
    api.replay_stop();
    stop();

    // A room whose files changed since the recording cannot replay it faithfully
    TestRoom changed = {};
    test_room_build(changed);
    changed.bin[0] ^= 1;
    ASSERT_TRUE(test_room_write(changed, rooms_dir, "test_box"));
    start();
    EXPECT_EQ(api.replay_start(path), PUPPETXR_ERROR_IO);
    EXPECT_STREQ(room(), "living_room") << "the room before the failed replay stays";
}

TEST_F(ApiRoomTest, replay_from_config_loads_the_recorded_room)
{
    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxr_api_room_config.xrrec");
    os_set_env("PUPPETXR_ROOM", "other_box");
    start();
    ASSERT_EQ(api.record_start(path), PUPPETXR_SUCCESS);
    frame();
    ASSERT_EQ(api.record_stop(), PUPPETXR_SUCCESS);
    stop();

    os_set_env("PUPPETXR_ROOM", "test_box");
    os_set_env("PUPPETXR_REPLAY", path);
    start();
    os_unset_env("PUPPETXR_REPLAY");
    EXPECT_STREQ(room(), "other_box");
}

//
// Control server
//

namespace
{

struct ApiControlTest : ApiTest
{
    OS_Socket* client = nullptr;
    char       discovery[OS_MAX_PATH] = {};

    void SetUp () override
    {
        os_set_env("PUPPETXR_CONTROL", "1");
        ApiTest::SetUp();
        char dir[OS_MAX_PATH];
        ASSERT_TRUE(os_runtime_dir(dir, sizeof(dir)));
        cstr_format(discovery, sizeof(discovery), "%s/puppetxr/%u.json", dir, os_process_id());
    }

    void TearDown () override
    {
        os_socket_close(client);
        ApiTest::TearDown();
        os_unset_env("PUPPETXR_CONTROL");
    }

    u16 discovered_port ()
    {
        JsonParser* parser = json_parse_from_file(discovery);
        if (!parser)
        {
            return 0;
        }
        u32 port = 0;
        json_find_uint(json_get_root(parser), "port", port);
        json_release(parser);
        return (u16)port;
    }

    void connect ()
    {
        client = os_socket_connect("127.0.0.1", discovered_port(), 1000);
        ASSERT_NE(client, nullptr);
    }

    // Sends one request line and runs frames until its reply arrives; the server answers from
    // xrWaitFrame. Bounded by time, not frames: headless frames take next to nothing, and 20 of them
    // passed before a loaded macOS runner answered, so each reply went to the next request
    JsonParser* request (const char* line)
    {
        usize sent = 0;
        EXPECT_EQ(os_socket_send(client, line, cstr_length(line), sent), OS_SocketResult::Ok);
        EXPECT_EQ(os_socket_send(client, "\n", 1, sent), OS_SocketResult::Ok);
        static char reply[8192];
        usize       length = 0;
        const u64   start  = os_monotonic_ns();
        while (os_monotonic_ns() - start < 5000000000ull)
        {
            frame();
            usize received = 0;
            while (os_socket_receive(client, reply + length, sizeof(reply) - 1 - length, received) == OS_SocketResult::Ok)
            {
                length += received;
            }
            if (length > 0 && reply[length - 1] == '\n')
            {
                reply[length] = 0;
                return json_parse_from_string(reply);
            }
        }
        return nullptr;
    }

    void hello ()
    {
        char line[128];
        cstr_format(line, sizeof(line), "{\"id\":1,\"cmd\":\"hello\",\"version\":\"%u.%u.%u\"}", PUPPETXR_BUILD_VERSION_MAJOR, PUPPETXR_BUILD_VERSION_MINOR, PUPPETXR_BUILD_VERSION_PATCH);
        JsonParser* reply = request(line);
        ASSERT_NE(reply, nullptr);
        b8 ok = false;
        json_find_bool(json_get_root(reply), "ok", ok);
        EXPECT_TRUE(ok);
        json_release(reply);
    }

    b8 reply_ok (JsonParser* reply)
    {
        b8 ok = false;
        return reply && json_find_bool(json_get_root(reply), "ok", ok) && ok;
    }

    void expect_error (const char* line, const char* code)
    {
        JsonParser* reply = request(line);
        ASSERT_NE(reply, nullptr) << line;
        EXPECT_FALSE(reply_ok(reply)) << line;
        char error[64] = {};
        json_find_string(json_get_root(reply), "error", error, nullptr, sizeof(error));
        EXPECT_STREQ(error, code) << line;
        json_release(reply);
    }
};

} // namespace

TEST_F(ApiTest, control_server_is_off_by_default)
{
    start();
    char dir[OS_MAX_PATH];
    ASSERT_TRUE(os_runtime_dir(dir, sizeof(dir)));
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/puppetxr/%u.json", dir, os_process_id());
    EXPECT_FALSE(os_file_exists(path));
}

TEST_F(ApiControlTest, discovery_file_lives_with_the_instance)
{
    char dir[OS_MAX_PATH];
    ASSERT_TRUE(os_runtime_dir(dir, sizeof(dir)));
    char stale[OS_MAX_PATH];
    cstr_format(stale, sizeof(stale), "%s/puppetxr", dir);
    ASSERT_TRUE(os_create_directory(stale));
    cstr_format(stale, sizeof(stale), "%s/puppetxr/999999999.json", dir);
    ASSERT_TRUE(os_write_file(stale, "{}", 2));

    start();
    EXPECT_TRUE(os_file_exists(discovery));
    EXPECT_FALSE(os_lock_file_is_stale(discovery));
    EXPECT_FALSE(os_file_exists(stale)); // swept: nobody held it

    JsonParser* parser = json_parse_from_file(discovery);
    ASSERT_NE(parser, nullptr);
    u32  pid      = 0;
    char app[64]  = {};
    char vers[32] = {};
    json_find_uint(json_get_root(parser), "pid", pid);
    json_find_string(json_get_root(parser), "app", app, nullptr, sizeof(app));
    json_find_string(json_get_root(parser), "version", vers, nullptr, sizeof(vers));
    json_release(parser);
    EXPECT_EQ(pid, os_process_id());
    EXPECT_STREQ(app, app_name);
    EXPECT_NE(vers[0], 0);
    EXPECT_NE(discovered_port(), 0);

    stop();
    EXPECT_FALSE(os_file_exists(discovery));
}

TEST_F(ApiControlTest, handshake_needs_the_exact_version)
{
    start();
    connect();
    expect_error("{\"id\":1,\"cmd\":\"status\"}", "hello_first");
    expect_error("{\"id\":2,\"cmd\":\"hello\",\"version\":\"0.0.0-other\"}", "version");
    for (u32 i = 0; i < 5; i++)
    {
        frame();
    }
    char  byte     = 0;
    usize received = 0;
    EXPECT_EQ(os_socket_receive(client, &byte, 1, received), OS_SocketResult::Closed);
}

TEST_F(ApiControlTest, commands_drive_the_simulator)
{
    start();
    connect();
    hello();

    JsonParser* reply = request("{\"id\":3,\"cmd\":\"set_input\",\"device\":\"left\",\"input\":\"trigger/value\",\"value\":[0.5,0]}");
    EXPECT_TRUE(reply_ok(reply));
    json_release(reply);
    reply = request("{\"id\":4,\"cmd\":\"get_input\",\"device\":\"left\",\"input\":\"trigger/value\"}");
    ASSERT_TRUE(reply_ok(reply));
    i32 id = 0;
    json_find_int(json_get_root(reply), "id", id);
    EXPECT_EQ(id, 4);
    f32 x = 0.0f;
    json_as_float(json_at(json_find(json_get_root(reply), "value"), 0), x);
    EXPECT_FLOAT_EQ(x, 0.5f);
    json_release(reply);

    reply = request("{\"id\":5,\"cmd\":\"set_pose\",\"device\":\"right\",\"position\":[0.1,1.2,-0.3],\"orientation\":[0,0,0,1]}");
    EXPECT_TRUE(reply_ok(reply));
    json_release(reply);
    EXPECT_FLOAT_EQ(sim_get_device_pose(oxr_get_sim(), SIM_Device::RightHand).position.y, 1.2f);

    reply = request("{\"id\":6,\"cmd\":\"status\"}");
    ASSERT_TRUE(reply_ok(reply));
    u32 start_frame = 0;
    json_find_uint(json_get_root(reply), "frame", start_frame);
    f32  refresh                   = 0.0f;
    char color_space[16]           = {};
    json_find_float(json_get_root(reply), "refresh_rate_hz", refresh);
    json_find_string(json_get_root(reply), "color_space", color_space, nullptr, sizeof(color_space));
    EXPECT_FLOAT_EQ(refresh, 90.0f);
    EXPECT_STREQ(color_space, "rec709");
    json_release(reply);
    reply = request("{\"id\":7,\"cmd\":\"wait_frames\",\"frames\":3}");
    ASSERT_TRUE(reply_ok(reply));
    u32 waited = 0;
    json_find_uint(json_get_root(reply), "frame", waited);
    json_release(reply);
    EXPECT_GE(waited, start_frame + 3);

    reply = request("{\"id\":8,\"cmd\":\"set_controller\",\"controller\":\"khr_simple\"}");
    EXPECT_TRUE(reply_ok(reply));
    json_release(reply);
    EXPECT_EQ(sim_get_controller(oxr_get_sim()), SIM_Controller::KhrSimple);

    expect_error("{\"id\":9,\"cmd\":\"no_such_command\"}", "unknown_command");
    expect_error("{\"id\":10,\"cmd\":\"set_input\",\"device\":\"nose\",\"input\":\"a/click\",\"value\":1}", "invalid_argument");
    expect_error("not json at all", "bad_request");
    expect_error("{\"id\":11,\"cmd\":\"screenshot\",\"path\":\"/tmp/x.png\"}", "no_graphics");
}

// Regression: a full line buffer that still held whole lines asked the socket for 0 bytes, which
// reads as a closed peer, so a client sending more than one buffer of requests at once was dropped
TEST_F(ApiControlTest, many_requests_in_one_send_are_all_answered)
{
    start();
    connect();
    hello();

    const u32   count = 300;
    Array<char> lines = {};
    defer { lines.release(); };
    for (u32 i = 0; i < count; i++)
    {
        char line[64];
        const usize length = cstr_format(line, sizeof(line), "{\"id\":%u,\"cmd\":\"get_controller\"}\n", 100 + i);
        lines.add(line, length);
    }
    ASSERT_GT(lines.length, 2u * 4096u);
    usize offset = 0;
    for (u32 attempt = 0; attempt < 100 && offset < lines.length; attempt++)
    {
        usize sent = 0;
        os_socket_send(client, lines.data + offset, lines.length - offset, sent);
        offset += sent;
        frame();
    }
    ASSERT_EQ(offset, lines.length);

    u32  replies  = 0;
    char buffer[4096];
    for (u32 i = 0; i < 100 && replies < count; i++)
    {
        frame();
        usize received = 0;
        while (os_socket_receive(client, buffer, sizeof(buffer), received) == OS_SocketResult::Ok)
        {
            for (usize k = 0; k < received; k++)
            {
                replies += buffer[k] == '\n';
            }
        }
    }
    EXPECT_EQ(replies, count);
}

// Runs puppetxrctl against this process while frames keep running, since the server only answers
// between frames; returns its exit code and what it printed
static i32 run_ctl (const ApiControlTest& test, const char* const* args, u32 count, char* out_output, usize output_size, const char* input = nullptr)
{
    char output_path[OS_MAX_PATH];
    char input_path[OS_MAX_PATH];
    // ctest runs every test in its own process at once, so the files carry the pid
    char output_name[64];
    char input_name[64];
    cstr_format(output_name, sizeof(output_name), "puppetxrctl_output_%u.txt", os_process_id());
    cstr_format(input_name, sizeof(input_name), "puppetxrctl_input_%u.txt", os_process_id());
    api_path(output_path, sizeof(output_path), output_name);
    api_path(input_path, sizeof(input_path), input_name);
    if (input)
    {
        os_write_file(input_path, input, cstr_length(input));
    }
    const char* argv[16] = {};
    for (u32 i = 0; i < count + 1 && i < 15; i++)
    {
        argv[i] = i == 0 ? PUPPETXR_CTL_PATH : args[i - 1];
    }
    OS_Process* process = os_process_spawn(argv, input ? input_path : nullptr, output_path);
    if (!process)
    {
        return -1;
    }
    i32 exit_code = -1;
    b8  exited    = false;
    for (u32 i = 0; i < 2000 && !exited; i++)
    {
        test.frame();
        exited = os_process_wait(process, 1, exit_code);
    }
    os_process_release(process);
    u8*   data = nullptr;
    usize size = 0;
    out_output[0] = 0;
    if (os_read_file(output_path, &data, &size))
    {
        const usize copied = size < output_size - 1 ? size : output_size - 1;
        memory_copy(out_output, data, copied);
        out_output[copied] = 0;
        os_memory_release(data);
    }
    return exited ? exit_code : -1;
}

TEST_F(ApiControlTest, haptics_can_be_read)
{
    start();
    connect();
    hello();
    sim_apply_haptic(oxr_get_sim(), SIM_Device::RightHand, SIM_HapticOutput::Main, 0.75f, 2000000000, 160.0f);

    JsonParser* reply = request("{\"id\":2,\"cmd\":\"get_haptic\",\"device\":\"right\"}");
    ASSERT_TRUE(reply_ok(reply));
    const JsonValue root      = json_get_root(reply);
    b8              active    = false;
    f32             amplitude = 0.0f;
    f32             frequency = 0.0f;
    f32             remaining = 0.0f;
    json_find_bool(root, "active", active);
    json_find_float(root, "amplitude", amplitude);
    json_find_float(root, "frequency", frequency);
    json_find_float(root, "remaining_s", remaining);
    json_release(reply);
    EXPECT_TRUE(active);
    EXPECT_FLOAT_EQ(amplitude, 0.75f);
    EXPECT_FLOAT_EQ(frequency, 160.0f);
    EXPECT_GT(remaining, 1.5f);
    EXPECT_LE(remaining, 2.0f);

    sim_apply_haptic(oxr_get_sim(), SIM_Device::RightHand, SIM_HapticOutput::Thumb, 0.25f, 2000000000, 0.0f);
    reply = request("{\"id\":9,\"cmd\":\"get_haptic\",\"device\":\"right\",\"output\":\"thumb\"}");
    ASSERT_TRUE(reply_ok(reply));
    json_find_float(json_get_root(reply), "amplitude", amplitude);
    json_release(reply);
    EXPECT_FLOAT_EQ(amplitude, 0.25f);
    reply = request("{\"id\":10,\"cmd\":\"get_haptic\",\"device\":\"right\",\"output\":\"elbow\"}");
    EXPECT_FALSE(reply_ok(reply));
    json_release(reply);

    reply = request("{\"id\":3,\"cmd\":\"get_haptic\",\"device\":\"left\"}");
    ASSERT_TRUE(reply_ok(reply));
    json_find_bool(json_get_root(reply), "active", active);
    json_release(reply);
    EXPECT_FALSE(active);
    expect_error("{\"id\":4,\"cmd\":\"get_haptic\",\"device\":\"head\"}", "invalid_argument");
}

TEST_F(ApiControlTest, hand_source_can_be_switched)
{
    start();
    connect();
    hello();
    JsonParser* reply = request("{\"id\":2,\"cmd\":\"set_hand_source\",\"device\":\"right\",\"source\":\"hand\"}");
    ASSERT_TRUE(reply_ok(reply));
    json_release(reply);
    EXPECT_EQ(sim_get_hand_source(oxr_get_sim(), SIM_Device::RightHand), SIM_HandSource::Hand);

    reply = request("{\"id\":3,\"cmd\":\"get_hand_source\",\"device\":\"right\"}");
    ASSERT_TRUE(reply_ok(reply));
    char source[16] = {};
    json_find_string(json_get_root(reply), "source", source, nullptr, sizeof(source));
    json_release(reply);
    EXPECT_STREQ(source, "hand");

    expect_error("{\"id\":4,\"cmd\":\"set_hand_source\",\"device\":\"right\",\"source\":\"glove\"}", "invalid_argument");
    expect_error("{\"id\":5,\"cmd\":\"get_hand_source\",\"device\":\"head\"}", "invalid_argument");
}

TEST_F(ApiControlTest, puppetxrctl_types_into_the_virtual_keyboard)
{
    start(XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME);
    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    char output[4096];
    const char* status[] = { "--target", target, "--json", "vkb" };
    EXPECT_EQ(run_ctl(*this, status, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"exists\":false"), nullptr) << output;
    const char* type[] = { "--target", target, "vkb", "type", "a\"b", "c\\nd\\b" };
    EXPECT_NE(run_ctl(*this, type, 6, output, sizeof(output)), 0) << "nothing shown";
    EXPECT_NE(cstr_find(output, "not_available"), nullptr) << output;

    show_keyboard(*this);
    EXPECT_EQ(run_ctl(*this, status, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"visible\":true"), nullptr) << output;
    const char* key[] = { "--target", target, "--json", "vkb", "key", "space" };
    EXPECT_EQ(run_ctl(*this, key, 6, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"position\":["), nullptr) << output;
    last_state();
    EXPECT_EQ(run_ctl(*this, type, 6, output, sizeof(output)), 0) << output;
    frame();
    char typed[32] = {};
    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (poll_event(instance, &event) == XR_SUCCESS)
    {
        const usize used = cstr_length(typed);
        if (event.type == XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_COMMIT_TEXT_META)
        {
            cstr_copy(typed + used, sizeof(typed) - used, ((XrEventDataVirtualKeyboardCommitTextMETA*)&event)->text);
        }
        if (event.type == XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_ENTER_META)
        {
            cstr_copy(typed + used, sizeof(typed) - used, ">");
        }
        if (event.type == XR_TYPE_EVENT_DATA_VIRTUAL_KEYBOARD_BACKSPACE_META)
        {
            cstr_copy(typed + used, sizeof(typed) - used, "<");
        }
        event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
    EXPECT_STREQ(typed, "a\"b c>d<") << "words join with spaces, \\n and \\b are enter and backspace";
}

// Every edit of puppetxrctl body reaches the simulated body; save writes what load reads back
TEST_F(ApiControlTest, puppetxrctl_shapes_and_poses_the_body)
{
    start();
    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    char        output[16384];
    SIM_Sim*    sim = oxr_get_sim();
    const char* joints[] = { "--target", target, "--json", "body" };
    EXPECT_EQ(run_ctl(*this, joints, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"name\":\"right_foot_ball\""), nullptr) << output;

    const char* height[] = { "--target", target, "body", "height", "1.6" };
    EXPECT_EQ(run_ctl(*this, height, 5, output, sizeof(output)), 0) << output;
    EXPECT_FLOAT_EQ(sim_get_body(sim).height, 1.6f);
    const char* bone[] = { "--target", target, "body", "bone", "left_arm_upper", "1.4" };
    EXPECT_EQ(run_ctl(*this, bone, 6, output, sizeof(output)), 0) << output;
    EXPECT_FLOAT_EQ(sim_get_body(sim).bone_scales[(u32)SIM_BodyJoint::LeftArmUpper], 1.4f);
    const char* pose[] = { "--target", target, "body", "pose", "t_pose" };
    EXPECT_EQ(run_ctl(*this, pose, 5, output, sizeof(output)), 0) << output;
    EXPECT_TRUE(zm::equal(sim_get_body(sim).rotations[(u32)SIM_BodyJoint::LeftArmUpper], zm::QUAT_IDENTITY));
    const char* rotate[] = { "--target", target, "body", "joint", "neck", "rot", "0", "0.38268343", "0", "0.92387953" };
    EXPECT_EQ(run_ctl(*this, rotate, 10, output, sizeof(output)), 0) << output;
    EXPECT_FLOAT_EQ(sim_get_body(sim).rotations[(u32)SIM_BodyJoint::Neck].y, 0.38268343f);
    const char* drop[] = { "--target", target, "body", "joint", "hips", "tracked", "off" };
    EXPECT_EQ(run_ctl(*this, drop, 7, output, sizeof(output)), 0) << output;
    EXPECT_FALSE(sim_get_body(sim).tracked[(u32)SIM_BodyJoint::Hips]);
    const char* confidence[] = { "--target", target, "body", "joint", "hips", "confidence", "0.3" };
    EXPECT_EQ(run_ctl(*this, confidence, 7, output, sizeof(output)), 0) << output;
    EXPECT_FLOAT_EQ(sim_get_body(sim).confidences[(u32)SIM_BodyJoint::Hips], 0.3f);
    const char* arm[] = { "--target", target, "body", "arm", "right", "posed" };
    EXPECT_EQ(run_ctl(*this, arm, 6, output, sizeof(output)), 0) << output;
    EXPECT_EQ(sim_get_body(sim).limbs[(u32)SIM_Limb::RightArm], SIM_LimbMode::Posed);
    const char* leg[] = { "--target", target, "body", "leg", "left", "posed" };
    EXPECT_EQ(run_ctl(*this, leg, 6, output, sizeof(output)), 0) << output;
    EXPECT_EQ(sim_get_body(sim).limbs[(u32)SIM_Limb::LeftLeg], SIM_LimbMode::Posed);
    const char* unknown[] = { "--target", target, "body", "bone", "tail", "2" };
    EXPECT_NE(run_ctl(*this, unknown, 6, output, sizeof(output)), 0) << output;

    char name[64];
    char path[OS_MAX_PATH];
    cstr_format(name, sizeof(name), "puppetxrctl_body_%u.json", os_process_id());
    api_path(path, sizeof(path), name);
    const SIM_BodyState edited = sim_get_body(sim);
    const char* save[] = { "--target", target, "body", "save", path };
    EXPECT_EQ(run_ctl(*this, save, 5, output, sizeof(output)), 0) << output;
    const char* reset[] = { "--target", target, "body", "reset" };
    EXPECT_EQ(run_ctl(*this, reset, 4, output, sizeof(output)), 0) << output;
    EXPECT_FLOAT_EQ(sim_get_body(sim).height, 1.75f);
    const char* load[] = { "--target", target, "body", "load", path };
    EXPECT_EQ(run_ctl(*this, load, 5, output, sizeof(output)), 0) << output;
    EXPECT_TRUE(memory_equal(&sim_get_body(sim), &edited, sizeof(edited))) << "load starts from the configured body";
    os_delete_file(path);
}

// puppetxrctl face edits the simulated face; save writes what load reads back
TEST_F(ApiControlTest, puppetxrctl_sets_the_face)
{
    start();
    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    char        output[16384];
    SIM_Sim*    sim = oxr_get_sim();
    const char* show[] = { "--target", target, "--json", "face" };
    EXPECT_EQ(run_ctl(*this, show, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"jaw_drop\""), nullptr) << output;
    EXPECT_NE(cstr_find(output, "\"eyes\""), nullptr) << output;

    const char* preset[] = { "--target", target, "face", "preset", "surprise" };
    EXPECT_EQ(run_ctl(*this, preset, 5, output, sizeof(output)), 0) << output;
    EXPECT_GT(sim_get_face(sim).weights[(u32)SIM_FaceExpression::JawDrop], 0.0f);
    const char* weight[] = { "--target", target, "face", "weight", "jaw_drop", "0.9" };
    EXPECT_EQ(run_ctl(*this, weight, 6, output, sizeof(output)), 0) << output;
    EXPECT_FLOAT_EQ(sim_get_face(sim).weights[(u32)SIM_FaceExpression::JawDrop], 0.9f);
    const char* viseme[] = { "--target", target, "face", "viseme", "oh", "0.7" };
    EXPECT_EQ(run_ctl(*this, viseme, 6, output, sizeof(output)), 0) << output;
    EXPECT_FLOAT_EQ(sim_get_face(sim).visemes[13], 0.7f);
    const char* blink[] = { "--target", target, "face", "blink", "off" };
    EXPECT_EQ(run_ctl(*this, blink, 5, output, sizeof(output)), 0) << output;
    EXPECT_FALSE(sim_get_face(sim).blink);
    const char* gaze[] = { "--target", target, "face", "gaze", "-20", "10" };
    EXPECT_EQ(run_ctl(*this, gaze, 6, output, sizeof(output)), 0) << output;
    EXPECT_FLOAT_EQ(sim_get_face(sim).gaze_yaw, -20.0f);
    const char* look[] = { "--target", target, "face", "look", "0", "1.6", "-2" };
    EXPECT_EQ(run_ctl(*this, look, 7, output, sizeof(output)), 0) << output;
    EXPECT_TRUE(sim_get_face(sim).look_at);
    const char* bad[] = { "--target", target, "face", "weight", "ear_wiggle", "1" };
    EXPECT_NE(run_ctl(*this, bad, 6, output, sizeof(output)), 0) << output;

    char name[64];
    char path[OS_MAX_PATH];
    cstr_format(name, sizeof(name), "puppetxrctl_face_%u.json", os_process_id());
    api_path(path, sizeof(path), name);
    const SIM_FaceState edited = sim_get_face(sim);
    const char* save[] = { "--target", target, "face", "save", path };
    EXPECT_EQ(run_ctl(*this, save, 5, output, sizeof(output)), 0) << output;
    const char* ahead[] = { "--target", target, "face", "look", "ahead" };
    EXPECT_EQ(run_ctl(*this, ahead, 5, output, sizeof(output)), 0) << output;
    EXPECT_FALSE(sim_get_face(sim).look_at);
    const char* reset[] = { "--target", target, "face", "reset" };
    EXPECT_EQ(run_ctl(*this, reset, 4, output, sizeof(output)), 0) << output;
    EXPECT_TRUE(sim_get_face(sim).blink);
    const char* load[] = { "--target", target, "face", "load", path };
    EXPECT_EQ(run_ctl(*this, load, 5, output, sizeof(output)), 0) << output;
    EXPECT_TRUE(memory_equal(&sim_get_face(sim), &edited, sizeof(edited)));
    os_delete_file(path);
}

// An absolute path reaches the simulator as it is; on Windows it starts with a drive letter
TEST_F(ApiControlTest, puppetxrctl_passes_absolute_paths_unchanged)
{
    start();
    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    char name[64];
    char path[OS_MAX_PATH];
    cstr_format(name, sizeof(name), "puppetxrctl_absolute_%u.xrrec", os_process_id());
    api_path(path, sizeof(path), name);
    ASSERT_TRUE(os_path_is_absolute(path)) << path;
    os_delete_file(path);
    char        output[4096];
    const char* record[] = { "--target", target, "record", "start", path };
    EXPECT_EQ(run_ctl(*this, record, 5, output, sizeof(output)), 0) << output;
    const char* stop[] = { "--target", target, "record", "stop" };
    EXPECT_EQ(run_ctl(*this, stop, 4, output, sizeof(output)), 0) << output;
    EXPECT_TRUE(os_file_exists(path)) << path;
    os_delete_file(path);
}

TEST_F(ApiControlTest, puppetxrctl_places_and_moves_props)
{
    start();
    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    char output[4096];
    const char* add_qr[] = { "--target", target, "--json", "prop", "add", "qr", "https://example.com", "0", "1.5", "-1" };
    EXPECT_EQ(run_ctl(*this, add_qr, 10, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"prop\":1"), nullptr) << output;
    const char* add_keyboard[] = { "--target", target, "prop", "add", "keyboard", "0", "0.75", "-0.4", "0", "0.7071068", "0", "0.7071068" };
    EXPECT_EQ(run_ctl(*this, add_keyboard, 12, output, sizeof(output)), 0) << output;
    const char* move[] = { "--target", target, "prop", "move", "1", "0.5", "1.4", "-1.2" };
    EXPECT_EQ(run_ctl(*this, move, 8, output, sizeof(output)), 0) << output;
    const char* circle[] = { "--target", target, "prop", "motion", "2", "circle", "0.3", "0.2" };
    EXPECT_EQ(run_ctl(*this, circle, 8, output, sizeof(output)), 0) << output;
    const char* linear[] = { "--target", target, "prop", "motion", "1", "linear", "1", "1.4", "-1.2", "0.5" };
    EXPECT_EQ(run_ctl(*this, linear, 10, output, sizeof(output)), 0) << output;
    const char* size[] = { "--target", target, "prop", "size", "1", "0.2" };
    EXPECT_EQ(run_ctl(*this, size, 6, output, sizeof(output)), 0) << output;
    const char* text[] = { "--target", target, "prop", "text", "1", "second" };
    EXPECT_EQ(run_ctl(*this, text, 6, output, sizeof(output)), 0) << output;

    const SIM_Prop* props = nullptr;
    ASSERT_EQ(sim_props(oxr_get_sim(), props), 2u);
    EXPECT_FLOAT_EQ(props[0].pose.position.x, 0.5f);
    EXPECT_EQ(props[0].motion, SIM_PropMotion::Linear);
    EXPECT_FLOAT_EQ(props[0].target.x, 1.0f);
    EXPECT_FLOAT_EQ(props[0].speed, 0.5f);
    EXPECT_FLOAT_EQ(props[0].size, 0.2f);
    EXPECT_STREQ(props[0].text, "second");
    EXPECT_EQ(props[1].kind, SIM_PropKind::Keyboard);
    EXPECT_EQ(props[1].motion, SIM_PropMotion::Circle);
    EXPECT_FLOAT_EQ(props[1].radius, 0.3f);
    EXPECT_NEAR(props[1].pose.orientation.y, 0.7071068f, 1e-6f);

    const char* list[] = { "--target", target, "--json", "props" };
    EXPECT_EQ(run_ctl(*this, list, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"kind\":\"keyboard\""), nullptr) << output;
    EXPECT_NE(cstr_find(output, "\"motion\":\"circle\""), nullptr) << output;
    EXPECT_NE(cstr_find(output, "\"current_position\""), nullptr) << output;
    const char* remove[] = { "--target", target, "prop", "remove", "1" };
    EXPECT_EQ(run_ctl(*this, remove, 5, output, sizeof(output)), 0) << output;
    EXPECT_NE(run_ctl(*this, remove, 5, output, sizeof(output)), 0) << "already gone";
    EXPECT_EQ(sim_props(oxr_get_sim(), props), 1u);
}

TEST_F(ApiControlTest, puppetxrctl_drifts_the_tracking_origin)
{
    start();
    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    char output[4096];
    const char* step[] = { "--target", target, "drift", "0.1", "0", "-0.2", "90" };
    EXPECT_EQ(run_ctl(*this, step, 7, output, sizeof(output)), 0) << output;
    Pose drift = sim_get_tracking_drift(oxr_get_sim());
    EXPECT_FLOAT_EQ(drift.position.x, 0.1f);
    EXPECT_FLOAT_EQ(drift.position.z, -0.2f);
    EXPECT_NEAR(drift.orientation.y, 0.70710678f, 1e-6f);
    const char* rate[] = { "--target", target, "drift", "rate", "0.5", "0", "0", "2" };
    EXPECT_EQ(run_ctl(*this, rate, 8, output, sizeof(output)), 0) << output;
    Vec3 velocity = {};
    f32  yaw_rate = 0.0f;
    sim_get_tracking_drift_rate(oxr_get_sim(), velocity, yaw_rate);
    EXPECT_FLOAT_EQ(velocity.x, 0.5f);
    EXPECT_FLOAT_EQ(yaw_rate, 2.0f);

    const char* get[] = { "--target", target, "--json", "drift" };
    EXPECT_EQ(run_ctl(*this, get, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"yaw_rate\":2"), nullptr) << output;
    EXPECT_NE(cstr_find(output, "\"velocity\":[0.5,0,0]"), nullptr) << output;
    EXPECT_NE(cstr_find(output, "\"yaw_degrees\":"), nullptr) << output;

    const char* tilted[] = { "--target", target, "send", "{\"cmd\":\"set_tracking_drift\",\"orientation\":[0.1,0,0,0.995]}" };
    EXPECT_NE(run_ctl(*this, tilted, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "invalid_argument"), nullptr) << output;
    const char* reset[] = { "--target", target, "drift", "reset" };
    EXPECT_EQ(run_ctl(*this, reset, 4, output, sizeof(output)), 0) << output;
    drift = sim_get_tracking_drift(oxr_get_sim());
    EXPECT_TRUE(zm::equal(drift, zm::POSE_IDENTITY));
    sim_get_tracking_drift_rate(oxr_get_sim(), velocity, yaw_rate);
    EXPECT_TRUE(zm::equal(velocity, Vec3{}));
    EXPECT_EQ(yaw_rate, 0.0f);
    const char* bad[] = { "--target", target, "drift", "rate", "fast" };
    EXPECT_EQ(run_ctl(*this, bad, 5, output, sizeof(output)), 2) << output;
}

TEST_F(ApiControlTest, puppetxrctl_fails_frame_calls)
{
    start();
    XrSpace                          stage      = XR_NULL_HANDLE;
    const XrReferenceSpaceCreateInfo space_info = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE, .poseInReferenceSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    ASSERT_EQ(proc<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(session, &space_info, &stage), XR_SUCCESS);
    const PFN_xrLocateViews locate_views = proc<PFN_xrLocateViews>("xrLocateViews");
    XrFrameState            state        = { .type = XR_TYPE_FRAME_STATE };
    ASSERT_EQ(wait_frame(session, nullptr, &state), XR_SUCCESS);
    ASSERT_EQ(begin_frame(session, nullptr), XR_SUCCESS);
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = state.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    ASSERT_EQ(end_frame(session, &end_info), XR_SUCCESS);
    const XrViewLocateInfo locate_info = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = state.predictedDisplayTime, .space = stage };
    XrViewState            view_state  = { .type = XR_TYPE_VIEW_STATE };
    XrView                 views[2]    = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
    u32                    count       = 0;

    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    char        output[4096];
    const char* fault[] = { "--target", target, "frame", "fault", "locate", "-1", "2" };
    EXPECT_EQ(run_ctl(*this, fault, 7, output, sizeof(output)), 0) << output;
    EXPECT_EQ(locate_views(session, &locate_info, &view_state, 2, &count, views), XR_ERROR_VALIDATION_FAILURE);
    EXPECT_EQ(count, 0u);
    const char* clear[] = { "--target", target, "frame", "fault", "locate", "0", "0" };
    EXPECT_EQ(run_ctl(*this, clear, 7, output, sizeof(output)), 0) << output;
    EXPECT_EQ(locate_views(session, &locate_info, &view_state, 2, &count, views), XR_SUCCESS);
    EXPECT_EQ(count, 2u);
    const char* once[] = { "--target", target, "frame", "fault", "locate", "-2" };
    EXPECT_EQ(run_ctl(*this, once, 6, output, sizeof(output)), 0) << output;
    EXPECT_EQ(locate_views(session, &locate_info, &view_state, 2, &count, views), XR_ERROR_RUNTIME_FAILURE);
    EXPECT_EQ(locate_views(session, &locate_info, &view_state, 2, &count, views), XR_SUCCESS) << "count defaults to 1";

    const char* not_for_wait[] = { "--target", target, "frame", "fault", "wait", "-37" };
    EXPECT_NE(run_ctl(*this, not_for_wait, 6, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "invalid_argument"), nullptr) << output;
    const char* raw[] = { "--target", target, "send", "{\"cmd\":\"set_frame_fault\",\"op\":\"present\",\"result\":-1}" };
    EXPECT_NE(run_ctl(*this, raw, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "invalid_argument"), nullptr) << output;
    const char* bad[] = { "--target", target, "frame", "fault", "end", "lots" };
    EXPECT_EQ(run_ctl(*this, bad, 6, output, sizeof(output)), 2) << output;
}

// Unreal destroys its first instance and creates another of the same name, so its control server
// moves to another port; a shell connected to the first follows the process to the second
TEST_F(ApiControlTest, puppetxrctl_shell_follows_the_app_to_its_next_instance)
{
    start();
    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    const u16 first_port = discovered_port();
    char output_path[OS_MAX_PATH];
    char input_path[OS_MAX_PATH];
    char name[64];
    cstr_format(name, sizeof(name), "puppetxrctl_follow_out_%u.txt", os_process_id());
    api_path(output_path, sizeof(output_path), name);
    cstr_format(name, sizeof(name), "puppetxrctl_follow_in_%u.txt", os_process_id());
    api_path(input_path, sizeof(input_path), name);
    const char* input = "input left trigger/value 0.25\nwait 5\ninput left trigger/value 0.5\n";
    ASSERT_TRUE(os_write_file(input_path, input, cstr_length(input)));

    const char* argv[] = { PUPPETXR_CTL_PATH, "--target", target, "shell", nullptr };
    OS_Process* process = os_process_spawn(argv, input_path, output_path);
    ASSERT_NE(process, nullptr);
    i32 exit_code = -1;
    b8  exited    = false;
    for (u32 i = 0; i < 2000 && sim_get_input(oxr_get_sim(), SIM_Device::LeftHand, SIM_Input::TriggerValue).value.x != 0.25f; i++)
    {
        frame();
        exited = os_process_wait(process, 1, exit_code);
    }
    ASSERT_FALSE(exited);
    stop();
    start();
    EXPECT_NE(discovered_port(), first_port);
    for (u32 i = 0; i < 4000 && !exited; i++)
    {
        frame();
        exited = os_process_wait(process, 1, exit_code);
    }
    os_process_release(process);
    char output[4096] = {};
    u8*   data = nullptr;
    usize size = 0;
    if (os_read_file(output_path, &data, &size))
    {
        memory_copy(output, data, size < sizeof(output) - 1 ? size : sizeof(output) - 1);
        os_memory_release(data);
    }
    EXPECT_TRUE(exited);
    EXPECT_EQ(exit_code, 0) << output;
    EXPECT_FLOAT_EQ(sim_get_input(oxr_get_sim(), SIM_Device::LeftHand, SIM_Input::TriggerValue).value.x, 0.5f) << output;
}

// An app that crashed leaves no simulator to wait for, so the shell gives up at once
TEST_F(ApiControlTest, puppetxrctl_shell_gives_up_on_an_exited_process)
{
    char output_path[OS_MAX_PATH];
    char input_path[OS_MAX_PATH];
    char name[64];
    cstr_format(name, sizeof(name), "puppetxrctl_exited_out_%u.txt", os_process_id());
    api_path(output_path, sizeof(output_path), name);
    cstr_format(name, sizeof(name), "puppetxrctl_exited_in_%u.txt", os_process_id());
    api_path(input_path, sizeof(input_path), name);
    ASSERT_TRUE(os_write_file(input_path, "status\n", 7));
    const char* argv[] = { PUPPETXR_CTL_PATH, "--target", "4294967280", "--timeout", "300000", "shell", nullptr };
    OS_Process* process = os_process_spawn(argv, input_path, output_path);
    ASSERT_NE(process, nullptr);
    i32 exit_code = -1;
    EXPECT_TRUE(os_process_wait(process, 10000, exit_code));
    os_process_release(process);
    EXPECT_EQ(exit_code, 3);
}

// Between Unreal's two instances no simulator listens; a shell started then waits for the next
TEST_F(ApiControlTest, puppetxrctl_shell_waits_for_its_target_to_listen)
{
    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    char output_path[OS_MAX_PATH];
    char input_path[OS_MAX_PATH];
    char name[64];
    cstr_format(name, sizeof(name), "puppetxrctl_wait_out_%u.txt", os_process_id());
    api_path(output_path, sizeof(output_path), name);
    cstr_format(name, sizeof(name), "puppetxrctl_wait_in_%u.txt", os_process_id());
    api_path(input_path, sizeof(input_path), name);
    const char* input = "input left trigger/value 0.75\n";
    ASSERT_TRUE(os_write_file(input_path, input, cstr_length(input)));

    const char* argv[] = { PUPPETXR_CTL_PATH, "--target", target, "shell", nullptr };
    OS_Process* process = os_process_spawn(argv, input_path, output_path);
    ASSERT_NE(process, nullptr);
    i32 exit_code = -1;
    b8  exited    = os_process_wait(process, 500, exit_code);
    EXPECT_FALSE(exited) << "it waits";
    start();
    for (u32 i = 0; i < 4000 && !exited; i++)
    {
        frame();
        exited = os_process_wait(process, 1, exit_code);
    }
    os_process_release(process);
    EXPECT_TRUE(exited);
    EXPECT_EQ(exit_code, 0);
    EXPECT_FLOAT_EQ(sim_get_input(oxr_get_sim(), SIM_Device::LeftHand, SIM_Input::TriggerValue).value.x, 0.75f);
}

// `puppetxrctl profiler start <path>` and `profiler stop`; `profile` names a control profile
TEST_F(ApiControlTest, puppetxrctl_profiles_a_running_session)
{
    start();
    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    char output[4096];
    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxrctl_profile.json");
    const char* start_args[] = { "--target", target, "profiler", "start", path };
    EXPECT_EQ(run_ctl(*this, start_args, 5, output, sizeof(output)), 0) << output;
    EXPECT_TRUE(os_profile_active());
    for (u32 i = 0; i < 5; i++)
    {
        frame();
    }
    const char* stop_args[] = { "--target", target, "profiler", "stop" };
    EXPECT_EQ(run_ctl(*this, stop_args, 4, output, sizeof(output)), 0) << output;
    EXPECT_FALSE(os_profile_active());
    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(path, &data, &size));
    EXPECT_TRUE(file_has(data, size, "\"name\":\"oxr_xrWaitFrame\""));
    EXPECT_TRUE(file_has(data, size, "\"name\":\"sim step\""));
    os_memory_release(data);
    os_delete_file(path);
}

TEST_F(ApiControlTest, puppetxrctl_drives_the_simulator)
{
    start();
    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    char output[4096];

    const char* list[] = { "list" };
    EXPECT_EQ(run_ctl(*this, list, 1, output, sizeof(output)), 0);
    EXPECT_NE(cstr_find(output, app_name), nullptr) << output;

    const char* set[] = { "--target", target, "input", "left", "trigger/value", "0.25" };
    EXPECT_EQ(run_ctl(*this, set, 6, output, sizeof(output)), 0) << output;
    EXPECT_FLOAT_EQ(sim_get_input(oxr_get_sim(), SIM_Device::LeftHand, SIM_Input::TriggerValue).value.x, 0.25f);

    const char* get[] = { "--target", app_name, "--json", "input", "left", "trigger/value" };
    EXPECT_EQ(run_ctl(*this, get, 6, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"value\":[0.25,0]"), nullptr) << output;

    const char* pose[] = { "--target", target, "pose", "head", "0", "1.5", "0.5" };
    EXPECT_EQ(run_ctl(*this, pose, 7, output, sizeof(output)), 0) << output;
    EXPECT_FLOAT_EQ(sim_get_device_pose(oxr_get_sim(), SIM_Device::Head).position.z, 0.5f);

    const char* refused[] = { "--target", target, "input", "nose", "a/click", "on" };
    EXPECT_EQ(run_ctl(*this, refused, 6, output, sizeof(output)), 1) << output;
    EXPECT_NE(cstr_find(output, "invalid_argument"), nullptr) << output;

    const char* mode[] = { "--target", target, "mode", "right" };
    EXPECT_EQ(run_ctl(*this, mode, 4, output, sizeof(output)), 0) << output;
    EXPECT_STREQ(sim_get_config(oxr_get_sim()).profiles[sim_get_profile(oxr_get_sim())].name, "right");
    const char* get_mode[] = { "--target", target, "--json", "mode" };
    EXPECT_EQ(run_ctl(*this, get_mode, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"mode\":\"right\""), nullptr) << output;
    const char* status[] = { "--target", target, "--json", "status" };
    EXPECT_EQ(run_ctl(*this, status, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"control_mode\":\"right\""), nullptr) << output;
    const char* profile[] = { "--target", target, "profile", "stick" };
    EXPECT_EQ(run_ctl(*this, profile, 4, output, sizeof(output)), 0) << output;
    const char* get_profile[] = { "--target", target, "--json", "profile" };
    EXPECT_EQ(run_ctl(*this, get_profile, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"profile\":\"stick\""), nullptr) << output;
    EXPECT_EQ(run_ctl(*this, mode, 4, output, sizeof(output)), 0) << output;

    const char* hand[] = { "--target", target, "hand", "left", "hand" };
    EXPECT_EQ(run_ctl(*this, hand, 5, output, sizeof(output)), 0) << output;
    EXPECT_EQ(sim_get_hand_source(oxr_get_sim(), SIM_Device::LeftHand), SIM_HandSource::Hand);
    const char* get_hand[] = { "--target", target, "--json", "hand", "left" };
    EXPECT_EQ(run_ctl(*this, get_hand, 5, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"source\":\"hand\""), nullptr) << output;

    const char* key_down[] = { "--target", target, "key", "w", "down" };
    EXPECT_EQ(run_ctl(*this, key_down, 5, output, sizeof(output)), 0) << output;
    frame();
    const char* key_up[] = { "--target", target, "key", "w", "up" };
    EXPECT_EQ(run_ctl(*this, key_up, 5, output, sizeof(output)), 0) << output;
    const Pose start_right = sim_get_device_pose(oxr_get_sim(), SIM_Device::RightHand);
    const char* reset[] = { "--target", target, "reset" };
    EXPECT_EQ(run_ctl(*this, reset, 3, output, sizeof(output)), 0) << output;
    EXPECT_LT(sim_get_device_pose(oxr_get_sim(), SIM_Device::RightHand).position.y, start_right.position.y);
    EXPECT_FLOAT_EQ(sim_get_device_pose(oxr_get_sim(), SIM_Device::Head).position.z, 0.0f);
    const char* reset_hands[] = { "--target", target, "reset", "hands" };
    EXPECT_EQ(run_ctl(*this, reset_hands, 4, output, sizeof(output)), 0) << output;
    const char* point[] = { "--target", target, "point", "right", "0.5", "0.5" };
    EXPECT_EQ(run_ctl(*this, point, 6, output, sizeof(output)), 0) << output;
    Pose eye = {};
    Fov  fov = {};
    sim_get_view(oxr_get_sim(), 0, eye, fov);
    EXPECT_NEAR(zm::length(sim_get_hand_pose(oxr_get_sim(), SIM_Device::RightHand, SIM_HandPose::Aim).position - eye.position), 0.0f, 1e-5f);
    const char* point_right[] = { "--target", target, "point", "left", "0.1", "0.9", "right" };
    EXPECT_EQ(run_ctl(*this, point_right, 7, output, sizeof(output)), 0) << output;
    sim_get_view(oxr_get_sim(), 1, eye, fov);
    EXPECT_NEAR(zm::length(sim_get_hand_pose(oxr_get_sim(), SIM_Device::LeftHand, SIM_HandPose::Aim).position - eye.position), 0.0f, 1e-5f);
    const char* point_grip[] = { "--target", target, "point", "left", "0.5", "0.5", "grip", "-60" };
    EXPECT_EQ(run_ctl(*this, point_grip, 8, output, sizeof(output)), 0) << output;
    sim_get_view(oxr_get_sim(), 0, eye, fov);
    EXPECT_NEAR(zm::length(sim_get_device_pose(oxr_get_sim(), SIM_Device::LeftHand).position - eye.position), 0.0f, 1e-5f);
    const char* point_ahead[] = { "--target", target, "point", "right", "0.25", "0.75", "ahead", "0.5" };
    EXPECT_EQ(run_ctl(*this, point_ahead, 8, output, sizeof(output)), 0) << output;
    const Pose aim = sim_get_hand_pose(oxr_get_sim(), SIM_Device::RightHand, SIM_HandPose::Aim);
    EXPECT_NEAR(zm::length(aim.position - zm::rotate(aim.orientation, { 0.0f, 0.0f, -0.5f }) - eye.position), 0.0f, 1e-5f);
    const char* point_grip_ahead[] = { "--target", target, "point", "left", "0.5", "0.5", "right", "grip", "-60", "ahead", "0.4" };
    EXPECT_EQ(run_ctl(*this, point_grip_ahead, 11, output, sizeof(output)), 0) << output;
    sim_get_view(oxr_get_sim(), 1, eye, fov);
    const Pose grip = sim_get_device_pose(oxr_get_sim(), SIM_Device::LeftHand);
    const Quat ray  = grip.orientation * zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -60.0f * zm::DEG_TO_RAD);
    EXPECT_NEAR(zm::length(grip.position - zm::rotate(ray, { 0.0f, 0.0f, -0.4f }) - eye.position), 0.0f, 1e-5f);
    const char* bad_ahead[] = { "--target", target, "point", "left", "0.5", "0.5", "ahead", "-1" };
    EXPECT_EQ(run_ctl(*this, bad_ahead, 8, output, sizeof(output)), 1) << output;
    const char* bad_point[]= { "--target", target, "point", "head", "0.5", "0.5" };
    EXPECT_EQ(run_ctl(*this, bad_point, 6, output, sizeof(output)), 1) << output;
    const char* mouse[] = { "--target", target, "mouse", "10", "-5" };
    EXPECT_EQ(run_ctl(*this, mouse, 5, output, sizeof(output)), 0) << output;
    const char* bad_key[] = { "--target", target, "key", "nope", "down" };
    EXPECT_EQ(run_ctl(*this, bad_key, 5, output, sizeof(output)), 1) << output;
    const char* bad_mode[] = { "--target", target, "mode", "feet" };
    EXPECT_EQ(run_ctl(*this, bad_mode, 4, output, sizeof(output)), 1) << output;
    const char* bad_press[] = { "--target", target, "key", "e", "sideways" };
    EXPECT_EQ(run_ctl(*this, bad_press, 5, output, sizeof(output)), 2) << output;

    const char* anchors[] = { "--target", target, "--json", "anchors" };
    EXPECT_EQ(run_ctl(*this, anchors, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"count\":0"), nullptr) << output;
    const char* wipe[] = { "--target", target, "anchors", "wipe", "cloud" };
    EXPECT_EQ(run_ctl(*this, wipe, 5, output, sizeof(output)), 0) << output;
    const char* fault[] = { "--target", target, "anchors", "fault", "load", "-1000169001", "2" };
    EXPECT_EQ(run_ctl(*this, fault, 7, output, sizeof(output)), 0) << output;
    const char* bad_fault[] = { "--target", target, "anchors", "fault", "sing", "-1", "1" };
    EXPECT_EQ(run_ctl(*this, bad_fault, 7, output, sizeof(output)), 1) << output;
    const char* timing[] = { "--target", target, "anchors", "timing", "0", "0" };
    EXPECT_EQ(run_ctl(*this, timing, 6, output, sizeof(output)), 0) << output;
    const char* origin[] = { "--target", target, "--json", "origin" };
    EXPECT_EQ(run_ctl(*this, origin, 4, output, sizeof(output)), 0) << output;
    EXPECT_NE(cstr_find(output, "\"position\":[0,0,0]"), nullptr) << output;

    const char* usage[] = { "--target", target, "fly" };
    EXPECT_EQ(run_ctl(*this, usage, 3, output, sizeof(output)), 2);

    const char* missing[] = { "--target", "no_such_app", "status" };
    EXPECT_EQ(run_ctl(*this, missing, 3, output, sizeof(output)), 3);
    EXPECT_NE(cstr_find(output, "no simulator matches"), nullptr) << output;
}

TEST_F(ApiTest, extensions_can_be_disabled_before_the_instance)
{
    PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
    gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumerate);
    u32 all = 0;
    ASSERT_EQ(enumerate(nullptr, 0, &all, nullptr), XR_SUCCESS);
    ASSERT_EQ(api.set_disabled_extensions("XR_FB_passthrough,XR_EXT_local_floor"), PUPPETXR_SUCCESS);
    u32 fewer = 0;
    ASSERT_EQ(enumerate(nullptr, 0, &fewer, nullptr), XR_SUCCESS);
    EXPECT_EQ(fewer, all - 2);
    ASSERT_EQ(api.set_disabled_extensions(nullptr), PUPPETXR_SUCCESS);
    ASSERT_EQ(enumerate(nullptr, 0, &fewer, nullptr), XR_SUCCESS);
    EXPECT_EQ(fewer, all);
}

namespace
{

struct ApiAnchorTest : ApiTest
{
    char anchors_dir[OS_MAX_PATH] = {};

    void SetUp () override
    {
        ApiTest::SetUp();
        ASSERT_TRUE(os_create_temp_directory("puppetxr_api_anchors", anchors_dir, sizeof(anchors_dir)));
        os_set_env("PUPPETXR_ANCHORS_DIR", anchors_dir);
        os_set_env("PUPPETXR_ORIGIN_OFFSET", "90,0.5,0");
    }

    void TearDown () override
    {
        ApiTest::TearDown();
        os_unset_env("PUPPETXR_ANCHORS_DIR");
        os_delete_tree(anchors_dir);
        os_unset_env("PUPPETXR_ORIGIN_OFFSET");
    }

    // What an earlier run of the app saved, as the store file holds it
    void store_one_anchor ()
    {
        cstr_format(app_name, sizeof(app_name), "api_test_%u", os_process_id());
        W_Store        store  = {};
        W_StoredAnchor anchor = { .room_hash = 0, .pose = { { 1, 2, 3 }, zm::QUAT_IDENTITY } };
        anchor.uuid.bytes[15] = 7;
        w_store_put(store, anchor);
        char path[OS_MAX_PATH];
        cstr_format(path, sizeof(path), "%s/%s/user_1.json", anchors_dir, app_name);
        ASSERT_TRUE(w_store_save(path, store));
        w_store_release(store);
    }
};

} // namespace

TEST_F(ApiAnchorTest, anchors_are_listed_and_wiped)
{
    store_one_anchor();
    start();
    PUPPETXR_U32 count      = 0;
    PUPPETXR_Anchor anchors[2] = {};
    ASSERT_EQ(api.get_anchors(anchors, 2, &count), PUPPETXR_SUCCESS);
    ASSERT_EQ(count, 1u);
    EXPECT_STREQ(anchors[0].uuid, "0000000000000000000000000000000" "7");
    EXPECT_STREQ(anchors[0].room, "");
    EXPECT_FLOAT_EQ(anchors[0].pose.position[2], 3.0f);
    EXPECT_EQ(anchors[0].cloud, 0);
    EXPECT_EQ(api.get_anchors(nullptr, 0, &count), PUPPETXR_SUCCESS);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(api.get_anchors(anchors, 2, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);

    char recording[OS_MAX_PATH];
    api_path(recording, sizeof(recording), "puppetxr_api_anchor_busy.xrrec");
    ASSERT_EQ(api.record_start(recording), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.wipe_anchors(0), PUPPETXR_ERROR_BUSY);
    EXPECT_EQ(api.set_anchor_timing(1, 1), PUPPETXR_ERROR_BUSY);
    ASSERT_EQ(api.record_stop(), PUPPETXR_SUCCESS);
    os_delete_file(recording);
    ASSERT_EQ(api.wipe_anchors(0), PUPPETXR_SUCCESS);
    ASSERT_EQ(api.get_anchors(anchors, 2, &count), PUPPETXR_SUCCESS);
    EXPECT_EQ(count, 0u);
}

TEST_F(ApiAnchorTest, origin_offset_faults_and_timing)
{
    start();
    PUPPETXR_Pose origin = {};
    ASSERT_EQ(api.get_origin_offset(&origin), PUPPETXR_SUCCESS);
    EXPECT_FLOAT_EQ(origin.position[0], 0.5f);
    EXPECT_NEAR(origin.orientation[1], 0.70710678f, 1e-6f);
    EXPECT_EQ(api.get_origin_offset(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);

    EXPECT_EQ(api.set_anchor_fault(PUPPETXR_ANCHOR_OP_SAVE, -1000169002, 2), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_anchor_fault(PUPPETXR_ANCHOR_OP_SAVE, 0, 0), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_anchor_fault(PUPPETXR_ANCHOR_OP_SHARE, 1, 1), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_anchor_fault((PUPPETXR_AnchorOp)9, -1, 1), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_anchor_timing(2, 30), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_anchor_timing(0, 0), PUPPETXR_SUCCESS);
}

namespace
{
struct ApiControlPausedTest : ApiControlTest
{
    void SetUp () override
    {
        os_set_env("PUPPETXR_CONTROL_PAUSED", "1");
        ApiControlTest::SetUp();
    }

    void TearDown () override
    {
        ApiControlTest::TearDown();
        os_unset_env("PUPPETXR_CONTROL_PAUSED");
    }
};
} // namespace

// A paused simulator holds the app in xrWaitFrame; step runs exactly that many frames
TEST_F(ApiControlPausedTest, only_stepped_frames_run)
{
    start();
    const u64 first = sim_frame_index(oxr_get_sim());
    char      target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    const char* args[] = { "--target", target, "shell" };
    char        output[4096];
    EXPECT_EQ(run_ctl(*this, args, 3, output, sizeof(output), "status\nstep 5\nstatus\nresume\n"), 0) << output;

    char expected[64];
    const u64 stepped = first + 5;
    cstr_format(expected, sizeof(expected), "\"frame\":%llu,", (unsigned long long)stepped);
    const char* second_status = cstr_find(cstr_find(output, "\"paused\":true"), "\n");
    ASSERT_NE(second_status, nullptr) << output;
    EXPECT_NE(cstr_find(second_status, expected), nullptr) << output;
    EXPECT_NE(cstr_find(output, "\"paused\":true"), nullptr) << output;

    const u64 resumed = sim_frame_index(oxr_get_sim());
    frame();
    frame();
    EXPECT_EQ(sim_frame_index(oxr_get_sim()), resumed + 2);
}

// An agent stepping slowly holds the app in xrWaitFrame for as long as it likes: that time is
// spent in the control server's pause, which the frame gate's timeout does not count
static u32 count_gate_warnings ()
{
    u32 count = 0;
    for (u32 i = 0; i < os_log_history_count(); i++)
    {
        count += cstr_find(os_log_history(i)->text, "for xrBeginFrame of the previous frame") != nullptr;
    }
    return count;
}

namespace
{
struct SlowAgent
{
    u16 port;
};
} // namespace

static i32 slow_agent (void* user) // NOLINT(misc-const-correctness): OS_ThreadFunction
{
    const SlowAgent* agent  = (const SlowAgent*)user;
    OS_Socket*       socket = os_socket_connect("127.0.0.1", agent->port, 1000);
    if (!socket)
    {
        return 1;
    }
    char hello[128];
    cstr_format(hello, sizeof(hello), "{\"cmd\":\"hello\",\"version\":\"%u.%u.%u\"}\n", PUPPETXR_BUILD_VERSION_MAJOR, PUPPETXR_BUILD_VERSION_MINOR, PUPPETXR_BUILD_VERSION_PATCH);
    const char* const lines[] = { hello, "{\"cmd\":\"step\",\"frames\":1}\n", "{\"cmd\":\"resume\"}\n" };
    for (const char* line : lines)
    {
        os_sleep_ns(1500000000ull);
        usize sent = 0;
        os_socket_send(socket, line, cstr_length(line), sent);
    }
    os_sleep_ns(200000000ull);
    os_socket_close(socket);
    return 0;
}

TEST_F(ApiControlPausedTest, slow_steps_do_not_time_out_the_frame_gate)
{
    start();
    const u32  warnings = count_gate_warnings();
    SlowAgent  agent    = { .port = discovered_port() };
    OS_Thread* thread   = os_thread_create("slow_agent", slow_agent, &agent);
    ASSERT_NE(thread, nullptr);
    const u64 first = sim_frame_index(oxr_get_sim());
    const u64 start_ns = os_monotonic_ns();
    frame(); // held until the step
    frame(); // held until the resume
    frame();
    EXPECT_EQ(os_thread_join(thread), 0);
    EXPECT_GE(os_monotonic_ns() - start_ns, 3000000000ull) << "the frames were held by the pause";
    EXPECT_EQ(sim_frame_index(oxr_get_sim()), first + 3);
    EXPECT_EQ(count_gate_warnings(), warnings);
}

// Unreal waits for the next frame on its game thread while its render thread still ends the last
// one. A step answered once the frame was waited, with the runtime locked while paused, let a
// screenshot taken after it show the frame before, in some runs and not in others
namespace
{
struct PipelinedApp
{
    const ApiControlPausedTest* test;
    OS_Semaphore*               begun; // a frame was waited, for the render thread to begin and end
    OS_Semaphore*               done;
    XrTime                      display_time;
    u32                         ended; // under the runtime's lock
    u32                         frames;
};

struct SteppingAgent
{
    u16                 port;
    const PipelinedApp* app;
    u32                 steps;
    u32                 ended_at_reply[4];
    u64                 frame_at_reply[4];
};
} // namespace

static i32 pipelined_render_thread (void* user) // NOLINT(misc-const-correctness): OS_ThreadFunction
{
    PipelinedApp& app = *(PipelinedApp*)user;
    for (u32 i = 0; i < app.frames; i++)
    {
        os_semaphore_wait(app.begun);
        if (app.test->begin_frame(app.test->session, nullptr) != XR_SUCCESS)
        {
            return 1;
        }
        os_sleep_ns(100000000ull);
        const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = app.display_time, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        if (app.test->end_frame(app.test->session, &end_info) != XR_SUCCESS)
        {
            return 1;
        }
        oxr_lock();
        app.ended++;
        oxr_unlock();
    }
    os_semaphore_signal(app.done);
    return 0;
}

static b8 agent_line (OS_Socket* socket, const char* line, char* reply, usize size)
{
    usize sent = 0;
    if (os_socket_send(socket, line, cstr_length(line), sent) != OS_SocketResult::Ok)
    {
        return false;
    }
    usize length = 0;
    for (u32 i = 0; i < 1000; i++)
    {
        usize received = 0;
        while (os_socket_receive(socket, reply + length, size - 1 - length, received) == OS_SocketResult::Ok && received > 0)
        {
            length += received;
        }
        if (length > 0 && reply[length - 1] == '\n')
        {
            reply[length] = 0;
            return true;
        }
        os_sleep_ns(10000000ull);
    }
    return false;
}

static i32 stepping_agent (void* user) // NOLINT(misc-const-correctness): OS_ThreadFunction
{
    SteppingAgent& agent  = *(SteppingAgent*)user;
    OS_Socket*     socket = os_socket_connect("127.0.0.1", agent.port, 1000);
    if (!socket)
    {
        return 1;
    }
    defer { os_socket_close(socket); };
    char reply[1024];
    char hello[128];
    cstr_format(hello, sizeof(hello), "{\"cmd\":\"hello\",\"version\":\"%u.%u.%u\"}\n", PUPPETXR_BUILD_VERSION_MAJOR, PUPPETXR_BUILD_VERSION_MINOR, PUPPETXR_BUILD_VERSION_PATCH);
    if (!agent_line(socket, hello, reply, sizeof(reply)))
    {
        return 2;
    }
    for (u32 i = 0; i < agent.steps; i++)
    {
        if (!agent_line(socket, "{\"cmd\":\"step\",\"frames\":1}\n", reply, sizeof(reply)))
        {
            return 3;
        }
        oxr_lock();
        agent.ended_at_reply[i] = agent.app->ended;
        agent.frame_at_reply[i] = sim_frame_index(oxr_get_sim());
        oxr_unlock();
    }
    return agent_line(socket, "{\"cmd\":\"resume\"}\n", reply, sizeof(reply)) ? 0 : 4;
}

TEST_F(ApiControlPausedTest, a_step_answers_once_its_frame_ended_on_another_thread)
{
    start();
    // The frames start() ran ended on this thread
    PipelinedApp app = { .test = this, .begun = os_semaphore_create(0), .done = os_semaphore_create(0), .display_time = 0, .ended = 0, .frames = 4 };
    defer {
        os_semaphore_destroy(app.begun);
        os_semaphore_destroy(app.done);
    };
    const u64     first  = sim_frame_index(oxr_get_sim());
    SteppingAgent agent  = { .port = discovered_port(), .app = &app, .steps = 3, .ended_at_reply = {}, .frame_at_reply = {} };
    OS_Thread*    render = os_thread_create("render", pipelined_render_thread, &app);
    OS_Thread*    driver = os_thread_create("agent", stepping_agent, &agent);
    ASSERT_NE(render, nullptr);
    ASSERT_NE(driver, nullptr);
    for (u32 i = 0; i < app.frames; i++)
    {
        XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
        ASSERT_EQ(wait_frame(session, nullptr, &state), XR_SUCCESS);
        app.display_time = state.predictedDisplayTime;
        os_semaphore_signal(app.begun);
    }
    os_semaphore_wait(app.done);
    EXPECT_EQ(os_thread_join(render), 0);
    EXPECT_EQ(os_thread_join(driver), 0);
    for (u32 i = 0; i < agent.steps; i++)
    {
        EXPECT_EQ(agent.frame_at_reply[i], first + i + 1) << "step " << i;
        EXPECT_EQ(agent.ended_at_reply[i], i + 1) << "step " << i << ": its frame was not ended yet";
    }
}

// A client that disconnects while paused would otherwise leave the app hanging
TEST_F(ApiControlPausedTest, the_last_client_leaving_resumes)
{
    start();
    char target[16];
    cstr_format(target, sizeof(target), "%u", os_process_id());
    const char* args[] = { "--target", target, "status" };
    char        output[4096];
    EXPECT_EQ(run_ctl(*this, args, 3, output, sizeof(output)), 0) << output;
    const u64 before = sim_frame_index(oxr_get_sim());
    frame();
    EXPECT_EQ(sim_frame_index(oxr_get_sim()), before + 1);
}


TEST_F(ApiTest, body_tracking)
{
    PUPPETXR_U32 count = 0;
    EXPECT_EQ(api.get_body_joints(nullptr, 0, &count), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_body("{}"), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.reset_body(), PUPPETXR_ERROR_NO_INSTANCE);
    start();

    EXPECT_EQ(api.get_body_joints(nullptr, 0, &count), PUPPETXR_SUCCESS);
    EXPECT_EQ(count, 84u) << "XR_META_body_tracking_full_body's joints";
    PUPPETXR_BodyJoint joints[84] = {};
    EXPECT_EQ(api.get_body_joints(joints, 84, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_body_joints(nullptr, 84, &count), PUPPETXR_ERROR_INVALID_ARGUMENT);
    ASSERT_EQ(api.get_body_joints(joints, 84, &count), PUPPETXR_SUCCESS);
    SIM_Sim*   sim    = oxr_get_sim();
    SIM_Sample sample = {};
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    SIM_BodyJoints expected = {};
    sim_body_joints(sim, sample, expected);
    const Vec3 head = expected.poses[(u32)SIM_BodyJoint::Head].position;
    EXPECT_EQ(joints[(u32)SIM_BodyJoint::Head].pose.position[1], head.y);
    EXPECT_EQ(joints[(u32)SIM_BodyJoint::Head].tracked, 1);
    EXPECT_FLOAT_EQ(joints[(u32)SIM_BodyJoint::Head].confidence, 1.0f);

    EXPECT_EQ(api.set_body(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_body("{\"bones\":{\"tail\":1}}"), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_body("not json"), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_body("{\"bones\":{\"left_arm_upper\":1.5},\"joints\":{\"hips\":{\"confidence\":0.5,\"tracked\":false}}}"), PUPPETXR_SUCCESS);
    EXPECT_EQ(sim_get_body(sim).bone_scales[(u32)SIM_BodyJoint::LeftArmUpper], 1.5f);
    ASSERT_EQ(api.get_body_joints(joints, 84, &count), PUPPETXR_SUCCESS);
    EXPECT_EQ(joints[(u32)SIM_BodyJoint::Hips].tracked, 0);
    EXPECT_FLOAT_EQ(joints[(u32)SIM_BodyJoint::Hips].confidence, 0.5f);

    char      json[16384] = {};
    PUPPETXR_U32 length   = 0;
    ASSERT_EQ(api.get_body(json, sizeof(json), &length), PUPPETXR_SUCCESS);
    EXPECT_EQ(length, (PUPPETXR_U32)cstr_length(json));
    EXPECT_NE(cstr_find(json, "\"left_arm_upper\":1.5"), nullptr) << json;
    char small[8] = {};
    EXPECT_EQ(api.get_body(small, sizeof(small), &length), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_GT(length, (PUPPETXR_U32)sizeof(small)) << "the size it needs";

    EXPECT_EQ(api.reset_body(), PUPPETXR_SUCCESS);
    EXPECT_EQ(sim_get_body(sim).bone_scales[(u32)SIM_BodyJoint::LeftArmUpper], 1.0f);
    EXPECT_EQ(api.set_body(json), PUPPETXR_SUCCESS) << "what get_body saved loads back";
    EXPECT_EQ(sim_get_body(sim).bone_scales[(u32)SIM_BodyJoint::LeftArmUpper], 1.5f);

    // The body is recorded, so it may change while recording
    char path[OS_MAX_PATH];
    api_path(path, sizeof(path), "puppetxr_api_body.xrrec");
    ASSERT_EQ(api.record_start(path), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_body("{\"pose\":\"t_pose\"}"), PUPPETXR_SUCCESS);
    ASSERT_EQ(api.record_stop(), PUPPETXR_SUCCESS);
}

TEST_F(ApiTest, face_tracking)
{
    PUPPETXR_U32 count = 0;
    EXPECT_EQ(api.get_face_weights(nullptr, 0, &count), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.set_face("{}"), PUPPETXR_ERROR_NO_INSTANCE);
    EXPECT_EQ(api.reset_face(), PUPPETXR_ERROR_NO_INSTANCE);
    start();

    EXPECT_EQ(api.set_face("{\"preset\":\"smile\",\"blink\":false,\"gaze\":[10,0]}"), PUPPETXR_SUCCESS);
    EXPECT_EQ(api.set_face("{\"weights\":{\"ear_wiggle\":1}}"), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.set_face(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(api.get_face_weights(nullptr, 0, &count), PUPPETXR_SUCCESS);
    EXPECT_EQ(count, 70u);
    float weights[70] = {};
    EXPECT_EQ(api.get_face_weights(weights, 70, nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    ASSERT_EQ(api.get_face_weights(weights, 70, &count), PUPPETXR_SUCCESS);
    EXPECT_GT(weights[(u32)SIM_FaceExpression::LipCornerPullerL], 0.5f);
    EXPECT_GT(weights[(u32)SIM_FaceExpression::EyesLookLeftL], 0.2f) << "the eyes follow the gaze";

    PUPPETXR_Pose eyes[2] = {};
    EXPECT_EQ(api.get_eye_gazes(nullptr), PUPPETXR_ERROR_INVALID_ARGUMENT);
    ASSERT_EQ(api.get_eye_gazes(eyes), PUPPETXR_SUCCESS);
    EXPECT_LT(eyes[0].position[0], eyes[1].position[0]) << "left, then right";

    char      json[8192] = {};
    PUPPETXR_U32 length  = 0;
    ASSERT_EQ(api.get_face(json, sizeof(json), &length), PUPPETXR_SUCCESS);
    EXPECT_EQ(length, (PUPPETXR_U32)cstr_length(json));
    EXPECT_NE(cstr_find(json, "\"lip_corner_puller_l\":0.800000012"), nullptr) << json;
    char small[8] = {};
    EXPECT_EQ(api.get_face(small, sizeof(small), &length), PUPPETXR_ERROR_INVALID_ARGUMENT);
    EXPECT_GT(length, (PUPPETXR_U32)sizeof(small));

    EXPECT_EQ(api.reset_face(), PUPPETXR_SUCCESS);
    EXPECT_TRUE(sim_get_face(oxr_get_sim()).blink);
    EXPECT_EQ(api.set_face(json), PUPPETXR_SUCCESS) << "what get_face saved loads back";
    EXPECT_FALSE(sim_get_face(oxr_get_sim()).blink);
}
