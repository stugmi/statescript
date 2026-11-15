// statescript/vm/vm.h -- Statescript graph interpreter.
//
// GraphInstance executes ONE decoded graph (vm/graph.h) and implements runtime::EngineActions (plus the lifecycle /
// scheduler / scheduling boundaries) so the already recovered runtime/execution.cpp, lifecycle.cpp and scheduling.cpp
// pump the nodes: notification queue (Lifetime/Timer/EvaluateExits/TraverseFrom/SetVariable/LogicalButton/...),
// plug activation, begin_state / end_state ordering, same-frame cancellation.  What this layer adds is the part the
// runtime deliberately left as a boundary: node BEHAVIOURS (vm/vm.h Behavior), the variable bag, the config-var /
// expression evaluator (vm/eval.h) and the game-side host interface (Host).
//
// Behaviour evidence rules (docs/statescript/vm.md): every Behavior documents the client RVA it was read from, or is
// flagged [INFERENCE] (server-side classes have NO client implementation; see vm.md section "Where behaviour lives").
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <set>
#include <string_view>
#include <vector>

#include "statescript/runtime/lifecycle.h"
#include "statescript/vm/eval.h"
#include "statescript/vm/graph.h"
#include "statescript/vm/value.h"

namespace ow::statescript::vm {

class GraphInstance;

// ------------------------------------------------------------------------------------------------------------------
// Host: every game-world side effect and world query. Pure virtual: the server/client implements it; the smoke
// test records calls.  One envelope type for all effects, so adding a node class never changes the ABI.
// ------------------------------------------------------------------------------------------------------------------
struct HostCall {
    enum class Phase : std::uint8_t { Once, Begin, End, Tick };
    std::string cls;                 // authored node class, e.g. "STUStatescriptActionEffect"
    std::uint64_t graph = 0;         // graph resource id
    std::uint32_t node = 0;          // node index
    Phase phase = Phase::Once;
    std::uint64_t resource = 0;      // primary resource: effect / anim alias / game message / aura / entity definition / hardpoint ...
    std::string detail;              // short verb for the log ("play", "damage", "pressed", "cooldown"...)
    std::vector<std::pair<std::string, Var>> fields;   // evaluated authored parameters (member names without "m_")
    const Var* field(std::string_view name) const;
};

class Host {
public:
    virtual ~Host() = default;
    // --- world queries (config-var getters) -------------------------------------------------------------------
    virtual std::uint64_t owner_entity() = 0;                                  // STUConfigVarGetOwner
    // A ConfigVar getter the evaluator has no built-in for (GetTeam, GetEntityHealth, GetEntityExists, ...).
    // `args` are the already evaluated child config vars in authored field order. Return false + error if unknown.
    virtual bool world_query(const std::string& config_class, const std::vector<Var>& args, Var& out, std::string& error) = 0;
    // --- gameplay side effects -------------------------------------------------------------------------------
    virtual void apply_effect(const HostCall&) = 0;        // ActionEffect / StateEffect / BeamEffect / aura-like (Begin/End phases)
    virtual void spawn_projectile(const HostCall&) = 0;    // WeaponVolley shot / projectile entity
    virtual void play_animation(const HostCall&) = 0;      // StateAnim / anim alias
    virtual void modify_health(const HostCall&) = 0;       // damage / heal / health pool
    virtual void ability_event(const HostCall&) = 0;       // ability pressed/released, charge, cooldown, ultimate charge, resource spend
    virtual void track_targets(const HostCall&, std::vector<std::uint64_t>& out_entities) = 0;   // StateTrackTargets query
    virtual void send_game_message(const HostCall&) = 0;   // ActionSendGameMessage targets+params
    virtual void criteria_event(const HostCall&) = 0;      // ActionCriteria (identifier + targets): stat/challenge criteria trigger
    virtual void apply_aura(const HostCall&) = 0;          // StateApplyAura (Begin applies, End removes)
    virtual void destroy_entity(const HostCall&) = 0;      // ActionDestroyEntity
    virtual void presentation(const HostCall&) = 0;        // anything cosmetic/other (UI, camera, movement mod, cosmetic entity, ...)
};

// ------------------------------------------------------------------------------------------------------------------
// Behaviours.
// ------------------------------------------------------------------------------------------------------------------
enum class Support : std::uint8_t {
    Implemented,     // class specific logic, evidence in vm.md
    HostForwarded,   // generic base lifecycle (proven for classes without a client factory: F4E230/F47740) + a Host call
};

struct VmState;
struct StateData { virtual ~StateData() = default; };    // behaviour private per-state storage

struct NodeCtx {
    GraphInstance& g;
    const GraphNode& node;
    VmState* state = nullptr;              // null for action / entry nodes
    const InPlug* input = nullptr;         // plug the activation arrived on (execute / consume_input)
    runtime::StateRuntime* sender = nullptr;   // runtime state that fired the plug (may be null)
    bool eval(const char* member, Var& out) const;                 // false (+ instance error) on failure
    bool eval_opt(const char* member, Var& out) const;             // false silently when the member is Null/absent
    bool assign(const char* member, const Var& v) const;           // write through an STUConfigVarDynamic member
    bool fire(const char* plug_member, std::uint32_t element = 0) const;   // activate every link of the plug (OR of results)
    void abort_group(const char* plug_member, std::uint32_t element = 0) const;   // abort the states in the plug's group list (F4A8B0)
    void host_call(HostCall::Phase phase, void (Host::*fn)(const HostCall&), HostCall call) const;
};

class Behavior {
public:
    virtual ~Behavior() = default;
    virtual std::string_view cls() const = 0;                 // Schema class name this implements
    virtual Support support() const { return Support::Implemented; }
    // ---- non state nodes (actions, entries, conditions): runtime activate_link -> execute_node (node vtbl +0xF8)
    // Return the activation result (true = executed).  Default: error "not an action".
    virtual bool execute(NodeCtx& c) const;
    // ---- state nodes: hooks named by the original vtable byte offsets (runtime::StateHook)
    virtual std::unique_ptr<StateData> create_data(NodeCtx&) const { return nullptr; }
    virtual void on_initialize(NodeCtx&) const {}             // +0x100 (after instantiation)
    virtual void on_pre_begin(NodeCtx&, bool) const {}        // +0x160
    virtual void on_begin(NodeCtx&) const {}                  // +0x108  (before onBegin/subgraph plugs fire)
    virtual void on_post_begin(NodeCtx&) const {}             // +0x110  (after plugs; Wait/Stack/... start timers here)
    virtual void on_reenter(NodeCtx&) const {}                // +0x118
    virtual void on_finish(NodeCtx&) const {}                 // +0x148 / +0x250
    virtual void on_abort(NodeCtx&) const {}                  // +0x150 / +0x248
    virtual void on_end(NodeCtx&) const {}                    // +0x158 / +0x168 (always, finish or abort)
    virtual void on_timer(NodeCtx&, std::uint32_t /*user*/) const {}        // +0x140
    virtual void on_tick(NodeCtx&, std::int64_t /*frame*/) const {}         // +0x138 (only when wants_tick)
    virtual bool wants_tick() const { return false; }
    virtual void on_attribute_changed(NodeCtx&) const {}      // +0x170 (a subscribed variable changed)
    virtual void on_finished(NodeCtx&) const {}               // +0x250 (fires authored onFinishedPlug-style plugs; after on_end)
    virtual void on_aborted(NodeCtx&) const {}                // +0x248 (fires authored onAbortPlug-style plugs; after on_end)
    // +0x1E8: exit evaluation after begin / timer / var change. Default = generic transitions (see vm.md); a
    // behaviour overrides to finish itself (`c.g.finish(*c.state)`).
    virtual void evaluate_exits(NodeCtx&) const {}
    // +0x240: consume an input before it schedules Begin (true = consumed, no Begin scheduled)
    virtual bool consume_input(NodeCtx&) const { return false; }
    // LogicalButton notification (type 7). Return value of runtime::logical_button_match (0 none / 1 matched / 2 unconditional);
    // default: not a button node.
    virtual int on_logical_button(NodeCtx&, std::int32_t /*button*/, bool /*going_down*/) const { return 0; }
    // States that must be routed logical-button events even before they ever began (the client instantiates them lazily, F4FB2A).
    virtual bool listens_buttons() const { return false; }
    // EntryGameMessage-style entry nodes: called for every entry node when a game message is delivered; return true
    // when this node accepted the message (it then fires its own plugs). Default: not a message entry.
    virtual bool on_game_message(NodeCtx&, std::uint64_t /*message*/, const std::vector<std::pair<std::uint64_t, Var>>& /*params*/) const { return false; }
};

class BehaviorRegistry {
public:
    using Fallback = std::function<std::unique_ptr<Behavior>(const GraphNode&)>;
    static const BehaviorRegistry& standard();                // all built-in behaviours (every register_*)
    void add(std::unique_ptr<Behavior> b);
    // Structural fallback for classes without a dedicated behaviour (see nodes_host.cpp). Returns null = unsupported.
    void set_fallback(Fallback f) { fallback_ = std::move(f); }
    const Behavior* find(std::string_view cls) const;          // dedicated behaviours only
    const Behavior* resolve(const GraphNode& n) const;         // dedicated, else structural fallback, else nullptr
    std::vector<std::string> classes() const;
private:
    std::map<std::string, std::unique_ptr<Behavior>, std::less<>> by_class_;
    Fallback fallback_;
    mutable std::map<std::string, std::unique_ptr<Behavior>, std::less<>> generated_;
    mutable std::set<std::string, std::less<>> rejected_;
};
// one per nodes_*.cpp
void register_flow_nodes(BehaviorRegistry&);      // nodes_flow.cpp:    Entry, State, Wait, BooleanSwitch, Switch, Stack, Watch, condition, ...
void register_data_nodes(BehaviorRegistry&);      // nodes_data.cpp:    SetVar, Criteria, array/var actions, ChaseVar, DataFlowMapping
void register_ability_nodes(BehaviorRegistry&);   // nodes_ability.cpp: LogicalButton, Ability, SubScript, PlayScript, game messages, weapon
void register_host_nodes(BehaviorRegistry&);      // nodes_host.cpp:    Effect/Anim/Health/... classes forwarded to Host

// ------------------------------------------------------------------------------------------------------------------
// Runtime state object (what runtime::StateRuntime::native_object points at).
// ------------------------------------------------------------------------------------------------------------------
struct VmState {
    runtime::StateRuntime rt;
    const GraphState* gs = nullptr;
    const GraphNode* node = nullptr;
    const Behavior* behavior = nullptr;
    std::unique_ptr<StateData> data;
    std::vector<VarPath> subscriptions;
};

struct TraceEvent {
    enum class Kind { Execute, Begin, Finish, Abort, Timer, Button, Var, Host, Error, Note } kind = Kind::Note;
    std::int64_t frame = 0;
    std::uint64_t graph = 0;
    std::int32_t node = -1;
    std::string cls, text;
};

struct InstanceOptions {
    enum class Side { Both, Server, Client } side = Side::Server;   // which nodes run: serverOnly nodes are skipped on Client and vice versa
    bool strict = true;      // unsupported node class: record Error and stop that flow (never silently succeed)
    int depth = 0;           // sub-script nesting depth (internal)
    std::int64_t tick_ms = 50;   // granularity of on_tick for states whose behaviour wants_tick (command-frame step is not modelled)
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
};

class GraphInstance : private runtime::EngineActions, private runtime::LifecycleActions,
                      private runtime::SchedulerActions, private runtime::StateSchedulingActions {
public:
    GraphInstance(const Graph& graph, const GraphLibrary& library, Host& host, const BehaviorRegistry& registry,
                  InstanceOptions options = {}, std::uint16_t instance_id = 1);
    ~GraphInstance();
    GraphInstance(const GraphInstance&) = delete;
    GraphInstance& operator=(const GraphInstance&) = delete;

    // ---- driving ------------------------------------------------------------------------------------------
    // Start: queue a TraverseFrom notification for every plain STUStatescriptEntry node (Statescript__OnScriptEntry)
    // and pump the queue at the current frame.
    bool start(bool include_play_script_entries = false);
    void deliver_logical_button(std::int32_t button, bool going_down);        // queue + pump (NotificationType::LogicalButton)
    // EntryGameMessage: deliver message `message` (resource id 0x024...) with parameters keyed by def id (0x0A4...).
    void deliver_game_message(std::uint64_t message, const std::vector<std::pair<std::uint64_t, Var>>& params);
    void advance_to(std::int64_t frame);            // run timers/ticks/notifications up to and including `frame` (ms)
    void advance_by(std::int64_t ms) { advance_to(frame_ + ms); }
    void pump();                                    // run due notifications at the current frame
    void stop();                                    // StopInstance
    // Cooperative stepping used for child instances: earliest pending time (own queue, ticks, children), step to it.
    std::int64_t next_time() const;                 // INT64_MAX when nothing is pending
    void step_to(std::int64_t t);                   // frame = t; pump; tick; recurse into children
    bool idle() const;                              // queue empty and no active non-entry state
    GraphInstance* parent() const { return parent_; }
    std::uint64_t instance_tag() const { return instance_id_; }
    // F4A8B0: abort every state listed in a subgraph plug's group (m_EAECE4C0) and cancel their same-frame pending begins.
    void abort_group(const OutPlug& plug);
    void cancel_timers(VmState& s);                 // drop pending Timer/AttributeChanged notifications of one state
    NodeCtx make_ctx(VmState& s) { return ctx(*s.node, &s); }
    // Per-variable priority stacks (STUStatescriptStateStack). Entries are owned by the instance, see nodes_flow.cpp.
    struct StackEntry { std::uint32_t state_index = 0; std::uint64_t priority = 0, seq = 0; Var value; };
    std::map<VarPath, std::vector<StackEntry>>& stacks() { return stacks_; }
    std::uint64_t next_seq() { return ++seq_; }
    VmState* mutable_state(std::uint32_t idx) { return const_cast<VmState*>(state(idx)); }
    std::vector<std::unique_ptr<VmState>>& all_states() { return states_; }

    // ---- state of the instance -----------------------------------------------------------------------------
    std::int64_t frame() const { return frame_; }
    const Graph& graph() const { return graph_; }
    const GraphLibrary& library() const { return library_; }
    Host& host() { return host_; }
    VarBag& bag() { return bag_; }
    const VarBag& bag() const { return bag_; }
    Evaluator& evaluator() { return evaluator_; }
    const InstanceOptions& options() const { return options_; }
    bool stopped() const { return (view_.flags & 4) != 0; }
    bool is_active(std::uint32_t state_index) const;
    const VmState* state(std::uint32_t state_index) const;
    const std::vector<TraceEvent>& trace() const { return trace_; }
    const std::vector<std::string>& errors() const { return errors_; }
    std::vector<std::unique_ptr<GraphInstance>>& children() { return children_; }
    // Unsupported node classes that were actually executed: class -> count.
    const std::map<std::string, std::size_t>& unsupported_hits() const { return unsupported_; }
    std::size_t executed_nodes() const { return executed_; }

    // ---- API used by behaviours ----------------------------------------------------------------------------
    void note(TraceEvent::Kind kind, const GraphNode* n, std::string text);
    void error(const GraphNode* n, std::string text);                         // trace + errors()
    bool fire_plug(const GraphNode& n, const OutPlug& plug, runtime::StateRuntime* sender);
    void request_begin(std::uint32_t state_index, std::int64_t delay_ms = 0);  // schedule Lifetime Begin
    void request_finish(VmState& s, std::int64_t delay_ms = 0);                // schedule Lifetime Finish (F4A9C0 kind 1)
    void request_abort(std::uint32_t state_index, std::int64_t delay_ms = 0);  // schedule Lifetime Abort
    void schedule_timer(VmState& s, std::uint32_t user_data, std::int64_t delay_ms);   // F47A50: timer notification at frame+delay
    void subscribe(VmState& s, const VarPath& path);                           // wake `s` (AttributeChanged) when `path` changes
    // Start a child instance of `graph` sharing host + registry (StateSubScript / ActionPlayScript). The child
    // runs its entries immediately. Variables are NOT shared unless the behaviour copies them (documented per class).
    GraphInstance* start_child(const Graph& graph, std::function<void(GraphInstance&)> preset = {});
    void stop_child(GraphInstance* child);
    // Host-driven completion of a forwarded state (anim ended, ...): fires the state's onFinishedPlug-style plug via the
    // behaviour's on_finished hook and finishes it (same path as a timer finishing a Wait).
    bool complete_state(std::uint32_t node_index);
    // External variable write (server snapshot / host): queued as a SetVariable notification and pumped.
    void set_variable_external(const VarPath& path, const Var& value);
    void set_message_param(std::uint64_t def_id, Var v) { msg_params_[def_id] = std::move(v); }
    bool message_param(std::uint64_t def_id, Var& out) const;
    double random01();                              // deterministic xorshift, seeded by InstanceOptions::seed
    bool button_held(std::int32_t button) const { return pressed_.count(button) != 0; }
    std::int32_t current_button() const { return cur_button_; }        // button of the notification being routed, -1 outside
    bool current_going_down() const { return cur_down_; }
    void finish(VmState& s) { request_finish(s, 0); }

private:
    friend struct NodeCtx;
    // runtime::EngineActions
    runtime::StateRuntime* instantiate_state(std::uint32_t state_index) override;
    const runtime::AuthoredNode* authored_state(std::uint32_t state_index) override;
    void* instantiate_action(std::uint32_t node_index) override;
    bool begin_state(runtime::StateRuntime&, const runtime::Notification&) override;
    void finish_state(runtime::StateRuntime&) override;
    void abort_state(runtime::StateRuntime&) override;
    void tick_state(runtime::StateRuntime&, std::int64_t command_frame) override;
    void timer(runtime::StateRuntime&, std::uint32_t user_data) override;
    void evaluate_exits(runtime::StateRuntime&) override;
    void attribute_changed(runtime::StateRuntime&) override;
    void traverse_from(std::uint32_t node_index, const runtime::Notification&) override;
    void set_variable(const runtime::Notification&, bool array) override;
    void stop_instance() override;
    void logical_button(const runtime::Notification&) override;
    bool condition(const runtime::AuthoredNode& target) override;
    bool execute_node(void* implementation, const runtime::AuthoredNode& target, const void* input_plug, void* sender_runtime) override;
    void enter_scope() override;
    void leave_scope() override;
    // runtime::LifecycleActions
    void state_hook(runtime::StateRuntime&, runtime::StateHook, int argument) override;
    void manager_event(runtime::StateRuntime&, runtime::ManagerStateEvent) override;
    void cancel_state_notifications(std::uint32_t state_index) override;
    void cancel_pending_begins(runtime::IndexRange, std::int64_t frame) override;
    bool retain_subscriptions(runtime::StateRuntime&) override;
    bool retain_memberships(runtime::StateRuntime&) override;
    void remove_special_update(runtime::StateRuntime&) override;
    // runtime::SchedulerActions
    std::int32_t associated_state(const runtime::Notification&) override;
    void release(runtime::Notification&) override;
    void reset_frame_traversal() override;
    void runaway_warning() override;
    // runtime::StateSchedulingActions
    bool can_schedule(std::uint32_t state_index) override;
    runtime::Notification& allocate_lifetime() override;
    void submit(runtime::Notification&) override;
    bool consume_input(runtime::StateRuntime&, const void* input_plug) override;

    runtime::Notification& new_notification();
    VmState* vm_state(runtime::StateRuntime& rt) { return static_cast<VmState*>(rt.native_object); }
    NodeCtx ctx(const GraphNode& n, VmState* s = nullptr, const InPlug* in = nullptr, runtime::StateRuntime* sender = nullptr);
    void tick_active(std::int64_t frame);
    void on_var_changed(const VarPath& path);
    bool has_ticker() const;
    void queue_notification(runtime::Notification& n) { queue_.insert_sorted(n, frame_); }

    const Graph& graph_;
    const GraphLibrary& library_;
    Host& host_;
    const BehaviorRegistry& registry_;
    InstanceOptions options_;
    std::uint16_t instance_id_;
    std::int64_t frame_ = 0;
    VarBag bag_;
    Evaluator evaluator_;
    runtime::RuntimeInstanceView view_;
    runtime::NotificationQueue queue_;
    runtime::StateLists lists_;
    std::vector<std::unique_ptr<VmState>> states_;
    std::vector<runtime::StateRuntime*> state_slots_;
    std::vector<void*> action_slots_;
    std::vector<std::uint32_t> state_activity_;
    std::deque<runtime::Notification> pool_;
    std::vector<runtime::Notification*> free_;
    std::vector<TraceEvent> trace_;
    std::vector<std::string> errors_;
    std::map<std::string, std::size_t> unsupported_;
    std::vector<std::unique_ptr<GraphInstance>> children_;
    std::size_t executed_ = 0;
    int scope_depth_ = 0;
    // variable subscriptions: path -> states
    std::map<VarPath, std::vector<std::uint32_t>> watchers_;
    std::map<std::uint64_t, Var> msg_params_;
    std::set<std::int32_t> pressed_;
    std::uint64_t rng_ = 1;
    std::vector<std::pair<VarPath, Var>> pending_sets_;
    std::vector<std::pair<std::int32_t, bool>> pending_buttons_;
    std::int64_t next_tick_ = 0;
    GraphInstance* parent_ = nullptr;
    std::map<VarPath, std::vector<StackEntry>> stacks_;
    std::uint64_t seq_ = 0;
    std::int32_t cur_button_ = -1;
    bool cur_down_ = false;
    std::vector<runtime::Notification*> due_scratch_;
    friend class Evaluator;
};

// Coverage over a graph library: how many nodes belong to classes with a registered Behavior.
struct Coverage {
    std::size_t total = 0, implemented = 0, forwarded = 0, unsupported = 0;
    std::map<std::string, std::size_t> total_by_class, unsupported_by_class;
};
Coverage compute_coverage(const GraphLibrary&, const BehaviorRegistry&);

}  // namespace ow::statescript::vm
