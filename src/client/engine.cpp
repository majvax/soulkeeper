// src/client/engine.cpp
#include "client/engine.hpp"

#include "client/scene/connect.hpp" // the initial scene (Connect menu)
#include "client/scene/lobby.hpp"   // reset_to_lobby rebuilds [Lobby over Connect]
#include "core/timestep.hpp"

namespace client {

void Engine::reset_to_lobby()
{
    scenes_.clear();
    scenes_.push<ConnectScene>(this);
    scenes_.push<LobbyScene>(this); // top: the session is still connected
}

std::expected<Engine, EngineError> Engine::create(const EngineConfig& config)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { return std::unexpected(EngineError::sdl_init_failed); }
    SDLContext context{ true };

    WindowPtr window{ SDL_CreateWindow(config.title, config.width, config.height,
                                       SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE) };
    if (!window) { return std::unexpected(EngineError::window_creation_failed); }

    RendererPtr renderer{ SDL_CreateRenderer(window.get(), nullptr) };
    if (!renderer) { return std::unexpected(EngineError::renderer_creation_failed); }

    // 0 uncaps the frame rate; failure here is non-fatal.
    SDL_SetRenderVSync(renderer.get(), config.vsync);

    return Engine{ std::move(context), std::move(window), std::move(renderer), config };
}

void Engine::on_event(const SDL_Event& event)
{
    ui_layer_.process_event(event);
    const bool unconsumed = scenes_.handle_event(event);
    if (event.type == SDL_EVENT_QUIT) { quit(); }
    // Global exit key — only when no scene claimed the event (modal scenes
    // like the console use ESC themselves).
    if (unconsumed && event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) { quit(); }
}

void Engine::render(float alpha)
{
    ui_layer_.begin_frame();
    scenes_.render(alpha);
    ui_layer_.end_frame(renderer_.get());
    SDL_RenderPresent(renderer_.get());
}

void Engine::run()
{
    running_ = true;

    // Bring up the render VM: register draw bindings, then load plugins. Same
    // plugin set as the server -> identical content wire ids.
    install_render_bindings(render_host_);
    render_host_.load_dir("mods");

    // Mod sound bindings (mod:sound) land in the mixer before anything plays;
    // in-order application = last declaration wins, like player_sprite.
    for (const auto& [name, path] : render_host_.state().sounds) {
        audio_->set_override(name, path);
    }

    session_.set_mods_hash(render_host_.plugin_hash()); // sent in Join; must match the server
    scenes_.push<ConnectScene>(this); // entry menu; connect happens on Join, the rest self-drive
    scenes_.apply_pending();

    core::FixedTimestep timestep{ config_.fixed_hz };
    const auto freq = static_cast<double>(SDL_GetPerformanceFrequency());
    Uint64 previous = SDL_GetPerformanceCounter();

    while (running_) {
        const Uint64 now = SDL_GetPerformanceCounter();
        const double frame_seconds = static_cast<double>(now - previous) / freq;
        previous = now;
        timestep.add_time(frame_seconds);

        SDL_Event event;
        while (SDL_PollEvent(&event)) { on_event(event); }
        scenes_.apply_pending(); // transitions requested from handle_event (e.g. TAB)

        session_.poll(static_cast<float>(frame_seconds));
        audio_->update(static_cast<float>(frame_seconds)); // music fade + loop refill

        while (timestep.consume()) { scenes_.update(timestep.dt()); }
        scenes_.apply_pending(); // transitions requested from update (level-up, lobby->game)

        render(timestep.alpha());
    }
}

} // namespace client
