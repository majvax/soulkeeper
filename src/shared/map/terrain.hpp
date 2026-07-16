#pragma once
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Deterministic chunk-procedural terrain. Server and client run the SAME pure
// generator from the per-run seed (proto::StateMsg carries it), so static
// obstacles cost zero network traffic: collision (kernel + client prediction)
// and rendering both derive from these functions. Bullets fly OVER obstacles
// by design — they block movement, not shots (no cover camping).
namespace shared::map {

inline constexpr float chunk_size = 512.0f;

// Colliders. kind picks the ART CLASS; the client hashes the position for the
// concrete sprite variant, and scales the art to the collider (the sim never
// reads pixels). Trees collide on a small TRUNK circle — the canopy overhangs
// and the client's y-sort handles who draws in front.
struct Obstacle
{
    float x, y;
    float r;
    std::uint8_t kind; // 0 = tree (trunk), 1 = rock (footprint)
};

// Ground decoration (client-only use, but generated here so it stays seeded):
// kind 0 = plant/tuft, 1 = pebble, 2 = bush, 3 = stump.
struct Deco
{
    float x, y;
    std::uint8_t kind;
};

// SplitMix64: the one-liner PRNG behind every roll here. Pure function of
// (seed, chunk, salt) — the whole map IS this hash.
inline std::uint64_t mix(std::uint64_t v)
{
    v += 0x9e3779b97f4a7c15ULL;
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
    return v ^ (v >> 31);
}

inline std::uint64_t chunk_key(std::int32_t cx, std::int32_t cy)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32)
         | static_cast<std::uint32_t>(cy);
}

// n-th roll for a chunk, in [0, 1).
inline float roll(std::uint32_t seed, std::int32_t cx, std::int32_t cy, std::uint32_t salt)
{
    const std::uint64_t h = mix(chunk_key(cx, cy) ^ (static_cast<std::uint64_t>(seed) << 1)
                                ^ (static_cast<std::uint64_t>(salt) * 0xda942042e4dd58b5ULL));
    return static_cast<float>(h >> 40) / static_cast<float>(1ULL << 24);
}

// Smooth value noise in [0, 1): bilinear (smoothstepped) over a 1024 px
// lattice of hashed corners. Pure + cheap — the biome domain warp below and
// the client's ground shading both sample it.
inline float vnoise(std::uint32_t seed, float x, float y, std::uint32_t salt)
{
    const float gx = x / 1024.0f;
    const float gy = y / 1024.0f;
    const float fx = std::floor(gx);
    const float fy = std::floor(gy);
    const auto ix = static_cast<std::int32_t>(fx);
    const auto iy = static_cast<std::int32_t>(fy);
    const auto corner = [&](std::int32_t cx, std::int32_t cy) {
        const std::uint64_t h = mix(chunk_key(cx, cy) ^ (static_cast<std::uint64_t>(seed) << 5)
                                    ^ (static_cast<std::uint64_t>(salt) * 0xff51afd7ed558ccdULL));
        return static_cast<float>(h >> 40) / static_cast<float>(1ULL << 24);
    };
    float tx = gx - fx;
    float ty = gy - fy;
    tx = tx * tx * (3.0f - (2.0f * tx)); // smoothstep: no lattice-line creases
    ty = ty * ty * (3.0f - (2.0f * ty));
    const float top = corner(ix, iy) + ((corner(ix + 1, iy) - corner(ix, iy)) * tx);
    const float bot = corner(ix, iy + 1) + ((corner(ix + 1, iy + 1) - corner(ix, iy + 1)) * tx);
    return top + ((bot - top) * ty);
}

// Biome region id: 0 = PLAIN, 1 = FOREST, 2 = SNOW. Regions are LARGE
// (5120 px cells — dozens of seconds of travel between crossings) and
// DOMAIN-WARPED by value noise, so borders meander organically instead of
// tracing square cells. The client keys ground/flora on it; the generator
// keys obstacle density — shared, so sim collision always matches.
inline std::uint8_t biome_at(std::uint32_t seed, float x, float y)
{
    const float wx = x + ((vnoise(seed, x, y, 11) - 0.5f) * 1400.0f);
    const float wy = y + ((vnoise(seed, x, y, 13) - 0.5f) * 1400.0f);
    const auto bx = static_cast<std::int32_t>(std::floor(wx / 5120.0f));
    const auto by = static_cast<std::int32_t>(std::floor(wy / 5120.0f));
    return static_cast<std::uint8_t>(mix(chunk_key(bx, by) ^ seed) % 3U);
}

// Obstacles of one chunk. The spawn neighborhood (|cx|,|cy| <= 1 — players
// enter the world at the origin) stays empty. Density and the tree/rock mix
// follow the BIOME: forests are thick with trees, plains stay open kiting
// ground, snowfields lean rocky.
inline void obstacles_in(std::uint32_t seed, std::int32_t cx, std::int32_t cy,
                         std::vector<Obstacle>& out)
{
    if (cx >= -1 && cx <= 1 && cy >= -1 && cy <= 1) { return; }
    const float base_x = static_cast<float>(cx) * chunk_size;
    const float base_y = static_cast<float>(cy) * chunk_size;
    const std::uint8_t biome =
      biome_at(seed, base_x + (chunk_size * 0.5f), base_y + (chunk_size * 0.5f));
    const float density = roll(seed, cx, cy, 0);
    int count = 0;
    float tree_p = 0.5f;
    if (biome == 1) { // forest: 1-4, tree-heavy — it should read as WOODS
        count = density < 0.35f ? 1 : (density < 0.70f ? 2 : (density < 0.92f ? 3 : 4));
        tree_p = 0.85f;
    } else if (biome == 2) { // snow: 0-2, rock-leaning
        count = density < 0.35f ? 0 : (density < 0.85f ? 1 : 2);
        tree_p = 0.45f;
    } else { // plain: 0-1, open ground
        count = density < 0.55f ? 0 : 1;
        tree_p = 0.50f;
    }
    for (int i = 0; i < count; ++i) {
        const auto salt = static_cast<std::uint32_t>(1 + (i * 4));
        const bool tree = roll(seed, cx, cy, salt) < tree_p;
        out.push_back(Obstacle{
          .x = base_x + 40.0f + (roll(seed, cx, cy, salt + 1) * (chunk_size - 80.0f)),
          .y = base_y + 40.0f + (roll(seed, cx, cy, salt + 2) * (chunk_size - 80.0f)),
          .r = tree ? 9.0f + (roll(seed, cx, cy, salt + 3) * 3.0f)
                    : 13.0f + (roll(seed, cx, cy, salt + 3) * 7.0f),
          .kind = tree ? std::uint8_t{ 0 } : std::uint8_t{ 1 },
        });
    }
}

// Ground deco of one chunk (0..5 pieces; purely visual — no collision). Kind
// weights follow the biome: plains grow tufts, forests undergrowth, snow is
// pebbles and dead stumps.
inline void deco_in(std::uint32_t seed, std::int32_t cx, std::int32_t cy, std::vector<Deco>& out)
{
    const std::uint8_t biome =
      biome_at(seed, (static_cast<float>(cx) + 0.5f) * chunk_size,
               (static_cast<float>(cy) + 0.5f) * chunk_size);
    // Cumulative thresholds per biome: plant / pebble / bush / stump.
    const float* cut = nullptr;
    static constexpr float plain_cut[3] = { 0.50f, 0.80f, 0.92f };
    static constexpr float forest_cut[3] = { 0.40f, 0.50f, 0.85f };
    static constexpr float snow_cut[3] = { 0.15f, 0.70f, 0.75f };
    cut = biome == 1 ? forest_cut : (biome == 2 ? snow_cut : plain_cut);
    const int count = static_cast<int>(roll(seed, cx, cy, 100) * 6.0f);
    for (int i = 0; i < count; ++i) {
        const auto salt = static_cast<std::uint32_t>(101 + (i * 3));
        const float pick = roll(seed, cx, cy, salt);
        out.push_back(Deco{
          .x = static_cast<float>(cx) * chunk_size + (roll(seed, cx, cy, salt + 1) * chunk_size),
          .y = static_cast<float>(cy) * chunk_size + (roll(seed, cx, cy, salt + 2) * chunk_size),
          .kind = pick < cut[0] ? std::uint8_t{ 0 }   // plant
                : pick < cut[1] ? std::uint8_t{ 1 }   // pebble
                : pick < cut[2] ? std::uint8_t{ 2 }   // bush
                                : std::uint8_t{ 3 },  // stump
        });
    }
}

// Per-consumer chunk cache: generation runs once per chunk, not per entity
// per tick. Reset when the seed changes (a new run rolls a new world).
struct ChunkCache
{
    std::uint32_t seed = 0;
    std::unordered_map<std::uint64_t, std::vector<Obstacle>> chunks;

    const std::vector<Obstacle>& get(std::uint32_t want_seed, std::int32_t cx, std::int32_t cy)
    {
        if (want_seed != seed) {
            chunks.clear();
            seed = want_seed;
        }
        auto [it, fresh] = chunks.try_emplace(chunk_key(cx, cy));
        if (fresh) { obstacles_in(seed, cx, cy, it->second); }
        return it->second;
    }
};

// Hard circle pushout against the 3x3 chunk neighborhood (walls are walls —
// separation's soft nudge is for crowds). Obstacles inside the clear circle
// are ignored: boss arenas stomp the ground flat so choreography (charges,
// checkmate walls) never grinds on a rock. Returns true if (x, y) moved.
inline bool resolve_terrain(ChunkCache& cache, std::uint32_t seed, float& x, float& y,
                            float radius, float clear_x = 0.0f, float clear_y = 0.0f,
                            float clear_r = 0.0f)
{
    const auto cx = static_cast<std::int32_t>(std::floor(x / chunk_size));
    const auto cy = static_cast<std::int32_t>(std::floor(y / chunk_size));
    bool pushed = false;
    for (std::int32_t j = cy - 1; j <= cy + 1; ++j) {
        for (std::int32_t i = cx - 1; i <= cx + 1; ++i) {
            for (const Obstacle& ob : cache.get(seed, i, j)) {
                if (clear_r > 0.0f) {
                    const float ox = ob.x - clear_x;
                    const float oy = ob.y - clear_y;
                    if ((ox * ox) + (oy * oy) < clear_r * clear_r) { continue; }
                }
                const float apart = ob.r + radius;
                float dx = x - ob.x;
                float dy = y - ob.y;
                const float d2 = (dx * dx) + (dy * dy);
                if (d2 >= apart * apart) { continue; }
                float dist = std::sqrt(d2);
                if (dist < 0.01f) { // dead center: eject along a fixed axis
                    dx = 1.0f;
                    dy = 0.0f;
                    dist = 1.0f;
                }
                x = ob.x + (dx / dist * apart);
                y = ob.y + (dy / dist * apart);
                pushed = true;
            }
        }
    }
    return pushed;
}

} // namespace shared::map
