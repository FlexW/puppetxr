#ifndef PUPPETXR_SHADER_H
#define PUPPETXR_SHADER_H

// Prelude of the shader dialect (docs/shaders.md): GLSL with macros for declarations and the
// few constructs GLSL and HLSL spell differently. The build or the backend defines one of
// R_SHADER_VULKAN, R_SHADER_GL, R_SHADER_GLES, R_SHADER_HLSL and one of R_STAGE_VERTEX,
// R_STAGE_FRAGMENT. GL and GLES pick the sampler of R_TEXTURE_LAYERED with R_SAMPLER_2D or
// R_SAMPLER_CUBE (default 2D array), because their swapchain textures have no array views.

#if defined(R_SHADER_VULKAN) || defined(R_SHADER_GL) || defined(R_SHADER_GLES)
#define R_SHADER_GLSL 1
#endif

#define R_NO_FIELDS(X)
#define R_FIELD(type, name)              type name;
#define R_FIELD_ARRAY(type, name, count) type name[count];

//
// GLSL: Vulkan GLSL 450, GL 330 core, GLSL ES 300
//

#ifdef R_SHADER_GLSL

#ifdef R_SHADER_GLES
precision highp float;
precision highp int;
precision highp sampler2D;
precision highp sampler2DArray;
precision highp samplerCube;
#endif

#define atan2(y, x)             atan(y, x)
#define mul(a, b)               ((a) * (b))
#define r_mat3_cols(c0, c1, c2) mat3(c0, c1, c2)
#define r_splat2(x)             vec2(x)
#define r_splat3(x)             vec3(x)
#define r_splat4(x)             vec4(x)

#define r_sample(t, uv) texture(t, uv)

// Texel xy of array layer `layer` of an R_TEXTURE_ARRAY, unfiltered
#define r_fetch_array(t, xy, layer) texelFetch(t, ivec3(xy, layer), 0)

#ifdef R_SHADER_VULKAN
#define R_PUSH_BLOCK(FIELDS)             layout(push_constant) uniform R_Push { FIELDS(R_FIELD, R_FIELD_ARRAY) } push;
#define R_TEXTURE_2D(name, slot)         layout(set = 0, binding = slot) uniform sampler2D name;
#define R_TEXTURE_LAYERED(name, slot)    layout(set = 0, binding = slot) uniform sampler2DArray name;
#define R_TEXTURE_ARRAY(name, slot)      layout(set = 0, binding = slot) uniform sampler2DArray name;
#define R_ATTRIBUTE(type, name, loc)     layout(location = loc) in type name;
#ifdef R_STAGE_VERTEX
#define R_VARYING(type, name, loc)       layout(location = loc) out type name;
#else
#define R_VARYING(type, name, loc)       layout(location = loc) in type name;
#endif
#define R_VERTEX_ID gl_VertexIndex
#else
// Before GL 4.2 and ES 3.1 there are no explicit bindings and varyings match by name; the backend binds the block R_Push and the samplers by name
#define R_PUSH_BLOCK(FIELDS)             layout(std140) uniform R_Push { FIELDS(R_FIELD, R_FIELD_ARRAY) } push;
#define R_TEXTURE_2D(name, slot)         uniform sampler2D name;
#if defined(R_SAMPLER_2D)
#define R_TEXTURE_LAYERED(name, slot)    uniform sampler2D name;
#elif defined(R_SAMPLER_CUBE)
#define R_TEXTURE_LAYERED(name, slot)    uniform samplerCube name;
#else
#define R_TEXTURE_LAYERED(name, slot)    uniform sampler2DArray name;
#endif
#define R_TEXTURE_ARRAY(name, slot)      uniform sampler2DArray name;
#define R_ATTRIBUTE(type, name, loc)     layout(location = loc) in type name;
#ifdef R_STAGE_VERTEX
#define R_VARYING(type, name, loc)       out type name;
#else
#define R_VARYING(type, name, loc)       in type name;
#endif
#define R_VERTEX_ID gl_VertexID
#endif

#if defined(R_SAMPLER_CUBE) && !defined(R_SHADER_VULKAN)
// The direction a samplerCube maps to face `layer` at st, the inverse of the face selection
// (Vulkan 16.5.4, the same table in GL); faces are +X, -X, +Y, -Y, +Z, -Z
vec3 r_cube_direction (vec2 st, float layer)
{
    vec2 c    = st * 2.0 - 1.0;
    int  face = int(layer + 0.5);
    if (face == 0)
    {
        return vec3(1.0, -c.y, -c.x);
    }
    if (face == 1)
    {
        return vec3(-1.0, -c.y, c.x);
    }
    if (face == 2)
    {
        return vec3(c.x, 1.0, c.y);
    }
    if (face == 3)
    {
        return vec3(c.x, -1.0, -c.y);
    }
    if (face == 4)
    {
        return vec3(c.x, -c.y, 1.0);
    }
    return vec3(-c.x, -c.y, -1.0);
}
#define r_sample_layer(t, uv, layer)          texture(t, r_cube_direction(uv, layer))
#define r_sample_layer_lod(t, uv, layer, lod) textureLod(t, r_cube_direction(uv, layer), lod)
#elif defined(R_SAMPLER_2D) && !defined(R_SHADER_VULKAN)
#define r_sample_layer(t, uv, layer)          texture(t, uv)
#define r_sample_layer_lod(t, uv, layer, lod) textureLod(t, uv, lod)
#else
#define r_sample_layer(t, uv, layer)          texture(t, vec3(uv, layer))
#define r_sample_layer_lod(t, uv, layer, lod) textureLod(t, vec3(uv, layer), lod)
#endif

#define R_VERTEX_INPUTS(FIELDS) FIELDS(R_ATTRIBUTE)
#define R_VARYINGS(FIELDS)      FIELDS(R_VARYING)
#define R_IN(name)              name
#define R_VAR(name)             name
#define R_POSITION              gl_Position
#define R_COLOR                 r_color
#define R_DEPTH                 gl_FragDepth
#define R_FRAG_COORD            gl_FragCoord

#if defined(R_SHADER_VULKAN) || defined(R_CLIP_ZERO_TO_ONE)
#define R_VERTEX_MAIN_BEGIN void main() {
#else
// Clip depth is [0, 1] as in Vulkan and D3D; GL without glClipControl expects [-w, w]
#define R_VERTEX_MAIN_BEGIN void r_main(); void main() { r_main(); gl_Position.z = 2.0 * gl_Position.z - gl_Position.w; } void r_main() {
#endif
#define R_VERTEX_RETURN             return
#define R_VERTEX_MAIN_END           }
#define R_FRAGMENT_MAIN_BEGIN       layout(location = 0) out vec4 r_color; void main() {
#define R_FRAGMENT_MAIN_END         }
// A fragment shader that writes R_DEPTH as well, on every path that does not discard
#define R_FRAGMENT_COLOR_DEPTH_MAIN_BEGIN R_FRAGMENT_MAIN_BEGIN
#define R_FRAGMENT_COLOR_DEPTH_MAIN_END   }
#define R_FRAGMENT_DEPTH_MAIN_BEGIN void main() {
#define R_FRAGMENT_DEPTH_MAIN_END   }

#endif

//
// HLSL: shader model 5.0 (D3DCompile) and 6.0 (dxc); entry point main
//

#ifdef R_SHADER_HLSL

#define vec2  float2
#define vec3  float3
#define vec4  float4
#define ivec2 int2
#define ivec3 int3
#define ivec4 int4
#define uvec2 uint2
#define uvec3 uint3
#define uvec4 uint4
#define bvec2 bool2
#define bvec3 bool3
#define bvec4 bool4
#define mat3  float3x3
#define mat4  float4x4

#ifdef R_HLSL_NO_ATAN2
// Wine's d3dcompiler, which cross builds run (preset mingw-debug), has neither atan nor atan2;
// a polynomial with an error below 1e-5 stands in for tests under Wine
float r_atan2 (float y, float x)
{
    const float pi = 3.14159265358979;
    float ax = abs(x);
    float ay = abs(y);
    float t  = min(ax, ay) / max(max(ax, ay), 1e-30);
    float t2 = t * t;
    float a  = t * (0.99997726 + t2 * (-0.33262347 + t2 * (0.19354346 + t2 * (-0.11643287 + t2 * (0.05265332 + t2 * -0.01172120)))));
    a = ay > ax ? pi * 0.5 - a : a;
    a = x < 0.0 ? pi - a : a;
    return y < 0.0 ? -a : a;
}
#define atan2(y, x) r_atan2(y, x)
#endif

#define mix              lerp
#define fract            frac
#define dFdx             ddx
#define dFdy             ddy
#define inversesqrt      rsqrt
#define lessThan(a, b)    ((a) < (b))
#define greaterThan(a, b) ((a) > (b))

// HLSL constructors fill rows; matrices in constant buffers are column major as in GLSL, so mul(m, v) is m * v
#define r_mat3_cols(c0, c1, c2) transpose(float3x3(c0, c1, c2))
#define r_splat2(x)             ((float2)(x))
#define r_splat3(x)             ((float3)(x))
#define r_splat4(x)             ((float4)(x))

#define R_PUSH_BLOCK(FIELDS)          struct R_PushData { FIELDS(R_FIELD, R_FIELD_ARRAY) }; cbuffer R_Push : register(b0) { R_PushData push; };
#define R_TEXTURE_2D(name, slot)      Texture2D name : register(t##slot); SamplerState name##_sampler : register(s##slot);
#define R_TEXTURE_LAYERED(name, slot) Texture2DArray name : register(t##slot); SamplerState name##_sampler : register(s##slot);
#define R_TEXTURE_ARRAY(name, slot)   Texture2DArray name : register(t##slot); SamplerState name##_sampler : register(s##slot);

#define r_sample(t, uv)                       t.Sample(t##_sampler, uv)
#define r_sample_layer(t, uv, layer)          t.Sample(t##_sampler, float3(uv, layer))
#define r_sample_layer_lod(t, uv, layer, lod) t.SampleLevel(t##_sampler, float3(uv, layer), lod)
#define r_fetch_array(t, xy, layer)           t.Load(int4(xy, layer, 0))

// Inputs and outputs live in statics so that helper functions reach them as in GLSL
#define R_ATTRIBUTE(type, name, loc)      type name : ATTRIBUTE##loc;
#define R_VARYING(type, name, loc)        type name : TEXCOORD##loc;
#define R_VERTEX_INPUTS(FIELDS)           struct R_VertexIn { uint r_vertex_id : SV_VertexID; FIELDS(R_ATTRIBUTE) }; static R_VertexIn r_vin;
#ifdef R_STAGE_VERTEX
#define R_VARYINGS(FIELDS)                struct R_VertexOut { float4 r_position : SV_Position; FIELDS(R_VARYING) }; static R_VertexOut r_vout;
#define R_VAR(name)                       r_vout.name
#else
#define R_VARYINGS(FIELDS)                struct R_FragmentIn { float4 r_position : SV_Position; FIELDS(R_VARYING) }; static R_FragmentIn r_fin;
#define R_VAR(name)                       r_fin.name
#endif
#define R_IN(name)                        r_vin.name
#define R_VERTEX_ID                       int(r_vin.r_vertex_id)
#define R_POSITION                        r_vout.r_position
#define R_COLOR                           r_color
#define R_DEPTH                           r_depth
#define R_FRAG_COORD                      r_fin.r_position

// D3D puts NDC y = +1 at row 0 where Vulkan puts y = -1 (docs/shaders.md, Clip space)
#define R_VERTEX_MAIN_BEGIN         void r_main(); R_VertexOut main(R_VertexIn input) { r_vin = input; r_vout = (R_VertexOut)0; r_main(); r_vout.r_position.y = -r_vout.r_position.y; return r_vout; } void r_main() {
#define R_VERTEX_RETURN             return
#define R_VERTEX_MAIN_END           }
#define R_FRAGMENT_MAIN_BEGIN       static float4 r_color; void r_main(); float4 main(R_FragmentIn input) : SV_Target { r_fin = input; r_color = (float4)0; r_main(); return r_color; } void r_main() {
#define R_FRAGMENT_MAIN_END         }
#define R_FRAGMENT_COLOR_DEPTH_MAIN_BEGIN static float4 r_color; static float r_depth; struct R_FragmentOut { float4 color : SV_Target; float depth : SV_Depth; }; void r_main(); R_FragmentOut main(R_FragmentIn input) { r_fin = input; r_color = (float4)0; r_depth = 0.0; r_main(); R_FragmentOut output; output.color = r_color; output.depth = r_depth; return output; } void r_main() {
#define R_FRAGMENT_COLOR_DEPTH_MAIN_END   }
#define R_FRAGMENT_DEPTH_MAIN_BEGIN void main() {
#define R_FRAGMENT_DEPTH_MAIN_END   }

#endif

//
// Both dialects
//

// A value in [0, 1] as 24 bits in the rgb of an RGBA8 UNORM target, which stores them exactly:
// render targets of every backend can hold it, where float formats need extensions on GLES
vec4 r_pack_unorm24 (float value)
{
    uint bits = uint(value * 16777215.0 + 0.5);
    return vec4(float(bits >> 16u), float((bits >> 8u) & 255u), float(bits & 255u), 255.0) / 255.0;
}

float r_unpack_unorm24 (vec4 rgba)
{
    uvec3 bytes = uvec3(rgba.rgb * 255.0 + 0.5);
    return float((bytes.r << 16u) | (bytes.g << 8u) | bytes.b) / 16777215.0;
}

#endif
