#include "shader.h"
#include "blocks.h"

R_PUSH_BLOCK(UI_PUSH_FIELDS)

#define ATTRIBUTES(X)       \
    X(vec2, a_position, 0)  \
    X(vec2, a_uv,       1)  \
    X(vec4, a_color,    2)

#define VARYINGS(X)      \
    X(vec2, v_uv,    0)  \
    X(vec4, v_color, 1)

R_VERTEX_INPUTS(ATTRIBUTES)
R_VARYINGS(VARYINGS)

// ImGui colours are sRGB bytes; an sRGB attachment encodes on write, so decode here to avoid encoding twice
vec3 srgb_to_linear (vec3 c)
{
    vec3 low  = c / 12.92;
    vec3 high = pow(max((c + 0.055) / 1.055, r_splat3(0.0)), r_splat3(2.4));
    return mix(low, high, step(r_splat3(0.04045), c));
}

R_VERTEX_MAIN_BEGIN
    vec4 color     = R_IN(a_color);
    R_VAR(v_uv)    = R_IN(a_uv);
    R_VAR(v_color) = push.srgb_target != 0u ? vec4(srgb_to_linear(color.rgb), color.a) : color;
    R_POSITION     = vec4(R_IN(a_position) * push.scale + push.translate, 0.0, 1.0);
R_VERTEX_MAIN_END
