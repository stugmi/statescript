#include "lookup.h"

namespace ow::statescript::runtime {
namespace {
// RVA 0xF435D2..0xF43619. 64-bit mixing, then 32-bit multiply/mask;
// the bucket count is a power of two in the original hash table.
std::uint32_t bucket_for(std::uintptr_t key, std::uint32_t bucket_count) noexcept {
    if (bucket_count == 0) return 0;
    std::uint64_t h = ~std::uint64_t(key) + (std::uint64_t(key) << 21);
    h ^= h >> 24;
    h *= 265;
    h ^= h >> 14;
    h *= 21;
    return (std::uint32_t(h ^ (h >> 28)) * 0x80000001u) & (bucket_count - 1);
}
}

const ImplementationRecord* find_record(const ImplementationTable& table, std::uintptr_t key) noexcept {
    if (table.entry_count == 0 || !table.buckets) return nullptr; // 0xF435C7, 0xF43626
    for (auto* node = table.buckets[bucket_for(key, table.bucket_count)]; node; node = node->next) {
        if (node->inheritance_key == key) return node->record; // 0xF43634..0xF43641
    }
    return nullptr;
}

void* lookup_implementation(const ImplementationTable& table, std::uintptr_t key) noexcept {
    const auto* record = find_record(table, key);
    return record ? record->implementation : nullptr; // 0xF43641..0xF4364A
}

void* (*lookup_factory(const ImplementationTable& table, std::uintptr_t key) noexcept)() {
    const auto* record = find_record(table, key);
    return record ? record->factory : nullptr;
}

void* create_for_type(const ImplementationTable& table, std::uintptr_t exact_key,
                      const InheritanceLink* ancestors) {
    const auto* record = find_record(table, exact_key); // F434EE tests record, not factory
    if (!record) {
        for (auto* link = ancestors; link; link = link->next) {
            record = find_record(table, link->inheritance_key);
            if (record) break;
        }
    }
    // Registered records have a callable factory in the client. A null factory
    // is malformed input, not permission to continue searching the base chain.
    return record && record->factory ? record->factory() : nullptr;
}
} // namespace ow::statescript::runtime
