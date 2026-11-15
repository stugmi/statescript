#pragma once
#include <cstdint>

namespace ow::statescript::runtime {

// Logical projections of the 64-bit-key implementation registry at qword_1854D18.
// The table's first 0x10 bytes are not interpreted here. RVAs 0xF435C0,
// 0xF433A0 and 0xF43450; no IDA memory is owned by these views.
struct ImplementationRecord {
    void* (*factory)(); // +0x00: F43450 calls this to create a runtime node.
    void* implementation; // +0x08: F435C0 returns this without calling it.
};
struct RegistryNode {
    RegistryNode* next; // +0x00
    ImplementationRecord* record; // +0x08
    std::uint64_t unknown_10; // +0x10, not used by the lookup
    std::uintptr_t inheritance_key; // +0x18, STU InheritanceInfo* identity
};
struct ImplementationTable {
    std::uint32_t bucket_count; // registry +0x10
    std::uint32_t entry_count; // registry +0x14 (zero disables lookup)
    RegistryNode* const* buckets; // registry +0x18
};
// The STU class's +8 inheritance chain consists of (type key, next) links.
struct InheritanceLink {
    std::uintptr_t inheritance_key;
    const InheritanceLink* next;
};

const ImplementationRecord* find_record(const ImplementationTable& table, std::uintptr_t key) noexcept;
void* lookup_implementation(const ImplementationTable& table, std::uintptr_t key) noexcept; // 0xF435C0
void* (*lookup_factory(const ImplementationTable& table, std::uintptr_t key) noexcept)();
// 0xF43450: exact class first; then each ancestor in the caller's STU chain.
void* create_for_type(const ImplementationTable& table, std::uintptr_t exact_key,
                      const InheritanceLink* ancestors);
} // namespace ow::statescript::runtime
