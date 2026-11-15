// Cleaned beta DataFlow value classification. RVA 0xEE82D0.
#pragma once
#include <cstdint>
#include "stu/rtti.h"

namespace statescript {
// Runtime codes, not STU hashes or component IDs. Names follow the six
// registry types selected by 0xEE82D0; 0 is the actual default return.
enum class DataFlowValueKind : std::uint32_t {
    Unknown = 0,
    Float = 1,
    Vec2 = 2,
    Vec3 = 3,
    Vec4 = 4,
    Guid = 5,
    EntityId = 6,
};

// The original calls object.vtable[5]() and reads STUInfo::Hash at +0x50.
// Taking the descriptor explicitly avoids inventing a host C++ vtable ABI.
// This matches EXACT types: a derived type is not automatically accepted.
constexpr DataFlowValueKind ClassifyDataFlowType(std::uint32_t hash) noexcept {
    switch (hash) {
    case 0x74A4E1E9u: return DataFlowValueKind::Float;    // STUDataFlowType_Float
    case 0x1B462083u: return DataFlowValueKind::Vec2;     // STUDataFlowType_Vec2
    case 0x6C411015u: return DataFlowValueKind::Vec3;     // STUDataFlowType_Vec3
    case 0xF22585B6u: return DataFlowValueKind::Vec4;     // STUDataFlowType_Vec4
    case 0x9722AF42u: return DataFlowValueKind::Guid;     // STUDataFlowType_GUID
    case 0x2A1D1E36u: return DataFlowValueKind::EntityId; // STUDataFlowType_EntityID
    default: return DataFlowValueKind::Unknown;
    }
}

constexpr DataFlowValueKind ClassifyDataFlowType(const stu::STUInfo& type) noexcept {
    return ClassifyDataFlowType(type.Hash);
}
} // namespace statescript
