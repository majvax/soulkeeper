-- mods/core/upgrades.lua — repeatable, rarity-scaled stat upgrades.
--
-- Amounts are per tier { common, uncommon, rare, epic, legendary }; a missing
-- or 0 entry means the upgrade is NOT offered at that tier.
local MIN_COOLDOWN = 0.08 -- fire-rate floor (keeps the game fun)


---@param mod Mod
---@param C core.Components
return function(mod, C)
    mod:upgrade("damage", "Sharp Rounds", { 3, 5, 8, 12, 20 },
        function(e, _, amount)
            local w = e:get(C.Weapon)
            if w then w.damage = w.damage + amount end
        end,
        { value_format = "+{} DMG", sprite = "assets/icons/dmg.png" })

    mod:upgrade("firerate", "Rapid Fire", { 0.02, 0.04, 0.06, 0.09, 0.14 },
        function(e, _, amount)
            local w = e:get(C.Weapon)
            if w then w.cooldown_max = math.max(MIN_COOLDOWN, w.cooldown_max - amount) end
        end,
        {
            value_text = function(amount) return "-" .. math.floor(amount * 1000 + 0.5) .. "ms CD" end,
            available = function(e)
                return e:get(C.Weapon).cooldown_max > MIN_COOLDOWN
            end

        })

    mod:upgrade("movespeed", "Swift Boots", { 15, 25, 40, 60, 90 },
        function(e, _, amount)
            local s = e:get(Speed)
            if s then s.value = s.value + amount end
        end,
        {
            value_format = "+{} SPD",
            available = function(e) return not e:has(C.Goliath) end, -- Goliath walks forever
        })

    -- Raises the heart LIMIT only — it never heals (find hearts for that).
    -- Vitality shows on the body: +6% size per heart gained (kernel Scale,
    -- capped so late-game stacks stay reasonable). Hitbox (Radius) unchanged.
    mod:upgrade("maxhp", "Vitality", { 0, 0, 1, 1, 2 },
        function(e, _, amount)
            local h = e:get(Hearts)
            if h then h.max = math.floor(h.max + amount) end -- kernel int field
            local s = e:get(Scale)
            if s then s.value = s.value + 0.20 * amount end
        end,
        {
            value_format = "+{} MAX HEART",
            sprite = "assets/icons/hearth.png",
            available = function(e) return not e:has(C.David) end, -- David stays glassy
        })

    -- AOE Zone grows the damage aura (only offered once you own one).
    mod:upgrade("aoezone", "AOE Zone", { 5, 15, 25, 40, 60 },
        function(e, _, amount)
            local a = e:get(C.Aura)
            if a then
                a.radius = a.radius + amount
                a.per_second = a.per_second + amount * 0.5
            end
        end,
        {
            value_format = "+{} AURA",
            available = function(e) return e:has(C.Aura) end,
        })

    -- Orbit lanes (only offered once you own the Blades — like AOE Zone).
    -- Both are HARD-capped: spin at 6 rad/s (past that a blade's per-tick
    -- travel outruns its 26 px hit radius and enemies slip between cuts),
    -- count at 8 (a solid ring stops being a pattern).
    mod:upgrade("bladespin", "Blade Dance", { 0.4, 0.6, 0.9, 1.3, 1.8 },
        function(e, _, amount)
            local o = e:get(C.Orbit)
            if o then o.spin = math.min(6.0, o.spin + amount) end
        end,
        {
            value_text = function(amount) return "+" .. amount .. " BLADE SPEED" end,
            available = function(e)
                return e:has(C.Orbit) and e:get(C.Orbit).spin < 6.0
            end,
        })
    mod:upgrade("bladecount", "Extra Blade", { 0, 0, 1, 1, 2 },
        function(e, _, amount)
            local o = e:get(C.Orbit)
            if o then o.count = math.min(8, math.floor(o.count + amount)) end
        end,
        {
            value_format = "+{} BLADE",
            available = function(e)
                return e:has(C.Orbit) and e:get(C.Orbit).count < 8
            end,
        })

    mod:upgrade("bulletspeed", "Bullet Velocity", { 40, 70, 110, 160, 250 },
        function(e, _, amount)
            local w = e:get(C.Weapon)
            if w then w.bullet_speed = w.bullet_speed + amount end
        end,
        { value_format = "+{} BULLET SPD" })

    -- (Long Barrel / range is gone: base Weapon.lifetime carries the reach now.)

    -- Heavy Caliber: fatter bullets (a bigger hitbox) — the forgiveness lane.
    mod:upgrade("bulletsize", "Heavy Caliber", { 1, 2, 3, 4, 6 },
        function(e, _, amount)
            local w = e:get(C.Weapon)
            if w then w.bullet_radius = w.bullet_radius + amount end
        end,
        { value_format = "+{} BULLET SIZE" })

    -- Reaper: hits that leave non-boss trash under the threshold finish it.
    -- Rare+ — an anti-tank lane against wave-compounded health pools.
    mod:upgrade("cull", "Reaper", { 0, 0, 0.03, 0.05, 0.08 },
        function(e, _, amount)
            local w = e:get(C.Weapon)
            if w then w.cull = w.cull + amount end
        end,
        { value_text = function(amount)
            return "+" .. math.floor(amount * 100 + 0.5) .. "% EXECUTE"
        end })

    -- Volatile Rounds: killing bullets detonate a small friendly burst at the
    -- victim. Epic+ — the crowd-clear lane.
    mod:upgrade("volatile", "Volatile Rounds", { 0, 0, 0, 25, 45 },
        function(e, _, amount)
            local w = e:get(C.Weapon)
            if w then w.volatile = w.volatile + amount end
        end,
        { value_format = "+{} BLAST DMG" })

    mod:upgrade("critchance", "Keen Eye", { 0.02, 0.04, 0.07, 0.11, 0.18 },
        function(e, _, amount)
            local c = e:get(C.Crit)
            if c then c.chance = c.chance + amount end
        end,
        {
            value_text = function(amount) return "+" .. math.floor(amount * 100 + 0.5) .. "% CRIT" end,
            -- Executioner's Edge OWNS crit chance (it tracks pierce instead).
            available = function(e)
                return e:get(C.Crit).chance < 1.0 and not e:has(C.Executioner)
            end
        })

    mod:upgrade("critdamage", "Lethality", { 0.10, 0.20, 0.35, 0.55, 0.90 },
        function(e, _, amount)
            local c = e:get(C.Crit)
            if c then c.multiplier = c.multiplier + amount end
        end,
        { value_text = function(amount) return "+" .. math.floor(amount * 100 + 0.5) .. "% CRIT DMG" end })

    -- Dash cooldown: only exists at Common/Uncommon/Rare (grey/green/blue).
    mod:upgrade("dashcd", "Sprint", { 0.05, 0.10, 0.15 },
        function(e, _, amount)
            local d = e:get(Dash)
            if d then d.cooldown_max = d.cooldown_max * (1 - amount) end
        end,
        { value_text = function(amount) return "-" .. math.floor(amount * 100 + 0.5) .. "% DASH CD" end })

    -- Extra dash charge: Epic only.
    mod:upgrade("dashcharge", "Extra Dash", { 0, 0, 0, 1 },
        function(e, _, amount)
            local d = e:get(Dash)
            if d then
                d.max_charges = math.floor(d.max_charges + amount) -- kernel int fields
                d.charges = math.floor(d.charges + amount)
            end
        end,
        { value_format = "+{} DASH" })

    -- (Extra bullets moved to OBJECTS — Split Barrel / Mirror Barrel: a chest
    -- decision, not a repeatable stat lane.)

    -- Piercing Rounds: bullets punch through extra enemies. Rare+.
    mod:upgrade("pierce", "Piercing Rounds", { 0, 0, 1, 1, 2 },
        function(e, _, amount)
            local w = e:get(C.Weapon)
            if w then w.pierce = math.floor(w.pierce + amount) end
        end,
        { value_format = "+{} PIERCE" })

    -- Ricochet: bullets re-aim at the next enemy after a hit. Epic+.
    mod:upgrade("ricochet", "Ricochet", { 0, 0, 0, 1, 2 },
        function(e, _, amount)
            local w = e:get(C.Weapon)
            if w then w.bounces = math.floor(w.bounces + amount) end
        end,
        { value_format = "+{} BOUNCE" })

    -- Magnet: drops fly to you from farther away.
    mod:upgrade("magnet", "Magnet", { 20, 35, 55, 80, 120 },
        function(e, _, amount)
            if not e:has(C.Magnet) then e:set(C.Magnet, {}) end
            local m = e:get(C.Magnet)
            m.radius = m.radius + amount
        end,
        { value_format = "+{} PULL" })

    -- Leech: kills roll a chance to restore a heart. Rare+ (hearts are gold).
    mod:upgrade("leech", "Leech", { 0, 0, 0.02, 0.04, 0.07 },
        function(e, _, amount)
            if not e:has(C.Leech) then e:set(C.Leech, {}) end
            local l = e:get(C.Leech)
            l.chance = l.chance + amount
        end,
        {
            value_text = function(amount)
                return "+" .. math.floor(amount * 100 + 0.5) .. "% KILL HEAL"
            end
        })

    -- Greed: XP orbs are worth more to YOU (per-player, it's your pickup).
    -- Stops at +150% — it's an economy valve, not a build axis (unbounded XP
    -- income defeats the quadratic level curve).
    mod:upgrade("greed", "Greed", { 0.04, 0.08, 0.12, 0.18, 0.30 },
        function(e, _, amount)
            if not e:has(C.Greed) then e:set(C.Greed, {}) end
            local g = e:get(C.Greed)
            g.mult = math.min(2.5, g.mult + amount)
        end,
        {
            value_text = function(amount)
                return "+" .. math.floor(amount * 100 + 0.5) .. "% XP"
            end,
            available = function(e)
                return not e:has(C.Greed) or e:get(C.Greed).mult < 2.5
            end,
        })
end
