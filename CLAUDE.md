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
                   #   progression(GameStats{xp,wave}, Downed{respawn_wave})
    system/        #   KERNEL systems only: grid (spatial rebuild), dash, movement
                   #   + input.hpp (apply_input, start_dash/tick_dash shared w/ prediction, PLAYER_SPEED, DASH_*)
    factory/       #   create_player / create_enemy — kernel parts only (loadout/stats come from Lua)
    sim/           #   World (Registry+SystemManager); make_game_world() = kernel pipeline + singletons;
                   #   world.hpp phase constants (grid/targeting/motion/shooting/movement/projectile/combat/update/pickup/death)
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
                            #   panels/buttons/inputs/hearts from assets/ui/*.png (BlackAndWhite
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
    scene/{lobby,game,console,level_up}.hpp
  client.cpp       # ~15-line bootstrap: argv host/name -> Engine::create -> run()
mods/core/         # ALL built-in content as a Lua plugin (upgrades, objects, components, systems)
types/kernel.lua   # lua-language-server stubs for the ENGINE mod API (editor autocomplete);
                   # a mod's own types go in mods/<name>/types.lua (component shapes inferred inline)
modding.md         # the full modding guide (API reference, performance model, internals)
```

## Runtime model (how it actually works)
- **Server-authoritative; C++ is the engine, Lua is the game.** The kernel pipeline is
  `Grid → Dash → Movement` at 120 Hz; **every game rule** (targeting, shooting, bullets, contact
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
`volley/spread/variant/bullet_radius` — Ent fans 3, Cyclop snipes a heavy variant-3 bullet),
**Slasher**, wave-banded **Brute/Silverback/Goldhorn** Rhino tiers · HP scaling is **compound
+7%/wave** · manual-aim projectiles with **knockback** (`Weapon.knockback` rides each bullet) ·
**scripted boss ladder to wave 50** (`MILESTONES` table: **@5 Rhino Charger** (boss-scale
`C.Lunge` charge) · **@10 Frog King** (nova rings + **rage phases**: <50% HP faster/denser novas
+ aimed volley, <25% speed-up) · **@15 Frog Prince** (`C.Lunge` as a LEAP + `burst` landing-slam
bullet ring, FrogMonster Jump clips) · **@20 Bomb Lord** (`C.Planter` carpets the arena with
`core:mine` Powder Kegs — stationary shootable `C.Bomber` enemies that zone the floor — + aimed
volleys + novas) · **@30 Vampire Lord** (fast novas + `C.Summon{pool=2}` bat swarms + `C.Regen`
self-heal DPS check) · **@40 Elder Ent** (`C.Nova.spin` ROTATING spiral rings + wide fans +
`core:bramble` pods) · **@50 GAME MASTER finale = the WIN** (`WIN_WAVE=50`; Speed 0 + blink,
spiral raging novas, ELITE summons `pool=3`, heavy aimed 5-volleys); 25/35/45 rotate the minis;
every %10 wave is an ARENA (1920×1080 rect = `arena {960,540}` opt + matching `C.Nova.arena_w/h`,
`WaveHold` freezes the wave clock); all carry `C.Boss` — death system keys **loot CHEST** +
guaranteed drops/team-revive/win on it; client draws a **top-center boss HP bar** + shakes on
boss fx) · client **fx vocabulary**: `Render.fx` 1 = ATK once, 2 = charge loop, 3 = telegraph
held (`sprites.hpp` discovers ATK/Fire, Charge_RunLoop/Dash/Jump_Full, Prepare/Charge_Begin
clips) · **XP orbs → shared team level pool → synchronized offer scene** (LevelUp msg carries an
`OfferFlavor` byte: level vs chest — the scene titles TREASURE for chests): the offer is rolled
IN LUA (`mod:level_offer(player, level, ctx)` + `world:offerable` entries carry
`kind = upgrade|object`; core rolls **upgrades-only on level-ups, objects-only on chests** —
**objects come exclusively from boss chests** (`KIND.chest` drop on `C.Boss` death → pickup calls
`world:open_chest()` → the ChestOpen mailbox triggers one offer round for EVERYONE — the co-op
fairness fix); rarity-first roll falls back down then UP a tier (an epic-only object pool must
fill on a common roll); **Crystal Ball** = +1 card, **Lucky Clover** `C.Luck` tilts tier weights)
with **5 rarity tiers** (grey/green/blue/purple/gold) — all content in `mods/core/` Lua (**20
stat upgrades** incl. Split Shot `projectiles` fan, Piercing Rounds (`hit_cd` guards
double-hits), Ricochet re-aim, Heavy Impact, Magnet `C.Magnet`, Leech kill-heal riding bullets,
Greed per-player XP mult + **10 objects**:
**Onion**/**Frost Belt**/**Shockwave Dash**/**Auto Target**/**Crystal Ball**/**Orbiting Blades**
(`C.Orbit`, server-spun networked phase = client draw = hitbox)/**Spiked
Armor**/**Phoenix Feather** (cheat death once)/**Lucky Clover**/**Adrenaline Core** (post-dash
fire-rate)) · **heart life** (3 hearts, 1 s i-frames, rare heart drops heal) · **dash on LSHIFT**
(predicted; Shockwave makes it damage) · **crit** · co-op **downed → proximity revive** (3 s arc
via `mod:draw`+`ctx:arc`, boss kills revive everyone; red edge arrows point at downed teammates) ·
**game-over** (all downed = defeat; frozen-world overlay, host returns everyone to the lobby) ·
TAB console: `/pause` `/resume` + **mod commands** (`mod:command` — `/givexp` `/heal` `/wave`
`/stress`) with TAB-completion + history · **audio**: full SFX set + lobby/game/boss music (auto
cross-fade; `assets/sound/` canonical names, all client-side triggers off snapshot state; local
`/volume` `/sfx` `/music` verbs; mods rebind any sound via `mod:sound` and fire their own with
`ctx:play/play_at`) · **headless sim test**: `tests/sim_test.cpp` (29 scenarios over the full
mods/core pipeline incl. the chest round + offer filtering; build line in its header — needs
`-freflection -fcontracts` + the sol2 defines; LuaHost must outlive the World).

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
- F11 fullscreen toggle. (XP magnet shipped: base pull + boss-kill vacuum + Magnet upgrade.)

## Coding standards
- **DoD**: components are POD; systems are flat `view<...>().each` loops; tags are empty structs.
- Keep simulation SDL-free (see Layering). RAII wrappers (`unique_ptr` + custom deleters, guard
  types) for SDL/ENet; no owning raw pointers; `std::expected`/`std::optional` over exceptions.
- Prefer data tables over branches for balance/variation. Self-documenting code, concise comments on
  *why* (perf/arch). Match the surrounding style.
