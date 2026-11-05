# An install tree works after being moved: installs into a temporary prefix, holds it to the files
# the release ships (docs/building.md; a dependency's install rules must not leak in), moves it,
# runs puppetxr_example through the moved manifest, requires a room compiled into the library to load
# with no rooms on disk, and the installed puppetxrctl to run, as both ship in the release.
# Usage: cmake -DAPP=<puppetxr_example> -DBUILD=<build dir> -DOUT=<dir> -DVERSION=<x.y.z> -P install_relocated.cmake

file(REMOVE_RECURSE ${OUT})
execute_process(COMMAND ${CMAKE_COMMAND} --install ${BUILD} --prefix ${OUT}/installed OUTPUT_QUIET RESULT_VARIABLE result)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (${result})")
endif ()

file(GLOB_RECURSE installed RELATIVE ${OUT}/installed ${OUT}/installed/*)
set(shipped
    "^bin/puppetxrctl(\\.exe)?$"
    "^bin/(lib)?puppetxr\\.dll$"
    "^lib[^/]*/(.+/)?libpuppetxr\\.so$"
    "^lib/libpuppetxr\\.dylib$"
    "^include/puppetxr\\.h$"
    "^share/doc/puppetxr/LICENSE$"
    "^share/openxr/1/puppetxr\\.json$"
    "^install-runtime\\.ps1$")
set(unexpected "")
foreach (path ${installed})
    set(known FALSE)
    foreach (pattern ${shipped})
        if (path MATCHES "${pattern}")
            set(known TRUE)
        endif ()
    endforeach ()
    if (NOT known)
        string(APPEND unexpected "\n  ${path}")
    endif ()
endforeach ()
if (unexpected)
    message(FATAL_ERROR "the install tree has files the release does not ship:${unexpected}")
endif ()

file(RENAME ${OUT}/installed ${OUT}/moved)

file(GLOB manifest ${OUT}/moved/share/openxr/1/puppetxr.json)
if (NOT manifest)
    message(FATAL_ERROR "no installed manifest under ${OUT}/moved/share/openxr/1")
endif ()

set(ENV{XR_RUNTIME_JSON} ${manifest})
set(ENV{PUPPETXR_HEADLESS} 1)
set(ENV{PUPPETXR_FIXED_STEP} 1)
set(ENV{PUPPETXR_ROOM} bedroom)
file(MAKE_DIRECTORY ${OUT}/no_rooms)
set(ENV{PUPPETXR_ROOMS_DIR} ${OUT}/no_rooms)
set(ENV{PUPPETXR_LOG_FILE} ${OUT}/runtime.log)
execute_process(COMMAND ${EMULATOR} ${APP} --frames 5
    OUTPUT_FILE ${OUT}/app.log
    ERROR_FILE ${OUT}/app.err
    RESULT_VARIABLE result
    TIMEOUT 120)
if (NOT result EQUAL 0)
    file(READ ${OUT}/app.err errors)
    message(FATAL_ERROR "puppetxr_example failed through the moved install (${result}):\n${errors}")
endif ()

file(READ ${OUT}/runtime.log log)
if (NOT log MATCHES "Room bedroom loaded")
    message(FATAL_ERROR "the moved library did not load its compiled-in room, see ${OUT}/runtime.log")
endif ()

file(GLOB puppetxrctl ${OUT}/moved/bin/puppetxrctl ${OUT}/moved/bin/puppetxrctl.exe)
if (NOT puppetxrctl)
    message(FATAL_ERROR "no installed puppetxrctl under ${OUT}/moved/bin")
endif ()
execute_process(COMMAND ${EMULATOR} ${puppetxrctl} --version
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    RESULT_VARIABLE result
    TIMEOUT 60)
if (NOT result EQUAL 0 OR NOT output MATCHES "^puppetxrctl ${VERSION}")
    message(FATAL_ERROR "the moved puppetxrctl does not run (${result}): ${output}${errors}")
endif ()
message(STATUS "install relocated: the moved tree runs, loads a compiled-in room and runs puppetxrctl")
