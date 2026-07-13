-- mods/core/brains.lua
--
-- Boss BRAINS: the fight director. Each milestone boss carries C.Brain{id};
-- its discrete mechanic components are PARKED in the def (cooldown/timer 9999)
-- and the move tables here decide WHEN they fire — weighted-random picks that
-- never repeat back-to-back, jittered cooldowns, health-gated phases, and
-- repositioning moves. The mechanic systems in systems.lua keep owning HOW
-- each attack works (telegraphs, anim, bullets); a brain move usually just
-- zeroes a parked timer ("poke"). Continuous mechanics (sprinkler, regen,
-- blink-evasion) stay ambient and are never parked.
--
-- Move fields: { name, windup (s of fx=3 telegraph, Speed parked; 0 for
-- delegated moves — the mechanic telegraphs itself), cooldown (base, jittered
-- x0.75..1.25 and scaled by phase aggression), weight (number or fn(boss) ->
-- number; 0 = ineligible right now), phase (min brain phase), fx (Render.fx
-- to flash on execute, for inline strikes), fn(boss, brain), duration +
-- during(boss, brain, dt) + finish(boss, brain) for channeled moves }.
return function(mod, C)
    local BRAIN = {
        RHINO = 1,
        FROG_KING = 2,
        FROG_PRINCE = 3,
        BOMB_LORD = 4,
        VAMPIRE = 5,
        ENT = 6,
        GAMEMASTER = 7,
    }

    ------------------------------------------------------------------ helpers
    local function nearest_player(x, y)
        return world:closest(x, y, Player, Position, { without = Downed })
    end

    local function live_players()
        local out = {}
        for p in world:each(Player, Position) do
            if not p:has(Downed) then out[#out + 1] = p end
        end
        return out
    end

    local function random_player()
        local players = live_players()
        if #players == 0 then return nil end
        return players[math.random(#players)]
    end

    -- Fire a parked mechanic: zero its timer, its own system takes over on
    -- its next tick (<= 100 ms) with its own telegraph/anim handling.
    local function poke(boss, comp)
        local m = boss:get(comp)
        if m then m.timer = 0 end
    end

    local function hostile(b, damage, lifetime, variant)
        b:set(C.Bullet, { damage = damage, hostile = 1, lifetime = lifetime })
        b:get(Render).variant = variant
    end

    -- Aimed fan of `n` bullets, `spread` radians wide, at the nearest player.
    local function cone_at(boss, n, spread, speed, damage, variant, lifetime)
        local ep = boss:get(Position)
        local _, d2, px, py = nearest_player(ep.x, ep.y)
        if not d2 then return end
        local base = math.atan(py - ep.y, px - ep.x)
        for i = 1, n do
            local a = base + (n > 1 and spread * ((i - 1) / (n - 1) - 0.5) or 0)
            local b = spawn_bullet(ep.x, ep.y, math.cos(a) * speed, math.sin(a) * speed)
            hostile(b, damage, lifetime, variant)
        end
    end

    -- Short radial shock — deliberately short-LIVED (a stomp around the boss,
    -- never an arena-crossing ring: rings stay the Frog King's).
    local function shock(boss, n, speed, damage, variant, lifetime)
        local ep = boss:get(Position)
        for i = 1, n do
            local a = (i / n) * 2 * math.pi
            local b = spawn_bullet(ep.x, ep.y, math.cos(a) * speed, math.sin(a) * speed)
            hostile(b, damage, lifetime, variant)
        end
    end

    -- A lit powder keg (core:mine with the fuse pre-set — bomber_arm skips
    -- already-fused kegs). `speed` > 0 makes it a ROLLER: targeting drives it
    -- at the nearest player until it pops.
    local function keg(x, y, fuse, speed)
        local k = spawn_enemy(x, y, "core:mine")
        if not k then return nil end
        k:set(C.Fuse, { timer = fuse })
        k:set(C.NoLoot, {}) -- boss hazards pay nothing
        if speed and speed > 0 then
            local s = k:get(Speed)
            if s then s.value = speed end
        end
        return k
    end

    -------------------------------------------------------------- move tables
    local BRAINS = {}

    -- @5 Rhino Charger: cross-field charges + a close-range stomp; at half
    -- health the tells shorten and everything comes faster.
    BRAINS[BRAIN.RHINO] = {
        phases = { 0.5 },
        on_phase = function(boss)
            local l = boss:get(C.Lunge)
            if l then l.windup = 0.55 end -- rage: half the warning
        end,
        moves = {
            { name = "charge", windup = 0, cooldown = 2.2, weight = 3,
              fn = function(boss) poke(boss, C.Lunge) end },
            { name = "stomp", windup = 0.5, cooldown = 1.8, fx = 1,
              weight = function(boss) -- only worth it with someone in reach
                  local ep = boss:get(Position)
                  local _, d2 = nearest_player(ep.x, ep.y)
                  return (d2 and d2 < 230 * 230) and 4 or 0
              end,
              fn = function(boss) shock(boss, 10, 340, 1, 1, 0.32) end },
        },
    }

    -- @10 Frog King: his rings (C.Nova keeps its own rage densification) now
    -- come from CHANGING spots — he hops around the arena — plus an aimed
    -- spit cone once he's hurt.
    BRAINS[BRAIN.FROG_KING] = {
        phases = { 0.5, 0.25 },
        moves = {
            { name = "rings", windup = 0, cooldown = 2.0, weight = 3,
              fn = function(boss) poke(boss, C.Nova) end },
            { name = "hop", windup = 0.45, cooldown = 2.8, duration = 0.5,
              fn = function(boss, brain)
                  local arena = boss:get(C.Arena)
                  local ep = boss:get(Position)
                  local tx = arena.cx + (math.random() * 2 - 1) * arena.w * 0.6
                  local ty = arena.cy + (math.random() * 2 - 1) * arena.h * 0.6
                  brain.c1, brain.c2 = (tx - ep.x) / 0.5, (ty - ep.y) / 0.5 -- px/s
              end,
              during = function(boss, brain, dt)
                  local ep = boss:get(Position)
                  ep.x = ep.x + brain.c1 * dt
                  ep.y = ep.y + brain.c2 * dt
              end },
            { name = "spit", windup = 0.5, cooldown = 2.4, phase = 2, fx = 1,
              fn = function(boss) cone_at(boss, 5, 0.7, 260, 1, 8, 1.8) end },
        },
    }

    -- @15 Frog Prince: leap-slams, a short spit arc (frog family, HIS small
    -- pattern), and below 60% a chained DOUBLE leap you can't camp through.
    BRAINS[BRAIN.FROG_PRINCE] = {
        phases = { 0.6, 0.4 },
        on_phase = function(boss, phase)
            if phase >= 3 then
                local l = boss:get(C.Lunge)
                if l then l.burst = l.burst + 4 end -- bigger landing ring
            end
        end,
        moves = {
            { name = "leap", windup = 0, cooldown = 2.4, weight = 3,
              fn = function(boss) poke(boss, C.Lunge) end },
            { name = "spit_arc", windup = 0.45, cooldown = 2.2, fx = 1,
              fn = function(boss) cone_at(boss, 3, 0.5, 240, 1, 8, 1.6) end },
            { name = "double_leap", windup = 0, cooldown = 4.5, phase = 2, duration = 3.0,
              fn = function(boss) -- unpark the lunge: it chains on its own...
                  local l = boss:get(C.Lunge)
                  l.cooldown = 0.55
                  l.timer = 0
              end,
              finish = function(boss) -- ...until the channel re-parks it
                  local l = boss:get(C.Lunge)
                  l.cooldown = 9999
                  l.timer = 9999
              end },
        },
    }

    -- @20 Bomb Lord: keg tosses, carpet BATCHES, homing roller kegs, and — once
    -- hurt — a keg CAGE around a player (the gap timing is the way out). At
    -- 30% every new fuse burns faster.
    BRAINS[BRAIN.BOMB_LORD] = {
        phases = { 0.6, 0.3 },
        on_phase = function(boss, phase)
            if phase >= 3 then
                local t = boss:get(C.Toss)
                if t then t.fuse = t.fuse * 0.65 end
            end
        end,
        moves = {
            { name = "toss", windup = 0, cooldown = 2.2, weight = 2,
              fn = function(boss) poke(boss, C.Toss) end },
            { name = "carpet", windup = 0, cooldown = 3.2,
              fn = function(boss)
                  local pl = boss:get(C.Planter)
                  pl.count = 6 -- one big batch, not a drip
                  pl.timer = 0
              end },
            { name = "roller", windup = 0.4, cooldown = 2.4, fx = 1,
              fn = function(boss, brain)
                  local ep = boss:get(Position)
                  keg(ep.x, ep.y, brain.phase >= 3 and 1.3 or 1.9, 250)
              end },
            { name = "cage", windup = 0.7, cooldown = 5.0, phase = 2, fx = 1,
              fn = function(boss, brain)
                  local p = random_player()
                  if not p then return end
                  local pp = p:get(Position)
                  for i = 1, 8 do
                      local a = (i / 8) * 2 * math.pi
                      keg(pp.x + math.cos(a) * 180, pp.y + math.sin(a) * 180,
                          brain.phase >= 3 and 1.6 or 2.2)
                  end
              end },
        },
    }

    -- @30 Vampire Lord: bolt fans and bat swarms on HIS schedule now, plus a
    -- blink-strike below half health — he appears beside a random player
    -- already swinging. Regen + flee-blink stay ambient.
    BRAINS[BRAIN.VAMPIRE] = {
        phases = { 0.5 },
        on_phase = function(boss)
            local bc = boss:get(C.BoltCaster)
            if bc then bc.bolts = bc.bolts + 2 end
        end,
        moves = {
            { name = "bolts", windup = 0, cooldown = 1.7, weight = 2,
              fn = function(boss) poke(boss, C.BoltCaster) end },
            { name = "bats", windup = 0, cooldown = 5.5,
              fn = function(boss) poke(boss, C.Summon) end },
            { name = "blink_strike", windup = 0.55, cooldown = 2.6, phase = 2, fx = 1,
              fn = function(boss)
                  local p = random_player()
                  if not p then return end
                  local pp = p:get(Position)
                  local a = math.random() * 2 * math.pi
                  local ep = boss:get(Position)
                  ep.x, ep.y = pp.x + math.cos(a) * 140, pp.y + math.sin(a) * 140
                  cone_at(boss, 3, 0.6, 300, 1, 4, 1.2) -- straight (no homing): dash through
              end },
        },
    }

    -- @40 Elder Ent: the sprinkler stays ambient geometry, but it REVERSES
    -- once he's hurt (orbit rhythm broken), seeds come as fans, and roots
    -- (brambles) sprout under every player's feet.
    BRAINS[BRAIN.ENT] = {
        phases = { 0.6, 0.3 },
        on_phase = function(boss, phase)
            local sl = boss:get(C.SeedLauncher)
            if sl and phase >= 2 then sl.volley = 3 end -- fanned seeds
            local s = boss:get(C.Sprinkler)
            if s and phase >= 3 then s.arms = s.arms + 1 end
        end,
        moves = {
            { name = "seeds", windup = 0, cooldown = 2.0, weight = 2,
              fn = function(boss) poke(boss, C.SeedLauncher) end },
            { name = "roots", windup = 0.6, cooldown = 2.8, fx = 1,
              fn = function()
                  for _, p in ipairs(live_players()) do
                      local pp = p:get(Position)
                      local root = spawn_enemy(pp.x, pp.y, "core:bramble") -- arm+fuse = dodge window
                      if root then root:set(C.NoLoot, {}) end
                  end
              end },
            { name = "reverse", windup = 0, cooldown = 3.0, phase = 2,
              fn = function(boss)
                  local s = boss:get(C.Sprinkler)
                  s.angular_vel = -s.angular_vel
                  s.angle = s.angle + math.random() * math.pi -- and jump the arms
              end },
        },
    }

    -- @50 GAME MASTER: five moves — cross barrages, elite summons, a channeled
    -- SPIRAL, simultaneous lance rain at every player, and below 35% the
    -- CHECKMATE WALL: he takes the arena center and sweeps a bullet wall with
    -- one 2-slot gap across half the board. Blink-evasion stays ambient.
    BRAINS[BRAIN.GAMEMASTER] = {
        phases = { 0.65, 0.35 },
        moves = {
            { name = "cross", windup = 0, cooldown = 1.5, weight = 2,
              fn = function(boss) poke(boss, C.Barrage) end },
            { name = "elites", windup = 0, cooldown = 7.0,
              fn = function(boss) poke(boss, C.Summon) end },
            { name = "spiral", windup = 0.6, cooldown = 2.4, duration = 1.2,
              fn = function(_, brain)
                  brain.c1 = math.random() * 2 * math.pi -- arm seed
                  brain.c2 = 0                           -- emission accumulator
              end,
              during = function(boss, brain, dt)
                  brain.c2 = brain.c2 + dt
                  while brain.c2 > 0.05 do -- 20 bullets/s along walking angles
                      brain.c2 = brain.c2 - 0.05
                      local ep = boss:get(Position)
                      local b = spawn_bullet(ep.x, ep.y, math.cos(brain.c1) * 210,
                          math.sin(brain.c1) * 210)
                      hostile(b, 1, 2.4, 7)
                      brain.c1 = brain.c1 + 2.4 -- golden-ish step: spiral ARMS, not rings
                  end
              end },
            { name = "lance_rain", windup = 0.55, cooldown = 1.9, phase = 2, fx = 1,
              fn = function(boss)
                  local ep = boss:get(Position)
                  for _, p in ipairs(live_players()) do -- every player, at once
                      local pp = p:get(Position)
                      local dx, dy = pp.x - ep.x, pp.y - ep.y
                      local len = math.max(1, math.sqrt(dx * dx + dy * dy))
                      for k = 1, 3 do
                          local mult = 0.85 + 0.15 * (k - 1) -- the lance feel
                          local b = spawn_bullet(ep.x, ep.y, dx / len * 320 * mult,
                              dy / len * 320 * mult)
                          hostile(b, 1, 2.2, 7)
                      end
                  end
              end },
            { name = "checkmate", windup = 0.9, cooldown = 4.5, phase = 3, fx = 1,
              fn = function(boss)
                  local arena = boss:get(C.Arena)
                  local ep = boss:get(Position)
                  ep.x, ep.y = arena.cx, arena.cy -- take the center: the wall is fair
                  local dir = math.random() < 0.5 and 1 or -1
                  local spacing = 44
                  local slots = math.floor((arena.h * 2) / spacing)
                  local gap = math.random(2, slots - 3) -- the 2-slot way through
                  for s = 0, slots do
                      if s ~= gap and s ~= gap + 1 then
                          local b = spawn_bullet(arena.cx, arena.cy - arena.h + s * spacing,
                              dir * 240, 0)
                          hostile(b, 1, arena.w / 240, 7)
                      end
                  end
              end },
        },
    }

    ---------------------------------------------------------------- the brain
    local AGGRO = { 1.0, 0.85, 0.7 } -- phase -> cooldown scale

    local function jittered(base, phase)
        return base * (0.75 + math.random() * 0.5) * (AGGRO[phase] or 0.7)
    end

    -- Weighted pick among phase-eligible moves, excluding the one just used
    -- (unless nothing else is eligible — then repeating beats standing idle).
    local function pick_move(def, brain, boss, allow_repeat)
        local total, pool = 0, {}
        for i, m in ipairs(def.moves) do
            if (m.phase or 1) <= brain.phase and (allow_repeat or i ~= math.floor(brain.move)) then
                local w = m.weight or 1
                if type(w) == "function" then w = w(boss) end
                if w > 0 then
                    pool[#pool + 1] = { i = i, m = m, w = w }
                    total = total + w
                end
            end
        end
        if total <= 0 then
            if allow_repeat then return nil end
            return pick_move(def, brain, boss, true)
        end
        local r = math.random() * total
        for _, e in ipairs(pool) do
            r = r - e.w
            if r <= 0 then return e.i, e.m end
        end
        return pool[#pool].i, pool[#pool].m
    end

    local function park_speed(boss, brain)
        if brain.saved_speed < 0 then
            local s = boss:get(Speed)
            if s then
                brain.saved_speed = s.value
                s.value = 0
            end
        end
    end
    local function unpark_speed(boss, brain)
        if brain.saved_speed >= 0 then
            local s = boss:get(Speed)
            if s then s.value = brain.saved_speed end
            brain.saved_speed = -1
        end
    end

    mod:system("brain_sys", { phase = "update", rate = 20, stagger = 0.55 }, function(dt)
        for boss in world:each(C.Brain, Position, Health, Enemy) do
            local brain = boss:get(C.Brain)
            local def = BRAINS[math.floor(brain.id)]
            if def then
                -- Health-gated phases: one-way escalation, on_phase fires once.
                local h = boss:get(Health)
                local frac = h.current / math.max(1, h.max)
                local want = 1
                for k, thr in ipairs(def.phases or {}) do
                    if frac < thr then want = k + 1 end
                end
                if want > brain.phase then
                    brain.phase = want
                    if def.on_phase then def.on_phase(boss, want) end
                end

                local m = def.moves[math.floor(brain.move)]
                if brain.channel > 0 then
                    brain.channel = brain.channel - dt
                    if m and m.during then m.during(boss, brain, dt) end
                    if brain.channel <= 0 then
                        if m and m.finish then m.finish(boss, brain) end
                        unpark_speed(boss, brain)
                        boss:get(Render).fx = 0
                        brain.timer = jittered(m and m.cooldown or 2.0, brain.phase)
                    end
                elseif brain.winding > 0 then
                    brain.winding = brain.winding - dt
                    if brain.winding <= 0 and m then
                        m.fn(boss, brain)
                        brain.used_mask = math.floor(brain.used_mask) | (1 << math.floor(brain.move))
                        if m.fx then
                            boss:get(Render).fx = m.fx
                            brain.anim = 0.5
                        end
                        if (m.duration or 0) > 0 then
                            brain.channel = m.duration
                            -- Inline channels show the charge loop; delegated
                            -- ones (windup 0) let their mechanic drive fx.
                            if (m.windup or 0) > 0 and not m.fx then boss:get(Render).fx = 2 end
                        else
                            unpark_speed(boss, brain)
                            brain.timer = jittered(m.cooldown, brain.phase)
                        end
                    end
                else
                    if brain.anim > 0 then -- inline-strike fx window
                        brain.anim = brain.anim - dt
                        if brain.anim <= 0 then boss:get(Render).fx = 0 end
                    end
                    brain.timer = brain.timer - dt
                    if brain.timer <= 0 then
                        local i, mv = pick_move(def, brain, boss, false)
                        if i then
                            brain.move = i
                            brain.winding = math.max(mv.windup or 0, 0.01)
                            if (mv.windup or 0) > 0 then
                                park_speed(boss, brain)
                                boss:get(Render).fx = 3
                            end
                        else
                            brain.timer = 0.4 -- nothing eligible: re-check shortly
                        end
                    end
                end
            end
        end
    end)

    return BRAIN
end
