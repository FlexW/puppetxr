#include "shader.h"
#include "blocks.h"

R_PUSH_BLOCK(COMP_PASSTHROUGH_PUSH_FIELDS)
R_TEXTURE_2D(albedo, 0)
R_TEXTURE_2D(color_map, 1) // 256 x 1, indexed by luminance, or the LUT atlas of color maps 4 and 5

#define VARYINGS(X)    \
    X(vec2, v_uv, 0)

R_VARYINGS(VARYINGS)

vec3 srgb_encode (vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(max(c, r_splat3(0.0)), r_splat3(1.0 / 2.4)) - 0.055, step(r_splat3(0.0031308), c));
}

vec3 srgb_decode (vec3 c)
{
    return mix(c / 12.92, pow(max((c + 0.055) / 1.055, r_splat3(0.0)), r_splat3(2.4)), step(r_splat3(0.04045), c));
}

// XR_FB_passthrough: brightness, contrast and saturation act in CIELAB, white point D65
float lab_f (float t)
{
    return t > 0.008856 ? pow(max(t, 0.0), 1.0 / 3.0) : t / 0.128418 + 4.0 / 29.0;
}

float lab_f_inverse (float t)
{
    return t > 6.0 / 29.0 ? t * t * t : 0.128418 * (t - 4.0 / 29.0);
}

vec3 linear_to_lab (vec3 c)
{
    const mat3 to_xyz = r_mat3_cols(vec3(0.4124, 0.2126, 0.0193), vec3(0.3576, 0.7152, 0.1192), vec3(0.1805, 0.0722, 0.9505));
    vec3 xyz = mul(to_xyz, c) / vec3(0.95047, 1.0, 1.08883);
    vec3 f   = vec3(lab_f(xyz.x), lab_f(xyz.y), lab_f(xyz.z));
    return vec3(116.0 * f.y - 16.0, 500.0 * (f.x - f.y), 200.0 * (f.y - f.z));
}

vec3 lab_to_linear (vec3 lab)
{
    const mat3 to_rgb = r_mat3_cols(vec3(3.2406, -0.9689, 0.0557), vec3(-1.5372, 1.8758, -0.2040), vec3(-0.4986, 0.0415, 1.0570));
    float fy  = (lab.x + 16.0) / 116.0;
    vec3  xyz = vec3(lab_f_inverse(fy + lab.y / 500.0), lab_f_inverse(fy), lab_f_inverse(fy - lab.z / 200.0)) * vec3(0.95047, 1.0, 1.08883);
    return clamp(mul(to_rgb, xyz), 0.0, 1.0);
}

// XR_META_passthrough_color_lut: element i of the LUT of resolution n starting at row base, at
// its texel center
vec4 lut_element (vec3 i, float n, float base)
{
    float tiles = float(COMP_COLOR_LUT_ATLAS_WIDTH) / n;
    float row   = floor(i.z / tiles);
    vec2  texel = vec2((i.z - row * tiles) * n + i.x, base + row * n + i.y) + r_splat2(0.5);
    return r_sample(color_map, texel / vec2(float(COMP_COLOR_LUT_ATLAS_WIDTH), float(COMP_COLOR_LUT_ATLAS_HEIGHT)));
}

// Spec: trilinear between the elements around an sRGB color
vec4 sample_lut (vec3 c, int lut)
{
    float n    = float(lut == 0 ? (push.lut_resolution & 255) : (push.lut_resolution >> 8));
    float base = lut == 0 ? 0.0 : float(COMP_COLOR_LUT_ATLAS_HEIGHT / 2);
    vec3  p  = clamp(c, 0.0, 1.0) * (n - 1.0);
    vec3  i0 = floor(p);
    vec3  i1 = min(i0 + r_splat3(1.0), r_splat3(n - 1.0));
    vec3  f  = p - i0;
    vec4  x0 = mix(lut_element(vec3(i0.x, i0.y, i0.z), n, base), lut_element(vec3(i1.x, i0.y, i0.z), n, base), f.x);
    vec4  x1 = mix(lut_element(vec3(i0.x, i1.y, i0.z), n, base), lut_element(vec3(i1.x, i1.y, i0.z), n, base), f.x);
    vec4  x2 = mix(lut_element(vec3(i0.x, i0.y, i1.z), n, base), lut_element(vec3(i1.x, i0.y, i1.z), n, base), f.x);
    vec4  x3 = mix(lut_element(vec3(i0.x, i1.y, i1.z), n, base), lut_element(vec3(i1.x, i1.y, i1.z), n, base), f.x);
    return mix(mix(x0, x1, f.y), mix(x2, x3, f.y), f.z);
}

R_FRAGMENT_MAIN_BEGIN
    vec4  color = vec4(r_sample(albedo, R_VAR(v_uv)).rgb, 1.0);
    float luma  = dot(srgb_encode(color.rgb), vec3(0.299, 0.587, 0.114));
    vec2  index = vec2((floor(clamp(luma, 0.0, 1.0) * 255.0) + 0.5) / 256.0, 0.5);

    if (push.color_map == 1)
    {
        vec4 mapped = r_sample(color_map, index);
        color       = vec4(srgb_decode(mapped.rgb), mapped.a);
    }
    else if (push.color_map == 2)
    {
        color = vec4(srgb_decode(r_splat3(r_sample(color_map, index).r)), 1.0);
    }
    else if (push.color_map == 3)
    {
        vec3 lab = linear_to_lab(color.rgb);
        lab.x    = clamp((lab.x - 50.0) * push.adjust.z + 50.0, 0.0, 100.0);
        lab.x    = clamp(lab.x + push.adjust.y, 0.0, 100.0);
        lab.yz  *= push.adjust.w;
        color.rgb = lab_to_linear(lab);
    }
    // Spec: map 4 is (1 - w) * C + w * lut[C], map 5 (1 - w) * source[C] + w * target[C], in sRGB
    else if (push.color_map == 4 || push.color_map == 5)
    {
        vec4 srgb   = vec4(srgb_encode(color.rgb), 1.0);
        vec4 first  = sample_lut(srgb.rgb, 0);
        vec4 mapped = push.color_map == 4 ? mix(srgb, first, push.lut_weight) : mix(first, sample_lut(srgb.rgb, 1), push.lut_weight);
        color       = vec4(srgb_decode(mapped.rgb), mapped.a);
    }

    // Edges of the imagery, drawn on top of the mapped colors
    float edge = smoothstep(0.02, 0.08, length(vec2(dFdx(luma), dFdy(luma)))) * push.edge_color.a;
    color.rgb  = mix(color.rgb, srgb_decode(push.edge_color.rgb), edge);

    float alpha = push.source_alpha != 0 ? clamp(color.a * push.adjust.x, 0.0, 1.0) : 1.0;
    R_COLOR     = vec4(color.rgb * alpha, alpha);
R_FRAGMENT_MAIN_END
