#include "native_hud_moments.h"

#include "level_session.h"
#include "native_hud.h"
#include "native_screens.h"
#include "swf_view.h"
#include "ui_assets.h"

#include "bf3/core/level/budget.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_motor.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

constexpr int32_t kRenderFrames = 3;
constexpr int32_t kMaxFillWaitFrames = 600;
constexpr double kGroupWaitTimeout = 90.0;
constexpr double kArriveTimeout = 10.0;
constexpr std::string_view kNameGlyphs = "WMHNEI";
constexpr float kArriveXz = 0.25f;
constexpr float kArriveY = 0.5f;
constexpr float kScanAboveSpawns = 3.0f;
constexpr float kScanBelowSpawns = 5.0f;
constexpr float kScanStep = 0.05f;
constexpr uint32_t kScanMaxFloors = 64;
constexpr float kFloorNormalY = 0.7f;
constexpr float kNearStep = 0.5f;
constexpr float kNearRadius = 8.0f;
constexpr int64_t kRepeatBytesListed = 16;

std::string repeat_byte_text(int64_t p_index, int64_t p_width, int64_t p_layer_bytes, uint8_t p_first, uint8_t p_now) {
	const bool in_layer = p_index < p_layer_bytes;
	const int64_t pixel = in_layer ? p_index / 4 : p_index - p_layer_bytes;
	const std::string channel = in_layer ? std::string(1, "rgba"[p_index % 4]) : std::string("transmittance");
	return "; " + channel + " at " + std::to_string(pixel % p_width) + "," + std::to_string(pixel / p_width) + ": " +
			std::to_string(int(p_first)) + " then " + std::to_string(int(p_now));
}

struct MomentFloor {
	sim::Vec3 at{};
	bool clear = false;
};

std::optional<MomentFloor> moment_floor(const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor, std::span<const sim::SpawnPoint> p_spawns, float p_x, float p_z) {
	const sim::SpawnPoint *nearest = nullptr;
	float best = 0.0f;
	float low = std::numeric_limits<float>::infinity();
	float high = -std::numeric_limits<float>::infinity();
	for (const sim::SpawnPoint &sp : p_spawns) {
		const float dx = sp.position[0] - p_x;
		const float dz = sp.position[2] - p_z;
		const float d = dx * dx + dz * dz;
		if (nearest == nullptr || d < best) {
			nearest = &sp;
			best = d;
		}
		low = std::min(low, sp.position[1]);
		high = std::max(high, sp.position[1]);
	}
	if (nearest == nullptr) {
		return std::nullopt;
	}
	const float bottom = low - kScanBelowSpawns;
	const auto closer = [&](const sim::Vec3 &p_floor, const std::optional<sim::Vec3> &p_best) {
		return !p_best || std::fabs(p_floor[1] - nearest->position[1]) < std::fabs((*p_best)[1] - nearest->position[1]);
	};
	std::optional<sim::Vec3> clear_floor;
	std::optional<sim::Vec3> any_floor;
	float y = high + kScanAboveSpawns;
	for (uint32_t n = 0; n < kScanMaxFloors && y > bottom; ++n) {
		sim::StaticHit hit;
		if (!p_world.ground_below(sim::Vec3{ p_x, y, p_z }, y - bottom, sim::query_mask::kMovement, &hit)) {
			break;
		}
		y = hit.position[1] - kScanStep;
		if (hit.normal[1] < kFloorNormalY) {
			continue;
		}
		const sim::Vec3 capsule_base{ hit.position[0], hit.position[1] + p_motor.tuning.character_padding, hit.position[2] };
		const bool clear = !p_world.overlap_capsule(capsule_base, p_motor.radius, p_motor.poses[size_t(sim::Pose::Stand)].height, sim::query_mask::kMovement,
				p_motor.tuning.pose_change_max_penetration);
		if (clear && closer(hit.position, clear_floor)) {
			clear_floor = hit.position;
		}
		if (closer(hit.position, any_floor)) {
			any_floor = hit.position;
		}
	}
	if (clear_floor) {
		return MomentFloor{ *clear_floor, true };
	}
	if (any_floor) {
		return MomentFloor{ *any_floor, false };
	}
	return std::nullopt;
}

}

std::string native_hud_xyz(const sim::Vec3 &p_v) {
	return "(" + std::to_string(p_v[0]) + ", " + std::to_string(p_v[1]) + ", " + std::to_string(p_v[2]) + ")";
}

bool native_hud_at_feet(const sim::Vec3 &p_at, const sim::Vec3 &p_feet) {
	const float dx = p_at[0] - p_feet[0];
	const float dz = p_at[2] - p_feet[2];
	return std::sqrt(dx * dx + dz * dz) <= kArriveXz && std::fabs(p_at[1] - p_feet[1]) <= kArriveY;
}

const char *native_hud_text_of(const UiScreenHost &p_hud, const char *p_path) {
	for (size_t i = 0; i < p_hud.screen_count(); ++i) {
		const Bf3SwfView *v = p_hud.screen_slot(i).view;
		const ui::SwfSceneNode *n = v != nullptr && v->scene() != nullptr ? v->scene()->find_node(p_path) : nullptr;
		if (n != nullptr) {
			return n->text.c_str();
		}
	}
	return nullptr;
}

const ui::SwfSceneNode *native_hud_node_of(const UiScreenHost &p_hud, const char *p_path) {
	for (size_t i = 0; i < p_hud.screen_count(); ++i) {
		const Bf3SwfView *v = p_hud.screen_slot(i).view;
		const ui::SwfSceneNode *n = v != nullptr && v->scene() != nullptr ? v->scene()->find_node(p_path) : nullptr;
		if (n != nullptr) {
			return n;
		}
	}
	return nullptr;
}

const ui::SwfScene *native_hud_scene_of(const UiScreenHost &p_hud, const char *p_path) {
	for (size_t i = 0; i < p_hud.screen_count(); ++i) {
		const Bf3SwfView *v = p_hud.screen_slot(i).view;
		if (v != nullptr && v->scene() != nullptr && v->scene()->find_node(p_path) != nullptr) {
			return v->scene();
		}
	}
	return nullptr;
}

float native_hud_layer_scale(const UiScreenHost &p_hud, const char *p_path) {
	for (size_t i = 0; i < p_hud.screen_count(); ++i) {
		const Bf3SwfView *v = p_hud.screen_slot(i).view;
		if (v != nullptr && v->scene() != nullptr && v->scene()->find_node(p_path) != nullptr) {
			return float(v->stage_fit().scale_x);
		}
	}
	return 1.0f;
}

std::vector<NativeHudMessageRow> native_hud_message_rows(const UiScreenHost &p_hud, const char *p_widget, const char *p_text) {
	std::vector<NativeHudMessageRow> out;
	const std::string holder = std::string(p_widget) + ".messageHolder";
	const ui::SwfScene *sc = native_hud_scene_of(p_hud, holder.c_str());
	const ui::SwfSceneNode *h = sc != nullptr ? sc->find_node(holder) : nullptr;
	if (h == nullptr) {
		return out;
	}
	for (const ui::SwfNodeId id : h->children) {
		const ui::SwfSceneNode *row = sc->node(id);
		if (row == nullptr || !row->world_visible) {
			continue;
		}
		const ui::SwfSceneNode *t = sc->find_node(row->path + p_text);
		const ui::SwfSceneNode *body = sc->find_node(row->path + ".container");
		const float alpha = body != nullptr ? body->world_color.mul[3] : row->world_color.mul[3];
		out.push_back(NativeHudMessageRow{ row->world.ty, row->frame, alpha, t != nullptr ? t->text : std::string() });
	}
	std::sort(out.begin(), out.end(), [](const NativeHudMessageRow &p_a, const NativeHudMessageRow &p_b) { return p_a.y < p_b.y; });
	return out;
}

std::string NativeHudNameFit::open(const char *p_movie, std::initializer_list<const char *> p_linkages) {
	Bf3UiAssets *assets = Bf3UiAssets::current();
	if (assets == nullptr || assets->movies() == nullptr || assets->swf_fonts() == nullptr) {
		return "no Bf3UiAssets movies or fonts";
	}
	fonts_ = assets->swf_fonts();
	bf3::core::Result<ui::SwfScene> s = ui::SwfScene::open(*assets->movies(), p_movie);
	if (!s) {
		return std::string(p_movie) + ": " + s.error().message;
	}
	scene_ = std::make_shared<ui::SwfScene>(std::move(*s));
	const std::string root = scene_->node(scene_->root())->path;
	int32_t depth = 1;
	for (const char *linkage : p_linkages) {
		bf3::core::Result<ui::SwfNodeId> a = scene_->attach_movie(root, linkage, linkage, depth++);
		if (!a) {
			return std::string(linkage) + ": " + a.error().message;
		}
	}
	prefix_ = root.empty() ? std::string() : root + ".";
	return {};
}

float NativeHudNameFit::chain_scale(std::string_view p_field) const {
	float s = 1.0f;
	std::string path = prefix_;
	size_t at = 0;
	while (at <= p_field.size()) {
		const size_t dot = std::min(p_field.find('.', at), p_field.size());
		path += std::string(p_field.substr(at, dot - at));
		if (const ui::SwfSceneNode *n = scene_->find_node(path)) {
			s *= n->matrix.a;
		}
		path += ".";
		at = dot + 1;
	}
	return s;
}

float NativeHudNameFit::width(std::string_view p_field, std::string_view p_text) {
	const std::string path = prefix_ + std::string(p_field);
	if (!scene_->set_text(path, p_text)) {
		return 0.0f;
	}
	const ui::SwfSceneNode *n = scene_->find_node(path);
	if (n == nullptr) {
		return 0.0f;
	}
	bf3::core::level::WorkBudget budget(ui::kSwfTextBudgetUnits);
	bf3::core::Result<ui::SwfTextLayout> l = ui::swf_layout_node_text(*fonts_, *n, budget);
	return l ? l->text_width : 0.0f;
}

std::optional<uint32_t> NativeHudNameFit::label(std::string_view p_linkage, std::string_view p_label) const {
	const ui::SwfSceneNode *n = scene_->find_node(prefix_ + std::string(p_linkage));
	return n != nullptr && n->timeline != nullptr ? n->timeline->label_frame(p_label) : std::nullopt;
}

uint32_t NativeHudNameFit::frames(std::string_view p_linkage) const {
	const ui::SwfSceneNode *n = scene_->find_node(prefix_ + std::string(p_linkage));
	return n != nullptr ? n->total_frames() : 0;
}

std::optional<float> NativeHudNameFit::alpha_at(std::string_view p_linkage, std::string_view p_child, uint32_t p_frame) {
	const std::string path = prefix_ + std::string(p_linkage);
	if (!scene_->goto_frame(path, p_frame, false)) {
		return std::nullopt;
	}
	const ui::SwfSceneNode *n = scene_->find_node(path);
	const ui::SwfSceneNode *c = scene_->find_node(path + "." + std::string(p_child));
	if (n == nullptr || c == nullptr || n->frame != p_frame) {
		return std::nullopt;
	}
	return c->color.mul[3];
}

std::string NativeHudNameFit::fit(std::string_view p_field, float p_units) {
	std::vector<std::pair<float, char>> glyphs;
	for (const char c : kNameGlyphs) {
		glyphs.emplace_back(width(p_field, std::string_view(&c, 1)), c);
	}
	std::sort(glyphs.begin(), glyphs.end(), [](const auto &p_a, const auto &p_b) { return p_a.first > p_b.first; });
	std::string out;
	for (const auto &[w, c] : glyphs) {
		while (out.size() + 1 < ui::kHudNameBytes && width(p_field, out + c) <= p_units) {
			out += c;
		}
	}
	return out;
}

NativeHudMinimapPose native_hud_moment_pose(const sim::Vec3 &p_own, float p_view_yaw) {
	NativeHudMinimapPose p;
	p.x = p_own[0];
	p.z = p_own[2];
	p.yaw = p_view_yaw;
	p.valid = true;
	return p;
}

std::optional<sim::Vec3> native_hud_moment_feet(const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor, std::span<const sim::SpawnPoint> p_spawns, float p_x,
		float p_z) {
	const std::optional<MomentFloor> f = moment_floor(p_world, p_motor, p_spawns, p_x, p_z);
	return f ? std::optional<sim::Vec3>(f->at) : std::nullopt;
}

std::optional<sim::Vec3> native_hud_moment_floor_near(const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor, std::span<const sim::SpawnPoint> p_spawns,
		float p_x, float p_z, float p_radius) {
	std::optional<sim::Vec3> fallback;
	for (float r = 0.0f; r <= p_radius + 1e-3f; r += kNearStep) {
		const int32_t around = r > 0.0f ? std::max(8, int32_t(std::ceil(2.0f * float(std::numbers::pi) * r / kNearStep))) : 1;
		for (int32_t i = 0; i < around; ++i) {
			const float a = 2.0f * float(std::numbers::pi) * float(i) / float(around);
			const std::optional<MomentFloor> f = moment_floor(p_world, p_motor, p_spawns, p_x + r * std::cos(a), p_z + r * std::sin(a));
			if (f && f->clear) {
				return f->at;
			}
			if (f && !fallback) {
				fallback = f->at;
			}
		}
	}
	return fallback;
}

NativeHudMomentRun::NativeHudMomentRun(std::string p_out_dir, std::string_view p_ids) : out_dir_(std::move(p_out_dir)) {
	std::vector<std::string_view> ids;
	for (size_t at = 0; at < p_ids.size();) {
		const size_t end = std::min(p_ids.find(',', at), p_ids.size());
		if (end > at) {
			ids.push_back(p_ids.substr(at, end - at));
		}
		at = end + 1;
	}
	register_native_hud_moment_files(groups_);
	const auto take = [this](std::optional<std::string_view> p_id) {
		for (const std::unique_ptr<NativeHudMomentGroup> &g : groups_) {
			for (size_t i = 0; i < g->moment_count(); ++i) {
				if (!p_id || *p_id == g->moment_id(i)) {
					moments_.push_back(Selected{ g.get(), i, std::string(g->moment_id(i)) });
				}
			}
		}
	};
	if (ids.empty()) {
		take(std::nullopt);
	}
	for (const std::string_view id : ids) {
		take(id);
	}
	for (const char *sub : { "captures", "layers" }) {
		DirAccess::make_dir_recursive_absolute(String::utf8((out_dir_ + "/" + sub).c_str()));
	}
}

NativeHudMomentRun::~NativeHudMomentRun() = default;

bool NativeHudMomentRun::holding_for_fills(const Bf3NativeHud &p_hud) {
	if (!p_hud.page_fills_pending()) {
		fill_wait_ = 0;
		return false;
	}
	if (fill_wait_ > kMaxFillWaitFrames) {
		return false;
	}
	if (++fill_wait_ > kMaxFillWaitFrames) {
		row(moments_[index_].id + ".fills", false,
				"page fills still pending after " + std::to_string(kMaxFillWaitFrames) + " frames");
		return false;
	}
	frames_ = 0;
	return true;
}

void NativeHudMomentRun::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	failures_ += p_pass ? 0 : 1;
	UtilityFunctions::print(String::utf8(("NATIVE HUD ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail).c_str()));
}

void NativeHudMomentRun::hold(Bf3LevelSession &r_session) {
	host::ClientSession *client = r_session.client();
	if (client == nullptr) {
		return;
	}
	const float yaw = view_yaw_;
	r_session.mapper().script_view(yaw, 0.0f);
	net::DebugRequest q;
	q.kind = net::DebugKind::Teleport;
	q.id = ++request_id_;
	q.position = feet_;
	q.yaw = sim::quantize_yaw(yaw);
	client->send_debug(q);
}

void NativeHudMomentRun::show_screens(Bf3LevelSession &r_session, bool p_shown) {
	if (Bf3UiScreens *stack = r_session.screen_stack()) {
		stack->set_visible(p_shown);
	}
}

void NativeHudMomentRun::check_stack(const Bf3LevelSession &p_session, const Bf3NativeHud &p_hud) {
	const Bf3UiScreens *stack = p_session.screen_stack();
	const UiScreenRecords *records = p_session.screen_records();
	if (stack == nullptr || records == nullptr) {
		row("stack", false, "the round has no native screen stack");
		return;
	}
	const UiScreenHost &hud = p_hud;
	const UiScreenRecords &hud_records = p_hud;
	std::string differ;
	for (const std::string_view p : kNativeHudPartitions) {
		const UiScreenSlot a = stack->find_screen(p);
		const UiScreenSlot b = hud.find_screen(p);
		const std::optional<UiScreenRecord> ra = records->screen_record(p);
		const std::optional<UiScreenRecord> rb = hud_records.screen_record(p);
		const bool same_slot = a.partition == p && a.screen == b.screen && a.view == b.view && a.shown == b.shown &&
				a.order == b.order && !a.modal && !a.focused && a.screen != nullptr && !a.screen->has_focus();
		const bool same_record = ra && rb && ra->visible == rb->visible && ra->shown == rb->shown;
		if (!same_slot || !same_record) {
			differ += (differ.empty() ? "" : ", ") + std::string(p) + (same_slot ? " record" : " slot");
		}
	}
	const std::optional<UiScreenPhase> mp = stack->phase(ui::kHudMpScreenPartition);
	const bool following = mp == UiScreenPhase::Shown && stack->stats().entered > 0 && stack->errors().empty();
	const std::string hosted = differ.empty() ? "yes" : "no, " + differ;
	row("stack", differ.empty() && following,
			"Bf3UiScreens slots and round records of the 4 in-round screens are Bf3NativeHud's: " + hosted +
					"; hudmpscreen " + (mp ? ui_screen_phase_name(*mp) : "not in the stack") + ", " +
					std::to_string(stack->stats().entered) + " entered, " + std::to_string(stack->errors().size()) +
					" errors");
}

void NativeHudMomentRun::check_host(Bf3NativeHud &r_hud) {
	UiScreenHost &host = r_hud;
	bool slots = host.screen_count() == kNativeHudViews;
	for (size_t i = 0; slots && i < kNativeHudViews; ++i) {
		const UiScreenSlot s = host.screen_slot(i);
		slots = s.partition == kNativeHudPartitions[i] && s.screen != nullptr && s.view != nullptr && s.shown && s.order == int32_t(i) && s.view->get_index() == int32_t(i);
	}
	row("host.slots", slots, std::to_string(host.screen_count()) + " in-round screens open, shown and drawn in partition order");
	const std::string_view crosshair = ui::kCrosshairDefaultPartition;
	const bool hidden = host.hide_screen(crosshair) && !host.find_screen(crosshair).shown && !host.find_screen(crosshair).view->is_visible();
	const bool reshown = host.show_screen(crosshair) && host.find_screen(crosshair).shown && host.find_screen(crosshair).view->is_visible();
	row("host.show_hide", hidden && reshown, std::string("crosshairdefault hidden ") + (hidden ? "yes" : "no") + ", shown again " + (reshown ? "yes" : "no"));
	const bool top = host.order_screen(ui::kHudMpScreenPartition, int32_t(kNativeHudViews)) && host.find_screen(ui::kHudMpScreenPartition).view->get_index() == int32_t(kNativeHudViews) - 1;
	const bool back = host.order_screen(ui::kHudMpScreenPartition, 0) && host.find_screen(ui::kHudMpScreenPartition).view->get_index() == 0;
	row("host.order", top && back, std::string("hudmpscreen drawn last at order ") + std::to_string(kNativeHudViews) + " " + (top ? "yes" : "no") + ", first again at 0 " + (back ? "yes" : "no"));
	bool inert = !host.focus_screen(ui::kKillScreenPartition) && !host.set_screen_modal(ui::kKillScreenPartition, false);
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		const std::string_view p = kNativeHudPartitions[i];
		inert = inert && !host.focus_screen(p) && !host.set_screen_modal(p, true) && host.set_screen_modal(p, false);
		const UiScreenSlot s = host.screen_slot(i);
		inert = inert && !s.modal && !s.focused && s.screen != nullptr && !s.screen->has_focus();
	}
	host.clear_focus();
	inert = inert && host.focused_screen().partition.empty() && host.top_modal_screen().partition.empty();
	row("host.focus_modal", inert, std::string("the in-round screens never take focus and are never modal: ") + (inert ? "focus and modal refused, none focused, none modal" : "a screen took focus or became modal"));
	const bool closed = host.close_screen(crosshair) && host.find_screen(crosshair).screen == nullptr && host.find_screen(crosshair).view == nullptr;
	const bf3::core::Status reopened = host.open_screen(crosshair);
	r_hud.step(kNativeHudMomentStep);
	const UiScreenSlot again = host.find_screen(crosshair);
	const bool live = reopened && again.screen != nullptr && again.view != nullptr && again.shown && again.screen->initialized() && again.view->get_index() == 2;
	row("host.close_open", closed && live,
			std::string("crosshairdefault closed ") + (closed ? "yes" : "no") + ", reopened " + (reopened ? std::string("yes") : reopened.error().message) + ", initialized after a step " +
					(live ? "yes" : "no"));
	const UiScreenRecords &records = r_hud;
	const std::optional<UiScreenRecord> crosshair_record = records.screen_record(crosshair);
	const std::optional<UiScreenRecord> mp_record = records.screen_record(ui::kHudMpScreenPartition);
	const bool recorded = crosshair_record && crosshair_record->visible && crosshair_record->shown == 3 && mp_record &&
			mp_record->visible && mp_record->shown == 1 && !records.reports(ui::kKillScreenPartition);
	row("host.records", recorded,
			"crosshairdefault shown " + (crosshair_record ? std::to_string(crosshair_record->shown) : std::string("unreported")) + " times (built, shown again, reopened), hudmpscreen " +
					(mp_record ? std::to_string(mp_record->shown) : std::string("unreported")) + ", killscreen not reported");
	const bf3::core::Status foreign = host.open_screen(ui::kKillScreenPartition);
	row("host.foreign", !foreign && host.screen_count() == kNativeHudViews, "killscreen refused: " + (foreign ? std::string("opened") : foreign.error().message));
}

bool NativeHudMomentRun::step(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, double p_delta) {
	switch (phase_) {
		case Phase::Wait: {
			waited_ += p_delta;
			if (!r_hud.is_ready()) {
				return false;
			}
			if (r_hud.has_failed() || moments_.empty()) {
				row("built", false, r_hud.has_failed() ? r_hud.error() : std::string("no moment selected"));
				phase_ = Phase::Done;
				return true;
			}
			const NativeHudMomentWait wait = moments_[index_].group->ready(r_session, r_hud);
			if (!wait.met) {
				if (waited_ > kGroupWaitTimeout) {
					row(wait.row, false, "after " + std::to_string(kGroupWaitTimeout) + " s: " + wait.detail);
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			if (!scripted_) {
				scripted_ = true;
				r_hud.set_manual(true);
				InputMapper &mapper = r_session.mapper();
				mapper.set_scripted(true);
				mapper.release_all();
				mapper.set_free_camera(false);
				r_session.cameras().set_free(false);
			}
			waited_ = 0.0;
			phase_ = Phase::Place;
			return false;
		}
		case Phase::Place: {
			const Selected &m = moments_[index_];
			host::ClientSession *client = r_session.client();
			const sim::CollisionWorld *world = r_session.collision_world();
			const std::shared_ptr<const host::SharedLevel> level = r_session.shared_level();
			const std::optional<NativeHudMomentPlace> place = m.group->place(*this, r_hud, m.moment);
			if (!place) {
				phase_ = Phase::Done;
				return true;
			}
			view_yaw_ = place->view_yaw;
			const std::array<float, 2> at_xz{ place->x, place->z };
			const std::optional<sim::Vec3> feet =
					world != nullptr && level != nullptr ? native_hud_moment_floor_near(*world, level->motor, level->spawns, at_xz[0], at_xz[1], kNearRadius) : std::nullopt;
			if (client == nullptr) {
				row(std::string(m.id) + ".place", false, "no client");
				phase_ = Phase::Done;
				return true;
			}
			if (!feet) {
				row(std::string(m.id) + ".place", false, "no floor under (" + std::to_string(at_xz[0]) + ", " + std::to_string(at_xz[1]) + ")");
				feet_ = r_session.frame().own_curr.position;
				phase_ = Phase::Settle;
				return false;
			}
			feet_ = *feet;
			const float off = std::hypot(feet_[0] - at_xz[0], feet_[2] - at_xz[1]);
			if (off > kArriveXz) {
				UtilityFunctions::print(String::utf8((std::string("NATIVE HUD FLAG ") + m.id + " no clear standing floor at the reference xz; the nearest is " + std::to_string(off) + " m away at " + native_hud_xyz(feet_)).c_str()));
			}
			hold(r_session);
			arriving_ = 0.0;
			phase_ = Phase::Arrive;
			return false;
		}
		case Phase::Arrive: {
			const Selected &m = moments_[index_];
			arriving_ += p_delta;
			hold(r_session);
			const sim::Vec3 &at = r_session.frame().own_curr.position;
			if (!native_hud_at_feet(at, feet_)) {
				if (arriving_ > kArriveTimeout) {
					row(std::string(m.id) + ".place", false, "the own soldier is at " + native_hud_xyz(at) + " " + std::to_string(kArriveTimeout) + " s after the teleport to " + native_hud_xyz(feet_));
					phase_ = Phase::Settle;
					return false;
				}
				return false;
			}
			phase_ = Phase::Settle;
			return false;
		}
		case Phase::Settle: {
			const Selected &m = moments_[index_];
			m.group->settle(*this, r_session, r_hud, m.moment);
			r_hud.set_visible(true);
			frames_ = 0;
			fill_wait_ = 0;
			phase_ = Phase::Render;
			return false;
		}
		case Phase::Render:
			hold(r_session);
			r_hud.set_visible(true);
			if (holding_for_fills(r_hud)) {
				return false;
			}
			if (++frames_ < kRenderFrames) {
				return false;
			}
			phase_ = Phase::Capture;
			return false;
		case Phase::Capture: {
			composite_ = r_hud.get_viewport() != nullptr ? r_hud.get_viewport()->get_texture()->get_image() : Ref<Image>();
			show_screens(r_session, false);
			hold(r_session);
			frames_ = 0;
			phase_ = Phase::HudOnly;
			return false;
		}
		case Phase::HudOnly: {
			hold(r_session);
			if (holding_for_fills(r_hud)) {
				return false;
			}
			if (++frames_ < kRenderFrames) {
				return false;
			}
			colour_ = r_hud.capture_views();
			r_hud.set_additive_coverage(true);
			frames_ = 0;
			phase_ = Phase::Coverage;
			return false;
		}
		case Phase::Coverage: {
			const Selected &m = moments_[index_];
			hold(r_session);
			if (holding_for_fills(r_hud)) {
				return false;
			}
			if (++frames_ < kRenderFrames) {
				return false;
			}
			const std::array<Ref<Image>, kNativeHudViews> coverage = r_hud.capture_views();
			r_hud.set_additive_coverage(false);
			show_screens(r_session, true);
			Ref<Image> composite = composite_;
			Ref<Image> layer = Bf3NativeHud::capture_layer(colour_, coverage);
			Ref<Image> trans = swf_view_transmittance(colour_);
			composite_ = Ref<Image>();
			colour_ = {};
			bool ok = composite.is_valid() && layer.is_valid() && trans.is_valid();
			std::string detail;
			if (ok) {
				composite->convert(Image::FORMAT_RGB8);
				const std::string cp = out_dir_ + "/captures/" + m.id + ".png";
				const std::string lp = out_dir_ + "/layers/" + m.id + ".png";
				const std::string tp = out_dir_ + "/layers/" + m.id + "_trans.png";
				ok = composite->save_png(String::utf8(cp.c_str())) == OK && layer->save_png(String::utf8(lp.c_str())) == OK && trans->save_png(String::utf8(tp.c_str())) == OK;
				detail = std::to_string(composite->get_width()) + "x" + std::to_string(composite->get_height()) + " composite, " +
						std::to_string(layer->get_width()) + "x" + std::to_string(layer->get_height()) + " HUD layer, " + std::to_string(trans->get_width()) + "x" +
						std::to_string(trans->get_height()) + " transmittance -> " + cp;
				ok = ok && composite->get_width() == 1280 && composite->get_height() == 720 && layer->get_width() == 1280 && layer->get_height() == 720 &&
						trans->get_width() == 1280 && trans->get_height() == 720;
			} else {
				detail = "no viewport image, HUD layer or transmittance";
			}
			row(std::string(m.id) + ".capture", ok, detail);
			captured_ += ok ? 1 : 0;
			if (ok) {
				const PackedByteArray bytes = layer->get_data() + trans->get_data();
				const auto [first, fresh] = first_capture_.try_emplace(m.id, bytes);
				if (!fresh) {
					const int64_t n = std::min(first->second.size(), bytes.size());
					int64_t differ = std::max(first->second.size(), bytes.size()) - n;
					std::string where;
					for (int64_t i = 0; i < n; ++i) {
						if (first->second[i] != bytes[i]) {
							++differ;
							if (differ <= kRepeatBytesListed) {
								where += repeat_byte_text(
										i, layer->get_width(), layer->get_data().size(), first->second[i], bytes[i]);
							}
						}
					}
					row(std::string(m.id) + ".repeat", differ == 0,
							std::to_string(differ) + " of " + std::to_string(bytes.size()) +
									" HUD layer and transmittance bytes differ from the first capture of " + m.id +
									where);
				}
			}
			m.group->check(*this, r_session, r_hud, m.moment, ok ? layer : Ref<Image>());
			if (++index_ >= moments_.size()) {
				check_stack(r_session, r_hud);
				check_host(r_hud);
				phase_ = Phase::Done;
				return true;
			}
			phase_ = moments_[index_].group == moments_[index_ - 1].group ? Phase::Place : Phase::Wait;
			return false;
		}
		case Phase::Done:
			return true;
	}
	return true;
}


}
