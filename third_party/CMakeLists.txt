include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# lld-link (LLVM 22 and 23) never finishes the PDB of a link with third-party code as full LTO
# bitcode with CodeView debug info (the Windows dist build), so on Windows only our code is LTO
if (WIN32)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
endif ()

# The full source release ships the dependencies in vendor/ (tools/dist/dist-src.sh); an explicit
# FETCHCONTENT_SOURCE_DIR_* still wins
foreach (dep SDL3 jsmn googletest OpenXR VulkanHeaders volk imgui implot stb)
    string(TOUPPER ${dep} upper)
    if (IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/vendor/${dep} AND NOT FETCHCONTENT_SOURCE_DIR_${upper})
        set(FETCHCONTENT_SOURCE_DIR_${upper} ${CMAKE_CURRENT_SOURCE_DIR}/vendor/${dep})
    endif ()
endforeach ()

#
# SDL3, static, trimmed to window, events, input, time, memory and logging
#

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
# SDL_OPENGL only for SDL_HINT_VIDEO_X11_WINDOW_VISUALID: GLX mirror windows need the app's visual
set(SDL_OPENGL ON CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
if (PUPPETXR_HEADLESS_ONLY)
    set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
endif ()

# Pinned so SDL does not stamp the git revision of whatever repository it is built inside
set(SDL_REVISION "SDL-3.4.18" CACHE STRING "" FORCE)

FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz
    URL_HASH SHA256=c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
    SYSTEM
)

#
# jsmn, header only
#

FetchContent_Declare(jsmn
    URL https://github.com/zserge/jsmn/archive/refs/tags/v1.1.0.tar.gz
    URL_HASH SHA256=5f0913a10657fe7ec8d5794ccf00a01000e3e1f2f1e1f143c34a0f7b47edcb38
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
)

#
# googletest
#

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
    SYSTEM
)

#
# Declared now, made available by the steps that need them
#

FetchContent_Declare(OpenXR
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.65.tar.gz
    URL_HASH SHA256=27aa797baf680c5f836b6138e78e59e2f72d6becfef6d4587ed9a57756d6d3b3
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
    URL_HASH SHA256=4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_Declare(volk
    URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
    URL_HASH SHA256=1547d8d74395d4048fb3f4a6313da56db626e89b55db238d0d7e8944c8a645f3
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz
    URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
)
# 1.1 is the first release that builds against ImGui 1.93
FetchContent_Declare(implot
    URL https://github.com/epezent/implot/archive/refs/tags/v1.1.tar.gz
    URL_HASH SHA256=f13e1e111db2df1687e1568644429c2184eb5115867dc76bb0a8bd17a2e72ae8
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d.tar.gz
    URL_HASH SHA256=ae498e10fadeba0c2e7aa86c71cc9989c4d895201cd26b8b72061c76cfde4edf
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(SDL3 jsmn)
# The Objective-C runtime keeps one class per name for the whole process, whatever the export list
# says, and the app may bring its own SDL3: SDL's classes get puppetxr's prefix
if (APPLE)
    foreach (class SDL3Application SDL3AppDelegate SDL3TranslatorResponder SDL3MessageBoxPresenter SDL3OpenGLContext
                   SDL3Window SDL3View SDL3Cocoa_WindowListener SDL3_RumbleContext SDL3_RumbleMotor SDL3_cocoametalview
                   SDL_CocoaVideoData SDL_CocoaWindowData Cocoa_PasteboardDataProvider)
        target_compile_definitions(SDL3-static PRIVATE ${class}=PUPPETXR_${class})
    endforeach ()
endif ()
# Third-party warnings are not ours to fix
if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(SDL3-static PRIVATE -w)
elseif (MSVC)
    target_compile_options(SDL3-static PRIVATE /w)
endif ()
if (PUPPETXR_BUILD_TESTS)
    FetchContent_MakeAvailable(googletest)
    # gtest picks its own runtime for MSVC; ours must match (CMakeLists.txt)
    if (CMAKE_MSVC_RUNTIME_LIBRARY)
        set_target_properties(gtest gtest_main PROPERTIES MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
    endif ()
    # The MSVC STL annotates its containers under ASan, and the linker refuses objects that
    # disagree (/failifmismatch annotate_string), so gtest is built sanitized like the tests
    if (WIN32 AND PUPPETXR_SANITIZE STREQUAL "address")
        if (MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(gtest PRIVATE /fsanitize=address)
            target_compile_options(gtest_main PRIVATE /fsanitize=address)
        else ()
            target_compile_options(gtest PRIVATE -fsanitize=address)
            target_compile_options(gtest_main PRIVATE -fsanitize=address)
        endif ()
    endif ()
endif ()

add_library(jsmn INTERFACE)
target_include_directories(jsmn SYSTEM INTERFACE ${jsmn_SOURCE_DIR})

# The runtime only needs the headers; the loader target exists for apps and is built on demand.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
# EXCLUDE_FROM_ALL of FetchContent_Declare, which keeps the loader's install rules out of ours,
# needs CMake 3.28; older versions take the subdirectory by hand
if (CMAKE_VERSION VERSION_LESS 3.28)
    FetchContent_GetProperties(OpenXR)
    if (NOT openxr_POPULATED)
        FetchContent_Populate(OpenXR)
        add_subdirectory(${openxr_SOURCE_DIR} ${openxr_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif ()
else ()
    FetchContent_MakeAvailable(OpenXR)
endif ()
# The loader's -Werror=undef trips over mingw's headers
if (MINGW AND TARGET openxr_loader)
    target_compile_options(openxr_loader PRIVATE -Wno-undef)
endif ()
# The static loader picks the DLL runtime and, like gtest, must match ours and our sanitizer
if (MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND TARGET openxr_loader AND PUPPETXR_SANITIZE STREQUAL "address")
    get_target_property(loader_type openxr_loader TYPE)
    if (loader_type STREQUAL "STATIC_LIBRARY")
        set_target_properties(openxr_loader PROPERTIES MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
        target_compile_options(openxr_loader PRIVATE /fsanitize=address)
    endif ()
endif ()
# The API trace's printers are generated from the registry of the same pin (tools/gen_trace.py)
set(PUPPETXR_OPENXR_REGISTRY ${openxr_SOURCE_DIR}/specification/registry/xr.xml CACHE INTERNAL "")
add_library(openxr_headers INTERFACE)
target_include_directories(openxr_headers SYSTEM INTERFACE ${openxr_SOURCE_DIR}/include)
target_compile_definitions(openxr_headers INTERFACE XR_NO_PROTOTYPES)

#
# Vulkan headers and volk (loads the Vulkan loader at runtime, nothing to link)
#

set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(VulkanHeaders)

set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(volk)
target_link_libraries(volk PUBLIC Vulkan::Headers)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
set_target_properties(volk PROPERTIES C_VISIBILITY_PRESET hidden)

#
# Dear ImGui: core plus the SDL3 platform backend; rendering is our own (ui.cpp)
#

if (NOT PUPPETXR_HEADLESS_ONLY)
    FetchContent_MakeAvailable(imgui)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    )
    target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
    # The test engine hooks let ui.cpp record where items are, for tests that click them
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS IMGUI_ENABLE_TEST_ENGINE)
    set_target_properties(imgui PROPERTIES CXX_VISIBILITY_PRESET hidden)

    # ImPlot: the performance tab's frame time graph
    FetchContent_MakeAvailable(implot)
    add_library(implot STATIC ${implot_SOURCE_DIR}/implot.cpp ${implot_SOURCE_DIR}/implot_items.cpp)
    target_include_directories(implot SYSTEM PUBLIC ${implot_SOURCE_DIR})
    target_link_libraries(implot PUBLIC imgui)
    set_target_properties(implot PROPERTIES CXX_VISIBILITY_PRESET hidden)
endif ()

#
# stb_image_write for PNG screenshots, stb_image for room albedo atlases
#

FetchContent_MakeAvailable(stb)
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
