#include "server/game_server.hpp"

#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/components/progression.hpp"
#include "shared/factory/enemy.hpp"
#include "shared/factory/player.hpp"
#include "shared/sim/game_world.hpp"
#include "shared/system/input.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

// Spawn / wave tuning.
constexpr float wave_duration = 25.0f;   // seconds per wave
constexpr float spawn_distance = 600.0f; // ring radius around a player
// Measured via /stress (2026-07): 600 enemies = 1.8 ms avg / <7 ms max tick
// (clean); 750 grazes the 8.33 ms budget on spike ticks; 1000 busts it.
constexpr std::size_t max_enemies = 600;

float random_angle()
{
    static std::mt19937 gen{ std::random_device{}() };
    std::uniform_real_distribution<float> dist{ 0.0f, 2.0f * std::numbers::pi_v<float> };
    return dist(gen);
}

// Seconds between spawns — shorter as the run progresses.
float spawn_interval_for(std::uint16_t wave)
{
    return std::max(0.4f, 1.6f - (static_cast<float>(wave) * 0.12f));
}

} // namespace

namespace server {

GameServer::GameServer(net::Server server) : server_{ std::move(server) }, world_{ shared::make_game_world() }
{
    // Bind the ECS into the sim VM, then load plugins (upgrades + objects).
    // Bindings capture &world_.registry(), which is stable for our lifetime.
    mod::install_sim_bindings(lua_host_, world_.registry());
    lua_host_.load_dir("mods");
    mod::install_script_systems(lua_host_, world_); // add Lua-defined systems to the pipeline
    // AFTER load_dir: the XP curve is game balance (mod:xp_curve, linear
    // engine fallback) — seeding earlier would miss the mod's hook.
    xp_needed_ = mod::run_xp_curve(lua_host_, level_);
    if (lua_host_.enemies().count() == 0) {
        spdlog::warn("no enemy archetypes registered (mods/core missing?) — waves will spawn nothing");
    }
    // Enemies are pure component bags — hold plugins to the kernel contract.
    for (const mod::EnemyDef& def : lua_host_.enemies().defs()) {
        bool has_health = false;
        bool has_radius = false;
        for (const mod::EnemyComponentInit& init : def.components) {
            has_health = has_health || init.ref.id == "Health";
            has_radius = has_radius || init.ref.id == "Radius";
        }
        if (!has_health || !has_radius) {
            spdlog::warn("enemy '{}' lacks {} — it won't die/collide properly", def.id,
                         !has_health ? "Health" : "Radius");
        }
    }
}

void GameServer::poll()
{
    for (const net::Event& ev : server_.poll()) {
        if (ev.type == net::EventType::Disconnect) {
            on_disconnect(ev.peer_id);
        } else if (ev.type == net::EventType::Receive) {
            proto::ByteReader reader(ev.payload);
            const auto type = reader.get<proto::MsgType>();
            if (type == proto::MsgType::Join) {
                on_join(ev.peer_id, reader);
            } else if (type == proto::MsgType::StartGame) {
                on_start(ev.peer_id);
            } else if (type == proto::MsgType::Input) {
                on_input(ev.peer_id, reader);
            } else if (type == proto::MsgType::Command) {
                on_command(ev.peer_id, reader);
            } else if (type == proto::MsgType::LuaCommand) {
                on_lua_command(ev.peer_id, reader);
            } else if (type == proto::MsgType::SelectUpgrade) {
                on_select(ev.peer_id, reader);
            }
        }
    }
}

void GameServer::update(core::FixedTimestep& timestep)
{
    while (timestep.consume()) {
        if (state_ != proto::GameState::Playing) { break; }
        const auto tick_start = std::chrono::steady_clock::now();
        // Frozen while paused or during a level-up selection, but keep streaming
        // the (frozen) world so clients stay consistent.
        if (!paused_ && !leveling_) {
            spawn_enemies(timestep.dt());
            snapshot_enemies();       // record the enemy set before stepping
            world_.step(timestep.dt());
            emit_enemy_deaths();      // any enemy gone after the step died
            emit_downed_transitions();
            check_level_up();
            check_run_end();      // mods may have called world:end_game this step
            check_chest();        // ...or opened a boss chest (skipped once run_over_)
            check_offer_grants(); // ...or a dev /upgrade /object grant (one per round)
        }
        if (tick_ % proto::snapshot_every_n_ticks == 0) { stream_snapshots(); }
        ++tick_;
        record_tick_time(std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - tick_start)
                           .count());
    }
}

// Rolling tick-time telemetry: a 5 s summary (avg/max ms + entity count) and a
// warning whenever a single tick blows the fixed-step budget — the yardstick
// for every sim optimization.
void GameServer::record_tick_time(double ms)
{
    constexpr double budget_ms = 1000.0 / proto::sim_hz;
    tick_ms_sum_ += ms;
    tick_ms_max_ = std::max(tick_ms_max_, ms);
    ++tick_ms_count_;
    if (ms > budget_ms) { ++tick_ms_over_; }
    if (tick_ms_count_ >= static_cast<std::uint32_t>(proto::sim_hz) * 5) {
        std::size_t entities = 0;
        world_.registry().view<Position>().each([&](core::Entity, const Position&) { ++entities; });
        const double snap_kbps = static_cast<double>(snapshot_bytes_sent_) / 1024.0 / 5.0;
        if (tick_ms_over_ > 0) {
            spdlog::warn("tick avg {:.2f}ms max {:.2f}ms ({} over {:.2f}ms budget) | {} entities | snap {:.1f} kB/s",
                         tick_ms_sum_ / tick_ms_count_, tick_ms_max_, tick_ms_over_, budget_ms,
                         entities, snap_kbps);
        } else {
            spdlog::info("tick avg {:.2f}ms max {:.2f}ms | {} entities | snap {:.1f} kB/s",
                         tick_ms_sum_ / tick_ms_count_, tick_ms_max_, entities, snap_kbps);
        }
        tick_ms_sum_ = 0.0;
        tick_ms_max_ = 0.0;
        tick_ms_count_ = 0;
        tick_ms_over_ = 0;
        snapshot_bytes_sent_ = 0;
    }
}

void GameServer::spawn_enemies(float dt)
{
    core::Registry& registry = world_.registry();

    // Boss arena: while any WaveHold entity lives (mods tag the boss itself),
    // the wave clock and natural spawning are frozen — the milestone doesn't
    // end until the players end it.
    bool held = false;
    registry.view<WaveHold>().each([&](core::Entity, const WaveHold&) { held = true; });
    if (held) { return; }

    // Advance the wave clock.
    std::uint16_t wave = 1;
    registry.view<GameStats>().each([&](core::Entity, GameStats& stats) {
        wave_timer_ += dt;
        if (wave_timer_ >= wave_duration) {
            wave_timer_ -= wave_duration;
            ++stats.wave;
            spdlog::info("wave {}", stats.wave);
            lua_host_.events().emit("on_wave_start", static_cast<int>(stats.wave));
        }
        wave = stats.wave;
    });

    spawn_timer_ += dt;
    if (spawn_timer_ < spawn_interval_for(wave)) { return; }
    spawn_timer_ = 0.0f;

    // Collect connected players to spawn near, and count current enemies.
    std::vector<Position> players;
    registry.view<PlayerTag, Position>().each(
      [&](core::Entity, const PlayerTag&, const Position& pos) { players.push_back(pos); });
    if (players.empty()) { return; }

    std::size_t enemy_count = 0;
    registry.view<EnemyTag>().each([&](core::Entity, const EnemyTag&) { ++enemy_count; });
    if (enemy_count >= max_enemies) { return; }

    if (wave != spawn_weights_wave_) { refresh_spawn_weights(wave); }
    if (spawn_variants_.empty()) { return; } // nothing weighted > 0 this wave

    const std::size_t roll = spawn_dist_(rng_);
    const mod::EnemyDef* def = lua_host_.enemies().by_wire(spawn_variants_[roll]);
    const Position& target = players[enemy_count % players.size()];
    const float angle = random_angle();
    mod::spawn_enemy(registry, target.x + (std::cos(angle) * spawn_distance),
                     target.y + (std::sin(angle) * spawn_distance), *def, spawn_inits_[roll],
                     *lua_host_.bindings());
}

void GameServer::refresh_spawn_weights(std::uint16_t wave)
{
    spawn_weights_wave_ = wave;
    spawn_variants_.clear();
    spawn_inits_.clear();
    std::vector<float> weights;
    for (const mod::EnemyDef& def : lua_host_.enemies().defs()) {
        const float weight = def.weight_at(wave);
        if (weight <= 0.0f) { continue; }
        spawn_variants_.push_back(def.wire_id);
        spawn_inits_.push_back(def.inits_at(wave)); // Lua scaling runs once per wave
        weights.push_back(weight);
    }
    spawn_dist_ = std::discrete_distribution<std::size_t>{ weights.begin(), weights.end() };
}

void GameServer::snapshot_enemies()
{
    core::Registry& registry = world_.registry();
    pre_step_enemies_.clear();
    registry.view<EnemyTag, Position, Render, XpReward>().each(
      [&](core::Entity e, const EnemyTag&, const Position& pos, const Render& render, const XpReward& reward) {
          pre_step_enemies_.push_back(
            { .entity = e, .x = pos.x, .y = pos.y, .variant = render.variant, .xp = reward.value });
      });
}

void GameServer::emit_enemy_deaths()
{
    core::Registry& registry = world_.registry();
    if (!lua_host_.events().has("on_enemy_death")) { return; } // skip table churn if unsubscribed
    for (const EnemyDeathSnap& s : pre_step_enemies_) {
        if (registry.valid(s.entity)) { continue; } // still alive
        sol::table victim = lua_host_.lua().create_table();
        victim["x"] = s.x;
        victim["y"] = s.y;
        victim["variant"] = s.variant;
        victim["xp"] = s.xp;
        lua_host_.events().emit("on_enemy_death", victim);
    }
}

void GameServer::emit_downed_transitions()
{
    core::Registry& registry = world_.registry();
    const bool subscribed = lua_host_.events().has("on_player_downed");
    for (auto& [token, session] : sessions_) {
        if (!registry.valid(session.entity)) { continue; }
        const bool downed = registry.has<Downed>(session.entity);
        if (downed && !session.was_downed && subscribed) {
            lua_host_.events().emit("on_player_downed",
                                    mod::EntityHandle{ .reg = &registry, .entity = session.entity });
        }
        session.was_downed = downed;
    }
}

void GameServer::on_join(std::uint32_t peer_id, proto::ByteReader& reader)
{
    const auto join = reader.get<proto::Join>();
    if (!join) { return; }
    const std::string name = proto::read_name(join->name);

    // Validate the plugin set BEFORE the token lookup: a reconnecting player
    // whose mods/ changed would desync just like a fresh mismatched client.
    if (join->mods_hash != lua_host_.plugin_hash()) {
        spdlog::warn("denied '{}': plugin-set hash {:016x} != ours {:016x}", name, join->mods_hash,
                     lua_host_.plugin_hash());
        proto::ByteWriter writer;
        writer.put(proto::MsgType::JoinDenied);
        writer.put(proto::JoinDenied{ .server_hash = lua_host_.plugin_hash() });
        server_.send(peer_id, writer.bytes(), true);
        server_.kick(peer_id);
        return;
    }

    core::Registry& registry = world_.registry();

    bool reconnected = false;
    if (const auto it = sessions_.find(join->token); it != sessions_.end()) {
        // Reconnect: re-bind this peer to the existing player, dropping any stale
        // mapping from the previous (dead) peer.
        if (it->second.peer_id != 0) { peer_token_.erase(it->second.peer_id); }
        it->second.peer_id = peer_id;
        it->second.connected = true;
        // The fresh client instance has no snapshot history: restart it on full
        // snapshots (last_ack_tick only ever grows via max(), so it MUST be
        // reset here or we'd delta against a baseline the client never had).
        it->second.last_ack_tick = 0;
        peer_token_[peer_id] = join->token;
        reconnected = true;
        spdlog::info("'{}' reconnected -> entity {}", it->second.name, it->second.entity);
    } else {
        // New player.
        const core::Entity player = create_player(registry, 0, 0);
        Session session{ .entity = player, .name = name, .peer_id = peer_id, .connected = true };
        if (!have_host_) {
            session.is_host = true;
            host_token_ = join->token;
            have_host_ = true;
        }
        sessions_[join->token] = session;
        peer_token_[peer_id] = join->token;
        spdlog::info("'{}' joined -> entity {}{}", name, player, session.is_host ? " (host)" : "");
        // The loadout (weapon, crit, ...) is Lua content — mods attach it here.
        lua_host_.events().emit("on_player_spawn",
                                mod::EntityHandle{ .reg = &registry, .entity = player });
    }

    const Session& session = sessions_[join->token];
    proto::ByteWriter welcome;
    welcome.put(proto::MsgType::Welcome);
    welcome.put(proto::Welcome{ .player_net_id = session.entity,
                                .is_host = static_cast<std::uint8_t>(session.is_host ? 1 : 0) });
    server_.send(peer_id, welcome.bytes(), true);
    send_state(peer_id);
    broadcast_roster();

    // Reconnecting after the run already ended: the world is frozen (paused_,
    // still nominally Playing), so without the verdict the returning player would
    // land in a walkable-but-frozen world instead of the game-over screen.
    // Re-send the cached GameOver so they see the same overlay as everyone else.
    if (reconnected && run_over_) {
        proto::ByteWriter over;
        put_game_over(over);
        server_.send(peer_id, over.bytes(), true);
    }

    // Reconnecting mid-level-up: restore the menu and re-arm the obligation so
    // the returning player can pick (and the team isn't frozen waiting on them).
    // Re-send the SAME stored cards (no reroll abuse); only roll fresh if they
    // had none yet (were disconnected when the level-up began). A player who
    // already picked this level (chosen_) is NOT re-offered.
    if (reconnected && leveling_ && !chosen_.contains(join->token)) {
        if (offered_.contains(join->token)) {
            send_level_up(join->token);
        } else {
            start_level_up_for(join->token);
        }
        pending_.insert(join->token);
    }
}

void GameServer::on_disconnect(std::uint32_t peer_id)
{
    const auto it = peer_token_.find(peer_id);
    if (it == peer_token_.end()) { return; }
    const std::uint64_t token = it->second;
    peer_token_.erase(it);

    Session& session = sessions_[token];
    // Ignore a stale peer that a faster reconnect already replaced: its token is
    // now bound to the live peer and MUST stay in pending_ (dropping it here is
    // exactly the bug that froze the game). Level-up cleanup only for the live peer.
    if (session.peer_id != peer_id) { return; }
    session.connected = false;
    session.peer_id = 0;
    if (Velocity* vel = world_.registry().try_get<Velocity>(session.entity)) { *vel = { .dx = 0, .dy = 0 }; }

    // A live player leaving mid-level-up forfeits their pick so the team can
    // resume; on reconnect they get the SAME cards back (offered_ is kept, so
    // no reroll abuse). Only drop them from pending_ here.
    if (leveling_) {
        pending_.erase(token);
        if (pending_.empty()) { leveling_ = false; }
    }
    spdlog::info("'{}' disconnected (entity kept for reconnect)", session.name);
    broadcast_roster();
}

void GameServer::on_start(std::uint32_t peer_id)
{
    const auto it = peer_token_.find(peer_id);
    if (it == peer_token_.end() || it->second != host_token_ || state_ != proto::GameState::Lobby) { return; }
    state_ = proto::GameState::Playing;
    proto::ByteWriter writer;
    writer.put(proto::MsgType::State);
    writer.put(proto::StateMsg{ .state = static_cast<std::uint8_t>(state_) });
    server_.broadcast(writer.bytes(), true);
    spdlog::info("host started the game");
}

void GameServer::on_input(std::uint32_t peer_id, proto::ByteReader& reader)
{
    const auto input = reader.get<proto::Input>();
    const auto it = peer_token_.find(peer_id);
    if (!input || it == peer_token_.end() || state_ != proto::GameState::Playing) { return; }
    Session& session = sessions_[it->second];
    // Snapshot ack first — it must flow even for downed players (whose input is
    // otherwise ignored), or their delta stream would stall while dead.
    session.last_ack_tick = std::max(session.last_ack_tick, input->ack_tick);
    core::Registry& registry = world_.registry();
    const core::Entity player = session.entity;
    if (registry.try_get<Downed>(player) != nullptr) { return; } // no control while down
    if (Velocity* vel = registry.try_get<Velocity>(player)) {
        const float speed = registry.try_get<Speed>(player) != nullptr ? registry.get<Speed>(player).value : PLAYER_SPEED;
        apply_input(*vel, input->move_x, input->move_y, speed);
    }
    if (AimState* aim = registry.try_get<AimState>(player)) {
        *aim = { .dx = input->aim_x, .dy = input->aim_y, .firing = input->firing };
    }
    if (input->dash != 0) {
        if (Dash* dash = registry.try_get<Dash>(player)) {
            // Dash toward the move direction; standing still dashes toward the aim.
            const bool moving = input->move_x != 0 || input->move_y != 0;
            const float dx = moving ? static_cast<float>(input->move_x) : input->aim_x;
            const float dy = moving ? static_cast<float>(input->move_y) : input->aim_y;
            start_dash(*dash, dx, dy);
        }
    }
}

void GameServer::on_command(std::uint32_t peer_id, proto::ByteReader& reader)
{
    const auto command = reader.get<proto::Command>();
    const auto it = peer_token_.find(peer_id);
    if (!command || it == peer_token_.end() || it->second != host_token_) { return; } // host only
    switch (*command) {
    case proto::Command::Pause:
        paused_ = true;
        spdlog::info("game paused by host");
        break;
    case proto::Command::Resume:
        paused_ = false;
        spdlog::info("game resumed by host");
        break;
    case proto::Command::BackToLobby:
        // Only meaningful on the game-over screen: full run reset to Lobby.
        if (run_over_) { reset_run(); }
        break;
    }
}

// A mod console command from the client: "name args..." (no slash). Host-only
// and Playing-only (callbacks poke live entities). Dispatch to the matching
// mod:command callback with (invoking player, args...) — numeric tokens are
// passed as numbers, the rest as strings. Protected call: a broken command
// logs and is skipped, like every other mod hook.
void GameServer::on_lua_command(std::uint32_t peer_id, proto::ByteReader& reader)
{
    const auto len = reader.get<std::uint8_t>();
    const auto it = peer_token_.find(peer_id);
    if (!len || it == peer_token_.end() || it->second != host_token_ // host only
        || state_ != proto::GameState::Playing) {
        return;
    }
    std::string line;
    line.reserve(*len);
    for (std::uint8_t i = 0; i < *len; ++i) {
        const auto ch = reader.get<char>();
        if (!ch) { return; }
        line.push_back(*ch);
    }

    std::vector<std::string> tokens;
    for (std::size_t pos = 0; pos < line.size();) {
        const std::size_t start = line.find_first_not_of(' ', pos);
        if (start == std::string::npos) { break; }
        const std::size_t end = line.find(' ', start);
        tokens.push_back(line.substr(start, end - start));
        pos = end == std::string::npos ? line.size() : end;
    }
    if (tokens.empty()) { return; }

    const mod::ModState::ConsoleCommand* cmd = nullptr;
    for (const mod::ModState::ConsoleCommand& candidate : lua_host_.state().commands) {
        if (candidate.name == tokens.front()) {
            cmd = &candidate;
            break;
        }
    }
    if (cmd == nullptr) {
        spdlog::warn("unknown console command '/{}'", tokens.front());
        return;
    }

    sol::state& lua = lua_host_.lua();
    std::vector<sol::object> args;
    args.reserve(tokens.size());
    core::Registry& registry = world_.registry();
    args.push_back(sol::make_object(lua, mod::EntityHandle{ .reg = &registry,
                                                            .entity = sessions_[it->second].entity }));
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        char* parse_end = nullptr;
        const double num = std::strtod(tokens[i].c_str(), &parse_end);
        if (parse_end != nullptr && *parse_end == '\0' && parse_end != tokens[i].c_str()) {
            args.push_back(sol::make_object(lua, num));
        } else {
            args.push_back(sol::make_object(lua, tokens[i]));
        }
    }
    const sol::protected_function_result result = cmd->fn(sol::as_args(args));
    if (!result.valid()) {
        const sol::error err = result;
        spdlog::warn("console command '/{}' failed: {}", cmd->name, err.what());
    } else {
        spdlog::info("console command '/{}' by host", line);
    }
}

// The mods decide when a run ends (world:end_game -> a RunEnd mailbox entity);
// the engine carries out the transition: freeze the sim (clients keep getting
// the frozen world under their game-over overlay) and tell everyone, once.
void GameServer::check_run_end()
{
    if (run_over_) { return; }
    core::Registry& registry = world_.registry();
    std::uint8_t won = 0;
    std::vector<core::Entity> notes;
    registry.view<RunEnd>().each([&](core::Entity e, const RunEnd& note) {
        if (notes.empty()) { won = note.won; } // first verdict wins
        notes.push_back(e);
    });
    if (notes.empty()) { return; }
    for (const core::Entity e : notes) { registry.destroy(e); }

    run_over_ = true;
    paused_ = true;
    std::uint16_t wave = 1;
    registry.view<GameStats>().each([&](core::Entity, const GameStats& stats) { wave = stats.wave; });
    run_over_msg_ = proto::GameOverMsg{ .won = won, .final_wave = wave, .final_level = level_ };
    // Freeze each player's RunStats (Lua-incremented) into the scoreboard
    // block — cached with the verdict so reconnecters see the same numbers.
    run_over_entries_.clear();
    for (const auto& [token, session] : sessions_) {
        const RunStats* rs = registry.try_get<RunStats>(session.entity);
        if (rs == nullptr) { continue; }
        const auto clamp16 = [](std::int32_t v) {
            return static_cast<std::uint16_t>(std::clamp(v, 0, 65535));
        };
        run_over_entries_.push_back(proto::GameOverEntry{
          .net_id = session.entity,
          .damage = static_cast<std::uint32_t>(std::max(0.0f, rs->damage)),
          .kills = clamp16(rs->kills), .downs = clamp16(rs->downs),
          .revives = clamp16(rs->revives) });
    }
    proto::ByteWriter writer;
    put_game_over(writer);
    server_.broadcast(writer.bytes(), true);
    spdlog::info("run over: {} at wave {} (level {})", won != 0 ? "VICTORY" : "defeat", wave, level_);
}

void GameServer::put_game_over(proto::ByteWriter& writer) const
{
    writer.put(proto::MsgType::GameOver);
    writer.put(run_over_msg_);
    writer.put(static_cast<std::uint8_t>(run_over_entries_.size()));
    for (const proto::GameOverEntry& entry : run_over_entries_) { writer.put(entry); }
}

// A boss chest was opened (world:open_chest -> a ChestOpen mailbox entity):
// run ONE offer round for every connected player through the level-up
// machinery, flavored Chest — the mod rolls it objects-only, the client shows
// the treasure title. level_/xp are untouched; multiple notes in one step
// (overlapping chests) still collapse into a single round.
void GameServer::check_chest()
{
    if (run_over_) { return; } // the winning kill's chest dies with the run
    core::Registry& registry = world_.registry();
    std::vector<core::Entity> notes;
    registry.view<ChestOpen>().each([&](core::Entity e, const ChestOpen&) { notes.push_back(e); });
    if (notes.empty()) { return; }
    for (const core::Entity e : notes) { registry.destroy(e); }

    begin_offer_round(proto::OfferFlavor::Chest);
    spdlog::info("boss chest opened -> object pick for {} player(s)", pending_.size());
}

// Full reset for another run: wipe every world entity, rebuild each session's
// player through the same path as a fresh join (loadout via on_player_spawn),
// clear all progression/spawn/snapshot state, and drop everyone in the Lobby.
// tick_ stays monotonic on purpose — snapshot acks are max()-based on both ends.
void GameServer::reset_run()
{
    core::Registry& registry = world_.registry();

    std::vector<core::Entity> doomed; // players, enemies, bullets, orbs, hearts
    registry.view<Position>().each([&](core::Entity e, const Position&) { doomed.push_back(e); });
    // Pending grant notes have no Position (they're bare mailboxes): sweep them
    // too so a queued /upgrade /object can't leak into the next run.
    registry.view<OfferGrant>().each([&](core::Entity e, const OfferGrant&) { doomed.push_back(e); });
    for (const core::Entity e : doomed) { registry.destroy(e); } // script rows go too
    registry.view<GameStats>().each([&](core::Entity, GameStats& stats) {
        stats = GameStats{ .xp = 0, .wave = 1 };
    });
    run_over_entries_.clear(); // the old scoreboard dies with the run

    for (auto& [token, session] : sessions_) {
        session.entity = create_player(registry, 0, 0);
        session.was_downed = false;
        session.last_ack_tick = 0; // fresh entities => clients restart on fulls
        lua_host_.events().emit("on_player_spawn",
                                mod::EntityHandle{ .reg = &registry, .entity = session.entity });
        if (session.connected) { // the old net id died with the old entity
            proto::ByteWriter welcome;
            welcome.put(proto::MsgType::Welcome);
            welcome.put(proto::Welcome{ .player_net_id = session.entity,
                                        .is_host = session.is_host ? std::uint8_t{ 1 } : std::uint8_t{ 0 } });
            server_.send(session.peer_id, welcome.bytes(), true);
        }
    }

    level_ = 1;
    xp_needed_ = mod::run_xp_curve(lua_host_, level_);
    leveling_ = false;
    pending_.clear();
    offered_.clear();
    chosen_.clear();
    spawn_timer_ = 0.0f;
    wave_timer_ = 0.0f;
    spawn_weights_wave_ = 0; // forces a weight refresh on the next run's wave 1
    spawn_variants_.clear();
    spawn_inits_.clear();
    paused_ = false;
    run_over_ = false;
    snapshot_history_.clear();
    pre_step_enemies_.clear();

    state_ = proto::GameState::Lobby;
    proto::ByteWriter state;
    state.put(proto::MsgType::State);
    state.put(proto::StateMsg{ .state = static_cast<std::uint8_t>(state_) });
    server_.broadcast(state.bytes(), true);
    broadcast_roster();
    spdlog::info("run reset: back to lobby");
}

void GameServer::broadcast_roster()
{
    proto::ByteWriter writer;
    writer.put(proto::MsgType::Roster);
    writer.put(proto::RosterHeader{ .count = static_cast<std::uint8_t>(sessions_.size()) });
    for (const auto& [token, session] : sessions_) {
        proto::RosterEntry entry{};
        entry.net_id = session.entity;
        proto::write_name(entry.name, session.name);
        entry.is_host = session.is_host ? 1 : 0;
        entry.connected = session.connected ? 1 : 0;
        writer.put(entry);
    }
    server_.broadcast(writer.bytes(), true);
}

void GameServer::send_state(std::uint32_t peer_id)
{
    proto::ByteWriter writer;
    writer.put(proto::MsgType::State);
    writer.put(proto::StateMsg{ .state = static_cast<std::uint8_t>(state_) });
    server_.send(peer_id, writer.bytes(), true);
}

void GameServer::stream_snapshots()
{
    core::Registry& registry = world_.registry();

    proto::SnapshotState state;
    state.tick = tick_;
    state.level = level_;

    // Quantization origin: the players' centroid — full-snapshot positions
    // travel as int16 offsets from it, so it must sit near the action.
    std::size_t player_count = 0;
    registry.view<PlayerTag, Position>().each(
      [&](core::Entity, const PlayerTag&, const Position& pos) {
          state.origin_x += pos.x;
          state.origin_y += pos.y;
          ++player_count;
      });
    if (player_count > 0) {
        state.origin_x /= static_cast<float>(player_count);
        state.origin_y /= static_cast<float>(player_count);
    }
    // Snap the origin to the quantization grid: with a free-moving origin the
    // rounded offset of a STATIONARY entity oscillates ±0.25 px between
    // snapshots (visible as sprite facing/idle flicker on the client).
    state.origin_x = std::round(state.origin_x * proto::snapshot_pos_scale) / proto::snapshot_pos_scale;
    state.origin_y = std::round(state.origin_y * proto::snapshot_pos_scale) / proto::snapshot_pos_scale;

    proto::ByteWriter blob; // all script-comp bytes, sliced per entity below
    registry.view<Position>().each([&](core::Entity entity, const Position& pos) {
        // Kind + variant come straight from the kernel Render component
        // (stamped by factories, variant mutated freely by Lua).
        std::uint8_t kind = static_cast<std::uint8_t>(proto::EntityKind::Mover);
        std::uint8_t variant = 0;
        std::uint8_t fx = 0;
        if (const Render* render = registry.try_get<Render>(entity)) {
            kind = render->kind;
            variant = render->variant;
            fx = render->fx;
        }

        std::uint8_t health = 255;
        if (const Health* hp = registry.try_get<Health>(entity); hp && hp->max > 0.0f) {
            const float frac = std::clamp(hp->current / hp->max, 0.0f, 1.0f);
            health = static_cast<std::uint8_t>(frac * 255.0f);
        }

        std::uint16_t move_speed = 0;
        if (kind == static_cast<std::uint8_t>(proto::EntityKind::Player)) {
            // Players use discrete hearts: health byte = current, variant = max.
            if (const Hearts* hearts = registry.try_get<Hearts>(entity)) {
                health = static_cast<std::uint8_t>(std::clamp<int>(hearts->current, 0, 255));
                variant = static_cast<std::uint8_t>(std::clamp<int>(hearts->max, 1, 255));
            }
            if (const Speed* speed = registry.try_get<Speed>(entity)) {
                move_speed = static_cast<std::uint16_t>(speed->value);
            }
            // Per-player trailer: authoritative aim + trigger (so the sprite
            // faces/shoots where the SIM aims, e.g. autofire, not the mouse) and
            // dash state (params the game/Lua set, which the client can't guess).
            proto::PlayerAim rec{ .id = entity };
            if (const AimState* aim = registry.try_get<AimState>(entity)) {
                rec.aim_qx = proto::quantize_aim(aim->dx);
                rec.aim_qy = proto::quantize_aim(aim->dy);
                rec.firing = aim->firing;
            }
            if (const Dash* dash = registry.try_get<Dash>(entity)) {
                rec.dash_charges = dash->charges;
                rec.dash_max = dash->max_charges;
                rec.dash_cd_ms = proto::seconds_to_ms(dash->cooldown);
                rec.dash_cd_max_ms = proto::seconds_to_ms(dash->cooldown_max);
            }
            state.aims.push_back(rec);
        }

        std::uint8_t scale_q = 0; // 0 = no Scale component = 1.0
        if (const Scale* scale = registry.try_get<Scale>(entity)) {
            scale_q = proto::quantize_scale(scale->value);
        }

        const std::size_t blob_start = blob.bytes().size();
        mod::write_networked(blob, registry, lua_host_.scripts(), entity);
        state.entities.push_back({ .id = entity,
                                   .qx = proto::to_half_px(pos.x),
                                   .qy = proto::to_half_px(pos.y),
                                   .move_speed = move_speed,
                                   .kind = kind, .health = health, .variant = variant,
                                   .scale_q = scale_q, .fx = fx,
                                   .script_off = static_cast<std::uint32_t>(blob_start),
                                   .script_len = static_cast<std::uint16_t>(blob.bytes().size() - blob_start) });
    });
    state.script_blob.assign(blob.bytes().begin(), blob.bytes().end());
    state.sort_entities();

    // Shared XP progress + wave for the HUD.
    registry.view<GameStats>().each([&](core::Entity, const GameStats& stats) {
        const float frac = std::clamp(static_cast<float>(stats.xp) / static_cast<float>(xp_needed_), 0.0f, 1.0f);
        state.xp_frac = static_cast<std::uint8_t>(frac * 255.0f);
        state.wave = stats.wave;
    });

    snapshot_history_.push_back(std::move(state));
    if (snapshot_history_.size() > snapshot_history_len) { snapshot_history_.pop_front(); }
    const proto::SnapshotState& current = snapshot_history_.back();

    // Per-peer: delta against the peer's acked baseline when it's still in the
    // ring, else a full snapshot (fresh join, stalled acks, long loss burst).
    // Peers sharing a baseline share one encoding.
    proto::ByteWriter full;
    std::unordered_map<std::uint32_t, proto::ByteWriter> deltas; // baseline tick -> packet
    for (const auto& [token, session] : sessions_) {
        if (!session.connected) { continue; }
        const proto::SnapshotState* baseline = nullptr;
        for (const proto::SnapshotState& past : snapshot_history_) {
            if (past.tick == session.last_ack_tick) {
                baseline = &past;
                break;
            }
        }
        if (baseline != nullptr && baseline->tick != current.tick) {
            proto::ByteWriter& packet = deltas[baseline->tick];
            if (packet.bytes().empty()) {
                packet.put(proto::MsgType::SnapshotDelta);
                proto::encode_delta(current, *baseline, packet);
            }
            server_.send(session.peer_id, packet.bytes(), false);
            snapshot_bytes_sent_ += packet.bytes().size();
        } else {
            if (full.bytes().empty()) {
                full.reserve(1 + sizeof(proto::SnapshotHeader)
                             + (current.entities.size() * (sizeof(proto::SnapshotEntry) + 4))
                             + (current.aims.size() * sizeof(proto::PlayerAim)));
                full.put(proto::MsgType::Snapshot);
                proto::encode_full(current, full);
            }
            server_.send(session.peer_id, full.bytes(), false);
            snapshot_bytes_sent_ += full.bytes().size();
        }
    }

    // Floating combat numbers queued by the mods this snapshot window: one
    // unreliable broadcast, then the queue resets (a lost packet drops a few
    // numbers, never the game state).
    std::vector<proto::DamageEvent>& events = lua_host_.state().damage_events;
    if (!events.empty()) {
        proto::ByteWriter packet;
        packet.reserve(2 + (events.size() * sizeof(proto::DamageEvent)));
        packet.put(proto::MsgType::DamageEvents);
        packet.put(static_cast<std::uint8_t>(events.size()));
        for (const proto::DamageEvent& ev : events) { packet.put(ev); }
        server_.broadcast(packet.bytes(), false);
        snapshot_bytes_sent_ += packet.bytes().size();
        events.clear();
    }
}

void GameServer::check_level_up()
{
    core::Registry& registry = world_.registry();
    GameStats* stats = nullptr;
    registry.view<GameStats>().each([&](core::Entity, GameStats& s) { stats = &s; });
    if (stats == nullptr || stats->xp < xp_needed_) { return; }

    stats->xp -= xp_needed_;
    ++level_;
    xp_needed_ = mod::run_xp_curve(lua_host_, level_);

    // Freeze the world and ask every connected player to choose an upgrade.
    begin_offer_round(proto::OfferFlavor::Level);
    spdlog::info("team reached level {}", level_);
    lua_host_.events().emit("on_level_up", static_cast<int>(level_));
}

// Freeze the sim and roll+send an offer to every connected player (the shared
// core of level-ups, boss chests and the /upgrade /object grants). Keyed by
// token so a mid-round reconnect (new peer_id) still resolves.
void GameServer::begin_offer_round(proto::OfferFlavor flavor)
{
    leveling_ = true;
    offer_flavor_ = flavor;
    pending_.clear();
    offered_.clear();
    chosen_.clear();
    for (const auto& [token, session] : sessions_) {
        if (!session.connected) { continue; }
        start_level_up_for(token);
        pending_.insert(token);
    }
    if (pending_.empty()) { leveling_ = false; } // nobody to choose
}

// OfferGrant mailbox (/upgrade, /object): consume exactly ONE note and run its
// round. One-per-tick + the round's own freeze means N queued notes become N
// SEQUENTIAL menus. Only fires between rounds (the tick loop gates on
// !leveling_) and never during the game-over freeze.
void GameServer::check_offer_grants()
{
    if (run_over_ || leveling_) { return; }
    core::Registry& registry = world_.registry();
    core::Entity note{};
    bool found = false;
    std::uint8_t flavor = 0;
    registry.view<OfferGrant>().each([&](core::Entity e, const OfferGrant& g) {
        if (found) { return; } // one per round; the rest wait their turn
        note = e;
        flavor = g.flavor;
        found = true;
    });
    if (!found) { return; }
    registry.destroy(note);
    begin_offer_round(flavor != 0 ? proto::OfferFlavor::Chest : proto::OfferFlavor::Level);
    spdlog::info("granted a {} offer round", flavor != 0 ? "object" : "upgrade");
}

void GameServer::start_level_up_for(std::uint64_t token)
{
    // The GAME rolls the offer (mod:level_offer — count and picks are mod
    // policy, e.g. Crystal Ball's +1 card); the engine's fixed 3-card roll is
    // the fallback. Rolled ONCE per (player, level): reconnects re-SEND the
    // stored offer (send_level_up), they never re-roll.
    const core::Entity player = sessions_[token].entity;
    const char* context = offer_flavor_ == proto::OfferFlavor::Chest ? "chest" : "level";
    std::vector<proto::LevelUpChoice> offer =
      mod::run_level_offer(lua_host_, world_.registry(), player, static_cast<int>(level_), context);
    if (offer.empty()) { offer = roll_upgrades(player); }
    offered_[token] = std::move(offer);
    send_level_up(token);
}

// Re-send the cards already stored for this token WITHOUT re-rolling — so a
// disconnect/reconnect can't be used to reroll for a better upgrade.
void GameServer::send_level_up(std::uint64_t token)
{
    const auto it = offered_.find(token);
    if (it == offered_.end()) { return; }
    proto::ByteWriter writer;
    writer.put(proto::MsgType::LevelUp);
    writer.put(static_cast<std::uint8_t>(offer_flavor_)); // level-up vs boss chest (scene theme)
    writer.put(static_cast<std::uint8_t>(it->second.size()));
    for (const proto::LevelUpChoice& choice : it->second) { writer.put(choice); }
    server_.send(sessions_[token].peer_id, writer.bytes(), true);
}

std::vector<proto::LevelUpChoice> GameServer::roll_upgrades(core::Entity player)
{
    const mod::ContentRegistry& registry = lua_host_.registry();
    const mod::EntityHandle handle{ .reg = &world_.registry(), .entity = player };

    // Wire ids of content available to this specific player (each def's own
    // `available` predicate decides — e.g. Onion only without an aura).
    std::vector<std::uint8_t> pool;
    for (const mod::ContentDef& d : registry.defs()) {
        if (mod::run_available(d, handle)) { pool.push_back(d.wire_id); }
    }

    std::array<float, mod::rarity_count> rarity_weights{};
    for (std::uint8_t r = 0; r < mod::rarity_count; ++r) {
        rarity_weights[r] = mod::rarity_weight(static_cast<mod::Rarity>(r));
    }

    // Per card: roll the tier FIRST, then pick among content offered at that
    // tier (stat upgrades with a nonzero amount, objects with that declared
    // rarity). No candidates -> fall back a tier (L->E->R->U->C). This is what
    // keeps legendaries actually rare — objects no longer force gold cards.
    std::vector<proto::LevelUpChoice> out(proto::level_up_choices);
    for (std::size_t k = 0; k < proto::level_up_choices; ++k) {
        proto::LevelUpChoice choice{}; // pool exhausted -> pad with content 0 at Common
        std::discrete_distribution<std::size_t> tier_dist{ rarity_weights.begin(), rarity_weights.end() };
        int tier = static_cast<int>(tier_dist(rng_));
        for (; tier >= 0; --tier) {
            std::vector<std::size_t> candidates; // indices into pool
            for (std::size_t i = 0; i < pool.size(); ++i) {
                const mod::ContentDef* d = registry.by_wire(pool[i]);
                if (d != nullptr && mod::offered_at(*d, static_cast<mod::Rarity>(tier))) {
                    candidates.push_back(i);
                }
            }
            if (candidates.empty()) { continue; }
            std::uniform_int_distribution<std::size_t> pick{ 0, candidates.size() - 1 };
            const std::size_t idx = candidates[pick(rng_)];
            choice = { .id = pool[idx], .rarity = static_cast<std::uint8_t>(tier) };
            pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(idx)); // no duplicates
            break;
        }
        out[k] = choice;
    }
    return out;
}

void GameServer::on_select(std::uint32_t peer_id, proto::ByteReader& reader)
{
    const auto select = reader.get<proto::SelectUpgrade>();
    const auto peer_it = peer_token_.find(peer_id);
    if (!select || !leveling_ || peer_it == peer_token_.end()) { return; }
    const std::uint64_t token = peer_it->second;
    const auto offer_it = offered_.find(token);
    if (offer_it == offered_.end()) { return; } // already chose, or wasn't offered
    if (select->index >= offer_it->second.size()) { return; } // offers vary in count now

    const proto::LevelUpChoice& chosen = offer_it->second[select->index];
    if (const mod::ContentDef* d = lua_host_.registry().by_wire(chosen.id)) {
        const mod::EntityHandle handle{ .reg = &world_.registry(), .entity = sessions_[token].entity };
        mod::run_apply(*d, handle, static_cast<mod::Rarity>(chosen.rarity));
    }

    pending_.erase(token);
    offered_.erase(offer_it);
    chosen_.insert(token); // don't re-offer on a later reconnect this level
    if (pending_.empty()) { leveling_ = false; } // everyone chose -> resume
}

} // namespace server
