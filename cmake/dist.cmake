# The distributed build (preset `dist`, docs/building.md): optimized, link-time optimized,
# hardened, reproducible, shipped without symbols. Applies to every target, third-party
# code included, since all of it ends up in the shipped library.

if (NOT PUPPETXR_DIST)
    return()
endif ()

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|MSVC")
    message(FATAL_ERROR "PUPPETXR_DIST supports GCC, clang and MSVC")
endif ()
if (NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "PUPPETXR_DIST needs CMAKE_BUILD_TYPE=Release, not '${CMAKE_BUILD_TYPE}'")
endif ()

include(CheckIPOSupported)
check_ipo_supported(RESULT puppetxr_ipo OUTPUT puppetxr_ipo_error)
if (NOT puppetxr_ipo)
    message(FATAL_ERROR "PUPPETXR_DIST needs link-time optimization: ${puppetxr_ipo_error}")
endif ()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
# CMake picks ThinLTO for clang, which renames promoted locals with a per-build module hash
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_C_COMPILE_OPTIONS_IPO -flto=full)
    set(CMAKE_CXX_COMPILE_OPTIONS_IPO -flto=full)
endif ()

# Windows: debug info in a PDB next to the binary, referenced by file name only; control flow
# guard and CET shadow stacks; /Brepro replaces the link time stamp with a content hash; clang
# maps paths as on Linux, MSVC trims the source directory from __FILE__
if (WIN32)
    if (NOT MSVC AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        message(FATAL_ERROR "PUPPETXR_DIST on Windows needs MSVC or clang targeting the MSVC ABI")
    endif ()
    if (MSVC)
        add_compile_options(/Zi /Gy /Gw /guard:cf /Brepro "/d1trimfile:${CMAKE_SOURCE_DIR}/" "/d1trimfile:${CMAKE_BINARY_DIR}/")
    else ()
        # -mguard=cf is the MinGW spelling; for the MSVC ABI the driver takes clang-cl's cc1 flag
        add_compile_options(-gcodeview -ffunction-sections -fdata-sections "SHELL:-Xclang -cfguard"
            -ffile-prefix-map=${CMAKE_SOURCE_DIR}=. -ffile-prefix-map=${CMAKE_BINARY_DIR}=build
            -mno-incremental-linker-compatible)
        # link.exe cannot read LLVM bitcode
        add_link_options(-fuse-ld=lld)
    endif ()
    add_link_options(LINKER:/DEBUG LINKER:/PDBALTPATH:%_PDB% LINKER:/OPT:REF LINKER:/OPT:ICF LINKER:/GUARD:CF
        LINKER:/CETCOMPAT LINKER:/DYNAMICBASE LINKER:/HIGHENTROPYVA LINKER:/Brepro)
    # The PDB is already separate; it stays in the build directory and is never installed
    function(puppetxr_split_debug_info target)
    endfunction()
    return()
endif ()

# macOS: debug info in a .dSYM bundle next to the binary, local symbols stripped. With LTO the
# linker compiles the program into one temporary object, which the debug map names and the linker
# then deletes: the .dSYM lacked all LTO code, so each architecture's object is kept
# (-object_path_lto) until dsymutil read it (test dsym_holds_the_debug_info_<arch>)
if (APPLE)
    add_compile_options(
        -g
        -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
        -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
        -ffile-prefix-map=${CMAKE_BINARY_DIR}=build)
    add_link_options(-Wl,-dead_strip)
    find_program(PUPPETXR_DSYMUTIL dsymutil REQUIRED)
    find_program(PUPPETXR_STRIP strip REQUIRED)
    function(puppetxr_split_debug_info target)
        set(lto_dir ${CMAKE_BINARY_DIR}/lto/${target})
        file(MAKE_DIRECTORY ${lto_dir})
        foreach (arch ${CMAKE_OSX_ARCHITECTURES})
            target_link_options(${target} PRIVATE "SHELL:-Xarch_${arch} -Wl,-object_path_lto,${lto_dir}/${arch}.o")
        endforeach ()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${PUPPETXR_DSYMUTIL} $<TARGET_FILE_NAME:${target}> -o $<TARGET_FILE_NAME:${target}>.dSYM
            COMMAND ${PUPPETXR_STRIP} -x $<TARGET_FILE_NAME:${target}>
            WORKING_DIRECTORY $<TARGET_FILE_DIR:${target}>
            VERBATIM)
    endfunction()
    return()
endif ()

# Debug info goes into a separate .debug file next to the build (puppetxr_split_debug_info), never
# into the shipped binaries. Paths are made relative so builds are byte-identical anywhere.
add_compile_options(
    -g
    -ffunction-sections -fdata-sections
    -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -ffile-prefix-map=${CMAKE_BINARY_DIR}=build)
# GCC names LTO-local symbols after the file, a random seed and the unmapped working directory:
# with the seed fixed its builds repeat only in the same directory, so the preset uses clang
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-frandom-seed=puppetxr)
    add_link_options(-frandom-seed=puppetxr)
endif ()
# The C++ runtime is linked in: the library then needs glibc alone, whatever libstdc++ the app or
# the distribution has (test shipped_binaries_need_only_glibc); the version script keeps it hidden
add_link_options(
    -static-libstdc++ -static-libgcc
    -Wl,--gc-sections -Wl,--as-needed
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)

if (NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "PUPPETXR_DIST needs objcopy to strip the shipped binaries")
endif ()

# Moves the target's debug info into <file>.debug, strips everything but the dynamic symbols
# and links the two by build id and debuglink, so crash backtraces can be symbolized later
function(puppetxr_split_debug_info target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE_NAME:${target}> $<TARGET_FILE_NAME:${target}>.debug
        COMMAND ${CMAKE_OBJCOPY} --strip-all --add-gnu-debuglink=$<TARGET_FILE_NAME:${target}>.debug $<TARGET_FILE_NAME:${target}>
        WORKING_DIRECTORY $<TARGET_FILE_DIR:${target}>
        VERBATIM)
endfunction()
