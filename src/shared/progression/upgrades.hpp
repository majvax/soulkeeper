#pragma once
#include "core/ecs.hpp"
#include "shared/components/combat.hpp"
#include "shared/components/physics.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Level-up upgrades. Shared so the server applies them and the client displays
// their name + value + rarity colour. Values are FLAT (percentages compound and
// break easily) and scale with rarity.

enum class Rarity : std::uint8_t { Common, Uncommon, Legendary };
enum class UpgradeId : std::uint8_t { Damage, FireRate, MoveSpeed, MaxHp, AoeZone, Onion };

inline constexpr std::uint8_t upgrade_count = 6;
inline constexpr float min_cooldown = 0.08f; // fire-rate floor

// The concrete magnitude of an upgrade at a given rarity (all flat).
inline float upgrade_amount(UpgradeId id, Rarity rarity)
{
    const std::size_t tier = static_cast<std::size_t>(rarity); // 0/1/2
    switch (id) {
    case UpgradeId::Damage:    return std::array{ 4.0f, 8.0f, 15.0f }[tier];
    case UpgradeId::FireRate:  return std::array{ 0.03f, 0.06f, 0.10f }[tier];
    case UpgradeId::MoveSpeed: return std::array{ 20.0f, 35.0f, 60.0f }[tier];
    case UpgradeId::MaxHp:     return std::array{ 20.0f, 40.0f, 70.0f }[tier];
    case UpgradeId::AoeZone:   return std::array{ 20.0f, 35.0f, 55.0f }[tier];
    case UpgradeId::Onion:     return 0.0f; // object, fixed
    }
    return 0.0f;
}

inline const char* upgrade_label(UpgradeId id)
{
    switch (id) {
    case UpgradeId::Damage:    return "Sharp Rounds";
    case UpgradeId::FireRate:  return "Rapid Fire";
    case UpgradeId::MoveSpeed: return "Swift Boots";
    case UpgradeId::MaxHp:     return "Vitality";
    case UpgradeId::AoeZone:   return "AOE Zone";
    case UpgradeId::Onion:     return "Onion";
    }
    return "?";
}

inline std::string upgrade_value_text(UpgradeId id, Rarity rarity)
{
    const float amount = upgrade_amount(id, rarity);
    switch (id) {
    case UpgradeId::Damage:    return "+" + std::to_string(static_cast<int>(amount)) + " DMG";
    case UpgradeId::FireRate:  return "-" + std::to_string(static_cast<int>(amount * 1000.0f)) + "ms CD";
    case UpgradeId::MoveSpeed: return "+" + std::to_string(static_cast<int>(amount)) + " SPD";
    case UpgradeId::MaxHp:     return "+" + std::to_string(static_cast<int>(amount)) + " MAX HP";
    case UpgradeId::AoeZone:   return "+" + std::to_string(static_cast<int>(amount)) + " AURA";
    case UpgradeId::Onion:     return "grants Aura";
    }
    return "";
}

inline float rarity_weight(Rarity rarity)
{
    switch (rarity) {
    case Rarity::Common:    return 0.70f;
    case Rarity::Uncommon:  return 0.25f;
    case Rarity::Legendary: return 0.05f;
    }
    return 0.0f;
}

// Onion is an object bought once (only while you have no aura); AOE Zone grows
// the aura (only once you have one). The stat upgrades are always available.
inline bool is_available(const core::Registry& registry, core::Entity player, UpgradeId id)
{
    const bool has_aura = registry.try_get<Aura>(player) != nullptr;
    switch (id) {
    case UpgradeId::Onion:   return !has_aura;
    case UpgradeId::AoeZone: return has_aura;
    default:                 return true;
    }
}

inline void apply_upgrade(core::Registry& registry, core::Entity player, UpgradeId id, Rarity rarity)
{
    const float amount = upgrade_amount(id, rarity);
    switch (id) {
    case UpgradeId::Damage: {
        if (Weapon* w = registry.try_get<Weapon>(player)) { w->damage += amount; }
        break;
    }
    case UpgradeId::FireRate: {
        if (Weapon* w = registry.try_get<Weapon>(player)) {
            w->cooldown_max = std::max(min_cooldown, w->cooldown_max - amount);
        }
        break;
    }
    case UpgradeId::MoveSpeed: {
        if (Speed* s = registry.try_get<Speed>(player)) { s->value += amount; }
        break;
    }
    case UpgradeId::MaxHp: {
        if (Health* h = registry.try_get<Health>(player)) {
            h->max += amount;
            h->current = h->max;
        }
        break;
    }
    case UpgradeId::AoeZone: {
        if (Aura* a = registry.try_get<Aura>(player)) {
            a->radius += amount;
            a->per_second += amount * 0.5f;
        }
        break;
    }
    case UpgradeId::Onion: {
        if (registry.try_get<Aura>(player) == nullptr) {
            registry.assign(player, Aura{ .radius = 120.0f, .per_second = 25.0f });
        }
        break;
    }
    }
}
