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
        -- Reactive Plating: the lost heart bites back — a friendly burst from
        -- the hit. This is THE player-damage site, so every hit source counts.
        if p:has(C.Reactive) then
            local ra = p:get(C.Reactive)
            local pp = p:get(Position)
            local pid = p:id()
            local n = math.floor(ra.bullets)
            for i = 1, n do
                local a = (i / n) * 2 * math.pi
                local b = spawn_bullet(pp.x, pp.y, math.cos(a) * ra.bullet_speed,
                    math.sin(a) * ra.bullet_speed)
                b:set(C.Bullet, { damage = ra.damage, lifetime = ra.lifetime, owner = pid })
                b:get(Render).variant = 9 -- electric zap (visual + shoot_9 cast sound)
            end
        end
        return true
    end

    -- Run-stats scoreboard: credit damage (and a kill) to a player handle, or
    -- to a bullet's stamped owner id (a <=4-player scan — hits are rare next
    -- to ticks). Shown on the game-over screen.
    local function credit(p, damage, killed)
        local rs = p:get(RunStats)
        if rs then
            rs.damage = rs.damage + damage
            if killed then rs.kills = math.floor(rs.kills + 1) end
        end
        -- Hunter's Instinct: every kill YOU land refunds dash cooldown. Riding
        -- the credit path covers every damage source (bullets/orbit/aura/dash).
        if killed and p:has(C.Hunter) then
            local d = p:get(Dash)
            if d then d.cooldown = math.max(0, d.cooldown - p:get(C.Hunter).refund) end
        end
    end
    local function credit_owner(owner_id, damage, killed)
        if owner_id == 0 then return end
        for p in world:each(Player, RunStats) do
            if p:id() == owner_id then
                credit(p, damage, killed)
                return
            end
        end
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
                local pid = p:id() -- stamped on each bullet for kill credit
                -- One volley per direction: forward always, backward too with
                -- the Mirror Barrel (each bullet rolls its own crit).
                local function volley(ax, ay)
                    local n = math.floor(w.projectiles)
                    for i = 1, n do
                        local a = n > 1 and 0.14 * ((i - 1) - (n - 1) / 2) or 0
                        local ca, sa = math.cos(a), math.sin(a)
                        local dx, dy = ax * ca - ay * sa, ax * sa + ay * ca
                        local damage = w.damage
                        local is_crit = crit and math.random() < crit.chance
                        if is_crit then damage = damage * crit.multiplier end
                        local b = spawn_bullet(pp.x, pp.y, dx * w.bullet_speed, dy * w.bullet_speed)
                        b:set(C.Bullet, {
                            damage = damage,
                            lifetime = w.lifetime,
                            pierce = w.pierce,
                            bounces = w.bounces,
                            leech = leech,
                            owner = pid,
                            cull = w.cull,         -- Reaper execute threshold
                            volatile = w.volatile, -- Volatile Rounds kill-burst
                        })
                        if w.bullet_radius > 4 then b:get(Radius).value = w.bullet_radius end
                        if is_crit then b:get(Render).variant = 2 end -- orange
                    end
                end
                volley(aim.dx, aim.dy)
                if w.mirror == 1 then volley(-aim.dx, -aim.dy) end
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
                            local was_alive = h.current > 0
                            -- Shieldbearer plate: flat reduction per bullet, floored
                            -- at 1 (chip damage always lands — never immune).
                            local dmg = bullet.damage
                            if e:has(C.Armor) then
                                dmg = math.max(1, dmg - e:get(C.Armor).flat)
                            end
                            h.current = h.current - dmg
                            -- Reaper: a hit that leaves non-boss trash under the
                            -- execute threshold finishes it outright.
                            if h.current > 0 and bullet.cull > 0
                                and h.current < bullet.cull * h.max
                                and not e:has(C.Boss) then
                                h.current = 0
                            end
                            -- Floating number; crit bullets were tagged variant 2 at fire time.
                            world:damage_number(ep.x, ep.y, dmg,
                                b:get(Render).variant == 2 and 1 or 0)
                            -- Corpse overlaps (death system runs at 30 Hz) don't pad stats.
                            if was_alive then
                                credit_owner(bullet.owner, dmg, h.current <= 0)
                            end
                            -- Volatile Rounds: the killing bullet detonates a small
                            -- friendly burst at the victim. Burst bullets carry
                            -- volatile = 0 — no chain reactions.
                            if was_alive and h.current <= 0 and bullet.volatile > 0 then
                                for k = 1, 6 do
                                    local a = (k / 6) * 2 * math.pi
                                    local vb = spawn_bullet(ep.x, ep.y,
                                        math.cos(a) * 280, math.sin(a) * 280)
                                    vb:set(C.Bullet, {
                                        damage = bullet.volatile,
                                        lifetime = 0.4,
                                        owner = bullet.owner,
                                    })
                                end
                            end
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
                            local was_alive = h.current > 0
                            local thorns = p:get(C.Thorns).damage
                            h.current = h.current - thorns
                            world:damage_number(ep.x, ep.y, thorns, 0)
                            if was_alive then credit(p, thorns, h.current <= 0) end
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
                    local was_alive = h.current > 0
                    h.current = h.current - d.shockwave
                    world:damage_number(ep.x, ep.y, d.shockwave, 0)
                    if was_alive then credit(p, d.shockwave, h.current <= 0) end
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
                -- Hearts only pull toward a HURT player — a full-HP player
                -- must not bank a train of reserve hearts behind them.
                local h = p:get(Hearts)
                if h and h.current < h.max then
                    magnetize(pp, C.Heal, dt, radius)
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
                -- Boss-spawned minions (C.NoLoot) die without paying anything:
                -- summon spam must be pressure, not an XP/heart fountain.
                if e:has(C.NoLoot) then goto skip_loot end
                -- Supply crate (map POI): its OWN payout table replaces the
                -- standard drop — 60% a 3-orb burst / 30% a heart (the area
                -- cap still applies) / 10% a 5-orb jackpot.
                if e:has(C.CrateLoot) then
                    do
                        local roll = math.random()
                        local orbs = roll < 0.60 and 3 or (roll >= 0.90 and 5 or 0)
                        for i = 1, orbs do
                            local a = (i / orbs) * 2 * math.pi
                            local orb = spawn_entity(ep.x + math.cos(a) * 18,
                                                     ep.y + math.sin(a) * 18)
                            orb:get(Render).kind = KIND.orb
                            orb:set(C.Xp, { value = 2 })
                        end
                        if orbs == 0 then
                            local stocked = 0
                            for _ in world:nearby(ep.x, ep.y, 240, C.Heal) do
                                stocked = stocked + 1
                            end
                            if stocked < 3 then
                                local heart = spawn_entity(ep.x, ep.y)
                                heart:get(Render).kind = KIND.heart
                                heart:set(C.Heal, {})
                            end
                        end
                    end
                    goto skip_loot
                end
                do -- scope the payout locals so the goto can't jump into them
                local xp_value = 1
                if e:has(XpReward) then xp_value = e:get(XpReward).value end
                local orb = spawn_entity(ep.x, ep.y)
                orb:get(Render).kind = KIND.orb
                orb:set(C.Xp, { value = xp_value })
                if e:has(C.EliteDrop) or math.random() < 0.04 then
                    -- Late waves kill fast enough to CARPET the map in hearts
                    -- (an unkillable reserve): suppress the drop when the area
                    -- is already stocked. Boss payouts skip this cap.
                    local stocked = 0
                    for _ in world:nearby(ep.x, ep.y, 240, C.Heal) do
                        stocked = stocked + 1
                    end
                    if stocked < 3 then
                        local heart = spawn_entity(ep.x + 14, ep.y)
                        heart:get(Render).kind = KIND.heart
                        heart:set(C.Heal, {})
                    end
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
                end -- payout scope
                ::skip_loot::
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
                local reviver, d2 = nearest_player(pp.x, pp.y)
                if d2 < REVIVE_RADIUS * REVIVE_RADIUS then
                    rv.progress = rv.progress + dt / REVIVE_SECONDS
                else
                    rv.progress = math.max(0, rv.progress - dt / (REVIVE_SECONDS * 2))
                end
                if rv.progress >= 1 then
                    revive(p)
                    -- Scoreboard credit to whoever finished the arc (boss-kill
                    -- mass revives credit nobody — no single rescuer).
                    if reviver then
                        local rs = reviver:get(RunStats)
                        if rs then rs.revives = math.floor(rs.revives + 1) end
                    end
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
                    local rs = p:get(RunStats)
                    if rs then rs.downs = math.floor(rs.downs + 1) end
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

    -- Points of interest: every ~20-30 s, drop a supply dummy 500-800 px from
    -- a random live player — the infinite plane gets things worth walking to.
    -- ~10% of the rolls place a MIMIC instead (the sleeping ambusher): the POI
    -- that bites back. Skipped during arena fights (the entrance sweep would
    -- eat it) and capped so the map never turns into a loot farm.
    local poi_timer = 12.0 -- lands early in wave 2: teach the habit
    mod:system("poi_spawner", { phase = "update", rate = 1, stagger = 0.12 }, function(dt)
        if world:wave() < 2 then return end -- wave 1 is the learn-the-map minute
        poi_timer = poi_timer - dt
        if poi_timer > 0 then return end
        for _ in world:each(WaveHold) do return end -- arena fight: hold the roll
        poi_timer = 20 + math.random() * 10
        local crates = 0
        for _ in world:each(C.CrateLoot) do crates = crates + 1 end
        if crates >= 6 then return end
        local players = {}
        for p in world:each(Player, Position) do
            if not p:has(Downed) then players[#players + 1] = p end
        end
        if #players == 0 then return end
        local pp = players[math.random(#players)]:get(Position)
        local a = math.random() * 2 * math.pi
        local r = 500 + math.random() * 300
        spawn_enemy(pp.x + math.cos(a) * r, pp.y + math.sin(a) * r,
                    math.random() < 0.10 and "core:mimic" or "core:crate")
    end)

    -- Dropped hearts go STALE: a generous grab window, then they fade — the
    -- drop cap above bounds density, this bounds time (no permanent heart
    -- carpets banking an unkillable reserve). 2 Hz + accumulated dt is plenty.
    mod:system("heal_decay", { phase = "update", rate = 2, stagger = 0.75 }, function(dt)
        for h in world:each(C.Heal) do
            local heal = h:get(C.Heal)
            heal.ttl = heal.ttl - dt
            if heal.ttl <= 0 then h:destroy() end
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
            -- Homing shot (the Acolyte's orb): the bullet steers after you.
            if r.homing > 0 then b:set(C.Homing, { turn = r.homing }) end
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
                        r.anim = 0.5     -- play the attack clip (Render.fx)
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

    -- Ambusher (the Mimic): inert until a player closes within trigger range,
    -- then a ONE-WAY wake — Speed snaps on and the reveal flashes the ATK
    -- clip. 10 Hz: the trigger radius dwarfs per-100 ms movement.
    mod:system("ambush_sys", { phase = "update", rate = 10, stagger = 0.3 }, function(dt)
        for e in world:each(C.Ambush, Position, Enemy) do
            local am = e:get(C.Ambush)
            if am.awake == 0 then
                local ep = e:get(Position)
                local _, d2 = nearest_player(ep.x, ep.y)
                if d2 < am.trigger * am.trigger then
                    am.awake = 1
                    e:get(Speed).value = am.wake_speed
                    am.anim = 0.5
                    e:get(Render).fx = 1
                end
            elseif am.anim > 0 then
                am.anim = am.anim - dt
                if am.anim <= 0 then e:get(Render).fx = 0 end
            end
        end
    end)

    -- Fool's gold (the Mimic King): a fake orb that pops into a hostile burst
    -- when reached for — or when its fuse gives up on you. 20 Hz: the 60 px
    -- trigger sits just past the 45 px pickup radius, so it fires in reach.
    mod:system("fools_gold_sys", { phase = "update", rate = 20, stagger = 0.42 }, function(dt)
        for g in world:each(C.FoolsGold, Position) do
            local fg = g:get(C.FoolsGold)
            fg.fuse = fg.fuse - dt
            local gp = g:get(Position)
            local _, d2 = nearest_player(gp.x, gp.y)
            if fg.fuse <= 0 or d2 < fg.trigger * fg.trigger then
                if d2 < fg.trigger * fg.trigger then -- reached for: it bites
                    local n = math.floor(fg.bullets)
                    for i = 1, n do
                        local a = (i / n) * 2 * math.pi
                        local b = spawn_bullet(gp.x, gp.y, math.cos(a) * fg.bullet_speed,
                            math.sin(a) * fg.bullet_speed)
                        b:set(C.Bullet, { damage = fg.damage, hostile = 1, lifetime = 0.5 })
                        b:get(Render).variant = 5 -- coin pellets: the Mimic King's palette
                    end
                end
                g:destroy() -- an expired fuse just fizzles the lure
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
                    b:set(C.Bullet, {
                        damage = bomber.damage,
                        hostile = 1,
                        lifetime = bomber.blast_range / bomber.blast_speed
                    })
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

    -- The Frog King's nova (HIS signature — no other boss rings): a radial
    -- ring of hostile bullets every Nova.cooldown seconds, with RAGE phases
    -- keyed on its health: < 50% the rings come faster and denser; < 25% the
    -- boss itself speeds up. 10 Hz is plenty for multi-second cooldowns; the
    -- timer uses the accumulated dt. One boss alive at a time: tiny walk.
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
                    b:get(Render).variant = 8 -- frog spit: bright-green ring pellet
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
        { "core:bandit",          "core:scout",         "core:marauder" },     -- 1: trash
        { "core:scout",           "core:scout",         "core:scout_elite" },  -- 2: bat swarm
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
                    local add = spawn_enemy(ep.x + math.cos(a) * 110, ep.y + math.sin(a) * 110,
                        kinds[math.random(#kinds)])
                    if add then add:set(C.NoLoot, {}) end -- summons pay nothing
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

    -- Boss arena confinement: while a C.Arena bearer lives, players (and the
    -- boss itself) can't leave the FIXED rect recorded at its spawn — no
    -- hit-and-run. The client predicts the same clamp (via the archetype's
    -- `arena` opt) so the wall doesn't rubber-band. 60 Hz keeps the overshoot
    -- under ~4 px at run speed.
    local function rect_clamp(pos, arena, margin)
        pos.x = math.max(arena.cx - arena.w + margin, math.min(arena.cx + arena.w - margin, pos.x))
        pos.y = math.max(arena.cy - arena.h + margin, math.min(arena.cy + arena.h - margin, pos.y))
    end
    mod:system("arena", { phase = "update", rate = 60, stagger = 0.25 }, function(dt)
        -- Terrain clearing: while an arena fight is live, the kernel Terrain
        -- singleton's clear circle covers the whole rect (diag radius — the
        -- SAME formula the client mirrors) so charges/walls/rings fight on
        -- flat ground. Reset when no arena bearer lives.
        local arena_live = false
        for e in world:each(C.Arena, Position) do
            local arena = e:get(C.Arena)
            if arena.w > 0 then
                arena_live = true
                for t in world:each(Terrain) do
                    local terrain = t:get(Terrain)
                    terrain.clear_x = arena.cx
                    terrain.clear_y = arena.cy
                    terrain.clear_r = math.sqrt(arena.w * arena.w + arena.h * arena.h)
                end
            end
        end
        if not arena_live then
            for t in world:each(Terrain) do
                local terrain = t:get(Terrain)
                if terrain.clear_r > 0 then terrain.clear_r = 0 end
            end
        end
        for e in world:each(C.Arena, Position) do
            local arena = e:get(C.Arena)
            if arena.w > 0 then
                -- The boss clamps to an INSET rect (blink pushes can't lodge
                -- it in the corner) — but an inset is still a wall: if the
                -- clamp is actively HOLDING it (chasing a wall-hugger), it
                -- teleports to a random arena point instead of grinding.
                local ep = e:get(Position)
                local px, py = ep.x, ep.y
                rect_clamp(ep, arena, 90)
                if math.abs(ep.x - px) + math.abs(ep.y - py) > 0.5 then
                    arena.pinned = arena.pinned + dt
                    if arena.pinned > 1.2 then
                        ep.x = arena.cx + (math.random() * 2 - 1) * arena.w * 0.55
                        ep.y = arena.cy + (math.random() * 2 - 1) * arena.h * 0.55
                        arena.pinned = 0
                    end
                else
                    arena.pinned = 0
                end
                for p in world:each(Player, Position) do
                    rect_clamp(p:get(Position), arena, 0)
                end
            end
        end
    end)

    -- Orbiting Blades: spin the phase, damage enemies at each blade point.
    -- 60 Hz: the networked phase is what the client draws — at 20 Hz it
    -- stepped visibly under 60 Hz snapshots (user: "laggy"). ≤4 players x ≤8
    -- blades keeps the walk trivial. Cut damage scales with Weapon.damage so
    -- the blades stay lethal against wave-compounded health.
    mod:system("orbit_sys", { phase = "update", rate = 60, stagger = 0.8 }, function(dt)
        for p in world:each(C.Orbit, Position, Player) do
            local o = p:get(C.Orbit)
            o.phase = o.phase + dt * o.spin
            local pp = p:get(Position)
            local w = p:get(C.Weapon)
            local dps = o.dps + 0.7 * (w and w.damage or 0)
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
                        local was_alive = h.current > 0
                        h.current = h.current - dps * dt
                        -- No floating numbers for aura ticks (spam), but the
                        -- damage still counts on the scoreboard.
                        if was_alive then credit(p, dps * dt, h.current <= 0) end
                    end
                end
            end
        end
    end)

    -- Identity enforcement: the one-time identity objects declare LIVE rules
    -- ("my speed is capped", "my crit tracks pierce") — this keeps them true
    -- against later picks (Lead Plates, Sharp Rounds, boss buffs...). 2 Hz on
    -- <=4 players is free.
    mod:system("identity_sys", { phase = "update", rate = 2, stagger = 0.5 }, function(dt)
        for p in world:each(Player) do
            if p:has(C.Goliath) then
                local s = p:get(Speed)
                if s and s.value > 200 then s.value = 200 end
            end
            if p:has(C.David) then
                local h = p:get(Hearts)
                if h and h.max > 3 then
                    h.max = 3
                    h.current = math.floor(math.min(h.current, 3))
                end
            end
            if p:has(C.Executioner) then
                local w = p:get(C.Weapon)
                local c = p:get(C.Crit)
                if w and c then c.chance = math.min(1.0, 0.08 * w.pierce) end
            end
        end
    end)

    -- Static Charge: a periodic friendly shock ring from the player, damage
    -- keyed to the CURRENT weapon so the object keeps pace with the build.
    -- 10 Hz: the cooldown is seconds; the timer uses the accumulated dt.
    mod:system("static_sys", { phase = "update", rate = 10, stagger = 0.58 }, function(dt)
        for p in world:each(C.Static, Position, Player) do
            local st = p:get(C.Static)
            st.timer = st.timer - dt
            if st.timer <= 0 then
                st.timer = st.cooldown
                local pp = p:get(Position)
                local w = p:get(C.Weapon)
                local damage = 0.5 * (w and w.damage or 10)
                local pid = p:id()
                local n = math.floor(st.bullets)
                for i = 1, n do
                    local a = (i / n) * 2 * math.pi
                    local b = spawn_bullet(pp.x, pp.y, math.cos(a) * st.bullet_speed,
                        math.sin(a) * st.bullet_speed)
                    b:set(C.Bullet, { damage = damage, lifetime = st.lifetime, owner = pid })
                    b:get(Render).variant = 9 -- electric zap (visual + shoot_9 cast sound)
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

    -- Bomb toss (Bomb Lord): lob a PRE-LIT keg at each live player's spot —
    -- ground denial ON you (the carpet planter denies random ground). The keg
    -- is a core:mine whose C.Fuse is already burning; the existing fuse
    -- system detonates it, and it's shootable until then.
    mod:system("toss_sys", { phase = "update", rate = 10, stagger = 0.05 }, function(dt)
        for e in world:each(C.Toss, Position, Enemy) do
            local t = e:get(C.Toss)
            t.timer = t.timer - dt
            if t.timer <= 0 then
                for p in world:each(Player, Position) do
                    if not p:has(Downed) then
                        local pp = p:get(Position)
                        local a = math.random() * 2 * math.pi
                        local r = math.random() * t.scatter
                        local keg = spawn_enemy(pp.x + math.cos(a) * r, pp.y + math.sin(a) * r,
                            "core:mine")
                        if keg then
                            keg:set(C.Fuse, { timer = t.fuse })
                            keg:set(C.NoLoot, {})
                        end
                    end
                end
                t.timer = t.cooldown
                t.anim = 0.6
                e:get(Render).fx = 1
            elseif t.anim > 0 then
                t.anim = t.anim - dt
                if t.anim <= 0 then e:get(Render).fx = 0 end
            end
        end
    end)

    -- Homing blood bolts (Vampire Lord): cast a few slow bullets that STEER.
    mod:system("bolt_sys", { phase = "update", rate = 10, stagger = 0.2 }, function(dt)
        for e in world:each(C.BoltCaster, Position, Enemy) do
            local bc = e:get(C.BoltCaster)
            bc.timer = bc.timer - dt
            if bc.timer <= 0 then
                local ep = e:get(Position)
                local _, d2, px, py = nearest_player(ep.x, ep.y)
                if d2 < math.huge then
                    local len = math.max(1, math.sqrt(d2))
                    for i = 1, math.floor(bc.bolts) do
                        -- Launch fanned; the homing does the aiming.
                        local a = (i - (bc.bolts + 1) / 2) * 0.5
                        local ca, sa = math.cos(a), math.sin(a)
                        local dx, dy = (px - ep.x) / len, (py - ep.y) / len
                        local vx, vy = dx * ca - dy * sa, dx * sa + dy * ca
                        local b = spawn_bullet(ep.x, ep.y, vx * bc.bullet_speed, vy * bc.bullet_speed)
                        b:set(C.Bullet, { damage = bc.damage, hostile = 1, lifetime = bc.lifetime })
                        b:set(C.Homing, { turn = bc.turn_rate })
                        b:get(Render).variant = 4 -- blood bolt: dark-red tear w/ trail
                    end
                    bc.timer = bc.cooldown
                    bc.anim = 0.5
                    e:get(Render).fx = 1
                end
            elseif bc.anim > 0 then
                bc.anim = bc.anim - dt
                if bc.anim <= 0 then e:get(Render).fx = 0 end
            end
        end
    end)

    -- Homing steer: bend each homing bullet's velocity toward the nearest
    -- live player, capped at `turn` rad/s — outrun it or break its arc with
    -- a dash; never ignore it. 20 Hz over a handful of bolts.
    mod:system("homing_sys", { phase = "motion", rate = 20, stagger = 0.6 }, function(dt)
        for b in world:each(C.Homing, Velocity, Position) do
            local bp = b:get(Position)
            local _, d2, px, py = nearest_player(bp.x, bp.y)
            if d2 < math.huge then
                local v = b:get(Velocity)
                local speed = math.sqrt(v.dx * v.dx + v.dy * v.dy)
                if speed > 1 then
                    local want = math.atan(py - bp.y, px - bp.x)
                    local cur = math.atan(v.dy, v.dx)
                    local diff = want - cur
                    while diff > math.pi do diff = diff - 2 * math.pi end
                    while diff < -math.pi do diff = diff + 2 * math.pi end
                    local max_turn = b:get(C.Homing).turn * dt
                    diff = math.max(-max_turn, math.min(max_turn, diff))
                    local na = cur + diff
                    v.dx, v.dy = math.cos(na) * speed, math.sin(na) * speed
                end
            end
        end
    end)

    -- Bullet sprinkler (Elder Ent): rotating STREAMS — one bullet per arm per
    -- emission tick, arms sweeping continuously. You orbit through the gaps;
    -- they chase. ~12 Hz x 3 arms = 36 bullets/s, ~85 alive at 2.4 s life.
    mod:system("sprinkler_sys", { phase = "update", rate = 12, stagger = 0.35 }, function(dt)
        for e in world:each(C.Sprinkler, Position, Enemy) do
            local s = e:get(C.Sprinkler)
            s.angle = s.angle + s.angular_vel * dt
            local ep = e:get(Position)
            local n = math.floor(s.arms)
            for i = 1, n do
                local a = s.angle + (i / n) * 2 * math.pi
                local b = spawn_bullet(ep.x, ep.y, math.cos(a) * s.bullet_speed,
                    math.sin(a) * s.bullet_speed)
                b:set(C.Bullet, { damage = s.damage, hostile = 1, lifetime = s.lifetime })
                b:get(Render).variant = 5 -- sickly-green sprinkler pellet
            end
        end
    end)

    -- Blooming seeds (Elder Ent): lob a fat slow seed at the nearest player;
    -- mid-flight it POPS into a ring of slow petals (seed_bloom below).
    mod:system("seed_sys", { phase = "update", rate = 10, stagger = 0.7 }, function(dt)
        for e in world:each(C.SeedLauncher, Position, Enemy) do
            local sl = e:get(C.SeedLauncher)
            sl.timer = sl.timer - dt
            if sl.timer <= 0 then
                local ep = e:get(Position)
                local _, d2, px, py = nearest_player(ep.x, ep.y)
                if d2 < math.huge then
                    local len = math.max(1, math.sqrt(d2))
                    local base = math.atan(py - ep.y, px - ep.x)
                    local n = math.floor(sl.volley) -- > 1: fanned spread (brain phase 2)
                    for i = 1, n do
                        local a = base + (n > 1 and 0.55 * ((i - 1) / (n - 1) - 0.5) or 0)
                        local b = spawn_bullet(ep.x, ep.y, math.cos(a) * sl.bullet_speed,
                            math.sin(a) * sl.bullet_speed)
                        b:set(C.Bullet, {
                            damage = sl.damage,
                            hostile = 1,
                            lifetime = sl.bloom_after + 2.0
                        })
                        b:set(C.Seed, {
                            bloom = sl.bloom_after,
                            petals = sl.petals,
                            petal_speed = sl.petal_speed,
                            damage = sl.damage
                        })
                        b:get(Render).variant = 6 -- fat pulsing seed: "that one will pop"
                    end
                    sl.timer = sl.cooldown
                    sl.anim = 0.5
                    e:get(Render).fx = 1
                end
            elseif sl.anim > 0 then
                sl.anim = sl.anim - dt
                if sl.anim <= 0 then e:get(Render).fx = 0 end
            end
        end
    end)

    mod:system("seed_bloom", { phase = "update", rate = 20, stagger = 0.9 }, function(dt)
        for b in world:each(C.Seed, Position) do
            local seed = b:get(C.Seed)
            seed.bloom = seed.bloom - dt
            if seed.bloom <= 0 then
                local bp = b:get(Position)
                local n = math.floor(seed.petals)
                for i = 1, n do
                    local a = (i / n) * 2 * math.pi
                    local petal = spawn_bullet(bp.x, bp.y, math.cos(a) * seed.petal_speed,
                        math.sin(a) * seed.petal_speed)
                    petal:set(C.Bullet, { damage = seed.damage, hostile = 1, lifetime = 1.6 })
                    petal:get(Render).variant = 5 -- petals match the sprinkler green
                end
                b:destroy()
            end
        end
    end)

    -- Rotating cross barrage (Game Master): tight lances in `lanes`
    -- directions at staggered speeds, the whole cross rotating each volley —
    -- lanes, not rings: stand BETWEEN them, and don't camp (they rotate).
    mod:system("barrage_sys", { phase = "update", rate = 10, stagger = 0.4 }, function(dt)
        for e in world:each(C.Barrage, Position, Enemy) do
            local bar = e:get(C.Barrage)
            bar.timer = bar.timer - dt
            if bar.timer <= 0 then
                local ep = e:get(Position)
                local lanes = math.floor(bar.lanes)
                local per = math.floor(bar.per_lane)
                for i = 1, lanes do
                    local a = bar.angle + (i / lanes) * 2 * math.pi
                    local ca, sa = math.cos(a), math.sin(a)
                    for k = 1, per do
                        local mult = 0.85 + 0.15 * (k - 1) -- lance: staggered speeds
                        local b = spawn_bullet(ep.x, ep.y, ca * bar.bullet_speed * mult,
                            sa * bar.bullet_speed * mult)
                        b:set(C.Bullet, { damage = bar.damage, hostile = 1, lifetime = 2.6 })
                        b:get(Render).variant = 7 -- white-violet lance shard
                    end
                end
                bar.angle = bar.angle + bar.rotate
                bar.timer = bar.cooldown
                bar.anim = 0.7
                e:get(Render).fx = 1
            elseif bar.anim > 0 then
                bar.anim = bar.anim - dt
                if bar.anim <= 0 then e:get(Render).fx = 0 end
            end
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
                    if e:has(C.Arena) and e:get(C.Arena).w > 0 then
                        local arena = e:get(C.Arena)
                        x = arena.cx + (math.random() * 2 - 1) * (arena.w - 80)
                        y = arena.cy + (math.random() * 2 - 1) * (arena.h - 80)
                    else
                        local a = math.random() * 2 * math.pi
                        local r = 150 + math.random() * 300
                        x, y = ep.x + math.cos(a) * r, ep.y + math.sin(a) * r
                    end
                    local hazard = spawn_enemy(x, y, kind)
                    if hazard then hazard:set(C.NoLoot, {}) end -- planted, not earned
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
    -- The burn scales with Weapon.damage — a flat 25/s was irrelevant against
    -- wave-30 health pools; now the aura grows with the build. Kills count on
    -- the scoreboard (and feed Hunter's Instinct) like the blades'.
    mod:system("aura_sys", { phase = "update", rate = 20, stagger = 0.5 }, function(dt)
        for p in world:each(C.Aura, Position, Player) do
            local a = p:get(C.Aura)
            local pp = p:get(Position)
            local w = p:get(C.Weapon)
            local dps = a.per_second + 0.6 * (w and w.damage or 0)
            for e in world:nearby(pp.x, pp.y, a.radius, Enemy, Health) do
                local h = e:get(Health)
                local was_alive = h.current > 0
                h.current = h.current - dps * dt
                if was_alive then credit(p, dps * dt, h.current <= 0) end
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
