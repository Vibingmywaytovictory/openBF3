// UI ui2: MinimapCheck, the in-game minimap gates G9-G11 (minimap_check.h; docs/design/ui-minimap-icons.md §10.4).
#include "minimap_check.h"

#include "fp_camera.h"
#include "godot_util.h"
#include "input_mapper.h"
#include "level_session.h"
#include "minimap_control.h"
#include "minimap_hud.h"
#include "world_icons.h"

#include "bf3/core/install_snapshot.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace godot;
namespace ui = bf3::core::ui;

namespace bf3::godot_side {

namespace {

constexpr int kSettleFrames = 3; // frames between a change and its read-back (design §10.4: at least two)
constexpr float kPi = 3.14159265358979323846f;
constexpr int kDiffThreshold = 6; // per-channel byte difference that counts as "changed"
constexpr float kMarkerTolerance = 1.0f; // canvas units
constexpr float kRimTolerance = 1.0f; // canvas units
constexpr float kDirectionToleranceDeg = 2.0f;
constexpr float kWorldIconTolerance = 0.5f; // canvas units

enum Phase : int {
	kStart,
	kNorthUv,
	kNorthHidden,
	kNorthMarker,
	kNorthFull,
	kRotUv,
	kRotHidden,
	kRotMarker,
	kRotFull,
	kWorldHidden,
	kWorldVisible,
	kShots,
	kFinish,
};

std::string fmt(const char *p_format, ...) {
	char buf[1024];
	va_list args;
	va_start(args, p_format);
	std::vsnprintf(buf, sizeof(buf), p_format, args);
	va_end(args);
	return buf;
}

bool read_rgb(const Ref<Image> &p_image, int p_x, int p_y, std::array<uint8_t, 3> &r_rgb) {
	if (p_image.is_null() || p_x < 0 || p_y < 0 || p_x >= p_image->get_width() || p_y >= p_image->get_height()) {
		return false;
	}
	const Color c = p_image->get_pixel(p_x, p_y);
	r_rgb = { uint8_t(c.get_r8()), uint8_t(c.get_g8()), uint8_t(c.get_b8()) };
	return true;
}

bool differs(const Ref<Image> &p_a, const Ref<Image> &p_b, int p_x, int p_y) {
	std::array<uint8_t, 3> a{}, b{};
	if (!read_rgb(p_a, p_x, p_y, a) || !read_rgb(p_b, p_x, p_y, b)) {
		return false;
	}
	for (int i = 0; i < 3; ++i) {
		if (std::abs(int(a[i]) - int(b[i])) > kDiffThreshold) {
			return true;
		}
	}
	return false;
}

// Viewport pixel bounds of a control-local rect through a canvas -> viewport transform.
void pixel_bounds(const Transform2D &p_to_vp, const Rect2 &p_rect, int &r_x0, int &r_y0, int &r_x1, int &r_y1) {
	const Vector2 corners[4] = { p_rect.position, p_rect.position + Vector2(p_rect.size.x, 0), p_rect.position + Vector2(0, p_rect.size.y), p_rect.get_end() };
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	for (const Vector2 &c : corners) {
		const Vector2 q = p_to_vp.xform(c);
		x0 = std::min(x0, q.x);
		y0 = std::min(y0, q.y);
		x1 = std::max(x1, q.x);
		y1 = std::max(y1, q.y);
	}
	r_x0 = int(std::floor(x0));
	r_y0 = int(std::floor(y0));
	r_x1 = int(std::ceil(x1));
	r_y1 = int(std::ceil(y1));
}

size_t count_diff(const Ref<Image> &p_a, const Ref<Image> &p_b, int p_x0, int p_y0, int p_x1, int p_y1) {
	size_t n = 0;
	for (int y = p_y0; y < p_y1; ++y) {
		for (int x = p_x0; x < p_x1; ++x) {
			n += differs(p_a, p_b, x, y) ? 1 : 0;
		}
	}
	return n;
}

float angle_between_deg(float p_ax, float p_ay, float p_bx, float p_by) {
	const float la = std::sqrt(p_ax * p_ax + p_ay * p_ay);
	const float lb = std::sqrt(p_bx * p_bx + p_by * p_by);
	if (!(la > 0.0f) || !(lb > 0.0f)) {
		return 180.0f;
	}
	const float c = std::clamp((p_ax * p_bx + p_ay * p_by) / (la * lb), -1.0f, 1.0f);
	return std::acos(c) * 180.0f / kPi;
}

bool uv_in_unit(const ui::MinimapUv &p_uv) {
	return p_uv.u >= 0.0f && p_uv.u <= 1.0f && p_uv.v >= 0.0f && p_uv.v <= 1.0f;
}

} // namespace

MinimapCheck::MinimapCheck(Bf3MinimapHud &p_hud, std::string p_shots_dir, bool p_rows) :
		hud_(p_hud), shots_dir_(std::move(p_shots_dir)), rows_enabled_(p_rows) {}

int MinimapCheck::exit_code() const {
	return exit_code_;
}

void MinimapCheck::add(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	rows_.push_back({ p_name, p_pass, p_detail });
	UtilityFunctions::print(to_godot(std::string("UI2 CHECK ") + (p_pass ? "PASS " : "FAIL ") + p_name + " " + p_detail));
}

Ref<Image> MinimapCheck::grab() const {
	Ref<Image> img = hud_.get_viewport()->get_texture()->get_image();
	if (img.is_valid() && img->get_format() != Image::FORMAT_RGBA8) {
		img->convert(Image::FORMAT_RGBA8);
	}
	return img;
}

void MinimapCheck::row_calibration(const Ref<Image> &p_image) {
	size_t exact = 0;
	std::array<uint8_t, 3> first{};
	bool have_first = false;
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			std::array<uint8_t, 3> rgb{};
			if (read_rgb(p_image, calibration_px_.x + x, calibration_px_.y + y, rgb)) {
				if (!have_first) {
					first = rgb;
					have_first = true;
				}
				exact += (rgb[0] == 17 && rgb[1] == 34 && rgb[2] == 51) ? 1 : 0;
			}
		}
	}
	add("readback.calibration", exact == 16,
			fmt("%zu / 16 pixels exactly (17, 34, 51) at viewport (%d, %d); first read (%d, %d, %d); image %d x %d", exact, calibration_px_.x, calibration_px_.y, first[0],
					first[1], first[2], p_image.is_valid() ? p_image->get_width() : 0, p_image.is_valid() ? p_image->get_height() : 0));
}

void MinimapCheck::row_map_uv(const char *p_name, const ViewFrames &p_frames) {
	std::shared_ptr<const MinimapAssets> a = hud_.assets();
	Bf3MinimapControl *c = hud_.control();
	const ui::MinimapMapping mapping = ui::minimap_mapping(a->level.data);
	const uint32_t side = a->satellite_side;
	const ui::MinimapView &view = p_frames.view;
	const Transform2D to_vp = c->canvas_to_viewport();
	const Transform2D to_local = to_vp.affine_inverse();
	const Vector2 centre(view.center_x, view.center_y);
	const float radius = c->disc_radius();
	struct Sample {
		Vector2 at;
		bool up = false;
	};
	std::vector<Sample> samples;
	samples.push_back({ centre, false });
	const float fractions[kMinimapCheckRings] = { 0.25f, 0.50f, 0.85f };
	for (int ring = 0; ring < kMinimapCheckRings; ++ring) {
		for (int k = 0; k < kMinimapCheckRingSamples; ++k) {
			const float ang = 2.0f * kPi * float(k) / float(kMinimapCheckRingSamples);
			samples.push_back({ centre + Vector2(std::sin(ang), -std::cos(ang)) * (radius * fractions[ring]), k == 0 });
		}
	}
	size_t used = 0, skipped = 0, unread = 0;
	float max0 = 0.0f;
	float centre_err = -1.0f;
	std::array<float, ui::kMinimapMappingVariants> max_variant{};
	float worst_forward = 0.0f;
	size_t forward_checked = 0;
	float sum_du = 0.0f, sum_dv = 0.0f;
	for (size_t si = 0; si < samples.size(); ++si) {
		const Vector2 q = to_vp.xform(samples[si].at);
		const int ix = int(std::floor(q.x));
		const int iy = int(std::floor(q.y));
		const Vector2 pc = to_local.xform(Vector2(float(ix) + 0.5f, float(iy) + 0.5f));
		const ui::ScreenPoint sp{ pc.x, pc.y };
		const ui::MinimapUv expected = ui::minimap_expected_uv(view, mapping, sp, 0);
		if (!uv_in_unit(expected)) {
			++skipped;
			continue;
		}
		std::array<uint8_t, 3> rgb{};
		if (!read_rgb(p_frames.uv, ix, iy, rgb)) {
			++unread;
			continue;
		}
		++used;
		const ui::MinimapUv decoded = ui::decode_minimap_debug_uv({ rgb[0], rgb[1], rgb[2] });
		const float e0 = ui::minimap_uv_error_texels(decoded, expected, side);
		max0 = std::max(max0, e0);
		if (si == 0) {
			centre_err = e0;
		}
		sum_du += (decoded.u - expected.u) * float(side);
		sum_dv += (decoded.v - expected.v) * float(side);
		for (uint8_t v = 1; v < ui::kMinimapMappingVariants; ++v) {
			max_variant[v] = std::max(max_variant[v], ui::minimap_uv_error_texels(decoded, ui::minimap_expected_uv(view, mapping, sp, v), side));
		}
		if (view.rotate && samples[si].up) {
			// Forward is up: the world point under a sample straight above the centre lies in the view direction.
			const std::array<float, 2> w = ui::minimap_uv_to_world(mapping, decoded);
			const float dev = angle_between_deg(w[0] - view.player_x, w[1] - view.player_z, std::sin(view.heading), std::cos(view.heading));
			worst_forward = std::max(worst_forward, dev);
			++forward_checked;
		}
	}
	float min_variant = std::numeric_limits<float>::infinity();
	uint8_t min_variant_id = 0;
	for (uint8_t v = 1; v < ui::kMinimapMappingVariants; ++v) {
		if (max_variant[v] < min_variant) {
			min_variant = max_variant[v];
			min_variant_id = v;
		}
	}
	bool pass = used >= 40 && unread == 0 && max0 <= ui::kMinimapUvCheckTexels && min_variant > ui::kMinimapUvMirrorTexels;
	std::string detail = fmt("%zu samples used (%zu outside the image skipped, %zu unread), max error %.3f texels (centre %.3f; limit %.1f), the other 7 "
							 "variants' max error >= %.1f texels (variant %u; limit > %.0f); heading %.3f rad, player (%.2f, %.2f), %.4f units/m",
			used, skipped, unread, max0, centre_err, ui::kMinimapUvCheckTexels, min_variant, unsigned(min_variant_id), ui::kMinimapUvMirrorTexels, view.heading,
			view.player_x, view.player_z, view.pixels_per_metre);
	if (used > 0) {
		detail += fmt("; mean signed error (%.3f, %.3f) texels", sum_du / float(used), sum_dv / float(used));
	}
	if (view.rotate) {
		// Every up sample inside the image counts (one ring's can fall outside it near the map's edge).
		pass = pass && forward_checked >= 1 && worst_forward <= kDirectionToleranceDeg;
		detail += fmt("; forward is up: %zu / %d up samples inside the image, worst %.2f deg (limit %.1f)", forward_checked, kMinimapCheckRings, worst_forward, kDirectionToleranceDeg);
	}
	add(p_name, pass, detail);
}

void MinimapCheck::row_marker_pixels(const ViewFrames &p_north, const ViewFrames &p_rotating) {
	std::shared_ptr<const MinimapAssets> a = hud_.assets();
	Bf3MinimapControl *c = hud_.control();
	const Transform2D to_vp = c->canvas_to_viewport();
	const Transform2D to_local = to_vp.affine_inverse();
	const float k = c->icon_scale();
	bool pass = true;
	std::string detail;
	for (const ViewFrames *f : { &p_north, &p_rotating }) {
		const Vector2 centre(f->view.center_x, f->view.center_y);
		const float half = 20.0f * k + 4.0f; // the marker frame is 17 x 19 texels
		int x0, y0, x1, y1;
		pixel_bounds(to_vp, Rect2(centre - Vector2(half, half), Vector2(2 * half, 2 * half)), x0, y0, x1, y1);
		int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
		size_t n = 0;
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (differs(f->hidden, f->marker, x, y)) {
					++n;
					bx0 = std::min(bx0, x);
					by0 = std::min(by0, y);
					bx1 = std::max(bx1, x);
					by1 = std::max(by1, y);
				}
			}
		}
		const float angle = ui::minimap_marker_angle(f->view);
		const Vector2 off = Vector2(a->marker_alpha_dx, a->marker_alpha_dy).rotated(angle) * k;
		const Vector2 expected = centre + off;
		float err = std::numeric_limits<float>::infinity();
		Vector2 measured;
		if (n > 0) {
			measured = to_local.xform(Vector2(0.5f * float(bx0 + bx1 + 1), 0.5f * float(by0 + by1 + 1)));
			err = measured.distance_to(expected);
		}
		const bool ok = n > 0 && err <= kMarkerTolerance;
		pass = pass && ok;
		detail += fmt("%s%s: %zu changed pixels, box centre (%.2f, %.2f) vs centre + frame offset (%.2f, %.2f), error %.2f units (limit %.1f), angle %.1f deg",
				detail.empty() ? "" : "; ", f->view.rotate ? "rotating" : "north-up", n, measured.x, measured.y, expected.x, expected.y, err, kMarkerTolerance,
				angle * 180.0f / kPi);
	}
	add("marker.pixels", pass, detail);
}

void MinimapCheck::evaluate_icons(const ViewFrames &p_frames, std::string &r_detail, bool &r_pass, size_t &r_checked) {
	std::shared_ptr<const MinimapAssets> a = hud_.assets();
	Bf3MinimapControl *c = hud_.control();
	const ui::MinimapMapping mapping = ui::minimap_mapping(a->level.data);
	const uint32_t side = a->satellite_side;
	const ui::MinimapView &view = p_frames.view;
	const Transform2D to_vp = c->canvas_to_viewport();
	const Transform2D to_local = to_vp.affine_inverse();
	const Vector2 centre(view.center_x, view.center_y);
	const bool north = !view.rotate;
	const std::vector<ui::ScreenPoint> &pts = north ? north_icons_ : rotating_icons_;
	const std::vector<Rect2> &rects = north ? north_rects_ : rotating_rects_;
	const std::vector<bool> &clamped = north ? north_clamped_ : rotating_clamped_;
	const std::vector<MinimapIconDraw> &drawn = c->icons();
	size_t on_disc = 0, on_rim = 0, bad = 0;
	float worst_uv = 0.0f, worst_rim = 0.0f, worst_dir = 0.0f;
	size_t min_pixels = std::numeric_limits<size_t>::max();
	std::string problems;
	for (size_t i = 0; i < a->level.objectives.size(); ++i) {
		const ui::MinimapObjective &o = a->level.objectives[i];
		const ui::HudIconChoice choice = ui::objective_icon(o.kind == ui::ObjectiveKind::Hq, o.initial_owner, kM1LocalTeam);
		bool ok = true;
		if (i >= drawn.size() || drawn[i].icon == nullptr || drawn[i].icon->type != choice.icon || drawn[i].state != choice.state) {
			ok = false;
			problems += fmt(" #%zu entry/state", i);
		}
		if (i >= pts.size() || !std::isfinite(pts[i].x) || !std::isfinite(pts[i].y)) {
			++bad;
			problems += fmt(" #%zu not drawn", i);
			continue;
		}
		const Vector2 at(pts[i].x, pts[i].y);
		if (clamped[i]) {
			++on_rim;
			const float rim_err = std::fabs(at.distance_to(centre) - c->rim_radius());
			const ui::ScreenPoint p = ui::minimap_project(view, o.position[0], o.position[2]);
			const float dir = angle_between_deg(at.x - centre.x, at.y - centre.y, p.x - centre.x, p.y - centre.y);
			worst_rim = std::max(worst_rim, rim_err);
			worst_dir = std::max(worst_dir, dir);
			if (rim_err > kRimTolerance || dir > kDirectionToleranceDeg) {
				ok = false;
				problems += fmt(" #%zu rim %.2f / %.2f deg", i, rim_err, dir);
			}
		} else {
			++on_disc;
			const Vector2 q = to_vp.xform(at);
			const int ix = int(std::floor(q.x));
			const int iy = int(std::floor(q.y));
			const Vector2 pc = to_local.xform(Vector2(float(ix) + 0.5f, float(iy) + 0.5f));
			// The objective's texel, moved by the map's own gradient from the icon point to the pixel centre read.
			const ui::MinimapUv at_obj = ui::world_to_minimap_uv(mapping, o.position[0], o.position[2]);
			const ui::MinimapUv e_pc = ui::minimap_expected_uv(view, mapping, { pc.x, pc.y });
			const ui::MinimapUv e_at = ui::minimap_expected_uv(view, mapping, { at.x, at.y });
			const ui::MinimapUv expected{ at_obj.u + (e_pc.u - e_at.u), at_obj.v + (e_pc.v - e_at.v) };
			std::array<uint8_t, 3> rgb{};
			if (!read_rgb(p_frames.uv, ix, iy, rgb)) {
				ok = false;
				problems += fmt(" #%zu unread", i);
			} else {
				const float err = ui::minimap_uv_error_texels(ui::decode_minimap_debug_uv({ rgb[0], rgb[1], rgb[2] }), expected, side);
				worst_uv = std::max(worst_uv, err);
				if (err > ui::kMinimapUvCheckTexels) {
					ok = false;
					problems += fmt(" #%zu uv %.2f texels", i, err);
				}
			}
		}
		int x0, y0, x1, y1;
		pixel_bounds(to_vp, rects[i], x0, y0, x1, y1);
		const size_t n = count_diff(p_frames.hidden, p_frames.full, x0, y0, x1, y1);
		min_pixels = std::min(min_pixels, n);
		if (n == 0) {
			ok = false;
			problems += fmt(" #%zu no pixels", i);
		}
		bad += ok ? 0 : 1;
		++r_checked;
	}
	if (bad > 0 || a->level.objectives.empty()) {
		r_pass = false;
	}
	r_detail += fmt("%s%s: %zu on the disc (worst uv %.3f texels), %zu on the rim (worst %.2f units, %.2f deg), fewest icon pixels %zu%s%s",
			r_detail.empty() ? "" : "; ", north ? "north-up" : "rotating", on_disc, worst_uv, on_rim, worst_rim, worst_dir,
			min_pixels == std::numeric_limits<size_t>::max() ? size_t(0) : min_pixels, problems.empty() ? "" : "; problems:", problems.c_str());
}

void MinimapCheck::row_icons_minimap(const ViewFrames &p_north, const ViewFrames &p_rotating) {
	std::shared_ptr<const MinimapAssets> a = hud_.assets();
	bool pass = true;
	size_t checked = 0;
	std::string detail = fmt("%s %s, %zu objectives", a->level.data.level_id.c_str(), a->level.mode.c_str(), a->level.objectives.size());
	std::string views;
	evaluate_icons(p_north, views, pass, checked);
	evaluate_icons(p_rotating, views, pass, checked);
	add("icons.minimap", pass && checked == 2 * a->level.objectives.size(), detail + "; " + views);
}

void MinimapCheck::row_icons_world(const Ref<Image> &p_hidden, const Ref<Image> &p_visible) {
	std::shared_ptr<const MinimapAssets> a = hud_.assets();
	Bf3WorldIcons *w = hud_.world_icons();
	Camera3D *cam = hud_.session()->cameras().first_person();
	const Transform3D ct = cam->get_camera_transform();
	const Transform3D inv = ct.affine_inverse();
	const Projection pr = cam->get_camera_projection();
	const Rect2 vis = w->get_viewport()->get_visible_rect();
	const Transform2D to_vp = w->get_viewport()->get_final_transform() * w->get_global_transform_with_canvas();
	const std::vector<ui::ScreenPoint> &pts = w->last_points();
	const std::vector<Rect2> &rects = w->last_rects();
	size_t front = 0, behind = 0, on_screen = 0, bad = 0;
	float worst = 0.0f;
	size_t min_pixels = std::numeric_limits<size_t>::max();
	std::string problems;
	for (size_t i = 0; i < a->level.objectives.size(); ++i) {
		const ui::MinimapObjective &o = a->level.objectives[i];
		const Vector3 p(o.position[0], o.position[1] + w->style().lift, o.position[2]);
		const Vector3 pc = inv.xform(p);
		const bool own_behind = -pc.z < cam->get_near();
		const bool drawn = i < pts.size() && std::isfinite(pts[i].x) && std::isfinite(pts[i].y);
		if (own_behind) {
			++behind;
			if (drawn) {
				++bad;
				problems += fmt(" #%zu drawn while behind", i);
			}
			continue;
		}
		++front;
		if (!drawn) {
			++bad;
			problems += fmt(" #%zu not drawn", i);
			continue;
		}
		const Vector4 clip = pr.xform(Vector4(pc.x, pc.y, pc.z, 1.0f));
		const float ndc_x = clip.x / clip.w;
		const float ndc_y = clip.y / clip.w;
		const Vector2 own((ndc_x * 0.5f + 0.5f) * vis.size.x, (-ndc_y * 0.5f + 0.5f) * vis.size.y + w->style().pixel_offset_y);
		const float err = own.distance_to(Vector2(pts[i].x, pts[i].y));
		worst = std::max(worst, err);
		if (err > kWorldIconTolerance) {
			++bad;
			problems += fmt(" #%zu off by %.2f", i, err);
			continue;
		}
		if (rects[i].intersects(Rect2(Vector2(), vis.size))) {
			++on_screen;
			int x0, y0, x1, y1;
			pixel_bounds(to_vp, rects[i], x0, y0, x1, y1);
			const size_t n = count_diff(p_hidden, p_visible, x0, y0, x1, y1);
			min_pixels = std::min(min_pixels, n);
			if (n == 0) {
				++bad;
				problems += fmt(" #%zu no pixels", i);
			}
		}
	}
	add("icons.world", bad == 0 && !a->level.objectives.empty() && front > 0,
			fmt("view yaw %.3f rad towards the objectives' centroid: %zu in front (%zu on screen, worst %.3f units vs own projection, limit %.1f; fewest icon pixels %zu), %zu behind not drawn%s%s", world_yaw_, front, on_screen, worst,
					kWorldIconTolerance, min_pixels == std::numeric_limits<size_t>::max() ? size_t(0) : min_pixels, behind, problems.empty() ? "" : "; problems:",
					problems.c_str()));
}

void MinimapCheck::finish_rows() {
	Bf3LevelSession *s = hud_.session();
	if (rows_enabled_) {
		if (s != nullptr && s->install_snapshot_at_start().valid) {
			const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(s->install_snapshot_at_start(), s->install().snapshot());
			add("install.unchanged", d.clean(), fmt("%zu index files; %s", s->install_snapshot_at_start().files.size(), d.clean() ? "unchanged" : d.summary().c_str()));
		} else {
			add("install.unchanged", false, "no install snapshot");
		}
		size_t passed = 0;
		for (const Row &r : rows_) {
			passed += r.pass ? 1 : 0;
		}
		const bool all = passed == rows_.size();
		UtilityFunctions::print(to_godot(fmt("UI2 MINIMAP CHECK: %s (%zu/%zu)", all ? "PASS" : "FAIL", passed, rows_.size())));
		exit_code_ = all ? 0 : 1;
	} else {
		UtilityFunctions::print(to_godot(fmt("[ui2] %zu minimap shots written to %s", shots_saved_, shots_dir_.c_str())));
		exit_code_ = shots_failed_ ? 1 : 0;
	}
	done_ = true;
}

bool MinimapCheck::step(double /*p_delta*/) {
	if (done_) {
		return true;
	}
	if (wait_ > 0) {
		--wait_;
		return false;
	}
	Bf3LevelSession *s = hud_.session();
	Bf3MinimapControl *c = hud_.control();
	switch (phase_) {
		case kStart: {
			std::shared_ptr<const MinimapAssets> a = hud_.assets();
			if (!hud_.ready_to_draw() || a == nullptr || c == nullptr) {
				add("minimap.loaded", false, a == nullptr ? std::string("nothing published") : "not drawn: " + a->error);
				finish_rows();
				return true;
			}
			if (rows_enabled_) {
				const bool metro = a->level.data.level_id == "MP_Subway";
				const bool sizes = metro ? (a->satellite_side == 2048 && a->combat_area_side == 128 && a->atlas_width == 256 && a->atlas_height == 1024)
										 : (a->satellite_side > 0 && a->atlas_width > 0);
				add("minimap.loaded", sizes && a->seconds <= 5.0,
						fmt("%s %s: satellite %u^2 L8, combat area %u^2 (%zu masks), atlas %u x %u RGBA8, %zu objectives; loader %.2f s (limit 5.0, %s), peak working set "
							"%+.1f MiB, working set %+.1f MiB across the loader (reported)",
								a->level.data.level_id.c_str(), a->level.mode.c_str(), a->satellite_side, a->combat_area_side, a->level.combat_areas.size(), a->atlas_width,
								a->atlas_height, a->level.objectives.size(), a->seconds, a->steps.c_str(), double(a->peak_working_set_delta) / (1024.0 * 1024.0),
								double(a->working_set_delta) / (1024.0 * 1024.0)));
				// marker.spawn: the first drawn frame, no input yet.
				const ui::MinimapView &v = hud_.view();
				std::shared_ptr<const bf3::host::SharedLevel> shared = s->shared_level();
				float best = std::numeric_limits<float>::infinity();
				size_t best_i = 0;
				const size_t n_spawns = shared != nullptr ? shared->spawns.size() : 0;
				for (size_t i = 0; i < n_spawns; ++i) {
					const auto &sp = shared->spawns[i];
					const float d = std::hypot(sp.position[0] - v.player_x, sp.position[2] - v.player_z);
					if (d < best) {
						best = d;
						best_i = i;
					}
				}
				add("marker.spawn", n_spawns > 0 && best <= 0.5f,
						fmt("own (%.2f, %.2f) is %.3f m from spawn %zu of %zu (limit 0.5); marker at the control centre (%.1f, %.1f)", v.player_x, v.player_z, best, best_i,
								n_spawns, v.center_x, v.center_y));
			}
			if (!rows_enabled_) {
				phase_ = kShots;
				return false;
			}
			// Freeze the view: the minimap stays on the own position, the view turns only when scripted.
			const ui::MinimapView &v = hud_.view();
			hud_.set_center_override(v.player_x, v.player_z);
			InputMapper &m = s->mapper();
			m.set_scripted(true);
			yaw0_ = std::round(m.view().yaw / (0.5f * kPi)) * (0.5f * kPi); // a multiple of 90 degrees (marker.pixels)
			pitch0_ = m.view().pitch;
			m.script_view(yaw0_ + 0.5f * kPi, pitch0_); // north-up at a turned heading (the marker turns by -heading)
			// The calibration patch near the bottom-right corner: a ColorRect at a whole canvas-unit origin (Controls
			// snap their origin to whole units, gui/common/snap_controls_to_pixels) covering at least 8 x 8 viewport
			// pixels; the 4 x 4 pixels around its centre are read.
			Viewport *vp = hud_.get_viewport();
			const Transform2D fin = vp->get_final_transform();
			const Rect2 vis = vp->get_visible_rect();
			const float scale = std::max(1e-3f, std::fabs(fin.get_scale().x));
			const float side_units = std::ceil(8.0f / scale) + 1.0f;
			const Vector2 end = vis.get_end();
			const Vector2 p0(std::round(end.x - 16.0f - side_units), std::round(end.y - 16.0f - side_units));
			const Vector2 mid = fin.xform(p0 + Vector2(side_units, side_units) * 0.5f);
			calibration_px_ = Vector2i(int(std::floor(mid.x)) - 2, int(std::floor(mid.y)) - 2);
			const Vector2 p1 = p0 + Vector2(side_units, side_units);
			calibration_layer_ = memnew(CanvasLayer);
			calibration_layer_->set_layer(120);
			calibration_ = memnew(ColorRect);
			calibration_->set_color(Color(17.0f / 255.0f, 34.0f / 255.0f, 51.0f / 255.0f, 1.0f));
			calibration_->set_position(p0);
			calibration_->set_size(p1 - p0);
			calibration_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			calibration_layer_->add_child(calibration_);
			hud_.add_child(calibration_layer_);
			hud_.set_north_up(true);
			c->set_icons_visible(false);
			c->set_debug_uv(true);
			phase_ = kNorthUv;
			wait_ = kSettleFrames;
			return false;
		}
		case kNorthUv: {
			const Ref<Image> img = grab();
			row_calibration(img);
			calibration_layer_->set_visible(false);
			north_.uv = img;
			north_.view = c->view();
			row_map_uv("map.uv.north_up", north_);
			c->set_debug_uv(false);
			phase_ = kNorthHidden;
			wait_ = kSettleFrames;
			return false;
		}
		case kNorthHidden:
		case kRotHidden: {
			(phase_ == kNorthHidden ? north_ : rotating_).hidden = grab();
			c->set_icons_visible(true);
			c->set_marker_only(true);
			phase_ = phase_ == kNorthHidden ? kNorthMarker : kRotMarker;
			wait_ = kSettleFrames;
			return false;
		}
		case kNorthMarker:
		case kRotMarker: {
			(phase_ == kNorthMarker ? north_ : rotating_).marker = grab();
			c->set_marker_only(false);
			phase_ = phase_ == kNorthMarker ? kNorthFull : kRotFull;
			wait_ = kSettleFrames;
			return false;
		}
		case kNorthFull: {
			north_.full = grab();
			north_icons_ = c->last_icons();
			north_rects_ = c->last_icon_rects();
			north_clamped_ = c->last_clamped();
			hud_.set_north_up(false);
			s->mapper().script_view(yaw0_ + kPi, pitch0_); // a scripted 90-degree turn from the north-up frames
			c->set_icons_visible(false);
			c->set_debug_uv(true);
			phase_ = kRotUv;
			wait_ = kSettleFrames;
			return false;
		}
		case kRotUv: {
			rotating_.uv = grab();
			rotating_.view = c->view();
			row_map_uv("map.uv.rotating", rotating_);
			c->set_debug_uv(false);
			phase_ = kRotHidden;
			wait_ = kSettleFrames;
			return false;
		}
		case kRotFull: {
			rotating_.full = grab();
			rotating_icons_ = c->last_icons();
			rotating_rects_ = c->last_icon_rects();
			rotating_clamped_ = c->last_clamped();
			row_marker_pixels(north_, rotating_);
			row_icons_minimap(north_, rotating_);
			{
				// icons.world looks towards the objectives' centroid (Metro's spawns face along the corridor, where a
				// 90-degree turn can leave every objective behind), level.
				std::shared_ptr<const MinimapAssets> a = hud_.assets();
				float sx = 0.0f, sz = 0.0f;
				for (const ui::MinimapObjective &o : a->level.objectives) {
					sx += o.position[0];
					sz += o.position[2];
				}
				const size_t n = std::max<size_t>(1, a->level.objectives.size());
				const Vector3 eye = s->cameras().last_eye();
				world_yaw_ = std::atan2(sx / float(n) - eye.x, sz / float(n) - eye.z);
				s->mapper().script_view(world_yaw_, 0.0f);
			}
			hud_.world_icons()->set_visible(false);
			phase_ = kWorldHidden;
			wait_ = kSettleFrames;
			return false;
		}
		case kWorldHidden: {
			world_hidden_ = grab();
			hud_.world_icons()->set_visible(true);
			phase_ = kWorldVisible;
			wait_ = kSettleFrames;
			return false;
		}
		case kWorldVisible: {
			row_icons_world(world_hidden_, grab());
			// The debug frames are not kept.
			north_ = ViewFrames{};
			rotating_ = ViewFrames{};
			world_hidden_.unref();
			phase_ = kShots;
			frames_in_phase_ = 0;
			return false;
		}
		case kShots: {
			if (shots_dir_.empty()) {
				phase_ = kFinish;
				return false;
			}
			std::shared_ptr<const MinimapAssets> a = hud_.assets();
			struct Shot {
				const char *where;
				bool override;
				float x, z;
				bool north;
			};
			std::vector<Shot> shots;
			const ui::MinimapView &v = hud_.view();
			const float own_x = hud_.session()->cameras().last_eye().x;
			const float own_z = hud_.session()->cameras().last_eye().z;
			(void)v;
			shots.push_back({ "spawn", true, own_x, own_z, false });
			shots.push_back({ "spawn", true, own_x, own_z, true });
			const ui::MinimapObjective *hq = nullptr;
			const ui::MinimapObjective *flag = nullptr;
			for (const ui::MinimapObjective &o : a->level.objectives) {
				if (o.kind == ui::ObjectiveKind::Hq && hq == nullptr) {
					hq = &o;
				}
				if (o.kind == ui::ObjectiveKind::Flag && flag == nullptr) {
					flag = &o;
				}
			}
			if (hq != nullptr) {
				shots.push_back({ "hq", true, hq->position[0], hq->position[2], false });
				shots.push_back({ "hq", true, hq->position[0], hq->position[2], true });
			}
			if (flag != nullptr) {
				shots.push_back({ "flag", true, flag->position[0], flag->position[2], false });
				shots.push_back({ "flag", true, flag->position[0], flag->position[2], true });
			}
			if (shot_index_ >= shots.size()) {
				hud_.clear_center_override();
				phase_ = kFinish;
				return false;
			}
			const Shot &sh = shots[shot_index_];
			if (frames_in_phase_ == 0) {
				c->set_debug_uv(false);
				c->set_icons_visible(true);
				c->set_marker_only(false);
				hud_.set_center_override(sh.x, sh.z);
				hud_.set_north_up(sh.north);
				frames_in_phase_ = 1;
				wait_ = kSettleFrames;
				return false;
			}
			std::string dir = shots_dir_;
			while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
				dir.pop_back();
			}
			const std::string path = dir + "/ui2_" + sh.where + "_" + (sh.north ? "north_up" : "rotating") + ".png";
			if (s->save_screenshot(path)) {
				++shots_saved_;
				UtilityFunctions::print(to_godot("[ui2] shot " + path));
			} else {
				shots_failed_ = true;
			}
			++shot_index_;
			frames_in_phase_ = 0;
			return false;
		}
		case kFinish:
		default: {
			if (rows_enabled_ && !shots_dir_.empty()) {
				add("shots", !shots_failed_ && shots_saved_ >= 4, fmt("%zu PNGs written to %s (inspect by eye, G11)", shots_saved_, shots_dir_.c_str()));
			}
			finish_rows();
			return true;
		}
	}
}

} // namespace bf3::godot_side
