#pragma once

struct Position
{
    float x, y;
}; // simulation state at the current tick
struct PrevPosition
{
    float x, y;
}; // state at the previous tick (for render lerp)
struct Velocity
{
    float dx, dy;
}; // pixels per second

struct Speed
{
    float value;
}; // movement speed magnitude in pixels/second
