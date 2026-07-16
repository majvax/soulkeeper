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
        lifetime = 1.5,  -- base reach ~825 px (the range upgrade is gone: this IS the range)
        projectiles = 1, -- bullets per trigger pull (a fan when > 1)
        pierce = 0,      -- extra enemies each bullet punches through
        bounces = 0,     -- ricochets toward the next enemy after a hit
        mirror = 0,      -- 1 = the whole volley also fires BACKWARD (Mirror Barrel)
        bullet_radius = 4, -- bullet hitbox px (Heavy Caliber grows it — forgiveness lane)
        cull = 0,        -- execute threshold: non-boss enemies below cull*max die (Reaper)
        volatile = 0,    -- kill-burst damage riding each bullet (Volatile Rounds)
    }, { networked = true })
    C.Crit = mod:component("crit", { chance = 0.05, multiplier = 1.5 }, { networked = true })

    -- A bullet in flight (attached to spawn_bullet entities).
    -- hostile = 1: enemy-fired, hits players instead of enemies.
    C.Bullet = mod:component("bullet", {
        damage = 10,
        lifetime = 1.2,
        hostile = 0,
        pierce = 0,  -- hits left to punch through (fly on, don't die)
        bounces = 0, -- ricochets left (re-aim at the next enemy on hit)
        leech = 0,   -- shooter's kill-heal chance, carried by the bullet
        hit_cd = 0,  -- short immunity after a pierce so one enemy isn't hit twice
        owner = 0,   -- shooter's entity id (p:id()) for the run-stats scoreboard
        cull = 0,    -- shooter's execute threshold (Reaper), carried by the bullet
        volatile = 0, -- kill-burst damage (Volatile Rounds); burst bullets carry 0 — no chains
    })

    -- Contact damage an enemy deals: whole HEARTS per hit.
    C.Touch = mod:component("touch", { hearts = 1 })

    -- Post-hit invulnerability window (players ignore hits while > 0).
    C.IFrames = mod:component("iframes", { remaining = 1.0 })

    -- Drops.
    C.Xp = mod:component("xp", { value = 1 })      -- orb: team XP on pickup
    -- Heart pickup: +hearts while hurt; fades after ttl (heal_decay) so late
    -- waves can't carpet the floor into an unkillable reserve.
    C.Heal = mod:component("heal", { amount = 1, ttl = 25 })

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
        timer = 1.0,       -- brief grace period after spawning
        windup = 0.4,      -- telegraph length (0 = fire instantly, old behavior)
        winding = 0,       -- telegraph time left (internal)
        anim = 0,          -- attack-clip window left (drives Render.fx)
        volley = 1,        -- bullets per shot (a fan when > 1)
        spread = 0,        -- total fan angle in radians (volley > 1)
        variant = 1,       -- bullet visual: 1 = red, 3 = heavy (Cyclop)
        bullet_radius = 0, -- override the bullet hitbox (0 = default)
        homing = 0,        -- > 0: each shot STEERS (C.Homing turn rate — the Acolyte's orb)
    })
    -- Suicide bomber: within `trigger` px of a player it stops, lights the
    -- fuse (fx=3 telegraph) and detonates into a ring of hostile bullets.
    -- Killing it before the fuse runs out cancels the blast entirely.
    C.Bomber = mod:component("bomber", {
        trigger = 70,
        fuse = 0.9, -- telegraph seconds once triggered
        blast_bullets = 10,
        blast_speed = 250,
        blast_range = 90, -- bullets live blast_range / blast_speed seconds
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
        timer = 1.0, -- grace after spawn, then per-lunge cooldown
        burst = 0,   -- hostile bullets on dash end (0 = plain lunge)
        burst_speed = 240,
        burst_damage = 1,
        winding = 0,     -- telegraph time left (internal)
        dashing = 0,     -- dash time left (internal)
        dx = 0,
        dy = 0,          -- locked dash direction (internal)
        saved_speed = 0, -- Speed.value stashed during windup/dash (internal)
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

    -- Bomb toss (Bomb Lord's second act): every `cooldown` s, lob a PRE-LIT
    -- keg at each live player's position (small scatter — dodge the spot).
    -- The keg is a core:mine with its C.Fuse already burning.
    C.Toss = mod:component("toss", {
        cooldown = 4.5,
        timer = 3.0,
        fuse = 1.3,    -- lit fuse length on the lobbed keg
        scatter = 200, -- landing offset radius around the player
        anim = 0,
    })

    -- Homing blood bolts (Vampire Lord): fires `bolts` bullets that STEER
    -- toward the nearest player (each bullet carries C.Homing).
    C.BoltCaster = mod:component("boltcaster", {
        cooldown = 2.2,
        timer = 2.0,
        bolts = 3,
        bullet_speed = 200,
        damage = 1,
        lifetime = 3.5,
        turn_rate = 2.2, -- rad/s copied onto each bolt
        anim = 0,
    })
    -- On a hostile bullet: the homing system bends its velocity toward the
    -- nearest live player, capped at `turn` rad/s. Outrunnable, not ignorable.
    C.Homing = mod:component("homing", { turn = 2.2 })

    -- Bullet sprinkler (Elder Ent): continuously rotating STREAMS — every
    -- emission tick, one bullet per arm at the current angle; the arms sweep.
    -- Rings pulse (Frog King); streams sweep (Elder Ent).
    C.Sprinkler = mod:component("sprinkler", {
        arms = 3,
        angular_vel = 0.9, -- rad/s arm sweep
        angle = 0,         -- internal
        bullet_speed = 190,
        damage = 1,
        lifetime = 2.4,
    })

    -- Blooming seeds (Elder Ent's second act): lob a slow fat seed at a
    -- player; mid-flight it POPS into a ring of `petals` slow bullets.
    C.SeedLauncher = mod:component("seedlauncher", {
        cooldown = 3.2,
        timer = 2.5,
        volley = 1, -- seeds per cast, fanned (the Ent's brain raises it in phase 2)
        bullet_speed = 170,
        bloom_after = 1.1,
        petals = 6,
        petal_speed = 150,
        damage = 1,
        anim = 0,
    })
    C.Seed = mod:component("seed", { bloom = 1.1, petals = 6, petal_speed = 150, damage = 1 })

    -- Rotating cross barrage (Game Master): `lanes` tight bullet lances at
    -- staggered speeds; the whole cross rotates `rotate` radians per volley
    -- (+ then x then +...). Lanes, not rings — his geometry.
    C.Barrage = mod:component("barrage", {
        cooldown = 2.4,
        timer = 2.0,
        lanes = 4,
        per_lane = 3,
        rotate = 0.7853981, -- pi/4
        angle = 0,          -- internal
        bullet_speed = 260,
        damage = 1,
        anim = 0,
    })

    -- Elites always pay out a healing heart on death (death system checks it).
    C.EliteDrop = mod:component("elitedrop", {})

    -- Flat reduction against every BULLET hit (floor 1 — never immune). The
    -- Shieldbearer: a walking DPS check that pierce/crit builds shred and
    -- pea-shooter builds bounce off. Contact tech (orbit/aura/dash) ignores it.
    C.Armor = mod:component("armor", { flat = 5 })

    -- Ambusher (the Mimic): spawns INERT (Speed 0 — reads as a prop) and wakes
    -- ONE-WAY when a player closes within `trigger` px: Speed jumps to
    -- `wake_speed` and the ATK clip flashes the reveal.
    C.Ambush = mod:component("ambush", {
        trigger = 150,
        wake_speed = 250,
        awake = 0, -- internal one-way latch
        anim = 0,  -- reveal-flash window left (drives Render.fx)
    })

    -- Fool's gold (the Mimic King): a FAKE XP orb (Render.kind = orb, but no
    -- C.Xp — magnets ignore it, THE tell) that pops into a hostile burst when
    -- a player reaches for it or the fuse runs out.
    C.FoolsGold = mod:component("foolsgold", {
        fuse = 6.0,
        trigger = 60, -- just past the 45 px pickup reach: it pops in your face
        bullets = 8,
        bullet_speed = 240,
        damage = 1,
    })

    -- Summoner boss: every `cooldown` s it calls `count` minions to its side
    -- (ATK clip via fx=1), and BLINKS away from any player that closes within
    -- `blink_range` — you kill it at range or chase it forever.
    -- `pool` picks the minion preset: 1 = trash, 2 = bats (Vampire Lord),
    -- 3 = ELITES (the Game Master finale).
    C.Summon = mod:component("summon", {
        cooldown = 5.0,
        timer = 2.5, -- first summon shortly after the entrance
        count = 3,
        pool = 1,
        blink_range = 170,
        blink_dist = 300,
        blink_cooldown = 1.2,
        blink_cd = 0, -- internal
        anim = 0,     -- attack-clip window left (drives Render.fx)
    })

    -- Boss BRAIN: the fight director (brains.lua). `id` picks the move table;
    -- the boss's mechanic components are PARKED (cooldown/timer 9999) and the
    -- brain fires them — weighted-random moves, never the same twice in a row,
    -- jittered cooldowns, health-gated phases. Mechanics keep owning the HOW.
    C.Brain = mod:component("brain", {
        id = 0,
        timer = 2.0,       -- until the next move pick
        winding = 0,       -- telegraph remaining (fx = 3, Speed parked)
        channel = 0,       -- channeled-move remaining (`during` runs each tick)
        move = 0,          -- move index being wound/channeled (also = last used)
        phase = 1,         -- health-gated escalation step (one-way)
        saved_speed = -1,  -- Speed parked during telegraph/channel (-1 = none)
        anim = 0,          -- fx window left for inline strikes (delegates self-manage)
        used_mask = 0,     -- bitmask of executed move indices (variety telemetry/tests)
        c1 = 0,
        c2 = 0, -- per-move scratch registers (hop velocity, spiral angle/accumulator)
    })

    -- Pure TAGS: zero-field components used only for membership (has/each).
    C.AutoTarget = mod:component("autotarget", {})
    C.Boss = mod:component("boss", {})
    -- Boss loot chest: walking over it opens the objects-only pick for the
    -- whole team (pickups system -> world:open_chest()).
    C.Chest = mod:component("chesttag", {})

    -- IDENTITY tags (one-time playstyle transforms; each blocks an upgrade
    -- from ever being offered again — see upgrades.lua `available` checks —
    -- and identity_sys keeps their live rules enforced).
    C.Goliath = mod:component("goliath", {})         -- tank: hearts up, speed clamped
    C.David = mod:component("david", {})             -- glassy speedster: fire rate up, hearts clamped
    C.Executioner = mod:component("executioner", {}) -- crit chance = f(pierce)

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
    -- client draws the blades exactly where they cut. Contact damage scales
    -- with Weapon.damage (orbit_sys) so the blades stay lethal late-game;
    -- Blade Dance / Extra Blade upgrades grow spin/count up to hard caps.
    C.Orbit = mod:component("orbit", {
        count = 3,
        radius = 95, -- arm's length: blades cut AHEAD of the body, not on it
        dps = 70,    -- base; + 0.7 x Weapon.damage per second at the cut
        spin = 2.5,  -- rad/s (capped at 6 — per-tick travel must stay under the hit radius)
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

    -- Static Charge (object): a periodic friendly shock ring from the player,
    -- damage keyed to Weapon.damage (static_sys) — a passive that keeps pace.
    C.Static = mod:component("static", {
        cooldown = 3.0,
        timer = 3.0,
        bullets = 8,
        bullet_speed = 300,
        lifetime = 0.5, -- ~150 px reach: a personal-space zap, not artillery
    })

    -- Hunter's Instinct (object): every kill you land shaves `refund` seconds
    -- off your dash cooldown (hooked in the scoreboard credit path).
    C.Hunter = mod:component("hunter", { refund = 0.5 })

    -- Reactive Plating (object): LOSING a heart bursts a friendly ring —
    -- hooked in hurt_player, the single site where players take damage.
    C.Reactive = mod:component("reactive", {
        bullets = 10,
        bullet_speed = 320,
        damage = 25,
        lifetime = 0.45,
    })

    -- Adrenaline Core (object): finishing a dash boosts fire rate for a burst.
    C.Overcharge = mod:component("overcharge", {
        mult = 0.5,       -- +50% fire rate while active
        duration = 3.0,
        remaining = 0,    -- internal
        was_bursting = 0, -- internal dash-end edge detector
    })

    -- The Frog King's SIGNATURE (his alone): every `cooldown` s, a radial
    -- ring of `bullets` hostile projectiles. RAGES as it drops: below 50%
    -- health the rings come faster and denser; below 25% it speeds up.
    C.Nova = mod:component("nova", {
        cooldown = 3.0,
        timer = 2.0, -- first ring shortly after the arena entrance
        bullets = 16,
        bullet_speed = 210,
        damage = 1,
        spin = 0,  -- radians each ring rotates past the last: > 0 = SPIRAL walls
        angle = 0, -- accumulated ring rotation (internal)
        phase = 0, -- 0 calm / 1 enraged / 2 frenzied (internal)
        anim = 0,  -- attack-clip window left (drives Render.fx)
    })

    -- Arena confinement (sim side; the CLIENT keys its wall/prediction off
    -- the archetype's `arena` opt): players and the bearer stay inside the
    -- fixed rect. Center = spawn point (set by the milestone hook). The
    -- bearer clamps to a 90 px INSET rect, and `pinned` accumulates while the
    -- clamp is actively holding it — pinned too long (wall-hugging player)
    -- means it TELEPORTS to a random arena point instead of grinding.
    C.Arena = mod:component("arena", {
        cx = 0,
        cy = 0,
        w = 960, -- half extents (1920x1080 full)
        h = 540,
        pinned = 0, -- internal: seconds the clamp has been holding the bearer
    })

    -- Boss-SPAWNED minions/hazards (summons, tossed kegs, planted brambles):
    -- they die without paying XP or heart rolls — boss adds are pressure, not
    -- a loot fountain that trivializes the fight.
    C.NoLoot = mod:component("noloot", {})

    -- Supply crate (POI): the death system pays its OWN loot table (orb burst
    -- or a heart) instead of the standard drop — a map feature worth the walk.
    C.CrateLoot = mod:component("crateloot", {})
    return C
end
