#pragma once
#include <cstdint>

// KERNEL combat components — the engine contract. Everything gameplay-defining
// (weapons, crit, contact damage, bullets, i-frames, pickups) is Lua-defined in
// mods/core; only what netcode, prediction, snapshots and the collision service
// depend on lives here.
//
// Default member initializers are LOAD-BEARING: the reflective Lua bindings
// (sim_bindings.cpp) use T{} as the defaults for `e:set(H, {...})`, so a
// component's defaults live here and nowhere else.

// Float hit points (enemies; the snapshot health byte = current/max fraction).
struct Health
{
    float current = 0.0f, max = 0.0f;
};

// Player life in discrete hearts (feeds the player snapshot bytes + HUD).
struct Hearts
{
    std::int16_t current = 3, max = 3;
};

// Collision radius in pixels (the spatial/nearby service works center-to-
// center; callers add radii themselves).
struct Radius
{
    float value = 0.0f;
};

// Marks an enemy entity (broad-phase grouping + snapshots + death events).
struct EnemyTag
{
};

// How the client draws this entity: kind = proto::EntityKind, variant = a
// per-kind byte (enemies: archetype wire id; bullets: 1 hostile / 2 crit).
// Stamped by the kernel factories; Lua mutates `variant` freely. `fx` is the
// Lua-driven animation state (0 = auto Idle/Move; 1 = attacking — the client
// plays the sprite pack's ATK/Attack clip once per 0->1 transition).
struct Render
{
    std::uint8_t kind = 0, variant = 0, fx = 0;
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
    std::uint32_t value = 1;
};

// The player's current aim (normalized direction) + whether the trigger is held.
// Set by the server from the client's Input packet.
struct AimState
{
    float dx = 0.0f, dy = 0.0f;
    std::uint8_t firing = 0;
};

// Dash tuning: a burst of DASH_MULT x speed for DASH_DURATION seconds
// (~155 px at base speed), one charge, DASH_COOLDOWN s to refill. Shared so
// client prediction runs the exact same math (see system/input.hpp).
inline constexpr float DASH_MULT = 4.0f;
inline constexpr float DASH_DURATION = 0.16f;
inline constexpr float DASH_COOLDOWN = 4.0f;

// Player dash: a short burst of DASH_MULT x speed (see system/input.hpp for
// the shared tick — client prediction runs the same math).
// Charges refill one at a time on a cooldown. `shockwave` (set by the
// Shockwave Dash object) is read by the Lua shockwave system.
struct Dash
{
    float cooldown_max = DASH_COOLDOWN;
    float cooldown = 0.0f;        // time until the next charge (while charges < max)
    float burst_remaining = 0.0f; // > 0 while dashing
    float dir_x = 1.0f, dir_y = 0.0f; // normalized burst direction
    float shockwave = 0.0f;           // damage to enemies passed through (0 = off)
    std::uint8_t charges = 1, max_charges = 1;
};
