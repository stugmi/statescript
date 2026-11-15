// statescript/data/stud_image.h -- generic, schema driven reader for STUD v3 images (CASC STU resources).
//
// Wire format: recovered/docs/formats/stu-binary-formats.md section 2 (loader 0x833CE0, relocation slot 26).
// Field layouts come from the beta registry dump (tables generated into stu_schema_data.inc), NOT from retail/OWLib.
// Every read is bounds checked; malformed input yields an error string, never a partial result.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ow::statescript::data {

// STUArgumentInfo constraint kinds (stu/constraint.h).
enum class FieldKind : std::uint32_t {
    Primitive = 0, PrimitiveList = 1, ObjectPtr = 2, ObjectPtrList = 3, InlineObject = 4, InlineObjectList = 5,
    Map = 7, Enum = 8, EnumList = 9, NonStuResourceRef = 10, NonStuResourceRefList = 11, ResourceRef = 12, ResourceRefList = 13,
};

struct SchemaField { std::uint32_t hash; std::uint32_t offset; std::uint32_t kind; std::uint32_t ref; };
struct SchemaType { std::uint32_t hash, parent, size, field_begin, field_count; };
struct SchemaPrim { std::uint32_t hash; std::uint32_t size; std::uint32_t kind; };   // kind: 0 raw 1 string 2 f32 3 f64 4 uint 5 sint
struct SchemaEnum { std::uint32_t hash; std::uint32_t size; };
struct SchemaName { std::uint32_t hash; const char* name; };

// CRC32 of the lower-cased name: type hash == field hash == this (StringHash_CRC32Lower, RVA 0x7F88E0).
std::uint32_t name_hash(const std::string& name);

class Schema {
public:
    static const Schema& beta();                       // the 3,639 beta STU types
    const SchemaType* find(std::uint32_t type_hash) const;
    const SchemaPrim* prim(std::uint32_t hash) const;
    std::uint32_t enum_size(std::uint32_t enum_hash) const;   // 4 unless the enum is 1 byte wide (0 = unknown enum)
    bool is_a(std::uint32_t type_hash, std::uint32_t base_hash) const;
    std::string field_name(std::uint32_t field_hash) const;   // solved name or m_XXXXXXXX
    std::string class_name(std::uint32_t type_hash) const;    // "STUxxx" or "STU_XXXXXXXX" when the name is unsolved
    std::uint32_t parent(std::uint32_t type_hash) const;
    // fields of the type, parent fields first (== decode order)
    void fields_of(std::uint32_t type_hash, std::vector<const SchemaField*>& out) const;
private:
    Schema();
    std::map<std::uint32_t, const SchemaType*> types_;
    std::map<std::uint32_t, const SchemaPrim*> prims_;
    std::map<std::uint32_t, const SchemaEnum*> enums_;
    std::map<std::uint32_t, const char*> names_, field_names_;
};

struct Object;
struct Value {
    enum class Kind { Null, UInt, SInt, Float, String, Enum, ObjRef, Inline, List, ResRef, Map, Raw } kind = Kind::Null;
    std::uint64_t u = 0;                 // UInt / ResRef id / Enum value (sign-extended) / Map entry count
    std::int64_t i = 0;                  // SInt / Enum value
    double f = 0;                        // Float
    std::string s;                       // String; Raw bytes
    std::uint32_t type = 0;              // enum hash / referenced STU type hash (ResRef kind 12) / primitive hash
    const Object* ref = nullptr;         // ObjRef target (nullptr == null pointer)
    std::shared_ptr<Object> inl;         // Inline
    std::vector<Value> list;             // List; Map: one UInt per item (u = key, i = raw 8-byte value)
    bool is_null() const { return kind == Kind::Null || (kind == Kind::ObjRef && !ref); }
};

struct Object {
    std::uint32_t offset = 0;            // file offset of the instance
    std::uint32_t type = 0;              // STU type hash (u32 at the instance start)
    std::vector<std::pair<std::uint32_t, Value>> fields;   // (field hash, value), parent fields first
    const Value* get(std::uint32_t field_hash) const;
    const Value* get(const char* field_name) const;
};

class StudImage {
public:
    // Parses the image and decodes the entire object graph reachable from entry 0 (the root at file+0x10).
    static bool parse(std::vector<std::uint8_t> bytes, const Schema& schema, StudImage& out, std::string& error);
    const Object* root() const { return root_; }
    // Decode (or fetch) the instance at a file offset that is not reachable through object pointers, e.g. the raw values of
    // an entity's component map. Returns nullptr and sets error on malformed input.
    const Object* object_at(std::uint32_t offset, std::string& error);
    std::size_t object_count() const { return objects_.size(); }
    const std::map<std::uint32_t, std::unique_ptr<Object>>& objects() const { return objects_; }
    const Schema& schema() const { return *schema_; }
private:
    friend struct Decoder;
    std::vector<std::uint8_t> data_;
    const Schema* schema_ = nullptr;
    std::map<std::uint32_t, std::unique_ptr<Object>> objects_;
    const Object* root_ = nullptr;
};

}  // namespace ow::statescript::data
