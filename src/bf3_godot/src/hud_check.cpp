#include "hud_check.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "hud_controls.h"
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
constexpr float kAmmoStageX = 1194.0f;
const Color kBackground(0.10f, 0.12f, 0.14f, 1.0f);

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

double luminance(const Color &p_c) {
	return 0.2126 * p_c.r + 0.7152 * p_c.g + 0.0722 * p_c.b;
}

bool ink(const Color &p_c) {
	return std::max({ std::fabs(p_c.r - kBackground.r), std::fabs(p_c.g - kBackground.g), std::fabs(p_c.b - kBackground.b) }) > 0.25f;
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
	std::string label;
};

struct Bf3HudCheck::Tally {
	size_t frames = 0;
	size_t stale = 0;
	size_t ammo_bad = 0;
	size_t ammo_font_bad = 0;
	size_t ammo_size_bad = 0;
	size_t ammo_place_bad = 0;
	double ammo_stage_x_min = 1e9;
	double ammo_stage_x_max = -1e9;
	size_t crosshair_bad = 0;
	size_t gap_frames = 0;
	double max_gap_err = 0;
	size_t ads_frames = 0;
	size_t ads_hidden = 0;
	size_t hit_bad = 0;
	size_t hit_frames = 0;
	size_t own_hits = 0;
	size_t other_hits = 0;
	size_t rising = 0;
	size_t latency_bad = 0;
	size_t hit_art_missing = 0;
	bool prev_hit = false;
	size_t name_bad = 0;
	size_t icon_bad = 0;
	std::set<size_t> names_ok;
	std::set<size_t> icons_ok;
	std::set<size_t> names_seen;
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
	if (Bf3UiAssets *assets = Bf3UiAssets::current()) {
		assets->set_atlas_movies(Bf3Hud::required_atlas_movies());
	}
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
	hud_ = memnew(Bf3Hud);
	hud_->set_name("Hud");
	viewport_->add_child(hud_);
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
				fr.ammo_visible = alive && row.show_ammo;
				fr.crosshair_visible = alive && !zoomed;

				fr.hit_visible = double(int64_t(n) - last_own_hit) < host::kHitIndicatorSeconds * kModelHz;
				fr.name_visible = alive;
				fr.health_visible = alive;
				const std::string size = std::to_string(kResolutions[r].w) + "x" + std::to_string(kResolutions[r].h);
				if (i == 0) {
					if (f == 13) {
						fr.label = size + "_hit";
					} else if (f == 30) {
						fr.label = size + "_hip";
					} else if (f == 48) {
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
		if (!work_done_.load(std::memory_order_acquire) || !hud_->is_loaded()) {
			return;
		}
		worker_.join();
		const double ready_s = double(Time::get_singleton()->get_ticks_usec()) * 1e-6 - started_s_;
		add_row("load.hud", hud_->has_layout() && hud_->item_index() != nullptr,
				fmt("hudmpscreen layout %s (%zu nodes, %zu missing), %zu item ids with an icon, %zu icons failed, ready after %.2f s", hud_->has_layout() ? "read" : "MISSING",
						hud_->layout().nodes, hud_->layout().missing.size(), hud_->icon_count(), hud_->icons_failed(), ready_s));
		add_row("load.fonts", hud_->number_font_name().find("=system") == std::string::npos && hud_->text_font_name().find("=system") == std::string::npos,
				hud_->number_font_name() + ", " + hud_->text_font_name());
		add_row("load.hit_art", !hud_->hit_pieces(host::kHitIndicatorLevel).empty(),
				fmt("%zu atlas pieces for %s %s", hud_->hit_pieces(host::kHitIndicatorLevel).size(), std::string(host::kHitIndicatorMovie).c_str(),
						std::string(host::kHitIndicatorRegions[0]).c_str()));
		if (!work_->error.empty()) {
			add_row("load.static", false, work_->error);
			finish();
			return;
		}
		size_t filled = 0;
		if (hud_->item_index() != nullptr) {
			filled = host::apply_item_names(work_->hud_static, *hud_->item_index());
		}
		add_row("load.static", work_->hud_static.rows.size() == kItems && filled == 1,
				fmt("%zu HudStatic rows from the install's 12 kit items in %.2f s; apply_item_names filled %zu (the M67)", work_->hud_static.rows.size(), work_->seconds, filled));
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
	fr.serial = work_->model.serial;
	++next_;
	pending_verify_ = true;
}

void Bf3HudCheck::verify_frame(const Frame &p_frame) {
	Tally &t = tallies_[p_frame.res];
	++t.frames;
	const float w = float(kResolutions[p_frame.res].w);
	const float h = float(kResolutions[p_frame.res].h);
	const float s = host::hud_scale(w, h);
	const std::string where = fmt("%dx%d item %zu frame %d: ", kResolutions[p_frame.res].w, kResolutions[p_frame.res].h, p_frame.item, p_frame.f);
	const HudDrawRecord &a = hud_->ammo_widget()->record();
	const HudDrawRecord &c = hud_->crosshair_widget()->record();
	const HudDrawRecord &hi = hud_->hit_widget()->record();
	const HudDrawRecord &nm = hud_->name_widget()->record();
	const HudDrawRecord &he = hud_->health_widget()->record();
	for (const HudDrawRecord *r : { &a, &c, &hi, &nm, &he }) {
		if (r->serial != p_frame.serial) {
			++t.stale;
			t.note(where + fmt("a widget drew serial %llu, want %llu", (unsigned long long)r->serial, (unsigned long long)p_frame.serial));
			break;
		}
	}

	bool ammo_ok = a.visible == p_frame.ammo_visible;
	if (ammo_ok && a.visible) {
		ammo_ok = a.text_a == std::to_string(p_frame.magazine) && a.text_b == std::to_string(p_frame.reserve);
		if (a.font.rfind(std::string(host::kHudNumberFont) + "=", 0) != 0 || a.font.find("=system") != std::string::npos) {
			++t.ammo_font_bad;
		}
		const host::HudTextSlot &clip = hud_->slot_clip();
		const host::HudTextSlot &res = hud_->slot_reserve();
		if (a.size_a != std::max(1, int32_t(std::lround(clip.size * clip.scale_y * s))) || a.size_b != std::max(1, int32_t(std::lround(res.size * res.scale_y * s)))) {
			++t.ammo_size_bad;
		}

		const double stage_x = 1280.0 + (double(a.at_a.x) - double(w)) / double(s);
		t.ammo_stage_x_min = std::min(t.ammo_stage_x_min, stage_x);
		t.ammo_stage_x_max = std::max(t.ammo_stage_x_max, stage_x);
		if (std::fabs(stage_x - kAmmoStageX) > 2.0) {
			++t.ammo_place_bad;
		}
	}
	if (!ammo_ok) {
		++t.ammo_bad;
		t.note(where + "ammo drew '" + a.text_a + " / " + a.text_b + (a.visible ? "'" : "' hidden") + fmt(", want %d / %d", p_frame.magazine, p_frame.reserve) +
				(p_frame.ammo_visible ? "" : " hidden"));
	}

	bool cross_ok = c.visible == p_frame.crosshair_visible;
	if (p_frame.zoomed) {
		++t.ads_frames;
		t.ads_hidden += c.visible ? 0 : 1;
	}
	if (cross_ok && c.visible) {
		const double want = 0.5 * double(h) * std::tan(deg_to_rad(p_frame.spread)) / std::tan(deg_to_rad(0.5 * p_frame.vfov));
		const double err = std::fabs(c.gap_px - want);
		++t.gap_frames;
		t.max_gap_err = std::max(t.max_gap_err, err);
		if (err > kGapTolerancePx || std::fabs(c.centre.x - 0.5f * w) > 0.01f || std::fabs(c.centre.y - 0.5f * h) > 0.01f) {
			cross_ok = false;
		}
	}
	if (!cross_ok) {
		++t.crosshair_bad;
		t.note(where + fmt("crosshair %s gap %.3f (want %s, spread %.3f vfov %.1f)", c.visible ? "shown" : "hidden", c.gap_px, p_frame.crosshair_visible ? "shown" : "hidden",
							   p_frame.spread, p_frame.vfov));
	}

	if (hi.visible != p_frame.hit_visible) {
		++t.hit_bad;
		t.note(where + std::string("hit indicator ") + (hi.visible ? "shown" : "hidden") + (p_frame.hit_visible ? ", want shown" : ", want hidden"));
	}
	if (hi.visible) {
		++t.hit_frames;
		t.hit_art_missing += hi.art ? 0 : 1;
		if (!t.prev_hit) {
			++t.rising;
		}
	}
	t.prev_hit = hi.visible;
	if (p_frame.own_hit) {
		++t.own_hits;
		if (!hi.visible) {
			++t.latency_bad;
		}
	}
	t.other_hits += p_frame.other_hit ? 1 : 0;

	if (p_frame.name_visible) {
		t.names_seen.insert(p_frame.item);
		const bool name_ok = nm.visible && nm.text_a == kKitItems[p_frame.item].name;
		const bool icon_ok = nm.visible && nm.art && nm.icon_item == work_->hud_static.rows[p_frame.item].item_id && nm.icon_item != 0;
		if (name_ok) {
			t.names_ok.insert(p_frame.item);
		} else {
			++t.name_bad;
			t.note(where + "name '" + nm.text_a + "', want '" + kKitItems[p_frame.item].name + "'");
		}
		if (icon_ok) {
			t.icons_ok.insert(p_frame.item);
		} else {
			++t.icon_bad;
			t.note(where + fmt("icon of item %u not drawn", work_->hud_static.rows[p_frame.item].item_id));
		}
	} else if (nm.visible) {
		++t.name_bad;
		t.note(where + "name shown while dead");
	}

	const bool health_ok = he.visible == p_frame.health_visible && (!he.visible || he.text_a == std::to_string(std::lround(p_frame.health)));
	if (!health_ok) {
		++t.health_bad;
		t.note(where + "health '" + he.text_a + "'" + (he.visible ? "" : " hidden"));
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
	const HudDrawRecord &c = hud_->crosshair_widget()->record();
	const HudDrawRecord &a = hud_->ammo_widget()->record();
	const HudDrawRecord &nm = hud_->name_widget()->record();
	const int row = int(std::floor(cy));
	const int reach = int(std::ceil(160.0f * s));

	if (p_frame.crosshair_visible && c.visible && work_->hud_static.rows[p_frame.item].crosshair != host::HudCrosshairKind::Knife) {
		++t.pixel_checks;
		int first = -1;
		for (int x = int(std::floor(cx)) + 1; x < int(cx) + reach; ++x) {
			if (luminance(at(x, row)) > 0.5) {
				first = x;
				break;
			}
		}

		const double edge = first < 0 ? 1e9 : double(first) + 0.5 - (double(cx) + c.gap_px);
		const double err = std::fabs(edge) <= 0.5 ? 0.0 : std::fabs(edge) - 0.5;
		++t.edge_checks;
		t.max_edge_err = std::max(t.max_edge_err, first < 0 ? 1e9 : std::fabs(double(first) + 0.5 - (double(cx) + c.gap_px)));
		if (first < 0 || err > kEdgeTolerancePx) {
			++t.pixel_bad;
			t.note(p_frame.label + fmt(": crosshair inner edge at x %d, want %.2f", first, double(cx) + c.gap_px));
		}
	}
	if (p_frame.zoomed) {
		++t.pixel_checks;
		size_t lit = 0;
		for (int x = int(cx) - reach / 2; x < int(cx) + reach / 2; ++x) {
			lit += luminance(at(x, row)) > 0.5 ? 1 : 0;
			lit += luminance(at(int(cx), row + (x - int(cx)))) > 0.5 ? 1 : 0;
		}
		if (lit != 0) {
			++t.pixel_bad;
			t.note(p_frame.label + fmt(": %zu crosshair pixels in ADS", lit));
		}
	}

	{
		++t.pixel_checks;
		size_t inked = 0;
		const int half = int(std::ceil(13.0f * s));
		const int off = int(std::lround(18.0f * s));
		for (int q = 0; q < 4; ++q) {
			const int qx = int(cx) + ((q & 1) != 0 ? off : -off);
			const int qy = int(cy) + ((q & 2) != 0 ? off : -off);
			for (int y = qy - half; y <= qy + half; ++y) {
				for (int x = qx - half; x <= qx + half; ++x) {
					if (std::abs(x - int(cx)) < int(4.0f * s) + 2 || std::abs(y - int(cy)) < int(4.0f * s) + 2) {
						continue;
					}
					inked += ink(at(x, y)) ? 1 : 0;
				}
			}
		}
		if ((inked > 0) != p_frame.hit_visible) {
			++t.pixel_bad;
			t.note(p_frame.label + fmt(": %zu hit indicator pixels, want %s", inked, p_frame.hit_visible ? "some" : "none"));
		}
	}

	if (p_frame.ammo_visible && a.visible) {
		++t.pixel_checks;
		size_t inked = 0;
		for (int y = int(a.at_a.y) - int(0.7f * float(a.size_a)); y < int(a.at_a.y); ++y) {
			for (int x = int(a.at_a.x) - int(1.2f * float(a.size_a)); x < int(a.at_a.x); ++x) {
				inked += ink(at(x, y)) ? 1 : 0;
			}
		}
		if (inked == 0) {
			++t.pixel_bad;
			t.note(p_frame.label + ": no ammo digit pixels");
		}
	}
	if (p_frame.name_visible && nm.visible && nm.art) {
		++t.pixel_checks;
		size_t inked = 0;
		for (int y = int(nm.icon_at.y); y < int(nm.icon_at.y + nm.icon_size.y); ++y) {
			for (int x = int(nm.icon_at.x); x < int(nm.icon_at.x + nm.icon_size.x); ++x) {
				inked += ink(at(x, y)) ? 1 : 0;
			}
		}
		if (inked == 0) {
			++t.pixel_bad;
			t.note(p_frame.label + ": no icon pixels");
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
				fmt("%zu frames verified (want %zu), %zu with a widget not drawing the frame's model", t.frames, kItems * kItemFrames, t.stale));
		add_row(p + "ammo", t.ammo_bad == 0 && t.ammo_font_bad == 0 && t.ammo_size_bad == 0 && t.ammo_place_bad == 0,
				fmt("%zu text mismatches, %zu font, %zu size, %zu placement; clip right end at stage x %.2f..%.2f (Ammo_01 %.0f +- 2)", t.ammo_bad, t.ammo_font_bad,
						t.ammo_size_bad, t.ammo_place_bad, t.ammo_stage_x_min, t.ammo_stage_x_max, double(kAmmoStageX)) +
						err);
		add_row(p + "crosshair", t.crosshair_bad == 0 && t.ads_hidden == t.ads_frames && t.max_gap_err <= kGapTolerancePx,
				fmt("%zu mismatches; gap max |err| %.4f px over %zu frames (<= %.1f); hidden on %zu/%zu ADS frames", t.crosshair_bad, t.max_gap_err, t.gap_frames,
						kGapTolerancePx, t.ads_hidden, t.ads_frames));
		add_row(p + "hit", t.hit_bad == 0 && t.latency_bad == 0 && t.rising == t.own_hits && t.hit_art_missing == 0,
				fmt("%zu mismatches; %zu own hits -> %zu indicators (rising edges), %zu shown late, %zu other hits not shown; %zu frames shown (%.2f per hit at %.0f Hz, "
					"%.1f s); %zu frames without the atlas art",
						t.hit_bad, t.own_hits, t.rising, t.latency_bad, t.other_hits, t.hit_frames, t.own_hits ? double(t.hit_frames) / double(t.own_hits) : 0.0, kModelHz,
						host::kHitIndicatorSeconds, t.hit_art_missing));
		add_row(p + "name", t.names_ok.size() == kItems && t.name_bad == 0, fmt("%zu/%zu kit items' localised names, %zu mismatching frames", t.names_ok.size(), kItems, t.name_bad));
		add_row(p + "icon", t.icons_ok.size() == kItems && t.icon_bad == 0, fmt("%zu/%zu _small icons drawn, %zu frames without", t.icons_ok.size(), kItems, t.icon_bad));
		add_row(p + "health", t.health_bad == 0, fmt("%zu mismatches", t.health_bad));
		add_row(p + "pixels", t.pixel_bad == 0 && t.pixel_checks > 0,
				fmt("%zu/%zu pixel checks on %zu shot frames; crosshair inner edge max |off| %.2f px over %zu (<= %.1f + half a pixel)", t.pixel_checks - t.pixel_bad,
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
