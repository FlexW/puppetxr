#include "zmath.h"

namespace zm
{

//
// Statics
//
// Scalar functions are evaluated in f64 and rounded once to f32. All constants and
// algorithms are plain IEEE arithmetic, so results are the same on every platform.

static const f64 D_PI      = 3.14159265358979323846;
static const f64 D_PI_2    = 1.57079632679489661923;
static const f64 D_PI_6    = 0.52359877559829887308;
static const f64 D_SQRT3   = 1.73205080756887729353;
static const f64 D_TAN_PI_12 = 0.26794919243112270647;
static const f64 D_2_PI    = 0.63661977236758134308;
static const f64 D_LN2     = 0.69314718055994530942;
static const f64 D_LN2_HI  = 6.93147180369123816490e-01;
static const f64 D_LN2_LO  = 1.90821492927058770002e-10;
static const f64 D_1_LN2   = 1.44269504088896338700;
static const f64 D_SQRT2   = 1.41421356237309504880;

// pi/2 split so that k * D_PIO2_1 is exact for |k| < 2^20 (from fdlibm)
static const f64 D_PIO2_1 = 1.57079632673412561417e+00;
static const f64 D_PIO2_2 = 6.07710050650619224932e-11;
static const f64 D_PIO2_3 = 2.02226624879595063154e-21;

static u64 f64_bits (f64 x)
{
    u64 bits = 0;
    memory_copy(&bits, &x, 8);
    return bits;
}

static f64 f64_from_bits (u64 bits)
{
    f64 x = 0.0;
    memory_copy(&x, &bits, 8);
    return x;
}

static u32 f32_bits (f32 x)
{
    u32 bits = 0;
    memory_copy(&bits, &x, 4);
    return bits;
}

static f32 f32_from_bits (u32 bits)
{
    f32 x = 0.0f;
    memory_copy(&x, &bits, 4);
    return x;
}

static b8 f64_is_finite (f64 x)
{
    return ((f64_bits(x) >> 52) & 0x7FF) != 0x7FF;
}

static f64 f64_trunc (f64 x)
{
    if (x >= 4503599627370496.0 || x <= -4503599627370496.0 || !f64_is_finite(x))
    {
        return x;
    }
    return (f64)(i64)x;
}

static f64 f64_floor (f64 x)
{
    const f64 t = f64_trunc(x);
    return t > x ? t - 1.0 : t;
}

static f64 f64_round (f64 x)
{
    return x < 0.0 ? -f64_floor(-x + 0.5) : f64_floor(x + 0.5);
}

static f64 f64_ldexp (f64 x, i32 k)
{
    while (k > 1000)
    {
        x *= f64_from_bits((u64)(1000 + 1023) << 52);
        k -= 1000;
    }
    while (k < -1000)
    {
        x *= f64_from_bits((u64)(-1000 + 1023) << 52);
        k += 1000;
    }
    return x * f64_from_bits((u64)(k + 1023) << 52);
}

static f64 poly_sin (f64 r)
{
    const f64 r2 = r * r;
    f64 p = 1.0 / 1307674368000.0;              // 1/15!
    p = p * r2 - 1.0 / 6227020800.0;            // 1/13!
    p = p * r2 + 1.0 / 39916800.0;              // 1/11!
    p = p * r2 - 1.0 / 362880.0;                // 1/9!
    p = p * r2 + 1.0 / 5040.0;                  // 1/7!
    p = p * r2 - 1.0 / 120.0;                   // 1/5!
    p = p * r2 + 1.0 / 6.0;                     // 1/3!
    return r - r * r2 * p;
}

static f64 poly_cos (f64 r)
{
    const f64 r2 = r * r;
    f64 p = 1.0 / 20922789888000.0;             // 1/16!
    p = p * r2 - 1.0 / 87178291200.0;           // 1/14!
    p = p * r2 + 1.0 / 479001600.0;             // 1/12!
    p = p * r2 - 1.0 / 3628800.0;               // 1/10!
    p = p * r2 + 1.0 / 40320.0;                 // 1/8!
    p = p * r2 - 1.0 / 720.0;                   // 1/6!
    p = p * r2 + 1.0 / 24.0;                    // 1/4!
    return 1.0 - r2 * 0.5 + r2 * r2 * p;
}

static void reduce_pi_2 (f64 x, i32& out_quadrant, f64& out_r)
{
    const f64 k = f64_round(x * D_2_PI);
    f64       r = x - k * D_PIO2_1;
    r           = r - k * D_PIO2_2;
    r           = r - k * D_PIO2_3;

    out_quadrant = (i32)((i64)k & 3);
    out_r        = r;
}

static void sin_cos_f64 (f64 x, f64& out_sin, f64& out_cos)
{
    i32 quadrant = 0;
    f64 r        = 0.0;
    reduce_pi_2(x, quadrant, r);

    const f64 s = poly_sin(r);
    const f64 c = poly_cos(r);

    switch (quadrant)
    {
    case 0:
        out_sin = s;
        out_cos = c;
        break;
    case 1:
        out_sin = c;
        out_cos = -s;
        break;
    case 2:
        out_sin = -s;
        out_cos = -c;
        break;
    default:
        out_sin = -c;
        out_cos = s;
        break;
    }
}

static f64 atan_f64 (f64 x)
{
    const b8  negative = x < 0.0;
    f64       a        = negative ? -x : x;
    f64       offset   = 0.0;
    b8        inverted = false;

    if (a > 1.0)
    {
        a        = 1.0 / a;
        inverted = true;
    }
    if (a > D_TAN_PI_12)
    {
        a      = (a * D_SQRT3 - 1.0) / (a + D_SQRT3);
        offset = D_PI_6;
    }

    const f64 a2 = a * a;
    f64 p = 1.0 / 27.0;
    p = p * a2 - 1.0 / 25.0;
    p = p * a2 + 1.0 / 23.0;
    p = p * a2 - 1.0 / 21.0;
    p = p * a2 + 1.0 / 19.0;
    p = p * a2 - 1.0 / 17.0;
    p = p * a2 + 1.0 / 15.0;
    p = p * a2 - 1.0 / 13.0;
    p = p * a2 + 1.0 / 11.0;
    p = p * a2 - 1.0 / 9.0;
    p = p * a2 + 1.0 / 7.0;
    p = p * a2 - 1.0 / 5.0;
    p = p * a2 + 1.0 / 3.0;
    f64 result = a - a * a2 * p + offset;

    if (inverted)
    {
        result = D_PI_2 - result;
    }
    return negative ? -result : result;
}

static f64 atan2_f64 (f64 y, f64 x)
{
    const b8 y_negative = (f64_bits(y) >> 63) != 0;
    const b8 x_negative = (f64_bits(x) >> 63) != 0;

    if (y == 0.0)
    {
        if (x_negative)
        {
            return y_negative ? -D_PI : D_PI;
        }
        return y;
    }
    if (x == 0.0)
    {
        return y_negative ? -D_PI_2 : D_PI_2;
    }
    if (!f64_is_finite(x) || !f64_is_finite(y))
    {
        return x - x + y - y; // NOLINT(misc-redundant-expression): NaN for infinite or NaN input, as libm
    }

    const f64 a = atan_f64(y / x);
    if (x_negative)
    {
        return y_negative ? a - D_PI : a + D_PI;
    }
    return a;
}

static f64 exp_f64 (f64 x)
{
    if (!f64_is_finite(x))
    {
        return x < 0.0 ? 0.0 : x;
    }
    if (x > 709.0)
    {
        return f64_from_bits(0x7FF0000000000000ull);
    }
    if (x < -745.0)
    {
        return 0.0;
    }

    const f64 k = f64_round(x * D_1_LN2);
    f64       r = x - k * D_LN2_HI;
    r           = r - k * D_LN2_LO;

    f64 p = 1.0 / 6227020800.0;                 // 1/13!
    p = p * r + 1.0 / 479001600.0;              // 1/12!
    p = p * r + 1.0 / 39916800.0;               // 1/11!
    p = p * r + 1.0 / 3628800.0;                // 1/10!
    p = p * r + 1.0 / 362880.0;                 // 1/9!
    p = p * r + 1.0 / 40320.0;                  // 1/8!
    p = p * r + 1.0 / 5040.0;                   // 1/7!
    p = p * r + 1.0 / 720.0;                    // 1/6!
    p = p * r + 1.0 / 120.0;                    // 1/5!
    p = p * r + 1.0 / 24.0;                     // 1/4!
    p = p * r + 1.0 / 6.0;                      // 1/3!
    p = p * r + 0.5;
    p = p * r + 1.0;
    p = p * r + 1.0;

    return f64_ldexp(p, (i32)k);
}

static f64 log_f64 (f64 x)
{
    if (x < 0.0 || !f64_is_finite(x))
    {
        return x < 0.0 ? f64_from_bits(0x7FF8000000000000ull) : x;
    }
    if (x == 0.0)
    {
        return f64_from_bits(0xFFF0000000000000ull);
    }

    u64 bits = f64_bits(x);
    i32 e    = (i32)((bits >> 52) & 0x7FF) - 1023;
    if (e == -1023)
    {
        x    = x * 18014398509481984.0; // 2^54, bring subnormals into range
        bits = f64_bits(x);
        e    = (i32)((bits >> 52) & 0x7FF) - 1023 - 54;
    }

    f64 m = f64_from_bits((bits & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull);
    if (m > D_SQRT2)
    {
        m *= 0.5;
        e += 1;
    }

    const f64 s  = (m - 1.0) / (m + 1.0);
    const f64 s2 = s * s;
    f64 p = 1.0 / 21.0;
    p = p * s2 + 1.0 / 19.0;
    p = p * s2 + 1.0 / 17.0;
    p = p * s2 + 1.0 / 15.0;
    p = p * s2 + 1.0 / 13.0;
    p = p * s2 + 1.0 / 11.0;
    p = p * s2 + 1.0 / 9.0;
    p = p * s2 + 1.0 / 7.0;
    p = p * s2 + 1.0 / 5.0;
    p = p * s2 + 1.0 / 3.0;
    p = p * s2 + 1.0;

    return (f64)e * D_LN2 + 2.0 * s * p;
}

static Quat quat_from_axes (Vec3 x_axis, Vec3 y_axis, Vec3 z_axis)
{
    const f32 trace = x_axis.x + y_axis.y + z_axis.z;
    Quat      q = {};
    if (trace > 0.0f)
    {
        const f32 s = sqrt(trace + 1.0f) * 2.0f;
        q = { (y_axis.z - z_axis.y) / s, (z_axis.x - x_axis.z) / s, (x_axis.y - y_axis.x) / s, 0.25f * s };
    }
    else if (x_axis.x > y_axis.y && x_axis.x > z_axis.z)
    {
        const f32 s = sqrt(1.0f + x_axis.x - y_axis.y - z_axis.z) * 2.0f;
        q = { 0.25f * s, (y_axis.x + x_axis.y) / s, (z_axis.x + x_axis.z) / s, (y_axis.z - z_axis.y) / s };
    }
    else if (y_axis.y > z_axis.z)
    {
        const f32 s = sqrt(1.0f + y_axis.y - x_axis.x - z_axis.z) * 2.0f;
        q = { (y_axis.x + x_axis.y) / s, 0.25f * s, (z_axis.y + y_axis.z) / s, (z_axis.x - x_axis.z) / s };
    }
    else
    {
        const f32 s = sqrt(1.0f + z_axis.z - x_axis.x - y_axis.y) * 2.0f;
        q = { (z_axis.x + x_axis.z) / s, (z_axis.y + y_axis.z) / s, 0.25f * s, (x_axis.y - y_axis.x) / s };
    }
    return normalize(q);
}

//
// Scalars
//

f32 sqrt (f32 x)
{
    return __builtin_sqrtf(x);
}

f32 sin (f32 x)
{
    if (!is_finite(x))
    {
        return x - x; // NOLINT(misc-redundant-expression): NaN for infinite or NaN input, as libm
    }
    f64 s = 0.0;
    f64 c = 0.0;
    sin_cos_f64((f64)x, s, c);
    return (f32)s;
}

f32 cos (f32 x)
{
    if (!is_finite(x))
    {
        return x - x; // NOLINT(misc-redundant-expression): NaN for infinite or NaN input, as libm
    }
    f64 s = 0.0;
    f64 c = 0.0;
    sin_cos_f64((f64)x, s, c);
    return (f32)c;
}

f32 tan (f32 x)
{
    if (!is_finite(x))
    {
        return x - x; // NOLINT(misc-redundant-expression): NaN for infinite or NaN input, as libm
    }
    f64 s = 0.0;
    f64 c = 0.0;
    sin_cos_f64((f64)x, s, c);
    return (f32)(s / c);
}

f32 asin (f32 x)
{
    if (x < -1.0f || x > 1.0f || !is_finite(x))
    {
        return f32_from_bits(0x7FC00000u);
    }
    const f64 d = (f64)x;
    return (f32)atan2_f64(d, __builtin_sqrt(1.0 - d * d));
}

f32 acos (f32 x)
{
    if (x < -1.0f || x > 1.0f || !is_finite(x))
    {
        return f32_from_bits(0x7FC00000u);
    }
    const f64 d = (f64)x;
    return (f32)atan2_f64(__builtin_sqrt(1.0 - d * d), d);
}

f32 atan (f32 x)
{
    if (!is_finite(x))
    {
        return x != x ? x : (x < 0.0f ? -(f32)D_PI_2 : (f32)D_PI_2);
    }
    return (f32)atan_f64((f64)x);
}

f32 atan2 (f32 y, f32 x)
{
    return (f32)atan2_f64((f64)y, (f64)x);
}

f32 exp (f32 x)
{
    return (f32)exp_f64((f64)x);
}

f32 log (f32 x)
{
    return (f32)log_f64((f64)x);
}

f32 pow (f32 x, f32 y)
{
    if (y == 0.0f)
    {
        return 1.0f;
    }
    if (x == 0.0f)
    {
        return y > 0.0f ? 0.0f : f32_from_bits(0x7F800000u);
    }
    if (x < 0.0f)
    {
        if (trunc(y) != y)
        {
            return f32_from_bits(0x7FC00000u);
        }
        const f32 magnitude = (f32)exp_f64((f64)y * log_f64(-(f64)x));
        const b8  odd       = fmod(y, 2.0f) != 0.0f;
        return odd ? -magnitude : magnitude;
    }
    return (f32)exp_f64((f64)y * log_f64((f64)x));
}

f32 floor (f32 x)
{
    if (!is_finite(x) || x >= 8388608.0f || x <= -8388608.0f)
    {
        return x;
    }
    const f32 t = (f32)(i64)x;
    return t > x ? t - 1.0f : t;
}

f32 ceil (f32 x)
{
    if (!is_finite(x) || x >= 8388608.0f || x <= -8388608.0f)
    {
        return x;
    }
    const f32 t = (f32)(i64)x;
    return t < x ? t + 1.0f : t;
}

f32 round (f32 x)
{
    return x < 0.0f ? -floor(-x + 0.5f) : floor(x + 0.5f);
}

f32 trunc (f32 x)
{
    if (!is_finite(x) || x >= 8388608.0f || x <= -8388608.0f)
    {
        return x;
    }
    return (f32)(i64)x;
}

f32 fmod (f32 x, f32 y)
{
    if (y == 0.0f || !is_finite(x) || y != y)
    {
        return f32_from_bits(0x7FC00000u);
    }
    const f64 dx = (f64)x;
    const f64 dy = (f64)y;
    return (f32)(dx - f64_trunc(dx / dy) * dy);
}

b8 is_finite (f32 x)
{
    return ((f32_bits(x) >> 23) & 0xFF) != 0xFF;
}

f32 nan ()
{
    return f32_from_bits(0x7fc00000u);
}

//
// Vectors
//

Vec2 normalize (Vec2 a)
{
    const f32 len = length(a);
    return len > 0.0f ? a / len : Vec2{};
}

Vec3 normalize (Vec3 a)
{
    const f32 len = length(a);
    return len > 0.0f ? a / len : Vec3{};
}

//
// Quat
//

Quat operator* (Quat a, Quat b)
{
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat normalize (Quat a)
{
    const f32 len = length(a);
    return len > 0.0f ? a * (1.0f / len) : QUAT_IDENTITY;
}

Quat inverse (Quat a)
{
    const f32 len_sq = dot(a, a);
    if (len_sq <= 0.0f)
    {
        return QUAT_IDENTITY;
    }
    return conjugate(a) * (1.0f / len_sq);
}

Vec3 rotate (Quat q, Vec3 v)
{
    const Vec3 u = { q.x, q.y, q.z };
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

Quat quat_from_axis_angle (Vec3 axis, f32 angle)
{
    const Vec3 n = normalize(axis);
    const f32  h = angle * 0.5f;
    const f32  s = sin(h);
    return { n.x * s, n.y * s, n.z * s, cos(h) };
}

Quat quat_from_euler (Vec3 euler)
{
    const Quat yaw   = quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, euler.y);
    const Quat pitch = quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, euler.x);
    const Quat roll  = quat_from_axis_angle({ 0.0f, 0.0f, 1.0f }, euler.z);
    return yaw * pitch * roll;
}

Vec3 euler_from_quat (Quat q)
{
    const Vec3 forward = rotate(q, { 0.0f, 0.0f, -1.0f });
    const f32  pitch   = asin(clamp(forward.y, -1.0f, 1.0f));
    const f32  yaw     = atan2(-forward.x, -forward.z);

    const Quat yaw_pitch = quat_from_axis_angle({ 0.0f, 1.0f, 0.0f }, yaw) * quat_from_axis_angle({ 1.0f, 0.0f, 0.0f }, pitch);
    const Quat remainder = inverse(yaw_pitch) * q;
    const f32  roll      = 2.0f * atan2(remainder.z, remainder.w);

    return { pitch, yaw, roll };
}

Quat quat_look_rotation (Vec3 forward, Vec3 up)
{
    const Vec3 z_axis = normalize(-forward);
    Vec3       x_axis = cross(up, z_axis);
    if (length_sq(x_axis) < EPSILON * EPSILON)
    {
        x_axis = cross({ 0.0f, 0.0f, 1.0f }, z_axis);
    }
    x_axis            = normalize(x_axis);
    const Vec3 y_axis = cross(z_axis, x_axis);
    return quat_from_axes(x_axis, y_axis, z_axis);
}

Quat slerp (Quat a, Quat b, f32 t)
{
    f32 cos_theta = dot(a, b);
    if (cos_theta < 0.0f)
    {
        b         = -b;
        cos_theta = -cos_theta;
    }
    if (cos_theta > 0.9995f)
    {
        return nlerp(a, b, t);
    }
    const f32 theta     = acos(cos_theta);
    const f32 sin_theta = sin(theta);
    const f32 wa        = sin((1.0f - t) * theta) / sin_theta;
    const f32 wb        = sin(t * theta) / sin_theta;
    return normalize(a * wa + b * wb);
}

Quat nlerp (Quat a, Quat b, f32 t)
{
    if (dot(a, b) < 0.0f)
    {
        b = -b;
    }
    return normalize(a * (1.0f - t) + b * t);
}

//
// Pose
//

Pose pose_mul (const Pose& a, const Pose& b)
{
    return {
        .position    = a.position + rotate(a.orientation, b.position),
        .orientation = a.orientation * b.orientation,
    };
}

Pose pose_inverse (const Pose& a)
{
    const Quat inv = inverse(a.orientation);
    return {
        .position    = rotate(inv, -a.position),
        .orientation = inv,
    };
}

Pose pose_relative (const Pose& base, const Pose& target)
{
    return pose_mul(pose_inverse(base), target);
}

Vec3 transform_point (const Pose& pose, Vec3 point)
{
    return pose.position + rotate(pose.orientation, point);
}

Vec3 transform_vector (const Pose& pose, Vec3 vector)
{
    return rotate(pose.orientation, vector);
}

b8 is_finite (const Pose& pose)
{
    return is_finite(pose.position.x) && is_finite(pose.position.y) && is_finite(pose.position.z) &&
           is_finite(pose.orientation.x) && is_finite(pose.orientation.y) && is_finite(pose.orientation.z) && is_finite(pose.orientation.w);
}

//
// Mat4
//

Mat4 operator* (const Mat4& a, const Mat4& b)
{
    Mat4 r = {};
    for (u32 c = 0; c < 4; c++)
    {
        for (u32 row = 0; row < 4; row++)
        {
            r.m[c][row] = a.m[0][row] * b.m[c][0] + a.m[1][row] * b.m[c][1] + a.m[2][row] * b.m[c][2] + a.m[3][row] * b.m[c][3];
        }
    }
    return r;
}

Vec4 operator* (const Mat4& m, Vec4 v)
{
    return {
        m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z + m.m[3][0] * v.w,
        m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z + m.m[3][1] * v.w,
        m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z + m.m[3][2] * v.w,
        m.m[0][3] * v.x + m.m[1][3] * v.y + m.m[2][3] * v.z + m.m[3][3] * v.w,
    };
}

Mat4 transpose (const Mat4& m)
{
    Mat4 r = {};
    for (u32 c = 0; c < 4; c++)
    {
        for (u32 row = 0; row < 4; row++)
        {
            r.m[c][row] = m.m[row][c];
        }
    }
    return r;
}

Mat4 inverse (const Mat4& mat)
{
    const f32* m = &mat.m[0][0];
    f32        inv[16];

    inv[0]  =  m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8]  =  m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5]  =  m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9]  = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] =  m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2]  =  m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6]  = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] =  m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3]  = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7]  =  m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] =  m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f)
    {
        return MAT4_IDENTITY;
    }

    const f32 inv_det = 1.0f / det;
    Mat4      r = {};
    for (u32 i = 0; i < 16; i++)
    {
        (&r.m[0][0])[i] = inv[i] * inv_det;
    }
    return r;
}

Mat4 mat4_translation (Vec3 t)
{
    Mat4 r  = MAT4_IDENTITY;
    r.m[3][0] = t.x;
    r.m[3][1] = t.y;
    r.m[3][2] = t.z;
    return r;
}

Mat4 mat4_scale (Vec3 s)
{
    Mat4 r  = MAT4_IDENTITY;
    r.m[0][0] = s.x;
    r.m[1][1] = s.y;
    r.m[2][2] = s.z;
    return r;
}

Mat4 mat4_from_quat (Quat q)
{
    const f32 xx = q.x * q.x;
    const f32 yy = q.y * q.y;
    const f32 zz = q.z * q.z;
    const f32 xy = q.x * q.y;
    const f32 xz = q.x * q.z;
    const f32 yz = q.y * q.z;
    const f32 wx = q.w * q.x;
    const f32 wy = q.w * q.y;
    const f32 wz = q.w * q.z;

    Mat4 r = MAT4_IDENTITY;
    r.m[0][0] = 1.0f - 2.0f * (yy + zz);
    r.m[0][1] = 2.0f * (xy + wz);
    r.m[0][2] = 2.0f * (xz - wy);
    r.m[1][0] = 2.0f * (xy - wz);
    r.m[1][1] = 1.0f - 2.0f * (xx + zz);
    r.m[1][2] = 2.0f * (yz + wx);
    r.m[2][0] = 2.0f * (xz + wy);
    r.m[2][1] = 2.0f * (yz - wx);
    r.m[2][2] = 1.0f - 2.0f * (xx + yy);
    return r;
}

Mat4 mat4_from_pose (const Pose& pose)
{
    Mat4 r  = mat4_from_quat(pose.orientation);
    r.m[3][0] = pose.position.x;
    r.m[3][1] = pose.position.y;
    r.m[3][2] = pose.position.z;
    return r;
}

Mat4 mat4_projection (const Fov& fov, f32 near_z, f32 far_z)
{
    const f32 tl = tan(fov.angle_left);
    const f32 tr = tan(fov.angle_right);
    const f32 tu = tan(fov.angle_up);
    const f32 td = tan(fov.angle_down);
    const f32 w  = tr - tl;
    const f32 h  = tu - td;

    Mat4 r = {};
    r.m[0][0] = 2.0f / w;
    r.m[2][0] = (tr + tl) / w;
    r.m[1][1] = -2.0f / h;
    r.m[2][1] = -(tu + td) / h;
    r.m[2][2] = far_z / (near_z - far_z);
    r.m[3][2] = far_z * near_z / (near_z - far_z);
    r.m[2][3] = -1.0f;
    return r;
}

Mat4 mat4_look_at (Vec3 eye, Vec3 target, Vec3 up)
{
    const Vec3 f = normalize(target - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);

    Mat4 r = MAT4_IDENTITY;
    r.m[0][0] = s.x;  r.m[1][0] = s.y;  r.m[2][0] = s.z;
    r.m[0][1] = u.x;  r.m[1][1] = u.y;  r.m[2][1] = u.z;
    r.m[0][2] = -f.x; r.m[1][2] = -f.y; r.m[2][2] = -f.z;
    r.m[3][0] = -dot(s, eye);
    r.m[3][1] = -dot(u, eye);
    r.m[3][2] = dot(f, eye);
    return r;
}

Vec3 transform_point (const Mat4& m, Vec3 p)
{
    return xyz(m * Vec4{ p.x, p.y, p.z, 1.0f });
}

Vec3 transform_vector (const Mat4& m, Vec3 v)
{
    return xyz(m * Vec4{ v.x, v.y, v.z, 0.0f });
}

} // namespace zm
