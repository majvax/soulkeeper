#pragma once
#include "client/engine.hpp"
#include "client/mod/render_bindings.hpp"
#include "client/renderer.hpp"
#include "core/ecs.hpp"
#include "client/scene.hpp"
#include "client/scene/console.hpp"
#include "client/scene/level_up.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"
#include "shared/protocol.hpp"
#include "shared/system/input.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <iterator>
#include <string>
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

        engine_->session().send_input(
          proto::Input{ .move_x = mx, .move_y = my, .aim_x = aim_x, .aim_y = aim_y, .firing = firing });

        if (has_player_ && !downed) {
            const float speed = my_move_speed_ > 0 ? static_cast<float>(my_move_speed_) : PLAYER_SPEED;
            Velocity& vel = registry_.get<Velocity>(player_);
            apply_input(vel, mx, my, speed);
            Position& pos = registry_.get<Position>(player_);
            pos.x += vel.dx * dt;
            pos.y += vel.dy * dt;
        }
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
            // Every entry is followed by its networked script components.
            std::vector<mod::NetComp> comps = mod::read_networked(reader, engine_->mods().scripts());

            if (has_player_ && entry->id == my_net_id_) {
                registry_.get<Position>(player_) = { .x = entry->x, .y = entry->y }; // snap correction
                my_health_ = entry->health;
                my_move_speed_ = entry->move_speed;
                script_state_[entry->id] = std::move(comps);
                seen.insert(entry->id);
                continue;
            }

            seen.insert(entry->id);
            script_state_[entry->id] = std::move(comps);
            const auto it = remotes_.find(entry->id);
            if (it == remotes_.end()) {
                const core::Entity e = registry_.create();
                registry_.assign(e, Position{ .x = entry->x, .y = entry->y });
                registry_.assign(e, PrevPosition{ .x = entry->x, .y = entry->y });
                registry_.assign(e, Remote{ .kind = entry->kind, .net_id = entry->id, .health = entry->health,
                                            .variant = entry->variant });
                remotes_[entry->id] = e;
            } else {
                Position& pos = registry_.get<Position>(it->second);
                registry_.get<PrevPosition>(it->second) = { .x = pos.x, .y = pos.y };
                pos = { .x = entry->x, .y = entry->y };
                Remote& rem = registry_.get<Remote>(it->second);
                rem.health = entry->health;
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
        if (my_health_ == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "DOWNED - respawning...");
        } else {
            ImGui::Text("HP: %d%%", static_cast<int>(my_health_) * 100 / 255);
        }
        ImGui::Text("Wave %u   Level %u", static_cast<unsigned>(wave_), static_cast<unsigned>(level_));
        ImGui::ProgressBar(static_cast<float>(xp_frac_) / 255.0f, ImVec2(160.0f, 0.0f), "XP");
        ImGui::End();

        SDL_Texture* player_tex = textures_.get(asset_player);
        SDL_Texture* enemy_tex = textures_.get(asset_enemy);

        const float t = std::min(time_since_snapshot_ * static_cast<float>(proto::snapshot_hz), 1.0f);
        registry_.view<Position, PrevPosition, Remote>().each(
          [&](core::Entity, const Position& p, const PrevPosition& prev, const Remote& rem) {
              const float x = ox + prev.x + ((p.x - prev.x) * t);
              const float y = oy + prev.y + ((p.y - prev.y) * t);
              if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Enemy)) {
                  draw_enemy(r, x, y, enemy_tex, rem.variant);
                  health_bar(r, x, y, rem.health);
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Player)) {
                  draw_object_hooks(x, y, script_state_for(rem.net_id));
                  draw_entity(r, x, y, player_tex, SDL_Color{ 220, 200, 80, 255 });
                  health_bar(r, x, y, rem.health);
                  label(x, y, engine_->session().name_of(rem.net_id));
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::Projectile)) {
                  draw_projectile(r, x, y);
              } else if (rem.kind == static_cast<std::uint8_t>(proto::EntityKind::XpOrb)) {
                  draw_xp_orb(r, x, y);
              } else {
                  draw_entity(r, x, y, nullptr, SDL_Color{ 220, 80, 80, 255 });
                  health_bar(r, x, y, rem.health);
              }
          });

        if (has_player_) {
            const Position& p = registry_.get<Position>(player_);
            draw_object_hooks(ox + p.x, oy + p.y, script_state_for(my_net_id_));
            draw_entity(r, ox + p.x, oy + p.y, player_tex, SDL_Color{ 80, 220, 100, 255 });
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

    static void draw_projectile(SDL_Renderer* r, float cx, float cy)
    {
        constexpr float size = 7.0f;
        SDL_SetRenderDrawColor(r, 250, 230, 120, 255);
        const SDL_FRect rect{ .x = cx - (size * 0.5f), .y = cy - (size * 0.5f), .w = size, .h = size };
        SDL_RenderFillRect(r, &rect);
    }

    // Archetypes are Lua-defined (mod:add_enemy): scale/tint/sprite come from
    // the render VM's enemy registry, keyed by the snapshot variant (wire id).
    void draw_enemy(SDL_Renderer* r, float cx, float cy, SDL_Texture* shared_tex, std::uint8_t variant)
    {
        float scale = 1.0f;
        SDL_Color tint{ 255, 255, 255, 255 };
        SDL_Texture* tex = shared_tex;
        if (const mod::EnemyDef* def = engine_->mods().enemies().by_wire(variant)) {
            scale = def->scale;
            tint = SDL_Color{ def->tint[0], def->tint[1], def->tint[2], 255 };
            if (!def->sprite.empty()) {
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

    // Held-input state, maintained from key/mouse events in handle_event.
    struct InputState
    {
        bool up = false, down = false, left = false, right = false, firing = false;
    };
    void clear_input() { input_ = {}; }

    client::Textures textures_;
    client::DrawContext draw_ctx_;  // reused surface for plugin draw hooks
    sol::object ctx_obj_;           // persistent Lua handle to draw_ctx_
    InputState input_;
    core::Registry registry_;
    std::unordered_map<std::uint32_t, core::Entity> remotes_; // net id -> local entity
    core::Entity player_{};
    std::uint32_t my_net_id_ = 0;
    bool has_player_ = false;
    bool level_open_ = false;
    std::uint8_t my_health_ = 255;
    std::uint16_t my_move_speed_ = 0;
    // Per-entity networked script components (net id -> components), for draw hooks.
    std::unordered_map<std::uint32_t, std::vector<mod::NetComp>> script_state_;
    std::uint16_t level_ = 1;
    std::uint8_t xp_frac_ = 0;
    std::uint16_t wave_ = 1;
    float time_since_snapshot_ = 0.0f;
};
