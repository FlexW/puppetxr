#pragma once

#include "sim.h"
#include "world.h"
#include "render.h"

struct UI_Context;

struct UI_CreateInfo
{
    R_Device*  device;
    OS_Window* window;
    R_Format   target_format; // format of the images ui_draw renders into
};

// Requests the UI raised this frame; the caller performs them
struct UI_Requests
{
    b8 screenshot;
    b8 recenter;
    b8 quit;
    b8             record_toggle;
    b8             replay_stop;
    b8             set_controller;
    SIM_Controller controller;
    b8             set_room;
    char           room[W_MAX_NAME]; // empty for none
    b8             wipe_anchors;
    b8             wipe_cloud;       // with wipe_anchors: the user's cloud anchors too
    b8             toggle_welcome;
    b8             set_welcome_at_start;
    b8             welcome_at_start;
    b8             set_lens_mask;
    b8             lens_mask;
};

#define UI_FRAME_TIMES 300 // frames the performance tab graphs, about 3 s at 90 Hz

// The app's last frames by the wall clock, for the performance tab; never fed to the simulation
struct UI_FrameTimes
{
    f32 frame_ms[UI_FRAME_TIMES]; // from one xrEndFrame to the next
    f32 puppetxr_ms[UI_FRAME_TIMES]; // of that, the time puppetxr held its lock: its own work
    u32 count;                    // up to UI_FRAME_TIMES
    u32 next;                     // where the next frame goes, the oldest once full
    u64 mirror_presents;          // frames the mirror window presented
    f32 mirror_fps;               // its presents in the last whole second
};

// Over the frames of the last second, or all kept if fewer
struct UI_FrameStats
{
    f32 fps;
    f32 mean_ms;
    f32 max_ms;
    f32 puppetxr_mean_ms;
};

UI_FrameStats ui_frame_stats (const UI_FrameTimes& times);

// What the caller knows about recording and replay, for display
struct UI_Status
{
    b8          recording;
    u64         recorded_frames;
    const char* record_path;
    b8          replaying;
    b8          replay_finished;
    u64         replay_frame;
    u64         replay_total;
    u32         divergences;
    u64         first_divergence;
    const char  (*rooms)[W_MAX_NAME]; // the rooms that can be selected
    u32         rooms_count;
    const char* room;                 // the loaded room, empty for none
    u32         anchors_stored;       // in the app's local store
    Pose        room_from_stage;      // where the stage origin lies in the room
    b8          keyboard_exists;      // the app's XR_META_virtual_keyboard
    b8          keyboard_visible;
    b8          keyboard_shifted;
    b8          show_welcome;         // the welcome overlay, drawn even when the panels are hidden
    b8          welcome_dismissed;    // the user chose not to see it at start
    const char* keyboard_context;     // the end of the app's text before the cursor
    b8          lens_mask;            // drawn and given to the app
    b8          mouse_captured;       // the mirror window holds the mouse
    const UI_FrameTimes* frame_times; // null shows no performance numbers
};

UI_Context* ui_create          (const UI_CreateInfo& info);
void        ui_destroy         (UI_Context* ui);

void        ui_handle_raw_event(const UI_Context* ui, const OS_RawEvent& event);
b8          ui_wants_mouse     (const UI_Context* ui);
b8          ui_wants_keyboard  (const UI_Context* ui);
void        ui_set_captured    (UI_Context* ui, b8 captured); // the mirror window holds the mouse: no mouse or key reaches the panels
b8          ui_wants_click     (const UI_Context* ui, f32 x, f32 y); // a window of the last frame that takes the mouse lies under window pixel (x, y), or a popup is open
void        ui_release_targets (UI_Context* ui); // drops the framebuffers of the images drawn into; D3D makes no new window swapchain while they hold the old one
void        ui_set_probe       (UI_Context* ui, b8 enabled);
void        ui_probed_mouse    (const UI_Context* ui, f32& out_x, f32& out_y); // where the last probed frame had the mouse                           // record where each labelled item is in every frame, for tests
b8          ui_find_item_rect  (const UI_Context* ui, const char* label, f32& out_x0, f32& out_y0, f32& out_x1, f32& out_y1); // its rectangle in window pixels, for sliders
b8          ui_find_item       (const UI_Context* ui, const char* label, f32& out_x, f32& out_y, b8* out_hovered = nullptr); // centre in window pixels of the first item so labelled in the last probed frame, and whether the mouse was on it

// Builds and records the panels into `target`, which must be in TransferDst layout and ends in Present layout
// The REC or REPLAY badge and the welcome overlay are drawn even when the panels are hidden
UI_Requests ui_draw            (UI_Context* ui, SIM_Sim* sim, R_CmdBufferHandle cmd, R_ImageHandle target, b8 show_panels, SIM_MirrorView& mirror_view, const UI_Status& status);
// ui_draw in two halves: the frame is built on the window thread, where the window calls it makes run,
// and recorded later on the thread of the app's queue; a render records the frame built last
UI_Requests ui_build           (UI_Context* ui, SIM_Sim* sim, b8 show_panels, SIM_MirrorView& mirror_view, const UI_Status& status);
void        ui_render          (UI_Context* ui, R_CmdBufferHandle cmd, R_ImageHandle target);
