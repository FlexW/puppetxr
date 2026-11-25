#include "shader.h"
#include "blocks.h"

// One triangle over the whole viewport, the destination rectangle of the blit

R_PUSH_BLOCK(BLIT_PUSH_FIELDS)

R_VERTEX_INPUTS(R_NO_FIELDS)

#define VARYINGS(X) \
    X(vec2, v_uv, 0)

R_VARYINGS(VARYINGS)

R_VERTEX_MAIN_BEGIN
    vec2 corner = vec2(float((R_VERTEX_ID << 1) & 2), float(R_VERTEX_ID & 2));
    R_VAR(v_uv) = push.src_rect.xy + corner * push.src_rect.zw;
    R_POSITION  = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
R_VERTEX_MAIN_END
