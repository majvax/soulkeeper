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
        const std::size_t count = engine_->session().choices().size();
        if (event.type == SDL_EVENT_KEY_DOWN) {
            // Keys 1..N pick the Nth card (the GAME decides how many, <= 5).
            for (std::size_t i = 0; i < count && i < proto::max_level_up_choices; ++i) {
                if (event.key.key == SDLK_1 + static_cast<SDL_Keycode>(i)) {
                    pick(static_cast<std::uint8_t>(i));
                }
            }
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            const int card = card_at(event.button.x, event.button.y);
            if (card >= 0) { pick(static_cast<std::uint8_t>(card)); }
        }
        return Stop;
    }

    auto update(float) -> Propagation override { return Stop; }

    auto render(float) -> Propagation override
    {
        SDL_Renderer* r = engine_->renderer();
        const float w = static_cast<float>(engine_->width());
        const float h = static_cast<float>(engine_->height());

        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 175);
        const SDL_FRect full{ .x = 0, .y = 0, .w = w, .h = h };
        SDL_RenderFillRect(r, &full);

        client::Gui& ui = engine_->gui();
        const float us = ui.scale();
        ui.text_centered(w * 0.5f, (h * 0.5f) - (card_h * 0.5f) - (18.0f * us), "LEVEL UP",
                         client::colors::accent, 12.0f * us);

        const mod::ContentRegistry& registry = engine_->mods().registry();
        const auto& choices = engine_->session().choices();
        for (std::size_t i = 0; i < choices.size(); ++i) {
            const SDL_FRect rect = card_rect(i, choices.size(), w, h);
            const auto rarity = static_cast<mod::Rarity>(choices[i].rarity);
            const mod::ContentDef* def = registry.by_wire(choices[i].id);
            const SDL_Color col = rarity_color(rarity);

            // Card background: a darkened rarity tint (grey / green / yellow).
            SDL_SetRenderDrawColor(r, static_cast<Uint8>(col.r / 3), static_cast<Uint8>(col.g / 3),
                                   static_cast<Uint8>(col.b / 3), 245);
            SDL_RenderFillRect(r, &rect);

            // Icon: centered near the top, drawn at its natural size (clamped) —
            // never stretched to fill the card.
            if (def != nullptr && !def->sprite.empty()) {
                if (SDL_Texture* icon = textures_.get(def->sprite)) {
                    float iw = 0.0f;
                    float ih = 0.0f;
                    SDL_GetTextureSize(icon, &iw, &ih);
                    constexpr float max_icon = 96.0f;
                    const float scale = std::min(1.0f, max_icon / std::max({ iw, ih, 1.0f }));
                    iw *= scale;
                    ih *= scale;
                    const SDL_FRect dst{ .x = rect.x + ((rect.w - iw) * 0.5f),
                                         .y = rect.y + (rect.h * 0.30f) - (ih * 0.5f),
                                         .w = iw, .h = ih };
                    SDL_RenderTexture(r, icon, nullptr, &dst);
                }
            }

            // Rarity border.
            SDL_SetRenderDrawColor(r, col.r, col.g, col.b, 255);
            for (int b = 0; b < 3; ++b) {
                const SDL_FRect border{ .x = rect.x - static_cast<float>(b),
                                        .y = rect.y - static_cast<float>(b),
                                        .w = rect.w + static_cast<float>(2 * b),
                                        .h = rect.h + static_cast<float>(2 * b) };
                SDL_RenderRect(r, &border);
            }

            const client::GuiColor rcol{ col.r, col.g, col.b, 255 };
            const float pad = 6.0f * us;
            const float small = 6.0f * us; // sub-line size (native 8 reads too big on cards)
            ui.text(rect.x + pad, rect.y + pad, std::to_string(i + 1), client::colors::dim, small);
            const std::string label = (def != nullptr) ? def->label : "?";
            ui.text_centered(rect.x + (rect.w * 0.5f), rect.y + (rect.h * 0.52f), label,
                             client::colors::text, small);
            const std::string value = (def != nullptr)
                                        ? def->value_text[static_cast<std::size_t>(rarity)]
                                        : std::string{};
            ui.text_centered(rect.x + (rect.w * 0.5f), rect.y + (rect.h * 0.52f) + ui.line_h(),
                             value, rcol, small);
            ui.text_centered(rect.x + (rect.w * 0.5f), rect.y + rect.h - (12.0f * us),
                             rarity_name(rarity), rcol, small);
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
    static constexpr float card_w = 200.0f;
    static constexpr float card_h = 280.0f;
    static constexpr float card_gap = 30.0f;

    void pick(std::uint8_t index)
    {
        engine_->audio().play("select");
        engine_->session().send_select(index);
        engine_->scenes().pop(); // close ourselves
    }

    static SDL_FRect card_rect(std::size_t index, std::size_t count, float w, float h)
    {
        const auto n = static_cast<float>(std::max<std::size_t>(count, 1));
        const float total = (n * card_w) + ((n - 1.0f) * card_gap);
        const float x = ((w - total) * 0.5f) + (static_cast<float>(index) * (card_w + card_gap));
        return { .x = x, .y = (h - card_h) * 0.5f, .w = card_w, .h = card_h };
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
};
