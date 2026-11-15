// Variable / data node behaviours (SetVar, array ops, Criteria, ChaseVar, TrackTargets). See docs/statescript/vm.md.
#include <algorithm>

#include "statescript/vm/nodes_common.h"

namespace ow::statescript::vm {
namespace {

// ---- ActionSetVar ------------------------------------------------------------------------------------------------
// The client has NO runtime implementation for this class (not in the registration table, see node-impl-registry.json):
// on the client it runs the generic action F47740 (fire outPlug +0x80). Values reach the client through SetVariable
// notifications (execute_notification type 4/5 -> varbag +0xE0/+0x130). The assignment semantics below are therefore the
// SERVER-side meaning reconstructed from the authored fields (out_Var, value, index, key) [INFERENCE].
struct SetVarB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptActionSetVar"; }
    bool execute(NodeCtx& c) const override {
        Var v;
        c.eval_opt("m_value", v);
        if (const data::Value* k = c.node.field("m_key"); k && !k->is_null()) {
            c.g.error(&c.node, "ActionSetVar: dictionary key assignment is not implemented");
            return false;
        }
        Var idx;
        if (c.eval_opt("m_index", idx)) {
            VarPath path; std::string err;
            const data::Value* ov = c.node.field("m_out_Var");
            if (!ov || !c.g.evaluator().resolve_path(*ov, path, err)) { c.g.error(&c.node, "SetVar: out_Var: " + err); return false; }
            Var arr = c.g.bag().get(path);
            if (arr.kind != Var::Kind::Array) arr = Var::array({});
            const std::int64_t i = idx.as_int();
            if (i < 0 || i > 4096) { c.g.error(&c.node, "SetVar: array index out of range"); return false; }
            if (static_cast<std::size_t>(i) >= arr.arr.size()) arr.arr.resize(static_cast<std::size_t>(i) + 1);
            arr.arr[static_cast<std::size_t>(i)] = v;
            c.assign("m_out_Var", arr);
        } else if (!c.assign("m_out_Var", v)) {
            return false;
        }
        c.fire("m_outPlug");
        return true;
    }
};

// STU_0CDC2E1A: out_Array / index / key / value / A7E46EA2. [INFERENCE] array append (or set at index); A7E46EA2 = "unique".
struct ArrayOpB final : Behavior {
    std::string_view cls() const override { return "STU_0CDC2E1A"; }
    bool execute(NodeCtx& c) const override {
        Var v, idx;
        c.eval_opt("m_value", v);
        VarPath path; std::string err;
        const data::Value* ov = c.node.field("m_out_Array");
        if (!ov || !c.g.evaluator().resolve_path(*ov, path, err)) { c.g.error(&c.node, "array op: out_Array: " + err); return false; }
        Var arr = c.g.bag().get(path);
        if (arr.kind != Var::Kind::Array) arr = Var::array({});
        if (c.eval_opt("m_index", idx)) {
            const std::int64_t i = idx.as_int();
            if (i < 0 || i > 4096) { c.g.error(&c.node, "array op: index out of range"); return false; }
            if (static_cast<std::size_t>(i) >= arr.arr.size()) arr.arr.resize(static_cast<std::size_t>(i) + 1);
            arr.arr[static_cast<std::size_t>(i)] = v;
        } else {
            bool dup = false;
            if (c.node.get_bool("m_A7E46EA2"))
                for (const Var& e : arr.arr) if (e.equals(v)) dup = true;
            if (!dup) arr.arr.push_back(v);
        }
        c.assign("m_out_Array", arr);
        c.fire("m_outPlug");
        return true;
    }
};

// ---- ActionCriteria ----------------------------------------------------------------------------------------------
// No client implementation (generic action). Server meaning [INFERENCE]: report criteria `identifier` for `targets`.
struct CriteriaB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptActionCriteria"; }
    Support support() const override { return Support::HostForwarded; }
    bool execute(NodeCtx& c) const override {
        HostCall call;
        call.detail = "criteria";
        collect_fields(c, call);
        pick_primary_resource(call);
        c.host_call(HostCall::Phase::Once, &Host::criteria_event, std::move(call));
        c.fire("m_outPlug");
        return true;
    }
};

// ---- ChaseVar ----------------------------------------------------------------------------------------------------
// Client impls STU_D3285EF7/8925715E/1BF410A8 exist (vtables not read). Reconstructed from the authored fields
// [INFERENCE]: out_Var moves linearly from its current value to `destination` over `fixedDuration` seconds (or by
// speed m_41430518 when no duration); at the end the plug onFinishedPlug fires and the state finishes.
struct ChaseData : StateData { VarPath path; double start = 0, dest = 0; std::int64_t begin = 0, dur_ms = 0; bool int_dest = false; bool ok = false; };
struct ChaseVarB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptStateChaseVar"; }
    bool wants_tick() const override { return true; }
    std::unique_ptr<StateData> create_data(NodeCtx&) const override { return std::make_unique<ChaseData>(); }
    void on_post_begin(NodeCtx& c) const override {
        auto& d = static_cast<ChaseData&>(*c.state->data);
        std::string err;
        const data::Value* ov = c.node.field("m_out_Var");
        if (!ov || !c.g.evaluator().resolve_path(*ov, d.path, err)) { c.g.error(&c.node, "ChaseVar: out_Var: " + err); return; }
        Var dest, dur, speed;
        if (!c.eval("m_destination", dest)) return;
        if (dest.kind == Var::Kind::Vec3) { c.g.error(&c.node, "ChaseVar: vec3 destination not implemented"); return; }
        d.dest = dest.as_double();
        d.int_dest = dest.kind == Var::Kind::Int;
        Var cur = c.g.bag().get(d.path);
        d.start = cur.is_set() ? cur.as_double() : 0.0;
        double secs = 0;
        if (c.eval_opt("m_fixedDuration", dur)) secs = dur.as_double();
        else if (c.eval_opt("m_41430518", speed) && speed.as_double() > 0) secs = std::fabs(d.dest - d.start) / speed.as_double();
        d.dur_ms = seconds_to_ms(secs);
        d.begin = c.g.frame();
        d.ok = true;
        if (d.dur_ms <= 0) { finish_now(c, d); return; }
        c.g.schedule_timer(*c.state, 1, d.dur_ms);
    }
    static void write(NodeCtx& c, const ChaseData& d, double v, bool final_value) {
        Var out = final_value && d.int_dest ? Var::integer(static_cast<std::int64_t>(std::llround(v))) : Var::real(v);
        c.g.bag().set(d.path, out, c.g.frame());
    }
    static void finish_now(NodeCtx& c, ChaseData& d) { write(c, d, d.dest, true); c.g.request_finish(*c.state, 0); }
    void on_tick(NodeCtx& c, std::int64_t frame) const override {
        auto& d = static_cast<ChaseData&>(*c.state->data);
        if (!d.ok || d.dur_ms <= 0) return;
        const double t = std::min(1.0, static_cast<double>(frame - d.begin) / static_cast<double>(d.dur_ms));
        write(c, d, d.start + (d.dest - d.start) * t, false);
    }
    void on_timer(NodeCtx& c, std::uint32_t user) const override {
        auto& d = static_cast<ChaseData&>(*c.state->data);
        if (user == 1 && d.ok) finish_now(c, d);
    }
    void on_finished(NodeCtx& c) const override { c.fire("m_onFinishedPlug"); }
    void on_aborted(NodeCtx& c) const override { c.fire("m_onAbortPlug"); }
};

// ---- TrackTargets ------------------------------------------------------------------------------------------------
// Server-only gameplay query (no client side effect). [INFERENCE] while active it keeps `out_Targets` equal to the host's
// answer for (posEntity, posOffset, radius, filter) on every tick; End clears the variable.
struct TrackTargetsB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptStateTrackTargets"; }
    Support support() const override { return Support::HostForwarded; }
    bool wants_tick() const override { return true; }
    static void update(NodeCtx& c, bool log) {
        HostCall call;
        call.detail = "track_targets";
        collect_fields(c, call);
        std::vector<std::uint64_t> targets;
        call.cls = c.node.cls; call.graph = c.g.graph().resource_id; call.node = c.node.index;
        if (log) c.g.note(TraceEvent::Kind::Host, &c.node, "track_targets");
        c.g.host().track_targets(call, targets);
        std::vector<Var> arr;
        for (std::uint64_t t : targets) arr.push_back(Var::entity(t));
        c.assign("m_out_Targets", Var::array(std::move(arr)));
    }
    void on_post_begin(NodeCtx& c) const override { update(c, true); }
    void on_tick(NodeCtx& c, std::int64_t) const override { update(c, false); }
    void on_end(NodeCtx& c) const override { c.assign("m_out_Targets", Var::array({})); }
};

}  // namespace

void register_data_nodes(BehaviorRegistry& r) {
    r.add(std::make_unique<SetVarB>());
    r.add(std::make_unique<ArrayOpB>());
    r.add(std::make_unique<CriteriaB>());
    r.add(std::make_unique<ChaseVarB>());
    r.add(std::make_unique<TrackTargetsB>());
}

}  // namespace ow::statescript::vm
