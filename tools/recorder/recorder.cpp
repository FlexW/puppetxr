#include "recorder.h"
#include "recorder_shaders.h"

#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <volk.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define JSMN_STATIC
#define JSMN_STRICT
#include <jsmn.h>

typedef uint8_t  u8;
typedef int16_t  i16;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint64_t u64;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;
typedef bool     b8;
typedef size_t   usize;

#define RC_MAX_IMAGES      8
#define RC_MAX_BUTTONS     8
#define RC_MAX_TAKES       1024
#define RC_PANEL_WIDTH     640
#define RC_PANEL_HEIGHT    400
#define RC_PANEL_METRES    0.48f // width; the height keeps the pixels square
#define RC_PANEL_AHEAD     0.45f
#define RC_PANEL_BELOW     0.30f
#define RC_POKE_DEPTH      0.01f // a tip closer to the plane than this, in front or behind, presses
#define RC_POKE_BEHIND     0.03f
#define RC_POKE_RELEASE    0.02f // and must leave this far before it presses again
#define RC_GLYPH_W         5
#define RC_GLYPH_H         7
#define RC_STATUS_SIZE     128
#define RC_RAY_WIDTH       0.004f
#define RC_RAY_LENGTH      1.5f   // a ray that hits nothing
#define RC_RAY_PIXELS      4
#define RC_NO_BUTTON       0xff
#define RC_MAX_INSTANCES   128     // boxes of the skeleton: 26 joints and 24 bones a hand
#define RC_BONE_RADIUS     0.004f
#define RC_JOINT_RADIUS    0.006f
#define RC_NEAR            0.05f
#define RC_FAR             100.0f
#define RC_DEPTH_FORMAT    VK_FORMAT_D16_UNORM
#define RC_PINCH_CLOSE     0.015f // thumb and index tips closer than this pinch
#define RC_PINCH_OPEN      0.03f  // and farther than this release
#define RC_PROTOCOL_TOKENS 2048

//
// Types
//

namespace
{

struct V3
{
    f32 x, y, z;
};

struct Q
{
    f32 x, y, z, w;
};

struct Rect
{
    i32 x, y, w, h;
};

enum class Mode : u8
{
    Idle,
    Countdown,
    Recording,
};

enum class Action : u8
{
    Record,
    Redo,
    Previous,
    Next,
    Free,
    Recenter,
    Count,
};

struct Button
{
    Action      action;
    const char* label;
    Rect        rect;
};

struct Xr
{
#define RC_XR_FUNCTIONS(X)                     \
    X(xrDestroyInstance)                       \
    X(xrGetInstanceProperties)                 \
    X(xrGetSystem)                             \
    X(xrGetSystemProperties)                   \
    X(xrPollEvent)                             \
    X(xrCreateSession)                         \
    X(xrDestroySession)                        \
    X(xrBeginSession)                          \
    X(xrEndSession)                            \
    X(xrRequestExitSession)                    \
    X(xrEnumerateReferenceSpaces)              \
    X(xrCreateReferenceSpace)                  \
    X(xrDestroySpace)                          \
    X(xrLocateSpace)                           \
    X(xrEnumerateSwapchainFormats)             \
    X(xrCreateSwapchain)                       \
    X(xrDestroySwapchain)                      \
    X(xrEnumerateSwapchainImages)              \
    X(xrAcquireSwapchainImage)                 \
    X(xrWaitSwapchainImage)                    \
    X(xrReleaseSwapchainImage)                 \
    X(xrWaitFrame)                             \
    X(xrBeginFrame)                            \
    X(xrEndFrame)                              \
    X(xrLocateViews)                           \
    X(xrStringToPath)                          \
    X(xrCreateActionSet)                       \
    X(xrDestroyActionSet)                      \
    X(xrCreateAction)                          \
    X(xrSuggestInteractionProfileBindings)     \
    X(xrAttachSessionActionSets)               \
    X(xrSyncActions)                           \
    X(xrGetActionStateFloat)                   \
    X(xrEnumerateViewConfigurationViews)
#define RC_DECLARE(fn) PFN_##fn fn;
    RC_XR_FUNCTIONS(RC_DECLARE)
#undef RC_DECLARE
    PFN_xrGetVulkanGraphicsRequirements2KHR get_vulkan_requirements;
    PFN_xrCreateVulkanInstanceKHR           create_vulkan_instance;
    PFN_xrGetVulkanGraphicsDevice2KHR       get_vulkan_device;
    PFN_xrCreateVulkanDeviceKHR             create_vulkan_device;
    PFN_xrCreateHandTrackerEXT              create_hand_tracker;
    PFN_xrDestroyHandTrackerEXT             destroy_hand_tracker;
    PFN_xrLocateHandJointsEXT               locate_hand_joints;
    PFN_xrSetHandTrackingFrequencyHintMETA  set_hand_frequency;
    PFN_xrCreatePassthroughFB               create_passthrough;
    PFN_xrDestroyPassthroughFB              destroy_passthrough;
    PFN_xrCreatePassthroughLayerFB          create_passthrough_layer;
    PFN_xrDestroyPassthroughLayerFB         destroy_passthrough_layer;
};

struct Extensions
{
    b8 hand_tracking;
    b8 aim;
    b8 data_source;
    b8 motion_range;
    b8 frequency_hint;
    b8 passthrough;
    b8 local_floor;
    b8 hand_interaction;
};

struct Take
{
    FILE* file;
    u32   frames;
    i64   start_time;
    f32   seconds; // 0 until Stop
    char  name[RC_NAME_SIZE];
};

struct Instance
{
    f32 start_radius[4];
    f32 end_color[4]; // w: 0 left bone, 1 right bone, 2 joint (skeleton.vert)
};

// XR_EXT_hand_interaction's values, recorded next to the joints
enum class Interaction : u8
{
    Pinch,
    AimActivate,
    Grasp,
    Count,
};

struct UiView
{
    char title[RC_STATUS_SIZE];
    char instruction[RC_INSTRUCTION_SIZE];
    char status[RC_STATUS_SIZE];
    i16  cursors[2][2]; // panel pixels where each hand's ray hits it
    u8   cursor_on[2];
    u8   hovered[2];    // the button each ray is on, RC_NO_BUTTON for none
    u8   recording;
    u8   free_take;
};

} // namespace

struct RC_App
{
    RC_Options       options;
    PFN_xrGetInstanceProcAddr gipa;
    Xr               xr;
    Extensions       has;
    XrInstance       instance;
    XrSystemId       system;
    XrSession        session;
    XrSpace          base;
    XrSpace          view;
    XrSessionState   state;
    b8               running;
    b8               exit_requested;
    b8               failed;
    const char*      space_name;
    char             runtime_name[XR_MAX_RUNTIME_NAME_SIZE];
    char             system_name[XR_MAX_SYSTEM_NAME_SIZE];

    VkInstance       vk_instance;
    VkPhysicalDevice vk_physical;
    VkDevice         vk_device;
    VolkDeviceTable  vk;
    VkQueue          queue;
    u32              queue_family;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;
    VkBuffer         staging;
    VkDeviceMemory   staging_memory;
    void*            staging_mapped;

    XrSwapchain      panel_swapchain;
    VkImage          panel_images[RC_MAX_IMAGES];
    u32              panel_images_count;
    b8               panel_uploaded;
    b8               panel_placed;
    XrPosef          panel_pose;
    u32              pixels[RC_PANEL_WIDTH * RC_PANEL_HEIGHT];
    UiView           shown;
    Button           buttons[RC_MAX_BUTTONS];
    u32              buttons_count;
    b8               poke_armed[2];
    b8               was_pinching[2];
    b8               tips_pinching[2];
    XrSwapchain      ray_swapchain;
    VkImage          ray_images[RC_MAX_IMAGES];
    u32              ray_images_count;
    b8               ray_uploaded;
    b8               ray_shown[2];
    XrPosef          ray_poses[2];
    f32              ray_lengths[2];
    i16              cursors[2][2];
    b8               cursor_on[2];
    u8               hovered[2];

    u32              vulkan_errors;
    VkDebugUtilsMessengerEXT messenger;
    XrSwapchain      eye_swapchain;
    VkImage          eye_images[RC_MAX_IMAGES];
    VkImageView      eye_views[RC_MAX_IMAGES][2];
    VkFramebuffer    eye_framebuffers[RC_MAX_IMAGES][2];
    u32              eye_images_count;
    u32              eye_width;
    u32              eye_height;
    VkImage          depth_image;
    VkDeviceMemory   depth_memory;
    VkImageView      depth_view;
    VkRenderPass     render_pass;
    VkPipelineLayout pipeline_layout;
    VkPipeline       pipeline;
    VkBuffer         instance_buffer;
    VkDeviceMemory   instance_memory;
    Instance*        instances;
    u32              instances_count;

    XrActionSet      action_set;
    XrAction         interactions[(u32)Interaction::Count];
    XrPath           hand_paths[2];
    b8               actions_synced;

    XrPassthroughFB      passthrough;
    XrPassthroughLayerFB passthrough_layer;
    XrHandTrackerEXT     trackers[2];

    RC_Step          steps[RC_MAX_STEPS];
    u32              steps_count;
    u32              step;
    Mode             mode;
    b8               free_take;
    i64              countdown_end;
    Take             take;
    char             status[RC_STATUS_SIZE];
    char             take_paths[RC_MAX_TAKES][RC_PATH_SIZE];
    u32              takes_count;
    i64              display_period;
};

//
// Statics
//

// Each glyph is 7 rows of 5 pixels, the leftmost first; letters are upper case only
static const struct
{
    char        c;
    const char* rows;
} rc_glyphs[] = {
    { 'A', "01110100011000111111100011000110001" },
    { 'B', "11110100011000111110100011000111110" },
    { 'C', "01110100011000010000100001000101110" },
    { 'D', "11110100011000110001100011000111110" },
    { 'E', "11111100001000011110100001000011111" },
    { 'F', "11111100001000011110100001000010000" },
    { 'G', "01110100011000010111100011000101111" },
    { 'H', "10001100011000111111100011000110001" },
    { 'I', "01110001000010000100001000010001110" },
    { 'J', "00111000100001000010000101001001100" },
    { 'K', "10001100101010011000101001001010001" },
    { 'L', "10000100001000010000100001000011111" },
    { 'M', "10001110111010110101100011000110001" },
    { 'N', "10001100011100110101100111000110001" },
    { 'O', "01110100011000110001100011000101110" },
    { 'P', "11110100011000111110100001000010000" },
    { 'Q', "01110100011000110001101011001001101" },
    { 'R', "11110100011000111110101001001010001" },
    { 'S', "01111100001000001110000010000111110" },
    { 'T', "11111001000010000100001000010000100" },
    { 'U', "10001100011000110001100011000101110" },
    { 'V', "10001100011000110001100010101000100" },
    { 'W', "10001100011000110101101011010101010" },
    { 'X', "10001100010101000100010101000110001" },
    { 'Y', "10001100010101000100001000010000100" },
    { 'Z', "11111000010001000100010001000011111" },
    { '0', "01110100011001110101110011000101110" },
    { '1', "00100011000010000100001000010001110" },
    { '2', "01110100010000100010001000100011111" },
    { '3', "11111000100010000010000011000101110" },
    { '4', "00010001100101010010111110001000010" },
    { '5', "11111100001111000001000011000101110" },
    { '6', "00110010001000011110100011000101110" },
    { '7', "11111000010001000100010000100001000" },
    { '8', "01110100011000101110100011000101110" },
    { '9', "01110100011000101111000010001001100" },
    { '.', "00000000000000000000000000110001100" },
    { ',', "00000000000000000000011000010001000" },
    { ':', "00000011000110000000011000110000000" },
    { ';', "00000011000110000000011000010001000" },
    { '/', "00000000010001000100010001000000000" },
    { '-', "00000000000000011111000000000000000" },
    { '_', "00000000000000000000000000000011111" },
    { '(', "00010001000100001000010000010000010" },
    { ')', "01000001000001000010000100010001000" },
    { '!', "00100001000010000100001000000000100" },
    { '?', "01110100010000100010001000000000100" },
    { '\'', "00100001000100000000000000000000000" },
    { '%', "11000110010001000100010001001100011" },
    { '+', "00000001000010011111001000010000000" },
    { '=', "00000000001111100000111110000000000" },
    { '<', "00010001000100010000010000010000010" },
    { '>', "01000001000001000001000100010001000" },
};

// XrHandJointEXT parents; the palm and the wrist have none
static const u8 rc_joint_parents[XR_HAND_JOINT_COUNT_EXT] = {
    0xff, 0xff,
    1, 2, 3, 4,
    1, 6, 7, 8, 9,
    1, 11, 12, 13, 14,
    1, 16, 17, 18, 19,
    1, 21, 22, 23, 24,
};

static const char* const rc_action_labels[(u32)Action::Count] = { "RECORD", "REDO", "PREV", "NEXT", "FREE TAKE", "RECENTER" };

#define RC_STEP(name, seconds, hands, instruction) { name, instruction, seconds, RC_Hands::hands }

// Calibration first (docs/recorder.md, Protocol), then the motions of the built-in gestures
// (docs/hand-motion.md, Library)
static const RC_Step rc_builtin_steps[] = {
    RC_STEP("still_open",        10.0f, Both,  "Hold both hands open in front of you, fingers apart, as still as you can."),
    RC_STEP("still_relaxed",     10.0f, Both,  "Let both hands hang relaxed in front of you, as you would while waiting."),
    RC_STEP("curl_each",         20.0f, Both,  "Slowly curl each finger alone into the palm and open it again: index, middle, ring, little, thumb."),
    RC_STEP("thumb_sweep",       10.0f, Both,  "Move both thumbs through their whole range: wide circles, then touch the base of the little finger."),
    RC_STEP("pinch_each",        12.0f, Both,  "Touch the thumb to each fingertip in turn, index to little and back, twice."),
    RC_STEP("spread",            8.0f,  Both,  "Spread the fingers wide, then close them together, five times."),
    RC_STEP("fist",              8.0f,  Both,  "Make a fist and open the hand, five times, slow then fast."),
    RC_STEP("fast_swipe",        8.0f,  Right, "Swipe the right hand fast from side to side in front of you."),
    RC_STEP("cross",             8.0f,  Both,  "Move one hand over the other and back, palms down, then palms facing."),
    RC_STEP("out_of_view",       10.0f, Right, "Lower the right hand to your side, out of view, wait, raise it again. Twice."),
    RC_STEP("pinch_tap_right",   12.0f, Right, "Reach to different places and pinch index and thumb briefly, five times, slow then fast."),
    RC_STEP("pinch_hold_right",  10.0f, Right, "Pinch and hold for two seconds, release. Three times."),
    RC_STEP("double_pinch_right", 8.0f, Right, "Pinch twice quickly, three times."),
    RC_STEP("middle_pinch_right", 8.0f, Right, "Pinch thumb and middle finger briefly, five times."),
    RC_STEP("poke_right",        12.0f, Right, "Press buttons in the air with the index finger: five points at arm's length."),
    RC_STEP("pinch_drag_right",  12.0f, Right, "Pinch, drag a window to a new place, release. Three times in different directions."),
    RC_STEP("pinch_scroll_right", 10.0f, Right, "Pinch and scroll a list up and down, three times."),
    RC_STEP("swipe_right",       10.0f, Right, "Swipe with the open hand: left, right, up, down, twice each."),
    RC_STEP("grab_ball_right",   10.0f, Right, "Grab an imagined ball the size of an orange, move it, let go. Three times."),
    RC_STEP("grab_handle_right", 10.0f, Right, "Grab an imagined handle like a broom stick, move it, let go. Three times."),
    RC_STEP("throw_overhand",    12.0f, Right, "Throw an imagined ball overhand at a target three metres ahead. Three times."),
    RC_STEP("throw_underhand",   12.0f, Right, "Throw an imagined ball underhand at a target two metres ahead. Three times."),
    RC_STEP("pull_lever",        10.0f, Right, "Grab an imagined lever at waist height and pull it toward you. Three times."),
    RC_STEP("turn_wheel",        12.0f, Both,  "Hold an imagined steering wheel with both hands and turn it left and right."),
    RC_STEP("turn_dial",         10.0f, Right, "Turn an imagined dial on a wall with thumb and fingers, both ways."),
    RC_STEP("two_hand_scale",    10.0f, Both,  "Pinch with both hands and pull them apart, then together, three times."),
    RC_STEP("two_hand_rotate",   10.0f, Both,  "Pinch with both hands and rotate an imagined object, both ways."),
    RC_STEP("clap",              6.0f,  Both,  "Clap five times."),
    RC_STEP("poses",             15.0f, Right, "Show each for two seconds: point, thumbs up, OK, victory, open palm, fist."),
    RC_STEP("palm_up",           8.0f,  Left,  "Turn the left palm up toward you as if looking at a watch, then down. Three times."),
    RC_STEP("wave",              8.0f,  Right, "Wave hello."),
    RC_STEP("beckon",            6.0f,  Right, "Beckon someone to come closer, palm up."),
    RC_STEP("system_gesture",    10.0f, Right, "Look at the right palm and pinch, as for the system menu. Three times."),
    RC_STEP("microgestures",     12.0f, Right, "Tap the thumb on the side of the curled index, then swipe it forward, back, left, right."),
    RC_STEP("talking_hands",     15.0f, Both,  "Talk about your day and move your hands as you naturally do."),
    RC_STEP("scratch_head",      6.0f,  Right, "Scratch your head, then lower the hand."),
    RC_STEP("idle",              15.0f, Both,  "Stand naturally, hands wherever they rest, without doing anything."),
};

#undef RC_STEP

static void logf (const RC_App* app, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void logf (const RC_App* app, const char* fmt, ...)
{
    char    line[512];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (app && app->options.log)
    {
        app->options.log(line);
    }
    else
    {
        (void)fprintf(stderr, "puppetxr-recorder: %s\n", line);
    }
}

static b8 check (RC_App* app, XrResult result, const char* what)
{
    if (XR_FAILED(result))
    {
        logf(app, "%s failed with %d", what, (int)result);
        return false;
    }
    return true;
}

static V3 v3 (const XrVector3f& v)
{
    return { v.x, v.y, v.z };
}

static V3 sub (V3 a, V3 b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

static V3 cross (V3 a, V3 b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

static V3 rotate (Q q, V3 v)
{
    const V3 u = { q.x, q.y, q.z };
    const V3 c = cross(u, v);
    const V3 t = { 2.0f * c.x, 2.0f * c.y, 2.0f * c.z };
    const V3 d = cross(u, t);
    return { v.x + q.w * t.x + d.x, v.y + q.w * t.y + d.y, v.z + q.w * t.z + d.z };
}

static Q q (const XrQuaternionf& o)
{
    return { o.x, o.y, o.z, o.w };
}

static Q conjugate (Q o)
{
    return { -o.x, -o.y, -o.z, o.w };
}

static f32 dot (V3 a, V3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static V3 scale (V3 v, f32 s)
{
    return { v.x * s, v.y * s, v.z * s };
}

static V3 normalize (V3 v)
{
    const f32 length = sqrtf(dot(v, v));
    return length > 1e-6f ? scale(v, 1.0f / length) : V3{ 0.0f, 0.0f, 1.0f };
}

// The rotation whose columns are the orthonormal axes x, y, z
static Q from_axes (V3 x, V3 y, V3 z)
{
    const f32 trace = x.x + y.y + z.z;
    if (trace > 0.0f)
    {
        const f32 s = sqrtf(trace + 1.0f) * 2.0f;
        return { (y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s };
    }
    if (x.x > y.y && x.x > z.z)
    {
        const f32 s = sqrtf(1.0f + x.x - y.y - z.z) * 2.0f;
        return { 0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s };
    }
    if (y.y > z.z)
    {
        const f32 s = sqrtf(1.0f + y.y - x.x - z.z) * 2.0f;
        return { (y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s };
    }
    const f32 s = sqrtf(1.0f + z.z - x.x - y.y) * 2.0f;
    return { (z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s };
}

static b8 name_valid (const char* name)
{
    if (!name[0])
    {
        return false;
    }
    for (const char* c = name; *c; c++)
    {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
        {
            return false;
        }
    }
    return true;
}

static void copy_string (char* out, u32 size, const char* in, u32 length)
{
    const u32 n = length < size - 1 ? length : size - 1;
    memcpy(out, in, n);
    out[n] = 0;
}

//
// Protocol
//

static b8 token_is (const char* json, const jsmntok_t& token, const char* text)
{
    const u32 length = (u32)(token.end - token.start);
    return token.type == JSMN_STRING && strlen(text) == length && memcmp(json + token.start, text, length) == 0;
}

// Tokens of a value and everything inside it
static i32 skip (const jsmntok_t* tokens, i32 index)
{
    i32 remaining = 1;
    while (remaining > 0)
    {
        remaining += tokens[index].size - 1; // jsmn counts an object's keys and gives each key its value
        index++;
    }
    return index;
}

// A JSON string's source text without its escapes; \u escapes outside ASCII become '?'
static void copy_unescaped (char* out, u32 size, const char* in, u32 length)
{
    u32 n = 0;
    for (u32 i = 0; i < length && n + 1 < size; i++)
    {
        char c = in[i];
        if (c == '\\' && i + 1 < length)
        {
            const char e = in[++i];
            c            = e == 'n' ? ' ' : e == 't' ? ' ' : e == 'b' || e == 'f' || e == 'r' ? ' ' : e;
            if (e == 'u')
            {
                const char          hex[5] = { i + 4 < length ? in[i + 1] : '0', i + 4 < length ? in[i + 2] : '0', i + 4 < length ? in[i + 3] : '0', i + 4 < length ? in[i + 4] : '0', 0 };
                const unsigned long code   = strtoul(hex, nullptr, 16);
                c                        = code >= 0x20 && code < 0x7f ? (char)code : '?';
                i += 4;
            }
        }
        out[n++] = c;
    }
    out[n] = 0;
}

static b8 parse_step (const char* json, const jsmntok_t* tokens, i32 index, RC_Step& out, char* error, u32 error_size, u32 step)
{
    out           = { .seconds = 10.0f, .hands = RC_Hands::Both };
    const i32 end = skip(tokens, index);
    i32       i   = index + 1;
    while (i < end)
    {
        const jsmntok_t& key   = tokens[i];
        const jsmntok_t& value = tokens[i + 1];
        const char*      text  = json + value.start;
        const u32        size  = (u32)(value.end - value.start);
        if (token_is(json, key, "name") && value.type == JSMN_STRING)
        {
            copy_unescaped(out.name, RC_NAME_SIZE, text, size);
        }
        else if (token_is(json, key, "instruction") && value.type == JSMN_STRING)
        {
            copy_unescaped(out.instruction, RC_INSTRUCTION_SIZE, text, size);
        }
        else if (token_is(json, key, "seconds") && value.type == JSMN_PRIMITIVE)
        {
            out.seconds = strtof(text, nullptr);
        }
        else if (token_is(json, key, "hands") && value.type == JSMN_STRING)
        {
            out.hands = token_is(json, value, "left") ? RC_Hands::Left : token_is(json, value, "right") ? RC_Hands::Right : RC_Hands::Both;
        }
        i = skip(tokens, i + 1);
    }
    if (!name_valid(out.name) || !(out.seconds > 0.0f && out.seconds <= 600.0f))
    {
        (void)snprintf(error, error_size, "steps[%u]: needs a name of [a-z0-9_] and seconds in (0, 600]", step);
        return false;
    }
    return true;
}

//
// Panel
//

static const char* glyph (char c)
{
    const char upper = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
    for (const auto& g : rc_glyphs)
    {
        if (g.c == upper)
        {
            return g.rows;
        }
    }
    return upper == ' ' ? nullptr : "01110100010000100010001000000000100";
}

static void fill (RC_App* app, Rect r, u32 color)
{
    for (i32 y = r.y; y < r.y + r.h; y++)
    {
        for (i32 x = r.x; x < r.x + r.w; x++)
        {
            if (x >= 0 && y >= 0 && x < RC_PANEL_WIDTH && y < RC_PANEL_HEIGHT)
            {
                app->pixels[(u32)y * RC_PANEL_WIDTH + (u32)x] = color;
            }
        }
    }
}

static void text (RC_App* app, i32 x, i32 y, i32 scale, u32 color, const char* s)
{
    for (; *s; s++, x += (RC_GLYPH_W + 1) * scale)
    {
        const char* rows = glyph(*s);
        if (!rows)
        {
            continue;
        }
        for (i32 gy = 0; gy < RC_GLYPH_H; gy++)
        {
            for (i32 gx = 0; gx < RC_GLYPH_W; gx++)
            {
                if (rows[gy * RC_GLYPH_W + gx] == '1')
                {
                    fill(app, { x + gx * scale, y + gy * scale, scale, scale }, color);
                }
            }
        }
    }
}

// Word wrapped; returns the y below the last line
static i32 paragraph (RC_App* app, i32 x, i32 y, i32 scale, u32 color, const char* s, i32 width)
{
    const i32 per_line = width / ((RC_GLYPH_W + 1) * scale);
    char      line[RC_INSTRUCTION_SIZE];
    while (*s)
    {
        i32 length = (i32)strlen(s);
        if (length > per_line)
        {
            length = per_line;
            while (length > 0 && s[length] != ' ')
            {
                length--;
            }
            length = length > 0 ? length : per_line;
        }
        copy_string(line, sizeof(line), s, (u32)length);
        text(app, x, y, scale, color, line);
        y += (RC_GLYPH_H + 3) * scale;
        s += length;
        while (*s == ' ')
        {
            s++;
        }
    }
    return y;
}

// RGBA8 in memory order
static u32 rgba (u8 r, u8 g, u8 b)
{
    return (u32)r | (u32)g << 8 | (u32)b << 16 | 0xff000000u;
}

static void layout_buttons (RC_App* app)
{
    const i32 w = 148, h = 56, gap = 8, x0 = 12;
    for (u32 a = 0; a < (u32)Action::Count; a++)
    {
        const i32 column = (i32)(a % 4);
        const i32 row    = (i32)(a / 4);
        app->buttons[a]  = { (Action)a, rc_action_labels[a], { x0 + column * (w + gap), 264 + row * (h + gap), w, h } };
    }
    app->buttons_count = (u32)Action::Count;
}

static void draw_panel (RC_App* app, const UiView& view)
{
    const u32 background = rgba(24, 26, 30);
    const u32 foreground = rgba(235, 235, 235);
    const u32 dim        = rgba(160, 165, 175);
    const u32 red        = rgba(220, 50, 50);
    fill(app, { 0, 0, RC_PANEL_WIDTH, RC_PANEL_HEIGHT }, view.recording ? red : background);
    fill(app, { 6, 6, RC_PANEL_WIDTH - 12, RC_PANEL_HEIGHT - 12 }, background);
    text(app, 16, 16, 3, foreground, view.title);
    paragraph(app, 16, 56, 2, dim, view.instruction, RC_PANEL_WIDTH - 32);
    text(app, 16, 226, 2, view.recording ? red : foreground, view.status);
    for (u32 b = 0; b < app->buttons_count; b++)
    {
        const Button& button = app->buttons[b];
        const char*   label  = button.action == Action::Free && view.free_take ? "STOP" : button.label;
        const b8      hovered = view.hovered[0] == b || view.hovered[1] == b;
        fill(app, button.rect, hovered ? rgba(70, 105, 160) : rgba(60, 64, 74));
        const i32 text_width = (i32)strlen(label) * (RC_GLYPH_W + 1) * 2;
        text(app, button.rect.x + (button.rect.w - text_width) / 2, button.rect.y + (button.rect.h - RC_GLYPH_H * 2) / 2, 2, foreground, label);
    }
    for (u32 side = 0; side < 2; side++)
    {
        if (view.cursor_on[side])
        {
            const i32 x = view.cursors[side][0], y = view.cursors[side][1];
            fill(app, { x - 7, y - 7, 14, 14 }, rgba(20, 20, 20));
            fill(app, { x - 5, y - 5, 10, 10 }, foreground);
        }
    }
}

static void build_view (const RC_App* app, i64 now, UiView& out)
{
    out = {};
    if (app->free_take)
    {
        (void)snprintf(out.title, sizeof(out.title), "FREE TAKE");
    }
    else
    {
        (void)snprintf(out.title, sizeof(out.title), "%u/%u %s", app->step + 1, app->steps_count, app->steps[app->step].name);
        (void)snprintf(out.instruction, sizeof(out.instruction), "%s", app->steps[app->step].instruction);
    }
    out.recording = app->mode == Mode::Recording;
    for (u32 side = 0; side < 2; side++)
    {
        out.cursor_on[side]  = app->cursor_on[side];
        out.cursors[side][0] = app->cursor_on[side] ? app->cursors[side][0] : (i16)0;
        out.cursors[side][1] = app->cursor_on[side] ? app->cursors[side][1] : (i16)0;
        out.hovered[side]    = app->hovered[side];
    }
    out.free_take = app->free_take;
    if (app->mode == Mode::Countdown)
    {
        (void)snprintf(out.status, sizeof(out.status), "GET READY: %d", (int)((app->countdown_end - now) / 1000000000 + 1));
    }
    else if (app->mode == Mode::Recording)
    {
        const f64 elapsed = (f64)(now - app->take.start_time) * 1e-9;
        (void)snprintf(out.status, sizeof(out.status), "RECORDING %.1f S", app->free_take ? elapsed : (f64)app->take.seconds - elapsed);
    }
    else
    {
        (void)snprintf(out.status, sizeof(out.status), "%s", app->status);
    }
}

//
// Takes
//

static void write_escaped (FILE* file, const char* s)
{
    (void)fputc('"', file);
    for (; *s; s++)
    {
        const u8 c = (u8)*s;
        if (c == '"' || c == '\\')
        {
            (void)fputc('\\', file);
            (void)fputc(c, file);
        }
        else if (c < 0x20)
        {
            (void)fprintf(file, "\\u%04x", c);
        }
        else
        {
            (void)fputc(c, file);
        }
    }
    (void)fputc('"', file);
}

static void write_pose (FILE* file, const XrPosef& pose)
{
    (void)fprintf(file, "[%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%.6f]", (double)pose.position.x, (double)pose.position.y, (double)pose.position.z, (double)pose.orientation.x, (double)pose.orientation.y, (double)pose.orientation.z, (double)pose.orientation.w);
}

static const char* hands_name (RC_Hands hands)
{
    return hands == RC_Hands::Left ? "left" : hands == RC_Hands::Right ? "right" : "both";
}

static void start_take (RC_App* app, i64 now)
{
    if (app->takes_count == RC_MAX_TAKES)
    {
        (void)snprintf(app->status, sizeof(app->status), "TOO MANY TAKES, RESTART");
        app->mode = Mode::Idle;
        return;
    }
    const RC_Step* step = app->free_take ? nullptr : &app->steps[app->step];
    const char*    name = step ? step->name : "free";
    char*          path = app->take_paths[app->takes_count];
    char           built[RC_PATH_SIZE];
    (void)snprintf(built, sizeof(built), "%s/%03u_%s.json", app->options.out_dir, app->takes_count + 1, name);
    memcpy(path, built, sizeof(built));
    FILE* file = fopen(path, "wb");
    if (!file)
    {
        logf(app, "cannot write %s", path);
        (void)snprintf(app->status, sizeof(app->status), "CANNOT WRITE THE TAKE");
        app->mode = Mode::Idle;
        return;
    }
    app->take = { .file = file, .start_time = now, .seconds = step ? step->seconds : 0.0f };
    copy_string(app->take.name, RC_NAME_SIZE, name, (u32)strlen(name));
    app->mode = Mode::Recording;

    (void)fprintf(file, "{\n\"format\":\"puppetxr-take\",\n\"version\":1,\n\"name\":\"%s\",\n\"instruction\":", name);
    write_escaped(file, step ? step->instruction : "");
    (void)fprintf(file, ",\n\"hands\":\"%s\",\n\"space\":\"%s\",\n\"runtime\":", hands_name(step ? step->hands : RC_Hands::Both), app->space_name);
    write_escaped(file, app->runtime_name);
    (void)fprintf(file, ",\n\"system\":");
    write_escaped(file, app->system_name);
    (void)fprintf(file, ",\n\"tracks\":[\"head\"%s%s%s],\n\"joint_set\":\"XR_EXT_hand_tracking\",\n\"display_period_ns\":%lld,\n\"frames\":[\n", app->has.hand_tracking ? ",\"hands\"" : "", app->has.aim ? ",\"aim\"" : "", app->has.hand_interaction ? ",\"interaction\"" : "", (long long)app->display_period);
}

static void finish_take (RC_App* app)
{
    (void)fprintf(app->take.file, "\n]\n}\n");
    const b8 ok = fclose(app->take.file) == 0;
    (void)snprintf(app->status, sizeof(app->status), ok ? "SAVED %03u_%s, %u FRAMES" : "WRITING %03u_%s FAILED", app->takes_count + 1, app->take.name, app->take.frames);
    logf(app, "%s: %u frames", app->take_paths[app->takes_count], app->take.frames);
    app->takes_count += ok ? 1u : 0u;
    app->take         = {};
    app->mode         = Mode::Idle;
    if (app->free_take)
    {
        app->free_take = false;
    }
    else if (app->step + 1 < app->steps_count)
    {
        app->step++;
    }
    else if (app->options.automatic && !app->exit_requested)
    {
        app->exit_requested = true;
        app->xr.xrRequestExitSession(app->session);
    }
}

struct HandFrame
{
    XrHandJointLocationEXT           joints[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT          locations;
    XrHandTrackingAimStateFB         aim;
    XrHandTrackingDataSourceStateEXT source;
    b8                               interaction_active;
    f32                              interaction[(u32)Interaction::Count];
};

static void locate_hand (RC_App* app, u32 side, i64 time, HandFrame& out)
{
    out           = {};
    out.aim       = { .type = XR_TYPE_HAND_TRACKING_AIM_STATE_FB };
    out.source    = { .type = XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT };
    out.locations = { .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT, .jointCount = XR_HAND_JOINT_COUNT_EXT, .jointLocations = out.joints };
    void* chain   = nullptr;
    if (app->has.data_source)
    {
        out.source.next = chain;
        chain           = &out.source;
    }
    if (app->has.aim)
    {
        out.aim.next = chain;
        chain        = &out.aim;
    }
    out.locations.next = chain;

    const XrHandJointsMotionRangeInfoEXT range = { .type = XR_TYPE_HAND_JOINTS_MOTION_RANGE_INFO_EXT, .handJointsMotionRange = XR_HAND_JOINTS_MOTION_RANGE_UNOBSTRUCTED_EXT };
    const XrHandJointsLocateInfoEXT      info  = { .type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, .next = app->has.motion_range ? &range : nullptr, .baseSpace = app->base, .time = time };
    if (!app->trackers[side] || XR_FAILED(app->xr.locate_hand_joints(app->trackers[side], &info, &out.locations)))
    {
        out.locations.isActive = XR_FALSE;
    }
}

static void write_interaction (FILE* file, const HandFrame& hand)
{
    if (hand.interaction_active)
    {
        (void)fprintf(file, ",\"interaction\":{\"pinch\":%.4f,\"aim_activate\":%.4f,\"grasp\":%.4f}", (double)hand.interaction[(u32)Interaction::Pinch], (double)hand.interaction[(u32)Interaction::AimActivate], (double)hand.interaction[(u32)Interaction::Grasp]);
    }
}

static void write_hand (const RC_App* app, FILE* file, const HandFrame& hand)
{
    if (!hand.locations.isActive)
    {
        (void)fprintf(file, "{\"active\":0");
        write_interaction(file, hand);
        (void)fprintf(file, "}");
        return;
    }
    (void)fprintf(file, "{\"active\":1");
    write_interaction(file, hand);
    if (app->has.data_source)
    {
        const char* source = !hand.source.isActive ? "none" : hand.source.dataSource == XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT ? "unobstructed" : "controller";
        (void)fprintf(file, ",\"source\":\"%s\"", source);
    }
    (void)fprintf(file, ",\"flags\":[");
    for (u32 j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++)
    {
        (void)fprintf(file, j ? ",%u" : "%u", (u32)hand.joints[j].locationFlags);
    }
    (void)fprintf(file, "],\"joints\":[");
    for (u32 j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++)
    {
        const XrHandJointLocationEXT& joint = hand.joints[j];
        const XrPosef&                p     = joint.pose;
        (void)fprintf(file, "%s[%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%.6f,%.5f]", j ? "," : "", (double)p.position.x, (double)p.position.y, (double)p.position.z, (double)p.orientation.x, (double)p.orientation.y, (double)p.orientation.z, (double)p.orientation.w, (double)joint.radius);
    }
    (void)fprintf(file, "]");
    if (app->has.aim)
    {
        (void)fprintf(file, ",\"aim\":{\"status\":%llu,\"pose\":", (unsigned long long)hand.aim.status);
        write_pose(file, hand.aim.aimPose);
        (void)fprintf(file, ",\"pinch\":[%.4f,%.4f,%.4f,%.4f]}", (double)hand.aim.pinchStrengthIndex, (double)hand.aim.pinchStrengthMiddle, (double)hand.aim.pinchStrengthRing, (double)hand.aim.pinchStrengthLittle);
    }
    (void)fprintf(file, "}");
}

static void write_frame (RC_App* app, i64 time, const XrSpaceLocation& head, const HandFrame (&hands)[2])
{
    FILE* file = app->take.file;
    (void)fprintf(file, "%s{\"t\":%lld,\"head\":", app->take.frames ? ",\n" : "", (long long)(time - app->take.start_time));
    const b8 head_valid = (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    if (head_valid)
    {
        write_pose(file, head.pose);
    }
    else
    {
        (void)fprintf(file, "null");
    }
    if (app->has.hand_tracking)
    {
        (void)fprintf(file, ",\"left\":");
        write_hand(app, file, hands[0]);
        (void)fprintf(file, ",\"right\":");
        write_hand(app, file, hands[1]);
    }
    (void)fprintf(file, "}");
    (void)fflush(file);
    app->take.frames++;
}

//
// Interaction
//

static void place_panel (RC_App* app, const XrPosef& head)
{
    const V3  forward = rotate(q(head.orientation), { 0.0f, 0.0f, -1.0f });
    const f32 flat    = sqrtf(forward.x * forward.x + forward.z * forward.z);
    const V3  ahead   = flat > 1e-3f ? V3{ forward.x / flat, 0.0f, forward.z / flat } : V3{ 0.0f, 0.0f, -1.0f };
    const f32 yaw     = atan2f(-ahead.x, -ahead.z);
    app->panel_pose   = {
        { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) },
        { head.position.x + ahead.x * RC_PANEL_AHEAD, head.position.y - RC_PANEL_BELOW, head.position.z + ahead.z * RC_PANEL_AHEAD },
    };
    app->panel_placed = true;
}

static f32 panel_height ()
{
    return RC_PANEL_METRES * (f32)RC_PANEL_HEIGHT / (f32)RC_PANEL_WIDTH;
}

static void act (RC_App* app, Action action, i64 now)
{
    const b8 idle = app->mode == Mode::Idle;
    if (app->mode == Mode::Recording && !(action == Action::Free && app->free_take))
    {
        return;
    }
    switch (action)
    {
        case Action::Record:
            if (idle)
            {
                app->mode          = Mode::Countdown;
                app->countdown_end = now + (i64)(app->options.countdown_seconds * 1e9f);
            }
            break;
        case Action::Redo:
            if (idle)
            {
                app->step          = app->step > 0 && app->takes_count > 0 ? app->step - 1 : app->step;
                app->mode          = Mode::Countdown;
                app->countdown_end = now + (i64)(app->options.countdown_seconds * 1e9f);
            }
            break;
        case Action::Previous:
            app->step = app->step > 0 ? app->step - 1 : app->step;
            app->mode = Mode::Idle;
            break;
        case Action::Next:
            app->step = app->step + 1 < app->steps_count ? app->step + 1 : app->step;
            app->mode = Mode::Idle;
            break;
        case Action::Free:
            if (app->free_take && app->mode == Mode::Recording)
            {
                finish_take(app);
            }
            else if (idle)
            {
                app->free_take = true;
                start_take(app, now);
            }
            break;
        case Action::Recenter:
            app->panel_placed = false;
            break;
        case Action::Count:
            break;
    }
}

static u8 button_at (const RC_App* app, f32 px, f32 py)
{
    for (u32 b = 0; b < app->buttons_count; b++)
    {
        const Rect& r = app->buttons[b].rect;
        if (px >= (f32)r.x && px < (f32)(r.x + r.w) && py >= (f32)r.y && py < (f32)(r.y + r.h))
        {
            return (u8)b;
        }
    }
    return RC_NO_BUTTON;
}

// The system's pointer: the aim ray of XR_FB_hand_tracking_aim, a click on the index pinch's start
static void aim (RC_App* app, u32 side, const HandFrame& hand, const XrSpaceLocation& head, i64 now)
{
    app->ray_shown[side] = false;
    app->cursor_on[side] = false;
    app->hovered[side]   = RC_NO_BUTTON;
    const b8 valid       = app->has.aim && hand.locations.isActive && (hand.aim.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB);
    // A Quest 3 reported no pinch in the aim state while the tips touched: the joints decide too
    const XrHandJointLocationEXT& thumb = hand.joints[XR_HAND_JOINT_THUMB_TIP_EXT];
    const XrHandJointLocationEXT& index = hand.joints[XR_HAND_JOINT_INDEX_TIP_EXT];
    const b8                      tips  = hand.locations.isActive && (thumb.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (index.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    const V3                      gap   = sub(v3(thumb.pose.position), v3(index.pose.position));
    const f32                     apart = sqrtf(dot(gap, gap));
    app->tips_pinching[side]            = tips && (apart < RC_PINCH_CLOSE || (app->tips_pinching[side] && apart < RC_PINCH_OPEN));
    const b8 pinching    = valid && ((hand.aim.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB) || app->tips_pinching[side]);
    const b8 pressed     = pinching && !app->was_pinching[side];
    app->was_pinching[side] = pinching;
    // A free take ends with a click on STOP, so its ray stays; a step's take hides it
    if (!valid || !app->panel_placed || (app->mode == Mode::Recording && !app->free_take))
    {
        return;
    }

    const V3 origin    = v3(hand.aim.aimPose.position);
    const V3 direction = rotate(q(hand.aim.aimPose.orientation), { 0.0f, 0.0f, -1.0f });
    const Q  panel     = conjugate(q(app->panel_pose.orientation));
    const V3 local     = rotate(panel, sub(origin, v3(app->panel_pose.position)));
    const V3 along     = rotate(panel, direction);
    f32      length    = RC_RAY_LENGTH;
    if (local.z > 0.0f && along.z < -1e-4f)
    {
        const f32 t  = local.z / -along.z;
        const f32 px = ((local.x + along.x * t) / RC_PANEL_METRES + 0.5f) * (f32)RC_PANEL_WIDTH;
        const f32 py = (0.5f - (local.y + along.y * t) / panel_height()) * (f32)RC_PANEL_HEIGHT;
        if (px >= 0.0f && py >= 0.0f && px < (f32)RC_PANEL_WIDTH && py < (f32)RC_PANEL_HEIGHT)
        {
            length                  = t;
            app->cursor_on[side]    = true;
            app->cursors[side][0]   = (i16)px;
            app->cursors[side][1]   = (i16)py;
            app->hovered[side]      = button_at(app, px, py);
        }
    }
    if (pressed && app->hovered[side] != RC_NO_BUTTON)
    {
        act(app, app->buttons[app->hovered[side]].action, now);
    }

    // A thin quad from the hand along the ray, turned toward the eyes
    const V3 to_head   = (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? sub(v3(head.pose.position), origin) : V3{ 0.0f, 0.0f, 1.0f };
    const V3 facing    = normalize(sub(to_head, scale(direction, dot(to_head, direction))));
    const V3 side_axis = cross(direction, facing);
    const Q  rotation  = from_axes(side_axis, direction, facing);
    const V3 middle    = { origin.x + direction.x * length * 0.5f, origin.y + direction.y * length * 0.5f, origin.z + direction.z * length * 0.5f };
    app->ray_poses[side]   = { { rotation.x, rotation.y, rotation.z, rotation.w }, { middle.x, middle.y, middle.z } };
    app->ray_lengths[side] = length;
    app->ray_shown[side]   = true;
}

static void poke (RC_App* app, u32 side, const HandFrame& hand, i64 now)
{
    const XrHandJointLocationEXT& tip   = hand.joints[XR_HAND_JOINT_INDEX_TIP_EXT];
    const b8                      valid = hand.locations.isActive && (tip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    if (!valid || !app->panel_placed)
    {
        app->poke_armed[side] = true;
        return;
    }
    const V3  local = rotate(conjugate(q(app->panel_pose.orientation)), sub(v3(tip.pose.position), v3(app->panel_pose.position)));
    const f32 px    = (local.x / RC_PANEL_METRES + 0.5f) * (f32)RC_PANEL_WIDTH;
    const f32 py    = (0.5f - local.y / panel_height()) * (f32)RC_PANEL_HEIGHT;
    if (local.z > RC_POKE_RELEASE || local.z < -RC_POKE_BEHIND)
    {
        app->poke_armed[side] = true;
        return;
    }
    if (local.z > RC_POKE_DEPTH || !app->poke_armed[side])
    {
        return;
    }
    const u8 button = button_at(app, px, py);
    if (button != RC_NO_BUTTON)
    {
        app->poke_armed[side] = false;
        act(app, app->buttons[button].action, now);
    }
}

//
// Setup
//

static b8 load_xr (RC_App* app)
{
#define RC_LOAD(fn) app->gipa(app->instance, #fn, (PFN_xrVoidFunction*)&app->xr.fn);
    RC_XR_FUNCTIONS(RC_LOAD)
#undef RC_LOAD
    app->gipa(app->instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction*)&app->xr.get_vulkan_requirements);
    app->gipa(app->instance, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction*)&app->xr.create_vulkan_instance);
    app->gipa(app->instance, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction*)&app->xr.get_vulkan_device);
    app->gipa(app->instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction*)&app->xr.create_vulkan_device);
    if (app->has.hand_tracking)
    {
        app->gipa(app->instance, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction*)&app->xr.create_hand_tracker);
        app->gipa(app->instance, "xrDestroyHandTrackerEXT", (PFN_xrVoidFunction*)&app->xr.destroy_hand_tracker);
        app->gipa(app->instance, "xrLocateHandJointsEXT", (PFN_xrVoidFunction*)&app->xr.locate_hand_joints);
    }
    if (app->has.frequency_hint)
    {
        app->gipa(app->instance, "xrSetHandTrackingFrequencyHintMETA", (PFN_xrVoidFunction*)&app->xr.set_hand_frequency);
    }
    if (app->has.passthrough)
    {
        app->gipa(app->instance, "xrCreatePassthroughFB", (PFN_xrVoidFunction*)&app->xr.create_passthrough);
        app->gipa(app->instance, "xrDestroyPassthroughFB", (PFN_xrVoidFunction*)&app->xr.destroy_passthrough);
        app->gipa(app->instance, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction*)&app->xr.create_passthrough_layer);
        app->gipa(app->instance, "xrDestroyPassthroughLayerFB", (PFN_xrVoidFunction*)&app->xr.destroy_passthrough_layer);
    }
    return app->xr.xrWaitFrame && app->xr.get_vulkan_requirements && app->xr.create_vulkan_instance && app->xr.get_vulkan_device && app->xr.create_vulkan_device;
}

static b8 create_instance (RC_App* app)
{
    PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
    PFN_xrCreateInstance                       create    = nullptr;
    app->gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumerate);
    app->gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&create);
    if (!enumerate || !create)
    {
        logf(app, "the runtime has no xrCreateInstance");
        return false;
    }

    static XrExtensionProperties available[512];
    u32                          available_count = 0;
    for (XrExtensionProperties& properties : available)
    {
        properties = { .type = XR_TYPE_EXTENSION_PROPERTIES };
    }
    if (!check(app, enumerate(nullptr, 512, &available_count, available), "xrEnumerateInstanceExtensionProperties"))
    {
        return false;
    }
    auto offered = [&] (const char* name)
    {
        for (u32 i = 0; i < available_count; i++)
        {
            if (strcmp(available[i].extensionName, name) == 0)
            {
                return true;
            }
        }
        return false;
    };

    const char* enabled[16 + RC_MAX_EXTENSIONS] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    u32         enabled_count                   = 1;
    auto        want = [&] (const char* name, b8& out_has)
    {
        out_has = offered(name);
        if (out_has)
        {
            enabled[enabled_count++] = name;
        }
        else
        {
            logf(app, "the runtime has no %s", name);
        }
    };
    want(XR_EXT_HAND_TRACKING_EXTENSION_NAME, app->has.hand_tracking);
    if (app->has.hand_tracking)
    {
        want(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME, app->has.aim);
        want(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME, app->has.data_source);
        want(XR_EXT_HAND_JOINTS_MOTION_RANGE_EXTENSION_NAME, app->has.motion_range);
        want(XR_META_HAND_TRACKING_FREQUENCY_HINT_EXTENSION_NAME, app->has.frequency_hint);
        want(XR_EXT_HAND_INTERACTION_EXTENSION_NAME, app->has.hand_interaction);
    }
    want(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME, app->has.local_floor);
    if (app->options.passthrough)
    {
        want(XR_FB_PASSTHROUGH_EXTENSION_NAME, app->has.passthrough);
    }
    for (u32 i = 0; i < app->options.extensions_count; i++)
    {
        enabled[enabled_count++] = app->options.extensions[i];
    }

    XrInstanceCreateInfo info = { .type = XR_TYPE_INSTANCE_CREATE_INFO, .next = app->options.instance_next, .enabledExtensionCount = enabled_count, .enabledExtensionNames = enabled };
    strcpy(info.applicationInfo.applicationName, "puppetxr-recorder");
    info.applicationInfo.applicationVersion = 1;
    info.applicationInfo.apiVersion         = XR_API_VERSION_1_0;
    if (!check(app, create(&info, &app->instance), "xrCreateInstance") || !load_xr(app))
    {
        return false;
    }

    XrInstanceProperties instance_properties = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    app->xr.xrGetInstanceProperties(app->instance, &instance_properties);
    copy_string(app->runtime_name, sizeof(app->runtime_name), instance_properties.runtimeName, (u32)strlen(instance_properties.runtimeName));

    const XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    if (!check(app, app->xr.xrGetSystem(app->instance, &system_info, &app->system), "xrGetSystem"))
    {
        return false;
    }
    XrSystemProperties system_properties = { .type = XR_TYPE_SYSTEM_PROPERTIES };
    app->xr.xrGetSystemProperties(app->instance, app->system, &system_properties);
    copy_string(app->system_name, sizeof(app->system_name), system_properties.systemName, (u32)strlen(system_properties.systemName));
    return true;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message (VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) // NOLINT(misc-const-correctness): PFN_vkDebugUtilsMessengerCallbackEXT
{
    RC_App* app = (RC_App*)user;
    app->vulkan_errors++;
    logf(app, "Vulkan validation: %s", data->pMessage);
    return VK_FALSE;
}

static u32 memory_type (const RC_App* app, u32 bits, VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties memory = {};
    vkGetPhysicalDeviceMemoryProperties(app->vk_physical, &memory);
    for (u32 type = 0; type < memory.memoryTypeCount; type++)
    {
        if ((bits & (1u << type)) && (memory.memoryTypes[type].propertyFlags & wanted) == wanted)
        {
            return type;
        }
    }
    return UINT32_MAX;
}

static b8 create_vulkan (RC_App* app)
{
    if (volkGetInstanceVersion() == 0 && volkInitialize() != VK_SUCCESS)
    {
        logf(app, "no Vulkan loader");
        return false;
    }
    XrGraphicsRequirementsVulkan2KHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    if (!check(app, app->xr.get_vulkan_requirements(app->instance, app->system, &requirements), "xrGetVulkanGraphicsRequirements2KHR"))
    {
        return false;
    }

    VkResult                            vk_result        = VK_SUCCESS;
    const VkApplicationInfo             app_info         = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "puppetxr-recorder", .apiVersion = VK_API_VERSION_1_1 };
    // The loader hides portability drivers such as MoltenVK from instances that do not ask for them
    VkExtensionProperties instance_extensions[256]   = {};
    u32                   instance_extensions_count = 256;
    vkEnumerateInstanceExtensionProperties(nullptr, &instance_extensions_count, instance_extensions);
    b8 portability = false;
    b8 debug_utils = false;
    for (u32 i = 0; i < instance_extensions_count; i++)
    {
        portability = portability || strcmp(instance_extensions[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
        debug_utils = debug_utils || strcmp(instance_extensions[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
    }
    VkLayerProperties layers[64]   = {};
    u32               layers_count = 64;
    vkEnumerateInstanceLayerProperties(&layers_count, layers);
    b8 validation = false;
    for (u32 i = 0; i < layers_count && app->options.vulkan_validation; i++)
    {
        validation = validation || strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation") == 0;
    }
    validation                         = validation && debug_utils;
    const char* extension_names[2]     = {};
    u32         extension_names_count  = 0;
    if (portability)
    {
        extension_names[extension_names_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    }
    if (validation)
    {
        extension_names[extension_names_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }
    else if (app->options.vulkan_validation)
    {
        logf(app, "VK_LAYER_KHRONOS_validation is not installed; running without it");
    }
    const char* const                   layer_name       = "VK_LAYER_KHRONOS_validation";
    const VkInstanceCreateInfo          instance_info    = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = portability ? (VkInstanceCreateFlags)VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0u, .pApplicationInfo = &app_info, .enabledLayerCount = validation ? 1u : 0u, .ppEnabledLayerNames = &layer_name, .enabledExtensionCount = extension_names_count, .ppEnabledExtensionNames = extension_names };
    const XrVulkanInstanceCreateInfoKHR xr_instance_info = { .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = app->system, .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = &instance_info };
    if (!check(app, app->xr.create_vulkan_instance(app->instance, &xr_instance_info, &app->vk_instance, &vk_result), "xrCreateVulkanInstanceKHR") || vk_result != VK_SUCCESS)
    {
        return false;
    }
    volkLoadInstanceOnly(app->vk_instance);
    if (validation)
    {
        const VkDebugUtilsMessengerCreateInfoEXT messenger_info = { .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, .pfnUserCallback = validation_message, .pUserData = app };
        vkCreateDebugUtilsMessengerEXT(app->vk_instance, &messenger_info, nullptr, &app->messenger);
    }

    const XrVulkanGraphicsDeviceGetInfoKHR device_get_info = { .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = app->system, .vulkanInstance = app->vk_instance };
    if (!check(app, app->xr.get_vulkan_device(app->instance, &device_get_info, &app->vk_physical), "xrGetVulkanGraphicsDevice2KHR"))
    {
        return false;
    }
    VkQueueFamilyProperties families[16] = {};
    u32                     family_count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(app->vk_physical, &family_count, families);
    while (app->queue_family < family_count && !(families[app->queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    {
        app->queue_family++;
    }
    if (app->queue_family == family_count)
    {
        logf(app, "no graphics queue");
        return false;
    }

    const f32                         priority       = 1.0f;
    const VkDeviceQueueCreateInfo     queue_info     = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = app->queue_family, .queueCount = 1, .pQueuePriorities = &priority };
    // Spec: a device that offers VK_KHR_portability_subset must have it enabled
    VkExtensionProperties device_extensions[512]   = {};
    u32                   device_extensions_count = 512;
    vkEnumerateDeviceExtensionProperties(app->vk_physical, nullptr, &device_extensions_count, device_extensions);
    b8 subset = false;
    for (u32 i = 0; i < device_extensions_count; i++)
    {
        subset = subset || strcmp(device_extensions[i].extensionName, "VK_KHR_portability_subset") == 0;
    }
    const char* const                 subset_name    = "VK_KHR_portability_subset";
    const VkDeviceCreateInfo          device_info    = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info, .enabledExtensionCount = subset ? 1u : 0u, .ppEnabledExtensionNames = &subset_name };
    const XrVulkanDeviceCreateInfoKHR xr_device_info = { .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = app->system, .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanPhysicalDevice = app->vk_physical, .vulkanCreateInfo = &device_info };
    if (!check(app, app->xr.create_vulkan_device(app->instance, &xr_device_info, &app->vk_device, &vk_result), "xrCreateVulkanDeviceKHR") || vk_result != VK_SUCCESS)
    {
        return false;
    }
    volkLoadDeviceTable(&app->vk, app->vk_device);
    app->vk.vkGetDeviceQueue(app->vk_device, app->queue_family, 0, &app->queue);

    const VkCommandPoolCreateInfo pool_info  = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = app->queue_family };
    const VkFenceCreateInfo       fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (app->vk.vkCreateCommandPool(app->vk_device, &pool_info, nullptr, &app->pool) != VK_SUCCESS || app->vk.vkCreateFence(app->vk_device, &fence_info, nullptr, &app->fence) != VK_SUCCESS)
    {
        return false;
    }
    const VkCommandBufferAllocateInfo cmd_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = app->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (app->vk.vkAllocateCommandBuffers(app->vk_device, &cmd_info, &app->cmd) != VK_SUCCESS)
    {
        return false;
    }

    const VkBufferCreateInfo buffer_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof(app->pixels), .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (app->vk.vkCreateBuffer(app->vk_device, &buffer_info, nullptr, &app->staging) != VK_SUCCESS)
    {
        return false;
    }
    VkMemoryRequirements memory_requirements = {};
    app->vk.vkGetBufferMemoryRequirements(app->vk_device, app->staging, &memory_requirements);
    VkPhysicalDeviceMemoryProperties memory = {};
    vkGetPhysicalDeviceMemoryProperties(app->vk_physical, &memory);
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    u32                         type   = 0;
    while (type < memory.memoryTypeCount && !((memory_requirements.memoryTypeBits & (1u << type)) && (memory.memoryTypes[type].propertyFlags & wanted) == wanted))
    {
        type++;
    }
    const VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = memory_requirements.size, .memoryTypeIndex = type };
    return type < memory.memoryTypeCount && app->vk.vkAllocateMemory(app->vk_device, &allocate, nullptr, &app->staging_memory) == VK_SUCCESS && app->vk.vkBindBufferMemory(app->vk_device, app->staging, app->staging_memory, 0) == VK_SUCCESS && app->vk.vkMapMemory(app->vk_device, app->staging_memory, 0, VK_WHOLE_SIZE, 0, &app->staging_mapped) == VK_SUCCESS;
}

static b8 create_space (RC_App* app)
{
    XrReferenceSpaceType types[64]   = {};
    u32                  types_count = 0;
    app->xr.xrEnumerateReferenceSpaces(app->session, 0, &types_count, nullptr);
    types_count = types_count < 64 ? types_count : 64;
    if (XR_FAILED(app->xr.xrEnumerateReferenceSpaces(app->session, types_count, &types_count, types)))
    {
        types_count = 0;
    }
    auto has_space = [&] (XrReferenceSpaceType type)
    {
        for (u32 i = 0; i < types_count; i++)
        {
            if (types[i] == type)
            {
                return true;
            }
        }
        return false;
    };
    XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_LOCAL;
    app->space_name           = "local";
    if (app->options.space == RC_Space::LocalFloor && app->has.local_floor && has_space(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT))
    {
        type            = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
        app->space_name = "local_floor";
    }
    else if (has_space(XR_REFERENCE_SPACE_TYPE_STAGE))
    {
        type            = XR_REFERENCE_SPACE_TYPE_STAGE;
        app->space_name = "stage";
    }
    XrReferenceSpaceCreateInfo info = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = type, .poseInReferenceSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    if (!check(app, app->xr.xrCreateReferenceSpace(app->session, &info, &app->base), "xrCreateReferenceSpace"))
    {
        return false;
    }
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    return check(app, app->xr.xrCreateReferenceSpace(app->session, &info, &app->view), "xrCreateReferenceSpace");
}

// The hand skeleton

static b8 create_skeleton_renderer (RC_App* app, i64 format)
{
    XrViewConfigurationView views[2]  = { { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW }, { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    u32                     count     = 0;
    if (!check(app, app->xr.xrEnumerateViewConfigurationViews(app->instance, app->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views), "xrEnumerateViewConfigurationViews"))
    {
        return false;
    }
    app->eye_width  = views[0].recommendedImageRectWidth;
    app->eye_height = views[0].recommendedImageRectHeight;

    const XrSwapchainCreateInfo swapchain_info = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, .format = format, .sampleCount = 1, .width = app->eye_width, .height = app->eye_height, .faceCount = 1, .arraySize = 2, .mipCount = 1 };
    if (!check(app, app->xr.xrCreateSwapchain(app->session, &swapchain_info, &app->eye_swapchain), "xrCreateSwapchain"))
    {
        return false;
    }
    XrSwapchainImageVulkan2KHR images[RC_MAX_IMAGES] = {};
    for (XrSwapchainImageVulkan2KHR& image : images)
    {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    }
    if (!check(app, app->xr.xrEnumerateSwapchainImages(app->eye_swapchain, RC_MAX_IMAGES, &app->eye_images_count, (XrSwapchainImageBaseHeader*)images), "xrEnumerateSwapchainImages"))
    {
        return false;
    }

    const VkDevice device = app->vk_device;
    const VkImageCreateInfo depth_info = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = RC_DEPTH_FORMAT, .extent = { app->eye_width, app->eye_height, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (app->vk.vkCreateImage(device, &depth_info, nullptr, &app->depth_image) != VK_SUCCESS)
    {
        return false;
    }
    VkMemoryRequirements depth_requirements = {};
    app->vk.vkGetImageMemoryRequirements(device, app->depth_image, &depth_requirements);
    const VkMemoryAllocateInfo depth_allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = depth_requirements.size, .memoryTypeIndex = memory_type(app, depth_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (depth_allocate.memoryTypeIndex == UINT32_MAX || app->vk.vkAllocateMemory(device, &depth_allocate, nullptr, &app->depth_memory) != VK_SUCCESS || app->vk.vkBindImageMemory(device, app->depth_image, app->depth_memory, 0) != VK_SUCCESS)
    {
        return false;
    }
    const VkImageViewCreateInfo depth_view_info = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = app->depth_image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = RC_DEPTH_FORMAT, .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
    if (app->vk.vkCreateImageView(device, &depth_view_info, nullptr, &app->depth_view) != VK_SUCCESS)
    {
        return false;
    }

    // Spec XR_KHR_vulkan_enable2: a released image is in COLOR_ATTACHMENT_OPTIMAL
    const VkAttachmentDescription attachments[2] = {
        { .format = (VkFormat)format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        { .format = RC_DEPTH_FORMAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
    };
    const VkAttachmentReference color_reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    const VkAttachmentReference depth_reference = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    const VkSubpassDescription  subpass         = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &color_reference, .pDepthStencilAttachment = &depth_reference };
    // The depth image is cleared again for the second eye while the first eye's pass may still test against it
    const VkSubpassDependency   dependency      = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    const VkRenderPassCreateInfo pass_info      = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = attachments, .subpassCount = 1, .pSubpasses = &subpass, .dependencyCount = 1, .pDependencies = &dependency };
    if (app->vk.vkCreateRenderPass(device, &pass_info, nullptr, &app->render_pass) != VK_SUCCESS)
    {
        return false;
    }

    for (u32 i = 0; i < app->eye_images_count; i++)
    {
        app->eye_images[i] = images[i].image;
        for (u32 eye = 0; eye < 2; eye++)
        {
            const VkImageViewCreateInfo view_info = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = images[i].image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = (VkFormat)format, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1 } };
            if (app->vk.vkCreateImageView(device, &view_info, nullptr, &app->eye_views[i][eye]) != VK_SUCCESS)
            {
                return false;
            }
            const VkImageView             framebuffer_views[2] = { app->eye_views[i][eye], app->depth_view };
            const VkFramebufferCreateInfo framebuffer_info     = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = app->render_pass, .attachmentCount = 2, .pAttachments = framebuffer_views, .width = app->eye_width, .height = app->eye_height, .layers = 1 };
            if (app->vk.vkCreateFramebuffer(device, &framebuffer_info, nullptr, &app->eye_framebuffers[i][eye]) != VK_SUCCESS)
            {
                return false;
            }
        }
    }

    const VkPushConstantRange        push        = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof(f32) };
    const VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
    if (app->vk.vkCreatePipelineLayout(device, &layout_info, nullptr, &app->pipeline_layout) != VK_SUCCESS)
    {
        return false;
    }
    VkShaderModule                 modules[2]   = {};
    const VkShaderModuleCreateInfo vert_info    = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(rc_skeleton_vert), .pCode = rc_skeleton_vert };
    const VkShaderModuleCreateInfo frag_info    = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(rc_skeleton_frag), .pCode = rc_skeleton_frag };
    if (app->vk.vkCreateShaderModule(device, &vert_info, nullptr, &modules[0]) != VK_SUCCESS || app->vk.vkCreateShaderModule(device, &frag_info, nullptr, &modules[1]) != VK_SUCCESS)
    {
        return false;
    }
    const VkPipelineShaderStageCreateInfo        stages[2]     = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main" },
    };
    const VkVertexInputBindingDescription        binding       = { 0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE };
    const VkVertexInputAttributeDescription      attributes[2] = { { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(f32) } };
    const VkPipelineVertexInputStateCreateInfo   input         = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attributes };
    const VkPipelineInputAssemblyStateCreateInfo assembly      = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    const VkPipelineViewportStateCreateInfo      viewport      = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    const VkPipelineRasterizationStateCreateInfo raster        = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    const VkPipelineMultisampleStateCreateInfo   multisample   = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    const VkPipelineDepthStencilStateCreateInfo  depth         = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS };
    const VkPipelineColorBlendAttachmentState    blend_target  = { .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    const VkPipelineColorBlendStateCreateInfo    blend         = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend_target };
    const VkDynamicState                         dynamic[2]    = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    const VkPipelineDynamicStateCreateInfo       dynamics      = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dynamic };
    const VkGraphicsPipelineCreateInfo           pipeline_info = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &input, .pInputAssemblyState = &assembly, .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &multisample, .pDepthStencilState = &depth, .pColorBlendState = &blend, .pDynamicState = &dynamics, .layout = app->pipeline_layout, .renderPass = app->render_pass };
    const VkResult pipeline_result = app->vk.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &app->pipeline);
    app->vk.vkDestroyShaderModule(device, modules[0], nullptr);
    app->vk.vkDestroyShaderModule(device, modules[1], nullptr);
    if (pipeline_result != VK_SUCCESS)
    {
        return false;
    }

    const VkBufferCreateInfo buffer_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof(Instance) * RC_MAX_INSTANCES, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (app->vk.vkCreateBuffer(device, &buffer_info, nullptr, &app->instance_buffer) != VK_SUCCESS)
    {
        return false;
    }
    VkMemoryRequirements buffer_requirements = {};
    app->vk.vkGetBufferMemoryRequirements(device, app->instance_buffer, &buffer_requirements);
    const VkMemoryAllocateInfo buffer_allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = buffer_requirements.size, .memoryTypeIndex = memory_type(app, buffer_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    return buffer_allocate.memoryTypeIndex != UINT32_MAX && app->vk.vkAllocateMemory(device, &buffer_allocate, nullptr, &app->instance_memory) == VK_SUCCESS && app->vk.vkBindBufferMemory(device, app->instance_buffer, app->instance_memory, 0) == VK_SUCCESS && app->vk.vkMapMemory(device, app->instance_memory, 0, VK_WHOLE_SIZE, 0, (void**)&app->instances) == VK_SUCCESS;
}

static void collect_instances (RC_App* app, const HandFrame (&hands)[2])
{
    app->instances_count = 0;
    for (u32 side = 0; side < 2; side++)
    {
        const HandFrame& hand = hands[side];
        if (!hand.locations.isActive)
        {
            continue;
        }
        for (u32 j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++)
        {
            const XrHandJointLocationEXT& joint = hand.joints[j];
            if (!(joint.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
            {
                continue;
            }
            const XrVector3f& p = joint.pose.position;
            app->instances[app->instances_count++] = { { p.x, p.y, p.z, RC_JOINT_RADIUS }, { p.x, p.y, p.z, 2.0f } };
            const u8 parent = rc_joint_parents[j];
            if (parent != 0xff && (hand.joints[parent].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
            {
                const XrVector3f& a = hand.joints[parent].pose.position;
                app->instances[app->instances_count++] = { { a.x, a.y, a.z, RC_BONE_RADIUS }, { p.x, p.y, p.z, (f32)side } };
            }
        }
    }
}

// Column-major, Vulkan's clip space: y down, depth 0 to 1 (as the OpenXR SDK's xr_linear.h)
static void view_projection (const XrView& view, f32 (&out)[16])
{
    const f32 left   = tanf(view.fov.angleLeft);
    const f32 right  = tanf(view.fov.angleRight);
    const f32 up     = tanf(view.fov.angleUp);
    const f32 down   = tanf(view.fov.angleDown);
    const f32 width  = right - left;
    const f32 height = down - up;
    const f32 projection[16] = {
        2.0f / width, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f / height, 0.0f, 0.0f,
        (right + left) / width, (up + down) / height, -RC_FAR / (RC_FAR - RC_NEAR), -1.0f,
        0.0f, 0.0f, -(RC_FAR * RC_NEAR) / (RC_FAR - RC_NEAR), 0.0f,
    };
    // The inverse of the eye's pose: rotate by the conjugate, after moving to the eye
    const Q  inverse = conjugate(q(view.pose.orientation));
    const V3 x       = rotate(inverse, { 1.0f, 0.0f, 0.0f });
    const V3 y       = rotate(inverse, { 0.0f, 1.0f, 0.0f });
    const V3 z       = rotate(inverse, { 0.0f, 0.0f, 1.0f });
    const V3 t       = scale(rotate(inverse, v3(view.pose.position)), -1.0f);
    const f32 eye[16] = {
        x.x, x.y, x.z, 0.0f,
        y.x, y.y, y.z, 0.0f,
        z.x, z.y, z.z, 0.0f,
        t.x, t.y, t.z, 1.0f,
    };
    for (u32 column = 0; column < 4; column++)
    {
        for (u32 row = 0; row < 4; row++)
        {
            f32 sum = 0.0f;
            for (u32 k = 0; k < 4; k++)
            {
                sum += projection[k * 4 + row] * eye[column * 4 + k];
            }
            out[column * 4 + row] = sum;
        }
    }
}

static b8 render_skeleton (RC_App* app, const XrView (&views)[2])
{
    u32 index = 0;
    if (!check(app, app->xr.xrAcquireSwapchainImage(app->eye_swapchain, nullptr, &index), "xrAcquireSwapchainImage"))
    {
        return false;
    }
    const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
    if (!check(app, app->xr.xrWaitSwapchainImage(app->eye_swapchain, &wait_info), "xrWaitSwapchainImage"))
    {
        return false;
    }
    app->vk.vkResetFences(app->vk_device, 1, &app->fence);
    const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    app->vk.vkBeginCommandBuffer(app->cmd, &begin);
    const VkClearValue clears[2] = { { .color = { { 0.0f, 0.0f, 0.0f, 0.0f } } }, { .depthStencil = { 1.0f, 0 } } };
    const VkViewport   viewport  = { 0.0f, 0.0f, (f32)app->eye_width, (f32)app->eye_height, 0.0f, 1.0f };
    const VkRect2D     scissor   = { { 0, 0 }, { app->eye_width, app->eye_height } };
    const VkDeviceSize offset    = 0;
    for (u32 eye = 0; eye < 2; eye++)
    {
        const VkRenderPassBeginInfo pass = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = app->render_pass, .framebuffer = app->eye_framebuffers[index][eye], .renderArea = scissor, .clearValueCount = 2, .pClearValues = clears };
        f32 matrix[16] = {};
        view_projection(views[eye], matrix);
        app->vk.vkCmdBeginRenderPass(app->cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
        app->vk.vkCmdBindPipeline(app->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app->pipeline);
        app->vk.vkCmdSetViewport(app->cmd, 0, 1, &viewport);
        app->vk.vkCmdSetScissor(app->cmd, 0, 1, &scissor);
        app->vk.vkCmdPushConstants(app->cmd, app->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(matrix), matrix);
        app->vk.vkCmdBindVertexBuffers(app->cmd, 0, 1, &app->instance_buffer, &offset);
        app->vk.vkCmdDraw(app->cmd, 36, app->instances_count, 0, 0);
        app->vk.vkCmdEndRenderPass(app->cmd);
    }
    app->vk.vkEndCommandBuffer(app->cmd);
    const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &app->cmd };
    app->vk.vkQueueSubmit(app->queue, 1, &submit, app->fence);
    app->vk.vkWaitForFences(app->vk_device, 1, &app->fence, VK_TRUE, UINT64_MAX);
    return check(app, app->xr.xrReleaseSwapchainImage(app->eye_swapchain, nullptr), "xrReleaseSwapchainImage");
}

static void destroy_skeleton_renderer (RC_App* app)
{
    const VkDevice device = app->vk_device;
    for (u32 i = 0; i < app->eye_images_count; i++)
    {
        for (u32 eye = 0; eye < 2; eye++)
        {
            if (app->eye_framebuffers[i][eye])
            {
                app->vk.vkDestroyFramebuffer(device, app->eye_framebuffers[i][eye], nullptr);
            }
            if (app->eye_views[i][eye])
            {
                app->vk.vkDestroyImageView(device, app->eye_views[i][eye], nullptr);
            }
        }
    }
    if (app->pipeline)
    {
        app->vk.vkDestroyPipeline(device, app->pipeline, nullptr);
    }
    if (app->pipeline_layout)
    {
        app->vk.vkDestroyPipelineLayout(device, app->pipeline_layout, nullptr);
    }
    if (app->render_pass)
    {
        app->vk.vkDestroyRenderPass(device, app->render_pass, nullptr);
    }
    if (app->depth_view)
    {
        app->vk.vkDestroyImageView(device, app->depth_view, nullptr);
    }
    if (app->depth_image)
    {
        app->vk.vkDestroyImage(device, app->depth_image, nullptr);
    }
    if (app->depth_memory)
    {
        app->vk.vkFreeMemory(device, app->depth_memory, nullptr);
    }
    if (app->instance_buffer)
    {
        app->vk.vkDestroyBuffer(device, app->instance_buffer, nullptr);
    }
    if (app->instance_memory)
    {
        app->vk.vkFreeMemory(device, app->instance_memory, nullptr);
    }
}

static const char* const rc_interaction_names[(u32)Interaction::Count]      = { "pinch", "aim_activate", "grasp" };
static const char* const rc_interaction_components[(u32)Interaction::Count] = { "pinch_ext/value", "aim_activate_ext/value", "grasp_ext/value" };

static b8 create_actions (RC_App* app)
{
    XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(set_info.actionSetName, "recorder");
    strcpy(set_info.localizedActionSetName, "Recorder");
    if (!check(app, app->xr.xrCreateActionSet(app->instance, &set_info, &app->action_set), "xrCreateActionSet"))
    {
        return false;
    }
    app->xr.xrStringToPath(app->instance, "/user/hand/left", &app->hand_paths[0]);
    app->xr.xrStringToPath(app->instance, "/user/hand/right", &app->hand_paths[1]);
    XrActionSuggestedBinding bindings[(u32)Interaction::Count * 2] = {};
    for (u32 i = 0; i < (u32)Interaction::Count; i++)
    {
        XrActionCreateInfo action_info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionType = XR_ACTION_TYPE_FLOAT_INPUT, .countSubactionPaths = 2, .subactionPaths = app->hand_paths };
        (void)snprintf(action_info.actionName, sizeof(action_info.actionName), "%s", rc_interaction_names[i]);
        (void)snprintf(action_info.localizedActionName, sizeof(action_info.localizedActionName), "%s", rc_interaction_names[i]);
        if (!check(app, app->xr.xrCreateAction(app->action_set, &action_info, &app->interactions[i]), "xrCreateAction"))
        {
            return false;
        }
        for (u32 side = 0; side < 2; side++)
        {
            char path[128];
            (void)snprintf(path, sizeof(path), "/user/hand/%s/input/%s", side == 0 ? "left" : "right", rc_interaction_components[i]);
            bindings[i * 2 + side].action = app->interactions[i];
            app->xr.xrStringToPath(app->instance, path, &bindings[i * 2 + side].binding);
        }
    }
    XrPath profile = XR_NULL_PATH;
    app->xr.xrStringToPath(app->instance, "/interaction_profiles/ext/hand_interaction_ext", &profile);
    const XrInteractionProfileSuggestedBinding suggested = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, .interactionProfile = profile, .countSuggestedBindings = (u32)Interaction::Count * 2, .suggestedBindings = bindings };
    const XrSessionActionSetsAttachInfo        attach    = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &app->action_set };
    return check(app, app->xr.xrSuggestInteractionProfileBindings(app->instance, &suggested), "xrSuggestInteractionProfileBindings") && check(app, app->xr.xrAttachSessionActionSets(app->session, &attach), "xrAttachSessionActionSets");
}

static void read_interactions (const RC_App* app, u32 side, HandFrame& out)
{
    out.interaction_active = false;
    if (!app->actions_synced)
    {
        return;
    }
    for (u32 i = 0; i < (u32)Interaction::Count; i++)
    {
        XrActionStateFloat         state = { .type = XR_TYPE_ACTION_STATE_FLOAT };
        const XrActionStateGetInfo info  = { .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = app->interactions[i], .subactionPath = app->hand_paths[side] };
        if (XR_SUCCEEDED(app->xr.xrGetActionStateFloat(app->session, &info, &state)) && state.isActive)
        {
            out.interaction_active = true;
            out.interaction[i]     = state.currentState;
        }
    }
}

static b8 create_session (RC_App* app)
{
    const XrGraphicsBindingVulkan2KHR binding = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = app->vk_instance, .physicalDevice = app->vk_physical, .device = app->vk_device, .queueFamilyIndex = app->queue_family, .queueIndex = 0 };
    const XrSessionCreateInfo         info    = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = app->system };
    if (!check(app, app->xr.xrCreateSession(app->instance, &info, &app->session), "xrCreateSession") || !create_space(app))
    {
        return false;
    }

    u32 formats_count = 0;
    app->xr.xrEnumerateSwapchainFormats(app->session, 0, &formats_count, nullptr);
    i64* formats = (i64*)calloc(formats_count ? formats_count : 1, sizeof(i64));
    i64  format  = 0;
    if (formats && XR_SUCCEEDED(app->xr.xrEnumerateSwapchainFormats(app->session, formats_count, &formats_count, formats)))
    {
        for (u32 i = 0; i < formats_count && !format; i++)
        {
            format = formats[i] == VK_FORMAT_R8G8B8A8_SRGB ? formats[i] : 0;
        }
    }
    free(formats);
    if (!format)
    {
        logf(app, "the runtime has no R8G8B8A8_SRGB swapchains");
        return false;
    }
    const XrSwapchainCreateInfo swapchain_info = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, .format = format, .sampleCount = 1, .width = RC_PANEL_WIDTH, .height = RC_PANEL_HEIGHT, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
    if (!check(app, app->xr.xrCreateSwapchain(app->session, &swapchain_info, &app->panel_swapchain), "xrCreateSwapchain"))
    {
        return false;
    }
    XrSwapchainImageVulkan2KHR images[RC_MAX_IMAGES] = {};
    for (XrSwapchainImageVulkan2KHR& image : images)
    {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    }
    if (!check(app, app->xr.xrEnumerateSwapchainImages(app->panel_swapchain, RC_MAX_IMAGES, &app->panel_images_count, (XrSwapchainImageBaseHeader*)images), "xrEnumerateSwapchainImages"))
    {
        return false;
    }
    for (u32 i = 0; i < app->panel_images_count; i++)
    {
        app->panel_images[i] = images[i].image;
    }
    XrSwapchainCreateInfo ray_info = swapchain_info;
    ray_info.createFlags           = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    ray_info.width                 = RC_RAY_PIXELS;
    ray_info.height                = RC_RAY_PIXELS;
    if (!check(app, app->xr.xrCreateSwapchain(app->session, &ray_info, &app->ray_swapchain), "xrCreateSwapchain") || !check(app, app->xr.xrEnumerateSwapchainImages(app->ray_swapchain, RC_MAX_IMAGES, &app->ray_images_count, (XrSwapchainImageBaseHeader*)images), "xrEnumerateSwapchainImages"))
    {
        return false;
    }
    for (u32 i = 0; i < app->ray_images_count; i++)
    {
        app->ray_images[i] = images[i].image;
    }
    if (!create_skeleton_renderer(app, format))
    {
        logf(app, "cannot create the skeleton's renderer");
        return false;
    }

    if (app->has.hand_tracking)
    {
        XrHandTrackingDataSourceEXT           unobstructed = XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT;
        const XrHandTrackingDataSourceInfoEXT sources      = { .type = XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT, .requestedDataSourceCount = 1, .requestedDataSources = &unobstructed };
        for (u32 side = 0; side < 2; side++)
        {
            const XrHandTrackerCreateInfoEXT tracker_info = { .type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, .next = app->has.data_source ? &sources : nullptr, .hand = side == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT, .handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT };
            if (!check(app, app->xr.create_hand_tracker(app->session, &tracker_info, &app->trackers[side]), "xrCreateHandTrackerEXT"))
            {
                return false;
            }
        }
        if (app->has.frequency_hint)
        {
            check(app, app->xr.set_hand_frequency(app->session, XR_HAND_TRACKING_FREQUENCY_HINT_HIGH_META), "xrSetHandTrackingFrequencyHintMETA");
        }
    }

    if (app->has.hand_interaction && !create_actions(app))
    {
        return false;
    }
    if (app->has.passthrough)
    {
        const XrPassthroughCreateInfoFB      passthrough_info = { .type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB };
        const XrPassthroughLayerCreateInfoFB layer_info       = { .type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB, .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB };
        const b8 created = check(app, app->xr.create_passthrough(app->session, &passthrough_info, &app->passthrough), "xrCreatePassthroughFB");
        XrPassthroughLayerCreateInfoFB       layer             = layer_info;
        layer.passthrough                                      = app->passthrough;
        if (!created || !check(app, app->xr.create_passthrough_layer(app->session, &layer, &app->passthrough_layer), "xrCreatePassthroughLayerFB"))
        {
            app->passthrough_layer = XR_NULL_HANDLE;
        }
    }
    return true;
}

static b8 upload_image (RC_App* app, XrSwapchain swapchain, const VkImage* images, const u32* pixels, u32 width, u32 height)
{
    u32 index = 0;
    if (!check(app, app->xr.xrAcquireSwapchainImage(swapchain, nullptr, &index), "xrAcquireSwapchainImage"))
    {
        return false;
    }
    const XrSwapchainImageWaitInfo wait_info = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
    if (!check(app, app->xr.xrWaitSwapchainImage(swapchain, &wait_info), "xrWaitSwapchainImage"))
    {
        return false;
    }
    memcpy(app->staging_mapped, pixels, (usize)width * height * sizeof(u32));

    const VkImage image = images[index];
    app->vk.vkResetFences(app->vk_device, 1, &app->fence);
    const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    app->vk.vkBeginCommandBuffer(app->cmd, &begin);
    const VkImageSubresourceRange range   = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier          barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = range };
    app->vk.vkCmdPipelineBarrier(app->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    const VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { width, height, 1 } };
    app->vk.vkCmdCopyBufferToImage(app->cmd, app->staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    // Spec XR_KHR_vulkan_enable2: a released image is in COLOR_ATTACHMENT_OPTIMAL
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    app->vk.vkCmdPipelineBarrier(app->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    app->vk.vkEndCommandBuffer(app->cmd);
    const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &app->cmd };
    app->vk.vkQueueSubmit(app->queue, 1, &submit, app->fence);
    app->vk.vkWaitForFences(app->vk_device, 1, &app->fence, VK_TRUE, UINT64_MAX);
    return check(app, app->xr.xrReleaseSwapchainImage(swapchain, nullptr), "xrReleaseSwapchainImage");
}

static void poll_events (RC_App* app)
{
    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (app->xr.xrPollEvent(app->instance, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            app->state = ((const XrEventDataSessionStateChanged*)&event)->state;
            logf(app, "session state %d", (int)app->state);
            if (app->state == XR_SESSION_STATE_READY)
            {
                const XrSessionBeginInfo begin_info = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
                app->running = check(app, app->xr.xrBeginSession(app->session, &begin_info), "xrBeginSession");
                app->failed  = !app->running;
            }
            else if (app->state == XR_SESSION_STATE_STOPPING)
            {
                app->xr.xrEndSession(app->session);
                app->running = false;
            }
        }
        event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
}

static b8 frame (RC_App* app)
{
    XrFrameState state = { .type = XR_TYPE_FRAME_STATE };
    if (!check(app, app->xr.xrWaitFrame(app->session, nullptr, &state), "xrWaitFrame") || !check(app, app->xr.xrBeginFrame(app->session, nullptr), "xrBeginFrame"))
    {
        return false;
    }
    const i64 now        = state.predictedDisplayTime;
    app->display_period = state.predictedDisplayPeriod;

    XrSpaceLocation head = { .type = XR_TYPE_SPACE_LOCATION };
    app->xr.xrLocateSpace(app->view, app->base, now, &head);
    const b8 head_valid = (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    if (!app->panel_placed && head_valid)
    {
        place_panel(app, head.pose);
    }

    if (app->action_set)
    {
        const XrActiveActionSet active = { app->action_set, XR_NULL_PATH };
        const XrActionsSyncInfo sync   = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active };
        app->actions_synced            = app->xr.xrSyncActions(app->session, &sync) == XR_SUCCESS;
    }
    HandFrame hands[2] = {};
    for (u32 side = 0; side < 2; side++)
    {
        locate_hand(app, side, now, hands[side]);
        read_interactions(app, side, hands[side]);
    }
    for (u32 side = 0; side < 2; side++)
    {
        poke(app, side, hands[side], now);
        aim(app, side, hands[side], head, now);
    }
    collect_instances(app, hands);
    XrView               views[2]   = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW }};
    XrViewState          view_state = { .type = XR_TYPE_VIEW_STATE };
    u32                  view_count = 0;
    const XrViewLocateInfo locate   = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = now, .space = app->base };
    const b8 views_valid = XR_SUCCEEDED(app->xr.xrLocateViews(app->session, &locate, &view_state, 2, &view_count, views)) && view_count == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    const b8 skeleton    = state.shouldRender && views_valid && app->instances_count > 0 && render_skeleton(app, views);
    if (!skeleton)
    {
        app->instances_count = 0;
    }
    if (!app->ray_uploaded)
    {
        // Premultiplied white at 70 %: the default blend of XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
        u32 ray[RC_RAY_PIXELS * RC_RAY_PIXELS];
        for (u32& pixel : ray)
        {
            pixel = 0xb3b3b3b3u;
        }
        app->ray_uploaded = upload_image(app, app->ray_swapchain, app->ray_images, ray, RC_RAY_PIXELS, RC_RAY_PIXELS);
    }

    if (app->mode == Mode::Idle && app->options.automatic && !app->exit_requested)
    {
        start_take(app, now);
    }
    if (app->mode == Mode::Countdown && now >= app->countdown_end)
    {
        start_take(app, now);
    }
    if (app->mode == Mode::Recording && !app->free_take && (f64)(now - app->take.start_time) >= (f64)app->take.seconds * 1e9)
    {
        finish_take(app);
    }
    if (app->mode == Mode::Recording)
    {
        write_frame(app, now, head, hands);
    }

    UiView view = {};
    build_view(app, now, view);
    if (!app->panel_uploaded || memcmp(&view, &app->shown, sizeof(view)) != 0)
    {
        draw_panel(app, view);
        app->shown          = view;
        app->panel_uploaded = upload_image(app, app->panel_swapchain, app->panel_images, app->pixels, RC_PANEL_WIDTH, RC_PANEL_HEIGHT);
    }

    const XrCompositionLayerPassthroughFB passthrough = { .type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB, .flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, .layerHandle = app->passthrough_layer };
    const XrCompositionLayerQuad          quad        = {
        .type          = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .space         = app->base,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage      = { .swapchain = app->panel_swapchain, .imageRect = { { 0, 0 }, { RC_PANEL_WIDTH, RC_PANEL_HEIGHT } } },
        .pose          = app->panel_pose,
        .size          = { RC_PANEL_METRES, panel_height() },
    };
    XrCompositionLayerQuad rays[2] = {};
    for (u32 side = 0; side < 2; side++)
    {
        rays[side] = {
            .type          = XR_TYPE_COMPOSITION_LAYER_QUAD,
            .layerFlags    = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
            .space         = app->base,
            .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
            .subImage      = { .swapchain = app->ray_swapchain, .imageRect = { { 0, 0 }, { RC_RAY_PIXELS, RC_RAY_PIXELS } } },
            .pose          = app->ray_poses[side],
            .size          = { RC_RAY_WIDTH, app->ray_lengths[side] },
        };
    }
    XrCompositionLayerProjectionView eyes[2] = {};
    for (u32 eye = 0; eye < 2; eye++)
    {
        eyes[eye] = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = views[eye].pose, .fov = views[eye].fov, .subImage = { .swapchain = app->eye_swapchain, .imageRect = { { 0, 0 }, { (i32)app->eye_width, (i32)app->eye_height } }, .imageArrayIndex = eye } };
    }
    // Over the panel, which covers the passthrough hand
    const XrCompositionLayerProjection projection = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, .space = app->base, .viewCount = 2, .views = eyes };
    const XrCompositionLayerBaseHeader* layers[5]    = {};
    u32                                 layers_count = 0;
    if (state.shouldRender && app->passthrough_layer)
    {
        layers[layers_count++] = (const XrCompositionLayerBaseHeader*)&passthrough;
    }
    if (state.shouldRender && app->panel_placed && app->panel_uploaded)
    {
        layers[layers_count++] = (const XrCompositionLayerBaseHeader*)&quad;
    }
    if (skeleton)
    {
        layers[layers_count++] = (const XrCompositionLayerBaseHeader*)&projection;
    }
    for (u32 side = 0; side < 2; side++)
    {
        if (state.shouldRender && app->ray_shown[side] && app->ray_uploaded)
        {
            layers[layers_count++] = (const XrCompositionLayerBaseHeader*)&rays[side];
        }
    }
    const XrFrameEndInfo end_info = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = now, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = layers_count, .layers = layers };
    return check(app, app->xr.xrEndFrame(app->session, &end_info), "xrEndFrame");
}

//
// API
//

uint32_t rc_builtin_protocol (const RC_Step** out_steps)
{
    *out_steps = rc_builtin_steps;
    return (u32)(sizeof(rc_builtin_steps) / sizeof(rc_builtin_steps[0]));
}

bool rc_load_protocol (const char* path, RC_Step (&out_steps)[RC_MAX_STEPS], uint32_t& out_count, char* error, uint32_t error_size)
{
    out_count  = 0;
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        (void)snprintf(error, error_size, "cannot read %s", path);
        return false;
    }
    static char json[256 * 1024];
    const usize size = fread(json, 1, sizeof(json) - 1, file);
    (void)fclose(file);
    json[size] = 0;

    static jsmntok_t tokens[RC_PROTOCOL_TOKENS];
    jsmn_parser      parser = {};
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, json, size, tokens, RC_PROTOCOL_TOKENS);
    if (count < 1 || tokens[0].type != JSMN_OBJECT)
    {
        (void)snprintf(error, error_size, "%s: not a JSON object", path);
        return false;
    }
    i32 i = 1;
    while (i < count)
    {
        if (token_is(json, tokens[i], "steps") && tokens[i + 1].type == JSMN_ARRAY)
        {
            const i32 steps = tokens[i + 1].size;
            i32       s     = i + 2;
            for (i32 n = 0; n < steps; n++)
            {
                if (tokens[s].type != JSMN_OBJECT || out_count == RC_MAX_STEPS || !parse_step(json, tokens, s, out_steps[out_count], error, error_size, out_count))
                {
                    if (tokens[s].type != JSMN_OBJECT || out_count == RC_MAX_STEPS)
                    {
                        (void)snprintf(error, error_size, "steps[%d]: not an object, or more than %d steps", n, RC_MAX_STEPS);
                    }
                    return false;
                }
                out_count++;
                s = skip(tokens, s);
            }
        }
        i = skip(tokens, i + 1);
    }
    if (out_count == 0)
    {
        (void)snprintf(error, error_size, "%s: no steps", path);
        return false;
    }
    return true;
}

RC_App* rc_create (const RC_Options& options)
{
    RC_App* app = (RC_App*)calloc(1, sizeof(RC_App));
    if (!app)
    {
        return nullptr;
    }
    app->options = options;
    app->gipa    = (PFN_xrGetInstanceProcAddr)options.get_instance_proc_addr;
    (void)snprintf(app->status, sizeof(app->status), "POKE RECORD TO START");
    layout_buttons(app);
    for (u32 side = 0; side < 2; side++)
    {
        app->poke_armed[side] = true;
        app->hovered[side]    = RC_NO_BUTTON;
    }

    char error[256] = {};
    if (options.protocol)
    {
        if (!rc_load_protocol(options.protocol, app->steps, app->steps_count, error, sizeof(error)))
        {
            logf(app, "%s", error);
            free(app);
            return nullptr;
        }
    }
    else
    {
        const RC_Step* steps = nullptr;
        app->steps_count     = rc_builtin_protocol(&steps);
        memcpy(app->steps, steps, sizeof(RC_Step) * app->steps_count);
    }

    if (!app->gipa || !create_instance(app) || !create_vulkan(app) || !create_session(app))
    {
        rc_destroy(app);
        return nullptr;
    }
    return app;
}

RC_Status rc_frame (RC_App* app)
{
    poll_events(app);
    if (app->failed)
    {
        return RC_Status::Failed;
    }
    if (app->state == XR_SESSION_STATE_EXITING || app->state == XR_SESSION_STATE_LOSS_PENDING)
    {
        return RC_Status::Ended;
    }
    if (app->running && !frame(app))
    {
        app->failed = true;
        return RC_Status::Failed;
    }
    return RC_Status::Running;
}

void rc_destroy (RC_App* app)
{
    if (!app)
    {
        return;
    }
    if (app->take.file)
    {
        finish_take(app);
    }
    if (app->vk_device)
    {
        app->vk.vkDeviceWaitIdle(app->vk_device);
    }
    for (XrHandTrackerEXT tracker : app->trackers)
    {
        if (tracker)
        {
            app->xr.destroy_hand_tracker(tracker);
        }
    }
    if (app->action_set)
    {
        app->xr.xrDestroyActionSet(app->action_set);
    }
    if (app->passthrough_layer)
    {
        app->xr.destroy_passthrough_layer(app->passthrough_layer);
    }
    if (app->passthrough)
    {
        app->xr.destroy_passthrough(app->passthrough);
    }
    if (app->panel_swapchain)
    {
        app->xr.xrDestroySwapchain(app->panel_swapchain);
    }
    if (app->ray_swapchain)
    {
        app->xr.xrDestroySwapchain(app->ray_swapchain);
    }
    if (app->eye_swapchain)
    {
        app->xr.xrDestroySwapchain(app->eye_swapchain);
    }
    if (app->vk_device)
    {
        destroy_skeleton_renderer(app);
    }
    if (app->session)
    {
        app->xr.xrDestroySession(app->session);
    }
    if (app->vk_device)
    {
        if (app->staging)
        {
            app->vk.vkDestroyBuffer(app->vk_device, app->staging, nullptr);
        }
        if (app->staging_memory)
        {
            app->vk.vkFreeMemory(app->vk_device, app->staging_memory, nullptr);
        }
        if (app->fence)
        {
            app->vk.vkDestroyFence(app->vk_device, app->fence, nullptr);
        }
        if (app->pool)
        {
            app->vk.vkDestroyCommandPool(app->vk_device, app->pool, nullptr);
        }
        app->vk.vkDestroyDevice(app->vk_device, nullptr);
    }
    if (app->messenger)
    {
        vkDestroyDebugUtilsMessengerEXT(app->vk_instance, app->messenger, nullptr);
    }
    if (app->vk_instance)
    {
        vkDestroyInstance(app->vk_instance, nullptr);
    }
    if (app->instance)
    {
        app->xr.xrDestroyInstance(app->instance);
    }
    free(app);
}

uint32_t rc_takes_count (const RC_App* app)
{
    return app->takes_count;
}

const char* rc_take_path (const RC_App* app, uint32_t index)
{
    return index < app->takes_count ? app->take_paths[index] : nullptr;
}

bool rc_recording (const RC_App* app)
{
    return app->mode == Mode::Recording;
}

uint32_t rc_skeleton_instances (const RC_App* app)
{
    return app->instances_count;
}

uint32_t rc_vulkan_errors (const RC_App* app)
{
    return app->vulkan_errors;
}

bool rc_button_center (const RC_App* app, const char* label, float out_position[3])
{
    if (!app->panel_placed)
    {
        return false;
    }
    for (u32 b = 0; b < app->buttons_count; b++)
    {
        const Button& button = app->buttons[b];
        if (strcmp(button.label, label) != 0)
        {
            continue;
        }
        const f32 x     = ((f32)button.rect.x + (f32)button.rect.w * 0.5f) / (f32)RC_PANEL_WIDTH;
        const f32 y     = ((f32)button.rect.y + (f32)button.rect.h * 0.5f) / (f32)RC_PANEL_HEIGHT;
        const V3  local = { (x - 0.5f) * RC_PANEL_METRES, (0.5f - y) * panel_height(), 0.0f };
        const V3  world = rotate(q(app->panel_pose.orientation), local);
        out_position[0] = world.x + app->panel_pose.position.x;
        out_position[1] = world.y + app->panel_pose.position.y;
        out_position[2] = world.z + app->panel_pose.position.z;
        return true;
    }
    return false;
}
