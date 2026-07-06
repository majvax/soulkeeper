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
    sprites.hpp             # SpritePacks: auto-discovers <Clip>_<N>x1.png strip folders; draw_clip
                            #   (frame slicing, Idle/Move OR 8-way <State>_<Dir8> directional packs,
                            #   right-facing flip, play-once) — Lua only names the folder
    mod/render_bindings.*   # render VM bindings: draw ctx (texture/rect/circle/text) + player view
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
  slotted into named phases between the kernel systems. Snapshots broadcast at 60 Hz carry
  `Render{kind,variant}` bytes (Lua-controlled visuals); entries are **packed + quantized**
  (int16 half-px offsets from a player-centroid origin in the header — 14 B/entity, cap 500
  enemies), followed by a small **PlayerAim trailer** (≤4 × 7 B: authoritative aim dir +
  firing bit per player, so the client drives sprite facing/shoot-pose from the SIM's aim — a
  server-side override like autofire shows correctly — not the local mouse). Clients **predict**
  the local player (same `apply_input`/`tick_dash`, corrected by snapshots) and **interpolate**
  remotes. `Session` on the client mirrors control state; `GameScene` keeps a render-only `Registry`.
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
Lobby + host-start + reconnect · waves (15s) with Lua-defined archetypes
**Bandit/Scout/Mushroom/Brute/Slinger/Slasher/Vampire** (per-wave spawn weights, each with its own
**animated sprite pack** — `mods/core/enemies.lua`; Slinger + Vampire stand off and fire
**hostile projectiles** via Lua `core:ranged` systems; the player is the animated Knight via
`mod:player_sprite`) · manual-aim projectiles · **XP orbs → shared team level pool → synchronized level-up
card scene** with **5 rarity tiers** (grey/green/blue/purple/gold; rarity-first roll, per-tier
amounts, objects declare their tier) — all content in `mods/core/` Lua (13 stat upgrades incl.
crit/dash lines + **Onion**/**Frost Belt**/**Shockwave Dash** objects) · **heart life** (3 hearts,
1 s i-frames, rare heart drops heal; Vitality raises the cap only) · enemies **scale per wave**
(Lua stats fn) · **dash on LSHIFT** (predicted client-side; Shockwave object makes it damage) ·
**crit** (chance/multiplier; crit bullets render orange) · co-op **downed → respawn a few waves
later** · `/pause` `/resume` console.

## Dev workflow & gotchas
- Build: `cmake -S . -B build && cmake --build build -j 1` → `bin/client`, `bin/server`.
  **Use `-j 1` (or at most 2): parallel sol2-heavy TUs OOM-kill this machine.** Adding a new
  `.cpp` requires re-running `cmake -S . -B build` (GLOB re-scan).
  **Never benchmark or play a Debug build** — the sol2/Lua sim is 10-50x slower at -O0 and blows
  the tick budget by itself. The top-level CMakeLists defaults to RelWithDebInfo; a stale cache
  can still pin Debug (check `grep CMAKE_BUILD_TYPE build/CMakeCache.txt`).
- Run: `./bin/server` then `./bin/client [host] [name]`; host presses **ENTER** in the lobby. Client
  is **fullscreen** and loads assets by **relative path** → run from the repo root. Assets:
  `assets/sprite/{player,enemy}.png`, `assets/background.png`, optional `assets/ui/card_*.png`.
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
  - Client smoke → `SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software ./bin/client …`.
- **Kill leftover procs with `pkill -x server` / `pkill -x client`** — `pkill -f bin/server` also
  matches (and kills) the invoking shell.

## Known-next / deferred
- Game-over/win, XP magnet, delta/quantized snapshots, F11 fullscreen toggle.

## Coding standards
- **DoD**: components are POD; systems are flat `view<...>().each` loops; tags are empty structs.
- Keep simulation SDL-free (see Layering). RAII wrappers (`unique_ptr` + custom deleters, guard
  types) for SDL/ENet; no owning raw pointers; `std::expected`/`std::optional` over exceptions.
- Prefer data tables over branches for balance/variation. Self-documenting code, concise comments on
  *why* (perf/arch). Match the surrounding style.
