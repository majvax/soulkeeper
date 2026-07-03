// src/shared/mod/script_ecs.hpp
//
// The scripting-ECS metadata: Lua-defined components (a named schema of double
// fields, optionally networked) and Lua-defined systems. SDL-free; used by both
// VMs. On the server the registry is bound to the world Registry so each defined
// component gets a DynamicPool; on the client only the schema/net-id is kept (to
// deserialize snapshots and expose fields to draw hooks).
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "core/ecs.hpp"
#include "shared/protocol.hpp"

namespace mod {

// A Lua-defined component type.
struct ScriptSchema
{
    std::string id;                  // namespaced, e.g. "core:slow"
    std::vector<std::string> fields; // ordered field names (all doubles)
    std::vector<double> defaults;    // per-field default (e:set fills unset fields from these)
    bool networked = false;
    std::uint32_t pool_id = 0;       // DynamicPool id (valid only on a bound/server registry)
    bool has_pool = false;
    int net_id = -1;                 // sorted index among networked comps (both ends agree)

    [[nodiscard]] int field_index(const std::string& name) const
    {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i] == name) { return static_cast<int>(i); }
        }
        return -1;
    }
};

// A Lua-defined system: a function run each tick (or throttled to `rate` Hz) at
// the given pipeline `order`. The server wraps these into the World.
struct ScriptSystem
{
    std::string id;
    int order;
    double rate; // 0 = every tick
    sol::protected_function fn;
};

// Registry of script component schemas. Holds them in a deque so pointers stay
// stable as more are defined.
class ScriptComponentRegistry
{
public:
    // Server binds the world Registry so define() can allocate DynamicPools.
    void bind(core::Registry* reg) noexcept { registry_ = reg; }

    ScriptSchema* define(std::string id, std::vector<std::string> fields, std::vector<double> defaults,
                         bool networked)
    {
        if (ScriptSchema* existing = by_id(id)) { return existing; } // ignore duplicate
        ScriptSchema s;
        s.id = std::move(id);
        s.fields = std::move(fields);
        s.defaults = std::move(defaults);
        s.defaults.resize(s.fields.size(), 0.0);
        s.networked = networked;
        if (registry_ != nullptr) {
            s.pool_id = registry_->create_dynamic_pool(static_cast<std::uint32_t>(s.fields.size()));
            s.has_pool = true;
        }
        schemas_.push_back(std::move(s));
        return &schemas_.back();
    }

    // Assign deterministic net ids = sorted index among networked schemas.
    void finalize()
    {
        std::vector<ScriptSchema*> net;
        for (ScriptSchema& s : schemas_) {
            if (s.networked) { net.push_back(&s); }
        }
        std::sort(net.begin(), net.end(),
                  [](const ScriptSchema* a, const ScriptSchema* b) { return a->id < b->id; });
        for (std::size_t i = 0; i < net.size(); ++i) { net[i]->net_id = static_cast<int>(i); }
    }

    [[nodiscard]] ScriptSchema* by_id(const std::string& id)
    {
        for (ScriptSchema& s : schemas_) {
            if (s.id == id) { return &s; }
        }
        return nullptr;
    }
    [[nodiscard]] const ScriptSchema* by_id(const std::string& id) const
    {
        for (const ScriptSchema& s : schemas_) {
            if (s.id == id) { return &s; }
        }
        return nullptr;
    }
    [[nodiscard]] const ScriptSchema* by_net(int net_id) const
    {
        for (const ScriptSchema& s : schemas_) {
            if (s.net_id == net_id) { return &s; }
        }
        return nullptr;
    }
    [[nodiscard]] std::deque<ScriptSchema>& all() noexcept { return schemas_; }

private:
    std::deque<ScriptSchema> schemas_;
    core::Registry* registry_ = nullptr;
};

// --- snapshot (de)serialization of an entity's networked script components ----
// Wire layout per entity: uint8 count; then { uint8 net_id; float[fieldN] } × count.
// The field count is known from the shared schema, so it isn't sent.

inline void write_networked(proto::ByteWriter& w, core::Registry& reg, ScriptComponentRegistry& scripts,
                            core::Entity e)
{
    // Collect present networked components first (need the count up front).
    struct Present { std::uint8_t net_id; core::DynamicPool* pool; const ScriptSchema* schema; };
    std::vector<Present> present;
    for (ScriptSchema& s : scripts.all()) {
        if (!s.networked || !s.has_pool || s.net_id < 0) { continue; }
        core::DynamicPool* pool = reg.dynamic_pool(s.pool_id);
        if (pool != nullptr && pool->row(e) != nullptr) {
            present.push_back({ static_cast<std::uint8_t>(s.net_id), pool, &s });
        }
    }
    w.put(static_cast<std::uint8_t>(present.size()));
    for (const Present& p : present) {
        w.put(p.net_id);
        const double* row = p.pool->row(e);
        for (std::size_t k = 0; k < p.schema->fields.size(); ++k) {
            w.put(static_cast<float>(row[k]));
        }
    }
}

// One networked component's values, as read from a snapshot.
struct NetComp
{
    int net_id;
    std::vector<float> values;
};

inline std::vector<NetComp> read_networked(proto::ByteReader& r, const ScriptComponentRegistry& scripts)
{
    std::vector<NetComp> out;
    const auto count = r.get<std::uint8_t>();
    if (!count) { return out; }
    for (std::uint8_t i = 0; i < *count; ++i) {
        const auto net_id = r.get<std::uint8_t>();
        if (!net_id) { break; }
        const ScriptSchema* schema = scripts.by_net(*net_id);
        const std::size_t nfields = (schema != nullptr) ? schema->fields.size() : 0;
        NetComp nc{ .net_id = *net_id, .values = {} };
        nc.values.reserve(nfields);
        for (std::size_t k = 0; k < nfields; ++k) {
            const auto v = r.get<float>();
            nc.values.push_back(v.value_or(0.0f));
        }
        out.push_back(std::move(nc));
    }
    return out;
}

} // namespace mod
