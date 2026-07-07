// src/client/mod/render_bindings.cpp
#include "client/mod/render_bindings.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <numbers>

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

void HudContext::begin_panel(const std::string& title, sol::optional<float> x, sol::optional<float> y)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float px = x.value_or(vp->WorkPos.x + 12.0f);
    const float py = y.value_or(vp->WorkPos.y + 12.0f);
    ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                       | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize
                                       | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
                                       | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin(title.c_str(), nullptr, flags); // Begin/End must balance regardless of return
    ++open_;
}

void HudContext::end_panel()
{
    if (open_ > 0) {
        ImGui::End();
        --open_;
    }
}

// Close any window a hook opened but didn't (e.g. it errored mid-draw, which the
// protected call swallowed) — an unbalanced ImGui Begin corrupts state + crashes.
void HudContext::close_dangling()
{
    while (open_ > 0) {
        ImGui::End();
        --open_;
    }
}

void HudContext::text(const std::string& s) { ImGui::TextUnformatted(s.c_str()); }

void HudContext::text_colored(int r, int g, int b, const std::string& s)
{
    ImGui::TextColored(ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                              static_cast<float>(b) / 255.0f, 1.0f),
                       "%s", s.c_str());
}

void HudContext::separator() { ImGui::Separator(); }

void HudContext::same_line() { ImGui::SameLine(); }

void HudContext::image(const std::string& path, float size)
{
    image_tinted(path, size, 255, 255, 255, 255);
}

void HudContext::image_tinted(const std::string& path, float size, int r, int g, int b, int a)
{
    SDL_Texture* tex = (textures != nullptr) ? textures->get(path) : nullptr;
    const ImVec4 tint(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                      static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f);
    if (tex != nullptr) {
        ImGui::Image(reinterpret_cast<ImTextureID>(tex), ImVec2(size, size), ImVec2(0, 0),
                     ImVec2(1, 1), tint, ImVec4(0, 0, 0, 0));
    } else {
        ImGui::Dummy(ImVec2(size, size)); // keep layout stable if the icon is missing
    }
}

void HudContext::pie(float radius, float fraction, int r, int g, int b, int a)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 center(p.x + radius, p.y + radius);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = IM_COL32(r, g, b, a);
    dl->AddCircle(center, radius, IM_COL32(255, 255, 255, 40), 0, 2.0f); // faint ring
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction >= 1.0f) {
        dl->AddCircleFilled(center, radius, col);
    } else if (fraction > 0.0f) {
        constexpr float pi = 3.14159265358979f;
        const float a0 = -pi * 0.5f; // start at the top
        dl->PathLineTo(center);
        dl->PathArcTo(center, radius, a0, a0 + (fraction * 2.0f * pi));
        dl->PathFillConvex(col);
    }
    ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f)); // reserve + advance the cursor
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
      "separator", &HudContext::separator, "same_line", &HudContext::same_line, "image",
      &HudContext::image, "image_tinted", &HudContext::image_tinted, "pie", &HudContext::pie);
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
