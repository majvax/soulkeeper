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

namespace mod {

// Upgrade rarity. Kept here (rather than in progression/) so the registry — the
// lowest layer of the content system — owns it. Common/Uncommon/Legendary = 0/1/2.
enum class Rarity : std::uint8_t { Common, Uncommon, Legendary };
inline constexpr std::uint8_t rarity_count = 3;

// Roll weights: commons are frequent, legendaries rare.
[[nodiscard]] inline float rarity_weight(Rarity r)
{
    switch (r) {
    case Rarity::Common:    return 0.70f;
    case Rarity::Uncommon:  return 0.25f;
    case Rarity::Legendary: return 0.05f;
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

    // Callbacks (populated per-VM; may be empty — check .valid()).
    sol::protected_function available; // (Entity) -> bool           [sim]
    sol::protected_function apply;     // (Entity, rarity, amount)    [sim, stat]
    sol::protected_function acquire;   // (Entity)                    [sim, object]
    sol::protected_function draw;      // (ctx, view)                 [render]

    std::uint8_t wire_id = 0; // assigned by finalize()
};

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

} // namespace mod
