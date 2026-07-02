#pragma once
#include <bitset>
#include <cstdint>

// Marks an entity that is driven by a connected player's input.
struct PlayerTag
{
};

// Which objects a player already owns, indexed by content wire id. The engine
// uses this to enforce "each object obtainable once" (independent of any
// component an object happens to grant). 256 = the uint8 wire-id range.
struct ObjectInventory
{
    std::bitset<256> owned;
};
