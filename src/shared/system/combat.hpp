#pragma once
#include "core/ecs.hpp"
#include "core/spatial.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/components/progression.hpp"
#include <cmath>

// Resolve player <-> enemy contact each tick: an enemy damages the player it
// physically overlaps. Enemies go into a spatial grid so each player only checks
// nearby ones. (Area effects like the damage aura are Lua script systems now.)
class CombatSystem
{
public:
    void operator()(core::Registry& registry, float dt)
    {
        core::SpatialGrid grid{ cell_size };
        registry.view<EnemyTag, Position>().each(
          [&](core::Entity enemy, const EnemyTag&, const Position& pos) { grid.insert(enemy, pos.x, pos.y); });

        registry.view<PlayerTag, Position, Health, Radius>().each(
          [&](core::Entity player, const PlayerTag&, const Position& ppos, Health& php, const Radius& prad) {
              if (registry.try_get<Downed>(player) != nullptr) { return; } // downed = out of combat
              const float reach = prad.value + cell_size;
              for (const core::Entity enemy :
                   grid.query(ppos.x - reach, ppos.y - reach, ppos.x + reach, ppos.y + reach)) {
                  const Position* epos = registry.try_get<Position>(enemy);
                  const Radius* erad = registry.try_get<Radius>(enemy);
                  const Damage* edmg = registry.try_get<Damage>(enemy);
                  if (!epos || !erad || !edmg) { continue; }

                  const float dx = epos->x - ppos.x;
                  const float dy = epos->y - ppos.y;
                  const float dist = std::sqrt((dx * dx) + (dy * dy));
                  if (dist < prad.value + erad->value) { php.current -= edmg->per_second * dt; }
              }
          });
    }

private:
    static constexpr float cell_size = 128.0f;
};
