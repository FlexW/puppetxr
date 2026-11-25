#include "shader.h"
#include "blocks.h"

// Scaled image copies for backends without a blit command (D3D11, D3D12)

R_PUSH_BLOCK(BLIT_PUSH_FIELDS)

R_TEXTURE_LAYERED(source, 0)

#define VARYINGS(X) \
    X(vec2, v_uv, 0)

R_VARYINGS(VARYINGS)

R_FRAGMENT_MAIN_BEGIN
    R_COLOR = r_sample_layer_lod(source, R_VAR(v_uv), float(push.layer), 0.0);
R_FRAGMENT_MAIN_END
