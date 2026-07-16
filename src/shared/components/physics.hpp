#pragma once

struct Position
{
    float x = 0.0f, y = 0.0f;
}; // simulation state at the current tick
struct PrevPosition
{
    float x = 0.0f, y = 0.0f;
}; // state at the previous tick (for render lerp)
struct Velocity
{
    float dx = 0.0f, dy = 0.0f;
}; // pixels per second

struct Speed
{
    float value = 0.0f;
}; // movement speed magnitude in pixels/second

// Airborne mover: the terrain pass skips it entirely — flies over water AND
// obstacle colliders (like bullets already do). An enemy def opts in with
// `flying = true`; the client draws nothing special, the sprite carries it.
struct Flying
{
};
