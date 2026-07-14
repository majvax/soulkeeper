// src/client/mod/render_bindings.cpp
#include "client/mod/render_bindings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <utility>

#include <imgui.h>

#include "shared/mod/sim_bindings.hpp" // register_engine_components (shared kernel schema)

namespace client {

void DrawContext::texture(const std::string& path, float x, float y, float w, float h)
{
    if (textures == nullptr) { return; }
    if (SDL_Texture* t = textures->get(path)) { draw_centered(renderer, t, x, y, w, h); }
}

void DrawContext::rect(float x, float y, float w, float h, int r, int g, int b, int a)
{
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, static_cast<Uint8>(r), static_cast<Uint8>(g), static_cast<Uint8>(b),
                           static_cast<Uint8>(a));
    const SDL_FRect rc{ .x = x, .y = y, .w = w, .h = h };
    SDL_RenderFillRect(renderer, &rc);
}

// These world overlays draw into the BACKGROUND list: above the SDL-rendered
// world but BELOW every ImGui window, so an aura or label never covers a scene
// stacked on top (e.g. the level-up card menu). Matches rect/texture, which go
// straight through SDL and are likewise behind the UI.
void DrawContext::circle_filled(float cx, float cy, float radius, int r, int g, int b, int a)
{
    ImGui::GetBackgroundDrawList()->AddCircleFilled(
      ImVec2(cx, cy), radius, IM_COL32(r, g, b, a));
}

void DrawContext::circle(float cx, float cy, float radius, int r, int g, int b, int a, float thickness)
{
    ImGui::GetBackgroundDrawList()->AddCircle(
      ImVec2(cx, cy), radius, IM_COL32(r, g, b, a), 0, thickness);
}

// Partial ring for progress cues (revive arcs, cast bars): `fraction` (0..1)
// of a turn, clockwise from the top.
void DrawContext::arc(float cx, float cy, float radius, float fraction, int r, int g, int b, int a,
                      float thickness)
{
    const float frac = std::clamp(fraction, 0.0f, 1.0f);
    if (frac <= 0.0f) { return; }
    constexpr float top = -std::numbers::pi_v<float> / 2.0f;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    list->PathArcTo(ImVec2(cx, cy), radius, top, top + (frac * 2.0f * std::numbers::pi_v<float>), 32);
    list->PathStroke(IM_COL32(r, g, b, a), 0, thickness);
}

void DrawContext::text(float x, float y, const std::string& s, int r, int g, int b, int a)
{
    ImGui::GetBackgroundDrawList()->AddText(ImVec2(x, y), IM_COL32(r, g, b, a), s.c_str());
}

float HudContext::text_px() const
{
    return gui != nullptr ? gui->body_px() : 12.0f;
}

// Layout: items flow top-to-bottom; same_line() chains the next item onto the
// current row. Returns the item's content-relative position.
std::pair<float, float> HudContext::place(float w, float h)
{
    const float s = gui != nullptr ? gui->scale() : 3.0f;
    float x = 0.0f;
    float y = cursor_y_;
    if (same_line_) {
        x = row_end_x_ + (4.0f * s); // gap between same-line items
        y = row_y_;
        same_line_ = false;
    } else {
        row_y_ = y;
        row_h_ = 0.0f;
    }
    row_end_x_ = x + w;
    row_h_ = std::max(row_h_, h);
    cursor_y_ = row_y_ + row_h_ + (3.0f * s); // row gap
    max_w_ = std::max(max_w_, row_end_x_);
    return { x, y };
}

void HudContext::begin_panel(const std::string& title, sol::optional<float> x,
                             sol::optional<float> y)
{
    const float s = gui != nullptr ? gui->scale() : 3.0f;
    if (open_) { end_panel(); } // a hook forgot end_panel between two begins
    open_ = true;
    panel_x_ = x.value_or(4.0f * s);
    panel_y_ = y.value_or(4.0f * s);
    items_.clear();
    cursor_y_ = 0.0f;
    row_end_x_ = 0.0f;
    row_y_ = 0.0f;
    row_h_ = 0.0f;
    max_w_ = 0.0f;
    same_line_ = false;

    // A non-empty title becomes the panel heading (accent) + a divider, so
    // `hud:begin_panel("Stats")` reads as a titled panel instead of the title
    // being silently dropped. Buffered like any other item, above the content.
    if (!title.empty() && gui != nullptr) {
        text_colored(colors::accent.r, colors::accent.g, colors::accent.b, title);
        separator();
    }
}

// Draw everything: the auto-sized 9-slice panel first, then the buffered items
// offset by the content origin.
void HudContext::end_panel()
{
    if (!open_ || gui == nullptr || renderer == nullptr) {
        open_ = false;
        return;
    }
    open_ = false;
    const float s = gui->scale();
    const float pad = gui->panel_pad(); // clear the 9-slice frame + rivets
    gui->panel(panel_x_, panel_y_, max_w_ + (pad * 2.0f), (cursor_y_ - (3.0f * s)) + (pad * 2.0f));
    const float ox = panel_x_ + pad;
    const float oy = panel_y_ + pad;

    for (const Item& it : items_) {
        const float x = ox + it.x;
        const float y = oy + it.y;
        switch (it.kind) {
        case Item::Kind::Text: gui->text(x, y, it.str, it.col, text_px()); break;
        case Item::Kind::Separator: {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 45);
            const SDL_FRect line{ .x = ox, .y = y, .w = max_w_, .h = 1.0f };
            SDL_RenderFillRect(renderer, &line);
            break;
        }
        case Item::Kind::Bar: {
            // Full-content-width track + a filled portion for `frac`.
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            const SDL_FRect track{ .x = ox, .y = y, .w = max_w_, .h = it.size };
            SDL_SetRenderDrawColor(renderer, 20, 20, 24, 220);
            SDL_RenderFillRect(renderer, &track);
            const SDL_FRect fill{ .x = ox, .y = y, .w = max_w_ * std::clamp(it.frac, 0.0f, 1.0f),
                                  .h = it.size };
            SDL_SetRenderDrawColor(renderer, it.col.r, it.col.g, it.col.b, it.col.a);
            SDL_RenderFillRect(renderer, &fill);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 50); // thin frame
            SDL_RenderRect(renderer, &track);
            break;
        }
        case Item::Kind::Image: {
            SDL_Texture* tex = (textures != nullptr) ? textures->get(it.str) : nullptr;
            if (tex != nullptr) {
                SDL_SetTextureColorMod(tex, it.col.r, it.col.g, it.col.b);
                SDL_SetTextureAlphaMod(tex, it.col.a);
                const SDL_FRect dst{ .x = x, .y = y, .w = it.size, .h = it.size };
                SDL_RenderTexture(renderer, tex, nullptr, &dst);
                SDL_SetTextureColorMod(tex, 255, 255, 255); // shared cache — restore
                SDL_SetTextureAlphaMod(tex, 255);
            }
            break;
        }
        case Item::Kind::Pie: {
            // Triangle-fan wedge + faint full ring, clockwise from the top.
            const float r = it.size;
            const SDL_FPoint c{ .x = x + r, .y = y + r };
            const SDL_FColor col{ .r = static_cast<float>(it.col.r) / 255.0f,
                                  .g = static_cast<float>(it.col.g) / 255.0f,
                                  .b = static_cast<float>(it.col.b) / 255.0f,
                                  .a = static_cast<float>(it.col.a) / 255.0f };
            constexpr int segs = 24;
            constexpr float tau = 2.0f * std::numbers::pi_v<float>;
            const float frac = std::clamp(it.frac, 0.0f, 1.0f);
            std::vector<SDL_Vertex> verts;
            const int used = static_cast<int>(frac * segs);
            for (int i = 0; i < used; ++i) {
                const float a0 = -(tau / 4.0f) + (static_cast<float>(i) / segs * tau);
                const float a1 = -(tau / 4.0f) + (static_cast<float>(i + 1) / segs * tau);
                verts.push_back({ .position = c, .color = col, .tex_coord = {} });
                verts.push_back({ .position = { .x = c.x + (std::cos(a0) * r),
                                                .y = c.y + (std::sin(a0) * r) },
                                  .color = col, .tex_coord = {} });
                verts.push_back({ .position = { .x = c.x + (std::cos(a1) * r),
                                                .y = c.y + (std::sin(a1) * r) },
                                  .color = col, .tex_coord = {} });
            }
            if (!verts.empty()) {
                SDL_RenderGeometry(renderer, nullptr, verts.data(),
                                   static_cast<int>(verts.size()), nullptr, 0);
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 40); // the faint ring
            SDL_FPoint ring[segs + 1];
            for (int i = 0; i <= segs; ++i) {
                const float a = static_cast<float>(i) / segs * tau;
                ring[i] = { .x = c.x + (std::cos(a) * r), .y = c.y + (std::sin(a) * r) };
            }
            SDL_RenderLines(renderer, ring, segs + 1);
            break;
        }
        }
    }
}

// Flush a panel a hook opened but didn't close (it errored mid-draw, which the
// protected call swallowed) — the buffered items still land on screen.
void HudContext::close_dangling()
{
    if (open_) { end_panel(); }
}

void HudContext::text(const std::string& s) { text_colored(220, 220, 220, s); }

void HudContext::text_colored(int r, int g, int b, const std::string& s)
{
    if (!open_ || gui == nullptr) { return; }
    const auto [x, y] = place(gui->text_width(s, text_px()), text_px());
    items_.push_back({ .kind = Item::Kind::Text, .x = x, .y = y, .size = 0.0f, .frac = 0.0f,
                       .col = { static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                                static_cast<std::uint8_t>(b), 255 },
                       .str = s });
}

void HudContext::separator()
{
    if (!open_) { return; }
    const auto [x, y] = place(0.0f, 5.0f); // full width resolved at end_panel
    items_.push_back({ .kind = Item::Kind::Separator, .x = x, .y = y + 2.0f, .size = 0.0f,
                       .frac = 0.0f, .col = {}, .str = {} });
}

void HudContext::bar(float fraction, int r, int g, int b, int a)
{
    if (!open_ || gui == nullptr) { return; }
    const float bh = gui->body_px() * 0.7f;      // bar height
    const auto [x, y] = place(0.0f, bh);          // width resolved at end_panel
    items_.push_back({ .kind = Item::Kind::Bar, .x = x, .y = y, .size = bh, .frac = fraction,
                       .col = { static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                                static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a) },
                       .str = {} });
}

void HudContext::same_line() { same_line_ = true; }

void HudContext::image(const std::string& path, float size)
{
    image_tinted(path, size, 255, 255, 255, 255);
}

void HudContext::image_tinted(const std::string& path, float size, int r, int g, int b, int a)
{
    if (!open_) { return; }
    const auto [x, y] = place(size, size);
    items_.push_back({ .kind = Item::Kind::Image, .x = x, .y = y, .size = size, .frac = 0.0f,
                       .col = { static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                                static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a) },
                       .str = path });
}

void HudContext::pie(float radius, float fraction, int r, int g, int b, int a)
{
    if (!open_) { return; }
    const auto [x, y] = place(radius * 2.0f, radius * 2.0f);
    items_.push_back({ .kind = Item::Kind::Pie, .x = x, .y = y, .size = radius, .frac = fraction,
                       .col = { static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                                static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a) },
                       .str = {} });
}

sol::object DrawView::get(const mod::ComponentRef& ref, sol::this_state ts) const
{
    sol::state_view lua(ts);
    // Engine (kernel) handles: dispatch through the SAME BindingTable the sim
    // uses (mirror of sim_bindings' EntityHandle::get), over the client's render
    // registry. Set only on the HUD view; null elsewhere -> nil, as before.
    if (ref.is_engine()) {
        if (reg == nullptr || table == nullptr
            || ref.engine_tag >= static_cast<int>(table->size())) {
            return sol::lua_nil;
        }
        return (*table)[ref.engine_tag].get(ts, *reg, entity);
    }
    // Script (Lua-defined) components: read from the entity's networked blob.
    if (comps == nullptr) { return sol::lua_nil; }
    const mod::ScriptSchema* schema = ref.schema;
    if (schema == nullptr || schema->net_id < 0) { return sol::lua_nil; }
    for (const mod::NetComp& c : *comps) {
        if (c.net_id != schema->net_id) { continue; }
        sol::table t = lua.create_table();
        for (std::size_t i = 0; i < schema->fields.size() && i < c.values.size(); ++i) {
            t[schema->fields[i]] = c.values[i];
        }
        return t;
    }
    return sol::lua_nil;
}

void install_render_bindings(mod::LuaHost& host)
{
    sol::state& lua = host.lua();

    // Give the render VM the same kernel-component dispatch table as the sim, so
    // a HUD hook's view:get(Hearts/Speed/Dash/...) resolves through one schema
    // (no client-only field-name copy). The render VM already has the prelude
    // Hearts/Dash/... handles from lua_host; this fills in the missing table.
    auto table = std::make_shared<mod::BindingTable>();
    mod::register_engine_components(lua, *table);
    host.state().bindings = table;

    lua.new_usertype<DrawView>("DrawView", sol::no_constructor, "x", &DrawView::x, "y", &DrawView::y,
                               "get", &DrawView::get);
    lua.new_usertype<DrawContext>("DrawContext", sol::no_constructor,
                                  "texture", &DrawContext::texture, "rect", &DrawContext::rect,
                                  "circle_filled", &DrawContext::circle_filled, "circle", &DrawContext::circle,
                                  "arc", &DrawContext::arc,
                                  "play", &DrawContext::play, "play_at", &DrawContext::play_at,
                                  "camera_lock", &DrawContext::camera_lock,
                                  "camera_release", &DrawContext::camera_release,
                                  "text", &DrawContext::text, "world_to_screen", &DrawContext::world_to_screen);
    lua.new_usertype<HudContext>(
      "HudContext", sol::no_constructor, "begin_panel", &HudContext::begin_panel, "end_panel",
      &HudContext::end_panel, "text", &HudContext::text, "text_colored", &HudContext::text_colored,
      "bar", &HudContext::bar, "separator", &HudContext::separator, "same_line",
      &HudContext::same_line, "image", &HudContext::image, "image_tinted", &HudContext::image_tinted,
      "pie", &HudContext::pie,
      // Team run state (read-only), so the stats panel can show the XP bar.
      "level", sol::readonly(&HudContext::level), "wave", sol::readonly(&HudContext::wave), "xp",
      sol::readonly(&HudContext::xp),
      // CTRL held: the panel may expand into its full stat breakdown.
      "detail", sol::readonly(&HudContext::detail));
}

void run_object_draws(mod::LuaHost& host, const sol::object& ctx_obj, const DrawView& view)
{
    for (const mod::ContentDef& d : host.registry().defs()) {
        if (d.kind != mod::ContentKind::Object || !d.draw.valid()) { continue; }
        sol::protected_function_result res = d.draw(ctx_obj, view);
        if (!res.valid()) {
            const sol::error err = res;
            std::fprintf(stderr, "[mod] '%s' draw() error: %s\n", d.id.c_str(), err.what());
        }
    }
    // Object-less world draw hooks (mod:draw) run on the same per-player view.
    for (const sol::protected_function& hook : host.state().draw_hooks) {
        if (!hook.valid()) { continue; }
        sol::protected_function_result res = hook(ctx_obj, view);
        if (!res.valid()) {
            const sol::error err = res;
            std::fprintf(stderr, "[mod] draw() error: %s\n", err.what());
        }
    }
}

void run_hud_hooks(mod::LuaHost& host, const sol::object& ctx_obj, HudContext& ctx, const DrawView& view)
{
    for (const sol::protected_function& hook : host.state().hud_hooks) {
        if (!hook.valid()) { continue; }
        sol::protected_function_result res = hook(ctx_obj, view);
        if (!res.valid()) {
            const sol::error err = res;
            std::fprintf(stderr, "[mod] hud() error: %s\n", err.what());
        }
        ctx.close_dangling(); // balance Begin/End even if the hook errored mid-draw
    }
}

} // namespace client
