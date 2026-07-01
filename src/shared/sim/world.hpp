#pragma once
#include "core/ecs.hpp"
#include "core/systems.hpp"

namespace shared {

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

    void add_system(SystemManager::System system) { systems_.register_system(std::move(system)); }

    // Advance the simulation by one fixed step.
    void step(float dt) { systems_.update(registry_, dt); }

private:
    core::Registry registry_;
    SystemManager systems_;
};

} // namespace shared
