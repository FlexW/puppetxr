#include "shader.h"
#include "blocks.h"

R_PUSH_BLOCK(COMP_PUSH_FIELDS)

#define VARYINGS(X)    \
    X(vec2, v_uv,  0)  \
    X(vec2, v_ndc, 1)

R_VERTEX_INPUTS(R_NO_FIELDS)
R_VARYINGS(VARYINGS)

R_VERTEX_MAIN_BEGIN
    // Two triangles over the corners 0, 1, 2 and 2, 1, 3
    int  id     = R_VERTEX_ID;
    int  corner = id < 3 ? id : id == 3 ? 2 : id == 4 ? 1 : 3;
    vec2 uv     = vec2(float(corner & 1), float(corner >> 1));
    R_VAR(v_uv)  = push.src_rect.xy + uv * push.src_rect.zw;
    R_VAR(v_ndc) = uv * 2.0 - 1.0;
    if ((push.flags & (COMP_FLAG_CYLINDER | COMP_FLAG_SPHERE | COMP_FLAG_CUBE | COMP_FLAG_LENS_MASK)) != 0u)
    {
        R_POSITION = vec4(R_VAR(v_ndc), 0.0, 1.0);
        R_VERTEX_RETURN;
    }
    // Indexing the push constant array with a dynamic index draws nothing on RADV (Mesa, NAVI22)
    R_POSITION = corner == 0 ? push.geometry[0] : corner == 1 ? push.geometry[1] : corner == 2 ? push.geometry[2] : push.geometry[3];
R_VERTEX_MAIN_END
