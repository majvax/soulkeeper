-- mods/core/mod.lua
--
-- Soulkeeper's built-in content, and the reference every mod copies. Split
-- across sibling files loaded with include() (relative to this folder).
-- main()'s return value is the plugin's EXPORTS: other plugins reach these
-- handles with `local core = import("core")`.
function main()
    local mod = register_mod("core", "Soulkeeper's built-in content", "majvax")

    local C = include("components.lua")(mod) -- component handles (Ranged, Aura, Slow, ...)
    include("systems.lua")(mod, C)
    include("upgrades.lua")(mod, C)
    include("objects.lua")(mod, C)
    include("enemies.lua")(mod, C)

    return C -- exports: the component library
end
