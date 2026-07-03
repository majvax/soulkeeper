#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/components/progression.hpp"
#include "shared/factory/heart.hpp"
#include "shared/factory/xp_orb.hpp"
#include <random>
#include <vector>

// Enemies die -> drop an XP orb worth their archetype's value, and rarely a
// healing heart. Players don't die permanently (co-op): they go Downed and
// respawn a few waves later.
class DeathSystem
{
public:
    static constexpr std::uint16_t respawn_waves = 2;
    static constexpr float heart_drop_chance = 0.04f;

    void operator()(core::Registry& registry, float /*dt*/)
    {
        std::uint16_t wave = 0;
        registry.view<GameStats>().each([&](core::Entity, const GameStats& stats) { wave = stats.wave; });

        std::vector<core::Entity> dead;
        registry.view<EnemyTag, Health>().each([&](core::Entity enemy, const EnemyTag&, const Health& hp) {
            if (hp.current <= 0.0f) { dead.push_back(enemy); }
        });
        for (const core::Entity enemy : dead) {
            const Position& pos = registry.get<Position>(enemy);
            std::uint32_t xp = 1;
            if (const XpReward* reward = registry.try_get<XpReward>(enemy)) { xp = reward->value; }
            create_xp_orb(registry, pos.x, pos.y, xp);
            if (chance_(rng_) < heart_drop_chance) { create_heart(registry, pos.x + 14, pos.y); }
            registry.destroy(enemy);
        }

        registry.view<PlayerTag, Hearts, Position>().each(
          [&](core::Entity player, const PlayerTag&, Hearts& hearts, Position& pos) {
              if (Downed* downed = registry.try_get<Downed>(player)) {
                  if (wave >= downed->respawn_wave) {
                      registry.remove<Downed>(player);
                      hearts.current = hearts.max;
                      pos = { .x = 0, .y = 0 };
                  }
              } else if (hearts.current <= 0) {
                  registry.assign(player, Downed{ .respawn_wave = static_cast<std::uint16_t>(wave + respawn_waves) });
                  if (Velocity* vel = registry.try_get<Velocity>(player)) { *vel = { .dx = 0, .dy = 0 }; }
              }
          });
    }

private:
    std::minstd_rand rng_{ std::random_device{}() };
    std::uniform_real_distribution<float> chance_{ 0.0f, 1.0f };
};
