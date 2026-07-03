#pragma once
#include "shared/components/gameplay.hpp"
#include "shared/components/progression.hpp"
#include "shared/sim/world.hpp"
#include "shared/system/dash.hpp"
#include "shared/system/grid.hpp"
#include "shared/system/movement.hpp"

namespace shared {

// Single source of truth for the simulation: the KERNEL pipeline (spatial
// grid, dash movement, integration) plus the shared singletons. All gameplay
// systems — targeting, shooting, bullets, combat, death, pickups — are
// Lua-defined in mods/core and installed after these (install_script_systems).
inline World make_game_world()
{
    World world;

    const core::Entity stats = world.registry().create();
    world.registry().assign(stats, GameStats{ .xp = 0, .wave = 1 });
    world.registry().assign(stats, WorldGrid{});

    world.add_system(phase::Grid, GridSystem{});         // rebuild the spatial hash
    world.add_system(phase::Motion, DashSystem{});       // dash burst/recharge (prediction-coupled)
    world.add_system(phase::Movement, MovementSystem{}); // integrate velocity
    return world;
}

} // namespace shared
