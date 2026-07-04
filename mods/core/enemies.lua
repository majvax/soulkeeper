-- mods/core/enemies.lua — the built-in enemy archetypes.
--
-- An enemy is a pure component bag: mod:enemy declares identity + visuals +
-- spawn weight, then :component(Handle, fields | fun(wave)) attaches
-- everything gameplay-defining. Function inits re-resolve once per wave —
-- that's the wave-scaling mechanism. Damage is discrete HEARTS per hit
-- (players have heart life with i-frames).
--
-- Visuals: `sprite` names an animation-pack FOLDER (of <Clip>_<N>x1.png
-- strips) — the engine slices frames, plays Idle/Move and flips for facing
-- on its own. `scale` is the on-screen height factor.

-- Health scaling: +15% per wave.
local function health(base)
    return function(wave)
        local h = base * (1 + 0.05 * (wave - 1))
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
    mod:enemy("bandit", "Bandit", {
        weight = 6,
        sprite = "assets/sprite/Goblin_Regular_01 (Green Skinned)",
        scale = 0.9,
    })
        :component(Health, health(20))
        :component(Speed, speed(120))
        :component(C.Touch, { hearts = 1 })
        :component(Radius, { value = 10 })
        :component(XpReward, xp(1))

    mod:enemy("scout", "Scout", {
        weight = function(wave) return math.max(0, wave - 1) * 1.5 end,
        sprite = "assets/sprite/Monsterfly_01",
        scale = 0.8,
    })
        :component(Health, health(10))
        :component(Speed, speed(200))
        :component(C.Touch, { hearts = 1 })
        :component(Radius, { value = 8 })
        :component(XpReward, xp(1))

    mod:enemy("mushroom", "Mushroom", {
        weight = function(wave) return math.max(0, wave - 1) * 0.8 end,
        sprite = "assets/sprite/Mushroom",
        scale = 1.0,
    })
        :component(Health, health(40))
        :component(Speed, speed(55))
        :component(C.Touch, { hearts = 1 })
        :component(Radius, { value = 12 })
        :component(XpReward, xp(2))

    mod:enemy("brute", "Brute", {
        weight = function(wave) return math.max(0, wave - 3) * 1.0 end,
        sprite = "assets/sprite/RhinoMonster_01_Regular",
        scale = 1.5,
    })
        :component(Health, health(60))
        :component(Speed, speed(70))
        :component(C.Touch, { hearts = 2 }) -- heavy hit
        :component(Radius, { value = 16 })
        :component(XpReward, xp(3))

    mod:enemy("slinger", "Slinger", {
        weight = function(wave) return math.max(0, wave - 2) * 1.2 end,
        sprite = "assets/sprite/Orc_Archer_01 (Green Skinned)",
        scale = 1.0,
    })
        :component(Health, health(15))
        :component(Speed, speed(100))
        :component(C.Touch, { hearts = 1 })
        :component(Radius, { value = 9 })
        :component(XpReward, xp(2))
        :component(C.Ranged, {})

    mod:enemy("slasher", "Slasher", {
        weight = function(wave) return math.max(0, wave - 4) * 1.0 end,
        sprite = "assets/sprite/MonsterSlasher_01",
        scale = 1.2,
    })
        :component(Health, health(45))
        :component(Speed, speed(150)) -- fast AND heavy: late-wave pressure
        :component(C.Touch, { hearts = 2 })
        :component(Radius, { value = 12 })
        :component(XpReward, xp(3))

    mod:enemy("vampire", "Vampire Archer", {
        weight = function(wave) return math.max(0, wave - 5) * 0.8 end,
        sprite = "assets/sprite/Vampire_Archer_01",
        scale = 1.1,
    })
        :component(Health, health(30))
        :component(Speed, speed(110))
        :component(C.Touch, { hearts = 1 })
        :component(Radius, { value = 10 })
        :component(XpReward, xp(4))
        :component(C.Ranged, { range = 380, cooldown = 1.2, bullet_speed = 320 })
end
