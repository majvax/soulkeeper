---@meta
-- Soulkeeper modding API — type annotations for lua-language-server (LuaCATS).
-- This file is NOT loaded by the game (the loader only runs mods/<name>/mod.lua).
-- Point your editor's Lua library path at the `types/` folder for autocomplete.

--=============================================================================
-- Engine components (built-in, C++). Each has a tag global (e.g. `Weapon`) you
-- pass to Entity:get/has/assign/remove. Mods can also define their OWN script
-- components with Mod:define_component (referenced by string id, e.g. "core:aura").
--=============================================================================

---@class Position
---@field x number
---@field y number

---@class Velocity
---@field dx number
---@field dy number

---@class Speed
---@field value number  # movement speed, px/s

---@class Health
---@field current number
---@field max number

---@class Radius
---@field value number  # collision radius, px

---@class Damage
---@field per_second number  # contact damage dealt to players

---@class Weapon
---@field cooldown_max number
---@field cooldown_current number
---@field bullet_speed number
---@field damage number
---@field projectile_lifetime number

---@class AimState
---@field dx number
---@field dy number
---@field firing integer  # 1 while the trigger is held

---@class EnemyTag   # membership-only tag (no fields)
---@class PlayerTag  # membership-only tag (no fields)

---@alias Component Position|Velocity|Speed|Health|Radius|Damage|Weapon|AimState|EnemyTag|PlayerTag

-- Component tag globals (pass these to Entity/world methods).
---@type Component
Position = nil
---@type Component
Velocity = nil
---@type Component
Speed = nil
---@type Component
Health = nil
---@type Component
Radius = nil
---@type Component
Damage = nil
---@type Component
Weapon = nil
---@type Component
AimState = nil
---@type Component
Enemy = nil
---@type Component
Player = nil

--=============================================================================
-- Entity handle (passed into sim callbacks and yielded by world:each). Engine
-- components are addressed by their tag global; script components by string id.
--=============================================================================

---@class Entity
local Entity = {}

---Return the component (mutate its fields in place), or nil if absent.
---@param component Component|string
---@return any
function Entity:get(component) end

---@param component Component|string
---@return boolean
function Entity:has(component) end

---Add or replace an ENGINE component from a table of its fields.
---@param component Component
---@param fields table
function Entity:assign(component, fields) end

---Add or replace a SCRIPT component (by string id) from a table of its fields.
---@param id string
---@param fields table
function Entity:set(id, fields) end

---@param component Component|string
function Entity:remove(component) end

--=============================================================================
-- The `world` query facade (sim VM; use inside systems).
--=============================================================================

---@class World
world = {}

---Iterate every entity owning ALL of the given components (tag globals or script
---ids). Use in a generic for: `for e in world:each(Enemy, Position) do ... end`.
---@param ... Component|string
---@return fun(): Entity|nil
function world:each(...) end

-- Spawn factories (sim VM). Return an Entity for further configuration.
---@return Entity
function spawn_projectile(x, y, vx, vy, damage, lifetime) end
---@return Entity
function spawn_xp_orb(x, y, value) end
---@return Entity
function spawn_enemy(x, y, type) end

--=============================================================================
-- Draw context + view (render VM; passed into an object's `draw` hook). `x,y`
-- are screen-space. `view:get(id)` returns the entity's networked script
-- component as a table of fields (or nil).
--=============================================================================

---@class DrawView
---@field x number
---@field y number
local DrawView = {}

---@param id string  # a networked script component id, e.g. "core:aura"
---@return table|nil
function DrawView:get(id) end

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

---@class StatUpgradeOpts
---@field available? AvailableFn
---@field sprite? string
---@field value_format? string   # fmt-style, applied to the rarity amount, e.g. "+{} DMG"
---@field value_text? ValueTextFn # advanced: compute the card text yourself

---@class ObjectOpts
---@field available? AvailableFn
---@field sprite? string
---@field value_text? ValueTextFn
---@field draw? DrawFn

---@class ComponentOpts
---@field networked? boolean  # sync to clients so draw hooks can read it

---@class SystemOpts
---@field phase? string   # "motion" (before Movement) or "update" (default, after Combat)
---@field rate? number    # optional throttle in Hz (default: every tick)

--=============================================================================
-- Mod handle + registration.
--=============================================================================

---@class Mod
local Mod = {}

---Register a repeatable, rarity-scaled stat upgrade.
---@param id string       # namespaced, e.g. "core:damage"
---@param label string
---@param amounts number[] # { common, uncommon, legendary }
---@param apply ApplyFn
---@param opts? StatUpgradeOpts
function Mod:add_stat_upgrade(id, label, amounts, apply, opts) end

---Register a one-time object (no rarity scaling; the engine enforces "once").
---@param id string
---@param label string
---@param acquire AcquireFn
---@param opts? ObjectOpts
function Mod:add_object(id, label, acquire, opts) end

---Define a component: a named list of number fields, optionally networked.
---@param id string
---@param fields string[]
---@param opts? ComponentOpts
function Mod:define_component(id, fields, opts) end

---Define a system: fn(dt) run each tick (or throttled) at a pipeline phase.
---@param id string
---@param opts SystemOpts
---@param fn SystemFn
function Mod:define_system(id, opts, fn) end

---Subscribe to a game event. Known events (server-side):
---  "on_wave_start"    fun(wave: integer)
---  "on_enemy_death"   fun(victim: { x: number, y: number, variant: integer, xp: integer })
---  "on_player_downed" fun(player: Entity)
---  "on_level_up"      fun(level: integer)
---@param event string
---@param handler function
function Mod:subscribe(event, handler) end

---Create a mod. Call once from `main()` and return the handle.
---@param namespace string
---@param description string
---@param author string
---@return Mod
function register_mod(namespace, description, author) end

---Plugin entry point. Define this in your mod.lua; the loader calls it once.
---@return Mod
function main() end
