-- mods/core/components.lua — Lua-defined components (networked = synced to clients).
return function(mod)
    mod:define_component("core:aura", { "radius", "per_second" }, { networked = true })
    mod:define_component("core:slow", { "radius", "factor" }, { networked = true })
end
