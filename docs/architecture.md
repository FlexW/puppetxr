# Architecture

## What it is

`libpuppetxr` is an OpenXR runtime. The OpenXR loader finds it through a manifest JSON
(`XR_RUNTIME_JSON=.../puppetxr.json`), `dlopen`s it into the app process and calls
`xrNegotiateLoaderRuntimeInterface`. From then on every `xr*` call the app makes lands
in `oxr.cpp`. There is no service process and no runtime thread: the simulator window
and UI are pumped from inside `xrWaitFrame` and `xrEndFrame` on the app's thread.

The exceptions are Windows, where a window's messages reach only the thread that created
it and pumping takes every message of the calling thread, the app's included, and Linux, where
the app's main thread is the app's to block (`HAVE_WINDOW_THREAD`). There the platform layer
runs video on a window thread of its own (`platform.cpp`): it owns the mirror window, pumps it
and queues the events, which `os_poll_events` hands out on the app's thread as before. `os_*`
window functions and the UI's frame run there through `os_run_on_window_thread`. Input still reaches the sim only in `xrWaitFrame`, so recordings
and determinism are unchanged. Teardown happens in the last `xrDestroyInstance`: on Windows
library unload runs under the loader lock, where no thread may be joined.

## Layers

Bottom up. A layer includes only headers of layers below it. Each layer is one
translation unit so a change recompiles only the layers above it.

```
platform -> base -> zmath -> sim -> world -> render (+render_vulkan, render_gl, render_d3d11, render_d3d12) -> comp -> ui -> rec -> oxr (+oxr_vulkan, oxr_gl, oxr_d3d, oxr_trace) -> api
```

| Layer | Owns | Must not know about |
|---|---|---|
| platform (`os_`) | OS, windows and input, memory, time, log, assert (Platform layer below) | everything above |
| base | containers, strings, json, hashes | OpenXR, Vulkan |
| zmath (`zm::`) | deterministic math | OpenXR types |
| sim (`SIM_`) | devices, input sources, spaces, clock, config, controls | OpenXR, Vulkan, rendering |
| world (`W_`) | the simulated room: entities, global mesh, raycast, visual mesh (`world.md`) | OpenXR, Vulkan, sim |
| render (`R_`) | GPU resources and commands on the app's device; `render.cpp` dispatches to one `.cpp` per backend | OpenXR handles, sim |
| comp (`COMP_`) | layer compositing, mirror window, screenshots | OpenXR handles |
| ui (`UI_`) | ImGui panels editing sim state | OpenXR handles |
| rec (`REC_`) | recording file format, replay cursor | OpenXR, rendering |
| oxr (`OXR_`) | every `xr*` entry point, handles, validation, state machines | nothing; it is the top |
| api (`puppetxr_`) | exported control functions | implemented on top of oxr and sim |

Third-party headers are confined: `vulkan.h` in `render_vulkan.cpp` and
`oxr_vulkan.cpp`, `imgui.h` in `ui.cpp`, the X11 and xkbcommon headers in `platform_linux.cpp`,
AppKit in `platform_macos.mm`, `windows.h` in `platform_win32.cpp`, the D3D and DXGI headers in
`render_d3d11.cpp` and `render_d3d12.cpp` (and the test helper `tests/test_d3d.h`), `openxr.h` in `oxr*.cpp` and `puppetxr_api.cpp`, `jsmn.h` in `base.cpp`,
`stb_image_write.h` in `comp.cpp`, `stb_image.h` in `world.cpp`, `stb_truetype.h` in `platform_linux.cpp`. Across layer boundaries native
objects travel as `void*` or `u64`.

## Platform layer

One header, `platform.h`, over one file of shared code and one per platform; `platform_internal.h`
is what they share and nothing else includes.

| File | Holds |
|---|---|
| `platform.cpp` | log, asserts, profiler, strings, key names, the event queue, the window thread and its calls, the window functions, the offscreen video driver |
| `platform_unix.cpp` | Linux and macOS: pthreads, time, files, `posix_spawn`, sockets, lock files, `dlopen`, output capture |
| `platform_linux.cpp` | the Wayland and X11 video drivers |
| `platform_win32.cpp` | everything Win32: system calls and the `windows` video driver |
| `platform_macos.mm` | the main thread's call queue and the `cocoa` video driver (Objective-C++) |

- Video driver: a table of function pointers (`OS_VideoDriver`), one per window system:
  `wayland`, `x11`, `windows`, `cocoa`, and `offscreen`, whose windows have only a size and a
  headless Vulkan surface (`VK_EXT_headless_surface`). `$PUPPETXR_VIDEO_DRIVER` names one at
  `os_initialize`; otherwise the first of the platform's that finds a display (Linux: Wayland,
  then X11). None found: headless. `os_set_video_driver` switches at runtime (a GL app with an
  Xlib binding moves to `x11`, `docs/openxr.md`).
- Linux loads libwayland-client, libxkbcommon, libX11 and libXi at runtime, libwayland-cursor,
  libfontconfig, libXrandr and libXcursor when they exist, so `libpuppetxr.so` links none of them
  (`cmake/check_glibc.cmake`); their headers are needed to build, except Wayland's, whose ABI and
  protocol tables `platform_linux.cpp` declares. They stay loaded for the process.
- Wayland decorations: the compositor's (`xdg-decoration`) where it draws them (KDE, wlroots), else
  a title bar of puppetxr's own (GNOME): a subsurface above the window drawn on the CPU into shared
  memory (the title in the desktop's bold sans serif, which fontconfig names when it is installed,
  drawn by `stb_truetype`, else in a built-in 5x7 font; minimize, maximize, close; double click maximizes, right
  click opens the window menu), and a transparent subsurface below it, a 1x1 buffer stretched by
  its viewport, whose 8 points around the window take resize drags. Moving and resizing are the
  compositor's (`xdg_toplevel` move and resize); the window geometry takes in the title bar. A shown
  window waits for the first configure, as a buffer before it ends the connection.
- Raw events (`OS_RawEvent`): what a driver reports, in window points, queued from the video
  thread; `os_poll_events` hands the raw event to the UI's hook, then translates it to
  `OS_Event` for the sim. Repeats and text go to the UI only.
- The UI feeds ImGui itself from the raw events and sets the cursor shape, clipboard and input
  method position through `os_window_set_cursor`, `os_clipboard_text` and
  `os_window_text_input`; no ImGui backend file is built.
- Input methods: IMM32 on Windows, `NSTextInputClient` on macOS, XIM on X11, `zwp_text_input_v3`
  on Wayland, which routes a text field's keys through the input method: a key it takes comes back
  as committed text, a key it leaves arrives as a key and types as usual. The candidates show where `os_window_text_input`
  says; the preedit string is not drawn.
- Window thread (Linux, Windows): sleeps until the window system has events or
  `os_sys_wake_signal` (a pipe, an event) ends the wait; a call or post wakes it.

## Handles

An OpenXR handle given to the app is a `u64` made of `slot << 32 | generation`,
looked up in a `SparseArray` of the owning object. A handle is validated by
`SparseArray::is_valid` before use, so a stale, foreign or random handle returns
`XR_ERROR_HANDLE_INVALID` instead of crashing.

## Frame flow

- `xrWaitFrame`: take the runtime lock, pass the frame gate, advance `SIM_Clock`,
  pump window events and apply the control mapping (window mode), apply the replay frame
  or append the record frame, compute predicted display time and period, release the
  lock, sleep until the predicted time in realtime mode.
- `xrLocateViews` / `xrLocateSpace`: read sim state at the requested time
  (poses are sampled at frame boundaries; no extrapolation in v1).
- `xrEndFrame`: validate layers, composite them into the offscreen target on the
  app's queue, blit to the window swapchain, draw ImGui, present.

## Vulkan binding

With `XR_KHR_vulkan_enable2` the runtime creates the app's `VkInstance` and
`VkDevice`, adding the surface and swapchain extensions it needs for the mirror
window. With `XR_KHR_vulkan_enable` it lists those extensions for the app to enable.
Swapchain images are `VkImage`s created on the app's device; no external memory.
Headless mode skips the surface, composites offscreen only, and works on lavapipe.

## Validation policy

Spec-mandated checks (handles, null pointers, `type`/`next` chains, enums, call
order, session state) are always compiled in and return the spec's `XrResult`.
`PUPPETXR_VALIDATION=ON` additionally enables internal `ASSERT`s and consistency checks.
Both builds are shipped; the one without validation exists for performance
comparisons and should rarely be needed.

## Determinism

See `determinism.md`. Summary: one clock, fixed update order, own math, recordings
carry per-frame deltas, a state hash per frame for tests.

## Engine compatibility

The targets are native apps, Unity, Unreal Engine and Godot, all through the standard
loader. What they need from the runtime and how the design covers it:

| Need | Status |
|---|---|
| Both `XR_KHR_vulkan_enable` (Unreal creates its own `VkDevice`) and `XR_KHR_vulkan_enable2` (Unity, Godot) | both implemented; the device is wrapped either way |
| Calls from several threads: Unreal waits frames on the game thread and begins/ends them on the RHI thread, Unity syncs actions from the main thread while rendering elsewhere | one runtime mutex per call; `xrWaitFrame` blocks with the mutex released, so wait/begin on different threads works; `OxrThreadTest` covers both patterns under `clang-tsan` |
| Array swapchains (`arraySize = 2`, single-pass stereo) and depth swapchains | supported up to 2 layers; depth formats accepted, not composited |
| Many interaction profiles suggested at once (Vive, Index, WMR, touch, simple) | must not fail: every profile path the spec defines is accepted and stored, only touch and simple ever become active (TODO step 6) |
| `XR_KHR_composition_layer_depth`, `XR_KHR_visibility_mask`, `XR_EXT_hand_tracking`, `XR_FB_display_refresh_rate` are queried optionally | missing extensions are reported as unsupported, which all three engines handle; depth and visibility mask are planned for CTS |
| D3D11 / D3D12 (Unreal and Unity default to Direct3D on Windows) | `render_d3d11.cpp` and `render_d3d12.cpp` on the app's device and queue; tested on WARP and under Wine, not yet with the engines |
| OpenGL and OpenGL ES (Godot compatibility renderer, `XR_KHR_opengl_enable`, `XR_KHR_opengl_es_enable`) | `render_gl.cpp` over a context shared with the app's: EGL, GLX and WGL |
| Realtime pacing: engines expect `xrWaitFrame` to block for a display period | realtime clock mode sleeps the remainder of the period since the previous `xrWaitFrame` returned (`OxrTest.realtime_waits_pace_every_frame_to_the_display_period`) |
| Window events pumped inside `xrWaitFrame` while the engine calls it from a non-main thread | on Windows and Linux the window thread pumps; on macOS the app's main thread does, when it comes by (`docs/compositor.md`) |

The in-process design holds for all four: none of them needs a separate compositor
process, and sharing the app's `VkDevice` works because every engine hands the
runtime its instance and device through the graphics binding.
