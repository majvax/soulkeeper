#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/system/input.hpp"

// Spawn a player entity. Players shoot bullets toward their aim; the aura is a
// later upgrade and is intentionally not assigned here.
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
    registry.assign(player, Weapon{ .cooldown_max = 0.35f,
                                    .cooldown_current = 0.0f,
                                    .bullet_speed = 550.0f,
                                    .damage = 10.0f,
                                    .projectile_lifetime = 1.2f });
    registry.assign(player, AimState{ .dx = 1.0f, .dy = 0.0f, .firing = 0 });
    registry.assign(player, Dash{ .cooldown_max = DASH_COOLDOWN, .cooldown = 0.0f,
                                  .burst_remaining = 0.0f, .dir_x = 1.0f, .dir_y = 0.0f,
                                  .shockwave = 0.0f, .charges = 1, .max_charges = 1 });
    registry.assign(player, Crit{ .chance = 0.05f, .multiplier = 1.5f });
    return player;
}
