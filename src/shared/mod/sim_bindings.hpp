// src/shared/mod/sim_bindings.hpp
//
// Server-side (sim VM) Lua bindings: component usertypes, an entity handle that
// mutates the ECS, spawn factories, and dispatch helpers that invoke a def's
// apply/available/acquire callbacks. SDL-free (lives in shared/, linked by the
// server). The client uses render_bindings instead.
#pragma once

#include "core/ecs.hpp"
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

// Create an enemy from a registered def and fire its optional on_spawn hook
// (protected; errors logged). Used by the wave spawner and the Lua spawn_enemy
// global so scripted spawns behave exactly like natural ones.
core::Entity spawn_enemy(core::Registry& reg, float x, float y, const EnemyDef& def);

// After load_dir(): add every Lua-defined system to the World's pipeline (at its
// phase order, honoring an optional rate throttle). Server-only.
void install_script_systems(LuaHost& host, shared::World& world);

// Dispatch a def's availability predicate (default true if none). Never throws.
[[nodiscard]] bool run_available(const ContentDef& def, EntityHandle handle);

// Dispatch a selection: stat upgrades -> apply(entity, rarity, amount); objects
// -> acquire(entity). Callback errors are logged, never fatal.
void run_apply(const ContentDef& def, EntityHandle handle, Rarity rarity);

} // namespace mod
