#include "execution.h"

namespace ow::statescript::runtime {
StateRuntime* state_at(const RuntimeInstanceView& instance, std::uint32_t index) noexcept {
    return instance.states && index < instance.state_count ? instance.states[index] : nullptr;
}
void* action_at(const RuntimeInstanceView& instance, std::uint32_t index) noexcept {
    return instance.actions && index < instance.action_count ? instance.actions[index] : nullptr;
}
bool can_finish(const StateRuntime& state) noexcept { return (state.flags & 3) == 1; }
bool is_active(const StateRuntime& state) noexcept { return (state.flags & 1) != 0; }
int logical_button_match(int event_button, bool going_down, int resolved_button,
                         bool on_down, bool on_up, bool unconditional_a,
                         bool unconditional_b) noexcept {
    if (event_button != resolved_button) return 0;
    if (unconditional_a || unconditional_b) return 2;
    return (on_down && going_down) || (on_up && !going_down);
}

void NotificationQueue::append(Notification& event, std::int64_t current_frame) noexcept {
    if (event.frame < current_frame) event.frame = current_frame;
    event.next = nullptr;
    event.prev = head;
    if (head) head->next = &event;
    else tail = &event;
    head = &event;
    ++count;
}
void NotificationQueue::insert_sorted(Notification& event, std::int64_t current_frame) noexcept {
    if (event.frame < current_frame) event.frame = current_frame;
    // The original head is the next event to execute; next points towards head.
    auto* position = tail;
    while (position && position->frame > event.frame) position = position->next;
    if (!position) { append(event, current_frame); return; }
    event.prev = position->prev;
    event.next = position;
    if (position->prev) position->prev->next = &event;
    else tail = &event;
    position->prev = &event;
    ++count;
}
void NotificationQueue::remove(Notification& event) noexcept {
    if (event.prev) event.prev->next = event.next;
    else tail = event.next;
    if (event.next) event.next->prev = event.prev;
    else head = event.prev;
    event.next = event.prev = nullptr;
    --count;
}
Notification* NotificationQueue::pop_due(std::int64_t current_frame) noexcept {
    if (!head || head->frame > current_frame) return nullptr;
    auto* event = head;
    remove(*event);
    return event;
}

bool activate_link(RuntimeInstanceView& instance, const AuthoredNode& sender,
                   const AuthoredNode* target, EngineActions& engine,
                   const void* input_plug, void* sender_runtime,
                   std::uintptr_t output_plug, std::int32_t link_index) {
    if (!target) return true;
    if ((instance.flags & 4) || !engine.condition(*target)) return false;
    instance.current_execution_tag = target->execution_tag;
    instance.sender_node_index = sender.node_index;
    instance.output_plug = output_plug;
    instance.link_index = link_index;
    if (target->is_state) {
        auto* state = state_at(instance, target->state_index);
        if (!state) state = engine.instantiate_state(target->state_index);
        return !state || engine.execute_node(state->native_object, *target, input_plug, sender_runtime);
    }
    auto* implementation = engine.instantiate_action(target->node_index);
    if (!implementation) return true; // absent client implementation is explicitly tolerated
    if (target->scoped) engine.enter_scope();
    const bool result = engine.execute_node(implementation, *target, input_plug, sender_runtime);
    if (target->scoped) engine.leave_scope();
    return result;
}
bool activate_plug(RuntimeInstanceView& instance, const AuthoredNode& sender,
                   const LinkTarget* links, std::size_t count, EngineActions& engine,
                   void* sender_runtime, std::uintptr_t output_plug) {
    bool any = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (activate_link(instance, sender, links[i].node, engine, links[i].input_plug,
                          sender_runtime, output_plug, -1)) any = true;
    }
    return any;
}

bool execute_notification(RuntimeInstanceView& instance, const Notification& event, EngineActions& engine) {
    switch (event.type) {
    case NotificationType::Lifetime: {
        auto* state = state_at(instance, event.state_index);
        if (!state) {
            if (!engine.authored_state(event.state_index)) return true;
            state = engine.instantiate_state(event.state_index);
        }
        if (!state) return true; // allocation failure belongs to the host
        switch (event.payload & 0xffffu) {
        case 0: return engine.begin_state(*state, event);
        case 1: engine.finish_state(*state); break;
        case 2: engine.abort_state(*state); break;
        default: break;
        }
        return true;
    }
    case NotificationType::Timer: {
        auto* state = state_at(instance, event.timer_state_index);
        if (!state || !is_active(*state)) return true;
        if (state->scoped) engine.enter_scope();
        const auto saved_tag = instance.current_execution_tag;
        const auto* authored = engine.authored_state(state->state_index);
        if (authored) instance.current_execution_tag = authored->execution_tag;
        engine.timer(*state, event.payload);
        engine.evaluate_exits(*state);
        instance.current_execution_tag = saved_tag;
        if (state->scoped) engine.leave_scope();
        return true;
    }
    case NotificationType::EvaluateExits: {
        auto* state = state_at(instance, event.payload);
        if (state && is_active(*state)) engine.evaluate_exits(*state);
        return true;
    }
    case NotificationType::TraverseFrom: engine.traverse_from(event.payload, event); return true;
    case NotificationType::SetVariable:
    case NotificationType::SetVariableArray:
        if (event.control & 4) engine.enter_scope();
        engine.set_variable(event, event.type == NotificationType::SetVariableArray);
        if (event.control & 4) engine.leave_scope();
        return true;
    case NotificationType::StopInstance: engine.stop_instance(); return true;
    case NotificationType::LogicalButton: engine.logical_button(event); return true;
    case NotificationType::AttributeChanged: {
        auto* state = state_at(instance, event.payload);
        if (!state || !is_active(*state)) return true;
        if (state->scoped) engine.enter_scope();
        engine.attribute_changed(*state);
        if (state->scoped) engine.leave_scope();
        return true;
    }
    }
    return true;
}

PumpResult pump_notifications(RuntimeInstanceView& instance, NotificationQueue& queue,
                              std::int64_t frame, bool client_only,
                              EngineActions& engine, SchedulerActions& scheduler) {
    std::int64_t batch_frame = 0;
    unsigned executed = 0;
    while (queue.head && queue.head->frame <= frame) {
        auto* event = queue.head;
        if (client_only && !(event->control & 4)) {
            const auto index = scheduler.associated_state(*event);
            auto* state = index < 0 ? nullptr : state_at(instance, static_cast<std::uint32_t>(index));
            if (!state || !state->scoped) {
                scheduler.reset_frame_traversal();
                return PumpResult::ScopeBlocked;
            }
        }
        queue.remove(*event);
        instance.flags |= 0x80;
        if (batch_frame != event->frame) {
            if (batch_frame != 0) scheduler.reset_frame_traversal();
            batch_frame = event->frame;
            executed = 0;
        }
        if (!execute_notification(instance, *event, engine)) {
            // F4F6EB reattaches a failed Begin at head without changing its time.
            event->next = nullptr;
            event->prev = queue.head;
            if (queue.head) queue.head->next = event;
            else queue.tail = event;
            queue.head = event;
            ++queue.count;
            instance.flags |= 0x80;
            return PumpResult::Deferred;
        }
        scheduler.release(*event);
        if (instance.flags & 4) return PumpResult::Stopped;
        if (++executed > 1000) {
            instance.flags &= ~0x100000u;
            engine.stop_instance();
            return PumpResult::Runaway;
        }
        if (executed == 980) {
            scheduler.runaway_warning();
            instance.flags |= 0x100000;
        }
    }
    scheduler.reset_frame_traversal();
    return PumpResult::Drained;
}
} // namespace ow::statescript::runtime
