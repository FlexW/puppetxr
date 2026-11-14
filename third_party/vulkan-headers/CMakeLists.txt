# ~~~
# Copyright 2018-2023 The Khronos Group Inc.
# Copyright 2018-2023 Valve Corporation
# Copyright 2018-2023 LunarG, Inc.
#
# SPDX-License-Identifier: Apache-2.0
# ~~~
cmake_minimum_required(VERSION 3.22.1)


# NOTE: Parsing the version like this is suboptimal but neccessary due to our release process:
# https://github.com/KhronosGroup/Vulkan-Headers/pull/346
#
# As shown a more robust approach would be just to add basic test code to check the project version.
function(vlk_get_header_version)
    set(vulkan_core_header_file "${CMAKE_CURRENT_SOURCE_DIR}/include/vulkan/vulkan_core.h")
    if (NOT EXISTS ${vulkan_core_header_file})
        message(FATAL_ERROR "Couldn't find vulkan_core.h!")
    endif()

    file(READ ${vulkan_core_header_file} ver)

    if (ver MATCHES "#define[ ]+VK_HEADER_VERSION_COMPLETE[ ]+VK_MAKE_API_VERSION\\([ ]*[0-9]+,[ ]*([0-9]+),[ ]*([0-9]+),[ ]*VK_HEADER_VERSION[ ]*\\)")
        set(MAJOR_VERSION "${CMAKE_MATCH_1}")
        set(MINOR_VERSION "${CMAKE_MATCH_2}")
    else()
        message(FATAL_ERROR "Couldn't get major/minor version")
    endif()

    if (ver MATCHES "#define[ ]+VK_HEADER_VERSION[ ]+([0-9]+)")
        set(PATCH_VERSION "${CMAKE_MATCH_1}")
    else()
        message(FATAL_ERROR "Couldn't get the patch version")
    endif()

    set(VK_VERSION_STRING "${MAJOR_VERSION}.${MINOR_VERSION}.${PATCH_VERSION}" PARENT_SCOPE)
endfunction()
vlk_get_header_version()

project(VULKAN_HEADERS LANGUAGES C CXX VERSION ${VK_VERSION_STRING})

# set up Vulkan-Headers
add_library(Vulkan-Headers INTERFACE)
add_library(Vulkan::Headers ALIAS Vulkan-Headers)
target_include_directories(Vulkan-Headers INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
