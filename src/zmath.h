#pragma once

#include "platform.h"

namespace zm
{

//
// Types
//

struct Vec2
{
    f32 x;
    f32 y;
};

struct Vec3
{
    f32 x;
    f32 y;
    f32 z;
};

struct Vec4
{
    f32 x;
    f32 y;
    f32 z;
    f32 w;
};

struct Quat
{
    f32 x;
    f32 y;
    f32 z;
    f32 w;
};

// Column major: m[column][row], matching Vulkan and OpenXR conventions
struct Mat4
{
    f32 m[4][4];
};

struct Pose
{
    Vec3 position;
    Quat orientation;
};

// Angles in radians, left and down are negative, like XrFovf
struct Fov
{
    f32 angle_left;
    f32 angle_right;
    f32 angle_up;
    f32 angle_down;
};

constexpr f32 PI         = 3.14159265358979323846f;
constexpr f32 TAU        = 6.28318530717958647692f;
constexpr f32 DEG_TO_RAD = PI / 180.0f;
constexpr f32 RAD_TO_DEG = 180.0f / PI;
constexpr f32 F32_MAX    = 3.402823466e+38f;
constexpr f32 EPSILON    = 1e-6f;

constexpr Quat QUAT_IDENTITY = { 0.0f, 0.0f, 0.0f, 1.0f };
constexpr Pose POSE_IDENTITY = { { 0.0f, 0.0f, 0.0f }, QUAT_IDENTITY };
constexpr Mat4 MAT4_IDENTITY = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } } };

//
// Scalars
//
// All results are bit-identical across platforms: no libm, no FMA

f32  sqrt     (f32 x);
f32  sin      (f32 x);
f32  cos      (f32 x);
f32  tan      (f32 x);
f32  asin     (f32 x);
f32  acos     (f32 x);
f32  atan     (f32 x);
f32  atan2    (f32 y, f32 x);
f32  exp      (f32 x);
f32  log      (f32 x);
f32  pow      (f32 x, f32 y);
f32  floor    (f32 x);
f32  ceil     (f32 x);
f32  round    (f32 x);
f32  trunc    (f32 x);
f32  fmod     (f32 x, f32 y);
b8   is_finite(f32 x);
f32  nan      (); // quiet NaN, 0x7fc00000

inline f32 abs   (f32 x)               { return x < 0.0f ? -x : x; }
inline f32 min   (f32 a, f32 b)        { return a < b ? a : b; }
inline f32 max   (f32 a, f32 b)        { return a > b ? a : b; }
inline i32 min   (i32 a, i32 b)        { return a < b ? a : b; }
inline i32 max   (i32 a, i32 b)        { return a > b ? a : b; }
inline u32 min   (u32 a, u32 b)        { return a < b ? a : b; }
inline u32 max   (u32 a, u32 b)        { return a > b ? a : b; }
inline u64 min   (u64 a, u64 b)        { return a < b ? a : b; }
inline u64 max   (u64 a, u64 b)        { return a > b ? a : b; }
inline i64 min   (i64 a, i64 b)        { return a < b ? a : b; }
inline i64 max   (i64 a, i64 b)        { return a > b ? a : b; }
#if !USIZE_IS_U64
inline usize min (usize a, usize b)    { return a < b ? a : b; }
inline usize max (usize a, usize b)    { return a > b ? a : b; }
#endif
inline f32 clamp (f32 x, f32 lo, f32 hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline i32 clamp (i32 x, i32 lo, i32 hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline u32 clamp (u32 x, u32 lo, u32 hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline f32 lerp  (f32 a, f32 b, f32 t) { return a + (b - a) * t; }
inline f32 sign  (f32 x)               { return x < 0.0f ? -1.0f : (x > 0.0f ? 1.0f : 0.0f); }
inline b8  near  (f32 a, f32 b, f32 tolerance = EPSILON) { return abs(a - b) <= tolerance; }

//
// Vec2
//

inline Vec2 operator+ (Vec2 a, Vec2 b)  { return { a.x + b.x, a.y + b.y }; }
inline Vec2 operator- (Vec2 a, Vec2 b)  { return { a.x - b.x, a.y - b.y }; }
inline Vec2 operator* (Vec2 a, f32 s)   { return { a.x * s, a.y * s }; }
inline Vec2 operator* (f32 s, Vec2 a)   { return { a.x * s, a.y * s }; }
inline Vec2 operator* (Vec2 a, Vec2 b)  { return { a.x * b.x, a.y * b.y }; }
inline Vec2 operator/ (Vec2 a, f32 s)   { return { a.x / s, a.y / s }; }
inline Vec2 operator- (Vec2 a)          { return { -a.x, -a.y }; }
inline Vec2& operator+= (Vec2& a, Vec2 b) { a = a + b; return a; }
inline Vec2& operator-= (Vec2& a, Vec2 b) { a = a - b; return a; }
inline Vec2& operator*= (Vec2& a, f32 s)  { a = a * s; return a; }

inline f32  dot       (Vec2 a, Vec2 b)        { return a.x * b.x + a.y * b.y; }
inline f32  length_sq (Vec2 a)                { return dot(a, a); }
inline f32  length    (Vec2 a)                { return sqrt(dot(a, a)); }
inline Vec2 lerp      (Vec2 a, Vec2 b, f32 t) { return a + (b - a) * t; }
inline b8   equal     (Vec2 a, Vec2 b)        { return a.x == b.x && a.y == b.y; }
inline b8   near      (Vec2 a, Vec2 b, f32 tolerance = EPSILON) { return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance); }
Vec2        normalize (Vec2 a);

//
// Vec3
//

inline Vec3 operator+ (Vec3 a, Vec3 b)  { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator- (Vec3 a, Vec3 b)  { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator* (Vec3 a, f32 s)   { return { a.x * s, a.y * s, a.z * s }; }
inline Vec3 operator* (f32 s, Vec3 a)   { return { a.x * s, a.y * s, a.z * s }; }
inline Vec3 operator* (Vec3 a, Vec3 b)  { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
inline Vec3 operator/ (Vec3 a, f32 s)   { return { a.x / s, a.y / s, a.z / s }; }
inline Vec3 operator- (Vec3 a)          { return { -a.x, -a.y, -a.z }; }
inline Vec3& operator+= (Vec3& a, Vec3 b) { a = a + b; return a; }
inline Vec3& operator-= (Vec3& a, Vec3 b) { a = a - b; return a; }
inline Vec3& operator*= (Vec3& a, f32 s)  { a = a * s; return a; }

inline f32  dot       (Vec3 a, Vec3 b)        { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross     (Vec3 a, Vec3 b)        { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline f32  length_sq (Vec3 a)                { return dot(a, a); }
inline f32  length    (Vec3 a)                { return sqrt(dot(a, a)); }
inline Vec3 lerp      (Vec3 a, Vec3 b, f32 t) { return a + (b - a) * t; }
inline b8   equal     (Vec3 a, Vec3 b)        { return a.x == b.x && a.y == b.y && a.z == b.z; }
inline b8   near      (Vec3 a, Vec3 b, f32 tolerance = EPSILON) { return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance); }
inline Vec3 min       (Vec3 a, Vec3 b)        { return { min(a.x, b.x), min(a.y, b.y), min(a.z, b.z) }; }
inline Vec3 max       (Vec3 a, Vec3 b)        { return { max(a.x, b.x), max(a.y, b.y), max(a.z, b.z) }; }
Vec3        normalize (Vec3 a);

//
// Vec4
//

inline Vec4 operator+ (Vec4 a, Vec4 b)  { return { a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w }; }
inline Vec4 operator- (Vec4 a, Vec4 b)  { return { a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w }; }
inline Vec4 operator* (Vec4 a, f32 s)   { return { a.x * s, a.y * s, a.z * s, a.w * s }; }
inline Vec4 operator* (f32 s, Vec4 a)   { return { a.x * s, a.y * s, a.z * s, a.w * s }; }
inline Vec4 operator- (Vec4 a)          { return { -a.x, -a.y, -a.z, -a.w }; }

inline f32  dot   (Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline Vec3 xyz   (Vec4 a)         { return { a.x, a.y, a.z }; }
inline Vec4 vec4  (Vec3 a, f32 w)  { return { a.x, a.y, a.z, w }; }
inline b8   equal (Vec4 a, Vec4 b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

//
// Quat
//

Quat operator* (Quat a, Quat b);

inline Quat operator+ (Quat a, Quat b)  { return { a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w }; }
inline Quat operator* (Quat a, f32 s)   { return { a.x * s, a.y * s, a.z * s, a.w * s }; }
inline Quat operator- (Quat a)          { return { -a.x, -a.y, -a.z, -a.w }; }

inline f32  dot       (Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline f32  length    (Quat a)         { return sqrt(dot(a, a)); }
inline Quat conjugate (Quat a)         { return { -a.x, -a.y, -a.z, a.w }; }
inline b8   equal     (Quat a, Quat b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
inline b8   near      (Quat a, Quat b, f32 tolerance = EPSILON) { return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance) && near(a.w, b.w, tolerance); }

Quat normalize            (Quat a);
Quat inverse              (Quat a);
Vec3 rotate               (Quat q, Vec3 v);
Quat quat_from_axis_angle (Vec3 axis, f32 angle);
Quat quat_from_euler      (Vec3 euler); // pitch (x), yaw (y), roll (z), applied as yaw * pitch * roll
Vec3 euler_from_quat      (Quat q);
Quat quat_look_rotation   (Vec3 forward, Vec3 up);
Quat slerp                (Quat a, Quat b, f32 t);
Quat nlerp                (Quat a, Quat b, f32 t);

//
// Pose
//

Pose pose_mul       (const Pose& a, const Pose& b);     // a then b, i.e. b expressed in a's frame
Pose pose_inverse   (const Pose& a);
Pose pose_relative  (const Pose& base, const Pose& target); // target in base's frame, so pose_mul(base, result) == target
Vec3 transform_point  (const Pose& pose, Vec3 point);
Vec3 transform_vector (const Pose& pose, Vec3 vector);
b8   is_finite      (const Pose& pose);

inline b8 equal (const Pose& a, const Pose& b) { return equal(a.position, b.position) && equal(a.orientation, b.orientation); }
inline b8 near  (const Pose& a, const Pose& b, f32 tolerance = EPSILON) { return near(a.position, b.position, tolerance) && near(a.orientation, b.orientation, tolerance); }

//
// Mat4
//

Mat4 operator* (const Mat4& a, const Mat4& b);
Vec4 operator* (const Mat4& m, Vec4 v);

Mat4 transpose        (const Mat4& m);
Mat4 inverse          (const Mat4& m);
Mat4 mat4_translation (Vec3 t);
Mat4 mat4_scale       (Vec3 s);
Mat4 mat4_from_quat   (Quat q);
Mat4 mat4_from_pose   (const Pose& pose);
Mat4 mat4_projection  (const Fov& fov, f32 near_z, f32 far_z); // right handed, depth 0..1, Vulkan clip space (y down)
Mat4 mat4_look_at     (Vec3 eye, Vec3 target, Vec3 up);
Vec3 transform_point  (const Mat4& m, Vec3 point);
Vec3 transform_vector (const Mat4& m, Vec3 vector);

} // namespace zm

using zm::Vec2;
using zm::Vec3;
using zm::Vec4;
using zm::Quat;
using zm::Mat4;
using zm::Pose;
using zm::Fov;
