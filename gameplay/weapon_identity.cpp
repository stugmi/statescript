#include "weapon_volley.h"

namespace ow::statescript::gameplay {
WeaponKey weapon_key(std::uint32_t entity_id, std::uint16_t instance_id,
                     std::uint32_t state_index, std::uint8_t sequence) noexcept {
    return {(std::uint64_t(entity_id) << 32)
          | (std::uint64_t(instance_id & 0x1fff) << 19)
          | (std::uint64_t(state_index & 0x7ff) << 8)
          | sequence};
}
std::uint16_t weapon_seed(std::uint16_t component_seed, std::uint16_t instance_id,
                          std::uint32_t state_index, std::uint16_t begin_count) noexcept {
    return static_cast<std::uint16_t>(component_seed + 23u * instance_id
                                      + 7u * state_index + (begin_count & 0x3f));
}
WeaponRecord* find_weapon_record(const WeaponRecordTable& table, WeaponKey key) noexcept {
    for (const auto& entry : table.buckets[key.value & 7]) {
        if (entry.key.value == key.value) return entry.record;
    }
    return nullptr;
}
WeaponRecord& acquire_weapon_record(const WeaponRecordTable& table, WeaponKey key,
                                    std::uint32_t flags, WeaponRecordFactory& factory) {
    flags |= 8;
    if (auto* record = find_weapon_record(table, key)) {
        record->flags |= flags;
        return *record;
    }
    return factory.create(key, flags);
}
void delete_tracked_projectiles(std::span<TrackedProjectile> projectiles, ProjectileDeletion& deletion) {
    for (auto& projectile : projectiles) {
        if (projectile.current_id == -1) continue;
        if (auto* entity = deletion.find(projectile.current_id)) deletion.erase(entity);
        const auto old = projectile.current_id;
        projectile.current_id = -1;
        projectile.previous_id = old;
    }
}
} // namespace ow::statescript::gameplay
