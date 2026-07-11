# Soulkeeper

A **multiplayer survivor-style roguelike** built from scratch in modern **C++26** — no commercial engine. Survive escalating waves in a post-apocalyptic world, loot and upgrade between fights at the campfire, and race to synthesize the antidote before the convoy falls.

Soulkeeper is the C++26 successor to [`wild-woods-2`](../wild-woods-2), a Python / `pygame-ce` / `esper` prototype. It keeps that game's proven architecture — a scene stack, an ordered system pipeline, data-driven entity factories, and a spatial-hash broad-phase — and rebuilds it on a custom cache-friendly ECS, a fixed-timestep engine, and an authoritative client-server network model.

## Features & goals

- **Custom ECS** — sparse-set component storage with flat dense arrays for cache efficiency; versioned entity handles with recycling.
- **Fixed-timestep simulation** — deterministic `update(dt)` decoupled from variable-rate `render(alpha)` with state interpolation (*Fix Your Timestep!* accumulator).
- **Data-oriented gameplay** — POD components, pure-logic systems, archetype/balance tuning in data tables.
- **Survivor loop** — waves, enemy archetypes, weapons/perks/dash/auras, loot & economy, a campfire shop hub, difficulty scaling, win (antidote) / game-over, and an endless mode.
- **Authoritative multiplayer** — a headless server runs the simulation; clients send input and render interpolated snapshots over ENet (UDP).
- **Cross-platform** — native Windows + Linux via SDL3; Dear ImGui for debug tooling.

## Tech stack

| Concern      | Choice                                             |
|--------------|----------------------------------------------------|
| Language     | C++26                                              |
| Graphics     | SDL3 (built-in batch renderer)                     |
| Debug UI     | Dear ImGui (docking branch, SDL3 backend)          |
| Networking   | ENet (UDP, authoritative client-server)            |
| Math         | GLM                                                |
| Logging      | spdlog / fmt                                        |
| Build        | CMake (≥ 4.3) + CPM.cmake (dependencies auto-fetched) |

## Building

Requirements: a **C++26** compiler (GCC ≥ 15, Clang ≥ 19, or MSVC v19.4x), **CMake ≥ 4.3**, Git, and a network connection on first configure (CPM downloads dependencies).

```bash
cmake -S . -B build
cmake --build build
```

This produces two executables in `bin/`:

- `bin/client` — the game client (window, rendering, input, debug UI)
- `bin/server` — the headless authoritative simulation host

> **Linux/Wayland note:** if the window opens at the wrong size, force the X11 backend:
> ```bash
> SDL_VIDEODRIVER=x11 ./bin/client
> ```

## Running

```bash
./bin/server     # start the simulation host
./bin/client     # connect and play
```

**Controls:**

| Key            | Action                                  |
|----------------|-----------------------------------------|
| `Z Q S D`      | Move                                    |
| Mouse          | Aim                                     |
| Left click     | Fire (hold)                             |
| `Left Shift`   | Dash                                    |
| `Tab`          | Console (`/pause`, `/wave`, `/givexp`…) |
| `Enter`        | Start the run (host, in the lobby)      |
| `Esc`          | Menu in-game (volumes, pause, leave); quit on the entry screens |
| `F11`          | Toggle fullscreen / windowed            |

The debug overlay shows live FPS, frame time, and entity count.

## Architecture

The runtime is organized as **Scenes → ECS world → ordered System pipeline**, with entities assembled by **Factories** and neighbor queries served by a **Spatial Grid**. Simulation is kept strictly separate from rendering so the same systems run headless on the server.

```text
src/
├── client.cpp        # client entry point (window, render, input)
├── server.cpp        # server entry point (headless simulation)
├── core/
│   ├── ecs.hpp       # sparse-set ECS: Registry, ComponentPool, View
│   ├── engine.hpp    # fixed-timestep loop, SDL RAII, vsync
│   ├── scene.hpp     # scene stack (per-scene ECS world)   [planned]
│   └── spatial.hpp   # uniform-grid spatial hash           [planned]
├── client/           # renderer, ImGui UI
├── systems/          # pure-logic update systems           [planned]
├── factory/          # data-driven entity factories        [planned]
├── scene/            # menu, game, pause, shop, gameover    [planned]
├── server/           # authoritative host                  [planned]
└── shared/
    ├── components.hpp # POD components & tags               [planned]
    └── protocol.hpp   # network packets                     [planned]
```

The full engineering specification — system ordering, networking model, factory/archetype conventions, and coding standards — lives in [`CLAUDE.md`](CLAUDE.md).

## Status

Early development. **Done:** custom ECS, fixed-timestep engine with interpolation, SDL3 bootstrap with RAII, ImGui debug overlay. **Next:** components → spatial grid → system pipeline → factories → scene stack → renderer → networking.

## Credits

Successor to **wild-woods-2** by Guillaume Dehez, Hugo Viard-Crétat, and Enzo Ballandras. Built with SDL3, Dear ImGui, ENet, GLM, spdlog, and CPM.cmake.
