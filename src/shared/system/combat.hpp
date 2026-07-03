#pragma once
#include "core/ecs.hpp"
#include "core/spatial.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/components/progression.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

// Resolve player <-> enemy contact each tick. Player life is discrete hearts:
// a touching enemy removes Damage.per_second hearts (the field holds
// hearts-per-hit for enemies) and grants HIT_IFRAMES of invulnerability, so
// contact can't melt a player tick-by-tick. Enemies go into a spatial grid so
// each player only checks nearby ones. (Area effects like the damage aura are
// Lua script systems now.)
class CombatSystem
{
public:
    void operator()(core::Registry& registry, float dt)
    {
        // Tick down i-frames (players only; cheap pool).
        std::vector<core::Entity> shielded_done;
        registry.view<Invulnerable>().each([&](core::Entity entity, Invulnerable& inv) {
            inv.remaining -= dt;
            if (inv.remaining <= 0.0f) { shielded_done.push_back(entity); }
        });
        for (const core::Entity entity : shielded_done) { registry.remove<Invulnerable>(entity); }

        core::SpatialGrid grid{ cell_size };
        registry.view<EnemyTag, Position>().each(
          [&](core::Entity enemy, const EnemyTag&, const Position& pos) { grid.insert(enemy, pos.x, pos.y); });

        registry.view<PlayerTag, Position, Hearts, Radius>().each(
          [&](core::Entity player, const PlayerTag&, const Position& ppos, Hearts& hearts, const Radius& prad) {
              if (registry.try_get<Downed>(player) != nullptr) { return; } // downed = out of combat
              if (registry.try_get<Invulnerable>(player) != nullptr) { return; } // still flashing
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
                  if (dist < prad.value + erad->value) {
                      hearts.current -= static_cast<std::int16_t>(std::max(1.0f, edmg->per_second));
                      registry.assign(player, Invulnerable{ .remaining = HIT_IFRAMES });
                      break; // one hit per window; i-frames cover the rest
                  }
              }
          });
    }

private:
    static constexpr float cell_size = 128.0f;
};
