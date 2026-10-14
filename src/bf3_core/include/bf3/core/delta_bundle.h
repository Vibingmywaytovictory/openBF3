#pragma once
// Delta bundles (0xDE17AAAA) of patched layout-`delta` superbundles (XP2-4 levels, Xp2-4Chunks,
// Loc/*). Ported from IW4L superbundle_bf3 (delta.rs).
//
//   at patchSb[entry.offset], big-endian:
//   u32 runsSize (multiple of 16) | u32 magic 0xDE17AAAA | u64 pad
//   Run[runsSize / 16] = { u32 copyBytes, u32 fileId, u64 offset }
//   fileId == 0: the next copyBytes of the patch data that follows the table (consumed in order)
//   fileId != 0: baseSb[offset .. offset + copyBytes] (offset ABSOLUTE in the base .sb file)
// The rebuilt bundle is an ordinary binary bundle. It is never materialised: reads are served
// from the runs (binary search over cumulative virtual offsets).

#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::core {

constexpr uint32_t kDeltaMagic = 0xDE17AAAAu;
constexpr size_t kDeltaHeaderLen = 16;

// Positional reader over a whole file (or any byte range addressed from 0).
class ByteSource {
public:
	virtual ~ByteSource() = default;
	// Fills p_dst completely or fails (short read, I/O error).
	virtual Status read_exact_at(uint64_t p_offset, std::span<uint8_t> p_dst) const = 0;
};

struct DeltaRun {
	uint64_t start = 0; // virtual offset in the rebuilt bundle
	uint64_t len = 0;
	bool from_patch = false;
	uint32_t file = 0; // base file id (from_patch == false)
	uint64_t offset = 0; // absolute offset in the patch .sb (from_patch) or the base .sb
};

// Header + run table length (16 + runsSize) from the first 16 bytes.
Result<uint64_t> delta_table_len(std::span<const uint8_t> p_header);

class DeltaBundle {
public:
	// p_table: the header and run table; p_patch_offset/p_patch_len: the toc entry's offset/size
	// in the patch .sb (run offsets are absolute in the patch and base .sb files).
	static Result<DeltaBundle> parse(std::span<const uint8_t> p_table, uint64_t p_patch_offset, uint64_t p_patch_len);

	uint64_t len() const { return len_; }
	bool empty() const { return len_ == 0; }
	const std::vector<DeltaRun> &runs() const { return runs_; }

	Status read_at(const ByteSource &p_patch, const ByteSource &p_base, uint64_t p_offset, std::span<uint8_t> p_dst) const;

private:
	std::vector<DeltaRun> runs_;
	uint64_t len_ = 0;
};

} // namespace bf3::core
