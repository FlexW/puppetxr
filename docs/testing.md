# Testing

```
ctest --test-dir build --output-on-failure
build/bin/puppetxr_tests --gtest_filter='Oxr.*'
```

What each suite costs (AGENTS.md rule 12), wall clock on 4 threads unless noted; update it
when a suite gets slower:

| suite | how | time |
|---|---|---|
| unit tests | `ctest --preset gcc-debug -E '^samples$'` (about 660 tests, parallel) | about 2 min |
| Meta samples | `tools/sample_tests.py --apps meta_openxr_sdk` | about 2 min on 16 threads |
| Godot demos | `tools/sample_tests.py --apps godot_demos` | about 4 min, longest test about 2 min; on Windows (8 `_vk` tests) about 1 min |
| Godot vendors samples | `tools/sample_tests.py --apps godot_vendors` | about 1 min on 4 threads, longest test about 30 s |
| core apps | `tools/sample_tests.py --apps openxr_sdk,openxr_simple_example,xrgears` | about 20 s |
| macOS `samples` | `ctest --preset mac-debug -R '^samples$'` (8 Godot `_vk` tests, hello_xr Vulkan and Vulkan2, one at a time) | about 1 min |

## CI

Two tiers on hosted runners (4 CPUs), everything in the pinned image of `tools/samples/Dockerfile`
through `tools/samples/run.sh`. Linux for now; Windows and macOS join the same jobs.

| workflow | when | what | time |
|---|---|---|---|
| `.github/workflows/ci-fast.yml` | every push to a pull request and to `main` | unit tests of `gcc-debug` and `clang-debug` | about 4 min with warm caches, 7 min cold |
| `.github/workflows/ci-full.yml` | a pull request with the label `full-ci`, each push while it has it, every push to `main`, by hand | unit tests of the other presets, all samples, `tools/dist/dist.sh` with its tests | about 16 min with warm caches (the release build is not cached), 22 min cold |

- The job `full-ci` of `ci-full.yml` is the check to require on `main`. It fails until the label is
  on and the other jobs passed, as a required check counts a skipped job as passed.
- `tools/dist/dist.sh` runs but nothing is uploaded: CI publishes no release binaries. Only logs
  and sample screenshots of failed runs become artifacts (7 days).
- Caches: LFS files and the image (`.github/actions/setup`), `ccache` per preset
  (`.github/actions/unit`), `build/docker-samples` keyed on what the samples are built from. Each
  is saved even when a test fails.
- Run a tier's suites locally: `tools/samples/run.sh --unit <preset>`, `tools/samples/run.sh`.
  `PUPPETXR_DOCKER_ARGS=--cpuset-cpus=0-3` and `--jobs 4` come close to a runner; `--jobs` is
  needed as the sample driver counts the host's CPUs.

## Wall-clock bounds

A test with an upper bound on wall-clock time goes into `puppetxr_timed_tests` in
`tests/CMakeLists.txt`, which runs them alone (`RUN_SERIAL`). Its bound is what the bug it catches
would take, not how fast this machine draws: `FRAME_BOUND_NS` is half of the main thread timeout a
blocked frame waits for, and `frames_never_wait_for_the_display` allows 45 of its 60 display
periods. `TEST_TIME_SCALE` scales such bounds under the sanitizers.

## Vulkan device

Tests needing a Vulkan device skip themselves when none is found. CI uses lavapipe:
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` (Arch, as in `tools/samples/Dockerfile`: `lvp_icd.json`).

Render and UI tests run once per backend (`Backends/RenderTest.*/Vulkan`, `/OpenGL`,
`/OpenGLES`, `/D3D11`, `/D3D12`). The GL ones play the app with a surfaceless EGL context
(`tests/test_gl.h`, EGL loaded at runtime), or where there is no EGL (Windows) a WGL core
context on a hidden window, and skip without one; Mesa's llvmpipe suffices
(`LIBGL_ALWAYS_SOFTWARE=1`). A `GL error` the backend logs fails the test, as a Vulkan
validation error does. The D3D ones (`tests/test_d3d.h`) create the app's device on WARP,
Windows' software rasterizer, else the default adapter, and skip in builds without D3D.
`OxrD3DTest` and `OxrD3DWindowedTest` drive the D3D bindings end to end.

Platform-specific tests are few and in `tests_base.cpp` (state directory). Everything else is
portable: temporary files go through `os_temp_dir` and `os_create_temp_directory`, child
processes through `os_process_spawn`. The test environment sets `PUPPETXR_STATE_DIR` so windowed
tests never see the UI choices of whoever runs them.

## Windows builds under Wine

The `mingw-debug` preset cross-compiles with mingw-w64 GCC (`cmake/mingw-w64.cmake`) and runs
the tests through Wine (`CMAKE_CROSSCOMPILING_EMULATOR`). It needs `mingw-w64`, `wine64` and an
X server for Wine's window and GL drivers (`Xvfb :99 &` and `DISPLAY=:99`). Wine's D3D11
(wined3d over GL) and D3D12 (vkd3d over Vulkan, lavapipe suffices) run the D3D tests:

```
cmake --preset mingw-debug && cmake --build --preset mingw-debug
DISPLAY=:99 WINEDEBUG=-all ctest --preset mingw-debug -j3
```

What Wine does not cover, and needs a Windows machine:

- `example_e2e` and `install_relocated` fail: Wine runs everything elevated, and the OpenXR
  loader ignores `XR_RUNTIME_JSON` in elevated processes. With
  `wine reg add 'HKLM\SOFTWARE\Khronos\OpenXR\1' /v ActiveRuntime /d Z:\<build>\puppetxr-dev.json`
  `example_e2e` passes; remove the value afterwards.
- The real window tests that need the window at its shown size fail under WSLg's X server:
  `OxrD3DRealWindowTest.first_frame_has_the_shown_windows_size/D3D12` and
  `a_click_on_a_panel_leaves_the_mouse_to_the_panels` (the click lands where no panel is); on
  Windows they pass.
- Shaders compile with Wine's d3dcompiler, which lacks `atan2` (`docs/shaders.md`); the real
  bytecode comes from Windows' `d3dcompiler_47.dll`.
- Wine's D3D11 has no `ClearView` and mishandles `UpdateSubresource` into typeless textures, so
  the test app fills swapchain images with cleared textures and `CopySubresourceRegion`.
- MSVC and clang for the MSVC ABI, their warnings, and the real D3D runtimes and drivers.

## Running apps against the runtime

Any OpenXR app, e.g. `hello_xr` built from OpenXR-SDK-Source (not part of this build):

```
XR_RUNTIME_JSON=$PWD/build/gcc-debug/puppetxr-dev.json hello_xr -g Vulkan2
PUPPETXR_HEADLESS=1 XR_RUNTIME_JSON=$PWD/build/gcc-debug/puppetxr-dev.json hello_xr -g Vulkan2
```

Never edit the user's `active_runtime.json`; always select the runtime with
`XR_RUNTIME_JSON`.

A headless smoke test of any app, usable in CI:

```
PUPPETXR_HEADLESS=1 PUPPETXR_FIXED_STEP=1 PUPPETXR_SCREENSHOT_AT=30 PUPPETXR_EXIT_AT=60 PUPPETXR_SCREENSHOT_DIR=out \
XR_RUNTIME_JSON=$PWD/build/gcc-debug/puppetxr-dev.json hello_xr -g Vulkan2
```

This renders 60 frames at a fixed step, writes `out/puppetxr_frame_30.png` and asks the app
to exit through the normal `STOPPING`/`EXITING` session states.

## Conformance

`-DPUPPETXR_BUILD_CTS=ON` builds OpenXR-CTS (`cmake/cts.cmake`, an ExternalProject because
the CTS brings its own loader and openxr targets) and adds three ctest entries, all
headless with invalid handle and type checks (`-H -T`) and interactive tests excluded:

- `cts_vulkan2`: everything on `XR_KHR_vulkan_enable2`
- `cts_vulkan`: everything on `XR_KHR_vulkan_enable`
- `cts_opengl`: everything on `XR_KHR_opengl_enable` (Xlib binding). The CTS OpenGL plugin
  opens a GLX window of its own and needs an X server with GLX and XFree86-VidMode (Xorg or
  Xwayland; Xvfb lacks VidMode and the plugin crashes); without `DISPLAY` it is skipped. The CTS
  OpenGL ES plugin only has the Android binding, so GLES is covered by `OxrGLTest` and the
  render tests. On Windows the plugin uses WGL and a window of its own; it always runs.
- `cts_d3d11`, `cts_d3d12` (Windows): everything on `XR_KHR_D3D11_enable` and
  `XR_KHR_D3D12_enable`
- `cts_headless`: only `[XR_MND_headless]` with the extension enabled; enabled globally
  it makes binding-less sessions valid, which the graphics binding tests reject
- `cts_automation_simple`, `cts_automation_touch`, `cts_automation_touch_pro`,
  `cts_automation_touch_plus`: the interactive input tests (`[actions][interactive]~[no_auto]`)
  with `XR_EXT_conformance_automation`, which lets the CTS press the buttons itself, on
  `khr/simple_controller`, `oculus/touch_controller`, `facebook/touch_controller_pro` and
  `meta/touch_controller_plus` (the last two with their extension enabled).
- `cts_composition`: the 17 core interactive composition tests, see below.
- `cts_composition_gl`: the same scenes drawn by the CTS OpenGL plugin on llvmpipe
  (`--graphics OpenGL`, `LIBGL_ALWAYS_SOFTWARE=1`), compared against the Vulkan references with
  tolerance 3: GL and Vulkan compositing agree. `GradientFormatsLinearVsNonLinear` is left out:
  its labels name GL formats, and the plugin never enables `GL_FRAMEBUFFER_SRGB`, so its gradient
  reaches the sRGB swapchain unencoded and shows darker on any runtime that samples it as sRGB.
  Needs an X display; skipped without `DISPLAY`.
- `cts_scenarios`: the core scenario tests (`[scenario][interactive][no_auto]`) and those of
  implemented extensions, scripted, see below.

```
cmake -B build-cts -G Ninja -DPUPPETXR_BUILD_CTS=ON -DPUPPETXR_BUILD_APPS=OFF -DPUPPETXR_BUILD_TESTS=OFF
ninja -C build-cts
ctest --test-dir build-cts --output-on-failure
```

On Windows configure from a developer shell (`cmake --preset msvc-release -B build/cts-windows
-DPUPPETXR_BUILD_CTS=ON ...`); the CTS itself always builds with `cl`, whichever compiler builds
puppetxr, as clang fails its `-Werror`. `cts_composition` and `cts_scenarios` need lavapipe and run
on Linux only. Catch2 reads arguments starting with `/` as options on Windows, so values that are
paths go as `--option=/path`.

Each pass writes `build-cts/<name>.xml` (JUnit) and `build-cts/<name>.log`
(`PUPPETXR_LOG_FILE`; Catch2 swallows stderr). The CLI must run from its own directory, which
holds its assets and the conformance API layer (`XR_API_LAYER_PATH`); the CTS enables
that layer itself, so do not pass `-L`. Results are tracked in `TODO.md` step 9.

### Composition tests against reference images

The CTS composition tests show a scene and wait for a person to press Select (pass) or Menu.
`tools/cts_composition.py` runs them unattended and lets images decide instead:

1. One `conformance_cli` process per test, on lavapipe (`VK_DRIVER_FILES`), headless, fixed
   step, 640x640 per eye, with the simulator started paused (`PUPPETXR_CONTROL_PAUSED=1`).
2. One `puppetxrctl shell` per instance: `mirror both`, the test's setup (`SETUP`, e.g. hands in
   front of the head for QuadHands), `step 60`, `screenshot`, then Select and `step 5` twice.
   Repeated while the instance shows further scenes; a new instance gets a new shell.
   Stepping a paused simulator puts every screenshot on an exact frame of its instance, so
   time-based scenes (StaleSwapchain toggles at 1 Hz) are reproducible.
3. Each `<test>_<scene>.png` is compared with `tests/cts/composition/`. A pixel fails when a
   channel differs by more than `--tolerance` (`PUPPETXR_CTS_TOLERANCE`, default 0). Failures write
   `<scene>_diff.png` (failing pixels red over the dimmed reference). `report.html` shows
   reference, result and difference side by side.

Updating references: run with `--update`, look at every image in `report.html` against the
CTS example images (`*.png` next to `conformance_cli`), commit only what is right.

`EXTENSION_TESTS` adds the composition tests of implemented extensions: the visibility mask (with
the lens mask on, `ENV`; without it the mask cuts nothing),
inverted alpha, image layout, equirect, equirect2 and `ProjectionDepth`
(`XR_FB_composition_layer_depth_test`, the cubes interleaved as in `projection_depth.png`).

Reproducibility: the references are bit-exact for lavapipe of Mesa 26.2.4 (LLVM 23.1.1,
256-bit vectors). Another Mesa or LLVM may move edge pixels. CI must use that Mesa, or set a
tolerance. Two runs on the same machine are bit-identical.

Lavapipe without root, on Arch: fetch the exact package from the archive, verify it, unpack it,
and point an ICD manifest at it.

```
curl -LO "https://archive.archlinux.org/packages/v/vulkan-swrast/vulkan-swrast-1:26.2.4-1-x86_64.pkg.tar.zst"{,.sig}
gpg --homedir "$(mktemp -d)" --import /usr/share/pacman/keyrings/archlinux.gpg   # then gpg --verify with that homedir
mkdir -p build/lavapipe/root && tar -C build/lavapipe/root -xf vulkan-swrast-*.pkg.tar.zst
sed "s|\"library_path\": *\"[^\"]*\"|\"library_path\": \"$PWD/build/lavapipe/root/usr/lib/libvulkan_lvp.so\"|" \
    build/lavapipe/root/usr/share/vulkan/icd.d/lvp_icd.json > build/lavapipe/lvp_icd.json
cmake build-cts -DPUPPETXR_LAVAPIPE_ICD=$PWD/build/lavapipe/lvp_icd.json
```

With a system lavapipe (`/usr/share/vulkan/icd.d/lvp_icd*.json`) CMake finds it by itself.

### Scripted scenario tests

`tools/cts_scenarios.py` plays each scenario as a person would. The same paused setup is used,
and one `puppetxrctl shell` sets device poses and inputs and steps frame by frame
(`FRAME_RATE` 90 Hz, so scripted velocities are exact). The CTS judges the result itself.
Scripts may return problems of their own, and visual scenes are compared with
`tests/cts/scenarios/` like the composition tests. A scenario still running after its script
gets Menu, which ends every core one, and counts as failed; the hand tracking scenario ignores
Menu and runs into the timeout instead.

| test | script | what it exercises |
|---|---|---|
| SpaceOffsets | grip moves along and turns about each axis (up to 1.4 m/s, 11 rad/s) | offset spaces, linear and angular velocities, lever arm |
| HapticInterrupt | Menu twice, checks `haptic right` each time | 2 s haptic at 0.25, replaced at 0.75, ends after 2 s |
| InteractiveThrow | swings at a computed launch velocity, releases Select, once per target | locate at `lastChangeTime` from the pose history, velocities |
| GripAndAimPose, GripSurface | hands thumbs-up (grip pitched up 60 degrees, so aim is level), screenshot, Menu to swap, screenshot | grip, aim and grip_surface offsets against the spec diagrams |
| local_floor-local/-stage, XR_EXT_local_floor-local/-stage | head steps back and looks down, screenshot | LOCAL_FLOOR at stage y = 0 under LOCAL |
| XR_FB_hand_tracking_mesh-interactive, XR_EXT_hand_tracking-interactive | both hands tracked (`hand <side> hand`), swing apart, then the index tips meet in front of the head | hand meshes (joint cubes for EXT) of both hands, joints located in LOCAL; the CTS ends the test when the tips are within 1 cm (`INDEX_TIP` holds their offset from the grip) |

Targets of InteractiveThrow are in LOCAL; the script assumes LOCAL's origin is the default
head position (0, 1.6, 0). Add a script to `SCRIPTS` for a new scenario.
`VK_DRIVER_FILES=<lvp_icd.json> ctest --test-dir build` runs the whole suite on lavapipe too.

## Sample apps

`-DPUPPETXR_BUILD_SAMPLES=ON` fetches third-party OpenXR samples, patches them for Linux and runs
them as end-to-end tests (`cmake/samples.cmake`, ctest entry `samples`, driver
`tools/sample_tests.py`).

- Layout: `tests/samples/<app>/*.patch` (applied with `git apply` after checkout),
  `tests/samples/<app>/references/<test>/<shot>.png` (left eye). Each app is an
  ExternalProject pinned by commit and built into `build/<preset>/samples/<app>`.
- Run: one process per test, `--jobs` (default: the CPU count) side by side, headless, fixed
  step, started paused (`PUPPETXR_CONTROL_PAUSED=1`),
  room from the test (`PUPPETXR_ROOM`), anchor stores in the test's output directory. One
  `puppetxrctl shell` steps it, moves devices, presses inputs, takes screenshots, then sends
  `exit`. A test passes when its script found nothing wrong, the app exited with code 0 and
  every screenshot matches its reference (`--tolerance`, default 0, or the test's `tolerance`;
  at most `max_pixels` beyond it). A device command the simulator refuses (`input`, `pose`,
  `point`: a misspelt input such as Touch's `trigger/click`, which only has `trigger/value`) fails
  the test. Each result line has the test's seconds. Tests start longest first, by the
  durations of the last run kept in
  `<out>/timings.json`, so a run takes about as long as its longest test. The Meta samples take
  about 2 minutes on 16 threads, bound by XrHandsAndControllers and XrHandDataSource (about one
  frame a second each); the 16 Godot tests about 4 minutes on 4 threads, the longest (`godot_spectator_view_vk`) about
  2 minutes beside the others.
- A test is an entry in `TESTS` with a script; `script_still` is a single screenshot after
  60 frames. `--tests a,b` runs some, `--update` rewrites the references for review. Asked for
  nothing in particular it runs the apps built in `--samples` and names the others; a named test
  whose app is missing fails.
- Names of other projects stay as they are when this project is renamed: the vendors samples
  quit given `--xrsim-automated-tests`, upstream's flag for Meta's XR Simulator.
- Rendering: GLES apps run on llvmpipe through `EGL_PLATFORM=surfaceless`,
  `LIBGL_ALWAYS_SOFTWARE=1` (surfaceless EGL otherwise picks the GPU; radeonsi lacks
  `GL_OVR_multiview` on GLES, so XrPassthroughOcclusion aborts there), `LP_NUM_THREADS=0` and
  `MESA_SHADER_CACHE_DISABLE=true`. Shaders llvmpipe loads from its disk cache crash
  XrPassthrough, XrSceneModel, XrSpatialAnchor and XrSceneSharing in JIT code (Mesa 26.2.4, LLVM
  23.1.1: every run with a warm cache, none of 6 without). With rasterizer threads, 64-pixel
  tiles of XrSceneModel come out stale and differ from run to run (TODO step 12). Two runs are
  bit-identical; references are from Mesa 26.2.4 llvmpipe (LLVM 23.1.1), another Mesa may move
  pixels.
- Patches never touch puppetxr and stay minimal. Order of preference: compile fixes, the Linux
  platform branch, then app bugs puppetxr exposes (each with a comment); puppetxr follows the spec,
  not the quirks of other runtimes.

Meta OpenXR SDK (`meta-quest/Meta-OpenXR-SDK` v85, `tests/samples/meta_openxr_sdk/linux.patch`):
the patch adds `XR_OS_LINUX` (GLES 3 on EGL, `XrGraphicsBindingEGLMNDX`, pbuffer configs,
`apk://` mapped to the executable's directory, stderr logging, KTX, zlib and the loader built
from source) and destroys XrPassthroughOcclusion's depth swapchain before its provider (a
child dies with its parent). Clang only: the sources are not GCC-clean.

Godot XR demos (`godot-demo-projects` tag 4.7-6ad6167 on the official Godot 4.7.2 binary,
`tests/samples/godot_demos/quit_on_stop.patch`), Linux x86_64 with `Xvfb`, or Windows x64 (below):
- The build downloads Godot (SHA-256 pinned), clones the demos and imports each project
  headless, so runs start without importing. Godot's user data, its shader cache included,
  goes to `build/<preset>/samples/godot/userdata` (`XDG_DATA_HOME`).
- The driver starts one Xvfb display for all Godot tests. Each demo runs as `godot --path
  <demo> --fixed-fps 90` (process and physics steps independent of the wall clock) with
  `--rendering-method gl_compatibility --rendering-driver opengl3` (`_gl`) or `mobile` and
  `vulkan` (`_vk`), and an `PUPPETXR_CONFIG` of 640x672 per eye. Steps are at most 10 frames:
  Godot on lavapipe draws about a frame a second while other tests run, and puppetxrctl gives up
  on a step after a minute.
- On Windows x64 (`cmake/samples.cmake`) the build downloads the Windows build of the same Godot
  and mesa-dist-win 26.2.4 (`mesa3d-26.2.4-release-msvc.7z`, SHA-256 pinned), the Mesa of the
  references, into `build/<preset>/samples/mesa`. The `_vk` tests run on its lavapipe
  (`VK_DRIVER_FILES`); Godot's user data goes to `samples/godot/userdata` (`APPDATA`). The `_gl`
  tests do not run: Godot sets its window's pixel format twice, which Mesa's WGL refuses, so the
  compatibility renderer cannot start on llvmpipe. A few pixels differ from the Linux references by
  up to 3 (same Mesa, LLVM 23.1.2 instead of 23.1.1), so the Windows `samples` entry passes
  `--tolerance 3`; two runs are bit-identical. The 8 `_vk` tests take about 1 minute on 4 threads.
  Meta's samples do not run on Windows yet (TODO step 12).
- On macOS (`cmake/samples.cmake`) the build downloads the universal macOS build of the same
  Godot, which carries MoltenVK; the `_vk` tests draw on the Apple GPU, the `_gl` ones do not run
  (no OpenGL, docs/building.md, macOS). Godot keeps its user data in
  `~/Library/Application Support/Godot`. The GPU shades and filters a few steps from the llvmpipe
  references and more at edges, so the macOS `samples` entry passes `--tolerance 16
  --pixel-budget 0.5` (at most 0.5 % of a screenshot's pixels beyond the tolerance) and `--jobs 1`.
  A screenshot of the Apple GPU that differs beyond that gets a macOS reference in
  `<references>/<app>/references_macos/<test>/`, which the driver takes over the common one and
  `--update` on macOS writes; two runs must give bit-identical frames first.
- Godot compiles pipelines in the background and draws with fallbacks until they are ready,
  so before its script a demo runs until two screenshots 10 frames apart are equal
  (`settle_still`); a demo that never holds still fails.

Godot OpenXR vendors samples (`godot_openxr_vendors` 5.1.0-stable and its released addon):
the build copies the addon into each sample and imports it twice
(`tests/samples/godot_vendors/import.cmake`: the first import aborts on exit); the driver runs
them as the Godot demos, plus `-- --puppetxr-automated-tests`.

Core apps (hello_xr of OpenXR-SDK-Source 1.1.63, Monado's openxr-simple-example and xrgears):
hello_xr needs `Xxf86vm` and `Xrandr` (on macOS nothing; `tests/samples/openxr_sdk/macos.patch`
lets its `XR_KHR_vulkan_enable` path, which makes its own instance, ask for MoltenVK), the simple example SDL2, xrgears meson, `xxd`,
`glslangValidator` and glm; each is skipped when its tools are missing. A test entry with
`display` runs in the Xvfb display, one with `stdin_open` keeps stdin open until it ends.

Unity samples on macOS: `-DPUPPETXR_UNITY=<Unity.app>/Contents/MacOS/Unity` builds the MRUK sample
as an Apple silicon player (`player/MRUK.app`, run as `Contents/MacOS/MRUKSample`) on Unity's
OpenXR plugin and Metal (`XR_KHR_metal_enable` under Unity's provisional name `XR_KHRX2_metal_enable`,
whose requirements function is `xrGetMetalGraphicsRequirementsKHRX2`); the Oculus XR plugin has no
macOS player, so only the 13 `unity_mruk_*` tests run (about 15 minutes).
- Unity's macOS editor aborts in native code after a successful batch run ("terminate_handler
  unexpectedly returned"), so `build.cmake` counts each step as done by its log (`unity_run`).
- The plugin loads `openxr_loader` plus `.dylib` from the player's `PlugIns`, where the package's
  loader is `libopenxr_loader.dylib`; `PuppetxrBuild.cs` copies it under the name the plugin loads.
  The player asks for the microphone, which needs a usage description on macOS.
- The player loads no Vulkan loader, so the mirror window opens without Vulkan
  (`Platform.a_window_without_vulkan_still_opens`).
- Each test first deletes `~/Library/Application Support/Meta/MRUKSample/mruk_config.dat`: it
  keeps the world-locking anchor of the previous run, which changes the panel's log.
- Metal draws a few pixels differently from the Windows D3D11 references, so the entry passes
  `--tolerance 16 --pixel-budget 0.5`; a test whose Metal image differs beyond that gets a macOS
  reference (`references_macos`), not a looser tolerance: `unity_mruk_keyboardtracker` (the mask
  over Unity's sky and the keyboard's edge) and `unity_mruk_findmultispawn` (the dashed spawn rings
  and the controllers' outlines, 2166 pixels against a budget of 2150). Three runs gave
  bit-identical frames first.
- `ignore_macos` in a test lists rectangles (fractions of the image) that the comparison skips on
  macOS and draws blue in the diff (`cts_composition.compare`'s `ignore`): MRUK logs its
  asynchronous events onto the keyboard tracker's panel in varying order there.
- OVRPlugin's analytics thread sometimes locks a destroyed mutex while a Unity player quits on
  macOS (`exit_crash_ok`, `ovrplugin_crash_after_openxr`). That exit passes only when the player's
  log shows OpenXR shut down ("Report Reason: System Shutdown"), the crash report of the process
  (`~/Library/Logs/DiagnosticReports/*.ips`) has its crashed thread in `OVRPlugin.dylib`, and no
  frame of the report is in `libpuppetxr.dylib`. No player crashed in the runs so far, so the report
  parsing has not met a real report yet.
- No occlusion in `unity_mruk` is expected: Meta's Depth API reaches Unity only through the
  Oculus XR plugin, which has no macOS player; Oppy is drawn over the table, as on Windows.

Unity samples (Windows): `-DPUPPETXR_UNITY=<Unity.exe>` (the projects' editor, 6000.0.66f2, with a
license) adds Meta's MRUK samples (`oculus-samples/Unity-MRUtilityKitSample` at the 207.0.0
release), its Shared Spatial Anchors sample and the ctest entry `samples_unity`:
- `puppetxr_unity_sample` (`cmake/samples.cmake`) clones a sample per player and
  `tests/samples/unity/build.cmake` builds it in batch mode. Sample repositories ignore
  `Assets/TextMesh Pro`, which the editor imports interactively on first use; without it menus
  have neither text nor buttons, so the first build resolves the packages and imports uGUI's
  `TMP Essential Resources.unitypackage`. The first build takes about 6 minutes; later ones
  reuse the project's `Library` in the build tree. `PuppetxrBuild.cs` builds the player with Mono
  (a project's IL2CPP needs an editor module that may be missing) and the project's scene list;
  `PROJECT` names the project's folder in the repository, `UNITY` another editor: the Depth API
  sample needs 6000.3 (`-DPUPPETXR_UNITY_6000_3=<Unity.exe>`; without it the sample is skipped). With
  the player argument `-puppetxr-scene <name>` (`PuppetxrFixedStep.cs`) a player starts in that scene,
  which the Starter Samples and Depth API tests use instead of clicking through menus.
  `PACKAGE_SAMPLES` (`<package>/<folder under Samples~>`) imports samples of the project's
  packages into `Assets/Samples`, as the Package Manager's Import button does, and the player
  gets their scenes after the project's own (`-puppetxrScenes`); the Starter Samples player carries
  the Meta XR Core SDK's Sample Scenes and Haptics this way.
- Two players of the MRUK sample: `unity_mruk` on Unity's OpenXR loader, as the sample ships,
  and `unity_mruk_oculus` on Unity's Oculus XR plugin (`LOADER oculus`: the build adds
  `com.unity.xr.oculus` 4.5.2 and `PuppetxrOculusLoader.cs` switches the Windows loader and
  registers its settings). Meta's Depth API has no Windows provider on the OpenXR loader; on the
  Oculus loader OVRPlugin drives the runtime itself, its app instance is named `Unity`, and
  occlusion works. OVRPlugin paces Unity's frames there, not locked to the steps, so its tests
  allow 6000 differing pixels (`max_pixels`) and skip the walk.
- The player draws with D3D11 on the GPU, so the references are of one machine (TODO step 12).
  Without a GPU it runs on WARP with `-force-driver-type-warp`: bit-identical run to run,
  about twice as slow, not matching the GPU references.
  It runs as `Oculus` with time 0 accepted, which puppetxr sets itself when OVRPlugin is loaded
  (docs/config.md, Known modules), 640x672 per eye
  and a 320x240 window. Unity creates two probe instances before its own, so the driver waits
  for the discovery file of `MRUKSample` (`instance`), then until the menu's first button shows
  (`ready`, wall-clock loading and the splash screen).
- `unity_mruk_menu` shots the menu once it holds still; `unity_mruk_<scene>` clicks the scene's
  button (`puppetxrctl point right <u> <v> left grip -60`, right trigger pressed and released over
  it), turns both rays to the floor (a ray left on a panel may stay drawn), hides the
  controllers (some scenes light them with moving lights) and shots after 300 frames.
  KeyboardTracker gets a keyboard prop and QRCodeDetection a QR prop (`PUPPETXR_PROPS`).
  `*_passthroughrelighting` waits for Oppy to spawn, looks down at him (`oppy`), then from low
  behind the coffee table (`behind_table`: the Oculus loader's Depth API hides him behind it,
  OpenXR draws him over it) and above it (`over_table`), then walks him forward and right with
  the left stick (`walked`, `walked_right`, OpenXR only), his light relighting the floor and the
  TV stand; its controllers go behind the head, as the stick needs them tracked.
- Unity's game time follows the wall clock, and its random numbers a seed from it. The build
  copies `tests/samples/unity/PuppetxrFixedStep.cs` into the project: with the player argument
  `-puppetxr-fixed-step` it sets `Time.captureDeltaTime` to 1/90 s and seeds `UnityEngine.Random`,
  so videos, animations, spawns and walks follow the stepped frames and every run is
  bit-identical (tolerance 0, no pixel allowance). Particle systems seed themselves anew each
  time they play, so each one gets a fixed seed when its scene loads (a train's smoke differed
  run to run); UI colour fades (`Selectable` transitions) run in unscaled time, which follows
  the wall clock even then, so every `Selectable` fades in no time.
- Players start one at a time (`start_alone`): OVRPlugin of players starting together crashed in
  its JSON reader (exit code 0xC0000409). It still does now and then, reading the log another
  running player writes to `%LOCALAPPDATA%\Oculus\Unity\logs` (a known folder, so the
  environment cannot separate them): a test whose player ends with that code before its first
  screenshot runs up to twice more (`retry_start_crash`), and the result says so. An app whose simulator is not up after 90 s, or that has
  not exited 60 s after `exit`, fails, with the stacks of its threads in `start_stacks.txt` or
  `hang_stacks.txt` when `cdb` is installed.
- Hangs end a test in minutes, not at its timeout. Once the app is ready, a control command
  without a reply in 120 s (`HANG_SECONDS`) marks it hung. A line in the ends of `app.log` or
  `game.log` after which an app never draws again (`FATAL_MARKERS`: a lost Vulkan or D3D device,
  Unreal's `Fatal error!`) ends the wait for ready. Either way the test saves the stacks to
  `hang_stacks.txt`, kills the app and reports the command or the log line with its two lines
  before. Before ready the shell waits as long as `ready_seconds`, as Unreal compiling shaders makes
  no call to puppetxr for minutes.
- The player starts an adb server, which inherits its handles and kept the test's logs open, so
  on Windows every sample runs in a job object that kills what it started when the test ends.
- `unreal_shared_anchors_sharing` runs two Unreal players the same way (`peer`); each gets
  `-puppetxr-user-id=<its PUPPETXR_USER_ID>`, which the sample's patch hands out as the Meta platform's
  user id. Unreal's sessions and replication time out in wall-clock time, so both run freely
  (`resume`): the script waits on frame counts (`wait_frames`), on the logs (`Join succeeded`) and
  on the API trace (`PUPPETXR_TRACE`), which also decides; the shots are not compared.
- `unity_ssa_colocation` (Meta's `Unity-SharedSpatialAnchors` at the 207.0.0 release) runs two
  players on one simulated cloud: the test's `peer` starts a second process with
  `PUPPETXR_USER_ID=2`, anchors of its own and the shared `PUPPETXR_CLOUD_DIR`, driven through its own
  shell; `{prefix}` in arguments keeps its logs apart (`peer_player.log`). In Colocation & Group
  Sharing the host advertises, places an anchor at a fixed right controller pose and shares it
  with its group; the peer discovers the group, joins it (which loads the group's anchors) and
  reloads; both shoot the anchor from close by (`host_anchor`, `peer_anchor`). The panel hangs off
  the left controller and prints wall-clock times, so the controllers go behind the head first.
  The sample's buttons wait for a user of Meta's platform service, which a player without an app
  id never gets; `tests/samples/unity_ssa/sample_tests.patch` enables them when the platform fails
  to start, as the sample does when the login fails (group sharing needs no user).
  `puppetxr_unity_sample` applies `tests/samples/<name>/sample_tests.patch` when there is one.
- `samples_unity` runs one player at a time (about 4 minutes for the 17 tests; with players side
  by side OVRPlugin's start crash came often) and never beside `samples` (`RESOURCE_LOCK`):
  software-rendered samples beside them stretched the wall-clock loading past every timeout.

Unreal samples (Windows): `-DPUPPETXR_UNREAL=<UnrealEditor.exe>` of Meta's Unreal fork
(`Oculus-VR/UnrealEngine`, branch `oculus-5.8`, built from source; GitHub access needs an Epic
Games account linked to it) adds Meta's MRUK sample for Unreal (`oculus-samples/Unreal-MRUtilityKitSample`
at `5.8.0-v207` and the following commit) and the ctest entry `samples_unreal`:
- Meta's Passthrough, Occlusion, Shared Anchors, Interaction SDK and Movement samples run the same way
  (`puppetxr_unreal_sample`; docs/samples.md): the Quest ones on the mobile renderer (`-FeatureLevelES31`,
  their masks write only alpha), the Occlusion map's lighting baked with Lightmass after checkout
  (its lights are static and no built lighting is in the repository). OVRPlugin names its session
  instance after the project or the engine, so a test's `instance` may be a tuple of names; a
  test's `exit_codes` accepts an app's crash at exit that is the engine's (PTColorLut).
- `cmake/samples.cmake` clones it and builds its editor target with the engine's `Build.bat`
  (under a minute once the engine is built). The tests run it uncooked:
  `UnrealEditor.exe <uproject> -game -vr -deterministic -FPS=90`, 320x240 window, game log in the
  test's folder (`app_command`, `unreal` key of a test).
- It runs on Unreal's OpenXR plugin without OVRPlugin, as `puppetxr`; MRUK's shared library is a
  known module (docs/config.md). Unreal waits frames on its game thread and ends them on its RHI
  thread, so puppetxr reads its frames back in `xrEndFrame` (docs/openxr.md, Frame loop).
- Unreal destroys its first instance and makes a second of the same name; `puppetxrctl shell`
  waits for the process to listen again and follows it to the second one's server. Its first
  instance makes no calls while Unreal loads, so its shell has the long timeout Godot's has.
- `-dpcvars` turns off anti-aliasing (its jitter), film grain and the tonemapper's dither to the
  8-bit backbuffer (`r.BackbufferQuantizationDitheringOverride=24`), which change every frame. A
  few pixels of the lit room still change by one level, so the menu counts as still with 50
  pixels differing and the test allows as many (`max_pixels`).
- `unreal_mruk_menu` waits until the room is discovered and dressed (the sample's dark ceiling at
  the top of the view), then shoots the menu once it holds still: 22 s. `unreal_mruk_<map>` opens a
  demo map from the start level (`-ExecCmds=open /Game/...`, test key `args`; `map` would start one
  on its own), turns the head 10 degrees left of a piece of furniture whose colour is random and
  shoots after 300 frames, about 20 s each. Maps drawn differently every run (`UNREAL_MRUK_RANDOM`)
  have no reference (`compare` false); their script checks that most pixels are lit and the menu
  shows. Which maps and why: `docs/samples.md`. `unreal_mruk_core_demos` picks the 8 other entries of
  the menu's drop-down (16 clicks of 70 frames, 36 s), `unreal_mruk_maps_tab` loads Guardian from
  the Maps tab (33 s). `unreal_mruk_vr_preview` runs the editor in real time with puppetxr's window
  in front (it takes the foreground of the desktop) through three previews and its exit: 70 s. The 15
  tests take about 5 minutes one at a time. The first run after a
  build compiles the project's shaders before Unreal creates its session, so the test waits up
  to 15 minutes for it (`ready_seconds`) while screenshots fail.
- Graphics APIs and renderers: one test per Unreal project (`UNREAL_RHI_TESTS` in
  `tools/sample_tests.py`, with the RHI its project starts on: `DefaultGraphicsRHI`, else Unreal's
  D3D12) also runs as `<test>_<d3d11|d3d12|vulkan>_<mobile|desktop>` for every other combination
  (`-d3d11`, `-d3d12`, `-vulkan`; `-FeatureLevelES31` for the mobile renderer). OpenGL is left out:
  Meta's plugin presents only through D3D11, D3D12 and Vulkan. 30 variants of about 30 s each, 15
  minutes in all once the shaders are cached; the first run of a combination compiles the
  project's shaders for it, up to an hour (`ready_seconds` 3600).

Which samples run, what each misses and every change of the patch: `docs/samples.md`.

The pinned environment, `tools/samples/Dockerfile`: Arch Linux from the Arch Linux Archive
snapshot of 2026-10-15 (Mesa 26.2.4, LLVM 23.1.1, Xvfb, GCC and clang), base image pinned by
digest on ghcr.io. `tools/samples/run.sh [sample_tests.py args]` builds the image if needed,
builds puppetxr (Release) and the samples into `build/docker-samples` as the calling user, and runs
the tests; `--shell` opens a shell in it; `--unit [preset]` configures, builds and runs a preset's
unit tests (default `gcc-debug`) in it under Xvfb and lavapipe, the way to run the Linux tests
from macOS or Windows. The image is x86_64, emulated on ARM hosts (with Colima on macOS start it
with `--vz-rosetta`; QEMU works but is several times slower). The Dockerfile needs BuildKit
(`docker buildx`). A proxy (`HTTPS_PROXY`) and its CA (`SSL_CERT_FILE`)
are passed through; `PUPPETXR_CMAKE_ARGS` and `PUPPETXR_DOCKER_ARGS` add configure and `docker run`
arguments, e.g. `FETCHCONTENT_SOURCE_DIR_<dep>` with a `-v` mount for local dependency
checkouts. All sample references come from it. llvmpipe JITs for the host CPU, so another x86 CPU
moves pixels by 1 (TODO step 12): `run.sh` and the Linux `samples` entry pass `--tolerance 1`.

## End-to-end tests for apps

The pattern, shown by `apps/puppetxr_example.cpp` and its ctest `example_e2e`
(`apps/example_e2e.cmake`):

1. Record a session once: interactively (Simulator menu, Start recording) or scripted
   through the control API, with `PUPPETXR_RECORD=session.xrrec`.
2. In CI, replay it headless at a fixed step and let the app exit itself:

   ```
   PUPPETXR_HEADLESS=1 PUPPETXR_FIXED_STEP=1 PUPPETXR_REPLAY=session.xrrec PUPPETXR_REPLAY_EXIT=0 \
   XR_RUNTIME_JSON=$PWD/build/gcc-debug/puppetxr-dev.json my_app
   ```

3. Assert on what the app observed (its own log or state) and on screenshots taken
   with `PUPPETXR_SCREENSHOT_AT` or the API's `screenshot`. Replays are bit-identical, so
   screenshots can be compared byte for byte on the same driver; across GPUs compare
   with a tolerance, since drivers may round colour conversions differently.

`get_status` reports `divergences`: a replayed frame whose simulator state differs from
the recording, which means the recording does not belong to this build or was edited.

The example app takes `--frames N` (exit after N frames), `--script` (drive the
right trigger and the head through the control API), `--log` (one line per frame with
time, trigger and head position) and `--screenshot PATH FRAME`.

## Leaks

The `clang-asan` test preset runs LeakSanitizer on every test, `example_e2e` included, which
loads the runtime through the OpenXR loader like any app. It sets
`VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1`. Otherwise the Vulkan loader unloads the driver
before the leak check, and memory the driver still references shows up as leaked from
`<unknown module>`.

`tests/lsan.supp` suppresses leaks in code we do not build (Vulkan drivers and layers, the
Vulkan loader, dbus, glib). A suppression matches any frame of a stack, so the OpenXR loader is
never listed: every call into puppetxr passes through it. Never suppress a leak with a puppetxr frame.

The loader unloads the runtime after the last `xrDestroyInstance`. A library destructor then
calls `oxr_release_runtime`, which frees the runtime's global state, and `os_release`, which
frees the log mutex and closes the log file. Both refuse to run while an instance is alive.

To check an app for leaks of the runtime, build `clang-asan` and preload the ASan runtime if
the app is not built with it, as for TSan below
(`LD_PRELOAD=$(clang -print-runtime-dir)/libclang_rt.asan-x86_64.so`).

## Threads and ThreadSanitizer

Engines call the runtime from several threads. `OxrThreadTest` in `tests/tests_oxr.cpp` does
what they do: Unreal waits frames and reads input on the game thread while the RHI thread
begins, renders and ends them one frame behind; Unity renders on its render thread while the
main thread polls events, syncs actions, locates spaces and finally ends the session. Every
entry point holds the runtime mutex (`OXR_Lock`), so these tests only find something under
the `clang-tsan` preset. Removing one `OXR_Lock` makes them fail there.

Only our targets are instrumented, so TSan does not see SDL's atomics as ordering anything: a plain
field published through an `SDL_AtomicInt` shows as a race now and then. Make the field atomic too.
SDL's mutexes and semaphores are seen, through the pthread calls under them.

`tests/tsan.supp` holds suppressions for code we do not build (dbus under SDL video, Mesa's
drivers: `libgallium` where lavapipe is merged into it, `libvulkan_lvp` where it is not, and the
LLVM its shader compiler threads run in). Never suppress a report with a frame in puppetxr; fix it.

Any app, built without TSan, against the TSan runtime: the TSan runtime has to be preloaded, as
an uninstrumented executable does not bring it.

```
cmake --preset clang-tsan && cmake --build --preset clang-tsan
LD_PRELOAD=$(clang -print-runtime-dir)/libclang_rt.tsan-x86_64.so \
TSAN_OPTIONS="suppressions=$PWD/tests/tsan.supp" \
XR_RUNTIME_JSON=$PWD/build/clang-tsan/puppetxr-dev.json <app>
```

Only reports with a `libpuppetxr.so` or `src/` frame are ours. Reports in the app's own code can
be false positives, since its atomics are not instrumented. TSan exits with code 66 when it
reported anything.

## Scripted sessions

A recording is also a script: write one with `rec_writer_*` (inputs, poses, the room name and
its `w_content_hash`) and run any app with `PUPPETXR_REPLAY=<file> PUPPETXR_HEADLESS=1`, plus
`PUPPETXR_SCREENSHOT_AT` for pictures. Replays of generated files report every frame as
diverged since they carry no state hashes. Mixed reality apps only see what the eyes see:
raycasts outside the view return `HIT_POINT_OUTSIDE_OF_FOV`, so a script that places
objects must look at the surface, as a user would.

## Apps known to work

| App | Status |
|---|---|
| `hello_xr` (OpenXR SDK) | Vulkan2 and Vulkan under test (`docs/samples.md`); OpenGL (GLX) headless, frame 60 matches Vulkan2 but for one edge pixel. Its OpenGL window needs XFree86-VidMode (not Xvfb), and stdin must not be at EOF: its "press any key" thread quits on EOF |
| Monado's openxr-simple-example and xrgears | under test, `docs/samples.md` |
| `apps/puppetxr_example` | all modes, record and replay e2e |
| `apps/body_dump` | body tracking skeleton and joints of both joint sets; ctest `body_dump` compares a fixed-step run with `tests/body_dump_puppetxr.json` (`docs/body-tracking.md`) |
| `apps/eye_dump` | the eye gaze action space, the per-eye poses and the face weights, in the stage and the view space; ctest `eye_dump` compares a fixed-step run with `tests/eye_dump_puppetxr.json` (`docs/face-tracking.md`) |
| Meta OpenXR SDK samples | 12 of 20 under test, `docs/samples.md` |
| Godot 4.7.2 XR demos | 8 demos on the compatibility (GLX) and mobile (Vulkan) renderers, `docs/samples.md` |
| Godot OpenXR vendors 5.1.0 samples | 8 samples (Meta scene, passthrough, hands, layers, space warp, metrics), `docs/samples.md` |
| Meta's Unity MRUK samples (207) | Windows, D3D11: the start menu and all 12 scenes under test, `docs/samples.md` |

Vulkan tests enable `VK_LAYER_KHRONOS_validation` when it is installed
(`tests/test_vulkan.h`) and fail on any validation error, including objects leaked at
`vkDestroyInstance`.
