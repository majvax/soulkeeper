#pragma once
#include <functional>
#include "core/ecs.hpp"
#include <vector>



class SystemManager
{
public:
    using System = std::function<void(core::Registry&, float dt)>;

    void register_system(System system) { systems_.push_back(std::move(system)); }

    void update(core::Registry& registry, float dt)
    {
        for (auto& system : systems_) { system(registry, dt); }
    }

private:
    std::vector<System> systems_;
};
