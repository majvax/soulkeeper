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
    -- Players fire toward their aim every Weapon.cooldown_max seconds; with
    -- Split Shot the trigger pull fans `projectiles` bullets. Each BULLET
    -- rolls Crit independently: a crit multiplies damage and renders bigger/
    -- orange. Pierce/bounce/leech ride on the bullet (it outlives the pull).
    mod:system("shooting", { phase = "shooting" }, function(dt)
        for p in world:each(Player, Position, C.Weapon, AimState) do
            if p:has(C.AutoTarget) then auto_aim(p) end
            local w = p:get(C.Weapon)
            -- Adrenaline Core: the cooldown ticks faster while overcharged.
            local rate = 1.0
            if p:has(C.Overcharge) then
                local oc = p:get(C.Overcharge)
                if oc.remaining > 0 then rate = 1.0 + oc.mult end
            end
            if w.cooldown > 0 then w.cooldown = w.cooldown - dt * rate end
            local aim = p:get(AimState)
            if aim.firing == 1 and w.cooldown <= 0 and (aim.dx ~= 0 or aim.dy ~= 0) then
                local pp = p:get(Position)
                local crit = p:get(C.Crit)
                local leech = p:has(C.Leech) and p:get(C.Leech).chance or 0
                local n = math.floor(w.projectiles)
                for i = 1, n do
                    local a = n > 1 and 0.14 * ((i - 1) - (n - 1) / 2) or 0
                    local ca, sa = math.cos(a), math.sin(a)
                    local dx, dy = aim.dx * ca - aim.dy * sa, aim.dx * sa + aim.dy * ca
                    local damage = w.damage
                    local is_crit = crit and math.random() < crit.chance
                    if is_crit then damage = damage * crit.multiplier end
                    local b = spawn_bullet(pp.x, pp.y, dx * w.bullet_speed, dy * w.bullet_speed)
                    b:set(C.Bullet, {
                        damage = damage, lifetime = w.lifetime, knockback = w.knockback,
                        pierce = w.pierce, bounces = w.bounces, leech = leech,
                    })
                    if is_crit then b:get(Render).variant = 2 end -- orange
                end
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
                elseif bullet.hit_cd > 0 then
                    bullet.hit_cd = bullet.hit_cd - dt -- clearing the enemy it pierced
                else
                    for e in world:nearby(bp.x, bp.y, br + 40, Enemy, Health, Position, Radius) do
                        local ep = e:get(Position)
                        local dx, dy = ep.x - bp.x, ep.y - bp.y
                        local hit = br + e:get(Radius).value
                        if dx * dx + dy * dy < hit * hit then
                            local h = e:get(Health)
                            h.current = h.current - bullet.damage
                            -- Kill-heal (Leech): the shooter's chance rides the
                            -- bullet; the heart goes to the nearest live player.
                            if h.current <= 0 and bullet.leech > 0
                                and math.random() < bullet.leech then
                                local p = nearest_player(ep.x, ep.y)
                                if p then
                                    local hearts = p:get(Hearts)
                                    hearts.current = math.floor(
                                        math.min(hearts.max, hearts.current + 1))
                                end
                            end
                            -- Impact shove along the bullet's flight (game feel +
                            -- kiting room). Bosses don't budge.
                            if bullet.knockback > 0 and not e:has(C.Boss) and not e:has(C.Nova) then
                                local v = b:get(Velocity)
                                local len = math.sqrt(v.dx * v.dx + v.dy * v.dy)
                                if len > 0 then
                                    ep.x = ep.x + v.dx / len * bullet.knockback
                                    ep.y = ep.y + v.dy / len * bullet.knockback
                                end
                            end
                            if bullet.pierce > 0 then
                                -- Punch through: fly on, briefly blind so the
                                -- same enemy isn't hit twice while overlapping.
                                bullet.pierce = bullet.pierce - 1
                                bullet.hit_cd = 0.08
                            elseif bullet.bounces > 0 then
                                -- Ricochet: re-aim at the nearest OTHER enemy in
                                -- reach (tiny local scan, bullet hits only).
                                local best_d2, bx, by = math.huge, 0, 0
                                for o in world:nearby(bp.x, bp.y, 260, Enemy, Health, Position) do
                                    if o:get(Health).current > 0 then
                                        local op = o:get(Position)
                                        local ox, oy = op.x - bp.x, op.y - bp.y
                                        local od2 = ox * ox + oy * oy
                                        if od2 > hit * hit and od2 < best_d2 then
                                            best_d2, bx, by = od2, op.x, op.y
                                        end
                                    end
                                end
                                if best_d2 < math.huge then
                                    local v = b:get(Velocity)
                                    local speed = math.sqrt(v.dx * v.dx + v.dy * v.dy)
                                    local len = math.sqrt(best_d2)
                                    v.dx, v.dy = (bx - bp.x) / len * speed, (by - bp.y) / len * speed
                                    bullet.bounces = bullet.bounces - 1
                                    bullet.lifetime = bullet.lifetime + 0.4
                                    bullet.hit_cd = 0.08
                                else
                                    b:destroy()
                                end
                            else
                                b:destroy()
                            end
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
                for e in world:nearby(pp.x, pp.y, pr + 40, Enemy, C.Touch, Position, Radius, Health) do
                    local ep = e:get(Position)
                    local dx, dy = ep.x - pp.x, ep.y - pp.y
                    local reach = pr + e:get(Radius).value
                    if dx * dx + dy * dy < reach * reach then
                        -- Spiked Armor: a landed hit bites back.
                        if hurt_player(p, e:get(C.Touch).hearts) and p:has(C.Thorns) then
                            local h = e:get(Health)
                            h.current = h.current - p:get(C.Thorns).damage
                        end
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

    -------------------------------------------------------------------- magnet
    -- Drops sit where the enemy died (spawn_entity gives no Velocity), so a
    -- swarm leaves a field of orbs you have to walk over. The magnet sweeps any
    -- orb/heart within MAGNET_RADIUS toward the nearest player, accelerating as
    -- it closes, until the 45 px pickup grabs it. 60 Hz so the pulled motion
    -- stays smooth under 60 Hz snapshots; the loop is cheap (<=4 players x the
    -- few drops in range). Moves Position directly, like the enemy separation.
    local MAGNET_RADIUS = 130 -- base pull range (C.Magnet upgrades grow it per player)
    local MAGNET_SPEED = 240  -- base px/s; ramps up close to the player
    local VACUUM_SPEED = 900  -- boss-kill sweep: fast pull from anywhere on the map
    local vacuuming = false   -- set on boss kill; clears once no XP orbs remain

    local function magnetize(pp, drop_comp, dt, radius)
        for drop in world:nearby(pp.x, pp.y, radius, drop_comp) do
            local op = drop:get(Position)
            local dx, dy = pp.x - op.x, pp.y - op.y
            local d = math.sqrt(dx * dx + dy * dy)
            if d > 0.001 then
                local speed = MAGNET_SPEED * (1.0 + (1.0 - d / radius)) -- snappier up close
                local step = math.min(d, speed * dt)                    -- never overshoot the player
                op.x = op.x + dx / d * step
                op.y = op.y + dy / d * step
            end
        end
    end

    -- Pull one drop toward its nearest LIVE player at `speed`, from any distance
    -- (the boss-kill vacuum; the normal magnet only reaches MAGNET_RADIUS).
    local function pull_to(drop, dt, speed)
        local op = drop:get(Position)
        local p, _, px, py = nearest_player(op.x, op.y)
        if not p then return end -- everyone downed: leave it be
        local dx, dy = px - op.x, py - op.y
        local d = math.sqrt(dx * dx + dy * dy)
        if d > 0.001 then
            local step = math.min(d, speed * dt)
            op.x = op.x + dx / d * step
            op.y = op.y + dy / d * step
        end
    end

    mod:system("magnet", { phase = "pickup", rate = 60 }, function(dt)
        -- Boss-kill reward: every XP orb rushes to its nearest player from
        -- across the whole map, then the pickup grabs it — the XP STREAMS in
        -- instead of teleporting. Runs until the field is empty so none strand.
        if vacuuming then
            local any = false
            for orb in world:each(C.Xp, Position) do
                any = true
                pull_to(orb, dt, VACUUM_SPEED)
            end
            vacuuming = any
        end
        for p in world:each(Player, Position) do
            if not p:has(Downed) then -- downed players can't collect (see pickups)
                local pp = p:get(Position)
                local radius = p:has(C.Magnet) and p:get(C.Magnet).radius or MAGNET_RADIUS
                magnetize(pp, C.Xp, dt, radius)
                magnetize(pp, C.Heal, dt, radius)
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
            local greed = p:has(C.Greed) and p:get(C.Greed).mult or 1.0
            for orb in world:nearby(pp.x, pp.y, 45, C.Xp) do
                -- add_xp needs a true integer (crashes on a Lua float)
                world:add_xp(math.floor((math.tointeger(orb:get(C.Xp).value) or 0) * greed + 0.5))
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
            -- Boss loot chest: one player opens it, EVERYONE gets the object
            -- pick (the server runs a chest offer round — see levelup.lua).
            for chest in world:nearby(pp.x, pp.y, 55, C.Chest) do
                world:open_chest()
                chest:destroy()
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
    local WIN_WAVE = 50 -- the Game Master finale; its arena WaveHold gates the kill
    -- Downed players come back when a teammate stands close (a progress arc
    -- shows on every client via C.Revive) — or instantly when a boss falls.
    local REVIVE_RADIUS = 90
    local REVIVE_SECONDS = 3.0

    -- Revive a downed player in place with half their hearts.
    local function revive(p)
        p:remove(Downed)
        if p:has(C.Revive) then p:remove(C.Revive) end
        local h = p:get(Hearts)
        h.current = math.floor(math.max(1, math.ceil(h.max / 2)))
    end

    mod:system("death", { phase = "death", rate = 30, stagger = 0.5 }, function(dt)
        for e in world:each(Enemy, Health, Position) do
            if e:get(Health).current <= 0 then
                local ep = e:get(Position)
                local xp_value = 1
                if e:has(XpReward) then xp_value = e:get(XpReward).value end
                local orb = spawn_entity(ep.x, ep.y)
                orb:get(Render).kind = KIND.orb
                orb:set(C.Xp, { value = xp_value })
                if e:has(C.EliteDrop) or math.random() < 0.04 then
                    local heart = spawn_entity(ep.x + 14, ep.y)
                    heart:get(Render).kind = KIND.heart
                    heart:set(C.Heal, {})
                end
                -- Any milestone boss (C.Boss tag) always pays out: a loot
                -- CHEST (the only source of objects — walking over it opens
                -- the team's object pick), two hearts + a spray of bonus orbs
                -- — and killing one at/after WIN_WAVE wins the run.
                if e:has(C.Boss) then
                    local chest = spawn_entity(ep.x, ep.y - 24)
                    chest:get(Render).kind = KIND.chest
                    chest:set(C.Chest, {})
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
                    -- A boss kill picks the whole team back up.
                    for p in world:each(Player, Hearts) do
                        if p:has(Downed) then revive(p) end
                    end
                    -- ...and sweeps every XP orb on the map (the bonus spray plus
                    -- any leftovers) toward the players — the reward streams in.
                    vacuuming = true
                    if world:wave() >= WIN_WAVE then world:end_game(true) end
                end
                if e:has(C.Boss) then
                    mod:emit("on_boss_kill")
                end
                e:destroy()
            end
        end

        local players, alive = 0, 0
        for p in world:each(Player, Hearts, Position) do
            players = players + 1
            if p:has(Downed) then
                -- Proximity revive: a LIVE teammate within REVIVE_RADIUS fills
                -- the bar in REVIVE_SECONDS; it drains at half speed alone.
                -- (nearest_player already ignores Downed players.)
                local pp = p:get(Position)
                if not p:has(C.Revive) then p:set(C.Revive, {}) end
                local rv = p:get(C.Revive)
                local _, d2 = nearest_player(pp.x, pp.y)
                if d2 < REVIVE_RADIUS * REVIVE_RADIUS then
                    rv.progress = rv.progress + dt / REVIVE_SECONDS
                else
                    rv.progress = math.max(0, rv.progress - dt / (REVIVE_SECONDS * 2))
                end
                if rv.progress >= 1 then
                    revive(p)
                    alive = alive + 1
                end
            elseif p:get(Hearts).current <= 0 then
                if p:has(C.Phoenix) then
                    -- Phoenix Feather: consumed instead of going down — back up
                    -- at half hearts with a long i-frame window to escape.
                    p:remove(C.Phoenix)
                    local h = p:get(Hearts)
                    h.current = math.floor(math.max(1, math.ceil(h.max / 2)))
                    p:set(C.IFrames, { remaining = 2.0 })
                    alive = alive + 1
                else
                    p:set(Downed, { respawn_wave = 0 }) -- wave respawn retired: revive instead
                    p:set(C.Revive, {})
                    local v = p:get(Velocity)
                    if v then v.dx, v.dy = 0, 0 end
                end
            else
                alive = alive + 1
            end
        end

        if players > 0 and alive == 0 then
            world:end_game(false) -- everyone down at once: defeat
        end
    end)

    ----------------------------------------------------------- ranged / object
    -- Ranged enemies TELEGRAPH (fx=3, the Prepare pose) for `windup` s once a
    -- player is in range, then fire a hostile bullet at the nearest player.
    -- The shot aims at fire time (dodgeable but honest); a target that fully
    -- escaped 1.25x range during the wind-up whiffs the shot.
    -- (The standoff behavior lives in targeting — one enemy pass, same tick.)
    -- 20 Hz: cooldowns are ≥ 1.2 s; timers use the accumulated dt.
    local function fire_ranged(e, r)
        local ep = e:get(Position)
        local target, d2, tx, ty = nearest_player(ep.x, ep.y)
        local grace = r.range * 1.25
        if not target or d2 >= grace * grace then return false end
        local len = math.sqrt(d2)
        if len <= 0 then return false end
        local dx, dy = (tx - ep.x) / len, (ty - ep.y) / len
        local n = math.floor(r.volley)
        for i = 1, n do
            -- Fan the volley symmetrically around the aim line.
            local a = n > 1 and r.spread * ((i - 1) / (n - 1) - 0.5) or 0
            local ca, sa = math.cos(a), math.sin(a)
            local vx, vy = dx * ca - dy * sa, dx * sa + dy * ca
            local b = spawn_bullet(ep.x, ep.y, vx * r.bullet_speed, vy * r.bullet_speed)
            b:set(C.Bullet, {
                damage = r.damage,
                hostile = 1,
                lifetime = r.range / r.bullet_speed + 0.5
            })
            b:get(Render).variant = math.floor(r.variant)
            if r.bullet_radius > 0 then b:get(Radius).value = r.bullet_radius end
        end
        return true
    end

    mod:system("ranged_fire", { phase = "update", rate = 20, stagger = 0.66 }, function(dt)
        for e in world:each(C.Ranged, Position, Enemy) do
            local r = e:get(C.Ranged)
            if r.winding > 0 then
                r.winding = r.winding - dt
                if r.winding <= 0 then
                    r.timer = r.cooldown -- pay the cooldown even on a whiff
                    if fire_ranged(e, r) then
                        r.anim = 0.5 -- play the attack clip (Render.fx)
                        e:get(Render).fx = 1
                    else
                        e:get(Render).fx = 0
                    end
                end
            else
                r.timer = r.timer - dt
                if r.timer <= 0 then
                    local ep = e:get(Position)
                    local target, d2 = nearest_player(ep.x, ep.y)
                    if target and d2 < r.range * r.range then
                        if r.windup > 0 then
                            r.winding = r.windup
                            e:get(Render).fx = 3 -- telegraph: hold the Prepare pose
                        else
                            r.timer = r.cooldown
                            if fire_ranged(e, r) then
                                r.anim = 0.5
                                e:get(Render).fx = 1
                            end
                        end
                    end
                elseif r.anim > 0 then
                    r.anim = r.anim - dt
                    if r.anim <= 0 then e:get(Render).fx = 0 end
                end
            end
        end
    end)

    -- Bomber arming: close enough to a player, it stops (Speed 0 keeps the
    -- 30 Hz targeting writing a zero velocity — no per-enemy check there),
    -- lights the fuse and holds the fx=3 telegraph pose. 20 Hz: the trigger
    -- radius dwarfs per-50 ms movement. Only unlit bombers pay nearest_player.
    mod:system("bomber_arm", { phase = "update", rate = 20, stagger = 0.15 }, function(dt)
        for e in world:each(C.Bomber, Position, Enemy) do
            if not e:has(C.Fuse) then
                local bomber = e:get(C.Bomber)
                local ep = e:get(Position)
                local _, d2 = nearest_player(ep.x, ep.y)
                if d2 < bomber.trigger * bomber.trigger then
                    e:set(C.Fuse, { timer = bomber.fuse })
                    e:get(Speed).value = 0
                    e:get(Render).fx = 3
                end
            end
        end
    end)

    -- Lit fuses burn down, then the bomber dies in a ring of hostile bullets.
    -- Health goes to 0 (not destroy) so the death system still pays the XP orb;
    -- killing it BEFORE the timer ends skips this system entirely — no blast.
    mod:system("fuse", { phase = "update", rate = 30, stagger = 0.75 }, function(dt)
        for e in world:each(C.Fuse, C.Bomber, Position) do
            local fuse = e:get(C.Fuse)
            fuse.timer = fuse.timer - dt
            if fuse.timer <= 0 then
                local bomber = e:get(C.Bomber)
                local ep = e:get(Position)
                local n = math.floor(bomber.blast_bullets)
                for i = 1, n do
                    local a = (i / n) * 2 * math.pi
                    local b = spawn_bullet(ep.x, ep.y, math.cos(a) * bomber.blast_speed,
                        math.sin(a) * bomber.blast_speed)
                    b:set(C.Bullet, { damage = bomber.damage, hostile = 1,
                                      lifetime = bomber.blast_range / bomber.blast_speed })
                    b:get(Render).variant = 1
                end
                e:get(Health).current = 0 -- the death system drops the orb
            end
        end
    end)

    -- Melee lunge (Berserker/Mimic): wind up frozen in place (fx=3), lock the
    -- dash line at wind-up START (dodgeable), then burst along it (fx=2).
    -- Motion phase in LOCKSTEP with targeting (like slow_sys): targeting wrote
    -- velocity this same tick, the dash overwrites it.
    mod:system("lunge_sys", { phase = "motion", rate = 30 }, function(dt)
        for e in world:each(C.Lunge, Position, Velocity, Speed, Enemy) do
            local l = e:get(C.Lunge)
            if l.dashing > 0 then
                l.dashing = l.dashing - dt
                local v = e:get(Velocity)
                if l.dashing <= 0 then -- recover: hand motion back to targeting
                    v.dx, v.dy = 0, 0
                    e:get(Speed).value = l.saved_speed
                    e:get(Render).fx = 0
                    l.timer = l.cooldown
                    -- Landing slam (Frog Prince): a ring of hostile bullets
                    -- where the dash ended — don't stand at the landing spot.
                    local n = math.floor(l.burst)
                    if n > 0 then
                        local ep = e:get(Position)
                        for i = 1, n do
                            local a = (i / n) * 2 * math.pi
                            local b = spawn_bullet(ep.x, ep.y, math.cos(a) * l.burst_speed,
                                math.sin(a) * l.burst_speed)
                            b:set(C.Bullet, { damage = l.burst_damage, hostile = 1, lifetime = 0.7 })
                            b:get(Render).variant = 1
                        end
                    end
                else
                    v.dx, v.dy = l.dx * l.speed, l.dy * l.speed
                end
            elseif l.winding > 0 then
                l.winding = l.winding - dt
                if l.winding <= 0 then
                    l.dashing = l.duration
                    e:get(Render).fx = 2
                end
            else
                l.timer = l.timer - dt
                if l.timer <= 0 then
                    local ep = e:get(Position)
                    local _, d2, tx, ty = nearest_player(ep.x, ep.y)
                    if d2 < l.range * l.range then
                        local len = math.sqrt(d2)
                        if len > 0 then
                            l.dx, l.dy = (tx - ep.x) / len, (ty - ep.y) / len
                            l.winding = l.windup
                            l.saved_speed = e:get(Speed).value
                            e:get(Speed).value = 0 -- targeting parks it for the telegraph
                            e:get(Render).fx = 3
                        end
                    else
                        l.timer = 0.2 -- out of range: re-check shortly
                    end
                end
            end
        end
    end)

    -- Boss nova: a radial ring of hostile bullets every Nova.cooldown seconds,
    -- with RAGE phases keyed on its health: < 50% the rings come faster and
    -- denser plus a 3-bullet aimed volley; < 25% the boss itself speeds up.
    -- 10 Hz is plenty for multi-second cooldowns; the timer uses the
    -- accumulated dt. One boss alive at a time, so the walk is tiny.
    mod:system("nova", { phase = "update", rate = 10, stagger = 0.33 }, function(dt)
        for e in world:each(C.Nova, Position, Health, Enemy) do
            local nova = e:get(C.Nova)
            local h = e:get(Health)
            local frac = h.current / h.max
            if frac < 0.5 and nova.phase < 1 then
                nova.phase = 1
                nova.cooldown = nova.cooldown * 0.6
                nova.bullets = nova.bullets + 8
            end
            if frac < 0.25 and nova.phase < 2 then
                nova.phase = 2
                local s = e:get(Speed)
                if s then s.value = s.value * 1.6 end
            end
            nova.timer = nova.timer - dt
            if nova.timer <= 0 then
                local ep = e:get(Position)
                local n = math.floor(nova.bullets)
                -- spin > 0: each ring rotates past the last — the gaps WALK,
                -- so hugging one safe lane stops working (spiral bullet hell).
                nova.angle = nova.angle + nova.spin
                for i = 1, n do
                    local a = nova.angle + (i / n) * 2 * math.pi
                    local b = spawn_bullet(ep.x, ep.y, math.cos(a) * nova.bullet_speed,
                        math.sin(a) * nova.bullet_speed)
                    b:set(C.Bullet, { damage = nova.damage, hostile = 1, lifetime = 2.2 })
                    b:get(Render).variant = 1 -- hostile: red
                end
                -- Enraged: an aimed 3-bullet volley punishes standing still.
                if nova.phase >= 1 then
                    local _, d2, px, py = nearest_player(ep.x, ep.y)
                    if d2 < math.huge then
                        local len = math.sqrt(d2)
                        if len > 0 then
                            local dx, dy = (px - ep.x) / len, (py - ep.y) / len
                            for k = -1, 1 do
                                local a = k * 0.22
                                local ca, sa = math.cos(a), math.sin(a)
                                local vx, vy = dx * ca - dy * sa, dx * sa + dy * ca
                                local b = spawn_bullet(ep.x, ep.y, vx * nova.bullet_speed * 1.5,
                                    vy * nova.bullet_speed * 1.5)
                                b:set(C.Bullet, { damage = nova.damage, hostile = 1, lifetime = 2.0 })
                                b:get(Render).variant = 3 -- heavy: reads as the aimed shot
                            end
                        end
                    end
                end
                nova.timer = nova.cooldown
                nova.anim = 0.9 -- play the boss's attack clip (Render.fx)
                e:get(Render).fx = 1
            elseif nova.anim > 0 then
                nova.anim = nova.anim - dt
                if nova.anim <= 0 then e:get(Render).fx = 0 end
            end
        end
    end)

    -- Summoner bosses: call minions to their side on a cooldown and BLINK
    -- away from any player that closes in. 10 Hz: both timers are
    -- multi-second; the blink check radius dwarfs per-100 ms movement.
    local SUMMON_POOLS = {
        { "core:bandit", "core:scout", "core:marauder" },                -- 1: trash
        { "core:scout", "core:scout", "core:scout_elite" },              -- 2: bat swarm
        { "core:berserker_elite", "core:slasher_elite", "core:bomber_elite" }, -- 3: ELITES
    }
    mod:system("summon_sys", { phase = "update", rate = 10, stagger = 0.65 }, function(dt)
        for e in world:each(C.Summon, Position, Enemy) do
            local s = e:get(C.Summon)
            local ep = e:get(Position)
            s.timer = s.timer - dt
            if s.timer <= 0 then
                local kinds = SUMMON_POOLS[math.floor(s.pool)] or SUMMON_POOLS[1]
                local n = math.floor(s.count + world:wave() / 10)
                for i = 1, n do
                    local a = math.random() * 2 * math.pi
                    spawn_enemy(ep.x + math.cos(a) * 110, ep.y + math.sin(a) * 110,
                                kinds[math.random(#kinds)])
                end
                s.timer = s.cooldown
                s.anim = 0.8 -- the ATK clip doubles as the summon cast
                e:get(Render).fx = 1
            elseif s.anim > 0 then
                s.anim = s.anim - dt
                if s.anim <= 0 then e:get(Render).fx = 0 end
            end
            s.blink_cd = math.max(0, s.blink_cd - dt)
            if s.blink_cd <= 0 then
                local _, d2, px, py = nearest_player(ep.x, ep.y)
                if d2 < s.blink_range * s.blink_range then
                    local dx, dy = ep.x - px, ep.y - py
                    local len = math.sqrt(dx * dx + dy * dy)
                    if len < 1 then dx, dy, len = 1, 0, 1 end
                    ep.x = ep.x + dx / len * s.blink_dist
                    ep.y = ep.y + dy / len * s.blink_dist
                    s.blink_cd = s.blink_cooldown
                end
            end
        end
    end)

    -- Boss arena confinement: while a nova-bearer lives, players (and the
    -- boss itself) can't leave the FIXED rect recorded at its spawn — no
    -- hit-and-run. The client predicts the same clamp (via the archetype's
    -- `arena` opt) so the wall doesn't rubber-band. 60 Hz keeps the overshoot
    -- under ~4 px at run speed.
    local function rect_clamp(pos, nova)
        pos.x = math.max(nova.cx - nova.arena_w, math.min(nova.cx + nova.arena_w, pos.x))
        pos.y = math.max(nova.cy - nova.arena_h, math.min(nova.cy + nova.arena_h, pos.y))
    end
    mod:system("arena", { phase = "update", rate = 60, stagger = 0.25 }, function(dt)
        for e in world:each(C.Nova, Position) do
            local nova = e:get(C.Nova)
            if nova.arena_w > 0 then
                rect_clamp(e:get(Position), nova) -- the boss stays home too
                for p in world:each(Player, Position) do
                    rect_clamp(p:get(Position), nova)
                end
            end
        end
    end)

    -- Orbiting Blades: spin the phase, damage enemies at each blade point.
    -- 20 Hz: dps scales by the accumulated dt. The networked phase is what
    -- the client draws, so the cut and the visual agree.
    mod:system("orbit_sys", { phase = "update", rate = 20, stagger = 0.8 }, function(dt)
        for p in world:each(C.Orbit, Position, Player) do
            local o = p:get(C.Orbit)
            o.phase = o.phase + dt * o.spin
            local pp = p:get(Position)
            local n = math.floor(o.count)
            for i = 1, n do
                local a = o.phase + (i / n) * 2 * math.pi
                local bx = pp.x + math.cos(a) * o.radius
                local by = pp.y + math.sin(a) * o.radius
                for e in world:nearby(bx, by, 26, Enemy, Health, Position) do
                    local ep = e:get(Position)
                    local dx, dy = ep.x - bx, ep.y - by
                    if dx * dx + dy * dy < 26 * 26 then
                        local h = e:get(Health)
                        h.current = h.current - o.dps * dt
                    end
                end
            end
        end
    end)

    -- Adrenaline Core: a finished dash burst grants a fire-rate window (the
    -- shooting system reads `remaining`). 30 Hz sees every burst end (bursts
    -- last far longer than 33 ms).
    mod:system("overcharge_sys", { phase = "update", rate = 30, stagger = 0.9 }, function(dt)
        for p in world:each(C.Overcharge, Dash, Player) do
            local oc = p:get(C.Overcharge)
            local bursting = p:get(Dash).burst_remaining > 0 and 1 or 0
            if oc.was_bursting == 1 and bursting == 0 then
                oc.remaining = oc.duration
            end
            oc.was_bursting = bursting
            if oc.remaining > 0 then oc.remaining = oc.remaining - dt end
        end
    end)

    -- Bomb/bramble planter (Bomb Lord, Elder Ent): drop stationary hazards at
    -- random ground spots — inside the boss's own arena rect when it has one
    -- (C.Nova), else scattered around it. The hazards are "core:mine"/
    -- "core:bramble" archetypes reusing the full C.Bomber tech: shootable,
    -- trigger + fuse + blast — they eat the arena's safe ground over time.
    local PLANT_KINDS = { "core:mine", "core:bramble" }
    mod:system("planter_sys", { phase = "update", rate = 10, stagger = 0.45 }, function(dt)
        for e in world:each(C.Planter, Position, Enemy) do
            local pl = e:get(C.Planter)
            pl.timer = pl.timer - dt
            if pl.timer <= 0 then
                local ep = e:get(Position)
                local kind = PLANT_KINDS[math.floor(pl.kind)] or PLANT_KINDS[1]
                for i = 1, math.floor(pl.count) do
                    local x, y
                    if e:has(C.Nova) and e:get(C.Nova).arena_w > 0 then
                        local nova = e:get(C.Nova)
                        x = nova.cx + (math.random() * 2 - 1) * (nova.arena_w - 80)
                        y = nova.cy + (math.random() * 2 - 1) * (nova.arena_h - 80)
                    else
                        local a = math.random() * 2 * math.pi
                        local r = 150 + math.random() * 300
                        x, y = ep.x + math.cos(a) * r, ep.y + math.sin(a) * r
                    end
                    spawn_enemy(x, y, kind)
                end
                pl.timer = pl.cooldown
                pl.anim = 0.6
                e:get(Render).fx = 1
            elseif pl.anim > 0 then
                pl.anim = pl.anim - dt
                if pl.anim <= 0 then e:get(Render).fx = 0 end
            end
        end
    end)

    -- Passive boss regen (Vampire Lord): a DPS check — heal steadily, capped
    -- at max. 10 Hz with accumulated dt keeps the rate exact.
    mod:system("regen_sys", { phase = "update", rate = 10, stagger = 0.85 }, function(dt)
        for e in world:each(C.Regen, Health, Enemy) do
            local h = e:get(Health)
            if h.current > 0 and h.current < h.max then
                h.current = math.min(h.max, h.current + e:get(C.Regen).per_second * dt)
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



    -- Every 50 waves, vacuum all drops into the team. This MUST live inside a
    -- system: at module scope it ran at LOAD time and indexed the sim-only
    -- `world` global, which is nil in the client's render VM (crash -> plugin
    -- hash mismatch -> join denied). rate=2 — it only matters on a wave boundary.
    -- (Dormant while WIN_WAVE=20 caps a run below 50; raise the cap or lower the
    -- 50 to actually trigger it.)
    mod:system("wave_vacuum", { phase = "pickup", rate = 2 }, function()
        if world:wave() % 50 ~= 0 then return end
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
    end)

    mod:subscribe("on_boss_kill", function()
        for p in world:each(Player, Hearts) do
            if p:has(Downed) then revive(p) end
        end
    end)
end
