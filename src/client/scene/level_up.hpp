#pragma once
#include "client/engine.hpp"
#include "client/renderer.hpp"
#include "client/scene.hpp"
#include "shared/mod/registry.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

// Modal card scene shown while the team picks an upgrade. Returns Stop from
// update/handle_event so the frozen game (rendered below) can't be controlled.
// Pops itself once a choice is made. Cards are drawn natively (image-or-rect +
// text overlay), so they don't look like an ImGui debug window.
class LevelUpScene final : public client::Scene
{
public:
    explicit LevelUpScene(client::Engine* engine) : Scene(engine), textures_{ engine->renderer() } {}

    auto handle_event(const SDL_Event& event) -> Propagation override
    {
        if (picked_ >= 0) { return Stop; } // the pick flash owns the exit
        const std::size_t count = engine_->session().choices().size();
        if (event.type == SDL_EVENT_KEY_DOWN) {
            // Keys 1..N pick the Nth card (the GAME decides how many, <= 5).
            for (std::size_t i = 0; i < count && i < proto::max_level_up_choices; ++i) {
                if (event.key.key == SDLK_1 + static_cast<SDL_Keycode>(i)) {
                    pick(static_cast<std::uint8_t>(i));
                }
            }
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            const int card = card_at(event.motion.x, event.motion.y);
            if (card != hovered_ && card >= 0) { engine_->audio().play("click"); }
            hovered_ = card;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            const int card = card_at(event.button.x, event.button.y);
            if (card >= 0) { pick(static_cast<std::uint8_t>(card)); }
        }
        return Stop;
    }

    auto update(float dt) -> Propagation override
    {
        age_ += dt; // drives the deal-in and the pick flash
        // The pick lands after its flash beat (pops are deferred; guard the
        // extra update tick between our pop request and its apply).
        if (picked_ >= 0 && age_ >= pick_done_ && !sent_) {
            sent_ = true;
            engine_->session().send_select(static_cast<std::uint8_t>(picked_));
            engine_->scenes().pop(); // close ourselves
        }
        return Stop;
    }

    auto render(float) -> Propagation override
    {
        SDL_Renderer* r = engine_->renderer();
        const float w = static_cast<float>(engine_->width());
        const float h = static_cast<float>(engine_->height());

        client::Gui& ui = engine_->gui();
        ui.dim_overlay(); // scrim over the frozen world beneath us

        const float us = ui.scale();
        const auto& choices = engine_->session().choices();
        const std::size_t count = choices.size();
        const float ch = card_rect(0, count, w, h).h; // card height for the title offset
        const bool chest = engine_->session().offer_is_chest();
        ui.text_centered(w * 0.5f, (h * 0.5f) - (ch * 0.5f) - (14.0f * us),
                         chest ? "TREASURE" : "LEVEL UP",
                         chest ? client::GuiColor{ 255, 205, 110, 255 } : client::colors::accent,
                         12.0f * us);

        const mod::ContentRegistry& registry = engine_->mods().registry();

        // Uniform text sizes across ALL cards (they're identical in size): shrink
        // the base size to whatever fits the LONGEST label/value so every card
        // reads at the same size — otherwise fit_px shrinks only the long ones and
        // the row looks ragged.
        const SDL_FRect ref = card_rect(0, count, w, h);
        const float pad = ref.w * 0.08f;
        const float inner_w = ref.w - (2.0f * pad);
        float label_px = ref.h * 0.085f;
        float value_px = ref.h * 0.075f;
        const float sub_px = ref.h * 0.06f;
        for (std::size_t i = 0; i < count; ++i) {
            const mod::ContentDef* d = registry.by_wire(choices[i].id);
            if (d == nullptr) { continue; }
            label_px = std::min(label_px, ui.fit_px(d->label, inner_w, ref.h * 0.085f));
            const std::string& v = d->value_text[static_cast<std::size_t>(choices[i].rarity)];
            value_px = std::min(value_px, ui.fit_px(v, inner_w, ref.h * 0.075f));
        }

        for (std::size_t i = 0; i < count; ++i) {
            SDL_FRect rect = card_rect(i, count, w, h);
            const auto rarity = static_cast<mod::Rarity>(choices[i].rarity);
            const mod::ContentDef* def = registry.by_wire(choices[i].id);
            const SDL_Color col = rarity_color(rarity);

            // Deal-in: card i eases up from below after its stagger beat.
            const float deal = std::clamp((age_ - (static_cast<float>(i) * 0.07f)) / 0.18f,
                                          0.0f, 1.0f);
            const float ease = 1.0f - ((1.0f - deal) * (1.0f - deal) * (1.0f - deal));
            rect.y += (1.0f - ease) * 40.0f;
            float alpha_mul = ease;
            // Pick beat: the chosen card holds bright, the others fall away.
            if (picked_ >= 0 && static_cast<std::size_t>(picked_) != i) {
                alpha_mul *= std::clamp((pick_done_ - age_) / 0.22f, 0.0f, 1.0f) * 0.6f;
            }
            // Hover: grow around the center (hitbox stays the base rect — a
            // grown card can't steal its neighbor's click).
            if (picked_ < 0 && hovered_ == static_cast<int>(i)) {
                const float gw = rect.w * 0.06f;
                const float gh = rect.h * 0.06f;
                rect = SDL_FRect{ .x = rect.x - (gw * 0.5f), .y = rect.y - (gh * 0.5f),
                                  .w = rect.w + gw, .h = rect.h + gh };
            }
            const auto a8 = [&](float base) {
                return static_cast<Uint8>(std::clamp(base * alpha_mul, 0.0f, 255.0f));
            };

            // Card background: a darkened rarity tint (grey / green / yellow).
            SDL_SetRenderDrawColor(r, static_cast<Uint8>(col.r / 3), static_cast<Uint8>(col.g / 3),
                                   static_cast<Uint8>(col.b / 3), a8(245.0f));
            SDL_RenderFillRect(r, &rect);

            // Icon: centered in the upper third, its natural size clamped to a
            // fraction of the card — never stretched to fill.
            if (def != nullptr && !def->sprite.empty()) {
                if (SDL_Texture* icon = textures_.get(def->sprite)) {
                    float iw = 0.0f;
                    float ih = 0.0f;
                    SDL_GetTextureSize(icon, &iw, &ih);
                    const float max_icon = rect.h * 0.34f; // proportional to the card
                    const float scale = std::min(1.0f, max_icon / std::max({ iw, ih, 1.0f }));
                    iw *= scale;
                    ih *= scale;
                    const SDL_FRect dst{ .x = rect.x + ((rect.w - iw) * 0.5f),
                                         .y = rect.y + (rect.h * 0.30f) - (ih * 0.5f),
                                         .w = iw, .h = ih };
                    SDL_SetTextureAlphaMod(icon, a8(255.0f));
                    SDL_RenderTexture(r, icon, nullptr, &dst);
                    SDL_SetTextureAlphaMod(icon, 255); // the cache shares textures
                }
            }

            // Rarity border (thickness scales with the UI; hover brightens it).
            const bool hot = picked_ < 0 && hovered_ == static_cast<int>(i);
            SDL_SetRenderDrawColor(r, hot ? static_cast<Uint8>(std::min(255, col.r + 60)) : col.r,
                                   hot ? static_cast<Uint8>(std::min(255, col.g + 60)) : col.g,
                                   hot ? static_cast<Uint8>(std::min(255, col.b + 60)) : col.b,
                                   a8(255.0f));
            const int thick = std::max(2, static_cast<int>(us));
            for (int b = 0; b < thick; ++b) {
                const SDL_FRect border{ .x = rect.x - static_cast<float>(b),
                                        .y = rect.y - static_cast<float>(b),
                                        .w = rect.w + static_cast<float>(2 * b),
                                        .h = rect.h + static_cast<float>(2 * b) };
                SDL_RenderRect(r, &border);
            }

            // Uniform sizes (computed above) so every card matches.
            const client::GuiColor rcol{ col.r, col.g, col.b, a8(255.0f) };
            client::GuiColor dim = client::colors::dim;
            client::GuiColor body = client::colors::text;
            dim.a = a8(static_cast<float>(dim.a));
            body.a = a8(static_cast<float>(body.a));
            ui.text(rect.x + pad, rect.y + pad, std::to_string(i + 1), dim, sub_px);
            const std::string label = (def != nullptr) ? def->label : "?";
            ui.text_centered(rect.x + (rect.w * 0.5f), rect.y + (rect.h * 0.54f), label,
                             body, label_px);
            const std::string value = (def != nullptr)
                                        ? def->value_text[static_cast<std::size_t>(rarity)]
                                        : std::string{};
            ui.text_centered(rect.x + (rect.w * 0.5f), rect.y + (rect.h * 0.54f) + (label_px * 1.7f),
                             value, rcol, value_px);
            ui.text_centered(rect.x + (rect.w * 0.5f), rect.y + rect.h - (sub_px * 1.8f),
                             rarity_name(rarity), rcol, sub_px);

            // Pick flash: the chosen card whites out over its beat.
            if (picked_ >= 0 && static_cast<std::size_t>(picked_) == i) {
                const float f = std::clamp(1.0f - ((pick_done_ - age_) / 0.22f), 0.0f, 1.0f);
                SDL_SetRenderDrawColor(r, 255, 255, 240, static_cast<Uint8>(f * 210.0f));
                SDL_RenderFillRect(r, &rect);
            }
        }

        // Keep the stats panel visible + bright OVER our dim overlay — the
        // upgrade choice depends on it. GameScene (below us) published it.
        engine_->render_hud();
        return Stop;
    }

    LevelUpScene(const LevelUpScene&) = delete;
    LevelUpScene(LevelUpScene&&) = delete;
    LevelUpScene& operator=(const LevelUpScene&) = delete;
    LevelUpScene& operator=(LevelUpScene&&) = delete;
    ~LevelUpScene() override = default;

private:
    void pick(std::uint8_t index)
    {
        engine_->audio().play("select");
        picked_ = index; // the flash beat plays out, then update() sends + pops
        pick_done_ = age_ + 0.22f;
    }

    // Cards are sized as a fraction of the SCREEN, not a multiple of the UI
    // scale: they're the focal element and must stay big + consistent at every
    // resolution (a scale-multiple shrank them to ~half size on 720/900p, where
    // scale() floors at 2). Portrait cards ~half the screen height; the width is
    // capped so up to 5 cards + gaps always fit, then the height follows the
    // aspect so the row never overflows.
    static SDL_FRect card_rect(std::size_t index, std::size_t count, float w, float h)
    {
        const auto n = static_cast<float>(std::max<std::size_t>(count, 1));
        const float gap = w * 0.02f;
        float cw = std::min(h * 0.30f, ((w * 0.94f) - ((n - 1.0f) * gap)) / n);
        const float chh = std::min(h * 0.52f, cw / 0.68f); // portrait aspect, height-capped
        cw = chh * 0.68f;                                   // re-derive if height clamped
        const float total = (n * cw) + ((n - 1.0f) * gap);
        const float x = ((w - total) * 0.5f) + (static_cast<float>(index) * (cw + gap));
        return { .x = x, .y = (h - chh) * 0.5f, .w = cw, .h = chh };
    }

    [[nodiscard]] int card_at(float mx, float my) const
    {
        const float w = static_cast<float>(engine_->width());
        const float h = static_cast<float>(engine_->height());
        const std::size_t count = engine_->session().choices().size();
        for (std::size_t i = 0; i < count; ++i) {
            const SDL_FRect rc = card_rect(i, count, w, h);
            if (mx >= rc.x && mx <= rc.x + rc.w && my >= rc.y && my <= rc.y + rc.h) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    static SDL_Color rarity_color(mod::Rarity rarity)
    {
        switch (rarity) {
        case mod::Rarity::Common:    return { .r = 150, .g = 150, .b = 150, .a = 255 }; // grey
        case mod::Rarity::Uncommon:  return { .r = 90, .g = 200, .b = 90, .a = 255 };   // green
        case mod::Rarity::Rare:      return { .r = 90, .g = 140, .b = 255, .a = 255 };  // blue
        case mod::Rarity::Epic:      return { .r = 190, .g = 90, .b = 230, .a = 255 };  // purple
        case mod::Rarity::Legendary: return { .r = 230, .g = 190, .b = 60, .a = 255 };  // gold
        }
        return { .r = 150, .g = 150, .b = 150, .a = 255 };
    }

    static const char* rarity_name(mod::Rarity rarity)
    {
        switch (rarity) {
        case mod::Rarity::Common:    return "COMMON";
        case mod::Rarity::Uncommon:  return "UNCOMMON";
        case mod::Rarity::Rare:      return "RARE";
        case mod::Rarity::Epic:      return "EPIC";
        case mod::Rarity::Legendary: return "LEGENDARY";
        }
        return "";
    }

    client::Textures textures_;
    // Animation state: a scene-local clock drives the deal-in stagger, the
    // hover grow and the pick flash (the pick pops AFTER its beat).
    float age_ = 0.0f;
    int hovered_ = -1;
    int picked_ = -1;
    float pick_done_ = -1.0f;
    bool sent_ = false;
};
