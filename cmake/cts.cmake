# OpenXR-CTS as an external build: it brings its own loader and openxr targets, which
# would clash with ours in one CMake project. Each pass is a ctest entry; interactive
# tests run unattended where puppetxr can drive them.

include(ExternalProject)

# On Windows the CTS builds warning free with MSVC only; clang fails its -Werror
set(puppetxr_cts_compilers "")
if (WIN32)
    set(puppetxr_cts_compilers -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl)
endif ()

ExternalProject_Add(openxr_cts
    URL https://github.com/KhronosGroup/OpenXR-CTS/archive/refs/tags/openxr-cts-1.1.63.0.tar.gz
    URL_HASH SHA256=2745ed7cc94f13bccedf2ffbd3a8f3068b65429da2ef7b10c156002c67f9dc9f
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DBUILD_API_LAYERS=OFF -DBUILD_SDK_TESTS=OFF ${puppetxr_cts_compilers}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target conformance_cli
    INSTALL_COMMAND ""
    UPDATE_DISCONNECTED ON)

ExternalProject_Get_Property(openxr_cts BINARY_DIR)
set(puppetxr_cts_dir ${BINARY_DIR}/src/conformance/conformance_cli)

# The CLI loads brdf_lut.png and its conformance layer relative to its own directory
function(puppetxr_add_cts_test name filter)
    add_test(NAME ${name}
        COMMAND ${puppetxr_cts_dir}/conformance_cli ${filter} -H -T ${ARGN} -r junit -o ${CMAKE_BINARY_DIR}/${name}.xml
        WORKING_DIRECTORY ${puppetxr_cts_dir})
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 1800
        ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_BINARY_DIR}/puppetxr-dev.json;XR_API_LAYER_PATH=${puppetxr_cts_dir};PUPPETXR_HEADLESS=1;PUPPETXR_ROOM=none;PUPPETXR_LOG_FILE=${CMAKE_BINARY_DIR}/${name}.log")
endfunction()

puppetxr_add_cts_test(cts_vulkan2 "~[interactive]" -G Vulkan2)
puppetxr_add_cts_test(cts_vulkan "~[interactive]" -G Vulkan)

if (WIN32)
    # On Windows the OpenGL plugin draws on WGL into a window of its own
    puppetxr_add_cts_test(cts_opengl "~[interactive]" -G OpenGL)
    puppetxr_add_cts_test(cts_d3d11 "~[interactive]" -G D3D11)
    puppetxr_add_cts_test(cts_d3d12 "~[interactive]" -G D3D12)
else ()
    # The CTS OpenGL plugin draws into a GLX window of its own and needs an X server with GLX and
    # XFree86-VidMode (Xorg, Xwayland; not Xvfb); without DISPLAY the test is skipped
    add_test(NAME cts_opengl
        COMMAND sh -c "[ -n \"$DISPLAY\" ] || exit 77; exec ./conformance_cli \"~[interactive]\" -H -T -G OpenGL -r junit -o ${CMAKE_BINARY_DIR}/cts_opengl.xml < /dev/null"
        WORKING_DIRECTORY ${puppetxr_cts_dir})
    set_tests_properties(cts_opengl PROPERTIES
        TIMEOUT 1800
        SKIP_RETURN_CODE 77
        ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_BINARY_DIR}/puppetxr-dev.json;XR_API_LAYER_PATH=${puppetxr_cts_dir};PUPPETXR_HEADLESS=1;PUPPETXR_ROOM=none;PUPPETXR_LOG_FILE=${CMAKE_BINARY_DIR}/cts_opengl.log")
endif ()

# With XR_MND_headless enabled a session without a graphics binding is valid, which the
# graphics binding tests do not expect, so only the headless test runs with it
puppetxr_add_cts_test(cts_headless "[XR_MND_headless]" -G Vulkan2 -E XR_MND_headless)

# Interactive input tests run unattended: the CTS drives the simulated controllers through
# XR_EXT_conformance_automation. Composition and scenario tests still need a person.
puppetxr_add_cts_test(cts_automation_simple "[actions][interactive]~[no_auto]" -G Vulkan2 -E XR_EXT_conformance_automation)
puppetxr_add_cts_test(cts_automation_touch "[actions][interactive]~[no_auto]" -G Vulkan2 -E XR_EXT_conformance_automation --interactionProfiles=/interaction_profiles/oculus/touch_controller)
puppetxr_add_cts_test(cts_automation_touch_pro "[actions][interactive]~[no_auto]" -G Vulkan2 -E XR_EXT_conformance_automation -E XR_FB_touch_controller_pro --interactionProfiles=/interaction_profiles/facebook/touch_controller_pro)
puppetxr_add_cts_test(cts_automation_touch_plus "[actions][interactive]~[no_auto]" -G Vulkan2 -E XR_EXT_conformance_automation -E XR_META_touch_controller_plus --interactionProfiles=/interaction_profiles/meta/touch_controller_plus)

# Composition tests compare what puppetxr composited against approved references, so they need the
# reference renderer: lavapipe of the Mesa version the references were made with (docs/testing.md)
set(PUPPETXR_LAVAPIPE_ICD "" CACHE FILEPATH "lavapipe ICD manifest (lvp_icd*.json) for cts_composition; found in the system if empty")
set(PUPPETXR_CTS_TOLERANCE 0 CACHE STRING "largest channel difference cts_composition accepts per pixel")
set(puppetxr_lavapipe_icd ${PUPPETXR_LAVAPIPE_ICD})
if (NOT puppetxr_lavapipe_icd)
    file(GLOB puppetxr_lavapipe_icd /usr/share/vulkan/icd.d/lvp_icd*.json)
    if (puppetxr_lavapipe_icd)
        list(GET puppetxr_lavapipe_icd 0 puppetxr_lavapipe_icd)
    endif ()
endif ()
find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND AND puppetxr_lavapipe_icd)
    add_test(NAME cts_composition
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/cts_composition.py
            --cts ${puppetxr_cts_dir} --runtime ${CMAKE_BINARY_DIR}/puppetxr-dev.json --puppetxrctl $<TARGET_FILE:puppetxrctl>
            --icd ${puppetxr_lavapipe_icd} --references ${CMAKE_SOURCE_DIR}/tests/cts/composition
            --out ${CMAKE_BINARY_DIR}/cts_composition --tolerance ${PUPPETXR_CTS_TOLERANCE})
    set_tests_properties(cts_composition PROPERTIES TIMEOUT 1800)

    # The same scenes drawn by the CTS OpenGL plugin on llvmpipe against the Vulkan references: GL
    # and Vulkan compositing agree within 3. GradientFormatsLinearVsNonLinear is left out, its
    # labels name GL formats and the plugin writes linear values unencoded into sRGB swapchains
    # (it never enables GL_FRAMEBUFFER_SRGB). Skipped without DISPLAY, like cts_opengl.
    add_test(NAME cts_composition_gl
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/cts_composition.py
            --cts ${puppetxr_cts_dir} --runtime ${CMAKE_BINARY_DIR}/puppetxr-dev.json --puppetxrctl $<TARGET_FILE:puppetxrctl>
            --icd ${puppetxr_lavapipe_icd} --references ${CMAKE_SOURCE_DIR}/tests/cts/composition
            --out ${CMAKE_BINARY_DIR}/cts_composition_gl --graphics OpenGL --tolerance 3 --skip GradientFormatsLinearVsNonLinear)
    set_tests_properties(cts_composition_gl PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 77)

    # Scenario tests play scripted motions and button presses; the visual ones also compare screenshots
    add_test(NAME cts_scenarios
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/cts_scenarios.py
            --cts ${puppetxr_cts_dir} --runtime ${CMAKE_BINARY_DIR}/puppetxr-dev.json --puppetxrctl $<TARGET_FILE:puppetxrctl>
            --icd ${puppetxr_lavapipe_icd} --references ${CMAKE_SOURCE_DIR}/tests/cts/scenarios
            --out ${CMAKE_BINARY_DIR}/cts_scenarios --tolerance ${PUPPETXR_CTS_TOLERANCE})
    set_tests_properties(cts_scenarios PROPERTIES TIMEOUT 1800)
else ()
    message(STATUS "cts_composition and cts_scenarios need Python 3 and lavapipe (PUPPETXR_LAVAPIPE_ICD); skipped")
endif ()

