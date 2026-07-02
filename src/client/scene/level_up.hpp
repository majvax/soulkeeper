#pragma once
#include "client/engine.hpp"
#include "client/renderer.hpp"
#include "client/scene.hpp"
#include "shared/mod/registry.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
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
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_1) { pick(0); }
            if (event.key.key == SDLK_2) { pick(1); }
            if (event.key.key == SDLK_3) { pick(2); }
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

        ImDrawList* draw = ImGui::GetForegroundDrawList();
        draw->AddText(ImVec2(w * 0.5f - 90.0f, h * 0.5f - 200.0f), IM_COL32(255, 255, 255, 255),
                      "LEVEL UP - choose an upgrade");

        const mod::ContentRegistry& registry = engine_->mods().registry();
        const auto& choices = engine_->session().choices();
        for (std::size_t i = 0; i < choices.size(); ++i) {
            const SDL_FRect rect = card_rect(i, w, h);
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

            const ImU32 rc = IM_COL32(col.r, col.g, col.b, 255);
            const std::string num = std::to_string(i + 1);
            draw->AddText(ImVec2(rect.x + 10.0f, rect.y + 8.0f), IM_COL32(200, 200, 200, 255), num.c_str());
            const char* label = (def != nullptr) ? def->label.c_str() : "?";
            draw->AddText(ImVec2(rect.x + 16.0f, rect.y + rect.h * 0.42f), IM_COL32(255, 255, 255, 255), label);
            const std::string value = (def != nullptr) ? def->value_text[static_cast<std::size_t>(rarity)]
                                                        : std::string{};
            draw->AddText(ImVec2(rect.x + 16.0f, rect.y + rect.h * 0.42f + 22.0f), rc, value.c_str());
            draw->AddText(ImVec2(rect.x + 16.0f, rect.y + rect.h - 24.0f), rc, rarity_name(rarity));
        }
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
        engine_->session().send_select(index);
        engine_->scenes().pop(); // close ourselves
    }

    static SDL_FRect card_rect(std::size_t index, float w, float h)
    {
        const float total = (3.0f * card_w) + (2.0f * card_gap);
        const float x = ((w - total) * 0.5f) + (static_cast<float>(index) * (card_w + card_gap));
        return { .x = x, .y = (h - card_h) * 0.5f, .w = card_w, .h = card_h };
    }

    [[nodiscard]] int card_at(float mx, float my) const
    {
        const float w = static_cast<float>(engine_->width());
        const float h = static_cast<float>(engine_->height());
        for (std::size_t i = 0; i < 3; ++i) {
            const SDL_FRect rc = card_rect(i, w, h);
            if (mx >= rc.x && mx <= rc.x + rc.w && my >= rc.y && my <= rc.y + rc.h) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    static SDL_Color rarity_color(mod::Rarity rarity)
    {
        switch (rarity) {
        case mod::Rarity::Common:    return { .r = 150, .g = 150, .b = 150, .a = 255 };
        case mod::Rarity::Uncommon:  return { .r = 90, .g = 200, .b = 90, .a = 255 };
        case mod::Rarity::Legendary: return { .r = 230, .g = 190, .b = 60, .a = 255 };
        }
        return { .r = 150, .g = 150, .b = 150, .a = 255 };
    }

    static const char* rarity_name(mod::Rarity rarity)
    {
        switch (rarity) {
        case mod::Rarity::Common:    return "COMMON";
        case mod::Rarity::Uncommon:  return "UNCOMMON";
        case mod::Rarity::Legendary: return "LEGENDARY";
        }
        return "";
    }

    client::Textures textures_;
};
