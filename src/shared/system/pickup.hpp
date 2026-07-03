#pragma once
#include "core/ecs.hpp"
#include "core/spatial.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/components/progression.hpp"
#include <unordered_set>

// Players collect nearby XP orbs into the shared team pool (GameStats), and
// healing hearts into their own Hearts (only while hurt — full players leave
// hearts on the ground). Downed players don't collect. A pickup grabbed in the
// same tick by two players only counts once.
class PickupSystem
{
public:
    void operator()(core::Registry& registry, float /*dt*/)
    {
        GameStats* stats = nullptr;
        registry.view<GameStats>().each([&](core::Entity, GameStats& s) { stats = &s; });
        if (stats == nullptr) { return; }

        core::SpatialGrid grid{ cell_size };
        registry.view<XpOrb, Position>().each(
          [&](core::Entity orb, const XpOrb&, const Position& pos) { grid.insert(orb, pos.x, pos.y); });

        core::SpatialGrid hearts_grid{ cell_size };
        registry.view<HeartPickup, Position>().each(
          [&](core::Entity heart, const HeartPickup&, const Position& pos) {
              hearts_grid.insert(heart, pos.x, pos.y);
          });

        std::unordered_set<core::Entity> collected;
        registry.view<PlayerTag, Position>().each([&](core::Entity player, const PlayerTag&, const Position& ppos) {
            if (registry.try_get<Downed>(player) != nullptr) { return; }
            const float reach = collect_radius + cell_size;
            for (const core::Entity orb :
                 grid.query(ppos.x - reach, ppos.y - reach, ppos.x + reach, ppos.y + reach)) {
                const Position* opos = registry.try_get<Position>(orb);
                const XpOrb* xp = registry.try_get<XpOrb>(orb);
                if (!opos || !xp) { continue; }
                const float dx = opos->x - ppos.x;
                const float dy = opos->y - ppos.y;
                if ((dx * dx) + (dy * dy) < collect_radius * collect_radius) {
                    if (collected.insert(orb).second) { stats->xp += xp->value; }
                }
            }

            Hearts* hearts = registry.try_get<Hearts>(player);
            if (hearts == nullptr || hearts->current >= hearts->max) { return; } // full: leave heals
            for (const core::Entity heart :
                 hearts_grid.query(ppos.x - reach, ppos.y - reach, ppos.x + reach, ppos.y + reach)) {
                const Position* hpos = registry.try_get<Position>(heart);
                if (hpos == nullptr) { continue; }
                const float dx = hpos->x - ppos.x;
                const float dy = hpos->y - ppos.y;
                if ((dx * dx) + (dy * dy) < collect_radius * collect_radius) {
                    if (collected.insert(heart).second) {
                        ++hearts->current;
                        if (hearts->current >= hearts->max) { break; }
                    }
                }
            }
        });

        for (const core::Entity picked : collected) { registry.destroy(picked); }
    }

private:
    static constexpr float collect_radius = 45.0f;
    static constexpr float cell_size = 128.0f;
};
