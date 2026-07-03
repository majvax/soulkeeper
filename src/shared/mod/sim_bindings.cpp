// src/shared/mod/sim_bindings.cpp
#include "shared/mod/sim_bindings.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/factory/enemy.hpp"
#include "shared/factory/projectile.hpp"
#include "shared/factory/xp_orb.hpp"
#include "shared/mod/script_ecs.hpp"
#include "shared/sim/world.hpp"
#include "shared/system/input.hpp" // DASH_COOLDOWN default for the Dash binding

namespace mod {

namespace {

// One entry per engine component type: the runtime dispatch behind
// e:has/get/assign/remove, plus an erased pool fetcher for runtime queries. The
// Lua global for each component (e.g. `Weapon`) is its index into this table.
struct ComponentBinding
{
    std::function<bool(core::Registry&, core::Entity)> has;
    std::function<sol::object(sol::state_view, core::Registry&, core::Entity)> get;
    std::function<void(core::Registry&, core::Entity, const sol::table&)> assign;
    std::function<void(core::Registry&, core::Entity)> remove;
    std::function<core::SparseSet*(core::Registry&)> pool; // for world:each membership
};

using BindingTable = std::vector<ComponentBinding>;

// Register an engine component usertype + its dispatch entry, and expose `name`
// as the tag global. `builder` constructs the component from a Lua table.
template <typename T, typename Builder, typename... Fields>
void register_component(sol::state& lua, BindingTable& table, const char* name, Builder builder,
                        Fields&&... fields)
{
    // Hidden metatable name so it doesn't shadow the `name` tag global.
    lua.new_usertype<T>(std::string("_ct_") + name, std::forward<Fields>(fields)...);
    const int tag = static_cast<int>(table.size());
    table.push_back(ComponentBinding{
      .has = [](core::Registry& r, core::Entity e) { return r.has<T>(e); },
      .get = [](sol::state_view sv, core::Registry& r, core::Entity e) -> sol::object {
          T* p = r.try_get<T>(e);
          return p ? sol::make_object(sv, std::ref(*p)) : sol::lua_nil;
      },
      .assign = [builder](core::Registry& r, core::Entity e, const sol::table& t) {
          if (r.has<T>(e)) { r.remove<T>(e); } // assign == set/replace
          r.assign(e, builder(t));
      },
      .remove = [](core::Registry& r, core::Entity e) { if (r.has<T>(e)) { r.remove<T>(e); } },
      .pool = [](core::Registry& r) { return r.raw_pool(core::type_id<T>()); },
    });
    lua[name] = tag;
}

// Write-through proxy over a script component's field row. Re-resolves the row
// each access so it survives pool reallocation.
struct ScriptFieldProxy
{
    core::DynamicPool* pool;
    core::Entity entity;
    const ScriptSchema* schema;
};

// Resolve a query argument (int engine tag or string script id) to a pool, or
// nullptr (missing → the query is empty).
core::SparseSet* resolve_pool(const sol::object& arg, core::Registry& reg, const BindingTable& table,
                              ScriptComponentRegistry& scripts)
{
    if (arg.get_type() == sol::type::number) {
        const int tag = arg.as<int>();
        if (tag >= 0 && tag < static_cast<int>(table.size())) { return table[tag].pool(reg); }
        return nullptr;
    }
    if (arg.get_type() == sol::type::string) {
        ScriptSchema* s = scripts.by_id(arg.as<std::string>());
        if (s != nullptr && s->has_pool) { return reg.dynamic_pool(s->pool_id); }
        return nullptr;
    }
    return nullptr;
}

// The `world` facade handed to Lua systems: runtime queries over components.
struct ScriptWorld
{
    core::Registry* reg;
    std::shared_ptr<BindingTable> table;
    ScriptComponentRegistry* scripts;
    sol::state_view lua;

    // world:each(a, b, ...) -> iterator over EntityHandles owning all components.
    sol::object each(sol::variadic_args va)
    {
        std::vector<core::SparseSet*> pools;
        pools.reserve(va.size());
        for (const sol::stack_proxy arg : va) {
            core::SparseSet* p = resolve_pool(arg, *reg, *table, *scripts);
            pools.push_back(p); // nullptr => empty result
        }

        const core::SparseSet* lead = nullptr;
        bool empty = pools.empty();
        for (core::SparseSet* p : pools) {
            if (p == nullptr) { empty = true; break; }
            if (lead == nullptr || p->size() < lead->size()) { lead = p; }
        }

        auto ents = std::make_shared<std::vector<core::Entity>>();
        auto others = std::make_shared<std::vector<core::SparseSet*>>();
        if (!empty && lead != nullptr) {
            ents->assign(lead->begin(), lead->end()); // snapshot: safe against edits mid-iteration
            for (core::SparseSet* p : pools) {
                if (p != lead) { others->push_back(p); }
            }
        }

        core::Registry* rr = reg;
        sol::state_view sv = lua;
        auto idx = std::make_shared<std::size_t>(0);
        std::function<sol::object(sol::variadic_args)> iter =
          [rr, ents, others, idx, sv](sol::variadic_args) -> sol::object {
              while (*idx < ents->size()) {
                  const core::Entity e = (*ents)[(*idx)++];
                  if (!rr->valid(e)) { continue; }
                  bool ok = true;
                  for (core::SparseSet* p : *others) {
                      if (!p->contains(e)) { ok = false; break; }
                  }
                  if (ok) { return sol::make_object(sv, EntityHandle{ .reg = rr, .entity = e }); }
              }
              return sol::lua_nil;
          };
        return sol::make_object(sv, iter);
    }
};

} // namespace

void install_sim_bindings(LuaHost& host, core::Registry& world_reg)
{
    sol::state& lua = host.lua();
    host.scripts().bind(&world_reg); // define_component allocates pools in this registry
    auto table = std::make_shared<BindingTable>();

    register_component<Position>(lua, *table, "Position",
      [](const sol::table& t) { return Position{ .x = t.get_or("x", 0.0f), .y = t.get_or("y", 0.0f) }; },
      "x", &Position::x, "y", &Position::y);
    register_component<Velocity>(lua, *table, "Velocity",
      [](const sol::table& t) { return Velocity{ .dx = t.get_or("dx", 0.0f), .dy = t.get_or("dy", 0.0f) }; },
      "dx", &Velocity::dx, "dy", &Velocity::dy);
    register_component<Speed>(lua, *table, "Speed",
      [](const sol::table& t) { return Speed{ .value = t.get_or("value", 0.0f) }; },
      "value", &Speed::value);
    register_component<Health>(lua, *table, "Health",
      [](const sol::table& t) { return Health{ .current = t.get_or("current", 0.0f), .max = t.get_or("max", 0.0f) }; },
      "current", &Health::current, "max", &Health::max);
    register_component<Hearts>(lua, *table, "Hearts",
      [](const sol::table& t) {
          return Hearts{ .current = static_cast<std::int16_t>(t.get_or("current", 3)),
                         .max = static_cast<std::int16_t>(t.get_or("max", 3)) };
      },
      "current", &Hearts::current, "max", &Hearts::max);
    register_component<Radius>(lua, *table, "Radius",
      [](const sol::table& t) { return Radius{ .value = t.get_or("value", 0.0f) }; },
      "value", &Radius::value);
    register_component<Damage>(lua, *table, "Damage",
      [](const sol::table& t) { return Damage{ .per_second = t.get_or("per_second", 0.0f) }; },
      "per_second", &Damage::per_second);
    // (Aura / SlowField are Lua script components now — not engine bindings.)
    register_component<Weapon>(lua, *table, "Weapon",
      [](const sol::table& t) {
          return Weapon{ .cooldown_max = t.get_or("cooldown_max", 0.0f),
                         .cooldown_current = t.get_or("cooldown_current", 0.0f),
                         .bullet_speed = t.get_or("bullet_speed", 0.0f),
                         .damage = t.get_or("damage", 0.0f),
                         .projectile_lifetime = t.get_or("projectile_lifetime", 0.0f) };
      },
      "cooldown_max", &Weapon::cooldown_max, "cooldown_current", &Weapon::cooldown_current,
      "bullet_speed", &Weapon::bullet_speed, "damage", &Weapon::damage,
      "projectile_lifetime", &Weapon::projectile_lifetime);
    register_component<AimState>(lua, *table, "AimState",
      [](const sol::table& t) {
          return AimState{ .dx = t.get_or("dx", 0.0f), .dy = t.get_or("dy", 0.0f),
                           .firing = static_cast<std::uint8_t>(t.get_or("firing", 0)) };
      },
      "dx", &AimState::dx, "dy", &AimState::dy, "firing", &AimState::firing);
    register_component<Dash>(lua, *table, "Dash",
      [](const sol::table& t) {
          return Dash{ .cooldown_max = t.get_or("cooldown_max", DASH_COOLDOWN),
                       .cooldown = t.get_or("cooldown", 0.0f),
                       .burst_remaining = 0.0f,
                       .dir_x = 1.0f, .dir_y = 0.0f,
                       .shockwave = t.get_or("shockwave", 0.0f),
                       .charges = static_cast<std::uint8_t>(t.get_or("charges", 1)),
                       .max_charges = static_cast<std::uint8_t>(t.get_or("max_charges", 1)) };
      },
      "cooldown_max", &Dash::cooldown_max, "cooldown", &Dash::cooldown, "shockwave", &Dash::shockwave,
      "charges", &Dash::charges, "max_charges", &Dash::max_charges);
    register_component<Crit>(lua, *table, "Crit",
      [](const sol::table& t) {
          return Crit{ .chance = t.get_or("chance", 0.0f), .multiplier = t.get_or("multiplier", 1.5f) };
      },
      "chance", &Crit::chance, "multiplier", &Crit::multiplier);
    // Tag components — no fields; used for membership in queries (`Enemy`, `Player`).
    register_component<EnemyTag>(lua, *table, "Enemy", [](const sol::table&) { return EnemyTag{}; });
    register_component<PlayerTag>(lua, *table, "Player", [](const sol::table&) { return PlayerTag{}; });

    // Write-through proxy for script component fields.
    lua.new_usertype<ScriptFieldProxy>(
      "_ScriptFieldProxy", sol::no_constructor,
      sol::meta_function::index,
      [](ScriptFieldProxy& p, const std::string& key, sol::this_state ts) -> sol::object {
          const int i = p.schema->field_index(key);
          double* row = (i >= 0) ? p.pool->row(p.entity) : nullptr;
          return row ? sol::make_object(ts, row[i]) : sol::lua_nil;
      },
      sol::meta_function::new_index, [](ScriptFieldProxy& p, const std::string& key, double v) {
          const int i = p.schema->field_index(key);
          if (i < 0) { return; }
          if (double* row = p.pool->row(p.entity)) { row[i] = v; }
      });

    ScriptComponentRegistry* scripts = &host.scripts();

    // Entity handle: engine components by int tag, script components by string id.
    lua.new_usertype<EntityHandle>(
      "Entity", sol::no_constructor,
      "has", sol::overload(
               [table](EntityHandle& h, int tag) {
                   return tag >= 0 && tag < static_cast<int>(table->size()) && (*table)[tag].has(*h.reg, h.entity);
               },
               [scripts](EntityHandle& h, const std::string& id) {
                   ScriptSchema* s = scripts->by_id(id);
                   if (s == nullptr || !s->has_pool) { return false; }
                   core::DynamicPool* p = h.reg->dynamic_pool(s->pool_id);
                   return p != nullptr && p->row(h.entity) != nullptr;
               }),
      "get", sol::overload(
               [table](EntityHandle& h, int tag, sol::this_state ts) -> sol::object {
                   if (tag < 0 || tag >= static_cast<int>(table->size())) { return sol::lua_nil; }
                   return (*table)[tag].get(ts, *h.reg, h.entity);
               },
               [scripts](EntityHandle& h, const std::string& id, sol::this_state ts) -> sol::object {
                   ScriptSchema* s = scripts->by_id(id);
                   if (s == nullptr || !s->has_pool) { return sol::lua_nil; }
                   core::DynamicPool* p = h.reg->dynamic_pool(s->pool_id);
                   if (p == nullptr || p->row(h.entity) == nullptr) { return sol::lua_nil; }
                   return sol::make_object(ts, ScriptFieldProxy{ .pool = p, .entity = h.entity, .schema = s });
               }),
      "assign", [table](EntityHandle& h, int tag, sol::table fields) {
          if (tag >= 0 && tag < static_cast<int>(table->size())) { (*table)[tag].assign(*h.reg, h.entity, fields); }
      },
      "set", [scripts](EntityHandle& h, const std::string& id, sol::table fields) {
          ScriptSchema* s = scripts->by_id(id);
          if (s == nullptr || !s->has_pool) { return; }
          core::DynamicPool* p = h.reg->dynamic_pool(s->pool_id);
          if (p == nullptr) { return; }
          double* row = p->emplace_default(h.entity);
          for (std::size_t i = 0; i < s->fields.size(); ++i) { row[i] = fields.get_or(s->fields[i], 0.0); }
      },
      "remove", sol::overload(
                  [table](EntityHandle& h, int tag) {
                      if (tag >= 0 && tag < static_cast<int>(table->size())) { (*table)[tag].remove(*h.reg, h.entity); }
                  },
                  [scripts](EntityHandle& h, const std::string& id) {
                      ScriptSchema* s = scripts->by_id(id);
                      if (s != nullptr && s->has_pool) {
                          if (core::DynamicPool* p = h.reg->dynamic_pool(s->pool_id)) { p->remove(h.entity); }
                      }
                  }));

    // The `world` query facade.
    lua.new_usertype<ScriptWorld>("_ScriptWorld", sol::no_constructor, "each", &ScriptWorld::each);
    lua["world"] = ScriptWorld{ .reg = &world_reg, .table = table, .scripts = scripts, .lua = lua };

    // Spawn factories (for content that creates entities).
    core::Registry* reg = &world_reg;
    lua.set_function("spawn_projectile", [reg](float x, float y, float vx, float vy, float dmg, float life,
                                               sol::optional<bool> hostile) {
        return EntityHandle{ .reg = reg,
                             .entity = create_projectile(*reg, x, y, vx, vy, dmg, life,
                                                         hostile.value_or(false)) };
    });
    lua.set_function("spawn_xp_orb", [reg](float x, float y, int value) {
        return EntityHandle{ .reg = reg, .entity = create_xp_orb(*reg, x, y, static_cast<std::uint32_t>(value)) };
    });
    const EnemyRegistry* enemies = &host.enemies(); // heap-stable (lives in ModState)
    lua.set_function(
      "spawn_enemy",
      [reg, enemies, scripts](float x, float y, const std::string& id, sol::this_state ts) -> sol::object {
          const EnemyDef* def = enemies->by_id(id);
          if (def == nullptr) {
              std::fprintf(stderr, "[mod] spawn_enemy: unknown enemy id '%s'\n", id.c_str());
              return sol::lua_nil;
          }
          return sol::make_object(
            ts, EntityHandle{ .reg = reg, .entity = spawn_enemy(*reg, x, y, *def, def->stats, *scripts) });
      });
}

core::Entity spawn_enemy(core::Registry& reg, float x, float y, const EnemyDef& def, const EnemyStats& stats,
                         ScriptComponentRegistry& scripts)
{
    const core::Entity enemy = create_enemy(reg, x, y, stats, def.wire_id);
    // Builder-declared components go straight through the script-ECS pools —
    // no Lua on the spawn path.
    for (const EnemyComponentInit& init : def.components) {
        ScriptSchema* schema = scripts.by_id(init.component_id);
        if (schema == nullptr || !schema->has_pool) { continue; }
        core::DynamicPool* pool = reg.dynamic_pool(schema->pool_id);
        if (pool == nullptr) { continue; }
        double* row = pool->emplace_default(enemy);
        for (const auto& [name, value] : init.fields) {
            const int field = schema->field_index(name);
            if (field >= 0) { row[field] = value; }
        }
    }
    if (def.on_spawn.valid()) {
        sol::protected_function_result res = def.on_spawn(EntityHandle{ .reg = &reg, .entity = enemy });
        if (!res.valid()) {
            const sol::error err = res;
            std::fprintf(stderr, "[mod] enemy '%s' on_spawn error: %s\n", def.id.c_str(), err.what());
        }
    }
    return enemy;
}

void install_script_systems(LuaHost& host, shared::World& world)
{
    for (ScriptSystem& sys : host.script_systems()) {
        const sol::protected_function fn = sys.fn;
        const std::string id = sys.id;
        const auto log = [id](const sol::protected_function_result& res) {
            if (!res.valid()) {
                const sol::error err = res;
                std::fprintf(stderr, "[mod] system '%s' error: %s\n", id.c_str(), err.what());
            }
        };
        if (sys.rate <= 0.0) {
            world.add_system(sys.order, [fn, log](core::Registry&, float dt) { log(fn(dt)); });
        } else {
            const double interval = 1.0 / sys.rate;
            auto acc = std::make_shared<double>(0.0);
            world.add_system(sys.order, [fn, log, acc, interval](core::Registry&, float dt) {
                *acc += dt;
                if (*acc >= interval) {
                    log(fn(*acc));
                    *acc = 0.0;
                }
            });
        }
    }
}

bool run_available(const ContentDef& def, EntityHandle handle)
{
    // Objects are obtainable once — the engine tracks ownership, so a plugin
    // never has to encode "once" in its own `available`.
    if (def.kind == ContentKind::Object) {
        const ObjectInventory* inv = handle.reg->try_get<ObjectInventory>(handle.entity);
        if (inv != nullptr && inv->owned.test(def.wire_id)) { return false; }
    }
    if (!def.available.valid()) { return true; }
    sol::protected_function_result res = def.available(handle);
    if (!res.valid()) {
        const sol::error err = res;
        std::fprintf(stderr, "[mod] '%s' available() error: %s\n", def.id.c_str(), err.what());
        return false;
    }
    return res.get<bool>();
}

void run_apply(const ContentDef& def, EntityHandle handle, Rarity rarity)
{
    sol::protected_function_result res;
    if (def.kind == ContentKind::StatUpgrade) {
        if (!def.apply.valid()) { return; }
        const float amount = def.rarity_amounts[static_cast<std::size_t>(rarity)];
        res = def.apply(handle, static_cast<int>(rarity), amount);
    } else {
        // Record ownership so this object can't be offered again.
        ObjectInventory* inv = handle.reg->try_get<ObjectInventory>(handle.entity);
        if (inv == nullptr) { inv = &handle.reg->assign(handle.entity, ObjectInventory{}); }
        inv->owned.set(def.wire_id);
        if (!def.acquire.valid()) { return; }
        res = def.acquire(handle);
    }
    if (!res.valid()) {
        const sol::error err = res;
        std::fprintf(stderr, "[mod] '%s' apply/acquire error: %s\n", def.id.c_str(), err.what());
    }
}

} // namespace mod
