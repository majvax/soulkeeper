#include "core/timestep.hpp"
#include "server/game_server.hpp"
#include "shared/net/net.hpp"
#include "shared/protocol.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

// Authoritative server entry point: bring up networking, then run the fixed-rate
// loop. All the game/session logic lives in server::GameServer.
int main(int argc, char** argv)
{
    const std::uint16_t port =
      (argc > 1) ? static_cast<std::uint16_t>(std::atoi(argv[1])) : proto::default_port;

    net::ScopedInit net_init;
    if (!net_init) {
        spdlog::error("failed to initialize networking");
        return 1;
    }
    auto server_opt = net::Server::create(port, proto::max_players);
    if (!server_opt) {
        spdlog::error("failed to create server on port {}", port);
        return 1;
    }
    spdlog::info("Soulkeeper server listening on port {}", port);

    server::GameServer game{ std::move(*server_opt) };

    core::FixedTimestep timestep{ proto::sim_hz };
    auto previous = std::chrono::steady_clock::now();
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        timestep.add_time(std::chrono::duration<double>(now - previous).count());
        previous = now;

        game.poll();
        game.update(timestep);

        std::this_thread::sleep_for(std::chrono::duration<double>(timestep.time_until_next()));
    }
}
