#include "icons3d_check.h"

#include "native_hud.h"
#include "native_tags.h"
#include "ui_stage.h"

#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/icon3d_placement.h"
#include "bf3/core/ui/ui_stage.h"
#include "bf3/core/util/format.h"

#include <godot_cpp/classes/camera3d.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

constexpr std::string_view kAlertManagerPath = "instance1.AlertManager_01";
constexpr float kPi = 3.14159265358979f;

const NativeAlert *first_alert(const Bf3NativeHud &p_hud, bool p_snapped) {
	const NativeTags *t = p_hud.tags();
	if (t == nullptr) {
		return nullptr;
	}
	for (const NativeAlert &a : t->alerts()) {
		if (a.placement.snapped == p_snapped) {
			return &a;
		}
	}
	return nullptr;
}

std::string tag_path(uint32_t p_id) {
	return std::string(kAlertManagerPath) + ".tag" + std::to_string(p_id);
}

bool snap_flag(const Bf3NativeHud &p_hud, uint32_t p_id) {
	for (const ui::HudWorldTag &t : p_hud.world_tags().tags) {
		if (t.id != p_id) {
			continue;
		}
		for (const ui::HudDataEntry &e : t.data) {
			if (e.key == "Snap") {
				return e.value.truthy();
			}
		}
	}
	return false;
}

}

void Icons3dCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0u : 1u;
	lines_.push_back("ICONS3D ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail);
}

bool Icons3dCheck::front_row(Bf3NativeHud &r_hud) {
	const NativeAlert *a = first_alert(r_hud, false);
	const Camera3D *cam = r_hud.world_camera();
	if (a == nullptr || cam == nullptr || r_hud.tags() == nullptr) {
		return false;
	}
	const ui::Icon3dStyle &style = r_hud.tags()->alert_style();
	const Vector3 at(a->position[0], a->position[1], a->position[2]);
	const Vector2 w = cam->unproject_position(at);
	const ui::UiStage stage = ui_stage(&r_hud);
	const ui::SwfPoint want = ui::ui_window_to_stage(stage, ui::SwfPoint{ w.x, w.y });
	const float distance = cam->get_global_transform().origin.distance_to(at);
	const float want_scale = 100.0f * ui::icon3d_scale(style, distance);
	const float want_alpha = 100.0f * ui::icon3d_alpha(style, distance);
	const std::optional<NativeHudClipState> manager = r_hud.clip_state(kNativeHudSharedView, kAlertManagerPath);
	const std::optional<NativeHudClipState> tag = r_hud.clip_state(kNativeHudSharedView, tag_path(a->id));
	if (!manager || !tag) {
		++front_frames_;
		++front_bad_;
		if (front_bad_ == 1) {
			front_detail_ = core::fmt("grenade (slot %u, shot %u) has an alert but no AlertManager_01.tag%u clip",
					unsigned(a->slot), unsigned(a->shot_index), unsigned(a->id));
		}
		return true;
	}
	if (const std::optional<NativeHudClipState> icon =
					r_hud.clip_state(kNativeHudSharedView, tag_path(a->id) + ".grIcon")) {
		normal_frame_ = icon->frame;
	}
	grenade_slot_ = a->slot;
	grenade_shot_ = a->shot_index;
	const std::optional<ui::SwfRect> drawn = r_hud.window_bounds(kNativeHudSharedView, tag_path(a->id));
	const float gx = manager->x + tag->x;
	const float gy = manager->y + tag->y;
	const bool covers = drawn && w.x >= drawn->x_min - 1.0f && w.x <= drawn->x_max + 1.0f &&
			w.y >= drawn->y_min - 1.0f && w.y <= drawn->y_max + 1.0f;
	const bool ok = style.from_data && tag->visible && std::fabs(gx - want.x) <= kIcons3dStageTolerance &&
			std::fabs(gy - want.y) <= kIcons3dStageTolerance &&
			std::fabs(tag->xscale - want_scale) <= kIcons3dPercentTolerance &&
			std::fabs(tag->alpha - want_alpha) <= kIcons3dPercentTolerance && covers;
	tag_id_ = a->id;
	front_worst_px_ = std::max({ front_worst_px_, std::fabs(gx - want.x), std::fabs(gy - want.y) });
	++front_frames_;
	front_bad_ += ok ? 0u : 1u;
	front_near_ = std::min(front_near_, distance);
	front_far_ = std::max(front_far_, distance);
	front_min_alpha_ = std::min(front_min_alpha_, tag->alpha);
	front_min_scale_ = std::min(front_min_scale_, tag->xscale);
	if ((!ok && front_bad_ == 1) || (front_bad_ == 0 && distance >= front_far_)) {
		front_detail_ = core::fmt("grenade (slot %u, shot %u) %.2f m away: AlertManager_01.tag%u at stage (%.1f, "
								  "%.1f), its own projection (%.1f, %.1f); "
								  "_xscale %.1f wants %.1f, _alpha %.1f wants %.1f (UIAlerttagComp %s); drawn %s the "
								  "projected pixel (%.0f, %.0f)",
				unsigned(a->slot), unsigned(a->shot_index), double(distance), unsigned(a->id), double(gx), double(gy),
				double(want.x), double(want.y), double(tag->xscale), double(want_scale), double(tag->alpha),
				double(want_alpha), style.from_data ? "from data" : "defaults",
				drawn ? (covers ? "over" : "away from") : "nothing at", double(w.x), double(w.y));
	}
	return true;
}
bool Icons3dCheck::snap_row(Bf3NativeHud &r_hud) {
	const NativeAlert *a = first_alert(r_hud, true);
	if (a == nullptr || r_hud.tags() == nullptr) {
		return false;
	}
	const std::optional<NativeHudClipState> manager = r_hud.clip_state(kNativeHudSharedView, kAlertManagerPath);
	const std::optional<NativeHudClipState> tag = r_hud.clip_state(kNativeHudSharedView, tag_path(a->id));
	if (!manager || !tag) {
		return false;
	}
	const ui::SwfRect box = ui::icon3d_snap_box(r_hud.tags()->alert_style(), ui_stage(&r_hud).visible);
	const float gx = manager->x + tag->x;
	const float gy = manager->y + tag->y;
	const float edge = std::min({ std::fabs(gx - box.x_min), std::fabs(gx - box.x_max), std::fabs(gy - box.y_min),
			std::fabs(gy - box.y_max) });
	const bool inside = gx >= box.x_min - kIcons3dStageTolerance && gx <= box.x_max + kIcons3dStageTolerance &&
			gy >= box.y_min - kIcons3dStageTolerance && gy <= box.y_max + kIcons3dStageTolerance;
	const bool flag = snap_flag(r_hud, a->id);
	const std::optional<NativeHudClipState> icon = r_hud.clip_state(kNativeHudSharedView, tag_path(a->id) + ".grIcon");
	const bool reacted = icon && icon->frame != normal_frame_;
	const bool ok = tag->visible && inside && edge <= kIcons3dStageTolerance && flag && reacted && !a->in_front;
	row("icons3d.snap", ok,
			core::fmt("turned away (grenade %s, %.2f m): tag%u at stage (%.1f, %.1f), %.2f from the SnapSafeZone box "
					  "(%.0f..%.0f, %.0f..%.0f), Snap %s, grIcon frame %u (normal %u)",
					a->in_front ? "in front" : "behind", double(a->distance_m), unsigned(a->id), double(gx), double(gy),
					double(edge), double(box.x_min), double(box.x_max), double(box.y_min), double(box.y_max),
					flag ? "true" : "false", icon ? unsigned(icon->frame) : 0u, unsigned(normal_frame_)));
	return true;
}

bool Icons3dCheck::step(Bf3NativeHud &r_hud, const Icons3dIo &p_io, double p_delta) {
	if (done_) {
		return true;
	}
	clock_ += std::isfinite(p_delta) && p_delta > 0.0 ? p_delta : 0.0;
	const auto next = [&](Phase p_phase) {
		phase_ = p_phase;
		phase_start_ = clock_;
		frames_ = 0;
	};
	const auto finish = [&] {
		phase_ = Phase::Done;
		done_ = true;
		return true;
	};
	if (clock_ > kIcons3dWaitSeconds) {
		row("icons3d.timeout", false,
				core::fmt("phase %d not finished within %.0f s", int(phase_), kIcons3dWaitSeconds));
		return finish();
	}
	const double in_phase = clock_ - phase_start_;
	switch (phase_) {
		case Phase::Wait:
			if (!p_io.alive || r_hud.tags() == nullptr || !r_hud.tags()->loaded() || r_hud.world_camera() == nullptr) {
				phase_start_ = clock_;
				return false;
			}
			if (in_phase < kIcons3dSettleSeconds) {
				return false;
			}
			yaw_ = p_io.yaw;
			p_io.view(yaw_, kIcons3dThrowPitch);
			p_io.throw_grenade();
			next(Phase::Throw);
			return false;
		case Phase::Throw:
			p_io.view(yaw_, kIcons3dThrowPitch);
			if (first_alert(r_hud, false) == nullptr) {
				if (in_phase > kIcons3dFrontSeconds) {
					row("icons3d.front", false,
							core::fmt(
									"no grenade alert tag in front within %.1f s of the throw", kIcons3dFrontSeconds));
					return finish();
				}
				return false;
			}
			next(Phase::Front);
			return false;
		case Phase::Front:
			p_io.view(yaw_, kIcons3dThrowPitch);
			if (++frames_ < kIcons3dSettleFrames) {
				return false;
			}
			front_row(r_hud);
			if (in_phase < kIcons3dFrontSeconds && first_alert(r_hud, true) == nullptr && r_hud.tags() != nullptr &&
					!r_hud.tags()->alerts().empty()) {
				return false;
			}
			row("icons3d.front", front_frames_ > 0 && front_bad_ == 0,
					core::fmt("%u frames with the alert in front, %u off, worst %.2f stage px off its projection; "
							  "distance %.2f..%.2f m, lowest _xscale %.1f, "
							  "lowest _alpha %.1f; last or worst: %s",
							front_frames_, front_bad_, double(front_worst_px_), double(front_near_), double(front_far_),
							double(front_min_scale_), double(front_min_alpha_), front_detail_.c_str()));
			if (front_frames_ == 0) {
				return finish();
			}
			p_io.view(yaw_ + kPi, 0.0f);
			next(Phase::Turn);
			return false;
		case Phase::Turn:
			p_io.view(yaw_ + kPi, 0.0f);
			if (++frames_ < kIcons3dSettleFrames) {
				return false;
			}
			if (!snap_row(r_hud)) {
				if (in_phase > kIcons3dFrontSeconds) {
					row("icons3d.snap", false,
							"no snapped alert after turning away (detonated or beyond DrawDistance)");
					return finish();
				}
				return false;
			}
			next(Phase::Gone);
			return false;
		case Phase::Gone: {
			p_io.view(yaw_ + kPi, 0.0f);
			const bool live = !p_io.grenade_live || p_io.grenade_live(grenade_slot_, grenade_shot_);
			const std::optional<NativeHudClipState> tag = r_hud.clip_state(kNativeHudSharedView, tag_path(tag_id_));
			const bool shown = tag && tag->visible;
			if ((live || shown) && in_phase < kIcons3dGoneSeconds) {
				return false;
			}
			row("icons3d.gone", !live && !shown && p_io.alive,
					core::fmt("grenade (slot %u, shot %u) %s, soldier %s; its alert tag%u is %s (%.1f s after the snap "
							  "row)",
							unsigned(grenade_slot_), unsigned(grenade_shot_),
							live ? "still live" : "gone from the client", p_io.alive ? "alive" : "dead",
							unsigned(tag_id_), shown ? "still shown" : (tag ? "hidden" : "removed"), in_phase));
			return finish();
		}
		case Phase::Done:
			return finish();
	}
	return finish();
}

std::string Icons3dCheck::report() const {
	std::string out;
	for (const std::string &l : lines_) {
		out += l + "\n";
	}
	out += core::fmt("ICONS3D CHECK: %s (%u rows, %u failed)", done_ && failures_ == 0 && rows_ > 0 ? "PASS" : "FAIL",
			rows_, failures_);
	return out;
}

}
