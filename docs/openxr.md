# OpenXR layer

`src/oxr.cpp` implements every `xr*` entry point. `src/oxr_vulkan.cpp` (step 6) adds
the Vulkan graphics binding and is the only other file including Vulkan headers;
`src/oxr_gl.cpp` adds the OpenGL and OpenGL ES bindings (no GL, GLX or EGL headers: it declares
the opaque platform types `openxr_platform.h` refers to).
Monado (https://gitlab.freedesktop.org/monado/monado) was the reference for
semantics; nothing is copied from it.

## Entry and dispatch

- `xrNegotiateLoaderRuntimeInterface` is the only exported OpenXR symbol (see
  `cmake/puppetxr.map`). It validates the loader structs and hands out
  `oxr_xrGetInstanceProcAddr`.
- `OXR_FUNCTIONS` is an X-macro listing every function with its availability
  condition (`true` for the three pre-instance functions, `inst` for core, an
  extension or version expression otherwise). `xrGetInstanceProcAddr` returns
  `XR_ERROR_FUNCTION_UNSUPPORTED` when the condition is false and
  `XR_ERROR_HANDLE_INVALID` for instance functions queried without an instance.
- `OXR_EXTENSIONS` is the X-macro of supported extensions; it generates the
  enumeration list, the create-time check and the `OXR_Extensions` flags.

## API trace

`trace` in the config or `PUPPETXR_TRACE` (docs/config.md) logs calls: `*` (or `1`, `true`) every
function, else a comma-separated list of names with `*` wildcards (`xrDiscover*,*Space*`). A whole
trace logs a hundred calls a frame; a filter keeps the app at its speed.

- `xrGetInstanceProcAddr` hands out a wrapper for each matching function (`oxr_trace_wrap`,
  `src/oxr_trace.cpp`); unmatched functions are not wrapped and cost nothing. The filter is read
  by every lookup without an instance (the loader's, before it creates one) and again by
  `xrCreateInstance`.
- A call is one line, `trace: xrName(args) -> XrResult {outputs}`, logged when it returns: inputs
  with enum names, handles in hex, paths as their strings while one instance lives, UUIDs as hex,
  structs with their `next` chains and base headers by their type, arrays up to
  `TRACE_MAX_ELEMENTS` (128) and then `, ... N more`, nesting up to `TRACE_MAX_DEPTH`; outputs only on success. An array of base
  headers (swapchain images) prints its count and first element, as the derived type's size is
  unknown. Lines go whole to the log file, stderr and the log callback (`os_log_text`); the log
  history keeps their first `OS_LOG_LINE_LENGTH` bytes.
- `tools/gen_trace.py` writes the wrappers and printers from the pinned registry (`xr.xml`) into
  `build/<preset>/generated/oxr_trace_gen.h` at build time. Functions and structs of platform
  headers (graphics bindings, Win32, timespec) are not traced: their headers stay in their two
  files (AGENTS.md, rule 6); structs of them print their type. The previews puppetxr declares itself
  (dynamic objects, event channels) have hand-written wrappers in `oxr_trace.cpp`. A function
  without a wrapper logs `trace: xrName is not traced` when handed out.

## Runtime object and locking

One `OXR_Runtime` per process (`runtime()`), created lazily, never freed. It owns one
`SparseArray` per handle type, the `SIM_Sim` of the live instance and a mutex. Every
entry point takes the mutex (`OXR_Lock`). `xrWaitFrame` releases it while sleeping.

Only one `XrInstance` may be alive; a second `xrCreateInstance` returns
`XR_ERROR_LIMIT_REACHED`. Creating the instance loads the config (`docs/config.md`),
initializes the platform and creates the simulator; destroying it tears all of that
down, so the CTS pattern of creating and destroying instances repeatedly works.

## Startup timing

A run's startup is logged once, at its first frame with layers (`startup_log`): the time since
the first `xrCreateInstance`, how much of it puppetxr's calls took (the lock held, counted in
`oxr_lock`/`oxr_unlock`), puppetxr's big steps (room loading over all instances, the room's upload
to the GPU, window, graphics device, compositor, UI) and when the session was created, began
running, waited its first frame and ended it. The rest is the app's. Destroying the instance
after that frame starts the next run, so every Play of an editor is measured.

```
Startup: first frame with layers 2108 ms after the first xrCreateInstance; puppetxr's calls took 250 ms of it, the app the rest
Startup in puppetxr: rooms 68, room upload 16, window 23, graphics device 1, compositor 22, UI 1 ms
Startup milestones: session created at 235 ms, running at 1098, first xrWaitFrame at 1860, first xrEndFrame at 1867
```

That is Unity 6000.0.66f2's editor playing the MRUK start scene: the session is ready at once,
Unity begins it 860 ms later and waits its first frame another 760 ms after that.

## Handles

`encode_handle(type, value)` puts an `OXR_HandleType` tag in the top byte of the
generational `SparseArray` handle. `decode_handle` checks the tag and
`SparseArray::try_get` checks slot and generation, so any `u64` the app passes
resolves either to the live object of the right type or to
`XR_ERROR_HANDLE_INVALID`. Handles are never dereferenced as pointers.

Destruction cascades: instance -> sessions -> spaces and swapchains; instance ->
action sets -> actions.

## Validation order inside an entry point

1. Handles (`OXR_GET_*`), 2. required pointers and `type` fields (`OXR_CHECK`,
`OXR_CHECK_TYPE`), 3. state checks in the order the spec lists its error codes
(for example running before call order before time). Two-call enumerations go
through `two_call()`. Unknown `next` structs are ignored.

A validation failure goes through `OXR_CHECK` or `OXR_FAIL(reason)`, which log the failed
condition with `LOG_ERROR` once per call site (`OXR_LOG_ONCE`, `os_claim_once`), so an app that
repeats a bad call every frame shows up in the log once. `xrEndFrame` also logs its first
rejected layer once, with its type and result.

`xrPollEvent` accepts any `type` in the buffer because apps commonly reuse the
buffer without resetting it. The queue holds `OXR_MAX_EVENTS`. On overflow the oldest event
is dropped and counted in `OXR_Instance::events_lost`; the next poll returns
`XrEventDataEventsLost` first, which is where the dropped events were (spec 2.22.1).

`XR_EXTX1_event_channel` (Meta SDK v85 preview, spec version 3; types in `src/oxr_internal.h`
as `OXR_*EXTX1`). The header leaves the semantics open; puppetxr's:

- One queue holds every event, each tagged with the channel it went to when it was queued
  (`OXR_Event::channel`, 0 for `xrPollEvent`): its session's target, else the instance's
  default channel, else none. Changing the default or destroying a channel never moves queued
  events. `xrPollEventChannelEXTX1` returns the oldest event of its channel, `xrPollEvent` the
  oldest untagged one; both run the same per-poll work (reference space changes, the control
  server). `XrEventDataEventsLost` only comes from `xrPollEvent`.
- `OXR_EventChannelTargetEXTX1` chained to `XrSessionCreateInfo` sends the session's events
  (those that name it) to that channel; a channel of another instance or a destroyed one is
  `XR_ERROR_HANDLE_INVALID`. No other create info takes a target.
- `xrSetDefaultEventChannelEXTX1` with `XR_NULL_HANDLE` sends new events to `xrPollEvent` again.
- `xrDestroyEventChannelEXTX1` drops the events still in the channel; sessions targeting it and
  a default naming it fall back to the default and to `xrPollEvent`. Destroying the instance
  destroys its channels. At most `OXR_MAX_EVENT_CHANNELS` per instance, then
  `XR_ERROR_LIMIT_REACHED`.
- `xrSelectEventChannelEXTX1` needs `flags` 0, at least one channel, all of the instance; it
  returns the index of the first listed channel holding an event, else waits in steps of
  `OXR_FRAME_GATE_POLL_NS` with the lock released until `timeout` (0 does not wait,
  `XR_INFINITE_DURATION` waits forever) and returns `XR_TIMEOUT_EXPIRED`. Events arise in the
  app's calls, so only another thread of the app ends a wait early.

## Session state machine

```
create:   IDLE, READY
begin:    SYNCHRONIZED, VISIBLE, FOCUSED        (immediately; no compositor wait)
request exit: VISIBLE, SYNCHRONIZED, STOPPING
end:      IDLE, then EXITING (exit requested) or READY
```

Without a graphics binding, `xrCreateSession` needs `XR_MND_headless`, otherwise it
returns `XR_ERROR_GRAPHICS_DEVICE_INVALID`. Headless sessions reject swapchain
creation with `XR_ERROR_FEATURE_UNSUPPORTED`, report no swapchain formats, report
`shouldRender = false` and reject layers in `xrEndFrame`.

## Frame loop

- `xrWaitFrame`: blocks (polling every 100 us with the lock released) while a frame
  was waited and not yet begun (spec 10.4.1), also while an older frame is begun and not yet
  ended, as Unreal's game thread runs a frame ahead of its RHI thread. Engines begin it on another thread; with no
  such thread that is a deadlock, so after `OXR_FRAME_GATE_TIMEOUT_NS` (1 s) it logs a
  warning, which also reaches `puppetxr_set_log_callback`, and goes on as if the frame had been
  begun. A paused simulator holds `xrWaitFrame` in the control server's poll hook before the
  gate, so an agent stepping slowly never trips the timeout. Then `sim_begin_frame` + `sim_commit_frame` advance
  the simulator exactly once per frame, and the predicted display time is
  `sim_predicted_display_time`. In realtime clock mode it then sleeps the rest of
  the display period.
- `xrBeginFrame`: `XR_ERROR_CALL_ORDER_INVALID` without a new wait; `XR_FRAME_DISCARDED`
  when the previous begun frame was never ended.
- `xrEndFrame`: validates time, blend mode (`OPAQUE` only), layer count and layers. A layer
  may name a space the app destroyed since its session's last `xrEndFrame`: it is composited
  where that space was, with a warning. Unity's OpenXR plugin replaces its app space within a
  frame and ends that frame with the destroyed one; Quest composites it, and rejecting it made
  every later frame of such an app fail. A space destroyed before is `XR_ERROR_HANDLE_INVALID`
  (`OXR_RetiredSpace`, at most `OXR_MAX_RETIRED_SPACES`).

## Frame faults

`oxr_set_frame_fault(OXR_FrameOp op, i32 result, u32 count)`, for testing how an app handles
failing frame calls (C API `set_frame_fault`, `puppetxrctl frame fault`). The next `count` calls of
`op` (`Wait`, `Begin`, `LocateViews`, `End`) that pass their own checks (handles, structs,
running, call order, times, blend mode, layer count) return `result` without doing their work;
count 0 clears. Faults are runtime wide, may be set without an instance, and are cleared when the
live instance is destroyed (as anchor faults are).

- Allowed results are the errors the registry lists for the call (`frame_fault_allowed`): all four
  take `XR_ERROR_VALIDATION_FAILURE`, `RUNTIME_FAILURE`, `HANDLE_INVALID`, `INSTANCE_LOST`,
  `SESSION_LOST`; wait, begin and end `SESSION_NOT_RUNNING`; begin and end `CALL_ORDER_INVALID`;
  locate and end `TIME_INVALID`; locate `SIZE_INSUFFICIENT`, `VIEW_CONFIGURATION_TYPE_UNSUPPORTED`;
  end `ENVIRONMENT_BLEND_MODE_UNSUPPORTED`, `LAYER_INVALID`, `LAYER_LIMIT_EXCEEDED`,
  `POSE_INVALID`, `SWAPCHAIN_RECT_INVALID`. Anything else, success codes included, is
  `OXR_ControlResult::Invalid`.
- A fault returns only the code: the session state does not change and no event is queued, also
  for `SESSION_LOST` or `INSTANCE_LOST`.
- `xrWaitFrame`: the simulator does not step, no frame is waited and the predicted display time
  stays; the next wait gives the frame the failed one would have.
- `xrBeginFrame`: no frame is begun, so `xrEndFrame` is `XR_ERROR_CALL_ORDER_INVALID`. The waited
  frame may be begun by a second call, and the next `xrWaitFrame` does not wait for it to be begun
  (`OXR_Session::begin_failed`), so a single threaded app that goes back to waiting runs on.
- `xrLocateViews`: `viewCountOutput` 0 and `viewStateFlags` 0. The capacity query (capacity 0)
  counts as a call.
- `xrEndFrame`: the frame stays begun and nothing is composited; ending it again works, and the
  next `xrBeginFrame` returns `XR_FRAME_DISCARDED`.
- `PUPPETXR_ERROR_BUSY` (`OXR_ControlResult::Busy`) while recording or replaying: the faults pending
  when a recording starts are in its header and a replay sets them again, so it fails
  the same calls.

## Spaces

Every locate samples the simulator at the requested time first (`sim_sample`):

- The sim keeps `SIM_HISTORY_SAMPLES` committed frames (31 periods, at least the 50 ms of
  spec 2.16). The live state is the newest sample, at the predicted display time.
- Between samples, device poses and velocities are interpolated; older times clamp to the
  oldest sample. Later times extrapolate the live state by its velocities, up to
  `SIM_MAX_PREDICTION_NS`. Apps that locate at `predictedDisplayTime` see the live state
  exactly.
- Reference space origins switch at their change time and are never interpolated.

`locate_world(space, sample)` computes a space's pose in the simulator's frame: stage space
without tracking drift (`docs/anchors.md`, Tracking drift). Locating one space in another cancels
the frame out, so drift shows only between the tracking spaces and everything else.

- Reference spaces come from `sim_sample_reference_space_pose` times the creation offset; all
  but VIEW lie at the tracking drift.
- Action spaces come from the bound hand pose (grip, aim or grip_surface) of the first
  active side the space allows.
- Velocities are those of the space origin, so the device velocity plus ω × offset.

`locate_relative` turns two world poses into the result and the flags. They are valid when
both are valid and tracked when both are tracked. Velocities are relative to the moving and
rotating base, in base coordinates. `xrLocateViews` uses the same path with
`sim_sample_view`. `LOCAL_FLOOR` is available for 1.1 instances and with `XR_EXT_local_floor`.

Recentering (spec 7.1): `sim_recenter` and replayed origin changes bump
`sim_local_origin_change`'s generation.

- The change time is the predicted display time of the frame being built. Between frames
  the app already holds that frame's time, so it is the next frame's time instead.
- Locates before the change time see the previous origin.
- `queue_reference_space_changes` runs at every `xrPollEvent`. It queues
  `XrEventDataReferenceSpaceChangePending` for LOCAL and, when available, LOCAL_FLOOR for
  every session that has not seen the new origin, with the new origin in the old one.

STATIONARY (`XR_EXT_stationary_reference_space`, preview `XR_EXTX2_stationary_reference_space`):

- Offered by `xrEnumerateReferenceSpaces` and `xrCreateReferenceSpace` only with either extension
  enabled. Its origin is `SIM_Sim::stationary_origin`, the LOCAL origin at `sim_create` (yaw only,
  so gravity aligned); recentering never moves it and queues no change event for it.
- The simulator never loses tracking, so there is one generation: `xrGetStationaryReferenceSpaceGenerationIdEXT`
  (and the preview's `xrGetStationaryReferenceSpaceIdEXTX2`, same structs and type values) returns
  the same UUID in every run, and `XR_ERROR_SESSION_NOT_RUNNING` outside a running session.
- A restarted app sees the same origin as long as the config's start pose is the same, which is
  how the spec's persistence across app restarts holds.

## Actions

- Names are validated like paths (`name_valid`), duplicates per instance (sets) or
  per set (actions) are rejected, and nothing can be created or suggested after
  `xrAttachSessionActionSets`.
- Suggestions are validated against every profile of the registry,
  `src/oxr_profiles.inc`, generated by `tools/gen_profiles.py <xr.xml> src/oxr_profiles.inc`
  from the registry of the pinned OpenXR SDK (`third_party/openxr/specification/registry/xr.xml`).
  Rerun it when the SDK pin moves. Each profile and binding path carries an availability
  (OR of feature masks); `OXR_Instance::spec_features` holds the api version and the
  enabled extensions. An unknown or unavailable profile, or a path that is neither an
  available binding nor its parent (`.../input/trigger` for `.../input/trigger/value`,
  spec 6.3.2), is `XR_ERROR_PATH_UNSUPPORTED`. Four paths outside the registry stand for the
  registry path they name (`oxr_binding_aliases`): OVRPlugin binds `trigger/proximity_fb` and
  `thumb_fb/proximity_fb` on `meta/touch_controller_plus` and `trigger_haptic_fb` and
  `thumb_haptic_fb` on `facebook/touch_controller_pro`, which Meta's runtime accepts; rejected,
  its Touch Plus and Pro suggestions failed whole and its input stayed dead.
- Suggestions for emulated profiles are stored in `OXR_Instance::suggested`; those for
  other profiles are accepted and dropped. Paths valid in the registry that the
  simulator does not emulate (the Meta swipe and tap microgestures) stay unbound.
- The simulated hardware (`SIM_Controller`, both hands) decides which profiles can be
  active (`hardware_profiles`): Meta Touch Plus serves `meta/touch_controller_plus`, Touch Pro
  `facebook/touch_controller_pro`, and both then `oculus/touch_controller` and
  `khr/simple_controller`; Oculus Touch serves the last two, KHR simple only
  `khr/simple_controller`. Each Touch profile has its OpenXR 1.1 name, preferred over the
  extension name on the same hardware: `meta/touch_plus_controller`, `meta/touch_pro_controller`
  and `meta/touch_controller_quest_2` (served by every Touch generation). They share the tables
  of their extension profile; the registry decides which paths each accepts (`trigger_curl/value`
  in 1.1, `trigger/curl_fb` in the extension). The Plus and Pro profiles need their extension, so an app without
  it gets Touch on that hardware. `XR_FB_touch_controller_proximity` adds `trigger/proximity_fb`
  and `thumb_fb/proximity_fb` to Touch; the core 1.1 `trigger/proximity` and
  `thumb_resting_surfaces/proximity` read the same inputs. A hand tracked without its
  controller (`SIM_HandSource::Hand`) serves `ext/hand_interaction_ext` (with
  `XR_EXT_hand_interaction`), then `khr/simple_controller` with `select` as the pinch.
  Each hand has its own current profile (`OXR_Session::profiles`), so one hand can hold its
  controller while the other is tracked. At every sync, per hand, the first of its profiles
  the attached sets suggested bindings for becomes current, else none; bindings resolve at
  attach already, but the profile only becomes current, with its event, at the first sync.
  A profile change pushes `XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED`; a hardware or
  source change that keeps the profiles rebinds silently. Profile tables are keyed
  by profile and hardware (`profile_components`): `select/click` reads the trigger on
  Touch hardware and the `select/click` button on simple hardware.
- `xrSuggestInteractionProfileBindings` and `xrCreateAction` refuse an action set once it was
  attached to any session (`OXR_ActionSet::ever_attached`, kept after the session), not the whole
  instance: editors keep their instance and build new action sets for each play session.
- `bind_actions` resolves each suggestion of a hand's profile into `OXR_Action::sources[side]`,
  dropping paths of the other hand and sources whose kind does not fit the action type
  (`source_compatible`). `ready_ext` components (`OXR_SourceKind::Ready`) read true while the hand
  is tracked; `pinch_ext/pose`, `poke_ext/pose` and `palm_ext/pose` exist on every hand profile
  (`SIM_HandPose::Pinch`, `Poke`, `Palm`).
- Priority (spec 11.2): every `xrSyncActions` runs `suppress_by_priority` first. A source
  is suppressed when an action of a higher priority set that is active on the same side
  binds the same input source, compared by `OXR_BoundSource::identifier` (the path
  without its component, so `trigger/value` and `trigger/touch` collide). Suppressed
  sources count as unbound for state, poses, haptics and `xrEnumerateBoundSourcesForAction`.
- `xrSyncActions` marks the sets in the sync info active (per side when a subaction
  path is given), returns `XR_SESSION_NOT_FOCUSED` and leaves everything inactive
  when the session is not focused, and recomputes every attached action: per side
  the longest source value wins, the combined (null subaction) state is the longest
  active side, `changedSinceLastSync` and `lastChangeTime` follow the spec.
- Boolean actions on float sources use a 0.5 threshold; `khr/simple_controller`'s
  `select/click` reads the trigger value that way.
- Binding modifiers (`XR_KHR_binding_modification`): `xrSuggestInteractionProfileBindings`
  checks every modification before keeping any and stores them per profile
  (`OXR_Instance::modifiers`); `bind_actions` copies the matching one into each bound source
  (`OXR_BindingModifier`), which also keeps its state across syncs (`bound_value`).
  - `XR_VALVE_analog_threshold`, in the modifications or chained directly (deprecated): a
    boolean action on a float binding is true from `onThreshold` until the value falls below
    `offThreshold`. On or off out of [0, 1], or on below off, is a validation failure; an
    action and binding pair the suggestion lacks is `XR_ERROR_PATH_UNSUPPORTED`.
  - `XR_EXT_dpad_binding` (needs the former): `.../thumbstick/dpad_up` and the other directions
    resolve to the thumbstick (`resolve_dpad_binding`) and share its input source for priority.
    A direction is on outside `centerRegion` within `wedgeAngle / 2` of its axis; a sticky dpad
    latches the regions it entered until the stick returns to center. Without a struct for the
    input and action set the spec defaults apply (center 0.5, wedge pi/2, not sticky). Ranges as
    the spec lists and one struct per input and set, else a validation failure.
  - `onHaptic` and `offHaptic` play on the side's main output when the boolean changes;
    only `XrHapticVibration`, the other haptic types are accepted with a warning.
- Haptics forward to `sim_apply_haptic` for every bound side and output (`OXR_ProfileComponent::haptic`:
  `/output/haptic`, and Touch Pro's `haptic_trigger_fb` and `haptic_thumb_fb`, `haptic_trigger` and
  `haptic_thumb` in 1.1).
  `XrHapticAmplitudeEnvelopeVibrationFB` (with `XR_FB_haptic_amplitude_envelope`) goes to
  `sim_apply_haptic_envelope`: at least one finite sample and a duration of 0 or more, else
  `XR_ERROR_VALIDATION_FAILURE`. The sim keeps up to `SIM_MAX_HAPTIC_SAMPLES` per hand and
  output in fixed arrays, resamples longer envelopes linearly, and `sim_get_haptic` reports the
  amplitude interpolated at the current sim time, so it is the same on every run.
- `XrHapticPcmVibrationFB` (`XR_FB_haptic_pcm`) goes to `sim_apply_haptic_pcm`: the buffer is
  resampled to `SIM_HAPTIC_PCM_RATE` (2000 Hz) into the same per-output arrays and the absolute
  value is the amplitude. `append` adds to a running PCM effect, which ends when the arrays are
  full. `samplesConsumed` is the minimum over the bound outputs, 0 when nothing is bound.
  `xrGetDeviceSampleRateFB` reports 2000 Hz.
- `XrHapticParametricVibrationEXT` (`XR_EXT_haptic_parametric`, and `XR_EXTX1_haptic_parametric`,
  the preview the Meta SDK uses: same struct type values and layout, its
  `xrHapticParametricGetPropertiesEXTX1` is the EXT function) goes to
  `sim_apply_haptic_parametric`: up to `SIM_MAX_HAPTIC_POINTS` amplitude and frequency points,
  frequency mapped linearly from 0..1 to the effect's range within 50..500 Hz; transients are
  validated and ignored. A stream starts with `NONE` or `FIRST_FRAME`; `INTERMEDIATE_FRAME` and
  `LAST_FRAME` append to the running stream (else `XR_ERROR_VALIDATION_FAILURE`), and a stream
  holds its last value for `SIM_HAPTIC_HOLD_NS` (50 ms) waiting for the next frame. Frequency
  points and transients may reach past the amplitudes in `FIRST` and `INTERMEDIATE` frames, not
  in `NONE` and `LAST` ones. Later frames go on in stream time: their first amplitude and
  frequency points are not earlier and their first transient is later than the stream's
  (`sim_haptic_stream_end`), and the last frame's amplitudes outlast every frame's frequencies
  and transients. A rejected `LAST_FRAME` still stops the stream it meant to end.
  `xrHapticParametricGetPropertiesEXT` reports a 20 ms ideal frame period and 50..500 Hz.

## Hand tracking

`XR_EXT_hand_tracking`, `XR_FB_hand_tracking_aim`, `_capsules`, `_mesh`,
`XR_EXT_hand_tracking_data_source`, `XR_EXT_hand_joints_motion_range`, `XR_EXT_hand_interaction`,
`XR_META_simultaneous_hands_and_controllers`, `XR_META_hand_tracking_microgestures`,
`XR_META_hand_tracking_wide_motion_mode` (Meta preview) and `_mode2`,
`XR_META_hand_tracking_frequency_hint`, `XR_META_hand_tracking_unextrapolated_poses`. The FB
extensions, the data source, the motion range, simultaneous hands and controllers, wide motion,
the frequency hint and unextrapolated poses need `XR_EXT_hand_tracking`; microgestures need
`XR_EXT_hand_interaction`
(`XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED`).

- Each hand is tracked by its controller or by hand tracking (`SIM_HandSource`, key T,
  `puppetxrctl hand`, the C API, recorded per frame). A tracked hand has only
  `trigger/value` (pinch), `squeeze/value` (grab) and `menu/click` (menu gesture).
- `sim_hand_joints` (`src/sim.cpp`, "Hand model") builds the 26 joints by forward kinematics
  from the grip pose: fixed bone lengths of an adult right hand, mirrored for the left; the
  palm joint sits where a held handle is, so joints and controller agree. Pinch curls the index
  to 0.6 and swings the thumb onto its tip (angles fitted offline, `sim_thumb_pinch`); grab curls
  middle, ring and little; both make a fist, grab alone points. Joint axes follow the spec: -Z
  along the bone to the tip, +Y out of the back of the hand.
- `xrLocateHandJointsEXT` samples the grip at the requested time like an action space; joint
  velocities are the hand's, with the lever arm to each joint. Without
  `XrHandTrackingDataSourceInfoEXT` a tracker reports only an unobstructed, tracked hand; with
  it, `CONTROLLER` also reports a hand holding its controller. `CONFORMING_TO_CONTROLLER`
  curls that hand around the handle (`SIM_HELD_GRAB`, `SIM_HELD_PINCH`); a tracked hand has
  nothing to conform to. An inactive hand reports `isActive` false and zero flags everywhere.
- FB aim: the sim's aim pose, `pinchStrengthIndex` from the trigger, `INDEX_PINCHING` from 0.9,
  `DOMINANT_HAND` for the configured primary hand, `menu/click` is `MENU_PRESSED` on the left
  and `SYSTEM_GESTURE` on the right. Capsules: one per bone from a joint to the next, the
  joint's radius. Scale: 1, or the override the app asks for (joints are not scaled).
- Hand tracking sees a tracked hand only inside a cone around the head's forward axis
  (`hands.tracking_fov_degrees`, `sim_hand_in_view`, at the sampled time). Outside it the hand's
  trackers report it inactive and its action poses are invalid. A tracker in wide motion mode
  (the preview's `XrHandTrackingWideMotionModeInfoMETA`, type 1000539000, or every tracker with
  `_mode2`) keeps reporting it, valid but not tracked. Controllers ignore the cone.
- Microgestures: `swipe_*_meta/click` and `tap_thumb_meta/click` of `ext/hand_interaction_ext`
  read `SIM_Input::SwipeLeft` .. `TapThumb`, one frame pulses (`age_microgestures`: a pulse set
  between frames is seen through the next frame, a key press in its own). The thumb swings for
  `SIM_MICROGESTURE_NS` (200 ms) after each, derived from the recorded inputs and sim time.
- Quest switches both hands between controllers and hand tracking together unless the app resumes
  simultaneous tracking; the simulator always lets each hand switch alone, so
  `xrResume/PauseSimultaneousHandsAndControllersTrackingMETA` only validate. A controller that is
  put down disappears instead of being tracked as a detached controller.
- Detached controllers (`XR_META_detached_controllers`): a hand switching to hand tracking leaves
  its controller at its grip pose (`sim_get_detached_pose`, recorded per frame). While the hand is
  tracked, `/user/detached_controller_meta/<side>` (`OXR_TopLevel::DetachedLeft`, `Right`)
  serves the hand's controller profile (`choose_profiles`), so a profile is current on at most one
  of the two paths. Its poses come from the put down pose (`sim_detached_hand_pose`, pinch and poke
  are the aim pose), its inputs read zero, its haptics play on the hand's outputs.
- Frequency hint: `xrSetHandTrackingFrequencyHintMETA` validates the hint and keeps it in
  `OXR_Session::hand_frequency_hint`; the simulated hands update every frame either way.
- Unextrapolated poses: with `XrHandTrackingUnextrapolatedPosesRequestMETA` chained,
  `xrLocateHandJointsEXT` takes the hand (joints, aim, the camera cone) from the live state at
  `sim_predicted_display_time`, the time the simulation computes it for, whatever the requested
  time; the base space is still located at the requested time. `captureTime` is that time, 0
  when the hand is not reported, and is written only when requested.
- `xrGetHandMeshFB`: `sim_get_hand_mesh`, built at `sim_create`: 19 tapered octagonal prisms,
  one per bone, every vertex skinned to its bone's joint with weight 1, in the open hand's
  wrist frame (the bind poses). Zero capacities return the counts. Only joints of Meta's 24 bone
  hand skeleton are skinned to: the index, middle and ring metacarpal prisms move with the
  wrist, and the unused blend slots repeat the joint. OVRPlugin converts the mesh to that
  skeleton and maps the palm and those metacarpals to no bone (65535), which Unreal's hand
  component fails an array check on.

## Render models

`XR_FB_render_model`: `/model_fb/controller/left` and `/right`, `/model_fb/keyboard/local` and
`/remote`, and with `XR_META_virtual_keyboard` also `/model_meta/keyboard/virtual`
(`oxr_render_model_paths`).
Properties need a prior `xrEnumerateRenderModelPathsFB` (a count query is enough), else
`XR_ERROR_CALL_ORDER_INVALID`; other paths are `XR_ERROR_PATH_INVALID`. The key encodes "XRSM",
the simulated controller type (0 for keyboards, which do not depend on it) and the path, so a key
names one model for good; loading the key
of a controller that is no longer simulated is `XR_RENDER_MODEL_UNAVAILABLE_FB` with no data,
an unknown key `XR_ERROR_RENDER_MODEL_KEY_INVALID_FB`. Controllers and the physical keyboards are
offered at glTF 2.0 subsets 1 and 2 (the lowest requested is reported), the textured virtual
keyboard at subset 2 only (`render_model_levels`); a request without one of the model's levels is
unavailable. `sim_controller_model` builds the controller from
capped cylinders (handle, head, trigger, buttons, stick) and, for Touch, a torus ring, in grip
space, the left mirroring the right; `sim_keyboard_model` a slab with five rows of box caps around
its bounding box centre, +Y up, its front at +Z (spec: a keyboard's origin is that centre).
`write_render_model` writes a node, mesh, primitive and material per part into a GLB: positions,
normals, texture coordinates (textured models only) and 16-bit indices in one buffer, then the key
times 0 and 1 and, per part with a `press`, a translation animation from rest to `press`.

`XR_EXT_render_model` with `XR_EXT_interaction_render_model` (which needs it):
- Ids: each `xrSyncActions` (`sync_render_models`) lists a model for a hand whose source is a
  controller and which has a bound source of an attached action; another controller type or a
  hand that comes back gets a new id from the session counter (spec: ids are never reused),
  and a change queues `XrEventDataInteractionRenderModelsChangedEXT`. Nothing is listed before
  the first sync. Only listed ids create models; handles of ids that left the list stay valid
  but inactive.
- Assets: the hand's `sim_controller_model` through `write_render_model` (parts named `body`,
  `trigger`, `buttons`, `ring`; no glTF animations, no extensions, so the app's glTF extension
  list changes nothing). The UUID is a hash of the GLB bytes, so a UUID always names the same
  bytes; `xrCreateRenderModelAssetEXT` accepts only UUIDs the session returned
  (`OXR_Session::render_model_uuids`).
- Animatable nodes: `trigger` (moves 8 mm along +Y with the trigger value) and `buttons` (2 mm
  along -Y while a face button or the stick is clicked); poses replace the nodes' identity
  transforms. State uses the inputs of the last device update for any display time > 0.
- Spaces: the grip pose of the model's hand, located only while the model is listed and the
  session is focused; no flags once the model is destroyed.
- Paths: the subaction path is the hand's when an action of an active set lists and binds it;
  the top-level user path is the hand's when it is among the given ones, else `XR_NULL_PATH`.

## Virtual keyboard

`XR_META_virtual_keyboard` (`OXR_VirtualKeyboard`, one per session, else
`XR_ERROR_LIMIT_REACHED`).

- Layout (`sim_vkb_layout`, `src/sim.cpp`): digits, `qwertyuiop` + backspace, `asdfghjkl` + enter,
  shift + `zxcvbnm,.`, space; 3.6 cm pitch, keys face +Z of keyboard space, about 43 x 21 cm at
  scale 1. Shift makes the next letter upper case and `,` `.` into `?` `!`.
- Model (`sim_vkb_model`): a quad per key 4 mm in front of the panel, in layout order, so key k is
  node k and animation k (a 3 mm press along -Z), then `panel` and `collision` (spec: the app's
  colliders), all showing texture 1, `metaVirtualKeyboard://texture/1?w=256&h=256&fmt=RGBA32`.
- Labels (`sim_vkb_texture`): a 256 x 256 RGBA atlas, a cell per key drawn with the built-in 5x7
  font `SIM_FONT`; shift redraws it. `xrGetVirtualKeyboardDirtyTexturesMETA` lists texture 1 once
  after creation and after every shift change; the id read clears it.
- Animation states: the keys whose pressed state changed since the last read (every key on the
  first), fraction 1 pressed, 0 released.
- Places: `CUSTOM` uses the pose in the space at the predicted display time and the scale;
  `FAR` is 0.8 m ahead of the head, 0.25 m down, tilted 20 degrees, `DIRECT` 0.35 m ahead,
  0.45 m down, tilted 45 degrees, both scale 1 and yaw only. The pose is fixed in stage space
  until the next suggestion; the keyboard space (`OXR_SpaceKind::VirtualKeyboard`) follows it and
  is lost when the keyboard is destroyed.
- Visibility: hidden at creation; a change queues SHOWN or HIDDEN at once and drops every press
  and shift. Hidden keyboards ignore input.
- Input (`xrSendVirtualKeyboardInputMETA`), per source: ray sources press the key the aim's -Z
  ray meets while `PRESSED` is set; direct sources press the key under the tip once it reaches the
  key tops. A press commits once, on the key where it started; dragging onto a key does not
  press. A direct input within 6 cm in front of (or 5 cm behind) the keys silences its side's ray
  from the next call. A direct tip pushed below a pressed key moves `interactorRootPose` back by
  the overshoot (poke limiting).
- Events: characters and space are COMMIT_TEXT, backspace and enter their own events; commits
  longer than `OXR_MAX_EVENT_TEXT` - 1 bytes are split between UTF-8 sequences.
- Typed text (`sim_type_text`, control API `type_text`, `puppetxrctl vkb type`, UI panel): queued
  for the next frame, part of the frame state and recorded; at `xrWaitFrame` it goes to
  the session's shown keyboard as commits, `\b` as backspace, `\n` as enter, else is dropped
  with a warning. At most `SIM_MAX_TYPED` bytes a frame.
- Text context: the last 255 bytes the app sent, shown in the UI and the control API.

## Spatial entities and scene

`XR_FB_spatial_entity`, `_query`, `_container`, `XR_FB_scene`, `_scene_capture`,
`XR_META_spatial_entity_discovery`, `_mesh`, `_room_mesh`. All of them read the loaded room
(`OXR_Runtime::world`, `docs/world.md`); without a room they find nothing.

- Spaces have an `OXR_SpaceKind`: `Entity` spaces point at a world entity and remember
  `world_generation`; once the room changes (any `load_room_locked`) they are stale:
  untracked, no supported components, only `xrGetSpaceUuidFB` still answers. `Anchor`
  spaces hold a fixed stage pose and survive room changes.
- Supported components (`entity_components`): every entity is locatable; planes are
  `BOUNDED_2D`, volumes `BOUNDED_3D`, rooms `ROOM_LAYOUT` and `SPACE_CONTAINER`, the
  global mesh `TRIANGLE_MESH_META`, rooms with a room mesh `ROOM_MESH_META`, everything but
  rooms has `SEMANTIC_LABELS`. A component
  is supported only when the extension defining it is enabled. Anchors are only locatable.
  Supported components start enabled; `xrSetSpaceComponentStatusFB` toggles them per
  space handle.
- Async requests finish at once: the call queues its events before returning (anchor
  created, status set, query results available then complete, discovery likewise, scene
  capture complete). Request ids come from `OXR_Instance::next_request_id`. Determinism
  needs nothing else since events follow the call order.
- Query and discovery results are held in `OXR_Runtime::queries` as entity indices and
  become spaces only when retrieved with enough capacity (two-call idiom); a retrieved
  request is gone. At most `OXR_MAX_QUERIES` unretrieved requests are kept, the oldest is
  dropped beyond that. A result is the session's live space of its entity, anchor or prop when
  there is one (`live_space`, matched by kind and UUID), a new space only once the app destroyed
  it: MRUK knows an entity by its handle and queries every second, and a new handle per query made
  its tracked keyboard leave and come back each time.
- `xrDiscoverSpacesMETA` needs a running session (`XR_ERROR_SESSION_NOT_RUNNING`) unless
  `discover_before_running` is set (docs/config.md, on with OVRPlugin); then it answers and logs
  an error.
- Labels (`scene_label`): world labels map to the FB strings. Without
  `XrSemanticLabelsSupportInfoFB`, `INVISIBLE_WALL_FACE` reads `WALL_FACE`. With it,
  invisible walls and `TABLE` (else `DESK`) follow the flags, and labels missing from
  `recognizedLabels` read `OTHER`. `xrGetSpaceSemanticLabelsFB` counts the characters without
  the terminator, as Meta's runtime does: MRUK and Meta's XrSceneModel sample build their
  string from `bufferCountOutput`, and with the terminator counted no label matched. A buffer
  of exactly that size gets no terminator, a larger one does.
- `xrGetSpaceBoundary2DFB` returns the plane polygon, or the rectangle's four corners
  counter-clockwise for planes without one. Triangle meshes are returned in entity space.
- Anchor uuids are deterministic: a fixed tag and a per-instance serial.
- `XR_META_spatial_entity_room_mesh`: `xrGetSpaceRoomMeshMETA` returns the room's
  `room_mesh` (`docs/room-format.md`) with vertices in the room entity's space; the vertex
  and face arrays follow one two-call (both capacities 0 asks for both counts, either too
  small is `XR_ERROR_SIZE_INSUFFICIENT`). Labels missing from `recognizedSemanticLabels`
  read `UNKNOWN`. `xrGetSpaceRoomMeshFaceIndicesMETA` returns a face's indices; a uuid that
  names no face of the room is `XR_ERROR_VALIDATION_FAILURE` (the spec names no error).
- `XR_EXT_future`: futures live in `OXR_Runtime::futures`, scoped to their session, and are
  ready as soon as they exist.

## Dynamic objects

`XR_META_dynamic_object_tracker` and `XR_META_dynamic_object_keyboard` are previews: only Meta's
SDK ships their header, so `oxr_internal.h` writes out the registry values and struct layouts
(`OXR_TYPE_DYNAMIC_OBJECT_*`, `OXR_DynamicObjectTracker*`). The keyboard extension needs the tracker.

- `xrCreateDynamicObjectTrackerMETA` and `xrSetDynamicObjectTrackedClassesMETA` succeed at once
  and queue their result events. Keyboards are the only class; any other is a validation error.
- With `keyboard_tracking.enabled` false (`PUPPETXR_KEYBOARD_TRACKING=0`) the create result is
  `XR_ERROR_INITIALIZATION_FAILED`, Meta's answer when the user turned the tracker off; the
  tracker can still be destroyed.
- While a tracker of the session tracks keyboards, `run_query` adds every keyboard prop
  (`docs/props.md`) after entities and anchors, for both `XR_FB_spatial_entity_query` and
  `XR_META_spatial_entity_discovery`. Its UUID is `puppetxrprop` and the prop id, its components
  Locatable, `DYNAMIC_OBJECT_DATA` and with `XR_FB_scene` Bounded2D and Bounded3D, all enabled.
- `OXR_SpaceKind::Prop` spaces follow the prop at the located time; a removed prop is not
  located. The space: +Z up out of the keys, +X to the right, +Y away from the user, at the box
  centre (the prop's pose turned -90 degrees about X). Bounds are centred: 30 x 11.2 cm in 2D,
  30 x 11.2 x 1.8 cm in 3D.
- `xrGetSpaceDynamicObjectDataMETA` reports the keyboard class for prop spaces,
  `XR_ERROR_SPACE_COMPONENT_NOT_SUPPORTED_FB` for others.

## Tracked keyboards

`XR_FB_keyboard_tracking` reports keyboard props (docs/props.md); code in `src/oxr.cpp` under
"Tracked keyboards".

- `xrQuerySystemTrackedKeyboardFB`: with the local bit, the oldest keyboard prop is the system
  keyboard: `EXISTS`, `LOCAL` and `CONNECTED`, its prop id as `trackedKeyboardId`, size
  30 x 1.8 x 11.2 cm (x right, y up, z towards the user), name "puppetxr Keyboard". No keyboard
  prop, or only the remote bit: all zero, `XR_SUCCESS`. Unknown query bits are a validation
  failure.
- `xrCreateKeyboardSpaceFB` takes the id of any keyboard prop; an id of no keyboard prop is
  `XR_ERROR_VALIDATION_FAILURE` (the spec names no error). The space
  (`OXR_SpaceKind::TrackedKeyboard`) is the prop's pose at the located time: origin at the box
  centre, +Y up, the front at +Z, the frame `/model_fb/keyboard/local` is drawn in. It is
  tracked wherever the prop is, seen or not, and not located once the prop is removed; a new
  prop is another keyboard.
- `XrSystemKeyboardTrackingPropertiesFB` reports support while the extension is enabled.
- `XR_FB_passthrough_keyboard_hands` layers still draw nothing over the keyboard.

## Spatial entity contexts

`XR_EXT_spatial_entity` (needs `XR_EXT_future`) with `XR_EXT_spatial_marker_tracking` (needs
the former) report QR props (`docs/props.md`) as markers. Types in `oxr_internal.h`
(`OXR_SpatialContext`, `OXR_SpatialEntity`, `OXR_SpatialSnapshot`), code in `src/oxr.cpp`
under "Spatial entities".

- Capabilities: only `MARKER_TRACKING_QR_CODE`, and only with marker tracking enabled. Its
  components are `BOUNDED_2D` and `MARKER`, its features `FIXED_SIZE_MARKERS` (reported
  extents become the given side length) and `STATIC_MARKERS` (accepted, changes nothing).
- Context creation checks in this order: no configs, unsupported capability, a config struct
  not matching its capability (validation failure), no components, a component the capability
  lacks, the same capability twice, a marker size not finite and positive (validation failure).
  The context exists from the async call; a cancelled or never completed future destroys it.
  Futures are ready at once.
- Found: a QR prop is tracked while either eye sees its centre, its front faces the head
  within 70 degrees and the head is at most 25 times its size away (`marker_in_view`).
  Otherwise its entity is `PAUSED`; once the prop is gone or no QR code it is `STOPPED` for
  good (`OXR_SpatialMarker::stopped`).
- Entities: discovery gives every QR prop in view without an entity one, ids from the session
  counter `OXR_Session::spatial_entity_ids` so they are unique across contexts. A context keeps
  at most `OXR_MAX_SPATIAL_ENTITIES` (64). A discovery snapshot holds all of them, filtered by
  tracking state; an update snapshot the listed entities, duplicates once.
- Recommendations: each `xrWaitFrame` (`poll_spatial_contexts`) queues one
  `XrEventDataSpatialDiscoveryRecommendedEXT` per completed context that sees a code without an
  entity; the next discovery re-arms it. Moving codes are no reason. `xrPollEvent` drops
  queued ones of destroyed contexts.
- Snapshots copy state, pose in the base space at the given time, extent and buffer id at
  creation and never change. Queries return entities having all the condition's components;
  list structs for components the condition does not name are validation failures unless it
  names none; short lists are `SIZE_INSUFFICIENT`; data of entities not tracked is not written.
- Buffers: a QR's text is a `STRING` buffer, or `UINT8` if not valid UTF-8. Buffer ids index
  `OXR_Session::spatial_texts`, which only grows, so the same id always has the same text and
  a changed text gets a new id. A buffer id not in the snapshot is `BUFFER_ID_INVALID`; the
  wrong getter a validation failure. `markerId` is always 0.
- Destroying a context destroys its entities, snapshots and pending discovery futures;
  destroying the session destroys its contexts.

## Passthrough

`XR_FB_passthrough`: one `OXR_Passthrough` per session (`XR_ERROR_FEATURE_ALREADY_CREATED_PASSTHROUGH_FB`
otherwise) and any number of `OXR_PassthroughLayer`s. Start and pause, on the feature and on
layers, are idempotent. A layer shows when its feature and itself run and its purpose is
`RECONSTRUCTION` or `PROJECTED`; a projected layer shows the room only on its geometry
instances (`place_geometry`, `docs/compositor.md`). `xrPassthroughLayerSetStyleFB` validates ranges and the mutual
exclusion of the colour maps and stores a `COMP_PassthroughStyle`. Passthrough composition
layers must have non-zero flags; their space may be null or any space of the session.
Without a room nothing is drawn. `xrEnumerateEnvironmentBlendModes` reports `OPAQUE` and
`ALPHA_BLEND`; the latter shows the room behind all layers (`docs/compositor.md`).

- **`XR_FB_passthrough_keyboard_hands`:** layers of the purposes `TRACKED_KEYBOARD_HANDS` and
  `TRACKED_KEYBOARD_MASKED_HANDS` are accepted and keep their intensities (in [0, 1], else
  `XR_ERROR_VALIDATION_FAILURE`); there is no tracked keyboard, so they show nothing.
- **`XR_META_passthrough_layer_resumed_event`:** creation with `IS_RUNNING_AT_CREATION` and
  every resume set `OXR_PassthroughLayer::resume_pending`. `passthrough_frame_events`, once per
  `xrEndFrame`, reports each pending layer whose feature runs, once.
- **`XR_META_boundary_visibility`:** suppression needs a running `RECONSTRUCTION` layer of a
  running feature (`session_shows_passthrough`), else
  `XR_BOUNDARY_VISIBILITY_SUPPRESSION_NOT_ALLOWED_META` and nothing changes. A change queues
  `XrEventDataBoundaryVisibilityChangedMETA`; when passthrough stops, the next frame restores
  the boundary with an event. `status` reports it. puppetxr draws no boundary.
- **`XR_META_passthrough_preferences`:** `DEFAULT_TO_ACTIVE` from `passthrough.default_active`
  in the config.
- **`XR_META_passthrough_color_lut`** (needs `XR_FB_passthrough`): `OXR_PassthroughColorLut`, a
  child of the passthrough, holds its elements as RGBA8 (`rgba`, RGB gets alpha 255) at a power
  of 2 resolution up to `COMP_COLOR_LUT_MAX_RESOLUTION` (64, `maxColorLutResolution`); a size
  other than resolution^3 times 3 or 4 returns `XR_ERROR_PASSTHROUGH_COLOR_LUT_BUFFER_SIZE_MISMATCH_META`,
  on create and update (the spec names both this and `VALIDATION_FAILURE` for update). The two
  colour map structs count as colour maps in the mutual exclusion; weights in [0, 1]. A layer's
  style points at the LUT data and keeps its handles (`color_luts`), counted in `users`: a
  destroyed LUT (by the app or with its passthrough) stays until no style uses it (spec), a
  session's LUTs go with it. Every data write bumps `OXR_Runtime::color_lut_generation`, and
  an update copies the new generation into the styles using the LUT, so the compositor uploads
  it again.
- **`XR_FB_triangle_mesh`:** `OXR_TriangleMesh` per session, at most
  `OXR_MAX_TRIANGLE_MESH_VERTICES` vertices and `OXR_MAX_TRIANGLE_MESH_TRIANGLES` triangles
  (`XR_ERROR_LIMIT_REACHED`). The runtime owns the vertex and index buffers the app writes,
  allocated at creation at the create-time counts. States follow the spec (`OXR_MeshState`);
  a call in the wrong state is `XR_ERROR_CALL_ORDER_INVALID`, an update call on an immutable
  mesh `XR_ERROR_VALIDATION_FAILURE`. Ending an update validates counts, finite vertices and
  indices below the vertex count (`XR_ERROR_VALIDATION_FAILURE`, the mesh keeps its state)
  and copies the buffers to `drawn_vertices`/`drawn_indices`, so the compositor shows the
  last finished update. Winding order is accepted and ignored: nothing is culled.
- **Geometry instances:** need a projected layer (`XR_ERROR_VALIDATION_FAILURE` otherwise; the
  spec leaves it open), a mesh, a base space of the session and a valid pose; at most
  `OXR_MAX_GEOMETRY_INSTANCES` per session. Destroying the mesh or the layer destroys their
  instances. `xrGeometryInstanceSetTransformFB` needs `time > 0` (`XR_ERROR_TIME_INVALID`);
  the base space is located at every frame's display time, so instances follow moving spaces
  (controllers). Mesh model matrix: translation, rotation, then `scale`.

## Environment depth and raycast

`XR_META_environment_depth`: one provider per session, created stopped; start and stop
of a provider already in that state return `XR_ERROR_UNEXPECTED_STATE_PASSTHROUGH_FB`.
One swapchain per provider: `OXR_DEPTH_IMAGES` D16 images of two layers at
`OXR_DEPTH_WIDTH` x `OXR_DEPTH_HEIGHT` (`comp_create_depth_target`; headless sessions get
`XR_ERROR_FEATURE_UNSUPPORTED`). `xrAcquireEnvironmentDepthImageMETA` needs a begun frame
and a running provider (`XR_ERROR_CALL_ORDER_INVALID`), once per frame
(`XR_ERROR_LIMIT_REACHED`, counted by `OXR_Session::frames_begun`). Every `xrEndFrame` of a
running provider renders the next image round robin from both eyes at the frame's display
time (`render_depth`, `comp_render_depth`) and leaves it in `SHADER_READ_ONLY_OPTIMAL`. The acquire
only hands out the newest one (`OXR_DepthProvider::rendered_index`, `rendered_time`) and uses no
GPU: apps acquire on any thread, often not the one owning the queue (Queue access below).
Until the first `xrEndFrame` after start it returns `XR_ENVIRONMENT_DEPTH_NOT_AVAILABLE_META`,
like a camera warming up, so an image is a frame old, as the depth of a headset's cameras is.
Depth uses an infinite projection with y up (`depth_projection`): `nearZ` 0.1, `farZ` infinity,
value `1 - near / distance`, the last row looking up; views are the eye poses at the image's
time in the acquire space and the eye fovs. Hand removal is accepted and changes nothing: there
are no simulated hands.

`XR_META_environment_raycast` (needs `XR_EXT_future`, else instance creation fails with
`XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED`): `xrCreateEnvironmentRaycasterAsyncMETA`
creates the raycaster at once and returns a ready future whose payload it is; cancelling
the future destroys it. `xrPerformEnvironmentRaycastMETA` transforms the ray into room
space and calls `w_raycast` on the global mesh, honouring the distance filter. A hit
outside both eyes' frustums is `HIT_POINT_OUTSIDE_OF_FOV` (the depth cameras see what the
eyes see), no room or no hit is `NO_HIT`. The hit pose's +Z is the surface normal and +Y
points back along the ray.

## Passthrough cameras

`XR_METAX1_passthrough_camera_data` has no header; the structs and functions in
`src/oxr_internal.h` are the layouts MR Utility Kit's shared library calls (structure types from
1000641000). Two cameras like Quest 3's (`oxr_cameras`): ids 50 (left) and 51 (right), 32 mm left
and right of the head, 2 cm below and 6 cm ahead, looking straight ahead. Extrinsics give that
offset in OpenXR axes and Camera2's `LENS_POSE_ROTATION` (1, 0, 0, 0), the sensor frame half
turned around x. Intrinsics: a 1280 x 1280 sensor, focal length 868 px, principal point at the
centre. Resolutions are Quest 3's (320 x 240 to 1280 x 960); an image is the centre of the sensor
scaled to fit, as MRUK's `CalcSensorCropRegion` assumes.

- Providers, at most `OXR_MAX_CAMERA_PROVIDERS` per session, are created stopped with a listed
  resolution; start and stop of a provider already in that state, and acquiring while stopped, are
  `XR_ERROR_CALL_ORDER_INVALID`. Headless sessions get `XR_ERROR_FEATURE_UNSUPPORTED`.
- Images are rendered in `xrEndFrame`, on the thread owning the app's queue (MRUK acquires on
  Unreal's game thread): the room and props as passthrough shows them, from the camera at the
  frame's display time (`comp_render_camera`), read back into one of `OXR_CAMERA_IMAGES` buffers.
  A frame renders on the frame nearest each camera period (`OXR_CAMERA_FRAME_INTERVAL_NS`, 60 Hz,
  the data rate Meta gives for Quest 3), counted from a schedule (`next_capture`) that falls back
  to the current frame after a pause. Rendering is deterministic: the display time and the sim state decide it.
- Acquire hands out the newest image, the same one until a newer is rendered, and holds its buffer
  until released; held buffers are never rendered into. Before the first image, acquire gives
  image 0, whose data is empty.
- The timestamp is the capture time in microseconds of the `XR_KHR_convert_timespec_time` clock,
  converted when the data is read, at the clock relation the app converts it back with. MRUK looks
  up the head pose at that time, which the sim keeps (`SIM_HISTORY_SAMPLES`), so what an image
  shows and the pose MRUK places it with agree.
- `xrEnumeratePassthroughCameraSupportedFormatsMETAX1` is not offered (unknown parameters, never
  called). The API trace prints the functions and their structs (`src/oxr_trace.cpp`, `trace_previews`).

## Extensions implemented

`XR_MND_headless`, `XR_EXT_local_floor`, `XR_KHR_locate_spaces`, `XR_KHR_vulkan_enable`,
`XR_KHR_vulkan_enable2`, `XR_KHR_opengl_enable`, `XR_KHR_opengl_es_enable`, `XR_MNDX_egl_enable`,
`XR_KHR_D3D11_enable` and `XR_KHR_D3D12_enable` (Windows builds), `XR_EXT_future`, the spatial entity and scene extensions above,
`XR_FB_passthrough`, `XR_FB_triangle_mesh`, `XR_META_environment_depth`, `XR_META_environment_raycast`,
`XR_EXT_conformance_automation`, `XR_KHR_convert_timespec_time`,
`XR_KHR_win32_convert_performance_counter_time` (Windows only), `XR_KHR_composition_layer_depth`,
`XR_KHR_visibility_mask`, `XR_KHR_composition_layer_color_scale_bias`,
`XR_EXT_composition_layer_inverted_alpha`, `XR_FB_composition_layer_image_layout`,
`XR_KHR_composition_layer_cylinder`, `XR_KHR_composition_layer_equirect`,
`XR_KHR_composition_layer_equirect2`, `XR_KHR_composition_layer_cube`.
FB and META, sections above and below: `XR_FB_touch_controller_pro`, `XR_META_touch_controller_plus`,
`XR_FB_touch_controller_proximity`, `XR_FB_haptic_amplitude_envelope`, `XR_FB_haptic_pcm`,
`XR_EXT_haptic_parametric`, `XR_EXTX1_haptic_parametric`, `XR_EXT_hand_tracking`,
`XR_FB_hand_tracking_aim`, `XR_FB_hand_tracking_capsules`, `XR_FB_hand_tracking_mesh`,
`XR_EXT_hand_tracking_data_source`, `XR_EXT_hand_joints_motion_range`, `XR_EXT_hand_interaction`,
`XR_META_simultaneous_hands_and_controllers`, `XR_META_hand_tracking_microgestures`,
`XR_META_hand_tracking_wide_motion_mode` (preview), `XR_META_hand_tracking_wide_motion_mode2`,
`XR_META_hand_tracking_frequency_hint`, `XR_META_hand_tracking_unextrapolated_poses`,
`XR_META_detached_controllers`, `XR_FB_render_model`, `XR_FB_keyboard_tracking`,
`XR_FB_display_refresh_rate`,
`XR_FB_color_space`, `XR_META_headset_id`, `XR_META_recommended_layer_resolution`,
`XR_META_local_dimming`, `XR_META_performance_metrics`, `XR_FB_space_warp`, `XR_META_tile_properties_hint`, `XR_META_boundary_visibility`,
`XR_META_passthrough_preferences`, `XR_FB_passthrough_keyboard_hands`,
`XR_META_passthrough_layer_resumed_event`, `XR_FB_composition_layer_settings`,
`XR_META_automatic_layer_filter`, `XR_FB_composition_layer_secure_content`,
`XR_FB_swapchain_update_state`, `XR_FB_swapchain_update_state_vulkan`,
`XR_META_vulkan_swapchain_create_info`, `XR_META_spatial_entity_semantic_label` (the label enum of
`XR_META_spatial_entity_room_mesh`), `XR_META_spatial_entity_room_mesh`,
`XR_EXT_stationary_reference_space`, `XR_EXTX2_stationary_reference_space` (preview),
`XR_EXTX1_event_channel` (preview), `XR_FB_composition_layer_depth_test`, `XR_FB_foveation`,
`XR_FB_foveation_configuration`, `XR_META_foveation_eye_tracked`,
`XR_FB_composition_layer_alpha_blend`, `XR_META_passthrough_color_lut`,
`XR_METAX1_passthrough_camera_data` (unpublished).
Status of every registry extension: `docs/extensions.md`.

OpenGL and OpenGL ES (`src/oxr_gl.cpp`, render backend in `docs/compositor.md`):

- Bindings: `XrGraphicsBindingOpenGLXlibKHR` (GLX) and `XrGraphicsBindingOpenGLWin32KHR` (WGL),
  both needing `XR_KHR_opengl_enable`, and `XrGraphicsBindingEGLMNDX` (`XR_MNDX_egl_enable`). The
  EGL binding names no API, so `eglQueryContext(EGL_CONTEXT_CLIENT_TYPE)` decides between
  `R_Backend::OpenGL` and `OpenGLES`; that API's extension must be enabled, else
  `XR_ERROR_GRAPHICS_DEVICE_INVALID`. The Android binding comes with that platform.
- Requirements: `xrGetOpenGLGraphicsRequirementsKHR` 3.3 to 4.6, `xrGetOpenGLESGraphicsRequirementsKHR`
  3.0 to 3.2. A session of an API whose requirements were not queried is
  `XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING`.
- Swapchains are GL textures in the app's share group (`XrSwapchainImageOpenGLKHR` or
  `...OpenGLESKHR`, the struct type of the session's API): `GL_TEXTURE_2D` for `arraySize` 1,
  `GL_TEXTURE_2D_ARRAY` above, `GL_TEXTURE_CUBE_MAP` for `faceCount` 6. Cube arrays are
  `XR_ERROR_FEATURE_UNSUPPORTED`. `xrEnumerateSwapchainFormats` lists only formats the backend
  has (no BGRA on GL), sRGB first.
- Every layer of an app swapchain is drawn with `COMP_Layer::flip_y` toggled: GL images have their
  origin at the bottom left. Cube layers are not flipped (cube face orientation is shared with Vulkan).
  D3D images have Vulkan's top-left origin and are not flipped.
- No fences per swapchain: every switch between the app's and puppetxr's context fences in both
  directions, so release, composite and the app's next rendering are ordered.
- Mirror window (`mirror_window_target`): GLX draws only into X11 windows of the app's visual, so
  an Xlib binding moves video to the x11 driver (`os_set_video_driver`, also on Wayland through
  Xwayland) and makes the window again with that visual (`os_create_window`'s `x11_visual`),
  unless it already has both. EGL bindings present to whatever window the driver has. Without a
  native window (the offscreen driver, headless) compositing goes on offscreen.

`XR_KHR_convert_timespec_time` (spec 12.11): the simulated clock runs at the rate of
CLOCK_MONOTONIC between frames but drifts from it (fixed step, clamped gaps), so both are related
at the last frame. `xrCreateInstance` and every `xrWaitFrame` store
`OXR_Runtime::clock_anchor_time` (`sim_time`) with `clock_anchor_timespec` (`os_timespec_ns`) and
`clock_anchor_monotonic` (`os_monotonic_ns`, for the performance counter). Conversions add the
difference to the anchor, so round trips are exact, times up to the next frame map forward, and
predicted display times stay ahead of a converted now. Results at or before the clock's start are
`XR_ERROR_TIME_INVALID`. Windows has no CLOCK_MONOTONIC: there timespecs are of the UTC wall clock,
which the CTS reads with `timespec_get(TIME_UTC)` and MRUK takes passthrough camera timestamps as
(Unix time for their date). Locates and other pose lookups at time 0
are `XR_ERROR_TIME_INVALID` too, unless `accept_time_zero` (docs/config.md) maps 0 to `sim_time`
(`locate_time` in `src/oxr.cpp`) for OVRPlugin. `xrCreateInstance` also
starts the realtime clock (`sim_start_clock`), so the first frame counts from the first anchor:
otherwise a slow session creation (GL compiles shaders) left the first predicted display time
before a time converted in between.

`XR_KHR_composition_layer_depth` (spec 12.17): `validate_depth_info` checks every
`XrCompositionLayerDepthInfoKHR` chained to a projection view when the extension is enabled. It
must name a released depth swapchain of the session and a rect inside it, with
0 <= minDepth < maxDepth <= 1 and positive, distinct nearZ and farZ (reversed and infinite far
are fine). Without the extension the struct is ignored like any unknown one. The compositor
uses the depth only for `XR_FB_composition_layer_depth_test` (`docs/compositor.md`, depth
composition); positional reprojection could use it later.

`XR_FB_composition_layer_depth_test`: `XrCompositionLayerDepthTestFB` on a layer's chain needs a
`compareOp` of `XrCompareOpFB` (else `XR_ERROR_VALIDATION_FAILURE`) and sets
`COMP_Layer::depth_test`, `depth_compare`, `depth_write`. Projection layers keep it only when a
view has depth infos (spec: ignored on layers without depth); passthrough layers never. The CTS
composition test `ProjectionDepth` shows what the CTS example image shows and is compared in
`cts_composition` (`docs/testing.md`).

`XR_FB_composition_layer_alpha_blend`: `XrCompositionLayerAlphaBlendFB` on a layer's chain needs
four `XrBlendFactorFB` values (else `XR_ERROR_VALIDATION_FAILURE`) and sets
`COMP_Layer::blend_custom` and `blend_factors` (`docs/compositor.md`); passthrough layers ignore
it. With the extension enabled `xrEnumerateEnvironmentBlendModes` reports only OPAQUE and
`xrEndFrame` with ALPHA_BLEND returns `XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED` (spec: the
layers' blending replaces the environment blend mode).

`XR_FB_space_warp`: `validate_space_warp_info` checks every `XrCompositionLayerSpaceWarpInfoFB`
chained to a projection view when the extension is enabled: only the FRAME_SKIP flag,
0 <= minDepth <= maxDepth <= 1, nearZ and farZ not negative or NaN and distinct (infinite and
reversed are fine), the motion vector sub-image like a colour one (`validate_sub_image`) and the
depth sub-image like the KHR depth one (`validate_depth_sub_image`). `appSpaceDeltaPose` is not
checked: the spec has no rule and CTS 1.1.63 sends all zeros. The compositor does not
extrapolate frames, so the infos are dropped; a frame that had them sets the
`compositor/spacewarp_mode` counter of `XR_META_performance_metrics` to 1.
`XrSystemSpaceWarpPropertiesFB` recommends half the view resolution. The CTS test
`XR_FB_space_warp` passes.

Layer and swapchain hints (`validate_layer`, `oxr_xrCreateSwapchain`, `src/oxr_vulkan.cpp`):
`XrCompositionLayerSettingsFB` flags must be the four FB bits, plus the automatic filter bit with
`XR_META_automatic_layer_filter` and never alone; they have no effect.
`XrCompositionLayerSecureContentFB` sets `COMP_Layer::secure` (`docs/compositor.md`).
`XrVulkanSwapchainCreateInfoMETA` adds `MUTABLE_FORMAT` and plain usage bits to the images when
`vkGetPhysicalDeviceImageFormatProperties` allows them (`r_image_flags_supported`); other bits
are `XR_ERROR_FEATURE_UNSUPPORTED`. `xrUpdateSwapchainFB` and `xrGetSwapchainStateFB` take
`XrSwapchainStateSamplerVulkanFB` (with `XR_FB_swapchain_update_state_vulkan`), validated and kept
as `OXR_SamplerState` (Vulkan defaults at creation), and `XrSwapchainStateFoveationFB`.

Foveation (`XR_FB_foveation`, `XR_FB_foveation_configuration`, `XR_META_foveation_eye_tracked`):
nothing is foveated, since the compositor shows the app's images as they are; the extensions are
accepted and validated.

- Each extension needs the one before it (`XR_FB_foveation` needs `XR_FB_swapchain_update_state`),
  else `XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED`.
- `xrCreateFoveationProfileFB` keeps an `OXR_Foveation`: level, vertical offset and dynamic from
  `XrFoveationLevelProfileCreateInfoFB`, eye tracking from
  `XrFoveationEyeTrackedProfileCreateInfoMETA` chained to it. Without a level the profile applies
  none (spec). Profiles die with their session.
- `XrSwapchainCreateInfoFoveationFB` takes both flags without effect. `XR_FB_foveation_vulkan`, the
  only way to get a fragment density map, is not offered (it needs a device extension the simulator
  does not enable), so the map flag changes nothing an app sees; OVRPlugin sets it on Vulkan.
- `XrSwapchainStateFoveationFB` copies the profile's settings into the swapchain, which keeps them
  when the profile is destroyed (spec); the state reports the profile handle as applied.
- The simulated user looks straight ahead: `xrGetFoveationEyeTrackedStateMETA` reports both
  foveation centers at (0, 0) and valid, and `supportsFoveationEyeTracked` is true.

Layer options (`validate_layer`): `layerFlags` of app layers must be a subset of the core bits,
plus `XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT` when that extension is enabled, else
`XR_ERROR_VALIDATION_FAILURE`. `XrCompositionLayerColorScaleBiasKHR` and
`XrCompositionLayerImageLayoutFB` chained to a layer are read only when their extension is
enabled; unknown image layout bits are invalid. They map to `COMP_Layer::color_scale`,
`color_bias` and `flip_y` (`docs/compositor.md`).

Curved layers (`place_curved_layer`, spec 12.7, 12.9, 12.10): `COMP_LayerType::Cylinder` and
`Sphere`, ray cast by the compositor (`docs/compositor.md`).

- **Validation:** a negative or NaN radius, a NaN angle, aspect ratio, scale or bias, or an
  invalid `eyeVisibility` are `XR_ERROR_VALIDATION_FAILURE`; without the extension the layer is
  `XR_ERROR_LAYER_INVALID`.
- **Angle ranges:** the spec gives them in member descriptions, not valid usage, so angles
  outside [0, 2 pi] and latitudes outside [-pi / 2, pi / 2] are clamped (`clamp_layer_angle`)
  and logged once as a warning, unless within 1e-4 of the bound (an app's rounded 2 pi).
- **Degenerate but valid:** a zero angle, aspect ratio or scale, or upper <= lower latitude,
  draws nothing.
- **uv_map:** u = angle * [0] + [1] with the angle around +Y, 0 at -Z, growing towards +X;
  v = vertical * [2] + [3], vertical being the height in radii (cylinder) or the latitude.
- **Equirect scale and bias:** the spec leaves them open. As the CTS uses them, scale is the
  fraction of the sphere the image covers, centered on -Z (0.25, 0.5 is 90 by 90 degrees), and
  bias is added to the texture coordinates: u = angle / (2 pi scale.x) + 0.5 + bias.x.

Display and system (`src/oxr.cpp`, section "Display and system"):

- **`XR_FB_display_refresh_rate`:** the rates are `sim_refresh_rates` (config, sorted). A request
  of another rate (0 is the configured one) calls `sim_set_refresh_rate`, which changes the
  display period from the next `xrWaitFrame` and the config a recording started later writes,
  and queues `XrEventDataDisplayRefreshRateChangedFB`. While recording or replaying, a change is
  `XR_ERROR_FEATURE_UNSUPPORTED`, since a recording has one display period.
- **`XR_FB_color_space`:** the native space is Rec.709, as the mirror is an sRGB monitor. Every
  listed space is accepted into `OXR_Session::color_space` and reported by `status`; colors are
  not converted.
- **`XR_META_headset_id`:** a fixed id, `oxr_headset_id`; without the extension the struct is
  left as the app passed it.
- **`XR_META_recommended_layer_resolution`:** the submitted sub-image extent (first view of a
  projection), `isValid` true; a display time before the last `xrWaitFrame` is
  `XR_ERROR_TIME_INVALID` (with `accept_older_frame_time`, docs/config.md, only one of a frame
  already ended), a layer type it does not know `XR_ERROR_LAYER_INVALID`.
- **`XR_META_local_dimming`:** `XrLocalDimmingFrameEndInfoMETA` on the frame end chain must be OFF
  or ON; the last one is kept and reported by `status`.
- **`XR_META_performance_metrics`:** four counters of the simulated frame loop, deterministic, per
  session (`oxr_perf_counter_paths`): `app/cpu_frametime` and `app/gpu_frametime` (ms, float)
  are both the interval between the display times of the last two ended frames (the display
  period while the app keeps up, 0 before its second frame; there are no separate CPU and GPU
  timelines); `compositor/dropped_frame_count` (generic, uint) counts `XR_FRAME_DISCARDED`
  frames since session creation; `compositor/spacewarp_mode` (generic, uint) is 1 when the last ended frame had
  `XR_FB_space_warp` infos, else 0. Querying
  with the system disabled is `XR_ERROR_VALIDATION_FAILURE`, an unknown path
  `XR_ERROR_PATH_INVALID`, a path not listed `XR_ERROR_PATH_UNSUPPORTED`. The CTS test
  `XR_META_performance_metrics` passes.
- **`XR_META_tile_properties_hint`:** validated and dropped. Entries typed
  `XR_TYPE_TILE_PROPERTIES_HINT_META` are accepted as well, since CTS 1.1.63 sends them that way.

`XR_KHR_composition_layer_cube` (spec 12.6): the swapchain must have 6 faces and the space and
swapchain belong to the session; an invalid `imageArrayIndex` or `eyeVisibility` is
`XR_ERROR_VALIDATION_FAILURE`, an invalid orientation `XR_ERROR_POSE_INVALID`. The spec leaves
the face orientation to the graphics API: the view direction in the layer's frame (`orientation`
in `space`) picks a texel as a Vulkan samplerCube would, so a map an app samples with
`texture(cube, direction)` looks the same as a layer. Swapchain layout: `docs/compositor.md`.

`XR_KHR_visibility_mask` (spec 12.23): each eye has a simulated lens.

- **Shape:** the view is the rectangle of tangents its fov spans on the z = -1 plane. An ellipse
  of `views.mask_radius` half-extents around its center cuts the corners.
- **Boundary:** `mask_boundary` walks `OXR_MASK_POINTS` directions (a multiple of 8, so the
  corners are among them). Each gives an inner point, where the ellipse or the view edge ends
  the visible area, and an outer point on the view edge.
- **Meshes:** the line loop is the inner points. The visible mesh is a fan around the center.
  The hidden mesh is the strip between inner and outer points, without zero-area triangles,
  since the CTS checks winding strictly. All are counter-clockwise.
- **No mask:** with the lens mask off (`views.lens_mask`, the default) or a radius of 0, or of
  sqrt 2 and more, the whole view is visible: the visible mesh is the view's rectangle (two
  triangles), the line loop its four corners, and the hidden mesh one triangle without area.
  The spec's empty meshes are for a view without any mask, which Meta's runtime never has; Meta's
  Unreal plugin crashed on them (`BuildOcclusionMesh` creates a zero-sized buffer).
- **Events:** switching the lens mask (UI, `puppetxrctl lens mask`) queues
  `XrEventDataVisibilityMaskChangedKHR` for both views. With the lens mask on, the compositor
  blacks out the mirror and screenshots outside it.

`XR_EXT_conformance_automation` (spec 12.32) is always advertised and acts only when enabled.

- The first successful call turns the keyboard and mouse controls off
  (`sim_set_controls_enabled`), as the spec requires.
- `xrSetInputDeviceActiveEXT` takes an emulated profile only. It switches the simulated
  hardware to that profile's own controller (simple or Touch), remembers the profile in
  `OXR_Session::automation_profile` and sets the hand active.
- The state setters resolve the input path through `resolve_binding`. They use the
  activated profile, else the session's, else the hardware's preferred one. A bool or
  float sets the input's value, a vector sets a stick, and `.../x` or `.../y` sets one
  axis. A pose or haptic path, or a value of the wrong shape, is `XR_ERROR_PATH_UNSUPPORTED`.
- `xrSetInputDeviceLocationEXT` places the named pose (grip, aim, grip_surface) in the given
  space. `sim_set_hand_pose` moves the grip under it. An unlocatable space is ignored.
- The changes are ordinary sim state, so recordings capture them.

Metal (`src/oxr_metal.cpp`, render backend in `docs/compositor.md`):

- `XR_KHR_metal_enable`, also offered as `XR_KHRX2_metal_enable`, the provisional name Unity's
  OpenXR plugin asks for; both take the same structs. Offered where the build has Metal (macOS);
  `oxr_metal.cpp` passes only pointers and builds everywhere.
- Requirements: the system default device (`r_metal_default_device`), on which apps make their
  queue. A session before the requirements call is `XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING`,
  a null command queue `XR_ERROR_GRAPHICS_DEVICE_INVALID`.
- Swapchains are `MTLTexture`s of the listed `MTLPixelFormat`s, sRGB first: 2D for one layer, 2D
  array for more, cube for six faces. No 24-bit depth.

D3D11 and D3D12 (`src/oxr_d3d.cpp`, render backends in `docs/compositor.md`):

- Offered where `r_backend_available` says the build has them (Windows). `oxr_d3d.cpp` declares
  the few D3D types `openxr_platform.h` names and builds everywhere.
- Requirements: the LUID of DXGI's first adapter (`r_d3d_adapter_luid`), feature level 11_0. A
  session before the requirements call is `XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING`. A
  device on another adapter only logs a warning: the simulator renders anywhere, and hybrid
  laptops put apps on either GPU.
- Bindings: `XrGraphicsBindingD3D11KHR` (device), `XrGraphicsBindingD3D12KHR` (device and a
  direct queue); a null device or queue is `XR_ERROR_GRAPHICS_DEVICE_INVALID`. The runtime takes
  a reference on both for the session.
- Swapchains are typeless textures (`R8G8B8A8_TYPELESS` for `R8G8B8A8_UNORM` and `_SRGB`,
  `R32_TYPELESS` for `D32_FLOAT`, `R32G8X24_TYPELESS` for `D32_FLOAT_S8X24_UINT`, ...), so apps
  view them as sRGB or linear; the formats listed are the typed ones, sRGB first. Depth formats
  are `D32_FLOAT`, `D16_UNORM`, `D24_UNORM_S8_UINT` and `D32_FLOAT_S8X24_UINT` (OVRPlugin's own
  D3D11 path asks for the last as its sampled eye depth). Cube arrays are allowed. D3D12 images are in `RENDER_TARGET`
  (colour) or `DEPTH_WRITE` (depth) at acquire and must be back in that state at release.
- Queue access: XR_KHR_vulkan_enable (Concurrency) lets the runtime use the app's queue only in
  `xrBeginFrame`, `xrEndFrame` and the swapchain image calls, which the app synchronizes; D3D's
  bindings say the same of the queue and the immediate context. puppetxr submits only in
  `xrEndFrame` (frames, passthrough camera images, environment depth); D3D11 runs its command
  lists on the immediate context there and restores the app's context state. With the control
  server on, `xrEndFrame` also reads each frame back (`comp_snapshot`, set up at session creation
  with `comp_set_snapshots`) and screenshots and frame reads (`oxr_read_frame`,
  `oxr_save_screenshot`) answer from that copy; before the first frame there is none. The server
  answers in `xrWaitFrame` and `xrPollEvent`, on Unreal's game thread or Unity's main thread
  while their RHI or render thread submits: reading the GPU there lost Unreal's Vulkan device
  (two threads in `vkQueueSubmit`) and crashed AMD's D3D11 driver. Without control a read goes
  to the GPU on the calling thread, which must own the queue then (C API `puppetxr_screenshot`
  from the app). `oxr_frame_read_thread` names the thread that read last.

Time conversion: `xrConvertWin32PerformanceCounterToTimeKHR` and back take the
`LARGE_INTEGER` as a 64-bit count (`os_native_ticks_to_ns`), related to `XrTime` at the last
frame as the timespec conversion is.
