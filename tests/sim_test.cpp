// Standalone SDL-free sim test: boots the exact server pipeline (kernel world
// + mods/core Lua) headless, spawns scenarios, steps at 120 Hz and asserts on
// world state THROUGH the sim VM (import("core") reaches the mod's handles).
//
// Build (from the repo root, after a normal cmake build supplied the deps):
//   g++ -std=c++26 -O1 -I src -I build/_deps/sol2-src/include -I build/_deps/lua-src \
//       tests/sim_test.cpp src/shared/mod/lua_host.cpp src/shared/mod/registry.cpp \
//       src/shared/mod/sim_bindings.cpp build/liblua.a -o /tmp/sim_test
// Run from the repo root (load_dir("mods") is a relative path).
#include "shared/factory/player.hpp"
#include "shared/mod/lua_host.hpp"
#include "shared/mod/sim_bindings.hpp"
#include "shared/sim/game_world.hpp"
#include <cstdio>

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
    step(1.5f); // the player stands inside blink range
    check(lua_bool(R"(
        local n = 0
        for e in world:each(Enemy) do n = n + 1 end
        return n > 1
    )"), "game master summoned adds");
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
