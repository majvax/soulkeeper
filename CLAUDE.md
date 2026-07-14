# Soulkeeper — project context

Expert **C++26**, data-oriented, authoritative-netcode game engineer. **Soulkeeper** is a
**multiplayer (2–4 co-op) survivor-style roguelike** built from scratch (custom ECS, no commercial
engine), server-authoritative, SDL3 + Dear ImGui on the client, ENet UDP. Cross-platform
(Windows/Linux). It's the C++26 successor to `../wild-woods-2` (Python/pygame/esper prototype) —
use it only as a **design/tuning reference** (`src/client/{factory,processor,scene}/`), don't copy
its OO structure.

## Tech stack
- **C++26** (`<expected>`/`<optional>`, concepts). GCC 16 available.
- **SDL3** (client only), **Dear ImGui** (docking branch, SDL3 + SDLRenderer3 backends, built as a
  CPM lib), **ENet** (UDP), **stb_image** (PNG), **spdlog/fmt** (server logging).
- **CMake ≥ 4.3 + CPM.cmake**; deps in `Dependencies.cmake`. `src/CMakeLists.txt` builds **two
  targets** (`client`, `server`) from `file(GLOB_RECURSE …)`.

## Layering (STRICT — the load-bearing invariant)
`core/` and `shared/` and `server/` are **SDL/ImGui/rendering-FREE**; only `client/` has SDL/ImGui.
The server must run the exact same simulation headless.
- `grep -rlE 'SDL3/SDL.h|imgui' src/core src/shared src/server` → **must be empty**.
- `ldd bin/server` → **no SDL/ImGui**.
- The Engine + Scene stack are client concerns and live in `client/`, not `core/`.

## Layout (what's where — all implemented)
```
src/
  core/            # SDL-free engine primitives (namespace core)
    ecs.hpp        #   sparse-set ECS: Registry, versioned Entity(20b idx/12b ver), ComponentPool, view<>().each
    timestep.hpp   #   FixedTimestep accumulator (add_time/consume/dt/alpha)
    spatial.hpp    #   SpatialGrid uniform hash (insert/query AABB), broad-phase
    systems.hpp    #   SystemManager (ordered vector of void(Registry&,float))
  shared/          # SDL-free, used by client AND server
    components/    #   KERNEL comps only: physics(Position,PrevPosition,Velocity,Speed) combat(Health[enemies],
                   #   Hearts[players],Radius,AimState,Dash,Render{kind,variant},Scale[wire size
                   #   multiplier, Lua-mutated],XpReward,EnemyTag)
                   #   gameplay(PlayerTag, ObjectInventory, WorldGrid[spatial-hash singleton])
                   #   progression(GameStats{xp,wave}, Downed{respawn_wave}, RunStats{damage,
                   #   kills,downs,revives} — Lua-incremented scoreboard, shipped in GameOver)
    system/        #   KERNEL systems only: grid (spatial rebuild), dash, movement, separation
                   #   (enemy anti-cramming: soft pair nudges post-Movement via the WorldGrid;
                   #   Speed<=0 enemies are ANCHORS — planted kegs, sleeping Mimics, parked/
                   #   telegraphing bosses hold their ground; mass ∝ radius² so trash can't
                   #   shove a boss; spacing 1.6x hitbox sum, golden-angle fan on exact stacks)
                   #   + input.hpp (apply_input, start_dash/tick_dash shared w/ prediction, PLAYER_SPEED, DASH_*)
    factory/       #   create_player / create_enemy — kernel parts only (loadout/stats come from Lua)
    sim/           #   World (Registry+SystemManager); make_game_world() = kernel pipeline + singletons;
                   #   world.hpp phase constants (grid/targeting/motion/shooting/movement/separation/projectile/combat/update/pickup/death)
    mod/           #   Lua modding layer (SDL-free): component_ref (THE handle type), bindings_table (engine
                   #   dispatch + prelude list), registry (content/enemy defs -> deterministic wire-id),
                   #   lua_host (sol::state, register_mod, import() loader, mod verbs), events (bus),
                   #   script_ecs (Lua components + net sync), sim_bindings (entity/world API + services)
    net/           #   net.hpp/.cpp — ENet wrapper (PImpl, enet only in .cpp): Server, Client, ScopedInit, Event
    protocol.hpp   #   MsgType, EntityKind, GameState, Command; ByteWriter/Reader (little-endian); packet structs; Snapshot{Header,Entry}
  server/          # SDL-free authoritative host (namespace server)
    game_server.*  #   GameServer: sessions-by-token, lobby/playing/pause, wave spawner, level-up orchestration, snapshots
  server.cpp       #   bootstrap + std::chrono fixed-tick loop
  client/          # SDL/ImGui (namespace client)
    engine.hpp/.cpp#   Engine = the client app: owns Session + SceneManager + ImGuiLayer; fixed-timestep run(); FULLSCREEN
    scene.hpp      #   Scene base (handle_event/update/render -> Propagation Continue/Stop) + SceneManager (DEFERRED push/pop/clear + apply_pending)
    session.hpp    #   client net control-plane: connect/reconnect, drains net, roster/state/id, latest snapshot, send_*
    ui.hpp / renderer.hpp   # ImGuiLayer RAII; Textures cache (stb_image, image-or-rect fallback)
    gui.hpp/.cpp            # the game's OWN widget kit (immediate-mode): 9-sliced pixel-art
                            #   panels/buttons/sliders/inputs/hearts from assets/ui/*.png (BlackAndWhite
                            #   pack) + Press Start 2P text baked via stb_truetype (assets/font/).
                            #   USER-FACING UI draws with this: connect/lobby/level-up/game-over,
                            #   banners, AND mod HUD panels (HudContext buffers items, then draws
                            #   an auto-sized kit panel). ImGui is DEV-ONLY now (console, debug
                            #   drawlist overlays). Engine owns it (engine->gui()); ALL widget
                            #   input is event-driven (Engine forwards events; no GetMouseState).
    audio.hpp/.cpp          # Audio mixer on raw SDL3 streams (device mixes bound streams): 16-voice
                            #   SFX pool (pitch jitter, 40ms same-name throttle, play_at falloff) +
                            #   looping music stream w/ cross-fade; clips = assets/sound/<name>.wav|.ogg
                            #   (OGG via stb_vorbis — impl TU stb_vorbis_impl.cpp); NO device = silent,
                            #   never fatal. Canonical names in modding.md; mod:sound rebinds them.
    sprites.hpp             # SpritePacks: auto-discovers <Clip>_<N>x1.png strip folders; draw_clip
                            #   (frame slicing, Idle/Move OR 8-way <State>_<Dir8> directional packs,
                            #   right-facing flip, play-once) — Lua only names the folder
    mod/render_bindings.*   # render VM bindings: draw ctx (texture/rect/circle/text/play) + player view
    scene/{connect,lobby,game,console,level_up,pause,game_over}.hpp # game_over owns the local-bests file
  client.cpp       # ~15-line bootstrap: argv host/name -> Engine::create -> run()
mods/core/         # ALL built-in content as a Lua plugin (upgrades, objects, components, systems)
types/kernel.lua   # lua-language-server stubs for the ENGINE mod API (editor autocomplete);
                   # a mod's own types go in mods/<name>/types.lua (component shapes inferred inline)
modding.md         # the full modding guide (API reference, performance model, internals)
```

## Runtime model (how it actually works)
- **Server-authoritative; C++ is the engine, Lua is the game.** The kernel pipeline is
  `Grid → Dash → Movement → Separation` at 120 Hz; **every game rule** (targeting, shooting, bullets, contact
  damage + i-frames, deaths/drops/respawns, pickups, auras) is a Lua system in `mods/core/`
  slotted into named phases between the kernel systems. Snapshots stream at 60 Hz carrying
  `Render{kind,variant}` bytes (Lua-controlled visuals); entries are **packed + quantized**
  (int16 half-px offsets from a player-centroid origin in the header — 14 B/entity, cap 600
  enemies), followed by a small **PlayerAim trailer** (≤4 × 13 B: authoritative aim dir +
  firing bit + dash state per player, so the client drives sprite facing/shoot-pose from the
  SIM's aim — a server-side override like autofire shows correctly — not the local mouse).
  **Delta snapshots** (`shared/snapshot_codec.*`, Quake-style): each snapshot tick the server
  captures a `SnapshotState` into a 32-deep ring; clients ack the newest APPLIED tick in every
  `Input.ack_tick`, and the server sends each peer a per-entity field-delta vs its acked
  baseline (flags byte: int8 pos delta / omitted-if-unchanged fields / explicit removals; full
  snapshot on join, reconnect, or stale ack). The client keeps its own applied-state ring and
  decodes with the SAME codec (`decode_delta` returns the complete merged state, so the apply
  path is identical for full and delta). Telemetry logs `snap kB/s` every 5 s. Clients
  **predict** the local player (same `apply_input`/`tick_dash`, corrected by snapshots) and
  **interpolate** remotes. `Session` mirrors control state + ack; `GameScene` keeps a
  render-only `Registry`.
- **Scenes self-drive transitions** via `engine_->scenes()` (deferred push/pop, applied at safe
  points): Lobby→Game on `Playing`; GameScene TAB pushes Console (pops itself); GameScene pushes
  LevelUp on level-up (pops itself). The Engine only hardcodes the initial `LobbyScene`.
- **Input is event-driven** — `GameScene::handle_event` maintains a held-key set (WASD/Z-Q-S-D +
  LMB); a scene above returning **Stop** naturally blocks it. **Do NOT** use `SDL_GetKeyboardState`
  or `io.WantCaptureKeyboard` for gameplay input (that bypasses the scene stack).
- `Engine::width()/height()` return the **real render output size** (`SDL_GetRenderOutputSize`), not
  the config size — UI must center against these (fullscreen / any aspect ratio).
- Reconnect identity = `token = hash(player name)`; a dropped player's entity is kept and resumed.

## Modding layer (the game = Lua plugins)
ALL game logic is **Lua 5.4 plugins** (sol2) over kernel services — see `modding.md` (full
guide), `mods/core/` (Soulkeeper's entire gameplay), `types/kernel.lua` (engine LSP stubs).
- **Handle-based API (v2, no strings)**: `mod:component("ranged", {range=340,...})` returns a
  `ComponentRef` handle; `e:get/set/has/remove`, `world:each/nearby`, the enemy builder and
  `view:get` take handles only. Kernel comps are prelude handles (`Position`, `Hearts`, ...).
  Field access is **strict** — unknown names raise. NOTE: kernel integer fields (Hearts, Dash
  charges) need `math.floor(...)` on float arithmetic before writing back.
- **import("name")** = require-style lazy plugin loading (folder name = namespace = import name);
  a plugin's exports are its `main()`'s return. Component-library plugins are first-class.
- **Services**: `world:nearby(x,y,r,H…)` (kernel spatial hash), `world:closest(x,y,H…,{without=H})`
  (nearest entity + d² + pos in ONE call — never target-scan from Lua), `world:wave()/add_xp`,
  `world:end_game(won)` (the GAME decides when a run ends — core: all downed = defeat, wave 20 =
  win; the engine freezes + shows the game-over screen, host returns everyone to the lobby),
  `spawn_bullet/spawn_entity/spawn_enemy`, `KIND` table, `on_player_spawn` event (loadout hook).
- **Perf**: system opts `rate` (Hz throttle, fn gets accumulated dt) + `stagger` (0..1, offsets
  which tick same-rate systems fire on — unstaggered 30 Hz systems pile onto one tick and bust
  the 8.3 ms budget). Keep rate+stagger EQUAL only for systems that must see each other's
  same-tick writes (targeting→slow_sys). Server logs `tick avg/max ms + entities` every 5 s.
- **Two VMs, same `mod.lua`**: sim VM (server: systems/events/apply) + render VM (client: draw
  hooks + card metadata).
- **Deterministic wire ids** = lexicographic sort index of namespaced ids; the `mods/` set is
  **validated at join** (`plugin_hash()` in `Join` → `JoinDenied` + kick on mismatch).
- Every Lua callback is a protected call — a broken mod logs and is skipped, never crashes.

## Gameplay implemented
Lobby + host-start + reconnect · waves (25s) with Lua-defined archetypes (~15 in
`mods/core/enemies.lua`, **data-driven `register{}` table + auto gold ELITE twins** from wave 8 —
4x HP/XP, 1.4x size, guaranteed heart via `C.EliteDrop`): fodder
**Bandit/Marauder/Scout/Mushroom**, specials **Bomber** (Goblin Barrel: stops, fx=3 fuse
telegraph, dies in a hostile bullet ring — kill it early to defuse), **Berserker** (`C.Lunge`
telegraph→locked-line dash, motion-phase lockstep with targeting), **Slinger/Vampire/Cyclop/Ent**
(all ranged attacks now **telegraph** `windup` s on fx=3 Prepare, then fire; `C.Ranged` grew
`volley/spread/variant/bullet_radius/homing` — Ent fans 3, Cyclop snipes a heavy variant-3
bullet, the **Acolyte** casts a slow HOMING variant-6 orb), **Slasher**, **Squire** (fast light
melee), **Shieldbearer** (`C.Armor{flat}`: flat reduction per BULLET hit, floor 1 — a walking
DPS check), **Mimic** (`C.Ambush`: spawns Speed-0 inert, one-way WAKE when a player closes
within `trigger` — `ambush_sys`), wave-banded **Brute/Silverback/Goldhorn** Rhino tiers ·
HP scaling is **compound
+8%/wave × (1 + 0.5·(TOTAL players − 1))** — downed players still count; bosses steeper:
**+10%/wave × (1 + 0.7·per extra player)** (`health`/`boss_health` in enemies.lua, resolved at
spawn in the sim VM) · manual-aim projectiles (knockback REMOVED after playtest — base
`Weapon.lifetime` 1.5 s carries the reach, the range upgrade is gone too) ·
**scripted boss ladder to wave 50, each boss a UNIQUE kit driven by a BRAIN**
(`mods/core/brains.lua`: the fight director — every milestone boss carries `C.Brain{id}`, its
mechanic comps are PARKED in the def (`cooldown/timer 9999`) and brain MOVES fire them by
zeroing the timer ("poke"); moves = weighted-random picks, **never the same twice in a row**,
cooldowns jittered ×0.75–1.25 and tightened by health-gated PHASES (`phases` thresholds +
one-time `on_phase` escalations); windup>0 moves telegraph fx=3 with Speed parked, channeled
moves run `during(boss,brain,dt)`; continuous mechanics (sprinkler/regen/blink-evasion) stay
ambient, TRASH archetypes stay brainless self-firing; everything a boss SPAWNS (summons,
kegs, brambles) carries `C.NoLoot` — dies without XP orbs or heart rolls, so add spam is
pressure, never a loot fountain. Kits, still no cross-boss recycling:
**@5 Rhino Charger** (`C.Lunge` charges + close-range STOMP shock; <50% shorter tells) ·
**@10 Frog King** (`C.Nova` rings HIS alone w/ rage densify + arena HOPS reposition the rings +
<50% aimed spit cone, variant 8) · **@15 Frog Prince** (`C.Lunge` LEAP-slam + spit arc + <60%
chained DOUBLE leap; <40% bigger landing ring) · **@20 Bomb Lord** (all ground: `C.Planter`
carpet BATCHES + `C.Toss` pre-lit kegs + homing ROLLER keg (mine w/ Speed) + <60% keg CAGE
ringing a player; <30% faster fuses) · **@30 Vampire Lord** (`C.BoltCaster` homing bolts + bat
`C.Summon{pool=2}` (lore) + <50% BLINK-STRIKE beside a random player + `C.Regen` DPS check;
a PARKED `C.Ranged{standoff=380}` makes him a true kiter — he never walks into his own
blink range, the flee-blink only punishes divers) ·
**@40 Elder Ent** (ambient `C.Sprinkler` streams that REVERSE direction when hurt +
`C.SeedLauncher` seed fans (`volley`, phase 2 = 3) + root snares: `core:bramble` under every
player) · **@50 GAME MASTER finale = the WIN** (`WIN_WAVE=50`; Speed 0 + blink, 5 moves:
`C.Barrage` rotating CROSS lances + ELITE summons `pool=3` + channeled SPIRAL burst +
simultaneous LANCE RAIN at every player + <35% CHECKMATE WALL — takes center, sweeps a
gap-2-slot bullet wall across half the arena); the 25/35/45 minis are REAL KITS too (roam
free, no arena): **@25 Knight Commander** (Knight_LVL4: shield charges + aimed sword waves +
squire rallies (7 s — summons stay spaced) + <50% close-range shield slam) · **@35 Archmage**
(Sorcerer_LVL4: arcane fans + BLINK-then-volley reposition + mirror-image acolytes + <55%
slow homing chase orbs) · **@45 Mimic King** (Mimic_LVL4: wide coin sprays + chomp lunge +
sleeper Mimic adds + <45% FOOL'S GOLD — fake XP orbs (`C.FoolsGold`, Render orb but no C.Xp
so magnets ignore them = the tell) that pop a hostile ring when reached for, `fools_gold_sys`);
every %10 wave is
an ARENA (1920×1080: client wall = the def's `arena {960,540}` opt, sim clamp = the dedicated
`C.Arena{cx,cy,w,h,pinned}` comp — decoupled from C.Nova; the BEARER clamps to a 90 px INSET
rect AND `pinned` tracks the clamp actively holding it: pinned > 1.2 s (wall-hugging player) →
the boss TELEPORTS to a random arena point instead of grinding; `WaveHold` freezes the wave
clock); all carry
`C.Boss` — death system keys **loot CHEST** + guaranteed drops/team-revive/win on it; client
draws a **top-center boss HP bar** + shakes on boss fx; the bar FLASHES white + a `sting` plays
when the health byte crosses 50%/25% (the brains escalate around those marks), a boss kill pops
a full-screen white flash + a big ring, and any enemy jumping >250 px in one snapshot plays the
`blink` whoosh — covers every boss teleport incl. the anti-pin arena hop) · client **fx vocabulary**: `Render.fx` 1 = ATK once, 2 = charge loop, 3 = telegraph
held (`sprites.hpp` discovers ATK/Fire, Charge_RunLoop/Dash/Jump_Full, Prepare/Charge_Begin
clips) · **XP orbs → shared team level pool → synchronized offer scene** (LevelUp msg carries an
`OfferFlavor` byte: level vs chest — the scene titles TREASURE for chests): the offer is rolled
IN LUA (`mod:level_offer(player, level, ctx)` + `world:offerable` entries carry
`kind = upgrade|object`; core rolls **upgrades-only on level-ups, objects-only on chests** —
**objects come exclusively from boss chests** (`KIND.chest` drop on `C.Boss` death → pickup calls
`world:open_chest()` → the ChestOpen mailbox triggers one offer round for EVERYONE — the co-op
fairness fix); **chest objects roll UNIFORMLY** (every object same rate — the old rarity-first
roll made epic/legendary objects near-mythical; the card frame shows the object's home tier),
level-up upgrades keep the weighted rarity-first roll with down-then-up tier fallback;
**Crystal Ball** = +1 card, **Lucky Clover** `C.Luck` tilts tier weights — upgrades only now)
with **5 rarity tiers** (grey/green/blue/purple/gold); the **XP cost curve is Lua policy too**
(`mod:xp_curve`, linear engine fallback — core uses a QUADRATIC `5+3L+0.8L²`: linear cost vs
wave-growing income was a level-112-by-wave-32 runaway) — all content in `mods/core/` Lua
(**22 stat upgrades**, deliberately UNCAPPED (all-in builds are strategy; exceptions: Greed
stops at +150% (economy valve) and the Orbit lanes **Blade Dance/Extra Blade** hard-cap at
spin 6 rad/s / 8 blades (past that a blade's per-tick travel outruns its hit radius) — both
gated on owning the Blades like AOE Zone) incl. Piercing Rounds (`hit_cd` guards double-hits),
Ricochet re-aim, Magnet `C.Magnet`, Leech kill-heal riding bullets, **Heavy Caliber**
(`Weapon.bullet_radius` — fatter hitbox), **Reaper** (`Weapon.cull`: hits that leave NON-boss
trash under the threshold execute it; explicitly skips `C.Boss`), **Volatile Rounds**
(`Weapon.volatile`: killing bullets burst 6 friendly bullets at the victim; burst carries
volatile 0 — no chains) + **22 objects**:
**Onion** (aura dps = `per_second + 0.6×Weapon.damage` — scales with the build, credits the
scoreboard)/**Frost Belt**/**Shockwave Dash**/**Crystal Ball**/**Orbiting Blades** (`C.Orbit`,
server-spun networked phase = client draw = hitbox; `orbit_sys` at 60 Hz — 20 Hz stepped
visibly under 60 Hz snapshots; cut dps = `dps + 0.7×Weapon.damage`, radius 95)/**Spiked
Armor**/**Phoenix Feather** (cheat
death once)/**Lucky Clover**/**Adrenaline Core** (post-dash fire-rate)/**Split Barrel** (+2
bullets)/**Mirror Barrel** (`Weapon.mirror`: the volley fires backward too)/**Static Charge**
(`C.Static`: periodic friendly ring, damage = 50% of live Weapon.damage)/**Hunter's Instinct**
(`C.Hunter`: kills refund 0.5 s dash cooldown — hooked in the `credit` path so bullets/orbit/
aura/dash kills all count)/**Reactive Plating** (`C.Reactive`: losing a heart bursts a friendly
ring — hooked in `hurt_player`, THE single player-damage site) · CURSED objects
show their price (**Auto Target** = auto-aim −30% DMG, **Berserker Sigil** +60% DMG −1 heart,
**Glass Cannon** ×2 DMG at 1 heart, **Lead Plates** +2 hearts −25% SPD, **Heavy Rounds** ×2
DMG but +55% fire cooldown −25% bullet speed) · IDENTITY objects
transform a playstyle and BLOCK an upgrade lane forever via its `available` (`C.Goliath` +3
hearts/speed clamped 200/no Swift Boots, `C.David` +54% fire rate/3 hearts max/no Vitality,
`C.Executioner` crit chance = 8%×pierce/no Keen Eye — the 2 Hz `identity_sys` enforces the live
rules)) · **heart life** (3 hearts, 1 s i-frames, rare heart drops heal; hearts only magnetize
toward HURT players, drops are SUPPRESSED when ≥3 `C.Heal` sit within 240 px (boss payouts
exempt) and unclaimed hearts FADE after `C.Heal.ttl` 25 s (`heal_decay`) — no banked reserve
trains or heart carpets) · **dash on LSHIFT**
(predicted; Shockwave makes it damage) · **crit** · co-op **downed → proximity revive** (3 s arc
via `mod:draw`+`ctx:arc`, boss kills revive everyone) · **off-screen pointers on a fixed-radius
ring around the player** (NOT screen-edge — user call): teammates green/red-when-down + name,
arena boss + loot chest gold ·
**floating damage numbers**: Lua damage sites call `world:damage_number(x,y,amount,kind)` →
ModState buffer → ONE unreliable `DamageEvents` packet per snapshot tick (cap 48) → client
FloatNum pool (rise+fade 0.8 s, crits gold/bigger; per-HIT only — aura DoT ticks stay silent) ·
**boss bullet identity**: `Render.variant` 4 blood bolt / 5 pellet-petal (also the Mimic King's
coins + fool's-gold bites) / 6 pulsing seed (despawn
= green pop + `pop` sound) / 7 lance / 8 frog spit / 9 electric zap (Static Charge + Reactive
Plating rings, flickering cyan) — variants 4/7 draw ORIENTED along motion
(trail squares behind the head, direction from the interp delta); synthesized `shoot_4..9.wav`
cast sounds ride the existing `shoot_<variant>` hook ·
**damage feedback**: losing a heart flashes a red EDGE VIGNETTE (4 gradient quads via
`SDL_RenderGeometry`) + at exactly 1 heart it pulses continuously with a `heartbeat` thump
(1.2 s period, update-driven) · **minimap radar** (top-right 180 px, `draw_minimap`): built from
the render registry — teammates green/red, enemy density dots in radar reach (~1.2k px, cap
220), boss-sized archetypes (def scale ≥ 2) + chests CLAMP to the rim in gold, arena rect
drawn when active · **level-up cards ANIMATE** (scene-local `age_` clock: staggered deal-in
ease, hover grow +6% (hitbox stays the base rect) + border brighten + `click`, pick = white
flash beat THEN send+pop — input ignored during the beat) ·
**compact stats HUD**: the panel shows vitals only (wave/level/XP bar, hearts, dash, downed)
until **CTRL is held** — `hud.detail` on the HudContext, fed by `SDL_GetModState` DELIBERATELY
(not the event key-set: the panel redraws over the level-up cards where GameScene's events are
blocked, and card picks are exactly when stats matter); core's mod.lua gates the weapon/crit/
magnet/greed/leech block on it ·
**game-over** (all downed = defeat; frozen-world overlay; per-player SCOREBOARD from RunStats
(kills/dmg/revives; `C.Bullet.owner` = `p:id()` stamps kill credit) + local bests file
(`SDL_GetPrefPath` records.txt: best wave/wins/runs, "NEW BEST!" flash); host returns everyone
to the lobby) · **ESC pause menu** (PauseScene: volume sliders (`Gui::slider`), host-only
run pause toggle via `Command::Pause/Resume`, DISCONNECT → `reset_to_connect()`; update returns
Continue — the co-op world keeps running beneath, only input is modal) · **F11** fullscreen ↔
windowed toggle (Engine::on_event, before scenes) ·
TAB console: `/pause` `/resume` + **mod commands** (`mod:command` — `/givexp` `/heal` `/wave`
`/stress` `/upgrade <n>` `/object <n>` — the last two file `OfferGrant` mailbox notes via
`world:grant_offer(kind)`; the server runs ONE per round so N notes = N sequential menus,
shared `begin_offer_round` with level-ups/chests) with TAB-completion + history · **audio**: full SFX set + lobby/game/boss music (auto
cross-fade; `assets/sound/` canonical names, all client-side triggers off snapshot state; local
`/volume` `/sfx` `/music` verbs; mods rebind any sound via `mod:sound` and fire their own with
`ctx:play/play_at`) · **headless sim test**: `tests/sim_test.cpp` (89 checks over the full
mods/core pipeline incl. chest rounds, offer filtering, xp curve, co-op scaling, identity
objects, RunStats attribution, damage-number queue, boss bullet variants, brain variety/
phases/forced moves, the 25/35/45 kits, armor/ambush/homing archetypes, the new object/
upgrade lanes and the weapon-scaled aura/blades; build line in its header — needs
`-freflection -fcontracts` + the sol2
defines; LuaHost must outlive the World. Boss scenarios fire PARKED mechanics
deterministically by zeroing `comp.timer` (a brain poke) or force a brain move via
`brain.move = i; brain.winding = 0.05` — never wait on autonomous picks, they're jittered;
one-shot state checks that a spawn pool or drop roll can erase (bomber elites self-detonate)
must POLL per sim tick, not read once at window end).

## Dev workflow & gotchas
- Build: `cmake -S . -B build && cmake --build build -j 1` → `bin/client`, `bin/server`.
  **Use `-j 1` (or at most 2): parallel sol2-heavy TUs OOM-kill this machine.** Adding a new
  `.cpp` requires re-running `cmake -S . -B build` (GLOB re-scan).
  **Never benchmark or play a Debug build** — the sol2/Lua sim is 10-50x slower at -O0 and blows
  the tick budget by itself. The top-level CMakeLists defaults to RelWithDebInfo; a stale cache
  can still pin Debug (check `grep CMAKE_BUILD_TYPE build/CMakeCache.txt`).
- Run: `./bin/server` then `./bin/client [host] [name]`; host presses **ENTER** in the lobby. Client
  is **fullscreen** and loads assets by **relative path** → run from the repo root. Assets:
  `assets/sprite/{player,enemy}.png`, `assets/background.png`, optional `assets/ui/card_*.png`;
  widget-kit sprites in `assets/ui/` (panel/button/pill/hearts, 9-slice metrics hardcoded in
  gui.cpp) + `assets/font/PressStart2P.ttf` (OFL); `assets/sound/` (canonical names, modding.md).
  **Animation packs**: `assets/sprite/<Pack>/` folders of `<Clip>_<N>x1.png` horizontal strips
  (enemy packs have Idle+Move, all face RIGHT + flip). A **directional player pack** names clips
  `<State>_<Dir8>` (states Idle/Move/Shoot/MoveShoot/Dash/Death; 8 compass dirs, pure Left/Right
  optional → Down-diagonal substitutes) + optional `Shadow_1x1.png`; the engine picks state (by
  aim/movement/dash/downed) and 8-way direction — no flip. An enemy's `sprite` opt or
  `mod:player_sprite(path)` names the folder — `client/sprites.hpp` handles frames/state/facing/
  play-once; Lua never animates. (`assets/sprite/player/_unused/` holds the raw source sheets +
  unused weapon/dust/shadow variants — inert; the loader scans top-level strips only.)
- **Testing without a display:**
  - Gameplay logic → standalone sim test, SDL-free: `g++ -std=c++26 -I src test.cpp` (build a World,
    step it, assert on components).
  - Networked flow → scripted bot using `net`+`protocol`:
    `g++ -std=c++26 -I src -I build/_deps/enet-src/include t.cpp src/shared/net/net.cpp build/_deps/enet-build/libenet.a`.
  - Client smoke → `SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software SDL_AUDIO_DRIVER=dummy ./bin/client …`.
- **Kill leftover procs with `pkill -x server` / `pkill -x client`** — `pkill -f bin/server` also
  matches (and kills) the invoking shell.

## Known-next / deferred
- Unlockable starting loadouts / lobby character select, gated on the run-stats records (the
  "stats first, unlocks later" plan — RunStats + local bests shipped). (F11 + XP magnet shipped.)

## Coding standards
- **DoD**: components are POD; systems are flat `view<...>().each` loops; tags are empty structs.
- Keep simulation SDL-free (see Layering). RAII wrappers (`unique_ptr` + custom deleters, guard
  types) for SDL/ENet; no owning raw pointers; `std::expected`/`std::optional` over exceptions.
- Prefer data tables over branches for balance/variation. Self-documenting code, concise comments on
  *why* (perf/arch). Match the surrounding style.
