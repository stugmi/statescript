// statescript/vm/nodes_common.h -- helpers shared by nodes_*.cpp (not part of the public VM API).
#pragma once
#include <cmath>
#include <string>

#include "statescript/vm/vm.h"

namespace ow::statescript::vm {

// F52F77..F52F9F (Wait) / F5D7A0: seconds -> milliseconds: value * 1000 (dword_162D2E8), then +/-0.5 (dword_162CAEC), truncate.
inline std::int64_t seconds_to_ms(double seconds) {
    const double ms = seconds * 1000.0;
    return static_cast<std::int64_t>(ms >= 0.0 ? ms + 0.5 : ms - 0.5);
}

// Evaluate every ConfigVar-like member of the authored node into a HostCall (generic pass-through for host-forwarded
// classes). Plug members and the STUGraphItem/STUStatescriptBase/STUStatescriptState header members are skipped.
// Nested non-config objects are flattened as "outer.inner"; resource references keep their id as Var::Id.
void collect_fields(NodeCtx& c, HostCall& call);

// First resource-like member of the call (effect / gameMessage / aura / animAlias / ... ) as the call's primary resource.
void pick_primary_resource(HostCall& call);

// Read an STU object member that is a list of {m_def, m_value|m_out_Var} pairs (STU_A51F99C5 / STU_C679B01F /
// STUStatescriptSubScriptInitData). Calls fn(def_id, Value* value_or_outvar).
using PairFn = std::function<void(std::uint64_t def, const data::Value* cfg)>;
void for_each_pair(const GraphNode& node, const char* list_member, const char* cfg_member, const PairFn& fn);

}  // namespace ow::statescript::vm
