#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/factory/projectile.hpp"
#include <random>
#include <vector>

// Players fire a bullet toward their aim direction every Weapon.cooldown_max
// seconds while the trigger is held. Each shot rolls the player's Crit stats:
// a crit multiplies the damage and tags the bullet (renders bigger/orange).
class ShootingSystem
{
public:
    void operator()(core::Registry& registry, float dt)
    {
        // Collect spawns first so we don't create entities mid-iteration.
        struct Spawn
        {
            float x, y, vx, vy, damage, lifetime;
            bool crit;
        };
        std::vector<Spawn> spawns;

        registry.view<PlayerTag, Position, Weapon, AimState>().each(
          [&](core::Entity player, const PlayerTag&, const Position& pos, Weapon& weapon, const AimState& aim) {
              if (weapon.cooldown_current > 0.0f) { weapon.cooldown_current -= dt; }

              const bool has_aim = (aim.dx * aim.dx) + (aim.dy * aim.dy) > 0.0f;
              if (aim.firing == 0 || weapon.cooldown_current > 0.0f || !has_aim) { return; }

              const Crit* crit = registry.try_get<Crit>(player);
              const bool is_crit = crit != nullptr && roll_(rng_) < crit->chance;
              spawns.push_back({ .x = pos.x,
                                 .y = pos.y,
                                 .vx = aim.dx * weapon.bullet_speed,
                                 .vy = aim.dy * weapon.bullet_speed,
                                 .damage = weapon.damage * (is_crit ? crit->multiplier : 1.0f),
                                 .lifetime = weapon.projectile_lifetime,
                                 .crit = is_crit });
              weapon.cooldown_current = weapon.cooldown_max;
          });

        for (const Spawn& s : spawns) {
            const core::Entity bullet = create_projectile(registry, s.x, s.y, s.vx, s.vy, s.damage, s.lifetime);
            if (s.crit) { registry.assign(bullet, CritTag{}); }
        }
    }

private:
    std::minstd_rand rng_{ std::random_device{}() };
    std::uniform_real_distribution<float> roll_{ 0.0f, 1.0f };
};
