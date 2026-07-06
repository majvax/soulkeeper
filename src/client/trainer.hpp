#pragma once
// ============================================================================
// Practice trainer — client-side autoaim. OFF unless the env var SK_TRAINER=1.
//
// It snaps the outgoing aim toward the nearest enemy using state the client
// ALREADY decodes from snapshots (the render Registry). It is NOT a mod, so the
// server's join-time plugin-hash check is unaffected — the server validates the
// mod set, not your client binary — you won't be kicked. Aim-only: you still
// hold LMB to fire, so it just reads as clean aim.
//
// Enable:  SK_TRAINER=1 ./bin/client <host> <name>
// Hook:    one call in GameScene, right after the mouse aim is computed and
//          before send_input (see game.hpp).
// ============================================================================
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>

#include "core/ecs.hpp"
#include "shared/components/physics.hpp"
#include "shared/protocol.hpp"

namespace trainer {

// Read the env var once (first call). Set SK_TRAINER=1 in the launching shell.
[[nodiscard]] inline bool enabled()
{
    static const bool on = [] {
        const char* v = std::getenv("SK_TRAINER");
        return v != nullptr && v[0] == '1';
    }();
    return on;
}

// Snap (ax, ay) to the unit direction from the local player to the nearest live
// enemy. `RemoteT` is the client's per-entity tag carrying `.kind` (EntityKind);
// templated so this file needn't know GameScene's internals. The camera is
// player-centered and screen/world axes are aligned, so a world-space direction
// is exactly the aim the mouse would have produced — no screen projection needed.
template <typename RemoteT>
void autoaim(core::Registry& reg, core::Entity player, bool has_player, float& ax, float& ay)
{
    if (!enabled() || !has_player) { return; }
    const Position* me = reg.try_get<Position>(player);
    if (me == nullptr) { return; }

    float best = std::numeric_limits<float>::max();
    float bx = 0.0f;
    float by = 0.0f;
    bool found = false;
    reg.view<RemoteT, Position>().each([&](core::Entity, const RemoteT& r, const Position& p) {
        if (r.kind != static_cast<std::uint8_t>(proto::EntityKind::Enemy)) { return; }
        const float dx = p.x - me->x;
        const float dy = p.y - me->y;
        const float d2 = (dx * dx) + (dy * dy);
        if (d2 < best) {
            best = d2;
            bx = p.x;
            by = p.y;
            found = true;
        }
    });
    if (!found) { return; } // no enemies in view -> keep the mouse aim

    const float dx = bx - me->x;
    const float dy = by - me->y;
    const float len = std::sqrt((dx * dx) + (dy * dy));
    if (len > 0.001f) {
        ax = dx / len;
        ay = dy / len;
    }
}

} // namespace trainer
