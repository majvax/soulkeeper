#pragma once
#include "client/engine.hpp"
#include "client/mod/render_bindings.hpp"
#include "client/renderer.hpp"
#include "core/ecs.hpp"
#include "client/scene.hpp"
#include "client/scene/console.hpp"
#include "client/scene/game_over.hpp"
#include "client/scene/level_up.hpp"
#include "client/scene/pause.hpp"
#include "client/sprites.hpp"
#include "client/trainer.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"
#include "shared/map/terrain.hpp"
#include "shared/protocol.hpp"
#include "shared/snapshot_codec.hpp"
#include "shared/system/input.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <imgui.h>
#include <iterator>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Playing-phase scene: renders the world from server snapshots (remote entities
// interpolated), predicts the local player, and follows it with the camera.
// All networking lives in the injected client::Session.
struct Remote
{
    std::uint8_t kind;    // proto::EntityKind
    std::uint32_t net_id; // server id, for the name label
    std::uint8_t health;  // 0..255 fraction of max, for the health bar
    std::uint8_t variant; // enemies: archetype wire id (mod EnemyRegistry)
    float face = 1.0f;    // last horizontal direction (packs face right; -1 flips)
    float scale = 1.0f;   // kernel Scale component (Lua-driven size, e.g. Vitality)
    bool moving = false;  // position changed last snapshot -> Move vs Idle clip
    float dir_x = 1.0f;   // last movement direction — remote players' 8-way clips
    float dir_y = 0.0f;
    float aim_x = 1.0f;   // authoritative aim (players) — facing/shoot pose off the wire
    float aim_y = 0.0f;
    bool firing = false;  // authoritative trigger (players) — Shoot vs Move/Idle
    float death_start = -1.0f; // anim clock when a player went down (-1 = alive)
    float flash_until = -1.0f; // anim clock deadline for the red hit-flash
    std::uint8_t fx = 0;       // Render.fx anim state off the wire (1 = attacking)
    float fx_start = -1.0f;    // anim clock at the 0->1 transition (attack plays once)
};

// A short client-only burst effect where an entity died/was picked up (the
// entity itself is already gone from the render registry).
struct Poof
{
    float x, y;      // world position at despawn
    float t0;        // anim clock at spawn
    float radius;    // final ring radius (scaled to what vanished)
    bool pickup;     // orbs/hearts sparkle small and bright
};

// A floating combat number (server DamageEvents packet): rises + fades over
// its short life, crits bigger and gold.
struct FloatNum
{
    float x, y;           // world position of the hit
    float t0;             // anim clock at arrival
    std::uint16_t amount;
    bool crit;
};

// Everything draw_player needs to pick a clip in the directional player pack.
// Remotes only know movement (+ downed via hearts); firing/dash are local-only.
struct PlayerAnim
{
    float dir_x = 1.0f;
    float dir_y = 0.0f;
    bool moving = false;
    bool firing = false;
    float dash_frac = -1.0f;   // 0..1 burst progress, < 0 = not dashing
    float death_start = -1.0f; // anim clock at down time, < 0 = alive
};

// One entry of the per-frame y-sorted world pass: obstacles, enemies and
// players draw in FEET order so bodies pass behind trees and in front of
// rocks correctly (the map's z-axis).
struct WorldItem
{
    float key;  // sort key: screen feet y
    float x, y; // screen position
    enum : std::uint8_t { Enemy, RemotePlayer, LocalPlayer, Obst } type;
    const Remote* rem = nullptr;         // Enemy / RemotePlayer
    shared::map::Obstacle ob{};          // Obst (world coords)
};

class GameScene : public client::Scene
{
public:
    explicit GameScene(client::Engine* engine) : Scene(engine), textures_{ engine->renderer() }
    {
        // Count the map art variants once: obstacle/deco kinds hash into these
        // (the sim only knows collider classes; sprites are pure client).
        // Exact "<prefix>NN.png" match — "stump_" must not swallow "stump_snow_".
        const auto count_art = [](const std::string& prefix) {
            int n = 0;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator("assets/map", ec)) {
                const std::string name = e.path().filename().string();
                if (name.starts_with(prefix) && name.size() == prefix.size() + 6) { ++n; }
            }
            return n;
        };
        art_tree_forest_ = count_art("tree_forest_");
        art_tree_plain_ = count_art("tree_plain_");
        art_tree_snow_ = count_art("tree_snow_");
        art_rocks_ = count_art("rock_");
        art_bushes_ = count_art("bush_");
        art_plants_ = count_art("plant_");
        art_pebbles_ = count_art("pebble_");
        art_stumps_ = count_art("stump_");
        art_stump_snow_ = count_art("stump_snow_");
        draw_ctx_.renderer = engine->renderer();
        draw_ctx_.textures = &textures_;
        draw_ctx_.audio = &engine->audio();
        // One persistent Lua object referencing our DrawContext, reused every
        // frame for all plugin draw hooks (no per-call allocation).
        ctx_obj_ = sol::make_object(engine->mods().lua(), std::ref(draw_ctx_));
        hud_ctx_.textures = &textures_; // cached icons for plugin HUD hooks
        hud_ctx_.gui = &engine->gui();  // HUD panels render through the widget kit
        hud_ctx_.renderer = engine->renderer();
        hud_ctx_obj_ = sol::make_object(engine->mods().lua(), std::ref(hud_ctx_));
    }

    // Input comes through the event stack (not SDL_GetKeyboardState), so a scene
    // that returns Stop above us naturally blocks it — no ImGui focus checks.
    auto handle_event(const SDL_Event& event) -> Propagation override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_TAB) {
            clear_input(); // don't leave a key "held" once we hand focus to the console
            engine_->scenes().push<ConsoleScene>(engine_);
            return Stop;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
            clear_input();
            engine_->scenes().push<PauseScene>(engine_);
            return Stop;
        }
        // A held key whose KEY_UP we might miss (window/alt-tab) → reset.
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            clear_input();
            return Continue;
        }
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            const bool down = event.type == SDL_EVENT_KEY_DOWN; // key-repeat just re-sets true (idempotent)
            switch (event.key.key) {
            case SDLK_Z: input_.up = down; break;
            case SDLK_S: input_.down = down; break;
            case SDLK_Q: input_.left = down; break;
            case SDLK_D: input_.right = down; break;
            case SDLK_LSHIFT: // dash is an edge, not a hold
                if (down && !event.key.repeat) { input_.dash_queued = true; }
                break;
            default: break;
            }
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            input_.firing = true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            input_.firing = false;
        }
        return Continue;
    }

    auto update(float dt) -> Propagation override
    {
        client::Session& session = engine_->session();

        // Apply the latest server snapshot (world stays live even if input is
        // blocked by the console).
        if (auto snap = session.take_snapshot()) { consume_snapshot(*snap); }

        // Floating combat numbers: stamp arrivals with the anim clock; the
        // render pass animates + expires them. Bounded so a hectic fight can't
        // grow the pool (oldest numbers are the ones nearly faded anyway).
        for (const proto::DamageEvent& ev : session.take_damage_events()) {
            float_nums_.push_back(FloatNum{ .x = ev.x, .y = ev.y, .t0 = anim_time_,
                                            .amount = ev.amount, .crit = ev.kind != 0 });
        }
        if (float_nums_.size() > 96) {
            float_nums_.erase(float_nums_.begin(),
                              float_nums_.begin() + static_cast<std::ptrdiff_t>(float_nums_.size() - 96));
        }
        if (!has_player_ && session.has_id()) { spawn_local_player(session.my_net_id()); }

        // Open the level-up card scene on the rising edge of a level-up.
        // ONLY when we're the top scene: the console (and pause menu) keep our
        // update running (their update returns Continue) but sit above us, and
        // a scene pushed now lands ABOVE them — visible-order breaks (the card
        // is on top for input but the console's render Stop hides it). Waiting
        // until they're closed makes the transition land cleanly on top.
        if (session.leveling() && !level_open_ && engine_->scenes().is_top(this)) {
            clear_input(); // hand focus to the card scene
            engine_->audio().play("levelup");
            engine_->scenes().push<LevelUpScene>(engine_);
            level_open_ = true;
        } else if (!session.leveling()) {
            level_open_ = false;
        }

        // The run ended: overlay the game-over screen (it owns the transition
        // back to the lobby — our update is blocked while it's on top). Same
        // top-scene gate as the level-up card.
        if (session.game_over() && !game_over_open_ && engine_->scenes().is_top(this)) {
            clear_input();
            engine_->audio().stop_music(); // the sting owns the soundscape
            engine_->audio().play(session.game_over_stats().won != 0 ? "win" : "defeat");
            engine_->scenes().push<GameOverScene>(engine_);
            game_over_open_ = true;
        } else if (!session.game_over()) {
            game_over_open_ = false;
        }

        // Music follows the fight: the boss track while an arena is up. music()
        // is idempotent + cross-fades, so re-stating the target per frame is free.
        if (!session.game_over()) {
            engine_->audio().music(arena_active_ ? "music_boss" : "music_game");
        }

        send_and_predict(dt);
        // Ease the local render position toward the (authoritative) predicted
        // Position so the 60 Hz snapshot snap doesn't jerk the camera/world.
        if (has_player_ && render_init_) {
            const Position& p = registry_.get<Position>(player_);
            const float dx = p.x - render_x_;
            const float dy = p.y - render_y_;
            if ((dx * dx) + (dy * dy) > 256.0f * 256.0f) {
                render_x_ = p.x; // respawn/teleport: cut, don't glide across the map
                render_y_ = p.y;
            } else {
                const float a = 1.0f - std::exp(-14.0f * dt); // ~100 ms settle
                render_x_ += dx * a;
                render_y_ += dy * a;
            }
        }
        anim_time_ += dt;           // shared clock for all animation clips
        time_since_snapshot_ += dt; // drives remote interpolation (alpha toward the newest snapshot)
        shake_amp_ *= std::exp(-9.0f * dt); // camera shake settles in ~0.3 s

        // Last-heart warning: the vignette pulses slowly (drawn in render_game)
        // and a heartbeat thumps on each pulse — you should FEEL one heart left
        // without reading the HUD. Silent while downed (0) or safe (>= 2).
        if (my_health_ == 1 && has_player_ && !session.game_over()) {
            heartbeat_next_ -= dt;
            if (heartbeat_next_ <= 0.0f) {
                engine_->audio().play("heartbeat");
                heartbeat_next_ = 1.2f;
            }
        } else {
            heartbeat_next_ = 0.0f; // re-arm: the first pulse lands immediately
        }
        return Continue;
    }

    auto render(float /*alpha*/) -> Propagation override
    {
        render_game(engine_->renderer());
        return Continue;
    }

    GameScene(const GameScene&) = delete;
    GameScene(GameScene&&) = delete;
    GameScene& operator=(const GameScene&) = delete;
    GameScene& operator=(GameScene&&) = delete;
    // Drop the HUD thunk we published (it captures `this`) before we die.
    ~GameScene() override { engine_->set_hud_render(nullptr); }

private:
    void spawn_local_player(std::uint32_t net_id)
    {
        my_net_id_ = net_id;
        if (const auto it = remotes_.find(net_id); it != remotes_.end()) {
            registry_.destroy(it->second);
            remotes_.erase(it);
        }
        player_ = registry_.create();
        registry_.assign(player_, Position{ .x = 0, .y = 0 });
        registry_.assign(player_, Velocity{ .dx = 0, .dy = 0 });
        has_player_ = true;
    }

    void send_and_predict(float dt)
    {
        const bool downed = my_health_ == 0; // dead: no control until respawn

        // Movement from the held-key set (built from events -> respects Stop).
        std::int8_t mx = 0;
        std::int8_t my = 0;
        if (!downed) {
            mx = static_cast<std::int8_t>((input_.right ? 1 : 0) - (input_.left ? 1 : 0));
            my = static_cast<std::int8_t>((input_.down ? 1 : 0) - (input_.up ? 1 : 0));
        }

        // Aim = direction from the player's SCREEN position to the cursor —
        // not from the screen center: the camera can be elsewhere (boss arena
        // lock), which used to skew every shot. draw_ctx_ holds the previous
        // frame's camera offset; one frame of lag is irrelevant for an aim
        // direction. Aiming isn't a blocked action (only firing is).
        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
        SDL_GetMouseState(&mouse_x, &mouse_y);
        float px_screen = static_cast<float>(engine_->width()) * 0.5f;
        float py_screen = static_cast<float>(engine_->height()) * 0.5f;
        if (has_player_) {
            // Aim from the player's on-screen position, which is the SMOOTHED
            // render position (where the sprite is drawn), not the raw predicted
            // Position — otherwise aim would jitter with the correction.
            px_screen = render_x_ + draw_ctx_.ox;
            py_screen = render_y_ + draw_ctx_.oy;
        }
        float aim_x = mouse_x - px_screen;
        float aim_y = mouse_y - py_screen;
        const float len = std::sqrt((aim_x * aim_x) + (aim_y * aim_y));
        if (len > 0.001f) {
            aim_x /= len;
            aim_y /= len;
        } else {
            aim_x = 1.0f;
            aim_y = 0.0f;
        }
        // Trainer (SK_TRAINER=1): snap aim to the nearest enemy. No-op otherwise.
        // Wrap in `if (input_.firing)` to only assist while shooting (subtler).
        trainer::autoaim<Remote>(registry_, player_, has_player_, aim_x, aim_y);
        const std::uint8_t firing = (input_.firing && !downed) ? 1 : 0;

        // Dash: forward the edge to the server and mirror it locally so the
        // predicted position bursts in the same tick. The local Dash uses base
        // constants — upgraded cooldown/charges live server-side; position is
        // corrected by snapshots either way.
        std::uint8_t dash_flag = 0;
        if (input_.dash_queued && !downed) {
            dash_flag = 1;
            const bool moving = mx != 0 || my != 0;
            if (start_dash(local_dash_, moving ? static_cast<float>(mx) : aim_x,
                           moving ? static_cast<float>(my) : aim_y)) {
                shake_amp_ = std::max(shake_amp_, 3.0f); // dash kick
                engine_->audio().play("dash");
            }
        }
        input_.dash_queued = false;

        // Dash trail: sample the predicted position while bursting; ghosts
        // fade fast (render_game draws them), so the ring stays tiny.
        if (has_player_ && local_dash_.burst_remaining > 0.0f) {
            const Position& pos = registry_.get<Position>(player_);
            if (trail_.size() >= 12) { trail_.erase(trail_.begin()); }
            trail_.push_back({ .x = pos.x, .y = pos.y, .t0 = anim_time_, .radius = 0, .pickup = false });
        }

        engine_->session().send_input(proto::Input{ .move_x = mx, .move_y = my, .aim_x = aim_x,
                                                    .aim_y = aim_y, .firing = firing, .dash = dash_flag });

        my_moving_ = false;

        if (has_player_ && !downed) {
            const float speed = my_move_speed_ > 0 ? static_cast<float>(my_move_speed_) : PLAYER_SPEED;
            Velocity& vel = registry_.get<Velocity>(player_);
            apply_input(vel, mx, my, speed);
            tick_dash(local_dash_, vel, speed, dt);
            Position& pos = registry_.get<Position>(player_);
            pos.x += vel.dx * dt;
            pos.y += vel.dy * dt;
            // Boss arena: clamp the prediction like the server clamps the sim
            // (core's arena system) — otherwise the wall would rubber-band.
            if (arena_active_) {
                pos.x = std::clamp(pos.x, arena_cx_ - arena_hw_, arena_cx_ + arena_hw_);
                pos.y = std::clamp(pos.y, arena_cy_ - arena_hh_, arena_cy_ + arena_hh_);
            }
            // Terrain: the SAME deterministic pushout the kernel runs, so
            // rocks/trunks are solid in the prediction too (no rubber-band).
            // 12 px = the player's kernel Radius (create_player); the clear
            // circle mirrors the arena's flattened ground (core's arena system
            // writes the same diag-radius circle server-side).
            if (const std::uint32_t seed = engine_->session().world_seed(); seed != 0) {
                const float clear_r =
                  arena_active_ ? std::sqrt((arena_hw_ * arena_hw_) + (arena_hh_ * arena_hh_)) : 0.0f;
                shared::map::resolve_terrain(terrain_cache_, seed, pos.x, pos.y, 12.0f,
                                             arena_cx_, arena_cy_, clear_r);
            }
            my_moving_ = vel.dx != 0.0f || vel.dy != 0.0f;
        }

        // 8-way facing (twin-stick): the gun follows the AUTHORITATIVE aim (off
        // the wire — so autofire/overrides show), legs win while running with the
        // trigger up, and a dash locks its direction (client-predicted for snap).
        // firing is authoritative too: the shoot pose matches when bullets fire.
        if (local_dash_.burst_remaining > 0.0f) {
            my_dir_x_ = local_dash_.dir_x;
            my_dir_y_ = local_dash_.dir_y;
        } else if (my_moving_ && !auth_firing_) {
            const Velocity& vel = registry_.get<Velocity>(player_);
            my_dir_x_ = vel.dx;
            my_dir_y_ = vel.dy;
        } else {
            my_dir_x_ = auth_aim_x_;
            my_dir_y_ = auth_aim_y_;
        }

        // Death clip: remember when we went down, play once from there.
        if (downed && my_death_start_ < 0.0f) {
            my_death_start_ = anim_time_;
            engine_->audio().play("downed");
        }
        if (!downed && my_death_start_ >= 0.0f) { engine_->audio().play("revive"); }
        if (!downed) { my_death_start_ = -1.0f; }
    }

    // Decode a snapshot packet (full or delta) into a complete SnapshotState
    // via the shared codec, apply it, then remember + ack it — the state we
    // just applied is what the server may delta against next.
    void consume_snapshot(std::span<const std::byte> packet)
    {
        if (packet.empty()) { return; }
        const auto tag = static_cast<proto::MsgType>(packet[0]);
        const std::span<const std::byte> payload = packet.subspan(1);
        if (!field_counts_ready_) { // stable after mod load; the codec needs blob shapes
            script_field_counts_ = mod::networked_field_counts(engine_->mods().scripts());
            field_counts_ready_ = true;
        }

        std::optional<proto::SnapshotState> state;
        if (tag == proto::MsgType::Snapshot) {
            state = proto::decode_full(payload, script_field_counts_);
        } else if (tag == proto::MsgType::SnapshotDelta) {
            proto::ByteReader peek(payload);
            const auto header = peek.get<proto::DeltaHeader>();
            if (!header) { return; }
            const proto::SnapshotState* baseline = nullptr;
            for (const proto::SnapshotState& past : snap_history_) {
                if (past.tick == header->baseline_tick) {
                    baseline = &past;
                    break;
                }
            }
            // Baseline fell out of our ring: skip (and don't ack) — the server
            // falls back to full snapshots once our acks stall.
            if (baseline == nullptr) { return; }
            state = proto::decode_delta(payload, *baseline, script_field_counts_);
        }
        if (!state) { return; }
        apply_state(*state);
        engine_->session().set_acked(state->tick);
        snap_history_.push_back(std::move(*state));
        if (snap_history_.size() > snap_history_len) { snap_history_.pop_front(); }
    }

    void apply_state(const proto::SnapshotState& state)
    {
        level_ = state.level;
        xp_frac_ = state.xp_frac;
        if (state.wave != wave_ && state.wave > 1) { // wave banner (skip the initial wave 1)
            banner_until_ = anim_time_ + 2.5f;
            engine_->audio().play("wave");
        }
        wave_ = state.wave;

        // Listener for positional SFX = the local player (previous-snapshot pos
        // is fine: falloff over ~1000 px doesn't care about a 0.5 px step).
        client::Audio& audio = engine_->audio();
        float lis_x = 0.0f;
        float lis_y = 0.0f;
        if (has_player_) {
            const Position& me = registry_.get<Position>(player_);
            lis_x = me.x;
            lis_y = me.y;
        }

        std::unordered_set<std::uint32_t> seen;
        for (const proto::EntityRec& rec : state.entities) {
            const float ex = proto::from_half_px(rec.qx);
            const float ey = proto::from_half_px(rec.qy);
            // Each entity carries its networked script components as a blob.
            proto::ByteReader blob(state.script_of(rec));
            std::vector<mod::NetComp> comps = mod::read_networked(blob, engine_->mods().scripts());

            if (has_player_ && rec.id == my_net_id_) {
                registry_.get<Position>(player_) = { .x = ex, .y = ey }; // snap correction
                if (!render_init_) { render_x_ = ex; render_y_ = ey; render_init_ = true; }
                if (rec.health < my_health_) {
                    shake_amp_ = 7.0f; // ouch: kick the camera
                    audio.play("hurt");
                    vignette_until_ = anim_time_ + 0.45f; // red edge flash
                }
                my_health_ = rec.health;      // current hearts
                my_max_hearts_ = rec.variant; // max hearts
                my_move_speed_ = rec.move_speed;
                my_scale_ = proto::dequantize_scale(rec.scale_q);
                // Mirror kernel stats into the render registry for the Lua HUD
                // (view:get(Hearts/Speed/Scale) dispatches through the shared table).
                set_local(Hearts{ .current = static_cast<std::int16_t>(rec.health),
                                  .max = static_cast<std::int16_t>(rec.variant) });
                set_local(Speed{ .value = static_cast<float>(rec.move_speed) });
                set_local(Scale{ .value = my_scale_ });
                script_state_[rec.id] = std::move(comps);
                seen.insert(rec.id);
                continue;
            }

            seen.insert(rec.id);
            script_state_[rec.id] = std::move(comps);
            const auto it = remotes_.find(rec.id);
            if (it == remotes_.end()) {
                const core::Entity e = registry_.create();
                registry_.assign(e, Position{ .x = ex, .y = ey });
                registry_.assign(e, PrevPosition{ .x = ex, .y = ey });
                registry_.assign(e, Remote{ .kind = rec.kind, .net_id = rec.id, .health = rec.health,
                                            .variant = rec.variant });
                remotes_[rec.id] = e;
                // A projectile's first sighting is its muzzle flash — the only
                // "shot fired" signal the client gets (player and enemy alike).
                // The bullet's variant byte picks a per-archetype sound when one
                // is bound ("shoot_<variant>": core ships shoot_1 for hostile
                // arrows; mods bind theirs via mod:sound), else the base "shoot".
                if (rec.kind == static_cast<std::uint8_t>(proto::EntityKind::Projectile)) {
                    const std::string variant_shot = "shoot_" + std::to_string(rec.variant);
                    audio.play_at(audio.has(variant_shot) ? variant_shot : "shoot",
                                  ex, ey, lis_x, lis_y);
                }
                // Boss arena entrance: an arena archetype's FIRST sighting is
                // its spawn = the arena's fixed center (the wall + name banner
                // appear; the camera keeps following the player).
                if (!arena_active_
                    && rec.kind == static_cast<std::uint8_t>(proto::EntityKind::Enemy)) {
                    const mod::EnemyDef* def = engine_->mods().enemies().by_wire(rec.variant);
                    if (def != nullptr && def->arena_w > 0.0f) {
                        arena_active_ = true;
                        arena_net_id_ = rec.id;
                        arena_cx_ = ex;
                        arena_cy_ = ey;
                        arena_hw_ = def->arena_w;
                        arena_hh_ = def->arena_h;
                        boss_banner_ = def->label;
                        boss_banner_until_ = anim_time_ + 2.5f;
                        audio.play("boss"); // the sting rides over the music switch
                    }
                }
            } else {
                Position& pos = registry_.get<Position>(it->second);
                registry_.get<PrevPosition>(it->second) = { .x = pos.x, .y = pos.y };
                Remote& rem = registry_.get<Remote>(it->second);
                // Animation state from the snapshot step: Move vs Idle + facing.
                // The flip needs a full quantization step (0.5 px) of hysteresis
                // — a nearly-vertical chase has a tiny alternating-sign x step
                // that would otherwise thrash the sprite left/right.
                const float step_x = ex - pos.x;
                const float step_y = ey - pos.y;
                rem.moving = std::abs(step_x) + std::abs(step_y) > 0.1f;
                if (step_x > 0.5f) {
                    rem.face = 1.0f;
                } else if (step_x < -0.5f) {
                    rem.face = -1.0f;
                }
                // Remote players' 8-way clips follow their movement; steps under
                // a quantization step are noise and keep the previous direction.
                if (std::abs(step_x) + std::abs(step_y) > 0.5f) {
                    rem.dir_x = step_x;
                    rem.dir_y = step_y;
                }
                // Teleport tell: an EXISTING enemy jumping this far in one
                // snapshot blinked (Vampire/Archmage/GM evasion, the anti-pin
                // arena hop) — a whoosh sells it. Versioned entity ids mean a
                // reused slot is a NEW entry, never a fake jump.
                if (rec.kind == static_cast<std::uint8_t>(proto::EntityKind::Enemy)
                    && (step_x * step_x) + (step_y * step_y) > 250.0f * 250.0f) {
                    audio.play_at("blink", ex, ey, lis_x, lis_y);
                }
                pos = { .x = ex, .y = ey };
                if (rec.health < rem.health) {
                    rem.flash_until = anim_time_ + 0.12f; // hit!
                    audio.play_at("hit", ex, ey, lis_x, lis_y);
                }
                // Boss phase escalation: the arena boss's bar crossing a rage
                // threshold gets a sting + bar flash + rattle — the fight
                // audibly shifts gears (brains escalate around these marks).
                if (arena_active_ && rec.id == arena_net_id_) {
                    const auto crossed = [&](std::uint8_t mark) {
                        return rem.health > mark && rec.health <= mark;
                    };
                    if (crossed(127) || crossed(63)) { // 50% / 25%
                        bar_flash_until_ = anim_time_ + 0.6f;
                        shake_amp_ = std::max(shake_amp_, 4.0f);
                        audio.play("sting");
                    }
                }
                rem.health = rec.health;
                rem.scale = proto::dequantize_scale(rec.scale_q);
                // Any fx VALUE change restarts the clip clock (telegraph -> charge
                // must replay from frame 0, not inherit the wind-up's time).
                if (rec.fx != rem.fx && rec.fx != 0) {
                    rem.fx_start = anim_time_;
                    // The arena boss winding up or striking rattles the camera.
                    if (arena_active_ && rec.id == arena_net_id_) {
                        shake_amp_ = std::max(shake_amp_, 5.0f);
                    }
                    // The Mimic's reveal: its only fx is the one-way wake — the
                    // "prop" springing to life deserves its chirp.
                    if (rec.fx == 1
                        && rec.kind == static_cast<std::uint8_t>(proto::EntityKind::Enemy)) {
                        const mod::EnemyDef* def = engine_->mods().enemies().by_wire(rec.variant);
                        if (def != nullptr && def->id == "core:mimic") {
                            audio.play_at("wake", ex, ey, lis_x, lis_y);
                        }
                    }
                }
                rem.fx = rec.fx;
                if (rec.kind == static_cast<std::uint8_t>(proto::EntityKind::Player)) {
                    // Downed players stay in the snapshot with 0 hearts.
                    if (rec.health == 0 && rem.death_start < 0.0f) {
                        rem.death_start = anim_time_;
                        audio.play("downed"); // a teammate falling matters anywhere on the map
                    }
                    if (rec.health != 0 && rem.death_start >= 0.0f) { audio.play("revive"); }
                    if (rec.health != 0) { rem.death_start = -1.0f; }
                }
            }
        }

        // The decoded state is COMPLETE (deltas were merged over their baseline
        // by the codec), so absence still means despawn — for fulls and deltas.
        // A despawn is the only "it died / got picked up" signal the client
        // gets, so it doubles as the poof-effect trigger (players keep their
        // Death clip instead).
        for (auto it = remotes_.begin(); it != remotes_.end();) {
            if (!seen.contains(it->first)) {
                const Remote& rem = registry_.get<Remote>(it->second);
                if (rem.kind != static_cast<std::uint8_t>(proto::EntityKind::Player)
                    && poofs_.size() < 48) {
                    const Position& pos = registry_.get<Position>(it->second);
                    const bool pickup = rem.kind == static_cast<std::uint8_t>(proto::EntityKind::XpOrb)
                                     || rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Heart)
                                     || rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Chest);
                    float arch_scale = 1.0f; // size the burst to what vanished (boss >> bandit)
                    if (const mod::EnemyDef* def = engine_->mods().enemies().by_wire(rem.variant)) {
                        arch_scale = def->scale;
                    }
                    poofs_.push_back({ .x = pos.x, .y = pos.y, .t0 = anim_time_,
                                       .radius = pickup ? 10.0f : 20.0f * rem.scale * arch_scale,
                                       .pickup = pickup });
                    if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::XpOrb)) {
                        audio.play_at("pickup", pos.x, pos.y, lis_x, lis_y);
                    } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Heart)
                               || rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Chest)) {
                        audio.play_at("heart", pos.x, pos.y, lis_x, lis_y);
                    } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Enemy)) {
                        audio.play_at("death", pos.x, pos.y, lis_x, lis_y);
                    } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Projectile)
                               && rem.variant == 6 && audio.has("pop")) {
                        // The Elder Ent's seed blooming into its petal ring.
                        audio.play_at("pop", pos.x, pos.y, lis_x, lis_y);
                    }
                }
                if (arena_active_ && it->first == arena_net_id_) {
                    arena_active_ = false; // boss down: drop the wall
                    // The kill deserves a moment: a white screen flash + one
                    // BIG expanding ring on top of the normal death poof.
                    boss_flash_until_ = anim_time_ + 0.35f;
                    shake_amp_ = std::max(shake_amp_, 8.0f);
                    const Position& pos = registry_.get<Position>(it->second);
                    poofs_.push_back({ .x = pos.x, .y = pos.y, .t0 = anim_time_,
                                       .radius = 110.0f, .pickup = false });
                }
                registry_.destroy(it->second);
                it = remotes_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = script_state_.begin(); it != script_state_.end();) {
            it = seen.contains(it->first) ? std::next(it) : script_state_.erase(it);
        }

        // Trailer: authoritative per-player aim + trigger. Drives sprite facing
        // and the shoot pose so a server-side aim override (autofire) shows up,
        // and remote players aim correctly instead of only facing their motion.
        for (const proto::PlayerAim& pa : state.aims) {
            const float ax = proto::dequantize_aim(pa.aim_qx);
            const float ay = proto::dequantize_aim(pa.aim_qy);
            if (has_player_ && pa.id == my_net_id_) {
                auth_aim_x_ = ax;
                auth_aim_y_ = ay;
                auth_firing_ = pa.firing != 0;
                // Dash params + state are authoritative (the game/Lua sets
                // max_charges/cooldown_max — the client can't guess them). Correct
                // the prediction here; the burst (burst_remaining) stays predicted,
                // and tick_dash smooths the cooldown between snapshots.
                local_dash_.max_charges = pa.dash_max;
                local_dash_.charges = pa.dash_charges;
                local_dash_.cooldown = proto::ms_to_seconds(pa.dash_cd_ms);
                local_dash_.cooldown_max = proto::ms_to_seconds(pa.dash_cd_max_ms);
            } else if (const auto it = remotes_.find(pa.id); it != remotes_.end()) {
                Remote& rem = registry_.get<Remote>(it->second);
                rem.aim_x = ax;
                rem.aim_y = ay;
                rem.firing = pa.firing != 0;
            }
        }
        time_since_snapshot_ = 0.0f;
    }

    void render_game(SDL_Renderer* r)
    {
        // Camera target: the local player — unless a MOD locked it somewhere
        // (ctx:camera_lock, e.g. a cutscene or a future boss intro). Lock
        // changes glide (start_cam_blend) instead of cutting.
        float cam_x = 0.0f;
        float cam_y = 0.0f;
        if (draw_ctx_.cam_locked) {
            cam_x = draw_ctx_.cam_x;
            cam_y = draw_ctx_.cam_y;
        } else if (has_player_) {
            // Smoothed local position (not the raw predicted Position) so a
            // snapshot correction pans the world instead of jerking it.
            cam_x = render_x_;
            cam_y = render_y_;
        }
        if (draw_ctx_.cam_locked != cam_was_locked_) {
            cam_was_locked_ = draw_ctx_.cam_locked;
            start_cam_blend();
        }
        if (anim_time_ < cam_blend_until_) {
            const float t = 1.0f - ((cam_blend_until_ - anim_time_) / cam_blend_len);
            const float s = t * t * (3.0f - (2.0f * t)); // smoothstep
            cam_x = cam_blend_from_x_ + ((cam_x - cam_blend_from_x_) * s);
            cam_y = cam_blend_from_y_ + ((cam_y - cam_blend_from_y_) * s);
        }
        last_cam_x_ = cam_x;
        last_cam_y_ = cam_y;
        const float ww = static_cast<float>(engine_->width());
        const float wh = static_cast<float>(engine_->height());
        // Camera shake: a decaying two-frequency jitter on the world offset.
        // Plugin draws shake too (they share draw_ctx_) — they're world-space.
        const float shake_x = shake_amp_ > 0.05f ? std::sin(anim_time_ * 71.0f) * shake_amp_ : 0.0f;
        const float shake_y = shake_amp_ > 0.05f ? std::cos(anim_time_ * 57.0f) * shake_amp_ : 0.0f;
        const float ox = (ww * 0.5f) - cam_x + shake_x;
        const float oy = (wh * 0.5f) - cam_y + shake_y;
        draw_ctx_.ox = ox; // keep the plugin draw context's camera current
        draw_ctx_.oy = oy;
        if (has_player_) { // ctx:play_at attenuates from the local player
            const Position& me = registry_.get<Position>(player_);
            draw_ctx_.listener_x = me.x;
            draw_ctx_.listener_y = me.y;
        }

        if (engine_->session().world_seed() != 0) {
            draw_ground(r, cam_x, cam_y, ww, wh, ox, oy); // biome-tiled chunks
        } else {
            draw_background(r, cam_x, cam_y, ww, wh, ox, oy); // flat lobby world
        }
        draw_terrain_deco(r, cam_x, cam_y, ww, wh, ox, oy); // flat ground clutter

        // Wave banner: big centered "WAVE N", fading out over its last second.
        // Skipped when a modal (console/level-up/game-over) is stacked above —
        // foreground-list text would bleed over it.
        if (anim_time_ < banner_until_ && engine_->scenes().is_top(this)) {
            const float remain = banner_until_ - anim_time_;
            const float alpha = std::clamp(remain, 0.0f, 1.0f); // fade the last second
            client::Gui& ui = engine_->gui();
            ui.text_centered(ww * 0.5f, wh * 0.22f, "WAVE " + std::to_string(wave_),
                             client::GuiColor{ 255, 225, 140,
                                               static_cast<std::uint8_t>(alpha * 255.0f) },
                             16.0f * ui.scale());
        }

        // Arena boundary: a double rectangle around the FIXED center so the
        // wall players are clamped to actually reads on screen. Plus the boss
        // name banner during the entrance.
        if (arena_active_ && engine_->scenes().is_top(this)) {
            ImDrawList* fx = ImGui::GetBackgroundDrawList();
            const ImVec2 lo(arena_cx_ - arena_hw_ + ox, arena_cy_ - arena_hh_ + oy);
            const ImVec2 hi(arena_cx_ + arena_hw_ + ox, arena_cy_ + arena_hh_ + oy);
            fx->AddRect(lo, hi, IM_COL32(255, 200, 90, 170), 0.0f, 0, 3.0f);
            fx->AddRect(ImVec2(lo.x - 6.0f, lo.y - 6.0f), ImVec2(hi.x + 6.0f, hi.y + 6.0f),
                        IM_COL32(255, 200, 90, 60), 0.0f, 0, 8.0f);
        }
        if (anim_time_ < boss_banner_until_ && engine_->scenes().is_top(this)) {
            const float remain = boss_banner_until_ - anim_time_;
            const float alpha = std::clamp(remain, 0.0f, 1.0f);
            client::Gui& ui = engine_->gui();
            std::string shout = boss_banner_;
            std::transform(shout.begin(), shout.end(), shout.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            ui.text_centered(ww * 0.5f, wh * 0.32f, shout,
                             client::GuiColor{ 255, 205, 110,
                                               static_cast<std::uint8_t>(alpha * 255.0f) },
                             12.0f * ui.scale());
        }

        // Death poofs + dash trail: short-lived world-space rings (ImGui bg
        // list composites over SDL — fine, they're bursts ON things).
        if (engine_->scenes().is_top(this)) {
            ImDrawList* fx = ImGui::GetBackgroundDrawList();
            constexpr float poof_life = 0.35f;
            std::erase_if(poofs_, [&](const Poof& p) { return anim_time_ - p.t0 > poof_life; });
            for (const Poof& p : poofs_) {
                const float age = (anim_time_ - p.t0) / poof_life; // 0..1
                const auto alpha = static_cast<int>((1.0f - age) * 200.0f);
                const ImVec2 at(p.x + ox, p.y + oy);
                const ImU32 col = p.pickup ? IM_COL32(160, 240, 255, alpha)
                                           : IM_COL32(255, 180, 90, alpha);
                fx->AddCircle(at, 4.0f + (age * p.radius), col, 0, p.pickup ? 2.0f : 3.0f);
                if (!p.pickup) { // inner flash on kills
                    fx->AddCircleFilled(at, (1.0f - age) * p.radius * 0.35f,
                                        IM_COL32(255, 230, 170, alpha / 2));
                }
            }
            constexpr float trail_life = 0.22f;
            std::erase_if(trail_, [&](const Poof& g) { return anim_time_ - g.t0 > trail_life; });
            for (const Poof& g : trail_) {
                const float age = (anim_time_ - g.t0) / trail_life;
                fx->AddCircleFilled(ImVec2(g.x + ox, g.y + oy), (1.0f - age) * 9.0f + 2.0f,
                                    IM_COL32(140, 210, 255, static_cast<int>((1.0f - age) * 110.0f)));
            }
        }

        // "Net" = dev debug only (fps/ms + your id). Wave/level/XP + per-player
        // stats now live in the Lua mod:hud stats panel, drawn LAST (topmost).
        ImGui::Begin("Net", nullptr, ImGuiWindowFlags_NoBackground);
        ImGui::Text("you: %s (id %u)", engine_->session().name().c_str(), my_net_id_);
        ImGui::Text("%.1f fps (%.2f ms)", static_cast<double>(ImGui::GetIO().Framerate),
                    1000.0 / std::max(1.0, static_cast<double>(ImGui::GetIO().Framerate)));
        ImGui::End();

        SDL_Texture* enemy_tex = textures_.get(asset_enemy);

        // ImGui draw-list overlays (name labels, plugin aura hooks) composite
        // after ALL SDL — including a scene stacked above us that draws with SDL
        // (the level-up menu). Only emit them when we're the top scene, else
        // they'd float over that menu. The world itself still renders (dimmed
        // behind the menu); we skip only the ImGui-list bits that would bleed.
        const bool overlays = engine_->scenes().is_top(this);

        const float t = std::min(time_since_snapshot_ * static_cast<float>(proto::snapshot_hz), 1.0f);

        // Player screen positions (self + remotes), for idle enemies that turn
        // toward the nearest player (archers at standoff).
        player_screen_.clear();
        if (has_player_) {
            player_screen_.push_back({ ox + render_x_, oy + render_y_ });
        }
        registry_.view<Position, PrevPosition, Remote>().each(
          [&](core::Entity, const Position& p, const PrevPosition& prev, const Remote& rem) {
              if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Player)) {
                  player_screen_.push_back({ ox + prev.x + ((p.x - prev.x) * t),
                                             oy + prev.y + ((p.y - prev.y) * t) });
              }
          });
        // ---- the y-sorted world pass -------------------------------------
        // Ground items (orbs/hearts/chests) draw FLAT first; obstacles +
        // enemies + players collect into one list sorted by feet-y (bodies
        // pass BEHIND trees and in front of rocks — the map's z-axis);
        // projectiles are airborne and draw over everything at the end.
        world_items_.clear();
        shots_.clear();
        registry_.view<Position, PrevPosition, Remote>().each(
          [&](core::Entity, const Position& p, const PrevPosition& prev, const Remote& rem) {
              const float x = ox + prev.x + ((p.x - prev.x) * t);
              const float y = oy + prev.y + ((p.y - prev.y) * t);
              if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Enemy)) {
                  world_items_.push_back(WorldItem{
                    .key = y + 20.0f, .x = x, .y = y, .type = WorldItem::Enemy, .rem = &rem });
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Player)) {
                  world_items_.push_back(WorldItem{
                    .key = y + 20.0f, .x = x, .y = y, .type = WorldItem::RemotePlayer, .rem = &rem });
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Projectile)) {
                  float ddx = p.x - prev.x;
                  float ddy = p.y - prev.y;
                  const float dlen = std::sqrt((ddx * ddx) + (ddy * ddy));
                  shots_.push_back(Shot{ .x = x, .y = y,
                                         .dx = dlen > 0.01f ? ddx / dlen : 1.0f,
                                         .dy = dlen > 0.01f ? ddy / dlen : 0.0f,
                                         .variant = rem.variant });
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::XpOrb)) {
                  draw_xp_orb(r, x, y);
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Heart)) {
                  draw_heart_pickup(r, x, y);
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Chest)) {
                  draw_chest(r, x, y);
              } else {
                  draw_entity(r, x, y, nullptr, SDL_Color{ 220, 80, 80, 255 });
                  health_bar(r, x, y, rem.health);
              }
          });
        if (has_player_) {
            // Smoothed render position (matches the camera), not the raw
            // predicted Position — keeps the local player screen-centered.
            world_items_.push_back(WorldItem{ .key = oy + render_y_ + 20.0f,
                                              .x = ox + render_x_, .y = oy + render_y_,
                                              .type = WorldItem::LocalPlayer });
        }
        // Obstacles of the visible chunks (a margin for tall canopies), minus
        // anything on the arena's flattened ground.
        if (const std::uint32_t seed = engine_->session().world_seed(); seed != 0) {
            const float clear_r =
              arena_active_ ? std::sqrt((arena_hw_ * arena_hw_) + (arena_hh_ * arena_hh_)) : 0.0f;
            const auto c0x = static_cast<std::int32_t>(
              std::floor((cam_x - (ww * 0.5f) - 200.0f) / shared::map::chunk_size));
            const auto c1x = static_cast<std::int32_t>(
              std::floor((cam_x + (ww * 0.5f) + 200.0f) / shared::map::chunk_size));
            const auto c0y = static_cast<std::int32_t>(
              std::floor((cam_y - (wh * 0.5f) - 100.0f) / shared::map::chunk_size));
            const auto c1y = static_cast<std::int32_t>(
              std::floor((cam_y + (wh * 0.5f) + 320.0f) / shared::map::chunk_size));
            for (std::int32_t cj = c0y; cj <= c1y; ++cj) {
                for (std::int32_t ci = c0x; ci <= c1x; ++ci) {
                    for (const shared::map::Obstacle& ob : terrain_cache_.get(seed, ci, cj)) {
                        if (clear_r > 0.0f) {
                            const float dx = ob.x - arena_cx_;
                            const float dy = ob.y - arena_cy_;
                            if ((dx * dx) + (dy * dy) < clear_r * clear_r) { continue; }
                        }
                        world_items_.push_back(WorldItem{ .key = ob.y + ob.r + oy,
                                                          .x = ob.x + ox, .y = ob.y + oy,
                                                          .type = WorldItem::Obst, .ob = ob });
                    }
                }
            }
        }
        std::sort(world_items_.begin(), world_items_.end(),
                  [](const WorldItem& a, const WorldItem& b) { return a.key < b.key; });
        for (const WorldItem& item : world_items_) {
            switch (item.type) {
            case WorldItem::Enemy:
                draw_enemy(r, item.x, item.y, enemy_tex, *item.rem);
                health_bar(r, item.x, item.y, item.rem->health);
                break;
            case WorldItem::RemotePlayer: {
                const Remote& rem = *item.rem;
                if (overlays) { draw_object_hooks(item.x, item.y, script_state_for(rem.net_id)); }
                // Same rule as the local player: aim (authoritative) unless
                // running with the trigger up, then face movement.
                float fdx = rem.aim_x;
                float fdy = rem.aim_y;
                if (rem.moving && !rem.firing) {
                    fdx = rem.dir_x;
                    fdy = rem.dir_y;
                }
                draw_player(r, item.x, item.y,
                            PlayerAnim{ .dir_x = fdx, .dir_y = fdy, .moving = rem.moving,
                                        .firing = rem.firing, .death_start = rem.death_start },
                            rem.net_id, rem.scale, SDL_Color{ 220, 200, 80, 255 });
                // No health bar over players — the HUD hearts show life now.
                if (overlays) { label(item.x, item.y, engine_->session().name_of(rem.net_id)); }
                break;
            }
            case WorldItem::LocalPlayer: {
                if (overlays) { draw_object_hooks(item.x, item.y, script_state_for(my_net_id_)); }
                const float dash_frac = local_dash_.burst_remaining > 0.0f
                                          ? 1.0f - (local_dash_.burst_remaining / DASH_DURATION)
                                          : -1.0f;
                draw_player(r, item.x, item.y,
                            PlayerAnim{ .dir_x = my_dir_x_, .dir_y = my_dir_y_, .moving = my_moving_,
                                        .firing = auth_firing_, .dash_frac = dash_frac,
                                        .death_start = my_death_start_ },
                            my_net_id_, my_scale_, SDL_Color{ 80, 220, 100, 255 });
                if (overlays) { label(item.x, item.y, engine_->session().name()); }
                break;
            }
            case WorldItem::Obst:
                draw_obstacle(r, item);
                break;
            }
        }
        for (const Shot& shot : shots_) { // airborne: over rocks AND canopies
            draw_projectile(r, shot.x, shot.y, shot.variant, shot.dx, shot.dy, anim_time_);
        }

        // Floating combat numbers: pixel-font text rising off the hit point,
        // fading over its last third. Drawn AFTER the world so enemies never
        // walk over the numbers, with SDL (gui text) so a scene stacked above
        // still covers them.
        {
            constexpr float num_life = 0.8f;
            std::erase_if(float_nums_, [&](const FloatNum& n) { return anim_time_ - n.t0 > num_life; });
            client::Gui& ui = engine_->gui();
            for (const FloatNum& n : float_nums_) {
                const float age = (anim_time_ - n.t0) / num_life; // 0..1
                const float rise = 26.0f * age;
                const float fade = std::min(1.0f, (1.0f - age) * 3.0f); // full, then fade out
                const auto alpha = static_cast<std::uint8_t>(fade * 255.0f);
                const client::GuiColor col = n.crit ? client::GuiColor{ 255, 205, 110, alpha }
                                                    : client::GuiColor{ 255, 240, 210, alpha };
                const float px = (n.crit ? 5.0f : 3.0f) * ui.scale(); // scale-multiples stay crisp
                ui.text_centered(n.x + ox, n.y + oy - 22.0f - rise, std::to_string(n.amount),
                                 col, px);
            }
        }

        // Damage vignette: red edge glow on losing a heart, plus a slow pulse
        // that never leaves while you sit on your LAST heart. SDL draw — a
        // modal stacked above (cards/pause) still dims over it.
        {
            float strength = 0.0f;
            if (anim_time_ < vignette_until_) {
                strength = std::clamp((vignette_until_ - anim_time_) / 0.45f, 0.0f, 1.0f) * 140.0f;
            }
            if (my_health_ == 1 && has_player_) { // heartbeat-synced pulse (1.2 s period)
                const float pulse = 0.5f + (0.5f * std::sin(anim_time_ * (2.0f * std::numbers::pi_v<float> / 1.2f)));
                strength = std::max(strength, pulse * 60.0f);
            }
            if (strength > 2.0f) {
                draw_vignette(r, ww, wh, static_cast<std::uint8_t>(strength));
            }
        }

        // Boss-kill white flash: a short full-screen pop over the world.
        if (anim_time_ < boss_flash_until_) {
            const float remain = (boss_flash_until_ - anim_time_) / 0.35f;
            SDL_SetRenderDrawColor(r, 255, 250, 235,
                                   static_cast<Uint8>(std::clamp(remain, 0.0f, 1.0f) * 120.0f));
            const SDL_FRect all{ .x = 0.0f, .y = 0.0f, .w = ww, .h = wh };
            SDL_RenderFillRect(r, &all);
        }

        // Off-screen pointers: arrows on a FIXED RADIUS ring around the local
        // player (screen-edge arrows sat in peripheral vision and got missed),
        // pointing at teammates (green; red when down — the "go revive" cue),
        // the arena boss and any loot chest (gold).
        if (overlays && has_player_) {
            const float cx = ox + render_x_; // ring center = the local player
            const float cy = oy + render_y_;
            const float ring = 46.0f * engine_->gui().scale();
            const auto pointer = [&](float sx, float sy, ImU32 col, const std::string& label_text) {
                constexpr float margin = 34.0f;
                if (sx >= margin && sx <= ww - margin && sy >= margin && sy <= wh - margin) {
                    return; // on screen: the sprite itself is the cue
                }
                float dx = sx - cx;
                float dy = sy - cy;
                const float len = std::sqrt((dx * dx) + (dy * dy));
                if (len < 1.0f) { return; }
                dx /= len;
                dy /= len;
                const float ax = cx + (dx * ring);
                const float ay = cy + (dy * ring);
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                const ImVec2 tip(ax + (dx * 13.0f), ay + (dy * 13.0f));
                const ImVec2 left(ax - (dy * 8.0f), ay + (dx * 8.0f));
                const ImVec2 right(ax + (dy * 8.0f), ay - (dx * 8.0f));
                fg->AddTriangleFilled(tip, left, right, col);
                const ImVec2 sz = ImGui::CalcTextSize(label_text.c_str());
                // Label just INSIDE the ring (between player and arrow) so it
                // never covers the arrow head.
                fg->AddText(ImVec2(ax - (dx * 14.0f) - (sz.x * 0.5f), ay - (dy * 14.0f) - sz.y),
                            col, label_text.c_str());
            };
            registry_.view<Position, PrevPosition, Remote>().each(
              [&](core::Entity, const Position& p, const PrevPosition& prev, const Remote& rem) {
                  const float sx = ox + prev.x + ((p.x - prev.x) * t);
                  const float sy = oy + prev.y + ((p.y - prev.y) * t);
                  if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Player)) {
                      const bool down = rem.health == 0;
                      pointer(sx, sy, down ? IM_COL32(240, 80, 80, 230) : IM_COL32(120, 220, 130, 230),
                              engine_->session().name_of(rem.net_id));
                  } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Chest)) {
                      pointer(sx, sy, IM_COL32(255, 205, 110, 230), "LOOT");
                  } else if (arena_active_ && rem.net_id == arena_net_id_) {
                      pointer(sx, sy, IM_COL32(255, 205, 110, 230), "BOSS");
                  }
              });
        }

        // Boss fight: a big top-center health bar for the arena boss, drawn
        // after the world so nothing walks over it.
        if (overlays && arena_active_) { draw_boss_bar(r, ww); }

        // Minimap radar (top-right): teammates, boss, loot, enemy density —
        // the map answers "where is everyone" without swinging the camera.
        if (overlays && has_player_) { draw_minimap(r, ww, t); }

        // Plugin HUD hooks (mod:hud) LAST so the stats panel is topmost — the
        // world (enemies, orbs) never draws over it. When we're top, draw it
        // now; either way, publish it as a thunk so a scene stacked above us
        // (the level-up menu) can redraw it ON TOP of its own dim overlay —
        // the player picks upgrades based on these stats, so they must stay
        // visible + bright there.
        if (has_player_) {
            engine_->set_hud_render([this] { draw_stats_hud(); });
            if (engine_->scenes().is_top(this)) { draw_stats_hud(); }
        } else {
            engine_->set_hud_render(nullptr);
        }
    }

public:
    // Draw the local player's stats panel (mod:hud hooks) via the widget kit.
    // Public so the level-up scene can keep it on screen (see set_hud_render).
    void draw_stats_hud()
    {
        if (!has_player_) { return; }
        set_local(local_dash_); // predicted dash -> render registry for the HUD
        hud_ctx_.level = static_cast<int>(level_);
        hud_ctx_.wave = static_cast<int>(wave_);
        hud_ctx_.xp = static_cast<float>(xp_frac_) / 255.0f;
        // HUD verbosity, not gameplay input: a direct modifier query (instead of
        // the event-driven held-key set) is deliberate — the panel also redraws
        // over the level-up cards, where our events are blocked, and choosing a
        // card is exactly when the full stat breakdown matters.
        hud_ctx_.detail = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
        const client::DrawView view{ .scripts = &engine_->mods().scripts(),
                                     .comps = script_state_for(my_net_id_),
                                     .reg = &registry_,
                                     .entity = player_,
                                     .table = engine_->mods().state().bindings.get() };
        client::run_hud_hooks(engine_->mods(), hud_ctx_obj_, hud_ctx_, view);
    }

private:

    void draw_background(SDL_Renderer* r, float cam_x, float cam_y, float ww, float wh, float ox, float oy)
    {
        SDL_Texture* bg = textures_.get(asset_background);
        if (bg == nullptr) { return; }
        // Biome tint: per-tile color mod from the seeded region id — three
        // subtle palettes break the single-tile monotony for free.
        static constexpr SDL_Color biome_tints[3] = {
            { .r = 255, .g = 255, .b = 255, .a = 255 }, // neutral
            { .r = 222, .g = 244, .b = 212, .a = 255 }, // cool green
            { .r = 248, .g = 232, .b = 200, .a = 255 }, // warm autumn
        };
        const std::uint32_t seed = engine_->session().world_seed();
        const int i0 = static_cast<int>(std::floor((cam_x - (ww * 0.5f)) / bg_tile));
        const int i1 = static_cast<int>(std::ceil((cam_x + (ww * 0.5f)) / bg_tile));
        const int j0 = static_cast<int>(std::floor((cam_y - (wh * 0.5f)) / bg_tile));
        const int j1 = static_cast<int>(std::ceil((cam_y + (wh * 0.5f)) / bg_tile));
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                if (seed != 0) {
                    const SDL_Color tint = biome_tints[shared::map::biome_at(
                      seed, (static_cast<float>(i) + 0.5f) * bg_tile,
                      (static_cast<float>(j) + 0.5f) * bg_tile)];
                    SDL_SetTextureColorMod(bg, tint.r, tint.g, tint.b);
                }
                const SDL_FRect dst{ .x = (static_cast<float>(i) * bg_tile) + ox,
                                     .y = (static_cast<float>(j) * bg_tile) + oy,
                                     .w = bg_tile, .h = bg_tile };
                SDL_RenderTexture(r, bg, nullptr, &dst);
            }
        }
        SDL_SetTextureColorMod(bg, 255, 255, 255); // the texture cache shares it
    }

    // ---- biome ground: composited chunk textures --------------------------
    // Each 512 px chunk composes ONCE into a render-target texture, then draws
    // as a single quad (4-9/frame). Recipe v5 (prototyped in PIL against the
    // real assets, constants identical): the ground is the FLOOR TILESET cut
    // on its true units — grass/dirt/gravel are 16x16 period tiles, snow/sand
    // each have a light + dark 80x32 shade tile. The chunk samples a MATERIAL
    // per 16 px sub-cell (warped biome + a low-frequency shade blob), paints
    // each cell from its tile with the source anchored to WORLD coordinates
    // (patterns run continuously across cells and chunk borders), and any
    // material change (biome OR shade) dithers with 4 px squares of both
    // sides. Accents: dirt specks on grass, gravel specks on snow.

    // Materials: 0 grass, 1 sand light, 2 sand dark, 3 snow light, 4 snow dark.
    static constexpr const char* mat_paths[5] = {
        "assets/map/tile_grass.png", "assets/map/tile_sand_a.png", "assets/map/tile_sand_b.png",
        "assets/map/tile_snow_a.png", "assets/map/tile_snow_b.png",
    };
    static constexpr SDL_Color mat_flat[5] = { // fallback if art is missing
        { .r = 60, .g = 122, .b = 8, .a = 255 },    { .r = 240, .g = 195, .b = 145, .a = 255 },
        { .r = 227, .g = 168, .b = 106, .a = 255 }, { .r = 231, .g = 235, .b = 245, .a = 255 },
        { .r = 200, .g = 210, .b = 233, .a = 255 },
    };

    // Ground material at a world position: forest = grass; plain/snow pick
    // their light/dark shade tile from a low-frequency noise blob.
    static std::uint8_t material_at(std::uint32_t seed, float x, float y)
    {
        const std::uint8_t biome = shared::map::biome_at(seed, x, y);
        if (biome == 1) { return 0; }
        const bool dark = shared::map::vnoise(seed, x, y, 21) > 0.55f;
        return biome == 2 ? (dark ? 4 : 3) : (dark ? 2 : 1);
    }

    SDL_Texture* ground_chunk(SDL_Renderer* r, std::int32_t ci, std::int32_t cj,
                              std::uint32_t seed)
    {
        if (ground_seed_ != seed) {
            ground_cache_.clear();
            ground_seed_ = seed;
        }
        const std::uint64_t key = shared::map::chunk_key(ci, cj);
        if (const auto it = ground_cache_.find(key); it != ground_cache_.end()) {
            return it->second.get();
        }
        if (ground_cache_.size() > 96) { ground_cache_.clear(); } // ~1 MB each; recompose is cheap
        const auto side = static_cast<int>(shared::map::chunk_size);
        SDL_Texture* tex =
          SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, side, side);
        if (tex == nullptr) { return nullptr; }
        SDL_SetRenderTarget(r, tex);

        const float wx0 = static_cast<float>(ci) * shared::map::chunk_size;
        const float wy0 = static_cast<float>(cj) * shared::map::chunk_size;

        // Deterministic per-chunk stream (compose-time only, cheap).
        std::uint64_t stream = shared::map::mix(key ^ (static_cast<std::uint64_t>(seed) << 17));
        const auto next = [&stream] {
            stream = shared::map::mix(stream);
            return static_cast<float>(stream >> 40) / static_cast<float>(1ULL << 24);
        };

        constexpr int cell = 16;
        constexpr int cells = static_cast<int>(shared::map::chunk_size) / cell; // 32
        SDL_Texture* mat_tex[5];
        for (int m = 0; m < 5; ++m) { mat_tex[m] = textures_.get(mat_paths[m]); }

        // Material per sub-cell (sampled at cell centers, in WORLD space).
        std::array<std::uint8_t, static_cast<std::size_t>(cells * cells)> mat{};
        for (int j = 0; j < cells; ++j) {
            for (int i = 0; i < cells; ++i) {
                mat[static_cast<std::size_t>((j * cells) + i)] = material_at(
                  seed, wx0 + ((static_cast<float>(i) + 0.5f) * cell),
                  wy0 + ((static_cast<float>(j) + 0.5f) * cell));
            }
        }

        // Paint each cell from its material tile, source anchored to WORLD
        // coordinates: clip to the cell, draw the grid-aligned tile copies
        // covering it — patterns continue across cells AND chunk borders.
        const auto paint = [&](std::uint8_t m, float dx, float dy, float dw, float dh) {
            const SDL_Rect clip{ .x = static_cast<int>(dx), .y = static_cast<int>(dy),
                                 .w = static_cast<int>(dw), .h = static_cast<int>(dh) };
            if (mat_tex[m] == nullptr) {
                const SDL_Color c = mat_flat[m];
                SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
                const SDL_FRect fr{ .x = dx, .y = dy, .w = dw, .h = dh };
                SDL_RenderFillRect(r, &fr);
                return;
            }
            SDL_SetRenderClipRect(r, &clip);
            float tw = 0.0f;
            float th = 0.0f;
            SDL_GetTextureSize(mat_tex[m], &tw, &th);
            const float world_x = wx0 + dx;
            const float world_y = wy0 + dy;
            const float ax = dx - (world_x - (std::floor(world_x / tw) * tw));
            const float ay = dy - (world_y - (std::floor(world_y / th) * th));
            for (float ty = ay; ty < dy + dh; ty += th) {
                for (float tx = ax; tx < dx + dw; tx += tw) {
                    const SDL_FRect dst{ .x = tx, .y = ty, .w = tw, .h = th };
                    SDL_RenderTexture(r, mat_tex[m], nullptr, &dst);
                }
            }
            SDL_SetRenderClipRect(r, nullptr);
        };
        for (int j = 0; j < cells; ++j) {
            for (int i = 0; i < cells; ++i) {
                paint(mat[static_cast<std::size_t>((j * cells) + i)],
                      static_cast<float>(i * cell), static_cast<float>(j * cell),
                      static_cast<float>(cell), static_cast<float>(cell));
            }
        }

        // Dither along ANY material change (biome or shade): 4 px squares of
        // both sides scattered around boundary cells. Neighbor cells beyond
        // the chunk are sampled in world space, so chunk borders dither too.
        const auto mat_of = [&](int i, int j) {
            if (i >= 0 && i < cells && j >= 0 && j < cells) {
                return mat[static_cast<std::size_t>((j * cells) + i)];
            }
            return material_at(seed, wx0 + ((static_cast<float>(i) + 0.5f) * cell),
                               wy0 + ((static_cast<float>(j) + 0.5f) * cell));
        };
        const auto square4 = [&](std::uint8_t m, float dx, float dy) {
            if (dx < 0.0f || dy < 0.0f || dx > shared::map::chunk_size - 4.0f
                || dy > shared::map::chunk_size - 4.0f) {
                return; // the neighbor chunk draws its own side of the seam
            }
            if (mat_tex[m] == nullptr) {
                const SDL_Color c = mat_flat[m];
                SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
                const SDL_FRect fr{ .x = dx, .y = dy, .w = 4.0f, .h = 4.0f };
                SDL_RenderFillRect(r, &fr);
                return;
            }
            float tw = 0.0f;
            float th = 0.0f;
            SDL_GetTextureSize(mat_tex[m], &tw, &th);
            const SDL_FRect src{ .x = next() * (tw - 4.0f), .y = next() * (th - 4.0f),
                                 .w = 4.0f, .h = 4.0f };
            const SDL_FRect dst{ .x = dx, .y = dy, .w = 4.0f, .h = 4.0f };
            SDL_RenderTexture(r, mat_tex[m], &src, &dst);
        };
        for (int j = 0; j < cells; ++j) {
            for (int i = 0; i < cells; ++i) {
                const std::uint8_t self = mat[static_cast<std::size_t>((j * cells) + i)];
                std::uint8_t other = self;
                for (const auto& [di, dj] : { std::pair{ 1, 0 }, std::pair{ -1, 0 },
                                              std::pair{ 0, 1 }, std::pair{ 0, -1 } }) {
                    const std::uint8_t nb = mat_of(i + di, j + dj);
                    if (nb != self) {
                        other = nb;
                        break;
                    }
                }
                if (other == self) { continue; }
                for (int k = 0; k < 6; ++k) {
                    square4(next() < 0.5f ? self : other,
                            static_cast<float>(i * cell) + (next() * 24.0f) - 8.0f,
                            static_cast<float>(j * cell) + (next() * 24.0f) - 8.0f);
                }
            }
        }

        // Sparse accents: dirt specks on grass, gravel specks on snow.
        SDL_Texture* dirt = textures_.get("assets/map/tile_dirt.png");
        SDL_Texture* gravel = textures_.get("assets/map/tile_gravel.png");
        for (int k = 0; k < 26; ++k) {
            const float dx = next() * (shared::map::chunk_size - 5.0f);
            const float dy = next() * (shared::map::chunk_size - 5.0f);
            const std::uint8_t m = material_at(seed, wx0 + dx, wy0 + dy);
            SDL_Texture* speck = m == 0 ? dirt : (m >= 3 ? gravel : nullptr);
            if (speck == nullptr || next() < 0.55f) { continue; }
            float tw = 0.0f;
            float th = 0.0f;
            SDL_GetTextureSize(speck, &tw, &th);
            const SDL_FRect src{ .x = next() * (tw - 5.0f), .y = next() * (th - 5.0f),
                                 .w = 5.0f, .h = 5.0f };
            const SDL_FRect dst{ .x = dx, .y = dy, .w = 5.0f, .h = 5.0f };
            SDL_RenderTexture(r, speck, &src, &dst);
        }

        SDL_SetRenderTarget(r, nullptr);
        SDL_Texture* raw = tex;
        ground_cache_.emplace(key, client::TexturePtr{ tex });
        return raw;
    }

    void draw_ground(SDL_Renderer* r, float cam_x, float cam_y, float ww, float wh, float ox,
                     float oy)
    {
        const std::uint32_t seed = engine_->session().world_seed();
        const auto c0x = static_cast<std::int32_t>(
          std::floor((cam_x - (ww * 0.5f)) / shared::map::chunk_size));
        const auto c1x = static_cast<std::int32_t>(
          std::floor((cam_x + (ww * 0.5f)) / shared::map::chunk_size));
        const auto c0y = static_cast<std::int32_t>(
          std::floor((cam_y - (wh * 0.5f)) / shared::map::chunk_size));
        const auto c1y = static_cast<std::int32_t>(
          std::floor((cam_y + (wh * 0.5f)) / shared::map::chunk_size));
        for (std::int32_t cj = c0y; cj <= c1y; ++cj) {
            for (std::int32_t ci = c0x; ci <= c1x; ++ci) {
                SDL_Texture* tex = ground_chunk(r, ci, cj, seed);
                if (tex == nullptr) { continue; }
                const SDL_FRect dst{ .x = (static_cast<float>(ci) * shared::map::chunk_size) + ox,
                                     .y = (static_cast<float>(cj) * shared::map::chunk_size) + oy,
                                     .w = shared::map::chunk_size, .h = shared::map::chunk_size };
                SDL_RenderTexture(r, tex, nullptr, &dst);
            }
        }
    }

    // Map art path: "assets/map/<prefix>_<NN>.png", NN in 01..count.
    static std::string map_path(const char* prefix, std::uint64_t hash, int count)
    {
        const int idx = static_cast<int>(hash % static_cast<std::uint64_t>(count)) + 1;
        return std::string("assets/map/") + prefix + (idx < 10 ? "_0" : "_")
             + std::to_string(idx) + ".png";
    }

    static std::uint64_t spot_hash(float x, float y)
    {
        return shared::map::mix((static_cast<std::uint64_t>(static_cast<std::int64_t>(x)) << 21)
                                ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(y)));
    }

    // Flat ground clutter (plants/pebbles/bushes/stumps) from the seeded deco
    // field — drawn right over the background, under everything that walks.
    void draw_terrain_deco(SDL_Renderer* r, float cam_x, float cam_y, float ww, float wh,
                           float ox, float oy)
    {
        const std::uint32_t seed = engine_->session().world_seed();
        if (seed == 0) { return; }
        if (deco_seed_ != seed) {
            deco_cache_.clear();
            deco_seed_ = seed;
        }
        static constexpr const char* prefixes[4] = { "plant", "pebble", "bush", "stump" };
        const int counts[4] = { art_plants_, art_pebbles_, art_bushes_, art_stumps_ };
        const auto c0x = static_cast<std::int32_t>(
          std::floor((cam_x - (ww * 0.5f) - 64.0f) / shared::map::chunk_size));
        const auto c1x = static_cast<std::int32_t>(
          std::floor((cam_x + (ww * 0.5f) + 64.0f) / shared::map::chunk_size));
        const auto c0y = static_cast<std::int32_t>(
          std::floor((cam_y - (wh * 0.5f) - 64.0f) / shared::map::chunk_size));
        const auto c1y = static_cast<std::int32_t>(
          std::floor((cam_y + (wh * 0.5f) + 64.0f) / shared::map::chunk_size));
        for (std::int32_t cj = c0y; cj <= c1y; ++cj) {
            for (std::int32_t ci = c0x; ci <= c1x; ++ci) {
                auto [it, fresh] = deco_cache_.try_emplace(shared::map::chunk_key(ci, cj));
                if (fresh) { shared::map::deco_in(seed, ci, cj, it->second); }
                for (const shared::map::Deco& deco : it->second) {
                    const char* prefix = prefixes[deco.kind];
                    int count = counts[deco.kind];
                    // Snowfield stumps wear their frosted variant when we have one.
                    if (deco.kind == 3 && art_stump_snow_ > 0
                        && shared::map::biome_at(seed, deco.x, deco.y) == 2) {
                        prefix = "stump_snow";
                        count = art_stump_snow_;
                    }
                    if (count <= 0) { continue; }
                    SDL_Texture* tex =
                      textures_.get(map_path(prefix, spot_hash(deco.x, deco.y), count));
                    if (tex == nullptr) { continue; }
                    float tw = 0.0f;
                    float th = 0.0f;
                    SDL_GetTextureSize(tex, &tw, &th);
                    const SDL_FRect dst{ .x = deco.x + ox - (tw * 0.5f),
                                         .y = deco.y + oy - th, .w = tw, .h = th };
                    SDL_RenderTexture(r, tex, nullptr, &dst);
                }
            }
        }
    }

    // One obstacle of the y-sorted pass, bottom-anchored on its collider base.
    // Tall trees FADE when someone stands behind their canopy — the sorted
    // list is scanned for entities above (drawn earlier = hidden).
    void draw_obstacle(SDL_Renderer* r, const WorldItem& item)
    {
        const bool tree = item.ob.kind == 0;
        // Trees wear their BIOME's family: frozen crowns in the snow, greens
        // in the forest, gold/bare on the plains. Rocks are rocks everywhere.
        const char* prefix = "rock";
        int count = art_rocks_;
        if (tree) {
            const std::uint8_t biome = shared::map::biome_at(engine_->session().world_seed(),
                                                             item.ob.x, item.ob.y);
            if (biome == 2 && art_tree_snow_ > 0) {
                prefix = "tree_snow";
                count = art_tree_snow_;
            } else if (biome == 1 && art_tree_forest_ > 0) {
                prefix = "tree_forest";
                count = art_tree_forest_;
            } else {
                prefix = "tree_plain";
                count = art_tree_plain_ > 0 ? art_tree_plain_ : art_tree_forest_;
                if (art_tree_plain_ <= 0) { prefix = "tree_forest"; }
            }
        }
        SDL_Texture* tex =
          count > 0 ? textures_.get(map_path(prefix, spot_hash(item.ob.x, item.ob.y), count))
                    : nullptr;
        if (tex == nullptr) { // no art: a flat block so the collider still reads
            SDL_SetRenderDrawColor(r, tree ? 70 : 110, tree ? 110 : 110, tree ? 60 : 115, 255);
            const float s = item.ob.r * 2.0f;
            const SDL_FRect dst{ .x = item.x - (s * 0.5f), .y = item.y - (s * 0.5f), .w = s, .h = s };
            SDL_RenderFillRect(r, &dst);
            return;
        }
        float tw = 0.0f;
        float th = 0.0f;
        SDL_GetTextureSize(tex, &tw, &th);
        const SDL_FRect dst{ .x = item.x - (tw * 0.5f), .y = item.y + item.ob.r - th,
                             .w = tw, .h = th };
        std::uint8_t alpha = 255;
        if (tree && th > 60.0f) {
            for (const WorldItem& other : world_items_) {
                if (other.type == WorldItem::Obst || other.key >= item.key) { continue; }
                if (other.x > dst.x - 4.0f && other.x < dst.x + dst.w + 4.0f && other.y > dst.y
                    && other.y < dst.y + dst.h) {
                    alpha = 140; // someone is behind the canopy: show them through
                    break;
                }
            }
        }
        SDL_SetTextureAlphaMod(tex, alpha);
        SDL_RenderTexture(r, tex, nullptr, &dst);
        SDL_SetTextureAlphaMod(tex, 255); // the texture cache shares handles
    }

    static void draw_entity(SDL_Renderer* r, float cx, float cy, SDL_Texture* tex, SDL_Color fallback)
    {
        if (tex != nullptr) {
            client::draw_centered(r, tex, cx, cy, sprite_size, sprite_size);
            return;
        }
        SDL_SetRenderDrawColor(r, fallback.r, fallback.g, fallback.b, fallback.a);
        const SDL_FRect rect{ .x = cx - (entity_size * 0.5f), .y = cy - (entity_size * 0.5f),
                              .w = entity_size, .h = entity_size };
        SDL_RenderFillRect(r, &rect);
    }

    // Variants 4..8 are the bosses' signature bullets — each fight's threat
    // reads by silhouette/color alone. dx/dy = unit motion direction (from the
    // interpolation delta): 4/7 trail shrinking squares BEHIND the head, the
    // cheap way to say "flying that way" without rotated geometry.
    static void draw_projectile(SDL_Renderer* r, float cx, float cy, std::uint8_t variant,
                                float dx, float dy, float time)
    {
        const auto square = [&](float x, float y, float s) {
            const SDL_FRect rect{ .x = x - (s * 0.5f), .y = y - (s * 0.5f), .w = s, .h = s };
            SDL_RenderFillRect(r, &rect);
        };
        float size = 7.0f;
        if (variant == 1) {
            SDL_SetRenderDrawColor(r, 255, 90, 70, 255); // hostile (enemy-fired)
        } else if (variant == 2) {
            SDL_SetRenderDrawColor(r, 255, 170, 60, 255); // crit: bigger + orange
            size = 11.0f;
        } else if (variant == 3) {
            SDL_SetRenderDrawColor(r, 220, 50, 90, 255); // heavy hostile (Cyclop)
            size = 14.0f;
        } else if (variant == 4) { // Vampire Lord blood bolt: dark tear + tail
            SDL_SetRenderDrawColor(r, 140, 15, 45, 255);
            for (int i = 1; i <= 3; ++i) {
                square(cx - (dx * 7.0f * static_cast<float>(i)),
                       cy - (dy * 7.0f * static_cast<float>(i)),
                       7.0f - (1.5f * static_cast<float>(i)));
            }
            SDL_SetRenderDrawColor(r, 210, 30, 60, 255);
            size = 9.0f;
        } else if (variant == 5) { // Elder Ent sprinkler pellet / seed petal
            SDL_SetRenderDrawColor(r, 150, 210, 90, 255);
            size = 6.0f;
        } else if (variant == 6) { // Elder Ent seed: fat two-tone pulse
            const float pulse = 12.0f + (3.0f * std::sin(time * 9.0f));
            SDL_SetRenderDrawColor(r, 70, 120, 40, 255);
            square(cx, cy, pulse + 5.0f);
            SDL_SetRenderDrawColor(r, 160, 230, 90, 255);
            size = pulse;
        } else if (variant == 7) { // Game Master lance: pale shard + violet tail
            SDL_SetRenderDrawColor(r, 145, 115, 220, 255);
            for (int i = 1; i <= 3; ++i) {
                square(cx - (dx * 6.0f * static_cast<float>(i)),
                       cy - (dy * 6.0f * static_cast<float>(i)),
                       8.0f - (2.0f * static_cast<float>(i)));
            }
            SDL_SetRenderDrawColor(r, 225, 210, 255, 255);
            size = 8.0f;
        } else if (variant == 8) { // Frog King spit: bright-green ring pellet
            SDL_SetRenderDrawColor(r, 110, 225, 90, 255);
            size = 8.0f;
        } else if (variant == 9) { // electric zap (Static Charge / Reactive Plating)
            SDL_SetRenderDrawColor(r, 110, 220, 255, 255);
            square(cx, cy, 8.0f + (2.0f * std::sin(time * 40.0f))); // flicker halo
            SDL_SetRenderDrawColor(r, 235, 250, 255, 255);
            size = 4.0f;
        } else {
            SDL_SetRenderDrawColor(r, 250, 230, 120, 255);
        }
        square(cx, cy, size);
    }

    // Move while moving, Idle while still — whichever the pack actually has.
    static const client::AnimClip* pick_clip(const client::SpritePack& pack, bool moving)
    {
        // pack.move() falls back through Move -> Move_Full -> *Move* — packs
        // with only phase clips (FrogBoss) still get a walk cycle.
        const client::AnimClip* clip = moving ? pack.move() : pack.clip("Idle");
        if (clip == nullptr) { clip = moving ? pack.clip("Idle") : pack.move(); }
        return clip;
    }

    // Draw an enemy clip at the pack's shared pixel density, bottom-anchored:
    // a canvas taller than the pack's ref_h (FrogBoss jumps carry the leap's
    // air space) extends UPWARD from the same ground line instead of squashing
    // and displacing the body (the old center-scale "teleport").
    static void draw_pack_clip(SDL_Renderer* r, const client::SpritePack& pack,
                               const client::AnimClip& clip, float cx, float cy, float base_h,
                               float time, bool flip, SDL_Color tint, float fps = 12.0f,
                               bool once = false)
    {
        const float ref = pack.ref_h > 0.0f ? pack.ref_h : clip.frame_h;
        const float h = base_h * (clip.frame_h / ref);
        const float bottom = cy + (base_h * 0.5f);
        client::draw_clip(r, clip, cx, bottom - (h * 0.5f), h, time, flip, tint, fps, once);
    }

    // Desynchronize identical archetypes so a wave doesn't animate in lockstep.
    static float phase_offset(std::uint32_t net_id) { return static_cast<float>(net_id % 16U) * 0.37f; }

    // Begin a camera pan: capture where the camera IS so render_game can
    // smoothstep it to the (new) target — used when the boss arena toggles.
    void start_cam_blend()
    {
        cam_blend_from_x_ = last_cam_x_;
        cam_blend_from_y_ = last_cam_y_;
        cam_blend_until_ = anim_time_ + cam_blend_len;
    }

    // Archetypes are Lua-defined (mod:enemy): scale/tint/sprite come from the
    // render VM's enemy registry, keyed by the snapshot variant (wire id). A
    // sprite naming an animation-pack FOLDER animates (Idle/Move + facing);
    // a .png path stays a static texture; no sprite -> shared enemy.png.
    void draw_enemy(SDL_Renderer* r, float cx, float cy, SDL_Texture* shared_tex, const Remote& rem)
    {
        float scale = 1.0f;
        SDL_Color tint{ 255, 255, 255, 255 };
        SDL_Texture* tex = shared_tex;
        // A standing enemy (archer at standoff) faces the nearest player, with
        // an 8 px deadband so walking right past it flips the sprite once, not
        // rapidly. Movement facing (rem.face) wins whenever it moves.
        float face = rem.face;
        if (!rem.moving) {
            float best_d2 = std::numeric_limits<float>::max();
            float best_dx = 0.0f;
            for (const auto& [px, py] : player_screen_) {
                const float dx = px - cx;
                const float dy = py - cy;
                const float d2 = (dx * dx) + (dy * dy);
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_dx = dx;
                }
            }
            if (best_dx > 8.0f) {
                face = 1.0f;
            } else if (best_dx < -8.0f) {
                face = -1.0f;
            }
        }

        const mod::EnemyDef* def = engine_->mods().enemies().by_wire(rem.variant);
        if (def != nullptr) {
            scale = def->scale * rem.scale; // archetype size x dynamic kernel Scale
            tint = SDL_Color{ def->tint[0], def->tint[1], def->tint[2], 255 };
        }
        // Hit flash: a short red pulse when a snapshot showed its health drop.
        // (Color-mod is multiplicative, so red — not white — is the loud one.)
        if (anim_time_ < rem.flash_until) {
            tint = SDL_Color{ 255, 70, 70, 255 };
        }
        if (def != nullptr) {
            if (!def->sprite.empty()) {
                if (const client::SpritePack* pack = packs_.get(def->sprite)) {
                    // Attacking (Render.fx, Lua-driven): play the pack's attack
                    // clip ONCE from the transition — discovered from the
                    // assets, no per-archetype wiring. 24 fps: 22 frames fit
                    // the nova's 0.9 s window without freezing on the end.
                    if (rem.fx == 1 && rem.fx_start >= 0.0f) {
                        if (const client::AnimClip* atk = pack->attack()) {
                            draw_pack_clip(r, *pack, *atk, cx, cy, sprite_size * scale,
                                           anim_time_ - rem.fx_start, face < 0, tint, 24.0f,
                                           /*once=*/true);
                            return;
                        }
                    }
                    // Charging (fx=2): loop the rush cycle for as long as Lua
                    // holds the state. Telegraph (fx=3): play the wind-up once
                    // and freeze on the strike pose — the "dodge now" signal.
                    if (rem.fx == 2 && rem.fx_start >= 0.0f) {
                        if (const client::AnimClip* run = pack->charge()) {
                            draw_pack_clip(r, *pack, *run, cx, cy, sprite_size * scale,
                                           anim_time_ - rem.fx_start, face < 0, tint, 18.0f);
                            return;
                        }
                    }
                    if (rem.fx == 3 && rem.fx_start >= 0.0f) {
                        if (const client::AnimClip* pre = pack->prepare()) {
                            draw_pack_clip(r, *pack, *pre, cx, cy, sprite_size * scale,
                                           anim_time_ - rem.fx_start, face < 0, tint, 18.0f,
                                           /*once=*/true);
                            return;
                        }
                    }
                    if (const client::AnimClip* clip = pick_clip(*pack, rem.moving)) {
                        draw_pack_clip(r, *pack, *clip, cx, cy, sprite_size * scale,
                                       anim_time_ + phase_offset(rem.net_id), face < 0, tint);
                        return;
                    }
                }
                if (SDL_Texture* own = textures_.get(def->sprite)) { tex = own; }
            }
        }
        const float size = sprite_size * scale;
        if (tex != nullptr) {
            SDL_SetTextureColorMod(tex, tint.r, tint.g, tint.b);
            client::draw_centered(r, tex, cx, cy, size, size);
            SDL_SetTextureColorMod(tex, 255, 255, 255);
        } else {
            SDL_SetRenderDrawColor(r, tint.r, tint.g, tint.b, 255);
            const SDL_FRect rect{ .x = cx - (size * 0.5f), .y = cy - (size * 0.5f), .w = size, .h = size };
            SDL_RenderFillRect(r, &rect);
        }
    }

    // Players: the pack declared by mods via mod:player_sprite (animated),
    // falling back to the static player.png, then a colored square. `scale`
    // is the kernel Scale component off the wire — the RULES that change it
    // (e.g. Vitality growing you per heart) live in Lua, not here.
    //
    // Directional packs (clips named <State>_<Dir8>) pick by state priority
    // Death > Dash > MoveShoot > Shoot > Move > Idle; a pack with only plain
    // Idle/Move clips keeps the legacy right-facing + flip behavior.
    void draw_player(SDL_Renderer* r, float cx, float cy, const PlayerAnim& anim,
                     std::uint32_t net_id, float scale, SDL_Color fallback)
    {
        const float size = sprite_size * scale;
        const std::string& pack_path = engine_->mods().player_sprite();
        if (!pack_path.empty()) {
            if (const client::SpritePack* pack = packs_.get(pack_path)) {
                const std::string_view dir = client::dir8_name(anim.dir_x, anim.dir_y);
                const client::AnimClip* clip = nullptr;
                float time = anim_time_ + phase_offset(net_id);
                float fps = 12.0f;
                bool once = false;
                if (anim.death_start >= 0.0f) {
                    clip = pack->directional("Death", dir);
                    time = anim_time_ - anim.death_start;
                    fps = 10.0f; // 8 frames -> a 0.8 s collapse, then freeze
                    once = true;
                } else if (anim.dash_frac >= 0.0f) {
                    clip = pack->directional("Dash", dir);
                    if (clip != nullptr) {
                        // Map burst progress straight onto the strip: fps=frames
                        // makes time=frac select frame frac*frames.
                        time = anim.dash_frac;
                        fps = static_cast<float>(clip->frames);
                        once = true;
                    }
                }
                if (clip == nullptr && anim.death_start < 0.0f) {
                    clip = anim.firing ? pack->directional(anim.moving ? "MoveShoot" : "Shoot", dir)
                                       : pack->directional(anim.moving ? "Move" : "Idle", dir);
                }
                if (clip != nullptr) {
                    // Same-box drop shadow first, if the pack ships one.
                    if (const client::AnimClip* shadow = pack->clip("Shadow")) {
                        client::draw_clip(r, *shadow, cx, cy, size, 0.0f, false);
                    }
                    client::draw_clip(r, *clip, cx, cy, size, time, false,
                                      SDL_Color{ 255, 255, 255, 255 }, fps, once);
                    return;
                }
                if (const client::AnimClip* legacy = pick_clip(*pack, anim.moving)) {
                    client::draw_clip(r, *legacy, cx, cy, size,
                                      anim_time_ + phase_offset(net_id), anim.dir_x < 0.0f);
                    return;
                }
            }
        }
        if (SDL_Texture* tex = textures_.get(asset_player)) {
            client::draw_centered(r, tex, cx, cy, size, size);
            return;
        }
        draw_entity(r, cx, cy, nullptr, fallback);
    }

    void draw_heart_pickup(SDL_Renderer* r, float cx, float cy)
    {
        if (SDL_Texture* icon = textures_.get(asset_heart)) {
            client::draw_centered(r, icon, cx, cy, orb_size, orb_size);
            return;
        }
        constexpr float size = 10.0f;
        SDL_SetRenderDrawColor(r, 230, 60, 70, 255); // heal: red (fallback)
        const SDL_FRect rect{ .x = cx - (size * 0.5f), .y = cy - (size * 0.5f), .w = size, .h = size };
        SDL_RenderFillRect(r, &rect);
    }

    // Boss chest: a gold pixel box (no art asset yet) with a slow glow pulse —
    // it has to read as "walk over me" from across the arena.
    void draw_chest(SDL_Renderer* r, float cx, float cy)
    {
        const float pulse = 0.5f + (0.5f * std::sin(anim_time_ * 3.0f));
        const float w = 26.0f;
        const float h = 18.0f;
        // Glow halo (pulsing), then body, lid band and a dark keyhole.
        SDL_SetRenderDrawColor(r, 255, 205, 110, static_cast<Uint8>(40.0f + (50.0f * pulse)));
        const SDL_FRect halo{ .x = cx - (w * 0.5f) - 5.0f, .y = cy - (h * 0.5f) - 5.0f,
                              .w = w + 10.0f, .h = h + 10.0f };
        SDL_RenderFillRect(r, &halo);
        SDL_SetRenderDrawColor(r, 172, 108, 48, 255);
        const SDL_FRect body{ .x = cx - (w * 0.5f), .y = cy - (h * 0.5f), .w = w, .h = h };
        SDL_RenderFillRect(r, &body);
        SDL_SetRenderDrawColor(r, 255, 205, 110, 255);
        const SDL_FRect lid{ .x = body.x, .y = body.y, .w = w, .h = 5.0f };
        SDL_RenderFillRect(r, &lid);
        const SDL_FRect band{ .x = cx - 2.0f, .y = body.y, .w = 4.0f, .h = h };
        SDL_RenderFillRect(r, &band);
        SDL_SetRenderDrawColor(r, 60, 36, 16, 255);
        const SDL_FRect keyhole{ .x = cx - 1.5f, .y = cy - 1.0f, .w = 3.0f, .h = 5.0f };
        SDL_RenderFillRect(r, &keyhole);
    }

    void draw_xp_orb(SDL_Renderer* r, float cx, float cy)
    {
        if (SDL_Texture* coin = textures_.get(asset_coin)) {
            client::draw_centered(r, coin, cx, cy, orb_size, orb_size);
            return;
        }
        constexpr float size = 9.0f;
        SDL_SetRenderDrawColor(r, 90, 200, 255, 255); // XP: cyan (fallback)
        const SDL_FRect rect{ .x = cx - (size * 0.5f), .y = cy - (size * 0.5f), .w = size, .h = size };
        SDL_RenderFillRect(r, &rect);
    }

    // Run every plugin Object draw hook for a player (e.g. the Onion aura ring).
    // Hooks self-gate (the Onion skips a zero radius), so calling is cheap.
    [[nodiscard]] const std::vector<mod::NetComp>* script_state_for(std::uint32_t net_id) const
    {
        const auto it = script_state_.find(net_id);
        return it != script_state_.end() ? &it->second : nullptr;
    }

    void draw_object_hooks(float cx, float cy, const std::vector<mod::NetComp>* comps)
    {
        const client::DrawView view{ .x = cx, .y = cy, .scripts = &engine_->mods().scripts(), .comps = comps };
        client::run_object_draws(engine_->mods(), ctx_obj_, view);
    }

    // Screen-edge red glow (damage feedback): four gradient quads, outer edge
    // solid -> inner edge transparent. SDL has no gradient rects; vertex colors
    // through SDL_RenderGeometry do it in one call per side.
    static void draw_vignette(SDL_Renderer* r, float ww, float wh, std::uint8_t alpha)
    {
        const float bx = ww * 0.14f; // glow depth per side
        const float by = wh * 0.20f;
        const SDL_FColor outer{ .r = 0.72f, .g = 0.05f, .b = 0.05f,
                                .a = static_cast<float>(alpha) / 255.0f };
        const SDL_FColor inner{ .r = 0.72f, .g = 0.05f, .b = 0.05f, .a = 0.0f };
        const auto quad = [&](SDL_FPoint p0, SDL_FPoint p1, SDL_FPoint p2, SDL_FPoint p3) {
            // p0/p1 = the OUTER edge, p2/p3 = the faded inner edge.
            const SDL_Vertex verts[4] = { { .position = p0, .color = outer, .tex_coord = {} },
                                          { .position = p1, .color = outer, .tex_coord = {} },
                                          { .position = p2, .color = inner, .tex_coord = {} },
                                          { .position = p3, .color = inner, .tex_coord = {} } };
            constexpr int idx[6] = { 0, 1, 2, 0, 2, 3 };
            SDL_RenderGeometry(r, nullptr, verts, 4, idx, 6);
        };
        quad({ .x = 0, .y = 0 }, { .x = ww, .y = 0 }, { .x = ww, .y = by }, { .x = 0, .y = by });
        quad({ .x = 0, .y = wh }, { .x = ww, .y = wh }, { .x = ww, .y = wh - by }, { .x = 0, .y = wh - by });
        quad({ .x = 0, .y = 0 }, { .x = 0, .y = wh }, { .x = bx, .y = wh }, { .x = bx, .y = 0 });
        quad({ .x = ww, .y = 0 }, { .x = ww, .y = wh }, { .x = ww - bx, .y = wh }, { .x = ww - bx, .y = 0 });
    }

    // Corner radar built from the same state the renderer already mirrors:
    // dots in radar range draw in place, mission-critical marks (players,
    // boss, chest) CLAMP to the rim so their direction always reads.
    void draw_minimap(SDL_Renderer* r, float ww, float t)
    {
        constexpr float size = 180.0f;    // panel px
        constexpr float scale = 1.0f / 14.0f; // world px per map px (~1.2k px reach)
        const float mx = ww - size - 14.0f;
        const float my = 14.0f;
        const float cx = mx + (size * 0.5f);
        const float cy = my + (size * 0.5f);
        SDL_SetRenderDrawColor(r, 14, 12, 12, 190);
        const SDL_FRect panel{ .x = mx, .y = my, .w = size, .h = size };
        SDL_RenderFillRect(r, &panel);
        SDL_SetRenderDrawColor(r, 255, 205, 110, 120); // frame matches the boss bar
        SDL_RenderRect(r, &panel);

        const auto dot = [&](float map_x, float map_y, float s) {
            const SDL_FRect d{ .x = map_x - (s * 0.5f), .y = map_y - (s * 0.5f), .w = s, .h = s };
            SDL_RenderFillRect(r, &d);
        };
        constexpr float rim = 5.0f; // clamp inset for edge marks
        const auto clamp_map = [&](float& map_x, float& map_y) {
            map_x = std::clamp(map_x, mx + rim, mx + size - rim);
            map_y = std::clamp(map_y, my + rim, my + size - rim);
        };

        // Arena rect (relative to me), clipped by the panel bounds.
        if (arena_active_) {
            float ax0 = cx + ((arena_cx_ - arena_hw_ - render_x_) * scale);
            float ay0 = cy + ((arena_cy_ - arena_hh_ - render_y_) * scale);
            float ax1 = cx + ((arena_cx_ + arena_hw_ - render_x_) * scale);
            float ay1 = cy + ((arena_cy_ + arena_hh_ - render_y_) * scale);
            ax0 = std::clamp(ax0, mx, mx + size);
            ay0 = std::clamp(ay0, my, my + size);
            ax1 = std::clamp(ax1, mx, mx + size);
            ay1 = std::clamp(ay1, my, my + size);
            SDL_SetRenderDrawColor(r, 255, 205, 110, 90);
            const SDL_FRect arena{ .x = ax0, .y = ay0, .w = ax1 - ax0, .h = ay1 - ay0 };
            SDL_RenderRect(r, &arena);
        }

        int enemy_dots = 0;
        registry_.view<Position, PrevPosition, Remote>().each(
          [&](core::Entity, const Position& p, const PrevPosition& prev, const Remote& rem) {
              float map_x = cx + (((prev.x + ((p.x - prev.x) * t)) - render_x_) * scale);
              float map_y = cy + (((prev.y + ((p.y - prev.y) * t)) - render_y_) * scale);
              const bool inside = map_x > mx + 2.0f && map_x < mx + size - 2.0f
                               && map_y > my + 2.0f && map_y < my + size - 2.0f;
              if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Player)) {
                  clamp_map(map_x, map_y);
                  if (rem.health == 0) { SDL_SetRenderDrawColor(r, 240, 80, 80, 235); }
                  else { SDL_SetRenderDrawColor(r, 120, 220, 130, 235); }
                  dot(map_x, map_y, 4.0f);
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Chest)) {
                  clamp_map(map_x, map_y);
                  SDL_SetRenderDrawColor(r, 255, 205, 110, 235);
                  const SDL_FRect c{ .x = map_x - 3.0f, .y = map_y - 3.0f, .w = 6.0f, .h = 6.0f };
                  SDL_RenderRect(r, &c);
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Enemy)) {
                  // Boss-sized archetypes (the milestone kits all draw >= 2x)
                  // mark gold and clamp; trash only shows in radar range.
                  const mod::EnemyDef* def = engine_->mods().enemies().by_wire(rem.variant);
                  if (def != nullptr && def->scale >= 2.0f) {
                      clamp_map(map_x, map_y);
                      SDL_SetRenderDrawColor(r, 255, 205, 110, 235);
                      dot(map_x, map_y, 5.0f);
                  } else if (inside && enemy_dots < 220) {
                      ++enemy_dots;
                      SDL_SetRenderDrawColor(r, 200, 70, 60, 170);
                      dot(map_x, map_y, 2.0f);
                  }
              }
          });

        SDL_SetRenderDrawColor(r, 245, 245, 245, 255); // me: center, always
        dot(cx, cy, 4.0f);
    }

    // Top-center boss health bar while an arena fight is live: name above a
    // wide red bar (SDL rects like health_bar; the gui text draws after the
    // world, so both sit on top of it).
    void draw_boss_bar(SDL_Renderer* r, float ww)
    {
        const auto it = remotes_.find(arena_net_id_);
        if (it == remotes_.end()) { return; }
        const Remote& rem = registry_.get<Remote>(it->second);
        const float frac = static_cast<float>(rem.health) / 255.0f;
        client::Gui& ui = engine_->gui();
        const float bar_w = std::min(ww * 0.42f, 620.0f);
        const float bar_h = 6.0f * ui.scale();
        const float x = (ww - bar_w) * 0.5f;
        const float name_px = 7.0f * ui.scale();
        const float y = 16.0f + name_px + 8.0f;
        std::string shout = boss_banner_;
        std::transform(shout.begin(), shout.end(), shout.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        ui.text_centered(ww * 0.5f, 16.0f, shout, client::GuiColor{ 255, 205, 110, 255 }, name_px);
        SDL_SetRenderDrawColor(r, 20, 16, 16, 235);
        const SDL_FRect back{ .x = x - 3.0f, .y = y - 3.0f, .w = bar_w + 6.0f, .h = bar_h + 6.0f };
        SDL_RenderFillRect(r, &back);
        SDL_SetRenderDrawColor(r, 60, 30, 30, 255);
        const SDL_FRect track{ .x = x, .y = y, .w = bar_w, .h = bar_h };
        SDL_RenderFillRect(r, &track);
        // Phase-threshold flash: the fill lerps to white while the sting rings
        // (set when the health byte crossed 50% / 25% — the boss shifted gears).
        if (anim_time_ < bar_flash_until_) {
            const float f = std::clamp((bar_flash_until_ - anim_time_) / 0.6f, 0.0f, 1.0f);
            SDL_SetRenderDrawColor(r, static_cast<Uint8>(210.0f + (45.0f * f)),
                                   static_cast<Uint8>(55.0f + (195.0f * f)),
                                   static_cast<Uint8>(55.0f + (195.0f * f)), 255);
        } else {
            SDL_SetRenderDrawColor(r, 210, 55, 55, 255);
        }
        const SDL_FRect fill{ .x = x, .y = y, .w = bar_w * frac, .h = bar_h };
        SDL_RenderFillRect(r, &fill);
        SDL_SetRenderDrawColor(r, 255, 205, 110, 255);
        SDL_RenderRect(r, &back);
    }

    static void health_bar(SDL_Renderer* r, float cx, float cy, std::uint8_t health)
    {
        if (health >= 255) { return; }
        const float frac = static_cast<float>(health) / 255.0f;
        const float bar_x = cx - (sprite_size * 0.5f);
        const float bar_y = cy - (sprite_size * 0.5f) - 6.0f;
        SDL_SetRenderDrawColor(r, 40, 40, 40, 255);
        const SDL_FRect back{ .x = bar_x, .y = bar_y, .w = sprite_size, .h = 4.0f };
        SDL_RenderFillRect(r, &back);
        SDL_SetRenderDrawColor(r, 90, 220, 90, 255);
        const SDL_FRect front{ .x = bar_x, .y = bar_y, .w = sprite_size * frac, .h = 4.0f };
        SDL_RenderFillRect(r, &front);
    }

    static void label(float cx, float cy, const std::string& text)
    {
        if (text.empty()) { return; }
        const float tx = cx - (sprite_size * 0.4f);
        // Just above the head; the ~22px gap that used to hold the health bar is
        // gone now that hearts live in the HUD, so the tag sits snug.
        const float ty = cy - (sprite_size * 0.5f) - 8.0f;
        // Background list: over the world, under any window (e.g. the level-up
        // menu) — a name tag must not float above a scene stacked on top.
        ImGui::GetBackgroundDrawList()->AddText(ImVec2(tx, ty), IM_COL32(235, 235, 235, 255), text.c_str());
    }

    static constexpr float entity_size = 12.0f;
    static constexpr float sprite_size = 48.0f;
    static constexpr float orb_size = 20.0f;
    static constexpr float bg_tile = 256.0f;
    static constexpr const char* asset_player = "assets/sprite/player.png";
    static constexpr const char* asset_enemy = "assets/sprite/enemy.png";
    static constexpr const char* asset_background = "assets/background.png";
    static constexpr const char* asset_coin = "assets/icons/coin.png";
    static constexpr const char* asset_heart = "assets/icons/hearth.png";

    // Held-input state, maintained from key/mouse events in handle_event.
    struct InputState
    {
        bool up = false, down = false, left = false, right = false, firing = false;
        bool dash_queued = false; // edge: set on SHIFT keydown, consumed per tick
    };
    void clear_input() { input_ = {}; }

    // Mirror one of the local player's kernel components into the render registry
    // (create on first sight, then overwrite), so the Lua HUD can read it through
    // the same BindingTable the sim uses. Only called while has_player_.
    template <typename T>
    void set_local(const T& value)
    {
        if (T* p = registry_.try_get<T>(player_)) { *p = value; }
        else { registry_.assign(player_, T{ value }); }
    }

    client::Textures textures_;
    client::SpritePacks packs_{ &textures_ }; // animation packs (Idle/Move strips)
    std::vector<std::pair<float, float>> player_screen_; // per-frame player positions (idle facing)
    // Terrain (deterministic from session.world_seed()): collision cache for
    // the prediction, deco cache + art variant counts for the renderer.
    shared::map::ChunkCache terrain_cache_;
    std::unordered_map<std::uint64_t, std::vector<shared::map::Deco>> deco_cache_;
    std::uint32_t deco_seed_ = 0;
    // Art variant counts per family (scanned once from assets/map).
    int art_tree_forest_ = 0, art_tree_plain_ = 0, art_tree_snow_ = 0;
    int art_rocks_ = 0, art_bushes_ = 0;
    int art_plants_ = 0, art_pebbles_ = 0, art_stumps_ = 0, art_stump_snow_ = 0;
    // Composited biome ground chunks (render targets), keyed like terrain chunks.
    std::unordered_map<std::uint64_t, client::TexturePtr> ground_cache_;
    std::uint32_t ground_seed_ = 0;
    std::vector<WorldItem> world_items_; // per-frame y-sorted world pass
    struct Shot
    {
        float x, y, dx, dy;
        std::uint8_t variant;
    };
    std::vector<Shot> shots_; // projectiles draw after the sorted pass (airborne)
    client::DrawContext draw_ctx_;  // reused surface for plugin draw hooks
    sol::object ctx_obj_;           // persistent Lua handle to draw_ctx_
    client::HudContext hud_ctx_;    // reused surface for plugin HUD hooks
    sol::object hud_ctx_obj_;       // persistent Lua handle to hud_ctx_
    InputState input_;
    core::Registry registry_;
    std::unordered_map<std::uint32_t, core::Entity> remotes_; // net id -> local entity
    core::Entity player_{};
    std::uint32_t my_net_id_ = 0;
    bool has_player_ = false;
    bool level_open_ = false;
    bool game_over_open_ = false;
    std::uint8_t my_health_ = 255;     // current hearts (snapshot health byte)
    std::uint8_t my_max_hearts_ = 3;   // max hearts (snapshot variant byte)
    std::uint16_t my_move_speed_ = 0;
    float my_scale_ = 1.0f; // kernel Scale off the wire (Lua-driven, e.g. Vitality)
    // Local render smoothing: the camera + local sprite track this eased position
    // instead of the raw predicted Position. A snapshot correction hard-snaps the
    // authoritative Position (needed for the sim), and since the camera is glued
    // to the player that snap would jerk the whole world; easing the RENDER pos
    // pans it smoothly instead. Seeded to the first server position (render_init_).
    float render_x_ = 0.0f, render_y_ = 0.0f;
    bool render_init_ = false;
    // Local dash prediction (struct defaults = base constants; server is authoritative).
    Dash local_dash_{};
    // Applied-snapshot ring: the delta baselines we can decode against (the
    // server only deltas vs ticks we acked, i.e. states stored here).
    static constexpr std::size_t snap_history_len = 32;
    std::deque<proto::SnapshotState> snap_history_;
    std::vector<std::uint8_t> script_field_counts_; // blob shapes for the codec
    bool field_counts_ready_ = false;
    // Per-entity networked script components (net id -> components), for draw hooks.
    std::unordered_map<std::uint32_t, std::vector<mod::NetComp>> script_state_;
    std::uint16_t level_ = 1;
    std::uint8_t xp_frac_ = 0;
    std::uint16_t wave_ = 1;
    float banner_until_ = -1.0f; // anim_time_ deadline for the "WAVE N" banner
    // Game-feel state (client-only, inferred from snapshot deltas).
    float shake_amp_ = 0.0f;     // camera shake amplitude px (decays in update)
    float vignette_until_ = -1.0f;   // red edge flash deadline (heart lost)
    float heartbeat_next_ = 0.0f;    // last-heart thump timer (update)
    float bar_flash_until_ = -1.0f;  // boss bar white flash (phase threshold)
    float boss_flash_until_ = -1.0f; // full-screen pop on a boss kill
    std::vector<Poof> poofs_;    // death/pickup bursts (cap 48)
    std::vector<FloatNum> float_nums_; // floating combat numbers (cap 96)
    std::vector<Poof> trail_;    // dash ghost samples (radius unused)
    // Boss arena (fixed rect): center = the arena enemy's spawn position (its
    // first sighting), half extents from its def. Cleared when the boss dies.
    bool arena_active_ = false;
    std::uint32_t arena_net_id_ = 0;
    float arena_cx_ = 0.0f, arena_cy_ = 0.0f, arena_hw_ = 0.0f, arena_hh_ = 0.0f;
    std::string boss_banner_;          // def label shown at the entrance
    float boss_banner_until_ = -1.0f;
    // Camera pan when a mod locks/releases the camera (ctx:camera_lock):
    // smoothstep from the captured start instead of cutting.
    static constexpr float cam_blend_len = 0.9f;
    float cam_blend_from_x_ = 0.0f, cam_blend_from_y_ = 0.0f;
    float cam_blend_until_ = -1.0f;
    float last_cam_x_ = 0.0f, last_cam_y_ = 0.0f;
    bool cam_was_locked_ = false; // edge detector for the pan
    float time_since_snapshot_ = 0.0f;
    float anim_time_ = 0.0f;   // drives every animation clip
    float my_dir_x_ = 1.0f; // local 8-way facing (aim while armed, legs while running)
    float my_dir_y_ = 0.0f;
    bool my_moving_ = false;
    // Authoritative aim + trigger for the local player, off the snapshot trailer
    // (not the mouse) — so a sim-side aim override (autofire) drives the sprite.
    float auth_aim_x_ = 1.0f;
    float auth_aim_y_ = 0.0f;
    bool auth_firing_ = false;
    float my_death_start_ = -1.0f; // anim clock when downed (-1 = alive)
};
