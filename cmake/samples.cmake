# Third-party OpenXR samples as end-to-end tests (docs/testing.md, Sample apps). Each project is
# an ExternalProject pinned by commit, patched from tests/samples/<app>/ and built into
# build/samples/<app>; tools/sample_tests.py drives them through puppetxrctl.

include(ExternalProject)

find_package(Python3 COMPONENTS Interpreter)
if (NOT Python3_FOUND)
    message(STATUS "Sample tests need Python 3; skipped")
    return()
endif ()
set(PUPPETXR_SAMPLE_APPS "")

# Meta's samples are not GCC-clean (-Wchanges-meaning), so they always build with clang. On Linux
# they draw with OpenGL ES 3 on EGL (XR_MNDX_egl_enable), added by linux.patch. Their KTX
# requires CMake 2.8.12, which CMake 4 refuses without CMAKE_POLICY_VERSION_MINIMUM.
find_program(PUPPETXR_SAMPLES_CLANG clang)
find_program(PUPPETXR_SAMPLES_CLANGXX clang++)
find_library(PUPPETXR_SAMPLES_EGL EGL)
find_library(PUPPETXR_SAMPLES_GLES GLESv2)
if (PUPPETXR_SAMPLES_CLANG AND PUPPETXR_SAMPLES_CLANGXX AND PUPPETXR_SAMPLES_EGL AND PUPPETXR_SAMPLES_GLES)
    ExternalProject_Add(meta_openxr_sdk
        GIT_REPOSITORY https://github.com/meta-quest/Meta-OpenXR-SDK.git
        GIT_TAG bbed2f20e38a5df7113630771c83cb8279e4fc26 # v85
        GIT_SHALLOW OFF
        PATCH_COMMAND git apply ${CMAKE_SOURCE_DIR}/tests/samples/meta_openxr_sdk/linux.patch
        SOURCE_SUBDIR Samples
        BINARY_DIR ${CMAKE_BINARY_DIR}/samples/meta_openxr_sdk
        CMAKE_GENERATOR Ninja
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=${PUPPETXR_SAMPLES_CLANG} -DCMAKE_CXX_COMPILER=${PUPPETXR_SAMPLES_CLANGXX} -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        INSTALL_COMMAND ""
        UPDATE_DISCONNECTED ON)
    list(APPEND PUPPETXR_SAMPLE_APPS meta_openxr_sdk)
else ()
    message(STATUS "Meta's samples need clang, EGL and GLESv2; skipped")
endif ()

# Godot's official XR demos on the official build of Godot 4.7.2: the compatibility renderer over
# OpenGL and the mobile renderer over Vulkan. The patch makes each demo quit when its session stops.
# Projects are imported once here, so runs start at once. On Linux Godot draws in an Xvfb display
# the driver starts. On Windows it draws on llvmpipe (Mesa's opengl32.dll next to it, which puppetxr
# then loads too) and lavapipe (VK_DRIVER_FILES) of mesa-dist-win, the Mesa of the references.
find_program(PUPPETXR_SAMPLES_XVFB Xvfb)
set(PUPPETXR_GODOT "")
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND PUPPETXR_SAMPLES_XVFB)
    set(PUPPETXR_GODOT ${CMAKE_BINARY_DIR}/samples/godot/godot)
    ExternalProject_Add(godot
        URL https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_linux.x86_64.zip
        URL_HASH SHA256=cadd3204e728a35d3f13adb7fd0d7902636b79f6b95c40c265eb73b6c35329e4
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/Godot_v4.7.2-stable_linux.x86_64 ${PUPPETXR_GODOT})
elseif (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    set(PUPPETXR_GODOT ${CMAKE_BINARY_DIR}/samples/godot/godot.exe)
    ExternalProject_Add(mesa
        URL https://github.com/pal1000/mesa-dist-win/releases/download/26.2.4/mesa3d-26.2.4-release-msvc.7z
        URL_HASH SHA256=351fc8c8b695878ffb3eaa044b3ead08672a48b1a045e3c3e3975811df0f6695
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR>/x64 ${CMAKE_BINARY_DIR}/samples/mesa)
    ExternalProject_Add(godot
        DEPENDS mesa
        URL https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_win64.exe.zip
        URL_HASH SHA256=731980f9608d61333e5baf54a2ef17210acc7a538446c0cb9969f002aca1e953
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/Godot_v4.7.2-stable_win64.exe ${PUPPETXR_GODOT}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/samples/mesa/opengl32.dll ${CMAKE_BINARY_DIR}/samples/mesa/libgallium_wgl.dll ${CMAKE_BINARY_DIR}/samples/godot)
elseif (APPLE)
    # Godot's macOS build carries MoltenVK and draws with Vulkan on the GPU. The archive's one
    # folder, Godot.app, is the source directory
    set(PUPPETXR_GODOT ${CMAKE_BINARY_DIR}/samples/godot/Godot.app/Contents/MacOS/Godot)
    ExternalProject_Add(godot
        URL https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_macos.universal.zip
        URL_HASH SHA256=c58a24e31d720be9d62f60cb5627c4e695fb72f21b0cfe1bc9ccaa9a3b3ba63e
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR> ${CMAKE_BINARY_DIR}/samples/godot/Godot.app)
else ()
    message(STATUS "Godot's XR demos need Linux on x86_64 with Xvfb, Windows on x64 or macOS; skipped")
endif ()
if (PUPPETXR_GODOT)
    set(PUPPETXR_GODOT_DEMOS ${CMAKE_BINARY_DIR}/samples/godot_demos)
    set(PUPPETXR_GODOT_PROJECTS openxr_binding_modifier_demo openxr_character_centric_movement openxr_composition_layers openxr_hand_tracking_demo
        openxr_origin_centric_movement openxr_passthrough openxr_render_models openxr_spectator_view)
    set(PUPPETXR_GODOT_IMPORT "")
    foreach (project IN LISTS PUPPETXR_GODOT_PROJECTS)
        list(APPEND PUPPETXR_GODOT_IMPORT COMMAND ${PUPPETXR_GODOT} --headless --path ${PUPPETXR_GODOT_DEMOS}/xr/${project} --import)
    endforeach ()
    ExternalProject_Add(godot_demos
        DEPENDS godot
        GIT_REPOSITORY https://github.com/godotengine/godot-demo-projects.git
        GIT_TAG 6ad6167e0577fe3622c18546138f456b107ce93c # 4.7-6ad6167, the demos released with Godot 4.7
        GIT_SHALLOW OFF
        SOURCE_DIR ${PUPPETXR_GODOT_DEMOS}
        PATCH_COMMAND git apply ${CMAKE_SOURCE_DIR}/tests/samples/godot_demos/quit_on_stop.patch
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ${PUPPETXR_GODOT_IMPORT}
        BUILD_IN_SOURCE ON
        INSTALL_COMMAND ""
        UPDATE_DISCONNECTED ON)
    list(APPEND PUPPETXR_SAMPLE_APPS godot_demos)
endif ()

# The samples of the Godot OpenXR vendors plugin run on Linux only so far
if (PUPPETXR_GODOT AND NOT WIN32 AND NOT APPLE)
    # The samples of the Godot OpenXR vendors plugin, each given the released addon and the
    # repository's common scripts as its build does, then imported (import.cmake).
    set(PUPPETXR_GODOT_VENDORS ${CMAKE_BINARY_DIR}/samples/godot_vendors)
    set(PUPPETXR_GODOT_VENDORS_PROJECTS demo samples/body-tracking-sample samples/meta-composition-layers-sample samples/meta-hand-tracking-sample
        samples/meta-passthrough-sample samples/meta-scene-sample samples/meta-space-warp-sample samples/performance-metrics-sample)
    ExternalProject_Add(godot_vendors_addon
        URL https://github.com/GodotVR/godot_openxr_vendors/releases/download/5.1.0-stable/godotopenxrvendorsaddon.zip
        URL_HASH SHA256=6a838dbdf4115549e4511ebee0da9a5dcc8f9f6258d4cc2f2ee57a907a3e2911
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND "")
    ExternalProject_Get_Property(godot_vendors_addon SOURCE_DIR)
    set(PUPPETXR_GODOT_VENDORS_ADDON ${SOURCE_DIR}/addons/godotopenxrvendors)
    set(PUPPETXR_GODOT_VENDORS_SETUP "")
    foreach (project IN LISTS PUPPETXR_GODOT_VENDORS_PROJECTS)
        set(dir ${PUPPETXR_GODOT_VENDORS}/${project})
        list(APPEND PUPPETXR_GODOT_VENDORS_SETUP
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${PUPPETXR_GODOT_VENDORS_ADDON} ${dir}/addons/godotopenxrvendors
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${PUPPETXR_GODOT_VENDORS}/demo/addons/common ${dir}/addons/common
            COMMAND ${CMAKE_COMMAND} -DGODOT=${PUPPETXR_GODOT} -DPROJECT=${dir} -P ${CMAKE_SOURCE_DIR}/tests/samples/godot_vendors/import.cmake)
    endforeach ()
    ExternalProject_Add(godot_vendors
        DEPENDS godot godot_vendors_addon
        GIT_REPOSITORY https://github.com/GodotVR/godot_openxr_vendors.git
        GIT_TAG 7afcd397e6206d9751b82100559ff39a93a8f59e # 5.1.0-stable
        GIT_SHALLOW OFF
        GIT_SUBMODULES ""
        SOURCE_DIR ${PUPPETXR_GODOT_VENDORS}
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ${PUPPETXR_GODOT_VENDORS_SETUP}
        BUILD_IN_SOURCE ON
        INSTALL_COMMAND ""
        UPDATE_DISCONNECTED ON)
    list(APPEND PUPPETXR_SAMPLE_APPS godot_vendors)
endif ()

# Core apps on the OpenXR loader of OpenXR-SDK-Source 1.1.63, installed to openxr_sdk_prefix:
# its hello_xr, which needs Xxf86vm and Xrandr even when it draws with Vulkan, and Monado's
# openxr-simple-example (SDL2) and xrgears (meson, glm, xxd, glslangValidator)
find_library(PUPPETXR_SAMPLES_XXF86VM Xxf86vm)
find_library(PUPPETXR_SAMPLES_XRANDR Xrandr)
# On macOS hello_xr draws with Vulkan through MoltenVK in a window of its own
if ((CMAKE_SYSTEM_NAME STREQUAL "Linux" AND PUPPETXR_SAMPLES_XVFB AND PUPPETXR_SAMPLES_XXF86VM AND PUPPETXR_SAMPLES_XRANDR) OR APPLE)
    set(PUPPETXR_OPENXR_PREFIX ${CMAKE_BINARY_DIR}/samples/openxr_sdk_prefix)
    # hello_xr's XR_KHR_vulkan_enable path makes its own instance, which must ask for MoltenVK
    set(puppetxr_openxr_sdk_patch "")
    if (APPLE)
        set(puppetxr_openxr_sdk_patch git checkout -- . COMMAND git apply ${CMAKE_SOURCE_DIR}/tests/samples/openxr_sdk/macos.patch)
    endif ()
    ExternalProject_Add(openxr_sdk
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK-Source.git
        GIT_TAG 2b99fec95e9cdf352c1a98e9cb23bf4def1cf8e6 # release-1.1.63
        GIT_SHALLOW OFF
        PATCH_COMMAND ${puppetxr_openxr_sdk_patch}
        BINARY_DIR ${CMAKE_BINARY_DIR}/samples/openxr_sdk
        CMAKE_GENERATOR Ninja
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${PUPPETXR_OPENXR_PREFIX} -DBUILD_API_LAYERS=OFF -DBUILD_TESTS=ON
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target hello_xr openxr_loader
        UPDATE_DISCONNECTED ON)
    list(APPEND PUPPETXR_SAMPLE_APPS openxr_sdk)

    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(PUPPETXR_SAMPLES_SDL2 QUIET sdl2)
    endif ()
    if (PUPPETXR_SAMPLES_SDL2_FOUND)
        ExternalProject_Add(openxr_simple_example
            DEPENDS openxr_sdk
            GIT_REPOSITORY https://gitlab.freedesktop.org/monado/demos/openxr-simple-example.git
            GIT_TAG bf920800ff156424e2710e3ca63f84be121e62cb
            GIT_SHALLOW OFF
            GIT_SUBMODULES ""
            BINARY_DIR ${CMAKE_BINARY_DIR}/samples/openxr_simple_example
            CMAKE_GENERATOR Ninja
            CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=${PUPPETXR_OPENXR_PREFIX}
            INSTALL_COMMAND ""
            UPDATE_DISCONNECTED ON)
        list(APPEND PUPPETXR_SAMPLE_APPS openxr_simple_example)
    else ()
        message(STATUS "openxr-simple-example needs SDL2; skipped")
    endif ()

    find_program(PUPPETXR_SAMPLES_MESON meson)
    find_program(PUPPETXR_SAMPLES_XXD xxd)
    find_program(PUPPETXR_SAMPLES_GLSLANG glslangValidator)
    if (PUPPETXR_SAMPLES_MESON AND PUPPETXR_SAMPLES_XXD AND PUPPETXR_SAMPLES_GLSLANG)
        set(PUPPETXR_XRGEARS ${CMAKE_BINARY_DIR}/samples/xrgears)
        ExternalProject_Add(xrgears
            DEPENDS openxr_sdk
            GIT_REPOSITORY https://gitlab.freedesktop.org/monado/demos/xrgears.git
            GIT_TAG 51ef6c779c8d3134d8df9eca1294779e61c8243f
            GIT_SHALLOW OFF
            GIT_SUBMODULES ""
            PATCH_COMMAND git apply ${CMAKE_SOURCE_DIR}/tests/samples/xrgears/sample_tests.patch
            CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env PKG_CONFIG_PATH=${PUPPETXR_OPENXR_PREFIX}/lib/pkgconfig
                ${PUPPETXR_SAMPLES_MESON} setup --reconfigure --buildtype=release ${PUPPETXR_XRGEARS} <SOURCE_DIR>
            BUILD_COMMAND ${PUPPETXR_SAMPLES_MESON} compile -C ${PUPPETXR_XRGEARS}
            INSTALL_COMMAND ""
            UPDATE_DISCONNECTED ON)
        list(APPEND PUPPETXR_SAMPLE_APPS xrgears)
    else ()
        message(STATUS "xrgears needs meson, xxd and glslangValidator; skipped")
    endif ()
else ()
    message(STATUS "hello_xr and Monado's demos need Linux with Xvfb, Xxf86vm and Xrandr, or macOS; skipped")
endif ()

# Meta's Unity samples, built into Windows players by the editor named by PUPPETXR_UNITY (the
# projects' 6000.0.66f2, which needs a Unity license) with tests/samples/unity/build.cmake. The
# first build imports a project for a long while; later ones reuse its Library in the build tree.
# A sample built with LOADER oculus runs on Unity's Oculus XR plugin instead of OpenXR; PROJECT
# names the project's folder in the repository, UNITY another editor for a project of another
# version (skipped when empty), PACKAGE_SAMPLES package samples to import and build in
# (`<package>/<folder under Samples~>`); tests/samples/<name>/sample_tests.patch (without an
# _oculus suffix) is applied to the checkout.
set(PUPPETXR_UNITY "" CACHE FILEPATH "Unity editor (Unity.exe, on macOS Unity.app/Contents/MacOS/Unity) that builds the Unity sample players; empty skips them")
set(PUPPETXR_UNITY_6000_3 "" CACHE FILEPATH "Unity 6000.3 editor for the samples made with it (Depth API); empty skips them")
set(PUPPETXR_UNITY_APPS "")
function (puppetxr_unity_sample name repository commit player loader)
    cmake_parse_arguments(PARSE_ARGV 5 sample "" "PROJECT;UNITY" "PACKAGE_SAMPLES;EMBED")
    set(unity ${PUPPETXR_UNITY})
    if ("UNITY" IN_LIST ARGN)
        set(unity ${sample_UNITY})
    endif ()
    if (NOT unity)
        message(STATUS "${name} needs another Unity editor; skipped")
        return()
    endif ()
    set(dir ${CMAKE_BINARY_DIR}/samples/${name})
    set(project <SOURCE_DIR>)
    if (sample_PROJECT)
        set(project <SOURCE_DIR>/${sample_PROJECT})
    endif ()
    # A sample's players on either loader share its patch
    string(REGEX REPLACE "_oculus$" "" patch_name ${name})
    set(patch ${CMAKE_SOURCE_DIR}/tests/samples/${patch_name}/sample_tests.patch)
    list(JOIN sample_PACKAGE_SAMPLES "," package_samples)
    set(patch_command "")
    if (EXISTS ${patch})
        set(patch_command git apply --ignore-whitespace ${patch})
    endif ()
    # EMBED <package> <repository> <commit>: a package cloned into the project's Packages folder,
    # which Unity uses as it is; Unity's own git fails on macOS (its x86_64 helper finds no xcrun)
    if (sample_EMBED)
        list(GET sample_EMBED 0 embed_name)
        list(GET sample_EMBED 1 embed_repository)
        list(GET sample_EMBED 2 embed_commit)
        set(embed_dir <SOURCE_DIR>/Packages/${embed_name})
        if (NOT patch_command)
            set(patch_command ${CMAKE_COMMAND} -E true)
        endif ()
        list(APPEND patch_command
            COMMAND ${CMAKE_COMMAND} -E rm -rf ${embed_dir}
            COMMAND git clone --quiet ${embed_repository} ${embed_dir}
            COMMAND git -C ${embed_dir} -c advice.detachedHead=false checkout --quiet ${embed_commit})
    endif ()
    ExternalProject_Add(${name}
        GIT_REPOSITORY ${repository}
        GIT_TAG ${commit}
        GIT_SHALLOW OFF
        SOURCE_DIR ${dir}/project
        PATCH_COMMAND ${patch_command}
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ${CMAKE_COMMAND} -DUNITY=${unity} -DPROJECT=${project} -DOUT=${dir} -DPLAYER=${player} -DLOADER=${loader}
            -DPACKAGE_SAMPLES=${package_samples} -P ${CMAKE_SOURCE_DIR}/tests/samples/unity/build.cmake
        BUILD_IN_SOURCE ON
        INSTALL_COMMAND ""
        UPDATE_DISCONNECTED ON)
    set(PUPPETXR_UNITY_APPS ${PUPPETXR_UNITY_APPS} ${name} PARENT_SCOPE)
endfunction ()
if (PUPPETXR_UNITY AND WIN32)
    set(mruk https://github.com/oculus-samples/Unity-MRUtilityKitSample.git 8b6daeb6e592e30faa9d2c2ab423a1e3d5bb1136) # Release for updated SDK 207.0.0
    puppetxr_unity_sample(unity_mruk ${mruk} MRUK.exe openxr)
    puppetxr_unity_sample(unity_mruk_oculus ${mruk} MRUK.exe oculus)
    set(ssa https://github.com/oculus-samples/Unity-SharedSpatialAnchors.git 71b653caac743b29942a2b4f942320b7b8847972) # Release for updated SDK 207.0.0
    puppetxr_unity_sample(unity_ssa ${ssa} SSA.exe openxr)
    puppetxr_unity_sample(unity_ssa_oculus ${ssa} SSA.exe oculus)
    set(starter https://github.com/oculus-samples/Unity-StarterSamples.git a2ba45d7206dcf21a08af7a0ee9bf7768689579f)
    # With the Core SDK's own samples: hands, microgestures, controllers and haptics
    puppetxr_unity_sample(unity_starter ${starter} StarterSamples.exe openxr PACKAGE_SAMPLES com.meta.xr.sdk.core/Scenes com.meta.xr.sdk.core/Haptics)
    set(movement com.meta.xr.sdk.movement https://github.com/oculus-samples/Unity-Movement.git 77274f138974087ba2909bab2723fc6ff851338b) # v207.0.0
    puppetxr_unity_sample(unity_movement ${starter} Movement.exe openxr PACKAGE_SAMPLES com.meta.xr.sdk.movement/. com.meta.xr.sdk.movement/BodyTrackingSamples com.meta.xr.sdk.movement/FaceTrackingSamples EMBED ${movement})
    # Meta's Depth API reaches Unity on Windows only through the Oculus XR plugin
    set(depthapi https://github.com/oculus-samples/Unity-DepthAPI.git 82a8f3f6f38e0833a08dee768a7f016a3ca5c86d)
    puppetxr_unity_sample(unity_depthapi ${depthapi} DepthAPI.exe oculus PROJECT DepthAPI-URP UNITY "${PUPPETXR_UNITY_6000_3}")
    list(JOIN PUPPETXR_UNITY_APPS "," PUPPETXR_UNITY_APPS_ARG)
    # Their scenes load in wall-clock time, which the software-rendered samples beside them
    # would stretch past every timeout, so an entry of their own. One player at a time: OVRPlugin
    # of players running side by side crashed now and then reading another's log.
    add_test(NAME samples_unity
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/sample_tests.py
            --samples ${CMAKE_BINARY_DIR}/samples --runtime ${CMAKE_BINARY_DIR}/puppetxr-dev.json --puppetxrctl $<TARGET_FILE:puppetxrctl>
            --references ${CMAKE_SOURCE_DIR}/tests/samples --out ${CMAKE_BINARY_DIR}/samples_out --apps ${PUPPETXR_UNITY_APPS_ARG} --jobs 1)
    set_tests_properties(samples_unity PROPERTIES TIMEOUT 5400 RESOURCE_LOCK samples)
elseif (PUPPETXR_UNITY AND APPLE)
    # The Apple silicon player on Metal, through Unity's OpenXR plugin; the Oculus XR plugin has no
    # macOS player, so only the OpenXR samples
    set(mruk https://github.com/oculus-samples/Unity-MRUtilityKitSample.git 8b6daeb6e592e30faa9d2c2ab423a1e3d5bb1136) # Release for updated SDK 207.0.0
    puppetxr_unity_sample(unity_mruk ${mruk} MRUK.app openxr)
    # Meta's Movement SDK (body tracking, retargeting) hosted by the Starter samples' project, which
    # has the Core SDK; the patch leaves the package's body tracking scene the only one
    set(starter https://github.com/oculus-samples/Unity-StarterSamples.git a2ba45d7206dcf21a08af7a0ee9bf7768689579f)
    set(movement com.meta.xr.sdk.movement https://github.com/oculus-samples/Unity-Movement.git 77274f138974087ba2909bab2723fc6ff851338b) # v207.0.0
    puppetxr_unity_sample(unity_movement ${starter} Movement.app openxr PACKAGE_SAMPLES com.meta.xr.sdk.movement/. com.meta.xr.sdk.movement/BodyTrackingSamples com.meta.xr.sdk.movement/FaceTrackingSamples EMBED ${movement})
    list(JOIN PUPPETXR_UNITY_APPS "," PUPPETXR_UNITY_APPS_ARG)
    add_test(NAME samples_unity
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/sample_tests.py
            --samples ${CMAKE_BINARY_DIR}/samples --runtime ${CMAKE_BINARY_DIR}/puppetxr-dev.json --puppetxrctl $<TARGET_FILE:puppetxrctl>
            --references ${CMAKE_SOURCE_DIR}/tests/samples --out ${CMAKE_BINARY_DIR}/samples_out --apps ${PUPPETXR_UNITY_APPS_ARG} --jobs 1
            --tolerance 16 --pixel-budget 0.5)
    set_tests_properties(samples_unity PROPERTIES TIMEOUT 5400 RESOURCE_LOCK samples)
elseif (PUPPETXR_UNITY)
    message(STATUS "The Unity samples run on Windows and macOS only; skipped")
endif ()

# Meta's MRUK sample for Unreal, on Meta's fork of the engine (Oculus-VR/UnrealEngine on GitHub,
# branch oculus-5.8, which needs an Epic Games account linked to GitHub) built from source.
# PUPPETXR_UNREAL names its UnrealEditor.exe; the project's editor target is built with the engine's
# Build.bat and the tests run it uncooked with -game; tests/samples/unreal_mruk/sample_tests.patch
# is applied to the checkout.
set(PUPPETXR_UNREAL "" CACHE FILEPATH "UnrealEditor.exe of Meta's Unreal fork that runs the Unreal samples; empty skips them")
if (PUPPETXR_UNREAL AND WIN32)
    get_filename_component(unreal_binaries ${PUPPETXR_UNREAL} DIRECTORY)
    get_filename_component(unreal_root ${unreal_binaries}/../../.. ABSOLUTE)
    ExternalProject_Add(unreal_mruk
        GIT_REPOSITORY https://github.com/oculus-samples/Unreal-MRUtilityKitSample.git
        GIT_TAG 3abad9230d4c8194c11713e2779915c9b9d415de # 5.8.0-v207 and its mobile forward shading pin
        GIT_SHALLOW OFF
        SOURCE_DIR ${CMAKE_BINARY_DIR}/samples/unreal_mruk/project
        # Reverts the files a previous patch changed first: the step reruns on a patched checkout
        PATCH_COMMAND git checkout -- . COMMAND git apply --ignore-whitespace ${CMAKE_SOURCE_DIR}/tests/samples/unreal_mruk/sample_tests.patch
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ${unreal_root}/Engine/Build/BatchFiles/Build.bat MRUtilityKitSampleEditor Win64 Development
            -Project=<SOURCE_DIR>/MRUtilityKitSample.uproject -WaitMutex
        BUILD_IN_SOURCE ON
        INSTALL_COMMAND ""
        UPDATE_DISCONNECTED ON)
    # Meta's other Unreal samples, run uncooked on the mobile renderer they target
    # (-FeatureLevelES31). Blueprint-only projects build nothing; C++ ones their editor target.
    # tests/samples/<name>/sample_tests.patch, if there is one, is applied to the checkout.
    # The Occlusion sample's lights are static and its repository holds no built lighting, so its
    # meshes would be black: its map's lighting is baked with Lightmass (built with the engine's
    # UnrealLightmass target) once after checkout.
    # WARM runs the game once after the build, compiling asynchronously: the tests compile
    # synchronously, and that path fails an assert in the engine's shader compiler on global
    # shaders it has not cached yet (the Movement sample's forward shading and instanced stereo).
    # It runs on headless puppetxr, whatever runtime is active: OculusXR's movement module fails an
    # assert without an OpenXR system.
    set(PUPPETXR_UNREAL_APPS unreal_mruk)
    function (puppetxr_unreal_sample name repository commit project)
        cmake_parse_arguments(PARSE_ARGV 4 sample "WARM" "TARGET;BAKE" "")
        set(build_command ${CMAKE_COMMAND} -E true)
        if (sample_TARGET)
            set(build_command ${unreal_root}/Engine/Build/BatchFiles/Build.bat ${sample_TARGET} Win64 Development -Project=<SOURCE_DIR>/${project} -WaitMutex)
        endif ()
        if (sample_BAKE)
            list(APPEND build_command COMMAND ${unreal_root}/Engine/Build/BatchFiles/Build.bat UnrealLightmass Win64 Development -WaitMutex
                COMMAND ${unreal_binaries}/UnrealEditor-Cmd.exe <SOURCE_DIR>/${project} -run=resavepackages -buildlighting -allowcommandletrendering
                    -map=${sample_BAKE} -quality=Production -unattended -nosplash)
        endif ()
        set(depends "")
        if (sample_WARM)
            list(APPEND build_command COMMAND ${CMAKE_COMMAND} -E env XR_RUNTIME_JSON=${CMAKE_BINARY_DIR}/puppetxr-dev.json PUPPETXR_HEADLESS=1
                ${unreal_binaries}/UnrealEditor.exe <SOURCE_DIR>/${project} -game -windowed -ResX=320 -ResY=240 -nosound -unattended -nosplash -ExecCmds=quit)
            set(depends DEPENDS puppetxr)
        endif ()
        set(patch ${CMAKE_SOURCE_DIR}/tests/samples/${name}/sample_tests.patch)
        set(patch_command "")
        if (EXISTS ${patch})
            # Reverts the files a previous patch changed first: the step reruns on a patched checkout
            set(patch_command git checkout -- . COMMAND git apply --ignore-whitespace ${patch})
        endif ()
        ExternalProject_Add(${name}
            GIT_REPOSITORY ${repository}
            GIT_TAG ${commit}
            GIT_SHALLOW OFF
            SOURCE_DIR ${CMAKE_BINARY_DIR}/samples/${name}/project
            PATCH_COMMAND ${patch_command}
            CONFIGURE_COMMAND ""
            BUILD_COMMAND ${build_command}
            BUILD_IN_SOURCE ON
            INSTALL_COMMAND ""
            UPDATE_DISCONNECTED ON
            ${depends})
        set(PUPPETXR_UNREAL_APPS ${PUPPETXR_UNREAL_APPS} ${name} PARENT_SCOPE)
    endfunction ()
    puppetxr_unreal_sample(unreal_passthrough https://github.com/oculus-samples/Unreal-PassthroughSample.git abe38260a14f5ecde1dc9482896a2ca6ec413b0c PTSamples.uproject)
    puppetxr_unreal_sample(unreal_occlusion https://github.com/oculus-samples/Unreal-OcclusionSample.git e5e0f5865b9c6fae5883c08d0e15eb038c190893 OcclusionSample.uproject
        BAKE /Game/Occlusions/Maps/Occlusions)
    puppetxr_unreal_sample(unreal_shared_anchors https://github.com/oculus-samples/Unreal-SharedAnchorsSample.git 56a2f986ebfa54383486ac6e3b206041f0d15a14 SharedAnchorsSample.uproject
        TARGET SharedAnchorsSampleEditor)
    puppetxr_unreal_sample(unreal_isdk https://github.com/oculus-samples/Unreal-InteractionSDK-Sample.git de94c3674d48f40ab61fd11dc30449ebf5243f61 MetaIsdkSample.uproject
        TARGET MetaIsdkSampleEditor)
    puppetxr_unreal_sample(unreal_movement https://github.com/oculus-samples/Unreal-Movement.git 8d7ede9e9864a0f0940097386dcaa9eb86df06a6 MovementSample.uproject
        TARGET MovementSampleEditor WARM)
    list(JOIN PUPPETXR_UNREAL_APPS "," PUPPETXR_UNREAL_APPS_ARG)
    # The first run compiles the project's shaders for minutes, into its DerivedDataCache
    add_test(NAME samples_unreal
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/sample_tests.py
            --samples ${CMAKE_BINARY_DIR}/samples --runtime ${CMAKE_BINARY_DIR}/puppetxr-dev.json --puppetxrctl $<TARGET_FILE:puppetxrctl>
            --references ${CMAKE_SOURCE_DIR}/tests/samples --out ${CMAKE_BINARY_DIR}/samples_out --apps ${PUPPETXR_UNREAL_APPS_ARG} --unreal ${PUPPETXR_UNREAL} --jobs 1)
    set_tests_properties(samples_unreal PROPERTIES TIMEOUT 7200 RESOURCE_LOCK samples)
elseif (PUPPETXR_UNREAL)
    message(STATUS "The Unreal samples run on Windows only; skipped")
endif ()

if (NOT PUPPETXR_SAMPLE_APPS)
    return()
endif ()
list(JOIN PUPPETXR_SAMPLE_APPS "," PUPPETXR_SAMPLE_APPS_ARG)
# The Mesa of mesa-dist-win draws a few pixels of the Linux references up to 3 apart
# llvmpipe JITs for the host CPU, so another x86 CPU moves pixels by 1 (docs/testing.md)
set(PUPPETXR_SAMPLES_TOLERANCE 1)
set(PUPPETXR_SAMPLES_EXTRA "")
if (WIN32)
    set(PUPPETXR_SAMPLES_TOLERANCE 3)
endif ()
# macOS draws on the GPU against the llvmpipe references: shading and filtering differ by a few
# steps over the image and more at edges. One test at a time: a moving Godot demo beside other
# tests on the Apple GPU sometimes showed the frame before
if (APPLE)
    set(PUPPETXR_SAMPLES_TOLERANCE 16)
    set(PUPPETXR_SAMPLES_EXTRA --pixel-budget 0.5 --jobs 1)
endif ()
add_test(NAME samples
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/sample_tests.py
        --samples ${CMAKE_BINARY_DIR}/samples --runtime ${CMAKE_BINARY_DIR}/puppetxr-dev.json --puppetxrctl $<TARGET_FILE:puppetxrctl>
        --references ${CMAKE_SOURCE_DIR}/tests/samples --out ${CMAKE_BINARY_DIR}/samples_out --apps ${PUPPETXR_SAMPLE_APPS_ARG}
        --tolerance ${PUPPETXR_SAMPLES_TOLERANCE} ${PUPPETXR_SAMPLES_EXTRA})
set_tests_properties(samples PROPERTIES TIMEOUT 3600 RESOURCE_LOCK samples ENVIRONMENT "${PUPPETXR_TEST_ENVIRONMENT}")
