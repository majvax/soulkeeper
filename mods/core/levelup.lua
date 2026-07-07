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

    -- Weighted tier roll -> 1..5 (Lua-side index; wire rarity = tier - 1).
    local function roll_tier()
        local total = 0
        for _, w in ipairs(WEIGHTS) do total = total + w end
        local x = math.random() * total
        for tier, w in ipairs(WEIGHTS) do
            x = x - w
            if x <= 0 then return tier end
        end
        return 1
    end

    mod:level_offer(function(player, _)
        local pool = world:offerable(player)
        local cards = BASE_CARDS
        if player:has(C.Insight) then
            cards = cards + math.floor(player:get(C.Insight).extra)
        end
        cards = math.max(1, math.min(cards, 5))

        local offer = {}
        for _ = 1, cards do
            if #pool == 0 then break end
            -- Rarity FIRST, then a uniform pick among content offered at that
            -- tier; no candidates -> fall back a tier (L->E->R->U->C). Keeps
            -- legendaries rare — objects can't force gold cards.
            local tier = roll_tier()
            while tier >= 1 do
                local candidates = {}
                for i, entry in ipairs(pool) do
                    if entry.tiers[tier] then candidates[#candidates + 1] = i end
                end
                if #candidates > 0 then
                    local idx = candidates[math.random(#candidates)]
                    offer[#offer + 1] = { id = pool[idx].id, rarity = tier - 1 }
                    table.remove(pool, idx) -- no duplicate cards in one offer
                    break
                end
                tier = tier - 1
            end
        end
        return offer
    end)
end
