// src/client/engine.cpp
#include "client/engine.hpp"

#include "client/scene/lobby.hpp" // the initial scene
#include "core/timestep.hpp"

namespace client {

std::expected<Engine, EngineError> Engine::create(const EngineConfig& config, std::string host, std::string name)
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

    return Engine{ std::move(context),   std::move(window), std::move(renderer),
                   config,                std::move(host),   std::move(name) };
}

void Engine::on_event(const SDL_Event& event)
{
    ui_layer_.process_event(event);
    scenes_.handle_event(event);
    if (event.type == SDL_EVENT_QUIT) { quit(); }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) { quit(); }
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

    session_.connect();
    scenes_.push<LobbyScene>(this); // the only hardcoded scene; the rest self-drive
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

        while (timestep.consume()) { scenes_.update(timestep.dt()); }
        scenes_.apply_pending(); // transitions requested from update (level-up, lobby->game)

        render(timestep.alpha());
    }
}

} // namespace client
