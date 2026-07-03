#pragma once
#include "core/ecs.hpp"
#include "core/spatial.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/components/progression.hpp"
#include <cmath>
#include <vector>

// Advance bullets: expire them by lifetime, and on overlap deal their damage
// and despawn (no pierce). Friendly bullets hit enemies (bucketed into a
// spatial grid); Hostile (enemy-fired) bullets hit players — few enough (<=4)
// to check directly.
class ProjectileSystem
{
public:
    void operator()(core::Registry& registry, float dt)
    {
        core::SpatialGrid grid{ cell_size };
        registry.view<EnemyTag, Position>().each(
          [&](core::Entity enemy, const EnemyTag&, const Position& pos) { grid.insert(enemy, pos.x, pos.y); });

        struct PlayerTarget
        {
            core::Entity entity;
            float x, y, radius;
        };
        std::vector<PlayerTarget> players;
        registry.view<PlayerTag, Position, Radius>().each(
          [&](core::Entity player, const PlayerTag&, const Position& pos, const Radius& rad) {
              // Downed players are out of combat; invulnerable ones let bullets pass.
              if (registry.try_get<Downed>(player) == nullptr
                  && registry.try_get<Invulnerable>(player) == nullptr) {
                  players.push_back({ .entity = player, .x = pos.x, .y = pos.y, .radius = rad.value });
              }
          });

        std::vector<core::Entity> dead_bullets;

        registry.view<Projectile, Position, Radius, Lifetime>().each(
          [&](core::Entity bullet, const Projectile& proj, const Position& bpos, const Radius& brad, Lifetime& life) {
              life.remaining -= dt;
              if (life.remaining <= 0.0f) {
                  dead_bullets.push_back(bullet);
                  return;
              }

              if (registry.has<Hostile>(bullet)) {
                  for (const PlayerTarget& target : players) {
                      const float dx = target.x - bpos.x;
                      const float dy = target.y - bpos.y;
                      const float hit = brad.value + target.radius;
                      if ((dx * dx) + (dy * dy) < hit * hit) {
                          if (Hearts* hearts = registry.try_get<Hearts>(target.entity)) {
                              hearts->current -= static_cast<std::int16_t>(std::max(1.0f, proj.damage));
                              registry.assign(target.entity, Invulnerable{ .remaining = HIT_IFRAMES });
                          }
                          dead_bullets.push_back(bullet);
                          break; // one hit, bullet is consumed
                      }
                  }
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
