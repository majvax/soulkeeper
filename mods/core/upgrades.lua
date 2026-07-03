-- mods/core/upgrades.lua — repeatable, rarity-scaled stat upgrades.
--
-- Amounts are per tier { common, uncommon, rare, epic, legendary }; a missing
-- or 0 entry means the upgrade is NOT offered at that tier.
local MIN_COOLDOWN = 0.08 -- fire-rate floor (keeps the game fun)

return function(mod, C)
    mod:upgrade("damage", "Sharp Rounds", { 3, 5, 8, 12, 20 },
        function(e, _, amount)
            local w = e:get(Weapon)
            if w then w.damage = w.damage + amount end
        end,
        { value_format = "+{} DMG", sprite = "assets/icons/dmg.png" })

    mod:upgrade("firerate", "Rapid Fire", { 0.02, 0.04, 0.06, 0.09, 0.14 },
        function(e, _, amount)
            local w = e:get(Weapon)
            if w then w.cooldown_max = math.max(MIN_COOLDOWN, w.cooldown_max - amount) end
        end,
        { value_text = function(amount) return "-" .. math.floor(amount * 1000 + 0.5) .. "ms CD" end })

    mod:upgrade("movespeed", "Swift Boots", { 15, 25, 40, 60, 90 },
        function(e, _, amount)
            local s = e:get(Speed)
            if s then s.value = s.value + amount end
        end,
        { value_format = "+{} SPD" })

    -- Raises the heart LIMIT only — it never heals (find hearts for that).
    mod:upgrade("maxhp", "Vitality", { 0, 0, 1, 1, 2 },
        function(e, _, amount)
            local h = e:get(Hearts)
            if h then h.max = h.max + amount end
        end,
        { value_format = "+{} MAX HEART", sprite = "assets/icons/hearth.png" })

    -- AOE Zone grows the damage aura (only offered once you own one).
    mod:upgrade("aoezone", "AOE Zone", { 15, 25, 40, 60, 90 },
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

    mod:upgrade("bulletspeed", "Bullet Velocity", { 40, 70, 110, 160, 250 },
        function(e, _, amount)
            local w = e:get(Weapon)
            if w then w.bullet_speed = w.bullet_speed + amount end
        end,
        { value_format = "+{} BULLET SPD" })

    mod:upgrade("range", "Long Barrel", { 0.10, 0.18, 0.28, 0.42, 0.65 },
        function(e, _, amount)
            local w = e:get(Weapon)
            if w then w.projectile_lifetime = w.projectile_lifetime + amount end
        end,
        { value_text = function(amount) return "+" .. math.floor(amount * 100 + 0.5) / 100 .. "s RANGE" end })

    mod:upgrade("critchance", "Keen Eye", { 0.02, 0.04, 0.07, 0.11, 0.18 },
        function(e, _, amount)
            local c = e:get(Crit)
            if c then c.chance = c.chance + amount end
        end,
        { value_text = function(amount) return "+" .. math.floor(amount * 100 + 0.5) .. "% CRIT" end })

    mod:upgrade("critdamage", "Lethality", { 0.10, 0.20, 0.35, 0.55, 0.90 },
        function(e, _, amount)
            local c = e:get(Crit)
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
                d.max_charges = d.max_charges + amount
                d.charges = d.charges + amount
            end
        end,
        { value_format = "+{} DASH" })
end
