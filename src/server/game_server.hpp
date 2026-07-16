#pragma once
#include "core/timestep.hpp"
#include "shared/mod/lua_host.hpp"
#include "shared/mod/sim_bindings.hpp"
#include "shared/net/net.hpp"
#include "shared/protocol.hpp"
#include "shared/sim/world.hpp"
#include "shared/snapshot_codec.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    bool was_downed = false; // for the on_player_downed rising edge
    // Newest snapshot tick this client applied (from Input.ack_tick) — the
    // delta baseline. 0 = never acked: keep sending full snapshots.
    std::uint32_t last_ack_tick = 0;
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
    void on_lua_command(std::uint32_t peer_id, proto::ByteReader& reader); // mod:command dispatch
    void on_select(std::uint32_t peer_id, proto::ByteReader& reader);

    void broadcast_roster();
    void send_state(std::uint32_t peer_id);
    void stream_snapshots(); // capture SnapshotState, then per-peer full/delta

    void record_tick_time(double ms); // rolling avg/max log + over-budget warnings

    void spawn_enemies(float dt);
    void refresh_spawn_weights(std::uint16_t wave); // re-evaluate enemy defs' weight(wave)
    void check_level_up();
    void check_chest();          // ChestOpen mailbox -> one objects-only offer round for everyone
    void check_offer_grants();   // OfferGrant mailbox -> one forced round (/upgrade, /object)
    void begin_offer_round(proto::OfferFlavor flavor); // freeze + roll+send cards to every player
    void check_run_end(); // RunEnd mailbox -> freeze + GameOver broadcast
    void reset_run();     // host BackToLobby -> wipe the run, back to Lobby
    void start_level_up_for(std::uint64_t token); // roll fresh cards (Lua hook first), store, send
    void send_level_up(std::uint64_t token);      // (re)send the already-stored offer

    // Sim-event emission (mod hooks). on_enemy_death is detected by diffing the
    // enemy set around world_.step(); on_player_downed by a per-session edge.
    void snapshot_enemies();      // fill pre_step_enemies_ before stepping
    void emit_enemy_deaths();     // emit on_enemy_death for enemies gone after step
    void emit_downed_transitions(); // emit on_player_downed on the rising edge

    struct EnemyDeathSnap
    {
        core::Entity entity;
        float x, y;
        std::uint8_t variant;
        std::uint32_t xp;
    };
    std::vector<EnemyDeathSnap> pre_step_enemies_;
    // Engine fallback roll (fixed 3 cards) — used when no mod:level_offer hook
    // is registered or the hook returned nothing usable.
    [[nodiscard]] std::vector<proto::LevelUpChoice> roll_upgrades(core::Entity player);

    net::Server server_;
    // lua_host_ MUST be declared before world_: the World's Lua-defined systems
    // hold sol references, so the World must destruct before the Lua state.
    mod::LuaHost lua_host_; // sim VM: content + script components/systems (SDL-free)
    shared::World world_;
    proto::GameState state_ = proto::GameState::Lobby;
    std::unordered_map<std::uint64_t, Session> sessions_; // token -> session
    std::unordered_map<std::uint32_t, std::uint64_t> peer_token_;
    std::uint64_t host_token_ = 0;
    bool have_host_ = false;
    bool paused_ = false;
    bool run_over_ = false; // game-over screen up; waiting for host BackToLobby
    proto::GameOverMsg run_over_msg_{}; // cached verdict, re-sent to reconnecters
    // Cached per-player scoreboard (RunStats at run end), sent after the msg.
    std::vector<proto::GameOverEntry> run_over_entries_;
    void put_game_over(proto::ByteWriter& writer) const; // verdict + scoreboard block
    std::uint32_t tick_ = 0;
    float spawn_timer_ = 0.0f;
    float wave_timer_ = 0.0f;

    // Snapshot history ring: the last N captured states, the delta baselines
    // clients can ack (32 @ 60 Hz ≈ 0.5 s — an ack older than that gets fulls).
    static constexpr std::size_t snapshot_history_len = 32;
    std::deque<proto::SnapshotState> snapshot_history_;

    // Tick-time telemetry window (record_tick_time).
    double tick_ms_sum_ = 0.0;
    double tick_ms_max_ = 0.0;
    std::uint32_t tick_ms_count_ = 0;
    std::uint32_t tick_ms_over_ = 0;
    std::uint64_t snapshot_bytes_sent_ = 0; // window total, logged as kB/s

    // Spawn table for the current wave: Lua weight/component-init callbacks run
    // once per wave (refresh_spawn_weights), the per-spawn hot path just samples
    // the cached distribution. Parallel arrays indexed by the same roll.
    std::vector<std::uint8_t> spawn_variants_;
    std::vector<std::vector<std::pair<const mod::ComponentRef*, sol::table>>> spawn_inits_;
    std::discrete_distribution<std::size_t> spawn_dist_;
    std::uint16_t spawn_weights_wave_ = 0; // 0 = never refreshed (waves start at 1)

    // Shared progression + synchronized level-up.
    std::uint16_t level_ = 1;
    std::uint32_t xp_needed_ = 8;
    bool leveling_ = false;
    // What the CURRENT offer round is (level-up vs boss chest): rolled into
    // every LevelUp packet so the client can theme the pick scene, and passed
    // to mod:level_offer as its context ("level"/"chest"). Round-wide, so a
    // mid-round reconnect re-sends the right flavor.
    proto::OfferFlavor offer_flavor_ = proto::OfferFlavor::Level;
    // Keyed by session TOKEN (stable across reconnects), NOT peer_id (which
    // changes on every reconnect) — otherwise a reconnecting player is orphaned
    // in pending_ and the level-up never completes (world freezes).
    std::unordered_set<std::uint64_t> pending_;                                  // tokens still choosing
    std::unordered_map<std::uint64_t, std::vector<proto::LevelUpChoice>> offered_; // token -> cards (2..5)
    std::unordered_set<std::uint64_t> chosen_;                                   // tokens that already picked this level
    std::mt19937 rng_{ std::random_device{}() };
    // Per-run terrain seed: rolled at start, zeroed on reset (0 = flat lobby
    // world). Written into the sim's Terrain singleton and every StateMsg.
    std::uint32_t world_seed_ = 0;
};

} // namespace server
