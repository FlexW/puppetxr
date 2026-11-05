add_executable(puppetxr_example puppetxr_example.cpp)
target_compile_features(puppetxr_example PRIVATE cxx_std_20)
target_link_libraries(puppetxr_example PRIVATE openxr_loader volk Vulkan::Headers)
target_include_directories(puppetxr_example PRIVATE ${CMAKE_SOURCE_DIR}/src)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_link_libraries(puppetxr_example PRIVATE ${CMAKE_DL_LIBS})
endif ()
puppetxr_apply_flags(puppetxr_example)
add_dependencies(puppetxr_example puppetxr)

# Body tracking as an app sees it, to compare puppetxr with a headset (docs/body-tracking.md)
add_executable(body_dump body_dump.cpp)
target_compile_features(body_dump PRIVATE cxx_std_20)
target_link_libraries(body_dump PRIVATE openxr_loader volk Vulkan::Headers)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_link_libraries(body_dump PRIVATE ${CMAKE_DL_LIBS})
endif ()
puppetxr_apply_flags(body_dump)
add_dependencies(body_dump puppetxr)

# Eye tracking as an app sees it, to compare puppetxr with a headset (docs/face-tracking.md)
add_executable(eye_dump eye_dump.cpp)
target_compile_features(eye_dump PRIVATE cxx_std_20)
target_link_libraries(eye_dump PRIVATE openxr_loader volk Vulkan::Headers)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_link_libraries(eye_dump PRIVATE ${CMAKE_DL_LIBS})
endif ()
puppetxr_apply_flags(eye_dump)
add_dependencies(eye_dump puppetxr)

if (PUPPETXR_BUILD_TESTS)
    add_test(NAME example_e2e
        COMMAND ${CMAKE_COMMAND} "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}" -DAPP=$<TARGET_FILE:puppetxr_example> -DRUNTIME_JSON=${CMAKE_BINARY_DIR}/puppetxr-dev.json -DOUT=${CMAKE_BINARY_DIR}/example_e2e -P ${CMAKE_CURRENT_SOURCE_DIR}/example_e2e.cmake)
    add_test(NAME install_relocated
        COMMAND ${CMAKE_COMMAND} "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}" -DAPP=$<TARGET_FILE:puppetxr_example> -DBUILD=${CMAKE_BINARY_DIR} -DOUT=${CMAKE_BINARY_DIR}/install_relocated -DVERSION=${PROJECT_VERSION} -P ${CMAKE_CURRENT_SOURCE_DIR}/install_relocated.cmake)
    set_tests_properties(example_e2e install_relocated PROPERTIES ENVIRONMENT "${PUPPETXR_TEST_ENVIRONMENT}")
    # The joints of a fixed-step run are the same on every platform; a change to the body model
    # updates tests/body_dump_puppetxr.json from the --out file after checking the difference
    add_test(NAME body_dump
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:body_dump> --frames 3 --out ${CMAKE_BINARY_DIR}/body_dump.json --expect ${CMAKE_SOURCE_DIR}/tests/body_dump_puppetxr.json)
    set_tests_properties(body_dump PROPERTIES ENVIRONMENT "${PUPPETXR_TEST_ENVIRONMENT};XR_RUNTIME_JSON=${CMAKE_BINARY_DIR}/puppetxr-dev.json;PUPPETXR_HEADLESS=1;PUPPETXR_FIXED_STEP=1;PUPPETXR_SHOW_WELCOME=0")
    # The gaze of a fixed-step run is the same on every platform; a change to the eyes or the face
    # updates tests/eye_dump_puppetxr.json from the --out file after checking the difference
    add_test(NAME eye_dump
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:eye_dump> --frames 3 --out ${CMAKE_BINARY_DIR}/eye_dump.json --expect ${CMAKE_SOURCE_DIR}/tests/eye_dump_puppetxr.json)
    set_tests_properties(eye_dump PROPERTIES ENVIRONMENT "${PUPPETXR_TEST_ENVIRONMENT};XR_RUNTIME_JSON=${CMAKE_BINARY_DIR}/puppetxr-dev.json;PUPPETXR_HEADLESS=1;PUPPETXR_FIXED_STEP=1;PUPPETXR_SHOW_WELCOME=0")
endif ()
