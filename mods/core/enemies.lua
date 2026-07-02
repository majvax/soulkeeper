-- mods/core/enemies.lua — the built-in enemy archetypes.
--
-- `weight` is the relative spawn chance, re-evaluated once per wave (a plain
-- number or a function(wave)). Enemies default to the shared enemy sprite,
-- distinguished by tint + scale.

return function(mod)
    mod:add_enemy("core:bandit", "Bandit",
        { health = 20, speed = 120, damage = 20, radius = 10, xp = 1 },
        { weight = 6, tint = { 230, 120, 230 } }) -- magenta, always in the pool

    mod:add_enemy("core:scout", "Scout",
        { health = 10, speed = 200, damage = 15, radius = 8, xp = 1 },
        {
            weight = function(wave) return math.max(0, wave - 1) * 1.5 end, -- from wave 2
            scale = 0.8,
            tint = { 120, 220, 255 }, -- cyan
        })

    mod:add_enemy("core:brute", "Brute",
        { health = 60, speed = 70, damage = 35, radius = 16, xp = 3 },
        {
            weight = function(wave) return math.max(0, wave - 3) * 1.0 end, -- from wave 4
            scale = 1.5,
            tint = { 255, 110, 100 }, -- red
        })
end
