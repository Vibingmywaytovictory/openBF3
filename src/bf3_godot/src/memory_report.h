#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::godot_side {

struct CommittedMemory {
	uint64_t private_bytes = 0;
	uint64_t write_combined_bytes = 0;
	uint64_t heap_bytes = 0;
	uint64_t image_bytes = 0;
	uint64_t mapped_bytes = 0;
	size_t allocations = 0;
	std::vector<uint64_t> largest;

	uint64_t cpu_bytes() const { return private_bytes - write_combined_bytes; }
};

CommittedMemory scan_committed_memory();

class MemoryReport {
public:
	void watch(uint64_t p_frame);
	void sample(uint64_t p_frame);
	std::string text() const;

private:
	uint64_t samples_ = 0;
	uint64_t video_local_ = 0;
	uint64_t video_non_local_ = 0;
	uint64_t video_budget_ = 0;
	uint64_t working_set_ = 0;
	uint64_t cpu_play_ = 0;
	uint64_t cpu_peak_ = 0;
	uint64_t reported_ = 0;
};

}
