#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"
#include "shared/components/progression.hpp"
#include <cstdint>

// Spawn an experience orb dropped by a dead enemy. Collected by walking near it.
inline core::Entity create_xp_orb(core::Registry& registry, float x, float y, std::uint32_t value)
{
    const core::Entity orb = registry.create();
    registry.assign(orb, Position{ .x = x, .y = y });
    registry.assign(orb, PrevPosition{ .x = x, .y = y });
    registry.assign(orb, XpOrb{ .value = value });
    registry.assign(orb, Radius{ .value = 8 });
    return orb;
}
