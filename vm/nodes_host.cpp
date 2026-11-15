// Host-forwarded node classes (structural fallback) and the field-collection helpers. See docs/statescript/vm.md.
#include <cstring>
#include <algorithm>
#include <set>

#include "statescript/vm/nodes_common.h"

namespace ow::statescript::vm {
namespace {
using data::Object;
using data::Value;

const std::set<std::uint32_t>& header_hashes() {
    static const std::set<std::uint32_t> s = [] {
        std::set<std::uint32_t> r;
        for (const char* n : {"m_displayName", "m_comment", "m_pos", "m_uniqueID", "m_debugText", "m_F8F89F89", "m_A56DC772",
                              "m_53BFB779", "m_4B062861", "m_clientScope", "m_isState", "m_clientOnly", "m_serverOnly",
                              "m_stateGroup", "m_065D841C", "m_652A186C", "m_3882159E", "m_63D0B555", "m_36391723",
                              "m_parentSubgraph", "m_83E83924", "m_links", "m_parentNode", "m_visible"})
            r.insert(field_hash(n));
        return r;
    }();
    return s;
}
const Object* obj_of(const Value& v) {
    if (v.kind == Value::Kind::ObjRef) return v.ref;
    if (v.kind == Value::Kind::Inline) return v.inl.get();
    return nullptr;
}
bool is_plug_val(const Value& v) { return v.kind == Value::Kind::ObjRef && v.ref && v.ref->get("m_links") && v.ref->get("m_parentNode"); }
std::string member_name(std::uint32_t hash) {
    std::string n = data::Schema::beta().field_name(hash);
    return n.size() > 2 && n[0] == 'm' && n[1] == '_' ? n.substr(2) : n;
}

void flatten(NodeCtx& c, HostCall& call, const Value& v, const std::string& name, int depth) {
    const data::Schema& sc = data::Schema::beta();
    switch (v.kind) {
        case Value::Kind::Null: return;
        case Value::Kind::UInt: call.fields.emplace_back(name, Var::integer(static_cast<std::int64_t>(v.u))); return;
        case Value::Kind::SInt: case Value::Kind::Enum: call.fields.emplace_back(name, Var::integer(v.i)); return;
        case Value::Kind::Float: call.fields.emplace_back(name, Var::real(v.f)); return;
        case Value::Kind::String: call.fields.emplace_back(name, Var::str(v.s)); return;
        case Value::Kind::ResRef: call.fields.emplace_back(name, Var::resource(v.u)); return;
        case Value::Kind::Raw:
            if (v.s.size() == 12) { Vec3 x; std::memcpy(&x, v.s.data(), 12); call.fields.emplace_back(name, Var::vec3(x)); }
            return;
        case Value::Kind::Map: return;
        case Value::Kind::ObjRef:
        case Value::Kind::Inline: {
            const Object* o = obj_of(v);
            if (!o) return;
            if (sc.is_a(o->type, data::name_hash("STUConfigVar"))) {
                Var out; std::string err;
                if (c.g.evaluator().eval(v, out, err)) call.fields.emplace_back(name, std::move(out));
                else call.fields.emplace_back(name, Var::str("<unevaluable " + sc.class_name(o->type) + ": " + err + ">"));
                return;
            }
            if (depth >= 2) return;
            for (const auto& f : o->fields) {
                if (header_hashes().count(f.first) || is_plug_val(f.second)) continue;
                flatten(c, call, f.second, name + "." + member_name(f.first), depth + 1);
            }
            return;
        }
        case Value::Kind::List: {
            if (v.list.empty()) return;
            std::vector<Var> items;
            bool primitive = true;
            for (std::size_t i = 0; i < v.list.size(); ++i) {
                const Value& e = v.list[i];
                const Object* o = obj_of(e);
                if (!o) {
                    HostCall tmp;
                    flatten(c, tmp, e, "x", depth + 1);
                    if (!tmp.fields.empty()) items.push_back(tmp.fields[0].second);
                    continue;
                }
                primitive = false;
                if (sc.is_a(o->type, data::name_hash("STUConfigVar"))) {
                    HostCall tmp;
                    flatten(c, tmp, e, "x", depth + 1);
                    if (!tmp.fields.empty()) items.push_back(tmp.fields[0].second);
                } else if (const Value* def = o->get("m_def")) {   // {def, value|out_Var} pairs
                    HostCall tmp;
                    if (const Value* val = o->get("m_value")) flatten(c, tmp, *val, "v", depth + 1);
                    items.push_back(Var::array({Var::resource(def->u), tmp.fields.empty() ? Var{} : tmp.fields[0].second}));
                } else {
                    flatten(c, call, e, name + "[" + std::to_string(i) + "]", depth + 1);
                }
            }
            (void)primitive;
            if (!items.empty()) call.fields.emplace_back(name, Var::array(std::move(items)));
            return;
        }
    }
}

// ---- structural fallback -----------------------------------------------------------------------------------------
using HostFn = void (Host::*)(const HostCall&);

HostFn host_fn_for(const std::string& cls) {
    auto has = [&](const char* s) { return cls.find(s) != std::string::npos; };
    if (has("Aura")) return &Host::apply_aura;
    if (has("DestroyEntity")) return &Host::destroy_entity;
    if (has("ModifyHealth") || has("HealthPool") || has("Invincible")) return &Host::modify_health;
    if (has("Anim")) return &Host::play_animation;
    if (has("Effect") && !has("CosmeticEntity")) return &Host::apply_effect;
    if (has("Beam")) return &Host::apply_effect;
    if (has("Projectile") || has("Weapon")) return &Host::spawn_projectile;
    if (has("Ability")) return &Host::ability_event;
    if (has("Criteria")) return &Host::criteria_event;
    if (has("SendGameMessage")) return &Host::send_game_message;
    return &Host::presentation;
}

struct ForwardB final : Behavior {
    std::string name;
    bool state = false;
    HostFn fn = nullptr;
    std::string_view cls() const override { return name; }
    Support support() const override { return Support::HostForwarded; }
    void call(NodeCtx& c, HostCall::Phase ph, const char* detail) const {
        HostCall hc;
        hc.detail = detail;
        collect_fields(c, hc);
        pick_primary_resource(hc);
        c.host_call(ph, fn, std::move(hc));
    }
    void on_begin(NodeCtx& c) const override { if (state) call(c, HostCall::Phase::Begin, "begin"); }
    void on_finish(NodeCtx& c) const override { if (state) { HostCall hc; hc.detail = "end(finish)"; c.host_call(HostCall::Phase::End, fn, std::move(hc)); } }
    void on_abort(NodeCtx& c) const override { if (state) { HostCall hc; hc.detail = "end(abort)"; c.host_call(HostCall::Phase::End, fn, std::move(hc)); } }
    void on_finished(NodeCtx& c) const override { c.fire("m_onFinishedPlug"); }
    void on_aborted(NodeCtx& c) const override { c.fire("m_onAbortPlug"); }
    bool execute(NodeCtx& c) const override {
        call(c, HostCall::Phase::Once, "action");
        c.fire("m_outPlug");   // generic action F47740 fires the +0x80 plug
        return true;
    }
};

std::unique_ptr<Behavior> make_fallback(const GraphNode& n) {
    const data::Schema& sc = data::Schema::beta();
    const bool state = sc.is_a(n.type_hash, data::name_hash("STUStatescriptState"));
    const bool action = sc.is_a(n.type_hash, data::name_hash("STUStatescriptAction"));
    if (!state && !action) return nullptr;
    static const std::set<std::string> base_state = {"m_transitionPlug", "m_beginPlug", "m_abortPlug", "m_onBeginPlug", "m_onEndPlug", "m_subgraphPlug"};
    static const std::set<std::string> allowed_state = {"m_onFinishedPlug", "m_onAbortPlug"};
    static const std::set<std::string> base_action = {"m_inPlug", "m_outPlug"};
    std::vector<const data::SchemaField*> fields;
    sc.fields_of(n.type_hash, fields);
    const std::uint32_t plug_base = data::name_hash("STUGraphPlug");
    for (const auto* f : fields) {
        if (f->kind != static_cast<std::uint32_t>(data::FieldKind::ObjectPtr) && f->kind != static_cast<std::uint32_t>(data::FieldKind::ObjectPtrList)) continue;
        if (!sc.is_a(f->ref, plug_base)) continue;
        const std::string name = sc.field_name(f->hash);
        if (state) { if (base_state.count(name) || allowed_state.count(name)) continue; }
        else if (base_action.count(name)) continue;
        return nullptr;   // a flow-bearing plug this layer does not know: refuse rather than guess
    }
    auto b = std::make_unique<ForwardB>();
    b->name = n.cls;
    b->state = state;
    b->fn = host_fn_for(n.cls);
    return b;
}

}  // namespace

void collect_fields(NodeCtx& c, HostCall& call) {
    for (const auto& f : c.node.obj->fields) {
        if (header_hashes().count(f.first) || is_plug_val(f.second)) continue;
        if (f.second.kind == Value::Kind::List && !f.second.list.empty() && is_plug_val(f.second.list[0])) continue;
        flatten(c, call, f.second, member_name(f.first), 0);
    }
}

void pick_primary_resource(HostCall& call) {
    static const char* prefer[] = {"effect", "gameMessage", "aura", "anim", "identifier", "entityDef", "hardPoint", "animAlias"};
    for (const char* p : prefer)
        if (const Var* v = call.field(p)) { if (v->kind == Var::Kind::Id && v->id) { call.resource = v->id; return; } }
    for (const auto& f : call.fields)
        if (f.second.kind == Var::Kind::Id && f.second.id) { call.resource = f.second.id; return; }
}

void for_each_pair(const GraphNode& node, const char* list_member, const char* cfg_member, const PairFn& fn) {
    const Value* lst = node.field(list_member);
    if (!lst) return;
    for (const Value& e : lst->list) {
        const Object* o = obj_of(e);
        if (!o) continue;
        const Value* def = o->get("m_def");
        const Value* cfg = o->get(cfg_member);
        fn(def ? def->u : 0, cfg);
    }
}

void register_host_nodes(BehaviorRegistry& r) { r.set_fallback(make_fallback); }

}  // namespace ow::statescript::vm
