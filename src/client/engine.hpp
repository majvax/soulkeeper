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

#include "client/audio.hpp"
#include "client/gui.hpp"
#include "client/mod/render_bindings.hpp"
#include "client/scene.hpp"
#include "client/session.hpp"
#include "client/ui.hpp"
#include "shared/mod/lua_host.hpp"

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
    // Bring up SDL + the window/renderer, then own a Session (connected later
    // from the Connect menu) and the scene stack. Move-only thereafter.
    [[nodiscard]] static std::expected<Engine, EngineError> create(const EngineConfig& config);

    // Non-owning handles for scenes.
    [[nodiscard]] SDL_Window* window() const noexcept { return window_.get(); }
    [[nodiscard]] SDL_Renderer* renderer() const noexcept { return renderer_.get(); }
    // Actual render output size (not the requested config size) — correct under
    // fullscreen / any aspect ratio, so the UI stays centered.
    [[nodiscard]] int width() const noexcept
    {
        int w = 0;
        int h = 0;
        SDL_GetRenderOutputSize(renderer_.get(), &w, &h);
        return w;
    }
    [[nodiscard]] int height() const noexcept
    {
        int w = 0;
        int h = 0;
        SDL_GetRenderOutputSize(renderer_.get(), &w, &h);
        return h;
    }
    [[nodiscard]] Session& session() noexcept { return session_; }
    [[nodiscard]] SceneManager& scenes() noexcept { return scenes_; }
    // Rebuild the pre-game stack [Lobby over Connect] (deferred). Lives on the
    // Engine (defined in engine.cpp) because scene headers can't name Connect/
    // Lobby without an include cycle — the Engine already owns the initial push.
    void reset_to_lobby();
    // The client's render VM: content metadata (labels/sprites) + draw hooks,
    // shared by the game and level-up scenes.
    [[nodiscard]] mod::LuaHost& mods() noexcept { return render_host_; }
    // The mixer. Always valid; silent mode if no audio device (never fatal).
    [[nodiscard]] Audio& audio() noexcept { return *audio_; }
    // The game's pixel-art widget kit (panels/buttons/text/inputs) — what
    // user-facing scenes draw with. ImGui remains for dev surfaces only.
    [[nodiscard]] Gui& gui() noexcept { return gui_; }

    void quit() noexcept { running_ = false; }
    [[nodiscard]] bool running() const noexcept { return running_; }

    // Run the fixed-timestep loop until quit() / SDL_EVENT_QUIT.
    void run();

private:
    Engine(SDLContext context, WindowPtr window, RendererPtr renderer, const EngineConfig& config)
      : context_{ std::move(context) }, window_{ std::move(window) }, renderer_{ std::move(renderer) },
        config_{ config }, ui_textures_{ std::make_unique<Textures>(renderer_.get()) },
        ui_layer_{ window_.get(), renderer_.get() }
    {
        // ui_textures_ is heap-allocated because Engine is MOVED out of
        // create(): Gui keeps this pointer, and a member address would dangle.
        gui_.init(renderer_.get(), window_.get(), ui_textures_.get());
    }

    void on_event(const SDL_Event& event);
    void render(float alpha);

    // Declaration order matters for teardown (members destruct in reverse):
    //  * ui/session/renderer/window destroy before context_ runs SDL_Quit;
    //  * render_host_ (the render VM) MUST outlive scenes_ — scenes hold sol
    //    references (e.g. GameScene's draw-context object) that unref into its
    //    Lua state on destruction.
    SDLContext context_;
    WindowPtr window_;
    RendererPtr renderer_;
    EngineConfig config_;
    // unique_ptr: Audio pins raw SDL stream handles, and Engine is moved out of
    // create() — the indirection keeps those handles stable across the move.
    std::unique_ptr<Audio> audio_ = std::make_unique<Audio>();
    std::unique_ptr<Textures> ui_textures_; // widget-kit sprites (heap: Gui holds the pointer across the move)
    Gui gui_;                               // the pixel-art widget kit (init'd in the ctor body)
    mod::LuaHost render_host_; // render VM: content metadata + draw hooks
    Session session_;
    SceneManager scenes_;
    ImGuiLayer ui_layer_;
    bool running_ = false;
};

} // namespace client
