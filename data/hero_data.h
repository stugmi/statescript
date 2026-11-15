// statescript/data/hero_data.h -- heroes, abilities, logical-button bindings and Statescript graphs read from the real
// beta 0.8 (build 24919) CASC archive.  Semantics and evidence: recovered/docs/statescript/{hero-data,graph-format,ability-bindings}.md
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "resource/casc/store.h"
#include "statescript/data/stud_image.h"

namespace ow::statescript::data {

// Resource type fields ((rid >> 48) & 0xFFF), verified by sampling every type field of the archive (see hero-data.md).
constexpr std::uint32_t kTypeHero = 0x2E0, kTypeHeroVariant = 0x050, kTypeEntity = 0x400, kTypeGraph = 0x580,
                        kTypeLoadout = 0xB90, kTypeInputButton = 0xA90;
constexpr std::uint32_t kEnumLogicalButton = 0xDC1624CF;   // STULogicalButton
constexpr std::int32_t kButtonAbility1 = 119, kButtonAbility2 = 120, kButtonAbility3 = 121;
inline std::uint32_t type_field(std::uint64_t rid) { return static_cast<std::uint32_t>((rid >> 48) & 0xFFF); }
const char* logical_button_name(std::int32_t value);   // nullptr when the enum value's name is unsolved or out of range

struct LoadoutEntry {                 // STULoadout (type 0xB90, hash of STULoadout in stu_types.h)
    std::uint64_t resource = 0;
    std::string name, description;    // DisplayText resources (type 0xDE0)
    int category = 0;                 // LoadoutCategory enum ef0df8c0: 0 Weapon, 1/2 unnamed, 3 ultimate_ability
    std::uint64_t button_resource = 0;   // STUInputLogicalButton resource (type 0xA90)
    int button_enum = -1;             // STULogicalButton value == (button resource index) - 1
    std::string button;               // enum name ("" when unsolved)
    std::string button_display;       // STUInputLogicalButton.m_name text ("ABILITY 3", "PRIMARY FIRE")
    int loadout_group = 0;
};
struct ButtonBinding {                // one graph node that reacts to a logical button
    int button_enum = -1;
    std::uint64_t graph = 0;
    int node_index = 0;               // STUStatescriptBase node index (m_53BFB779) inside the graph
    std::string node_class;
};
struct Hero {
    std::uint64_t resource = 0;       // STUHero (0x2E0)
    std::string name;                 // m_externalName text
    int presence_id = 0;
    std::uint64_t entity = 0;         // STUEntityDefinition (0x400) from m_gameplayEntity
    std::vector<LoadoutEntry> loadout;
    std::vector<std::uint64_t> graphs_direct;     // graphs named by the entity's components
    std::vector<std::uint64_t> graphs_closure;    // plus every graph reachable through graph->graph references
    std::vector<ButtonBinding> bindings;          // over graphs_closure
    // ultimate = loadout entries of category 3; resolved to the graph nodes bound to the same logical button
    struct Ultimate { std::string name; int button_enum; std::vector<ButtonBinding> nodes; };
    std::vector<Ultimate> ultimates;
};
struct GraphInfo {
    std::uint64_t resource = 0;
    std::size_t nodes = 0, states = 0, entries = 0, sync_vars = 0, remote_sync_nodes = 0;
    std::set<std::uint64_t> subgraph_refs;
    std::map<std::string, std::size_t> node_classes;
    std::size_t links = 0;            // STUGraphLink objects reachable from the nodes' plugs
};
struct GameModeEntity {
    std::uint64_t entity = 0;
    std::string gamemode_class;
    std::vector<std::uint64_t> graphs;
};

class Catalog {
public:
    static bool load(const casc::Store& store, Catalog& out, std::string& error);
    std::vector<Hero> heroes;
    std::vector<std::uint64_t> hero_variants;
    std::vector<GameModeEntity> gamemodes;
    std::map<std::uint64_t, GraphInfo> graphs;
    std::map<std::string, std::size_t> node_class_histogram;
    std::map<std::uint64_t, std::string> input_buttons;   // STUInputLogicalButton resource -> display name (named ones only)
    std::size_t abilities() const;    // loadout entries with category != Weapon, over all heroes

    // Text dump of one graph (nodes, node classes, scope, links); format in graph-format.md
    bool dump_graph(std::uint64_t graph, std::string& out, std::string& error) const;
private:
    const casc::Store* store_ = nullptr;
};

}  // namespace ow::statescript::data
