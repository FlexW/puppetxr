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
# Tests with a wall-clock upper bound run alone: beside other tests on a software renderer or a
# small machine they measure the load, not the code
set(puppetxr_timed_tests "Platform.window_thread_calls_return_at_once:Platform.posted_window_thread_calls_run_at_once:Platform.window_calls_from_other_threads_run_on_the_main_thread:Platform.window_calls_the_main_thread_never_takes_are_withdrawn:Platform.a_main_thread_waiting_for_a_mutex_serves_window_calls:Platform.a_waiting_main_thread_takes_a_mutex_once_it_is_free:Platform.showing_a_maximized_window*:Profile.zones_cost_next_to_nothing_while_stopped:OxrTest.a_second_wait_without_begin_times_out_with_a_warning:OxrFrameFaultTest.a_failed_begin_begins_no_frame:OxrThreadWindowedTest.*:OxrVulkanRealWindowTest.frames_never_wait_for_the_display:OxrVulkanRealWindowTest.the_mirror_presents_at_most_at_the_display_rate")
gtest_discover_tests(puppetxr_tests DISCOVERY_TIMEOUT 60 TEST_FILTER "-${puppetxr_real_window_tests}:${puppetxr_timed_tests}"
    PROPERTIES ENVIRONMENT "PUPPETXR_STATE_DIR=${CMAKE_BINARY_DIR}/test_state;${PUPPETXR_TEST_ENVIRONMENT}")
gtest_discover_tests(puppetxr_tests DISCOVERY_TIMEOUT 60 TEST_FILTER "${puppetxr_real_window_tests}-${puppetxr_timed_tests}" TEST_LIST puppetxr_real_window_tests
    PROPERTIES ENVIRONMENT "PUPPETXR_STATE_DIR=${CMAKE_BINARY_DIR}/test_state;${PUPPETXR_TEST_ENVIRONMENT}" RESOURCE_LOCK real_window)
gtest_discover_tests(puppetxr_tests DISCOVERY_TIMEOUT 60 TEST_FILTER "${puppetxr_timed_tests}" TEST_LIST puppetxr_timed_tests_found
    PROPERTIES ENVIRONMENT "PUPPETXR_STATE_DIR=${CMAKE_BINARY_DIR}/test_state;${PUPPETXR_TEST_ENVIRONMENT}" RUN_SERIAL TRUE)

# The sample driver fails a test whose app is missing instead of waiting for it (docs/testing.md)
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_FOUND)
    set(puppetxr_sample_driver ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/sample_tests.py --samples ${CMAKE_CURRENT_BINARY_DIR}/no_samples
        --runtime none --puppetxrctl none --references ${CMAKE_SOURCE_DIR}/tests/samples --out ${CMAKE_CURRENT_BINARY_DIR}/sample_driver_out)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/no_samples)
    add_test(NAME sample_driver_fails_tests_of_a_missing_app COMMAND ${puppetxr_sample_driver} --tests unity_mruk_menu,unity_mruk_mrukbase --jobs 2)
    set_tests_properties(sample_driver_fails_tests_of_a_missing_app PROPERTIES PASS_REGULAR_EXPRESSION "0 of 2 passed" TIMEOUT 30)
    add_test(NAME sample_driver_runs_only_built_apps COMMAND ${puppetxr_sample_driver})
    set_tests_properties(sample_driver_runs_only_built_apps PROPERTIES PASS_REGULAR_EXPRESSION "no sample app is built" TIMEOUT 30)
    # llvmpipe draws a few pixels of the AMD references differently on Intel CPUs
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/cpuinfo_intel "processor\t: 0\nvendor_id\t: GenuineIntel\n")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/cpuinfo_amd "processor\t: 0\nvendor_id\t: AuthenticAMD\n")
    add_test(NAME sample_driver_takes_the_intel_tolerance_on_intel COMMAND ${puppetxr_sample_driver} --tolerance 1 --intel-tolerance 3 --cpuinfo ${CMAKE_CURRENT_BINARY_DIR}/cpuinfo_intel)
    set_tests_properties(sample_driver_takes_the_intel_tolerance_on_intel PROPERTIES PASS_REGULAR_EXPRESSION "tolerance 3 on GenuineIntel" TIMEOUT 30)
    add_test(NAME sample_driver_keeps_its_tolerance_elsewhere COMMAND ${puppetxr_sample_driver} --tolerance 1 --intel-tolerance 3 --cpuinfo ${CMAKE_CURRENT_BINARY_DIR}/cpuinfo_amd)
    set_tests_properties(sample_driver_keeps_its_tolerance_elsewhere PROPERTIES PASS_REGULAR_EXPRESSION "tolerance 1 on AuthenticAMD" TIMEOUT 30)
endif ()
