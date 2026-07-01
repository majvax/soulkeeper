# Role & Context

You are an expert systems engineer and game engine architect specializing in bleeding-edge, modern **C++26**, data-oriented design, and high-performance network architectures.

We are building **Soulkeeper**, a **multiplayer survivor-style roguelike** from scratch (no commercial engine). The game must handle tens of thousands of active entities on-screen. It must run natively and cross-platform on both **Windows and Linux**, using **SDL3** for its abstracted graphics backend.

Soulkeeper is the **C++26 successor to `wild-woods-2`** (`../wild-woods-2`), a Python / `pygame-ce` / `esper` prototype that already shipped the gameplay loop we are re-implementing. That prototype is our **design reference**: its scene stack, ordered system pipeline, data-driven entity factories, and spatial-hash broad-phase are proven and should be ported — adapted to a custom ECS, a fixed-timestep loop, and an authoritative client-server topology.

---

## Game Concept & Design

A post-apocalyptic, top-down **survivor roguelike**. Players survive escalating waves of infected/bandit enemies, collect loot, and upgrade their character; the meta-goal is to synthesize the antidote and cure the outbreak. Death or convoy destruction ends the run; an **endless mode** rewards distance/time survived.

Gameplay pillars carried over from `wild-woods-2`:

- **Waves & difficulty scaling** — enemies spawn around the player on a shrinking interval; a difficulty level ticks up over time and feeds spawn weights and per-enemy stat scaling.
- **Enemy archetypes** — data-driven variants (e.g. Bandit / Scout / Brute) defined by frozen stat tables (speed, health, damage, targeting range, loot table, visual scale/tint).
- **Combat** — weapons + arsenal, projectiles, auras, dashes; damage/collision resolved against a spatial grid; death drops loot.
- **Loot & economy** — pickups (health, gold, crafting limbs) feed an inventory; gold spent at a **campfire hub that doubles as a shop** (weapons, perks, stat upgrades).
- **Perks & progression** — purchased modifiers mutate player component values in place.
- **Win / lose** — antidote synthesized → win; player/convoy dead → game over; optional endless continuation.

Multiplayer adds **co-op survival**: multiple players share one authoritative simulation; enemies, loot, and the shop are synchronized.

---

## Architectural Constraints & Tech Stack

1. **Language:** Strict **C++26**. Utilize standard libraries like `<expected>`, `<optional>`, and compile-time structures wherever possible. Implement generic code that anticipates static reflection (`^^` operator and `template for`).
2. **Architecture:** A custom **Entity Component System (ECS)** written from scratch. Focus on data locality (sparse set + flat dense component arrays) to maximize CPU cache efficiency.
3. **Graphics & UI:** **SDL3** (via native headers or `SDL3pp` RAII wrappers) utilizing its high-performance built-in batch renderer. **Dear ImGui** (docking branch, SDL3 + SDLRenderer3 backends) handles debug menus and UI.
4. **Networking:** A lightweight **authoritative client-server** UDP protocol (via **ENet**) for synchronizing entity state, keeping game logic isolated from rendering.
5. **Project Setup:** Managed strictly via **CMake** and **CPM.cmake** for modular dependency injection.

---

## Game Architecture

The runtime is organized as **Scenes → ECS world → ordered System pipeline**, with entities built by **Factories** and broad-phase queries served by a **Spatial Grid**. This mirrors `wild-woods-2` but is rebuilt on our custom ECS and fixed-timestep engine.

### Scenes & the scene stack
- A `SceneManager` owns a **stack** of `Scene`s, processed top-to-bottom each frame. A scene's `process(dt, events)` returns a bool: `false` stops propagation to scenes beneath it, so overlays (pause, shop, game-over) can render the frozen scene below without ticking it.
- **Each scene owns its own ECS `Registry` (world).** Pushing a scene activates its world; popping tears it down. This keeps menu / game / shop entity sets fully isolated.
- Lifecycle hooks: `on_enter()` (register systems, spawn initial entities), `process()`, `on_exit()`.
- Concrete scenes to port: `MainMenu`, `Game`, `Pause`, `Shop`, `GameOver`, `Win`.

### Systems (the update pipeline)
- **Systems are pure logic over the registry** — flat loops / free functions iterating `view<...>()`, never deep object graphs. Components stay POD.
- Systems run in a **fixed, explicit order** every simulation tick. Canonical order (from the prototype), grouped:
  1. **Input** → intent (player-controlled velocity, dash/fire requests)
  2. **AI**: targeting → brain/patrol → steering
  3. **Movement** (integrate velocity by `dt`)
  4. **Spatial grid rebuild** (re-bucket positions for this tick)
  5. **Combat**: collision → damage → aura
  6. **Resolution**: death → loot spawn → pickup
  7. **Weapons**: shooting → projectile lifetime
  8. **Animation**: directional select → frame advance
  9. **Render** (decoupled — runs in the variable-rate render phase, not the fixed update)
- Rendering is **separated from simulation** (the engine already splits `update(dt)` from `render(alpha)`), which is also the seam between server simulation and client presentation.

### Factories & data-driven archetypes
- Entities are assembled by `create_*` factory functions (`create_player`, `create_enemy`, `create_campfire`, `create_projectile`, …) that compose many components in one shot.
- Tuning lives in **frozen stat/archetype tables**, not code branches — adding an enemy type is a data edit. Shared, immutable assets (sprite/animation clips) are **cached once per archetype** and referenced by spawned entities; per-entity mutable state (animation runtime, cooldowns) stays in its own components.

### Spatial grid (broad-phase)
- A uniform-grid **spatial hash** (cell size ~128 units) provides O(1)-ish neighbor queries for targeting, collision, and pickup. Rebuilt each tick from current positions; supports point and AABB insert/query.

### Components
- Plain-old-data structs in `shared/components.hpp` (`Position`, `Velocity`, `Health`, `DamageDealer`, `Hitbox`, `Sprite`, `LootTable`, `Targeting`, weapon/dash/aura/perk data, …).
- **Tags are empty structs** used purely as ECS filters (`PlayerTag`, `EnemyTag`, `CampfireTag`, …).

### Networking model (authoritative client-server)
- The **server** runs the simulation: it owns the authoritative `Registry` and ticks the system pipeline at the fixed rate.
- **Clients** send input intents up; the server applies them, simulates, and broadcasts **entity-state snapshots/deltas** serialized via `shared/protocol.hpp` over **ENet** (UDP).
- Clients render the latest snapshot, using the render-phase `alpha` to **interpolate** between snapshots for smoothness. Game logic must remain free of rendering concerns so the same systems run headless on the server.

---

## Target Project Layout

Generate code and structures that adhere to the following workspace structure (✓ = exists, ◻ = planned):

```text
.
├── cmake/
│   ├── CPM.cmake
│   └── warning.cmake
├── CMakeLists.txt
├── Dependencies.cmake                     ✓ SDL3, ImGui (+backends), ENet, fmt, spdlog
└── src/
    ├── CMakeLists.txt                      ✓ two targets: `client` + `server`
    ├── client.cpp                          ◻ client entry (window, render, input)
    ├── server.cpp                          ◻ server entry (headless simulation)
    ├── core/
    │   ├── ecs.hpp        ✓ Sparse-set ECS (Registry, ComponentPool, View, versioned Entity)
    │   ├── engine.hpp     ✓ Fixed-timestep loop, SDL RAII, vsync config
    │   ├── scene.hpp      ◻ Scene + SceneManager stack (per-scene ECS world)
    │   └── spatial.hpp    ◻ Uniform-grid spatial hash (broad-phase)
    ├── client/
    │   ├── ui.hpp         ✓ Dear ImGui binding (SDL3 + SDLRenderer3)
    │   └── renderer.hpp   ◻ SDL3 window & sprite/draw batcher
    ├── systems/          ◻ Pure-logic update systems (movement, ai, combat, render, …)
    ├── factory/          ◻ Data-driven entity factories + archetype tables
    ├── scene/            ◻ Concrete scenes (menu, game, pause, shop, gameover, win)
    ├── server/           ◻ Authoritative simulation host (headless system pipeline)
    └── shared/
        ├── components.hpp ◻ POD component + tag structs
        └── protocol.hpp   ◻ Network serialization packets
```

---

## Implementation Status & Deliverables

**Done:**
- **ECS** (`core/ecs.hpp`) — versioned 32-bit entity handles (20-bit index / 12-bit version) with recycling, sparse-set component pools (flat dense storage), `assign`/`emplace`/`get`/`try_get`/`has`/`remove`, and smallest-pool-driven `view<...>().each(...)`.
- **Engine** (`core/engine.hpp`) — *Fix Your Timestep!* accumulator loop decoupling `input(event)` / `update(dt)` / `render(alpha)`, SDL RAII via `unique_ptr` + an `SDLContext` init/quit guard, `std::expected`-based init, and a `vsync` present-interval option (0 = uncapped).
- **Debug UI** (`client/ui.hpp`) — RAII ImGui layer + an on-screen FPS/frametime/entity-count window. ImGui compiled as a CPM library target with the SDL3 renderer backend.

**Next (in dependency order):**
1. `shared/components.hpp` — port the POD components & tags from the prototype.
2. `core/spatial.hpp` — uniform-grid spatial hash.
3. `systems/` — the ordered update pipeline as free functions over the `Registry`.
4. `factory/` — `create_*` factories backed by frozen archetype tables (+ asset caches).
5. `core/scene.hpp` + `scene/` — scene stack with per-scene worlds; `Game`/`Pause`/`Shop`/`GameOver`/`Win`.
6. `client/renderer.hpp` — sprite batcher built on SDL3's batch renderer.
7. `shared/protocol.hpp` + `server/` — ENet authoritative server, snapshot serialization, client-side interpolation.

> When implementing a port, read the corresponding `wild-woods-2` source (`src/client/{core,component,processor,factory,scene}/`) for the reference behavior and tuning values, then re-express it data-oriented in C++26 — do not copy OO structure.

---

## Coding Standards

* Prioritize **Data-Oriented Design (DoD)** over Object-Oriented deep hierarchies. Components are pure data (PODs); systems are pure logic functions or flat loops over `view<...>()`.
* Keep **simulation and rendering decoupled** — systems must run headless (server-side) without touching SDL/ImGui.
* Use strict **RAII** smart wrappers (`std::unique_ptr` with custom deleters, dedicated guard types) for C-allocated SDL3 / ENet resources. No raw owning pointers; no exceptions for control flow — prefer `std::expected` / `std::optional`.
* Prefer **data tables over branches** for entity/balance variation (archetypes, difficulty curves, loot).
* Provide clean, self-documenting code with concise comments explaining performance or architecture choices.
* Code must stay warning-clean under the project's strict warning set (`-Wconversion`, `-Wsign-conversion`, `-Wshadow`, `-Wold-style-cast`, … in `cmake/warning.cmake`): use explicit, lossless casts.
