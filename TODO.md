# TODO

**Current focus:** verify the macOS port on the other platforms (the Linux presets, Windows and Wine; Pending verification), then real apps as tests (step 12): make the blocked Meta samples run by implementing what they need, then core apps and Godot. Extensions land with their CTS tests and, where a sample uses them, its sample test. Verify the Pending verification list below on real machines (Windows port, the release scripts and workflow).

Steps are ordered; each ends with green tests and updated docs. Check items off as they land.

## Pending verification

Implemented and tested on Linux (Windows code under Wine, release scripts with software
rendering), not yet on the real targets. Each needs a run on real hardware; check it off, or fix
what fails with a regression test.

Linux release (`tools/dist/dist.sh`, docs/building.md "Linux releases"):
- [x] the image builds: the dnf package names in `tools/dist/Dockerfile` (gcc-toolset-15, clang 21, lld, powertools), clang finding gcc-toolset-15's `libstdc++.a`, the pinned shaderc
- [x] `tools/dist/dist.sh` passes all tests inside the container on its lavapipe (834), including `shipped_binaries_need_only_glibc` at glibc 2.28
- [x] two runs from a clean build directory give the same `.buildinfo`, checksums included (the image line hashes the layers, as BuildKit gives each build a new image id)
- [ ] the tarball runs on an old distribution (e.g. Debian 10) and a new one

Source release (`tools/dist/dist-src.sh`, docs/building.md "Source releases"):
- [ ] the unpacked `-src.tar.gz` configures, builds and passes the tests with the network off

macOS (step 10; docs/building.md "macOS", "macOS releases"):
- [x] `mac-debug` passes all tests on Apple silicon (macOS 27, Apple clang 21, MoltenVK 1.4); `mac-asan` too; GCC 16 compiles the sources warning free (`-fsyntax-only`; SDL's Cocoa code needs clang, so no GCC build)
- [x] the `samples` entry: the Godot demos' `_vk` tests and hello_xr Vulkan and Vulkan2 on the Apple GPU; `samples_unity`: the 13 MRUK tests on a Metal player
- [x] `tools/dist/dist-macos.sh --no-tests`: universal tarball, the dylib check, a .dSYM with the LTO code for both architectures; two fresh runs give byte-identical tarballs; the extracted tarball runs an app through its manifest
- [x] Unity's editor (6000.0.66f2) in Play mode with the MRUK project, entered by script: about 100 frames a second with the control server reading back every frame, no withdrawn window call
- [ ] by hand: Unity's editor in Play mode with the MRUK project, played with keyboard and mouse (a click takes the mouse, moves turn, holding the left button drags MRUK's panel, Escape gives it back, WASD and QE never stick, 1 2 3, ] and [ switch profiles, Tab shows the panels, Fn+F1 the help, about 120 fps, the window maximized at once); Godot's `openxr_character_centric_movement` windowed
- [ ] HiDPI: `UiTest.panels_fill_windows_of_high_pixel_density` skips on a display of one pixel per point; run on a Retina display
- [ ] the extracted tarball on another Mac, through its manifest and as the active runtime; an Intel Mac for the x86_64 slice
- [x] `mac-release` passes all tests
- [x] the `samples` suite under load: the Godot walk tests' `walked` shot showed the frame before now and then. Physics, input and the world origin Godot hands its renderer were the same in every run, and puppetxr composited each frame's fresh image (also with the queue idle before compositing); Godot on MoltenVK drew some frames with the uniforms of the frame before. The shot now waits until the player stopped; the Linux `gl` references of every Godot demo, older than the lens mask and the 60 degree aim, were made again
- [ ] Linux `gcc-debug` in the pinned image (`tools/samples/run.sh --unit`) run x86_64 under Rosetta on an ARM Mac: rerun now that Linux has a window thread. The 5 `OxrThreadWindowedTest` aborts were SDL's assert that only its main thread pumps (`SDL_PumpEventsInternal`), hit by a frame ended off the main thread; `Platform.showing_a_maximized_window_does_not_wait_for_an_animation` skips where no window manager maximizes. `OxrVulkanRealWindowTest.frames_never_wait_for_the_display` took 1.5 s for 60 frames (bound 300 ms) on emulated lavapipe. Done when the run passes, or each failure is fixed or skips in the environment that cannot run it
- [x] `OxrThreadWindowedTest.frames_never_wait_for_a_main_thread_blocked_on_them` and `taking_the_mouse_never_waits_for_a_main_thread_blocked_on_frames` failed under load (slow frames on hosted macOS runners and lavapipe, no withdrawn call in the log): they now assert that no window call was withdrawn while the frames ran, not a frame time. `OxrVulkanRealWindowTest.frames_never_wait_for_the_display` keeps its wall-clock bound (55 of 60 display periods) and still fails on lavapipe under load
- [ ] a key remapped by Karabiner-Elements to send the backtick (for a Logitech MX Keys) reached no binding when the key left of 1 was the UI key (Tab is now). Likely SDL's ISO swap (`SDL_cocoakeyboard.m`: key codes 10 and 50 swap when `KBGetLayoutType(LMGetKbdType())` says ISO, and that is the last keyboard used, not the one that sent the event). Matters to anyone binding `grave`. Run an app with `SDL_EVENT_LOGGING=1`, press the key, read the scancode; then decide the fix and add a test to `PlatformAppEventsTest`/`Platform`
- [ ] the help names `grave` by its symbol on the layout (`os_key_label`); on an ISO Mac keyboard with a US International layout that is § (± shifted) where people expect the backtick. Only shows when `controls.ui` or a binding is `grave`
- [ ] `mac-tsan`: 939 of 941 pass with Apple clang 21; `OxrMetalRealWindowTest.frames_composite_and_present_to_the_mirror_window` deadlocks inside the sanitizer runtime (TSan's `LibIgnore` lock against dyld's loader lock: SDL's `SDL_Metal_CreateView` `dlclose`s the Vulkan loader of the window while AppKit's dock thread `dlopen`s), no puppetxr frame involved. Done when it passes, or skips under TSan with the runtime named

Native platform layer (SDL3 replaced; docs/architecture.md "Platform layer"). Linux (Wayland,
X11) passes all tests; the Win32 and Cocoa drivers are written but have not run yet:
- [ ] `win-clang-debug`, `msvc-debug` and `mingw-debug` build `src/platform_win32.cpp` and pass all tests; by hand: the mirror window, the panels (typing, the clipboard, cursor shapes, the input method's candidates under a text field), looking with the mouse held (raw input, the cursor clipped and put back), maximize, the window thread's wake (no 100 ms waits)
- [x] `mac-debug`, `mac-asan`, `mac-tsan` and `mac-release` build `src/platform_macos.mm` and pass all tests (1067) on Apple silicon, macOS 27, a 1080p display: the real Cocoa windows with Metal and Vulkan mirrors, `PlatformAppEventsTest.*`, and `OxrMetalRealWindowTest.frames_composite_and_present_to_the_mirror_window` under TSan, which deadlocked with SDL's Metal view
- [ ] macOS by hand: the mirror window and panels with a real keyboard and mouse (typing, the clipboard, cursor shapes, an input method's candidates), looking with the mouse held, maximize, the ISO swap of the keys left of 1 and Z, Unity's editor with its own NSApp; Retina density (`UiTest.panels_fill_windows_of_high_pixel_density` skips on 1080p)
- [ ] Wayland input methods by hand (`zwp_text_input_v3`; `Platform.text_input_turns_on_and_off` only covers an unfocused window, which never gets the enter): Japanese or Chinese through IBus on GNOME into a panel's text field, the candidates under the field
- [x] Wayland title bar on GNOME by hand: moving, double click, the buttons (since then libadwaita's dark look with a close button alone), fractional scaling, dimming when inactive; typing into a text field (the QR code entry) failed while the text input was on, as GNOME passes the keys no input method takes as keys and their text was dropped: a test needs a window with the keyboard focus, which test runs never get
- [ ] a mouse leaving the window keeps ImGui's last hover until the next move on all platforms (no leave event yet)
- [ ] Wayland title bar by hand on GNOME (`docs/architecture.md`): move, resize from every edge and corner, double click, the window menu, minimize, maximize and restore, close; at 100 % and a fractional scale

Windows (step 10; docs/building.md "Windows", "Windows releases"):
- [x] `win-clang-debug` and `msvc-debug` build warning free and pass all tests (Visual Studio 2026, MSVC 14.44 and 14.51, clang 22.1.6 and 23.1.3); `win-clang-tidy` clean with the pinned clang-tidy 23.1.3
- [x] `win-clang-asan`, `msvc-asan` (the static OpenXR loader takes the sanitized DLL runtime), `win-clang-release`, `msvc-release` and `msvc-dist` build and pass their tests
- [x] `mingw-debug` under Wine after the Windows fixes (WSL Ubuntu 24.04, Wine 9 under WSLg): all tests but those Wine cannot run (docs/testing.md, Windows builds under Wine)
- [x] D3D11 and D3D12 on WARP (the test suite) and on an AMD Radeon RX 6700 XT (the CTS); WGL on the same GPU (the test suite and the CTS)
- [ ] the mirror window, UI and input on the window thread by hand; puppetxrctl against a running app outside the tests
- [x] `tools\dist\dist.ps1` on the pinned toolchain (clang 23.1.3 from the LLVM release archive, MSVC 14.44 and SDK 10.0.26100 of Visual Studio 2026, glslc of shaderc v2026.4, PowerShell 7.6): developer shell entry, build, all tests, `shipped_binaries_need_only_windows_dlls` and `shipped_binaries_are_hardened`. Fixed on the way: `-mguard=cf` (MinGW only) broke the clang compile, and lld-link hung on the PDB while third-party code was LTO
- [x] two runs give the same `.buildinfo` checksums (the second from an empty build directory, both on the pinned toolchain)
- [ ] the unpacked zip with `install-runtime.ps1` (and `-Uninstall`) runs hello_xr; Windows 10 1809 still loads the DLL

Release workflow (`.github/workflows/release.yml`, docs/building.md "Releasing"):
- [x] a manual run with `pin` on `main`: `tools/dist/windows-toolchain.json` committed (clang 23.1.3, MSVC 14.44, SDK 10.0.26100); `dist.ps1` keeps the installed clang before the one the VS developer shell puts first
- [ ] a manual dry run on `main` passes all jobs; in particular on the hosted Windows runner, which runs elevated so the OpenXR loader ignores `XR_RUNTIME_JSON`: check `example_e2e` and `install_relocated`. Blocked on 2026-10-22: the account's artifact storage quota was full, so the first upload (`notes` of the `check` job) failed and every build job was skipped; the 3.3 GB of another repository's artifacts were deleted, GitHub recalculates usage every 6 to 12 hours. Run `gh workflow run release.yml --ref main` again; the Linux and source jobs passed locally (`dist.sh` with all tests, `dist-src.sh`, offline build of the full tarball), the Windows job has never run
- [x] `tools/release.sh` checked in a clone with a local bare remote: refuses a bad, older or existing version, uncommitted changes and a no; releasing 0.1.0 and 0.1.1 commits, tags and pushes, and the workflow's notes extraction reads the new section
- [ ] the control API tests (`ApiControlPausedTest.*`) hang until the ctest timeout (25 min) when the control socket cannot connect (a network namespace without loopback) instead of failing fast
- [ ] `tools/release.sh` on the first release: the draft has both Linux tarballs, both Windows zips, both macOS tarballs, both source tarballs, the four `.buildinfo`, `SHA256SUMS`

GPUs:
- [ ] an NVIDIA GPU (only AMD and software renderers so far): the test suite, hello_xr on Vulkan, OpenGL, D3D11, D3D12


## 1. Project docs and skeleton
- [x] AGENTS.md (CLAUDE.md imports it), README.md, LICENSE (MIT), .gitignore
- [x] docs/: README, architecture, style, building, testing, stubs for the layer docs
- [x] CMake skeleton: options, dependencies in third_party/, puppetxr_core object library, libpuppetxr shared library, tests target, manifest generation
- [x] install step: library, relocatable manifest, optional active_runtime.json (PUPPETXR_INSTALL_ACTIVE_RUNTIME)

## 2. platform + base
- [x] platform.h: types, macros, ASSERT/FAIL/LOG, os_* declarations
- [x] platform.cpp on SDL3: memory, time, log, assert, files; headless init without video (since replaced by the native platform layer)
- [x] base.h/.cpp: defer, Array, Handle, SparseArray, HashMap, cstr helpers, FNV-1a, CRC32
- [x] json_* on jsmn
- [x] tests_base.cpp, tests_json.cpp

## 3. zmath (deterministic)
- [x] Vec2/3/4, Quat, Mat3/Mat4, Pose, projection from fov
- [x] own f32 sin/cos/tan/asin/acos/atan/atan2/exp/log/pow with range reduction; sqrt via hardware
- [x] tests_zmath.cpp: golden bit patterns, error bounds, no-FMA check
- [x] docs/determinism.md

## 4. sim
- [x] SIM_Clock: realtime and fixed_step, ns time, display period
- [x] devices head/left/right: pose, velocity, tracking flags
- [x] input sources addressed by path string, values bool/float/vec2
- [x] reference spaces view/local/stage/local_floor, stage bounds
- [x] JSON config (puppetxr.json): devices, window, clock, controls, record/replay paths
- [x] keyboard/mouse control mapping, modal device selection
- [x] tests_sim.cpp

## 5. oxr core (no graphics)
- [x] negotiate, xrGetInstanceProcAddr table gated by extension X-macro
- [x] instance, extension and layer enumeration, properties, result/struct strings
- [x] system, view configurations, blend modes
- [x] paths (string <-> atom), events queue
- [x] session state machine, frame gate (wait/begin/end), null graphics mode for tests
- [x] reference and action spaces, xrLocateSpace, xrLocateViews, xrLocateSpaces
- [x] actions, action sets, bindings, khr/simple_controller and oculus/touch_controller tables, xrSyncActions, action states, bound sources, haptics stub
- [x] tests_oxr.cpp covering every entry point including invalid handles and structs
- [x] docs/openxr.md

## 6. render + Vulkan binding + headless compositor
- [x] render.h API, render_vulkan.cpp over the app's device
- [x] XR_KHR_vulkan_enable and vulkan_enable2
- [x] swapchains: formats, images, acquire/wait/release, layout tracking
- [x] comp.cpp: offscreen composite of projection layers, PNG screenshot
- [x] accept every spec-defined interaction profile path in xrSuggestInteractionProfileBindings (engines suggest many); only touch and simple become active
- [x] hello_xr headless end to end with screenshot
- [x] tests_render.cpp, tests_comp.cpp (skip without a Vulkan device)
- [x] docs/compositor.md

## 7. mirror window + controls + ImGui
- [x] SDL window, window swapchain, blit from offscreen target
- [x] control mapping applied per xrWaitFrame
- [x] ui.cpp: ImGui over our R_Device renderer, panels for devices/inputs/clock/record/screenshot
- [x] hello_xr with head and controllers moving
- [x] Performance tab: frame rate, frame time graph (ImPlot), puppetxr's share, mirror presents a second (`docs/compositor.md`, Performance tab)
- [ ] every backend's real-window test asserts a present, as `OxrMetalRealWindowTest` and `OxrVulkanRealWindowTest.the_mirror_presents_at_most_at_the_display_rate` do: the GL and D3D ones check only the composite or the swapchain (`tests_oxr.cpp`, `window_presented_ns`); a Metal mirror without a swapchain went unnoticed that way

## 8. record/replay + control API + determinism
- [x] rec.cpp: format, writer, reader, replay cursor
- [x] puppetxr.h C API, exported from the library
- [x] replay twice, compare state hashes and screenshots
- [x] example end-to-end test app in apps/
- [x] docs/recording-format.md, docs/control-api.md

- [ ] Windows: puppetxr_load_api via GetModuleHandle is untested on a real Windows machine (Wine runs the tests with the runtime linked in)

## 8b. Devices, controls, hands and props
- [x] recording format: named device and input tables, version dispatch, controller type, writer version
- [x] C API versioning: puppetxr_get_version, negotiated table version, status struct_size, controller get/set
- [x] simulated controller types: Oculus Touch (default) and KHR simple, in config, UI, recordings and API
- [x] Meta Touch Pro and Touch Plus (default) with curl, slide, force, proximity and Touch Pro's extra haptics; FB/META profile extensions
- [x] haptic amplitude envelopes (XR_FB_haptic_amplitude_envelope), sampled at sim time
- [x] display and system queries: refresh rates (clock.refresh_rates), color space, headset id, recommended layer resolution, local dimming, tile hints; refresh rate, color space and dimming in `status`
- [x] passthrough and boundary: boundary visibility (suppressed only with passthrough running), passthrough preferences (passthrough.default_active), keyboard hands layers, layer resumed events
- [x] layers and swapchains: layer settings and automatic filter hints, secure content left out of screenshots, swapchain sampler state (mag filter honoured), extra Vulkan image flags
- [x] XR_META_spatial_entity_semantic_label advertised (the label enum of room mesh)
- [ ] more controller types (Valve Index, HTC Vive) as needed
- [x] controls: body/head/left/right modes (1-4), Shift/Ctrl pick the hand on one key cluster (IJKL sticks, U/O/H/Y/N/M), controller-neutral slots mapped per controller and hand, auto touch, hand reset (0); hands start with a level aim pose (`docs/config.md`)
- [x] analog keys: triggers, grips and sticks ramp (`analog_ramp` 0.5 s, `analog_release` 0.1 s), Space freezes a ramp for partial values; R resets head and hands without velocity; C API and puppetxrctl `mode`, `reset [hands]`, `key`, `mouse` for agents
- [x] the room picked in the UI is the default of the next start (`state.json` in the per-user state dir; config, env and headless runs win)
- [x] the mirror window starts maximized (`window.maximized`)
- [x] UI: one calm style, Simulator panel in collapsing sections, one Devices panel with tabs following the control mode, Help menu; welcome overlay at start (F1 toggles, Enter dismisses for good in `state.json`, Escape for the run, `show_welcome` / `PUPPETXR_SHOW_WELCOME=0` for development)
- [x] room bakes without noise or JPEG blocks: float bake at 2048 samples, OIDN with an albedo guide, the scene's view transform on unclipped values, PNG atlas (`docs/world.md`)
- [x] mouse capture: a click into the view takes the mouse (look, trigger, grip, A/X), Escape or focus loss gives it back, the panels get no input meanwhile; the key left of 1 shows the panels (`docs/compositor.md`, Mouse capture)
- [x] control profiles replace modes 1-4: a profile says what WASD/QE, I/J/K/L (and the arrows, always the same), the mouse, Shift+mouse and the wheel drive (walk the body, move or turn the head or a hand in the view's plane, a thumbstick). Built in: `body` (1, today's mode 1), `stick` (2, WASD the left stick, I/J/K/L the right, the mouse turns the body), `hands` (3, WASD/QE the left hand, I/J/K/L the right, the wheel pushes the right, the mouse turns the right, Shift+mouse the left, Shift+wheel rolls the left; the head stays). Tab and Shift+Tab cycle, the number keys pick; `controls.profiles` adds or overrides (`extends`). Replaces `SIM_ControlMode`, the C API's control mode (version bump) and `puppetxrctl mode`; a toast names the profile and its keys. (`docs/config.md`, Profiles)
- [ ] profiles: hold a key to use a profile only while it is held (a quick look at a wrist panel); profiles chosen per app (known module or app name); a crosshair where the right aim ray points; edit and save profiles in the UI
- [ ] mouse wheel scales the strength of key-held analog inputs
- [ ] more controllers through the slot table: a secondary axis slot for trackpads (Index, Vive), finger curls and force
- [x] hands: per side source controller/hand (none is the existing active flag), trigger pinches, grip grabs, both a fist, grip alone points; procedural FK skeleton (not a pose library) and a generated skinned mesh; per hand interaction profiles with `ext/hand_interaction_ext`; recorded; C API, `puppetxrctl hand`, key T, UI
- [x] hands: microgestures (stick swipes, primary taps, one frame pulses), hand tracking cone and wide motion mode (preview and mode2), recorded
- [x] XR_META_detached_controllers (a put down controller stays where it was, recorded), XR_FB_render_model (procedural controllers)
- [x] XR_META_virtual_keyboard: procedural labelled model (5x7 font atlas, key press animations), ray and direct input with poke limiting, FAR/DIRECT/CUSTOM places; typed text from the UI, `puppetxrctl vkb type` and the C API, recorded per frame
- [ ] virtual keyboard: swipe typing and word suggestions (more dirty textures), the runtime's own hide or show animations, a keyboard per session beyond the first in the UI and control API
- [x] props: keyboards and QR codes (still, back and forth, circle), config `props` / `PUPPETXR_PROPS`, UI panel, C API, `puppetxrctl prop`, recorded; drawn into passthrough with decodable QR codes (`docs/props.md`)
- [x] XR_META_dynamic_object_tracker, XR_META_dynamic_object_keyboard (previews): keyboard props found by FB queries and META discovery
- [x] keyboard tracking turned off in the settings: `keyboard_tracking.enabled` / `PUPPETXR_KEYBOARD_TRACKING=0` fails the tracker's creation (`OxrDynamicObjectTest.a_tracker_turned_off_in_the_settings_fails_to_initialize`); turning it on or off while an app runs (UI, control API) is open
- [x] XR_FB_keyboard_tracking: the oldest keyboard prop as the system keyboard, its keyboard space (`OxrKeyboardTrackingTest.*`)
- [ ] keyboard tracking checked against a device that tracks keyboards: whether the remote query bit finds the local keyboard, the name and size Meta reports, whether the keyboard space is lost while the cameras do not see it
- [x] XR_EXT_spatial_entity with XR_EXT_spatial_marker_tracking: QR props as markers, tracked while in view, paused out of it, stopped once gone
- [ ] spatial entities: an end-to-end app test (no Meta sample uses them), plane tracking and anchors on the room, a UI and `puppetxrctl` view of what each context tracks
- [ ] props: environment depth and the global mesh raycast see them; more kinds (mouse, other markers); XR_BD_dynamic_object_tracking on the same props
- [ ] render models: textured or authored controller and keyboard models (Blender, like the rooms); a way to move a put down controller (puppetxrctl, C API, UI)
- [ ] hands: a hand scale override is reported but does not scale the joints; `ready_ext` and action states of a hand outside the cone stay active
- [ ] host gamepad drives the simulator: SDL gamepad events in platform, sticks/triggers/bumpers/face buttons to both Touch controllers, View held moves the body, deadzone in sim
- [ ] simulated OpenXR gamepad: `microsoft/xbox_controller` on `/user/gamepad`, a gamepad device with its inputs and four haptic outputs, recording format bump


## 9. robustness + CTS
- [x] PUPPETXR_BUILD_CTS: OpenXR-CTS 1.1.63 as an ExternalProject, ctest entries cts_vulkan2, cts_vulkan, cts_headless
- [x] interaction profiles and binding paths of the whole registry, with version and extension availability (tools/gen_profiles.py)
- [ ] extension tests the CTS skips until we implement them: EXT_hand_tracking, EXT_dpad_binding, EXT_eye_gaze_interaction (full list in docs/extensions.md)
- [ ] garbage-input tests for every entry point
- [x] CMake presets (gcc/clang debug and release, clang-asan, gcc-headless in build/<preset>), strict warnings as errors on GCC and clang, all presets green
- [x] LeakSanitizer in clang-asan with tests/lsan.supp for drivers and system libraries; runtime state released when the loader unloads the runtime
- [x] clang-tidy (`.clang-tidy`, `clang-tidy` preset fails the build on findings), code const correct
- [x] `dist` preset: Release, full LTO, hardened, reproducible, stripped with separate debug files, logging asserts, relocatable install
- [x] Linux release builds on glibc 2.28: `tools/dist/dist.sh` in an AlmaLinux 8 container, static libstdc++, glibc floor test, reproducible tarballs (docs/building.md)
- [x] source release: `tools/dist/dist-src.sh`, one tarball with the dependencies of `third_party/` (offline), LFS content resolved, reproducible (docs/building.md)
- [x] dependencies in the repository: `third_party/` holds the trimmed sources, licenses and patched CMake builds, versions in `third_party/README.md`; no download at configure time
- [x] Windows release script: `tools/dist/dist.ps1`, clang MSVC ABI, toolchain pinned by check, DLL import test, Windows 10 1809 API floor, reproducible zips, `install-runtime.ps1` (docs/building.md)
- [x] releases from a tag: `tools/release.sh` (version bump, CHANGELOG.md, tag, push), `.github/workflows/release.yml` builds Linux, Windows (hosted runner, pinned LLVM and shaderc) and source, drafts the GitHub release with SHA256SUMS (docs/building.md)
- [ ] once the repository is public: build provenance of the release files (`actions/attest-build-provenance` in the `release` job of `.github/workflows/release.yml`, permissions `id-token: write` and `attestations: write`; private repositories need GitHub Enterprise Cloud); done when `gh attestation verify <file> --repo FlexW/puppetxr` passes and docs/building.md "Releasing" says how to verify
- [ ] Windows: sign the DLL and puppetxrctl.exe (needs a code signing certificate)
- [x] Linux CI: `.github/workflows/ci-fast.yml` (unit tests of gcc-debug and clang-debug on every push), `ci-full.yml` (label `full-ci`: the other presets, all samples, the release build uploading nothing), docs/testing.md "CI"
- [x] the checks required on `main`: all `unit` jobs of the fast tier and `full-ci`
- [x] Windows and macOS in the fast tier: `win-clang-debug`, `msvc-debug`, `mac-debug` in `ci-fast.yml` (docs/testing.md, CI)
- [ ] Windows in the full tier: the other presets (`win-clang-release`, `win-clang-asan`, `win-clang-tidy`, `msvc-release`, `msvc-asan`), `samples` and the release build uploading nothing in `ci-full.yml`. Done when they run on `windows-2022`
- [ ] macOS in the full tier: `mac-release`, `mac-asan`, `samples` and the release build uploading nothing in `ci-full.yml` (`docs/building.md`, macOS). Done when they run on a hosted macOS runner
- [ ] the OpenGL tests on the Windows runner: 80 skip as it has no OpenGL driver. Mesa's `opengl32.dll` and `libgallium_wgl.dll` of `.github/actions/windows-vulkan` next to the test binary should provide one (`tools/sample_tests.py` does this for Godot); done when they run in `ci-fast.yml`
- [x] `samples` on Intel runners: 8 of 46 tests off by 1 to 12 pixels, 2 or 3 steps apart (an Intel Xeon Platinum 8573C; AMD EPYC 7763, 9V74 and a Ryzen 5800X match at 1); the driver takes `--intel-tolerance 3` on an Intel CPU (docs/testing.md)
- [ ] a compiler cache for the Windows and macOS jobs of `ci-fast.yml`, which rebuild everything each run (`.github/actions/unit` has it on Linux); clang and MSVC on Windows need embedded debug info for it. Done when a run without source changes builds in under a minute
- [x] macOS in `release.yml` (job `macos`, `tools/dist/dist-macos.sh`) and its release build in `ci-full.yml` (`release-macos`); the Linux release image cached (`.github/actions/dist-image`)
- [ ] release binaries of all three platforms: the dry run of `release.yml` (Pending verification), then `tools/release.sh`
- [ ] threads: clang Thread Safety Analysis (`-Wthread-safety`; the runtime mutex as one global capability, `OXR_Lock` a scoped capability, runtime state `GUARDED_BY` it, `*_locked` helpers `REQUIRES` it) so a missing lock fails the build; macros empty on GCC
- [x] threads: tests calling the runtime from two threads as Unreal and Unity do (OxrThreadTest), `clang-tsan` preset; also runs uninstrumented apps with the TSan runtime preloaded (testing.md)
- [x] interactive input tests unattended through XR_EXT_conformance_automation (cts_automation_simple, cts_automation_touch, cts_automation_touch_pro, cts_automation_touch_plus)
- [x] control server and puppetxrctl: discovery with crash-proof lock files, exact version handshake, commands of the C API, wait_frames, shell mode for agents (docs/control-cli.md)
- [x] interactive composition tests on lavapipe: 17 core tests, 32 scenes, paused stepping, approved references in tests/cts/composition (cts_composition)
- [x] puppetxrctl pause, resume, step; control.paused
- [ ] composition tests of extensions once they land: quad views (visibility mask, inverted alpha, image layout, equirect, equirect2 and ProjectionDepth done; cylinder and cube have none)
- [ ] CI image with Mesa 26.2.4 lavapipe for cts_composition
- [x] C API: `set_log_callback` with history replay, for processing runtime logs and breakpoints
- [ ] puppetxrctl: event stream (session state, profile changes, log lines), screenshots returned over the socket for remote simulators
- [x] core scenario tests scripted through puppetxrctl: SpaceOffsets, HapticInterrupt, InteractiveThrow, GripAndAimPose, GripSurface, local_floor (cts_scenarios)
- [x] scenario tests XR_FB_hand_tracking_mesh-interactive and XR_EXT_hand_tracking-interactive: the index tips of both tracked hands meet (cts_scenarios)
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
- [x] READY sessions pass SYNCHRONIZED before STOPPING
- [x] suggested bindings validated against the registry, parent paths (spec 6.3.2)
- [x] grip_surface/pose (1.1) and the grip/aim parent paths on emulated profiles
- [x] action set priority suppression (spec 11.2)
- [x] locate at the requested time: 31 frames of history, interpolation, extrapolation (spec 2.16)
- [x] XrEventDataReferenceSpaceChangePending on recenter, with change time (spec 7.1)
- [x] velocities of offset spaces and against rotating bases
- [x] compositor: quads in space, rotation reprojection of projection views, layer alpha flags, eye visibility (spec 10.6)
- [x] XrEventDataEventsLost on queue overflow (spec 2.22.1)
- [x] swapchain mip chains and arrays up to 256 layers
- [ ] same-entity spaces report tracked even when untracked (spec 7.4); needs simulated tracking loss first
- [ ] optional: PRIMARY_MONO and PRIMARY_STEREO_WITH_FOVEATED_INSET view configurations, MSAA swapchains
- [ ] emulate more controllers than Touch and simple (bindings of all registry profiles validate already)

## 10. Windows + macOS
- [x] Windows build: puppetxr.dll, relocatable manifest, registry note for ActiveRuntime; presets for clang (MSVC ABI) and MSVC: debug, release, asan, dist (`docs/building.md`)
- [x] Windows platform layer: Winsock, lock files, UTF-8 paths and environment, output capture on the shared UCRT, high-resolution clock and XR_KHR_win32_convert_performance_counter_time, SDL video on a window thread of its own, teardown outside the loader lock
- [x] D3D11 and D3D12 render backends and bindings; DXBC at build time through `D3DCompile` (tools/hlsl_compile.cpp)
- [x] WGL binding of XR_KHR_opengl_enable
- [x] mingw-w64 cross preset running the tests in Wine, D3D and WGL included (`docs/testing.md`)
- [ ] Windows apps: hello_xr with -g D3D11, D3D12, OpenGL, Vulkan2; Unity and Unreal on D3D11 and D3D12
- [x] CTS on Windows: D3D11, D3D12 and OpenGL graphics plugins in cmake/cts.cmake, the CTS built with MSVC (docs/testing.md)
- [x] D3D: UAV swapchain usage (`R_ImageUsageFlags::UnorderedAccess`, test `OxrD3DTest.unordered_access_usage_makes_writable_images`)
- [ ] Windows on ARM64
- [x] Metal backend and `XR_KHR_metal_enable` (also as `XR_KHRX2_metal_enable`): `src/render_metal.mm` over the app's command queue, MSL from `spirv-cross` compiled at runtime, mirror window on a `CAMetalLayer`; render, UI and session tests on Metal (`docs/compositor.md`, `docs/shaders.md`)
- [ ] Android binding of XR_KHR_opengl_es_enable

OpenGL and OpenGL ES (native backend over the app's share group, GL 3.3 core and ES 3.0; Godot compatibility renderer):
- [x] shader dialect: one source per stage for Vulkan, GL 330, ES 300 and HLSL (`shaders/shader.h`), push blocks shared with C++ (`shaders/blocks.h`), GL/GLES/HLSL variants validated at build time; Vulkan output bit-identical (`docs/shaders.md`)
- [x] backend-neutral render API: function table dispatch in `render.cpp`, `R_ShaderCode` with SPIR-V, GL text and sampler names, `r_native_format`, `r_flush_buffer`, `R_WindowTarget`, `SparseArray` handle tags
- [x] `render_gl.cpp`: own context shared with the app's, fences on every context switch, GL 3.3 and GLES 3.0 offscreen over EGL; tests_render and tests_ui on every backend (`docs/compositor.md`)
- [ ] GLES depth readback (ES 3.0 reads colour only); cube array swapchains on GL (samplerCubeArray variant)
- [x] `oxr_gl.cpp`: XR_KHR_opengl_enable (Xlib and EGL), XR_KHR_opengl_es_enable (EGL), XR_MNDX_egl_enable; requirements, GL textures as swapchains, flipped GL layers; GLX contexts in render_gl; hello_xr `-g OpenGL` and `cts_opengl` 0 failing (`docs/openxr.md`)
- [x] realtime clock counts its first frame from xrCreateInstance, where time conversions anchor (CTS "Matches frame timing" on OpenGL)
- [ ] CI with an X server that has GLX and XFree86-VidMode for cts_opengl (Xorg dummy driver)
- [x] force the graphics APIs with `PUPPETXR_GRAPHICS` and config `"graphics"`: only their binding extensions are offered; every backend of a platform is always built
- [ ] show the offered graphics APIs (and disabled extensions) in the UI, `puppetxrctl status` and the C API status
- [x] native GL mirror window: GLX (window remade with the app's visual on X11), EGL on X11 and Wayland, ImGui on GL; hello_xr `-g OpenGL` windowed
- [ ] an EGL app with a window (X11 and Wayland) to test the EGL mirror path with; only GLX is tested with an app
- [x] GL composition checked against the Vulkan references (`cts_composition_gl`, tolerance 3, 46 scenes); Vulkan recordings replay in GL sessions with no divergence
- [x] macOS: window calls and the event pump on the main thread when engines call xrWaitFrame elsewhere: main-thread call queue with a 250 ms withdrawal, frames post the window tick (`docs/compositor.md`, One flow on every platform)
- [x] the window tick on Windows: posted through `SDL_RunOnMainThread` with a `PostThreadMessageW` wake; `win-clang-debug` passes all tests, `OxrD3DRealWindowTest.*` and `OxrThreadWindowedTest.*` included
- [ ] `mingw-debug` in Wine (Xvfb, no window manager): all pass but `OxrD3DRealWindowTest.first_frame_has_the_shown_windows_size` and `a_click_on_a_panel_leaves_the_mouse_to_the_panels` (Wine's X11 driver maximizes after the frame the test checks: the window grows to 1920x1054 once the swapchain was made at 1280x720, and the click at (1000, 600) comes before), and `example_e2e`, `install_relocated`, `body_dump` when run as root, where the OpenXR loader ignores `XR_RUNTIME_JSON` (an elevated context). Done when the D3D tests wait for the size Wine settles on, or skip in Wine with the reason
- [ ] the control server reads back every frame and waits for it (`comp_snapshot`), about 10 ms a frame in a debug build: copy into a staging buffer with a fence and read it when a reader asks
- [ ] an MRUK sample test that drags a panel (EnvironmentPanelPlacement: aim with `point right <u> <v> left grip -60`, hold the trigger, move)
- [ ] the first event pumps at startup take 32 and 18 ms on macOS (profile of a Unity player)
- [x] Linux: a window thread of its own like Windows (`HAVE_WINDOW_THREAD`), so frames ended on other threads post the tick there instead of pumping SDL off its main thread; a window thread waiting for a mutex runs the window call of the thread holding it, `xrDestroyInstance` shuts the platform down after the runtime lock, and synthetic events go straight into the event queue (`Platform.a_window_thread_waiting_for_a_mutex_runs_the_calls_of_its_holder`, `OxrWindowedTest.*`, `OxrThreadWindowedTest.*`)
- [ ] a regression test for IMM32 staying loaded after the loader unloads puppetxr on Windows (the windows driver loads and pins it): `puppetxr_tests` imports IMM32 itself, so it needs a program that loads the runtime through the loader, destroys its instance and then creates a window; `puppetxr_example` under `procdump -e` crashed in 16 of 120 runs with OBS's capture hook injected before the fix and in none after
- [x] macOS build (MoltenVK): `mac-*` presets, portability enumeration and subset, the app's Vulkan loader found by install name, exported symbols list, HiDPI windows, no GL (`docs/building.md`, macOS)
- [ ] CI matrix, cross-arch determinism check
- [ ] macOS: `godot_character_centric_movement_vk` and `godot_origin_centric_movement_vk` show one of two `walked` frames from run to run (6595 pixels beyond the tolerance in one), even alone; the other six demos and hello_xr are stable. Godot's call order is the same in both (one thread: wait, sync, begin, end) and `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1` changes nothing; suspects: Godot's physics jitter fix on the wall clock, or Godot drawing on another queue than the one bound. Done when 10 runs give one frame

## 11. Mixed reality: rooms and Meta extensions

Goal: mixed reality apps run unmodified with passthrough, scene, depth, raycast and anchors.
CTS 1.1.63 has no tests for these extensions (only the proc-address checks every advertised
extension gets), so the gtests are their conformance suite. With the scene extensions
advertised: cts_vulkan2 103 cases, 0 failing (2026-10-20).

- [x] world layer: room format v1, entities, rooms, global mesh, BVH raycast, room listing (`docs/world.md`, `docs/room-format.md`)
- [x] room selection: `room` / `rooms_dir` in config, UI combo, control API, recordings carry room name + content hash
- [x] XR_EXT_future, XR_FB_spatial_entity (anchors, component status), XR_FB_spatial_entity_query, XR_FB_spatial_entity_container
- [x] XR_FB_scene, XR_FB_scene_capture (always succeeds, no UI), XR_META_spatial_entity_discovery, XR_META_spatial_entity_mesh
- [x] render: depth attachments, depth-only passes, two-texture pipelines; comp: passthrough layer from the visual mesh
- [x] XR_FB_passthrough with styles, ALPHA_BLEND environment blend mode
- [x] XR_META_environment_depth (visual mesh depth, D16 array swapchain), XR_META_environment_raycast
- [ ] environment depth from cameras apart from the eyes: the depth is rendered from the eye views, so an app that maps a
  pixel into the wrong depth view (the eye's pose or field of view where the depth view's belongs, the other eye's, a
  reprojection that drops the offset) still lines up and no test of it can fail. On a Quest the depth comes from
  cameras offset from the eyes, with fields of view of their own, at a lower rate than frames and some milliseconds
  old. Start: a config block (`docs/config.md`) for the depth views' offset from each eye (position, orientation),
  their field of view, and how many frames old an image is; `comp_render_depth` renders from those views and
  `xrAcquireEnvironmentDepthImageMETA` reports them (`docs/compositor.md`, Environment depth). Done when a gtest
  shows an occluder's edge, mapped with the reported views, where the room has it, and mapped with the eye's views,
  off by the offset; the default stays the eye views so existing goldens hold
- [x] Blender exporter add-on (`tools/blender`), rooms: bedroom, office, living_room, l_shaped
- [x] disabled_extensions: config, `PUPPETXR_DISABLE_EXTENSIONS`, C API, for testing apps without an extension
- [x] tracking origin offset: stage space placed in the room fixed or per run (`origin_offset`, `docs/anchors.md`)
- [x] tracking drift: STAGE, LOCAL, LOCAL_FLOOR and STATIONARY move in the room as a step or a rate while devices, props, the room and anchors stay (`docs/anchors.md`, Tracking drift); C API, `puppetxrctl drift`, `drift_rate`, recorded
- [ ] tracking drift in the UI: the Simulator panel's room section shows the stage origin of the instance only; show the drift and its rate there, with fields to set them (`src/ui.cpp`, `room_from_stage` in `UI_Status`; a `UiTest` that sets the drift by `ui click` and reads it back with `sim_get_tracking_drift`)
- [x] anchor storage: XR_FB_spatial_entity_storage, _storage_batch, XR_META_spatial_entity_persistence; per app and user JSON store in room space, room hash checked
- [x] anchor sharing: XR_FB_spatial_entity_user, _sharing, XR_META_spatial_entity_sharing, _group_sharing through a cloud directory shared by processes or machines
- [x] XR_META_colocation_discovery: advertisements as lock files in the cloud directory
- [x] recordings: store snapshot, origin, anchor timing and faults, external reads; C API and puppetxrctl `anchors`, `origin`; UI wipe buttons
- [x] frame faults: the next `xrWaitFrame`, `xrBeginFrame`, `xrLocateViews` or `xrEndFrame` calls fail with a registry-listed error, without doing their work (`docs/openxr.md`, Frame faults); C API `set_frame_fault`, `puppetxrctl frame fault`, recorded
- [x] Meta's spatial anchor sample as a test app (`meta_xrspatialanchor`)
- [ ] later: EXT_spatial_entity family on the same world (XR_EXT_spatial_persistence on the same store), scene v2 (high fidelity)

## 12. Real apps and samples as tests

Fetched at a pinned commit, patched from `tests/samples/<app>/`, driven by `tools/sample_tests.py`
through puppetxrctl, judged by screenshots against references (`docs/testing.md`, Sample apps).
Mostly Linux; on Windows the Godot demos (Vulkan) and Meta's Unity MRUK samples run.

- [x] `PUPPETXR_BUILD_SAMPLES`, `cmake/samples.cmake`, `tools/sample_tests.py`, ctest `samples`
- [x] sample references committed (`tests/samples/meta_openxr_sdk/references/`, 18 apps, LFS), made on Mesa 26.2.4 llvmpipe and reviewed against docs/samples.md
- [x] XrPassthrough segfaulted in llvmpipe's shader code when run without the test script (`PUPPETXR_EXIT_AT=30`, `living_room`): Mesa's disk shader cache, as below (1 of 3 runs crashed with it, 0 of 11 without); not puppetxr
- [x] Meta OpenXR SDK v85 on Linux (GLES 3 over EGL, `linux.patch`): 12 samples under test
- [x] sample crashes in llvmpipe JIT code (XrPassthrough, XrSceneModel, XrSpatialAnchor, XrSceneSharing): shaders loaded from Mesa's disk shader cache (26.2.4, LLVM 23.1.1), not puppetxr; the tests run with `MESA_SHADER_CACHE_DISABLE=true`
- [ ] llvmpipe with rasterizer threads leaves stale 64-pixel tiles in XrSceneModel, differently each run, and single-threaded it shows lines on every 64th row. Not puppetxr's fencing: with `glFinish` on every context switch it stays. It goes away with `LP_PERF=no_depth`, so llvmpipe's threaded depth test inside the app's own frame; standalone repros (multiview layered depth, the sample's state sequence, invalidating UBO maps) do not trigger it. Next: an apitrace of XrSceneModel replayed with and without threads, then a Mesa report; until then `LP_NUM_THREADS=0` and tests run in parallel instead
- [x] sample references remade with Mesa 26.2.4 llvmpipe, the Mesa of the composition references
- [x] the sample test image `tools/samples/Dockerfile` (Arch Linux Archive snapshot 2026-10-15: Mesa 26.2.4, LLVM 23.1.1, Xvfb) and `tools/samples/run.sh`, which builds into `build/docker-samples` and runs the tests in it; the Godot references come from it
- [ ] references identical on every x86 machine, then `--tolerance 0` again in `tools/samples/run.sh` and `cmake/samples.cmake` (1 until then, as CI runs on whatever CPU the runner has): all references are now made in the image; Meta references made on another machine with the same Mesa and LLVM differed by 1 on about 10000 pixels (same with puppetxr and the samples built outside the image, so not the build). llvmpipe JITs for the host CPU; `GALLIUM_OVERRIDE_CPU_CAPS=avx` or `avx2` do not change it, `sse4.1` changes much more. Find the CPU feature (AVX-512 here), pin it in `run.sh`, remake all references in the image; done when two machines with different CPUs pass the same references
- [x] CI runs `tools/samples/run.sh` in `ci-full.yml`: image and `build/docker-samples` from the actions cache, `samples_out` an artifact when a test fails
- [x] XrPassthrough: XR_FB_triangle_mesh, projected passthrough as a depth mask of the geometry instances
- [x] XrSceneModel: XR_META_spatial_entity_room_mesh; room format with `room_mesh`, generated by the Blender exporter (`--room-mesh-only` for existing rooms), starter rooms updated
- [x] XrControllers: XR_FB_haptic_pcm, XR_EXT_haptic_parametric and the EXTX1 preview, the OpenXR 1.1 Meta profile names
- [x] XR_EXT_stationary_reference_space and the EXTX2 preview: STATIONARY is the LOCAL origin at sim creation, one constant generation id (docs/openxr.md)
- [x] XR_META_performance_metrics: frame times, dropped frames and space warp mode of the simulated frame loop; CTS `XR_META_performance_metrics` passes
- [x] XR_FB_space_warp: space warp infos validated and dropped, no frame extrapolation; CTS `XR_FB_space_warp` passes
- [x] XR_EXTX1_event_channel (preview, spec version 3): channels tag events at queue time, session targets, a default, select with a timeout (docs/openxr.md)
- [x] XR_FB_composition_layer_alpha_blend: per-layer blend factors on every backend (docs/compositor.md)
- [x] XR_META_passthrough_color_lut: 3D LUTs on passthrough layers, single and interpolated (docs/compositor.md)
- [x] XR_META_hand_tracking_frequency_hint, XR_META_hand_tracking_unextrapolated_poses (docs/openxr.md, hand tracking)
- [x] depth composition: XR_FB_composition_layer_depth_test on every backend with projection depth from XR_KHR_composition_layer_depth, quads, cylinders, equirects and cubes (docs/compositor.md); render compare ops complete; D3D11 and D3D12 depth clears take their value
- [x] XR_METAX1_passthrough_camera_data (unpublished, layouts from MRUK's shared library): two cameras, images of the room at 30 Hz with capture timestamps, traced (docs/openxr.md, Passthrough cameras); `unreal_mruk_passthroughcameraaccess` compares a screenshot on Windows
- [ ] XR_METAX1_passthrough_camera_data on Linux: MRUK's Unity `PassthroughCameraAccess` should get frames there too; add a Unity sample test for it (`tools/sample_tests.py`, `unity_mruk_*`). Done when that test passes on Linux
- [ ] the other unpublished Meta extensions (docs/extensions.md, "Unpublished"): `XR_METAX1_room_entity_info` next; each needs its layouts from Meta's binaries
- [ ] Meta's XR Operator layer drives the Meta XR Simulator through `XR_METAX1_simulator_compositor_output_capture` and `XR_METAX2_simulator_head_pose`; offering both would let it drive puppetxr too (same header problem)
- [x] XR_FB_foveation, XR_FB_foveation_configuration, XR_META_foveation_eye_tracked: profiles validated and kept per swapchain, no visual effect (docs/openxr.md)
- [ ] XR_FB_foveation_vulkan: fragment density map images per swapchain image; puppetxr would add VK_EXT_fragment_density_map to the device extensions it asks for when the GPU has it. Done when a test on a GPU with the extension renders with the map (lavapipe lacks it)
- [x] meta_xrscenemodel now finds STATIONARY and draws its axes at the start pose; its references remade in the image with the subdivided global mesh
- [ ] the other 1.1 Touch names, `meta/touch_controller_rift_cv1` and `meta/touch_controller_quest_1_rift_s`, are accepted but not emulated
- [x] XrHandsFB, XrHandDataSource, XrHandsAndControllers: XR_EXT_hand_tracking, the FB aim, capsules and mesh, data source, motion range, hand interaction
- [x] XrHandTrackingWideMotionMode, XrMicrogestures
- [x] XrVirtualKeyboard: XR_META_virtual_keyboard
- [x] XrDynamicObjects: XR_META_dynamic_object_tracker and _keyboard on keyboard props
- [ ] XrBodyFaceEyeSocial: XR_FB_body_tracking, XR_FB_face_tracking2, XR_FB_eye_tracking_social, or a fixed exit path
- [x] core apps: hello_xr (Vulkan2, Vulkan), Monado's openxr-simple-example (GLX through SDL2) and xrgears (Vulkan, equirect2 sky, quads) through the same driver
- [ ] more core apps: KhronosGroup/OpenXR-Tutorials, janhsimon/openxr-vulkan-example, maluoi/openxr-explorer
- [x] rejected calls log their failed check once per call site (`OXR_CHECK`, `OXR_FAIL`); `xrEndFrame` logs its first rejected layer once
- [x] curved layer angles outside their spec ranges are clamped and logged once (xrgears' sky)
- [x] Godot 4.7.2's official XR demos (godot-demo-projects `xr/openxr_*`, tag 4.7-6ad6167), each on the compatibility renderer (GLX) and the mobile renderer (Vulkan): 16 tests, about 4 minutes on 4 threads; puppetxr accepts Godot's Xlib binding without a config
- [x] Godot demo `openxr_render_models`: `XR_EXT_render_model` and `XR_EXT_interaction_render_model` on the procedural controllers
- [x] Godot demo `openxr_binding_modifier_demo`: `XR_KHR_binding_modification`, `XR_EXT_dpad_binding` (thumbsticks) and `XR_VALVE_analog_threshold`; the test ramps both triggers and pushes both sticks
- [ ] trackpad dpads (`dpad_center`, click and force activation) once a simulated controller has a trackpad; binding modifier haptics other than `XrHapticVibration`
- [ ] Godot's mobile renderer on lavapipe started without its directional light (ground and hands darker, sky unchanged) for a whole run in 3 of about 70 Vulkan runs, all in full-suite runs with Mesa's shader disk cache on; with `MESA_SHADER_CACHE_DISABLE=true` 0 of 32 runs. If it comes back, find out whether Godot's background pipeline compilation or puppetxr's queue use is at fault (`docs/compositor.md`, Vulkan ordering)
- [x] Godot OpenXR vendors 5.1.0 samples on its released Linux addon: demo, Meta scene, passthrough, hand tracking, composition layers, space warp, performance metrics, body tracking (8 tests); puppetxr offers controller render models at glTF subset 2 as well
- [x] body tracking phases 1 and 2 (`docs/body-tracking.md`): `XR_FB_body_tracking`, `XR_META_body_tracking_full_body`, `_fidelity`, `_calibration`; posed skeleton in Meta's joint axes following the head, arms and legs by two-bone IK, shape, pose and tracking from the Body tab, `puppetxrctl body`, the config and the C API, recorded, `apps/body_dump` with a reference in `tests/`
- [ ] body tracking checked against a device that tracks the body: run `apps/body_dump` there and compare with `tests/body_dump_puppetxr.json`: joint axes, parents (is the root's parent -1 or `XR_FULL_BODY_JOINT_NONE_META` in the full body set, the wrist's the wrist twist), proportions, where the root is while tracking. Fix the differences or list them in `docs/body-tracking.md`
- [x] body tracking with Meta's Unity-Movement: `unity_movement_body` (macOS reference; `docs/samples.md`)
- [x] body and face tracking with Meta's Unreal-Movement sample: `unreal_movement_body`, `unreal_movement_face` (Windows, D3D12; `docs/samples.md`); `unity_movement_body`, `_face` and `_gaze` pass on Windows against references of their own
- [ ] Unreal-Movement's animation blueprint maps (`MAP_HighFidelity_AnimBlueprint`, `MAP_RetargetMannequinAnimBlueprint`) as sample tests, and dropped joints: retargeted avatars stay sound; a difference counts against puppetxr only when `body_dump` or `XrBodyFaceEyeSocial` show it too
- [x] face and eye tracking: `XR_FB_face_tracking2`, `XR_FB_face_tracking`, `XR_META_face_tracking_visemes`, `XR_FB_eye_tracking_social`; Face tab, `puppetxrctl face`, config, C API, recorded, `eyes` profile; `unity_movement_face` (`docs/face-tracking.md`)
- [x] the gaze checked against a device that tracks eyes (`docs/face-tracking.md`, "Measured on a device that tracks eyes"): the origin sits at the view's origin within 1.5 mm, the roll keeps the head's up, yaw and pitch turn the way the simulator turns them (against targets of known position, the yaw error averaging -0.04 degrees), and the pose is never unusable nor frozen, all as simulated; the sample time differs, the hardware sampling 28 to 51 ms before the frame's display time on a clock drifting -88 ppm against it
- [ ] simulate the gaze's sample lag: puppetxr reports the located time, hardware a sample 31 to 36 ms older (`docs/face-tracking.md`). Either report a configurable lag in `XrEyeGazeSampleTimeEXT` and locate the gaze at that older time, or write in the docs that the simulator's gaze is always current. Done when an app that corrects for the lag behaves the same on both
- [ ] the face half, which needs a device that tracks the face: where the per-eye poses of `XR_FB_eye_tracking_social` sit, their confidence, and the weights' ranges in the vendor presets, `SIM_FACE_MAX_GAZE_YAW` 35 and `SIM_FACE_MAX_GAZE_PITCH` 30 above all (`src/sim.h`, numbers nothing has checked). Compare with `apps/eye_dump` and Meta's `XrBodyFaceEyeSocial`
- [x] `XR_EXT_eye_gaze_interaction` from the simulated gaze (`OxrEyeGazeTest.*`)
- [x] gaze-and-pinch in Movement's scene menu: `unity_movement_gaze` (Eye Gaze Interaction feature and Interaction SDK in the patch)
- [ ] the other face extensions on the same weights: `XR_ANDROID_face_tracking`, `XR_HTC_facial_tracking`, `XR_BD_facial_simulation`; `XR_ANDROID_eye_tracking`
- [ ] body tracking: an option that keeps the controllers within the arms' reach, off by default (tests place controllers exactly)
- [ ] body tracking: steps when the body walks, instead of feet sliding with the root
- [x] body tracking: a skeleton change period, the count rising with the shape the same (`docs/body-tracking.md`, Model)
- [x] vendors body-tracking-sample gets body tracking (`docs/body-tracking.md`): the reference of `vendors_body_tracking_gl` (Linux, GL) shows the avatar following head and controllers
- [ ] `vendors_body_tracking_gl` with a longer left arm (`puppetxrctl body bone left_arm_upper 1.3`): one more screenshot in its script; done when the sample test compares it
- [x] Godot demos on Windows: the Windows build of Godot 4.7.2 on lavapipe of mesa-dist-win 26.2.4, the 8 `_vk` tests against the Linux references within 3 (docs/testing.md)
- [ ] Godot demos on Windows with the compatibility renderer: Godot sets its window's pixel format twice and Mesa's WGL refuses the second call; needs a Mesa that accepts the same format again, or a Godot fix upstream, then drop the `_gl` exclusion in `tools/sample_tests.py`
- [ ] Meta OpenXR SDK samples on Windows: the SDK's own Windows path draws desktop GL over WGL, the references are GLES on EGL; either port `linux.patch` to Mesa's Windows EGL and GLES of mesa-dist-win (`libEGL.dll`, `libGLESv2.dll`), or a Windows patch with references of its own
- [ ] Godot vendors plugin samples, hello_xr and Monado's demos on Windows (`cmake/samples.cmake` builds them on Linux only)
- [x] Unity MRUK samples on Windows (`oculus-samples/Unity-MRUtilityKitSample` at the 207.0.0 release, Unity 6000.0.66f2, D3D11 on the GPU): `-DPUPPETXR_UNITY=<Unity.exe>` builds the player (`tests/samples/unity/build.cmake`, which imports the TMP Essential Resources the repository ignores), ctest `samples_unity` opens each of the 12 scenes from the start menu and compares a screenshot (docs/testing.md, Unity samples). Needed in puppetxr: `runtime_name` (OVRPlugin refuses other runtimes), `puppetxrctl point` with `grip -60` (OVRPlugin's UI ray), layers naming a space destroyed during the frame (Unity switches its app space within a frame)
- [ ] Unity MRUK references are of one machine (D3D11 on an AMD Radeon RX 6700 XT); a second GPU needs references of its own or a tolerance. A CI without a GPU runs them on WARP: the players take `-force-driver-type-warp` (Microsoft Basic Render Driver), all 17 tests take their shots in 7.8 minutes (4.3 on the GPU) and two runs gave bit-identical images, but none matches the GPU references, so WARP needs references of its own (a `sample_tests.py` switch passing the argument, references beside the GPU ones). A Unity license for CI
- [x] Unity MRUK: `OVRManager` set the Stage origin again every frame and Unity recreated its app space each time: OVRPlugin locates views and spaces at time 0, which the spec rejects; `accept_time_zero` maps it to the latest time, MRUKBase now logs three app space changes
- [x] Unity Depth API sample (`oculus-samples/Unity-DepthAPI`, project `DepthAPI-URP`, LOADER oculus) on Unity 6000.3 (`PUPPETXR_UNITY_6000_3`; 6000.0 reports invalid module dependencies); OcclusionToggler, SceneAPIPlacement, HandsRemoval and DepthMask run as tests
- [x] Unity Shared Spatial Anchors sample (`unity_ssa_colocation`): two players share an anchor through a colocation group (docs/testing.md). Needed in puppetxr: time 0 for `xrCreateSpatialAnchorFB` (`accept_time_zero`), the group filter as the query's filter; in the sample a patch enabling its buttons without Meta's platform service
- [ ] `unity_ssa_colocation` lands in one of two stable pictures: the peer's anchor axes a few pixels apart and another menu entry highlighted (3312 pixels in `peer_anchor`), two runs in a row each way. The host and the peer start one after the other in wall-clock time; find which of their exchanges is not stepped (the advertisement poll, `discovery_next_poll`, or the peer's join), then make it wait on a condition. Done when ten runs pass at 0 pixels
- [ ] Unity Shared Spatial Anchors: the Basic Sharing scene (SharedSpatialAnchors) matches players and exchanges their user ids and anchor uuids through Photon, which needs an app id and the network. A patch replacing that exchange with a local one (a file in the shared `PUPPETXR_CLOUD_DIR`), and the user id from the command line as for the Unreal sample, would exercise sharing with users in Unity too; `unity_ssa_colocation` covers sharing with groups
- [x] Unity MRUK drew its global mesh as opaque cyan and built no spawn, destruction or virtual room content: MRUK builds label strings from `bufferCountOutput`, which counted the terminator; it no longer does (docs/openxr.md, Spatial entities)
- [x] Apps on OVRPlugin run without settings: `sim_known_modules` (`src/sim.cpp`) sets `runtime_name` `Oculus` and `accept_time_zero` when `OVRPlugin.dll` is in the process; the Unity sample tests no longer set them (docs/config.md, Known modules)
- [x] MRUK scenes played directly in the Unity editor found no room: MRUK discovers in `Awake`, before the session runs, and never retries; with OVRPlugin `discover_before_running` answers it and logs an error (`OxrSceneTest.discovery_before_running_when_allowed`)
- [ ] Regenerate `cts_composition` and `cts_scenarios` (`tests/cts/`, lavapipe of the pinned Mesa) after the 60 degree aim (hands held 60 degrees up; `cts_scenarios.THUMBS_UP` holds accordingly); run each with `--update` in the image and review. The sample references made in the Linux container (`meta_openxr_sdk`, `openxr_sdk`, `openxr_simple_example`, `xrgears`, `godot_vendors`) and the Windows-made ones are updated
- [x] Device presets `quest3` (default), `quest3s`, `quest2`, `questpro` (`sim_device_presets`, `Sim.device_presets_set_the_headset_and_explicit_keys_win`)
- [ ] Device presets: each model's asymmetric eye angles (`xrLocateViews` fov) instead of the shared `views.fov_degrees`; the recording header stores one angle, so this is a `REC_FORMAT_VERSION` bump with a reader for the old header. Measure the angles on the devices (`apps/eye_dump` prints the fov per view) rather than taking spec sheet numbers
- [ ] Device presets: Meta's VR glasses. OVRPlugin 207 knows them as `Meta_Link_VR_Glasses` by the headset id `df221d8c-91c6-4b49-8a1d-16f588875959`; their system name, eye buffer, rates and controllers are not known yet
- [x] D3D11 mirror window resize: the UI's framebuffers are released before the window swapchain is made again (OxrD3DRealWindowTest.mirror_window_swapchain_is_recreated_on_resize, a real window on Windows)
- [x] Startup timing in the log (`startup_log`, `OxrVulkanTest.startup_is_logged_at_the_first_frame_with_layers`); the mirror window hidden until its first frame (`Platform.windows_start_hidden_until_shown`)
- [ ] Startup: puppetxr's share is about 200 to 250 ms of 1.4 s (MRUK player) to 2.1 s (editor). Each of Unity's two or three instances loads the room again (about 23 ms each, the albedo PNG decode); keep the decoded room across instances of a process. The compositor (21 ms) and the window (16 to 23 ms) are next
- [x] Time 0 is the predicted display time of the frame being drawn (`OxrTest.time_zero_is_the_frame_being_drawn_when_accepted`); before, OVRPlugin's moving controllers were located a frame behind Unity's
- [x] UI click tests (`UiClicker`, ImGui test engine hooks) and `puppetxrctl ui click`; the props motion drop-down picks a motion (`UiTest.prop_motion_is_picked_from_its_drop_down`, and in the MRUK player)
- [ ] The props motion drop-down was reported to close the QR code entry when clicked with a real mouse in the Unity editor; synthetic clicks (tests and `puppetxrctl ui click` in the MRUK player) work. Reproduce with a real mouse, find what differs (SDL's global mouse state on the window thread, the editor's input), add a test
- [x] The UI hovered and clicked where the mouse had been, in Unity's editor: the window got no mouse moves (ImGui had no mouse, `puppetxrctl ui find` said so) and clicks went to the last move. Clicks now carry their own position to ImGui (`UiTest.clicks_act_where_they_happen_without_moves`) and the UI takes the system cursor each frame while it is over the window (`os_window_cursor`). Why the editor's process keeps the moves from the window is not known; synthetic cursor moves reach it
- [ ] UI click tests for every panel: each button, checkbox, combo and drag of the Simulator, Devices, Virtual keyboard, Props and Log panels and the menus, through `UiClicker`
- [x] Black lens mask in the composite (`COMP_FLAG_LENS_MASK`, `OxrVulkanTest.lens_mask_is_black`)
- [x] Rooms compiled into the library (`cmake/embed_rooms.cmake`, `puppetxr_rooms`); a rooms dir adds to them (docs/world.md)
- [x] The default room is `living_room`, so MRUK scenes find a room without settings; the CTS runs set `PUPPETXR_ROOM=none`
- [ ] The Unity MRUK sample tests enter every scene from the start menu, with the session running; one scene started directly (as the editor does) would cover MRUK's discovery in `Awake`
- [x] Unity MRUK on the Oculus XR plugin (`unity_mruk_oculus`): Depth API occlusion of Oppy behind the coffee table, after puppetxr accepted OVRPlugin's non-registry binding paths and offered D32S8 swapchains
- [ ] `unity_mruk_oculus` allows 6000 differing pixels: OVRPlugin paces Unity's frames on the Oculus loader, not locked to puppetxr's steps, so Oppy's idle animation lands a frame apart now and then, and its walk is skipped. Lock the frames (find what OVRPlugin's frame thread waits on), or check occlusion directly (the table's pixels where Oppy stands in the OpenXR view). The per-frame read-back in `xrEndFrame` with control on moved PassthroughRelighting's Oppy to another, again stable, frame of his animation (reference updated)
- [x] Global mesh with scan-sized triangles: the exporter cuts it until no edge is longer than 0.2 m and turns its entity like the floor, local +Z up, as Meta's scans have it (`--global-mesh-only` replaced it in the four rooms); MRUK's DestructibleMesh then breaks the walls into segments and keeps the floor as its reserved one. Rays along an axis through a box face of the BVH were lost to 0 * inf (`ray_slab`)
- [x] Unreal DestructibleMesh destroyed no segment: Meta's `AVRPawn` sets `InputComponent` in its constructor, and `APawn::PawnClientRestart` sets up a pawn's input (`SetupPlayerInputComponent`, the Blueprint's input events) only when it has none, so `IA_Select` triggered with nothing bound to it, on any runtime; the menu button's C++ binding is as dead. `tests/samples/unreal_mruk/sample_tests.patch` leaves `InputComponent` unset, and `unreal_mruk_destructiblemesh` checks that the trigger changes the image more than the scene does by itself. Report to Meta (with the other Unreal items)
- [ ] Unity MRUK scenes beyond loading: throw in FindMultiSpawn, place panels in EnvironmentPanelPlacement (DestructibleMesh destroys a segment); KeyboardTracker and QRCodeDetection track their props (a keyboard, a QR code), moving them is next. PassthroughRelighting walks Oppy; occlusion of him needs Depth API in the Windows player (below). Animations and spawns are bit-identical with `-puppetxr-fixed-step`. KeyboardTracker came out in one of two images because each query gave the keyboard a new space, which MRUK took for a new keyboard; queries return the live space now, EnvironmentPanelPlacement 643 and 661 pixels apart (a controller ray left on its panel, fixed by turning the rays to the floor first). Players that failed after 4 s without a screenshot crashed in OVRPlugin's JSON reader (0xC0000409), reading another player's log as it is written; they start one at a time and such a test runs up to twice more (reported to Meta). A player whose simulator had no discovery file was puppetxr's: the rename onto a path a reader held failed, now retried. Once a player hung after its shutdown log; `hang_stacks.txt` will show where if it happens again. In a full Windows run on 3 jobs KeyboardTracker (both loaders) showed other lines in its on-screen log and EnvironmentPanelPlacement a hover outline and ray dot on its panel; alone they pass. Both follow the wall clock (the log's lines, the ray's hover); find what in them is not stepped
- [x] Unreal MRUK sample on Meta's Unreal fork (`-DPUPPETXR_UNREAL`, `samples_unreal`): its menu in the discovered, dressed living room (`unreal_mruk_menu`)
- [x] Unreal MRUK demo maps: each opened from the start level (`unreal_mruk_<map>`, docs/samples.md)
- [x] Unreal MRUK menu clicked with the Interaction SDK's ray: the core demos (`unreal_mruk_core_demos`), the Maps tab loading Guardian (`unreal_mruk_maps_tab`); KeyboardTracker and QRCodeTracker with their props
- [x] Unreal's VR Preview (`unreal_mruk_vr_preview`): puppetxr's window in front with each preview, gone after it, two previews in a row, the editor quits cleanly
- [x] Meta's Unreal Passthrough, Occlusion, Shared Anchors and Interaction SDK samples and the Unity Starter Samples and Depth API sample as sample tests (docs/samples.md); Unity 6000.3 for the Depth API (`PUPPETXR_UNITY_6000_3`), `-puppetxr-scene` opens a Unity scene directly
- [ ] Send Meta the reports in `docs/meta-reports/` (the "Report to Meta" items below are written up there); keep them current as items are fixed or found
- [ ] Report to Meta: in Unreal's Occlusion sample every virtual object is near black from Meta's plugin (docs/samples.md, Unreal Occlusion sample: `InvAlphaTexture` lacks the sRGB flag on D3D); puppetxr's depth, the matrices and the occlusion result are right (RenderDoc). The tests run unchanged and are not compared; once the plugin is fixed, record their references and compare them
- [ ] Unreal Occlusion sample: the modes are a combo box on a poke panel (`IsdkInteractableWidget_0`, `IsdkPokeInteractablePanelConfig`) that stands low and tilted in front of the player: world (0, 0, 80) cm, stage about (0, 0.8, -0.4) m, below the view when looking ahead; the test still presses A. Earlier pushes went through empty space at eye height. Pushed there (head pitched 40 degrees down, the right grip at stage (0, 0.8, -0.25) to (0, 0.8, -0.45)), the panel's `IsdkPokeInteractable` goes HOVER, then SELECT (editor probe, 2026-10-26). The panel is `WBP_UI` on a 1024x500 widget component at scale 0.1 (102.4 x 50 cm), yaw 180, pitch 10; its `OcclusionsComboBox` (No, Hard, Soft Occlusions; Soft at start) and `HandsRemovalCheckBox` (checked) are widget properties of that name, and the game mode's `CurrentOcclusionMode` and `HandsRemovalEnabled` show the effect. `unreal.SlateLibrary` (`local_to_absolute`, `get_local_size`) gives the items' places on the panel. Next: poke each item at its place, check the state changes, then compare each mode in `unreal_occlusion_modes`. Editor probes of this project stalled at start in four of nine runs (the editor's command script took 15 minutes to report ready, no shader compile in the log)
- [ ] Unreal Passthrough sample: PTMasked's circle on the left controller after A shows no passthrough; compare with the brush, which paints passthrough (`unreal_passthrough_maskedbrush`). An editor probe of a Blueprint-only project needs `puppetxr.VRPreview`: copy the built `Plugins/SampleEditor` of the MRUK sample into its build copy for the probe (not into the patch)
- [ ] Unreal Passthrough sample: click through the remaining widgets (PTStyles' contrast, brightness and posterize sliders, PTColorMap's entries, PTColorLut's weight and type, PTColorScaleAndOffset's stop) and compare each style; PTStyles' curves and edge alpha are tested. A slider dragged with the ray follows only the first steps: the edge alpha stops at 0.42, the LUT weight barely moves; find what the Interaction SDK's ray needs to keep the drag (hand speed, hover, a pinch strength)
- [ ] Unreal Interaction SDK sample: the levels are compared after 600 frames, with tolerance 3 on up to 2000 pixels: their fog settles over the first seconds as far as the load took in wall-clock time, and a few widgets (a torch, a card's icon) animate in wall-clock time. Find what drives them (Unreal's real time in a material or widget animation) to compare at 0. Interact in each level: the controller pokes PokeExamples' red button (`unreal_isdk_poke`); poke the other buttons, with a finger too, select with the ray, grab and throw, transform, gaze, hand poses. TransformerExamples crashes in `UIsdkHandGrabPose::InitializeDebugVisual` (it asks for the editor's world under `-game`); run it in the editor's VR Preview instead
- [ ] Report to Meta: its Unreal plugin (`OculusXRLayerExtensionPlugin.cpp`, `UpdateCompositionLayers_RHIThread`) passes `xrGetRecommendedLayerResolutionMETA` the display time of the frame its RHI thread submits, older than the last `xrWaitFrame`'s, which the extension makes `XR_ERROR_TIME_INVALID`, and logs an ensure for each refusal. puppetxr answers such a time for the plugin (`accept_older_frame_time`, on with `UnrealEditor-OculusXRHMD.dll` loaded, docs/config.md Known modules); once the plugin passes the waited frame's time, drop that row
- [x] Unreal sample tests on D3D11, D3D12 and Vulkan with the mobile and desktop renderers: one test per project as `<test>_<rhi>_<renderer>` (docs/testing.md, Unreal samples). They found the next two
- [x] Vulkan device lost in Unreal's Occlusion sample (two of three runs; `VK_ERROR_DEVICE_LOST`, a GPU read at address 0; validation: two threads in `vkQueueSubmit` on the app's queue): puppetxr rendered environment depth in `xrAcquireEnvironmentDepthImageMETA` and read frames for screenshots in `xrWaitFrame` and `xrPollEvent`, all outside the calls the app synchronizes its queue for. Depth renders in `xrEndFrame` now, frames are read back only there with control on (`OxrDepthTest.acquire_leaves_the_apps_queue_alone`, `OxrControlVulkanTest.frames_are_read_off_the_gpu_only_in_xrEndFrame`)
- [x] OVRPlugin on Vulkan asks for fragment density map foveation without `XR_FB_foveation_vulkan`; refused, Unreal recreated its instance forever. Accepted without effect
- [ ] Unreal desktop-renderer variants differ from their references and from run to run: `unreal_passthrough_startup_{d3d11,d3d12,vulkan}_desktop`, `unreal_passthrough_styles_{d3d11,d3d12,vulkan}_desktop` (thousands of pixels, by 1 on D3D11, up to 20 on Vulkan) and `unreal_mruk_menu_vulkan_desktop` (about 600 pixels, up to 118), one at a time, on the runtime before and after the window thread fix alike (2026-10-25). The mobile renderer's variants match. Find what in the desktop renderer depends on more than the stepped frames (temporal AA history, auto exposure, a wall clock), then pin it in the test's `-dpcvars` or compare these at a tolerance
- [ ] Unreal shaders compiled cold: `unreal_shared_anchors_session_{d3d11,vulkan}_desktop` showed grey-grid menus on their first run, which compiled the shaders cold: Unreal draws a default material while it compiles asynchronously. The Unreal tests compile synchronously now (`bAllowAsynchronousShaderCompiling=False`); done when a run with an emptied `DerivedDataCache` of a sample project matches the references
- [ ] Report to Meta: Unreal's Vulkan renderer draws parts of two scenes wrong, with a warm shader cache, while D3D11 and D3D12 are right and puppetxr composites whole images the same way on every API: `unreal_passthrough_startup_vulkan_desktop` (grey noise on Oppy's head, no reference) and `unreal_isdk_poke_vulkan_mobile` (table and furniture black, not compared). The validation layer reports errors in Unreal's own commands on the mobile renderer (`viewMask-06178`, `viewType-07752`, `shaderTileImageColorReadAccess-08718`). Once fixed, record and compare both
- [x] Vulkan validation on Unreal's Occlusion sample (`-vulkandebug`), after the queue fix: no threading errors in six runs. What remains is the app's: `VUID-VkImageViewCreateInfo-pNext-02662` (a view of the depth swapchain with usage beyond the `usageFlags` 0x3a the app asked for, all of which the image has), one `InvalidImageLayout` at the first submit (Unreal expects `DEPTH_READ_ONLY` after its own transition to `TRANSFER_DST`), `viewType-07752`, `viewMask-06178` and a tile image barrier in Unreal's own draws. RenderDoc 1.x crashes in Unreal's `vkCmdBeginRendering`, so no captures on Vulkan
- [ ] Unreal Shared Anchors sample: Create Session, Create Anchor (`unreal_shared_anchors_session`) and sharing with users between two players (`unreal_shared_anchors_sharing`) are tested. Next: Load Anchors (from the store after a restart), Unsave and Erase, Orient to Anchor, the cubes (Spawn Cube, Explode Cube) replicated to the peer
- [ ] Unity Starter Samples: Stereo180Video draws black in the app's own projection layer (its Media Foundation video); OVROverlay's modes, EnableDisablePassthrough's toggle, SpatialAnchor's placement, its save, hide, load and erase, and Locomotion's teleport are tested; OVROverlayCanvas shows some labels mirrored, which needs a device's picture to judge
- [ ] Unity Depth API sample: grab objects with the controllers; OcclusionToggler's modes, HandsRemoval's thumb taps, SceneAPIPlacement's posters and DepthMask's mask are tested (`unity_depthapi_occlusionmodes`, `unity_depthapi_hands`, `unity_depthapi_posters`, `unity_depthapi_mask`)
- [ ] Unity Passthrough Camera API samples (`Unity-PassthroughCameraApiSamples`, MRUK 85): the player starts no OpenXR session (the project's Windows XR loader setting), and MRUK 85 reads the camera on Android only; a newer MRUK may take images on Windows through `XR_METAX1_passthrough_camera_data`
- [ ] later: Proton's wineopenxr maps D3D11/12 to XR_KHR_vulkan_enable as an alternative
- [ ] later vendors: Android XR (godot_openxr_vendors samples), PICO native samples
