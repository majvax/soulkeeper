#pragma once
#include "shared/components/progression.hpp"
#include "shared/sim/world.hpp"
#include "shared/system/combat.hpp"
#include "shared/system/dash.hpp"
#include "shared/system/death.hpp"
#include "shared/system/movement.hpp"
#include "shared/system/pickup.hpp"
#include "shared/system/projectile.hpp"
#include "shared/system/shooting.hpp"
#include "shared/system/targeting.hpp"

namespace shared {

// Single source of truth for the simulation: register the system pipeline, in
// order, and create the singleton that holds the shared XP pool. Players and
// enemies are spawned by the server at runtime.
inline World make_game_world()
{
    World world;

    const core::Entity stats = world.registry().create();
    world.registry().assign(stats, GameStats{ .xp = 0, .wave = 1 });

    world.add_system(phase::Targeting, TargetingSystem{});   // enemies aim at the nearest player
    world.add_system(phase::Motion, DashSystem{});           // dash burst/recharge (+ shockwave)
    // (phase::Motion is also where Lua slow-field systems run — see mods/core)
    world.add_system(phase::Shooting, ShootingSystem{});     // players fire bullets toward their aim
    world.add_system(phase::Movement, MovementSystem{});     // integrate velocity (incl. bullets)
    world.add_system(phase::Projectile, ProjectileSystem{}); // bullets hit enemies / expire
    world.add_system(phase::Combat, CombatSystem{});         // enemy contact damage (+ optional aura)
    world.add_system(phase::Pickup, PickupSystem{});         // players collect XP orbs -> shared pool
    world.add_system(phase::Death, DeathSystem{});           // enemies drop orbs; players go down/respawn
    return world;
}

} // namespace shared
