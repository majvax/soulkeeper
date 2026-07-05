-- mods/core/mod.lua
--
-- Soulkeeper's built-in content — and, since the mod API v2 migration, the
-- ENTIRE game logic: weapons, bullets, contact damage, deaths, drops, pickups
-- are Lua systems over kernel services. main()'s return value is the plugin's
-- EXPORTS: other plugins reach these handles with `import("core")`.
function main()
    local mod = register_mod("core", "Soulkeeper's built-in content", "majvax")

    local C = include("components.lua")(mod) -- component handles (Weapon, Bullet, ...)
    include("systems.lua")(mod, C)
    include("upgrades.lua")(mod, C)
    include("objects.lua")(mod, C)
    include("enemies.lua")(mod, C)

    -- The player's look: an animation pack (a folder of <Clip>_<N>x1.png
    -- strips). The engine handles frames, Idle/Move switching and facing.
    mod:player_sprite("assets/sprite/player")

    -- The player loadout: the kernel spawns bodies (position/hearts/dash),
    -- content decides what they fight with.
    mod:subscribe("on_player_spawn", function(e)
        e:set(C.Weapon, {})
        e:set(C.Crit, {})
    end)

    -- HUD: the local player's stats, drawn by Lua in its own top-left panel.
    -- view:get reads our networked script comps (Weapon/Crit) AND our kernel
    -- stats (Speed/Hearts/Dash) — same field names as the server side.
    mod:hud(function(hud, view)
        hud:begin_panel("Stats")

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
