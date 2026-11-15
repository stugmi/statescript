#include "statescript/vm/eval.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "statescript/vm/vm.h"

namespace ow::statescript::vm {
using data::Object;
using data::Value;

namespace {
std::map<std::string, ConfigGetter>& getters() { static std::map<std::string, ConfigGetter> m; return m; }

const Object* obj_of(const Value& v) {
    if (v.kind == Value::Kind::ObjRef) return v.ref;
    if (v.kind == Value::Kind::Inline) return v.inl.get();
    return nullptr;
}
std::int64_t num_i(const Value& v) {
    switch (v.kind) {
        case Value::Kind::SInt: case Value::Kind::Enum: return v.i;
        case Value::Kind::UInt: return static_cast<std::int64_t>(v.u);
        case Value::Kind::Float: return static_cast<std::int64_t>(v.f);
        default: return 0;
    }
}
double num_f(const Value& v) { return v.kind == Value::Kind::Float ? v.f : static_cast<double>(num_i(v)); }

bool is_cfg(const data::Schema& sc, std::uint32_t type) { return sc.is_a(type, data::name_hash("STUConfigVar")); }
bool isa(const data::Schema& sc, std::uint32_t type, const char* base) { return sc.is_a(type, data::name_hash(base)); }

// Opcodes of the stack machine at RVA 0xF6C7B0 (`switch (*v13)`), see vm.md "Expressions".
bool op_has_arg(std::int64_t op) {
    return (op >= 1 && op <= 5) || op == 0x1E || op == 0x1F || op == 0x20 || op == 0x24 || op == 0x46;
}

struct ExprFailure { std::string text; };
}  // namespace

void register_config_getter(const std::string& schema_class, ConfigGetter getter) { getters()[schema_class] = std::move(getter); }
const ConfigGetter* find_config_getter(const std::string& schema_class) {
    auto it = getters().find(schema_class);
    return it == getters().end() ? nullptr : &it->second;
}

bool Evaluator::resolve_path(const Value& cfg, VarPath& out, std::string& error) {
    const Object* o = obj_of(cfg);
    if (!o || o->type != data::name_hash("STUConfigVarDynamic")) { error = "not a STUConfigVarDynamic"; return false; }
    out.clear();
    if (const Value* ids = o->get("m_4F6799B5"))
        for (const Value& e : ids->list) out.push_back(e.u);
    return true;
}

bool Evaluator::assign(const Value& cfg, const Var& v, std::string& error) {
    VarPath path;
    if (!resolve_path(cfg, path, error)) return false;
    if (path.empty()) { error = "STUConfigVarDynamic with an empty variable list"; return false; }
    if (g_.bag().set(path, v, g_.frame())) g_.note(TraceEvent::Kind::Var, nullptr, path_string(path) + " = " + v.to_string());
    return true;
}

bool Evaluator::eval_field(const GraphNode& node, const char* member, Var& out, std::string& error) {
    const Value* v = node.field(member);
    if (!v) { out = Var{}; return true; }
    return eval(*v, out, error);
}

bool Evaluator::eval(const Value& cfg, Var& out, std::string& error) {
    const data::Schema& sc = data::Schema::beta();
    const Object* o = obj_of(cfg);
    if (!o) { out = Var{}; return true; }
    const std::string cls = sc.class_name(o->type);

    if (const ConfigGetter* gt = find_config_getter(cls)) return (*gt)(*this, *o, out, error);

    if (cls == "STUConfigVarDynamic") {
        VarPath p;
        if (!resolve_path(cfg, p, error)) return false;
        out = g_.bag().get(p);
        return true;
    }
    if (cls == "STUConfigVarGetOwner") { out = Var::entity(g_.host().owner_entity()); return true; }

    // expressions: STUConfigVarExpression and its renamed subclass STU_EC502A81 share the layout
    if (const Value* ex = o->get("m_expression")) {
        const Object* data = obj_of(*ex);
        if (!data) { error = cls + ": expression without data"; return false; }
        const Value* ops = data->get("m_opcodes");
        const Value* dyn = data->get("m_dynamicVars");
        const Value* consts = data->get("m_1B183605");
        const Value* cvs = o->get("m_configVars");
        const std::size_t n_ops = ops ? ops->list.size() : 0;
        std::vector<Var> st;
        auto push = [&](Var v) { st.push_back(std::move(v)); };
        auto pop = [&](Var& v) -> bool { if (st.empty()) return false; v = std::move(st.back()); st.pop_back(); return true; };
        auto dyn_path = [&](std::int64_t k, VarPath& p) -> bool {
            if (!dyn || k < 0 || static_cast<std::size_t>(k) >= dyn->list.size()) return false;
            std::string e;
            return resolve_path(dyn->list[static_cast<std::size_t>(k)], p, e);
        };
        std::size_t pc = 0;
        std::size_t steps = 0;
        while (pc < n_ops) {
            if (++steps > 4096) { error = cls + ": expression step limit"; return false; }
            const std::int64_t op = num_i(ops->list[pc]);
            const std::int64_t arg = op_has_arg(op) && pc + 1 < n_ops ? num_i(ops->list[pc + 1]) : 0;
            Var a, b, c;
            if (op == 0) break;   // terminator (stream ends with an explicit 0 when the last op is an arg-op)
            switch (op) {
                case 1: pc += static_cast<std::size_t>(arg); continue;
                case 2: if (st.empty()) { error = cls + ": jump on empty stack"; return false; }
                        if (st.back().truthy()) { st.pop_back(); pc += 2; } else pc += static_cast<std::size_t>(arg); continue;
                case 3: if (st.empty()) { error = cls + ": jump on empty stack"; return false; }
                        if (st.back().truthy()) pc += static_cast<std::size_t>(arg); else { st.pop_back(); pc += 2; } continue;
                case 4: if (!pop(a)) { error = cls + ": jump on empty stack"; return false; }
                        if (a.truthy()) pc += 2; else pc += static_cast<std::size_t>(arg); continue;
                case 5: if (!pop(a)) { error = cls + ": jump on empty stack"; return false; }
                        if (!a.truthy()) pc += 2; else pc += static_cast<std::size_t>(arg); continue;
                case 0x1E: {   // push configVars[arg]
                    if (!cvs || arg < 0 || static_cast<std::size_t>(arg) >= cvs->list.size()) { error = cls + ": configVar index out of range"; return false; }
                    Var v;
                    if (!eval(cvs->list[static_cast<std::size_t>(arg)], v, error)) return false;
                    push(std::move(v)); pc += 2; continue;
                }
                case 0x1F: {   // push dynamicVars[arg]
                    VarPath p;
                    if (!dyn_path(arg, p)) { error = cls + ": dynamicVar index out of range"; return false; }
                    Var v = g_.bag().get(p);
                    if (!v.is_set()) v = Var::integer(0);   // 0xEE8C00 resets to int 0
                    push(std::move(v)); pc += 2; continue;
                }
                case 0x24: {   // [INFERENCE] message-parameter read (vtbl +0xC8); parameters live in instance.msg_params()
                    VarPath p;
                    if (!dyn_path(arg, p) || p.empty()) { error = cls + ": param index out of range"; return false; }
                    Var v;
                    if (!g_.message_param(p.back(), v)) v = g_.bag().get(p);
                    if (!v.is_set()) v = Var::integer(0);
                    push(std::move(v)); pc += 2; continue;
                }
                case 0x20: {   // [INFERENCE] array element: index on stack, array variable dynamicVars[arg]
                    VarPath p;
                    if (!dyn_path(arg, p) || !pop(a)) { error = cls + ": array op malformed"; return false; }
                    Var arr = g_.bag().get(p);
                    const std::int64_t i = a.as_int();
                    if (arr.kind == Var::Kind::Array && i >= 0 && static_cast<std::size_t>(i) < arr.arr.size()) push(arr.arr[static_cast<std::size_t>(i)]);
                    else push(Var::integer(0));
                    pc += 2; continue;
                }
                case 0x46: {   // push constant m_1B183605[arg] (float table)
                    if (!consts || arg < 0 || static_cast<std::size_t>(arg) >= consts->list.size()) { error = cls + ": constant index out of range"; return false; }
                    push(Var::real(num_f(consts->list[static_cast<std::size_t>(arg)]))); pc += 2; continue;
                }
                case 0x23: if (!pop(a)) { error = cls + ": stack underflow"; return false; } pc += 1; continue;   // sets entity context (m_D815B771); [INFERENCE] ignored
                case 100: case 101: case 102: case 103: case 0xC8: case 0xC9: case 0x8C: case 0x8D: case 0x8E: case 0x8F: case 0x90: case 0x91: case 0xCE: {
                    if (!pop(b) || !pop(a)) { error = cls + ": stack underflow"; return false; }
                    const double x = a.as_double(), y = b.as_double();
                    const bool ints = a.kind == Var::Kind::Int && b.kind == Var::Kind::Int;
                    constexpr double eps = 1e-5;   // [INFERENCE] dword_162C6E8 magnitude; the exact constant was not read
                    switch (op) {
                        case 100: push(ints ? Var::integer(a.i + b.i) : Var::real(x + y)); break;
                        case 101: push(ints ? Var::integer(a.i - b.i) : Var::real(x - y)); break;
                        case 102: push(ints ? Var::integer(a.i * b.i) : Var::real(x * y)); break;
                        case 103: push(Var::real(y != 0 ? x / y : 0.0)); break;       // [INFERENCE] divide by zero -> 0
                        case 0xC8: push(Var::real(std::min(x, y))); break;
                        case 0xC9: push(Var::real(std::max(x, y))); break;
                        case 0x8C: push(Var::boolean(y >= x - eps)); break;           // 140: a <= b   (F6C7B0 case 0x8C: b >= a - eps)
                        case 0x8D: push(Var::boolean(y - eps > x)); break;            // 141: a <  b
                        case 0x8E: push(Var::boolean(x + eps >= y)); break;           // 142: a >= b
                        case 0x8F: push(Var::boolean(x - eps > y)); break;            // 143: a >  b   [INFERENCE: mirror of 141, body shares tail code]
                        case 0x90: push(Var::boolean(a.equals(b))); break;            // 144
                        case 0x91: push(Var::boolean(!a.equals(b))); break;           // 145
                        case 0xCE: {   // 206 random(a,b): uniform float in [a,b] [INFERENCE: F6C310 not read]
                            push(Var::real(x + (y - x) * g_.random01()));
                            break;
                        }
                    }
                    pc += 1; continue;
                }
                case 106: if (!pop(a)) { error = cls + ": stack underflow"; return false; }
                          push(a.kind == Var::Kind::Int ? Var::integer(-a.i) : Var::real(-a.as_double())); pc += 1; continue;
                case 120: if (!pop(a)) { error = cls + ": stack underflow"; return false; } push(Var::boolean(!a.truthy())); pc += 1; continue;
                case 0xCA: {   // 202: three operands; [INFERENCE] clamp(x, lo, hi)
                    if (!pop(c) || !pop(b) || !pop(a)) { error = cls + ": stack underflow"; return false; }
                    push(Var::real(std::min(std::max(a.as_double(), b.as_double()), c.as_double()))); pc += 1; continue;
                }
                default:
                    error = cls + ": unsupported expression opcode " + std::to_string(op);
                    return false;
            }
        }
        if (st.empty()) { out = Var{}; return true; }
        out = st.back();
        return true;
    }

    // literals --------------------------------------------------------------------------------------------------
    const Value* val = o->get("m_value");
    if (val) {
        if (isa(sc, o->type, "STUConfigVarBoolBase") && cls != "STU_C81F8768") { out = Var::boolean(num_i(*val) != 0); return true; }
        if (isa(sc, o->type, "STUConfigVarIntBase")) { out = Var::integer(num_i(*val)); return true; }
        if (isa(sc, o->type, "STUConfigVarFloatBase")) { out = Var::real(num_f(*val)); return true; }
        if (val->kind == Value::Kind::String) { out = Var::str(val->s); return true; }
        if (val->kind == Value::Kind::Raw && val->s.size() >= 12) {
            Vec3 v; std::memcpy(&v, val->s.data(), 12);
            out = Var::vec3(v); return true;
        }
        if (val->kind == Value::Kind::Float) { out = Var::real(val->f); return true; }
    }
    if (cls == "STUConfigVarIntRandom" || cls == "STUConfigVarFloatRandom") {
        const Value* lo = o->get("m_min"); const Value* hi = o->get("m_max");
        const double a = lo ? num_f(*lo) : 0, b = hi ? num_f(*hi) : 0;
        const double r = a + (b - a) * g_.random01();
        out = cls == "STUConfigVarIntRandom" ? Var::integer(static_cast<std::int64_t>(std::llround(r))) : Var::real(r);
        return true;
    }
    if (cls == "STUConfigVarBoolRandom") {
        const Value* ch = o->get("m_chance");
        out = Var::boolean(g_.random01() < (ch ? num_f(*ch) : 0.0)); return true;
    }
    if (cls == "STUConfigVarArray") {
        std::vector<Var> items;
        if (const Value* l = o->get("m_configVars"))
            for (const Value& e : l->list) { Var v; if (!eval(e, v, error)) return false; items.push_back(std::move(v)); }
        out = Var::array(std::move(items)); return true;
    }
    if (cls == "STU_C81F8768") {   // Bool config var over a stack variable; [INFERENCE] reads the variable as a bool
        if (const Value* v = o->get("m_var")) { Var x; if (!eval(*v, x, error)) return false; out = Var::boolean(x.truthy()); return true; }
    }
    if (isa(sc, o->type, "STUConfigVarResource") || isa(sc, o->type, "STUConfigVarEnum")) {
        for (const auto& f : o->fields) {
            if (f.first == data::name_hash("m_83E83924")) continue;
            if (f.second.kind == Value::Kind::ResRef) { out = Var::resource(f.second.u); return true; }
            if (f.second.kind == Value::Kind::Enum) { out = Var::integer(f.second.i); return true; }
        }
        out = Var::resource(0); return true;
    }
    // world getters: evaluate child config vars in authored order, let the host answer
    {
        std::vector<Var> args;
        for (const auto& f : o->fields) {
            const Object* c = obj_of(f.second);
            if (c && is_cfg(sc, c->type)) { Var v; if (!eval(f.second, v, error)) return false; args.push_back(std::move(v)); }
        }
        return g_.host().world_query(cls, args, out, error);
    }
}

bool config_class_supported(const std::string& cls) {
    static const std::set<std::string> known = {
        "STUConfigVarDynamic", "STUConfigVarGetOwner", "STUConfigVarExpression", "STU_EC502A81", "STUConfigVarIntRandom",
        "STUConfigVarFloatRandom", "STUConfigVarBoolRandom", "STUConfigVarArray", "STU_C81F8768"};
    return known.count(cls) != 0;
}

}  // namespace ow::statescript::vm
