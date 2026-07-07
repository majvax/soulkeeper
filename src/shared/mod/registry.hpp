// src/shared/mod/registry.hpp
//
// The content registry: the SDL-free heart of the modding layer. Plugins (Lua)
// register upgrades and objects; the registry assigns each a deterministic wire
// id (the lexicographic-sort index of its namespaced string id), so the server
// and client agree on ids without any runtime counter. Both processes build an
// identical registry from the same plugin files.
//
// A ContentDef carries metadata declared identically on both ends (id, label,
// rarity amounts, value text, sprite) plus per-VM callbacks: the sim VM fills
// apply/available/acquire; the render VM fills draw. Unused callbacks stay empty.
#pragma once

#include <array>
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <sol/sol.hpp>

#include "shared/mod/component_ref.hpp"

namespace mod {

// Upgrade rarity. Kept here (rather than in progression/) so the registry — the
// lowest layer of the content system — owns it. Common..Legendary = 0..4.
enum class Rarity : std::uint8_t { Common, Uncommon, Rare, Epic, Legendary };
inline constexpr std::uint8_t rarity_count = 5;

// Roll weights: commons are frequent, legendaries rare.
[[nodiscard]] inline float rarity_weight(Rarity r)
{
    switch (r) {
    case Rarity::Common:    return 0.45f;
    case Rarity::Uncommon:  return 0.28f;
    case Rarity::Rare:      return 0.16f;
    case Rarity::Epic:      return 0.08f;
    case Rarity::Legendary: return 0.03f;
    }
    return 0.0f;
}

// Two content kinds — kept distinct so objects never carry a meaningless amount.
enum class ContentKind : std::uint8_t { StatUpgrade, Object };

// One registered piece of content. POD-ish metadata + optional Lua callbacks.
struct ContentDef
{
    std::string id;    // namespaced, e.g. "core:damage" — the sort key
    std::string label; // display name, e.g. "Sharp Rounds"
    ContentKind kind = ContentKind::StatUpgrade;

    // Stat-upgrade only: per-rarity magnitude + the card value text (precomputed
    // at finalize via string.format so the client never formats per frame).
    std::array<float, rarity_count> rarity_amounts{};
    std::array<std::string, rarity_count> value_text{}; // precomputed card text
    std::string value_format;         // simple case: printf-style on the amount, e.g. "+%d DMG"
    sol::protected_function value_fn;  // advanced: (amount, rarity) -> string; wins over value_format

    std::string sprite; // optional card sprite path (relative to repo root)

    // Objects only: the tier the object rolls at (no amount scaling). Stat
    // upgrades ignore this — their tiers come from nonzero rarity_amounts.
    Rarity object_rarity = Rarity::Epic;

    // Callbacks (populated per-VM; may be empty — check .valid()).
    sol::protected_function available; // (Entity) -> bool           [sim]
    sol::protected_function apply;     // (Entity, rarity, amount)    [sim, stat]
    sol::protected_function acquire;   // (Entity)                    [sim, object]
    sol::protected_function draw;      // (ctx, view)                 [render]

    std::uint8_t wire_id = 0; // assigned by finalize()
};

// Whether a piece of content can be offered at a tier: stat upgrades need a
// nonzero amount there (missing/0 in the Lua table = "not at this tier");
// objects roll only at their declared rarity.
[[nodiscard]] inline bool offered_at(const ContentDef& def, Rarity tier)
{
    if (def.kind == ContentKind::Object) { return def.object_rarity == tier; }
    return def.rarity_amounts[static_cast<std::size_t>(tier)] != 0.0f;
}

// Holds all registered content and the string-id -> wire-id map.
class ContentRegistry
{
public:
    // Append a def during plugin load. Duplicate ids are dropped (first wins).
    // wire ids are NOT valid until finalize().
    void add(ContentDef def)
    {
        if (index_of(def.id) != npos) { return; } // ignore duplicate id
        defs_.push_back(std::move(def));
    }

    // Sort by string id and assign wire ids = sorted index (identical on every
    // process that loaded the same plugin set), and precompute stat value text
    // (fmt::format for value_format, else the Lua value_text fn). See registry.cpp.
    void finalize();

    [[nodiscard]] std::size_t count() const noexcept { return defs_.size(); }
    [[nodiscard]] const std::vector<ContentDef>& defs() const noexcept { return defs_; }

    [[nodiscard]] const ContentDef* by_wire(std::uint8_t wire) const noexcept
    {
        return wire < defs_.size() ? &defs_[wire] : nullptr;
    }
    [[nodiscard]] ContentDef* by_wire(std::uint8_t wire) noexcept
    {
        return wire < defs_.size() ? &defs_[wire] : nullptr;
    }

    [[nodiscard]] const ContentDef* by_id(const std::string& id) const
    {
        const auto it = id_to_wire_.find(id);
        return it == id_to_wire_.end() ? nullptr : &defs_[it->second];
    }

    [[nodiscard]] float amount(std::uint8_t wire, Rarity r) const noexcept
    {
        const ContentDef* d = by_wire(wire);
        return d ? d->rarity_amounts[static_cast<std::size_t>(r)] : 0.0f;
    }

private:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    [[nodiscard]] std::size_t index_of(const std::string& id) const
    {
        for (std::size_t i = 0; i < defs_.size(); ++i) {
            if (defs_[i].id == id) { return i; }
        }
        return npos;
    }

    std::vector<ContentDef> defs_;
    std::unordered_map<std::string, std::uint8_t> id_to_wire_;
};

// A component attached to every spawned instance of an archetype, declared via
// the builder's :component(Handle, fields | fun(wave) -> fields). Enemies are
// pure component bags: Health/Speed/Radius/... come through this list too.
// Function inits are resolved once per wave (cached next to the spawn weights).
struct EnemyComponentInit
{
    ComponentRef ref;
    sol::object init; // a fields table, or fun(wave) -> fields table
};

// One registered enemy archetype. Enemies live in their OWN registry (separate
// wire-id space from upgrades/objects): the snapshot `variant` byte is the
// enemy wire id, and the level-up roll never has to filter them out.
struct EnemyDef
{
    std::string id;    // namespaced, e.g. "core:brute" — the sort key
    std::string label; // display name

    std::vector<EnemyComponentInit> components; // the archetype's component bag

    // Resolve the init list for a wave: tables pass through, functions are
    // called (protected; errors logged -> empty table). registry.cpp.
    [[nodiscard]] std::vector<std::pair<const ComponentRef*, sol::table>>
    inits_at(std::uint16_t wave) const;

    // Spawn weighting: either a constant or a Lua fn(wave) -> number (wins over
    // the constant). Evaluated once per wave server-side, never per spawn.
    float weight = 0.0f;
    sol::protected_function weight_fn;
    [[nodiscard]] float weight_at(std::uint16_t wave) const; // registry.cpp (protected call + log)

    // Render VM: how to draw it (shared enemy sprite unless `sprite` overrides).
    float scale = 1.0f;
    std::array<std::uint8_t, 3> tint{ 255, 255, 255 };
    std::string sprite;

    // Arena rect half-extents (0 = none), opt `arena = { w, h }`: while this
    // enemy lives, the CLIENT clamps local prediction to the FIXED rect around
    // the point where it spawned (= its position at first sighting; arena
    // bosses spawn at their arena's center) and draws the wall. The SIM-side
    // confinement is mod logic (core clamps via C.Nova) — these def fields
    // just let the client know the shape, hash-free.
    float arena_w = 0.0f, arena_h = 0.0f;

    sol::protected_function on_spawn; // optional (Entity) [sim] — attach extra components etc.

    std::uint8_t wire_id = 0; // assigned by finalize()
};

// Registered enemies + the same deterministic string-id -> wire-id scheme as
// ContentRegistry (wire id = lexicographic sort index).
class EnemyRegistry
{
public:
    void add(EnemyDef def)
    {
        for (const EnemyDef& d : defs_) {
            if (d.id == def.id) { return; } // ignore duplicate id (first wins)
        }
        defs_.push_back(std::move(def));
    }

    void finalize(); // sort by id, wire id = index — see registry.cpp

    [[nodiscard]] std::size_t count() const noexcept { return defs_.size(); }
    [[nodiscard]] const std::vector<EnemyDef>& defs() const noexcept { return defs_; }

    [[nodiscard]] const EnemyDef* by_wire(std::uint8_t wire) const noexcept
    {
        return wire < defs_.size() ? &defs_[wire] : nullptr;
    }

    [[nodiscard]] const EnemyDef* by_id(const std::string& id) const
    {
        const auto it = id_to_wire_.find(id);
        return it == id_to_wire_.end() ? nullptr : &defs_[it->second];
    }

    // Mutable pre-finalize lookup (linear scan — the id map doesn't exist yet).
    // Used by the EnemyBuilder to append component inits during load.
    [[nodiscard]] EnemyDef* find(const std::string& id)
    {
        for (EnemyDef& def : defs_) {
            if (def.id == id) { return &def; }
        }
        return nullptr;
    }

private:
    std::vector<EnemyDef> defs_;
    std::unordered_map<std::string, std::uint8_t> id_to_wire_;
};

} // namespace mod
