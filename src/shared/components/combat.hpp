#pragma once
#include <cstdint>

// Combat-related plain-old-data components.

struct Health
{
    float current, max;
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

// A player's damage aura: enemies within `radius` take `per_second` damage.
// Not assigned by default — a future upgrade.
struct Aura
{
    float radius;
    float per_second;
};

// Marks an enemy entity.
struct EnemyTag
{
};

// Enemy archetypes (kept here so the client can read the snapshot variant).
enum class EnemyType : std::uint8_t { Bandit = 0, Scout = 1, Brute = 2 };

// The archetype id carried by an enemy entity (for snapshots + XP lookup).
struct Archetype
{
    std::uint8_t id;
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

// Time (seconds) an entity has left before it despawns.
struct Lifetime
{
    float remaining;
};
