// Statescript editor/runtime metadata, not graph execution.
// Reconstructed from RVAs 0xEEAE00, 0xEEC2F0, 0xEED8F0 and 0xEEEA00.
#pragma once
#include <cstdint>
#include "stu/rtti.h"
#include "stu/containers.h"

namespace statescript {
// Exact beta type dispatch, not RTTI inheritance matching. The original
// class names are stripped in the dump; the returned fallback may be empty.
const char* DefaultStateDisplayName(std::uint32_t typeHash, const char* typeName) noexcept;

// A nonempty STU m_displayName at object+8 wins over the type-derived name.
// Input string storage must be valid and NUL terminated, as in the binary.
const char* StateDisplayName(const stu::STUInfo& type, const stu::TeString& displayName) noexcept;

// field6C is m_4B062861 at STU object+0x6C. Its source-level meaning remains
// unresolved. 0xEED8F0 tests it only for 122 exact types, otherwise returns true.
bool PassesStateIndexGate(std::uint32_t typeHash, std::int32_t field6C) noexcept;

// 0xEEEA00's entire hash decision tree reduces to this signed test, even for
// unknown hashes. At 0xF49540 it gates a slot-54 call before setting flag 0x40.
constexpr bool HasStateIndex(std::int32_t field6C) noexcept { return field6C >= 0; }
} // namespace statescript
