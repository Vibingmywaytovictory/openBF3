#pragma once

#include "bf3/core/result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace bf3::godot_side {

struct ProcessVideoMemory {
	uint64_t local_bytes = 0;
	uint64_t local_budget_bytes = 0;
	uint64_t non_local_bytes = 0;
};

class VideoMemoryMeter {
public:
	VideoMemoryMeter();
	~VideoMemoryMeter();
	VideoMemoryMeter(const VideoMemoryMeter &) = delete;
	VideoMemoryMeter &operator=(const VideoMemoryMeter &) = delete;

	bf3::core::Status open(std::string_view p_adapter_name);
	bool is_open() const;
	std::optional<ProcessVideoMemory> read() const;
	const std::string &adapter() const;
	const std::string &error() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

VideoMemoryMeter &process_video_memory_meter();
std::optional<ProcessVideoMemory> process_video_memory();
std::string process_video_memory_text();
double video_memory_mib(uint64_t p_bytes);

}
