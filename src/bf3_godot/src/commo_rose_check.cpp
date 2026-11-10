#include "commo_rose_check.h"

#include "commo_frame.h"
#include "native_hud.h"
#include "native_rose.h"
#include "swf_view.h"

#include "bf3/core/util/format.h"
#include "bf3/sim/commo.h"

#include <cmath>

namespace bf3::godot_side {

namespace ui = bf3::core::ui;

void CommoRoseCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0u : 1u;
	lines_.push_back("COMMO ROSE ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail);
}

bool CommoRoseCheck::step(Bf3NativeHud &r_hud, const CommoRoseIo &p_io, double p_delta) {
	if (done_) {
		return true;
	}
	clock_ += std::isfinite(p_delta) && p_delta > 0.0 ? p_delta : 0.0;
	const auto next = [&](Phase p_phase) {
		phase_ = p_phase;
		phase_start_ = clock_;
	};
	const auto finish = [&] {
		p_io.hold(false);
		phase_ = Phase::Done;
		done_ = true;
		return true;
	};
	if (clock_ > kCommoRoseWaitSeconds) {
		row("rose.timeout", false,
				core::fmt("phase %d not finished within %.0f s; %s", int(phase_), kCommoRoseWaitSeconds,
						r_hud.rose_error().c_str()));
		return finish();
	}
	NativeRose *rose = r_hud.rose();
	const double in_phase = clock_ - phase_start_;
	switch (phase_) {
		case Phase::Wait: {
			if (!p_io.alive || p_io.commo == nullptr || rose == nullptr || !r_hud.is_ready()) {
				phase_start_ = clock_;
				return false;
			}
			if (in_phase < kCommoRoseSettleSeconds) {
				return false;
			}
			for (const ui::CommoRoseEntry &e : rose->layout().entries) {
				if (e.command(false) == int32_t(sim::commo::OrderType::RequestHeal)) {
					want_button_ = e.button;
					want_instance_ = e.instance;
				}
			}
			picks_before_ = p_io.commo->stats().rose_picks;
			orders_before_ = p_io.commo->stats().orders_sent;
			p_io.hold(true);
			next(Phase::Hold);
			return false;
		}
		case Phase::Hold: {
			if (in_phase < kCommoRoseHoldWaitSeconds) {
				return false;
			}
			const bool open =
					p_io.commo->rose_open() && rose->shown() && rose->view() != nullptr && rose->view()->is_visible();
			row("rose.open", open && want_button_ >= 0,
					core::fmt("after %.2f s of held Q: commo frame %s, rose view %s, %llu opens; the NEEDMEDIC entry "
							  "is %s (rose button %d)",
							in_phase, p_io.commo->rose_open() ? "open" : "closed", rose->shown() ? "shown" : "hidden",
							(unsigned long long)rose->opens(), want_instance_.c_str(), want_button_));
			if (!open || want_button_ < 0) {
				return finish();
			}
			const std::optional<godot::Vector2> at = rose->button_offset(want_button_);
			if (!at) {
				row("rose.select", false, "no ComRose clip for the NEEDMEDIC button");
				return finish();
			}
			aim_x_ = at->x * kCommoRoseAimReach;
			aim_y_ = at->y * kCommoRoseAimReach;
			p_io.cursor(aim_x_, aim_y_);
			next(Phase::Aim);
			return false;
		}
		case Phase::Aim: {
			if (in_phase < kCommoRoseAimSeconds) {
				return false;
			}
			const ui::CommoRoseEntry *e = rose->selected_entry();
			const std::optional<sim::commo::OrderType> command = rose_command(e, false);
			row("rose.select",
					e != nullptr && e->button == want_button_ && command == sim::commo::OrderType::RequestHeal,
					core::fmt("cursor (%.1f, %.1f) toward rose button %d selects button %d (%s), command %d", aim_x_,
							aim_y_, want_button_, rose->selected_button(), e != nullptr ? e->instance.c_str() : "none",
							command ? int(*command) : -2));
			p_io.hold(false);
			next(Phase::Release);
			return false;
		}
		case Phase::Release: {
			if (in_phase < kCommoRoseAimSeconds) {
				return false;
			}
			const CommoStats &s = p_io.commo->stats();
			const bool picked = s.rose_picks == picks_before_ + 1u &&
					s.last_pick == int32_t(sim::commo::OrderType::RequestHeal) && s.orders_sent > orders_before_;
			row("rose.pick", picked && !rose->shown() && !p_io.commo->rose_open(),
					core::fmt("released Q: %llu picks, last pick %d, %llu orders sent, rose %s",
							(unsigned long long)(s.rose_picks - picks_before_), s.last_pick,
							(unsigned long long)(s.orders_sent - orders_before_),
							rose->shown() ? "still shown" : "closed"));
			return finish();
		}
		case Phase::Done:
			return true;
	}
	return false;
}

std::string CommoRoseCheck::report() const {
	std::string out;
	for (const std::string &l : lines_) {
		out += l + "\n";
	}
	out += core::fmt("COMMO ROSE CHECK: %s (%u rows, %u failed)",
			done_ && failures_ == 0 && rows_ > 0 ? "PASS" : "FAIL", rows_, failures_);
	return out;
}

}
