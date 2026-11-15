#include "statescript/vm/value.h"

#include <cmath>
#include <cstdio>

namespace ow::statescript::vm {

bool Var::truthy() const {
    switch (kind) {
        case Kind::Unset: return false;
        case Kind::Bool: return b;
        case Kind::Int: return i != 0;
        case Kind::Float: return f != 0.0;
        case Kind::Vec3: return v.x != 0 || v.y != 0 || v.z != 0;
        case Kind::String: return !s.empty();
        case Kind::Id:
        case Kind::Entity: return id != 0;
        case Kind::Array: return !arr.empty();
    }
    return false;
}

bool Var::equals(const Var& o) const {
    // Expression opcode 144 (EE99C0) is a typed equality; unset variables read as 0 in expressions.
    const bool an = is_number() || kind == Kind::Unset, bn = o.is_number() || o.kind == Kind::Unset;
    if (an && bn) return std::fabs(as_double() - o.as_double()) < 1e-6;   // [INFERENCE] tolerance mirrors the 162C6E8 epsilon used by the compare opcodes
    if (kind != o.kind) return false;
    switch (kind) {
        case Kind::Vec3: return v.x == o.v.x && v.y == o.v.y && v.z == o.v.z;
        case Kind::String: return s == o.s;
        case Kind::Id:
        case Kind::Entity: return id == o.id;
        case Kind::Array:
            if (arr.size() != o.arr.size()) return false;
            for (std::size_t k = 0; k < arr.size(); ++k) if (!arr[k].equals(o.arr[k])) return false;
            return true;
        default: return true;
    }
}

std::string Var::to_string() const {
    char buf[96];
    switch (kind) {
        case Kind::Unset: return "unset";
        case Kind::Bool: return b ? "true" : "false";
        case Kind::Int: return std::to_string(i);
        case Kind::Float: std::snprintf(buf, sizeof buf, "%g", f); return buf;
        case Kind::Vec3: std::snprintf(buf, sizeof buf, "(%g,%g,%g)", v.x, v.y, v.z); return buf;
        case Kind::String: return "'" + s + "'";
        case Kind::Id: std::snprintf(buf, sizeof buf, "res:%016llX", static_cast<unsigned long long>(id)); return buf;
        case Kind::Entity: std::snprintf(buf, sizeof buf, "entity:%llX", static_cast<unsigned long long>(id)); return buf;
        case Kind::Array: {
            std::string r = "[";
            for (std::size_t k = 0; k < arr.size(); ++k) r += (k ? "," : "") + arr[k].to_string();
            return r + "]";
        }
    }
    return "?";
}

std::string path_string(const VarPath& p) {
    std::string r;
    char buf[24];
    for (std::size_t k = 0; k < p.size(); ++k) {
        std::snprintf(buf, sizeof buf, "%s%X", k ? "/" : "", static_cast<unsigned>(p[k] & 0xFFFFFFFFFFFFull));
        r += buf;
    }
    return r.empty() ? "<empty>" : r;
}

const Var* VarBag::find(const VarPath& p) const {
    auto it = vars_.find(p);
    return it == vars_.end() ? nullptr : &it->second;
}

Var VarBag::get(const VarPath& p) const {
    const Var* v = find(p);
    return v ? *v : Var{};
}

bool VarBag::set(const VarPath& p, Var value, std::int64_t frame) {
    auto it = vars_.find(p);
    Var before = it == vars_.end() ? Var{} : it->second;
    if (it != vars_.end() && before.kind == value.kind && before.equals(value)) return false;
    if (it == vars_.end() && value.kind == Var::Kind::Unset) return false;
    vars_[p] = value;
    changes_.push_back({p, before, value, frame});
    if (on_change) on_change(p);
    return true;
}

bool VarBag::erase(const VarPath& p, std::int64_t frame) {
    auto it = vars_.find(p);
    if (it == vars_.end()) return false;
    Var before = it->second;
    vars_.erase(it);
    changes_.push_back({p, before, Var{}, frame});
    if (on_change) on_change(p);
    return true;
}

}  // namespace ow::statescript::vm
