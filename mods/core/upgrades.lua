-- mods/core/upgrades.lua — repeatable, rarity-scaled stat upgrades.
--
-- Amounts are per tier { common, uncommon, rare, epic, legendary }; a missing
-- or 0 entry means the upgrade is NOT offered at that tier.
local MIN_COOLDOWN = 0.08 -- fire-rate floor (keeps the game fun)

return function(mod)
    mod:add_stat_upgrade("core:damage", "Sharp Rounds", { 3, 5, 8, 12, 20 },
        function(e, _, amount)
            local w = e:get(Weapon)
            if w then w.damage = w.damage + amount end
        end,
        { value_format = "+{} DMG", sprite = "assets/icons/dmg.png" })

    mod:add_stat_upgrade("core:firerate", "Rapid Fire", { 0.02, 0.04, 0.06, 0.09, 0.14 },
        function(e, _, amount)
            local w = e:get(Weapon)
            if w then w.cooldown_max = math.max(MIN_COOLDOWN, w.cooldown_max - amount) end
        end,
        { value_text = function(amount) return "-" .. math.floor(amount * 1000 + 0.5) .. "ms CD" end })

    mod:add_stat_upgrade("core:movespeed", "Swift Boots", { 15, 25, 40, 60, 90 },
        function(e, _, amount)
            local s = e:get(Speed)
            if s then s.value = s.value + amount end
        end,
        { value_format = "+{} SPD" })

    mod:add_stat_upgrade("core:maxhp", "Vitality", { 15, 30, 50, 75, 120 },
        function(e, _, amount)
            local h = e:get(Health)
            if h then
                h.max = h.max + amount
                h.current = h.max
            end
        end,
        { value_format = "+{} MAX HP", sprite = "assets/icons/hearth.png" })

    -- AOE Zone grows the damage aura (only offered once you own one).
    mod:add_stat_upgrade("core:aoezone", "AOE Zone", { 15, 25, 40, 60, 90 },
        function(e, _, amount)
            local a = e:get("core:aura")
            if a then
                a.radius = a.radius + amount
                a.per_second = a.per_second + amount * 0.5
            end
        end,
        {
            value_format = "+{} AURA",
            available = function(e) return e:has("core:aura") end,
        })
end
