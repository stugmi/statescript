// statescript/vm/vm.cpp -- GraphInstance: binds the recovered runtime (runtime/execution|lifecycle|scheduling) to decoded graphs.
#include "statescript/vm/vm.h"

#include <algorithm>
#include <climits>
#include <cstdio>

namespace ow::statescript::vm {
using runtime::Notification;
using runtime::NotificationType;
using runtime::StateRuntime;

// ------------------------------------------------------------------------------------------------------------------
// HostCall / registry
// ------------------------------------------------------------------------------------------------------------------
const Var* HostCall::field(std::string_view name) const {
    for (const auto& f : fields)
        if (f.first == name) return &f.second;
    return nullptr;
}

bool Behavior::execute(NodeCtx& c) const {
    c.g.error(&c.node, "behavior for " + std::string(cls()) + " has no action implementation (not an action node?)");
    return false;
}

void BehaviorRegistry::add(std::unique_ptr<Behavior> b) {
    std::string name(b->cls());
    by_class_[name] = std::move(b);
}
const Behavior* BehaviorRegistry::find(std::string_view cls) const {
    auto it = by_class_.find(cls);
    return it == by_class_.end() ? nullptr : it->second.get();
}
const Behavior* BehaviorRegistry::resolve(const GraphNode& n) const {
    if (const Behavior* b = find(n.cls)) return b;
    auto g = generated_.find(n.cls);
    if (g != generated_.end()) return g->second.get();
    if (rejected_.count(n.cls)) return nullptr;
    if (fallback_) {
        if (auto b = fallback_(n)) {
            const Behavior* raw = b.get();
            generated_[n.cls] = std::move(b);
            return raw;
        }
    }
    rejected_.insert(n.cls);
    return nullptr;
}
std::vector<std::string> BehaviorRegistry::classes() const {
    std::vector<std::string> r;
    for (const auto& kv : by_class_) r.push_back(kv.first);
    return r;
}
const BehaviorRegistry& BehaviorRegistry::standard() {
    static const BehaviorRegistry reg = [] {
        BehaviorRegistry r;
        register_flow_nodes(r);
        register_data_nodes(r);
        register_ability_nodes(r);
        register_host_nodes(r);
        return r;
    }();
    return reg;
}

// ------------------------------------------------------------------------------------------------------------------
// NodeCtx
// ------------------------------------------------------------------------------------------------------------------
bool NodeCtx::eval(const char* member, Var& out) const {
    std::string err;
    if (g.evaluator_.eval_field(node, member, out, err)) return true;
    g.error(&node, std::string("eval ") + member + ": " + err);
    return false;
}
bool NodeCtx::eval_opt(const char* member, Var& out) const {
    const data::Value* v = node.field(member);
    if (!v || v->is_null()) { out = Var{}; return false; }
    return eval(member, out);
}
bool NodeCtx::assign(const char* member, const Var& v) const {
    const data::Value* f = node.field(member);
    if (!f || f->is_null()) { g.error(&node, std::string("assign ") + member + ": member is not set"); return false; }
    std::string err;
    if (g.evaluator_.assign(*f, v, err)) return true;
    g.error(&node, std::string("assign ") + member + ": " + err);
    return false;
}
bool NodeCtx::fire(const char* plug_member, std::uint32_t element) const {
    const OutPlug* p = node.out(plug_member, element);
    if (!p) return false;
    return g.fire_plug(node, *p, state ? &state->rt : nullptr);
}
void NodeCtx::host_call(HostCall::Phase phase, void (Host::*fn)(const HostCall&), HostCall call) const {
    call.cls = node.cls;
    call.graph = g.graph_.resource_id;
    call.node = node.index;
    call.phase = phase;
    std::string text = call.detail;
    if (call.resource) { char b[24]; std::snprintf(b, sizeof b, " res:%016llX", (unsigned long long)call.resource); text += b; }
    g.note(TraceEvent::Kind::Host, &node, text);
    (g.host_.*fn)(call);
}

// ------------------------------------------------------------------------------------------------------------------
// GraphInstance
// ------------------------------------------------------------------------------------------------------------------
GraphInstance::GraphInstance(const Graph& graph, const GraphLibrary& library, Host& host, const BehaviorRegistry& registry,
                             InstanceOptions options, std::uint16_t instance_id)
    : graph_(graph), library_(library), host_(host), registry_(registry), options_(options), instance_id_(instance_id),
      evaluator_(*this) {
    state_slots_.assign(graph.states.size(), nullptr);
    action_slots_.assign(graph.nodes.size(), nullptr);
    state_activity_.assign(graph.states.size(), 0);
    view_.states = state_slots_.data();
    view_.state_count = state_slots_.size();
    view_.actions = action_slots_.data();
    view_.action_count = action_slots_.size();
    view_.flags = 0;
    view_.state_activity = state_activity_.data();
    rng_ = (options.seed ^ (static_cast<std::uint64_t>(instance_id) * 0xD6E8FEB86659FD93ull)) | 1;
    bag_.on_change = [this](const VarPath& p) { on_var_changed(p); };
}
GraphInstance::~GraphInstance() = default;

double GraphInstance::random01() {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17;
    return static_cast<double>(rng_ >> 11) / 9007199254740992.0;
}
bool GraphInstance::message_param(std::uint64_t def, Var& out) const {
    auto it = msg_params_.find(def);
    if (it == msg_params_.end()) return false;
    out = it->second;
    return true;
}

void GraphInstance::note(TraceEvent::Kind kind, const GraphNode* n, std::string text) {
    TraceEvent e;
    e.kind = kind; e.frame = frame_; e.graph = graph_.resource_id;
    e.node = n ? static_cast<std::int32_t>(n->index) : -1;
    e.cls = n ? n->cls : "";
    e.text = std::move(text);
    trace_.push_back(std::move(e));
}
void GraphInstance::error(const GraphNode* n, std::string text) {
    char b[48];
    std::snprintf(b, sizeof b, "[%016llX", (unsigned long long)graph_.resource_id);
    std::string full = std::string(b) + "#" + (n ? std::to_string(n->index) : std::string("-")) + "] " + text;
    errors_.push_back(full);
    note(TraceEvent::Kind::Error, n, std::move(text));
}

NodeCtx GraphInstance::ctx(const GraphNode& n, VmState* s, const InPlug* in, StateRuntime* sender) {
    return NodeCtx{*this, n, s, in, sender};
}

Notification& GraphInstance::new_notification() {
    Notification* n;
    if (!free_.empty()) { n = free_.back(); free_.pop_back(); }
    else { pool_.emplace_back(); n = &pool_.back(); }
    *n = Notification{};
    n->instance_id = instance_id_;
    n->frame = frame_;
    return *n;
}

// ---- runtime::EngineActions --------------------------------------------------------------------------------------
StateRuntime* GraphInstance::instantiate_state(std::uint32_t idx) {
    const GraphState* gs = graph_.state(idx);
    if (!gs) return nullptr;
    auto vs = std::make_unique<VmState>();
    vs->gs = gs;
    vs->node = gs->node;
    vs->behavior = registry_.resolve(*gs->node);
    vs->rt.native_object = vs.get();
    vs->rt.flags = 0;
    vs->rt.state_index = idx;
    vs->rt.unique_id_low = gs->node->authored.unique_id_low;
    vs->rt.scoped = gs->node->authored.scoped;   // F4E230 copies authored +0x75
    vs->rt.instance = &view_;
    StateRuntime* rt = &vs->rt;
    state_slots_[idx] = rt;
    state_activity_[idx] = 0;
    VmState* raw = vs.get();
    states_.push_back(std::move(vs));
    if (raw->behavior) {
        NodeCtx c = ctx(*raw->node, raw);
        raw->data = raw->behavior->create_data(c);
        raw->behavior->on_initialize(c);
    }
    return rt;
}

const runtime::AuthoredNode* GraphInstance::authored_state(std::uint32_t idx) {
    const GraphState* gs = graph_.state(idx);
    return gs ? &gs->node->authored : nullptr;
}

void GraphInstance::abort_group(const OutPlug& plug) {
    // F4A8B0: skipped when instance flag 0x100 is set; in mode 0x1000 only client-scoped nodes participate.
    if (plug.subgraph_states.empty() || (view_.flags & 0x100)) return;
    std::vector<std::uint32_t> idx;
    for (std::int32_t s : plug.subgraph_states) {
        if (s < 0) continue;
        idx.push_back(static_cast<std::uint32_t>(s));
        if (StateRuntime* rt = runtime::state_at(view_, static_cast<std::uint32_t>(s))) abort_state(*rt);
    }
    runtime::cancel_pending_begins(view_, queue_, runtime::IndexRange{idx.data(), idx.size()}, frame_, *this);   // F4FD00
}
void GraphInstance::cancel_timers(VmState& s) { cancel_state_notifications(s.rt.state_index); }
void NodeCtx::abort_group(const char* plug_member, std::uint32_t element) const {
    if (const OutPlug* p = node.out(plug_member, element)) g.abort_group(*p);
}

void* GraphInstance::instantiate_action(std::uint32_t node_index) {
    if (node_index >= action_slots_.size()) return nullptr;
    if (!action_slots_[node_index]) action_slots_[node_index] = const_cast<GraphNode*>(graph_.node(node_index));
    return action_slots_[node_index];
}

bool GraphInstance::begin_state(StateRuntime& rt, const Notification&) {
    VmState* vs = vm_state(rt);
    if (!vs->behavior) {
        error(vs->node, "unsupported node class " + vs->node->cls + " (state): not started");
        ++unsupported_[vs->node->cls];
        return true;
    }
    runtime::begin_state(view_, rt, vs->gs->def, frame_, 0, *this, *this);
    return true;
}
void GraphInstance::finish_state(StateRuntime& rt) {
    VmState* vs = vm_state(rt);
    if (!vs->behavior) return;
    runtime::end_state(view_, rt, vs->gs->def, lists_, frame_, false, *this, *this);
}
void GraphInstance::abort_state(StateRuntime& rt) {
    VmState* vs = vm_state(rt);
    if (!vs->behavior) return;
    runtime::end_state(view_, rt, vs->gs->def, lists_, frame_, true, *this, *this);
}
void GraphInstance::tick_state(StateRuntime& rt, std::int64_t frame) {
    VmState* vs = vm_state(rt);
    if (!vs->behavior || !runtime::is_active(rt)) return;
    NodeCtx c = ctx(*vs->node, vs);
    vs->behavior->on_tick(c, frame);
}
void GraphInstance::timer(StateRuntime& rt, std::uint32_t user) {
    VmState* vs = vm_state(rt);
    if (!vs->behavior) return;
    note(TraceEvent::Kind::Timer, vs->node, "timer user=" + std::to_string(user));
    NodeCtx c = ctx(*vs->node, vs);
    vs->behavior->on_timer(c, user);
}
void GraphInstance::evaluate_exits(StateRuntime& rt) {
    VmState* vs = vm_state(rt);
    if (!vs->behavior) return;
    // F48870: a state with a pending exit (flag 0x10) is not evaluated again.
    if (rt.flags & 0x10) return;
    rt.flags &= ~4u;
    NodeCtx c = ctx(*vs->node, vs);
    vs->behavior->evaluate_exits(c);
    bool exit = false;
    // F48790 (generic state predicate, vtbl +0xD0): every transitionPlug element in order; the first one whose
    // link activation succeeds ends the evaluation.
    for (const OutPlug& p : vs->node->out_plugs) {
        if (p.field != field_hash("m_transitionPlug")) continue;
        if (runtime::activate_plug(view_, vs->node->authored, p.links.data(), p.links.size(), *this, vs->rt.native_object, p.field)) { exit = true; break; }
    }
    if (exit && !(view_.flags & 4) && !(rt.flags & 4) && runtime::is_active(rt)) {
        rt.flags |= 0x10;
        note(TraceEvent::Kind::Note, vs->node, "transition taken: aborting state");
        request_abort(rt.state_index, 0);
        runtime::cancel_pending_finish(view_, queue_, rt.state_index, frame_, *this);   // F4FE30
    }
    rt.flags &= ~4u;
}
void GraphInstance::attribute_changed(StateRuntime& rt) {
    VmState* vs = vm_state(rt);
    if (!vs->behavior) return;
    NodeCtx c = ctx(*vs->node, vs);
    vs->behavior->on_attribute_changed(c);
    evaluate_exits(rt);   // [INFERENCE] a watched variable change also re-evaluates the transitions
}
void GraphInstance::traverse_from(std::uint32_t node_index, const Notification&) {
    const GraphNode* n = graph_.node(node_index);
    if (!n) { error(nullptr, "TraverseFrom of unknown node " + std::to_string(node_index)); return; }
    runtime::activate_link(view_, n->authored, &n->authored, *this, nullptr, nullptr, 0, -1);
}
void GraphInstance::set_variable(const Notification& ev, bool) {
    if (ev.payload >= pending_sets_.size()) { error(nullptr, "SetVariable notification without payload"); return; }
    const auto& p = pending_sets_[ev.payload];
    bag_.set(p.first, p.second, frame_);
    note(TraceEvent::Kind::Var, nullptr, path_string(p.first) + " = " + p.second.to_string() + " (external)");
}
void GraphInstance::stop_instance() {
    note(TraceEvent::Kind::Note, nullptr, "StopInstance");
    view_.flags |= 4;
    for (auto& s : states_)
        if (runtime::is_active(s->rt)) abort_state(s->rt);
    for (auto& ch : children_) ch->stop();
}
// execute_notification case 7 (F4F600): record the pressed set (+0x377 down flag, +0x380 held list), then (1) every
// button-condition node (class STU_4768E4A3: authored +0xA0 button, +0xA8/+0xA9 down/up edge, +0xAA/+0xAB unconditional,
// F66640 == runtime::logical_button_match) that matches re-evaluates the exits (vtbl +0x1E8) of the active states listed in
// its +0x90 list; (2) every button-listening state (instantiated on demand, F4FB2A) gets its vtbl +0x1C8 handler.
void GraphInstance::logical_button(const Notification& ev) {
    const std::int32_t button = static_cast<std::int32_t>(ev.payload);
    if (ev.going_down) pressed_.insert(button); else pressed_.erase(button);
    cur_button_ = button;
    cur_down_ = ev.going_down;
    std::size_t cond_hits = 0, matched = 0;
    for (const auto& up : graph_.nodes) {
        const GraphNode& n = *up;
        if (n.cls != "STU_4768E4A3") continue;
        Var b;
        std::string err;
        if (!evaluator_.eval_field(n, "m_logicalButton", b, err)) { error(&n, "button condition: " + err); continue; }
        const int m = runtime::logical_button_match(button, ev.going_down, b.is_set() ? static_cast<int>(b.as_int()) : -1,
                                                    n.get_bool("m_30C0A41D"), n.get_bool("m_86CD8AF0"),
                                                    n.get_bool("m_A3E7D1E9"), n.get_bool("m_B4015D42"));
        if (m == 0) continue;
        ++cond_hits;
        if (const data::Value* lst = n.field("m_C77035BE"))
            for (const data::Value& e : lst->list) {
                StateRuntime* rt = runtime::state_at(view_, static_cast<std::uint32_t>(e.kind == data::Value::Kind::SInt ? e.i : static_cast<std::int64_t>(e.u)));
                if (rt && runtime::is_active(*rt)) evaluate_exits(*rt);
            }
    }
    for (const auto& gs : graph_.states) {
        const Behavior* b = registry_.resolve(*gs->node);
        if (!b || !b->listens_buttons()) continue;
        StateRuntime* rt = runtime::state_at(view_, gs->state_index);
        if (!rt) rt = instantiate_state(gs->state_index);
        if (!rt) continue;
        VmState* vs = vm_state(*rt);
        NodeCtx c = ctx(*vs->node, vs);
        if (b->on_logical_button(c, button, ev.going_down) != 0) ++matched;
    }
    cur_button_ = -1;
    note(TraceEvent::Kind::Button, nullptr, "button " + std::to_string(button) + (ev.going_down ? " down" : " up") + ": " +
         std::to_string(cond_hits) + " condition node(s), " + std::to_string(matched) + " state handler(s) matched");
    for (auto& ch : children_) ch->deliver_logical_button(button, ev.going_down);
}
bool GraphInstance::condition(const runtime::AuthoredNode& target) {
    // [INFERENCE] instance vtbl +0x88: client/server scope gate. serverOnly nodes do not run on a client instance and
    // clientOnly nodes do not run on a server instance (authored +0x75/+0x76).
    const GraphNode* n = graph_.node(target.node_index);
    if (!n) return false;
    if (options_.side == InstanceOptions::Side::Server && n->client_only) return false;
    if (options_.side == InstanceOptions::Side::Client && n->server_only) return false;
    return true;
}
bool GraphInstance::execute_node(void* implementation, const runtime::AuthoredNode& target, const void* input_plug, void* sender_runtime) {
    const GraphNode* n = graph_.node(target.node_index);
    if (!n) return false;
    const InPlug* in = static_cast<const InPlug*>(input_plug);
    StateRuntime* sender = nullptr;
    if (sender_runtime) sender = &static_cast<VmState*>(sender_runtime)->rt;
    if (target.is_state) {
        VmState* vs = static_cast<VmState*>(implementation);
        if (!vs->behavior) {
            error(n, "unsupported node class " + n->cls + " (state): activation ignored");
            ++unsupported_[n->cls];
            return false;
        }
        // F48700 / vtbl +0xF8
        return runtime::execute_state_input(view_, vs->rt, input_plug, in && in->abort, sender_runtime, frame_, instance_id_, *this);
    }
    const Behavior* b = registry_.resolve(*n);
    if (!b) {
        error(n, "unsupported node class " + n->cls + ": not executed");
        ++unsupported_[n->cls];
        return false;
    }
    ++executed_;
    note(TraceEvent::Kind::Execute, n, "execute");
    NodeCtx c = ctx(*n, nullptr, in, sender);
    return b->execute(c);
}
void GraphInstance::enter_scope() { ++scope_depth_; }
void GraphInstance::leave_scope() { if (scope_depth_ > 0) --scope_depth_; }

// ---- runtime::LifecycleActions -----------------------------------------------------------------------------------
void GraphInstance::state_hook(StateRuntime& rt, runtime::StateHook hook, int arg) {
    VmState* vs = vm_state(rt);
    if (!vs->behavior) return;
    NodeCtx c = ctx(*vs->node, vs);
    using H = runtime::StateHook;
    switch (hook) {
        case H::Initialize: break;   // done in instantiate_state
        case H::Subscribe:
            if (const data::Value* deps = vs->node->field("m_652A186C"))
                for (const data::Value& d : deps->list) {
                    VarPath p; std::string e;
                    if (evaluator_.resolve_path(d, p, e)) subscribe(*vs, p);
                }
            break;
        case H::Unsubscribe:
            for (const VarPath& p : vs->subscriptions) {
                auto it = watchers_.find(p);
                if (it == watchers_.end()) continue;
                it->second.erase(std::remove(it->second.begin(), it->second.end(), rt.state_index), it->second.end());
            }
            vs->subscriptions.clear();
            break;
        case H::PreBegin160: vs->behavior->on_pre_begin(c, arg != 0); break;
        case H::Begin108:
            ++executed_;
            note(TraceEvent::Kind::Begin, vs->node, "begin");
            vs->behavior->on_begin(c);
            break;
        case H::Begin110: vs->behavior->on_post_begin(c); break;
        case H::Reenter118: note(TraceEvent::Kind::Begin, vs->node, "reenter"); vs->behavior->on_reenter(c); break;
        case H::EvaluateExits: evaluate_exits(rt); break;
        case H::Finish148: note(TraceEvent::Kind::Finish, vs->node, "finish"); vs->behavior->on_finish(c); break;
        case H::Abort150: note(TraceEvent::Kind::Abort, vs->node, "abort"); vs->behavior->on_abort(c); break;
        case H::End158: vs->behavior->on_end(c); break;
        case H::PreEnd168: break;
        case H::Abort248: vs->behavior->on_aborted(c); break;
        case H::Finish250: vs->behavior->on_finished(c); break;
        default: break;
    }
}
void GraphInstance::manager_event(StateRuntime&, runtime::ManagerStateEvent) {}
void GraphInstance::cancel_state_notifications(std::uint32_t state_index) {
    for (Notification* e = queue_.head; e;) {
        Notification* following = e->prev;
        bool match = false;
        if (e->type == NotificationType::Timer) match = e->timer_state_index == state_index;
        else if (e->type == NotificationType::AttributeChanged || e->type == NotificationType::EvaluateExits) match = e->payload == state_index;
        if (match) { queue_.remove(*e); release(*e); }
        e = following;
    }
}
void GraphInstance::cancel_pending_begins(runtime::IndexRange range, std::int64_t frame) {
    runtime::cancel_pending_begins(view_, queue_, range, frame, *this);
}
bool GraphInstance::retain_subscriptions(StateRuntime&) { return false; }
bool GraphInstance::retain_memberships(StateRuntime&) { return false; }
void GraphInstance::remove_special_update(StateRuntime&) {}

// ---- runtime::SchedulerActions -----------------------------------------------------------------------------------
std::int32_t GraphInstance::associated_state(const Notification& e) {
    switch (e.type) {
        case NotificationType::Lifetime: return e.state_index;
        case NotificationType::Timer: return e.timer_state_index;
        case NotificationType::AttributeChanged:
        case NotificationType::EvaluateExits: return static_cast<std::int32_t>(e.payload);
        default: return -1;
    }
}
void GraphInstance::release(Notification& e) { free_.push_back(&e); }
void GraphInstance::reset_frame_traversal() {}
void GraphInstance::runaway_warning() { error(nullptr, "runaway: 980 notifications in one frame"); }

// ---- runtime::StateSchedulingActions ----------------------------------------------------------------------------
bool GraphInstance::can_schedule(std::uint32_t) { return true; }
Notification& GraphInstance::allocate_lifetime() { return new_notification(); }
void GraphInstance::submit(Notification& n) { queue_.insert_sorted(n, frame_); }
bool GraphInstance::consume_input(StateRuntime& rt, const void* input_plug) {
    VmState* vs = vm_state(rt);
    if (!vs->behavior) return false;
    NodeCtx c = ctx(*vs->node, vs, static_cast<const InPlug*>(input_plug));
    return vs->behavior->consume_input(c);
}

// ---- API for behaviours ------------------------------------------------------------------------------------------
bool GraphInstance::fire_plug(const GraphNode& n, const OutPlug& plug, StateRuntime* sender) {
    return runtime::activate_plug(view_, n.authored, plug.links.data(), plug.links.size(), *this,
                                  sender ? sender->native_object : nullptr, plug.field);
}
void GraphInstance::request_begin(std::uint32_t state_index, std::int64_t delay_ms) {
    StateRuntime* rt = runtime::state_at(view_, state_index);
    if (!rt) rt = instantiate_state(state_index);
    if (rt) runtime::schedule_lifetime(view_, *rt, runtime::LifetimeKind::Begin, frame_, delay_ms, instance_id_, *this);
}
void GraphInstance::request_finish(VmState& s, std::int64_t delay_ms) {
    runtime::schedule_lifetime(view_, s.rt, runtime::LifetimeKind::Finish, frame_, delay_ms, instance_id_, *this);
}
void GraphInstance::request_abort(std::uint32_t state_index, std::int64_t delay_ms) {
    StateRuntime* rt = runtime::state_at(view_, state_index);
    if (rt) runtime::schedule_lifetime(view_, *rt, runtime::LifetimeKind::Abort, frame_, delay_ms, instance_id_, *this);
}
void GraphInstance::schedule_timer(VmState& s, std::uint32_t user, std::int64_t delay_ms) {
    if (view_.flags & 4) return;
    Notification& n = new_notification();
    n.type = NotificationType::Timer;
    n.frame = frame_ + delay_ms;
    n.payload = user;
    n.timer_state_index = static_cast<std::uint16_t>(s.rt.state_index);
    queue_.insert_sorted(n, frame_);
}
void GraphInstance::subscribe(VmState& s, const VarPath& path) {
    if (path.empty()) return;
    auto& w = watchers_[path];
    if (std::find(w.begin(), w.end(), s.rt.state_index) == w.end()) w.push_back(s.rt.state_index);
    if (std::find(s.subscriptions.begin(), s.subscriptions.end(), path) == s.subscriptions.end()) s.subscriptions.push_back(path);
}
void GraphInstance::on_var_changed(const VarPath& path) {
    auto it = watchers_.find(path);
    if (it == watchers_.end()) return;
    for (std::uint32_t idx : it->second) {
        StateRuntime* rt = runtime::state_at(view_, idx);
        if (!rt || !runtime::is_active(*rt)) continue;
        Notification& n = new_notification();
        n.type = NotificationType::AttributeChanged;
        n.payload = idx;
        queue_.insert_sorted(n, frame_);
    }
}

GraphInstance* GraphInstance::start_child(const Graph& graph, std::function<void(GraphInstance&)> preset) {
    if (options_.depth >= 8) { error(nullptr, "sub-script nesting too deep"); return nullptr; }
    InstanceOptions o = options_;
    o.depth = options_.depth + 1;
    auto child = std::make_unique<GraphInstance>(graph, library_, host_, registry_, o,
                                                 static_cast<std::uint16_t>(instance_id_ + children_.size() + 1));
    child->frame_ = frame_;
    child->parent_ = this;
    if (preset) preset(*child);
    GraphInstance* raw = child.get();
    children_.push_back(std::move(child));
    char b[48];
    std::snprintf(b, sizeof b, "child graph %016llX", (unsigned long long)graph.resource_id);
    note(TraceEvent::Kind::Note, nullptr, b);
    raw->start(true);
    return raw;
}
void GraphInstance::stop_child(GraphInstance* child) {
    if (child) child->stop();
}
bool GraphInstance::complete_state(std::uint32_t node_index) {
    const GraphNode* n = graph_.node(node_index);
    if (!n || !n->is_state) return false;
    StateRuntime* rt = runtime::state_at(view_, n->authored.state_index);
    if (!rt || !runtime::is_active(*rt)) return false;
    request_finish(*vm_state(*rt), 0);
    pump();
    return true;
}
void GraphInstance::set_variable_external(const VarPath& path, const Var& value) {
    pending_sets_.emplace_back(path, value);
    Notification& n = new_notification();
    n.type = NotificationType::SetVariable;
    n.payload = static_cast<std::uint32_t>(pending_sets_.size() - 1);
    queue_.insert_sorted(n, frame_);
    pump();
}

// ---- driving -----------------------------------------------------------------------------------------------------
bool GraphInstance::start(bool include_play_script_entries) {
    if (view_.flags & 4) return false;
    std::size_t queued = 0;
    for (std::uint32_t idx : graph_.entries) {
        const GraphNode* n = graph_.node(idx);
        const bool plain = n->cls == "STUStatescriptEntry";
        const bool script = n->cls == "STUStatescriptEntryPlayScript";
        if (!plain && !(script && include_play_script_entries)) continue;
        Notification& e = new_notification();
        e.type = NotificationType::TraverseFrom;
        e.payload = idx;
        queue_.insert_sorted(e, frame_);
        ++queued;
    }
    note(TraceEvent::Kind::Note, nullptr, "start: " + std::to_string(queued) + " entry traversal(s)");
    pump();
    return true;
}
void GraphInstance::deliver_logical_button(std::int32_t button, bool going_down) {
    if (view_.flags & 4) return;
    Notification& n = new_notification();
    n.type = NotificationType::LogicalButton;
    n.payload = static_cast<std::uint32_t>(button);
    n.going_down = going_down;
    queue_.insert_sorted(n, frame_);
    pump();
}
void GraphInstance::deliver_game_message(std::uint64_t message, const std::vector<std::pair<std::uint64_t, Var>>& params) {
    if (view_.flags & 4) return;
    std::size_t hits = 0;
    for (std::uint32_t idx : graph_.entries) {
        const GraphNode* n = graph_.node(idx);
        const Behavior* b = registry_.resolve(*n);
        if (!b) continue;
        NodeCtx c = ctx(*n);
        if (!b->on_game_message(c, message, params)) continue;
        ++hits;
        Notification& e = new_notification();
        e.type = NotificationType::TraverseFrom;
        e.payload = idx;
        queue_.insert_sorted(e, frame_);
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "game message %016llX: %zu entry(ies)", (unsigned long long)message, hits);
    note(TraceEvent::Kind::Note, nullptr, buf);
    pump();
    for (auto& ch : children_) ch->deliver_game_message(message, params);
}
void GraphInstance::pump() {
    if (view_.flags & 4) return;
    const auto r = runtime::pump_notifications(view_, queue_, frame_, false, *this, *this);
    if (r == runtime::PumpResult::Runaway) error(nullptr, "pump aborted: runaway instance stopped");
    for (auto& ch : children_) ch->pump();
}
void GraphInstance::stop() {
    if (view_.flags & 4) return;
    Notification& n = new_notification();
    n.type = NotificationType::StopInstance;
    queue_.insert_sorted(n, frame_);
    pump();
}
bool GraphInstance::is_active(std::uint32_t idx) const {
    const StateRuntime* rt = runtime::state_at(view_, idx);
    return rt && runtime::is_active(*rt);
}
const VmState* GraphInstance::state(std::uint32_t idx) const {
    const StateRuntime* rt = runtime::state_at(view_, idx);
    return rt ? static_cast<const VmState*>(rt->native_object) : nullptr;
}
bool GraphInstance::has_ticker() const {
    for (const auto& s : states_)
        if (s->behavior && s->behavior->wants_tick() && runtime::is_active(s->rt)) return true;
    return false;
}
void GraphInstance::tick_active(std::int64_t frame) {
    std::vector<VmState*> snapshot;
    for (auto& s : states_) snapshot.push_back(s.get());
    for (VmState* vs : snapshot)
        if (vs->behavior && vs->behavior->wants_tick() && runtime::is_active(vs->rt)) tick_state(vs->rt, frame);
}
bool GraphInstance::idle() const {
    if (queue_.head) return false;
    for (const auto& s : states_)
        if (runtime::is_active(s->rt)) return false;
    return true;
}
std::int64_t GraphInstance::next_time() const {
    if (view_.flags & 4) return INT64_MAX;
    std::int64_t t = queue_.head ? queue_.head->frame : INT64_MAX;
    if (has_ticker()) {
        const std::int64_t tm = options_.tick_ms > 0 ? options_.tick_ms : 1;
        t = std::min(t, (frame_ / tm + 1) * tm);
    }
    for (const auto& ch : children_) t = std::min(t, ch->next_time());
    return t;
}
void GraphInstance::step_to(std::int64_t t) {
    if (t > frame_) frame_ = t;
    pump();
    if (has_ticker() && options_.tick_ms > 0 && frame_ % options_.tick_ms == 0) { tick_active(frame_); pump(); }
    for (auto& ch : children_) ch->step_to(t);
}
void GraphInstance::advance_to(std::int64_t target) {
    for (int guard = 0; guard < 1000000; ++guard) {
        const std::int64_t t = next_time();
        if (t == INT64_MAX || t > target) break;
        step_to(std::max(t, frame_));
    }
    if (target > frame_) { frame_ = target; for (auto& ch : children_) ch->step_to(target); }
    pump();
}

// ------------------------------------------------------------------------------------------------------------------
Coverage compute_coverage(const GraphLibrary& lib, const BehaviorRegistry& reg) {
    Coverage c;
    for (std::uint64_t rid : lib.ids()) {
        const Graph* g = lib.find(rid);
        for (const auto& n : g->nodes) {
            ++c.total;
            ++c.total_by_class[n->cls];
            const Behavior* b = reg.resolve(*n);
            if (!b) { ++c.unsupported; ++c.unsupported_by_class[n->cls]; }
            else if (b->support() == Support::Implemented) ++c.implemented;
            else ++c.forwarded;
        }
    }
    return c;
}

}  // namespace ow::statescript::vm
