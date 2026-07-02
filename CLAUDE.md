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
    components/    #   physics(Position,PrevPosition,Velocity,Speed) combat(Health,Radius,Damage,Aura,
                   #   Weapon,AimState,Projectile,Lifetime,EnemyTag,EnemyType,Archetype) gameplay(PlayerTag)
                   #   progression(GameStats{xp,wave}, XpOrb, Downed{respawn_wave})
    system/        #   pure systems: targeting, shooting, movement, projectile, combat, pickup, death
                   #   + input.hpp (apply_input(vel,mx,my,speed) helper, PLAYER_SPEED)
    factory/       #   create_player / create_enemy(x,y,EnemyType) / create_projectile / create_xp_orb
    sim/           #   World (Registry+SystemManager); make_game_world() registers the pipeline + GameStats singleton
    mod/           #   Lua modding layer (SDL-free): registry (ContentDef/ContentRegistry, string-id ->
                   #   deterministic wire-id), lua_host (sol::state, register_mod, mods/*/mod.lua discovery),
                   #   events (bus), script_ecs (Lua components + net sync), sim_bindings (entity/world API)
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
    mod/render_bindings.*   # render VM bindings: draw ctx (texture/rect/circle/text) + player view
    scene/{lobby,game,console,level_up}.hpp
  client.cpp       # ~15-line bootstrap: argv host/name -> Engine::create -> run()
mods/core/         # ALL built-in content as a Lua plugin (upgrades, objects, components, systems)
types/library.lua  # lua-language-server stubs for the mod API (editor autocomplete)
modding.md         # the full modding guide (API reference, performance model, internals)
```

## Runtime model (how it actually works)
- **Server-authoritative.** Server owns the World + runs the system pipeline
  (`Targeting → Shooting → Movement → Projectile → Combat → Pickup → Death`) at 120 Hz, broadcasts
  full snapshots at 60 Hz. Clients **predict** the local player (same `apply_input`, corrected by
  snapshots) and **interpolate** remotes. `Session` on the client mirrors control state; `GameScene`
  keeps a render-only `Registry`.
- **Scenes self-drive transitions** via `engine_->scenes()` (deferred push/pop, applied at safe
  points): Lobby→Game on `Playing`; GameScene TAB pushes Console (pops itself); GameScene pushes
  LevelUp on level-up (pops itself). The Engine only hardcodes the initial `LobbyScene`.
- **Input is event-driven** — `GameScene::handle_event` maintains a held-key set (WASD/Z-Q-S-D +
  LMB); a scene above returning **Stop** naturally blocks it. **Do NOT** use `SDL_GetKeyboardState`
  or `io.WantCaptureKeyboard` for gameplay input (that bypasses the scene stack).
- `Engine::width()/height()` return the **real render output size** (`SDL_GetRenderOutputSize`), not
  the config size — UI must center against these (fullscreen / any aspect ratio).
- Reconnect identity = `token = hash(player name)`; a dropped player's entity is kept and resumed.

## Modding layer (content = Lua plugins)
Upgrades, objects, and even components/systems are **Lua 5.4 plugins** (sol2), not C++ — see
`modding.md` (full guide), `mods/core/` (all built-in content), `types/library.lua` (LSP stubs).
- **Two VMs, same `mod.lua`**: the server runs the **sim VM** (apply/acquire/available, events,
  Lua-defined systems), the client the **render VM** (draw hooks + card metadata). Each side only
  fires its own callbacks.
- **Deterministic wire ids** = lexicographic sort index of namespaced string ids (`core:damage`).
  No runtime counter → identical on every process, but the `mods/` set **must match** across
  machines (join-time validation not yet implemented).
- **Perf model**: continuous effects are Lua-defined components ticked by Lua-defined systems
  (dynamic pools live in `core::Registry`; `networked` components ride snapshots); event callbacks
  fire only on discrete moments (level-up roll, pick, game events).
- Every Lua callback is a protected call — a broken mod logs and is skipped, never crashes.

## Gameplay implemented
Lobby + host-start + reconnect · waves (15s) with archetypes **Bandit/Scout/Brute** (tint+scale on
one sprite) · manual-aim projectiles · **XP orbs → shared team level pool → synchronized level-up
card scene** with **rarity** upgrades — all defined in `mods/core/` Lua (stat upgrades + **Onion**
aura / **Frost Belt** objects with Lua draw hooks) · co-op **downed → respawn a few waves later** ·
`/pause` `/resume` console.

## Dev workflow & gotchas
- Build: `cmake -S . -B build && cmake --build build` → `bin/client`, `bin/server`. **Adding a new
  `.cpp` requires re-running `cmake -S . -B build`** (GLOB re-scan).
- Run: `./bin/server` then `./bin/client [host] [name]`; host presses **ENTER** in the lobby. Client
  is **fullscreen** and loads assets by **relative path** → run from the repo root. Assets:
  `assets/sprite/{player,enemy}.png`, `assets/background.png`, optional `assets/ui/card_*.png`.
- **Testing without a display:**
  - Gameplay logic → standalone sim test, SDL-free: `g++ -std=c++26 -I src test.cpp` (build a World,
    step it, assert on components).
  - Networked flow → scripted bot using `net`+`protocol`:
    `g++ -std=c++26 -I src -I build/_deps/enet-src/include t.cpp src/shared/net/net.cpp build/_deps/enet-build/libenet.a`.
  - Client smoke → `SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software ./bin/client …`.
- **Kill leftover procs with `pkill -x server` / `pkill -x client`** — `pkill -f bin/server` also
  matches (and kills) the invoking shell.

## Known-next / deferred
- **`mod:add_enemy`** — enemy archetypes are the last hardcoded content (`enemy_stats()` switch in
  `factory/enemy.hpp`, spawn weights in `game_server.cpp`, tint/scale switch in `scene/game.hpp`);
  move them into `mods/core/` like upgrades/objects (promised in `modding.md` §3).
- **Plugin-set validation at join** — hash the sorted id list, reject mismatched clients.
- Later: game-over/win, XP magnet, weapon variety, delta/quantized snapshots, F11 fullscreen toggle.

## Coding standards
- **DoD**: components are POD; systems are flat `view<...>().each` loops; tags are empty structs.
- Keep simulation SDL-free (see Layering). RAII wrappers (`unique_ptr` + custom deleters, guard
  types) for SDL/ENet; no owning raw pointers; `std::expected`/`std::optional` over exceptions.
- Prefer data tables over branches for balance/variation. Self-documenting code, concise comments on
  *why* (perf/arch). Match the surrounding style.
