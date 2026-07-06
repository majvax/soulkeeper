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

---@param mod Mod
---@param C core.Components
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

    -- Mini-boss: hand-spawned every 5 waves (weight 0 = never rolls naturally).
    -- Huge, slow, very tanky bruiser — a wave milestone that scales like the rest.
    mod:enemy("miniboss", "Mini-Boss", {
        weight = 0,
        sprite = "assets/sprite/FrogBoss",
        scale = 2.6,
        tint = { 255, 150, 150 },           -- reddish: reads as "elite"
    })
        :component(Health, health(350))     -- ~6x a Brute; scales +5%/wave
        :component(Speed, speed(80))        -- lumbering
        :component(C.Touch, { hearts = 2 }) -- heavy contact (i-frames still apply)
        :component(Radius, { value = 32 })  -- big hitbox to match the size
        :component(XpReward, xp(20))        -- worth the fight

    -- THE boss: hand-spawned every 10 waves. A towering, golden Frog King —
    -- slow, enormously tanky, and every few seconds it blasts a radial NOVA of
    -- hostile bullets (C.Nova system). Its death guarantees drops (death system
    -- keys on C.Nova). Health scales like everything else, so wave 20's king
    -- is meaner than wave 10's.
    mod:enemy("boss", "Frog King", {
        weight = 0,
        sprite = "assets/sprite/FrogBoss",
        scale = 3.4,
        tint = { 255, 215, 120 },            -- gold: unmistakably THE boss
    })
        :component(Health, health(1200))     -- a proper health bar to chew through
        :component(Speed, speed(60))         -- walks, never runs
        :component(C.Touch, { hearts = 2 })
        :component(Radius, { value = 44 })
        :component(XpReward, xp(60))
        :component(C.Nova, {})               -- radial bullet rings (defaults)

    -- Wave milestones: every 10th wave THE boss, every other 5th a mini-boss,
    -- dropped on the spawn ring around a live player. Runs in the SIM VM only
    -- (on_wave_start is server-emitted); no spawn if everyone's downed.
    mod:subscribe("on_wave_start", function(wave)
        if wave % 5 ~= 0 then return end
        local id = (wave % 10 == 0) and "core:boss" or "core:miniboss"
        for p in world:each(Player, Position) do
            if not p:has(Downed) then
                local pp = p:get(Position)
                local a = math.random() * 2 * math.pi
                spawn_enemy(pp.x + math.cos(a) * 600, pp.y + math.sin(a) * 600, id)
                return
            end
        end
    end)
end
