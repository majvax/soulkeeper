// src/shared/mod/registry.cpp
#include "shared/mod/registry.hpp"

#include <algorithm>
#include <cstdio>

#include <fmt/format.h>

namespace mod {

void ContentRegistry::finalize()
{
    std::sort(defs_.begin(), defs_.end(),
              [](const ContentDef& a, const ContentDef& b) { return a.id < b.id; });
    id_to_wire_.clear();
    for (std::size_t i = 0; i < defs_.size(); ++i) {
        ContentDef& d = defs_[i];
        d.wire_id = static_cast<std::uint8_t>(i);
        id_to_wire_.emplace(d.id, d.wire_id);
        for (std::uint8_t r = 0; r < rarity_count; ++r) {
            if (d.value_fn.valid()) {
                // Advanced: the plugin computes the string (e.g. ms from a ratio).
                sol::protected_function_result res = d.value_fn(d.rarity_amounts[r], r);
                d.value_text[r] = res.valid() ? res.get<std::string>() : std::string{};
            } else if (!d.value_format.empty()) {
                // Simple case: a fmt-style format applied to the rarity amount.
                try {
                    d.value_text[r] = fmt::format(fmt::runtime(d.value_format), d.rarity_amounts[r]);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[mod] '%s' bad value_format '%s': %s\n", d.id.c_str(),
                                 d.value_format.c_str(), e.what());
                    d.value_text[r] = {};
                }
            }
        }
    }
}

void EnemyRegistry::finalize()
{
    std::sort(defs_.begin(), defs_.end(),
              [](const EnemyDef& a, const EnemyDef& b) { return a.id < b.id; });
    id_to_wire_.clear();
    for (std::size_t i = 0; i < defs_.size(); ++i) {
        defs_[i].wire_id = static_cast<std::uint8_t>(i);
        id_to_wire_.emplace(defs_[i].id, defs_[i].wire_id);
    }
}

float EnemyDef::weight_at(std::uint16_t wave) const
{
    if (!weight_fn.valid()) { return weight; }
    sol::protected_function_result res = weight_fn(static_cast<int>(wave));
    if (!res.valid()) {
        const sol::error err = res;
        std::fprintf(stderr, "[mod] enemy '%s' weight(wave) error: %s\n", id.c_str(), err.what());
        return 0.0f;
    }
    return res.get<float>();
}

} // namespace mod
