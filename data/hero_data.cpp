#include "statescript/data/hero_data.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace ow::statescript::data {
namespace {
#include "statescript/data/logical_button_names.inc"

[[noreturn]] void fail(const std::string& m) { throw std::runtime_error(m); }

std::uint32_t reverse12(std::uint32_t v) { std::uint32_t r = 0; for (int i = 0; i < 12; ++i) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
// Store::list_resources_of_type takes the handler index: field = reverse12(index - 1) (casc/store.cpp:89).
std::vector<std::uint64_t> by_type_field(const casc::Store& s, std::uint32_t field) { return s.list_resources_of_type(static_cast<std::uint16_t>(reverse12(field) + 1)); }

std::string hex16(std::uint64_t v) { char b[20]; std::snprintf(b, sizeof b, "%016llX", (unsigned long long)v); return b; }

// Field accessors (all by name hash, resolved with the beta registry layouts).
const Object& need_obj(const Value* v, const char* what) { if (!v || v->kind != Value::Kind::ObjRef || !v->ref) fail(std::string("missing object ") + what); return *v->ref; }
const Value& need(const Object& o, const char* name) { const Value* v = o.get(name); if (!v) fail(std::string("missing field ") + name); return *v; }
std::uint64_t res_of(const Value& v) { if (v.kind != Value::Kind::ResRef) fail("expected resource reference"); return v.u; }
std::int64_t int_of(const Value& v) {
    if (v.kind == Value::Kind::SInt || v.kind == Value::Kind::Enum) return v.i;
    if (v.kind == Value::Kind::UInt) return static_cast<std::int64_t>(v.u);
    fail("expected integer");
}

constexpr std::uint32_t kComponentStatescript = 0x9B38211B, kComponentWeapon = 0x378F54CD;

struct Ctx {
    const casc::Store& store;
    const Schema& sc;
    explicit Ctx(const casc::Store& s) : store(s), sc(Schema::beta()) {}
    std::map<std::uint64_t, std::unique_ptr<StudImage>> images;

    StudImage& image(std::uint64_t rid) {
        auto it = images.find(rid);
        if (it != images.end()) return *it->second;
        std::vector<std::uint8_t> bytes; std::string err;
        if (!store.read_resource(rid, bytes, err)) fail("read " + hex16(rid) + ": " + err);
        auto img = std::make_unique<StudImage>();
        if (!StudImage::parse(std::move(bytes), sc, *img, err)) fail("decode " + hex16(rid) + ": " + err);
        return *(images[rid] = std::move(img));
    }
    // DisplayText-like non-STU resource: 16-byte header then NUL terminated UTF-8 [INFERENCE: header meaning]
    std::string text(std::uint64_t rid) {
        if (!rid) return {};
        std::vector<std::uint8_t> bytes; std::string err;
        if (!store.read_resource(rid, bytes, err)) fail("read " + hex16(rid) + ": " + err);
        if (bytes.size() > 16 && std::memcmp(bytes.data(), "DUTS", 4) != 0) {
            std::size_t end = 16; while (end < bytes.size() && bytes[end]) ++end;
            return std::string(reinterpret_cast<const char*>(bytes.data() + 16), end - 16);
        }
        return {};
    }
    // component objects of an STUEntityDefinition: key (component type hash) -> object
    std::vector<std::pair<std::uint32_t, const Object*>> components(StudImage& img) {
        std::vector<std::pair<std::uint32_t, const Object*>> out;
        const Value& map = need(*img.root(), "m_componentMap");
        for (const Value& e : map.list) {
            std::string err;
            const Object* o = img.object_at(static_cast<std::uint32_t>(e.i), err);
            if (!o) fail("component decode: " + err);
            out.emplace_back(static_cast<std::uint32_t>(e.u), o);
        }
        return out;
    }
};

void walk(const Value& v, std::set<const Object*>& seen, const std::function<void(const Value&)>& cb) {
    cb(v);
    switch (v.kind) {
        case Value::Kind::List: for (const Value& x : v.list) walk(x, seen, cb); break;
        case Value::Kind::Inline: for (const auto& f : v.inl->fields) walk(f.second, seen, cb); break;
        case Value::Kind::ObjRef:
            if (v.ref && seen.insert(v.ref).second) for (const auto& f : v.ref->fields) walk(f.second, seen, cb);
            break;
        default: break;
    }
}
void walk_object(const Object& o, std::set<const Object*>& seen, const std::function<void(const Value&)>& cb) {
    if (!seen.insert(&o).second) return;
    for (const auto& f : o.fields) walk(f.second, seen, cb);
}

// Buttons: classes holding an STUConfigVarLogicalButton, and the field name (beta names; see ability-bindings.md)
const char* button_field(const std::string& cls) {
    if (cls == "STUStatescriptStateLogicalButton") return "m_logicalButton";
    if (cls == "STUStatescriptStateAbility") return "m_startButton";
    if (cls == "STU_D689C470") return "m_logicalButton";
    if (cls == "STU_4768E4A3") return "m_logicalButton";
    if (cls == "STU_D3F788BE") return "m_button";
    return nullptr;
}
}  // namespace

const char* logical_button_name(std::int32_t value) {
    if (value < 0 || value >= static_cast<std::int32_t>(sizeof kLogicalButtonNames / sizeof *kLogicalButtonNames)) return nullptr;
    return kLogicalButtonNames[value];
}

std::size_t Catalog::abilities() const {
    std::size_t n = 0;
    for (const Hero& h : heroes) for (const LoadoutEntry& l : h.loadout) if (l.category != 0) ++n;
    return n;
}

bool Catalog::load(const casc::Store& store, Catalog& out, std::string& error) {
    error.clear();
    try {
        out = Catalog();
        out.store_ = &store;
        Ctx cx(store);
        const Schema& sc = cx.sc;

        // ---- graphs ------------------------------------------------------------------------------------------------
        std::map<std::uint64_t, std::vector<ButtonBinding>> graph_buttons;
        for (std::uint64_t gid : by_type_field(store, kTypeGraph)) {
            StudImage& img = cx.image(gid);
            const Object& r = *img.root();
            if (r.type != name_hash("STUStatescriptGraph")) fail("type field 0x580 resource " + hex16(gid) + " is not STUStatescriptGraph");
            GraphInfo gi; gi.resource = gid;
            const Value& nodes = need(r, "m_nodes");
            gi.nodes = nodes.list.size();
            gi.states = need(r, "m_states").list.size();
            gi.entries = need(r, "m_entries").list.size();
            gi.sync_vars = need(r, "m_syncVars").list.size();
            gi.remote_sync_nodes = need(r, "m_remoteSyncNodes").list.size();
            for (const Value& nv : nodes.list) {
                const Object& n = need_obj(&nv, "node");
                std::string cls = sc.class_name(n.type);
                ++gi.node_classes[cls];
                if (const char* fld = button_field(cls)) {
                    const Value* b = n.get(fld);
                    if (b && b->kind == Value::Kind::ObjRef && b->ref && sc.class_name(b->ref->type) == "STUConfigVarLogicalButton") {
                        ButtonBinding bb;
                        bb.button_enum = static_cast<int>(int_of(need(*b->ref, "m_logicalButton")));
                        bb.graph = gid; bb.node_index = static_cast<int>(int_of(need(n, "m_53BFB779"))); bb.node_class = cls;
                        graph_buttons[gid].push_back(bb);
                    }
                }
            }
            for (const auto& o : img.objects()) if (sc.class_name(o.second->type) == "STUGraphLink") ++gi.links;
            std::set<const Object*> seen;
            walk_object(r, seen, [&](const Value& v) {
                if (v.kind == Value::Kind::ResRef && type_field(v.u) == kTypeGraph && v.u != gid) gi.subgraph_refs.insert(v.u);
            });
            for (const auto& c : gi.node_classes) out.node_class_histogram[c.first] += c.second;
            out.graphs[gid] = std::move(gi);
            cx.images.erase(gid);   // graph images are large; nothing keeps pointers into them
        }

        auto closure = [&](const std::vector<std::uint64_t>& seeds) {
            std::set<std::uint64_t> seen; std::vector<std::uint64_t> st(seeds.begin(), seeds.end());
            while (!st.empty()) {
                std::uint64_t g = st.back(); st.pop_back();
                auto it = out.graphs.find(g);
                if (it == out.graphs.end() || !seen.insert(g).second) continue;
                for (std::uint64_t s : it->second.subgraph_refs) st.push_back(s);
            }
            return seen;
        };
        auto entity_graphs = [&](std::uint64_t entity) {
            std::vector<std::uint64_t> g; std::set<const Object*> seen;
            StudImage& img = cx.image(entity);
            for (const auto& c : cx.components(img)) {
                walk_object(*c.second, seen, [&](const Value& v) {
                    if (v.kind == Value::Kind::ResRef && type_field(v.u) == kTypeGraph && std::find(g.begin(), g.end(), v.u) == g.end()) g.push_back(v.u);
                });
            }
            std::sort(g.begin(), g.end());
            return g;
        };

        // ---- input buttons ---------------------------------------------------------------------------------------------
        for (std::uint64_t rid : by_type_field(store, kTypeInputButton)) {
            const Object& o = *cx.image(rid).root();
            std::string name = cx.text(res_of(need(o, "m_name")));
            if (!name.empty()) out.input_buttons[rid] = name;
        }

        // ---- heroes --------------------------------------------------------------------------------------------------
        for (std::uint64_t hid : by_type_field(store, kTypeHero)) {
            const Object& ho = *cx.image(hid).root();
            if (ho.type != name_hash("STUHero")) fail("0x2E0 resource " + hex16(hid) + " is not STUHero");
            Hero h; h.resource = hid;
            h.name = cx.text(res_of(need(ho, "m_externalName")));
            h.presence_id = static_cast<int>(int_of(need(ho, "m_presenceID")));
            h.entity = res_of(need(ho, "m_gameplayEntity"));
            for (const Value& lv : need(ho, "m_heroLoadout").list) {
                const Object& lo = *cx.image(res_of(lv)).root();
                LoadoutEntry e; e.resource = res_of(lv);
                e.name = cx.text(res_of(need(lo, "m_name")));
                e.description = cx.text(res_of(need(lo, "m_description")));
                e.category = static_cast<int>(int_of(need(lo, "m_category")));
                e.button_resource = res_of(need(lo, "m_logicalButton"));
                e.loadout_group = static_cast<int>(int_of(need(lo, "m_loadoutGroup")));
                if (e.button_resource) {
                    e.button_enum = static_cast<int>(static_cast<std::uint32_t>(e.button_resource)) - 1;   // resource index == enum + 1
                    if (const char* n = logical_button_name(e.button_enum)) e.button = n;
                    e.button_display = cx.text(res_of(need(*cx.image(e.button_resource).root(), "m_name")));
                }
                h.loadout.push_back(std::move(e));
            }
            h.graphs_direct = entity_graphs(h.entity);
            auto cl = closure(h.graphs_direct);
            h.graphs_closure.assign(cl.begin(), cl.end());
            for (std::uint64_t g : h.graphs_closure) for (const ButtonBinding& b : graph_buttons[g]) h.bindings.push_back(b);
            for (const LoadoutEntry& e : h.loadout) {
                if (e.category != 3) continue;
                Hero::Ultimate u; u.name = e.name; u.button_enum = e.button_enum;
                for (const ButtonBinding& b : h.bindings) if (b.button_enum == e.button_enum) u.nodes.push_back(b);
                h.ultimates.push_back(std::move(u));
            }
            out.heroes.push_back(std::move(h));
        }
        for (std::uint64_t rid : by_type_field(store, kTypeHeroVariant)) out.hero_variants.push_back(rid);

        // ---- game modes: entities carrying STUPvPGameComponent with a game mode object ----------------------------------
        for (std::uint64_t eid : by_type_field(store, kTypeEntity)) {
            StudImage& img = cx.image(eid);
            GameModeEntity gm; bool is_gm = false;
            for (const auto& c : cx.components(img)) {
                if (sc.class_name(c.second->type) != "STUPvPGameComponent") continue;
                const Value* g = c.second->get("m_gamemode");
                if (g && g->kind == Value::Kind::ObjRef && g->ref) { is_gm = true; gm.gamemode_class = sc.class_name(g->ref->type); }
            }
            if (!is_gm) continue;
            gm.entity = eid;
            for (const auto& c : cx.components(img)) {
                if (c.first != kComponentStatescript) continue;
                for (const Value& g : need(*c.second, "m_initialGraphs").list) gm.graphs.push_back(res_of(need(*g.inl, "m_graph")));
            }
            out.gamemodes.push_back(std::move(gm));
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        out = Catalog();
        return false;
    }
}

// ------------------------------------------------------------------------------------------------------------------------
// graph text dump
// ------------------------------------------------------------------------------------------------------------------------
namespace {
bool is_plug(const Value& v) { return v.kind == Value::Kind::ObjRef && v.ref && v.ref->get("m_links") && v.ref->get("m_parentNode"); }

std::string summary(const Schema& sc, const Value& v, int depth) {
    switch (v.kind) {
        case Value::Kind::Null: return "None";
        case Value::Kind::UInt: return std::to_string(v.u);
        case Value::Kind::SInt: return std::to_string(v.i);
        case Value::Kind::Float: { char b[32]; std::snprintf(b, sizeof b, "%g", v.f); return b; }
        case Value::Kind::String: return "'" + v.s + "'";
        case Value::Kind::Enum: {
            if (v.type == kEnumLogicalButton) { const char* n = logical_button_name(static_cast<std::int32_t>(v.i)); if (n) return std::string(n) + "(" + std::to_string(v.i) + ")"; }
            return "enum(" + std::to_string(v.i) + ")";
        }
        case Value::Kind::ResRef: return "res:" + hex16(v.u);
        case Value::Kind::Raw: { std::string s = "raw:"; char b[4]; for (unsigned char c : v.s) { std::snprintf(b, sizeof b, "%02x", c); s += b; } return s; }
        case Value::Kind::Map: return "map[" + std::to_string(v.u) + "]";
        case Value::Kind::List: {
            std::string s = "[";
            for (std::size_t i = 0; i < v.list.size() && i < 8; ++i) s += (i ? ", " : "") + summary(sc, v.list[i], depth);
            if (v.list.size() > 8) s += ", ...";
            return s + "]";
        }
        case Value::Kind::ObjRef:
        case Value::Kind::Inline: {
            const Object* o = v.kind == Value::Kind::ObjRef ? v.ref : v.inl.get();
            if (!o) return "None";
            std::string s = sc.class_name(o->type);
            if (depth <= 0) return s;
            std::string inner;
            for (const auto& f : o->fields) {
                if (f.second.kind == Value::Kind::ObjRef && is_plug(f.second)) continue;
                if (f.first == 0x83E83924u) continue;   // hidden configVar implementation slot
                std::string x = summary(sc, f.second, depth - 1);
                if (x == "None" || x == "[]" || x == "0" || x == "''") continue;
                inner += (inner.empty() ? "" : " ") + sc.field_name(f.first).substr(2) + "=" + x;
            }
            return inner.empty() ? s : s + "(" + inner + ")";
        }
    }
    return "?";
}
}  // namespace

bool Catalog::dump_graph(std::uint64_t gid, std::string& out, std::string& error) const {
    error.clear(); out.clear();
    try {
        if (!store_) fail("catalog not loaded");
        Ctx cx(*store_);
        const Schema& sc = cx.sc;
        StudImage& img = cx.image(gid);
        const Object& r = *img.root();
        const Value& nodes = need(r, "m_nodes");
        std::map<const Object*, std::pair<int, std::string>> plug_owner;   // plug -> (node index, field name)
        std::vector<const Object*> node_ptr;
        for (const Value& nv : nodes.list) node_ptr.push_back(&need_obj(&nv, "node"));
        for (const Object* n : node_ptr) {
            int idx = static_cast<int>(int_of(need(*n, "m_53BFB779")));
            for (const auto& f : n->fields) {
                std::string fn = sc.field_name(f.first);
                if (is_plug(f.second)) plug_owner[f.second.ref] = {idx, fn};
                else if (f.second.kind == Value::Kind::List)
                    for (std::size_t i = 0; i < f.second.list.size(); ++i)
                        if (is_plug(f.second.list[i])) plug_owner[f.second.list[i].ref] = {idx, fn + "[" + std::to_string(i) + "]"};
            }
        }
        std::set<int> entries;
        for (const Value& e : need(r, "m_entries").list) entries.insert(static_cast<int>(int_of(need(need_obj(&e, "entry"), "m_53BFB779"))));
        char head[256];
        std::snprintf(head, sizeof head, "graph %s: nodes=%zu states=%zu entries=%zu syncVars=%zu remoteSyncNodes=%zu\n", hex16(gid).c_str(),
                      nodes.list.size(), need(r, "m_states").list.size(), entries.size(), need(r, "m_syncVars").list.size(), need(r, "m_remoteSyncNodes").list.size());
        out = head;
        for (const Object* n : node_ptr) {
            int idx = static_cast<int>(int_of(need(*n, "m_53BFB779")));
            bool client = int_of(need(*n, "m_clientOnly")) != 0, server = int_of(need(*n, "m_serverOnly")) != 0;
            out += "  [" + std::to_string(idx) + "] " + sc.class_name(n->type) + " uid=" + std::to_string(int_of(need(*n, "m_uniqueID"))) + " state=" +
                   std::to_string(int_of(need(*n, "m_4B062861"))) + " scope=" + (client ? "client" : server ? "server" : "both") + (entries.count(idx) ? " ENTRY" : "") + "\n";
            static const std::set<std::string> base = {"m_displayName", "m_comment", "m_pos", "m_uniqueID", "m_debugText", "m_F8F89F89", "m_A56DC772", "m_53BFB779",
                "m_4B062861", "m_clientScope", "m_isState", "m_clientOnly", "m_serverOnly", "m_stateGroup", "m_065D841C", "m_652A186C", "m_3882159E", "m_63D0B555",
                "m_36391723", "m_beginPlug", "m_abortPlug", "m_onBeginPlug", "m_onEndPlug", "m_subgraphPlug", "m_transitionPlug", "m_parentSubgraph"};
            auto emit_links = [&](const std::string& plug, const Value& pv) {
                for (const Value& lk : need(*pv.ref, "m_links").list) {
                    const Object* link = lk.ref;
                    const Value* ip = link ? link->get("m_inputPlug") : nullptr;
                    auto it = ip && ip->ref ? plug_owner.find(ip->ref) : plug_owner.end();
                    out += "      " + plug.substr(2) + " -> " + (it == plug_owner.end() ? std::string("?") : "[" + std::to_string(it->second.first) + "]." + it->second.second.substr(2)) + "\n";
                }
            };
            for (const auto& f : n->fields) {
                std::string fn = sc.field_name(f.first);
                if (is_plug(f.second)) { emit_links(fn, f.second); continue; }
                if (f.second.kind == Value::Kind::List && !f.second.list.empty() && is_plug(f.second.list[0])) {
                    for (const Value& p : f.second.list) emit_links(fn, p);
                    continue;
                }
                if (base.count(fn)) continue;
                std::string s = summary(sc, f.second, 3);
                if (s == "None" || s == "[]" || s == "0" || s == "''") continue;
                out += "      " + fn.substr(2) + " = " + s + "\n";
            }
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        out.clear();
        return false;
    }
}

}  // namespace ow::statescript::data
