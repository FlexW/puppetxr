# Third-party code

The dependencies are part of the repository so a checkout or source tarball builds offline. Each
directory holds the subset of an upstream release that puppetxr builds, its license files as
upstream ships them, and, where upstream has one, its CMake build patched for this use.
`CMakeLists.txt` here adds them; nothing is downloaded at configure time.

## Versions

| Directory | Upstream | Version | Archive SHA-256 | License |
|---|---|---|---|---|
| `sdl` | https://github.com/libsdl-org/SDL | `release-3.4.18` | `c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90` | Zlib (`LICENSE.txt`); hidapi's in `src/hidapi/` |
| `jsmn` | https://github.com/zserge/jsmn | `v1.1.0` | `5f0913a10657fe7ec8d5794ccf00a01000e3e1f2f1e1f143c34a0f7b47edcb38` | MIT |
| `googletest` | https://github.com/google/googletest | `v1.18.0` | `6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5` | BSD-3-Clause |
| `openxr` | https://github.com/KhronosGroup/OpenXR-SDK | `release-1.1.65` | `27aa797baf680c5f836b6138e78e59e2f72d6becfef6d4587ed9a57756d6d3b3` | Apache-2.0 (`LICENSE`, `LICENSES/`); jsoncpp and sanitizers-cmake MIT in `src/external/` |
| `vulkan-headers` | https://github.com/KhronosGroup/Vulkan-Headers | `vulkan-sdk-1.4.363.0` | `4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e` | Apache-2.0 or MIT |
| `volk` | https://github.com/zeux/volk | `vulkan-sdk-1.4.363.0` | `1547d8d74395d4048fb3f4a6313da56db626e89b55db238d0d7e8944c8a645f3` | MIT |
| `imgui` | https://github.com/ocornut/imgui | `v1.93.0` | `f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee` | MIT |
| `implot` | https://github.com/epezent/implot | `v1.1` (first release building against ImGui 1.93) | `f13e1e111db2df1687e1568644429c2184eb5115867dc76bb0a8bd17a2e72ae8` | MIT |
| `stb` | https://github.com/nothings/stb | `a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d` | `ae498e10fadeba0c2e7aa86c71cc9989c4d895201cd26b8b72061c76cfde4edf` | MIT or public domain |

The archive is GitHub's `archive/refs/tags/<version>.tar.gz` (for stb `archive/<commit>.tar.gz`).
Files that are kept are byte-identical to it except the CMake files listed under Changes.

## Contents and changes

Removed everywhere: documentation, tests, samples, examples, CI and IDE project files, other
build systems.

- `sdl`: `CMakeLists.txt`, `cmake/` (no `cmake/test`, `cmake/android`), `include/`, `src/`,
  `wayland-protocols/`. Gone from `src/`: the backends of platforms puppetxr does not build for
  (Android, Emscripten, iOS, Haiku, the BSDs, consoles, RISC OS, QNX, GDK but `joystick/gdk`,
  the GameInput joystick driver of Windows builds, Raspberry Pi, Vivante);
  the backends of the subsystems `CMakeLists.txt` turns off, keeping their core and `dummy`
  backend (audio, camera, sensor, dialog but for the zenity message box, power, GPU; haptic keeps
  `darwin`, `windows` and `hidapi`, whose headers the joystick code includes);
  the render backends but `software`; hidapi but `hidapi/hidapi.h`, `SDL_hidapi*` and its
  licenses; `src/test`, `video/openvr`, `video/directx`; `SDL_test*.h` and the Android, iOS,
  Xbox and GDK build configs. `CMakeLists.txt`: the install, uninstall, tests and examples
  blocks removed. The branches for the removed platforms remain and are unreachable.
- `jsmn`: `jsmn.h`.
- `googletest`: `googletest/` with `include/`, `src/`, `cmake/internal_utils.cmake`; no
  googlemock. `CMakeLists.txt`: builds googletest only, no googlemock, Abseil or install options.
  `googletest/CMakeLists.txt`: package files, Abseil, install, samples and tests removed.
  `internal_utils.cmake`: the executable, test and install helpers removed.
- `openxr`: `include/`, `specification/registry/xr.xml` (for `tools/gen_trace.py` and
  `tools/gen_profiles.py`), the loader (`src/loader/`, `src/common/`, the pregenerated dispatch
  tables, the CMake modules it uses, jsoncpp's three sources, sanitizers-cmake). No code
  generation scripts: the headers and loader sources are pregenerated upstream.
  `CMakeLists.txt`: `BUILD_FORCE_GENERATION`, the uninstall target and the specification build
  removed. `src/loader/CMakeLists.txt`: the Android sources and Android packaging removed.
- `vulkan-headers`: the C headers of `include/vulkan/` and `include/vk_video/`; no Vulkan-Hpp,
  registry or tests. `CMakeLists.txt`: the C++ module, tests and install removed.
- `volk`: `volk.c`, `volk.h`. `CMakeLists.txt`: install removed.
- `imgui`: the core sources and `backends/imgui_impl_sdl3.*`; no demo.
- `implot`: `implot.cpp`, `implot_items.cpp` and headers; no demo.
- `stb`: `stb_image.h`, `stb_image_write.h`.

## Updating

1. Download the new release archive, record its SHA-256 and version in the table above.
2. Replace the kept files with the new ones, take over new files the build now needs, drop what
   the list above excludes, and redo the CMake changes listed under Changes.
3. Build and test every preset that the dependency affects (`docs/building.md`, `docs/testing.md`).
