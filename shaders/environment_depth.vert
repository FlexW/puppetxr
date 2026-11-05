#include "shader.h"
#include "blocks.h"

R_PUSH_BLOCK(COMP_DEPTH_PUSH_FIELDS)

#define ATTRIBUTES(X)       \
    X(vec3, a_position, 0)

R_VERTEX_INPUTS(ATTRIBUTES)
R_VARYINGS(R_NO_FIELDS)

R_VERTEX_MAIN_BEGIN
    R_POSITION = mul(push.clip_from_room, vec4(R_IN(a_position), 1.0));
R_VERTEX_MAIN_END
