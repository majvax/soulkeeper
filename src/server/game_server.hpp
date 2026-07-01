#pragma once
#include "core/timestep.hpp"
#include "shared/net/net.hpp"
#include "shared/progression/upgrades.hpp"
#include "shared/protocol.hpp"
#include "shared/sim/world.hpp"

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace server {

// One connected (or recently-disconnected) player, keyed by its client token so
// a reconnecting client resumes the same entity instead of getting a fresh one.
struct Session
{
    core::Entity entity;
    std::string name;
    bool is_host = false;
    std::uint32_t peer_id = 0;
    bool connected = false;
};

// Authoritative game server: owns the simulation + all session bookkeeping.
// main() just feeds it time and calls poll()/update().
class GameServer
{
public:
    explicit GameServer(net::Server server);

    // Drain and dispatch all pending network events.
    void poll();
    // Advance the simulation and stream snapshots while playing.
    void update(core::FixedTimestep& timestep);

private:
    void on_join(std::uint32_t peer_id, proto::ByteReader& reader);
    void on_disconnect(std::uint32_t peer_id);
    void on_start(std::uint32_t peer_id);
    void on_input(std::uint32_t peer_id, proto::ByteReader& reader);
    void on_command(std::uint32_t peer_id, proto::ByteReader& reader);
    void on_select(std::uint32_t peer_id, proto::ByteReader& reader);

    void broadcast_roster();
    void send_state(std::uint32_t peer_id);
    void broadcast_snapshot();

    void spawn_enemies(float dt);
    void check_level_up();
    void start_level_up_for(std::uint32_t peer_id);
    [[nodiscard]] std::array<proto::LevelUpChoice, proto::level_up_choices> roll_upgrades(core::Entity player);

    net::Server server_;
    shared::World world_;
    proto::GameState state_ = proto::GameState::Lobby;
    std::unordered_map<std::uint64_t, Session> sessions_; // token -> session
    std::unordered_map<std::uint32_t, std::uint64_t> peer_token_;
    std::uint64_t host_token_ = 0;
    bool have_host_ = false;
    bool paused_ = false;
    std::uint32_t tick_ = 0;
    float spawn_timer_ = 0.0f;
    float wave_timer_ = 0.0f;

    // Shared progression + synchronized level-up.
    std::uint16_t level_ = 1;
    std::uint32_t xp_needed_ = 8;
    bool leveling_ = false;
    std::unordered_set<std::uint32_t> pending_;                                             // peers still choosing
    std::unordered_map<std::uint32_t, std::array<proto::LevelUpChoice, proto::level_up_choices>> offered_;
    std::mt19937 rng_{ std::random_device{}() };
};

} // namespace server
