# Transfer logic without any UI. The tests use it too.
add_library(fc_transfer STATIC
    config.cpp
    transfer.cpp
)
target_include_directories(fc_transfer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fc_transfer PUBLIC file_collector)
if(NOT MSVC)
    target_compile_options(fc_transfer PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT FC_BUILD_VISUALIZER)
    return()
endif()

include(FetchContent)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.5.1
    GIT_SHALLOW TRUE
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.93.0
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(glfw imgui)
find_package(OpenGL REQUIRED)

# ImGui has no CMakeLists.txt, so we build it from its sources.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
if(APPLE)
    target_compile_definitions(imgui PUBLIC GL_SILENCE_DEPRECATION)
endif()

add_executable(fc_visualizer main.cpp app.cpp)
target_link_libraries(fc_visualizer PRIVATE fc_transfer imgui)
# The app reads this config.ini by default, so edits here work without rebuilding.
target_compile_definitions(fc_visualizer PRIVATE FC_DEFAULT_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/config.ini")
if(NOT MSVC)
    target_compile_options(fc_visualizer PRIVATE -Wall -Wextra)
endif()
