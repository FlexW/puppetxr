#include "shader.h"

// One triangle over the whole viewport

R_VERTEX_INPUTS(R_NO_FIELDS)
R_VARYINGS(R_NO_FIELDS)

R_VERTEX_MAIN_BEGIN
    vec2 corner = vec2(float((R_VERTEX_ID << 1) & 2), float(R_VERTEX_ID & 2));
    R_POSITION  = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
R_VERTEX_MAIN_END
