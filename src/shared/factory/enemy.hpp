#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"
#include "shared/protocol.hpp"
#include <cstdint>

// Spawn the KERNEL part of an enemy (variant = its enemy wire id): position,
// motion, tags, render identity. Everything gameplay-defining (Health, Speed,
// Touch damage, ...) is declared by the archetype's :component() bag and
// applied by mod::spawn_enemy.
inline core::Entity create_enemy(core::Registry& registry, float x, float y, std::uint8_t variant)
{
    const core::Entity enemy = registry.create();
    registry.assign(enemy, Position{ .x = x, .y = y });
    registry.assign(enemy, PrevPosition{ .x = x, .y = y });
    registry.assign(enemy, Velocity{ .dx = 0, .dy = 0 });
    registry.assign(enemy, EnemyTag{});
    registry.assign(enemy, Render{ .kind = static_cast<std::uint8_t>(proto::EntityKind::Enemy),
                                   .variant = variant });
    return enemy;
}
