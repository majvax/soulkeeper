# Soulkeeper Modding Guide

Soulkeeper's split is radical: **C++ is the engine, Lua is the game.** The kernel does the heavy
lifting — ECS storage, movement integration, the spatial hash, dash prediction, netcode,
snapshots, rendering — and **every game rule is a plugin**: weapons, bullets, contact damage,
i-frames, deaths, drops, pickups, enemy AI, upgrades, objects, enemy archetypes. `mods/core/` *is*
Soulkeeper's gameplay; your plugin sits right next to it with the same powers.

For editor autocomplete, point your Lua LSP's library path at `types/` (see `types/library.lua`).

---

## 1. Mental model

Soulkeeper is **server-authoritative** and runs as **two processes**. A plugin is loaded into two
Lua VMs:

| VM | Runs in | Sees | Used for |
|----|---------|------|----------|
| **sim VM** | `server` | the ECS world | systems, events, apply/acquire callbacks |
| **render VM** | `client` | the on-screen frame | `draw` hooks + card metadata |

**The same `mod.lua` runs in both.** You declare content once; each side only fires its own
callbacks.

### The kernel contract (what stays C++)
Kernel components — available as **prelude globals**: `Position{x,y}`, `Velocity{dx,dy}`,
`Speed{value}`, `Health{current,max}` (float HP, enemies), `Hearts{current,max}` (player heart
life), `Radius{value}`, `AimState{dx,dy,firing}`, `Dash{cooldown_max, cooldown, burst_remaining,
shockwave, charges, max_charges}`, `XpReward{value}`, `Render{kind,variant}` (how the client draws
an entity), `Downed{respawn_wave}`, plus the `Enemy` / `Player` tags. Kernel systems: spatial-hash
rebuild, dash (client-predicted), movement integration. Everything else is Lua.

### Plugins, exports, import()
A plugin is a folder `mods/<name>/mod.lua` defining a global `main()`. **The folder name is the
plugin's namespace and import name.** `main()`'s return value is the plugin's **exports**:

```lua
-- mods/mymod/mod.lua
function main()
    local core = import("core")                    -- another plugin's exports (lazy, memoized)
    local mod = register_mod("mymod", "My stuff", "you")
    local Burn = mod:component("burn", { per_second = 5, remaining = 3 })
    -- ... systems/content using Burn and core.* handles ...
    return { Burn = Burn }                          -- your exports
end
```

`import()` loads dependencies on demand (cycles error cleanly), so a **component-library plugin**
just works: whoever imports it first triggers its load. Path order decides nothing.

### Ids and the join check
Every registered name is auto-prefixed with the namespace (`"burn"` → `"mymod:burn"`). Wire ids
are the lexicographic sort index of those ids — identical on every process. The client sends a
hash of its registered content in `Join`; the server refuses a mismatched `mods/` set and the
lobby shows both hashes. Lua *code* isn't hashed — only the registered identity.

---

## 2. Components

`mod:component(name, fields, opts)` declares fields **with defaults** and returns **the handle** —
the only way to name a component anywhere (no string ids at use sites):

```lua
local Ranged = mod:component("ranged", {
    range = 340, cooldown = 1.6, timer = 0,   -- field = default
}, { networked = false })                      -- networked: synced to clients for draw hooks
```

- `e:set(Ranged, { range = 400 })` — add/replace; unset fields take their defaults.
- `e:get(Ranged).timer = 1` — write-through proxy; **unknown field names raise an error** (typos
  are loud, not silent nils).
- `e:has(Ranged)`, `e:remove(Ranged)`, `e:destroy()`.
- Kernel components use the same verbs with the prelude handles: `e:get(Health)`, `e:set(Downed,
  { respawn_wave = 7 })`.

Fields are numbers (stored as doubles in flat pools). Field order is the sorted field-name order —
deterministic for the networked wire layout.

## 3. Systems — the gameplay pipeline

`mod:system(name, opts, fn)` registers `fn(dt)` at a pipeline phase (sim VM only):

```
grid (kernel) → targeting → motion (kernel dash + yours) → shooting → movement (kernel)
             → projectile → combat → update (default) → pickup → death
```

```lua
mod:system("burn_tick", { phase = "update", rate = 20 }, function(dt)  -- rate: throttle in Hz
    for e in world:each(Burn, Health) do
        local b, h = e:get(Burn), e:get(Health)
        h.current = h.current - b.per_second * dt
        b.remaining = b.remaining - dt
        if b.remaining <= 0 then e:remove(Burn) end
    end
end)
```

### World services
- `world:each(H, ...)` — all entities owning every component.
- `world:nearby(x, y, radius, H, ...)` — entities within `radius` px (center distance), served by
  the kernel spatial hash. **The broad-phase workhorse** — this is how core does bullet hits,
  contact damage, auras and pickups. Add collision radii into `radius` yourself.
- `world:closest(x, y, H, ..., { without = H })` — nearest entity owning every component; returns
  `entity, d², px, py` (all nil on no match). `without` excludes owners of a component (e.g.
  `Downed` players). **Use this, never a Lua loop, to pick a target inside a per-entity system** —
  it's one engine call instead of an iterator per entity.
- `world:wave()`, `world:add_xp(n)`.
- `spawn_bullet(x, y, vx, vy)` — kinetic drawable (bullet kind); attach your bullet component for
  behavior, set `Render.variant` for tint (1 = hostile red, 2 = crit orange in the core skin).
- `spawn_entity(x, y)` — bare drawable for drops/markers: set `Render.kind` (see the `KIND`
  table: `KIND.orb`, `KIND.heart`, ...) and attach components.
- `spawn_enemy(x, y, "core:brute")` — spawn a registered archetype (applies its component bag at
  the current wave + fires `on_spawn`).

Errors anywhere are contained: every callback is a protected call; a broken system logs and skips,
never crashes the server.

## 4. Content

### Stat upgrade
`amounts` = per tier `{ common, uncommon, rare, epic, legendary }`; **missing/0 = not offered at
that tier** (`{ 5, 10, 15 }` exists only at C/U/R, `{ 0, 0, 0, 1 }` only at Epic).

```lua
mod:upgrade("damage", "Sharp Rounds", { 3, 5, 8, 12, 20 },
    function(e, rarity, amount)
        local w = e:get(core.Weapon)
        if w then w.damage = w.damage + amount end
    end,
    { value_format = "+{} DMG", sprite = "assets/icons/dmg.png" })
```

### Object
Acquired **once** (engine-tracked), rolls at its declared tier (`rarity = "epic"` default; the
roll picks the tier first, so legendary objects are genuinely rare):

```lua
mod:object("onion", "Onion",
    function(e) e:set(C.Aura, { radius = 120 }) end,
    {
        rarity = "epic",
        value_text = function() return "damage aura" end,
        draw = function(ctx, view)              -- render VM, per frame
            local a = view:get(C.Aura)          -- networked component fields
            if a then ctx:circle(view.x, view.y, a.radius, 120, 180, 255, 120, 2) end
        end,
    })
```

### Enemy
An archetype = identity + visuals + spawn weight, then a **component bag** — kernel and Lua
components alike. Function inits re-resolve **once per wave** (the wave-scaling mechanism):

```lua
mod:enemy("slinger", "Slinger", {
        weight = function(wave) return math.max(0, wave - 2) * 1.2 end,
        scale = 0.9, tint = { 150, 255, 140 },
    })
    :component(Health, function(wave) local h = 15 * (1 + 0.15 * (wave - 1))
                                      return { current = h, max = h } end)
    :component(Speed, { value = 100 })
    :component(C.Touch, { hearts = 1 })     -- contact damage (hearts per hit)
    :component(Radius, { value = 9 })
    :component(XpReward, { value = 2 })
    :component(C.Ranged, {})                -- all defaults
```

Every enemy needs at least `Health` and `Radius` (the server warns otherwise). `on_spawn` in opts
remains the escape hatch for dynamic per-spawn logic.

### Visuals: animation packs (zero animation code)
`sprite` (enemies) and `mod:player_sprite(path)` accept an **animation-pack folder** — a directory
of horizontal strip PNGs named `<Clip>_<N>x1.png` (N frames; a character-name prefix like
`Goblin_Regular_01_Move_10x1.png` is fine). Declare the folder and you're done — **the engine does
everything else in C++**: it discovers the clips, slices frames from the filename, plays `Move`
while the entity moves and `Idle` while it stands, mirrors the sprite when heading left (packs
face right), staggers animation phases so a wave doesn't move in lockstep, and scales to `scale`
keeping pixel aspect. A plain `.png` path still works as a static sprite, and a missing pack falls
back gracefully (static texture → colored rect).

```lua
mod:player_sprite("assets/sprite/Knight_LVL1")
mod:enemy("brute", "Brute", { sprite = "assets/sprite/RhinoMonster_01_Regular", scale = 1.5 })
```

### The `opts` tables
| Key | Kind | VM | Meaning |
|-----|------|----|---------|
| `available(e) -> bool` | both | sim | queried when rolling a level-up; `false` hides it |
| `sprite` (string) | both | render | card sprite path |
| `value_format` (string) | upgrade | — | fmt-style on the amount, e.g. `"+{} DMG"` |
| `value_text(amount, rarity) -> string` | both | — | compute the card text yourself |
| `rarity` (string) | object | sim | `"common"`…`"legendary"` (default `"epic"`) |
| `weight` (number or `fun(wave)`) | enemy | sim | spawn weight, re-evaluated once per wave |
| `scale` / `tint` / `sprite` | enemy | render | visuals; `sprite` may be an animation-pack folder |
| `on_spawn(e)` | enemy | sim | dynamic per-spawn hook |

## 5. Events

`mod:subscribe(name, fn)` — engine events fire server-side:

| Event | Handler | Fires when |
|-------|---------|-----------|
| `on_player_spawn` | `fun(player: Entity)` | a NEW player entity is created — **attach the loadout here** |
| `on_wave_start` | `fun(wave: integer)` | a new wave begins |
| `on_enemy_death` | `fun(victim: {x, y, variant, xp})` | an enemy dies (snapshot table) |
| `on_player_downed` | `fun(player: Entity)` | a player drops to 0 hearts |
| `on_level_up` | `fun(level: integer)` | the team levels |

Custom cross-plugin events: `mod:emit("boss_spawned", x, y)` (auto-namespaced →
`"mymod:boss_spawned"`); others `mod:subscribe("mymod:boss_spawned", fn)`.

## 6. Render API (client VM)

An object's `draw(ctx, view)` runs per frame for each player: `view.x/.y` are screen-space,
`view:get(H)` reads a **networked** component's fields as a table.

```
ctx:texture(path, x, y, w, h)                  ctx:rect(x, y, w, h, r, g, b, a)
ctx:circle_filled(cx, cy, rad, r, g, b, a)     ctx:circle(cx, cy, rad, r, g, b, a, thickness)
ctx:text(x, y, str, r, g, b, a)                ctx:world_to_screen(wx, wy) -> sx, sy
```

## 7. Performance model

The sim ticks at 120 Hz with up to ~500 enemies — Lua 5.4 handles the core pipeline comfortably,
but the cost that matters is **Lua↔engine boundary crossings** (every `e:get`, every iterator),
not Lua itself. The discipline that keeps 500 enemies under ~1 ms/tick:

- **One query call beats a Lua loop.** `world:closest` picks a target in one crossing; never scan
  candidates from Lua inside a per-entity system (one nested `world:each` per enemy per tick was
  the single biggest cost in the whole game before it became `world:closest`).
- **Broad-phase first.** Use `world:nearby` (kernel spatial hash) for anything area-shaped; the
  core systems are the reference.
- **Throttle with `rate`** — most systems don't need 120 Hz (core runs targeting at 30, pickups
  at 20). The callback receives the *accumulated* dt, so per-second quantities are unchanged.
- **Spread with `stagger`** — same-rate systems all fire on the same tick by default, and that
  aligned tick can bust the frame budget by itself. Give heavy systems different `stagger`
  fractions (see `mods/core/systems.lua`); keep stagger equal only when one system must see
  another's same-tick writes (targeting → slow_sys).
- The server logs `tick avg/max ms + entities` every 5 s and warns on budget overruns — watch it
  while developing a mod.
- Event callbacks fire on discrete moments — keep per-tick work in systems.
- If a system still profiles hot, it can move back to C++ without changing the API for anyone
  else.

## 8. Reference: internals

- **Runtime:** Lua 5.4 via [sol2](https://github.com/ThePhD/sol2); generational GC.
  Registration API + sim bindings in `src/shared/mod/` (rendering-free); render bindings in
  `src/client/mod/`. The server links Lua but never SDL.
- **Handles:** `ComponentRef` wraps an engine dispatch-table index or a Lua schema pointer —
  one type for both worlds (`component_ref.hpp`, `bindings_table.hpp`).
- **Loader:** two passes — every `mod.lua` runs (defining `main`), then mains run lazily via
  `import()` with cycle detection (`lua_host.cpp`).
- **Wire:** level-up choices travel as `{wire id, rarity}`; snapshot entries carry
  `Render.kind/variant` + per-entity networked component blobs (`count + {net_id, floats}`),
  net-ids = sorted index among networked components.
