#pragma once
#include <cstddef>
#include <cstdint>

namespace ow::statescript::simulation {
struct Vec3 { float x, y, z; };
struct CameraView {
    Vec3 position; // original camera transform +30
    Vec3 forward; // original camera transform +20; not normalized here
};
// Semantic component projection, NOT an ABI overlay. object identifies the host
// component. These mutable fields are the scheduler's actual model state; a raw
// client adapter must explicitly synchronize them with the original object.
struct ComponentView {
    void* object;
    std::uint32_t instance_count; // StatescriptComponent +1B8
    bool always_eligible; // +419, descriptive name inferred
    std::uint32_t context_interest_mask; // +42C, descriptive name inferred
    std::uint32_t deferred_context_mask; // +500
    bool was_deferred; // +504
};
struct WorkItem {
    ComponentView* component;
    std::int64_t scheduling_allowance; // +8; passed in R8 to D084A0, unused there
    float priority; // +10, highest first
};
struct FrameContext {
    std::uint32_t simulation_frame;
    std::uint32_t context_mask; // admin context component +1C
    bool include_context_candidates; // original tick flags &2
    std::int32_t allowance_multiplier; // signed timing-context +28 (timing +78)
    float seconds_per_counter_tick = 1.0000000116860974e-7f; // global181F544, caller refreshes
    CameraView camera;
};
struct Services {
    virtual ~Services() = default;
    virtual bool is_local_aim_entity(ComponentView&) = 0; // D13700, component+C0 manager
    // Return false ONLY for absent SceneRenderingComponent (entity bit1).
    virtual bool world_position(const ComponentView&, Vec3& out) = 0; // 964BC0
    virtual std::uint32_t performance_counter_low() = 0; // low32 QueryPerformanceCounter
    // allowance is intentional scheduler data but NOT a consumed VM time budget.
    virtual void execute(ComponentView&, std::uint32_t simulation_frame,
                         std::int64_t unused_scheduling_allowance) = 0; // D084A0
};
inline constexpr float mandatory_priority = 101.0f;
inline constexpr float frame_budget_seconds = 0.0012000000569969416f;
// C2FBB0. Local aim, previously deferred, or zero instances =>101. No scene=>0.
// Tiny/NaN squared distance uses original direction(0,0,1) and distance -1.
float priority_for(const ComponentView&, bool is_local_aim,
                    const CameraView&, Services&);
// C30970 semantic ordering: descending, no stability/tie-order guarantee.
// Allocation-free min-heap sort replaces the original partition/insertion code.
void sort_descending(WorkItem* work, std::size_t count) noexcept;

enum class FrameStatus { Completed, BudgetExceeded, InsufficientWorkStorage };
struct FrameResult {
    FrameStatus status;
    std::size_t candidates;
    std::size_t processed;
    std::size_t deferred;
};
// C2FD80 scheduling proper; caller separately performs its C306A0 context-change
// prepass. Every component is classified once, in input enumeration order.
// Work may be overwritten. On insufficient capacity, candidates reports required
// capacity; no execute/clock calls or deferred-field mutations occur. Predicate
// callbacks may still fill their own D13700 classification cache.
// Inputs are valid, non-null component pointers; work has capacity elements (or
// may be null when capacity==0). Component list and work storage must not alias.
FrameResult execute_frame(ComponentView* const* components, std::size_t count,
                           WorkItem* work, std::size_t capacity,
                           const FrameContext&, Services&);
} // namespace ow::statescript::simulation
