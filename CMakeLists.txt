cmake_minimum_required(VERSION 3.20)
project(RE3HeadTracking VERSION 0.0.0 LANGUAGES CXX)

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

# Host-side unit tests. Off by default so normal/CI plugin builds are
# unaffected; enable with -DRE3HT_BUILD_TESTS=ON. Both suites are standalone -
# they pin core's camera math and the GUI dedup/memo behaviour against the
# pre-refactor implementations copied into the test itself - so neither links
# any plugin source.
option(RE3HT_BUILD_TESTS "Build RE3 head tracking host unit tests" OFF)
if(RE3HT_BUILD_TESTS)
    enable_testing()

    add_executable(re3ht_camera_math_test tests/camera_math_test.cpp)
    target_link_libraries(re3ht_camera_math_test PRIVATE cameraunlock)
    add_test(NAME re3ht_camera_math_test COMMAND re3ht_camera_math_test)

    add_executable(re3ht_perf_equivalence_test tests/perf_equivalence_test.cpp)
    target_link_libraries(re3ht_perf_equivalence_test PRIVATE cameraunlock)
    add_test(NAME re3ht_perf_equivalence_test COMMAND re3ht_perf_equivalence_test)
endif()
