#include "client/engine.hpp"
#include "shared/net/net.hpp"
#include "shared/protocol.hpp"

#include <SDL3/SDL.h>
#include <string>

// Usage: client [host] [name]   (defaults: 127.0.0.1, "Player")
int main(int argc, char* argv[])
{
    const std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    const std::string name = (argc > 2) ? argv[2] : "Player";

    net::ScopedInit net_init; // must outlive the Engine (Session owns the net::Client)
    if (!net_init) {
        SDL_Log("failed to initialize networking");
        return 1;
    }

    auto engine = client::Engine::create(
      { .title = "Soulkeeper", .width = 1280, .height = 720, .fixed_hz = proto::sim_hz, .vsync = 0 }, host, name);
    if (!engine) { return 1; }

    engine->run();
    return 0;
}
