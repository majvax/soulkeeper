#pragma once
#include "core/spatial.hpp"
#include <bitset>
#include <cstdint>

// Marks an entity that is driven by a connected player's input.
struct PlayerTag
{
};

// Singleton: the per-tick spatial hash over every Position entity, rebuilt by
// GridSystem and served to Lua as world:nearby(...). Lives in a component so
// systems (which only see the Registry) and bindings share it without extra
// plumbing.
struct WorldGrid
{
    core::SpatialGrid grid{ 128.0f };
};

// Which objects a player already owns, indexed by content wire id. The engine
// uses this to enforce "each object obtainable once" (independent of any
// component an object happens to grant). 256 = the uint8 wire-id range.
struct ObjectInventory
{
    std::bitset<256> owned;
};
