#include "client/engine.hpp"
#include "shared/net/net.hpp"
#include "shared/protocol.hpp"

#include <SDL3/SDL.h>

// The server IP, port, and username are chosen in-game from the Connect menu.
int main()
{
    net::ScopedInit net_init; // must outlive the Engine (Session owns the net::Client)
    if (!net_init) {
        SDL_Log("failed to initialize networking");
        return 1;
    }

    auto engine = client::Engine::create(
      { .title = "Soulkeeper", .width = 1280, .height = 720, .fixed_hz = proto::sim_hz, .vsync = 0 });
    if (!engine) { return 1; }

    engine->run();
    return 0;
}
