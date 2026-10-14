#pragma once
// Resource type ids (u32 hashes stored as i32 bits in manifests) and their names.
// The name table is ported from the ebx-dump probe (research/probes/tools/ebx-dump, res_type_name).

#include <cstdint>
#include <string>

namespace bf3::core {

namespace res_type {
constexpr uint32_t kDxTexture = 0x5C4954A6u;
constexpr uint32_t kMeshSet = 0x49B156D4u;
constexpr uint32_t kHavokPhysicsData = 0x91043F65u;
constexpr uint32_t kHavokDestructionPhysicsData = 0x4864737Bu;
constexpr uint32_t kTerrain = 0x6BB6D7D2u;
constexpr uint32_t kSwfMovie = 0x2D47A5FFu;
} // namespace res_type

// Known name ("DxTexture"), else "0x%08X".
std::string res_type_name(uint32_t p_type);

} // namespace bf3::core
