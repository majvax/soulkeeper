#pragma once
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"
#include <cmath>
#include <cstdint>

// Player movement speed in pixels/second. Shared so the server is the single
// authority on how fast a player moves.
inline constexpr float PLAYER_SPEED = 240.0f;

// (DASH_MULT / DASH_DURATION / DASH_COOLDOWN live next to the Dash component
// in components/combat.hpp — its default member initializers use them.)

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

// Try to start a dash toward (dx, dy). Consumes a charge; false when out of
// charges, already bursting, or the direction is zero.
inline bool start_dash(Dash& dash, float dx, float dy)
{
    if (dash.charges == 0 || dash.burst_remaining > 0.0f) { return false; }
    const float len = std::sqrt((dx * dx) + (dy * dy));
    if (len <= 0.0f) { return false; }
    if (dash.charges == dash.max_charges) { dash.cooldown = dash.cooldown_max; } // start refilling
    --dash.charges;
    dash.dir_x = dx / len;
    dash.dir_y = dy / len;
    dash.burst_remaining = DASH_DURATION;
    return true;
}

// Advance dash state one tick: refill charges on cooldown, and while bursting
// override the velocity with the stored direction at DASH_MULT x speed. Runs
// identically in the server DashSystem and the client's local prediction.
inline void tick_dash(Dash& dash, Velocity& vel, float speed, float dt)
{
    if (dash.charges < dash.max_charges) {
        dash.cooldown -= dt;
        if (dash.cooldown <= 0.0f) {
            ++dash.charges;
            dash.cooldown = dash.cooldown_max;
        }
    }
    if (dash.burst_remaining > 0.0f) {
        dash.burst_remaining -= dt;
        vel.dx = dash.dir_x * speed * DASH_MULT;
        vel.dy = dash.dir_y * speed * DASH_MULT;
    }
}
