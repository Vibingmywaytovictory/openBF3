#pragma once

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace bf3::godot_side {

inline constexpr double kFireGuardTimeout = 90.0;
inline constexpr double kFireGuardSettle = 1.5;
inline constexpr double kFireGuardTap = 0.1;
inline constexpr double kFireGuardWatch = 0.6;
inline constexpr double kFireGuardOpenWait = 3.0;

struct FireGuardIo {
	bool alive = false;
	bool game_input = false;
	bool captured = false;
	bool rose_open = false;
	bool spot_held = false;
	uint64_t own_shots = 0;
	int32_t fire_mode = -1;
	int64_t fire_mode_key = 0;
	godot::Vector2 centre;
	std::function<void(const godot::Ref<godot::InputEvent> &)> push;
	std::function<bool(const std::string &)> shot;
};

class FireGuardCheck {
public:
	explicit FireGuardCheck(std::string p_shots) : shot_dir_(std::move(p_shots)) {}
	bool step(const FireGuardIo &p_io, double p_delta);
	int exit_code() const { return done_ && failures_ == 0 && rows_ > 0 ? 0 : 1; }
	std::string report() const;

private:
	enum class Phase : uint8_t {
		Wait,
		Click,
		ModeKey,
		Escape,
		MenuClick,
		Resume,
		ClickAfterMenu,
		ModeBack,
		RoseOpen,
		RoseClick,
		RoseRelease,
		HeldFire,
		HeldRose,
		HeldRoseWatch,
		HeldRoseClosed,
		ClickLast,
		Done
	};

	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void next(Phase p_phase);
	void key(const FireGuardIo &p_io, int64_t p_key, bool p_pressed);
	void mouse(const FireGuardIo &p_io, godot::Vector2 p_at, bool p_pressed);
	bool finish();
	void shoot(const FireGuardIo &p_io, const char *p_name) const;

	std::string shot_dir_;
	Phase phase_ = Phase::Wait;
	double clock_ = 0.0;
	double phase_start_ = 0.0;
	uint8_t step_ = 0;
	uint8_t mode_presses_ = 0;
	uint64_t shots_ = 0;
	uint64_t mark_ = 0;
	int32_t mode_ = -1;
	int32_t spawn_mode_ = -1;
	bool seen_ = false;
	bool done_ = false;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
	std::vector<std::string> lines_;
};

}
