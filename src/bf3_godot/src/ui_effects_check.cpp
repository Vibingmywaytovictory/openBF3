#include "ui_effects_check.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "level_session.h"
#include "overview_map.h"
#include "swf_view.h"
#include "ui_assets.h"
#include "ui_cursor.h"
#include "ui_post_process.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/core/ui/ui_post_process.h"
#include "bf3/core/util/format.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

constexpr double kTimeoutSeconds = 150.0;
constexpr double kSettleSeconds = 2.0;
constexpr int kCaptureFrames = 3;
constexpr std::string_view kSpawnGraph = "ui/flow/graph/spawn/spawngraphpc";

struct Rgb {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
};

Rgb pixel(const Ref<Image> &p_img, int32_t p_x, int32_t p_y) {
	const Color c = p_img->get_pixel(std::clamp(p_x, 0, p_img->get_width() - 1), std::clamp(p_y, 0, p_img->get_height() - 1));
	return Rgb{ c.r, c.g, c.b };
}

double mean_abs_diff(const Ref<Image> &p_a, const Ref<Image> &p_b) {
	if (p_a.is_null() || p_b.is_null() || p_a->get_size() != p_b->get_size()) {
		return -1.0;
	}
	double sum = 0.0;
	size_t n = 0;
	for (int32_t y = 0; y < p_a->get_height(); y += 2) {
		for (int32_t x = 0; x < p_a->get_width(); x += 2) {
			const Rgb a = pixel(p_a, x, y);
			const Rgb b = pixel(p_b, x, y);
			sum += std::fabs(a.r - b.r) + std::fabs(a.g - b.g) + std::fabs(a.b - b.b);
			n += 3;
		}
	}
	return n > 0 ? sum / double(n) : -1.0;
}

std::optional<double> median_ratio(const Ref<Image> &p_img, int p_num, int p_den) {
	std::vector<double> ratios;
	const int32_t w = p_img->get_width();
	const int32_t h = p_img->get_height();
	for (int32_t y = h / 4; y < h * 3 / 4; y += 3) {
		for (int32_t x = w / 4; x < w * 3 / 4; x += 3) {
			const Rgb c = pixel(p_img, x, y);
			const float v[3] = { c.r, c.g, c.b };
			if (v[p_den] >= 24.0f / 255.0f && v[p_num] < 250.0f / 255.0f) {
				ratios.push_back(double(v[p_num]) / double(v[p_den]));
			}
		}
	}
	if (ratios.size() < 100) {
		return std::nullopt;
	}
	std::nth_element(ratios.begin(), ratios.begin() + std::ptrdiff_t(ratios.size() / 2), ratios.end());
	return ratios[ratios.size() / 2];
}

bool near(double p_a, double p_b, double p_tol) {
	return std::isfinite(p_a) && std::fabs(p_a - p_b) <= p_tol;
}

Rgb patch_mean(const Ref<Image> &p_img, int32_t p_x, int32_t p_y, int32_t p_side) {
	Rgb sum;
	int32_t n = 0;
	for (int32_t y = p_y; y < p_y + p_side; ++y) {
		for (int32_t x = p_x; x < p_x + p_side; ++x) {
			const Rgb c = pixel(p_img, x, y);
			sum.r += c.r;
			sum.g += c.g;
			sum.b += c.b;
			++n;
		}
	}
	return n > 0 ? Rgb{ sum.r / float(n), sum.g / float(n), sum.b / float(n) } : sum;
}

constexpr std::string_view kDeployScreenMovie = "ui/assets/spawnscreenpc";
constexpr std::string_view kMenuScreenMovie = "ui/assets/ingamemenump";
constexpr ui::SwfRect kDeployMovieBox{ 769.0f, 1183.3f, 132.0f, 547.8f };
constexpr ui::SwfRect kMenuMovieBox{ 778.0f, 1179.0f, 153.0f, 554.0f };
constexpr ui::SwfRect kDeployFootageBox{ 777.0f, 1177.0f, 136.0f, 526.0f };
constexpr ui::SwfRect kMenuFootageBox{ 783.0f, 1173.0f, 153.0f, 545.0f };
constexpr float kOverviewBoxTolerance = 1.5f;

const ui::SwfLayoutNode *find_overview_node(const ui::SwfLayoutNode &p_node) {
	if (p_node.symbol == kOverviewMapSymbol) {
		return &p_node;
	}
	for (const ui::SwfLayoutNode &c : p_node.children) {
		if (const ui::SwfLayoutNode *hit = find_overview_node(c)) {
			return hit;
		}
	}
	return nullptr;
}

bf3::core::Result<ui::SwfMatrix> screen_overview_placement(std::string_view p_movie) {
	Bf3UiAssets *assets = Bf3UiAssets::current();
	if (assets == nullptr || !assets->is_loaded() || assets->movies() == nullptr) {
		return bf3::core::make_error("Bf3UiAssets is not loaded");
	}
	ui::WorkBudget budget(ui::kSwfLayoutBudgetUnits);
	bf3::core::Result<ui::SwfLayoutNode> tree = ui::read_layout(*assets->movies(), p_movie, ui::SwfLayoutOptions{}, budget);
	if (!tree) {
		return tree.error();
	}
	const ui::SwfLayoutNode *n = find_overview_node(*tree);
	if (n == nullptr) {
		return bf3::core::make_error("no OverviewMap instance in " + std::string(p_movie));
	}
	return n->stage;
}

std::string matrix_text(const ui::SwfMatrix &p_m) {
	return bf3::core::fmt("%.0f,%.0f x %.4f/%.4f", double(p_m.tx), double(p_m.ty), double(p_m.a), double(p_m.d));
}

std::string overview_symbol_path(const ui::SwfScene &p_scene) {
	for (const ui::SwfNodeId id : p_scene.draw_order()) {
		const ui::SwfSceneNode *n = p_scene.node(id);
		if (n != nullptr && n->symbol == kOverviewMapSymbol) {
			return n->path;
		}
	}
	return {};
}

std::optional<ui::SwfRect> overview_box(const Bf3SwfView &p_view, std::string_view p_path) {
	const std::vector<SwfEngineTarget> targets = p_view.engine_targets(kOverviewMapRenderer);
	const ui::SwfScene *scene = p_view.scene();
	const auto it =
			std::find_if(targets.begin(), targets.end(), [&](const SwfEngineTarget &t) { return t.path == p_path; });
	if (it == targets.end() || scene == nullptr) {
		return std::nullopt;
	}
	const ui::SwfSceneNode *n = scene->node(it->node);
	if (n == nullptr) {
		return std::nullopt;
	}
	const ui::SwfRect &b = it->local_bounds;
	const SwfStageFit f = p_view.stage_fit();
	const ui::SwfMatrix &m = n->world;
	ui::SwfRect out{ 1e9f, -1e9f, 1e9f, -1e9f };
	for (const float x : { b.x_min, b.x_max }) {
		for (const float y : { b.y_min, b.y_max }) {
			const float sx = float((m.a * x + m.c * y + m.tx) * f.scale_x + f.offset_x);
			const float sy = float((m.b * x + m.d * y + m.ty) * f.scale_y + f.offset_y);
			out.x_min = std::min(out.x_min, sx);
			out.x_max = std::max(out.x_max, sx);
			out.y_min = std::min(out.y_min, sy);
			out.y_max = std::max(out.y_max, sy);
		}
	}
	return out;
}

bool box_near(const ui::SwfRect &p_a, const ui::SwfRect &p_b) {
	return near(p_a.x_min, p_b.x_min, kOverviewBoxTolerance) && near(p_a.x_max, p_b.x_max, kOverviewBoxTolerance) &&
			near(p_a.y_min, p_b.y_min, kOverviewBoxTolerance) && near(p_a.y_max, p_b.y_max, kOverviewBoxTolerance);
}

std::string box_text(const ui::SwfRect &p_r) {
	return bf3::core::fmt("%.0f..%.0f x %.0f..%.0f", double(p_r.x_min), double(p_r.x_max), double(p_r.y_min), double(p_r.y_max));
}

}

struct Bf3UiEffectsCheck::State {
	Bf3LevelSession *session = nullptr;
	Bf3UiPostProcess *post = nullptr;
	std::string shot_dir;
	double clock = 0.0;
	double ready_at = -1.0;
	int phase = 0;
	int wait = 0;
	bool failed = false;
	bool done = false;
	uint32_t rows = 0;

	std::thread loader;
	std::atomic<bool> loaded{ false };
	std::vector<ui::PostProcessAction> actions;
	std::string load_error;

	Ref<Image> base;
	Ref<Image> graded;
	Ref<Image> after;
	Ref<Image> glitched;
	Ref<Image> deploy;
	double dim_diff = 0.0;

	Bf3UiCursor *cursor = nullptr;
	Vector2 cursor_at;
	float cursor_pixels = 1.0f;
	Rgb cursor_fill;

	Bf3OverviewMap *overview = nullptr;
	CanvasLayer *overview_layer = nullptr;
	Bf3SwfView *overview_view = nullptr;
	std::string overview_path;
	ui::SwfMatrix deploy_placement;
	ui::SwfMatrix menu_placement;
	int overview_waits = 0;
};

Bf3UiEffectsCheck::Bf3UiEffectsCheck() :
		state_(std::make_unique<State>()) {}

Bf3UiEffectsCheck::~Bf3UiEffectsCheck() {
	if (state_ && state_->loader.joinable()) {
		state_->loader.join();
	}
}

void Bf3UiEffectsCheck::_bind_methods() {
}

void Bf3UiEffectsCheck::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	State &s = *state_;
	s.shot_dir = user_arg_value("shot-dir");
	s.session = Object::cast_to<Bf3LevelSession>(get_node_or_null(NodePath("../Session")));
	set_process(true);
}

void Bf3UiEffectsCheck::row(const std::string &p_name, bool p_ok, const std::string &p_detail) {
	state_->rows++;
	if (!p_ok) {
		state_->failed = true;
	}
	UtilityFunctions::print(to_godot("[ui effects] " + p_name + ": " + (p_ok ? "PASS" : "FAIL") + " " + p_detail));
}

void Bf3UiEffectsCheck::finish() {
	State &s = *state_;
	if (s.done) {
		return;
	}
	s.done = true;
	if (s.loader.joinable()) {
		s.loader.join();
	}
	UtilityFunctions::print(to_godot(std::string("UI EFFECTS CHECK: ") + (s.failed || s.rows == 0 ? "FAIL" : "PASS") + " (" + std::to_string(s.rows) + " rows)"));
	const int code = s.failed || s.rows == 0 ? 1 : 0;
	if (s.session != nullptr) {
		s.session->finish(code);
	} else {
		get_tree()->quit(code);
	}
}

void Bf3UiEffectsCheck::_process(double p_delta) {
	State &s = *state_;
	if (s.done) {
		return;
	}
	s.clock += p_delta;
	if (s.clock > kTimeoutSeconds) {
		row("timeout", false, "phase " + std::to_string(s.phase) + " after " + std::to_string(int(kTimeoutSeconds)) + " s");
		finish();
		return;
	}
	if (s.wait > 0) {
		--s.wait;
		return;
	}
	const auto capture = [&]() {
		Ref<Image> img = get_viewport()->get_texture()->get_image();
		if (img.is_valid() && img->get_format() != Image::FORMAT_RGB8) {
			img->convert(Image::FORMAT_RGB8);
		}
		return img;
	};
	const auto save = [&](const Ref<Image> &p_img, const char *p_name) {
		if (s.shot_dir.empty() || p_img.is_null()) {
			return;
		}
		DirAccess::make_dir_recursive_absolute(to_godot(s.shot_dir));
		const std::string path = s.shot_dir + "/" + p_name;
		if (p_img->save_png(to_godot(path)) != OK) {
			row(std::string("save ") + p_name, false, path);
		}
	};
	switch (s.phase) {
		case 0: {
			if (s.session == nullptr) {
				row("session", false, "no ../Session");
				finish();
				return;
			}
			if (s.session->failed()) {
				row("session", false, "level session failed");
				finish();
				return;
			}
			if (!s.session->done() || !s.session->frame().valid) {
				return;
			}
			if (s.post == nullptr) {
				s.post = Bf3UiPostProcess::ensure(get_parent(), s.session->level_id());
				std::shared_ptr<const bf3::core::Install> install = Bf3Localisation::current() != nullptr ? Bf3Localisation::current()->install() : nullptr;
				const std::string sb = "Win32/Levels/" + s.session->level_id() + "/" + s.session->level_id();
				s.loader = std::thread([&s, install, sb] {
					if (install == nullptr) {
						s.load_error = "no install";
					} else if (bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> lib = bf3::core::EbxLibrary::open(install, { sb }, 2); !lib) {
						s.load_error = lib.error().message;
					} else if (bf3::core::Result<std::vector<ui::PostProcessAction>> a = ui::read_post_process_actions(**lib, kSpawnGraph); !a) {
						s.load_error = a.error().message;
					} else {
						s.actions = std::move(*a);
					}
					s.loaded.store(true, std::memory_order_release);
				});
				s.ready_at = s.clock;
				return;
			}
			if (s.post->has_failed()) {
				row("post process data", false, s.post->error());
				finish();
				return;
			}
			if (!s.post->is_ready() || !s.loaded.load(std::memory_order_acquire) || s.clock - s.ready_at < kSettleSeconds) {
				return;
			}
			s.loader.join();
			const ui::UiPostProcessData &d = *s.post->data();
			row("post process data", s.load_error.empty() && !s.actions.empty() && d.enabled && d.time_scale > 0.0f && s.post->noise() != nullptr,
					bf3::core::fmt("RShift %.3f Distort %.3f XPower %.2f TimeScale %.1f Highlight %.1f/%.1f noise %ux%u, %zu PostProcessEffect actions in %s %s",
							double(d.r_shift), double(d.distort), double(d.x_power), double(d.time_scale), double(d.highlight), double(d.highlight_threshold),
							s.post->noise() != nullptr ? s.post->noise()->width : 0u, s.post->noise() != nullptr ? s.post->noise()->height : 0u, s.actions.size(),
							std::string(kSpawnGraph).c_str(), s.load_error.c_str()));
			if (s.failed) {
				finish();
				return;
			}
			s.post->set_manual(true);
			s.post->step(0.0);
			s.phase = 1;
			s.wait = kCaptureFrames;
			return;
		}
		case 1: {
			s.base = capture();
			save(s.base, "ui_effects_base.png");
			s.post->dim_in();
			s.post->step(0.125);
			const float half = s.post->dim_visibility();
			const bool drawn_half = s.post->grade_drawn();
			s.post->step(0.126);
			const float full = s.post->dim_visibility();
			const float uniform = float(s.post->grade_material()->get_shader_parameter("visibility"));
			const ui::UiDimmerRamp &r = *s.post->ramp();
			row("dim ramp", near(half, 0.5, 1e-3) && drawn_half && near(full, 1.0, 1e-6) && near(uniform, 1.0, 1e-6) && r.in.length_ms == 250.0f,
					bf3::core::fmt("visibility %.3f at 125 ms, %.3f at 251 ms (uniform %.3f), ramp %.0f ms over %s", double(half), double(full), double(uniform),
							double(r.in.length_ms), r.visual_environment.c_str()));
			s.phase = 2;
			s.wait = kCaptureFrames;
			return;
		}
		case 2: {
			s.graded = capture();
			save(s.graded, "ui_effects_dim.png");
			const ui::MenuGrade &g = *s.post->grade();
			const std::optional<double> br = median_ratio(s.graded, 2, 0);
			const std::optional<double> gr = median_ratio(s.graded, 1, 0);
			const double want_br = double(g.brightness[2]) / double(g.brightness[0]);
			const double want_gr = double(g.brightness[1]) / double(g.brightness[0]);
			const std::optional<double> base_br = median_ratio(s.base, 2, 0);
			row("dim grade", br && gr && near(*br, want_br, want_br * 0.12) && near(*gr, want_gr, want_gr * 0.12),
					bf3::core::fmt("median B/R %.3f (grade %.3f, before %.3f), G/R %.3f (grade %.3f)", br.value_or(-1.0), want_br, base_br.value_or(-1.0),
							gr.value_or(-1.0), want_gr));
			s.post->dim_out();
			s.post->step(0.125);
			const float half = s.post->dim_visibility();
			s.post->step(0.126);
			row("dim out", near(half, 0.5, 1e-3) && s.post->dim_visibility() == 0.0f && !s.post->grade_drawn() && !s.post->dim_enabled() &&
							s.post->stats().dim_ins == 1 && s.post->stats().dim_outs == 1,
					bf3::core::fmt("visibility %.3f at 125 ms, %.3f at 251 ms, grade drawn %d", double(half), double(s.post->dim_visibility()), int(s.post->grade_drawn())));
			s.phase = 3;
			s.wait = kCaptureFrames;
			return;
		}
		case 3: {
			s.after = capture();
			const double graded = mean_abs_diff(s.graded, s.base);
			s.dim_diff = mean_abs_diff(s.after, s.base);
			row("dim gone", graded > 0.0 && s.dim_diff >= 0.0 && s.dim_diff < graded * 0.25,
					bf3::core::fmt("mean |after - before| %.4f against |graded - before| %.4f", s.dim_diff, graded));
			const float power = s.actions.front().power;
			s.post->trigger_glitch(power);
			s.post->step(0.0);
			const ui::UiPostProcessData &d = *s.post->data();
			const Ref<ShaderMaterial> m = s.post->glitch_material();
			const auto param = [&](const char *p_name) { return double(float(m->get_shader_parameter(p_name))); };
			row("glitch trigger", near(s.post->glitch_power(), power, 1e-6) && s.post->glitch_drawn() && near(param("power"), power, 1e-6) &&
							near(param("r_shift"), d.r_shift, 1e-7) && near(param("distort"), d.distort, 1e-7) && near(param("x_power"), d.x_power, 1e-7) &&
							near(param("y_power"), d.y_power, 1e-7) && near(param("time_scale"), d.time_scale, 1e-7) && near(param("highlight"), d.highlight, 1e-7) &&
							near(param("highlight_threshold"), d.highlight_threshold, 1e-7) && s.post->stats().last_trigger_power == power,
					bf3::core::fmt("power %.3f from %s %s, shader RShift %.3f Distort %.3f XPower %.2f TimeScale %.1f Highlight %.1f/%.1f", double(power),
							std::string(kSpawnGraph).c_str(), s.actions.front().name.c_str(), param("r_shift"), param("distort"), param("x_power"), param("time_scale"),
							param("highlight"), param("highlight_threshold")));
			s.phase = 4;
			s.wait = kCaptureFrames;
			return;
		}
		case 4: {
			s.glitched = capture();
			save(s.glitched, "ui_effects_glitch.png");
			const double diff = mean_abs_diff(s.glitched, s.after);
			row("glitch drawn", diff > 0.01 && diff > s.dim_diff * 3.0, bf3::core::fmt("mean |glitch - before| %.4f against frame noise %.4f", diff, s.dim_diff));
			const ui::UiPostProcessData &d = *s.post->data();
			const float power = s.actions.front().power;
			const double life = ui::ui_glitch_seconds(d, power);
			s.post->step(life * 0.5);
			const float mid = s.post->glitch_power();
			s.post->step(life * 0.5 + 1e-4);
			row("glitch decay", near(mid, power * 0.5, 1e-4) && s.post->glitch_power() == 0.0f && !s.post->glitch_drawn(),
					bf3::core::fmt("power %.3f at %.1f ms, 0 after %.1f ms (TimeScale %.1f per s)", double(mid), life * 500.0, life * 1000.0, double(d.time_scale)));
			s.post->dim_in();
			s.post->step(0.3);
			s.post->trigger_glitch(power);
			s.post->step(0.0);
			s.phase = 5;
			s.wait = kCaptureFrames;
			return;
		}
		case 5: {
			save(capture(), "ui_effects_glitch_dim.png");
			s.post->step(1.0);
			s.phase = 6;
			s.wait = kCaptureFrames;
			return;
		}
		case 6: {
			s.deploy = capture();
			save(s.deploy, "ui_effects_deploy.png");
			row("frames", s.post->stats().glitch_frames >= 2 && s.post->stats().dim_frames >= 4,
					bf3::core::fmt("%u glitch frames, %u dim frames, %u triggers", s.post->stats().glitch_frames, s.post->stats().dim_frames, s.post->stats().glitch_triggers));
			s.cursor = Bf3UiCursor::ensure(get_parent());
			const Rect2 visible = get_viewport()->get_visible_rect();
			s.cursor_at = visible.position + visible.size * Vector2(0.5f, 0.5f);
			s.cursor_pixels = s.deploy.is_valid() && visible.size.y > 0.0f ? float(s.deploy->get_height()) / visible.size.y : 1.0f;
			s.cursor->set_cursor_position(s.cursor_at);
			s.cursor->show_cursor();
			s.phase = 7;
			s.wait = kCaptureFrames;
			return;
		}
		case 7: {
			const Ref<Image> shown = capture();
			save(shown, "ui_effects_cursor.png");
			const float unit = s.cursor->cursor_scale() * s.cursor_pixels;
			const Vector2 tip = s.cursor_at * s.cursor_pixels;
			const Vector2 inside = tip + Vector2(3.0f, 9.0f) * unit;
			const Vector2 outside = tip + Vector2(12.0f, 4.0f) * unit;
			const Rgb fill = pixel(shown, int32_t(inside.x), int32_t(inside.y));
			s.cursor_fill = fill;
			const Rgb off = pixel(shown, int32_t(outside.x), int32_t(outside.y));
			const Rgb off_before = pixel(s.deploy, int32_t(outside.x), int32_t(outside.y));
			const auto c8 = [](float p_v) { return double(p_v) * 255.0; };
			const bool fill_ok = near(c8(fill.r), kUiCursorFillRgb[0], 4.0) && near(c8(fill.g), kUiCursorFillRgb[1], 4.0) && near(c8(fill.b), kUiCursorFillRgb[2], 4.0);
			const bool off_ok = near(off.r, off_before.r, 2.0 / 255.0) && near(off.g, off_before.g, 2.0 / 255.0) && near(off.b, off_before.b, 2.0 / 255.0);
			row("cursor shown", s.cursor->cursor_shown() && s.cursor->get_layer() > kUiGlitchLayer && fill_ok && off_ok && s.cursor->stats().shows == 1,
					bf3::core::fmt("layer %d over %d, scale %.3f, fill at (%.0f, %.0f) %.0f %.0f %.0f (art %u %u %u), beside the arrow unchanged %d", int(s.cursor->get_layer()),
							int(kUiGlitchLayer), double(s.cursor->cursor_scale()), double(inside.x), double(inside.y), c8(fill.r), c8(fill.g), c8(fill.b),
							unsigned(kUiCursorFillRgb[0]), unsigned(kUiCursorFillRgb[1]), unsigned(kUiCursorFillRgb[2]), int(off_ok)));
			s.cursor->hide_cursor();
			s.phase = 8;
			s.wait = kCaptureFrames;
			return;
		}
		case 8: {
			const Ref<Image> hidden = capture();
			const float unit = s.cursor->cursor_scale() * s.cursor_pixels;
			const Vector2 inside = s.cursor_at * s.cursor_pixels + Vector2(3.0f, 9.0f) * unit;
			const Rgb now = pixel(hidden, int32_t(inside.x), int32_t(inside.y));
			const Rgb before = pixel(s.deploy, int32_t(inside.x), int32_t(inside.y));
			const bool gone = near(now.r, before.r, 2.0 / 255.0) && near(now.g, before.g, 2.0 / 255.0) && near(now.b, before.b, 2.0 / 255.0);
			const float shown_diff = std::max({ std::fabs(s.cursor_fill.r - now.r), std::fabs(s.cursor_fill.g - now.g), std::fabs(s.cursor_fill.b - now.b) });
			row("cursor hidden", !s.cursor->cursor_shown() && gone && shown_diff > 0.25f && s.cursor->stats().hides == 1,
					bf3::core::fmt("pixel under the arrow %.0f %.0f %.0f, before the cursor %.0f %.0f %.0f, while shown %.0f %.0f %.0f", double(now.r) * 255.0,
							double(now.g) * 255.0, double(now.b) * 255.0, double(before.r) * 255.0, double(before.g) * 255.0, double(before.b) * 255.0,
							double(s.cursor_fill.r) * 255.0, double(s.cursor_fill.g) * 255.0, double(s.cursor_fill.b) * 255.0));
			s.overview_layer = memnew(CanvasLayer);
			s.overview_layer->set_name("OverviewCheckUi");
			get_parent()->add_child(s.overview_layer);
			s.overview_view = memnew(Bf3SwfView);
			s.overview_view->set_name("OverviewMapView");
			s.overview_view->set_playing(false);
			s.overview_layer->add_child(s.overview_view);
			const bf3::core::Result<ui::SwfMatrix> deploy_at = screen_overview_placement(kDeployScreenMovie);
			const bf3::core::Result<ui::SwfMatrix> menu_at = screen_overview_placement(kMenuScreenMovie);
			const bf3::core::Status read = !deploy_at ? deploy_at.error() : !menu_at ? menu_at.error() : bf3::core::Status::success();
			if (read) {
				s.deploy_placement = *deploy_at;
				s.menu_placement = *menu_at;
			}
			const bf3::core::Status opened = read ? s.overview_view->open("overviewmap") : read;
			s.overview_path = opened ? overview_symbol_path(*s.overview_view->scene()) : std::string();
			const bf3::core::Status placed = s.overview_path.empty() ? bf3::core::make_error("no OverviewMap instance")
																	 : s.overview_view->scene()->set_matrix(s.overview_path, s.deploy_placement);
			s.overview = Bf3OverviewMap::ensure(get_parent());
			const bf3::core::Status attached = opened && placed ? s.overview->attach(s.overview_view) : bf3::core::make_error("not opened");
			std::string detail = s.overview_view->scene() != nullptr ? s.overview_view->scene()->movie_name() + " " + s.overview_path : std::string();
			detail += bf3::core::fmt(", OverviewMap_01 at %s in %s and %s in %s", matrix_text(s.deploy_placement).c_str(), std::string(kDeployScreenMovie).c_str(),
					matrix_text(s.menu_placement).c_str(), std::string(kMenuScreenMovie).c_str());
			for (const bf3::core::Status *st : { &opened, &placed, &attached }) {
				if (!*st) {
					detail += " " + st->error().message;
				}
			}
			row("overview attach", opened && placed && attached && s.overview->stats().attached == 1, detail);
			if (s.failed) {
				finish();
				return;
			}
			s.phase = 9;
			return;
		}
		case 9: {
			const std::optional<ui::MinimapWorldRect> area = s.overview->area();
			if (!area) {
				if (++s.overview_waits > 600) {
					row("overview area", false, "the minimap source never produced a combat area");
					finish();
				}
				return;
			}
			const float cx = (area->x0 + area->x1) * 0.5f;
			const float cz = (area->z0 + area->z1) * 0.5f;
			s.overview->set_marks({ { OverviewMarkKind::Dead, cx - 10.0f, cz - 25.0f, 0.0f }, { OverviewMarkKind::Enemy, cx + 40.0f, cz - 20.0f, 1.0f },
					{ OverviewMarkKind::Friendly, cx + 20.0f, cz + 10.0f, 2.0f }, { OverviewMarkKind::Squad, cx - 25.0f, cz + 5.0f, 3.0f },
					{ OverviewMarkKind::Local, cx, cz, 0.5f } });
			s.phase = 10;
			s.wait = kCaptureFrames * 2;
			return;
		}
		case 10: {
			const Ref<Image> shot = capture();
			save(shot, "ui_effects_overview_deploy.png");
			const OverviewMapStats &st = s.overview->stats();
			const std::optional<ui::SwfRect> box = overview_box(*s.overview_view, s.overview_path);
			const float sx = shot.is_valid() ? float(shot->get_width()) / 1280.0f : 1.0f;
			const float mid_y = box ? (box->y_min + box->y_max) * 0.5f : 0.0f;
			const Rgb hatch = box ? patch_mean(shot, int32_t((box->x_min + 2.0f) * sx), int32_t((mid_y - 8.0f) * sx), int32_t(16.0f * sx)) : Rgb{};
			const Rgb inside = box ? patch_mean(shot, int32_t(((box->x_min + box->x_max) * 0.5f - 40.0f) * sx), int32_t((mid_y - 28.0f) * sx), int32_t(16.0f * sx)) : Rgb{};
			const float hatch_rb = hatch.r / std::max(hatch.b, 1e-3f);
			const float inside_rb = inside.r / std::max(inside.b, 1e-3f);
			const bool placed = box && box_near(*box, kDeployMovieBox);
			row("overview deploy",
					st.maps >= 1 && st.paints >= 1 && st.skulls == 1 && st.icons == 5 && placed &&
							hatch_rb > inside_rb + 0.2f,
					bf3::core::fmt("%s target on %s, clip box %s from %s's OverviewMap_01 %s (data %s, footage %s), "
								   "%.3f stage px/m (%.3f in the clip), area x %.0f..%.0f z %.0f..%.0f, %u icons, %u "
								   "skulls, %u objectives, R/B outside the area %.2f, inside %.2f",
							std::string(kOverviewMapRenderer).c_str(), s.overview_path.c_str(),
							box ? box_text(*box).c_str() : "none", std::string(kDeployScreenMovie).c_str(),
							matrix_text(s.deploy_placement).c_str(), box_text(kDeployMovieBox).c_str(),
							box_text(kDeployFootageBox).c_str(), double(st.stage_pixels_per_metre),
							double(st.pixels_per_metre), double(st.area.x0), double(st.area.x1), double(st.area.z0),
							double(st.area.z1), st.icons, st.skulls, st.objectives, double(hatch_rb),
							double(inside_rb)));
			if (const bf3::core::Status moved = s.overview_view->scene()->set_matrix(s.overview_path, s.menu_placement); !moved) {
				row("overview menu", false, moved.error().message);
				finish();
				return;
			}
			s.phase = 11;
			s.wait = kCaptureFrames * 2;
			return;
		}
		case 11: {
			const Ref<Image> shot = capture();
			save(shot, "ui_effects_overview_menu.png");
			const std::optional<ui::SwfRect> box = overview_box(*s.overview_view, s.overview_path);
			const OverviewMapStats &st = s.overview->stats();
			const SwfViewStats &vs = s.overview_view->stats();
			row("overview menu",
					box && box_near(*box, kMenuMovieBox) && st.skulls == 1 && st.icons == 5 && vs.missing_textures == 0,
					bf3::core::fmt("%s target on %s, clip box %s from %s's OverviewMap_01 %s (data %s, footage %s), "
								   "%.3f stage px/m (%.3f in the clip), %u paints; view %u items, %u bitmap fills, %u "
								   "missing textures, %u engine targets",
							std::string(kOverviewMapRenderer).c_str(), s.overview_path.c_str(),
							box ? box_text(*box).c_str() : "none", std::string(kMenuScreenMovie).c_str(),
							matrix_text(s.menu_placement).c_str(), box_text(kMenuMovieBox).c_str(),
							box_text(kMenuFootageBox).c_str(), double(st.stage_pixels_per_metre),
							double(st.pixels_per_metre), st.paints, vs.items, vs.bitmap_fill_items, vs.missing_textures,
							vs.engine_targets));
			finish();
			return;
		}
		default:
			finish();
			return;
	}
}

}

void openbf3_register_ui_effects_check() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3UiEffectsCheck);
}
