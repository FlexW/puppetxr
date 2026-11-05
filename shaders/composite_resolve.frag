#include "shader.h"
#include "blocks.h"

// XR_FB_composition_layer_depth_test: a projection view's depth in the compositor's convention,
// packed into RGBA8 so that every backend can render it. The app's value is affine in the inverse
// view depth, and so is the compositor's, so the conversion is color_scale.x + color_scale.y * value.

R_PUSH_BLOCK(COMP_PUSH_FIELDS)
R_TEXTURE_LAYERED(source, 0)

#define VARYINGS(X)    \
    X(vec2, v_uv,  0)  \
    X(vec2, v_ndc, 1)

R_VARYINGS(VARYINGS)

R_FRAGMENT_MAIN_BEGIN
    float value = r_sample_layer_lod(source, R_VAR(v_uv), float(push.layer), 0.0).r;
    R_COLOR = r_pack_unorm24(clamp(push.color_scale.x + push.color_scale.y * value, 0.0, 1.0));
R_FRAGMENT_MAIN_END
