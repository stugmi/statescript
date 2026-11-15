// statescript/vm/graph.h -- executable view of a decoded STUStatescriptGraph (data/stud_image.h object tree).
//
// Resolves, once per graph: node table (m_nodes order == m_53BFB779), state table (m_states order == m_63D0B555),
// entries, every output plug's link targets (outPlug -> [n].inPlug), and per-state lifecycle definitions
// (runtime::StateDefinition) so the already-recovered runtime/lifecycle.cpp can pump them.
// See docs/statescript/graph-format.md for the object model.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "resource/casc/store.h"
#include "statescript/data/stud_image.h"
#include "statescript/runtime/lifecycle.h"

namespace ow::statescript::vm {

// Field hash of an STU member name ("m_outPlug" etc.). Pass the name WITH the "m_" prefix.
std::uint32_t field_hash(const char* member_name);

struct GraphNode;

struct OutPlug {
    std::uint32_t field = 0;                 // member hash of the plug field
    std::string name;                        // "outPlug", "onGoingDownPlug", "subgraphPlug", "29909C7C" when unsolved
    std::uint32_t element = 0;               // index when the member is a list of plugs (transitionPlug[]), else 0
    const data::Object* plug = nullptr;      // STUGraphPlug object
    std::string plug_class;                  // STUStatescriptOutputPlug / ...Subgraph / ...Transition / STU_F1F0A36D
    std::vector<runtime::LinkTarget> links;  // resolved targets, authored order
    std::vector<std::int32_t> subgraph_states;   // OutputPlugSubgraph.m_EAECE4C0 (state indices), else empty
};

struct InPlug {
    std::uint32_t field = 0;
    std::string name;                        // "inPlug", "beginPlug", "abortPlug", ...
    std::string plug_class;
    bool abort = false;                      // STUStatescriptInputPlugAbort (the RTTI test of F48700)
    const GraphNode* owner = nullptr;
};

struct GraphNode {
    const data::Object* obj = nullptr;
    std::string cls;                         // Schema::class_name(type): "STUStatescriptStateWait" or "STU_B1032DAF"
    std::uint32_t type_hash = 0;
    std::uint32_t index = 0;                 // m_53BFB779
    std::int32_t remote_sync_index = -1;     // m_4B062861
    bool is_state = false, is_entry = false;
    bool client_only = false, server_only = false;
    runtime::AuthoredNode authored;          // projection consumed by runtime/execution
    std::vector<OutPlug> out_plugs;          // every plug-valued member, in object field order
    std::vector<InPlug> in_plugs;

    const OutPlug* out(std::uint32_t field, std::uint32_t element = 0) const;
    const OutPlug* out(const char* member_name, std::uint32_t element = 0) const { return out(field_hash(member_name), element); }
    const data::Value* field(const char* member_name) const { return obj->get(member_name); }
    const data::Value* field(std::uint32_t hash) const { return obj->get(hash); }
    // convenience readers (return the default when absent / wrong kind)
    bool get_bool(const char* member_name, bool def = false) const;
    std::int64_t get_int(const char* member_name, std::int64_t def = 0) const;
    std::uint64_t get_res(const char* member_name) const;
    std::int64_t get_enum(const char* member_name, std::int64_t def = 0) const;
};

struct GraphState {                          // one per m_states element
    const GraphNode* node = nullptr;
    std::uint32_t state_index = 0;
    std::vector<std::uint32_t> abort_on_begin;   // m_065D841C  (authored +0x98): states aborted when this begins
    std::vector<std::uint32_t> abort_on_end;     // subgraphPlug.m_EAECE4C0 (+0xF0 -> +0x28): states aborted when this ends
    std::vector<runtime::LinkTarget> begin_links, end_links, active_links;   // onBeginPlug (+0xE0), onEndPlug (+0xE8), subgraphPlug (+0xF0)
    runtime::StateDefinition def;            // views into the vectors above
};

struct SyncVarDecl { std::uint64_t var = 0; std::int64_t scope = 0; std::uint32_t flag = 0; };

class Graph {
public:
    static bool build(const data::StudImage& image, std::uint64_t resource_id, Graph& out, std::string& error);
    std::uint64_t resource_id = 0;
    std::vector<std::unique_ptr<GraphNode>> nodes;   // stable addresses
    std::vector<std::unique_ptr<GraphState>> states;
    std::vector<std::uint32_t> entries;              // node indices
    std::vector<SyncVarDecl> sync_vars;
    const GraphNode* node(std::uint32_t i) const { return i < nodes.size() ? nodes[i].get() : nullptr; }
    const GraphState* state(std::uint32_t i) const { return i < states.size() ? states[i].get() : nullptr; }
    // The input plug object -> its descriptor (used to classify the plug a link arrives on)
    const InPlug* in_plug(const void* plug_object) const;
private:
    std::map<const void*, InPlug*> in_plug_index_;
};

// All graphs of the install (type field 0x580), decoded once.
class GraphLibrary {
public:
    bool load(const casc::Store& store, std::string& error);
    std::vector<std::uint64_t> ids() const;
    const Graph* find(std::uint64_t rid) const;
    std::size_t total_nodes() const;
    // Resolve a graph referenced by a STUConfigVarStatescriptGraph resource id; nullptr when unknown.
    const Graph* graph(std::uint64_t rid) const { return find(rid); }
private:
    std::map<std::uint64_t, std::unique_ptr<data::StudImage>> images_;
    std::map<std::uint64_t, std::unique_ptr<Graph>> graphs_;
};

}  // namespace ow::statescript::vm
