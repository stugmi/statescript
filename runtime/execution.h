#pragma once
#include "lookup.h"
#include <cstddef>
#include <cstdint>

namespace ow::statescript::runtime {

// Narrow projections, NOT replacements for the full client/STU object layouts.
// Offsets belong to the original objects: authored node +0x68/+0x74/+0x75/+0xF8;
// runtime state +0x40/+0x70/+0x78/+0x80; instance +0xA0/+0xD0/+0x160/+0x258.
struct AuthoredNode {
    const void* stu_object;
    std::uint32_t node_index; // +0x68
    bool is_state;             // +0x74
    bool scoped;               // +0x75; F471F0 checks +0x75
    std::uint32_t state_index; // +0xF8 when is_state
    std::uint32_t execution_tag = 0; // +0x70, manager current-execution context
    std::uint32_t activity_mask = 0; // state +0xFC
    std::uint32_t unique_id_low = 0; // low dword of authored +0x30, copied by F4E230
};
struct RuntimeInstanceView;
struct StateRuntime;
struct StateLink { StateRuntime* next = nullptr; StateRuntime* prev = nullptr; };
struct StateRuntime {
    void* native_object;
    std::uint32_t flags; // runtime state +0x40: 1 active, 2 finishing, 0x80/0x100/0x400 list links
    std::uint32_t state_index; // runtime state +0x70
    std::uint32_t unique_id_low = 0; // runtime state +0x74
    bool scoped; // runtime state +0x80; selects the second set of manager lists
    RuntimeInstanceView* instance = nullptr; // logical owner, original state +0x78
    std::uint16_t begin_count = 0; // +0x44, word increment at F49775
    std::int64_t begin_frame = 0; // +0x48
    std::int64_t end_frame = -1; // +0x50
    std::int64_t timer_frame_a = -1; // +0x58
    std::int64_t timer_frame_b = -1; // +0x60
    std::uint64_t local_counter = 0; // +0x68, qword clear at F498A1
    StateLink links[3]; // +0x10/+0x20/+0x30, independent intrusive memberships
};
struct RuntimeInstanceView {
    StateRuntime** states; // instance +0xA0, state slot indexed by STU graph m_states
    std::size_t state_count; // +0xA8
    void** actions; // instance +0xD0, slot indexed by STU graph m_nodes index
    std::size_t action_count; // +0xD8
    std::uint32_t flags; // instance +0x258 (not the state flags)
    std::uint32_t* state_activity = nullptr; // +0xB8
    std::uint32_t activity_mask = 0; // +0x100, OR accumulation
    bool activity_dirty = false; // +0x104, recompute after removing a state
    std::uint32_t sender_node_index = 0; // +0x108
    std::uintptr_t output_plug = 0; // +0x110, discriminator, not a plug pointer
    std::int32_t link_index = -1; // +0x118
    std::uint32_t current_execution_tag = 0; // manager +0x3D0
};

StateRuntime* state_at(const RuntimeInstanceView& instance, std::uint32_t index) noexcept;
void* action_at(const RuntimeInstanceView& instance, std::uint32_t index) noexcept;
bool can_finish(const StateRuntime& state) noexcept; // 0xF499D0/0xF49DF0
bool is_active(const StateRuntime& state) noexcept;
// RVA 0xF66640: logical-button event versus an authored-state filter.
// The resource reference at authored state +0xA0 is resolved outside this pure predicate.
int logical_button_match(int event_button, bool going_down, int resolved_button,
                         bool on_down, bool on_up, bool unconditional_a,
                         bool unconditional_b) noexcept;

// Original notification object has vfptr +0x00, next +0x08, prev +0x10,
// command-frame timestamp +0x18, instance id +0x20, type +0x22,
// control bits +0x28 and type-dependent payload +0x30/+0x34.
enum class NotificationType : std::uint8_t {
    Lifetime = 0, Timer = 1, EvaluateExits = 2, TraverseFrom = 3,
    SetVariable = 4, SetVariableArray = 5, StopInstance = 6,
    LogicalButton = 7, AttributeChanged = 8
};
struct Notification {
    Notification* next = nullptr;
    Notification* prev = nullptr;
    std::int64_t frame = 0;
    std::uint16_t instance_id = 0;
    NotificationType type = NotificationType::Lifetime;
    std::uint32_t control = 0; // +0x28; bit 4 bypasses client-scope pause at 0xF4EF60
    std::uint32_t payload = 0; // +0x30: lifetime kind / timer id / index / button
    std::uint16_t state_index = 0; // +0x32 lifetime event (overlaps upper half of payload)
    std::uint16_t timer_state_index = 0; // +0x34 timer event
    bool going_down = false; // +0x34 for LogicalButton, independent of timer payload
};

// Intrusive queue. The caller owns notification allocation and deallocation.
struct NotificationQueue {
    Notification* head = nullptr; // instance +0xE8
    Notification* tail = nullptr; // instance +0xF0
    std::size_t count = 0; // instance +0xF8
    void insert_sorted(Notification& event, std::int64_t current_frame) noexcept; // 0xF4F310
    void append(Notification& event, std::int64_t current_frame) noexcept; // 0xF4F420
    void remove(Notification& event) noexcept;
    Notification* pop_due(std::int64_t current_frame) noexcept; // 0xF4EF60 extraction
};

// Genuine external engine boundary: concrete state virtual functions, expression
// evaluator, varbag, network/simulation and STU node implementations are not here.
// A host must supply behavior; none of these operations silently succeeds.
struct EngineActions {
    virtual ~EngineActions() = default;
    virtual StateRuntime* instantiate_state(std::uint32_t state_index) = 0; // F4E230
    virtual const AuthoredNode* authored_state(std::uint32_t state_index) = 0; // graph m_states
    virtual void* instantiate_action(std::uint32_t node_index) = 0; // F4E1A0
    virtual bool begin_state(StateRuntime&, const Notification&) = 0; // F495C0
    virtual void finish_state(StateRuntime&) = 0; // F499D0
    virtual void abort_state(StateRuntime&) = 0; // F49DF0
    virtual void tick_state(StateRuntime&, std::int64_t command_frame) = 0; // state vtbl +0x138
    virtual void timer(StateRuntime&, std::uint32_t user_data) = 0; // +0x140
    virtual void evaluate_exits(StateRuntime&) = 0; // +0x1E8
    virtual void attribute_changed(StateRuntime&) = 0; // +0x170
    virtual void traverse_from(std::uint32_t node_index, const Notification&) = 0;
    virtual void set_variable(const Notification&, bool array) = 0; // varbag +0xE0/0x130
    virtual void stop_instance() = 0;
    virtual void logical_button(const Notification&) = 0;
    virtual bool condition(const AuthoredNode& target) = 0; // INSTANCE vtbl +0x88
    virtual bool execute_node(void* implementation, const AuthoredNode& target,
                              const void* input_plug, void* sender_runtime) = 0; // node +0xF8
    virtual void enter_scope() = 0; // instance vtbl +0x198
    virtual void leave_scope() = 0; // +0x1A0
};

// Execute one notification from the actual 0xF4F600 type switch. False is a
// *deferred lifetime* event, not an error: caller must retain the event.
bool execute_notification(RuntimeInstanceView&, const Notification&, EngineActions&);
// RVA 0xF471F0: empty link succeeds; blocked sender/predicate fails; otherwise
// select a state or action runtime implementation and enter its executable slot.
bool activate_link(RuntimeInstanceView&, const AuthoredNode& sender,
                   const AuthoredNode* target, EngineActions&,
                   const void* input_plug = nullptr, void* sender_runtime = nullptr,
                   std::uintptr_t output_plug = 0, std::int32_t link_index = -1);
struct LinkTarget {
    const AuthoredNode* node;
    const void* input_plug;
};
// RVA F46F80: evaluate EVERY link, OR the results (never short-circuit).
bool activate_plug(RuntimeInstanceView&, const AuthoredNode& sender,
                   const LinkTarget* links, std::size_t count, EngineActions&,
                   void* sender_runtime, std::uintptr_t output_plug);

// A paused client-only pump blocks at the first due non-client notification;
// it must not skip that event to execute later ones. Notification vtbl +0x10
// supplies the associated state index (not always the event payload).
enum class PumpResult { Drained, ScopeBlocked, Deferred, Stopped, Runaway };
struct SchedulerActions {
    virtual ~SchedulerActions() = default;
    virtual std::int32_t associated_state(const Notification&) = 0;
    virtual void release(Notification&) = 0; // concrete notification destructor/allocator
    virtual void reset_frame_traversal() = 0; // instance +0x120 traversal scratch
    virtual void runaway_warning() = 0;
};
PumpResult pump_notifications(RuntimeInstanceView&, NotificationQueue&,
                              std::int64_t frame, bool client_only,
                              EngineActions&, SchedulerActions&);
} // namespace ow::statescript::runtime
