#pragma once
#include "client/engine.hpp"
#include "client/mod/render_bindings.hpp"
#include "client/renderer.hpp"
#include "core/ecs.hpp"
#include "client/scene.hpp"
#include "client/scene/console.hpp"
#include "client/scene/game_over.hpp"
#include "client/scene/level_up.hpp"
#include "client/sprites.hpp"
#include "client/trainer.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"
#include "shared/protocol.hpp"
#include "shared/snapshot_codec.hpp"
#include "shared/system/input.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <imgui.h>
#include <iterator>
#include <limits>
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

class GameScene : public client::Scene
{
public:
    explicit GameScene(client::Engine* engine) : Scene(engine), textures_{ engine->renderer() }
    {
        draw_ctx_.renderer = engine->renderer();
        draw_ctx_.textures = &textures_;
        draw_ctx_.audio = &engine->audio();
        // One persistent Lua object referencing our DrawContext, reused every
        // frame for all plugin draw hooks (no per-call allocation).
        ctx_obj_ = sol::make_object(engine->mods().lua(), std::ref(draw_ctx_));
        hud_ctx_.textures = &textures_; // cached icons for plugin HUD hooks
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
        if (!has_player_ && session.has_id()) { spawn_local_player(session.my_net_id()); }

        // Open the level-up card scene on the rising edge of a level-up.
        if (session.leveling() && !level_open_) {
            clear_input(); // hand focus to the card scene
            engine_->audio().play("levelup");
            engine_->scenes().push<LevelUpScene>(engine_);
            level_open_ = true;
        } else if (!session.leveling()) {
            level_open_ = false;
        }

        // The run ended: overlay the game-over screen (it owns the transition
        // back to the lobby — our update is blocked while it's on top).
        if (session.game_over() && !game_over_open_) {
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
        anim_time_ += dt;           // shared clock for all animation clips
        time_since_snapshot_ += dt; // drives remote interpolation (alpha toward the newest snapshot)
        shake_amp_ *= std::exp(-9.0f * dt); // camera shake settles in ~0.3 s
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
    ~GameScene() override = default;

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
            const Position& me = registry_.get<Position>(player_);
            px_screen = me.x + draw_ctx_.ox;
            py_screen = me.y + draw_ctx_.oy;
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
                if (rec.health < my_health_) {
                    shake_amp_ = 7.0f; // ouch: kick the camera
                    audio.play("hurt");
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
                pos = { .x = ex, .y = ey };
                if (rec.health < rem.health) {
                    rem.flash_until = anim_time_ + 0.12f; // hit!
                    audio.play_at("hit", ex, ey, lis_x, lis_y);
                }
                rem.health = rec.health;
                rem.scale = proto::dequantize_scale(rec.scale_q);
                if (rec.fx != 0 && rem.fx == 0) { rem.fx_start = anim_time_; } // attack begins
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
                                     || rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Heart);
                    float arch_scale = 1.0f; // size the burst to what vanished (boss >> bandit)
                    if (const mod::EnemyDef* def = engine_->mods().enemies().by_wire(rem.variant)) {
                        arch_scale = def->scale;
                    }
                    poofs_.push_back({ .x = pos.x, .y = pos.y, .t0 = anim_time_,
                                       .radius = pickup ? 10.0f : 20.0f * rem.scale * arch_scale,
                                       .pickup = pickup });
                    if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::XpOrb)) {
                        audio.play_at("pickup", pos.x, pos.y, lis_x, lis_y);
                    } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Heart)) {
                        audio.play_at("heart", pos.x, pos.y, lis_x, lis_y);
                    } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Enemy)) {
                        audio.play_at("death", pos.x, pos.y, lis_x, lis_y);
                    }
                }
                if (arena_active_ && it->first == arena_net_id_) {
                    arena_active_ = false; // boss down: drop the wall
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
            const Position& me = registry_.get<Position>(player_);
            cam_x = me.x;
            cam_y = me.y;
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

        draw_background(r, cam_x, cam_y, ww, wh, ox, oy);

        // Wave banner: big centered "WAVE N", fading out over its last second.
        // Skipped when a modal (console/level-up/game-over) is stacked above —
        // foreground-list text would bleed over it.
        if (anim_time_ < banner_until_ && engine_->scenes().is_top(this)) {
            const float remain = banner_until_ - anim_time_;
            const float alpha = std::clamp(remain, 0.0f, 1.0f); // fade the last second
            constexpr float banner_px = 56.0f;
            char banner[32];
            (void)std::snprintf(banner, sizeof(banner), "WAVE %u", static_cast<unsigned>(wave_));
            ImFont* font = ImGui::GetFont();
            const ImVec2 size = font->CalcTextSizeA(banner_px, FLT_MAX, 0.0f, banner);
            ImGui::GetForegroundDrawList()->AddText(
              font, banner_px, ImVec2((ww - size.x) * 0.5f, wh * 0.22f),
              IM_COL32(255, 225, 140, static_cast<int>(alpha * 255.0f)), banner);
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
            constexpr float banner_px = 44.0f;
            ImFont* font = ImGui::GetFont();
            const ImVec2 size = font->CalcTextSizeA(banner_px, FLT_MAX, 0.0f, boss_banner_.c_str());
            ImGui::GetForegroundDrawList()->AddText(
              font, banner_px, ImVec2((ww - size.x) * 0.5f, wh * 0.32f),
              IM_COL32(255, 205, 110, static_cast<int>(alpha * 255.0f)), boss_banner_.c_str());
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

        // "Net" = debug/team panel only (fps/ms + wave/level/xp). Per-player
        // stats (hearts/dash/speed/...) are drawn by Lua mod:hud hooks below.
        ImGui::Begin("Net", nullptr, ImGuiWindowFlags_NoBackground);
        ImGui::Text("you: %s (id %u)", engine_->session().name().c_str(), my_net_id_);
        ImGui::Text("%.1f fps (%.2f ms)", static_cast<double>(ImGui::GetIO().Framerate),
                    1000.0 / std::max(1.0, static_cast<double>(ImGui::GetIO().Framerate)));
        ImGui::Text("Wave %u   Level %u", static_cast<unsigned>(wave_), static_cast<unsigned>(level_));
        ImGui::ProgressBar(static_cast<float>(xp_frac_) / 255.0f, ImVec2(160.0f, 0.0f), "XP");
        ImGui::End();

        // Plugin HUD hooks (mod:hud): the local player's own stats/upgrades, in
        // the mod's OWN panel (begin_panel). Outside the Net window's Begin/End so
        // the hook opens a top-level window. The view exposes our networked script
        // comps (Weapon/Crit) AND our kernel stats (Position/Speed/Hearts/Dash),
        // both resolved via view:get through the shared BindingTable.
        if (has_player_) {
            set_local(local_dash_); // predicted dash -> render registry for the HUD
            const client::DrawView view{ .scripts = &engine_->mods().scripts(),
                                         .comps = script_state_for(my_net_id_),
                                         .reg = &registry_,
                                         .entity = player_,
                                         .table = engine_->mods().state().bindings.get() };
            client::run_hud_hooks(engine_->mods(), hud_ctx_obj_, hud_ctx_, view);
        }

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
            const Position& me = registry_.get<Position>(player_);
            player_screen_.push_back({ ox + me.x, oy + me.y });
        }
        registry_.view<Position, PrevPosition, Remote>().each(
          [&](core::Entity, const Position& p, const PrevPosition& prev, const Remote& rem) {
              if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Player)) {
                  player_screen_.push_back({ ox + prev.x + ((p.x - prev.x) * t),
                                             oy + prev.y + ((p.y - prev.y) * t) });
              }
          });
        registry_.view<Position, PrevPosition, Remote>().each(
          [&](core::Entity, const Position& p, const PrevPosition& prev, const Remote& rem) {
              const float x = ox + prev.x + ((p.x - prev.x) * t);
              const float y = oy + prev.y + ((p.y - prev.y) * t);
              if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Enemy)) {
                  draw_enemy(r, x, y, enemy_tex, rem);
                  health_bar(r, x, y, rem.health);
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Player)) {
                  if (overlays) { draw_object_hooks(x, y, script_state_for(rem.net_id)); }
                  // Same rule as the local player: aim (authoritative) unless
                  // running with the trigger up, then face movement.
                  float fdx = rem.aim_x;
                  float fdy = rem.aim_y;
                  if (rem.moving && !rem.firing) {
                      fdx = rem.dir_x;
                      fdy = rem.dir_y;
                  }
                  draw_player(r, x, y,
                              PlayerAnim{ .dir_x = fdx, .dir_y = fdy, .moving = rem.moving,
                                          .firing = rem.firing, .death_start = rem.death_start },
                              rem.net_id, rem.scale, SDL_Color{ 220, 200, 80, 255 });
                  // No health bar over players — the HUD hearts show life now.
                  if (overlays) { label(x, y, engine_->session().name_of(rem.net_id)); }
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Projectile)) {
                  draw_projectile(r, x, y, rem.variant);
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::XpOrb)) {
                  draw_xp_orb(r, x, y);
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Heart)) {
                  draw_heart_pickup(r, x, y);
              } else {
                  draw_entity(r, x, y, nullptr, SDL_Color{ 220, 80, 80, 255 });
                  health_bar(r, x, y, rem.health);
              }
          });

        if (has_player_) {
            const Position& p = registry_.get<Position>(player_);
            if (overlays) { draw_object_hooks(ox + p.x, oy + p.y, script_state_for(my_net_id_)); }
            const float dash_frac = local_dash_.burst_remaining > 0.0f
                                      ? 1.0f - (local_dash_.burst_remaining / DASH_DURATION)
                                      : -1.0f;
            draw_player(r, ox + p.x, oy + p.y,
                        PlayerAnim{ .dir_x = my_dir_x_, .dir_y = my_dir_y_, .moving = my_moving_,
                                    .firing = auth_firing_, .dash_frac = dash_frac,
                                    .death_start = my_death_start_ },
                        my_net_id_, my_scale_, SDL_Color{ 80, 220, 100, 255 });
            if (overlays) { label(ox + p.x, oy + p.y, engine_->session().name()); }
        }

        // Teammates off-screen: an edge arrow pointing at each remote player,
        // with their name — red when they're down (the "go revive" pointer).
        if (overlays && has_player_) {
            registry_.view<Position, PrevPosition, Remote>().each(
              [&](core::Entity, const Position& p, const PrevPosition& prev, const Remote& rem) {
                  if (rem.kind != static_cast<std::uint8_t>(proto::EntityKind::Player)) { return; }
                  const float sx = ox + prev.x + ((p.x - prev.x) * t);
                  const float sy = oy + prev.y + ((p.y - prev.y) * t);
                  constexpr float margin = 34.0f;
                  if (sx >= margin && sx <= ww - margin && sy >= margin && sy <= wh - margin) {
                      return; // on screen: the sprite itself is the cue
                  }
                  const float ax = std::clamp(sx, margin, ww - margin);
                  const float ay = std::clamp(sy, margin, wh - margin);
                  // Unit direction from the arrow anchor toward the teammate.
                  float dx = sx - ax;
                  float dy = sy - ay;
                  const float len = std::sqrt((dx * dx) + (dy * dy));
                  if (len < 1.0f) { return; }
                  dx /= len;
                  dy /= len;
                  const bool down = rem.health == 0;
                  const ImU32 col = down ? IM_COL32(240, 80, 80, 230) : IM_COL32(120, 220, 130, 230);
                  ImDrawList* fg = ImGui::GetForegroundDrawList();
                  const ImVec2 tip(ax + (dx * 13.0f), ay + (dy * 13.0f));
                  const ImVec2 left(ax - (dy * 8.0f), ay + (dx * 8.0f));
                  const ImVec2 right(ax + (dy * 8.0f), ay - (dx * 8.0f));
                  fg->AddTriangleFilled(tip, left, right, col);
                  const std::string name = engine_->session().name_of(rem.net_id);
                  const ImVec2 sz = ImGui::CalcTextSize(name.c_str());
                  fg->AddText(ImVec2(std::clamp(ax - (sz.x * 0.5f), 4.0f, ww - sz.x - 4.0f),
                                     std::clamp(ay - (dy * 16.0f) - (sz.y * 0.5f), 4.0f, wh - sz.y - 4.0f)),
                              col, name.c_str());
              });
        }
    }

    void draw_background(SDL_Renderer* r, float cam_x, float cam_y, float ww, float wh, float ox, float oy)
    {
        SDL_Texture* bg = textures_.get(asset_background);
        if (bg == nullptr) { return; }
        const int i0 = static_cast<int>(std::floor((cam_x - (ww * 0.5f)) / bg_tile));
        const int i1 = static_cast<int>(std::ceil((cam_x + (ww * 0.5f)) / bg_tile));
        const int j0 = static_cast<int>(std::floor((cam_y - (wh * 0.5f)) / bg_tile));
        const int j1 = static_cast<int>(std::ceil((cam_y + (wh * 0.5f)) / bg_tile));
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                const SDL_FRect dst{ .x = (static_cast<float>(i) * bg_tile) + ox,
                                     .y = (static_cast<float>(j) * bg_tile) + oy,
                                     .w = bg_tile, .h = bg_tile };
                SDL_RenderTexture(r, bg, nullptr, &dst);
            }
        }
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

    static void draw_projectile(SDL_Renderer* r, float cx, float cy, std::uint8_t variant)
    {
        float size = 7.0f;
        if (variant == 1) {
            SDL_SetRenderDrawColor(r, 255, 90, 70, 255); // hostile (enemy-fired)
        } else if (variant == 2) {
            SDL_SetRenderDrawColor(r, 255, 170, 60, 255); // crit: bigger + orange
            size = 11.0f;
        } else {
            SDL_SetRenderDrawColor(r, 250, 230, 120, 255);
        }
        const SDL_FRect rect{ .x = cx - (size * 0.5f), .y = cy - (size * 0.5f), .w = size, .h = size };
        SDL_RenderFillRect(r, &rect);
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
    std::vector<Poof> poofs_;    // death/pickup bursts (cap 48)
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
