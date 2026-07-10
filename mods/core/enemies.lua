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
-- on its own. `scale` is the on-screen height factor. Render.fx picks the
-- special clips: 1 = attack, 2 = charge loop, 3 = telegraph (Prepare pose).

-- Health scaling: compound +7% per wave (~1.9x @10, ~3.6x @20). The player's
-- DPS compounds too (multishot/pierce/crit stack multiplicatively), so late
-- waves need more than the old linear ramp to stay threatening.
local function health(base)
    return function(wave)
        local h = base * (1.07 ^ (wave - 1))
        return { current = h, max = h }
    end
end

-- Speed scaling: +2 px/s per wave, capped at +40%.
local function speed(base)
    return function(wave)
        return { value = math.min(base + 2 * (wave - 1), base * 1.4) }
    end
end

-- XP scaling: +1 every 3 waves (the run is 50 waves now — leveling has to
-- keep pace with the compound HP curve).
local function xp(base)
    return function(wave)
        return { value = base + math.floor((wave - 1) / 3) }
    end
end

---@param mod Mod
---@param C core.Components
return function(mod, C)
    -- Register an archetype and (unless no_elite) its gold ELITE twin:
    -- 4x health/XP, 1.4x size, always drops a heart. Elites start rolling at
    -- wave 8 and ramp toward ~45% of their base archetype's weight by wave
    -- ~18 — the same silhouette the player already knows, suddenly worth
    -- respecting; deep waves are elite-heavy on purpose.
    local ELITE_TINT = { 255, 200, 90 }
    local function register(a)
        local function apply(b, mult)
            b:component(Health, health(a.hp * mult))
            b:component(Speed, speed(a.speed))
            b:component(C.Touch, { hearts = a.touch })
            b:component(Radius, { value = a.radius })
            b:component(XpReward, xp(a.xp * mult))
            if a.ranged then b:component(C.Ranged, a.ranged) end
            if a.bomber then b:component(C.Bomber, a.bomber) end
            if a.lunge then b:component(C.Lunge, a.lunge) end
        end
        apply(mod:enemy(a.id, a.label, {
            weight = a.weight, sprite = a.sprite, scale = a.scale, tint = a.tint,
        }), 1)
        if not a.no_elite then
            local b = mod:enemy(a.id .. "_elite", "Elite " .. a.label, {
                weight = function(wave)
                    local w = type(a.weight) == "function" and a.weight(wave) or a.weight
                    return w * math.min(0.45, math.max(0, 0.045 * (wave - 7)))
                end,
                sprite = a.sprite, scale = a.scale * 1.4, tint = ELITE_TINT,
            })
            apply(b, 4)
            b:component(C.EliteDrop, {})
        end
    end

    ------------------------------------------------------------------- fodder
    register { -- the wave-1 baseline; fades but never disappears
        id = "bandit", label = "Bandit",
        sprite = "assets/sprite/Goblin_Regular_01 (Green Skinned)", scale = 0.9,
        weight = function(wave) return math.max(1.5, 7 - 0.35 * (wave - 1)) end,
        hp = 20, speed = 120, touch = 1, radius = 10, xp = 1,
    }
    register { -- red-skinned mid-game bandit replacement
        id = "marauder", label = "Marauder",
        sprite = "assets/sprite/Goblin_Regular_03 (Red Skinned)", scale = 1.0,
        weight = function(wave) return math.max(0, wave - 8) * 1.2 end,
        hp = 45, speed = 135, touch = 1, radius = 10, xp = 2,
    }
    register {
        id = "scout", label = "Scout",
        sprite = "assets/sprite/Monsterfly_01", scale = 0.8,
        weight = function(wave) return math.max(0, wave - 1) * 1.5 end,
        hp = 10, speed = 200, touch = 1, radius = 8, xp = 1,
    }
    register {
        id = "mushroom", label = "Mushroom",
        sprite = "assets/sprite/Mushroom", scale = 1.0,
        weight = function(wave) return math.max(0, wave - 1) * 0.8 end,
        hp = 45, speed = 55, touch = 1, radius = 12, xp = 2,
    }

    ---------------------------------------------------------------- specials
    register { -- suicide barrel goblin: rushes, stops, fuses, BOOM
        id = "bomber", label = "Bomber",
        sprite = "assets/sprite/Goblin_Barrel_01 (Green Skinned)", scale = 0.95,
        weight = function(wave) return math.max(0, wave - 1) * 0.9 end,
        hp = 25, speed = 145, touch = 1, radius = 10, xp = 2,
        bomber = {},
    }
    register { -- melee lunger: freezes, telegraphs, bursts along the lock line
        id = "berserker", label = "Berserker",
        sprite = "assets/sprite/Orc_Barbare_01 (Green Skinned)", scale = 1.1,
        weight = function(wave) return math.max(0, wave - 4) * 1.0 end,
        hp = 50, speed = 110, touch = 2, radius = 12, xp = 3,
        lunge = {},
    }
    register {
        id = "slinger", label = "Slinger",
        sprite = "assets/sprite/Orc_Archer_01 (Green Skinned)", scale = 1.0,
        weight = function(wave) return math.max(0, wave - 2) * 1.2 end,
        hp = 15, speed = 100, touch = 1, radius = 9, xp = 2,
        ranged = {},
    }
    register { -- heavy sniper: long telegraph, long range, fat slow bullet
        id = "cyclop", label = "Cyclop",
        sprite = "assets/sprite/Cyclop_Archer_01", scale = 1.3,
        weight = function(wave) return math.max(0, wave - 7) * 0.7 end,
        hp = 80, speed = 85, touch = 1, radius = 14, xp = 4,
        ranged = { range = 520, standoff = 420, cooldown = 2.8, windup = 0.9,
                   bullet_speed = 300, variant = 3, bullet_radius = 9 },
    }
    register { -- walking tree turret: slow, tanky, 3-bullet fan
        id = "ent", label = "Ent",
        sprite = "assets/sprite/Ent_LVL1", scale = 1.4,
        weight = function(wave) return math.max(0, wave - 6) * 0.6 end,
        hp = 120, speed = 50, touch = 1, radius = 15, xp = 4,
        ranged = { range = 300, standoff = 200, cooldown = 2.4, windup = 0.7,
                   bullet_speed = 220, volley = 3, spread = 0.55 },
    }
    register {
        id = "slasher", label = "Slasher",
        sprite = "assets/sprite/MonsterSlasher_01", scale = 1.2,
        weight = function(wave) return math.max(0, wave - 5) * 1.0 end,
        hp = 45, speed = 150, touch = 2, radius = 12, xp = 3, -- fast AND heavy
    }
    register {
        id = "vampire", label = "Vampire Archer",
        sprite = "assets/sprite/Vampire_Archer_01", scale = 1.1,
        weight = function(wave) return math.max(0, wave - 5) * 0.8 end,
        hp = 30, speed = 110, touch = 1, radius = 10, xp = 4,
        ranged = { range = 380, cooldown = 1.2, bullet_speed = 320 },
    }

    ------------------------------------------------------------------ brutes
    -- One silhouette, three wave bands: the Rhino skins ARE the difficulty tell.
    register {
        id = "brute", label = "Brute",
        sprite = "assets/sprite/RhinoMonster_01_Regular", scale = 1.5,
        weight = function(wave) return math.max(0, wave - 3) * (wave < 11 and 1.0 or 0.4) end,
        hp = 60, speed = 70, touch = 2, radius = 16, xp = 3,
    }
    register {
        id = "brute_silver", label = "Silverback",
        sprite = "assets/sprite/RhinoMonster_02_Silver", scale = 1.6,
        weight = function(wave) return math.max(0, wave - 8) * 0.9 end,
        hp = 130, speed = 75, touch = 2, radius = 16, xp = 5,
    }
    register {
        id = "brute_gold", label = "Goldhorn",
        sprite = "assets/sprite/RhinoMonster_03_Gold", scale = 1.7,
        weight = function(wave) return math.max(0, wave - 13) * 0.8 end,
        hp = 260, speed = 80, touch = 2, radius = 17, xp = 8,
    }

    -------------------------------------------------------------- boss roster
    -- All weight 0 (hand-spawned by the milestone hook below); all carry the
    -- C.Boss tag — the death system pays a loot CHEST (the only object
    -- source) + guaranteed drops, revives the team, and checks the WIN_WAVE
    -- rule on any of them. Every %10 wave is an ARENA fight (1920x1080 rect,
    -- half-extents 960x540 = the `arena` opt AND the C.Nova fields).
    local ARENA_W, ARENA_H = 960, 540

    -- Wave 5 mini: the Rhino Charger — a boss-sized lunge (the Berserker's
    -- C.Lunge with monster numbers): long stomp telegraph, then a
    -- cross-field charge you sidestep or eat 2 hearts.
    mod:enemy("rhino_charger", "Rhino Charger", {
        weight = 0,
        sprite = "assets/sprite/RhinoMonster_04_Devil",
        scale = 2.2,
    })
        :component(Health, health(600))
        :component(Speed, speed(90))
        :component(C.Touch, { hearts = 2 })
        :component(Radius, { value = 26 })
        :component(XpReward, xp(25))
        :component(C.Lunge, { range = 560, windup = 0.85, speed = 560,
                              duration = 0.8, cooldown = 2.2, timer = 2.0 })
        :component(C.Boss, {})

    -- Wave 15 mini: the Frog Prince — a leaper. Telegraph (Prepare), LEAP to
    -- the locked spot (C.Lunge as a jump, fx=2 plays Jump_Full), and the
    -- landing is a SLAM: a ring of hostile bullets (C.Lunge.burst).
    mod:enemy("frog_prince", "Frog Prince", {
        weight = 0,
        sprite = "assets/sprite/FrogMonster",
        scale = 2.0,
        tint = { 170, 235, 160 },
    })
        :component(Health, health(700))
        :component(Speed, speed(95))
        :component(C.Touch, { hearts = 2 })
        :component(Radius, { value = 24 })
        :component(XpReward, xp(30))
        :component(C.Lunge, { range = 640, windup = 0.7, speed = 640, duration = 0.55,
                              cooldown = 2.0, timer = 2.0, burst = 10, burst_speed = 240 })
        :component(C.Boss, {})

    -- Ground hazards the planter bosses drop (weight 0, spawned by
    -- planter_sys). Real enemies reusing the full C.Bomber tech: they sit
    -- still (Speed 0), arm when a player closes in, and blast a bullet ring —
    -- SHOOT them to reclaim the ground.
    mod:enemy("mine", "Powder Keg", {
        weight = 0,
        sprite = "assets/sprite/Goblin_Barrel_02 (Blue Skinned)",
        scale = 0.7,
    })
        :component(Health, health(25))
        :component(Speed, speed(0))
        :component(C.Touch, { hearts = 1 })
        :component(Radius, { value = 10 })
        :component(XpReward, { value = 1 })
        :component(C.Bomber, { trigger = 85, fuse = 0.55, blast_bullets = 12,
                               blast_speed = 260, blast_range = 130 })
    mod:enemy("bramble", "Spore Pod", {
        weight = 0,
        sprite = "assets/sprite/Mushroom",
        scale = 0.8,
        tint = { 150, 235, 130 },
    })
        :component(Health, health(30))
        :component(Speed, speed(0))
        :component(C.Touch, { hearts = 1 })
        :component(Radius, { value = 10 })
        :component(XpReward, { value = 1 })
        :component(C.Bomber, { trigger = 95, fuse = 0.7, blast_bullets = 14,
                               blast_speed = 220, blast_range = 150 })

    -- Legacy mini-boss (rotation filler between scripted milestones):
    -- a huge, slow, very tanky bruiser.
    mod:enemy("miniboss", "Mini-Boss", {
        weight = 0,
        sprite = "assets/sprite/FrogBoss",
        scale = 2.6,
        tint = { 255, 150, 150 },           -- reddish: reads as "elite"
    })
        :component(Health, health(500))     -- scales like the rest
        :component(Speed, speed(80))        -- lumbering
        :component(C.Touch, { hearts = 2 }) -- heavy contact (i-frames still apply)
        :component(Radius, { value = 32 })  -- big hitbox to match the size
        :component(XpReward, xp(20))        -- worth the fight
        :component(C.Boss, {})

    -- Wave 10: the Frog King — a towering, golden bullet fountain. Radial
    -- NOVAS that RAGE as its health drops (C.Nova phases: < 50% faster/denser
    -- + aimed volleys, < 25% it speeds up).
    mod:enemy("boss", "Frog King", {
        weight = 0,
        sprite = "assets/sprite/FrogBoss",
        scale = 3.4,
        tint = { 255, 215, 120 },            -- gold: unmistakably THE boss
        arena = { ARENA_W, ARENA_H },
    })
        :component(Health, health(2000))     -- a proper health bar to chew through
        :component(Speed, speed(70))         -- walks, never runs
        :component(C.Touch, { hearts = 2 })
        :component(Radius, { value = 44 })
        :component(XpReward, xp(60))
        :component(C.Nova, { cooldown = 2.4, bullets = 20, bullet_speed = 230,
                             arena_w = ARENA_W, arena_h = ARENA_H })
        :component(C.Boss, {})

    -- Wave 20: the Bomb Lord — the arena SHRINKS around you: it carpets the
    -- ground with Powder Kegs (planter_sys) while lobbing aimed 3-volleys and
    -- the odd nova ring. Shoot the kegs or run out of floor.
    mod:enemy("bomb_lord", "Bomb Lord", {
        weight = 0,
        sprite = "assets/sprite/Goblin_Barrel_03 (Red Skinned)",
        scale = 2.3,
        arena = { ARENA_W, ARENA_H },
    })
        :component(Health, health(2600))
        :component(Speed, speed(80))
        :component(C.Touch, { hearts = 2 })
        :component(Radius, { value = 30 })
        :component(XpReward, xp(80))
        :component(C.Nova, { cooldown = 3.2, bullets = 14, bullet_speed = 220,
                             arena_w = ARENA_W, arena_h = ARENA_H })
        :component(C.Planter, { cooldown = 3.5, count = 3, kind = 1 })
        :component(C.Ranged, { volley = 3, spread = 0.4, windup = 0.5, range = 520,
                               standoff = 0, cooldown = 2.0, bullet_speed = 300 })
        :component(C.Boss, {})

    -- Wave 30: the Vampire Lord — a DPS check. Fast rings, bat swarms
    -- (C.Summon pool 2), it blinks out of reach and it REGENERATES: stop
    -- shooting and the fight rewinds.
    mod:enemy("vampire_lord", "Vampire Lord", {
        weight = 0,
        sprite = "assets/sprite/Vampire_Archer_01",
        scale = 2.4,
        arena = { ARENA_W, ARENA_H },
    })
        :component(Health, health(3200))
        :component(Speed, speed(95))
        :component(C.Touch, { hearts = 2 })
        :component(Radius, { value = 28 })
        :component(XpReward, xp(100))
        :component(C.Nova, { cooldown = 2.6, bullets = 16, bullet_speed = 240,
                             arena_w = ARENA_W, arena_h = ARENA_H })
        :component(C.Summon, { pool = 2, cooldown = 6.0, count = 4,
                               blink_range = 200, blink_dist = 320 })
        :component(C.Regen, function(wave) return { per_second = 30 * (1.07 ^ (wave - 1)) } end)
        :component(C.Boss, {})

    -- Wave 40: the Elder Ent — spiral bullet hell. Its novas ROTATE
    -- (C.Nova.spin), the gaps walk, wide 5-bullet fans sweep the lanes, and
    -- Spore Pods (planter_sys) grow where you'd want to dodge.
    mod:enemy("elder_ent", "Elder Ent", {
        weight = 0,
        sprite = "assets/sprite/Ent_LVL4",
        scale = 2.6,
        arena = { ARENA_W, ARENA_H },
    })
        :component(Health, health(4200))
        :component(Speed, speed(55))
        :component(C.Touch, { hearts = 2 })
        :component(Radius, { value = 32 })
        :component(XpReward, xp(120))
        :component(C.Nova, { cooldown = 1.8, bullets = 18, bullet_speed = 200, spin = 0.45,
                             arena_w = ARENA_W, arena_h = ARENA_H })
        :component(C.Planter, { cooldown = 5.0, count = 4, kind = 2 })
        :component(C.Ranged, { volley = 5, spread = 0.9, windup = 0.6, range = 560,
                               standoff = 0, cooldown = 2.6, bullet_speed = 260 })
        :component(C.Boss, {})

    -- Wave 50: THE GAME MASTER — the finale, and the WIN condition. It never
    -- walks (Speed 0): it BLINKS. Spiral raging novas + heavy aimed 5-volleys
    -- + ELITE summons (pool 3). Everything at once; kill it to win the run.
    mod:enemy("gamemaster", "Game Master", {
        weight = 0,
        sprite = "assets/sprite/GameMaster",
        scale = 2.8,
        arena = { ARENA_W, ARENA_H },
    })
        :component(Health, health(5000))
        :component(Speed, speed(0))         -- it blinks, it never runs
        :component(C.Touch, { hearts = 2 })
        :component(Radius, { value = 26 })
        :component(XpReward, xp(200))
        :component(C.Nova, { cooldown = 2.0, bullets = 22, bullet_speed = 240, spin = 0.3,
                             arena_w = ARENA_W, arena_h = ARENA_H })
        :component(C.Summon, { pool = 3, cooldown = 5.5, count = 3,
                               blink_range = 220, blink_dist = 360 })
        :component(C.Ranged, { volley = 5, spread = 0.7, windup = 0.35, range = 640,
                               standoff = 500, cooldown = 2.2, bullet_speed = 340,
                               variant = 3, bullet_radius = 8 })
        :component(C.Boss, {})

    -- Wave milestones: the scripted boss ladder, dropped on the spawn ring
    -- around a live player. Runs in the SIM VM only (on_wave_start is
    -- server-emitted); no spawn if everyone's downed.
    -- Every %10 wave is an ARENA: the trash horde despawns (no drops), the
    -- team is ringed around the fixed center, and the boss carries the kernel
    -- WaveHold tag — the wave clock stays frozen until it dies. The %5 minis
    -- roam free; between/past the table they rotate through the mini pool.
    local MILESTONES = {
        [5] = "core:rhino_charger",
        [10] = "core:boss",
        [15] = "core:frog_prince",
        [20] = "core:bomb_lord",
        [30] = "core:vampire_lord",
        [40] = "core:elder_ent",
        [50] = "core:gamemaster",
    }
    local MINI_ROTATION = { "core:rhino_charger", "core:frog_prince", "core:miniboss" }
    mod:subscribe("on_wave_start", function(wave)
        if wave % 5 ~= 0 then return end
        local is_boss = wave % 10 == 0
        local id = MILESTONES[wave]
            or (is_boss and "core:boss"
                or MINI_ROTATION[math.floor(wave / 5) % #MINI_ROTATION + 1])
        for p in world:each(Player, Position) do
            if not p:has(Downed) then
                local pp = p:get(Position)
                if is_boss then
                    -- ARENA entrance: the anchor player's spot becomes the
                    -- arena's FIXED center — the boss lands exactly there
                    -- (the client keys the camera/wall off its spawn point),
                    -- the team is ringed around it, the trash clears and
                    -- WaveHold freezes the wave clock. No escape.
                    local cx, cy = pp.x, pp.y -- capture: the loop below moves pp too
                    local i = 0
                    for other in world:each(Player, Position) do
                        local op = other:get(Position)
                        op.x = cx + math.cos(i * 2.1) * 180
                        op.y = cy + math.sin(i * 2.1) * 180
                        i = i + 1
                    end
                    local boss = spawn_enemy(cx, cy, id)
                    if boss then
                        boss:set(WaveHold, {})
                        local nova = boss:get(C.Nova)
                        nova.cx, nova.cy = cx, cy -- the rect never moves
                        for e in world:each(Enemy) do
                            if not e:has(C.Nova) then e:destroy() end -- clear the arena
                        end
                    end
                else
                    local a = math.random() * 2 * math.pi
                    spawn_enemy(pp.x + math.cos(a) * 600, pp.y + math.sin(a) * 600, id)
                end
                return
            end
        end
    end)
end
