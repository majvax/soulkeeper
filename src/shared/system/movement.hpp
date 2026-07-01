#pragma once
#include "core/ecs.hpp"
#include "shared/components/physics.hpp"

// Integrate velocity into position. PrevPosition records the pre-step position
// (used by the client to interpolate). The world is free-scrolling — no bounds.
class MovementSystem
{
public:
    void operator()(core::Registry& registry, float dt)
    {
        registry.view<Position, PrevPosition, Velocity>().each(
          [&](core::Entity, Position& pos, PrevPosition& prev, const Velocity& vel) {
              prev = { .x = pos.x, .y = pos.y };
              pos.x += vel.dx * dt;
              pos.y += vel.dy * dt;
          });
    }
};
