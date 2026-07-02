-- mods/core/systems.lua — systems that tick the core components each frame.
return function(mod)
    -- Damage aura: hurt enemies inside any player's aura, every tick.
    mod:define_system("core:aura_sys", { phase = "update" }, function(dt)
        for p in world:each("core:aura", Position, Player) do
            local a = p:get("core:aura")
            local pp = p:get(Position)
            for e in world:each(Enemy, Position, Health) do
                local ep = e:get(Position)
                local dx, dy = ep.x - pp.x, ep.y - pp.y
                if dx * dx + dy * dy < a.radius * a.radius then
                    local h = e:get(Health)
                    h.current = h.current - a.per_second * dt
                end
            end
        end
    end)

    -- Slow field: scale the velocity of enemies inside it (runs before Movement).
    mod:define_system("core:slow_sys", { phase = "motion" }, function(dt)
        for p in world:each("core:slow", Position, Player) do
            local s = p:get("core:slow")
            local pp = p:get(Position)
            for e in world:each(Enemy, Position, Velocity) do
                local ep = e:get(Position)
                local dx, dy = ep.x - pp.x, ep.y - pp.y
                if dx * dx + dy * dy < s.radius * s.radius then
                    local v = e:get(Velocity)
                    v.dx = v.dx * s.factor
                    v.dy = v.dy * s.factor
                end
            end
        end
    end)
end
