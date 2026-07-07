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

#include "client/audio.hpp"
#include "client/gui.hpp"
#include "client/renderer.hpp"
#include "core/ecs.hpp"
#include "shared/mod/bindings_table.hpp"
#include "shared/mod/lua_host.hpp"
#include "shared/mod/script_ecs.hpp"

namespace client {

// What a draw hook receives about the entity being drawn (screen-space `x,y`),
// plus access to its networked script components: `view:get(C.Slow)` returns a
// table of that component's fields, or nil if the entity lacks it. Inside a
// mod:hud hook `reg`/`entity`/`table` are also set, so `view:get(Speed/Hearts/
// Dash/Scale/Position)` dispatches through the SAME kernel-component schema as
// the sim (see mod::register_engine_components) — no hardcoded field-name copy.
struct DrawView
{
    float x = 0.0f;
    float y = 0.0f;
    const mod::ScriptComponentRegistry* scripts = nullptr; // kept for schema lookups
    const std::vector<mod::NetComp>* comps = nullptr;
    // HUD view only: the client's render registry + local player entity + the
    // kernel dispatch table, so engine handles resolve like on the sim side.
    // Left null for world draw hooks (there, engine handles resolve to nil).
    core::Registry* reg = nullptr;
    core::Entity entity = core::null_entity;
    const mod::BindingTable* table = nullptr;

    [[nodiscard]] sol::object get(const mod::ComponentRef& ref, sol::this_state ts) const;
};

// Reusable per-frame drawing surface handed to plugin draw hooks. The scene
// owns one and refreshes `ox/oy` (camera offset) each frame.
struct DrawContext
{
    SDL_Renderer* renderer = nullptr;
    Textures* textures = nullptr;
    Audio* audio = nullptr;   // one-shot SFX from hooks (null in scenes w/o audio)
    float listener_x = 0.0f;  // local player world pos — play_at attenuation
    float listener_y = 0.0f;
    float ox = 0.0f; // world -> screen offset
    float oy = 0.0f;

    // One-shot SFX by bound name (mod:sound or a kernel name). play_at applies
    // distance falloff from the local player. Hooks run per frame — the caller
    // edge-detects; the 40 ms same-name throttle only softens mistakes.
    void play(const std::string& name, sol::optional<float> volume)
    {
        if (audio != nullptr) { audio->play(name, volume.value_or(1.0f)); }
    }
    void play_at(const std::string& name, float wx, float wy, sol::optional<float> volume)
    {
        if (audio != nullptr) {
            audio->play_at(name, wx, wy, listener_x, listener_y, volume.value_or(1.0f));
        }
    }

    // Mod camera control: while locked, the scene camera smooth-pans to and
    // holds the given WORLD point instead of following the player (takes
    // effect next frame; release() pans back). Call from any render hook.
    bool cam_locked = false;
    float cam_x = 0.0f, cam_y = 0.0f;
    void camera_lock(float wx, float wy)
    {
        cam_locked = true;
        cam_x = wx;
        cam_y = wy;
    }
    void camera_release() { cam_locked = false; }

    void texture(const std::string& path, float x, float y, float w, float h);
    void rect(float x, float y, float w, float h, int r, int g, int b, int a);
    void circle_filled(float cx, float cy, float radius, int r, int g, int b, int a);
    void circle(float cx, float cy, float radius, int r, int g, int b, int a, float thickness);
    void arc(float cx, float cy, float radius, float fraction, int r, int g, int b, int a,
             float thickness); // partial ring, clockwise from the top (progress cues)
    void text(float x, float y, const std::string& s, int r, int g, int b, int a);
    [[nodiscard]] std::pair<float, float> world_to_screen(float wx, float wy) const
    {
        return { wx + ox, wy + oy };
    }
};

// Handed to a HUD hook (mod:hud): the mod opens its OWN panel with
// begin_panel/end_panel, then draws text / cached icons / cooldown discs into
// it. Rendered with the game's widget kit (client/gui.hpp) — items are
// BUFFERED between begin/end so the 9-sliced panel can auto-size to its
// content before anything draws. `end` is a Lua keyword, hence `end_panel`.
struct HudContext
{
    Textures* textures = nullptr; // cached icon lookups for image()
    Gui* gui = nullptr;           // panel + bitmap text
    SDL_Renderer* renderer = nullptr;

    // Auto-sized panel. Default position is the top-left; pass x/y to override.
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

    // Flush a panel a hook left open (errored before end_panel) — called after
    // each hook so one misbehaving mod can't scramble layout for the next.
    void close_dangling();

private:
    struct Item // one buffered draw at a content-relative position
    {
        enum class Kind : std::uint8_t { Text, Image, Pie, Separator };
        Kind kind;
        float x, y, size, frac;
        GuiColor col;
        std::string str; // text or texture path
    };
    // Place an item of (w,h) at the cursor (honoring same_line), record its
    // content-relative position, and advance the layout.
    std::pair<float, float> place(float w, float h);
    [[nodiscard]] float text_px() const; // HUD line size at the current UI scale

    std::vector<Item> items_;
    bool open_ = false;
    float panel_x_ = 0.0f, panel_y_ = 0.0f;
    float cursor_y_ = 0.0f;   // next row's top
    float row_end_x_ = 0.0f;  // last item's right edge (same_line anchor)
    float row_y_ = 0.0f;      // last item's top
    float row_h_ = 0.0f;      // tallest item on the current row
    float max_w_ = 0.0f;
    bool same_line_ = false;
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
