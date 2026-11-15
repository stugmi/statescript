#include "statescript/vm/graph.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "statescript/data/hero_data.h"

namespace ow::statescript::vm {
using data::Object;
using data::Value;

std::uint32_t field_hash(const char* member_name) {
    // unsolved members are spelled m_XXXXXXXX with XXXXXXXX the field hash itself (same convention as Object::get(const char*))
    if (member_name[0] == 'm' && member_name[1] == '_' && std::strlen(member_name) == 10) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(member_name + 2, &end, 16);
        if (end == member_name + 10) return static_cast<std::uint32_t>(v);
    }
    return data::name_hash(member_name);
}

namespace {
std::int64_t as_int(const Value& v, std::int64_t def = 0) {
    switch (v.kind) {
        case Value::Kind::SInt:
        case Value::Kind::Enum: return v.i;
        case Value::Kind::UInt: return static_cast<std::int64_t>(v.u);
        default: return def;
    }
}
bool is_plug_value(const Value& v) {
    return v.kind == Value::Kind::ObjRef && v.ref && v.ref->get("m_links") && v.ref->get("m_parentNode");
}
std::string hex8(std::uint32_t h) { char b[12]; std::snprintf(b, sizeof b, "%08X", h); return b; }
}  // namespace

const OutPlug* GraphNode::out(std::uint32_t field, std::uint32_t element) const {
    for (const OutPlug& p : out_plugs)
        if (p.field == field && p.element == element) return &p;
    return nullptr;
}
bool GraphNode::get_bool(const char* n, bool def) const { const Value* v = field(n); return v ? as_int(*v, def) != 0 : def; }
std::int64_t GraphNode::get_int(const char* n, std::int64_t def) const { const Value* v = field(n); return v ? as_int(*v, def) : def; }
std::int64_t GraphNode::get_enum(const char* n, std::int64_t def) const { const Value* v = field(n); return v && v->kind == Value::Kind::Enum ? v->i : def; }
std::uint64_t GraphNode::get_res(const char* n) const { const Value* v = field(n); return v && v->kind == Value::Kind::ResRef ? v->u : 0; }

const InPlug* Graph::in_plug(const void* plug_object) const {
    auto it = in_plug_index_.find(plug_object);
    return it == in_plug_index_.end() ? nullptr : it->second;
}

bool Graph::build(const data::StudImage& image, std::uint64_t rid, Graph& out, std::string& error) {
    error.clear();
    out = Graph{};
    out.resource_id = rid;
    const data::Schema& sc = image.schema();
    const Object* root = image.root();
    if (!root) { error = "image has no root"; return false; }
    const Value* nodes = root->get("m_nodes");
    if (!nodes || nodes->kind != Value::Kind::List) { error = "graph has no m_nodes"; return false; }
    const std::uint32_t input_plug_type = data::name_hash("STUStatescriptInputPlug");

    std::map<const Object*, std::uint32_t> node_of_object;
    std::map<const Object*, InPlug*> in_of;
    std::vector<std::vector<std::pair<const Object*, std::size_t>>> pending;   // per node: (plug object, out_plugs index)

    // pass 1: nodes, plugs
    for (std::size_t pos = 0; pos < nodes->list.size(); ++pos) {
        const Value& nv = nodes->list[pos];
        if (nv.kind != Value::Kind::ObjRef || !nv.ref) { error = "null node at " + std::to_string(pos); return false; }
        auto n = std::make_unique<GraphNode>();
        n->obj = nv.ref;
        n->type_hash = nv.ref->type;
        n->cls = sc.class_name(nv.ref->type);
        const Value* idx = nv.ref->get("m_53BFB779");
        n->index = idx ? static_cast<std::uint32_t>(as_int(*idx)) : static_cast<std::uint32_t>(pos);
        if (n->index != pos) { error = "node index mismatch at " + std::to_string(pos); return false; }
        if (const Value* v = nv.ref->get("m_4B062861")) n->remote_sync_index = static_cast<std::int32_t>(as_int(*v, -1));
        n->is_state = n->get_bool("m_isState");
        n->client_only = n->get_bool("m_clientOnly");
        n->server_only = n->get_bool("m_serverOnly");
        n->authored.stu_object = nv.ref;
        n->authored.node_index = n->index;
        n->authored.is_state = n->is_state;
        n->authored.scoped = n->client_only;
        n->authored.state_index = n->is_state ? static_cast<std::uint32_t>(n->get_int("m_63D0B555")) : 0;
        n->authored.execution_tag = static_cast<std::uint32_t>(n->get_enum("m_clientScope"));
        n->authored.activity_mask = static_cast<std::uint32_t>(n->get_int("m_36391723"));
        n->authored.unique_id_low = static_cast<std::uint32_t>(n->get_int("m_uniqueID"));
        node_of_object[nv.ref] = n->index;

        std::vector<std::pair<const Object*, std::size_t>> pend;
        auto add_plug = [&](std::uint32_t fhash, std::uint32_t element, bool list, const Value& pv) {
            const Object* po = pv.ref;
            const std::string base = sc.field_name(fhash);
            const std::string name = base.size() > 2 && base[0] == 'm' && base[1] == '_' ? base.substr(2) : base;
            const std::string cls = sc.class_name(po->type);
            (void)list;
            if (sc.is_a(po->type, input_plug_type)) {
                InPlug ip;
                ip.field = fhash; ip.name = list ? name + "[" + std::to_string(element) + "]" : name;
                ip.plug_class = cls; ip.abort = cls == "STUStatescriptInputPlugAbort";
                ip.owner = n.get();
                n->in_plugs.push_back(ip);
            } else {
                OutPlug op;
                op.field = fhash; op.element = element; op.name = name; op.plug = po; op.plug_class = cls;
                if (const Value* ids = po->get("m_EAECE4C0"))
                    for (const Value& e : ids->list) op.subgraph_states.push_back(static_cast<std::int32_t>(as_int(e)));
                n->out_plugs.push_back(std::move(op));
                pend.emplace_back(po, n->out_plugs.size() - 1);
            }
        };
        for (const auto& f : nv.ref->fields) {
            if (is_plug_value(f.second)) add_plug(f.first, 0, false, f.second);
            else if (f.second.kind == Value::Kind::List)
                for (std::size_t i = 0; i < f.second.list.size(); ++i)
                    if (is_plug_value(f.second.list[i])) add_plug(f.first, static_cast<std::uint32_t>(i), true, f.second.list[i]);
        }
        pending.push_back(std::move(pend));
        out.nodes.push_back(std::move(n));
    }
    // input plug index (stable addresses: in_plugs vectors are final now)
    for (auto& up : out.nodes) {
        std::size_t k = 0;
        for (const auto& f : up->obj->fields) {
            auto reg = [&](const Value& pv) {
                if (!is_plug_value(pv) || !sc.is_a(pv.ref->type, input_plug_type)) return;
                InPlug* ip = &up->in_plugs.at(k++);
                out.in_plug_index_[pv.ref] = ip;
            };
            if (is_plug_value(f.second)) reg(f.second);
            else if (f.second.kind == Value::Kind::List)
                for (const Value& e : f.second.list) reg(e);
        }
    }
    // pass 2: links
    for (std::size_t ni = 0; ni < out.nodes.size(); ++ni) {
        GraphNode& n = *out.nodes[ni];
        for (const auto& pp : pending[ni]) {
            OutPlug& op = n.out_plugs[pp.second];
            const Value* links = pp.first->get("m_links");
            if (!links) continue;
            for (const Value& lk : links->list) {
                const Object* link = lk.ref;
                const Value* ip = link ? link->get("m_inputPlug") : nullptr;
                if (!ip || !ip->ref) { error = "link without input plug on node " + std::to_string(n.index) + "." + op.name; return false; }
                const InPlug* in = out.in_plug(ip->ref);
                if (!in) { error = "unresolved input plug on node " + std::to_string(n.index) + "." + op.name; return false; }
                op.links.push_back(runtime::LinkTarget{&in->owner->authored, in});
            }
        }
    }
    // entries
    if (const Value* ent = root->get("m_entries"))
        for (const Value& e : ent->list) {
            auto it = e.ref ? node_of_object.find(e.ref) : node_of_object.end();
            if (it == node_of_object.end()) { error = "entry not in m_nodes"; return false; }
            out.entries.push_back(it->second);
            out.nodes[it->second]->is_entry = true;
        }
    if (const Value* sv = root->get("m_syncVars"))
        for (const Value& e : sv->list) {
            const Object* o = e.kind == Value::Kind::Inline ? e.inl.get() : e.ref;
            if (!o) continue;
            SyncVarDecl d;
            if (const Value* v = o->get("m_4F6799B5")) d.var = v->u;
            if (const Value* v = o->get("m_9E2A316C")) d.scope = as_int(*v);
            if (const Value* v = o->get("m_98C7778B")) d.flag = static_cast<std::uint32_t>(as_int(*v));
            out.sync_vars.push_back(d);
        }
    // states
    const Value* sts = root->get("m_states");
    if (sts)
        for (std::size_t si = 0; si < sts->list.size(); ++si) {
            auto it = sts->list[si].ref ? node_of_object.find(sts->list[si].ref) : node_of_object.end();
            if (it == node_of_object.end()) { error = "state not in m_nodes"; return false; }
            auto gs = std::make_unique<GraphState>();
            gs->node = out.nodes[it->second].get();
            gs->state_index = static_cast<std::uint32_t>(si);
            if (gs->node->authored.state_index != si) { error = "state index mismatch at " + std::to_string(si); return false; }
            if (const Value* ab = gs->node->field("m_065D841C"))
                for (const Value& e : ab->list) gs->abort_on_begin.push_back(static_cast<std::uint32_t>(as_int(e)));
            if (const OutPlug* p = gs->node->out("m_subgraphPlug")) {
                for (std::int32_t s : p->subgraph_states) if (s >= 0) gs->abort_on_end.push_back(static_cast<std::uint32_t>(s));
                gs->active_links = p->links;
            }
            if (const OutPlug* p = gs->node->out("m_onBeginPlug")) gs->begin_links = p->links;
            if (const OutPlug* p = gs->node->out("m_onEndPlug")) gs->end_links = p->links;
            gs->def.node = gs->node->authored;
            gs->def.abort_on_begin = {gs->abort_on_begin.data(), gs->abort_on_begin.size()};
            gs->def.abort_on_end = {gs->abort_on_end.data(), gs->abort_on_end.size()};
            gs->def.begin_plug = {gs->begin_links.data(), gs->begin_links.size()};
            gs->def.end_plug = {gs->end_links.data(), gs->end_links.size()};
            gs->def.active_plug = {gs->active_links.data(), gs->active_links.size()};
            out.states.push_back(std::move(gs));
        }
    (void)hex8;
    return true;
}

namespace {
std::uint32_t reverse12(std::uint32_t v) { std::uint32_t r = 0; for (int i = 0; i < 12; ++i) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
}

bool GraphLibrary::load(const casc::Store& store, std::string& error) {
    error.clear();
    const data::Schema& sc = data::Schema::beta();
    const auto ids_found = store.list_resources_of_type(static_cast<std::uint16_t>(reverse12(data::kTypeGraph) + 1));
    for (std::uint64_t rid : ids_found) {
        std::vector<std::uint8_t> bytes;
        std::string err;
        if (!store.read_resource(rid, bytes, err)) { error = "read graph: " + err; return false; }
        auto img = std::make_unique<data::StudImage>();
        if (!data::StudImage::parse(std::move(bytes), sc, *img, err)) { error = "decode graph: " + err; return false; }
        auto g = std::make_unique<Graph>();
        if (!Graph::build(*img, rid, *g, err)) { char b[20]; std::snprintf(b, sizeof b, "%016llX", (unsigned long long)rid); error = std::string("build graph ") + b + ": " + err; return false; }
        images_[rid] = std::move(img);
        graphs_[rid] = std::move(g);
    }
    return true;
}

std::vector<std::uint64_t> GraphLibrary::ids() const {
    std::vector<std::uint64_t> r;
    for (const auto& kv : graphs_) r.push_back(kv.first);
    return r;
}
const Graph* GraphLibrary::find(std::uint64_t rid) const {
    auto it = graphs_.find(rid);
    return it == graphs_.end() ? nullptr : it->second.get();
}
std::size_t GraphLibrary::total_nodes() const {
    std::size_t n = 0;
    for (const auto& kv : graphs_) n += kv.second->nodes.size();
    return n;
}

}  // namespace ow::statescript::vm
