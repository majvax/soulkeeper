---@meta
-- Soulkeeper modding API — type annotations for lua-language-server (LuaCATS).
-- This file is NOT loaded by the game (the loader only runs mods/<name>/mod.lua).
-- Point your editor's Lua library path at the `types/` folder for autocomplete.

--=============================================================================
-- Component handles. EVERY component — kernel (C++) or Lua-defined — is a
-- Component handle: the prelude globals below, or the value returned by
-- Mod:component(). All APIs take handles; there are no string component ids.
--=============================================================================

---@class Component
---@field id string  # full id, e.g. "Position" or "core:ranged" (read-only)

-- Kernel component field shapes (what e:get(Handle) returns for each prelude
-- handle; mutate fields in place).

---@class PositionFields
---@field x number
---@field y number

---@class VelocityFields
---@field dx number
---@field dy number

---@class SpeedFields
---@field value number  # movement speed, px/s

---@class HealthFields   # enemies (float HP; players use Hearts)
---@field current number
---@field max number

---@class HeartsFields   # player heart life (discrete; hits cost whole hearts)
---@field current integer
---@field max integer

---@class RadiusFields
---@field value number  # collision radius, px

---@class RenderFields
---@field kind integer     # proto::EntityKind (see the KIND table)
---@field variant integer  # per-kind byte (enemy archetype / bullet tint)

---@class DownedFields
---@field respawn_wave integer

---@class AimStateFields
---@field dx number
---@field dy number
---@field firing integer  # 1 while the trigger is held

---@class DashFields
---@field cooldown_max number     # seconds per charge refill
---@field cooldown number         # time until the next charge
---@field burst_remaining number  # > 0 while dashing
---@field shockwave number        # damage to enemies passed through (0 = off)
---@field charges integer
---@field max_charges integer

---@class XpRewardFields
---@field value integer  # XP orb dropped on death

-- The kernel prelude (same handles in both VMs).
---@type Component
Position = nil
---@type Component
Velocity = nil
---@type Component
Speed = nil
---@type Component
Health = nil
---@type Component
Hearts = nil
---@type Component
Radius = nil
---@type Component
AimState = nil
---@type Component
Dash = nil
---@type Component
XpReward = nil
---@type Component
Render = nil
---@type Component
Downed = nil
---@type Component
Enemy = nil -- membership-only tag
---@type Component
Player = nil -- membership-only tag

---Render.kind values for Lua-spawned drawables.
---@type { mover: integer, player: integer, enemy: integer, bullet: integer, orb: integer, heart: integer }
KIND = nil

--=============================================================================
-- Entity handle (passed into sim callbacks and yielded by world:each).
--=============================================================================

---@class Entity
local Entity = {}

---Return the component (mutate its fields in place), or nil if absent.
---Lua-defined components are strict: reading/writing an unknown field errors.
---@param component Component
---@return table|nil
function Entity:get(component) end

---@param component Component
---@return boolean
function Entity:has(component) end

---Add or replace a component. Lua-defined components fill unset fields from
---their declared defaults; unknown field names raise an error (typo guard).
---@param component Component
---@param fields table
function Entity:set(component, fields) end

---@param component Component
function Entity:remove(component) end

---Destroy the entity (safe on already-dead handles).
function Entity:destroy() end

--=============================================================================
-- The `world` query facade (sim VM; use inside systems).
--=============================================================================

---@class World
world = {}

---Iterate every entity owning ALL of the given components.
---`for e in world:each(Enemy, Position, C.Ranged) do ... end`
---@param ... Component
---@return fun(): Entity|nil
function world:each(...) end

---Iterate entities within `radius` px (center distance) owning ALL of the
---given components — served by the kernel spatial hash. Add collision radii
---into `radius` yourself.
---@param x number
---@param y number
---@param radius number
---@param ... Component
---@return fun(): Entity|nil
function world:nearby(x, y, radius, ...) end

---@return integer # the current wave number
function world:wave() end

---Add to the shared team XP pool.
---@param value integer
function world:add_xp(value) end

-- Kernel spawn primitives (sim VM). Return an Entity to attach components to.
---Spawn a kinetic drawable (Position/Velocity/Radius 4/Render bullet kind).
---Attach your bullet component for behavior; set Render.variant for tint.
---@return Entity
function spawn_bullet(x, y, vx, vy) end
---Spawn a bare drawable (Position + Render). Set Render.kind (see KIND) and
---attach components — how drops/markers are made.
---@return Entity
function spawn_entity(x, y) end
---Spawn a registered enemy archetype (applies its component bag at the
---current wave + fires its on_spawn hook).
---@param id string  # a registered enemy id, e.g. "core:brute"
---@return Entity|nil # nil (and a logged error) if the id is unknown
function spawn_enemy(x, y, id) end

--=============================================================================
-- Cross-plugin: import.
--=============================================================================

---Run a file relative to the current plugin's folder and return its value.
---@param path string
---@return any
function include(path) end

---Load another plugin (by its mods/<name> folder name) and return its exports
---(whatever its main() returned — by convention a table of Component handles).
---Lazy + memoized; circular imports error. `local core = import("core")`
---@param name string
---@return any
function import(name) end

--=============================================================================
-- Draw context + view (render VM; passed into an object's `draw` hook). `x,y`
-- are screen-space. `view:get(H)` returns the entity's networked component as
-- a plain table of fields (or nil).
--=============================================================================

---@class DrawView
---@field x number
---@field y number
local DrawView = {}

---@param component Component  # a NETWORKED Lua-defined component
---@return table|nil
function DrawView:get(component) end

---@class DrawContext
local DrawContext = {}

function DrawContext:texture(path, x, y, w, h) end
function DrawContext:rect(x, y, w, h, r, g, b, a) end
function DrawContext:circle_filled(cx, cy, radius, r, g, b, a) end
function DrawContext:circle(cx, cy, radius, r, g, b, a, thickness) end
function DrawContext:text(x, y, s, r, g, b, a) end
---@return number sx, number sy
function DrawContext:world_to_screen(wx, wy) end

--=============================================================================
-- Callback signatures + option tables.
--=============================================================================

---@alias ApplyFn fun(e: Entity, rarity: integer, amount: number)
---@alias AcquireFn fun(e: Entity)
---@alias AvailableFn fun(e: Entity): boolean
---@alias ValueTextFn fun(amount: number, rarity: integer): string
---@alias DrawFn fun(ctx: DrawContext, view: DrawView)
---@alias SystemFn fun(dt: number)

---@class UpgradeOpts
---@field available? AvailableFn
---@field sprite? string
---@field value_format? string   # fmt-style, applied to the rarity amount, e.g. "+{} DMG"
---@field value_text? ValueTextFn # advanced: compute the card text yourself

---@class ObjectOpts
---@field available? AvailableFn
---@field sprite? string
---@field value_text? ValueTextFn
---@field draw? DrawFn
---@field rarity? string  # tier the object rolls at: "common".."legendary" (default "epic")

---@class ComponentOpts
---@field networked? boolean  # sync to clients so draw hooks can read it

---@class SystemOpts
---@field phase? string   # "motion" (before Movement) or "update" (default, after Combat)
---@field rate? number    # optional throttle in Hz (default: every tick)

---@class EnemyOpts
---@field weight? number|fun(wave: integer): number # relative spawn weight, re-evaluated once per wave (default 0 = never spawns naturally)
---@field scale? number    # on-screen size factor (default 1.0)
---@field tint? integer[]  # { r, g, b } colour mod on the sprite (default white)
---@field sprite? string   # an animation-pack FOLDER of <Clip>_<N>x1.png strips (the engine slices frames, plays Idle/Move, flips for facing) or a static .png path (default: the shared enemy sprite)
---@field on_spawn? fun(e: Entity) # escape hatch for dynamic per-spawn logic (prefer :component())

--=============================================================================
-- Mod handle + registration. All names are auto-namespaced with the mod's
-- namespace ("damage" in mod "core" -> "core:damage").
--=============================================================================

---Archetype handle returned by Mod:enemy — the enemy's component bag.
---@class EnemyArchetype
local EnemyArchetype = {}

---Attach a component to every spawned instance. `fields` may be a function of
---the wave (re-resolved once per wave — the wave-scaling mechanism). Chainable.
---Every enemy needs at least Health and Radius.
---@param component Component
---@param fields table|fun(wave: integer): table
---@return EnemyArchetype
function EnemyArchetype:component(component, fields) end

---@class Mod
local Mod = {}

---Define a component: fields with their defaults. Returns THE handle — store
---it, use it everywhere, export it for other plugins.
---@param name string  # bare name; auto-namespaced
---@param fields table<string, number>  # field -> default value
---@param opts? ComponentOpts
---@return Component
function Mod:component(name, fields, opts) end

---Define a system: fn(dt) run each tick (or throttled) at a pipeline phase.
---@param name string
---@param opts SystemOpts
---@param fn SystemFn
function Mod:system(name, opts, fn) end

---Register a repeatable, rarity-scaled stat upgrade.
---@param name string
---@param label string
---@param amounts number[] # per tier { common, uncommon, rare, epic, legendary }; missing/0 = not offered at that tier
---@param apply ApplyFn
---@param opts? UpgradeOpts
function Mod:upgrade(name, label, amounts, apply, opts) end

---Register a one-time object (no rarity scaling; the engine enforces "once").
---@param name string
---@param label string
---@param acquire AcquireFn
---@param opts? ObjectOpts
function Mod:object(name, label, acquire, opts) end

---Register an enemy archetype: identity + visuals + spawn weight. Chain
---:component(...) for everything gameplay-defining. See mods/core/enemies.lua.
---@param name string
---@param label string
---@param opts? EnemyOpts
---@return EnemyArchetype
function Mod:enemy(name, label, opts) end

---Declare the player character's visuals: an animation-pack folder (of
---<Clip>_<N>x1.png strips) or a static .png. The engine handles frame
---slicing, Idle/Move switching and facing — no animation code in Lua.
---Render-VM metadata only (not part of the plugin hash); last call wins.
---@param path string  # e.g. "assets/sprite/Knight_LVL1"
function Mod:player_sprite(path) end

---Subscribe to a game event. Engine events (server-side):
---  "on_wave_start"    fun(wave: integer)
---  "on_enemy_death"   fun(victim: { x: number, y: number, variant: integer, xp: integer })
---  "on_player_downed" fun(player: Entity)
---  "on_level_up"      fun(level: integer)
---Custom events use their emitter's full name, e.g. "core:boss_spawned".
---@param event string
---@param handler function
function Mod:subscribe(event, handler) end

---Fire a custom event other plugins can subscribe to. Bare names are
---auto-namespaced ("boss_spawned" -> "core:boss_spawned").
---@param event string
function Mod:emit(event, ...) end

---Create a mod. Call once from `main()`. The namespace MUST equal the plugin's
---folder name (it's the import name).
---@param namespace string
---@param description string
---@param author string
---@return Mod
function register_mod(namespace, description, author) end

---Plugin entry point. Define this in your mod.lua; the loader (or an import)
---calls it once. Its return value is the plugin's exports.
---@return any
function main() end
