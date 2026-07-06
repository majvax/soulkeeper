-- mods/core/systems.lua — THE game rules. The entire gameplay pipeline is Lua:
--   targeting -> (kernel dash/motion) -> shooting -> (kernel movement)
--   -> projectile -> combat -> update -> pickup -> death
-- The kernel provides motion, the spatial hash (world:nearby), snapshots.

local function auto_aim(player)
    local pp = player:get(Position)
    local aim = player:get(AimState)
    local e, _, ex, ey = world:closest(pp.x, pp.y, Enemy)
    if not e then
        aim.firing = 0
        return
    end
    local dx, dy = ex - pp.x, ey - pp.y
    local len = math.sqrt(dx * dx + dy * dy)
    if len <= 0 then return end
    aim.dx, aim.dy = dx / len, dy / len
    aim.firing = 1
end

---@param mod Mod
---@param C core.Components
return function(mod, C)
    -- Nearest live player to (x, y), or nil (+ squared distance). Downed
    -- players are ignored. world:closest is a single engine call — never loop
    -- candidates from Lua inside a per-entity system (that pattern dominated
    -- tick time before).
    local LIVE = { without = Downed } -- hoisted: this runs per enemy per tick
    local function nearest_player(x, y)
        local p, d2, px, py = world:closest(x, y, Player, Position, LIVE)
        return p, d2 or math.huge, px, py -- keep the "no target -> infinite distance" contract
    end

    -- Deal hearts to a player, honoring i-frames. Returns true if it landed.
    -- (Hearts fields are kernel INTEGERS — floor the Lua float arithmetic.)
    local function hurt_player(p, hearts)
        if p:has(Downed) or p:has(C.IFrames) then return false end
        local h = p:get(Hearts)
        if not h then return false end
        h.current = math.floor(h.current - hearts)
        p:set(C.IFrames, {}) -- 1 s of invulnerability (component default)
        return true
    end

    ----------------------------------------------------------------- targeting
    -- Steer every enemy toward the nearest live player at its Speed; ranged
    -- enemies hold position inside their standoff ring (folded in here so the
    -- heavy tick walks the enemy set ONCE). 30 Hz: steering staleness is
    -- ≤ 33 ms (~6 px at max speed) — invisible.
    -- NOTE: slow_sys also runs at 30 Hz stagger 0 ON PURPOSE — it scales the
    -- velocity targeting just wrote, so the cadences must match exactly.
    mod:system("targeting", { phase = "targeting", rate = 30 }, function(dt)
        -- Hoist the (≤4) live players ONCE into plain arrays; the per-enemy
        -- inner loop is then pure Lua arithmetic — zero engine calls. At 500
        -- enemies even one engine call per enemy here busts the tick budget.
        local px, py, pn = {}, {}, 0
        for p in world:each(Player, Position) do
            if not p:has(Downed) then
                local pp = p:get(Position)
                pn = pn + 1
                px[pn], py[pn] = pp.x, pp.y
            end
        end
        if pn == 0 then return end

        for e in world:each(Enemy, Position, Velocity, Speed) do
            local ep = e:get(Position)
            local ex, ey = ep.x, ep.y
            local best_d2, bx, by = math.huge, 0, 0
            for i = 1, pn do
                local dx, dy = px[i] - ex, py[i] - ey
                local d2 = dx * dx + dy * dy
                if d2 < best_d2 then best_d2, bx, by = d2, px[i], py[i] end
            end

            local v = e:get(Velocity)
            local hold = false
            if e:has(C.Ranged) then
                local standoff = e:get(C.Ranged).standoff
                hold = best_d2 < standoff * standoff
            end
            if hold then
                v.dx, v.dy = 0, 0
            elseif best_d2 > 0 then
                local len = math.sqrt(best_d2)
                local s = e:get(Speed)
                v.dx, v.dy = (bx - ex) / len * s.value, (by - ey) / len * s.value
            end
        end
    end)

    ------------------------------------------------------------------ shooting
    -- Players fire toward their aim every Weapon.cooldown_max seconds. Each
    -- shot rolls Crit: a crit multiplies damage and renders bigger/orange.
    mod:system("shooting", { phase = "shooting" }, function(dt)
        for p in world:each(Player, Position, C.Weapon, AimState) do
            if p:has(C.AutoTarget) then auto_aim(p) end
            local w = p:get(C.Weapon)
            if w.cooldown > 0 then w.cooldown = w.cooldown - dt end
            local aim = p:get(AimState)
            if aim.firing == 1 and w.cooldown <= 0 and (aim.dx ~= 0 or aim.dy ~= 0) then
                local pp = p:get(Position)
                local damage = w.damage
                local crit = p:get(C.Crit)
                local is_crit = crit and math.random() < crit.chance
                if is_crit then damage = damage * crit.multiplier end

                local b = spawn_bullet(pp.x, pp.y, aim.dx * w.bullet_speed, aim.dy * w.bullet_speed)
                b:set(C.Bullet, { damage = damage, lifetime = w.lifetime })
                if is_crit then b:get(Render).variant = 2 end -- orange
                w.cooldown = w.cooldown_max
            end
        end
    end)



    ---------------------------------------------------------------- projectile
    -- Bullet flight: expire by lifetime; friendly bullets hit the first enemy
    -- they overlap, hostile ones the first vulnerable player. One hit each.
    -- 60 Hz: a bullet moves ≤ ~9 px between checks, well under the summed hit
    -- radii; lifetimes use the accumulated dt. Staggered opposite `contact` so
    -- the two 60 Hz systems land on alternating ticks.
    mod:system("bullets", { phase = "projectile", rate = 60 }, function(dt)
        for b in world:each(C.Bullet, Position, Radius) do
            local bullet = b:get(C.Bullet)
            bullet.lifetime = bullet.lifetime - dt
            if bullet.lifetime <= 0 then
                b:destroy()
            else
                local bp = b:get(Position)
                local br = b:get(Radius).value
                if bullet.hostile == 1 then
                    for p in world:each(Player, Position, Hearts, Radius) do
                        local pp = p:get(Position)
                        local dx, dy = pp.x - bp.x, pp.y - bp.y
                        local hit = br + p:get(Radius).value
                        if dx * dx + dy * dy < hit * hit and hurt_player(p, bullet.damage) then
                            b:destroy()
                            break
                        end
                    end
                else
                    for e in world:nearby(bp.x, bp.y, br + 40, Enemy, Health, Position, Radius) do
                        local ep = e:get(Position)
                        local dx, dy = ep.x - bp.x, ep.y - bp.y
                        local hit = br + e:get(Radius).value
                        if dx * dx + dy * dy < hit * hit then
                            local h = e:get(Health)
                            h.current = h.current - bullet.damage
                            b:destroy()
                            break
                        end
                    end
                end
            end
        end
    end)

    -------------------------------------------------------------------- combat
    -- Tick down i-frames, then resolve enemy contact: a touching enemy costs
    -- Touch.hearts and grants the i-frame window (so contact can't melt you).
    mod:system("iframes", { phase = "combat" }, function(dt)
        for p in world:each(C.IFrames) do
            local inv = p:get(C.IFrames)
            inv.remaining = inv.remaining - dt
            if inv.remaining <= 0 then p:remove(C.IFrames) end
        end
    end)

    -- 60 Hz: contact windows are tiny next to the 1 s i-frames.
    mod:system("contact", { phase = "combat", rate = 60, stagger = 0.5 }, function(dt)
        for p in world:each(Player, Position, Hearts, Radius) do
            if not p:has(Downed) and not p:has(C.IFrames) then
                local pp = p:get(Position)
                local pr = p:get(Radius).value
                for e in world:nearby(pp.x, pp.y, pr + 40, Enemy, C.Touch, Position, Radius) do
                    local ep = e:get(Position)
                    local dx, dy = ep.x - pp.x, ep.y - pp.y
                    local reach = pr + e:get(Radius).value
                    if dx * dx + dy * dy < reach * reach then
                        hurt_player(p, e:get(C.Touch).hearts)
                        break -- one hit per window; i-frames cover the rest
                    end
                end
            end
        end
    end)

    -- Shockwave Dash (legendary object): while bursting, damage + shove
    -- enemies the player passes through. The shove exits the overlap, so a
    -- burst hits each enemy once.
    mod:system("shockwave", { phase = "combat" }, function(dt)
        for p in world:each(Player, Dash, Position) do
            local d = p:get(Dash)
            if d.burst_remaining > 0 and d.shockwave > 0 then
                local pp = p:get(Position)
                for e in world:nearby(pp.x, pp.y, 40, Enemy, Health, Position) do
                    local ep = e:get(Position)
                    local h = e:get(Health)
                    h.current = h.current - d.shockwave
                    local dx, dy = ep.x - pp.x, ep.y - pp.y
                    local len = math.sqrt(dx * dx + dy * dy)
                    if len < 1 then dx, dy, len = 1, 0, 1 end
                    ep.x = ep.x + dx / len * 60
                    ep.y = ep.y + dy / len * 60
                end
            end
        end
    end)

    -------------------------------------------------------------------- pickup
    -- Collect XP orbs into the team pool; hearts only while hurt.
    -- 20 Hz: the 45 px pickup radius dwarfs per-50 ms player movement.
    mod:system("pickups", { phase = "pickup", rate = 20, stagger = 0.33 }, function(dt)
        for p in world:each(Player, Position) do
            if p:has(Downed) then return end
            local pp = p:get(Position)
            for orb in world:nearby(pp.x, pp.y, 45, C.Xp) do
                world:add_xp(math.tointeger(orb:get(C.Xp).value) or 0) -- crashes without tointeger
                orb:destroy()
            end
            local h = p:get(Hearts)
            if h and h.current < h.max then
                for heart in world:nearby(pp.x, pp.y, 45, C.Heal) do
                    h.current = math.floor(math.min(h.max, h.current + heart:get(C.Heal).amount))
                    heart:destroy()
                    if h.current >= h.max then break end
                end
            end
        end
    end)

    --------------------------------------------------------------------- death
    -- Enemies at 0 HP drop an XP orb (and rarely a healing heart) and die.
    -- Players at 0 hearts go Downed and respawn two waves later, fully healed.
    -- 30 Hz: a 33 ms corpse/respawn latency is invisible. Staggered half an
    -- interval off targeting so the two enemy-wide walks hit different ticks.
    -- This system also owns the RUN-END rules: every player downed at once =
    -- defeat; KILLING a boss at/after WIN_WAVE = victory (bosses hold the wave
    -- clock, so the wave can't pass the milestone without the kill).
    local WIN_WAVE = 20

    mod:system("death", { phase = "death", rate = 30, stagger = 0.5 }, function(dt)
        for e in world:each(Enemy, Health, Position) do
            if e:get(Health).current <= 0 then
                local ep = e:get(Position)
                local xp_value = 1
                if e:has(XpReward) then xp_value = e:get(XpReward).value end
                local orb = spawn_entity(ep.x, ep.y)
                orb:get(Render).kind = KIND.orb
                orb:set(C.Xp, { value = xp_value })
                if math.random() < 0.04 then
                    local heart = spawn_entity(ep.x + 14, ep.y)
                    heart:get(Render).kind = KIND.heart
                    heart:set(C.Heal, {})
                end
                -- A nova-bearer (the boss) always pays out: two hearts + a
                -- spray of bonus orbs around the corpse — and killing one
                -- at/after WIN_WAVE wins the run.
                if e:has(C.Nova) then
                    for i = 1, 2 do
                        local heart = spawn_entity(ep.x - 20 * i, ep.y + 10)
                        heart:get(Render).kind = KIND.heart
                        heart:set(C.Heal, {})
                    end
                    for i = 1, 6 do
                        local a = (i / 6) * 2 * math.pi
                        local bonus = spawn_entity(ep.x + math.cos(a) * 30, ep.y + math.sin(a) * 30)
                        bonus:get(Render).kind = KIND.orb
                        bonus:set(C.Xp, { value = 5 })
                    end
                    if world:wave() >= WIN_WAVE then world:end_game(true) end
                end
                e:destroy()
            end
        end

        local players, alive = 0, 0
        for p in world:each(Player, Hearts, Position) do
            players = players + 1
            if p:has(Downed) then
                if world:wave() >= p:get(Downed).respawn_wave then
                    p:remove(Downed)
                    local h = p:get(Hearts)
                    h.current = h.max
                    local pp = p:get(Position)
                    pp.x, pp.y = 0, 0
                    alive = alive + 1
                end
            elseif p:get(Hearts).current <= 0 then
                p:set(Downed, { respawn_wave = world:wave() + 2 })
                local v = p:get(Velocity)
                if v then v.dx, v.dy = 0, 0 end
            else
                alive = alive + 1
            end
        end

        if players > 0 and alive == 0 then
            world:end_game(false) -- everyone down at once: defeat
        end
    end)

    ----------------------------------------------------------- ranged / object
    -- Ranged enemies fire a hostile bullet at the nearest player in range.
    -- (The standoff behavior lives in targeting — one enemy pass, same tick.)
    -- 20 Hz: cooldowns are ≥ 1.2 s; the timer uses the accumulated dt.
    mod:system("ranged_fire", { phase = "update", rate = 20, stagger = 0.66 }, function(dt)
        for e in world:each(C.Ranged, Position, Enemy) do
            local r = e:get(C.Ranged)
            r.timer = r.timer - dt
            if r.timer <= 0 then
                local ep = e:get(Position)
                local target, d2, tx, ty = nearest_player(ep.x, ep.y)
                if target and d2 < r.range * r.range then
                    local len = math.sqrt(d2)
                    if len > 0 then
                        local b = spawn_bullet(ep.x, ep.y,
                            (tx - ep.x) / len * r.bullet_speed, (ty - ep.y) / len * r.bullet_speed)
                        b:set(C.Bullet, {
                            damage = r.damage,
                            hostile = 1,
                            lifetime = r.range / r.bullet_speed + 0.5
                        })
                        b:get(Render).variant = 1 -- hostile: red
                        r.timer = r.cooldown
                    end
                end
            end
        end
    end)

    -- Boss nova: a radial ring of hostile bullets every Nova.cooldown seconds.
    -- 10 Hz is plenty for multi-second cooldowns; the timer uses the
    -- accumulated dt. One boss alive at a time, so the walk is tiny.
    mod:system("nova", { phase = "update", rate = 10, stagger = 0.33 }, function(dt)
        for e in world:each(C.Nova, Position, Enemy) do
            local nova = e:get(C.Nova)
            nova.timer = nova.timer - dt
            if nova.timer <= 0 then
                local ep = e:get(Position)
                local n = math.floor(nova.bullets)
                for i = 1, n do
                    local a = (i / n) * 2 * math.pi
                    local b = spawn_bullet(ep.x, ep.y, math.cos(a) * nova.bullet_speed,
                                           math.sin(a) * nova.bullet_speed)
                    b:set(C.Bullet, { damage = nova.damage, hostile = 1, lifetime = 2.2 })
                    b:get(Render).variant = 1 -- hostile: red
                end
                nova.timer = nova.cooldown
            end
        end
    end)

    -- Damage aura (Onion): hurt enemies inside any player's aura.
    -- 20 Hz: damage scales by the accumulated dt, so DPS is unchanged.
    mod:system("aura_sys", { phase = "update", rate = 20, stagger = 0.5 }, function(dt)
        for p in world:each(C.Aura, Position, Player) do
            local a = p:get(C.Aura)
            local pp = p:get(Position)
            for e in world:nearby(pp.x, pp.y, a.radius, Enemy, Health) do
                local h = e:get(Health)
                h.current = h.current - a.per_second * dt
            end
        end
    end)

    -- Slow field (Frost Belt): scale enemy velocity inside it (before Movement).
    -- 30 Hz in lockstep with targeting: it rewrites velocity, we scale it once.
    mod:system("slow_sys", { phase = "motion", rate = 30 }, function(dt)
        for p in world:each(C.Slow, Position, Player) do
            local s = p:get(C.Slow)
            local pp = p:get(Position)
            for e in world:nearby(pp.x, pp.y, s.radius, Enemy, Velocity) do
                local v = e:get(Velocity)
                v.dx = v.dx * s.factor
                v.dy = v.dy * s.factor
            end
        end
    end)


    mod:system("gather_all", { phase = "pickup", rate = 5 }, function(dt)
        -- every 50 waves, collect all
        if world:wave() % 50 == 0 then
            for p in world:each(Player, Position) do
                local pp = p:get(Position)
                for orb in world:nearby(pp.x, pp.y, 1000, C.Xp) do
                    world:add_xp(math.tointeger(orb:get(C.Xp).value) or 0)
                    orb:destroy()
                end
                local h = p:get(Hearts)
                if h and h.current < h.max then
                    for heart in world:nearby(pp.x, pp.y, 1000, C.Heal) do
                        h.current = math.floor(math.min(h.max, h.current + heart:get(C.Heal).amount))
                        heart:destroy()
                        if h.current >= h.max then break end
                    end
                end
            end
        end
    end)
end
