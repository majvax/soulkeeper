#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/protocol.hpp"
#include "shared/system/input.hpp"

// Spawn the KERNEL part of a player: motion, hearts, dash, aim, render kind.
// The loadout (weapon, crit, ...) is Lua content — mods/core attaches it in
// its on_player_spawn handler.
inline core::Entity create_player(core::Registry& registry, float x, float y)
{
    const core::Entity player = registry.create();
    registry.assign(player, Position{ .x = x, .y = y });
    registry.assign(player, PrevPosition{ .x = x, .y = y });
    registry.assign(player, Velocity{ .dx = 0, .dy = 0 });
    registry.assign(player, PlayerTag{});
    registry.assign(player, Hearts{ .current = 3, .max = 3 });
    registry.assign(player, Radius{ .value = 12 });
    registry.assign(player, Speed{ .value = PLAYER_SPEED });
    registry.assign(player, AimState{ .dx = 1.0f, .dy = 0.0f, .firing = 0 });
    registry.assign(player, Dash{ .cooldown_max = DASH_COOLDOWN, .cooldown = 0.0f,
                                  .burst_remaining = 0.0f, .dir_x = 1.0f, .dir_y = 0.0f,
                                  .shockwave = 0.0f, .charges = 1, .max_charges = 1 });
    registry.assign(player, Render{ .kind = static_cast<std::uint8_t>(proto::EntityKind::Player),
                                    .variant = 0 });
    return player;
}
