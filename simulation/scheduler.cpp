#include "scheduler.h"
#include <utility>
#include <xmmintrin.h>

namespace ow::statescript::simulation {
float priority_for(const ComponentView& component, bool is_local_aim,
                    const CameraView& camera, Services& services) {
    if (is_local_aim || component.was_deferred || component.instance_count == 0)
        return mandatory_priority;
    Vec3 position;
    if (!services.world_position(component, position)) return 0.0f;
    const Vec3 displacement{position.x - camera.position.x,
                            position.y - camera.position.y,
                            position.z - camera.position.z};
    const float squared = ((displacement.y * displacement.y)
                         + (displacement.x * displacement.x))
                         + (displacement.z * displacement.z);
    float distance;
    Vec3 direction;
    // COMISS/JNB at C2FCB4 also takes this fallback for unordered (NaN).
    if (!(squared >= 1.1754943508222875e-36f)) {
        distance = -1.0f; // X_1 at162D510 is NEGATIVE one
        direction = {0.0f, 0.0f, 1.0f};
    } else {
        // Lower-lane equivalent of original RSQRTPS + one Newton refinement.
        const float estimate = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(squared)));
        const float inverse = (3.0f - ((estimate * estimate) * squared)) * (estimate * 0.5f);
        direction = {displacement.x * inverse, displacement.y * inverse, displacement.z * inverse};
        distance = inverse * squared;
        // C2FD09 replaces direction only; it does NOT replace a NaN distance.
        if (!(distance >= 0.0f)) direction = {0.0f, 0.0f, 1.0f};
    }
    const float dot = ((camera.forward.y * direction.y) + (camera.forward.x * direction.x))
                    + (camera.forward.z * direction.z);
    float priority = -distance;
    if (dot > 0.5f) priority += ((dot - 0.5f) * 2.0f) * 100.0f;
    return priority;
}
namespace {
void sift_minimum(WorkItem* work, std::size_t root, std::size_t count) noexcept {
    while (root < count / 2) {
        auto child = root * 2 + 1;
        if (child + 1 < count && work[child + 1].priority < work[child].priority) ++child;
        if (!(work[child].priority < work[root].priority)) return;
        std::swap(work[root], work[child]);
        root = child;
    }
}
}
void sort_descending(WorkItem* work, std::size_t count) noexcept {
    for (auto start = count / 2; start != 0;) sift_minimum(work, --start, count);
    for (auto end = count; end > 1;) {
        --end;
        std::swap(work[0], work[end]);
        sift_minimum(work, 0, end);
    }
}
FrameResult execute_frame(ComponentView* const* components, std::size_t count,
                           WorkItem* work, std::size_t capacity,
                           const FrameContext& context, Services& services) {
    std::size_t candidates = 0;
    for (std::size_t i = 0; i < count; ++i) {
        auto& component = *components[i];
        const bool local_aim = services.is_local_aim_entity(component);
        const bool context_eligible = component.always_eligible || component.was_deferred
                                   || (component.context_interest_mask & context.context_mask) != 0;
        if (!local_aim && !(context.include_context_candidates && context_eligible)) continue;
        if (candidates < capacity) {
            auto& item = work[candidates];
            item.component = &component;
            item.scheduling_allowance = local_aim ? 16000
                : 16000 * static_cast<std::int64_t>(context.allowance_multiplier);
            item.priority = priority_for(component, local_aim, context.camera, services);
        }
        ++candidates;
    }
    if (candidates > capacity) return {FrameStatus::InsufficientWorkStorage, candidates, 0, 0};
    if (candidates == 0) return {FrameStatus::Completed, 0, 0, 0};
    sort_descending(work, candidates);
    const auto start = services.performance_counter_low();
    std::size_t processed = 0;
    for (; processed < candidates; ++processed) {
        auto& item = work[processed];
        // Even an optional first item executes. Mandatory scores never trigger a
        // clock query; COMISS in the client also skips unordered priorities.
        if (processed != 0 && item.priority < mandatory_priority) {
            const std::uint32_t elapsed = services.performance_counter_low() - start;
            const float seconds = static_cast<float>(elapsed) * context.seconds_per_counter_tick;
            if (seconds > frame_budget_seconds) break;
        }
        services.execute(*item.component, context.simulation_frame, item.scheduling_allowance);
        item.component->deferred_context_mask = 0;
        item.component->was_deferred = false;
    }
    for (std::size_t i = processed; i < candidates; ++i) {
        auto& component = *work[i].component;
        component.deferred_context_mask |= context.context_mask;
        component.was_deferred = true;
    }
    const auto deferred = candidates - processed;
    return {deferred ? FrameStatus::BudgetExceeded : FrameStatus::Completed,
            candidates, processed, deferred};
}
} // namespace ow::statescript::simulation
