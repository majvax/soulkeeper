// src/client/gui.hpp
//
// The game's own UI kit: an immediate-mode widget set drawn from the
// BlackAndWhite pixel-art pack (assets/ui/*.png, 9-sliced) with bitmap text
// baked from assets/font/PressStart2P.ttf via stb_truetype. This is what
// user-facing scenes (connect/lobby/level-up/game-over) render with — ImGui
// remains only for developer surfaces (console, mod HUD panels, debug).
//
// Model: the Engine owns one Gui and forwards every SDL event; scenes call
// widgets from render(). A widget is hot when the mouse is inside it and
// "clicked" on the release of a press that started inside it. One text input
// can hold focus; it owns SDL text-input state while it does. Missing assets
// degrade to flat rects / no text (logged once), matching Textures/Audio.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "client/renderer.hpp"

namespace client {

struct GuiColor
{
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
};

namespace colors {
inline constexpr GuiColor text{ 220, 220, 220, 255 };
inline constexpr GuiColor dim{ 140, 140, 140, 255 };
inline constexpr GuiColor accent{ 255, 205, 110, 255 }; // gold — titles, highlights
inline constexpr GuiColor danger{ 230, 70, 70, 255 };
inline constexpr GuiColor good{ 120, 220, 120, 255 };
} // namespace colors

class Gui
{
public:
    // Engine wiring. `window` owns SDL text-input state for focused inputs.
    void init(SDL_Renderer* renderer, SDL_Window* window, Textures* textures);

    // Record raw input (Engine::on_event forwards everything, before scenes).
    void handle_event(const SDL_Event& event);

    // Per-frame reset (Engine::render, before the scene stack draws). Computes
    // the pixel scale from the current output size so fullscreen stays crisp.
    void begin_frame();

    // ---- widgets (positions/sizes in screen pixels) ----

    // 9-sliced riveted panel.
    void panel(float x, float y, float w, float h);

    // Pill button; true on click. Disabled draws dim and never fires.
    bool button(std::string_view label, float x, float y, float w, float h, bool enabled = true);

    // Bitmap text. `px` is the on-screen glyph size — multiples of scale()
    // stay pixel-crisp (the font's native size is 8). Default (px <= 0) is
    // body size: 6 * scale().
    void text(float x, float y, std::string_view s, GuiColor c = colors::text, float px = 0.0f);
    void text_centered(float cx, float y, std::string_view s, GuiColor c = colors::text,
                       float px = 0.0f);
    [[nodiscard]] float text_width(std::string_view s, float px = 0.0f) const;
    // Largest glyph size <= `px` (stepping by scale()) at which `s` fits in
    // max_w — how widgets keep labels inside their box. Floors at 8px.
    [[nodiscard]] float fit_px(std::string_view s, float max_w, float px) const;

    // Single-line text field; true when ENTER is pressed while focused.
    // `numeric` filters input to digits. Click inside to focus.
    bool input(std::string_view id, std::string& buf, float x, float y, float w,
               bool numeric = false);

    // Heart pips (full / half / empty) — the pack's health icons, for menus.
    enum class Heart : std::uint8_t { Empty, Half, Full };
    void heart(float x, float y, Heart state);

    // UI pixel scale for layout math (3 at 1080p; >= 2 always).
    [[nodiscard]] float scale() const noexcept { return scale_; }
    // Body text size — the default for text()/inputs/HUD. Deliberately small
    // (widgets auto-fit to it): a scale-multiple stays chunky-but-crisp.
    [[nodiscard]] float body_px() const noexcept { return 5.0f * scale_; }
    // Default line height / widget sizes, all derived from the body size so a
    // scale change moves everything together.
    [[nodiscard]] float line_h() const noexcept { return body_px() * 1.6f; }
    [[nodiscard]] float button_h() const noexcept { return body_px() + (10.0f * scale_); }
    [[nodiscard]] float input_h() const noexcept { return body_px() + (8.0f * scale_); }
    // The content inset of a panel() — how far in from its edge a mod HUD (or
    // any caller) must place items to clear the 9-slice frame + rivets.
    [[nodiscard]] float panel_pad() const noexcept { return 8.0f * scale_; }

private:
    struct Glyph // baked quad in the font atlas (stbtt_bakedchar mirror)
    {
        float u0, v0, u1, v1; // atlas rect (px)
        float xoff, yoff, xadvance;
        float w, h;
    };

    void bake_font();
    void draw_9slice(const std::string& sprite, float x, float y, float w, float h, float border,
                     float src_w, float src_h, std::uint8_t brightness);
    // Destination 9-slice border for a widget of (w,h): border*scale, but capped
    // so two corners never overlap on a small widget (which squished the art +
    // pushed content onto the frame). This is THE robustness knob.
    [[nodiscard]] float slice_inset(float border, float w, float h) const;
    [[nodiscard]] std::uint32_t hash_id(std::string_view id) const;
    [[nodiscard]] bool mouse_in(float x, float y, float w, float h) const;

    SDL_Renderer* renderer_ = nullptr;
    SDL_Window* window_ = nullptr;
    Textures* textures_ = nullptr;

    // Font atlas (white glyphs; tinted per draw via color mod).
    SDL_Texture* font_tex_ = nullptr;
    std::vector<Glyph> glyphs_; // ASCII 32..126
    bool font_failed_ = false;

    float scale_ = 3.0f;

    // Mouse state: position sampled per frame; click = release after a press.
    float mouse_x_ = 0.0f, mouse_y_ = 0.0f;
    bool mouse_down_ = false;
    bool click_pending_ = false; // set by BUTTON_UP event, consumed next frame
    bool click_ = false;         // this frame's click edge
    float press_x_ = 0.0f, press_y_ = 0.0f; // where the press started

    // Text-input focus + this frame's typed data.
    std::uint32_t focus_id_ = 0;
    bool focus_seen_ = false; // did any input() claim focus this frame? (scene-change GC)
    std::string typed_;      // TEXT_INPUT accumulation for the focused field
    int backspaces_ = 0;
    bool submit_ = false;    // ENTER while focused
    float caret_timer_ = 0.0f;
};

} // namespace client
