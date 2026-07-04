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

    return C -- exports: the component library
end
