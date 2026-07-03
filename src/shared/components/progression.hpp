#pragma once
#include <cstdint>

// Shared world state — held by a single "world" singleton entity.
struct GameStats
{
    std::uint32_t xp;
    std::uint16_t wave;
};

// (XP orbs and heart pickups are Lua-defined entities now — see mods/core.)

// Marks a dead player waiting to respawn (co-op: death is not game-over).
// KERNEL because the server gates input on it; assigned/removed by the Lua
// death system.
struct Downed
{
    std::uint16_t respawn_wave; // respawns once GameStats.wave reaches this
};
