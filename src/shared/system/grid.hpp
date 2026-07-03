#pragma once
#include "core/ecs.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"

// Rebuild the shared spatial hash from current positions, first thing every
// tick. Everything with a Position goes in; consumers (Lua world:nearby)
// filter by component membership afterwards.
class GridSystem
{
public:
    void operator()(core::Registry& registry, float /*dt*/)
    {
        registry.view<WorldGrid>().each([&](core::Entity, WorldGrid& world_grid) {
            world_grid.grid.clear();
            registry.view<Position>().each([&](core::Entity entity, const Position& pos) {
                world_grid.grid.insert(entity, pos.x, pos.y);
            });
        });
    }
};
