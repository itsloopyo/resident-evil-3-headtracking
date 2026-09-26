cmake_minimum_required(VERSION 3.20)
project(RE3HeadTracking VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# cameraunlock-core shared library (static) with REFramework utilities
set(CAMERAUNLOCK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CAMERAUNLOCK_BUILD_REFRAMEWORK ON CACHE BOOL "" FORCE)
set(CAMERAUNLOCK_REFRAMEWORK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern CACHE PATH "" FORCE)
add_subdirectory(cameraunlock-core/cpp ${CMAKE_BINARY_DIR}/cameraunlock-core)

# Plugin sources
add_library(RE3HeadTracking SHARED
    src/pch.cpp
    src/plugin_main.cpp
    src/camera/gui_compensation.cpp
    src/camera/game_state_detector.cpp
)

target_precompile_headers(RE3HeadTracking PRIVATE src/pch.h)

set_target_properties(RE3HeadTracking PROPERTIES
    OUTPUT_NAME "RE3HeadTracking"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

target_link_libraries(RE3HeadTracking PRIVATE cameraunlock cameraunlock_reframework ws2_32)

target_include_directories(RE3HeadTracking PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/extern
)

# Host-side tests. Off by default so normal/CI plugin builds are unaffected and
# the packager never finds a test binary; enable with -DRE3HT_BUILD_TESTS=ON, or
# run 'pixi run test'. The camera math and dedup suites link no plugin source:
# they pin core's camera math and core's shipped GUI-name dedup. The config
# suites hold the committed HeadTracking.ini to the canonical table, and the
# import of an old HeadTracking.ini to the published build that read it.
option(RE3HT_BUILD_TESTS "Build RE3 head tracking host unit tests" OFF)
if(RE3HT_BUILD_TESTS)
    enable_testing()

    # Every test target hangs off this aggregate, so run-tests.ps1 builds them
    # all with one --target and selects them all with one ctest filter.
    add_custom_target(re3ht_tests)

    add_executable(re3ht_camera_math_test tests/camera_math_test.cpp)
    target_link_libraries(re3ht_camera_math_test PRIVATE cameraunlock)
    add_test(NAME re3ht_camera_math_test COMMAND re3ht_camera_math_test)

    # cameraunlock_reframework, not cameraunlock: the dedup suite drives the
    # shipped LogGuiElementNameOnce rather than a copy of it.
    add_executable(re3ht_perf_equivalence_test tests/perf_equivalence_test.cpp)
    target_include_directories(re3ht_perf_equivalence_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/extern)
    target_link_libraries(re3ht_perf_equivalence_test PRIVATE
        cameraunlock cameraunlock_reframework ws2_32)
    add_test(NAME re3ht_perf_equivalence_test COMMAND re3ht_perf_equivalence_test)

    # Both suites report failure through `assert`, and a failing assert in an
    # MSVC Debug build opens a modal report box that blocks ctest instead of
    # failing it. perf_equivalence_test redirects that report to stderr itself;
    # the timeout is what stops a blocked run from hanging CI regardless.
    set_tests_properties(re3ht_camera_math_test re3ht_perf_equivalence_test
        PROPERTIES TIMEOUT 120)
    add_dependencies(re3ht_tests re3ht_camera_math_test re3ht_perf_equivalence_test)

    function(re3ht_add_config_test name)
        add_executable(${name} ${ARGN})
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(${name} PRIVATE cameraunlock cameraunlock_reframework)
        set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
        if(MSVC)
            target_compile_options(${name} PRIVATE /W4 /WX /permissive-)
        endif()
        add_test(NAME ${name} COMMAND ${name})
        add_dependencies(re3ht_tests ${name})
    endfunction()

    # The committed HeadTracking.ini against the table, and the toggles' saves.
    # `--render-config <path>` writes the committed file instead (pixi run render-config).
    re3ht_add_config_test(re3ht_config_tests tests/config_tests.cpp)
    target_compile_definitions(re3ht_config_tests PRIVATE
        RE3HT_COMMITTED_CONFIG=L"${CMAKE_CURRENT_SOURCE_DIR}/HeadTracking.ini")

    # The config differential test. The oracle is the dev build's reader (234785c), the newest
    # published build, copied byte for byte into tests/config_differential/oracle; renaming its
    # namespace lets it link beside core's. oracle_adapter.cpp copies its Config out for the test.
    set(RE3HT_ORACLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential/oracle)
    add_library(re3ht_config_oracle STATIC
        ${RE3HT_ORACLE_DIR}/src/core/config.cpp
        ${RE3HT_ORACLE_DIR}/src/core/logger.cpp
        tests/config_differential/oracle_adapter.cpp
    )
    target_include_directories(re3ht_config_oracle PRIVATE
        ${RE3HT_ORACLE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential)
    target_compile_definitions(re3ht_config_oracle PRIVATE RE3HT=re3ht_oracle)
    target_link_libraries(re3ht_config_oracle PRIVATE cameraunlock)

    re3ht_add_config_test(re3ht_config_differential tests/config_differential/differential_tests.cpp)
    target_include_directories(re3ht_config_differential PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential)
    target_compile_definitions(re3ht_config_differential PRIVATE
        RE3HT_DIFFERENTIAL_INPUTS="${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential/inputs"
        RE3HT_COMMITTED_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/HeadTracking.ini")
    target_link_libraries(re3ht_config_differential PRIVATE re3ht_config_oracle)

    # Core's canonical config lint over the committed file and every file the differential test
    # migrated, which it writes beside itself under migrated\.
    add_test(NAME re3ht_config_lint COMMAND node
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential/lint-migrated.mjs
        $<TARGET_FILE_DIR:re3ht_config_differential>/migrated)
    set_tests_properties(re3ht_config_lint PROPERTIES DEPENDS re3ht_config_differential)

    # The oracle, the inputs and the core sources the import and the oracle compile, against the
    # hashes tests/config_differential/frozen.tsv records.
    add_test(NAME re3ht_config_frozen COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential/check-frozen.ps1)
endif()
