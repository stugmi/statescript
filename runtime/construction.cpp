#include "lifecycle.h"

namespace ow::statescript::runtime {
StateRuntime* instantiate_state(RuntimeInstanceView& instance, const AuthoredNode& authored,
                                 const ImplementationTable& registry, std::uintptr_t exact_key,
                                 const InheritanceLink* ancestors, StateLists& lists,
                                 StateConstructionActions& host) {
    if (!instance.states || authored.state_index >= instance.state_count) return nullptr;
    auto*& slot = instance.states[authored.state_index];
    if (slot) return slot; // caller-side lazy lookup in F471F0/F4F600
    auto* native = create_for_type(registry, exact_key, ancestors);
    if (!native) native = host.allocate_base_state();
    if (!native) return nullptr; // no replacement gameplay on allocation failure
    auto& state = host.project(native);
    state.native_object = native;
    state.instance = &instance;
    // F463D0 attaches constructor-requested membership before F4E230 copies
    // authored +75. Constructors normally start with no memberships.
    lists.attach(state);
    state.state_index = authored.state_index;
    state.unique_id_low = authored.unique_id_low;
    state.scoped = authored.scoped;
    slot = &state;
    if (instance.state_activity) instance.state_activity[authored.state_index] = 0;
    if (instance.flags & 8) host.debug_node(state, authored);
    for (unsigned capability = 0; capability != 2; ++capability) {
        if (host.capability(state, capability)) host.register_capability(state, capability);
    }
    host.initialize(state);
    return &state;
}
} // namespace ow::statescript::runtime
