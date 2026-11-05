#pragma once

// Shared between oxr.cpp and the graphics bindings (oxr_vulkan.cpp, oxr_gl.cpp, oxr_d3d.cpp) only. Everything else uses oxr.h.

#include "oxr.h"
#include "world.h"
#include "render.h"
#include "comp.h"
#include "rec.h"
// Also in headless-only builds, which leave out ui.cpp: the frame times are kept without panels
#include "ui.h"

#include <openxr/openxr.h>
#include <openxr/openxr_reflection.h>
#include <openxr/openxr_loader_negotiation.h>

//
// Types
//

#define OXR_RUNTIME_VERSION   XR_MAKE_VERSION(PUPPETXR_BUILD_VERSION_MAJOR, PUPPETXR_BUILD_VERSION_MINOR, PUPPETXR_BUILD_VERSION_PATCH)
#define OXR_SYSTEM_ID         1
#define OXR_MAX_EVENTS        256
#define OXR_MAX_EVENT_TEXT    64 // XR_META_virtual_keyboard commits longer text in pieces
#define OXR_MAX_BOUND_SOURCES 8
#define OXR_MAX_IMAGE_SIZE    8192
#define OXR_MAX_LAYERS        16
#define OXR_HAPTIC_FRAME_PERIOD 20000000 // XR_EXT_haptic_parametric streams: one frame every 20 ms suits the simulated outputs
#define OXR_MAX_TRIANGLE_MESH_VERTICES  65536  // per XR_FB_triangle_mesh mesh
#define OXR_MAX_TRIANGLE_MESH_TRIANGLES 131072
#define OXR_MAX_GEOMETRY_INSTANCES      64     // per session, drawn in projected passthrough layers
#define OXR_MAX_RETIRED_SPACES          8      // destroyed spaces the frames being ended may still name
#define OXR_MAX_ROOMS         64
#define OXR_MAX_QUERIES       64
#define OXR_COLOCATION_POLL_FRAMES 18 // advertisements are looked for five times a second at 90 Hz
#define OXR_MAX_SPATIAL_ENTITIES   64 // per XR_EXT_spatial_entity context; markers seen beyond it stay undiscovered

// XR_META_environment_depth: per-eye resolution and planes like Quest 3, infinite far plane
#define OXR_DEPTH_WIDTH       320
#define OXR_DEPTH_HEIGHT      320
#define OXR_DEPTH_IMAGES      3
#define OXR_DEPTH_NEAR        0.1f
#define OXR_VIEW_COUNT        2
#define OXR_FRAME_GATE_POLL_NS 100000
#define OXR_FRAME_GATE_TIMEOUT_NS 1000000000ll // a second xrWaitFrame gives up waiting for the previous frame's xrBeginFrame

#define OXR_MAX_SWAPCHAIN_IMAGES 3
#define OXR_MAX_LOOK_STEP        200.0f // pixels of one mouse event that still turn the view
#define OXR_AUDIO_DEVICE_ID_SIZE 128 // XR_MAX_AUDIO_DEVICE_STR_SIZE_OCULUS, a Windows-only header

#define OXR_EXTENSIONS(X)                                                                                  \
    X(MND_headless,       XR_MND_HEADLESS_EXTENSION_NAME,       XR_MND_headless_SPEC_VERSION)              \
    X(EXT_local_floor,    XR_EXT_LOCAL_FLOOR_EXTENSION_NAME,    XR_EXT_local_floor_SPEC_VERSION)           \
    X(KHR_locate_spaces,  XR_KHR_LOCATE_SPACES_EXTENSION_NAME,  XR_KHR_locate_spaces_SPEC_VERSION)         \
    X(KHR_vulkan_enable,  "XR_KHR_vulkan_enable",  10)         \
    X(KHR_vulkan_enable2, "XR_KHR_vulkan_enable2", 4)          \
    X(KHR_opengl_enable,    "XR_KHR_opengl_enable",    12)     \
    X(KHR_opengl_es_enable, "XR_KHR_opengl_es_enable", 10)     \
    X(MNDX_egl_enable,      "XR_MNDX_egl_enable",      2)      \
    X(KHR_D3D11_enable,     "XR_KHR_D3D11_enable",     11)     \
    X(OCULUS_audio_device_guid, "XR_OCULUS_audio_device_guid", 1) \
    X(KHR_D3D12_enable,     "XR_KHR_D3D12_enable",     11)     \
    X(KHR_metal_enable,     "XR_KHR_metal_enable",     3)      \
    X(KHRX2_metal_enable,   "XR_KHRX2_metal_enable",   1)      \
    X(EXT_future,                    XR_EXT_FUTURE_EXTENSION_NAME,                    XR_EXT_future_SPEC_VERSION)                    \
    X(FB_spatial_entity,             XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,             XR_FB_spatial_entity_SPEC_VERSION)             \
    X(FB_spatial_entity_query,       XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME,       XR_FB_spatial_entity_query_SPEC_VERSION)       \
    X(FB_spatial_entity_container,   XR_FB_SPATIAL_ENTITY_CONTAINER_EXTENSION_NAME,   XR_FB_spatial_entity_container_SPEC_VERSION)   \
    X(FB_scene,                      XR_FB_SCENE_EXTENSION_NAME,                      XR_FB_scene_SPEC_VERSION)                      \
    X(FB_scene_capture,              XR_FB_SCENE_CAPTURE_EXTENSION_NAME,              XR_FB_scene_capture_SPEC_VERSION)              \
    X(META_spatial_entity_discovery, XR_META_SPATIAL_ENTITY_DISCOVERY_EXTENSION_NAME, XR_META_spatial_entity_discovery_SPEC_VERSION) \
    X(META_spatial_entity_mesh,      XR_META_SPATIAL_ENTITY_MESH_EXTENSION_NAME,      XR_META_spatial_entity_mesh_SPEC_VERSION)      \
    X(FB_passthrough,                XR_FB_PASSTHROUGH_EXTENSION_NAME,                XR_FB_passthrough_SPEC_VERSION)                \
    X(FB_triangle_mesh,              XR_FB_TRIANGLE_MESH_EXTENSION_NAME,              XR_FB_triangle_mesh_SPEC_VERSION)              \
    X(META_environment_depth,        XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME,        XR_META_environment_depth_SPEC_VERSION)        \
    X(META_environment_raycast,      XR_META_ENVIRONMENT_RAYCAST_EXTENSION_NAME,      XR_META_environment_raycast_SPEC_VERSION)      \
    X(EXT_conformance_automation,    XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME,    XR_EXT_conformance_automation_SPEC_VERSION)    \
    X(KHR_convert_timespec_time,     "XR_KHR_convert_timespec_time",                  1)                                            \
    X(KHR_win32_convert_performance_counter_time, "XR_KHR_win32_convert_performance_counter_time", 1)                         \
    X(KHR_composition_layer_depth,   XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME,   XR_KHR_composition_layer_depth_SPEC_VERSION)   \
    X(KHR_visibility_mask,           XR_KHR_VISIBILITY_MASK_EXTENSION_NAME,           XR_KHR_visibility_mask_SPEC_VERSION)           \
    X(KHR_composition_layer_color_scale_bias, XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME, XR_KHR_composition_layer_color_scale_bias_SPEC_VERSION) \
    X(EXT_composition_layer_inverted_alpha,   XR_EXT_COMPOSITION_LAYER_INVERTED_ALPHA_EXTENSION_NAME,   XR_EXT_composition_layer_inverted_alpha_SPEC_VERSION)   \
    X(FB_composition_layer_image_layout,      XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME,      XR_FB_composition_layer_image_layout_SPEC_VERSION)      \
    X(KHR_composition_layer_cylinder,         XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME,         XR_KHR_composition_layer_cylinder_SPEC_VERSION)         \
    X(KHR_composition_layer_equirect,         XR_KHR_COMPOSITION_LAYER_EQUIRECT_EXTENSION_NAME,         XR_KHR_composition_layer_equirect_SPEC_VERSION)         \
    X(KHR_composition_layer_equirect2,        XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME,        XR_KHR_composition_layer_equirect2_SPEC_VERSION)        \
    X(KHR_composition_layer_cube,             XR_KHR_COMPOSITION_LAYER_CUBE_EXTENSION_NAME,             XR_KHR_composition_layer_cube_SPEC_VERSION)             \
    X(FB_spatial_entity_storage,              XR_FB_SPATIAL_ENTITY_STORAGE_EXTENSION_NAME,              XR_FB_spatial_entity_storage_SPEC_VERSION)              \
    X(FB_spatial_entity_storage_batch,        XR_FB_SPATIAL_ENTITY_STORAGE_BATCH_EXTENSION_NAME,        XR_FB_spatial_entity_storage_batch_SPEC_VERSION)        \
    X(META_spatial_entity_persistence,        XR_META_SPATIAL_ENTITY_PERSISTENCE_EXTENSION_NAME,        XR_META_spatial_entity_persistence_SPEC_VERSION)        \
    X(FB_spatial_entity_user,                 XR_FB_SPATIAL_ENTITY_USER_EXTENSION_NAME,                 XR_FB_spatial_entity_user_SPEC_VERSION)                 \
    X(FB_spatial_entity_sharing,              XR_FB_SPATIAL_ENTITY_SHARING_EXTENSION_NAME,              XR_FB_spatial_entity_sharing_SPEC_VERSION)              \
    X(META_spatial_entity_sharing,            XR_META_SPATIAL_ENTITY_SHARING_EXTENSION_NAME,            XR_META_spatial_entity_sharing_SPEC_VERSION)            \
    X(META_spatial_entity_group_sharing,      XR_META_SPATIAL_ENTITY_GROUP_SHARING_EXTENSION_NAME,      XR_META_spatial_entity_group_sharing_SPEC_VERSION)      \
    X(META_colocation_discovery,              XR_META_COLOCATION_DISCOVERY_EXTENSION_NAME,              XR_META_colocation_discovery_SPEC_VERSION)              \
    X(FB_touch_controller_pro,                XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME,                XR_FB_touch_controller_pro_SPEC_VERSION)                \
    X(META_touch_controller_plus,             XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME,             XR_META_touch_controller_plus_SPEC_VERSION)             \
    X(FB_touch_controller_proximity,          XR_FB_TOUCH_CONTROLLER_PROXIMITY_EXTENSION_NAME,          XR_FB_touch_controller_proximity_SPEC_VERSION)          \
    X(FB_haptic_amplitude_envelope,           XR_FB_HAPTIC_AMPLITUDE_ENVELOPE_EXTENSION_NAME,           XR_FB_haptic_amplitude_envelope_SPEC_VERSION)           \
    X(FB_haptic_pcm,                          XR_FB_HAPTIC_PCM_EXTENSION_NAME,                          XR_FB_haptic_pcm_SPEC_VERSION)                          \
    X(EXT_haptic_parametric,                  XR_EXT_HAPTIC_PARAMETRIC_EXTENSION_NAME,                  XR_EXT_haptic_parametric_SPEC_VERSION)                  \
    X(EXTX1_haptic_parametric,                "XR_EXTX1_haptic_parametric",                           1)                                                      \
    X(FB_display_refresh_rate,                XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,                XR_FB_display_refresh_rate_SPEC_VERSION)                \
    X(FB_color_space,                         XR_FB_COLOR_SPACE_EXTENSION_NAME,                         XR_FB_color_space_SPEC_VERSION)                         \
    X(META_headset_id,                        XR_META_HEADSET_ID_EXTENSION_NAME,                        XR_META_headset_id_SPEC_VERSION)                        \
    X(META_recommended_layer_resolution,      XR_META_RECOMMENDED_LAYER_RESOLUTION_EXTENSION_NAME,      XR_META_recommended_layer_resolution_SPEC_VERSION)      \
    X(META_local_dimming,                     XR_META_LOCAL_DIMMING_EXTENSION_NAME,                     XR_META_local_dimming_SPEC_VERSION)                     \
    X(META_tile_properties_hint,              XR_META_TILE_PROPERTIES_HINT_EXTENSION_NAME,              XR_META_tile_properties_hint_SPEC_VERSION)              \
    X(META_boundary_visibility,               XR_META_BOUNDARY_VISIBILITY_EXTENSION_NAME,               XR_META_boundary_visibility_SPEC_VERSION)               \
    X(META_passthrough_preferences,           XR_META_PASSTHROUGH_PREFERENCES_EXTENSION_NAME,           XR_META_passthrough_preferences_SPEC_VERSION)           \
    X(FB_passthrough_keyboard_hands,          XR_FB_PASSTHROUGH_KEYBOARD_HANDS_EXTENSION_NAME,          XR_FB_passthrough_keyboard_hands_SPEC_VERSION)          \
    X(META_passthrough_layer_resumed_event,   XR_META_PASSTHROUGH_LAYER_RESUMED_EVENT_EXTENSION_NAME,   XR_META_passthrough_layer_resumed_event_SPEC_VERSION)   \
    X(FB_composition_layer_settings,          XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME,          XR_FB_composition_layer_settings_SPEC_VERSION)          \
    X(META_automatic_layer_filter,            XR_META_AUTOMATIC_LAYER_FILTER_EXTENSION_NAME,            XR_META_automatic_layer_filter_SPEC_VERSION)            \
    X(FB_composition_layer_secure_content,    XR_FB_COMPOSITION_LAYER_SECURE_CONTENT_EXTENSION_NAME,    XR_FB_composition_layer_secure_content_SPEC_VERSION)    \
    X(FB_swapchain_update_state,              XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME,              XR_FB_swapchain_update_state_SPEC_VERSION)              \
    X(FB_swapchain_update_state_vulkan,       "XR_FB_swapchain_update_state_vulkan",                  1)                                                      \
    X(META_vulkan_swapchain_create_info,      "XR_META_vulkan_swapchain_create_info",                 1)                                                      \
    X(META_spatial_entity_semantic_label,     XR_META_SPATIAL_ENTITY_SEMANTIC_LABEL_EXTENSION_NAME,     XR_META_spatial_entity_semantic_label_SPEC_VERSION)     \
    X(META_spatial_entity_room_mesh,          XR_META_SPATIAL_ENTITY_ROOM_MESH_EXTENSION_NAME,          XR_META_spatial_entity_room_mesh_SPEC_VERSION)          \
    X(EXT_hand_tracking,                      XR_EXT_HAND_TRACKING_EXTENSION_NAME,                      XR_EXT_hand_tracking_SPEC_VERSION)                      \
    X(FB_hand_tracking_aim,                   XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME,                   XR_FB_hand_tracking_aim_SPEC_VERSION)                   \
    X(FB_hand_tracking_capsules,              XR_FB_HAND_TRACKING_CAPSULES_EXTENSION_NAME,              XR_FB_hand_tracking_capsules_SPEC_VERSION)              \
    X(FB_hand_tracking_mesh,                  XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME,                  XR_FB_hand_tracking_mesh_SPEC_VERSION)                  \
    X(EXT_hand_tracking_data_source,          XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME,          XR_EXT_hand_tracking_data_source_SPEC_VERSION)          \
    X(EXT_hand_joints_motion_range,           XR_EXT_HAND_JOINTS_MOTION_RANGE_EXTENSION_NAME,           XR_EXT_hand_joints_motion_range_SPEC_VERSION)           \
    X(FB_body_tracking,                       XR_FB_BODY_TRACKING_EXTENSION_NAME,                       XR_FB_body_tracking_SPEC_VERSION)                       \
    X(META_body_tracking_full_body,           XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME,           XR_META_body_tracking_full_body_SPEC_VERSION)           \
    X(META_body_tracking_fidelity,            XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME,            XR_META_body_tracking_fidelity_SPEC_VERSION)            \
    X(META_body_tracking_calibration,         XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME,         XR_META_body_tracking_calibration_SPEC_VERSION)         \
    X(FB_face_tracking2,                      XR_FB_FACE_TRACKING2_EXTENSION_NAME,                      XR_FB_face_tracking2_SPEC_VERSION)                      \
    X(FB_face_tracking,                       XR_FB_FACE_TRACKING_EXTENSION_NAME,                       XR_FB_face_tracking_SPEC_VERSION)                       \
    X(META_face_tracking_visemes,             XR_META_FACE_TRACKING_VISEMES_EXTENSION_NAME,             XR_META_face_tracking_visemes_SPEC_VERSION)             \
    X(FB_eye_tracking_social,                 XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME,                 XR_FB_eye_tracking_social_SPEC_VERSION)                 \
    X(EXT_eye_gaze_interaction,               XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,               XR_EXT_eye_gaze_interaction_SPEC_VERSION)               \
    X(EXT_hand_interaction,                   XR_EXT_HAND_INTERACTION_EXTENSION_NAME,                   XR_EXT_hand_interaction_SPEC_VERSION)                   \
    X(META_simultaneous_hands_and_controllers, XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME, XR_META_simultaneous_hands_and_controllers_SPEC_VERSION) \
    X(META_hand_tracking_microgestures,       XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME,       XR_META_hand_tracking_microgestures_SPEC_VERSION)       \
    X(META_hand_tracking_wide_motion_mode,    "XR_META_hand_tracking_wide_motion_mode",               1)                                                      \
    X(META_hand_tracking_wide_motion_mode2,   XR_META_HAND_TRACKING_WIDE_MOTION_MODE2_EXTENSION_NAME,   XR_META_hand_tracking_wide_motion_mode2_SPEC_VERSION)   \
    X(META_detached_controllers,              XR_META_DETACHED_CONTROLLERS_EXTENSION_NAME,              XR_META_detached_controllers_SPEC_VERSION)              \
    X(FB_render_model,                        XR_FB_RENDER_MODEL_EXTENSION_NAME,                        XR_FB_render_model_SPEC_VERSION)                        \
    X(META_virtual_keyboard,                  XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME,                  XR_META_virtual_keyboard_SPEC_VERSION)                  \
    X(FB_keyboard_tracking,                   XR_FB_KEYBOARD_TRACKING_EXTENSION_NAME,                   XR_FB_keyboard_tracking_SPEC_VERSION)                   \
    X(META_dynamic_object_tracker,            "XR_META_dynamic_object_tracker",                       1)                                                      \
    X(META_dynamic_object_keyboard,           "XR_META_dynamic_object_keyboard",                      1)                                                      \
    X(EXT_spatial_entity,                     XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME,                     XR_EXT_spatial_entity_SPEC_VERSION)                     \
    X(EXT_spatial_marker_tracking,            XR_EXT_SPATIAL_MARKER_TRACKING_EXTENSION_NAME,            XR_EXT_spatial_marker_tracking_SPEC_VERSION)            \
    X(EXT_stationary_reference_space,         XR_EXT_STATIONARY_REFERENCE_SPACE_EXTENSION_NAME,         XR_EXT_stationary_reference_space_SPEC_VERSION)         \
    X(EXTX2_stationary_reference_space,       "XR_EXTX2_stationary_reference_space",                  1)                                                      \
    X(META_performance_metrics,               XR_META_PERFORMANCE_METRICS_EXTENSION_NAME,               XR_META_performance_metrics_SPEC_VERSION)               \
    X(FB_space_warp,                          XR_FB_SPACE_WARP_EXTENSION_NAME,                          XR_FB_space_warp_SPEC_VERSION)                          \
    X(EXTX1_event_channel,                    "XR_EXTX1_event_channel",                               3)                                                      \
    X(FB_composition_layer_depth_test,        XR_FB_COMPOSITION_LAYER_DEPTH_TEST_EXTENSION_NAME,        XR_FB_composition_layer_depth_test_SPEC_VERSION)        \
    X(KHR_binding_modification,               XR_KHR_BINDING_MODIFICATION_EXTENSION_NAME,               XR_KHR_binding_modification_SPEC_VERSION)               \
    X(EXT_dpad_binding,                       XR_EXT_DPAD_BINDING_EXTENSION_NAME,                       XR_EXT_dpad_binding_SPEC_VERSION)                       \
    X(VALVE_analog_threshold,                 XR_VALVE_ANALOG_THRESHOLD_EXTENSION_NAME,                 XR_VALVE_analog_threshold_SPEC_VERSION)                 \
    X(EXT_render_model,                       XR_EXT_RENDER_MODEL_EXTENSION_NAME,                       XR_EXT_render_model_SPEC_VERSION)                       \
    X(EXT_interaction_render_model,           XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME,           XR_EXT_interaction_render_model_SPEC_VERSION)           \
    X(FB_foveation,                           XR_FB_FOVEATION_EXTENSION_NAME,                           XR_FB_foveation_SPEC_VERSION)                           \
    X(FB_foveation_configuration,             XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME,             XR_FB_foveation_configuration_SPEC_VERSION)             \
    X(META_foveation_eye_tracked,             XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME,             XR_META_foveation_eye_tracked_SPEC_VERSION)             \
    X(FB_composition_layer_alpha_blend,       XR_FB_COMPOSITION_LAYER_ALPHA_BLEND_EXTENSION_NAME,       XR_FB_composition_layer_alpha_blend_SPEC_VERSION)       \
    X(META_passthrough_color_lut,             XR_META_PASSTHROUGH_COLOR_LUT_EXTENSION_NAME,             XR_META_passthrough_color_lut_SPEC_VERSION)             \
    X(META_hand_tracking_frequency_hint,      XR_META_HAND_TRACKING_FREQUENCY_HINT_EXTENSION_NAME,      XR_META_hand_tracking_frequency_hint_SPEC_VERSION)      \
    X(META_hand_tracking_unextrapolated_poses, XR_META_HAND_TRACKING_UNEXTRAPOLATED_POSES_EXTENSION_NAME, XR_META_hand_tracking_unextrapolated_poses_SPEC_VERSION) \
    X(METAX1_passthrough_camera_data,         "XR_METAX1_passthrough_camera_data",                    1)

enum class OXR_HandleType : u8
{
    None = 0,
    Instance,
    Session,
    Space,
    Swapchain,
    ActionSet,
    Action,
    Future,
    Passthrough,
    PassthroughLayer,
    DepthProvider,
    DepthSwapchain,
    Raycaster,
    SpaceUser,
    TriangleMesh,
    GeometryInstance,
    HandTracker,
    VirtualKeyboard,
    DynamicObjectTracker,
    SpatialContext,
    SpatialEntity,
    SpatialSnapshot,
    EventChannel,
    RenderModel,
    RenderModelAsset,
    FoveationProfile,
    PassthroughColorLut,
    CameraProvider,
    BodyTracker,
    FaceTracker,
    FaceTracker2,
    EyeTracker,
};

enum class OXR_SpaceKind : u8
{
    Reference = 0,
    Action,
    Entity, // a world entity, located at its room pose
    Anchor, // XR_FB_spatial_entity anchor, fixed in stage space
    VirtualKeyboard, // XR_META_virtual_keyboard, wherever its keyboard is
    Prop,            // a prop a dynamic object tracker found (docs/props.md), wherever it is
    TrackedKeyboard, // XR_FB_keyboard_tracking: a keyboard prop in keyboard space, wherever it is
    RenderModel,     // XR_EXT_render_model: the grip of the hand holding the model's controller
};

// XrSpaceComponentTypeFB values in a dense order, one bit each in a component mask
enum class OXR_Component : u8
{
    Locatable = 0,
    Storable,
    Sharable,
    Bounded2D,
    Bounded3D,
    SemanticLabels,
    RoomLayout,
    SpaceContainer,
    TriangleMesh,
    RoomMesh,
    DynamicObjectData, // XR_META_dynamic_object_tracker
    Count,
};

enum class OXR_FutureKind : u8
{
    None = 0,
    CreateRaycaster, // payload: the raycaster, destroyed with the future unless completed
    CreateSpatialContext,  // payload: the context, destroyed with the future unless completed
    CreateSpatialSnapshot, // payload: the context to discover in
};

enum class OXR_TopLevel : u8
{
    LeftHand = 0,
    RightHand,
    Head,
    Gamepad,
    DetachedLeft,  // XR_META_detached_controllers: a controller put down while its hand is tracked
    DetachedRight,
    Eyes,          // XR_EXT_eye_gaze_interaction, the simulated gaze
    Count,
};

enum class OXR_Profile : u8
{
    Simple = 0,
    Touch,
    TouchPro,
    TouchPlus,
    MetaQuest2,    // OpenXR 1.1 names of the three above
    MetaTouchPro,
    MetaTouchPlus,
    HandInteraction, // ext/hand_interaction_ext, served by tracked hands
    EyeGaze,         // ext/eye_gaze_interaction on /user/eyes_ext
    Count,
    None = Count,
};

enum class OXR_SourceKind : u8
{
    Bool = 0,
    FloatAsBool, // float input reported as a boolean click, for khr/simple_controller select
    Float,
    Vec2,
    Vec2X,
    Vec2Y,
    Pose,
    Haptic,
    Ready, // XR_EXT_hand_interaction ready_ext: true while the hand is tracked
};

struct OXR_ProfileComponent
{
    const char*      path;   // after /user/hand/<side>
    OXR_SourceKind   kind;
    SIM_Input        input;
    SIM_HandPose     pose;
    u8               sides;  // bit 0 left, bit 1 right
    SIM_HapticOutput haptic; // Haptic components
};

// Interaction profiles of the registry, generated into oxr_profiles.inc. An availability is an
// OR of terms, each term a mask of features that must all be present.
struct OXR_SpecAvail
{
    u16 first; // index into oxr_spec_terms
    u16 count;
};

struct OXR_SpecBinding
{
    const char*  path;
    XrActionType type;
    u16          avail;
    b8           system;
};

struct OXR_SpecProfile
{
    const char* path;
    u16         avail;
    u16         first; // index into oxr_spec_bindings
    u16         count;
};

struct OXR_Extensions
{
#define X(field, name, version) b8 field;
    OXR_EXTENSIONS(X)
#undef X
};

struct OXR_Instance;
struct OXR_Session;
struct OXR_Space;
struct OXR_Swapchain;
struct OXR_ActionSet;
struct OXR_Action;
struct OXR_Future;
struct OXR_Passthrough;
struct OXR_PassthroughLayer;
struct OXR_DepthProvider;
struct OXR_DepthSwapchain;
struct OXR_Raycaster;
struct OXR_SpaceUser;
struct OXR_TriangleMesh;
struct OXR_GeometryInstance;
struct OXR_HandTracker;
struct OXR_BodyTracker;
struct OXR_FaceTracker;
struct OXR_EyeTracker;
struct OXR_VirtualKeyboard;
struct OXR_DynamicObjectTracker;
struct OXR_SpatialContext;
struct OXR_SpatialEntity;
struct OXR_SpatialSnapshot;
struct OXR_EventChannel;
struct OXR_RenderModel;
struct OXR_RenderModelAsset;
struct OXR_FoveationProfile;
struct OXR_PassthroughColorLut;
struct OXR_CameraProvider;

#define OXR_RENDER_MODEL_NODES      2 // animatable: the trigger, then the face buttons
#define OXR_MAX_RENDER_MODEL_ASSETS 16 // UUIDs one session hands out: two hands times the controller types

// What the asset of a UUID is made of, kept per session: spec, only UUIDs the session returned make assets
struct OXR_RenderModelUuid
{
    XrUuid         uuid;
    SIM_Controller controller;
    u32            hand;
};

// XR_EXT_spatial_entity buffer ids index these, so the same id always has the same text
struct OXR_SpatialText
{
    char text[SIM_MAX_PROP_TEXT];
};

using OXR_InstanceHandle  = Handle<OXR_Instance>;
using OXR_SessionHandle   = Handle<OXR_Session>;
using OXR_SpaceHandle     = Handle<OXR_Space>;
using OXR_SwapchainHandle = Handle<OXR_Swapchain>;
using OXR_ActionSetHandle = Handle<OXR_ActionSet>;
using OXR_ActionHandle    = Handle<OXR_Action>;
using OXR_FutureHandle    = Handle<OXR_Future>;
using OXR_PassthroughHandle      = Handle<OXR_Passthrough>;
using OXR_PassthroughLayerHandle = Handle<OXR_PassthroughLayer>;
using OXR_DepthProviderHandle    = Handle<OXR_DepthProvider>;
using OXR_DepthSwapchainHandle   = Handle<OXR_DepthSwapchain>;
using OXR_RaycasterHandle        = Handle<OXR_Raycaster>;
using OXR_SpaceUserHandle        = Handle<OXR_SpaceUser>;
using OXR_TriangleMeshHandle     = Handle<OXR_TriangleMesh>;
using OXR_GeometryInstanceHandle = Handle<OXR_GeometryInstance>;
using OXR_HandTrackerHandle      = Handle<OXR_HandTracker>;
using OXR_BodyTrackerHandle      = Handle<OXR_BodyTracker>;
using OXR_FaceTrackerHandle      = Handle<OXR_FaceTracker>;
using OXR_EyeTrackerHandle       = Handle<OXR_EyeTracker>;
using OXR_VirtualKeyboardHandle  = Handle<OXR_VirtualKeyboard>;
using OXR_DynamicObjectTrackerHandle = Handle<OXR_DynamicObjectTracker>;
using OXR_SpatialContextHandle  = Handle<OXR_SpatialContext>;
using OXR_SpatialEntityHandle   = Handle<OXR_SpatialEntity>;
using OXR_SpatialSnapshotHandle = Handle<OXR_SpatialSnapshot>;
using OXR_EventChannelHandle     = Handle<OXR_EventChannel>;
using OXR_RenderModelHandle      = Handle<OXR_RenderModel>;
using OXR_RenderModelAssetHandle = Handle<OXR_RenderModelAsset>;
using OXR_FoveationProfileHandle = Handle<OXR_FoveationProfile>;
using OXR_PassthroughColorLutHandle = Handle<OXR_PassthroughColorLut>;
using OXR_CameraProviderHandle      = Handle<OXR_CameraProvider>;

struct OXR_Event
{
    XrStructureType      type;
    XrSession            session;
    XrSessionState       state;
    XrTime               time;       // state change time, or the reference space change time
    XrReferenceSpaceType space_type; // reference space changes only
    Pose                 pose;       // new origin in the previous one, reference space changes only
    u64                  request_id; // XrAsyncRequestIdFB of spatial entity events
    XrResult             result;
    XrSpace              space;
    W_Uuid               uuid;
    u32                  component;  // XrSpaceComponentTypeFB
    b8                   enabled;
    u32                  location;   // XrSpaceStorageLocationFB
    u32                  advert;     // colocation discovery results: index into OXR_Instance::advert_payloads
    f32                  from_rate;  // XR_FB_display_refresh_rate
    f32                  to_rate;
    XrBoundaryVisibilityMETA boundary; // XR_META_boundary_visibility
    XrPassthroughLayerFB     layer;    // XR_META_passthrough_layer_resumed_event
    XrVirtualKeyboardMETA    keyboard; // XR_META_virtual_keyboard
    u64                      tracker;  // XR_META_dynamic_object_tracker
    u64                      spatial_context; // XR_EXT_spatial_entity discovery recommendations
    char                     text[OXR_MAX_EVENT_TEXT]; // committed text, terminated
    u64                      channel;  // XR_EXTX1_event_channel: the XrEventChannelEXTX1 it waits in, 0 for xrPollEvent
    u32                      view_index; // XR_KHR_visibility_mask
};

// An injected fault of oxr_set_anchor_fault or oxr_set_frame_fault
struct OXR_Fault
{
    XrResult result;
    u32      remaining; // requests or calls still to fail
};

struct OXR_SuggestedBinding
{
    OXR_ActionHandle action;
    u32              path = {}; // XrPath value
};

// XR_EXT_dpad_binding and XR_VALVE_analog_threshold: how a bound source turns into a boolean
enum class OXR_ModifierKind : u8
{
    None = 0,
    Dpad,      // a direction of a thumbstick
    Threshold, // a float read with hysteresis
};

#define OXR_DPAD_UP    1u
#define OXR_DPAD_DOWN  2u
#define OXR_DPAD_LEFT  4u
#define OXR_DPAD_RIGHT 8u

struct OXR_BindingModifier
{
    OXR_ModifierKind    kind = {};
    u8                  direction = {}; // Dpad: one OXR_DPAD_* bit
    b8                  sticky = {};    // Dpad
    f32                 on = {};        // Threshold: onThreshold
    f32                 off = {};       // Threshold: offThreshold
    f32                 center = {};    // Dpad: centerRegion
    f32                 wedge = {};     // Dpad: wedgeAngle
    XrHapticVibration   on_haptic = {}; // type 0 for none
    XrHapticVibration   off_haptic = {};
};

// An XrInteractionProfileDpadBindingEXT or XrInteractionProfileAnalogThresholdVALVE of a suggestion
struct OXR_SuggestedModifier
{
    OXR_ActionSetHandle set;    // Dpad
    OXR_ActionHandle    action; // Threshold
    u32                 path = {}; // the input without component (Dpad) or the binding (Threshold)
    OXR_BindingModifier modifier = {};
};

struct OXR_Instance
{
    XrVersion         api_version;
    char              application_name[XR_MAX_APPLICATION_NAME_SIZE];
    OXR_Extensions    ext;
    u64               spec_features; // mask over oxr_spec_features: api versions and enabled extensions

    Array<char>       path_chars;
    Array<u32>        path_offsets; // index i holds the start of path i + 1; path i ends at the next offset
    HashMap<u64, u32> path_lookup;  // hash -> XrPath value

    OXR_Event         events[OXR_MAX_EVENTS];
    u32               events_head;
    u32               events_count;
    u32               events_lost; // dropped from the front since the last xrPollEvent
    u64               default_channel; // XR_EXTX1_event_channel: where new events go, 0 for xrPollEvent
    OXR_Event         delayed[OXR_MAX_EVENTS]; // anchor completions waiting for their frame, in order
    u64               delayed_frames[OXR_MAX_EVENTS];
    u32               delayed_count;

    Array<OXR_SuggestedBinding> suggested[(u32)OXR_Profile::Count];
    Array<OXR_SuggestedModifier> modifiers[(u32)OXR_Profile::Count];

    u64               next_request_id; // XrAsyncRequestIdFB, never 0
    Array<W_Advert>   advert_payloads; // buffers of queued colocation discovery results, emptied with the queue

    // OpenGL and OpenGL ES binding state, owned by oxr_gl.cpp
    b8                gl_requirements_called;
    b8                gles_requirements_called;
    // D3D11 and D3D12 binding state, owned by oxr_d3d.cpp
    b8                d3d11_requirements_called;
    b8                d3d12_requirements_called;
    b8                metal_requirements_called;
    // Vulkan binding state, owned by oxr_vulkan.cpp
    b8                vk_requirements_called;
    void*             vk_get_instance_proc_addr; // from xrCreateVulkanInstanceKHR, null for KHR_vulkan_enable
    void*             vk_instance;               // the app's VkInstance once known
    void*             vk_physical_device;        // the device handed out by xrGetVulkanGraphicsDevice(2)KHR
    u64               vk_surface;                // VkSurfaceKHR of the mirror window, 0 when headless
};

struct OXR_Session
{
    OXR_InstanceHandle     instance;
    XrTime                 waited_display_time = {};
    u64                    last_wait_os_time = {}; // when the last xrWaitFrame returned, after its pacing
    XrSessionState         state = {};
    b8                     running = {};
    b8                     exit_requested = {};
    b8                     headless = {};
    b8                     frame_waited = {};
    b8                     frame_begun = {};
    b8                     begin_failed = {}; // a frame fault failed the waited frame's xrBeginFrame: the next xrWaitFrame does not wait for it
    b8                     synchronize_pending = {}; // a graphics session reaches SYNCHRONIZED with its first submitted frame

    b8                     perf_metrics = {};      // XR_META_performance_metrics enabled
    b8                     space_warp = {};        // the last ended frame had XR_FB_space_warp infos
    u32                    frames_discarded = {};  // xrBeginFrame returned XR_FRAME_DISCARDED
    u64                    frames_begun = {};      // counts xrBeginFrame, for once-per-frame rules
    XrTime                 last_display_time = {}; // of the last ended frame
    XrTime                 frame_interval = {};    // between the display times of the last two ended frames
    u64                    event_channel = {};     // XR_EXTX1_event_channel target of the session's events, 0 for the default
    u64                    room_uploaded = {};     // world_generation the compositor shows, 0 before the first upload

    Array<OXR_ActionSetHandle> attached_sets = {};
    b8                     attached = {};
    OXR_Profile            profiles[(u32)OXR_TopLevel::Count] = {}; // per top level path; each hand switches between its controller and hand tracking alone
    OXR_Profile            automation_profile = {}; // last profile activated through XR_EXT_conformance_automation
    SIM_Controller         profile_hardware = {};   // the controller the current bindings were resolved for
    SIM_HandSource         profile_sources[2] = {}; // and the hand sources

    Pose                   origin_seen = {};
    u64                    origin_generation = {}; // the local origin the app was last told about

    OXR_PassthroughHandle  passthrough;       // XR_FB_passthrough allows one per session
    OXR_DepthProviderHandle depth_provider;   // one per session

    XrColorSpaceFB         color_space = {};   // XR_FB_color_space, no conversion
    XrLocalDimmingModeMETA local_dimming = {}; // XR_META_local_dimming, last frame's
    XrBoundaryVisibilityMETA boundary = {};    // XR_META_boundary_visibility
    XrHandTrackingFrequencyHintMETA hand_frequency_hint = XR_HAND_TRACKING_FREQUENCY_HINT_DEFAULT_META; // kept, the simulated hands update every frame anyway

    // XR_META_colocation_discovery
    OS_LockFile*           advert_file = {};         // held while advertising; null during replays
    u64                    advert_request = {};
    u64                    discovery_request = {};
    u64                    discovery_next_poll = {}; // sim frame of the next look for advertisements
    Array<W_Uuid>          discovered = {};          // reported once each
    W_Uuid                 advert_uuid = {};
    b8                     advertising = {};
    b8                     discovering = {};

    u32                    geometry_count = {};
    COMP_Geometry          geometry[OXR_MAX_GEOMETRY_INSTANCES] = {}; // projected passthrough of the frame being ended

    // XR_EXT_spatial_entity: spec, entity ids are never reused in a session
    u64                    spatial_entity_ids = {};
    Array<OXR_SpatialText> spatial_texts = {};

    R_Device*              device = {};     // null for headless sessions
    COMP_Compositor*       compositor = {};
    R_WindowSwapchainHandle window_swapchain;
    R_WindowTarget          window_target = {}; // what window_swapchain presents to, kept for recreating it
    UI_Context*            ui = {};
    b8                     window_resized = {};
    u64                    window_presented_ns = {}; // the mirror's last present, which the display rate limits
    u64                    wait_thread = {};         // the thread of the last xrWaitFrame
    b8                     split_logged = {};        // that frames are waited and ended on different threads

    // XR_EXT_interaction_render_model, as of the last xrSyncActions: spec, ids are never reused
    b8                     render_models_synced = {};
    SIM_Controller         render_model_controller[2] = {};
    b8                     render_model_subaction[2] = {};         // the hand's own path is a subaction path of an active action
    b8                     render_model_paths_enumerated = {};     // XR_FB_render_model: properties need the paths first
    u32                    render_model_uuids_count = {};
    OXR_RenderModelUuid    render_model_uuids[OXR_MAX_RENDER_MODEL_ASSETS] = {};
    u64                    render_model_ids = {};                  // the last id handed out
    u64                    render_model_id[2] = {};                // per hand, 0 for none
};

struct OXR_Space
{
    OXR_SessionHandle    session;
    OXR_SpaceKind        kind = {};
    XrReferenceSpaceType reference_type = {};
    OXR_ActionHandle     action;
    OXR_TopLevel         subaction = {}; // Count means any
    Pose                 offset = {};

    u32                  entity = {};             // world entity index, Entity spaces
    u64                  world_generation = {};   // the room the entity belongs to; stale once the room changes
    W_Uuid               uuid = {};               // Entity and Anchor spaces
    Pose                 anchor_pose = {};        // Anchor spaces, in stage space
    u64                  localized_frame = {};    // Anchor spaces: unlocalized before this sim frame
    u32                  components_enabled = {}; // bit per OXR_Component
    OXR_VirtualKeyboardHandle keyboard;           // VirtualKeyboard spaces
    u32                  prop = {};               // Prop and TrackedKeyboard spaces: the SIM_Prop id
    OXR_RenderModelHandle render_model;           // RenderModel spaces; the model may be gone
};

// A space the app destroyed since its session's last xrEndFrame. Unity's OpenXR plugin replaces
// its app space within a frame and ends that frame with the space it destroyed; Quest composites it
struct OXR_RetiredSpace
{
    OXR_SpaceHandle handle;
    OXR_Space       space;
};

// A finished XR_FB_spatial_entity_query or XR_META_spatial_entity_discovery request whose results
// wait to be retrieved; spaces are created only on retrieval so unretrieved results leak nothing
struct OXR_Query
{
    OXR_SessionHandle session;
    u64               request_id = {};
    b8                discovery = {};
    u64               world_generation = {};
    Array<u32>        entities = {};
    Array<W_StoredAnchor> anchors = {}; // stored anchors found, after the entities
    Array<u32>        props = {};   // ids of the tracked props found, after the anchors
};

struct OXR_Passthrough
{
    OXR_SessionHandle session;
    b8                running = {};
};

// XR_FB_triangle_mesh states (spec: Undefined Topology, Defining Topology, Updating Mesh,
// Updating Vertices, Ready); immutable meshes are always Ready
enum class OXR_MeshState : u8
{
    Undefined = 0,
    DefiningTopology,
    UpdatingMesh,
    UpdatingVertices,
    Ready,
};

// The app writes `vertices` and `indices` between the begin and end of an update; the end
// copies them to what the compositor draws, so a projected layer shows the last finished update
struct OXR_TriangleMesh
{
    OXR_SessionHandle session;
    b8                mutable_mesh = {};
    OXR_MeshState     state = {};
    u32               max_vertices = {};
    u32               max_triangles = {};
    u32               vertices_count = {}; // of the last finished update
    u32               triangles_count = {};
    Array<Vec3>       vertices = {};       // the app's buffers, max sizes
    Array<u32>        indices = {};
    Array<Vec3>       drawn_vertices = {};
    Array<u32>        drawn_indices = {};
};

// XR_EXT_hand_tracking_data_source: which sources the app asked for. Without the extension's
// struct only an unobstructed, tracked hand is reported.
struct OXR_HandTracker
{
    OXR_SessionHandle session;
    SIM_Device        hand = {};
    b8                unobstructed = {};
    b8                controller = {};
    b8                wide_motion = {}; // XR_META_hand_tracking_wide_motion_mode: keeps reporting a hand the cameras lost
};

// XR_FB_body_tracking; the full body joint set of XR_META_body_tracking_full_body; low fidelity
// (XR_META_body_tracking_fidelity) keeps the fingers at rest
struct OXR_BodyTracker
{
    OXR_SessionHandle session;
    b8                full_body = {};
    b8                low_fidelity = {};
};

// XR_FB_face_tracking and XR_FB_face_tracking2 (their handles in separate arrays)
struct OXR_FaceTracker
{
    OXR_SessionHandle session;
    b8                audio = {}; // face tracking 2 asked for audio only
};

// XR_FB_eye_tracking_social
struct OXR_EyeTracker
{
    OXR_SessionHandle session;
};

// XR_META_dynamic_object_tracker and XR_META_dynamic_object_keyboard are previews: only Meta's
// SDK has their header, so the values of its registry and the layouts are written out here
#define OXR_OBJECT_TYPE_DYNAMIC_OBJECT_TRACKER_META                       1000288000u
#define OXR_TYPE_DYNAMIC_OBJECT_TRACKER_CREATE_INFO_META                  1000288001u
#define OXR_TYPE_DYNAMIC_OBJECT_TRACKED_CLASSES_SET_INFO_META             1000288002u
#define OXR_TYPE_DYNAMIC_OBJECT_DATA_META                                 1000288003u
#define OXR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_TRACKER_CREATE_RESULT_META     1000288004u
#define OXR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_SET_TRACKED_CLASSES_RESULT_META 1000288005u
#define OXR_TYPE_SYSTEM_DYNAMIC_OBJECT_TRACKER_PROPERTIES_META            1000288006u
#define OXR_SPACE_COMPONENT_TYPE_DYNAMIC_OBJECT_DATA_META                 1000288007u
#define OXR_DYNAMIC_OBJECT_CLASS_KEYBOARD_META                            1000587000u
#define OXR_TYPE_SYSTEM_DYNAMIC_OBJECT_KEYBOARD_PROPERTIES_META           1000587001u

struct OXR_DynamicObjectTrackerCreateInfoMETA
{
    XrStructureType type;
    const void*     next;
};

struct OXR_DynamicObjectTrackedClassesSetInfoMETA
{
    XrStructureType type;
    const void*     next;
    u32             classCount;
    const u32*      classes; // XrDynamicObjectClassMETA
};

struct OXR_DynamicObjectDataMETA
{
    XrStructureType type;
    void*           next;
    u32             classType;
};

// XrEventDataDynamicObjectTrackerCreateResultMETA and ..SetTrackedClassesResultMETA
struct OXR_EventDataDynamicObjectResultMETA
{
    XrStructureType type;
    const void*     next;
    u64             handle; // XrDynamicObjectTrackerMETA
    XrResult        result;
};

// XrSystemDynamicObjectTrackerPropertiesMETA and XrSystemDynamicObjectKeyboardPropertiesMETA
struct OXR_SystemDynamicObjectPropertiesMETA
{
    XrStructureType type;
    void*           next;
    XrBool32        supported;
};

typedef XrResult (XRAPI_PTR* PFN_xrCreateDynamicObjectTrackerMETA)(XrSession session, const OXR_DynamicObjectTrackerCreateInfoMETA* createInfo, u64* dynamicObjectTracker);
typedef XrResult (XRAPI_PTR* PFN_xrDestroyDynamicObjectTrackerMETA)(u64 dynamicObjectTracker);
typedef XrResult (XRAPI_PTR* PFN_xrSetDynamicObjectTrackedClassesMETA)(u64 dynamicObjectTracker, const OXR_DynamicObjectTrackedClassesSetInfoMETA* setInfo);
typedef XrResult (XRAPI_PTR* PFN_xrGetSpaceDynamicObjectDataMETA)(XrSpace space, OXR_DynamicObjectDataMETA* dynamicObjectDataOutput);

struct OXR_DynamicObjectTracker
{
    OXR_SessionHandle session;
    b8                keyboards = {}; // tracks XR_DYNAMIC_OBJECT_CLASS_KEYBOARD_META
};

// XR_EXTX1_event_channel (spec version 3) is a preview: only Meta's SDK has its header
#define OXR_OBJECT_TYPE_EVENT_CHANNEL_EXTX1     1000170000u
#define OXR_TYPE_EVENT_CHANNEL_CREATE_INFO_EXTX1 1000170001u
#define OXR_TYPE_EVENT_CHANNEL_TARGET_EXTX1      1000170002u
#define OXR_TYPE_SELECT_EVENT_CHANNEL_INFO_EXTX1 1000170003u
#define OXR_MAX_EVENT_CHANNELS                   16 // per instance

struct OXR_EventChannelCreateInfoEXTX1
{
    XrStructureType type;
    const void*     next;
};

// Chained to XrSessionCreateInfo: the session's events go to this channel
struct OXR_EventChannelTargetEXTX1
{
    XrStructureType type;
    const void*     next;
    u64             channel; // XrEventChannelEXTX1
};

struct OXR_SelectEventChannelInfoEXTX1
{
    XrStructureType type;
    const void*     next;
    u32             flags; // XrSelectEventChannelFlagsEXTX1, none defined
    XrDuration      timeout;
    u32             eventChannelCount;
    const u64*      eventChannels;
};

typedef XrResult (XRAPI_PTR* PFN_xrCreateEventChannelEXTX1)(XrInstance instance, const OXR_EventChannelCreateInfoEXTX1* createInfo, u64* channel);
typedef XrResult (XRAPI_PTR* PFN_xrDestroyEventChannelEXTX1)(u64 channel);
typedef XrResult (XRAPI_PTR* PFN_xrPollEventChannelEXTX1)(u64 channel, XrEventDataBuffer* eventData);
typedef XrResult (XRAPI_PTR* PFN_xrSelectEventChannelEXTX1)(XrInstance instance, OXR_SelectEventChannelInfoEXTX1* info, uint32_t* channelWithEvent);
typedef XrResult (XRAPI_PTR* PFN_xrSetDefaultEventChannelEXTX1)(XrInstance instance, u64 channel);

struct OXR_EventChannel
{
    OXR_InstanceHandle instance;
};

// XR_METAX1_passthrough_camera_data is a preview without a public header; the layouts are those
// Meta's MR Utility Kit calls it with. Cameras deliver RGBA8 images (docs/openxr.md, Passthrough
// camera data).
#define OXR_TYPE_PASSTHROUGH_CAMERA_INTRINSICS_METAX1           1000641001u
#define OXR_TYPE_PASSTHROUGH_CAMERA_EXTRINSICS_METAX1           1000641002u
#define OXR_TYPE_PASSTHROUGH_CAMERA_PROVIDER_CREATE_INFO_METAX1 1000641003u
#define OXR_TYPE_PASSTHROUGH_CAMERA_IMAGE_DATA_METAX1           1000641005u
#define OXR_PASSTHROUGH_CAMERA_POSITION_LEFT_METAX1             1u
#define OXR_PASSTHROUGH_CAMERA_POSITION_RIGHT_METAX1            2u
#define OXR_CAMERA_COUNT              2  // left and right
#define OXR_CAMERA_IMAGES             4  // per provider: the newest, those the app holds and one to render into
#define OXR_MAX_CAMERA_PROVIDERS      4  // per session
#define OXR_CAMERA_FRAME_INTERVAL_NS  16666667ll // the cameras run at 60 Hz, as Meta gives Quest 3's data rate
#define OXR_CAMERA_SENSOR_SIZE        1280       // square, in pixels
#define OXR_CAMERA_FOCAL              868.0f     // sensor pixels: about 73 degrees across the sensor

// Output arrays of xrEnumeratePassthroughCameraSourcePropertiesMETAX1 come without a type
struct OXR_PassthroughCameraSourcePropertiesMETAX1
{
    XrStructureType type;
    void*           next;
    u64             cameraId;
    u32             position; // OXR_PASSTHROUGH_CAMERA_POSITION_*
};

struct OXR_PassthroughCameraIntrinsicsMETAX1
{
    XrStructureType type;
    void*           next;
    XrVector2f      focalLength;      // sensor pixels
    XrVector2f      principalPoint;
    XrExtent2Di     sensorResolution;
};

// The pose is the camera's in the head's frame: the position in OpenXR axes, the orientation as
// Android's Camera2 LENS_POSE_ROTATION (sensor frame to camera frame)
struct OXR_PassthroughCameraExtrinsicsMETAX1
{
    XrStructureType type;
    void*           next;
    XrPosef         pose;
    u8              unknown[20]; // the struct is 64 bytes; MRUK reads only the pose
};

struct OXR_PassthroughCameraProviderCreateInfoMETAX1
{
    XrStructureType type;
    const void*     next;
    u64             cameraId;
    XrExtent2Di     resolution; // one of the supported resolutions
    u32             unknown[2]; // MRUK sets 1 and 2
};

struct OXR_PassthroughCameraImageDataMETAX1
{
    XrStructureType type;
    void*           next;
    u32             dataLength; // 0 for an image the camera has not delivered yet
    u8*             data;       // RGBA8 rows from the top, valid until the image is released
    u64             timestamp;  // capture time, microseconds of the XR_KHR_convert_timespec_time clock
};

typedef XrResult (XRAPI_PTR* PFN_xrEnumeratePassthroughCameraSourcePropertiesMETAX1)(XrSession session, u32 capacity, u32* countOutput, OXR_PassthroughCameraSourcePropertiesMETAX1* properties);
typedef XrResult (XRAPI_PTR* PFN_xrEnumeratePassthroughCameraSupportedResolutionsMETAX1)(XrSession session, u64 cameraId, u32 capacity, u32* countOutput, XrExtent2Di* resolutions);
typedef XrResult (XRAPI_PTR* PFN_xrGetPassthroughCameraIntrinsicsMETAX1)(XrSession session, u64 cameraId, OXR_PassthroughCameraIntrinsicsMETAX1* intrinsics);
typedef XrResult (XRAPI_PTR* PFN_xrGetPassthroughCameraExtrinsicsMETAX1)(XrSession session, u64 cameraId, OXR_PassthroughCameraExtrinsicsMETAX1* extrinsics);
typedef XrResult (XRAPI_PTR* PFN_xrCreatePassthroughCameraProviderMETAX1)(XrSession session, const OXR_PassthroughCameraProviderCreateInfoMETAX1* createInfo, u64* provider);
typedef XrResult (XRAPI_PTR* PFN_xrDestroyPassthroughCameraProviderMETAX1)(u64 provider);
typedef XrResult (XRAPI_PTR* PFN_xrStartPassthroughCameraProviderMETAX1)(u64 provider);
typedef XrResult (XRAPI_PTR* PFN_xrStopPassthroughCameraProviderMETAX1)(u64 provider);
typedef XrResult (XRAPI_PTR* PFN_xrAcquirePassthroughCameraImageMETAX1)(u64 provider, u64* image);
typedef XrResult (XRAPI_PTR* PFN_xrGetPassthroughCameraImageDataMETAX1)(u64 provider, u64 image, OXR_PassthroughCameraImageDataMETAX1* data);
typedef XrResult (XRAPI_PTR* PFN_xrReleasePassthroughCameraImageMETAX1)(u64 provider, u64 image);

// One buffer of a camera provider; images are numbered from 1 in the order they were rendered
struct OXR_CameraImage
{
    u8*    rgba = {};
    u64    number = {}; // 0 before the first render into it
    XrTime time = {};   // capture time
    u32    holds = {};  // acquired and not yet released
};

struct OXR_CameraProvider
{
    OXR_SessionHandle  session;
    u32                camera = {}; // 0 left, 1 right
    u32                width = {};
    u32                height = {};
    b8                 running = {};
    XrTime             next_capture = {}; // when the next image is due: the last due time plus a camera period
    u64                images_rendered = {};
    OXR_CameraImage    images[OXR_CAMERA_IMAGES] = {};
    COMP_CameraTarget* target = {};
};

// XR_FB_foveation: what a profile asks for; the compositor shows the app's images unchanged
struct OXR_Foveation
{
    XrFoveationLevelFB   level = {};          // XR_FB_foveation_configuration; NONE without a level
    f32                  vertical_offset = {};
    XrFoveationDynamicFB dynamic = {};
    b8                   eye_tracked = {};    // XR_META_foveation_eye_tracked
};

struct OXR_FoveationProfile
{
    OXR_SessionHandle session;
    OXR_Foveation     foveation = {};
};

//
// XR_EXT_spatial_entity, XR_EXT_spatial_marker_tracking: QR code props (docs/props.md) as markers
//

// The component types markers have, one bit each in a mask
enum class OXR_SpatialComponent : u8
{
    Bounded2D = 0,
    Marker,
    Count,
};

// A QR prop a context discovered; its id is the entity id for good
struct OXR_SpatialMarker
{
    u64 id;      // XrSpatialEntityIdEXT, unique in the session
    u32 prop;    // SIM_Prop id
    b8  stopped; // spec: a stopped entity never changes state again
};

struct OXR_SpatialContext
{
    OXR_SessionHandle session;
    u32               components = {};  // enabled OXR_SpatialComponent bits
    f32               marker_size = {}; // XR_SPATIAL_CAPABILITY_FEATURE_MARKER_TRACKING_FIXED_SIZE_MARKERS_EXT, 0 if not given
    b8                completed = {};   // the app holds the handle, so discovery may be recommended
    b8                recommended = {}; // a recommendation is out that no discovery answered yet
    OXR_SpatialMarker markers[OXR_MAX_SPATIAL_ENTITIES] = {};
    u32               markers_count = {};
};

struct OXR_SpatialEntity
{
    OXR_SpatialContextHandle context;
    u64                      id = {};
};

// Spec: a snapshot never changes, so it holds copies; texts live in OXR_Session::spatial_texts
struct OXR_SnapshotEntity
{
    u64                             id;
    XrSpatialEntityTrackingStateEXT state;
    u32                             components; // OXR_SpatialComponent bits the snapshot has data for
    Pose                            center;     // in the snapshot's base space
    f32                             extent;     // edge length
    u64                             buffer;     // XrSpatialBufferIdEXT of the decoded text
    XrSpatialBufferTypeEXT          buffer_type;
};

struct OXR_SpatialSnapshot
{
    OXR_SpatialContextHandle context;
    OXR_SnapshotEntity       entities[OXR_MAX_SPATIAL_ENTITIES] = {};
    u32                      entities_count = {};
};

//
// XR_EXT_render_model, XR_EXT_interaction_render_model: the controllers in the hands
//


struct OXR_RenderModel
{
    OXR_SessionHandle session;
    u64               id = {};
    u32               hand = {};       // 0 left, 1 right
    SIM_Controller    controller = {};
    XrUuid            cache_id = {};
};

struct OXR_RenderModelAsset
{
    OXR_SessionHandle session;
    Array<u8>         glb = {};
};

#define OXR_VKB_SOURCES     8   // XrVirtualKeyboardInputSourceMETA values 1 to 8
#define OXR_VKB_MAX_CONTEXT 256

// XR_META_virtual_keyboard: one per session, placed in stage space
struct OXR_VirtualKeyboard
{
    OXR_SessionHandle session;
    SIM_VkbLayout     layout = {};
    Pose              pose = zm::POSE_IDENTITY; // stage space
    f32               scale = 1.0f;
    b8                visible = {};
    b8                shifted = {};
    b8                texture_dirty = {};
    u64               changed_keys = {};              // bit per key whose animation state the app has not read
    b8                held[SIM_VKB_MAX_KEYS] = {};    // pressed by any source
    i32               pressing[OXR_VKB_SOURCES] = {}; // the key each source holds down, -1 for none
    b8                was_pressed[OXR_VKB_SOURCES] = {};
    b8                direct_near[2] = {};            // per side: its direct input was near the keys
    char              context[OXR_VKB_MAX_CONTEXT] = {}; // the end of the app's text before the cursor
};

struct OXR_GeometryInstance
{
    OXR_SessionHandle          session;
    OXR_PassthroughLayerHandle layer;
    OXR_TriangleMeshHandle     mesh;
    OXR_SpaceHandle            base_space;
    Pose                       pose = {};
    Vec3                       scale = {};
};

struct OXR_PassthroughLayer
{
    OXR_SessionHandle             session;
    OXR_PassthroughHandle         passthrough;
    XrPassthroughLayerPurposeFB   purpose = {};
    b8                            running = {};
    b8                            resume_pending = {};    // XR_META_passthrough_layer_resumed_event: report at the next frame it is ready
    f32                           keyboard_hands[2] = {}; // XR_FB_passthrough_keyboard_hands intensity, left and right
    COMP_PassthroughStyle         style = {};
    OXR_PassthroughColorLutHandle color_luts[2] = {};     // XR_META_passthrough_color_lut: the LUTs style uses
};

// XR_META_passthrough_color_lut: a LUT the app destroyed stays until no layer's style uses it
struct OXR_PassthroughColorLut
{
    OXR_SessionHandle     session;
    OXR_PassthroughHandle passthrough;
    u32                   resolution = {};
    u32                   size = {};       // bytes of the app's data
    u8*                   rgba = {};       // resolution^3 RGBA8 elements, red fastest
    u64                   generation = {}; // changes with the data
    u32                   users = {};      // passthrough layers whose style uses it
    b8                    destroyed = {};
};

struct OXR_DepthProvider
{
    OXR_SessionHandle        session;
    OXR_DepthSwapchainHandle swapchain;     // at most one
    b8                       running = {};
    b8                       hand_removal = {};  // no simulated hands, so it changes nothing
    u64                      acquired_frame = {};
    XrTime                   rendered_time = {};  // of the newest image, rendered in xrEndFrame; 0 before the first after start
    u32                      rendered_index = {};
};

struct OXR_DepthSwapchain
{
    OXR_SessionHandle       session;
    OXR_DepthProviderHandle provider;
    COMP_DepthTarget*       target = {};
    u32                     next_index = {};
};

struct OXR_Raycaster
{
    OXR_SessionHandle session;
};

// XR_FB_spatial_entity_user: a person to share anchors with
struct OXR_SpaceUser
{
    OXR_SessionHandle session;
    u64               user_id = {};
};

struct OXR_Future
{
    OXR_SessionHandle session; // the associated handle; destroying it invalidates the future
    OXR_FutureKind    kind = {};
    XrResult          result = {};
    u64               payload = {}; // the handle the operation created, if any
    u32               components = {};     // spatial discovery: OXR_SpatialComponent bits asked for, 0 for all enabled
    XrSpatialEntityTrackingStateEXT tracking_filter = {}; // spatial discovery: 0 for any
};

// XR_FB_swapchain_update_state_vulkan: XrSwapchainStateSamplerVulkanFB with the Vulkan enums as numbers,
// since vulkan.h is confined to oxr_vulkan.cpp
struct OXR_SamplerState
{
    u32 min_filter;
    u32 mag_filter;
    u32 mipmap_mode;
    u32 wrap_s;
    u32 wrap_t;
    u32 swizzle[4];
    f32 max_anisotropy;
    f32 border[4];
};

struct OXR_Swapchain
{
    OXR_SessionHandle session;
    R_ImageHandle     images[OXR_MAX_SWAPCHAIN_IMAGES];
    u32               images_count = {};
    u32               width = {};
    u32               height = {};
    u32               array_size = {};
    u32               face_count = {}; // 1, or 6 for cube maps
    OXR_SamplerState  sampler = {};    // the compositor honours the mag filter
    R_Format          format = {};
    i64               vk_format = {};
    b8                depth = {};

    // Acquire/wait/release state. Indices are handed out round robin; `acquired` is the
    // FIFO of images the app holds, the first `waited_count` of which it has waited on.
    u32               acquired[OXR_MAX_SWAPCHAIN_IMAGES] = {};
    u32               acquired_count = {};
    u32               waited_count = {};  // 0 or 1: a waited image must be released before the next wait
    u32               next_index = {};
    i32               last_released = {}; // -1 until the first release
    b8                is_static = {};     // XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT: one image, acquired once
    b8                ever_acquired = {};

    u64               foveation_profile = {}; // the XrFoveationProfileFB last applied, reported back as given
    OXR_Foveation     foveation = {};         // its settings, kept when the profile is destroyed
};

struct OXR_ActionSet
{
    OXR_InstanceHandle instance;
    char               name[XR_MAX_ACTION_SET_NAME_SIZE] = {};
    char               localized_name[XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE] = {};
    u32                priority = {};
    b8                 attached = {};
    b8                 ever_attached = {}; // stays after its session, spec xrSuggestInteractionProfileBindings
    b8                 active = {};       // in the last xrSyncActions
    u8                 active_sides = {}; // bit per OXR_TopLevel
};

struct OXR_BoundSource
{
    u32                         path = {};       // the suggested binding path, for xrEnumerateBoundSourcesForAction
    const OXR_ProfileComponent* component = {};
    u64                         identifier = {}; // hash of the path without its component, e.g. .../input/trigger
    b8                          suppressed = {}; // by a higher priority action set in the last xrSyncActions
    OXR_BindingModifier         modifier;
    b8                          on = {};         // the modified boolean in the last xrSyncActions
    u8                          latched = {};    // sticky dpad: the OXR_DPAD_* bits held until the stick returns to center
};

struct OXR_ActionState
{
    Vec2   value;
    b8     active;
    b8     changed;
    XrTime last_change_time;
};

struct OXR_Action
{
    OXR_ActionSetHandle set;
    char                name[XR_MAX_ACTION_NAME_SIZE] = {};
    char                localized_name[XR_MAX_LOCALIZED_ACTION_NAME_SIZE] = {};
    XrActionType        type = {};
    u8                  subaction_mask = {};      // sides it binds to; both hands and the eyes when created without subaction paths
    b8                  has_subaction_paths = {}; // queries with a subaction path need one
    OXR_TopLevel        pose_side = {};           // the side spaces without a subaction path track, chosen at sync, Count for none
    b8                  destroyed = {};           // by xrDestroyAction: the handle is gone, the state lives on with its set

    OXR_BoundSource     sources[(u32)OXR_TopLevel::Count][OXR_MAX_BOUND_SOURCES] = {};
    u32                 sources_count[(u32)OXR_TopLevel::Count] = {};
    OXR_ActionState     states[(u32)OXR_TopLevel::Count + 1] = {}; // last entry is the combined state
};

struct OXR_Runtime
{
    OS_Mutex*                 mutex = {};
    i64                       clock_anchor_time      = 0; // XrTime of the last frame, paired with
    u64                       clock_anchor_monotonic = 0; // os_monotonic_ns read at that frame
    u64                       clock_anchor_timespec  = 0; // and os_timespec_ns
    SparseArray<OXR_Instance>  instances = {};
    SparseArray<OXR_Session>   sessions = {};
    SparseArray<OXR_Space>     spaces = {};
    OXR_RetiredSpace           retired_spaces[OXR_MAX_RETIRED_SPACES] = {};
    u32                        retired_spaces_count = {};
    SparseArray<OXR_Swapchain> swapchains = {};
    SparseArray<OXR_ActionSet> action_sets = {};
    SparseArray<OXR_Action>    actions = {};
    SparseArray<OXR_Future>    futures = {};
    SparseArray<OXR_Passthrough>      passthroughs = {};
    SparseArray<OXR_PassthroughLayer> passthrough_layers = {};
    SparseArray<OXR_DepthProvider>    depth_providers = {};
    SparseArray<OXR_DepthSwapchain>   depth_swapchains = {};
    SparseArray<OXR_Raycaster>        raycasters = {};
    SparseArray<OXR_SpaceUser>        space_users = {};
    SparseArray<OXR_TriangleMesh>     triangle_meshes = {};
    SparseArray<OXR_HandTracker>      hand_trackers = {};
    SparseArray<OXR_BodyTracker>      body_trackers = {};
    SparseArray<OXR_FaceTracker>      face_trackers = {};
    SparseArray<OXR_FaceTracker>      face_trackers2 = {};
    SparseArray<OXR_EyeTracker>       eye_trackers = {};
    SparseArray<OXR_VirtualKeyboard>  keyboards = {};
    SparseArray<OXR_DynamicObjectTracker> dynamic_trackers = {};
    SparseArray<OXR_EventChannel>     event_channels = {};
    SparseArray<OXR_FoveationProfile> foveation_profiles = {};
    SparseArray<OXR_PassthroughColorLut> color_luts = {};
    u64                               color_lut_generation = {}; // counts LUT data writes, never 0 for data
    SparseArray<OXR_SpatialContext>   spatial_contexts = {};
    SparseArray<OXR_SpatialEntity>    spatial_entities = {};
    SparseArray<OXR_SpatialSnapshot>  spatial_snapshots = {};
    SparseArray<OXR_RenderModel>      render_models = {};
    SparseArray<OXR_RenderModelAsset> render_model_assets = {};
    SparseArray<OXR_GeometryInstance> geometry_instances = {};
    SparseArray<OXR_CameraProvider>   camera_providers = {};
    OXR_Query                  queries[OXR_MAX_QUERIES];
    u32                        queries_count = {};
    SIM_Sim*                   sim = {};
    OXR_InstanceHandle         live_instance;
    OS_Window*                 window = {};        // mirror window, null when headless
    u32                        window_x11_visual = {}; // the X visual it was made with for GLX, 0 for SDL's choice
    b8                         window_closed = {};
    b8                         window_shown = {};  // created hidden, shown once a frame is in it
    b8                         mouse_captured = {}; // a click into the view took the mouse: moves turn, the panels get no input; Escape gives it back
    b8                         capture_click = {};  // the release of the click that took the mouse is still to come
    // Frames never wait for the window thread (docs/compositor.md, Mirror window and UI): what they
    // need of the window is cached, what they change of it waits for the next window tick
    b8                         window_tick_pending = {};     // a tick is posted to the window thread and has not run
    b8                         window_relative_pending = {}; // relative mouse mode is to follow mouse_captured
    u32                        window_pixel_width = {};      // the window's size, from its creation, resizes and ticks
    u32                        window_pixel_height = {};
    f32                        window_refresh_hz = {};       // of the window's display, 0 when unknown
    b8                         lens_mask = {};     // from views.lens_mask, switched in the UI
    u64                        frame_read_thread = {}; // the thread that last read a frame off the GPU
    b8                         show_ui = {};       // toggled with controls.ui, starts from config
    b8                         show_welcome = {};      // toggled with F1, starts shown unless dismissed or switched off
    b8                         welcome_dismissed = {}; // remembered in state.json

    REC_Writer*                recorder = {};
    char                       record_path[OS_MAX_PATH] = {};
    REC_Recording*             replay = {};
    u64                        replay_cursor = {};
    b8                         replay_finished = {};
    u32                        replay_divergences = {};
    u64                        replay_first_divergence = {};
    u64                        replay_external = {}; // next external input of the replay
    // The performance tab's numbers (docs/compositor.md, Performance tab): wall clock, never fed
    // to the simulation
    UI_FrameTimes              frame_times = {};
    u64                        frame_end_ns = {};       // the last xrEndFrame, 0 before a session's first
    u64                        frame_locked_ns = {};    // the lock held since then, but for the current hold
    u64                        locked_since = {};       // the current hold began
    u64                        mirror_second_start = {};
    u64                        mirror_second_presents = {}; // mirror_presents when that second began
    b8                         exit_requested = {};   // by the control API or the end of a replay
    SIM_MirrorView             mirror_view = {};   // which eye the window shows, starts from config

    W_World*                   world = {};         // the simulated room, null for none
    char                       room[W_MAX_NAME] = {};
    char                       rooms_dir[OS_MAX_PATH] = {};
    u64                        world_generation = {}; // counts room changes; scene spaces of an older generation are stale
    char                       room_names[OXR_MAX_ROOMS][W_MAX_NAME] = {}; // listed at instance creation, for the UI
    u32                        room_names_count = {};

    char                       disabled_extensions[SIM_MAX_DISABLED_EXTENSIONS * SIM_MAX_EXTENSION_NAME] = {}; // oxr_set_disabled_extensions, comma separated

    // Anchor storage (docs/anchors.md). The room is the physical world; stage space lies in it
    // at room_from_stage, fixed for the instance.
    Pose                       room_from_stage = {};
    Pose                       stage_from_room = {};
    W_Store                    store = {};
    char                       store_path[OS_MAX_PATH] = {};   // empty when the store must not be written
    char                       app_key[W_MAX_NAME] = {};
    char                       cloud_anchors_dir[OS_MAX_PATH] = {};
    char                       cloud_adverts_dir[OS_MAX_PATH] = {};
    u64                        user_id = {};
    u32                        anchor_latency_frames = {};
    u32                        anchor_localize_frames = {};
    OXR_Fault                  anchor_faults[(u32)OXR_AnchorOp::Count] = {};
    OXR_Fault                  frame_faults[(u32)OXR_FrameOp::Count] = {};
};


//
// Shared helpers (oxr.cpp)
//

// Holds the runtime lock for its scope; `name`, the entry point's __func__, is its profile zone
struct OXR_Lock
{
    OS_ProfileZone zone;

    explicit OXR_Lock (const char* name) : zone(name)
    {
        oxr_lock();
    }

    ~OXR_Lock ()
    {
        oxr_unlock();
    }
};

OXR_Instance*  oxr_find_instance  (XrInstance instance);
OXR_Session*   oxr_find_session   (XrSession session);
OXR_Space*     oxr_find_space     (XrSpace space);
OXR_Swapchain* oxr_find_swapchain (XrSwapchain swapchain);
OXR_ActionSet* oxr_find_action_set(XrActionSet action_set);
OXR_Action*    oxr_find_action    (XrAction action);
OXR_Future*    oxr_find_future    (XrFutureEXT future);
OXR_Passthrough*      oxr_find_passthrough       (XrPassthroughFB passthrough);
OXR_PassthroughLayer* oxr_find_passthrough_layer (XrPassthroughLayerFB layer);
OXR_DepthProvider*    oxr_find_depth_provider    (XrEnvironmentDepthProviderMETA provider);
OXR_DepthSwapchain*   oxr_find_depth_swapchain   (XrEnvironmentDepthSwapchainMETA swapchain);
OXR_Raycaster*        oxr_find_raycaster         (XrEnvironmentRaycasterMETA raycaster);
OXR_SpaceUser*        oxr_find_space_user        (XrSpaceUserFB user);
XrResult       oxr_two_call       (u32 capacity, u32* count_output, const void* output, u32 count);

// NOLINTBEGIN(bugprone-macro-parentheses): var names the variable the macro declares
#define OXR_GET_INSTANCE(var, h)   OXR_Instance*  var = oxr_find_instance(h);   if (!var) return XR_ERROR_HANDLE_INVALID
#define OXR_GET_SESSION(var, h)    OXR_Session*   var = oxr_find_session(h);    if (!var) return XR_ERROR_HANDLE_INVALID
#define OXR_GET_SPACE(var, h)      OXR_Space*     var = oxr_find_space(h);      if (!var) return XR_ERROR_HANDLE_INVALID
#define OXR_GET_SWAPCHAIN(var, h)  OXR_Swapchain* var = oxr_find_swapchain(h);  if (!var) return XR_ERROR_HANDLE_INVALID
#define OXR_GET_ACTION_SET(var, h) OXR_ActionSet* var = oxr_find_action_set(h); if (!var) return XR_ERROR_HANDLE_INVALID
#define OXR_GET_ACTION(var, h)     OXR_Action*    var = oxr_find_action(h);     if (!var) return XR_ERROR_HANDLE_INVALID

#define OXR_GET_CONST_INSTANCE(var, h)   const OXR_Instance*  var = oxr_find_instance(h);   if (!var) return XR_ERROR_HANDLE_INVALID
#define OXR_GET_CONST_SESSION(var, h)    const OXR_Session*   var = oxr_find_session(h);    if (!var) return XR_ERROR_HANDLE_INVALID
#define OXR_GET_CONST_SPACE(var, h)      const OXR_Space*     var = oxr_find_space(h);      if (!var) return XR_ERROR_HANDLE_INVALID
#define OXR_GET_CONST_SWAPCHAIN(var, h)  const OXR_Swapchain* var = oxr_find_swapchain(h);  if (!var) return XR_ERROR_HANDLE_INVALID
#define OXR_GET_CONST_ACTION_SET(var, h) const OXR_ActionSet* var = oxr_find_action_set(h); if (!var) return XR_ERROR_HANDLE_INVALID
#define OXR_GET_CONST_ACTION(var, h)     const OXR_Action*    var = oxr_find_action(h);     if (!var) return XR_ERROR_HANDLE_INVALID
// NOLINTEND(bugprone-macro-parentheses)

// Rule 2: a rejected call logs why, once per call site, so an app that repeats it every frame
// shows up in the log without flooding it
#define OXR_LOG_ONCE(log, fmt, ...)            \
    do                                         \
    {                                          \
        static u32 oxr_logged_ = 0;            \
        if (os_claim_once(&oxr_logged_))       \
        {                                      \
            log(fmt, ##__VA_ARGS__);           \
        }                                      \
    }                                          \
    while (0)
#define OXR_FAIL(reason)                                                                                         \
    do                                                                                                           \
    {                                                                                                            \
        OXR_LOG_ONCE(LOG_ERROR, "%s rejected: %s (%s:%d)", FUNCTION_NAME, reason, FILE_NAME, LINE_NUMBER);      \
        return XR_ERROR_VALIDATION_FAILURE;                                                                      \
    }                                                                                                            \
    while (0)
#define OXR_CHECK(cond)      \
    do                       \
    {                        \
        if (!(cond))         \
        {                    \
            OXR_FAIL(#cond); \
        }                    \
    }                        \
    while (0)
#define OXR_CHECK_TYPE(ptr, t) OXR_CHECK((ptr) && (ptr)->type == (t))

//
// OpenGL and OpenGL ES bindings (oxr_gl.cpp)
//

PFN_xrVoidFunction oxr_gl_get_proc                (const char* name, b8 opengl_enable, b8 opengl_es_enable);
b8                 oxr_gl_is_binding              (const OXR_Instance* instance, XrStructureType type); // a GL binding whose extension is enabled
XrResult           oxr_gl_create_session_graphics (const OXR_Instance* instance, const XrBaseInStructure* graphics_binding, R_Device*& out_device, u32& out_x11_visual); // visual: what a GLX mirror window must have, 0 for EGL
XrResult           oxr_gl_write_swapchain_images  (const u64* native_images, u32 count, XrSwapchainImageBaseHeader* images, b8 es);

//
// D3D11 and D3D12 bindings (oxr_d3d.cpp)
//

PFN_xrVoidFunction oxr_d3d_get_proc                (const char* name, b8 d3d11_enable, b8 d3d12_enable);
b8                 oxr_d3d_is_binding              (const OXR_Instance* instance, XrStructureType type); // a D3D binding whose extension is enabled
XrResult           oxr_d3d_create_session_graphics (const OXR_Instance* instance, const XrBaseInStructure* graphics_binding, R_Device*& out_device);
XrResult           oxr_d3d_write_swapchain_images  (const u64* native_images, u32 count, XrSwapchainImageBaseHeader* images, b8 d3d12);

//
// Metal binding (oxr_metal.cpp, macOS only)
//

PFN_xrVoidFunction oxr_metal_get_proc                (const char* name, b8 metal_enable);
b8                 oxr_metal_is_binding              (const OXR_Instance* instance, XrStructureType type); // a Metal binding whose extension, or Unity's provisional name of it, is enabled
XrResult           oxr_metal_create_session_graphics (const OXR_Instance* instance, const XrBaseInStructure* graphics_binding, R_Device*& out_device);
XrResult           oxr_metal_write_swapchain_images  (const u64* native_images, u32 count, XrSwapchainImageBaseHeader* images);

//
// API trace (oxr_trace.cpp)
//

PFN_xrVoidFunction oxr_trace_wrap      (const char* name, PFN_xrVoidFunction real); // a wrapper logging each call of `real`, or `real` itself when the registry lacks the function
b8                 oxr_trace_path_name (XrPath path, char* out, usize size);        // oxr.cpp: the path's string while exactly one instance lives
b8                 oxr_trace_matches   (const char* filter, const char* name);    // a name of the config's "trace": comma separated, * matches any run of characters

//
// Vulkan binding (oxr_vulkan.cpp)
//

PFN_xrVoidFunction oxr_vk_get_proc                (const char* name, b8 vulkan_enable, b8 vulkan_enable2);
XrResult           oxr_vk_create_session_graphics (const OXR_Instance* instance, const XrBaseInStructure* graphics_binding, R_Device*& out_device);
XrResult           oxr_vk_write_swapchain_images  (const u64* native_images, u32 count, XrSwapchainImageBaseHeader* images);
// XR_META_vulkan_swapchain_create_info on a swapchain create chain; flags puppetxr cannot pass on are XR_ERROR_FEATURE_UNSUPPORTED
XrResult           oxr_vk_swapchain_extra_flags   (const XrBaseInStructure* chain, u32& out_create_flags, u32& out_usage_flags);
OXR_SamplerState   oxr_vk_default_sampler_state   ();
XrResult           oxr_vk_set_sampler_state       (OXR_SamplerState& state, const XrSwapchainStateBaseHeaderFB* header);
XrResult           oxr_vk_get_sampler_state       (const OXR_SamplerState& state, XrSwapchainStateBaseHeaderFB* header);
void               oxr_vk_ensure_surface          (OXR_Instance* instance); // creates the window surface once the VkInstance is known
void               oxr_vk_release_surface         (OXR_Instance* instance); // before the app can destroy its VkInstance
