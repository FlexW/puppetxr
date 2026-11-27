# TODO

**Current focus:** real apps as tests (step 12): make the blocked Meta samples run by implementing what they need, then core apps and Godot. Extensions land with their CTS tests and, where a sample uses them, its sample test. Verify the Pending verification list below on real machines.

Steps are ordered; each ends with green tests and updated docs. Delete items as they land; git history keeps what is done.

## Pending verification

Implemented and tested in CI, not yet on the real targets. Each needs a run on real hardware;
delete it, or fix what fails with a regression test.

Linux release (`tools/dist/dist.sh`, docs/building.md "Linux releases"):
- [ ] the tarball runs on an old distribution (e.g. Debian 10) and a new one

Source release (`tools/dist/dist-src.sh`, docs/building.md "Source releases"):
- [ ] the unpacked `-src.tar.gz` configures, builds and passes the tests with the network off

macOS (step 10; docs/building.md "macOS", "macOS releases"):
- [ ] by hand: Unity's editor in Play mode with the MRUK project, played with keyboard and mouse (a click takes the mouse, moves turn, holding the left button drags MRUK's panel, Escape gives it back, WASD and QE never stick, 1 2 3, ] and [ switch profiles, Tab shows the panels, Fn+F1 the help, about 120 fps, the window maximized at once); Godot's `openxr_character_centric_movement` windowed
- [ ] HiDPI: `UiTest.panels_fill_windows_of_high_pixel_density` skips on a display of one pixel per point; run on a Retina display
- [ ] the extracted tarball on another Mac, through its manifest and as the active runtime; an Intel Mac for the x86_64 slice
- [ ] Linux `gcc-debug` in the pinned image (`tools/samples/run.sh --unit`) run x86_64 under Rosetta on an ARM Mac. `OxrVulkanRealWindowTest.frames_never_wait_for_the_display` took 1.5 s for 60 frames (bound 300 ms) on emulated lavapipe. Done when the run passes, or each failure is fixed or skips where it cannot run
- [ ] a key that a remapper (e.g. Karabiner-Elements) makes send the backtick may reach no `grave` binding on an ISO Mac: `src/platform_macos.mm` swaps `kVK_ISO_Section` and `kVK_ANSI_Grave` when the last keyboard used is ISO (`iso_keyboard`), not the one that sent the event. Done when the swap follows the sending keyboard, or stays off for synthetic events, with a `Platform` test
- [ ] the help names `grave` by its symbol on the layout (`os_key_label`); on an ISO Mac keyboard with a US International layout that is § (± shifted) where people expect the backtick. Only shows when `controls.ui` or a binding is `grave`
- [ ] `mac-tsan`: rerun on the native platform layer; `OxrMetalRealWindowTest.frames_composite_and_present_to_the_mirror_window` deadlocked inside the TSan runtime under SDL. Done when it passes, or skips under TSan with the runtime named

Native platform layer (SDL3 replaced; docs/architecture.md "Platform layer"). Linux (Wayland,
X11), Windows and macOS pass all tests:
- [ ] Wayland input methods by hand (`zwp_text_input_v3`; `Platform.text_input_turns_on_and_off` only covers an unfocused window, which never gets the enter): Japanese or Chinese through IBus on GNOME into a panel's text field, the candidates under the field
- [ ] Wayland title bar: the right-click window menu and minimizing from it

Windows (step 10; docs/building.md "Windows", "Windows releases"):
- [ ] the mirror window, UI and input on the window thread by hand; puppetxrctl against a running app outside the tests
- [ ] the unpacked zip with `install-runtime.ps1` (and `-Uninstall`) runs hello_xr; Windows 10 1809 still loads the DLL

Release workflow (`.github/workflows/release.yml`, docs/building.md "Releasing"):
- [ ] the control API tests (`ApiControlPausedTest.*`) hang until the ctest timeout (25 min) when the control socket cannot connect (a network namespace without loopback) instead of failing fast

Motion recorder APK (`tools/recorder/build_apk.sh`, docs/recorder.md):
- [ ] measure the pinch in `XR_FB_hand_tracking_aim` that a Quest 3 reports to an app on OVRPlugin: it left it at 0 for the recorder, a plain OpenXR app (docs/recorder.md, On a Quest 3), and puppetxr reports it to every app as OVRPlugin has no other (`oxr_xrLocateHandJointsEXT`)
- [ ] on a Quest 3: `build_apk.sh --run` starts it in the headset with passthrough, the panel ahead; poking RECORD with the index tip records a step and `pull.sh` fetches takes that parse, with both hands, the aim state and the data source; a `push_protocol.sh` protocol replaces the built-in one at the next start. The recorder has only run on puppetxr so far

GPUs:
- [ ] an NVIDIA GPU (only AMD and software renderers so far): the test suite, hello_xr on Vulkan, OpenGL, D3D11, D3D12

## 7. mirror window + controls + ImGui
- [ ] every backend's real-window test asserts a present, as `OxrMetalRealWindowTest` and `OxrVulkanRealWindowTest.the_mirror_presents_at_most_at_the_display_rate` do: the GL and D3D ones check only the composite or the swapchain (`tests_oxr.cpp`, `window_presented_ns`); a Metal mirror without a swapchain went unnoticed that way

## 8b. Devices, controls, hands and props
- [ ] mouse wheel scales the strength of key-held analog inputs
- [ ] more controllers through the slot table, Valve Index and HTC Vive first: a secondary axis slot for trackpads, finger curls and force
- [ ] keyboard tracking checked against a device that tracks keyboards: whether the remote query bit finds the local keyboard, the name and size Meta reports, whether the keyboard space is lost while the cameras do not see it
- [ ] render models: a way to move a put down controller (puppetxrctl, C API, UI)
- [ ] hands: a hand scale override is reported but does not scale the joints; `ready_ext` and action states of a hand outside the cone stay active
- [ ] host gamepad drives the simulator: gamepad input in each platform file, sticks/triggers/bumpers/face buttons to both Touch controllers, View held moves the body, deadzone in sim
- [ ] simulated OpenXR gamepad: `microsoft/xbox_controller` on `/user/gamepad`, a gamepad device with its inputs and four haptic outputs, recording format bump

## 8c. Realistic hands (docs/recorder.md, docs/hand-motion.md)

Ordered: real takes first, then the model fitted to them, then gestures.

- [ ] trim and resample the takes the tests need into `data/motion/takes/`; a second person's session shows which fitted values are personal
- [ ] fingers that lose tracking while the wrist stays tracked, as measured (docs/recorder.md, On a Quest 3): part of the noise model's dropouts (docs/hand-motion.md, Noise)
- [ ] `unreal_isdk_*` differ from their references in more than the ghost hands: the pinned sample sets `bGazeEnabled=True` (`Config/DefaultEngine.ini`), with which the Interaction SDK hides the controllers' rays and reticles; the references have them, recorded with gaze off. With `-ini:Engine:[/Script/OculusInteraction.IsdkRuntimeSettings]:bGazeEnabled=False` in the tests' `args` the rays match the references. Decide gaze on or off, then record
- [ ] `unreal_mruk_vr_preview` hangs where Meta's telemetry consent was never answered: its editor plugin opens a modal dialog at startup (`OculusXRPrivacyNotification.cpp`), `-unattended` does not hide it. Answer it once in the editor, or find a setting that skips it without answering
- [ ] `unity_mruk_passthroughrelighting` differed in 1 of 7 runs (Oppy's idle animation, up to 11111 pixels), the first one after the build, whose scene loaded slower (1.5 s against 0.9 s); the polls for the loaded scene were as many. Find what of the load follows the wall clock
- [ ] Unreal samples: building one project may recompile an engine module and give the engine a new build id, and projects built before it are refused at start ("The game module ... could not be found"); their build steps count as done. Rebuild the editor targets until none changes after the last one (`cmake/samples.cmake`). Done when a fresh build tree passes `samples_unreal`
- [ ] a test for the sample driver's start (`App.__init__`, `tools/sample_tests.py`): an app whose first instance and discovery file go while it waits is not killed as without a simulator
- [ ] Windows: `win-clang-asan`, `win-clang-tidy`, `msvc-asan` (see the full tier in step 9), the samples and D3D conformance. The 6 `OxrD3DRealWindowTest` mouse tests and `Platform.pushed_mouse_events_outrank_the_real_cursor` fail in a session without a desktop; have them skip there
- [ ] Windows: `OxrAnchorTest.erased_anchors_are_gone` failed once, the anchor store could not be written (30 repeats passed). `os_write_file_atomic` does not retry `MoveFileExW` when another process has the file open; a test holding the file open should reproduce it
- [ ] the pinch pose mixes two habits: in the takes the right hand pinches with the other fingers open (tips 93 mm from the palm), the left curls them (49 mm); `pinch`, one medoid of both, sits between (65 mm; `tools/hand_fit.py`). Fitting it to one hand changes the grasp a pinching hand reports, which is derived from the curl: a Quest 3 reported a grasp of 1.0 in the right hand's pinch takes but 0.48 at the median for pinches with open fingers across all takes, so `grasp_ext` is no function of curl and pinch alone. Record pinches with the other fingers open and curled, both hands, and fit the grasp to the finger joints' angles before choosing; done when `SimGesture.built_in_gestures_look_like_their_recordings` holds pinch_tap to the right hand's pinch within 2 cm and the grasp test still matches the headset
- [ ] `open_palm` means the palm forward, fingers up (a stop sign); the `poses` take showed an open palm as the palm up, held out. Decide which apps expect; a check of the wrist's back (watch face up, palm down, as in two of the three `palm_up` repeats) would be a gesture of its own
- [ ] gesture paths beyond `to` and `home`: `circle`, `twist`, `shake`, `swing`, `curve`, `throw` (docs/hand-motion.md, Throw), and solved shapes (`around` a handle, ball or dial)
- [ ] gestures on controllers: paths move the grip (anchor `tip` for poking), strengths set trigger and squeeze, a step's `inputs` (docs/hand-motion.md, Controllers)
- [ ] noise: jitter, tremor, sloppiness, dropouts, fitted to the takes, seeded
- [ ] recordings hold the joint rotations and strengths of each tracked hand: `REC_FORMAT_VERSION` 7 with a reader; versions 1 to 6 still read
- [ ] gesture events for observed edges (a pinch strength crossing 0.5 up and down, tracking lost and found) (docs/hand-motion.md, Control API and CLI)
- [ ] the rest of the gesture library (docs/hand-motion.md, Library, Planned), each checked against its takes (`data/motion/takes/`), and an app test that `idle` and `talking_hands` trigger nothing
- [ ] clip steps: a user's take plays as a gesture, inside one or alone

## 9. robustness + CTS
- [ ] the CTS with `-E <extension>` for each implemented extension it tests (XR_EXT_hand_tracking, XR_EXT_dpad_binding, XR_EXT_eye_gaze_interaction and the rest per docs/extensions.md), through `puppetxr_add_cts_test` (`cmake/cts.cmake`); done when each passes and is in the results table
- [ ] a test generated from the registry (as `tools/gen_trace.py` is) that calls every exported `xr*` with null and garbage handles, null pointers and wrong `type`s; done when each returns an error and none crashes under `clang-asan`
- [ ] an app that calls `xrGetVulkanGraphicsDevice2KHR` with the mirror window up, never makes a session, and destroys its `VkInstance` before `xrDestroyInstance` aborts in the Vulkan loader: the mirror window's surface is made on the app's instance in `xrGetVulkanGraphicsDevice2KHR` (`oxr_vk_ensure_surface`, `src/oxr_vulkan.cpp`) and freed through it in `destroy_instance` (`oxr_vk_release_surface`). Repro: in `OxrWindowedTest`, `create_vulkan()` alone; the fixture's `TearDown` destroys the `VkInstance` first. Done when that test passes, e.g. with the surface made at `xrCreateSession`, where the spec requires the binding's instance to be alive
- [ ] build provenance of the release files (`actions/attest-build-provenance` in the `release` job of `.github/workflows/release.yml`, permissions `id-token: write` and `attestations: write`); done when `gh attestation verify <file> --repo FlexW/puppetxr` passes and docs/building.md "Releasing" says how to verify
- [ ] Windows: sign the DLL and puppetxrctl.exe (needs a code signing certificate)
- [ ] Windows in the full tier: the other presets (`win-clang-release`, `win-clang-asan`, `win-clang-tidy`, `msvc-release`, `msvc-asan`), `samples` and the release build uploading nothing in `ci-full.yml`. Done when they run on `windows-2022`
- [ ] macOS in the full tier: `mac-release`, `mac-asan` and `samples` in `ci-full.yml` (`docs/building.md`, macOS). Done when they run on `macos-15`
- [ ] the OpenGL tests on the Windows runner: 80 skip as it has no OpenGL driver. Mesa's `opengl32.dll` and `libgallium_wgl.dll` of `.github/actions/windows-vulkan` next to the test binary should provide one (`tools/sample_tests.py` does this for Godot); done when they run in `ci-fast.yml`
- [ ] a compiler cache for the Windows and macOS jobs of `ci-fast.yml`, which rebuild everything each run (`.github/actions/unit` has it on Linux); clang and MSVC on Windows need embedded debug info for it. Done when a run without source changes builds in under a minute
- [ ] threads: clang Thread Safety Analysis (`-Wthread-safety`; the runtime mutex as one global capability, `OXR_Lock` a scoped capability, runtime state `GUARDED_BY` it, `*_locked` helpers `REQUIRES` it) so a missing lock fails the build; macros empty on GCC
- [ ] `cts_composition` and `cts_scenarios` in `ci-full.yml`, in the image of `tools/samples/Dockerfile` (Mesa 26.2.4); done when they run there
- [ ] puppetxrctl: event stream (session state, profile changes, log lines), screenshots returned over the socket for remote simulators
- [ ] scenario tests of extensions: palm_pose, eye gaze, dpad, plane detection, spatial anchors
- [ ] the hand tracking scenarios take no screenshot of the hands; add one each with references made on Mesa 26.2.4 lavapipe (`--update`, then review)

CTS results (1.1.63, non-interactive, `-H -T`, 2026-11-24 on an AMD Radeon RX 6700 XT):

| pass | command | result |
|---|---|---|
| cts_vulkan2 | `"~[interactive]" -G Vulkan2` | 103 cases, 0 failing (65 run, 38 skipped for missing extensions) |
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
- [ ] same-entity spaces stay tracked when the entity is untracked (spec 7.4): `locate_relative` (`src/oxr.cpp`) needs to know both spaces belong to one device; test: a hand's aim located in its grip outside the tracking cone reports TRACKED
- [ ] optional: PRIMARY_MONO and PRIMARY_STEREO_WITH_FOVEATED_INSET view configurations, MSAA swapchains

## 10. Windows + macOS
- [ ] Windows apps: Unity on D3D12 (the `unity_mruk_*` tests run on D3D11; hello_xr on Windows is in step 12)

OpenGL and OpenGL ES (native backend over the app's share group, GL 3.3 core and ES 3.0; Godot compatibility renderer):
- [ ] MSL validated at build time where Xcode's `metal` compiler is found (`xcrun -f metal`), as glslangValidator and dxc check the other variants: `cmake/shaders.cmake` writes the text with the prelude `render_metal.mm` prepends and runs `xcrun metal -std=metal2.3 -fsyntax-only` on it. Today a dialect mistake shows up only in `RenderTest.every_shader_makes_a_pipeline/Metal`. Done when a broken MSL macro fails a macOS build with Xcode
- [ ] GLES depth readback (ES 3.0 reads colour only); cube array swapchains on GL (samplerCubeArray variant)
- [ ] Intel HD Graphics 620 on Windows 10 (driver of its Vulkan 1.3.215): `Apis/OxrGLTest.sessions_are_created_on_a_thread_without_a_current_context/OpenGL` fails, `wglCreateContextAttribsARB` returns no context when sharing with the app's context from a thread on which only `begin_wgl_bootstrap`'s legacy context is current (`render_gl.cpp`, `create_wgl_context`). The CTS multithreading test creates sessions this way. Start by creating the shared context while no context is current, or on a hidden window of our own with the app's pixel format. Done when the test passes on that GPU
- [ ] CI with an X server that has GLX and XFree86-VidMode for cts_opengl (Xorg dummy driver)
- [ ] show the offered graphics APIs (and disabled extensions) in the UI, `puppetxrctl status` and the C API status
- [ ] an EGL app with a window (X11 and Wayland) to test the EGL mirror path with; only GLX is tested with an app
- [ ] `mingw-debug` in Wine (Xvfb, no window manager): all pass but `OxrD3DRealWindowTest.first_frame_has_the_shown_windows_size` and `a_click_on_a_panel_leaves_the_mouse_to_the_panels` (Wine's X11 driver maximizes after the frame the test checks: the window grows to 1920x1054 once the swapchain was made at 1280x720, and the click at (1000, 600) comes before), and `example_e2e`, `install_relocated`, `body_dump` when run as root, where the OpenXR loader ignores `XR_RUNTIME_JSON` (an elevated context). Done when the D3D tests wait for the size Wine settles on, or skip in Wine with the reason
- [ ] the control server reads back every frame synchronously (`comp_snapshot`, `xrEndFrame`): copy into a staging buffer with a fence and read it when a reader asks; done when the Performance tab shows no snapshot zone in frames nobody reads
- [ ] an MRUK sample test that drags a panel (EnvironmentPanelPlacement: aim with `point right <u> <v> left grip -60`, hold the trigger, move)
- [ ] a regression test for IMM32 staying loaded after the loader unloads puppetxr on Windows (the windows driver loads and pins it): `puppetxr_tests` imports IMM32 itself, so it needs a program that loads the runtime through the loader, destroys its instance and then creates a window; `puppetxr_example` under `procdump -e` crashed in 16 of 120 runs with OBS's capture hook injected before the fix and in none after
- [ ] a recording made on x86_64, checked in, replays to the same `sim` state hashes on arm64 (`mac-debug` in `ci-fast.yml`)
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
- [ ] Windows sample references re-recorded with the Quest-like defaults: `tools/sample_tests.py` runs the `unity_*` and
  `unreal_*` tests with `PUPPETXR_ENVIRONMENT_DEPTH=eyes` and `PUPPETXR_ENVIRONMENT_RAYCAST=mesh`, as their references
  were recorded. Re-record on Windows with the editors those that show depth or raycast (`unity_depthapi_*`, the MRUK
  occlusion scenes, `unreal_mruk_environmentraycast`, `unity_mruk_environmentpanelplacement`), those with translucent
  passthrough, and the Unreal ones that show the rooms in passthrough, the ghost hands at the controllers, tracked and
  body tracked hands or the menu's room details (`unreal_mruk_*`, `unreal_shared_anchors_*`, `unreal_movement_*`,
  `unreal_passthrough_*` with their RHI variants), each looked at against its old reference; drop the pins.
  `unreal_occlusion_hands` is not compared. Done when `tools/sample_tests.py` passes twice on Windows without them
- [ ] environment depth: hands and controllers. Quest 3's depth shows the user's hands and forearms (and hand
  removal takes the hands out, from the wrist, within a frame or two, the forearms stay); puppetxr's has the room
  and the props. Start: the hands' and controllers' shapes as `W_Instance`s next to the props' (`prop_instances`,
  `src/oxr.cpp`; `w_create_mesh_model` from the hand meshes); `OXR_DepthProvider::hand_removal` then leaves the
  hands out. Done when a gtest sees a hand in the depth with removal off and the wall behind it with removal on
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

## 12. Real apps and samples as tests

Fetched at a pinned commit, patched from `tests/samples/<app>/`, driven by `tools/sample_tests.py`
through puppetxrctl, judged by screenshots against references (`docs/testing.md`, Sample apps).
Mostly Linux; on Windows the Godot demos (Vulkan) and Meta's Unity MRUK samples run.

- [ ] llvmpipe with rasterizer threads leaves stale 64-pixel tiles in XrSceneModel (gone with `LP_PERF=no_depth`, so llvmpipe's threaded depth test). Next: an apitrace replayed with and without threads, then a Mesa report; until then `LP_NUM_THREADS=0`
- [ ] sample references identical on every x86 CPU, then `--tolerance 0` in `tools/samples/run.sh` and `cmake/samples.cmake`: llvmpipe JITs for the host CPU (AVX-512 suspected; `GALLIUM_OVERRIDE_CPU_CAPS=avx` or `avx2` do not change it). Pin the feature in `run.sh` and remake the references in the image; done when two machines with different CPUs pass the same references
- [ ] XR_METAX1_passthrough_camera_data on Linux: MRUK's Unity `PassthroughCameraAccess` should get frames there too; add a Unity sample test for it (`tools/sample_tests.py`, `unity_mruk_*`). Meta's `Unity-PassthroughCameraApiSamples` need a newer MRUK: MRUK 85 reads the camera on Android only and its player starts no OpenXR session on Windows. Done when that test passes on Linux
- [ ] the other unpublished Meta extensions (docs/extensions.md, "Unpublished"): `XR_METAX1_room_entity_info` next; each needs its layouts from Meta's binaries
- [ ] Meta's XR Operator layer drives the Meta XR Simulator through `XR_METAX1_simulator_compositor_output_capture` and `XR_METAX2_simulator_head_pose`; offering both would let it drive puppetxr too (same header problem)
- [ ] XR_FB_foveation_vulkan: fragment density map images per swapchain image; puppetxr would add VK_EXT_fragment_density_map to the device extensions it asks for when the GPU has it. Done when a test on a GPU with the extension renders with the map (lavapipe lacks it)
- [ ] the other 1.1 Touch names, `meta/touch_controller_rift_cv1` and `meta/touch_controller_quest_1_rift_s`, are accepted but not emulated
- [ ] XrBodyFaceEyeSocial: XR_FB_body_tracking, XR_FB_face_tracking2, XR_FB_eye_tracking_social, or a fixed exit path
- [ ] more core apps: KhronosGroup/OpenXR-Tutorials, janhsimon/openxr-vulkan-example, maluoi/openxr-explorer
- [ ] trackpad dpads (`dpad_center`, click and force activation) once a simulated controller has a trackpad; binding modifier haptics other than `XrHapticVibration`
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
- [ ] Startup: puppetxr's share is about 200 to 250 ms of 1.4 s (MRUK player) to 2.1 s (editor). Each of Unity's two or three instances loads the room again (about 23 ms each, the albedo PNG decode); keep the decoded room across instances of a process. The compositor (21 ms), the window (16 to 23 ms) and the first event pumps on macOS (32 and 18 ms) are next
- [ ] the props motion drop-down closes the QR code entry when clicked with a real mouse in the Unity editor; synthetic clicks work. Reproduce with a real mouse, add a `UiTest`
- [ ] UI click tests for every panel: each button, checkbox, combo and drag of the Simulator, Devices, Virtual keyboard, Props and Log panels and the menus, through `UiClicker`
- [ ] The Unity MRUK sample tests enter every scene from the start menu, with the session running; one scene started directly (as the editor does) would cover MRUK's discovery in `Awake`
- [ ] `unity_mruk_oculus` allows 6000 differing pixels: OVRPlugin paces Unity's frames on the Oculus loader, not locked to puppetxr's steps, so Oppy's idle animation lands a frame apart now and then, and its walk is skipped. Lock the frames (find what OVRPlugin's frame thread waits on), or check occlusion directly (the table's pixels where Oppy stands in the OpenXR view).
- [ ] Unity MRUK scenes beyond loading: throw in FindMultiSpawn, place panels in EnvironmentPanelPlacement, move KeyboardTracker's keyboard and QRCodeDetection's code, occlusion of Oppy in PassthroughRelighting (needs Depth API in the Windows player). In a full Windows run on 3 jobs KeyboardTracker shows other log lines and EnvironmentPanelPlacement a hover outline on its panel; both follow the wall clock. Find what is not stepped; done when both pass in a parallel run
- [ ] Report to Meta: in Unreal's Occlusion sample every virtual object is near black from Meta's plugin (docs/samples.md, Unreal Occlusion sample: `InvAlphaTexture` lacks the sRGB flag on D3D); puppetxr's depth, the matrices and the occlusion result are right (RenderDoc). The tests run unchanged and are not compared; once the plugin is fixed, record their references and compare them
- [ ] Unreal Occlusion sample: the modes are a combo box on a poke panel (`IsdkInteractableWidget_0`, `IsdkPokeInteractablePanelConfig`) that stands low and tilted in front of the player: world (0, 0, 80) cm, stage about (0, 0.8, -0.4) m, below the view when looking ahead; the test still presses A. Pushed there (head pitched 40 degrees down, the right grip at stage (0, 0.8, -0.25) to (0, 0.8, -0.45)), the panel's `IsdkPokeInteractable` goes HOVER, then SELECT. The panel is `WBP_UI` on a 1024x500 widget component at scale 0.1 (102.4 x 50 cm), yaw 180, pitch 10; its `OcclusionsComboBox` (No, Hard, Soft Occlusions; Soft at start) and `HandsRemovalCheckBox` (checked) are widget properties of that name, and the game mode's `CurrentOcclusionMode` and `HandsRemovalEnabled` show the effect. `unreal.SlateLibrary` (`local_to_absolute`, `get_local_size`) gives the items' places on the panel. Next: poke each item at its place, check the state changes, then compare each mode in `unreal_occlusion_modes`.
- [ ] Unreal Passthrough sample: PTMasked's circle on the left controller after A shows no passthrough; compare with the brush, which paints passthrough (`unreal_passthrough_maskedbrush`). An editor probe of a Blueprint-only project needs `puppetxr.VRPreview`: copy the built `Plugins/SampleEditor` of the MRUK sample into its build copy for the probe (not into the patch)
- [ ] Unreal Passthrough sample: click through the remaining widgets (PTStyles' contrast, brightness and posterize sliders, PTColorMap's entries, PTColorLut's weight and type, PTColorScaleAndOffset's stop) and compare each style; PTStyles' curves and edge alpha are tested. A slider dragged with the ray follows only the first steps: the edge alpha stops at 0.42, the LUT weight barely moves; find what the Interaction SDK's ray needs to keep the drag (hand speed, hover, a pinch strength)
- [ ] Unreal Interaction SDK sample: the levels are compared after 600 frames, with tolerance 3 on up to 2000 pixels: their fog settles over the first seconds as far as the load took in wall-clock time, and a few widgets (a torch, a card's icon) animate in wall-clock time. Find what drives them (Unreal's real time in a material or widget animation) to compare at 0. Interact in each level: the controller pokes PokeExamples' red button (`unreal_isdk_poke`); poke the other buttons, with a finger too, select with the ray, grab and throw, transform, gaze, hand poses. TransformerExamples crashes in `UIsdkHandGrabPose::InitializeDebugVisual` (it asks for the editor's world under `-game`); run it in the editor's VR Preview instead
- [ ] Report to Meta: its Unreal plugin (`OculusXRLayerExtensionPlugin.cpp`, `UpdateCompositionLayers_RHIThread`) passes `xrGetRecommendedLayerResolutionMETA` the display time of the frame its RHI thread submits, older than the last `xrWaitFrame`'s, which the extension makes `XR_ERROR_TIME_INVALID`, and logs an ensure for each refusal. puppetxr answers such a time for the plugin (`accept_older_frame_time`, on with `UnrealEditor-OculusXRHMD.dll` loaded, docs/config.md Known modules); once the plugin passes the waited frame's time, drop that row
- [ ] Unreal desktop-renderer variants differ from their references and from run to run: `unreal_passthrough_startup_{d3d11,d3d12,vulkan}_desktop`, `unreal_passthrough_styles_{d3d11,d3d12,vulkan}_desktop` (thousands of pixels, by 1 on D3D11, up to 20 on Vulkan) and `unreal_mruk_menu_vulkan_desktop` (about 600 pixels, up to 118), one at a time. The mobile renderer's variants match. Find what in the desktop renderer depends on more than the stepped frames (temporal AA history, auto exposure, a wall clock), then pin it in the test's `-dpcvars` or compare these at a tolerance
- [ ] Unreal shaders compiled cold: `unreal_shared_anchors_session_{d3d11,vulkan}_desktop` showed grey-grid menus on their first run, which compiled the shaders cold: Unreal draws a default material while it compiles asynchronously. The Unreal tests compile synchronously now (`bAllowAsynchronousShaderCompiling=False`); done when a run with an emptied `DerivedDataCache` of a sample project matches the references
- [ ] Report to Meta: Unreal's Vulkan renderer draws parts of two scenes wrong, with a warm shader cache, while D3D11 and D3D12 are right and puppetxr composites whole images the same way on every API: `unreal_passthrough_startup_vulkan_desktop` (grey noise on Oppy's head, no reference) and `unreal_isdk_poke_vulkan_mobile` (table and furniture black, not compared). The validation layer reports errors in Unreal's own commands on the mobile renderer (`viewMask-06178`, `viewType-07752`, `shaderTileImageColorReadAccess-08718`). Once fixed, record and compare both
- [ ] Unreal Shared Anchors sample: Create Session, Create Anchor (`unreal_shared_anchors_session`) and sharing with users between two players (`unreal_shared_anchors_sharing`) are tested. Next: Load Anchors (from the store after a restart), Unsave and Erase, Orient to Anchor, the cubes (Spawn Cube, Explode Cube) replicated to the peer
- [ ] Unity Starter Samples: Stereo180Video draws black in the app's own projection layer (its Media Foundation video); OVROverlayCanvas shows some labels mirrored, which needs a device's picture to judge
- [ ] Unity Depth API sample: grab objects with the controllers
