# cmake -DGIT=<git> -DREPO=<checkout> -P check_line_endings.cmake
# Fails if a file tracked in REPO has a carriage return at a line end, in the index or in the
# working tree (AGENTS.md, Style: Unix line endings). Git tells text from binary files.

cmake_minimum_required(VERSION 3.25)

# Git refuses a repository another user owns, as an elevated Windows runner sees a fresh one
execute_process(COMMAND ${GIT} -c safe.directory=* -C ${REPO} ls-files --eol OUTPUT_VARIABLE listing ERROR_VARIABLE error RESULT_VARIABLE result)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "git ls-files --eol in ${REPO} failed: ${error}")
endif ()
string(REPLACE "\n" ";" lines "${listing}")
set(failed FALSE)
foreach (line ${lines})
    if (line MATCHES "^i/([^ ]*) +w/([^ ]*) +attr/[^\t]*\t(.*)$")
        set(index ${CMAKE_MATCH_1})
        set(tree ${CMAKE_MATCH_2})
        set(path ${CMAKE_MATCH_3})
        if (index MATCHES "crlf|mixed" OR tree MATCHES "crlf|mixed")
            message(SEND_ERROR "${path} has CRLF line endings (index ${index}, working tree ${tree})")
            set(failed TRUE)
        endif ()
    endif ()
endforeach ()
if (failed)
    message(FATAL_ERROR "Not all files in ${REPO} have Unix line endings")
endif ()
