// Standalone SDL-free sim test: boots the exact server pipeline (kernel world
// + mods/core Lua) headless, spawns scenarios, steps at 120 Hz and asserts on
// world state THROUGH the sim VM (import("core") reaches the mod's handles).
//
// Build (from the repo root, after a normal cmake build supplied the deps):
//   g++ -std=c++26 -O1 -freflection -fcontracts -DSOL_NO_LUA_HPP=1 -DSOL_SAFE_FUNCTION=1 \
//       -I src -isystem build/_deps/sol2-src/include -isystem build/_deps/lua-src \
//       -isystem build/_deps/fmt-src/include \
//       tests/sim_test.cpp src/shared/mod/lua_host.cpp src/shared/mod/registry.cpp \
//       src/shared/mod/sim_bindings.cpp build/liblua.a build/_deps/fmt-build/libfmt.a \
//       -o /tmp/sim_test
// Run from the repo root (load_dir("mods") is a relative path).
#include "shared/factory/player.hpp"
#include "shared/map/terrain.hpp"
#include "shared/mod/lua_host.hpp"
#include "shared/mod/sim_bindings.hpp"
#include "shared/sim/game_world.hpp"
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {
int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { ++failures; }
}
} // namespace

int main()
{
    // LuaHost must OUTLIVE the World: the script systems registered into the
    // World's pipeline hold sol references that unref on destruction.
    mod::LuaHost host;
    shared::World world = shared::make_game_world();
    mod::install_sim_bindings(host, world.registry());
    host.load_dir("mods");
    mod::install_script_systems(host, world);
    if (host.enemies().count() == 0) {
        std::printf("FAIL  mods/core did not load (run from the repo root)\n");
        return 1;
    }

    const core::Entity player = create_player(world.registry(), 0, 0);
    host.events().emit("on_player_spawn",
                       mod::EntityHandle{ .reg = &world.registry(), .entity = player });

    sol::state& lua = host.lua();
    const auto step = [&](float seconds) {
        const int ticks = static_cast<int>(seconds * 120.0f);
        for (int i = 0; i < ticks; ++i) { world.step(1.0f / 120.0f); }
    };
    const auto lua_bool = [&](const char* code) {
        return lua.script(code).get<bool>();
    };
    // Between scenarios: wipe enemies/bullets/drops, refill the player.
    const auto reset = [&] {
        lua.script(R"(
            local C = import("core")
            for e in world:each(Enemy) do e:destroy() end
            for e in world:each(C.Bullet) do e:destroy() end
            for e in world:each(C.Xp) do e:destroy() end
            for e in world:each(C.Heal) do e:destroy() end
            for e in world:each(C.Chest) do e:destroy() end
            for p in world:each(Player, Hearts) do
                p:get(Hearts).current = p:get(Hearts).max
                local a = p:get(AimState)
                a.firing = 0
                if p:has(C.IFrames) then p:remove(C.IFrames) end
                if p:has(Downed) then p:remove(Downed) end -- downed = no target for enemies
                if p:has(C.Revive) then p:remove(C.Revive) end
                -- Re-park at the origin with zero velocity: nothing feeds input
                -- here, so leftover velocity (e.g. a faked dash) drifts forever.
                local v = p:get(Velocity)
                v.dx, v.dy = 0, 0
                local pos = p:get(Position)
                pos.x, pos.y = 0, 0
                local d = p:get(Dash)
                d.burst_remaining = 0
            end
        )");
        step(0.1f);
    };

    // --- Scenario 1: bomber arms near the player, then detonates -----------
    lua.script(R"(spawn_enemy(40, 0, "core:bomber"))");
    step(0.5f);
    check(lua_bool(R"(
        local C = import("core")
        for e in world:each(C.Fuse) do return true end
        return false
    )"), "bomber lights its fuse near a player");
    // Poll for the hurt MOMENT every sim tick: the bomber's 4% death
    // heart-drop can magnetize to the freshly hurt player and heal them back
    // within a fraction of a second.
    bool blast_hurt = false;
    for (int i = 0; i < 180; ++i) {
        world.step(1.0f / 120.0f);
        blast_hurt = blast_hurt || lua_bool(R"(
            for p in world:each(Player, Hearts) do
                return p:get(Hearts).current < p:get(Hearts).max
            end
        )");
    }
    check(lua_bool(R"(
        for e in world:each(Enemy) do return false end
        return true
    )"), "bomber blast consumed it (death system ran)");
    check(blast_hurt, "blast bullets hurt the player");
    reset();

    // --- Scenario 2: berserker telegraphs then lunges -----------------------
    lua.script(R"(spawn_enemy(150, 0, "core:berserker"))");
    bool telegraphed = false;
    for (int i = 0; i < 30 && !telegraphed; ++i) { // watch up to 3 s in 0.1 slices
        step(0.1f);
        telegraphed = lua_bool(R"(
            local C = import("core")
            for e in world:each(C.Lunge) do
                if e:get(C.Lunge).winding > 0 then return true end
            end
            return false
        )");
    }
    check(telegraphed, "berserker enters its wind-up telegraph");
    step(3.0f);
    check(lua_bool(R"(
        for p in world:each(Player, Hearts) do
            return p:get(Hearts).current < p:get(Hearts).max
        end
    )"), "berserker lunge reached and hurt the player");
    reset();

    // --- Scenario 3: ranged wind-up then shot -------------------------------
    lua.script(R"(spawn_enemy(200, 0, "core:slinger"))");
    step(1.2f); // 1.0 s spawn grace + into the 0.4 s wind-up
    check(lua_bool(R"(
        local C = import("core")
        for e in world:each(C.Ranged) do
            return e:get(C.Ranged).winding > 0 or e:get(C.Ranged).timer > 1.0
        end
        return false
    )"), "slinger telegraphs (or already fired and cools down)");
    step(1.0f);
    check(lua_bool(R"(
        for p in world:each(Player, Hearts) do
            return p:get(Hearts).current < p:get(Hearts).max
        end
    )"), "slinger arrow hit the player");
    reset();

    // --- Scenario 4: Ent fires a 3-bullet fan --------------------------------
    lua.script(R"(spawn_enemy(250, 0, "core:ent"))");
    int volley = 0;
    for (int i = 0; i < 40 && volley == 0; ++i) { // sample mid-flight
        step(0.05f);
        volley = static_cast<int>(lua.script(R"(
            local C = import("core")
            local n = 0
            for b in world:each(C.Bullet) do
                if b:get(C.Bullet).hostile == 1 then n = n + 1 end
            end
            return n
        )").get<double>());
    }
    check(volley == 3, "ent volley is a 3-bullet fan");
    reset();

    // --- Scenario 5: elites always drop a heart ------------------------------
    lua.script(R"(
        local e = spawn_enemy(300, 300, "core:bandit_elite")
        e:get(Health).current = 0
    )");
    step(0.2f);
    check(lua_bool(R"(
        local C = import("core")
        for h in world:each(C.Heal) do return true end
        return false
    )"), "elite death drops a guaranteed heart");
    reset();

    // --- Scenario 7: Rhino Charger telegraphs, charges, connects ------------
    // Force the CHARGE move (its brain may open with the stomp when it has
    // closed in by pick time — scenario 36 covers autonomous picking).
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(400, 0, "core:rhino_charger")
        local brain = boss:get(C.Brain)
        brain.timer = 9999
        brain.move = 1 -- charge
        brain.winding = 0.05
    )");
    step(4.0f); // windup + the cross-field dash
    check(lua_bool(R"(
        for p in world:each(Player, Hearts) do
            return p:get(Hearts).current < p:get(Hearts).max
        end
    )"), "rhino charger's lunge reached the player");
    reset();

    // --- Scenario 8: Game Master summons adds and blinks away ---------------
    // Brained bosses park their mechanics (cooldown 9999); tests fire them
    // deterministically by zeroing the timer — exactly what a brain move does.
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(100, 0, "core:gamemaster")
        boss:get(C.Summon).timer = 0
    )");
    // Poll per tick: a pool-3 roll can hand out three bomber elites, and all
    // three can rush the adjacent player and self-detonate before a single
    // end-of-window count (a rare all-bombers flake).
    bool gm_adds = false;
    for (int i = 0; i < 180; ++i) { // 1.5 s — the player stands inside blink range
        world.step(1.0f / 120.0f);
        gm_adds = gm_adds || lua_bool(R"(
            local n = 0
            for e in world:each(Enemy) do n = n + 1 end
            return n > 1
        )");
    }
    check(gm_adds, "game master summoned adds");
    check(lua_bool(R"(
        local C = import("core")
        for e in world:each(C.Summon, Position) do
            local p = e:get(Position)
            return (p.x * p.x + p.y * p.y) > 200 * 200 -- blinked away from (100,0)
        end
        return false
    )"), "game master blinked away from the player");
    reset();

    // --- Scenario 9: Frog King rages below half health ----------------------
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(900, 900, "core:boss")
        boss:get(Health).current = boss:get(Health).max * 0.4
    )");
    step(0.5f);
    check(lua_bool(R"(
        local C = import("core")
        for e in world:each(C.Nova) do
            return e:get(C.Nova).phase == 1
        end
        return false
    )"), "frog king entered rage phase below 50% health");
    lua.script(R"(
        local C = import("core")
        for e in world:each(C.Nova, Health) do
            e:get(Health).current = 0
        end
    )");
    step(0.2f);
    check(lua_bool(R"(
        local C = import("core")
        for h in world:each(C.Heal) do return true end
        return false
    )"), "boss death paid the guaranteed drops (C.Boss path)");
    reset();

    // --- Scenario 10: Split Shot fans, Piercing Rounds punch through --------
    lua.script(R"(
        local C = import("core")
        spawn_enemy(80, 0, "core:bandit")
        spawn_enemy(140, 0, "core:bandit")
        for p in world:each(Player, C.Weapon, AimState) do
            local w = p:get(C.Weapon)
            w.projectiles, w.pierce, w.damage = 3, 1, 100
            local a = p:get(AimState)
            a.dx, a.dy, a.firing = 1, 0, 1
        end
    )");
    int fan = 0;
    for (int i = 0; i < 8 && fan < 3; ++i) { // catch the first volley mid-flight
        step(1.0f / 60.0f);
        fan = static_cast<int>(lua.script(R"(
            local C = import("core")
            local n = 0
            for b in world:each(C.Bullet) do
                if b:get(C.Bullet).hostile == 0 then n = n + 1 end
            end
            return n
        )").get<double>());
    }
    check(fan >= 3, "split shot fans 3 bullets per pull");
    step(1.5f);
    check(lua_bool(R"(
        for e in world:each(Enemy) do return false end
        return true
    )"), "one piercing bullet killed both bandits in line");
    reset();

    // --- Scenario 11: Leech heals on bullet kills ----------------------------
    lua.script(R"(
        local C = import("core")
        spawn_enemy(100, 0, "core:bandit")
        for p in world:each(Player, C.Weapon, AimState, Hearts) do
            p:set(C.Leech, { chance = 1.0 })
            p:get(C.Weapon).damage = 100
            p:get(Hearts).current = 1
            local a = p:get(AimState)
            a.dx, a.dy, a.firing = 1, 0, 1
        end
    )");
    step(1.5f);
    check(lua_bool(R"(
        for p in world:each(Player, Hearts) do
            return p:get(Hearts).current >= 2
        end
    )"), "leech kill restored a heart");
    reset();

    // --- Scenario 12: Orbiting Blades shred what stands on the ring ---------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player) do p:set(C.Orbit, {}) end
        local e = spawn_enemy(70, 0, "core:bandit") -- parked ON the blade ring
        e:get(Speed).value = 0
    )");
    step(3.0f); // 3 blades at 2.5 rad/s: a pass every 0.84 s melts 20 hp at 70 dps
    check(lua_bool(R"(
        for e in world:each(Enemy) do return false end
        return true
    )"), "orbiting blades killed the target on the ring");
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player) do p:remove(C.Orbit) end
    )");
    reset();

    // --- Scenario 13: Spiked Armor reflects contact hits ---------------------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player) do p:set(C.Thorns, {}) end
        spawn_enemy(30, 0, "core:bandit")
    )");
    step(1.0f); // touch lands -> 20 reflect kills the 20 hp bandit
    check(lua_bool(R"(
        for e in world:each(Enemy) do return false end
        return true
    )"), "spiked armor killed the biter");
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player) do p:remove(C.Thorns) end
    )");
    reset();

    // --- Scenario 14: Phoenix Feather cheats death ---------------------------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, Hearts) do
            p:set(C.Phoenix, {})
            p:get(Hearts).current = 0
        end
    )");
    step(0.2f);
    check(lua_bool(R"(
        local C = import("core")
        for p in world:each(Player, Hearts) do
            return not p:has(Downed) and p:get(Hearts).current >= 2
                   and not p:has(C.Phoenix)
        end
    )"), "phoenix feather consumed itself instead of a down");
    reset();

    // --- Scenario 15: Adrenaline Core arms after a dash ----------------------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, Dash) do
            p:set(C.Overcharge, {})
            p:get(Dash).burst_remaining = 0.1 -- mid-burst; it ends next ticks
        end
    )");
    step(0.5f);
    check(lua_bool(R"(
        local C = import("core")
        for p in world:each(C.Overcharge) do
            return p:get(C.Overcharge).remaining > 0
        end
        return false
    )"), "adrenaline core armed when the dash burst ended");
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player) do p:remove(C.Overcharge) end
    )");
    reset();

    // --- Scenario 16: boss death drops a chest; walking on it opens it ------
    lua.script(R"(
        local e = spawn_enemy(30, 0, "core:miniboss")
        e:get(Health).current = 0
    )");
    step(0.5f); // death drops the chest ~38 px away; pickups (20 Hz) grab it
    int chest_notes = 0;
    world.registry().view<ChestOpen>().each([&](core::Entity, const ChestOpen&) { ++chest_notes; });
    check(chest_notes > 0, "opening the chest filed a ChestOpen round request");
    check(lua_bool(R"(
        local C = import("core")
        for c in world:each(C.Chest) do return false end
        return true
    )"), "the chest was consumed on pickup");
    world.registry().view<ChestOpen>().each(
      [&](core::Entity e, const ChestOpen&) { world.registry().destroy(e); });
    reset();

    // --- Scenario 16b: /upgrade /object grants file OfferGrant notes ---------
    // The dev verb backing the console commands: N calls = N notes, flavored
    // level (0) vs object (1); the server consumes one per round.
    lua.script(R"(
        world:grant_offer("level")
        world:grant_offer("object")
        world:grant_offer("object")
    )");
    int grant_level = 0;
    int grant_object = 0;
    world.registry().view<OfferGrant>().each([&](core::Entity, const OfferGrant& g) {
        if (g.flavor == 0) { ++grant_level; } else { ++grant_object; }
    });
    check(grant_level == 1 && grant_object == 2, "grant_offer files one note per call, flavored");
    world.registry().view<OfferGrant>().each(
      [&](core::Entity e, const OfferGrant&) { world.registry().destroy(e); });
    reset();

    // --- Scenario 17: offer filtering — levels roll upgrades, chests objects -
    {
        const auto kind_of = [&](std::uint8_t wire_id) {
            for (const mod::ContentDef& d : host.registry().defs()) {
                if (d.wire_id == wire_id) { return d.kind; }
            }
            return mod::ContentKind::StatUpgrade;
        };
        bool level_clean = true;
        for (const proto::LevelUpChoice& c :
             mod::run_level_offer(host, world.registry(), player, 1, "level")) {
            if (kind_of(c.id) == mod::ContentKind::Object) { level_clean = false; }
        }
        check(level_clean, "level-up offers contain no objects");
        const auto chest_offer = mod::run_level_offer(host, world.registry(), player, 1, "chest");
        bool chest_clean = !chest_offer.empty();
        for (const proto::LevelUpChoice& c : chest_offer) {
            if (kind_of(c.id) != mod::ContentKind::Object) { chest_clean = false; }
        }
        check(chest_clean, "chest offers contain only objects");
    }

    // --- Scenario 18: Frog Prince leaps and slams ----------------------------
    lua.script(R"(spawn_enemy(400, 0, "core:frog_prince"))");
    step(6.0f);
    check(lua_bool(R"(
        for p in world:each(Player, Hearts) do
            return p:get(Hearts).current < p:get(Hearts).max
        end
    )"), "frog prince's leap reached the player");
    reset();

    // --- Scenario 19: Bomb Lord carpets the ground with kegs ----------------
    // Spawned raw, its arena rect defaults to center (0,0) — kegs land around
    // the origin, which is exactly where the test player idles.
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(700, 0, "core:bomb_lord")
        boss:get(C.Planter).timer = 0 -- fire the (parked) carpet batch
    )");
    step(1.0f);
    check(lua_bool(R"(
        local n = 0
        for e in world:each(Enemy) do n = n + 1 end
        return n > 3
    )"), "bomb lord planted powder kegs");
    reset();

    // --- Scenario 20: Vampire Lord regenerates -------------------------------
    lua.script(R"(
        local C = import("core")
        local v = spawn_enemy(900, 900, "core:vampire_lord")
        v:get(Health).current = v:get(Health).max * 0.5
    )");
    step(1.0f);
    check(lua_bool(R"(
        local C = import("core")
        for e in world:each(C.Regen, Health) do
            return e:get(Health).current > e:get(Health).max * 0.5
        end
        return false
    )"), "vampire lord regenerated while unharmed");
    reset();

    // --- Scenario 6: the player still shoots and kills ----------------------
    lua.script(R"(
        local C = import("core")
        spawn_enemy(120, 0, "core:bandit")
        for p in world:each(Player, C.Weapon, AimState) do
            local w = p:get(C.Weapon)
            w.projectiles, w.pierce, w.damage = 1, 0, 10 -- back to the base loadout
            if p:has(C.Leech) then p:remove(C.Leech) end
            local a = p:get(AimState)
            a.dx, a.dy, a.firing = 1, 0, 1
        end
    )");
    step(2.0f);
    check(lua_bool(R"(
        for e in world:each(Enemy) do return false end
        return true
    )"), "player bullets still kill (shoot pipeline intact)");

    reset();

    // --- Scenario 27: arena clamp works WITHOUT C.Nova (C.Arena decouple) ---
    lua.script(R"(
        spawn_enemy(700, 0, "core:bomb_lord") -- C.Arena, no C.Nova
        for p in world:each(Player, Position) do
            local pos = p:get(Position)
            pos.x, pos.y = 2000, 0 -- outside the (default-centered) rect
        end
    )");
    step(0.2f);
    check(lua_bool(R"(
        for p in world:each(Player, Position) do
            return p:get(Position).x <= 960.5
        end
    )"), "arena clamp confines players without a nova");
    reset();

    // --- Scenario 28: Bomb Lord tosses pre-lit kegs at the player -----------
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(700, 0, "core:bomb_lord")
        boss:remove(C.Planter)      -- isolate the toss: only lobbed kegs remain
        boss:get(C.Toss).timer = 0  -- fire it now (parked otherwise)
    )");
    step(0.6f); // keg lands lit (fuse 1.3 s) within scatter(200) of the player
    check(lua_bool(R"(
        local C = import("core")
        -- 260 covers the full 200 px scatter (the boss is 700 px away, so a
        -- hit here can only be a lobbed keg, never boss-adjacent placement).
        for keg in world:each(C.Fuse, Position) do
            local p = keg:get(Position)
            if p.x * p.x + p.y * p.y < 260 * 260 then return true end
        end
        return false
    )"), "bomb lord lobbed a lit keg at the player's feet");
    reset();

    // --- Scenario 29: Vampire Lord's blood bolts home ------------------------
    lua.script(R"(
        local C = import("core")
        spawn_enemy(600, 0, "core:vampire_lord"):get(C.BoltCaster).timer = 0
    )");
    step(1.0f); // cast fires immediately; the homing bends them over ~1 s
    check(lua_bool(R"(
        local C = import("core")
        for b in world:each(C.Homing, Velocity, Position) do
            -- steered: velocity points at the player (at the origin)
            local v = b:get(Velocity)
            local p = b:get(Position)
            local dot = v.dx * (0 - p.x) + v.dy * (0 - p.y)
            if dot > 0 then return true end
        end
        return false
    )"), "homing blood bolts steer toward the player");
    reset();

    // --- Scenario 30: Elder Ent — sprinkler streams + blooming seeds --------
    lua.script(R"(
        local C = import("core")
        spawn_enemy(500, 0, "core:elder_ent"):get(C.SeedLauncher).timer = 0
    )");
    step(0.8f); // before the seed's 1.1 s bloom
    check(lua_bool(R"(
        local C = import("core")
        local n = 0
        for b in world:each(C.Bullet) do
            if b:get(C.Bullet).hostile == 1 then n = n + 1 end
        end
        return n >= 6 -- the sprinkler streams continuously
    )"), "elder ent's sprinkler streams bullets");
    check(lua_bool(R"(
        local C = import("core")
        for s in world:each(C.Seed) do return true end
        return false
    )"), "elder ent lobbed a blooming seed");
    step(3.0f);
    check(lua_bool(R"(
        local C = import("core")
        for s in world:each(C.Seed) do return false end
        return true
    )"), "the seed bloomed (popped into petals and died)");
    reset();

    // --- Scenario 31: Game Master's rotating cross barrage ------------------
    lua.script(R"(
        local C = import("core")
        spawn_enemy(500, 0, "core:gamemaster"):get(C.Barrage).timer = 0
    )");
    step(0.5f); // one full volley, before the brain layers anything else on
    check(lua_bool(R"(
        local C = import("core")
        local n = 0
        for b in world:each(C.Bullet) do
            if b:get(C.Bullet).hostile == 1 then n = n + 1 end
        end
        return n >= 12 -- 4 lanes x 3-bullet lances
    )"), "game master fired a full cross barrage");
    reset();

    // --- Scenario 32: the rage aimed-volley is GONE (frog rings only) -------
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(400, 0, "core:boss")
        boss:get(Health).current = boss:get(Health).max * 0.4 -- straight to rage
        boss:get(C.Nova).timer = 0                            -- ring now (parked)
    )");
    step(1.0f); // an enraged ring (+ whatever the brain layers on)
    check(lua_bool(R"(
        local C = import("core")
        local rings, heavies = 0, 0
        for b in world:each(C.Bullet) do
            if b:get(C.Bullet).hostile == 1 then
                rings = rings + 1
                if b:get(Render).variant == 3 then heavies = heavies + 1 end
            end
        end
        return rings > 0 and heavies == 0
    )"), "frog king rages with rings only — no aimed heavies");
    reset();

    // --- Scenario 21: the XP curve is the mod's quadratic, not the fallback -
    check(mod::run_xp_curve(host, 30) == 5U + 90U + 720U, // 5 + 3L + 0.8L^2
          "xp curve is the mod's quadratic");
    check(mod::run_xp_curve(host, 60) > 3 * mod::run_xp_curve(host, 30),
          "xp curve grows super-linearly"); // doubling the level ~4x the cost (minus linear drag)

    // --- Scenario 22: hearts only magnetize toward HURT players -------------
    lua.script(R"(
        local C = import("core")
        local heart = spawn_entity(80, 0)
        heart:get(Render).kind = KIND.heart
        heart:set(C.Heal, {})
    )");
    step(0.6f);
    check(lua_bool(R"(
        local C = import("core")
        for h in world:each(C.Heal, Position) do
            return h:get(Position).x > 70 -- untouched: no reserve trains at full HP
        end
        return false
    )"), "a full-HP player does not attract hearts");
    lua.script(R"(
        for p in world:each(Player, Hearts) do
            local h = p:get(Hearts)
            h.current = math.floor(h.max - 1)
        end
    )");
    step(0.8f); // magnet pulls it in, pickup heals
    check(lua_bool(R"(
        for p in world:each(Player, Hearts) do
            local h = p:get(Hearts)
            return h.current == h.max
        end
    )"), "a hurt player pulls and eats the heart");
    reset();

    // --- Scenario 23: Mirror Barrel doubles the volley backward -------------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, C.Weapon, AimState) do
            local w = p:get(C.Weapon)
            w.projectiles, w.mirror = 2, 1
            local a = p:get(AimState)
            a.dx, a.dy, a.firing = 1, 0, 1
        end
    )");
    int mirrored = 0;
    for (int i = 0; i < 8 && mirrored < 4; ++i) {
        step(1.0f / 60.0f);
        mirrored = static_cast<int>(lua.script(R"(
            local C = import("core")
            local n = 0
            for b in world:each(C.Bullet) do
                if b:get(C.Bullet).hostile == 0 then n = n + 1 end
            end
            return n
        )").get<double>());
    }
    check(mirrored >= 4, "mirror barrel fires the fan both ways");
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, C.Weapon) do
            local w = p:get(C.Weapon)
            w.projectiles, w.mirror = 1, 0
        end
    )");
    reset();

    // --- Scenario 24: Goliath clamps speed and retires Swift Boots ----------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, Speed) do
            p:set(C.Goliath, {})
            p:get(Speed).value = 320
        end
    )");
    step(1.2f); // identity_sys runs at 2 Hz
    check(lua_bool(R"(
        for p in world:each(Player, Speed) do
            return p:get(Speed).value <= 200
        end
    )"), "goliath clamps speed to 200");
    check(lua_bool(R"(
        for p in world:each(Player) do
            for _, entry in ipairs(world:offerable(p)) do
                if entry.id == "core:movespeed" then return false end
            end
            return true
        end
    )"), "swift boots left goliath's pool");

    // --- Scenario 25: Executioner's Edge — crit tracks pierce ---------------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, C.Weapon) do
            p:set(C.Executioner, {})
            p:get(C.Weapon).pierce = 3
        end
    )");
    step(1.2f);
    check(lua_bool(R"(
        local C = import("core")
        for p in world:each(C.Crit) do
            local c = p:get(C.Crit).chance
            return c > 0.23 and c < 0.25 -- 8% x 3 pierce
        end
    )"), "executioner crit chance tracks pierce");
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player) do
            p:remove(C.Goliath)
            p:remove(C.Executioner)
            p:get(Speed).value = 240
            p:get(C.Weapon).pierce = 0
            p:get(C.Crit).chance = 0.05
        end
    )");
    reset();

    // --- Scenario 33: RunStats scoreboard + floating damage numbers ---------
    host.state().damage_events.clear();
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player) do p:set(RunStats, {}) end -- clean slate
        local e = spawn_enemy(120, 0, "core:bandit")
        e:get(Health).current = 1 -- one hit kills
        e:get(Speed).value = 0    -- parked on the aim line, out of contact reach
        for p in world:each(Player, AimState) do
            local a = p:get(AimState)
            a.dx, a.dy, a.firing = 1, 0, 1
        end
    )");
    step(1.0f);
    check(lua_bool(R"(
        for p in world:each(Player, RunStats) do
            local rs = p:get(RunStats)
            return rs.kills >= 1 and rs.damage > 0
        end
    )"), "bullet kill credits the shooter's RunStats");
    check(!host.state().damage_events.empty(), "damage numbers queued for the snapshot drain");
    check(host.state().damage_events.size() <= proto::max_damage_events,
          "damage-number queue respects its cap");
    host.state().damage_events.clear();
    reset();

    // --- Scenario 34: signature bullets carry their boss's variant byte -----
    lua.script(R"(
        local C = import("core")
        spawn_enemy(300, 0, "core:vampire_lord"):get(C.BoltCaster).timer = 0
    )");
    step(0.8f);
    check(lua_bool(R"(
        local C = import("core")
        for b in world:each(C.Homing) do
            return b:get(Render).variant == 4 -- blood bolt
        end
        return false
    )"), "vampire bolts wear the blood-bolt variant");
    reset();
    lua.script(R"(
        local C = import("core")
        spawn_enemy(300, 0, "core:elder_ent"):get(C.SeedLauncher).timer = 0
    )");
    step(0.8f); // before the 1.1 s bloom; the ambient sprinkler supplies pellets
    check(lua_bool(R"(
        local C = import("core")
        local seed, pellet = false, false
        for b in world:each(C.Seed) do
            if b:get(Render).variant == 6 then seed = true end
        end
        for b in world:each(C.Bullet) do
            if b:get(Render).variant == 5 then pellet = true end
        end
        return seed and pellet
    )"), "elder ent seeds/pellets wear their green variants");
    reset();
    lua.script(R"(
        local C = import("core")
        spawn_enemy(300, 0, "core:gamemaster"):get(C.Barrage).timer = 0
    )");
    step(0.5f);
    check(lua_bool(R"(
        local C = import("core")
        for b in world:each(C.Bullet) do
            if b:get(Render).variant == 7 then return true end
        end
        return false
    )"), "game master lances wear the lance variant");
    reset();

    // --- Scenario 36: the brain runs the fight — move VARIETY ----------------
    // Left alone the Game Master must execute at least two DIFFERENT moves
    // (used_mask gains a bit per distinct move; never-repeat guarantees
    // variety once two picks happened). 14 s covers the worst opener: the
    // elite summon at its jittered-longest cooldown (2 + 7 x 1.25 ~= 10.8 s).
    lua.script(R"(spawn_enemy(600, 0, "core:gamemaster"))");
    step(14.0f);
    check(lua_bool(R"(
        local C = import("core")
        for boss in world:each(C.Brain) do
            local m = math.floor(boss:get(C.Brain).used_mask)
            return m > 0 and (m & (m - 1)) ~= 0 -- at least two bits set
        end
        return false
    )"), "the brain fired at least two distinct moves in 10 s");
    reset();

    // --- Scenario 37: health-gated phase escalation --------------------------
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(900, 900, "core:gamemaster")
        boss:get(Health).current = boss:get(Health).max * 0.30 -- below both gates
    )");
    step(0.5f);
    check(lua_bool(R"(
        local C = import("core")
        for boss in world:each(C.Brain) do
            return boss:get(C.Brain).phase == 3
        end
        return false
    )"), "the brain escalates to phase 3 below 35% health");
    reset();

    // --- Scenario 38: Bomb Lord's keg CAGE rings the player ------------------
    // Force the move (index 4 in its table) by loading it into the brain's
    // wind-up directly — the same path a natural pick takes.
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(700, 0, "core:bomb_lord")
        local brain = boss:get(C.Brain)
        brain.timer = 9999
        brain.move = 4       -- cage
        brain.winding = 0.05
    )");
    step(0.3f);
    check(lua_bool(R"(
        local C = import("core")
        local n = 0
        for keg in world:each(C.Fuse, Position) do
            local p = keg:get(Position)
            if p.x * p.x + p.y * p.y < 260 * 260 then n = n + 1 end
        end
        return n >= 8
    )"), "the forced cage move ringed the player with lit kegs");
    reset();

    // --- Scenario 39: the boss clamps to an INSET arena rect -----------------
    // Chasing a wall-hugger must not grind the boss into the wall: players
    // clamp to the full rect, the arena BEARER stops 90 px short.
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(700, 0, "core:bomb_lord")
        boss:get(Position).x = 2000
        for p in world:each(Player, Position) do p:get(Position).x = 2000 end
    )");
    step(0.2f);
    check(lua_bool(R"(
        local C = import("core")
        for boss in world:each(C.Arena, Position) do
            local bx = boss:get(Position).x
            for p in world:each(Player, Position) do
                return bx <= 960 - 90 + 1 and p:get(Position).x > 950
            end
        end
        return false
    )"), "the boss stops 90 px short of the wall players can hug");
    reset();

    // --- Scenario 40: heart drops are SUPPRESSED in a stocked area ----------
    lua.script(R"(
        local C = import("core")
        for i = 1, 3 do
            local heart = spawn_entity(500 + i * 10, 500)
            heart:get(Render).kind = KIND.heart
            heart:set(C.Heal, {})
        end
        local elite = spawn_enemy(500, 500, "core:bandit")
        elite:set(C.EliteDrop, {}) -- guaranteed drop... unless the area is stocked
        elite:get(Health).current = 0
    )");
    step(0.2f); // death system pays out (or, here, doesn't)
    check(lua_bool(R"(
        local C = import("core")
        local n = 0
        for h in world:each(C.Heal) do n = n + 1 end
        return n == 3
    )"), "no 4th heart drops where 3 already sit");
    reset();

    // --- Scenario 41: hearts go stale (heal_decay TTL) ----------------------
    lua.script(R"(
        local C = import("core")
        local heart = spawn_entity(600, 0) -- out of full-HP pickup reach anyway
        heart:get(Render).kind = KIND.heart
        heart:set(C.Heal, { ttl = 1.5 }) -- short clock for the test
    )");
    step(2.2f);
    check(lua_bool(R"(
        local C = import("core")
        for h in world:each(C.Heal) do return false end
        return true
    )"), "an unclaimed heart despawns after its ttl");
    reset();

    // --- Scenario 42: a PINNED boss teleports off the wall -------------------
    // The inset alone is still a wall — held against it by a wall-hugging
    // player for > 1.2 s, the boss must randomize its position instead.
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(700, 0, "core:bomb_lord")
        boss:get(Position).x = 2000
        for p in world:each(Player, Position) do p:get(Position).x = 2000 end
    )");
    step(1.8f); // clamp holds it at 870 while it chases; tp fires at ~1.25 s
    check(lua_bool(R"(
        local C = import("core")
        for boss in world:each(C.Arena, Position) do
            return boss:get(Position).x < 800 -- teleported well off the wall
        end
        return false
    )"), "a wall-pinned boss teleports to a random arena point");
    reset();

    // --- Scenario 43: the Vampire Lord KITES (standoff, no walk-up-blink) ---
    lua.script(R"(spawn_enemy(600, 0, "core:vampire_lord"))");
    step(3.2f); // walks 600 -> its 380 px standoff, then HOLDS
    check(lua_bool(R"(
        local C = import("core")
        for v in world:each(C.BoltCaster, Position) do
            local x = v:get(Position).x
            return x > 340 and x < 460 -- approached, then held the ring
        end
        return false
    )"), "vampire holds his standoff instead of walking into blink range");
    reset();

    // --- Scenario 44: boss-summoned minions pay NO loot ----------------------
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(900, 900, "core:gamemaster")
        boss:get(C.Summon).timer = 0
    )");
    step(0.4f); // adds spawn (ELITE pool — normally guaranteed heart drops)
    lua.script(R"(
        local C = import("core")
        for e in world:each(Enemy, Health) do
            if not e:has(C.Brain) then e:get(Health).current = 0 end
        end
    )");
    step(0.3f); // death system consumes them
    check(lua_bool(R"(
        local C = import("core")
        for orb in world:each(C.Xp) do return false end
        for heart in world:each(C.Heal) do return false end
        return true
    )"), "dead summons drop neither XP nor hearts");
    reset();

    // --- Scenario 45: @25 Knight Commander — milestone + rally ---------------
    host.events().emit("on_wave_start", 25);
    step(0.1f);
    check(lua_bool(R"(
        local C = import("core")
        for b in world:each(C.Brain) do return b:get(C.Brain).id == 8 end
        return false
    )"), "wave 25 spawns the Knight Commander");
    lua.script(R"(
        local C = import("core")
        for b in world:each(C.Brain) do -- force the rally move (a brain poke)
            local brain = b:get(C.Brain)
            brain.move = 3
            brain.winding = 0.05
        end
    )");
    step(0.4f);
    check(lua_bool(R"(
        local C = import("core")
        local squires = 0
        for e in world:each(Enemy, C.NoLoot) do squires = squires + 1 end
        return squires == 3
    )"), "the rally raised three loot-free squires");
    reset();

    // --- Scenario 46: @35 Archmage — milestone + chase orbs ------------------
    host.events().emit("on_wave_start", 35);
    step(0.1f);
    check(lua_bool(R"(
        local C = import("core")
        for b in world:each(C.Brain) do return b:get(C.Brain).id == 9 end
        return false
    )"), "wave 35 spawns the Archmage");
    lua.script(R"(
        local C = import("core")
        for b in world:each(C.Brain) do
            local brain = b:get(C.Brain)
            brain.move = 4 -- chase_orbs (phase-gated in play; forced here)
            brain.winding = 0.05
        end
    )");
    step(0.4f);
    check(lua_bool(R"(
        local C = import("core")
        for b in world:each(C.Bullet, C.Homing) do
            return b:get(C.Bullet).hostile == 1
        end
        return false
    )"), "the archmage's orbs are hostile and they steer");
    reset();

    // --- Scenario 47: @45 Mimic King — milestone + fool's gold ---------------
    host.events().emit("on_wave_start", 45);
    step(0.1f);
    check(lua_bool(R"(
        local C = import("core")
        for b in world:each(C.Brain) do return b:get(C.Brain).id == 10 end
        return false
    )"), "wave 45 spawns the Mimic King");
    lua.script(R"(
        local C = import("core")
        for b in world:each(C.Brain) do
            local brain = b:get(C.Brain)
            brain.move = 4 -- fool's gold
            brain.winding = 0.05
        end
    )");
    step(0.4f);
    check(lua_bool(R"(
        local C = import("core")
        local lures = 0
        for g in world:each(C.FoolsGold) do lures = lures + 1 end
        return lures == 2 -- two per live player, one player here
    )"), "fool's gold salts two lures around the player");
    lua.script(R"(
        local C = import("core")
        for g in world:each(C.FoolsGold, Position) do -- reach for ONE lure
            local gp = g:get(Position)
            for p in world:each(Player, Position) do
                local pp = p:get(Position)
                gp.x, gp.y = pp.x, pp.y
            end
            break
        end
    )");
    step(0.15f);
    check(lua_bool(R"(
        local C = import("core")
        local lures = 0
        for g in world:each(C.FoolsGold) do lures = lures + 1 end
        if lures ~= 1 then return false end
        for b in world:each(C.Bullet) do
            -- coin pellets: the Mimic King's palette (variant 5 sound/visual)
            if b:get(C.Bullet).hostile == 1 then return b:get(Render).variant == 5 end
        end
        return false
    )"), "a reached-for lure pops into a hostile coin bite");
    reset();

    // --- Scenario 48: Shieldbearer armor — flat reduction, floor 1 -----------
    lua.script(R"(
        local C = import("core")
        local sb = spawn_enemy(500, 0, "core:shieldbearer")
        sb:get(Speed).value = 0 -- park it for a clean before/after read
        _M = sb:get(Health).max
        _F = sb:get(C.Armor).flat
        local b = spawn_bullet(500, 0, 0, 0)
        b:set(C.Bullet, { damage = 10 })
    )");
    step(0.1f);
    check(lua_bool(R"(
        local C = import("core")
        for sb in world:each(C.Armor, Health) do
            local loss = _M - sb:get(Health).current
            local want = math.max(1, 10 - _F)
            if math.abs(loss - want) > 0.01 then return false end
            -- chip shot: damage below the plate still lands 1
            local b = spawn_bullet(sb:get(Position).x, sb:get(Position).y, 0, 0)
            b:set(C.Bullet, { damage = 1 })
            _M = sb:get(Health).current
            return true
        end
        return false
    )"), "armor shaves flat damage off the bullet");
    step(0.1f);
    check(lua_bool(R"(
        local C = import("core")
        for sb in world:each(C.Armor, Health) do
            return math.abs((_M - sb:get(Health).current) - 1) < 0.01
        end
        return false
    )"), "chip damage floors at 1 - armor never means immune");
    reset();

    // --- Scenario 49: Acolyte — its orb homes ---------------------------------
    lua.script(R"(spawn_enemy(300, 0, "core:acolyte"))");
    step(2.4f); // spawn grace 1.0 + windup 0.7, fired well within this
    check(lua_bool(R"(
        local C = import("core")
        for b in world:each(C.Bullet, C.Homing) do
            return b:get(C.Bullet).hostile == 1
        end
        return false
    )"), "the acolyte's orb carries homing");
    reset();

    // --- Scenario 50: Mimic — inert until approached --------------------------
    lua.script(R"(spawn_enemy(400, 0, "core:mimic"))");
    step(0.4f);
    const bool mimic_asleep = lua_bool(R"(
        local C = import("core")
        for m in world:each(C.Ambush, Speed) do
            return m:get(Speed).value == 0 and m:get(C.Ambush).awake == 0
        end
        return false
    )");
    lua.script(R"(
        for p in world:each(Player, Position) do p:get(Position).x = 300 end
    )");
    step(0.3f); // 100 px away — inside the 150 px trigger
    check(mimic_asleep && lua_bool(R"(
        local C = import("core")
        for m in world:each(C.Ambush, Speed) do
            return m:get(Speed).value > 0 and m:get(C.Ambush).awake == 1
        end
        return false
    )"), "the mimic sleeps at range and wakes one-way in reach");
    reset();

    // --- Scenario 51: Static Charge — a periodic owner-stamped ring -----------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player) do p:set(C.Static, {}) end
    )");
    step(3.3f); // first ring at the 3.0 s cooldown
    check(lua_bool(R"(
        local C = import("core")
        for p in world:each(Player) do
            for b in world:each(C.Bullet) do
                local bullet = b:get(C.Bullet)
                if bullet.hostile == 0 and bullet.owner == p:id() then
                    return b:get(Render).variant == 9 -- the electric zap identity
                end
            end
        end
        return false
    )"), "static charge fires an owner-stamped zap ring");
    lua.script(R"(
        local C = import("core")
        for p in world:each(C.Static) do p:remove(C.Static) end
    )");
    reset();

    // --- Scenario 52: Hunter's Instinct — kills refund dash cooldown ----------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, Dash) do
            p:set(C.Hunter, {})
            local d = p:get(Dash)
            d.charges = 0
            d.cooldown = 2.0
            local e = spawn_enemy(300, 0, "core:bandit")
            local b = spawn_bullet(300, 0, 0, 0)
            b:set(C.Bullet, { damage = 100000, owner = p:id() })
        end
    )");
    step(0.1f);
    check(lua_bool(R"(
        for p in world:each(Player, Dash) do
            return p:get(Dash).cooldown < 1.6 -- 2.0 - 0.5 refund (- ticked time)
        end
    )"), "a kill refunds dash cooldown through hunter's instinct");
    lua.script(R"(
        local C = import("core")
        for p in world:each(C.Hunter, Dash) do
            p:remove(C.Hunter)
            local d = p:get(Dash)
            d.charges = d.max_charges
            d.cooldown = 0
        end
    )");
    reset();

    // --- Scenario 53: Reactive Plating — a lost heart bites back --------------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, Position) do
            p:set(C.Reactive, {})
            local pp = p:get(Position)
            local b = spawn_bullet(pp.x, pp.y, 0, 0)
            b:set(C.Bullet, { damage = 1, hostile = 1 })
        end
    )");
    step(0.1f);
    check(lua_bool(R"(
        local C = import("core")
        local burst = 0
        for b in world:each(C.Bullet) do
            if b:get(C.Bullet).hostile == 0 then burst = burst + 1 end
        end
        for p in world:each(Player, Hearts) do
            return burst == 10 and p:get(Hearts).current < p:get(Hearts).max
        end
        return false
    )"), "losing a heart bursts the reactive ring");
    lua.script(R"(
        local C = import("core")
        for p in world:each(C.Reactive) do p:remove(C.Reactive) end
    )");
    reset();

    // --- Scenario 54: Heavy Caliber — the hitbox rides the weapon stat --------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, C.Weapon, AimState) do
            p:get(C.Weapon).bullet_radius = 9
            local a = p:get(AimState)
            a.firing, a.dx, a.dy = 1, 1, 0
        end
    )");
    step(0.05f);
    check(lua_bool(R"(
        local C = import("core")
        for b in world:each(C.Bullet, Radius) do
            if b:get(C.Bullet).hostile == 0 then return b:get(Radius).value == 9 end
        end
        return false
    )"), "heavy caliber fires fatter bullets");
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, C.Weapon, AimState) do
            p:get(C.Weapon).bullet_radius = 4
            p:get(AimState).firing = 0
        end
    )");
    reset();

    // --- Scenario 55: Reaper — executes trash, spares bosses -------------------
    lua.script(R"(
        local C = import("core")
        local e = spawn_enemy(400, 0, "core:bandit")
        e:get(Speed).value = 0
        local m = e:get(Health).max
        local b = spawn_bullet(400, 0, 0, 0)
        b:set(C.Bullet, { damage = m - 0.5, cull = 0.1 }) -- leaves 0.5 < 10%
    )");
    step(0.2f);
    check(lua_bool(R"(
        for e in world:each(Enemy) do return false end
        return true
    )"), "reaper executes trash left under the threshold");
    reset();
    lua.script(R"(
        local C = import("core")
        local boss = spawn_enemy(400, 0, "core:miniboss")
        boss:get(Speed).value = 0
        local m = boss:get(Health).max
        local b = spawn_bullet(400, 0, 0, 0)
        b:set(C.Bullet, { damage = m - 0.5, cull = 0.99 })
    )");
    step(0.2f);
    check(lua_bool(R"(
        local C = import("core")
        for boss in world:each(C.Boss, Health) do
            return boss:get(Health).current > 0 -- hurt to a sliver, never culled
        end
        return false
    )"), "reaper never executes a boss");
    reset();

    // --- Scenario 56: Volatile Rounds — kill-burst, no chains ------------------
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player) do
            local e = spawn_enemy(400, 0, "core:bandit")
            local b = spawn_bullet(400, 0, 0, 0)
            b:set(C.Bullet, { damage = 100000, volatile = 30, owner = p:id() })
        end
    )");
    step(0.05f);
    check(lua_bool(R"(
        local C = import("core")
        local burst, clean = 0, true
        for b in world:each(C.Bullet) do
            local bullet = b:get(C.Bullet)
            if bullet.hostile == 0 and bullet.damage == 30 then
                burst = burst + 1
                if bullet.volatile ~= 0 then clean = false end
            end
        end
        return burst == 6 and clean
    )"), "a volatile kill bursts six chain-free bullets");
    reset();

    // --- Scenario 57: Onion + Blades scale with the weapon ---------------------
    // Target = the Goldhorn (fat health pool): base aura/blade numbers can't
    // meaningfully dent it in the window, so the measured damage IS the scaling.
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, C.Weapon) do
            p:set(C.Aura, { radius = 120, per_second = 25 })
            p:get(C.Weapon).damage = 1000 -- aura dps becomes 25 + 600
            local e = spawn_enemy(60, 0, "core:brute_gold")
            e:get(Speed).value = 0
            _M = e:get(Health).max
        end
    )");
    step(0.5f);
    check(lua_bool(R"(
        for e in world:each(Enemy, Health) do
            return _M - e:get(Health).current > 200 -- base 25/s tops out ~13
        end
        return true -- burned clean through: scaled beyond doubt
    )"), "the onion aura scales with weapon damage");
    reset();
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player) do
            p:remove(C.Aura)
            p:set(C.Orbit, {})
            local e = spawn_enemy(95, 0, "core:brute_gold") -- parked ON the blade ring
            e:get(Speed).value = 0
            _M = e:get(Health).max
        end
    )");
    step(1.0f); // a full sweep guarantees a blade pass (2.5 rad > the 2.09 gap)
    check(lua_bool(R"(
        for e in world:each(Enemy, Health) do
            return _M - e:get(Health).current > 50 -- base 70/s per pass tops ~15
        end
        return true
    )"), "the orbiting blades scale with weapon damage");
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, C.Weapon) do p:get(C.Weapon).damage = 10 end
    )");
    reset();

    // --- Scenario 58: the orbit upgrade lanes gate + cap ------------------------
    // (the player still owns C.Orbit from scenario 57)
    check(lua_bool(R"(
        for p in world:each(Player) do
            local spin, count = false, false
            for _, entry in ipairs(world:offerable(p)) do
                if entry.id == "core:bladespin" then spin = true end
                if entry.id == "core:bladecount" then count = true end
            end
            return spin and count
        end
    )"), "blade lanes join the pool once you own the blades");
    lua.script(R"(
        local C = import("core")
        for p in world:each(C.Orbit) do
            local o = p:get(C.Orbit)
            o.spin, o.count = 6.0, 8 -- both hard caps reached
        end
    )");
    check(lua_bool(R"(
        for p in world:each(Player) do
            for _, entry in ipairs(world:offerable(p)) do
                if entry.id == "core:bladespin" or entry.id == "core:bladecount" then
                    return false
                end
            end
            return true
        end
    )"), "capped blade lanes leave the pool");
    lua.script(R"(
        local C = import("core")
        for p in world:each(C.Orbit) do p:remove(C.Orbit) end
    )");
    reset();

    // --- Scenario 59: enemy separation — a stack fans out into a crowd -------
    // Down the player so targeting leaves the horde idle (no live target =
    // velocities stay zero): what spreads them is PURE separation.
    lua.script(R"(
        for p in world:each(Player) do p:set(Downed, { respawn_wave = 0 }) end
        for i = 1, 12 do
            local e = spawn_enemy(500, 500, "core:bandit")
            e:get(Position).x, e:get(Position).y = 500, 500 -- exact stack
        end
    )");
    step(1.5f);
    check(lua_bool(R"(
        local xs, ys, n = {}, {}, 0
        for e in world:each(Enemy, Position) do
            n = n + 1
            xs[n], ys[n] = e:get(Position).x, e:get(Position).y
        end
        if n ~= 12 then return false end
        for i = 1, n do
            for j = i + 1, n do
                local dx, dy = xs[i] - xs[j], ys[i] - ys[j]
                if dx * dx + dy * dy < 20 * 20 then return false end
            end
        end
        return true
    )"), "a 12-enemy stack separates into a crowd");
    reset();

    // --- Scenario 60: separation anchors — planted hazards hold their ground -
    lua.script(R"(
        for p in world:each(Player) do p:set(Downed, { respawn_wave = 0 }) end
        local mine = spawn_enemy(500, 500, "core:mine") -- Speed 0: an anchor
        mine:get(Position).x, mine:get(Position).y = 500, 500
        for i = 1, 5 do
            local e = spawn_enemy(500, 500, "core:bandit")
            e:get(Position).x, e:get(Position).y = 500, 500
        end
    )");
    step(1.5f);
    check(lua_bool(R"(
        local C = import("core")
        for mine in world:each(C.Bomber, Position) do
            local mp = mine:get(Position)
            local dx, dy = mp.x - 500, mp.y - 500
            if dx * dx + dy * dy > 1 then return false end -- shoved: fail
        end
        -- ...while the movers around it spread out.
        for e in world:each(Enemy, Position, Speed) do
            if e:get(Speed).value > 0 then
                local ep = e:get(Position)
                local dx, dy = ep.x - 500, ep.y - 500
                if dx * dx + dy * dy < 15 * 15 then return false end
            end
        end
        return true
    )"), "anchored hazards hold while the crowd around them spreads");
    reset();

    // --- Scenario 61: terrain — deterministic chunk generation ---------------
    {
        std::vector<shared::map::Obstacle> a;
        std::vector<shared::map::Obstacle> b;
        std::vector<shared::map::Obstacle> c;
        for (std::int32_t i = 2; i < 12 && a.empty(); ++i) { // find a populated chunk
            shared::map::obstacles_in(12345, i, 3, a);
        }
        bool same = !a.empty();
        for (std::int32_t i = 2; i < 12 && b.size() < a.size(); ++i) {
            shared::map::obstacles_in(12345, i, 3, b);
            shared::map::obstacles_in(99999, i, 3, c);
        }
        same = same && a.size() <= b.size();
        for (std::size_t i = 0; i < a.size() && same; ++i) {
            same = a[i].x == b[i].x && a[i].y == b[i].y && a[i].r == b[i].r;
        }
        bool differs = a.size() != c.size();
        for (std::size_t i = 0; i < a.size() && i < c.size() && !differs; ++i) {
            differs = a[i].x != c[i].x || a[i].y != c[i].y;
        }
        check(same, "one seed generates the same chunk twice");
        check(differs, "another seed generates another world");
        check([&] { // the spawn neighborhood stays clear
            std::vector<shared::map::Obstacle> spawn;
            for (std::int32_t j = -1; j <= 1; ++j) {
                for (std::int32_t i = -1; i <= 1; ++i) {
                    shared::map::obstacles_in(12345, i, j, spawn);
                }
            }
            return spawn.empty();
        }(), "the origin spawn area has no obstacles");
        check([&] { // biome density: forests grow thicker than plains
            float sum[3] = { 0.0f, 0.0f, 0.0f };
            int chunks[3] = { 0, 0, 0 };
            std::vector<shared::map::Obstacle> tmp;
            for (std::int32_t j = 2; j < 22; ++j) {
                for (std::int32_t i = 2; i < 22; ++i) {
                    const std::uint8_t biome = shared::map::biome_at(
                      12345, (static_cast<float>(i) + 0.5f) * shared::map::chunk_size,
                      (static_cast<float>(j) + 0.5f) * shared::map::chunk_size);
                    tmp.clear();
                    shared::map::obstacles_in(12345, i, j, tmp);
                    sum[biome] += static_cast<float>(tmp.size());
                    ++chunks[biome];
                }
            }
            return chunks[0] > 0 && chunks[1] > 0
                && (sum[1] / static_cast<float>(chunks[1]))
                     > (sum[0] / static_cast<float>(chunks[0]));
        }(), "forest regions are denser than plains");
    }

    // --- Scenario 62: terrain collision — pushout, and the clear circle ------
    {
        // Find a real obstacle of seed 12345 and hand its spot to Lua.
        std::vector<shared::map::Obstacle> obs;
        for (std::int32_t i = 2; i < 20 && obs.empty(); ++i) {
            shared::map::obstacles_in(12345, i, 5, obs);
        }
        check(!obs.empty(), "the probe seed has an obstacle to test against");
        lua["_OX"] = obs[0].x;
        lua["_OY"] = obs[0].y;
        lua["_OR"] = obs[0].r;
        lua.script(R"(
            for t in world:each(Terrain) do t:get(Terrain).seed = 12345 end
            for p in world:each(Player, Position) do
                local pp = p:get(Position)
                pp.x, pp.y = _OX, _OY -- dead center of the rock
            end
        )");
        step(0.2f);
        check(lua_bool(R"(
            for p in world:each(Player, Position) do
                local pp = p:get(Position)
                local dx, dy = pp.x - _OX, pp.y - _OY
                return dx * dx + dy * dy >= _OR * _OR -- ejected past the collider
            end
        )"), "a player inside an obstacle is pushed out");
        lua.script(R"(
            for p in world:each(Player) do p:set(Downed, { respawn_wave = 0 }) end
            local e = spawn_enemy(_OX, _OY, "core:bandit")
            e:get(Position).x, e:get(Position).y = _OX, _OY
        )");
        step(0.2f);
        check(lua_bool(R"(
            for e in world:each(Enemy, Position) do
                local ep = e:get(Position)
                local dx, dy = ep.x - _OX, ep.y - _OY
                return dx * dx + dy * dy >= _OR * _OR
            end
        )"), "an enemy inside an obstacle is pushed out");
        // Clear circle: a direct resolve check (in the live sim, core's arena
        // system OWNS the circle and zeroes it whenever no arena is up — the
        // sim-level behavior is scenario 63's).
        {
            shared::map::ChunkCache cache;
            float px = obs[0].x;
            float py = obs[0].y;
            const bool pushed_flat = shared::map::resolve_terrain(
              cache, 12345, px, py, 12.0f, obs[0].x, obs[0].y, 120.0f);
            check(!pushed_flat && px == obs[0].x && py == obs[0].y,
                  "the clear circle flattens obstacles (arena ground)");
        }
        lua.script(R"(
            for t in world:each(Terrain) do
                local terrain = t:get(Terrain)
                terrain.seed = 0
                terrain.clear_r = 0
            end
        )");
        reset();
    }

    // --- Scenario 63: arena fights write the terrain clear circle ------------
    lua.script(R"(
        for t in world:each(Terrain) do
            local terrain = t:get(Terrain)
            terrain.seed = 12345
            terrain.clear_r = 0
        end
        spawn_enemy(300, 0, "core:boss") -- Frog King carries C.Arena
    )");
    step(0.2f);
    check(lua_bool(R"(
        for t in world:each(Terrain) do return t:get(Terrain).clear_r > 0 end
    )"), "an arena fight flattens its ground");
    lua.script(R"(
        local C = import("core")
        for b in world:each(C.Boss, Health) do b:get(Health).current = 0 end
    )");
    step(0.3f);
    check(lua_bool(R"(
        for t in world:each(Terrain) do return t:get(Terrain).clear_r == 0 end
    )"), "the clearing lifts when the boss dies");
    lua.script(R"(for t in world:each(Terrain) do t:get(Terrain).seed = 0 end)");
    reset();

    // --- Scenario 66: water — deterministic ponds, dry spawn, hard walls -----
    {
        // Find a pond of seed 12345 (scan outward past the spawn ramp).
        float wx = 0.0f;
        float wy = 0.0f;
        bool found = false;
        for (float y = 1600.0f; y < 20000.0f && !found; y += 48.0f) {
            for (float x = 1600.0f; x < 20000.0f && !found; x += 48.0f) {
                if (shared::map::water_field(12345, x, y)
                    > shared::map::water_threshold + 0.015f) { // deep-ish, not shore
                    wx = x;
                    wy = y;
                    found = true;
                }
            }
        }
        check(found, "the probe seed has a pond to test against");
        check([&] { // the radial ramp keeps the whole spawn neighborhood dry
            for (float y = -1200.0f; y <= 1200.0f; y += 32.0f) {
                for (float x = -1200.0f; x <= 1200.0f; x += 32.0f) {
                    if (shared::map::water_at(12345, x, y)) { return false; }
                }
            }
            return true;
        }(), "the origin spawn area has no water");
        check([&] { // the generator filter: nothing grows in (or right at) a pond
            std::vector<shared::map::Obstacle> obs;
            for (std::int32_t j = 2; j < 30; ++j) {
                for (std::int32_t i = 2; i < 30; ++i) {
                    shared::map::obstacles_in(12345, i, j, obs);
                }
            }
            for (const auto& ob : obs) {
                if (shared::map::water_at(12345, ob.x, ob.y)) { return false; }
            }
            return !obs.empty();
        }(), "no obstacle stands in a pond");
        // A player dropped mid-pond is walked out to the shore in one tick.
        lua["_WX"] = wx;
        lua["_WY"] = wy;
        lua.script(R"(
            for t in world:each(Terrain) do t:get(Terrain).seed = 12345 end
            for p in world:each(Player, Position) do
                local pp = p:get(Position)
                pp.x, pp.y = _WX, _WY
            end
        )");
        step(0.1f);
        const auto player_dry = [&] {
            lua.script(R"(
                for p in world:each(Player, Position) do
                    local pp = p:get(Position)
                    _PX, _PY = pp.x, pp.y
                end
            )");
            return !shared::map::water_at(12345, lua["_PX"].get<float>(),
                                          lua["_PY"].get<float>());
        };
        check(player_dry(), "a player dropped in a pond is ejected to the shore");
        // Hard wall: walking INTO the pond grinds on the shore, never enters.
        lua.script(R"(
            for p in world:each(Player, Position, Velocity) do
                local pp = p:get(Position)
                local v = p:get(Velocity)
                local dx, dy = _WX - pp.x, _WY - pp.y
                local len = math.sqrt(dx * dx + dy * dy)
                v.dx, v.dy = dx / len * 240, dy / len * 240
            end
        )");
        bool stayed_dry = true;
        for (int i = 0; i < 120; ++i) { // poll per tick: never a wet frame
            world.step(1.0f / 120.0f);
            stayed_dry = stayed_dry && player_dry();
        }
        check(stayed_dry, "water is a hard wall to walkers");
        // No dash-crossing either: a burst aimed at the pond center stays dry.
        lua.script(R"(
            for p in world:each(Player, Position, Velocity, Dash) do
                local pp = p:get(Position)
                local v = p:get(Velocity)
                v.dx, v.dy = 0, 0
                local d = p:get(Dash)
                local dx, dy = _WX - pp.x, _WY - pp.y
                local len = math.sqrt(dx * dx + dy * dy)
                d.dir_x, d.dir_y = dx / len, dy / len
                d.burst_remaining = 0.25
            end
        )");
        stayed_dry = true;
        for (int i = 0; i < 60; ++i) {
            world.step(1.0f / 120.0f);
            stayed_dry = stayed_dry && player_dry();
        }
        check(stayed_dry, "a dash cannot cross the water line");
        // An enemy spawned mid-pond walks out too.
        lua.script(R"(
            local e = spawn_enemy(_WX, _WY, "core:bandit")
            e:get(Position).x, e:get(Position).y = _WX, _WY
        )");
        step(0.2f);
        check(lua_bool(R"(
            for e in world:each(Enemy, Position) do
                local ep = e:get(Position)
                _EX, _EY = ep.x, ep.y
                return true
            end
            return false
        )") && !shared::map::water_at(12345, lua["_EX"].get<float>(),
                                      lua["_EY"].get<float>()),
              "an enemy spawned in a pond is ejected to the shore");
        // The clear circle suppresses water like it does obstacles.
        {
            shared::map::ChunkCache cache;
            float px = wx;
            float py = wy;
            const bool pushed = shared::map::resolve_terrain(cache, 12345, px, py, 12.0f,
                                                             wx, wy, 300.0f);
            check(!pushed && px == wx && py == wy, "an arena decree dries the pond");
        }
        // Shore steering: a walker whose straight line dives into the pond is
        // slid along the rim (never wet, never pinned) — put the player on the
        // far shore and let the bandit's own targeting drive it into the water.
        float west = wx;
        float east = wx;
        while (shared::map::water_at(12345, west, wy)) { west -= 8.0f; }
        while (shared::map::water_at(12345, east, wy)) { east += 8.0f; }
        lua["_EAST"] = east + 60.0f;
        lua["_WEST"] = west - 20.0f;
        lua.script(R"(
            for e in world:each(Enemy) do e:destroy() end
            for p in world:each(Player, Position, Velocity) do
                local pp = p:get(Position)
                pp.x, pp.y = _EAST, _WY
                local v = p:get(Velocity)
                v.dx, v.dy = 0, 0
            end
            local e = spawn_enemy(_WEST, _WY, "core:bandit")
            e:get(Position).x, e:get(Position).y = _WEST, _WY
        )");
        bool enemy_dry = true;
        for (int i = 0; i < 240; ++i) { // 2 s: skirting, polled per tick
            world.step(1.0f / 120.0f);
            lua.script(R"(
                for e in world:each(Enemy, Position) do
                    local ep = e:get(Position)
                    _EX, _EY = ep.x, ep.y
                end
            )");
            enemy_dry = enemy_dry
                     && !shared::map::water_at(12345, lua["_EX"].get<float>(),
                                               lua["_EY"].get<float>());
        }
        const float moved_x = lua["_EX"].get<float>() - (west - 20.0f);
        const float moved_y = std::abs(lua["_EY"].get<float>() - wy);
        check(enemy_dry && (moved_x > 40.0f || moved_y > 40.0f),
              "a blocked enemy skirts the pond instead of grinding the shore");
        lua.script(R"(for t in world:each(Terrain) do t:get(Terrain).seed = 0 end)");
        reset();
    }

    // --- Scenario 64: supply dummy — its own loot table -----------------------
    lua.script(R"(
        local C = import("core")
        local crate = spawn_enemy(400, 0, "core:crate")
        crate:get(Health).current = 0
    )");
    step(0.2f);
    check(lua_bool(R"(
        local C = import("core")
        for e in world:each(C.CrateLoot) do return false end -- consumed
        for orb in world:each(C.Xp) do return true end       -- paid orbs...
        for heart in world:each(C.Heal) do return true end   -- ...or a heart
        return false
    )"), "a broken supply dummy pays its loot table");
    reset();

    // --- Scenario 65: the poi spawner salts the map ---------------------------
    // POIs are gated to wave >= 2 (which also keeps them out of every other
    // scenario here — the suite runs at wave 1); cover a full roll interval.
    lua.script(R"(world:set_wave(2))");
    step(31.0f);
    check(lua_bool(R"(
        local C = import("core")
        for e in world:each(C.CrateLoot) do return true end
        for e in world:each(C.Ambush) do return true end -- the 10% mimic roll
        return false
    )"), "the poi spawner placed a crate (or its mimic trap)");
    lua.script(R"(world:set_wave(1))");
    reset();

    // --- Scenario 26 (LAST: adds a 2nd player): co-op health scaling --------
    float solo_hp = 0.0f;
    lua.script(R"(
        local e = spawn_enemy(500, 500, "core:bandit")
        _SOLO_HP = e:get(Health).max
    )");
    solo_hp = lua["_SOLO_HP"].get<float>();
    reset();
    const core::Entity p2 = create_player(world.registry(), 40, 0);
    host.events().emit("on_player_spawn",
                       mod::EntityHandle{ .reg = &world.registry(), .entity = p2 });
    lua.script(R"(
        local e = spawn_enemy(500, 500, "core:bandit")
        _DUO_HP = e:get(Health).max
        local b = spawn_enemy(600, 600, "core:miniboss")
        _DUO_BOSS = b:get(Health).max
    )");
    const float duo_hp = lua["_DUO_HP"].get<float>();
    const float duo_boss = lua["_DUO_BOSS"].get<float>();
    check(duo_hp > solo_hp * 1.4f && duo_hp < solo_hp * 1.6f,
          "trash scales +50% with a second player");
    check(duo_boss > 500.0f * 1.6f && duo_boss < 500.0f * 1.8f,
          "boss scales +70% with a second player");
    reset();

    // --- Scenario 35: kill credit lands on the SHOOTER only (2 players) -----
    lua["_P1"] = static_cast<double>(player);
    lua.script(R"(
        local C = import("core")
        for p in world:each(Player, AimState) do
            p:set(RunStats, {})
            local a = p:get(AimState)
            if p:id() == _P1 then a.dx, a.dy, a.firing = 1, 0, 1 else a.firing = 0 end
        end
        local e = spawn_enemy(120, 0, "core:bandit")
        e:get(Health).current = 1
        e:get(Speed).value = 0
    )");
    step(1.0f);
    check(lua_bool(R"(
        local killer, bystander = false, true
        for p in world:each(Player, RunStats) do
            local rs = p:get(RunStats)
            if p:id() == _P1 then
                killer = rs.kills >= 1
            elseif rs.kills ~= 0 then
                bystander = false
            end
        end
        return killer and bystander
    )"), "kill credit lands on the shooter only");

    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
