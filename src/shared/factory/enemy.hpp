#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"
#include <cstdint>

// Per-archetype stats. Archetypes are Lua-defined (mod:add_enemy — see
// mods/core/enemies.lua); this is the plain-data slice the factory needs.
struct EnemyStats
{
    float health;
    float speed;
    float damage;
    float radius;
    std::uint32_t xp; // dropped as an orb on death
};

// Spawn an enemy of the given archetype (variant = its enemy wire id). Enemies
// chase the nearest player and deal contact damage.
inline core::Entity create_enemy(core::Registry& registry, float x, float y, const EnemyStats& stats,
                                 std::uint8_t variant)
{
    const core::Entity enemy = registry.create();
    registry.assign(enemy, Position{ .x = x, .y = y });
    registry.assign(enemy, PrevPosition{ .x = x, .y = y });
    registry.assign(enemy, Velocity{ .dx = 0, .dy = 0 });
    registry.assign(enemy, Speed{ .value = stats.speed });
    registry.assign(enemy, EnemyTag{});
    registry.assign(enemy, Archetype{ .id = variant });
    registry.assign(enemy, XpReward{ .value = stats.xp });
    registry.assign(enemy, Health{ .current = stats.health, .max = stats.health });
    registry.assign(enemy, Radius{ .value = stats.radius });
    registry.assign(enemy, Damage{ .per_second = stats.damage });
    return enemy;
}
