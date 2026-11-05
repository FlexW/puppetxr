#ifndef PUPPETXR_BLOCKS_H
#define PUPPETXR_BLOCKS_H

// Push blocks, shared by the shaders and the C++ that fills them.
// Fields are F(type, name) or A(type, name, count). Arrays are of vec4 or mat4, no field crosses
// a 16-byte boundary and blocks are a multiple of 16 bytes, so Vulkan push constants, std140
// uniform blocks and HLSL constant buffers share the C layout (docs/shaders.md).

//
// Compositor (shaders/composite.*)
//

#define COMP_FLAG_SOURCE_ALPHA    1u
#define COMP_FLAG_UNPREMULTIPLIED 2u
#define COMP_FLAG_INVERTED_ALPHA  4u
#define COMP_FLAG_COLOR_TRANSFORM 8u
#define COMP_FLAG_CYLINDER        16u
#define COMP_FLAG_SPHERE          32u
#define COMP_FLAG_CUBE            64u
#define COMP_FLAG_REPLACE         128u /* capture of a XR_COMPOSITION_LAYER_SECURE_CONTENT_REPLACE_LAYER_BIT_FB layer */
#define COMP_FLAG_DEPTH_RESOLVED  256u /* composite_depth.frag: the depth is layer depth_layer of resolved_depth */
#define COMP_FLAG_CUSTOM_BLEND    512u /* XR_FB_composition_layer_alpha_blend: the texture's color and alpha as they are, for the pipeline's factors */
#define COMP_FLAG_LENS_MASK       1024u /* black outside the lens, a circle of `radius` in the eye's NDC; no texture is read */

/* XR_FB_composition_layer_depth_test: the compositor's depth is 1 - COMP_DEPTH_NEAR / view depth,
   an infinite far plane; nearer than COMP_DEPTH_NEAR clamps to 0 */
#define COMP_DEPTH_NEAR 0.01

/* XR_META_passthrough_color_lut: a layer's LUTs in one RGBA8 image, the first in the top half,
   an interpolated map's target in the bottom half. Slice b of a LUT of resolution n is the n x n
   tile (b % (width / n), b / (width / n)), element (r, g) at its column r and row g; 64^3 elements
   fill a half. The push block's lut_resolution is the first LUT's resolution | the second's << 8 */
#define COMP_COLOR_LUT_ATLAS_WIDTH  512
#define COMP_COLOR_LUT_ATLAS_HEIGHT 1024
#define COMP_COLOR_LUT_MAX_RESOLUTION 64

/* geometry: Image: clip-space top-left, top-right, bottom-left, bottom-right (COMP_View::corners);
   Cylinder, Sphere, Cube: COMP_View::rays, then COMP_Layer::uv_map.
   src_rect: u, v, width, height in texture coordinates; a negative height flips.
   composite_resolve.frag takes the app's depth to the compositor's as color_scale.x + color_scale.y * depth */
#define COMP_PUSH_FIELDS(F, A)  \
    A(vec4,  geometry, 4)       \
    F(vec4,  src_rect)          \
    F(vec4,  color_scale)       \
    F(vec4,  color_bias)        \
    F(int,   layer)             \
    F(uint,  flags)             \
    F(float, radius)            \
    F(int,   depth_layer)

/* adjust: texture opacity, brightness, contrast, saturation.
   color_map: 0 none, 1 mono to rgba, 2 mono to mono, 3 brightness contrast saturation */
#define COMP_PASSTHROUGH_PUSH_FIELDS(F, A) \
    F(mat4, clip_from_room)                \
    F(vec4, edge_color)                    \
    F(vec4, adjust)                        \
    F(int,  color_map)                     \
    F(int,  source_alpha)                  \
    F(int,  lut_resolution)                \
    F(float, lut_weight)

#define COMP_DEPTH_PUSH_FIELDS(F, A) \
    F(mat4, clip_from_room)

//
// ImGui (shaders/imgui.*)
//

/* srgb_target: decode the sRGB vertex colours so that an sRGB target does not encode them twice */
#define UI_PUSH_FIELDS(F, A)   \
    F(vec2, scale)             \
    F(vec2, translate)         \
    F(uint, srgb_target)       \
    F(uint, padding0)          \
    F(uint, padding1)          \
    F(uint, padding2)

//
// Blits of backends without a blit command (shaders/blit.*)
//

/* src_rect: u, v, width, height of the source in texture coordinates */
#define BLIT_PUSH_FIELDS(F, A) \
    F(vec4, src_rect)          \
    F(int,  layer)             \
    F(int,  padding0)          \
    F(int,  padding1)          \
    F(int,  padding2)

//
// C++ structs of the blocks
//

#ifdef __cplusplus

#include "platform.h"

typedef f32 R_BlockType_float;
typedef i32 R_BlockType_int;
typedef u32 R_BlockType_uint;
typedef f32 R_BlockType_vec2[2];
typedef f32 R_BlockType_vec4[4];
typedef f32 R_BlockType_mat4[16];

#define R_BLOCK_FIELD(type, name)        R_BlockType_##type name;
#define R_BLOCK_ARRAY(type, name, count) R_BlockType_##type name[count];
#define R_BLOCK(Name, FIELDS)            struct Name { FIELDS(R_BLOCK_FIELD, R_BLOCK_ARRAY) }; static_assert(sizeof(Name) % 16 == 0, #Name " is not a multiple of 16 bytes");

R_BLOCK(COMP_Push,            COMP_PUSH_FIELDS)
R_BLOCK(COMP_PassthroughPush, COMP_PASSTHROUGH_PUSH_FIELDS)
R_BLOCK(COMP_DepthPush,       COMP_DEPTH_PUSH_FIELDS)
R_BLOCK(UI_Push,              UI_PUSH_FIELDS)
R_BLOCK(R_BlitPush,           BLIT_PUSH_FIELDS)

#endif

#endif
