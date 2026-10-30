#include "minimap_modes_check.h"

#include "minimap_hud.h"
#include "native_hud.h"
#include "overview_map.h"

#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/ui_input.h"
#include "bf3/core/util/format.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

struct Pixels {
	int32_t width = 0;
	int32_t height = 0;
	std::vector<uint8_t> rgba;
};

std::optional<Pixels> read_target(RID p_texture) {
	if (!p_texture.is_valid()) {
		return std::nullopt;
	}
	Ref<Image> img = RenderingServer::get_singleton()->texture_2d_get(p_texture);
	if (img.is_null() || img->is_empty()) {
		return std::nullopt;
	}
	if (img->get_format() != Image::FORMAT_RGBA8) {
		img->convert(Image::FORMAT_RGBA8);
	}
	Pixels p;
	p.width = img->get_width();
	p.height = img->get_height();
	const PackedByteArray bytes = img->get_data();
	p.rgba.assign(bytes.ptr(), bytes.ptr() + bytes.size());
	return p;
}

std::optional<ui::MinimapUv> decode_at(
		const Pixels &p_pixels, const MinimapDebugFrame &p_frame, float p_local_x, float p_local_y) {
	const ui::SwfRect &b = p_frame.local_bounds;
	if (!(b.width() > 0.0f) || !(b.height() > 0.0f) || p_pixels.width <= 0 || p_pixels.height <= 0) {
		return std::nullopt;
	}
	const float fx = (p_local_x - b.x_min) * float(p_pixels.width) / b.width();
	const float fy = (p_local_y - b.y_min) * float(p_pixels.height) / b.height();
	const auto texel = [&](int32_t p_x, int32_t p_y) -> std::optional<ui::MinimapUv> {
		if (p_x < 0 || p_y < 0 || p_x >= p_pixels.width || p_y >= p_pixels.height) {
			return std::nullopt;
		}
		const size_t at = (size_t(p_y) * size_t(p_pixels.width) + size_t(p_x)) * 4;
		if (p_pixels.rgba[at + 3] != 255) {
			return std::nullopt;
		}
		return ui::decode_minimap_debug_uv(
				ui::MinimapDebugUvPixel{ p_pixels.rgba[at], p_pixels.rgba[at + 1], p_pixels.rgba[at + 2] });
	};
	const int32_t x0 = int32_t(std::floor(fx - 0.5f));
	const int32_t y0 = int32_t(std::floor(fy - 0.5f));
	const float tx = fx - 0.5f - float(x0);
	const float ty = fy - 0.5f - float(y0);
	const std::optional<ui::MinimapUv> c00 = texel(x0, y0), c10 = texel(x0 + 1, y0), c01 = texel(x0, y0 + 1),
									   c11 = texel(x0 + 1, y0 + 1);
	if (c00 && c10 && c01 && c11) {
		const auto mix = [&](float a, float b, float c, float d) {
			return (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (c * (1.0f - tx) + d * tx) * ty;
		};
		return ui::MinimapUv{ mix(c00->u, c10->u, c01->u, c11->u), mix(c00->v, c10->v, c01->v, c11->v) };
	}
	return texel(int32_t(std::floor(fx)), int32_t(std::floor(fy)));
}

ui::ScreenPoint local_of_pixel(const MinimapDebugFrame &p_frame, int32_t p_x, int32_t p_y, const Pixels &p_pixels) {
	const ui::SwfRect &b = p_frame.local_bounds;
	return { b.x_min + (float(p_x) + 0.5f) * b.width() / float(p_pixels.width),
		b.y_min + (float(p_y) + 0.5f) * b.height() / float(p_pixels.height) };
}

}

MinimapModesCheck::MinimapModesCheck(std::string p_level, std::string p_mode) :
		level_(std::move(p_level)), mode_(std::move(p_mode)) {}

void MinimapModesCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0u : 1u;
	lines_.push_back("MINIMAP MODES ROW " + (level_.empty() ? std::string() : level_ + ".") + p_name + " " +
			(p_pass ? "PASS" : "FAIL") + " " + p_detail);
}

bool MinimapModesCheck::load(Bf3NativeHud &r_hud) {
	if (source_ == nullptr && r_hud.is_inside_tree()) {
		const TypedArray<Node> found = r_hud.get_tree()->get_root()->find_children("*", "Bf3MinimapHud", true, false);
		if (!found.is_empty()) {
			source_ = Object::cast_to<Bf3MinimapHud>(found[0]);
		}
	}
	if (source_ == nullptr || !source_->ready_to_draw()) {
		return false;
	}
	const std::shared_ptr<const MinimapAssets> a = source_->assets();
	const std::optional<ui::L8Image> field = source_->combat_area_field();
	if (a == nullptr || !field) {
		return false;
	}
	mapping_ = ui::minimap_mapping(a->level.data);
	satellite_side_ = a->satellite_side;
	threshold_ = a->level.data.combat_area_alpha_threshold;
	field_ = *field;
	objectives_.clear();
	for (const ui::MinimapObjective &o : a->level.objectives) {
		MinimapObjectiveTally t;
		t.world_x = o.position.x;
		t.world_z = o.position.z;
		t.hq = o.kind == ui::ObjectiveKind::Hq;
		objectives_.push_back(t);
	}
	return true;
}

void MinimapModesCheck::tally(const MinimapDebugFrame &p_frame, std::map<size_t, MinimapObjectiveTally> &r_tally,
		bool p_only_target, size_t p_target) {
	const std::optional<Pixels> pixels = read_target(p_frame.texture);
	for (const MinimapDebugHit &h : p_frame.hits) {
		if (h.kind != MinimapDebugKind::Objective) {
			continue;
		}
		size_t best = objectives_.size();
		float best_d = std::numeric_limits<float>::max();
		for (size_t k = 0; k < objectives_.size(); ++k) {
			const float d = std::hypot(objectives_[k].world_x - h.world_x, objectives_[k].world_z - h.world_z);
			if (d < best_d) {
				best_d = d;
				best = k;
			}
		}
		if (best >= objectives_.size() || best_d > 0.5f || (p_only_target && best != p_target)) {
			continue;
		}
		MinimapObjectiveTally &t = r_tally[best];
		t.world_x = objectives_[best].world_x;
		t.world_z = objectives_[best].world_z;
		t.hq = objectives_[best].hq;
		++t.drawn;
		const ui::MinimapUv want = ui::world_to_minimap_uv(mapping_, t.world_x, t.world_z);
		const std::optional<ui::MinimapUv> got =
				pixels ? decode_at(*pixels, p_frame, h.local_x, h.local_y) : std::nullopt;
		const float err =
				got ? ui::minimap_uv_error_texels(*got, want, satellite_side_) : std::numeric_limits<float>::infinity();
		const bool inside = got && ui::sdf_inside(field_.at_uv(*got), threshold_);
		t.worst_texels = std::max(t.worst_texels, err);
		t.outside_mask += inside ? 0u : 1u;
		t.good += err <= ui::kMinimapUvCheckTexels && inside ? 1u : 0u;
	}
}

std::optional<ui::MinimapUv> MinimapModesCheck::uv_at(
		const MinimapDebugFrame &p_frame, float p_local_x, float p_local_y) const {
	const std::optional<Pixels> pixels = read_target(p_frame.texture);
	return pixels ? decode_at(*pixels, p_frame, p_local_x, p_local_y) : std::nullopt;
}

void MinimapModesCheck::objective_rows(
		const std::string &p_view, const std::map<size_t, MinimapObjectiveTally> &p_tally) {
	size_t ok = 0, hqs = 0;
	float worst = 0.0f;
	std::string missing;
	for (size_t k = 0; k < objectives_.size(); ++k) {
		const auto it = p_tally.find(k);
		const bool good = it != p_tally.end() && it->second.drawn > 0 && it->second.good == it->second.drawn;
		ok += good ? 1u : 0u;
		hqs += good && objectives_[k].hq ? 1u : 0u;
		if (it != p_tally.end()) {
			worst = std::max(worst, it->second.worst_texels);
		}
		if (!good) {
			missing += core::fmt(" #%zu%s(%s)", k, objectives_[k].hq ? " HQ" : "",
					it == p_tally.end() ? "not drawn"
										: core::fmt("%u/%u, %.2f texels, %u outside", it->second.good, it->second.drawn,
												  double(it->second.worst_texels), it->second.outside_mask)
												  .c_str());
		}
	}
	row(mode_ + "." + p_view + ".objectives", !objectives_.empty() && ok == objectives_.size(),
			core::fmt("%zu of %zu flags and HQs (%zu HQs) drawn with their centre's debug UV within %.1f satellite "
					  "texel of the "
					  "objective's UV and the mode's mask inside there; worst %.2f texels",
					ok, objectives_.size(), hqs, double(ui::kMinimapUvCheckTexels), double(worst)) +
					missing);
}

void MinimapModesCheck::whole_view_row(const MinimapDebugFrame &p_frame) {
	const std::optional<ui::MinimapWorldRect> area = source_ != nullptr ? source_->combat_area_rect() : std::nullopt;
	float centre_err = -1.0f;
	float local_err = -1.0f;
	if (area) {
		const std::optional<ui::MinimapUv> centre = uv_at(p_frame, p_frame.view.center_x, p_frame.view.center_y);
		const ui::MinimapUv want =
				ui::world_to_minimap_uv(mapping_, 0.5f * (area->x0 + area->x1), 0.5f * (area->z0 + area->z1));
		centre_err = centre ? ui::minimap_uv_error_texels(*centre, want, satellite_side_) : -1.0f;
	}
	for (const MinimapDebugHit &h : p_frame.hits) {
		if (h.kind == MinimapDebugKind::Local) {
			const std::optional<ui::MinimapUv> got = uv_at(p_frame, h.local_x, h.local_y);
			local_err = got ? ui::minimap_uv_error_texels(
									  *got, ui::world_to_minimap_uv(mapping_, h.world_x, h.world_z), satellite_side_)
							: -1.0f;
		}
	}
	const bool ok = !p_frame.view.rotate && p_frame.view.heading == 0.0f && centre_err >= 0.0f &&
			centre_err <= ui::kMinimapUvCheckTexels && local_err >= 0.0f && local_err <= ui::kMinimapUvCheckTexels;
	row(mode_ + ".minimap_large_whole.view", ok,
			core::fmt("the large map at its start step is %s, heading %.2f, %.3f m per Minimap_01 unit; its centre "
					  "reads the "
					  "combat area rect's centre UV within %.2f texels (the overview's fit of that rect, an inference "
					  "for this "
					  "step) and the own marker sits at its own UV within %.2f texels",
					p_frame.view.rotate ? "rotating" : "north-up", double(p_frame.view.heading),
					double(p_frame.metres_per_unit), double(centre_err), double(local_err)));
}

bool MinimapModesCheck::sweep(Bf3NativeHud &r_hud, std::map<size_t, MinimapObjectiveTally> &r_tally) {
	if (sweep_index_ >= objectives_.size()) {
		r_hud.set_minimap_pose_override(std::nullopt);
		return true;
	}
	const MinimapObjectiveTally &o = objectives_[sweep_index_];
	const float angle = 0.7f + 1.3f * float(sweep_index_);
	NativeHudMinimapPose pose;
	pose.x = o.world_x + kMinimapModesOffsetMetres * std::cos(angle);
	pose.z = o.world_z + kMinimapModesOffsetMetres * std::sin(angle);
	pose.yaw = 0.4f + 0.9f * float(sweep_index_);
	pose.valid = true;
	r_hud.set_minimap_pose_override(pose);
	if (++frames_ < kMinimapModesSettleFrames) {
		return false;
	}
	tally(r_hud.minimap_debug(), r_tally, true, sweep_index_);
	frames_ = 0;
	++sweep_index_;
	return false;
}

bool MinimapModesCheck::step(Bf3NativeHud &r_hud, const MinimapModesIo &p_io, double p_delta) {
	if (done_) {
		return true;
	}
	clock_ += std::isfinite(p_delta) && p_delta > 0.0 ? p_delta : 0.0;
	const auto next = [&](Phase p_phase) {
		phase_ = p_phase;
		phase_start_ = clock_;
		frames_ = 0;
		sweep_index_ = 0;
	};
	const auto finish = [&] {
		r_hud.set_minimap_debug_uv(false);
		r_hud.set_minimap_debug_record(false);
		r_hud.set_minimap_pose_override(std::nullopt);
		if (Bf3OverviewMap *o = Bf3OverviewMap::current()) {
			o->set_debug_uv(false);
			o->set_debug_record(false);
		}
		phase_ = Phase::Done;
		done_ = true;
		return true;
	};
	const auto press = [&](ui::UiInputConcept p_concept) {
		const std::string key = p_io.concept_press ? p_io.concept_press(int32_t(p_concept)) : std::string();
		last_key_ = key;
		if (key.empty()) {
			row(mode_ + ".key." + std::string(ui::ui_input_concept_name(p_concept)), false,
					"no key in the router's bindings reaches the concept");
		}
		return key;
	};
	if (clock_ > kMinimapModesWaitSeconds) {
		row(mode_ + ".timeout", false,
				core::fmt("phase %d not finished within %.0f s", int(phase_), kMinimapModesWaitSeconds));
		return finish();
	}
	const std::optional<ui::SwfRect> box = r_hud.window_bounds(1, "instance1.Minimap_01");
	switch (phase_) {
		case Phase::Wait: {
			Bf3OverviewMap *o = Bf3OverviewMap::current();
			if (!p_io.spawn_screen || o == nullptr || o->debug_frames().empty() || !load(r_hud)) {
				return false;
			}
			o->set_debug_record(true);
			o->set_debug_uv(true);
			r_hud.set_minimap_debug_record(true);
			next(Phase::Overview);
			return false;
		}
		case Phase::Overview: {
			if (++frames_ < kMinimapModesSettleFrames) {
				return false;
			}
			Bf3OverviewMap *o = Bf3OverviewMap::current();
			const std::vector<MinimapDebugFrame> frames =
					o != nullptr ? o->debug_frames() : std::vector<MinimapDebugFrame>{};
			for (const MinimapDebugFrame &f : frames) {
				tally(f, overview_, false, 0);
			}
			objective_rows("overview", overview_);
			if (o != nullptr) {
				o->set_debug_uv(false);
			}
			p_io.deploy();
			next(Phase::Deploy);
			return false;
		}
		case Phase::Deploy:
			if (!p_io.alive || !r_hud.minimap_ready() || r_hud.minimap_debug().paints == 0) {
				if (clock_ - phase_start_ > 2.0 && !p_io.alive && p_io.spawn_screen) {
					p_io.deploy();
					phase_start_ = clock_;
				}
				return false;
			}
			next(Phase::Team);
			return false;
		case Phase::Team: {
			if (++frames_ < kMinimapModesSettleFrames * 4) {
				return false;
			}
			uint32_t friendly = 0, enemy = 0;
			for (const MinimapDebugHit &h : r_hud.minimap_debug().hits) {
				friendly += h.kind == MinimapDebugKind::Friendly || h.kind == MinimapDebugKind::Squad ? 1u : 0u;
				enemy += h.kind == MinimapDebugKind::Enemy ? 1u : 0u;
			}
			if (friendly == 0 && clock_ - phase_start_ < 10.0) {
				return false;
			}
			row(mode_ + ".team_icons", friendly > 0,
					core::fmt("%u team-mates and %u revealed enemies drawn in Minimap_01 with UIHudIcon_Player by "
							  "relation",
							friendly, enemy));
			r_hud.set_minimap_debug_uv(true);
			next(Phase::Small);
			return false;
		}
		case Phase::Small:
			if (!sweep(r_hud, small_)) {
				return false;
			}
			objective_rows("minimap_small", small_);
			{
				const MinimapDebugFrame &f = r_hud.minimap_debug();
				const std::optional<ui::MinimapUv> centre = uv_at(f, f.view.center_x, f.view.center_y);
				const ui::MinimapUv want = ui::world_to_minimap_uv(mapping_, f.view.player_x, f.view.player_z);
				const float err = centre ? ui::minimap_uv_error_texels(*centre, want, satellite_side_) : -1.0f;
				row(mode_ + ".minimap_small.centre", centre && err <= ui::kMinimapUvCheckTexels && f.view.rotate,
						core::fmt("the map's centre reads the player's UV within %.2f texels (%s, heading %.2f rad); "
								  "the rotation "
								  "and scale off the centre are godot.ui2's map.uv rows",
								double(err), f.view.rotate ? "rotating" : "north-up", double(f.view.heading)));
			}
			small_box_width_ = box ? box->width() : 0.0f;
			r_hud.set_minimap_debug_uv(false);
			if (press(ui::UiInputConcept::MapSize).empty()) {
				return finish();
			}
			next(Phase::ToggleLarge);
			return false;
		case Phase::ToggleLarge: {
			const NativeHudMinimapState &s = r_hud.minimap_state();
			if (s.small && clock_ - phase_start_ < kMinimapModesSizeSeconds) {
				return false;
			}
			if (++frames_ < kMinimapModesSettleFrames) {
				return false;
			}
			const float large_width = box ? box->width() : 0.0f;
			row(mode_ + ".map_size.toggle",
					!s.small && s.from_flow && small_box_width_ > 0.0f &&
							large_width >= kMinimapModesLargeBoxGrowth * small_box_width_ && !s.player_is_center,
					core::fmt("the bound MapSize key (%s) made the flow's uiminimapcomp IsSmall %s; Minimap_01 drawn "
							  "%.0f "
							  "px wide (small %.0f px, "
							  "wants x%.1f); zoom step %d = %.2f, player %s (the large map's start step wants not "
							  "centred)",
							last_key_.c_str(), s.small ? "True" : "False", double(large_width),
							double(small_box_width_), double(kMinimapModesLargeBoxGrowth), s.step, double(s.zoom),
							s.player_is_center ? "centred" : "not centred"));
			if (s.small) {
				return finish();
			}
			r_hud.set_minimap_debug_uv(true);
			next(Phase::LargeWhole);
			return false;
		}
		case Phase::LargeWhole:
			if (sweep_index_ < objectives_.size()) {
				sweep(r_hud, large_whole_);
				return false;
			}
			if (sweep_index_ == objectives_.size()) {
				r_hud.set_minimap_pose_override(std::nullopt);
				objective_rows("minimap_large_whole", large_whole_);
				++sweep_index_;
				frames_ = 0;
				return false;
			}
			if (++frames_ < kMinimapModesSettleFrames) {
				return false;
			}
			whole_view_row(r_hud.minimap_debug());
			r_hud.set_minimap_debug_uv(false);
			zoom_step_before_ = r_hud.minimap_state().step;
			if (press(ui::UiInputConcept::MapZoom).empty()) {
				return finish();
			}
			next(Phase::Zoom);
			return false;
		case Phase::Zoom: {
			const NativeHudMinimapState &s = r_hud.minimap_state();
			if (s.step == zoom_step_before_ && clock_ - phase_start_ < kMinimapModesSizeSeconds) {
				return false;
			}
			row(mode_ + ".map_size.zoom",
					!s.small && s.from_flow && s.step != zoom_step_before_ && s.step == kMinimapModesFirstLargeZoomStep,
					core::fmt("the bound MapZoom key (%s) stepped the large map from step %d to %d = %.2f (player %s); "
							  "the "
							  "flow's "
							  "ZoomLevelIncreaseMinimapLarge from an unset ZoomLevel wants step %d",
							last_key_.c_str(), zoom_step_before_, s.step, double(s.zoom),
							s.player_is_center ? "centred" : "not centred", kMinimapModesFirstLargeZoomStep));
			r_hud.set_minimap_debug_uv(true);
			next(Phase::Large);
			return false;
		}
		case Phase::Large: {
			if (!sweep(r_hud, large_)) {
				return false;
			}
			objective_rows("minimap_large", large_);
			const MinimapDebugFrame &f = r_hud.minimap_debug();
			const std::optional<Pixels> pixels = read_target(f.texture);
			float measured = 0.0f;
			if (pixels) {
				const int32_t cy = pixels->height / 2;
				const int32_t x0 = pixels->width / 2 - kMinimapModesScaleSpanPixels / 2;
				const ui::ScreenPoint a = local_of_pixel(f, x0, cy, *pixels);
				const ui::ScreenPoint b = local_of_pixel(f, x0 + kMinimapModesScaleSpanPixels, cy, *pixels);
				const std::optional<ui::MinimapUv> ua = decode_at(*pixels, f, a.x, a.y);
				const std::optional<ui::MinimapUv> ub = decode_at(*pixels, f, b.x, b.y);
				if (ua && ub) {
					const std::array<float, 2> wa = ui::minimap_uv_to_world(mapping_, *ua);
					const std::array<float, 2> wb = ui::minimap_uv_to_world(mapping_, *ub);
					measured = std::hypot(wb[0] - wa[0], wb[1] - wa[1]) / ((b.x - a.x) / f.local_per_unit);
				}
			}
			const float want = ui::minimap_metres_per_unit(r_hud.minimap_state().zoom);
			row(mode_ + ".map_size.drawn_scale",
					measured > 0.0f && std::fabs(measured - want) <= kMinimapModesScaleTolerance * want,
					core::fmt("the painter draws %.3f m per Minimap_01 unit (debug UV over %d px) where its formula "
							  "1.375 (1 + MapZoom) "
							  "gives %.3f at MapZoom %.2f; this checks the painter against the inferred formula, not "
							  "against BF3",
							double(measured), kMinimapModesScaleSpanPixels, double(want),
							double(r_hud.minimap_state().zoom)));
			next(Phase::Downed);
			return false;
		}
		case Phase::Downed: {
			const std::vector<NativeHudDownedMate> &mates = r_hud.downed_mates();
			if (mates.empty() || mates.front().slot != downed_slot_) {
				frames_ = 0;
				downed_slot_ = mates.empty() ? uint8_t(0xFF) : mates.front().slot;
			}
			if (mates.empty()) {
				r_hud.set_minimap_pose_override(std::nullopt);
				if (clock_ - phase_start_ > kMinimapModesDownedSeconds) {
					row(mode_ + ".revive_icons", false,
							core::fmt("no team-mate went down within %.0f s", kMinimapModesDownedSeconds));
					return finish();
				}
				return false;
			}
			NativeHudMinimapPose pose;
			pose.x = mates.front().x + 20.0f;
			pose.z = mates.front().z;
			pose.valid = true;
			r_hud.set_minimap_pose_override(pose);
			if (++frames_ < kMinimapModesSettleFrames) {
				return false;
			}
			const MinimapDebugFrame &f = r_hud.minimap_debug();
			float err = -1.0f;
			std::string icon;
			for (const MinimapDebugHit &h : f.hits) {
				if (h.kind == MinimapDebugKind::Downed) {
					const std::optional<ui::MinimapUv> got = uv_at(f, h.local_x, h.local_y);
					const ui::MinimapUv want = ui::world_to_minimap_uv(mapping_, h.world_x, h.world_z);
					err = got ? ui::minimap_uv_error_texels(*got, want, satellite_side_) : -1.0f;
					icon = h.icon;
					break;
				}
			}
			row(mode_ + ".revive_icons", err >= 0.0f && err <= ui::kMinimapUvCheckTexels,
					core::fmt("%zu downed team-mates in their revive window; the first drawn as %s at its corpse's UV "
							  "within %.2f texels",
							mates.size(), icon.c_str(), double(err)));
			return finish();
		}
		case Phase::Done:
			return true;
	}
	return finish();
}

std::string MinimapModesCheck::report() const {
	std::string out;
	for (const std::string &l : lines_) {
		out += l + "\n";
	}
	out += core::fmt("MINIMAP MODES %s: %s (%u rows, %u failed)", mode_.c_str(),
			done_ && failures_ == 0 ? "PASS" : "FAIL", rows_, failures_);
	return out;
}

}
