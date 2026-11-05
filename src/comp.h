#pragma once

#include "sim.h"
#include "world.h"
#include "render.h"

#define COMP_MAX_LAYERS   16
#define COMP_MAX_VIEWS    2
#define COMP_MAX_GEOMETRY 64 // projected passthrough meshes per frame

struct COMP_Compositor;
struct COMP_DepthTarget;
struct COMP_CameraTarget;

enum class COMP_ViewMode : u8
{
    Left = 0, // same values as SIM_MirrorView
    Right,
    Both,
};

// One layer as seen by one display eye. Image layers are the image rectangle drawn as a quad
// whose corners are already projected into the eye; perspective-correct interpolation of the uv
// makes this exact for world-placed quads and for rotation-only reprojected projection views.
// Cylinder, Sphere and Cube layers are ray cast per pixel in the layer's frame.
struct COMP_View
{
    R_ImageHandle image;
    u32           array_layer = {};
    R_Rect        rect = {};          // sub-rectangle of the image in pixels
    f32           corners[4][4] = {}; // clip-space x, y, z, w of the rect's top-left, top-right, bottom-left, bottom-right
    f32           rays[3][4] = {};    // Cylinder, Sphere, Cube: ray = rays[0] * ndc x + rays[1] * ndc y + rays[2] in xyz, the eye position in w
    b8            visible = {};
    b8            nearest = {};       // XR_FB_swapchain_update_state_vulkan nearest filtering, else linear
};

enum class COMP_LayerType : u8
{
    Image = 0,   // a swapchain image placed by its view corners
    Passthrough, // the room's visual mesh seen from each eye
    Cylinder,    // the inside of a cylinder around the layer's y axis
    Sphere,      // the inside of a sphere, equirectangular
    Cube,        // a cube map at infinity: faces are array layers from COMP_View::array_layer on
};

// XR_FB_composition_layer_secure_content: what screenshots and captures show of the layer
enum class COMP_Secure : u8
{
    None = 0,
    Exclude, // left out
    Replace, // opaque black
};

enum class COMP_ColorMap : u8
{
    None = 0,
    MonoToRgba,
    MonoToMono,
    BrightnessContrastSaturation,
    Lut,             // XR_META_passthrough_color_lut
    InterpolatedLut,
};

// XR_FB_passthrough styles: maps index the imagery's luminance; lut colors are sRGB-encoded
struct COMP_PassthroughStyle
{
    f32           opacity;
    f32           edge_color[4]; // sRGB-encoded, alpha 0 disables edges
    COMP_ColorMap color_map;
    f32           brightness;    // -100..100, neutral 0
    f32           contrast;      // neutral 1
    f32           saturation;    // neutral 1
    u8            lut[256][4];   // MonoToRgba and MonoToMono (gray in rgb)
    const u8*     color_luts[2];            // Lut and InterpolatedLut: resolution^3 RGBA8 elements, red fastest, valid while the style is
    u64           color_lut_generations[2]; // change with the data
    u32           color_lut_resolutions[2];
    f32           color_lut_weight;
};

// XR_FB_passthrough geometry instance: an app mesh (XR_FB_triangle_mesh) a projected layer shows
// the room on; the arrays stay valid until comp_compose returns
struct COMP_Geometry
{
    const Vec3* positions;
    const u32*  indices;
    u32         vertices_count;
    u32         indices_count;
    f32         clip_from_mesh[COMP_MAX_VIEWS][16]; // column major
};

// XR_KHR_composition_layer_depth: the app's depth of a projection view; its values become the
// compositor's depth (COMP_DEPTH_NEAR in shaders/blocks.h) as map[0] + map[1] * value
struct COMP_DepthView
{
    R_ImageHandle image;
    u32           array_layer = {};
    R_Rect        rect = {}; // sub-rectangle of the image in pixels
    f32           map[2] = {};
    b8            valid = {};
};

// Spec 10.6.2: without source alpha the layer alpha is one; unpremultiplied colors are
// multiplied by alpha; then composite = layer + composite * (1 - layer alpha)
struct COMP_Layer
{
    const COMP_PassthroughStyle* style = {};                              // Passthrough layers
    const COMP_Geometry*         geometry = {};                           // projected Passthrough layers
    u32                          geometry_count = {};
    f32                          radius = {};                             // Cylinder, Sphere: 0 is infinite
    COMP_View                    views[COMP_MAX_VIEWS];                   // per display eye, Image layers
    f32                          clip_from_room[COMP_MAX_VIEWS][16] = {}; // Passthrough layers, column major
    f32                          color_scale[4] = {};
    f32                          color_bias[4] = {};
    f32                          uv_map[4] = {};                          // Cylinder, Sphere: u = angle * [0] + [1], v = height or latitude * [2] + [3]; drawn where both are in [0, 1]
    R_BlendFactor                blend_factors[4] = {};                   // XR_FB_composition_layer_alpha_blend with blend_custom: source color, target color, source alpha, target alpha
    COMP_DepthView               depth[COMP_MAX_VIEWS];                   // per display eye, projection layers with depth infos
    R_CompareOp                  depth_compare = {};                      // XR_FB_composition_layer_depth_test
    b8                           depth_test = {};
    b8                           depth_write = {};
    b8                           blend_custom = {};                       // the blend factors replace the layer blending
    COMP_LayerType               type = {};
    b8                           source_alpha = {};
    b8                           unpremultiplied = {};
    b8                           inverted_alpha = {};                     // XR_EXT_composition_layer_inverted_alpha: alpha is transparency
    b8                           flip_y = {};                             // XR_FB_composition_layer_image_layout: origin at the bottom left
    b8                           color_transform = {};                    // XR_KHR_composition_layer_color_scale_bias
    b8                           eye_visible[COMP_MAX_VIEWS] = {};        // Passthrough layers
    b8                           projected = {};                          // Passthrough layers: the room shows only on `geometry`
    COMP_Secure                  secure = {};
};

struct COMP_CreateInfo
{
    R_Device* device;
    u32       eye_width;  // the target is one eye (Left/Right) or two side by side (Both) at this resolution
    u32       eye_height;
    f32       lens_mask_radius; // black outside a circle of this many half-extents of the eye (sim mask_radius); 0 none
};

COMP_Compositor* comp_create       (const COMP_CreateInfo& info);
void             comp_destroy      (COMP_Compositor* comp);

// The room passthrough layers show; null shows nothing. Uploads the visual mesh and albedo atlas.
b8               comp_set_room     (COMP_Compositor* comp, const W_World* world);

// The props passthrough layers show with the room (docs/props.md), posed at `time` and placed in
// the room; they stay until the next call, which every composed frame should make
void             comp_set_props    (COMP_Compositor* comp, const SIM_Prop* props, u32 count, i64 time, const Pose& room_from_stage);

// Clears the target to black and composites the layers in order for the selected eye(s): Left
// and Right fill the target with that eye, Both puts eye i into horizontal slot i. Source images
// must be in R_ImageLayout::ColorAttachment and are left there.
void             comp_compose      (COMP_Compositor* comp, const COMP_Layer* layers, u32 layers_count, COMP_ViewMode mode);
void             comp_set_lens_mask (COMP_Compositor* comp, f32 radius); // as COMP_CreateInfo::lens_mask_radius, 0 none

R_ImageHandle    comp_target       (const COMP_Compositor* comp); // R8G8B8A8_SRGB at native eye resolution, in ColorAttachment layout between frames
void             comp_target_size  (const COMP_Compositor* comp, u32& out_width, u32& out_height);
b8               comp_read_target  (const COMP_Compositor* comp, u8* out_rgba, usize out_size); // sRGB-encoded bytes, 4 per pixel; what captures see, without secure layers
b8               comp_read_mirror  (const COMP_Compositor* comp, u8* out_rgba, usize out_size); // what the headset shows, secure layers included
b8               comp_save_png     (const COMP_Compositor* comp, const char* path);
void             comp_set_snapshots (COMP_Compositor* comp, b8 on); // the readers above answer from comp_snapshot's copy, never the GPU
b8               comp_snapshot     (COMP_Compositor* comp); // copies what the readers read off the GPU, on a thread owning the app's queue
b8               comp_frame_ready  (const COMP_Compositor* comp); // the readers have a frame: always without snapshots, else once one was taken

// Environment depth: D16 images of two layers (left, right eye), rendered from the room's visual
// mesh, left in ShaderReadOnly layout for the app to sample. Rendering is ordered before the app's
// later submissions on the same queue; a null room renders far depth everywhere.
COMP_DepthTarget* comp_create_depth_target  (COMP_Compositor* comp, u32 width, u32 height, u32 images_count);
void              comp_destroy_depth_target (const COMP_Compositor* comp, COMP_DepthTarget* target);
u64               comp_depth_image_native   (const COMP_Compositor* comp, const COMP_DepthTarget* target, u32 index); // VkImage
void              comp_render_depth         (const COMP_Compositor* comp, const COMP_DepthTarget* target, u32 index, const f32 clip_from_room[COMP_MAX_VIEWS][16]);

// Passthrough cameras: the room and props as passthrough shows them, seen through clip_from_room
// (column major) and read back as sRGB-encoded RGBA8 rows from the top. Reading waits for the
// GPU, so it runs on a thread owning the app's queue.
COMP_CameraTarget* comp_create_camera_target  (const COMP_Compositor* comp, u32 width, u32 height);
void               comp_destroy_camera_target (const COMP_Compositor* comp, COMP_CameraTarget* target);
b8                 comp_render_camera         (const COMP_Compositor* comp, const COMP_CameraTarget* target, const f32 clip_from_room[16], u8* out_rgba, usize out_size);
