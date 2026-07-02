// src/shared/mod/lua_host.hpp
//
// Owns a Lua VM (sol::state) plus the mod state it populates: a ContentRegistry
// and an EventBus. SDL-free, so both the server (sim VM) and client (render VM)
// use it. The platform installs its own bindings (sim_bindings / render_bindings)
// BEFORE load_dir(); plugins then register content via `register_mod(...)`.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "shared/mod/events.hpp"
#include "shared/mod/registry.hpp"
#include "shared/mod/script_ecs.hpp"

namespace mod {

// Everything a plugin can register into, in one heap block so the registration
// closures capture a stable pointer (not `this`, which would dangle when the
// owning object — e.g. the client Engine — is moved by value).
struct ModState
{
    ContentRegistry registry;
    EnemyRegistry enemies;
    EventBus events;
    ScriptComponentRegistry scripts;
    std::vector<ScriptSystem> script_systems;
    std::string current_dir; // dir of the mod being loaded (for include())
};

class LuaHost
{
public:
    LuaHost();

    [[nodiscard]] sol::state& lua() noexcept { return lua_; }
    [[nodiscard]] ContentRegistry& registry() noexcept { return state_->registry; }
    [[nodiscard]] const ContentRegistry& registry() const noexcept { return state_->registry; }
    [[nodiscard]] EnemyRegistry& enemies() noexcept { return state_->enemies; }
    [[nodiscard]] const EnemyRegistry& enemies() const noexcept { return state_->enemies; }
    [[nodiscard]] EventBus& events() noexcept { return state_->events; }
    [[nodiscard]] const EventBus& events() const noexcept { return state_->events; }
    [[nodiscard]] ScriptComponentRegistry& scripts() noexcept { return state_->scripts; }
    [[nodiscard]] std::vector<ScriptSystem>& script_systems() noexcept { return state_->script_systems; }

    // Discover every `<dir>/*/mod.lua`, run it, call its global main(), then
    // finalize the registry (sort ids -> deterministic wire ids). Safe to call
    // once, after bindings are installed. Errors are logged, never fatal.
    void load_dir(const std::string& dir);

private:
    void install_registration_api(); // register_mod + the Mod usertype

    sol::state lua_;
    std::unique_ptr<ModState> state_ = std::make_unique<ModState>();
};

} // namespace mod
