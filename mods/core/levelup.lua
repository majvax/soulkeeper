-- mods/core/levelup.lua — the level-up OFFER is game policy, not engine code.
--
-- The engine hands us the FACTS (world:offerable = content this player can
-- get right now, with the tiers each piece rolls at) and we decide the cards:
-- how many (3 + the player's Insight, e.g. from the Crystal Ball object),
-- the rarity weights, and the picks. The engine validates, stores the offer
-- (reconnects re-SEND it, never re-roll) and ships it to the client.
---@param mod Mod
---@param C core.Components
return function(mod, C)
    -- Roll weights per tier (Common..Legendary). Mod-owned balance now.
    local WEIGHTS = { 0.45, 0.28, 0.16, 0.08, 0.03 }
    local BASE_CARDS = 3

    -- XP cost of the NEXT level: QUADRATIC. Income grows ~linearly with the
    -- wave (more enemies, richer orbs, elites), so a linear cost curve runs
    -- away (playtest: level 112 by wave 32, a menu every few seconds). This
    -- lands around level 35-45 by wave 30 — roughly a level per wave late.
    mod:xp_curve(function(level)
        return 5 + 3 * level + math.floor(0.8 * level * level)
    end)

    -- Weighted tier roll -> 1..5 (Lua-side index; wire rarity = tier - 1).
    -- Luck (the Lucky Clover) tilts the roll upward: each tier's weight is
    -- multiplied by 1 + luck * (tier - 1), so commons keep their base weight
    -- and legendaries gain the most.
    local function roll_tier(luck)
        local w = {}
        local total = 0
        for tier, base in ipairs(WEIGHTS) do
            w[tier] = base * (1 + luck * (tier - 1))
            total = total + w[tier]
        end
        local x = math.random() * total
        for tier, weight in ipairs(w) do
            x = x - weight
            if x <= 0 then return tier end
        end
        return 1
    end

    -- ctx: "level" (XP threshold) or "chest" (a boss loot chest was opened).
    -- Objects come ONLY from chests now — a level-up rolls upgrades, a chest
    -- rolls objects (upgrades again once this player owns every object), so
    -- object acquisition is a fair team-wide beat instead of a lucky roll.
    mod:level_offer(function(player, _, ctx)
        local pool = world:offerable(player)
        local want = ctx == "chest" and "object" or "upgrade"
        local filtered = {}
        for _, entry in ipairs(pool) do
            if entry.kind == want then filtered[#filtered + 1] = entry end
        end
        if #filtered == 0 and want == "object" then
            -- Chest but everything's owned: fall back to an upgrade pick.
            for _, entry in ipairs(pool) do
                if entry.kind == "upgrade" then filtered[#filtered + 1] = entry end
            end
        end
        pool = filtered

        local cards = BASE_CARDS
        if player:has(C.Insight) then
            cards = cards + math.floor(player:get(C.Insight).extra)
        end
        cards = math.max(1, math.min(cards, 5))
        local luck = player:has(C.Luck) and player:get(C.Luck).bonus or 0

        -- Indices of pool entries offered at `tier`.
        local function candidates_at(tier)
            local out = {}
            for i, entry in ipairs(pool) do
                if entry.tiers[tier] then out[#out + 1] = i end
            end
            return out
        end

        local offer = {}
        if want == "object" and #pool > 0 and pool[1].kind == "object" then
            -- CHEST rounds: every object drops at the SAME rate (playtest: the
            -- rarity roll made epic/legendary objects near-mythical — one seen
            -- per run). Uniform pick; the card frame still shows the object's
            -- home tier so its class reads at a glance.
            for _ = 1, cards do
                if #pool == 0 then break end
                local idx = math.random(#pool)
                local home = 1
                for t, offered in ipairs(pool[idx].tiers) do
                    if offered then home = t break end
                end
                offer[#offer + 1] = { id = pool[idx].id, rarity = home - 1 }
                table.remove(pool, idx) -- no duplicate cards in one offer
            end
            return offer
        end
        for _ = 1, cards do
            if #pool == 0 then break end
            -- Rarity FIRST, then a uniform pick among content offered at that
            -- tier; no candidates -> fall back a tier (L->E->R->U->C), and if
            -- NOTHING exists at-or-below the roll climb upward instead — an
            -- upgrade pool missing low tiers must still fill cards on a common
            -- roll. Keeps legendaries rare in the level-up pools.
            local tier = roll_tier(luck)
            local candidates = {}
            for t = tier, 1, -1 do
                candidates = candidates_at(t)
                if #candidates > 0 then tier = t break end
            end
            if #candidates == 0 then
                for t = tier + 1, #WEIGHTS do
                    candidates = candidates_at(t)
                    if #candidates > 0 then tier = t break end
                end
            end
            if #candidates > 0 then
                local idx = candidates[math.random(#candidates)]
                offer[#offer + 1] = { id = pool[idx].id, rarity = tier - 1 }
                table.remove(pool, idx) -- no duplicate cards in one offer
            end
        end
        return offer
    end)
end
