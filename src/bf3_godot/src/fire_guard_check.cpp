#include "fire_guard_check.h"

#include "bf3/core/util/format.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>

#include <cmath>

namespace bf3::godot_side {

using namespace godot;
using bf3::core::fmt;

void FireGuardCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0u : 1u;
	lines_.push_back("FIRE GUARD ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail);
}

void FireGuardCheck::next(Phase p_phase) {
	phase_ = p_phase;
	phase_start_ = clock_;
	step_ = 0;
	seen_ = false;
}

void FireGuardCheck::key(const FireGuardIo &p_io, int64_t p_key, bool p_pressed) {
	Ref<InputEventKey> k;
	k.instantiate();
	k->set_keycode(Key(p_key));
	k->set_physical_keycode(Key(p_key));
	k->set_pressed(p_pressed);
	p_io.push(k);
}

void FireGuardCheck::mouse(const FireGuardIo &p_io, Vector2 p_at, bool p_pressed) {
	Ref<InputEventMouseButton> e;
	e.instantiate();
	e->set_position(p_at);
	e->set_global_position(p_at);
	e->set_button_index(MOUSE_BUTTON_LEFT);
	e->set_button_mask(p_pressed ? MOUSE_BUTTON_MASK_LEFT : BitField<MouseButtonMask>(0));
	e->set_pressed(p_pressed);
	p_io.push(e);
}

void FireGuardCheck::shoot(const FireGuardIo &p_io, const char *p_name) const {
	if (!shot_dir_.empty() && p_io.shot) {
		(void)p_io.shot(shot_dir_ + "/" + p_name + ".png");
	}
}

bool FireGuardCheck::finish() {
	phase_ = Phase::Done;
	done_ = true;
	return true;
}

bool FireGuardCheck::step(const FireGuardIo &p_io, double p_delta) {
	if (done_) {
		return true;
	}
	clock_ += std::isfinite(p_delta) && p_delta > 0.0 ? p_delta : 0.0;
	if (clock_ > kFireGuardTimeout) {
		row("timeout", false,
				fmt("phase %d not finished within %.0f s; alive %d, game input %d, mouse captured %d, fire mode %d",
						int(phase_), kFireGuardTimeout, int(p_io.alive), int(p_io.game_input), int(p_io.captured),
						p_io.fire_mode));
		return finish();
	}
	const double in_phase = clock_ - phase_start_;
	const uint64_t shots = p_io.own_shots;
	const auto tap = [&](Vector2 p_at) {
		if (step_ == 0) {
			mouse(p_io, p_at, true);
			step_ = 1;
			return false;
		}
		if (step_ == 1 && in_phase >= kFireGuardTap) {
			mouse(p_io, p_at, false);
			step_ = 2;
		}
		return step_ == 2 && in_phase >= kFireGuardTap + kFireGuardWatch;
	};
	const auto press_key = [&](int64_t p_key) {
		if (step_ == 0) {
			key(p_io, p_key, true);
			step_ = 1;
			return false;
		}
		if (step_ == 1) {
			key(p_io, p_key, false);
			step_ = 2;
		}
		return true;
	};
	const auto watched = [&](bool p_reached) {
		if (p_reached && !seen_) {
			seen_ = true;
			phase_start_ = clock_;
		}
		return seen_ && clock_ - phase_start_ >= kFireGuardWatch;
	};
	const Vector2 corner(2.0f, 2.0f);
	switch (phase_) {
		case Phase::Wait:
			if (p_io.alive && p_io.game_input && !p_io.captured && step_ == 0) {
				mouse(p_io, p_io.centre, true);
				step_ = 1;
				return false;
			}
			if (step_ == 1) {
				mouse(p_io, p_io.centre, false);
				step_ = 0;
				return false;
			}
			if (!p_io.alive || !p_io.game_input || !p_io.captured || p_io.fire_mode < 0 || p_io.fire_mode_key == 0) {
				phase_start_ = clock_;
				return false;
			}
			if (in_phase < kFireGuardSettle) {
				return false;
			}
			spawn_mode_ = p_io.fire_mode;
			shots_ = shots;
			next(Phase::Click);
			return false;
		case Phase::Click:
			if (!tap(p_io.centre)) {
				return false;
			}
			row("click_fires", shots > shots_,
					fmt("a left click fired %llu shots", (unsigned long long)(shots - shots_)));
			next(Phase::ModeKey);
			return false;
		case Phase::ModeKey:
			if (!press_key(p_io.fire_mode_key) || in_phase < kFireGuardWatch) {
				return false;
			}
			mode_ = p_io.fire_mode;
			row("fire_mode_key", mode_ >= 0 && mode_ != spawn_mode_,
					fmt("the fire mode key moved the primary from mode %d to %d", spawn_mode_, mode_));
			shots_ = shots;
			next(Phase::Escape);
			return false;
		case Phase::Escape:
			(void)press_key(KEY_ESCAPE);
			if (!watched(!p_io.game_input)) {
				if (in_phase > kFireGuardOpenWait) {
					row("escape_no_shot", false, "Escape did not open the in-game menu");
					return finish();
				}
				return false;
			}
			row("escape_no_shot", shots == shots_ && p_io.fire_mode == mode_,
					fmt("Escape opened the in-game menu: %llu shots, fire mode %d (was %d)",
							(unsigned long long)(shots - shots_), p_io.fire_mode, mode_));
			shoot(p_io, "escape_menu");
			next(Phase::MenuClick);
			return false;
		case Phase::MenuClick:
			if (!tap(corner)) {
				return false;
			}
			row("menu_click_no_shot", shots == shots_ && !p_io.game_input && p_io.fire_mode == mode_,
					fmt("a left click in the menu: %llu shots, fire mode %d, menu %s",
							(unsigned long long)(shots - shots_), p_io.fire_mode, p_io.game_input ? "closed" : "open"));
			next(Phase::Resume);
			return false;
		case Phase::Resume:
			(void)press_key(KEY_ESCAPE);
			if (!watched(p_io.game_input && p_io.captured)) {
				if (in_phase > kFireGuardOpenWait) {
					row("resume_no_shot", false,
							fmt("Escape did not give the game back: game input %d, mouse captured %d",
									int(p_io.game_input), int(p_io.captured)));
					return finish();
				}
				return false;
			}
			row("resume_no_shot", shots == shots_ && p_io.fire_mode == mode_,
					fmt("Escape closed the menu and took the mouse back: %llu shots, fire mode %d",
							(unsigned long long)(shots - shots_), p_io.fire_mode));
			shoot(p_io, "resume");
			next(Phase::ClickAfterMenu);
			return false;
		case Phase::ClickAfterMenu:
			if (!tap(p_io.centre)) {
				return false;
			}
			row("click_fires_after_menu", shots > shots_,
					fmt("a left click after the menu fired %llu shots", (unsigned long long)(shots - shots_)));
			mode_presses_ = 0;
			next(Phase::ModeBack);
			return false;
		case Phase::ModeBack:
			if (p_io.fire_mode == spawn_mode_ && step_ == 0) {
				shots_ = shots;
				key(p_io, KEY_Q, true);
				next(Phase::RoseOpen);
				return false;
			}
			if (!press_key(p_io.fire_mode_key) || in_phase < kFireGuardTap) {
				return false;
			}
			if (++mode_presses_ > 4) {
				row("fire_mode_back", false, fmt("four fire mode keys did not bring back mode %d", spawn_mode_));
				return finish();
			}
			next(Phase::ModeBack);
			return false;
		case Phase::RoseOpen:
			if (!p_io.rose_open) {
				if (in_phase > kFireGuardOpenWait) {
					row("rose_opens", false,
							fmt("held Q %.1f s, the commo rose did not open; mouse %s, spot key %s", in_phase,
									p_io.captured ? "captured" : "free", p_io.spot_held ? "held" : "not held"));
					return finish();
				}
				return false;
			}
			next(Phase::RoseClick);
			return false;
		case Phase::RoseClick:
			if (!tap(p_io.centre)) {
				return false;
			}
			row("rose_click_no_shot", shots == shots_ && p_io.rose_open,
					fmt("a left click with the commo rose open fired %llu shots, rose %s",
							(unsigned long long)(shots - shots_), p_io.rose_open ? "open" : "closed"));
			shoot(p_io, "rose_click");
			key(p_io, KEY_Q, false);
			next(Phase::RoseRelease);
			return false;
		case Phase::RoseRelease:
			if (in_phase < kFireGuardWatch) {
				return false;
			}
			row("rose_closed_no_shot", shots == shots_ && !p_io.rose_open && p_io.captured,
					fmt("Q released: rose %s, mouse %s, %llu shots since the rose opened",
							p_io.rose_open ? "open" : "closed", p_io.captured ? "captured" : "free",
							(unsigned long long)(shots - shots_)));
			mouse(p_io, p_io.centre, true);
			next(Phase::HeldFire);
			return false;
		case Phase::HeldFire:
			if (in_phase < kFireGuardWatch) {
				return false;
			}
			row("held_fire_fires", shots >= shots_ + 2u,
					fmt("the held left button fired %llu shots in %.1f s in mode %d",
							(unsigned long long)(shots - shots_), kFireGuardWatch, p_io.fire_mode));
			key(p_io, KEY_Q, true);
			next(Phase::HeldRose);
			return false;
		case Phase::HeldRose:
			if (!p_io.rose_open) {
				if (in_phase > kFireGuardOpenWait) {
					row("rose_opens_held", false, fmt("held Q %.1f s, the commo rose did not open", in_phase));
					return finish();
				}
				return false;
			}
			if (!seen_) {
				seen_ = true;
				phase_start_ = clock_;
				return false;
			}
			if (clock_ - phase_start_ < kFireGuardTap) {
				return false;
			}
			mark_ = shots;
			next(Phase::HeldRoseWatch);
			return false;
		case Phase::HeldRoseWatch:
			if (in_phase < kFireGuardWatch) {
				return false;
			}
			row("rose_stops_held_fire", shots == mark_ && p_io.rose_open,
					fmt("the left button held with the commo rose open fired %llu shots in %.1f s",
							(unsigned long long)(shots - mark_), kFireGuardWatch));
			key(p_io, KEY_Q, false);
			next(Phase::HeldRoseClosed);
			return false;
		case Phase::HeldRoseClosed:
			if (in_phase < kFireGuardWatch) {
				return false;
			}
			row("held_fire_waits_for_release", shots == mark_ && !p_io.rose_open,
					fmt("the left button still held after the rose closed fired %llu shots in %.1f s",
							(unsigned long long)(shots - mark_), kFireGuardWatch));
			mouse(p_io, p_io.centre, false);
			shots_ = shots;
			next(Phase::ClickLast);
			return false;
		case Phase::ClickLast:
			if (in_phase < kFireGuardTap) {
				return false;
			}
			if (!tap(p_io.centre)) {
				return false;
			}
			row("click_fires_after_rose", shots > shots_,
					fmt("a left click after the rose fired %llu shots", (unsigned long long)(shots - shots_)));
			return finish();
		case Phase::Done:
			return true;
	}
	return false;
}

std::string FireGuardCheck::report() const {
	std::string out;
	for (const std::string &l : lines_) {
		out += l + "\n";
	}
	out += fmt("FIRE GUARD CHECK: %s (%u rows, %u failed)", done_ && failures_ == 0 && rows_ > 0 ? "PASS" : "FAIL",
			rows_, failures_);
	return out;
}

}
