# Profiling

A zone profiler in the platform layer (`src/platform.h`, `src/platform.cpp`, section Zone
profiler) records where the runtime spends its time, on every thread that calls it.

## Capturing

- Config key `profile` or `PUPPETXR_PROFILE=<path>`: from `xrCreateInstance` to the runtime's
  shutdown, when the last instance goes. Engines that make a probe instance first (OVRPlugin)
  end that capture with the probe; use `puppetxrctl` for them.
- `puppetxrctl profiler start <path>` and `puppetxrctl profiler stop` on a running app
  (`docs/control-cli.md`); the path is the app's.
- C API: `profiler_start(path)`, `profiler_stop()` (`docs/control-api.md`).

The capture is a Chrome trace (`{"traceEvents":[...]}`, complete events `"ph":"X"` in
microseconds, one `thread_name` per thread) for Perfetto or `chrome://tracing`. Stopping also logs
the 20 zones of the most total time: calls, total, mean, max.

## Zones

`PROF_ZONE("name")` lasts to the end of its scope; names are string literals or `__func__`.
`os_profile_begin`/`os_profile_end` are the same by hand.

- Every `xr*` call: `OXR_Lock(__func__)` (`src/oxr_internal.h`), named after the entry point
  (`oxr_xrWaitFrame`); `xrWaitFrame` takes the lock by hand and has its own.
- Frames: `runtime lock wait`, `frame gate wait`, `frame pacing`, `sim step`, `window events`,
  `compose`, `compositor snapshot`, `mirror window`, `mirror skipped`, `ui render`.
- The window thread: `window tick`, `window show`, `ui build`, and on other threads
  `window thread call wait` while a call waits for the main thread (macOS).
- The render layer's dispatch (`src/render.cpp`), so every backend is covered at once:
  `gpu frame begin`, `gpu submit`, `gpu submit present`, `gpu immediate`, `gpu wait idle`,
  `gpu readback`, `window acquire`, `window present`, `window swapchain create`.

## Invariants

- Stopped, a zone costs one atomic load, so zones stay compiled in, dist builds too
  (`Profile.zones_cost_next_to_nothing_while_stopped`).
- Each thread has a ring of `OS_PROFILE_RING_EVENTS` (65536) zones and keeps the latest. It is
  allocated at the thread's first zone and kept for later captures, since the thread may still
  write while a capture stops; only its thread writes, and its count publishes the events. At most
  `OS_PROFILE_MAX_THREADS` (64) threads ever record. Nothing is allocated per zone.
- Zones deeper than `OS_PROFILE_MAX_DEPTH` (32) are not recorded. A zone still open when a
  capture starts belongs to neither capture (a generation per capture).
- Profiling never feeds the simulation, so determinism holds.
- Thread names come from `pthread_getname_np`, on Windows `GetThreadDescription` resolved with
  `GetProcAddress`; a thread without one shows as `main` (macOS's main thread) or `thread <id>`.

Tests: `Profile.*` (`tests_base.cpp`; the trace is checked by counting in its raw text, as a JSON
parse of a big trace took most of a minute), `OxrVulkanRealWindowTest.profile_names_the_calls_and_the_frame_parts`,
`ApiTest.profiler_starts_and_stops`, `ApiControlTest.puppetxrctl_profiles_a_running_session`.

## Findings

- Godot on MoltenVK (60 Hz display): with FIFO the mirror waited in `vkQueueSubmit` for a
  drawable and Godot ran at 53 instead of 100 frames a second; MAILBOX, else IMMEDIATE, fixed it.
- Presenting faster than the display held all of the window server's drawables, and the next
  acquire waited up to 20 ms; presents are capped at the display's refresh rate and skipped while
  the last one is on the GPU (`docs/compositor.md`, the mirror window never paces the app).
- Every window call from Unity's render thread waited out the 250 ms main-thread timeout while its
  main thread blocked on it; frames now post the window tick.
- The waiting main thread slept a millisecond between tries for the runtime lock; it now sleeps on
  the call queue's semaphore, which the unlock signals, after a 0.2 ms spin.
- Showing a hidden maximized window took 240 ms of `-[NSWindow zoom:]` animation; it takes
  milliseconds with the frame set without animation.
- The control server reads back every frame and waits for it (`comp_snapshot`), about 10 ms a
  frame in a debug build (TODO).
