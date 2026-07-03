-- mods/core/objects.lua — one-time objects (each grants its own component).
return function(mod)
    mod:add_object("core:onion", "Onion",
        function(e) e:set("core:aura", { radius = 120, per_second = 25 }) end,
        {
            rarity = "epic",
            value_text = function() return "damage aura" end,
            draw = function(ctx, view)
                local a = view:get("core:aura")
                if not a or a.radius <= 0 then return end
                ctx:circle_filled(view.x, view.y, a.radius, 120, 180, 255, 30)
                ctx:circle(view.x, view.y, a.radius, 120, 180, 255, 120, 2)
            end,
        })

    -- Legendary: dashing damages and shoves enemies you pass through.
    mod:add_object("core:shockwave", "Shockwave Dash",
        function(e)
            local d = e:get(Dash)
            if d then d.shockwave = 25 end
        end,
        {
            rarity = "legendary",
            value_text = function() return "dash damages enemies" end,
        })

    mod:add_object("core:frostbelt", "Frost Belt",
        function(e) e:set("core:slow", { radius = 140, factor = 0.5 }) end,
        {
            rarity = "epic",
            value_text = function() return "slows enemies" end,
            draw = function(ctx, view)
                local s = view:get("core:slow")
                if not s or s.radius <= 0 then return end
                ctx:circle_filled(view.x, view.y, s.radius, 150, 220, 255, 25)
                ctx:circle(view.x, view.y, s.radius, 180, 235, 255, 140, 2)
            end,
        })
end
