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
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <utility>
#include <vector>

namespace {

// XP required to reach the next level (curve on the current level).
std::uint32_t xp_needed_for(std::uint16_t level)
{
    return 5u + static_cast<std::uint32_t>(level) * 4u;
}

} // namespace

namespace {

// Spawn / wave tuning.
constexpr float wave_duration = 15.0f;   // seconds per wave
constexpr float spawn_distance = 600.0f; // ring radius around a player
constexpr std::size_t max_enemies = 200;

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
    xp_needed_ = xp_needed_for(level_);
    // Bind the ECS into the sim VM, then load plugins (upgrades + objects).
    // Bindings capture &world_.registry(), which is stable for our lifetime.
    mod::install_sim_bindings(lua_host_, world_.registry());
    lua_host_.load_dir("mods");
    mod::install_script_systems(lua_host_, world_); // add Lua-defined systems to the pipeline
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
        // Frozen while paused or during a level-up selection, but keep streaming
        // the (frozen) world so clients stay consistent.
        if (!paused_ && !leveling_) {
            spawn_enemies(timestep.dt());
            snapshot_enemies();       // record the enemy set before stepping
            world_.step(timestep.dt());
            emit_enemy_deaths();      // any enemy gone after the step died
            emit_downed_transitions();
            check_level_up();
        }
        if (tick_ % proto::snapshot_every_n_ticks == 0) { broadcast_snapshot(); }
        ++tick_;
    }
}

void GameServer::spawn_enemies(float dt)
{
    core::Registry& registry = world_.registry();

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

    if (const auto it = sessions_.find(join->token); it != sessions_.end()) {
        // Reconnect: re-bind this peer to the existing player, dropping any stale
        // mapping from the previous (dead) peer.
        if (it->second.peer_id != 0) { peer_token_.erase(it->second.peer_id); }
        it->second.peer_id = peer_id;
        it->second.connected = true;
        peer_token_[peer_id] = join->token;
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
}

void GameServer::on_disconnect(std::uint32_t peer_id)
{
    const auto it = peer_token_.find(peer_id);
    if (it == peer_token_.end()) { return; }
    const std::uint64_t token = it->second;
    peer_token_.erase(it);

    // If this peer still owed a level-up choice, drop it and resume if last.
    if (leveling_) {
        pending_.erase(peer_id);
        offered_.erase(peer_id);
        if (pending_.empty()) { leveling_ = false; }
    }

    Session& session = sessions_[token];
    // Ignore a stale peer that a faster reconnect already replaced.
    if (session.peer_id != peer_id) { return; }
    session.connected = false;
    session.peer_id = 0;
    if (Velocity* vel = world_.registry().try_get<Velocity>(session.entity)) { *vel = { .dx = 0, .dy = 0 }; }
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
    core::Registry& registry = world_.registry();
    const core::Entity player = sessions_[it->second].entity;
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
    }
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

void GameServer::broadcast_snapshot()
{
    core::Registry& registry = world_.registry();
    std::vector<proto::SnapshotEntry> entries;
    registry.view<Position>().each([&](core::Entity entity, const Position& pos) {
        // Kind + variant come straight from the kernel Render component
        // (stamped by factories, variant mutated freely by Lua).
        std::uint8_t kind = static_cast<std::uint8_t>(proto::EntityKind::Mover);
        std::uint8_t variant = 0;
        if (const Render* render = registry.try_get<Render>(entity)) {
            kind = render->kind;
            variant = render->variant;
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
        }

        entries.push_back({ .id = entity, .x = pos.x, .y = pos.y,
                            .kind = kind, .health = health, .variant = variant,
                            .move_speed = move_speed });
    });

    // Shared XP progress + wave for the HUD.
    std::uint8_t xp_frac = 0;
    std::uint16_t wave = 1;
    registry.view<GameStats>().each([&](core::Entity, const GameStats& stats) {
        const float frac = std::clamp(static_cast<float>(stats.xp) / static_cast<float>(xp_needed_), 0.0f, 1.0f);
        xp_frac = static_cast<std::uint8_t>(frac * 255.0f);
        wave = stats.wave;
    });

    proto::ByteWriter writer;
    writer.put(proto::MsgType::Snapshot);
    writer.put(proto::SnapshotHeader{ .server_tick = tick_,
                                      .count = static_cast<std::uint16_t>(entries.size()),
                                      .level = level_,
                                      .xp_frac = xp_frac,
                                      .wave = wave });
    // Each entry is followed by the entity's networked script components.
    for (const proto::SnapshotEntry& entry : entries) {
        writer.put(entry);
        mod::write_networked(writer, registry, lua_host_.scripts(), entry.id);
    }
    server_.broadcast(writer.bytes(), false);
}

void GameServer::check_level_up()
{
    core::Registry& registry = world_.registry();
    GameStats* stats = nullptr;
    registry.view<GameStats>().each([&](core::Entity, GameStats& s) { stats = &s; });
    if (stats == nullptr || stats->xp < xp_needed_) { return; }

    stats->xp -= xp_needed_;
    ++level_;
    xp_needed_ = xp_needed_for(level_);

    // Freeze the world and ask every connected player to choose an upgrade.
    leveling_ = true;
    pending_.clear();
    offered_.clear();
    for (const auto& [peer, token] : peer_token_) {
        start_level_up_for(peer);
        pending_.insert(peer);
    }
    if (pending_.empty()) { leveling_ = false; } // nobody to choose
    spdlog::info("team reached level {}", level_);
    lua_host_.events().emit("on_level_up", static_cast<int>(level_));
}

void GameServer::start_level_up_for(std::uint32_t peer_id)
{
    const core::Entity player = sessions_[peer_token_[peer_id]].entity;
    const std::array<proto::LevelUpChoice, proto::level_up_choices> choices = roll_upgrades(player);
    offered_[peer_id] = choices;

    proto::ByteWriter writer;
    writer.put(proto::MsgType::LevelUp);
    for (const proto::LevelUpChoice& choice : choices) { writer.put(choice); }
    server_.send(peer_id, writer.bytes(), true);
}

std::array<proto::LevelUpChoice, proto::level_up_choices> GameServer::roll_upgrades(core::Entity player)
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
    std::array<proto::LevelUpChoice, proto::level_up_choices> out{};
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
    const auto offer_it = offered_.find(peer_id);
    if (!select || !leveling_ || peer_it == peer_token_.end() || offer_it == offered_.end()
        || select->index >= proto::level_up_choices) {
        return;
    }

    const proto::LevelUpChoice& chosen = offer_it->second[select->index];
    if (const mod::ContentDef* d = lua_host_.registry().by_wire(chosen.id)) {
        const mod::EntityHandle handle{ .reg = &world_.registry(),
                                        .entity = sessions_[peer_it->second].entity };
        mod::run_apply(*d, handle, static_cast<mod::Rarity>(chosen.rarity));
    }

    pending_.erase(peer_id);
    offered_.erase(offer_it);
    if (pending_.empty()) { leveling_ = false; } // everyone chose -> resume
}

} // namespace server
