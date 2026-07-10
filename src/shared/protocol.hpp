#pragma once
// Wire protocol for client <-> server. Header-only POD + tiny cursor helpers.
// No ENet here — this only describes bytes. Little-endian host is assumed (all
// our targets are LE); revisit with explicit byte-swapping if that ever changes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace proto {

inline constexpr std::uint16_t default_port = 1234;
inline constexpr std::size_t max_name_len = 24;
inline constexpr std::size_t max_players = 4;

// Bumped on any wire-format change. Seeds the plugin-set hash carried in Join,
// so a version skew is denied cleanly instead of mis-parsing packets.
inline constexpr std::uint16_t protocol_version = 12;

// Simulation runs at 120 Hz; the server sends a snapshot every 2nd tick (60 Hz).
inline constexpr double sim_hz = 120.0;
inline constexpr int snapshot_every_n_ticks = 2;
inline constexpr double snapshot_hz = sim_hz / snapshot_every_n_ticks;

// First byte of every packet tells the receiver how to read the rest.
enum class MsgType : std::uint8_t {
    Join = 1,          // C2S: { token, name } sent right after connecting
    Input = 2,         // C2S: player movement intent
    StartGame = 3,     // C2S: host asks to leave the lobby (no payload)
    Command = 4,       // C2S: a console command, e.g. Pause (reliable)
    SelectUpgrade = 5, // C2S: chosen level-up option index (reliable)
    Welcome = 6,       // S2C: your entity id + whether you are the host (reliable)
    Roster = 7,        // S2C: the list of players (reliable)
    State = 8,         // S2C: lobby vs playing (reliable)
    LevelUp = 9,       // S2C: 3 upgrade choices to pick from (reliable)
    Snapshot = 10,     // S2C: full world state (unreliable)
    JoinDenied = 11,   // S2C: plugin-set hash mismatch; peer is then kicked (reliable)
    SnapshotDelta = 12, // S2C: world state as a delta vs an acked full/delta (unreliable)
    GameOver = 13,      // S2C: the run ended (won/lost + final stats); sim freezes (reliable)
    LuaCommand = 14,    // C2S: a mod console command line, "name args..." (host-only, reliable)
};

enum class EntityKind : std::uint8_t { Mover = 0, Player = 1, Enemy = 2, Projectile = 3, XpOrb = 4, Heart = 5, Chest = 6 };
enum class GameState : std::uint8_t { Lobby = 0, Playing = 1 };

// LevelUp payload = u8 flavor, u8 count, count x LevelUpChoice. The flavor
// only themes the client pick scene: a boss chest opens the same UI as a
// level, but titled as treasure (and rolled objects-only by the mod).
enum class OfferFlavor : std::uint8_t { Level = 0, Chest = 1 };

// Card counts: the LevelUp message is `u8 count` + count entries. The GAME
// (mod:level_offer) decides the count per player; these are the engine's
// fallback and hard cap (the client lays out at most max_level_up_choices).
inline constexpr std::uint8_t level_up_choices = 3;     // C++ fallback roll
inline constexpr std::uint8_t max_level_up_choices = 5; // wire + layout cap

// Console commands sent to the server (payload of a MsgType::Command packet).
// BackToLobby: host-only, accepted only on the game-over screen — full run reset.
enum class Command : std::uint8_t { Pause = 0, Resume = 1, BackToLobby = 2 };

// --- fixed-size name helpers ----------------------------------------------
using Name = char[max_name_len];

inline void write_name(Name& dst, std::string_view src) noexcept
{
    const std::size_t n = std::min(src.size(), max_name_len - 1);
    std::memset(dst, 0, max_name_len);
    std::memcpy(dst, src.data(), n);
}

inline std::string read_name(const Name& src)
{
    std::size_t n = 0;
    while (n < max_name_len && src[n] != '\0') { ++n; }
    return std::string(src, n);
}

// --- byte cursor helpers ---------------------------------------------------
class ByteWriter
{
public:
    template<typename T>
        requires std::is_trivially_copyable_v<T>
    void put(const T& value)
    {
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        buffer_.insert(buffer_.end(), bytes, bytes + sizeof(T));
    }

    // Size the buffer once up front (snapshots know their entry count).
    void reserve(std::size_t bytes) { buffer_.reserve(bytes); }

    // Append a raw byte run (opaque blobs the snapshot codec shuttles around).
    void put_bytes(std::span<const std::byte> bytes)
    {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return buffer_; }

private:
    std::vector<std::byte> buffer_;
};

class ByteReader
{
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept : data_{ data } {}

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] std::optional<T> get() noexcept
    {
        if (pos_ + sizeof(T) > data_.size()) { return std::nullopt; }
        T value;
        std::memcpy(&value, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

// --- message payloads (each preceded by a MsgType byte) --------------------
struct Join
{
    std::uint64_t token;     // stable per client, lets the server resume your player
    std::uint64_t mods_hash; // LuaHost::plugin_hash() — must match the server's
    Name name;
};

// Join refused: the client's plugin set doesn't match. Carries the server's
// hash (the client knows its own) so the mismatch can be shown.
struct JoinDenied
{
    std::uint64_t server_hash;
};

struct Input
{
    std::int8_t move_x; // -1 / 0 / +1
    std::int8_t move_y;
    float aim_x, aim_y; // normalized aim direction
    std::uint8_t firing; // 1 while the trigger is held
    std::uint8_t dash;   // 1 = dash requested this packet (edge, not held)
    // Snapshot ack: server_tick of the newest snapshot this client fully
    // applied (0 = none yet). The server deltas subsequent snapshots against
    // it — piggybacked here because Input already flows every frame.
    std::uint32_t ack_tick;
};

struct SelectUpgrade
{
    std::uint8_t index; // which of the 3 offered choices
};

struct LevelUpChoice
{
    std::uint8_t id;     // content wire id (mod::ContentRegistry)
    std::uint8_t rarity; // mod::Rarity
};

struct Welcome
{
    std::uint32_t player_net_id;
    std::uint8_t is_host;
};

struct StateMsg
{
    std::uint8_t state; // proto::GameState
};

// The run ended (broadcast reliable). The sim freezes on the final frame; the
// host then sends Command::BackToLobby to reset everything for another run.
struct GameOverMsg
{
    std::uint8_t won; // 1 = the mods' win rule fired; 0 = everyone downed
    std::uint16_t final_wave;
    std::uint16_t final_level;
};

struct RosterHeader
{
    std::uint8_t count;
};

struct RosterEntry
{
    std::uint32_t net_id;
    Name name;
    std::uint8_t is_host;
    std::uint8_t connected;
};

// Snapshot positions are quantized: int16 offsets from the header's origin in
// 0.5 px steps → ±16383 px of playfield around the action (many screens).
// Entities farther out clamp to the edge; they're way off-camera, and their
// positions snap back to exact as soon as they re-enter range. This (plus the
// packed layouts below) cuts a 500-entity snapshot by ~a third — snapshots go
// out 60×/s and fragment past the UDP MTU, so every byte here is hot.
inline constexpr float snapshot_pos_scale = 2.0f; // steps per px (1 / 0.5 px)

[[nodiscard]] inline std::int16_t quantize_pos(float v, float origin) noexcept
{
    const float q = std::round((v - origin) * snapshot_pos_scale);
    return static_cast<std::int16_t>(std::clamp(q, -32768.0f, 32767.0f));
}
[[nodiscard]] inline float dequantize_pos(std::int16_t q, float origin) noexcept
{
    return origin + (static_cast<float>(q) / snapshot_pos_scale);
}

#pragma pack(push, 1) // wire structs: no padding bytes on the wire
struct SnapshotHeader
{
    std::uint32_t server_tick;
    std::uint16_t count;
    std::uint16_t level;   // shared team level (for the HUD)
    std::uint8_t xp_frac;  // 0..255 progress toward the next level
    std::uint16_t wave;    // current wave number
    std::uint8_t player_count; // PlayerAim records in the trailer after the entries
    float origin_x, origin_y;  // quantization origin (near the players)
};

struct SnapshotEntry // 15 bytes packed
{
    std::uint32_t id;         // server entity id == the network id
    std::int16_t qx, qy;      // quantize_pos(pos, header origin)
    std::uint16_t move_speed; // players: current move speed px/s (for prediction)
    std::uint8_t kind;        // proto::EntityKind
    std::uint8_t health;      // players: current hearts; others: 0..255 fraction of max health
    std::uint8_t variant;     // enemies: archetype wire id; players: max hearts; 0 otherwise
    std::uint8_t scale_q;     // kernel Scale x32 (1.0 -> 32, max ~8x); 0 = no Scale = 1.0
    std::uint8_t fx;          // Render.fx: Lua-driven anim state (1 = attacking)
};

// Trailer after the entry array: the authoritative aim + trigger of each player,
// so the CLIENT drives sprite facing/shoot-pose from the sim's aim (which a
// server-side override like autofire mutates) instead of the local mouse. Kept
// off SnapshotEntry so the 500-enemy array stays 14 B/entity — only <=4 of these.
// Now also carries each player's authoritative dash state: the game (Lua) sets
// max_charges/cooldown_max, which the client's dash prediction + HUD can't guess.
struct PlayerAim // 13 bytes packed
{
    std::uint32_t id;             // player net id (matches its SnapshotEntry)
    std::int8_t aim_qx, aim_qy;   // AimState direction, quantize_aim (x127)
    std::uint8_t firing;          // 1 = trigger effectively held (manual or autofire)
    std::uint8_t dash_charges;    // Dash.charges (ready dashes)
    std::uint8_t dash_max;        // Dash.max_charges
    std::uint16_t dash_cd_ms;     // Dash.cooldown, ms until the next charge
    std::uint16_t dash_cd_max_ms; // Dash.cooldown_max, ms per charge refill
};
#pragma pack(pop)

// Aim direction codec: components are already normalized to [-1, 1].
[[nodiscard]] inline std::int8_t quantize_aim(float v) noexcept
{
    return static_cast<std::int8_t>(std::clamp(std::lround(v * 127.0f), -127L, 127L));
}
[[nodiscard]] inline float dequantize_aim(std::int8_t q) noexcept
{
    return static_cast<float>(q) / 127.0f;
}

// Seconds <-> milliseconds (u16) for dash cooldown on the wire (0..65.535 s).
[[nodiscard]] inline std::uint16_t seconds_to_ms(float s) noexcept
{
    return static_cast<std::uint16_t>(std::clamp(std::lround(s * 1000.0f), 0L, 65535L));
}
[[nodiscard]] inline float ms_to_seconds(std::uint16_t ms) noexcept
{
    return static_cast<float>(ms) / 1000.0f;
}

// Scale byte codec (32 steps per 1.0x; plenty for "grows a bit" visuals).
inline constexpr float snapshot_scale_step = 32.0f;
[[nodiscard]] inline std::uint8_t quantize_scale(float value) noexcept
{
    return static_cast<std::uint8_t>(std::clamp(std::round(value * snapshot_scale_step), 1.0f, 255.0f));
}
[[nodiscard]] inline float dequantize_scale(std::uint8_t q) noexcept
{
    return q == 0 ? 1.0f : static_cast<float>(q) / snapshot_scale_step;
}
// Each SnapshotEntry is immediately followed on the wire by the entity's
// networked script components (see mod::write_networked / read_networked):
//   uint8 count; { uint8 net_comp_id; float fields[schema.field_count] } * count

} // namespace proto
