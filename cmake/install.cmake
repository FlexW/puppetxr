include(GNUInstallDirs)

option(PUPPETXR_INSTALL_ACTIVE_RUNTIME "Also install active_runtime.json so puppetxr becomes the system default OpenXR runtime" OFF)
set(PUPPETXR_ACTIVE_RUNTIME_DIR "" CACHE PATH "Directory for active_runtime.json (default: <sysconfdir>/xdg/openxr/1, i.e. /etc/xdg/openxr/1 for prefix /usr)")

install(TARGETS puppetxr
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS puppetxrctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
# Windows has no package manager to make puppetxr the active runtime; the script sets the registry key
if (WIN32)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/tools/dist/install-runtime.ps1 DESTINATION .)
endif ()
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/puppetxr.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# The installed manifest points at the library relative to itself, so the install tree can be relocated.
set(puppetxr_manifest_dir ${CMAKE_INSTALL_DATADIR}/openxr/1)
file(RELATIVE_PATH puppetxr_library_relative ${CMAKE_INSTALL_PREFIX}/${puppetxr_manifest_dir} ${PUPPETXR_INSTALL_FULL_MODULEDIR}/$<TARGET_FILE_NAME:puppetxr>)
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/install/puppetxr.json CONTENT "{\n \"file_format_version\": \"1.0.0\",\n    \"runtime\": {\n        \"name\": \"puppetxr\",\n     \"library_path\": \"${puppetxr_library_relative}\"\n }\n}\n")
install(FILES ${CMAKE_BINARY_DIR}/install/puppetxr.json DESTINATION ${puppetxr_manifest_dir})

if (PUPPETXR_INSTALL_ACTIVE_RUNTIME)
    if (WIN32)
        message(WARNING "PUPPETXR_INSTALL_ACTIVE_RUNTIME: on Windows the active runtime is the registry value HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\\ActiveRuntime; see docs/building.md")
    else ()
        set(active_dir ${PUPPETXR_ACTIVE_RUNTIME_DIR})
        # The macOS loader reads only this one, with no per-user file (PlatformGetGlobalRuntimeFileName)
        if (NOT active_dir AND APPLE)
            set(active_dir /usr/local/share/openxr/1)
        elseif (NOT active_dir)
            set(active_dir ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/openxr/1)
        endif ()
        install(CODE "
            file(MAKE_DIRECTORY \"${active_dir}\")
            file(CREATE_LINK \"${CMAKE_INSTALL_PREFIX}/${puppetxr_manifest_dir}/puppetxr.json\" \"${active_dir}/active_runtime.json\" SYMBOLIC)
            message(STATUS \"Active OpenXR runtime: ${active_dir}/active_runtime.json -> puppetxr.json\")
        ")
    endif ()
endif ()
