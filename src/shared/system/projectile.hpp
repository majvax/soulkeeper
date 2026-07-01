#pragma once
#include "core/ecs.hpp"
#include "core/spatial.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"
#include <cmath>
#include <vector>

// Advance bullets: expire them by lifetime, and on overlap with an enemy deal
// their damage and despawn (no pierce). Enemies are bucketed into a spatial grid
// so each bullet only checks nearby ones.
class ProjectileSystem
{
public:
    void operator()(core::Registry& registry, float dt)
    {
        core::SpatialGrid grid{ cell_size };
        registry.view<EnemyTag, Position>().each(
          [&](core::Entity enemy, const EnemyTag&, const Position& pos) { grid.insert(enemy, pos.x, pos.y); });

        std::vector<core::Entity> dead_bullets;

        registry.view<Projectile, Position, Radius, Lifetime>().each(
          [&](core::Entity bullet, const Projectile& proj, const Position& bpos, const Radius& brad, Lifetime& life) {
              life.remaining -= dt;
              if (life.remaining <= 0.0f) {
                  dead_bullets.push_back(bullet);
                  return;
              }

              const float reach = brad.value + cell_size;
              for (const core::Entity enemy :
                   grid.query(bpos.x - reach, bpos.y - reach, bpos.x + reach, bpos.y + reach)) {
                  const Position* epos = registry.try_get<Position>(enemy);
                  const Radius* erad = registry.try_get<Radius>(enemy);
                  Health* ehp = registry.try_get<Health>(enemy);
                  if (!epos || !erad || !ehp) { continue; }

                  const float dx = epos->x - bpos.x;
                  const float dy = epos->y - bpos.y;
                  if ((dx * dx) + (dy * dy) < (brad.value + erad->value) * (brad.value + erad->value)) {
                      ehp->current -= proj.damage;
                      dead_bullets.push_back(bullet);
                      break; // one hit, bullet is consumed
                  }
              }
          });

        for (const core::Entity bullet : dead_bullets) { registry.destroy(bullet); }
    }

private:
    static constexpr float cell_size = 128.0f;
};
