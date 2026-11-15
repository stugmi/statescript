#include "weapon_volley.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace ow::statescript::gameplay {
namespace {
constexpr float minimum_rate = 0.00001f; // dword_162C6E4
float recovered_sqrt(float value) noexcept {
    // 8597A0, called by the accelerating-rate inverse at 107828E.
    if (value <= 1.1754943508222875e-36f) return 0.0f;
    const float inverse = 1.0f / std::sqrt(value);
    return value * ((3.0f - value * (inverse * inverse)) * (inverse * 0.5f));
}
std::int64_t round_milliseconds(float seconds) noexcept {
    float value = seconds * 1000.0f;
    value = value >= 0.0f ? value + 0.5f : value - 0.5f;
    // Original CVTTSS2SI targets EAX, followed by CDQE. Preserve its indefinite
    // result rather than invoking undefined C++ float-to-int conversion.
    if (!(value >= -2147483648.0f && value < 2147483648.0f))
        return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(value);
}
}
RateRamp make_rate_ramp(float initial, const RateDefinition* definition) noexcept {
    RateRamp rate;
    rate.initial = initial;
    rate.target = initial;
    if (definition) {
        rate.target = definition->target;
        if (definition->explicit_slope_magnitude <= 0.0f) {
            rate.slope = (rate.target - initial) / std::fmax(definition->duration, minimum_rate);
        } else if (rate.target > initial) {
            rate.slope = std::fabs(definition->explicit_slope_magnitude);
        } else if (rate.target < initial) {
            rate.slope = -std::fabs(definition->explicit_slope_magnitude);
        }
    }
    if (rate.slope != 0.0f) rate.duration = (rate.target - initial) / rate.slope;
    rate.transition_count = ((rate.slope * 0.5f * rate.duration) * rate.duration)
                          + (initial * rate.duration);
    return rate;
}
std::int64_t shot_time_ms(const RateRamp& rate, std::int32_t shot_index) noexcept {
    const float count = static_cast<float>(shot_index);
    if (rate.slope == 0.0f || count < 0.0f) return round_milliseconds(count / rate.initial);
    if (count < rate.transition_count) {
        const float discriminant = (rate.slope * 2.0f * count) + (rate.initial * rate.initial);
        return round_milliseconds((recovered_sqrt(discriminant) - rate.initial) / rate.slope);
    }
    return round_milliseconds(((count - rate.transition_count) / rate.target) + rate.duration);
}
void initialize_volley(VolleyProgress& progress, const WeaponInputs& inputs,
                         const RateDefinition* definition) noexcept {
    progress.total_shots = std::max(inputs.total_shots, 0);
    progress.group_divisor = std::max(inputs.group_divisor, 1);
    progress.fraction = std::fmax(std::fmin(inputs.fraction, 1.0f), 0.0f);
    progress.rate = make_rate_ramp(std::fmax(inputs.initial_rate, minimum_rate), definition);
    progress.shot_index = 0;
    progress.emitted_index = 0;
    progress.scheduled_time_copy = 0;
    progress.next_shot_time = 0;
    progress.last_timer_index = 0;
    progress.cycle_id = 0;
    progress.time_offset = 0;
}
void schedule_volley_timers(VolleyProgress& progress, TimerLimits limits, std::int64_t frame,
                            std::int64_t begin_frame, WeaponTimers& host) {
    const auto elapsed = frame >= begin_frame ? frame - begin_frame : 0;
    const auto adjustment = progress.time_offset - elapsed;
    const auto groups = progress.total_shots / progress.group_divisor;
    if (limits.first < groups) {
        host.schedule(frame + shot_time_ms(progress.rate, std::max(limits.first - 1, 0)) + adjustment, 1);
    }
    progress.last_timer_index = std::max(std::min(groups, limits.second) - 1, 0);
    host.schedule(frame + shot_time_ms(progress.rate, progress.last_timer_index) + adjustment, 2);
}
ShotVisit visit_shot(std::vector<ShotHistoryEntry>& history, std::int64_t frame,
                      std::uint16_t begin_count, std::int32_t cycle_id,
                      std::int32_t shot_index) {
    const auto generation = static_cast<std::uint16_t>(begin_count & 0x3f);
    for (std::size_t i = 0; i < history.size();) {
        auto& entry = history[i];
        if (entry.frame < frame - 2000) {
            if (i + 1 != history.size()) entry = history.back();
            history.pop_back();
            continue; // re-examine the swapped-in entry, as D46B0D does
        }
        if (entry.begin_generation == generation && entry.cycle_id == cycle_id) {
            if (shot_index <= entry.highest_shot_index) return ShotVisit::Duplicate;
            entry.frame = frame;
            entry.highest_shot_index = shot_index;
            return ShotVisit::Advanced;
        }
        ++i;
    }
    history.push_back({frame, cycle_id, shot_index, generation});
    return ShotVisit::First;
}
bool permits_shot_effect(ShotVisit visit, std::uint32_t instance_flags) noexcept {
    return visit != ShotVisit::Duplicate &&
           (visit == ShotVisit::First || !(instance_flags & 0x1000));
}
std::size_t update_volley(VolleyProgress& progress, std::vector<ShotHistoryEntry>& history,
                          std::int64_t frame, std::int64_t begin_frame,
                          std::uint16_t begin_count, std::uint32_t instance_flags,
                          VolleyRecordGate record, bool force_one, bool has_weapon,
                          bool has_projectile_resource, bool has_authored_resource,
                          VolleyEffects& effects) {
    if (!record.present || !(record.mode == 3 || (record.allow_instance_mode && (instance_flags & 0x400000))))
        return 0;
    const auto elapsed = frame >= begin_frame ? frame - begin_frame : 0;
    const auto shot_clock = elapsed - progress.time_offset;
    std::size_t attempts = 0;
    while (force_one || (progress.next_shot_time <= shot_clock && progress.shot_index < progress.total_shots)) {
        if (has_weapon) {
            const auto visit = visit_shot(history, frame, begin_count, progress.cycle_id, progress.shot_index);
            if (permits_shot_effect(visit, instance_flags) && has_projectile_resource)
                effects.submit_shot({progress.shot_index, visit, frame});
        }
        // The authored +1E8 resource and progression happen even if an effect was
        // duplicate/suppressed or the required weapon component was absent.
        if (has_authored_resource) effects.trigger_authored_resource();
        ++progress.shot_index;
        progress.emitted_index = progress.shot_index;
        progress.next_shot_time = shot_time_ms(progress.rate, progress.shot_index);
        progress.scheduled_time_copy = progress.next_shot_time;
        ++attempts;
        force_one = false;
    }
    return attempts;
}
bool weapon_begin_gate(runtime::RuntimeInstanceView& instance, runtime::StateRuntime& state,
                        bool has_weapon_component, std::int64_t frame,
                        std::uint16_t instance_id, runtime::StateSchedulingActions& host) {
    if (has_weapon_component && (instance.flags & 8)) return true;
    runtime::schedule_lifetime(instance, state, runtime::LifetimeKind::Finish, frame, 0, instance_id, host);
    return false;
}
void weapon_timer(runtime::RuntimeInstanceView& instance, runtime::StateRuntime& state,
                   std::uint32_t user_data, std::int64_t frame,
                   std::uint16_t instance_id, runtime::StateSchedulingActions& host,
                   WeaponTimerEffects& effects) {
    if (user_data == 1) effects.traverse_timer_plug();
    else if (user_data == 2) {
        effects.update();
        runtime::schedule_lifetime(instance, state, runtime::LifetimeKind::Finish, frame, 0, instance_id, host);
    }
}
BatchResult execute_projectile_batch(ProjectileBatch& batch,
                                     std::span<const std::uint16_t> random_table,
                                     bool job_mode, ProjectileEffects& effects) {
    std::size_t completed = 0;
    for (std::int32_t pellet = 0; pellet < batch.projectile_count; ++pellet) {
        const auto random_slot = (static_cast<std::uint32_t>(batch.shot_index) * 23u
                                + static_cast<std::uint32_t>(batch.seed) * 131u
                                + static_cast<std::uint32_t>(pellet) * 3u) & 0x3fffu;
        batch.random_index = static_cast<std::uint16_t>(random_table[random_slot] & 0x3fff);
        batch.pellet_index = pellet;
        batch.launch = effects.calculate_launch(batch);
        void* entity = nullptr;
        if (static_cast<std::size_t>(pellet) < batch.reusable_entities.size()) {
            auto& id = batch.reusable_entities[static_cast<std::size_t>(pellet)];
            entity = effects.find_entity(id);
            if (entity) {
                id = batch.cleared_entity_id;
                batch.spawn_status = 0;
            }
        }
        if (!entity) entity = effects.create_projectile(batch, job_mode);
        if (!entity) return {completed, true};
        auto& component = effects.projectile_component(entity);
        effects.append_result({batch.launch, &component, batch.random_index});
        component.flag_e2a = false;
        ++completed;
    }
    return {completed, false};
}
} // namespace ow::statescript::gameplay
