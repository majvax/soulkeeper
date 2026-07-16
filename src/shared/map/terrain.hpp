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
    std::uint8_t kind; // 0 = tree (trunk), 1 = rock (footprint), 2 = pond (water disc)
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

// Pond of one chunk (0 or 1): CIRCULAR by design. The v1 noise-blob field
// grew organic shapes, and organic means CONCAVE — walkers pocketed in the
// inner corner of an L-shaped pond no matter how the steering was tuned
// (reactive steering is memoryless; only convex shorelines are pocket-free).
// A circle is convex, collides EXACTLY like the rocks (it becomes a kind-2
// obstacle below), and tangent steering around it provably progresses.
// Contained in its own chunk (center margin > radius), so ponds never merge
// into concave unions and a point's water test only needs ITS chunk. The
// wetland gate (very low-freq noise) keeps ponds clustered in districts;
// plain + forest only; the spawn neighborhood (±2 chunks) stays dry.
struct Pond
{
    float x = 0.0f, y = 0.0f;
    float r = 0.0f; // 0 = this chunk has no pond
};

inline Pond pond_in(std::uint32_t seed, std::int32_t cx, std::int32_t cy)
{
    if (cx >= -2 && cx <= 2 && cy >= -2 && cy <= 2) { return {}; } // spawn + margin
    const float mx = (static_cast<float>(cx) + 0.5f) * chunk_size;
    const float my = (static_cast<float>(cy) + 0.5f) * chunk_size;
    if (biome_at(seed, mx, my) == 2) { return {}; } // snowfields stay dry
    const float wet = vnoise(seed, mx * 0.6f, my * 0.6f, 37);
    if (wet < 0.58f) { return {}; }                                   // dry district
    if (roll(seed, cx, cy, 50) > (wet - 0.58f) * 1.4f) { return {}; } // denser when wetter
    const float r = 80.0f + (roll(seed, cx, cy, 51) * 120.0f);        // 80-200 px
    const float margin = r + 40.0f; // contained + room for the shore band
    return Pond{
        .x = (static_cast<float>(cx) * chunk_size) + margin
             + (roll(seed, cx, cy, 52) * (chunk_size - (2.0f * margin))),
        .y = (static_cast<float>(cy) * chunk_size) + margin
             + (roll(seed, cx, cy, 53) * (chunk_size - (2.0f * margin))),
        .r = r,
    };
}

inline bool water_at(std::uint32_t seed, float x, float y)
{
    const Pond pond = pond_in(seed, static_cast<std::int32_t>(std::floor(x / chunk_size)),
                              static_cast<std::int32_t>(std::floor(y / chunk_size)));
    if (pond.r <= 0.0f) { return false; }
    const float dx = x - pond.x;
    const float dy = y - pond.y;
    return (dx * dx) + (dy * dy) < pond.r * pond.r;
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
    // The pond IS a collider: same hard circle pushout as the rocks, drawn as
    // water by the client's ground compositor instead of a sprite.
    const Pond pond = pond_in(seed, cx, cy);
    if (pond.r > 0.0f) {
        out.push_back(Obstacle{ .x = pond.x, .y = pond.y, .r = pond.r, .kind = 2 });
    }
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
        const float x = base_x + 40.0f + (roll(seed, cx, cy, salt + 1) * (chunk_size - 80.0f));
        const float y = base_y + 40.0f + (roll(seed, cx, cy, salt + 2) * (chunk_size - 80.0f));
        // nothing grows in (or right at) a pond — margin keeps trunks off the shore band
        if (pond.r > 0.0f) {
            const float dx = x - pond.x;
            const float dy = y - pond.y;
            const float keep = pond.r + 26.0f;
            if ((dx * dx) + (dy * dy) < keep * keep) { continue; }
        }
        out.push_back(Obstacle{
          .x = x,
          .y = y,
          .r = tree ? 9.0f + (roll(seed, cx, cy, salt + 3) * 3.0f)
                    : 13.0f + (roll(seed, cx, cy, salt + 3) * 7.0f),
          .kind = tree ? std::uint8_t{ 0 } : std::uint8_t{ 1 },
        });
    }
}

// Ground deco of one chunk (purely visual — no collision). Kind weights and
// DENSITY follow the biome: plains grow tufts and flowers, forests thick
// undergrowth, snow stays sparse (pebbles and dead stumps). Ponds grow a
// ring of shore reeds on top.
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
    const float croll = roll(seed, cx, cy, 100);
    const int count = biome == 1 ? 5 + static_cast<int>(croll * 8.0f)   // forest 5-12
                    : biome == 2 ? 1 + static_cast<int>(croll * 5.0f)   // snow   1-5
                                 : 3 + static_cast<int>(croll * 7.0f);  // plain  3-9
    const Pond pond = pond_in(seed, cx, cy);
    for (int i = 0; i < count; ++i) {
        const auto salt = static_cast<std::uint32_t>(101 + (i * 3));
        const float pick = roll(seed, cx, cy, salt);
        const float x = static_cast<float>(cx) * chunk_size + (roll(seed, cx, cy, salt + 1) * chunk_size);
        const float y = static_cast<float>(cy) * chunk_size + (roll(seed, cx, cy, salt + 2) * chunk_size);
        if (pond.r > 0.0f) { // shore deco ok, floating deco not
            const float dx = x - pond.x;
            const float dy = y - pond.y;
            if ((dx * dx) + (dy * dy) < pond.r * pond.r) { continue; }
        }
        out.push_back(Deco{
          .x = x,
          .y = y,
          .kind = pick < cut[0] ? std::uint8_t{ 0 }   // plant
                : pick < cut[1] ? std::uint8_t{ 1 }   // pebble
                : pick < cut[2] ? std::uint8_t{ 2 }   // bush
                                : std::uint8_t{ 3 },  // stump
        });
    }
    // Shore reeds: a scattered ring of tufts (and the odd pebble) hugging the
    // pond rim — the shoreline reads lush, dry chunks pay nothing.
    if (pond.r > 0.0f) {
        const int reeds = 7 + static_cast<int>(roll(seed, cx, cy, 300) * 6.0f); // 7-12
        for (int i = 0; i < reeds; ++i) {
            const auto salt = static_cast<std::uint32_t>(301 + (i * 3));
            const float ang = roll(seed, cx, cy, salt + 1) * 6.2831853f;
            const float rad = pond.r + 10.0f + (roll(seed, cx, cy, salt + 2) * 22.0f);
            out.push_back(Deco{
              .x = pond.x + (std::cos(ang) * rad),
              .y = pond.y + (std::sin(ang) * rad),
              .kind = roll(seed, cx, cy, salt) < 0.8f ? std::uint8_t{ 0 }  // reed tuft
                                                      : std::uint8_t{ 1 }, // wet pebble
            });
        }
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

// Tangent steering around ponds for the sim's GROUND enemies. Ponds are
// CIRCLES, so a blocked walk direction has an exact answer: rotate the
// velocity onto the disc's tangent and skim the shoreline. Convex means no
// pockets — this provably progresses where the old whiskers oscillated in
// the L-corners of the noise-blob ponds. The graze side follows the side
// the heading already passes the center on, hash-stable from `who` when
// dead-on. Players are NOT steered (a wall is a wall under manual control);
// fliers never reach this (they skip the terrain pass).
inline bool steer_around_water(ChunkCache& cache, std::uint32_t seed, float x, float y,
                               float& vx, float& vy, float radius, std::uint32_t who,
                               float clear_x = 0.0f, float clear_y = 0.0f,
                               float clear_r = 0.0f)
{
    const float sp2 = (vx * vx) + (vy * vy);
    if (sp2 < 1.0f) { return false; } // parked anchors don't steer
    const float sp = std::sqrt(sp2);
    const float hx = vx / sp;
    const float hy = vy / sp;
    // Nearest pond whose disc the current heading would cut into.
    const Obstacle* block = nullptr;
    float block_gap = 1e9f;
    float block_reach = 0.0f;
    const auto ccx = static_cast<std::int32_t>(std::floor(x / chunk_size));
    const auto ccy = static_cast<std::int32_t>(std::floor(y / chunk_size));
    for (std::int32_t j = ccy - 1; j <= ccy + 1; ++j) {
        for (std::int32_t i = ccx - 1; i <= ccx + 1; ++i) {
            for (const Obstacle& ob : cache.get(seed, i, j)) {
                if (ob.kind != 2) { continue; }
                if (clear_r > 0.0f) { // arena water is dry by decree
                    const float ax = ob.x - clear_x;
                    const float ay = ob.y - clear_y;
                    if ((ax * ax) + (ay * ay) < clear_r * clear_r) { continue; }
                }
                const float reach = ob.r + radius + 8.0f; // graze radius
                const float cx = ob.x - x;
                const float cy = ob.y - y;
                const float d = std::sqrt((cx * cx) + (cy * cy));
                if (d - reach > 220.0f) { continue; }         // too far to matter yet
                if ((hx * cx) + (hy * cy) <= 0.0f) { continue; } // pond is behind
                const float perp = (hx * cy) - (hy * cx);     // center offset from the ray
                if (perp >= reach || perp <= -reach) { continue; } // ray misses it
                if (d - reach < block_gap) {
                    block_gap = d - reach;
                    block = &ob;
                    block_reach = reach;
                }
            }
        }
    }
    if (block == nullptr) { return false; }
    const float cx = block->x - x;
    const float cy = block->y - y;
    const float d = std::sqrt((cx * cx) + (cy * cy));
    if (d < 1e-3f) { return false; } // pushout owns this case
    // Tangent from an external point: rotate unit(center) by ±asin(reach/d).
    // Sign +asin keeps the center on the RIGHT of travel; pick the side the
    // heading already favors (sign of the perpendicular offset).
    const float perp = (hx * cy) - (hy * cx);
    const float side = (perp < -0.05f * d) ? 1.0f
                     : (perp > 0.05f * d)  ? -1.0f
                                           : ((mix(who) & 1U) != 0U ? 1.0f : -1.0f);
    const float sin_a = std::fmin(block_reach / d, 0.999f);
    const float cos_a = std::sqrt(1.0f - (sin_a * sin_a));
    const float ux = cx / d;
    const float uy = cy / d;
    const float rx = (ux * cos_a) - (uy * sin_a * side);
    const float ry = (ux * sin_a * side) + (uy * cos_a);
    vx = rx * sp;
    vy = ry * sp;
    return true;
}

} // namespace shared::map
