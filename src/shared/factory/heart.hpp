#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"

// Spawn a healing heart dropped by a dead enemy. Restores one heart to the
// first hurt player who walks near it.
inline core::Entity create_heart(core::Registry& registry, float x, float y)
{
    const core::Entity heart = registry.create();
    registry.assign(heart, Position{ .x = x, .y = y });
    registry.assign(heart, PrevPosition{ .x = x, .y = y });
    registry.assign(heart, HeartPickup{});
    registry.assign(heart, Radius{ .value = 9 });
    return heart;
}
