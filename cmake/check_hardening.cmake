# cmake -DREADOBJ=<llvm-readobj> -DFILES=<a;b> [-DREQUIRE=<flag;...>] -P check_hardening.cmake
# Fails unless every file is built as the Windows release promises (docs/building.md): control
# flow guard with instrumented code, CET shadow stack compatibility, ASLR with high entropy, NX.
# REQUIRE replaces the flags looked for, for the test of this check.

cmake_minimum_required(VERSION 3.25)

if (NOT DEFINED REQUIRE)
    set(REQUIRE IMAGE_DLL_CHARACTERISTICS_GUARD_CF CF_INSTRUMENTED IMAGE_DLL_CHARACTERISTICS_EX_CET_COMPAT
        IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA IMAGE_DLL_CHARACTERISTICS_NX_COMPAT)
endif ()
set(failed FALSE)
foreach (file ${FILES})
    execute_process(COMMAND ${READOBJ} --file-headers --coff-load-config --coff-debug-directory ${file} OUTPUT_VARIABLE dump RESULT_VARIABLE result)
    if (NOT result EQUAL 0)
        message(SEND_ERROR "${READOBJ} ${file} failed")
        set(failed TRUE)
        continue()
    endif ()
    foreach (flag ${REQUIRE})
        string(FIND "${dump}" "${flag} " found)
        if (found EQUAL -1)
            message(SEND_ERROR "${file} lacks ${flag}")
            set(failed TRUE)
        endif ()
    endforeach ()
endforeach ()
if (failed)
    message(FATAL_ERROR "Not hardened: ${FILES}")
endif ()
