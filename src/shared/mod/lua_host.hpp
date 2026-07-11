// src/shared/mod/lua_host.hpp
//
// Owns a Lua VM (sol::state) plus the mod state it populates: a ContentRegistry
// and an EventBus. SDL-free, so both the server (sim VM) and client (render VM)
// use it. The platform installs its own bindings (sim_bindings / render_bindings)
// BEFORE load_dir(); plugins then register content via `register_mod(...)`.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "shared/mod/bindings_table.hpp"
#include "shared/mod/component_ref.hpp"
#include "shared/mod/events.hpp"
#include "shared/mod/registry.hpp"
#include "shared/mod/script_ecs.hpp"
#include "shared/protocol.hpp"

namespace mod {

// A discovered plugin whose main() hasn't run yet (import()/pass 2 runs it).
struct PendingPlugin
{
    sol::protected_function main;
    std::string dir;
};

// Everything a plugin can register into, in one heap block so the registration
// closures capture a stable pointer (not `this`, which would dangle when the
// owning object — e.g. the client Engine — is moved by value).
struct ModState
{
    ContentRegistry registry;
    EnemyRegistry enemies;
    EventBus events;
    ScriptComponentRegistry scripts;
    std::vector<ScriptSystem> script_systems;

    // Plugin loader state (import()).
    std::vector<std::string> plugin_order;                    // discovery order (folder names)
    std::unordered_map<std::string, PendingPlugin> pending;   // folder name -> unrun main()
    std::unordered_map<std::string, sol::object> exports;     // folder name -> main()'s return
    std::vector<std::string> import_stack;                    // cycle detection
    std::string current_plugin;                               // folder of the running main()
    std::string current_dir;                                  // its dir (for include())

    // Engine component dispatch (filled by install_sim_bindings; sim VM only).
    std::shared_ptr<BindingTable> bindings;

    // Player visuals declared by mods (mod:player_sprite). Pure render-VM
    // metadata — never hashed, never simulated. Last declaration wins.
    std::string player_sprite;

    // Sound bindings declared by mods (mod:sound): name -> file path (repo-root
    // relative, like sprites). Pure render-VM metadata (the sim is headless);
    // binding a kernel name ("shoot", "music_game", ...) reskins that sound.
    // Kept in registration order — the client applies them in order, so the
    // last declaration wins, matching player_sprite.
    std::vector<std::pair<std::string, std::string>> sounds;

    // HUD panel hooks (mod:hud). Render-VM only: each is called once per frame
    // with (hud_ctx, local-player view) so plugins can print the local player's
    // stats into the HUD. Stored in BOTH VMs but only the client ever calls them.
    std::vector<sol::protected_function> hud_hooks;

    // World draw hooks (mod:draw). Render-VM only: each runs once per PLAYER
    // entity per frame with (ctx, view) — view carries that player's screen pos
    // + networked script comps — so plugins draw per-player world overlays
    // (revive arcs, auras) without owning an Object.
    std::vector<sol::protected_function> draw_hooks;

    // Level-up offer hook (mod:level_offer). Sim-VM only: fn(player, level)
    // returns the card list `{ { id = "core:...", rarity = 0..4 }, ... }` —
    // THE GAME rolls the offer; the engine only validates, stores (reconnects
    // re-send, never re-roll) and ships it. One hook; last registration wins.
    sol::protected_function level_offer;

    // XP cost curve hook (mod:xp_curve). Sim-VM only: fn(level) -> XP needed
    // to reach the NEXT level. Leveling pace is game balance, so the GAME owns
    // the curve; the engine falls back to its linear default without a hook.
    sol::protected_function xp_curve;

    // Console commands (mod:command). Registered in BOTH VMs from the same
    // mod.lua: the SIM VM runs fn (server side, host-typed `/name args...`);
    // the render VM only reads name+usage for console autocompletion/help.
    struct ConsoleCommand
    {
        std::string name;  // bare (typed as /name — no namespace, first wins)
        std::string usage; // e.g. "/givexp <amount>  -- add team XP"
        sol::protected_function fn; // fn(player, args...) — numeric args arrive as numbers
    };
    std::vector<ConsoleCommand> commands;

    // Floating combat numbers queued by world:damage_number (sim VM only).
    // The server drains this every snapshot tick into a DamageEvents packet;
    // capped at max_damage_events so a mod loop can't flood the wire.
    std::vector<proto::DamageEvent> damage_events;
};

class LuaHost
{
public:
    LuaHost();

    [[nodiscard]] sol::state& lua() noexcept { return lua_; }
    [[nodiscard]] ContentRegistry& registry() noexcept { return state_->registry; }
    [[nodiscard]] const ContentRegistry& registry() const noexcept { return state_->registry; }
    [[nodiscard]] EnemyRegistry& enemies() noexcept { return state_->enemies; }
    [[nodiscard]] const EnemyRegistry& enemies() const noexcept { return state_->enemies; }
    [[nodiscard]] EventBus& events() noexcept { return state_->events; }
    [[nodiscard]] const EventBus& events() const noexcept { return state_->events; }
    [[nodiscard]] ScriptComponentRegistry& scripts() noexcept { return state_->scripts; }
    [[nodiscard]] std::vector<ScriptSystem>& script_systems() noexcept { return state_->script_systems; }
    [[nodiscard]] ModState& state() noexcept { return *state_; }
    [[nodiscard]] const std::string& player_sprite() const noexcept { return state_->player_sprite; }
    [[nodiscard]] const std::shared_ptr<BindingTable>& bindings() const noexcept { return state_->bindings; }

    // Discover every `<dir>/*/mod.lua`, run it, call its global main(), then
    // finalize the registry (sort ids -> deterministic wire ids). Safe to call
    // once, after bindings are installed. Errors are logged, never fatal.
    void load_dir(const std::string& dir);

    // Deterministic FNV-1a hash of the registered plugin identity (content ids
    // + kinds, enemy ids, script schemas), seeded with proto::protocol_version.
    // Sent in Join so the server can reject a mismatched mods/ set before it
    // silently desyncs. Valid after load_dir().
    [[nodiscard]] std::uint64_t plugin_hash() const noexcept { return plugin_hash_; }

private:
    void install_registration_api(); // register_mod/import/prelude + the Mod usertype
    void finalize_content();         // sort registries -> wire ids, then hash
    void compute_plugin_hash();      // cache plugin_hash_ (end of load_dir)

    sol::state lua_;
    std::unique_ptr<ModState> state_ = std::make_unique<ModState>();
    std::uint64_t plugin_hash_ = 0;
};

} // namespace mod
