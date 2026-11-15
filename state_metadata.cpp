#include "state_metadata.h"
#include <algorithm>
#include <cstddef>
#include <iterator>

namespace statescript {
namespace {
enum RuleFlags : std::uint8_t { StripStatePrefix = 1, RequireIndex = 2 };
struct TypeRule { std::uint32_t hash; std::uint8_t flags; };
constexpr TypeRule kRules[] = {
#include "state_metadata_rules.inc"
};
constexpr bool RulesAreSorted() noexcept {
    for (std::size_t i = 1; i < std::size(kRules); ++i)
        if (kRules[i - 1].hash >= kRules[i].hash) return false;
    return true;
}
static_assert(RulesAreSorted(), "Binary search requires unique sorted type hashes");

std::uint8_t RulesFor(std::uint32_t hash) noexcept {
    const auto* rule = std::lower_bound(std::begin(kRules), std::end(kRules), hash,
        [](const TypeRule& rule, std::uint32_t value) { return rule.hash < value; });
    return rule != std::end(kRules) && rule->hash == hash ? rule->flags : 0;
}

// RVA 0xEEACB0 compares ASCII case-insensitively; locale folding would change it.
constexpr char UpperAscii(char value) noexcept {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - ('a' - 'A')) : value;
}

// STU TeString storage: 0xEEAC70 and duplicated paths at 0xEEC3BD.
// These are non-owning memory-layout views, not host std::string objects.
struct HeapString {
    union { const char* heap; char inlineBytes[16]; } storage;
    std::int32_t length;
    std::uint32_t capacity;
};
struct ImageString { std::int32_t length; std::uint32_t unknown04; const char* data; };
static_assert(offsetof(HeapString, length) == 0x10);
static_assert(offsetof(HeapString, capacity) == 0x14);
static_assert(offsetof(ImageString, data) == 8);

const char* NonemptyDisplayName(const stu::TeString& name) noexcept {
    // The binary assumes a valid representation even for empty/mode-0 cells.
    if (name.mode == 1) {
        const auto& rep = *static_cast<const HeapString*>(name.rep);
        if (rep.length <= 0) return nullptr;
        return rep.capacity > 16 ? rep.storage.heap : rep.storage.inlineBytes;
    }
    const auto& rep = *static_cast<const ImageString*>(name.rep);
    return rep.length > 0 ? rep.data : nullptr;
}
} // namespace

const char* DefaultStateDisplayName(std::uint32_t hash, const char* typeName) noexcept {
    if (!(RulesFor(hash) & StripStatePrefix)) return typeName;
    constexpr char prefix[] = "STUStatescriptState"; // decrypted string at 0x1803164
    const char* suffix = typeName;
    for (char ch : prefix) {
        if (!ch) break;
        if (UpperAscii(*suffix) != UpperAscii(ch)) return typeName;
        ++suffix;
    }
    return *suffix ? suffix : "State"; // decrypted fallback at 0x1803184
}

const char* StateDisplayName(const stu::STUInfo& type, const stu::TeString& displayName) noexcept {
    if (const char* text = NonemptyDisplayName(displayName)) return text;
    return DefaultStateDisplayName(type.Hash, type.Name_str);
}

bool PassesStateIndexGate(std::uint32_t hash, std::int32_t field6C) noexcept {
    return !(RulesFor(hash) & RequireIndex) || field6C >= 0;
}
} // namespace statescript
