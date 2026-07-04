// src/shared/mod/sim_bindings.cpp
#include "shared/mod/sim_bindings.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "shared/components/combat.hpp"
#include "shared/components/gameplay.hpp"
#include "shared/components/physics.hpp"
#include "shared/components/progression.hpp"
#include "shared/factory/enemy.hpp"
#include "shared/mod/script_ecs.hpp"
#include "shared/protocol.hpp" // EntityKind for spawn_bullet
#include "shared/sim/world.hpp"
#include "shared/system/input.hpp" // DASH_COOLDOWN default for the Dash binding

namespace mod {

namespace {

// Register an engine component usertype + its dispatch entry. The table index
// MUST match the component's position in engine_component_names (the prelude
// handles installed by lua_host carry that index as their engine_tag).
template <typename T, typename Builder, typename... Fields>
void register_component(sol::state& lua, BindingTable& table, const char* name, Builder builder,
                        Fields&&... fields)
{
    if (std::string_view(engine_component_names.at(table.size())) != name) {
        std::fprintf(stderr, "[mod] FATAL: engine component '%s' registered out of prelude order\n", name);
    }
    // Hidden metatable name so it doesn't shadow the `name` prelude handle.
    lua.new_usertype<T>(std::string("_ct_") + name, std::forward<Fields>(fields)...);
    table.push_back(ComponentBinding{
      .has = [](core::Registry& r, core::Entity e) { return r.has<T>(e); },
      .get = [](sol::state_view sv, core::Registry& r, core::Entity e) -> sol::object {
          T* p = r.try_get<T>(e);
          return p ? sol::make_object(sv, std::ref(*p)) : sol::lua_nil;
      },
      .assign = [builder](core::Registry& r, core::Entity e, const sol::table& t) {
          if (r.has<T>(e)) { r.remove<T>(e); } // set == replace
          r.assign(e, builder(t));
      },
      .remove = [](core::Registry& r, core::Entity e) { if (r.has<T>(e)) { r.remove<T>(e); } },
      .pool = [](core::Registry& r) { return r.raw_pool(core::type_id<T>()); },
    });
}

// Write-through proxy over a script component's field row. Re-resolves the row
// each access so it survives pool reallocation. STRICT: unknown field names
// raise a Lua error (typos are loud, not silent nils).
struct ScriptFieldProxy
{
    core::DynamicPool* pool;
    core::Entity entity;
    const ScriptSchema* schema;
};

// Fill a script component's row: defaults first, then the given overrides.
// `strict` raises on unknown keys (e:set); the spawn path logs instead (it
// runs on the C++ tick, not under a protected call).
void fill_script_row(double* row, const ScriptSchema& schema, const sol::table& fields, bool strict)
{
    for (std::size_t i = 0; i < schema.defaults.size(); ++i) { row[i] = schema.defaults[i]; }
    for (const auto& [key, value] : fields) {
        if (!key.is<std::string>() || !value.is<double>()) { continue; }
        const std::string name = key.as<std::string>();
        const int field = schema.field_index(name);
        if (field >= 0) {
            row[field] = value.as<double>();
        } else if (strict) {
            throw std::runtime_error("component '" + schema.id + "' has no field '" + name + "'");
        } else {
            std::fprintf(stderr, "[mod] component '%s' has no field '%s' (ignored)\n", schema.id.c_str(),
                         name.c_str());
        }
    }
}

// Resolve a handle to a pool for membership tests, or nullptr (=> empty query).
core::SparseSet* resolve_pool(const ComponentRef& ref, core::Registry& reg, const BindingTable& table)
{
    if (ref.is_engine()) {
        if (ref.engine_tag < static_cast<int>(table.size())) { return table[ref.engine_tag].pool(reg); }
        return nullptr;
    }
    if (ref.schema != nullptr && ref.schema->has_pool) { return reg.dynamic_pool(ref.schema->pool_id); }
    return nullptr;
}

// Reusable per-iterator storage: entity snapshot + membership pools + cursor.
// each/nearby run thousands of times per second inside Lua systems, so their
// buffers are pooled on the world (returned by the shared_ptr deleter when the
// Lua iterator is collected) instead of heap-allocated per call.
struct IterScratch
{
    std::vector<core::Entity> ents;
    std::vector<core::SparseSet*> pools;
    std::size_t idx = 0;
};
using ScratchPool = std::vector<std::unique_ptr<IterScratch>>;

// The `world` facade handed to Lua systems: runtime queries over components.
struct ScriptWorld
{
    core::Registry* reg;
    std::shared_ptr<BindingTable> table;
    sol::state_view lua;
    std::shared_ptr<ScratchPool> scratch = std::make_shared<ScratchPool>();

    // world:each(H, ...) -> iterator over EntityHandles owning all components.
    sol::object each(sol::variadic_args va)
    {
        auto s = acquire();
        gather_pools(s->pools, va, "world:each expects component handles");

        const core::SparseSet* lead = nullptr;
        bool empty = s->pools.empty();
        for (core::SparseSet* p : s->pools) {
            if (p == nullptr) { empty = true; break; }
            if (lead == nullptr || p->size() < lead->size()) { lead = p; }
        }
        if (!empty && lead != nullptr) {
            s->ents.assign(lead->begin(), lead->end()); // snapshot: safe against edits mid-iteration
            std::erase(s->pools, lead);                 // lead membership is implied
        }
        return make_iter(std::move(s));
    }

    // world:nearby(x, y, radius, H, ...) -> iterator over entities within
    // `radius` (center distance) owning all components. Served by the shared
    // spatial hash GridSystem rebuilds each tick — the broad-phase workhorse
    // behind the Lua combat/bullet/pickup systems.
    sol::object nearby(float x, float y, float radius, sol::variadic_args va)
    {
        auto s = acquire();
        const bool empty =
          gather_pools(s->pools, va, "world:nearby expects component handles after (x, y, radius)");

        if (!empty) {
            reg->view<WorldGrid>().each([&](core::Entity, WorldGrid& world_grid) {
                world_grid.grid.query(x - radius, y - radius, x + radius, y + radius, s->ents);
            });
            // Distance filter here (cheap, C++); membership filter in the iterator.
            const float r2 = radius * radius;
            std::erase_if(s->ents, [&](core::Entity e) {
                const Position* pos = reg->try_get<Position>(e);
                if (pos == nullptr) { return true; }
                const float dx = pos->x - x;
                const float dy = pos->y - y;
                return (dx * dx) + (dy * dy) > r2;
            });
        }
        return make_iter(std::move(s));
    }

    // world:closest(x, y, H, ..., { without = H }) -> nearest entity owning all
    // handles, its d² and its position (entity, d2, px, py), or nil. One
    // boundary crossing replaces the "iterate all candidates from Lua" pattern
    // that dominated tick time (the per-enemy nearest-player scan); returning
    // the position spares callers a get(Position) proxy per hit. Entities
    // without Position are ignored; `without` (a handle or array of handles)
    // excludes owners, e.g. Downed players.
    std::tuple<sol::object, sol::object, sol::object, sol::object> closest(float x, float y,
                                                                           sol::variadic_args va)
    {
        // Called per entity from hot systems (targeting runs it per enemy) —
        // everything lives on the stack, no heap, no scratch acquisition.
        constexpr std::size_t max_handles = 8;
        std::array<core::SparseSet*, max_handles> pools{};
        std::array<core::SparseSet*, max_handles> excludes{};
        std::size_t pool_count = 0;
        std::size_t exclude_count = 0;
        for (const sol::stack_proxy arg : va) {
            if (arg.is<ComponentRef>() && pool_count < max_handles) {
                pools[pool_count++] = resolve_pool(arg.as<ComponentRef>(), *reg, *table);
            } else if (arg.is<sol::table>()) {
                const sol::object without = arg.as<sol::table>()["without"];
                if (without.is<ComponentRef>() && exclude_count < max_handles) {
                    excludes[exclude_count++] = resolve_pool(without.as<ComponentRef>(), *reg, *table);
                } else if (without.is<sol::table>()) {
                    for (const auto& [_, v] : without.as<sol::table>()) {
                        if (v.is<ComponentRef>() && exclude_count < max_handles) {
                            excludes[exclude_count++] =
                              resolve_pool(v.as<ComponentRef>(), *reg, *table);
                        }
                    }
                }
            } else {
                throw std::runtime_error(
                  "world:closest expects component handles (+ optional { without = H })");
            }
        }
        const auto miss = [] {
            return std::tuple<sol::object, sol::object, sol::object, sol::object>{
                sol::lua_nil, sol::lua_nil, sol::lua_nil, sol::lua_nil
            };
        };

        const core::SparseSet* lead = nullptr;
        for (std::size_t i = 0; i < pool_count; ++i) {
            if (pools[i] == nullptr) { return miss(); }
            if (lead == nullptr || pools[i]->size() < lead->size()) { lead = pools[i]; }
        }
        if (lead == nullptr) { return miss(); }

        core::Entity best{};
        Position best_pos{};
        float best_d2 = std::numeric_limits<float>::max();
        bool found = false;
        for (const core::Entity e : *lead) {
            bool ok = true;
            for (std::size_t i = 0; i < pool_count && ok; ++i) {
                ok = pools[i] == lead || pools[i]->contains(e);
            }
            for (std::size_t i = 0; i < exclude_count && ok; ++i) {
                ok = excludes[i] == nullptr || !excludes[i]->contains(e);
            }
            if (!ok) { continue; }
            const Position* pos = reg->try_get<Position>(e);
            if (pos == nullptr) { continue; }
            const float dx = pos->x - x;
            const float dy = pos->y - y;
            const float d2 = (dx * dx) + (dy * dy);
            if (d2 < best_d2) {
                best_d2 = d2;
                best = e;
                best_pos = *pos;
                found = true;
            }
        }
        if (!found) { return miss(); }
        return { sol::make_object(lua, EntityHandle{ .reg = reg, .entity = best }),
                 sol::make_object(lua, best_d2), sol::make_object(lua, best_pos.x),
                 sol::make_object(lua, best_pos.y) };
    }

private:
    // Returns true if any handle resolved to no pool (query can't match).
    bool gather_pools(std::vector<core::SparseSet*>& pools, sol::variadic_args va, const char* err)
    {
        bool empty = false;
        for (const sol::stack_proxy arg : va) {
            if (!arg.is<ComponentRef>()) { throw std::runtime_error(err); }
            core::SparseSet* p = resolve_pool(arg.as<ComponentRef>(), *reg, *table);
            if (p == nullptr) { empty = true; }
            pools.push_back(p);
        }
        return empty;
    }

    [[nodiscard]] std::shared_ptr<IterScratch> acquire()
    {
        std::shared_ptr<ScratchPool> pool = scratch;
        std::unique_ptr<IterScratch> owned;
        if (!pool->empty()) {
            owned = std::move(pool->back());
            pool->pop_back();
        } else {
            owned = std::make_unique<IterScratch>();
        }
        IterScratch* raw = owned.release();
        raw->ents.clear(); // keep capacity — that's the whole point
        raw->pools.clear();
        raw->idx = 0;
        return { raw, [pool](IterScratch* p) { pool->emplace_back(p); } };
    }

    sol::object make_iter(std::shared_ptr<IterScratch> s)
    {
        core::Registry* rr = reg;
        sol::state_view sv = lua;
        std::function<sol::object(sol::variadic_args)> iter =
          [rr, s, sv](sol::variadic_args) -> sol::object {
              while (s->idx < s->ents.size()) {
                  const core::Entity e = s->ents[s->idx++];
                  if (!rr->valid(e)) { continue; }
                  bool ok = true;
                  for (core::SparseSet* p : s->pools) {
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
    host.scripts().bind(&world_reg); // mod:component allocates pools in this registry
    auto table = std::make_shared<BindingTable>();
    host.state().bindings = table; // spawn path + game server dispatch through this

    // Kernel components, in engine_component_names order (= prelude tags).
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
      "cooldown_max", &Dash::cooldown_max, "cooldown", &Dash::cooldown,
      "burst_remaining", &Dash::burst_remaining, "shockwave", &Dash::shockwave,
      "charges", &Dash::charges, "max_charges", &Dash::max_charges);
    register_component<XpReward>(lua, *table, "XpReward",
      [](const sol::table& t) {
          return XpReward{ .value = static_cast<std::uint32_t>(t.get_or("value", 1)) };
      },
      "value", &XpReward::value);
    register_component<Render>(lua, *table, "Render",
      [](const sol::table& t) {
          return Render{ .kind = static_cast<std::uint8_t>(t.get_or("kind", 0)),
                         .variant = static_cast<std::uint8_t>(t.get_or("variant", 0)) };
      },
      "kind", &Render::kind, "variant", &Render::variant);
    register_component<Downed>(lua, *table, "Downed",
      [](const sol::table& t) {
          return Downed{ .respawn_wave = static_cast<std::uint16_t>(t.get_or("respawn_wave", 0)) };
      },
      "respawn_wave", &Downed::respawn_wave);
    // Tag components — no fields; used for membership in queries (`Enemy`, `Player`).
    register_component<EnemyTag>(lua, *table, "Enemy", [](const sol::table&) { return EnemyTag{}; });
    register_component<PlayerTag>(lua, *table, "Player", [](const sol::table&) { return PlayerTag{}; });

    // Write-through proxy for script component fields (strict on unknowns).
    lua.new_usertype<ScriptFieldProxy>(
      "_ScriptFieldProxy", sol::no_constructor,
      sol::meta_function::index,
      [](ScriptFieldProxy& p, const std::string& key, sol::this_state ts) -> sol::object {
          const int i = p.schema->field_index(key);
          if (i < 0) {
              throw std::runtime_error("component '" + p.schema->id + "' has no field '" + key + "'");
          }
          double* row = p.pool->row(p.entity);
          return row ? sol::make_object(ts, row[i]) : sol::lua_nil;
      },
      sol::meta_function::new_index, [](ScriptFieldProxy& p, const std::string& key, double v) {
          const int i = p.schema->field_index(key);
          if (i < 0) {
              throw std::runtime_error("component '" + p.schema->id + "' has no field '" + key + "'");
          }
          if (double* row = p.pool->row(p.entity)) { row[i] = v; }
      });

    // Entity handle: one verb set, handles only.
    lua.new_usertype<EntityHandle>(
      "Entity", sol::no_constructor,
      "has", [table](EntityHandle& h, const ComponentRef& ref) {
          if (ref.is_engine()) {
              return ref.engine_tag < static_cast<int>(table->size())
                     && (*table)[ref.engine_tag].has(*h.reg, h.entity);
          }
          if (ref.schema == nullptr || !ref.schema->has_pool) { return false; }
          core::DynamicPool* p = h.reg->dynamic_pool(ref.schema->pool_id);
          return p != nullptr && p->row(h.entity) != nullptr;
      },
      "get", [table](EntityHandle& h, const ComponentRef& ref, sol::this_state ts) -> sol::object {
          if (ref.is_engine()) {
              if (ref.engine_tag >= static_cast<int>(table->size())) { return sol::lua_nil; }
              return (*table)[ref.engine_tag].get(ts, *h.reg, h.entity);
          }
          if (ref.schema == nullptr || !ref.schema->has_pool) { return sol::lua_nil; }
          core::DynamicPool* p = h.reg->dynamic_pool(ref.schema->pool_id);
          if (p == nullptr || p->row(h.entity) == nullptr) { return sol::lua_nil; }
          return sol::make_object(ts, ScriptFieldProxy{ .pool = p, .entity = h.entity, .schema = ref.schema });
      },
      "set", [table](EntityHandle& h, const ComponentRef& ref, const sol::table& fields) {
          if (ref.is_engine()) {
              if (ref.engine_tag < static_cast<int>(table->size())) {
                  (*table)[ref.engine_tag].assign(*h.reg, h.entity, fields);
              }
              return;
          }
          if (ref.schema == nullptr || !ref.schema->has_pool) { return; }
          core::DynamicPool* p = h.reg->dynamic_pool(ref.schema->pool_id);
          if (p == nullptr) { return; }
          fill_script_row(p->emplace_default(h.entity), *ref.schema, fields, /*strict=*/true);
      },
      "remove", [table](EntityHandle& h, const ComponentRef& ref) {
          if (ref.is_engine()) {
              if (ref.engine_tag < static_cast<int>(table->size())) {
                  (*table)[ref.engine_tag].remove(*h.reg, h.entity);
              }
              return;
          }
          if (ref.schema != nullptr && ref.schema->has_pool) {
              if (core::DynamicPool* p = h.reg->dynamic_pool(ref.schema->pool_id)) { p->remove(h.entity); }
          }
      },
      "destroy", [](EntityHandle& h) {
          if (h.reg->valid(h.entity)) { h.reg->destroy(h.entity); }
      });

    // The `world` query facade + engine services.
    core::Registry* reg = &world_reg;
    lua.new_usertype<ScriptWorld>("_ScriptWorld", sol::no_constructor,
                                  "each", &ScriptWorld::each,
                                  "nearby", &ScriptWorld::nearby,
                                  "closest", &ScriptWorld::closest,
                                  "wave", [reg](ScriptWorld&) {
                                      int wave = 1;
                                      reg->view<GameStats>().each(
                                        [&](core::Entity, const GameStats& stats) { wave = stats.wave; });
                                      return wave;
                                  },
                                  "add_xp", [reg](ScriptWorld&, int value) {
                                      reg->view<GameStats>().each([&](core::Entity, GameStats& stats) {
                                          stats.xp += static_cast<std::uint32_t>(std::max(0, value));
                                      });
                                  });
    lua["world"] = ScriptWorld{ .reg = &world_reg, .table = table, .lua = lua };

    // Kernel spawn primitives. Bullets are kinetic + drawable; their behavior
    // (damage, lifetime, allegiance) is a Lua component the caller attaches.
    lua.set_function("spawn_bullet", [reg](float x, float y, float vx, float vy) {
        const core::Entity bullet = reg->create();
        reg->assign(bullet, Position{ .x = x, .y = y });
        reg->assign(bullet, PrevPosition{ .x = x, .y = y });
        reg->assign(bullet, Velocity{ .dx = vx, .dy = vy });
        reg->assign(bullet, Radius{ .value = 4 });
        reg->assign(bullet, Render{ .kind = static_cast<std::uint8_t>(proto::EntityKind::Projectile),
                                    .variant = 0 });
        return EntityHandle{ .reg = reg, .entity = bullet };
    });
    // A bare drawable entity (drops, markers): position it, set(Render, ...),
    // attach your components.
    lua.set_function("spawn_entity", [reg](float x, float y) {
        const core::Entity entity = reg->create();
        reg->assign(entity, Position{ .x = x, .y = y });
        reg->assign(entity, PrevPosition{ .x = x, .y = y });
        reg->assign(entity, Render{ .kind = 0, .variant = 0 });
        return EntityHandle{ .reg = reg, .entity = entity };
    });
    const EnemyRegistry* enemies = &host.enemies(); // heap-stable (lives in ModState)
    lua.set_function(
      "spawn_enemy",
      [reg, enemies, table](float x, float y, const std::string& id, sol::this_state ts) -> sol::object {
          const EnemyDef* def = enemies->by_id(id);
          if (def == nullptr) {
              std::fprintf(stderr, "[mod] spawn_enemy: unknown enemy id '%s'\n", id.c_str());
              return sol::lua_nil;
          }
          std::uint16_t wave = 1;
          reg->view<GameStats>().each([&](core::Entity, const GameStats& stats) { wave = stats.wave; });
          const auto inits = def->inits_at(wave);
          return sol::make_object(
            ts, EntityHandle{ .reg = reg, .entity = spawn_enemy(*reg, x, y, *def, inits, *table) });
      });
}

core::Entity spawn_enemy(core::Registry& reg, float x, float y, const EnemyDef& def,
                         const std::vector<std::pair<const ComponentRef*, sol::table>>& inits,
                         const BindingTable& bindings)
{
    const core::Entity enemy = create_enemy(reg, x, y, def.wire_id);
    // The archetype's component bag — kernel comps dispatch through the binding
    // table, Lua comps go straight into the script-ECS pools. No Lua calls here.
    for (const auto& [ref, fields] : inits) {
        if (ref->is_engine()) {
            if (ref->engine_tag < static_cast<int>(bindings.size())) {
                bindings[ref->engine_tag].assign(reg, enemy, fields);
            }
            continue;
        }
        if (ref->schema == nullptr || !ref->schema->has_pool) { continue; }
        core::DynamicPool* pool = reg.dynamic_pool(ref->schema->pool_id);
        if (pool == nullptr) { continue; }
        fill_script_row(pool->emplace_default(enemy), *ref->schema, fields, /*strict=*/false);
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
            // `stagger` pre-fills the accumulator so same-rate systems fire on
            // DIFFERENT ticks — without it every 30 Hz system lands on one tick
            // and that aligned tick busts the frame budget. The first firing
            // sees a slightly inflated dt (the pre-fill); harmless at t≈0.
            auto acc = std::make_shared<double>(interval * std::clamp(sys.stagger, 0.0, 1.0));
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
