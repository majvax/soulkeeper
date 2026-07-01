#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/components/progression.hpp"
#include <cmath>
#include <vector>

// Steer every enemy toward the nearest player at its Speed. Players are few, so
// a brute-force nearest search per enemy is cheap.
class TargetingSystem
{
public:
    void operator()(core::Registry& registry, float /*dt*/)
    {
        std::vector<Position> players;
        registry.view<PlayerTag, Position>().each([&](core::Entity player, const PlayerTag&, const Position& pos) {
            if (registry.try_get<Downed>(player) == nullptr) { players.push_back(pos); }
        });
        if (players.empty()) { return; }

        registry.view<EnemyTag, Position, Velocity, Speed>().each(
          [&](core::Entity, const EnemyTag&, const Position& pos, Velocity& vel, const Speed& speed) {
              const Position* nearest = &players.front();
              float best = distance_sq(pos, *nearest);
              for (const Position& candidate : players) {
                  const float d = distance_sq(pos, candidate);
                  if (d < best) {
                      best = d;
                      nearest = &candidate;
                  }
              }
              float dx = nearest->x - pos.x;
              float dy = nearest->y - pos.y;
              const float len = std::sqrt((dx * dx) + (dy * dy));
              if (len > 0.0f) {
                  dx /= len;
                  dy /= len;
              }
              vel.dx = dx * speed.value;
              vel.dy = dy * speed.value;
          });
    }

private:
    static float distance_sq(const Position& a, const Position& b)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return (dx * dx) + (dy * dy);
    }
};
