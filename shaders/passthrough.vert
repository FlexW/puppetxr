#include "shader.h"
#include "blocks.h"

R_PUSH_BLOCK(COMP_PASSTHROUGH_PUSH_FIELDS)

#define ATTRIBUTES(X)       \
    X(vec3, a_position, 0)  \
    X(vec2, a_uv,       1)

#define VARYINGS(X)    \
    X(vec2, v_uv, 0)

R_VERTEX_INPUTS(ATTRIBUTES)
R_VARYINGS(VARYINGS)

R_VERTEX_MAIN_BEGIN
    R_VAR(v_uv) = R_IN(a_uv);
    R_POSITION  = mul(push.clip_from_room, vec4(R_IN(a_position), 1.0));
R_VERTEX_MAIN_END
