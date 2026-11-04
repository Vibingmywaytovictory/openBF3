#include "net_overlay_check.h"

#include "bf3/core/ui/debug_screen.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/util/format.h"

namespace bf3::godot_side {

namespace {

namespace ui = bf3::core::ui;
using bf3::core::fmt;

}

void NetOverlayCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0u : 1u;
	lines_.push_back("[net-overlay] " + p_name + ": " + (p_pass ? "PASS" : "FAIL") + " (" + p_detail + ")");
}

void NetOverlayCheck::enter(Phase p_phase, double p_now_s) {
	phase_ = p_phase;
	phase_s_ = p_now_s;
}

void NetOverlayCheck::compare(const NetOverlay &p_overlay) {
	ui::HudScreen *hud = p_overlay.screen();
	if (hud == nullptr) {
		++mismatches_;
		if (first_mismatch_.empty()) {
			first_mismatch_ = "the debugscreen is not open";
		}
		return;
	}
	bool hidden = false, off_colour = false;
	for (size_t i = 0; i < expected_.size() && i < ui::kDebugScreenFields; ++i) {
		const std::string path = ui::debug_field_path(i);
		const ui::As2Clip field = hud->clip(path);
		const ui::As2Clip label = hud->clip(path + ".txtLabel");
		const ui::As2Clip value = hud->clip(path + ".txtValue");
		const ui::SwfSceneNode *node = field.node();
		hidden = hidden || node == nullptr || !node->world_visible;
		off_colour = off_colour || value.text_color() != std::optional<uint32_t>(ui::kDebugFieldRgb) ||
				label.text_color() != std::optional<uint32_t>(ui::kDebugFieldRgb);
		const std::string want_label = expected_[i].label + ":";
		if (label.text() != want_label || value.text() != expected_[i].value) {
			++mismatches_;
			if (first_mismatch_.empty()) {
				first_mismatch_ = fmt("revision %llu field %zu draws '%s' '%s', the frame's NetStats give '%s' '%s'",
						(unsigned long long)p_overlay.revision(), i + 1, label.text().c_str(), value.text().c_str(),
						want_label.c_str(), expected_[i].value.c_str());
			}
			return;
		}
	}
	hidden_frames_ += hidden ? 1u : 0u;
	off_colour_frames_ += off_colour ? 1u : 0u;
}

void NetOverlayCheck::finish_compare(const NetOverlay &p_overlay, const bf3::host::NetStats &p_net,
		const std::optional<bf3::audio::SoundFateTotals> &p_sounds) {
	row("fields_equal_netstats",
			frames_ >= kNetOverlayCheckFrames && revisions_ >= kNetOverlayCheckRevisions && mismatches_ == 0,
			fmt("%u frames over %u NetStats revisions, %u frames whose drawn DebugField text differs from the frame's "
				"NetStats and ledger%s%s",
					frames_, revisions_, mismatches_, first_mismatch_.empty() ? "" : ": ", first_mismatch_.c_str()));
	row("fields_drawn", hidden_frames_ == 0 && off_colour_frames_ == 0 && expected_.size() == p_overlay.fields().size(),
			fmt("%zu DebugFields of %zu; %u frames with a field not drawn, %u frames with a field not in the AS2's red "
				"%06X",
					expected_.size(), ui::kDebugScreenFields, hidden_frames_, off_colour_frames_,
					unsigned(ui::kDebugFieldRgb)));
	row("netstats_live",
			p_net.in.total_bytes_per_s > 0.0f && p_net.out.total_bytes_per_s > 0.0f && p_net.snapshots_per_s > 0.0f &&
					p_net.server_tick > 0,
			fmt("in %.0f B/s, out %.0f B/s, %.1f snapshots/s, rtt %.1f ms, server tick %u",
					double(p_net.in.total_bytes_per_s), double(p_net.out.total_bytes_per_s),
					double(p_net.snapshots_per_s), double(p_net.rtt_ms), unsigned(p_net.server_tick)));
	const uint64_t requested = p_sounds ? p_sounds->requested() : 0;
	row("ledger_counted", p_sounds.has_value() && requested > 0,
			p_sounds ? fmt("the sound fate ledger holds %llu requests, %llu dropped by data, %llu dropped otherwise",
							   (unsigned long long)requested, (unsigned long long)p_sounds->data_drops(),
							   (unsigned long long)p_sounds->other_drops())
					 : std::string("no audio, so no ledger"));
}

bool NetOverlayCheck::step(double p_now_s, const NetOverlay &p_overlay, const bf3::host::NetStats &p_net,
		const std::optional<bf3::audio::SoundFateTotals> &p_sounds, const NetOverlayCheckIo &p_io) {
	if (done_) {
		return true;
	}
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
		phase_s_ = p_now_s;
	}
	const double in = p_now_s - phase_s_;
	switch (phase_) {
		case Phase::Deploy:
			if (p_io.alive) {
				enter(Phase::Hidden, p_now_s);
			} else if (deploy_s_ < 0.0 || p_now_s - deploy_s_ >= kNetOverlayCheckDeploySeconds) {
				deploy_s_ = p_now_s;
				p_io.deploy();
			}
			break;
		case Phase::Hidden:
			if (in >= kNetOverlayCheckSettleSeconds) {
				row("hidden_by_default", p_overlay.attached() && !p_overlay.shown() && !p_overlay.opened(),
						fmt("overlay layer %s, %s, debugscreen %s", p_overlay.attached() ? "attached" : "missing",
								p_overlay.shown() ? "shown" : "hidden", p_overlay.opened() ? "open" : "not open"));
				p_io.toggle_key();
				enter(Phase::Opening, p_now_s);
			}
			break;
		case Phase::Opening:
			if (p_overlay.shown() && p_overlay.opened()) {
				row("f9_opens", p_overlay.error().empty() && !p_overlay.fields().empty(),
						fmt("F9 opened %s with %zu DebugFields, %.2f s after the key",
								std::string(ui::kDebugScreenMovie).c_str(), p_overlay.fields().size(), in));
				seen_revision_ = p_overlay.revision();
				enter(Phase::Compare, p_now_s);
			} else if (in >= kNetOverlayCheckOpenSeconds) {
				row("f9_opens", false,
						fmt("overlay %s, debugscreen %s after %.1f s%s%s", p_overlay.shown() ? "shown" : "hidden",
								p_overlay.opened() ? "open" : "not open", in, p_overlay.error().empty() ? "" : ": ",
								p_overlay.error().c_str()));
				enter(Phase::Done, p_now_s);
			}
			break;
		case Phase::Compare:
			if (p_overlay.revision() != seen_revision_) {
				seen_revision_ = p_overlay.revision();
				expected_ = net_overlay_fields(p_net, p_sounds);
				++revisions_;
			}
			if (revisions_ > 0) {
				++frames_;
				compare(p_overlay);
				if (!shot_taken_ && frames_ >= kNetOverlayCheckFrames / 2 && !shots_dir_.empty() && p_io.shot) {
					shot_taken_ = true;
					const std::string path = shots_dir_ + "/net_overlay.png";
					lines_.push_back("[net-overlay] shot " + path + (p_io.shot(path) ? "" : " FAILED"));
				}
			}
			if ((frames_ >= kNetOverlayCheckFrames && revisions_ >= kNetOverlayCheckRevisions) ||
					in >= kNetOverlayCheckCompareSeconds) {
				finish_compare(p_overlay, p_net, p_sounds);
				p_io.toggle_key();
				enter(Phase::Hiding, p_now_s);
			}
			break;
		case Phase::Hiding:
			if (in >= kNetOverlayCheckHideSeconds) {
				row("f9_hides", !p_overlay.shown(),
						std::string("the second F9 left the overlay ") + (p_overlay.shown() ? "shown" : "hidden"));
				enter(Phase::Done, p_now_s);
			}
			break;
		case Phase::Done:
			done_ = true;
			break;
	}
	if (!done_ && p_now_s - start_s_ > kNetOverlayCheckSeconds) {
		row("phase " + std::to_string(int(phase_)), false, fmt("timed out after %.1f s", p_now_s - start_s_));
		done_ = true;
	}
	return done_;
}

std::string NetOverlayCheck::report() const {
	std::string out;
	for (const std::string &l : lines_) {
		out += l + "\n";
	}
	out += fmt("[net-overlay] %u rows, %u failed\n", rows_, failures_);
	out += std::string("NET OVERLAY: ") + (exit_code() == 0 ? "PASS" : "FAIL");
	return out;
}

}
