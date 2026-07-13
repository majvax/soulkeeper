-- mods/core/objects.lua — one-time objects (each grants its own component).
---@param mod Mod
---@param C core.Components
return function(mod, C)
    mod:object("onion", "Onion",
        function(e) e:set(C.Aura, { radius = 120, per_second = 25 }) end,
        {
            rarity = "epic",
            value_text = function() return "damage aura" end,
            draw = function(ctx, view)
                local a = view:get(C.Aura)
                if not a or a.radius <= 0 then return end
                ctx:circle_filled(view.x, view.y, a.radius, 120, 180, 255, 30)
                ctx:circle(view.x, view.y, a.radius, 120, 180, 255, 120, 2)
            end,
        })

    -- Legendary: dashing damages and shoves enemies you pass through.
    mod:object("shockwave", "Shockwave Dash",
        function(e)
            local d = e:get(Dash)
            if d then d.shockwave = 25 end
        end,
        {
            rarity = "legendary",
            value_text = function() return "dash damages enemies" end,
        })

    mod:object("frostbelt", "Frost Belt",
        function(e) e:set(C.Slow, { radius = 140, factor = 0.5 }) end,
        {
            rarity = "epic",
            value_text = function() return "slows enemies" end,
            draw = function(ctx, view)
                local s = view:get(C.Slow)
                if not s or s.radius <= 0 then return end
                ctx:circle_filled(view.x, view.y, s.radius, 150, 220, 255, 25)
                ctx:circle(view.x, view.y, s.radius, 180, 235, 255, 140, 2)
            end,
        })

    -- CURSED: free aim comes at a price — the sim aims for you, but every
    -- shot hits softer. A comfort-vs-power decision, not a default pick.
    mod:object("autotarget", "Auto Target",
        function(e)
            e:set(C.AutoTarget, {})
            local w = e:get(C.Weapon)
            if w then w.damage = w.damage * 0.7 end
        end,
        {
            rarity = "legendary",
            value_text = function() return "auto-aim, -30% DMG" end,
        })

    -- +1 card on every future level-up (levelup.lua reads C.Insight).
    mod:object("crystal_ball", "Crystal Ball",
        function(e)
            if e:has(C.Insight) then
                local i = e:get(C.Insight)
                i.extra = math.floor(i.extra + 1)
            else
                e:set(C.Insight, { extra = 1 })
            end
        end,
        {
            rarity = "epic",
            value_text = function() return "+1 level-up choice" end,
        })

    -- Legendary: blades circle you and shred whatever they touch. The server
    -- spins the (networked) phase, so the drawn blades ARE the hitbox.
    mod:object("orbit", "Orbiting Blades",
        function(e) e:set(C.Orbit, {}) end,
        {
            rarity = "legendary",
            value_text = function() return "spinning blades" end,
            draw = function(ctx, view)
                local o = view:get(C.Orbit)
                if not o or o.count <= 0 then return end
                local n = math.floor(o.count)
                for i = 1, n do
                    local a = o.phase + (i / n) * 2 * math.pi
                    local bx = view.x + math.cos(a) * o.radius
                    local by = view.y + math.sin(a) * o.radius
                    ctx:circle_filled(bx, by, 7, 210, 225, 255, 220)
                    ctx:circle(bx, by, 7, 120, 160, 255, 255, 2)
                end
            end,
        })

    -- Epic: enemies that land a contact hit take damage back.
    mod:object("thorns", "Spiked Armor",
        function(e) e:set(C.Thorns, {}) end,
        {
            rarity = "epic",
            value_text = function() return "hits bite back" end,
        })

    -- Legendary one-shot: instead of going down, rise at half hearts.
    mod:object("phoenix", "Phoenix Feather",
        function(e) e:set(C.Phoenix, {}) end,
        {
            rarity = "legendary",
            value_text = function() return "cheat death once" end,
        })

    -- Epic: better level-up rarity rolls from now on (levelup.lua reads it).
    mod:object("clover", "Lucky Clover",
        function(e)
            if e:has(C.Luck) then
                local l = e:get(C.Luck)
                l.bonus = l.bonus + 0.25
            else
                e:set(C.Luck, { bonus = 0.5 })
            end
        end,
        {
            rarity = "epic",
            value_text = function() return "rarer level-up cards" end,
        })

    -- Epic: finishing a dash overcharges the trigger for a few seconds.
    mod:object("adrenaline", "Adrenaline Core",
        function(e) e:set(C.Overcharge, {}) end,
        {
            rarity = "epic",
            value_text = function() return "dash boosts fire rate" end,
        })

    -- Epic: a periodic shock ring from your body — damage keys off your
    -- weapon, so the passive keeps pace with the build (static_sys).
    mod:object("static_charge", "Static Charge",
        function(e) e:set(C.Static, {}) end,
        {
            rarity = "epic",
            value_text = function() return "periodic shock ring" end,
        })

    -- Epic: kills refund dash cooldown — the aggression flywheel (kill to
    -- move, move to kill). Hooked in the scoreboard credit path.
    mod:object("hunter", "Hunter's Instinct",
        function(e) e:set(C.Hunter, {}) end,
        {
            rarity = "epic",
            value_text = function() return "kills refund dash" end,
        })

    -- Legendary: losing a heart bursts a friendly ring from the hit — the
    -- swarm that tagged you pays for it.
    mod:object("reactive", "Reactive Plating",
        function(e) e:set(C.Reactive, {}) end,
        {
            rarity = "legendary",
            value_text = function() return "lost hearts bite back" end,
        })

    ------------------------------------------------------------ bullet barrels
    -- Extra bullets are a chest DECISION now (the Split Shot upgrade is gone).
    mod:object("split_barrel", "Split Barrel",
        function(e)
            local w = e:get(C.Weapon)
            if w then w.projectiles = math.floor(w.projectiles + 2) end
        end,
        {
            rarity = "epic",
            value_text = function() return "+2 bullets" end,
        })
    mod:object("mirror_barrel", "Mirror Barrel",
        function(e)
            local w = e:get(C.Weapon)
            if w then w.mirror = 1 end
        end,
        {
            rarity = "legendary",
            value_text = function() return "volley fires backward too" end,
        })

    ----------------------------------------------------------------- cursed
    -- Power with a visible price — pick them FOR a strategy.
    mod:object("berserker_sigil", "Berserker Sigil",
        function(e)
            local w = e:get(C.Weapon)
            if w then w.damage = w.damage * 1.6 end
            local h = e:get(Hearts)
            if h then
                h.max = math.floor(math.max(1, h.max - 1))
                h.current = math.floor(math.min(h.current, h.max))
            end
        end,
        {
            rarity = "legendary",
            value_text = function() return "+60% DMG, -1 max heart" end,
        })
    mod:object("glass_cannon", "Glass Cannon",
        function(e)
            local w = e:get(C.Weapon)
            if w then w.damage = w.damage * 2 end
            local h = e:get(Hearts)
            if h then
                h.max = 1
                h.current = math.floor(math.min(h.current, 1))
            end
        end,
        {
            rarity = "legendary",
            value_text = function() return "x2 DMG, 1 max heart" end,
        })
    mod:object("heavy_rounds", "Heavy Rounds",
        function(e)
            local w = e:get(C.Weapon)
            if w then
                w.damage = w.damage * 2
                w.cooldown_max = w.cooldown_max * 1.55
                w.bullet_speed = w.bullet_speed * 0.75
            end
        end,
        {
            rarity = "legendary",
            value_text = function() return "x2 DMG, slower gun" end,
        })
    mod:object("lead_plates", "Lead Plates",
        function(e)
            local h = e:get(Hearts)
            if h then h.max = math.floor(h.max + 2) end
            local s = e:get(Speed)
            if s then s.value = s.value * 0.75 end
        end,
        {
            rarity = "epic",
            value_text = function() return "+2 max hearts, -25% SPD" end,
        })

    ---------------------------------------------------------------- identity
    -- One-time playstyle transforms: each BLOCKS an upgrade lane forever
    -- (upgrades.lua `available` checks) and identity_sys enforces the live
    -- rule so later picks can't undo it.
    mod:object("goliath", "Goliath",
        function(e)
            local h = e:get(Hearts)
            if h then h.max = math.floor(h.max + 3) end
            e:set(C.Goliath, {}) -- identity_sys clamps Speed to 200; no more Swift Boots
        end,
        {
            rarity = "epic",
            value_text = function() return "+3 hearts, slow forever" end,
        })
    mod:object("david", "David",
        function(e)
            local w = e:get(C.Weapon)
            if w then w.cooldown_max = math.max(0.08, w.cooldown_max * 0.65) end
            e:set(C.David, {}) -- identity_sys clamps hearts to 3; no more Vitality
        end,
        {
            rarity = "epic",
            value_text = function() return "+54% fire rate, 3 hearts max" end,
        })
    -- SYNERGY: crit chance BECOMES a function of pierce (8% per pierce) —
    -- pierce stacking feeds two axes, Keen Eye leaves the pool. The
    -- pierce-crit build.
    mod:object("executioner", "Executioner's Edge",
        function(e) e:set(C.Executioner, {}) end,
        {
            rarity = "legendary",
            value_text = function() return "crit = 8% per pierce" end,
        })
end
