#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

// Anti-cramming: enemies steering at the same player collapse into one stacked
// blob (a swarm reading as a single enemy on a single hitbox). Right after
// Movement, nudge overlapping enemy PAIRS apart — soft crowd pressure, not
// rigid-body popping. Enemy-enemy only: walking INTO players is contact
// damage's job.
//
// Anchors: an enemy with Speed <= 0 is never pushed, and that rule rides the
// existing mechanics for free — mines/brambles (planted), sleeping Mimics
// (props), the Game Master (never walks), and any boss mid-telegraph or
// mid-lunge (lunge_sys/brain_sys PARK Speed during windups and dashes) hold
// their ground, so choreography and locked dash lines can't be shoved. Anchors
// still push others away (they show up as neighbors).
class SeparationSystem
{
public:
    void operator()(core::Registry& registry, float dt)
    {
        const core::SpatialGrid* grid = nullptr;
        registry.view<WorldGrid>().each(
          [&](core::Entity, const WorldGrid& world_grid) { grid = &world_grid.grid; });
        if (grid == nullptr) { return; }

        // Hitboxes are far smaller than the art (10 px bandits under 48 px
        // sprites): separate to a multiple so bodies brush without merging.
        constexpr float spacing = 1.6f;
        constexpr float resolve_rate = 8.0f;  // fraction of overlap closed per second
        constexpr float max_push = 140.0f;    // px/s displacement cap (soft, never a pop)
        constexpr float query_pad = 48.0f;    // fattest boss radius: catches every pair once
        const float soft = std::min(1.0f, resolve_rate * dt);

        registry.view<EnemyTag, Position, Radius, Speed>().each(
          [&](core::Entity self, const EnemyTag&, Position& pos, const Radius& radius,
              const Speed& speed) {
              if (speed.value <= 0.0f) { return; } // anchored: planted/telegraphing/parked
              const float self_r = radius.value;
              neighbors_.clear();
              grid->query(pos.x - self_r - query_pad, pos.y - self_r - query_pad,
                          pos.x + self_r + query_pad, pos.y + self_r + query_pad, neighbors_);

              float push_x = 0.0f;
              float push_y = 0.0f;
              for (const core::Entity other : neighbors_) {
                  if (other == self || !registry.has<EnemyTag>(other)) { continue; }
                  const Position* opos = registry.try_get<Position>(other);
                  const Radius* orad = registry.try_get<Radius>(other);
                  if (opos == nullptr || orad == nullptr) { continue; }
                  const float apart = (self_r + orad->value) * spacing;
                  float dx = pos.x - opos->x;
                  float dy = pos.y - opos->y;
                  const float d2 = (dx * dx) + (dy * dy);
                  if (d2 >= apart * apart) { continue; }
                  float dist = std::sqrt(d2);
                  if (dist < 0.01f) {
                      // Exact stack (summons on one point): fan out on a
                      // deterministic golden-angle spiral keyed by the id.
                      const float a = static_cast<float>(self) * 2.399963f;
                      dx = std::cos(a);
                      dy = std::sin(a);
                      dist = 1.0f;
                  } else {
                      dx /= dist;
                      dy /= dist;
                  }
                  // Mass by hitbox area: trash can't bulldoze a boss, a boss
                  // plows through trash. Each mover resolves only its own share.
                  const float weight = (orad->value * orad->value)
                                     / ((self_r * self_r) + (orad->value * orad->value));
                  const float overlap = apart - dist;
                  push_x += dx * overlap * weight;
                  push_y += dy * overlap * weight;
              }

              const float mag = std::sqrt((push_x * push_x) + (push_y * push_y));
              if (mag <= 0.0f) { return; }
              const float step = std::min(mag * soft, max_push * dt);
              pos.x += (push_x / mag) * step;
              pos.y += (push_y / mag) * step;
          });
    }

private:
    std::vector<core::Entity> neighbors_; // reused query buffer (no per-call allocs)
};
