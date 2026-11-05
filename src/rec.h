#pragma once

#include "sim.h"
#include "world.h"

// The version this build writes. Readers for older versions are kept forever once puppetxr
// reaches 1.0; before that, a layout change may drop them.
#define REC_FORMAT_VERSION 1
#define REC_ANCHOR_OPS     4 // OXR_AnchorOp::Count, which this layer cannot see
#define REC_FRAME_OPS      4 // OXR_FrameOp::Count

// An injected fault: the next `remaining` calls return `result`
struct REC_Fault
{
    i32 result;    // XrResult
    u32 remaining;
};

struct REC_Writer;
struct REC_Recording;

// What a replay needs besides the frames: the parts of the config an app observes and
// the simulator state before the first frame
struct REC_Header
{
    u32            format_version; // filled on load
    u32            writer_version; // PUPPETXR_VERSION of the build that recorded it
    SIM_Controller controller;
    u32            eye_width;
    u32            eye_height;
    f32            refresh_rate_hz;
    i64            display_period_ns;
    f32            fov_degrees;
    f32            ipd;
    Vec2           stage_bounds;
    Pose           aim_offset;
    Pose           palm_offset;
    char           room[SIM_MAX_NAME]; // empty when recorded without a room
    u64            room_hash;          // w_content_hash of the room, 0 without one
    SIM_Snapshot   initial;
    Pose           room_from_stage;    // where the stage lay in the room
    u32            anchor_latency_frames;
    u32            anchor_localize_frames;
    REC_Fault      anchor_faults[REC_ANCHOR_OPS]; // pending when the recording started
    f32            hand_tracking_fov_degrees;
    REC_Fault      frame_faults[REC_FRAME_OPS]; // pending when the recording started
};

// Something the simulator read from outside the process during the recording, such as another
// user's cloud anchors; a replay takes these in order instead of reading again
enum class REC_ExternalKind : u8
{
    CloudAnchors = 1,
    Adverts      = 2,
};

struct REC_External
{
    REC_ExternalKind      kind;
    u64                   frame_index; // the sim frame it was read in
    Array<W_StoredAnchor> anchors;     // CloudAnchors
    Array<W_Advert>       adverts;     // Adverts
};

void           rec_header_from_config (const SIM_Config& config, const SIM_Snapshot& initial, REC_Header& out_header);
void           rec_apply_header       (const REC_Header& header, SIM_Config& config);

REC_Writer*    rec_writer_create      (const char* path, const REC_Header& header, const W_Store* store = nullptr); // the anchor store as the recording starts
b8             rec_writer_append      (REC_Writer* writer, u64 frame_index, u64 state_hash, const SIM_FrameState& state); // flushed, so a crash keeps every frame
b8             rec_writer_append_external (REC_Writer* writer, const REC_External& external);
u64            rec_writer_frame_count (const REC_Writer* writer);
b8             rec_writer_close       (REC_Writer* writer); // writes the end marker; false if any write failed

// Loading validates every chunk. A truncated or corrupt file keeps the frames before the
// first bad chunk; a bad header or an incompatible version fails the load.
REC_Recording* rec_load               (const char* path);
REC_Recording* rec_load_from_memory   (const u8* data, usize size);
void           rec_release            (REC_Recording* recording);
const REC_Header& rec_header          (const REC_Recording* recording);
u64            rec_frame_count        (const REC_Recording* recording);
b8             rec_is_complete        (const REC_Recording* recording); // the end marker was found and matches
u32            rec_unknown_inputs     (const REC_Recording* recording); // inputs in the file this build does not know, their values are dropped
u32            rec_unknown_devices    (const REC_Recording* recording);
b8             rec_read_frame         (const REC_Recording* recording, u64 index, u64& out_frame_index, u64& out_state_hash, SIM_FrameState& out_state);
const W_Store& rec_store              (const REC_Recording* recording);
u64            rec_external_count     (const REC_Recording* recording);
b8             rec_read_external      (const REC_Recording* recording, u64 index, REC_External& out_external); // replaces the arrays' contents
void           rec_external_release   (REC_External& external);
