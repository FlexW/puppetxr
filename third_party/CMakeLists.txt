# The dependencies live in this directory, trimmed to what puppetxr builds; README.md lists their
# versions and local changes. Nothing is downloaded at configure time.

# lld-link (LLVM 22 and 23) never finishes the PDB of a link with third-party code as full LTO
# bitcode with CodeView debug info (the Windows dist build), so on Windows only our code is LTO
if (WIN32)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
endif ()

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

add_subdirectory(sdl EXCLUDE_FROM_ALL SYSTEM)
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

#
# googletest
#

if (PUPPETXR_BUILD_TESTS)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(googletest EXCLUDE_FROM_ALL SYSTEM)
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

#
# jsmn, header only
#

add_library(jsmn INTERFACE)
target_include_directories(jsmn SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/jsmn)

#
# OpenXR SDK: the runtime only needs the headers; the loader target exists for apps and is built on
# demand
#

add_subdirectory(openxr EXCLUDE_FROM_ALL)
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
set(PUPPETXR_OPENXR_REGISTRY ${CMAKE_CURRENT_SOURCE_DIR}/openxr/specification/registry/xr.xml CACHE INTERNAL "")
add_library(openxr_headers INTERFACE)
target_include_directories(openxr_headers SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/openxr/include)
target_compile_definitions(openxr_headers INTERFACE XR_NO_PROTOTYPES)

#
# Vulkan headers and volk (loads the Vulkan loader at runtime, nothing to link)
#

add_subdirectory(vulkan-headers EXCLUDE_FROM_ALL SYSTEM)

set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
add_subdirectory(volk EXCLUDE_FROM_ALL SYSTEM)
target_link_libraries(volk PUBLIC Vulkan::Headers)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
set_target_properties(volk PROPERTIES C_VISIBILITY_PRESET hidden)

#
# Dear ImGui: core plus the SDL3 platform backend; rendering is our own (ui.cpp)
#

if (NOT PUPPETXR_HEADLESS_ONLY)
    add_library(imgui STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_draw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_tables.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_widgets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends/imgui_impl_sdl3.cpp
    )
    target_include_directories(imgui SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/imgui ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends)
    target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
    # The test engine hooks let ui.cpp record where items are, for tests that click them
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS IMGUI_ENABLE_TEST_ENGINE)
    set_target_properties(imgui PROPERTIES CXX_VISIBILITY_PRESET hidden)

    # ImPlot: the performance tab's frame time graph
    add_library(implot STATIC ${CMAKE_CURRENT_SOURCE_DIR}/implot/implot.cpp ${CMAKE_CURRENT_SOURCE_DIR}/implot/implot_items.cpp)
    target_include_directories(implot SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/implot)
    target_link_libraries(implot PUBLIC imgui)
    set_target_properties(implot PROPERTIES CXX_VISIBILITY_PRESET hidden)
endif ()

#
# stb_image_write for PNG screenshots, stb_image for room albedo atlases
#

add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)
