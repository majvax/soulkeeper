// src/shared/mod/sim_bindings.hpp
//
// Server-side (sim VM) Lua bindings: component usertypes, an entity handle that
// mutates the ECS, spawn factories, and dispatch helpers that invoke a def's
// apply/available/acquire callbacks. SDL-free (lives in shared/, linked by the
// server). The client uses render_bindings instead.
#pragma once

#include <utility>
#include <vector>

#include "core/ecs.hpp"
#include "shared/mod/bindings_table.hpp"
#include "shared/mod/component_ref.hpp"
#include "shared/mod/lua_host.hpp"
#include "shared/mod/registry.hpp"

namespace shared {
class World;
}

namespace mod {

// A lightweight, copyable handle passed into Lua callbacks: it references the
// authoritative registry so field writes go straight through to components.
struct EntityHandle
{
    core::Registry* reg = nullptr;
    core::Entity entity = core::null_entity;
};

// Register component usertypes, the EntityHandle usertype, the `world` query
// facade, and spawn_* globals into the host's VM, bound to `world_reg` (the
// authoritative registry). Call once, before load_dir().
void install_sim_bindings(LuaHost& host, core::Registry& world_reg);

// Register just the kernel-component usertypes + their dispatch entries into
// `lua`, filling `table` in engine_component_names order. Shared by the sim VM
// (via install_sim_bindings) and the client render VM, so a HUD hook resolves
// kernel handles (`view:get(Hearts)`) through the SAME schema — no hardcoded
// field-name copy on the client. Call once per sol::state.
void register_engine_components(sol::state& lua, BindingTable& table);

// Create an enemy from a registered def: kernel comps from the factory, then
// the archetype's (possibly wave-resolved) component inits, then the optional
// on_spawn hook (protected; errors logged). Used by the wave spawner (cached
// per-wave inits) and the Lua spawn_enemy global (ad-hoc resolution).
core::Entity spawn_enemy(core::Registry& reg, float x, float y, const EnemyDef& def,
                         const std::vector<std::pair<const ComponentRef*, sol::table>>& inits,
                         const BindingTable& bindings);

// After load_dir(): add every Lua-defined system to the World's pipeline (at its
// phase order, honoring an optional rate throttle). Server-only.
void install_script_systems(LuaHost& host, shared::World& world);

// Dispatch a def's availability predicate (default true if none). Never throws.
[[nodiscard]] bool run_available(const ContentDef& def, EntityHandle handle);

// Dispatch a selection: stat upgrades -> apply(entity, rarity, amount); objects
// -> acquire(entity). Callback errors are logged, never fatal.
void run_apply(const ContentDef& def, EntityHandle handle, Rarity rarity);

// Roll an offer through the mods' hook (mod:level_offer): validates
// ids/rarities against the registry and clamps to max_level_up_choices.
// `context` is forwarded as the hook's 3rd arg: "level" for XP level-ups,
// "chest" for boss-chest rounds (the mod filters the pool by it).
// Empty result = no hook, hook error, or nothing valid -> caller falls back
// to the engine's built-in roll.
[[nodiscard]] std::vector<proto::LevelUpChoice> run_level_offer(LuaHost& host, core::Registry& reg,
                                                                core::Entity player, int level,
                                                                const char* context = "level");

} // namespace mod
