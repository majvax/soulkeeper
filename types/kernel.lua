---@meta
-- Soulkeeper KERNEL modding API — type annotations for lua-language-server.
-- This is the ENGINE surface: the Mod/Entity/World/DrawView/Hud handles, the
-- kernel component *Fields + prelude globals, spawn_*/include/import. A mod's
-- OWN types (component shapes, custom event payloads) live with the mod:
-- component shapes are inferred inline in its components.lua; extra hand-written
-- types can go in mods/<name>/types.lua (any ---@meta file is auto-loaded).
-- NOT loaded by the game (the loader only runs mods/<name>/mod.lua). Point your
-- editor's Lua library path at `types/` (the repo's .luarc.json already does).

--=============================================================================
-- Component handles. EVERY component — kernel (C++) or Lua-defined — is a
-- Component handle: the prelude globals below, or the value returned by
-- Mod:component(). All APIs take handles; there are no string component ids.
--=============================================================================

--- A component HANDLE — the prelude globals below, or a `Mod:component` return.
--- Typed permissively so every handle is accepted by has/set/each/nearby/closest;
--- the field TYPE flows on the READ side (`e:get`/`view:get`), via an identity
--- generic: `e:get(H)` returns H's own field shape. Kernel handles are typed as
--- their `*Fields` class (closed → autocomplete + typo errors); Lua handles get
--- the defaults-table shape inferred by `Mod:component` (autocomplete).
---@alias Component any

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
---@field dir_x number            # normalized burst direction
---@field dir_y number
---@field shockwave number        # damage to enemies passed through (0 = off)
---@field charges integer
---@field max_charges integer

---@class XpRewardFields
---@field value integer  # XP orb dropped on death

---@class ScaleFields
---@field value number  # on-screen size multiplier (networked; 1.0 = normal, capped ~8x on the wire). Players spawn with it; enemies multiply it onto their archetype scale. Visual only — Radius is the hitbox.

-- The kernel prelude (same handles in both VMs). Each carries its field shape so
-- e:get(Handle) autocompletes (Enemy/Player are membership-only tags: no fields).
---@type PositionFields
Position = nil
---@type VelocityFields
Velocity = nil
---@type SpeedFields
Speed = nil
---@type HealthFields
Health = nil
---@type HeartsFields
Hearts = nil
---@type RadiusFields
Radius = nil
---@type AimStateFields
AimState = nil
---@type DashFields
Dash = nil
---@type XpRewardFields
XpReward = nil
---@type RenderFields
Render = nil
---@type DownedFields
Downed = nil
---@type Component
Enemy = nil  -- membership-only tag (no fields)
---@type Component
Player = nil -- membership-only tag (no fields)
---@type ScaleFields
Scale = nil

---Render.kind values for Lua-spawned drawables.
---@type { mover: integer, player: integer, enemy: integer, bullet: integer, orb: integer, heart: integer }
KIND = nil

--=============================================================================
-- Entity handle (passed into sim callbacks and yielded by world:each).
--=============================================================================

---@class Entity
local Entity = {}

---Return the component (mutate its fields in place). The returned table has the
---handle's field shape — autocomplete + unknown-field typo-check. Typed as
---always-present to fit the `world:each(A, B, …)`/`e:has(H)` idiom (where the
---filter already guarantees it); it is nil only if the entity truly lacks the
---component, so still `if c then …` when presence isn't guaranteed.
---Lua-defined components are strict: reading/writing an unknown field errors.
---@generic T
---@param component T
---@return T
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

---Nearest entity owning ALL of the given components, with its squared
---distance and position — ONE engine call. Use this instead of looping
---candidates from Lua inside a per-entity system (that pattern dominates
---tick time). An optional trailing `{ without = H }` (or `{ without = {H1,
---H2} }`) excludes entities owning a component, e.g. Downed players:
---`local p, d2, px, py = world:closest(x, y, Player, Position, { without = Downed })`
---Entities without Position are ignored. All four returns are nil on no match.
---@param x number
---@param y number
---@param ... Component|{ without: Component|Component[] }
---@return Entity|nil entity, number|nil d2, number|nil px, number|nil py
function world:closest(x, y, ...) end

---@return integer # the current wave number
function world:wave() end

---Add to the shared team XP pool.
---@param value integer
function world:add_xp(value) end

---End the run (the GAME decides when — e.g. everyone downed, or a win rule).
---The engine freezes the sim, broadcasts the game-over screen with final
---stats, and waits for the host to return everyone to the lobby.
---@param won boolean # true = victory, false = defeat
function world:end_game(won) end

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

---Read a component's fields (typed by the handle). Works for NETWORKED Lua
---components. Inside a mod:hud hook it ALSO resolves the local player's kernel
---handles: Position, Speed, Hearts, Dash, Scale (same schema as the server
---side) — those aren't readable in world draw hooks. Returns nil if the entity
---lacks the component, so guard with `if c then …` when it isn't guaranteed.
---@generic T
---@param component T
---@return T
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

-- Handed to a mod:hud hook. A hook opens its OWN panel (begin_panel/end_panel),
-- then draws text / cached icons / cooldown circles into it. `end` is a Lua
-- keyword, so the closer is `end_panel`.
---@class HudContext
local HudContext = {}

---Open the hook's window: fixed, borderless, non-movable, auto-sized. Defaults
---to the top-left of the screen; pass x/y (pixels) to place it elsewhere.
---@param title string  # window id (not shown; used as the ImGui id)
---@param x? number
---@param y? number
function HudContext:begin_panel(title, x, y) end

function HudContext:end_panel() end

---@param s string
function HudContext:text(s) end

---@param r integer  # 0..255
---@param g integer
---@param b integer
---@param s string
function HudContext:text_colored(r, g, b, s) end

function HudContext:separator() end

---Keep the next item on the same line (lay icons/circles in a row).
function HudContext:same_line() end

---Draw a cached texture (by asset path) at size x size, at the cursor.
---@param path string
---@param size number
function HudContext:image(path, size) end

---Same as image() but multiplied by an RGBA tint (e.g. dim an empty heart).
---@param path string
---@param size number
---@param r integer @param g integer @param b integer @param a integer
function HudContext:image_tinted(path, size, r, g, b, a) end

---Cooldown/loading disc: a faint ring plus a wedge filled for `fraction` (0..1)
---of a turn from the top (full disc at >= 1). Advances the cursor by 2*radius.
---@param radius number
---@param fraction number  # 0..1
---@param r integer @param g integer @param b integer @param a integer
function HudContext:pie(radius, fraction, r, g, b, a) end

--=============================================================================
-- Callback signatures + option tables.
--=============================================================================

---@alias ApplyFn fun(e: Entity, rarity: integer, amount: number)
---@alias AcquireFn fun(e: Entity)
---@alias AvailableFn fun(e: Entity): boolean
---@alias ValueTextFn fun(amount: number, rarity: integer): string
---@alias DrawFn fun(ctx: DrawContext, view: DrawView)
---@alias HudFn fun(hud: HudContext, view: DrawView)
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
---@field rate? number    # optional throttle in Hz (default: every tick); fn receives the ACCUMULATED dt
---@field stagger? number # 0..1 fraction of the rate interval: offsets which tick this fires on, so same-rate systems spread across ticks instead of piling onto one (default 0). Systems that must see each other's same-tick writes (e.g. one rewrites a velocity another scales) keep the same rate AND stagger.

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
---it, use it everywhere, export it for other plugins. The defaults table's shape
---becomes the handle's field type, so `e:get(H).field` autocompletes with
---nothing else to declare.
---@generic T
---@param name string  # bare name; auto-namespaced
---@param fields T  # field -> default value (its shape types the handle)
---@param opts? ComponentOpts
---@return T
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
---slicing, state switching and facing — no animation code in Lua.
---A DIRECTIONAL pack names clips <State>_<Dir8> (states Idle/Move/Shoot/
---MoveShoot/Dash/Death; dirs Down/DownLeft/Left/UpLeft/Up/UpRight/Right/
---DownRight — pure Left/Right optional, the Down diagonal substitutes) plus
---an optional Shadow_1x1.png; the engine picks state and 8-way direction
---(aim/movement/dash/downed). A plain Idle/Move pack keeps right-facing+flip.
---Render-VM metadata only (not part of the plugin hash); last call wins.
---@param path string  # e.g. "assets/sprite/player"
function Mod:player_sprite(path) end

---Register a HUD hook: fn(hud, view) runs once per frame on the client, in the
---HUD panel, for the LOCAL player. `view:get(component)` reads that player's
---NETWORKED script components (e.g. a stats component) — mark them networked.
---Render-VM only (not part of the plugin hash); the sim VM stores but never runs it.
---@param fn HudFn
function Mod:hud(fn) end

---Register a console command: `/name args...` typed in the client's TAB
---console runs fn on the SERVER (host-only, sim VM) with the invoking player
---and the whitespace-split args — numeric tokens arrive as numbers, the rest
---as strings. `usage` feeds the console's autocompletion and /help. Names are
---bare (typed by hand — no namespace); on a clash the first registration wins.
---```lua
---mod:command("givexp", "/givexp <amount>  -- add team XP", function(player, amount)
---    world:add_xp(math.floor(amount or 0))
---end)
---```
---@param name string   # bare command name (no slash)
---@param usage string  # one-line usage/help shown by autocomplete + /help
---@param fn fun(player: Entity, ...: number|string)
function Mod:command(name, usage, fn) end

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
