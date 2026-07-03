// src/shared/mod/lua_host.cpp
#include "shared/mod/lua_host.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "shared/protocol.hpp"  // protocol_version (plugin-hash seed)
#include "shared/sim/world.hpp" // shared::phase constants

namespace mod {

namespace {

// FNV-1a 64 accumulator for the plugin-set hash. Strings are folded with their
// terminating NUL so ("ab","c") and ("a","bc") can't collide.
struct Fnv1a
{
    std::uint64_t hash = 14695981039346656037ULL;

    void byte(std::uint8_t value) noexcept
    {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    void str(const std::string& s) noexcept
    {
        for (const char c : s) { byte(static_cast<std::uint8_t>(c)); }
        byte(0);
    }
};

// Run a plugin's main() on demand (memoized). This is what import() calls and
// what pass 2 of load_dir drives — require-style lazy loading with cycle
// detection; the plugin's exports are whatever its main() returned.
sol::object run_plugin(ModState* st, const std::string& name)
{
    if (const auto done = st->exports.find(name); done != st->exports.end()) { return done->second; }
    const auto pending = st->pending.find(name);
    if (pending == st->pending.end()) {
        std::fprintf(stderr, "[mod] import('%s'): no such plugin in mods/\n", name.c_str());
        return sol::lua_nil;
    }
    for (const std::string& loading : st->import_stack) {
        if (loading == name) {
            std::fprintf(stderr, "[mod] circular import of '%s' (via '%s')\n", name.c_str(),
                         st->import_stack.back().c_str());
            return sol::lua_nil;
        }
    }

    st->import_stack.push_back(name);
    const std::string saved_dir = std::exchange(st->current_dir, pending->second.dir);
    const std::string saved_plugin = std::exchange(st->current_plugin, name);

    sol::object result = sol::lua_nil;
    sol::protected_function_result res = pending->second.main();
    if (res.valid()) {
        result = res.get<sol::object>();
    } else {
        const sol::error err = res;
        std::fprintf(stderr, "[mod] '%s' main() error: %s\n", name.c_str(), err.what());
    }

    st->current_dir = saved_dir;
    st->current_plugin = saved_plugin;
    st->import_stack.pop_back();
    st->exports.emplace(name, result); // memoize (even a nil/failed load — no retry storms)
    return result;
}

// Handle returned by mod:enemy — attaches components (kernel or Lua-defined,
// static table or fun(wave) for per-wave scaling) to every spawned instance:
//   mod:enemy("slinger", "Slinger", {...})
//       :component(Health, function(wave) return { current = h, max = h } end)
//       :component(Ranged, { range = 340 })
struct EnemyBuilder
{
    ModState* state = nullptr;
    std::string id;

    EnemyBuilder component(const ComponentRef& ref, const sol::object& init)
    {
        EnemyDef* def = state->enemies.find(id);
        if (def == nullptr) { return *this; }
        if (!ref.valid()) {
            std::fprintf(stderr, "[mod] enemy '%s': :component() got an invalid handle\n", id.c_str());
            return *this;
        }
        if (!init.is<sol::table>() && !init.is<sol::protected_function>()) {
            std::fprintf(stderr, "[mod] enemy '%s': %s init must be a table or fun(wave)\n", id.c_str(),
                         ref.id.c_str());
            return *this;
        }
        def->components.push_back(EnemyComponentInit{ .ref = ref, .init = init });
        return *this;
    }
};

// The `Mod` handle plugins get from register_mod(). Holds a stable pointer to
// the host's ModState (survives a LuaHost move) plus the mod's namespace.
// Every registered name is auto-prefixed "ns:" — plugins never write full ids.
struct ModHandle
{
    ModState* state = nullptr;
    std::string ns;

    [[nodiscard]] std::string qualify(const std::string& name) const
    {
        if (name.find(':') != std::string::npos) {
            std::fprintf(stderr, "[mod] '%s': name '%s' contains ':' — names are auto-namespaced\n",
                         ns.c_str(), name.c_str());
            return name;
        }
        return ns + ":" + name;
    }

    static void read_amounts(ContentDef& d, const sol::table& amounts)
    {
        // Lua tables are 1-based. Missing entries stay 0 = "not offered at
        // that tier" (e.g. { 5, 10, 15 } exists only at Common/Uncommon/Rare).
        for (std::uint8_t r = 0; r < rarity_count; ++r) {
            d.rarity_amounts[r] = amounts.get_or(r + 1, 0.0f);
        }
    }

    // Object rarity opt: "common" | "uncommon" | "rare" | "epic" | "legendary".
    static Rarity parse_rarity(const std::string& name, const std::string& id)
    {
        if (name == "common") { return Rarity::Common; }
        if (name == "uncommon") { return Rarity::Uncommon; }
        if (name == "rare") { return Rarity::Rare; }
        if (name == "epic") { return Rarity::Epic; }
        if (name == "legendary") { return Rarity::Legendary; }
        std::fprintf(stderr, "[mod] '%s' unknown rarity '%s' — using epic\n", id.c_str(), name.c_str());
        return Rarity::Epic;
    }

    // Define a component: fields with defaults ({ range = 340, timer = 0 }).
    // Returns THE handle — store it, share it via exports; no string ids.
    // Field order is the sorted field-name order (deterministic on every
    // process, required for the networked wire layout).
    ComponentRef component(const std::string& name, const sol::table& fields, sol::optional<sol::table> opts)
    {
        std::string id = qualify(name);
        std::vector<std::pair<std::string, double>> decl;
        for (const auto& [key, value] : fields) {
            if (key.is<std::string>() && value.is<double>()) {
                decl.emplace_back(key.as<std::string>(), value.as<double>());
            } else {
                std::fprintf(stderr, "[mod] component '%s': fields must be name = number-default\n",
                             id.c_str());
            }
        }
        std::sort(decl.begin(), decl.end());
        std::vector<std::string> names;
        std::vector<double> defaults;
        names.reserve(decl.size());
        defaults.reserve(decl.size());
        for (auto& [field, def_value] : decl) {
            names.push_back(std::move(field));
            defaults.push_back(def_value);
        }
        const bool networked = opts ? opts->get_or("networked", false) : false;
        ScriptSchema* schema = state->scripts.define(id, std::move(names), std::move(defaults), networked);
        return ComponentRef{ .engine_tag = -1, .schema = schema, .id = std::move(id) };
    }

    // Define a system: fn(dt) run each tick at a phase, optionally throttled.
    void system(const std::string& name, sol::table opts, sol::protected_function fn)
    {
        const std::string phase = opts.get_or<std::string>("phase", "update");
        const int order = (phase == "motion") ? shared::phase::Motion : shared::phase::Update;
        const double rate = opts.get_or("rate", 0.0);
        state->script_systems.push_back(
          { .id = qualify(name), .order = order, .rate = rate, .fn = std::move(fn) });
    }

    void upgrade(const std::string& name, std::string label, const sol::table& amounts,
                 sol::protected_function apply, sol::optional<sol::table> opts)
    {
        ContentDef d;
        d.kind = ContentKind::StatUpgrade;
        d.id = qualify(name);
        d.label = std::move(label);
        read_amounts(d, amounts);
        d.apply = std::move(apply);
        if (opts) {
            d.available = opts->get_or<sol::protected_function>("available", {});
            d.sprite = opts->get_or<std::string>("sprite", "");
            d.value_format = opts->get_or<std::string>("value_format", "");
            d.value_fn = opts->get_or<sol::protected_function>("value_text", {});
        }
        state->registry.add(std::move(d));
    }

    void object(const std::string& name, std::string label, sol::protected_function acquire,
                sol::optional<sol::table> opts)
    {
        ContentDef d;
        d.kind = ContentKind::Object;
        d.id = qualify(name);
        d.label = std::move(label);
        d.acquire = std::move(acquire);
        if (opts) {
            d.available = opts->get_or<sol::protected_function>("available", {});
            d.sprite = opts->get_or<std::string>("sprite", "");
            d.value_fn = opts->get_or<sol::protected_function>("value_text", {});
            d.draw = opts->get_or<sol::protected_function>("draw", {});
            const std::string rarity = opts->get_or<std::string>("rarity", "");
            if (!rarity.empty()) { d.object_rarity = parse_rarity(rarity, d.id); }
        }
        state->registry.add(std::move(d));
    }

    // Register an enemy archetype. Pure component bag: chain :component(...)
    // for everything gameplay-defining (Health, Speed, Damage, Radius, ...).
    EnemyBuilder enemy(const std::string& name, std::string label, sol::optional<sol::table> opts)
    {
        EnemyDef d;
        d.id = qualify(name);
        d.label = std::move(label);
        if (opts) {
            const sol::object weight = (*opts)["weight"];
            if (weight.is<sol::protected_function>()) {
                d.weight_fn = weight.as<sol::protected_function>();
            } else if (weight.is<double>()) {
                d.weight = weight.as<float>();
            }
            d.scale = opts->get_or("scale", 1.0f);
            if (const sol::optional<sol::table> tint = (*opts)["tint"]) {
                for (std::size_t c = 0; c < d.tint.size(); ++c) {
                    d.tint[c] = static_cast<std::uint8_t>(tint->get_or(c + 1, 255));
                }
            }
            d.sprite = opts->get_or<std::string>("sprite", "");
            d.on_spawn = opts->get_or<sol::protected_function>("on_spawn", {});
        }
        std::string id = d.id;
        state->enemies.add(std::move(d));
        return EnemyBuilder{ .state = state, .id = std::move(id) };
    }

    void subscribe(std::string event, sol::protected_function handler)
    {
        state->events.subscribe(std::move(event), std::move(handler));
    }

    // Fire a custom event other plugins can subscribe to. Bare names are
    // auto-namespaced ("boss_spawned" -> "core:boss_spawned").
    void emit(const std::string& event, sol::variadic_args args)
    {
        state->events.emit_variadic(qualify(event), args);
    }
};

} // namespace

LuaHost::LuaHost()
{
    lua_.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
    // Generational GC keeps per-frame collection cheap (short-lived garbage from
    // draw hooks is reclaimed in minor cycles).
    lua_.change_gc_mode_generational(0, 0);
    install_registration_api();
}

void LuaHost::install_registration_api()
{
    // Capture the heap-stable ModState pointer (NOT `this`) so closures/handles
    // survive a move of the LuaHost.
    ModState* st = state_.get();

    // The universal component handle (created by the prelude + mod:component).
    lua_.new_usertype<ComponentRef>("Component", sol::no_constructor,
                                    "id", sol::readonly(&ComponentRef::id));

    // Engine prelude: kernel components as handles, same names in BOTH VMs
    // (the render VM only stores them — e.g. in enemy builders — never
    // dispatches; the sim VM's BindingTable is built in this exact order).
    for (std::size_t i = 0; i < engine_component_names.size(); ++i) {
        const char* name = engine_component_names[i];
        lua_[name] = ComponentRef{ .engine_tag = static_cast<int>(i), .schema = nullptr, .id = name };
    }

    lua_.new_usertype<EnemyBuilder>(
      "EnemyArchetype", sol::no_constructor,
      "component", &EnemyBuilder::component);

    lua_.new_usertype<ModHandle>(
      "Mod", sol::no_constructor,
      "component", &ModHandle::component,
      "system", &ModHandle::system,
      "upgrade", &ModHandle::upgrade,
      "object", &ModHandle::object,
      "enemy", &ModHandle::enemy,
      "subscribe", &ModHandle::subscribe,
      "emit", &ModHandle::emit);

    // include("file.lua") runs a file relative to the current mod's folder and
    // returns its value — lets a plugin split itself across files.
    lua_.set_function("include", [st](const std::string& rel, sol::this_state ts) -> sol::object {
        sol::state_view lua(ts);
        const std::string path = st->current_dir + "/" + rel;
        sol::protected_function_result res = lua.safe_script_file(path, sol::script_pass_on_error);
        if (!res.valid()) {
            const sol::error err = res;
            std::fprintf(stderr, "[mod] include('%s') error: %s\n", rel.c_str(), err.what());
            return sol::lua_nil;
        }
        return res.get<sol::object>();
    });

    // import("core") -> that plugin's exports (its main()'s return value),
    // loading it on demand. THE cross-plugin mechanism: a components-library
    // plugin exports handles; dependents import them. Load order solves itself.
    lua_.set_function("import", [st](const std::string& name) { return run_plugin(st, name); });

    lua_.set_function("register_mod",
                      [st](std::string ns, sol::optional<std::string> description,
                           sol::optional<std::string> author) {
                          if (!st->current_plugin.empty() && ns != st->current_plugin) {
                              std::fprintf(stderr,
                                           "[mod] plugin folder '%s' declares namespace '%s' — the "
                                           "folder name is the import name; keep them identical\n",
                                           st->current_plugin.c_str(), ns.c_str());
                          }
                          std::fprintf(stdout, "[mod] loaded '%s' — %s by %s\n", ns.c_str(),
                                       description.value_or("(no description)").c_str(),
                                       author.value_or("(unknown)").c_str());
                          return ModHandle{ .state = st, .ns = std::move(ns) };
                      });
}

void LuaHost::load_dir(const std::string& dir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::fprintf(stderr, "[mod] no mods directory '%s' — no content loaded\n", dir.c_str());
        finalize_content();
        return;
    }

    // Pass 1: run every entry script (it just defines main()); folder name =
    // plugin/import name. Sorted for stable logs; wire ids come from id sort,
    // and Lua-visible load order from import(), so path order decides nothing.
    std::vector<fs::path> entries;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory()) { continue; }
        fs::path script = e.path() / "mod.lua";
        if (fs::exists(script)) { entries.push_back(script); }
    }
    std::sort(entries.begin(), entries.end());

    for (const fs::path& script : entries) {
        const std::string plugin = script.parent_path().filename().string();
        state_->current_dir = script.parent_path().string(); // for include() at file scope
        sol::protected_function_result loaded =
          lua_.safe_script_file(script.string(), sol::script_pass_on_error);
        if (!loaded.valid()) {
            const sol::error err = loaded;
            std::fprintf(stderr, "[mod] error loading %s: %s\n", script.string().c_str(), err.what());
            continue;
        }
        sol::protected_function main = lua_["main"];
        if (!main.valid()) {
            std::fprintf(stderr, "[mod] %s has no global main() — skipped\n", script.string().c_str());
            continue;
        }
        state_->pending.emplace(plugin,
                                PendingPlugin{ .main = main, .dir = script.parent_path().string() });
        state_->plugin_order.push_back(plugin);
        lua_["main"] = sol::lua_nil; // don't leak this mod's main() into the next
    }

    // Pass 2: run every main() (import() may already have pulled some in).
    for (const std::string& plugin : state_->plugin_order) { run_plugin(state_.get(), plugin); }

    finalize_content();
}

void LuaHost::finalize_content()
{
    state_->registry.finalize();
    state_->enemies.finalize();
    state_->scripts.finalize();
    compute_plugin_hash();
}

// Hash the registered identity — everything the wire depends on (content and
// enemy wire-id maps, networked schema layouts) — NOT Lua behavior. Registries
// are already finalized (sorted); schemas are hashed in sorted-by-id order
// since the deque keeps load order.
void LuaHost::compute_plugin_hash()
{
    Fnv1a fnv;
    fnv.byte(static_cast<std::uint8_t>(proto::protocol_version & 0xFF));
    fnv.byte(static_cast<std::uint8_t>(proto::protocol_version >> 8));

    for (const ContentDef& def : state_->registry.defs()) {
        fnv.str(def.id);
        fnv.byte(static_cast<std::uint8_t>(def.kind));
    }
    for (const EnemyDef& def : state_->enemies.defs()) { fnv.str(def.id); }

    std::vector<const ScriptSchema*> schemas;
    for (const ScriptSchema& schema : state_->scripts.all()) { schemas.push_back(&schema); }
    std::sort(schemas.begin(), schemas.end(),
              [](const ScriptSchema* a, const ScriptSchema* b) { return a->id < b->id; });
    for (const ScriptSchema* schema : schemas) {
        fnv.str(schema->id);
        for (const std::string& field : schema->fields) { fnv.str(field); }
        fnv.byte(schema->networked ? 1 : 0);
    }
    plugin_hash_ = fnv.hash;
}

} // namespace mod
