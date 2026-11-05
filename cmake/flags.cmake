include(CheckCXXCompilerFlag)

# Warnings, no exceptions, no RTTI, strict floating point. Applied privately to the C++
# sources of our own targets so third-party code keeps its own flags. Every warning
# builds clean with both GCC and clang (docs/building.md).
function(puppetxr_apply_flags target)
    set(flags)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        list(APPEND flags
            -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion
            -Wformat=2 -Wundef -Wvla -Wpointer-arith -Wcast-qual -Wextra-semi
            -Wmissing-declarations -Wimplicit-fallthrough -Wnon-virtual-dtor
            -fno-exceptions -fno-rtti -fno-math-errno)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            list(APPEND flags -Wduplicated-cond -Wduplicated-branches -Wlogical-op)
        else ()
            list(APPEND flags -Wshadow-all -Wcomma -Wunreachable-code -Wloop-analysis)
        endif ()
        if (PUPPETXR_WERROR)
            list(APPEND flags -Werror)
        endif ()
        if (NOT PUPPETXR_FAST_MATH)
            list(APPEND flags -ffp-contract=off -fno-fast-math)
        endif ()
        # Only our targets: third-party libraries keep linking with their own --no-undefined
        if (PUPPETXR_SANITIZE STREQUAL "address")
            list(APPEND flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
            target_link_options(${target} PUBLIC -fsanitize=address,undefined)
        elseif (PUPPETXR_SANITIZE STREQUAL "thread" AND WIN32)
            message(FATAL_ERROR "ThreadSanitizer does not exist on Windows")
        elseif (PUPPETXR_SANITIZE STREQUAL "thread")
            list(APPEND flags -fsanitize=thread -fno-omit-frame-pointer)
            target_link_options(${target} PUBLIC -fsanitize=thread)
        elseif (PUPPETXR_SANITIZE)
            message(FATAL_ERROR "PUPPETXR_SANITIZE must be empty, address or thread, not ${PUPPETXR_SANITIZE}")
        endif ()

        set(CMAKE_REQUIRED_FLAGS "-Werror")
        check_cxx_compiler_flag(-Wno-missing-designated-field-initializers HAVE_WNO_MISSING_DESIGNATED_FIELD_INITIALIZERS)
        unset(CMAKE_REQUIRED_FLAGS)
        if (HAVE_WNO_MISSING_DESIGNATED_FIELD_INITIALIZERS)
            list(APPEND flags -Wno-missing-designated-field-initializers)
        endif ()
        list(APPEND flags -Wno-missing-field-initializers)
    elseif (MSVC)
        # /W4 covers -Wall -Wextra -Wconversion; the w14xxx ones are off by default in MSVC and
        # match -Wshadow, -Wsign-conversion, -Wnon-virtual-dtor and -Wimplicit-fallthrough.
        # C4127 (constant conditions) and C4324 (alignas padding) have no GCC counterpart.
        # Without contraction /fp:precise keeps a * b + c two roundings (docs/determinism.md).
        list(APPEND flags /W4 /w14456 /w14457 /w14458 /w14459 /w14245 /w14388 /w14265 /w15262
            /wd4127 /wd4324 /permissive- /Zc:preprocessor /Zc:__cplusplus /Zc:inline /utf-8
            /EHs-c- /GR- /fp:precise)
        if (PUPPETXR_WERROR)
            list(APPEND flags /WX)
        endif ()
        if (PUPPETXR_FAST_MATH)
            list(APPEND flags /fp:contract)
        endif ()
        if (PUPPETXR_SANITIZE STREQUAL "address")
            list(APPEND flags /fsanitize=address)
            target_link_options(${target} PUBLIC /INCREMENTAL:NO)
        elseif (PUPPETXR_SANITIZE)
            message(FATAL_ERROR "MSVC supports PUPPETXR_SANITIZE=address only, not ${PUPPETXR_SANITIZE}")
        endif ()
    endif ()
    if (WIN32)
        # No exceptions in the MSVC STL either, Win32 headers without min/max macros and ANSI
        # entry points (platform.cpp calls the W ones explicitly). The SDK headers hide what is
        # newer than Windows 10 1809, the oldest Windows the release supports (docs/building.md).
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
            _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006)
    endif ()
    # The tests take four fifths of the clang-tidy time, so only an explicit full run checks them
    if (PUPPETXR_CLANG_TIDY AND (PUPPETXR_CLANG_TIDY_ALL OR target STREQUAL "puppetxr_core" OR target STREQUAL "puppetxrctl"))
        find_program(PUPPETXR_CLANG_TIDY_PROGRAM clang-tidy REQUIRED)
        set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${PUPPETXR_CLANG_TIDY_PROGRAM}")
    endif ()
    list(TRANSFORM flags PREPEND "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:")
    list(TRANSFORM flags APPEND ">")
    target_compile_options(${target} PRIVATE ${flags})
endfunction()
