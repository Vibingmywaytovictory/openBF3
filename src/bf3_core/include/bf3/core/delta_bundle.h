#pragma once

#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::core {

constexpr uint32_t kDeltaMagic = 0xDE17AAAAu;
constexpr size_t kDeltaHeaderLen = 16;

class ByteSource {
public:
	virtual ~ByteSource() = default;

	virtual Status read_exact_at(uint64_t p_offset, std::span<uint8_t> p_dst) const = 0;
};

struct DeltaRun {
	uint64_t start = 0;
	uint64_t len = 0;
	bool from_patch = false;
	uint32_t file = 0;
	uint64_t offset = 0;
};

Result<uint64_t> delta_table_len(std::span<const uint8_t> p_header);

class DeltaBundle {
public:

	static Result<DeltaBundle> parse(std::span<const uint8_t> p_table, uint64_t p_patch_offset, uint64_t p_patch_len);

	uint64_t len() const { return len_; }
	bool empty() const { return len_ == 0; }
	const std::vector<DeltaRun> &runs() const { return runs_; }

	Status read_at(const ByteSource &p_patch, const ByteSource &p_base, uint64_t p_offset, std::span<uint8_t> p_dst) const;

private:
	std::vector<DeltaRun> runs_;
	uint64_t len_ = 0;
};

}
