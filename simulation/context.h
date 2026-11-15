#pragma once
#include <array>
#include <cstdint>

namespace ow::statescript::simulation {
// Component113/0x71, DWORD fields+18/+1C/+20. Constructor1016941/49
// initializes these bits to all ones. Names describe behavior, not source names.
struct ContextChangeMasks {
    std::uint32_t current = ~std::uint32_t{0};
    std::uint32_t combined = ~std::uint32_t{0};
    std::uint32_t published = ~std::uint32_t{0};
    // 102FFF0: immediate AND retained publication, not pending-only.
    void publish(std::uint32_t mask) noexcept {
        current |= mask;
        combined |= mask;
        published |= mask;
    }
    // C306A0: no clear of the published field occurs in this operation.
    void refresh(std::uint32_t lobby_current) noexcept {
        current = published;
        combined = current | lobby_current;
    }
};
struct ContextSubscription { std::uint8_t component_id; std::uint32_t mask; };
// C305C0 registers these in order through MirrorSystem's first callback table.
inline constexpr std::array<ContextSubscription, 8> context_subscriptions{{
    {40, 2}, {32, 4}, {19, 8}, {61, 0x10},
    {62, 0x20}, {16, 0x40}, {64, 0x80}, {63, 0x100}
}};
struct ContextObservationCache {
    bool local_attachment_gate = false; // system+248B8
    std::int32_t filter_category = -1; // system+248BC; do not assume team id
};
// C306A0 emits changes only when cached observations differ. Calls the engine
// notifier (when present) BEFORE updating each corresponding cache field.
struct ContextChangeSink {
    virtual ~ContextChangeSink() = default;
    virtual void publish_changes(std::uint32_t mask) = 0;
};
inline void refresh_context_observations(ContextObservationCache& cache,
                                         bool local_attachment_gate,
                                         std::int32_t filter_category,
                                         ContextChangeSink* notifier) {
    if (cache.local_attachment_gate != local_attachment_gate) {
        if (notifier) notifier->publish_changes(0x800);
        cache.local_attachment_gate = local_attachment_gate;
    }
    if (cache.filter_category != filter_category) {
        if (notifier) notifier->publish_changes(0x400000);
        cache.filter_category = filter_category;
    }
}
// C30560/102FF70: bit39 is ProjectileComponent; the STU flag is
// STUProjectileVisualComponent.m_BFFBE115. Its original semantic name is unknown.
constexpr bool allows_component_context_notification(bool has_projectile,
                                                       bool projectile_visual_flag) {
    return !has_projectile || projectile_visual_flag;
}
} // namespace ow::statescript::simulation
