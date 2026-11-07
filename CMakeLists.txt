cmake_minimum_required(VERSION 3.25)

project(puppetxr VERSION 0.1.0 LANGUAGES C CXX)
# The Metal backend is Objective-C++ (src/render_metal.mm)
if (APPLE)
    enable_language(OBJCXX)
endif ()

option(PUPPETXR_VALIDATION  "Internal asserts and consistency checks" ON)
option(PUPPETXR_HEADLESS_ONLY "No SDL video, no ImGui, no mirror window" OFF)
option(PUPPETXR_WERROR    "Treat warnings as errors" ON)
set(PUPPETXR_SANITIZE "" CACHE STRING "Sanitizers on our targets: address (ASan and UBSan) or thread (TSan)")
set_property(CACHE PUPPETXR_SANITIZE PROPERTY STRINGS "" address thread)
option(PUPPETXR_CLANG_TIDY  "Run clang-tidy (.clang-tidy) on the runtime and puppetxrctl while building; findings fail the build" OFF)
option(PUPPETXR_CLANG_TIDY_ALL "With PUPPETXR_CLANG_TIDY, also on the tests, apps and build tools" OFF)
option(PUPPETXR_DIST        "The distributed build: Release, LTO, hardened, reproducible, stripped (cmake/dist.cmake)" OFF)
set(PUPPETXR_MAX_GLIBC "" CACHE STRING "Newest glibc symbol version the shipped binaries may need, e.g. 2.28 (test shipped_binaries_need_only_glibc); empty: any")
option(PUPPETXR_FAST_MATH   "Allow libm and FMA; breaks cross-platform determinism" OFF)
option(PUPPETXR_BUILD_TESTS "Build the gtest binary" ON)
option(PUPPETXR_BUILD_APPS  "Build the sample apps in apps/" ON)
option(PUPPETXR_BUILD_CTS   "Fetch OpenXR-CTS and add a ctest entry" OFF)
option(PUPPETXR_BUILD_SAMPLES "Fetch, patch and build third-party OpenXR samples and add the samples ctest entry" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Windows: the C runtime is linked in except for the UCRT, a part of Windows 10 and later. The
# library then needs no redistributable and shares stdin, stdout and the environment with every
# app on the UCRT (os_capture_output_*). The debug UCRT comes with Visual Studio. Sanitized builds
# take the plain DLL runtime, the one the ASan runtime intercepts.
if (WIN32 AND (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    if (PUPPETXR_SANITIZE)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        foreach (lang C CXX)
            string(REPLACE "/RTC1" "" CMAKE_${lang}_FLAGS_DEBUG "${CMAKE_${lang}_FLAGS_DEBUG}")
            string(REPLACE "-D_DEBUG" "" CMAKE_${lang}_FLAGS_DEBUG "${CMAKE_${lang}_FLAGS_DEBUG}")
        endforeach ()
    else ()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        add_link_options("LINKER:/NODEFAULTLIB:libucrt$<$<CONFIG:Debug>:d>.lib")
        link_libraries("ucrt$<$<CONFIG:Debug>:d>")
    endif ()
    # Windows finds a DLL beside the executable; clang's ASan runtime is in its resource directory,
    # MSVC's on the PATH of its developer shell
    if (PUPPETXR_SANITIZE STREQUAL "address" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir OUTPUT_VARIABLE clang_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
        find_file(PUPPETXR_ASAN_RUNTIME clang_rt.asan_dynamic-x86_64.dll PATHS ${clang_resource_dir}/lib/windows ${clang_resource_dir}/lib/x86_64-pc-windows-msvc NO_DEFAULT_PATH REQUIRED)
        file(COPY ${PUPPETXR_ASAN_RUNTIME} DESTINATION ${CMAKE_BINARY_DIR}/bin)
    endif ()
endif ()

include(cmake/dist.cmake)
add_subdirectory(third_party)
include(cmake/flags.cmake)
include(cmake/shaders.cmake)

#
# Shaders, compiled to SPIR-V and embedded as C arrays so the library ships as one file
#

# Windows compiles the shaders to DXBC for D3D11 and D3D12 with a tool of its own (tools/hlsl_compile.cpp)
if (WIN32)
    add_executable(puppetxr_hlsl tools/hlsl_compile.cpp)
    target_compile_features(puppetxr_hlsl PRIVATE cxx_std_20)
    puppetxr_apply_flags(puppetxr_hlsl)
endif ()

puppetxr_add_shaders(puppetxr_shaders
    shaders/blit.vert
    shaders/blit.frag
    shaders/composite.vert
    shaders/composite.frag
    shaders/composite_depth.frag
    shaders/composite_resolve.frag
    shaders/imgui.vert
    shaders/imgui.frag
    shaders/passthrough.vert
    shaders/passthrough.frag
    shaders/environment_depth.vert
    shaders/environment_depth.frag
)

#
# Core object library, shared by the runtime and the tests
#

set(PUPPETXR_SOURCES
    src/platform.h
    src/platform.cpp
    src/base.h
    src/base.cpp
    src/zmath.h
    src/zmath.cpp
    src/sim.h
    src/sim.cpp
    src/world.h
    src/world.cpp
    src/oxr.h
    src/oxr.cpp
    src/render.h
    src/render_internal.h
    src/render.cpp
    src/render_vulkan.cpp
    src/render_gl.cpp
    src/comp.h
    src/comp.cpp
    src/rec.h
    src/rec.cpp
    src/oxr_internal.h
    src/oxr_vulkan.cpp
    src/oxr_gl.cpp
    src/oxr_d3d.cpp
    src/oxr_metal.cpp
    src/oxr_trace.cpp
    src/puppetxr.h
    src/puppetxr_api.cpp
)
if (NOT PUPPETXR_HEADLESS_ONLY)
    list(APPEND PUPPETXR_SOURCES src/ui.h src/ui.cpp)
endif ()
# D3D11 and D3D12 exist on Windows only; the rest of the runtime asks r_backend_available
if (WIN32)
    set(PUPPETXR_HAVE_D3D ON)
    list(APPEND PUPPETXR_SOURCES src/render_d3d11.cpp src/render_d3d12.cpp)
else ()
    set(PUPPETXR_HAVE_D3D OFF)
endif ()
# macOS has no GLX, EGL or WGL to share the app's context through; render_gl.cpp still builds
if (APPLE)
    set(PUPPETXR_HAVE_GL OFF)
else ()
    set(PUPPETXR_HAVE_GL ON)
endif ()
set(PUPPETXR_HAVE_METAL OFF)
if (APPLE)
    set(PUPPETXR_HAVE_METAL ON)
    list(APPEND PUPPETXR_SOURCES src/render_metal.mm)
endif ()

# The rooms of rooms/ are compiled in, so the library ships as one file (docs/world.md). The
# generated source is tens of megabytes of bytes, so it is a library of its own that clang-tidy skips.
file(GLOB puppetxr_room_files ${CMAKE_CURRENT_SOURCE_DIR}/rooms/*/room.json ${CMAKE_CURRENT_SOURCE_DIR}/rooms/*/room.bin ${CMAKE_CURRENT_SOURCE_DIR}/rooms/*/room_albedo.png)
set(puppetxr_rooms_source ${CMAKE_BINARY_DIR}/rooms/rooms_embedded.cpp)
add_custom_command(
    OUTPUT ${puppetxr_rooms_source}
    COMMAND ${CMAKE_COMMAND} -DROOMS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/rooms -DOUTPUT=${puppetxr_rooms_source} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_rooms.cmake
    DEPENDS ${puppetxr_room_files} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_rooms.cmake
    COMMENT "Embedding rooms"
    VERBATIM)
add_library(puppetxr_rooms STATIC ${puppetxr_rooms_source})
target_compile_features(puppetxr_rooms PRIVATE cxx_std_20)
target_include_directories(puppetxr_rooms PRIVATE src)
set_target_properties(puppetxr_rooms PROPERTIES POSITION_INDEPENDENT_CODE ON)
puppetxr_apply_flags(puppetxr_rooms)

# The OpenXR API trace prints every registry command and struct (docs/openxr.md, API trace)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(puppetxr_trace_header ${CMAKE_BINARY_DIR}/generated/oxr_trace_gen.h)
add_custom_command(
    OUTPUT ${puppetxr_trace_header}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_trace.py ${PUPPETXR_OPENXR_REGISTRY} ${puppetxr_trace_header}
    DEPENDS ${PUPPETXR_OPENXR_REGISTRY} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_trace.py
    COMMENT "Generating the OpenXR API trace"
    VERBATIM)

add_library(puppetxr_core OBJECT ${PUPPETXR_SOURCES} ${puppetxr_trace_header})
target_compile_features(puppetxr_core PUBLIC cxx_std_20)
target_include_directories(puppetxr_core PUBLIC src PRIVATE ${CMAKE_BINARY_DIR}/generated)
# Winsock for the control sockets (platform.cpp)
if (WIN32)
    set(PUPPETXR_PLATFORM_LIBS ws2_32 ole32)
elseif (APPLE)
    set(PUPPETXR_PLATFORM_LIBS "-framework Metal" "-framework QuartzCore" "-framework Foundation")
endif ()
target_link_libraries(puppetxr_core PUBLIC puppetxr_rooms SDL3::SDL3-static jsmn openxr_headers volk stb puppetxr_shaders ${PUPPETXR_PLATFORM_LIBS} ${CMAKE_DL_LIBS})
if (NOT PUPPETXR_HEADLESS_ONLY)
    target_link_libraries(puppetxr_core PUBLIC imgui implot)
endif ()
target_compile_definitions(puppetxr_core PUBLIC
    PUPPETXR_VALIDATION=$<BOOL:${PUPPETXR_VALIDATION}>
    PUPPETXR_HEADLESS_ONLY=$<BOOL:${PUPPETXR_HEADLESS_ONLY}>
    PUPPETXR_FAST_MATH=$<BOOL:${PUPPETXR_FAST_MATH}>
    PUPPETXR_DIST=$<BOOL:${PUPPETXR_DIST}>
    PUPPETXR_HAVE_D3D=$<BOOL:${PUPPETXR_HAVE_D3D}>
    PUPPETXR_HAVE_GL=$<BOOL:${PUPPETXR_HAVE_GL}>
    PUPPETXR_HAVE_METAL=$<BOOL:${PUPPETXR_HAVE_METAL}>
    PUPPETXR_BUILD_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PUPPETXR_BUILD_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PUPPETXR_BUILD_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Rooms are found next to the library when it is installed (wherever the tree was moved), in
# the source tree for development builds, else at the install prefix. The dist build never
# looks into the source tree it was built from.
include(GNUInstallDirs)
# Windows installs DLLs next to the executables
if (WIN32)
    set(PUPPETXR_INSTALL_FULL_MODULEDIR ${CMAKE_INSTALL_FULL_BINDIR})
else ()
    set(PUPPETXR_INSTALL_FULL_MODULEDIR ${CMAKE_INSTALL_FULL_LIBDIR})
endif ()
file(RELATIVE_PATH puppetxr_relative_rooms_dir ${PUPPETXR_INSTALL_FULL_MODULEDIR} ${CMAKE_INSTALL_FULL_DATADIR}/puppetxr/rooms)
if (PUPPETXR_DIST)
    set(puppetxr_source_rooms_dir "")
else ()
    set(puppetxr_source_rooms_dir "${CMAKE_CURRENT_SOURCE_DIR}/rooms")
endif ()
target_compile_definitions(puppetxr_core PRIVATE
    PUPPETXR_RELATIVE_ROOMS_DIR="${puppetxr_relative_rooms_dir}"
    PUPPETXR_SOURCE_ROOMS_DIR="${puppetxr_source_rooms_dir}"
    PUPPETXR_INSTALL_ROOMS_DIR="${CMAKE_INSTALL_FULL_DATADIR}/puppetxr/rooms"
)
puppetxr_apply_flags(puppetxr_core)

#
# The runtime
#

add_library(puppetxr SHARED)
target_link_libraries(puppetxr PRIVATE puppetxr_core)
# libpuppetxr.so, but puppetxr.dll as Windows names libraries (PUPPETXR_LIBRARY_NAME in puppetxr.h)
if (WIN32)
    set_target_properties(puppetxr PROPERTIES PREFIX "" OUTPUT_NAME "puppetxr")
else ()
    set_target_properties(puppetxr PROPERTIES PREFIX "lib" OUTPUT_NAME "puppetxr")
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE AND NOT WIN32)
    target_link_options(puppetxr PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/puppetxr.map)
    # The sanitizer runtimes are linked into the executable, so a sanitized library has undefined symbols
    if (NOT PUPPETXR_SANITIZE)
        target_link_options(puppetxr PRIVATE -Wl,--no-undefined)
    endif ()
endif ()
# Only the loader's entry point and the control API leave the dylib, not the SDL linked into it
if (APPLE)
    target_link_options(puppetxr PRIVATE -Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/cmake/puppetxr.exports)
    set_target_properties(puppetxr PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/puppetxr.exports INSTALL_NAME_DIR "@rpath")
endif ()

#
# puppetxrctl: drives running simulators over their control port. It only needs the platform and
# base layers, so it builds them itself instead of linking the whole runtime.
#

add_executable(puppetxrctl tools/puppetxrctl.cpp src/platform.cpp src/base.cpp)
target_compile_features(puppetxrctl PRIVATE cxx_std_20)
target_include_directories(puppetxrctl PRIVATE src)
target_link_libraries(puppetxrctl PRIVATE SDL3::SDL3-static jsmn ${PUPPETXR_PLATFORM_LIBS} ${CMAKE_DL_LIBS})
target_compile_definitions(puppetxrctl PRIVATE
    PUPPETXR_VALIDATION=$<BOOL:${PUPPETXR_VALIDATION}>
    PUPPETXR_HEADLESS_ONLY=$<BOOL:${PUPPETXR_HEADLESS_ONLY}>
    PUPPETXR_DIST=$<BOOL:${PUPPETXR_DIST}>
    PUPPETXR_BUILD_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PUPPETXR_BUILD_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PUPPETXR_BUILD_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
puppetxr_apply_flags(puppetxrctl)
if (PUPPETXR_DIST)
    puppetxr_split_debug_info(puppetxr)
    puppetxr_split_debug_info(puppetxrctl)
endif ()

include(cmake/manifest.cmake)
puppetxr_write_manifest(puppetxr ${CMAKE_BINARY_DIR}/puppetxr-dev.json)
include(cmake/install.cmake)

#
# Tests
#

# macOS's dyld searches neither Homebrew's lib nor a manifest's bare library name, so the tests
# get the Vulkan loader's directory and a validation layer manifest with the full library path
set(PUPPETXR_TEST_ENVIRONMENT "")
if (APPLE)
    find_library(PUPPETXR_VULKAN_LOADER NAMES vulkan.1 vulkan HINTS /opt/homebrew/lib /usr/local/lib)
    if (PUPPETXR_VULKAN_LOADER)
        get_filename_component(puppetxr_vulkan_dir ${PUPPETXR_VULKAN_LOADER} DIRECTORY)
        list(APPEND PUPPETXR_TEST_ENVIRONMENT "DYLD_FALLBACK_LIBRARY_PATH=${puppetxr_vulkan_dir}:/usr/local/lib:/usr/lib")
        set(puppetxr_layer_manifest ${puppetxr_vulkan_dir}/../share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json)
        if (EXISTS ${puppetxr_layer_manifest})
            file(READ ${puppetxr_layer_manifest} puppetxr_layer_json)
            string(REPLACE "\"library_path\": \"libVkLayer" "\"library_path\": \"${puppetxr_vulkan_dir}/libVkLayer" puppetxr_layer_json "${puppetxr_layer_json}")
            file(WRITE ${CMAKE_BINARY_DIR}/vulkan_layers/VkLayer_khronos_validation.json "${puppetxr_layer_json}")
            list(APPEND PUPPETXR_TEST_ENVIRONMENT "VK_LAYER_PATH=${CMAKE_BINARY_DIR}/vulkan_layers")
        endif ()
    else ()
        message(WARNING "No Vulkan loader found (brew install vulkan-loader molten-vk): the Vulkan tests skip")
    endif ()
endif ()

if (PUPPETXR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    # The shipped binaries run on any distribution with PUPPETXR_MAX_GLIBC (tools/dist/dist.sh);
    # the second test proves the check catches a binary that does not
    if (PUPPETXR_DIST AND NOT WIN32 AND NOT APPLE)
        set(puppetxr_shipped $<TARGET_FILE:puppetxr> $<TARGET_FILE:puppetxrctl>)
        add_test(NAME shipped_binaries_need_only_glibc
            COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${CMAKE_OBJDUMP} "-DFILES=${puppetxr_shipped}" -DMAX_GLIBC=${PUPPETXR_MAX_GLIBC} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_glibc.cmake)
        add_test(NAME glibc_check_rejects_newer_glibc
            COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${CMAKE_OBJDUMP} "-DFILES=${puppetxr_shipped}" -DMAX_GLIBC=2.0 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_glibc.cmake)
        set_tests_properties(glibc_check_rejects_newer_glibc PROPERTIES PASS_REGULAR_EXPRESSION "needs GLIBC_2[.0-9]+, newer than 2.0")
        # CMake wraps a long message between words, wherever the checkout's path length puts the
        # line end; the paths of growing length move it across the whole message
        set(puppetxr_padded_paths)
        foreach (padding RANGE 39)
            string(REPEAT "./" ${padding} puppetxr_padding)
            list(APPEND puppetxr_padded_paths "$<TARGET_FILE_DIR:puppetxr>/${puppetxr_padding}$<TARGET_FILE_NAME:puppetxr>")
        endforeach ()
        add_test(NAME glibc_check_messages_survive_wrapping
            COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${CMAKE_OBJDUMP} "-DFILES=${puppetxr_padded_paths}" -DMAX_GLIBC=2.0 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_glibc.cmake)
        set_tests_properties(glibc_check_messages_survive_wrapping PROPERTIES PASS_REGULAR_EXPRESSION "needs GLIBC_2[.0-9]+, newer than 2.0" FAIL_REGULAR_EXPRESSION "needs\n|GLIBC_2[.0-9]+,\n|newer\n|than\n")
    endif ()
    # On macOS they need nothing beyond the system (tools/dist/dist-macos.sh), and each
    # architecture's .dSYM holds the debug info of the code link-time optimization made
    if (PUPPETXR_DIST AND APPLE)
        set(puppetxr_shipped $<TARGET_FILE:puppetxr> $<TARGET_FILE:puppetxrctl>)
        add_test(NAME shipped_binaries_need_only_system_libraries
            COMMAND ${CMAKE_COMMAND} "-DFILES=${puppetxr_shipped}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_dylibs.cmake)
        add_test(NAME dylib_check_rejects_other_libraries
            COMMAND ${CMAKE_COMMAND} "-DFILES=${puppetxr_shipped}" -DALLOW=/usr/lib/ -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_dylibs.cmake)
        set_tests_properties(dylib_check_rejects_other_libraries PROPERTIES PASS_REGULAR_EXPRESSION "links[ \r\n]+/System/")
        foreach (arch ${CMAKE_OSX_ARCHITECTURES})
            add_test(NAME dsym_holds_the_debug_info_${arch}
                COMMAND dwarfdump --arch=${arch} --name=oxr_xrWaitFrame $<TARGET_FILE:puppetxr>.dSYM)
            set_tests_properties(dsym_holds_the_debug_info_${arch} PROPERTIES PASS_REGULAR_EXPRESSION "DW_TAG_subprogram")
        endforeach ()
    endif ()
    # On Windows they need no redistributable (tools/dist/dist.ps1); llvm-objdump comes with clang
    if (PUPPETXR_DIST AND WIN32)
        get_filename_component(puppetxr_compiler_dir ${CMAKE_CXX_COMPILER} DIRECTORY)
        find_program(PUPPETXR_LLVM_OBJDUMP llvm-objdump HINTS ${puppetxr_compiler_dir})
        if (PUPPETXR_LLVM_OBJDUMP)
            set(puppetxr_shipped $<TARGET_FILE:puppetxr> $<TARGET_FILE:puppetxrctl>)
            add_test(NAME shipped_binaries_need_only_windows_dlls
                COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${PUPPETXR_LLVM_OBJDUMP} "-DFILES=${puppetxr_shipped}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_dlls.cmake)
            add_test(NAME dll_check_rejects_other_dlls
                COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${PUPPETXR_LLVM_OBJDUMP} "-DFILES=${puppetxr_shipped}" -DDENY=kernel32.dll -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_dlls.cmake)
            set_tests_properties(dll_check_rejects_other_dlls PROPERTIES PASS_REGULAR_EXPRESSION "imports[ \r\n]+kernel32.dll")
        else ()
            message(WARNING "llvm-objdump not found: shipped_binaries_need_only_windows_dlls is not run")
        endif ()
        find_program(PUPPETXR_LLVM_READOBJ llvm-readobj HINTS ${puppetxr_compiler_dir})
        if (PUPPETXR_LLVM_READOBJ)
            set(puppetxr_shipped $<TARGET_FILE:puppetxr> $<TARGET_FILE:puppetxrctl>)
            add_test(NAME shipped_binaries_are_hardened
                COMMAND ${CMAKE_COMMAND} -DREADOBJ=${PUPPETXR_LLVM_READOBJ} "-DFILES=${puppetxr_shipped}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_hardening.cmake)
            add_test(NAME hardening_check_rejects_missing_flags
                COMMAND ${CMAKE_COMMAND} -DREADOBJ=${PUPPETXR_LLVM_READOBJ} "-DFILES=${puppetxr_shipped}" -DREQUIRE=NO_SUCH_FLAG -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_hardening.cmake)
            set_tests_properties(hardening_check_rejects_missing_flags PROPERTIES PASS_REGULAR_EXPRESSION "lacks[ \r\n]+NO_SUCH_FLAG")
        else ()
            message(WARNING "llvm-readobj not found: shipped_binaries_are_hardened is not run")
        endif ()
    endif ()
endif ()

if (PUPPETXR_BUILD_CTS)
    enable_testing()
    include(cmake/cts.cmake)
endif ()

if (PUPPETXR_BUILD_SAMPLES)
    enable_testing()
    include(cmake/samples.cmake)
endif ()

if (PUPPETXR_BUILD_APPS)
    add_subdirectory(apps)
endif ()
