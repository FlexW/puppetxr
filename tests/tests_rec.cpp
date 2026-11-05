#include "rec.h"

#include <gtest/gtest.h>

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

static void temp_path (char* out, usize size, const char* name)
{
    cstr_format(out, size, "%s/%s", temp_dir(), name);
}

static SIM_FrameState make_state (u32 i)
{
    SIM_FrameState state = {};
    state.dt             = 11111111 + (i % 3);
    state.local_origin   = { { 0.1f * (f32)i, 1.6f, 0.0f }, zm::quat_from_euler({ 0.0f, 0.01f * (f32)i, 0.0f }) };
    for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
    {
        state.devices[d].pose   = { { (f32)d, 1.0f + 0.01f * (f32)i, -0.5f }, zm::quat_from_euler({ 0.02f * (f32)i, 0.0f, 0.1f * (f32)d }) };
        state.devices[d].active = (i + d) % 5 != 0;
    }
    state.devices[(u32)SIM_Device::LeftHand].inputs[(u32)SIM_Input::TriggerValue].value  = { (f32)(i % 10) * 0.1f, 0.0f };
    state.devices[(u32)SIM_Device::RightHand].inputs[(u32)SIM_Input::Thumbstick].value   = { -0.5f, (f32)(i % 4) * 0.25f };
    state.devices[(u32)SIM_Device::RightHand].inputs[(u32)SIM_Input::AClick].value       = { (f32)(i % 2), 0.0f };
    state.devices[(u32)SIM_Device::RightHand].source = i % 7 == 3 ? SIM_HandSource::Hand : SIM_HandSource::Controller;
    if (state.devices[(u32)SIM_Device::RightHand].source == SIM_HandSource::Hand)
    {
        state.devices[(u32)SIM_Device::RightHand].detached = { { 0.3f, 0.7f + 0.01f * (f32)i, -0.4f }, zm::quat_from_euler({ 0.0f, 0.3f, 0.0f }) };
    }
    // One QR code first, a moving keyboard joins at frame 3
    state.props_present = true;
    state.props_count   = i < 3 ? 1 : 2;
    state.props[0]      = { .id = 1, .kind = SIM_PropKind::QrCode, .pose = { { 0.0f, 1.5f, -1.0f }, zm::QUAT_IDENTITY }, .size = 0.15f, .start = 1000 };
    cstr_copy(state.props[0].text, sizeof(state.props[0].text), "https://example.com/\xc3\xa9");
    state.props[1] = { .id = 4, .kind = SIM_PropKind::Keyboard, .motion = SIM_PropMotion::Circle, .pose = { { 0.0f, 0.8f, -0.5f }, zm::quat_from_euler({ 0.0f, 0.5f, 0.0f }) }, .target = { 0.0f, 0.8f, -0.5f }, .radius = 0.2f, .speed = 0.1f, .start = 5000 };
    // The default body, then from frame 3 a longer left arm, a posed elbow and a dropped knee
    state.body_present = true;
    sim_default_body(1.75f, state.body);
    if (i >= 3)
    {
        state.body.height                                         = 1.6f;
        state.body.bone_scales[(u32)SIM_BodyJoint::LeftArmUpper]  = 1.25f;
        state.body.rotations[(u32)SIM_BodyJoint::LeftArmLower]    = zm::quat_from_euler({ 0.0f, 0.4f, 0.0f });
        state.body.tracked[(u32)SIM_BodyJoint::LeftLowerLeg]      = false;
        state.body.confidences[(u32)SIM_BodyJoint::Hips]          = 0.5f;
        state.body.limbs[(u32)SIM_Limb::LeftArm]                  = SIM_LimbMode::Posed;
    }
    // A neutral face, then from frame 4 a smile looking at a point with a viseme and no blinks
    state.face_present = true;
    sim_default_face(state.face);
    if (i >= 4)
    {
        state.face.weights[(u32)SIM_FaceExpression::LipCornerPullerL] = 0.8f;
        state.face.visemes[3]                                         = 0.5f;
        state.face.confidences[0]                                     = 0.25f;
        state.face.blink                                              = false;
        state.face.gaze_yaw                                           = -12.5f;
        state.face.look_at                                            = true;
        state.face.target                                             = { 0.1f, 1.5f, -2.0f };
    }
    // No tracking drift, a step at frame 2, another from frame 4 on
    state.drift_present  = true;
    state.tracking_drift = zm::POSE_IDENTITY;
    if (i >= 2)
    {
        state.tracking_drift = { { 0.05f, 0.0f, -0.02f }, zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, i >= 4 ? -0.1f : 0.03f) };
    }
    if (i % 5 == 2)
    {
        const char text[] = "h\xc3\xa9!\b\n";
        memory_copy(state.typed, text, sizeof(text) - 1);
        state.typed_length = sizeof(text) - 1;
    }
    return state;
}

static void expect_state_equal (const SIM_FrameState& a, const SIM_FrameState& b)
{
    EXPECT_EQ(a.dt, b.dt);
    EXPECT_TRUE(zm::equal(a.local_origin, b.local_origin));
    for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
    {
        EXPECT_TRUE(zm::equal(a.devices[d].pose, b.devices[d].pose)) << "device " << d;
        EXPECT_EQ(a.devices[d].active, b.devices[d].active) << "device " << d;
        EXPECT_EQ(a.devices[d].source, b.devices[d].source) << "device " << d;
        if (a.devices[d].source == SIM_HandSource::Hand)
        {
            EXPECT_TRUE(zm::equal(a.devices[d].detached, b.devices[d].detached)) << "device " << d;
        }
        for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
        {
            if (d == (u32)SIM_Device::Head)
            {
                continue;
            }
            EXPECT_TRUE(zm::equal(a.devices[d].inputs[i].value, b.devices[d].inputs[i].value)) << "device " << d << " input " << i;
        }
    }
    ASSERT_EQ(a.typed_length, b.typed_length);
    EXPECT_TRUE(memory_equal(a.typed, b.typed, a.typed_length));
    if (a.face_present && b.face_present)
    {
        EXPECT_TRUE(memory_equal(&a.face, &b.face, sizeof(a.face))) << "face";
    }
    if (a.body_present && b.body_present)
    {
        EXPECT_TRUE(memory_equal(&a.body, &b.body, sizeof(a.body))) << "body";
    }
    if (a.drift_present && b.drift_present)
    {
        EXPECT_TRUE(memory_equal(&a.tracking_drift, &b.tracking_drift, sizeof(Pose))) << "tracking drift";
    }
    if (a.props_present && b.props_present)
    {
        ASSERT_EQ(a.props_count, b.props_count);
        for (u32 p = 0; p < a.props_count; p++)
        {
            const SIM_Prop& x = a.props[p];
            const SIM_Prop& y = b.props[p];
            EXPECT_EQ(x.id, y.id);
            EXPECT_EQ(x.kind, y.kind);
            EXPECT_EQ(x.motion, y.motion);
            EXPECT_TRUE(zm::equal(x.pose, y.pose));
            EXPECT_TRUE(zm::equal(x.target, y.target));
            EXPECT_EQ(x.radius, y.radius);
            EXPECT_EQ(x.speed, y.speed);
            EXPECT_EQ(x.size, y.size);
            EXPECT_EQ(x.start, y.start);
            EXPECT_STREQ(x.text, y.text);
        }
    }
}

static REC_Header make_header ()
{
    SIM_Config config = {};
    sim_default_config(config);
    config.room[0]       = 0; // a recording names its room only where a test sets one
    SIM_Snapshot initial = {};
    initial.state        = make_state(1000);
    initial.time         = 5000000000LL;
    initial.frame_index  = 42;
    for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
    {
        initial.previous_poses[d] = make_state(999).devices[d].pose;
    }
    REC_Header header = {};
    rec_header_from_config(config, initial, header);
    return header;
}

static b8 write_recording (const char* path, u32 frames)
{
    REC_Writer* writer = rec_writer_create(path, make_header());
    if (!writer)
    {
        return false;
    }
    for (u32 i = 0; i < frames; i++)
    {
        rec_writer_append(writer, i + 1, 0xABCD0000ull + i, make_state(i));
    }
    return rec_writer_close(writer);
}

namespace
{

struct RecTest : testing::Test
{
    void SetUp () override
    {
        os_set_assert_mode(OS_AssertMode::Log);
    }
};

} // namespace

TEST_F(RecTest, round_trip)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_round_trip.xrrec");
    ASSERT_TRUE(write_recording(path, 300));

    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };

    EXPECT_TRUE(rec_is_complete(recording));
    EXPECT_EQ(rec_frame_count(recording), 300u);

    const REC_Header& header   = rec_header(recording);
    const REC_Header  expected = make_header();
    EXPECT_EQ(header.eye_width, expected.eye_width);
    EXPECT_EQ(header.eye_height, expected.eye_height);
    EXPECT_EQ(header.display_period_ns, expected.display_period_ns);
    EXPECT_FLOAT_EQ(header.refresh_rate_hz, expected.refresh_rate_hz);
    EXPECT_TRUE(zm::equal(header.aim_offset, expected.aim_offset));
    expect_state_equal(header.initial.state, expected.initial.state);
    EXPECT_EQ(header.initial.time, expected.initial.time);
    EXPECT_EQ(header.initial.frame_index, expected.initial.frame_index);
    for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
    {
        EXPECT_TRUE(zm::equal(header.initial.previous_poses[d], expected.initial.previous_poses[d]));
    }

    for (u32 i = 0; i < 300; i++)
    {
        u64            frame = 0;
        u64            hash  = 0;
        SIM_FrameState state = {};
        ASSERT_TRUE(rec_read_frame(recording, i, frame, hash, state));
        EXPECT_EQ(frame, i + 1u);
        EXPECT_EQ(hash, 0xABCD0000ull + i);
        expect_state_equal(state, make_state(i));
    }

    u64            frame = 0;
    u64            hash = 0;
    SIM_FrameState state = {};
    EXPECT_FALSE(rec_read_frame(recording, 300, frame, hash, state));
}

TEST_F(RecTest, header_records_the_room)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_room.xrrec");
    REC_Header header = make_header();
    cstr_copy(header.room, sizeof(header.room), "living_room");
    header.room_hash = 0x0123456789abcdefull;
    REC_Writer* writer = rec_writer_create(path, header);
    ASSERT_NE(writer, nullptr);
    rec_writer_append(writer, 1, 0, make_state(0));
    ASSERT_TRUE(rec_writer_close(writer));

    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_EQ(rec_header(recording).format_version, (u32)REC_FORMAT_VERSION);
    EXPECT_STREQ(rec_header(recording).room, "living_room");
    EXPECT_EQ(rec_header(recording).room_hash, 0x0123456789abcdefull);

    SIM_Config config = {};
    sim_default_config(config);
    rec_apply_header(rec_header(recording), config);
    EXPECT_STREQ(config.room, "living_room");
}

TEST_F(RecTest, header_without_a_room_round_trips)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_no_room.xrrec");
    ASSERT_TRUE(write_recording(path, 3));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_STREQ(rec_header(recording).room, "");
    EXPECT_EQ(rec_header(recording).room_hash, 0u);
}

TEST_F(RecTest, frames_are_compact)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_compact.xrrec");
    ASSERT_TRUE(write_recording(path, 1000));
    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(path, &data, &size));
    os_memory_release(data);
    EXPECT_LT(size, 1000u * 205u);
}

TEST_F(RecTest, header_applies_to_config)
{
    REC_Header header  = make_header();
    header.eye_width   = 640;
    header.eye_height  = 480;
    header.ipd         = 0.07f;
    SIM_Config config  = {};
    sim_default_config(config);
    rec_apply_header(header, config);
    EXPECT_EQ(config.view_width, 640u);
    EXPECT_EQ(config.view_height, 480u);
    EXPECT_FLOAT_EQ(config.ipd, 0.07f);
}

TEST_F(RecTest, every_truncation_is_safe)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_truncated.xrrec");
    ASSERT_TRUE(write_recording(path, 12));
    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(path, &data, &size));
    defer { os_memory_release(data); };

    u64 previous = 0;
    for (usize length = 0; length < size; length++)
    {
        REC_Recording* recording = rec_load_from_memory(data, length);
        if (!recording)
        {
            continue;
        }
        EXPECT_FALSE(rec_is_complete(recording)) << length;
        const u64 frames = rec_frame_count(recording);
        EXPECT_LE(frames, 12u);
        EXPECT_GE(frames, previous) << length;
        previous = frames;
        rec_release(recording);
    }
    EXPECT_EQ(previous, 12u);
}

TEST_F(RecTest, every_corrupted_byte_is_safe)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_corrupt.xrrec");
    ASSERT_TRUE(write_recording(path, 6));
    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(path, &data, &size));
    defer { os_memory_release(data); };

    for (usize i = 0; i < size; i++)
    {
        for (const u8 flip : { (u8)0x01, (u8)0x80, (u8)0xFF })
        {
            data[i] ^= flip;
            REC_Recording* recording = rec_load_from_memory(data, size);
            if (recording)
            {
                const u64 frames = rec_frame_count(recording);
                EXPECT_LE(frames, 6u);
                for (u64 f = 0; f < frames; f++)
                {
                    u64            frame = 0;
                    u64            hash = 0;
                    SIM_FrameState state = {};
                    EXPECT_TRUE(rec_read_frame(recording, f, frame, hash, state));
                }
                rec_release(recording);
            }
            data[i] ^= flip;
        }
    }
}

TEST_F(RecTest, rejects_foreign_and_future_files)
{
    EXPECT_EQ(rec_load_from_memory(nullptr, 0), nullptr);
    EXPECT_EQ(rec_load_from_memory((const u8*)"NOTPUPPETXR\x01\0\0\0", 12), nullptr);
    EXPECT_EQ(rec_load("/nonexistent/recording.xrrec"), nullptr);
    EXPECT_EQ(rec_load(nullptr), nullptr);

    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_version.xrrec");
    ASSERT_TRUE(write_recording(path, 2));
    u8*   data = nullptr;
    usize size = 0;
    ASSERT_TRUE(os_read_file(path, &data, &size));
    defer { os_memory_release(data); };
    data[8] = 0;
    EXPECT_EQ(rec_load_from_memory(data, size), nullptr);
    data[8] = REC_FORMAT_VERSION + 1;
    EXPECT_EQ(rec_load_from_memory(data, size), nullptr);
    data[8] = REC_FORMAT_VERSION;
    REC_Recording* recording = rec_load_from_memory(data, size);
    ASSERT_NE(recording, nullptr);
    EXPECT_EQ(rec_header(recording).format_version, (u32)REC_FORMAT_VERSION);
    rec_release(recording);
}

namespace
{

// Edits a name in the header the way a different build would have written it, then fixes the CRC
struct EditedRecording
{
    u8*   data = nullptr;
    usize size = 0;

    ~EditedRecording ()
    {
        os_memory_release(data);
    }

    b8 load (const char* path)
    {
        return os_read_file(path, &data, &size);
    }

    b8 rename (const char* from, const char* to) const
    {
        const usize length = cstr_length(from);
        if (length != cstr_length(to))
        {
            return false;
        }
        const u32 header_size = (u32)data[12] | ((u32)data[13] << 8) | ((u32)data[14] << 16) | ((u32)data[15] << 24);
        u8*       header      = data + 16;
        for (usize i = 0; i + length < header_size; i++)
        {
            if (header[i] == length && memory_equal(header + i + 1, from, length))
            {
                memory_copy(header + i + 1, to, length);
                const u32 crc = crc32(header, header_size);
                u8*       out = header + header_size;
                out[0] = (u8)crc;
                out[1] = (u8)(crc >> 8);
                out[2] = (u8)(crc >> 16);
                out[3] = (u8)(crc >> 24);
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_F(RecTest, header_records_the_writer_and_controller)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_writer.xrrec");
    SIM_Config config = {};
    sim_default_config(config);
    config.controller = SIM_Controller::KhrSimple;
    REC_Header header = {};
    rec_header_from_config(config, make_header().initial, header);
    ASSERT_TRUE(rec_writer_close(rec_writer_create(path, header)));

    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    EXPECT_EQ(rec_header(recording).writer_version, (u32)PUPPETXR_VERSION);
    EXPECT_EQ(rec_header(recording).controller, SIM_Controller::KhrSimple);
    EXPECT_EQ(rec_unknown_inputs(recording), 0u);
    EXPECT_EQ(rec_unknown_devices(recording), 0u);

    SIM_Config replay = {};
    sim_default_config(replay);
    rec_apply_header(rec_header(recording), replay);
    EXPECT_EQ(replay.controller, SIM_Controller::KhrSimple);
    rec_release(recording);
}

TEST_F(RecTest, meta_controllers_round_trip)
{
    const SIM_Controller controllers[2] = { SIM_Controller::MetaTouchPro, SIM_Controller::MetaTouchPlus };
    for (const SIM_Controller controller : controllers)
    {
        char path[OS_MAX_PATH];
        temp_path(path, sizeof(path), "puppetxr_test_meta_controller.xrrec");
        SIM_Config config = {};
        sim_default_config(config);
        config.controller = controller;
        REC_Header header = {};
        rec_header_from_config(config, make_header().initial, header);
        ASSERT_TRUE(rec_writer_close(rec_writer_create(path, header)));
        REC_Recording* recording = rec_load(path);
        ASSERT_NE(recording, nullptr);
        EXPECT_EQ(rec_header(recording).controller, controller);
        EXPECT_EQ(rec_unknown_inputs(recording), 0u);
        rec_release(recording);
    }
}

// A recording from a build with an input this one lacks (or the reverse) still replays
TEST_F(RecTest, unknown_inputs_are_skipped_and_missing_ones_read_zero)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_unknown_input.xrrec");
    ASSERT_TRUE(write_recording(path, 8));
    EditedRecording file;
    ASSERT_TRUE(file.load(path));
    ASSERT_TRUE(file.rename("trigger/value", "trigger/other"));

    REC_Recording* recording = rec_load_from_memory(file.data, file.size);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_EQ(rec_unknown_inputs(recording), 1u);
    EXPECT_EQ(rec_frame_count(recording), 8u);
    EXPECT_TRUE(rec_is_complete(recording));

    for (u32 i = 0; i < 8; i++)
    {
        u64            frame = 0;
        u64            hash = 0;
        SIM_FrameState state = {};
        ASSERT_TRUE(rec_read_frame(recording, i, frame, hash, state));
        const SIM_FrameState expected = make_state(i);
        const u32            left     = (u32)SIM_Device::LeftHand;
        const u32            right    = (u32)SIM_Device::RightHand;
        EXPECT_FLOAT_EQ(state.devices[left].inputs[(u32)SIM_Input::TriggerValue].value.x, 0.0f);
        EXPECT_TRUE(zm::equal(state.devices[right].inputs[(u32)SIM_Input::Thumbstick].value, expected.devices[right].inputs[(u32)SIM_Input::Thumbstick].value));
        EXPECT_TRUE(zm::equal(state.devices[left].pose, expected.devices[left].pose));
    }
}

TEST_F(RecTest, unknown_devices_are_skipped)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_unknown_device.xrrec");
    ASSERT_TRUE(write_recording(path, 4));
    EditedRecording file;
    ASSERT_TRUE(file.load(path));
    ASSERT_TRUE(file.rename("left", "foot"));

    REC_Recording* recording = rec_load_from_memory(file.data, file.size);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_EQ(rec_unknown_devices(recording), 1u);

    u64            frame = 0;
    u64            hash = 0;
    SIM_FrameState state = {};
    ASSERT_TRUE(rec_read_frame(recording, 1, frame, hash, state));
    EXPECT_FALSE(state.devices[(u32)SIM_Device::LeftHand].active);
    EXPECT_TRUE(zm::equal(state.devices[(u32)SIM_Device::LeftHand].pose, zm::POSE_IDENTITY));
    EXPECT_TRUE(zm::equal(state.devices[(u32)SIM_Device::RightHand].pose, make_state(1).devices[(u32)SIM_Device::RightHand].pose));
}

TEST_F(RecTest, unknown_controllers_and_duplicate_names_are_rejected)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_layout_errors.xrrec");
    ASSERT_TRUE(write_recording(path, 2));
    {
        EditedRecording file;
        ASSERT_TRUE(file.load(path));
        ASSERT_TRUE(file.rename("meta_touch_plus", "valve_index_ctl"));
        EXPECT_EQ(rec_load_from_memory(file.data, file.size), nullptr);
    }
    {
        EditedRecording file;
        ASSERT_TRUE(file.load(path));
        ASSERT_TRUE(file.rename("x/touch", "y/touch"));
        EXPECT_EQ(rec_load_from_memory(file.data, file.size), nullptr);
    }
    {
        EditedRecording file;
        ASSERT_TRUE(file.load(path));
        ASSERT_TRUE(file.rename("x/touch", "X/touch"));
        EXPECT_EQ(rec_load_from_memory(file.data, file.size), nullptr);
    }
}

TEST_F(RecTest, invalid_frame_values_truncate_the_recording)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_nan.xrrec");
    REC_Writer* writer = rec_writer_create(path, make_header());
    ASSERT_NE(writer, nullptr);
    for (u32 i = 0; i < 10; i++)
    {
        SIM_FrameState state = make_state(i);
        if (i == 5)
        {
            state.devices[0].pose.position.x = zm::nan();
        }
        if (i == 7)
        {
            state.dt = -1;
        }
        rec_writer_append(writer, i, i, state);
    }
    EXPECT_EQ(rec_writer_frame_count(writer), 10u);
    ASSERT_TRUE(rec_writer_close(writer));

    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    EXPECT_EQ(rec_frame_count(recording), 5u);
    EXPECT_FALSE(rec_is_complete(recording));
    rec_release(recording);
}

TEST_F(RecTest, frames_survive_without_close)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_crash.xrrec");
    REC_Writer* writer = rec_writer_create(path, make_header());
    ASSERT_NE(writer, nullptr);
    for (u32 i = 0; i < 3; i++)
    {
        ASSERT_TRUE(rec_writer_append(writer, i, i, make_state(i)));
    }

    // The app crashed: the writer was never closed
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    EXPECT_EQ(rec_frame_count(recording), 3u);
    EXPECT_FALSE(rec_is_complete(recording));
    rec_release(recording);
    rec_writer_close(writer);
}

TEST_F(RecTest, writer_fails_cleanly)
{
    EXPECT_EQ(rec_writer_create("/nonexistent/dir/rec.xrrec", make_header()), nullptr);
    EXPECT_FALSE(rec_writer_close(nullptr));
}

//
// Anchors
//

static W_StoredAnchor rec_anchor (u8 tag, const char* room, u64 hash)
{
    W_StoredAnchor anchor = { .room_hash = hash, .pose = { { 0.25f * (f32)tag, 1.0f, -2.0f }, zm::quat_from_euler({ 0.0f, 0.1f * (f32)tag, 0.0f }) } };
    anchor.uuid.bytes[0]  = tag;
    anchor.uuid.bytes[15] = 0xee;
    cstr_copy(anchor.room, sizeof(anchor.room), room);
    return anchor;
}

static void expect_anchor_equal (const W_StoredAnchor& a, const W_StoredAnchor& b)
{
    EXPECT_TRUE(w_uuid_equal(a.uuid, b.uuid));
    EXPECT_STREQ(a.room, b.room);
    EXPECT_EQ(a.room_hash, b.room_hash);
    EXPECT_TRUE(zm::equal(a.pose, b.pose));
}

TEST_F(RecTest, header_records_the_anchor_settings_and_the_store)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_anchor_header.xrrec");
    REC_Header header             = make_header();
    header.room_from_stage        = { { 0.3f, 0.0f, -0.2f }, zm::quat_from_euler({ 0.0f, 1.0f, 0.0f }) };
    header.anchor_latency_frames  = 3;
    header.anchor_localize_frames = 7;
    header.anchor_faults[1]       = { .result = -1000169002, .remaining = 2 };
    W_Store store     = {};
    store.next_serial = 9;
    store.runs        = 4;
    w_store_put(store, rec_anchor(1, "bedroom", 0x1234));
    w_store_put(store, rec_anchor(2, "", 0));
    defer { w_store_release(store); };

    REC_Writer* writer = rec_writer_create(path, header, &store);
    ASSERT_NE(writer, nullptr);
    rec_writer_append(writer, 1, 0, make_state(0));
    ASSERT_TRUE(rec_writer_close(writer));

    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_TRUE(rec_is_complete(recording));
    EXPECT_EQ(rec_frame_count(recording), 1u);
    const REC_Header& loaded = rec_header(recording);
    EXPECT_TRUE(zm::equal(loaded.room_from_stage, header.room_from_stage));
    EXPECT_EQ(loaded.anchor_latency_frames, 3u);
    EXPECT_EQ(loaded.anchor_localize_frames, 7u);
    EXPECT_EQ(loaded.anchor_faults[1].result, -1000169002);
    EXPECT_EQ(loaded.anchor_faults[1].remaining, 2u);
    EXPECT_EQ(loaded.anchor_faults[0].remaining, 0u);

    const W_Store& recorded = rec_store(recording);
    EXPECT_EQ(recorded.next_serial, 9u);
    EXPECT_EQ(recorded.runs, 4u);
    ASSERT_EQ(recorded.anchors.length, 2u);
    expect_anchor_equal(recorded.anchors[0], store.anchors[0]);
    expect_anchor_equal(recorded.anchors[1], store.anchors[1]);

    SIM_Config config = {};
    sim_default_config(config);
    rec_apply_header(loaded, config);
    EXPECT_EQ(config.origin_offset_mode, SIM_OriginOffset::Fixed);
    EXPECT_TRUE(zm::equal(config.origin_offset, header.room_from_stage));
    EXPECT_EQ(config.anchor_latency_frames, 3u);
    EXPECT_EQ(config.anchor_localize_frames, 7u);
}

TEST_F(RecTest, header_records_the_pending_frame_faults)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_frame_faults.xrrec");
    REC_Header header      = make_header();
    header.frame_faults[0] = { .result = -2, .remaining = 3 };
    header.frame_faults[3] = { .result = -13, .remaining = 1 };
    REC_Writer* writer     = rec_writer_create(path, header, nullptr);
    ASSERT_NE(writer, nullptr);
    rec_writer_append(writer, 1, 0, make_state(0));
    ASSERT_TRUE(rec_writer_close(writer));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    const REC_Header& loaded = rec_header(recording);
    EXPECT_EQ(loaded.frame_faults[0].result, -2);
    EXPECT_EQ(loaded.frame_faults[0].remaining, 3u);
    EXPECT_EQ(loaded.frame_faults[1].remaining, 0u);
    EXPECT_EQ(loaded.frame_faults[3].result, -13);
    EXPECT_EQ(loaded.frame_faults[3].remaining, 1u);

    header.frame_faults[2] = { .result = 1, .remaining = 1 };
    writer                 = rec_writer_create(path, header, nullptr);
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(rec_writer_close(writer));
    EXPECT_EQ(rec_load(path), nullptr) << "a pending fault is an error";
}

TEST_F(RecTest, recordings_without_a_store_replay_an_empty_one)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_no_store.xrrec");
    ASSERT_TRUE(write_recording(path, 2));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_EQ(rec_store(recording).anchors.length, 0u);
    EXPECT_TRUE(zm::equal(rec_header(recording).room_from_stage, zm::POSE_IDENTITY));
    EXPECT_EQ(rec_external_count(recording), 0u);
}

TEST_F(RecTest, external_inputs_replay_in_order)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_external.xrrec");
    REC_Writer* writer = rec_writer_create(path, make_header(), nullptr);
    ASSERT_NE(writer, nullptr);
    rec_writer_append(writer, 1, 0, make_state(0));

    REC_External cloud = { .kind = REC_ExternalKind::CloudAnchors, .frame_index = 1 };
    cloud.anchors.add(rec_anchor(3, "office", 77));
    ASSERT_TRUE(rec_writer_append_external(writer, cloud));
    rec_external_release(cloud);
    rec_writer_append(writer, 2, 0, make_state(1));

    REC_External adverts = { .kind = REC_ExternalKind::Adverts, .frame_index = 2 };
    W_Advert     advert  = { .owner = 5, .size = 4, .buffer = { 1, 2, 3, 4 } };
    advert.uuid.bytes[3] = 9;
    adverts.adverts.add(advert);
    ASSERT_TRUE(rec_writer_append_external(writer, adverts));
    rec_external_release(adverts);
    const REC_External nothing = { .kind = REC_ExternalKind::CloudAnchors, .frame_index = 2 };
    ASSERT_TRUE(rec_writer_append_external(writer, nothing));
    ASSERT_TRUE(rec_writer_close(writer));

    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_TRUE(rec_is_complete(recording));
    EXPECT_EQ(rec_frame_count(recording), 2u);
    ASSERT_EQ(rec_external_count(recording), 3u);

    REC_External read = {};
    defer { rec_external_release(read); };
    ASSERT_TRUE(rec_read_external(recording, 0, read));
    EXPECT_EQ(read.kind, REC_ExternalKind::CloudAnchors);
    EXPECT_EQ(read.frame_index, 1u);
    ASSERT_EQ(read.anchors.length, 1u);
    expect_anchor_equal(read.anchors[0], rec_anchor(3, "office", 77));

    ASSERT_TRUE(rec_read_external(recording, 1, read));
    EXPECT_EQ(read.kind, REC_ExternalKind::Adverts);
    EXPECT_EQ(read.anchors.length, 0u);
    ASSERT_EQ(read.adverts.length, 1u);
    EXPECT_EQ(read.adverts[0].owner, 5u);
    EXPECT_EQ(read.adverts[0].uuid.bytes[3], 9);
    ASSERT_EQ(read.adverts[0].size, 4u);
    EXPECT_EQ(read.adverts[0].buffer[3], 4);

    ASSERT_TRUE(rec_read_external(recording, 2, read));
    EXPECT_EQ(read.anchors.length, 0u);
    EXPECT_FALSE(rec_read_external(recording, 3, read));
}

TEST_F(RecTest, hand_sources_round_trip)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_hand_source.xrrec");
    ASSERT_TRUE(write_recording(path, 8));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    u64            frame = 0;
    u64            hash  = 0;
    SIM_FrameState state = {};
    ASSERT_TRUE(rec_read_frame(recording, 3, frame, hash, state));
    EXPECT_EQ(state.devices[(u32)SIM_Device::RightHand].source, SIM_HandSource::Hand);
    ASSERT_TRUE(rec_read_frame(recording, 4, frame, hash, state));
    EXPECT_EQ(state.devices[(u32)SIM_Device::RightHand].source, SIM_HandSource::Controller);
}

TEST_F(RecTest, typed_text_round_trips)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_typed.xrrec");
    ASSERT_TRUE(write_recording(path, 8));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    u64            frame = 0;
    u64            hash  = 0;
    SIM_FrameState state = {};
    ASSERT_TRUE(rec_read_frame(recording, 2, frame, hash, state));
    ASSERT_EQ(state.typed_length, 6u);
    EXPECT_TRUE(memory_equal(state.typed, "h\xc3\xa9!\b\n", 6));
    ASSERT_TRUE(rec_read_frame(recording, 3, frame, hash, state));
    EXPECT_EQ(state.typed_length, 0u);
}

// Props go into a frame only when they changed since the last one written
TEST_F(RecTest, props_are_written_when_they_change)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_props.xrrec");
    ASSERT_TRUE(write_recording(path, 6));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_TRUE(rec_header(recording).initial.state.props_present);
    EXPECT_EQ(rec_header(recording).initial.state.props_count, 2u);
    const b8 present[6] = { true, false, false, true, false, false };
    for (u32 i = 0; i < 6; i++)
    {
        u64            frame = 0;
        u64            hash  = 0;
        SIM_FrameState state = {};
        ASSERT_TRUE(rec_read_frame(recording, i, frame, hash, state));
        EXPECT_EQ(state.props_present, present[i]) << i;
        if (state.props_present)
        {
            expect_state_equal(state, make_state(i));
        }
    }
}

TEST_F(RecTest, body_is_written_when_it_changes)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_body.xrrec");
    ASSERT_TRUE(write_recording(path, 6));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_TRUE(rec_header(recording).initial.state.body_present);
    const b8 present[6] = { true, false, false, true, false, false };
    for (u32 i = 0; i < 6; i++)
    {
        u64            frame = 0;
        u64            hash  = 0;
        SIM_FrameState state = {};
        ASSERT_TRUE(rec_read_frame(recording, i, frame, hash, state));
        EXPECT_EQ(state.body_present, present[i]) << i;
        if (state.body_present)
        {
            const SIM_FrameState expected = make_state(i);
            EXPECT_TRUE(memory_equal(&state.body, &expected.body, sizeof(state.body))) << i;
        }
    }
}

TEST_F(RecTest, face_is_written_when_it_changes)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_face.xrrec");
    ASSERT_TRUE(write_recording(path, 6));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_TRUE(rec_header(recording).initial.state.face_present);
    const b8 present[6] = { true, false, false, false, true, false };
    for (u32 i = 0; i < 6; i++)
    {
        u64            frame = 0;
        u64            hash  = 0;
        SIM_FrameState state = {};
        ASSERT_TRUE(rec_read_frame(recording, i, frame, hash, state));
        EXPECT_EQ(state.face_present, present[i]) << i;
        if (state.face_present)
        {
            const SIM_FrameState expected = make_state(i);
            EXPECT_TRUE(memory_equal(&state.face, &expected.face, sizeof(state.face))) << i;
        }
    }
}

TEST_F(RecTest, tracking_drift_is_written_when_it_changes)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_drift.xrrec");
    ASSERT_TRUE(write_recording(path, 6));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_TRUE(rec_header(recording).initial.state.drift_present);
    const b8 present[6] = { true, false, true, false, true, false };
    for (u32 i = 0; i < 6; i++)
    {
        u64            frame = 0;
        u64            hash  = 0;
        SIM_FrameState state = {};
        ASSERT_TRUE(rec_read_frame(recording, i, frame, hash, state));
        EXPECT_EQ(state.drift_present, present[i]) << i;
        if (state.drift_present)
        {
            const SIM_FrameState expected = make_state(i);
            EXPECT_TRUE(memory_equal(&state.tracking_drift, &expected.tracking_drift, sizeof(Pose))) << i;
        }
    }
}

// Tracking keeps gravity, so a tilted drift can only come from a corrupt file
TEST_F(RecTest, a_tilted_drift_truncates_the_recording)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_drift_invalid.xrrec");
    REC_Writer* writer = rec_writer_create(path, make_header());
    ASSERT_NE(writer, nullptr);
    for (u32 i = 0; i < 6; i++)
    {
        SIM_FrameState state = make_state(i);
        if (i == 3)
        {
            state.tracking_drift.orientation = zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, 0.2f);
        }
        rec_writer_append(writer, i, i, state);
    }
    ASSERT_TRUE(rec_writer_close(writer));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_EQ(rec_frame_count(recording), 3u);
}

TEST_F(RecTest, invalid_face_truncates_the_recording)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_face_invalid.xrrec");
    REC_Writer* writer = rec_writer_create(path, make_header());
    ASSERT_NE(writer, nullptr);
    for (u32 i = 0; i < 6; i++)
    {
        SIM_FrameState state = make_state(i);
        if (i == 5)
        {
            state.face.weights[(u32)SIM_FaceExpression::JawDrop] = 1.5f;
        }
        rec_writer_append(writer, i, i, state);
    }
    ASSERT_TRUE(rec_writer_close(writer));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_EQ(rec_frame_count(recording), 5u);
}

TEST_F(RecTest, invalid_body_truncates_the_recording)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_body_invalid.xrrec");
    REC_Writer* writer = rec_writer_create(path, make_header());
    ASSERT_NE(writer, nullptr);
    for (u32 i = 0; i < 6; i++)
    {
        SIM_FrameState state = make_state(i);
        if (i == 4)
        {
            state.body.bone_scales[(u32)SIM_BodyJoint::Chest] = 9.0f;
        }
        rec_writer_append(writer, i, i, state);
    }
    ASSERT_TRUE(rec_writer_close(writer));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_EQ(rec_frame_count(recording), 4u);
}

TEST_F(RecTest, invalid_props_truncate_the_recording)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_props_invalid.xrrec");
    REC_Writer* writer = rec_writer_create(path, make_header());
    ASSERT_NE(writer, nullptr);
    for (u32 i = 0; i < 6; i++)
    {
        SIM_FrameState state = make_state(i);
        if (i == 4)
        {
            state.props[1].speed = zm::nan();
        }
        rec_writer_append(writer, i, i, state);
    }
    ASSERT_TRUE(rec_writer_close(writer));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_EQ(rec_frame_count(recording), 4u);
}

TEST_F(RecTest, control_characters_in_typed_text_truncate_the_recording)
{
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_typed_control.xrrec");
    REC_Writer* writer = rec_writer_create(path, make_header());
    ASSERT_NE(writer, nullptr);
    for (u32 i = 0; i < 6; i++)
    {
        SIM_FrameState state = make_state(i);
        if (i == 4)
        {
            state.typed[0]     = '\x1b';
            state.typed_length = 1;
        }
        rec_writer_append(writer, i, i, state);
    }
    ASSERT_TRUE(rec_writer_close(writer));
    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_EQ(rec_frame_count(recording), 4u);
}

TEST_F(RecTest, header_records_the_hand_tracking_cone)
{
    SIM_Config config = {};
    sim_default_config(config);
    config.hand_tracking_fov_degrees = 100.0f;
    REC_Header header = {};
    rec_header_from_config(config, make_header().initial, header);
    char path[OS_MAX_PATH];
    temp_path(path, sizeof(path), "puppetxr_test_hand_cone.xrrec");
    REC_Writer* writer = rec_writer_create(path, header);
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(rec_writer_close(writer));

    REC_Recording* recording = rec_load(path);
    ASSERT_NE(recording, nullptr);
    defer { rec_release(recording); };
    EXPECT_FLOAT_EQ(rec_header(recording).hand_tracking_fov_degrees, 100.0f);
    SIM_Config replayed = {};
    sim_default_config(replayed);
    rec_apply_header(rec_header(recording), replayed);
    EXPECT_FLOAT_EQ(replayed.hand_tracking_fov_degrees, 100.0f);
}
