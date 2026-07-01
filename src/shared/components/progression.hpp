#pragma once
#include <cstdint>

// Shared world state — held by a single "world" singleton entity.
struct GameStats
{
    std::uint32_t xp;
    std::uint16_t wave;
};

// A dropped experience orb; collected by walking near it.
struct XpOrb
{
    std::uint32_t value;
};

// Marks a dead player waiting to respawn (co-op: death is not game-over).
struct Downed
{
    std::uint16_t respawn_wave; // respawns once GameStats.wave reaches this
};
