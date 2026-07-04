// src/shared/mod/bindings_table.hpp
//
// The engine-component dispatch table behind ComponentRef. One entry per
// kernel component type (type-erased has/get/set/remove/pool), built by
// install_sim_bindings in the FIXED order of engine_component_names below —
// that order defines the engine tags, and lua_host installs the same-named
// prelude ComponentRef globals in BOTH VMs from the same list (the render VM
// has the handles but no table; it only stores them, never dispatches).
#pragma once

#include <array>
#include <functional>
#include <vector>

#include <sol/sol.hpp>

#include "core/ecs.hpp"

namespace mod {

struct ComponentBinding
{
    std::function<bool(core::Registry&, core::Entity)> has;
    std::function<sol::object(sol::state_view, core::Registry&, core::Entity)> get;
    std::function<void(core::Registry&, core::Entity, const sol::table&)> assign;
    std::function<void(core::Registry&, core::Entity)> remove;
    std::function<core::SparseSet*(core::Registry&)> pool; // for world:each membership
};

using BindingTable = std::vector<ComponentBinding>;

// Prelude globals, in engine-tag order. install_sim_bindings must register its
// BindingTable entries in exactly this order (asserted there).
inline constexpr std::array<const char*, 14> engine_component_names = {
    "Position", "Velocity", "Speed", "Health", "Hearts", "Radius", "AimState",
    "Dash", "XpReward", "Render", "Downed", "Enemy", "Player", "Scale",
};

} // namespace mod
