#include "shader.h"

R_TEXTURE_2D(font, 0)

#define VARYINGS(X)      \
    X(vec2, v_uv,    0)  \
    X(vec4, v_color, 1)

R_VARYINGS(VARYINGS)

R_FRAGMENT_MAIN_BEGIN
    R_COLOR = R_VAR(v_color) * r_sample(font, R_VAR(v_uv));
R_FRAGMENT_MAIN_END
