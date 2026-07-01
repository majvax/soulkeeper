#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"
#include <cstdint>

// Per-archetype stats. Data-driven so adding an enemy type is a table edit.
struct EnemyStats
{
    float health;
    float speed;
    float damage;
    float radius;
    std::uint32_t xp; // dropped as an orb on death
};

inline EnemyStats enemy_stats(EnemyType type)
{
    switch (type) {
    case EnemyType::Bandit: return { .health = 20, .speed = 120, .damage = 20, .radius = 10, .xp = 1 };
    case EnemyType::Scout:  return { .health = 10, .speed = 200, .damage = 15, .radius = 8, .xp = 1 };
    case EnemyType::Brute:  return { .health = 60, .speed = 70, .damage = 35, .radius = 16, .xp = 3 };
    }
    return { .health = 20, .speed = 120, .damage = 20, .radius = 10, .xp = 1 };
}

// Spawn an enemy of the given archetype. Enemies chase the nearest player and
// deal contact damage.
inline core::Entity create_enemy(core::Registry& registry, float x, float y, EnemyType type = EnemyType::Bandit)
{
    const EnemyStats stats = enemy_stats(type);
    const core::Entity enemy = registry.create();
    registry.assign(enemy, Position{ .x = x, .y = y });
    registry.assign(enemy, PrevPosition{ .x = x, .y = y });
    registry.assign(enemy, Velocity{ .dx = 0, .dy = 0 });
    registry.assign(enemy, Speed{ .value = stats.speed });
    registry.assign(enemy, EnemyTag{});
    registry.assign(enemy, Archetype{ .id = static_cast<std::uint8_t>(type) });
    registry.assign(enemy, Health{ .current = stats.health, .max = stats.health });
    registry.assign(enemy, Radius{ .value = stats.radius });
    registry.assign(enemy, Damage{ .per_second = stats.damage });
    return enemy;
}
