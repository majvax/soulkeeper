#pragma once
#include "shared/components/physics.hpp"
#include <cmath>
#include <cstdint>

// Player movement speed in pixels/second. Shared so the server is the single
// authority on how fast a player moves.
inline constexpr float PLAYER_SPEED = 240.0f;

// Turn a movement intent (-1/0/+1 on each axis) into a velocity at `speed`.
// Diagonals are normalized so they aren't faster than cardinal movement.
inline void apply_input(Velocity& vel, std::int8_t move_x, std::int8_t move_y, float speed = PLAYER_SPEED)
{
    float x = static_cast<float>(move_x);
    float y = static_cast<float>(move_y);
    const float len = std::sqrt(x * x + y * y);
    if (len > 0.0f) {
        x /= len;
        y /= len;
    }
    vel.dx = x * speed;
    vel.dy = y * speed;
}
