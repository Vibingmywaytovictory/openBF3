#include "hud_check.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "hud_items.h"
#include "native_hud.h"
#include "ui_assets.h"

#include "bf3/core/install_snapshot.h"
#include "bf3/core/util/format.h"
#include "bf3/host/hud_feed.h"
#include "bf3/sim/weapons/weapon_install.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <set>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;
namespace host = bf3::host;
namespace gp = bf3::core::gameplay;
namespace sw = bf3::sim::weapons;

namespace {

using bf3::core::fmt;

struct KitItem {
	const char *variant;
	const char *name;
	const char *label;
};

constexpr KitItem kKitItems[] = {
	{ "weapons/m16a4/u_m16a4", "M16A3", "m16a3" },
	{ "weapons/m4a1/u_m4a1", "M4A1", "m4a1" },
	{ "weapons/m27iar/u_m27iar", "M27 IAR", "m27iar" },
	{ "weapons/mk11/u_mk11", "MK11 MOD 0", "mk11" },
	{ "weapons/ak74m/u_ak74m", "AK-74M", "ak74m" },
	{ "weapons/aks74u/u_aks74u", "AKS-74u", "aks74u" },
	{ "weapons/rpk/u_rpk-74m", "RPK-74M", "rpk74m" },
	{ "weapons/svd/u_svd", "SVD", "svd" },
	{ "weapons/m9/u_m9", "M9", "m9" },
	{ "weapons/mp443/u_mp443", "MP443", "mp443" },
	{ "weapons/knife/u_knife", "KNIFE", "knife" },
	{ "weapons/m67/u_m67", "M67 GRENADE", "m67" },
};
constexpr size_t kItems = std::size(kKitItems);

struct Resolution {
	int w;
	int h;
};
constexpr Resolution kResolutions[] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 } };
constexpr size_t kResolutionCount = std::size(kResolutions);
constexpr size_t kItemShotResolution = 1;

constexpr double kModelHz = 64.0;
constexpr int kItemFrames = 96;
constexpr int kSettleFrames = 4;
constexpr int kHoldFrames = 2;
constexpr double kGapTolerancePx = 0.5;
constexpr double kEdgeTolerancePx = 1.0;
constexpr double kZoomOutSeconds = 0.2;
constexpr double kZoomInSeconds = 0.1;
const Color kBackground(0.10f, 0.12f, 0.14f, 1.0f);
constexpr float kOutlineContrast = 0.04f;

constexpr int kDeployEnd = 8;
constexpr int kFireEnd = 40;
constexpr int kAdsEnd = 56;
constexpr int kReloadEnd = 72;
constexpr int kAliveEnd = 88;
constexpr int kOwnHitFrames[] = { 12, 44, 76 };
constexpr int kOtherHitFrames[] = { 26, 50 };

double deg_to_rad(double p_deg) {
	return p_deg * 3.14159265358979323846 / 180.0;
}

bool ink(const Color &p_c, float p_contrast = 0.25f) {
	return std::max({ std::fabs(p_c.r - kBackground.r), std::fabs(p_c.g - kBackground.g), std::fabs(p_c.b - kBackground.b) }) > p_contrast;
}

}

struct Bf3HudCheck::Work {
	std::shared_ptr<const bf3::core::Install> install;
	std::string error;
	std::shared_ptr<const sw::WeaponTable> table;
	host::HudStatic hud_static;
	double seconds = 0;

	host::HudFeedState feed;
	ui::HudModel model{};
};

struct Bf3HudCheck::Frame {
	size_t res = 0;
	bool first_of_res = false;
	size_t item = 0;
	int f = 0;
	host::HudFeedInput input;
	std::vector<host::PresentEvent> events;
	double vfov = 55.0;
	uint64_t serial = 0;

	int32_t magazine = 0;
	int32_t reserve = 0;
	float spread = 0;
	float health = 0;
	bool ammo_visible = false;
	bool crosshair_visible = false;
	bool hit_visible = false;
	bool name_visible = false;
	bool health_visible = false;
	bool own_hit = false;
	bool other_hit = false;
	bool zoomed = false;
	double zoom_age = 0;
	int64_t hit_age = 0;
	std::string label;
};

struct Bf3HudCheck::Tally {
	size_t frames = 0;
	size_t stale = 0;
	size_t ammo_frames = 0;
	size_t ammo_bad = 0;
	size_t crosshair_bad = 0;
	size_t gap_frames = 0;
	double max_gap_err = 0;
	size_t ads_frames = 0;
	size_t ads_hidden = 0;
	size_t hip_settled = 0;
	size_t hip_opaque = 0;
	size_t hit_bad = 0;
	size_t hit_frames = 0;
	size_t own_hits = 0;
	size_t other_hits = 0;
	size_t rising = 0;
	size_t latency_bad = 0;
	bool prev_hit = false;
	size_t health_bad = 0;
	size_t shots = 0;
	size_t shots_saved = 0;
	size_t pixel_checks = 0;
	size_t pixel_bad = 0;
	double max_edge_err = 0;
	size_t edge_checks = 0;
	std::vector<std::string> errors;
	void note(const std::string &p_error) {
		if (errors.size() < 6) {
			errors.push_back(p_error);
		}
	}
};

Bf3HudCheck::Bf3HudCheck() = default;

Bf3HudCheck::~Bf3HudCheck() {
	if (worker_.joinable()) {
		worker_.join();
	}
}

void Bf3HudCheck::_exit_tree() {
	if (worker_.joinable()) {
		worker_.join();
	}
}

void Bf3HudCheck::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	check_ = has_user_arg("hud-check");
	shot_dir_ = user_arg_value("hud-shots");
	Window *w = get_window();
	w->set_content_scale_mode(Window::CONTENT_SCALE_MODE_DISABLED);
	w->set_size(Vector2i(1280, 720));
	if (check_) {
		DisplayServer::get_singleton()->window_set_vsync_mode(DisplayServer::VSYNC_DISABLED);
		Engine::get_singleton()->set_max_fps(0);
	}
	viewport_ = memnew(SubViewport);
	viewport_->set_name("HudViewport");
	viewport_->set_size(Vector2i(kResolutions[0].w, kResolutions[0].h));
	viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
	viewport_->set_disable_3d(true);
	viewport_->set_transparent_background(false);
	add_child(viewport_);
	background_ = memnew(ColorRect);
	background_->set_color(kBackground);
	background_->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	background_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	viewport_->add_child(background_);
	hud_ = memnew(Bf3NativeHud);
	hud_->set_name("NativeHud");
	hud_->set_manual(true);
	viewport_->add_child(hud_);
	items_ = memnew(Bf3HudItems);
	items_->set_name("HudItems");
	add_child(items_);
	view_ = memnew(TextureRect);
	view_->set_texture(viewport_->get_texture());
	view_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	view_->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	view_->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	view_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	add_child(view_);
	tallies_.resize(kResolutionCount);
	started_s_ = double(Time::get_singleton()->get_ticks_usec()) * 1e-6;
	set_process(true);
}

void Bf3HudCheck::start_work() {
	Bf3Localisation *loc = Bf3Localisation::current();
	work_ = std::make_unique<Work>();
	work_->install = loc->install();
	work_done_.store(false, std::memory_order_relaxed);
	Work *w = work_.get();
	worker_ = std::thread([this, w] {
		const auto t0 = std::chrono::steady_clock::now();
		bf3::core::Result<sw::InstallWeapons> weapons = sw::open_install_weapons(w->install);
		if (!weapons) {
			w->error = "open_install_weapons: " + weapons.error().message;
		} else {
			std::vector<gp::WeaponDef> defs;
			for (const KitItem &k : kKitItems) {
				bf3::core::Result<gp::WeaponDef> d = weapons->catalog->resolve(k.variant);
				if (!d) {
					w->error = std::string(k.variant) + ": " + d.error().message;
					break;
				}
				defs.push_back(std::move(*d));
			}
			if (w->error.empty()) {
				bf3::core::Result<std::shared_ptr<const sw::WeaponTable>> table = sw::make_weapon_table(defs, weapons->materials, sw::install_table_options(*weapons));
				if (!table) {
					w->error = "make_weapon_table: " + table.error().message;
				} else {
					w->table = *table;
					bf3::core::Result<host::HudStatic> st = host::make_hud_static(**table, defs);
					if (!st) {
						w->error = "make_hud_static: " + st.error().message;
					} else {
						w->hud_static = std::move(*st);
					}
				}
			}
		}
		w->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		work_done_.store(true, std::memory_order_release);
	});
}

void Bf3HudCheck::add_row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	rows_.push_back({ p_name, p_pass, p_detail });
}

void Bf3HudCheck::build_script() {
	const sw::WeaponTable &table = *work_->table;
	const host::HudStatic &st = work_->hud_static;
	frames_.clear();
	frames_.reserve(kResolutionCount * kItems * kItemFrames);
	uint64_t n = 0;
	int64_t last_own_hit = INT64_MIN / 2;
	for (size_t r = 0; r < kResolutionCount; ++r) {
		for (size_t i = 0; i < kItems; ++i) {
			const sw::WeaponSimDef &d = table.defs[i];
			const host::HudStaticRow &row = st.rows[i];
			const int32_t cap = std::max(d.magazine_capacity, 0);
			int32_t magazine = cap;
			int32_t reserve = cap * std::max(d.magazines, 0);
			const uint8_t slot = row.crosshair == host::HudCrosshairKind::Pistol ? 1 : row.crosshair == host::HudCrosshairKind::Knife ? 2 : row.crosshair == host::HudCrosshairKind::Grenade ? 3 : 0;
			for (int f = 0; f < kItemFrames; ++f, ++n) {
				Frame fr;
				fr.res = r;
				fr.first_of_res = i == 0 && f == 0;
				fr.item = i;
				fr.f = f;
				const double now = 1.0 + double(n) / kModelHz;
				const bool alive = f < kAliveEnd;
				host::PresentedWeapon &pw = fr.input.weapon;
				pw.valid = alive;
				pw.def = uint16_t(i);
				pw.active_slot = slot;
				pw.magazine_capacity = cap;
				pw.mode = 0;
				pw.time_s = now;
				float spread = 1.0f;
				double vfov = 55.0;
				bool zoomed = false;
				if (f < kDeployEnd) {
					pw.phase = sw::WeaponPhase::Deploying;
					pw.phase_progress = float(f) / float(kDeployEnd);
				} else if (f < kFireEnd) {
					pw.phase = sw::WeaponPhase::Ready;
					if ((f - kDeployEnd) % 4 == 0 && cap > 1 && !d.unlimited_ammo && magazine > 0) {
						--magazine;
					}
					spread = 0.4f + 0.07f * float(f - kDeployEnd);
				} else if (f < kAdsEnd) {
					pw.phase = sw::WeaponPhase::Ready;
					zoomed = true;
					spread = 0.15f;
					vfov = 40.0;
				} else if (f < kReloadEnd) {
					pw.phase = sw::WeaponPhase::Reloading;
					pw.phase_progress = float(f - kAdsEnd) / float(kReloadEnd - kAdsEnd);
				} else {
					if (f == kReloadEnd) {
						const int32_t take = std::min(cap - magazine, reserve);
						magazine += take;
						reserve -= take;
					}
					pw.phase = sw::WeaponPhase::Ready;
					spread = 1.2f;
					vfov = 75.0;
				}
				if (row.crosshair == host::HudCrosshairKind::Knife) {
					spread = 0.0f;
				}
				pw.magazine = magazine;
				pw.reserve = reserve;
				pw.spread_deg = spread;
				pw.zoomed = zoomed;
				fr.input.alive = alive;
				fr.input.health = !alive ? 0.0f : f < kReloadEnd ? 100.0f : 67.0f;
				fr.input.pose = f < kReloadEnd ? bf3::sim::Pose::Stand : f < 80 ? bf3::sim::Pose::Crouch : bf3::sim::Pose::Prone;
				fr.input.now_s = now;
				for (int hf : kOwnHitFrames) {
					if (f == hf) {
						host::PresentEvent e;
						e.kind = host::PresentKind::Hit;
						e.timeline = host::PresentTimeline::Immediate;
						e.flags = host::present_flags::kOwnShooter;
						e.def = uint16_t(i);
						fr.events.push_back(e);
						fr.own_hit = true;
						last_own_hit = int64_t(n);
					}
				}
				for (int hf : kOtherHitFrames) {
					if (f == hf) {
						host::PresentEvent e;
						e.kind = host::PresentKind::Hit;
						e.timeline = host::PresentTimeline::Immediate;
						e.flags = host::present_flags::kOwnVictim;
						fr.events.push_back(e);
						fr.other_hit = true;
					}
				}
				fr.vfov = vfov;
				fr.magazine = magazine;
				fr.reserve = reserve;
				fr.spread = spread;
				fr.health = fr.input.health;
				fr.zoomed = alive && zoomed;
				fr.zoom_age = zoomed ? double(f - kFireEnd) / kModelHz : f >= kAdsEnd ? double(f - kAdsEnd) / kModelHz : 1.0;
				fr.ammo_visible = alive && row.show_ammo;
				fr.crosshair_visible = alive && !zoomed;

				fr.hit_visible = double(int64_t(n) - last_own_hit) < host::kHitIndicatorSeconds * kModelHz;
				fr.hit_age = int64_t(n) - last_own_hit;
				fr.name_visible = alive;
				fr.health_visible = alive;
				const std::string size = std::to_string(kResolutions[r].w) + "x" + std::to_string(kResolutions[r].h);
				if (i == 0) {
					if (f == 13) {
						fr.label = size + "_hit";
					} else if (f == 30) {
						fr.label = size + "_hip";
					} else if (f == kAdsEnd - 1) {
						fr.label = size + "_ads";
					} else if (f == 64) {
						fr.label = size + "_reload";
					}
				}
				if (r == kItemShotResolution && f == 20) {
					fr.label = size + fmt("_item%02zu_", i) + kKitItems[i].label;
				}
				frames_.push_back(std::move(fr));
			}
		}
	}
}

void Bf3HudCheck::_process(double p_delta) {
	(void)p_delta;
	if (!check_) {
		return;
	}
	if (state_ == 0) {
		Bf3Localisation *loc = Bf3Localisation::current();
		if (loc == nullptr || loc->has_failed() || (loc->is_loaded() && !loc->install())) {
			UtilityFunctions::print(to_godot("M2A HUD CHECK: no usable install (" + (loc != nullptr ? to_std(loc->get_error()) : std::string("no Bf3Localisation")) + ")"));
			exit_code_ = 1;
			set_process(false);
			get_tree()->quit(exit_code_);
			return;
		}
		if (!loc->is_loaded()) {
			return;
		}
		if (!worker_.joinable()) {
			start_work();
			return;
		}
		if (!work_done_.load(std::memory_order_acquire) || !hud_->is_ready() || !items_->is_loaded()) {
			return;
		}
		worker_.join();
		const double ready_s = double(Time::get_singleton()->get_ticks_usec()) * 1e-6 - started_s_;
		add_row("load.hud", !hud_->has_failed() && items_->item_index() != nullptr,
				fmt("native HUD %s, %zu item ids with an icon, %zu icons failed, ready after %.2f s", hud_->has_failed() ? ("FAILED: " + hud_->error()).c_str() : "built",
						items_->icon_count(), items_->icons_failed(), ready_s));
		if (!work_->error.empty()) {
			add_row("load.static", false, work_->error);
			finish();
			return;
		}
		size_t filled = 0;
		if (items_->item_index() != nullptr) {
			filled = host::apply_item_names(work_->hud_static, *items_->item_index());
		}
		add_row("load.static", work_->hud_static.rows.size() == kItems && filled == 1,
				fmt("%zu HudStatic rows from the install's 12 kit items in %.2f s; apply_item_names filled %zu (the M67)", work_->hud_static.rows.size(), work_->seconds, filled));
		size_t names = 0;
		size_t icons = 0;
		std::string missing;
		for (size_t i = 0; i < kItems && i < work_->hud_static.rows.size(); ++i) {
			const host::HudStaticRow &row = work_->hud_static.rows[i];
			const bool named = items_->localised(row.name_sid_hash) == kKitItems[i].name;
			const bool icon = items_->icon(row.item_id).is_valid();
			names += named ? 1 : 0;
			icons += icon ? 1 : 0;
			if (!named || !icon) {
				missing += std::string(" ") + kKitItems[i].label + (named ? "" : "(name)") + (icon ? "" : "(icon)");
			}
		}
		add_row("load.names", names == kItems, fmt("%zu/%zu kit items' localised names%s", names, kItems, missing.c_str()));
		add_row("load.icons", icons == kItems, fmt("%zu/%zu _small icons decode", icons, kItems));
		build_script();
		state_ = 1;
		return;
	}
	if (state_ != 1) {
		return;
	}
	if (settle_ > 0) {
		--settle_;
		return;
	}
	if (pending_verify_) {
		pending_verify_ = false;
		const Frame &done = frames_[next_ - 1];
		verify_frame(done);
		if (!done.label.empty()) {
			hold_ = kHoldFrames;
			return;
		}
	}
	if (hold_ > 0) {
		if (--hold_ > 0) {
			return;
		}
		check_shot(frames_[next_ - 1]);
	}
	if (next_ >= frames_.size()) {
		finish();
		return;
	}
	Frame &fr = frames_[next_];
	if (fr.first_of_res && viewport_->get_size() != Vector2i(kResolutions[fr.res].w, kResolutions[fr.res].h)) {
		viewport_->set_size(Vector2i(kResolutions[fr.res].w, kResolutions[fr.res].h));
		settle_ = kSettleFrames;
		return;
	}
	hud_->set_vfov_deg(fr.vfov);
	host::fill_hud(fr.input, fr.events, work_->hud_static, work_->feed, work_->model);
	hud_->set_model(work_->model);
	hud_->step(1.0 / kModelHz);
	fr.serial = work_->model.serial;
	++next_;
	pending_verify_ = true;
}

void Bf3HudCheck::verify_frame(const Frame &p_frame) {
	Tally &t = tallies_[p_frame.res];
	++t.frames;
	const std::string where = fmt("%dx%d item %zu frame %d: ", kResolutions[p_frame.res].w, kResolutions[p_frame.res].h, p_frame.item, p_frame.f);
	const NativeHudReadout r = hud_->readout();
	if (r.serial != p_frame.serial) {
		++t.stale;
		t.note(where + fmt("the HUD stepped serial %llu, want %llu", (unsigned long long)r.serial, (unsigned long long)p_frame.serial));
	}
	const bool alive = p_frame.health_visible;

	if (p_frame.ammo_visible) {
		++t.ammo_frames;
		if (!r.ammo_visible || r.clip_text != std::to_string(p_frame.magazine) || r.reserve_text != std::to_string(p_frame.reserve)) {
			++t.ammo_bad;
			t.note(where + "ammo '" + r.clip_text + " / " + r.reserve_text + (r.ammo_visible ? "'" : "' hidden") + fmt(", want %d / %d", p_frame.magazine, p_frame.reserve));
		}
	}

	bool cross_ok = r.crosshair_widget == alive;
	if (cross_ok && alive) {
		if (!p_frame.zoomed) {
			const double want = std::round(0.5 * double(r.stage_height) * std::tan(deg_to_rad(p_frame.spread)) / std::tan(deg_to_rad(0.5 * p_frame.vfov)));
			const double err = std::fabs(double(r.crosshair_arm) - want);
			++t.gap_frames;
			t.max_gap_err = std::max(t.max_gap_err, err);
			cross_ok = err <= kGapTolerancePx;
			if (p_frame.zoom_age > kZoomInSeconds + 1.0 / kModelHz) {
				++t.hip_settled;
				const bool opaque = std::fabs(r.crosshair_alpha - 1.0f) <= 1e-3f;
				t.hip_opaque += opaque ? 1 : 0;
				cross_ok = cross_ok && opaque;
			}
		} else if (p_frame.zoom_age > kZoomOutSeconds + 1.0 / kModelHz) {
			++t.ads_frames;
			t.ads_hidden += r.crosshair_alpha <= 1e-3f ? 1 : 0;
			cross_ok = r.crosshair_alpha <= 1e-3f;
		}
	}
	if (!cross_ok) {
		++t.crosshair_bad;
		t.note(where + fmt("crosshair %s arm %.2f alpha %.3f (want %s, spread %.3f vfov %.1f stage h %.1f%s)", r.crosshair_widget ? "shown" : "hidden", r.crosshair_arm,
							   r.crosshair_alpha, alive ? "shown" : "hidden", p_frame.spread, p_frame.vfov, r.stage_height, p_frame.zoomed ? " zoomed" : ""));
	}

	if (r.hit_shown != p_frame.hit_visible) {
		++t.hit_bad;
		t.note(where + std::string("hit indicator ") + (r.hit_shown ? "shown" : "hidden") + (p_frame.hit_visible ? ", want shown" : ", want hidden"));
	}
	if (r.hit_shown) {
		++t.hit_frames;
		if (!t.prev_hit) {
			++t.rising;
		}
	}
	t.prev_hit = r.hit_shown;
	if (p_frame.own_hit) {
		++t.own_hits;
		if (!r.hit_shown) {
			++t.latency_bad;
		}
	}
	t.other_hits += p_frame.other_hit ? 1 : 0;

	if (alive && r.health_text != std::to_string(std::lround(p_frame.health))) {
		++t.health_bad;
		t.note(where + "health '" + r.health_text + "'" + fmt(", want %ld", std::lround(p_frame.health)));
	}
}

void Bf3HudCheck::check_shot(const Frame &p_frame) {
	Tally &t = tallies_[p_frame.res];
	++t.shots;
	const Ref<ViewportTexture> tex = viewport_->get_texture();
	const Ref<Image> img = tex.is_valid() ? tex->get_image() : Ref<Image>();
	const int w = kResolutions[p_frame.res].w;
	const int h = kResolutions[p_frame.res].h;
	if (img.is_null() || img->get_width() != w || img->get_height() != h) {
		++t.pixel_bad;
		t.note(p_frame.label + ": no read-back of the viewport");
		return;
	}
	const float s = host::hud_scale(float(w), float(h));
	const float cx = 0.5f * float(w);
	const float cy = 0.5f * float(h);
	const auto at = [&](int x, int y) { return img->get_pixel(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1)); };
	const NativeHudReadout r = hud_->readout();
	const int reach = int(std::ceil(160.0f * s));
	const bool alive = p_frame.health_visible;

	const std::optional<ui::SwfRect> arm = hud_->window_bounds(2, "instance1.Crosshair_01.crosshair.right");
	const std::optional<ui::SwfRect> all = hud_->window_bounds(2, "instance1.Crosshair_01.crosshair");
	const bool right_outermost = !arm || !all || arm->x_max + 0.5f >= all->x_max;
	if (alive && !p_frame.zoomed && r.crosshair_widget && r.crosshair_alpha > 0.999f && right_outermost) {
		++t.pixel_checks;
		int outer = -1;
		if (arm) {
			const int row = int(std::floor(0.5f * (arm->y_min + arm->y_max)));
			for (int x = int(cx) + reach; x > int(cx); --x) {
				if (ink(at(x, row), kOutlineContrast)) {
					outer = x;
					break;
				}
			}
		}
		const double want = arm ? double(arm->x_max) : 1e9;
		const double off = outer < 0 ? 1e9 : std::fabs(double(outer) + 0.5 - want);
		++t.edge_checks;
		t.max_edge_err = std::max(t.max_edge_err, off);
		if (outer < 0 || off > kEdgeTolerancePx + 0.5) {
			++t.pixel_bad;
			t.note(p_frame.label + fmt(": crosshair right arm outer edge at x %d, want %.2f (the right arm's drawn geometry)", outer, want));
		}
	}
	if (p_frame.zoomed) {
		++t.pixel_checks;
		size_t lit = 0;
		for (int d = -reach / 2; d < reach / 2; ++d) {
			if (std::abs(d) < int(4.0f * s) + 2) {
				continue;
			}
			lit += ink(at(int(cx) + d, int(cy))) ? 1 : 0;
			lit += ink(at(int(cx), int(cy) + d)) ? 1 : 0;
		}
		if (lit != 0) {
			++t.pixel_bad;
			t.note(p_frame.label + fmt(": %zu crosshair pixels in ADS", lit));
		}
	}
	{
		++t.pixel_checks;
		const std::optional<ui::SwfRect> hb = hud_->window_bounds(0, "instance1.Hitindicator_01.hitindicator");
		size_t inked = 0;
		if (hb) {
			for (int y = int(std::floor(hb->y_min)); y < int(std::ceil(hb->y_max)); ++y) {
				for (int x = int(std::floor(hb->x_min)); x < int(std::ceil(hb->x_max)); ++x) {
					if (std::abs(x - int(cx)) < int(4.0f * s) + 2 || std::abs(y - int(cy)) < int(4.0f * s) + 2) {
						continue;
					}
					inked += ink(at(x, y)) ? 1 : 0;
				}
			}
		}
		const bool fresh = p_frame.hit_visible && p_frame.hit_age <= 1;
		if ((!p_frame.hit_visible && inked > 0) || (fresh && inked == 0)) {
			++t.pixel_bad;
			t.note(p_frame.label + fmt(": %zu hit indicator pixels inside its shape bounds, want %s", inked, fresh ? "some" : "none"));
		}
	}
	if (p_frame.ammo_visible && r.ammo_visible) {
		++t.pixel_checks;
		const std::optional<ui::SwfRect> cb = hud_->window_bounds(0, "instance1.Ammo_01.Ammo.clip.cltxt.txtDisplay");
		size_t inked = 0;
		if (cb) {
			for (int y = int(std::floor(cb->y_min)); y < int(std::ceil(cb->y_max)); ++y) {
				for (int x = int(std::floor(cb->x_min)); x < int(std::ceil(cb->x_max)); ++x) {
					inked += ink(at(x, y)) ? 1 : 0;
				}
			}
		}
		if (inked == 0) {
			++t.pixel_bad;
			t.note(p_frame.label + ": no ammo digit pixels inside the clip text field");
		}
	}
	if (shot_dir_.empty()) {
		return;
	}
	const std::filesystem::path dir = std::filesystem::path(std::u8string(shot_dir_.begin(), shot_dir_.end()));
	const std::filesystem::path target = dir / ("hud_" + p_frame.label + ".png");
	Bf3Localisation *loc = Bf3Localisation::current();
	if (loc == nullptr || !loc->reader().write_guard().allows_write(target)) {
		t.note(p_frame.label + ": screenshot refused (inside the install)");
		return;
	}
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	const std::u8string u8 = target.u8string();
	const std::string path(u8.begin(), u8.end());
	if (img->save_png(to_godot(path)) == OK) {
		++t.shots_saved;
		UtilityFunctions::print(to_godot(fmt("[m2a hud] screenshot %s (%dx%d)", path.c_str(), w, h)));
	} else {
		t.note(p_frame.label + ": screenshot failed: " + path);
	}
}

void Bf3HudCheck::finish() {
	state_ = 2;
	set_process(false);
	for (size_t r = 0; r < tallies_.size() && !frames_.empty(); ++r) {
		const Tally &t = tallies_[r];
		const std::string p = fmt("e10.%dx%d.", kResolutions[r].w, kResolutions[r].h);
		const std::string err = t.errors.empty() ? std::string() : "; first: " + t.errors.front();
		add_row(p + "frames", t.frames == kItems * kItemFrames && t.stale == 0,
				fmt("%zu frames verified (want %zu), %zu where the native HUD had not stepped the frame's model", t.frames, kItems * kItemFrames, t.stale));
		add_row(p + "ammo", t.ammo_bad == 0 && t.ammo_frames > 0,
				fmt("hudmpscreen Ammo_01 clip / reserve text == magazine / reserve on %zu frames, %zu mismatches", t.ammo_frames, t.ammo_bad) + (t.ammo_bad ? err : std::string()));
		add_row(p + "crosshair", t.crosshair_bad == 0 && t.max_gap_err <= kGapTolerancePx && t.ads_frames > 0 && t.ads_hidden == t.ads_frames && t.hip_opaque == t.hip_settled,
				fmt("%zu mismatches; arms at round((h/2) tan(spread) / tan(vfov/2)) stage px, max |err| %.4f over %zu frames (<= %.1f); alpha 0 on %zu/%zu settled ADS "
					"frames, 1 on %zu/%zu settled hip frames",
						t.crosshair_bad, t.max_gap_err, t.gap_frames, kGapTolerancePx, t.ads_hidden, t.ads_frames, t.hip_opaque, t.hip_settled) +
						(t.crosshair_bad ? err : std::string()));
		add_row(p + "hit", t.hit_bad == 0 && t.latency_bad == 0 && t.rising == t.own_hits,
				fmt("%zu mismatches; %zu own hits -> %zu indicators (rising edges), %zu shown late, %zu other hits not shown; %zu frames shown (%.2f per hit at %.0f Hz, "
					"%.1f s)",
						t.hit_bad, t.own_hits, t.rising, t.latency_bad, t.other_hits, t.hit_frames, t.own_hits ? double(t.hit_frames) / double(t.own_hits) : 0.0, kModelHz,
						host::kHitIndicatorSeconds));
		add_row(p + "health", t.health_bad == 0, fmt("%zu mismatches", t.health_bad));
		add_row(p + "pixels", t.pixel_bad == 0 && t.pixel_checks > 0,
				fmt("%zu/%zu pixel checks on %zu shot frames; crosshair arm outer edge max |off| %.2f px over %zu (<= %.1f + half a pixel)", t.pixel_checks - t.pixel_bad,
						t.pixel_checks, t.shots, t.max_edge_err, t.edge_checks, kEdgeTolerancePx) +
						(t.pixel_bad ? err : std::string()));
		if (!shot_dir_.empty()) {
			add_row(p + "shots", t.shots_saved == t.shots, fmt("%zu/%zu PNGs in %s", t.shots_saved, t.shots, shot_dir_.c_str()));
		}
	}
	Bf3Localisation *loc = Bf3Localisation::current();
	if (loc == nullptr || !loc->install_snapshot_at_start().valid) {
		add_row("install.unchanged", false, "no snapshot at start");
	} else {
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(loc->install_snapshot_at_start(), loc->reader().snapshot());
		add_row("install.unchanged", d.clean(), std::to_string(loc->install_snapshot_at_start().files.size()) + " index files" + (d.clean() ? "" : ": " + d.summary()));
	}
	size_t passed = 0;
	for (const Row &r : rows_) {
		passed += r.pass ? 1 : 0;
		UtilityFunctions::print(to_godot(fmt("  %-4s %-26s ", r.pass ? "PASS" : "FAIL", r.name.c_str()) + r.detail));
	}
	const bool ok = passed == rows_.size() && !rows_.empty();
	const double total = double(Time::get_singleton()->get_ticks_usec()) * 1e-6 - started_s_;
	UtilityFunctions::print(to_godot(fmt("M2A HUD CHECK: %s (%zu/%zu) in %.1f s", ok ? "PASS" : "FAIL", passed, rows_.size(), total)));
	exit_code_ = ok ? 0 : 1;
	if (worker_.joinable()) {
		worker_.join();
	}
	get_tree()->quit(exit_code_);
}

}
