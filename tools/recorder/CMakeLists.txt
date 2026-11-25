# The motion recorder (docs/recorder.md). Added by the top CMakeLists.txt, it builds the desktop
# executable through the OpenXR loader; the tests build recorder.cpp into puppetxr_tests. As a
# project of its own, for the NDK (build_apk.sh), it builds the shared library NativeActivity
# loads, with the loader and only what the recorder needs.

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(puppetxr_recorder LANGUAGES C CXX)
    if (NOT ANDROID)
        message(FATAL_ERROR "tools/recorder alone builds only for Android; on the desktop configure the repository's root")
    endif ()
    set(root ${CMAKE_CURRENT_SOURCE_DIR}/../..)
    option(PUPPETXR_WERROR "Treat warnings as errors" ON)
    set(PUPPETXR_SANITIZE "")
    set(PUPPETXR_FAST_MATH OFF)
    include(${root}/cmake/flags.cmake)

    add_subdirectory(${root}/third_party/vulkan-headers vulkan-headers EXCLUDE_FROM_ALL SYSTEM)
    set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
    add_subdirectory(${root}/third_party/volk volk EXCLUDE_FROM_ALL SYSTEM)
    target_link_libraries(volk PUBLIC Vulkan::Headers)
    target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
    add_subdirectory(${root}/third_party/openxr openxr EXCLUDE_FROM_ALL)
    add_library(jsmn INTERFACE)
    target_include_directories(jsmn SYSTEM INTERFACE ${root}/third_party/jsmn)

    add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
    target_include_directories(native_app_glue SYSTEM PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

    add_library(puppetxr_recorder SHARED recorder.cpp recorder_android.cpp)
    target_compile_features(puppetxr_recorder PRIVATE cxx_std_20)
    target_link_libraries(puppetxr_recorder PRIVATE openxr_loader volk jsmn native_app_glue android log)
    # NativeActivity looks the entry up by name; nothing else references it
    target_link_options(puppetxr_recorder PRIVATE -u ANativeActivity_onCreate)
    puppetxr_apply_flags(puppetxr_recorder)
    return()
endif ()

add_executable(puppetxr_recorder recorder.cpp recorder_desktop.cpp)
target_compile_features(puppetxr_recorder PRIVATE cxx_std_20)
target_link_libraries(puppetxr_recorder PRIVATE openxr_loader volk jsmn Vulkan::Headers)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_link_libraries(puppetxr_recorder PRIVATE ${CMAKE_DL_LIBS})
endif ()
puppetxr_apply_flags(puppetxr_recorder)
add_dependencies(puppetxr_recorder puppetxr)
