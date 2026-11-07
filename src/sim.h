#pragma once

#include "base.h"
#include "zmath.h"

#define SIM_MAX_BINDINGS 64
#define SIM_MAX_NAME     64
#define SIM_MAX_PROFILES 16
#define SIM_MAX_PROFILE_NAME 32
#define SIM_MAX_DISABLED_EXTENSIONS 32
#define SIM_MAX_EXTENSION_NAME      128
#define SIM_MAX_VIEWS    2

// Spec 2.16: at least 50 ms of pose history, here 31 frame periods
#define SIM_HISTORY_SAMPLES   32
#define SIM_MAX_PREDICTION_NS 100000000 // extrapolation horizon past the live state

struct SIM_Sim;

enum class SIM_Device : u8
{
    Head = 0,
    LeftHand,
    RightHand,
    Count,
};

enum class SIM_HandPose : u8
{
    Grip = 0,
    Aim,
    Palm,
    Pinch, // between the thumb and index tips, oriented like aim (XR_EXT_hand_interaction pinch_ext)
    Poke,  // the index tip, oriented like its distal bone (poke_ext)
};

// What tracks a hand: the controller it holds, or optical hand tracking without one. Names
// are part of recordings and the control API.
enum class SIM_HandSource : u8
{
    Controller = 0,
    Hand,
    Count,
};

// XR_EXT_hand_joints_motion_range
enum class SIM_HandMotion : u8
{
    Unobstructed = 0,
    ConformingToController,
};

// XrHandJointEXT order
enum class SIM_HandJoint : u8
{
    Palm = 0,
    Wrist,
    ThumbMetacarpal,
    ThumbProximal,
    ThumbDistal,
    ThumbTip,
    IndexMetacarpal,
    IndexProximal,
    IndexIntermediate,
    IndexDistal,
    IndexTip,
    MiddleMetacarpal,
    MiddleProximal,
    MiddleIntermediate,
    MiddleDistal,
    MiddleTip,
    RingMetacarpal,
    RingProximal,
    RingIntermediate,
    RingDistal,
    RingTip,
    LittleMetacarpal,
    LittleProximal,
    LittleIntermediate,
    LittleDistal,
    LittleTip,
    Count,
};

#define SIM_HAND_JOINT_COUNT ((u32)SIM_HandJoint::Count)

enum class SIM_ReferenceSpace : u8
{
    View = 0,
    Local,
    Stage,
    LocalFloor,
    Stationary, // XR_EXT_stationary_reference_space: the first LOCAL origin, never recentered
};

// Graphics APIs an app may use, by their OpenXR binding extensions; names are part of the config
enum class SIM_GraphicsApi : u8
{
    Vulkan = 0,
    OpenGL,
    OpenGLES,
    D3D11,
    D3D12,
    Metal,
    Count,
};

#define SIM_GRAPHICS_ALL ((1u << (u32)SIM_GraphicsApi::Count) - 1)

enum class SIM_ClockMode : u8
{
    Realtime = 0,
    FixedStep,
};

enum class SIM_MirrorView : u8
{
    Left = 0,
    Right,
    Both,
};

enum class SIM_InputType : u8
{
    Bool = 0,
    Float,
    Vec2,
};

// Simulated controller hardware. Each type has a subset of the inputs below.
enum class SIM_Controller : u8
{
    OculusTouch = 0,
    KhrSimple,
    MetaTouchPro,  // Quest Pro
    MetaTouchPlus, // Quest 3
    Count,
};

// Union of the input components of every simulated controller. Names (sim_input_name) are
// the OpenXR component path without the /user/hand/<side>/input/ prefix and are what
// recordings and the control API use, so a name never changes once released.
enum class SIM_Input : u8
{
    XClick = 0,
    XTouch,
    YClick,
    YTouch,
    AClick,
    ATouch,
    BClick,
    BTouch,
    MenuClick,
    SystemClick,
    SqueezeValue,
    TriggerValue,
    TriggerTouch,
    Thumbstick,
    ThumbstickClick,
    ThumbstickTouch,
    ThumbrestTouch,
    SelectClick,
    TriggerProximity, // trigger/proximity_fb, trigger/proximity_meta
    ThumbProximity,   // thumb_fb/proximity_fb, thumb_meta/proximity_meta
    TriggerCurl,      // trigger/curl_fb, trigger/curl_meta
    TriggerSlide,     // trigger/slide_fb, trigger/slide_meta
    TriggerForce,
    ThumbrestForce,
    StylusForce,      // stylus_fb/force
    SwipeLeft,        // XR_META_hand_tracking_microgestures, tracked hands only; one frame pulses
    SwipeRight,
    SwipeForward,
    SwipeBackward,
    TapThumb,
    Count,
};

// Haptic outputs of a hand: /output/haptic, and on Touch Pro haptic_trigger_fb and haptic_thumb_fb
enum class SIM_HapticOutput : u8
{
    Main = 0,
    Trigger,
    Thumb,
    Count,
};

// Controller-neutral roles a key binds to; each controller type maps them per hand to its
// inputs, so one keyboard scheme fits every controller
enum class SIM_Slot : u8
{
    Trigger = 0,
    Grip,
    Primary,   // the lower face button: X, A
    Secondary, // the upper face button: Y, B
    Menu,
    System,
    Stick,
    StickClick,
    Count,
};

// What a key cluster, the mouse or the wheel drives in a control profile (SIM_Profile)
enum class SIM_Drive : u8
{
    None = 0,
    Walk,       // head and hands as one body, in the plane of the head's yaw; Q/E down and up
    MoveHead,   // the head alone, as Walk
    MoveLeft,   // a hand in the view's plane: forward and back keys up and down, Q/E toward and away
    MoveRight,
    TurnBody,   // yaw turns the hands around the head, pitch tilts the head and the hands with it
    TurnHead,
    TurnLeft,   // a hand about the world's up and its own side axis
    TurnRight,
    LeftStick,  // keys only
    RightStick,
    PushLeft,   // wheel only: a hand along the view's forward
    PushRight,
    RollLeft,   // wheel only: a hand about its grip's forward axis
    RollRight,
    TurnEyes,   // the gaze from the head (docs/face-tracking.md); lets go of a point the eyes looked at
    Count,
};

// Hand of a binding: Active is the profile's hand, else the config's primary hand
enum class SIM_BindHand : u8
{
    Active = 0,
    Left,
    Right,
};

#define SIM_MOD_SHIFT 0x1u
#define SIM_MOD_CTRL  0x2u

struct SIM_InputValue
{
    Vec2 value; // bool: x is 0 or 1, float: x in [0, 1], vec2: x and y in [-1, 1]
};

struct SIM_DeviceState
{
    Pose pose; // grip pose for hands, view pose for the head, in stage space
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    b8   active;
    b8   tracked;
};

// The tracked state at one display time: committed frames are kept in a history, the live
// state is the newest sample at the predicted display time
struct SIM_Sample
{
    i64             time;
    SIM_DeviceState devices[(u32)SIM_Device::Count];
    Pose            local_origin;
    Pose            stationary_origin;
    Pose            tracking_drift; // where STAGE lies in the simulator's frame (sim_set_tracking_drift)
};

#define SIM_MAX_HAPTIC_SAMPLES 4000      // XR_MAX_HAPTIC_PCM_BUFFER_SIZE_FB
#define SIM_HAPTIC_PCM_RATE    2000.0f   // samples per second the simulated controllers play (XR_FB_haptic_pcm)
#define SIM_MAX_HAPTIC_POINTS  1024      // parametric points kept per curve and output, two frames of XR_HAPTIC_PARAMETRIC_MAX_POINTS_TRANSIENTS_EXT
#define SIM_HAPTIC_MIN_HZ      50.0f     // the simulated actuators' frequency range (XR_EXT_haptic_parametric)
#define SIM_HAPTIC_MAX_HZ      500.0f
#define SIM_HAPTIC_HOLD_NS     50000000ll // XR_HAPTIC_PARAMETRIC_VIBRATION_EXTEND_DURATION_EXT
#define SIM_MAX_REFRESH_RATES  8 // XR_MAX_HAPTIC_AMPLITUDE_ENVELOPE_SAMPLES_FB

// amplitude is the current one, sampled from the envelope when there is one
struct SIM_Haptic
{
    b8  active;
    f32 amplitude;
    f32 frequency;
    i64 start_time;
    i64 end_time;
};

// XrHapticParametricStreamFrameTypeEXT
enum class SIM_HapticStream : u8
{
    None = 0,
    First,
    Intermediate,
    Last,
};

// A point of a parametric curve: time in ns from the start of the effect or stream, value in [0, 1]
struct SIM_HapticPoint
{
    i64 time;
    f32 value;
};

// XR_EXT_haptic_parametric: amplitude and frequency curves; no frequency points is a constant
// 0.5, a range of 0 and 0 the device's. Transients are not simulated.
struct SIM_HapticParametric
{
    const SIM_HapticPoint* amplitudes = {};
    u32                    amplitudes_count = {};
    const SIM_HapticPoint* frequencies = {};
    u32                    frequencies_count = {};
    f32                    min_hz = {};
    f32                    max_hz = {};
    SIM_HapticStream       stream = {};
    i64                    last_transient = -1; // time of the frame's last transient, -1 for none; transients are not played
};

// Latest times of a running parametric stream, from its start; -1 where it has none
struct SIM_HapticStreamEnd
{
    i64 amplitude;
    i64 frequency;
    i64 transient;
};

// Joint poses follow XR_EXT_hand_tracking: -Z along the bone towards the tip, +Y out of the back
// of the hand, the same axes for both hands
struct SIM_HandJoints
{
    Pose poses[SIM_HAND_JOINT_COUNT];
    f32  radii[SIM_HAND_JOINT_COUNT];
};

// XR_META_body_tracking_full_body order; the first 70 are XR_FB_body_tracking's. The 26 joints of
// each hand follow at LeftHandPalm and RightHandPalm in XrHandJointEXT order
enum class SIM_BodyJoint : u8
{
    Root = 0,
    Hips,
    SpineLower,
    SpineMiddle,
    SpineUpper,
    Chest,
    Neck,
    Head,
    LeftShoulder,
    LeftScapula,
    LeftArmUpper,
    LeftArmLower,
    LeftWristTwist,
    RightShoulder,
    RightScapula,
    RightArmUpper,
    RightArmLower,
    RightWristTwist,
    LeftHandPalm   = 18,
    LeftHandWrist  = 19,
    RightHandPalm  = 44,
    RightHandWrist = 45,
    LeftUpperLeg   = 70,
    LeftLowerLeg,
    LeftAnkleTwist,
    LeftAnkle,
    LeftSubtalar,
    LeftTransverse,
    LeftBall,
    RightUpperLeg,
    RightLowerLeg,
    RightAnkleTwist,
    RightAnkle,
    RightSubtalar,
    RightTransverse,
    RightBall,
    Count,
};

#define SIM_BODY_JOINT_COUNT    ((u32)SIM_BodyJoint::Count)
#define SIM_BODY_FB_JOINT_COUNT 70u
#define SIM_BODY_NO_PARENT      0xffu

enum class SIM_BodyPose : u8
{
    Stand = 0, // arms down, legs straight
    TPose,
    APose,
    ArmsForward,
    Sit, // also switches both legs to posed
    Count,
};

enum class SIM_Limb : u8
{
    LeftArm = 0,
    RightArm,
    LeftLeg,
    RightLeg,
    Count,
};

// Follow: an arm reaches for its hand's wrist, a leg stands on the floor. Posed: the joints'
// rotations alone
enum class SIM_LimbMode : u8
{
    Follow = 0,
    Posed,
    Count,
};

#define SIM_BODY_MIN_HEIGHT 0.5f
#define SIM_BODY_MAX_HEIGHT 3.0f
#define SIM_BODY_MIN_SCALE  0.25f
#define SIM_BODY_MAX_SCALE  4.0f
#define SIM_MAX_SKELETON_CHANGE_PERIOD_MS 3600000ull // an hour; longer is as good as never

// What the UI, the control API and the config set; the joints follow from it and the devices
struct SIM_BodyState
{
    f32          height;                              // metres, the T-pose's top of the head
    f32          bone_scales[SIM_BODY_JOINT_COUNT];   // the bone from the parent to the joint (sim_body_joint_scalable)
    Quat         rotations[SIM_BODY_JOINT_COUNT];     // relative to the T-pose, in the joint's T-pose frame, whose axes are the body's
    f32          confidences[SIM_BODY_JOINT_COUNT];
    b8           tracked[SIM_BODY_JOINT_COUNT];
    SIM_LimbMode limbs[(u32)SIM_Limb::Count];
};

// The T-pose of the current shape, in the body's space: root on the floor at the origin, facing -Z
struct SIM_BodySkeleton
{
    Pose poses[SIM_BODY_JOINT_COUNT];
    u8   parents[SIM_BODY_JOINT_COUNT]; // SIM_BODY_NO_PARENT for the root
};

struct SIM_BodyJoints
{
    Pose poses[SIM_BODY_JOINT_COUNT]; // stage space
    b8   tracked[SIM_BODY_JOINT_COUNT];
    f32  confidence;                  // of the whole body
};

// XR_FB_face_tracking2 order; XR_FB_face_tracking's 63 are the first
enum class SIM_FaceExpression : u8
{
    BrowLowererL,
    BrowLowererR,
    CheekPuffL,
    CheekPuffR,
    CheekRaiserL,
    CheekRaiserR,
    CheekSuckL,
    CheekSuckR,
    ChinRaiserB,
    ChinRaiserT,
    DimplerL,
    DimplerR,
    EyesClosedL,
    EyesClosedR,
    EyesLookDownL,
    EyesLookDownR,
    EyesLookLeftL,
    EyesLookLeftR,
    EyesLookRightL,
    EyesLookRightR,
    EyesLookUpL,
    EyesLookUpR,
    InnerBrowRaiserL,
    InnerBrowRaiserR,
    JawDrop,
    JawSidewaysLeft,
    JawSidewaysRight,
    JawThrust,
    LidTightenerL,
    LidTightenerR,
    LipCornerDepressorL,
    LipCornerDepressorR,
    LipCornerPullerL,
    LipCornerPullerR,
    LipFunnelerLb,
    LipFunnelerLt,
    LipFunnelerRb,
    LipFunnelerRt,
    LipPressorL,
    LipPressorR,
    LipPuckerL,
    LipPuckerR,
    LipStretcherL,
    LipStretcherR,
    LipSuckLb,
    LipSuckLt,
    LipSuckRb,
    LipSuckRt,
    LipTightenerL,
    LipTightenerR,
    LipsToward,
    LowerLipDepressorL,
    LowerLipDepressorR,
    MouthLeft,
    MouthRight,
    NoseWrinklerL,
    NoseWrinklerR,
    OuterBrowRaiserL,
    OuterBrowRaiserR,
    UpperLidRaiserL,
    UpperLidRaiserR,
    UpperLipRaiserL,
    UpperLipRaiserR,
    TongueTipInterdental,
    TongueTipAlveolar,
    TongueFrontDorsalPalate,
    TongueMidDorsalPalate,
    TongueBackDorsalVelar,
    TongueOut,
    TongueRetreat,
    Count,
};

#define SIM_FACE_EXPRESSION_COUNT ((u32)SIM_FaceExpression::Count)
#define SIM_FACE_FB1_EXPRESSION_COUNT 63u
#define SIM_FACE_VISEME_COUNT 15u // XR_META_face_tracking_visemes, sil to ou
#define SIM_FACE_MAX_GAZE_YAW   35.0f // degrees the eyes turn from the head, either way
#define SIM_FACE_MAX_GAZE_PITCH 30.0f

enum class SIM_FacePreset : u8
{
    Neutral = 0,
    Smile,
    Frown,
    Surprise,
    EyesClosed,
    JawOpen,
    Count,
};

// What the UI, the control API and the config set (docs/face-tracking.md); the eye blendshapes
// follow the gaze and blinks each frame
struct SIM_FaceState
{
    f32  weights[SIM_FACE_EXPRESSION_COUNT]; // 0 to 1
    f32  visemes[SIM_FACE_VISEME_COUNT];     // 0 to 1
    f32  confidences[2];                     // lower face, upper face
    b8   blink;                              // a blink every SIM_FACE_BLINK_PERIOD_NS
    b8   look_at;                            // the eyes look at `target`, else along the gaze angles
    Vec3 target;                             // stage space
    f32  gaze_yaw;                           // from the head, degrees, positive to the left
    f32  gaze_pitch;                         // degrees, positive up
};

// One frame of the face: the state's weights with the eyes' and the blinks', and where the eyes
// look, in stage space, -Z along the gaze
struct SIM_FaceFrame
{
    f32  weights[SIM_FACE_EXPRESSION_COUNT];
    f32  visemes[SIM_FACE_VISEME_COUNT];
    f32  confidences[2];
    Pose eyes[2];                            // left, right
};

#define SIM_HAND_MESH_MAX_VERTICES 512
#define SIM_HAND_MESH_MAX_INDICES  2048

// A low-poly hand of tapered prisms around the bones, each vertex rigidly skinned to one joint,
// modelled in the bind pose: the open hand with the wrist at the origin (XR_FB_hand_tracking_mesh)
struct SIM_HandMesh
{
    Pose bind_poses[SIM_HAND_JOINT_COUNT];
    f32  radii[SIM_HAND_JOINT_COUNT];
    u8   parents[SIM_HAND_JOINT_COUNT]; // the wrist is its own parent
    Vec3 positions[SIM_HAND_MESH_MAX_VERTICES];
    Vec3 normals[SIM_HAND_MESH_MAX_VERTICES];
    Vec2 uvs[SIM_HAND_MESH_MAX_VERTICES];
    u8   joints[SIM_HAND_MESH_MAX_VERTICES];
    u32  vertices_count;
    i16  indices[SIM_HAND_MESH_MAX_INDICES]; // counter-clockwise seen from outside
    u32  indices_count;
};

#define SIM_MODEL_MAX_PARTS    64
#define SIM_MODEL_MAX_VERTICES 2048
#define SIM_MODEL_MAX_INDICES  8192
#define SIM_MODEL_MAX_NAME     16

struct SIM_ModelPart
{
    u32  vertices_first;
    u32  vertices_count;
    u32  indices_first;
    u32  indices_count;
    Vec4 color;                    // linear RGBA
    char name[SIM_MODEL_MAX_NAME]; // empty for none
    Vec3 press;                    // non-zero: an animation moves the part this far
};

// A simple procedural render model (XR_FB_render_model): a controller in its grip space, a
// keyboard around its bounding box centre with +Y up and its front at +Z, or the virtual keyboard;
// counter-clockwise seen from outside, one part per node and material
struct SIM_RenderModel
{
    Vec3          positions[SIM_MODEL_MAX_VERTICES];
    Vec3          normals[SIM_MODEL_MAX_VERTICES];
    Vec2          uvs[SIM_MODEL_MAX_VERTICES]; // texture models only, image rows top down
    u32           vertices_count;
    u16           indices[SIM_MODEL_MAX_INDICES];
    u32           indices_count;
    SIM_ModelPart parts[SIM_MODEL_MAX_PARTS];
    u32           parts_count;
    u64           texture_id;     // non-zero: every part shows this RGBA8 texture
    u32           texture_width;
    u32           texture_height;
};

#define SIM_VKB_MAX_KEYS       48
#define SIM_VKB_TEXTURE_WIDTH  256
#define SIM_VKB_TEXTURE_HEIGHT 256
#define SIM_VKB_TEXTURE_ID     1      // the label atlas, the virtual keyboard's only texture
#define SIM_VKB_KEY_HEIGHT     0.004f // key tops stand this far in front of the panel
#define SIM_VKB_PRESS          0.003f // a pressed key sinks this far

enum class SIM_VkbKeyKind : u8
{
    Char = 0,
    Backspace,
    Enter,
    Shift,
    Space,
};

// XR_META_virtual_keyboard keys in keyboard space: they face +Z, x right, y up, meters at scale 1
struct SIM_VkbKey
{
    SIM_VkbKeyKind kind;
    char           lower; // the character a Char key commits, and its label
    char           upper; // with shift
    Vec2           center;
    Vec2           half;
    u32            cell_x; // its label in the atlas, pixels, 32 high
    u32            cell_y;
    u32            cell_width;
};

struct SIM_VkbLayout
{
    SIM_VkbKey keys[SIM_VKB_MAX_KEYS];
    u32        keys_count;
    Vec2       half; // the panel behind the keys, centred on the origin
};

#define SIM_MAX_TYPED 256 // bytes of text typed into a virtual keyboard per frame

#define SIM_MAX_PROPS     16
#define SIM_MAX_PROP_TEXT 214 // a QR code's text and its terminator (W_QR_MAX_BYTES + 1)

enum class SIM_PropKind : u8
{
    Keyboard = 0,
    QrCode,
    Count,
};

enum class SIM_PropMotion : u8
{
    Still = 0,
    Linear,
    Circle,
    Count,
};

// Something in the room the headset can track: a keyboard (the sim_keyboard_model: +Y up, its
// front at +Z, origin at its bounding box centre) or a QR code (centred in its XY plane, facing +Z)
struct SIM_Prop
{
    u32            id;     // from 1, never reused while the sim lives; ignored by sim_add_prop
    SIM_PropKind   kind;
    SIM_PropMotion motion;
    Pose           pose;   // stage space: where it is (Still), where it starts (Linear), the centre it circles (Circle)
    Vec3           target; // Linear: the far end; it goes back and forth
    f32            radius; // Circle: horizontal, counter-clockwise seen from above starting at +X, turning with the path
    f32            speed;  // meters per second along the path
    f32            size;   // QR: edge length in meters
    i64            start;  // sim time its motion started; set by sim_add_prop and sim_update_prop
    char           text[SIM_MAX_PROP_TEXT]; // QR: what it encodes
};

struct SIM_DeviceConfig
{
    Pose pose;
    b8   active;
};

struct SIM_Binding
{
    OS_Key       key;
    u8           modifiers; // SIM_MOD_* held, matched exactly
    SIM_BindHand hand;
    b8           is_slot;
    SIM_Slot     slot;      // if is_slot
    SIM_Input    input;     // otherwise
    Vec2         value;     // written to the input while the key is held
};

// What the keys and the mouse drive. Profiles with a key are picked by it and cycled by
// key_next_profile and key_previous_profile; the others only through the UI and the control API
struct SIM_Profile
{
    char         name[SIM_MAX_PROFILE_NAME]; // stable, used by the control API
    OS_Key       key;
    SIM_Drive    wasd;        // the move keys (WASD, Q/E), with any modifier
    SIM_Drive    ijkl;        // the turn keys (I/J/K/L, and the arrows), without modifiers
    SIM_Drive    mouse;
    SIM_Drive    shift_mouse; // None is the same as mouse
    SIM_Drive    wheel;
    SIM_Drive    shift_wheel; // None is the same as wheel
    SIM_BindHand hand;        // of bare bindings and the hand source key; Active is the config's primary hand
};

// Everything that defines what an app observes in one frame, as recorded and replayed.
// Velocities, haptics and the frame index are derived or app-driven and are not part of it.
struct SIM_DeviceFrame
{
    Pose           pose;
    b8             active;
    SIM_HandSource source;   // always Controller for the head
    Pose           detached; // the controller put down while the hand is tracked (source Hand)
    SIM_InputValue inputs[(u32)SIM_Input::Count]; // always zero for the head
};

struct SIM_FrameState
{
    i64             dt; // clock advance of this frame
    Pose            local_origin;
    SIM_DeviceFrame devices[(u32)SIM_Device::Count];
    char            typed[SIM_MAX_TYPED]; // text typed into the app's virtual keyboard this frame
    u32             typed_length;
    b8              props_present;        // false: the props did not change, keep them
    u32             props_count;
    SIM_Prop        props[SIM_MAX_PROPS];
    b8              body_present;         // false: the body did not change, keep it
    SIM_BodyState   body;
    b8              face_present;         // false: the face did not change, keep it
    SIM_FaceState   face;
    b8              drift_present;        // false: the tracking drift did not change, keep it
    Pose            tracking_drift;
};

// Where a recording starts: the frame state plus what the next frame derives from it
struct SIM_Snapshot
{
    SIM_FrameState state;
    Pose           previous_poses[(u32)SIM_Device::Count]; // velocities of the next frame are measured from these
    i64            time;
    u64            frame_index;
};

// Where the tracking origin (stage space) lies in the simulated room at instance creation
enum class SIM_OriginOffset : u8
{
    None = 0, // stage origin is room origin
    Fixed,    // origin_offset
    PerRun,   // a new offset every instance, from origin_seed and the anchor store's run count
};

struct SIM_Config
{
    const char*      known_module;               // a library of the known module table found in the process, null when none
    char             runtime_name[SIM_MAX_NAME]; // reported by xrGetInstanceProperties; some engine plugins accept only their vendor's
    char             device[16];                 // the preset the headset started from (sim_apply_device)
    char             system_name[SIM_MAX_NAME];  // reported by xrGetSystemProperties; OVRPlugin tells Quest models apart by it
    char             trace[256];                 // xr* functions whose calls are logged with their arguments and results: "*" all, else names with * wildcards, comma separated; empty none (docs/openxr.md, API trace)
    u32              vendor_id;
    u8               headset_id[16];             // XR_META_headset_id; OVRPlugin's headset type comes from it
    b8               device_from_env;            // PUPPETXR_DEVICE chose the preset, which the file's "device" does not change
    b8               lens_mask;                  // the lens mask on: drawn black and given to apps (XR_KHR_visibility_mask); off shows the whole view
    b8               accept_time_zero;           // locates at time 0 use the latest time, for OVRPlugin; the spec says invalid
    b8               accept_older_frame_time;    // xrGetRecommendedLayerResolutionMETA answers for any frame not yet ended, for Meta's Unreal plugin; the extension says invalid
    b8               discover_before_running;    // xrDiscoverSpacesMETA answers before the session runs, for MRUK
    b8               discover_before_running_set; // the file or the environment chose it; a module loaded later does not change it
    b8               headless;
    b8               show_ui;
    b8               show_welcome; // the welcome overlay at start, unless the user dismissed it for good
    b8               replay_exit; // request session exit when a replay ends
    SIM_ClockMode    clock_mode;
    u32              window_width;
    u32              window_height;
    b8               window_maximized; // window_width and window_height are then the restored size

    f32              refresh_rate_hz;
    i64              display_period_ns; // derived from refresh_rate_hz
    f32              refresh_rates[SIM_MAX_REFRESH_RATES]; // XR_FB_display_refresh_rate choices besides refresh_rate_hz
    u32              refresh_rates_count;

    u32              view_width;  // per eye; apps cannot change resolution later, so this is fixed per run
    u32              view_height;
    f32              fov_degrees; // full angle per eye, both axes
    f32              mask_radius; // lens mask: an ellipse this many half-extents of the view wide cuts the corners; 0 none
    f32              ipd;
    SIM_MirrorView   mirror_view;
    SIM_Controller   controller; // both hands

    Vec2             stage_bounds;
    SIM_DeviceConfig devices[(u32)SIM_Device::Count];
    Pose             aim_offset;
    Pose             palm_offset;

    f32              move_speed;       // m/s
    f32              look_sensitivity; // rad/pixel
    f32              turn_speed;       // rad/s
    f32              analog_ramp;      // s for a held key to push a trigger, grip or stick from 0 to full; 0 instant
    f32              analog_release;   // s for it to fall back from full to 0 once released; 0 instant
    f32              hand_tracking_fov_degrees; // full angle of the cone around the head's forward axis where hand tracking sees hands
    i64              skeleton_change_period_ns; // body_tracking.skeleton_change_period_ms: a new skeleton this often, the shape the same; 0 never
    SIM_BodyState    body;                      // the simulated body at start and after a reset (docs/body-tracking.md)
    SIM_FaceState    face;                      // the simulated face at start and after a reset (docs/face-tracking.md)
    SIM_Device       primary_hand;     // hand of bare bindings in profiles without a hand of their own
    f32              hand_speed;       // m/s of a hand moved by keys
    b8               auto_touch;       // keys pressing a click, trigger or stick also touch it
    b8               passthrough_default_active; // XR_META_passthrough_preferences: the user prefers to start in passthrough
    b8               keyboard_tracking;          // the user's setting; off, XR_META_dynamic_object_tracker fails to initialize
    SIM_Prop         props[SIM_MAX_PROPS]; // spawned at start; config "props", env PUPPETXR_PROPS
    u32              props_count;
    OS_Key           key_move_forward;
    OS_Key           key_move_back;
    OS_Key           key_move_left;
    OS_Key           key_move_right;
    OS_Key           key_move_up;
    OS_Key           key_move_down;
    OS_Key           key_turn_left;
    OS_Key           key_turn_right;
    OS_Key           key_turn_up;
    OS_Key           key_turn_down;
    OS_Key           key_reset_hands;
    OS_Key           key_reset;
    OS_Key           key_freeze; // holds ramping inputs where they are until their keys are released
    OS_Key           key_hand_source; // switches a hand between its controller and hand tracking; bare the active hand, Shift the left, Ctrl the right
    OS_Key           key_ui;          // shows and hides the panels
    OS_Key           key_next_profile;     // the next profile with a key
    OS_Key           key_previous_profile; // the one before
    SIM_Profile      profiles[SIM_MAX_PROFILES]; // the built-in ones first (sim_default_config)
    u32              profiles_count;
    SIM_Binding      bindings[SIM_MAX_BINDINGS];
    u32              bindings_count;

    u64              screenshot_at_frame; // 0 means never
    u64              exit_at_frame;       // 0 means never
    char             record_path[OS_MAX_PATH];
    char             replay_path[OS_MAX_PATH];
    char             screenshot_dir[OS_MAX_PATH];
    char             profile_path[OS_MAX_PATH]; // a profile capture from xrCreateInstance to the runtime's shutdown; empty for none
    char             room[SIM_MAX_NAME];     // empty means no room
    b8               room_explicit;          // set by the config file or the environment, so the remembered room does not apply
    char             rooms_dir[OS_MAX_PATH]; // empty means the built-in rooms

    b8               control;             // listen for puppetxrctl, off by default
    u32              control_port;        // 0 lets the system pick one
    char             control_address[64]; // 127.0.0.1; any other address exposes the simulator to that network
    b8               control_paused;      // hold the first frame until a client steps or resumes

    char             disabled_extensions[SIM_MAX_DISABLED_EXTENSIONS][SIM_MAX_EXTENSION_NAME]; // never offered, to test apps without them
    u32              disabled_extensions_count;
    u32              graphics_apis; // bit per SIM_GraphicsApi whose binding extensions are offered

    Vec3             drift_velocity; // m/s the tracking origin drifts, in the simulator's frame (sim_set_tracking_drift_rate)
    f32              drift_yaw_rate; // degrees per second it turns about +Y
    SIM_OriginOffset origin_offset_mode;
    Pose             origin_offset; // Fixed: the stage origin in room space, on the floor and turned about Y only
    u64              origin_seed;   // PerRun

    char             anchors_dir[OS_MAX_PATH]; // local anchor stores; empty means os_state_dir()/anchors
    char             cloud_dir[OS_MAX_PATH];   // the simulated cloud shared between users; empty means <anchors_dir>/cloud
    char             app_id[SIM_MAX_NAME];     // what anchors are stored under; empty means the application name
    u64              user_id;                  // the simulated user, never 0
    u32              anchor_latency_frames;    // frames until anchor storage and sharing requests complete
    u32              anchor_localize_frames;   // frames a loaded anchor stays unlocalized
};

// Choices made in the UI that the next start picks up, in os_state_dir()/state.json
struct SIM_State
{
    b8   has_room;
    char room[SIM_MAX_NAME]; // empty means none
    b8   welcome_dismissed;
};

//
// Config
//

void              sim_default_config         (SIM_Config& out_config);
b8                sim_parse_config           (SIM_Config& config, const char* json);
b8                sim_load_config            (SIM_Config& config, const char* path); // file on top of the current contents
void              sim_apply_env              (SIM_Config& config);
b8                sim_find_config_path       (char* out_path, usize out_size);
b8                sim_apply_device           (SIM_Config& config, const char* name); // a headset preset's name, ids, controller, eye size and rates; false and unchanged for an unknown one
u32               sim_device_count           ();
const char*       sim_device_name            (u32 index);
const char*       sim_apply_known_modules    (SIM_Config& config, b8 (*loaded)(const char* module)); // defaults of the loaded known modules; returns the first or null
const char*       sim_late_discovery_module  (const SIM_Config& config, b8 (*loaded)(const char* module)); // a known module discovering before running, loaded after the config was read; null when none or the config chose
void              sim_load_full_config       (SIM_Config& out_config); // defaults, known modules, the config file, then the environment
b8                sim_extension_disabled     (const SIM_Config& config, const char* name);
b8                sim_graphics_api_allowed   (const SIM_Config& config, SIM_GraphicsApi api);
const char*       sim_graphics_api_name      (SIM_GraphicsApi api); // "unknown" when out of range

b8                sim_parse_state            (const char* json, SIM_State& out_state);
b8                sim_load_state             (const char* path, SIM_State& out_state);
b8                sim_save_state             (const char* path, const SIM_State& state);
void              sim_apply_state            (SIM_Config& config, const SIM_State& state); // only what config and environment left unset, never headless

//
// Lifecycle and frames
//

SIM_Sim*          sim_create                 (const SIM_Config& config);
void              sim_destroy                (SIM_Sim* sim);
const SIM_Config& sim_get_config             (const SIM_Sim* sim);

void              sim_start_clock            (SIM_Sim* sim, u64 os_time_ns); // the wall time the realtime clock counts its first frame from; later calls do nothing
void              sim_begin_frame            (SIM_Sim* sim, u64 os_time_ns); // advances the clock, applies controls
void              sim_commit_frame           (SIM_Sim* sim);                 // velocities, haptic expiry, frame index

void              sim_capture_state          (const SIM_Sim* sim, SIM_FrameState& out_state); // dt is the last frame's advance
void              sim_capture_snapshot       (const SIM_Sim* sim, SIM_Snapshot& out_snapshot);
void              sim_restore_snapshot       (SIM_Sim* sim, const SIM_Snapshot& snapshot);
void              sim_replay_frame           (SIM_Sim* sim, const SIM_FrameState& state);     // replaces sim_begin_frame during replay

i64               sim_time                   (const SIM_Sim* sim);
i64               sim_predicted_display_time (const SIM_Sim* sim);
i64               sim_display_period         (const SIM_Sim* sim);
f32               sim_refresh_rate           (const SIM_Sim* sim);
f32               sim_default_refresh_rate   (const SIM_Sim* sim); // the configured one
// The refresh rates the display supports, sorted; returns the count and fills at most capacity
u32               sim_refresh_rates          (const SIM_Sim* sim, f32* out_rates, u32 capacity);
// One of sim_refresh_rates; changes the display period from the next frame and the config a recording writes
b8                sim_set_refresh_rate       (SIM_Sim* sim, f32 hz);
u64               sim_frame_index            (const SIM_Sim* sim);
SIM_ClockMode     sim_clock_mode             (const SIM_Sim* sim);
void              sim_set_clock_mode         (SIM_Sim* sim, SIM_ClockMode mode);

//
// Devices and inputs
//

Pose              sim_get_device_pose        (const SIM_Sim* sim, SIM_Device device);
void              sim_set_device_pose        (SIM_Sim* sim, SIM_Device device, const Pose& pose);
SIM_DeviceState   sim_get_device_state       (const SIM_Sim* sim, SIM_Device device);
void              sim_set_device_active      (SIM_Sim* sim, SIM_Device device, b8 active);
Pose              sim_get_hand_pose          (const SIM_Sim* sim, SIM_Device hand, SIM_HandPose kind);
void              sim_set_hand_pose          (SIM_Sim* sim, SIM_Device hand, SIM_HandPose kind, const Pose& pose); // moves the grip so the kind's pose lands on pose
// Moves the grip so `ray`, a pose in grip space looking down its -Z, starts at the view's eye and
// passes through (u, v) of its image, 0 0 top left and 1 1 bottom right
void              sim_point_hand             (SIM_Sim* sim, SIM_Device hand, u32 view, f32 u, f32 v, const Pose& ray);

const char*       sim_device_name            (SIM_Device device); // "head", "left", "right"; stable, used by recordings
b8                sim_device_from_name       (const char* name, SIM_Device& out_device);

SIM_Controller    sim_get_controller         (const SIM_Sim* sim);
void              sim_set_controller         (SIM_Sim* sim, SIM_Controller controller); // zeroes inputs the new type lacks
const char*       sim_controller_name        (SIM_Controller controller);
b8                sim_controller_from_name   (const char* name, SIM_Controller& out_controller);
b8                sim_controller_has_input   (SIM_Controller controller, SIM_Input input);
SIM_Input         sim_controller_slot_input  (SIM_Controller controller, SIM_Device hand, SIM_Slot slot); // Count if unmapped
const char*       sim_slot_name              (SIM_Slot slot);
b8                sim_slot_from_name         (const char* name, SIM_Slot& out_slot);

SIM_HandSource    sim_get_hand_source        (const SIM_Sim* sim, SIM_Device hand);
void              sim_set_hand_source        (SIM_Sim* sim, SIM_Device hand, SIM_HandSource source); // zeroes inputs the source lacks
const char*       sim_hand_source_name       (SIM_HandSource source); // "controller", "hand"; stable, used by recordings
b8                sim_hand_source_from_name  (const char* name, SIM_HandSource& out_source);
b8                sim_hand_has_input         (SIM_HandSource source, SIM_Controller controller, SIM_Device hand, SIM_Input input); // a Touch's X, Y and menu are the left one's, A, B and system the right one's; a tracked hand has trigger/value (pinch), squeeze/value (grab), menu/click and the microgestures
b8                sim_hand_in_view           (const SIM_Sim* sim, const SIM_Sample& sample, SIM_Device hand); // within the hand tracking cone of the sampled head
void              sim_hand_joints            (const SIM_Sim* sim, SIM_Device hand, const Pose& grip, SIM_HandMotion motion, SIM_HandJoints& out_joints); // stage space, fingers from the hand's current inputs
const SIM_HandMesh& sim_get_hand_mesh        (const SIM_Sim* sim, SIM_Device hand);
Pose              sim_get_detached_pose      (const SIM_Sim* sim, SIM_Device hand); // grip pose of the controller a tracked hand put down where it switched to hand tracking
void              sim_set_detached_pose      (SIM_Sim* sim, SIM_Device hand, const Pose& pose);
Pose              sim_detached_hand_pose     (const SIM_Sim* sim, SIM_Device hand, SIM_HandPose kind); // no hand holds it: pinch and poke are its aim pose
void              sim_controller_model       (SIM_Controller controller, SIM_Device hand, SIM_RenderModel& out_model);
void              sim_keyboard_model         (SIM_RenderModel& out_model);

// Face and eye tracking (docs/face-tracking.md)
const SIM_FaceState& sim_get_face            (const SIM_Sim* sim);
void              sim_set_face               (SIM_Sim* sim, const SIM_FaceState& state); // clamps, keeps the last of invalid values
void              sim_default_face           (SIM_FaceState& out_state);
void              sim_reset_face             (SIM_Sim* sim); // to the configured face
void              sim_set_face_preset        (SIM_Sim* sim, SIM_FacePreset preset);
void              sim_face_frame             (const SIM_Sim* sim, const SIM_Sample& sample, SIM_FaceFrame& out_frame);
b8                sim_face_apply_json        (const JsonValue& root, SIM_FaceState& inout_state); // all or nothing; members preset, weights, visemes, confidences, blink, gaze, look_at
b8                sim_face_to_json           (const SIM_FaceState& state, char* out, usize size); // what differs from neutral; false if it does not fit
const char*       sim_face_expression_name   (SIM_FaceExpression expression); // XR_FB_face_tracking2's in lower case, "jaw_drop"; stable, used by recordings
b8                sim_face_expression_from_name (const char* name, SIM_FaceExpression& out_expression);
const char*       sim_face_viseme_name       (u32 viseme); // "sil" .. "ou"
b8                sim_face_viseme_from_name  (const char* name, u32& out_viseme);
const char*       sim_face_preset_name       (SIM_FacePreset preset);
b8                sim_face_preset_from_name  (const char* name, SIM_FacePreset& out_preset);

// Body tracking (docs/body-tracking.md). Setters validate and clamp; a shape change (height or a
// bone scale) raises the skeleton generation
const SIM_BodyState& sim_get_body            (const SIM_Sim* sim);
void              sim_set_body               (SIM_Sim* sim, const SIM_BodyState& state);
void              sim_default_body           (f32 height, SIM_BodyState& out_state);
void              sim_reset_body             (SIM_Sim* sim); // to the configured body
b8                sim_body_apply_json        (const JsonValue& root, SIM_BodyState& inout_state); // all or nothing; members height, pose, bones, joints, limbs
b8                sim_body_to_json           (const SIM_BodyState& state, char* out, usize size); // what differs from standing; false if it does not fit
void              sim_set_body_pose          (SIM_Sim* sim, SIM_BodyPose pose);
void              sim_set_body_joint         (SIM_Sim* sim, SIM_BodyJoint joint, Quat rotation); // switches its limb to posed
u32               sim_body_generation        (const SIM_Sim* sim);
u32               sim_body_skeleton_changes  (const SIM_Sim* sim, i64 time); // XrBodyJointLocationsFB::skeletonChangedCount at time: the generation and one per period passed
i64               sim_get_skeleton_change_period (const SIM_Sim* sim);
void              sim_set_skeleton_change_period (SIM_Sim* sim, i64 period_ns); // 0 or less is never
void              sim_body_skeleton          (const SIM_Sim* sim, SIM_BodySkeleton& out_skeleton);
void              sim_body_joints            (const SIM_Sim* sim, const SIM_Sample& sample, SIM_BodyJoints& out_joints, b8 fingers = true); // without fingers the hands are open, XR_META_body_tracking_fidelity's low
b8                sim_body_joint_scalable    (SIM_BodyJoint joint); // not the root, the twists, nor the hand joints past the wrist
SIM_Limb          sim_body_joint_limb        (SIM_BodyJoint joint); // Count for the torso and head
const char*       sim_body_joint_name        (SIM_BodyJoint joint); // the extension's name without prefix and suffix, "left_arm_upper"; stable, used by recordings
b8                sim_body_joint_from_name   (const char* name, SIM_BodyJoint& out_joint);
const char*       sim_body_pose_name         (SIM_BodyPose pose);
b8                sim_body_pose_from_name    (const char* name, SIM_BodyPose& out_pose);
const char*       sim_limb_name              (SIM_Limb limb);
b8                sim_limb_from_name         (const char* name, SIM_Limb& out_limb);

// XR_META_virtual_keyboard: the key layout, its label atlas (SIM_VKB_TEXTURE_WIDTH x HEIGHT RGBA8)
// and its model: one part per key in layout order, each pressed by its animation, then "panel"
// and "collision". Key names are the lower character, or backspace, enter, shift, space.
void              sim_vkb_layout             (SIM_VkbLayout& out_layout);
void              sim_vkb_texture            (const SIM_VkbLayout& layout, b8 shifted, u8* out_rgba);
void              sim_vkb_model              (const SIM_VkbLayout& layout, SIM_RenderModel& out_model);
i32               sim_vkb_key_at             (const SIM_VkbLayout& layout, Vec2 point); // -1 for none
i32               sim_vkb_find_key           (const SIM_VkbLayout& layout, const char* name); // -1 for none
// Text for the app's virtual keyboard, typed at the next frame: printable ASCII, UTF-8, \b for
// backspace and \n for enter; false, and nothing typed, when it would not fit SIM_MAX_TYPED
b8                sim_type_text              (SIM_Sim* sim, const char* text);
const char*       sim_typed_text             (const SIM_Sim* sim, u32& out_length); // typed this frame, not terminated

// Props (docs/props.md): poses follow their motion from `start`, so a prop moves without being
// updated each frame. Changes are part of the frame state and recorded.
u32               sim_add_prop               (SIM_Sim* sim, const SIM_Prop& prop); // its id, 0 when invalid or full; its motion starts now
b8                sim_update_prop            (SIM_Sim* sim, u32 id, const SIM_Prop& prop); // keeps the id, restarts the motion
b8                sim_remove_prop            (SIM_Sim* sim, u32 id);
u32               sim_props                  (const SIM_Sim* sim, const SIM_Prop*& out_props); // in the order they were added
Pose              sim_prop_pose              (const SIM_Prop& prop, i64 time); // stage space
Vec3              sim_prop_extent            (const SIM_Prop& prop); // bounding box size in its own space
b8                sim_prop_from_json         (const JsonValue& value, SIM_Prop& inout_prop); // applies the fields of a prop object (docs/props.md); unchanged and false when the result is invalid
const char*       sim_prop_kind_name         (SIM_PropKind kind);     // "keyboard", "qr"; stable, used by recordings and the control API
b8                sim_prop_kind_from_name    (const char* name, SIM_PropKind& out_kind);
const char*       sim_prop_motion_name       (SIM_PropMotion motion); // "still", "linear", "circle"
b8                sim_prop_motion_from_name  (const char* name, SIM_PropMotion& out_motion);

SIM_InputValue    sim_get_input              (const SIM_Sim* sim, SIM_Device device, SIM_Input input);
void              sim_set_input              (SIM_Sim* sim, SIM_Device device, SIM_Input input, SIM_InputValue value); // ignored if the controller or tracked hand lacks the input
const char*       sim_input_name             (SIM_Input input);
SIM_InputType     sim_input_type             (SIM_Input input);
b8                sim_input_from_name        (const char* name, SIM_Input& out_input);

void              sim_apply_haptic           (SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output, f32 amplitude, i64 duration_ns, f32 frequency);
// XR_FB_haptic_amplitude_envelope: amplitudes spread evenly over the duration, resampled to at most SIM_MAX_HAPTIC_SAMPLES
void              sim_apply_haptic_envelope  (SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output, const f32* amplitudes, u32 count, i64 duration_ns);
// XR_FB_haptic_pcm: samples resampled to SIM_HAPTIC_PCM_RATE, played from now or appended after
// the ones not played yet; returns how many of `samples` were taken, fewer when the buffer is full
u32               sim_apply_haptic_pcm       (SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output, const f32* samples, u32 count, f32 sample_rate, b8 append);
// XR_EXT_haptic_parametric; validated by the caller, Intermediate and Last frames only on a running stream
void              sim_apply_haptic_parametric (SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output, const SIM_HapticParametric& parametric);
b8                sim_haptic_stream_running  (const SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output);
// false when no parametric stream runs on the output
b8                sim_haptic_stream_end      (const SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output, SIM_HapticStreamEnd& out_end);
void              sim_stop_haptic            (SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output);
SIM_Haptic        sim_get_haptic             (const SIM_Sim* sim, SIM_Device device, SIM_HapticOutput output);

//
// Spaces and views
//

Pose              sim_get_reference_space_pose (const SIM_Sim* sim, SIM_ReferenceSpace space); // in stage space, as tracking measures it: without the drift
void              sim_recenter               (SIM_Sim* sim);
void              sim_local_origin_change    (const SIM_Sim* sim, u64& out_generation, i64& out_change_time); // generation counts origin changes

// Tracking drift (docs/anchors.md): devices, props and the body are physical and kept in the
// simulator's frame, the stage as it lay at creation. The drift is where the tracked STAGE origin
// lies in that frame; STAGE, LOCAL, LOCAL_FLOOR and STATIONARY move with it, VIEW stays on the
// head. A yaw about +Y and a translation: false, and nothing changed, for a non-finite pose, a
// quaternion of length outside [0.5, 2] or one turning about another axis. The rate applies at
// every live frame with its clock step; a replay applies the recorded drift instead.
#define SIM_DRIFT_MAX_TILT 1e-4f // x and z of the quaternion against its length: 0.01 degrees, rounding of a yaw made elsewhere
b8                sim_set_tracking_drift      (SIM_Sim* sim, const Pose& drift);
Pose              sim_get_tracking_drift      (const SIM_Sim* sim);
b8                sim_tracking_drift_valid    (const Pose& drift); // what sim_set_tracking_drift accepts
b8                sim_set_tracking_drift_rate (SIM_Sim* sim, Vec3 velocity, f32 yaw_degrees_per_second); // false for non-finite values
void              sim_get_tracking_drift_rate (const SIM_Sim* sim, Vec3& out_velocity, f32& out_yaw_degrees_per_second);

// Poses and velocities are interpolated between samples and extrapolated past the live state;
// reference space origins hold until the next sample so a recenter never blends
void              sim_sample                 (const SIM_Sim* sim, i64 time, SIM_Sample& out_sample);
Pose              sim_sample_reference_space_pose (const SIM_Sample& sample, SIM_ReferenceSpace space); // in the simulator's frame, with the drift
Pose              sim_sample_hand_pose       (const SIM_Sim* sim, const SIM_Sample& sample, SIM_Device hand, SIM_HandPose kind);
void              sim_sample_view            (const SIM_Sim* sim, const SIM_Sample& sample, u32 view, Pose& out_pose, Fov& out_fov);
Vec2              sim_get_stage_bounds       (const SIM_Sim* sim);
u32               sim_view_count             (const SIM_Sim* sim);
void              sim_get_view               (const SIM_Sim* sim, u32 view, Pose& out_pose, Fov& out_fov); // pose in stage space

//
// Controls
//

void              sim_control_key            (SIM_Sim* sim, OS_Key key, b8 down);
void              sim_control_mouse_motion   (SIM_Sim* sim, f32 dx, f32 dy);
void              sim_control_wheel          (SIM_Sim* sim, f32 notches); // positive away from the user
void              sim_set_controls_enabled   (SIM_Sim* sim, b8 enabled);
b8                sim_controls_enabled       (const SIM_Sim* sim);
u32               sim_get_profile            (const SIM_Sim* sim); // index into the config's profiles
void              sim_set_profile            (SIM_Sim* sim, u32 index);
b8                sim_profile_from_name      (const SIM_Sim* sim, const char* name, u32& out_index);
SIM_Device        sim_profile_device         (const SIM_Profile& profile); // what the mouse turns: a hand, else the head
const char*       sim_drive_name             (SIM_Drive drive); // "none", "walk", "move_left", ...; stable, used in config files
b8                sim_drive_from_name        (const char* name, SIM_Drive& out_drive);
void              sim_reset_hands            (SIM_Sim* sim); // hands back to their configured offset from the head
void              sim_reset_poses            (SIM_Sim* sim); // head and hands back to their configured start poses

// Hash of everything an app observes from the simulator: time, frame index, local origin,
// device poses, velocities, active flags and inputs. Haptics are app output and not included.
u64               sim_state_hash             (const SIM_Sim* sim);
