// src/shared/mod/bindings_table.hpp
//
// The engine-component dispatch table behind ComponentRef. One entry per
// kernel component type (type-erased has/get/set/remove/pool), built by
// install_sim_bindings from the engine_components list below — its order
// defines the engine tags, and lua_host installs the same-named prelude
// ComponentRef globals in BOTH VMs from the same list (the render VM has the
// handles but no table; it only stores them, never dispatches).
#pragma once

#include <array>
#include <functional>
#include <meta>
#include <vector>

#include <sol/sol.hpp>

#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/components/progression.hpp"

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

// THE canonical kernel-component list: index = engine tag = prelude handle.
// register_engine_components reflects over it (fields, defaults and Lua
// accessors all come from the struct itself), so adding a kernel component is
// exactly: write the struct, append one entry here. Names are explicit because
// the prelude name can differ from the type ("Enemy" vs EnemyTag).
// A consteval function, not a variable: std::meta::info is consteval-only, so
// the list may only ever exist inside constant evaluation.
struct EngineComponent
{
    std::meta::info type;
    const char* name;
};

consteval auto engine_components()
{
    return std::array{
        EngineComponent{ ^^Position, "Position" }, EngineComponent{ ^^Velocity, "Velocity" },
        EngineComponent{ ^^Speed, "Speed" },       EngineComponent{ ^^Health, "Health" },
        EngineComponent{ ^^Hearts, "Hearts" },     EngineComponent{ ^^Radius, "Radius" },
        EngineComponent{ ^^AimState, "AimState" }, EngineComponent{ ^^Dash, "Dash" },
        EngineComponent{ ^^XpReward, "XpReward" }, EngineComponent{ ^^Render, "Render" },
        EngineComponent{ ^^Downed, "Downed" },     EngineComponent{ ^^EnemyTag, "Enemy" },
        EngineComponent{ ^^PlayerTag, "Player" },  EngineComponent{ ^^Scale, "Scale" },
        EngineComponent{ ^^WaveHold, "WaveHold" },
    };
}

// Prelude globals, in engine-tag order — generated, so it can't drift.
inline constexpr auto engine_component_names = [] consteval {
    std::array<const char*, engine_components().size()> names{};
    for (std::size_t i = 0; i < names.size(); ++i) { names[i] = engine_components()[i].name; }
    return names;
}();

} // namespace mod
