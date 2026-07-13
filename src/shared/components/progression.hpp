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

// While ANY entity carries this tag, the wave timer and natural spawning are
// frozen (boss arenas: mods put it ON the boss, so the hold dies with it).
// Prelude handle so Lua can set it; never networked.
struct WaveHold
{
};

// One-shot sim->server mailbox (RunEnd twin): `world:open_chest()` spawns an
// entity with this; the server consumes it after the step and runs a CHEST
// offer round for every player — the level-up machinery with flavor = Chest
// (the mod rolls it objects-only). Not networked, not a prelude handle.
struct ChestOpen
{
};

// One-shot sim->server mailbox: `world:grant_offer(kind)` files one of these to
// force an offer round WITHOUT the usual trigger (XP threshold / boss chest) —
// the dev commands /upgrade and /object. The server consumes exactly ONE per
// round, so N notes = N sequential menus (a granted round freezes the sim like
// any other, and the next note fires once picks complete). flavor mirrors
// proto::OfferFlavor (0 = Level/upgrade, 1 = Chest/object).
struct OfferGrant
{
    std::uint8_t flavor = 0;
};

// Per-player run scoreboard, incremented by the Lua damage/death/revive
// systems (prelude handle) and read by the server at run end for the
// game-over stats block. Never snapshotted — it only matters once.
struct RunStats
{
    float damage = 0.0f;        // total damage dealt to enemies
    std::int32_t kills = 0;     // killing blows credited (C.Bullet.owner etc.)
    std::int32_t downs = 0;     // times this player went down
    std::int32_t revives = 0;   // teammates picked back up
};
