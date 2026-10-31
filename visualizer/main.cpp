#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include "app.h"
#include "config.h"

#ifndef FC_DEFAULT_CONFIG
#define FC_DEFAULT_CONFIG "config.ini"
#endif

namespace {

void OnGlfwError(int code, const char* text) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, text);
}

void OnDrop(GLFWwindow* window, int count, const char** paths) {
    if (count > 0) {
        static_cast<viz::App*>(glfwGetWindowUserPointer(window))->OnFileDropped(paths[0]);
    }
}

}  // namespace

// Usage: fc_visualizer [path/to/config.ini]
int main(int argc, char** argv) {
    const std::string configPath = argc > 1 ? argv[1] : FC_DEFAULT_CONFIG;
    std::vector<std::string> messages;
    const viz::Config config = viz::LoadConfig(configPath, messages);
    for (const std::string& message : messages) {
        std::fprintf(stderr, "%s\n", message.c_str());
    }

    glfwSetErrorCallback(OnGlfwError);
    if (!glfwInit()) {
        return 1;
    }

#if defined(__APPLE__)
    // macOS only gives a modern OpenGL context when asked like this.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    const float scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    GLFWwindow* window = glfwCreateWindow(static_cast<int>(1280 * scale), static_cast<int>(800 * scale),
                                          "FileCollector visualizer", nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // do not save window positions to imgui.ini
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(nullptr);

    {
        viz::App app(config, configPath, messages);
        glfwSetWindowUserPointer(window, &app);
        glfwSetDropCallback(window, OnDrop);

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
                ImGui_ImplGlfw_Sleep(10);
                continue;
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.Draw();
            ImGui::Render();

            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }

        glfwSetDropCallback(window, nullptr);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
