#include "lifecycle.h"

namespace ow::statescript::runtime {
namespace {
constexpr std::uint32_t membership_bits[] = {0x80, 0x100, 0x400};
void abort_group(RuntimeInstanceView& instance, IndexRange indices,
                 std::int64_t frame, EngineActions& engine, LifecycleActions& hooks) {
    if (indices.count == 0) return;
    for (std::size_t i = 0; i < indices.count; ++i) {
        if (auto* other = state_at(instance, indices.data[i])) engine.abort_state(*other);
    }
    hooks.cancel_pending_begins(indices, frame);
}
void fire(RuntimeInstanceView& instance, StateRuntime& state, const AuthoredNode& sender,
          PlugRange plug, std::uintptr_t offset, EngineActions& engine) {
    activate_plug(instance, sender, plug.data, plug.count, engine, state.native_object, offset);
}
}
void StateLists::add(StateRuntime& state, StateListKind kind) noexcept {
    const auto index = static_cast<unsigned>(kind);
    const auto bit = membership_bits[index];
    if (state.flags & bit) return;
    auto& list = lanes[index][state.scoped ? 1 : 0];
    auto& link = state.links[index];
    link.next = nullptr;
    link.prev = list.tail;
    if (list.tail) list.tail->links[index].next = &state;
    else list.head = &state;
    list.tail = &state;
    ++list.count;
    state.flags |= bit;
}
void StateLists::remove(StateRuntime& state, StateListKind kind) noexcept {
    const auto index = static_cast<unsigned>(kind);
    const auto bit = membership_bits[index];
    if (!(state.flags & bit)) return;
    auto& list = lanes[index][state.scoped ? 1 : 0];
    auto& link = state.links[index];
    if (link.prev) link.prev->links[index].next = link.next;
    else list.head = link.next;
    if (link.next) link.next->links[index].prev = link.prev;
    else list.tail = link.prev;
    link.next = link.prev = nullptr;
    --list.count;
    state.flags &= ~bit;
}
void StateLists::attach(StateRuntime& state) noexcept {
    // Detached bits describe requested membership, not already linked nodes.
    const auto requested = state.flags;
    state.flags &= ~(0x80u | 0x100u | 0x400u);
    for (unsigned i = 0; i != 3; ++i) {
        if (requested & membership_bits[i]) add(state, static_cast<StateListKind>(i));
    }
}

bool begin_state(RuntimeInstanceView& instance, StateRuntime& state, const StateDefinition& authored,
                 std::int64_t current_frame, std::int64_t elapsed_frames,
                 EngineActions& engine, LifecycleActions& hooks) {
    instance.current_execution_tag = authored.node.execution_tag;
    if (authored.node.scoped) engine.enter_scope();
    if (instance.state_activity && state.state_index < instance.state_count)
        instance.state_activity[state.state_index] = authored.node.activity_mask;
    instance.activity_mask |= authored.node.activity_mask;
    if (is_active(state)) {
        hooks.manager_event(state, ManagerStateEvent::Reenter);
        hooks.state_hook(state, StateHook::Reenter118);
    } else {
        ++state.begin_count;
        state.timer_frame_a = state.timer_frame_b = -1;
        hooks.manager_event(state, ManagerStateEvent::Begin);
        abort_group(instance, authored.abort_on_begin, current_frame, engine, hooks);
        state.flags = (state.flags & ~0x11u) | 1;
        state.local_counter = 0;
        state.begin_frame = current_frame - elapsed_frames;
        hooks.state_hook(state, StateHook::Subscribe);
        hooks.state_hook(state, StateHook::PreBegin160, (instance.flags & 0x1000) != 0);
        hooks.state_hook(state, StateHook::Begin108);
        fire(instance, state, authored.node, authored.begin_plug, 0xE0, engine);
        fire(instance, state, authored.node, authored.active_plug, 0xF0, engine);
        hooks.state_hook(state, StateHook::Begin110);
        hooks.state_hook(state, StateHook::EvaluateExits);
    }
    if (authored.node.scoped) engine.leave_scope();
    return true; // F495C0's generic implementation always succeeds
}

bool end_state(RuntimeInstanceView& instance, StateRuntime& state, const StateDefinition& authored,
               StateLists& lists, std::int64_t current_frame, bool abort,
               EngineActions& engine, LifecycleActions& hooks) {
    if (!can_finish(state)) return false;
    state.flags |= 2;
    instance.current_execution_tag = authored.node.execution_tag;
    if (instance.state_activity && state.state_index < instance.state_count)
        instance.state_activity[state.state_index] = 0;
    instance.activity_dirty = true;
    if (authored.node.scoped) engine.enter_scope();
    hooks.manager_event(state, ManagerStateEvent::End);
    hooks.cancel_state_notifications(state.state_index);
    // F484E0 suppresses the +F0 abort group during the 0x1000 mode unless
    // the authored state is client-only. It is not a blanket client/server gate.
    if (!(instance.flags & 0x1000) || authored.node.scoped)
        abort_group(instance, authored.abort_on_end, current_frame, engine, hooks);
    hooks.state_hook(state, abort ? StateHook::Abort150 : StateHook::Finish148);
    hooks.state_hook(state, StateHook::End158);
    const int reason = (instance.flags & 0x1000) ? 6 : (abort ? 0 : 1);
    hooks.state_hook(state, StateHook::PreEnd168, reason);
    hooks.state_hook(state, abort ? StateHook::Abort248 : StateHook::Finish250);
    fire(instance, state, authored.node, authored.end_plug, 0xE8, engine);
    if (!hooks.retain_subscriptions(state)) hooks.state_hook(state, StateHook::Unsubscribe);
    state.flags &= ~3u;
    state.end_frame = current_frame;
    if (!hooks.retain_memberships(state)) {
        lists.remove(state, StateListKind::Family80);
        lists.remove(state, StateListKind::Family100);
        if (state.flags & 0x200) {
            state.flags &= ~0x200u;
            hooks.remove_special_update(state);
        }
        lists.remove(state, StateListKind::Family400);
    }
    if (authored.node.scoped) engine.leave_scope();
    return true;
}

void* resolve_action(RuntimeInstanceView& instance, const AuthoredNode& node,
                     const ImplementationTable& registry, std::uintptr_t exact_key,
                     bool derives_from_terminator, std::uintptr_t terminator_key) noexcept {
    if (!instance.actions || node.node_index >= instance.action_count) return nullptr;
    auto*& slot = instance.actions[node.node_index];
    if (slot) return slot;
    slot = lookup_implementation(registry, exact_key);
    if (!slot && derives_from_terminator)
        slot = lookup_implementation(registry, terminator_key);
    return slot; // a miss stays null and is queried again next time, as in F4E1A0
}
} // namespace ow::statescript::runtime
