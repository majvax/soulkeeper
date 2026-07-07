-- mods/core/mod.lua
--
-- Soulkeeper's built-in content — and, since the mod API v2 migration, the
-- ENTIRE game logic: weapons, bullets, contact damage, deaths, drops, pickups
-- are Lua systems over kernel services. main()'s return value is the plugin's
-- EXPORTS: other plugins reach these handles with `import("core")`.
function main()
    local mod = register_mod("core", "Soulkeeper's built-in content", "majvax")

    ---@type core.Components
    local C = include("components.lua")(mod) -- component handles (Weapon, Bullet, ...)
    include("systems.lua")(mod, C)
    include("upgrades.lua")(mod, C)
    include("objects.lua")(mod, C)
    include("enemies.lua")(mod, C)
    include("levelup.lua")(mod, C) -- the level-up offer roll (mod:level_offer)

    -- The player's look: an animation pack (a folder of <Clip>_<N>x1.png
    -- strips). The engine handles frames, Idle/Move switching and facing.
    mod:player_sprite("assets/sprite/player")

    -- The player loadout: the kernel spawns bodies (position/hearts/dash),
    -- content decides what they fight with.
    mod:subscribe("on_player_spawn", function(e)
        e:set(C.Weapon, {})
        e:set(C.Crit, {})
    end)

    -- Console commands (typed as /name in the TAB console; host-only, run on
    -- the server with the invoking player's handle — numeric args are numbers).
    mod:command("givexp", "/givexp <amount>  -- add team XP", function(_, amount)
        world:add_xp(math.floor(tonumber(amount) or 0))
    end)
    mod:command("heal", "/heal  -- refill your hearts", function(player)
        local h = player:get(Hearts)
        if h then h.current = h.max end
    end)
    mod:command("wave", "/wave <n>  -- jump to wave n", function(_, n)
        world:set_wave(math.floor(tonumber(n) or 1))
    end)
    mod:command("stress", "/stress <count>  -- spawn a horde around you (perf testing)",
        function(player, count)
            local n = math.floor(tonumber(count) or 100)
            local pp = player:get(Position)
            local kinds = { "core:bandit", "core:scout", "core:brute" }
            for i = 1, n do
                local a = math.random() * 2 * math.pi
                local r = 400 + math.random() * 400
                spawn_enemy(pp.x + math.cos(a) * r, pp.y + math.sin(a) * r,
                            kinds[(i % #kinds) + 1])
            end
        end)

    -- World overlay: the revive progress arc over any downed player (their
    -- networked C.Revive rides the snapshot; runs per player entity per frame).
    mod:draw(function(ctx, view)
        local rv = view:get(C.Revive)
        if rv and rv.progress > 0 then
            ctx:circle(view.x, view.y, 26, 40, 40, 40, 160, 5)               -- track
            ctx:arc(view.x, view.y, 26, rv.progress, 255, 215, 100, 235, 5)  -- fill
        end
    end)

    -- HUD: the local player's stats, drawn by Lua in its own top-left panel.
    -- view:get reads our networked script comps (Weapon/Crit) AND our kernel
    -- stats (Speed/Hearts/Dash) — same field names as the server side.
    mod:hud(function(hud, view)
        hud:begin_panel("Stats")

        -- Team header: wave + level, then the shared XP bar (moved here from
        -- the old debug window). hud.wave/level/xp are fed by the engine.
        hud:text_colored(255, 205, 110, string.format("WAVE %d   LEVEL %d", hud.wave, hud.level))
        hud:bar(hud.xp, 90, 200, 255, 255) -- cyan XP bar, full content width
        hud:separator()

        -- Life: a row of heart icons (dim when empty), cached by the engine.
        local hearts = view:get(Hearts)
        if hearts then
            if hearts.current <= 0 then
                hud:text_colored(255, 90, 90, "DOWNED - respawning...")
            end
            for i = 0, hearts.max - 1 do
                if i > 0 then hud:same_line() end
                if i < hearts.current then
                    hud:image("assets/icons/hearth.png", 22)
                else
                    hud:image_tinted("assets/icons/hearth.png", 22, 60, 60, 60, 220)
                end
            end
        end

        -- Dash: a loading circle per charge (full = ready, arc = cooldown).
        local dash = view:get(Dash)
        if dash and dash.max_charges > 0 then
            for i = 0, dash.max_charges - 1 do
                if i > 0 then hud:same_line() end
                if i < dash.charges then
                    hud:pie(10, 1.0, 120, 210, 255, 255) -- ready
                elseif i == dash.charges and dash.cooldown_max > 0 then
                    local frac = (dash.cooldown_max - dash.cooldown) / dash.cooldown_max
                    hud:pie(10, frac, 120, 210, 255, 200) -- recharging
                else
                    hud:pie(10, 0.0, 120, 210, 255, 200)  -- empty ring
                end
            end
        end

        local speed = view:get(Speed)
        if speed then hud:text(string.format("Speed      %.0f", speed.value)) end

        local w = view:get(C.Weapon)
        if w then
            hud:separator()
            hud:text_colored(200, 220, 255, "-- Weapon --")
            hud:text(string.format("DMG        %.0f", w.damage))
            hud:text(string.format("Fire rate  %.0f ms", w.cooldown_max * 1000))
            hud:text(string.format("Bullet spd %.0f", w.bullet_speed))
            hud:text(string.format("Range      %.2f s", w.lifetime))
        end
        local c = view:get(C.Crit)
        if c then
            hud:text(string.format("Crit       %.0f%% x%.2f", c.chance * 100, c.multiplier))
        end

        hud:end_panel()
    end)

    return C -- exports: the component library
end
