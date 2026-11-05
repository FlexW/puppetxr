# cmake -DOBJDUMP=<llvm-objdump> -DFILES=<a;b> [-DDENY=<dll;...>] -P check_dlls.cmake
# Fails unless every file imports only DLLs that Windows 10 ships, so the release needs no
# redistributable: the C++ runtime is linked in and the UCRT is part of Windows
# (docs/building.md). DENY removes DLLs from the allowed set, for the test of this check.

cmake_minimum_required(VERSION 3.25)

set(allowed
    kernel32.dll user32.dll gdi32.dll advapi32.dll shell32.dll ole32.dll oleaut32.dll imm32.dll
    setupapi.dll cfgmgr32.dll version.dll winmm.dll ws2_32.dll dwmapi.dll uxtheme.dll hid.dll
    psapi.dll shlwapi.dll bcrypt.dll ntdll.dll ucrtbase.dll)
string(TOLOWER "${DENY}" deny)
list(REMOVE_ITEM allowed ${deny})
set(failed FALSE)
foreach (file ${FILES})
    execute_process(COMMAND ${OBJDUMP} -p ${file} OUTPUT_VARIABLE dump RESULT_VARIABLE result)
    if (NOT result EQUAL 0)
        message(SEND_ERROR "${OBJDUMP} -p ${file} failed")
        set(failed TRUE)
        continue()
    endif ()
    string(REGEX MATCHALL "DLL Name: [^\r\n]+" imports "${dump}")
    list(REMOVE_DUPLICATES imports)
    foreach (entry ${imports})
        string(REGEX REPLACE "DLL Name: " "" dll "${entry}")
        string(TOLOWER "${dll}" dll)
        # The UCRT and core API sets resolve to DLLs of Windows itself
        if (dll MATCHES "^api-ms-win-(crt|core)-" OR dll IN_LIST allowed)
            continue()
        endif ()
        message(SEND_ERROR "${file} imports ${dll}")
        set(failed TRUE)
    endforeach ()
endforeach ()
if (failed)
    message(FATAL_ERROR "Not portable: ${FILES}")
endif ()
