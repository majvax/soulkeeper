// src/client/ui.hpp
//
// Dear ImGui binding setup for the SDL3 renderer backend. Wraps the ImGui
// context + platform/renderer backends in a single RAII object so the debug UI
// is initialized and torn down in the right order relative to the SDL window.
//
// Lifetime ordering: an ImGuiLayer must be destroyed BEFORE the SDL renderer /
// window it was created against. Declare it after the Engine in main() so it
// shuts down first.

#pragma once

#include <utility>

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

namespace client {

class ImGuiLayer {
public:
    // Stand up the ImGui context and the SDL3 + SDLRenderer3 backends.
    ImGuiLayer(SDL_Window* window, SDL_Renderer* renderer) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // docking branch
        ImGui::StyleColorsDark();

        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);
        active_ = true;
    }

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    ImGuiLayer(ImGuiLayer&& other) noexcept : active_{other.active_} {
        other.active_ = false;
    }
    ImGuiLayer& operator=(ImGuiLayer&& other) noexcept {
        if (this != &other) {
            shutdown();
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }

    ~ImGuiLayer() { shutdown(); }

    // Feed an SDL event to ImGui (so it can claim mouse/keyboard focus).
    void process_event(const SDL_Event& event) const noexcept {
        ImGui_ImplSDL3_ProcessEvent(&event);
    }

    // Start a new ImGui frame; issue ImGui widget calls after this.
    void begin_frame() const noexcept {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    // Render the accumulated ImGui draw data onto the given renderer. Call after
    // the world has been drawn but before SDL_RenderPresent.
    void end_frame(SDL_Renderer* renderer) const noexcept {
        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    }

private:
    void shutdown() noexcept {
        if (!active_) {
            return;
        }
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        active_ = false;
    }

    bool active_ = false;
};

} // namespace client
