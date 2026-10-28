#include "fov_round_check.h"

#include "game_settings.h"

#include "bf3/core/ui/settings_effects.h"

#include <cmath>
#include <utility>

namespace bf3::godot_side {

namespace ui = bf3::core::ui;

namespace {

bool same(float p_a, float p_b) {
	return std::abs(p_a - p_b) < kFovRoundCheckTolerance;
}

std::string deg(float p_value) {
	return std::to_string(p_value) + " deg";
}

}

void FovRoundCheck::fail(std::string p_why) {
	failure_ = std::move(p_why);
	step_ = Step::Failed;
}

void FovRoundCheck::frame(GameSettings &r_settings, const FovRoundView &p_view) {
	switch (step_) {
		case Step::FirstHip:
			if (p_view.round_over || !p_view.hip) {
				return;
			}
			level_fov_ = p_view.camera_fov;
			if (!same(level_fov_, r_settings.hip_fov_deg()) ||
					!same(level_fov_, float(r_settings.store().value(ui::kFieldOfViewOption)))) {
				fail("the hip camera FOV " + deg(level_fov_) + " is not the level's FieldOfView " +
						deg(r_settings.hip_fov_deg()));
				return;
			}
			saved_fov_ = float(double(level_fov_) + kFovRoundCheckRaise);
			if (!r_settings.store().set_value(ui::kFieldOfViewOption, double(saved_fov_))) {
				fail("FieldOfView " + deg(saved_fov_) + " is refused by the store");
				return;
			}
			r_settings.store().save();
			step_ = Step::SameRound;
			return;
		case Step::SameRound:
			if (p_view.round_over) {
				step_ = Step::NextRound;
			} else if (p_view.hip) {
				++same_round_frames_;
				same_round_fov_ = p_view.camera_fov;
				if (!same(p_view.camera_fov, level_fov_)) {
					fail("the hip camera FOV moved to " + deg(p_view.camera_fov) +
							" in the round FieldOfView was saved in, before a level load");
				}
			}
			return;
		case Step::NextRound:
			if (p_view.round_over || !p_view.hip) {
				return;
			}
			if (same(p_view.camera_fov, saved_fov_)) {
				step_ = Step::Passed;
			} else if (same(p_view.camera_fov, level_fov_)) {
				++stale_frames_;
			} else {
				fail("the next round's hip camera FOV is " + deg(p_view.camera_fov) + ", not the saved " +
						deg(saved_fov_));
			}
			return;
		case Step::Passed:
		case Step::Failed:
			return;
	}
}

std::string FovRoundCheck::report() const {
	if (step_ == Step::Passed) {
		return "[fov] FOV ROUND: PASS the hip camera FOV " + deg(level_fov_) + " held through " +
				std::to_string(same_round_frames_) + " hip frames (last " + deg(same_round_fov_) +
				") of the round FieldOfView " + deg(saved_fov_) + " was saved in, and is " + deg(saved_fov_) +
				" in the next round";
	}
	if (step_ == Step::Failed) {
		return "[fov] FOV ROUND: FAIL " + failure_;
	}
	static constexpr const char *kSteps[] = { "no hip view in a round", "the round did not end",
		"no hip view at the saved FieldOfView in the next round" };
	return std::string("[fov] FOV ROUND: FAIL ") + kSteps[size_t(step_)] + " (saved " + deg(saved_fov_) + ", " +
			std::to_string(stale_frames_) + " next-round hip frames at the level's " + deg(level_fov_) + ")";
}

}
