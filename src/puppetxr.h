#ifndef PUPPETXR_H
#define PUPPETXR_H

/* C89: no nullptr, and (void) is required. NOLINTBEGIN(modernize-redundant-void-arg,modernize-use-nullptr) */

/*
 * puppetxr control API. C89, no dependencies. The runtime is loaded by the OpenXR loader, so an
 * app or test finds it with puppetxr_load_api() once its XrInstance exists. Every call is
 * thread safe and takes effect at the next xrWaitFrame.
 *
 * Compatibility (binding from puppetxr 1.0 on):
 * - PUPPETXR_API_VERSION only grows. Functions are only appended to PUPPETXR_Api, fields only to
 *   PUPPETXR_Status, values only to enums; nothing is removed, reordered or changes meaning.
 * - puppetxr_get_api fills the lower of the requested and the runtime's version and stores it in
 *   PUPPETXR_Api.version. Check it before calling functions newer than version 1.
 * - PUPPETXR_Status starts with struct_size, set by the caller; the runtime writes only that much.
 * - Input, device and controller names are part of the API and never change.
 */

/* Fixed-width types without requiring C99's stdint.h. C89 has no 64-bit integer, so the
   compiler's own is used: every supported compiler has one. */
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
#include <stdint.h>
typedef uint32_t PUPPETXR_U32;
typedef int32_t  PUPPETXR_I32;
typedef uint64_t PUPPETXR_U64;
typedef int64_t  PUPPETXR_I64;
#elif defined(_MSC_VER)
typedef unsigned __int32 PUPPETXR_U32;
typedef __int32          PUPPETXR_I32;
typedef unsigned __int64 PUPPETXR_U64;
typedef __int64          PUPPETXR_I64;
#else
typedef unsigned int PUPPETXR_U32;
typedef int          PUPPETXR_I32;
#if defined(__GNUC__)
__extension__ typedef unsigned long long PUPPETXR_U64;
__extension__ typedef long long          PUPPETXR_I64;
#else
typedef unsigned long long PUPPETXR_U64;
typedef long long          PUPPETXR_I64;
#endif
#endif

/* C89 has no inline */
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)
#define PUPPETXR_INLINE static inline
#elif defined(_MSC_VER)
#define PUPPETXR_INLINE static __inline
#elif defined(__GNUC__)
#define PUPPETXR_INLINE static __inline__
#else
#define PUPPETXR_INLINE static
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PUPPETXR_API_VERSION 1

#define PUPPETXR_MAKE_VERSION(major, minor, patch) (((PUPPETXR_U32)(major) << 22) | ((PUPPETXR_U32)(minor) << 12) | (PUPPETXR_U32)(patch))
#define PUPPETXR_VERSION_GET_MAJOR(version)     ((PUPPETXR_U32)(version) >> 22)
#define PUPPETXR_VERSION_GET_MINOR(version)     (((PUPPETXR_U32)(version) >> 12) & 0x3FFu)
#define PUPPETXR_VERSION_GET_PATCH(version)     ((PUPPETXR_U32)(version) & 0xFFFu)

#if defined(PUPPETXR_BUILDING_LIBRARY)
#if defined(_WIN32)
#define PUPPETXR_API_DECL __declspec(dllexport)
#else
#define PUPPETXR_API_DECL __attribute__((visibility("default")))
#endif
#else
#define PUPPETXR_API_DECL
#endif

typedef enum PUPPETXR_Result
{
    PUPPETXR_SUCCESS             = 0,
    PUPPETXR_ERROR_NOT_LOADED    = -1, /* the runtime is not loaded in this process */
    PUPPETXR_ERROR_VERSION       = -2, /* the runtime is older than this header */
    PUPPETXR_ERROR_NO_INSTANCE   = -3, /* no XrInstance is alive */
    PUPPETXR_ERROR_INVALID_ARGUMENT = -4,
    PUPPETXR_ERROR_REPLAYING     = -5, /* a replay drives the simulator; stop it first */
    PUPPETXR_ERROR_IO            = -6,
    PUPPETXR_ERROR_NO_GRAPHICS   = -7, /* screenshots need a session with a graphics binding */
    PUPPETXR_ERROR_BUSY          = -8, /* not possible while recording or replaying */
    PUPPETXR_ERROR_NOT_AVAILABLE = -9, /* the app has nothing of the kind, such as a shown virtual keyboard */
    PUPPETXR_RESULT_MAX_ENUM     = 0x7fffffff
} PUPPETXR_Result;

typedef enum PUPPETXR_Device
{
    PUPPETXR_DEVICE_HEAD    = 0,
    PUPPETXR_DEVICE_LEFT_HAND = 1,
    PUPPETXR_DEVICE_RIGHT_HAND = 2,
    PUPPETXR_DEVICE_MAX_ENUM = 0x7fffffff
} PUPPETXR_Device;

typedef enum PUPPETXR_Controller
{
    PUPPETXR_CONTROLLER_OCULUS_TOUCH = 0,
    PUPPETXR_CONTROLLER_KHR_SIMPLE   = 1,
    PUPPETXR_CONTROLLER_META_TOUCH_PRO = 2,
    PUPPETXR_CONTROLLER_META_TOUCH_PLUS = 3, /* the default */
    PUPPETXR_CONTROLLER_MAX_ENUM     = 0x7fffffff
} PUPPETXR_Controller;

/* What tracks a hand: its controller, or hand tracking without one. A tracked hand pinches with
   "trigger/value", grabs with "squeeze/value" and makes the menu gesture with "menu/click"; it has
   no other inputs. */
typedef enum PUPPETXR_HandSource
{
    PUPPETXR_HAND_SOURCE_CONTROLLER = 0,
    PUPPETXR_HAND_SOURCE_HAND    = 1,
    PUPPETXR_HAND_SOURCE_MAX_ENUM = 0x7fffffff
} PUPPETXR_HandSource;

/* What keyboard and mouse controls move: the control profiles "body", "head", "left" and
   "right" (get_profile, set_profile). */
typedef enum PUPPETXR_ControlMode
{
    PUPPETXR_CONTROL_MODE_BODY    = 0, /* head and both hands as one rigid body */
    PUPPETXR_CONTROL_MODE_HEAD    = 1,
    PUPPETXR_CONTROL_MODE_LEFT_HAND = 2,
    PUPPETXR_CONTROL_MODE_RIGHT_HAND = 3,
    PUPPETXR_CONTROL_MODE_MAX_ENUM = 0x7fffffff
} PUPPETXR_ControlMode;

typedef enum PUPPETXR_MirrorView
{
    PUPPETXR_MIRROR_LEFT = 0,
    PUPPETXR_MIRROR_RIGHT = 1,
    PUPPETXR_MIRROR_BOTH = 2,
    PUPPETXR_MIRROR_MAX_ENUM = 0x7fffffff
} PUPPETXR_MirrorView;

typedef enum PUPPETXR_LogLevel
{
    PUPPETXR_LOG_DEBUG = 0,
    PUPPETXR_LOG_INFO = 1,
    PUPPETXR_LOG_WARNING = 2,
    PUPPETXR_LOG_ERROR = 3,
    PUPPETXR_LOG_LEVEL_MAX_ENUM = 0x7fffffff
} PUPPETXR_LogLevel;

/* Anchor requests the simulator can make fail. */
typedef enum PUPPETXR_AnchorOp
{
    PUPPETXR_ANCHOR_OP_SAVE = 0, /* xrSaveSpaceFB, xrSaveSpaceListFB, xrSaveSpacesMETA */
    PUPPETXR_ANCHOR_OP_ERASE = 1, /* xrEraseSpaceFB, xrEraseSpacesMETA */
    PUPPETXR_ANCHOR_OP_LOAD = 2, /* xrQuerySpacesFB, xrDiscoverSpacesMETA */
    PUPPETXR_ANCHOR_OP_SHARE = 3, /* xrShareSpacesFB, xrShareSpacesMETA */
    PUPPETXR_ANCHOR_OP_MAX_ENUM = 0x7fffffff
} PUPPETXR_AnchorOp;

/* Frame calls set_frame_fault can make fail, docs/openxr.md "Frame faults" */
typedef enum PUPPETXR_FrameOp
{
    PUPPETXR_FRAME_OP_WAIT      = 0, /* xrWaitFrame */
    PUPPETXR_FRAME_OP_BEGIN     = 1, /* xrBeginFrame */
    PUPPETXR_FRAME_OP_LOCATE_VIEWS = 2, /* xrLocateViews */
    PUPPETXR_FRAME_OP_END       = 3, /* xrEndFrame */
    PUPPETXR_FRAME_OP_MAX_ENUM = 0x7fffffff
} PUPPETXR_FrameOp;

/* A runtime log message, without a trailing newline. Called on the thread that logged, possibly
   while the runtime holds its lock: return quickly and do not call into OpenXR or puppetxr. Lines
   the callback logs itself through puppetxr are kept but not passed to it again. */
typedef void (*PUPPETXR_LogCallback)(PUPPETXR_LogLevel level, const char* message, void* user);

/* Stage space, meters; orientation is a unit quaternion x, y, z, w. Hands use the grip pose. */
typedef struct PUPPETXR_Pose
{
    float position[3];
    float orientation[4];
} PUPPETXR_Pose;

/* A stored anchor of the running app and simulated user. */
typedef struct PUPPETXR_Anchor
{
    char       uuid[33];  /* 32 lowercase hex digits */
    char       room[64];  /* the room it was saved in, "" for none */
    PUPPETXR_Pose pose;   /* in room space, not stage space */
    PUPPETXR_I32 cloud;   /* 1 when in the simulated cloud, 0 when in the local store */
    PUPPETXR_U64 owner;   /* user id that uploaded it, cloud anchors only */
} PUPPETXR_Anchor;

/* Props, docs/props.md. */
typedef enum PUPPETXR_PropKind
{
    PUPPETXR_PROP_KEYBOARD   = 0, /* +Y up, its front at +Z, origin at its bounding box centre */
    PUPPETXR_PROP_QR_CODE    = 1, /* centred in its XY plane, facing +Z */
    PUPPETXR_PROP_KIND_MAX_ENUM = 0x7fffffff
} PUPPETXR_PropKind;

typedef enum PUPPETXR_PropMotion
{
    PUPPETXR_PROP_STILL        = 0,
    PUPPETXR_PROP_LINEAR       = 1, /* back and forth between pose and target */
    PUPPETXR_PROP_CIRCLE       = 2, /* horizontal circle around pose, counter-clockwise from above, turning with the path */
    PUPPETXR_PROP_MOTION_MAX_ENUM = 0x7fffffff
} PUPPETXR_PropMotion;

/* A body tracking joint: XR_META_body_tracking_full_body's joints in their order */
typedef struct PUPPETXR_BodyJoint
{
    PUPPETXR_Pose pose;    /* stage space, in Meta's joint axes (docs/body-tracking.md) */
    PUPPETXR_I32 tracked;
    float      confidence;
} PUPPETXR_BodyJoint;

typedef struct PUPPETXR_Prop
{
    PUPPETXR_U32     struct_size; /* sizeof(PUPPETXR_Prop); for get_props in the first element */
    PUPPETXR_U32     id;          /* set by get_props, ignored by add_prop and update_prop */
    PUPPETXR_PropKind kind;
    PUPPETXR_PropMotion motion;
    PUPPETXR_Pose    pose;        /* stage space: where it is, where its line starts, or the centre it circles */
    float            target[3];   /* linear: the far end */
    float            radius;      /* circle */
    float            speed;       /* meters per second along the path */
    float            size;        /* QR code edge length, 0.01 to 10 m */
    char             text[214];   /* QR code text, up to 213 bytes */
    PUPPETXR_Pose    current;     /* get_props: where it is now */
} PUPPETXR_Prop;

/* The app's XR_META_virtual_keyboard. */
typedef struct PUPPETXR_VirtualKeyboard
{
    PUPPETXR_U32 struct_size; /* set to sizeof(PUPPETXR_VirtualKeyboard) before calling get_virtual_keyboard */
    PUPPETXR_I32 exists;     /* the app created one */
    PUPPETXR_I32 visible;
    PUPPETXR_I32 shifted;    /* the next character is upper case */
    PUPPETXR_Pose pose;      /* stage space; the keys face its +Z */
    float      scale;
    char       context[256]; /* the end of the app's text before the cursor */
} PUPPETXR_VirtualKeyboard;

typedef struct PUPPETXR_Status
{
    PUPPETXR_U32 struct_size;   /* set to sizeof(PUPPETXR_Status) before calling get_status */
    PUPPETXR_U64 frame_index;
    PUPPETXR_I64 time_ns;        /* simulated time, the XrTime of the last waited frame */
    PUPPETXR_U64 state_hash;    /* changes whenever anything an app can observe changes */
    PUPPETXR_I32 recording;
    PUPPETXR_U64 recorded_frames;
    PUPPETXR_I32 replaying;
    PUPPETXR_I32 replay_finished;
    PUPPETXR_U64 replay_frame;
    PUPPETXR_U64 replay_total;
    PUPPETXR_U32 divergences;   /* replayed frames whose state differed from the recording */
    PUPPETXR_U64 first_divergence;
    PUPPETXR_I32 controller;     /* PUPPETXR_Controller */
    PUPPETXR_I32 control_mode;   /* PUPPETXR_ControlMode, -1 in a profile of another name */
    char       profile[32];      /* the control profile's name */
} PUPPETXR_Status;

/* Input names are the OpenXR component paths below /user/hand/<side>/input/, for example
   "trigger/value", "a/click", "thumbstick". Bool inputs take 0 or 1 in x, floats x in [0, 1],
   vec2 inputs x and y in [-1, 1]; values are clamped. */
typedef struct PUPPETXR_Api
{
    PUPPETXR_U32 version;       /* the API version the runtime filled in */
    PUPPETXR_U32 runtime_version; /* PUPPETXR_MAKE_VERSION of the loaded runtime */

    PUPPETXR_Result (*get_status)        (PUPPETXR_Status* out_status);
    PUPPETXR_Result (*get_device_pose)   (PUPPETXR_Device device, PUPPETXR_Pose* out_pose);
    PUPPETXR_Result (*set_device_pose)   (PUPPETXR_Device device, const PUPPETXR_Pose* pose);
    PUPPETXR_Result (*set_device_active) (PUPPETXR_Device device, PUPPETXR_I32 active);
    PUPPETXR_Result (*get_input)         (PUPPETXR_Device device, const char* name, float* out_x, float* out_y);
    PUPPETXR_Result (*set_input)         (PUPPETXR_Device device, const char* name, float x, float y);
    PUPPETXR_Result (*set_fixed_step)    (PUPPETXR_I32 enabled);
    PUPPETXR_Result (*set_controls_enabled) (PUPPETXR_I32 enabled);
    PUPPETXR_Result (*recenter)          (void);
    PUPPETXR_Result (*set_mirror_view)   (PUPPETXR_MirrorView view);
    PUPPETXR_Result (*screenshot)        (const char* path); /* PNG of the last composited frame at native eye resolution; PUPPETXR_ERROR_NOT_AVAILABLE before the first frame with the control server on */
    PUPPETXR_Result (*record_start)      (const char* path);
    PUPPETXR_Result (*record_stop)       (void);
    PUPPETXR_Result (*replay_start)      (const char* path);
    PUPPETXR_Result (*replay_stop)       (void);
    PUPPETXR_Result (*request_exit)      (void);       /* the session goes to STOPPING at the next frame */
    PUPPETXR_Result (*get_controller)    (PUPPETXR_Controller* out_controller);
    PUPPETXR_Result (*set_controller)    (PUPPETXR_Controller controller); /* PUPPETXR_ERROR_BUSY while recording or replaying */

    /* Rooms are directories of the rooms dir (config "rooms_dir"); names are [a-z0-9_-]. */
    PUPPETXR_Result (*set_room)          (const char* name); /* null or "" for none; PUPPETXR_ERROR_BUSY while recording or replaying */
    PUPPETXR_Result (*get_room)          (char* out_name, PUPPETXR_U32 size); /* "" when no room is loaded */

    /* Logging. Works without an instance. A non-zero replay_history first passes the lines the
       runtime kept, oldest first, such as those of xrCreateInstance. Null removes the callback. */
    PUPPETXR_Result (*set_log_callback)  (PUPPETXR_LogCallback callback, void* user, PUPPETXR_I32 replay_history);

    /* Keyboard and mouse controls exactly as a person drives them, for agents and
       scripts; key names as in docs/config.md. Keys act at the next frame, so hold one for frames
       to move or ramp. All return PUPPETXR_ERROR_REPLAYING during a replay. */
    PUPPETXR_Result (*get_control_mode)  (PUPPETXR_ControlMode* out_mode); /* PUPPETXR_ERROR_NOT_AVAILABLE in a profile of another name */
    PUPPETXR_Result (*set_control_mode)  (PUPPETXR_ControlMode mode);
    PUPPETXR_Result (*reset_poses)       (void); /* head and hands to their configured start poses, without velocity */
    PUPPETXR_Result (*reset_hands)       (void); /* hands to their configured offset from the head */
    PUPPETXR_Result (*control_key)       (const char* key, PUPPETXR_I32 down);
    PUPPETXR_Result (*control_mouse)     (float dx, float dy); /* pixels, as relative mouse motion */

    /* Anchors, docs/anchors.md. */
    PUPPETXR_Result (*set_disabled_extensions) (const char* names); /* comma separated, on top of the config; works without an instance and applies from the next xrCreateInstance; null or "" for none */
    PUPPETXR_Result (*get_anchors)       (PUPPETXR_Anchor* out_anchors, PUPPETXR_U32 capacity, PUPPETXR_U32* out_count); /* local store first, then visible cloud anchors; out_count is the total */
    PUPPETXR_Result (*wipe_anchors)      (PUPPETXR_I32 include_cloud); /* PUPPETXR_ERROR_BUSY while recording or replaying */
    PUPPETXR_Result (*set_anchor_fault)  (PUPPETXR_AnchorOp op, PUPPETXR_I32 xr_result, PUPPETXR_U32 count); /* the next count requests complete with xr_result, a negative XrResult; count 0 clears */
    PUPPETXR_Result (*set_anchor_timing) (PUPPETXR_U32 latency_frames, PUPPETXR_U32 localize_frames);
    PUPPETXR_Result (*get_origin_offset) (PUPPETXR_Pose* out_room_from_stage); /* where the stage origin lay in the room at instance creation; with tracking drift it lies at this times the drift */

    /* Hands, docs/openxr.md "Hand tracking". Recorded per frame, so it may change while recording. */
    PUPPETXR_Result (*get_hand_source)   (PUPPETXR_Device hand, PUPPETXR_HandSource* out_source);
    PUPPETXR_Result (*set_hand_source)   (PUPPETXR_Device hand, PUPPETXR_HandSource source); /* zeroes the inputs the new source lacks */

    /* Virtual keyboard, docs/openxr.md "Virtual keyboard". Key names are a key's
       lower case character, or backspace, enter, shift, space. */
    PUPPETXR_Result (*get_virtual_keyboard)  (PUPPETXR_VirtualKeyboard* out_keyboard);
    PUPPETXR_Result (*get_virtual_keyboard_key) (const char* name, PUPPETXR_Pose* out_pose); /* the key top's centre in stage space; PUPPETXR_ERROR_NOT_AVAILABLE without a keyboard */
    /* Types into the shown keyboard at the next frame: UTF-8, \b is backspace, \n enter, at most
       256 bytes a frame. Recorded per frame. PUPPETXR_ERROR_NOT_AVAILABLE unless the keyboard is shown. */
    PUPPETXR_Result (*type_text)             (const char* text);

    /* Props, docs/props.md: keyboards and QR codes the headset tracks, at most 16.
       Changes are recorded per frame; PUPPETXR_ERROR_REPLAYING during a replay. Invalid props, a
       full list and unknown ids give PUPPETXR_ERROR_INVALID_ARGUMENT. A change restarts the motion. */
    PUPPETXR_Result (*add_prop)              (const PUPPETXR_Prop* prop, PUPPETXR_U32* out_id);
    PUPPETXR_Result (*update_prop)           (PUPPETXR_U32 id, const PUPPETXR_Prop* prop);
    PUPPETXR_Result (*remove_prop)           (PUPPETXR_U32 id);
    /* out_count is the total; at most capacity are written, each out_props[0].struct_size apart */
    PUPPETXR_Result (*get_props)             (PUPPETXR_Prop* out_props, PUPPETXR_U32 capacity, PUPPETXR_U32* out_count);

    /* Moves a hand so a ray starts at the eye of view (0 left, 1 right) and passes
       through (u, v) of that view's image, 0 0 top left and 1 1 bottom right: tests pick a UI
       element from a screenshot without knowing how far away it is. The ray is a pose in grip
       space looking down its -Z; null is the aim pose. Engines that derive their pointer from
       the grip need their own, such as Meta's OVRPlugin the grip turned 60 degrees down. */
    PUPPETXR_Result (*point_hand)            (PUPPETXR_Device hand, PUPPETXR_U32 view, float u, float v, const PUPPETXR_Pose* ray);

    /* Control profiles, docs/config.md "Profiles": what keys and mouse drive. Names
       are the built-in ones (body, stick, hands, head, left, right) and the config's. */
    PUPPETXR_Result (*get_profile)           (char* out_name, PUPPETXR_U32 size);
    PUPPETXR_Result (*set_profile)           (const char* name); /* PUPPETXR_ERROR_INVALID_ARGUMENT for an unknown name */

    /* A zone profile capture into a Chrome trace at `path` (docs/profiling.md);
       needs no instance. PUPPETXR_ERROR_BUSY while one runs, PUPPETXR_ERROR_IO when the file cannot
       be made; stop writes it and is PUPPETXR_ERROR_NOT_AVAILABLE without a capture. */
    PUPPETXR_Result (*profiler_start)        (const char* path);
    PUPPETXR_Result (*profiler_stop)         (void);

    /* Body tracking, docs/body-tracking.md. The body is JSON: {"height": metres,
       "pose": "stand"|"t_pose"|"a_pose"|"arms_forward"|"sit", "bones": {joint: scale},
       "joints": {joint: {"rotation": [x, y, z, w], "tracked": bool, "confidence": c}},
       "limbs": {"left_arm"|"right_arm"|"left_leg"|"right_leg": "follow"|"posed"}}, every
       member optional. Joint names are the extension's without prefix and suffix, lower case
       ("left_arm_upper"). Edits are recorded; PUPPETXR_ERROR_REPLAYING during a replay. */
    PUPPETXR_Result (*get_body_joints)       (PUPPETXR_BodyJoint* out_joints, PUPPETXR_U32 capacity, PUPPETXR_U32* out_count); /* at the predicted display time; out_count is the total, 84 */
    PUPPETXR_Result (*get_body)              (char* out_json, PUPPETXR_U32 size, PUPPETXR_U32* out_length); /* what differs from standing; PUPPETXR_ERROR_INVALID_ARGUMENT with the length it needs when size is too small */
    PUPPETXR_Result (*set_body)              (const char* json); /* merges onto the body, all or nothing */
    PUPPETXR_Result (*reset_body)            (void); /* to the configured body */

    /* Face and eye tracking, docs/face-tracking.md. The face is JSON: {"preset":
       "neutral"|"smile"|"frown"|"surprise"|"eyes_closed"|"jaw_open", "weights": {expression: w},
       "visemes": {viseme: w}, "confidences": [lower, upper], "blink": bool, "gaze": [yaw, pitch]
       degrees from the head, "look_at": [x, y, z] in stage space or null}, every member
       optional. Expressions are XR_FB_face_tracking2's in lower case ("jaw_drop"), visemes
       sil, pp, ff, th, dd, kk, ch, ss, nn, rr, aa, e, ih, oh, ou. Edits are recorded;
       PUPPETXR_ERROR_REPLAYING during a replay. */
    PUPPETXR_Result (*get_face_weights)      (float* out_weights, PUPPETXR_U32 capacity, PUPPETXR_U32* out_count); /* the 70 at the predicted display time, with the eyes' and the blinks' */
    PUPPETXR_Result (*get_eye_gazes)         (PUPPETXR_Pose* out_eyes); /* two: left, right, in stage space, -Z along the gaze */
    PUPPETXR_Result (*get_face)              (char* out_json, PUPPETXR_U32 size, PUPPETXR_U32* out_length); /* what differs from neutral; as get_body when size is too small */
    PUPPETXR_Result (*set_face)              (const char* json); /* merges onto the face, all or nothing */
    PUPPETXR_Result (*reset_face)            (void); /* to the configured face */

    /* Tracking drift, docs/anchors.md "Tracking drift". The drift is where the
       tracked STAGE origin lies in the stage as it was at instance creation, the space of every
       other pose of this API: a yaw about +Y and a translation in metres. STAGE, LOCAL,
       LOCAL_FLOOR and STATIONARY move with it; the head, hands, props, body, room and anchors stay
       where they are, so the app sees them move by its inverse, with no reference space event.
       A non-finite pose, a quaternion of length outside [0.5, 2] or a tilted one (x and z above
       1e-4 of its length) is PUPPETXR_ERROR_INVALID_ARGUMENT. Recorded per frame, so it may change
       while recording; PUPPETXR_ERROR_REPLAYING during a replay. */
    PUPPETXR_Result (*set_tracking_drift)    (const PUPPETXR_Pose* drift); /* from the next locate on; null for none */
    /* Added to the drift at every frame with its clock step: velocity x, y, z in m/s (null for
       none) in the same space and the yaw in degrees per second about the drifted origin. Zero
       stops it and keeps the drift reached. Non-finite values are PUPPETXR_ERROR_INVALID_ARGUMENT. */
    PUPPETXR_Result (*set_tracking_drift_rate) (const float* velocity, float yaw_degrees_per_second);
    PUPPETXR_Result (*get_tracking_drift)    (PUPPETXR_Pose* out_drift, float* out_velocity, float* out_yaw_degrees_per_second); /* any may be null; out_velocity takes 3 */

    /* Frame faults, docs/openxr.md "Frame faults": the next count calls of op that
       pass their own checks return xr_result without doing their work, so a failed xrWaitFrame
       steps no frame and a failed xrBeginFrame begins none; the session state stays. xr_result
       must be an error the OpenXR registry lists for that function, else
       PUPPETXR_ERROR_INVALID_ARGUMENT; count 0 clears. Works without an instance, cleared when the
       instance is destroyed; PUPPETXR_ERROR_BUSY while recording or replaying (a recording keeps the
       faults pending as it starts, and its replay fails the same calls). */
    PUPPETXR_Result (*set_frame_fault)       (PUPPETXR_FrameOp op, PUPPETXR_I32 xr_result, PUPPETXR_U32 count);
} PUPPETXR_Api;

/* Fills the lower of `version` and the runtime's PUPPETXR_API_VERSION; version 0 is invalid */
PUPPETXR_API_DECL PUPPETXR_Result puppetxr_get_api (PUPPETXR_U32 version, PUPPETXR_Api* out_api);
/* Works without an XrInstance; either pointer may be null */
PUPPETXR_API_DECL PUPPETXR_Result puppetxr_get_version (PUPPETXR_U32* out_runtime_version, PUPPETXR_U32* out_api_version);

#if !defined(PUPPETXR_NO_LOADER) && !defined(PUPPETXR_BUILDING_LIBRARY)

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(_WIN32)
#define PUPPETXR_LIBRARY_NAME "puppetxr.dll"
#elif defined(__APPLE__)
#define PUPPETXR_LIBRARY_NAME "libpuppetxr.dylib"
#else
#define PUPPETXR_LIBRARY_NAME "libpuppetxr.so"
#endif

/* Finds the runtime the OpenXR loader already loaded; it never loads it itself. */
PUPPETXR_INLINE PUPPETXR_Result puppetxr_load_api (PUPPETXR_Api* out_api)
{
    typedef PUPPETXR_Result (*PFN_puppetxr_get_api)(PUPPETXR_U32, PUPPETXR_Api*);
    PFN_puppetxr_get_api get_api = 0;
#if defined(_WIN32)
    HMODULE module = GetModuleHandleA(PUPPETXR_LIBRARY_NAME);
    if (module)
    {
        /* Through a generic function pointer, as FARPROC names no parameters */
        get_api = (PFN_puppetxr_get_api)(void (*)(void))GetProcAddress(module, "puppetxr_get_api");
    }
#else
    void* module = dlopen(PUPPETXR_LIBRARY_NAME, RTLD_NOW | RTLD_NOLOAD);
    if (module)
    {
        /* The POSIX idiom for dlsym; a plain cast from void* to a function pointer is not ISO C */
        *(void**)(&get_api) = dlsym(module, "puppetxr_get_api");
        dlclose(module);
    }
#endif
    if (!get_api)
    {
        return PUPPETXR_ERROR_NOT_LOADED;
    }
    return get_api(PUPPETXR_API_VERSION, out_api);
}

#endif

#ifdef __cplusplus
}
#endif

/* NOLINTEND(modernize-redundant-void-arg,modernize-use-nullptr) */

#endif
