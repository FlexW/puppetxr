# End-to-end example: record a scripted session of puppetxr_example, replay it without the script
# and require the same per-frame observations and a byte-identical screenshot.
# Usage: cmake -DAPP=<puppetxr_example> -DRUNTIME_JSON=<puppetxr-dev.json> -DOUT=<dir> -P example_e2e.cmake

file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})

set(ENV{XR_RUNTIME_JSON} ${RUNTIME_JSON})
set(ENV{PUPPETXR_HEADLESS} 1)
set(ENV{PUPPETXR_FIXED_STEP} 1)

function(run_app name)
    execute_process(COMMAND ${EMULATOR} ${APP} ${ARGN}
        OUTPUT_FILE ${OUT}/${name}.log
        ERROR_FILE ${OUT}/${name}.err
        RESULT_VARIABLE result
        TIMEOUT 120)
    if (NOT result EQUAL 0)
        file(READ ${OUT}/${name}.err errors)
        message(FATAL_ERROR "${name} run failed (${result}):\n${errors}")
    endif ()
endfunction()

set(ENV{PUPPETXR_RECORD} ${OUT}/session.xrrec)
run_app(record --frames 60 --script --log --screenshot ${OUT}/record.png 40)
unset(ENV{PUPPETXR_RECORD})

set(ENV{PUPPETXR_REPLAY} ${OUT}/session.xrrec)
set(ENV{PUPPETXR_REPLAY_EXIT} 0)
run_app(replay --frames 60 --log --screenshot ${OUT}/replay.png 40)

file(STRINGS ${OUT}/record.log record_frames REGEX "^frame ")
file(STRINGS ${OUT}/replay.log replay_frames REGEX "^frame ")
list(LENGTH record_frames count)
if (NOT count EQUAL 60)
    message(FATAL_ERROR "expected 60 frame lines, got ${count}")
endif ()
if (NOT record_frames STREQUAL replay_frames)
    message(FATAL_ERROR "replayed frames differ from the recorded ones, compare ${OUT}/record.log and ${OUT}/replay.log")
endif ()
if (NOT record_frames MATCHES "trigger 0\\.5263")
    message(FATAL_ERROR "the scripted trigger never reached the app")
endif ()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/record.png ${OUT}/replay.png RESULT_VARIABLE different)
if (different)
    message(FATAL_ERROR "replayed screenshot differs from the recorded one")
endif ()
message(STATUS "example e2e: 60 frames and the screenshot match")
