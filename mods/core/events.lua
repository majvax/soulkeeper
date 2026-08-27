-- mods/core/events.lua — wave events: per-wave run modifiers (blood moon, fog,
-- horde rush, golden wave). The ENGINE only knows the registry (mod:wave_event
-- defs -> wire ids, the snapshot header's event byte, the client's banner/tint/
-- fog draws); the roll policy, the runner and every sim effect live HERE.
-- Returns EV, shared mutable state other core files read (systems.lua: the
-- death system multiplies XP orbs by EV.xp_mult).
---@param mod Mod
---@param C core.Components
return function(mod, C)
    local EV = { xp_mult = 1.0 }

    local active = nil  -- entry from world:wave_events() while an event runs
    local last_id = nil -- roll memory: never the same event twice in a row

    local function stop_event()
        if not active then return end
        if active.on_end then active.on_end() end
        active = nil
        world:set_event(nil)
    end
    local function start_event(entry, wave)
        stop_event() -- on_end always precedes the next on_start
        active = entry
        world:set_event(entry.id)
        if entry.on_start then entry.on_start(wave) end
    end

    -- Roll policy: ~22% from wave 4 on, never on milestone waves (%5 — the
    -- boss ladder owns those; also keeps WaveHold arena fights event-free),
    -- never the same event twice in a row. An event lasts exactly one wave:
    -- stop_event runs on EVERY wave start, so a milestone entrance ends the
    -- previous wave's event even though it never rolls a new one.
    mod:subscribe("on_wave_start", function(wave)
        stop_event()
        if wave < 4 or wave % 5 == 0 then return end
        if math.random() >= 0.22 then return end
        local pool = world:wave_events(wave)
        local total = 0
        for _, e in ipairs(pool) do
            if e.id ~= last_id then total = total + e.weight end
        end
        if total <= 0 then return end
        local roll = math.random() * total
        for _, e in ipairs(pool) do
            if e.id ~= last_id then
                roll = roll - e.weight
                if roll <= 0 then
                    last_id = e.id
                    start_event(e, wave)
                    return
                end
            end
        end
    end)

    -- Runner: per-tick during(dt). GameStats.event (world:event) is the source
    -- of truth — reset_run zeroes it C++-side with no Lua event fired, so a
    -- stale event from the previous run self-clears on the new run's first tick.
    mod:system("wave_event_runner", { phase = "update" }, function(dt)
        if not active then return end
        if world:event() == nil then -- run was reset under us
            active = nil
            EV.xp_mult = 1.0
            return
        end
        if active.during then active.during(dt) end
    end)

    mod:command("event", "/event <name|off>  -- force a wave event", function(_, name)
        if name == nil or name == "off" then
            stop_event()
            return
        end
        local id = string.find(tostring(name), ":") and tostring(name)
                   or ("core:" .. tostring(name))
        for _, e in ipairs(world:wave_events(world:wave())) do
            if e.id == id then
                start_event(e, world:wave())
                return
            end
        end
    end)

    -- A random live player's position (spawn anchor), or nil.
    local function anchor()
        local ps = {}
        for p in world:each(Player, Position) do
            if not p:has(Downed) then ps[#ps + 1] = p end
        end
        if #ps == 0 then return nil end
        return ps[math.random(#ps)]:get(Position)
    end
    local function ring(pp, r)
        local a = math.random() * 2 * math.pi
        return pp.x + math.cos(a) * r, pp.y + math.sin(a) * r
    end

    ------------------------------------------------------------------ roster
    -- Blood Moon: kills pay DOUBLE XP, but gold elites keep arriving. The XP
    -- doubling is baked into orbs at kill time (death system reads EV.xp_mult)
    -- so Greed still multiplies on top at pickup.
    local blood_timer = 0
    mod:wave_event("blood_moon", "BLOOD MOON", {
        tint = { 170, 20, 20, 44 },
        weight = 1,
        on_start = function()
            EV.xp_mult = 2.0
            blood_timer = 3.0
        end,
        during = function(dt)
            blood_timer = blood_timer - dt
            if blood_timer > 0 then return end
            blood_timer = 6.0
            local pp = anchor()
            if not pp then return end
            local elites = { "core:bandit_elite", "core:scout_elite", "core:marauder_elite" }
            local x, y = ring(pp, 600)
            spawn_enemy(x, y, elites[math.random(#elites)])
        end,
        on_end = function() EV.xp_mult = 1.0 end,
    })

    -- Fog: purely visual — the client draws a vision circle; threats appear
    -- late. No sim callbacks at all.
    mod:wave_event("fog", "FOG", { vision = 320, weight = 1 })

    -- Horde Rush: extra fodder pumped on top of the server's normal spawner.
    -- Capped on live enemies so a stalled team can't accumulate past the
    -- kernel's snapshot budget (Lua spawn_enemy bypasses the C++ cap).
    local horde_timer = 0
    mod:wave_event("horde_rush", "HORDE RUSH", {
        tint = { 120, 80, 20, 26 },
        weight = 1,
        on_start = function() horde_timer = 1.0 end,
        during = function(dt)
            horde_timer = horde_timer - dt
            if horde_timer > 0 then return end
            horde_timer = 2.0
            local enemies = 0
            for _ in world:each(Enemy) do enemies = enemies + 1 end
            if enemies >= 500 then return end
            local pp = anchor()
            if not pp then return end
            for _ = 1, 3 do
                local x, y = ring(pp, 600)
                spawn_enemy(x, y, math.random() < 0.5 and "core:bandit" or "core:scout")
            end
        end,
    })

    -- Golden Wave: supply crates rain for the wave. Its own cadence — zero
    -- coupling with poi_spawner (which keeps its slow 20-30 s drip alongside).
    local gold_timer = 0
    mod:wave_event("golden_wave", "GOLDEN WAVE", {
        tint = { 220, 180, 60, 22 },
        weight = 1,
        on_start = function() gold_timer = 2.0 end,
        during = function(dt)
            gold_timer = gold_timer - dt
            if gold_timer > 0 then return end
            gold_timer = 5.0
            local crates = 0
            for _ in world:each(C.CrateLoot) do crates = crates + 1 end
            if crates >= 10 then return end
            local pp = anchor()
            if not pp then return end
            local x, y = ring(pp, 500 + math.random() * 300)
            spawn_enemy(x, y, "core:crate")
        end,
    })

    return EV
end
