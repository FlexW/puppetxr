#include "comp.h"

#include "composite.vert.h"
#include "composite.frag.h"
#include "composite_depth.frag.h"
#include "composite_resolve.frag.h"
#include "passthrough.vert.h"
#include "passthrough.frag.h"
#include "environment_depth.vert.h"
#include "environment_depth.frag.h"
#include "blocks.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x) ASSERT(x)
#define STBIW_MALLOC(size) os_memory_alloc(size)
#define STBIW_REALLOC(pointer, size) os_memory_realloc(pointer, size)
#define STBIW_FREE(pointer) os_memory_release(pointer)
#include <stb_image_write.h>

//
// Types
//

#define COMP_TARGET_FORMAT R_Format::R8G8B8A8_SRGB
#define COMP_DEPTH_FORMAT  R_Format::D32_SFLOAT
#define COMP_RESOLVED_FORMAT R_Format::R8G8B8A8_UNORM // layer depth packed by composite_resolve.frag
#define COMP_COMPARE_OPS     8                        // R_CompareOp values
#define COMP_MAX_PIPELINES   32                       // layer pipelines besides the default one, by COMP_PipelineKey
#define COMP_LUT_SIZE      256
#define COMP_ENV_DEPTH_FORMAT R_Format::D16_UNORM
#define COMP_MAX_DEPTH_IMAGES 4
#define COMP_GEOMETRY_SLOTS   (R_MAX_FRAMES_IN_FLIGHT + 1) // a slot is rewritten only after every frame that read it finished
#define COMP_GEOMETRY_VERTICES 65536 // per frame, over every projected layer
#define COMP_GEOMETRY_INDICES  (3 * 65536)
#define COMP_PROP_ATLAS_WIDTH   512
#define COMP_PROP_ATLAS_HEIGHT  256
#define COMP_PROP_CELL          72  // a version 10 QR code with its quiet zone, 57 + 2 * 4 modules
#define COMP_PROP_CELLS_PER_ROW 7
#define COMP_PROP_QUIET         4   // modules of the quiet zone around a QR code
#define COMP_PROP_SOLID_Y       244 // the row of solid colours, 12 texels high
#define COMP_PROP_PLATE         0.002f // a QR code is printed on a plate this thick

namespace
{

struct COMP_Room
{
    R_BufferHandle vertices;
    R_BufferHandle indices;
    u32            indices_count = {};
    R_ImageHandle  albedo;
};

// Props drawn into passthrough: geometry rebuilt every frame into the slot of that frame, and an
// atlas of QR codes (a cell per prop) above a row of solid colours, uploaded when the codes change
struct COMP_Props
{
    SIM_RenderModel* keyboard = {}; // the template every keyboard copies
    R_BufferHandle   vertices[COMP_GEOMETRY_SLOTS];
    R_BufferHandle   indices[COMP_GEOMETRY_SLOTS];
    u32              max_vertices = {};
    u32              max_indices = {};
    u32              slot = {};
    u32              indices_count = {};
    R_ImageHandle    atlas;
    u64              atlas_hash = {};
    u8*              atlas_pixels = {};
};

// Where one projected geometry landed in this frame's geometry buffers
struct COMP_GeometryDraw
{
    u32 geometry; // index into the layer's geometry
    u32 first_index;
    i32 vertex_offset;
    u32 indices_count;
};

} // namespace

struct COMP_DepthTarget
{
    u32                 width = {};
    u32                 height = {};
    u32                 images_count = {};
    R_ImageHandle       images[COMP_MAX_DEPTH_IMAGES];
    R_FramebufferHandle framebuffers[COMP_MAX_DEPTH_IMAGES][COMP_MAX_VIEWS];
};

struct COMP_CameraTarget
{
    u32                 width = {};
    u32                 height = {};
    R_ImageHandle       image;
    R_ImageHandle       depth;
    R_FramebufferHandle framebuffer;
};

namespace
{

// What a layer's pipeline differs in from the default one
struct COMP_PipelineKey
{
    b8            tested;          // XR_FB_composition_layer_depth_test
    R_CompareOp   depth_compare;
    b8            depth_write;
    b8            blend_custom;    // XR_FB_composition_layer_alpha_blend
    R_BlendFactor blend_factors[4];
};

struct COMP_LayerPipeline
{
    COMP_PipelineKey key = {};
    R_PipelineHandle pipeline;
};

} // namespace

struct COMP_Compositor
{
    R_Device*           device = {};
    u32                 eye_width = {};
    u32                 eye_height = {};
    u32                 width = {};
    u32                 height = {};
    COMP_ViewMode       mode = {};
    R_ImageHandle       target;
    R_ImageHandle       depth;
    R_RenderPassHandle  render_pass;
    R_FramebufferHandle framebuffer;
    R_ImageHandle       capture;             // target without secure layers, for screenshots
    R_ImageHandle       capture_depth;
    R_FramebufferHandle capture_framebuffer;
    u8*                 snapshot = {};       // comp_snapshot's copy of what screenshots read, then of the mirror; made for the target's size
    b8                  capture_valid = {};  // the last frame had secure layers
    b8                  snapshots = {};      // readers answer from the snapshot
    b8                  snapshot_valid = {};
    b8                  snapshot_secure = {}; // the mirror differs from the screenshot copy, second in the buffer
    R_PipelineHandle    pipeline;
    f32                 lens_mask_radius = {};
    R_PipelineHandle    passthrough_pipeline;
    R_RenderPassHandle  depth_pass;     // created with the first depth target
    R_PipelineHandle    depth_pipeline;
    COMP_Room           room;
    R_ImageHandle       luts[COMP_MAX_LAYERS]; // per layer slot, uploaded when the style's map changes
    u64                 lut_hashes[COMP_MAX_LAYERS] = {};
    R_ImageHandle       color_lut_atlases[COMP_MAX_LAYERS] = {}; // XR_META_passthrough_color_lut, made when a slot first uses one
    u64                 color_lut_keys[COMP_MAX_LAYERS] = {};
    u8*                 color_lut_pixels = {};                    // an atlas being filled, made with the first
    R_PipelineHandle    mask_pipeline;          // projected passthrough geometry, depth only
    R_BufferHandle      geometry_vertices[COMP_GEOMETRY_SLOTS]; // made with the first projected layer
    R_BufferHandle      geometry_indices[COMP_GEOMETRY_SLOTS];
    u32                 geometry_slot = {};
    COMP_GeometryDraw   geometry_draws[COMP_MAX_GEOMETRY] = {};
    u32                 layer_first_draw[COMP_MAX_LAYERS] = {};
    u32                 layer_draws[COMP_MAX_LAYERS] = {};
    COMP_Props          props;
    R_RenderPassHandle  resolve_pass;     // XR_FB_composition_layer_depth_test, made with the first depth-tested layer
    R_PipelineHandle    resolve_pipeline;
    R_ImageHandle       resolved;         // projection depth in the compositor's convention, one array layer per layer slot
    u32                 resolved_layers = {};
    R_FramebufferHandle resolved_framebuffers[COMP_MAX_LAYERS];
    COMP_LayerPipeline  layer_pipelines[COMP_MAX_PIPELINES] = {}; // made when first used
    u32                 layer_pipelines_count = {};
};

namespace
{

struct COMP_PngWriter
{
    Array<u8> bytes;
};

} // namespace

//
// Statics
//

static void png_write (void* context, void* data, int size) // NOLINT(misc-const-correctness): stbi_write_func
{
    ((COMP_PngWriter*)context)->bytes.add((const u8*)data, (usize)size);
}

static R_PipelineHandle create_pipeline (const COMP_Compositor* comp)
{
    return r_create_pipeline(comp->device, {
        .vertex              = &composite_vert_shader,
        .fragment            = &composite_frag_shader,
        .render_pass         = comp->render_pass,
        .textures_count      = 1,
        .texture_type        = R_TextureType::Type2DArray,
        .push_constants_size = sizeof(COMP_Push),
        .blend_enable        = true,
        .blend_premultiplied = true,
    });
}

static b8 has_depth_views (const COMP_Layer& layer)
{
    return layer.depth[0].valid || layer.depth[1].valid;
}

// Spec XR_FB_composition_layer_depth_test: a projection view without depth is not tested
static COMP_PipelineKey pipeline_key (const COMP_Compositor* comp, const COMP_Layer& layer, u32 eye)
{
    COMP_PipelineKey key = {};
    key.tested           = layer.depth_test && comp->resolved && (u32)layer.depth_compare < COMP_COMPARE_OPS && (!has_depth_views(layer) || layer.depth[eye].valid);
    key.depth_compare    = key.tested ? layer.depth_compare : R_CompareOp::LessOrEqual;
    key.depth_write      = key.tested && layer.depth_write;
    key.blend_custom     = layer.blend_custom;
    for (u32 i = 0; i < 4 && key.blend_custom; i++)
    {
        key.blend_factors[i] = layer.blend_factors[i];
    }
    return key;
}

static R_PipelineHandle find_pipeline (const COMP_Compositor* comp, const COMP_PipelineKey& key)
{
    if (!key.tested && !key.blend_custom)
    {
        return comp->pipeline;
    }
    for (u32 i = 0; i < comp->layer_pipelines_count; i++)
    {
        if (memory_equal(&comp->layer_pipelines[i].key, &key, sizeof(key)))
        {
            return comp->layer_pipelines[i].pipeline;
        }
    }
    return {};
}

static void ensure_pipeline (COMP_Compositor* comp, const COMP_PipelineKey& key)
{
    if (find_pipeline(comp, key))
    {
        return;
    }
    if (comp->layer_pipelines_count == COMP_MAX_PIPELINES)
    {
        LOG_WARNING("More than %d layer pipelines; such layers are not drawn", COMP_MAX_PIPELINES);
        return;
    }
    const R_PipelineHandle pipeline = r_create_pipeline(comp->device, {
        .vertex              = &composite_vert_shader,
        .fragment            = key.tested ? &composite_depth_frag_shader : &composite_frag_shader,
        .render_pass         = comp->render_pass,
        .textures_count      = key.tested ? 2u : 1u,
        .texture_type        = R_TextureType::Type2DArray,
        .push_constants_size = sizeof(COMP_Push),
        .blend_enable        = true,
        .blend_premultiplied = true,
        .blend_custom        = key.blend_custom,
        .blend_factors       = { key.blend_factors[0], key.blend_factors[1], key.blend_factors[2], key.blend_factors[3] },
        .depth_test          = key.tested,
        .depth_compare       = key.depth_compare,
        .depth_write         = key.depth_write,
    });
    if (pipeline)
    {
        comp->layer_pipelines[comp->layer_pipelines_count++] = { .key = key, .pipeline = pipeline };
    }
}

static void release_resolved (COMP_Compositor* comp)
{
    for (R_FramebufferHandle& framebuffer : comp->resolved_framebuffers)
    {
        r_destroy_framebuffer(comp->device, framebuffer);
        framebuffer = {};
    }
    r_destroy_image(comp->device, comp->resolved);
    comp->resolved        = {};
    comp->resolved_layers = 0;
}

// At least two layers, so that GL makes it an array texture as R_TEXTURE_ARRAY expects
static b8 ensure_resolved (COMP_Compositor* comp, u32 layers)
{
    layers = zm::max(layers, 2u);
    if (comp->resolved && comp->resolved_layers >= layers)
    {
        return true;
    }
    if (!comp->resolve_pass)
    {
        comp->resolve_pass = r_create_render_pass(comp->device, {
            .color_format   = COMP_RESOLVED_FORMAT,
            .load_op        = R_LoadOp::Clear,
            .initial_layout = R_ImageLayout::ShaderReadOnly,
            .final_layout   = R_ImageLayout::ShaderReadOnly,
        });
        comp->resolve_pipeline = r_create_pipeline(comp->device, {
            .vertex              = &composite_vert_shader,
            .fragment            = &composite_resolve_frag_shader,
            .render_pass         = comp->resolve_pass,
            .textures_count      = 1,
            .texture_type        = R_TextureType::Type2DArray,
            .push_constants_size = sizeof(COMP_Push),
        });
    }
    release_resolved(comp);
    comp->resolved = r_create_image(comp->device, {
        .width          = comp->width,
        .height         = comp->height,
        .layers         = layers,
        .format         = COMP_RESOLVED_FORMAT,
        .usage_flags    = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::Sampled,
        .initial_layout = R_ImageLayout::ShaderReadOnly,
    });
    b8 created = comp->resolve_pass && comp->resolve_pipeline && comp->resolved;
    for (u32 i = 0; i < layers && created; i++)
    {
        comp->resolved_framebuffers[i] = r_create_framebuffer(comp->device, { .render_pass = comp->resolve_pass, .image = comp->resolved, .layer = i });
        created                        = (b8)comp->resolved_framebuffers[i];
    }
    if (!created)
    {
        LOG_ERROR("Failed to create the compositor's layer depth");
        release_resolved(comp);
        return false;
    }
    comp->resolved_layers = layers;
    return true;
}

static R_PipelineHandle create_passthrough_pipeline (const COMP_Compositor* comp)
{
    return r_create_pipeline(comp->device, {
        .vertex                  = &passthrough_vert_shader,
        .fragment                = &passthrough_frag_shader,
        .render_pass             = comp->render_pass,
        .textures_count          = 2,
        .push_constants_size     = sizeof(COMP_PassthroughPush),
        .blend_enable            = true,
        .blend_premultiplied     = true,
        .depth_test              = true,
        .depth_write             = true,
        .vertex_stride           = sizeof(W_VisualVertex),
        .vertex_attributes       = { { 0, R_Format::R32G32B32_SFLOAT, 0 }, { 1, R_Format::R32G32_SFLOAT, 12 } },
        .vertex_attributes_count = 2,
    });
}

// Writes depth 1 where the geometry covers and leaves colors as they are
static R_PipelineHandle create_mask_pipeline (const COMP_Compositor* comp)
{
    return r_create_pipeline(comp->device, {
        .vertex                  = &environment_depth_vert_shader,
        .fragment                = &environment_depth_frag_shader,
        .render_pass             = comp->render_pass,
        .push_constants_size     = sizeof(COMP_DepthPush),
        .depth_test              = true,
        .depth_compare           = R_CompareOp::Always,
        .depth_write             = true,
        .no_color_write          = true,
        .vertex_stride           = sizeof(Vec3),
        .vertex_attributes       = { { 0, R_Format::R32G32B32_SFLOAT, 0 } },
        .vertex_attributes_count = 1,
    });
}

// Copies the geometry of every projected layer into this frame's buffers. What does not fit is
// left out with a warning.
static void upload_geometry (COMP_Compositor* comp, const COMP_Layer* layers, u32 layers_count)
{
    u32 draws    = 0;
    u32 vertices = 0;
    u32 indices  = 0;
    b8  needed   = false;
    for (u32 i = 0; i < layers_count; i++)
    {
        needed |= layers[i].projected && layers[i].geometry_count > 0;
    }
    comp->geometry_slot = (comp->geometry_slot + 1) % COMP_GEOMETRY_SLOTS;
    const u32 slot      = comp->geometry_slot;
    if (needed && !comp->geometry_vertices[slot])
    {
        comp->geometry_vertices[slot] = r_create_buffer(comp->device, { .size = COMP_GEOMETRY_VERTICES * sizeof(Vec3), .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
        comp->geometry_indices[slot]  = r_create_buffer(comp->device, { .size = (usize)COMP_GEOMETRY_INDICES * sizeof(u32), .usage_flags = (u32)R_BufferUsageFlags::Index, .host_visible = true });
    }
    const b8 ready  = comp->geometry_vertices[slot] && comp->geometry_indices[slot];
    Vec3*    out_v  = ready ? (Vec3*)r_get_buffer_mapped_data(comp->device, comp->geometry_vertices[slot]) : nullptr;
    u32*     out_i  = ready ? (u32*)r_get_buffer_mapped_data(comp->device, comp->geometry_indices[slot]) : nullptr;
    b8       warned = false;
    for (u32 i = 0; i < layers_count; i++)
    {
        comp->layer_first_draw[i] = draws;
        comp->layer_draws[i]      = 0;
        for (u32 g = 0; ready && layers[i].projected && g < layers[i].geometry_count; g++)
        {
            const COMP_Geometry& geometry = layers[i].geometry[g];
            if (draws == COMP_MAX_GEOMETRY || vertices + geometry.vertices_count > COMP_GEOMETRY_VERTICES || indices + geometry.indices_count > COMP_GEOMETRY_INDICES)
            {
                if (!warned)
                {
                    LOG_WARNING("Projected passthrough geometry exceeds %u meshes, %u vertices or %u indices this frame; the rest is not drawn", COMP_MAX_GEOMETRY, COMP_GEOMETRY_VERTICES, COMP_GEOMETRY_INDICES);
                    warned = true;
                }
                continue;
            }
            memory_copy(out_v + vertices, geometry.positions, geometry.vertices_count * sizeof(Vec3));
            memory_copy(out_i + indices, geometry.indices, geometry.indices_count * sizeof(u32));
            comp->geometry_draws[draws++] = { .geometry = g, .first_index = indices, .vertex_offset = (i32)vertices, .indices_count = geometry.indices_count };
            comp->layer_draws[i]++;
            vertices += geometry.vertices_count;
            indices  += geometry.indices_count;
        }
    }
    if (vertices > 0)
    {
        r_flush_buffer(comp->device, comp->geometry_vertices[slot], 0, vertices * sizeof(Vec3));
        r_flush_buffer(comp->device, comp->geometry_indices[slot], 0, indices * sizeof(u32));
    }
}

// Sampled RGBA8 image filled through a staging buffer, left in ShaderReadOnly
static R_ImageHandle upload_image (R_Device* device, u32 width, u32 height, R_Format format, const u8* rgba, R_ImageHandle reuse)
{
    R_ImageHandle image = reuse;
    if (!r_is_valid(device, image))
    {
        image = r_create_image(device, { .width = width, .height = height, .format = format, .usage_flags = (u32)R_ImageUsageFlags::TransferDst | (u32)R_ImageUsageFlags::Sampled, .initial_layout = R_ImageLayout::TransferDst });
        if (!image)
        {
            return NULL_HANDLE;
        }
    }
    const u64            size    = (u64)width * height * 4;
    const R_BufferHandle staging = r_create_buffer(device, { .size = size, .usage_flags = (u32)R_BufferUsageFlags::TransferSrc, .host_visible = true });
    if (!staging)
    {
        r_destroy_image(device, image);
        return NULL_HANDLE;
    }
    memory_copy(r_get_buffer_mapped_data(device, staging), rgba, size);
    r_flush_buffer(device, staging, 0, size);
    const R_CmdBufferHandle cmd = r_begin_immediate(device);
    if (image == reuse)
    {
        r_cmd_transition_image(device, cmd, image, R_ImageLayout::ShaderReadOnly, R_ImageLayout::TransferDst);
    }
    r_cmd_copy_buffer_to_image(device, cmd, staging, image, 0);
    r_cmd_transition_image(device, cmd, image, R_ImageLayout::TransferDst, R_ImageLayout::ShaderReadOnly);
    r_end_immediate(device, cmd);
    r_destroy_buffer(device, staging);
    return image;
}

static void release_room (COMP_Compositor* comp)
{
    r_wait_idle(comp->device);
    r_destroy_buffer(comp->device, comp->room.vertices);
    r_destroy_buffer(comp->device, comp->room.indices);
    r_destroy_image(comp->device, comp->room.albedo);
    comp->room = {};
}

static u64 lut_hash (const COMP_PassthroughStyle& style)
{
    return hash((const u8*)style.lut, sizeof(style.lut));
}

static b8 uses_color_luts (const COMP_PassthroughStyle& style)
{
    return style.color_map == COMP_ColorMap::Lut || style.color_map == COMP_ColorMap::InterpolatedLut;
}

// Generations change with the data, so they and the resolutions name what an atlas holds
static u64 color_lut_key (const COMP_PassthroughStyle& style)
{
    const u64 parts[4] = { style.color_lut_generations[0], style.color_map == COMP_ColorMap::InterpolatedLut ? style.color_lut_generations[1] : 0, style.color_lut_resolutions[0], style.color_lut_resolutions[1] };
    return hash((const u8*)parts, sizeof(parts));
}

// Lays the style's LUTs out as shaders/blocks.h describes and uploads them into the slot's atlas
static void upload_color_luts (COMP_Compositor* comp, const COMP_PassthroughStyle& style, u32 slot)
{
    const usize size = (usize)COMP_COLOR_LUT_ATLAS_WIDTH * COMP_COLOR_LUT_ATLAS_HEIGHT * 4;
    if (!comp->color_lut_pixels)
    {
        comp->color_lut_pixels = (u8*)os_memory_alloc(size);
        if (!comp->color_lut_pixels)
        {
            return;
        }
    }
    memory_zero(comp->color_lut_pixels, size);
    const u32 count = style.color_map == COMP_ColorMap::InterpolatedLut ? 2u : 1u;
    for (u32 l = 0; l < count; l++)
    {
        const u32 n    = style.color_lut_resolutions[l];
        const u8* rgba = style.color_luts[l];
        if (!rgba || n == 0 || n > COMP_COLOR_LUT_MAX_RESOLUTION)
        {
            continue;
        }
        const u32 tiles = COMP_COLOR_LUT_ATLAS_WIDTH / n;
        for (u32 b = 0; b < n; b++)
        {
            for (u32 g = 0; g < n; g++)
            {
                const usize x = (usize)(b % tiles) * n;
                const usize y = (usize)l * (COMP_COLOR_LUT_ATLAS_HEIGHT / 2) + (usize)(b / tiles) * n + g;
                memory_copy(comp->color_lut_pixels + (y * COMP_COLOR_LUT_ATLAS_WIDTH + x) * 4, rgba + ((usize)b * n + g) * n * 4, (usize)n * 4);
            }
        }
    }
    comp->color_lut_atlases[slot] = upload_image(comp->device, COMP_COLOR_LUT_ATLAS_WIDTH, COMP_COLOR_LUT_ATLAS_HEIGHT, R_Format::R8G8B8A8_UNORM, comp->color_lut_pixels, comp->color_lut_atlases[slot]);
}

static void draw_passthrough (const COMP_Compositor* comp, R_CmdBufferHandle cmd, const COMP_Layer& layer, u32 slot, u32 eye, const R_Rect& area)
{
    R_Device* device = comp->device;
    if ((!comp->room.indices_count && !comp->props.indices_count) || !layer.style || !layer.eye_visible[eye])
    {
        return;
    }
    const COMP_PassthroughStyle& style = *layer.style;

    // A LUT atlas that could not be made leaves the imagery unmapped
    const b8             luts      = uses_color_luts(style) && comp->color_lut_atlases[slot];
    const R_ImageHandle  color_map = luts ? comp->color_lut_atlases[slot] : comp->luts[slot];
    COMP_PassthroughPush push      = {
        .edge_color     = { style.edge_color[0], style.edge_color[1], style.edge_color[2], style.edge_color[3] },
        .adjust         = { style.opacity, style.brightness, style.contrast, style.saturation },
        .color_map      = uses_color_luts(style) && !luts ? (i32)COMP_ColorMap::None : (i32)style.color_map,
        .source_alpha   = layer.source_alpha,
        .lut_resolution = (i32)(style.color_lut_resolutions[0] | style.color_lut_resolutions[1] << 8),
        .lut_weight     = style.color_lut_weight,
    };
    memory_copy(push.clip_from_room, layer.clip_from_room[eye], sizeof(push.clip_from_room));

    // Every passthrough layer sees only the room, never an earlier passthrough layer's depth. A
    // projected layer shows the room only on its geometry: depth 0 blocks it everywhere, the
    // geometry opens it at depth 1 (clip z = w).
    r_cmd_clear_depth(device, cmd, area, layer.projected ? 0.0f : 1.0f);
    if (layer.projected)
    {
        if (comp->layer_draws[slot] == 0)
        {
            return;
        }
        r_cmd_bind_pipeline(device, cmd, comp->mask_pipeline);
        r_cmd_bind_vertex_buffer(device, cmd, comp->geometry_vertices[comp->geometry_slot], 0);
        r_cmd_bind_index_buffer(device, cmd, comp->geometry_indices[comp->geometry_slot], 0, false);
        for (u32 d = 0; d < comp->layer_draws[slot]; d++)
        {
            const COMP_GeometryDraw& draw = comp->geometry_draws[comp->layer_first_draw[slot] + d];
            const f32*               clip = layer.geometry[draw.geometry].clip_from_mesh[eye];
            COMP_DepthPush           mask = {};
            memory_copy(mask.clip_from_room, clip, sizeof(mask.clip_from_room));
            for (u32 c = 0; c < 4; c++)
            {
                mask.clip_from_room[c * 4 + 2] = clip[c * 4 + 3];
            }
            r_cmd_push_constants(device, cmd, comp->mask_pipeline, &mask, sizeof(mask));
            r_cmd_draw_indexed(device, cmd, draw.indices_count, draw.first_index, draw.vertex_offset);
        }
    }

    r_cmd_bind_pipeline(device, cmd, comp->passthrough_pipeline);
    r_cmd_push_constants(device, cmd, comp->passthrough_pipeline, &push, sizeof(push));
    if (comp->room.indices_count)
    {
        const R_ImageHandle textures[2] = { comp->room.albedo, color_map };
        r_cmd_bind_textures(device, cmd, comp->passthrough_pipeline, textures, 2, R_Filter::Linear);
        r_cmd_bind_vertex_buffer(device, cmd, comp->room.vertices, 0);
        r_cmd_bind_index_buffer(device, cmd, comp->room.indices, 0, false);
        r_cmd_draw_indexed(device, cmd, comp->room.indices_count, 0, 0);
    }
    // Props are part of what the cameras see, in depth order with the room; their QR modules stay sharp
    if (comp->props.indices_count)
    {
        const COMP_Props&   props       = comp->props;
        const R_ImageHandle textures[2] = { props.atlas, color_map };
        r_cmd_bind_textures(device, cmd, comp->passthrough_pipeline, textures, 2, R_Filter::Nearest);
        r_cmd_bind_vertex_buffer(device, cmd, props.vertices[props.slot], 0);
        r_cmd_bind_index_buffer(device, cmd, props.indices[props.slot], 0, false);
        r_cmd_draw_indexed(device, cmd, props.indices_count, 0, 0);
    }
}

// Solid colours below the QR cells, sRGB: keyboard body, keys, the back of a QR plate
static const u8 comp_prop_solids[3][4] = { { 40, 40, 46, 255 }, { 190, 190, 190, 255 }, { 120, 120, 120, 255 } };

static Vec2 solid_uv (u32 solid)
{
    return { ((f32)solid * 16.0f + 8.0f) / (f32)COMP_PROP_ATLAS_WIDTH, ((f32)COMP_PROP_SOLID_Y + 6.0f) / (f32)COMP_PROP_ATLAS_HEIGHT };
}

static u64 props_atlas_hash (const SIM_Prop* props, u32 count)
{
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < count; i++)
    {
        h = (h ^ (u64)props[i].kind) * 0x100000001B3ull;
        h = (h ^ hash((const u8*)props[i].text, cstr_length(props[i].text))) * 0x100000001B3ull;
    }
    return h;
}

// QR code i in cell i, one texel per module, white quiet zone; paper white, not pure white
static b8 update_props_atlas (COMP_Compositor* comp, const SIM_Prop* props, u32 count)
{
    COMP_Props& p = comp->props;
    const u64   h = props_atlas_hash(props, count);
    if (p.atlas && h == p.atlas_hash)
    {
        return true;
    }
    const usize size = (usize)COMP_PROP_ATLAS_WIDTH * COMP_PROP_ATLAS_HEIGHT * 4;
    if (!p.atlas_pixels)
    {
        p.atlas_pixels = (u8*)os_memory_alloc(size);
    }
    memory_zero(p.atlas_pixels, size);
    for (u32 s = 0; s < ARRAY_SIZE(comp_prop_solids); s++)
    {
        for (u32 y = COMP_PROP_SOLID_Y; y < COMP_PROP_SOLID_Y + 12; y++)
        {
            for (u32 x = s * 16; x < s * 16 + 16; x++)
            {
                memory_copy(p.atlas_pixels + ((usize)y * COMP_PROP_ATLAS_WIDTH + x) * 4, comp_prop_solids[s], 4);
            }
        }
    }
    static W_QrCode code; // NOLINT(misc-use-anonymous-namespace): too large for the stack, used under the runtime lock only
    for (u32 i = 0; i < count; i++)
    {
        if (props[i].kind != SIM_PropKind::QrCode || !w_qr_encode(props[i].text, code))
        {
            continue;
        }
        const u32 cell_x = (i % COMP_PROP_CELLS_PER_ROW) * COMP_PROP_CELL;
        const u32 cell_y = (i / COMP_PROP_CELLS_PER_ROW) * COMP_PROP_CELL;
        const u32 side   = code.size + 2 * COMP_PROP_QUIET;
        for (u32 y = 0; y < side; y++)
        {
            for (u32 x = 0; x < side; x++)
            {
                const b8 inside = x >= COMP_PROP_QUIET && y >= COMP_PROP_QUIET && x < COMP_PROP_QUIET + code.size && y < COMP_PROP_QUIET + code.size;
                const b8 dark   = inside && code.modules[y - COMP_PROP_QUIET][x - COMP_PROP_QUIET];
                u8*      texel  = p.atlas_pixels + ((usize)(cell_y + y) * COMP_PROP_ATLAS_WIDTH + cell_x + x) * 4;
                const u8 value  = dark ? 10 : 240;
                texel[0]        = value;
                texel[1]        = value;
                texel[2]        = value;
                texel[3]        = 255;
            }
        }
    }
    p.atlas      = upload_image(comp->device, COMP_PROP_ATLAS_WIDTH, COMP_PROP_ATLAS_HEIGHT, R_Format::R8G8B8A8_SRGB, p.atlas_pixels, p.atlas);
    p.atlas_hash = h;
    return p.atlas != NULL_HANDLE;
}

static void put_prop_quad (W_VisualVertex* vertices, u32* indices, u32& inout_vertices, u32& inout_indices, const Pose& room_from_prop, f32 half, f32 z, Vec2 uv_low, Vec2 uv_high, b8 front)
{
    const u32 base = inout_vertices;
    for (u32 c = 0; c < 4; c++)
    {
        const f32 x = c == 1 || c == 2 ? half : -half;
        const f32 y = c >= 2 ? half : -half;
        // Texture rows run top down, prop y up
        vertices[inout_vertices++] = { zm::transform_point(room_from_prop, { x, y, z }), { x > 0.0f ? uv_high.x : uv_low.x, y > 0.0f ? uv_low.y : uv_high.y } };
    }
    const u32 order[2][6] = { { 0, 1, 2, 0, 2, 3 }, { 0, 2, 1, 0, 3, 2 } };
    for (const u32 corner : order[front ? 0 : 1])
    {
        indices[inout_indices++] = base + corner;
    }
}

// XR_FB_composition_layer_secure_content: screenshots and captures read a second target, made
// the first time a frame has a secure layer
static b8 ensure_capture (COMP_Compositor* comp)
{
    if (comp->capture_framebuffer)
    {
        return true;
    }
    comp->capture = r_create_image(comp->device, {
        .width          = comp->width,
        .height         = comp->height,
        .format         = COMP_TARGET_FORMAT,
        .usage_flags    = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::TransferSrc,
        .initial_layout = R_ImageLayout::ColorAttachment,
    });
    comp->capture_depth = r_create_image(comp->device, {
        .width          = comp->width,
        .height         = comp->height,
        .format         = COMP_DEPTH_FORMAT,
        .usage_flags    = (u32)R_ImageUsageFlags::DepthStencilAttachment,
        .initial_layout = R_ImageLayout::DepthStencilAttachment,
    });
    if (comp->capture && comp->capture_depth)
    {
        comp->capture_framebuffer = r_create_framebuffer(comp->device, { .render_pass = comp->render_pass, .image = comp->capture, .layer = 0, .depth_image = comp->capture_depth, .depth_layer = 0 });
    }
    if (!comp->capture_framebuffer)
    {
        LOG_ERROR("Failed to create the capture target");
    }
    return (b8)comp->capture_framebuffer;
}

static void release_capture (COMP_Compositor* comp)
{
    r_destroy_framebuffer(comp->device, comp->capture_framebuffer);
    r_destroy_image(comp->device, comp->capture);
    r_destroy_image(comp->device, comp->capture_depth);
    comp->capture_framebuffer = {};
    comp->capture             = {};
    comp->capture_depth       = {};
    comp->capture_valid       = false;
}

static b8 create_target (COMP_Compositor* comp, COMP_ViewMode mode)
{
    r_wait_idle(comp->device);
    release_capture(comp);
    release_resolved(comp);
    r_destroy_framebuffer(comp->device, comp->framebuffer);
    r_destroy_image(comp->device, comp->target);
    r_destroy_image(comp->device, comp->depth);

    os_memory_release(comp->snapshot);
    comp->snapshot       = nullptr;
    comp->snapshot_valid = false;
    comp->mode   = mode;
    comp->width  = mode == COMP_ViewMode::Both ? comp->eye_width * 2 : comp->eye_width;
    comp->height = comp->eye_height;
    comp->target = r_create_image(comp->device, {
        .width          = comp->width,
        .height         = comp->height,
        .format         = COMP_TARGET_FORMAT,
        .usage_flags    = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::TransferSrc | (u32)R_ImageUsageFlags::Sampled,
        .initial_layout = R_ImageLayout::ColorAttachment,
    });
    comp->depth = r_create_image(comp->device, {
        .width          = comp->width,
        .height         = comp->height,
        .format         = COMP_DEPTH_FORMAT,
        .usage_flags    = (u32)R_ImageUsageFlags::DepthStencilAttachment,
        .initial_layout = R_ImageLayout::DepthStencilAttachment,
    });
    comp->framebuffer = r_create_framebuffer(comp->device, { .render_pass = comp->render_pass, .image = comp->target, .layer = 0, .depth_image = comp->depth, .depth_layer = 0 });
    return comp->target && comp->depth && comp->framebuffer;
}

static void source_rect (const R_Rect& rect, u32 image_width, u32 image_height, b8 flip_y, f32* out_rect)
{
    out_rect[0] = (f32)rect.x / (f32)image_width;
    out_rect[1] = (f32)rect.y / (f32)image_height;
    out_rect[2] = (f32)rect.width / (f32)image_width;
    out_rect[3] = (f32)rect.height / (f32)image_height;
    if (flip_y)
    {
        out_rect[1] += out_rect[3];
        out_rect[3]  = -out_rect[3];
    }
}

// XR_FB_composition_layer_depth_test: a projection layer's depth, where its views land in the
// target, into array layer `slot` of `resolved`; far wherever it has none
static void resolve_depth (const COMP_Compositor* comp, R_CmdBufferHandle cmd, const COMP_Layer& layer, u32 slot, COMP_ViewMode mode)
{
    R_Device* device = comp->device;
    r_cmd_begin_render_pass(device, cmd, comp->resolve_pass, comp->resolved_framebuffers[slot], { 1.0f, 1.0f, 1.0f, 1.0f });
    r_cmd_bind_pipeline(device, cmd, comp->resolve_pipeline);
    const u32 first_eye = mode == COMP_ViewMode::Right ? 1 : 0;
    const u32 eyes      = mode == COMP_ViewMode::Both ? COMP_MAX_VIEWS : 1;
    for (u32 eye_slot = 0; eye_slot < eyes; eye_slot++)
    {
        const u32             eye   = first_eye + eye_slot;
        const COMP_DepthView& depth = layer.depth[eye];
        u32                   image_width  = 0;
        u32                   image_height = 0;
        u32                   image_layers = 0;
        r_get_image_size(device, depth.image, image_width, image_height, image_layers);
        if (!depth.valid || !layer.views[eye].visible || image_width == 0 || depth.array_layer >= image_layers)
        {
            continue;
        }
        const R_Rect area = { (i32)(eye_slot * comp->eye_width), 0, comp->eye_width, comp->eye_height };
        r_cmd_set_viewport(device, cmd, { (f32)area.x, 0.0f, (f32)area.width, (f32)area.height });
        r_cmd_set_scissor(device, cmd, area);
        COMP_Push push = { .layer = (i32)depth.array_layer };
        memory_copy(push.geometry, layer.views[eye].corners, sizeof(push.geometry));
        source_rect(depth.rect, image_width, image_height, layer.flip_y, push.src_rect);
        push.color_scale[0] = depth.map[0];
        push.color_scale[1] = depth.map[1];
        r_cmd_bind_texture(device, cmd, comp->resolve_pipeline, depth.image, R_Filter::Nearest);
        r_cmd_push_constants(device, cmd, comp->resolve_pipeline, &push, sizeof(push));
        r_cmd_draw(device, cmd, 6, 0);
    }
    r_cmd_end_render_pass(device, cmd);
}

// One pass over the layers into a target; the capture pass leaves out or blanks secure layers.
// depth_slots: the resolved layer of each projection layer with depth, COMP_MAX_LAYERS for none.
// alpha: the target's initial alpha, 0 for XR_FB_composition_layer_alpha_blend
static void draw_layers (const COMP_Compositor* comp, R_CmdBufferHandle cmd, const COMP_Layer* layers, u32 layers_count, COMP_ViewMode mode, R_FramebufferHandle framebuffer, b8 capture, const u32* depth_slots, f32 alpha)
{
    R_Device* device = comp->device;
    r_cmd_begin_render_pass(device, cmd, comp->render_pass, framebuffer, { 0.0f, 0.0f, 0.0f, alpha });

    const u32 first_eye = mode == COMP_ViewMode::Right ? 1 : 0;
    const u32 eyes      = mode == COMP_ViewMode::Both ? COMP_MAX_VIEWS : 1;
    for (u32 slot = 0; slot < eyes; slot++)
    {
        const u32    eye  = first_eye + slot;
        const R_Rect area = { (i32)(slot * comp->eye_width), 0, comp->eye_width, comp->eye_height };
        r_cmd_set_viewport(device, cmd, { (f32)area.x, 0.0f, (f32)area.width, (f32)area.height });
        r_cmd_set_scissor(device, cmd, area);

        for (u32 i = 0; i < layers_count; i++)
        {
            const COMP_Layer& layer = layers[i];
            if (capture && layer.secure == COMP_Secure::Exclude)
            {
                continue;
            }
            if (layer.type == COMP_LayerType::Passthrough)
            {
                // Its room depth is not composition depth: later depth tests start from far
                draw_passthrough(comp, cmd, layer, i, eye, area);
                r_cmd_clear_depth(device, cmd, area, 1.0f);
                continue;
            }
            const COMP_View& view = layer.views[eye];
            if (!view.visible)
            {
                continue;
            }
            u32 image_width = 0;
            u32 image_height = 0;
            u32 image_layers = 0;
            r_get_image_size(device, view.image, image_width, image_height, image_layers);
            if (image_width == 0 || view.array_layer >= image_layers)
            {
                continue;
            }

            COMP_Push push = {};
            u32       shape = 0;
            if (layer.type == COMP_LayerType::Image)
            {
                memory_copy(push.geometry, view.corners, sizeof(push.geometry));
            }
            else
            {
                memory_copy(push.geometry, view.rays, sizeof(view.rays));
                memory_copy(push.geometry[3], layer.uv_map, sizeof(layer.uv_map));
                push.radius = layer.radius;
                shape       = layer.type == COMP_LayerType::Cylinder ? COMP_FLAG_CYLINDER : layer.type == COMP_LayerType::Sphere ? COMP_FLAG_SPHERE : COMP_FLAG_CUBE;
            }
            source_rect(view.rect, image_width, image_height, layer.flip_y, push.src_rect);
            push.layer = (i32)view.array_layer;
            push.flags = (layer.source_alpha ? COMP_FLAG_SOURCE_ALPHA : 0u) | (layer.unpremultiplied ? COMP_FLAG_UNPREMULTIPLIED : 0u) | (layer.inverted_alpha ? COMP_FLAG_INVERTED_ALPHA : 0u) | (layer.color_transform ? COMP_FLAG_COLOR_TRANSFORM : 0u) | (layer.blend_custom ? COMP_FLAG_CUSTOM_BLEND : 0u) | shape;
            push.flags |= capture && layer.secure == COMP_Secure::Replace ? COMP_FLAG_REPLACE : 0u;
            memory_copy(push.color_scale, layer.color_scale, sizeof(push.color_scale));
            memory_copy(push.color_bias, layer.color_bias, sizeof(push.color_bias));

            const R_Filter         filter   = view.nearest ? R_Filter::Nearest : R_Filter::Linear;
            const COMP_PipelineKey key      = pipeline_key(comp, layer, eye);
            const R_PipelineHandle pipeline = find_pipeline(comp, key);
            if (!pipeline)
            {
                continue;
            }
            r_cmd_bind_pipeline(device, cmd, pipeline);
            if (key.tested)
            {
                const R_ImageHandle textures[2] = { view.image, comp->resolved };
                const b8            resolved    = layer.depth[eye].valid && depth_slots[i] < comp->resolved_layers;
                push.flags                     |= resolved ? COMP_FLAG_DEPTH_RESOLVED : 0u;
                push.depth_layer                = resolved ? (i32)depth_slots[i] : 0;
                r_cmd_bind_textures(device, cmd, pipeline, textures, 2, filter);
            }
            else
            {
                r_cmd_bind_texture(device, cmd, pipeline, view.image, filter);
            }
            r_cmd_push_constants(device, cmd, pipeline, &push, sizeof(push));
            r_cmd_draw(device, cmd, 6, 0);
        }

        // The shader reads no texture for the mask, but every pipeline binding needs one
        if (comp->lens_mask_radius > 0.0f)
        {
            const COMP_Push mask = { .flags = COMP_FLAG_LENS_MASK, .radius = comp->lens_mask_radius };
            r_cmd_bind_pipeline(device, cmd, comp->pipeline);
            r_cmd_bind_texture(device, cmd, comp->pipeline, comp->luts[0], R_Filter::Nearest);
            r_cmd_push_constants(device, cmd, comp->pipeline, &mask, sizeof(mask));
            r_cmd_draw(device, cmd, 6, 0);
        }
    }

    r_cmd_end_render_pass(device, cmd);
}

//
// Public API
//

COMP_Compositor* comp_create (const COMP_CreateInfo& info)
{
    if (!info.device || info.eye_width == 0 || info.eye_height == 0)
    {
        LOG_ERROR("Compositor needs a device and a non-empty eye resolution");
        return nullptr;
    }

    COMP_Compositor* comp = (COMP_Compositor*)os_memory_alloc(sizeof(COMP_Compositor));
    comp->device          = info.device;
    comp->eye_width       = info.eye_width;
    comp->eye_height      = info.eye_height;
    comp_set_lens_mask(comp, info.lens_mask_radius);

    b8 success = false;
    defer {
        if (!success)
        {
            comp_destroy(comp);
        }
    };

    comp->render_pass = r_create_render_pass(comp->device, {
        .color_format         = COMP_TARGET_FORMAT,
        .load_op              = R_LoadOp::Clear,
        .initial_layout       = R_ImageLayout::ColorAttachment,
        .final_layout         = R_ImageLayout::ColorAttachment,
        .depth_format         = COMP_DEPTH_FORMAT,
        .depth_load_op        = R_LoadOp::Clear,
        .depth_initial_layout = R_ImageLayout::DepthStencilAttachment,
        .depth_final_layout   = R_ImageLayout::DepthStencilAttachment,
    });
    comp->pipeline             = create_pipeline(comp);
    comp->passthrough_pipeline = create_passthrough_pipeline(comp);
    comp->mask_pipeline        = create_mask_pipeline(comp);

    u8 neutral[COMP_LUT_SIZE * 4] = {};
    for (u32 i = 0; i < COMP_MAX_LAYERS; i++)
    {
        comp->luts[i] = upload_image(comp->device, COMP_LUT_SIZE, 1, R_Format::R8G8B8A8_UNORM, neutral, NULL_HANDLE);
        if (!comp->luts[i])
        {
            LOG_ERROR("Failed to create compositor color maps");
            return nullptr;
        }
    }

    if (!comp->render_pass || !comp->pipeline || !comp->passthrough_pipeline || !comp->mask_pipeline || !create_target(comp, COMP_ViewMode::Left))
    {
        LOG_ERROR("Failed to create compositor resources");
        return nullptr;
    }

    success = true;
    return comp;
}

void comp_set_lens_mask (COMP_Compositor* comp, f32 radius)
{
    ASSERT(comp);
    // A circle reaching past the corners hides nothing
    comp->lens_mask_radius = radius < 1.41421356f ? zm::max(radius, 0.0f) : 0.0f;
}

void comp_destroy (COMP_Compositor* comp)
{
    if (!comp)
    {
        return;
    }
    release_room(comp);
    r_destroy_pipeline(comp->device, comp->depth_pipeline);
    r_destroy_render_pass(comp->device, comp->depth_pass);
    for (const R_ImageHandle lut : comp->luts)
    {
        r_destroy_image(comp->device, lut);
    }
    for (const R_ImageHandle atlas : comp->color_lut_atlases)
    {
        r_destroy_image(comp->device, atlas);
    }
    os_memory_release(comp->color_lut_pixels);
    for (u32 i = 0; i < COMP_GEOMETRY_SLOTS; i++)
    {
        r_destroy_buffer(comp->device, comp->geometry_vertices[i]);
        r_destroy_buffer(comp->device, comp->geometry_indices[i]);
        r_destroy_buffer(comp->device, comp->props.vertices[i]);
        r_destroy_buffer(comp->device, comp->props.indices[i]);
    }
    r_destroy_image(comp->device, comp->props.atlas);
    os_memory_release(comp->props.keyboard);
    os_memory_release(comp->props.atlas_pixels);
    os_memory_release(comp->snapshot);
    r_destroy_pipeline(comp->device, comp->mask_pipeline);
    r_destroy_pipeline(comp->device, comp->passthrough_pipeline);
    r_destroy_pipeline(comp->device, comp->pipeline);
    for (u32 i = 0; i < comp->layer_pipelines_count; i++)
    {
        r_destroy_pipeline(comp->device, comp->layer_pipelines[i].pipeline);
    }
    release_resolved(comp);
    r_destroy_pipeline(comp->device, comp->resolve_pipeline);
    r_destroy_render_pass(comp->device, comp->resolve_pass);
    release_capture(comp);
    r_destroy_framebuffer(comp->device, comp->framebuffer);
    r_destroy_render_pass(comp->device, comp->render_pass);
    r_destroy_image(comp->device, comp->depth);
    r_destroy_image(comp->device, comp->target);
    os_memory_release(comp);
}

b8 comp_set_room (COMP_Compositor* comp, const W_World* world)
{
    ASSERT(comp);
    release_room(comp);
    if (!world)
    {
        return true;
    }

    const W_VisualVertex* vertices       = nullptr;
    const u32*            indices        = nullptr;
    u32                   vertices_count = 0;
    u32                   indices_count  = 0;
    w_visual_mesh(world, vertices, vertices_count, indices, indices_count);
    if (vertices_count == 0 || indices_count == 0)
    {
        return true;
    }

    R_Device* device    = comp->device;
    comp->room.vertices = r_create_buffer(device, { .size = (u64)vertices_count * sizeof(W_VisualVertex), .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
    comp->room.indices  = r_create_buffer(device, { .size = (u64)indices_count * sizeof(u32), .usage_flags = (u32)R_BufferUsageFlags::Index, .host_visible = true });

    // An untextured room shows mid gray
    const u8  gray[4] = { 128, 128, 128, 255 };
    const u8* rgba    = nullptr;
    u32       width   = 0;
    u32       height  = 0;
    if (!w_albedo(world, rgba, width, height))
    {
        rgba   = gray;
        width  = 1;
        height = 1;
    }
    comp->room.albedo = upload_image(device, width, height, R_Format::R8G8B8A8_SRGB, rgba, NULL_HANDLE);

    if (!comp->room.vertices || !comp->room.indices || !comp->room.albedo)
    {
        LOG_ERROR("Failed to upload the room %s for passthrough", w_name(world));
        release_room(comp);
        return false;
    }
    memory_copy(r_get_buffer_mapped_data(device, comp->room.vertices), vertices, (usize)vertices_count * sizeof(W_VisualVertex));
    memory_copy(r_get_buffer_mapped_data(device, comp->room.indices), indices, (usize)indices_count * sizeof(u32));
    r_flush_buffer(device, comp->room.vertices, 0, (u64)vertices_count * sizeof(W_VisualVertex));
    r_flush_buffer(device, comp->room.indices, 0, (u64)indices_count * sizeof(u32));
    comp->room.indices_count = indices_count;
    return true;
}

void comp_set_props (COMP_Compositor* comp, const SIM_Prop* props, u32 count, i64 time, const Pose& room_from_stage)
{
    ASSERT(comp);
    COMP_Props& p    = comp->props;
    p.indices_count = 0;
    if (count == 0)
    {
        return;
    }
    R_Device* device = comp->device;
    if (!p.keyboard)
    {
        p.keyboard = (SIM_RenderModel*)os_memory_alloc(sizeof(SIM_RenderModel));
        sim_keyboard_model(*p.keyboard);
        p.max_vertices = SIM_MAX_PROPS * zm::max(p.keyboard->vertices_count, 8u);
        p.max_indices  = SIM_MAX_PROPS * zm::max(p.keyboard->indices_count, 12u);
    }
    p.slot = (p.slot + 1) % COMP_GEOMETRY_SLOTS;
    if (!p.vertices[p.slot])
    {
        p.vertices[p.slot] = r_create_buffer(device, { .size = (u64)p.max_vertices * sizeof(W_VisualVertex), .usage_flags = (u32)R_BufferUsageFlags::Vertex, .host_visible = true });
        p.indices[p.slot]  = r_create_buffer(device, { .size = (u64)p.max_indices * sizeof(u32), .usage_flags = (u32)R_BufferUsageFlags::Index, .host_visible = true });
    }
    if (!p.vertices[p.slot] || !p.indices[p.slot] || !update_props_atlas(comp, props, count))
    {
        return;
    }
    W_VisualVertex* vertices        = (W_VisualVertex*)r_get_buffer_mapped_data(device, p.vertices[p.slot]);
    u32*            indices         = (u32*)r_get_buffer_mapped_data(device, p.indices[p.slot]);
    u32             vertices_count  = 0;
    u32             indices_count   = 0;
    for (u32 i = 0; i < count && i < SIM_MAX_PROPS; i++)
    {
        const SIM_Prop& prop           = props[i];
        const Pose      room_from_prop = zm::pose_mul(room_from_stage, sim_prop_pose(prop, time));
        if (prop.kind == SIM_PropKind::Keyboard)
        {
            const SIM_RenderModel& model = *p.keyboard;
            const u32              base  = vertices_count;
            for (u32 part = 0; part < model.parts_count; part++)
            {
                const SIM_ModelPart& mp = model.parts[part];
                const Vec2           uv = solid_uv(part == 0 ? 0 : 1);
                for (u32 v = mp.vertices_first; v < mp.vertices_first + mp.vertices_count; v++)
                {
                    vertices[base + v] = { zm::transform_point(room_from_prop, model.positions[v]), uv };
                }
            }
            for (u32 k = 0; k < model.indices_count; k++)
            {
                indices[indices_count++] = base + model.indices[k];
            }
            vertices_count += model.vertices_count;
            continue;
        }
        // The code's modules span `size`; the quiet zone adds four modules each side
        const u32 modules = w_qr_size(prop.text);
        if (modules == 0)
        {
            continue;
        }
        const u32  side   = modules + 2 * COMP_PROP_QUIET;
        const f32  half   = 0.5f * prop.size * (f32)side / (f32)modules;
        const u32  row    = i / COMP_PROP_CELLS_PER_ROW;
        const f32  cell_x = (f32)((i % COMP_PROP_CELLS_PER_ROW) * COMP_PROP_CELL);
        const f32  cell_y = (f32)(row * COMP_PROP_CELL);
        const Vec2 low    = { cell_x / (f32)COMP_PROP_ATLAS_WIDTH, cell_y / (f32)COMP_PROP_ATLAS_HEIGHT };
        const Vec2 high   = { (cell_x + (f32)side) / (f32)COMP_PROP_ATLAS_WIDTH, (cell_y + (f32)side) / (f32)COMP_PROP_ATLAS_HEIGHT };
        put_prop_quad(vertices, indices, vertices_count, indices_count, room_from_prop, half, 0.0f, low, high, true);
        put_prop_quad(vertices, indices, vertices_count, indices_count, room_from_prop, half, -COMP_PROP_PLATE, solid_uv(2), solid_uv(2), false);
    }
    r_flush_buffer(device, p.vertices[p.slot], 0, (u64)vertices_count * sizeof(W_VisualVertex));
    r_flush_buffer(device, p.indices[p.slot], 0, (u64)indices_count * sizeof(u32));
    p.indices_count = indices_count;
}

void comp_compose (COMP_Compositor* comp, const COMP_Layer* layers, u32 layers_count, COMP_ViewMode mode)
{
    ASSERT(comp);
    ASSERT(layers_count == 0 || layers);
    layers_count = zm::min(layers_count, (u32)COMP_MAX_LAYERS);

    if (mode != comp->mode && !create_target(comp, mode))
    {
        LOG_ERROR("Failed to resize the compositor target");
        return;
    }

    for (u32 i = 0; i < layers_count; i++)
    {
        const COMP_Layer& layer = layers[i];
        if (layer.type != COMP_LayerType::Passthrough || !layer.style)
        {
            continue;
        }
        const u64 h = lut_hash(*layer.style);
        if (h != comp->lut_hashes[i])
        {
            upload_image(comp->device, COMP_LUT_SIZE, 1, R_Format::R8G8B8A8_UNORM, &layer.style->lut[0][0], comp->luts[i]);
            comp->lut_hashes[i] = h;
        }
        const u64 key = uses_color_luts(*layer.style) ? color_lut_key(*layer.style) : 0;
        if (key && (key != comp->color_lut_keys[i] || !comp->color_lut_atlases[i]))
        {
            upload_color_luts(comp, *layer.style, i);
            comp->color_lut_keys[i] = comp->color_lut_atlases[i] ? key : 0;
        }
    }

    // Views often share one array image (both eyes in one swapchain); transition each image once
    R_ImageHandle sources[COMP_MAX_LAYERS * COMP_MAX_VIEWS];
    u32           sources_count = 0;
    for (u32 i = 0; i < layers_count; i++)
    {
        for (u32 v = 0; v < COMP_MAX_VIEWS; v++)
        {
            const R_ImageHandle image = layers[i].views[v].image;
            b8                  seen  = layers[i].type == COMP_LayerType::Passthrough || !layers[i].views[v].visible;
            for (u32 k = 0; k < sources_count; k++)
            {
                seen |= sources[k] == image;
            }
            if (!seen)
            {
                sources[sources_count++] = image;
            }
        }
    }

    // XR_FB_composition_layer_depth_test: projection layers with depth get a resolved layer each
    u32 depth_slots[COMP_MAX_LAYERS];
    u32 depth_layers_count = 0;
    b8  depth_tested       = false;
    for (u32 i = 0; i < layers_count; i++)
    {
        const COMP_Layer& layer = layers[i];
        depth_slots[i]          = COMP_MAX_LAYERS;
        if (!layer.depth_test || layer.type == COMP_LayerType::Passthrough || (u32)layer.depth_compare >= COMP_COMPARE_OPS)
        {
            continue;
        }
        depth_tested = true;
        if (has_depth_views(layer))
        {
            depth_slots[i] = depth_layers_count++;
        }
    }
    if (depth_tested && !ensure_resolved(comp, depth_layers_count))
    {
        depth_layers_count = 0;
    }
    // XR_FB_composition_layer_alpha_blend: the target's alpha starts at 0 in frames that use it
    b8 custom_blend = false;
    for (u32 i = 0; i < layers_count; i++)
    {
        custom_blend |= layers[i].blend_custom && layers[i].type != COMP_LayerType::Passthrough;
        for (u32 eye = 0; eye < COMP_MAX_VIEWS && layers[i].type != COMP_LayerType::Passthrough; eye++)
        {
            ensure_pipeline(comp, pipeline_key(comp, layers[i], eye));
        }
    }
    R_ImageHandle depth_sources[COMP_MAX_LAYERS * COMP_MAX_VIEWS];
    u32           depth_sources_count = 0;
    for (u32 i = 0; i < layers_count && depth_layers_count > 0; i++)
    {
        for (u32 v = 0; v < COMP_MAX_VIEWS && depth_slots[i] < COMP_MAX_LAYERS; v++)
        {
            const R_ImageHandle image = layers[i].depth[v].image;
            b8                  seen  = !layers[i].depth[v].valid;
            for (u32 k = 0; k < depth_sources_count; k++)
            {
                seen |= depth_sources[k] == image;
            }
            if (!seen)
            {
                depth_sources[depth_sources_count++] = image;
            }
        }
    }

    // Created before recording, so its initial layout is set at the start of this frame
    b8 secure = false;
    for (u32 i = 0; i < layers_count; i++)
    {
        secure |= layers[i].secure != COMP_Secure::None;
    }
    comp->capture_valid = secure && ensure_capture(comp);

    R_Device*               device = comp->device;
    const R_CmdBufferHandle cmd    = r_begin_frame(device);
    upload_geometry(comp, layers, layers_count);

    for (u32 i = 0; i < sources_count; i++)
    {
        r_cmd_transition_image(device, cmd, sources[i], R_ImageLayout::ColorAttachment, R_ImageLayout::ShaderReadOnly);
    }
    for (u32 i = 0; i < depth_sources_count; i++)
    {
        r_cmd_transition_image(device, cmd, depth_sources[i], R_ImageLayout::DepthStencilAttachment, R_ImageLayout::ShaderReadOnly);
    }
    for (u32 i = 0; i < layers_count && depth_layers_count > 0; i++)
    {
        if (depth_slots[i] < COMP_MAX_LAYERS)
        {
            resolve_depth(comp, cmd, layers[i], depth_slots[i], mode);
        }
    }

    const f32 alpha = custom_blend ? 0.0f : 1.0f;
    draw_layers(comp, cmd, layers, layers_count, mode, comp->framebuffer, false, depth_slots, alpha);
    if (comp->capture_valid)
    {
        draw_layers(comp, cmd, layers, layers_count, mode, comp->capture_framebuffer, true, depth_slots, alpha);
    }

    for (u32 i = 0; i < sources_count; i++)
    {
        r_cmd_transition_image(device, cmd, sources[i], R_ImageLayout::ShaderReadOnly, R_ImageLayout::ColorAttachment);
    }
    for (u32 i = 0; i < depth_sources_count; i++)
    {
        r_cmd_transition_image(device, cmd, depth_sources[i], R_ImageLayout::ShaderReadOnly, R_ImageLayout::DepthStencilAttachment);
    }

    r_end_frame(device, cmd);
}

R_ImageHandle comp_target (const COMP_Compositor* comp)
{
    ASSERT(comp);
    return comp->target;
}

void comp_target_size (const COMP_Compositor* comp, u32& out_width, u32& out_height)
{
    ASSERT(comp);
    out_width  = comp->width;
    out_height = comp->height;
}

// The display shows no alpha, so neither do screenshots; XR_FB_composition_layer_alpha_blend
// leaves the target's alpha below 1
static b8 read_opaque (const COMP_Compositor* comp, R_ImageHandle image, u8* out_rgba, usize out_size)
{
    r_wait_idle(comp->device);
    if (!r_read_image(comp->device, image, 0, R_ImageLayout::ColorAttachment, out_rgba, out_size))
    {
        return false;
    }
    for (usize i = 3; i < out_size; i += 4)
    {
        out_rgba[i] = 255;
    }
    return true;
}

// While snapshots are on the GPU is never read here: the caller may not own the app's queue
static b8 read_snapshot (const COMP_Compositor* comp, b8 mirror, u8* out_rgba, usize out_size)
{
    const usize size = (usize)comp->width * comp->height * 4;
    if (!comp->snapshot_valid || out_size != size)
    {
        return false;
    }
    memory_copy(out_rgba, comp->snapshot + (mirror && comp->snapshot_secure ? size : 0), size);
    return true;
}

b8 comp_read_target (const COMP_Compositor* comp, u8* out_rgba, usize out_size)
{
    ASSERT(comp);
    if (comp->snapshots)
    {
        return read_snapshot(comp, false, out_rgba, out_size);
    }
    return read_opaque(comp, comp->capture_valid ? comp->capture : comp->target, out_rgba, out_size);
}

b8 comp_read_mirror (const COMP_Compositor* comp, u8* out_rgba, usize out_size)
{
    ASSERT(comp);
    if (comp->snapshots)
    {
        return read_snapshot(comp, true, out_rgba, out_size);
    }
    return read_opaque(comp, comp->target, out_rgba, out_size);
}

void comp_set_snapshots (COMP_Compositor* comp, b8 on)
{
    ASSERT(comp);
    comp->snapshots      = on;
    comp->snapshot_valid = false;
}

b8 comp_snapshot (COMP_Compositor* comp)
{
    ASSERT(comp);
    const usize size = (usize)comp->width * comp->height * 4;
    if (!comp->snapshot)
    {
        comp->snapshot = (u8*)os_memory_alloc(size * 2);
    }
    comp->snapshot_secure = comp->capture_valid;
    comp->snapshot_valid  = read_opaque(comp, comp->capture_valid ? comp->capture : comp->target, comp->snapshot, size) &&
                            (!comp->capture_valid || read_opaque(comp, comp->target, comp->snapshot + size, size));
    return comp->snapshot_valid;
}

b8 comp_frame_ready (const COMP_Compositor* comp)
{
    ASSERT(comp);
    return !comp->snapshots || comp->snapshot_valid;
}

b8 comp_save_png (const COMP_Compositor* comp, const char* path)
{
    ASSERT(comp);
    if (!path || !path[0])
    {
        return false;
    }

    const usize size   = (usize)comp->width * comp->height * 4;
    u8*         pixels = (u8*)os_memory_alloc(size);
    defer { os_memory_release(pixels); };
    if (!comp_read_target(comp, pixels, size))
    {
        return false;
    }

    COMP_PngWriter writer = {};
    defer { writer.bytes.release(); };
    if (!stbi_write_png_to_func(png_write, &writer, (int)comp->width, (int)comp->height, 4, pixels, (int)comp->width * 4))
    {
        LOG_ERROR("PNG encoding failed for %s", path);
        return false;
    }
    if (!os_write_file(path, writer.bytes.data, writer.bytes.length))
    {
        LOG_ERROR("Cannot write %s", path);
        return false;
    }
    LOG_INFO("Screenshot written to %s", path);
    return true;
}

COMP_DepthTarget* comp_create_depth_target (COMP_Compositor* comp, u32 width, u32 height, u32 images_count)
{
    ASSERT(comp);
    if (width == 0 || height == 0 || images_count == 0 || images_count > COMP_MAX_DEPTH_IMAGES)
    {
        return nullptr;
    }
    R_Device* device = comp->device;
    if (!comp->depth_pass)
    {
        comp->depth_pass = r_create_render_pass(device, {
            .depth_format         = COMP_ENV_DEPTH_FORMAT,
            .depth_load_op        = R_LoadOp::Clear,
            .depth_initial_layout = R_ImageLayout::ShaderReadOnly,
            .depth_final_layout   = R_ImageLayout::ShaderReadOnly,
        });
        comp->depth_pipeline = r_create_pipeline(device, {
            .vertex                  = &environment_depth_vert_shader,
            .fragment                = &environment_depth_frag_shader,
            .render_pass             = comp->depth_pass,
            .push_constants_size     = sizeof(COMP_DepthPush),
            .depth_test              = true,
            .depth_write             = true,
            .vertex_stride           = sizeof(W_VisualVertex),
            .vertex_attributes       = { { 0, R_Format::R32G32B32_SFLOAT, 0 } },
            .vertex_attributes_count = 1,
        });
        if (!comp->depth_pass || !comp->depth_pipeline)
        {
            LOG_ERROR("Failed to create the environment depth pipeline");
            return nullptr;
        }
    }

    COMP_DepthTarget* target = (COMP_DepthTarget*)os_memory_alloc(sizeof(COMP_DepthTarget));
    target->width            = width;
    target->height           = height;
    target->images_count     = images_count;
    for (u32 i = 0; i < images_count; i++)
    {
        target->images[i] = r_create_image(device, {
            .width          = width,
            .height         = height,
            .layers         = COMP_MAX_VIEWS,
            .format         = COMP_ENV_DEPTH_FORMAT,
            .usage_flags    = (u32)R_ImageUsageFlags::DepthStencilAttachment | (u32)R_ImageUsageFlags::Sampled | (u32)R_ImageUsageFlags::TransferSrc,
            .initial_layout = R_ImageLayout::ShaderReadOnly,
        });
        for (u32 v = 0; v < COMP_MAX_VIEWS && target->images[i]; v++)
        {
            target->framebuffers[i][v] = r_create_framebuffer(device, { .render_pass = comp->depth_pass, .depth_image = target->images[i], .depth_layer = v });
        }
        if (!target->images[i] || !target->framebuffers[i][0] || !target->framebuffers[i][1])
        {
            LOG_ERROR("Failed to create environment depth images");
            comp_destroy_depth_target(comp, target);
            return nullptr;
        }
    }
    return target;
}

void comp_destroy_depth_target (const COMP_Compositor* comp, COMP_DepthTarget* target)
{
    ASSERT(comp);
    if (!target)
    {
        return;
    }
    for (u32 i = 0; i < target->images_count; i++)
    {
        for (u32 v = 0; v < COMP_MAX_VIEWS; v++)
        {
            r_destroy_framebuffer(comp->device, target->framebuffers[i][v]);
        }
        r_destroy_image(comp->device, target->images[i]);
    }
    os_memory_release(target);
}

u64 comp_depth_image_native (const COMP_Compositor* comp, const COMP_DepthTarget* target, u32 index)
{
    ASSERT(comp && target && index < target->images_count);
    return r_get_image_native(comp->device, target->images[index]);
}

void comp_render_depth (const COMP_Compositor* comp, const COMP_DepthTarget* target, u32 index, const f32 clip_from_room[COMP_MAX_VIEWS][16])
{
    ASSERT(comp && target && index < target->images_count);
    R_Device*               device = comp->device;
    const R_CmdBufferHandle cmd    = r_begin_frame(device);
    for (u32 v = 0; v < COMP_MAX_VIEWS; v++)
    {
        r_cmd_begin_render_pass(device, cmd, comp->depth_pass, target->framebuffers[index][v], {});
        if (comp->room.indices_count > 0)
        {
            r_cmd_bind_pipeline(device, cmd, comp->depth_pipeline);
            r_cmd_set_viewport(device, cmd, { 0.0f, 0.0f, (f32)target->width, (f32)target->height });
            r_cmd_set_scissor(device, cmd, { 0, 0, target->width, target->height });
            r_cmd_push_constants(device, cmd, comp->depth_pipeline, clip_from_room[v], sizeof(COMP_DepthPush));
            r_cmd_bind_vertex_buffer(device, cmd, comp->room.vertices, 0);
            r_cmd_bind_index_buffer(device, cmd, comp->room.indices, 0, false);
            r_cmd_draw_indexed(device, cmd, comp->room.indices_count, 0, 0);
        }
        r_cmd_end_render_pass(device, cmd);
    }
    r_end_frame(device, cmd);
}

COMP_CameraTarget* comp_create_camera_target (const COMP_Compositor* comp, u32 width, u32 height)
{
    ASSERT(comp);
    if (width == 0 || height == 0)
    {
        return nullptr;
    }
    R_Device*          device = comp->device;
    COMP_CameraTarget* target = (COMP_CameraTarget*)os_memory_alloc(sizeof(COMP_CameraTarget));
    target->width             = width;
    target->height            = height;
    target->image             = r_create_image(device, {
        .width          = width,
        .height         = height,
        .format         = COMP_TARGET_FORMAT,
        .usage_flags    = (u32)R_ImageUsageFlags::ColorAttachment | (u32)R_ImageUsageFlags::TransferSrc,
        .initial_layout = R_ImageLayout::ColorAttachment,
    });
    target->depth = r_create_image(device, {
        .width          = width,
        .height         = height,
        .format         = COMP_DEPTH_FORMAT,
        .usage_flags    = (u32)R_ImageUsageFlags::DepthStencilAttachment,
        .initial_layout = R_ImageLayout::DepthStencilAttachment,
    });
    if (target->image && target->depth)
    {
        target->framebuffer = r_create_framebuffer(device, { .render_pass = comp->render_pass, .image = target->image, .layer = 0, .depth_image = target->depth, .depth_layer = 0 });
    }
    if (!target->framebuffer)
    {
        LOG_ERROR("Failed to create a passthrough camera target");
        comp_destroy_camera_target(comp, target);
        return nullptr;
    }
    return target;
}

void comp_destroy_camera_target (const COMP_Compositor* comp, COMP_CameraTarget* target)
{
    ASSERT(comp);
    if (!target)
    {
        return;
    }
    r_wait_idle(comp->device);
    r_destroy_framebuffer(comp->device, target->framebuffer);
    r_destroy_image(comp->device, target->image);
    r_destroy_image(comp->device, target->depth);
    os_memory_release(target);
}

b8 comp_render_camera (const COMP_Compositor* comp, const COMP_CameraTarget* target, const f32 clip_from_room[16], u8* out_rgba, usize out_size)
{
    ASSERT(comp && target && clip_from_room && out_rgba);
    if (out_size != (usize)target->width * target->height * 4)
    {
        return false;
    }
    static const COMP_PassthroughStyle style = { .opacity = 1.0f, .contrast = 1.0f, .saturation = 1.0f };
    COMP_Layer                         layer = {};
    layer.type                               = COMP_LayerType::Passthrough;
    layer.style                              = &style;
    layer.eye_visible[0]                     = true;
    memory_copy(layer.clip_from_room[0], clip_from_room, sizeof(layer.clip_from_room[0]));

    R_Device*               device = comp->device;
    const R_Rect            area   = { 0, 0, target->width, target->height };
    const R_CmdBufferHandle cmd    = r_begin_frame(device);
    r_cmd_begin_render_pass(device, cmd, comp->render_pass, target->framebuffer, { 0.0f, 0.0f, 0.0f, 1.0f });
    r_cmd_set_viewport(device, cmd, { 0.0f, 0.0f, (f32)target->width, (f32)target->height });
    r_cmd_set_scissor(device, cmd, area);
    draw_passthrough(comp, cmd, layer, 0, 0, area);
    r_cmd_end_render_pass(device, cmd);
    r_end_frame(device, cmd);
    return read_opaque(comp, target->image, out_rgba, out_size);
}
