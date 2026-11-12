#include "memory_report.h"

#include "texture_ledger.h"
#include "video_memory.h"

#include "bf3/core/util/format.h"
#include "bf3/host/server_session.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

namespace bf3::godot_side {

namespace {

constexpr size_t kLargestAllocations = 12;
constexpr uint64_t kScanPeriod = 30;
constexpr uint64_t kGrowthStep = 128ull << 20;

}

CommittedMemory scan_committed_memory() {
	CommittedMemory out;
	std::map<uintptr_t, uint64_t> allocations;
	MEMORY_BASIC_INFORMATION info{};
	uintptr_t at = 0;
	while (VirtualQuery(reinterpret_cast<const void *>(at), &info, sizeof(info)) == sizeof(info)) {
		if (info.State == MEM_COMMIT) {
			if (info.Type == MEM_PRIVATE) {
				out.private_bytes += info.RegionSize;
				out.write_combined_bytes += (info.Protect & PAGE_WRITECOMBINE) != 0 ? info.RegionSize : 0;
				allocations[reinterpret_cast<uintptr_t>(info.AllocationBase)] += info.RegionSize;
			} else if (info.Type == MEM_IMAGE) {
				out.image_bytes += info.RegionSize;
			} else {
				out.mapped_bytes += info.RegionSize;
			}
		}
		const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
		if (next <= at) {
			break;
		}
		at = next;
	}
	std::vector<HANDLE> heaps(256);
	const DWORD n = GetProcessHeaps(DWORD(heaps.size()), heaps.data());
	for (DWORD i = 0; i < std::min<DWORD>(n, DWORD(heaps.size())); ++i) {
		HEAP_SUMMARY summary{};
		summary.cb = sizeof(summary);
		if (HeapSummary(heaps[i], 0, &summary)) {
			out.heap_bytes += summary.cbCommitted;
		}
	}
	out.allocations = allocations.size();
	for (const auto &[base, bytes] : allocations) {
		out.largest.push_back(bytes);
	}
	std::sort(out.largest.begin(), out.largest.end(), std::greater<>());
	out.largest.resize(std::min(out.largest.size(), kLargestAllocations));
	return out;
}

void MemoryReport::watch(uint64_t p_frame) {
	if (p_frame % kScanPeriod != 0) {
		return;
	}
	const CommittedMemory c = scan_committed_memory();
	const uint64_t cpu = c.cpu_bytes();
	cpu_peak_ = std::max(cpu_peak_, cpu);
	if (cpu >= reported_ + kGrowthStep) {
		reported_ = cpu;
		godot::UtilityFunctions::print(godot::String::utf8(core::fmt("[memory] frame %llu: cpu private %.1f MiB, "
																	 "write-combined %.1f MiB, heaps %.1f MiB",
				static_cast<unsigned long long>(p_frame), video_memory_mib(cpu),
				video_memory_mib(c.write_combined_bytes), video_memory_mib(c.heap_bytes))
						.c_str()));
	}
}

void MemoryReport::sample(uint64_t p_frame) {
	++samples_;
	if (const std::optional<ProcessVideoMemory> v = process_video_memory()) {
		video_local_ = std::max(video_local_, v->local_bytes);
		video_non_local_ = std::max(video_non_local_, v->non_local_bytes);
		video_budget_ = v->local_budget_bytes;
	}
	working_set_ = std::max(working_set_, process_working_set_bytes());
	if (p_frame % kScanPeriod == 0) {
		cpu_play_ = std::max(cpu_play_, scan_committed_memory().cpu_bytes());
	}
}

std::string MemoryReport::text() const {
	const CommittedMemory c = scan_committed_memory();
	std::string largest;
	for (const uint64_t bytes : c.largest) {
		largest += core::fmt(" %.1f", video_memory_mib(bytes));
	}
	return core::fmt("[memory] %llu samples after the level was done: video local peak %.1f MiB, non-local peak "
					 "%.1f MiB, budget %.1f MiB; cpu private peak %.1f MiB in play, %.1f MiB in the load; process "
					 "peak commit %.1f MiB; working set peak %.1f MiB; on %s\n",
				   static_cast<unsigned long long>(samples_), video_memory_mib(video_local_),
				   video_memory_mib(video_non_local_), video_memory_mib(video_budget_), video_memory_mib(cpu_play_),
				   video_memory_mib(cpu_peak_), video_memory_mib(host::process_peak_private_bytes()),
				   video_memory_mib(working_set_), process_video_memory_meter().adapter().c_str()) +
			core::fmt("[memory] now: private %.1f MiB in %zu allocations, of it %.1f MiB write-combined (the driver's "
					  "mappings of video memory) and %.1f MiB in the process heaps; image %.1f MiB, mapped %.1f MiB; "
					  "largest allocations MiB:%s\n",
					video_memory_mib(c.private_bytes), c.allocations, video_memory_mib(c.write_combined_bytes),
					video_memory_mib(c.heap_bytes), video_memory_mib(c.image_bytes), video_memory_mib(c.mapped_bytes),
					largest.c_str()) +
			texture_ledger_text();
}

}
