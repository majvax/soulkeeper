// src/shared/snapshot_codec.cpp
#include "shared/snapshot_codec.hpp"

#include <algorithm>
#include <cstring>

namespace proto {

namespace {

// Skip one script-component blob in `reader`, returning its byte length, or
// -1 on a malformed blob (unknown component id / truncated packet).
[[nodiscard]] int blob_length(ByteReader& reader, ScriptFieldCounts field_counts)
{
    const auto count = reader.get<std::uint8_t>();
    if (!count) { return -1; }
    int len = 1;
    for (std::uint8_t i = 0; i < *count; ++i) {
        const auto net_id = reader.get<std::uint8_t>();
        if (!net_id || *net_id >= field_counts.size()) { return -1; }
        len += 1;
        for (std::uint8_t f = 0; f < field_counts[*net_id]; ++f) {
            if (!reader.get<float>()) { return -1; }
            len += static_cast<int>(sizeof(float));
        }
    }
    return len;
}

// Read one blob out of `payload` at `pos` into state.script_blob, filling the
// rec's offset/len. Returns false on malformed input.
bool read_blob(std::span<const std::byte> payload, std::size_t& pos, SnapshotState& state,
               EntityRec& rec, ScriptFieldCounts field_counts)
{
    ByteReader probe(payload.subspan(pos));
    const int len = blob_length(probe, field_counts);
    if (len < 0) { return false; }
    rec.script_off = static_cast<std::uint32_t>(state.script_blob.size());
    rec.script_len = static_cast<std::uint16_t>(len);
    state.script_blob.insert(state.script_blob.end(), payload.begin() + pos,
                             payload.begin() + pos + len);
    pos += static_cast<std::size_t>(len);
    return true;
}

// Copy a baseline rec into `state`, re-homing its script bytes.
void carry_over(SnapshotState& state, const SnapshotState& baseline, const EntityRec& rec)
{
    EntityRec copy = rec;
    const std::span<const std::byte> blob = baseline.script_of(rec);
    copy.script_off = static_cast<std::uint32_t>(state.script_blob.size());
    state.script_blob.insert(state.script_blob.end(), blob.begin(), blob.end());
    state.entities.push_back(copy);
}

void put_full_entry(const SnapshotState& state, const EntityRec& rec, std::int32_t origin_qx,
                    std::int32_t origin_qy, ByteWriter& out)
{
    // Same clamping as quantize_pos: far-off entities pin to the int16 edge.
    const auto rel = [](std::int32_t q, std::int32_t origin_q) {
        return static_cast<std::int16_t>(std::clamp(q - origin_q, -32768, 32767));
    };
    out.put(SnapshotEntry{ .id = rec.id,
                           .qx = rel(rec.qx, origin_qx),
                           .qy = rel(rec.qy, origin_qy),
                           .move_speed = rec.move_speed,
                           .kind = rec.kind,
                           .health = rec.health,
                           .variant = rec.variant,
                           .scale_q = rec.scale_q,
                           .fx = rec.fx });
    out.put_bytes(state.script_of(rec));
}

} // namespace

void SnapshotState::sort_entities()
{
    std::sort(entities.begin(), entities.end(),
              [](const EntityRec& a, const EntityRec& b) { return a.id < b.id; });
}

void encode_full(const SnapshotState& state, ByteWriter& out)
{
    const std::int32_t oqx = to_half_px(state.origin_x);
    const std::int32_t oqy = to_half_px(state.origin_y);
    out.put(SnapshotHeader{ .server_tick = state.tick,
                            .count = static_cast<std::uint16_t>(state.entities.size()),
                            .level = state.level,
                            .xp_frac = state.xp_frac,
                            .wave = state.wave,
                            .event = state.event,
                            .player_count = static_cast<std::uint8_t>(state.aims.size()),
                            .origin_x = state.origin_x,
                            .origin_y = state.origin_y });
    for (const EntityRec& rec : state.entities) { put_full_entry(state, rec, oqx, oqy, out); }
    for (const PlayerAim& aim : state.aims) { out.put(aim); }
}

void encode_delta(const SnapshotState& state, const SnapshotState& baseline, ByteWriter& out)
{
    const std::int32_t oqx = to_half_px(state.origin_x);
    const std::int32_t oqy = to_half_px(state.origin_y);

    // Merge-walk both sorted entity lists into a changes buffer + removed list.
    ByteWriter changes;
    std::uint16_t changed = 0;
    std::vector<std::uint32_t> removed;
    std::size_t bi = 0;
    for (const EntityRec& cur : state.entities) {
        while (bi < baseline.entities.size() && baseline.entities[bi].id < cur.id) {
            removed.push_back(baseline.entities[bi].id); // in baseline, gone now
            ++bi;
        }
        if (bi >= baseline.entities.size() || baseline.entities[bi].id != cur.id) {
            changes.put(cur.id); // brand new entity: ship the full entry
            changes.put(delta::New);
            put_full_entry(state, cur, oqx, oqy, changes);
            ++changed;
            continue;
        }
        const EntityRec& base = baseline.entities[bi];
        ++bi;

        const std::int32_t dqx = cur.qx - base.qx;
        const std::int32_t dqy = cur.qy - base.qy;
        const std::span<const std::byte> cur_blob = state.script_of(cur);
        const std::span<const std::byte> base_blob = baseline.script_of(base);
        const bool script_changed =
          cur_blob.size() != base_blob.size()
          || std::memcmp(cur_blob.data(), base_blob.data(), cur_blob.size()) != 0;

        std::uint8_t flags = 0;
        if (dqx != 0 || dqy != 0) {
            const bool fits = dqx >= -128 && dqx <= 127 && dqy >= -128 && dqy <= 127;
            flags |= fits ? delta::Pos8 : delta::Pos16;
        }
        if (cur.health != base.health) { flags |= delta::Health; }
        if (cur.kind != base.kind || cur.variant != base.variant
            || cur.move_speed != base.move_speed || cur.fx != base.fx) {
            flags |= delta::Meta;
        }
        if (cur.scale_q != base.scale_q) { flags |= delta::Scale; }
        if (script_changed) { flags |= delta::Script; }
        if (flags == 0) { continue; } // untouched: omitted, client keeps it

        changes.put(cur.id);
        changes.put(flags);
        if ((flags & delta::Pos8) != 0) {
            changes.put(static_cast<std::int8_t>(dqx));
            changes.put(static_cast<std::int8_t>(dqy));
        } else if ((flags & delta::Pos16) != 0) {
            changes.put(static_cast<std::int16_t>(std::clamp(cur.qx - oqx, -32768, 32767)));
            changes.put(static_cast<std::int16_t>(std::clamp(cur.qy - oqy, -32768, 32767)));
        }
        if ((flags & delta::Health) != 0) { changes.put(cur.health); }
        if ((flags & delta::Meta) != 0) {
            changes.put(cur.kind);
            changes.put(cur.variant);
            changes.put(cur.fx);
            changes.put(cur.move_speed);
        }
        if ((flags & delta::Scale) != 0) { changes.put(cur.scale_q); }
        if ((flags & delta::Script) != 0) { changes.put_bytes(cur_blob); }
        ++changed;
    }
    for (; bi < baseline.entities.size(); ++bi) { removed.push_back(baseline.entities[bi].id); }

    out.put(DeltaHeader{ .server_tick = state.tick,
                         .baseline_tick = baseline.tick,
                         .changed = changed,
                         .removed = static_cast<std::uint16_t>(removed.size()),
                         .level = state.level,
                         .xp_frac = state.xp_frac,
                         .wave = state.wave,
                         .event = state.event,
                         .player_count = static_cast<std::uint8_t>(state.aims.size()),
                         .origin_x = state.origin_x,
                         .origin_y = state.origin_y });
    out.put_bytes(changes.bytes());
    for (const std::uint32_t id : removed) { out.put(id); }
    for (const PlayerAim& aim : state.aims) { out.put(aim); }
}

std::optional<SnapshotState> decode_full(std::span<const std::byte> payload,
                                         ScriptFieldCounts field_counts)
{
    ByteReader reader(payload);
    const auto header = reader.get<SnapshotHeader>();
    if (!header) { return std::nullopt; }

    SnapshotState state;
    state.tick = header->server_tick;
    state.level = header->level;
    state.xp_frac = header->xp_frac;
    state.wave = header->wave;
    state.event = header->event;
    state.origin_x = header->origin_x;
    state.origin_y = header->origin_y;
    const std::int32_t oqx = to_half_px(header->origin_x);
    const std::int32_t oqy = to_half_px(header->origin_y);

    std::size_t pos = sizeof(SnapshotHeader);
    state.entities.reserve(header->count);
    for (std::uint16_t i = 0; i < header->count; ++i) {
        ByteReader at(payload.subspan(pos));
        const auto entry = at.get<SnapshotEntry>();
        if (!entry) { return std::nullopt; }
        pos += sizeof(SnapshotEntry);
        EntityRec rec{ .id = entry->id,
                       .qx = oqx + entry->qx,
                       .qy = oqy + entry->qy,
                       .move_speed = entry->move_speed,
                       .kind = entry->kind,
                       .health = entry->health,
                       .variant = entry->variant,
                       .scale_q = entry->scale_q,
                       .fx = entry->fx,
                       .script_off = 0,
                       .script_len = 0 };
        if (!read_blob(payload, pos, state, rec, field_counts)) { return std::nullopt; }
        state.entities.push_back(rec);
    }
    ByteReader tail(payload.subspan(pos));
    for (std::uint8_t i = 0; i < header->player_count; ++i) {
        const auto aim = tail.get<PlayerAim>();
        if (!aim) { return std::nullopt; }
        state.aims.push_back(*aim);
    }
    state.sort_entities();
    return state;
}

std::optional<SnapshotState> decode_delta(std::span<const std::byte> payload,
                                          const SnapshotState& baseline,
                                          ScriptFieldCounts field_counts)
{
    ByteReader reader(payload);
    const auto header = reader.get<DeltaHeader>();
    if (!header || header->baseline_tick != baseline.tick) { return std::nullopt; }

    SnapshotState state;
    state.tick = header->server_tick;
    state.level = header->level;
    state.xp_frac = header->xp_frac;
    state.wave = header->wave;
    state.event = header->event;
    state.origin_x = header->origin_x;
    state.origin_y = header->origin_y;
    const std::int32_t oqx = to_half_px(header->origin_x);
    const std::int32_t oqy = to_half_px(header->origin_y);
    state.entities.reserve(baseline.entities.size() + header->changed);
    state.script_blob.reserve(baseline.script_blob.size());

    // Changed records arrive sorted by id (the encoder walks sorted lists), so
    // one merge pass rebuilds the complete sorted state: for every changed id,
    // first carry over the baseline entities before it.
    std::size_t pos = sizeof(DeltaHeader);
    std::size_t bi = 0;
    for (std::uint16_t i = 0; i < header->changed; ++i) {
        ByteReader at(payload.subspan(pos));
        const auto id = at.get<std::uint32_t>();
        const auto flags = at.get<std::uint8_t>();
        if (!id || !flags) { return std::nullopt; }
        pos += sizeof(std::uint32_t) + sizeof(std::uint8_t);

        while (bi < baseline.entities.size() && baseline.entities[bi].id < *id) {
            carry_over(state, baseline, baseline.entities[bi]);
            ++bi;
        }

        if ((*flags & delta::New) != 0) {
            ByteReader ne(payload.subspan(pos));
            const auto entry = ne.get<SnapshotEntry>();
            if (!entry || entry->id != *id) { return std::nullopt; }
            pos += sizeof(SnapshotEntry);
            EntityRec rec{ .id = entry->id,
                           .qx = oqx + entry->qx,
                           .qy = oqy + entry->qy,
                           .move_speed = entry->move_speed,
                           .kind = entry->kind,
                           .health = entry->health,
                           .variant = entry->variant,
                           .scale_q = entry->scale_q,
                           .fx = entry->fx,
                           .script_off = 0,
                           .script_len = 0 };
            if (!read_blob(payload, pos, state, rec, field_counts)) { return std::nullopt; }
            state.entities.push_back(rec);
            // A New record may replace a same-id baseline entry (shouldn't
            // happen — ids carry version bits — but stay well-defined).
            if (bi < baseline.entities.size() && baseline.entities[bi].id == *id) { ++bi; }
            continue;
        }

        if (bi >= baseline.entities.size() || baseline.entities[bi].id != *id) {
            return std::nullopt; // delta against an entity we don't have: corrupt
        }
        EntityRec rec = baseline.entities[bi];
        ++bi;
        ByteReader fields(payload.subspan(pos));
        std::size_t taken = 0;
        const auto take = [&](auto& dst) {
            using T = std::remove_reference_t<decltype(dst)>;
            const auto v = fields.get<T>();
            if (v) {
                dst = *v;
                taken += sizeof(T);
            }
            return v.has_value();
        };
        bool ok = true;
        if ((*flags & delta::Pos8) != 0) {
            std::int8_t dqx = 0;
            std::int8_t dqy = 0;
            ok = ok && take(dqx) && take(dqy);
            rec.qx += dqx;
            rec.qy += dqy;
        } else if ((*flags & delta::Pos16) != 0) {
            std::int16_t rx = 0;
            std::int16_t ry = 0;
            ok = ok && take(rx) && take(ry);
            rec.qx = oqx + rx;
            rec.qy = oqy + ry;
        }
        if (ok && (*flags & delta::Health) != 0) { ok = take(rec.health); }
        if (ok && (*flags & delta::Meta) != 0) {
            ok = take(rec.kind) && take(rec.variant) && take(rec.fx) && take(rec.move_speed);
        }
        if (ok && (*flags & delta::Scale) != 0) { ok = take(rec.scale_q); }
        if (!ok) { return std::nullopt; }
        pos += taken;
        if ((*flags & delta::Script) != 0) {
            if (!read_blob(payload, pos, state, rec, field_counts)) { return std::nullopt; }
        } else {
            const std::span<const std::byte> blob = baseline.script_of(rec);
            rec.script_off = static_cast<std::uint32_t>(state.script_blob.size());
            state.script_blob.insert(state.script_blob.end(), blob.begin(), blob.end());
        }
        state.entities.push_back(rec);
    }

    // Removed ids (sorted, like the baseline). Carry over everything else.
    ByteReader tail(payload.subspan(pos));
    std::vector<std::uint32_t> removed;
    removed.reserve(header->removed);
    for (std::uint16_t i = 0; i < header->removed; ++i) {
        const auto id = tail.get<std::uint32_t>();
        if (!id) { return std::nullopt; }
        removed.push_back(*id);
    }
    std::size_t ri = 0;
    for (; bi < baseline.entities.size(); ++bi) {
        const std::uint32_t id = baseline.entities[bi].id;
        while (ri < removed.size() && removed[ri] < id) { ++ri; }
        if (ri < removed.size() && removed[ri] == id) { continue; }
        carry_over(state, baseline, baseline.entities[bi]);
    }
    // Removals interleaved with changed ids were skipped during the merge pass
    // above (baseline entries below a changed id are carried over blindly), so
    // prune any that slipped through.
    if (!removed.empty()) {
        std::erase_if(state.entities, [&](const EntityRec& r) {
            return std::binary_search(removed.begin(), removed.end(), r.id);
        });
    }

    for (std::uint8_t i = 0; i < header->player_count; ++i) {
        const auto aim = tail.get<PlayerAim>();
        if (!aim) { return std::nullopt; }
        state.aims.push_back(*aim);
    }
    state.sort_entities(); // New ids can interleave anywhere; keep the invariant
    return state;
}

} // namespace proto
