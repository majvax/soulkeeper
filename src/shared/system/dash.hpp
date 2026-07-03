#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/system/input.hpp"
#include <cmath>

// Ticks player dash state (charge refill + burst velocity override) just
// before Movement. Pure movement — prediction runs the same tick_dash on the
// client. Gameplay effects of dashing (the Shockwave object) are a Lua system.
class DashSystem
{
public:
    void operator()(core::Registry& registry, float dt)
    {
        registry.view<PlayerTag, Dash, Velocity, Speed>().each(
          [&](core::Entity, const PlayerTag&, Dash& dash, Velocity& vel, const Speed& speed) {
              tick_dash(dash, vel, speed.value, dt);
          });
    }
};
