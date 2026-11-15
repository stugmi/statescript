#include "lifecycle.h"

namespace ow::statescript::runtime {
bool schedule_lifetime(RuntimeInstanceView& instance, StateRuntime& state, LifetimeKind kind,
                       std::int64_t current_frame, std::int64_t delay,
                       std::uint16_t instance_id, StateSchedulingActions& host) {
    if (!host.can_schedule(state.state_index) || (instance.flags & 4)) return false;
    auto& event = host.allocate_lifetime();
    event.frame = current_frame + delay;
    event.instance_id = instance_id;
    event.type = NotificationType::Lifetime;
    event.payload = static_cast<std::uint16_t>(kind);
    event.state_index = static_cast<std::uint16_t>(state.state_index);
    host.submit(event);
    return true;
}
bool execute_state_input(RuntimeInstanceView& instance, StateRuntime& state, const void* input_plug,
                         bool abort_input, void* sender_runtime,
                         std::int64_t current_frame, std::uint16_t instance_id,
                         StateSchedulingActions& host) {
    if (abort_input) {
        schedule_lifetime(instance, state, LifetimeKind::Abort, current_frame, 0, instance_id, host);
        return true;
    }
    if (host.consume_input(state, input_plug)) return true;
    schedule_lifetime(instance, state, LifetimeKind::Begin, current_frame, 0, instance_id, host);
    if (sender_runtime == state.native_object) state.flags |= 4;
    return true;
}
namespace {
template<class Matches>
std::size_t cancel_at_frame(RuntimeInstanceView& instance, NotificationQueue& queue,
                            std::int64_t frame, SchedulerActions& host, Matches matches) {
    std::size_t removed = 0;
    for (auto* event = queue.head; event;) {
        if (event->frame > frame) break;
        auto* following = event->prev;
        if (event->frame == frame && event->type == NotificationType::Lifetime && matches(*event)) {
            queue.remove(*event);
            host.release(*event);
            instance.flags |= 0x80;
            ++removed;
        }
        event = following;
    }
    return removed;
}
}
std::size_t cancel_pending_begins(RuntimeInstanceView& instance, NotificationQueue& queue,
                                 IndexRange indices, std::int64_t frame, SchedulerActions& host) {
    return cancel_at_frame(instance, queue, frame, host, [indices](const Notification& event) {
        if ((event.payload & 0xffffu) != 0) return false;
        for (std::size_t i = 0; i < indices.count; ++i) {
            if (event.state_index == indices.data[i]) return true;
        }
        return false;
    });
}
std::size_t cancel_pending_finish(RuntimeInstanceView& instance, NotificationQueue& queue,
                                 std::uint32_t state_index, std::int64_t frame, SchedulerActions& host) {
    return cancel_at_frame(instance, queue, frame, host, [state_index](const Notification& event) {
        return (event.payload & 0xffffu) == 1 && event.state_index == state_index;
    });
}
} // namespace ow::statescript::runtime
