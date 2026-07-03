#pragma once
#include <cstdint>

// Combat-related plain-old-data components.

struct Health
{
    float current, max;
};

// Player life in discrete hearts (enemies keep float Health). Hits remove
// whole hearts and grant a brief invulnerability window.
struct Hearts
{
    std::int16_t current, max;
};

// Post-hit invulnerability: while remaining > 0 the player ignores hits.
struct Invulnerable
{
    float remaining;
};

// Seconds of invulnerability granted by any hit on a player.
inline constexpr float HIT_IFRAMES = 1.0f;

// A dropped heal: +1 heart when a hurt player walks over it (full players
// leave it on the ground).
struct HeartPickup
{
};

struct Radius
{
    float value; // collision radius in pixels
};

// Contact damage an enemy deals to a player it overlaps, per second.
struct Damage
{
    float per_second;
};

// (Player area effects like the damage aura / slow field are now Lua-defined
// script components — see mods/core/mod.lua — not engine C++ structs.)

// Marks an enemy entity.
struct EnemyTag
{
};

// The archetype carried by an enemy entity: the enemy wire id from the mod
// EnemyRegistry (Lua-defined — see mods/core/enemies.lua). Travels in
// snapshots as `variant` so the client can look up how to draw it.
struct Archetype
{
    std::uint8_t id;
};

// XP dropped as an orb when this entity dies (copied from its EnemyDef at
// spawn, so pure systems never need registry access).
struct XpReward
{
    std::uint32_t value;
};

// A player's ranged weapon: fires a bullet every `cooldown_max` seconds while firing.
struct Weapon
{
    float cooldown_max;
    float cooldown_current;
    float bullet_speed;
    float damage;
    float projectile_lifetime;
};

// Player dash: a short burst of DASH_MULT x speed (see system/input.hpp for
// the shared constants + tick). Charges refill one at a time on a cooldown.
// `shockwave` > 0 (set by the Shockwave Dash object) makes the burst damage
// and shove enemies the player passes through.
struct Dash
{
    float cooldown_max;
    float cooldown;        // time until the next charge (while charges < max)
    float burst_remaining; // > 0 while dashing
    float dir_x, dir_y;    // normalized burst direction
    float shockwave;       // damage to enemies passed through (0 = off)
    std::uint8_t charges, max_charges;
};

// Crit stats applied to a player's weapon shots.
struct Crit
{
    float chance;     // 0..1 roll per shot
    float multiplier; // damage x on a crit
};

// Marks a bullet that rolled a crit (snapshot variant 2 -> bigger/orange).
struct CritTag
{
};

// The player's current aim (normalized direction) + whether the trigger is held.
// Set by the server from the client's Input packet.
struct AimState
{
    float dx, dy;
    std::uint8_t firing;
};

// A bullet in flight: carries the damage it deals on hit.
struct Projectile
{
    float damage;
};

// Marks a projectile as enemy-fired: it hits players instead of enemies (and
// the client draws it hostile-tinted via snapshot variant 1).
struct Hostile
{
};

// Time (seconds) an entity has left before it despawns.
struct Lifetime
{
    float remaining;
};
