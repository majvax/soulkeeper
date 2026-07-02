-- mods/core/mod.lua
--
-- Soulkeeper's built-in content, and the reference every mod copies. Split
-- across sibling files loaded with include() (relative to this folder). Objects
-- are acquired once (engine-enforced) and each carries its OWN Lua-defined
-- component + system — no engine C++. See modding.md / types/library.lua.

function main()
    local mod = register_mod("core", "Soulkeeper's built-in content", "majvax")

    include("components.lua")(mod) -- core:aura, core:slow (networked)
    include("systems.lua")(mod)    -- tick the aura / slow each frame
    include("upgrades.lua")(mod)   -- stat upgrades
    include("objects.lua")(mod)    -- Onion, Frost Belt

    return mod
end
