-- mods/core/enemies.lua — the built-in enemy archetypes.
--
-- `weight` is the relative spawn chance and `stats` a function of the wave —
-- both re-evaluated once per wave (never per spawn). `damage` is discrete
-- HEARTS per hit (players have heart life with i-frames). Enemies default to
-- the shared enemy sprite, distinguished by tint + scale.

-- Standard wave scaling: health +15%/wave, a little extra speed (capped at
-- +40%), +1 xp every 4 waves. Contact hearts stay flat.
local function scale(base)
    return function(wave)
        local w = wave - 1
        return {
            health = base.health * (1 + 0.15 * w),
            speed = math.min(base.speed + 2 * w, base.speed * 1.4),
            damage = base.damage,
            radius = base.radius,
            xp = base.xp + math.floor(w / 4),
        }
    end
end

return function(mod)
    mod:add_enemy("core:bandit", "Bandit",
        scale({ health = 20, speed = 120, damage = 1, radius = 10, xp = 1 }),
        { weight = 6, tint = { 230, 120, 230 } }) -- magenta, always in the pool

    mod:add_enemy("core:scout", "Scout",
        scale({ health = 10, speed = 200, damage = 1, radius = 8, xp = 1 }),
        {
            weight = function(wave) return math.max(0, wave - 1) * 1.5 end, -- from wave 2
            scale = 0.8,
            tint = { 120, 220, 255 }, -- cyan
        })

    mod:add_enemy("core:brute", "Brute",
        scale({ health = 60, speed = 70, damage = 2, radius = 16, xp = 3 }),
        {
            weight = function(wave) return math.max(0, wave - 3) * 1.0 end, -- from wave 4
            scale = 1.5,
            tint = { 255, 110, 100 }, -- red
        })

    -- Ranged: hangs back and pelts players with hostile projectiles. Behaviour
    -- comes from the core:ranged component (attached below) + its systems.
    mod:add_enemy("core:slinger", "Slinger",
        scale({ health = 15, speed = 100, damage = 1, radius = 9, xp = 2 }),
        {
            weight = function(wave) return math.max(0, wave - 2) * 1.2 end, -- from wave 3
            scale = 0.9,
            tint = { 150, 255, 140 }, -- green
        })
        :component("core:ranged", {
            range = 340, standoff = 260,
            cooldown = 1.6, bullet_speed = 260, damage = 1, -- 1 heart per bullet
            timer = 1.0, -- brief grace period after spawning
        })
end
