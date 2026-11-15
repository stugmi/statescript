#include "statescript/data/stud_image.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace ow::statescript::data {
namespace {
#include "statescript/data/stu_schema_data.inc"

[[noreturn]] void fail(const std::string& m) { throw std::runtime_error(m); }
constexpr int kMaxDepth = 400;   // nested object depth; the largest beta graph needs well under 250
}  // namespace

std::uint32_t name_hash(const std::string& name) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char ch : name) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<unsigned char>(ch + 32);
        crc = table[(crc ^ ch) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

Schema::Schema() {
    for (const auto& t : kSchemaTypes) types_[t.hash] = &t;
    for (const auto& p : kSchemaPrims) prims_[p.hash] = &p;
    for (const auto& e : kSchemaEnums) enums_[e.hash] = &e;
    for (const auto& n : kSchemaNames) names_[n.hash] = n.name;
    for (const auto& n : kSchemaFieldNames) field_names_[n.hash] = n.name;
}
const Schema& Schema::beta() { static const Schema s; return s; }
const SchemaType* Schema::find(std::uint32_t h) const { auto it = types_.find(h); return it == types_.end() ? nullptr : it->second; }
const SchemaPrim* Schema::prim(std::uint32_t h) const { auto it = prims_.find(h); return it == prims_.end() ? nullptr : it->second; }
std::uint32_t Schema::enum_size(std::uint32_t h) const { auto it = enums_.find(h); return it == enums_.end() ? 0 : it->second->size; }
std::uint32_t Schema::parent(std::uint32_t h) const { auto t = find(h); return t ? t->parent : 0; }
bool Schema::is_a(std::uint32_t h, std::uint32_t base) const {
    for (int guard = 0; h && guard < 64; ++guard) {
        if (h == base) return true;
        h = parent(h);
    }
    return false;
}
std::string Schema::class_name(std::uint32_t h) const {
    auto it = names_.find(h);
    if (it != names_.end()) return it->second;
    char buf[32];
    std::snprintf(buf, sizeof buf, "STU_%08X", h);
    return buf;
}
std::string Schema::field_name(std::uint32_t h) const {
    auto it = field_names_.find(h);
    if (it != field_names_.end()) return it->second;
    char buf[16];
    std::snprintf(buf, sizeof buf, "m_%08X", h);
    return buf;
}
void Schema::fields_of(std::uint32_t h, std::vector<const SchemaField*>& out) const {
    const SchemaType* t = find(h);
    if (!t) fail("unregistered STU type " + class_name(h));
    if (t->parent) fields_of(t->parent, out);
    for (std::uint32_t i = 0; i < t->field_count; ++i) out.push_back(&kSchemaFields[t->field_begin + i]);
}

const Value* Object::get(std::uint32_t fh) const {
    for (const auto& f : fields) if (f.first == fh) return &f.second;
    return nullptr;
}
const Value* Object::get(const char* name) const {
    // unsolved fields are named m_XXXXXXXX where XXXXXXXX is the field hash itself
    if (name[0] == 'm' && name[1] == '_' && std::strlen(name) == 10) {
        char* end = nullptr;
        unsigned long v = std::strtoul(name + 2, &end, 16);
        if (end == name + 10) return get(static_cast<std::uint32_t>(v));
    }
    return get(name_hash(name));
}

struct Decoder {
    StudImage& img;
    const std::vector<std::uint8_t>& d;
    const Schema& sc;
    int depth = 0;

    void span(std::uint64_t off, std::uint64_t size) const {
        if (off > d.size() || size > d.size() - off) {
            char b[96]; std::snprintf(b, sizeof b, "STUD range %llx+%llx exceeds %zx", (unsigned long long)off, (unsigned long long)size, d.size());
            fail(b);
        }
    }
    std::uint32_t u32(std::uint64_t off) const { span(off, 4); std::uint32_t v; std::memcpy(&v, d.data() + off, 4); return v; }
    std::uint64_t u64(std::uint64_t off) const { span(off, 8); std::uint64_t v; std::memcpy(&v, d.data() + off, 8); return v; }

    const Object* object(std::uint64_t raw) {
        std::uint32_t off = static_cast<std::uint32_t>(raw);   // on disk: u32 offset, high dword unused; 0xFFFFFFFF/0 = null
        if (off == 0 || off == 0xFFFFFFFFu) return nullptr;
        auto it = img.objects_.find(off);
        if (it != img.objects_.end()) return it->second.get();
        auto o = std::make_unique<Object>();
        o->offset = off;
        o->type = u32(off);
        Object* p = o.get();
        img.objects_[off] = std::move(o);   // memoise before decoding: cycles resolve to the same pointer
        if (++depth > kMaxDepth) fail("STUD object nesting too deep");
        decode_fields(p->type, off, p->fields);
        --depth;
        return p;
    }
    void decode_fields(std::uint32_t type, std::uint64_t base, std::vector<std::pair<std::uint32_t, Value>>& out) {
        const SchemaType* t = sc.find(type);
        if (!t) fail("unregistered STU type " + sc.class_name(type));
        span(base, t->size);
        std::vector<const SchemaField*> fs;
        sc.fields_of(type, fs);
        out.reserve(fs.size());
        for (const SchemaField* f : fs) out.emplace_back(f->hash, value(f->kind, f->ref, base + f->offset));
    }
    std::uint32_t stride(std::uint32_t kind, std::uint32_t ref) const {
        switch (kind) {
            case 0: { auto p = sc.prim(ref); if (!p) fail("unknown primitive"); return p->size; }
            case 2: return 8;
            case 4: { auto t = sc.find(ref); if (!t) fail("unregistered inline type"); return t->size; }
            case 8: { auto s = sc.enum_size(ref); if (!s) fail("unknown enum"); return s; }
            case 10: case 12: return 16;
            default: fail("unknown list element kind");
        }
    }
    Value value(std::uint32_t kind, std::uint32_t ref, std::uint64_t off) {
        Value v;
        switch (kind) {
            case 1: case 3: case 5: case 9: case 11: case 13: {
                v.kind = Value::Kind::List;
                std::uint64_t header = u64(off);
                if (!header) return v;
                span(header, 16);
                std::int32_t count; std::memcpy(&count, d.data() + header, 4);
                std::uint64_t base = u64(header + 8);
                std::uint32_t st = stride(kind - 1, ref);
                if (count < 0 || (count && !base)) fail("invalid STUD list");
                span(base, std::uint64_t(count) * st);
                v.list.reserve(static_cast<std::size_t>(count));
                for (std::int32_t i = 0; i < count; ++i) v.list.push_back(value(kind - 1, ref, base + std::uint64_t(i) * st));
                return v;
            }
            case 2: v.kind = Value::Kind::ObjRef; v.type = ref; v.ref = object(u64(off)); return v;
            case 4: {
                v.kind = Value::Kind::Inline; v.type = ref;
                v.inl = std::make_shared<Object>();
                v.inl->type = ref; v.inl->offset = static_cast<std::uint32_t>(off);
                if (++depth > kMaxDepth) fail("STUD object nesting too deep");
                decode_fields(ref, off, v.inl->fields);
                --depth;
                return v;
            }
            case 10: case 12: v.kind = Value::Kind::ResRef; v.type = kind == 12 ? ref : 0; v.u = u64(off + 8); return v;
            case 8: {
                std::uint32_t sz = sc.enum_size(ref);
                if (!sz) fail("unknown enum");
                span(off, sz);
                v.kind = Value::Kind::Enum; v.type = ref;
                if (sz == 1) v.i = d[off]; else { std::int32_t x; std::memcpy(&x, d.data() + off, 4); v.i = x; }
                v.u = static_cast<std::uint64_t>(v.i);
                return v;
            }
            case 7: {
                v.kind = Value::Kind::Map;
                std::uint64_t header = u64(off);
                if (!header) return v;
                std::uint32_t n = u32(header);
                std::uint64_t ends = u64(header + 8), items = u64(header + 16);
                span(ends, std::uint64_t(n) * 4);
                std::uint32_t count = n ? u32(ends + std::uint64_t(n - 1) * 4) : 0;
                span(items, std::uint64_t(count) * 16);
                // item = {u32 key; pad; 8 raw value bytes}: the value constraint is not recoverable from (kind, ref) [OPEN];
                // the entity component map's value is the u32 offset of the component object (verified on real entities).
                v.u = count;
                for (std::uint32_t k = 0; k < count; ++k) {
                    Value e; e.kind = Value::Kind::UInt;
                    e.u = u32(items + std::uint64_t(k) * 16);
                    e.i = static_cast<std::int64_t>(u64(items + std::uint64_t(k) * 16 + 8));
                    v.list.push_back(std::move(e));
                }
                return v;
            }
            case 0: {
                auto p = sc.prim(ref);
                if (!p) fail("unknown primitive hash");
                span(off, p->size);
                v.type = ref;
                switch (p->kind) {
                    case 1: {   // teString: u64 -> header {u32 w0; u32 w1; u64 buffer}
                        v.kind = Value::Kind::String;
                        std::uint64_t header = u64(off);
                        if (!header) return v;
                        span(header, 16);
                        std::uint32_t w[2]; std::memcpy(w, d.data() + header, 8);
                        std::uint64_t buf = u64(header + 8);
                        span(buf, 1);
                        for (std::uint32_t n : w) {
                            if (n < d.size() - buf && d[buf + n] == 0 && !std::memchr(d.data() + buf, 0, n)) {
                                v.s.assign(reinterpret_cast<const char*>(d.data() + buf), n);
                                return v;
                            }
                        }
                        fail("STUD string length/terminator disagreement");
                    }
                    case 2: { v.kind = Value::Kind::Float; float x; std::memcpy(&x, d.data() + off, 4); v.f = x; return v; }
                    case 3: { v.kind = Value::Kind::Float; std::memcpy(&v.f, d.data() + off, 8); return v; }
                    case 4: { v.kind = Value::Kind::UInt; std::uint64_t x = 0; std::memcpy(&x, d.data() + off, p->size); v.u = x; return v; }
                    case 5: {
                        v.kind = Value::Kind::SInt; std::uint64_t x = 0; std::memcpy(&x, d.data() + off, p->size);
                        unsigned bits = p->size * 8;
                        if (bits < 64 && (x >> (bits - 1)) & 1) x |= ~0ull << bits;
                        v.i = static_cast<std::int64_t>(x); return v;
                    }
                    default: v.kind = Value::Kind::Raw; v.s.assign(reinterpret_cast<const char*>(d.data() + off), p->size); return v;
                }
            }
            default: fail("unsupported constraint kind " + std::to_string(kind));
        }
    }
};

const Object* StudImage::object_at(std::uint32_t offset, std::string& error) {
    error.clear();
    try {
        Decoder dec{*this, data_, *schema_};
        return dec.object(offset);
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

bool StudImage::parse(std::vector<std::uint8_t> bytes, const Schema& schema, StudImage& out, std::string& error) {
    error.clear();
    try {
        out = StudImage();
        out.data_ = std::move(bytes);
        out.schema_ = &schema;
        Decoder dec{out, out.data_, schema};
        if (out.data_.size() < 16 || dec.u32(0) != 0x53545544u || dec.u32(4) != 3) fail("not a STUD v3 image");
        std::uint64_t hdr = dec.u64(8);
        std::uint32_t count = dec.u32(hdr);
        std::uint64_t table = dec.u64(hdr + 8);
        dec.span(table, std::uint64_t(count) * 16);
        if (!count || dec.u32(table) != 16) fail("STUD root must be at file+0x10");
        // entry layoutSignature (per STUInfo, 0x833D8B) is not checked: the layouts used here are the registry's own.
        out.root_ = dec.object(16);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        out = StudImage();
        return false;
    }
}

}  // namespace ow::statescript::data
