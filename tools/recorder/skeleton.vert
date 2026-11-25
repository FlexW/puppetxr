#version 450
// A box per bone or joint of the hand skeleton (docs/recorder.md, Rendering): from start to end,
// grown by the radius on every side, so a joint (start equals end) is a cube

layout(location = 0) in vec4 start_radius;
layout(location = 1) in vec4 end_color;     // w: 0 left bone, 1 right bone, 2 joint

layout(push_constant) uniform Push
{
    mat4 view_projection;
} push;

layout(location = 0) out vec3 out_color;

const vec3 corners[8] = vec3[](
    vec3(-1, -1, 0), vec3(1, -1, 0), vec3(1, 1, 0), vec3(-1, 1, 0),
    vec3(-1, -1, 1), vec3(1, -1, 1), vec3(1, 1, 1), vec3(-1, 1, 1));
const int faces[36] = int[](
    0, 2, 1, 0, 3, 2,
    4, 5, 6, 4, 6, 7,
    0, 1, 5, 0, 5, 4,
    1, 2, 6, 1, 6, 5,
    2, 3, 7, 2, 7, 6,
    3, 0, 4, 3, 4, 7);
const vec3 normals[6] = vec3[](
    vec3(0, 0, -1), vec3(0, 0, 1), vec3(0, -1, 0), vec3(1, 0, 0), vec3(0, 1, 0), vec3(-1, 0, 0));
const vec3 colors[3] = vec3[](vec3(0.25, 0.65, 1.0), vec3(1.0, 0.6, 0.15), vec3(0.95, 0.95, 0.95));

void main()
{
    vec3  axis   = end_color.xyz - start_radius.xyz;
    float length = length(axis);
    vec3  z      = length > 1e-5 ? axis / length : vec3(0, 0, 1);
    vec3  x      = normalize(cross(abs(z.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0), z));
    vec3  y      = cross(z, x);
    float radius = start_radius.w;
    vec3  corner = corners[faces[gl_VertexIndex]];
    vec3  world  = start_radius.xyz - z * radius + x * (corner.x * radius) + y * (corner.y * radius) + z * (corner.z * (length + 2.0 * radius));
    vec3  normal = normals[gl_VertexIndex / 6];
    vec3  facing = x * normal.x + y * normal.y + z * normal.z;
    float light  = 0.5 + 0.5 * abs(dot(facing, normalize(vec3(0.3, 0.8, 0.5))));
    out_color    = colors[int(end_color.w + 0.5)] * light;
    gl_Position  = push.view_projection * vec4(world, 1.0);
}
