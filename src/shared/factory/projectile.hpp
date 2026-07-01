#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"

// Spawn a bullet at (x,y) moving at (vx,vy). Despawns on hit or after `lifetime`.
inline core::Entity create_projectile(core::Registry& registry, float x, float y, float vx, float vy,
                                      float damage, float lifetime)
{
    const core::Entity bullet = registry.create();
    registry.assign(bullet, Position{ .x = x, .y = y });
    registry.assign(bullet, PrevPosition{ .x = x, .y = y });
    registry.assign(bullet, Velocity{ .dx = vx, .dy = vy });
    registry.assign(bullet, Projectile{ .damage = damage });
    registry.assign(bullet, Lifetime{ .remaining = lifetime });
    registry.assign(bullet, Radius{ .value = 4 });
    return bullet;
}
