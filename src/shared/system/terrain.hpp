#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/map/terrain.hpp"

// Terrain collision: hard pushout of players and enemies against the seeded
// obstacle field (shared/map/terrain.hpp) — right after Separation, so crowd
// pressure can't shove anyone through a rock. Bullets/orbs are untouched
// (no tags): shots fly over obstacles, drops can't get stuck in them.
// Enemies steering into a rock slide around it (radial pushout + targeting's
// velocity keeps pointing at the player).
class TerrainSystem
{
public:
    void operator()(core::Registry& registry, float /*dt*/)
    {
        const Terrain* terrain = nullptr;
        registry.view<Terrain>().each(
          [&](core::Entity, const Terrain& t) { terrain = &t; });
        if (terrain == nullptr || terrain->seed == 0) { return; }

        const auto resolve = [&](Position& pos, const Radius& radius) {
            shared::map::resolve_terrain(cache_, terrain->seed, pos.x, pos.y, radius.value,
                                         terrain->clear_x, terrain->clear_y, terrain->clear_r);
        };
        registry.view<PlayerTag, Position, Radius>().each(
          [&](core::Entity, const PlayerTag&, Position& pos, const Radius& radius) {
              resolve(pos, radius);
          });
        registry.view<EnemyTag, Position, Radius>().each(
          [&](core::Entity, const EnemyTag&, Position& pos, const Radius& radius) {
              resolve(pos, radius);
          });
    }

private:
    shared::map::ChunkCache cache_; // per-chunk generation runs once, not per entity
};
