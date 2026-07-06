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
    }, { networked = true })
    C.Crit = mod:component("crit", { chance = 0.05, multiplier = 1.5 }, { networked = true })

    -- A bullet in flight (attached to spawn_bullet entities).
    -- hostile = 1: enemy-fired, hits players instead of enemies.
    C.Bullet = mod:component("bullet", { damage = 10, lifetime = 1.2, hostile = 0 })

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

    -- Ranged-attacker state: stands off at `standoff`, fires a hostile
    -- projectile every `cooldown` s at players within `range`.
    C.Ranged = mod:component("ranged", {
        range = 340,
        standoff = 260,
        cooldown = 1.6,
        bullet_speed = 260,
        damage = 1,
        timer = 1.0, -- brief grace period after spawning
        anim = 0,    -- attack-clip window left (drives Render.fx)
    })
    -- Pure TAGS: zero-field components used only for membership (has/each).
    C.AutoTarget = mod:component("autotarget", {})
    C.Boss = mod:component("boss", {})

    -- Boss special: every `cooldown` s, a radial ring of `bullets` hostile
    -- projectiles. Presence also marks the entity for guaranteed death drops.
    C.Nova = mod:component("nova", {
        cooldown = 3.0,
        timer = 2.0, -- first ring shortly after the arena entrance
        bullets = 16,
        bullet_speed = 210,
        damage = 1,
        anim = 0,    -- attack-clip window left (drives Render.fx)
    })
    return C
end
