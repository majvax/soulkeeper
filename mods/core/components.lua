-- mods/core/components.lua — Lua-defined components (networked = synced to clients).
return function(mod)
    mod:define_component("core:aura", { "radius", "per_second" }, { networked = true })
    mod:define_component("core:slow", { "radius", "factor" }, { networked = true })
    -- Ranged-attacker state (sim-only): stands off at `standoff`, fires a
    -- hostile projectile every `cooldown` s at players within `range`.
    mod:define_component("core:ranged",
        { "range", "standoff", "cooldown", "bullet_speed", "damage", "timer" })
end
