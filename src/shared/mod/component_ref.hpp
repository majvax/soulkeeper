// src/shared/mod/component_ref.hpp
//
// THE component handle: the one value Lua passes to every API that names a
// component (e:get/set/has/remove, world:each, archetype:component, view:get).
// Wraps either an engine component (index into the sim VM's BindingTable) or a
// Lua-defined schema. Handles are created by the engine prelude (Position,
// Velocity, ...) and by mod:component(...) — plugins never touch string ids,
// so a typo is an error at definition time, not a silent nil at use time.
#pragma once

#include <string>

namespace mod {

struct ScriptSchema;

struct ComponentRef
{
    int engine_tag = -1;            // >= 0: index into the engine BindingTable
    ScriptSchema* schema = nullptr; // Lua-defined component
    std::string id;                 // full id, e.g. "Position" / "core:ranged" (errors, wire, hash)

    [[nodiscard]] bool is_engine() const noexcept { return engine_tag >= 0; }
    [[nodiscard]] bool valid() const noexcept { return engine_tag >= 0 || schema != nullptr; }
};

} // namespace mod
