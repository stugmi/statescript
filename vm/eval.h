// statescript/vm/eval.h -- ConfigVar and expression evaluation for the Statescript interpreter.
//
// A node parameter is a decoded STUConfigVar* object (graph-format.md "Config vars / values"): a literal
// (STUConfigVarInt/Float/Bool/String/Vec3/Identifier/...), a variable reference (STUConfigVarDynamic.m_4F6799B5 = VarPath),
// a getter (GetOwner, GetTeam, GetEntityHealth, ...) or an expression (STU_EC502A81 / STUConfigVarExpression:
// STUConfigVarExpressionData { m_fragments, m_opcodes (op,arg pairs), m_dynamicVars, m_1B183605 floats } + m_configVars[]).
// Implementation: eval.cpp. Opcode semantics / evidence: docs/statescript/vm.md section "Expressions".
#pragma once
#include <functional>
#include <map>
#include <string>

#include "statescript/data/stud_image.h"
#include "statescript/vm/value.h"

namespace ow::statescript::vm {

class GraphInstance;
struct GraphNode;

class Evaluator {
public:
    explicit Evaluator(GraphInstance& instance) : g_(instance) {}
    // Evaluate a config-var Value (field value of a node: ObjRef or Inline object, or Null).
    // Null -> out = Unset and returns true (an absent optional parameter); unknown class / failure -> false + error.
    bool eval(const data::Value& cfg, Var& out, std::string& error);
    // Evaluate member `member` ("m_timeout") of `node`.
    bool eval_field(const GraphNode& node, const char* member, Var& out, std::string& error);
    // STUConfigVarDynamic -> its variable path (m_4F6799B5). False when `cfg` is not a Dynamic config var.
    bool resolve_path(const data::Value& cfg, VarPath& out, std::string& error);
    // Write `v` through an STUConfigVarDynamic (out_Var of SetVar / Stack / ChaseVar / ...). Returns false on failure.
    bool assign(const data::Value& cfg, const Var& v, std::string& error);
    GraphInstance& instance() { return g_; }
private:
    GraphInstance& g_;
};

// Pluggable getters for non-literal config-var classes that need world access or recursion. `obj` is the decoded
// STUConfigVar* object; use Evaluator::eval on its fields for children.
using ConfigGetter = std::function<bool(Evaluator&, const data::Object& obj, Var& out, std::string& error)>;
void register_config_getter(const std::string& schema_class, ConfigGetter getter);
const ConfigGetter* find_config_getter(const std::string& schema_class);

// Classes eval.cpp can handle (literals, Dynamic, expression, built-in getters): for the coverage report.
bool config_class_supported(const std::string& schema_class);

}  // namespace ow::statescript::vm
