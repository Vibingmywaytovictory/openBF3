#include "video_memory.h"

#include "godot_util.h"

#include "bf3/core/util/format.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/string.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dxgi1_4.h>

namespace bf3::godot_side {

struct VideoMemoryMeter::Impl {
	IDXGIAdapter3 *adapter = nullptr;
	std::string name;
	std::string error;

	~Impl() { reset(); }

	void reset() {
		if (adapter != nullptr) {
			adapter->Release();
			adapter = nullptr;
		}
	}
};

VideoMemoryMeter::VideoMemoryMeter() : impl_(std::make_unique<Impl>()) {}

VideoMemoryMeter::~VideoMemoryMeter() = default;

bf3::core::Status VideoMemoryMeter::open(std::string_view p_adapter_name) {
	impl_->reset();
	impl_->name.clear();
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
		impl_->error = "CreateDXGIFactory1 failed";
		return bf3::core::make_error(impl_->error);
	}
	uint64_t best_usage = 0;
	uint32_t matched = 0;
	IDXGIAdapter1 *a = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 desc{};
		IDXGIAdapter3 *a3 = nullptr;
		if (SUCCEEDED(a->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
				to_std(godot::String(desc.Description)) == p_adapter_name &&
				SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&a3)))) {
			DXGI_QUERY_VIDEO_MEMORY_INFO info{};
			const uint64_t usage = SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))
					? info.CurrentUsage
					: 0;
			if (impl_->adapter == nullptr || usage > best_usage) {
				impl_->reset();
				impl_->adapter = a3;
				best_usage = usage;
			} else {
				a3->Release();
			}
			++matched;
		}
		a->Release();
	}
	factory->Release();
	if (impl_->adapter == nullptr) {
		impl_->error = "no DXGI adapter named '" + std::string(p_adapter_name) + "'";
		return bf3::core::make_error(impl_->error);
	}
	impl_->name = std::string(p_adapter_name) + (matched > 1 ? " (" + std::to_string(matched) + " matched)" : "");
	impl_->error.clear();
	return bf3::core::Status::success();
}

bool VideoMemoryMeter::is_open() const {
	return impl_->adapter != nullptr;
}

std::optional<ProcessVideoMemory> VideoMemoryMeter::read() const {
	if (impl_->adapter == nullptr) {
		return std::nullopt;
	}
	DXGI_QUERY_VIDEO_MEMORY_INFO local{};
	DXGI_QUERY_VIDEO_MEMORY_INFO non_local{};
	if (FAILED(impl_->adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
			FAILED(impl_->adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local))) {
		return std::nullopt;
	}
	return ProcessVideoMemory{ local.CurrentUsage, local.Budget, non_local.CurrentUsage };
}

const std::string &VideoMemoryMeter::adapter() const {
	return impl_->name;
}

const std::string &VideoMemoryMeter::error() const {
	return impl_->error;
}

VideoMemoryMeter &process_video_memory_meter() {
	static VideoMemoryMeter meter;
	static bool tried = false;
	if (!tried) {
		tried = true;
		if (godot::RenderingServer *rs = godot::RenderingServer::get_singleton()) {
			meter.open(to_std(rs->get_video_adapter_name()));
		}
	}
	return meter;
}

std::optional<ProcessVideoMemory> process_video_memory() {
	return process_video_memory_meter().read();
}

double video_memory_mib(uint64_t p_bytes) {
	return double(p_bytes) / (1024.0 * 1024.0);
}

std::string process_video_memory_text() {
	const VideoMemoryMeter &m = process_video_memory_meter();
	const std::optional<ProcessVideoMemory> v = m.read();
	if (!v) {
		return "DXGI local video memory unavailable: " + (m.error().empty() ? std::string("no reading") : m.error());
	}
	return bf3::core::fmt("DXGI local video memory %.1f MiB of a %.1f MiB budget, non-local %.1f MiB, on %s",
			video_memory_mib(v->local_bytes), video_memory_mib(v->local_budget_bytes),
			video_memory_mib(v->non_local_bytes), m.adapter().c_str());
}

}
