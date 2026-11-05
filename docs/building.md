# Building

Requirements: CMake >= 3.25, Ninja, Python 3.6 or later (it generates the API trace from the
OpenXR registry, docs/openxr.md), a C++20 compiler (gcc, clang, MSVC), `glslc`
(from shaderc or the Vulkan SDK), a Vulkan driver (lavapipe suffices for headless). Optional:
`glslangValidator` and `dxc` to validate the shader variants of the other backends.

```
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug
```

On Ubuntu 24.04 every Linux preset (gcc, clang, clang-tidy, the sanitizers, headless, mingw-debug
under Wine) builds and passes with these packages; Docker and buildx run the sample and release
images (`tools/samples/run.sh`, `tools/dist/dist.sh`):

```
apt-get install build-essential gcc-14 g++-14 clang clang-tidy lld llvm cmake ninja-build pkg-config \
  python3 git git-lfs curl rsync glslc glslang-tools mesa-vulkan-drivers libvulkan1 vulkan-tools \
  libgl1-mesa-dri libegl1 libgles2 libegl-dev libgl-dev libglx-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxxf86vm-dev \
  libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libdrm-dev libgbm-dev \
  libdbus-1-dev libudev-dev libasound2-dev libpulse-dev libpipewire-0.3-dev xvfb xauth \
  mingw-w64 wine64 docker.io docker-buildx
```

Outputs: `build/gcc-debug/bin/libpuppetxr.so` (`puppetxr.dll` on Windows, `.dylib` on macOS), `build/gcc-debug/puppetxr-dev.json`
(manifest with an absolute path for use from the build tree), `build/gcc-debug/bin/puppetxr_tests`.

## Presets

`CMakePresets.json` has configure, build and test presets of the same name. Each builds into
`build/<preset>`, so all of them coexist in one checkout.

| Preset | Compiler | Type | Extra |
|---|---|---|---|
| `gcc-debug` | GCC | Debug | |
| `gcc-release` | GCC | RelWithDebInfo | |
| `clang-debug` | clang | Debug | |
| `clang-release` | clang | RelWithDebInfo | |
| `clang-asan` | clang | Debug | `PUPPETXR_SANITIZE=address`: ASan, UBSan and LeakSanitizer; tests run with `tests/lsan.supp` (testing.md) |
| `clang-tsan` | clang | RelWithDebInfo | `PUPPETXR_SANITIZE=thread`: TSan; tests run with `tests/tsan.supp`; also for running engines against the runtime (testing.md) |
| `clang-tidy` | clang | Debug | `PUPPETXR_CLANG_TIDY`: every compile of the runtime and `puppetxrctl` also runs clang-tidy; a finding fails the build |
| `dist` | clang | Release | `PUPPETXR_DIST`: the build we distribute, see Distribution |
| `gcc-headless` | GCC | Debug | `PUPPETXR_HEADLESS_ONLY` |
| `mingw-debug` | mingw-w64 GCC | Debug | Windows cross build on Linux, tests run in Wine (testing.md); not a release toolchain |
| `win-clang-debug`, `-release` | clang (MSVC ABI) | Debug, RelWithDebInfo | Windows |
| `win-clang-asan` | clang (MSVC ABI) | Debug | ASan and UBSan; no LeakSanitizer or TSan on Windows; clang's ASan runtime DLL is copied next to the binaries and gtest is built sanitized too (the STL's annotations must match) |
| `win-clang-tidy` | clang (MSVC ABI) | Debug | clang-tidy on the Windows code |
| `win-clang-dist` | clang (MSVC ABI) | Release | the Windows distribution, see Windows releases |
| `msvc-debug`, `-release` | MSVC | Debug, RelWithDebInfo | Windows |
| `msvc-asan` | MSVC | Debug | `/fsanitize=address` |
| `msvc-dist` | MSVC | Release | LTCG, hardened, PDB |

| `mac-debug`, `-release` | Apple clang | Debug, RelWithDebInfo | macOS 11+, arm64 |
| `mac-asan`, `mac-tsan` | Apple clang | Debug, RelWithDebInfo | the sanitizers |
| `mac-dist` | Apple clang | Release | universal arm64 and x86_64 |

Windows presets show only on Windows hosts, the `mac-*` presets only on macOS, `mingw-debug` only elsewhere.

## Warnings

`cmake/flags.cmake` applies one warning set to every target of ours (library, tests, apps,
tools), never to third-party code: `-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
-Wdouble-promotion -Wformat=2 -Wundef -Wvla -Wpointer-arith -Wcast-qual -Wextra-semi
-Wmissing-declarations -Wimplicit-fallthrough -Wnon-virtual-dtor`, plus `-Wduplicated-cond
-Wduplicated-branches -Wlogical-op` on GCC and `-Wshadow-all -Wcomma -Wunreachable-code
-Wloop-analysis` on clang. `PUPPETXR_WERROR` (default ON) makes them errors. Every change builds
clean with both GCC and clang. Only packagers on a newer compiler turn `PUPPETXR_WERROR` off.

Public C enums end in a `*_MAX_ENUM = 0x7fffffff` sentinel, so they are 32 bit and any `int`
a C caller passes is a valid value in C++ too; validation rejects it.

## Options

| Option | Default | Effect |
|---|---|---|
| `PUPPETXR_VALIDATION` | ON | internal `ASSERT`s and consistency checks |
| `PUPPETXR_WERROR` | ON | warnings are errors |
| `PUPPETXR_DIST` | OFF | the distributed build (`cmake/dist.cmake`), needs `CMAKE_BUILD_TYPE=Release` |
| `PUPPETXR_CLANG_TIDY` | OFF | run clang-tidy on the runtime and `puppetxrctl` while building, findings are errors |
| `PUPPETXR_CLANG_TIDY_ALL` | OFF | with `PUPPETXR_CLANG_TIDY`, also on the tests, apps and build tools |
| `PUPPETXR_SANITIZE` | empty | `address` (ASan and UBSan) or `thread` (TSan) on our targets, not third-party code |
| `PUPPETXR_HEADLESS_ONLY` | OFF | no SDL video, no ImGui, no mirror window |
| `PUPPETXR_FAST_MATH` | OFF | allow libm and FMA; breaks cross-platform determinism |
| `PUPPETXR_BUILD_TESTS` | ON | gtest binary |
| `PUPPETXR_BUILD_APPS` | ON | our sample apps in `apps/` |
| `PUPPETXR_BUILD_CTS` | OFF | build OpenXR-CTS and add the `cts_*` ctest entries (see testing.md) |
| `PUPPETXR_BUILD_SAMPLES` | OFF | fetch, patch and build third-party OpenXR samples (clang, EGL, GLESv2) and add the `samples` ctest entry (see testing.md) |
| `PUPPETXR_UNREAL` | empty | with `PUPPETXR_BUILD_SAMPLES` on Windows, the `UnrealEditor.exe` of Meta's Unreal fork (`oculus-5.8`) that builds and runs Meta's Unreal MRUK sample for the `samples_unreal` ctest entry (see testing.md) |
| `PUPPETXR_UNITY` | empty | with `PUPPETXR_BUILD_SAMPLES` on Windows, the `Unity.exe` (6000.0.66f2, licensed) that builds Meta's Unity MRUK samples for the `samples_unity` ctest entry (see testing.md) |

`PUPPETXR_HEADLESS_ONLY` builds SDL as a console build (`SDL_UNIX_CONSOLE_BUILD`), so it needs no
X11 or Wayland headers. Code using `ui.h` sits under `#if !PUPPETXR_HEADLESS_ONLY`. The `gcc-headless` preset
builds it; worth running for changes to `ui`, `oxr` or the CMake files.

## Distribution

`cmake --preset dist && cmake --build --preset dist && ctest --preset dist`, then
`cmake --install build/dist --prefix <dir>` gives the tree we ship. `cmake/dist.cmake` sets:

- Release (`-O3`), full link-time optimization (`-flto=full`; ThinLTO renames symbols per build).
- Hardening: `-fstack-protector-strong`, `_FORTIFY_SOURCE=3`, full RELRO, `BIND_NOW`,
  non-executable stack.
- Dead code removal: `-ffunction-sections -fdata-sections`, `--gc-sections`, `--as-needed`.
- Reproducible: `-ffile-prefix-map` for the source and build directories, SDL's revision
  pinned. The same source gives byte-identical binaries and debug files in any directory, so
  the preset uses clang: GCC's LTO hashes the unmapped working directory into symbol names.
- Stripped: `libpuppetxr.so` and `puppetxrctl` keep only their dynamic symbols (three exports for the
  library). The debug info goes to `bin/<file>.debug` in the build directory, linked by build
  id and debuglink and never installed; keep it per release to symbolize users' backtraces.
- Internal `ASSERT`s stay compiled in, but log instead of breaking (`os_default_assert_mode`),
  so a shipped build never stops the host app.
- Rooms are compiled into the library; more are found next to it
  (`<libdir>/../share/puppetxr/rooms`), never in the source tree, so an install tree can be moved
  anywhere (test `install_relocated` loads a compiled-in room with no rooms on disk and runs the
  moved `puppetxrctl --version`).

Tests and apps build with the same flags and run with `ctest --preset dist`; only the library,
`puppetxrctl`, the header and the manifest are installed.

- C++ runtime: `libstdc++` and `libgcc` are linked in statically (`-static-libstdc++
  -static-libgcc`), hidden by the version script, so the shipped binaries need glibc alone and
  never clash with the app's C++ runtime. Test `shipped_binaries_need_only_glibc`
  (`cmake/check_glibc.cmake`) fails on any other `NEEDED` library and, with
  `PUPPETXR_MAX_GLIBC`, on any glibc symbol version newer than it; `glibc_check_rejects_newer_glibc`
  proves the check catches one, `glibc_check_messages_survive_wrapping` that its message still
  matches wherever CMake wraps it (the line end depends on the checkout's path).

The binaries need the glibc of the build machine or newer, so releases are built on an old one.

## Linux releases

`tools/dist/dist.sh` builds the release in the container of `tools/dist/Dockerfile` and leaves
in `build/dist-linux/out/`:

- `puppetxr-<version>-linux-<arch>.tar.gz`: the install tree (library with its rooms compiled in,
  `puppetxrctl`, header, manifest, LICENSE)
- `...-debug.tar.gz`: the `.debug` files, kept per release to symbolize backtraces
- `.buildinfo`: commit, image layers (a hash of the image's layer digests), package and `glslc`
  versions, checksums

```
tools/dist/dist.sh              # build, all tests, package
tools/dist/dist.sh --no-tests   # build, portability check only, package
```

| Piece | Choice | Why |
|---|---|---|
| Base | AlmaLinux 8 pinned by digest | glibc 2.28: runs on Debian 10+, Ubuntu 18.10+, RHEL 8+, SteamOS; supported to 2029 |
| Compiler | clang (llvm-toolset) with gcc-toolset-15's libstdc++ (the toolset the clang package's `/etc/clang/*.cfg` names), lld | the `dist` preset as on any host; lld for full LTO |
| `glslc` | shaderc built from a pinned commit | not packaged for EL8; its version decides the embedded SPIR-V |
| glibc floor | `PUPPETXR_MAX_GLIBC=2.28` | the build fails its tests if anything needs newer |
| Tarball | `tar --sort=name --mtime=<commit time> --owner=0`, `gzip -n` | byte-identical for the same commit and image |

- The dnf packages follow AlmaLinux 8 updates; the `.buildinfo` records which were used.
  Rebuilding an old release byte for byte needs an image with the same layers (keep it); the
  `image layers` line identifies it. Not the image id: BuildKit's provenance attestation gives
  every build of the same layers a new id.
- The container runs as the calling user with the checkout at `/src`. `DOCKER=podman` switches
  the engine, `PUPPETXR_DIST_DOCKER_ARGS` adds `run` arguments (`--network host` behind a local
  proxy), `PUPPETXR_DIST_CMAKE_ARGS` adds configure arguments (`FETCHCONTENT_SOURCE_DIR_*`).
- `_FORTIFY_SOURCE=3` acts as level 2 with glibc 2.28 (level 3 needs 2.34).
- Tests run inside the container on its Mesa lavapipe and llvmpipe (`mesa-dri-drivers`);
  windowed tests skip without `DISPLAY`. EL8's Mesa enables lavapipe only with `RH_SW_VULKAN=1`
  (a Red Hat patch; without it `vkCreateInstance` of the driver fails silently), which the image
  sets.
- The build tree is configured `--fresh`: CMake caches `CFLAGS`/`LDFLAGS` of the first configure,
  so a tree of an older image would keep that image's flags.
- CMake 3.26 of EL8 predates `EXCLUDE_FROM_ALL` of `FetchContent_Declare` (3.28);
  `third_party/CMakeLists.txt` adds the OpenXR SDK by hand there, so its loader, headers and
  CMake package stay out of the install. `install_relocated` fails on any file the release does
  not ship.

## Source releases

`tools/dist/dist-src.sh` packages HEAD on the host (git, git-lfs, curl, tar, gzip) and leaves in
`build/dist-src/out/`:

- `puppetxr-<version>-src.tar.gz`: every tracked file, LFS files with their content; configuring
  downloads the dependencies as a checkout does
- `puppetxr-<version>-src-full.tar.gz`: the same plus every `FetchContent_Declare` of
  `third_party/CMakeLists.txt` in `third_party/vendor/<name>/`; configures offline (packagers)
- `.buildinfo`: commit, dependency URLs, checksums

| Piece | Choice | Why |
|---|---|---|
| Content | `git archive HEAD`, LFS pointers smudged from HEAD | uncommitted changes never ship; a file left as a pointer stops the script |
| Dependencies | URL and `URL_HASH` read from `third_party/CMakeLists.txt`, checked by `sha256sum` | one place pins them; cached in `build/dist-src/downloads` |
| Vendored use | `third_party/CMakeLists.txt` sets `FETCHCONTENT_SOURCE_DIR_<NAME>` for each `vendor/<name>` | an explicit `FETCHCONTENT_SOURCE_DIR_*` still wins |
| Tarball | `tar --sort=name --mtime=<commit time> --owner=0`, `gzip -n` | byte-identical for the same commit |

- A new dependency goes into the `foreach` list at the top of `third_party/CMakeLists.txt` too,
  or the full tarball ships it without using it.
- The CTS and the Meta samples (`PUPPETXR_BUILD_CTS`, `PUPPETXR_BUILD_SAMPLES`) are not vendored and
  still download.

## CMake versions

The minimum is 3.25 (presets version 6, `SYSTEM` in `FetchContent_Declare`); 3.25 builds and
passes the tests. `EXCLUDE_FROM_ALL` in `FetchContent_Declare` needs 3.28; older versions
ignore it and build a few unused third-party targets as part of `all`.

## Windows

Requirements: Visual Studio 2022 or 2026 (MSVC, or its clang, or LLVM's installer: plain `clang`, not
`clang-cl`, targeting the MSVC ABI) with the Windows 10 SDK, CMake and Ninja, `glslc` from the
Vulkan SDK. Configure from a Developer PowerShell or let Visual Studio or VS Code open the
presets; they use Ninja with the host's x64 toolchain (`"strategy": "external"`).

```
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug
set XR_RUNTIME_JSON=%CD%\build\msvc-debug\puppetxr-dev.json
```

- The library is `puppetxr.dll` (`PUPPETXR_LIBRARY_NAME` in `puppetxr.h`), exporting
  `xrNegotiateLoaderRuntimeInterface` and `puppetxr_*` by `__declspec(dllexport)`; everything else
  stays internal as with the version script on Linux.
- C runtime: the C++ runtime is linked in statically, the UCRT dynamically (`ucrtbase.dll`, part
  of Windows 10 and later): no redistributable to install, and the app's and puppetxr's stdin,
  stdout and environment are the same, which output capture needs. Sanitized builds use the DLL
  runtime throughout. gtest is forced onto the same runtime.
- Warnings: `/W4 /WX` plus the off-by-default warnings that match the GCC set (shadowing, signed
  conversion, non-virtual destructors, fallthrough), `/permissive- /Zc:preprocessor /utf-8`,
  `/fp:precise` without contraction (determinism). clang builds use the GCC and clang flags.
  MSVC prints `D9025 overriding '/EHs' with '/EHs-'` for our sources: CMake's default `/EHsc`
  stays for the third-party targets (the loader, gtest), ours turn exceptions off.
- Tests: `-fno-rtti` makes clang define `_HAS_STATIC_RTTI=0`, which hides the `std::any` gtest
  prints, so the clang test binary keeps RTTI; `cl /GR-` does not hide it.
- Shaders: D3D bytecode comes from `tools/hlsl_compile.cpp` (`puppetxr_hlsl`), a build tool calling
  `D3DCompile` of `d3dcompiler_47.dll`, which Windows 10 ships and `fxc` wraps; no `fxc` or
  `dxc` needs to be installed (`docs/shaders.md`).
- `dist` on Windows (`win-clang-dist`, `msvc-dist`): Release, full LTO (`-flto=full` with
  lld-link, or `/GL` + `/LTCG`), control flow guard, CET shadow stack compatibility, ASLR with
  high entropy, `/Brepro` for reproducible binaries, source paths trimmed, debug info in a PDB
  next to the binary in the build tree (`/PDBALTPATH:%_PDB%`), never installed. clang takes control
  flow guard as `-Xclang -cfguard` (`-mguard=cf` is the MinGW spelling). Only our code is LTO:
  lld-link (LLVM 22 and 23) never finishes the PDB when third-party code (SDL, ImGui, gtest) is
  full LTO bitcode with CodeView debug info, so `third_party/CMakeLists.txt` turns LTO off there.
  Test `shipped_binaries_are_hardened` (`cmake/check_hardening.cmake`, llvm-readobj) fails when
  guard, instrumentation, CET compatibility, ASLR or NX is missing; `hardening_check_rejects_missing_flags`
  proves the check works.
- Install: the DLL goes to `<prefix>/bin`, the manifest to `<prefix>/share/openxr/1/puppetxr.json`
  with a relative path, `install-runtime.ps1` to `<prefix>`. Rooms are compiled into the DLL
  (docs/world.md); `<prefix>/share/puppetxr/rooms` may add more.
- Active runtime: the loader reads `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` (a string
  with the manifest path; needs an administrator). `XR_RUNTIME_JSON` overrides it, except in
  elevated processes, where the loader ignores the environment.
- Oldest Windows: 10 version 1809. Our targets build with `_WIN32_WINNT=0x0A00` and
  `NTDDI_VERSION=0x0A000006` (`cmake/flags.cmake`), so the SDK headers hide newer API.

## macOS

Requirements: macOS 11 or later, Xcode's command line tools (Apple clang), and from Homebrew:

```
brew install cmake ninja shaderc spirv-cross vulkan-loader vulkan-headers molten-vk vulkan-validationlayers git-lfs
cmake --preset mac-debug && cmake --build --preset mac-debug && ctest --preset mac-debug
```

- The library is `libpuppetxr.dylib` with install name `@rpath/libpuppetxr.dylib`; only
  `xrNegotiateLoaderRuntimeInterface` and `puppetxr_*` are exported (`cmake/puppetxr.exports`), not the
  SDL linked into it. SDL's Objective-C classes are renamed with an `PUPPETXR_` prefix
  (`third_party/CMakeLists.txt`), since the Objective-C runtime keeps one class per name for the
  whole process and an app may bring its own SDL3. Installed, it goes to `<prefix>/lib` and the
  manifest names it relative to itself.
- Vulkan runs on MoltenVK. The loader lists it only to an instance created with
  `VK_KHR_portability_enumeration`; `xrCreateVulkanInstanceKHR` and `xrCreateVulkanDeviceKHR` add
  that extension and `VK_KHR_portability_subset` where offered, as the spec requires.
- The app's Vulkan loader is used, found among the loaded images by file or install name
  (`os_vulkan_loader`, `os_library_path`), even when the app bundle carries it off dyld's search
  path; SDL's window gets it through `SDL_HINT_VULKAN_LIBRARY`.
- Metal apps (Unity's macOS players and editor) bind `XR_KHR_metal_enable`; the backend is
  `src/render_metal.mm` (`PUPPETXR_HAVE_METAL`, linked with the Metal, QuartzCore and Foundation
  frameworks). Its shaders are MSL that `spirv-cross` (Homebrew) translates from the Vulkan
  SPIR-V at build time and Metal compiles at runtime, so Xcode's `metal` compiler is not needed
  (`docs/shaders.md`).
- No OpenGL: macOS has no GLX, EGL or WGL to share the app's context through, so `PUPPETXR_HAVE_GL`
  is off, `r_backend_available(OpenGL)` is false and the GL binding extensions are not offered.
- dyld does not search Homebrew's `lib`, and Homebrew's validation layer manifest names its
  library without a directory. The tests run through ctest, which sets
  `DYLD_FALLBACK_LIBRARY_PATH` to the Vulkan loader's directory and `VK_LAYER_PATH` to a copy of
  the manifest with the full path (`PUPPETXR_TEST_ENVIRONMENT` in `CMakeLists.txt`). Run directly,
  the Vulkan tests skip. Apps run from a shell need
  `DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib` unless they ship their own loader.
- Windows are high pixel density: `os_window_size` is in pixels, ImGui works in points and its
  clip rects are scaled by `FramebufferScale` (test `UiTest.panels_fill_windows_of_high_pixel_density`,
  which skips on a display of one pixel per point).
- Windows and their events belong to the app's main thread, the window thread on macOS
  (`os_is_window_thread`). Calls from other threads queue for it (`os_run_on_window_thread`,
  `os_post_to_window_thread`) and wake it through `dispatch_async_f` on the main queue; it also
  serves them whenever it waits for an os mutex (sleeping on the queue's semaphore, which every
  unlock signals, after a spin of `OS_MUTEX_SPIN_NS`) or pumps events. A call it does not start
  within `OS_MAIN_THREAD_TIMEOUT_MS` (250 ms) is withdrawn without running and logs one warning;
  later calls fail at once until the main thread comes by again. Video starts on the main thread
  with a 2 s timeout, else the runtime runs headless with a warning. Inside NSApp's running event
  loop the app dispatches the events, so the pump only takes SDL's queue (`app_runs_event_loop`).
- Keys and mouse in apps that own NSApp: SDL's Cocoa backend takes key events, and mouse moves in
  relative mode, only in its own pump or inside its own NSApp class, and Unity's editor and Godot
  dispatch events from their own loop with an NSApp of their own class. A local NSEvent monitor
  (`add_input_monitor`, which fires inside `-[NSApplication sendEvent:]`) hands the key and move
  events of our windows to SDL's handlers (`Cocoa_HandleKeyEvent`, `Cocoa_HandleMouseEvent`,
  through `SDL_GetVideoDevice`), except the one SDL's own pump just handled: an SDL event watch
  (`note_sdl_input_event`) notes SDL's last key (its raw code is the NSEvent's `keyCode`, with the
  ISO swap of the keys left of 1 and Z) and relative move. Modifier changes go through either way;
  SDL compares them with its own state. Tests: `PlatformAppEventsTest.*` (NSApp swapped to a
  plain `NSApplication`, real `NSEvent`s dispatched like the app or like SDL's pump).
- Off the main thread, `os_poll_queued_events` takes what SDL's queue holds without pumping
  (`SDL_PeepEvents` is thread-safe); text and drop events and everything after them stay for the
  main thread, as SDL frees their text when the thread that took them pumps.
- Active runtime: the macOS loader reads only `/usr/local/share/openxr/1/active_runtime.json`
  (`PlatformGetGlobalRuntimeFileName` in the OpenXR SDK); there is no per-user file.
  `XR_RUNTIME_JSON` overrides it.

## macOS releases

`tools/dist/dist-macos.sh [--no-tests]` configures `mac-dist` with `--fresh`, builds it, runs the
tests (with `--no-tests` only the release checks below) and leaves in `build/mac-dist/out/`:
`puppetxr-<version>-macos-universal.tar.gz` (`lib/libpuppetxr.dylib`, `bin/puppetxrctl`, the header, the
manifest, LICENSE), `…-debug.tar.gz` (the `.dSYM` bundles) and `.buildinfo` (commit, toolchain
versions including `spirv-cross`, sha256 sums).

- Universal arm64 and x86_64, full LTO, `-dead_strip`, stack protector, `_FORTIFY_SOURCE`, paths
  mapped as on Linux; local symbols stripped, debug info in `.dSYM` bundles (`cmake/dist.cmake`).
  With LTO the linker compiles the program into one temporary object, which the debug map names
  and the linker then deletes, so each architecture's object is kept with
  `-Xarch_<arch> -Wl,-object_path_lto` (test `dsym_holds_the_debug_info_<arch>`).
- `shipped_binaries_need_only_system_libraries` (`cmake/check_dylibs.cmake`): `otool -L` may list
  only `/usr/lib` and `/System`; `dylib_check_rejects_other_libraries` proves the check works.
- Reproducible: `ZERO_AR_DATE=1`, tarball entries sorted, one modification time (the commit's),
  owner 0, no Apple metadata (`COPYFILE_DISABLE`, `--no-mac-metadata`), `gzip -n`. Signing is ad hoc
  (`codesign -s -`).

Installing on another Mac: unpack (e.g. to `~/.local/opt/puppetxr`), `xattr -dr com.apple.quarantine`
on the folder, then `XR_RUNTIME_JSON=<folder>/share/openxr/1/puppetxr.json` per app, or a symlink
`/usr/local/share/openxr/1/active_runtime.json` to it for every app (the loader resolves the
symlink, so the manifest's relative `library_path` works). Metal apps need nothing more; Vulkan
apps need `brew install vulkan-loader molten-vk` unless they ship their own (Godot does).

## Windows releases

`tools/dist/dist.ps1` (PowerShell 7.4 or newer) builds the release on a Windows machine with
Visual Studio 2022 or its Build Tools (C++ workload, Windows SDK), LLVM (`clang`, `lld-link`,
`llvm-objdump`; the VS clang component or the LLVM installer), CMake, Ninja, `glslc` (Vulkan
SDK) and git on `PATH`. It enters the x64 developer environment itself, builds
`win-clang-dist` into `build/dist-windows`, runs the tests and leaves in
`build/dist-windows/out/`:

- `puppetxr-<version>-windows-x64.zip`: DLL (rooms compiled in), `puppetxrctl.exe`, header, manifest, LICENSE,
  `install-runtime.ps1`
- `...-pdb.zip`: `puppetxr.pdb` and `puppetxrctl.pdb`, kept per release to symbolize crash dumps
- `.buildinfo`: commit, toolchain versions, checksums

```
tools\dist\dist.ps1 -Pin       # once, and after each toolchain update: commit windows-toolchain.json
tools\dist\dist.ps1            # build, all tests, package
tools\dist\dist.ps1 -NoTests   # build, DLL check only, package
```

| Piece | Choice | Why |
|---|---|---|
| Build host | any Windows machine, toolchain pinned by checking | `tools/dist/windows-toolchain.json` names clang, MSVC toolset, SDK, `glslc` and PowerShell; any other version stops the build; the developer shell is entered with the pinned toolset and SDK (`-vcvars_ver`, `-winsdk`) when several are installed |
| Compiler | clang targeting the MSVC ABI, lld-link | the same compiler and flags as the Linux release; MSVC's reproducibility rests on the undocumented `/d1trimfile` |
| Target | x64, Windows 10 1809+ | UCRT and D3D12 ship with Windows; ARM64 is a TODO |
| C runtime | vcruntime static, UCRT from Windows | no redistributable; test `shipped_binaries_need_only_windows_dlls` (`cmake/check_dlls.cmake`) fails on an import outside Windows' own DLLs, `dll_check_rejects_other_dlls` proves the check works |
| Zip | entries sorted ordinally, one time (commit time), no directory entries | byte-identical for the same commit and toolchain; the deflate output depends on the .NET of PowerShell, hence its pin |

Users unpack the zip anywhere and run `powershell -ExecutionPolicy Bypass -File
install-runtime.ps1` from an administrator prompt: it makes the unpacked puppetxr the active
runtime and remembers the previous one; `-Uninstall` gives the role back. Only 64-bit apps see
it (32-bit apps read `WOW6432Node`, and there is no 32-bit build).

## Releasing

A pushed tag `vX.Y.Z` builds all releases on GitHub Actions and drafts a GitHub release;
publishing the draft is done by hand after checking it.

```
tools/release.sh 0.2.0     # checks, bumps the version, stamps CHANGELOG.md, commits, tags, pushes
```

`tools/release.sh` (Linux) refuses unless on `main`, clean, equal to `origin/main`, the tag is
new, the version is not older than `project(puppetxr VERSION ...)` and `## Unreleased` in
`CHANGELOG.md` has entries. It shows the commits since the last tag and the notes, asks, then
sets the CMake version, renames the section to `## X.Y.Z - <date>` under a new empty
Unreleased, commits `release X.Y.Z`, tags `vX.Y.Z` (annotated) and pushes both atomically.

`.github/workflows/release.yml`:

| Job | Runner | Does |
|---|---|---|
| `check` | ubuntu-24.04 | the tag equals `v` + the CMake version; the notes are the changelog section of the version (Unreleased for a manual run), which must not be empty |
| `linux` | ubuntu-24.04 | `tools/dist/dist.sh`, all tests in the container |
| `source` | ubuntu-24.04 | `tools/dist/dist-src.sh` |
| `windows` | windows-2022 | LLVM `LLVM_VERSION` from Chocolatey, `glslc` built from the shaderc tag and commit of `tools/dist/Dockerfile` (cached), `tools/dist/dist.ps1`, all tests |
| `release` | ubuntu-24.04 | tags only: `SHA256SUMS` over all files, draft release `puppetxr X.Y.Z` with every file; a re-run replaces the files of the existing draft |

- Dry run: run the workflow by hand (Actions, release, Run workflow) on any branch; it builds
  and tests everything and keeps the files as workflow artifacts, without a release.
- Windows pin: the hosted image decides MSVC, SDK and PowerShell. Run the workflow by hand with
  `pin` checked, download `windows-toolchain`, commit it as `tools/dist/windows-toolchain.json`.
  When the image drops the pinned MSVC or SDK the build stops; pin again. Releases are
  reproducible for the same commit and pin, not across pins.
- Users verify a download with `sha256sum -c SHA256SUMS --ignore-missing`.
- A failed release, still a draft: fix on `main`, then move the tag onto the fix
  (`git tag -fa vX.Y.Z -m "puppetxr X.Y.Z" && git push -f origin vX.Y.Z`); the new run replaces
  the draft's files. A published release is never moved; fix it with the next patch version.

## clang-tidy

`.clang-tidy` at the root configures every check; `tests/.clang-tidy` inherits it and only
allows out-of-range enum casts, which tests use on purpose. `cmake --build --preset clang-tidy`
runs it with every compile of the runtime (`src/`) and `puppetxrctl`, about a minute from scratch
on 16 cores. `-DPUPPETXR_CLANG_TIDY_ALL=ON` adds the
tests, apps and build tools: seven minutes, `tests/tests_oxr.cpp` alone takes as long as the
rest together. For a report without building,
`run-clang-tidy -p build/clang-debug "$PWD/(src|tests|tools|apps)/.*\.cpp$"`.

Enabled: the clang static analyzer, `bugprone-*`, `concurrency-*`, `misc-*` (const
correctness including pointees and parameters, internal linkage), `performance-*`,
`portability-*`, `cert-err33-c` (checked results), `cppcoreguidelines-init-variables` and
`-pro-type-member-init` (every local and field initialized), and the readability checks that
fit `docs/style.md` (braces, one declaration per line, matching parameter names, simplified
booleans). Off because they contradict the style guide: C casts, C arrays, macros, pointer
arithmetic, varargs, typedefs, `if (ptr)`, short names, uppercase literal suffixes, trailing
return types, magic numbers, enum sizes, `#pragma once`. Off for correctness: De Morgan
rewrites (`!(x > a && x < b)` and `x <= a || x >= b` differ for NaN).

Findings are fixed, not suppressed. A `NOLINT` is only for a false positive or a signature
someone else defines (the OpenXR spec, a callback type, `main`) and names the check and why.

## Installing

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
ninja -C build
sudo ninja -C build install
```

This installs `libpuppetxr.so` into `<prefix>/lib` and the manifest into
`<prefix>/share/openxr/1/puppetxr.json` (the manifest references the library by a relative
path, so the prefix can be relocated). Apps pick it with
`XR_RUNTIME_JSON=<prefix>/share/openxr/1/puppetxr.json`.

To make puppetxr the system default runtime, configure with
`-DPUPPETXR_INSTALL_ACTIVE_RUNTIME=ON`. On Linux this symlinks
`<sysconfdir>/xdg/openxr/1/active_runtime.json` (`/etc/xdg/openxr/1` for `/usr`,
override with `-DPUPPETXR_ACTIVE_RUNTIME_DIR=~/.config/openxr/1` for a per-user default)
to the manifest; on macOS `/usr/local/share/openxr/1/active_runtime.json`, the only one its
loader reads. It replaces whatever runtime was active, so it is off by default. On
Windows the active runtime is the registry value
`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`; set it to the manifest path by hand
or use `XR_RUNTIME_JSON`.

| Option | Default | Effect |
|---|---|---|
| `PUPPETXR_INSTALL_ACTIVE_RUNTIME` | OFF | install `active_runtime.json` pointing at puppetxr |
| `PUPPETXR_ACTIVE_RUNTIME_DIR` | sysconfdir/xdg/openxr/1; macOS /usr/local/share/openxr/1 | where that file goes |

## Dependencies

All fetched by `third_party/CMakeLists.txt` with `FetchContent`, pinned by tag and
hash, built static and linked into `libpuppetxr` with hidden visibility so only
`xrNegotiateLoaderRuntimeInterface` and `puppetxr_*` are exported.

| Dependency | Why |
|---|---|
| OpenXR-SDK | `openxr.h`, loader negotiation header; the loader for apps and tests |
| Vulkan-Headers, volk | Vulkan API without linking the Vulkan loader |
| SDL3 | window, events, time, memory, logging; built with `SDL_OPENGL` only for the X11 visual hint of GLX mirror windows, it never makes a GL context |
| Dear ImGui | configuration UI |
| ImPlot | the performance tab's frame time graph |
| jsmn | JSON tokenizer under `json_*` |
| stb_image_write | PNG screenshots |
| googletest | tests |

## Shaders

Shaders in `shaders/` are written once for every backend (`docs/shaders.md`). `glslc` compiles
them to SPIR-V at build time, embedded as `const u32[]` arrays by `cmake/embed.cmake`, so the
shipped library is a single file. Optional: `glslangValidator` (glslang) and `dxc`
(DirectXShaderCompiler) validate the GL, GLES and HLSL variants during the build.

Every render backend a platform can have is always built, with no option per backend, and loads
its API at runtime: Linux has Vulkan, OpenGL and OpenGL ES (`render_gl.cpp` and `oxr_gl.cpp`
declare the few GL, GLX, EGL and WGL types they need, so no GL, X11 or EGL development headers
are required); Windows adds D3D11 and D3D12 (`PUPPETXR_HAVE_D3D`), whose headers come with the
Windows SDK, and OpenGL through WGL. `d3d11.dll`, `d3d12.dll`, `dxgi.dll` and `opengl32.dll` are
loaded at runtime, never linked. Which backends apps may use is a runtime choice (`graphics` in
`docs/config.md`).
