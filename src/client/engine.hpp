// src/client/engine.hpp
//
// The client application. Owns the SDL subsystems, window, renderer, the network
// Session, the scene stack, and the ImGui layer, and drives a fixed-timestep
// loop ("Fix Your Timestep!"). Scenes reach the Session / scene stack through
// the Engine and drive their own transitions.
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>

#include <SDL3/SDL.h>

#include "client/scene.hpp"
#include "client/session.hpp"
#include "client/ui.hpp"

namespace client {

enum class EngineError : std::uint8_t {
    sdl_init_failed,
    window_creation_failed,
    renderer_creation_failed,
};

struct SDLDeleter
{
    void operator()(SDL_Window* window) const noexcept
    {
        if (window != nullptr) { SDL_DestroyWindow(window); }
    }
    void operator()(SDL_Renderer* renderer) const noexcept
    {
        if (renderer != nullptr) { SDL_DestroyRenderer(renderer); }
    }
};

using WindowPtr = std::unique_ptr<SDL_Window, SDLDeleter>;
using RendererPtr = std::unique_ptr<SDL_Renderer, SDLDeleter>;

// Owns the global SDL_Init/SDL_Quit pairing. Move-only: only the live instance
// calls SDL_Quit, exactly once, on destruction.
class SDLContext
{
public:
    SDLContext() noexcept = default;
    explicit SDLContext(bool active) noexcept : active_{ active } {}

    SDLContext(const SDLContext&) = delete;
    SDLContext& operator=(const SDLContext&) = delete;
    SDLContext(SDLContext&& other) noexcept : active_{ other.active_ } { other.active_ = false; }
    SDLContext& operator=(SDLContext&& other) noexcept
    {
        if (this != &other) {
            if (active_) { SDL_Quit(); }
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }
    ~SDLContext()
    {
        if (active_) { SDL_Quit(); }
    }

private:
    bool active_ = false;
};

struct EngineConfig
{
    const char* title;
    int width;
    int height;
    double fixed_hz; // simulation ticks per second
    int vsync;
};

// The client application object.
class Engine
{
public:
    // Bring up SDL + the window/renderer, then own a Session (connecting to
    // `host` as `name`) and the scene stack. Move-only thereafter.
    [[nodiscard]] static std::expected<Engine, EngineError> create(const EngineConfig& config,
                                                                   std::string host, std::string name);

    // Non-owning handles for scenes.
    [[nodiscard]] SDL_Window* window() const noexcept { return window_.get(); }
    [[nodiscard]] SDL_Renderer* renderer() const noexcept { return renderer_.get(); }
    [[nodiscard]] int width() const noexcept { return config_.width; }
    [[nodiscard]] int height() const noexcept { return config_.height; }
    [[nodiscard]] Session& session() noexcept { return session_; }
    [[nodiscard]] SceneManager& scenes() noexcept { return scenes_; }

    void quit() noexcept { running_ = false; }
    [[nodiscard]] bool running() const noexcept { return running_; }

    // Run the fixed-timestep loop until quit() / SDL_EVENT_QUIT.
    void run();

private:
    Engine(SDLContext context, WindowPtr window, RendererPtr renderer, const EngineConfig& config,
           std::string host, std::string name)
      : context_{ std::move(context) }, window_{ std::move(window) }, renderer_{ std::move(renderer) },
        config_{ config }, session_{ std::move(host), std::move(name) },
        ui_layer_{ window_.get(), renderer_.get() }
    {}

    void on_event(const SDL_Event& event);
    void render(float alpha);

    // Declaration order matters for teardown: ui/session/renderer/window are
    // destroyed before context_ runs SDL_Quit (members destruct in reverse).
    SDLContext context_;
    WindowPtr window_;
    RendererPtr renderer_;
    EngineConfig config_;
    Session session_;
    SceneManager scenes_;
    ImGuiLayer ui_layer_;
    bool running_ = false;
};

} // namespace client
