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
    std::uint16_t respawn_wave = 0; // respawns once GameStats.wave reaches this
};

// One-shot sim->server mailbox: `world:end_game(won)` spawns an entity with
// this; the server picks it up after the step and runs the game-over flow.
// Not networked, not a prelude handle — the verb is the only writer.
struct RunEnd
{
    std::uint8_t won = 0;
};
