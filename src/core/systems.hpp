#pragma once
#include "core/ecs.hpp"
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

// Ordered pipeline of systems. Each system has an integer `order` (see the
// phase constants in shared/sim/world.hpp); update() runs them ascending. Order
// lets Lua-defined (mod) systems slot into the right phase relative to the core
// systems (e.g. a slow effect must run after targeting, before movement).
class SystemManager
{
public:
    using System = std::function<void(core::Registry&, float dt)>;

    void register_system(int order, System system)
    {
        systems_.emplace_back(order, std::move(system));
        std::stable_sort(systems_.begin(), systems_.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    void update(core::Registry& registry, float dt)
    {
        for (auto& [order, system] : systems_) { system(registry, dt); }
    }

private:
    std::vector<std::pair<int, System>> systems_;
};
