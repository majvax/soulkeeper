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

// The local player's KERNEL stats, so a HUD hook can read engine handles the
// client has locally (they aren't script components). Set only on the HUD view;
// null for world draw hooks (there, engine handles resolve to nil as before).
struct LocalStats
{
    float x = 0.0f, y = 0.0f;
    float speed = 0.0f;       // Speed.value (px/s)
    float scale = 1.0f;       // Scale.value
    int hearts = 0, max_hearts = 0;             // Hearts.current / .max
    int dash_charges = 0, dash_max = 0;         // Dash.charges / .max_charges
    float dash_cooldown = 0.0f, dash_cooldown_max = 0.0f; // Dash.cooldown / .cooldown_max
};

// What a draw hook receives about the entity being drawn (screen-space `x,y`),
// plus access to its networked script components: `view:get(C.Slow)` returns a
// table of that component's fields, or nil if the entity lacks it. Inside a
// mod:hud hook `local` is also set, so `view:get(Speed/Hearts/Dash/Scale)` reads
// the local player's kernel stats.
struct DrawView
{
    float x = 0.0f;
    float y = 0.0f;
    const mod::ScriptComponentRegistry* scripts = nullptr; // kept for schema lookups
    const std::vector<mod::NetComp>* comps = nullptr;
    const LocalStats* local = nullptr; // HUD view only: local player's kernel stats

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

// Handed to a HUD hook (mod:hud): the mod opens its OWN borderless panel with
// begin_panel/end_panel, then draws text / cached icons / cooldown circles into
// it. Thin wrappers over ImGui. `end` is a Lua keyword, hence `end_panel`.
struct HudContext
{
    Textures* textures = nullptr; // cached icon lookups for image()

    // Own window: fixed, borderless, non-movable, auto-sized. Default position is
    // the top-left of the viewport work area; pass x/y to override.
    void begin_panel(const std::string& title, sol::optional<float> x, sol::optional<float> y);
    void end_panel();

    void text(const std::string& s);
    void text_colored(int r, int g, int b, const std::string& s);
    void separator();
    void same_line();
    // Cached texture (path -> Textures cache), drawn size x size at the cursor.
    void image(const std::string& path, float size);
    void image_tinted(const std::string& path, float size, int r, int g, int b, int a);
    // Cooldown/loading disc at the cursor: faint ring + a filled wedge for
    // `fraction` (0..1) of a turn from the top (full disc at >= 1). Advances the
    // cursor by 2*radius so same_line() lays several in a row.
    void pie(float radius, float fraction, int r, int g, int b, int a);

    // Balance any window a hook left open (Begin without End) — called after each
    // hook so one misbehaving mod can't corrupt ImGui state for the next.
    void close_dangling();

    int open_ = 0; // Begin/End depth for this frame's hook
};

// Register the DrawContext + DrawView + HudContext usertypes into the render VM.
// Call once, before load_dir().
void install_render_bindings(mod::LuaHost& host);

// Invoke every Object def's draw hook for the given view. `ctx_obj` is a
// persistent sol::object referencing the scene's DrawContext (reused each frame
// to avoid per-call allocation). Callback errors are logged, never fatal.
void run_object_draws(mod::LuaHost& host, const sol::object& ctx_obj, const DrawView& view);

// Invoke every mod:hud hook with (hud_ctx, view) for the local player. Mirrors
// run_object_draws; `ctx_obj` is a persistent handle to `ctx`. After each hook,
// any window it left open is balanced (ctx.close_dangling).
void run_hud_hooks(mod::LuaHost& host, const sol::object& ctx_obj, HudContext& ctx, const DrawView& view);

} // namespace client
