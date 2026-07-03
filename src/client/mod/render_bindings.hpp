// src/client/mod/render_bindings.hpp
//
// Client-side (render VM) Lua bindings: a draw context wrapping SDL/ImGui plus a
// per-player view, so plugins can draw custom visuals (e.g. the Onion aura ring)
// every frame. Lives in client/ — the only layer allowed to touch SDL/ImGui.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "client/renderer.hpp"
#include "shared/mod/lua_host.hpp"
#include "shared/mod/script_ecs.hpp"

namespace client {

// What a draw hook receives about the entity being drawn (screen-space `x,y`),
// plus access to its networked script components: `view:get(C.Slow)` returns a
// table of that component's fields, or nil if the entity lacks it.
struct DrawView
{
    float x = 0.0f;
    float y = 0.0f;
    const mod::ScriptComponentRegistry* scripts = nullptr; // kept for schema lookups
    const std::vector<mod::NetComp>* comps = nullptr;

    [[nodiscard]] sol::object get(const mod::ComponentRef& ref, sol::this_state ts) const;
};

// Reusable per-frame drawing surface handed to plugin draw hooks. The scene
// owns one and refreshes `ox/oy` (camera offset) each frame.
struct DrawContext
{
    SDL_Renderer* renderer = nullptr;
    Textures* textures = nullptr;
    float ox = 0.0f; // world -> screen offset
    float oy = 0.0f;

    void texture(const std::string& path, float x, float y, float w, float h);
    void rect(float x, float y, float w, float h, int r, int g, int b, int a);
    void circle_filled(float cx, float cy, float radius, int r, int g, int b, int a);
    void circle(float cx, float cy, float radius, int r, int g, int b, int a, float thickness);
    void text(float x, float y, const std::string& s, int r, int g, int b, int a);
    [[nodiscard]] std::pair<float, float> world_to_screen(float wx, float wy) const
    {
        return { wx + ox, wy + oy };
    }
};

// Register the DrawContext + DrawView usertypes into the render VM. Call once,
// before load_dir().
void install_render_bindings(mod::LuaHost& host);

// Invoke every Object def's draw hook for the given view. `ctx_obj` is a
// persistent sol::object referencing the scene's DrawContext (reused each frame
// to avoid per-call allocation). Callback errors are logged, never fatal.
void run_object_draws(mod::LuaHost& host, const sol::object& ctx_obj, const DrawView& view);

} // namespace client
