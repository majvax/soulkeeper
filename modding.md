# Soulkeeper Modding Guide

Soulkeeper content — **upgrades**, **objects**, and even new **components and systems** — is defined
by **Lua plugins**, not hardcoded in the engine. This guide explains how plugins work, the
performance model behind them, and the full Lua API. If you just want to ship content, jump to [Quick start](#8-quick-start) and copy
`mods/core/mod.lua`. For editor autocomplete, point your Lua LSP's library path at `types/` (see
`types/library.lua`).

---

## 1. Mental model

Soulkeeper is **server-authoritative** and runs as **two processes** (the host launches both a
`server` and a `client`). Because of that, and because a strict engine rule keeps simulation code
rendering-free, a plugin is loaded into **two separate Lua VMs**:

| VM | Runs in | Sees | Used for |
|----|---------|------|----------|
| **sim VM** | `server` process | the ECS world (components, entities) | `apply` / `acquire` / `available` + sim events |
| **render VM** | `client` process | the on-screen frame (draw context) | `draw` hooks |

**The same `mod.lua` runs in both.** You declare content once; the engine calls sim callbacks only on
the server and `draw` only on the client. A callback that doesn't apply to a side simply never fires.

### The entry point
Every plugin is a folder `mods/<name>/mod.lua` that defines a global **`main()`**. The loader runs
the file, then calls `main()`. Inside, you `register_mod(...)` and add content:

```lua
function main()
    local mod = register_mod("mymod", "My cool content", "you")
    -- mod:add_stat_upgrade(...) / mod:add_object(...) / mod:subscribe(...)
    return mod
end
```

### Deterministic ids
Every piece of content has a **namespaced string id** like `core:damage` or `mymod:flamethrower`.
The namespace (the first arg to `register_mod`) prevents collisions between mods. Internally these
map to a small integer that travels over the network; **the mapping is the lexicographic sort order
of all registered ids**, so the server and client always agree, regardless of load order. You never
touch the integer — just pick a unique `namespace:name`.

> The client and server **must load the same set of plugins**, and the engine enforces it: the
> client sends a deterministic hash of its registered content (upgrade/object ids + kinds, enemy
> ids, script component schemas) when joining, and the server refuses a mismatch. The lobby then
> shows both hashes so you can tell whose `mods/` folder is off. Lua *code* isn't hashed — only the
> registered identity that the wire format depends on.

---

## 2. Performance model — why plugins don't run every frame

The simulation ticks at 120 Hz. If every object ran Lua each tick, mods would tank the server.
Soulkeeper avoids this:

- **Content callbacks run on discrete events, not per frame.** `available` fires when rolling a
  level-up, `apply`/`acquire` when a player picks a card, `subscribe` handlers on game events. Rare.
- **Continuous effects are components ticked by a system.** The Onion doesn't hand-loop — it grants a
  `core:aura` component, and a `core:aura_sys` system damages enemies in range each tick. You can
  define both in Lua (see §6). Keep per-entity work tight; a system can declare a `rate` to run at a
  lower cadence (e.g. 20 Hz) instead of every tick.
- **Drawing is opt-in per-frame.** A `draw` callback runs every frame, but only for content that
  defines one, and only for the relevant entities.

**Rule of thumb:** model *continuous* effects as a component + system; reserve event callbacks for
*moments* (acquire, pick, events) and keep the hottest math small.

---

## 3. Registering content

### Stat upgrade
Repeatable, rarity-scaled. `amounts` has one value per tier —
`{ common, uncommon, rare, epic, legendary }` — and a **missing or 0 entry means the upgrade is
not offered at that tier**: `{ 5, 10, 15 }` exists only at Common/Uncommon/Rare, `{ 0, 0, 0, 1 }`
only at Epic.

```lua
mod:add_stat_upgrade("core:damage", "Sharp Rounds", { 3, 5, 8, 12, 20 },
    function(e, rarity, amount)          -- apply(entity, rarity, amount)
        local w = e:get(Weapon)
        if w then w.damage = w.damage + amount end
    end,
    { value_format = "+{} DMG" })         -- optional opts (see §4)
```

### Object
Acquired **once** (the engine tracks ownership — you don't write an `available` for that), no rarity
scaling. An object declares the single tier it rolls at (`rarity = "epic"` by default; the level-up
roll picks the tier first, so a legendary object is genuinely rare). Each object should carry its
**own component** (see §6) so multiple objects stay independent. Objects have **no amount**.

```lua
mod:add_object("core:onion", "Onion",
    function(e)                                   -- acquire(entity)
        e:set("core:aura", { radius = 120, per_second = 25 })
    end,
    {
        rarity = "epic",                          -- the tier this object rolls at
        value_text = function() return "damage aura" end,
        draw = function(ctx, view)               -- per-frame, client-side
            local a = view:get("core:aura")       -- networked component fields
            if not a or a.radius <= 0 then return end
            ctx:circle_filled(view.x, view.y, a.radius, 120, 180, 255, 30)
            ctx:circle(view.x, view.y, a.radius, 120, 180, 255, 120, 2)
        end,
    })
```

The `core:aura` component and the system that ticks it are defined in Lua too — see §6. The Frost
Belt is the same pattern with a `core:slow` component and a motion-phase system.

### Enemy
Enemy archetypes are plugins too. Stats drive the simulation, `weight` the wave spawner,
`scale`/`tint`/`sprite` the client. All of Soulkeeper's enemies live in `mods/core/enemies.lua`.

`stats` is a table **or a `fun(wave) -> table`** (per-wave scaling — evaluated once per wave,
like `weight`). `damage` is discrete **hearts per hit** (players have heart life with 1 s of
i-frames after any hit). `add_enemy` returns the archetype: chain **`:component(id, fields)`** to
attach script components to every spawned instance — declarative, and applied without any Lua on
the spawn path.

```lua
mod:add_enemy("core:slinger", "Slinger",
    function(wave)                      -- stats scale with the wave
        return { health = 15 * (1 + 0.15 * (wave - 1)), speed = 100,
                 damage = 1, radius = 9, xp = 2 }
    end,
    {
        weight = function(wave) return math.max(0, wave - 2) * 1.2 end,
        scale  = 0.9,
        tint   = { 150, 255, 140 },
    })
    :component("core:ranged", { range = 340, standoff = 260,
                                cooldown = 1.6, bullet_speed = 260, damage = 1, timer = 1.0 })
```

Enemy `opts` (all optional):

| Key | VM | Meaning |
|-----|----|---------|
| `weight` (number or `fun(wave) -> number`) | sim | relative spawn chance, **re-evaluated once per wave** (not per spawn); `0`/absent = never spawns naturally |
| `scale` (number) | render | sprite scale (default 1.0) |
| `tint` (`{r,g,b}`) | render | colour mod on the sprite (default white) |
| `sprite` (string) | render | own sprite path (default: the shared `assets/sprite/enemy.png`) |
| `on_spawn(e)` | sim | escape hatch for dynamic per-spawn logic (prefer `:component()` for static state) |

An enemy's wire id is what snapshots carry as `variant` (and what `on_enemy_death` reports); like
all content it's the lexicographic sort index, but within a **separate enemy id space**. Spawn one
from a script with `spawn_enemy(x, y, "core:scout")` — it fires `on_spawn` exactly like a natural
wave spawn.

---

## 4. The `opts` table

Both `add_stat_upgrade` and `add_object` take an optional trailing table:

| Key | Kind | VM | Meaning |
|-----|------|----|---------|
| `available(e) -> bool` | both | sim | queried when rolling a level-up; return `false` to hide |
| `sprite` (string) | both | render | card sprite path (relative to repo root) |
| `value_format` (string) | stat | — | **fmt-style** format applied to the amount, e.g. `"+{} DMG"` |
| `value_text(amount, rarity) -> string` | both | — | advanced: compute the card text yourself (wins over `value_format`) |
| `rarity` (string) | object | sim | the tier the object rolls at: `"common"`…`"legendary"` (default `"epic"`) |
| `draw(ctx, view)` | object | render | per-frame draw hook |

**Value text** is precomputed once at load (never per frame). `value_format` is formatted in **C++
with `fmt::format`** (`{}` placeholders). For anything a single format can't express — e.g. firerate
showing ms from a 0.03 ratio — use `value_text = function(amount) return "-"..(amount*1000).."ms" end`.

**Errors are contained.** Every callback is a protected call; if your Lua throws, the engine logs it
and keeps running — a broken mod can't crash the authoritative server or the client.

---

## 5. Events

Subscribe to game events with `mod:subscribe(name, handler)`. Events fire **server-side** (sim VM):

| Event | Handler | Fires when |
|-------|---------|-----------|
| `on_wave_start` | `fun(wave: integer)` | a new wave begins |
| `on_enemy_death` | `fun(victim: {x, y, variant, xp})` | an enemy dies |
| `on_player_downed` | `fun(player: Entity)` | a player drops to downed |
| `on_level_up` | `fun(level: integer)` | the team reaches a new level |

```lua
mod:subscribe("on_enemy_death", function(victim)
    print(("enemy died at %.0f,%.0f (xp %d)"):format(victim.x, victim.y, victim.xp))
end)
```

`on_enemy_death` passes a **snapshot table** (the entity is already gone); `on_player_downed` passes a
live `Entity` handle.

---

## 6. Components & systems (sim VM)

This is what makes the engine actually moddable: define **your own components and systems in Lua** —
no C++ edits.

### Entity handle
Sim callbacks and query results are entity handles. Engine components are addressed by their tag
global; **script components you defined** by their string id.

```lua
e:get(Weapon)             -- engine component (mutate fields in place), or nil
e:get("core:aura")        -- script component (proxy; mutate fields in place), or nil
e:has(Weapon) / e:has("core:aura")
e:assign(Weapon, {..})    -- add/replace an ENGINE component
e:set("core:aura", {..})  -- add/replace a SCRIPT component
e:remove(Weapon) / e:remove("core:aura")
```

Engine component globals & fields: `Position{x,y}`, `Velocity{dx,dy}`, `Speed{value}`,
`Health{current,max}` (enemies), `Hearts{current,max}` (player heart life),
`Radius{value}`, `Damage{per_second}` (for enemies: hearts per contact hit),
`Weapon{cooldown_max, cooldown_current, bullet_speed, damage, projectile_lifetime}`,
`AimState{dx,dy,firing}`, `Dash{cooldown_max, cooldown, shockwave, charges, max_charges}`,
`Crit{chance, multiplier}`. Tag globals for queries:
`Enemy`, `Player`. Spawn helpers: `spawn_projectile(x,y,vx,vy,damage,lifetime[,hostile])` (pass
`hostile = true` for enemy-fired bullets — they hit players instead of enemies),
`spawn_xp_orb(x,y,value)`, `spawn_enemy(x,y,id)` (`id` = a registered enemy like `"core:brute"`;
returns nil + logs on an unknown id). The Slinger in `mods/core/` is the worked example of a
ranged attacker: a sim-only `core:ranged` component, a motion-phase standoff system, and an
update-phase firing system — no C++.

### Defining a component
A component is a named list of **number** fields. Flag it `networked` to sync it to clients (so draw
hooks can read it via `view:get`).

```lua
mod:define_component("core:aura", { "radius", "per_second" }, { networked = true })
```

### Defining a system
`fn(dt)` runs each tick (or throttled with `rate`). `phase` places it in the pipeline: `"motion"`
(after enemy targeting, before movement — for velocity effects) or `"update"` (default, after combat —
for damage/logic). Query with `world:each(...)`, which yields entity handles owning **all** the given
components (tag globals or script ids):

```lua
mod:define_system("core:aura_sys", { phase = "update" }, function(dt)
    for p in world:each("core:aura", Position, Player) do
        local a, pp = p:get("core:aura"), p:get(Position)
        for e in world:each(Enemy, Position, Health) do
            local ep = e:get(Position)
            local dx, dy = ep.x - pp.x, ep.y - pp.y
            if dx*dx + dy*dy < a.radius*a.radius then
                local h = e:get(Health); h.current = h.current - a.per_second * dt
            end
        end
    end
end)
```

Systems run only on the server (the authoritative sim); clients render from snapshots. Handler errors
are caught and logged — a broken system won't kill the loop.

---

## 7. Render API (client VM)

An object's `draw(ctx, view)` receives a **draw context** and a **view** of the player being drawn.
`view.x, view.y` are screen-space; **`view:get("id")`** returns a networked script component's fields
as a table (or nil), so you draw modded state without any per-effect engine plumbing.

```lua
ctx:texture(path, x, y, w, h)                 -- cached PNG, centered
ctx:rect(x, y, w, h, r, g, b, a)              -- filled rectangle
ctx:circle_filled(cx, cy, radius, r,g,b,a)    -- filled disc
ctx:circle(cx, cy, radius, r,g,b,a, thickness)-- ring outline
ctx:text(x, y, str, r, g, b, a)               -- text label
ctx:world_to_screen(wx, wy) -> sx, sy         -- apply the camera transform
```

Missing sprite images fall back to a colored rectangle automatically.

---

## 8. Quick start

```
mods/mymod/
  mod.lua
  assets/thing.png
```

```lua
function main()
    local mod = register_mod("mymod", "Haste demo", "you")
    mod:add_stat_upgrade("mymod:haste", "Haste", { 10, 20, 30 },
        function(e, _rarity, amount)
            local s = e:get(Speed)
            if s then s.value = s.value + amount end
        end,
        { value_format = "+{} SPD" })
    return mod
end
```

Drop the folder in `mods/`, launch the game — your upgrade appears in the level-up pool. The
canonical, complete example is **`mods/core/mod.lua`** (all of Soulkeeper's built-in content). The
type annotations in **`types/library.lua`** give autocomplete and signatures in any editor using
lua-language-server.

---

## 9. Reference: runtime & internals

- **Runtime:** Lua 5.4 embedded via [sol2](https://github.com/ThePhD/sol2). Generational GC for low
  frame-time jitter; callbacks cached by wire-id in flat arrays so dispatch does no lookup on the hot
  path.
- **Layering:** the registration API + sim bindings live in `src/shared/mod/` (rendering-free); the
  render bindings live in `src/client/mod/`. The server binary links Lua but never SDL/ImGui.
- **Registry:** `src/shared/mod/registry.*` holds the string-id → wire-id maps and metadata
  (`ContentRegistry` for upgrades/objects, `EnemyRegistry` for enemy archetypes — separate id
  spaces, same deterministic sort scheme);
  `src/shared/mod/lua_host.*` owns the `sol::state`, exposes `register_mod`/`Mod`, and discovers
  `mods/*/mod.lua`. `src/shared/mod/events.hpp` is the event bus. `src/shared/mod/script_ecs.hpp` is
  the scripting-ECS (dynamic components + net-id assignment + snapshot serialization); dynamic
  component storage lives in `core::Registry` (`DynamicPool`).
- **Wire:** a level-up choice travels as `LevelUpChoice{ id, rarity }` (`id` = wire-id). Each snapshot
  entry is followed by its networked script components (`count` + `{net_id, floats}`), net-ids being
  the sorted index among networked components — identical on both ends.
```
