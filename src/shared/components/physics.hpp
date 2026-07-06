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
