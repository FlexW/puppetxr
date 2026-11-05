# cmake -DGODOT=<godot> -DPROJECT=<dir> -P import.cmake: imports a Godot project for the sample
# tests. The first import of a project with the vendors addon aborts on exit after it imported
# everything, so a second import must succeed.
execute_process(COMMAND ${GODOT} --headless --path ${PROJECT} --import OUTPUT_QUIET ERROR_QUIET)
execute_process(COMMAND ${GODOT} --headless --path ${PROJECT} --import OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE result)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "Importing ${PROJECT} failed: ${result}")
endif ()
