// The layer shader of the compositor, shared by composite.frag and composite_depth.frag (which
// defines COMP_DEPTH and writes the layer's depth for XR_FB_composition_layer_depth_test). The
// including file declares the textures: `source`, and with COMP_DEPTH `resolved_depth`.

#define VARYINGS(X)    \
    X(vec2, v_uv,  0)  \
    X(vec2, v_ndc, 1)

R_VARYINGS(VARYINGS)

// Distance along the ray to the far intersection with the surface where |p.xz| (cylinder) or
// |p| (sphere) is the radius: the inside of the surface, the only side that is visible
float far_hit (vec3 origin, vec3 ray)
{
    float a    = dot(ray, ray);
    float b    = dot(origin, ray);
    float c    = dot(origin, origin) - push.radius * push.radius;
    float disc = b * b - a * c;
    if (a <= 0.0 || disc < 0.0)
    {
        return -1.0;
    }
    return (-b + sqrt(disc)) / a;
}

// Texture coordinates of the point a pixel sees on a cylinder or sphere layer; the angle around
// y is 0 at -Z and grows towards +X. Outside [0, 1] the layer is not drawn there. Every ray has a
// view depth of one, so `distance` along it is the view depth of the point, 0 at infinity.
vec2 curved_uv (out float distance)
{
    distance = 0.0;
    vec3 ray    = push.geometry[0].xyz * R_VAR(v_ndc).x + push.geometry[1].xyz * R_VAR(v_ndc).y + push.geometry[2].xyz;
    vec3 origin = vec3(push.geometry[0].w, push.geometry[1].w, push.geometry[2].w);
    vec3 p      = ray;
    bool cylinder = (push.flags & COMP_FLAG_CYLINDER) != 0u;
    if (push.radius > 0.0)
    {
        vec3  mask = cylinder ? vec3(1.0, 0.0, 1.0) : r_splat3(1.0);
        float t    = far_hit(origin * mask, ray * mask);
        if (t <= 0.0)
        {
            return r_splat2(-1.0);
        }
        p        = origin + ray * t;
        distance = t;
    }
    float angle    = atan2(p.x, -p.z);
    float vertical = cylinder ? p.y / length(p.xz) : atan2(p.y, length(p.xz));
    return vec2(angle, vertical) * push.geometry[3].xz + push.geometry[3].yw;
}

// Face and texture coordinates as a samplerCube picks them (Vulkan 16.5.4, cube map face
// selection); faces are +X, -X, +Y, -Y, +Z, -Z
vec3 cube_coordinates ()
{
    vec3  ray = push.geometry[0].xyz * R_VAR(v_ndc).x + push.geometry[1].xyz * R_VAR(v_ndc).y + push.geometry[2].xyz;
    vec3  a   = abs(ray);
    float face;
    vec3  scm;
    if (a.x >= a.y && a.x >= a.z)
    {
        face = ray.x > 0.0 ? 0.0 : 1.0;
        scm  = vec3(ray.x > 0.0 ? -ray.z : ray.z, -ray.y, a.x);
    }
    else if (a.y >= a.z)
    {
        face = ray.y > 0.0 ? 2.0 : 3.0;
        scm  = vec3(ray.x, ray.y > 0.0 ? ray.z : -ray.z, a.y);
    }
    else
    {
        face = ray.z > 0.0 ? 4.0 : 5.0;
        scm  = vec3(ray.z > 0.0 ? ray.x : -ray.x, -ray.y, a.z);
    }
    return vec3(0.5 * (scm.xy / scm.z + 1.0), face);
}

// Spec 10.6.2 composition layer blending; the pipeline blends with ONE, ONE_MINUS_SRC_ALPHA
#ifdef COMP_DEPTH
R_FRAGMENT_COLOR_DEPTH_MAIN_BEGIN
#else
R_FRAGMENT_MAIN_BEGIN
    // The visibility mask's hidden area (XR_KHR_visibility_mask), as the wearer sees it
    if ((push.flags & COMP_FLAG_LENS_MASK) != 0u)
    {
        if (dot(R_VAR(v_ndc), R_VAR(v_ndc)) <= push.radius * push.radius)
        {
            discard;
        }
        R_COLOR = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
#endif
    vec4  color    = r_sample_layer(source, R_VAR(v_uv), float(push.layer));
    float distance = 0.0;
    if ((push.flags & (COMP_FLAG_CYLINDER | COMP_FLAG_SPHERE)) != 0u)
    {
        vec2 st = curved_uv(distance);
        if (any(lessThan(st, r_splat2(0.0))) || any(greaterThan(st, r_splat2(1.0))))
        {
            discard;
        }
        // Explicit level: derivatives are undefined across the discontinuities of the mapping
        color = r_sample_layer_lod(source, push.src_rect.xy + st * push.src_rect.zw, float(push.layer), 0.0);
    }
    if ((push.flags & COMP_FLAG_CUBE) != 0u)
    {
        vec3 coordinates = cube_coordinates();
        color = r_sample_layer_lod(source, coordinates.xy, float(push.layer) + coordinates.z, 0.0);
    }
    // XR_FB_composition_layer_alpha_blend: the pipeline's factors replace the layer blending, so
    // the color is neither premultiplied nor made opaque
    bool custom = (push.flags & COMP_FLAG_CUSTOM_BLEND) != 0u;
    if ((push.flags & COMP_FLAG_SOURCE_ALPHA) == 0u && !custom)
    {
        color.a = 1.0;
    }
    else
    {
        if ((push.flags & COMP_FLAG_INVERTED_ALPHA) != 0u)
        {
            color.a = 1.0 - color.a;
        }
        if ((push.flags & COMP_FLAG_UNPREMULTIPLIED) != 0u && !custom)
        {
            color.rgb *= color.a;
        }
    }
    if ((push.flags & COMP_FLAG_COLOR_TRANSFORM) != 0u && custom)
    {
        color = color * max(push.color_scale, r_splat4(0.0)) + push.color_bias;
    }
    // Spec 12.5: scale and bias act on the unpremultiplied color, which is premultiplied again
    else if ((push.flags & COMP_FLAG_COLOR_TRANSFORM) != 0u)
    {
        color.rgb = color.a > 0.0 ? color.rgb / color.a : r_splat3(0.0);
        color     = color * max(push.color_scale, r_splat4(0.0)) + push.color_bias;
        color.rgb *= color.a;
    }
    R_COLOR = clamp(color, 0.0, 1.0);
    // XR_FB_composition_layer_secure_content: captures get opaque proxy content where the layer is
    if ((push.flags & COMP_FLAG_REPLACE) != 0u)
    {
        R_COLOR = vec4(0.0, 0.0, 0.0, 1.0);
    }
#ifdef COMP_DEPTH
    // XR_FB_composition_layer_depth_test: depth is 1 - COMP_DEPTH_NEAR / view depth (docs/compositor.md)
    if ((push.flags & COMP_FLAG_DEPTH_RESOLVED) != 0u)
    {
        R_DEPTH = r_unpack_unorm24(r_fetch_array(resolved_depth, ivec2(R_FRAG_COORD.xy), push.depth_layer));
    }
    else if ((push.flags & COMP_FLAG_CUBE) != 0u || ((push.flags & (COMP_FLAG_CYLINDER | COMP_FLAG_SPHERE)) != 0u && distance <= 0.0))
    {
        R_DEPTH = 1.0;
    }
    else if ((push.flags & (COMP_FLAG_CYLINDER | COMP_FLAG_SPHERE)) != 0u)
    {
        R_DEPTH = clamp(1.0 - COMP_DEPTH_NEAR / distance, 0.0, 1.0);
    }
    else
    {
        R_DEPTH = R_FRAG_COORD.z;
    }
R_FRAGMENT_COLOR_DEPTH_MAIN_END
#else
R_FRAGMENT_MAIN_END
#endif
