add_executable(puppetxr_tests
    tests_base.cpp
    tests_json.cpp
    tests_zmath.cpp
    tests_sim.cpp
    tests_world.cpp
    tests_oxr.cpp
    tests_render.cpp
    tests_rec.cpp
    tests_api.cpp
    puppetxr_header.c
)
if (NOT PUPPETXR_HEADLESS_ONLY)
    target_sources(puppetxr_tests PRIVATE tests_ui.cpp)
endif ()
target_link_libraries(puppetxr_tests PRIVATE puppetxr_core GTest::gtest_main ${CMAKE_DL_LIBS})
target_compile_definitions(puppetxr_tests PRIVATE PUPPETXR_SOURCE_ROOMS_DIR="${CMAKE_SOURCE_DIR}/rooms" PUPPETXR_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}" PUPPETXR_CTL_PATH="$<TARGET_FILE:puppetxrctl>" PUPPETXR_TESTS_PATH="$<TARGET_FILE:puppetxr_tests>" PUPPETXR_TEST_VULKAN_LOADER="$<$<BOOL:${PUPPETXR_VULKAN_LOADER}>:${PUPPETXR_VULKAN_LOADER}>")
add_dependencies(puppetxr_tests puppetxrctl)
if (MSVC)
    # MSVC has no C89 mode; the GCC and clang builds check that
    set_source_files_properties(puppetxr_header.c PROPERTIES COMPILE_OPTIONS "/W4;/WX")
else ()
    set_source_files_properties(puppetxr_header.c PROPERTIES COMPILE_OPTIONS "-std=c89;-Wall;-Wextra;-Werror;-pedantic;-Wdeclaration-after-statement;-Wno-missing-field-initializers")
endif ()
puppetxr_apply_flags(puppetxr_tests)
# clang defines _HAS_STATIC_RTTI=0 for -fno-rtti on the MSVC ABI, which hides the std::any that
# gtest prints; cl keeps it with /GR-
if (CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    target_compile_options(puppetxr_tests PRIVATE -frtti)
endif ()

include(GoogleTest)
# Windowed tests would otherwise pick up the remembered UI choices of whoever runs them
# Tests on windows of the platform's video driver take one lock: side by side they stole each
# other's keyboard focus and dropped keys
set(puppetxr_real_window_tests "*RealWindow*:PlatformAppEventsTest.*:Platform.showing_a_maximized_window*:*high_pixel_density*")
gtest_discover_tests(puppetxr_tests DISCOVERY_TIMEOUT 60 TEST_FILTER "-${puppetxr_real_window_tests}"
    PROPERTIES ENVIRONMENT "PUPPETXR_STATE_DIR=${CMAKE_BINARY_DIR}/test_state;${PUPPETXR_TEST_ENVIRONMENT}")
gtest_discover_tests(puppetxr_tests DISCOVERY_TIMEOUT 60 TEST_FILTER "${puppetxr_real_window_tests}" TEST_LIST puppetxr_real_window_tests
    PROPERTIES ENVIRONMENT "PUPPETXR_STATE_DIR=${CMAKE_BINARY_DIR}/test_state;${PUPPETXR_TEST_ENVIRONMENT}" RESOURCE_LOCK real_window)
