#pragma once
#include "../runtime/lifecycle.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ow::statescript::gameplay {
struct WeaponKey {
    std::uint64_t value;
};
// 1078A10/1078BF0: entity[63:32], instance[31:19], state[18:8], sequence[7:0].
WeaponKey weapon_key(std::uint32_t entity_id, std::uint16_t instance_id,
                     std::uint32_t state_index, std::uint8_t sequence) noexcept;
std::uint16_t weapon_seed(std::uint16_t component_seed, std::uint16_t instance_id,
                          std::uint32_t state_index, std::uint16_t begin_count) noexcept;
struct WeaponRecord {
    void* native_object;
    std::uint32_t flags; // +8
};
struct WeaponRecordEntry { WeaponRecord* record; WeaponKey key; };
struct WeaponRecordTable { std::span<const WeaponRecordEntry> buckets[8]; };
WeaponRecord* find_weapon_record(const WeaponRecordTable&, WeaponKey) noexcept;
struct WeaponRecordFactory {
    virtual ~WeaponRecordFactory() = default;
    virtual WeaponRecord& create(WeaponKey, std::uint32_t flags) = 0; // 10451C0
};
// The caller supplies 0x40 for Volley (D45510/D45580). Reuses exact records.
WeaponRecord& acquire_weapon_record(const WeaponRecordTable&, WeaponKey,
                                    std::uint32_t flags, WeaponRecordFactory&);
// Independent logical models, not native layouts. Field names based on arithmetic
// are inferred; the accompanying evidence document gives original offsets.
struct RateRamp {
    float initial = 1.0f;
    float slope = 0.0f;
    float target = 1.0f;
    float duration = 0.0f;
    float transition_count = 0.0f;
};
struct RateDefinition {
    float target;
    float explicit_slope_magnitude;
    float duration;
};
RateRamp make_rate_ramp(float initial, const RateDefinition* definition) noexcept; // 1079570
std::int64_t shot_time_ms(const RateRamp&, std::int32_t shot_index) noexcept; // 1078220

struct WeaponInputs {
    std::int32_t total_shots;
    std::int32_t group_divisor;
    float initial_rate;
    float fraction;
};
struct VolleyProgress {
    std::int32_t total_shots = 0; // state +128
    std::int32_t group_divisor = 1; // +12C
    float fraction = 0.0f; // +130
    RateRamp rate; // +134..144
    std::int32_t shot_index = 0; // +110
    std::int32_t emitted_index = 0; // +104
    std::int64_t scheduled_time_copy = 0; // +108; set alongside +118
    std::int64_t next_shot_time = 0; // +118
    std::int32_t last_timer_index = 0; // +120
    std::int32_t cycle_id = 0; // +124, semantic name inferred
    std::int64_t time_offset = 0; // +E0
};
void initialize_volley(VolleyProgress&, const WeaponInputs&, const RateDefinition*) noexcept;
struct TimerLimits {
    std::int32_t first; // bound record +2C
    std::int32_t second; // bound record +30
};
struct WeaponTimers {
    virtual ~WeaponTimers() = default;
    virtual void schedule(std::int64_t absolute_frame, std::uint32_t user_data) = 0; // F47A50
};
void schedule_volley_timers(VolleyProgress&, TimerLimits, std::int64_t frame,
                            std::int64_t begin_frame, WeaponTimers&); // 1079570

// D46A70: replay/duplicate suppression, NOT a proven network acknowledgement.
struct ShotHistoryEntry {
    std::int64_t frame;
    std::int32_t cycle_id;
    std::int32_t highest_shot_index;
    std::uint16_t begin_generation;
};
enum class ShotVisit { Duplicate = 0, Advanced = 1, First = 2 };
ShotVisit visit_shot(std::vector<ShotHistoryEntry>&, std::int64_t frame,
                      std::uint16_t begin_count, std::int32_t cycle_id,
                      std::int32_t shot_index);
bool permits_shot_effect(ShotVisit, std::uint32_t instance_flags) noexcept;

struct ShotEmission {
    std::int32_t shot_index;
    ShotVisit visit;
    std::int64_t frame;
};
struct VolleyEffects {
    virtual ~VolleyEffects() = default;
    // Concrete job construction/resource resolution is external; the job's actual
    // projectile reuse/create/result loop is execute_projectile_batch below.
    virtual void submit_shot(const ShotEmission&) = 0; // D458EB..D45C13
    virtual void trigger_authored_resource() = 0; // D45C6C..D45C82, authored +1E8
};
struct VolleyRecordGate {
    bool present;
    std::int32_t mode; // bound record +104
    bool allow_instance_mode; // bound record +108
};
// D44D6D core catch-up loop + D456EE unconditional progression. has_weapon and
// has_projectile_resource are decoded ECS/resource boundaries, not defaults.
std::size_t update_volley(VolleyProgress&, std::vector<ShotHistoryEntry>&,
                          std::int64_t frame, std::int64_t begin_frame,
                          std::uint16_t begin_count, std::uint32_t instance_flags,
                          VolleyRecordGate, bool force_one, bool has_weapon,
                          bool has_projectile_resource, bool has_authored_resource, VolleyEffects&);
// 1077B30's unavailable path schedules Finish, never invents weapon/damage work.
bool weapon_begin_gate(runtime::RuntimeInstanceView&, runtime::StateRuntime&,
                        bool has_weapon_component, std::int64_t frame,
                        std::uint16_t instance_id, runtime::StateSchedulingActions&);
// 1077D00: timer 1 traverses authored +1F0; timer 2 updates then schedules Finish.
struct WeaponTimerEffects {
    virtual ~WeaponTimerEffects() = default;
    virtual void traverse_timer_plug() = 0;
    virtual void update() = 0;
};
void weapon_timer(runtime::RuntimeInstanceView&, runtime::StateRuntime&,
                   std::uint32_t user_data, std::int64_t frame,
                   std::uint16_t instance_id, runtime::StateSchedulingActions&,
                   WeaponTimerEffects&);

struct LaunchVector { float x, y, z, w; }; // native job +70, semantic name inferred
struct ProjectileComponent {
    void* native_object;
    bool flag_e2a; // original projectile component +E2A
};
struct ProjectileResult {
    LaunchVector launch;
    ProjectileComponent* component;
    std::uint16_t random_index;
};
struct ProjectileBatch {
    std::int32_t projectile_count; // bound record +24
    std::uint16_t seed; // shot job +A0
    std::int32_t shot_index; // +A8
    float parameter_a4; // +A4; full authored meaning unresolved
    std::uint8_t parameter_ac; // +AC
    std::span<std::uint32_t> reusable_entities; // +B0/+B8
    std::uint32_t cleared_entity_id; // m_D815B771, observed zero
    std::uint32_t spawn_status = 0; // +D0
    std::int32_t pellet_index = 0; // +48
    std::uint16_t random_index = 0; // +40
    LaunchVector launch{}; // +70
};
struct ProjectileEffects {
    virtual ~ProjectileEffects() = default;
    virtual LaunchVector calculate_launch(const ProjectileBatch&) = 0; // 1078340
    virtual void* find_entity(std::uint32_t id) = 0; // original entity-admin lookup
    virtual void* create_projectile(ProjectileBatch&, bool job_mode) = 0; // D354B0
    virtual ProjectileComponent& projectile_component(void* entity) = 0; // ECS bit 39
    virtual void append_result(const ProjectileResult&) = 0; // job result array +E0
};
struct BatchResult { std::size_t completed; bool creation_failed; };
// D45240. table must contain 16384 entries (original unk_18449F0, stride four).
// A failed create stops the whole batch; successful reuse consumes its ID slot.
std::span<const std::uint16_t> projectile_random_table() noexcept; // captured low 14 bits
BatchResult execute_projectile_batch(ProjectileBatch&, std::span<const std::uint16_t> random_table,
                                     bool job_mode, ProjectileEffects&);
struct TrackedProjectile {
    std::int32_t current_id; // native 80-byte record +40
    std::int32_t previous_id; // +44
};
struct ProjectileDeletion {
    virtual ~ProjectileDeletion() = default;
    virtual void* find(std::int32_t entity_id) = 0;
    virtual void erase(void* entity) = 0; // EntityAdmin__DeleteEntity(..., 0)
};
// D47550: deletion is a real client effect. Absent entities still rotate IDs.
void delete_tracked_projectiles(std::span<TrackedProjectile>, ProjectileDeletion&);
} // namespace ow::statescript::gameplay
