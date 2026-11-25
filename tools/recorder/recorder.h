// The motion recorder (docs/recorder.md): an OpenXR app that writes how tracked hands move into
// JSON takes. The entry points (recorder_desktop.cpp, recorder_android.cpp) and the tests drive it.

#pragma once

#include <stdint.h>

#define RC_MAX_EXTENSIONS   8
#define RC_MAX_STEPS        64
#define RC_NAME_SIZE        64
#define RC_INSTRUCTION_SIZE 256
#define RC_PATH_SIZE        512

enum class RC_Space : uint8_t
{
    LocalFloor, // falls back to STAGE, then LOCAL, where the runtime lacks it
    Stage,
};

enum class RC_Hands : uint8_t
{
    Both,
    Left,
    Right,
};

enum class RC_Status : uint8_t
{
    Running,
    Ended,
    Failed,
};

struct RC_Step
{
    char     name[RC_NAME_SIZE]; // [a-z0-9_], names the take files
    char     instruction[RC_INSTRUCTION_SIZE];
    float    seconds;
    RC_Hands hands;
};

struct RC_Options
{
    void*       get_instance_proc_addr;           // PFN_xrGetInstanceProcAddr of the loader or a runtime
    const void* instance_next;                    // chained to XrInstanceCreateInfo
    const char* extensions[RC_MAX_EXTENSIONS];    // enabled besides the recorder's own
    uint32_t    extensions_count;
    const char* out_dir;                          // must exist
    const char* protocol;                         // a protocol JSON file; null for the built-in protocol
    RC_Space    space;
    bool        passthrough;                      // XR_FB_passthrough under the panel where the runtime has it
    bool        automatic;                        // takes run one after the other without a countdown, the session ends after the last
    float       countdown_seconds;
    bool        vulkan_validation;                // VK_LAYER_KHRONOS_validation where installed; errors are logged and counted
    void      (*log)(const char* line);           // null for stderr
};

struct RC_App;

RC_App*     rc_create          (const RC_Options& options); // null after logging why
RC_Status   rc_frame           (RC_App* app);               // events, then one frame while the session runs
void        rc_destroy         (RC_App* app);
uint32_t    rc_takes_count     (const RC_App* app);
const char* rc_take_path       (const RC_App* app, uint32_t index);
bool        rc_recording       (const RC_App* app);
uint32_t    rc_skeleton_instances (const RC_App* app); // boxes of joints and bones drawn in the last frame
uint32_t    rc_vulkan_errors   (const RC_App* app); // validation errors so far, with vulkan_validation
bool        rc_button_center   (const RC_App* app, const char* label, float out_position[3]); // in the recording space, once the panel is placed
uint32_t    rc_builtin_protocol (const RC_Step** out_steps);
bool        rc_load_protocol   (const char* path, RC_Step (&out_steps)[RC_MAX_STEPS], uint32_t& out_count, char* error, uint32_t error_size);
