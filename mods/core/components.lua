-- mods/core/components.lua — Soulkeeper's gameplay components.
--
-- The ENTIRE damage model lives here as Lua components; the C++ kernel only
-- knows Position/Velocity/Health/Hearts/Radius/Dash/Render. mod:component
-- returns a HANDLE: store it, pass it to e:get/set, world:each/nearby,
-- enemy :component(...), view:get(...). Fields declare their defaults.
---@param mod Mod
return function(mod)
    ---@class core.Components
    local C = {}

    -- Player loadout (attached in on_player_spawn; mutated by upgrades).
    -- networked so the client HUD (mod:hud) can read the local player's stats.
    C.Weapon = mod:component("weapon", {
        cooldown_max = 0.35,
        cooldown = 0,
        bullet_speed = 550,
        damage = 10,
        lifetime = 1.2,
        knockback = 8,   -- px shove per hit (copied onto each bullet)
        projectiles = 1, -- bullets per trigger pull (a fan when > 1)
        pierce = 0,      -- extra enemies each bullet punches through
        bounces = 0,     -- ricochets toward the next enemy after a hit
    }, { networked = true })
    C.Crit = mod:component("crit", { chance = 0.05, multiplier = 1.5 }, { networked = true })

    -- A bullet in flight (attached to spawn_bullet entities).
    -- hostile = 1: enemy-fired, hits players instead of enemies.
    C.Bullet = mod:component("bullet", {
        damage = 10, lifetime = 1.2, hostile = 0,
        knockback = 0,
        pierce = 0,  -- hits left to punch through (fly on, don't die)
        bounces = 0, -- ricochets left (re-aim at the next enemy on hit)
        leech = 0,   -- shooter's kill-heal chance, carried by the bullet
        hit_cd = 0,  -- short immunity after a pierce so one enemy isn't hit twice
    })

    -- Contact damage an enemy deals: whole HEARTS per hit.
    C.Touch = mod:component("touch", { hearts = 1 })

    -- Post-hit invulnerability window (players ignore hits while > 0).
    C.IFrames = mod:component("iframes", { remaining = 1.0 })

    -- Drops.
    C.Xp = mod:component("xp", { value = 1 })      -- orb: team XP on pickup
    C.Heal = mod:component("heal", { amount = 1 }) -- heart: +hearts while hurt

    -- Objects.
    C.Aura = mod:component("aura", { radius = 120, per_second = 25 }, { networked = true })
    C.Slow = mod:component("slow", { radius = 140, factor = 0.5 }, { networked = true })

    -- Ranged-attacker state: stands off at `standoff`; when a player is in
    -- `range` it TELEGRAPHS for `windup` s (fx=3, the Prepare pose), then
    -- fires a hostile projectile and cools down for `cooldown` s.
    C.Ranged = mod:component("ranged", {
        range = 340,
        standoff = 260,
        cooldown = 1.6,
        bullet_speed = 260,
        damage = 1,
        timer = 1.0,  -- brief grace period after spawning
        windup = 0.4, -- telegraph length (0 = fire instantly, old behavior)
        winding = 0,  -- telegraph time left (internal)
        anim = 0,     -- attack-clip window left (drives Render.fx)
        volley = 1,   -- bullets per shot (a fan when > 1)
        spread = 0,   -- total fan angle in radians (volley > 1)
        variant = 1,  -- bullet visual: 1 = red, 3 = heavy (Cyclop)
        bullet_radius = 0, -- override the bullet hitbox (0 = default)
    })
    -- Suicide bomber: within `trigger` px of a player it stops, lights the
    -- fuse (fx=3 telegraph) and detonates into a ring of hostile bullets.
    -- Killing it before the fuse runs out cancels the blast entirely.
    C.Bomber = mod:component("bomber", {
        trigger = 70,
        fuse = 0.9,          -- telegraph seconds once triggered
        blast_bullets = 10,
        blast_speed = 250,
        blast_range = 90,    -- bullets live blast_range / blast_speed seconds
        damage = 1,
    })
    C.Fuse = mod:component("fuse", { timer = 0.9 }) -- lit: countdown to boom

    -- Melee lunge: telegraph (fx=3) toward a LOCKED target spot, then a fast
    -- dash along that line (fx=2). Dodgeable — the lock happens at wind-up
    -- start. Runs in the motion phase so the dash overwrites targeting.
    -- `burst` > 0 turns the dash end into a LANDING SLAM: a ring of hostile
    -- bullets (the Frog Prince's leap).
    C.Lunge = mod:component("lunge", {
        range = 190,
        windup = 0.55,
        speed = 430,
        duration = 0.35,
        cooldown = 2.2,
        timer = 1.0,      -- grace after spawn, then per-lunge cooldown
        burst = 0,        -- hostile bullets on dash end (0 = plain lunge)
        burst_speed = 240,
        burst_damage = 1,
        winding = 0,      -- telegraph time left (internal)
        dashing = 0,      -- dash time left (internal)
        dx = 0, dy = 0,   -- locked dash direction (internal)
        saved_speed = 0,  -- Speed.value stashed during windup/dash (internal)
    })

    -- Bomb/bramble planter (boss attack): every `cooldown` s, drop `count`
    -- stationary hazards at random spots — inside its own C.Nova arena rect
    -- when it has one, else scattered around itself. The hazards are real
    -- (shootable) enemies reusing C.Bomber: they zone the ground.
    C.Planter = mod:component("planter", {
        cooldown = 4.0,
        timer = 2.0,
        count = 3,
        kind = 1, -- 1 = core:mine (barrel), 2 = core:bramble (spore pod)
        anim = 0, -- attack-clip window left (drives Render.fx)
    })

    -- Passive self-heal (Vampire Lord): out-DPS it or the fight never ends.
    C.Regen = mod:component("regen", { per_second = 20 })

    -- Elites always pay out a healing heart on death (death system checks it).
    C.EliteDrop = mod:component("elitedrop", {})

    -- Summoner boss: every `cooldown` s it calls `count` minions to its side
    -- (ATK clip via fx=1), and BLINKS away from any player that closes within
    -- `blink_range` — you kill it at range or chase it forever.
    -- `pool` picks the minion preset: 1 = trash, 2 = bats (Vampire Lord),
    -- 3 = ELITES (the Game Master finale).
    C.Summon = mod:component("summon", {
        cooldown = 5.0,
        timer = 2.5,        -- first summon shortly after the entrance
        count = 3,
        pool = 1,
        blink_range = 170,
        blink_dist = 300,
        blink_cooldown = 1.2,
        blink_cd = 0,       -- internal
        anim = 0,           -- attack-clip window left (drives Render.fx)
    })

    -- Pure TAGS: zero-field components used only for membership (has/each).
    C.AutoTarget = mod:component("autotarget", {})
    C.Boss = mod:component("boss", {})
    -- Boss loot chest: walking over it opens the objects-only pick for the
    -- whole team (pickups system -> world:open_chest()).
    C.Chest = mod:component("chesttag", {})

    -- Revive bar on a Downed player (0..1). Networked so every client can
    -- draw the progress arc over the body (core's mod:draw hook).
    C.Revive = mod:component("revive", { progress = 0 }, { networked = true })

    -- Extra level-up cards for this player (levelup.lua reads it; the
    -- Crystal Ball object grants +1).
    C.Insight = mod:component("insight", { extra = 0 })

    -- Per-player pickup tuning (attached at spawn; upgrades grow them).
    -- Networked so the stats HUD (mod:hud) can show them.
    C.Magnet = mod:component("magnet", { radius = 130 }, { networked = true }) -- orb pull range
    C.Greed = mod:component("greed", { mult = 1.0 }, { networked = true })     -- XP multiplier on pickup
    C.Leech = mod:component("leech", { chance = 0 }, { networked = true })     -- kill-heal roll (via bullets)

    -- Orbiting Blades (object): `count` blades circle the player at `radius`,
    -- dealing dps on contact. The server spins `phase`; it's networked so the
    -- client draws the blades exactly where they cut.
    C.Orbit = mod:component("orbit", {
        count = 3,
        radius = 70,
        dps = 70, -- fodder crossing the ring dies mid-crossing
        spin = 2.5, -- rad/s
        phase = 0,
    }, { networked = true })

    -- Spiked Armor (object): enemies that land a contact hit take damage back.
    C.Thorns = mod:component("thorns", { damage = 20 })

    -- Phoenix Feather (object, one-shot): consumed instead of going Downed —
    -- back up at half hearts with a long i-frame window.
    C.Phoenix = mod:component("phoenix", {})

    -- Lucky Clover (object): shifts this player's level-up rarity roll upward
    -- (levelup.lua multiplies tier weights by 1 + bonus * (tier - 1)).
    C.Luck = mod:component("luck", { bonus = 0 })

    -- Adrenaline Core (object): finishing a dash boosts fire rate for a burst.
    C.Overcharge = mod:component("overcharge", {
        mult = 0.5,       -- +50% fire rate while active
        duration = 3.0,
        remaining = 0,    -- internal
        was_bursting = 0, -- internal dash-end edge detector
    })

    -- Boss special: every `cooldown` s, a radial ring of `bullets` hostile
    -- projectiles. RAGES as it drops: below 50% health the rings come faster
    -- and denser plus an aimed volley; below 25% it speeds up (phase tracks).
    C.Nova = mod:component("nova", {
        cooldown = 3.0,
        timer = 2.0,   -- first ring shortly after the arena entrance
        bullets = 16,
        bullet_speed = 210,
        damage = 1,
        spin = 0,      -- radians each ring rotates past the last: > 0 = SPIRAL walls
        angle = 0,     -- accumulated ring rotation (internal)
        phase = 0,     -- 0 calm / 1 enraged / 2 frenzied (internal)
        anim = 0,      -- attack-clip window left (drives Render.fx)
        -- Fixed arena rect: center = the boss's spawn point (set at spawn by
        -- enemies.lua), half extents = the archetype's `arena` opt values.
        cx = 0,
        cy = 0,
        arena_w = 560,
        arena_h = 300,
    })
    return C
end
