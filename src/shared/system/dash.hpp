#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/system/input.hpp"
#include <cmath>

// Ticks player dash state (charge refill + burst velocity override) just
// before Movement. With Dash.shockwave set (Shockwave Dash object), a bursting
// player damages enemies it overlaps and shoves them aside — the positional
// shove exits them from the overlap, so a burst hits each enemy once.
class DashSystem
{
public:
    void operator()(core::Registry& registry, float dt)
    {
        registry.view<PlayerTag, Dash, Velocity, Speed, Position>().each(
          [&](core::Entity, const PlayerTag&, Dash& dash, Velocity& vel, const Speed& speed,
              const Position& ppos) {
              tick_dash(dash, vel, speed.value, dt);
              if (dash.burst_remaining <= 0.0f || dash.shockwave <= 0.0f) { return; }

              registry.view<EnemyTag, Position, Health, Radius>().each(
                [&](core::Entity, const EnemyTag&, Position& epos, Health& ehp, const Radius& erad) {
                    const float dx = epos.x - ppos.x;
                    const float dy = epos.y - ppos.y;
                    const float overlap = erad.value + shock_radius;
                    const float d2 = (dx * dx) + (dy * dy);
                    if (d2 >= overlap * overlap) { return; }
                    ehp.current -= dash.shockwave;
                    const float len = std::sqrt(d2);
                    const float nx = len > 0.0f ? dx / len : 1.0f;
                    const float ny = len > 0.0f ? dy / len : 0.0f;
                    epos.x += nx * shove_distance;
                    epos.y += ny * shove_distance;
                });
          });
    }

private:
    static constexpr float shock_radius = 18.0f;   // around the dashing player
    static constexpr float shove_distance = 60.0f; // knockback (exits the overlap)
};
