-- mods/core/components.lua — Lua-defined components.
--
-- mod:component returns a HANDLE: store it, pass it to e:get/set, world:each,
-- enemy :component(...), view:get(...). Fields declare their defaults;
-- networked components are synced to clients for draw hooks.
return function(mod)
    local C = {}

    C.Aura = mod:component("aura", { radius = 120, per_second = 25 }, { networked = true })
    C.Slow = mod:component("slow", { radius = 140, factor = 0.5 }, { networked = true })

    -- Ranged-attacker state: stands off at `standoff`, fires a hostile
    -- projectile every `cooldown` s at players within `range`.
    C.Ranged = mod:component("ranged", {
        range = 340, standoff = 260,
        cooldown = 1.6, bullet_speed = 260, damage = 1,
        timer = 1.0, -- brief grace period after spawning
    })

    return C
end
