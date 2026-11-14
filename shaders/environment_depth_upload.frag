#include "shader.h"
#include "blocks.h"

// A traced environment depth image into the depth attachment, texel for texel: layer `layer` of
// `depth_bytes` holds each D16 value's high byte in red and its low byte in green. Fragment and texel
// coordinates count rows in memory order on every backend.

R_PUSH_BLOCK(COMP_DEPTH_UPLOAD_PUSH_FIELDS)
R_TEXTURE_ARRAY(depth_bytes, 0)
R_VARYINGS(R_NO_FIELDS)

R_FRAGMENT_DEPTH_WRITE_MAIN_BEGIN
    uvec2 bytes = uvec2(r_fetch_array(depth_bytes, ivec2(R_FRAG_COORD.xy), push.layer).rg * 255.0 + 0.5);
    // A quarter step above the value: the attachment's conversion gives it whether it rounds or truncates
    R_DEPTH = min((float((bytes.r << 8u) | bytes.g) + 0.25) / 65535.0, 1.0);
R_FRAGMENT_DEPTH_WRITE_MAIN_END
