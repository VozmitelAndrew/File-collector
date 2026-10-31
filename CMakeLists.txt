cmake_minimum_required(VERSION 3.20)
project(FileCollector VERSION 1.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FC_TRACKER "intervals" CACHE STRING "Coverage tracker: intervals | bitmap")
set_property(CACHE FC_TRACKER PROPERTY STRINGS intervals bitmap)

set(FC_SANITIZER "none" CACHE STRING "Sanitizer: none | thread | address")
set_property(CACHE FC_SANITIZER PROPERTY STRINGS none thread address)

option(FC_BUILD_TESTS "Build unit tests" ON)
option(FC_BUILD_VISUALIZER "Build the ImGui visualizer (downloads GLFW and ImGui)" OFF)

if(FC_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(FC_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(NOT FC_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "FC_SANITIZER must be 'none', 'thread' or 'address'")
endif()

find_package(Threads REQUIRED)

add_library(file_collector STATIC
    src/interval_tracker.cpp
    src/bitmap_tracker.cpp
)
add_library(FileCollector::file_collector ALIAS file_collector)

target_include_directories(file_collector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(file_collector PUBLIC cxx_std_20)
target_link_libraries(file_collector PUBLIC Threads::Threads)

if(NOT MSVC)
    target_compile_options(file_collector PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(FC_TRACKER STREQUAL "bitmap")
    target_compile_definitions(file_collector PUBLIC FC_TRACKER_BITMAP)
elseif(NOT FC_TRACKER STREQUAL "intervals")
    message(FATAL_ERROR "FC_TRACKER must be 'intervals' or 'bitmap'")
endif()

message(STATUS "FileCollector tracker: ${FC_TRACKER}")
message(STATUS "FileCollector sanitizer: ${FC_SANITIZER}")

if(FC_BUILD_TESTS OR FC_BUILD_VISUALIZER)
    add_subdirectory(visualizer)
endif()

if(FC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
