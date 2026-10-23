#include "native_tags.h"

#include "godot_util.h"
#include "level_session.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/install.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/text.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace godot;
namespace ui = bf3::core::ui;

namespace bf3::godot_side {

namespace {

constexpr std::array<const char *, 4> kKitRoles{ "assault", "engineer", "support", "recon" };

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

}

struct NativeTags::Loaded {
	ui::NametagStyle style;
	ui::SpottingRules spotting;
	std::array<std::string, 4> kit_icons;
	std::string notes;
	double seconds = 0.0;
};

NativeTags::NativeTags(Bf3LevelSession *p_session) :
		session_(p_session) {
	loader_ = std::thread([this]() { run_loader(); });
}

NativeTags::~NativeTags() {
	stop_.store(true);
	if (loader_.joinable()) {
		loader_.join();
	}
	if (stats_.frames > 0) {
		log_summary();
	}
}

void NativeTags::run_loader() {
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
			if (const core::EbxPartition *team = (*lib)->partition_by_name("gameplay/teams/us")) {
				if (core::Result<core::gameplay::TeamDef> td = core::gameplay::read_team_data(**lib, team->primary(), &budget)) {
					for (const std::string &kit_name : td->kits) {
						const std::string lower = core::to_lower_ascii(kit_name);
						const size_t slash = lower.find_last_of('/');
						const std::string tail = slash == std::string::npos ? lower : lower.substr(slash + 1);
						size_t k = kKitRoles.size();
						for (size_t r = 0; r < kKitRoles.size(); ++r) {
							if (tail.find(kKitRoles[r]) != std::string::npos) {
								k = r;
								break;
							}
						}
						if (k >= out->kit_icons.size()) {
							continue;
						}
						if (core::Result<core::gameplay::KitDef> kit = core::gameplay::read_kit_def(**lib, kit_name)) {
							if (kit->hud_icon.size() < 64) {
								out->kit_icons[k] = kit->hud_icon;
							}
						} else {
							notes += " kit " + kit_name + ": " + kit.error().message + ";";
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
	out->seconds = core::seconds_since(t0);
	std::lock_guard<std::mutex> lock(mutex_);
	loaded_ = out;
	published_.store(true, std::memory_order_release);
}

void NativeTags::apply_loaded() {
	if (applied_ || !published_.load(std::memory_order_acquire)) {
		return;
	}
	applied_ = true;
	std::shared_ptr<const Loaded> l;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		l = loaded_;
	}
	if (l == nullptr) {
		return;
	}
	style_ = l->style;
	spotting_ = l->spotting;
	kit_icons_ = l->kit_icons;
	char line[512];
	std::snprintf(line, sizeof(line),
			"[tags] nametags %s (FriendlyTint %.2f %.2f %.2f, SquadTint %.2f %.2f %.2f, EnemyTint %.2f %.2f %.2f, IconSize %.0f, ShowLabelRange %.0f m, look-at "
			"%.0f..%.0f within %.0f m, VerticalOffset %.2f m, %u clamped), spotting %s (fire reveal %.2f s), kit icons %s %s %s %s, %.2f s;%s",
			style_.from_data ? "from data" : "retail defaults", double(style_.friendly_tint[0]), double(style_.friendly_tint[1]), double(style_.friendly_tint[2]),
			double(style_.squad_tint[0]), double(style_.squad_tint[1]), double(style_.squad_tint[2]), double(style_.enemy_tint[0]), double(style_.enemy_tint[1]),
			double(style_.enemy_tint[2]), double(style_.icon_size), double(style_.show_label_range), double(style_.max_look_at_threshold),
			double(style_.min_look_at_threshold), double(style_.min_look_at_threshold_distance), double(style_.vertical_offset), style_.clamped,
			spotting_.from_data ? "from data" : "retail defaults", double(spotting_.fire_reveal_seconds()), kit_icons_[0].c_str(), kit_icons_[1].c_str(),
			kit_icons_[2].c_str(), kit_icons_[3].c_str(), l->seconds, l->notes.c_str());
	UtilityFunctions::print(line);
}

void NativeTags::log_summary() const {
	const double n = double(std::max<uint64_t>(1, stats_.frames));
	char line[640];
	std::snprintf(line, sizeof(line),
			"[tags] %llu frames: minimap per frame %.1f friendly + %.1f revealed enemies (%.1f enemies hidden); nametags per frame %.1f friendly icons, %.2f "
			"friendly names, %.3f enemy names; LOS rays %llu (%llu blocked); %llu soldier-frames without a team; style %s, reveal %.2f s; main thread %.1f us "
			"per frame (max %.1f us)",
			(unsigned long long)stats_.frames, double(stats_.minimap_friendly) / n, double(stats_.minimap_enemy) / n, double(stats_.minimap_hidden_enemies) / n,
			double(stats_.friendly_icons) / n, double(stats_.friendly_names) / n, double(stats_.enemy_names) / n, (unsigned long long)stats_.los_rays,
			(unsigned long long)stats_.los_blocked, (unsigned long long)stats_.unknown_team, style_.from_data ? "from data" : "defaults",
			double(spotting_.fire_reveal_seconds()), double(stats_.frame_ns) / n * 1e-3, double(stats_.frame_max_ns) * 1e-3);
	UtilityFunctions::print(line);
}

void NativeTags::update(const Camera3D *p_camera, bool p_show, Vector2 p_viewport_size) {
	tags_.clear();
	minimap_.clear();
	if (session_ == nullptr) {
		return;
	}
	apply_loaded();
	const auto t_frame = std::chrono::steady_clock::now();
	const uint64_t now_ns = net::monotonic_ns();
	const double now_s = double(now_ns) * 1e-9;
	std::array<IffSoldier, net::kMaxSlots> soldiers{};
	const host::PresentationFrame &f = session_->frame();
	const size_t n = iff_soldiers(f, now_ns, soldiers);
	ui::NametagViewer viewer;
	viewer.team = own_team_of(f);
	const std::span<const IffSoldier> all(soldiers.data(), n);
	reveal_.update(all, now_s, spotting_.fire_reveal_seconds());
	++stats_.frames;
	for (const IffSoldier &s : all) {
		stats_.unknown_team += s.team == 0 ? 1u : 0u;
	}

	std::array<IffMinimapEntry, net::kMaxSlots> entries{};
	IffMinimapCounts counts;
	const size_t m = iff_minimap(all, viewer, reveal_, now_s, entries, &counts);
	minimap_.assign(entries.begin(), entries.begin() + std::ptrdiff_t(m));
	stats_.minimap_friendly += counts.friendly + counts.squad;
	stats_.minimap_enemy += counts.enemy;
	stats_.minimap_hidden_enemies += counts.hidden_enemies;

	const bool show = p_show && p_camera != nullptr && p_camera->is_inside_tree() && viewer.team != 0 && p_viewport_size.y > 0.0f;
	if (show) {
		canvas_scale_ = p_viewport_size.y / ui::kNametagStageHeight;
		const Vector2 centre = p_viewport_size * 0.5f;
		const Vector3 eye = p_camera->get_global_transform().origin;
		const sim::CollisionWorld *world = session_->collision_world();
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
				sub.crosshair_stage = (screen - centre).length() / std::max(1e-3f, canvas_scale_);
			}
			ui::NametagDecision dec = ui::decide_nametag(style_, viewer, sub);
			if (dec.needs_line_of_sight && world != nullptr && rays < kMaxLosRaysPerFrame) {
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
			NativeTag t;
			t.slot = s.slot;
			t.relation = dec.relation;
			t.icon = dec.icon;
			t.name = dec.name;
			t.scale = dec.scale;
			t.alpha = dec.alpha;
			t.distance_m = sub.distance_m;
			t.kit = s.kit;
			t.anchor = screen + Vector2(style_.pixel_offset[0], style_.pixel_offset[1]) * canvas_scale_;
			t.text = s.name.empty() ? String("#") + String::num_int64(s.slot) : String::utf8(s.name.data(), int64_t(s.name.size()));
			stats_.friendly_icons += t.icon ? 1u : 0u;
			stats_.friendly_names += t.name && t.relation != ui::TagRelation::Enemy ? 1u : 0u;
			stats_.enemy_names += t.name && t.relation == ui::TagRelation::Enemy ? 1u : 0u;
			tags_.push_back(std::move(t));
		}
	}
	const uint64_t spent_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_frame).count());
	stats_.frame_ns += spent_ns;
	stats_.frame_max_ns = std::max(stats_.frame_max_ns, spent_ns);
	if (summary_at_s_ < 0.0) {
		summary_at_s_ = now_s + kNativeTagsSummaryEveryS;
	} else if (now_s >= summary_at_s_) {
		summary_at_s_ = now_s + kNativeTagsSummaryEveryS;
		log_summary();
	}
}

}
