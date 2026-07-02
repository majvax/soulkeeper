// src/client/mod/render_bindings.cpp
#include "client/mod/render_bindings.hpp"

#include <cstdio>

#include <imgui.h>

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

void DrawContext::circle_filled(float cx, float cy, float radius, int r, int g, int b, int a)
{
    ImGui::GetForegroundDrawList()->AddCircleFilled(
      ImVec2(cx, cy), radius, IM_COL32(r, g, b, a));
}

void DrawContext::circle(float cx, float cy, float radius, int r, int g, int b, int a, float thickness)
{
    ImGui::GetForegroundDrawList()->AddCircle(
      ImVec2(cx, cy), radius, IM_COL32(r, g, b, a), 0, thickness);
}

void DrawContext::text(float x, float y, const std::string& s, int r, int g, int b, int a)
{
    ImGui::GetForegroundDrawList()->AddText(ImVec2(x, y), IM_COL32(r, g, b, a), s.c_str());
}

sol::object DrawView::get(const std::string& id, sol::this_state ts) const
{
    if (scripts == nullptr || comps == nullptr) { return sol::lua_nil; }
    const mod::ScriptSchema* schema = scripts->by_id(id);
    if (schema == nullptr || schema->net_id < 0) { return sol::lua_nil; }
    for (const mod::NetComp& c : *comps) {
        if (c.net_id != schema->net_id) { continue; }
        sol::table t = sol::state_view(ts).create_table();
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
    lua.new_usertype<DrawView>("DrawView", sol::no_constructor, "x", &DrawView::x, "y", &DrawView::y,
                               "get", &DrawView::get);
    lua.new_usertype<DrawContext>("DrawContext", sol::no_constructor,
                                  "texture", &DrawContext::texture, "rect", &DrawContext::rect,
                                  "circle_filled", &DrawContext::circle_filled, "circle", &DrawContext::circle,
                                  "text", &DrawContext::text, "world_to_screen", &DrawContext::world_to_screen);
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
}

} // namespace client
