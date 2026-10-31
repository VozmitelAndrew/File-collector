include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(file_collector_tests
    tracker_test.cpp
    collector_test.cpp
    collector_mt_test.cpp
    transfer_test.cpp
)

target_link_libraries(file_collector_tests PRIVATE fc_transfer GTest::gtest_main)

if(NOT MSVC)
    target_compile_options(file_collector_tests PRIVATE -Wall -Wextra)
endif()

include(GoogleTest)
# PRE_TEST: tests are listed when ctest runs, not at build time,
# so a binary that cannot start (for example, a sanitizer runtime problem) does not break the build.
gtest_discover_tests(file_collector_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST)