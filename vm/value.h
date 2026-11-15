// statescript/vm/value.h -- dynamic values and the variable bag of the Statescript graph interpreter.
//
// A Statescript variable is addressed by STUConfigVarDynamic.m_4F6799B5, a LIST of variable-id resources
// (type field 0xD80, e.g. [0D80000000000004, 0D80000000000156]).  The bag keys on the whole list (VarPath).
// [INFERENCE] a 2-element path is "struct variable -> member"; the client's own varbag layout is only known as
// "scalar slot +0xE0 / dictionary slot +0x130" (docs/architecture/statescript-runtime.md), see docs/statescript/vm.md.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ow::statescript::vm {

struct Vec3 { float x = 0, y = 0, z = 0; };

// Type tags follow the DataFlow classification (dataflow_types.h) plus the literal ConfigVar kinds.
struct Var {
    enum class Kind : std::uint8_t { Unset, Bool, Int, Float, Vec3, String, Id, Entity, Array } kind = Kind::Unset;
    bool b = false;
    std::int64_t i = 0;     // Int
    double f = 0;           // Float
    Vec3 v;                 // Vec3
    std::string s;          // String
    std::uint64_t id = 0;   // Id (resource id: identifier / team / hero / ...) or Entity handle
    std::vector<Var> arr;   // Array

    static Var boolean(bool x) { Var r; r.kind = Kind::Bool; r.b = x; return r; }
    static Var integer(std::int64_t x) { Var r; r.kind = Kind::Int; r.i = x; return r; }
    static Var real(double x) { Var r; r.kind = Kind::Float; r.f = x; return r; }
    static Var vec3(Vec3 x) { Var r; r.kind = Kind::Vec3; r.v = x; return r; }
    static Var str(std::string x) { Var r; r.kind = Kind::String; r.s = std::move(x); return r; }
    static Var resource(std::uint64_t x) { Var r; r.kind = Kind::Id; r.id = x; return r; }
    static Var entity(std::uint64_t x) { Var r; r.kind = Kind::Entity; r.id = x; return r; }
    static Var array(std::vector<Var> x) { Var r; r.kind = Kind::Array; r.arr = std::move(x); return r; }

    bool is_set() const { return kind != Kind::Unset; }
    bool is_number() const { return kind == Kind::Int || kind == Kind::Float || kind == Kind::Bool; }
    double as_double() const { return kind == Kind::Float ? f : kind == Kind::Bool ? (b ? 1.0 : 0.0) : static_cast<double>(i); }
    std::int64_t as_int() const { return kind == Kind::Float ? static_cast<std::int64_t>(f) : kind == Kind::Bool ? (b ? 1 : 0) : i; }
    bool truthy() const;                     // Unset -> false; numbers != 0; strings non-empty; ids/entities != 0; arrays non-empty
    bool equals(const Var& o) const;         // numeric kinds compare by value; otherwise kind + payload
    std::string to_string() const;           // diagnostic / log form
};

using VarPath = std::vector<std::uint64_t>;   // STUConfigVarDynamic.m_4F6799B5
std::string path_string(const VarPath& p);    // "0D80..04/0D80..156"

struct VarChange { VarPath path; Var before, after; std::int64_t frame = 0; };

// The per-instance variable bag.  set() records a VarChange and invokes the change hook (used by the instance to
// wake subscribed states); an unchanged value (Var::equals) still records nothing and does not fire the hook.
class VarBag {
public:
    const Var* find(const VarPath& p) const;
    Var get(const VarPath& p) const;                          // Unset when absent
    bool set(const VarPath& p, Var value, std::int64_t frame);   // true when the value changed
    bool erase(const VarPath& p, std::int64_t frame);
    const std::vector<VarChange>& changes() const { return changes_; }
    void clear_changes() { changes_.clear(); }
    const std::map<VarPath, Var>& all() const { return vars_; }
    std::function<void(const VarPath&)> on_change;
private:
    std::map<VarPath, Var> vars_;
    std::vector<VarChange> changes_;
};

}  // namespace ow::statescript::vm
