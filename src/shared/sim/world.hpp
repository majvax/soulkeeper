#pragma once
#include "core/ecs.hpp"
#include "core/systems.hpp"

namespace shared {

// System ordering phases. Core systems occupy fixed slots; Lua (mod) systems map
// their `phase` to Motion (velocity tweaks, before Movement) or Update (general
// per-tick logic, after Combat). Gaps leave room for future phases.
namespace phase {
inline constexpr int Targeting = 100;
inline constexpr int Motion = 150;    // mod "motion": after Targeting, before Movement
inline constexpr int Shooting = 200;
inline constexpr int Movement = 300;
inline constexpr int Projectile = 400;
inline constexpr int Combat = 500;
inline constexpr int Update = 550;    // mod "update": after Combat, before Pickup/Death
inline constexpr int Pickup = 600;
inline constexpr int Death = 700;
} // namespace phase

// Headless simulation container: the authoritative ECS registry plus the
// ordered system pipeline. Deliberately free of SDL and ImGui so it compiles
// and runs identically on the dedicated server (authoritative) and inside a
// client scene (prediction / local play). Move-only (the Registry owns
// unique component pools).
class World
{
public:
    World() = default;

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = default;
    World& operator=(World&&) = default;
    ~World() = default;

    [[nodiscard]] core::Registry& registry() noexcept { return registry_; }
    [[nodiscard]] const core::Registry& registry() const noexcept { return registry_; }

    void add_system(int order, SystemManager::System system)
    {
        systems_.register_system(order, std::move(system));
    }

    // Advance the simulation by one fixed step.
    void step(float dt) { systems_.update(registry_, dt); }

private:
    core::Registry registry_;
    SystemManager systems_;
};

} // namespace shared
