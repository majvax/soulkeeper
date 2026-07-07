// src/client/gui.cpp
#include "client/gui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC // ImGui compiles its own stb_truetype; keep ours TU-local
#include <stb_truetype.h>

namespace client {

namespace {
// Native pixel metrics of the pack sprites (see assets/ui/). BORDER = the
// 9-slice edge = corner-round radius + bevel (MEASURED from the art, not
// guessed — an over-large border squished small widgets and overran text).
constexpr float BTN_W = 90.0f, BTN_H = 26.0f, BTN_BORDER = 6.0f;
constexpr float PANEL_W = 176.0f, PANEL_H = 62.0f, PANEL_BORDER = 10.0f;
constexpr float PILL_W = 48.0f, PILL_H = 16.0f, PILL_BORDER = 7.0f;
constexpr float HEART_W = 18.0f, HEART_H = 15.0f;
constexpr float FONT_PX = 8.0f;  // Press Start 2P native size
constexpr int FONT_ATLAS = 256;  // 95 glyphs at 8px fit comfortably
} // namespace

void Gui::init(SDL_Renderer* renderer, SDL_Window* window, Textures* textures)
{
    renderer_ = renderer;
    window_ = window;
    textures_ = textures;
    bake_font();
}

void Gui::bake_font()
{
    std::ifstream f("assets/font/PressStart2P.ttf", std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[gui] assets/font/PressStart2P.ttf missing — no UI text\n");
        font_failed_ = true;
        return;
    }
    const std::vector<unsigned char> ttf((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());

    std::vector<unsigned char> alpha(FONT_ATLAS * FONT_ATLAS);
    std::vector<stbtt_bakedchar> baked(95);
    if (stbtt_BakeFontBitmap(ttf.data(), 0, FONT_PX, alpha.data(), FONT_ATLAS, FONT_ATLAS, 32, 95,
                             baked.data()) <= 0) {
        std::fprintf(stderr, "[gui] font bake failed — no UI text\n");
        font_failed_ = true;
        return;
    }

    // White RGBA atlas: tint per draw via SDL color mod.
    std::vector<unsigned char> rgba(static_cast<std::size_t>(FONT_ATLAS) * FONT_ATLAS * 4, 255);
    for (std::size_t i = 0; i < alpha.size(); ++i) { rgba[(i * 4) + 3] = alpha[i]; }
    font_tex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                  FONT_ATLAS, FONT_ATLAS);
    if (font_tex_ == nullptr) {
        font_failed_ = true;
        return;
    }
    SDL_UpdateTexture(font_tex_, nullptr, rgba.data(), FONT_ATLAS * 4);
    SDL_SetTextureBlendMode(font_tex_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(font_tex_, SDL_SCALEMODE_NEAREST); // pixel font: never smooth

    glyphs_.resize(95);
    for (int i = 0; i < 95; ++i) {
        const stbtt_bakedchar& b = baked[static_cast<std::size_t>(i)];
        glyphs_[static_cast<std::size_t>(i)] =
          Glyph{ .u0 = static_cast<float>(b.x0), .v0 = static_cast<float>(b.y0),
                 .u1 = static_cast<float>(b.x1), .v1 = static_cast<float>(b.y1),
                 .xoff = b.xoff, .yoff = b.yoff, .xadvance = b.xadvance,
                 .w = static_cast<float>(b.x1 - b.x0), .h = static_cast<float>(b.y1 - b.y0) };
    }
}

void Gui::handle_event(const SDL_Event& event)
{
    // Mouse position comes from EVENTS, not SDL_GetMouseState — the whole
    // client is event-driven (see GameScene's held-key set), and events are
    // also what synthetic-input tests can inject.
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        mouse_x_ = event.motion.x;
        mouse_y_ = event.motion.y;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        mouse_down_ = true;
        mouse_x_ = event.button.x;
        mouse_y_ = event.button.y;
        press_x_ = event.button.x;
        press_y_ = event.button.y;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
        mouse_down_ = false;
        mouse_x_ = event.button.x;
        mouse_y_ = event.button.y;
        click_pending_ = true;
    }
    if (focus_id_ != 0) {
        if (event.type == SDL_EVENT_TEXT_INPUT) { typed_ += event.text.text; }
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.key == SDLK_BACKSPACE) { ++backspaces_; }
            if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) { submit_ = true; }
            if (event.key.key == SDLK_ESCAPE) { focus_id_ = 0; SDL_StopTextInput(window_); }
        }
    }
}

void Gui::begin_frame()
{
    click_ = click_pending_;
    click_pending_ = false;

    // The focused field's scene went away (nothing claimed focus last frame):
    // release focus so keystrokes stop accumulating into typed_.
    if (focus_id_ != 0 && !focus_seen_) {
        focus_id_ = 0;
        typed_.clear();
        submit_ = false;
        SDL_StopTextInput(window_);
    }
    focus_seen_ = false;

    int w = 0;
    int h = 0;
    SDL_GetRenderOutputSize(renderer_, &w, &h);
    scale_ = std::max(2.0f, std::floor(static_cast<float>(h) / 360.0f));
    caret_timer_ += 1.0f / 60.0f; // caret blink needs only a rough clock
}

std::uint32_t Gui::hash_id(std::string_view id) const
{
    std::uint32_t h = 2166136261U; // FNV-1a
    for (const char c : id) { h = (h ^ static_cast<std::uint8_t>(c)) * 16777619U; }
    return h == 0 ? 1 : h;
}

bool Gui::mouse_in(float x, float y, float w, float h) const
{
    return mouse_x_ >= x && mouse_x_ < x + w && mouse_y_ >= y && mouse_y_ < y + h;
}

float Gui::slice_inset(float border, float w, float h) const
{
    // Cap the destination border so opposite corners can't overlap (which
    // squished small widgets and left content sitting on the frame). Leave a
    // 1px seam so the stretched middle row is always drawn.
    return std::max(1.0f, std::min(border * scale_, (0.5f * std::min(w, h)) - 1.0f));
}

// Draw a pack sprite 9-sliced into (x,y,w,h). border/src_* are the sprite's
// NATIVE pixel metrics; corners render at slice_inset() (border*scale, capped)
// so outlines/rivets keep the pixel size without overlapping on small widgets.
// Missing texture -> flat dark rect fallback.
void Gui::draw_9slice(const std::string& sprite, float x, float y, float w, float h, float border,
                      float src_w, float src_h, std::uint8_t brightness)
{
    SDL_Texture* tex = textures_->get(sprite);
    if (tex == nullptr) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 25, 25, 28, 235);
        const SDL_FRect rc{ .x = x, .y = y, .w = w, .h = h };
        SDL_RenderFillRect(renderer_, &rc);
        return;
    }
    SDL_SetTextureColorMod(tex, brightness, brightness, brightness);
    const float b = border;                     // source border
    const float d = slice_inset(border, w, h);  // destination border (capped)
    const float sx[4] = { 0, b, src_w - b, src_w };
    const float sy[4] = { 0, b, src_h - b, src_h };
    const float dx[4] = { x, x + d, x + w - d, x + w };
    const float dy[4] = { y, y + d, y + h - d, y + h };
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const SDL_FRect src{ .x = sx[i], .y = sy[j], .w = sx[i + 1] - sx[i],
                                 .h = sy[j + 1] - sy[j] };
            const SDL_FRect dst{ .x = dx[i], .y = dy[j], .w = dx[i + 1] - dx[i],
                                 .h = dy[j + 1] - dy[j] };
            if (dst.w > 0 && dst.h > 0) { SDL_RenderTexture(renderer_, tex, &src, &dst); }
        }
    }
    SDL_SetTextureColorMod(tex, 255, 255, 255); // Textures cache is shared — restore
}

void Gui::panel(float x, float y, float w, float h)
{
    draw_9slice("assets/ui/panel.png", x, y, w, h, PANEL_BORDER, PANEL_W, PANEL_H, 255);
}

float Gui::text_width(std::string_view s, float px) const
{
    if (font_failed_) { return 0.0f; }
    const float k = (px <= 0.0f ? body_px() : px) / FONT_PX;
    float w = 0.0f;
    for (const char c : s) {
        const int idx = c - 32;
        if (idx >= 0 && idx < 95) { w += glyphs_[static_cast<std::size_t>(idx)].xadvance * k; }
    }
    return w;
}

float Gui::fit_px(std::string_view s, float max_w, float px) const
{
    if (font_failed_ || s.empty() || max_w <= 0.0f) { return px; }
    float size = px;
    // Step down by one UI pixel until it fits; never below native (crisp 1x).
    // If it still overflows at native, the caller's clip/centering handles it.
    while (size > FONT_PX && text_width(s, size) > max_w) { size -= scale_; }
    return std::max(FONT_PX, size);
}

void Gui::text(float x, float y, std::string_view s, GuiColor c, float px)
{
    if (font_failed_ || font_tex_ == nullptr) { return; }
    const float size = px <= 0.0f ? body_px() : px;
    const float k = size / FONT_PX;
    SDL_SetTextureColorMod(font_tex_, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(font_tex_, c.a);
    float pen_x = std::floor(x);
    const float baseline = std::floor(y) + size; // y = top of the line
    for (const char ch : s) {
        const int idx = ch - 32;
        if (idx < 0 || idx >= 95) { continue; }
        const Glyph& g = glyphs_[static_cast<std::size_t>(idx)];
        const SDL_FRect src{ .x = g.u0, .y = g.v0, .w = g.w, .h = g.h };
        const SDL_FRect dst{ .x = std::floor(pen_x + (g.xoff * k)),
                             .y = std::floor(baseline + (g.yoff * k)), .w = g.w * k, .h = g.h * k };
        SDL_RenderTexture(renderer_, font_tex_, &src, &dst);
        pen_x += g.xadvance * k;
    }
    SDL_SetTextureAlphaMod(font_tex_, 255);
}

void Gui::text_centered(float cx, float y, std::string_view s, GuiColor c, float px)
{
    text(cx - (text_width(s, px) * 0.5f), y, s, c, px);
}

bool Gui::button(std::string_view label, float x, float y, float w, float h, bool enabled)
{
    const bool hot = enabled && mouse_in(x, y, w, h);
    const bool press_inside = press_x_ >= x && press_x_ < x + w && press_y_ >= y && press_y_ < y + h;
    const bool held = hot && mouse_down_ && press_inside;

    const float off = held ? scale_ : 0.0f; // press: content sinks one UI pixel
    draw_9slice("assets/ui/button.png", x, y + off, w, h, BTN_BORDER, BTN_W, BTN_H,
                enabled ? (hot ? 255 : 210) : 120);

    // Label auto-fits the interior, ALWAYS reserving a gutter for the hover
    // marker on each side — so hovering never resizes or shifts the text (the
    // old '> ' prefix did both, overflowing the box).
    const float inset = slice_inset(BTN_BORDER, w, h);
    const float gutter = body_px(); // one glyph's worth, at body size
    const float avail = std::max(1.0f, w - (2.0f * inset) - (2.0f * gutter));
    const float tp = fit_px(label, avail, body_px());
    const float lw = text_width(label, tp);
    const float lx = x + ((w - lw) * 0.5f);
    const float ty = y + off + ((h - tp) * 0.5f);
    const GuiColor col = !enabled ? colors::dim : (hot ? colors::accent : colors::text);
    // Hover marker sits in the reserved gutter with a small gap before the label.
    if (hot) { text(lx - text_width(">", tp) - (0.4f * tp), ty, ">", colors::accent, tp); }
    text(lx, ty, label, col, tp);

    // Click = the release landed inside AND the press started inside.
    return enabled && click_ && mouse_in(x, y, w, h) && press_inside;
}

bool Gui::input(std::string_view id, std::string& buf, float x, float y, float w, bool numeric)
{
    const std::uint32_t my_id = hash_id(id);
    const float h = input_h();

    // Focus follows clicks: gain on click inside, lose on click outside.
    if (click_) {
        if (mouse_in(x, y, w, h)) {
            if (focus_id_ != my_id) {
                focus_id_ = my_id;
                typed_.clear();
                backspaces_ = 0;
                SDL_StartTextInput(window_);
            }
        } else if (focus_id_ == my_id) {
            focus_id_ = 0;
            SDL_StopTextInput(window_);
        }
    }
    const bool focused = focus_id_ == my_id;
    if (focused) {
        focus_seen_ = true; // keep begin_frame's focus GC from reclaiming us
        // ImGui's SDL3 backend stops SDL text input when IT doesn't want IME
        // (imgui_impl_sdl3 UpdateIme) — re-assert ours every frame we're focused.
        if (!SDL_TextInputActive(window_)) { SDL_StartTextInput(window_); }
        for (const char c : typed_) {
            if (numeric && (c < '0' || c > '9')) { continue; }
            if (c >= 32 && buf.size() < 64) { buf.push_back(c); }
        }
        typed_.clear();
        while (backspaces_ > 0) {
            if (!buf.empty()) { buf.pop_back(); }
            --backspaces_;
        }
    }

    draw_9slice("assets/ui/pill_dark.png", x, y, w, h, PILL_BORDER, PILL_W, PILL_H,
                focused ? 255 : 200);
    // Text starts clear of the pill's rounded end (= its 9-slice inset).
    const float pad = slice_inset(PILL_BORDER, w, h);
    const float tp = body_px();
    std::string shown = buf;
    if (focused && std::fmod(caret_timer_, 1.0f) < 0.6f) { shown += '_'; }
    // Clip from the left so the caret end stays visible in narrow fields.
    while (!shown.empty() && text_width(shown, tp) > w - (pad * 2.0f)) { shown.erase(0, 1); }
    text(x + pad, y + ((h - tp) * 0.5f), shown, focused ? colors::text : colors::dim, tp);

    const bool submitted = focused && submit_;
    if (submitted) { submit_ = false; }
    return submitted;
}

void Gui::heart(float x, float y, Heart state)
{
    const char* sprite = state == Heart::Full    ? "assets/ui/heart_full.png"
                         : state == Heart::Half ? "assets/ui/heart_half.png"
                                                 : "assets/ui/heart_broken.png";
    SDL_Texture* tex = textures_->get(sprite);
    if (tex == nullptr) { return; }
    const SDL_FRect dst{ .x = x, .y = y, .w = HEART_W * scale_, .h = HEART_H * scale_ };
    SDL_RenderTexture(renderer_, tex, nullptr, &dst);
}

} // namespace client
