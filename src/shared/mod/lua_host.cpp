// src/shared/mod/lua_host.cpp
#include "shared/mod/lua_host.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
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

// The `Mod` handle plugins get from register_mod(). Holds a stable pointer to
// the host's ModState (survives a LuaHost move) plus the mod's namespace.
struct ModHandle
{
    ModState* state = nullptr;
    std::string ns;

    // Warn (but proceed) if an id isn't namespaced under this mod.
    void check_ns(const std::string& id) const
    {
        if (id.empty()) {
            std::fprintf(stderr, "[mod] '%s' registered content with an empty id\n", ns.c_str());
        } else if (id.rfind(ns + ":", 0) != 0) {
            std::fprintf(stderr, "[mod] '%s' id '%s' is not namespaced '%s:' — collision risk\n",
                         ns.c_str(), id.c_str(), ns.c_str());
        }
    }

    static void read_amounts(ContentDef& d, const sol::table& amounts)
    {
        for (std::uint8_t r = 0; r < rarity_count; ++r) {
            d.rarity_amounts[r] = amounts.get_or(r + 1, 0.0f); // Lua tables are 1-based
        }
    }

    void add_stat_upgrade(std::string id, std::string label, sol::table amounts,
                          sol::protected_function apply, sol::optional<sol::table> opts)
    {
        check_ns(id);
        ContentDef d;
        d.kind = ContentKind::StatUpgrade;
        d.id = std::move(id);
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

    void add_object(std::string id, std::string label, sol::protected_function acquire,
                    sol::optional<sol::table> opts)
    {
        check_ns(id);
        ContentDef d;
        d.kind = ContentKind::Object;
        d.id = std::move(id);
        d.label = std::move(label);
        d.acquire = std::move(acquire);
        if (opts) {
            d.available = opts->get_or<sol::protected_function>("available", {});
            d.sprite = opts->get_or<std::string>("sprite", "");
            d.value_fn = opts->get_or<sol::protected_function>("value_text", {});
            d.draw = opts->get_or<sol::protected_function>("draw", {});
        }
        state->registry.add(std::move(d));
    }

    // Register an enemy archetype: stats drive the sim, scale/tint/sprite the
    // render VM, weight the wave spawner. Both VMs parse the same call.
    void add_enemy(std::string id, std::string label, sol::table stats, sol::optional<sol::table> opts)
    {
        check_ns(id);
        EnemyDef d;
        d.id = std::move(id);
        d.label = std::move(label);
        d.stats.health = stats.get_or("health", 1.0f);
        d.stats.speed = stats.get_or("speed", 100.0f);
        d.stats.damage = stats.get_or("damage", 0.0f);
        d.stats.radius = stats.get_or("radius", 10.0f);
        d.stats.xp = static_cast<std::uint32_t>(stats.get_or("xp", 1));
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
        state->enemies.add(std::move(d));
    }

    void subscribe(std::string event, sol::protected_function handler)
    {
        state->events.subscribe(std::move(event), std::move(handler));
    }

    // Define a Lua component: a named list of double fields, optionally networked
    // (synced to clients for rendering).
    void define_component(std::string id, sol::table fields, sol::optional<sol::table> opts)
    {
        check_ns(id);
        std::vector<std::string> names;
        for (std::size_t i = 1; i <= fields.size(); ++i) {
            names.push_back(fields.get_or(i, std::string{}));
        }
        const bool networked = opts ? opts->get_or("networked", false) : false;
        state->scripts.define(std::move(id), std::move(names), networked);
    }

    // Define a Lua system: fn(dt) run each tick at a phase, optionally throttled.
    void define_system(std::string id, sol::table opts, sol::protected_function fn)
    {
        check_ns(id);
        const std::string phase = opts.get_or<std::string>("phase", "update");
        const int order = (phase == "motion") ? shared::phase::Motion : shared::phase::Update;
        const double rate = opts.get_or("rate", 0.0);
        state->script_systems.push_back(
          { .id = std::move(id), .order = order, .rate = rate, .fn = std::move(fn) });
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

    lua_.new_usertype<ModHandle>(
      "Mod", sol::no_constructor,
      "add_stat_upgrade", &ModHandle::add_stat_upgrade,
      "add_object", &ModHandle::add_object,
      "add_enemy", &ModHandle::add_enemy,
      "subscribe", &ModHandle::subscribe,
      "define_component", &ModHandle::define_component,
      "define_system", &ModHandle::define_system);

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

    lua_.set_function("register_mod",
                      [st](std::string ns, sol::optional<std::string> description,
                           sol::optional<std::string> author) {
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
        state_->registry.finalize();
        state_->enemies.finalize();
        compute_plugin_hash();
        return;
    }

    // Load each mod's entry script sorted by path (stable logs; wire ids come
    // from id sort, so load order doesn't affect them).
    std::vector<fs::path> entries;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory()) { continue; }
        fs::path script = e.path() / "mod.lua";
        if (fs::exists(script)) { entries.push_back(script); }
    }
    std::sort(entries.begin(), entries.end());

    for (const fs::path& script : entries) {
        state_->current_dir = script.parent_path().string(); // for include()
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
        sol::protected_function_result res = main();
        if (!res.valid()) {
            const sol::error err = res;
            std::fprintf(stderr, "[mod] %s main() error: %s\n", script.string().c_str(), err.what());
        }
        lua_["main"] = sol::lua_nil; // don't leak this mod's main() into the next
    }

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
