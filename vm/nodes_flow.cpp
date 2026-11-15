// Flow-control node behaviours. Evidence per class is documented in docs/statescript/vm.md ("Node classes").
#include <algorithm>

#include "statescript/vm/nodes_common.h"

namespace ow::statescript::vm {
namespace {

// ---- Entry ------------------------------------------------------------------------------------------------------
// Statescript__OnScriptEntry F47800 (Entry vtbl+0x1D0) and generic F477D0 (+0xF8): fire the plug at authored +0x78.
struct EntryB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptEntry"; }
    bool execute(NodeCtx& c) const override { c.fire("m_outPlug"); return true; }   // F47800 returns 1
};

// EntryGameMessage: client executes F477D0 (fires outPlug). Message routing + parameter binding is the manager's job
// in the client (not read); here it is on_game_message. [INFERENCE: filter / EB7F6AB1 / Sender / Instigator not applied]
struct EntryGameMessageB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptEntryGameMessage"; }
    bool on_game_message(NodeCtx& c, std::uint64_t message, const std::vector<std::pair<std::uint64_t, Var>>& params) const override {
        Var gm;
        if (!c.eval("m_gameMessage", gm) || gm.kind != Var::Kind::Id || gm.id != message) return false;
        for_each_pair(c.node, "m_params", "m_out_Var", [&](std::uint64_t def, const data::Value* out_var) {
            for (const auto& p : params) {
                if (p.first != def) continue;
                c.g.set_message_param(def, p.second);
                std::string err;
                if (out_var && !c.g.evaluator().assign(*out_var, p.second, err)) c.g.error(&c.node, "bind param: " + err);
            }
        });
        return true;
    }
    bool execute(NodeCtx& c) const override { return c.fire("m_outPlug"); }
};

// EntryPlayScript: started by ActionPlayScript / child graph start; params were preset with set_message_param.
struct EntryPlayScriptB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptEntryPlayScript"; }
    bool execute(NodeCtx& c) const override {
        for_each_pair(c.node, "m_params", "m_out_Var", [&](std::uint64_t def, const data::Value* out_var) {
            Var v;
            std::string err;
            if (out_var && c.g.message_param(def, v) && !c.g.evaluator().assign(*out_var, v, err)) c.g.error(&c.node, "bind param: " + err);
        });
        Var owner = Var::entity(c.g.host().owner_entity());
        if (const data::Value* s = c.node.field("m_out_Sender")) {
            std::string err;
            if (!s->is_null()) c.g.evaluator().assign(*s, owner, err);   // [INFERENCE] sender == owner entity
        }
        return c.fire("m_outPlug");
    }
};

// ---- State (generic base) ----------------------------------------------------------------------------------------
// F4E230 allocates the generic base state (F47920) for types without a factory: lifecycle only (runtime/lifecycle).
struct GenericStateB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptState"; }
};

// ---- Wait --------------------------------------------------------------------------------------------------------
// vtbl 0x16052B0: +0x110 STU_8424754B__vf34 (F52F00), +0x118 vf35 (F53030), +0x120 vf36 (F53120), +0x140 F531E0,
// +0x248 D00510 (fires +0x108 onAbortPlug), +0x250 D00570 (fires +0x110 onFinishedPlug).
struct WaitB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptStateWait"; }
    void begin(NodeCtx& c) const {
        Var t;
        double secs = 0;
        if (c.eval_opt("m_timeout", t)) secs = t.as_double();
        const std::int64_t ms = seconds_to_ms(secs);
        // F52F9F..F52FB1: a non-positive timeout finishes at once (F4A9C0 kind 1, delay 0). Otherwise timer user 1 at
        // now + timeout (F47A50). m_C5A427FA (+0x119) selects the per-frame check of vf36 in the client; the elapsed
        // condition (now - begin >= timeout) is identical, modelled here by the same exact timer [documented deviation].
        if (ms <= 0) c.g.request_finish(*c.state, 0);
        else c.g.schedule_timer(*c.state, 1, ms);
    }
    void on_post_begin(NodeCtx& c) const override { begin(c); }
    void on_reenter(NodeCtx& c) const override {
        // vf35: when m_1BFF3867 (+0x118) is set the pending timer is removed and the begin logic re-run.
        if (!c.node.get_bool("m_1BFF3867")) return;
        c.g.cancel_timers(*c.state);
        begin(c);
    }
    void on_timer(NodeCtx& c, std::uint32_t user) const override {
        if (user == 1) c.g.request_finish(*c.state, 0);   // F531E0
    }
    void on_finished(NodeCtx& c) const override { c.fire("m_onFinishedPlug"); }
    void on_aborted(NodeCtx& c) const override { c.fire("m_onAbortPlug"); }
};

// ---- BooleanSwitch -----------------------------------------------------------------------------------------------
// vtbl 0x1605058: +0x110 F55170 (evaluate, then F55640 fires the branch), +0x140 F55270 (timer 1: re-evaluate; on change
// abort the OLD branch subgraph group via F4A8B0, store, fire new branch), +0x170 F55420 (attribute change: schedule an
// immediate timer 1), +0x150 F553C0 (abort: abort current branch group). F55640: true -> onTruePlug(+0x108) then
// trueSubgraphPlug(+0x118); false -> onFalsePlug(+0x110) then falseSubgraphPlug(+0x120).
struct SwitchData : StateData { bool value = false; };
struct BooleanSwitchB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptStateBooleanSwitch"; }
    std::unique_ptr<StateData> create_data(NodeCtx&) const override { return std::make_unique<SwitchData>(); }
    static bool cond(NodeCtx& c) { Var v; return c.eval_opt("m_condition", v) && v.truthy(); }
    static void fire_branch(NodeCtx& c, bool v) {
        if (v) { c.fire("m_onTruePlug"); c.fire("m_trueSubgraphPlug"); }
        else { c.fire("m_onFalsePlug"); c.fire("m_falseSubgraphPlug"); }
    }
    void on_post_begin(NodeCtx& c) const override {
        auto& d = static_cast<SwitchData&>(*c.state->data);
        d.value = cond(c);
        fire_branch(c, d.value);
    }
    void on_attribute_changed(NodeCtx& c) const override { c.g.schedule_timer(*c.state, 1, 0); }
    void on_timer(NodeCtx& c, std::uint32_t user) const override {
        if (user != 1) return;
        auto& d = static_cast<SwitchData&>(*c.state->data);
        const bool v = cond(c);
        if (v == d.value) return;
        c.abort_group(d.value ? "m_trueSubgraphPlug" : "m_falseSubgraphPlug");
        d.value = v;
        fire_branch(c, v);
    }
    void on_end(NodeCtx& c) const override {
        auto& d = static_cast<SwitchData&>(*c.state->data);
        c.abort_group(d.value ? "m_trueSubgraphPlug" : "m_falseSubgraphPlug");
    }
};

// ---- Condition (STU_B1032DAF) ------------------------------------------------------------------------------------
// vtbl 0x1605xxx +0xF8 F47350: evaluated condition false -> fire falsePlug(+0x88) and return its result; true -> fire
// truePlug(+0x80), return the result if it activated something; else 0 when either plug has links, 1 when both are empty.
// The condition itself is a ConfigVar (+0x90) evaluated by the shared evaluator [INFERENCE: the +0xF0 slot of this class is
// the client stub 0x6119C0, i.e. the client does not evaluate it; the server side is reconstructed from the data].
struct ConditionB final : Behavior {
    std::string_view cls() const override { return "STU_B1032DAF"; }
    bool execute(NodeCtx& c) const override {
        Var v;
        const bool truth = c.eval_opt("m_condition", v) && v.truthy();
        std::string detail = std::string("condition=") + (truth ? "true" : "false");
        c.g.note(TraceEvent::Kind::Note, &c.node, detail);
        if (!truth) return c.fire("m_falsePlug");
        const bool r = c.fire("m_truePlug");
        if (r) return true;
        const OutPlug* t = c.node.out("m_truePlug");
        const OutPlug* f = c.node.out("m_falsePlug");
        if ((t && !t->links.empty()) || (f && !f->links.empty())) return false;
        return true;
    }
};

// ---- Stack -------------------------------------------------------------------------------------------------------
// Client: +0x160 F55770 (begin: loads priority resource, registers), +0x170 F559D0, +0x120 F55890, serialization F55B10/F55BF0.
// The resolution rule below (highest priority wins, later wins ties; out_Var := top value; top/under group plugs follow
// the role) is [INFERENCE] from the authored fields and the plug naming; the client's priority table (F59F50/F59FD0)
// was not read. priority rank = low 32 bits of the STU_98B439CA resource id [INFERENCE].
struct StackData : StateData { VarPath path; bool in_stack = false; bool top = false; };
struct StackB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptStateStack"; }
    std::unique_ptr<StateData> create_data(NodeCtx&) const override { return std::make_unique<StackData>(); }

    static void apply_roles(GraphInstance& g, const VarPath& path, std::uint32_t exclude_state) {
        auto& stacks = g.stacks();
        auto it = stacks.find(path);
        if (it == stacks.end()) return;
        auto& entries = it->second;
        std::size_t top = entries.size();
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (top == entries.size() || entries[i].priority > entries[top].priority ||
                (entries[i].priority == entries[top].priority && entries[i].seq > entries[top].seq)) top = i;
        if (top == entries.size()) return;
        Var tv = entries[top].value.is_set() ? entries[top].value : Var::boolean(true);
        g.bag().set(path, tv, g.frame());
        g.note(TraceEvent::Kind::Var, nullptr, path_string(path) + " = " + tv.to_string() + " (stack top)");
        for (std::size_t i = 0; i < entries.size(); ++i) {
            VmState* vs = g.mutable_state(entries[i].state_index);
            if (!vs || vs->rt.state_index == exclude_state || !vs->data) continue;
            auto& d = static_cast<StackData&>(*vs->data);
            const bool now_top = i == top;
            if (now_top == d.top) continue;
            d.top = now_top;
            NodeCtx c = g.make_ctx(*vs);
            if (now_top) { c.abort_group("m_underSubgraphPlug"); c.fire("m_topSubgraphPlug"); }
            else { c.abort_group("m_topSubgraphPlug"); c.fire("m_underSubgraphPlug"); }
        }
    }
    void on_post_begin(NodeCtx& c) const override {
        auto& d = static_cast<StackData&>(*c.state->data);
        const data::Value* ov = c.node.field("m_out_Var");
        std::string err;
        if (!ov || !c.g.evaluator().resolve_path(*ov, d.path, err)) { c.g.error(&c.node, "Stack: out_Var: " + err); return; }
        Var pr; std::uint64_t prio = 0;
        if (c.eval_opt("m_priority", pr) && pr.kind == Var::Kind::Id) prio = pr.id & 0xFFFFFFFFull;
        Var val;
        c.eval_opt("m_value", val);
        auto& entries = c.g.stacks()[d.path];
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const GraphInstance::StackEntry& e) { return e.state_index == c.state->rt.state_index; }), entries.end());
        entries.push_back({c.state->rt.state_index, prio, c.g.next_seq(), val});
        d.in_stack = true;
        d.top = false;
        // an entry that becomes top fires topSubgraph, otherwise underSubgraph, after apply_roles has settled the others
        apply_roles(c.g, d.path, c.state->rt.state_index);
        auto& list = c.g.stacks()[d.path];
        std::size_t top = 0;
        for (std::size_t i = 1; i < list.size(); ++i)
            if (list[i].priority > list[top].priority || (list[i].priority == list[top].priority && list[i].seq > list[top].seq)) top = i;
        const bool me_top = list[top].state_index == c.state->rt.state_index;
        d.top = me_top;
        if (me_top) c.fire("m_topSubgraphPlug"); else c.fire("m_underSubgraphPlug");
    }
    void on_end(NodeCtx& c) const override {
        auto& d = static_cast<StackData&>(*c.state->data);
        if (!d.in_stack) return;
        d.in_stack = false;
        c.abort_group("m_topSubgraphPlug");
        c.abort_group("m_underSubgraphPlug");
        auto it = c.g.stacks().find(d.path);
        if (it == c.g.stacks().end()) return;
        auto& entries = it->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const GraphInstance::StackEntry& e) { return e.state_index == c.state->rt.state_index; }), entries.end());
        if (entries.empty()) {
            c.g.bag().erase(d.path, c.g.frame());
            c.g.note(TraceEvent::Kind::Var, &c.node, path_string(d.path) + " erased (stack empty)");
            c.g.stacks().erase(it);
        } else {
            apply_roles(c.g, d.path, c.state->rt.state_index);
        }
    }
};

// ---- ActionSwitch / StateSwitch ---------------------------------------------------------------------------------
// No client read (ActionSwitch has no client implementation; StateSwitch's client class STU_0BB3749C is mode 0, not read).
// [INFERENCE] from the authored layout: a list of case plugs each carrying an STUConfigVar `m_value`, a switchValue
// ConfigVar and a default plug. The first case whose value equals the switch value is selected, else the default plug.
int select_case(NodeCtx& c, std::uint32_t list_hash, const char* switch_member, const OutPlug*& chosen) {
    Var sw;
    if (!c.eval(switch_member, sw)) return -2;
    chosen = nullptr;
    int idx = -1;
    for (const OutPlug& p : c.node.out_plugs) {
        if (p.field != list_hash) continue;
        const data::Value* val = p.plug ? p.plug->get("m_value") : nullptr;
        Var cv; std::string err;
        if (!val || !c.g.evaluator().eval(*val, cv, err)) continue;
        if (cv.equals(sw)) { chosen = &p; idx = static_cast<int>(p.element); break; }
    }
    return idx;
}
struct ActionSwitchB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptActionSwitch"; }
    bool execute(NodeCtx& c) const override {
        const OutPlug* chosen = nullptr;
        if (select_case(c, field_hash("m_7D662124"), "m_switchValue", chosen) == -2) return false;
        bool r = false;
        if (chosen) r = c.g.fire_plug(c.node, *chosen, nullptr);
        else r = c.fire("m_dEFAULTPlug");
        c.fire("m_outPlug");
        return r || true;
    }
};
struct SwitchSel : StateData { int sel = -2; };
struct StateSwitchB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptStateSwitch"; }
    std::unique_ptr<StateData> create_data(NodeCtx&) const override { return std::make_unique<SwitchSel>(); }
    static void apply(NodeCtx& c, bool initial) {
        auto& d = static_cast<SwitchSel&>(*c.state->data);
        const OutPlug* chosen = nullptr;
        const int idx = select_case(c, field_hash("m_3FD0156C"), "m_switchValue", chosen);
        if (idx == -2) return;
        if (!initial && idx == d.sel) return;
        if (!initial && d.sel != -2) {
            if (d.sel >= 0) c.abort_group("m_3FD0156C", static_cast<std::uint32_t>(d.sel));
            else c.abort_group("m_F3C7685F");
        }
        d.sel = idx;
        if (chosen) c.g.fire_plug(c.node, *chosen, &c.state->rt);
        else c.fire("m_F3C7685F");
    }
    void on_post_begin(NodeCtx& c) const override { apply(c, true); }
    void on_attribute_changed(NodeCtx& c) const override { apply(c, false); }
    void on_end(NodeCtx& c) const override {
        auto& d = static_cast<SwitchSel&>(*c.state->data);
        if (d.sel >= 0) c.abort_group("m_3FD0156C", static_cast<std::uint32_t>(d.sel));
        else if (d.sel == -1) c.abort_group("m_F3C7685F");
    }
};

// ---- STU_9F74C07C: state that only owns one subgraph plug (m_0F944CC0) ------------------------------------------
// [INFERENCE] (client class STU_BC4E26C2 mode 2, not read): begin runs the subgraph, end aborts it, exactly like the
// abilitySubgraph / trueSubgraph pattern proven for BooleanSwitch.
struct SubgraphWrapperB final : Behavior {
    std::string_view cls() const override { return "STU_9F74C07C"; }
    void on_post_begin(NodeCtx& c) const override { c.fire("m_0F944CC0"); }
    void on_end(NodeCtx& c) const override { c.abort_group("m_0F944CC0"); }
};

// ---- StateWatch --------------------------------------------------------------------------------------------------
// [INFERENCE] (client class STU_619C0E3F, vtbl +0x110.. not read): while active, when watchedVal changes the new value is
// written to B0F08764 and A415BE52 fires.
struct WatchData : StateData { Var last; bool have = false; };
struct WatchB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptStateWatch"; }
    std::unique_ptr<StateData> create_data(NodeCtx&) const override { return std::make_unique<WatchData>(); }
    static void sample(NodeCtx& c, bool initial) {
        auto& d = static_cast<WatchData&>(*c.state->data);
        Var v;
        if (!c.eval("m_watchedVal", v)) return;
        if (d.have && v.kind == d.last.kind && v.equals(d.last)) return;
        const bool first = !d.have;
        d.last = v; d.have = true;
        if (first && initial) return;   // [INFERENCE] the initial value is not a change
        if (const data::Value* o = c.node.field("m_B0F08764"); o && !o->is_null()) c.assign("m_B0F08764", v);
        c.fire("m_A415BE52");
    }
    void on_post_begin(NodeCtx& c) const override { sample(c, true); }
    void on_attribute_changed(NodeCtx& c) const override { sample(c, false); }
};

// ---- STU_5720B660: array condition -------------------------------------------------------------------------------
// Condition (inPlug/truePlug/falsePlug) over m_array, m_key, m_value. [INFERENCE] true when the array holds `value`
// (at index `key` when a key is configured).
struct ArrayCondB final : Behavior {
    std::string_view cls() const override { return "STU_5720B660"; }
    bool execute(NodeCtx& c) const override {
        Var arr, key, val;
        c.eval_opt("m_array", arr); c.eval_opt("m_value", val);
        bool truth = false;
        if (arr.kind == Var::Kind::Array) {
            if (c.eval_opt("m_key", key)) { const std::int64_t i = key.as_int(); truth = i >= 0 && static_cast<std::size_t>(i) < arr.arr.size() && arr.arr[static_cast<std::size_t>(i)].equals(val); }
            else for (const Var& e : arr.arr) if (e.equals(val)) truth = true;
        }
        c.g.note(TraceEvent::Kind::Note, &c.node, std::string("array condition=") + (truth ? "true" : "false"));
        if (!truth) return c.fire("m_falsePlug");
        const bool r = c.fire("m_truePlug");
        if (r) return true;
        const OutPlug* t = c.node.out("m_truePlug");
        const OutPlug* f = c.node.out("m_falsePlug");
        return !((t && !t->links.empty()) || (f && !f->links.empty()));
    }
};

}  // namespace

void register_flow_nodes(BehaviorRegistry& r) {
    r.add(std::make_unique<EntryB>());
    r.add(std::make_unique<EntryGameMessageB>());
    r.add(std::make_unique<EntryPlayScriptB>());
    r.add(std::make_unique<GenericStateB>());
    r.add(std::make_unique<WaitB>());
    r.add(std::make_unique<BooleanSwitchB>());
    r.add(std::make_unique<ConditionB>());
    r.add(std::make_unique<StackB>());
    r.add(std::make_unique<ActionSwitchB>());
    r.add(std::make_unique<StateSwitchB>());
    r.add(std::make_unique<SubgraphWrapperB>());
    r.add(std::make_unique<WatchB>());
    r.add(std::make_unique<ArrayCondB>());
}

}  // namespace ow::statescript::vm
