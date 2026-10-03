// Vita skeleton of LoadingScreen (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/loading_screen.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/loading_screen.hpp"

#include "core/window.hpp"
#include "rendering/imgui_backend.hpp"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <vitaGL.h>

namespace wowee::rendering {

namespace { core::Window* g_window = nullptr; }

void LoadingScreen::attachWindow(core::Window* window) { g_window = window; }

bool LoadingScreen::initialize() { return true; }

LoadingScreen::~LoadingScreen() = default;

LoadingScreen::LoadingScreen() {}

// A plain progress screen: the status text and a bar, no background picture yet (VITA-16).
void LoadingScreen::render() {
    if (!g_window || !g_window->getImGuiBackend()) return;
    g_window->getImGuiBackend()->newFrame();
    ImGui::NewFrame();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##loading", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoBackground);
    ImGui::SetCursorPos(ImVec2(size.x * 0.1f, size.y * 0.45f));
    ImGui::Text("%s", statusText.c_str());
    ImGui::SetCursorPosX(size.x * 0.1f);
    ImGui::ProgressBar(loadProgress, ImVec2(size.x * 0.8f, 18.0f));
    ImGui::End();
    ImGui::Render();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    vglSwapBuffers(GL_FALSE);
}

void LoadingScreen::shutdown() { }

}  // namespace wowee::rendering
