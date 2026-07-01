#pragma once
// Wire protocol for client <-> server. Header-only POD + tiny cursor helpers.
// No ENet here — this only describes bytes. Little-endian host is assumed (all
// our targets are LE); revisit with explicit byte-swapping if that ever changes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
    Snapshot = 10,     // S2C: world state (unreliable)
};

enum class EntityKind : std::uint8_t { Mover = 0, Player = 1, Enemy = 2, Projectile = 3, XpOrb = 4 };
enum class GameState : std::uint8_t { Lobby = 0, Playing = 1 };

inline constexpr std::uint8_t level_up_choices = 3;

// Console commands sent to the server (payload of a MsgType::Command packet).
enum class Command : std::uint8_t { Pause = 0, Resume = 1 };

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
    std::uint64_t token; // stable per client, lets the server resume your player
    Name name;
};

struct Input
{
    std::int8_t move_x; // -1 / 0 / +1
    std::int8_t move_y;
    float aim_x, aim_y; // normalized aim direction
    std::uint8_t firing; // 1 while the trigger is held
};

struct SelectUpgrade
{
    std::uint8_t index; // which of the 3 offered choices
};

struct LevelUpChoice
{
    std::uint8_t id;     // UpgradeId
    std::uint8_t rarity; // Rarity
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

struct SnapshotHeader
{
    std::uint32_t server_tick;
    std::uint16_t count;
    std::uint16_t level;   // shared team level (for the HUD)
    std::uint8_t xp_frac;  // 0..255 progress toward the next level
    std::uint16_t wave;    // current wave number
};

struct SnapshotEntry
{
    std::uint32_t id; // server entity id == the network id
    float x, y;
    std::uint8_t kind;         // proto::EntityKind
    std::uint8_t health;       // 0..255 = fraction of max health
    std::uint8_t variant;      // enemies: archetype id (EnemyType); 0 otherwise
    std::uint16_t aura_radius; // players: aura radius in px (0 = none)
    std::uint16_t move_speed;  // players: current move speed px/s (for prediction)
};

} // namespace proto
