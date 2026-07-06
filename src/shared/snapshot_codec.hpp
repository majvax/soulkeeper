// src/shared/snapshot_codec.hpp
//
// Snapshot state + full/delta wire codec, shared VERBATIM by the server
// (encode) and the client (decode) so the two ends cannot drift — and so the
// whole thing is unit-testable headlessly (no net, no sim).
//
// Model: each snapshot tick the server captures a SnapshotState — the complete
// networked world (entities sorted by id, positions in ABSOLUTE half-pixel
// units so deltas are origin-independent, script components as an opaque byte
// blob per entity). A client acks the newest state it applied (Input.ack_tick);
// the server then encodes only what changed vs that baseline:
//   header | changed entities (id + flags + flagged fields) | removed ids | aims
// Unchanged entities are omitted entirely (removals are explicit). decode_delta
// re-merges baseline + delta into a COMPLETE SnapshotState, so the client-side
// apply path is identical for full and delta snapshots.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "shared/protocol.hpp"

namespace proto {

// One networked entity, position in absolute half-px steps (round(pos * 2)).
// The script-component bytes live in SnapshotState::script_blob (offset/len)
// to keep this POD and the 32-deep history ring allocation-light.
struct EntityRec
{
    std::uint32_t id;
    std::int32_t qx, qy; // absolute half-px (origin-independent)
    std::uint16_t move_speed;
    std::uint8_t kind, health, variant, scale_q;
    std::uint32_t script_off;
    std::uint16_t script_len;
};

struct SnapshotState
{
    std::uint32_t tick = 0;
    std::uint16_t level = 1;
    std::uint8_t xp_frac = 0;
    std::uint16_t wave = 1;
    float origin_x = 0.0f, origin_y = 0.0f;
    std::vector<EntityRec> entities;      // MUST be sorted by id (sort_entities)
    std::vector<std::byte> script_blob;   // concatenated per-entity comp bytes
    std::vector<PlayerAim> aims;          // always shipped in full (<= 4)

    [[nodiscard]] std::span<const std::byte> script_of(const EntityRec& r) const noexcept
    {
        return { script_blob.data() + r.script_off, r.script_len };
    }
    void sort_entities();
};

// Per-entity delta flags: which fields follow the id byte on the wire.
namespace delta {
inline constexpr std::uint8_t New = 1u << 0;    // full entry fields + script blob
inline constexpr std::uint8_t Pos8 = 1u << 1;   // int8 dqx, dqy (half-px vs baseline)
inline constexpr std::uint8_t Pos16 = 1u << 2;  // int16 vs origin (Pos8 overflow)
inline constexpr std::uint8_t Health = 1u << 3; // uint8
inline constexpr std::uint8_t Meta = 1u << 4;   // uint8 kind, uint8 variant, uint16 move_speed
inline constexpr std::uint8_t Scale = 1u << 5;  // uint8 scale_q
inline constexpr std::uint8_t Script = 1u << 6; // script-component blob re-sent
} // namespace delta

#pragma pack(push, 1)
struct DeltaHeader
{
    std::uint32_t server_tick;
    std::uint32_t baseline_tick; // the acked snapshot this delta applies to
    std::uint16_t changed;       // entity records following the header
    std::uint16_t removed;       // uint32 ids after the changed records
    std::uint16_t level;
    std::uint8_t xp_frac;
    std::uint16_t wave;
    std::uint8_t player_count;
    float origin_x, origin_y;
};
#pragma pack(pop)

// Script-component blob grammar (see mod::write_networked):
//   uint8 count; { uint8 net_id; float fields[field_counts[net_id]] } * count
// The codec never interprets it — field_counts (indexed by net component id,
// from the ScriptComponentRegistry) is just enough to find where a blob ends.
using ScriptFieldCounts = std::span<const std::uint8_t>;

// Encoders append the payload AFTER the MsgType tag (callers write the tag).
void encode_full(const SnapshotState& state, ByteWriter& out);
void encode_delta(const SnapshotState& state, const SnapshotState& baseline, ByteWriter& out);

// Decoders take the payload after the tag. decode_delta returns the complete
// merged state (baseline + changes - removals). nullopt = malformed packet.
[[nodiscard]] std::optional<SnapshotState> decode_full(std::span<const std::byte> payload,
                                                       ScriptFieldCounts field_counts);
[[nodiscard]] std::optional<SnapshotState> decode_delta(std::span<const std::byte> payload,
                                                        const SnapshotState& baseline,
                                                        ScriptFieldCounts field_counts);

// Absolute half-px <-> world/wire helpers shared by both codec paths.
[[nodiscard]] inline std::int32_t to_half_px(float v) noexcept
{
    return static_cast<std::int32_t>(std::lround(v * snapshot_pos_scale));
}
[[nodiscard]] inline float from_half_px(std::int32_t q) noexcept
{
    return static_cast<float>(q) / snapshot_pos_scale;
}

} // namespace proto
