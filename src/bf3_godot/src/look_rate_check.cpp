#include "look_rate_check.h"

#include "bf3_localisation.h"
#include "game_settings.h"
#include "godot_util.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/math.h"
#include "bf3/core/util/format.h"
#include "bf3/core/util/text.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <array>
#include <cmath>
#include <memory>

namespace bf3::godot_side {

using namespace godot;
using bf3::core::fmt;
namespace gp = bf3::core::gameplay;
namespace ui = bf3::core::ui;

namespace {

constexpr float kCounts = 200.0f;
constexpr float kRateEpsilon = 1e-6f;
constexpr float kPitchEpsilon = 1e-5f;

double degrees(float p_radians) {
	return double(p_radians) / bf3::core::kDegToRad;
}

}

void Bf3LookRateCheck::_bind_methods() {}

void Bf3LookRateCheck::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	set_process(true);
	set_process_unhandled_input(true);
}

void Bf3LookRateCheck::_unhandled_input(const Ref<InputEvent> &p_event) {
	++routed_;
	mapper_.handle_event(p_event);
}

void Bf3LookRateCheck::add(std::string p_name, bool p_pass, std::string p_detail) {
	rows_.push_back({ std::move(p_name), p_pass, std::move(p_detail) });
}

void Bf3LookRateCheck::_process(double p_delta) {
	(void)p_delta;
	if (done_) {
		return;
	}
	Bf3Localisation *loc = Bf3Localisation::current();
	if (loc == nullptr || loc->has_failed() || (loc->is_loaded() && !loc->install())) {
		UtilityFunctions::print(to_godot("LOOK RATE CHECK: no usable install (" +
				(loc != nullptr ? to_std(loc->get_error()) : std::string("no Bf3Localisation")) + ")"));
		done_ = true;
		set_process(false);
		get_tree()->quit(1);
		return;
	}
	if (!loc->is_loaded()) {
		return;
	}
	run();
	finish();
}

ViewAngles Bf3LookRateCheck::push_motion(float p_dx, float p_dy) {
	Ref<InputEventMouseMotion> ev;
	ev.instantiate();
	ev->set_relative(Vector2(p_dx, p_dy));
	ev->set_screen_relative(Vector2(p_dx, p_dy));
	get_viewport()->push_input(ev);
	return mapper_.view();
}

void Bf3LookRateCheck::run() {
	Bf3Localisation *loc = Bf3Localisation::current();
	const std::string path = user_arg_value("settings-file");
	if (path.empty()) {
		add("settings.file_arg", false, "the row passes --settings-file=<a scratch path of its own>");
		return;
	}
	bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> lib =
			bf3::core::EbxLibrary::open(loc->install(), { "Win32/Levels/MP_Subway/MP_Subway" });
	if (!lib) {
		add("library", false, lib.error().message);
		return;
	}
	bf3::core::Result<std::unique_ptr<GameSettings>> settings = GameSettings::load(**lib);
	bf3::core::Result<gp::WeaponDef> m16 = gp::resolve_weapon(**lib, "weapons/m16a4/u_m16a4", {});
	gp::WeaponResolveOptions rifle_scope;
	rifle_scope.unlocks = { "u_sv98_rifle_scope" };
	bf3::core::Result<gp::WeaponDef> sv98 = gp::resolve_weapon(**lib, "weapons/sv98/u_sv98", rifle_scope);
	const bool loaded = settings && m16 && sv98 && m16->aiming.zoom_levels.size() == 2 &&
			sv98->aiming.zoom_levels.size() == 2 &&
			bf3::core::to_lower_ascii(sv98->aiming_controller) == "weapons/common/aimingcontrollers/aim_slow_8x";
	add("load", loaded,
			loaded ? fmt("M16A4 %s, SV98 %s", m16->aiming_controller.c_str(), sv98->aiming_controller.c_str())
				   : std::string("settings, the M16A4 or the SV98 did not load with two zoom levels"));
	if (!loaded) {
		return;
	}
	mapper_.install_actions();
	const bf3::core::Status applied = (*settings)->frame(mapper_, nullptr, path);
	mapper_.capture_for_check();
	add("rate.base", applied.ok() && mapper_.look_scale == 1.0f && mapper_.base_sensitivity == kBaseLookRadiansPerCount,
			fmt("MouseSensitivity 0.5 (the install default) gives %.6f rad per count, the stated %.6f",
					double(mapper_.base_sensitivity), double(kBaseLookRadiansPerCount)));

	struct Case {
		const char *name;
		const gp::WeaponDef *def;
		bool zoomed;
		bool sprinting;
		float expected;
	};
	const float base = kBaseLookRadiansPerCount;
	const std::array<Case, 4> cases = { {
			{ "m16a4.hip", &*m16, false, false, base * m16->aiming.zoom_levels[0].look_speed },
			{ "m16a4.ads_sprint", &*m16, true, true,
					base * m16->aiming.zoom_levels[1].look_speed * m16->aiming.zoom_levels[1].sprint_look_speed },
			{ "sv98.ads_8x", &*sv98, true, false, base * sv98->aiming.zoom_levels[1].look_speed },
			{ "sv98.hip_sprint", &*sv98, false, true, base * sv98->aiming.zoom_levels[0].sprint_look_speed },
	} };
	const std::array<Vector2i, 2> sizes = { Vector2i(1280, 720), Vector2i(2560, 1440) };
	std::array<std::array<float, 4>, 2> measured{};
	for (size_t s = 0; s < sizes.size(); ++s) {
		get_window()->set_size(sizes[s]);
		const Vector2 visible = get_viewport()->get_visible_rect().size;
		add(fmt("size.%dx%d", sizes[s].x, sizes[s].y), int(visible.x) == sizes[s].x && int(visible.y) == sizes[s].y,
				fmt("the viewport is %.0fx%.0f", double(visible.x), double(visible.y)));
		for (size_t c = 0; c < cases.size(); ++c) {
			const Case &k = cases[c];
			const float zoom_look = k.def->aiming.zoom_levels[k.zoomed ? 1 : 0].look_speed;
			mapper_.set_own_look(own_look_of(&k.def->aiming, bf3::sim::Pose::Stand, k.sprinting, k.zoomed, zoom_look));
			mapper_.script_view(0.0f, 0.0f);
			const ViewAngles after = push_motion(kCounts, 0.0f);
			measured[s][c] = -after.yaw / kCounts;
			add(fmt("rate.%s.%d", k.name, sizes[s].x), std::fabs(measured[s][c] - k.expected) <= kRateEpsilon,
					fmt("%.0f counts turn %.6f rad: %.7f rad per count, the data's %.7f", double(kCounts),
							double(-after.yaw), double(measured[s][c]), double(k.expected)));
		}
	}
	bool same = true;
	for (size_t c = 0; c < cases.size(); ++c) {
		same = same && measured[0][c] == measured[1][c];
	}
	add("rate.size_independent", same, "every case turns the same per count at 1280x720 and 2560x1440");

	for (const bf3::sim::Pose pose : { bf3::sim::Pose::Stand, bf3::sim::Pose::Crouch, bf3::sim::Pose::Prone }) {
		const gp::AimingPose &limits = sv98->aiming.poses[size_t(pose)];
		mapper_.set_own_look(own_look_of(&sv98->aiming, pose, false, false, 1.0f));
		mapper_.script_view(0.0f, 0.0f);
		const float up = push_motion(0.0f, -1.0e5f).pitch;
		const float down = push_motion(0.0f, 1.0e5f).pitch;
		const float want_up = limits.max_pitch_deg * bf3::core::kDegToRadF;
		const float want_down = limits.min_pitch_deg * bf3::core::kDegToRadF;
		add(fmt("pitch.pose%d", int(pose)),
				std::fabs(up - want_up) <= kPitchEpsilon && std::fabs(down - want_down) <= kPitchEpsilon,
				fmt("up stops at %.3f deg, down at %.3f deg; the pose's AimingPoseData %.1f..%.1f", degrees(up),
						degrees(down), double(limits.min_pitch_deg), double(limits.max_pitch_deg)));
	}
	mapper_.set_free_camera(true);
	push_motion(0.0f, -1.0e5f);
	const float free_view_up = mapper_.free_view().pitch;
	mapper_.set_free_camera(false);
	add("pitch.free_camera", free_view_up == InputMapper::pitch_limit(),
			fmt("the free camera keeps the protocol's %.3f deg", degrees(free_view_up)));
	add("input.routed", routed_ == 2 * cases.size() + 7,
			fmt("%llu motion events reached the mapper through push_input", static_cast<unsigned long long>(routed_)));

	if (!loc->install_snapshot_at_start().valid) {
		add("install.unchanged", false, "no snapshot at start");
	} else {
		const bf3::core::SnapshotDiff d =
				bf3::core::diff_snapshots(loc->install_snapshot_at_start(), loc->reader().snapshot());
		add("install.unchanged", d.clean(),
				d.clean() ? fmt("%zu files: sizes and mtimes equal", loc->install_snapshot_at_start().files.size())
						  : d.summary());
	}
}

void Bf3LookRateCheck::finish() {
	done_ = true;
	set_process(false);
	size_t passed = 0;
	for (const Row &r : rows_) {
		passed += r.pass ? 1 : 0;
		UtilityFunctions::print(
				to_godot(fmt("  %-4s %-28s %s", r.pass ? "PASS" : "FAIL", r.name.c_str(), r.detail.c_str())));
	}
	const bool ok = !rows_.empty() && passed == rows_.size();
	UtilityFunctions::print(to_godot(fmt("LOOK RATE CHECK: %s (%zu/%zu)", ok ? "PASS" : "FAIL", passed, rows_.size())));
	get_tree()->quit(ok ? 0 : 1);
}

}
