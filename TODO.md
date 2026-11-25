# TODO

**Current focus:** verify the macOS port on the other platforms (the Linux presets, Windows and Wine; Pending verification), then real apps as tests (step 12): make the blocked Meta samples run by implementing what they need, then core apps and Godot. Extensions land with their CTS tests and, where a sample uses them, its sample test. Verify the Pending verification list below on real machines (Windows port, the release scripts and workflow).

Steps are ordered; each ends with green tests and updated docs. Delete items as they land; git history keeps what is done.

## Pending verification

Implemented and tested on Linux (Windows code under Wine, release scripts with software
rendering), not yet on the real targets. Each needs a run on real hardware; delete it, or fix
what fails with a regression test.

Linux release (`tools/dist/dist.sh`, docs/building.md "Linux releases"):
- [ ] the tarball runs on an old distribution (e.g. Debian 10) and a new one

Source release (`tools/dist/dist-src.sh`, docs/building.md "Source releases"):
- [ ] the unpacked `-src.tar.gz` configures, builds and passes the tests with the network off

macOS (step 10; docs/building.md "macOS", "macOS releases"):
- [ ] by hand: Unity's editor in Play mode with the MRUK project, played with keyboard and mouse (a click takes the mouse, moves turn, holding the left button drags MRUK's panel, Escape gives it back, WASD and QE never stick, 1 2 3, ] and [ switch profiles, Tab shows the panels, Fn+F1 the help, about 120 fps, the window maximized at once); Godot's `openxr_character_centric_movement` windowed
- [ ] HiDPI: `UiTest.panels_fill_windows_of_high_pixel_density` skips on a display of one pixel per point; run on a Retina display
- [ ] the extracted tarball on another Mac, through its manifest and as the active runtime; an Intel Mac for the x86_64 slice
- [ ] Linux `gcc-debug` in the pinned image (`tools/samples/run.sh --unit`) run x86_64 under Rosetta on an ARM Mac: rerun now that Linux has a window thread. The 5 `OxrThreadWindowedTest` aborts were SDL's assert that only its main thread pumps (`SDL_PumpEventsInternal`), hit by a frame ended off the main thread; `Platform.showing_a_maximized_window_does_not_wait_for_an_animation` skips where no window manager maximizes. `OxrVulkanRealWindowTest.frames_never_wait_for_the_display` took 1.5 s for 60 frames (bound 300 ms) on emulated lavapipe. Done when the run passes, or each failure is fixed or skips in the environment that cannot run it
- [ ] a key remapped by Karabiner-Elements to send the backtick (for a Logitech MX Keys) reached no binding when the key left of 1 was the UI key (Tab is now). Likely SDL's ISO swap (`SDL_cocoakeyboard.m`: key codes 10 and 50 swap when `KBGetLayoutType(LMGetKbdType())` says ISO, and that is the last keyboard used, not the one that sent the event). Matters to anyone binding `grave`. Run an app with `SDL_EVENT_LOGGING=1`, press the key, read the scancode; then decide the fix and add a test to `PlatformAppEventsTest`/`Platform`
- [ ] the help names `grave` by its symbol on the layout (`os_key_label`); on an ISO Mac keyboard with a US International layout that is § (± shifted) where people expect the backtick. Only shows when `controls.ui` or a binding is `grave`
- [ ] `mac-tsan`: 939 of 941 pass with Apple clang 21; `OxrMetalRealWindowTest.frames_composite_and_present_to_the_mirror_window` deadlocks inside the sanitizer runtime (TSan's `LibIgnore` lock against dyld's loader lock: SDL's `SDL_Metal_CreateView` `dlclose`s the Vulkan loader of the window while AppKit's dock thread `dlopen`s), no puppetxr frame involved. Done when it passes, or skips under TSan with the runtime named

Native platform layer (SDL3 replaced; docs/architecture.md "Platform layer"). Linux (Wayland,
X11), Windows and macOS pass all tests:
- [ ] `mingw-debug` builds `src/platform_win32.cpp` and passes under Wine
- [ ] macOS: Retina density (`UiTest.panels_fill_windows_of_high_pixel_density` skips on 1080p)
- [ ] Wayland input methods by hand (`zwp_text_input_v3`; `Platform.text_input_turns_on_and_off` only covers an unfocused window, which never gets the enter): Japanese or Chinese through IBus on GNOME into a panel's text field, the candidates under the field
- [ ] Wayland title bar: the right-click window menu and minimizing from it

Windows (step 10; docs/building.md "Windows", "Windows releases"):
- [ ] the mirror window, UI and input on the window thread by hand; puppetxrctl against a running app outside the tests
- [ ] the unpacked zip with `install-runtime.ps1` (and `-Uninstall`) runs hello_xr; Windows 10 1809 still loads the DLL

Release workflow (`.github/workflows/release.yml`, docs/building.md "Releasing"):
- [ ] a manual dry run on `main` passes all jobs; in particular on the hosted Windows runner, which runs elevated so the OpenXR loader ignores `XR_RUNTIME_JSON`: check `example_e2e` and `install_relocated`. Blocked on 2026-10-22: the account's artifact storage quota was full, so the first upload (`notes` of the `check` job) failed and every build job was skipped; the 3.3 GB of another repository's artifacts were deleted, GitHub recalculates usage every 6 to 12 hours. Run `gh workflow run release.yml --ref main` again; the Linux and source jobs passed locally (`dist.sh` with all tests, `dist-src.sh`, offline build of the full tarball), the Windows job has never run
- [ ] `release-macos` (ctest in parallel): `ApiControlTest.*` failed now and then with "Cannot write lock file
  /tmp/puppetxr/<pid>.json: No such file or directory", the discovery file missing and every request unanswered.
  Likely another simulator's sweep (`discovery_dir`'s stale lock files, `src/puppetxr_api.cpp`) deleting the temporary
  file before its rename on macOS; `Platform.lock_file_creation_survives_a_concurrent_sweep` passes there. Done when
  a test reproduces it and the sweep leaves files being written alone
- [ ] the control API tests (`ApiControlPausedTest.*`) hang until the ctest timeout (25 min) when the control socket cannot connect (a network namespace without loopback) instead of failing fast
- [ ] `tools/release.sh` on the first release: the draft has both Linux tarballs, both Windows zips, both macOS tarballs, both source tarballs, the four `.buildinfo`, `SHA256SUMS`

Motion recorder APK (`tools/recorder/build_apk.sh`, docs/recorder.md):
- [ ] the Unity and Unreal sample tests that pinch with tracked hands, on Windows with OVRPlugin: as Meta's runtime (`Oculus`, which OVRPlugin makes puppetxr) the aim state reports no pinch since a Quest 3 does so to an OpenXR app (`oxr_xrLocateHandJointsEXT`, `tests_oxr.cpp` `OxrHandOculusTest`). If OVRPlugin reads its pinch from the aim state, those tests stop pinching; then measure what a Quest 3 reports to an app on OVRPlugin and follow it
- [ ] on a Quest 3: `build_apk.sh --run` starts it in the headset with passthrough, the panel ahead; poking RECORD with the index tip records a step and `pull.sh` fetches takes that parse, with both hands, the aim state and the data source; a `push_protocol.sh` protocol replaces the built-in one at the next start. The recorder has only run on puppetxr so far

GPUs:
- [ ] an NVIDIA GPU (only AMD and software renderers so far): the test suite, hello_xr on Vulkan, OpenGL, D3D11, D3D12

## 7. mirror window + controls + ImGui
- [ ] every backend's real-window test asserts a present, as `OxrMetalRealWindowTest` and `OxrVulkanRealWindowTest.the_mirror_presents_at_most_at_the_display_rate` do: the GL and D3D ones check only the composite or the swapchain (`tests_oxr.cpp`, `window_presented_ns`); a Metal mirror without a swapchain went unnoticed that way

## 8. record/replay + control API + determinism
- [ ] Windows: puppetxr_load_api via GetModuleHandle is untested on a real Windows machine (Wine runs the tests with the runtime linked in)

## 8b. Devices, controls, hands and props
- [ ] more controller types (Valve Index, HTC Vive) as needed
- [ ] profiles: hold a key to use a profile only while it is held (a quick look at a wrist panel); profiles chosen per app (known module or app name); a crosshair where the right aim ray points; edit and save profiles in the UI
- [ ] mouse wheel scales the strength of key-held analog inputs
- [ ] more controllers through the slot table: a secondary axis slot for trackpads (Index, Vive), finger curls and force
- [ ] virtual keyboard: swipe typing and word suggestions (more dirty textures), the runtime's own hide or show animations, a keyboard per session beyond the first in the UI and control API
- [ ] keyboard tracking checked against a device that tracks keyboards: whether the remote query bit finds the local keyboard, the name and size Meta reports, whether the keyboard space is lost while the cameras do not see it
- [ ] spatial entities: an end-to-end app test (no Meta sample uses them), plane tracking and anchors on the room, a UI and `puppetxrctl` view of what each context tracks
- [ ] props: more kinds (mouse, other markers); XR_BD_dynamic_object_tracking on the same props
- [ ] render models: textured or authored controller and keyboard models (Blender, like the rooms); a way to move a put down controller (puppetxrctl, C API, UI)
- [ ] hands: a hand scale override is reported but does not scale the joints; `ready_ext` and action states of a hand outside the cone stay active
- [ ] host gamepad drives the simulator: SDL gamepad events in platform, sticks/triggers/bumpers/face buttons to both Touch controllers, View held moves the body, deadzone in sim
- [ ] simulated OpenXR gamepad: `microsoft/xbox_controller` on `/user/gamepad`, a gamepad device with its inputs and four haptic outputs, recording format bump

## 8c. Realistic hands (docs/recorder.md, docs/hand-motion.md)

Ordered: real takes first, then the model fitted to them, then gestures.

- [ ] trim and resample the takes the tests need into `data/motion/takes/`. The full protocol was recorded once on a Quest 3 (39 takes, 122 MB raw, kept outside the repository); a second person's session would show which fitted values are personal
- [ ] fingers that lose tracking while the wrist stays tracked, as measured (docs/recorder.md, On a Quest 3): part of the noise model's dropouts (docs/hand-motion.md, Noise)
- [ ] re-record on Windows the sample references that show tracked hands, which the fitted hand model changed (relaxed at rest, the measured pinch and skeleton): `unity_starter_hands`, `unity_starter_microgestures`, `unity_starter_simultaneous`, `unity_starter_controller_hands`, `unity_starter_handsinteractiontrainscene`, `unity_depthapi_hands` (`tools/sample_tests.py --update`, then review; the Linux ones were re-recorded with the model)
- [ ] the pinch pose mixes two habits: in the takes the right hand pinches with the other fingers open (tips 93 mm from the palm), the left curls them (49 mm); `pinch`, one medoid of both, sits between (65 mm; `tools/hand_fit.py`). Fitting it to one hand changes the grasp a pinching hand reports, which is derived from the curl: a Quest 3 reported a grasp of 1.0 in the right hand's pinch takes but 0.48 at the median for pinches with open fingers across all takes, so `grasp_ext` is no function of curl and pinch alone. Record pinches with the other fingers open and curled, both hands, and fit the grasp to the finger joints' angles before choosing; done when `SimGesture.built_in_gestures_look_like_their_recordings` holds pinch_tap to the right hand's pinch within 2 cm and the grasp test still matches the headset
- [ ] `open_palm` means the palm forward, fingers up (a stop sign); the `poses` take showed an open palm as the palm up, held out. Decide which apps expect; a check of the wrist's back (watch face up, palm down, as in two of the three `palm_up` repeats) would be a gesture of its own
- [ ] gesture paths beyond `to` and `home`: `circle`, `twist`, `shake`, `swing`, `curve`, `throw` (docs/hand-motion.md, Throw), and solved shapes (`around` a handle, ball or dial)
- [ ] gestures on controllers: paths move the grip (anchor `tip` for poking), strengths set trigger and squeeze, a step's `inputs` (docs/hand-motion.md, Controllers)
- [ ] noise: jitter, tremor, sloppiness, dropouts, fitted to the takes, seeded
- [ ] recording format 6: per tracked hand the joint rotations and strengths; versions 1 to 5 still read
- [ ] gesture events for observed edges (a pinch strength crossing 0.5 up and down, tracking lost and found) and a UI Gestures panel (docs/hand-motion.md, Control API and CLI)
- [ ] the rest of the gesture library (docs/hand-motion.md, Library, Planned), each checked against its takes (`data/motion/takes/`), and an app test that `idle` and `talking_hands` trigger nothing
- [ ] clip steps: a user's take plays as a gesture, inside one or alone
- [ ] body: the recorder's `body` track, body clips through the same step
- [ ] after the above: README screenshots showing the new hand gestures
- [ ] after the above: a nicer controller render model, made with zenvr's ZenApp kit skill (joins the render models item in 8b)

## 9. robustness + CTS
- [ ] extension tests the CTS skips until we implement them: EXT_hand_tracking, EXT_dpad_binding, EXT_eye_gaze_interaction (full list in docs/extensions.md)
- [ ] garbage-input tests for every entry point
- [ ] once the repository is public: build provenance of the release files (`actions/attest-build-provenance` in the `release` job of `.github/workflows/release.yml`, permissions `id-token: write` and `attestations: write`; private repositories need GitHub Enterprise Cloud); done when `gh attestation verify <file> --repo FlexW/puppetxr` passes and docs/building.md "Releasing" says how to verify
- [ ] Windows: sign the DLL and puppetxrctl.exe (needs a code signing certificate)
- [ ] Windows in the full tier: the other presets (`win-clang-release`, `win-clang-asan`, `win-clang-tidy`, `msvc-release`, `msvc-asan`), `samples` and the release build uploading nothing in `ci-full.yml`. Done when they run on `windows-2022`
- [ ] macOS in the full tier: `mac-release`, `mac-asan`, `samples` and the release build uploading nothing in `ci-full.yml` (`docs/building.md`, macOS). Done when they run on a hosted macOS runner
- [ ] the OpenGL tests on the Windows runner: 80 skip as it has no OpenGL driver. Mesa's `opengl32.dll` and `libgallium_wgl.dll` of `.github/actions/windows-vulkan` next to the test binary should provide one (`tools/sample_tests.py` does this for Godot); done when they run in `ci-fast.yml`
- [ ] a compiler cache for the Windows and macOS jobs of `ci-fast.yml`, which rebuild everything each run (`.github/actions/unit` has it on Linux); clang and MSVC on Windows need embedded debug info for it. Done when a run without source changes builds in under a minute
- [ ] release binaries of all three platforms: the dry run of `release.yml` (Pending verification), then `tools/release.sh`
- [ ] threads: clang Thread Safety Analysis (`-Wthread-safety`; the runtime mutex as one global capability, `OXR_Lock` a scoped capability, runtime state `GUARDED_BY` it, `*_locked` helpers `REQUIRES` it) so a missing lock fails the build; macros empty on GCC
- [ ] composition tests of extensions once they land: quad views (visibility mask, inverted alpha, image layout, equirect, equirect2 and ProjectionDepth done; cylinder and cube have none)
- [ ] CI image with Mesa 26.2.4 lavapipe for cts_composition
- [ ] puppetxrctl: event stream (session state, profile changes, log lines), screenshots returned over the socket for remote simulators
- [ ] scenario tests of extensions once they land: palm_pose, maintenance1 GripSurface, eye gaze, dpad, plane detection, spatial anchors
- [ ] the hand tracking scenarios take no screenshot of the hands; add one each with references made on Mesa 26.2.4 lavapipe (`--update`, then review)

CTS results (1.1.63, non-interactive, `-H -T`, 2026-10-20):

| pass | command | result |
|---|---|---|
| cts_vulkan2 | `"~[interactive]" -G Vulkan2` | 103 cases, 0 failing (65 run, 38 skipped for missing extensions) |
| cts_vulkan2, one case per process | same, on Mesa 25.2.8 lavapipe (2026-10-22) | 76 passed (XR_META_performance_metrics, XR_FB_space_warp among them), 25 skipped, 2 crash inside lavapipe 25.2.8 with and without this step's changes: SwapchainsUnorderedAccess (the CTS's compute clear), Timed_Pipelined_Frame_Submission |
| cts_vulkan | `"~[interactive]" -G Vulkan` | 103 cases, 0 failing |
| cts_opengl | `"~[interactive]" -G OpenGL` (2026-10-21, on an X display) | 73 cases passed, 0 failing |
| cts_headless | `"[XR_MND_headless]" -E XR_MND_headless` | 1 case, 0 failing |
| cts_automation_simple | `"[actions][interactive]~[no_auto]" -E XR_EXT_conformance_automation` | 22 cases, 0 failing (13 run) |
| cts_automation_touch | same with `-I /interaction_profiles/oculus/touch_controller` | 19 cases, 0 failing |
| cts_automation_touch_pro, _plus | same on `facebook/touch_controller_pro` and `meta/touch_controller_plus` with their extension | 0 failing |
| cts_composition | 17 core and 5 extension `[composition][interactive]` tests on lavapipe against references | 55 scenes, all bit-identical |
| cts_composition_gl | the same on the CTS OpenGL plugin and llvmpipe, against the Vulkan references (2026-10-21) | 46 scenes within 3 (20 identical); GradientFormatsLinearVsNonLinear left out |
| cts_scenarios | 9 core and 2 hand tracking `[scenario]` tests, scripted motion and input | 9 core passed, 8 screenshots bit-identical; the 2 hand tracking ones passed (2026-10-21, no screenshots) |
| Windows (2026-10-22) | cts_vulkan2, cts_vulkan, cts_opengl (WGL), cts_d3d11, cts_d3d12 on an AMD Radeon RX 6700 XT; cts_headless; the four cts_automation passes | 106 cases each for the graphics passes, 0 failing in all ten |

## 9b. core specification audit (1.1.65, chapters 2-11, every runtime "must")
- [ ] same-entity spaces report tracked even when untracked (spec 7.4); needs simulated tracking loss first
- [ ] optional: PRIMARY_MONO and PRIMARY_STEREO_WITH_FOVEATED_INSET view configurations, MSAA swapchains
- [ ] emulate more controllers than Touch and simple (bindings of all registry profiles validate already)

## 10. Windows + macOS
- [ ] Windows apps: hello_xr with -g D3D11, D3D12, OpenGL, Vulkan2; Unity and Unreal on D3D11 and D3D12
- [ ] Windows on ARM64
- [ ] Android binding of XR_KHR_opengl_es_enable

OpenGL and OpenGL ES (native backend over the app's share group, GL 3.3 core and ES 3.0; Godot compatibility renderer):
- [ ] MSL validated at build time where Xcode's `metal` compiler is found (`xcrun -f metal`), as glslangValidator and dxc check the other variants: `cmake/shaders.cmake` writes the text with the prelude `render_metal.mm` prepends and runs `xcrun metal -std=metal2.3 -fsyntax-only` on it. Today a dialect mistake shows up only in `RenderTest.every_shader_makes_a_pipeline/Metal`. Done when a broken MSL macro fails a macOS build with Xcode
- [ ] GLES depth readback (ES 3.0 reads colour only); cube array swapchains on GL (samplerCubeArray variant)
- [ ] Intel HD Graphics 620 on Windows 10 (driver of its Vulkan 1.3.215): `Apis/OxrGLTest.sessions_are_created_on_a_thread_without_a_current_context/OpenGL` fails, `wglCreateContextAttribsARB` returns no context when sharing with the app's context from a thread on which only `begin_wgl_bootstrap`'s legacy context is current (`render_gl.cpp`, `create_wgl_context`). The CTS multithreading test creates sessions this way. Start by creating the shared context while no context is current, or on a hidden window of our own with the app's pixel format. Done when the test passes on that GPU
- [ ] CI with an X server that has GLX and XFree86-VidMode for cts_opengl (Xorg dummy driver)
- [ ] show the offered graphics APIs (and disabled extensions) in the UI, `puppetxrctl status` and the C API status
- [ ] an EGL app with a window (X11 and Wayland) to test the EGL mirror path with; only GLX is tested with an app
- [ ] `mingw-debug` in Wine (Xvfb, no window manager): all pass but `OxrD3DRealWindowTest.first_frame_has_the_shown_windows_size` and `a_click_on_a_panel_leaves_the_mouse_to_the_panels` (Wine's X11 driver maximizes after the frame the test checks: the window grows to 1920x1054 once the swapchain was made at 1280x720, and the click at (1000, 600) comes before), and `example_e2e`, `install_relocated`, `body_dump` when run as root, where the OpenXR loader ignores `XR_RUNTIME_JSON` (an elevated context). Done when the D3D tests wait for the size Wine settles on, or skip in Wine with the reason
- [ ] the control server reads back every frame and waits for it (`comp_snapshot`), about 10 ms a frame in a debug build: copy into a staging buffer with a fence and read it when a reader asks
- [ ] an MRUK sample test that drags a panel (EnvironmentPanelPlacement: aim with `point right <u> <v> left grip -60`, hold the trigger, move)
- [ ] the first event pumps at startup take 32 and 18 ms on macOS (profile of a Unity player)
- [ ] a regression test for IMM32 staying loaded after the loader unloads puppetxr on Windows (the windows driver loads and pins it): `puppetxr_tests` imports IMM32 itself, so it needs a program that loads the runtime through the loader, destroys its instance and then creates a window; `puppetxr_example` under `procdump -e` crashed in 16 of 120 runs with OBS's capture hook injected before the fix and in none after
- [ ] CI matrix, cross-arch determinism check
- [ ] macOS: `godot_character_centric_movement_vk` and `godot_origin_centric_movement_vk` show one of two `walked` frames from run to run (6595 pixels beyond the tolerance in one), even alone; the other six demos and hello_xr are stable. Godot's call order is the same in both (one thread: wait, sync, begin, end) and `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1` changes nothing; suspects: Godot's physics jitter fix on the wall clock, or Godot drawing on another queue than the one bound. Done when 10 runs give one frame

## 11. Mixed reality: rooms and Meta extensions

Goal: mixed reality apps run unmodified with passthrough, scene, depth, raycast and anchors.
CTS 1.1.63 has no tests for these extensions (only the proc-address checks every advertised
extension gets), so the gtests are their conformance suite. With the scene extensions
advertised: cts_vulkan2 103 cases, 0 failing (2026-10-20).

- [ ] environment raycasts: what a Quest 3 does that puppetxr does not yet. A ray from an origin away from the head
  pointing sideways out of every view is `RAY_OCCLUDED` on the headset at 60 to 120 degrees from the view direction
  (puppetxr: `HIT_POINT_OUTSIDE_OF_FOV`); hits wander by about 10 mm over 30 s (the depth noise is new every capture,
  see the rounded edges item below). Start: `raycast_depth` in `src/oxr.cpp`; measure the sideways case with
  origins at known offsets from the head on a Quest 3 first. Done when a gtest reproduces each measured status
- [ ] Unity and Unreal sample references with Quest 3 raycasts: `tools/sample_tests.py` runs the `unity_*` and
  `unreal_*` tests with `PUPPETXR_ENVIRONMENT_RAYCAST=mesh`, as their references were recorded. Re-record those that
  raycast (`unreal_mruk_environmentraycast`, `unity_mruk_environmentpanelplacement`) with the default on Windows with
  the editors, together with the depth references below, and drop the pin
- [ ] Unity and Unreal sample references with the Quest 3 depth sensor: `tools/sample_tests.py` runs the `unity_*`
  and `unreal_*` tests with `PUPPETXR_ENVIRONMENT_DEPTH=eyes`, as their references were recorded (the other samples
  show no depth and run on the default). Re-record the references of those that show depth (`unity_depthapi_*`, the
  MRUK occlusion scenes, Unreal's Occlusion sample once it is compared) with the default sensor on Windows with the
  editors and drop the pin; the noise is seeded, so they stay stable from run to run
- [ ] environment depth: hands and controllers. Quest 3's depth shows the user's hands and forearms (and hand
  removal takes the hands out, from the wrist, within a frame or two, the forearms stay); puppetxr's has the room
  and the props. Start: the hands' and controllers' shapes as `W_Instance`s next to the props' (`prop_instances`,
  `src/oxr.cpp`; `w_create_mesh_model` from the hand meshes); `OXR_DepthProvider::hand_removal` then leaves the
  hands out. Done when a gtest sees a hand in the depth with removal off and the wall behind it with removal on
- [ ] Windows sample references after the dimmed passthrough fix: the Meta OpenXR samples' references showed the room's
  surfaces through each other wherever passthrough is translucent (re-recorded from CI). Run the Unity and Unreal
  sample tests on Windows (`tools/sample_tests.py`) and re-record any that show translucent passthrough, after looking
  at each against its old reference
- [ ] dimmed passthrough: a pixel where two surfaces tie in depth exactly (the outline of a closed shape) blends twice,
  one pixel in a view on lavapipe (`OxrPassthroughTest.dimmed_passthrough_shows_only_the_nearest_surface` allows 8).
  Start: draw a passthrough layer opaque into an offscreen image of the eye and composite that image once with the
  style's opacity, as a headset composites its camera image (`draw_passthrough`, `src/comp.cpp`). Done when the
  test allows none on lavapipe
- [ ] mesh props: textures with coordinates outside [0, 1] clamp, as the passthrough sampler does not repeat; glTF's
  `KHR_texture_transform` and wrap modes are ignored. Start: a sampler with the texture's wrap mode
  (`r_cmd_bind_textures`), or baking the repeat into the atlas; a gtest with a repeated texture on a quad
- [ ] mesh props: paths beyond a line and a circle (waypoints, a model's glTF animation) and models that change shape;
  `sim_prop_pose` stays a pure function of the prop and the time
- [ ] environment depth: Quest 3's edges are rounded, near objects' outlines bulge by a few pixels and small gaps
  close, and a still scene's depth wanders slowly (the noise is low frequency in time too). Start:
  `sim_depth_sensor` (`src/sim.cpp`), a dilation of near over far by a configurable radius and noise interpolated
  between captures; compare against depth images of a Quest 3 looking at a table edge
- [ ] environment depth off the frame: tracing takes 8 to 9 ms of `xrEndFrame` every 40 ms in the living room (two
  views, five threads, Ryzen 7 5800X, release). Start: trace a capture on the workers as soon as its pose is known
  (capture + pose delay) and upload it in the `xrEndFrame` before it shows; a room change waits for the trace. Done
  when `xrEndFrame` with a running provider takes no longer than without in the Performance tab
- [ ] tracking drift in the UI: the Simulator panel's room section shows the stage origin of the instance only; show the drift and its rate there, with fields to set them (`src/ui.cpp`, `room_from_stage` in `UI_Status`; a `UiTest` that sets the drift by `ui click` and reads it back with `sim_get_tracking_drift`)
- [ ] later: EXT_spatial_entity family on the same world (XR_EXT_spatial_persistence on the same store), scene v2 (high fidelity)

## 12. Real apps and samples as tests

Fetched at a pinned commit, patched from `tests/samples/<app>/`, driven by `tools/sample_tests.py`
through puppetxrctl, judged by screenshots against references (`docs/testing.md`, Sample apps).
Mostly Linux; on Windows the Godot demos (Vulkan) and Meta's Unity MRUK samples run.

- [ ] llvmpipe with rasterizer threads leaves stale 64-pixel tiles in XrSceneModel, differently each run, and single-threaded it shows lines on every 64th row. Not puppetxr's fencing: with `glFinish` on every context switch it stays. It goes away with `LP_PERF=no_depth`, so llvmpipe's threaded depth test inside the app's own frame; standalone repros (multiview layered depth, the sample's state sequence, invalidating UBO maps) do not trigger it. Next: an apitrace of XrSceneModel replayed with and without threads, then a Mesa report; until then `LP_NUM_THREADS=0` and tests run in parallel instead
- [ ] references identical on every x86 machine, then `--tolerance 0` again in `tools/samples/run.sh` and `cmake/samples.cmake` (1 until then, as CI runs on whatever CPU the runner has): all references are now made in the image; Meta references made on another machine with the same Mesa and LLVM differed by 1 on about 10000 pixels (same with puppetxr and the samples built outside the image, so not the build). llvmpipe JITs for the host CPU; `GALLIUM_OVERRIDE_CPU_CAPS=avx` or `avx2` do not change it, `sse4.1` changes much more. Find the CPU feature (AVX-512 here), pin it in `run.sh`, remake all references in the image; done when two machines with different CPUs pass the same references
- [ ] XR_METAX1_passthrough_camera_data on Linux: MRUK's Unity `PassthroughCameraAccess` should get frames there too; add a Unity sample test for it (`tools/sample_tests.py`, `unity_mruk_*`). Done when that test passes on Linux
- [ ] the other unpublished Meta extensions (docs/extensions.md, "Unpublished"): `XR_METAX1_room_entity_info` next; each needs its layouts from Meta's binaries
- [ ] Meta's XR Operator layer drives the Meta XR Simulator through `XR_METAX1_simulator_compositor_output_capture` and `XR_METAX2_simulator_head_pose`; offering both would let it drive puppetxr too (same header problem)
- [ ] XR_FB_foveation_vulkan: fragment density map images per swapchain image; puppetxr would add VK_EXT_fragment_density_map to the device extensions it asks for when the GPU has it. Done when a test on a GPU with the extension renders with the map (lavapipe lacks it)
- [ ] the other 1.1 Touch names, `meta/touch_controller_rift_cv1` and `meta/touch_controller_quest_1_rift_s`, are accepted but not emulated
- [ ] XrBodyFaceEyeSocial: XR_FB_body_tracking, XR_FB_face_tracking2, XR_FB_eye_tracking_social, or a fixed exit path
- [ ] more core apps: KhronosGroup/OpenXR-Tutorials, janhsimon/openxr-vulkan-example, maluoi/openxr-explorer
- [ ] trackpad dpads (`dpad_center`, click and force activation) once a simulated controller has a trackpad; binding modifier haptics other than `XrHapticVibration`
- [ ] Godot's mobile renderer on lavapipe started without its directional light (ground and hands darker, sky unchanged) for a whole run in 3 of about 70 Vulkan runs, all in full-suite runs with Mesa's shader disk cache on; with `MESA_SHADER_CACHE_DISABLE=true` 0 of 32 runs. If it comes back, find out whether Godot's background pipeline compilation or puppetxr's queue use is at fault (`docs/compositor.md`, Vulkan ordering)
- [ ] body tracking checked against a device that tracks the body: run `apps/body_dump` there and compare with `tests/body_dump_puppetxr.json`: joint axes, parents (is the root's parent -1 or `XR_FULL_BODY_JOINT_NONE_META` in the full body set, the wrist's the wrist twist), proportions, where the root is while tracking. Fix the differences or list them in `docs/body-tracking.md`
- [ ] Unreal-Movement's animation blueprint maps (`MAP_HighFidelity_AnimBlueprint`, `MAP_RetargetMannequinAnimBlueprint`) as sample tests, and dropped joints: retargeted avatars stay sound; a difference counts against puppetxr only when `body_dump` or `XrBodyFaceEyeSocial` show it too
- [ ] simulate the gaze's sample lag: puppetxr reports the located time, hardware a sample 31 to 36 ms older (`docs/face-tracking.md`). Either report a configurable lag in `XrEyeGazeSampleTimeEXT` and locate the gaze at that older time, or write in the docs that the simulator's gaze is always current. Done when an app that corrects for the lag behaves the same on both
- [ ] the face half, which needs a device that tracks the face: where the per-eye poses of `XR_FB_eye_tracking_social` sit, their confidence, and the weights' ranges in the vendor presets, `SIM_FACE_MAX_GAZE_YAW` 35 and `SIM_FACE_MAX_GAZE_PITCH` 30 above all (`src/sim.h`, numbers nothing has checked). Compare with `apps/eye_dump` and Meta's `XrBodyFaceEyeSocial`
- [ ] the other face extensions on the same weights: `XR_ANDROID_face_tracking`, `XR_HTC_facial_tracking`, `XR_BD_facial_simulation`; `XR_ANDROID_eye_tracking`
- [ ] body tracking: an option that keeps the controllers within the arms' reach, off by default (tests place controllers exactly)
- [ ] body tracking: steps when the body walks, instead of feet sliding with the root
- [ ] `vendors_body_tracking_gl` with a longer left arm (`puppetxrctl body bone left_arm_upper 1.3`): one more screenshot in its script; done when the sample test compares it
- [ ] Godot demos on Windows with the compatibility renderer: Godot sets its window's pixel format twice and Mesa's WGL refuses the second call; needs a Mesa that accepts the same format again, or a Godot fix upstream, then drop the `_gl` exclusion in `tools/sample_tests.py`
- [ ] Meta OpenXR SDK samples on Windows: the SDK's own Windows path draws desktop GL over WGL, the references are GLES on EGL; either port `linux.patch` to Mesa's Windows EGL and GLES of mesa-dist-win (`libEGL.dll`, `libGLESv2.dll`), or a Windows patch with references of its own
- [ ] Godot vendors plugin samples, hello_xr and Monado's demos on Windows (`cmake/samples.cmake` builds them on Linux only)
- [ ] Unity MRUK references are of one machine (D3D11 on an AMD Radeon RX 6700 XT); a second GPU needs references of its own or a tolerance. A CI without a GPU runs them on WARP: the players take `-force-driver-type-warp` (Microsoft Basic Render Driver), all 17 tests take their shots in 7.8 minutes (4.3 on the GPU) and two runs gave bit-identical images, but none matches the GPU references, so WARP needs references of its own (a `sample_tests.py` switch passing the argument, references beside the GPU ones). A Unity license for CI
- [ ] `unity_ssa_colocation` lands in one of two stable pictures: the peer's anchor axes a few pixels apart and another menu entry highlighted (3312 pixels in `peer_anchor`), two runs in a row each way. The host and the peer start one after the other in wall-clock time; find which of their exchanges is not stepped (the advertisement poll, `discovery_next_poll`, or the peer's join), then make it wait on a condition. Done when ten runs pass at 0 pixels
- [ ] Unity Shared Spatial Anchors: the Basic Sharing scene (SharedSpatialAnchors) matches players and exchanges their user ids and anchor uuids through Photon, which needs an app id and the network. A patch replacing that exchange with a local one (a file in the shared `PUPPETXR_CLOUD_DIR`), and the user id from the command line as for the Unreal sample, would exercise sharing with users in Unity too; `unity_ssa_colocation` covers sharing with groups
- [ ] Regenerate `cts_composition` and `cts_scenarios` (`tests/cts/`, lavapipe of the pinned Mesa) after the 60 degree aim (hands held 60 degrees up; `cts_scenarios.THUMBS_UP` holds accordingly); run each with `--update` in the image and review. The sample references made in the Linux container (`meta_openxr_sdk`, `openxr_sdk`, `openxr_simple_example`, `xrgears`, `godot_vendors`) and the Windows-made ones are updated
- [ ] Device presets: each model's asymmetric eye angles (`xrLocateViews` fov) instead of the shared `views.fov_degrees`; the recording header stores one angle, so this is a `REC_FORMAT_VERSION` bump with a reader for the old header. Measure the angles on the devices (`apps/eye_dump` prints the fov per view) rather than taking spec sheet numbers
- [ ] Device presets: Meta's VR glasses. OVRPlugin 207 knows them as `Meta_Link_VR_Glasses` by the headset id `df221d8c-91c6-4b49-8a1d-16f588875959`; their system name, eye buffer, rates and controllers are not known yet
- [ ] Startup: puppetxr's share is about 200 to 250 ms of 1.4 s (MRUK player) to 2.1 s (editor). Each of Unity's two or three instances loads the room again (about 23 ms each, the albedo PNG decode); keep the decoded room across instances of a process. The compositor (21 ms) and the window (16 to 23 ms) are next
- [ ] The props motion drop-down was reported to close the QR code entry when clicked with a real mouse in the Unity editor; synthetic clicks (tests and `puppetxrctl ui click` in the MRUK player) work. Reproduce with a real mouse, find what differs (SDL's global mouse state on the window thread, the editor's input), add a test
- [ ] UI click tests for every panel: each button, checkbox, combo and drag of the Simulator, Devices, Virtual keyboard, Props and Log panels and the menus, through `UiClicker`
- [ ] The Unity MRUK sample tests enter every scene from the start menu, with the session running; one scene started directly (as the editor does) would cover MRUK's discovery in `Awake`
- [ ] `unity_mruk_oculus` allows 6000 differing pixels: OVRPlugin paces Unity's frames on the Oculus loader, not locked to puppetxr's steps, so Oppy's idle animation lands a frame apart now and then, and its walk is skipped. Lock the frames (find what OVRPlugin's frame thread waits on), or check occlusion directly (the table's pixels where Oppy stands in the OpenXR view). The per-frame read-back in `xrEndFrame` with control on moved PassthroughRelighting's Oppy to another, again stable, frame of his animation (reference updated)
- [ ] Unity MRUK scenes beyond loading: throw in FindMultiSpawn, place panels in EnvironmentPanelPlacement (DestructibleMesh destroys a segment); KeyboardTracker and QRCodeDetection track their props (a keyboard, a QR code), moving them is next. PassthroughRelighting walks Oppy; occlusion of him needs Depth API in the Windows player (below). Animations and spawns are bit-identical with `-puppetxr-fixed-step`. KeyboardTracker came out in one of two images because each query gave the keyboard a new space, which MRUK took for a new keyboard; queries return the live space now, EnvironmentPanelPlacement 643 and 661 pixels apart (a controller ray left on its panel, fixed by turning the rays to the floor first). Players that failed after 4 s without a screenshot crashed in OVRPlugin's JSON reader (0xC0000409), reading another player's log as it is written; they start one at a time and such a test runs up to twice more (reported to Meta). A player whose simulator had no discovery file was puppetxr's: the rename onto a path a reader held failed, now retried. Once a player hung after its shutdown log; `hang_stacks.txt` will show where if it happens again. In a full Windows run on 3 jobs KeyboardTracker (both loaders) showed other lines in its on-screen log and EnvironmentPanelPlacement a hover outline and ray dot on its panel; alone they pass. Both follow the wall clock (the log's lines, the ray's hover); find what in them is not stepped
- [ ] Report to Meta: in Unreal's Occlusion sample every virtual object is near black from Meta's plugin (docs/samples.md, Unreal Occlusion sample: `InvAlphaTexture` lacks the sRGB flag on D3D); puppetxr's depth, the matrices and the occlusion result are right (RenderDoc). The tests run unchanged and are not compared; once the plugin is fixed, record their references and compare them
- [ ] Unreal Occlusion sample: the modes are a combo box on a poke panel (`IsdkInteractableWidget_0`, `IsdkPokeInteractablePanelConfig`) that stands low and tilted in front of the player: world (0, 0, 80) cm, stage about (0, 0.8, -0.4) m, below the view when looking ahead; the test still presses A. Earlier pushes went through empty space at eye height. Pushed there (head pitched 40 degrees down, the right grip at stage (0, 0.8, -0.25) to (0, 0.8, -0.45)), the panel's `IsdkPokeInteractable` goes HOVER, then SELECT (editor probe, 2026-10-26). The panel is `WBP_UI` on a 1024x500 widget component at scale 0.1 (102.4 x 50 cm), yaw 180, pitch 10; its `OcclusionsComboBox` (No, Hard, Soft Occlusions; Soft at start) and `HandsRemovalCheckBox` (checked) are widget properties of that name, and the game mode's `CurrentOcclusionMode` and `HandsRemovalEnabled` show the effect. `unreal.SlateLibrary` (`local_to_absolute`, `get_local_size`) gives the items' places on the panel. Next: poke each item at its place, check the state changes, then compare each mode in `unreal_occlusion_modes`. Editor probes of this project stalled at start in four of nine runs (the editor's command script took 15 minutes to report ready, no shader compile in the log)
- [ ] Unreal Passthrough sample: PTMasked's circle on the left controller after A shows no passthrough; compare with the brush, which paints passthrough (`unreal_passthrough_maskedbrush`). An editor probe of a Blueprint-only project needs `puppetxr.VRPreview`: copy the built `Plugins/SampleEditor` of the MRUK sample into its build copy for the probe (not into the patch)
- [ ] Unreal Passthrough sample: click through the remaining widgets (PTStyles' contrast, brightness and posterize sliders, PTColorMap's entries, PTColorLut's weight and type, PTColorScaleAndOffset's stop) and compare each style; PTStyles' curves and edge alpha are tested. A slider dragged with the ray follows only the first steps: the edge alpha stops at 0.42, the LUT weight barely moves; find what the Interaction SDK's ray needs to keep the drag (hand speed, hover, a pinch strength)
- [ ] Unreal Interaction SDK sample: the levels are compared after 600 frames, with tolerance 3 on up to 2000 pixels: their fog settles over the first seconds as far as the load took in wall-clock time, and a few widgets (a torch, a card's icon) animate in wall-clock time. Find what drives them (Unreal's real time in a material or widget animation) to compare at 0. Interact in each level: the controller pokes PokeExamples' red button (`unreal_isdk_poke`); poke the other buttons, with a finger too, select with the ray, grab and throw, transform, gaze, hand poses. TransformerExamples crashes in `UIsdkHandGrabPose::InitializeDebugVisual` (it asks for the editor's world under `-game`); run it in the editor's VR Preview instead
- [ ] Report to Meta: its Unreal plugin (`OculusXRLayerExtensionPlugin.cpp`, `UpdateCompositionLayers_RHIThread`) passes `xrGetRecommendedLayerResolutionMETA` the display time of the frame its RHI thread submits, older than the last `xrWaitFrame`'s, which the extension makes `XR_ERROR_TIME_INVALID`, and logs an ensure for each refusal. puppetxr answers such a time for the plugin (`accept_older_frame_time`, on with `UnrealEditor-OculusXRHMD.dll` loaded, docs/config.md Known modules); once the plugin passes the waited frame's time, drop that row
- [ ] Unreal desktop-renderer variants differ from their references and from run to run: `unreal_passthrough_startup_{d3d11,d3d12,vulkan}_desktop`, `unreal_passthrough_styles_{d3d11,d3d12,vulkan}_desktop` (thousands of pixels, by 1 on D3D11, up to 20 on Vulkan) and `unreal_mruk_menu_vulkan_desktop` (about 600 pixels, up to 118), one at a time, on the runtime before and after the window thread fix alike (2026-10-25). The mobile renderer's variants match. Find what in the desktop renderer depends on more than the stepped frames (temporal AA history, auto exposure, a wall clock), then pin it in the test's `-dpcvars` or compare these at a tolerance
- [ ] Unreal shaders compiled cold: `unreal_shared_anchors_session_{d3d11,vulkan}_desktop` showed grey-grid menus on their first run, which compiled the shaders cold: Unreal draws a default material while it compiles asynchronously. The Unreal tests compile synchronously now (`bAllowAsynchronousShaderCompiling=False`); done when a run with an emptied `DerivedDataCache` of a sample project matches the references
- [ ] Report to Meta: Unreal's Vulkan renderer draws parts of two scenes wrong, with a warm shader cache, while D3D11 and D3D12 are right and puppetxr composites whole images the same way on every API: `unreal_passthrough_startup_vulkan_desktop` (grey noise on Oppy's head, no reference) and `unreal_isdk_poke_vulkan_mobile` (table and furniture black, not compared). The validation layer reports errors in Unreal's own commands on the mobile renderer (`viewMask-06178`, `viewType-07752`, `shaderTileImageColorReadAccess-08718`). Once fixed, record and compare both
- [ ] Unreal Shared Anchors sample: Create Session, Create Anchor (`unreal_shared_anchors_session`) and sharing with users between two players (`unreal_shared_anchors_sharing`) are tested. Next: Load Anchors (from the store after a restart), Unsave and Erase, Orient to Anchor, the cubes (Spawn Cube, Explode Cube) replicated to the peer
- [ ] Unity Starter Samples: Stereo180Video draws black in the app's own projection layer (its Media Foundation video); OVROverlay's modes, EnableDisablePassthrough's toggle, SpatialAnchor's placement, its save, hide, load and erase, and Locomotion's teleport are tested; OVROverlayCanvas shows some labels mirrored, which needs a device's picture to judge
- [ ] Unity Depth API sample: grab objects with the controllers; OcclusionToggler's modes, HandsRemoval's thumb taps, SceneAPIPlacement's posters and DepthMask's mask are tested (`unity_depthapi_occlusionmodes`, `unity_depthapi_hands`, `unity_depthapi_posters`, `unity_depthapi_mask`)
- [ ] Unity Passthrough Camera API samples (`Unity-PassthroughCameraApiSamples`, MRUK 85): the player starts no OpenXR session (the project's Windows XR loader setting), and MRUK 85 reads the camera on Android only; a newer MRUK may take images on Windows through `XR_METAX1_passthrough_camera_data`
- [ ] later: Proton's wineopenxr maps D3D11/12 to XR_KHR_vulkan_enable as an alternative
- [ ] later vendors: Android XR (godot_openxr_vendors samples), PICO native samples
