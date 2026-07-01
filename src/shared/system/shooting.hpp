#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/factory/projectile.hpp"
#include <vector>

// Players fire a bullet toward their aim direction every Weapon.cooldown_max
// seconds while the trigger is held.
class ShootingSystem
{
public:
    void operator()(core::Registry& registry, float dt)
    {
        // Collect spawns first so we don't create entities mid-iteration.
        struct Spawn
        {
            float x, y, vx, vy, damage, lifetime;
        };
        std::vector<Spawn> spawns;

        registry.view<PlayerTag, Position, Weapon, AimState>().each(
          [&](core::Entity, const PlayerTag&, const Position& pos, Weapon& weapon, const AimState& aim) {
              if (weapon.cooldown_current > 0.0f) { weapon.cooldown_current -= dt; }

              const bool has_aim = (aim.dx * aim.dx) + (aim.dy * aim.dy) > 0.0f;
              if (aim.firing == 0 || weapon.cooldown_current > 0.0f || !has_aim) { return; }

              spawns.push_back({ .x = pos.x,
                                 .y = pos.y,
                                 .vx = aim.dx * weapon.bullet_speed,
                                 .vy = aim.dy * weapon.bullet_speed,
                                 .damage = weapon.damage,
                                 .lifetime = weapon.projectile_lifetime });
              weapon.cooldown_current = weapon.cooldown_max;
          });

        for (const Spawn& s : spawns) {
            create_projectile(registry, s.x, s.y, s.vx, s.vy, s.damage, s.lifetime);
        }
    }
};
