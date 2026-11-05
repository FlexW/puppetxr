# Builds the Windows player of a Unity sample, on macOS the Apple silicon one (cmake/samples.cmake):
#   cmake -DUNITY=<Unity.exe> -DPROJECT=<project> -DOUT=<dir> -DPLAYER=<name.exe or name.app> [-DLOADER=oculus]
#         [-DPACKAGE_SAMPLES=<package>/<sample>,...] -P build.cmake
# - PuppetxrFixedStep.cs goes into the project: -puppetxr-fixed-step makes game time and random
#   numbers follow the stepped frames. PuppetxrBuild.cs builds the player with Mono and the
#   project's scene list.
# - Sample repositories ignore Assets/TextMesh Pro, which the editor offers to import on first
#   use; without it menus have neither text nor buttons, so the first build imports it from the
#   uGUI package once the packages are resolved.
# - PACKAGE_SAMPLES imports samples of the project's packages, cached or embedded in Packages
#   (the folders under a package's Samples~, as the Package Manager's Import button copies them)
#   into Assets/Samples, and the player gets their scenes after the project's own; <package>/.
#   imports the files at the root of Samples~, which some packages' samples share.
# - LOADER=oculus adds Unity's Oculus XR plugin and makes it the Windows loader
#   (PuppetxrOculusLoader.cs), the only Windows path with Meta's Depth API.

set(here ${CMAKE_CURRENT_LIST_DIR})

# Runs the editor in batch mode. Unity's macOS editor aborts in native code after a successful
# batch run ("terminate_handler unexpectedly returned"), so each step counts as done by its log
function (unity_run log)
    execute_process(COMMAND ${UNITY} ${ARGN} -logFile ${log} RESULT_VARIABLE result)
    if (result EQUAL 0)
        return()
    endif ()
    file(READ ${log} text)
    if (CMAKE_HOST_APPLE AND (text MATCHES "Exiting batchmode successfully now!" OR text MATCHES "Build Finished, Result: Success"))
        return()
    endif ()
    message(FATAL_ERROR "Unity failed (${result}), see ${log}")
endfunction ()
file(COPY ${here}/PuppetxrFixedStep.cs DESTINATION ${PROJECT}/Assets)
file(COPY ${here}/PuppetxrBuild.cs DESTINATION ${PROJECT}/Assets/Editor)
if (LOADER STREQUAL "oculus")
    file(READ ${PROJECT}/Packages/manifest.json manifest)
    if (NOT manifest MATCHES "com.unity.xr.oculus")
        string(REPLACE "\"dependencies\": {" "\"dependencies\": {\n    \"com.unity.xr.oculus\": \"4.5.2\"," manifest "${manifest}")
        file(WRITE ${PROJECT}/Packages/manifest.json "${manifest}")
    endif ()
    file(COPY ${here}/PuppetxrOculusLoader.cs DESTINATION ${PROJECT}/Assets/Editor)
endif ()

if (NOT EXISTS "${PROJECT}/Assets/TextMesh Pro")
    unity_run(${OUT}/resolve.log -batchmode -quit -projectPath ${PROJECT})
    file(GLOB essentials "${PROJECT}/Library/PackageCache/com.unity.ugui@*/Package Resources/TMP Essential Resources.unitypackage")
    if (NOT essentials)
        message(FATAL_ERROR "No TMP Essential Resources in the uGUI package of ${PROJECT}")
    endif ()
    unity_run(${OUT}/import.log -batchmode -quit -projectPath ${PROJECT} -importPackage ${essentials})
endif ()
if (LOADER STREQUAL "oculus")
    unity_run(${OUT}/loader.log -batchmode -quit -projectPath ${PROJECT} -executeMethod PuppetxrOculusLoader.Run)
endif ()
set(extra_scenes "")
if (PACKAGE_SAMPLES)
    string(REPLACE "," ";" package_samples "${PACKAGE_SAMPLES}")
    foreach (sample ${package_samples})
        string(REPLACE "/" ";" parts ${sample})
        list(GET parts 0 package)
        list(GET parts 1 folder)
        if (folder STREQUAL ".")
            # The files at the root of Samples~, which samples of the package share
            file(GLOB files LIST_DIRECTORIES false "${PROJECT}/Library/PackageCache/${package}@*/Samples~/*" "${PROJECT}/Packages/${package}/Samples~/*")
            foreach (path ${files})
                # A sample folder's .meta comes with the folder, else Unity makes an empty one
                string(REGEX REPLACE "\\.meta$" "" folder_path "${path}")
                if (IS_DIRECTORY "${folder_path}")
                    list(REMOVE_ITEM files "${path}")
                endif ()
            endforeach ()
            if (NOT files)
                message(FATAL_ERROR "No files in the Samples~ of ${package} of ${PROJECT}")
            endif ()
            file(COPY ${files} DESTINATION ${PROJECT}/Assets/Samples/${package})
        elseif (NOT EXISTS ${PROJECT}/Assets/Samples/${package}/${folder})
            file(GLOB source "${PROJECT}/Library/PackageCache/${package}@*/Samples~/${folder}" "${PROJECT}/Packages/${package}/Samples~/${folder}")
            if (NOT source)
                message(FATAL_ERROR "No sample ${folder} in ${package} of ${PROJECT}")
            endif ()
            file(COPY ${source} DESTINATION ${PROJECT}/Assets/Samples/${package})
        endif ()
    endforeach ()
    set(extra_scenes -puppetxrScenes Assets/Samples)
endif ()
set(build_target Win64)
if (CMAKE_HOST_APPLE)
    set(build_target OSXUniversal)
endif ()
unity_run(${OUT}/build.log -batchmode -projectPath ${PROJECT} -buildTarget ${build_target} -executeMethod PuppetxrBuild.Build -puppetxrOutput ${OUT}/player/${PLAYER} ${extra_scenes})
