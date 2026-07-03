-- mods/core/systems.lua — systems that tick the core components each frame.
return function(mod, C)
    -- Damage aura: hurt enemies inside any player's aura, every tick.
    mod:system("aura_sys", { phase = "update" }, function(dt)
        for p in world:each(C.Aura, Position, Player) do
            local a = p:get(C.Aura)
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

    -- Nearest player to (x, y), or nil. Shared by the two ranged systems.
    local function nearest_player(x, y)
        local best, best_d2 = nil, math.huge
        for p in world:each(Player, Position) do
            local pp = p:get(Position)
            local dx, dy = pp.x - x, pp.y - y
            local d2 = dx * dx + dy * dy
            if d2 < best_d2 then best, best_d2 = p, d2 end
        end
        return best, best_d2
    end

    -- Ranged enemies hold position once close enough (runs before Movement, so
    -- it overrides the chase velocity from targeting).
    mod:system("ranged_standoff", { phase = "motion" }, function(dt)
        for e in world:each(C.Ranged, Position, Velocity, Enemy) do
            local r = e:get(C.Ranged)
            local ep = e:get(Position)
            local _, d2 = nearest_player(ep.x, ep.y)
            if d2 < r.standoff * r.standoff then
                local v = e:get(Velocity)
                v.dx, v.dy = 0, 0
            end
        end
    end)

    -- Ranged enemies fire a hostile projectile at the nearest player in range.
    mod:system("ranged_fire", { phase = "update" }, function(dt)
        for e in world:each(C.Ranged, Position, Enemy) do
            local r = e:get(C.Ranged)
            r.timer = r.timer - dt
            if r.timer <= 0 then
                local ep = e:get(Position)
                local target, d2 = nearest_player(ep.x, ep.y)
                if target and d2 < r.range * r.range then
                    local tp = target:get(Position)
                    local dx, dy = tp.x - ep.x, tp.y - ep.y
                    local len = math.sqrt(dx * dx + dy * dy)
                    if len > 0 then
                        spawn_projectile(ep.x, ep.y,
                            dx / len * r.bullet_speed, dy / len * r.bullet_speed,
                            r.damage, r.range / r.bullet_speed + 0.5, true) -- hostile
                        r.timer = r.cooldown
                    end
                end
            end
        end
    end)

    -- Slow field: scale the velocity of enemies inside it (runs before Movement).
    mod:system("slow_sys", { phase = "motion" }, function(dt)
        for p in world:each(C.Slow, Position, Player) do
            local s = p:get(C.Slow)
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
