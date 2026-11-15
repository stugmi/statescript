#pragma once
#include "execution.h"

namespace ow::statescript::runtime {
// Names for the three list families are deliberately neutral: full manager
// iteration semantics are not yet recovered. Each has normal/client-only lanes.
enum class StateListKind : unsigned { Family80, Family100, Family400 };
struct StateList {
    StateRuntime* head = nullptr;
    StateRuntime* tail = nullptr;
    std::size_t count = 0;
};
struct StateLists {
    StateList lanes[3][2]; // instance +168/+180, +198/+1B0, +1C8/+1E0
    void add(StateRuntime&, StateListKind) noexcept;
    void remove(StateRuntime&, StateListKind) noexcept;
    void attach(StateRuntime&) noexcept; // F463D0: materialize detached membership bits
};
struct IndexRange { const std::uint32_t* data = nullptr; std::size_t count = 0; };
struct PlugRange { const LinkTarget* data = nullptr; std::size_t count = 0; };
struct StateDefinition {
    AuthoredNode node;
    IndexRange abort_on_begin; // authored state +0x98
    IndexRange abort_on_end; // authored state +0xF0 -> plug +0x28
    PlugRange begin_plug; // +0xE0
    PlugRange end_plug; // +0xE8 (both Finish and Abort)
    PlugRange active_plug; // +0xF0
};
// Preserve unknown virtual names instead of inventing a gameplay meaning.
// Concrete weapon/ability behavior belongs to these implementations, not the
// generic lifecycle. Values are original vtable BYTE offsets.
enum class StateHook : std::uint16_t {
    Initialize = 0x100, Begin108 = 0x108, Begin110 = 0x110,
    Reenter118 = 0x118, Finish148 = 0x148, Abort150 = 0x150,
    End158 = 0x158, PreBegin160 = 0x160, PreEnd168 = 0x168,
    EvaluateExits = 0x1E8, Subscribe = 0x218, Unsubscribe = 0x220,
    Abort248 = 0x248, Finish250 = 0x250
};
enum class ManagerStateEvent { Begin, Reenter, End };
struct LifecycleActions {
    virtual ~LifecycleActions() = default;
    virtual void state_hook(StateRuntime&, StateHook, int argument = 0) = 0;
    virtual void manager_event(StateRuntime&, ManagerStateEvent) = 0; // manager +120/+128/+138
    virtual void cancel_state_notifications(std::uint32_t state_index) = 0; // instance +168
    virtual void cancel_pending_begins(IndexRange, std::int64_t frame) = 0; // F4FD00
    virtual bool retain_subscriptions(StateRuntime&) = 0; // state +210
    virtual bool retain_memberships(StateRuntime&) = 0; // first virtual-base capability
    virtual void remove_special_update(StateRuntime&) = 0; // F48370(false), manager +3E0
};
// No allocation; host owns concrete state objects, graph decoding and lists.
// Lifecycle hooks may recursively activate/end other states. Flags are updated
// in original order so the finishing bit prevents reentrant double termination.
bool begin_state(RuntimeInstanceView&, StateRuntime&, const StateDefinition&,
                 std::int64_t current_frame, std::int64_t elapsed_frames,
                 EngineActions&, LifecycleActions&);
bool end_state(RuntimeInstanceView&, StateRuntime&, const StateDefinition&,
               StateLists&, std::int64_t current_frame, bool abort,
               EngineActions&, LifecycleActions&);
// F4E1A0: shared action implementation cache, exact type then only the proven
// STU action-terminator fallback. This is NOT ancestor factory traversal.
void* resolve_action(RuntimeInstanceView&, const AuthoredNode&,
                     const ImplementationTable&, std::uintptr_t exact_key,
                     bool derives_from_terminator, std::uintptr_t terminator_key) noexcept;
struct StateConstructionActions {
    virtual ~StateConstructionActions() = default;
    virtual void* allocate_base_state() = 0; // F47920, 0xB8 bytes in client
    virtual StateRuntime& project(void* native_object) = 0; // preserve concrete ctor fields
    virtual bool capability(StateRuntime&, unsigned index) = 0; // two virtual bases
    virtual void register_capability(StateRuntime&, unsigned index) = 0; // manager +190/+1A0
    virtual void debug_node(StateRuntime&, const AuthoredNode&) = 0; // state +1A8
    virtual void initialize(StateRuntime&) = 0; // state +100
};
// F4E230. Null slots are lazy; the client really allocates the generic base
// when a type has no factory. This is NOT reconstructed server gameplay.
StateRuntime* instantiate_state(RuntimeInstanceView&, const AuthoredNode&,
                                 const ImplementationTable&, std::uintptr_t exact_key,
                                 const InheritanceLink* ancestors, StateLists&,
                                 StateConstructionActions&);
enum class LifetimeKind : std::uint16_t { Begin = 0, Finish = 1, Abort = 2 };
struct StateSchedulingActions {
    virtual ~StateSchedulingActions() = default;
    virtual bool can_schedule(std::uint32_t state_index) = 0; // instance +170
    virtual Notification& allocate_lifetime() = 0; // manager +A0, type zero
    virtual void submit(Notification&) = 0; // instance +140; owns the notification
    virtual bool consume_input(StateRuntime&, const void* input_plug) = 0; // state +240
};
// F4A9C0, called by base state's +1D0/+1D8 slots with Begin/Abort.
bool schedule_lifetime(RuntimeInstanceView&, StateRuntime&, LifetimeKind,
                       std::int64_t current_frame, std::int64_t delay,
                       std::uint16_t instance_id, StateSchedulingActions&);
// F48700. abort_input is the proven STU input RTTI test at input vtbl +18
// against inheritance key 0x1766570, decoded by the host.
bool execute_state_input(RuntimeInstanceView&, StateRuntime&, const void* input_plug,
                         bool abort_input, void* sender_runtime,
                         std::int64_t current_frame, std::uint16_t instance_id,
                         StateSchedulingActions&);
// F4FD00 / F4FE30: cancel ONLY matching lifetime events at this exact frame.
// Caller supplies a sorted queue: originals stop at the first future event.
std::size_t cancel_pending_begins(RuntimeInstanceView&, NotificationQueue&, IndexRange,
                                 std::int64_t frame, SchedulerActions&);
std::size_t cancel_pending_finish(RuntimeInstanceView&, NotificationQueue&,
                                 std::uint32_t state_index, std::int64_t frame,
                                 SchedulerActions&);
} // namespace ow::statescript::runtime
