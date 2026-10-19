// PT1 iff: the minimap's soldiers, the nametags and the --iff-shots capture (pt1_iff.h; docs/design/pt1-iff.md).
#include "pt1_iff.h"

#include "godot_util.h"
#include "input_mapper.h"
#include "level_session.h"
#include "level_session_m2a.h"
#include "ui_assets.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/install.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/static_world.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace godot;
namespace ui = bf3::core::ui;

namespace bf3::godot_side {

namespace {

constexpr const char *kPlayerIcon = "UIHudIcon_Player";
// The hud3dicons kit icons by host::KitRole when the kit data was not read (gameplay/kits/us* UIHudIcon, verified:
// bf3dump kits MP_Subway TDM).
constexpr std::array<const char *, 4> kKitIconDefaults{ "UIHudIcon_KitAssault", "UIHudIcon_KitEngineer", "UIHudIcon_KitSupport", "UIHudIcon_KitRecon" };
constexpr double kSummaryEveryS = 30.0;

ui::HudIconState state_of(ui::TagRelation p_relation) {
	switch (p_relation) {
		case ui::TagRelation::Friendly:
			return ui::HudIconState::Friendly;
		case ui::TagRelation::Squad:
			return ui::HudIconState::Squad;
		case ui::TagRelation::Enemy:
			return ui::HudIconState::Enemy;
		case ui::TagRelation::None:
			break;
	}
	return ui::HudIconState::Neutral;
}

Color colour(const std::array<float, 4> &p_c, float p_alpha) {
	return Color(p_c[0], p_c[1], p_c[2], p_c[3] * p_alpha);
}

uint8_t own_team_of(const host::PresentationFrame &p_frame) {
	if (p_frame.own_team == 1 || p_frame.own_team == 2) {
		return p_frame.own_team;
	}
	const size_t rows = std::min<size_t>(p_frame.player_count, p_frame.players.size());
	for (size_t i = 0; i < rows; ++i) {
		if (p_frame.players[i].slot == p_frame.stats.slot && (p_frame.players[i].team == 1 || p_frame.players[i].team == 2)) {
			return p_frame.players[i].team;
		}
	}
	return 0;
}

Vector3 vec(const sim::Vec3 &p_v) {
	return Vector3(p_v[0], p_v[1], p_v[2]);
}

} // namespace

// ---- Bf3Nametags ----------------------------------------------------------------------------------------------------

void Bf3Nametags::_bind_methods() {}

void Bf3Nametags::setup(Ref<ImageTexture> p_atlas, const ui::HudIconAtlas *p_icons) {
	atlas_ = p_atlas;
	icons_ = p_icons;
	set_mouse_filter(MOUSE_FILTER_IGNORE);
	for (size_t k = 0; k < kit_icons_.size(); ++k) {
		kit_icons_[k] = kKitIconDefaults[k];
	}
}

void Bf3Nametags::set_style(const ui::NametagStyle &p_style, const std::array<std::string, 4> &p_kit_icons) {
	style_ = p_style;
	for (size_t k = 0; k < kit_icons_.size(); ++k) {
		if (!p_kit_icons[k].empty()) {
			kit_icons_[k] = p_kit_icons[k];
		}
	}
}

void Bf3Nametags::update(std::vector<NametagDraw> p_draws, float p_canvas_scale) {
	draws_ = std::move(p_draws);
	canvas_scale_ = std::isfinite(p_canvas_scale) && p_canvas_scale > 0.0f ? p_canvas_scale : 1.0f;
	queue_redraw();
}

void Bf3Nametags::draw_frame(const ui::HudIcon *p_icon, ui::HudIconState p_state, Vector2 p_centre, float p_size, float p_alpha, const Color &p_modulate) {
	if (p_icon == nullptr || !atlas_.is_valid()) {
		return;
	}
	const ui::HudIconStateFrames *st = p_icon->state(p_state);
	if (st == nullptr) {
		st = p_icon->state(ui::HudIconState::Default);
	}
	if (st == nullptr || st->frames.empty()) {
		return;
	}
	const ui::HudIconRect r = ui::hud_icon_rect(st->frames.front(), uint32_t(atlas_->get_width()), uint32_t(atlas_->get_height()));
	if (r.width <= 0 || r.height <= 0) {
		return;
	}
	// One atlas texel per stage unit, capped by IconSize (the larger side) [inferred]; then the stage -> canvas scale.
	const float texel = std::min(1.0f, p_size / float(std::max(r.width, r.height))) * canvas_scale_;
	const float w = float(r.width) * texel, h = float(r.height) * texel;
	draw_texture_rect_region(atlas_, Rect2(r.flip_x ? p_centre.x + w * 0.5f : p_centre.x - w * 0.5f, r.flip_y ? p_centre.y + h * 0.5f : p_centre.y - h * 0.5f, r.flip_x ? -w : w, r.flip_y ? -h : h),
			Rect2(float(r.x), float(r.y), float(r.width), float(r.height)), Color(p_modulate.r, p_modulate.g, p_modulate.b, p_modulate.a * p_alpha));
}

void Bf3Nametags::_draw() {
	if (icons_ == nullptr) {
		return;
	}
	const ui::HudIcon *player = icons_->find(kPlayerIcon);
	Ref<Font> font = font_.is_valid() ? font_ : ThemeDB::get_singleton()->get_fallback_font();
	const int font_size = std::max(6, int(std::lround(style_.name_font_size * canvas_scale_)));
	const int glow = std::max(1, int(std::lround(style_.name_glow_size * 0.5f * canvas_scale_))); // NameGlowSize as an outline [inferred]
	for (const NametagDraw &d : draws_) {
		const float icon_size = style_.icon_size * d.scale;
		float top = d.anchor.y;
		if (d.icon) {
			draw_frame(player, state_of(d.relation), d.anchor, icon_size, d.alpha);
			top -= 0.5f * std::min(icon_size, 16.0f) * canvas_scale_ + 2.0f * canvas_scale_;
		}
		if (!d.name || !font.is_valid() || d.text.is_empty()) {
			continue;
		}
		const Color tint = colour(ui::tag_tint(style_, d.relation), d.alpha);
		const Vector2 ts = font->get_string_size(d.text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size);
		// The kit icon left of the name for teammates (hud3dicons UIHudIcon_Kit*, Default / Squad state).
		float kit_w = 0.0f;
		const ui::HudIcon *kit = d.relation != ui::TagRelation::Enemy && d.kit < kit_icons_.size() ? icons_->find(kit_icons_[d.kit]) : nullptr;
		if (kit != nullptr) {
			kit_w = 18.0f * canvas_scale_;
		}
		const float total = kit_w + ts.x;
		const float x0 = d.anchor.x - total * 0.5f;
		const float baseline = top - 2.0f * canvas_scale_;
		if (kit != nullptr) {
			// The Default frame is white: drawn in the relation's tint like the name [inferred]; Squad is green already.
			const bool squad = d.relation == ui::TagRelation::Squad;
			draw_frame(kit, squad ? ui::HudIconState::Squad : ui::HudIconState::Default, Vector2(x0 + kit_w * 0.5f - 1.0f * canvas_scale_, baseline - ts.y * 0.35f), 16.0f,
					d.alpha, squad ? Color(1, 1, 1, 1) : tint);
		}
		draw_string_outline(font, Vector2(x0 + kit_w, baseline), d.text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, glow, Color(0, 0, 0, 0.55f * d.alpha));
		draw_string(font, Vector2(x0 + kit_w, baseline), d.text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, tint);
	}
}

// ---- Pt1Iff ---------------------------------------------------------------------------------------------------------

struct Pt1Iff::Loaded {
	ui::NametagStyle style;
	ui::SpottingRules spotting;
	std::array<std::string, 4> kit_icons;
	std::string notes;
	double seconds = 0.0;
};

struct Pt1Iff::Shots {
	std::string dir;
	int phase = 0; // 0 wait for the spawn, 1 views, 2 done
	double alive_since = -1.0;
	double first_s = -1.0; // the first step: the whole capture gives up after kShotsTimeoutSeconds
	struct View {
		bool enemy = false;
		float metres = 0.0f;
		uint8_t slot = 0xFF;
		sim::Vec3 spot{};
		float yaw = 0.0f;
		bool placed = false;
	};
	std::vector<View> views;
	size_t at = 0;
	double t0 = 0.0; // phase start of the current view
	int step = 0; // 0 teleport, 1 look, 2 shoot
	int failures = 0;
	int saved = 0;
};

Pt1Iff::Pt1Iff(Bf3LevelSession *p_session, CanvasLayer *p_layer, Ref<ImageTexture> p_atlas, const ui::HudIconAtlas *p_icons) :
		session_(p_session), atlas_(p_atlas), icons_(p_icons) {
	tags_ = memnew(Bf3Nametags);
	tags_->set_name("Nametags");
	tags_->setup(p_atlas, p_icons);
	if (p_layer != nullptr) {
		p_layer->add_child(tags_);
		p_layer->move_child(tags_, 0); // under the minimap and the world icons
	}
	const PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
	for (int64_t i = 0; i < args.size(); ++i) {
		const std::string a = to_std(args[i]);
		if (a.rfind("--iff-shots=", 0) == 0 && a.size() > 12) {
			shots_ = std::make_unique<Shots>();
			shots_->dir = a.substr(12);
		}
	}
	loader_ = std::thread([this]() { run_loader(); });
}

Pt1Iff::~Pt1Iff() {
	stop_.store(true);
	if (loader_.joinable()) {
		loader_.join();
	}
	if (stats_.frames > 0) {
		log_summary();
	}
}

void Pt1Iff::run_loader() {
	const auto t0 = std::chrono::steady_clock::now();
	auto out = std::make_shared<Loaded>();
	std::string notes;
	core::Result<std::shared_ptr<core::Install>> install = core::Install::open(session_->install());
	if (install) {
		const std::string level = session_->level_id();
		core::Result<std::unique_ptr<core::EbxLibrary>> lib = core::EbxLibrary::open(*install, { "Win32/Levels/" + level + "/" + level }, 2);
		if (lib && !stop_.load()) {
			core::gameplay::WorkBudget budget;
			if (core::Result<ui::NametagStyle> s = ui::read_nametag_style(**lib, &budget)) {
				out->style = *s;
			} else {
				notes += " nametag style: " + s.error().message + " (retail defaults);";
			}
			if (core::Result<ui::SpottingRules> s = ui::read_spotting_rules(**lib, &budget)) {
				out->spotting = *s;
			} else {
				notes += " spotting: " + s.error().message + " (retail defaults);";
			}
			// The kit icons by KitRole (the role from the kit's name, as host/kits.cpp maps it): team 1's kits (the
			// same UIHudIcon names for team 2).
			if (const core::EbxPartition *team = (*lib)->partition_by_name("gameplay/teams/us")) {
				if (core::Result<core::gameplay::TeamDef> td = core::gameplay::read_team_data(**lib, team->primary(), &budget)) {
					for (const std::string &kit_name : td->kits) {
						const std::string lower = core::to_lower_ascii(kit_name);
						const size_t slash = lower.find_last_of('/');
						const std::string tail = slash == std::string::npos ? lower : lower.substr(slash + 1);
						const char *roles[4] = { "assault", "engineer", "support", "recon" };
						size_t k = 4;
						for (size_t r = 0; r < 4; ++r) {
							if (tail.find(roles[r]) != std::string::npos) {
								k = r;
								break;
							}
						}
						if (k >= out->kit_icons.size()) {
							continue;
						}
						if (const core::EbxPartition *kit = (*lib)->partition_by_name(kit_name)) {
							const std::optional<core::EbxValue> v = kit->primary().get("UIHudIcon");
							const std::optional<std::string_view> name = v ? v->enum_name() : std::nullopt;
							if (name && name->size() < 64) {
								out->kit_icons[k] = std::string(*name);
							}
						}
					}
				}
			}
		} else if (!lib) {
			notes += " level library: " + lib.error().message + ";";
		}
	} else {
		notes += " install: " + install.error().message + ";";
	}
	out->notes = notes;
	out->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::lock_guard<std::mutex> lock(mutex_);
	loaded_ = out;
	published_.store(true, std::memory_order_release);
}

void Pt1Iff::log_summary() {
	const double n = double(std::max<uint64_t>(1, stats_.frames));
	char line[768];
	std::snprintf(line, sizeof(line),
			"[pt1-iff] %llu frames: minimap per frame %.1f friendly + %.1f revealed enemies (%.1f enemies hidden; the M1 loop drew %.1f others as "
			"enemies); nametags per frame %.1f friendly icons, %.2f friendly names, %.3f enemy names; LOS rays %llu (%llu blocked); "
			"%llu soldier-frames without a team; style %s, reveal %.2f s; main thread %.1f us per frame (max %.1f us)",
			(unsigned long long)stats_.frames, double(stats_.minimap_friendly) / n, double(stats_.minimap_enemy) / n, double(stats_.minimap_hidden_enemies) / n,
			double(stats_.m1_rule_enemy_icons) / n, double(stats_.friendly_icons) / n, double(stats_.friendly_names) / n, double(stats_.enemy_names) / n,
			(unsigned long long)stats_.los_rays, (unsigned long long)stats_.los_blocked, (unsigned long long)stats_.unknown_team,
			style_.from_data ? "from data" : "defaults", double(spotting_.fire_reveal_seconds()), double(stats_.frame_ns) / n * 1e-3, double(stats_.frame_max_ns) * 1e-3);
	UtilityFunctions::print(line);
}

void Pt1Iff::frame(const Camera3D *p_camera, bool p_first_person, std::vector<MinimapIconDraw> &r_icons) {
	if (session_ == nullptr || tags_ == nullptr) {
		return;
	}
	if (!applied_ && published_.load(std::memory_order_acquire)) {
		applied_ = true;
		std::shared_ptr<const Loaded> l;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			l = loaded_;
		}
		if (l != nullptr) {
			style_ = l->style;
			spotting_ = l->spotting;
			tags_->set_style(style_, l->kit_icons);
			char line[512];
			std::snprintf(line, sizeof(line),
					"[pt1-iff] nametags %s (FriendlyTint %.2f %.2f %.2f, EnemyTint %.2f %.2f %.2f, ShowLabelRange %.0f m, look-at %.0f..%.0f within %.0f m, "
					"VerticalOffset %.2f m, %u clamped), spotting %s (fire reveal %.2f s), kit icons %s %s %s %s, %.2f s;%s",
					style_.from_data ? "from data" : "retail defaults", double(style_.friendly_tint[0]), double(style_.friendly_tint[1]), double(style_.friendly_tint[2]),
					double(style_.enemy_tint[0]), double(style_.enemy_tint[1]), double(style_.enemy_tint[2]), double(style_.show_label_range),
					double(style_.max_look_at_threshold), double(style_.min_look_at_threshold), double(style_.min_look_at_threshold_distance), double(style_.vertical_offset),
					style_.clamped, spotting_.from_data ? "from data" : "retail defaults", double(spotting_.fire_reveal_seconds()), l->kit_icons[0].c_str(),
					l->kit_icons[1].c_str(), l->kit_icons[2].c_str(), l->kit_icons[3].c_str(), l->seconds, l->notes.c_str());
			UtilityFunctions::print(line);
		}
	}
	if (!font_set_) {
		if (Bf3UiAssets *assets = session_->pt1_iff_ui_assets()) {
			const Ref<Font> f = assets->get_font(String("$Menu_medium"));
			if (f.is_valid()) {
				tags_->set_font(f);
				font_set_ = true;
			}
		}
	}
	const auto t_frame = std::chrono::steady_clock::now();
	const host::PresentationFrame &f = session_->frame();
	const uint64_t now_ns = net::monotonic_ns();
	const double now_s = double(now_ns) * 1e-9;
	std::array<IffSoldier, net::kMaxSlots> soldiers{};
	const size_t n = iff_soldiers(f, now_ns, soldiers);
	const std::span<const IffSoldier> all(soldiers.data(), n);
	ui::NametagViewer viewer;
	viewer.team = own_team_of(f);
	reveal_.update(all, now_s, spotting_.fire_reveal_seconds());
	++stats_.frames;
	stats_.m1_rule_enemy_icons += n;
	for (const IffSoldier &s : all) {
		stats_.unknown_team += s.team == 0 ? 1u : 0u;
	}
	// The minimap.
	std::array<IffMinimapEntry, net::kMaxSlots> entries{};
	IffMinimapCounts counts;
	const size_t m = iff_minimap(all, viewer, reveal_, now_s, entries, &counts);
	stats_.minimap_friendly += counts.friendly + counts.squad;
	stats_.minimap_enemy += counts.enemy;
	stats_.minimap_hidden_enemies += counts.hidden_enemies;
	const ui::HudIcon *player = icons_ != nullptr ? icons_->find(kPlayerIcon) : nullptr;
	for (size_t i = 0; i < m; ++i) {
		MinimapIconDraw ic;
		ic.icon = player;
		ic.state = state_of(entries[i].relation);
		ic.world_x = entries[i].feet[0];
		ic.world_y = entries[i].feet[1];
		ic.world_z = entries[i].feet[2];
		ic.yaw = entries[i].yaw;
		ic.objective = false;
		r_icons.push_back(ic);
	}
	// The nametags.
	std::vector<NametagDraw> draws;
	const bool show = p_camera != nullptr && p_first_person && p_camera->is_inside_tree() && viewer.team != 0;
	if (show) {
		const Rect2 vis = tags_->get_viewport_rect();
		tags_->set_position(vis.position);
		tags_->set_size(vis.size);
		const float canvas_scale = vis.size.y / ui::kNametagStageHeight;
		const Vector2 centre = vis.position + vis.size * 0.5f;
		const Vector3 eye = p_camera->get_global_transform().origin;
		const sim::CollisionWorld *world = session_->pt1_iff_world();
		uint32_t rays = 0;
		for (const IffSoldier &s : all) {
			ui::NametagSubject sub;
			sub.slot = s.slot;
			sub.team = s.team;
			sub.squad = s.squad;
			sub.alive = s.alive;
			const Vector3 anchor = vec(s.feet) + Vector3(0.0f, s.eye_height + style_.vertical_offset, 0.0f);
			if (!anchor.is_finite()) {
				continue;
			}
			sub.in_front = !p_camera->is_position_behind(anchor);
			sub.distance_m = eye.distance_to(anchor);
			Vector2 screen;
			if (sub.in_front) {
				screen = p_camera->unproject_position(anchor);
				sub.crosshair_stage = (screen - centre).length() / std::max(1e-3f, canvas_scale);
			}
			ui::NametagDecision dec = ui::decide_nametag(style_, viewer, sub);
			if (dec.needs_line_of_sight && world != nullptr && rays < kMaxLosRaysPerFrame) {
				// A clear static-world ray to the chest or the head (Collision + Raycast, the bullets' set).
				const sim::Vec3 from{ eye.x, eye.y, eye.z };
				bool clear = false;
				for (const float h : { 0.75f, 1.0f }) {
					const sim::Vec3 to{ s.feet[0], s.feet[1] + s.eye_height * h, s.feet[2] };
					++rays;
					++stats_.los_rays;
					if (!world->ray_any(from, { to[0] - from[0], to[1] - from[1], to[2] - from[2] }, sim::query_mask::kProjectile)) {
						clear = true;
						break;
					}
				}
				stats_.los_blocked += clear ? 0u : 1u;
				sub.line_of_sight = clear;
				dec = ui::decide_nametag(style_, viewer, sub);
			}
			if (!dec.icon && !dec.name) {
				continue;
			}
			NametagDraw d;
			d.slot = s.slot;
			d.relation = dec.relation;
			d.icon = dec.icon;
			d.name = dec.name;
			d.scale = dec.scale;
			d.alpha = dec.alpha;
			d.distance_m = sub.distance_m;
			d.kit = s.kit;
			d.anchor = screen + Vector2(style_.pixel_offset[0], style_.pixel_offset[1]) * canvas_scale;
			d.text = s.name.empty() ? String("#") + String::num_int64(s.slot) : String::utf8(s.name.data(), int64_t(s.name.size()));
			stats_.friendly_icons += d.icon ? 1u : 0u;
			stats_.friendly_names += d.name && d.relation != ui::TagRelation::Enemy ? 1u : 0u;
			stats_.enemy_names += d.name && d.relation == ui::TagRelation::Enemy ? 1u : 0u;
			draws.push_back(std::move(d));
		}
		tags_->update(std::move(draws), canvas_scale);
	} else {
		tags_->update({}, 1.0f);
	}
	tags_->set_visible(show);
	const uint64_t spent_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_frame).count());
	stats_.frame_ns += spent_ns;
	stats_.frame_max_ns = std::max(stats_.frame_max_ns, spent_ns);
	if (summary_at_s_ < 0.0) {
		summary_at_s_ = now_s + kSummaryEveryS;
	} else if (now_s >= summary_at_s_) {
		summary_at_s_ = now_s + kSummaryEveryS;
		log_summary();
	}
	if (shots_ != nullptr) {
		step_shots(p_camera, now_s);
	}
}

// ---- --iff-shots ----------------------------------------------------------------------------------------------------

void Pt1Iff::step_shots(const Camera3D *p_camera, double p_now_s) {
	Shots &sh = *shots_;
	if (sh.phase == 2) {
		return;
	}
	constexpr double kShotsTimeoutSeconds = 240.0;
	if (sh.first_s < 0.0) {
		sh.first_s = p_now_s;
	}
	if (p_now_s - sh.first_s > kShotsTimeoutSeconds) {
		UtilityFunctions::print(to_godot("[pt1-iff] shots: timed out after " + std::to_string(int(kShotsTimeoutSeconds)) + " s in phase " + std::to_string(sh.phase) +
				" (view " + std::to_string(sh.at) + " of " + std::to_string(sh.views.size()) + ")"));
		sh.phase = 2;
		session_->mapper().set_scripted(false);
		session_->finish(1);
		return;
	}
	const host::PresentationFrame &f = session_->frame();
	const bool alive = f.valid && (f.own_curr.flags & sim::soldier_flags::kAlive) != 0;
	const uint64_t now_ns = net::monotonic_ns();
	std::array<IffSoldier, net::kMaxSlots> soldiers{};
	const size_t n = iff_soldiers(f, now_ns, soldiers);
	const uint8_t own_team = own_team_of(f);
	const sim::CollisionWorld *world = session_->pt1_iff_world();
	if (sh.phase == 0) {
		if (!alive || !applied_ || own_team == 0 || world == nullptr) {
			sh.alive_since = -1.0;
			return;
		}
		if (sh.alive_since < 0.0) {
			sh.alive_since = p_now_s;
		}
		if (p_now_s - sh.alive_since < 3.0) {
			return;
		}
		// The nearest living teammate and enemy, from where the own soldier stands.
		const sim::Vec3 &me = f.own_curr.position;
		int best[2] = { -1, -1 };
		float best_d[2] = { 1e30f, 1e30f };
		for (size_t i = 0; i < n; ++i) {
			if (!soldiers[i].alive || soldiers[i].team == 0) {
				continue;
			}
			const int k = soldiers[i].team == own_team ? 0 : 1;
			const float dx = soldiers[i].feet[0] - me[0], dz = soldiers[i].feet[2] - me[2];
			if (dx * dx + dz * dz < best_d[k]) {
				best_d[k] = dx * dx + dz * dz;
				best[k] = int(i);
			}
		}
		if (best[0] < 0 || best[1] < 0) {
			return;
		}
		session_->mapper().set_scripted(true);
		for (const int k : { 0, 1 }) {
			for (const float d : { 6.0f, 15.0f, 35.0f }) {
				Shots::View v;
				v.enemy = k == 1;
				v.metres = d;
				v.slot = soldiers[size_t(best[k])].slot;
				sh.views.push_back(v);
			}
		}
		UtilityFunctions::print(to_godot("[pt1-iff] shots: own team " + std::to_string(own_team) + ", teammate slot " +
				std::to_string(soldiers[size_t(best[0])].slot) + ", enemy slot " + std::to_string(soldiers[size_t(best[1])].slot) + ", " +
				std::to_string(sh.views.size()) + " views -> " + sh.dir));
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(std::u8string(sh.dir.begin(), sh.dir.end())), ec);
		sh.phase = 1;
		sh.at = 0;
		sh.step = 0;
		sh.t0 = p_now_s;
	}
	if (sh.at >= sh.views.size()) {
		// The atlas frames this track draws, for the user (derived data under b\shots).
		if (atlas_.is_valid() && icons_ != nullptr) {
			Ref<Image> img = atlas_->get_image();
			const char *names[] = { "UIHudIcon_Player", "UIHudIcon_KitAssault", "UIHudIcon_KitEngineer", "UIHudIcon_KitSupport", "UIHudIcon_KitRecon",
				"UIHudIcon_LocalPlayer" };
			const int cell = 32, scale = 4;
			Ref<Image> sheet = Image::create_empty(cell * 4 * scale, cell * 6 * scale, false, Image::FORMAT_RGBA8);
			sheet->fill(Color(0.25f, 0.25f, 0.25f, 1.0f));
			for (int row = 0; row < 6; ++row) {
				const ui::HudIcon *ic = icons_->find(names[row]);
				if (ic == nullptr || img.is_null()) {
					continue;
				}
				int col = 0;
				for (const ui::HudIconStateFrames &st : ic->states) {
					if (st.frames.empty() || col >= 4) {
						continue;
					}
					const ui::HudIconRect r = ui::hud_icon_rect(st.frames.front(), uint32_t(img->get_width()), uint32_t(img->get_height()));
					if (r.width <= 0 || r.height <= 0 || r.width > cell || r.height > cell) {
						++col;
						continue;
					}
					Ref<Image> crop = img->get_region(Rect2i(r.x, r.y, r.width, r.height));
					crop->resize(r.width * scale, r.height * scale, Image::INTERPOLATE_NEAREST);
					sheet->blend_rect(crop, Rect2i(0, 0, r.width * scale, r.height * scale), Vector2i(col * cell * scale, row * cell * scale));
					++col;
				}
			}
			const std::string path = sh.dir + "/iff_atlas_frames.png";
			sheet->save_png(to_godot(path));
			UtilityFunctions::print(to_godot("[pt1-iff] atlas frames (rows Player, KitAssault, KitEngineer, KitSupport, KitRecon, LocalPlayer; columns the "
											 "data's state order) -> " + path));
		}
		log_summary();
		UtilityFunctions::print(to_godot("[pt1-iff] shots done: " + std::to_string(sh.saved) + " saved, " + std::to_string(sh.failures) + " failed"));
		sh.phase = 2;
		session_->mapper().set_scripted(false);
		session_->finish(sh.failures == 0 && sh.saved > 0 ? 0 : 1);
		return;
	}
	Shots::View &v = sh.views[sh.at];
	// A spot about p_metres from p_target, on the ground, with room to stand and a clear eye-to-chest ray: 24
	// directions from the target's facing, each at 1, 0.85 and 1.2 x the distance.
	struct SpotTally {
		uint32_t no_ground = 0, blocked = 0, no_sight = 0;
	};
	auto find_spot = [&](const IffSoldier &p_target, float p_metres, sim::Vec3 &r_spot, float &r_yaw, SpotTally &r_tally) {
		const sim::Vec3 chest{ p_target.feet[0], p_target.feet[1] + p_target.eye_height * 0.75f, p_target.feet[2] };
		for (int k = 0; k < 24 * 3; ++k) {
			const int dir = k / 3;
			const float scale = (k % 3) == 0 ? 1.0f : ((k % 3) == 1 ? 0.85f : 1.2f);
			const float turn = float((dir + 1) / 2) * 0.2618f * (dir % 2 == 0 ? 1.0f : -1.0f);
			const float a = p_target.yaw + turn;
			const float r = p_metres * scale;
			const sim::Vec3 above{ p_target.feet[0] + r * std::sin(a), p_target.feet[1] + 2.0f, p_target.feet[2] + r * std::cos(a) };
			sim::StaticHit ground;
			if (!world->ground_below(above, 6.0f, sim::query_mask::kMovement, &ground) || std::fabs(ground.position[1] - p_target.feet[1]) > 3.0f ||
					ground.normal[1] < 0.7f) {
				++r_tally.no_ground;
				continue;
			}
			const sim::Vec3 spot{ ground.position[0], ground.position[1] + 0.05f, ground.position[2] };
			if (world->overlap_capsule(spot, 0.4f, 1.8f, sim::query_mask::kMovement, 0.05f)) {
				++r_tally.blocked;
				continue;
			}
			const sim::Vec3 eye{ spot[0], spot[1] + 1.6f, spot[2] };
			if (world->ray_any(eye, { chest[0] - eye[0], chest[1] - eye[1], chest[2] - eye[2] }, sim::query_mask::kProjectile)) {
				++r_tally.no_sight;
				continue;
			}
			r_spot = spot;
			r_yaw = std::atan2(p_target.feet[0] - spot[0], p_target.feet[2] - spot[2]);
			return true;
		}
		return false;
	};
	if (sh.step == 0) {
		// The chosen soldier first, then the others of its relation nearest the own soldier first, until one has a spot.
		const uint8_t want_team = [&] {
			for (size_t i = 0; i < n; ++i) {
				if (soldiers[i].slot == v.slot) {
					return soldiers[i].team;
				}
			}
			return uint8_t(v.enemy ? (own_team == 1 ? 2 : 1) : own_team);
		}();
		std::vector<std::pair<float, size_t>> order;
		const sim::Vec3 &me = f.own_curr.position;
		for (size_t i = 0; i < n; ++i) {
			if (!soldiers[i].alive || soldiers[i].team != want_team) {
				continue;
			}
			const float dx = soldiers[i].feet[0] - me[0], dz = soldiers[i].feet[2] - me[2];
			order.emplace_back(soldiers[i].slot == v.slot ? -1.0f : dx * dx + dz * dz, i);
		}
		std::sort(order.begin(), order.end());
		SpotTally tally;
		bool found = false;
		size_t tried = 0;
		for (const auto &[d2, i] : order) {
			(void)d2;
			++tried;
			if (find_spot(soldiers[i], v.metres, v.spot, v.yaw, tally)) {
				v.slot = soldiers[i].slot;
				found = true;
				break;
			}
		}
		if (!found) {
			UtilityFunctions::print(to_godot("[pt1-iff] view " + std::to_string(sh.at) + ": no clear spot " + std::to_string(int(v.metres)) + " m from any of " +
					std::to_string(tried) + " soldiers of team " + std::to_string(want_team) + " (" + std::to_string(tally.no_ground) + " candidates without ground, " +
					std::to_string(tally.blocked) + " blocked, " + std::to_string(tally.no_sight) + " without sight): skipped"));
			++sh.failures;
			++sh.at;
			sh.t0 = p_now_s;
			return;
		}
		if (host::ClientSession *c = session_->client()) {
			net::DebugRequest q;
			q.kind = net::DebugKind::Teleport;
			q.position = v.spot;
			q.yaw = sim::quantize_yaw(v.yaw);
			session_->mapper().set_requested_pose(sim::Pose::Stand);
			c->send_debug(q);
		}
		sh.step = 1;
		sh.t0 = p_now_s;
		return;
	}
	const IffSoldier *target = nullptr;
	for (size_t i = 0; i < n; ++i) {
		if (soldiers[i].slot == v.slot && soldiers[i].alive) {
			target = &soldiers[i];
		}
	}
	if (target == nullptr) {
		UtilityFunctions::print(to_godot("[pt1-iff] view " + std::to_string(sh.at) + ": slot " + std::to_string(v.slot) + " no longer presented alive"));
		++sh.failures;
		++sh.at;
		sh.step = 0;
		sh.t0 = p_now_s;
		return;
	}
	const sim::Vec3 chest{ target->feet[0], target->feet[1] + target->eye_height * 0.75f, target->feet[2] };
	const double waited = p_now_s - sh.t0;
	if (sh.step == 1) {
		// Look at the chest from the presented eye (the teleport has landed by now).
		const Vector3 eye = p_camera != nullptr ? p_camera->get_global_transform().origin : vec(f.own_curr.position) + Vector3(0, 1.6f, 0);
		const float dx = chest[0] - eye.x, dy = chest[1] - eye.y, dz = chest[2] - eye.z;
		session_->mapper().script_view(std::atan2(dx, dz), std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
		if (waited >= 1.0) {
			sh.step = 2;
			sh.t0 = p_now_s;
		}
		return;
	}
	if (waited < 0.5) {
		return;
	}
	char name[96];
	std::snprintf(name, sizeof(name), "iff_%s_%02dm", v.enemy ? "enemy" : "teammate", int(v.metres));
	const std::string path = sh.dir + "/" + name + ".png";
	const bool ok = session_->save_screenshot(path);
	sh.saved += ok ? 1 : 0;
	sh.failures += ok ? 0 : 1;
	const NametagDraw *drawn = nullptr;
	for (const NametagDraw &d : tags_->draws()) {
		if (d.slot == v.slot) {
			drawn = &d;
		}
	}
	const Vector3 eye = p_camera != nullptr ? p_camera->get_global_transform().origin : Vector3();
	char line[400];
	std::snprintf(line, sizeof(line), "[pt1-iff] %s: slot %u at %.1f m (eye to anchor): %s%s%s; minimap counts this run so far: %.1f friendly / %.2f enemy per frame -> %s",
			name, unsigned(v.slot), double(eye.distance_to(vec(target->feet) + Vector3(0, target->eye_height + style_.vertical_offset, 0))),
			drawn == nullptr ? "no tag" : (drawn->icon ? "icon" : "no icon"), drawn != nullptr && drawn->name ? ", name " : "",
			drawn != nullptr && drawn->name ? to_std(drawn->text).c_str() : "", double(stats_.minimap_friendly) / double(std::max<uint64_t>(1, stats_.frames)),
			double(stats_.minimap_enemy) / double(std::max<uint64_t>(1, stats_.frames)), ok ? path.c_str() : "SAVE FAILED");
	UtilityFunctions::print(line);
	++sh.at;
	sh.step = 0;
	sh.t0 = p_now_s;
}

// ---- Bf3LevelSession: PT1 iff [context] ------------------------------------------------------------------------------

const sim::CollisionWorld *Bf3LevelSession::pt1_iff_world() const {
	return m2a_ ? m2a_->world.get() : nullptr;
}

Bf3UiAssets *Bf3LevelSession::pt1_iff_ui_assets() const {
	return m2a_ ? m2a_->ui_assets : nullptr;
}

} // namespace bf3::godot_side

// PT1 iff [register] (register_types.cpp).
void openbf3_register_pt1_iff() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3Nametags);
}
