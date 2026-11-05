#pragma once

#include "sim.h"
#include "world.h"

// The OpenXR layer owns one runtime per process. These accessors exist for the
// control API and the tests; everything else enters through xrGetInstanceProcAddr.

struct OXR_Runtime;

OXR_Runtime* oxr_get_runtime ();
SIM_Sim*     oxr_get_sim     (); // null when no instance is alive
void         oxr_lock        ();
void         oxr_unlock      ();
b8           oxr_release_runtime (); // frees the runtime's global state; false while an instance is alive

// Composited output of the first graphics session, for tests and the control API
b8           oxr_read_frame      (u8* out_rgba, usize out_size, u32& out_width, u32& out_height); // as screenshots see it, without secure layers; out_size 0 only gives the size
b8           oxr_read_mirror_frame (u8* out_rgba, usize out_size, u32& out_width, u32& out_height); // as the headset shows it
u64          oxr_frame_read_thread (); // the thread that last read a frame off the GPU: xrEndFrame's when the frame loop is split
b8           oxr_save_screenshot (const char* path);
b8           oxr_frame_ready     (); // a frame to read or save: with the control server on, once xrEndFrame read one back
b8           oxr_frames_in_flight (); // a session waited for a frame it has not ended yet
b8           oxr_ui_click        (const char* label); // left click on the mirror window's UI item so labelled last frame, through the window's events; false without one
b8           oxr_ui_find         (const char* label, f32& out_x, f32& out_y, b8& out_hovered, f32* out_mouse); // its centre in window pixels last frame, whether the UI had the mouse on it, and where the UI had the mouse
void         oxr_show_ui         (b8 shown);           // the panels, as controls.ui toggles them
b8           oxr_mouse_captured  ();                   // the mirror window holds the mouse
void         oxr_set_lens_mask   (b8 on);              // views.lens_mask while running; sessions get XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR
void         oxr_set_mirror_view (SIM_MirrorView view);

// The welcome overlay of the mirror window; the key is handled as if pressed in the window
b8           oxr_welcome_key     (OS_Key key, b8 editing_text = false); // true when the overlay consumed it; editing_text: an ImGui text field has the keyboard
b8           oxr_welcome_shown   ();

struct OXR_RecordStatus
{
    b8  recording;
    u64 recorded_frames;
    b8  replaying;          // a replay is loaded and has frames left
    b8  replay_finished;
    u64 replay_frame;       // frames replayed so far
    u64 replay_total;
    b8  replay_complete;    // the file had its end marker
    u32 divergences;        // frames whose state hash differed from the recording
    u64 first_divergence;   // sim frame index of the first one, 0 if none
};

// Recording starts from the current state; replay applies at the next xrWaitFrame. A replay
// started on a live instance must match its eye resolution and display period.
b8               oxr_record_start   (const char* path);
b8               oxr_record_stop    ();
b8               oxr_replay_start   (const char* path);
void             oxr_replay_stop    ();
OXR_RecordStatus oxr_record_status  ();
void             oxr_request_exit   ();

// The controller type is fixed for a recording, so changing it fails while recording or replaying
b8               oxr_set_controller (SIM_Controller controller);

enum class OXR_RoomResult : u8
{
    Ok = 0,
    NoInstance,
    NotFound, // unknown name, or the room files are invalid
    Busy,     // recording or replaying
};

// Rooms are fixed for a recording. Null or empty selects no room.
OXR_RoomResult   oxr_set_room       (const char* name);
OXR_RoomResult   oxr_choose_room    (const char* name); // as picked in the UI: also the default room of the next start
b8               oxr_get_room       (char* out_name, usize out_size); // false without an instance

// Extensions never offered, on top of the config's disabled_extensions: a comma separated list,
// null or empty for none. Works without an instance and applies from the next xrCreateInstance.
void             oxr_set_disabled_extensions (const char* names);

enum class OXR_ControlResult : u8
{
    Ok = 0,
    NoInstance,
    Busy,    // recording or replaying
    Invalid,
};

// Anchor requests the simulator can make fail, docs/anchors.md
enum class OXR_AnchorOp : u8
{
    Save = 0, // xrSaveSpaceFB, xrSaveSpaceListFB, xrSaveSpacesMETA
    Erase,    // xrEraseSpaceFB, xrEraseSpacesMETA
    Load,     // xrQuerySpacesFB, xrDiscoverSpacesMETA
    Share,    // xrShareSpacesFB, xrShareSpacesMETA
    Count,
};

// Frame calls the simulator can make fail, docs/openxr.md "Frame faults"
enum class OXR_FrameOp : u8
{
    Wait = 0,    // xrWaitFrame
    Begin,       // xrBeginFrame
    LocateViews, // xrLocateViews
    End,         // xrEndFrame
    Count,
};

struct OXR_AnchorEntry
{
    W_StoredAnchor anchor;
    b8             cloud; // in the simulated cloud, else in the local store
    u64            owner; // cloud only
};

// Anchors the app's store holds for the simulated user, local first, then the cloud anchors
// visible to the user; returns how many there are, writing at most capacity
u32               oxr_list_anchors      (OXR_AnchorEntry* out_entries, u32 capacity);
OXR_ControlResult oxr_wipe_anchors      (b8 include_cloud); // the local store, and the cloud anchors the user owns
// The next `count` requests of op complete with `result`, an error; count 0 clears the fault
OXR_ControlResult oxr_set_anchor_fault  (OXR_AnchorOp op, i32 result, u32 count);
// Frames until anchor requests complete, and frames a loaded anchor stays unlocalized
OXR_ControlResult oxr_set_anchor_timing (u32 latency_frames, u32 localize_frames);
// The next `count` calls of op that pass their checks return `result` without doing their work;
// only errors the spec lists for that function are taken. Count 0 clears the fault.
OXR_ControlResult oxr_set_frame_fault   (OXR_FrameOp op, i32 result, u32 count);
b8                oxr_get_origin_offset (Pose& out_room_from_stage); // false without an instance

// What the display was asked for: XR_FB_display_refresh_rate, XR_FB_color_space, XR_META_local_dimming, XR_META_boundary_visibility
struct OXR_DisplayState
{
    f32         refresh_rate;
    const char* color_space;   // "rec709", "p3", ...
    b8          local_dimming;
    b8          boundary_suppressed; // XR_META_boundary_visibility
};

b8                oxr_get_display_state (OXR_DisplayState& out_state); // false without an instance; the first session's choices

// XR_META_virtual_keyboard as the app placed it; the first session's keyboard
struct OXR_KeyboardState
{
    b8   exists;
    b8   visible;
    b8   shifted;
    Pose pose;  // stage space; the keys face its +Z
    f32  scale;
    char context[256]; // the end of the app's text before the cursor
};

b8                oxr_get_keyboard_state (OXR_KeyboardState& out_state); // false without an instance
// A key's top centre in stage space, oriented as the keyboard; false without a keyboard or for a
// name sim_vkb_find_key does not know
b8                oxr_get_keyboard_key   (const char* name, Pose& out_pose);

// The control server lives in the api layer above and registers these when the library loads
struct OXR_ControlHooks
{
    void (*instance_created)   (const SIM_Config& config, const char* application_name);
    void (*instance_destroyed) ();
    void (*poll)               (b8 frame); // first thing in xrPollEvent and, with frame set, xrWaitFrame, runtime lock held; may block to pause
};

void             oxr_set_control_hooks (const OXR_ControlHooks& hooks);

