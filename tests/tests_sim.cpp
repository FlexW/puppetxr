#include "sim.h"

#include <gtest/gtest.h>

static SIM_Sim* make_sim (SIM_ClockMode mode = SIM_ClockMode::FixedStep)
{
    SIM_Config config = {};
    sim_default_config(config);
    config.clock_mode = mode;
    return sim_create(config);
}

// Keys reach their value in the frame they are pressed
static SIM_Sim* make_instant_sim ()
{
    SIM_Config config = {};
    sim_default_config(config);
    config.clock_mode     = SIM_ClockMode::FixedStep;
    config.analog_ramp    = 0.0f;
    config.analog_release = 0.0f;
    return sim_create(config);
}

static void step (SIM_Sim* sim, u64 os_time_ns = 0)
{
    sim_begin_frame(sim, os_time_ns);
    sim_commit_frame(sim);
}

TEST(Sim, default_config_is_sane)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_EQ(config.clock_mode, SIM_ClockMode::Realtime);
    EXPECT_GT(config.display_period_ns, 0);
    EXPECT_FLOAT_EQ(config.refresh_rate_hz, 90.0f);
    EXPECT_EQ(config.mirror_view, SIM_MirrorView::Left);
    EXPECT_GT(config.view_width, 0u);
    EXPECT_GT(config.view_height, 0u);
    EXPECT_GT(config.ipd, 0.0f);
    EXPECT_TRUE(config.devices[(u32)SIM_Device::Head].active);
    EXPECT_GT(config.devices[(u32)SIM_Device::Head].pose.position.y, 1.0f);
    EXPECT_GT(config.bindings_count, 0u);
    ASSERT_GE(config.profiles_count, 6u);
    EXPECT_STREQ(config.profiles[0].name, "body");
    EXPECT_EQ(config.profiles[0].key, OS_Key::Digit1);
    EXPECT_STREQ(config.profiles[2].name, "hands");
    EXPECT_EQ(config.profiles[2].key, OS_Key::Digit3);
    EXPECT_EQ(config.key_next_profile, OS_Key::RightBracket);
    EXPECT_EQ(config.key_previous_profile, OS_Key::LeftBracket);
    EXPECT_EQ(config.key_ui, OS_Key::Tab);
    EXPECT_EQ(config.key_reset, OS_Key::R);
    EXPECT_EQ(config.key_freeze, OS_Key::Space);
    EXPECT_FLOAT_EQ(config.analog_ramp, 0.5f);
    EXPECT_FLOAT_EQ(config.analog_release, 0.1f);
}

TEST(Sim, config_parses_json_and_keeps_defaults_for_missing_keys)
{
    SIM_Config config = {};
    sim_default_config(config);
    const b8 ok = sim_parse_config(config, R"({
        "headless": true,
        "window": { "width": 640, "height": 480 },
        "clock": { "mode": "fixed_step", "refresh_rate_hz": 100 },
        "views": { "width": 512, "height": 256, "fov_degrees": 90, "ipd": 0.07, "mirror": "both" },
        "stage": { "width": 3, "depth": 4 },
        "devices": {
            "head":  { "position": [0, 1.7, 0], "orientation": [0, 0, 0, 1] },
            "right": { "active": false }
        },
        "controls": {
            "move_speed": 2.5,
            "look_sensitivity": 0.01,
            "turn_speed": 2.0,
            "profiles": [ { "name": "body", "key": "f1" } ],
            "reset_hands": "backspace",
            "reset": "f2",
            "freeze": "tab",
            "next_profile": "right_bracket",
            "previous_profile": "f3",
            "analog_ramp": 0.25,
            "analog_release": 0,
            "primary_hand": "left",
            "auto_touch": false,
            "bindings": [
                { "key": "space", "input": "trigger/value" },
                { "key": "mouse_middle", "slot": "grip", "modifier": "shift", "hand": "left" },
                { "key": "g", "slot": "nonsense" },
                { "key": "g", "slot": "primary", "modifier": "sideways" }
            ]
        },
        "record": "out.xrrec",
        "replay": "in.xrrec",
        "screenshot_dir": "shots",
        "room": "bedroom",
        "rooms_dir": "my/rooms"
    })");
    EXPECT_TRUE(ok);
    EXPECT_TRUE(config.headless);
    EXPECT_EQ(config.window_width, 640u);
    EXPECT_EQ(config.window_height, 480u);
    EXPECT_EQ(config.clock_mode, SIM_ClockMode::FixedStep);
    EXPECT_EQ(config.display_period_ns, 10000000);
    EXPECT_FLOAT_EQ(config.refresh_rate_hz, 100.0f);
    EXPECT_EQ(config.mirror_view, SIM_MirrorView::Both);
    EXPECT_EQ(config.view_width, 512u);
    EXPECT_EQ(config.view_height, 256u);
    EXPECT_FLOAT_EQ(config.fov_degrees, 90.0f);
    EXPECT_FLOAT_EQ(config.ipd, 0.07f);
    EXPECT_FLOAT_EQ(config.stage_bounds.x, 3.0f);
    EXPECT_FLOAT_EQ(config.stage_bounds.y, 4.0f);
    EXPECT_FLOAT_EQ(config.devices[(u32)SIM_Device::Head].pose.position.y, 1.7f);
    EXPECT_TRUE(config.devices[(u32)SIM_Device::LeftHand].active);
    EXPECT_FALSE(config.devices[(u32)SIM_Device::RightHand].active);
    EXPECT_FLOAT_EQ(config.move_speed, 2.5f);
    EXPECT_FLOAT_EQ(config.turn_speed, 2.0f);
    EXPECT_EQ(config.profiles[0].key, OS_Key::F1);
    EXPECT_EQ(config.profiles[0].wasd, SIM_Drive::Walk) << "a profile of a known name changes only what the entry names";
    EXPECT_EQ(config.key_reset_hands, OS_Key::Backspace);
    EXPECT_EQ(config.key_reset, OS_Key::F2);
    EXPECT_EQ(config.key_freeze, OS_Key::Tab);
    EXPECT_EQ(config.key_next_profile, OS_Key::RightBracket);
    EXPECT_EQ(config.key_previous_profile, OS_Key::F3);
    EXPECT_FLOAT_EQ(config.analog_ramp, 0.25f);
    EXPECT_FLOAT_EQ(config.analog_release, 0.0f);
    EXPECT_EQ(config.primary_hand, SIM_Device::LeftHand);
    EXPECT_FALSE(config.auto_touch);
    EXPECT_EQ(config.bindings_count, 2u);
    EXPECT_EQ(config.bindings[0].key, OS_Key::Space);
    EXPECT_FALSE(config.bindings[0].is_slot);
    EXPECT_EQ(config.bindings[0].input, SIM_Input::TriggerValue);
    EXPECT_EQ(config.bindings[0].modifiers, 0u);
    EXPECT_EQ(config.bindings[0].hand, SIM_BindHand::Active);
    EXPECT_TRUE(config.bindings[1].is_slot);
    EXPECT_EQ(config.bindings[1].slot, SIM_Slot::Grip);
    EXPECT_EQ(config.bindings[1].modifiers, (u8)SIM_MOD_SHIFT);
    EXPECT_EQ(config.bindings[1].hand, SIM_BindHand::Left);
    EXPECT_TRUE(cstr_equal(config.record_path, "out.xrrec"));
    EXPECT_TRUE(cstr_equal(config.replay_path, "in.xrrec"));
    EXPECT_TRUE(cstr_equal(config.screenshot_dir, "shots"));
    EXPECT_TRUE(cstr_equal(config.room, "bedroom"));
    EXPECT_TRUE(cstr_equal(config.rooms_dir, "my/rooms"));
}

// docs/profiling.md: a capture from the first instance to the runtime's shutdown
TEST(Sim, profile_path_from_config_and_environment)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_EQ(config.profile_path[0], 0);
    ASSERT_TRUE(sim_parse_config(config, R"({ "profile": "trace.json" })"));
    EXPECT_STREQ(config.profile_path, "trace.json");
    os_set_env("PUPPETXR_PROFILE", "env.json");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_PROFILE");
    EXPECT_STREQ(config.profile_path, "env.json");
}

// Apps that need a room, such as Meta's MRUK samples, find one without settings
TEST(Sim, room_defaults_to_living_room_and_env_overrides_it)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_TRUE(cstr_equal(config.room, "living_room"));

    EXPECT_TRUE(sim_parse_config(config, R"({ "room": "office" })"));
    EXPECT_TRUE(cstr_equal(config.room, "office"));
    EXPECT_TRUE(sim_parse_config(config, R"({ "room": "none" })"));
    EXPECT_EQ(config.room[0], 0);

    os_set_env("PUPPETXR_ROOM", "living_room");
    os_set_env("PUPPETXR_ROOMS_DIR", "/elsewhere");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_ROOM");
    os_unset_env("PUPPETXR_ROOMS_DIR");
    EXPECT_TRUE(cstr_equal(config.room, "living_room"));
    EXPECT_TRUE(cstr_equal(config.rooms_dir, "/elsewhere"));
}

TEST(Sim, remembered_room_applies_only_without_an_explicit_one)
{
    SIM_State state = {};
    EXPECT_TRUE(sim_parse_state(R"({ "room": "office" })", state));
    EXPECT_TRUE(state.has_room);
    EXPECT_TRUE(cstr_equal(state.room, "office"));

    SIM_Config config = {};
    sim_default_config(config);
    config.headless = false;
    EXPECT_FALSE(config.room_explicit);
    sim_apply_state(config, state);
    EXPECT_TRUE(cstr_equal(config.room, "office"));

    sim_default_config(config);
    config.headless = false;
    EXPECT_TRUE(sim_parse_config(config, R"({ "room": "bedroom" })"));
    EXPECT_TRUE(config.room_explicit);
    sim_apply_state(config, state);
    EXPECT_TRUE(cstr_equal(config.room, "bedroom"));

    sim_default_config(config);
    config.headless = false;
    EXPECT_TRUE(sim_parse_config(config, R"({ "room": "none" })"));
    sim_apply_state(config, state);
    EXPECT_EQ(config.room[0], 0);

    sim_default_config(config);
    config.headless = false;
    os_set_env("PUPPETXR_ROOM", "living_room");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_ROOM");
    sim_apply_state(config, state);
    EXPECT_TRUE(cstr_equal(config.room, "living_room"));

    // Tests, CI and agents run headless and must not depend on someone's last click
    sim_default_config(config);
    config.headless = true;
    sim_apply_state(config, state);
    EXPECT_TRUE(cstr_equal(config.room, "living_room"));

    sim_default_config(config);
    config.headless = false;
    cstr_copy(config.room, sizeof(config.room), "office");
    EXPECT_TRUE(sim_parse_state(R"({ "room": "none" })", state));
    EXPECT_TRUE(state.has_room);
    sim_apply_state(config, state);
    EXPECT_EQ(config.room[0], 0);

    EXPECT_FALSE(sim_parse_state("not json", state));
    EXPECT_FALSE(state.has_room);
    EXPECT_TRUE(sim_parse_state(R"({ "room": 5 })", state));
    EXPECT_FALSE(state.has_room);
    EXPECT_TRUE(sim_parse_state(R"({ "room": "this name is far too long for any room buffer, longer than sixty-four" })", state));
    EXPECT_FALSE(state.has_room);
    EXPECT_TRUE(sim_parse_state(R"({})", state));
    EXPECT_FALSE(state.has_room);
}

TEST(Sim, state_file_round_trips)
{
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/sim_state_%u.json", PUPPETXR_TEST_OUTPUT_DIR, os_process_id());
    defer { os_delete_file(path); };

    SIM_State saved = { .has_room = true };
    cstr_copy(saved.room, sizeof(saved.room), "l_shaped");
    ASSERT_TRUE(sim_save_state(path, saved));
    SIM_State loaded = {};
    ASSERT_TRUE(sim_load_state(path, loaded));
    EXPECT_TRUE(loaded.has_room);
    EXPECT_TRUE(cstr_equal(loaded.room, "l_shaped"));

    saved.room[0] = 0;
    ASSERT_TRUE(sim_save_state(path, saved));
    ASSERT_TRUE(sim_load_state(path, loaded));
    EXPECT_TRUE(loaded.has_room);
    EXPECT_EQ(loaded.room[0], 0);

    saved = { .has_room = true };
    cstr_copy(saved.room, sizeof(saved.room), "bad\"name");
    EXPECT_FALSE(sim_save_state(path, saved));

    saved = { .has_room = true, .welcome_dismissed = true };
    cstr_copy(saved.room, sizeof(saved.room), "office");
    ASSERT_TRUE(sim_save_state(path, saved));
    ASSERT_TRUE(sim_load_state(path, loaded));
    EXPECT_TRUE(loaded.has_room);
    EXPECT_TRUE(cstr_equal(loaded.room, "office"));
    EXPECT_TRUE(loaded.welcome_dismissed);

    saved = { .welcome_dismissed = true };
    ASSERT_TRUE(sim_save_state(path, saved));
    ASSERT_TRUE(sim_load_state(path, loaded));
    EXPECT_FALSE(loaded.has_room);
    EXPECT_TRUE(loaded.welcome_dismissed);

    os_delete_file(path);
    EXPECT_FALSE(sim_load_state(path, loaded));
    EXPECT_FALSE(loaded.has_room);
    EXPECT_FALSE(loaded.welcome_dismissed);
}

TEST(Sim, welcome_dismissal_is_remembered_next_to_the_room)
{
    SIM_State state = {};
    EXPECT_TRUE(sim_parse_state(R"({ "room": "office" })", state));
    EXPECT_FALSE(state.welcome_dismissed);
    EXPECT_TRUE(sim_parse_state(R"({ "welcome_dismissed": true })", state));
    EXPECT_TRUE(state.welcome_dismissed);
    EXPECT_FALSE(state.has_room);
    EXPECT_TRUE(sim_parse_state(R"({ "room": "none", "welcome_dismissed": false })", state));
    EXPECT_FALSE(state.welcome_dismissed);
    EXPECT_TRUE(state.has_room);
}

TEST(Sim, window_starts_maximized_unless_configured)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_TRUE(config.window_maximized);
    EXPECT_TRUE(sim_parse_config(config, R"({ "window": { "width": 800, "height": 600, "maximized": false } })"));
    EXPECT_FALSE(config.window_maximized);
    EXPECT_EQ(config.window_width, 800u);
}

TEST(Sim, welcome_can_be_switched_off_by_config_and_environment)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_TRUE(config.show_welcome);
    EXPECT_TRUE(sim_parse_config(config, R"({ "show_welcome": false })"));
    EXPECT_FALSE(config.show_welcome);

    os_set_env("PUPPETXR_SHOW_WELCOME", "1");
    sim_apply_env(config);
    EXPECT_TRUE(config.show_welcome);
    os_set_env("PUPPETXR_SHOW_WELCOME", "0");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_SHOW_WELCOME");
    EXPECT_FALSE(config.show_welcome);
}

TEST(Sim, config_rejects_bad_json_and_unknown_keys_gracefully)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_FALSE(sim_parse_config(config, "{ not json"));
    EXPECT_TRUE(sim_parse_config(config, R"({ "clock": { "mode": "bogus" }, "controls": { "bindings": [ { "key": "nokey", "input": "trigger/value" } ] } })"));
    EXPECT_EQ(config.clock_mode, SIM_ClockMode::Realtime);
    EXPECT_GT(config.bindings_count, 0u);
}

TEST(Sim, fixed_step_clock_advances_by_period)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    const i64 t0     = sim_time(sim);
    const i64 period = sim_display_period(sim);
    EXPECT_GT(t0, 0);
    EXPECT_EQ(sim_frame_index(sim), 0u);

    step(sim, 123456789);
    EXPECT_EQ(sim_time(sim), t0 + period);
    EXPECT_EQ(sim_frame_index(sim), 1u);
    EXPECT_EQ(sim_predicted_display_time(sim), t0 + period + period);

    step(sim, 999999999);
    EXPECT_EQ(sim_time(sim), t0 + 2 * period);
}

TEST(Sim, realtime_clock_follows_os_time)
{
    SIM_Sim* sim = make_sim(SIM_ClockMode::Realtime);
    defer { sim_destroy(sim); };

    const i64 t0 = sim_time(sim);
    step(sim, 1000000000);
    step(sim, 1005000000);
    EXPECT_EQ(sim_time(sim), t0 + 5000000);
    step(sim, 1004000000);
    EXPECT_EQ(sim_time(sim), t0 + 5000000);

    sim_set_clock_mode(sim, SIM_ClockMode::FixedStep);
    step(sim, 2000000000);
    EXPECT_EQ(sim_time(sim), t0 + 5000000 + sim_display_period(sim));
}

// The first frame counts from the clock's start (xrCreateInstance), where time conversions anchor
TEST(Sim, realtime_clock_counts_the_first_frame_from_its_start)
{
    SIM_Sim* sim = make_sim(SIM_ClockMode::Realtime);
    defer { sim_destroy(sim); };

    const i64 t0 = sim_time(sim);
    sim_start_clock(sim, 1000000000);
    step(sim, 1050000000);
    EXPECT_EQ(sim_time(sim), t0 + 50000000);
    sim_start_clock(sim, 2000000000); // only the first start counts
    step(sim, 1060000000);
    EXPECT_EQ(sim_time(sim), t0 + 60000000);
}

TEST(Sim, device_poses_and_velocities)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    const Pose head = sim_get_device_pose(sim, SIM_Device::Head);
    EXPECT_GT(head.position.y, 1.0f);

    sim_set_device_pose(sim, SIM_Device::LeftHand, { .position = { 1.0f, 1.0f, 1.0f }, .orientation = zm::QUAT_IDENTITY });
    step(sim);
    sim_set_device_pose(sim, SIM_Device::LeftHand, { .position = { 2.0f, 1.0f, 1.0f }, .orientation = zm::QUAT_IDENTITY });
    step(sim);

    const SIM_DeviceState state = sim_get_device_state(sim, SIM_Device::LeftHand);
    const f32             dt    = (f32)sim_display_period(sim) * 1e-9f;
    EXPECT_NEAR(state.linear_velocity.x, 1.0f / dt, 1e-2f);
    EXPECT_NEAR(state.linear_velocity.y, 0.0f, 1e-4f);
    EXPECT_TRUE(state.active);
    EXPECT_TRUE(state.tracked);

    sim_set_device_active(sim, SIM_Device::LeftHand, false);
    EXPECT_FALSE(sim_get_device_state(sim, SIM_Device::LeftHand).active);
}

TEST(Sim, hand_poses_derive_from_grip)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    const Pose grip = { .position = { 0.3f, 1.2f, -0.4f }, .orientation = zm::QUAT_IDENTITY };
    sim_set_device_pose(sim, SIM_Device::RightHand, grip);
    EXPECT_TRUE(zm::equal(sim_get_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Grip), grip));
    const Pose aim = sim_get_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Aim);
    EXPECT_FALSE(zm::equal(aim, grip));
    EXPECT_TRUE(zm::is_finite(aim));
}

// Meta's samples aim along OVRPlugin's controller pose, the grip turned 60 degrees down, as its UI
// pointer does: the aim pose is that, so what a person aims at is what those samples hit
TEST(Sim, default_hands_aim_level_and_forward)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    for (const SIM_Device hand : { SIM_Device::LeftHand, SIM_Device::RightHand })
    {
        const Pose aim     = sim_get_hand_pose(sim, hand, SIM_HandPose::Aim);
        const Vec3 forward = zm::rotate(aim.orientation, { 0.0f, 0.0f, -1.0f });
        EXPECT_NEAR(forward.x, 0.0f, 1e-5f);
        EXPECT_NEAR(forward.y, 0.0f, 1e-5f);
        EXPECT_NEAR(forward.z, -1.0f, 1e-5f);

        const Vec3 handle = zm::rotate(sim_get_device_pose(sim, hand).orientation, { 0.0f, 0.0f, -1.0f });
        EXPECT_NEAR(zm::asin(handle.y), 60.0f * zm::DEG_TO_RAD, 1e-4f);
        const Pose grip = sim_get_device_pose(sim, hand);
        EXPECT_NEAR(zm::dot(zm::rotate(grip.orientation, { 0.0f, 0.0f, -1.0f }), forward), zm::cos(60.0f * zm::DEG_TO_RAD), 1e-4f);
    }
}

// Tests pick a UI element from a screenshot; aiming from the eye hits it whatever its distance
TEST(Sim, pointing_aims_from_the_eye_through_an_image_point)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    sim_set_device_pose(sim, SIM_Device::Head, { .position = { 0.1f, 1.5f, 0.2f }, .orientation = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.3f) });

    Pose eye = {};
    Fov  fov = {};
    sim_get_view(sim, 1, eye, fov);
    sim_point_hand(sim, SIM_Device::LeftHand, 1, 0.75f, 0.25f, sim_get_config(sim).aim_offset);
    const Pose aim      = sim_get_hand_pose(sim, SIM_Device::LeftHand, SIM_HandPose::Aim);
    const Vec3 local    = zm::rotate(zm::conjugate(eye.orientation), zm::rotate(aim.orientation, { 0.0f, 0.0f, -1.0f }));
    const f32  tan_half = zm::tan(fov.angle_right);
    EXPECT_NEAR(zm::length(aim.position - eye.position), 0.0f, 1e-5f);
    EXPECT_NEAR(local.x / -local.z, 0.5f * tan_half, 1e-5f);
    EXPECT_NEAR(local.y / -local.z, 0.5f * tan_half, 1e-5f);
    EXPECT_LT(local.z, 0.0f);

    // Engines that derive their pointer from the grip, like Meta's OVRPlugin, need another ray
    const Pose ray = { .position = { 0.0f, 0.0f, 0.0f }, .orientation = zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, -60.0f * zm::DEG_TO_RAD) };
    sim_point_hand(sim, SIM_Device::RightHand, 0, 0.5f, 0.5f, ray);
    sim_get_view(sim, 0, eye, fov);
    const Pose grip  = sim_get_device_pose(sim, SIM_Device::RightHand);
    const Vec3 ahead = zm::rotate(zm::pose_mul(grip, ray).orientation, { 0.0f, 0.0f, -1.0f }) - zm::rotate(eye.orientation, { 0.0f, 0.0f, -1.0f });
    EXPECT_NEAR(zm::length(grip.position - eye.position), 0.0f, 1e-5f);
    EXPECT_NEAR(zm::length(ahead), 0.0f, 1e-5f);
}

TEST(Sim, inputs_by_enum_and_name)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    SIM_Input input = SIM_Input::Count;
    EXPECT_TRUE(sim_input_from_name("thumbstick", input));
    EXPECT_EQ(input, SIM_Input::Thumbstick);
    EXPECT_EQ(sim_input_type(SIM_Input::Thumbstick), SIM_InputType::Vec2);
    EXPECT_EQ(sim_input_type(SIM_Input::TriggerValue), SIM_InputType::Float);
    EXPECT_EQ(sim_input_type(SIM_Input::AClick), SIM_InputType::Bool);
    EXPECT_TRUE(cstr_equal(sim_input_name(SIM_Input::AClick), "a/click"));
    EXPECT_FALSE(sim_input_from_name("nope", input));

    for (u32 i = 0; i < (u32)SIM_Input::Count; i++)
    {
        SIM_Input round_trip = SIM_Input::Count;
        EXPECT_TRUE(sim_input_from_name(sim_input_name((SIM_Input)i), round_trip));
        EXPECT_EQ((u32)round_trip, i);
    }

    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue, { .value = { 0.75f, 0.0f } });
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue).value.x, 0.75f);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue).value.x, 0.0f);

    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick, { .value = { -0.5f, 0.25f } });
    const SIM_InputValue stick = sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick);
    EXPECT_FLOAT_EQ(stick.value.x, -0.5f);
    EXPECT_FLOAT_EQ(stick.value.y, 0.25f);

    sim_set_input(sim, SIM_Device::Head, SIM_Input::TriggerValue, { .value = { 1.0f, 0.0f } });
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::Head, SIM_Input::TriggerValue).value.x, 0.0f);
}

TEST(Sim, input_values_are_clamped_to_their_type)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue, { .value = { 7.0f, 3.0f } });
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue).value.x, 1.0f);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue).value.y, 0.0f);

    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::XClick, { .value = { 0.3f, 0.0f } });
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::XClick).value.x, 1.0f);

    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick, { .value = { -9.0f, 9.0f } });
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.x, -1.0f);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y, 1.0f);
}

TEST(Sim, the_stationary_origin_ignores_recentering)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    const Pose first = sim_get_reference_space_pose(sim, SIM_ReferenceSpace::Local);
    EXPECT_TRUE(zm::equal(sim_get_reference_space_pose(sim, SIM_ReferenceSpace::Stationary), first));

    sim_set_device_pose(sim, SIM_Device::Head, { .position = { 2.0f, 1.5f, -1.0f }, .orientation = zm::quat_from_euler({ 0.0f, 1.0f, 0.0f }) });
    sim_recenter(sim);
    EXPECT_FALSE(zm::equal(sim_get_reference_space_pose(sim, SIM_ReferenceSpace::Local), first));
    EXPECT_TRUE(zm::equal(sim_get_reference_space_pose(sim, SIM_ReferenceSpace::Stationary), first));

    SIM_Sample sample = {};
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    EXPECT_TRUE(zm::equal(sim_sample_reference_space_pose(sample, SIM_ReferenceSpace::Stationary), first));
}

TEST(Sim, reference_spaces)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    const Pose stage = sim_get_reference_space_pose(sim, SIM_ReferenceSpace::Stage);
    EXPECT_TRUE(zm::equal(stage, zm::POSE_IDENTITY));

    const Pose head  = sim_get_device_pose(sim, SIM_Device::Head);
    const Pose view  = sim_get_reference_space_pose(sim, SIM_ReferenceSpace::View);
    EXPECT_TRUE(zm::equal(view, head));

    const Pose local = sim_get_reference_space_pose(sim, SIM_ReferenceSpace::Local);
    EXPECT_FLOAT_EQ(local.position.y, head.position.y);
    const Pose local_floor = sim_get_reference_space_pose(sim, SIM_ReferenceSpace::LocalFloor);
    EXPECT_FLOAT_EQ(local_floor.position.y, 0.0f);
    EXPECT_FLOAT_EQ(local_floor.position.x, local.position.x);

    const Quat turned = zm::quat_from_euler({ 0.3f, 1.0f, 0.0f });
    sim_set_device_pose(sim, SIM_Device::Head, { .position = { 2.0f, 1.5f, -1.0f }, .orientation = turned });
    sim_recenter(sim);
    const Pose recentered = sim_get_reference_space_pose(sim, SIM_ReferenceSpace::Local);
    EXPECT_FLOAT_EQ(recentered.position.x, 2.0f);
    EXPECT_FLOAT_EQ(recentered.position.y, 1.5f);
    const Vec3 euler = zm::euler_from_quat(recentered.orientation);
    EXPECT_NEAR(euler.x, 0.0f, 1e-5f);
    EXPECT_NEAR(euler.y, 1.0f, 1e-5f);
    EXPECT_NEAR(euler.z, 0.0f, 1e-5f);

    const Vec2 bounds = sim_get_stage_bounds(sim);
    EXPECT_GT(bounds.x, 0.0f);
    EXPECT_GT(bounds.y, 0.0f);
}

TEST(Sim, views_are_offset_by_ipd)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    Pose left;
    Pose right;
    Fov  fov_left;
    Fov  fov_right;
    sim_get_view(sim, 0, left, fov_left);
    sim_get_view(sim, 1, right, fov_right);

    const f32 ipd = zm::length(right.position - left.position);
    EXPECT_NEAR(ipd, sim_get_config(sim).ipd, 1e-5f);
    EXPECT_LT(fov_left.angle_left, 0.0f);
    EXPECT_GT(fov_left.angle_right, 0.0f);
    EXPECT_GT(fov_left.angle_up, 0.0f);
    EXPECT_LT(fov_left.angle_down, 0.0f);
    EXPECT_EQ(sim_view_count(sim), 2u);
}

static void tap (SIM_Sim* sim, OS_Key key)
{
    sim_control_key(sim, key, true);
    sim_control_key(sim, key, false);
}

static void hold_for_frame (SIM_Sim* sim, OS_Key key)
{
    sim_control_key(sim, key, true);
    step(sim);
    sim_control_key(sim, key, false);
}

static void use_profile (SIM_Sim* sim, const char* name)
{
    u32 index = 0;
    ASSERT_TRUE(sim_profile_from_name(sim, name, index)) << name;
    sim_set_profile(sim, index);
}

static const char* profile_name (const SIM_Sim* sim)
{
    return sim_get_config(sim).profiles[sim_get_profile(sim)].name;
}

static f32 input_x (const SIM_Sim* sim, SIM_Device hand, SIM_Input input)
{
    return sim_get_input(sim, hand, input).value.x;
}

TEST(Sim, body_mode_moves_head_and_hands_together)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);

    EXPECT_STREQ(profile_name(sim), "body");
    Pose before[(u32)SIM_Device::Count] = {};
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        before[i] = sim_get_device_pose(sim, (SIM_Device)i);
    }

    hold_for_frame(sim, config.key_move_forward);

    const f32 dt = (f32)sim_display_period(sim) * 1e-9f;
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        const Pose after = sim_get_device_pose(sim, (SIM_Device)i);
        EXPECT_NEAR(after.position.z, before[i].position.z - config.move_speed * dt, 1e-5f) << i;
        EXPECT_FLOAT_EQ(after.position.x, before[i].position.x) << i;
        EXPECT_FLOAT_EQ(after.position.y, before[i].position.y) << i;
        EXPECT_TRUE(zm::equal(after.orientation, before[i].orientation)) << i;
    }
}

TEST(Sim, body_mode_turns_the_hands_around_the_head)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);

    const Pose head_before  = sim_get_device_pose(sim, SIM_Device::Head);
    const Pose left_before  = zm::pose_relative(head_before, sim_get_device_pose(sim, SIM_Device::LeftHand));
    const Pose right_before = zm::pose_relative(head_before, sim_get_device_pose(sim, SIM_Device::RightHand));

    sim_control_mouse_motion(sim, 100.0f, 0.0f);
    step(sim);
    hold_for_frame(sim, config.key_turn_left);

    const Pose head = sim_get_device_pose(sim, SIM_Device::Head);
    const f32  dt   = (f32)sim_display_period(sim) * 1e-9f;
    EXPECT_NEAR(zm::euler_from_quat(head.orientation).y, -100.0f * config.look_sensitivity + config.turn_speed * dt, 1e-4f);
    EXPECT_TRUE(zm::near(zm::pose_relative(head, sim_get_device_pose(sim, SIM_Device::LeftHand)), left_before, 1e-5f));
    EXPECT_TRUE(zm::near(zm::pose_relative(head, sim_get_device_pose(sim, SIM_Device::RightHand)), right_before, 1e-5f));
    EXPECT_TRUE(zm::near(head.position, head_before.position, 1e-6f));
}

// Looking up or down with the whole body carries the controllers along, as held in front of it
TEST(Sim, body_mode_pitch_turns_the_hands_with_the_head)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);

    const Pose head_before  = sim_get_device_pose(sim, SIM_Device::Head);
    const Pose left_before  = zm::pose_relative(head_before, sim_get_device_pose(sim, SIM_Device::LeftHand));
    const Pose right_before = zm::pose_relative(head_before, sim_get_device_pose(sim, SIM_Device::RightHand));
    hold_for_frame(sim, config.key_turn_up);

    const f32  dt   = (f32)sim_display_period(sim) * 1e-9f;
    const Pose head = sim_get_device_pose(sim, SIM_Device::Head);
    EXPECT_NEAR(zm::euler_from_quat(head.orientation).x, config.turn_speed * dt, 1e-4f);
    EXPECT_TRUE(zm::near(zm::pose_relative(head, sim_get_device_pose(sim, SIM_Device::LeftHand)), left_before, 1e-5f));
    EXPECT_TRUE(zm::near(zm::pose_relative(head, sim_get_device_pose(sim, SIM_Device::RightHand)), right_before, 1e-5f));
    EXPECT_TRUE(zm::near(head.position, head_before.position, 1e-6f));

    // Past the head's pitch limit nothing turns further, the hands neither
    for (u32 i = 0; i < 400; i++)
    {
        hold_for_frame(sim, config.key_turn_up);
    }
    const Pose limit = sim_get_device_pose(sim, SIM_Device::Head);
    EXPECT_TRUE(zm::near(zm::pose_relative(limit, sim_get_device_pose(sim, SIM_Device::LeftHand)), left_before, 1e-4f));
}

TEST(Sim, head_and_hand_modes_move_one_device)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);

    use_profile(sim, "head");
    const Pose left_before = sim_get_device_pose(sim, SIM_Device::LeftHand);
    const Pose head_before = sim_get_device_pose(sim, SIM_Device::Head);
    hold_for_frame(sim, config.key_move_forward);
    EXPECT_LT(sim_get_device_pose(sim, SIM_Device::Head).position.z, head_before.position.z);
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::LeftHand), left_before));

    use_profile(sim, "left");
    const Pose head_after = sim_get_device_pose(sim, SIM_Device::Head);
    const Pose right      = sim_get_device_pose(sim, SIM_Device::RightHand);
    hold_for_frame(sim, config.key_move_forward);
    hold_for_frame(sim, config.key_turn_left);
    EXPECT_GT(sim_get_device_pose(sim, SIM_Device::LeftHand).position.y, left_before.position.y) << "a hand moves in the view's plane: forward is up";
    EXPECT_FALSE(zm::equal(sim_get_device_pose(sim, SIM_Device::LeftHand).orientation, left_before.orientation));
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head), head_after));
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::RightHand), right));

    tap(sim, OS_Key::Digit1);
    EXPECT_STREQ(profile_name(sim), "body");

    sim_set_profile(sim, 99);
    EXPECT_STREQ(profile_name(sim), "body");
}

// The number keys pick the profiles with a key; ] goes to the next of them, [ back, and both skip
// the ones without a key
TEST(Sim, profile_keys_pick_and_brackets_cycle)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    tap(sim, OS_Key::Digit3);
    EXPECT_STREQ(profile_name(sim), "hands");
    tap(sim, OS_Key::RightBracket);
    EXPECT_STREQ(profile_name(sim), "eyes") << "head, left and right have no key";
    tap(sim, OS_Key::RightBracket);
    EXPECT_STREQ(profile_name(sim), "body");
    tap(sim, OS_Key::RightBracket);
    EXPECT_STREQ(profile_name(sim), "stick");
    tap(sim, OS_Key::LeftBracket);
    tap(sim, OS_Key::LeftBracket);
    EXPECT_STREQ(profile_name(sim), "eyes");
    tap(sim, OS_Key::LeftBracket);
    EXPECT_STREQ(profile_name(sim), "hands");
    tap(sim, OS_Key::Tab);
    EXPECT_STREQ(profile_name(sim), "hands") << "Tab shows the panels";
    use_profile(sim, "head");
    tap(sim, OS_Key::RightBracket);
    EXPECT_STREQ(profile_name(sim), "eyes") << "from one without a key to the next with one";
}

TEST(Sim, stick_profile_drives_the_thumbsticks)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);

    tap(sim, OS_Key::Digit2);
    const Pose head = sim_get_device_pose(sim, SIM_Device::Head);
    sim_control_key(sim, config.key_move_forward, true);
    sim_control_key(sim, config.key_turn_left, true);
    step(sim);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y, 1.0f);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::Thumbstick).value.x, -1.0f);
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head), head)) << "the app moves the player";
    sim_control_key(sim, config.key_move_forward, false);
    sim_control_key(sim, config.key_turn_left, false);
    step(sim);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y, 0.0f);

    sim_control_mouse_motion(sim, 100.0f, 0.0f);
    step(sim);
    EXPECT_NEAR(zm::euler_from_quat(sim_get_device_pose(sim, SIM_Device::Head).orientation).y, -100.0f * config.look_sensitivity, 1e-4f) << "the mouse still looks around";
}

// A panel on the left hand and a pointer on the right: each hand moves in the view's plane, the
// mouse turns the right, Shift+mouse the left, the wheel pushes the right and Shift+wheel rolls the
// left. The head stays
TEST(Sim, hands_profile_moves_and_turns_each_hand)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);
    const f32         dt     = (f32)sim_display_period(sim) * 1e-9f;

    tap(sim, OS_Key::Digit3);
    const Pose head  = sim_get_device_pose(sim, SIM_Device::Head);
    const Pose left  = sim_get_device_pose(sim, SIM_Device::LeftHand);
    const Pose right = sim_get_device_pose(sim, SIM_Device::RightHand);
    hold_for_frame(sim, config.key_move_forward);
    EXPECT_NEAR(sim_get_device_pose(sim, SIM_Device::LeftHand).position.y, left.position.y + config.hand_speed * dt, 1e-5f);
    hold_for_frame(sim, config.key_move_up);
    EXPECT_NEAR(sim_get_device_pose(sim, SIM_Device::LeftHand).position.z, left.position.z - config.hand_speed * dt, 1e-5f) << "E pushes it away";
    hold_for_frame(sim, config.key_turn_right);
    EXPECT_NEAR(sim_get_device_pose(sim, SIM_Device::RightHand).position.x, right.position.x + config.hand_speed * dt, 1e-5f);
    hold_for_frame(sim, OS_Key::ArrowUp);
    EXPECT_NEAR(sim_get_device_pose(sim, SIM_Device::RightHand).position.y, right.position.y + config.hand_speed * dt, 1e-5f) << "the arrows are the turn keys";
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head), head));

    const Pose moved_left  = sim_get_device_pose(sim, SIM_Device::LeftHand);
    const Pose moved_right = sim_get_device_pose(sim, SIM_Device::RightHand);
    sim_control_mouse_motion(sim, 100.0f, 0.0f);
    step(sim);
    EXPECT_FALSE(zm::near(sim_get_device_pose(sim, SIM_Device::RightHand).orientation, moved_right.orientation, 1e-4f));
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::LeftHand), moved_left));
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head), head));

    const Pose turned_right = sim_get_device_pose(sim, SIM_Device::RightHand);
    sim_control_key(sim, OS_Key::LeftShift, true);
    sim_control_mouse_motion(sim, 100.0f, 0.0f);
    step(sim);
    EXPECT_FALSE(zm::near(sim_get_device_pose(sim, SIM_Device::LeftHand).orientation, moved_left.orientation, 1e-4f));
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::RightHand), turned_right));

    const Pose turned_left = sim_get_device_pose(sim, SIM_Device::LeftHand);
    sim_control_wheel(sim, 1.0f);
    step(sim);
    EXPECT_TRUE(zm::near(sim_get_device_pose(sim, SIM_Device::LeftHand).position, turned_left.position, 1e-6f)) << "Shift+wheel rolls in place";
    EXPECT_FALSE(zm::near(sim_get_device_pose(sim, SIM_Device::LeftHand).orientation, turned_left.orientation, 1e-4f));
    sim_control_key(sim, OS_Key::LeftShift, false);

    sim_control_wheel(sim, 2.0f);
    step(sim);
    EXPECT_NEAR(sim_get_device_pose(sim, SIM_Device::RightHand).position.z, turned_right.position.z - 0.04f, 1e-5f) << "two notches push the right hand 4 cm away";
}

TEST(Sim, arrows_are_the_turn_keys_with_modifiers_too)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);
    const f32         dt     = (f32)sim_display_period(sim) * 1e-9f;

    hold_for_frame(sim, OS_Key::ArrowLeft);
    EXPECT_NEAR(zm::euler_from_quat(sim_get_device_pose(sim, SIM_Device::Head).orientation).y, config.turn_speed * dt, 1e-4f);
    sim_control_key(sim, OS_Key::LeftShift, true);
    sim_control_key(sim, OS_Key::ArrowUp, true);
    step(sim);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y, 1.0f) << "Shift+arrow up is Shift+I";
}

TEST(Sim, config_profiles_add_extend_and_change)
{
    SIM_Config config = {};
    sim_default_config(config);
    const u32 built_in = config.profiles_count;
    EXPECT_TRUE(sim_parse_config(config, R"({ "controls": { "profiles": [
        { "name": "panel", "extends": "hands", "key": "4", "wheel": "push_left" },
        { "name": "copy", "extends": "body" },
        { "name": "body", "mouse": "turn_head" },
        { "name": "bad", "wasd": "fly", "hand": "third" },
        { "extends": "body" }
    ] } })"));
    ASSERT_EQ(config.profiles_count, built_in + 3);
    const SIM_Profile& panel = config.profiles[built_in];
    EXPECT_STREQ(panel.name, "panel");
    EXPECT_EQ(panel.key, OS_Key::Digit4);
    EXPECT_EQ(panel.wasd, SIM_Drive::MoveLeft);
    EXPECT_EQ(panel.wheel, SIM_Drive::PushLeft);
    EXPECT_EQ(panel.hand, SIM_BindHand::Right);
    EXPECT_EQ(config.profiles[built_in + 1].key, OS_Key::None) << "an extended profile's key stays its own";
    EXPECT_EQ(config.profiles[built_in + 1].wasd, SIM_Drive::Walk);
    EXPECT_EQ(config.profiles[0].mouse, SIM_Drive::TurnHead);
    EXPECT_EQ(config.profiles[0].key, OS_Key::Digit1);
    EXPECT_EQ(config.profiles[built_in + 2].wasd, SIM_Drive::None) << "unknown drives are ignored";
    EXPECT_EQ(config.profiles[built_in + 2].hand, SIM_BindHand::Active);
}

// The mirror window sends moves only while it holds the mouse, so every move turns
TEST(Sim, mouse_moves_turn_without_a_key)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    const Pose before = sim_get_device_pose(sim, SIM_Device::Head);
    sim_control_mouse_motion(sim, 100.0f, 0.0f);
    step(sim);
    EXPECT_NEAR(zm::euler_from_quat(sim_get_device_pose(sim, SIM_Device::Head).orientation).y, zm::euler_from_quat(before.orientation).y - 100.0f * sim_get_config(sim).look_sensitivity, 1e-4f);
}

TEST(Sim, mouse_buttons_are_trigger_grip_and_primary)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };

    sim_control_key(sim, OS_Key::MouseLeft, true);
    sim_control_key(sim, OS_Key::MouseRight, true);
    sim_control_key(sim, OS_Key::MouseMiddle, true);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::SqueezeValue), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::AClick), 1.0f);
}

TEST(Sim, modifiers_pick_the_hand_of_a_slot)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };

    sim_control_key(sim, OS_Key::U, true);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue), 0.0f);

    sim_control_key(sim, OS_Key::LeftShift, true);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 0.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue), 1.0f);
    sim_control_key(sim, OS_Key::LeftShift, false);

    sim_control_key(sim, OS_Key::RightCtrl, true);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue), 0.0f);

    sim_control_key(sim, OS_Key::U, false);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 0.0f);
    sim_control_key(sim, OS_Key::RightCtrl, false);

    sim_control_key(sim, OS_Key::LeftShift, true);
    sim_control_key(sim, OS_Key::LeftCtrl, true);
    hold_for_frame(sim, OS_Key::U);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 0.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue), 0.0f);
}

TEST(Sim, bare_slots_drive_the_active_hand)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };

    use_profile(sim, "left");
    sim_control_key(sim, OS_Key::MouseLeft, true);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 0.0f);

    use_profile(sim, "head");
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue), 0.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 1.0f);
    sim_control_key(sim, OS_Key::MouseLeft, false);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 0.0f);
}

TEST(Sim, primary_hand_from_config)
{
    SIM_Config config = {};
    sim_default_config(config);
    config.clock_mode   = SIM_ClockMode::FixedStep;
    config.analog_ramp  = 0.0f;
    config.primary_hand = SIM_Device::LeftHand;
    SIM_Sim* sim = sim_create(config);
    defer { sim_destroy(sim); };

    hold_for_frame(sim, OS_Key::O);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::SqueezeValue), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::SqueezeValue), 0.0f);
}

TEST(Sim, slots_map_to_each_hands_buttons)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::OculusTouch, SIM_Device::LeftHand, SIM_Slot::Primary), SIM_Input::XClick);
    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::OculusTouch, SIM_Device::RightHand, SIM_Slot::Primary), SIM_Input::AClick);
    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::OculusTouch, SIM_Device::LeftHand, SIM_Slot::Secondary), SIM_Input::YClick);
    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::OculusTouch, SIM_Device::RightHand, SIM_Slot::Secondary), SIM_Input::BClick);
    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::OculusTouch, SIM_Device::LeftHand, SIM_Slot::System), SIM_Input::Count);
    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::OculusTouch, SIM_Device::RightHand, SIM_Slot::Menu), SIM_Input::Count);
    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::KhrSimple, SIM_Device::LeftHand, SIM_Slot::Trigger), SIM_Input::SelectClick);
    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::KhrSimple, SIM_Device::LeftHand, SIM_Slot::Stick), SIM_Input::Count);
    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::OculusTouch, SIM_Device::Head, SIM_Slot::Trigger), SIM_Input::Count);
    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::OculusTouch, SIM_Device::LeftHand, (SIM_Slot)99), SIM_Input::Count);
    EXPECT_EQ(sim_controller_slot_input((SIM_Controller)99, SIM_Device::LeftHand, SIM_Slot::Trigger), SIM_Input::Count);

    // Regression: the left Touch got a/click from a key bound to both A and X
    sim_control_key(sim, OS_Key::LeftShift, true);
    hold_for_frame(sim, OS_Key::H);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::XClick), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::AClick), 0.0f);
    sim_control_key(sim, OS_Key::LeftShift, false);
    step(sim);

    // The right Touch has no menu button, so the menu key presses the left one's: apps open their
    // menus from it (Meta's samples go back to their start scene)
    sim_control_key(sim, OS_Key::M, true);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::SystemClick), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::MenuClick), 0.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::MenuClick), 1.0f);
    sim_control_key(sim, OS_Key::LeftShift, true);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::MenuClick), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::SystemClick), 0.0f);
}

TEST(Sim, modified_turn_keys_drive_thumbsticks_instead_of_turning)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };

    const Pose head = sim_get_device_pose(sim, SIM_Device::Head);
    sim_control_key(sim, OS_Key::LeftShift, true);
    sim_control_key(sim, OS_Key::I, true);
    step(sim);
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head), head));
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y, 1.0f);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::Thumbstick).value.y, 0.0f);

    // Regression: two keys of one stick summed to a square corner of length 1.41
    sim_control_key(sim, OS_Key::L, true);
    step(sim);
    const Vec2 stick = sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value;
    EXPECT_NEAR(zm::length(stick), 1.0f, 1e-5f);
    EXPECT_NEAR(stick.x, stick.y, 1e-6f);
    EXPECT_GT(stick.x, 0.0f);
    sim_control_key(sim, OS_Key::LeftShift, false);
    sim_control_key(sim, OS_Key::I, false);
    sim_control_key(sim, OS_Key::L, false);

    sim_control_key(sim, OS_Key::LeftCtrl, true);
    hold_for_frame(sim, OS_Key::J);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::Thumbstick).value.x, -1.0f);
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head), head));
    sim_control_key(sim, OS_Key::LeftCtrl, false);
    step(sim);
    EXPECT_TRUE(zm::equal(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::Thumbstick).value, Vec2{}));
}

TEST(Sim, keys_touch_what_they_press)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };

    use_profile(sim, "stick");
    sim_control_key(sim, OS_Key::U, true);
    sim_control_key(sim, OS_Key::H, true);
    sim_control_key(sim, OS_Key::ArrowUp, true);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerTouch), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::ATouch), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::ThumbstickTouch), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::BTouch), 0.0f);
    sim_control_key(sim, OS_Key::U, false);
    sim_control_key(sim, OS_Key::H, false);
    sim_control_key(sim, OS_Key::ArrowUp, false);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerTouch), 0.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::ATouch), 0.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::ThumbstickTouch), 0.0f);

    SIM_Config config = {};
    sim_default_config(config);
    config.clock_mode  = SIM_ClockMode::FixedStep;
    config.auto_touch  = false;
    config.analog_ramp = 0.0f;
    SIM_Sim* manual = sim_create(config);
    defer { sim_destroy(manual); };
    hold_for_frame(manual, OS_Key::U);
    EXPECT_FLOAT_EQ(input_x(manual, SIM_Device::RightHand, SIM_Input::TriggerValue), 1.0f);
    EXPECT_FLOAT_EQ(input_x(manual, SIM_Device::RightHand, SIM_Input::TriggerTouch), 0.0f);
}

TEST(Sim, inputs_set_through_the_api_survive_unpressed_keys)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };

    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::BClick, { .value = { 1.0f, 0.0f } });
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue, { .value = { 0.5f, 0.0f } });
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::BClick), 1.0f);

    hold_for_frame(sim, OS_Key::U);
    step(sim);
    use_profile(sim, "left");
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 0.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::BClick), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue), 0.5f);
}

TEST(Sim, reset_puts_the_hands_back_in_front_of_the_head)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);

    const Pose head  = sim_get_device_pose(sim, SIM_Device::Head);
    const Pose left  = zm::pose_relative(head, sim_get_device_pose(sim, SIM_Device::LeftHand));
    const Pose right = zm::pose_relative(head, sim_get_device_pose(sim, SIM_Device::RightHand));

    use_profile(sim, "left");
    hold_for_frame(sim, config.key_move_up);
    hold_for_frame(sim, config.key_turn_left);
    use_profile(sim, "body");
    for (u32 i = 0; i < 10; i++)
    {
        hold_for_frame(sim, config.key_turn_right);
        hold_for_frame(sim, config.key_move_forward);
    }
    use_profile(sim, "head");
    hold_for_frame(sim, config.key_turn_up);

    hold_for_frame(sim, config.key_reset_hands);
    const Pose head_now = sim_get_device_pose(sim, SIM_Device::Head);
    const Pose level    = { .position = head_now.position, .orientation = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, zm::euler_from_quat(head_now.orientation).y) };
    EXPECT_TRUE(zm::near(zm::pose_relative(level, sim_get_device_pose(sim, SIM_Device::LeftHand)), left, 1e-5f));
    EXPECT_TRUE(zm::near(zm::pose_relative(level, sim_get_device_pose(sim, SIM_Device::RightHand)), right, 1e-5f));
}

TEST(Sim, reset_returns_head_and_hands_to_the_start_without_velocity)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);

    Pose start[(u32)SIM_Device::Count] = {};
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        start[i] = sim_get_device_pose(sim, (SIM_Device)i);
    }
    for (u32 i = 0; i < 10; i++)
    {
        hold_for_frame(sim, config.key_turn_left);
        hold_for_frame(sim, config.key_move_forward);
    }
    use_profile(sim, "right");
    hold_for_frame(sim, config.key_move_up);

    tap(sim, config.key_reset);
    step(sim);
    for (u32 i = 0; i < (u32)SIM_Device::Count; i++)
    {
        const SIM_DeviceState state = sim_get_device_state(sim, (SIM_Device)i);
        EXPECT_TRUE(zm::equal(state.pose, start[i])) << i;
        EXPECT_TRUE(zm::equal(state.linear_velocity, Vec3{})) << i;
        EXPECT_TRUE(zm::equal(state.angular_velocity, Vec3{})) << i;
    }
    EXPECT_STREQ(profile_name(sim), "right");

    hold_for_frame(sim, config.key_move_up);
    sim_reset_hands(sim);
    step(sim);
    EXPECT_TRUE(zm::equal(sim_get_device_state(sim, SIM_Device::RightHand).linear_velocity, Vec3{}));
}

TEST(Sim, analog_inputs_ramp_up_and_release)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);
    const f32         dt     = (f32)sim_display_period(sim) * 1e-9f;

    sim_control_key(sim, OS_Key::U, true);
    sim_control_key(sim, OS_Key::H, true);
    step(sim);
    EXPECT_NEAR(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), dt / config.analog_ramp, 1e-6f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::AClick), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerTouch), 1.0f);
    step(sim);
    EXPECT_NEAR(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 2.0f * dt / config.analog_ramp, 1e-6f);

    const u32 frames = (u32)(config.analog_ramp / dt) + 1;
    for (u32 i = 0; i < frames; i++)
    {
        step(sim);
    }
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 1.0f);

    sim_control_key(sim, OS_Key::U, false);
    sim_control_key(sim, OS_Key::H, false);
    step(sim);
    EXPECT_NEAR(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 1.0f - dt / config.analog_release, 1e-5f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::AClick), 0.0f);
    for (u32 i = 0; i < (u32)(config.analog_release / dt) + 1; i++)
    {
        step(sim);
    }
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 0.0f);

    // Released keys stop driving the input once it is back at zero
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 0.3f, 0.0f } });
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 0.3f);
}

TEST(Sim, diagonal_stick_ramps_along_the_diagonal)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);
    const f32         dt     = (f32)sim_display_period(sim) * 1e-9f;

    sim_control_key(sim, OS_Key::LeftCtrl, true);
    sim_control_key(sim, OS_Key::I, true);
    sim_control_key(sim, OS_Key::L, true);
    step(sim);
    Vec2 stick = sim_get_input(sim, SIM_Device::RightHand, SIM_Input::Thumbstick).value;
    EXPECT_NEAR(zm::length(stick), dt / config.analog_ramp, 1e-6f);
    EXPECT_NEAR(stick.x, stick.y, 1e-6f);
    for (u32 i = 0; i < (u32)(config.analog_ramp / dt) + 1; i++)
    {
        step(sim);
    }
    stick = sim_get_input(sim, SIM_Device::RightHand, SIM_Input::Thumbstick).value;
    EXPECT_NEAR(zm::length(stick), 1.0f, 1e-5f);
    EXPECT_NEAR(stick.x, stick.y, 1e-6f);
}

TEST(Sim, freeze_holds_a_ramp_until_its_keys_are_released)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);

    sim_control_key(sim, OS_Key::LeftShift, true);
    sim_control_key(sim, OS_Key::I, true);
    for (u32 i = 0; i < 10; i++)
    {
        step(sim);
    }
    tap(sim, config.key_freeze);
    step(sim);
    const f32 frozen = sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y;
    EXPECT_GT(frozen, 0.1f);
    EXPECT_LT(frozen, 0.5f);
    for (u32 i = 0; i < 60; i++)
    {
        step(sim);
    }
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y, frozen);

    tap(sim, config.key_freeze);
    step(sim);
    EXPECT_GT(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y, frozen);

    tap(sim, config.key_freeze);
    sim_control_key(sim, OS_Key::I, false);
    for (u32 i = 0; i < 30; i++)
    {
        step(sim);
    }
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y, 0.0f);

    sim_control_key(sim, OS_Key::I, true);
    step(sim);
    step(sim);
    const f32 restarted = sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y;
    step(sim);
    EXPECT_GT(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::Thumbstick).value.y, restarted);
}

TEST(Sim, scripted_keys_are_deterministic)
{
    u64 hashes[2] = {};
    for (u64& h : hashes)
    {
        SIM_Sim* sim = make_sim();
        defer { sim_destroy(sim); };
        const SIM_Config& config = sim_get_config(sim);
        for (u32 i = 0; i < 30; i++)
        {
            sim_control_mouse_motion(sim, 3.0f, -1.5f);
            sim_control_key(sim, i % 3 == 0 ? config.key_move_forward : config.key_turn_left, i % 2 == 0);
            sim_control_key(sim, OS_Key::LeftShift, i % 5 == 0);
            sim_control_key(sim, OS_Key::U, i % 4 < 2);
            sim_control_key(sim, OS_Key::L, i > 10);
            step(sim);
        }
        h = sim_state_hash(sim);
    }
    EXPECT_EQ(hashes[0], hashes[1]);
}

TEST(Sim, controls_can_be_disabled_for_replay)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);

    sim_set_controls_enabled(sim, false);
    const Pose before = sim_get_device_pose(sim, SIM_Device::Head);
    sim_control_key(sim, config.key_move_forward, true);
    step(sim);
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head), before));
}

TEST(Sim, haptics)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    sim_apply_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main, 0.5f, 100000000, 200.0f);
    const SIM_Haptic h = sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main);
    EXPECT_TRUE(h.active);
    EXPECT_FLOAT_EQ(h.amplitude, 0.5f);
    EXPECT_EQ(h.end_time, sim_time(sim) + 100000000);

    for (u32 i = 0; i < 20; i++)
    {
        step(sim);
    }
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);

    sim_apply_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main, 1.0f, -1, 0.0f);
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active);
    sim_stop_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active);
}

// Regression (found by fuzzing): XR_INFINITE_DURATION overflowed the end time, so the vibration ended
// at once (and UBSan reported it); it now lasts until stopped
TEST(Sim, infinite_haptics_last_until_stopped)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };
    step(sim);
    const f32 ramp[2] = { 0.25f, 1.0f };
    sim_apply_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main, 0.5f, INT64_MAX, 0.0f);
    sim_apply_haptic_envelope(sim, SIM_Device::RightHand, SIM_HapticOutput::Main, ramp, 2, INT64_MAX);
    for (u32 i = 0; i < 10; i++)
    {
        step(sim);
    }
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).amplitude, 0.5f);
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active);
    EXPECT_NEAR(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).amplitude, 0.25f, 1e-3f);
}

// XR_FB_haptic_amplitude_envelope: the amplitude follows the samples over the duration
TEST(Sim, haptic_envelopes_follow_sim_time)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };
    const f32 ramp[3]  = { 0.0f, 1.0f, 0.5f };
    const i64 duration = 100000000;
    sim_apply_haptic_envelope(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main, ramp, 3, duration);
    const i64 start = sim_time(sim);
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).amplitude, 0.0f);
    while (sim_time(sim) - start < duration)
    {
        const f32 t        = (f32)(sim_time(sim) - start) / (f32)duration * 2.0f;
        const f32 expected = t < 1.0f ? t : 1.0f - 0.5f * (t - 1.0f);
        EXPECT_NEAR(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).amplitude, expected, 1e-4f) << t;
        step(sim);
    }
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);

    // More samples than the runtime keeps are resampled; a plain vibration replaces an envelope
    static f32 long_ramp[8001];
    for (u32 i = 0; i < 8001; i++)
    {
        long_ramp[i] = (f32)i / 8000.0f;
    }
    sim_apply_haptic_envelope(sim, SIM_Device::RightHand, SIM_HapticOutput::Main, long_ramp, 8001, duration);
    const i64 long_start = sim_time(sim);
    step(sim);
    step(sim);
    const f32 progress = (f32)(sim_time(sim) - long_start) / (f32)duration;
    EXPECT_NEAR(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).amplitude, progress, 1e-3f);
    sim_apply_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main, 0.3f, duration, 0.0f);
    step(sim);
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).amplitude, 0.3f);
    sim_apply_haptic_envelope(sim, SIM_Device::RightHand, SIM_HapticOutput::Main, ramp, 3, duration);
    sim_stop_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::RightHand, SIM_HapticOutput::Main).active);

    os_set_assert_mode(OS_AssertMode::Log);
    const f32 bad[2] = { zm::nan(), 1.0f };
    sim_apply_haptic_envelope(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main, bad, 2, duration);
    sim_apply_haptic_envelope(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main, nullptr, 0, duration);
    sim_apply_haptic_envelope(sim, SIM_Device::Head, SIM_HapticOutput::Main, ramp, 3, duration);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);
}

// XR_FB_haptic_pcm: samples play at the device rate from the time they were applied
TEST(Sim, haptic_pcm_plays_samples_at_the_device_rate)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };
    const SIM_Device      hand = SIM_Device::LeftHand;
    const SIM_HapticOutput out = SIM_HapticOutput::Main;

    // 2000 samples at the device rate: one second, a ramp from 0 to 1
    static f32 ramp[2000];
    for (u32 i = 0; i < 2000; i++)
    {
        ramp[i] = (f32)i / 1999.0f;
    }
    EXPECT_EQ(sim_apply_haptic_pcm(sim, hand, out, ramp, 2000, SIM_HAPTIC_PCM_RATE, false), 2000u);
    const i64 start = sim_time(sim);
    EXPECT_TRUE(sim_get_haptic(sim, hand, out).active);
    EXPECT_EQ(sim_get_haptic(sim, hand, out).end_time, start + 1000000000);
    for (u32 i = 0; i < 45; i++)
    {
        step(sim);
    }
    const f32 progress = (f32)(sim_time(sim) - start) / 1e9f;
    EXPECT_NEAR(sim_get_haptic(sim, hand, out).amplitude, progress, 1e-3f);

    // Negative samples vibrate as strongly as positive ones
    const f32 swing[4] = { -1.0f, 1.0f, -0.5f, 0.5f };
    EXPECT_EQ(sim_apply_haptic_pcm(sim, hand, out, swing, 4, SIM_HAPTIC_PCM_RATE, false), 4u);
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, hand, out).amplitude, 1.0f);

    // Half the device rate: each sample lasts twice as long
    EXPECT_EQ(sim_apply_haptic_pcm(sim, hand, out, ramp, 1000, SIM_HAPTIC_PCM_RATE / 2.0f, false), 1000u);
    EXPECT_EQ(sim_get_haptic(sim, hand, out).end_time, sim_time(sim) + 1000000000);

    // The buffer keeps SIM_MAX_HAPTIC_SAMPLES; the rest is not consumed
    static f32 many[5000];
    EXPECT_EQ(sim_apply_haptic_pcm(sim, hand, out, many, 5000, SIM_HAPTIC_PCM_RATE, false), (u32)SIM_MAX_HAPTIC_SAMPLES);
    EXPECT_EQ(sim_apply_haptic_pcm(sim, hand, out, many, 10, SIM_HAPTIC_PCM_RATE, true), 0u) << "full";

    // Spec: appending continues after the samples not played yet
    sim_stop_haptic(sim, hand, out);
    const f32 ones[200] = { 1.0f };
    EXPECT_EQ(sim_apply_haptic_pcm(sim, hand, out, ramp, 200, SIM_HAPTIC_PCM_RATE, false), 200u); // 100 ms
    const i64 first = sim_time(sim);
    step(sim);
    EXPECT_EQ(sim_apply_haptic_pcm(sim, hand, out, ones, 200, SIM_HAPTIC_PCM_RATE, true), 200u);
    EXPECT_EQ(sim_get_haptic(sim, hand, out).end_time, first + 200000000);
    EXPECT_EQ(sim_apply_haptic_pcm(sim, hand, out, ones, 200, SIM_HAPTIC_PCM_RATE, false), 200u);
    EXPECT_EQ(sim_get_haptic(sim, hand, out).end_time, sim_time(sim) + 100000000) << "without append the effect restarts";

    // A plain vibration cancels PCM, and PCM appended to it starts afresh
    sim_apply_haptic(sim, hand, out, 0.25f, 100000000, 100.0f);
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, hand, out).amplitude, 0.25f);
    EXPECT_EQ(sim_apply_haptic_pcm(sim, hand, out, ones, 200, SIM_HAPTIC_PCM_RATE, true), 200u);
    EXPECT_EQ(sim_get_haptic(sim, hand, out).end_time, sim_time(sim) + 100000000);
    while (sim_get_haptic(sim, hand, out).active)
    {
        step(sim);
    }
    EXPECT_EQ(sim_apply_haptic_pcm(sim, hand, out, ones, 2, SIM_HAPTIC_PCM_RATE, true), 2u);
}

// XR_EXT_haptic_parametric: curves interpolate linearly over stream time
TEST(Sim, haptic_parametric_curves)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };
    const SIM_Device      hand = SIM_Device::RightHand;
    const SIM_HapticOutput out = SIM_HapticOutput::Main;

    const SIM_HapticPoint amplitudes[3]  = { { 0, 0.0f }, { 100000000, 1.0f }, { 200000000, 0.5f } };
    const SIM_HapticPoint frequencies[2] = { { 0, 0.0f }, { 200000000, 1.0f } };
    sim_apply_haptic_parametric(sim, hand, out, { .amplitudes = amplitudes, .amplitudes_count = 3, .frequencies = frequencies, .frequencies_count = 2, .min_hz = 100.0f, .max_hz = 300.0f });
    const i64 start = sim_time(sim);
    EXPECT_FALSE(sim_haptic_stream_running(sim, hand, out)) << "a single event is no stream";
    EXPECT_EQ(sim_get_haptic(sim, hand, out).end_time, start + 200000000);
    while (sim_time(sim) - start < 200000000)
    {
        const f32 t   = (f32)(sim_time(sim) - start) / 1e8f;
        const f32 amp = t < 1.0f ? t : 1.0f - 0.5f * (t - 1.0f);
        EXPECT_NEAR(sim_get_haptic(sim, hand, out).amplitude, amp, 1e-4f) << t;
        EXPECT_NEAR(sim_get_haptic(sim, hand, out).frequency, 100.0f + 100.0f * t, 1e-2f) << t;
        step(sim);
    }
    EXPECT_FALSE(sim_get_haptic(sim, hand, out).active);

    // Without frequency points the frequency is the middle of the range; without a range the device's
    sim_apply_haptic_parametric(sim, hand, out, { .amplitudes = amplitudes, .amplitudes_count = 3 });
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, hand, out).frequency, (SIM_HAPTIC_MIN_HZ + SIM_HAPTIC_MAX_HZ) / 2.0f);
    sim_apply_haptic_parametric(sim, hand, out, { .amplitudes = amplitudes, .amplitudes_count = 3, .min_hz = 1.0f, .max_hz = 1000.0f });
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, hand, out).frequency, (SIM_HAPTIC_MIN_HZ + SIM_HAPTIC_MAX_HZ) / 2.0f) << "the app's range is clamped to the device's";
}

// Spec XR_EXT_haptic_parametric streams: a late frame holds the last values for 50 ms, then the
// output is quiet but the stream runs on and takes its next frames at the current position
TEST(Sim, haptic_parametric_streams)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };
    const SIM_Device      hand = SIM_Device::LeftHand;
    const SIM_HapticOutput out = SIM_HapticOutput::Main;

    const SIM_HapticPoint first[2] = { { 0, 0.5f }, { 50000000, 0.5f } };
    sim_apply_haptic_parametric(sim, hand, out, { .amplitudes = first, .amplitudes_count = 2, .stream = SIM_HapticStream::First });
    const i64 start = sim_time(sim);
    EXPECT_TRUE(sim_haptic_stream_running(sim, hand, out));
    while (sim_time(sim) - start < 50000000 + SIM_HAPTIC_HOLD_NS)
    {
        EXPECT_FLOAT_EQ(sim_get_haptic(sim, hand, out).amplitude, 0.5f) << sim_time(sim) - start;
        step(sim);
    }
    EXPECT_FALSE(sim_get_haptic(sim, hand, out).active);
    EXPECT_TRUE(sim_haptic_stream_running(sim, hand, out));

    // The next frame continues in stream time
    const i64             now    = sim_time(sim) - start;
    const SIM_HapticPoint next[2] = { { now, 0.75f }, { now + 30000000, 0.75f } };
    sim_apply_haptic_parametric(sim, hand, out, { .amplitudes = next, .amplitudes_count = 2, .stream = SIM_HapticStream::Last });
    EXPECT_TRUE(sim_get_haptic(sim, hand, out).active);
    EXPECT_FLOAT_EQ(sim_get_haptic(sim, hand, out).amplitude, 0.75f);
    EXPECT_FALSE(sim_haptic_stream_running(sim, hand, out)) << "the last frame ends the stream";
    for (u32 i = 0; i < 4; i++)
    {
        step(sim);
    }
    EXPECT_FALSE(sim_get_haptic(sim, hand, out).active);

    // Stopping ends a stream, any other effect replaces it
    sim_apply_haptic_parametric(sim, hand, out, { .amplitudes = first, .amplitudes_count = 2, .stream = SIM_HapticStream::First });
    sim_stop_haptic(sim, hand, out);
    EXPECT_FALSE(sim_haptic_stream_running(sim, hand, out));
    sim_apply_haptic_parametric(sim, hand, out, { .amplitudes = first, .amplitudes_count = 2, .stream = SIM_HapticStream::First });
    sim_apply_haptic(sim, hand, out, 0.1f, 1000000, 0.0f);
    EXPECT_FALSE(sim_haptic_stream_running(sim, hand, out));
}

TEST(Sim, haptic_parametric_streams_remember_their_end)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };
    const SIM_Device       hand = SIM_Device::RightHand;
    const SIM_HapticOutput out  = SIM_HapticOutput::Main;

    SIM_HapticStreamEnd end = {};
    EXPECT_FALSE(sim_haptic_stream_end(sim, hand, out, end)) << "no stream runs";
    const SIM_HapticPoint amplitudes[2]  = { { 0, 0.5f }, { 50000000, 0.5f } };
    const SIM_HapticPoint frequencies[2] = { { 0, 0.5f }, { 80000000, 0.5f } };
    sim_apply_haptic_parametric(sim, hand, out, { .amplitudes = amplitudes, .amplitudes_count = 2, .frequencies = frequencies, .frequencies_count = 2, .stream = SIM_HapticStream::First, .last_transient = 60000000 });
    ASSERT_TRUE(sim_haptic_stream_end(sim, hand, out, end));
    EXPECT_EQ(end.amplitude, 50000000);
    EXPECT_EQ(end.frequency, 80000000);
    EXPECT_EQ(end.transient, 60000000);

    // A frame without frequencies or transients keeps the stream's
    const SIM_HapticPoint next[1] = { { 100000000, 0.5f } };
    sim_apply_haptic_parametric(sim, hand, out, { .amplitudes = next, .amplitudes_count = 1, .stream = SIM_HapticStream::Intermediate });
    ASSERT_TRUE(sim_haptic_stream_end(sim, hand, out, end));
    EXPECT_EQ(end.amplitude, 100000000);
    EXPECT_EQ(end.frequency, 80000000);
    EXPECT_EQ(end.transient, 60000000);

    // A new stream starts over
    sim_apply_haptic_parametric(sim, hand, out, { .amplitudes = amplitudes, .amplitudes_count = 2, .stream = SIM_HapticStream::First });
    ASSERT_TRUE(sim_haptic_stream_end(sim, hand, out, end));
    EXPECT_EQ(end.amplitude, 50000000);
    EXPECT_EQ(end.frequency, -1);
    EXPECT_EQ(end.transient, -1);
    sim_apply_haptic_parametric(sim, hand, out, { .amplitudes = next, .amplitudes_count = 1, .stream = SIM_HapticStream::Last });
    EXPECT_FALSE(sim_haptic_stream_end(sim, hand, out, end));
}

TEST(Sim, state_hash_is_deterministic_and_sensitive)
{
    SIM_Sim* a = make_sim();
    SIM_Sim* b = make_sim();
    defer { sim_destroy(a); sim_destroy(b); };

    EXPECT_EQ(sim_state_hash(a), sim_state_hash(b));
    step(a);
    step(b);
    EXPECT_EQ(sim_state_hash(a), sim_state_hash(b));

    sim_apply_haptic(a, SIM_Device::LeftHand, SIM_HapticOutput::Main, 1.0f, 1000000, 0.0f);
    EXPECT_EQ(sim_state_hash(a), sim_state_hash(b));

    sim_set_input(a, SIM_Device::LeftHand, SIM_Input::XClick, { .value = { 1.0f, 0.0f } });
    EXPECT_NE(sim_state_hash(a), sim_state_hash(b));
}

TEST(Sim, rejects_invalid_arguments_without_crashing)
{
    os_set_assert_mode(OS_AssertMode::Log);
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    const Pose nan_pose = { .position = { zm::nan(), 0.0f, 0.0f }, .orientation = zm::QUAT_IDENTITY };
    const Pose before   = sim_get_device_pose(sim, SIM_Device::Head);
    sim_set_device_pose(sim, SIM_Device::Head, nan_pose);
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head), before));

    sim_set_device_pose(sim, (SIM_Device)99, before);
    sim_set_input(sim, (SIM_Device)99, SIM_Input::AClick, {});
    sim_set_input(sim, SIM_Device::LeftHand, (SIM_Input)200, {});
    EXPECT_FLOAT_EQ(sim_get_input(sim, (SIM_Device)99, SIM_Input::AClick).value.x, 0.0f);
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, (SIM_Device)99), zm::POSE_IDENTITY));
    Pose pose;
    Fov  fov;
    sim_get_view(sim, 5, pose, fov);
    EXPECT_TRUE(zm::is_finite(pose));
}

TEST(Sim, captured_state_replays_to_identical_hashes)
{
    SIM_Sim* live = make_sim(SIM_ClockMode::Realtime);
    defer { sim_destroy(live); };

    // Start the recording mid-session, after a pose was set between frames
    for (u32 i = 0; i < 5; i++)
    {
        sim_begin_frame(live, 900000000ull + i * 7000000ull);
        sim_commit_frame(live);
    }
    sim_set_device_pose(live, SIM_Device::LeftHand, { .position = { -0.3f, 1.2f, -0.4f }, .orientation = zm::QUAT_IDENTITY });
    SIM_Snapshot initial = {};
    sim_capture_snapshot(live, initial);

    SIM_FrameState frames[20] = {};
    u64            hashes[20] = {};
    for (u32 i = 0; i < 20; i++)
    {
        if (i == 3)
        {
            sim_set_input(live, SIM_Device::LeftHand, SIM_Input::TriggerValue, { .value = { 0.6f, 0.0f } });
        }
        if (i == 7)
        {
            sim_set_device_pose(live, SIM_Device::RightHand, { .position = { 0.4f, 1.1f, -0.2f }, .orientation = zm::quat_from_euler({ 0.1f, 0.2f, 0.3f }) });
        }
        if (i == 11)
        {
            sim_set_device_active(live, SIM_Device::LeftHand, false);
            sim_recenter(live);
        }
        sim_begin_frame(live, 1000000000ull + i * 9000000ull + (i % 3) * 1000ull);
        if (i == 9)
        {
            sim_apply_haptic(live, SIM_Device::LeftHand, SIM_HapticOutput::Main, 1.0f, 50000000, 0.0f);
        }
        sim_commit_frame(live);
        sim_capture_state(live, frames[i]);
        hashes[i] = sim_state_hash(live);
    }

    for (u32 run = 0; run < 2; run++)
    {
        SIM_Config config = {};
        sim_default_config(config);
        config.devices[(u32)SIM_Device::Head].pose.position = { 5.0f, 5.0f, 5.0f };
        SIM_Sim* replay = sim_create(config);
        sim_set_controls_enabled(replay, false);
        sim_restore_snapshot(replay, initial);
        for (u32 i = 0; i < 20; i++)
        {
            sim_replay_frame(replay, frames[i]);
            sim_commit_frame(replay);
            EXPECT_EQ(sim_state_hash(replay), hashes[i]) << "frame " << i << " run " << run;
        }
        sim_destroy(replay);
    }
}

TEST(Sim, replayed_garbage_state_is_sanitized)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const Pose before = sim_get_device_pose(sim, SIM_Device::Head);

    SIM_FrameState state = {};
    sim_capture_state(sim, state);
    state.dt                                                                 = -50;
    state.devices[(u32)SIM_Device::Head].pose.position.x                     = zm::nan();
    state.devices[(u32)SIM_Device::LeftHand].inputs[(u32)SIM_Input::TriggerValue].value = { 9.0f, 0.0f };
    state.devices[(u32)SIM_Device::Head].inputs[(u32)SIM_Input::AClick].value           = { 1.0f, 0.0f };
    const i64 t0 = sim_time(sim);
    sim_replay_frame(sim, state);
    sim_commit_frame(sim);

    EXPECT_EQ(sim_time(sim), t0);
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head), before));
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue).value.x, 1.0f);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::Head, SIM_Input::AClick).value.x, 0.0f);
}

// Regression: replay used to normalize recorded orientations again, which flips the last bit of
// some unit quaternions and made replays diverge from their recording
TEST(Sim, replayed_poses_are_applied_bit_exactly)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    u32 renormalize_changes = 0;
    for (u32 i = 0; i < 200; i++)
    {
        const Quat q = zm::quat_from_euler({ 0.013f * (f32)i, 0.031f * (f32)i, 0.007f * (f32)i });
        const Quat n = zm::normalize(q);
        renormalize_changes += !zm::equal(zm::normalize(n), n);

        SIM_FrameState state = {};
        sim_capture_state(sim, state);
        state.devices[(u32)SIM_Device::LeftHand].pose = { { 0.1f, 1.2f, -0.3f }, n };
        state.local_origin                            = { { 0.0f, 1.6f, 0.0f }, n };
        sim_replay_frame(sim, state);
        EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::LeftHand).orientation, n)) << i;
        EXPECT_TRUE(zm::equal(sim_get_reference_space_pose(sim, SIM_ReferenceSpace::Local).orientation, n)) << i;
    }
    EXPECT_GT(renormalize_changes, 0u);
}

TEST(Sim, controller_types_and_their_inputs)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    EXPECT_EQ(sim_get_config(sim).controller, SIM_Controller::MetaTouchPlus); // Quest 3
    EXPECT_EQ(sim_get_controller(sim), SIM_Controller::MetaTouchPlus);
    sim_set_controller(sim, SIM_Controller::OculusTouch);
    EXPECT_TRUE(cstr_equal(sim_controller_name(SIM_Controller::OculusTouch), "oculus_touch"));
    EXPECT_TRUE(cstr_equal(sim_controller_name(SIM_Controller::KhrSimple), "khr_simple"));
    SIM_Controller parsed = SIM_Controller::Count;
    EXPECT_TRUE(sim_controller_from_name("khr_simple", parsed));
    EXPECT_EQ(parsed, SIM_Controller::KhrSimple);
    EXPECT_FALSE(sim_controller_from_name("nope", parsed));

    EXPECT_TRUE(sim_controller_has_input(SIM_Controller::OculusTouch, SIM_Input::TriggerValue));
    EXPECT_TRUE(sim_controller_has_input(SIM_Controller::OculusTouch, SIM_Input::ThumbrestTouch));
    EXPECT_FALSE(sim_controller_has_input(SIM_Controller::OculusTouch, SIM_Input::SelectClick));
    EXPECT_TRUE(sim_controller_has_input(SIM_Controller::KhrSimple, SIM_Input::SelectClick));
    EXPECT_TRUE(sim_controller_has_input(SIM_Controller::KhrSimple, SIM_Input::MenuClick));
    EXPECT_FALSE(sim_controller_has_input(SIM_Controller::KhrSimple, SIM_Input::TriggerValue));
    EXPECT_FALSE(sim_controller_has_input(SIM_Controller::KhrSimple, SIM_Input::Thumbstick));

    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue, { .value = { 0.5f, 0.0f } });
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::SelectClick, { .value = { 1.0f, 0.0f } });
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue).value.x, 0.5f);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::SelectClick).value.x, 0.0f);

    const u64 touch_hash = sim_state_hash(sim);
    sim_set_controller(sim, SIM_Controller::KhrSimple);
    EXPECT_NE(sim_state_hash(sim), touch_hash);
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue).value.x, 0.0f);
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::SelectClick, { .value = { 1.0f, 0.0f } });
    EXPECT_FLOAT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::SelectClick).value.x, 1.0f);

    sim_set_controller(sim, (SIM_Controller)99);
    EXPECT_EQ(sim_get_controller(sim), SIM_Controller::KhrSimple);
}

TEST(Sim, meta_touch_pro_and_plus_inputs)
{
    EXPECT_TRUE(cstr_equal(sim_controller_name(SIM_Controller::MetaTouchPro), "meta_touch_pro"));
    EXPECT_TRUE(cstr_equal(sim_controller_name(SIM_Controller::MetaTouchPlus), "meta_touch_plus"));
    SIM_Controller parsed = SIM_Controller::Count;
    EXPECT_TRUE(sim_controller_from_name("meta_touch_pro", parsed));
    EXPECT_EQ(parsed, SIM_Controller::MetaTouchPro);

    const SIM_Input common[] = { SIM_Input::TriggerValue, SIM_Input::TriggerTouch, SIM_Input::ThumbrestTouch, SIM_Input::TriggerProximity, SIM_Input::ThumbProximity, SIM_Input::AClick };
    for (const SIM_Input input : common)
    {
        EXPECT_TRUE(sim_controller_has_input(SIM_Controller::OculusTouch, input)) << sim_input_name(input);
        EXPECT_TRUE(sim_controller_has_input(SIM_Controller::MetaTouchPro, input)) << sim_input_name(input);
        EXPECT_TRUE(sim_controller_has_input(SIM_Controller::MetaTouchPlus, input)) << sim_input_name(input);
    }
    EXPECT_TRUE(sim_controller_has_input(SIM_Controller::MetaTouchPro, SIM_Input::TriggerCurl));
    EXPECT_TRUE(sim_controller_has_input(SIM_Controller::MetaTouchPro, SIM_Input::TriggerSlide));
    EXPECT_TRUE(sim_controller_has_input(SIM_Controller::MetaTouchPro, SIM_Input::ThumbrestForce));
    EXPECT_TRUE(sim_controller_has_input(SIM_Controller::MetaTouchPro, SIM_Input::StylusForce));
    EXPECT_FALSE(sim_controller_has_input(SIM_Controller::MetaTouchPro, SIM_Input::TriggerForce));
    EXPECT_TRUE(sim_controller_has_input(SIM_Controller::MetaTouchPlus, SIM_Input::TriggerCurl));
    EXPECT_TRUE(sim_controller_has_input(SIM_Controller::MetaTouchPlus, SIM_Input::TriggerSlide));
    EXPECT_TRUE(sim_controller_has_input(SIM_Controller::MetaTouchPlus, SIM_Input::TriggerForce));
    EXPECT_FALSE(sim_controller_has_input(SIM_Controller::MetaTouchPlus, SIM_Input::StylusForce));
    EXPECT_FALSE(sim_controller_has_input(SIM_Controller::OculusTouch, SIM_Input::TriggerCurl));
    EXPECT_FALSE(sim_controller_has_input(SIM_Controller::KhrSimple, SIM_Input::TriggerProximity));

    // Names are the core 1.1 component paths, or the vendor path without its suffix
    EXPECT_TRUE(cstr_equal(sim_input_name(SIM_Input::TriggerProximity), "trigger/proximity"));
    EXPECT_TRUE(cstr_equal(sim_input_name(SIM_Input::ThumbProximity), "thumb_resting_surfaces/proximity"));
    EXPECT_TRUE(cstr_equal(sim_input_name(SIM_Input::TriggerCurl), "trigger/curl"));
    EXPECT_TRUE(cstr_equal(sim_input_name(SIM_Input::TriggerSlide), "trigger/slide"));
    EXPECT_TRUE(cstr_equal(sim_input_name(SIM_Input::TriggerForce), "trigger/force"));
    EXPECT_TRUE(cstr_equal(sim_input_name(SIM_Input::ThumbrestForce), "thumbrest/force"));
    EXPECT_TRUE(cstr_equal(sim_input_name(SIM_Input::StylusForce), "stylus/force"));
    EXPECT_EQ(sim_input_type(SIM_Input::TriggerForce), SIM_InputType::Float);
    EXPECT_EQ(sim_input_type(SIM_Input::ThumbProximity), SIM_InputType::Bool);
    for (u32 c = 0; c < (u32)SIM_Controller::Count; c++)
    {
        for (u32 s = 0; s < (u32)SIM_Slot::Count; s++)
        {
            const SIM_Input mapped = sim_controller_slot_input((SIM_Controller)c, SIM_Device::RightHand, (SIM_Slot)s);
            EXPECT_TRUE(mapped == SIM_Input::Count || sim_controller_has_input((SIM_Controller)c, mapped)) << c << " " << s;
        }
    }
    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::MetaTouchPlus, SIM_Device::RightHand, SIM_Slot::Primary), SIM_Input::AClick);
    EXPECT_EQ(sim_controller_slot_input(SIM_Controller::MetaTouchPro, SIM_Device::LeftHand, SIM_Slot::Menu), SIM_Input::MenuClick);
}

// With auto_touch, a touched trigger is near the finger and any touched thumb surface is near the thumb
TEST(Sim, touch_implies_proximity)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };
    sim_control_key(sim, OS_Key::U, true);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerProximity), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::ThumbProximity), 0.0f);
    sim_control_key(sim, OS_Key::U, false);
    sim_control_key(sim, OS_Key::H, true);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerProximity), 0.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::ThumbProximity), 1.0f);
    sim_control_key(sim, OS_Key::H, false);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::ThumbProximity), 0.0f);

    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::ThumbProximity, { .value = { 1.0f, 0.0f } });
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerCurl, { .value = { 0.25f, 0.0f } });
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::ThumbProximity), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::LeftHand, SIM_Input::TriggerCurl), 0.25f);
}

TEST(Sim, haptic_outputs_are_separate)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    sim_apply_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Trigger, 0.5f, 100000000, 0.0f);
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Trigger).active);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main).active);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Thumb).active);
    sim_stop_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Main);
    EXPECT_TRUE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Trigger).active);
    sim_stop_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Trigger);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, SIM_HapticOutput::Trigger).active);
    EXPECT_FALSE(sim_get_haptic(sim, SIM_Device::LeftHand, (SIM_HapticOutput)9).active);
}

TEST(Sim, keyboard_slots_skip_inputs_the_controller_lacks)
{
    SIM_Config config = {};
    sim_default_config(config);
    config.clock_mode = SIM_ClockMode::FixedStep;
    config.controller = SIM_Controller::KhrSimple;
    SIM_Sim* sim = sim_create(config);
    defer { sim_destroy(sim); };

    sim_control_key(sim, OS_Key::MouseLeft, true);
    sim_control_key(sim, OS_Key::H, true);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::SelectClick), 1.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::TriggerValue), 0.0f);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::AClick), 0.0f);
    sim_control_key(sim, OS_Key::MouseLeft, false);
    sim_control_key(sim, OS_Key::H, false);
    step(sim);
    EXPECT_FLOAT_EQ(input_x(sim, SIM_Device::RightHand, SIM_Input::SelectClick), 0.0f);
}

// XR_FB_display_refresh_rate: the configured list plus the configured rate, sorted, without duplicates
TEST(Sim, refresh_rates_from_config_and_requests)
{
    SIM_Config config = {};
    sim_default_config(config);
    {
        SIM_Sim* sim = sim_create(config);
        defer { sim_destroy(sim); };
        f32       rates[SIM_MAX_REFRESH_RATES] = {};
        const u32 count                        = sim_refresh_rates(sim, rates, SIM_MAX_REFRESH_RATES);
        ASSERT_EQ(count, 4u);
        EXPECT_FLOAT_EQ(rates[0], 72.0f);
        EXPECT_FLOAT_EQ(rates[1], 80.0f);
        EXPECT_FLOAT_EQ(rates[2], 90.0f);
        EXPECT_FLOAT_EQ(rates[3], 120.0f);
        EXPECT_FLOAT_EQ(sim_refresh_rate(sim), 90.0f);

        EXPECT_TRUE(sim_set_refresh_rate(sim, 120.0f));
        EXPECT_FLOAT_EQ(sim_refresh_rate(sim), 120.0f);
        EXPECT_EQ(sim_display_period(sim), 8333333);
        EXPECT_FLOAT_EQ(sim_get_config(sim).refresh_rate_hz, 120.0f); // what a recording started now writes
        EXPECT_EQ(sim_get_config(sim).display_period_ns, 8333333);
        EXPECT_FALSE(sim_set_refresh_rate(sim, 75.0f));
        EXPECT_FALSE(sim_set_refresh_rate(sim, zm::nan()));
        EXPECT_FLOAT_EQ(sim_refresh_rate(sim), 120.0f);
        EXPECT_FLOAT_EQ(sim_default_refresh_rate(sim), 90.0f);
    }

    EXPECT_TRUE(sim_parse_config(config, R"({ "clock": { "refresh_rate_hz": 60, "refresh_rates": [ 120, 90, 90, 5000, "x" ] } })"));
    SIM_Sim* sim = sim_create(config);
    defer { sim_destroy(sim); };
    f32       rates[SIM_MAX_REFRESH_RATES] = {};
    const u32 count                        = sim_refresh_rates(sim, rates, SIM_MAX_REFRESH_RATES);
    ASSERT_EQ(count, 3u);
    EXPECT_FLOAT_EQ(rates[0], 60.0f);
    EXPECT_FLOAT_EQ(rates[1], 90.0f);
    EXPECT_FLOAT_EQ(rates[2], 120.0f);
    EXPECT_EQ(sim_refresh_rates(sim, rates, 1), 3u); // the count, whatever the capacity

    // A requested rate steps the clock the same on every run
    SIM_Sim* again = sim_create(config);
    defer { sim_destroy(again); };
    EXPECT_TRUE(sim_set_refresh_rate(sim, 120.0f));
    EXPECT_TRUE(sim_set_refresh_rate(again, 120.0f));
    for (u32 i = 0; i < 5; i++)
    {
        step(sim);
        step(again);
    }
    EXPECT_EQ(sim_time(sim), sim_time(again));
    EXPECT_EQ(sim_state_hash(sim), sim_state_hash(again));
}

TEST(Sim, controller_from_config)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_TRUE(sim_parse_config(config, R"({ "controller": "khr_simple" })"));
    EXPECT_EQ(config.controller, SIM_Controller::KhrSimple);
    EXPECT_TRUE(sim_parse_config(config, R"({ "controller": "bogus" })"));
    EXPECT_EQ(config.controller, SIM_Controller::KhrSimple);
}

TEST(Sim, angular_velocity_takes_the_short_arc_for_negated_quaternions)
{
    SIM_Sim* sim = make_sim();
    sim_set_device_pose(sim, SIM_Device::Head, { .position = {}, .orientation = zm::QUAT_IDENTITY });
    step(sim);
    sim_set_device_pose(sim, SIM_Device::Head, { .position = {}, .orientation = -zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.01f) });
    step(sim);
    const f32 period = (f32)sim_display_period(sim) * 1e-9f;
    const Vec3 omega = sim_get_device_state(sim, SIM_Device::Head).angular_velocity;
    EXPECT_NEAR(omega.x, 0.0f, 1e-3f);
    EXPECT_NEAR(omega.y, 0.01f / period, 1e-2f);
    EXPECT_NEAR(omega.z, 0.0f, 1e-3f);
    sim_destroy(sim);
}

// Spec 2.16: at least 50 ms of history, interpolated; any future time is valid
TEST(Sim, samples_interpolate_history_and_extrapolate_the_future)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const i64 period = sim_display_period(sim);

    for (u32 i = 0; i < 40; i++)
    {
        sim_set_device_pose(sim, SIM_Device::LeftHand, { .position = { (f32)i * 0.01f, 1.0f, 0.0f }, .orientation = zm::QUAT_IDENTITY });
        step(sim);
    }
    const i64 live = sim_predicted_display_time(sim);
    ASSERT_GE((i64)(SIM_HISTORY_SAMPLES - 1) * period, 50000000);

    SIM_Sample sample = {};
    sim_sample(sim, live, sample);
    EXPECT_NEAR(sample.devices[(u32)SIM_Device::LeftHand].pose.position.x, 0.39f, 1e-5f);

    sim_sample(sim, live - period / 2, sample);
    EXPECT_NEAR(sample.devices[(u32)SIM_Device::LeftHand].pose.position.x, 0.385f, 1e-4f);

    sim_sample(sim, live - 5 * period, sample);
    EXPECT_NEAR(sample.devices[(u32)SIM_Device::LeftHand].pose.position.x, 0.34f, 1e-4f);
    EXPECT_NEAR(sample.devices[(u32)SIM_Device::LeftHand].linear_velocity.x, 0.01f / ((f32)period * 1e-9f), 1e-2f);

    sim_sample(sim, live + 2 * period, sample);
    EXPECT_NEAR(sample.devices[(u32)SIM_Device::LeftHand].pose.position.x, 0.41f, 1e-4f);

    // Older than the history clamps to the oldest sample
    SIM_Sample oldest = {};
    sim_sample(sim, 1, oldest);
    sim_sample(sim, live - (i64)SIM_HISTORY_SAMPLES * period * 4, sample);
    EXPECT_EQ(sample.devices[(u32)SIM_Device::LeftHand].pose.position.x, oldest.devices[(u32)SIM_Device::LeftHand].pose.position.x);
    EXPECT_LT(oldest.devices[(u32)SIM_Device::LeftHand].pose.position.x, 0.39f - 0.01f * (f32)(SIM_HISTORY_SAMPLES - 2));
}

// Spec 7.1: a recentered origin applies only from its change time on
TEST(Sim, recentering_takes_effect_at_its_change_time)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    step(sim);
    step(sim);
    u64 generation  = 0;
    i64 change_time = 0;
    sim_local_origin_change(sim, generation, change_time);
    const u64 before = generation;

    sim_set_device_pose(sim, SIM_Device::Head, { .position = { 1.0f, 1.6f, 2.0f }, .orientation = zm::QUAT_IDENTITY });
    sim_recenter(sim);
    sim_local_origin_change(sim, generation, change_time);
    EXPECT_EQ(generation, before + 1);
    EXPECT_EQ(change_time, sim_predicted_display_time(sim) + sim_display_period(sim));

    SIM_Sample sample = {};
    sim_sample(sim, change_time - 1, sample);
    EXPECT_NE(sim_sample_reference_space_pose(sample, SIM_ReferenceSpace::Local).position.x, 1.0f);
    sim_sample(sim, change_time, sample);
    EXPECT_EQ(sim_sample_reference_space_pose(sample, SIM_ReferenceSpace::Local).position.x, 1.0f);
    step(sim);
    sim_sample(sim, change_time - 1, sample);
    EXPECT_NE(sim_sample_reference_space_pose(sample, SIM_ReferenceSpace::Local).position.x, 1.0f);
    sim_sample(sim, change_time + 1, sample);
    EXPECT_EQ(sim_sample_reference_space_pose(sample, SIM_ReferenceSpace::Local).position.x, 1.0f);
}

TEST(Sim, samples_before_the_first_frame_use_the_initial_origin)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    SIM_Sample sample = {};
    sim_sample(sim, 1, sample);
    EXPECT_NEAR(zm::length(sample.local_origin.orientation), 1.0f, 1e-5f);
    EXPECT_EQ(sample.local_origin.position.x, sim_get_reference_space_pose(sim, SIM_ReferenceSpace::Local).position.x);
}

TEST(Sim, setting_a_hand_pose_moves_the_grip_under_it)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const Pose target = { .position = { 0.2f, 1.1f, -0.3f }, .orientation = zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, 0.4f) };
    sim_set_hand_pose(sim, SIM_Device::LeftHand, SIM_HandPose::Aim, target);
    const Pose aim = sim_get_hand_pose(sim, SIM_Device::LeftHand, SIM_HandPose::Aim);
    EXPECT_NEAR(aim.position.x, target.position.x, 1e-5f);
    EXPECT_NEAR(aim.position.y, target.position.y, 1e-5f);
    EXPECT_NEAR(aim.position.z, target.position.z, 1e-5f);
    EXPECT_NEAR(zm::abs(zm::dot(aim.orientation, target.orientation)), 1.0f, 1e-5f);
}

TEST(Sim, control_server_config_from_json_and_environment)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_FALSE(config.control);
    EXPECT_EQ(config.control_port, 0u);
    EXPECT_STREQ(config.control_address, "127.0.0.1");

    EXPECT_FALSE(config.control_paused);
    ASSERT_TRUE(sim_parse_config(config, R"({ "control": { "enabled": true, "port": 4711, "address": "0.0.0.0", "paused": true } })"));
    EXPECT_TRUE(config.control);
    EXPECT_TRUE(config.control_paused);
    EXPECT_EQ(config.control_port, 4711u);
    EXPECT_STREQ(config.control_address, "0.0.0.0");

    sim_default_config(config);
    os_set_env("PUPPETXR_CONTROL", "1");
    os_set_env("PUPPETXR_CONTROL_PORT", "5000");
    os_set_env("PUPPETXR_CONTROL_ADDRESS", "192.168.1.2");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_CONTROL");
    os_unset_env("PUPPETXR_CONTROL_PORT");
    os_unset_env("PUPPETXR_CONTROL_ADDRESS");
    EXPECT_TRUE(config.control);
    EXPECT_EQ(config.control_port, 5000u);
    EXPECT_STREQ(config.control_address, "192.168.1.2");
}

// The lens mask is there to try how an app treats the corners; by default the whole view shows
TEST(Sim, lens_mask_radius_from_config)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_FLOAT_EQ(config.mask_radius, 1.25f);
    EXPECT_FALSE(config.lens_mask);
    ASSERT_TRUE(sim_parse_config(config, R"({ "views": { "mask_radius": 0, "lens_mask": true } })"));
    EXPECT_FLOAT_EQ(config.mask_radius, 0.0f);
    EXPECT_TRUE(config.lens_mask);
    os_set_env("PUPPETXR_LENS_MASK", "0");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_LENS_MASK");
    EXPECT_FALSE(config.lens_mask);
}


TEST(Sim, disabled_extensions_from_config_and_environment)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_EQ(config.disabled_extensions_count, 0u);
    ASSERT_TRUE(sim_parse_config(config, R"({ "disabled_extensions": [ "XR_FB_passthrough", "XR_EXT_future", 7 ] })"));
    ASSERT_EQ(config.disabled_extensions_count, 2u);
    EXPECT_STREQ(config.disabled_extensions[0], "XR_FB_passthrough");
    EXPECT_STREQ(config.disabled_extensions[1], "XR_EXT_future");

    os_set_env("PUPPETXR_DISABLE_EXTENSIONS", "XR_KHR_visibility_mask,,XR_EXT_local_floor");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_DISABLE_EXTENSIONS");
    ASSERT_EQ(config.disabled_extensions_count, 2u);
    EXPECT_STREQ(config.disabled_extensions[0], "XR_KHR_visibility_mask");
    EXPECT_STREQ(config.disabled_extensions[1], "XR_EXT_local_floor");
    EXPECT_TRUE(sim_extension_disabled(config, "XR_EXT_local_floor"));
    EXPECT_FALSE(sim_extension_disabled(config, "XR_FB_passthrough"));

    os_set_env("PUPPETXR_DISABLE_EXTENSIONS", "");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_DISABLE_EXTENSIONS");
    EXPECT_EQ(config.disabled_extensions_count, 0u);
}

TEST(Sim, graphics_apis_from_config_and_environment)
{
    SIM_Config config = {};
    sim_default_config(config);
    for (u32 i = 0; i < (u32)SIM_GraphicsApi::Count; i++)
    {
        EXPECT_TRUE(sim_graphics_api_allowed(config, (SIM_GraphicsApi)i)) << sim_graphics_api_name((SIM_GraphicsApi)i);
    }

    ASSERT_TRUE(sim_parse_config(config, R"({ "graphics": "opengl" })"));
    EXPECT_TRUE(sim_graphics_api_allowed(config, SIM_GraphicsApi::OpenGL));
    EXPECT_FALSE(sim_graphics_api_allowed(config, SIM_GraphicsApi::Vulkan));
    EXPECT_FALSE(sim_graphics_api_allowed(config, SIM_GraphicsApi::OpenGLES));

    ASSERT_TRUE(sim_parse_config(config, R"({ "graphics": [ "vulkan", "opengles", "glide", 3 ] })"));
    EXPECT_TRUE(sim_graphics_api_allowed(config, SIM_GraphicsApi::Vulkan));
    EXPECT_TRUE(sim_graphics_api_allowed(config, SIM_GraphicsApi::OpenGLES));
    EXPECT_FALSE(sim_graphics_api_allowed(config, SIM_GraphicsApi::OpenGL));

    // Nothing known means no restriction rather than no graphics at all
    ASSERT_TRUE(sim_parse_config(config, R"({ "graphics": [ "glide" ] })"));
    EXPECT_TRUE(sim_graphics_api_allowed(config, SIM_GraphicsApi::OpenGL));

    os_set_env("PUPPETXR_GRAPHICS", "d3d11,metal");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_GRAPHICS");
    EXPECT_TRUE(sim_graphics_api_allowed(config, SIM_GraphicsApi::D3D11));
    EXPECT_TRUE(sim_graphics_api_allowed(config, SIM_GraphicsApi::Metal));
    EXPECT_FALSE(sim_graphics_api_allowed(config, SIM_GraphicsApi::Vulkan));

    os_set_env("PUPPETXR_GRAPHICS", "");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_GRAPHICS");
    EXPECT_TRUE(sim_graphics_api_allowed(config, SIM_GraphicsApi::Vulkan));

    EXPECT_STREQ(sim_graphics_api_name(SIM_GraphicsApi::OpenGLES), "opengles");
    EXPECT_STREQ(sim_graphics_api_name(SIM_GraphicsApi::Count), "unknown");
    EXPECT_FALSE(sim_graphics_api_allowed(config, SIM_GraphicsApi::Count));
}

TEST(Sim, origin_offset_from_config_and_environment)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_EQ(config.origin_offset_mode, SIM_OriginOffset::None);

    ASSERT_TRUE(sim_parse_config(config, R"({ "origin_offset": { "yaw_degrees": 90, "position": [0.5, -0.25] } })"));
    EXPECT_EQ(config.origin_offset_mode, SIM_OriginOffset::Fixed);
    EXPECT_FLOAT_EQ(config.origin_offset.position.x, 0.5f);
    EXPECT_FLOAT_EQ(config.origin_offset.position.y, 0.0f);
    EXPECT_FLOAT_EQ(config.origin_offset.position.z, -0.25f);
    EXPECT_NEAR(config.origin_offset.orientation.y, 0.70710678f, 1e-6f);
    EXPECT_NEAR(config.origin_offset.orientation.w, 0.70710678f, 1e-6f);

    ASSERT_TRUE(sim_parse_config(config, R"({ "origin_offset": "per_run", "origin_seed": 42 })"));
    EXPECT_EQ(config.origin_offset_mode, SIM_OriginOffset::PerRun);
    EXPECT_EQ(config.origin_seed, 42u);
    ASSERT_TRUE(sim_parse_config(config, R"({ "origin_offset": "none" })"));
    EXPECT_EQ(config.origin_offset_mode, SIM_OriginOffset::None);
    ASSERT_TRUE(sim_parse_config(config, R"({ "origin_offset": "sideways" })"));
    EXPECT_EQ(config.origin_offset_mode, SIM_OriginOffset::None);

    os_set_env("PUPPETXR_ORIGIN_OFFSET", "180,1,2");
    sim_apply_env(config);
    EXPECT_EQ(config.origin_offset_mode, SIM_OriginOffset::Fixed);
    EXPECT_FLOAT_EQ(config.origin_offset.position.x, 1.0f);
    EXPECT_FLOAT_EQ(config.origin_offset.position.z, 2.0f);
    EXPECT_NEAR(zm::abs(config.origin_offset.orientation.y), 1.0f, 1e-6f);
    os_set_env("PUPPETXR_ORIGIN_OFFSET", "per_run");
    os_set_env("PUPPETXR_ORIGIN_SEED", "9");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_ORIGIN_OFFSET");
    os_unset_env("PUPPETXR_ORIGIN_SEED");
    EXPECT_EQ(config.origin_offset_mode, SIM_OriginOffset::PerRun);
    EXPECT_EQ(config.origin_seed, 9u);
}

static Pose yaw_pose (f32 yaw_degrees, Vec3 position)
{
    return { position, zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, yaw_degrees * zm::DEG_TO_RAD) };
}

// Tracking drift moves the tracking origin, not the physical devices: STAGE, LOCAL, LOCAL_FLOOR
// and STATIONARY lie at the drift in the simulator's frame, VIEW stays on the head
TEST(Sim, tracking_drift_moves_the_tracking_spaces_and_leaves_the_devices)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    step(sim);
    SIM_Sample before = {};
    sim_sample(sim, sim_predicted_display_time(sim), before);
    EXPECT_TRUE(zm::equal(sim_get_tracking_drift(sim), zm::POSE_IDENTITY));

    const Pose drift = yaw_pose(30.0f, { 0.2f, 0.05f, -0.1f });
    ASSERT_TRUE(sim_set_tracking_drift(sim, drift));
    EXPECT_TRUE(zm::near(sim_get_tracking_drift(sim), drift, 1e-6f));
    SIM_Sample after = {};
    sim_sample(sim, sim_predicted_display_time(sim), after);

    const SIM_ReferenceSpace tracking[] = { SIM_ReferenceSpace::Stage, SIM_ReferenceSpace::Local, SIM_ReferenceSpace::LocalFloor, SIM_ReferenceSpace::Stationary };
    for (const SIM_ReferenceSpace space : tracking)
    {
        const Pose expected = zm::pose_mul(drift, sim_sample_reference_space_pose(before, space));
        EXPECT_TRUE(zm::near(sim_sample_reference_space_pose(after, space), expected, 1e-5f)) << (u32)space;
    }
    EXPECT_TRUE(zm::equal(sim_sample_reference_space_pose(after, SIM_ReferenceSpace::View), sim_sample_reference_space_pose(before, SIM_ReferenceSpace::View)));
    for (u32 d = 0; d < (u32)SIM_Device::Count; d++)
    {
        EXPECT_TRUE(zm::equal(after.devices[d].pose, before.devices[d].pose)) << d;
    }
    // The origins as tracking measures them, relative to STAGE, do not change
    EXPECT_TRUE(zm::equal(sim_get_reference_space_pose(sim, SIM_ReferenceSpace::Local), before.local_origin));

    ASSERT_TRUE(sim_set_tracking_drift(sim, zm::POSE_IDENTITY));
    sim_sample(sim, sim_predicted_display_time(sim), after);
    EXPECT_TRUE(zm::equal(sim_sample_reference_space_pose(after, SIM_ReferenceSpace::Stage), zm::POSE_IDENTITY));
}

TEST(Sim, a_recenter_under_drift_puts_local_at_the_head)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    ASSERT_TRUE(sim_set_tracking_drift(sim, yaw_pose(-45.0f, { 0.5f, 0.0f, 0.3f })));
    const Pose head = { .position = { 1.0f, 1.6f, -0.5f }, .orientation = zm::quat_from_euler({ 0.2f, 0.7f, 0.0f }) };
    sim_set_device_pose(sim, SIM_Device::Head, head);
    sim_recenter(sim);
    step(sim);
    SIM_Sample sample = {};
    sim_sample(sim, sim_predicted_display_time(sim) + sim_display_period(sim), sample);
    const Pose local = sim_sample_reference_space_pose(sample, SIM_ReferenceSpace::Local);
    EXPECT_TRUE(zm::near(local.position, head.position, 1e-5f));
    const Vec3 euler = zm::euler_from_quat(local.orientation);
    EXPECT_NEAR(euler.x, 0.0f, 1e-5f);
    EXPECT_NEAR(euler.y, 0.7f, 1e-5f);
    EXPECT_NEAR(euler.z, 0.0f, 1e-5f);
}

TEST(Sim, tracking_drift_refuses_what_tracking_cannot_do)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const Pose kept = yaw_pose(10.0f, { 0.1f, 0.0f, 0.0f });
    ASSERT_TRUE(sim_set_tracking_drift(sim, kept));

    const f32  nan   = zm::sqrt(-1.0f);
    const Pose bad[] = {
        { { nan, 0.0f, 0.0f }, zm::QUAT_IDENTITY },
        { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 0.0f } },
        { { 0.0f, 0.0f, 0.0f }, { 0.0f, nan, 0.0f, 1.0f } },
        { { 0.0f, 0.0f, 0.0f }, zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, 0.1f) },  // gravity stays down
        { { 0.0f, 0.0f, 0.0f }, zm::quat_from_euler({ 0.0f, 0.5f, 0.01f }) },
    };
    for (const Pose& pose : bad)
    {
        EXPECT_FALSE(sim_set_tracking_drift(sim, pose));
        EXPECT_TRUE(zm::equal(sim_get_tracking_drift(sim), kept));
    }
    EXPECT_FALSE(sim_set_tracking_drift_rate(sim, { nan, 0.0f, 0.0f }, 0.0f));
    EXPECT_FALSE(sim_set_tracking_drift_rate(sim, {}, nan));
    Vec3 velocity = { 1.0f, 1.0f, 1.0f };
    f32  yaw_rate = 1.0f;
    sim_get_tracking_drift_rate(sim, velocity, yaw_rate);
    EXPECT_TRUE(zm::equal(velocity, Vec3{}));
    EXPECT_EQ(yaw_rate, 0.0f);

    // A quaternion of any length within reason is a rotation; it is kept unit length
    ASSERT_TRUE(sim_set_tracking_drift(sim, { {}, { 0.0f, 0.0f, 0.0f, 2.0f } }));
    EXPECT_TRUE(zm::equal(sim_get_tracking_drift(sim).orientation, zm::QUAT_IDENTITY));
}

// The rate applies once per frame with the frame's clock step, so fixed steps give the same
// drift on every run and machine
TEST(Sim, the_drift_rate_accumulates_over_fixed_steps)
{
    SIM_Sim* a = make_sim();
    SIM_Sim* b = make_sim();
    defer { sim_destroy(a); sim_destroy(b); };
    ASSERT_TRUE(sim_set_tracking_drift_rate(a, { 0.01f, 0.0f, -0.02f }, 3.0f));
    ASSERT_TRUE(sim_set_tracking_drift_rate(b, { 0.01f, 0.0f, -0.02f }, 3.0f));
    Vec3 velocity = {};
    f32  yaw_rate = 0.0f;
    sim_get_tracking_drift_rate(a, velocity, yaw_rate);
    EXPECT_TRUE(zm::equal(velocity, Vec3{ 0.01f, 0.0f, -0.02f }));
    EXPECT_EQ(yaw_rate, 3.0f);

    const u32 frames  = 180;
    const f32 seconds = (f32)((i64)frames * sim_display_period(a)) * 1e-9f;
    for (u32 i = 0; i < frames; i++)
    {
        step(a);
        step(b);
    }
    const Pose drift   = sim_get_tracking_drift(a);
    const Pose drift_b = sim_get_tracking_drift(b);
    EXPECT_TRUE(memory_equal(&drift, &drift_b, sizeof(Pose)));
    EXPECT_TRUE(zm::near(drift.position, Vec3{ 0.01f * seconds, 0.0f, -0.02f * seconds }, 1e-5f));
    EXPECT_TRUE(zm::near(drift, yaw_pose(3.0f * seconds, drift.position), 1e-5f));
    EXPECT_NEAR(seconds, 2.0f, 1e-3f);

    // Stopping the rate keeps the drift reached; a step resets it
    ASSERT_TRUE(sim_set_tracking_drift_rate(a, {}, 0.0f));
    step(a);
    EXPECT_TRUE(zm::equal(sim_get_tracking_drift(a), drift));
    ASSERT_TRUE(sim_set_tracking_drift(a, zm::POSE_IDENTITY));
    EXPECT_TRUE(zm::equal(sim_get_tracking_drift(a), zm::POSE_IDENTITY));
}

TEST(Sim, tracking_drift_is_part_of_the_replayed_state)
{
    SIM_Sim* live = make_sim();
    defer { sim_destroy(live); };
    step(live);
    const u64 undrifted = sim_state_hash(live);
    SIM_Snapshot initial = {};
    sim_capture_snapshot(live, initial);

    SIM_FrameState frames[12] = {};
    u64            hashes[12] = {};
    for (u32 i = 0; i < 12; i++)
    {
        if (i == 2)
        {
            ASSERT_TRUE(sim_set_tracking_drift(live, yaw_pose(5.0f, { 0.1f, 0.0f, 0.0f })));
        }
        if (i == 4)
        {
            ASSERT_TRUE(sim_set_tracking_drift_rate(live, { 0.0f, 0.01f, 0.03f }, -2.0f));
        }
        step(live);
        sim_capture_state(live, frames[i]);
        hashes[i] = sim_state_hash(live);
    }
    SIM_Sim* plain = make_sim();
    defer { sim_destroy(plain); };
    step(plain);
    EXPECT_EQ(sim_state_hash(plain), undrifted);
    ASSERT_TRUE(sim_set_tracking_drift(plain, yaw_pose(0.0f, { 0.0f, 0.0f, 0.001f })));
    EXPECT_NE(sim_state_hash(plain), undrifted);

    SIM_Sim* replay = make_sim();
    defer { sim_destroy(replay); };
    sim_set_controls_enabled(replay, false);
    ASSERT_TRUE(sim_set_tracking_drift_rate(replay, { 1.0f, 0.0f, 0.0f }, 90.0f)); // a replay follows the recorded drift, never its own rate
    sim_restore_snapshot(replay, initial);
    for (u32 i = 0; i < 12; i++)
    {
        sim_replay_frame(replay, frames[i]);
        sim_commit_frame(replay);
        EXPECT_EQ(sim_state_hash(replay), hashes[i]) << "frame " << i;
    }
    const Pose recorded = sim_get_tracking_drift(live);
    const Pose replayed = sim_get_tracking_drift(replay);
    EXPECT_TRUE(memory_equal(&recorded, &replayed, sizeof(Pose)));
}

TEST(Sim, drift_rate_from_config_and_environment)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_TRUE(zm::equal(config.drift_velocity, Vec3{}));
    EXPECT_EQ(config.drift_yaw_rate, 0.0f);

    ASSERT_TRUE(sim_parse_config(config, R"({ "drift_rate": { "yaw_degrees": 1.5, "velocity": [0.01, 0, -0.02] } })"));
    EXPECT_FLOAT_EQ(config.drift_yaw_rate, 1.5f);
    EXPECT_TRUE(zm::equal(config.drift_velocity, Vec3{ 0.01f, 0.0f, -0.02f }));
    ASSERT_TRUE(sim_parse_config(config, R"({ "drift_rate": { "velocity": [1, 2] } })"));
    EXPECT_TRUE(zm::equal(config.drift_velocity, Vec3{ 0.01f, 0.0f, -0.02f })) << "a malformed velocity is ignored";

    os_set_env("PUPPETXR_DRIFT_RATE", "-2,0.5,0,0.25");
    sim_apply_env(config);
    EXPECT_FLOAT_EQ(config.drift_yaw_rate, -2.0f);
    EXPECT_TRUE(zm::equal(config.drift_velocity, Vec3{ 0.5f, 0.0f, 0.25f }));
    os_set_env("PUPPETXR_DRIFT_RATE", "fast");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_DRIFT_RATE");
    EXPECT_FLOAT_EQ(config.drift_yaw_rate, -2.0f) << "a malformed value is ignored";

    SIM_Sim* sim = sim_create(config);
    defer { sim_destroy(sim); };
    Vec3 velocity = {};
    f32  yaw_rate = 0.0f;
    sim_get_tracking_drift_rate(sim, velocity, yaw_rate);
    EXPECT_TRUE(zm::equal(velocity, Vec3{ 0.5f, 0.0f, 0.25f }));
    EXPECT_FLOAT_EQ(yaw_rate, -2.0f);
}

TEST(Sim, anchor_store_config_from_json_and_environment)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_STREQ(config.anchors_dir, "");
    EXPECT_STREQ(config.cloud_dir, "");
    EXPECT_STREQ(config.app_id, "");
    EXPECT_EQ(config.user_id, 1u);
    EXPECT_EQ(config.anchor_latency_frames, 0u);
    EXPECT_EQ(config.anchor_localize_frames, 0u);

    ASSERT_TRUE(sim_parse_config(config, R"({ "anchors": { "dir": "/a", "cloud_dir": "/c", "app_id": "com.example.game", "user_id": 7, "latency_frames": 3, "localize_frames": 10 } })"));
    EXPECT_STREQ(config.anchors_dir, "/a");
    EXPECT_STREQ(config.cloud_dir, "/c");
    EXPECT_STREQ(config.app_id, "com.example.game");
    EXPECT_EQ(config.user_id, 7u);
    EXPECT_EQ(config.anchor_latency_frames, 3u);
    EXPECT_EQ(config.anchor_localize_frames, 10u);

    ASSERT_TRUE(sim_parse_config(config, R"({ "anchors": { "user_id": 0 } })"));
    EXPECT_EQ(config.user_id, 7u) << "user 0 is not a user";

    os_set_env("PUPPETXR_ANCHORS_DIR", "/x");
    os_set_env("PUPPETXR_CLOUD_DIR", "/y");
    os_set_env("PUPPETXR_APP_ID", "other");
    os_set_env("PUPPETXR_USER_ID", "12345678901");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_ANCHORS_DIR");
    os_unset_env("PUPPETXR_CLOUD_DIR");
    os_unset_env("PUPPETXR_APP_ID");
    os_unset_env("PUPPETXR_USER_ID");
    EXPECT_STREQ(config.anchors_dir, "/x");
    EXPECT_STREQ(config.cloud_dir, "/y");
    EXPECT_STREQ(config.app_id, "other");
    EXPECT_EQ(config.user_id, 12345678901ull);
}

TEST(Sim, keyboard_tracking_setting_from_json_and_environment)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_TRUE(config.keyboard_tracking) << "on, so apps find the keyboard props";
    ASSERT_TRUE(sim_parse_config(config, R"({ "keyboard_tracking": { "enabled": false } })"));
    EXPECT_FALSE(config.keyboard_tracking);
    sim_default_config(config);
    os_set_env("PUPPETXR_KEYBOARD_TRACKING", "0");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_KEYBOARD_TRACKING");
    EXPECT_FALSE(config.keyboard_tracking);
}

// Meta's OVRPlugin refuses runtimes not named like Meta's, so apps built on it need another name
TEST(Sim, accept_time_zero_from_json_and_environment)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_FALSE(config.accept_time_zero);
    ASSERT_TRUE(sim_parse_config(config, R"({ "accept_time_zero": true })"));
    EXPECT_TRUE(config.accept_time_zero);
    sim_default_config(config);
    os_set_env("PUPPETXR_ACCEPT_TIME_ZERO", "1");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_ACCEPT_TIME_ZERO");
    EXPECT_TRUE(config.accept_time_zero);
}

TEST(Sim, runtime_name_from_json_and_environment)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_STREQ(config.runtime_name, "puppetxr");

    ASSERT_TRUE(sim_parse_config(config, R"({ "runtime_name": "Oculus" })"));
    EXPECT_STREQ(config.runtime_name, "Oculus");
    ASSERT_TRUE(sim_parse_config(config, R"({ "runtime_name": "" })"));
    EXPECT_STREQ(config.runtime_name, "Oculus") << "an empty name keeps the previous one";

    os_set_env("PUPPETXR_RUNTIME_NAME", "Other");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_RUNTIME_NAME");
    EXPECT_STREQ(config.runtime_name, "Other");
}

// Apps tell Quest models apart by the system name (OVRPlugin's headset type); each preset is
// a model as Meta's runtime presents it, and keys set in the config still win
TEST(Sim, device_presets_set_the_headset_and_explicit_keys_win)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_STREQ(config.device, "quest3");
    EXPECT_STREQ(config.system_name, "Meta Quest 3");
    EXPECT_EQ(config.vendor_id, 0x2833u);
    EXPECT_EQ(config.headset_id[0], 0x64) << "Meta's id of a Quest 3 on Link";
    EXPECT_EQ(config.headset_id[15], 0x76);
    EXPECT_EQ(config.view_width, 1680u);
    EXPECT_EQ(config.view_height, 1760u);
    EXPECT_EQ(config.controller, SIM_Controller::MetaTouchPlus);
    EXPECT_EQ(config.refresh_rates_count, 4u);

    ASSERT_TRUE(sim_apply_device(config, "quest2"));
    EXPECT_STREQ(config.system_name, "Oculus Quest2");
    EXPECT_EQ(config.headset_id[0], 0x52);
    EXPECT_EQ(config.view_width, 1440u);
    EXPECT_EQ(config.view_height, 1584u);
    EXPECT_EQ(config.controller, SIM_Controller::OculusTouch);

    ASSERT_TRUE(sim_apply_device(config, "questpro"));
    EXPECT_STREQ(config.system_name, "Meta Quest Pro");
    EXPECT_EQ(config.controller, SIM_Controller::MetaTouchPro);
    EXPECT_EQ(config.refresh_rates_count, 2u);
    b8 has_rate = false;
    for (u32 i = 0; i < config.refresh_rates_count; i++)
    {
        has_rate |= config.refresh_rates[i] == config.refresh_rate_hz;
    }
    EXPECT_TRUE(has_rate) << "every preset offers the current rate";

    ASSERT_TRUE(sim_apply_device(config, "quest3s"));
    EXPECT_STREQ(config.system_name, "Meta Quest 3S");
    EXPECT_EQ(config.controller, SIM_Controller::MetaTouchPlus);
    EXPECT_FALSE(sim_apply_device(config, "quest4"));
    EXPECT_STREQ(config.device, "quest3s") << "an unknown preset changes nothing";
    EXPECT_GE(sim_device_count(), 4u);

    sim_default_config(config);
    ASSERT_TRUE(sim_parse_config(config, R"({ "views": { "width": 1000 }, "device": "quest2", "system_name": "Custom" })"));
    EXPECT_STREQ(config.device, "quest2");
    EXPECT_EQ(config.view_width, 1000u) << "the file's own keys win, wherever the device is named";
    EXPECT_EQ(config.view_height, 1584u);
    EXPECT_STREQ(config.system_name, "Custom");
}

TEST(Sim, device_from_the_environment_keeps_the_files_keys)
{
    char dir[OS_MAX_PATH];
    char path[OS_MAX_PATH];
    ASSERT_TRUE(os_create_temp_directory("puppetxr_device", dir, sizeof(dir)));
    defer { os_delete_tree(dir); };
    cstr_format(path, sizeof(path), "%s/puppetxr.json", dir);
    const char* json = R"({ "device": "quest2", "views": { "height": 1000 } })";
    ASSERT_TRUE(os_write_file(path, json, cstr_length(json)));
    os_set_env("PUPPETXR_CONFIG", path);
    os_set_env("PUPPETXR_DEVICE", "questpro");
    SIM_Config config = {};
    sim_load_full_config(config);
    os_unset_env("PUPPETXR_CONFIG");
    os_unset_env("PUPPETXR_DEVICE");
    EXPECT_STREQ(config.device, "questpro");
    EXPECT_STREQ(config.system_name, "Meta Quest Pro");
    EXPECT_EQ(config.view_height, 1000u);
}

static b8 ovrplugin_loaded (const char* module)
{
    return cstr_equal(module, "OVRPlugin.dll") || cstr_equal(module, "libOVRPlugin.so");
}

static b8 nothing_loaded (const char*)
{
    return false;
}

static b8 macos_ovrplugin_loaded (const char* module)
{
    return cstr_equal(module, "OVRPlugin.dylib");
}

static b8 macos_mruk_shared_loaded (const char* module)
{
    return cstr_equal(module, "libmrutilitykitshared.dylib");
}

// Unity's macOS players carry the plugins as dylibs
TEST(Sim, known_modules_are_recognised_under_their_macos_names)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_NE(sim_apply_known_modules(config, macos_ovrplugin_loaded), nullptr);
    EXPECT_STREQ(config.runtime_name, "Oculus");
    EXPECT_TRUE(config.accept_time_zero);
    EXPECT_TRUE(config.discover_before_running);

    sim_default_config(config);
    EXPECT_NE(sim_apply_known_modules(config, macos_mruk_shared_loaded), nullptr);
    EXPECT_STREQ(config.runtime_name, "puppetxr");
    EXPECT_FALSE(config.accept_time_zero);
    EXPECT_TRUE(config.discover_before_running);
}

// Apps on Meta's OVRPlugin run without a config file, which still overrides what was detected
TEST(Sim, known_modules_set_defaults_the_config_overrides)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_EQ(sim_apply_known_modules(config, nothing_loaded), nullptr);
    EXPECT_STREQ(config.runtime_name, "puppetxr");
    EXPECT_FALSE(config.accept_time_zero);
    EXPECT_FALSE(config.discover_before_running);

    const char* module = sim_apply_known_modules(config, ovrplugin_loaded);
    ASSERT_NE(module, nullptr);
    EXPECT_TRUE(ovrplugin_loaded(module));
    EXPECT_STREQ(config.runtime_name, "Oculus");
    EXPECT_TRUE(config.accept_time_zero);
    EXPECT_TRUE(config.discover_before_running);

    ASSERT_TRUE(sim_parse_config(config, R"({ "runtime_name": "puppetxr", "accept_time_zero": false, "discover_before_running": false })"));
    EXPECT_STREQ(config.runtime_name, "puppetxr");
    EXPECT_FALSE(config.accept_time_zero);
    EXPECT_FALSE(config.discover_before_running);

    sim_apply_known_modules(config, ovrplugin_loaded);
    os_set_env("PUPPETXR_ACCEPT_TIME_ZERO", "0");
    os_set_env("PUPPETXR_DISCOVER_BEFORE_RUNNING", "0");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_ACCEPT_TIME_ZERO");
    os_unset_env("PUPPETXR_DISCOVER_BEFORE_RUNNING");
    EXPECT_FALSE(config.accept_time_zero);
    EXPECT_FALSE(config.discover_before_running);

    sim_default_config(config);
    os_set_env("PUPPETXR_DISCOVER_BEFORE_RUNNING", "1");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_DISCOVER_BEFORE_RUNNING");
    EXPECT_TRUE(config.discover_before_running);
}

static b8 meta_unreal_plugin_loaded (const char* module)
{
    return cstr_equal(module, "UnrealEditor-OculusXRHMD.dll");
}

// Meta's Unreal plugin asks for layer resolutions with an older frame's time, on Unreal's OpenXR
// plugin without OVRPlugin, so the runtime keeps its name
TEST(Sim, meta_unreal_plugin_accepts_older_frame_times)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_FALSE(config.accept_older_frame_time);
    EXPECT_NE(sim_apply_known_modules(config, meta_unreal_plugin_loaded), nullptr);
    EXPECT_TRUE(config.accept_older_frame_time);
    EXPECT_STREQ(config.runtime_name, "puppetxr");
    EXPECT_FALSE(config.accept_time_zero);

    sim_default_config(config);
    sim_apply_known_modules(config, ovrplugin_loaded);
    EXPECT_FALSE(config.accept_older_frame_time) << "Unity's OVRPlugin asks with the waited frame's time";

    ASSERT_TRUE(sim_parse_config(config, R"({ "accept_older_frame_time": true })"));
    EXPECT_TRUE(config.accept_older_frame_time);
    sim_apply_known_modules(config, meta_unreal_plugin_loaded);
    os_set_env("PUPPETXR_ACCEPT_OLDER_FRAME_TIME", "0");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_ACCEPT_OLDER_FRAME_TIME");
    EXPECT_FALSE(config.accept_older_frame_time);
}

static b8 mruk_shared_loaded (const char* module)
{
    return cstr_equal(module, "mrutilitykitshared.dll") || cstr_equal(module, "libmrutilitykitshared.so");
}

// MRUK's shared library discovers before the session runs on Unreal's OpenXR plugin too, without
// OVRPlugin, so the runtime keeps its name and time 0 stays invalid
TEST(Sim, mruk_shared_library_only_discovers_before_running)
{
    SIM_Config config = {};
    sim_default_config(config);
    const char* module = sim_apply_known_modules(config, mruk_shared_loaded);
    ASSERT_NE(module, nullptr);
    EXPECT_TRUE(mruk_shared_loaded(module));
    EXPECT_STREQ(config.runtime_name, "puppetxr");
    EXPECT_FALSE(config.accept_time_zero);
    EXPECT_TRUE(config.discover_before_running);
}

// Unreal loads MRUK's library after xrCreateInstance read the config; discovery looks again,
// unless the file or the environment chose
TEST(Sim, a_module_loaded_after_the_config_still_discovers_unless_the_config_chose)
{
    SIM_Config config = {};
    sim_default_config(config);
    sim_apply_known_modules(config, nothing_loaded);
    EXPECT_FALSE(config.discover_before_running);
    EXPECT_EQ(sim_late_discovery_module(config, nothing_loaded), nullptr);
    const char* module = sim_late_discovery_module(config, mruk_shared_loaded);
    ASSERT_NE(module, nullptr);
    EXPECT_TRUE(mruk_shared_loaded(module));

    ASSERT_TRUE(sim_parse_config(config, R"({ "discover_before_running": false })"));
    EXPECT_EQ(sim_late_discovery_module(config, mruk_shared_loaded), nullptr);

    sim_default_config(config);
    os_set_env("PUPPETXR_DISCOVER_BEFORE_RUNNING", "0");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_DISCOVER_BEFORE_RUNNING");
    EXPECT_EQ(sim_late_discovery_module(config, mruk_shared_loaded), nullptr);
}

//
// Hands
//

static SIM_HandJoints hand_joints (const SIM_Sim* sim, SIM_Device hand, SIM_HandMotion motion = SIM_HandMotion::Unobstructed)
{
    SIM_HandJoints joints = {};
    sim_hand_joints(sim, hand, sim_get_device_pose(sim, hand), motion, joints);
    return joints;
}

static f32 joint_distance (const SIM_HandJoints& joints, SIM_HandJoint a, SIM_HandJoint b)
{
    return zm::length(joints.poses[(u32)a].position - joints.poses[(u32)b].position);
}

static Vec3 in_palm_frame (const SIM_HandJoints& joints, SIM_HandJoint joint)
{
    return zm::pose_relative(joints.poses[(u32)SIM_HandJoint::Palm], joints.poses[(u32)joint]).position;
}

TEST(Sim, hand_source_defaults_to_controller_and_has_names)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    EXPECT_EQ(sim_get_hand_source(sim, SIM_Device::LeftHand), SIM_HandSource::Controller);
    EXPECT_EQ(sim_get_hand_source(sim, SIM_Device::RightHand), SIM_HandSource::Controller);
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    EXPECT_EQ(sim_get_hand_source(sim, SIM_Device::RightHand), SIM_HandSource::Hand);
    EXPECT_EQ(sim_get_hand_source(sim, SIM_Device::LeftHand), SIM_HandSource::Controller);

    EXPECT_STREQ(sim_hand_source_name(SIM_HandSource::Controller), "controller");
    EXPECT_STREQ(sim_hand_source_name(SIM_HandSource::Hand), "hand");
    SIM_HandSource source = {};
    EXPECT_TRUE(sim_hand_source_from_name("hand", source));
    EXPECT_EQ(source, SIM_HandSource::Hand);
    EXPECT_FALSE(sim_hand_source_from_name("glove", source));
}

// A tracked hand has no buttons: pinch reads trigger/value, grab squeeze/value, the menu
// gesture menu/click, and everything else is zeroed when the source changes
TEST(Sim, tracked_hands_keep_only_gesture_inputs)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::AClick, { .value = { 1.0f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 0.5f, 0.0f } });
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    EXPECT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::AClick).value.x, 0.0f);
    EXPECT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue).value.x, 0.5f);

    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::BClick, { .value = { 1.0f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::SqueezeValue, { .value = { 0.7f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::MenuClick, { .value = { 1.0f, 0.0f } });
    EXPECT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::BClick).value.x, 0.0f);
    EXPECT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::SqueezeValue).value.x, 0.7f);
    EXPECT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::MenuClick).value.x, 1.0f);
}

TEST(Sim, open_hand_is_a_plausible_hand)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    for (const SIM_Device hand : { SIM_Device::LeftHand, SIM_Device::RightHand })
    {
        const SIM_HandJoints joints = hand_joints(sim, hand);
        for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
        {
            EXPECT_TRUE(zm::is_finite(joints.poses[j]));
            EXPECT_NEAR(zm::length(joints.poses[j].orientation), 1.0f, 1e-5f);
            EXPECT_GT(joints.radii[j], 0.004f);
            EXPECT_LT(joints.radii[j], 0.04f);
        }
        const f32 span = joint_distance(joints, SIM_HandJoint::Wrist, SIM_HandJoint::MiddleTip);
        EXPECT_GT(span, 0.17f);
        EXPECT_LT(span, 0.21f);

        // The palm joint is held where a controller's handle would be, within a few cm of the grip
        EXPECT_LT(zm::length(joints.poses[(u32)SIM_HandJoint::Palm].position - sim_get_device_pose(sim, hand).position), 0.05f);

        // Spec 12.30.4 (XR_EXT_hand_tracking): the thumb is on -X of the right hand and +X of the left
        const f32 thumb_x = in_palm_frame(joints, SIM_HandJoint::ThumbTip).x;
        EXPECT_TRUE(hand == SIM_Device::RightHand ? thumb_x < -0.03f : thumb_x > 0.03f) << thumb_x;
    }
}

// Spec XR_EXT_hand_tracking: -Z of a joint points along its bone towards the fingertip, +Y away
// from the back of the hand
TEST(Sim, finger_joints_point_along_their_bones)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };

    for (const f32 curl : { 0.0f, 0.6f, 1.0f })
    {
        sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { curl, 0.0f } });
        sim_set_input(sim, SIM_Device::RightHand, SIM_Input::SqueezeValue, { .value = { curl, 0.0f } });
        const SIM_HandJoints joints = hand_joints(sim, SIM_Device::RightHand);
        for (u32 finger = 0; finger < 4; finger++)
        {
            const u32 first = (u32)SIM_HandJoint::IndexMetacarpal + finger * 5;
            for (u32 j = first; j < first + 4; j++)
            {
                const Vec3 bone    = zm::normalize(joints.poses[j + 1].position - joints.poses[j].position);
                const Vec3 forward = zm::rotate(joints.poses[j].orientation, { 0.0f, 0.0f, -1.0f });
                EXPECT_GT(zm::dot(bone, forward), 0.999f) << "joint " << j << " curl " << curl;
            }
        }
    }
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, {});
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::SqueezeValue, {});
    const SIM_HandJoints open   = hand_joints(sim, SIM_Device::RightHand);
    const Vec3           dorsal = zm::rotate(open.poses[(u32)SIM_HandJoint::Palm].orientation, { 0.0f, 1.0f, 0.0f });
    const Vec3           grip_x = zm::rotate(sim_get_device_pose(sim, SIM_Device::RightHand).orientation, { 1.0f, 0.0f, 0.0f });
    EXPECT_GT(zm::dot(dorsal, grip_x), 0.99f) << "spec grip pose: +X of the right hand points into the palm, from its back";
}

TEST(Sim, pinch_and_fist_gestures_follow_trigger_and_squeeze)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    for (const SIM_Device hand : { SIM_Device::LeftHand, SIM_Device::RightHand })
    {
        sim_set_hand_source(sim, hand, SIM_HandSource::Hand);
        const SIM_HandJoints open = hand_joints(sim, hand);
        EXPECT_GT(joint_distance(open, SIM_HandJoint::ThumbTip, SIM_HandJoint::IndexTip), 0.05f);

        sim_set_input(sim, hand, SIM_Input::TriggerValue, { .value = { 1.0f, 0.0f } });
        const SIM_HandJoints pinch = hand_joints(sim, hand);
        EXPECT_LT(joint_distance(pinch, SIM_HandJoint::ThumbTip, SIM_HandJoint::IndexTip), 0.01f);
        EXPECT_GT(joint_distance(pinch, SIM_HandJoint::MiddleTip, SIM_HandJoint::Palm), 0.08f) << "a pinch leaves the other fingers open";

        sim_set_input(sim, hand, SIM_Input::SqueezeValue, { .value = { 1.0f, 0.0f } });
        const SIM_HandJoints fist = hand_joints(sim, hand);
        for (const SIM_HandJoint tip : { SIM_HandJoint::IndexTip, SIM_HandJoint::MiddleTip, SIM_HandJoint::RingTip, SIM_HandJoint::LittleTip })
        {
            EXPECT_LT(joint_distance(fist, tip, SIM_HandJoint::Palm), 0.065f) << (u32)tip;
        }

        sim_set_input(sim, hand, SIM_Input::TriggerValue, {});
        const SIM_HandJoints point = hand_joints(sim, hand);
        EXPECT_GT(joint_distance(point, SIM_HandJoint::IndexTip, SIM_HandJoint::Palm), 0.12f) << "grab without pinch points";
        EXPECT_LT(joint_distance(point, SIM_HandJoint::MiddleTip, SIM_HandJoint::Palm), 0.065f);
    }
}

TEST(Sim, left_hand_mirrors_the_right)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::TriggerValue, { .value = { 0.4f, 0.0f } });
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 0.4f, 0.0f } });
    const SIM_HandJoints left  = hand_joints(sim, SIM_Device::LeftHand);
    const SIM_HandJoints right = hand_joints(sim, SIM_Device::RightHand);
    for (u32 j = 0; j < SIM_HAND_JOINT_COUNT; j++)
    {
        const Vec3 l = in_palm_frame(left, (SIM_HandJoint)j);
        const Vec3 r = in_palm_frame(right, (SIM_HandJoint)j);
        EXPECT_NEAR(l.x, -r.x, 1e-5f) << j;
        EXPECT_NEAR(l.y, r.y, 1e-5f) << j;
        EXPECT_NEAR(l.z, r.z, 1e-5f) << j;
        EXPECT_EQ(left.radii[j], right.radii[j]);
    }
}

// XR_EXT_hand_joints_motion_range: a controller in the hand keeps the fingers around it
TEST(Sim, hands_conform_to_a_held_controller)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    const SIM_HandJoints free = hand_joints(sim, SIM_Device::RightHand, SIM_HandMotion::Unobstructed);
    const SIM_HandJoints held = hand_joints(sim, SIM_Device::RightHand, SIM_HandMotion::ConformingToController);
    EXPECT_GT(joint_distance(free, SIM_HandJoint::MiddleTip, SIM_HandJoint::Palm), 0.12f);
    EXPECT_LT(joint_distance(held, SIM_HandJoint::MiddleTip, SIM_HandJoint::Palm), 0.08f);

    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    const SIM_HandJoints bare = hand_joints(sim, SIM_Device::RightHand, SIM_HandMotion::ConformingToController);
    EXPECT_GT(joint_distance(bare, SIM_HandJoint::MiddleTip, SIM_HandJoint::Palm), 0.12f) << "without a controller there is nothing to conform to";
}

TEST(Sim, pinch_and_poke_poses_come_from_the_fingers)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TriggerValue, { .value = { 1.0f, 0.0f } });
    const SIM_HandJoints joints = hand_joints(sim, SIM_Device::RightHand);
    const Pose           pinch  = sim_get_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Pinch);
    const Vec3           middle = (joints.poses[(u32)SIM_HandJoint::ThumbTip].position + joints.poses[(u32)SIM_HandJoint::IndexTip].position) * 0.5f;
    EXPECT_TRUE(zm::near(pinch.position, middle, 1e-5f));
    EXPECT_TRUE(zm::near(pinch.orientation, sim_get_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Aim).orientation, 1e-6f));

    const Pose poke = sim_get_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Poke);
    EXPECT_TRUE(zm::near(poke.position, joints.poses[(u32)SIM_HandJoint::IndexTip].position, 1e-5f));

    // A controller in the hand holds the fingers around it, so its poke point is the held index tip
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Controller);
    SIM_HandJoints held = {};
    sim_hand_joints(sim, SIM_Device::RightHand, sim_get_device_pose(sim, SIM_Device::RightHand), SIM_HandMotion::ConformingToController, held);
    EXPECT_TRUE(zm::near(sim_get_hand_pose(sim, SIM_Device::RightHand, SIM_HandPose::Poke).position, held.poses[(u32)SIM_HandJoint::IndexTip].position, 1e-5f));
}

TEST(Sim, hand_mesh_is_skinned_to_the_open_hand)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    for (const SIM_Device hand : { SIM_Device::LeftHand, SIM_Device::RightHand })
    {
        const SIM_HandMesh& mesh = sim_get_hand_mesh(sim, hand);
        ASSERT_GT(mesh.vertices_count, 100u);
        ASSERT_LE(mesh.vertices_count, (u32)SIM_HAND_MESH_MAX_VERTICES);
        ASSERT_GT(mesh.indices_count, 300u);
        ASSERT_LE(mesh.indices_count, (u32)SIM_HAND_MESH_MAX_INDICES);
        EXPECT_EQ(mesh.indices_count % 3, 0u);
        for (u32 i = 0; i < mesh.indices_count; i++)
        {
            ASSERT_GE(mesh.indices[i], 0);
            ASSERT_LT((u32)mesh.indices[i], mesh.vertices_count);
        }
        for (u32 v = 0; v < mesh.vertices_count; v++)
        {
            ASSERT_LT(mesh.joints[v], (u8)SIM_HAND_JOINT_COUNT);
            EXPECT_NEAR(zm::length(mesh.normals[v]), 1.0f, 1e-4f);
            // Every vertex lies on its bone's surface, so it stays within a metacarpal's length of its joint
            const Vec3 local = zm::transform_point(zm::pose_inverse(mesh.bind_poses[mesh.joints[v]]), mesh.positions[v]);
            EXPECT_LT(zm::length(local), 0.1f) << v;
        }
        EXPECT_EQ(mesh.parents[(u32)SIM_HandJoint::Wrist], (u8)SIM_HandJoint::Wrist) << "the root is its own parent";
        EXPECT_EQ(mesh.parents[(u32)SIM_HandJoint::IndexTip], (u8)SIM_HandJoint::IndexDistal);
        EXPECT_EQ(mesh.parents[(u32)SIM_HandJoint::ThumbMetacarpal], (u8)SIM_HandJoint::Wrist);
    }
}

TEST(Sim, hand_source_key_toggles_the_active_hand)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_Config& config = sim_get_config(sim);

    sim_control_key(sim, config.key_hand_source, true);
    step(sim);
    sim_control_key(sim, config.key_hand_source, false);
    step(sim);
    EXPECT_EQ(sim_get_hand_source(sim, config.primary_hand), SIM_HandSource::Hand);
    EXPECT_EQ(sim_get_hand_source(sim, SIM_Device::LeftHand), SIM_HandSource::Controller);

    sim_control_key(sim, OS_Key::LeftShift, true);
    sim_control_key(sim, config.key_hand_source, true);
    step(sim);
    sim_control_key(sim, config.key_hand_source, false);
    sim_control_key(sim, OS_Key::LeftShift, false);
    step(sim);
    EXPECT_EQ(sim_get_hand_source(sim, SIM_Device::LeftHand), SIM_HandSource::Hand);

    // Trigger and grip slots still drive pinch and grab on a tracked hand
    sim_control_key(sim, OS_Key::U, true);
    for (u32 i = 0; i < 60; i++)
    {
        step(sim);
    }
    EXPECT_EQ(sim_get_input(sim, config.primary_hand, SIM_Input::TriggerValue).value.x, 1.0f);
}

TEST(Sim, hand_source_is_part_of_the_frame_state)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    const u64 before = sim_state_hash(sim);
    sim_set_hand_source(sim, SIM_Device::LeftHand, SIM_HandSource::Hand);
    EXPECT_NE(sim_state_hash(sim), before);

    SIM_FrameState state = {};
    sim_capture_state(sim, state);
    EXPECT_EQ(state.devices[(u32)SIM_Device::LeftHand].source, SIM_HandSource::Hand);

    SIM_Sim* other = make_sim();
    defer { sim_destroy(other); };
    sim_replay_frame(other, state);
    EXPECT_EQ(sim_get_hand_source(other, SIM_Device::LeftHand), SIM_HandSource::Hand);
}

//
// Microgestures and the hand tracking field of view
//

// The welcome overlay's controller buttons: each slot key presses its button on the profile's hand
// (the primary hand unless it names one), Shift on the left hand, Ctrl on the right; the right
// Touch has no menu, so its menu key presses the left one's. Only that hand's button moves
TEST(Sim, every_slot_key_presses_the_button_the_welcome_overlay_names)
{
    const SIM_Slot       slots[]       = { SIM_Slot::Trigger, SIM_Slot::Grip, SIM_Slot::Primary, SIM_Slot::Secondary, SIM_Slot::StickClick, SIM_Slot::Menu };
    const SIM_Controller controllers[] = { SIM_Controller::OculusTouch, SIM_Controller::KhrSimple, SIM_Controller::MetaTouchPro, SIM_Controller::MetaTouchPlus };
    const char* const    profiles[]    = { "body", "stick", "hands", "head", "left", "right" };
    const struct { u8 modifiers; OS_Key key; } chords[] = { { 0, OS_Key::None }, { SIM_MOD_SHIFT, OS_Key::LeftShift }, { SIM_MOD_CTRL, OS_Key::LeftCtrl } };
    for (SIM_Controller controller : controllers)
    {
        for (const char* profile : profiles)
        {
            for (const auto& chord : chords)
            {
                for (SIM_Slot slot : slots)
                {
                    SIM_Sim* sim = make_instant_sim();
                    defer { sim_destroy(sim); };
                    sim_set_controller(sim, controller);
                    use_profile(sim, profile);
                    const SIM_Config& config = sim_get_config(sim);
                    const b8          left   = cstr_equal(profile, "left");
                    const b8          right  = cstr_equal(profile, "right") || cstr_equal(profile, "hands");
                    SIM_Device hand = chord.modifiers == SIM_MOD_SHIFT ? SIM_Device::LeftHand : chord.modifiers == SIM_MOD_CTRL ? SIM_Device::RightHand
                                    : left ? SIM_Device::LeftHand : right ? SIM_Device::RightHand : config.primary_hand;
                    SIM_Input input = sim_controller_slot_input(controller, hand, slot);
                    if (input == SIM_Input::Count && slot == SIM_Slot::Menu)
                    {
                        hand  = hand == SIM_Device::LeftHand ? SIM_Device::RightHand : SIM_Device::LeftHand;
                        input = sim_controller_slot_input(controller, hand, slot);
                    }
                    for (u32 b = 0; b < config.bindings_count; b++)
                    {
                        const SIM_Binding& binding = config.bindings[b];
                        if (!binding.is_slot || binding.slot != slot || binding.modifiers != chord.modifiers)
                        {
                            continue;
                        }
                        SCOPED_TRACE(testing::Message() << sim_controller_name(controller) << " profile " << profile << " modifiers " << (u32)chord.modifiers << " slot " << (u32)slot << " key " << (u32)binding.key);
                        if (chord.key != OS_Key::None)
                        {
                            sim_control_key(sim, chord.key, true);
                        }
                        sim_control_key(sim, binding.key, true);
                        step(sim);
                        step(sim);
                        const SIM_Device other = hand == SIM_Device::LeftHand ? SIM_Device::RightHand : SIM_Device::LeftHand;
                        if (input != SIM_Input::Count)
                        {
                            EXPECT_GT(input_x(sim, hand, input), 0.5f) << sim_input_name(input) << " pressed";
                            if (sim_hand_has_input(SIM_HandSource::Controller, controller, other, input))
                            {
                                EXPECT_EQ(input_x(sim, other, input), 0.0f) << "the other hand's stays";
                            }
                        }
                        sim_control_key(sim, binding.key, false);
                        if (chord.key != OS_Key::None)
                        {
                            sim_control_key(sim, chord.key, false);
                        }
                        step(sim);
                        step(sim);
                        if (input != SIM_Input::Count)
                        {
                            EXPECT_EQ(input_x(sim, hand, input), 0.0f) << "released";
                        }
                    }
                }
            }
        }
    }
}

// The left Touch has X, Y and the menu button, the right one A, B and the system button, as their
// interaction profiles bind them; the UI showed all on both and the other hand's did nothing
TEST(Sim, touch_buttons_exist_on_their_own_hand)
{
    const SIM_Input left_only[]  = { SIM_Input::XClick, SIM_Input::XTouch, SIM_Input::YClick, SIM_Input::YTouch, SIM_Input::MenuClick };
    const SIM_Input right_only[] = { SIM_Input::AClick, SIM_Input::ATouch, SIM_Input::BClick, SIM_Input::BTouch, SIM_Input::SystemClick };
    const SIM_Controller touches[] = { SIM_Controller::OculusTouch, SIM_Controller::MetaTouchPro, SIM_Controller::MetaTouchPlus };
    for (SIM_Controller controller : touches)
    {
        for (SIM_Input input : left_only)
        {
            EXPECT_TRUE(sim_hand_has_input(SIM_HandSource::Controller, controller, SIM_Device::LeftHand, input)) << sim_input_name(input);
            EXPECT_FALSE(sim_hand_has_input(SIM_HandSource::Controller, controller, SIM_Device::RightHand, input)) << sim_input_name(input);
        }
        for (SIM_Input input : right_only)
        {
            EXPECT_FALSE(sim_hand_has_input(SIM_HandSource::Controller, controller, SIM_Device::LeftHand, input)) << sim_input_name(input);
            EXPECT_TRUE(sim_hand_has_input(SIM_HandSource::Controller, controller, SIM_Device::RightHand, input)) << sim_input_name(input);
        }
        EXPECT_TRUE(sim_hand_has_input(SIM_HandSource::Controller, controller, SIM_Device::LeftHand, SIM_Input::TriggerValue));
        EXPECT_TRUE(sim_hand_has_input(SIM_HandSource::Controller, controller, SIM_Device::RightHand, SIM_Input::Thumbstick));
    }
    EXPECT_TRUE(sim_hand_has_input(SIM_HandSource::Controller, SIM_Controller::KhrSimple, SIM_Device::LeftHand, SIM_Input::MenuClick));
    EXPECT_TRUE(sim_hand_has_input(SIM_HandSource::Controller, SIM_Controller::KhrSimple, SIM_Device::RightHand, SIM_Input::MenuClick));
    EXPECT_TRUE(sim_hand_has_input(SIM_HandSource::Hand, SIM_Controller::MetaTouchPlus, SIM_Device::RightHand, SIM_Input::MenuClick)) << "a tracked hand's menu gesture";

    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    sim_set_input(sim, SIM_Device::LeftHand, SIM_Input::AClick, { .value = { 1.0f, 0.0f } });
    EXPECT_EQ(sim_get_input(sim, SIM_Device::LeftHand, SIM_Input::AClick).value.x, 0.0f);
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::AClick, { .value = { 1.0f, 0.0f } });
    EXPECT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::AClick).value.x, 1.0f);
}

TEST(Sim, microgestures_exist_only_on_tracked_hands)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    const SIM_Input gestures[] = { SIM_Input::SwipeLeft, SIM_Input::SwipeRight, SIM_Input::SwipeForward, SIM_Input::SwipeBackward, SIM_Input::TapThumb };
    const char*     names[]    = { "swipe_left_meta/click", "swipe_right_meta/click", "swipe_forward_meta/click", "swipe_backward_meta/click", "tap_thumb_meta/click" };
    for (u32 i = 0; i < ARRAY_SIZE(gestures); i++)
    {
        EXPECT_STREQ(sim_input_name(gestures[i]), names[i]);
        EXPECT_EQ(sim_input_type(gestures[i]), SIM_InputType::Bool);
        EXPECT_FALSE(sim_hand_has_input(SIM_HandSource::Controller, SIM_Controller::MetaTouchPlus, SIM_Device::RightHand, gestures[i]));
        EXPECT_TRUE(sim_hand_has_input(SIM_HandSource::Hand, SIM_Controller::MetaTouchPlus, SIM_Device::RightHand, gestures[i]));
    }
    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TapThumb, { .value = { 1.0f, 0.0f } });
    EXPECT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::TapThumb).value.x, 0.0f) << "a controller has no microgestures";
}

// A microgesture is a pulse: set between frames it lasts through the next frame and is gone
// after it, as the app sees it from one xrWaitFrame to the next
TEST(Sim, microgestures_are_one_frame_pulses)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);

    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::SwipeLeft, { .value = { 1.0f, 0.0f } });
    step(sim);
    EXPECT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::SwipeLeft).value.x, 1.0f);
    step(sim);
    EXPECT_EQ(sim_get_input(sim, SIM_Device::RightHand, SIM_Input::SwipeLeft).value.x, 0.0f);
}

TEST(Sim, stick_and_primary_keys_make_microgestures_on_tracked_hands)
{
    SIM_Sim* sim = make_instant_sim();
    defer { sim_destroy(sim); };
    const SIM_Device hand = SIM_Device::RightHand;
    sim_set_hand_source(sim, hand, SIM_HandSource::Hand);
    use_profile(sim, "stick");

    struct KeyGesture
    {
        OS_Key    key;
        SIM_Input gesture;
    };
    const KeyGesture cases[] = {
        { OS_Key::ArrowLeft,  SIM_Input::SwipeLeft },
        { OS_Key::ArrowRight, SIM_Input::SwipeRight },
        { OS_Key::ArrowUp,    SIM_Input::SwipeForward },
        { OS_Key::ArrowDown,  SIM_Input::SwipeBackward },
        { OS_Key::H,          SIM_Input::TapThumb },
    };
    for (const KeyGesture& c : cases)
    {
        sim_control_key(sim, c.key, true);
        step(sim);
        EXPECT_EQ(sim_get_input(sim, hand, c.gesture).value.x, 1.0f) << sim_input_name(c.gesture);
        step(sim);
        EXPECT_EQ(sim_get_input(sim, hand, c.gesture).value.x, 0.0f) << "holding the key does not repeat " << sim_input_name(c.gesture);
        sim_control_key(sim, c.key, false);
        step(sim);
        EXPECT_EQ(sim_get_input(sim, hand, SIM_Input::Thumbstick).value.x, 0.0f);
    }
}

TEST(Sim, microgestures_move_the_thumb_briefly)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    step(sim);
    const Pose grip = sim_get_device_pose(sim, SIM_Device::RightHand);
    SIM_HandJoints rest = {};
    sim_hand_joints(sim, SIM_Device::RightHand, grip, SIM_HandMotion::Unobstructed, rest);

    sim_set_input(sim, SIM_Device::RightHand, SIM_Input::TapThumb, { .value = { 1.0f, 0.0f } });
    step(sim);
    step(sim);
    SIM_HandJoints tapping = {};
    sim_hand_joints(sim, SIM_Device::RightHand, grip, SIM_HandMotion::Unobstructed, tapping);
    const u32 tip = (u32)SIM_HandJoint::ThumbTip;
    EXPECT_GT(zm::length(tapping.poses[tip].position - rest.poses[tip].position), 0.005f);

    for (u32 i = 0; i < 60; i++)
    {
        step(sim);
    }
    SIM_HandJoints after = {};
    sim_hand_joints(sim, SIM_Device::RightHand, grip, SIM_HandMotion::Unobstructed, after);
    EXPECT_TRUE(zm::near(after.poses[tip].position, rest.poses[tip].position, 1e-6f));
}

TEST(Sim, hands_leave_the_tracking_cone)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_FLOAT_EQ(config.hand_tracking_fov_degrees, 120.0f);
    {
        SIM_Sim* defaults = sim_create(config);
        defer { sim_destroy(defaults); };
        SIM_Sample start = {};
        sim_sample(defaults, sim_time(defaults), start);
        EXPECT_TRUE(sim_hand_in_view(defaults, start, SIM_Device::LeftHand)) << "the default hands are in view";
        EXPECT_TRUE(sim_hand_in_view(defaults, start, SIM_Device::RightHand));
    }
    os_set_env("PUPPETXR_HAND_TRACKING_FOV", "70");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_HAND_TRACKING_FOV");
    EXPECT_FLOAT_EQ(config.hand_tracking_fov_degrees, 70.0f);
    ASSERT_TRUE(sim_parse_config(config, R"({ "hands": { "tracking_fov_degrees": 90 } })"));
    EXPECT_FLOAT_EQ(config.hand_tracking_fov_degrees, 90.0f);
    config.clock_mode = SIM_ClockMode::FixedStep;
    SIM_Sim* sim = sim_create(config);
    defer { sim_destroy(sim); };

    // 40 degrees to the side is inside a 90 degree cone, 50 degrees is outside
    const Pose head = sim_get_device_pose(sim, SIM_Device::Head);
    Pose       side = sim_get_device_pose(sim, SIM_Device::RightHand);
    side.position   = head.position + Vec3{ 0.4f * zm::sin(40.0f * zm::DEG_TO_RAD), 0.0f, -0.4f * zm::cos(40.0f * zm::DEG_TO_RAD) };
    sim_set_device_pose(sim, SIM_Device::RightHand, side);
    step(sim);
    SIM_Sample sample = {};
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    EXPECT_TRUE(sim_hand_in_view(sim, sample, SIM_Device::RightHand));

    side.position = head.position + Vec3{ 0.4f * zm::sin(50.0f * zm::DEG_TO_RAD), 0.0f, -0.4f * zm::cos(50.0f * zm::DEG_TO_RAD) };
    sim_set_device_pose(sim, SIM_Device::RightHand, side);
    step(sim);
    sim_sample(sim, sim_predicted_display_time(sim), sample);
    EXPECT_FALSE(sim_hand_in_view(sim, sample, SIM_Device::RightHand));
}

//
// Detached controllers (XR_META_detached_controllers)
//

TEST(Sim, a_put_down_controller_stays_where_it_was)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    const Pose held = sim_get_device_pose(sim, SIM_Device::LeftHand);
    sim_set_hand_source(sim, SIM_Device::LeftHand, SIM_HandSource::Hand);
    EXPECT_TRUE(zm::equal(sim_get_detached_pose(sim, SIM_Device::LeftHand), held));

    Pose moved = held;
    moved.position.y += 0.3f;
    sim_set_device_pose(sim, SIM_Device::LeftHand, moved);
    step(sim);
    EXPECT_TRUE(zm::equal(sim_get_detached_pose(sim, SIM_Device::LeftHand), held)) << "the hand moves on, the controller stays";

    // Picking it up again puts it in the hand; putting it down again drops it there
    sim_set_hand_source(sim, SIM_Device::LeftHand, SIM_HandSource::Controller);
    sim_set_hand_source(sim, SIM_Device::LeftHand, SIM_HandSource::Hand);
    EXPECT_TRUE(zm::near(sim_get_detached_pose(sim, SIM_Device::LeftHand), moved, 1e-6f));

    const Pose table = { { 0.5f, 0.8f, -0.5f }, zm::QUAT_IDENTITY };
    sim_set_detached_pose(sim, SIM_Device::LeftHand, table);
    EXPECT_TRUE(zm::equal(sim_get_detached_pose(sim, SIM_Device::LeftHand), table));
}

TEST(Sim, detached_poses_are_part_of_the_frame_state)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };

    sim_set_hand_source(sim, SIM_Device::RightHand, SIM_HandSource::Hand);
    const u64  before = sim_state_hash(sim);
    const Pose table  = { { 0.5f, 0.8f, -0.5f }, zm::QUAT_IDENTITY };
    sim_set_detached_pose(sim, SIM_Device::RightHand, table);
    EXPECT_NE(sim_state_hash(sim), before);

    SIM_FrameState state = {};
    sim_capture_state(sim, state);
    EXPECT_TRUE(zm::equal(state.devices[(u32)SIM_Device::RightHand].detached, table));
    SIM_Sim* other = make_sim();
    defer { sim_destroy(other); };
    sim_replay_frame(other, state);
    EXPECT_TRUE(zm::equal(sim_get_detached_pose(other, SIM_Device::RightHand), table));
}

//
// Controller render models (XR_FB_render_model)
//

TEST(Sim, controller_models_are_closed_meshes_in_grip_space)
{
    for (u32 c = 0; c < (u32)SIM_Controller::Count; c++)
    {
        for (const SIM_Device hand : { SIM_Device::LeftHand, SIM_Device::RightHand })
        {
            SIM_RenderModel model = {};
            sim_controller_model((SIM_Controller)c, hand, model);
            ASSERT_GT(model.parts_count, 1u) << c;
            u32 vertices = 0;
            for (u32 p = 0; p < model.parts_count; p++)
            {
                const SIM_ModelPart& part = model.parts[p];
                ASSERT_EQ(part.indices_count % 3, 0u);
                for (u32 i = part.indices_first; i < part.indices_first + part.indices_count; i++)
                {
                    ASSERT_GE(model.indices[i], part.vertices_first);
                    ASSERT_LT(model.indices[i], part.vertices_first + part.vertices_count);
                }
                vertices += part.vertices_count;
                EXPECT_GT(part.color.w, 0.0f);
            }
            ASSERT_EQ(vertices, model.vertices_count);
            for (u32 v = 0; v < model.vertices_count; v++)
            {
                // Spec XR_FB_render_model: a controller's origin is its grip pose; the controller fits in a 20 cm ball around it
                EXPECT_LT(zm::length(model.positions[v]), 0.2f);
                EXPECT_NEAR(zm::length(model.normals[v]), 1.0f, 1e-4f);
            }
        }
    }

    // Touch controllers carry their tracking ring, the left one on its left: grip +X points out of
    // the back of the right hand and into the back of the left one, so the models mirror
    SIM_RenderModel left  = {};
    SIM_RenderModel right = {};
    sim_controller_model(SIM_Controller::OculusTouch, SIM_Device::LeftHand, left);
    sim_controller_model(SIM_Controller::OculusTouch, SIM_Device::RightHand, right);
    ASSERT_EQ(left.vertices_count, right.vertices_count);
    for (u32 v = 0; v < left.vertices_count; v++)
    {
        EXPECT_NEAR(left.positions[v].x, -right.positions[v].x, 1e-6f);
        EXPECT_NEAR(left.positions[v].y, right.positions[v].y, 1e-6f);
    }
    SIM_RenderModel plus = {};
    sim_controller_model(SIM_Controller::MetaTouchPlus, SIM_Device::RightHand, plus);
    EXPECT_LT(plus.vertices_count, right.vertices_count) << "Touch Plus has no ring";
}

TEST(Sim, keyboard_model_is_centred_on_its_bounding_box)
{
    SIM_RenderModel model = {};
    sim_keyboard_model(model);
    ASSERT_GE(model.parts_count, 2u);
    Vec3 low  = model.positions[0];
    Vec3 high = low;
    for (u32 v = 0; v < model.vertices_count; v++)
    {
        low  = zm::min(low, model.positions[v]);
        high = zm::max(high, model.positions[v]);
        EXPECT_NEAR(zm::length(model.normals[v]), 1.0f, 1e-4f);
    }
    // Spec XR_FB_render_model: a keyboard's origin is the centre of its bounding box; glTF +Y is up
    EXPECT_TRUE(zm::near(low + high, Vec3{}, 1e-5f));
    const Vec3 size = high - low;
    EXPECT_GT(size.x, 0.25f);
    EXPECT_LT(size.x, 0.45f);
    EXPECT_LT(size.y, 0.04f);
    EXPECT_GT(size.z, 0.08f);
    EXPECT_LT(size.z, 0.2f);
    EXPECT_GT(model.parts[1].vertices_count / 24, 50u) << "a key cap is a box of 24 vertices";
}

//
// Virtual keyboard (XR_META_virtual_keyboard)
//

TEST(Sim, virtual_keyboard_has_every_key_once_on_its_panel)
{
    SIM_VkbLayout layout = {};
    sim_vkb_layout(layout);
    const char* const names[] = { "a", "z", "0", "9", ",", ".", "backspace", "enter", "shift", "space" };
    for (const char* name : names)
    {
        EXPECT_GE(sim_vkb_find_key(layout, name), 0) << name;
    }
    EXPECT_EQ(sim_vkb_find_key(layout, "tab"), -1);
    EXPECT_EQ(sim_vkb_find_key(layout, ""), -1);
    EXPECT_EQ(layout.keys_count, 26u + 10u + 2u + 4u);
    for (u32 k = 0; k < layout.keys_count; k++)
    {
        const SIM_VkbKey& key = layout.keys[k];
        EXPECT_LE(zm::abs(key.center.x) + key.half.x, layout.half.x);
        EXPECT_LE(zm::abs(key.center.y) + key.half.y, layout.half.y);
        EXPECT_EQ(sim_vkb_key_at(layout, key.center), (i32)k) << "keys do not overlap";
        EXPECT_LE(key.cell_x + key.cell_width, (u32)SIM_VKB_TEXTURE_WIDTH);
        EXPECT_LE(key.cell_y + 32, (u32)SIM_VKB_TEXTURE_HEIGHT);
    }
    EXPECT_EQ(sim_vkb_key_at(layout, { layout.half.x + 0.01f, 0.0f }), -1);
    const SIM_VkbKey& q = layout.keys[sim_vkb_find_key(layout, "q")];
    EXPECT_EQ(q.lower, 'q');
    EXPECT_EQ(q.upper, 'Q');
    EXPECT_GT(layout.half.x, 0.15f);
    EXPECT_LT(layout.half.x, 0.3f);
}

TEST(Sim, virtual_keyboard_labels_follow_shift)
{
    SIM_VkbLayout layout = {};
    sim_vkb_layout(layout);
    Array<u8> lower = {};
    Array<u8> upper = {};
    defer { lower.release(); };
    defer { upper.release(); };
    lower.resize(SIM_VKB_TEXTURE_WIDTH * SIM_VKB_TEXTURE_HEIGHT * 4);
    upper.resize(lower.length);
    sim_vkb_texture(layout, false, lower.data);
    sim_vkb_texture(layout, true, upper.data);

    // A digit's cell is the same either way, a letter's is not; every label has white texels
    const auto cell_equal = [&](const SIM_VkbKey& key) {
        for (u32 y = key.cell_y; y < key.cell_y + 32; y++)
        {
            const usize row = ((usize)y * SIM_VKB_TEXTURE_WIDTH + key.cell_x) * 4;
            if (!memory_equal(lower.data + row, upper.data + row, (usize)key.cell_width * 4))
            {
                return false;
            }
        }
        return true;
    };
    EXPECT_TRUE(cell_equal(layout.keys[sim_vkb_find_key(layout, "5")]));
    EXPECT_FALSE(cell_equal(layout.keys[sim_vkb_find_key(layout, "q")]));
    EXPECT_FALSE(cell_equal(layout.keys[sim_vkb_find_key(layout, "shift")])) << "an active shift is highlighted";
    for (u32 k = 0; k < layout.keys_count; k++)
    {
        const SIM_VkbKey& key   = layout.keys[k];
        u32               light = 0;
        for (u32 y = key.cell_y; y < key.cell_y + 32; y++)
        {
            for (u32 x = key.cell_x; x < key.cell_x + key.cell_width; x++)
            {
                light += lower[((usize)y * SIM_VKB_TEXTURE_WIDTH + x) * 4] > 200 ? 1u : 0u;
            }
        }
        EXPECT_GT(light, 9u) << "key " << k << " has a label";
    }
}

TEST(Sim, virtual_keyboard_model_has_a_node_per_key_then_panel_and_collision)
{
    SIM_VkbLayout layout = {};
    sim_vkb_layout(layout);
    SIM_RenderModel model = {};
    sim_vkb_model(layout, model);
    ASSERT_EQ(model.parts_count, layout.keys_count + 2);
    EXPECT_EQ(model.texture_id, (u64)SIM_VKB_TEXTURE_ID);
    EXPECT_STREQ(model.parts[layout.keys_count].name, "panel");
    EXPECT_STREQ(model.parts[layout.keys_count + 1].name, "collision");
    for (u32 p = 0; p < model.parts_count; p++)
    {
        const b8 key = p < layout.keys_count;
        EXPECT_EQ(model.parts[p].press.z < 0.0f, key) << "only keys are pressed";
    }
    for (u32 v = 0; v < model.vertices_count; v++)
    {
        EXPECT_GE(model.uvs[v].x, 0.0f);
        EXPECT_LE(model.uvs[v].x, 1.0f);
        EXPECT_GE(model.uvs[v].y, 0.0f);
        EXPECT_LE(model.uvs[v].y, 1.0f);
        EXPECT_GE(model.positions[v].z, -0.01f);
    }
    // The key's quad shows its own atlas cell: the top left corner maps to the cell's top left
    const SIM_VkbKey&    q    = layout.keys[sim_vkb_find_key(layout, "q")];
    const SIM_ModelPart& part = model.parts[sim_vkb_find_key(layout, "q")];
    for (u32 v = part.vertices_first; v < part.vertices_first + part.vertices_count; v++)
    {
        const b8 left = model.positions[v].x < q.center.x;
        const b8 top  = model.positions[v].y > q.center.y;
        EXPECT_EQ(model.uvs[v].x * SIM_VKB_TEXTURE_WIDTH < (f32)q.cell_x + 1.0f, left);
        EXPECT_EQ(model.uvs[v].y * SIM_VKB_TEXTURE_HEIGHT < (f32)q.cell_y + 1.0f, top);
    }
}

// Typed text is part of the frame like a microgesture: queued between frames, it belongs to the
// next frame only, and a replay reproduces it
TEST(Sim, typed_text_belongs_to_one_frame)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    u32 length = 0;
    EXPECT_FALSE(sim_type_text(sim, "bell\a"));
    EXPECT_FALSE(sim_type_text(sim, nullptr));
    ASSERT_TRUE(sim_type_text(sim, "hi\b"));
    ASSERT_TRUE(sim_type_text(sim, " there\n"));
    sim_typed_text(sim, length);
    EXPECT_EQ(length, 0u) << "nothing is typed before the frame";
    step(sim);
    const char* text = sim_typed_text(sim, length);
    ASSERT_EQ(length, 10u);
    EXPECT_TRUE(memory_equal(text, "hi\b there\n", length));

    SIM_FrameState state = {};
    sim_capture_state(sim, state);
    ASSERT_EQ(state.typed_length, 10u);
    SIM_Sim* other = make_sim();
    defer { sim_destroy(other); };
    sim_replay_frame(other, state);
    text = sim_typed_text(other, length);
    ASSERT_EQ(length, 10u);
    EXPECT_TRUE(memory_equal(text, "hi\b there\n", length));

    step(sim);
    sim_typed_text(sim, length);
    EXPECT_EQ(length, 0u);

    char lots[SIM_MAX_TYPED + 2] = {};
    for (u32 i = 0; i < SIM_MAX_TYPED + 1; i++)
    {
        lots[i] = 'x';
    }
    EXPECT_FALSE(sim_type_text(sim, lots)) << "more than a frame holds is refused whole";
    lots[SIM_MAX_TYPED] = 0;
    EXPECT_TRUE(sim_type_text(sim, lots));
    EXPECT_FALSE(sim_type_text(sim, "y"));
}

//
// Props: keyboards and QR codes in the room
//

static SIM_Prop qr_prop (const char* text, Vec3 position)
{
    SIM_Prop prop = { .kind = SIM_PropKind::QrCode, .pose = { position, zm::QUAT_IDENTITY }, .size = 0.1f };
    cstr_copy(prop.text, sizeof(prop.text), text);
    return prop;
}

TEST(Sim, props_come_from_the_config)
{
    SIM_Config config = {};
    sim_default_config(config);
    ASSERT_TRUE(sim_parse_config(config, R"({ "props": [
        { "kind": "keyboard", "position": [0, 0.75, -0.5], "yaw_degrees": 90 },
        { "kind": "qr", "text": "hello", "size": 0.2, "position": [1, 1.5, -2], "orientation": [0, 0, 0, 1],
          "motion": "linear", "target": [2, 1.5, -2], "speed": 0.5 },
        { "kind": "qr", "text": "spin", "position": [0, 1, -1], "motion": "circle", "radius": 0.3, "speed": 0.2 },
        { "kind": "lamp", "position": [0, 0, 0] }
    ] })"));
    ASSERT_EQ(config.props_count, 3u) << "the lamp is skipped";
    EXPECT_EQ(config.props[0].kind, SIM_PropKind::Keyboard);
    EXPECT_TRUE(zm::near(zm::rotate(config.props[0].pose.orientation, { 0.0f, 0.0f, -1.0f }), Vec3{ -1.0f, 0.0f, 0.0f }, 1e-5f)) << "yaw turns -Z towards -X";
    EXPECT_STREQ(config.props[1].text, "hello");
    EXPECT_FLOAT_EQ(config.props[1].size, 0.2f);
    EXPECT_EQ(config.props[1].motion, SIM_PropMotion::Linear);
    EXPECT_FLOAT_EQ(config.props[1].target.x, 2.0f);
    EXPECT_FLOAT_EQ(config.props[2].size, 0.1f) << "QR codes default to 10 cm";
    EXPECT_EQ(config.props[2].motion, SIM_PropMotion::Circle);

    SIM_Sim* sim = sim_create(config);
    defer { sim_destroy(sim); };
    const SIM_Prop* props = nullptr;
    ASSERT_EQ(sim_props(sim, props), 3u);
    EXPECT_EQ(props[0].id, 1u);
    EXPECT_EQ(props[2].id, 3u);

    os_set_env("PUPPETXR_PROPS", R"([{ "kind": "qr", "text": "env", "position": [0, 1, -1] }])");
    sim_apply_env(config);
    os_unset_env("PUPPETXR_PROPS");
    ASSERT_EQ(config.props_count, 1u) << "the environment replaces the config's props";
    EXPECT_STREQ(config.props[0].text, "env");
}

TEST(Sim, props_are_added_changed_and_removed)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const u64 before = sim_state_hash(sim);
    const u32 first  = sim_add_prop(sim, qr_prop("a", { 0.0f, 1.0f, -1.0f }));
    const u32 second = sim_add_prop(sim, qr_prop("b", { 0.5f, 1.0f, -1.0f }));
    EXPECT_EQ(first, 1u);
    EXPECT_EQ(second, 2u);
    EXPECT_NE(sim_state_hash(sim), before);

    SIM_Prop bad = qr_prop("c", { zm::nan(), 0.0f, 0.0f });
    EXPECT_EQ(sim_add_prop(sim, bad), 0u);
    bad = qr_prop("c", {});
    bad.pose.orientation = {};
    EXPECT_EQ(sim_add_prop(sim, bad), 0u) << "degenerate orientation";
    bad = qr_prop("c", {});
    bad.size = 0.0f;
    EXPECT_EQ(sim_add_prop(sim, bad), 0u);
    bad = qr_prop("c", {});
    bad.kind = SIM_PropKind::Count;
    EXPECT_EQ(sim_add_prop(sim, bad), 0u);
    bad        = qr_prop("c", {});
    bad.motion = SIM_PropMotion::Circle;
    EXPECT_EQ(sim_add_prop(sim, bad), 0u) << "a circle needs a radius";
    bad = qr_prop("c", {});
    for (u32 i = 0; i < SIM_MAX_PROP_TEXT; i++)
    {
        bad.text[i] = 'x';
    }
    EXPECT_EQ(sim_add_prop(sim, bad), 0u) << "unterminated text";

    SIM_Prop moved = qr_prop("a2", { 0.0f, 2.0f, -1.0f });
    EXPECT_TRUE(sim_update_prop(sim, first, moved));
    const SIM_Prop* props = nullptr;
    ASSERT_EQ(sim_props(sim, props), 2u);
    EXPECT_EQ(props[0].id, first);
    EXPECT_STREQ(props[0].text, "a2");
    EXPECT_FALSE(sim_update_prop(sim, 99, moved));
    EXPECT_TRUE(sim_remove_prop(sim, first));
    EXPECT_FALSE(sim_remove_prop(sim, first));
    ASSERT_EQ(sim_props(sim, props), 1u);
    EXPECT_EQ(props[0].id, second);
    EXPECT_EQ(sim_add_prop(sim, qr_prop("d", {})), 3u) << "ids are never reused";

    for (u32 i = 2; i < SIM_MAX_PROPS; i++)
    {
        EXPECT_NE(sim_add_prop(sim, qr_prop("f", {})), 0u);
    }
    EXPECT_EQ(sim_add_prop(sim, qr_prop("g", {})), 0u) << "full";
}

TEST(Sim, props_move_along_their_paths)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    SIM_Prop prop = qr_prop("go", { 0.0f, 1.0f, -1.0f });
    prop.motion   = SIM_PropMotion::Linear;
    prop.target   = { 1.0f, 1.0f, -1.0f };
    prop.speed    = 0.5f;
    ASSERT_NE(sim_add_prop(sim, prop), 0u);
    const SIM_Prop* props = nullptr;
    sim_props(sim, props);
    const SIM_Prop& linear = props[0];
    const i64       start  = linear.start;
    EXPECT_EQ(start, sim_time(sim));
    EXPECT_TRUE(zm::near(sim_prop_pose(linear, start).position, Vec3{ 0.0f, 1.0f, -1.0f }, 1e-6f));
    EXPECT_TRUE(zm::near(sim_prop_pose(linear, start + 1000000000).position, Vec3{ 0.5f, 1.0f, -1.0f }, 1e-5f));
    EXPECT_TRUE(zm::near(sim_prop_pose(linear, start + 2000000000).position, Vec3{ 1.0f, 1.0f, -1.0f }, 1e-5f)) << "at the far end after 1 m";
    EXPECT_TRUE(zm::near(sim_prop_pose(linear, start + 3000000000).position, Vec3{ 0.5f, 1.0f, -1.0f }, 1e-5f)) << "and back";
    EXPECT_TRUE(zm::near(sim_prop_pose(linear, start + 4000000000ll * 1000).position, Vec3{ 0.0f, 1.0f, -1.0f }, 1e-4f)) << "an hour later still exact";

    prop        = qr_prop("spin", { 0.0f, 1.0f, -1.0f });
    prop.motion = SIM_PropMotion::Circle;
    prop.radius = 0.5f;
    prop.speed  = zm::PI * 0.25f; // a quarter of the 0.5 m circle per second
    ASSERT_NE(sim_add_prop(sim, prop), 0u);
    sim_props(sim, props);
    const SIM_Prop& circle = props[1];
    const Pose      begin  = sim_prop_pose(circle, circle.start);
    const Pose      later  = sim_prop_pose(circle, circle.start + 1000000000);
    EXPECT_TRUE(zm::near(begin.position, Vec3{ 0.5f, 1.0f, -1.0f }, 1e-5f));
    EXPECT_TRUE(zm::near(later.position, Vec3{ 0.0f, 1.0f, -1.5f }, 1e-5f)) << "counter-clockwise seen from above";
    EXPECT_TRUE(zm::near(zm::rotate(later.orientation, { 1.0f, 0.0f, 0.0f }), Vec3{ 0.0f, 0.0f, -1.0f }, 1e-5f)) << "turning with the path";

    // A still prop stays; a change restarts the motion from now
    step(sim);
    step(sim);
    EXPECT_TRUE(sim_update_prop(sim, circle.id, prop));
    sim_props(sim, props);
    EXPECT_EQ(props[1].start, sim_time(sim));
}

TEST(Sim, props_are_part_of_the_frame_state)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    sim_add_prop(sim, qr_prop("a", { 0.0f, 1.0f, -1.0f }));
    const u32 removed = sim_add_prop(sim, qr_prop("b", { 0.0f, 1.0f, -1.0f }));
    sim_remove_prop(sim, removed);
    SIM_FrameState state = {};
    sim_capture_state(sim, state);
    ASSERT_TRUE(state.props_present);
    ASSERT_EQ(state.props_count, 1u);

    SIM_Sim* other = make_sim();
    defer { sim_destroy(other); };
    sim_replay_frame(other, state);
    const SIM_Prop* props = nullptr;
    ASSERT_EQ(sim_props(other, props), 1u);
    EXPECT_STREQ(props[0].text, "a");
    EXPECT_EQ(sim_add_prop(other, qr_prop("c", {})), 2u) << "ids continue after the replayed ones";

    state.props_present = false;
    sim_replay_frame(other, state);
    EXPECT_EQ(sim_props(other, props), 2u) << "a frame without props keeps them";
}

TEST(Sim, prop_names_round_trip)
{
    for (u32 k = 0; k < (u32)SIM_PropKind::Count; k++)
    {
        SIM_PropKind kind = SIM_PropKind::Count;
        ASSERT_TRUE(sim_prop_kind_from_name(sim_prop_kind_name((SIM_PropKind)k), kind));
        EXPECT_EQ((u32)kind, k);
    }
    for (u32 m = 0; m < (u32)SIM_PropMotion::Count; m++)
    {
        SIM_PropMotion motion = SIM_PropMotion::Count;
        ASSERT_TRUE(sim_prop_motion_from_name(sim_prop_motion_name((SIM_PropMotion)m), motion));
        EXPECT_EQ((u32)motion, m);
    }
    SIM_PropKind kind = {};
    EXPECT_FALSE(sim_prop_kind_from_name("lamp", kind));
    EXPECT_STREQ(sim_prop_kind_name(SIM_PropKind::QrCode), "qr");
    EXPECT_TRUE(zm::near(sim_prop_extent(qr_prop("x", {})), Vec3{ 0.1f, 0.1f, 0.0f }, 1e-6f));
    SIM_Prop keyboard = { .kind = SIM_PropKind::Keyboard, .pose = zm::POSE_IDENTITY };
    SIM_RenderModel model = {};
    sim_keyboard_model(model);
    Vec3 high = model.positions[0];
    for (u32 v = 0; v < model.vertices_count; v++)
    {
        high = zm::max(high, model.positions[v]);
    }
    EXPECT_TRUE(zm::near(sim_prop_extent(keyboard), high * 2.0f, 1e-5f)) << "a keyboard is as big as its model";
}

//
// Body tracking
//

// A standing user: eyes at `head` looking ahead, both controllers in front at waist height
static SIM_Sample body_sample (Vec3 head = { 0.0f, 1.6f, 0.0f })
{
    SIM_Sample sample = {};
    sample.devices[(u32)SIM_Device::Head]      = { .pose = { head, zm::QUAT_IDENTITY }, .active = true, .tracked = true };
    sample.devices[(u32)SIM_Device::LeftHand]  = { .pose = { head + Vec3{ -0.2f, -0.5f, -0.15f }, zm::QUAT_IDENTITY }, .active = true, .tracked = true };
    sample.devices[(u32)SIM_Device::RightHand] = { .pose = { head + Vec3{ 0.2f, -0.5f, -0.15f }, zm::QUAT_IDENTITY }, .active = true, .tracked = true };
    return sample;
}

static Vec3 joint_at (const SIM_BodyJoints& joints, SIM_BodyJoint joint)
{
    return joints.poses[(u32)joint].position;
}

static Vec3 skeleton_at (const SIM_BodySkeleton& skeleton, SIM_BodyJoint joint)
{
    return skeleton.poses[(u32)joint].position;
}

static f32 bone (const Pose* poses, SIM_BodyJoint from, SIM_BodyJoint to)
{
    return zm::length(poses[(u32)to].position - poses[(u32)from].position);
}

TEST(SimBody, names_round_trip)
{
    for (u32 j = 0; j < SIM_BODY_JOINT_COUNT; j++)
    {
        SIM_BodyJoint joint = SIM_BodyJoint::Count;
        ASSERT_TRUE(sim_body_joint_from_name(sim_body_joint_name((SIM_BodyJoint)j), joint)) << j;
        EXPECT_EQ((u32)joint, j);
    }
    EXPECT_STREQ(sim_body_joint_name(SIM_BodyJoint::LeftArmUpper), "left_arm_upper");
    EXPECT_STREQ(sim_body_joint_name(SIM_BodyJoint::RightWristTwist), "right_hand_wrist_twist");
    EXPECT_STREQ(sim_body_joint_name((SIM_BodyJoint)28), "left_hand_index_tip");
    EXPECT_STREQ(sim_body_joint_name(SIM_BodyJoint::RightBall), "right_foot_ball");
    for (u32 p = 0; p < (u32)SIM_BodyPose::Count; p++)
    {
        SIM_BodyPose pose = SIM_BodyPose::Count;
        ASSERT_TRUE(sim_body_pose_from_name(sim_body_pose_name((SIM_BodyPose)p), pose));
        EXPECT_EQ((u32)pose, p);
    }
    for (u32 l = 0; l < (u32)SIM_Limb::Count; l++)
    {
        SIM_Limb limb = SIM_Limb::Count;
        ASSERT_TRUE(sim_limb_from_name(sim_limb_name((SIM_Limb)l), limb));
        EXPECT_EQ((u32)limb, l);
    }
    SIM_BodyJoint joint = {};
    EXPECT_FALSE(sim_body_joint_from_name("tail", joint));
}

TEST(SimBody, skeleton_stands_on_the_floor_facing_forward)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    SIM_BodySkeleton skeleton = {};
    sim_body_skeleton(sim, skeleton);

    EXPECT_EQ(skeleton.parents[(u32)SIM_BodyJoint::Root], SIM_BODY_NO_PARENT);
    for (u32 j = 1; j < SIM_BODY_JOINT_COUNT; j++)
    {
        u32 at = j;
        u32 steps = 0;
        while (at != (u32)SIM_BodyJoint::Root && steps < 16)
        {
            ASSERT_LT(skeleton.parents[at], SIM_BODY_JOINT_COUNT) << sim_body_joint_name((SIM_BodyJoint)at);
            at = skeleton.parents[at];
            steps++;
        }
        EXPECT_EQ(at, (u32)SIM_BodyJoint::Root) << sim_body_joint_name((SIM_BodyJoint)j) << " reaches the root";
        EXPECT_GE(skeleton.poses[j].position.y, -1e-4f) << sim_body_joint_name((SIM_BodyJoint)j) << " above the floor";
    }
    EXPECT_TRUE(zm::near(skeleton_at(skeleton, SIM_BodyJoint::Root), Vec3{}, 1e-6f));
    EXPECT_GT(skeleton_at(skeleton, SIM_BodyJoint::Head).y, 1.4f);
    EXPECT_LT(skeleton_at(skeleton, SIM_BodyJoint::Head).y, 1.6f);
    EXPECT_LT(skeleton_at(skeleton, SIM_BodyJoint::LeftAnkle).y, 0.15f);
    EXPECT_LT(skeleton_at(skeleton, SIM_BodyJoint::LeftHandWrist).x, -0.6f) << "arms out to the sides";
    EXPECT_GT(skeleton_at(skeleton, SIM_BodyJoint::RightHandWrist).x, 0.6f);
    EXPECT_LT(skeleton_at(skeleton, SIM_BodyJoint::LeftBall).z, skeleton_at(skeleton, SIM_BodyJoint::LeftAnkle).z) << "toes point forward, -Z";
    EXPECT_NEAR(bone(skeleton.poses, SIM_BodyJoint::LeftArmUpper, SIM_BodyJoint::LeftArmLower), bone(skeleton.poses, SIM_BodyJoint::RightArmUpper, SIM_BodyJoint::RightArmLower), 1e-5f);

    SIM_BodyState state = sim_get_body(sim);
    state.height = 1.5f;
    sim_set_body(sim, state);
    SIM_BodySkeleton smaller = {};
    sim_body_skeleton(sim, smaller);
    EXPECT_NEAR(skeleton_at(smaller, SIM_BodyJoint::Head).y, skeleton_at(skeleton, SIM_BodyJoint::Head).y * 1.5f / 1.75f, 1e-3f);
}

// A bone scale lengthens the bones from its joint to the joint's children, on that side only
TEST(SimBody, bone_scale_changes_only_that_bone)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    SIM_BodySkeleton before = {};
    sim_body_skeleton(sim, before);
    const u32 generation = sim_body_generation(sim);

    SIM_BodyState state = sim_get_body(sim);
    state.bone_scales[(u32)SIM_BodyJoint::LeftArmUpper] = 1.5f;
    state.bone_scales[(u32)SIM_BodyJoint::Head]         = 2.0f;
    sim_set_body(sim, state);
    EXPECT_NE(sim_body_generation(sim), generation);
    EXPECT_EQ(sim_get_body(sim).bone_scales[(u32)SIM_BodyJoint::Head], 1.0f) << "the head has no bone of its own";
    SIM_BodySkeleton after = {};
    sim_body_skeleton(sim, after);
    EXPECT_NEAR(bone(after.poses, SIM_BodyJoint::LeftArmUpper, SIM_BodyJoint::LeftArmLower), 1.5f * bone(before.poses, SIM_BodyJoint::LeftArmUpper, SIM_BodyJoint::LeftArmLower), 1e-5f);
    EXPECT_NEAR(bone(after.poses, SIM_BodyJoint::LeftArmLower, SIM_BodyJoint::LeftHandWrist), bone(before.poses, SIM_BodyJoint::LeftArmLower, SIM_BodyJoint::LeftHandWrist), 1e-5f);
    EXPECT_NEAR(bone(after.poses, SIM_BodyJoint::RightArmUpper, SIM_BodyJoint::RightArmLower), bone(before.poses, SIM_BodyJoint::RightArmUpper, SIM_BodyJoint::RightArmLower), 1e-5f);

    state = sim_get_body(sim);
    state.bone_scales[(u32)SIM_BodyJoint::LeftArmUpper] = 10.0f;
    state.height                                        = -1.0f;
    sim_set_body(sim, state);
    EXPECT_EQ(sim_get_body(sim).bone_scales[(u32)SIM_BodyJoint::LeftArmUpper], SIM_BODY_MAX_SCALE);
    EXPECT_EQ(sim_get_body(sim).height, SIM_BODY_MIN_HEIGHT);

    const u32 settled = sim_body_generation(sim);
    sim_set_body_pose(sim, SIM_BodyPose::TPose);
    EXPECT_EQ(sim_body_generation(sim), settled) << "a pose is no shape change";
}

TEST(SimBody, bones_keep_their_lengths_in_every_pose)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    SIM_BodyState state = sim_get_body(sim);
    state.bone_scales[(u32)SIM_BodyJoint::RightLowerLeg] = 1.3f;
    state.bone_scales[(u32)SIM_BodyJoint::Chest]         = 0.8f;
    sim_set_body(sim, state);
    SIM_BodySkeleton skeleton = {};
    sim_body_skeleton(sim, skeleton);

    const Vec3 heads[] = { { 0.0f, 1.6f, 0.0f }, { 0.5f, 1.2f, -1.0f }, { 0.0f, 0.4f, 0.0f }, { 0.0f, 2.6f, 0.0f } };
    for (u32 preset = 0; preset < (u32)SIM_BodyPose::Count; preset++)
    {
        sim_set_body_pose(sim, (SIM_BodyPose)preset);
        for (u32 arms = 0; arms < 2; arms++)
        {
            state = sim_get_body(sim);
            state.limbs[(u32)SIM_Limb::LeftArm] = arms ? SIM_LimbMode::Posed : SIM_LimbMode::Follow;
            sim_set_body(sim, state);
            for (const Vec3& head : heads)
            {
                SIM_Sample sample = body_sample(head);
                sample.devices[(u32)SIM_Device::RightHand].pose.position = head + Vec3{ 1.5f, 0.3f, 0.0f };
                SIM_BodyJoints joints = {};
                sim_body_joints(sim, sample, joints);
                // The palm hangs from the wrist twist across the turning wrist, so its distance varies
                for (u32 j = 2; j < SIM_BODY_JOINT_COUNT; j++)
                {
                    if (j == (u32)SIM_BodyJoint::LeftHandPalm || j == (u32)SIM_BodyJoint::RightHandPalm)
                    {
                        continue;
                    }
                    const SIM_BodyJoint parent = (SIM_BodyJoint)skeleton.parents[j];
                    EXPECT_NEAR(bone(joints.poses, parent, (SIM_BodyJoint)j), bone(skeleton.poses, parent, (SIM_BodyJoint)j), 2e-4f) << sim_body_joint_name((SIM_BodyJoint)j) << " in " << sim_body_pose_name((SIM_BodyPose)preset) << " at height " << head.y;
                }
            }
        }
    }
}

TEST(SimBody, wrist_meets_the_controller_in_reach_and_points_at_it_beyond)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    SIM_Sample sample = body_sample();
    sample.devices[(u32)SIM_Device::RightHand].pose.position = { 2.0f, 1.4f, 0.0f };
    SIM_BodyJoints joints = {};
    sim_body_joints(sim, sample, joints);

    SIM_HandJoints hand = {};
    sim_hand_joints(sim, SIM_Device::LeftHand, sample.devices[(u32)SIM_Device::LeftHand].pose, SIM_HandMotion::Unobstructed, hand);
    EXPECT_TRUE(zm::near(joint_at(joints, SIM_BodyJoint::LeftHandWrist), hand.poses[(u32)SIM_HandJoint::Wrist].position, 1e-4f));
    const Vec3 to_elbow = zm::rotate(joints.poses[(u32)SIM_BodyJoint::LeftHandWrist].orientation, { 1.0f, 0.0f, 0.0f });
    EXPECT_GT(zm::dot(to_elbow, zm::rotate(hand.poses[(u32)SIM_HandJoint::Wrist].orientation, { 0.0f, 0.0f, 1.0f })), 0.9999f) << "Meta's left hand: +X toward the elbow";

    SIM_BodySkeleton skeleton = {};
    sim_body_skeleton(sim, skeleton);
    const f32  reach    = bone(skeleton.poses, SIM_BodyJoint::RightArmUpper, SIM_BodyJoint::RightArmLower) + bone(skeleton.poses, SIM_BodyJoint::RightArmLower, SIM_BodyJoint::RightHandWrist);
    const Vec3 shoulder = joint_at(joints, SIM_BodyJoint::RightArmUpper);
    const Vec3 wrist    = joint_at(joints, SIM_BodyJoint::RightHandWrist);
    sim_hand_joints(sim, SIM_Device::RightHand, sample.devices[(u32)SIM_Device::RightHand].pose, SIM_HandMotion::Unobstructed, hand);
    const Vec3 target   = hand.poses[(u32)SIM_HandJoint::Wrist].position;
    EXPECT_NEAR(zm::length(wrist - shoulder), reach, 1e-4f) << "the arm is straight";
    EXPECT_GT(zm::dot(zm::normalize(wrist - shoulder), zm::normalize(target - shoulder)), 0.9999f) << "and points at the controller";
    EXPECT_GT(zm::length(target - wrist), 0.5f) << "the wrist stops short of it";
}

TEST(SimBody, elbows_do_not_flip)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    SIM_BodyJoints joints = {};
    sim_body_joints(sim, body_sample(), joints);
    const Vec3 shoulder = joint_at(joints, SIM_BodyJoint::RightArmUpper);
    for (u32 plane = 0; plane < 3; plane++)
    {
        Vec3 previous = {};
        for (u32 step = 0; step <= 180; step++)
        {
            const f32  angle  = (f32)step * zm::DEG_TO_RAD;
            const f32  radius = 0.15f + 0.1f * (f32)plane;
            const Vec3 around = plane == 0 ? Vec3{ zm::cos(angle), zm::sin(angle), -0.3f } : (plane == 1 ? Vec3{ 0.2f, zm::cos(angle), -zm::sin(angle) } : Vec3{ zm::cos(angle), -0.3f, -zm::sin(angle) });
            SIM_Sample sample = body_sample();
            sample.devices[(u32)SIM_Device::RightHand].pose.position = shoulder + around * (radius * 2.0f);
            sim_body_joints(sim, sample, joints);
            const Vec3 elbow = joint_at(joints, SIM_BodyJoint::RightArmLower);
            if (step > 0)
            {
                EXPECT_LT(zm::length(elbow - previous), 0.05f) << "plane " << plane << " at " << step << " degrees";
            }
            previous = elbow;
        }
    }
}

TEST(SimBody, editing_an_arm_joint_poses_the_arm)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    sim_set_body_joint(sim, SIM_BodyJoint::LeftArmLower, zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.5f));
    EXPECT_EQ(sim_get_body(sim).limbs[(u32)SIM_Limb::LeftArm], SIM_LimbMode::Posed);
    EXPECT_EQ(sim_get_body(sim).limbs[(u32)SIM_Limb::RightArm], SIM_LimbMode::Follow);
    sim_set_body_joint(sim, SIM_BodyJoint::Chest, zm::quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, 0.1f));
    EXPECT_EQ(sim_get_body(sim).limbs[(u32)SIM_Limb::RightArm], SIM_LimbMode::Follow) << "the torso is no limb";

    const SIM_Sample sample = body_sample();
    SIM_BodyJoints   joints = {};
    sim_body_joints(sim, sample, joints);
    SIM_HandJoints hand = {};
    sim_hand_joints(sim, SIM_Device::LeftHand, sample.devices[(u32)SIM_Device::LeftHand].pose, SIM_HandMotion::Unobstructed, hand);
    EXPECT_GT(zm::length(joint_at(joints, SIM_BodyJoint::LeftHandWrist) - hand.poses[(u32)SIM_HandJoint::Wrist].position), 0.1f) << "the posed arm leaves the controller";
    sim_hand_joints(sim, SIM_Device::RightHand, sample.devices[(u32)SIM_Device::RightHand].pose, SIM_HandMotion::Unobstructed, hand);
    EXPECT_TRUE(zm::near(joint_at(joints, SIM_BodyJoint::RightHandWrist), hand.poses[(u32)SIM_HandJoint::Wrist].position, 1e-4f));

    sim_set_body_pose(sim, SIM_BodyPose::Sit);
    EXPECT_EQ(sim_get_body(sim).limbs[(u32)SIM_Limb::LeftLeg], SIM_LimbMode::Posed);
    EXPECT_EQ(sim_get_body(sim).limbs[(u32)SIM_Limb::RightLeg], SIM_LimbMode::Posed);
}

TEST(SimBody, feet_stay_on_the_floor_until_the_legs_cannot_follow)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    SIM_BodySkeleton skeleton = {};
    sim_body_skeleton(sim, skeleton);
    const f32 ankle = skeleton_at(skeleton, SIM_BodyJoint::LeftAnkle).y;
    const f32 ball  = skeleton_at(skeleton, SIM_BodyJoint::LeftBall).y;
    const f32 leg   = bone(skeleton.poses, SIM_BodyJoint::LeftUpperLeg, SIM_BodyJoint::LeftLowerLeg) + bone(skeleton.poses, SIM_BodyJoint::LeftLowerLeg, SIM_BodyJoint::LeftAnkle);

    SIM_BodyJoints joints = {};
    for (u32 step = 0; step <= 230; step++)
    {
        const f32 height = 0.2f + 0.01f * (f32)step;
        sim_body_joints(sim, body_sample({ 0.3f, height, -0.4f }), joints);
        for (SIM_BodyJoint side : { SIM_BodyJoint::LeftUpperLeg, SIM_BodyJoint::RightUpperLeg })
        {
            const Vec3 hip  = joints.poses[(u32)side].position;
            const Vec3 knee = joints.poses[(u32)side + 1].position;
            const Vec3 foot = joints.poses[(u32)side + 3].position;
            EXPECT_LE(knee.z, (hip.z + foot.z) * 0.5f + 1e-4f) << "the knee bends forward at head height " << height;
            EXPECT_NEAR(joints.poses[(u32)side + 6].position.y - foot.y, ball - ankle, 1e-4f) << "the sole is level at head height " << height;
        }
    }

    sim_body_joints(sim, body_sample(), joints);
    EXPECT_NEAR(joint_at(joints, SIM_BodyJoint::LeftAnkle).y, ankle, 1e-4f) << "standing";
    sim_body_joints(sim, body_sample({ 0.0f, 1.2f, 0.0f }), joints);
    EXPECT_NEAR(joint_at(joints, SIM_BodyJoint::LeftAnkle).y, ankle, 1e-4f) << "crouching";
    EXPECT_LT(zm::length(joint_at(joints, SIM_BodyJoint::LeftAnkle) - joint_at(joints, SIM_BodyJoint::LeftUpperLeg)), leg - 0.1f) << "with bent knees";
    sim_body_joints(sim, body_sample({ 0.0f, 0.3f, 0.0f }), joints);
    EXPECT_LT(joint_at(joints, SIM_BodyJoint::LeftAnkle).y, ankle - 0.1f) << "the head in the floor takes the feet with it";
    sim_body_joints(sim, body_sample({ 0.0f, 2.5f, 0.0f }), joints);
    EXPECT_GT(joint_at(joints, SIM_BodyJoint::LeftAnkle).y, ankle + 0.5f) << "flying";
    EXPECT_NEAR(zm::length(joint_at(joints, SIM_BodyJoint::LeftAnkle) - joint_at(joints, SIM_BodyJoint::LeftUpperLeg)), leg, 1e-4f) << "with straight legs";
}

TEST(SimBody, the_body_follows_the_head)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    SIM_Sample sample = body_sample({ 1.0f, 1.6f, 2.0f });
    sample.devices[(u32)SIM_Device::Head].pose.orientation = zm::quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.5f * zm::PI);
    SIM_BodyJoints joints = {};
    sim_body_joints(sim, sample, joints);
    EXPECT_NEAR(joint_at(joints, SIM_BodyJoint::Root).y, 0.0f, 1e-5f);
    EXPECT_NEAR(joint_at(joints, SIM_BodyJoint::Hips).x, 1.0f, 0.15f);
    EXPECT_NEAR(joint_at(joints, SIM_BodyJoint::Hips).z, 2.0f, 0.15f);
    EXPECT_GT(joint_at(joints, SIM_BodyJoint::LeftArmUpper).z, joint_at(joints, SIM_BodyJoint::RightArmUpper).z) << "turned left, the left shoulder is behind";
    EXPECT_GT(zm::dot(zm::rotate(joints.poses[(u32)SIM_BodyJoint::Head].orientation, { 0.0f, 1.0f, 0.0f }), Vec3{ -1.0f, 0.0f, 0.0f }), 0.999f) << "the head looks where the view does, along Meta's +Y";
    EXPECT_FLOAT_EQ(joints.confidence, 1.0f);
}

TEST(SimBody, presets_repeat_and_dropped_joints_report_untracked)
{
    SIM_Sim* a = make_sim();
    SIM_Sim* b = make_sim();
    defer { sim_destroy(a); sim_destroy(b); };
    sim_set_body_pose(a, SIM_BodyPose::ArmsForward);
    sim_set_body_pose(b, SIM_BodyPose::ArmsForward);
    SIM_BodyState state = sim_get_body(a);
    state.limbs[(u32)SIM_Limb::LeftArm] = SIM_LimbMode::Posed;
    sim_set_body(a, state);
    sim_set_body(b, state);
    SIM_BodyJoints first  = {};
    SIM_BodyJoints second = {};
    sim_body_joints(a, body_sample(), first);
    sim_body_joints(b, body_sample(), second);
    EXPECT_EQ(memcmp(&first, &second, sizeof(first)), 0);
    EXPECT_TRUE(first.tracked[(u32)SIM_BodyJoint::LeftArmLower]);
    EXPECT_LT(joint_at(first, SIM_BodyJoint::LeftHandWrist).z, joint_at(first, SIM_BodyJoint::LeftArmUpper).z - 0.4f) << "the posed arm points forward";

    state = sim_get_body(a);
    state.tracked[(u32)SIM_BodyJoint::LeftArmLower] = false;
    sim_set_body(a, state);
    sim_body_joints(a, body_sample(), first);
    EXPECT_FALSE(first.tracked[(u32)SIM_BodyJoint::LeftArmLower]);
    EXPECT_TRUE(first.tracked[(u32)SIM_BodyJoint::LeftArmUpper]);
}

// A replay applies the recorded body exactly; the state hash covers it once it is edited, so
// recordings of the default body keep their hashes
TEST(SimBody, replays_apply_the_body_and_hash_it_once_edited)
{
    SIM_Sim* a = make_sim();
    SIM_Sim* b = make_sim();
    defer { sim_destroy(a); sim_destroy(b); };
    const u64 unedited = sim_state_hash(a);
    sim_set_body_joint(a, SIM_BodyJoint::RightArmLower, zm::quat_from_euler({ 0.3f, 0.2f, 0.1f }));
    SIM_BodyState state = sim_get_body(a);
    state.bone_scales[(u32)SIM_BodyJoint::Neck] = 1.5f;
    sim_set_body(a, state);
    EXPECT_NE(sim_state_hash(a), unedited);

    SIM_FrameState frame = {};
    sim_capture_state(a, frame);
    EXPECT_TRUE(frame.body_present);
    sim_replay_frame(b, frame);
    EXPECT_TRUE(memory_equal(&sim_get_body(a), &sim_get_body(b), sizeof(SIM_BodyState)));
    EXPECT_EQ(sim_body_generation(b), 1u) << "the shape changed once";
    frame.body_present = false;
    sim_default_body(1.75f, frame.body);
    sim_replay_frame(b, frame);
    EXPECT_TRUE(memory_equal(&sim_get_body(a), &sim_get_body(b), sizeof(SIM_BodyState))) << "an absent body keeps the last";
}

// The control API, the CLI and the config edit the body as JSON; members are optional and merge
TEST(SimBody, json_edits_merge_and_round_trip)
{
    SIM_BodyState state = {};
    sim_default_body(1.75f, state);
    const char* edit = R"({ "height": 1.6, "pose": "t_pose", "bones": { "left_arm_upper": 1.3 },
        "joints": { "left_arm_lower": { "rotation": [0, 0.38268343, 0, 0.92387953], "tracked": false, "confidence": 0.25 } },
        "limbs": { "right_leg": "posed" } })";
    JsonParser* parser = json_parse_from_string(edit);
    ASSERT_NE(parser, nullptr);
    EXPECT_TRUE(sim_body_apply_json(json_get_root(parser), state));
    json_release(parser);
    EXPECT_FLOAT_EQ(state.height, 1.6f);
    EXPECT_FLOAT_EQ(state.bone_scales[(u32)SIM_BodyJoint::LeftArmUpper], 1.3f);
    EXPECT_TRUE(zm::equal(state.rotations[(u32)SIM_BodyJoint::RightArmUpper], zm::QUAT_IDENTITY)) << "the T-pose";
    EXPECT_FALSE(state.tracked[(u32)SIM_BodyJoint::LeftArmLower]);
    EXPECT_FLOAT_EQ(state.confidences[(u32)SIM_BodyJoint::LeftArmLower], 0.25f);
    EXPECT_EQ(state.limbs[(u32)SIM_Limb::LeftArm], SIM_LimbMode::Posed) << "a rotated arm joint poses its arm";
    EXPECT_EQ(state.limbs[(u32)SIM_Limb::RightLeg], SIM_LimbMode::Posed);
    EXPECT_EQ(state.limbs[(u32)SIM_Limb::RightArm], SIM_LimbMode::Follow);

    char text[16384];
    ASSERT_TRUE(sim_body_to_json(state, text, sizeof(text)));
    SIM_BodyState loaded = {};
    sim_default_body(1.75f, loaded);
    parser = json_parse_from_string(text);
    ASSERT_NE(parser, nullptr) << text;
    EXPECT_TRUE(sim_body_apply_json(json_get_root(parser), loaded));
    json_release(parser);
    EXPECT_TRUE(memory_equal(&loaded, &state, sizeof(state))) << text;

    char small[64];
    EXPECT_FALSE(sim_body_to_json(state, small, sizeof(small)));

    for (const char* bad : { R"({ "height": "tall" })", R"({ "bones": { "tail": 2 } })", R"({ "bones": { "head": 2 } })", R"({ "bones": { "hips": 9 } })",
                             R"({ "joints": { "neck": { "rotation": [0, 0, 0, 0] } } })", R"({ "joints": { "neck": { "rotation": [0, 0, 1] } } })",
                             R"({ "joints": { "neck": { "confidence": 2 } } })", R"({ "pose": "dab" })", R"({ "limbs": { "tail": "posed" } })",
                             R"({ "limbs": { "left_arm": "wave" } })", R"({ "wings": 2 })", R"([1])" })
    {
        SIM_BodyState unchanged = state;
        parser                  = json_parse_from_string(bad);
        ASSERT_NE(parser, nullptr) << bad;
        EXPECT_FALSE(sim_body_apply_json(json_get_root(parser), unchanged)) << bad;
        json_release(parser);
    }
}

TEST(SimBody, config_shapes_and_poses_the_body)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_TRUE(sim_parse_config(config, R"({ "body": { "height": 1.9, "bones": { "right_lower_leg": 1.2 }, "pose": "a_pose" } })"));
    EXPECT_FLOAT_EQ(config.body.height, 1.9f);
    SIM_Sim* sim = sim_create(config);
    defer { sim_destroy(sim); };
    EXPECT_FLOAT_EQ(sim_get_body(sim).bone_scales[(u32)SIM_BodyJoint::RightLowerLeg], 1.2f);
    EXPECT_EQ(sim_body_generation(sim), 0u) << "the configured shape is where the count starts";
    sim_set_body_pose(sim, SIM_BodyPose::Stand);
    sim_reset_body(sim);
    EXPECT_TRUE(memory_equal(&sim_get_body(sim), &config.body, sizeof(config.body))) << "a reset goes back to the configured body";
}

//
// Face and eye tracking
//

// The head at `head` looking ahead, at a time between blinks
static SIM_Sample face_sample (i64 time = 1000000000ll, Pose head = { { 0.0f, 1.6f, 0.0f }, zm::QUAT_IDENTITY })
{
    SIM_Sample sample = {};
    sample.time                                = time;
    sample.devices[(u32)SIM_Device::Head]      = { .pose = head, .active = true, .tracked = true };
    return sample;
}

static f32 face_weight (const SIM_FaceFrame& frame, SIM_FaceExpression expression)
{
    return frame.weights[(u32)expression];
}

static Vec3 gaze_of (const Pose& eye)
{
    return zm::rotate(eye.orientation, { 0.0f, 0.0f, -1.0f });
}

TEST(SimFace, names_round_trip)
{
    for (u32 e = 0; e < SIM_FACE_EXPRESSION_COUNT; e++)
    {
        SIM_FaceExpression expression = SIM_FaceExpression::Count;
        ASSERT_TRUE(sim_face_expression_from_name(sim_face_expression_name((SIM_FaceExpression)e), expression)) << e;
        EXPECT_EQ((u32)expression, e);
    }
    EXPECT_STREQ(sim_face_expression_name(SIM_FaceExpression::JawDrop), "jaw_drop");
    EXPECT_EQ((u32)SIM_FaceExpression::JawDrop, 24u) << "XR_FACE_EXPRESSION2_JAW_DROP_FB";
    EXPECT_EQ((u32)SIM_FaceExpression::TongueRetreat, 69u);
    for (u32 v = 0; v < SIM_FACE_VISEME_COUNT; v++)
    {
        u32 viseme = 99;
        ASSERT_TRUE(sim_face_viseme_from_name(sim_face_viseme_name(v), viseme));
        EXPECT_EQ(viseme, v);
    }
    EXPECT_STREQ(sim_face_viseme_name(10), "aa");
    for (u32 p = 0; p < (u32)SIM_FacePreset::Count; p++)
    {
        SIM_FacePreset preset = SIM_FacePreset::Count;
        ASSERT_TRUE(sim_face_preset_from_name(sim_face_preset_name((SIM_FacePreset)p), preset));
        EXPECT_EQ((u32)preset, p);
    }
    SIM_FaceExpression expression = {};
    EXPECT_FALSE(sim_face_expression_from_name("ear_wiggle", expression));
}

TEST(SimFace, a_neutral_face_looks_ahead)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    const SIM_FaceState& state = sim_get_face(sim);
    EXPECT_TRUE(state.blink) << "blinking by default";
    EXPECT_FLOAT_EQ(state.confidences[0], 1.0f);
    SIM_FaceFrame frame = {};
    sim_face_frame(sim, face_sample(), frame);
    for (u32 e = 0; e < SIM_FACE_EXPRESSION_COUNT; e++)
    {
        EXPECT_EQ(frame.weights[e], 0.0f) << sim_face_expression_name((SIM_FaceExpression)e);
    }
    const f32 ipd = sim_get_config(sim).ipd;
    EXPECT_TRUE(zm::near(frame.eyes[0].position, Vec3{ -0.5f * ipd, 1.6f, 0.0f }, 1e-5f)) << "the left eye";
    EXPECT_TRUE(zm::near(frame.eyes[1].position, Vec3{ 0.5f * ipd, 1.6f, 0.0f }, 1e-5f));
    EXPECT_TRUE(zm::near(gaze_of(frame.eyes[0]), Vec3{ 0.0f, 0.0f, -1.0f }, 1e-5f));
}

// The eye blendshapes follow the gaze: turned from the head, or converging on a point
TEST(SimFace, eyes_follow_the_gaze)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    SIM_FaceState state = sim_get_face(sim);
    state.gaze_yaw   = 20.0f;
    state.gaze_pitch = -15.0f;
    sim_set_face(sim, state);
    SIM_FaceFrame frame = {};
    sim_face_frame(sim, face_sample(), frame);
    EXPECT_NEAR(face_weight(frame, SIM_FaceExpression::EyesLookLeftL), 20.0f / 35.0f, 1e-3f);
    EXPECT_NEAR(face_weight(frame, SIM_FaceExpression::EyesLookLeftR), 20.0f / 35.0f, 1e-3f);
    EXPECT_NEAR(face_weight(frame, SIM_FaceExpression::EyesLookDownL), 15.0f / 30.0f, 1e-3f);
    EXPECT_EQ(face_weight(frame, SIM_FaceExpression::EyesLookRightL), 0.0f);
    EXPECT_EQ(face_weight(frame, SIM_FaceExpression::EyesLookUpR), 0.0f);
    const Vec3 gaze = gaze_of(frame.eyes[0]);
    EXPECT_LT(gaze.x, -0.3f) << "left is -X";
    EXPECT_LT(gaze.y, -0.2f);

    state.gaze_yaw = 200.0f;
    sim_set_face(sim, state);
    EXPECT_FLOAT_EQ(sim_get_face(sim).gaze_yaw, SIM_FACE_MAX_GAZE_YAW) << "clamped to the eyes' range";

    // A point 0.5 m ahead and right: both eyes turn right, the left one more
    state.look_at = true;
    state.target  = { 0.2f, 1.6f, -0.5f };
    sim_set_face(sim, state);
    sim_face_frame(sim, face_sample(), frame);
    for (u32 e = 0; e < 2; e++)
    {
        EXPECT_GT(zm::dot(gaze_of(frame.eyes[e]), zm::normalize(state.target - frame.eyes[e].position)), 0.9999f) << "eye " << e << " looks at the target";
    }
    EXPECT_GT(face_weight(frame, SIM_FaceExpression::EyesLookRightL), face_weight(frame, SIM_FaceExpression::EyesLookRightR)) << "the eyes converge";
    EXPECT_EQ(face_weight(frame, SIM_FaceExpression::EyesLookLeftL), 0.0f);
}

TEST(SimFace, blinks_close_the_eyes_now_and_then)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    SIM_FaceFrame frame  = {};
    u32           closed = 0;
    u32           open   = 0;
    for (u32 i = 0; i < 1000; i++)
    {
        sim_face_frame(sim, face_sample(10000000ll * (i64)i), frame);
        const f32 lid = face_weight(frame, SIM_FaceExpression::EyesClosedL);
        EXPECT_EQ(lid, face_weight(frame, SIM_FaceExpression::EyesClosedR));
        closed += lid > 0.99f ? 1u : 0u;
        open   += lid == 0.0f ? 1u : 0u;
    }
    EXPECT_GE(closed, 2u) << "10 s at 100 Hz sees blinks";
    EXPECT_GT(open, 900u) << "the eyes are mostly open";

    SIM_FaceState state = sim_get_face(sim);
    state.blink         = false;
    sim_set_face(sim, state);
    for (u32 i = 0; i < 1000; i++)
    {
        sim_face_frame(sim, face_sample(10000000ll * (i64)i), frame);
        ASSERT_EQ(face_weight(frame, SIM_FaceExpression::EyesClosedL), 0.0f) << "no blinks";
    }
}

TEST(SimFace, presets_and_weights)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    sim_set_face_preset(sim, SIM_FacePreset::Smile);
    SIM_FaceFrame frame = {};
    sim_face_frame(sim, face_sample(), frame);
    EXPECT_GT(face_weight(frame, SIM_FaceExpression::LipCornerPullerL), 0.5f);
    EXPECT_GT(face_weight(frame, SIM_FaceExpression::LipCornerPullerR), 0.5f);
    sim_set_face_preset(sim, SIM_FacePreset::EyesClosed);
    sim_face_frame(sim, face_sample(), frame);
    EXPECT_EQ(face_weight(frame, SIM_FaceExpression::EyesClosedL), 1.0f);
    EXPECT_EQ(face_weight(frame, SIM_FaceExpression::LipCornerPullerL), 0.0f) << "a preset replaces the expression";
    sim_set_face_preset(sim, SIM_FacePreset::Neutral);

    SIM_FaceState state = sim_get_face(sim);
    state.weights[(u32)SIM_FaceExpression::JawDrop] = 3.0f;
    state.visemes[10]                               = -1.0f;
    state.confidences[1]                            = zm::nan();
    sim_set_face(sim, state);
    EXPECT_EQ(sim_get_face(sim).weights[(u32)SIM_FaceExpression::JawDrop], 1.0f);
    EXPECT_EQ(sim_get_face(sim).visemes[10], 0.0f);
    EXPECT_EQ(sim_get_face(sim).confidences[1], 1.0f) << "an invalid value keeps the last";
    sim_face_frame(sim, face_sample(), frame);
    EXPECT_EQ(face_weight(frame, SIM_FaceExpression::JawDrop), 1.0f);
}

TEST(SimFace, json_edits_merge_and_round_trip)
{
    SIM_FaceState state = {};
    sim_default_face(state);
    JsonParser* parser = json_parse_from_string(R"({ "preset": "smile", "weights": { "jaw_drop": 0.25 }, "visemes": { "aa": 0.5 },
        "confidences": [0.5, 0.75], "blink": false, "gaze": [10, -5], "look_at": [0.1, 1.5, -1] })");
    ASSERT_NE(parser, nullptr);
    EXPECT_TRUE(sim_face_apply_json(json_get_root(parser), state));
    json_release(parser);
    EXPECT_GT(state.weights[(u32)SIM_FaceExpression::LipCornerPullerL], 0.5f);
    EXPECT_FLOAT_EQ(state.weights[(u32)SIM_FaceExpression::JawDrop], 0.25f);
    EXPECT_FLOAT_EQ(state.visemes[10], 0.5f);
    EXPECT_FLOAT_EQ(state.confidences[1], 0.75f);
    EXPECT_FALSE(state.blink);
    EXPECT_FLOAT_EQ(state.gaze_yaw, 10.0f);
    EXPECT_TRUE(state.look_at);
    EXPECT_TRUE(zm::equal(state.target, Vec3{ 0.1f, 1.5f, -1.0f }));

    char text[8192];
    ASSERT_TRUE(sim_face_to_json(state, text, sizeof(text)));
    SIM_FaceState loaded = {};
    sim_default_face(loaded);
    parser = json_parse_from_string(text);
    ASSERT_NE(parser, nullptr) << text;
    EXPECT_TRUE(sim_face_apply_json(json_get_root(parser), loaded));
    json_release(parser);
    EXPECT_TRUE(memory_equal(&loaded, &state, sizeof(state))) << text;

    parser = json_parse_from_string(R"({ "look_at": null })");
    EXPECT_TRUE(sim_face_apply_json(json_get_root(parser), loaded));
    json_release(parser);
    EXPECT_FALSE(loaded.look_at) << "null looks along the gaze angles again";

    for (const char* bad : { R"({ "weights": { "ear_wiggle": 1 } })", R"({ "weights": { "jaw_drop": 2 } })", R"({ "visemes": { "zz": 1 } })",
                             R"({ "confidences": [1] })", R"({ "blink": 1 })", R"({ "gaze": [100, 0] })", R"({ "look_at": [0, 1] })",
                             R"({ "preset": "wink" })", R"({ "mood": "happy" })" })
    {
        SIM_FaceState unchanged = state;
        parser                  = json_parse_from_string(bad);
        ASSERT_NE(parser, nullptr) << bad;
        EXPECT_FALSE(sim_face_apply_json(json_get_root(parser), unchanged)) << bad;
        EXPECT_TRUE(memory_equal(&unchanged, &state, sizeof(state))) << bad;
        json_release(parser);
    }
}

TEST(SimFace, config_sets_the_face_and_replays_apply_it)
{
    SIM_Config config = {};
    sim_default_config(config);
    EXPECT_TRUE(sim_parse_config(config, R"({ "face": { "preset": "frown", "blink": false } })"));
    EXPECT_FALSE(config.face.blink);
    SIM_Sim* a = sim_create(config);
    SIM_Sim* b = make_sim();
    defer { sim_destroy(a); sim_destroy(b); };
    EXPECT_TRUE(memory_equal(&sim_get_face(a), &config.face, sizeof(config.face)));
    const u64 unedited = sim_state_hash(b);

    SIM_FrameState state = {};
    sim_capture_state(a, state);
    EXPECT_TRUE(state.face_present);
    sim_replay_frame(b, state);
    EXPECT_TRUE(memory_equal(&sim_get_face(a), &sim_get_face(b), sizeof(SIM_FaceState)));
    EXPECT_NE(sim_state_hash(b), unedited) << "an edited face is part of the state";
    sim_reset_face(b);
    EXPECT_FALSE(memory_equal(&sim_get_face(a), &sim_get_face(b), sizeof(SIM_FaceState))) << "b's configured face is neutral";
}

// Key 4 picks the eyes profile: the mouse turns the eyes from the head, which stays
TEST(SimFace, the_eyes_profile_steers_the_gaze_with_the_mouse)
{
    SIM_Sim* sim = make_sim();
    defer { sim_destroy(sim); };
    tap(sim, OS_Key::Digit4);
    EXPECT_STREQ(profile_name(sim), "eyes");
    const Pose head = sim_get_device_pose(sim, SIM_Device::Head);
    sim_control_mouse_motion(sim, -50.0f, -20.0f);
    step(sim);
    EXPECT_GT(sim_get_face(sim).gaze_yaw, 1.0f) << "the mouse to the left turns the eyes left";
    EXPECT_GT(sim_get_face(sim).gaze_pitch, 0.5f) << "and up";
    EXPECT_TRUE(zm::equal(sim_get_device_pose(sim, SIM_Device::Head).orientation, head.orientation)) << "the head stays";
    sim_control_mouse_motion(sim, -100000.0f, 0.0f);
    step(sim);
    EXPECT_FLOAT_EQ(sim_get_face(sim).gaze_yaw, SIM_FACE_MAX_GAZE_YAW);
    SIM_FaceState state = sim_get_face(sim);
    state.look_at       = true;
    state.target        = { 0.0f, 1.6f, -1.0f };
    sim_set_face(sim, state);
    sim_control_mouse_motion(sim, 10.0f, 0.0f);
    step(sim);
    EXPECT_FALSE(sim_get_face(sim).look_at) << "steering the eyes lets go of the point";
}
