# puppetxr

An OpenXR simulator runtime that runs inside the app process. An unmodified OpenXR
app loads `libpuppetxr` through the standard OpenXR loader; the library simulates a
headset and two controllers driven by keyboard and mouse, composites the app's layers
into a mirror window (or offscreen in headless mode), and can record and replay
sessions bit-identically for bug reproduction and end-to-end tests.

Read `TODO.md` first: it says what is done and what is next. Keep it current. It is the
work list for agents and humans alike: each open item says what is missing, where to start
(files, docs, the test to write) and how to tell it is done. State facts about the code and the
targets, not about who worked on it or in which environment: no session history, tool
limitations or private context.

## Rules (non-negotiable)

1. **Test driven.** Write the gtest first, make it fail, make it pass. No feature
   without a test. `tests/` has one file per layer. Every bug found, by a user or by
   us, gets a regression test that reproduces it before the fix lands, so it can
   never come back.
2. **Never crash on input.** Every `xr*` entry validates handles, pointers, struct
   types, enums and call order and returns an `XrResult`. Handles are generational
   indices, never dereferenced pointers. Corrupt input is logged with `LOG_ERROR`
   and asserted so a debugger breaks; the build without validation still returns
   the error. Spec-mandated checks are always compiled in; `PUPPETXR_VALIDATION`
   only controls internal `ASSERT`s.
3. **Deterministic.** The only wall-clock read is `SIM_Clock` in realtime mode.
   Device updates run in a fixed order once per `xrWaitFrame`. All math goes through
   `zmath` (own transcendentals, `-ffp-contract=off`, no fast-math). Randomness
   must be seeded by the user. Determinism has tests.
4. **No STL** except where unavoidable (gtest in `tests/`). Containers are
   `Array`, `SparseArray`, `HashMap`, `Handle` from `base.h`. Strings are
   `const char*` and fixed `char[]` buffers.
5. **No allocation in hot paths.** Allocate at create time, prefer fixed arrays
   with `#define` maxima; use `Array` when assuming a size would make the runtime
   less robust.
6. **Compile times.** One translation unit per layer (see Architecture). Templates
   only for the base containers. No third-party header in our headers; a third-party
   header is included by at most two `.cpp` files. A new file needs a layer argument;
   ask before creating one.
7. **Comments** follow the Linux kernel style: what and why, never how. Justified
   only for a non-obvious invariant, a spec requirement (cite the section), a
   workaround or a tricky algorithm. No comments that restate code, no narration,
   no doc headers on functions. Section banners (`//\n// Swapchains\n//`) are fine.
8. **Dependencies** only if unavoidable and battle tested, fetched with CMake
   `FetchContent`, pinned by tag and hash. Prefer writing it ourselves.
9. **Docs.** Every feature updates the matching `docs/*.md`. Docs are for humans and
   LLMs: short sections, file paths, invariants, no narrative.
10. **Open source.** Everything in the repo stands on its own for any reader. No reference
    to files, paths, machines or people's accounts outside it, and no unpublished or private
    projects: name an app only if anyone can get it, otherwise describe what was tested.
    Binary assets carry no metadata with local paths, host or user names.
11. **Compatibility.** The public C API (`src/puppetxr.h`) is C89 and stays C89. From
    puppetxr 1.0 on, never break it or the recording format. The API only grows: append functions and fields, append
    enum values, never remove, reorder or change meaning, bump `PUPPETXR_API_VERSION` for
    additions. The recording reader keeps a case for every format version ever
    written; a layout change bumps `REC_FORMAT_VERSION` and adds a reader. Input,
    device and controller names are part of both and never change. Before 1.0 these
    may still break, but every break is a version bump.
12. **Iteration time.** How long a test takes is part of its design: unit tests run in seconds,
    one sample test in under two minutes alone. Run only the frames that prove the point, at the
    smallest resolution that shows it; wait on conditions, never on fixed sleeps; independent
    tests run side by side. `docs/testing.md` lists what each suite costs; a change that makes a
    suite slower says why in its commit message.

## Style

Full guide with examples: `docs/style.md`. Essentials: C-style C++20, `struct` only,
module prefixes (`R_Device`, `r_create_device`, `OXR_`, `SIM_`, `W_`, `COMP_`, `UI_`,
`REC_`, `os_`), `u8..u64 i8..i64 f32 f64 b8 usize` typedefs, Allman braces always (no
brace-less or same-line `if` bodies, `case` statements on their own line below the
label), long lines over wrapped lines (pull out a local instead), no virtual functions
(tagged struct + `switch`, or function pointers), 4 spaces,
column-aligned declaration blocks, designated initializers, `= {}` zeroing,
opaque structs defined in the `.cpp`, `ASSERT`/`ASSERT_MSG`/`FAIL_MSG`/`LOG_*`, `defer`,
the warnings in `cmake/flags.cmake` (`-Werror`, shadowing, implicit conversions; GCC and clang). Sections in a file are in a fixed order: types,
file-local statics, public API, each under a banner comment.

## Architecture

Layers, bottom up. A layer includes only headers of layers below it. One `.cpp`
per layer so a change recompiles only the layers above it.

| Layer | Files | Prefix | Role |
|---|---|---|---|
| platform | `src/platform.h/.cpp` | `os_` | types, macros, log, assert, memory, time, files, sockets, processes, threads, SDL3 window and events (on Windows on a window thread of its own); the only file with OS conditionals |
| base | `src/base.h/.cpp` | none | `defer`, `Array`, `Handle`, `SparseArray`, `HashMap`, strings, `json_*`, hashes |
| zmath | `src/zmath.h/.cpp` | `zm::` | vectors, quaternions, matrices, poses, deterministic transcendentals |
| sim | `src/sim.h/.cpp` | `SIM_`/`sim_` | devices, poses, input sources, reference spaces, clock, config, control mapping |
| world | `src/world.h/.cpp` | `W_`/`w_` | simulated room: semantic entities, global mesh, raycast, visual mesh and albedo for passthrough |
| render | `src/render.h/.cpp`, `src/render_internal.h`, `src/render_vulkan.cpp`, `src/render_gl.cpp`, `src/render_d3d11.cpp`, `src/render_d3d12.cpp`, `src/render_metal.mm` | `R_`/`r_` | backend-neutral device over the app's graphics device; `render.cpp` dispatches to one file per backend; D3D only in Windows builds (`PUPPETXR_HAVE_D3D`), Metal (the only Objective-C++) only in macOS builds (`PUPPETXR_HAVE_METAL`) |
| comp | `src/comp.h/.cpp` | `COMP_`/`comp_` | composites layers offscreen, blits to the mirror window, screenshots |
| ui | `src/ui.h/.cpp` | `UI_`/`ui_` | ImGui panels; the only file including `imgui.h`; absent in headless-only builds |
| rec | `src/rec.h/.cpp` | `REC_`/`rec_` | binary record and replay of per-frame sim state |
| oxr | `src/oxr.h/.cpp`, `src/oxr_vulkan.cpp`, `src/oxr_gl.cpp`, `src/oxr_d3d.cpp`, `src/oxr_metal.cpp`, `src/oxr_trace.cpp` | `OXR_`/`oxr_` | the `xr*` API; one file per graphics binding, `oxr_vulkan.cpp` also confines `vulkan.h`; `oxr_trace.cpp` the API trace generated from the registry by `tools/gen_trace.py` |
| api | `src/puppetxr.h`, `src/puppetxr_api.cpp` | `puppetxr_` | exported C control API for tests and tools |

Details: `docs/architecture.md`. Per-layer docs: `docs/openxr.md`, `docs/compositor.md`, `docs/shaders.md`, `docs/world.md`, `docs/room-format.md`,
`docs/recording-format.md`, `docs/control-api.md`, `docs/control-cli.md`, `docs/determinism.md`, `docs/anchors.md`, `docs/props.md`, `docs/profiling.md`.
`tools/puppetxrctl.cpp` is the control CLI; it builds only the platform and base layers.
`tools/hlsl_compile.cpp` is a Windows build tool compiling the shaders to DXBC.
`tools/dist/dist.sh` builds the Linux release in the container of `tools/dist/Dockerfile`, `tools/dist/dist.ps1` the Windows release, `tools/dist/dist-macos.sh` the macOS release, `tools/dist/dist-src.sh` the source tarballs.
`tools/release.sh <version>` tags a release; `.github/workflows/release.yml` builds all three on the tag and drafts the GitHub release.

## Build, test, run

```
cmake --preset gcc-debug && cmake --build --preset gcc-debug && ctest --preset gcc-debug
XR_RUNTIME_JSON=$PWD/build/gcc-debug/puppetxr-dev.json <any openxr app>
PUPPETXR_HEADLESS=1 XR_RUNTIME_JSON=$PWD/build/gcc-debug/puppetxr-dev.json <app>
PUPPETXR_CONTROL=1 XR_RUNTIME_JSON=$PWD/build/gcc-debug/puppetxr-dev.json <app> & build/gcc-debug/bin/puppetxrctl status
```

Presets (`CMakePresets.json`, each in `build/<preset>`): `gcc-debug`, `gcc-release`,
`clang-debug`, `clang-release`, `clang-asan`, `clang-tsan`, `clang-tidy`, `dist`, `gcc-headless`;
`mingw-debug` cross-builds for Windows and runs the tests in Wine (`docs/testing.md`); on Windows
`win-clang-{debug,release,asan,tidy,dist}` and `msvc-{debug,release,asan,dist}`; on macOS
`mac-{debug,release,asan,tsan,dist}`. **Warnings are errors, and every
change builds warning-free with both GCC and clang.** What to run before a commit is the
author's judgement, scaled to the risk of the change: usually `gcc-debug`; `clang-debug` and
`clang-tidy` (findings are errors; it checks `src/` and `puppetxrctl`, not the tests) when the
code could plausibly trip them; `clang-asan`, `clang-tsan` and `gcc-headless` for memory
handling, locking, `ui`, `oxr` or CMake; `mingw-debug` (built and run in Wine) for `platform`,
the D3D or WGL code. A check may be skipped when you are confident the change cannot affect it;
say so in your report. Every preset and the CTS run occasionally and before every release.
Never silence a warning with a cast that hides a real bug; fix the type.

Options: `PUPPETXR_VALIDATION` (default ON), `PUPPETXR_WERROR` (default ON), `PUPPETXR_DIST`, `PUPPETXR_CLANG_TIDY`, `PUPPETXR_CLANG_TIDY_ALL`, `PUPPETXR_SANITIZE`,
`PUPPETXR_HEADLESS_ONLY`, `PUPPETXR_FAST_MATH`, `PUPPETXR_BUILD_TESTS`, `PUPPETXR_BUILD_APPS`,
`PUPPETXR_BUILD_CTS`, `PUPPETXR_BUILD_SAMPLES`.
More in `docs/building.md` and `docs/testing.md`.

## Working conventions

- Before a task: read `TODO.md`, the doc of the layer you touch, and only the file
  ranges you need (grep for the `prefix_verb_noun` function name).
- After a task: tests green, docs updated, `TODO.md` updated, short lowercase
  imperative commit message.
- An extension that lands or changes updates its row in `docs/extensions.md`.
- A change users notice adds a line under `## Unreleased` in `CHANGELOG.md`; a break of the C API,
  the recording format or the config starts with **Breaking:**.
- A change users notice (a feature, platform, graphics API, build or install step, test suite)
  updates `README.md`. It is for humans: short, goals, features, build, testing; details stay in `docs/`.
- When a design choice is not covered here or in `docs/`, ask instead of guessing.
