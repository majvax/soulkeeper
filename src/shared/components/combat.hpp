#pragma once
#include <cstdint>

// KERNEL combat components — the engine contract. Everything gameplay-defining
// (weapons, crit, contact damage, bullets, i-frames, pickups) is Lua-defined in
// mods/core; only what netcode, prediction, snapshots and the collision service
// depend on lives here.

// Float hit points (enemies; the snapshot health byte = current/max fraction).
struct Health
{
    float current, max;
};

// Player life in discrete hearts (feeds the player snapshot bytes + HUD).
struct Hearts
{
    std::int16_t current, max;
};

// Collision radius in pixels (the spatial/nearby service works center-to-
// center; callers add radii themselves).
struct Radius
{
    float value;
};

// Marks an enemy entity (broad-phase grouping + snapshots + death events).
struct EnemyTag
{
};

// How the client draws this entity: kind = proto::EntityKind, variant = a
// per-kind byte (enemies: archetype wire id; bullets: 1 hostile / 2 crit).
// Stamped by the kernel factories; Lua mutates `variant` freely.
struct Render
{
    std::uint8_t kind, variant;
};

// On-screen size multiplier, shipped in snapshots (like Render, the engine
// only transports it — Lua owns the rules, e.g. Vitality growing the player).
// Absent == 1.0; enemies multiply it onto their archetype's static scale.
struct Scale
{
    float value = 1.0f;
};

// XP dropped on death (copied from the enemy def at spawn; read by the Lua
// death system and the on_enemy_death event payload).
struct XpReward
{
    std::uint32_t value;
};

// The player's current aim (normalized direction) + whether the trigger is held.
// Set by the server from the client's Input packet.
struct AimState
{
    float dx, dy;
    std::uint8_t firing;
};

// Player dash: a short burst of DASH_MULT x speed (see system/input.hpp for
// the shared constants + tick — client prediction runs the same math).
// Charges refill one at a time on a cooldown. `shockwave` (set by the
// Shockwave Dash object) is read by the Lua shockwave system.
struct Dash
{
    float cooldown_max;
    float cooldown;        // time until the next charge (while charges < max)
    float burst_remaining; // > 0 while dashing
    float dir_x, dir_y;    // normalized burst direction
    float shockwave;       // damage to enemies passed through (0 = off)
    std::uint8_t charges, max_charges;
};
