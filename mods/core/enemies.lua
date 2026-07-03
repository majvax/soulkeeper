-- mods/core/enemies.lua — the built-in enemy archetypes.
--
-- An enemy is a pure component bag: mod:enemy declares identity + visuals +
-- spawn weight, then :component(Handle, fields | fun(wave)) attaches
-- everything gameplay-defining. Function inits re-resolve once per wave —
-- that's the wave-scaling mechanism. Damage is discrete HEARTS per hit
-- (players have heart life with i-frames).

-- Health scaling: +15% per wave.
local function health(base)
    return function(wave)
        local h = base * (1 + 0.15 * (wave - 1))
        return { current = h, max = h }
    end
end

-- Speed scaling: +2 px/s per wave, capped at +40%.
local function speed(base)
    return function(wave)
        return { value = math.min(base + 2 * (wave - 1), base * 1.4) }
    end
end

-- XP scaling: +1 every 4 waves.
local function xp(base)
    return function(wave)
        return { value = base + math.floor((wave - 1) / 4) }
    end
end

return function(mod, C)
    mod:enemy("bandit", "Bandit", { weight = 6, tint = { 230, 120, 230 } }) -- magenta, always
        :component(Health, health(20))
        :component(Speed, speed(120))
        :component(Damage, { per_second = 1 }) -- 1 heart per contact hit
        :component(Radius, { value = 10 })
        :component(XpReward, xp(1))

    mod:enemy("scout", "Scout", {
            weight = function(wave) return math.max(0, wave - 1) * 1.5 end, -- from wave 2
            scale = 0.8,
            tint = { 120, 220, 255 }, -- cyan
        })
        :component(Health, health(10))
        :component(Speed, speed(200))
        :component(Damage, { per_second = 1 })
        :component(Radius, { value = 8 })
        :component(XpReward, xp(1))

    mod:enemy("brute", "Brute", {
            weight = function(wave) return math.max(0, wave - 3) * 1.0 end, -- from wave 4
            scale = 1.5,
            tint = { 255, 110, 100 }, -- red
        })
        :component(Health, health(60))
        :component(Speed, speed(70))
        :component(Damage, { per_second = 2 }) -- heavy: 2 hearts
        :component(Radius, { value = 16 })
        :component(XpReward, xp(3))

    -- Ranged: hangs back and pelts players with hostile projectiles. Behaviour
    -- comes from the core Ranged component + its systems (see systems.lua).
    mod:enemy("slinger", "Slinger", {
            weight = function(wave) return math.max(0, wave - 2) * 1.2 end, -- from wave 3
            scale = 0.9,
            tint = { 150, 255, 140 }, -- green
        })
        :component(Health, health(15))
        :component(Speed, speed(100))
        :component(Damage, { per_second = 1 })
        :component(Radius, { value = 9 })
        :component(XpReward, xp(2))
        :component(C.Ranged, {}) -- all defaults (range 340, 1 heart bullets, ...)
end
