#pragma once

#include "bf3/core/install.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/ui/loading_screen.h"
#include "bf3/host/level_load.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class CanvasLayer;
class Node;
}

namespace bf3::godot_side {

class Bf3UiScreens;
class Bf3UiTextures;

inline constexpr int32_t kNativeLoadingLayer = 100;
inline constexpr std::string_view kNativeLoadingPlatform = "win32";
inline constexpr double kNativeLoadingMaxStepSeconds = 1.0;
inline constexpr double kNativeLoadingStepSeconds = 1.0 / 30.0;

struct NativeLoadingFacts {
	uint64_t updates = 0;
	uint64_t open_update = 0;
	uint64_t drawn_updates = 0;
	uint64_t removed_update = 0;
	double open_s = -1.0;
	double removed_s = -1.0;
	bool decode_done_at_open = false;
	std::string stage_at_open;
	std::string header;
	std::string subheader;
	std::string hint;
	std::string message;
	uint64_t hints_served = 0;
	uint64_t opens = 0;
};

class NativeLoading {
public:
	NativeLoading() = default;
	NativeLoading(const NativeLoading &) = delete;
	NativeLoading &operator=(const NativeLoading &) = delete;
	~NativeLoading();

	void attach(godot::Node *p_parent, std::string p_level, std::string p_mode, uint64_t p_seed);
	void update(double p_now_s, const bf3::host::LoadProgress &p_progress);
	void remove(double p_now_s);
	bool shown() const { return layer_ != nullptr; }
	bool open() const { return opened_; }

	const NativeLoadingFacts &facts() const { return facts_; }
	const std::string &error() const { return error_; }
	const bf3::core::ui::LoadingScreenData *data() const { return data_.empty() ? nullptr : data_.back().get(); }
	std::string movie() const;

private:
	struct Prepared {
		bf3::core::ui::LoadingScreenSource source;
		std::string movie;
		std::shared_ptr<const bf3::core::LevelMount> mount;
		std::string error;
	};
	void start_worker(std::shared_ptr<const bf3::core::Install> p_install);
	void try_open(double p_now_s, const bf3::host::LoadProgress &p_progress);
	void read_facts();
	void join();

	godot::CanvasLayer *layer_ = nullptr;
	uint64_t layer_id_ = 0;
	uint64_t screens_id_ = 0;
	std::string level_;
	std::string mode_;
	uint64_t seed_ = 0;
	std::thread worker_;
	std::atomic<bool> worker_done_{ false };
	std::unique_ptr<Prepared> prepared_;
	std::vector<std::unique_ptr<bf3::core::ui::LoadingScreenData>> data_;
	std::shared_ptr<Bf3UiTextures> textures_;
	bool opened_ = false;
	double stepped_s_ = 0.0;
	std::string error_;
	NativeLoadingFacts facts_;

	Bf3UiScreens *screens() const;
};

}
