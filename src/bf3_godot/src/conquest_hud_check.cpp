#include "conquest_hud_check.h"

#include "native_hud.h"

#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/util/format.h"

#include <cmath>
#include <optional>

namespace bf3::godot_side {

namespace ui = bf3::core::ui;

ConquestHudCheck::ConquestHudCheck(std::string p_shot) : shot_(std::move(p_shot)) {}

void ConquestHudCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0u : 1u;
	lines_.push_back("CONQUEST HUD ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail);
}

void ConquestHudCheck::verify(Bf3NativeHud &r_hud, const ConquestHudIo &p_io) {
	const UiScreenSlot slot = r_hud.screen_slot(1);
	const bool conquest = slot.partition == ui::kHudConquestScreenPartition && slot.screen != nullptr && slot.shown;
	row("screen", conquest,
			"in-round mode screen " + std::string(slot.partition) + (slot.shown ? " shown" : " hidden"));
	row("spawn_counter", saw_spawn_counter_, "spawnscreenticketcounterconquestscreen drawn on the deploy screen");
	if (!conquest) {
		return;
	}
	const ui::HudModel &m = r_hud.model();
	const ui::As2Value status = ui::hud_objective_status(m.capture_points.capturepoints_status_data);
	row("model", status.size() > 0 && m.tickets.global_max_tickets > 0,
			core::fmt("%zu flags, tickets %d / %d of %d", status.size(), m.tickets.friendly_team_tickets,
					m.tickets.enemy_team_tickets, m.tickets.global_max_tickets));
	const std::string tc = "instance1.TicketCounter_01";
	const std::string friendly = slot.screen->clip(tc + ".tickets1.tickets").text();
	const std::string enemy = slot.screen->clip(tc + ".tickets0.tickets").text();
	const std::optional<NativeHudClipState> counter = r_hud.clip_state(1, tc);
	const bool counter_ok = counter && counter->visible &&
			friendly == std::to_string(m.tickets.friendly_team_tickets) &&
			enemy == std::to_string(m.tickets.enemy_team_tickets);
	row("tickets", counter_ok, "TicketCounter friendly " + friendly + ", enemy " + enemy);
	const std::string bar = "instance1.ObjectiveBar_01";
	const std::optional<NativeHudClipState> bar_state = r_hud.clip_state(1, bar);
	uint32_t shown = 0;
	std::string letters;
	for (size_t i = 0; i < status.size(); ++i) {
		const std::string icon = bar + ".conquest.icon" + std::to_string(i);
		const std::optional<NativeHudClipState> s = r_hud.clip_state(1, icon);
		shown += s && s->visible ? 1u : 0u;
		letters += slot.screen->clip(icon + ".label.nameTxt").text();
	}
	std::string expected;
	for (size_t i = 0; i < status.size(); ++i) {
		expected += status.at(i).field("Label").to_string();
	}
	row("objective_bar", bar_state && bar_state->visible && shown == status.size() && letters == expected,
			core::fmt("%u of %zu flag icons shown, letters %s", shown, status.size(), letters.c_str()));
	const NativeHudPaintStats &paint = r_hud.paint_stats();
	row("minimap_flags", paint.minimap_flags > 0 && paint.minimap_letters > 0,
			core::fmt(
					"%u flag icons and %u letters painted on the minimap", paint.minimap_flags, paint.minimap_letters));
	if (!shot_.empty() && p_io.screenshot) {
		row("shot", p_io.screenshot(shot_), shot_);
	}
}

bool ConquestHudCheck::step(Bf3NativeHud &r_hud, const ConquestHudIo &p_io, double p_delta) {
	if (done_) {
		return true;
	}
	clock_ += std::isfinite(p_delta) && p_delta > 0.0 ? p_delta : 0.0;
	saw_spawn_counter_ = saw_spawn_counter_ || p_io.spawn_ticket_counter;
	if (clock_ > kConquestHudWaitSeconds) {
		row("timeout", false, core::fmt("phase %d not finished within %.0f s", int(phase_), kConquestHudWaitSeconds));
		done_ = true;
		return true;
	}
	if (!r_hud.is_ready() || r_hud.has_failed()) {
		return false;
	}
	if (!p_io.alive) {
		frames_ = 0;
		phase_ = Phase::Deploy;
		if (p_io.spawn_screen && p_io.deploy && clock_ - deploy_clock_ > kConquestHudRedeploySeconds) {
			p_io.deploy();
			deploy_clock_ = clock_;
			++deploys_;
		}
		return false;
	}
	phase_ = Phase::Settle;
	if (++frames_ < kConquestHudSettleFrames) {
		return false;
	}
	verify(r_hud, p_io);
	phase_ = Phase::Done;
	done_ = true;
	return true;
}

std::string ConquestHudCheck::report() const {
	std::string out;
	for (const std::string &l : lines_) {
		out += l + "\n";
	}
	out += core::fmt("CONQUEST HUD: %s (%u rows, %u failed, %u deploys)", done_ && failures_ == 0 ? "PASS" : "FAIL",
			rows_, failures_, deploys_);
	return out;
}

}
