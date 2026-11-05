#include "zmath.h"

#include <gtest/gtest.h>
#include <math.h>

static u64 sweep_hash (f32 (*fn)(f32), f32 lo, f32 hi, u32 steps)
{
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i <= steps; i++)
    {
        const f32 x = lo + (hi - lo) * ((f32)i / (f32)steps);
        const f32 y = fn(x);
        u32       bits = 0;
        memory_copy(&bits, &y, 4);
        h = (h ^ bits) * 0x100000001B3ull;
    }
    return h;
}

static void expect_close (f32 (*fn)(f32), f64 (*ref)(f64), f32 lo, f32 hi, f64 tolerance)
{
    const u32 steps = 20000;
    for (u32 i = 0; i <= steps; i++)
    {
        const f32 x        = lo + (hi - lo) * ((f32)i / (f32)steps);
        const f64 expected = ref((f64)x);
        const f64 actual   = (f64)fn(x);
        const f64 scale    = fabs(expected) > 1.0 ? fabs(expected) : 1.0;
        ASSERT_NEAR(actual, expected, tolerance * scale) << "x = " << x;
    }
}

TEST(Zmath, nan_is_a_quiet_nan)
{
    const f32 n = zm::nan();
    EXPECT_FALSE(zm::is_finite(n));
    EXPECT_NE(n, n);
    u32 bits = 0;
    memcpy(&bits, &n, sizeof(bits));
    EXPECT_EQ(bits, 0x7fc00000u);
}

TEST(Zmath, exact_special_values)
{
    EXPECT_EQ(zm::sin(0.0f), 0.0f);
    EXPECT_EQ(zm::cos(0.0f), 1.0f);
    EXPECT_EQ(zm::tan(0.0f), 0.0f);
    EXPECT_EQ(zm::exp(0.0f), 1.0f);
    EXPECT_EQ(zm::log(1.0f), 0.0f);
    EXPECT_EQ(zm::sqrt(4.0f), 2.0f);
    EXPECT_EQ(zm::sqrt(0.0f), 0.0f);
    EXPECT_EQ(zm::atan(0.0f), 0.0f);
    EXPECT_EQ(zm::atan2(0.0f, 1.0f), 0.0f);
    EXPECT_EQ(zm::asin(0.0f), 0.0f);
    EXPECT_EQ(zm::acos(1.0f), 0.0f);
    EXPECT_EQ(zm::pow(2.0f, 10.0f), 1024.0f);
    EXPECT_EQ(zm::pow(0.0f, 2.0f), 0.0f);
    EXPECT_EQ(zm::pow(-2.0f, 3.0f), -8.0f);
    EXPECT_EQ(zm::floor(-1.5f), -2.0f);
    EXPECT_EQ(zm::ceil(-1.5f), -1.0f);
    EXPECT_EQ(zm::round(2.5f), 3.0f);
    EXPECT_EQ(zm::round(-2.5f), -3.0f);
    EXPECT_EQ(zm::trunc(-2.7f), -2.0f);
    EXPECT_EQ(zm::fmod(5.5f, 2.0f), 1.5f);
    EXPECT_EQ(zm::fmod(-5.5f, 2.0f), -1.5f);
    EXPECT_EQ(zm::abs(-3.0f), 3.0f);
}

TEST(Zmath, transcendentals_match_libm_within_tolerance)
{
    expect_close(zm::sin, ::sin, -100.0f, 100.0f, 2e-6);
    expect_close(zm::cos, ::cos, -100.0f, 100.0f, 2e-6);
    expect_close(zm::tan, ::tan, -1.5f, 1.5f, 4e-6);
    expect_close(zm::atan, ::atan, -50.0f, 50.0f, 2e-6);
    expect_close(zm::asin, ::asin, -1.0f, 1.0f, 4e-6);
    expect_close(zm::acos, ::acos, -1.0f, 1.0f, 4e-6);
    expect_close(zm::exp, ::exp, -80.0f, 80.0f, 2e-6);
    expect_close(zm::log, ::log, 1e-6f, 1e6f, 2e-6);
    expect_close(zm::sqrt, ::sqrt, 0.0f, 1e6f, 1e-7);

    for (i32 i = -200; i <= 200; i++)
    {
        for (i32 j = -200; j <= 200; j++)
        {
            const f32 y = (f32)i * 0.37f;
            const f32 x = (f32)j * 0.53f;
            EXPECT_NEAR((f64)zm::atan2(y, x), ::atan2((f64)y, (f64)x), 2e-6) << y << " " << x;
        }
    }

    for (i32 i = 1; i <= 50; i++)
    {
        for (i32 j = -20; j <= 20; j++)
        {
            const f32 x        = (f32)i * 0.5f;
            const f32 y        = (f32)j * 0.25f;
            const f64 expected = ::pow((f64)x, (f64)y);
            EXPECT_NEAR((f64)zm::pow(x, y), expected, 4e-6 * fabs(expected)) << x << " " << y;
        }
    }
}

// Golden hashes captured from the reference x86-64 build. A change here means the
// results changed bit for bit, which breaks replay of existing recordings.
TEST(Zmath, transcendentals_are_bit_identical_to_golden)
{
    EXPECT_EQ(sweep_hash(zm::sin, -50.0f, 50.0f, 4096), 0xE37E95EA58590F97ull);
    EXPECT_EQ(sweep_hash(zm::cos, -50.0f, 50.0f, 4096), 0x06693F0FBA4BA207ull);
    EXPECT_EQ(sweep_hash(zm::atan, -50.0f, 50.0f, 4096), 0x933B9B8CE367EAB7ull);
    EXPECT_EQ(sweep_hash(zm::exp, -50.0f, 50.0f, 4096), 0x105D9E9BC99948CBull);
    EXPECT_EQ(sweep_hash(zm::log, 0.001f, 1000.0f, 4096), 0x71150A228D5FB78Cull);
    EXPECT_EQ(sweep_hash(zm::asin, -1.0f, 1.0f, 4096), 0xE2393E9E805FEBAFull);
}

TEST(Zmath, no_fused_multiply_add)
{
    const volatile f32 x = 1.0f + 0x1p-23f;
    const volatile f32 y = 1.0f + 0x1p-22f;
    const f32    r = x * x - y;
    EXPECT_EQ(r, 0.0f);
}

TEST(Zmath, vec3_operations)
{
    const Vec3 a = { 1.0f, 2.0f, 3.0f };
    const Vec3 b = { 4.0f, 5.0f, 6.0f };

    EXPECT_EQ(zm::dot(a, b), 32.0f);
    const Vec3 c = zm::cross(a, b);
    EXPECT_EQ(c.x, -3.0f);
    EXPECT_EQ(c.y, 6.0f);
    EXPECT_EQ(c.z, -3.0f);

    const Vec3 s = a + b;
    EXPECT_EQ(s.x, 5.0f);
    EXPECT_EQ((a - b).z, -3.0f);
    EXPECT_EQ((a * 2.0f).y, 4.0f);
    EXPECT_EQ((2.0f * a).y, 4.0f);
    EXPECT_EQ((-a).x, -1.0f);
    EXPECT_EQ(zm::length(Vec3{ 3.0f, 4.0f, 0.0f }), 5.0f);
    EXPECT_EQ(zm::length(zm::normalize(b)), 1.0f);
    EXPECT_EQ(zm::length(zm::normalize(Vec3{})), 0.0f);
    EXPECT_EQ(zm::lerp(a, b, 0.5f).x, 2.5f);
    EXPECT_TRUE(zm::equal(a, a));
    EXPECT_FALSE(zm::equal(a, b));
    EXPECT_TRUE(zm::near(a, Vec3{ 1.0f, 2.0f, 3.0f + 1e-7f }, 1e-6f));
}

TEST(Zmath, quaternion_rotations)
{
    const Quat q    = zm::quat_from_axis_angle(Vec3{ 0.0f, 1.0f, 0.0f }, zm::PI * 0.5f);
    const Vec3 r    = zm::rotate(q, Vec3{ 1.0f, 0.0f, 0.0f });
    EXPECT_NEAR(r.x, 0.0f, 1e-6f);
    EXPECT_NEAR(r.y, 0.0f, 1e-6f);
    EXPECT_NEAR(r.z, -1.0f, 1e-6f);

    const Quat identity = zm::QUAT_IDENTITY;
    EXPECT_TRUE(zm::equal(zm::rotate(identity, r), r));
    EXPECT_NEAR(zm::length(q), 1.0f, 1e-6f);

    const Quat qi   = zm::inverse(q);
    const Vec3 back = zm::rotate(qi, r);
    EXPECT_NEAR(back.x, 1.0f, 1e-6f);
    EXPECT_NEAR(back.z, 0.0f, 1e-6f);

    const Quat qq   = q * q;
    const Vec3 half = zm::rotate(qq, Vec3{ 1.0f, 0.0f, 0.0f });
    EXPECT_NEAR(half.x, -1.0f, 1e-6f);

    const Quat e = zm::quat_from_euler(Vec3{ 0.0f, zm::PI * 0.5f, 0.0f });
    EXPECT_TRUE(zm::near(e, q, 1e-6f));

    const Quat s = zm::slerp(identity, q, 0.5f);
    const Vec3 sv = zm::rotate(s, Vec3{ 1.0f, 0.0f, 0.0f });
    EXPECT_NEAR(sv.x, 0.70710678f, 1e-5f);
    EXPECT_NEAR(sv.z, -0.70710678f, 1e-5f);

    const Vec3 euler = zm::euler_from_quat(q);
    EXPECT_NEAR(euler.y, zm::PI * 0.5f, 1e-5f);
}

TEST(Zmath, pose_composition)
{
    const Pose a = { .position = { 1.0f, 0.0f, 0.0f }, .orientation = zm::quat_from_axis_angle(Vec3{ 0.0f, 1.0f, 0.0f }, zm::PI * 0.5f) };
    const Pose b = { .position = { 0.0f, 0.0f, -1.0f }, .orientation = zm::QUAT_IDENTITY };

    const Pose ab = zm::pose_mul(a, b);
    EXPECT_NEAR(ab.position.x, 0.0f, 1e-6f);
    EXPECT_NEAR(ab.position.z, 0.0f, 1e-6f);

    const Vec3 p = zm::transform_point(a, Vec3{ 0.0f, 0.0f, -1.0f });
    EXPECT_NEAR(p.x, 0.0f, 1e-6f);
    EXPECT_NEAR(p.z, 0.0f, 1e-6f);

    const Pose inv      = zm::pose_inverse(a);
    const Pose identity = zm::pose_mul(inv, a);
    EXPECT_TRUE(zm::near(identity.position, Vec3{}, 1e-6f));
    EXPECT_TRUE(zm::near(identity.orientation, zm::QUAT_IDENTITY, 1e-6f));

    const Pose rel = zm::pose_relative(a, b);
    EXPECT_TRUE(zm::near(zm::pose_mul(a, rel), b, 1e-6f));
}

TEST(Zmath, matrices)
{
    const Mat4 t  = zm::mat4_translation(Vec3{ 1.0f, 2.0f, 3.0f });
    const Vec3 p  = zm::transform_point(t, Vec3{ 1.0f, 1.0f, 1.0f });
    EXPECT_EQ(p.x, 2.0f);
    EXPECT_EQ(p.y, 3.0f);
    EXPECT_EQ(p.z, 4.0f);

    const Mat4 r  = zm::mat4_from_quat(zm::quat_from_axis_angle(Vec3{ 0.0f, 0.0f, 1.0f }, zm::PI * 0.5f));
    const Vec3 rp = zm::transform_point(r, Vec3{ 1.0f, 0.0f, 0.0f });
    EXPECT_NEAR(rp.x, 0.0f, 1e-6f);
    EXPECT_NEAR(rp.y, 1.0f, 1e-6f);

    const Mat4 m   = t * r;
    const Mat4 inv = zm::inverse(m);
    const Mat4 id  = m * inv;
    for (u32 i = 0; i < 4; i++)
    {
        for (u32 j = 0; j < 4; j++)
        {
            EXPECT_NEAR(id.m[i][j], i == j ? 1.0f : 0.0f, 1e-5f);
        }
    }

    const Pose pose = { .position = { 1.0f, 2.0f, 3.0f }, .orientation = zm::quat_from_axis_angle(Vec3{ 1.0f, 0.0f, 0.0f }, 0.3f) };
    const Mat4 pm   = zm::mat4_from_pose(pose);
    const Vec3 a    = zm::transform_point(pm, Vec3{ 0.5f, -0.5f, 2.0f });
    const Vec3 b    = zm::transform_point(pose, Vec3{ 0.5f, -0.5f, 2.0f });
    EXPECT_TRUE(zm::near(a, b, 1e-5f));

    const Fov  fov  = { .angle_left = -0.8f, .angle_right = 0.8f, .angle_up = 0.7f, .angle_down = -0.7f };
    const Mat4 proj = zm::mat4_projection(fov, 0.1f, 100.0f);
    const Vec4 near_point = proj * Vec4{ 0.0f, 0.0f, -0.1f, 1.0f };
    const Vec4 far_point  = proj * Vec4{ 0.0f, 0.0f, -100.0f, 1.0f };
    EXPECT_NEAR(near_point.z / near_point.w, 0.0f, 1e-5f);
    EXPECT_NEAR(far_point.z / far_point.w, 1.0f, 1e-5f);
}
