// Ability / input / script-composition node behaviours. See docs/statescript/vm.md.
#include <algorithm>
#include <climits>

#include "statescript/vm/nodes_common.h"

namespace ow::statescript::vm {
namespace {

// ---- StateLogicalButton ------------------------------------------------------------------------------------------
// Client impl STU_640980B3 (registered at 0x573... mode 0), vtbl 0x15C9ED8: +0x110 F58370 (begin), +0x140 F585E0 (timer),
// +0x158 F58550 (end), +0x1C8 F586D0 (button event). Authored: +0x100 logicalButton, +0x108 (0338E8E4) press-buffer window
// in seconds, +0x110 (8CBE9822) bool, +0x120 onGoingDownPlug, +0x128 onComingUpPlug, +0x130 (8496114B) "held" subgraph plug,
// +0x138 (29909C7C) "released" subgraph plug. Plug names 8496114B/29909C7C are unsolved hashes; the roles above are read
// from F58370/F586D0/F585E0 (0x130 is begun on press and aborted on release; 0x138 the opposite).
struct ButtonData : StateData {
    std::int64_t last_press = 0;                      // state +0xA8 (command-frame ms of the last matching press)
    std::uint32_t counter = 0xFFFFFFFFu;              // state +0xB0 (begin_count the press belongs to)
};
struct LogicalButtonB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptStateLogicalButton"; }
    bool listens_buttons() const override { return true; }
    std::unique_ptr<StateData> create_data(NodeCtx&) const override { return std::make_unique<ButtonData>(); }

    static bool resolve(NodeCtx& c, std::int32_t& button) {
        Var b;
        if (!c.eval_opt("m_logicalButton", b)) return false;   // F58370: no button configured -> nothing
        button = static_cast<std::int32_t>(b.as_int());
        return true;
    }
    void on_post_begin(NodeCtx& c) const override {   // F58370
        std::int32_t btn;
        if (!resolve(c, btn)) return;
        auto& d = static_cast<ButtonData&>(*c.state->data);
        bool recent = false;
        if (d.counter == c.state->rt.begin_count) {
            Var w;
            double window = 0;
            if (c.eval_opt("m_0338E8E4", w)) window = w.as_double();
            recent = static_cast<double>(c.g.frame() - d.last_press) * 0.001 <= window;   // dword_162C73C == 0.001
        }
        if (recent) c.fire("m_onGoingDownPlug");
        if (c.g.button_held(btn)) { c.fire("m_8496114B"); return; }
        if (recent) {
            c.fire("m_8496114B");
            c.g.schedule_timer(*c.state, 1, 1);   // synthesized release one ms later (F584C4..F584F5)
        } else {
            c.fire("m_29909C7C");
        }
    }
    void on_timer(NodeCtx& c, std::uint32_t user) const override {   // F585E0
        if (user != 1) return;
        std::int32_t btn;
        if (!resolve(c, btn) || c.g.button_held(btn)) return;
        c.fire("m_onComingUpPlug");
        c.fire("m_29909C7C");
        c.abort_group("m_8496114B");
    }
    void on_end(NodeCtx& c) const override {   // F58550
        std::int32_t btn;
        if (!resolve(c, btn)) return;
        c.abort_group("m_8496114B");
        c.abort_group("m_29909C7C");
    }
    int on_logical_button(NodeCtx& c, std::int32_t button, bool going_down) const override {   // F586D0
        std::int32_t btn;
        if (!resolve(c, btn) || btn != button) return 0;
        // F5871E also tests m_8CBE9822 (+0x110) and instance vtbl +0x220; with both unmodelled the gate is open.
        auto& d = static_cast<ButtonData&>(*c.state->data);
        const bool active = runtime::is_active(c.state->rt);
        if (going_down) {
            d.last_press = c.g.frame();
            d.counter = c.state->rt.begin_count + (active ? 0 : 1);
        }
        if (active) {
            if (going_down) { c.fire("m_onGoingDownPlug"); c.fire("m_8496114B"); c.abort_group("m_29909C7C"); }
            else { c.fire("m_onComingUpPlug"); c.fire("m_29909C7C"); c.abort_group("m_8496114B"); }
        }
        return 1;
    }
};

// ---- STU_4768E4A3: button-edge condition -------------------------------------------------------------------------
// Authored: inPlug/truePlug/falsePlug, +0x90 list of states re-evaluated on a match, +0xA0 button, +0xA8/+0xA9 edges,
// +0xAA/+0xAB unconditional. The graph-level routing (F4F600 case 7, vm.cpp) calls F66640; as a plug-activated condition
// its value is F66640 against the notification currently being routed.
struct ButtonConditionB final : Behavior {
    std::string_view cls() const override { return "STU_4768E4A3"; }
    bool execute(NodeCtx& c) const override {
        Var b;
        const bool have = c.eval_opt("m_logicalButton", b);
        const int m = c.g.current_button() < 0 ? 0 :
            runtime::logical_button_match(c.g.current_button(), c.g.current_going_down(), have ? static_cast<int>(b.as_int()) : -1,
                                          c.node.get_bool("m_30C0A41D"), c.node.get_bool("m_86CD8AF0"),
                                          c.node.get_bool("m_A3E7D1E9"), c.node.get_bool("m_B4015D42"));
        c.g.note(TraceEvent::Kind::Note, &c.node, std::string("button condition=") + (m ? "true" : "false"));
        if (m == 0) return c.fire("m_falsePlug");
        const bool r = c.fire("m_truePlug");
        if (r) return true;
        const OutPlug* t = c.node.out("m_truePlug");
        const OutPlug* f = c.node.out("m_falsePlug");
        return !((t && !t->links.empty()) || (f && !f->links.empty()));
    }
};

// ---- StateSubScript / ActionPlayScript ---------------------------------------------------------------------------
// STUConfigVarStatescriptGraph.m_graph (res 0x580...) names the child graph (graph-format.md). Client impl of SubScript is
// STU_1BEDA8B9 (mode 1, not read). Reconstructed [INFERENCE]: begin starts a child instance sharing host+registry whose
// variables are preset from the init-data list (out_Var <- value, evaluated in the parent); the state finishes when the
// child becomes idle; end stops the child.
struct SubScriptData : StateData { GraphInstance* child = nullptr; };
struct SubScriptB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptStateSubScript"; }
    bool wants_tick() const override { return true; }
    std::unique_ptr<StateData> create_data(NodeCtx&) const override { return std::make_unique<SubScriptData>(); }
    void on_post_begin(NodeCtx& c) const override {
        auto& d = static_cast<SubScriptData&>(*c.state->data);
        Var g;
        if (!c.eval("m_statescriptGraph", g) || g.kind != Var::Kind::Id) return;
        const Graph* graph = c.g.library().find(g.id);
        if (!graph) { c.g.error(&c.node, "SubScript: graph not in library"); return; }
        std::vector<std::pair<VarPath, Var>> init;
        if (const data::Value* lst = c.node.field("m_7EB65D0D"))
            for (const data::Value& e : lst->list) {
                const data::Object* o = e.kind == data::Value::Kind::Inline ? e.inl.get() : e.ref;
                if (!o) continue;
                const data::Value* ov = o->get("m_out_Var");
                const data::Value* val = o->get("m_value");
                VarPath p; std::string err; Var v;
                if (ov && c.g.evaluator().resolve_path(*ov, p, err) && val && c.g.evaluator().eval(*val, v, err)) init.emplace_back(p, v);
            }
        d.child = c.g.start_child(*graph, [&](GraphInstance& child) { for (auto& kv : init) child.bag().set(kv.first, kv.second, child.frame()); });
    }
    void on_tick(NodeCtx& c, std::int64_t) const override {
        auto& d = static_cast<SubScriptData&>(*c.state->data);
        if (d.child && d.child->idle()) { d.child = nullptr; c.g.request_finish(*c.state, 0); }
    }
    void on_end(NodeCtx& c) const override {
        auto& d = static_cast<SubScriptData&>(*c.state->data);
        if (d.child) c.g.stop_child(d.child);
        d.child = nullptr;
    }
    void on_finished(NodeCtx& c) const override { c.fire("m_onFinishedPlug"); }
    void on_aborted(NodeCtx& c) const override { c.fire("m_onAbortPlug"); }
};

struct PlayScriptB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptActionPlayScript"; }
    bool execute(NodeCtx& c) const override {
        Var g;
        if (!c.eval("m_script", g) || g.kind != Var::Kind::Id) return false;
        const Graph* graph = c.g.library().find(g.id);
        if (!graph) { c.g.error(&c.node, "PlayScript: graph not in library"); return false; }
        std::vector<std::pair<std::uint64_t, Var>> params;
        for_each_pair(c.node, "m_params", "m_value", [&](std::uint64_t def, const data::Value* val) {
            Var v; std::string err;
            if (val && c.g.evaluator().eval(*val, v, err)) params.emplace_back(def, v);
            else if (val) c.g.error(&c.node, "PlayScript param: " + err);
        });
        c.g.start_child(*graph, [&](GraphInstance& child) { for (auto& p : params) child.set_message_param(p.first, p.second); });
        c.fire("m_outPlug");
        return true;
    }
};

// ---- ActionSendGameMessage ---------------------------------------------------------------------------------------
// Client impl STU_25F7A66F (mode 2, vtbl not read). Host call carries gameMessage, instigatorOverride, targets, params.
struct SendGameMessageB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptActionSendGameMessage"; }
    Support support() const override { return Support::HostForwarded; }
    bool execute(NodeCtx& c) const override {
        HostCall call;
        call.detail = "send_game_message";
        collect_fields(c, call);
        if (const Var* gm = call.field("gameMessage")) call.resource = gm->id;
        c.host_call(HostCall::Phase::Once, &Host::send_game_message, std::move(call));
        c.fire("m_outPlug");
        return true;
    }
};

// ---- StateAbility ------------------------------------------------------------------------------------------------
// Client impls STU_FD10476F (mode 2) / STU_DC919EF6 (mode 1) not read. The gameplay meaning of the vars-to-set lists,
// start/stop button, start condition and the two unsolved plugs is [INFERENCE]-only; this behaviour forwards the evaluated
// fields to Host::ability_event on begin/end and runs the authored abilitySubgraph plug (begin on start, abort on end).
struct AbilityB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptStateAbility"; }
    Support support() const override { return Support::HostForwarded; }
    void on_post_begin(NodeCtx& c) const override {
        HostCall call; call.detail = "ability_begin";
        collect_fields(c, call);
        c.host_call(HostCall::Phase::Begin, &Host::ability_event, std::move(call));
        c.fire("m_abilitySubgraph");
    }
    void on_end(NodeCtx& c) const override {
        c.abort_group("m_abilitySubgraph");
        HostCall call; call.detail = "ability_end";
        c.host_call(HostCall::Phase::End, &Host::ability_event, std::move(call));
    }
};

// ---- StateWeaponVolley -------------------------------------------------------------------------------------------
// Client class STU_713318AD (vtbl 0x15D2450, see docs/architecture/weapon-statescript.md): begin gate 1077B30 requires entity
// component bit 38 and instance flag 8, else Finish at once. On a server instance the gate is assumed open [INFERENCE].
// Shot k fires at k / numShotsPerSecond seconds after begin (constant-rate branch of 1078220: index/rate*1000 rounded);
// the rate-ramp definition (1079570) and spread (1078340) are not modelled here (gameplay/weapon_volley.* has the math).
struct VolleyData : StateData { std::int64_t begin = 0; std::int32_t shot = 0; double rate = 1; std::int64_t limit = INT_MAX; };
struct WeaponVolleyB final : Behavior {
    std::string_view cls() const override { return "STUStatescriptStateWeaponVolley"; }
    Support support() const override { return Support::HostForwarded; }
    std::unique_ptr<StateData> create_data(NodeCtx&) const override { return std::make_unique<VolleyData>(); }
    static void emit(NodeCtx& c, VolleyData& d, bool first) {
        HostCall call; call.detail = "shot " + std::to_string(d.shot);
        if (first) collect_fields(c, call);
        call.fields.emplace_back("shotIndex", Var::integer(d.shot));
        c.host_call(first ? HostCall::Phase::Begin : HostCall::Phase::Tick, &Host::spawn_projectile, std::move(call));
        ++d.shot;
    }
    static std::int64_t shot_time(const VolleyData& d, std::int32_t i) { return seconds_to_ms(static_cast<double>(i) / d.rate); }
    void on_post_begin(NodeCtx& c) const override {
        auto& d = static_cast<VolleyData&>(*c.state->data);
        Var rate, lim1, lim2;
        d.rate = c.eval_opt("m_numShotsPerSecond", rate) && rate.as_double() > 0.00001 ? rate.as_double() : 1.0;   // 0.00001 clamp (1078EA0)
        if (c.eval_opt("m_64389377", lim1) && lim1.as_int() > 0) d.limit = std::min<std::int64_t>(d.limit, lim1.as_int());
        if (c.eval_opt("m_A835E878", lim2) && lim2.as_int() > 0) d.limit = std::min<std::int64_t>(d.limit, lim2.as_int());
        d.begin = c.g.frame();
        d.shot = 0;
        emit(c, d, true);
        if (d.shot < d.limit) c.g.schedule_timer(*c.state, 1, shot_time(d, d.shot));
        else c.g.request_finish(*c.state, 0);
    }
    void on_timer(NodeCtx& c, std::uint32_t user) const override {
        if (user != 1) return;
        auto& d = static_cast<VolleyData&>(*c.state->data);
        emit(c, d, false);
        if (d.shot < d.limit) c.g.schedule_timer(*c.state, 1, d.begin + shot_time(d, d.shot) - c.g.frame());
        else c.g.request_finish(*c.state, 0);
    }
    void on_end(NodeCtx& c) const override {
        HostCall call; call.detail = "volley_end";
        c.host_call(HostCall::Phase::End, &Host::spawn_projectile, std::move(call));
    }
    void on_finished(NodeCtx& c) const override { c.fire("m_onFinishedPlug"); }
    void on_aborted(NodeCtx& c) const override { c.fire("m_onAbortPlug"); }
};

}  // namespace

void register_ability_nodes(BehaviorRegistry& r) {
    r.add(std::make_unique<LogicalButtonB>());
    r.add(std::make_unique<ButtonConditionB>());
    r.add(std::make_unique<SubScriptB>());
    r.add(std::make_unique<PlayScriptB>());
    r.add(std::make_unique<SendGameMessageB>());
    r.add(std::make_unique<AbilityB>());
    r.add(std::make_unique<WeaponVolleyB>());
}

}  // namespace ow::statescript::vm
