#pragma once
#include "client/engine.hpp"
#include "client/mod/render_bindings.hpp"
#include "client/renderer.hpp"
#include "core/ecs.hpp"
#include "client/scene.hpp"
#include "client/scene/console.hpp"
#include "client/scene/level_up.hpp"
#include "client/sprites.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"
#include "shared/protocol.hpp"
#include "shared/system/input.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <iterator>
#include <limits>
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
    float death_start = -1.0f; // anim clock when a player went down (-1 = alive)
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
        // One persistent Lua object referencing our DrawContext, reused every
        // frame for all plugin draw hooks (no per-call allocation).
        ctx_obj_ = sol::make_object(engine->mods().lua(), std::ref(draw_ctx_));
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
        if (auto snap = session.take_snapshot()) {
            proto::ByteReader reader(*snap);
            apply_snapshot(reader);
        }
        if (!has_player_ && session.has_id()) { spawn_local_player(session.my_net_id()); }

        // Open the level-up card scene on the rising edge of a level-up.
        if (session.leveling() && !level_open_) {
            clear_input(); // hand focus to the card scene
            engine_->scenes().push<LevelUpScene>(engine_);
            level_open_ = true;
        } else if (!session.leveling()) {
            level_open_ = false;
        }

        send_and_predict(dt);
        anim_time_ += dt;           // shared clock for all animation clips
        time_since_snapshot_ += dt; // drives remote interpolation (alpha toward the newest snapshot)
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

        // Aim = direction from screen center (our player) to the cursor. Aiming
        // isn't a blocked action (only firing is), so a position query is fine.
        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
        SDL_GetMouseState(&mouse_x, &mouse_y);
        float aim_x = mouse_x - (static_cast<float>(engine_->width()) * 0.5f);
        float aim_y = mouse_y - (static_cast<float>(engine_->height()) * 0.5f);
        const float len = std::sqrt((aim_x * aim_x) + (aim_y * aim_y));
        if (len > 0.001f) {
            aim_x /= len;
            aim_y /= len;
        } else {
            aim_x = 1.0f;
            aim_y = 0.0f;
        }
        const std::uint8_t firing = (input_.firing && !downed) ? 1 : 0;

        // Dash: forward the edge to the server and mirror it locally so the
        // predicted position bursts in the same tick. The local Dash uses base
        // constants — upgraded cooldown/charges live server-side; position is
        // corrected by snapshots either way.
        std::uint8_t dash_flag = 0;
        if (input_.dash_queued && !downed) {
            dash_flag = 1;
            const bool moving = mx != 0 || my != 0;
            start_dash(local_dash_, moving ? static_cast<float>(mx) : aim_x,
                       moving ? static_cast<float>(my) : aim_y);
        }
        input_.dash_queued = false;

        engine_->session().send_input(proto::Input{ .move_x = mx, .move_y = my, .aim_x = aim_x,
                                                    .aim_y = aim_y, .firing = firing, .dash = dash_flag });

        my_firing_ = firing != 0;
        my_moving_ = false;

        if (has_player_ && !downed) {
            const float speed = my_move_speed_ > 0 ? static_cast<float>(my_move_speed_) : PLAYER_SPEED;
            Velocity& vel = registry_.get<Velocity>(player_);
            apply_input(vel, mx, my, speed);
            tick_dash(local_dash_, vel, speed, dt);
            Position& pos = registry_.get<Position>(player_);
            pos.x += vel.dx * dt;
            pos.y += vel.dy * dt;
            my_moving_ = vel.dx != 0.0f || vel.dy != 0.0f;
        }

        // 8-way facing (twin-stick): the gun follows the cursor while firing or
        // standing; legs win while running un-armed; a dash locks its direction.
        if (local_dash_.burst_remaining > 0.0f) {
            my_dir_x_ = local_dash_.dir_x;
            my_dir_y_ = local_dash_.dir_y;
        } else if (my_moving_ && !my_firing_) {
            const Velocity& vel = registry_.get<Velocity>(player_);
            my_dir_x_ = vel.dx;
            my_dir_y_ = vel.dy;
        } else {
            my_dir_x_ = aim_x;
            my_dir_y_ = aim_y;
        }

        // Death clip: remember when we went down, play once from there.
        if (downed && my_death_start_ < 0.0f) { my_death_start_ = anim_time_; }
        if (!downed) { my_death_start_ = -1.0f; }
    }

    void apply_snapshot(proto::ByteReader& reader)
    {
        const auto header = reader.get<proto::SnapshotHeader>();
        if (!header) { return; }
        level_ = header->level;
        xp_frac_ = header->xp_frac;
        wave_ = header->wave;

        std::unordered_set<std::uint32_t> seen;
        for (std::uint16_t i = 0; i < header->count; ++i) {
            const auto entry = reader.get<proto::SnapshotEntry>();
            if (!entry) { break; }
            // Positions travel quantized relative to the header origin.
            const float ex = proto::dequantize_pos(entry->qx, header->origin_x);
            const float ey = proto::dequantize_pos(entry->qy, header->origin_y);
            // Every entry is followed by its networked script components.
            std::vector<mod::NetComp> comps = mod::read_networked(reader, engine_->mods().scripts());

            if (has_player_ && entry->id == my_net_id_) {
                registry_.get<Position>(player_) = { .x = ex, .y = ey }; // snap correction
                my_health_ = entry->health;      // current hearts
                my_max_hearts_ = entry->variant; // max hearts
                my_move_speed_ = entry->move_speed;
                my_scale_ = proto::dequantize_scale(entry->scale_q);
                script_state_[entry->id] = std::move(comps);
                seen.insert(entry->id);
                continue;
            }

            seen.insert(entry->id);
            script_state_[entry->id] = std::move(comps);
            const auto it = remotes_.find(entry->id);
            if (it == remotes_.end()) {
                const core::Entity e = registry_.create();
                registry_.assign(e, Position{ .x = ex, .y = ey });
                registry_.assign(e, PrevPosition{ .x = ex, .y = ey });
                registry_.assign(e, Remote{ .kind = entry->kind, .net_id = entry->id, .health = entry->health,
                                            .variant = entry->variant });
                remotes_[entry->id] = e;
            } else {
                Position& pos = registry_.get<Position>(it->second);
                registry_.get<PrevPosition>(it->second) = { .x = pos.x, .y = pos.y };
                Remote& rem = registry_.get<Remote>(it->second);
                // Animation state from the snapshot delta: Move vs Idle + facing.
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
                rem.health = entry->health;
                rem.scale = proto::dequantize_scale(entry->scale_q);
                if (entry->kind == static_cast<std::uint8_t>(proto::EntityKind::Player)) {
                    // Downed players stay in the snapshot with 0 hearts.
                    if (entry->health == 0 && rem.death_start < 0.0f) { rem.death_start = anim_time_; }
                    if (entry->health != 0) { rem.death_start = -1.0f; }
                }
            }
        }

        for (auto it = remotes_.begin(); it != remotes_.end();) {
            if (!seen.contains(it->first)) {
                registry_.destroy(it->second);
                it = remotes_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = script_state_.begin(); it != script_state_.end();) {
            it = seen.contains(it->first) ? std::next(it) : script_state_.erase(it);
        }
        time_since_snapshot_ = 0.0f;
    }

    void render_game(SDL_Renderer* r)
    {
        float cam_x = 0.0f;
        float cam_y = 0.0f;
        if (has_player_) {
            const Position& me = registry_.get<Position>(player_);
            cam_x = me.x;
            cam_y = me.y;
        }
        const float ww = static_cast<float>(engine_->width());
        const float wh = static_cast<float>(engine_->height());
        const float ox = (ww * 0.5f) - cam_x;
        const float oy = (wh * 0.5f) - cam_y;
        draw_ctx_.ox = ox; // keep the plugin draw context's camera current
        draw_ctx_.oy = oy;

        draw_background(r, cam_x, cam_y, ww, wh, ox, oy);

        ImGui::Begin("Net", nullptr, ImGuiWindowFlags_NoBackground);
        ImGui::Text("you: %s (id %u)", engine_->session().name().c_str(), my_net_id_);
        ImGui::Text("%.1f fps (%.2f ms)", static_cast<double>(ImGui::GetIO().Framerate),
                    1000.0 / std::max(1.0, static_cast<double>(ImGui::GetIO().Framerate)));
        if (my_health_ == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "DOWNED - respawning...");
        } else {
            draw_hearts_hud(my_health_, my_max_hearts_);
        }
        if (local_dash_.charges > 0) {
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "DASH READY (SHIFT)");
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "DASH %.1fs",
                               static_cast<double>(std::max(0.0f, local_dash_.cooldown)));
        }
        ImGui::Text("Wave %u   Level %u", static_cast<unsigned>(wave_), static_cast<unsigned>(level_));
        ImGui::ProgressBar(static_cast<float>(xp_frac_) / 255.0f, ImVec2(160.0f, 0.0f), "XP");
        ImGui::End();

        SDL_Texture* enemy_tex = textures_.get(asset_enemy);

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
                  draw_object_hooks(x, y, script_state_for(rem.net_id));
                  draw_player(r, x, y,
                              PlayerAnim{ .dir_x = rem.dir_x, .dir_y = rem.dir_y,
                                          .moving = rem.moving, .death_start = rem.death_start },
                              rem.net_id, rem.scale, SDL_Color{ 220, 200, 80, 255 });
                  // Player bytes are hearts (current/max) -> bar fraction.
                  health_bar(r, x, y,
                             static_cast<std::uint8_t>(rem.health * 255
                                                       / std::max<int>(1, rem.variant)));
                  label(x, y, engine_->session().name_of(rem.net_id));
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
            draw_object_hooks(ox + p.x, oy + p.y, script_state_for(my_net_id_));
            const float dash_frac = local_dash_.burst_remaining > 0.0f
                                      ? 1.0f - (local_dash_.burst_remaining / DASH_DURATION)
                                      : -1.0f;
            draw_player(r, ox + p.x, oy + p.y,
                        PlayerAnim{ .dir_x = my_dir_x_, .dir_y = my_dir_y_, .moving = my_moving_,
                                    .firing = my_firing_, .dash_frac = dash_frac,
                                    .death_start = my_death_start_ },
                        my_net_id_, my_scale_, SDL_Color{ 80, 220, 100, 255 });
            health_bar(r, ox + p.x, oy + p.y, my_health_);
            label(ox + p.x, oy + p.y, engine_->session().name());
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
        const client::AnimClip* clip = pack.clip(moving ? "Move" : "Idle");
        if (clip == nullptr) { clip = pack.clip(moving ? "Idle" : "Move"); }
        return clip;
    }

    // Desynchronize identical archetypes so a wave doesn't animate in lockstep.
    static float phase_offset(std::uint32_t net_id) { return static_cast<float>(net_id % 16U) * 0.37f; }

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

        if (const mod::EnemyDef* def = engine_->mods().enemies().by_wire(rem.variant)) {
            scale = def->scale * rem.scale; // archetype size x dynamic kernel Scale
            tint = SDL_Color{ def->tint[0], def->tint[1], def->tint[2], 255 };
            if (!def->sprite.empty()) {
                if (const client::SpritePack* pack = packs_.get(def->sprite)) {
                    if (const client::AnimClip* clip = pick_clip(*pack, rem.moving)) {
                        client::draw_clip(r, *clip, cx, cy, sprite_size * scale,
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

    // HUD hearts row: hearth icons (tinted dark when empty), red squares fallback.
    void draw_hearts_hud(std::uint8_t current, std::uint8_t max)
    {
        SDL_Texture* icon = textures_.get(asset_heart);
        constexpr float size = 22.0f;
        for (std::uint8_t i = 0; i < max; ++i) {
            if (i > 0) { ImGui::SameLine(0.0f, 4.0f); }
            const bool filled = i < current;
            if (icon != nullptr) {
                const ImVec4 tint = filled ? ImVec4(1, 1, 1, 1) : ImVec4(0.25f, 0.25f, 0.25f, 0.9f);
                ImGui::Image(reinterpret_cast<ImTextureID>(icon), ImVec2(size, size),
                             ImVec2(0, 0), ImVec2(1, 1), tint, ImVec4(0, 0, 0, 0));
            } else {
                const ImU32 col = filled ? IM_COL32(230, 60, 70, 255) : IM_COL32(70, 70, 70, 220);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), col, 4.0f);
                ImGui::Dummy(ImVec2(size, size));
            }
        }
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
        const float ty = cy - (sprite_size * 0.5f) - 22.0f;
        ImGui::GetForegroundDrawList()->AddText(ImVec2(tx, ty), IM_COL32(235, 235, 235, 255), text.c_str());
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

    client::Textures textures_;
    client::SpritePacks packs_{ &textures_ }; // animation packs (Idle/Move strips)
    std::vector<std::pair<float, float>> player_screen_; // per-frame player positions (idle facing)
    client::DrawContext draw_ctx_;  // reused surface for plugin draw hooks
    sol::object ctx_obj_;           // persistent Lua handle to draw_ctx_
    InputState input_;
    core::Registry registry_;
    std::unordered_map<std::uint32_t, core::Entity> remotes_; // net id -> local entity
    core::Entity player_{};
    std::uint32_t my_net_id_ = 0;
    bool has_player_ = false;
    bool level_open_ = false;
    std::uint8_t my_health_ = 255;     // current hearts (snapshot health byte)
    std::uint8_t my_max_hearts_ = 3;   // max hearts (snapshot variant byte)
    std::uint16_t my_move_speed_ = 0;
    float my_scale_ = 1.0f; // kernel Scale off the wire (Lua-driven, e.g. Vitality)
    // Local dash prediction (base constants; server is authoritative).
    Dash local_dash_{ .cooldown_max = DASH_COOLDOWN, .cooldown = 0.0f, .burst_remaining = 0.0f,
                      .dir_x = 1.0f, .dir_y = 0.0f, .shockwave = 0.0f, .charges = 1, .max_charges = 1 };
    // Per-entity networked script components (net id -> components), for draw hooks.
    std::unordered_map<std::uint32_t, std::vector<mod::NetComp>> script_state_;
    std::uint16_t level_ = 1;
    std::uint8_t xp_frac_ = 0;
    std::uint16_t wave_ = 1;
    float time_since_snapshot_ = 0.0f;
    float anim_time_ = 0.0f;   // drives every animation clip
    float my_dir_x_ = 1.0f; // local 8-way facing (aim while armed, legs while running)
    float my_dir_y_ = 0.0f;
    bool my_moving_ = false;
    bool my_firing_ = false;
    float my_death_start_ = -1.0f; // anim clock when downed (-1 = alive)
};
